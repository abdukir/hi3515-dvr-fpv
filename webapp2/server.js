/* webapp2/server.js — web console for OUR own DVR (device/dvr/dvr.c, firmware v2).
 *
 * Media path: the browser's per-channel /stream WebSocket is bridged to a raw TCP
 * connection to the device's H.264 stream server (port 8091, docs/PROTOCOL2.md).
 * Control path: ONE persistent connection to the device's control port 8090
 * (dvr/control.js, docs/CONTROL_PROTOCOL.md) — record, display, OSD, on-screen
 * playback, picture, and the INFO/LIST/DEL inventory the firmware now exposes.
 * Telnet is only used for what the control protocol genuinely can't do: the file
 * manager, the web shell, and editing dvr.conf.
 *
 * Env: DVR_HOST (192.168.1.108), DVR_STREAM_PORT (8091), DVR_CTL_PORT (8090),
 *      PORT (8092), TN_USER (root), TN_PASS (''), FILE_PORT (8081), CHANNELS (4).
 * Run: cd webapp2 && npm install && node server.js  ->  http://localhost:8092
 */
'use strict';
const http = require('http');
const net = require('net');
const fs = require('fs');
const os = require('os');
const path = require('path');
const url = require('url');
const { spawn } = require('child_process');
const WebSocket = require('ws');
const { TelnetExec, TelnetRaw } = require('./dvr/telnet');
const { DvrControl, parseInfo, parseList } = require('./dvr/control');

const CFG = {
  dvrHost: process.env.DVR_HOST || '192.168.1.108',
  streamPort: parseInt(process.env.DVR_STREAM_PORT || '8091', 10),
  ctlPort: parseInt(process.env.DVR_CTL_PORT || '8090', 10),
  port: parseInt(process.env.PORT || '8092', 10),
  tnUser: process.env.TN_USER || 'root',
  tnPass: process.env.TN_PASS || '',
  filePort: parseInt(process.env.FILE_PORT || '8081', 10),
  channels: parseInt(process.env.CHANNELS || '4', 10),
};
const FFMPEG = process.env.FFMPEG || path.join(__dirname, '..', 'webapp', 'bin', 'ffmpeg.exe');
const CACHE = path.join(os.tmpdir(), 'dvr2clips');
const CONF_PATH = '/root/rec/dvr.conf';          // read by dvr.c at startup (RAM disk)
const CONF_SATA = '/root/rec/a1/dvr.conf';       // the copy boot.sh restores after a reboot
try { fs.mkdirSync(CACHE, { recursive: true }); } catch (e) {}

// ---------------------------------------------------------------- control plane
const ctl = new DvrControl(CFG.dvrHost, CFG.ctlPort);

/* INFO is the UI's heartbeat: several panels and every open tab want it, so poll it
 * once here and serve everyone from the cache instead of multiplying device traffic. */
let info = null, infoAt = 0, infoPending = null;
function getInfo(maxAgeMs) {
  const age = Date.now() - infoAt;
  if (info && age < (maxAgeMs === undefined ? 900 : maxAgeMs)) return Promise.resolve(info);
  if (infoPending) return infoPending;
  infoPending = ctl.send('INFO').then((line) => {
    const p = parseInfo(line);
    if (p) { info = p; infoAt = Date.now(); }
    else if (line === null) { info = null; }
    infoPending = null;
    return info;
  });
  return infoPending;
}
const encChannels = () => (info && info.encChannels ? info.encChannels : CFG.channels);

function serveFileRange(req, res, file, type) {
  const size = fs.statSync(file).size;
  const range = req.headers.range && req.headers.range.match(/bytes=(\d+)-(\d*)/);
  if (range) {
    const start = parseInt(range[1], 10), end = range[2] ? parseInt(range[2], 10) : size - 1;
    res.writeHead(206, { 'Content-Type': type, 'Content-Range': `bytes ${start}-${end}/${size}`,
      'Accept-Ranges': 'bytes', 'Content-Length': end - start + 1 });
    fs.createReadStream(file, { start, end }).pipe(res);
  } else {
    res.writeHead(200, { 'Content-Type': type, 'Content-Length': size, 'Accept-Ranges': 'bytes' });
    fs.createReadStream(file).pipe(res);
  }
}

