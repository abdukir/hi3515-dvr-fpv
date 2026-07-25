'use strict';
// DVR web UI backend: live H.264 streams, DVR control, file manager, and web terminal.
const http = require('http');
const fs = require('fs');
const net = require('net');
const path = require('path');
const url = require('url');
const os = require('os');
const { spawn } = require('child_process');
const WebSocket = require('ws');
const { SourceManager } = require('./dvr/channelsource');
const { ControlClient } = require('./dvr/protocol');
const { TelnetExec, TelnetRaw } = require('./dvr/telnet');
const { parseLog, toSpans } = require('./dvr/recordings');
const { IFV_SIZE, parseIndex, mergeSegments, segRange } = require('./dvr/ifvindex');

const CFG = {
  dvrHost: process.env.DVR_HOST || '192.168.1.108',
  dvrPort: parseInt(process.env.DVR_PORT || '8670', 10),
  user: process.env.DVR_USER || 'admin',
  pass: process.env.DVR_PASS || '000000',
  telnetUser: process.env.TN_USER || 'root',
  telnetPass: process.env.TN_PASS || '',
  filePort: 8081,                 // busybox httpd on the device
  httpPort: parseInt(process.env.PORT || '8090', 10),
  channels: 4,
};

const MIME = { '.html': 'text/html', '.js': 'application/javascript', '.css': 'text/css',
  '.png': 'image/png', '.jpg': 'image/jpeg', '.svg': 'image/svg+xml', '.ico': 'image/x-icon' };
const PUB = path.join(__dirname, 'public');
const sources = new SourceManager(CFG.dvrHost, CFG.dvrPort);

// ---- recording clip transcode cache ----
const FFMPEG = process.env.FFMPEG || path.join(__dirname, 'bin', 'ffmpeg.exe');
const CACHE = path.join(__dirname, 'cache');
fs.mkdirSync(CACHE, { recursive: true });
let ncPort = 5200;
const clipJobs = new Map();   // key -> Promise (in-flight/done)

function getDeviceFile(p) {   // fetch a device file via busybox httpd
  return new Promise((resolve, reject) => {
    http.get({ host: CFG.dvrHost, port: CFG.filePort, path: encodeURI(p) }, (res) => {
      if (res.statusCode !== 200) { res.resume(); return reject(new Error('http ' + res.statusCode)); }
      const chunks = []; res.on('data', (d) => chunks.push(d)); res.on('end', () => resolve(Buffer.concat(chunks)));
    }).on('error', reject);
  });
}

// Stream an exact device byte range [localOff, localOff+count) to an HTTP response.
// dd is block-aligned (fast on old busybox) and the head remainder is dropped in Node,
// so the first byte is exact. The dd|nc is BACKGROUNDED on the device (`& echo`) so the
// shared telnet command queue frees immediately — ffmpeg opens overlapping connections
// (header read + seek target) that need their listeners up concurrently. When the HTTP
// client disconnects we destroy our socket, which SIGPIPEs that dd|nc.
function streamDeviceRange(ifvPath, localOff, count, res) {
  const BS = 65536;
  const alignedOff = Math.floor(localOff / BS) * BS;
  const headSkip = localOff - alignedOff;
  const blocks = Math.ceil((headSkip + count) / BS);
  const port = (ncPort = ncPort >= 5250 ? 5200 : ncPort + 1);
  tn.exec(`dd if=${ifvPath} bs=${BS} skip=${alignedOff / BS} count=${blocks} 2>/dev/null | nc -l -p ${port} >/dev/null 2>&1 & echo bg`);
  let sock = null, skipped = 0, forwarded = 0, done = false;
  const finish = () => { if (done) return; done = true; if (sock) sock.destroy(); try { res.end(); } catch (e) {} };
  const onData = (d) => {
    if (skipped < headSkip) { const drop = Math.min(headSkip - skipped, d.length); skipped += drop; d = d.slice(drop); if (!d.length) return; }
    if (forwarded + d.length > count) d = d.slice(0, count - forwarded);
    forwarded += d.length;
    if (d.length && !res.write(d)) { if (sock) sock.pause(); }
    if (forwarded >= count) finish();
  };
  setTimeout(() => {
    // always connect (even if the client already went away) so the backgrounded nc is
    // reaped rather than left listening.
    sock = net.connect({ host: CFG.dvrHost, port }, () => { if (done) sock.destroy(); });
    sock.on('data', onData);
    res.on('drain', () => { if (sock) sock.resume(); });
    sock.on('close', () => finish());
    sock.on('error', () => finish());
    sock.setTimeout(120000, () => sock.destroy());
  }, 800);
  res.on('close', finish);   // ffmpeg/browser disconnected -> stop pulling from the device
}

