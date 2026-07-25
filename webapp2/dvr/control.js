/* dvr/control.js — persistent client for our DVR's control port (TCP 8090).
 *
 * The old code opened a fresh TCP connection per command. The device only accepts
 * MAXCTL(4) control clients, so a couple of browser tabs polling every 3 s could
 * starve it — and every command paid a connect + 200 ms quiet-timeout. This keeps ONE
 * connection, queues commands, and matches replies in order (the device is strictly
 * sequential per connection), so a poll costs one round trip.
 *
 * Reply shapes (docs/CONTROL_PROTOCOL.md):
 *   single line          - everything except LIST
 *   "LIST <n>" ... "END" - the recordings inventory
 * Node v14 compatible (no optional chaining / nullish coalescing).
 */
'use strict';
const net = require('net');
const EventEmitter = require('events');

const CONNECT_RETRY_MS = 3000;
const CMD_TIMEOUT_MS = 6000;

class DvrControl extends EventEmitter {
  constructor(host, port) {
    super();
    this.host = host;
    this.port = port || 8090;
    this.sock = null;
    this.ready = false;
    this.buf = '';
    this.queue = [];      // {cmd, resolve, multi, lines, timer}
    this.closed = false;
    this._connect();
  }

  _connect() {
    if (this.closed || this.sock) return;
    const s = net.connect(this.port, this.host);
    this.sock = s;
    s.setNoDelay(true);
    s.on('connect', () => {
      this.ready = true;
      this.emit('open');
      console.log('[ctl] connected to ' + this.host + ':' + this.port);
      this._pump();          // anything queued while we were down goes out now
    });
    s.on('data', (d) => this._onData(d));
    s.on('error', () => {});
    s.on('close', () => this._down());
  }

  _down() {
    const wasReady = this.ready;
    this.ready = false;
    if (this.sock) { try { this.sock.destroy(); } catch (e) {} }
    this.sock = null;
    this.buf = '';
    // fail every pending command so callers get an answer instead of hanging
    const q = this.queue; this.queue = [];
    q.forEach((j) => { clearTimeout(j.timer); j.resolve(null); });
    if (wasReady) { this.emit('close'); console.log('[ctl] disconnected, retrying'); }
    if (!this.closed) setTimeout(() => this._connect(), CONNECT_RETRY_MS);
  }

  _onData(d) {
    this.buf += d.toString('latin1');
    let i;
    while ((i = this.buf.indexOf('\n')) >= 0) {
      const line = this.buf.slice(0, i).replace(/\r$/, '');
      this.buf = this.buf.slice(i + 1);
      this._onLine(line.trim());
    }
  }

  _onLine(line) {
    if (!line || line === 'DVR READY') return;   // banner, not a reply
    const job = this.queue[0];
    if (!job) return;                            // unsolicited — ignore
    if (job.multi) {
      if (line === 'END') { this._finish(job.lines); return; }
      job.lines.push(line);
      return;
    }
    // a LIST reply is the one multi-line shape; switch this job into collecting mode
    if (/^LIST \d+$/.test(line)) { job.multi = true; job.lines = [line]; return; }
    this._finish(line);
  }

  _finish(value) {
    const job = this.queue.shift();
    if (!job) return;
    clearTimeout(job.timer);
    job.resolve(value);
    this._pump();
  }

  _pump() {
    const job = this.queue[0];
    if (!job || job.sent || !this.ready) return;
    job.sent = true;
    /* A timeout is FATAL to the connection, for the same reason it is in dvr/telnet.js:
     * replies here are matched POSITIONALLY, so if the device answers late, that answer
     * lands on the next command's slot and every reply after it is off by one — STATUS
     * returning someone else's "OK", record state read wrong, REC/STOP unverifiable.
     * Proven, not theoretical: an INFO once came back as "PLAY err". Resolving null and
     * carrying on would leave the link quietly lying. Drop it; _down() fails the whole
     * queue and reconnects. */
    job.timer = setTimeout(() => {
      console.log('[ctl] timeout on "' + job.cmd + '" — dropping the link to stay in sync');
      try { this.sock.destroy(); } catch (e) {}
    }, CMD_TIMEOUT_MS);
    try { this.sock.write(job.cmd + '\n'); } catch (e) { this._finish(null); }
  }

