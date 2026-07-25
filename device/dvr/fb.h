/* fb.h — HiFB OSD graphics layer for the on-screen DVR UI (over the VGA video).
 * /dev/fb0 is a standard Linux framebuffer (1280x1024 RGB565 by default) with HiFB
 * alpha/colorkey extensions (magic 'F'=0x46). We mmap it and draw the UI; the hardware
 * composites it over the VO video. Readable, so the PC can screenshot the OSD too.
 * Requires oabi.h. Link -lgcc (division). memset/memcpy come from dvr.c.
 */
#ifndef DVR_FB_H
#define DVR_FB_H

#include "oabi.h"
#include "font8x8.h"
void *memset(void *d, int c, size_t n);
void *memcpy(void *d, const void *s, size_t n);

/* standard Linux fb ioctls */
#define FBIOGET_VSCREENINFO 0x4600
#define FBIOPUT_VSCREENINFO 0x4601
#define FBIOGET_FSCREENINFO 0x4602
/* HiFB */
#define FBIOPUT_ALPHA_HIFB  0x400c465d
#define FBIOPUT_SHOW_HIFB   0x40044665   /* _IOW('F',101,HI_BOOL) */

static int   g_fbfd = -1;
static unsigned char *g_fbmem = 0;        /* the real mmap'd fb (scanned out to VGA) */
static unsigned char *g_shadow = 0;       /* RAM draw buffer — all drawing lands here first */
static unsigned char  g_shadowbuf[1280*1024*2];   /* backing store for g_shadow (max 1280x1024x2) */
static unsigned g_fbw = 0, g_fbh = 0, g_fbbpp = 0, g_fbstride = 0, g_fblen = 0;
static unsigned g_fbvw = 0, g_fbvh = 0;   /* xres_virtual/yres_virtual read back (clamp check) */