const CLIP_WINDOW_SEC = parseInt(process.env.CLIP_WINDOW_SEC || '1800', 10);   // max transcode window

// Prepare a browser-playable MP4 for a recorded segment, seeking to `t` seconds in.
// ffmpeg reads the .ifv through our own /api/ifvstream range-proxy (a seekable view of
// the on-device file), so its `-ss` seek uses the container's internal index and only
// fetches the header + the bytes at the seek point — no full-segment pull, no matter how
// large the recording. Frame-accurate (verified: seek lands on the exact OSD second).
// Cached by (channel,start,end,t). Returns the cached mp4 path.
function prepareClip(channel, startOff, endOff, t) {
  t = Math.max(0, t || 0);
  const key = `ch${channel}_${startOff}_${endOff}_t${t}`;
  if (clipJobs.has(key)) return clipJobs.get(key);
  const mp4 = path.join(CACHE, key + '.mp4');
  const job = (async () => {
    if (fs.existsSync(mp4) && fs.statSync(mp4).size > 1000) return mp4;
    if (!tnReady) throw new Error('device shell not connected');
    const r = segRange({ startOff, endOff });
    const url = `http://127.0.0.1:${CFG.httpPort}/api/ifvstream?ch=${channel}` +
      `&file=${r.file}&base=${r.localStart}&len=${r.length}`;
    const args = ['-hide_banner', '-loglevel', 'error'];
    if (t > 0) args.push('-ss', String(t));
    args.push('-f', 'ifv', '-i', url, '-t', String(CLIP_WINDOW_SEC),
      '-c:v', 'libx264', '-preset', 'veryfast', '-pix_fmt', 'yuv420p',
      '-movflags', '+faststart', '-y', mp4);
    await new Promise((resolve, reject) => {
      const p = spawn(FFMPEG, args);
      p.on('close', (code) => { code === 0 || (fs.existsSync(mp4) && fs.statSync(mp4).size > 1000) ? resolve() : reject(new Error('ffmpeg failed')); });
      p.on('error', reject);
    });
    return mp4;
  })();
  clipJobs.set(key, job);
  job.catch(() => clipJobs.delete(key));   // allow retry on failure
  return job;
}

function serveFileRange(req, res, file, type) {
  const size = fs.statSync(file).size;
  const range = req.headers.range;
  if (range) {
    const m = range.match(/bytes=(\d+)-(\d*)/);
    const start = parseInt(m[1], 10); const end = m[2] ? parseInt(m[2], 10) : size - 1;
    res.writeHead(206, { 'Content-Type': type, 'Content-Range': `bytes ${start}-${end}/${size}`,
      'Accept-Ranges': 'bytes', 'Content-Length': end - start + 1 });
    fs.createReadStream(file, { start, end }).pipe(res);
  } else {
    res.writeHead(200, { 'Content-Type': type, 'Content-Length': size, 'Accept-Ranges': 'bytes' });
    fs.createReadStream(file).pipe(res);
  }
}

