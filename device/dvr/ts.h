/* ts.h — minimal freestanding MPEG-TS muxer for H.264 (video only), for the DVR
 * recorder. One tsmux per active recording. Produces a standard .ts that plays
 * directly and losslessly remuxes to MP4 (ffmpeg -c copy). Carries real PTS/PCR so
 * playback timing is correct even when a noisy signal drops frames.
 *
 * Layout: PAT(PID0) + PMT(PID0x1000) resent periodically; one video PES per access
 * unit on PID 0x0100 (stream_type 0x1B = H.264), 33-bit PTS @ 90 kHz, PCR in the
 * adaptation field of each keyframe. 188-byte packets with continuity counters.
 *
 * Requires oabi.h (sys_write, memcpy/memset from dvr.c). Feed raw Annex-B access
 * units (each starting 00 00 00 01 …); keyframe AUs carry SPS+PPS+IDR in-band.
 */
#ifndef DVR_TS_H
#define DVR_TS_H

#include "oabi.h"

/* provided by dvr.c (freestanding) */
void *memset(void *d, int c, size_t n);
void *memcpy(void *d, const void *s, size_t n);

#define TS_PID_PMT  0x1000
#define TS_PID_VID  0x0100
#define TS_PSI_EVERY 20        /* resend PAT/PMT every N access units */

/* Output buffer. Every TS packet used to be its own 188-byte sys_write: ~520 syscalls
 * per frame at this bitrate, ~15k/s at 30 fps, each one traversing vfat. That cost is
 * paid on the encoder-drain path, so it starved VENC and dropped ~20% of frames. One
 * 64 KB buffer turns those 520 calls into ~2. Sized per muxer (one per channel). */
#define TS_OBUF (64*1024)

typedef struct {
    int fd;
    unsigned cc_pat, cc_pmt, cc_vid;      /* 4-bit continuity counters */
    unsigned long long pts;               /* 90 kHz */
    unsigned pts_inc;                      /* 90000 / fps per AU */
    unsigned au_since_psi;
    int started;
    int werr;                              /* a write failed — the file is now suspect */
    unsigned olen;                         /* bytes pending in obuf */
    unsigned char obuf[TS_OBUF];
} tsmux;

/* Write out whatever is buffered. Loops on short writes and latches failures: a single
 * sys_write of up to 64 KB is far more likely to come up short than the 188-byte writes
 * this replaced, and dropping the remainder would silently corrupt the stream — exactly
 * where you least want it, at a disk filling up mid-flight. m->werr tells the recorder to
 * stop rather than keep producing a broken file. */
static void ts_flush(tsmux *m){
    if(m->olen && m->fd >= 0){
        unsigned off = 0;
        while(off < m->olen){
            long n = sys_write(m->fd, m->obuf + off, m->olen - off);
            if(n <= 0){ m->werr = 1; break; }
            off += (unsigned)n;
        }
    }
    m->olen = 0;
}
/* Reserve the next 188-byte slot and build the packet IN PLACE. Staging into a local
 * pkt[188] and copying cost a second full pass over every recorded byte (~96 KB per
 * frame) on the encoder-drain path — pure overhead, since the packet is assembled
 * exactly once and never revisited. */
static unsigned char *ts_slot(tsmux *m){
    if(m->olen + 188 > TS_OBUF) ts_flush(m);
    { unsigned char *p = m->obuf + m->olen; m->olen += 188; return p; }
}

/* MPEG-2 systems CRC32 (poly 0x04C11DB7, MSB-first, init 0xFFFFFFFF, no final xor) */
static unsigned ts_crc32(const unsigned char *d, int n){
    unsigned crc = 0xFFFFFFFFu; int i, j;
    for(i=0;i<n;i++){
        crc ^= (unsigned)d[i] << 24;
        for(j=0;j<8;j++) crc = (crc & 0x80000000u) ? (crc<<1) ^ 0x04C11DB7u : (crc<<1);
    }
    return crc;
}

