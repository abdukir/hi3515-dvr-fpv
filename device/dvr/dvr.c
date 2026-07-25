/* dvr.c — our own DVR recorder for the RR104P / Hi3520_MPP_V3.0.6.2.
 *
 * M0 (headless recorder): a RAW-IOCTL replay of docs/MPP_INIT_SEQUENCE.md — no SDK,
 * no libc. Because the only findable SDK (V1.0.0.0) mismatches the device's V3.0.6.2
 * (VI_SetPubAttr -> SYS_NOTREADY), we replay app.out's real, traced ioctl sequence
 * (dump/livedump_20260723/mpp_init_trace.log) byte-for-byte. We OWN the VENC channels,
 * so we can read our own encoder frames (the MMZ-isolation wall only blocks foreigners).
 *
 * Pipeline: capture chip (tl_R9508 + tw_286x) -> SYS+VB -> VO -> VI x4 -> GRP x4 ->
 * VENC x4 (create -> get buffer -> mmap -> bind) -> per-channel GetStream/Release loop
 * writing Annex-B .h264 to /root/rec/aN/.
 *
 * IMPORTANT: single MPP owner — STOP app.out first (serial: `closewd` then `exit`,
 * kill mydaemon.out). Watchdog is taken over here and disarmed on every exit path.
 *
 * This single file is the M0 first-light form; once video-to-disk is proven it splits
 * into mpp.c / rec.c / net.c / ui.c per docs/DVR_DESIGN.md.
 *
 * Build (WSL, from ext4): device/dvr/build.sh   Run: ./dvr [seconds] (0 = ~forever)
 */
#include "oabi.h"
#include "net.h"
#include "rtc.h"
#include "ts.h"
#include "ts_demux.h"
#include "fb.h"
#include "ui.h"
#include "mouse.h"
#include "keyboard.h"

/* Freestanding memset/memcpy — the compiler may also emit implicit calls to these.
 *
 * These are NOT hot-path-irrelevant. The recorder copies ~96 KB per frame on the
 * encoder-drain path, and a byte-at-a-time loop costs roughly 5 cycles/byte on this
 * ARM926: milliseconds per frame out of a 33 ms budget, which showed up as dropped
 * frames (docs/REVIEW.md R10). Move 32 bits at a time whenever alignment allows.
 * ARM926 cannot do unaligned word access, so the word path requires src and dst to
 * share alignment; otherwise fall back to bytes. */
void *memset(void *d, int c, size_t n){
    unsigned char *p=d; unsigned char v=(unsigned char)c;
    if(n >= 16){
        unsigned long w = (unsigned long)v; w |= w<<8; w |= w<<16;
        while(((unsigned long)p & 3) && n){ *p++=v; n--; }
        { unsigned long *q=(unsigned long*)p;
          while(n >= 16){ q[0]=w; q[1]=w; q[2]=w; q[3]=w; q+=4; n-=16; }
          while(n >= 4){ *q++=w; n-=4; }
          p=(unsigned char*)q; }
    }
    while(n--) *p++=v;
    return d;
}
void *memcpy(void *d, const void *s, size_t n){
    unsigned char *a=d; const unsigned char *b=s;
    if(n >= 16 && ((((unsigned long)a ^ (unsigned long)b) & 3) == 0)){
        while(((unsigned long)a & 3) && n){ *a++=*b++; n--; }
        { unsigned long *p=(unsigned long*)a; const unsigned long *q=(const unsigned long*)b;
          while(n >= 16){ p[0]=q[0]; p[1]=q[1]; p[2]=q[2]; p[3]=q[3]; p+=4; q+=4; n-=16; }
          while(n >= 4){ *p++=*q++; n-=4; }
          a=(unsigned char*)p; b=(const unsigned char*)q; }
    }
    while(n--) *a++=*b++;
    return d;
}
/* libgcc's integer-division helpers reference raise() on divide-by-zero; we never
 * divide by zero (constants), but the symbol must resolve under -nostdlib. */
int raise(int sig){ (void)sig; return 0; }

#define NCH 4
/* bumped whenever the control protocol grows a command — the web UI reads it from INFO
 * so it can light up (or hide) features without guessing what firmware is running. */
#define DVR_VER 2
static long g_start_sec = 0;   /* now_sec() at startup, for INFO's uptime */

/* ---- ioctl helpers ---- */
static int ioc(int fd, unsigned long cmd, void *arg){ return (int)sys_ioctl(fd, cmd, arg); }
/* by-value ioctl arg (e.g. tl_R9508 0xc00456d3 gets 0x64, VI EnableDev/Chn) */
static int iocv(int fd, unsigned long cmd, unsigned long val){ return (int)sys_ioctl(fd, cmd, (void*)val); }
/* ioctl with a single u32 arg passed by pointer */
static int ioc1(int fd, unsigned long cmd, unsigned v){ unsigned x=v; return (int)sys_ioctl(fd, cmd, &x); }

static void die(const char *msg, int rc){ puts_(msg); puts_(" rc="); putu((unsigned)rc); puts_("\n"); }

/* ================= exact struct bytes from the trace ================= */

/* VB pool config, 68 B (ioctl 0x4044420a): 0x78; blkSz 0x97e00 x8; blkSz2 0x25f80 x0x50; rest 0 */
static unsigned char VBCONF[68] = {
  0x78,0x00,0x00,0x00, 0x00,0x7e,0x09,0x00, 0x08,0x00,0x00,0x00,
  0x80,0x5f,0x02,0x00, 0x50,0x00,0x00,0x00
};
/* SYS config align=0x10, 8 B (ioctl 0x40085902) */
static unsigned char SYSCFG[8] = { 0x10,0x00,0x00,0x00, 0,0,0,0 };

/* VO setup */
static unsigned char VO_53[4]  = { 0x00,0x02,0x00,0x00 };
static unsigned char VO_02[56] = { 0x03,0,0,0, 0,0,0,0, 0x01,0,0,0 };
static unsigned char VO_0d[36] = { 0,0,0,0, 0,0,0,0, 0xd0,0x02,0,0, 0xe0,0x01,0,0,
                                   0xd0,0x02,0,0, 0xe0,0x01,0,0, 0x1e,0,0,0, 0x13,0,0,0,
                                   0xff,0xff,0xff,0xff };

/* VI PubAttr, 20 B (ioctl 0x40144902): workmode=3(4D1), norm=1(NTSC) */
static unsigned char VI_PUB[20] = { 0,0,0,0, 0x03,0,0,0, 0x01,0,0,0, 0,0,0,0, 0,0,0,0 };
/* VI ChnAttr, 36 B (ioctl 0x40244908): stCapRect{x,y,w=0x2c0(704),h}, enCapSel@16,
 * bDownScale@20, bChromaResample@24, bHighPri@28, enViPixFormat@32.
 * Capture full-width 704, single-field height 240, bDownScale=0 -> 704x240 (2CIF). The VIU caps
 * at NTSC's 30 frame/s: enCapSel=BOTH(2) still yields only 30fps (it can't emit both fields as
 * separate temporal frames), and BOTH-weave to a progressive 480 frame yields no frames at all.
 * So enCapSel=BOTTOM(1), 30fps is the ceiling. */
static unsigned char VI_CHN[36] = { 0,0,0,0, 0,0,0,0, 0xc0,0x02,0,0, 0xf0,0,0,0,
                                    0x01,0,0,0, 0x00,0,0,0, 0,0,0,0, 0,0,0,0, 0x13,0,0,0 };

/* VENC CreateChn attr, 104 B (ioctl 0x40684500): PT_H264(0x60), w=0x160(352) h=0xf0(240)
 * fps=0x1e(30), bMain=1, bufSize=0x1ef00 ... (verbatim from trace, same for all chn) */
static unsigned char VENC_ATTR[104] = {
  0x60,0,0,0, 0,0,0,0, 0x60,0x01,0,0, 0xf0,0,0,0,
  0x1e,0,0,0, 0x01,0,0,0, 0,0,0,0, 0x00,0xef,0x01,0,
  0x01,0,0,0, 0,0,0,0, 0x1e,0,0,0, 0x28,0,0,0,
  0x0a,0,0,0, 0,0,0,0, 0x00,0x02,0,0, 0x03,0,0,0
  /* remaining 40 bytes are zero */
};
/* VENC GetBufInfo input, 12 B (ioctl 0x800c450f): "/grp\0..." (OUT: {phys, phys2, size}) */
static unsigned char VENC_BUFQ[12] = { '/','g','r','p',0,0,0,0, 0,0,0,0 };
/* VENC SetChnAttr / RC, 76 B (ioctl 0x404c4506): needed before StartRecvPic for
 * the encoder to emit. Verbatim from trace (same for all chn). */
static unsigned char VENC_RC[76] = {
  0x60,0,0,0, 0,0,0,0, 0x60,0x01,0,0, 0xf0,0,0,0,
  0x1e,0,0,0, 0x01,0,0,0, 0,0,0,0, 0x00,0xef,0x01,0,
  0x01,0,0,0, 0,0,0,0, 0x1e,0,0,0, 0x3c,0,0,0,
  0x0a,0,0,0, 0,0,0,0, 0x00,0x08,0,0, 0,0,0,0,
  0,0,0,0, 0,0,0,0, 0,0,0,0
};
/* VENC bind, 16 B (ioctl 0xc0104517) */
static unsigned char VENC_BIND[16] = { 0xff,0xff,0xff,0xff, 0xff,0xff,0xff,0xff,
                                       0x01,0,0,0, 0x01,0,0,0 };
static unsigned char VENC_BIND_Q[16] = { 0 };  /* 0xc0104518 query, IN zero */

/* --- VGA display (VO dev 1), verbatim from trace fd=23 --- */
/* VO_PUB_ATTR 56B (0x40384f02): {bg?, enIntfType=2 VGA, enIntfSync=9 1280x1024@60, ...} */
static unsigned char VGA_PUB[56] = { 0x03,0,0,0, 0x02,0,0,0, 0x09,0,0,0 };
/* video-layer attr 36B (0x40244f0d): canvas 1280x1024, disp area 720x480, 30/19, -1 */
static unsigned char VGA_LAYER[36] = { 0,0,0,0, 0,0,0,0, 0x00,0x05,0,0, 0x00,0x04,0,0,
                                       0xd0,0x02,0,0, 0xe0,0x01,0,0, 0x1e,0,0,0, 0x13,0,0,0,
                                       0xff,0xff,0xff,0xff };

/* ================================================================== */

static int g_wdt = -1;
static int g_dump = 0;
static int g_ftw = -1;   /* /dev/tw_286x fd (global for the REG debug command) */
static int g_fvo = -1;   /* VGA VO fd (global for the SHOT screenshot command) */
static unsigned g_vifps = 0x3c;   /* VI capture framerate (60 = smoother; low-latency default) */
static unsigned g_vosync = 9;     /* VO output mode: 9=1280x1024, 2=720p, 6=1080p30, 10=1366x768, 11=1440x900 */
static unsigned g_votol = 0;      /* VO play toleration ms (0=leave default; 1..N lower=less buffering) */
static int      g_display = 1;    /* VGA fit: 0=fill(stretch) 1=pillarbox(aspect-correct, black bars) */
static int      g_dispch = 0;     /* channel shown on VGA (for the OSD HUD) */
static int      g_fvi[NCH];       /* per-channel /dev/vi fds (for the playback VO swap) */
static int      g_pb_stop = 0;    /* set to abort an in-progress playback */
static int      g_pb_active = 0;  /* 1 while a recording is playing (VDEC->VO) */
static int      g_pb_paused = 0;  /* 1 = feed halted, last frame latched on screen */
static int      g_pb_step = 0;    /* set to advance exactly one frame while paused */
static int      g_pb_back = 0;    /* set to step back one frame (re-decode from a keyframe) */
static int      g_pb_seek = 0;    /* pending seek request (scrub) */
static int      g_pb_seek_pm = 0; /* seek target, permille of the file (0..1000) */
static int      g_pb_scrub = 0;   /* 1 while dragging the progress bar (knob highlight) */
static int      g_pb_speed = 4;   /* playback speed: 4 = 1x, 2 = 1/2x, 1 = 1/4x (slow-mo) */
static long     g_pb_pos = 0, g_pb_total = 0;  /* byte position / size (progress bar) */
static long     g_pb_frame = 0;   /* frames fed/displayed so far (1-based current frame) */
static int      g_pb_ended = 0;   /* reached end of file — hold here, don't auto-exit to live */
static int      g_pb_replay = 0;  /* restart from the beginning */
static int      g_pb_sel = 2;     /* highlighted control button for -/+/M/X nav (0..PBC_N-1) */
static long     g_pb_cur_ms = 0, g_pb_tot_ms = 0;   /* elapsed / (estimated) total, ms */
static char     g_pb_title[24];   /* "MM/DD HH:MM CHn" of the playing clip */
static long     g_pb_lastui = 0;  /* now_ms() of the last control interaction (bar auto-hide) */
/* ring of recent keyframes {frame#, file offset} so back-step can seek to a decodable point */
#define KFRING 24
static long     g_kf_no[KFRING], g_kf_off[KFRING]; static int g_kf_cnt=0;
static int play_file(const char *path);   /* defined after ctl_poll */
static void pb_bar_draw(int full);         /* playback control bar (defined before play_file) */
static void thumb_rebind(void);            /* re-point g_pb_thumb[] after list_recordings (thumbs) */
static void thumb_teardown(void);          /* release the thumbnail-gen VDEC/VO if held */
static void ui_paint(void);                /* full OSD render + blit (defined below) */
static int  g_egg_on;                      /* easter egg running — suppresses OSD repaints */
static void beep(const unsigned char *seq, int n);  /* buzzer pattern (defined below) */
typedef struct { unsigned short hz, ms; } note_t;   /* melody note (see below) */
static void melody(const note_t *m, int n);
extern const note_t MEL_REC_START[], MEL_REC_STOP[];
#define MEL_REC_START_N 4
#define MEL_REC_STOP_N  3
extern const note_t MEL_NAV[], MEL_ENTER[], MEL_BACK[], MEL_EDIT[];
#define MEL_NAV_N 1
#define MEL_ENTER_N 2
#define MEL_BACK_N 2
#define MEL_EDIT_N 1
/* dvr.conf `ui_sound=0` (or the SND command) silences menu feedback */
static int g_ui_snd = 1;
static void ui_snd(char k);
static int  pump_encode(void);             /* encoder drain (defined below) */
static int      g_restart = 0;    /* UI "Reboot" -> restart our program (wrapper respawns) */
static ui_state g_ui;             /* on-screen UI model (ui.h) */
static int      g_ui_fb = 0;      /* 1 once /dev/fb0 opened for the OSD */
static void put32(unsigned char *p, int off, unsigned v);   /* defined below (config section) */
static void wdt_off(void){ if(g_wdt>=0){ sys_write(g_wdt,"V",1); sys_close(g_wdt); g_wdt=-1; } }
static void wpet(void){ if(g_wdt>=0) sys_write(g_wdt,"w",1); }   /* pet during init too */

/* Bring up the VGA output (VO dev 1) showing one VI channel fullscreen.
 * First attempt: VO-only (no VPP) relying on the driver's by-index VI->VO routing.
 * Verbatim VGA pub/layer attrs from the trace; channel window sized to the display area.
 * Returns the VO fd (kept open) or -1. Experimental — iterate on the real monitor. */
static int display_vga(int vichan){
    /* VGA output raster size for the selected mode (must match osd_wh + the VO_INTF_SYNC enum).
     * Higher-refresh modes (12=800x600@75, 13=1024x768@75) cut vsync/scan latency vs @60. */
    unsigned ow = 0x500, oh = 0x400;                     /* default 1280x1024@60 */
    if(g_vosync==2){ ow=0x500; oh=0x2d0; }               /* 720p 1280x720 */
    else if(g_vosync==6){ ow=0x780; oh=0x438; }          /* 1080p 1920x1080 */
    else if(g_vosync==7 || g_vosync==12){ ow=0x320; oh=0x258; }   /* 800x600  @60/@75 */
    else if(g_vosync==8 || g_vosync==13){ ow=0x400; oh=0x300; }   /* 1024x768 @60/@75 */
    else if(g_vosync==10){ ow=0x556; oh=0x300; }         /* 1366x768 */
    else if(g_vosync==11){ ow=0x5a0; oh=0x384; }         /* 1440x900 */
    VGA_PUB[8] = (unsigned char)g_vosync;                /* enIntfSync */
    VGA_PUB[0] = 0;                                       /* black background (pillarbox bars) */

    /* video layer display rectangle on the output raster: fill or aspect-correct pillarbox.
     * Source is ~4:3 (D1). Pillarbox fits 4:3 centered; the rest shows the black bg. */
    unsigned dx=0, dy=0, dw=ow, dh=oh;
    if(g_display==1){
        if(ow*3 > oh*4){ dh=oh; dw=oh*4/3; dx=(ow-dw)/2; dy=0; }   /* wide -> fit height */
        else           { dw=ow; dh=ow*3/4; dx=0; dy=(oh-dh)/2; }   /* tall -> fit width  */
    }
    put32(VGA_LAYER, 0, dx); put32(VGA_LAYER, 4, dy);
    put32(VGA_LAYER, 8, dw); put32(VGA_LAYER, 12, dh);
    /* imageSize (source) at offset 16/20 is 720 x (480 NTSC / 576 PAL) — set by apply_standard */
    unsigned src_h = (unsigned)VGA_LAYER[20] | ((unsigned)VGA_LAYER[21]<<8);
    if(src_h==0) src_h=480;

    int fvo = (int)sys_open("/dev/vo", O_RDWR, 0);      /* 2nd /dev/vo open -> dev 1 (VGA) */
    if(fvo < 0){ die("[dvr] VGA /dev/vo open failed", fvo); return -1; }
    ioc1(fvo, 0x40044f53, (unsigned)vichan);             /* select VO channel = the VI channel to show */
    iocv(fvo, 0x4f01, 0);
    ioc(fvo, 0x40384f02, VGA_PUB);                       /* SetPubAttr: VGA, mode=g_vosync, black bg */
    int re = iocv(fvo, 0x4f00, 0);                       /* EnableDev */
    ioc(fvo, 0x40244f0d, VGA_LAYER);                     /* video layer (dispRect = fill/pillarbox) */
    iocv(fvo, 0x4f0b, 0);                                /* enable layer */
    if(g_votol > 0){ unsigned t=g_votol; ioc(fvo, 0x40044f30, &t); }  /* SetPlayToleration (low lag) */
    /* channel window (VO_CHN_ATTR_S 28B): {priority, x, y, w, h, zoom, deflicker} — full source */
    unsigned ch[7]; ch[0]=1; ch[1]=0; ch[2]=0; ch[3]=0x2d0; ch[4]=src_h; ch[5]=1; ch[6]=0;
    iocv(fvo, 0x4f1e, 0);                                /* disable first: un-latch any stuck (VDEC) source */
    ioc(fvo, 0x401c4f1f, ch);
    iocv(fvo, 0x4f1d, 0);                                /* enable channel */
    /* VD output mux (from trace): {4,0} then {1,2} */
    int fvd = (int)sys_open("/dev/vd", O_RDWR, 0);
    if(fvd >= 0){ unsigned m[2]; m[0]=4; m[1]=0; ioc(fvd,0x40085705,m); m[0]=1; m[1]=2; ioc(fvd,0x40085705,m); }
    puts_("[dvr] VGA display: rc="); putu((unsigned)re);
    puts_(" ch"); putu((unsigned)vichan);
    puts_(g_display==1?" pillarbox\n":" fill\n");
    return fvo;
}
static void vo_relive(int vichn);     /* (re)source VGA from live VI — defined after g_standard */
static void ensure_live(int vichn);   /* verify the live VGA is moving, re-cycle until it is */

/* ---------- live H.264 TCP streaming (M2) ----------
 * Protocol (docs/PROTOCOL2.md): client connects to STREAM_PORT, sends 1 byte = channel
 * (0..3, ASCII '0'..'3' or raw 0..3). Server then streams that channel's raw Annex-B
 * H.264, starting at the next keyframe (SPS). Non-blocking + drop-on-slow so recording
 * latency is never held hostage to a slow viewer. */
#define STREAM_PORT 8091
#define MAXCLI 12    /* headroom for a 4-ch grid × a couple of browsers */
static int g_lfd = -1;
static int g_cfd[MAXCLI];   /* client fd, -1 = free */
static int g_cch[MAXCLI];   /* subscribed channel */
static int g_cst[MAXCLI];   /* 0 = waiting for keyframe, 1 = streaming */

static void net_init(void){
    int i; for(i=0;i<MAXCLI;i++){ g_cfd[i]=-1; g_cch[i]=0; g_cst[i]=0; }
    g_lfd = net_listen(STREAM_PORT);
    puts_("[dvr] stream server on :"); putu(STREAM_PORT);
    puts_(g_lfd>=0 ? " up\n" : " FAILED\n");
}
/* accept any pending clients; read the 1-byte channel subscription (bounded, non-block) */
static void net_poll(void){
    int nc, i, slot;
    if(g_lfd < 0) return;
    nc = net_accept(g_lfd);
    if(nc < 0) return;                       /* -EAGAIN: nobody waiting */
    net_nonblock(nc);
    /* read channel byte, up to ~100ms */
    { unsigned char b; int got=-1, tries;
      for(tries=0; tries<20; tries++){ if(sys_read(nc,&b,1)==1){ got=b; break; } msleep(5); }
      slot=-1; for(i=0;i<MAXCLI;i++){ if(g_cfd[i]<0){ slot=i; break; } }
      if(slot<0 || got<0){ sys_close(nc); return; }
      { int chq = (got>='0'&&got<='3')?(got-'0'):(got&0x3);
        g_cfd[slot]=nc; g_cch[slot]=chq; g_cst[slot]=0; }
      puts_("[dvr] stream client slot="); putu((unsigned)slot);
      puts_(" ch="); putu((unsigned)g_cch[slot]); puts_("\n");
    }
}
/* send a full frame to one client, bounded retry; return 0 ok, -1 drop client.
 * Uses net_send (MSG_NOSIGNAL) so a disconnected peer returns -EPIPE, never SIGPIPE. */
static int cli_send(int fd, const unsigned char *buf, unsigned len){
    unsigned off=0; int tries=0;
    while(off<len){
        long n = net_send(fd, buf+off, len-off);
        if(n>0){ off+=(unsigned)n; tries=0; }
        else if(n==-11){ if(++tries>3) return -1; msleep(2); }  /* EAGAIN: slow client, brief retry */
        else return -1;                                          /* EPIPE/other: drop client */
    }
    return 0;
}
/* fan one channel's Annex-B frame out to subscribed clients. is_key = batch starts at SPS. */
static void net_fanout(int chn, const unsigned char *buf, unsigned len, int is_key){
    int i;
    for(i=0;i<MAXCLI;i++){
        if(g_cfd[i]<0 || g_cch[i]!=chn) continue;
        if(g_cst[i]==0){ if(is_key) g_cst[i]=1; else continue; }  /* wait for keyframe */
        if(cli_send(g_cfd[i], buf, len) < 0){ sys_close(g_cfd[i]); g_cfd[i]=-1; }
    }
}
static void net_shutdown(void){
    int i; for(i=0;i<MAXCLI;i++){ if(g_cfd[i]>=0){ sys_close(g_cfd[i]); g_cfd[i]=-1; } }
    if(g_lfd>=0){ sys_close(g_lfd); g_lfd=-1; }
}