// ---- persistent DVR control connection ----
let ctrl = null, ctrlReady = false;
function connectControl() {
  const c = new ControlClient(CFG.dvrHost, CFG.dvrPort, CFG.user, CFG.pass);
  c.connect().then(() => {
    ctrl = c; ctrlReady = true; console.log('[control] logged in');
    c.on('close', () => { ctrlReady = false; setTimeout(connectControl, 3000); });
    c.on('error', () => {});
  }).catch((e) => { ctrlReady = false; console.log('[control] login failed:', e.message); setTimeout(connectControl, 3000); });
}
connectControl();

// ---- persistent telnet session for file ops + ensure httpd is up ----
let tn = null, tnReady = false;
function connectTelnet() {
  const t = new TelnetExec(CFG.dvrHost, CFG.telnetUser, CFG.telnetPass);
  t.connect().then(async () => {
    tn = t; tnReady = true; console.log('[telnet] shell ready');
    const pid = await t.exec(`pidof httpd >/dev/null || httpd -h / -p ${CFG.filePort}; pidof httpd`);
    console.log('[telnet] file server (httpd) pid:', pid.trim());
  }).catch((e) => { tnReady = false; console.log('[telnet] failed:', e.message); setTimeout(connectTelnet, 4000); });
}
connectTelnet();