/* open /dev/fb0; if w,h>0 set that resolution (16bpp) to match the VGA output */
static int fb_open_mode(unsigned setw, unsigned seth){
    unsigned v[48], f[48];
    g_fbfd = (int)sys_open("/dev/fb0", O_RDWR, 0);
    if(g_fbfd < 0) return -1;
    memset(v,0,sizeof(v));
    if(sys_ioctl(g_fbfd, FBIOGET_VSCREENINFO, v) != 0) return -1;
    if(setw && seth){                                  /* resize the OSD to match the screen */
        /* replicate app.out's FUN_00095bac: GET-first then patch in place (do NOT zero the
         * rest of the struct), ARGB1555 pixel format. xres_virtual/yres_virtual drive the
         * mmappable smem_len; HiFB clamps them to the VO graphics-layer canvas, so the VGA VO
         * must already be up at 1280x1024 (display_vga) before this runs. See DISPLAY_PATH.md. */
        v[0]=setw; v[1]=seth;                           /* xres, yres */
        v[2]=setw; v[3]=seth;                           /* xres_virtual, yres_virtual */
        v[6]=16;                                        /* bits_per_pixel */
        v[8]=10; v[9]=5;  v[10]=0;                       /* red   offset/len/msb  (ARGB1555) */
        v[11]=5; v[12]=5; v[13]=0;                       /* green */
        v[14]=0; v[15]=5; v[16]=0;                       /* blue  */
        v[17]=15; v[18]=1; v[19]=0;                      /* transp (top bit) */
        sys_ioctl(g_fbfd, FBIOPUT_VSCREENINFO, v);
        sys_ioctl(g_fbfd, FBIOGET_VSCREENINFO, v);      /* read back what stuck */
    }
    g_fbw = v[0]; g_fbbpp = v[6];                      /* xres@0, bpp@24 */
    g_fbvw = v[2]; g_fbvh = v[3];                      /* virtual dims (clamp discriminator) */
    memset(f,0,sizeof(f));
    sys_ioctl(g_fbfd, FBIOGET_FSCREENINFO, f);
    g_fbstride = f[11];                                /* line_length @ 44 */
    unsigned smem = f[5];                              /* smem_len @ 20 (real backing store) */
    if(g_fbbpp==0) g_fbbpp=16;
    if(g_fbstride==0) g_fbstride = g_fbw * (g_fbbpp/8);
    /* The real mmappable size is smem_len. If HiFB clamped the virtual dims to the VO
     * graphics-layer canvas, smem stays small even though yres reads back large — so we
     * cap the drawable height to what is actually backed (never write past it -> no SIGSEGV).
     * g_fbh may end up < reported yres; the UI adapts to g_fbw/g_fbh. */
    g_fblen = (smem && smem < g_fbstride * v[1]) ? smem : g_fbstride * v[1];
    g_fbh = (g_fbstride ? g_fblen / g_fbstride : v[1]);
    if(g_fbh > v[1]) g_fbh = v[1];
    g_fbmem = (unsigned char*)sys_mmap2(0, g_fblen, PROT_READ|PROT_WRITE, MAP_SHARED, g_fbfd, 0);
    if((long)g_fbmem == -1 || (long)g_fbmem < 0){ g_fbmem=0; return -1; }
    /* draw into a RAM shadow, then blit changed regions to the real fb (no blank frame ->
     * no flicker on a single-buffered fb). Falls back to direct if the buffer is too small. */
    g_shadow = (g_fblen <= sizeof(g_shadowbuf)) ? g_shadowbuf : g_fbmem;
    return 0;
}
static int fb_open(void){ return fb_open_mode(0,0); }
/* fully-transparent pixel value (video shows through): ARGB1555 top bit 0 */
#define FB_CLEAR 0x0000u
/* pack an OPAQUE color for the fb's bpp (ARGB1555: top alpha bit set) */
static unsigned fb_rgb(int r,int g,int b){
    if(g_fbbpp==16) return 0x8000u | (unsigned)(((r>>3)<<10)|((g>>3)<<5)|(b>>3));  /* ARGB1555 */
    return 0xff000000u | ((unsigned)r<<16) | ((unsigned)g<<8) | (unsigned)b;       /* ARGB8888 */
}
/* all drawing targets the shadow buffer */
static void fb_px(int x,int y,unsigned c){
    if(!g_shadow || x<0||y<0||x>=(int)g_fbw||y>=(int)g_fbh) return;
    unsigned char *p = g_shadow + (unsigned)y*g_fbstride + (unsigned)x*(g_fbbpp/8);
    if(g_fbbpp==16){ p[0]=c&0xff; p[1]=(c>>8)&0xff; }
    else { p[0]=c&0xff; p[1]=(c>>8)&0xff; p[2]=(c>>16)&0xff; p[3]=(c>>24)&0xff; }
}
/* copy a w x h ARGB1555 image (row-major u16) into the shadow at (x,y) — for thumbnails */
static void fb_blit_argb(int x, int y, const unsigned short *src, int w, int h){
    if(!g_shadow || !src || g_fbbpp!=16) return;
    int i,j;
    for(j=0;j<h;j++){ int yy=y+j; if(yy<0||yy>=(int)g_fbh) continue;
        unsigned char *row = g_shadow + (unsigned)yy*g_fbstride;
        for(i=0;i<w;i++){ int xx=x+i; if(xx<0||xx>=(int)g_fbw) continue;
            unsigned short c=src[j*w+i]; row[xx*2]=c&0xff; row[xx*2+1]=(c>>8)&0xff; } }
}
/* blit a region from the shadow to the real fb (memcpy per row — never blanks) */
static void fb_blit(int x,int y,int w,int h){
    if(!g_fbmem || !g_shadow || g_shadow==g_fbmem) return;
    if(x<0){ w+=x; x=0; } if(y<0){ h+=y; y=0; }
    if(x+w>(int)g_fbw) w=(int)g_fbw-x;
    if(y+h>(int)g_fbh) h=(int)g_fbh-y;
    if(w<=0||h<=0) return;
    int bpp=g_fbbpp/8, j;
    for(j=0;j<h;j++){
        unsigned off=(unsigned)(y+j)*g_fbstride + (unsigned)x*bpp;
        memcpy(g_fbmem+off, g_shadow+off, (unsigned)w*bpp);
    }
}
/* write a pixel straight to the REAL fb (used only to composite the cursor on top) */
static void fbreal_px(int x,int y,unsigned c){
    if(!g_fbmem || x<0||y<0||x>=(int)g_fbw||y>=(int)g_fbh) return;
    unsigned char *p = g_fbmem + (unsigned)y*g_fbstride + (unsigned)x*(g_fbbpp/8);
    p[0]=c&0xff; p[1]=(c>>8)&0xff;
}
static void fb_fill(unsigned c){
    if(!g_shadow) return;
    /* fast path: a solid fill whose two bytes are equal (esp. FB_CLEAR=0) is a byte-fill
     * over the whole draw buffer. MUST target g_shadow (the draw buffer), not g_fbmem —
     * else the shadow keeps stale content and fb_blit copies it back (menu wouldn't close). */
    if(g_fbbpp==16){
        unsigned lo=c&0xff, hi=(c>>8)&0xff;
        if(lo==hi){ memset(g_shadow, (int)lo, g_fblen); return; }
    }
    unsigned x,y;
    for(y=0;y<g_fbh;y++) for(x=0;x<g_fbw;x++) fb_px((int)x,(int)y,c);
}
static void fb_rect(int x,int y,int w,int h,unsigned c){
    int i,j; for(j=0;j<h;j++) for(i=0;i<w;i++) fb_px(x+i,y+j,c);
}
static void fb_border(int x,int y,int w,int h,int t,unsigned c){
    fb_rect(x,y,w,t,c); fb_rect(x,y+h-t,w,t,c);
    fb_rect(x,y,t,h,c); fb_rect(x+w-t,y,t,h,c);
}
/* ---- text (8x8 bitmap font, integer-scaled) -------------------------------
 * fb_char draws one glyph at pixel (x,y); scale>=1 blows each font pixel up to
 * a scale x scale block. col = foreground; if bg has bit 0x80000000 set it is
 * transparent (draw fg pixels only), else the glyph cell is filled with bg. */