/* Run ffmpeg once per output, serialized: concurrent requests (a <video> makes several
 * overlapping range requests) await the SAME job, and we write to a .part then rename so
 * a partial file is never served. Returns a Promise<outPath>. */
const jobs = new Map();
function runFfmpeg(out, args) {
  if (fs.existsSync(out) && fs.statSync(out).size > 512) return Promise.resolve(out);
  if (jobs.has(out)) return jobs.get(out);
  const tmp = out + '.part';
  const pr = new Promise((resolve, reject) => {
    const p = spawn(FFMPEG, ['-y'].concat(args.map((a) => (a === '@OUT@' ? tmp : a))));
    let err = '';
    const kill = setTimeout(() => { try { p.kill('SIGKILL'); } catch (e) {} }, 60000);
    p.stderr.on('data', (d) => { err += d; });
    p.on('error', (e) => { clearTimeout(kill); reject(e); });
    p.on('close', () => {
      clearTimeout(kill);
      if (fs.existsSync(tmp) && fs.statSync(tmp).size > 512) { try { fs.renameSync(tmp, out); } catch (e) {} resolve(out); }
      else { try { fs.unlinkSync(tmp); } catch (e) {} reject(new Error(err.slice(-200))); }
    });
  }).then((v) => { jobs.delete(out); return v; }, (e) => { jobs.delete(out); throw e; });
  jobs.set(out, pr);
  return pr;
}
function serveStatic(res, file, type) {
  const b = fs.readFileSync(file);
  res.writeHead(200, { 'Content-Type': type, 'Content-Length': b.length, 'Cache-Control': 'public, max-age=86400' });
  res.end(b);
}

const PUB = path.join(__dirname, 'public');
const MIME = { '.html': 'text/html', '.js': 'application/javascript', '.css': 'text/css', '.svg': 'image/svg+xml' };

// ------------------------------------------------- telnet (files / shell / config)
// The device reboots (RAM rootfs) and busybox telnetd can drop idle sessions; without
// drop-detection the session goes stale (socket dead but tnReady still true) and every
// exec() silently times out to "" -> empty file lists. So we watch the socket and
// reconnect, and only mark ready when login actually succeeded.
let tn = null, tnReady = false, tnConnecting = false;
function telnetDropped(why) {
  if (!tn && !tnReady) return;
  console.log('[telnet] session dropped (' + why + '), reconnecting');
  tnReady = false; try { if (tn) tn.close(); } catch (e) {} tn = null;
  setTimeout(connectTelnet, 1500);
}
function connectTelnet() {
  if (tnConnecting || tnReady) return;
  tnConnecting = true;
  const t = new TelnetExec(CFG.dvrHost, CFG.tnUser, CFG.tnPass);
  t.connect().then(async () => {
    tnConnecting = false;
    if (!t.ready) throw new Error('login timed out');
    tn = t; tnReady = true; console.log('[telnet] shell ready');
    if (t.sock) { t.sock.once('close', () => telnetDropped('close')); t.sock.once('error', () => telnetDropped('error')); }
    try {
      const pid = await t.exec(`pidof httpd >/dev/null || httpd -h / -p ${CFG.filePort}; pidof httpd`);
      console.log('[telnet] file httpd pid:', pid.trim());
    } catch (e) { /* non-fatal */ }
  }).catch((e) => { tnConnecting = false; tnReady = false; try { t.close(); } catch (_) {} console.log('[telnet] failed:', e.message); setTimeout(connectTelnet, 4000); });
}
connectTelnet();

