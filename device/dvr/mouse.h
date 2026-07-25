/* mouse.h — USB mouse input for the on-screen UI (drive the menus with a mouse
 * while the physical MCU buttons are being built).
 *
 * IMPORTANT: this DVR's kernel does NOT expose a standard 3-byte PS/2 mouse on
 * /dev/input/mice. It's a CUSTOM mousedev driver (the same one app.out reads as
 * /dev/mice) that delivers a **20-byte record with the ABSOLUTE, already-accumulated
 * and clamped cursor position** — the kernel does the delta→position accumulation, so
 * there is no acceleration/overflow/sensitivity for us to get wrong. Verified layout
 * (little-endian) from raw captures:
 *   [0..7]  timeval (ignored)
 *   [8..11] wheel (int32)
 *   [12..15] buttons (u32: bit0=L, bit1=R, bit2=M)
 *   [16..19] pos = (x & 0xffff) | (y << 16)   in a 1024x768 coordinate space
 * We just scale (x,y) from that space onto the screen. Smooth, no drift.
 *
 * Requires oabi.h. State is file-global; dvr.c passes g_mx/g_my to cursor_move().
 */
#ifndef DVR_MOUSE_H
#define DVR_MOUSE_H

#include "oabi.h"

#define MEV_MOVE  1
#define MEV_LEFT  2   /* left-button press edge */
#define MEV_RIGHT 4   /* right-button press edge */
#define MEV_WHEEL 8   /* scroll wheel moved (consume via mouse_wheel()) */

static int g_wheel_val=0, g_wheel_acc=0, g_wheel_init=0;
/* consume the accumulated wheel delta (notches; +up / -down) since the last call */
static int mouse_wheel(void){ int d=g_wheel_acc; g_wheel_acc=0; return d; }

#define MOUSE_PKT   20
#define MOUSE_DRV_W 1024   /* driver coordinate space (observed max 0x400 x 0x300) */
#define MOUSE_DRV_H 768

static int g_mfd = -1;
static int g_mx = 640, g_my = 512;     /* cursor position in screen pixels */
static long g_accx = -1, g_accy = -1;  /* (unused now — absolute positioning; kept for ABI) */
static int g_mbtn = 0;                 /* current button bitmask: 1=L 2=R */
static int g_mseen = 0;                /* set on the first real event (=> draw cursor) */
static int g_msens = 8;                /* (unused for movement — driver is absolute) */
/* debug counters (MDBG/MRAW commands) */
static unsigned long g_mdbg_pk=0, g_mdbg_ovf=0, g_mdbg_reads=0;
static int g_mdbg_b=0, g_mdbg_dx=0, g_mdbg_dy=0;
static unsigned char g_mraw[24]; static int g_mrawn=0;
static unsigned char g_mpk[MOUSE_PKT];
static int g_mpn = 0;

/* create /dev/input/mice if absent, then open it non-blocking. */
static void mouse_open(void){
    sys_mkdir("/dev/input", 0755);                 /* ignore EEXIST */
    sys_mknod("/dev/input/mice", 020644, (13<<8)|63);   /* S_IFCHR|0644, c 13 63 */
    g_mfd = (int)sys_open("/dev/input/mice", O_RDONLY|O_NONBLOCK, 0);
}

/* non-blocking poll: consume all pending 20-byte records, update cursor to the latest
 * absolute position (scaled to the screen), and return the OR of the MEV_* edges. */
static int mouse_poll(int maxx, int maxy){
    if(g_mfd < 0) return 0;
    unsigned char rd[80];
    long n = sys_read(g_mfd, rd, sizeof(rd));
    if(n <= 0) return 0;
    g_mdbg_reads++;
    { int cp = n<24?(int)n:24, k; for(k=0;k<cp;k++) g_mraw[k]=rd[k]; g_mrawn=cp; }
    int ev = 0, i;
    for(i=0; i<(int)n; i++){
        g_mpk[g_mpn++] = rd[i];
        if(g_mpn >= MOUSE_PKT){
            g_mpn = 0;
            unsigned btn = (unsigned)g_mpk[12] | ((unsigned)g_mpk[13]<<8)
                         | ((unsigned)g_mpk[14]<<16) | ((unsigned)g_mpk[15]<<24);
            unsigned pos = (unsigned)g_mpk[16] | ((unsigned)g_mpk[17]<<8)
                         | ((unsigned)g_mpk[18]<<16) | ((unsigned)g_mpk[19]<<24);
            int px = (int)(pos & 0xffff), py = (int)((pos>>16) & 0xffff);
            int nx = px * (maxx+1) / MOUSE_DRV_W;   /* scale driver space -> screen */
            int ny = py * (maxy+1) / MOUSE_DRV_H;
            if(nx<0)nx=0; if(nx>maxx)nx=maxx;
            if(ny<0)ny=0; if(ny>maxy)ny=maxy;
            g_mdbg_pk++; g_mdbg_b=(int)(btn&0xff); g_mdbg_dx=px; g_mdbg_dy=py;
            if(nx!=g_mx || ny!=g_my){ g_mx=nx; g_my=ny; ev |= MEV_MOVE; }
            int L=(int)(btn&1), R=(int)((btn>>1)&1);
            if(L && !(g_mbtn&1)) ev |= MEV_LEFT;    /* press edge only */
            if(R && !(g_mbtn&2)) ev |= MEV_RIGHT;
            g_mbtn = (L?1:0)|(R?2:0);
            /* wheel: driver reports an accumulated int32; turn it into notch deltas */
            { int wh=(int)((unsigned)g_mpk[8]|((unsigned)g_mpk[9]<<8)|((unsigned)g_mpk[10]<<16)|((unsigned)g_mpk[11]<<24));
              if(!g_wheel_init){ g_wheel_val=wh; g_wheel_init=1; }
              else if(wh!=g_wheel_val){ int dv=wh-g_wheel_val; if(dv<-8)dv=-8; if(dv>8)dv=8;
                  g_wheel_acc+=dv; g_wheel_val=wh; ev|=MEV_WHEEL; } }
        }
    }
    if(ev) g_mseen = 1;
    return ev;
}

#endif