/* write a PSI section as a single TS packet (section fits in 184 bytes) */
static void ts_send_section(tsmux *m, unsigned pid, unsigned *cc, const unsigned char *sec, int seclen){
    unsigned char *pkt = ts_slot(m); int p=0, i;
    pkt[p++] = 0x47;
    pkt[p++] = 0x40 | ((pid>>8)&0x1f);     /* payload_unit_start=1 */
    pkt[p++] = pid & 0xff;
    pkt[p++] = 0x10 | (*cc & 0x0f);        /* payload only */
    (*cc)++;
    pkt[p++] = 0x00;                        /* pointer_field */
    for(i=0;i<seclen;i++) pkt[p++] = sec[i];
    while(p<188) pkt[p++] = 0xff;           /* stuffing */
}

static void ts_send_pat(tsmux *m){
    unsigned char s[16]; int n=0; unsigned crc;
    s[n++]=0x00;                     /* table_id PAT */
    s[n++]=0xb0; s[n++]=0x0d;        /* syntax=1, section_length=13 */
    s[n++]=0x00; s[n++]=0x01;        /* transport_stream_id */
    s[n++]=0xc1;                     /* version 0, current_next=1 */
    s[n++]=0x00; s[n++]=0x00;        /* section_number, last */
    s[n++]=0x00; s[n++]=0x01;        /* program_number 1 */
    s[n++]=0xe0|((TS_PID_PMT>>8)&0x1f); s[n++]=TS_PID_PMT&0xff;  /* PMT PID */
    crc = ts_crc32(s, n);
    s[n++]=(crc>>24)&0xff; s[n++]=(crc>>16)&0xff; s[n++]=(crc>>8)&0xff; s[n++]=crc&0xff;
    ts_send_section(m, 0x0000, &m->cc_pat, s, n);
}

static void ts_send_pmt(tsmux *m){
    unsigned char s[24]; int n=0; unsigned crc;
    s[n++]=0x02;                     /* table_id PMT */
    s[n++]=0xb0; s[n++]=0x12;        /* syntax=1, section_length=18 */
    s[n++]=0x00; s[n++]=0x01;        /* program_number 1 */
    s[n++]=0xc1;                     /* version 0, current_next */
    s[n++]=0x00; s[n++]=0x00;        /* section_number, last */
    s[n++]=0xe0|((TS_PID_VID>>8)&0x1f); s[n++]=TS_PID_VID&0xff;  /* PCR_PID = video */
    s[n++]=0xf0; s[n++]=0x00;        /* program_info_length 0 */
    s[n++]=0x1b;                     /* stream_type H.264 */
    s[n++]=0xe0|((TS_PID_VID>>8)&0x1f); s[n++]=TS_PID_VID&0xff;  /* elementary PID */
    s[n++]=0xf0; s[n++]=0x00;        /* ES_info_length 0 */
    crc = ts_crc32(s, n);
    s[n++]=(crc>>24)&0xff; s[n++]=(crc>>16)&0xff; s[n++]=(crc>>8)&0xff; s[n++]=crc&0xff;
    ts_send_section(m, TS_PID_PMT, &m->cc_pmt, s, n);
}

static void ts_put_pcr(unsigned char *b, unsigned long long base){
    b[0]=(base>>25)&0xff; b[1]=(base>>17)&0xff; b[2]=(base>>9)&0xff; b[3]=(base>>1)&0xff;
    b[4]=((base&1)<<7)|0x7e; b[5]=0x00;     /* pcr_ext = 0 */
}
/* 5-byte PTS field, prefix '0010' (PTS only) */
static void ts_put_pts(unsigned char *b, unsigned long long pts){
    b[0]=0x21 | ((pts>>29)&0x0e);
    b[1]=(pts>>22)&0xff;
    b[2]=0x01 | ((pts>>14)&0xfe);
    b[3]=(pts>>7)&0xff;
    b[4]=0x01 | ((pts<<1)&0xfe);
}

/* open a recording: fd already opened by caller; set fps for PTS increment */
static void ts_open(tsmux *m, int fd, unsigned fps){
    memset(m, 0, sizeof(*m));
    m->fd = fd;
    m->pts_inc = fps ? (90000u / fps) : 3000u;
    m->pts = 90000;                 /* small initial offset */
    m->started = 1;
    m->au_since_psi = TS_PSI_EVERY; /* force PSI on first AU */
}

