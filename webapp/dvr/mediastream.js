'use strict';
// Open a media connection to the DVR and emit clean H.264 frames.
// Container: [8-byte prefix][per frame: 11B pkt hdr (62 19..)][12B LEN,FRAMENO,TS][LEN bytes Annex-B].
const net = require('net');
const EventEmitter = require('events');
const { mediaRequest } = require('./protocol');

const MARK = Buffer.from([0x62, 0x19]);

class MediaStream extends EventEmitter {
  constructor(host, port, channel, stream) {
    super();
    this.host = host; this.port = port;
    this.channel = channel; this.stream = stream || 0;
    this.buf = Buffer.alloc(0);
    this.started = false;
    this.frameCount = 0;
    this.sock = null;
  }

  start() {
    this.sock = net.connect({ host: this.host, port: this.port }, () => {
      this.sock.write(mediaRequest(this.channel, this.stream));
    });
    this.sock.on('data', (d) => this._onData(d));
    this.sock.on('error', (e) => this.emit('error', e));
    this.sock.on('close', () => this.emit('close'));
    return this;
  }

  stop() { if (this.sock) { this.sock.destroy(); this.sock = null; } }

  _onData(chunk) {
    this.buf = Buffer.concat([this.buf, chunk]);
    let b = this.buf;
    let off = 0;

    if (!this.started) {
      const p = b.indexOf(MARK);
      if (p < 0) { return; }        // wait for the first packet marker
      off = p; this.started = true;
    }

    while (true) {
      if (off + 23 > b.length) break;               // need pkt(11)+frame(12) headers
      if (!(b[off] === 0x62 && b[off + 1] === 0x19)) {
        const p = b.indexOf(MARK, off + 1);
        if (p < 0) { off = Math.max(off, b.length - 1); break; }
        off = p; continue;
      }
      const fh = off + 11;
      const LEN = b.readUInt32BE(fh);
      if (LEN <= 0 || LEN > 2000000) {              // implausible -> resync
        const p = b.indexOf(MARK, off + 2);
        if (p < 0) { off = b.length; break; }
        off = p; continue;
      }
      const pStart = fh + 12;
      if (pStart + LEN > b.length) break;           // wait for the whole frame
      const payload = b.slice(pStart, pStart + LEN);
      if (!(payload[0] === 0 && payload[1] === 0 && payload[2] === 0 && payload[3] === 1)) {
        const p = b.indexOf(MARK, off + 2);         // bad frame start -> resync
        if (p < 0) { off = b.length; break; }
        off = p; continue;
      }
      this.frameCount++;
      this.emit('frame', payload);                  // clean Annex-B H.264 for one frame
      off = pStart + LEN;
    }
    this.buf = b.slice(off);
  }
}

module.exports = MediaStream;
