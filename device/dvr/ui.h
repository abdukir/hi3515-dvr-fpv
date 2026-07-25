/* ui.h — on-screen VGA UI (OSD) for the DVR: a compact status HUD plus a menu
 * system overlaid on the live video. Self-contained: depends only on fb.h for
 * drawing. Navigation model matches the physical controls we will wire to the
 * MCU over ttyAMA1: '-' (up/prev), '+' (down/next), 'M' (menu/enter),
 * 'X' (exit/back). A dedicated hardware switch drives REC directly (not here).
 *
 * Design: ui.h owns only the UI *model + rendering*. It never touches device
 * state directly — ui_key() returns an action code and dvr.c applies it (toggle
 * recording, set standard, reboot, poke picture registers...), then refreshes
 * the cached fields in ui_state and calls ui_render(). This keeps the menu logic
 * testable and free of MPP/driver coupling.
 *
 * Compositing: the OSD is drawn on fb0 over the VO video. Black (0,0,0) is the
 * intended colorkey (see-through to video); everything else is opaque overlay.
 * Keep menu backgrounds non-black so panels read as solid over live video.
 */
#ifndef DVR_UI_H
#define DVR_UI_H

#include "fb.h"

/* ---- menu identifiers ----------------------------------------------------- */
#define MENU_MAIN     0
#define MENU_SETTINGS 1
#define MENU_PICTURE  2
#define MENU_RECORD   3
#define MENU_INFO     4   /* connection / logs (read-only info screen) */
#define MENU_PLAYBACK 5   /* recordings list (populated by dvr.c) */
#define PB_VISROWS   12   /* max recording rows shown at once (rest scroll) */

/* ---- item kinds ----------------------------------------------------------- */
#define KIND_SUB   0   /* target = menu id to descend into */
#define KIND_ACT   1   /* target = action id (fires on enter) */
#define KIND_VAL   2   /* target = value id (toggle on enter, or +/- when editing) */

/* ---- value ids (KIND_VAL) ------------------------------------------------- */
#define VAL_STD      0   /* NTSC / PAL / AUTO  (toggle) */
#define VAL_DISPLAY  1   /* fill / pillarbox   (toggle) */
#define VAL_BRIGHT   2   /* 0..255 picture     (edit +/-) */
#define VAL_CONTRAST 3
#define VAL_HUE      4
#define VAL_SAT      5
#define VAL_REC0     6   /* per-channel record toggles */
#define VAL_REC1     7
#define VAL_REC2     8
#define VAL_REC3     9
#define VAL_RECALL   10  /* all channels */
#define VAL_CHANNEL  11  /* displayed VGA channel (cycles CH1..4) */
#define VAL_SOUND    12  /* buzzer feedback on menu navigation (toggle) */

/* ---- action ids (KIND_ACT) ------------------------------------------------ */
#define ACT_NONE     0
#define ACT_BACK     1   /* pop the menu stack (or close at root) */
#define ACT_REBOOT   2
#define ACT_CLOSE    3
#define ACT_PICRESET 4   /* reset picture (bright/contrast/hue/sat) to defaults */

/* actions ui_key() returns to dvr.c (superset of KIND_ACT + value effects) */
#define UI_ACT_NONE        0
#define UI_ACT_REC         1   /* a = channel 0..3, or -1 for ALL; b = 1 on / 0 off */
#define UI_ACT_STD         2   /* a = new standard (1 NTSC / 2 PAL / 0 AUTO) */
#define UI_ACT_DISPLAY     3   /* a = new display (0 fill / 1 pillarbox) */
#define UI_ACT_PICTURE     4   /* a = VAL_BRIGHT..VAL_SAT; b = signed delta */
#define UI_ACT_REBOOT      5
#define UI_ACT_OPEN_PB     6   /* user opened Playback — dvr.c should list files */
#define UI_ACT_PB_PLAY     7   /* a = selected recording index */
#define UI_ACT_CHANNEL     8   /* a = new displayed channel 0..3 */
#define UI_ACT_PICRESET    9   /* reset picture to defaults on the shown channel */
#define UI_ACT_SOUND      10   /* a = 1 sounds on / 0 off */

typedef struct { const char *label; int kind; int target; } menu_item;

/* Static menu tables. Value rows render their live value on the right. */
static const menu_item MI_MAIN[] = {
    {"Channel",  KIND_VAL, VAL_CHANNEL},
    {"Settings", KIND_SUB, MENU_SETTINGS},
    {"Playback", KIND_SUB, MENU_PLAYBACK},
    {"Logs",     KIND_SUB, MENU_INFO},
    {"Reboot",   KIND_ACT, ACT_REBOOT},
    {"Close",    KIND_ACT, ACT_CLOSE},
};
static const menu_item MI_SETTINGS[] = {
    {"Picture",   KIND_SUB, MENU_PICTURE},
    {"Standard",  KIND_VAL, VAL_STD},
    {"Recording", KIND_SUB, MENU_RECORD},
    {"Display",   KIND_VAL, VAL_DISPLAY},
    {"Sound",     KIND_VAL, VAL_SOUND},
    {"Info",      KIND_SUB, MENU_INFO},
    {"Back",      KIND_ACT, ACT_BACK},
};
static const menu_item MI_PICTURE[] = {
    {"Brightness", KIND_VAL, VAL_BRIGHT},
    {"Contrast",   KIND_VAL, VAL_CONTRAST},
    {"Hue",        KIND_VAL, VAL_HUE},
    {"Saturation", KIND_VAL, VAL_SAT},
    {"Reset",      KIND_ACT, ACT_PICRESET},
    {"Back",       KIND_ACT, ACT_BACK},
};
static const menu_item MI_RECORD[] = {
    {"Rec CH1", KIND_VAL, VAL_REC0},
    {"Rec CH2", KIND_VAL, VAL_REC1},
    {"Rec CH3", KIND_VAL, VAL_REC2},
    {"Rec CH4", KIND_VAL, VAL_REC3},
    {"Rec ALL", KIND_VAL, VAL_RECALL},
    {"Back",    KIND_ACT, ACT_BACK},
};
static const menu_item MI_INFO[] = {
    {"Back", KIND_ACT, ACT_BACK},
};

