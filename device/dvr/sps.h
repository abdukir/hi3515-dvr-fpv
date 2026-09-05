/* sps.h — patch an H.264 SPS to carry a display aspect ratio.
 *
 * WHY THIS EXISTS. The VENC emits an SPS with no VUI at all, so nothing in the
 * bitstream says what shape the picture is. Every player therefore assumes square
 * pixels and renders 704x240 as 2.93:1 — a letterbox slot. The real frame is a
 * full-width single field: 704x240 (NTSC) or 704x288 (PAL), both of which display
 * as 4:3. So every recording, and the live stream, came out badly stretched.
 *
 * There is no container-level fix: for H.264 the display aspect lives ONLY in the
 * SPS VUI. Hence a bit-level rewrite of the SPS as it goes past — insert
 * vui_parameters() carrying Extended_SAR with the sample aspect that makes the
 * coded size come out 4:3. 704x240 -> SAR 5:11, 704x288 -> SAR 6:11, computed from
 * the dimensions the SPS itself declares, so it stays right if the geometry changes.
 *
 * The algorithm was validated on a real recording before being written here: the
 * Python reference in the repo history rewrote a captured .ts and ffmpeg went from
 * "704x240" to "704x240 [SAR 5:11 DAR 4:3]" with a clean decode.
 *
 * Safety: every parse step is bounds-checked and any surprise (unexpected profile,
 * scaling lists, VUI already present, buffer too small) returns 0 = "leave it alone".
 * A stream that is merely mis-shaped must never become a stream that is broken.
 *
 * Requires oabi.h and memcpy from dvr.c.
 */
#ifndef DVR_SPS_H
#define DVR_SPS_H

#include "oabi.h"
void *memcpy(void *d, const void *s, size_t n);

#define SPS_MAXIN  64
#define SPS_MAXOUT 128

typedef struct { const unsigned char *d; unsigned nbits, p; int bad; } sps_br;

static unsigned sps_u1(sps_br *r){
    if(r->p >= r->nbits){ r->bad = 1; return 0; }
    unsigned b = (r->d[r->p>>3] >> (7 - (r->p & 7))) & 1u;
    r->p++;
    return b;
}
static unsigned sps_u(sps_br *r, int n){
    unsigned v = 0; int i;
    for(i=0;i<n;i++) v = (v<<1) | sps_u1(r);
    return v;
}
static unsigned sps_ue(sps_br *r){
    int lz = 0;
    while(!r->bad && sps_u1(r) == 0){
        if(++lz > 31){ r->bad = 1; return 0; }
    }
    if(r->bad) return 0;
    return ((1u<<lz) - 1u) + (lz ? sps_u(r, lz) : 0u);
}

typedef struct { unsigned char *d; unsigned cap, nbits; int bad; } sps_bw;

static void sps_w1(sps_bw *w, unsigned b){
    if((w->nbits>>3) >= w->cap){ w->bad = 1; return; }
    unsigned char *p = &w->d[w->nbits>>3];
    unsigned sh = 7 - (w->nbits & 7);
    if(sh == 7) *p = 0;                     /* starting a fresh byte */
    if(b) *p |= (unsigned char)(1u<<sh);
    w->nbits++;
}
static void sps_w(sps_bw *w, unsigned v, int n){
    int i; for(i=n-1;i>=0;i--) sps_w1(w, (v>>i)&1u);
}

static unsigned sps_gcd(unsigned a, unsigned b){
    while(b){ unsigned t = a % b; a = b; b = t; }
    return a ? a : 1;
}

/* Build an aspect-corrected copy of one SPS NAL (payload incl. the header byte, no
 * start code). Returns the new length, or 0 to mean "emit the original unchanged". */
