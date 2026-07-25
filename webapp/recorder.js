'use strict';
// Record a ChannelSource to a standard MP4 on the PC via ffmpeg (stream copy, no re-encode).
const { spawn } = require('child_process');
const path = require('path');
const fs = require('fs');

const FFMPEG = process.env.FFMPEG || path.join(__dirname, 'bin', 'ffmpeg.exe');
const REC_DIR = path.join(__dirname, 'recordings');

function ts() {
  const d = new Date();
  const p = (n) => String(n).padStart(2, '0');
  return `${d.getFullYear()}${p(d.getMonth() + 1)}${p(d.getDate())}_${p(d.getHours())}${p(d.getMinutes())}${p(d.getSeconds())}`;
}

class Recorder {
  constructor(source, channel) {
    this.source = source;
    this.channel = channel;
    this.proc = null;
    this.file = null;
    this.unsub = null;
    this.startedAt = null;
    this.bytes = 0;
  }

  start() {
    if (this.proc) return this.file;
    fs.mkdirSync(REC_DIR, { recursive: true });
    const name = `ch${this.channel + 1}_${ts()}.mp4`;
    this.file = path.join(REC_DIR, name);
    // fragmented mp4 so the file stays valid even if we stop abruptly
    this.proc = spawn(FFMPEG, [
      '-hide_banner', '-loglevel', 'error',
      '-f', 'h264', '-framerate', '30', '-i', 'pipe:0',
      '-c', 'copy',
      '-movflags', 'frag_keyframe+empty_moov+default_base_moof',
      '-y', this.file,
    ], { stdio: ['pipe', 'ignore', 'pipe'] });
    this.proc.stderr.on('data', (d) => console.log(`[rec ch${this.channel}] ffmpeg: ${d}`));
    this.proc.on('close', () => { this.proc = null; });
    this.proc.stdin.on('error', () => {});    // ignore EPIPE on stop
    this.startedAt = Date.now();
    this.unsub = this.source.subscribe((frame) => {
      if (this.proc && this.proc.stdin.writable) { this.proc.stdin.write(frame); this.bytes += frame.length; }
    });
    return name;
  }

  stop() {
    if (this.unsub) { this.unsub(); this.unsub = null; }
    if (this.proc) { try { this.proc.stdin.end(); } catch (e) {} }
    const f = this.file;
    this.file = null;
    return f;
  }

  get active() { return !!this.proc; }
  get status() {
    return { channel: this.channel, active: this.active,
      file: this.file ? path.basename(this.file) : null,
      seconds: this.startedAt ? Math.floor((Date.now() - this.startedAt) / 1000) : 0 };
  }
}

module.exports = { Recorder, REC_DIR };