#define FB_TRANSPARENT 0x80000000u
static void fb_char(int x,int y,unsigned char ch,unsigned col,unsigned bg,int scale){
    if(ch<0x20 || ch>0x7e) ch=0x20;
    const unsigned char *g = FONT8X8[ch-0x20];
    int row,cx,sx,sy;
    for(row=0;row<8;row++){
        unsigned char bits = g[row];
        for(cx=0;cx<8;cx++){
            int on = (bits>>cx)&1;
            unsigned c = on ? col : bg;
            if(!on && (bg&FB_TRANSPARENT)) continue;
            for(sy=0;sy<scale;sy++) for(sx=0;sx<scale;sx++)
                fb_px(x+cx*scale+sx, y+row*scale+sy, c);
        }
    }
}
static int fb_text(int x,int y,const char *s,unsigned col,unsigned bg,int scale){
    int x0=x;
    for(; *s; s++){
        if(*s=='\n'){ y += 8*scale; x = x0; continue; }
        fb_char(x,y,(unsigned char)*s,col,bg,scale);
        x += 8*scale;
    }
    return x;
}
/* width in pixels of a string at a given scale (8px per glyph) */
static int fb_textw(const char *s,int scale){ int n=0; for(;*s;s++) n++; return n*8*scale; }

/* ---- software mouse cursor: a little jet, composited onto the real fb ------
 * The cursor lives only on the real fb (never in the shadow). To move it we blit
 * its old rect back from the shadow (erase) and draw the sprite at the new spot.
 * Because the clean UI lives in the shadow, this needs no per-cursor backing store
 * and never blanks. Hotspot = the nose (top-centre). 'X'=outline, '.'=fill. */