/* ---- UI state (dvr.c owns one instance) ----------------------------------- */
typedef struct {
    int  open;            /* 0 = HUD only, 1 = menu open */
    int  menu;            /* current menu id */
    int  sel;             /* selected row */
    int  editing;         /* 1 = adjusting a numeric value with +/- */
    int  stack[8];        /* menu navigation stack (menu ids) */
    int  selstk[8];       /* saved selection per level */
    int  sp;              /* stack depth */

    /* cached device state, refreshed by dvr.c before each render */
    int  chan;            /* displayed channel 0..3 */
    int  rec[4];          /* 0 off, 1 recording, 2 wanted-not-yet */
    int  standard;        /* 1 NTSC, 2 PAL, 0 AUTO */
    int  display;         /* 0 fill, 1 pillarbox */
    int  sound;           /* 1 = buzzer feedback on menu navigation */
    int  pic[4];          /* bright, contrast, hue, sat  (0..255) */
    char timestr[16];     /* "HH:MM:SS" */
    int  rec_secs;        /* elapsed seconds recording the shown channel (0 if idle) */

    /* playback list (filled by dvr.c) */
    const char *pb_names[64]; /* up to 64 recording labels (newest first) */
    int  pb_count;
    int  pb_top;          /* scroll offset (index of the first visible row) */

    /* last action detail (set by ui_key, read by dvr.c) */
    int  act_a, act_b;
} ui_state;

static void ui_init(ui_state *u){
    memset(u, 0, sizeof(*u));
    u->menu = MENU_MAIN;
    u->standard = 1;
    u->pic[0]=u->pic[1]=u->pic[2]=u->pic[3]=128;
    u->timestr[0]='-'; u->timestr[1]=0;
}

/* resolve the item table + count for a menu id */
static const menu_item *ui_items(int menu, int *n){
    switch(menu){
        case MENU_MAIN:     *n=(int)(sizeof(MI_MAIN)/sizeof(MI_MAIN[0]));     return MI_MAIN;
        case MENU_SETTINGS: *n=(int)(sizeof(MI_SETTINGS)/sizeof(MI_SETTINGS[0])); return MI_SETTINGS;
        case MENU_PICTURE:  *n=(int)(sizeof(MI_PICTURE)/sizeof(MI_PICTURE[0]));  return MI_PICTURE;
        case MENU_RECORD:   *n=(int)(sizeof(MI_RECORD)/sizeof(MI_RECORD[0]));    return MI_RECORD;
        case MENU_INFO:     *n=(int)(sizeof(MI_INFO)/sizeof(MI_INFO[0]));        return MI_INFO;
        default:            *n=0; return 0;
    }
}
static const char *ui_menu_title(int menu){
    switch(menu){
        case MENU_MAIN:     return "MENU";
        case MENU_SETTINGS: return "SETTINGS";
        case MENU_PICTURE:  return "PICTURE";
        case MENU_RECORD:   return "RECORDING";
        case MENU_INFO:     return "INFO / LOGS";
        case MENU_PLAYBACK: return "PLAYBACK";
        default:            return "MENU";
    }
}

/* render the right-hand value text for a KIND_VAL row into out[] */
static void ui_val_text(const ui_state *u, int val, char *out){
    int i=0, v;
    switch(val){
        case VAL_STD:
            if(u->standard==2){ out[0]='P';out[1]='A';out[2]='L';out[3]=0; }
            else if(u->standard==1){ out[0]='N';out[1]='T';out[2]='S';out[3]='C';out[4]=0; }
            else { out[0]='A';out[1]='U';out[2]='T';out[3]='O';out[4]=0; }
            return;
        case VAL_DISPLAY:
            if(u->display==1){ const char*s="PILLARBOX"; while(*s) out[i++]=*s++; out[i]=0; }
            else { const char*s="FILL"; while(*s) out[i++]=*s++; out[i]=0; }
            return;
        case VAL_SOUND: {
            const char*s = u->sound ? "ON" : "OFF";
            while(*s) out[i++]=*s++; out[i]=0;
            return; }
        case VAL_BRIGHT: case VAL_CONTRAST: case VAL_HUE: case VAL_SAT:
            v = u->pic[val-VAL_BRIGHT];
            out[0]='0'+((v/100)%10); out[1]='0'+((v/10)%10); out[2]='0'+(v%10); out[3]=0;
            return;
        case VAL_REC0: case VAL_REC1: case VAL_REC2: case VAL_REC3: {
            int r = u->rec[val-VAL_REC0];
            const char *s = r==1 ? "REC" : (r==2 ? "WAIT" : "OFF");
            while(*s) out[i++]=*s++; out[i]=0; return;
        }
        case VAL_RECALL: {
            int any=0,c; for(c=0;c<4;c++) if(u->rec[c]) any=1;
            const char *s = any ? "STOP ALL" : "REC ALL";
            while(*s) out[i++]=*s++; out[i]=0; return;
        }
        case VAL_CHANNEL:
            out[0]='C'; out[1]='H'; out[2]='0'+((u->chan+1)%10); out[3]=0; return;
        default: out[0]=0; return;
    }
}