/* ---------- encoder config + manual TS recording ---------- */
/* configurable encoder params. Default = 2CIF (704x240 NTSC / 704x288 PAL, full-width single
 * field — the max this VIU captures; apply_standard sets the height) at 4 Mbps for maximum
 * recording quality. On playback the 240 field is scaled to 480 (4:3). Overridable via dvr.conf. */
/* Rate control is VBR quality-0 (best): the encoder holds top quality and lets the bitrate float
 * UP TO g_bitrate (kbps cap), spending bits on detail/motion. FPV flight fills it; a static scene
 * uses far less (no wasted space). Default cap 12 Mbps — high headroom for 704x240 so quality is
 * never bitrate-limited. Raise via dvr.conf `bitrate=` (u32 kbps, no hw limit; SATA sustains it
 * easily). CBR (constant fill) would need the different HiSilicon CBR RC struct — not worth it for
 * FPV since it only pads static frames without adding quality. FIXQP (rc_mode=3, `qp`=const QP,
 * lower=more bits/higher quality) removes the quality ceiling — bitrate floats freely, best for
 * squeezing maximum data out of high-motion footage. */
/* GOP = 15 (keyframe every 0.5s): halves the backward-step re-decode "flash" (H.264 must replay
 * from the previous keyframe to show a frame) and doubles scrub precision, for ~7% more file size.
 * dvr.conf `gop=30` (or higher) trades that back for smaller files. */
static unsigned g_fps = 30, g_gop = 15, g_bitrate = 12288, g_ewidth = 704, g_eheight = 240;
/* Default RC = FIXQP QP16: constant near-transparent quality, bitrate floats freely (no cap) so
 * high-motion FPV gets all the data it needs — ~16 Mbps here, 20+ on motion (measured 0 errors to
 * 23 Mbps @ QP10). dvr.conf `qp=` lower (12/10) = even more; `rc_mode=0` = VBR quality-0 (self-
 * capped at `bitrate`, smaller files when motion is low). */
static unsigned g_rc_mode = 3;    /* 0=VBR(quality-0,cap), 3=FIXQP(const qp). enRcMode @ attr+52 */
static unsigned g_qp = 10;        /* FIXQP QP [10..50] lower=more bits. QpI @ +64, QpP @ +68.
                                   * QP10 = max quality (~23 Mbps here); a keyframe can't exceed the
                                   * raw frame (253KB) so the 1.8MB ring holds it safely. */
/* how many channels to ENCODE/record (VI+display stay all-4 for channel switching). This is an
 * FPV recorder with one camera, and full-width 704 capture can't sustain 4 channels — so default
 * to encoding just ch0 (the camera), giving it all the bandwidth for max resolution. dvr.conf
 * `enc_channels=4` restores full multi-channel recording (at reduced per-channel resolution). */
static unsigned g_enc_nch = 1;
static void patch_venc_attrs(void);   /* fwd: apply_standard re-derives size-dependent attrs */
static int g_standard = 1;   /* 1=NTSC 2=PAL 0=auto-detect (config `standard=`, STD cmd) */
static int g_std_changed = 0;/* STD command sets this; applied on next restart */
static unsigned g_min_free_mb = 300;    /* auto-stop recording if free space drops below */
static unsigned g_max_rec_mb  = 2000;   /* cap one recording file (vfat 4GB limit safety) */

static void put32(unsigned char *p, int off, unsigned v){
    p[off]=v&0xff; p[off+1]=(v>>8)&0xff; p[off+2]=(v>>16)&0xff; p[off+3]=(v>>24)&0xff;
}
/* two-hex-digit parser for GPIOSET block ids (15,16,17,18,1a...) */
static int k_atoi_hex(const char *s){ int v=0; if(!s) return 0;
    while(*s){ char c=*s; int d;
        if(c>=(char)0x30 && c<=(char)0x39) d=c-0x30;
        else if(c>=(char)0x61 && c<=(char)0x66) d=c-0x61+10;
        else if(c>=(char)0x41 && c<=(char)0x46) d=c-0x41+10;
        else break;
        v=v*16+d; s++; }
    return v; }
static int streq(const char *a, const char *b){ while(*a&&*b){ if(*a!=*b) return 0; a++;b++; } return *a==*b; }
static int u2s(char *b, unsigned v){ char t[12]; int i=0,l=0; if(!v){b[0]='0';return 1;} while(v){t[i++]="0123456789"[v%10];v/=10;} while(i)b[l++]=t[--i]; return l; }

/* Force the VGA VO to (re)source from the live VI: cycle the VO channel disable->SetChnAttr->enable
 * on BOTH VO devs (2=mirror, 0=VGA), THEN bind the VI to both. Exact sequence the playback
 * return-to-live path uses and that reliably un-freezes the display; also used on startup because a
 * respawn inherits the previous instance's VO state and a plain enable is a no-op on an already-
 * enabled channel (so the live view would stay frozen until a playback cycled it). */
static void vo_relive(int vichn){
    if(vichn<0 || vichn>=NCH) return;
    unsigned cattr[7]; cattr[0]=1; cattr[1]=0; cattr[2]=0;
    cattr[3]=0x2d0; cattr[4]=(g_standard==2)?576:480; cattr[5]=1; cattr[6]=0;
    int vd;
    vd=(int)sys_open("/dev/vo",O_RDWR,0);
    if(vd>=0){ ioc1(vd,0x40044f53,(unsigned)vichn|(2u<<8)); iocv(vd,0x4f1e,0); ioc(vd,0x401c4f1f,cattr); iocv(vd,0x4f1d,0); sys_close(vd); }
    vd=(int)sys_open("/dev/vo",O_RDWR,0);
    if(vd>=0){ ioc1(vd,0x40044f53,(unsigned)vichn|(0u<<8)); iocv(vd,0x4f1e,0); ioc(vd,0x401c4f1f,cattr); iocv(vd,0x4f1d,0); sys_close(vd); }
    if(g_fvi[vichn]>=0){ unsigned vb[2];
        vb[0]=2; vb[1]=(unsigned)vichn; ioc(g_fvi[vichn],0x40084918,vb);
        vb[0]=0; vb[1]=(unsigned)vichn; ioc(g_fvi[vichn],0x40084918,vb); }
}
/* scene-independent "is the VO advancing" signature from the GetScreenFrame info: the displayed
 * buffer phyaddr (f[4]/f[5]) and the frame timestamp (f[15]) both change every frame when the VO is
 * live, and are CONSTANT when it's latched on a stale frame. (Pixel content is unreliable — a dark
 * static scene looks identical frame-to-frame even when live.) Two calls differ => live. 0 = unreadable. */
static unsigned vo_frame_sig(void){
    if(g_fvo<0) return 0;
    unsigned f[40]; memset(f,0,sizeof(f));
    if(ioc(g_fvo,0xc0984f0f,f)!=0) return 0;                 /* GetScreenFrame */
    unsigned sig = f[4] ^ (f[5]<<1) ^ f[15] ^ (f[15]>>16);   /* buffer addr + frame timestamp */
    ioc(g_fvo,0x40984f10,f);                                 /* ReleaseScreenFrame */
    if(!sig) sig=1;                                          /* keep 0 reserved for "unreadable" */
    return sig;
}
/* Stronger version of vo_relive for the freeze watchdog: explicitly UNBIND the VI from both
 * VO devs before re-sourcing. vo_relive() only ever binds, and a bind on an already-bound
 * channel is a no-op — which is exactly the state a latched display is in, so the gentle
 * version cannot break it out. play_file() proves the unbind direction works (it takes the
 * VO away from VI for VDEC), so this is the symmetric move. Kept separate from vo_relive()
 * because the playback-return path depends on the gentle sequence and is known good. */
static void vo_force_relive(int vichn){
    if(vichn<0 || vichn>=NCH) return;
    if(g_fvi[vichn]>=0){ unsigned vb[2];
        vb[0]=0; vb[1]=(unsigned)vichn; ioc(g_fvi[vichn],0x40084919,vb);   /* unbind dev0 */
        vb[0]=2; vb[1]=(unsigned)vichn; ioc(g_fvi[vichn],0x40084919,vb); } /* unbind dev2 */
    msleep(40);
    vo_relive(vichn);                                                      /* re-source + bind */
}
/* make sure the live VGA is actually updating (not latched on a stale frame). On a respawn the VI/VO
 * bring-up races and the display sometimes comes up frozen — verify it's moving and re-cycle
 * (vo_relive) until it is. Self-heals the intermittent "live frozen on first open". */
static void ensure_live(int vichn){
    int t;
    for(t=0;t<5;t++){
        unsigned a=vo_frame_sig(); msleep(130); unsigned b=vo_frame_sig();
        if((a||b) && a!=b){ if(t) puts_("[dvr] live re-cycled\n"); return; }   /* moving -> live */
        if(t>=2) vo_force_relive(vichn); else vo_relive(vichn);
        msleep(150);
    }
    puts_("[dvr] WARN live still static after retries\n");
}

/* ---------------- live-video freeze watchdog ----------------
 * The failure this exists for: the VGA video layer latches on one frame while everything
 * else keeps running — OSD clock ticks, menus respond, the control port answers. It is NOT
 * fixed by our own restart (the OSD "Reboot" item and a channel switch both just respawn
 * the process; the MPP drivers keep their state across that), which is why it used to look
 * unfixable until the user cycled through all four channels and back.
 *
 * ensure_live() already knew how to detect and repair this, but it only ever ran ONCE at
 * startup — nothing watched for a freeze that develops later. This does, at 1 Hz.
 *
 * Detection: vo_frame_sig() hashes the displayed buffer address + the frame timestamp from
 * VO GetScreenFrame. Those advance every frame on a live video layer and are constant on a
 * latched one. Sampling at 1 Hz against 25-30 fps video means two consecutive samples must
 * differ, so N identical samples in a row is unambiguous. Deliberately NOT pixel-based: a
 * dark static scene looks identical frame to frame even when perfectly live. */
#define VW_STILL_SECS 4      /* consecutive identical 0.5 Hz samples => ~8 s of stillness */
static unsigned g_vw_last = 0;      /* previous signature */
static int      g_vw_same = 0;      /* consecutive identical samples */
static int      g_vw_stage = 0;     /* escalation stage */
static unsigned g_vw_heals = 0;     /* heal attempts this run (reported by INFO) */
static unsigned g_vw_froze = 0;     /* times we've declared a freeze */
static int g_vo_wdog = 1;           /* dvr.conf `vo_watchdog=0` disables the VO sampling */
/* dvr.conf `vi_all`: 1 = enable+start all four VI channels (the original behaviour),
 * 0 = only the channels actually consumed (the displayed one + the encoded ones).
 *
 * Why this is a suspect for the intermittent freeze: all four channels capture into the
 * shared VB pool, but only the displayed/encoded channel is ever drained — the other three
 * produce frames nobody consumes. The pool is 8 large blocks (VBCONF), so three idle
 * capture channels can hold enough of it that VI ch0 cannot get a buffer, which starves the
 * display and the encoder together — exactly the observed fault. Channel switching already
 * costs a restart, so nothing is lost by only enabling what we use. */
static int g_vi_all = 0;
static void video_watchdog(void){
    if(!g_vo_wdog) return;
    if(g_fvo < 0 || g_pb_active) { g_vw_same = 0; return; }   /* no display, or VDEC owns the VO */
    /* Sample every other second, not every second: GetScreenFrame is the one probe here
     * that touches the VO's buffer pool, so keep the duty cycle low. Against 25-30 fps
     * video even a 0.5 Hz sample must change between reads, so detection stays reliable. */
    static int skip = 0;
    if((skip ^= 1)) return;
    unsigned s = vo_frame_sig();
    if(!s){ g_vw_same = 0; return; }                          /* unreadable — never act on that */
    if(s != g_vw_last){                                       /* moving: healthy */
        if(g_vw_same >= VW_STILL_SECS) puts_("[dvr] live video recovered\n");
        g_vw_last = s; g_vw_same = 0; g_vw_stage = 0;
        return;
    }
    if(++g_vw_same < VW_STILL_SECS) return;                   /* not yet convinced */

    g_vw_same = 0;                       /* re-arm: give this attempt VW_STILL_SECS to take */
    g_vw_heals++; g_vw_froze++;
    puts_("[dvr] LIVE VIDEO FROZEN — heal stage "); putu((unsigned)g_vw_stage); puts_("\n");
    /* Same rule as the encoder watchdog: NEVER escalate to g_restart. A respawn does not
     * reset the MPP drivers, so it inherits the broken pipeline and can loop forever. Both
     * repairs below are VO-channel-local and were verified live not to disturb a healthy
     * display; if neither takes, we keep saying so and leave the decision to a human. */
    if(g_vw_stage == 0) vo_relive(g_dispch);        /* gentle: re-source the VO channel */
    else                vo_force_relive(g_dispch);  /* firm: unbind VI, then re-source   */
    if(g_vw_stage < 250) g_vw_stage++;
}

/* read /root/rec/dvr.conf (key=value lines) into the config globals */
static void apply_cfg(void){
    /* static (not stack) and big enough for a fully-commented dvr.conf — the old 600-byte
     * buffer silently dropped every key past the first ~15 lines, so a documented config
     * file lost most of its settings with no error anywhere. */
    static char buf[4096];
    int fd = (int)sys_open("/root/rec/dvr.conf", O_RDONLY, 0);
    if(fd < 0) return;
    int n = (int)sys_read(fd, buf, sizeof(buf)-1); sys_close(fd);
    if(n <= 0) return;
    buf[n]=0;
    int i=0;
    while(i<n){
        char key[24]; int kl=0;
        while(i<n && (buf[i]==' '||buf[i]=='\n'||buf[i]=='\r'||buf[i]=='\t')) i++;
        if(i<n && buf[i]=='#'){ while(i<n && buf[i]!='\n') i++; continue; }
        while(i<n && buf[i]!='=' && buf[i]!='\n' && kl<23){ key[kl++]=buf[i++]; }
        key[kl]=0;
        if(i>=n || buf[i]!='='){ while(i<n && buf[i]!='\n') i++; continue; }
        i++; /* skip = */
        int val = 0, sign=1;
        if(i<n && buf[i]=='-'){ sign=-1; i++; }
        while(i<n && buf[i]>='0' && buf[i]<='9'){ val=val*10+(buf[i]-'0'); i++; }
        val*=sign;
        while(i<n && buf[i]!='\n') i++;
        if(streq(key,"fps")&&val>0) g_fps=val;
        else if(streq(key,"gop")&&val>0) g_gop=val;
        else if(streq(key,"bitrate")&&val>0) g_bitrate=val;
        else if(streq(key,"width")&&val>0) g_ewidth=val;
        else if(streq(key,"height")&&val>0) g_eheight=val;
        else if(streq(key,"standard")&&val>=0&&val<=2) g_standard=val;
        else if(streq(key,"enc_channels")&&val>=1&&val<=NCH) g_enc_nch=val;
        else if(streq(key,"vosync")&&val>=1&&val<=14) g_vosync=val;   /* VGA mode (9=1280x1024@60, 13=1024x768@75, 12=800x600@75) — latency A/B */
        else if(streq(key,"vifps")&&val>0) g_vifps=val;               /* VI capture framerate */
        else if(streq(key,"votol")&&val>0) g_votol=val;               /* VO play toleration ms (1=min latency) */
        else if(streq(key,"rc_mode")&&(val==0||val==3)) g_rc_mode=val;
        else if(streq(key,"qp")&&val>=10&&val<=50) g_qp=val;
        else if(streq(key,"min_free_mb")&&val>0) g_min_free_mb=val;
        else if(streq(key,"max_rec_mb")&&val>0) g_max_rec_mb=val;
        else if(streq(key,"display")&&val>=0&&val<=1) g_display=val;
        else if(streq(key,"vo_watchdog")&&val>=0&&val<=1) g_vo_wdog=val;
        else if(streq(key,"vi_all")&&val>=0&&val<=1) g_vi_all=val;
        else if(streq(key,"ui_sound")&&val>=0&&val<=1) g_ui_snd=val;
    }
    /* runtime STD command persists here and overrides the conf on respawn */
    { int fd2=(int)sys_open("/root/rec/std", O_RDONLY, 0);
      if(fd2>=0){ char c; if(sys_read(fd2,&c,1)==1 && c>='0'&&c<='2') g_standard=c-'0'; sys_close(fd2); } }
    /* menu-sound toggle, set from the OSD Settings menu or the web UI */
    { int fd2=(int)sys_open("/root/rec/snd", O_RDONLY, 0);
      if(fd2>=0){ char c; if(sys_read(fd2,&c,1)==1 && (c=='0'||c=='1')) g_ui_snd=c-'0'; sys_close(fd2); } }
}
/* ---------- PAL/NTSC (TW2866 @ I2C 0x28, see docs/FIRMWARE_RE.md §8) ---------- */
/* read a TW2866 register via /dev/tw_286x ioctl 0xc00448cf {result,selector=0,reg,0,0} */
static int tw_read(int ftw, int reg){
    unsigned a[5]; a[0]=0; a[1]=0; a[2]=(unsigned)reg; a[3]=0; a[4]=0;
    sys_ioctl(ftw, 0xc00448cf, a);
    return (int)a[0];
}
/* write a TW2866 register via 0xc00448d0 {0,selector=0,reg,value,0} */
static void tw_write(int ftw, int reg, int val){
    unsigned a[5]; a[0]=0; a[1]=0; a[2]=(unsigned)reg; a[3]=(unsigned)val; a[4]=0;
    sys_ioctl(ftw, 0xc00448d0, a);
}
/* resolve the standard to use: explicit config, or auto-detect from the chip */
static int resolve_standard(int ftw){
    if(g_standard==1 || g_standard==2) return g_standard;   /* explicit */
    int present = tw_read(ftw, 0xFD) & 0x0F;                /* per-channel video-present bits */
    int ch = 0, c; for(c=0;c<NCH;c++) if(present & (1<<c)){ ch=c; break; }
    int v = tw_read(ftw, ch*0x10 + 0x00);                   /* reg 0x00 bit0: 0=NTSC 1=PAL */
    int std = (v & 1) ? 2 : 1;
    puts_("[dvr] auto-detect present="); puthex((unsigned)present);
    puts_(" reg00="); puthex((unsigned)v); puts_(" -> "); puts_(std==2?"PAL\n":"NTSC\n");
    return std;
}
/* apply the standard: set the TW2866 + patch pipeline geometry (VI norm/height, VO
 * layer source height, VENC framerate). NTSC keeps the exact proven values. */
static void apply_standard(int ftw){
    int std = resolve_standard(ftw);
    unsigned s = (std==2)?2:1;
    sys_ioctl(ftw, 0xc00448d3, &s);                 /* TW2866 set standard (app.out does this) */
    VI_PUB[8]  = (std==2)?0:1;                       /* VI norm: NTSC=1, PAL=0 */
    put32(VI_CHN, 12, (std==2)?0x120:0xf0);          /* VI capture field height 288/240 */
    put32(VGA_LAYER, 20, (std==2)?576:480);          /* VO layer source height (field doubled) */
    if(std==2) g_fps = 25;                           /* PAL = 25 fps (NTSC keeps configured fps) */
    g_eheight = (std==2)?288:240;                    /* encode the single-field height */
    patch_venc_attrs();                              /* re-derive VENC w/h/fps/gop/bitrate/bufsz */
    puts_("[dvr] standard="); puts_(std==2?"PAL":"NTSC"); puts_("\n");
}
/* the TW2866 set-standard leaves the per-channel picture regs at odd values (seen:
 * sat=0xB8, hue=0x78 -> pink/green cast). Reset all channels to neutral defaults so
 * the camera looks right on boot. (User can fine-tune via the Picture menu.) */
static void apply_picture_defaults(int ftw){
    int c; for(c=0;c<NCH;c++){ int base=c*0x10;
        tw_write(ftw, base+0x01, 0x00);   /* brightness (neutral) */
        tw_write(ftw, base+0x02, 0x64);   /* contrast */
        tw_write(ftw, base+0x04, 0x80);   /* saturation U */
        tw_write(ftw, base+0x05, 0x80);   /* saturation V */
        tw_write(ftw, base+0x06, 0x00);   /* hue */
    }
}
/* patch the create + RC attr byte arrays from the config globals */
static void patch_venc_attrs(void){
    /* stream buffer must hold a full keyframe; ~1s at the bitrate + headroom. Keep it modest —
     * a >4MB ring fails to allocate on this 43MB device (VENC then gives BUF_EMPTY). The default
     * 12288 basis = ~1.8MB, verified to hold FIXQP QP10 bursts to 23 Mbps with 0 errors. */
    unsigned raw = g_ewidth * g_eheight * 3 / 2;
    unsigned br  = g_bitrate * 1024 / 8;             /* ~1s of stream at the cap/basis, in bytes */
    unsigned bufsz = (br > raw ? br : raw) + (1u<<18);
    unsigned char *A = VENC_ATTR, *R = VENC_RC;
    put32(A,8,g_ewidth); put32(A,12,g_eheight); put32(A,16,g_fps); put32(A,28,bufsz);
    put32(A,40,g_fps);   put32(A,44,g_gop);     put32(A,52,g_rc_mode); put32(A,56,g_bitrate);
    put32(R,8,g_ewidth); put32(R,12,g_eheight); put32(R,16,g_fps); put32(R,28,bufsz);
    put32(R,40,g_fps);   put32(R,44,g_gop);     put32(R,52,g_rc_mode); put32(R,56,g_bitrate);
    if(g_rc_mode==3){    /* FIXQP: constant QpI/QpP, bitrate cap ignored */
        put32(A,64,g_qp); put32(A,68,g_qp); put32(R,64,g_qp); put32(R,68,g_qp);
    }
}

/* ---------- manual per-channel recording (MPEG-TS to SATA) ---------- */
static int      rec_want[NCH];      /* control-requested on/off */
static int      rec_on[NCH];        /* actually recording (started at a keyframe) */
static int      rec_fd[NCH];        /* output .ts file */
static tsmux    rec_ts[NCH];        /* per-channel TS muxer */
static unsigned rec_mb[NCH];        /* bytes written this recording, in MB */
static long     rec_start_sec[NCH]; /* wall-clock second recording began (for HUD elapsed) */
static unsigned long long rec_base_us[NCH];  /* encoder PTS (pack[6..7], µs) of this
                                     * recording's first frame — the file's timeline is the
                                     * encoder's own capture clock, so a dropped frame shows
                                     * up as a time gap instead of silently speeding playback */
