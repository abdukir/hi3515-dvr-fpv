/* ts_demux.h — demux our own MPEG-TS recordings back into H.264 access units for
 * playback (VDEC). Matches ts.h: video PES on PID 0x0100 (stream_id 0xE0), one Annex-B
 * access unit per PES (PUSI marks the start), 33-bit PTS @ 90kHz. 188-byte packets.
 * Streams the file one packet at a time (no full-file load). Requires oabi.h.
 */
#ifndef DVR_TS_DEMUX_H
#define DVR_TS_DEMUX_H

#include "oabi.h"
#ifndef TS_PID_VID
#define TS_PID_VID 0x0100
#endif
void *memcpy(void *d, const void *s, size_t n);

typedef struct {
    int fd;
    unsigned char pkt[188];
    unsigned char *au; unsigned au_cap, au_len;      /* the AU being returned */
    unsigned char carry[200]; int carry_len;         /* start of the NEXT AU (from its PUSI pkt) */
    unsigned long long pts, next_pts;
    int started;                                     /* seen the first PES */
    int eof;
    long last_pkt_off;   /* file offset of the packet currently in d->pkt */
    long cur_off;        /* file offset of the AU returned by the last tsdemux_next (its 1st packet) */
    long pend_off;       /* file offset where the NEXT AU starts (its PUSI packet) */
} tsdemux;

static int tsdemux_open(tsdemux *d, const char *path, unsigned char *aubuf, unsigned aucap){
    int i; unsigned char *p=(unsigned char*)d;
    for(i=0;i<(int)sizeof(*d);i++) p[i]=0;
    d->au=aubuf; d->au_cap=aucap;
    d->fd=(int)sys_open(path, O_RDONLY, 0);
    return d->fd>=0 ? 0 : -1;
}
static void tsdemux_close(tsdemux *d){ if(d->fd>=0){ sys_close(d->fd); d->fd=-1; } }

/* seek to a byte offset aligned to a TS packet (approx seek: caller passes bytes) */
static void tsdemux_seek(tsdemux *d, long off){
    off -= off % 188;
    sys_lseek(d->fd, off, 0);
    d->au_len=0; d->carry_len=0; d->started=0; d->eof=0;
    d->cur_off=off; d->pend_off=off;
}

static int tsd_read_pkt(tsdemux *d){
    int got=0;
    d->last_pkt_off = sys_lseek(d->fd, 0, 1);/* offset of this packet (before we read it) */
    while(got<188){
        long n=sys_read(d->fd, d->pkt+got, 188-got);
        if(n<=0){ d->eof=1; return 0; }
        got+=(int)n;
    }
    return 1;
}
static void tsd_append(tsdemux *d, const unsigned char *b, int n){
    if(n<=0) return;
    if(d->au_len + (unsigned)n > d->au_cap) n=(int)(d->au_cap - d->au_len);
    if(n<=0) return;
    memcpy(d->au + d->au_len, b, (size_t)n); d->au_len += (unsigned)n;
}

/* Fill d->au with the next complete access unit; return its length (0 at EOF).
 * d->pts holds its 90kHz PTS. */
static unsigned tsdemux_next(tsdemux *d){
    /* start the new AU with whatever we carried from the completing packet */
    d->au_len=0; d->pts=d->next_pts; d->cur_off=d->pend_off;
    if(d->carry_len>0){ tsd_append(d, d->carry, d->carry_len); d->carry_len=0; }
    for(;;){
        if(!tsd_read_pkt(d)){
            if(d->started && d->au_len>0){ d->started=0; return d->au_len; }  /* flush last */
            return 0;
        }
        if(d->pkt[0]!=0x47) continue;                       /* lost sync — skip */
        int pid=((d->pkt[1]&0x1f)<<8)|d->pkt[2];
        if(pid!=TS_PID_VID) continue;
        int pusi=d->pkt[1]&0x40;
        int afc=(d->pkt[3]>>4)&3;                           /* 1=payload 2=adapt 3=both */
        int off=4;
        if(afc==2||afc==3){ int al=d->pkt[4]; off=5+al; }
        if(afc==2 || off>=188) continue;                    /* no payload */
        unsigned char *pay=d->pkt+off; int paylen=188-off;
        if(pusi){
            /* strip the PES header, extract PTS, find the H.264 start */
            int ph=0; unsigned long long pts=d->pts;
            if(paylen>=9 && pay[0]==0 && pay[1]==0 && pay[2]==1){
                if(pay[7]&0x80){                            /* PTS present */
                    pts = ((unsigned long long)(pay[9]&0x0e)<<29)
                        | ((unsigned long long)pay[10]<<22) | ((unsigned long long)(pay[11]&0xfe)<<14)
                        | ((unsigned long long)pay[12]<<7)  | ((unsigned long long)(pay[13]>>1));
                }
                ph=9+pay[8];
            }
            if(d->started && d->au_len>0){
                /* previous AU is complete — stash this PES's start, return the old AU */
                int c=paylen-ph; if(c>(int)sizeof(d->carry)) c=(int)sizeof(d->carry);
                if(c>0) memcpy(d->carry, pay+ph, (size_t)c);
                d->carry_len=c; d->next_pts=pts; d->started=1;
                d->pend_off=d->last_pkt_off;             /* next AU starts at this packet */
                return d->au_len;
            }
            d->started=1; d->pts=pts; d->next_pts=pts;
            d->cur_off=d->last_pkt_off;                  /* this (first) AU starts at this packet */
            tsd_append(d, pay+ph, paylen-ph);
        } else if(d->started){
            tsd_append(d, pay, paylen);
        }
    }
}

#endif