/* Mux one Annex-B access unit (frame). is_key => carry PCR + resend PSI.
 *
 * pts90: explicit 90 kHz timestamp, or 0 to advance by the nominal frame interval
 * from ts_open's fps. Anything recording a real capture should pass a clock-derived
 * value: the encoder does NOT necessarily deliver at the configured rate (measured
 * ~23 fps against a configured 30 on this hardware, and it varies with load), so
 * stamping the nominal interval makes the file play fast and report a duration
 * shorter than the wall-clock recording. See rec_start_ms[] in dvr.c. */
static void ts_write(tsmux *m, const unsigned char *au, unsigned len, int is_key,
                     unsigned long long pts90){
    unsigned char peshdr[14]; int hlen=0;
    if(!m->started || len==0) return;
    if(pts90) m->pts = pts90;

    if(m->au_since_psi >= TS_PSI_EVERY || is_key){ ts_send_pat(m); ts_send_pmt(m); m->au_since_psi=0; }
    m->au_since_psi++;

    /* PES header: start(3) stream_id(1) len(2)=0 flags(2) hdrlen(1) PTS(5) */
    peshdr[hlen++]=0x00; peshdr[hlen++]=0x00; peshdr[hlen++]=0x01;
    peshdr[hlen++]=0xe0;                 /* video stream_id */
    peshdr[hlen++]=0x00; peshdr[hlen++]=0x00;   /* PES_packet_length = 0 (unbounded) */
    peshdr[hlen++]=0x80;                 /* '10', no scrambling/priority/etc */
    peshdr[hlen++]=0x80;                 /* PTS_only flag */
    peshdr[hlen++]=0x05;                 /* PES_header_data_length */
    ts_put_pts(&peshdr[hlen], m->pts); hlen+=5;

    unsigned total = (unsigned)hlen + len, sent = 0;
    int first = 1;
    while(sent < total){
        unsigned char *pkt = ts_slot(m); int p=0;
        unsigned remaining = total - sent;
        int af = 0; unsigned char afc[184]; int afn = 0;   /* adaptation content after length byte */

        if(first && is_key){ afc[afn++]=0x10; ts_put_pcr(&afc[afn], m->pts); afn+=6; af=1; }

        /* payload space before considering padding */
        unsigned avail = 184 - (af ? (1+afn) : 0);
        unsigned take = remaining < avail ? remaining : avail;
        unsigned pad = avail - take;      /* only nonzero on the final short packet */
        if(pad){
            /* grow/create adaptation field to consume 'pad' bytes so packet stays 188 */
            int target_total = 184 - (int)take;      /* AF bytes incl length byte */
            int target_content = target_total - 1;   /* bytes after length byte */
            if(!af){ af=1; afn=0; }
            if(afn==0 && target_content>=1){ afc[afn++]=0x00; }   /* flags=0 (no PCR) */
            while(afn < target_content) afc[afn++]=0xff;          /* stuffing */
        }

        pkt[p++]=0x47;
        pkt[p++]=(first?0x40:0x00) | ((TS_PID_VID>>8)&0x1f);
        pkt[p++]=TS_PID_VID & 0xff;
        pkt[p++]=(af?0x30:0x10) | (m->cc_vid & 0x0f);
        m->cc_vid++;
        if(af){ pkt[p++]=afn; { int i; for(i=0;i<afn;i++) pkt[p++]=afc[i]; } }

        unsigned t = take;
        while(t){
            if(sent < (unsigned)hlen){ unsigned k=(unsigned)hlen-sent; if(k>t)k=t; memcpy(&pkt[p],peshdr+sent,k); p+=k; sent+=k; t-=k; }
            else { unsigned off=sent-(unsigned)hlen; unsigned k=len-off; if(k>t)k=t; memcpy(&pkt[p],au+off,k); p+=k; sent+=k; t-=k; }
        }
        /* p must be 188 now */
        first = 0;
    }
    if(!pts90) m->pts += m->pts_inc;   /* only when the caller isn't supplying a clock */
}

/* flush FIRST — ts_flush needs the fd, and the caller closes it right after us */
static void ts_close(tsmux *m){ ts_flush(m); m->started = 0; m->fd = -1; }

#endif