static int ui_is_numeric(int val){ return val>=VAL_BRIGHT && val<=VAL_SAT; }

/* ---- geometry (shared by the renderer and the mouse hit-test) ------------- */
typedef struct { int px,py,pw,ph,rowh,titleh,ry0,scale,nrows; } ui_geom;
static void ui_menu_geom(const ui_state *u, ui_geom *g){
    int scale = (g_fbw >= 1000) ? 3 : 2;      /* bigger menu = easier to read + click */
    int rowh  = 12*scale;
    int n=0; ui_items(u->menu, &n);
    int vis = u->pb_count < PB_VISROWS ? u->pb_count : PB_VISROWS;   /* visible playback rows */
    int nrows = (u->menu==MENU_PLAYBACK) ? vis : n;
    int rows  = (u->menu==MENU_PLAYBACK && u->pb_count==0) ? 1 : nrows;
    int pw = (g_fbw >= 1000) ? 660 : 420;
    int titleh = rowh + 6*scale;
    g->scale=scale; g->rowh=rowh; g->titleh=titleh; g->pw=pw; g->nrows=nrows;
    g->ph = titleh + rows*rowh + 8*scale;
    g->px = ((int)g_fbw - pw)/2;
    g->py = ((int)g_fbh - g->ph)/2;
    g->ry0 = g->py + titleh + 2*scale;
}
/* build the on-screen text for a numeric picture row: "- DDD +" (always 7 chars) */
static void ui_num_text(const ui_state *u, int val, char *out){
    char d[8]; ui_val_text(u, val, d);   /* 3 digits */
    out[0]='-'; out[1]=' '; out[2]=d[0]; out[3]=d[1]; out[4]=d[2]; out[5]=' '; out[6]='+'; out[7]=0;
}

/* ---- rendering ------------------------------------------------------------ */
/* status HUD: compact strip at the top-left — "CH1  <REC>  HH:MM:SS".
 * Drawn with a thin dark plate behind the text for legibility over video. */
/* bounds of the clickable REC button on the HUD (set by ui_draw_hud, hit-tested by ui_mouse) */
static int g_hud_rec_x=0, g_hud_rec_y=0, g_hud_rec_w=0, g_hud_rec_h=0;
static void ui_draw_hud(const ui_state *u){
    int scale = (g_fbw >= 1000) ? 2 : 1;
    int gh = 8*scale, pad = 4*scale, gap = 6*scale;
    int x = 8, y = 8, ty = y + pad;
    unsigned plate = fb_rgb(0,0,40);
    unsigned fg    = fb_rgb(235,235,235);
    unsigned cyan  = fb_rgb(0,220,220);
    unsigned recon = fb_rgb(230,30,30);          /* recording (bright red) */
    unsigned recoff= fb_rgb(90,95,105);          /* idle button */
    unsigned white = fb_rgb(255,255,255);
    int recording = (u->chan>=0 && u->chan<4) ? (u->rec[u->chan]!=0) : 0;
    /* elapsed "MM:SS" (or the REC label when idle) */
    char rb[10];
    if(recording){ int s=u->rec_secs, m=(s/60)%100; s%=60;
        rb[0]='0'+(m/10);rb[1]='0'+(m%10);rb[2]=':';rb[3]='0'+(s/10);rb[4]='0'+(s%10);rb[5]=0; }
    else { rb[0]='R';rb[1]='E';rb[2]='C';rb[3]=0; }
    char chn[4]={'C','H',(char)('0'+u->chan+1),0};
    int chw=fb_textw(chn,scale), rbw=fb_textw(rb,scale), tw=fb_textw("00:00:00",scale);
    int btnw = rbw + pad*2 + (recording? gh:0);   /* room for the dot when recording */
    int total = pad + chw + gap + btnw + gap + tw + pad;
    fb_rect(x, y, total, gh + pad*2, plate);
    int cx = x + pad;
    cx = fb_text(cx, ty, chn, cyan, FB_TRANSPARENT, scale); cx += gap;
    /* REC button box */
    g_hud_rec_x=cx; g_hud_rec_y=y; g_hud_rec_w=btnw; g_hud_rec_h=gh+pad*2;
    fb_rect(cx, y+scale, btnw, gh+pad*2-2*scale, recording?recon:recoff);
    int bx = cx + pad;
    if(recording){ fb_rect(bx, ty+scale, gh-2*scale, gh-2*scale, white); bx += gh; }  /* ● dot */
    fb_text(bx, ty, rb, white, FB_TRANSPARENT, scale);
    cx += btnw + gap;
    fb_text(cx, ty, u->timestr, fg, FB_TRANSPARENT, scale);
}

