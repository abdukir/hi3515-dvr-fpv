'use strict';
// One DVR media connection per (channel, stream), fanned out to many consumers
// (live viewers + recorder). Caches SPS/PPS + last keyframe so new joiners decode fast.
const EventEmitter = require('events');
const MediaStream = require('./mediastream');

function nalType(frame) {
  // frame starts with 00 00 00 01; the byte after is the NAL header
  return frame.length > 4 ? (frame[4] & 0x1f) : -1;
}

class ChannelSource extends EventEmitter {
  constructor(host, port, channel, stream) {
    super();
    this.setMaxListeners(0);
    this.host = host; this.port = port; this.channel = channel; this.stream = stream;
    this.ms = null;
    this.refs = 0;
    this.sps = null; this.pps = null;   // cached parameter-set NALs (as full frames)
    this.lastFrames = 0;
    this.idleTimer = null;
  }

  _ensure() {
    if (this.ms) return;
    this.ms = new MediaStream(this.host, this.port, this.channel, this.stream).start();
    this.ms.on('frame', (frame) => {
      const t = nalType(frame);
      if (t === 7) this.sps = frame;
      else if (t === 8) this.pps = frame;
      this.emit('frame', frame);
    });
    this.ms.on('error', (e) => this.emit('error', e));
    this.ms.on('close', () => { this.ms = null; if (this.refs > 0) setTimeout(() => this._ensure(), 1500); });
  }

  // subscribe(fn): fn(frame) called per H.264 frame. Returns an unsubscribe function.
  subscribe(fn) {
    this.refs++;
    if (this.idleTimer) { clearTimeout(this.idleTimer); this.idleTimer = null; }
    this._ensure();
    // prime the new consumer with cached SPS/PPS so its decoder can init immediately
    if (this.sps) fn(this.sps);
    if (this.pps) fn(this.pps);
    this.on('frame', fn);
    return () => {
      this.removeListener('frame', fn);
      this.refs = Math.max(0, this.refs - 1);
      if (this.refs === 0) {
        // keep the DVR link a few seconds in case someone rejoins, then drop it
        this.idleTimer = setTimeout(() => { if (this.refs === 0 && this.ms) { this.ms.stop(); this.ms = null; } }, 5000);
      }
    };
  }

  get frameCount() { return this.ms ? this.ms.frameCount : 0; }
}

// registry of sources keyed by channel+stream
class SourceManager {
  constructor(host, port) { this.host = host; this.port = port; this.map = new Map(); }
  get(channel, stream) {
    const key = `${channel}_${stream}`;
    let s = this.map.get(key);
    if (!s) { s = new ChannelSource(this.host, this.port, channel, stream); this.map.set(key, s); }
    return s;
  }
}

module.exports = { ChannelSource, SourceManager, nalType };