static unsigned sps_fix(const unsigned char *nal, unsigned len,
                        unsigned char *out, unsigned outmax){
    unsigned char rb[SPS_MAXIN];            /* RBSP, emulation bytes removed */
    unsigned nrb = 0, i, zeros = 0;
    if(len < 5 || len > SPS_MAXIN || (nal[0] & 0x1f) != 7) return 0;

    for(i = 1; i < len; i++){               /* un-escape, skipping the NAL header */
        unsigned char b = nal[i];
        if(zeros >= 2 && b == 3){ zeros = 0; continue; }
        if(nrb >= SPS_MAXIN) return 0;
        rb[nrb++] = b;
        zeros = (b == 0) ? zeros + 1 : 0;
    }
    if(nrb < 4) return 0;

    sps_br r; r.d = rb; r.nbits = nrb * 8u; r.p = 0; r.bad = 0;
    unsigned prof = sps_u(&r, 8);
    sps_u(&r, 8);                            /* constraint flags + reserved */
    sps_u(&r, 8);                            /* level_idc */
    sps_ue(&r);                              /* seq_parameter_set_id */
    if(prof==100||prof==110||prof==122||prof==244||prof==44||prof==83||
       prof==86 ||prof==118||prof==128||prof==138||prof==139||prof==134||prof==135){
        unsigned cf = sps_ue(&r);
        if(cf == 3) sps_u1(&r);
        sps_ue(&r); sps_ue(&r); sps_u1(&r);
        if(sps_u1(&r)) return 0;             /* scaling lists: not worth parsing here */
    }
    sps_ue(&r);                              /* log2_max_frame_num_minus4 */
    unsigned poc = sps_ue(&r);
    if(poc == 0) sps_ue(&r);
    else if(poc == 1){
        sps_u1(&r); sps_ue(&r); sps_ue(&r);
        unsigned n = sps_ue(&r);
        if(n > 255) return 0;
        for(i=0;i<n;i++) sps_ue(&r);
    }
    sps_ue(&r);                              /* max_num_ref_frames */
    sps_u1(&r);                              /* gaps_in_frame_num_value_allowed */
    unsigned wmbs = sps_ue(&r) + 1;
    unsigned hmap = sps_ue(&r) + 1;
    unsigned fmo  = sps_u1(&r);
    if(!fmo) sps_u1(&r);                     /* mb_adaptive_frame_field_flag */
    sps_u1(&r);                              /* direct_8x8_inference_flag */
    if(sps_u1(&r)){ sps_ue(&r); sps_ue(&r); sps_ue(&r); sps_ue(&r); }   /* cropping */
    unsigned vui_at = r.p;                   /* the flag we are about to rewrite */
    unsigned had_vui = sps_u1(&r);
    if(r.bad || had_vui) return 0;           /* unparsable, or it already says something */

    unsigned w = wmbs * 16u;
    unsigned h = hmap * 16u * (fmo ? 1u : 2u);
    if(!w || !h) return 0;
    unsigned sw = 4u * h, sh = 3u * w;       /* SAR that makes w x h display as 4:3 */
    unsigned g = sps_gcd(sw, sh); sw /= g; sh /= g;
    if(!sw || !sh || sw > 0xffff || sh > 0xffff) return 0;

    unsigned char body[SPS_MAXOUT];
    sps_bw w2; w2.d = body; w2.cap = SPS_MAXOUT; w2.nbits = 0; w2.bad = 0;
    sps_br c; c.d = rb; c.nbits = nrb * 8u; c.p = 0; c.bad = 0;
    for(i = 0; i < vui_at; i++) sps_w1(&w2, sps_u1(&c));   /* everything before the flag */
    sps_w1(&w2, 1);                          /* vui_parameters_present_flag */
    sps_w1(&w2, 1);                          /* aspect_ratio_info_present_flag */
    sps_w(&w2, 255, 8);                      /* aspect_ratio_idc = Extended_SAR */
    sps_w(&w2, sw, 16);
    sps_w(&w2, sh, 16);
    /* The REST of vui_parameters(), all absent. There are exactly EIGHT of these:
     * overscan_info, video_signal_type, chroma_loc_info, timing_info,
     * nal_hrd_parameters, vcl_hrd_parameters, pic_struct, bitstream_restriction.
     * (No low_delay_hrd_flag, because it only exists when one of the hrd flags is set.)
     * Writing nine put an extra 0 in front of rbsp_stop_one_bit, so the stop bit read
     * back as 0 and the SPS was malformed. ffmpeg does not care -- it stops parsing
     * after bitstream_restriction_flag -- which is exactly why that slipped through
     * verification, while the DVR's hardware decoder rejected the stream outright and
     * played black. Count them if you ever touch this. */
    for(i = 0; i < 8; i++) sps_w1(&w2, 0);
    sps_w1(&w2, 1);                          /* rbsp_stop_one_bit (zero-padded by the writer) */
    if(w2.bad || c.bad) return 0;
    unsigned nbody = (w2.nbits + 7u) >> 3;

    unsigned o = 0;                          /* re-escape into the caller's buffer */
    if(outmax < 2) return 0;
    out[o++] = nal[0];
    zeros = 0;
    for(i = 0; i < nbody; i++){
        if(zeros >= 2 && body[i] <= 3){
            if(o >= outmax) return 0;
            out[o++] = 3; zeros = 0;
        }
        if(o >= outmax) return 0;
        out[o++] = body[i];
        zeros = (body[i] == 0) ? zeros + 1 : 0;
    }
    return o;
}

#endif