static int      rec_stopping[NCH];  /* STOP requested; draining the encoder before closing */
static unsigned rec_drain_end[NCH]; /* now_ms() cap on that drain */
#define REC_DRAIN_MS 1200
static unsigned rec_b[NCH];         /* sub-MB byte accumulator */
static unsigned char au_buf[768*1024];   /* access-unit assembly (one channel at a time) */
/* encode-loop state, global so play_file() can keep DRAINING + RECORDING the encoder while a
 * recording plays back (VENC and VDEC are separate engines; only our software loop was the
 * bottleneck — playback blocked the main loop, so the encoder buffer filled and recording paused). */
static int      fve[NCH];                 /* per-channel VENC fds (-1 = none) */
static unsigned char *bufv[NCH];          /* mmap'd VENC stream buffer base per channel */
static unsigned buf_hoff[NCH], buf_p2[NCH], buf_len[NCH];
static int      dbg[NCH];                 /* first-N GetStream debug prints */
static unsigned long total_bytes=0, total_packs=0;
static unsigned long g_packs_last=0;      /* for the 1 s encoder-throughput window */
static unsigned      g_pps=0;             /* encoder packs in the last second (INFO pps=) */

/* open /root/rec/aN/YYYYMMDD_HHMMSS_chN.ts and start the muxer */
static void rec_start(int chn){
    if(free_mb("/root/rec/a1") < g_min_free_mb){
        puts_("[dvr] REC refused ch"); putu((unsigned)chn); puts_(" — low disk space\n");
        rec_want[chn]=0; return;
    }
    char nm[48]; int p=0;
    memcpy(nm,"/root/rec/a",11); p=11; nm[p++]=(char)('1'+chn); nm[p++]='/';
    rtc_stamp(nm+p); p+=15;             /* YYYYMMDD_HHMMSS */
    memcpy(nm+p,"_ch",3); p+=3; nm[p++]=(char)('1'+chn);
    memcpy(nm+p,".ts",4);               /* includes NUL */
    rec_fd[chn] = (int)sys_open(nm, O_WRONLY|O_CREAT|O_TRUNC, 0644);
    if(rec_fd[chn] < 0){ puts_("[dvr] REC open fail ch"); putu((unsigned)chn); puts_("\n"); return; }
    ts_open(&rec_ts[chn], rec_fd[chn], g_fps);
    rec_on[chn] = 1; rec_mb[chn]=0; rec_b[chn]=0; rec_start_sec[chn]=now_sec();
    rec_base_us[chn]=0; rec_stopping[chn]=0;   /* base latched on the first frame written */
    melody(MEL_REC_START, MEL_REC_START_N);   /* rising arpeggio */
    puts_("[dvr] REC start ch"); putu((unsigned)chn); puts_(" -> "); puts_(nm); puts_("\n");
}
static void rec_stop(int chn){
    if(!rec_on[chn]) return;
    ts_close(&rec_ts[chn]);
    if(rec_fd[chn] >= 0){ sys_close(rec_fd[chn]); rec_fd[chn] = -1; }
    rec_on[chn] = 0; rec_stopping[chn] = 0;
    melody(MEL_REC_STOP, MEL_REC_STOP_N);     /* falling motif */
    puts_("[dvr] REC stop ch"); putu((unsigned)chn); puts_("\n");
}

/* ---------- control plane: TCP port 8090 (web) + /dev/ttyAMA1 (MCU), one command
 * set drives one record-state owner (rec_want[]). See docs/CONTROL_PROTOCOL.md. ---- */
#define CTL_PORT 8090
#define MAXCTL 4
static int  ctl_lfd = -1;
static int  ctl_cfd[MAXCTL];
static char ctl_line[MAXCTL][128]; static int ctl_len[MAXCTL];
static int  ser_fd = -1;
static char ser_line[128]; static int ser_len = 0;
/* g_standard / g_std_changed declared with the config globals above */

static void wrs(int fd, const char *s){ if(fd>=0) sys_write(fd, s, k_strlen(s)); }
/* case-insensitive: does token at *a (until space/end) equal keyword b? */
static int tok_eq(const char *a, const char *b){
    while(*b){ char c=*a; if(c>='a'&&c<='z') c-=32; if(c!=*b) return 0; a++; b++; }
    return (*a==0 || *a==' ' || *a=='\r');
}
static const char *tok_next(const char *s){ while(*s&&*s!=' ')s++; while(*s==' ')s++; return s; }

/* capture the actual composed VGA output (HI_MPI_VO_GetScreenFrame) to /root/rec/shot.yuv
 * as packed NV (SP420). PC pulls it + converts to PNG — a real screenshot of the display. */
static void do_shot(int rfd){
    if(g_fvo < 0){ wrs(rfd,"ERR novo\n"); return; }
    unsigned f[40]; memset(f,0,sizeof(f));
    if(ioc(g_fvo, 0xc0984f0f, f) != 0){ wrs(rfd,"ERR getframe\n"); return; }   /* GetScreenFrame */
    unsigned w=f[0], h=f[1], fmt=f[3], p0=f[4], p1=f[5], s0=f[10], s1=f[11];
    int fmem = (int)sys_open("/dev/mem", O_RDWR|O_SYNC, 0);
    if(fmem<0 || w==0 || h==0 || p0==0){ ioc(g_fvo,0x40984f10,f); wrs(rfd,"ERR frame\n"); return; }
    unsigned base = p0 & 0xfffff000u, off0 = p0 - base;
    unsigned span = s0*h + s1*(h/2) + 0x2000;
    unsigned mlen = (off0 + span + 0xfff) & 0xfffff000u;
    unsigned char *m = (unsigned char*)sys_mmap2(0, mlen, PROT_READ, MAP_SHARED, fmem, base>>12);
    if((long)m == -1 || (long)m < 0){ sys_close(fmem); ioc(g_fvo,0x40984f10,f); wrs(rfd,"ERR mmap\n"); return; }
    int of = (int)sys_open("/root/rec/shot.yuv", O_WRONLY|O_CREAT|O_TRUNC, 0644);
    if(of>=0){
        unsigned y;
        unsigned char *luma = m + off0;
        unsigned char *chroma = m + (p1 - base);
        for(y=0;y<h;y++)   sys_write(of, luma   + y*s0, w);     /* packed Y  */
        for(y=0;y<h/2;y++) sys_write(of, chroma + y*s1, w);     /* packed UV (interleaved) */
        sys_close(of);
    }
    sys_munmap(m, mlen); sys_close(fmem);
    ioc(g_fvo, 0x40984f10, f);   /* ReleaseScreenFrame */
    { char b[64]; int l=0; memcpy(b,"SHOT ",5); l=5;
      l+=u2s(b+l,w); b[l++]='x'; l+=u2s(b+l,h);
      memcpy(b+l," fmt=",5); l+=5; l+=u2s(b+l,fmt);
      memcpy(b+l," s0=",4); l+=4; l+=u2s(b+l,s0); b[l++]='\n';
      if(rfd>=0) sys_write(rfd,b,l); }
}

/* ---- on-screen UI glue (ui.h model <-> device state) ---------------------- */
/* TW2866 per-channel picture registers (base = ch*0x10): +01 bright, +02 contrast,
 * +04/05 saturation U/V, +06 hue (standard TW2864 map — docs/FIRMWARE_RE.md). */
static int ui_pic_off(int which){
    return which==VAL_BRIGHT?0x01 : which==VAL_CONTRAST?0x02 : which==VAL_HUE?0x06 : 0x04;
}
/* HUD clock, tracked from the monotonic uptime so the per-second HUD update never reads the RTC
 * (which may share the bitbang I2C with the video chip -> a live-frame drop). Synced from the RTC
 * once at boot and again whenever the TIME command sets it. */
static int  g_clk_valid=0;
static long g_clk_base_up=0;      /* now_sec() at the last sync */
static long g_clk_sod=0;          /* seconds-of-day at the last sync */
static void clock_sync(void){
    struct rtc_time_ t;
    if(rtc_read(&t)==0 && t.tm_year>=100 && t.tm_year<=200){
        g_clk_sod = (long)t.tm_hour*3600 + (long)t.tm_min*60 + t.tm_sec;
        g_clk_base_up = now_sec(); g_clk_valid=1;
    } else g_clk_valid=0;
}
/* pull current device state into the UI model for rendering */
static void ui_refresh(void){
    g_ui.sound = g_ui_snd;
    int c;
    g_ui.chan = (g_dispch>=0 && g_dispch<NCH) ? g_dispch : 0;
    for(c=0;c<NCH;c++) g_ui.rec[c] = rec_on[c]?1:(rec_want[c]?2:0);
    g_ui.rec_secs = rec_on[g_ui.chan] ? (int)(now_sec()-rec_start_sec[g_ui.chan]) : 0;
    g_ui.standard = g_standard;
    g_ui.display  = g_display;
    { char *b=g_ui.timestr;          /* derive HH:MM:SS from uptime — no per-second RTC/I2C read */
      if(g_clk_valid){ long sod=(g_clk_sod + (now_sec()-g_clk_base_up))%86400; if(sod<0) sod+=86400;
          int h=(int)(sod/3600), m=(int)((sod/60)%60), s=(int)(sod%60);
          two(b,h); b[2]=':'; two(b+3,m); b[5]=':'; two(b+6,s); b[8]=0;
      } else { b[0]='-'; b[1]=0; } }
    /* NOTE: the TW2866 picture registers are NOT read here. This runs every second (HUD clock),
     * and tw_read goes over the GPIO-bitbang I2C to the video-decoder chip, which busy-waits with
     * IRQs masked — delaying the VIU capture-done interrupt and DROPPING a live frame every second.
     * The picture values only change when the user edits them (we update g_ui.pic then), so we read
     * them once via ui_read_picture() at boot instead. */
}
/* read the TW2866 picture regs for the shown channel into g_ui.pic — call sparingly (boot only;
 * NEVER per-frame/per-second, see ui_refresh) as it disturbs live capture. */
static void ui_read_picture(void){
    if(g_ftw<0) return;
    int base = g_ui.chan*0x10;
    g_ui.pic[0]=tw_read(g_ftw,base+0x01)&0xff;
    g_ui.pic[1]=tw_read(g_ftw,base+0x02)&0xff;
    g_ui.pic[2]=tw_read(g_ftw,base+0x06)&0xff;
    g_ui.pic[3]=tw_read(g_ftw,base+0x04)&0xff;
}
/* switch which VI channel the VGA shows. The VI->VO binding is established in the main
 * pipeline at boot and can't be cleanly re-latched on a live VO (needs more VO RE), so
 * persist the choice and respawn — the wrapper re-inits the pipeline on the new channel.
 * (~a few seconds; the default channel is the camera, so the common case is instant.) */
/* Is any channel recording or armed? The things that respawn the process have to ask,
 * because a respawn ENDS a capture — cleanly (rec_stop flushes on the way out) but
 * silently, which is worse: the operator keeps flying believing they are still recording. */
static int rec_busy(void){
    int c; for(c=0;c<NCH;c++) if(rec_on[c] || rec_want[c]) return 1;
    return 0;
}
static const unsigned char BEEP_REFUSE[] = {4,3,4};   /* on/off/on, 20 ms units */
/* returns 0 if the display channel changed, -1 if refused. Changing it re-latches VI->VO,
 * which we can only do by respawning — so it is refused outright while recording. */
static int switch_channel(int ch){
    if(ch<0 || ch>=NCH) return -1;
    if(rec_busy()){
        beep(BEEP_REFUSE, 3);
        puts_("[dvr] channel switch refused — recording\n");
        return -1;
    }
    g_dispch = ch;
    int fd=(int)sys_open("/root/rec/dispch", O_WRONLY|O_CREAT|O_TRUNC, 0644);
    if(fd>=0){ char c=(char)('0'+ch); sys_write(fd,&c,1); sys_close(fd); }
    g_restart = 1;   /* run_dvr.sh respawns us showing the new channel */
    return 0;
}
/* scan /root/rec/a1..a4 for dated .ts recordings and fill the Playback list (newest first) */
#define PBMAX 64
static char pb_label[PBMAX][20];
static char pb_path[PBMAX][48];
static char pb_key[PBMAX][16];   /* YYYYMMDDHHMMSS sort key (from the filename) */
static void pb_swap(int i, int j){
    char t[48]; int k;
    for(k=0;k<20;k++){ t[k]=pb_label[i][k]; pb_label[i][k]=pb_label[j][k]; pb_label[j][k]=t[k]; }
    for(k=0;k<48;k++){ t[k]=pb_path[i][k];  pb_path[i][k]=pb_path[j][k];   pb_path[j][k]=t[k]; }
    for(k=0;k<16;k++){ t[k]=pb_key[i][k];   pb_key[i][k]=pb_key[j][k];     pb_key[j][k]=t[k]; }
}
/* fill pb_label/pb_path/pb_key from /root/rec/a1..a4 (newest first) and return the count.
 * Pure scan — touches no UI state, so the control-plane LIST command can use it too. */
static int scan_recordings(void){
    int cnt=0, p;
    for(p=0; p<NCH && cnt<PBMAX; p++){
        char dpath[16]; int k=0; const char *pre="/root/rec/a";
        while(pre[k]){ dpath[k]=pre[k]; k++; } dpath[k++]=(char)('1'+p); dpath[k]=0;
        int fd=(int)sys_open(dpath, O_RDONLY|O_DIRECTORY, 0);
        if(fd<0) continue;
        char buf[4096]; long n;
        while((n=sys_getdents64(fd, buf, sizeof(buf)))>0 && cnt<PBMAX){
            long off=0;
            while(off<n && cnt<PBMAX){
                unsigned char *e=(unsigned char*)(buf+off);
                unsigned short reclen = (unsigned short)(e[16] | (e[17]<<8));
                if(reclen==0) break;
                char *name=(char*)(e+19);
                int L=0; while(name[L]) L++;
                if(L>=22 && name[L-3]=='.'&&name[L-2]=='t'&&name[L-1]=='s'
                   && name[0]>='0'&&name[0]<='9'){       /* YYYYMMDD_HHMMSS_chN.ts */
                    int idx=cnt; char *lb=pb_label[idx];
                    lb[0]=name[4];lb[1]=name[5];lb[2]='/';lb[3]=name[6];lb[4]=name[7];lb[5]=' ';
                    lb[6]=name[9];lb[7]=name[10];lb[8]=':';lb[9]=name[11];lb[10]=name[12];lb[11]=' ';
                    lb[12]='C';lb[13]='H';lb[14]=(name[18]>='1'&&name[18]<='4')?name[18]:'?';lb[15]=0;
                    char *fp=pb_path[idx]; int j=0; while(dpath[j]){fp[j]=dpath[j];j++;}
                    fp[j++]='/'; int m=0; while(name[m] && j<47){fp[j++]=name[m++];} fp[j]=0;
                    /* sort key = YYYYMMDDHHMMSS (skip the '_' at name[8]) */
                    char *ky=pb_key[idx]; int ki=0;
                    for(m=0;m<15 && name[m];m++){ if(m!=8) ky[ki++]=name[m]; } ky[ki]=0;
                    cnt++;
                }
                off += reclen;
            }
        }
        sys_close(fd);
    }
    /* selection sort newest-first by the timestamp key (desc) */
    { int a,b,best; for(a=0;a<cnt-1;a++){ best=a;
        for(b=a+1;b<cnt;b++){ int c=0,z; for(z=0;z<14;z++){ if(pb_key[b][z]!=pb_key[best][z]){ c=(pb_key[b][z]>pb_key[best][z])?1:-1; break; } }
            if(c>0) best=b; }
        if(best!=a) pb_swap(a,best); } }
    return cnt;
}
/* the OSD Playback menu's view of the list: rescan and reset the grid selection */
static void list_recordings(void){
    int cnt=scan_recordings(), i;
    for(i=0;i<cnt;i++) g_ui.pb_names[i]=pb_label[i];
    g_ui.pb_count=cnt; g_ui.pb_top=0; g_ui.sel=0;
    thumb_rebind();   /* keep any already-generated thumbnails whose recording is still present */
}
/* rescan for the control plane without disturbing the user's on-screen selection */
static int refresh_recordings(void){
    int cnt=scan_recordings(), i;
    for(i=0;i<cnt;i++) g_ui.pb_names[i]=pb_label[i];
    if(g_ui.pb_count!=cnt){
        g_ui.pb_count=cnt;
        if(g_ui.sel>=cnt)    g_ui.sel = cnt?cnt-1:0;
        if(g_ui.pb_top>=cnt) g_ui.pb_top = 0;
        thumb_rebind();
    }
    return cnt;
}
/* Is this exactly one of our recordings, i.e. /root/rec/a<1..4>/<name>.ts ?
 * DEL unlinks whatever it is given, so this is the only thing standing between a
 * malformed control line and the rootfs: no directory component in the filename,
 * no "..", and the .ts suffix is mandatory. */
static int is_rec_path(const char *p){
    const char *pre="/root/rec/a"; int i=0, L;
    while(pre[i]){ if(p[i]!=pre[i]) return 0; i++; }
    if(p[i]<'1' || p[i]>'4') return 0; i++;
    if(p[i]!='/') return 0; i++;
    if(!p[i]) return 0;                       /* need a filename */
    L=i; while(p[L]) L++;
    if(L-i < 3) return 0;
    if(p[L-3]!='.' || p[L-2]!='t' || p[L-1]!='s') return 0;
    for(; p[i]; i++){                         /* filename must be a single component */
        if(p[i]=='/') return 0;
        if(p[i]=='.' && p[i+1]=='.') return 0;
    }
    return 1;
}
/* size of a file, or -1 (no stat syscall in our OABI runtime — open+lseek is enough) */
static long file_size(const char *path){
    int fd=(int)sys_open(path, O_RDONLY, 0);
    if(fd<0) return -1;
    long n=sys_lseek(fd,0,2);
    sys_close(fd);
    return n;
}
/* apply a UI action (from ui_key) to real device state */
static void ui_apply(int act){
    int c;
    switch(act){
        case UI_ACT_REC:
            if(g_ui.act_a<0){ for(c=0;c<NCH;c++) rec_want[c]=g_ui.act_b; }
            else if(g_ui.act_a<NCH){ rec_want[g_ui.act_a]=g_ui.act_b; }
            break;
        case UI_ACT_STD: {
            /* takes effect by respawning, so it would silently end a capture */
            if(rec_busy()){ beep(BEEP_REFUSE,3); puts_("[dvr] standard change refused — recording\n"); break; }
            g_standard=g_ui.act_a; g_std_changed=1;
            int fd2=(int)sys_open("/root/rec/std",O_WRONLY|O_CREAT|O_TRUNC,0644);
            if(fd2>=0){ char cc=(char)('0'+g_ui.act_a); sys_write(fd2,&cc,1); sys_close(fd2); }
            break; }
        case UI_ACT_DISPLAY:
            g_display=g_ui.act_a;           /* full effect on next pipeline init; STATUS reflects it */
            break;
        case UI_ACT_PICTURE: {
            if(g_ftw<0) break;
            int which=g_ui.act_a, base=g_ui.chan*0x10, off=ui_pic_off(which);
            int nv=(tw_read(g_ftw,base+off)&0xff)+g_ui.act_b;
            if(nv<0)nv=0; if(nv>255)nv=255;
            tw_write(g_ftw,base+off,nv);
            if(which==VAL_SAT) tw_write(g_ftw,base+0x05,nv);   /* SAT_V tracks SAT_U */
            if(which>=VAL_BRIGHT && which<=VAL_SAT) g_ui.pic[which-VAL_BRIGHT]=nv;
            break; }
        case UI_ACT_CHANNEL:
            switch_channel(g_ui.act_a);     /* re-run display setup so the switch takes effect */
            break;
        case UI_ACT_OPEN_PB:                /* user opened Playback — scan for recordings */
            list_recordings();
            break;
        case UI_ACT_PB_PLAY:                /* play the selected recording full-screen */
            if(g_ui.act_a>=0 && g_ui.act_a<g_ui.pb_count){
                int sel=g_ui.act_a;         /* remember the cell so we can restore the grid selection */
                thumb_teardown();           /* free the thumbnail VDEC/VO before play_file grabs its own */
                g_ui.open=0;                /* hide the menu; keep the HUD */
                ui_refresh(); ui_render(&g_ui); fb_blit(0,0,(int)g_fbw,(int)g_fbh);
                play_file(pb_path[sel]);
                /* media-player behavior: exiting a clip (RETURN / right-click / X) drops back to the
                 * Playback grid, not live — exit the grid once more to reach the live view. */
                g_ui.menu=MENU_PLAYBACK; g_ui.open=1;
                if(sel>=0 && sel<g_ui.pb_count) g_ui.sel=sel;
                ui_paint();
            }
            break;
        case UI_ACT_PICRESET:               /* TW2866 picture defaults for the shown channel */
            if(g_ftw>=0){ int base=g_ui.chan*0x10;
                tw_write(g_ftw,base+0x01,0x00);   /* brightness */
                tw_write(g_ftw,base+0x02,0x64);   /* contrast   */
                tw_write(g_ftw,base+0x06,0x00);   /* hue        */
                tw_write(g_ftw,base+0x04,0x80);   /* sat U      */
                tw_write(g_ftw,base+0x05,0x80);   /* sat V      */
            }
            break;
        case UI_ACT_SOUND: {
            g_ui_snd = g_ui.act_a ? 1 : 0;
            /* persist like STD/dispch do: a one-byte file boot.sh mirrors to SATA, so the
             * setting survives both our respawn and a power cycle */
            int fd2=(int)sys_open("/root/rec/snd",O_WRONLY|O_CREAT|O_TRUNC,0644);
            if(fd2>=0){ char cc=(char)('0'+g_ui_snd); sys_write(fd2,&cc,1); sys_close(fd2); }
            break; }
        case UI_ACT_REBOOT:
            g_restart=1;                    /* wrapper respawns us (persistence = M4) */
            break;
        default: break;
    }
}
/* pixel size of the current VGA output mode (g_vosync) — the OSD matches it */
static void osd_wh(unsigned *w, unsigned *h){
    switch(g_vosync){
        case 2:  *w=1280; *h=720;  break;
        case 6:  *w=1920; *h=1080; break;
        case 7:  case 12: *w=800;  *h=600;  break;   /* 800x600  @60 / @75 */
        case 8:  case 13: *w=1024; *h=768;  break;   /* 1024x768 @60 / @75 */
        case 10: *w=1366; *h=768;  break;
        case 11: *w=1440; *h=900;  break;
        default: *w=1280; *h=1024; break;   /* 9 = 1280x1024 */
    }
}
/* open /dev/fb0 for the OSD once, sized to the VGA output (the VO must already be up
 * at that resolution — display_vga ran first — so HiFB won't clamp the fb canvas). */
static void ui_ensure_fb(void){
    if(g_ui_fb) return;
    unsigned w,h; osd_wh(&w,&h);
    if(fb_open_mode(w,h)==0){ g_ui_fb=1; ui_init(&g_ui); co_open(); }
}
/* full render to the shadow, then blit the whole menu layer (structural changes) */
static void ui_paint(void){
    if(!g_ui_fb) return;
    ui_refresh();
    ui_render(&g_ui);
    fb_blit(0,0,(int)g_fbw,(int)g_fbh);
}
/* cheap periodic refresh (once/sec): redraw only the HUD strip so the clock + REC
 * state stay live. Skipped while the menu is open (it repaints on interaction). */