  /* Fire a command on its OWN short-lived connection, resolving as soon as it is sent.
   * For commands the device only answers when it FINISHES: PLAY blocks until playback
   * ends, which can be minutes. Such a command cannot share the ordered queue at any
   * timeout value — too short and the late reply corrupts the stream, too long and every
   * other command waits behind it. The device accepts MAXCTL(4) control clients, so one
   * transient extra is affordable, and its reply is read and discarded here. */
  sendDetached(cmd) {
    return new Promise((resolve) => {
      let s;
      try { s = net.connect(this.port, this.host); } catch (e) { return resolve(false); }
      let sent = false, seen = '';
      const kill = () => { try { s.destroy(); } catch (e) {} };
      s.setNoDelay(true);
      s.setTimeout(600000, kill);            // never hold a socket open forever
      s.on('connect', () => {
        try { s.write(cmd + '\n'); sent = true; resolve(true); } catch (e) { resolve(false); kill(); }
      });
      s.on('data', (d) => {                  // consume the banner, then the eventual reply
        seen += d.toString('latin1');
        if (seen.replace(/DVR READY\r?\n/, '').indexOf('\n') >= 0) kill();
      });
      s.on('error', () => { if (!sent) resolve(false); kill(); });
      s.on('close', () => { if (!sent) resolve(false); });
    });
  }

  /* send one command; resolves to the reply line, an array of lines for LIST,
   * or null if the device is unreachable / timed out. */
  send(cmd) {
    return new Promise((resolve) => {
      if (this.closed) return resolve(null);
      this.queue.push({ cmd: cmd, resolve: resolve, multi: false, lines: [], sent: false, timer: null });
      if (!this.ready) {
        // not connected: fail fast rather than queueing forever
        const j = this.queue[this.queue.length - 1];
        j.timer = setTimeout(() => {
          // only abandon it if it never went out; yanking a job that IS in flight would
          // leave its reply to land on the next one
          const k = this.queue.indexOf(j);
          if (k >= 0 && !j.sent) { this.queue.splice(k, 1); resolve(null); }
        }, 1200);
        return;
      }
      this._pump();
    });
  }

  close() { this.closed = true; if (this.sock) { try { this.sock.destroy(); } catch (e) {} } }
}

/* "INFO ver=2 up=11 res=704x240 rec=0000 recmb=0,0,0,0 ..." -> object */
function parseInfo(line) {
  if (!line || line.indexOf('INFO ') !== 0) return null;
  const o = {};
  line.slice(5).split(/\s+/).forEach((kv) => {
    const j = kv.indexOf('=');
    if (j > 0) o[kv.slice(0, j)] = kv.slice(j + 1);
  });
  const num = (k, d) => (o[k] !== undefined && /^\d+$/.test(o[k]) ? parseInt(o[k], 10) : d);
  const res = /^(\d+)x(\d+)$/.exec(o.res || '');
  const recBits = o.rec || '';
  const mb = (o.recmb || '').split(',').map((x) => parseInt(x, 10) || 0);
  const secs = (o.recsec || '').split(',').map((x) => parseInt(x, 10) || 0);
  const t = /^(\d{4})(\d{2})(\d{2})_(\d{2})(\d{2})(\d{2})$/.exec(o.time || '');
  return {
    ver: num('ver', 0),
    uptime: num('up', 0),
    encChannels: num('enc', 1),
    std: o.std || '',
    width: res ? parseInt(res[1], 10) : 0,
    height: res ? parseInt(res[2], 10) : 0,
    fps: num('fps', 0),
    gop: num('gop', 0),
    rcMode: num('rc', 0),
    qp: num('qp', 0),
    bitrate: num('br', 0),
    diskFreeMb: num('disk', 0),
    displayChannel: num('ch', 0),
    clients: num('cli', 0),
    playing: o.pb === '1',
    osdOpen: o.osd === '1',
    sound: o.snd === '1',          // buzzer feedback on menu navigation
    packs: num('packs', 0),
    rec: recBits.split('').map((c) => c === '1' || c === 'w'),
    recArmed: recBits.split('').map((c) => c === 'w'),
    recMb: mb,
    recSecs: secs,
    time: t ? (t[1] + '-' + t[2] + '-' + t[3] + ' ' + t[4] + ':' + t[5] + ':' + t[6]) : '',
  };
}

/* ["LIST 3", "R /root/rec/a1/20260725_014531_ch1.ts 65966004 20260725014531", ...] */
function parseList(lines) {
  if (!Array.isArray(lines)) return [];
  const out = [];
  lines.forEach((l) => {
    const m = /^R (\S+) (\d+) (\d{14})$/.exec(l);
    if (!m) return;
    const path = m[1];
    const file = path.split('/').pop();
    const f = /^(\d{4})(\d{2})(\d{2})_(\d{2})(\d{2})(\d{2})_ch(\d)\.ts$/.exec(file);
    if (!f) return;
    out.push({
      path: path,
      file: file,
      dir: path.slice(0, path.lastIndexOf('/')),
      size: parseInt(m[2], 10),
      key: m[3],
      date: f[1] + '-' + f[2] + '-' + f[3],
      time: f[4] + ':' + f[5] + ':' + f[6],
      ch: parseInt(f[7], 10) - 1,
    });
  });
  return out;
}

module.exports = { DvrControl, parseInfo, parseList };