/* ---- full-screen recordings grid (MENU_PLAYBACK) ------------------------------------------
 * A whole-screen browser: COLS x ROWS cells, each a recording with a thumbnail area + label.
 * dvr.c may fill g_pb_thumb[i] with a THUMB_W x THUMB_H ARGB1555 image (NULL = placeholder). */
#define PB_COLS 3
#define PB_ROWS 3
#define PB_PAGE (PB_COLS*PB_ROWS)
#define THUMB_W 176
#define THUMB_H 120
static const unsigned short *g_pb_thumb[64];   /* per-recording thumbnail (ARGB1555) or NULL */
typedef struct { int x0,y0,cw,ch,gap,scale,thh; } pbgrid_geom;
static void ui_pbgrid_geom(pbgrid_geom *g){
    int scale=(g_fbw>=1000)?2:1;
    int margin=18*scale, titleh=12*scale+6*scale, gap=10*scale;
    int gw=(int)g_fbw - margin*2, gh=(int)g_fbh - margin - titleh - margin;
    g->x0=margin; g->y0=margin+titleh;
    g->cw=(gw-(PB_COLS-1)*gap)/PB_COLS;
    g->ch=(gh-(PB_ROWS-1)*gap)/PB_ROWS;
    g->gap=gap; g->scale=scale; g->thh=g->ch*7/10;   /* thumbnail area height */
}
static void ui_pbgrid_cell(const pbgrid_geom *g, int slot, int *cx, int *cy){
    int r=slot/PB_COLS, c=slot%PB_COLS;
    *cx=g->x0 + c*(g->cw+g->gap); *cy=g->y0 + r*(g->ch+g->gap);
}
static int ui_pbgrid_hit(const ui_state *u, int mx, int my){   /* -> recording index, or -1 */
    if(u->pb_count<=0) return -1;
    pbgrid_geom g; ui_pbgrid_geom(&g);
    int page=u->sel/PB_PAGE, start=page*PB_PAGE, i;
    for(i=0;i<PB_PAGE && start+i<u->pb_count;i++){
        int cx,cy; ui_pbgrid_cell(&g,i,&cx,&cy);
        if(mx>=cx && mx<cx+g.cw && my>=cy && my<cy+g.ch) return start+i;
    }
    return -1;
}
/* draw ONE grid cell (border + thumbnail-or-placeholder + label) for on-page position `slot`.
 * Split out so dvr.c can repaint a single cell when a thumbnail finishes generating (pop-in). */
static void ui_draw_pbcell(const ui_state *u, const pbgrid_geom *g, int slot){
    int scale=g->scale, idx=(u->sel/PB_PAGE)*PB_PAGE + slot;
    if(idx<0 || idx>=u->pb_count) return;
    unsigned bord=fb_rgb(0,190,200), cell=fb_rgb(22,30,46), selbg=fb_rgb(0,90,100);
    unsigned fg=fb_rgb(236,239,243), dim=fb_rgb(150,160,175), thumbbg=fb_rgb(14,18,28);
    int cx,cy; ui_pbgrid_cell(g,slot,&cx,&cy);
    int selc=(idx==u->sel);
    fb_rect(cx,cy,g->cw,g->ch, selc?selbg:cell);
    if(selc) fb_border(cx,cy,g->cw,g->ch, scale, bord);
    int tx=cx+3*scale, ty=cy+3*scale, tw=g->cw-6*scale, th=g->thh-3*scale;
    fb_rect(tx,ty,tw,th, thumbbg);
    if(idx<64 && g_pb_thumb[idx]) fb_blit_argb(tx+(tw-THUMB_W)/2, ty+(th-THUMB_H)/2, g_pb_thumb[idx], THUMB_W, THUMB_H);
    else { const char *p="[ no preview ]"; fb_text(tx+(tw-fb_textw(p,scale))/2, ty+th/2-4*scale, p, dim, FB_TRANSPARENT, scale); }
    fb_text(cx+8*scale, cy+g->thh+6*scale, u->pb_names[idx], fg, FB_TRANSPARENT, scale);
}
static void ui_draw_pbgrid(const ui_state *u){
    pbgrid_geom g; ui_pbgrid_geom(&g); int scale=g.scale;
    unsigned bg=fb_rgb(8,10,18), bord=fb_rgb(0,190,200), cell=fb_rgb(22,30,46), selbg=fb_rgb(0,90,100);
    unsigned fg=fb_rgb(236,239,243), dim=fb_rgb(150,160,175), thumbbg=fb_rgb(14,18,28), bar=fb_rgb(0,90,140);
    fb_rect(0,0,(int)g_fbw,(int)g_fbh, bg);
    fb_rect(0,0,(int)g_fbw, g.y0-6*scale, bar);
    fb_text(g.x0, 6*scale, "PLAYBACK", fg, FB_TRANSPARENT, scale);
    if(u->pb_count==0){ fb_text(g.x0, g.y0+8*scale, "(no recordings — start one from the Live tab)", dim, FB_TRANSPARENT, scale); return; }
    { char c2[20]; int a=u->sel+1,b=u->pb_count,i2=0;
      if(a>=100)c2[i2++]='0'+(a/100)%10; if(a>=10)c2[i2++]='0'+(a/10)%10; c2[i2++]='0'+a%10; c2[i2++]='/';
      if(b>=100)c2[i2++]='0'+(b/100)%10; if(b>=10)c2[i2++]='0'+(b/10)%10; c2[i2++]='0'+b%10; c2[i2]=0;
      fb_text((int)g_fbw - g.x0 - fb_textw(c2,scale), 6*scale, c2, fg, FB_TRANSPARENT, scale); }
    int page=u->sel/PB_PAGE, start=page*PB_PAGE, i;
    for(i=0;i<PB_PAGE && start+i<u->pb_count;i++) ui_draw_pbcell(u, &g, i);
    (void)bord;(void)cell;(void)selbg;(void)thumbbg;   /* now used inside ui_draw_pbcell */
}

