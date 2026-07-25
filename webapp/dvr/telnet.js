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
    const run = () => new Promise((resolve) => {
      const s = this.sock; const r = Math.floor(Math.random() * 1e9);
      const S = 'S' + r + 'S', E = 'E' + r + 'E';
      // quote-split source so the echoed command text never contains the contiguous marker
      const Ssrc = "'S'" + r + "'S'", Esrc = "'E'" + r + "'E'";
      let acc = Buffer.alloc(0);
      const onData = (d) => {
        acc = Buffer.concat([acc, stripIAC(d, s)]);
        const t = acc.toString('latin1');
        const si = t.indexOf(S), ei = t.indexOf(E, si + S.length);
        if (si >= 0 && ei >= 0) {
          s.removeListener('data', onData);
          resolve(t.slice(si + S.length, ei).replace(/\x1b\[[0-9;]*m/g, '').replace(/\r/g, '').replace(/^\n+/, '').replace(/\n+$/, ''));
        }
      };
      s.on('data', onData);
      s.write('echo ' + Ssrc + '; ' + cmd + '; echo ' + Esrc + '\n');
      setTimeout(() => { try { s.removeListener('data', onData); } catch (e) {} resolve(acc.toString('latin1')); }, timeoutMs || 8000);
    });
    this.q = this.q.then(run, run);
    return this.q;
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