/* paper-airplane cursor (from plane cursor.png), pointing up-left; nose tip = hotspot (0,0) */
#define CUR_W 31
#define CUR_H 26
#define CUR_HOTX 0
#define CUR_HOTY 0
static const char *CURSOR_SPR[CUR_H] = {
    "XXXX",
    "XX..XX",
    "X.XX..XX",
    "X..XX...XX.",
    "X...XX....XX.",
    "X....XX.....XXXX",
    " X....XXX.......XXX",
    " X.....XXX.........XXX",
    " X.......XX...........XXX..",
    " X........XX.............XX...",
    " X........XXX..............XXX.",
    "  X.........XX................X",
    "  X.........XXXXX........XXX",
    "  X..........XXXXXX...XXX.",
    "  X..........XXXXXXXXX",
    "   X...........XXXXXX",
    "   X..........XXXXXXX",
    "   X.........XXXXXXXX",
    "   X........XXXXXXXXX",
    "    X......XXXXXXXXXX",
    "    X.....X.XXXXXXXXX",
    "    X...... XXXXXXXXX",
    "    X...XX   XXXXXXXX",
    "    X..X      XXXXXXX",
    "     XX        XXXXXX",
    "                 ..XX",
};
/* ---- cursor overlay: a dedicated, double-buffered layer (/dev/fb4), app.out's way.
 * The jet lives on fb4, NOT on the menu layer (fb0). Moving it draws into the hidden
 * back buffer then FBIOPAN_DISPLAY flips it — so the menu/video layer is never touched
 * (no flash) and only fully-drawn frames scan out (no cursor tear). */
#define FBIOPAN_DISPLAY 0x4606
static int g_cofd=-1;
static unsigned char *g_comem=0;
static unsigned g_cow=0, g_coh=0, g_costride=0;
static int g_cobuf=0;                        /* currently-shown buffer (0/1) */
static int g_colx[2]={0,0}, g_coly[2]={0,0}; /* last jet top-left (can be negative near edges) */
static int g_cur_shown=0;                    /* 1 once a cursor has been drawn (to erase) */
static int g_cuscale=1;
static void co_px(int buf,int x,int y,unsigned c){
    if(!g_comem||x<0||y<0||x>=(int)g_cow||y>=(int)g_coh) return;
    unsigned char *p=g_comem + (unsigned)(buf*(int)g_coh+y)*g_costride + (unsigned)x*2;
    p[0]=c&0xff; p[1]=(c>>8)&0xff;
}
static void co_jet(int buf,int x,int y){     /* draw the scaled jet into buffer `buf` */
    int s=g_cuscale, i,j,sx,sy;
    for(j=0;j<CUR_H;j++){ const char *r=CURSOR_SPR[j];
        for(i=0;i<CUR_W && r[i]; i++){
            unsigned c; if(r[i]=='X') c=0x8000; else if(r[i]=='.') c=0xffff; else continue;
            for(sy=0;sy<s;sy++) for(sx=0;sx<s;sx++) co_px(buf,x+i*s+sx,y+j*s+sy,c);
        } }
}
static void co_clear(int buf,int x,int y,int w,int h){
    int i,j; for(j=0;j<h;j++) for(i=0;i<w;i++) co_px(buf,x+i,y+j,0x0000);   /* transparent */
}
/* open + configure the cursor overlay at the screen size, double-buffered (ARGB1555) */
static int co_open(void){
    unsigned v[48], f[48];
    if(g_cofd>=0) return 0;                            /* idempotent */
    g_cofd=(int)sys_open("/dev/fb4", O_RDWR, 0);
    if(g_cofd<0) return -1;
    memset(v,0,sizeof(v));
    if(sys_ioctl(g_cofd, FBIOGET_VSCREENINFO, v)!=0){ sys_close(g_cofd); g_cofd=-1; return -1; }
    unsigned w=g_fbw, h=g_fbh;
    v[0]=w; v[1]=h; v[2]=w; v[3]=h;                   /* single buffer (page-flip tears w/o vsync) */
    v[6]=16;
    v[8]=10;v[9]=5;v[10]=0; v[11]=5;v[12]=5;v[13]=0;  /* ARGB1555 */
    v[14]=0;v[15]=5;v[16]=0; v[17]=15;v[18]=1;v[19]=0;
    sys_ioctl(g_cofd, FBIOPUT_VSCREENINFO, v);
    sys_ioctl(g_cofd, FBIOGET_VSCREENINFO, v);
    g_cow=v[0]; g_coh=v[1];
    memset(f,0,sizeof(f)); sys_ioctl(g_cofd, FBIOGET_FSCREENINFO, f);
    g_costride=f[11]; if(!g_costride) g_costride=g_cow*2;
    unsigned len=g_costride*g_coh, smem=f[5];
    if(smem && smem<len) len=smem;
    g_comem=(unsigned char*)sys_mmap2(0, len, PROT_READ|PROT_WRITE, MAP_SHARED, g_cofd, 0);
    if((long)g_comem==-1 || (long)g_comem<0){ g_comem=0; sys_close(g_cofd); g_cofd=-1; return -1; }
    memset(g_comem, 0, len);                          /* both buffers transparent */
    g_cuscale=1;   /* the imported sprite (31x26) is already full size */
    { unsigned char a[12]; memset(a,0,12); a[0]=1; a[9]=0xff;     /* per-pixel alpha */
      sys_ioctl(g_cofd, FBIOPUT_ALPHA_HIFB, a); }
    { unsigned one=1; sys_ioctl(g_cofd, FBIOPUT_SHOW_HIFB, &one); }
    g_cobuf=0; g_cur_shown=0;
    return 0;
}
static void co_pan(int buf){
    unsigned v[48]; if(g_cofd<0) return;
    if(sys_ioctl(g_cofd, FBIOGET_VSCREENINFO, v)!=0) return;
    v[4]=0; v[5]=(unsigned)(buf*(int)g_coh);          /* yoffset = buf*yres */
    sys_ioctl(g_cofd, FBIOPAN_DISPLAY, v);
}
/* move the jet so its nose is at (hotx,hoty): erase the old sprite, draw the new one,
 * directly on the (single-buffer) cursor layer. The sprite is tiny so the erase→draw
 * window is microseconds — a scanout rarely catches it, and it never touches fb0. */
