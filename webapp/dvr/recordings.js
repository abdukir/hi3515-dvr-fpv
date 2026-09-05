'use strict';
// Recording timeline from the DVR's per-channel log.txt (per-second record-status samples).
// Each channel records to partition a1..a4. Log record: [flag '0'/'1'][ts u32le][name 32B].

const http = require('http');

function fetchDevice(host, port, p) {
  return new Promise((resolve, reject) => {
    http.get({ host, port, path: encodeURI(p) }, (res) => {
      const chunks = []; res.on('data', (d) => chunks.push(d));
      res.on('end', () => resolve(Buffer.concat(chunks)));
    }).on('error', reject);
  });
}

// parse log.txt -> [{ts, on}] samples (one channel)
function parseLog(buf) {
  const samples = [];
  let i = 0;
  const NAME = Buffer.from('Channel');
  while (true) {
    const o = buf.indexOf(NAME, i);
    if (o < 0) break;
    i = o + 7;
    if (o < 5) continue;
    const flag = buf[o - 5];               // '0'(0x30) or '1'(0x31)
    const ts = buf.readUInt32LE(o - 4);
    if (ts > 0x60000000 && ts < 0x80000000 && (flag === 0x30 || flag === 0x31)) {
      samples.push({ ts, on: flag === 0x31 });
    }
  }
  return samples;
}

// coalesce consecutive "on" samples into spans (gap tolerance in seconds)
function toSpans(samples, gap) {
  gap = gap || 5;
  samples.sort((a, b) => a.ts - b.ts);
  const spans = []; let cur = null;
  for (const s of samples) {
    if (!s.on) { if (cur) { spans.push(cur); cur = null; } continue; }
    if (cur && s.ts - cur.end <= gap) cur.end = s.ts;
    else { if (cur) spans.push(cur); cur = { start: s.ts, end: s.ts }; }
  }
  if (cur) spans.push(cur);
  return spans.filter((s) => s.end > s.start).map((s) => ({ start: s.start, end: s.end + 1 }));
}

class Recordings {
  constructor(host, filePort) { this.host = host; this.filePort = filePort; }
  partition(channel) { return 'a' + (channel + 1); }        // ch0->a1 ... ch3->a4

  async spansForChannel(channel) {
    const part = this.partition(channel);
    let samples = [];
    for (const logName of ['log.txt', 'log00.txt', 'log01.txt']) {
      try {
        const buf = await fetchDevice(this.host, this.filePort, `/root/rec/${part}/${logName}`);
        if (buf && buf.length > 40) samples = samples.concat(parseLog(buf));
      } catch (e) { /* file may not exist */ }
    }
    return toSpans(samples);
  }

  // list the .ifv files (for time->file mapping) with sizes
  async ifvFiles(channel) {
    // handled by caller via telnet ls; kept here for future index use
    return [];
  }
}

module.exports = { Recordings, parseLog, toSpans };