/* the menu panel, centered, over the video */
static void ui_draw_menu(const ui_state *u){
    if(u->menu==MENU_PLAYBACK){ ui_draw_pbgrid(u); return; }   /* full-screen grid, not a panel */
    ui_geom g; ui_menu_geom(u, &g);
    int scale=g.scale, rowh=g.rowh, titleh=g.titleh;
    int pw=g.pw, ph=g.ph, px=g.px, py=g.py;
    int n=0; const menu_item *it = ui_items(u->menu, &n);

    unsigned panel = fb_rgb(12,18,32);           /* solid dark panel */
    unsigned bar   = fb_rgb(0,90,140);           /* title bar */
    unsigned bord  = fb_rgb(0,200,200);
    unsigned fg    = fb_rgb(220,220,220);
    unsigned dim    = fb_rgb(140,150,160);
    unsigned selbg = fb_rgb(0,120,120);
    unsigned selfg = fb_rgb(255,255,255);
    unsigned editbg= fb_rgb(160,110,0);

    fb_rect(px, py, pw, ph, panel);
    fb_rect(px, py, pw, titleh, bar);
    fb_border(px, py, pw, ph, scale, bord);
    fb_text(px + 6*scale, py + 2*scale, ui_menu_title(u->menu), selfg, FB_TRANSPARENT, scale);

    int ry = py + titleh + 2*scale;
    int r;
    for(r=0; r<n; r++){
        int selrow = (r==u->sel);
        unsigned rowfg = selrow?selfg:fg;
        if(selrow){
            unsigned hb = (u->editing && it[r].kind==KIND_VAL) ? editbg : selbg;
            fb_rect(px+scale, ry-scale, pw-2*scale, rowh, hb);
        }
        fb_text(px + 8*scale, ry, it[r].label, rowfg, FB_TRANSPARENT, scale);
        if(it[r].kind==KIND_VAL){
            char vt[16];
            if(ui_is_numeric(it[r].target)) ui_num_text(u, it[r].target, vt);  /* "- NNN +" */
            else ui_val_text(u, it[r].target, vt);
            int vw = fb_textw(vt, scale);
            unsigned vc = (it[r].target>=VAL_REC0 && it[r].target<=VAL_REC3
                           && u->rec[ (it[r].target-VAL_REC0)&3 ]==1) ? fb_rgb(255,60,60) : rowfg;
            fb_text(px + pw - vw - 8*scale, ry, vt, vc, FB_TRANSPARENT, scale);
        } else if(it[r].kind==KIND_SUB){
            fb_text(px + pw - fb_textw(">",scale) - 8*scale, ry, ">", dim, FB_TRANSPARENT, scale);
        }
        ry += rowh;
    }
}