static void cursor_move(int hotx,int hoty){
    if(g_cofd<0 || !g_comem) return;
    int s=g_cuscale, x=hotx-CUR_HOTX*s, y=hoty-CUR_HOTY*s;
    if(g_cur_shown) co_clear(0, g_colx[0], g_coly[0], CUR_W*s, CUR_H*s);  /* x/y may be <0 at edges */
    co_jet(0, x, y);
    g_colx[0]=x; g_coly[0]=y; g_cur_shown=1;
}

/* show/hide + full opacity, and dump the fb to a file (PC pulls + renders) */
static void fb_show(int on){ unsigned b=on?1:0; if(g_fbfd>=0) sys_ioctl(g_fbfd, FBIOPUT_SHOW_HIFB, &b); }
static void fb_alpha_opaque(void){
    if(g_fbfd<0) return;
    unsigned char a[12]; memset(a,0,12);
    a[0]=1;        /* bAlphaEnable */
    a[9]=0xff;     /* u8GlobalAlpha (matches app.out's 00 ff 00 00 tail) */
    sys_ioctl(g_fbfd, FBIOPUT_ALPHA_HIFB, a);
}
static void fb_dump(const char *path){
    int fd = (int)sys_open(path, O_WRONLY|O_CREAT|O_TRUNC, 0644);
    if(fd<0 || !g_fbmem) return;
    unsigned y, maxrows = g_fblen / (g_fbstride ? g_fbstride : 1);  /* clamp to mapped buffer */
    unsigned rows = g_fbh < maxrows ? g_fbh : maxrows;
    for(y=0;y<rows;y++) sys_write(fd, g_fbmem + (unsigned)y*g_fbstride, g_fbw*(g_fbbpp/8));
    sys_close(fd);
}

#endif