static void ui_tick(void){
    if(!g_ui_fb) return;                 /* HUD strip is top-left; safe over the corner menu... */
    if(g_egg_on) return;                 /* ...and must not repaint over the easter egg */
    if(g_ui.open && g_ui.menu==MENU_PLAYBACK) return;   /* ...but NOT over the full-screen grid */
    ui_refresh();
    ui_render_hud_only(&g_ui);
    fb_blit(0,0,460,46);
}

/* ---------------- easter egg: type "ucanhayri" on the live view ----------------
 * A big paper plane — the mouse cursor, seven times over — flies bottom-right to
 * top-left across "UÇAN HAYRİ WAS HERE", for ten seconds, and then everything is
 * exactly as it was.
 *
 * Two things keep it harmless. It only arms on the plain live view with nothing
 * recording and nothing playing back, so it can never scribble over a flight
 * capture. And it is a state machine ticked from the main loop, never a blocking
 * animation: a busy-wait here would stop draining the encoder and cost real frames
 * (docs/REVIEW.md R10), so every tick does a slice of work and returns.
 *
 * Drawing reuses the mouse cursor's trick. The caption lives in the shadow buffer
 * and the plane is punched straight onto the real fb, so erasing the plane is a
 * single blit from the shadow and the caption underneath survives untouched —
 * no per-frame recomposition of the whole screen. */
#define EGG_WORD     "egg"        /* short on purpose; the guards below are what keep it safe */
#define EGG_MS       10000u      /* whole show */
#define EGG_FLY_MS    8200u      /* plane crosses the screen in this long */
#define EGG_TEXT_MS   1100u      /* caption fades in */
#define EGG_HOLD_MS   9300u      /* caption goes, brief empty beat, then restore */
#define EGG_SCALE         7      /* 7x the cursor => 217 x 182 px of paper plane */

/* Ç and İ are not in the ASCII font. \001 and \002 stand in for them so the caption
 * string stays plain ASCII and nothing here has to decode UTF-8. Bit 0 = leftmost
 * pixel, matching FONT8X8. */
static const unsigned char EGG_G_CCED[8] = {0x3C,0x66,0x03,0x03,0x03,0x66,0x3C,0x18};
static const unsigned char EGG_G_IDOT[8] = {0x18,0x00,0x3E,0x18,0x18,0x18,0x3E,0x00};
#define EGG_CAPTION  "U\001AN HAYR\002 WAS HERE"

static int      g_egg_on = 0;   /* definition; forward-declared near the top */
static unsigned g_egg_t0 = 0;
static int      g_egg_px = 0, g_egg_py = 0, g_egg_drawn = 0;   /* last plane rect */
static int      g_egg_cap = 0;           /* caption currently sitting in the shadow */
static unsigned g_egg_phase = 0xffffu;   /* forces the first caption paint */

/* The flight music: ~8 s of original chiptune on a one-bit buzzer, landing a beat before
 * the caption clears at EGG_HOLD_MS.
 *
 * Every note is one 90 ms unit, and a SUSTAINED note is written as that unit repeated —
 * {1568,90},{1568,90} rather than {1568,180}. That is not stylistic. buzz_tone() bit-bangs
 * the GPIO and blocks for the note's whole length, and egg_tick() only gets to move the
 * plane between notes, so note length directly sets the animation's frame rate: 90 ms
 * units give ~10 fps, a single 300 ms note would freeze the plane mid-air for a third of
 * a second. Back-to-back units at one pitch read as a light tremolo, which suits a square
 * wave anyway. Corollary: never lengthen a note here — add another unit instead.
 *
 * These numbers only became meaningful once BUZZ_LOOP_PER_US was measured. Before that a
 * 90 ms note actually sounded for 17 ms, which is why the whole tune used to be over in
 * 3-4 seconds while claiming to be seven. The flip side: notes now block for their real
 * length, so the animation is smoother the shorter the unit. If it ever needs to be
 * silkier, cut the unit and add proportionally more of them — do not stretch the show. */
static const note_t MEL_EGG[] = {
    /* wheels up */
    { 523,90},{ 659,90},{ 784,90},{1047,90},{1047,90},{0,60},
    /* theme */
    {1047,90},{ 988,90},{1047,90},{1175,90},{1175,90},{0,50},
    {1047,90},{ 988,90},{ 880,90},{ 880,90},{0,70},
    /* answering phrase */
    { 784,90},{ 880,90},{ 988,90},{1047,90},{1047,90},{0,50},
    { 988,90},{ 880,90},{ 784,90},{ 784,90},{0,70},
    /* same shape, higher — climbing */
    {1047,90},{1175,90},{1319,90},{1568,90},{1568,90},{0,50},
    {1319,90},{1175,90},{1047,90},{1047,90},{0,60},
    /* long glide back down */
    {1568,90},{1319,90},{1047,90},{ 880,90},{ 784,90},{784,90},{0,60},
    /* victory lap */
    {1175,90},{1319,90},{1175,90},{1319,90},{1568,90},{1568,90},{0,60},
    /* trill */
    {1568,80},{1319,80},{1568,80},{1319,80},{1568,80},{1319,80},
    {1568,90},{1568,90},{1568,90},{0,80},
    /* one last climb */
    {1319,90},{1568,90},{1319,90},{1047,90},{1047,90},{0,60},
    /* touchdown */
    {1047,90},{1319,90},{1568,90},{1976,90},{1976,90},{1976,90}
};
#define MEL_EGG_N (int)(sizeof(MEL_EGG)/sizeof(MEL_EGG[0]))

static unsigned egg_colour(unsigned k){
    switch(k % 6u){
        case 0:  return fb_rgb(255, 90, 90);
        case 1:  return fb_rgb(255,200, 60);
        case 2:  return fb_rgb(120,255,120);
        case 3:  return fb_rgb( 80,220,255);
        case 4:  return fb_rgb(190,140,255);
        default: return fb_rgb(255,255,255);
    }
}
static int egg_caplen(void){ int n=0; const char *p=EGG_CAPTION; while(*p){n++;p++;} return n; }
/* biggest integer scale whose caption still leaves a margin on this panel */
static int egg_capscale(void){
    int s = ((int)g_fbw - 48) / (egg_caplen()*8);
    if(s < 2) s = 2; if(s > 8) s = 8; return s;
}
static void egg_glyph(int x,int y,unsigned char ch,unsigned col,int s){
    const unsigned char *g;
    if(ch==1) g=EGG_G_CCED; else if(ch==2) g=EGG_G_IDOT;
    else { fb_char(x,y,ch,col,FB_TRANSPARENT,s); return; }
    int row,cx,sx,sy;
    for(row=0;row<8;row++){ unsigned char b=g[row];
        for(cx=0;cx<8;cx++){ if(!((b>>cx)&1)) continue;
            for(sy=0;sy<s;sy++) for(sx=0;sx<s;sx++) fb_px(x+cx*s+sx, y+row*s+sy, col); } }
}
static void egg_caption(int x,int y,unsigned col,int s){
    const char *p=EGG_CAPTION;
    for(; *p; p++){ egg_glyph(x,y,(unsigned char)*p,col,s); x += 8*s; }
}
/* the plane goes straight onto the REAL fb, exactly like the cursor sprite */
static void egg_plane(int x,int y,unsigned fill){
    const int s=EGG_SCALE; int i,j,sx,sy;
    unsigned edge = fb_rgb(0,0,0);
    for(j=0;j<CUR_H;j++){ const char *r=CURSOR_SPR[j];
        for(i=0;i<CUR_W && r[i]; i++){
            unsigned c;
            if(r[i]=='X') c=edge; else if(r[i]=='.') c=fill; else continue;
            for(sy=0;sy<s;sy++) for(sx=0;sx<s;sx++) fbreal_px(x+i*s+sx, y+j*s+sy, c);
        } }
}
static int egg_can_run(void){
    int c;
    if(!g_ui_fb || g_egg_on) return 0;
    if(g_ui.open)   return 0;            /* a menu or the playback grid owns the screen */
    if(g_pb_active) return 0;
    for(c=0;c<NCH;c++) if(rec_on[c] || rec_want[c]) return 0;   /* never touch a capture */
    return 1;
}
static void egg_start(void){
    if(!egg_can_run()) return;
    g_egg_on=1; g_egg_t0=now_ms(); g_egg_drawn=0; g_egg_cap=0; g_egg_phase=0xffffu;
    fb_fill(FB_CLEAR); fb_blit(0,0,(int)g_fbw,(int)g_fbh);   /* HUD out of the way */
    /* Respects the Sound setting, unlike the record melodies. Those are 200 ms of
     * operational feedback you want even with beeps off; this is eight seconds of
     * music, and someone who silenced the box meant it. */
    if(g_ui_snd) melody(MEL_EGG, MEL_EGG_N);
    puts_("[dvr] ucanhayri — clear the runway\n");
}
static void egg_finish(void){
    g_egg_on=0; g_egg_drawn=0; g_egg_cap=0;
    fb_fill(FB_CLEAR); fb_blit(0,0,(int)g_fbw,(int)g_fbh);
    ui_paint();                          /* HUD back exactly as it was */
}
static void egg_tick(void){
    if(!g_egg_on) return;
    /* egg_start() checked these, but a REC or PLAY can still arrive afterwards over the
     * control port or the web UI, which egg_start() has no say over. Something real just
     * asked for the screen and the encoder — stop clowning and hand them back. */
    { int c; for(c=0;c<NCH;c++) if(rec_on[c] || rec_want[c]){ egg_finish(); return; } }
    if(g_pb_active){ egg_finish(); return; }
    unsigned t = now_ms() - g_egg_t0;
    if(t >= EGG_MS){ egg_finish(); return; }

    const int pw = CUR_W*EGG_SCALE, ph = CUR_H*EGG_SCALE;

    if(t >= EGG_TEXT_MS && t < EGG_HOLD_MS){
        unsigned k = t/300u;                     /* recolour ~3x/sec, not every tick */
        if(k != g_egg_phase){
            int s  = egg_capscale();
            int cw = egg_caplen()*8*s;
            int cx = ((int)g_fbw - cw)/2, cy = ((int)g_fbh - 8*s)/2;
            g_egg_phase = k;
            fb_rect(cx-10, cy-10, cw+20, 8*s+20, FB_CLEAR);
            egg_caption(cx+s/2, cy+s/2, fb_rgb(0,0,0), s);   /* drop shadow, for video behind */
            egg_caption(cx,     cy,     egg_colour(k), s);
            fb_blit(cx-10, cy-10, cw+20, 8*s+20);
            g_egg_cap = 1;
        }
    } else if(g_egg_cap && t >= EGG_HOLD_MS){
        fb_fill(FB_CLEAR); fb_blit(0,0,(int)g_fbw,(int)g_fbh);
        g_egg_cap = 0; g_egg_drawn = 0;          /* that blit erased the plane too */
    }

    if(g_egg_drawn) fb_blit(g_egg_px, g_egg_py, pw, ph);   /* erase: restores the caption */
    g_egg_drawn = 0;
    if(t < EGG_FLY_MS){
        int p = (int)((t*1000u)/EGG_FLY_MS); if(p>1000) p=1000;
        int x = (int)g_fbw - (((int)g_fbw + pw)*p)/1000;
        int y = (int)g_fbh - (((int)g_fbh + ph)*p)/1000;
        int arc = (p*(1000-p))/1000;             /* 0..250, peaks mid-flight */
        y -= (arc*200)/250;                      /* lifts the path into a lazy arc */
        egg_plane(x, y, egg_colour(t/220u));
        g_egg_px=x; g_egg_py=y; g_egg_drawn=1;
    }
}
/* apply a mouse event to the UI. The cursor rides its own overlay layer (fb4) so a move
 * NEVER touches the menu layer -> no flash. Only real menu changes blit fb0. */
/* activate a playback control-bar button (by index PBB_*) — sets flags the feed loop acts on */
static void pb_activate(int idx){
    g_pb_lastui = now_ms();
    if(idx==PBB_STOP) g_pb_stop=1;
    else if(idx==PBB_RW || idx==PBB_FF){          /* seek back/forward ~6% (scrub by button) */
        int cur=(g_pb_total>0)?(int)((long long)g_pb_pos*1000/g_pb_total):0;
        cur += (idx==PBB_FF)?60:-60; if(cur<0)cur=0; if(cur>1000)cur=1000;
        g_pb_seek_pm=cur; g_pb_seek=1; g_pb_paused=1;
    }
    else if(idx==PBB_BACK){ g_pb_paused=1; g_pb_back=1; }
    else if(idx==PBB_PP){ if(g_pb_ended) g_pb_replay=1; else g_pb_paused=!g_pb_paused; }
    else if(idx==PBB_STEP){ if(!g_pb_ended){ g_pb_paused=1; g_pb_step=1; } }
    else if(idx==PBB_SPEED) g_pb_speed=(g_pb_speed>=4)?2:(g_pb_speed==2)?1:4;
    pb_bar_draw(0);
}
/* physical-button / MCU keys during playback: -/+ move the highlight, M activates, X exits */
static void pb_btnkey(char k){
    g_pb_lastui = now_ms();
    if(k=='-'){ g_pb_sel=(g_pb_sel==0)?PBC_N-1:g_pb_sel-1; pb_bar_draw(0); }
    else if(k=='+'){ g_pb_sel=(g_pb_sel+1)%PBC_N; pb_bar_draw(0); }
    else if(k=='M') pb_activate(g_pb_sel);
    else if(k=='X') g_pb_stop=1;
}
static void ui_mouse_apply(int moved, int lclick, int rclick){
    if(!g_ui_fb) return;
    if(g_pb_active){                       /* playback: drive the on-screen control bar */
        if(lclick){
            g_pb_lastui = now_ms();
            int pm = ui_pbbar_seek(g_mx, g_my);
            if(pm>=0){ g_pb_scrub=1; g_pb_seek_pm=pm; g_pb_seek=1; }   /* grab the progress bar -> scrub */
            else { int idx=ui_pbbar_hit(g_mx,g_my);
                   if(idx>=0){ g_pb_sel=idx; pb_activate(idx); } else pb_bar_draw(0); }
        }
        if(rclick) g_pb_stop=1;            /* right-click anywhere = stop -> back to the Playback grid */
        if(moved){
            g_pb_lastui = now_ms();
            if(g_pb_scrub){                /* dragging the progress bar */
                if(g_mbtn&1){ int pm=ui_pbbar_seek(g_mx,g_my); if(pm>=0){ g_pb_seek_pm=pm; g_pb_seek=1; } }
                else { g_pb_scrub=0; pb_bar_draw(0); }   /* released */
            }
            cursor_move(g_mx, g_my);
        }
        return;
    }
    g_mseen = 1;
    int structural = 0, hovered = 0;
    if(lclick){ ui_snd('M'); int a = ui_mouse(&g_ui, g_mx, g_my, 1); if(a) ui_apply(a); structural = 1; }
    if(rclick){ ui_snd('X'); ui_mouse(&g_ui, g_mx, g_my, 2); structural = 1; }
    if(moved && !structural){ if(ui_hover(&g_ui, g_mx, g_my)) hovered = 1; }
    if(structural){
        ui_refresh(); ui_render(&g_ui);
        fb_blit(0,0,(int)g_fbw,(int)g_fbh);
    } else if(hovered){
        ui_render_menu_only(&g_ui);
        ui_geom g; ui_menu_geom(&g_ui, &g);
        fb_blit(g.px-2, g.py-2, g.pw+4, g.ph+4);
    }
    if(moved) cursor_move(g_mx, g_my);   /* just the overlay layer + a pan flip */
}
static unsigned g_last_move_ms = 0;
static void ui_mouse_poll(void){
    if(!g_ui_fb) return;
    if(g_egg_on) return;      /* a mouse move would repaint fb0 straight over the animation */
    int ev = mouse_poll((int)g_fbw-1, (int)g_fbh-1);
    if(!ev) return;
    if(ev & MEV_WHEEL){                       /* scroll wheel -> scroll the playback list */
        int d = mouse_wheel();
        if(d && !g_pb_active && g_ui.open && g_ui.menu==MENU_PLAYBACK){
            if(ui_pb_scroll(&g_ui, d)) ui_paint();   /* grid is full-screen -> full redraw */
        }
    }
    /* clicks are rare — apply immediately (mouse_poll already put g_mx/g_my at the click),
     * and snap the cursor to the click point so what you see is what you clicked. */
    if(ev & (MEV_LEFT|MEV_RIGHT)){
        cursor_move(g_mx, g_my); g_last_move_ms = now_ms();
        ui_mouse_apply(0, ev&MEV_LEFT, ev&MEV_RIGHT);
    }
    /* the driver streams ~200 positions/sec; the display is 60Hz. Panning the cursor
     * layer on every one ping-pongs the double buffer faster than refresh (ghosting).
     * Rate-limit the cursor+hover update to ~60Hz — g_mx/g_my already hold the latest. */
    if(ev & MEV_MOVE){
        unsigned now = now_ms();
        if(now - g_last_move_ms >= 16){ ui_mouse_apply(1, 0, 0); g_last_move_ms = now; }
    }
}
/* keyboard (USB, via the console VT): arrow keys drive the SAME actions the MCU buttons will —
 * Up/Left=-, Down/Right=+, Enter=M (select / open menu from live), Esc=X (back). Lets the user
 * test the button-based navigation before the physical buttons exist. */
/* ---------------- SoC GPIO access (buzzer) ----------------
 * The front-panel daughterboard (8 buttons, 5 LEDs, IR on an 18-pin header) is NOT used:
 * its switches are worn and fire on their own, so all of that code was removed. The RE of
 * it is preserved in docs/FRONT_PANEL.md if it is ever revisited.
 *
 * What remains is the buzzer, which is on the MAIN board, reached the same way the vendor
 * driver reaches it: a masked write to a GPIO data register. Keep the mapping tiny — one
 * block is all the buzzer needs. */
#define GPIO_NBLK 1
static const unsigned GPIO_BLK[GPIO_NBLK] = { 0x20150000u };   /* buzzer lives here */
static unsigned char *g_gpio[GPIO_NBLK];
static int g_gpio_ready = 0;
static void gpio_map(void){
    int fm = (int)sys_open("/dev/mem", O_RDWR|O_SYNC, 0), i;
    if(fm < 0) return;
    for(i=0;i<GPIO_NBLK;i++){
        unsigned char *m = (unsigned char*)sys_mmap2(0, 0x1000, PROT_READ|PROT_WRITE,
                                                     MAP_SHARED, fm, GPIO_BLK[i]>>12);
        g_gpio[i] = ((long)m == -1 || (long)m <= 0) ? 0 : m;
    }
    sys_close(fm);
    g_gpio_ready = 1;
}
/* read a mapped GPIO register; base must be one of GPIO_BLK */
static unsigned gpio_rd(unsigned base, unsigned off){
    int i; for(i=0;i<GPIO_NBLK;i++) if(GPIO_BLK[i]==base)
        return g_gpio[i] ? *(volatile unsigned*)(g_gpio[i]+off) : 0xffffffffu;
    return 0xffffffffu;
}
static void gpio_wr(unsigned base, unsigned off, unsigned val){
    int i; for(i=0;i<GPIO_NBLK;i++) if(GPIO_BLK[i]==base && g_gpio[i]){
        *(volatile unsigned*)(g_gpio[i]+off) = val; return; }
}
/* Set one data bit. The Hi3515 GPIO data registers are address-masked: the write address
 * carries the bit mask in addr[9:2], so writing at offset (1<<bit)<<2 touches only that
 * bit and needs no read-modify-write (and cannot race the other bits). */
static void gpio_bit(unsigned base, int bit, int high){
    gpio_wr(base, (unsigned)(1<<bit) << 2, high ? 0xffu : 0x00u);
}
/* direction register is a plain read-modify-write at +0x400; 1 = output */
static void gpio_dir(unsigned base, int bit, int out){
    unsigned d = gpio_rd(base, 0x400);
    if(d == 0xffffffffu) return;
    d = out ? (d | (1u<<bit)) : (d & ~(1u<<bit));
    gpio_wr(base, 0x400, d);
}

/* ================= buzzer =================
 * From tl_R9508.ko's buzz_control(): hs3515_wr(0x20150200, on ? 0x80 : 0). Offset 0x200 is
 * the masked-data address for bit 7, so the buzzer is block 0x20150000 bit 7, active high.
 * (A whole-chip baseline shows that bit sitting low = silent, which agrees.)
 *
 * Pins deliberately NOT touched, from the same module: 0x20180000 bit 5 is rs485_control
 * (the rear PTZ transmit enable) and 0x20160000 bit 7 is key-matrix row 0. power_control /
 * screen_control only act when hardware_type == 0x68, so they are inert on this board, but
 * their pins (0x20150000 bit 4, 0x20180000 bit 0) are avoided anyway. */
#define BUZZ_BLK 0x20150000u
#define BUZZ_BIT 7
static void buzz_set(int on){
    if(!g_gpio_ready) gpio_map();
    gpio_dir(BUZZ_BLK, BUZZ_BIT, 1);          /* output first, then data — as the vendor does */
    gpio_bit(BUZZ_BLK, BUZZ_BIT, on ? 1 : 0);
}
/* Non-blocking beep pattern: up to 4 on/off segments, advanced from the 1 Hz-ish main loop
 * tick at 20 ms granularity. Used for record start/stop so nothing stalls the encoder. */
static unsigned char g_beep_seq[8];      /* alternating on,off,on,off... durations /20ms */
static int      g_beep_n = 0, g_beep_i = 0;
static unsigned g_beep_next = 0;
static void beep(const unsigned char *seq, int n){
    int i; if(n > 8) n = 8;
    for(i=0;i<n;i++) g_beep_seq[i] = seq[i];
    g_beep_n = n; g_beep_i = 0; g_beep_next = now_ms();
}
static void beep_tick(void){
    if(g_beep_i >= g_beep_n){ if(g_beep_n){ buzz_set(0); g_beep_n = 0; } return; }
    unsigned now = now_ms();
    if(now < g_beep_next) return;
    buzz_set(!(g_beep_i & 1));                 /* even index = on, odd = off */
    g_beep_next = now + (unsigned)g_beep_seq[g_beep_i] * 20u;
    g_beep_i++;
}
/* Blocking square-wave tone. Only useful if the buzzer is a passive transducer; a self-
 * oscillating one ignores the frequency and just sounds while driven. Kept short because
 * it busy-waits and therefore stalls the encoder drain for its duration. */
/* Iterations of the delay loop below that take one microsecond on this SoC. MEASURED,
 * not guessed: the original 8 was a guess and it was out by a factor of five, so every
 * note played at ~19% of its written length and ~5x its written pitch — the note tables
 * were decorative. Timed by driving TONE over the control port and differencing a 400 ms
 * request against a 100 ms one to cancel the round trip. If tones ever drift again,
 * re-measure the same way rather than nudging this by ear. */