/* ---- playback control bar: a media-player style overlay during full-screen playback ---- */
#define PBB_STOP  0
#define PBB_RW    1   /* seek back (scrub) */
#define PBB_BACK  2   /* step back one frame */
#define PBB_PP    3   /* play / pause / (replay at end) */
#define PBB_STEP  4   /* step forward one frame */
#define PBB_FF    5   /* seek forward (scrub) */
#define PBB_SPEED 6
#define PBC_N     7
static int g_pbb_bx[PBC_N], g_pbb_bw[PBC_N], g_pbb_by=0, g_pbb_bh=0;   /* button hit-rects */
static int g_pbb_prx=0, g_pbb_pry=0, g_pbb_prw=0, g_pbb_prh=0;         /* progress-bar hit-rect (scrub) */
static void ui_mss(long ms, char *o){        /* ms -> "M:SS" (grows to "MMM:SS") */
    long s=ms/1000; if(s<0)s=0; int m=(int)(s/60), sec=(int)(s%60), i=0;
    if(m>=100) o[i++]='0'+(char)((m/100)%10);
    if(m>=10)  o[i++]='0'+(char)((m/10)%10);
    o[i++]='0'+(char)(m%10); o[i++]=':'; o[i++]='0'+(char)(sec/10); o[i++]='0'+(char)(sec%10); o[i]=0;
}
/* paused/ended, speed 4/2/1, elapsed & total ms, title, highlighted button (sel), scrub-active flag */
static void ui_draw_pbbar(int paused, int ended, int speed, long cur_ms, long tot_ms, const char *title, int sel, int scrubbing){
    int scale=(g_fbw>=1000)?2:1;
    int pad=8*scale, gap=7*scale, ch=8*scale, bh=ch+pad*2;
    char sp[8]; { const char*s=(speed>=4)?"1x":(speed==2)?"1/2x":"1/4x"; int i=0; while(*s)sp[i++]=*s++; sp[i]=0; }
    const char *lbl[PBC_N];
    lbl[PBB_STOP]="RETURN"; lbl[PBB_RW]="<<"; lbl[PBB_BACK]="< STEP";
    lbl[PBB_PP]= ended?"REPLAY":(paused?"PLAY":"PAUSE");
    lbl[PBB_STEP]="STEP >"; lbl[PBB_FF]=">>"; lbl[PBB_SPEED]=sp;
    int w[PBC_N], btot=0, i;
    for(i=0;i<PBC_N;i++){ w[i]=fb_textw(lbl[i],scale)+pad*2; btot += w[i]+(i?gap:0); }
    int panelw=(int)g_fbw*2/3; if(panelw<btot+pad*4) panelw=btot+pad*4;
    if(panelw>(int)g_fbw-8*scale) panelw=(int)g_fbw-8*scale;
    int lineh=ch+2*scale, prg_h=8*scale;
    int panelh = pad + lineh + 6*scale + prg_h + 12*scale + bh + pad;
    int px=((int)g_fbw-panelw)/2;
    int py=(int)g_fbh - panelh - (int)g_fbh/12;          /* ~8% bottom margin (clear of overscan) */
    unsigned plate=fb_rgb(10,14,24), bord=fb_rgb(0,180,190);
    unsigned btn=fb_rgb(26,44,66), btnpp=fb_rgb(0,120,90), selbg=fb_rgb(0,170,180), fg=fb_rgb(236,239,243);
    unsigned dim=fb_rgb(150,160,175), prgbg=fb_rgb(45,50,62), prgfg=fb_rgb(0,215,220), knob=fb_rgb(255,255,255);
    fb_rect(px,py,panelw,panelh,plate); fb_border(px,py,panelw,panelh,scale,bord);
    int iy=py+pad;
    if(title&&title[0]) fb_text(px+pad, iy, title, fg, FB_TRANSPARENT, scale);
    { char a[12], b[12], tm[28]; int j=0,k=0; ui_mss(cur_ms,a); ui_mss(tot_ms,b);
      while(a[k]) tm[j++]=a[k++]; tm[j++]=' '; tm[j++]='/'; tm[j++]=' '; k=0; while(b[k]) tm[j++]=b[k++]; tm[j]=0;
      fb_text(px+panelw-pad-fb_textw(tm,scale), iy, tm, ended?prgfg:dim, FB_TRANSPARENT, scale); }
    iy += lineh + 6*scale;
    /* progress bar (draggable to scrub) with a knob at the current position */
    int prx=px+pad, prw=panelw-pad*2;
    g_pbb_prx=prx; g_pbb_pry=iy-4*scale; g_pbb_prw=prw; g_pbb_prh=prg_h+8*scale;   /* fat hit area */
    fb_rect(prx, iy, prw, prg_h, prgbg);
    int fw=0; if(tot_ms>0){ long long f=(long long)prw*cur_ms/tot_ms; if(f>prw)f=prw; if(f<0)f=0; fw=(int)f; }
    if(fw>0) fb_rect(prx, iy, fw, prg_h, prgfg);
    { int kx=prx+fw-2*scale, kw=4*scale; if(kx<prx)kx=prx; if(kx>prx+prw-kw)kx=prx+prw-kw;
      fb_rect(kx, iy-3*scale, kw, prg_h+6*scale, scrubbing?knob:fg); }   /* scrub knob */
    iy += prg_h + 12*scale;
    int bx=px+(panelw-btot)/2; g_pbb_by=iy; g_pbb_bh=bh; /* buttons, centered, sel highlighted */
    for(i=0;i<PBC_N;i++){
        g_pbb_bx[i]=bx; g_pbb_bw[i]=w[i];
        unsigned bg=(i==sel)?selbg:((i==PBB_PP)?btnpp:btn);
        fb_rect(bx, iy, w[i], bh, bg);
        if(i==sel) fb_border(bx, iy, w[i], bh, scale, fg);
        fb_text(bx+pad, iy+pad, lbl[i], fg, FB_TRANSPARENT, scale);
        bx += w[i]+gap;
    }
}
static int ui_pbbar_hit(int mx,int my){                  /* -> button index 0..PBC_N-1, or -1 */
    int i; if(my<g_pbb_by || my>=g_pbb_by+g_pbb_bh) return -1;
    for(i=0;i<PBC_N;i++) if(mx>=g_pbb_bx[i] && mx<g_pbb_bx[i]+g_pbb_bw[i]) return i;
    return -1;
}
/* if (mx,my) is on the progress bar, return the scrub position in permille (0..1000), else -1 */
static int ui_pbbar_seek(int mx,int my){
    if(g_pbb_prw<=0) return -1;
    if(my<g_pbb_pry || my>=g_pbb_pry+g_pbb_prh) return -1;
    if(mx<g_pbb_prx) mx=g_pbb_prx; if(mx>=g_pbb_prx+g_pbb_prw) mx=g_pbb_prx+g_pbb_prw-1;
    return (int)((long)(mx-g_pbb_prx)*1000/g_pbb_prw);
}

/* full OSD render: clear to transparent (ARGB1555 top bit 0 = video shows through),
 * then draw the HUD, then the menu if open. All drawn pixels are opaque (fb_rgb). */