function parseLs(out) {
  const entries = [];
  for (const line of out.split('\n')) {
    const m = line.match(/^([dl\-][rwx\-]{9})\s+\d+\s+\S+\s+\S+\s+(\d+)\s+(\w+\s+\d+\s+[\d:]+)\s+(.+?)(\s*->\s*.+)?$/);
    if (!m) continue;
    const name = m[4].replace(/\x1b\[[0-9;]*m/g, '').trim();
    if (name === '.') continue;
    entries.push({ name, size: parseInt(m[2], 10), dir: m[1][0] === 'd', link: m[1][0] === 'l', mtime: m[3] });
  }
  return entries;
}
const sendJSON = (res, code, obj) => { res.writeHead(code, { 'Content-Type': 'application/json' }); res.end(JSON.stringify(obj)); };
const readBody = (req) => new Promise((r) => { let b = ''; req.on('data', (d) => b += d); req.on('end', () => r(b)); });
const safePath = (p) => { p = '/' + String(p || '/').replace(/\\/g, '/'); return path.posix.normalize(p).replace(/\.\.(\/|$)/g, ''); };
/* a device recording path we are willing to touch: /root/rec/aN/YYYYMMDD_HHMMSS_chN.ts */
const RECPATH = /^\/root\/rec\/a[1-4]\/\d{8}_\d{6}_ch\d\.ts$/;
const srcUrl = (p) => `http://${CFG.dvrHost}:${CFG.filePort}${p}`;

const server = http.createServer(async (req, res) => {
  const u = url.parse(req.url, true); const p = u.pathname;
  const q = u.query;
  const POST = req.method === 'POST';

  // ---------------------------------------------------------------- status
  if (p === '/api/config') {
    const i = await getInfo(3000);
    return sendJSON(res, 200, {
      channels: CFG.channels,
      encChannels: i ? i.encChannels : CFG.channels,
      dvr: CFG.dvrHost, fwVersion: i ? i.ver : 0, online: !!i,
    });
  }
  if (p === '/api/info') {
    const i = await getInfo(parseInt(q.maxAge || '900', 10));
    if (!i) return sendJSON(res, 503, { error: 'DVR control not connected' });
    return sendJSON(res, 200, i);
  }
  // kept for the old frontend contract
  if (p === '/api/recordstate') {
    const i = await getInfo();
    if (!i) return sendJSON(res, 503, { error: 'DVR control not connected' });
    const state = {};
    for (let k = 0; k < CFG.channels; k++) state[k] = !!i.rec[k];
    return sendJSON(res, 200, { state, std: i.std });
  }

  // ---------------------------------------------------------------- recording
  if (p === '/api/record' && POST) {
    const arg = (q.ch === 'all') ? 'ALL' : String(parseInt(q.ch, 10));
    if (arg !== 'ALL' && !(parseInt(arg, 10) >= 0 && parseInt(arg, 10) < CFG.channels))
      return sendJSON(res, 400, { error: 'bad channel' });
    const on = q.on === '1' || q.on === 'true';
    await ctl.send(`${on ? 'REC' : 'STOP'} ${arg}`);
    infoAt = 0;                                     // force a fresh read
    const i = await getInfo(0);
    if (!i) return sendJSON(res, 503, { error: 'DVR control not connected' });
    const state = {};
    for (let k = 0; k < CFG.channels; k++) state[k] = !!i.rec[k];
    return sendJSON(res, 200, { ok: true, state, info: i });
  }

  // ---------------------------------------------------------------- device settings
  if (p === '/api/time' && POST) {
    const d = new Date();
    const r = await ctl.send(`TIME ${d.getFullYear()} ${d.getMonth() + 1} ${d.getDate()} ${d.getHours()} ${d.getMinutes()} ${d.getSeconds()}`);
    infoAt = 0;
    return sendJSON(res, 200, { ok: r === 'OK', reply: r });
  }
  if (p === '/api/std' && POST) {
    const v = String(q.v || '').toUpperCase();
    if (['PAL', 'NTSC', 'AUTO'].indexOf(v) < 0) return sendJSON(res, 400, { error: 'bad standard' });
    const r = await ctl.send(`STD ${v}`);
    infoAt = 0;
    // reply is passed through so the UI can tell "refused, you're recording" apart from
    // a generic failure — the firmware answers ERR recording rather than ending a capture
    return sendJSON(res, 200, { ok: r === 'OK', reply: r, note: 'the pipeline restarts to apply this' });
  }
  // buzzer feedback on menu navigation (the DVR's own OSD; also on the panel buzzer)
  if (p === '/api/sound' && POST) {
    const on = q.on === '1' || q.on === 'true';
    const r = await ctl.send('SND ' + (on ? '1' : '0'));
    infoAt = 0;
    return sendJSON(res, 200, { ok: /^SND /.test(r || ''), sound: /on/.test(r || '') });
  }

  // which VI channel the DVR's own VGA output shows (the device restarts to re-latch VI->VO)
  if (p === '/api/display' && POST) {
    const ch = parseInt(q.ch, 10);
    if (!(ch >= 0 && ch < CFG.channels)) return sendJSON(res, 400, { error: 'bad channel' });
    const r = await ctl.send(`SHOW ${ch}`);
    infoAt = 0;
    return sendJSON(res, 200, { ok: /^SHOW /.test(r || ''), reply: r });
  }
  // TW2866 picture registers for the displayed channel
  if (p === '/api/picture') {
    const k = String(q.k || '').toUpperCase();
    let cmd = 'PIC';
    if (POST && k) {
      if (['BRIGHT', 'CONTRAST', 'HUE', 'SAT', 'RESET'].indexOf(k) < 0) return sendJSON(res, 400, { error: 'bad key' });
      cmd = 'PIC ' + k + (k === 'RESET' ? '' : ' ' + Math.max(0, Math.min(255, parseInt(q.v, 10) || 0)));
    }
    const r = await ctl.send(cmd);
    const m = /bright=(\d+) contrast=(\d+) hue=(\d+) sat=(\d+)/.exec(r || '');
    if (!m) return sendJSON(res, 503, { error: 'no reply' });
    return sendJSON(res, 200, { bright: +m[1], contrast: +m[2], hue: +m[3], sat: +m[4] });
  }

  // ---------------------------------------------------------------- on-screen UI (OSD)
  if (p === '/api/osd' && POST) {
    const op = String(q.op || 'key').toLowerCase();
    let cmd = null;
    if (op === 'key') {
      const k = String(q.k || '').toUpperCase();
      if (['UP', 'DOWN', 'ENTER', 'EXIT', 'MENU', 'BACK', '-', '+', 'M', 'X'].indexOf(k) < 0)
        return sendJSON(res, 400, { error: 'bad key' });
      cmd = 'UI ' + k;
    } else if (op === 'close') cmd = 'UICLOSE';
    else if (op === 'paint') cmd = 'UIPAINT';
    else if (op === 'show') cmd = 'UISHOW ' + (q.v === '0' ? '0' : '1');
    else if (op === 'mouse') cmd = `MOUSE ${parseInt(q.x, 10) || 0} ${parseInt(q.y, 10) || 0} ${parseInt(q.btn, 10) || 0}`;
    else return sendJSON(res, 400, { error: 'bad op' });
    const r = await ctl.send(cmd);
    infoAt = 0;
    return sendJSON(res, 200, { ok: r !== null, reply: r });
  }

  // ---------------------------------------------------------------- on-screen playback
  // Plays a recording full-screen on the DVR's OWN VGA monitor (VDEC -> VO), which is
  // a different thing from the browser player below. Transport controls map 1:1 to the
  // on-screen bar, so the web UI is a remote control for the device's front end.
  if (p === '/api/playback' && POST) {
    const op = String(q.op || '').toLowerCase();
    const map = { pause: 'PBPAUSE', play: 'PBPLAY', toggle: 'PBTOGGLE', replay: 'PBREPLAY',
                  step: 'PBSTEP', back: 'PBBACK', stop: 'PBSTOP' };
    let cmd;
    if (op === 'open') {
      const f = String(q.path || '');
      if (!RECPATH.test(f)) return sendJSON(res, 400, { error: 'bad path' });
      // PLAY does not answer until playback ENDS, which can be minutes. On the shared
      // ordered connection that late reply lands on whatever command is in flight when
      // it arrives and shifts every reply after it (observed: an INFO returning
      // "PLAY err"). Give it its own socket so it cannot corrupt the queue.
      const started = await ctl.sendDetached('PLAY ' + f);
      return sendJSON(res, started ? 200 : 503,
                      started ? { ok: true, started: f } : { error: 'DVR control not connected' });
    }
    if (op === 'seek') cmd = 'PBSEEK ' + Math.max(0, Math.min(1000, parseInt(q.pm, 10) || 0));
    else if (op === 'speed') cmd = 'PBSPEED ' + ([1, 2, 4].indexOf(parseInt(q.v, 10)) >= 0 ? q.v : '4');
    else if (map[op]) cmd = map[op];
    else return sendJSON(res, 400, { error: 'bad op' });
    const r = await ctl.send(cmd);
    return sendJSON(res, 200, { ok: r === 'OK', reply: r });
  }

  // ---------------------------------------------------------------- recordings
  if (p === '/api/recordings') {
    const lines = await ctl.send('LIST');
    if (lines === null) return sendJSON(res, 503, { error: 'DVR control not connected', recordings: [] });
    let recs = parseList(lines);
    if (q.ch !== undefined && q.ch !== '' && q.ch !== 'all') {
      const ch = parseInt(q.ch, 10);
      recs = recs.filter((r) => r.ch === ch);
    }
    return sendJSON(res, 200, { recordings: recs });
  }
  if (p === '/api/rec/delete' && POST) {
    const f = String(q.path || '');
    if (!RECPATH.test(f)) return sendJSON(res, 400, { error: 'bad path' });
    const r = await ctl.send('DEL ' + f);
    if (r === 'OK') {
      // drop the cached transcode/thumb so a re-recorded name can't serve stale video
      const base = f.split('/').pop().replace(/\.ts$/, '');
      ['.mp4', '.jpg'].forEach((e) => { try { fs.unlinkSync(path.join(CACHE, base + e)); } catch (x) {} });
      return sendJSON(res, 200, { ok: true });
    }
    return sendJSON(res, 409, { error: r || 'no reply' });
  }
  // play/download a recording in the BROWSER (lossless TS->MP4 remux, cached, range-served)
  if (p === '/api/rec/mp4' || p === '/api/rec/thumb') {
    let dev = String(q.path || '');
    if (!dev && q.file) dev = `/root/rec/a${(parseInt(q.ch, 10) || 0) + 1}/${q.file}`;
    if (!RECPATH.test(dev)) { res.writeHead(400); return res.end('bad path'); }
    const base = dev.split('/').pop().replace(/\.ts$/, '');
    try {
      if (p === '/api/rec/thumb') {
        const jpg = path.join(CACHE, base + '.jpg');
        await runFfmpeg(jpg, ['-i', srcUrl(dev), '-frames:v', '1', '-vf', 'scale=320:240', '-q:v', '5', '-f', 'mjpeg', '@OUT@']);
        return serveStatic(res, jpg, 'image/jpeg');
      }
      // -aspect 4:3: recordings are 704x240 (single field, full width); tag the display
      // aspect so players show them un-squished (the device VO scales the field to 480).
      const mp4 = path.join(CACHE, base + '.mp4');
      await runFfmpeg(mp4, ['-i', srcUrl(dev), '-c', 'copy', '-aspect', '4:3', '-movflags', '+faststart', '-f', 'mp4', '@OUT@']);
      if (q.dl) res.setHeader('Content-Disposition', `attachment; filename="${base}.mp4"`);
      return serveFileRange(req, res, mp4, 'video/mp4');
    } catch (e) { res.writeHead(502); return res.end('transcode failed'); }
  }

  // ---------------------------------------------------------------- dvr.conf editor
  if (p === '/api/conf') {
    if (!tnReady) return sendJSON(res, 503, { error: 'device shell not connected' });
    if (POST) {
      // The telnet/nc path is byte-oriented latin1; a UTF-8 em-dash pasted into a comment
      // would land on the device as mojibake. The parser only cares about ASCII key=value,
      // so fold anything else to '?' and keep the file readable on the device.
      const body = String(await readBody(req) || '')
        .replace(/\r/g, '').replace(/[^\x09\x0a\x20-\x7e]/g, '?');
      // dvr.c reads at most 4096 bytes of dvr.conf; refuse anything that would be clipped
      if (body.length > 4000) return sendJSON(res, 400, { error: 'too large (max 4000 bytes)' });
      try {
        // write both copies: the RAM one dvr.c reads now, and the SATA one boot.sh
        // restores after a reboot (the RAM disk is volatile — see docs/FLASH.md)
        await tn.putFile(CONF_PATH, body);
        await tn.exec(`cp -f ${CONF_PATH} ${CONF_SATA} 2>/dev/null; echo done`);
        return sendJSON(res, 200, { ok: true, note: 'restart the DVR to apply' });
      } catch (e) { return sendJSON(res, 502, { error: e.message }); }
    }
    try {
      const out = await tn.exec(`cat ${CONF_PATH} 2>/dev/null || cat ${CONF_SATA} 2>/dev/null`);
      return sendJSON(res, 200, { text: out.replace(/\r/g, '') });
    } catch (e) { return sendJSON(res, 502, { error: e.message }); }
  }
  if (p === '/api/restart' && POST) {          // bounce the DVR process (the wrapper respawns it)
    if (!tnReady) return sendJSON(res, 503, { error: 'device shell not connected' });
    try { await tn.exec('killall dvr; echo ok'); infoAt = 0; return sendJSON(res, 200, { ok: true }); }
    catch (e) { return sendJSON(res, 502, { error: e.message }); }
  }

  // ---------------------------------------------------------------- file manager
  if (p === '/api/files') {
    if (!tnReady) return sendJSON(res, 503, { error: 'device shell not connected' });
    const dir = safePath(q.path || '/root/rec/a1');
    try { const out = await tn.exec(`ls -la "${dir}"`); return sendJSON(res, 200, { path: dir, entries: parseLs(out) }); }
    catch (e) { return sendJSON(res, 502, { error: e.message }); }
  }
  if (p === '/api/fs' && POST) {
    if (!tnReady) return sendJSON(res, 503, { error: 'device shell not connected' });
    const body = JSON.parse(await readBody(req) || '{}');
    const target = safePath(body.path); let cmd = null;
    if (body.action === 'delete') cmd = `rm -f "${target}"`;
    else if (body.action === 'mkdir') cmd = `mkdir -p "${target}"`;
    else return sendJSON(res, 400, { error: 'bad action' });
    try { await tn.exec(cmd); return sendJSON(res, 200, { ok: true }); }
    catch (e) { return sendJSON(res, 502, { error: e.message }); }
  }
  if (p === '/api/download') {
    const fp = safePath(q.path);
    const dl = http.get({ host: CFG.dvrHost, port: CFG.filePort, path: encodeURI(fp) }, (dres) => {
      res.writeHead(dres.statusCode || 200, {
        'Content-Type': 'application/octet-stream',
        'Content-Disposition': `attachment; filename="${path.posix.basename(fp)}"`,
      });
      dres.pipe(res);
    });
    dl.on('error', () => { res.writeHead(502); res.end('device fetch failed'); });
    return;
  }

  // ---------------------------------------------------------------- static
  let file = p === '/' ? '/index.html' : p;
  file = path.join(PUB, path.normalize(file).replace(/^(\.\.[/\\])+/, ''));
  fs.readFile(file, (err, buf) => {
    if (err) { res.writeHead(404); res.end('not found'); return; }
    res.writeHead(200, { 'Content-Type': MIME[path.extname(file)] || 'application/octet-stream' });
    res.end(buf);
  });
});

// ---- WebSockets: /stream (live H.264 via our device) + /shell (telnet) ----
const wssStream = new WebSocket.Server({ noServer: true });
const wssShell = new WebSocket.Server({ noServer: true });
server.on('upgrade', (req, socket, head) => {
  const p = req.url.split('?')[0];
  if (p === '/stream') wssStream.handleUpgrade(req, socket, head, (ws) => wssStream.emit('connection', ws, req));
  else if (p === '/shell') wssShell.handleUpgrade(req, socket, head, (ws) => wssShell.emit('connection', ws, req));
  else socket.destroy();
});

// live: bridge browser WS <-> raw TCP to our device stream server, one per channel.
// Only `enc_channels` channels actually run an encoder; subscribing to any other one
// would sit forever waiting for a keyframe AND burn one of the device's MAXCLI slots,
// so refuse it up front and tell the UI why.
// A browser that stops draining (backgrounded tab, throttled laptop, paused decoder) does
// NOT slow the device down — we keep reading the TCP socket regardless, so ws.send() just
// queues in this process. At ~26 Mbps that is ~3 MB/s of heap per stalled viewer, which
// eventually takes the server down and with it everyone else's live view. So cap the
// queue and drop instead. Dropping mid-stream would leave the decoder chewing on a
// headless GOP, so resume only at an SPS: the next keyframe re-initialises it cleanly.
const WS_QUEUE_MAX = 4 * 1024 * 1024;
function findSps(b) {                       // 00 00 00 01 <nal>, type 7 = SPS
  for (let i = 0; i + 4 < b.length; i++) {
    if (b[i] === 0 && b[i + 1] === 0 && b[i + 2] === 0 && b[i + 3] === 1 &&
        (b[i + 4] & 0x1f) === 7) return i;
  }
  return -1;
}
wssStream.on('connection', (ws, req) => {
  const ch = parseInt(url.parse(req.url, true).query.ch || '0', 10) || 0;
  if (!(ch >= 0 && ch < CFG.channels)) { ws.close(4400, 'bad channel'); return; }
  if (ch >= encChannels()) { ws.close(4404, 'channel not encoded'); return; }
  const tcp = net.connect(CFG.streamPort, CFG.dvrHost, () => tcp.write(Buffer.from([0x30 + ch])));
  let dropping = false, dropped = 0;
  tcp.on('data', (d) => {
    if (ws.readyState !== WebSocket.OPEN) return;
    if (!dropping && ws.bufferedAmount > WS_QUEUE_MAX) { dropping = true; dropped = 0; }
    if (dropping) {
      dropped += d.length;
      if (ws.bufferedAmount > WS_QUEUE_MAX / 4) return;   // still behind — keep dropping
      const i = findSps(d);
      if (i < 0) return;                                  // wait for a clean entry point
      dropping = false;
      console.log(`[stream] ch${ch}: viewer fell behind, dropped ${(dropped / 1e6).toFixed(1)} MB, resynced at keyframe`);
      d = d.slice(i);
    }
    ws.send(d);
  });
  tcp.on('error', () => { try { ws.close(); } catch (_) {} });
  tcp.on('close', () => { try { ws.close(); } catch (_) {} });
  ws.on('close', () => { try { tcp.destroy(); } catch (_) {} });
  ws.on('error', () => { try { tcp.destroy(); } catch (_) {} });
});

// shell: bridge browser WS <-> interactive telnet
wssShell.on('connection', (ws) => {
  const raw = new TelnetRaw(CFG.dvrHost, CFG.tnUser, CFG.tnPass).start();
  raw.on('data', (d) => { if (ws.readyState === WebSocket.OPEN) ws.send(d.toString('latin1')); });
  raw.on('close', () => { try { ws.close(); } catch (_) {} });
  raw.on('error', () => { try { ws.close(); } catch (_) {} });
  ws.on('message', (m) => { try { raw.write(m.toString()); } catch (_) {} });
  ws.on('close', () => { try { raw.close(); } catch (_) {} });
});

// report what we're talking to once the control link is actually up (send() fails fast
// while disconnected, so asking before 'open' would always look like a dead device)
ctl.on('open', () => {
  getInfo(0).then((i) => {
    if (i) console.log(`[webapp2] device fw v${i.ver}: ${i.width}x${i.height}@${i.fps} ` +
                       `enc=${i.encChannels}ch ${i.std} disk ${i.diskFreeMb} MB free`);
    else console.log('[webapp2] control port connected but INFO did not answer ' +
                     '(old firmware? rebuild device/dvr — see docs/CONTROL_PROTOCOL.md)');
  });
});
server.listen(CFG.port, () => {
  console.log(`[webapp2] http://localhost:${CFG.port}  (device ${CFG.dvrHost}: ctl ${CFG.ctlPort}, stream ${CFG.streamPort}, telnet 23)`);
});