#define BUZZ_LOOP_PER_US 42u
static void buzz_tone(unsigned hz, unsigned ms){
    if(hz < 50) hz = 50; if(hz > 5000) hz = 5000;
    if(ms > 400) ms = 400;
    unsigned half_us = 500000u / hz;           /* half period */
    unsigned cycles  = (ms * 1000u) / (half_us * 2u);
    unsigned i;
    if(!g_gpio_ready) gpio_map();
    gpio_dir(BUZZ_BLK, BUZZ_BIT, 1);
    for(i=0;i<cycles;i++){
        volatile unsigned z;
        gpio_bit(BUZZ_BLK, BUZZ_BIT, 1);
        for(z=0; z<half_us*BUZZ_LOOP_PER_US; z++) ;
        gpio_bit(BUZZ_BLK, BUZZ_BIT, 0);
        for(z=0; z<half_us*BUZZ_LOOP_PER_US; z++) ;
        if(g_wdt>=0 && !(i & 0x3f)) sys_write(g_wdt,"w",1);
    }
    buzz_set(0);
}

/* ---- melodies ----
 * The buzzer is a passive transducer (verified by ear: 500 Hz and 3 kHz sound different),
 * so it can play actual pitches rather than just rhythm.
 *
 * buzz_tone() busy-waits, which stalls the encoder drain for its duration, so a melody is
 * NOT one long blocking burst. Each note is a short blocking burst fired from the main-loop
 * tick with a gap between, so the stall is bounded by a single note (<=120 ms) and
 * pump_encode() runs in between. The VENC ring holds roughly a second at our bitrate, so
 * there is plenty of margin. */
/* rising major arpeggio C5-E5-G5-C6 — reads as "go" */
const note_t MEL_REC_START[] = { {523,70},{659,70},{784,70},{1047,120} };
/* the same idea inverted for "stopped" */
const note_t MEL_REC_STOP[]  = { {784,70},{659,70},{523,120} };
/* two-tone chirp for anything that wants attention */
static const note_t MEL_ALERT[]     = { {1500,80},{0,40},{1500,80} };
/* ---- UI feedback: deliberately tiny so the encoder never notices ----
 * Each note is a separate blocking burst fired from its own main-loop tick, so the worst
 * case here is a 28 ms stall — an order of magnitude under the ~1 s the VENC ring holds.
 * The three are pitched to be told apart without looking: a flat tick for moving, rising
 * for going in, falling for coming back out. */
const note_t MEL_NAV[]   = { {1200,22} };                    /* move highlight  */
const note_t MEL_ENTER[] = { {1000,26},{1600,28} };          /* select / go in  */
const note_t MEL_BACK[]  = { {1500,26},{900,28} };           /* exit / go back  */
const note_t MEL_EDIT[]  = { {1800,16} };                    /* value +/-       */
/* One place every input source routes through, so panel buttons, the USB keyboard, the
 * mouse and the network UI command all sound identical for the same action. */
static void ui_snd(char k){
    if(!g_ui_snd) return;
    switch(k){
        case '-': case '+': melody(MEL_NAV,   MEL_NAV_N);   break;   /* move highlight */
        case 'M':           melody(MEL_ENTER, MEL_ENTER_N); break;   /* select / go in */
        case 'X':           melody(MEL_BACK,  MEL_BACK_N);  break;   /* back / exit    */
        case 'e':           melody(MEL_EDIT,  MEL_EDIT_N);  break;   /* value changed  */
        default: break;
    }
}
static const note_t *g_mel = 0;
static int      g_mel_n = 0, g_mel_i = 0;
static unsigned g_mel_next = 0;
static void melody(const note_t *m, int n){
    g_mel = m; g_mel_n = n; g_mel_i = 0; g_mel_next = now_ms();
}
static void melody_tick(void){
    if(!g_mel || g_mel_i >= g_mel_n){ g_mel = 0; return; }
    if(now_ms() < g_mel_next) return;
    { const note_t *n = &g_mel[g_mel_i];
      if(n->hz){ buzz_tone(n->hz, n->ms);      /* short blocking burst — bit-banged */
                 g_mel_next = now_ms() + 12; } /* small gap so notes stay distinct */
      /* A rest used to msleep(), which blocked the main loop to produce silence —
       * pointless, and it starved everything else for the duration. Just schedule the
       * next note instead: the loop keeps drawing and draining the encoder through it,
       * which is what makes a long tune survivable under the easter egg's animation. */
      else { buzz_set(0); g_mel_next = now_ms() + n->ms; } }
    g_mel_i++;
}

/* Field hotkey (keyboard R): flip recording without navigating anywhere.
 * Targets the channel on the monitor when that channel actually has an encoder, else
 * ch0 — with enc_channels=1 the other three cannot be recorded at all, and silently
 * doing nothing would be the worst possible behaviour with no screen to check.
 * Feedback is the buzzer: rec_start/rec_stop play their melodies, and the HUD shows
 * 'armed' immediately (recording itself begins at the next keyframe). */
static void rec_key_toggle(void){
    int ch = (g_dispch >= 0 && g_dispch < (int)g_enc_nch) ? g_dispch : 0;
    int on = rec_on[ch] || rec_want[ch];
    rec_want[ch] = on ? 0 : 1;
    ui_refresh();
    puts_("[dvr] REC key -> ch"); putu((unsigned)ch); puts_(on?" stop\n":" start\n");
}
static void ui_kbd_poll(void){
    int ev=kbd_poll();
    if(kbd_seq_ends(EGG_WORD)){ kbd_seq_clear(); egg_start(); return; }
    if(g_egg_on) return;              /* the show owns the screen — swallow keys until it ends */
    if(!ev) return;
    /* before the g_ui_fb gate: recording must not depend on the OSD being up */
    if(ev==KEV_REC){ rec_key_toggle(); if(g_ui_fb && !g_pb_active) ui_paint(); return; }
    if(!g_ui_fb) return;
    char k=(ev==KEV_PREV)?'-':(ev==KEV_NEXT)?'+':(ev==KEV_SEL)?'M':'X';
    ui_snd(k);
    if(g_pb_active){ pb_btnkey(k); return; }   /* playback bar: -/+ move highlight, M activate, X exit */
    int act=ui_key(&g_ui,k); if(act) ui_apply(act);
    ui_paint();
}
/* map a control token to a UI key: -/+/M/X or UP/DOWN/ENTER/MENU/EXIT/BACK/OPEN/CLOSE */
static char ui_tok_key(const char *t){
    if(tok_eq((char*)t,"UP")   || t[0]=='-') return '-';
    if(tok_eq((char*)t,"DOWN") || t[0]=='+') return '+';
    if(tok_eq((char*)t,"ENTER")|| tok_eq((char*)t,"MENU") || t[0]=='M') return 'M';
    if(tok_eq((char*)t,"EXIT") || tok_eq((char*)t,"BACK") || t[0]=='X') return 'X';
    return 0;
}

/* execute one command line; write reply text to rfd */
static void ctl_exec(char *line, int rfd){
    int c;
    if(tok_eq(line,"PING")){ wrs(rfd,"PONG\n"); return; }
    if(tok_eq(line,"REC") || tok_eq(line,"STOP")){
        int on = tok_eq(line,"REC");
        const char *a = tok_next(line);
        if(tok_eq(a,"ALL")){ for(c=0;c<NCH;c++) rec_want[c]=on; }
        else { c=k_atoi(a); if(c>=0&&c<NCH) rec_want[c]=on; }
        wrs(rfd,"OK\n"); return;
    }
    if(tok_eq(line,"STATUS")){
        char b[64]; int l=0;
        memcpy(b,"STATUS rec=",11); l=11;
        for(c=0;c<NCH;c++) b[l++] = rec_on[c]?'1':(rec_want[c]?'w':'0');
        memcpy(b+l," std=",5); l+=5;
        if(g_standard==2){ memcpy(b+l,"PAL",3); l+=3; }
        else if(g_standard==1){ memcpy(b+l,"NTSC",4); l+=4; }
        else { memcpy(b+l,"AUTO",4); l+=4; }
        b[l++]='\n'; if(rfd>=0) sys_write(rfd,b,l); return;
    }
    if(tok_eq(line,"STD")){
        const char *a = tok_next(line); int ns;
        /* A standard change is applied at pipeline init, i.e. by respawning — which would
         * end the recording without saying so. Same rule as DEL. */
        if(rec_busy()){ wrs(rfd,"ERR recording\n"); return; }
        if(tok_eq(a,"PAL")) ns=2; else if(tok_eq(a,"NTSC")) ns=1; else ns=0;
        g_standard=ns; g_std_changed=1;
        /* persist so the respawn applies it (standard is set at pipeline init) */
        int fd2=(int)sys_open("/root/rec/std", O_WRONLY|O_CREAT|O_TRUNC, 0644);
        if(fd2>=0){ char c=(char)('0'+ns); sys_write(fd2,&c,1); sys_close(fd2); }
        wrs(rfd,"OK\n"); return;
    }
    if(tok_eq(line,"SHOW")){    /* SHOW <ch> — switch which VI channel the VGA displays */
        const char *a=tok_next(line); int ch=k_atoi(a);
        if(rec_busy()){ wrs(rfd,"ERR recording\n"); return; }
        if(ch>=0 && ch<NCH) switch_channel(ch);
        char b[24]; int l=0; memcpy(b,"SHOW ",5); l=5; l+=u2s(b+l,(unsigned)g_dispch); b[l++]='\n';
        if(rfd>=0) sys_write(rfd,b,l); return;
    }
    if(tok_eq(line,"PLAY")){    /* PLAY <path> — decode+show a recording full-screen on the VGA */
        const char *a=tok_next(line);
        if(*a){ int r=play_file(a); wrs(rfd, r==0?"PLAY done\n":"PLAY err\n"); }
        else wrs(rfd,"PLAY ?\n");
        return;
    }
    if(tok_eq(line,"PBSTOP")){ g_pb_stop=1; wrs(rfd,"OK\n"); return; }   /* abort playback */
    if(tok_eq(line,"PBPAUSE")){ g_pb_lastui=now_ms(); g_pb_paused=1; if(g_pb_active)pb_bar_draw(0); wrs(rfd,"OK\n"); return; }
    if(tok_eq(line,"PBPLAY")){ g_pb_lastui=now_ms(); if(g_pb_ended)g_pb_replay=1; else g_pb_paused=0; if(g_pb_active)pb_bar_draw(0); wrs(rfd,"OK\n"); return; }
    if(tok_eq(line,"PBTOGGLE")){ g_pb_lastui=now_ms(); if(g_pb_ended)g_pb_replay=1; else g_pb_paused=!g_pb_paused; if(g_pb_active)pb_bar_draw(0); wrs(rfd,"OK\n"); return; }
    if(tok_eq(line,"PBREPLAY")){ g_pb_lastui=now_ms(); g_pb_replay=1; wrs(rfd,"OK\n"); return; }  /* restart */
    if(tok_eq(line,"PBSTEP")){ g_pb_lastui=now_ms(); g_pb_paused=1; g_pb_step=1; wrs(rfd,"OK\n"); return; }  /* one frame forward */
    if(tok_eq(line,"PBBACK")){ g_pb_paused=1; g_pb_back=1; wrs(rfd,"OK\n"); return; }  /* one frame back */
    if(tok_eq(line,"PBSEEK")){ const char *a=tok_next(line);   /* PBSEEK <permille 0..1000> — scrub */
        int pm=k_atoi(a); if(pm<0)pm=0; if(pm>1000)pm=1000; g_pb_lastui=now_ms();
        g_pb_seek_pm=pm; g_pb_seek=1; g_pb_paused=1; wrs(rfd,"OK\n"); return; }
    if(tok_eq(line,"PBSPEED")){ const char *a=tok_next(line);   /* PBSPEED <4|2|1> = 1x/half/quarter */
        int v=k_atoi(a); g_pb_speed=(v==1||v==2||v==4)?(unsigned)v:4; if(g_pb_active)pb_bar_draw(0);
        wrs(rfd,"OK\n"); return; }
    if(tok_eq(line,"SHOT")){ do_shot(rfd); return; }   /* screenshot the VGA output */
    if(tok_eq(line,"UI")){          /* UI <UP|DOWN|ENTER|EXIT|-|+|M|X> — drive the on-screen menu */
        ui_ensure_fb();
        if(!g_ui_fb){ wrs(rfd,"ERR fb\n"); return; }
        const char *a = tok_next(line);
        char k = ui_tok_key(a);
        if(g_pb_active){ if(k) pb_btnkey(k); wrs(rfd,"OK\n"); return; }   /* playback: -/+/M/X drive the bar */
        if(k){ int act = ui_key(&g_ui, k); if(act) ui_apply(act); }
        ui_paint();
        { char b[80]; int l=0; memcpy(b,"UI open=",8); l=8; b[l++]='0'+(g_ui.open?1:0);
          memcpy(b+l," menu=",6); l+=6; l+=u2s(b+l,(unsigned)g_ui.menu);
          memcpy(b+l," sel=",5); l+=5; l+=u2s(b+l,(unsigned)g_ui.sel);
          memcpy(b+l," edit=",6); l+=6; b[l++]='0'+(g_ui.editing?1:0); b[l++]='\n';
          if(rfd>=0) sys_write(rfd,b,l); }
        return;
    }
    if(tok_eq(line,"UIFB")){        /* report fb0 geometry (clamp discriminator) */
        ui_ensure_fb();
        char b[96]; int l=0; memcpy(b,"UIFB ",5); l=5;
        l+=u2s(b+l,g_fbw); b[l++]='x'; l+=u2s(b+l,g_fbh);
        memcpy(b+l," virt=",6); l+=6; l+=u2s(b+l,g_fbvw); b[l++]='x'; l+=u2s(b+l,g_fbvh);
        memcpy(b+l," bpp=",5); l+=5; l+=u2s(b+l,g_fbbpp);
        memcpy(b+l," stride=",8); l+=8; l+=u2s(b+l,g_fbstride);
        memcpy(b+l," len=",5); l+=5; l+=u2s(b+l,g_fblen); b[l++]='\n';
        if(rfd>=0) sys_write(rfd,b,l); return;
    }
    if(tok_eq(line,"MOUSE")){       /* MOUSE <x> <y> [btn] — inject a mouse event (test w/o hw) */
        ui_ensure_fb();
        if(!g_ui_fb){ wrs(rfd,"ERR fb\n"); return; }
        const char *a=tok_next(line); int x=k_atoi(a);
        const char *b=tok_next(a);   int y=k_atoi(b);
        const char *c=tok_next(b);   int btn=k_atoi(c);
        g_mx = x<0?0:(x>=(int)g_fbw?(int)g_fbw-1:x);
        g_my = y<0?0:(y>=(int)g_fbh?(int)g_fbh-1:y);
        g_accx=(long)g_mx*8; g_accy=(long)g_my*8;   /* keep the accumulator in sync */
        g_mseen = 1;
        ui_mouse_apply(1, btn==1, btn==2);
        { char m[80]; int l=0; memcpy(m,"MOUSE x=",8); l=8; l+=u2s(m+l,(unsigned)g_mx);
          memcpy(m+l," y=",3); l+=3; l+=u2s(m+l,(unsigned)g_my);
          memcpy(m+l," open=",6); l+=6; m[l++]='0'+(g_ui.open?1:0);
          memcpy(m+l," menu=",6); l+=6; l+=u2s(m+l,(unsigned)g_ui.menu);
          memcpy(m+l," sel=",5); l+=5; l+=u2s(m+l,(unsigned)g_ui.sel); m[l++]='\n';
          if(rfd>=0) sys_write(rfd,m,l); }
        return;
    }
    if(tok_eq(line,"UIFB4")){       /* report cursor-overlay (fb4) open state + geometry */
        char b[80]; int l=0; memcpy(b,"UIFB4 fd=",9); l=9; l+=u2s(b+l,(unsigned)(g_cofd&0x7fff));
        if(g_cofd<0){ memcpy(b+l," OPEN-FAIL",10); l+=10; }
        else { memcpy(b+l," ",1); l++; l+=u2s(b+l,g_cow); b[l++]='x'; l+=u2s(b+l,g_coh);
               memcpy(b+l," stride=",8); l+=8; l+=u2s(b+l,g_costride); }
        b[l++]='\n'; if(rfd>=0) sys_write(rfd,b,l); return;
    }
    if(tok_eq(line,"MRAW")){        /* hex-dump the last raw /dev/input/mice read (packet framing) */
        char m[128]; int l=0; memcpy(m,"MRAW n=",7); l=7; l+=u2s(m+l,(unsigned)g_mrawn); m[l++]=':';
        int k; const char *hx="0123456789abcdef";
        for(k=0;k<g_mrawn && l<120;k++){ m[l++]=' '; m[l++]=hx[(g_mraw[k]>>4)&0xf]; m[l++]=hx[g_mraw[k]&0xf]; }
        m[l++]='\n'; if(rfd>=0) sys_write(rfd,m,l); return;
    }
    if(tok_eq(line,"MDBG")){        /* report raw mouse activity (diagnose the real mouse) */
        char m[128]; int l=0; memcpy(m,"MDBG fd=",8); l=8; l+=u2s(m+l,(unsigned)(g_mfd&0x7fff));
        memcpy(m+l," reads=",7); l+=7; l+=u2s(m+l,(unsigned)g_mdbg_reads);
        memcpy(m+l," pk=",4); l+=4; l+=u2s(m+l,(unsigned)g_mdbg_pk);
        memcpy(m+l," ovf=",5); l+=5; l+=u2s(m+l,(unsigned)g_mdbg_ovf);
        memcpy(m+l," b=",3); l+=3; l+=u2s(m+l,(unsigned)(g_mdbg_b&0xff));
        memcpy(m+l," dx=",4); l+=4; if(g_mdbg_dx<0){m[l++]='-';l+=u2s(m+l,(unsigned)(-g_mdbg_dx));}else l+=u2s(m+l,(unsigned)g_mdbg_dx);
        memcpy(m+l," dy=",4); l+=4; if(g_mdbg_dy<0){m[l++]='-';l+=u2s(m+l,(unsigned)(-g_mdbg_dy));}else l+=u2s(m+l,(unsigned)g_mdbg_dy);
        memcpy(m+l," xy=",4); l+=4; l+=u2s(m+l,(unsigned)g_mx); m[l++]=','; l+=u2s(m+l,(unsigned)g_my);
        m[l++]='\n'; if(rfd>=0) sys_write(rfd,m,l); return;
    }
    if(tok_eq(line,"VOF")){         /* dump VO GetScreenFrame info words (find an advancing frame counter) */
        char m[320]; int l=0; const char *hx="0123456789abcdef";
        if(g_fvo<0){ wrs(rfd,"VOF novo\n"); return; }
        unsigned f[40]; memset(f,0,sizeof(f));
        if(ioc(g_fvo,0xc0984f0f,f)!=0){ wrs(rfd,"VOF err\n"); return; }
        int k; for(k=0;k<28;k++){ unsigned v=f[k]; int s;
            m[l++]='['; if(k>=10){m[l++]='0'+k/10;} m[l++]='0'+k%10; m[l++]=']';
            for(s=28;s>=0;s-=4) m[l++]=hx[(v>>s)&0xf]; m[l++]=' '; }
        m[l++]='\n'; ioc(g_fvo,0x40984f10,f);
        if(rfd>=0) sys_write(rfd,m,l); return;
    }
    if(tok_eq(line,"KBDBG")){       /* report keyboard reads/last keycode (verify the VT keyboard works) */
        char m[96]; int l=0; memcpy(m,"KBDBG fd=",9); l=9; l+=u2s(m+l,(unsigned)(g_kfd&0x7fff));
        memcpy(m+l," reads=",7); l+=7; l+=u2s(m+l,(unsigned)g_kbd_reads);
        memcpy(m+l," keys=",6); l+=6; l+=u2s(m+l,(unsigned)g_kbd_keys);
        memcpy(m+l," last=",6); l+=6; if(g_kbd_last<0){m[l++]='-';m[l++]='1';} else l+=u2s(m+l,(unsigned)g_kbd_last);
        memcpy(m+l," oldmode=",9); l+=9; l+=u2s(m+l,(unsigned)g_kbd_oldmode);
        m[l++]='\n'; if(rfd>=0) sys_write(rfd,m,l); return;
    }
    if(tok_eq(line,"MSENS")){       /* MSENS <n> — mouse pointer speed (1/8 units, 8=1:1) */
        const char *a=tok_next(line); int n=k_atoi(a);
        if(n>=1 && n<=20) g_msens=n;
        char m[32]; int l=0; memcpy(m,"MSENS ",6); l=6; l+=u2s(m+l,(unsigned)g_msens); m[l++]='\n';
        if(rfd>=0) sys_write(rfd,m,l); return;
    }
    if(tok_eq(line,"UICLOSE")){ ui_ensure_fb(); g_ui.open=0; ui_paint(); wrs(rfd,"OK\n"); return; }
    if(tok_eq(line,"UIPAINT")){ ui_ensure_fb(); ui_paint(); wrs(rfd,"OK\n"); return; }
    if(tok_eq(line,"UISHOW")){      /* UISHOW <0|1> — composite the OSD layer over the video */
        ui_ensure_fb();
        const char *a = tok_next(line);
        int on = k_atoi(a);
        if(on){ fb_alpha_opaque(); fb_show(1); } else { fb_show(0); }
        wrs(rfd, g_ui_fb ? "OK\n" : "ERR fb\n"); return;
    }
    if(tok_eq(line,"UITEST")){                          /* draw a full-screen test OSD + dump fb */
        /* fb0's buffer supports 1280x1024 (app.out-proven); other sizes may not grow it */
        if(fb_open_mode(1280,1024)!=0){ wrs(rfd,"ERR fb\n"); return; }
        int W=(int)g_fbw, H=(int)g_fbh;
        fb_fill(fb_rgb(0,0,0));                          /* transparent-black clear */
        fb_border(0,0,W,H, 4, fb_rgb(0,255,0));          /* green full-screen border */
        fb_rect(0,0,W,40, fb_rgb(0,90,140));             /* top status bar */
        fb_rect(8,8,24,24, fb_rgb(255,0,0));             /* TL red marker */
        fb_rect(W-32,8,24,24, fb_rgb(255,255,0));        /* TR yellow marker */
        fb_rect(8,H-32,24,24, fb_rgb(0,255,255));        /* BL cyan marker */
        fb_rect(W-32,H-32,24,24, fb_rgb(255,0,255));     /* BR magenta marker */
        fb_blit(0,0,W,H);                                /* push shadow -> real fb */
        fb_show(1); fb_alpha_opaque();
        fb_dump("/root/rec/fb.raw");
        { char b[64]; int l=0; memcpy(b,"UITEST ",7); l=7;
          l+=u2s(b+l,g_fbw); b[l++]='x'; l+=u2s(b+l,g_fbh);
          memcpy(b+l," bpp=",5); l+=5; l+=u2s(b+l,g_fbbpp);
          memcpy(b+l," stride=",8); l+=8; l+=u2s(b+l,g_fbstride); b[l++]='\n';
          if(rfd>=0) sys_write(rfd,b,l); }
        return;
    }
    if(tok_eq(line,"REG")){     /* REG <reg> [val] — read/write a TW2866 register (decimal) */
        const char *a = tok_next(line); int reg = k_atoi(a);
        const char *b = tok_next(a);
        if(*b && g_ftw>=0){ tw_write(g_ftw, reg, k_atoi(b)); }
        int rv = (g_ftw>=0) ? tw_read(g_ftw, reg) : -1;
        char m[48]; int l=0; memcpy(m,"REG ",4); l=4; l+=u2s(m+l,(unsigned)reg);
        m[l++]='='; l+=u2s(m+l,(unsigned)(rv&0xff)); m[l++]='\n';
        if(rfd>=0) sys_write(rfd,m,l); return;
    }
    /* ---- machine-readable status/inventory for the web UI (docs/CONTROL_PROTOCOL.md).
     * These exist so webapp2 doesn't have to shell out over telnet for things the DVR
     * already knows; they touch no MPP state, so they're safe to poll. ---- */
    if(tok_eq(line,"INFO")){
        char b[320]; int l=0;
        #define ADDS(s) { const char *q=(s); while(*q) b[l++]=*q++; }
        #define ADDU(v) { l+=u2s(b+l,(unsigned)(v)); }
        ADDS("INFO ver="); ADDU(DVR_VER);
        ADDS(" up=");      ADDU(now_sec()-g_start_sec);
        ADDS(" enc=");     ADDU(g_enc_nch);
        ADDS(" std=");     ADDS(g_standard==2?"PAL":(g_standard==1?"NTSC":"AUTO"));
        ADDS(" res=");     ADDU(g_ewidth); b[l++]='x'; ADDU(g_eheight);
        ADDS(" fps=");     ADDU(g_fps);
        ADDS(" gop=");     ADDU(g_gop);
        ADDS(" rc=");      ADDU(g_rc_mode);
        ADDS(" qp=");      ADDU(g_qp);
        ADDS(" br=");      ADDU(g_bitrate);
        ADDS(" disk=");    ADDU(free_mb("/root/rec/a1"));
        ADDS(" ch=");      ADDU(g_dispch);
        ADDS(" rec=");     for(c=0;c<NCH;c++) b[l++]=rec_on[c]?'1':(rec_want[c]?'w':'0');
        ADDS(" recmb=");   for(c=0;c<NCH;c++){ if(c) b[l++]=','; ADDU(rec_mb[c]); }
        ADDS(" recsec="); for(c=0;c<NCH;c++){ if(c) b[l++]=',';
                              ADDU(rec_on[c]?(unsigned)(now_sec()-rec_start_sec[c]):0u); }
        { int nc=0; for(c=0;c<MAXCLI;c++) if(g_cfd[c]>=0) nc++;
          ADDS(" cli="); ADDU(nc); }
        ADDS(" pb=");      b[l++]='0'+(g_pb_active?1:0);
        ADDS(" osd=");     b[l++]='0'+(g_ui.open?1:0);
        ADDS(" snd=");     b[l++]='0'+(g_ui_snd?1:0);
        ADDS(" packs=");   ADDU(total_packs);
        /* live-video health: pps = encoder frames/s (capture side), still = consecutive
         * seconds the VO video layer has not advanced, heals = freeze recoveries so far */
        ADDS(" pps=");     ADDU(g_pps);
        ADDS(" still=");   ADDU((unsigned)g_vw_same);
        ADDS(" heals=");   ADDU(g_vw_heals);
        ADDS(" froze=");   ADDU(g_vw_froze);
        /* RTC wall clock — recordings are named from it, so the web UI shows the
         * DEVICE time rather than the browser's (they drift; the RTC is the truth). */
        { char st[16]; rtc_stamp(st); ADDS(" time="); ADDS(st); }
        b[l++]='\n';
        #undef ADDS
        #undef ADDU
        if(rfd>=0) sys_write(rfd,b,l); return;
    }
    if(tok_eq(line,"VOSIG")){   /* is the VGA video layer actually advancing? (freeze diagnosis) */
        unsigned a=vo_frame_sig(); msleep(150); unsigned b=vo_frame_sig();
        char m[96]; int l=0; memcpy(m,"VOSIG a=",8); l=8; l+=u2s(m+l,a);
        memcpy(m+l," b=",3); l+=3; l+=u2s(m+l,b);
        memcpy(m+l," moving=",8); l+=8; m[l++]=((a||b)&&a!=b)?'1':'0';
        memcpy(m+l," still=",7); l+=7; l+=u2s(m+l,(unsigned)g_vw_same);
        memcpy(m+l," heals=",7); l+=7; l+=u2s(m+l,g_vw_heals);
        memcpy(m+l," pps=",5); l+=5; l+=u2s(m+l,g_pps); m[l++]='\n';
        if(rfd>=0) sys_write(rfd,m,l); return;
    }
    if(tok_eq(line,"RELIVE")){  /* force the freeze heal by hand: RELIVE [hard] */
        const char *a=tok_next(line);
        if(tok_eq(a,"HARD")) vo_force_relive(g_dispch); else vo_relive(g_dispch);
        g_vw_same=0; g_vw_stage=0;
        wrs(rfd,"OK\n"); return;
    }
    if(tok_eq(line,"VIKICK")){  /* try to restart a wedged VI channel without restarting anything else.
                                 * Diagnosed 2026-07-25: when the live feed freezes, the TW2866 is still
                                 * locked (reg0 VDLOSS=0, HLOCK/SLOCK/VLOCK set) and the VO still
                                 * composites — it is the SoC VI that stops delivering, which starves the
                                 * display AND the encoder at once. Reports each ioctl's rc so the
                                 * sequence can be refined against a real stall. */
        const char *a=tok_next(line); int hard = tok_eq(a,"HARD");
        int ch = g_dispch, fd = g_fvi[ch];
        char m[200]; int l=0; memcpy(m,"VIKICK ch=",10); l=10; l+=u2s(m+l,(unsigned)ch);
        if(fd<0){ memcpy(m+l," no-fd\n",7); l+=7; if(rfd>=0) sys_write(rfd,m,l); return; }
        unsigned vs[2]; int r1,r2,r3,r4,r5=0;
        vs[0]=0; vs[1]=(unsigned)ch; ioc(fd,0x40084919,vs);      /* unbind VI->VO dev0 */
        vs[0]=2; vs[1]=(unsigned)ch; ioc(fd,0x40084919,vs);      /* unbind dev2 */
        if(hard) r5 = iocv(fd, 0x4905, (unsigned)ch);            /* guess: DisableChn */
        r1 = ioc1(fd, 0x4004492e, (unsigned)ch);                 /* select dev/chn */
        r2 = ioc(fd, 0x40244908, VI_CHN);                        /* SetChnAttr */
        r3 = iocv(fd, 0x4904, (unsigned)ch);                     /* EnableChn */
        r4 = ioc1(fd, 0x4004490a, g_vifps);                      /* framerate */
        vs[0]=2; vs[1]=(unsigned)ch; ioc(fd,0x40084918,vs);      /* rebind dev2 */
        vs[0]=0; vs[1]=(unsigned)ch; ioc(fd,0x40084918,vs);      /* rebind dev0 */
        memcpy(m+l," sel=",5); l+=5; l+=u2s(m+l,(unsigned)r1);
        memcpy(m+l," attr=",6); l+=6; l+=u2s(m+l,(unsigned)r2);
        memcpy(m+l," en=",4); l+=4; l+=u2s(m+l,(unsigned)r3);
        memcpy(m+l," fps=",5); l+=5; l+=u2s(m+l,(unsigned)r4);
        if(hard){ memcpy(m+l," dis=",5); l+=5; l+=u2s(m+l,(unsigned)r5); }
        m[l++]='\n'; if(rfd>=0) sys_write(rfd,m,l); return;
    }
    if(tok_eq(line,"LIST")){        /* LIST — every dated .ts across a1..a4, newest first.
                                     * Reply: "LIST <n>", then n × "R <path> <bytes> <ts>", then "END". */
        int cnt=refresh_recordings(), i;
        char b[96]; int l=0;
        memcpy(b,"LIST ",5); l=5; l+=u2s(b+l,(unsigned)cnt); b[l++]='\n';
        if(rfd>=0) sys_write(rfd,b,l);
        for(i=0;i<cnt;i++){
            long sz=file_size(pb_path[i]); const char *s;
            l=0; b[l++]='R'; b[l++]=' ';
            for(s=pb_path[i]; *s; s++) b[l++]=*s;
            b[l++]=' '; l+=u2s(b+l,(unsigned)(sz>0?sz:0));
            b[l++]=' ';
            for(s=pb_key[i]; *s; s++) b[l++]=*s;
            b[l++]='\n';
            if(rfd>=0) sys_write(rfd,b,l);
        }
        wrs(rfd,"END\n"); return;
    }
    if(tok_eq(line,"DEL")){         /* DEL <path> — delete one recording */
        const char *a=tok_next(line);
        char pth[64]; int n=0, c2;
        while(a[n] && a[n]!=' ' && a[n]!='\r' && a[n]!='\n' && n<(int)sizeof(pth)-1){ pth[n]=a[n]; n++; }
        pth[n]=0;
        /* Refuse while anything is recording or playing: the file could be the one
         * being written or read, and neither path re-checks its fd mid-stream. */
        for(c2=0;c2<NCH;c2++) if(rec_on[c2]||rec_want[c2]){ wrs(rfd,"ERR recording\n"); return; }
        if(g_pb_active){ wrs(rfd,"ERR playing\n"); return; }
        if(!is_rec_path(pth)){ wrs(rfd,"ERR path\n"); return; }
        if(sys_unlink(pth)!=0){ wrs(rfd,"ERR path\n"); return; }
        refresh_recordings();       /* keep the OSD grid and thumbnails in step */
        wrs(rfd,"OK\n"); return;
    }
    if(tok_eq(line,"SND")){         /* SND <0|1> — menu feedback sounds on/off */
        const char *a=tok_next(line);
        if(*a){ g_ui_snd = k_atoi(a) ? 1 : 0;
            int fd2=(int)sys_open("/root/rec/snd",O_WRONLY|O_CREAT|O_TRUNC,0644);
            if(fd2>=0){ char cc=(char)('0'+g_ui_snd); sys_write(fd2,&cc,1); sys_close(fd2); } }
        wrs(rfd, g_ui_snd ? "SND on\n" : "SND off\n"); return;
    }
    if(tok_eq(line,"BUZZ")){        /* BUZZ [ms] — simple beep (non-blocking) */
        const char *a=tok_next(line); int ms=k_atoi(a); if(ms<20||ms>2000) ms=120;
        { unsigned char seq[2]; seq[0]=(unsigned char)(ms/20); seq[1]=1; beep(seq,2); }
        wrs(rfd,"OK\n"); return;
    }
    if(tok_eq(line,"TONE")){        /* TONE <hz> [ms] — square wave; blocks while sounding */
        const char *a=tok_next(line), *b=tok_next(a);
        unsigned hz=(unsigned)k_atoi(a), ms=(unsigned)k_atoi(b);
        if(!hz) hz=2000; if(!ms) ms=150;
        buzz_tone(hz, ms);
        { char m[64]; int l=0; memcpy(m,"TONE ",5); l=5; l+=u2s(m+l,hz);
          memcpy(m+l,"Hz ",3); l+=3; l+=u2s(m+l,ms); memcpy(m+l,"ms\n",3); l+=3;
          if(rfd>=0) sys_write(rfd,m,l); }
        return;
    }
    if(tok_eq(line,"TIME")){    /* TIME YYYY MM DD HH MM SS */
        const char *a = tok_next(line); struct rtc_time_ t; int v[6], i;
        for(i=0;i<6;i++){ v[i]=k_atoi(a); a=tok_next(a); }
        t.tm_year=v[0]-1900; t.tm_mon=v[1]-1; t.tm_mday=v[2];
        t.tm_hour=v[3]; t.tm_min=v[4]; t.tm_sec=v[5]; t.tm_wday=0; t.tm_yday=0; t.tm_isdst=-1;
        int rok = rtc_set(&t); clock_sync();   /* re-sync the uptime clock to the new time */
        wrs(rfd, rok==0 ? "OK\n" : "ERR\n"); return;
    }
    wrs(rfd,"ERR\n");
}
/* feed received bytes into a line buffer; dispatch on newline */
static void ctl_feed(char *buf, int *len, const unsigned char *d, int n, int rfd){
    int i;
    for(i=0;i<n;i++){
        char ch = (char)d[i];
        if(ch=='\n' || ch=='\r'){ if(*len>0){ buf[*len]=0; ctl_exec(buf, rfd); *len=0; } }
        else if(*len < 126) buf[(*len)++]=ch;
    }
}
static void ctl_init(void){
    int i; for(i=0;i<MAXCTL;i++){ ctl_cfd[i]=-1; ctl_len[i]=0; }
    ctl_lfd = net_listen(CTL_PORT);
    puts_("[dvr] control port :"); putu(CTL_PORT); puts_(ctl_lfd>=0?" up\n":" FAILED\n");
    /* MCU serial link (untested without hardware): 9600 8N1 raw, non-blocking */
    ser_fd = (int)sys_open("/dev/ttyAMA1", O_RDWR|O_NONBLOCK, 0);
    if(ser_fd>=0){
        unsigned char tio[60]; memset(tio,0,sizeof(tio));
        /* c_cflag at offset 8: B9600(0x0d)|CS8(0x30)|CREAD(0x80)|CLOCAL(0x800) */
        unsigned cflag = 0x0d|0x30|0x80|0x800;
        tio[8]=cflag&0xff; tio[9]=(cflag>>8)&0xff;
        sys_ioctl(ser_fd, 0x5402, tio);   /* TCSETS */
        puts_("[dvr] MCU serial /dev/ttyAMA1 open\n");
    }
}
static void ctl_poll(void){
    int i; unsigned char rb[256];
    if(ctl_lfd>=0){
        int nc = net_accept(ctl_lfd);
        if(nc>=0){ net_nonblock(nc);
            int slot=-1; for(i=0;i<MAXCTL;i++) if(ctl_cfd[i]<0){slot=i;break;}
            if(slot<0){ sys_close(nc); } else { ctl_cfd[slot]=nc; ctl_len[slot]=0; wrs(nc,"DVR READY\n"); }
        }
        for(i=0;i<MAXCTL;i++){
            if(ctl_cfd[i]<0) continue;
            long n = sys_read(ctl_cfd[i], rb, sizeof(rb));
            if(n>0) ctl_feed(ctl_line[i], &ctl_len[i], rb, (int)n, ctl_cfd[i]);
            else if(n==0){ sys_close(ctl_cfd[i]); ctl_cfd[i]=-1; }  /* peer closed */
        }
    }
    if(ser_fd>=0){
        long n = sys_read(ser_fd, rb, sizeof(rb));
        if(n>0) ctl_feed(ser_line, &ser_len, rb, (int)n, ser_fd);
    }
}
static void ctl_shutdown(void){
    int i; for(i=0;i<MAXCTL;i++) if(ctl_cfd[i]>=0){ sys_close(ctl_cfd[i]); ctl_cfd[i]=-1; }
    if(ctl_lfd>=0){ sys_close(ctl_lfd); ctl_lfd=-1; }
    if(ser_fd>=0){ sys_close(ser_fd); ser_fd=-1; }
}