static void ui_render(const ui_state *u){
    fb_fill(FB_CLEAR);                /* transparent everywhere */
    ui_draw_hud(u);
    if(u->open) ui_draw_menu(u);
}
/* redraw only the menu panel region (cheap hover update — no full-screen clear) */
static void ui_render_menu_only(const ui_state *u){
    if(!u->open) return;
    ui_geom g; ui_menu_geom(u, &g);
    fb_rect(g.px-2, g.py-2, g.pw+4, g.ph+4, FB_CLEAR);   /* clear panel area (+margin) */
    ui_draw_menu(u);
}
/* redraw only the HUD strip (cheap clock/REC tick) */
static void ui_render_hud_only(const ui_state *u){
    fb_rect(0, 0, 460, 46, FB_CLEAR);
    ui_draw_hud(u);
}

/* ---- navigation ----------------------------------------------------------- */
static void ui_push(ui_state *u, int menu){
    if(u->sp < 8){ u->stack[u->sp]=u->menu; u->selstk[u->sp]=u->sel; u->sp++; }
    u->menu = menu; u->sel = 0; u->editing = 0;
}
static int ui_pop(ui_state *u){        /* returns 1 if popped, 0 if already at root */
    if(u->sp > 0){ u->sp--; u->menu=u->stack[u->sp]; u->sel=u->selstk[u->sp]; u->editing=0; return 1; }
    return 0;
}
static int ui_rowcount(const ui_state *u){
    if(u->menu==MENU_PLAYBACK) return u->pb_count>0?u->pb_count:1;
    int n=0; ui_items(u->menu,&n); return n;
}
/* keep pb_top so the selected recording stays within the visible window */
static void ui_pb_follow(ui_state *u){
    if(u->menu!=MENU_PLAYBACK) return;
    if(u->sel < u->pb_top) u->pb_top = u->sel;
    else if(u->sel >= u->pb_top + PB_VISROWS) u->pb_top = u->sel - PB_VISROWS + 1;
    int maxtop = (u->pb_count>PB_VISROWS)?(u->pb_count-PB_VISROWS):0;
    if(u->pb_top > maxtop) u->pb_top = maxtop;
    if(u->pb_top < 0) u->pb_top = 0;
}
/* scroll the playback list by d rows (wheel); moves sel with the view. Returns 1 if changed. */
static int ui_pb_scroll(ui_state *u, int d){       /* wheel: move the grid selection by a row */
    if(!u->open || u->menu!=MENU_PLAYBACK || u->pb_count<=0) return 0;
    int old=u->sel;
    u->sel -= d*PB_COLS;                            /* wheel up (d>0) = toward newest (index 0) */
    if(u->sel<0) u->sel=0; if(u->sel>=u->pb_count) u->sel=u->pb_count-1;
    return u->sel!=old;
}

/* toggle a KIND_VAL row that is a discrete toggle; sets act_* + returns UI_ACT_* */
static int ui_toggle_val(ui_state *u, int val){
    switch(val){
        case VAL_STD:
            u->standard = (u->standard==1)?2:(u->standard==2)?0:1;  /* NTSC->PAL->AUTO->NTSC */
            u->act_a = u->standard; return UI_ACT_STD;
        case VAL_DISPLAY:
            u->display = u->display?0:1; u->act_a = u->display; return UI_ACT_DISPLAY;
        case VAL_SOUND:
            u->sound = u->sound?0:1; u->act_a = u->sound; return UI_ACT_SOUND;
        case VAL_REC0: case VAL_REC1: case VAL_REC2: case VAL_REC3: {
            int c = val-VAL_REC0; int on = u->rec[c]?0:1;
            u->act_a = c; u->act_b = on; return UI_ACT_REC;
        }
        case VAL_RECALL: {
            int any=0,c; for(c=0;c<4;c++) if(u->rec[c]) any=1;
            u->act_a = -1; u->act_b = any?0:1; return UI_ACT_REC;
        }
        case VAL_CHANNEL:
            u->chan = (u->chan+1) & 3;        /* cycle CH1..4 */
            u->act_a = u->chan; return UI_ACT_CHANNEL;
        default: return UI_ACT_NONE;
    }
}
/* process one key: '-' '+' 'M' 'X'. Returns a UI_ACT_* code for dvr.c. */
static int ui_key(ui_state *u, char key){
    if(!u->open){
        if(key=='M'){ u->open=1; u->menu=MENU_MAIN; u->sel=0; u->sp=0; u->editing=0; }
        return UI_ACT_NONE;
    }
    int nrows = ui_rowcount(u);
    if(nrows<=0) nrows=1;

    /* editing a numeric value: +/- adjust, M/X leave edit mode */
    if(u->editing){
        int n=0; const menu_item *it = ui_items(u->menu,&n);
        int val = (u->sel<n) ? it[u->sel].target : -1;
        if(key=='+' || key=='-'){
            int d = (key=='+')?8:-8;
            u->act_a = val; u->act_b = d;
            return UI_ACT_PICTURE;                 /* dvr.c clamps + writes reg + updates u->pic */
        }
        if(key=='M' || key=='X'){ u->editing=0; }
        return UI_ACT_NONE;
    }

    if(key=='-'){ u->sel = (u->sel==0)? nrows-1 : u->sel-1; ui_pb_follow(u); return UI_ACT_NONE; }
    if(key=='+'){ u->sel = (u->sel+1)%nrows;               ui_pb_follow(u); return UI_ACT_NONE; }
    if(key=='X'){ if(!ui_pop(u)){ u->open=0; } return UI_ACT_NONE; }
    if(key=='M'){
        if(u->menu==MENU_PLAYBACK){
            if(u->pb_count>0){ u->act_a=u->sel; return UI_ACT_PB_PLAY; }
            return UI_ACT_NONE;
        }
        int n=0; const menu_item *it = ui_items(u->menu,&n);
        if(u->sel>=n) return UI_ACT_NONE;
        int kind=it[u->sel].kind, tgt=it[u->sel].target;
        if(kind==KIND_SUB){
            ui_push(u, tgt);
            if(tgt==MENU_PLAYBACK){ u->act_a=0; return UI_ACT_OPEN_PB; }
            return UI_ACT_NONE;
        }
        if(kind==KIND_ACT){
            if(tgt==ACT_BACK){ if(!ui_pop(u)) u->open=0; return UI_ACT_NONE; }
            if(tgt==ACT_CLOSE){ u->open=0; return UI_ACT_NONE; }
            if(tgt==ACT_REBOOT){ return UI_ACT_REBOOT; }
            if(tgt==ACT_PICRESET){ return UI_ACT_PICRESET; }
            return UI_ACT_NONE;
        }
        if(kind==KIND_VAL){
            if(ui_is_numeric(tgt)){ u->editing=1; return UI_ACT_NONE; }
            return ui_toggle_val(u, tgt);
        }
    }
    return UI_ACT_NONE;
}

