'use strict';
// RR104P / TLNetDvr 8670 protocol — control client + media request builder.
// See PROTOCOL.md for the reverse-engineered format.
const net = require('net');
const EventEmitter = require('events');

const MAGIC = Buffer.from('6178dab5d38e43db9ed7f22078361879', 'hex');
const CONTROL_PREAMBLE = Buffer.concat([MAGIC, Buffer.from([0x01, 0x01, 0x00, 0x00])]);

const CMD = {
  LOGIN: 0x2711, GET_DEVINFO: 0x2713, GET_VIDEO: 0x2733, SET_VIDEO: 0x2734,
  GET_RECPARAM: 0x272f, SET_RECPARAM: 0x2730, GET_SUBSTREAM: 0x2731, SET_SUBSTREAM: 0x2732,
  GET_RECSTATE: 0x2757, SET_RECSTATE: 0x2758, REC_SEARCH: 0x275a, PTZ: 0x2756,
  GET_TIME: 0x2751, REBOOT: 0x2755, MAKE_KEYFRAME: 0x2781,
};

function fixed(str, len) {
  const b = Buffer.alloc(len);
  Buffer.from(String(str), 'latin1').copy(b, 0, 0, Math.min(String(str).length, len - 1));
  return b;
}
function frame(cmd, seq, payload) {
  payload = payload || Buffer.alloc(0);
  const b = Buffer.alloc(12 + payload.length);
  b.writeUInt32BE(b.length, 0);
  b.writeUInt16BE(0x0000, 4);      // request
  b.writeUInt16BE(cmd, 6);
  b.writeUInt16BE(seq & 0xffff, 8);
  b.writeUInt16BE(0x0100, 10);
  payload.copy(b, 12);
  return b;
}
function loginPayload(user, pass) {
  return Buffer.concat([fixed(user, 12), fixed(pass, 12), fixed('00:00:00:00:00:00', 18), Buffer.from([127, 0, 0, 1])]);
}
// 100-byte media (StartRealPlay) request: MAGIC + 01 02 00 00 00 <ch> <stream>
function mediaRequest(channel, stream) {
  const b = Buffer.alloc(100);
  MAGIC.copy(b, 0);
  b[16] = 0x01; b[17] = 0x02;
  b[21] = channel & 0xff;
  b[22] = (stream || 0) & 0xff;   // 0 = main
  return b;
}

class ControlClient extends EventEmitter {
  constructor(host, port, user, pass) {
    super();
    this.host = host; this.port = port; this.user = user; this.pass = pass;
    this.sock = null; this.buf = Buffer.alloc(0);
    this.seq = 0x20; this.pending = new Map(); this.ready = false;
  }

  connect() {
    return new Promise((resolve, reject) => {
      const sock = net.connect({ host: this.host, port: this.port }, () => {
        sock.write(CONTROL_PREAMBLE);
        // login (seq reserved 0x02); resolve when its reply arrives
        this._loginResolve = resolve;
        sock.write(frame(CMD.LOGIN, 0x02, loginPayload(this.user, this.pass)));
      });
      this.sock = sock;
      sock.on('data', (d) => this._onData(d));
      sock.on('error', (e) => { this.ready = false; if (!this.ready) reject(e); this.emit('error', e); });
      sock.on('close', () => { this.ready = false; this.emit('close'); });
      sock.setTimeout(0);
    });
  }

  _onData(d) {
    this.buf = Buffer.concat([this.buf, d]);
    let off = 0;
    while (off + 12 <= this.buf.length) {
      const len = this.buf.readUInt32BE(off);
      if (len < 12 || off + len > this.buf.length) break;
      const type = this.buf.readUInt16BE(off + 4);
      const seq = this.buf.readUInt16BE(off + 8);
      const payload = this.buf.slice(off + 12, off + len);
      if (type === 0x0002) {              // reply
        if (seq === 0x02 && this._loginResolve) {
          this.ready = true; const r = this._loginResolve; this._loginResolve = null; r(this);
        }
        const p = this.pending.get(seq);
        if (p) { this.pending.delete(seq); clearTimeout(p.timer); p.resolve(payload); }
      } else if (type === 0x0001) {
        this.emit('event', this.buf.readUInt16BE(off + 6), payload);
      }
      off += len;
    }
    this.buf = this.buf.slice(off);
  }

  sendCommand(cmd, payload, timeoutMs) {
    return new Promise((resolve, reject) => {
      if (!this.sock || !this.ready) return reject(new Error('control not ready'));
      const seq = this.seq; this.seq = (this.seq + 1) & 0xffff; if (this.seq < 0x20) this.seq = 0x20;
      const timer = setTimeout(() => { this.pending.delete(seq); reject(new Error('cmd timeout 0x' + cmd.toString(16))); }, timeoutMs || 5000);
      this.pending.set(seq, { resolve, timer });
      this.sock.write(frame(cmd, seq, payload));
    });
  }

  async getRecordState() {
    const r = await this.sendCommand(CMD.GET_RECSTATE, Buffer.alloc(0));
    return Buffer.from(r.slice(0, 4));
  }
  // record state is a big-endian u32 bitmask (channel N = bit N)
  async setRecord(channel, on) {
    const cur = await this.getRecordState();
    let mask = cur.readUInt32BE(0);
    if (on) mask |= (1 << channel); else mask &= ~(1 << channel);
    const out = Buffer.alloc(4); out.writeUInt32BE(mask >>> 0, 0);
    await this.sendCommand(CMD.SET_RECSTATE, out);
    return this.getRecordState();
  }
  // decode a raw state buffer into a per-channel array
  static maskToChannels(buf, n) {
    const mask = buf.readUInt32BE(0); const a = [];
    for (let i = 0; i < n; i++) a.push((mask >> i) & 1);
    return a;
  }
  close() { if (this.sock) { this.sock.destroy(); this.sock = null; } }
}

module.exports = { MAGIC, CONTROL_PREAMBLE, CMD, frame, loginPayload, mediaRequest, ControlClient };