/* Decode + display a recording full-screen on the VGA (VDEC -> VO), then return to live.
 * Swap = unbind the live VI from our VO window, route VDEC to the same window, feed H.264
 * access units (demuxed from the .ts) each followed by an AUD trailer to flush the frame.
 * See docs/PLAYBACK_PATH.md. Blocking; polls the control plane so PBSTOP / mouse still work. */
static unsigned char pb_au[600*1024];
/* draw the playback control bar over the video (OSD fb0). full=1 clears the whole OSD first
 * (entry / state change); full=0 refreshes just the bottom strip. Auto-hides while playing:
 * the bar shows when paused/ended or within 4s of an interaction, else it clears (media-player UX). */
static void pb_bar_draw(int full){
    if(!g_ui_fb) return;
    int strip = (int)g_fbh/3; if(strip<260) strip=260;
    int sy = (int)g_fbh - strip;
    if(full) fb_fill(FB_CLEAR); else fb_rect(0, sy, (int)g_fbw, strip, FB_CLEAR);
    int show = g_pb_paused || g_pb_ended || g_pb_scrub || (now_ms()-g_pb_lastui < 4000);
    if(show) ui_draw_pbbar(g_pb_paused, g_pb_ended, g_pb_speed, g_pb_cur_ms, g_pb_tot_ms, g_pb_title, g_pb_sel, g_pb_scrub);
    if(full) fb_blit(0,0,(int)g_fbw,(int)g_fbh); else fb_blit(0, sy, (int)g_fbw, strip);
}
/* ---------------- encoder-stall watchdog ----------------
 * The other half of the freeze problem, and a genuinely different fault: the VO video layer
 * keeps advancing (so the screen looks fine) while VENC stops emitting entirely — observed
 * live as `packs` frozen for ~3.5 minutes with `still=0` throughout, then recovering on its
 * own. That window is invisible without instrumentation and it is exactly the window in
 * which a recording would silently capture nothing, so it must self-heal.
 *
 * Heal: re-issue StartRecvPic on the stalled channel (harmless if it was already running),
 * then re-bind GRP->VENC, then give up and restart the pipeline. Threshold is deliberately
 * long — a few seconds of no packs is normal when the encoder is between GOPs under FIXQP. */
#define EW_STALL_SECS 12
static int      g_ew_zero = 0;      /* consecutive seconds with pps == 0 */
static int      g_ew_stage = 0;
static unsigned g_ew_heals = 0;
static void encoder_watchdog(void){
    if(g_pb_active || fve[0] < 0){ g_ew_zero = 0; return; }   /* VDEC owns the codec / no encoder */
    if(g_pps){ if(g_ew_zero >= EW_STALL_SECS) puts_("[dvr] encoder recovered\n");
               g_ew_zero = 0; g_ew_stage = 0; return; }
    if(++g_ew_zero < EW_STALL_SECS) return;

    g_ew_zero = 0; g_ew_heals++;
    puts_("[dvr] ENCODER STALLED (VENC BUF_EMPTY) — re-issuing StartRecvPic\n");
    /* ONLY StartRecvPic, which is idempotent and cannot make things worse.
     *
     * Two things this deliberately does NOT do, both learned the hard way on 2026-07-25:
     *  - re-assert the GRP->VENC bind while the pipeline is live. Doing that on a stalled
     *    encoder appears to poison it rather than restart it.
     *  - escalate to g_restart. Our "restart" is only a process respawn; the MPP drivers
     *    keep their state, so a restart inherits the broken pipeline and the encoder comes
     *    up dead — two frames then permanent BUF_EMPTY. That turns a transient stall into
     *    an endless restart loop, which is far worse than the stall. Only a real kernel
     *    reboot clears MPP, and this watchdog must never decide to do that on its own.
     * If StartRecvPic doesn't take, we log it and leave the box alone; `heals=`/`pps=` in
     * INFO make the condition visible, and the operator can reboot. */
    { int c;
      for(c=0; c<(int)g_enc_nch; c++) if(fve[c] >= 0) ioc(fve[c], 0x0000450a, 0); }
    if(g_ew_stage < 250) g_ew_stage++;   /* just a counter now; no escalation */
}
/* drain the encoder(s) once: GetStream -> fan out to live viewers + write to disk if recording.
 * Called from the main loop AND from play_file (so recording keeps running during playback). */