/* ---- mouse ---------------------------------------------------------------- */
/* which menu row is under (mx,my)? -1 if outside the panel. For a numeric row,
 * *zone is set to -1 (left "-" half) or +1 (right "+" half) of the value block. */
static int ui_hit_row(const ui_state *u, int mx, int my, int *zone){
    if(zone) *zone=0;
    if(!u->open) return -1;
    if(u->menu==MENU_PLAYBACK) return ui_pbgrid_hit(u, mx, my);   /* full-screen grid cell */
    ui_geom g; ui_menu_geom(u, &g);
    if(mx < g.px || mx >= g.px+g.pw) return -1;
    if(my < g.ry0) return -1;
    int row = (my - g.ry0)/g.rowh;
    if(row < 0 || row >= g.nrows) return -1;
    if(zone && u->menu!=MENU_PLAYBACK){
        int n=0; const menu_item *it = ui_items(u->menu, &n);
        if(row<n && it[row].kind==KIND_VAL && ui_is_numeric(it[row].target)){
            int vw = fb_textw("- 000 +", g.scale);
            int mid = g.px + g.pw - 8*g.scale - vw/2;   /* split the value block */
            *zone = (mx >= mid) ? +1 : -1;
        }
    }
    return row;
}
/* hover: highlight the row under the cursor. Returns 1 if the selection moved.
 * Disabled for the full-screen grid (avoids a full-screen redraw per mouse move — the grid
 * uses click-to-play + button-nav highlight instead). */
static int ui_hover(ui_state *u, int mx, int my){
    if(!u->open || u->menu==MENU_PLAYBACK) return 0;
    int row = ui_hit_row(u, mx, my, 0);
    if(row>=0 && row!=u->sel){ u->sel=row; return 1; }
    return 0;
}
/* click: button 1=left (activate), 2=right (back). Returns a UI_ACT_* for dvr.c. */
static int ui_mouse(ui_state *u, int mx, int my, int button){
    if(button==2){ if(!ui_pop(u)) u->open=0; return UI_ACT_NONE; }
    if(button!=1) return UI_ACT_NONE;
    if(!u->open){
        /* click the HUD REC button -> start/stop recording the shown channel; else open menu */
        if(mx>=g_hud_rec_x && mx<g_hud_rec_x+g_hud_rec_w && my>=g_hud_rec_y && my<g_hud_rec_y+g_hud_rec_h){
            u->act_a = u->chan;
            u->act_b = (u->chan>=0 && u->chan<4 && u->rec[u->chan]) ? 0 : 1;
            return UI_ACT_REC;
        }
        u->open=1; u->menu=MENU_MAIN; u->sel=0; u->sp=0; u->editing=0; return UI_ACT_NONE;
    }
    int zone=0, row = ui_hit_row(u, mx, my, &zone);
    if(row<0) return UI_ACT_NONE;
    u->sel = row;
    if(u->menu==MENU_PLAYBACK){ if(u->pb_count>0){ u->act_a=row; return UI_ACT_PB_PLAY; } return UI_ACT_NONE; }
    int n=0; const menu_item *it = ui_items(u->menu, &n);
    if(row>=n) return UI_ACT_NONE;
    int kind=it[row].kind, tgt=it[row].target;
    if(kind==KIND_SUB){
        ui_push(u, tgt);
        if(tgt==MENU_PLAYBACK){ u->act_a=0; return UI_ACT_OPEN_PB; }
        return UI_ACT_NONE;
    }
    if(kind==KIND_ACT){
        if(tgt==ACT_BACK){ if(!ui_pop(u)) u->open=0; return UI_ACT_NONE; }
        if(tgt==ACT_CLOSE){ u->open=0; return UI_ACT_NONE; }
        if(tgt==ACT_REBOOT) return UI_ACT_REBOOT;
        if(tgt==ACT_PICRESET) return UI_ACT_PICRESET;
        return UI_ACT_NONE;
    }
    if(kind==KIND_VAL){
        if(ui_is_numeric(tgt)){ u->act_a=tgt; u->act_b=(zone>=0)?8:-8; return UI_ACT_PICTURE; }
        return ui_toggle_val(u, tgt);
    }
    return UI_ACT_NONE;
}

#endif
