'use strict';
// Telnet client for the DVR (busybox telnetd on :23, login root / blank password).
// Provides one-shot exec() (queued on a persistent session) and raw interactive bridging.
const net = require('net');
const EventEmitter = require('events');

function stripIAC(buf, sock) {
  const out = []; let i = 0;
  while (i < buf.length) {
    const b = buf[i];
    if (b === 0xFF && i + 2 < buf.length) {
      const cmd = buf[i + 1], opt = buf[i + 2];
      if (cmd === 0xFD) sock.write(Buffer.from([0xFF, 0xFC, opt]));      // DO   -> WONT
      else if (cmd === 0xFB) sock.write(Buffer.from([0xFF, 0xFE, opt])); // WILL -> DONT
      i += 3;
    } else { out.push(b); i += 1; }
  }
  return Buffer.from(out);
}

// A logged-in session that serializes exec() calls.
class TelnetExec {
  constructor(host, user, pass) { this.host = host; this.user = user || 'root'; this.pass = pass || ''; this.sock = null; this.ready = false; this.q = Promise.resolve(); }

  connect() {
    return new Promise((resolve, reject) => {
      const s = net.connect({ host: this.host, port: 23 }, () => {});
      s.setKeepAlive(true, 10000);   // detect a dead peer (device reboot) on an idle session
      this.sock = s; let acc = Buffer.alloc(0); let stage = 'login';
      const onData = (d) => {
        acc = Buffer.concat([acc, stripIAC(d, s)]);
        const txt = acc.toString('latin1');
        if (stage === 'login' && /ogin:/.test(txt)) { stage = 'pass'; acc = Buffer.alloc(0); s.write(this.user + '\n'); }
        else if (stage === 'pass' && /assword:/.test(txt)) { stage = 'wait'; acc = Buffer.alloc(0); s.write(this.pass + '\n'); }
        else if ((stage === 'pass' || stage === 'wait' || stage === 'login') && /[#$]\s*$/.test(txt)) {
          stage = 'ready'; acc = Buffer.alloc(0);
          s.removeListener('data', onData);
          // wide terminal (no 80-col wrap) + quiet prompt
          s.write('stty -echo cols 1000 rows 60 2>/dev/null; PS1=""; export PS1\n');
          setTimeout(() => { this.ready = true; resolve(this); }, 400);
        }
      };
      s.on('data', onData);
      s.on('error', reject);
      s.setTimeout(0);
      setTimeout(() => { if (!this.ready) { try { s.removeListener('data', onData); } catch (e) {} resolve(this); } }, 7000);
    });
  }

  exec(cmd, timeoutMs) {
    const run = () => new Promise((resolve, reject) => {
      const s = this.sock; const r = Math.floor(Math.random() * 1e9);
      if (!s || this.poisoned) return reject(new Error('telnet session unusable'));
      const S = 'S' + r + 'S', E = 'E' + r + 'E';
      // quote-split source so the echoed command text never contains the contiguous marker
      const Ssrc = "'S'" + r + "'S'", Esrc = "'E'" + r + "'E'";
      let acc = Buffer.alloc(0);
      let timer = null;
      const onData = (d) => {
        acc = Buffer.concat([acc, stripIAC(d, s)]);
        const t = acc.toString('latin1');
        const si = t.indexOf(S), ei = t.indexOf(E, si + S.length);
        if (si >= 0 && ei >= 0) {
          clearTimeout(timer);
          s.removeListener('data', onData);
          resolve(t.slice(si + S.length, ei).replace(/\x1b\[[0-9;]*m/g, '').replace(/\r/g, '').replace(/^\n+/, '').replace(/\n+$/, ''));
        }
      };
      s.on('data', onData);
      s.write('echo ' + Ssrc + '; ' + cmd + '; echo ' + Esrc + '\n');
      // A timeout used to resolve with whatever had arrived so far. That is the one way
      // this session can break PERMANENTLY: the slow command's output shows up later and
      // is read as the *next* command's reply, so every subsequent exec() returns the
      // previous one's text — silently, forever. Treat it as fatal and reconnect instead.
      timer = setTimeout(() => {
        try { s.removeListener('data', onData); } catch (e) {}
        this.poisoned = true;
        try { s.destroy(); } catch (e) {}       // the owner's 'close' handler reconnects
        reject(new Error('telnet exec timed out: ' + String(cmd).slice(0, 60)));
      }, timeoutMs || 8000);
    });
    const p = this.q.then(run, run);
    this.q = p.catch(() => {});   // keep the queue alive; the caller owns the rejection
    return p;
  }
  /* Write a file to the device, byte-count verified.
   *
   * Two things make this trickier than it looks:
   *  - exec() frames ONE SINGLE-LINE command between echo markers, so a heredoc (or
   *    anything multi-line) leaves the shell at a `>` continuation prompt and every
   *    later exec() on that session returns garbage.
   *  - the transfer therefore goes out of band: the DEVICE listens with `nc -l` and the
   *    PC connects out (the PC firewall blocks inbound). But a backgrounded `nc -l`
   *    inherits its shell's stdin, and once a peer connects it forwards whatever arrives
   *    there — so on the shared session it would swallow our next command. Redirecting
   *    its stdin from /dev/null doesn't help either: busybox 1.1.2 nc treats immediate
   *    stdin EOF as "done" and drops the connection before the data lands.
   *
   * So the transfer gets its OWN short-lived telnet session, which is torn down straight
   * after. Nothing it can eat matters, and the long-lived session stays clean. */
  putFile(remotePath, content) {
    const buf = Buffer.isBuffer(content) ? content : Buffer.from(String(content), 'binary');
    const port = 9500 + Math.floor(Math.random() * 400);
    const tmp = '/tmp/put.' + port;
    const host = this.host;
    const side = new TelnetExec(host, this.user, this.pass);
    const done = (v) => { try { side.close(); } catch (e) {} return v; };
    return side.connect()
      .then(() => side.exec(`rm -f ${tmp}; (nc -l -p ${port} > ${tmp} &); sleep 1; echo listening`))
      .then(() => new Promise((resolve, reject) => {
        const s = net.connect(port, host, () => s.end(buf));
        s.setTimeout(20000, () => { s.destroy(); reject(new Error('upload timed out')); });
        s.on('close', resolve);
        s.on('error', reject);
      }))
      .then(() => new Promise((r) => setTimeout(r, 900)))   // let nc flush and exit
      // verify + install from the MAIN session: the side session may be poisoned by nc
      .then(() => this.exec(`wc -c < ${tmp}`))
      .then((sz) => {
        const n = parseInt(String(sz).trim(), 10);
        if (n !== buf.length) throw new Error(`upload size mismatch (${n} != ${buf.length})`);
        return this.exec(`mv -f ${tmp} "${remotePath}" && echo moved-ok`);
      })
      .then((r) => {
        if (String(r).indexOf('moved-ok') < 0) throw new Error('could not move the file into place');
        return done(true);
      }, (e) => { done(null); throw e; });
  }
  close() { if (this.sock) { this.sock.destroy(); this.sock = null; } }
}

// A raw interactive session for the web terminal (bridge bytes to a WebSocket).
class TelnetRaw extends EventEmitter {
  constructor(host, user, pass) { super(); this.host = host; this.user = user || 'root'; this.pass = pass || ''; }
  start() {
    const s = net.connect({ host: this.host, port: 23 }, () => {});
    this.sock = s; let stage = 'login';
    s.on('data', (d) => {
      const clean = stripIAC(d, s);
      const txt = clean.toString('latin1');
      if (stage === 'login' && /ogin:/.test(txt)) { stage = 'pass'; s.write(this.user + '\n'); }
      else if (stage === 'pass' && /assword:/.test(txt)) { stage = 'ready'; s.write(this.pass + '\n'); }
      this.emit('data', clean);
    });
    s.on('close', () => this.emit('close'));
    s.on('error', (e) => this.emit('error', e));
    return this;
  }
  write(data) { if (this.sock) this.sock.write(data); }
  close() { if (this.sock) this.sock.destroy(); }
}

module.exports = { TelnetExec, TelnetRaw };