static int pump_encode(void){
    int chn, r, gotany=0;
    for(chn=0; chn<NCH; chn++){
        if(fve[chn]<0) continue;
        /* STOP: arm a drain, don't close here. The encoder still holds frames that were
         * captured BEFORE the request (encode + retrieval latency), and closing at the top
         * of this loop discarded them — GetStream below would hand them over with rec_on
         * already 0, so they went to live viewers and never to disk. Measured as ~1 s
         * missing off the end of every recording: exactly the "last frames are cut"
         * symptom. We now keep writing until GetStream reports the FIFO empty (which is
         * also the old fast path for "frames stopped flowing entirely"), bounded by
         * REC_DRAIN_MS so a wedged encoder can never latch the file open.
         * START stays keyframe-gated below. */
        if(rec_want[chn] && rec_stopping[chn]) rec_stopping[chn] = 0;   /* STOP then REC again */
        if(!rec_want[chn] && rec_on[chn] && !rec_stopping[chn]){
            rec_stopping[chn] = 1; rec_drain_end[chn] = now_ms() + REC_DRAIN_MS;
        }
        #define MAXPK 32
        unsigned pkbuf[MAXPK*11], pack[11], stream[3], one=1, q[4], npk, k;
        int drain_pass = 0;
        /* Normally exactly one GetStream per channel per tick. While a STOP is draining we
         * keep pumping THIS channel until the FIFO reports empty: at 30 fps with a loop
         * this busy the queue is almost never empty on any single poll, so taking one pass
         * per tick just ran the REC_DRAIN_MS cap out and recorded ~1 s past the stop.
         * Bounded by the cap AND a pass count — a wedged encoder must not spin here. */
        for(;;){
        q[0]=1; q[1]=0; q[2]=0; q[3]=1;
        ioc(fve[chn], 0x8010450e, q);                   /* Query status */
        ioc(fve[chn], 0x40044512, &one);                /* prep (flag=1) — immediately before Get */
        memset(pkbuf, 0, sizeof(pkbuf));
        stream[0]=(unsigned)(long)pkbuf; stream[1]=MAXPK; stream[2]=0;
        r = ioc(fve[chn], 0xc00c450c, stream);          /* GetStream */
        if(dbg[chn] > 0){ dbg[chn]--;
            puts_("[dvr] ch"); putu((unsigned)chn); puts_(" q2="); puthex(q[2]);
            puts_(" get rc="); puthex((unsigned)r); puts_(" nout="); puthex(stream[1]);
            if(r==0){ puts_(" p0:"); for(k=0;k<11;k++){ puts_(" "); puthex(pkbuf[k]); } } puts_("\n"); }
        if(r != 0){                                     /* empty / not ready */
            if(rec_stopping[chn]) rec_stop(chn);        /* FIFO drained — finish the STOP */
            break;
        }
        gotany = 1;
        npk = stream[1]; if(npk==0 || npk>MAXPK) npk = 1;
        int batch_key = 0;                              /* keyframe? peek pack0 NAL (7=SPS,5=IDR) */
        { unsigned p0=pkbuf[2], l0=pkbuf[4];
          if(p0>=buf_p2[chn] && (p0-buf_p2[chn])<buf_len[chn] && l0>0x45){
              unsigned char *b0 = bufv[chn] + (p0-buf_p2[chn]) + buf_hoff[chn];
              int nt = b0[0x44]&0x1f; if(nt==7 || nt==5) batch_key=1; } }
        if(batch_key){                                  /* record START only at a keyframe (STOP handled above) */
            if(rec_want[chn] && !rec_on[chn]) rec_start(chn); }
        unsigned au_len = 0;
        for(k=0; k<npk; k++){
            unsigned phys, tlen, boff, dlen; unsigned char *base;
            memcpy(pack, &pkbuf[k*11], 44);
            phys = pack[2]; tlen = pack[4];
            if(phys < buf_p2[chn] || (phys - buf_p2[chn]) >= buf_len[chn]) continue;
            boff = (phys - buf_p2[chn]) + buf_hoff[chn]; base = bufv[chn] + boff;
            if(tlen <= 0x40) continue;
            dlen = tlen - 0x40;
            net_fanout(chn, base + 0x40, dlen, batch_key);
            if(rec_on[chn] && au_len + dlen <= sizeof(au_buf)){ memcpy(au_buf+au_len, base+0x40, dlen); au_len += dlen; }
            { unsigned phys1b = pack[3], len1b = pack[5];  /* seg1: ring-wrap tail (no 0x40 hdr) */
              if(len1b > 0 && phys1b >= buf_p2[chn] && (phys1b - buf_p2[chn]) < buf_len[chn]){
                  unsigned char *base1b = bufv[chn] + (phys1b - buf_p2[chn]) + buf_hoff[chn];
                  net_fanout(chn, base1b, len1b, 0);
                  if(rec_on[chn] && au_len + len1b <= sizeof(au_buf)){ memcpy(au_buf+au_len, base1b, len1b); au_len += len1b; } } }
            total_packs++;
        }
        if(rec_on[chn] && au_len){
            /* Timeline comes from the encoder's own capture clock — pack[6..7] is a u64
             * microsecond stamp, verified to step 33367 µs (29.97 fps, NTSC) between
             * consecutive frames. Stamping the *configured* fps instead made files play
             * fast and report a duration well short of the wall-clock recording, because
             * the encoder delivers fewer frames than fps= claims under record load. Using
             * the real stamp also means a dropped frame becomes a gap, not a speed-up. */
            unsigned long long pk_us = ((unsigned long long)pkbuf[7] << 32)
                                     |  (unsigned long long)pkbuf[6];
            if(!rec_base_us[chn]) rec_base_us[chn] = pk_us;
            unsigned long long pts90 = 90000ull;
            if(pk_us > rec_base_us[chn])                /* µs -> 90 kHz ticks (×90/1000) */
                pts90 += (pk_us - rec_base_us[chn]) * 9ull / 100ull;
            ts_write(&rec_ts[chn], au_buf, au_len, batch_key, pts90);
            total_bytes += au_len; rec_b[chn] += au_len;
            while(rec_b[chn] >= (1u<<20)){ rec_b[chn] -= (1u<<20); rec_mb[chn]++; }
            /* A failed write (disk full, I/O error) means every later byte lands in a file
             * that is already wrong. Stop now and say so, rather than run to the size cap
             * producing something unplayable. free_mb's 1 s poll is the first line of
             * defence; this is the one that catches what happens between polls. */
            if(rec_ts[chn].werr){ puts_("[dvr] REC write error ch"); putu((unsigned)chn);
                puts_(" — stopping (disk full?)\n"); rec_want[chn]=0; rec_stop(chn); }
            else if(rec_mb[chn] >= g_max_rec_mb){ puts_("[dvr] REC size cap ch"); putu((unsigned)chn); puts_("\n");
                rec_want[chn]=0; rec_stop(chn); } }
        ioc(fve[chn], 0x400c450d, stream);              /* ReleaseStream (once per GetStream) */
        if(!rec_stopping[chn]) break;                   /* normal path: one pass per tick */
        if((int)(now_ms() - rec_drain_end[chn]) >= 0 || ++drain_pass > 300){
            rec_stop(chn); break;                       /* cap hit — never latch the file open */
        }
        }   /* end drain pump */
    }
    return gotany;
}
/* feed one access unit to VDEC + an AUD trailer to flush the frame out immediately */
static void pb_feed(int vdec, const unsigned char *buf, unsigned len){
    static const unsigned char aud[8]={0x00,0x00,0x01,0x09,0x10,0x00,0x00,0x01};
    unsigned st[4]; st[0]=(unsigned)(long)buf; st[1]=len; st[2]=0; st[3]=0;
    ioc(vdec,0x40104409,st);
    st[0]=(unsigned)(long)aud; st[1]=8; st[2]=0; st[3]=0; ioc(vdec,0x40104409,st);
}
/* "MM/DD HH:MM CHn" title from a /root/rec/aN/YYYYMMDD_HHMMSS_chN.ts path */
static void pb_set_title(const char *path){
    const char *n=path, *p=path; while(*p){ if(*p=='/') n=p+1; p++; }
    char *t=g_pb_title; int i=0;
    if(n[0]>='0'&&n[0]<='9'){
        t[i++]=n[4];t[i++]=n[5];t[i++]='/';t[i++]=n[6];t[i++]=n[7];t[i++]=' ';
        t[i++]=n[9];t[i++]=n[10];t[i++]=':';t[i++]=n[11];t[i++]=n[12];t[i++]=' ';
        t[i++]='C';t[i++]='H';t[i++]=(n[18]>='1'&&n[18]<='4')?n[18]:'?';t[i]=0;
    } else t[0]=0;
}
/* seek (scrub) to a permille of the file: jump there, scan to the next keyframe, decode it,
 * pause on it. Keyframe-granular (GOP ~1s), so scrubbing lands on the nearest keyframe. */
static void pb_do_seek(tsdemux *d, int vdec, int pm){
    if(g_pb_total<=0) return;
    if(pm<0)pm=0; if(pm>1000)pm=1000;
    long P=(long)((long long)g_pb_total*pm/1000);
    tsdemux_seek(d, P);
    unsigned l2; int found=0; long guard=0;
    while(!g_pb_stop && (l2=tsdemux_next(d))>0){         /* scan to the next keyframe (don't feed P/B) */
        if(guard++>6000) break;
        if(pb_au[0]==0&&pb_au[1]==0&&pb_au[2]==0&&pb_au[3]==1 &&
           ((pb_au[4]&0x1f)==7 || (pb_au[4]&0x1f)==5)){ found=1; break; }
        if(g_wdt>=0) sys_write(g_wdt,"w",1);
    }
    if(found){
        pb_feed(vdec, pb_au, l2);
        g_pb_pos=d->cur_off;
        if(g_pb_tot_ms>0) g_pb_cur_ms=(long)((long long)g_pb_tot_ms*g_pb_pos/g_pb_total);
        g_pb_frame=(long)((long long)g_pb_cur_ms*30/1000);
        g_kf_no[0]=g_pb_frame; g_kf_off[0]=d->cur_off; g_kf_cnt=1;   /* reset ring to this keyframe */
    }
    g_pb_paused=1; g_pb_ended=0;
}
/* step back one frame: seek to the newest cached keyframe <= (current-1), re-decode forward to it */
static void pb_do_back(tsdemux *d, int vdec){
    long target=g_pb_frame-1; if(target<1) target=1;
    long Kno=-1, Koff=0; int j;
    for(j=0;j<g_kf_cnt;j++) if(g_kf_no[j]<=target){ Kno=g_kf_no[j]; Koff=g_kf_off[j]; }
    /* Fast, frame-accurate path: a cached keyframe sits within a few GOPs before target (the normal
     * case while stepping through recently-played frames — the ring holds ~12s of keyframes). */
    if(Kno>=0 && (target-(Kno-1)) <= 60){
        tsdemux_seek(d, Koff);
        long fr=Kno-1; unsigned l2;
        while(fr<target && !g_pb_stop && (l2=tsdemux_next(d))>0){ fr++; pb_feed(vdec, pb_au, l2);
            if(g_wdt>=0) sys_write(g_wdt,"w",1); }
        g_pb_frame=target; g_pb_paused=1; g_pb_ended=0; return;
    }
    /* Degraded path — target is before the cached-keyframe window (stepped back past the ring, or the
     * ring was reset by a scrub). The OLD code fell back to keyframe offset 0 and re-decoded EVERY frame
     * from the file start at decoder speed -> a ~minute-long UI freeze. Instead estimate target's byte
     * offset from the current frame/offset ratio and land on the nearest keyframe there (coarse but instant). */
    long of=(g_pb_frame>0)?g_pb_frame:1, op=g_pb_pos;
    long est=(op>0)?(long)((long long)op*target/of):0; if(est<0) est=0;
    tsdemux_seek(d, est);
    unsigned l2; int found=0; long guard=0;
    while(!g_pb_stop && (l2=tsdemux_next(d))>0){
        if(guard++>6000) break;                         /* ~1.1MB scan cap */
        if(pb_au[0]==0&&pb_au[1]==0&&pb_au[2]==0&&pb_au[3]==1 &&
           ((pb_au[4]&0x1f)==7 || (pb_au[4]&0x1f)==5)){ found=1; break; }
        if(g_wdt>=0) sys_write(g_wdt,"w",1);
    }
    if(found){
        pb_feed(vdec, pb_au, l2);
        g_pb_pos=d->cur_off;
        g_pb_frame=(op>0)?(long)((long long)of*d->cur_off/op):target; if(g_pb_frame<1) g_pb_frame=1;
        g_kf_no[0]=g_pb_frame; g_kf_off[0]=d->cur_off; g_kf_cnt=1;   /* reseed the ring at this keyframe */
        if(g_pb_total>0) g_pb_cur_ms=(long)((long long)g_pb_tot_ms*g_pb_pos/g_pb_total);
    }
    g_pb_paused=1; g_pb_ended=0;
}
static int play_file(const char *path){
    int vichn=(g_dispch>=0&&g_dispch<NCH)?g_dispch:0;
    tsdemux d;
    if(tsdemux_open(&d,path,pb_au,sizeof(pb_au))!=0) return -1;
    g_pb_stop=0; g_pb_active=1;
    puts_("[dvr] PLAY "); puts_(path); puts_("\n");
    unsigned vb[2];
    if(g_fvi[vichn]>=0){ vb[0]=0;vb[1]=(unsigned)vichn; ioc(g_fvi[vichn],0x40084919,vb);
                         vb[0]=2;vb[1]=(unsigned)vichn; ioc(g_fvi[vichn],0x40084919,vb); }
    int vdec=(int)sys_open("/dev/vdec",O_RDWR,0);
    if(vdec>=0){
        unsigned chn=0; ioc(vdec,0x40044414,&chn);             /* AttachChn */
        unsigned char attr[32]; int i; for(i=0;i<32;i++) attr[i]=0;
        unsigned w=720, h=(g_standard==2)?576:480;
        put32(attr,0,0x60); put32(attr,4,w*h*2);               /* PT_H264, stream buf */
        put32(attr,12,w); put32(attr,16,h);                    /* pic W/H */
        put32(attr,20,2); put32(attr,24,1);
        int cr=ioc(vdec,0x40204400,attr);                      /* CreateChn */
        sys_ioctl(vdec,0x4411,0);                              /* StartRecvStream */
        { unsigned m=0; ioc(vdec,0x4004440d,&m); }             /* send mode */
        /* app.out (FUN_00139b18) re-configures + enables the VO window on BOTH dev2 and dev0
         * BEFORE binding VDEC: AttachChn(chn|dev<<8) -> SetChnAttr(28B rect) -> EnableChn.
         * VI-unbind alone leaves the channel unable to pull decoded frames. */
        unsigned cattr[7]; cattr[0]=1; cattr[1]=0; cattr[2]=0;
        cattr[3]=0x2d0; cattr[4]=h; cattr[5]=1; cattr[6]=0;     /* {prio,x,y,w=720,h,zoom,deflk} */
        /* per app.out stop-preview(FUN_00166634)+vdec-open: DisableChn 0x4f1e (releases the VI
         * source) -> SetChnAttr -> EnableChn 0x4f1d. The disable->enable cycle is what lets the
         * VO channel re-source from VDEC instead of staying latched to the (unbound) VI. */
        int vd;
        vd=(int)sys_open("/dev/vo",O_RDWR,0);
        if(vd>=0){ ioc1(vd,0x40044f53,(unsigned)vichn|(2u<<8)); iocv(vd,0x4f1e,0);
                   ioc(vd,0x401c4f1f,cattr); iocv(vd,0x4f1d,0); sys_close(vd); }
        vd=(int)sys_open("/dev/vo",O_RDWR,0);
        if(vd>=0){ ioc1(vd,0x40044f53,(unsigned)vichn|(0u<<8)); iocv(vd,0x4f1e,0);
                   ioc(vd,0x401c4f1f,cattr); iocv(vd,0x4f1d,0); sys_close(vd); }
        vb[0]=2;vb[1]=(unsigned)vichn; ioc(vdec,0x40084406,vb); /* BindOutput dev2 */
        vb[0]=0;vb[1]=(unsigned)vichn; ioc(vdec,0x40084406,vb); /* BindOutput dev0 */
        puts_("[dvr] VDEC create rc="); puthex((unsigned)cr); puts_("\n");
        /* progress + control state, then show the control bar */
        g_pb_total = sys_lseek(d.fd, 0, 2); sys_lseek(d.fd, 0, 0);   /* file size, rewind */
        g_pb_pos=0; g_pb_paused=0; g_pb_step=0; g_pb_back=0; g_pb_replay=0; g_pb_ended=0;
        g_pb_seek=0; g_pb_scrub=0;
        g_pb_speed=4; g_pb_frame=0; g_kf_cnt=0; g_pb_sel=PBB_PP;
        g_pb_cur_ms=0; g_pb_tot_ms=0; g_pb_lastui=now_ms();
        pb_set_title(path);
        pb_bar_draw(1);
        unsigned len, poll=0; unsigned long long prev_pts=0, pts0=0; int have_prev=0, have_pts0=0; unsigned barcd=0;
        while(!g_pb_stop){
            len = tsdemux_next(&d);
            if(len==0){                                        /* END OF FILE — hold, behave like a player */
                if(!g_pb_ended){ g_pb_ended=1; g_pb_paused=1; g_pb_sel=PBB_PP;
                                 if(g_pb_cur_ms>g_pb_tot_ms) g_pb_tot_ms=g_pb_cur_ms; pb_bar_draw(0); }
                while(!g_pb_stop && !g_pb_replay && !g_pb_back && !g_pb_seek){ msleep(30); ctl_poll(); ui_mouse_poll(); ui_kbd_poll(); pump_encode();
                    if(g_wdt>=0) sys_write(g_wdt,"w",1); }
                if(g_pb_stop) break;
                if(g_pb_replay){ g_pb_replay=0; g_pb_ended=0; g_pb_paused=0; g_pb_frame=0; g_kf_cnt=0;
                    have_prev=0; have_pts0=0; g_pb_cur_ms=0; tsdemux_seek(&d,0); g_pb_lastui=now_ms(); pb_bar_draw(1); continue; }
                if(g_pb_seek){ g_pb_seek=0; pb_do_seek(&d, vdec, g_pb_seek_pm); have_prev=0; pb_bar_draw(0); continue; }
                if(g_pb_back){ g_pb_back=0; pb_do_back(&d, vdec); have_prev=0; pb_bar_draw(0); continue; }
                continue;
            }
            g_pb_ended=0;
            /* remember keyframes {frame#, file offset} for back-step (AU = 00 00 00 01 <nal>;
             * our GOPs open with SPS(7); IDR=5). Ring keeps the most recent KFRING. */
            if(pb_au[0]==0&&pb_au[1]==0&&pb_au[2]==0&&pb_au[3]==1 &&
               ((pb_au[4]&0x1f)==7 || (pb_au[4]&0x1f)==5)){
                if(g_kf_cnt<KFRING){ g_kf_no[g_kf_cnt]=g_pb_frame+1; g_kf_off[g_kf_cnt]=d.cur_off; g_kf_cnt++; }
                else { int j; for(j=1;j<KFRING;j++){ g_kf_no[j-1]=g_kf_no[j]; g_kf_off[j-1]=g_kf_off[j]; }
                       g_kf_no[KFRING-1]=g_pb_frame+1; g_kf_off[KFRING-1]=d.cur_off; }
            }
            if(!have_pts0){ pts0=d.pts; have_pts0=1; }         /* elapsed / (estimated) total time */
            g_pb_cur_ms = (long)((d.pts - pts0)/90ULL);
            g_pb_pos = sys_lseek(d.fd, 0, 1);
            if(g_pb_pos>0 && g_pb_total>0) g_pb_tot_ms=(long)((long long)g_pb_cur_ms*g_pb_total/g_pb_pos);
            /* pause: hold on the last decoded frame until resume / step / back / stop */
            if(g_pb_paused) pb_bar_draw(0);
            while(g_pb_paused && !g_pb_stop && !g_pb_step && !g_pb_back && !g_pb_seek){
                msleep(20); ctl_poll(); ui_mouse_poll(); ui_kbd_poll(); pump_encode();
                if(g_wdt>=0) sys_write(g_wdt,"w",1);
            }
            if(g_pb_stop) break;
            if(g_pb_seek){ g_pb_seek=0; pb_do_seek(&d, vdec, g_pb_seek_pm); have_prev=0; pb_bar_draw(0); continue; }
            if(g_pb_back){ g_pb_back=0; pb_do_back(&d, vdec); have_prev=0; pb_bar_draw(0); continue; }
            int stepping = g_pb_step; g_pb_step=0;
            if(!stepping){                                     /* real-time pace, scaled by slow-mo */
                long delay=0;
                if(have_prev){ long dur=(long)((d.pts-prev_pts)/90ULL); if(dur<0)dur=0; if(dur>500)dur=33;
                    delay = dur*4/(g_pb_speed>0?g_pb_speed:4); }
                long tend=now_ms()+delay;
                for(;;){ long now=now_ms(); if(now>=tend||g_pb_stop||g_pb_paused) break;
                    msleep(3);
                    /* Drain the encoder EVERY pass, not every 4th. Playing a clip while
                     * recording used to cost 31% of the recorded frames (20.8 fps against
                     * 29.97) because the VENC ring overflowed while we sat in this pacing
                     * loop. The input polls stay rate-limited — they are for a human. */
                    pump_encode();
                    if(!((++poll)&3)){ ctl_poll(); ui_mouse_poll(); ui_kbd_poll(); }
                    if(g_wdt>=0) sys_write(g_wdt,"w",1); }
            }
            prev_pts=d.pts; have_prev=1;
            pb_feed(vdec, pb_au, len);                         /* SendStream + AUD */
            g_pb_frame++;
            if(stepping) g_pb_paused=1;                        /* stepped one frame -> re-pause */
            if(((++barcd)&7)==0 || stepping) pb_bar_draw(0);   /* progress/auto-hide tick + step feedback */
            pump_encode();                                     /* every decoded frame, not every 8th */
            if(!((++poll)&7)){ ctl_poll(); ui_mouse_poll(); ui_kbd_poll(); }
            if(g_wdt>=0) sys_write(g_wdt,"w",1);               /* pet watchdog during playback */
        }
        g_pb_paused=0; g_pb_ended=0;
        sys_ioctl(vdec,0x4412,0);                              /* StopRecvStream */
        sys_ioctl(vdec,0x4401,0);                              /* DestroyChn (drops bind) */
        sys_close(vdec);
    }
    tsdemux_close(&d);
    /* return to live: release the (destroyed) VDEC source and re-source from the live VI. Same helper
     * used on startup — cycles the VO channel disable->SetChnAttr->enable on BOTH devs, then re-binds VI. */
    vo_relive(vichn);
    g_pb_active=0;
    if(g_ui_fb){ ui_refresh(); ui_render(&g_ui); fb_blit(0,0,(int)g_fbw,(int)g_fbh); }
    puts_("[dvr] PLAY end\n");
    return 0;
}

/* ================= recording thumbnails (Playback grid) =========================================
 * Decode each recording's FIRST keyframe to the VO (hidden under the opaque grid), grab the composed
 * frame with VO GetScreenFrame (native NV12 720x480), downscale + BT.601 -> ARGB1555, cache. Done
 * incrementally (one per main-loop tick while the grid is open) so the UI stays responsive and
 * thumbnails pop in. VDEC shares the VEDU codec with VENC, so like playback this briefly lowers the
 * recorded framerate while generating — but only for ~1s when the page first opens. */
#define TG_SLOTS 24
static unsigned short tg_pix[TG_SLOTS][THUMB_W*THUMB_H];  /* ~1.0 MB ARGB1555 LRU cache */
static char     tg_key[TG_SLOTS][16];   /* sort-key of the cached recording; [0]==0 => free slot */
static unsigned tg_lru[TG_SLOTS];       /* LRU stamp (lowest = evict) */
static unsigned tg_clock=0;
static char     tg_tried[64];           /* per-list attempt flag (cleared on re-list) — no retry storms */
static int      tg_vdec=-1;             /* VDEC fd while a thumb-gen session holds the VO */
static int      tg_vichn=0;

static int tg_key_eq(const char *a, const char *b){ int i; for(i=0;i<14;i++){ if(a[i]!=b[i]) return 0; } return 1; }

/* re-point g_pb_thumb[] at any cached slot whose key still matches the (re-sorted) list */
static void thumb_rebind(void){
    int i,s;
    for(i=0;i<64;i++){ g_pb_thumb[i]=0; tg_tried[i]=0; }
    for(i=0;i<g_ui.pb_count && i<64;i++)
        for(s=0;s<TG_SLOTS;s++)
            if(tg_key[s][0] && tg_key_eq(tg_key[s], pb_key[i])){ g_pb_thumb[i]=tg_pix[s]; break; }
}

/* NV12 (Y plane + interleaved Cb,Cr) -> nearest-downscale + BT.601 -> ARGB1555 THUMB_W x THUMB_H */
static void tg_nv12_to_thumb(unsigned char *y, unsigned char *uv, unsigned sw, unsigned sh,
                             unsigned ys, unsigned cs, unsigned short *dst){
    unsigned ox,oy;
    for(oy=0;oy<THUMB_H;oy++){
        unsigned syv=oy*sh/THUMB_H;
        unsigned char *yl=y+syv*ys, *cl=uv+(syv>>1)*cs;
        for(ox=0;ox<THUMB_W;ox++){
            unsigned sx=ox*sw/THUMB_W;
            int Y=yl[sx], U=cl[sx&~1u], V=cl[(sx&~1u)+1];
            int C=Y-16, D=U-128, E=V-128;
            int R=(298*C+409*E+128)>>8, G=(298*C-100*D-208*E+128)>>8, B=(298*C+516*D+128)>>8;
            if(R<0)R=0; else if(R>255)R=255;
            if(G<0)G=0; else if(G>255)G=255;
            if(B<0)B=0; else if(B>255)B=255;
            dst[oy*THUMB_W+ox]=(unsigned short)(0x8000 | ((R>>3)<<10) | ((G>>3)<<5) | (B>>3));
        }
    }
}