// parse `ls -la` into structured entries
function parseLs(out, base) {
  const entries = [];
  for (const line of out.split('\n')) {
    const m = line.match(/^([dl\-][rwx\-]{9})\s+\d+\s+\S+\s+\S+\s+(\d+)\s+(\w+\s+\d+\s+[\d:]+)\s+(.+?)(\s*->\s*.+)?$/);
    if (!m) continue;
    const name = m[4].replace(/\x1b\[[0-9;]*m/g, '').trim();
    if (name === '.' ) continue;
    entries.push({ name, size: parseInt(m[2], 10), dir: m[1][0] === 'd', link: m[1][0] === 'l', mtime: m[3] });
  }
  return entries;
}

function sendJSON(res, code, obj) { res.writeHead(code, { 'Content-Type': 'application/json' }); res.end(JSON.stringify(obj)); }
function readBody(req) { return new Promise((r) => { let b = ''; req.on('data', (d) => b += d); req.on('end', () => r(b)); }); }
function safePath(p) { p = '/' + String(p || '/').replace(/\\/g, '/'); return path.posix.normalize(p).replace(/\.\.(\/|$)/g, ''); }

const server = http.createServer(async (req, res) => {
  const u = url.parse(req.url, true); let p = u.pathname;

  if (p === '/api/config') return sendJSON(res, 200, { channels: CFG.channels, dvr: CFG.dvrHost });

  if (p === '/api/recordstate') {
    if (!ctrlReady) return sendJSON(res, 503, { error: 'DVR control not connected' });
    try { const st = await ctrl.getRecordState(); return sendJSON(res, 200, { state: ControlClient.maskToChannels(st, CFG.channels) }); }
    catch (e) { return sendJSON(res, 502, { error: e.message }); }
  }
  if (p === '/api/record' && req.method === 'POST') {
    if (!ctrlReady) return sendJSON(res, 503, { error: 'DVR control not connected' });
    const ch = parseInt(u.query.ch, 10), on = u.query.on === '1' || u.query.on === 'true';
    if (!(ch >= 0 && ch < CFG.channels)) return sendJSON(res, 400, { error: 'bad channel' });
    try { const st = await ctrl.setRecord(ch, on); return sendJSON(res, 200, { ok: true, state: ControlClient.maskToChannels(st, CFG.channels) }); }
    catch (e) { return sendJSON(res, 502, { error: e.message }); }
  }

  // ---- file manager ----
  if (p === '/api/files') {
    if (!tnReady) return sendJSON(res, 503, { error: 'device shell not connected' });
    const dir = safePath(u.query.path || '/root/rec/a1');
    try { const out = await tn.exec(`ls -la "${dir}"`); return sendJSON(res, 200, { path: dir, entries: parseLs(out, dir) }); }
    catch (e) { return sendJSON(res, 502, { error: e.message }); }
  }
  if (p === '/api/fs' && req.method === 'POST') {
    if (!tnReady) return sendJSON(res, 503, { error: 'device shell not connected' });
    const body = JSON.parse(await readBody(req) || '{}');
    const target = safePath(body.path); let cmd = null;
    if (body.action === 'delete') cmd = `rm -f "${target}"`;
    else if (body.action === 'mkdir') cmd = `mkdir -p "${target}"`;
    else return sendJSON(res, 400, { error: 'bad action' });
    try { await tn.exec(cmd); return sendJSON(res, 200, { ok: true }); }
    catch (e) { return sendJSON(res, 502, { error: e.message }); }
  }
  if (p === '/api/download') {           // proxy from the device httpd, stream to browser
    const fp = safePath(u.query.path);
    const dl = http.get({ host: CFG.dvrHost, port: CFG.filePort, path: encodeURI(fp) }, (dres) => {
      res.writeHead(dres.statusCode || 200, {
        'Content-Type': 'application/octet-stream',
        'Content-Length': dres.headers['content-length'] || undefined,
        'Content-Disposition': `attachment; filename="${path.posix.basename(fp)}"`,
      });
      dres.pipe(res);
    });
    dl.on('error', () => { res.writeHead(502); res.end('device fetch failed'); });
    return;
  }

  // ---- recordings ----
  if (p === '/api/recordings') {
    if (!tnReady) return sendJSON(res, 503, { error: 'device shell not connected' });
    const ch = Math.max(0, Math.min(CFG.channels - 1, parseInt(u.query.ch || '0', 10)));
    const part = 'a' + (ch + 1);
    try {
      // .ifv files (for count -> circular-buffer size, and raw download links)
      const lsOut = await tn.exec(`ls -la /root/rec/${part}/*.ifv 2>/dev/null`);
      const files = [];
      for (const line of lsOut.split('\n')) {
        const m = line.match(/(\d+)\s+(\w+\s+\d+\s+[\d:]+)\s+(\S+\.ifv)\s*$/);
        if (m) files.push({ name: path.posix.basename(m[3]), size: parseInt(m[1], 10), mtime: m[2] });
      }
      const totalBytes = Math.max(files.length, 1) * IFV_SIZE;

      // recorded segments from the index (exact time -> byte-offset map)
      let records = [];
      for (const idx of ['index00.bin', 'index01.bin']) {
        try { const b = await getDeviceFile(`/root/rec/${part}/${idx}`); records = records.concat(parseIndex(b, totalBytes)); }
        catch (e) { /* index may not exist */ }
      }
      const segments = mergeSegments(records);

      // log-based spans (coarse cross-check; independent of the index)
      let samples = [];
      for (const lg of ['log.txt', 'log00.txt', 'log01.txt']) {
        try { const b = await getDeviceFile(`/root/rec/${part}/${lg}`); if (b && b.length > 40) samples = samples.concat(parseLog(b)); }
        catch (e) { /* not present */ }
      }
      const spans = toSpans(samples);
      return sendJSON(res, 200, { channel: ch, segments, spans, files });
    } catch (e) { return sendJSON(res, 502, { error: e.message }); }
  }
  if (p === '/api/clip') {
    const ch = Math.max(0, Math.min(CFG.channels - 1, parseInt(u.query.ch || '0', 10)));
    const start = parseInt(u.query.start, 10), end = parseInt(u.query.end, 10);
    const t = Math.max(0, parseFloat(u.query.t) || 0);   // seek: seconds into the segment
    if (!(start >= 0 && end > start)) return sendJSON(res, 400, { error: 'bad segment range' });
    try {
      const mp4 = await prepareClip(ch, start, end, t);
      return serveFileRange(req, res, mp4, 'video/mp4');
    } catch (e) { return sendJSON(res, 502, { error: e.message }); }
  }
  // seekable range-proxy over a device .ifv segment (backs ffmpeg's -ss for /api/clip)
  if (p === '/api/ifvstream') {
    if (!tnReady) { res.writeHead(503); return res.end('device shell not connected'); }
    const ch = Math.max(0, Math.min(CFG.channels - 1, parseInt(u.query.ch || '0', 10)));
    const file = String(u.query.file || '').replace(/[^\w.]/g, '');
    const base = parseInt(u.query.base, 10), len = parseInt(u.query.len, 10);
    if (!/^fly\d+\.ifv$/.test(file) || !(base >= 0) || !(len > 0)) { res.writeHead(400); return res.end('bad range params'); }
    const ifvPath = `/root/rec/a${ch + 1}/${file}`;
    let s = 0, e = len - 1;
    const rng = req.headers.range && req.headers.range.match(/bytes=(\d+)-(\d*)/);
    if (rng) { s = parseInt(rng[1], 10); e = rng[2] ? Math.min(parseInt(rng[2], 10), len - 1) : len - 1; }
    const count = e - s + 1;
    const head = { 'Content-Type': 'application/octet-stream', 'Accept-Ranges': 'bytes', 'Content-Length': count };
    if (rng) head['Content-Range'] = `bytes ${s}-${e}/${len}`;
    res.writeHead(rng ? 206 : 200, head);
    if (req.method === 'HEAD') return res.end();
    return streamDeviceRange(ifvPath, base + s, count, res);
  }

  // static
  if (p === '/') p = '/index.html';
  const file = path.join(PUB, path.normalize(p).replace(/^(\.\.[/\\])+/, ''));
  fs.readFile(file, (err, data) => {
    if (err) { res.writeHead(404); return res.end('not found'); }
    res.writeHead(200, { 'Content-Type': MIME[path.extname(file)] || 'application/octet-stream' });
    res.end(data);
  });
});

// ---- WebSockets (noServer + manual upgrade routing) ----
const wssStream = new WebSocket.Server({ noServer: true });
const wssShell = new WebSocket.Server({ noServer: true });
server.on('upgrade', (req, socket, head) => {
  const p = url.parse(req.url).pathname;
  if (p === '/stream') wssStream.handleUpgrade(req, socket, head, (ws) => wssStream.emit('connection', ws, req));
  else if (p === '/shell') wssShell.handleUpgrade(req, socket, head, (ws) => wssShell.emit('connection', ws, req));
  else socket.destroy();
});

wssStream.on('connection', (ws, req) => {
  const q = url.parse(req.url, true).query;
  const channel = Math.max(0, Math.min(CFG.channels - 1, parseInt(q.ch || '0', 10)));
  const stream = parseInt(q.stream || '0', 10);
  const unsub = sources.get(channel, stream).subscribe((h264) => { if (ws.readyState === WebSocket.OPEN) ws.send(h264); });
  ws.on('close', () => unsub()); ws.on('error', () => unsub());
});

wssShell.on('connection', (ws) => {
  const raw = new TelnetRaw(CFG.dvrHost, CFG.telnetUser, CFG.telnetPass).start();
  raw.on('data', (d) => { if (ws.readyState === WebSocket.OPEN) ws.send(d.toString('latin1')); });
  raw.on('close', () => { if (ws.readyState === WebSocket.OPEN) ws.close(); });
  ws.on('message', (m) => raw.write(m.toString()));
  ws.on('close', () => raw.close());
});

server.listen(CFG.httpPort, '0.0.0.0', () => {
  const ips = [].concat(...Object.values(os.networkInterfaces()))
    .filter((i) => i.family === 'IPv4' && !i.internal).map((i) => i.address);
  console.log(`DVR web UI:  http://localhost:${CFG.httpPort}`);
  ips.forEach((ip) => console.log(`   on LAN:   http://${ip}:${CFG.httpPort}`));
  console.log(`   DVR:      ${CFG.dvrHost}:${CFG.dvrPort}`);
});