/* GetScreenFrame the composed VO output (the decoded frame) into dst; return 1 on success. */
static int tg_capture(unsigned short *dst){
    if(g_fvo<0) return 0;
    unsigned f[40]; memset(f,0,sizeof(f));
    if(ioc(g_fvo,0xc0984f0f,f)!=0) return 0;                       /* GetScreenFrame */
    unsigned w=f[0],h=f[1],p0=f[4],p1=f[5],s0=f[10],s1=f[11];
    int fmem=(int)sys_open("/dev/mem",O_RDWR|O_SYNC,0);
    if(fmem<0 || w==0 || h==0 || p0==0){ ioc(g_fvo,0x40984f10,f); return 0; }
    unsigned base=p0&0xfffff000u, off0=p0-base;
    unsigned span=s0*h+s1*(h/2)+0x2000, mlen=(off0+span+0xfff)&0xfffff000u;
    unsigned char *m=(unsigned char*)sys_mmap2(0,mlen,PROT_READ,MAP_SHARED,fmem,base>>12);
    int ok=0;
    if((long)m!=-1 && (long)m>=0){ tg_nv12_to_thumb(m+off0, m+(p1-base), w, h, s0, s1, dst); sys_munmap(m,mlen); ok=1; }
    sys_close(fmem);
    ioc(g_fvo,0x40984f10,f);                                       /* ReleaseScreenFrame */
    return ok;
}

/* the VDEC->VO setup / teardown mirror play_file's head/tail (see the comments there) */
static void thumb_setup(void){
    tg_vichn=(g_dispch>=0&&g_dispch<NCH)?g_dispch:0;
    unsigned vb[2], w=720, h=(g_standard==2)?576:480; int i;
    if(g_fvi[tg_vichn]>=0){ vb[0]=0;vb[1]=(unsigned)tg_vichn; ioc(g_fvi[tg_vichn],0x40084919,vb);
                            vb[0]=2;vb[1]=(unsigned)tg_vichn; ioc(g_fvi[tg_vichn],0x40084919,vb); }
    int vdec=(int)sys_open("/dev/vdec",O_RDWR,0);
    if(vdec<0) return;
    unsigned chn=0; ioc(vdec,0x40044414,&chn);
    unsigned char attr[32]; for(i=0;i<32;i++) attr[i]=0;
    put32(attr,0,0x60); put32(attr,4,w*h*2); put32(attr,12,w); put32(attr,16,h); put32(attr,20,2); put32(attr,24,1);
    ioc(vdec,0x40204400,attr);
    sys_ioctl(vdec,0x4411,0);
    { unsigned m=0; ioc(vdec,0x4004440d,&m); }
    unsigned cattr[7]; cattr[0]=1;cattr[1]=0;cattr[2]=0; cattr[3]=0x2d0; cattr[4]=h; cattr[5]=1; cattr[6]=0;
    int vd;
    vd=(int)sys_open("/dev/vo",O_RDWR,0);
    if(vd>=0){ ioc1(vd,0x40044f53,(unsigned)tg_vichn|(2u<<8)); iocv(vd,0x4f1e,0); ioc(vd,0x401c4f1f,cattr); iocv(vd,0x4f1d,0); sys_close(vd); }
    vd=(int)sys_open("/dev/vo",O_RDWR,0);
    if(vd>=0){ ioc1(vd,0x40044f53,(unsigned)tg_vichn|(0u<<8)); iocv(vd,0x4f1e,0); ioc(vd,0x401c4f1f,cattr); iocv(vd,0x4f1d,0); sys_close(vd); }
    vb[0]=2;vb[1]=(unsigned)tg_vichn; ioc(vdec,0x40084406,vb);
    vb[0]=0;vb[1]=(unsigned)tg_vichn; ioc(vdec,0x40084406,vb);
    tg_vdec=vdec;
}
static void thumb_teardown(void){
    if(tg_vdec<0) return;
    sys_ioctl(tg_vdec,0x4412,0); sys_ioctl(tg_vdec,0x4401,0); sys_close(tg_vdec); tg_vdec=-1;
    unsigned cattr[7]; cattr[0]=1;cattr[1]=0;cattr[2]=0; cattr[3]=0x2d0; cattr[4]=(g_standard==2)?576:480; cattr[5]=1; cattr[6]=0;
    int vd;
    vd=(int)sys_open("/dev/vo",O_RDWR,0);
    if(vd>=0){ ioc1(vd,0x40044f53,(unsigned)tg_vichn|(2u<<8)); iocv(vd,0x4f1e,0); ioc(vd,0x401c4f1f,cattr); iocv(vd,0x4f1d,0); sys_close(vd); }
    vd=(int)sys_open("/dev/vo",O_RDWR,0);
    if(vd>=0){ ioc1(vd,0x40044f53,(unsigned)tg_vichn|(0u<<8)); iocv(vd,0x4f1e,0); ioc(vd,0x401c4f1f,cattr); iocv(vd,0x4f1d,0); sys_close(vd); }
    unsigned vb[2];
    if(g_fvi[tg_vichn]>=0){ vb[0]=2;vb[1]=(unsigned)tg_vichn; ioc(g_fvi[tg_vichn],0x40084918,vb);
                            vb[0]=0;vb[1]=(unsigned)tg_vichn; ioc(g_fvi[tg_vichn],0x40084918,vb); }
}
static int tg_alloc_slot(const char *key){
    int s, free_s=-1, lru_s=0;
    for(s=0;s<TG_SLOTS;s++){ if(tg_key[s][0] && tg_key_eq(tg_key[s],key)) return s;   /* reuse */
                             if(!tg_key[s][0] && free_s<0) free_s=s;
                             if(tg_lru[s]<tg_lru[lru_s]) lru_s=s; }
    return (free_s>=0)?free_s:lru_s;
}
/* wait `ms` while keeping the encoder drained + watchdog pet (so recording doesn't hiccup) */
static void tg_wait(int ms){ int t; for(t=0;t<ms;t+=10){ msleep(10); pump_encode(); if(g_wdt>=0) sys_write(g_wdt,"w",1); } }

/* generate one recording's thumbnail (decode its 1st keyframe, capture, cache) */
static void thumb_one(int idx){
    if(idx<0 || idx>=g_ui.pb_count || idx>=64 || g_pb_thumb[idx] || tg_vdec<0) return;
    tsdemux dd;
    if(tsdemux_open(&dd,pb_path[idx],pb_au,sizeof(pb_au))!=0) return;
    /* feed the first two AUs (SPS/PPS/IDR then next frame) to push the IDR through VDEC */
    int fed=0; unsigned len;
    while(fed<2 && (len=tsdemux_next(&dd))>0){ pb_feed(tg_vdec, pb_au, len); fed++; tg_wait(20); }
    tsdemux_close(&dd);
    if(fed==0) return;
    tg_wait(70);                                       /* let the decoded frame reach the VO */
    int slot=tg_alloc_slot(pb_key[idx]);
    if(tg_capture(tg_pix[slot])){
        int k; for(k=0;k<14;k++) tg_key[slot][k]=pb_key[idx][k]; tg_key[slot][14]=0; tg_key[slot][15]=0;
        tg_lru[slot]=++tg_clock; g_pb_thumb[idx]=tg_pix[slot];
    }
}
/* main-loop driver: when the Playback grid is open, generate the first missing visible thumbnail
 * per call and pop it into its cell; release the VDEC/VO once the whole page is done or the grid closes. */
static void thumb_tick(void){
    if(!g_ui_fb || g_pb_active) return;
    if(!(g_ui.open && g_ui.menu==MENU_PLAYBACK && g_ui.pb_count>0)){ if(tg_vdec>=0) thumb_teardown(); return; }
    int page=g_ui.sel/PB_PAGE, start=page*PB_PAGE, i, want=-1, onpage=-1;
    for(i=0;i<PB_PAGE && start+i<g_ui.pb_count;i++){ int idx=start+i;
        if(idx<64 && !g_pb_thumb[idx] && !tg_tried[idx]){ want=idx; onpage=i; break; } }
    if(want<0){ if(tg_vdec>=0) thumb_teardown(); return; }   /* page fully generated -> free the codec */
    if(tg_vdec<0){ thumb_setup(); if(tg_vdec<0){ tg_tried[want]=1; return; } }
    thumb_one(want);
    tg_tried[want]=1;
    if(g_pb_thumb[want]){                               /* pop the finished thumbnail into just its cell */
        pbgrid_geom g; ui_pbgrid_geom(&g);
        int cx,cy; ui_pbgrid_cell(&g,onpage,&cx,&cy);
        ui_draw_pbcell(&g_ui,&g,onpage);
        fb_blit(cx,cy,g.cw,g.ch);
    }
}

int main(int argc, char **argv){
    int secs = (argc > 1) ? k_atoi(argv[1]) : 0;   /* 0 = run ~forever */
    int dispch = (argc > 2) ? k_atoi(argv[2]) : -1; /* >=0 => also show that VI ch on VGA */
    /* a runtime channel switch persists here so the respawn shows the chosen channel */
    if(dispch >= 0){ int fd2=(int)sys_open("/root/rec/dispch", O_RDONLY, 0);
        if(fd2>=0){ char c; if(sys_read(fd2,&c,1)==1 && c>='0'&&c<='3') dispch=c-'0'; sys_close(fd2); } }
    /* resolve the displayed channel here, BEFORE the VI bring-up: with vi_all=0 the VI
     * loop only enables channels we consume, and it needs to know which one that is. */
    g_dispch = (dispch >= 0 && dispch < NCH) ? dispch : 0;
    int norec  = (argc > 3 && argv[3][0]=='d');     /* "d" => display-only (skip VENC/record) */
    if(argc > 4){ int f = k_atoi(argv[4]); if(f>0) g_vifps = (unsigned)f; }  /* VI fps override */
    if(argc > 5){ int s = k_atoi(argv[5]); if(s>0) g_vosync = (unsigned)s; } /* VO output mode */
    if(argc > 6){ int t = k_atoi(argv[6]); if(t>0) g_votol = (unsigned)t; }  /* VO play toleration ms */
    g_start_sec = now_sec();
    int chn, i, r;
    int fvi[NCH];                  /* per-channel VI fds (setup only; g_fvi[] is the global) */
    unsigned buf_page[NCH];        /* phys1 page base used for mmap per channel (setup only) */
    /* fve[], bufv[], buf_hoff[], buf_p2[], buf_len[], dbg[], total_* are now file-scope globals
     * so pump_encode() can run from play_file too (record while playing back). */

    for(i=0;i<NCH;i++){ fve[i]=-1; fvi[i]=-1; bufv[i]=0; dbg[i]=6;
                        rec_want[i]=0; rec_on[i]=0; rec_fd[i]=-1; }

    /* config: /root/rec/dvr.conf overrides encoder defaults */
    apply_cfg();
    patch_venc_attrs();

    /* take over the watchdog (app.out stopped -> ~10s to reboot if not petted) */
    g_wdt = (int)sys_open("/dev/watchdog", O_WRONLY, 0);

    puts_("[dvr] recorder starting\n");
    puts_("[dvr] cfg: "); putu(g_ewidth); puts_("x"); putu(g_eheight);
    puts_(" fps="); putu(g_fps); puts_(" gop="); putu(g_gop);
    puts_(" bitrate="); putu(g_bitrate); puts_("\n");

    /* ---- 1. capture chip: tl_R9508 board glue + tw_286x ---- */
    int ftw = (int)sys_open("/dev/tw_286x", O_RDWR, 0);
    int ftl = (int)sys_open("/dev/tl_R9508", O_RDWR, 0);
    puts_("[dvr] tw_286x="); putu((unsigned)ftw); puts_(" tl_R9508="); putu((unsigned)ftl); puts_("\n");
    if(ftl<0 || ftw<0){ die("[dvr] capture chip open failed", 0); wdt_off(); return 1; }
    r = iocv(ftl, 0xc00456d3, 0x64);  puts_("[dvr] tl vin(0x64) rc="); putu((unsigned)r); puts_("\n");
    ioc1(ftl, 0xc00456ce, 0x28);
    ioc1(ftw, 0xc00448d8, 0);
    g_ftw = ftw;           /* expose for the REG debug command */
    apply_standard(ftw);   /* PAL/NTSC: set TW2866 + patch VI/VO/VENC geometry */
    apply_picture_defaults(ftw);   /* neutral bright/contrast/sat/hue (fix pink/green cast) */

    /* ---- 2. SYS + VB ---- */
    int fsys = (int)sys_open("/dev/sys", O_RDWR, 0);
    ioc(fsys, 0x5901, 0);
    int fvb = (int)sys_open("/dev/vb", O_RDWR, 0);
    ioc(fvb, 0x4208, 0);
    ioc(fvb, 0x4044420a, VBCONF);
    ioc(fvb, 0x4207, 0);
    ioc(fsys, 0x40085902, SYSCFG);
    r = ioc(fsys, 0x5900, 0);
    puts_("[dvr] SYS+VB init rc="); putu((unsigned)r); puts_("\n");
    wpet();

    /* ---- 3. VO (app.out inits it before VI; keep it to match the trace) ---- */
    int fvo = (int)sys_open("/dev/vo", O_RDWR, 0);
    ioc(fvo, 0x40044f53, VO_53);
    iocv(fvo, 0x4f01, 0);
    ioc(fvo, 0x40384f02, VO_02);
    iocv(fvo, 0x4f00, 0);
    ioc(fvo, 0x40244f0d, VO_0d);
    iocv(fvo, 0x4f0b, 0);
    ioc1(ftw, 0xc00448d3, 1);

    /* ---- 4. VI x4 (fresh /dev/vi per channel) ---- */
    for(chn=0; chn<NCH; chn++){
        /* with vi_all=0, only bring up the channels something actually drains — see the
         * comment on g_vi_all. Channel 0 must always come up: it carries the dev-level
         * init (SetPubAttr/EnableDev) that the whole VIU depends on. */
        int used = g_vi_all || chn==0 || chn==g_dispch || chn<(int)g_enc_nch;
        if(!used){ fvi[chn] = -1; g_fvi[chn] = -1; continue; }
        int vfd = (int)sys_open("/dev/vi", O_RDWR, 0);
        ioc1(vfd, 0x4004492e, chn);            /* select dev/chn */
        if(chn==0){
            ioc(vfd, 0x4901, 0);               /* dev-level, chn0 only */
            ioc(vfd, 0x40144902, VI_PUB);      /* SetPubAttr */
            iocv(vfd, 0x4900, 0x80);           /* EnableDev */
        }
        ioc(vfd, 0x40244908, VI_CHN);          /* SetChnAttr */
        iocv(vfd, 0x4904, (unsigned)chn);      /* EnableChn */
        ioc1(vfd, 0x4004490a, g_vifps);        /* framerate (30 default, 60 for low-latency display) */
        fvi[chn] = vfd;                        /* keep for VI-start pass */
        wpet();
    }
    puts_("[dvr] VI x4 up\n");

    /* /dev/mem for mapping VENC stream buffers (uncached) */
    int fmem = (int)sys_open("/dev/mem", O_RDWR|O_SYNC, 0);
    if(fmem<0){ die("[dvr] /dev/mem open failed", fmem); }

    /* ---- 5. GRP + VENC (only g_enc_nch channels; skipped in display-only mode) ---- */
    for(chn=0; !norec && chn<(int)g_enc_nch; chn++){
        int fgrp = (int)sys_open("/dev/grp", O_RDWR, 0);
        ioc1(fgrp, 0x40044705, chn);
        ioc(fgrp, 0x4700, 0);
        { unsigned gb[2]; gb[0]=0; gb[1]=(unsigned)chn; ioc(fgrp, 0x40084702, gb); }

        int fv = (int)sys_open("/dev/venc", O_RDWR, 0);
        ioc1(fv, 0x4004451b, chn);             /* select chn */
        r = ioc(fv, 0x40684500, VENC_ATTR);    /* CreateChn */
        if(r!=0){ puts_("[dvr] CreateChn ch"); putu((unsigned)chn); die(" failed", r); }

        /* get stream buffer {phys1, phys2, size}. app.out maps phys1 (info[0]) via
         * /dev/mem (readable); pack.w[2] addrs are relative to phys2 (info[1]).
         * So: map phys1's page, and index with (pack.w[2] - info[1]). */
        unsigned info[3]; memcpy(info, VENC_BUFQ, 12);
        r = ioc(fv, 0x800c450f, info);
        unsigned phys = info[0], size = info[2];
        unsigned page = phys & 0xfffff000u;
        unsigned mlen = (size + (phys - page) + 0xfff) & 0xfffff000u;
        puts_("[dvr] ch"); putu((unsigned)chn); puts_(" phys1="); puthex(info[0]);
        puts_(" phys2="); puthex(info[1]); puts_(" size="); puthex(size); puts_("\n");
        unsigned char *m = (unsigned char*)sys_mmap2(0, mlen, PROT_READ|PROT_WRITE, MAP_SHARED, fmem, page>>12);
        if((long)m == -1 || (long)m < 0){ die("[dvr] mmap stream buf failed", (int)(long)m); }
        bufv[chn] = m; buf_page[chn] = page; buf_len[chn] = mlen;
        buf_hoff[chn] = info[0] - page; buf_p2[chn] = info[1];

        ioc1(fv, 0x40044508, chn);             /* start recv */
        ioc(fv, 0xc0104518, VENC_BIND_Q);      /* bind query */
        ioc(fv, 0xc0104517, VENC_BIND);        /* bind venc <- grp/vi */
        fve[chn] = fv;
        wpet();
    }
    puts_("[dvr] VENC x4 created + bound\n");

    /* ---- 6. START pass (app.out does these after all creates) ----
     * VI start (0x40084918 {2,chn} then {0,chn}) then VENC StartRecvPic
     * (0x0000450a, _IO('E',10), no arg). Without these no frames flow. */
    for(chn=0; chn<NCH; chn++){
        unsigned vs[2];
        if(fvi[chn] < 0) continue;             /* not brought up (vi_all=0) */
        vs[0]=2; vs[1]=(unsigned)chn; ioc(fvi[chn], 0x40084918, vs);
        vs[0]=0; vs[1]=(unsigned)chn; ioc(fvi[chn], 0x40084918, vs);
        g_fvi[chn]=fvi[chn];                   /* expose for the playback VO swap */
    }
    for(chn=0; !norec && chn<(int)g_enc_nch; chn++){
        int r6 = ioc(fve[chn], 0x404c4506, VENC_RC);   /* SetChnAttr (RC) */
        r = ioc(fve[chn], 0x0000450a, 0);              /* StartRecvPic */
        puts_("[dvr] ch"); putu((unsigned)chn);
        puts_(" SetChnAttr rc="); putu((unsigned)r6);
        puts_(" StartRecvPic rc="); putu((unsigned)r); puts_("\n");
    }

    /* ---- 7. optional VGA display (VO dev1) of one channel, low-latency (no codec) ---- */
    if(dispch >= 0 && dispch < NCH){
        wpet();
        g_fvo = display_vga(dispch);   /* keep the VO fd for the SHOT screenshot command */
        ensure_live(dispch);           /* (re)source the VGA from live VI + verify it's moving, re-cycling
                                        * until it is — fixes the intermittent "live frozen on first open" */
    }
    if(norec) puts_("[dvr] DISPLAY-ONLY mode (no recording)\n");

    /* recording is MANUAL, per channel — nothing is written to disk until requested
     * (control plane in Phase 2; interim trigger = touch /root/rec.<N>). Capture,
     * VGA display and the live stream all stay on regardless. */
    if(!norec){ net_init(); ctl_init(); }   /* live stream server + control plane */

    /* bring up the on-screen status overlay (HUD) on the VGA output + USB mouse */
    if(dispch >= 0 && dispch < NCH){
        ui_ensure_fb();
        if(g_ui_fb){ clock_sync(); ui_refresh(); ui_read_picture();   /* one-time RTC + picture reads */
                     ui_render(&g_ui); fb_blit(0,0,(int)g_fbw,(int)g_fbh);
                     fb_alpha_opaque(); fb_show(1);
                     co_open();   /* dedicated cursor overlay layer (fb4) — flash-free */ }
        mouse_open();   /* drive the menus with a USB mouse (cursor appears on first move) */
        kbd_open();     /* USB keyboard (arrow keys) -> same actions as the MCU buttons */
    }

    puts_("[dvr] up: display+stream on; recording idle (manual)\n");

    long t0 = now_sec(), wlast = 0;
    int running = 1;
    while(running){
        long tnow = now_sec();
        if(tnow!=wlast){
            if(g_wdt>=0) sys_write(g_wdt,"w",1);            /* pet */
            wlast=tnow;
            int sf = (int)sys_open("/root/stop", O_RDONLY, 0);  /* clean stop: touch /root/stop */
            if(sf>=0){ sys_close(sf); running=0; }
            /* a STD change needs a pipeline re-init: exit so the wrapper respawns us
             * (the new standard is read from /root/rec/std by apply_cfg). */
            if(g_std_changed){ puts_("[dvr] standard change -> restart\n"); running=0; }
            if(g_restart){ puts_("[dvr] UI restart\n"); running=0; }
            /* low-disk guard: auto-stop all recordings before the SATA fills */
            { int rc=0, c2; for(c2=0;c2<NCH;c2++) if(rec_on[c2]) rc=1;
              if(rc && free_mb("/root/rec/a1") < g_min_free_mb){
                  puts_("[dvr] low disk — auto-stopping recordings\n");
                  for(c2=0;c2<NCH;c2++){ rec_want[c2]=0; rec_stop(c2); }
              } }
            /* encoder throughput, 1 s window. Together with the freeze watchdog this tells
             * capture-side death (pps -> 0) apart from a display-only latch (pps normal
             * while the video layer is frozen) — the two need different fixes. */
            g_pps = (unsigned)(total_packs - g_packs_last); g_packs_last = total_packs;
            video_watchdog();     /* detect + heal a latched live video layer */
            encoder_watchdog();   /* detect + heal a stalled VENC (screen fine, no frames) */
            ui_tick();   /* keep the HUD clock + REC state live (cheap, HUD-only) */
        }
        net_poll();   /* accept new stream clients (non-blocking) */
        if(!norec) ctl_poll();   /* control: record start/stop, STATUS, TIME, STD */
        ui_mouse_poll();         /* USB mouse -> menu navigation (cheap, cursor-only when idle) */
        ui_kbd_poll();           /* USB keyboard arrows -> MCU-button actions */
        beep_tick();             /* simple buzzer patterns */
        melody_tick();           /* one melody note per tick (record start/stop) */
        egg_tick();              /* one animation slice per tick — never blocks pump_encode */
        thumb_tick();            /* generate Playback-grid thumbnails incrementally while it's open */

        int gotany = pump_encode();   /* drain + fan-out + record the encoder(s) */

        if(secs && (tnow - t0) >= secs) running = 0;
        if(!gotany) msleep(10);
    }

    puts_("[dvr] stopping. packs="); putu((unsigned)total_packs);
    puts_(" recbytes="); putu((unsigned)total_bytes); puts_("\n");
    net_shutdown();
    ctl_shutdown();
    for(chn=0; chn<NCH; chn++) rec_stop(chn);
    kbd_close();   /* restore the console keyboard mode we borrowed */
    wdt_off();
    return 0;
}
