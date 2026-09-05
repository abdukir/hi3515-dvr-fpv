/* keyboard.h — USB keyboard input for the on-screen UI, so the menu + playback
 * control bar can be driven with arrow keys exactly like the future MCU buttons
 * (-, +, M, X). Lets us test the button-based navigation before the buttons exist.
 *
 * IMPORTANT: this box's kernel has NO evdev (no /dev/input/eventN). The USB keyboard
 * uses the legacy 'kbd' handler, so keystrokes land on the foreground VT console.
 * We open /dev/tty0, switch the keyboard to K_MEDIUMRAW, put the tty in raw
 * non-blocking mode, and read 1-byte keycodes:  press = keycode,  release =
 * keycode|0x80  (for keycode < 128 — arrows/enter/esc are all < 128). The original
 * console keyboard + tty modes are restored on close. Requires oabi.h.
 *
 * Key mapping (matches the MCU buttons):
 *   Up / Left     -> '-'  (previous / move selection back)
 *   Down / Right  -> '+'  (next / move selection forward)
 *   Enter / Space -> 'M'  (select / activate; opens the menu from live)
 *   Esc / Backspace -> 'X' (back / exit)
 *   R             -> KEV_REC: start/stop recording. Deliberately NOT a menu key —
 *                    it fires from the live view, from inside a menu, and during
 *                    playback, because in the field you want one keypress and no
 *                    navigation. The buzzer's start/stop melodies are the
 *                    confirmation, so it works without looking at the screen.
 */
#ifndef DVR_KEYBOARD_H
#define DVR_KEYBOARD_H

#include "oabi.h"

/* console / termios ioctls */
#define KDGKBMODE   0x4B44
#define KDSKBMODE   0x4B45
#define K_MEDIUMRAW 2
#define K_XLATE     1          /* the normal console mode we must hand back */
#define KB_TCGETS   0x5401
#define KB_TCSETS   0x5402

/* Linux input keycodes (all < 128, so 1 byte each in mediumraw) */
#define KC_ESC        1
#define KC_BACKSPACE 14
#define KC_ENTER     28
#define KC_SPACE     57
#define KC_KPENTER   96
#define KC_UP       103
#define KC_LEFT     105
#define KC_RIGHT    106
#define KC_DOWN     108
#define KC_R         19   /* record toggle — see KEV_REC */

/* mapped edge events (press only) */
#define KEV_NONE 0
#define KEV_PREV 1   /* -> '-' */
#define KEV_NEXT 2   /* -> '+' */
#define KEV_SEL  3   /* -> 'M' */
#define KEV_BACK 4   /* -> 'X' */
#define KEV_REC  5   /* R: start/stop recording, from anywhere — not a menu key */

static int  g_kfd = -1;
static long g_kbd_oldmode = 1;                 /* K_XLATE; restored on close */
static unsigned char g_kbd_oldtio[64];
static int  g_kbd_have_tio = 0;
/* debug (KBDBG command): so we can confirm keys are being read without a screen */
static unsigned g_kbd_reads=0, g_kbd_keys=0; static int g_kbd_last=-1;

/* Rolling history of the last few letters typed, so callers can spot a typed WORD
 * without this layer knowing what any word means. Only the letters we actually care
 * about are decoded — this is not a keymap and is not trying to become one. */
#define KBD_SEQ 16
static char g_kbd_seq[KBD_SEQ+1];
static char kbd_letter(unsigned char c){
    switch(c){
        case 18: return 'e';  case 34: return 'g';
    }
    return 0;
}
static void kbd_seq_push(char ch){
    int i; for(i=0;i<KBD_SEQ-1;i++) g_kbd_seq[i]=g_kbd_seq[i+1];
    g_kbd_seq[KBD_SEQ-1]=ch; g_kbd_seq[KBD_SEQ]=0;
}
static void kbd_seq_clear(void){ int i; for(i=0;i<=KBD_SEQ;i++) g_kbd_seq[i]=0; }
static int kbd_seq_ends(const char *w){
    int n=0,i; while(w[n]) n++;
    if(n<=0 || n>KBD_SEQ) return 0;
    for(i=0;i<n;i++) if(g_kbd_seq[KBD_SEQ-n+i]!=w[i]) return 0;
    return 1;
}

/* open the foreground VT, save its state, and switch the keyboard to mediumraw + raw tty */
static void kbd_open(void){
    int fd=(int)sys_open("/dev/tty0", O_RDWR|O_NONBLOCK, 0);
    if(fd<0) fd=(int)sys_open("/dev/tty1", O_RDWR|O_NONBLOCK, 0);
    if(fd<0){ g_kfd=-1; return; }
    { unsigned char tio[64]; int i;
      if(sys_ioctl(fd,(unsigned long)KB_TCGETS,tio)==0){
          for(i=0;i<64;i++) g_kbd_oldtio[i]=tio[i]; g_kbd_have_tio=1;
          tio[0]=tio[1]=tio[2]=tio[3]=0;       /* c_iflag = 0 */
          tio[4]=tio[5]=tio[6]=tio[7]=0;       /* c_oflag = 0 */
          tio[12]=tio[13]=tio[14]=tio[15]=0;   /* c_lflag = 0 (no ICANON/ECHO/ISIG) */
          tio[22]=0; tio[23]=0;                /* c_cc[VTIME]=0, c_cc[VMIN]=0 */
          sys_ioctl(fd,(unsigned long)KB_TCSETS,tio);
      } }
    sys_ioctl(fd,(unsigned long)KDGKBMODE,&g_kbd_oldmode);          /* save mode (by pointer) */
    /* If a previous run was SIGKILLed (or respawned) kbd_close never ran, so the console
     * is STILL in mediumraw and we would faithfully "restore" it to raw on our next clean
     * exit — leaving the VT keyboard useless until a reboot. Mediumraw is never a
     * legitimate console default, so read it as "somebody else's leftover" and restore
     * K_XLATE instead. */
    if(g_kbd_oldmode == K_MEDIUMRAW) g_kbd_oldmode = K_XLATE;
    sys_ioctl(fd,(unsigned long)KDSKBMODE,(void*)K_MEDIUMRAW);      /* set mode (by value) */
    g_kfd=fd;
}
/* restore the console so the keyboard works normally again after we exit */
static void kbd_close(void){
    if(g_kfd<0) return;
    sys_ioctl(g_kfd,(unsigned long)KDSKBMODE,(void*)g_kbd_oldmode);
    if(g_kbd_have_tio) sys_ioctl(g_kfd,(unsigned long)KB_TCSETS,g_kbd_oldtio);
    sys_close(g_kfd); g_kfd=-1;
}
/* non-blocking: return the first mapped KEV_* press seen this poll (0 if none) */
static int kbd_poll(void){
    if(g_kfd<0) return KEV_NONE;
    unsigned char b[32];
    long n=sys_read(g_kfd,b,sizeof(b));
    if(n<=0) return KEV_NONE;
    g_kbd_reads++;
    int i, ev=KEV_NONE;
    for(i=0;i<(int)n;i++){
        unsigned char c=b[i];
        if(c & 0x80) continue;                 /* key release */
        if(c==0){ if(i+2<(int)n) i+=2; continue; }   /* extended (>=128) keycode: skip 2 bytes */
        g_kbd_keys++; g_kbd_last=(int)c;
        { char lt=kbd_letter(c); if(lt) kbd_seq_push(lt); }
        int e=KEV_NONE;
        if(c==KC_UP || c==KC_LEFT)  e=KEV_PREV;
        else if(c==KC_DOWN || c==KC_RIGHT) e=KEV_NEXT;
        else if(c==KC_ENTER || c==KC_KPENTER || c==KC_SPACE) e=KEV_SEL;
        else if(c==KC_ESC || c==KC_BACKSPACE) e=KEV_BACK;
        else if(c==KC_R) e=KEV_REC;
        if(e && !ev) ev=e;                     /* first mapped press wins this poll */
    }
    return ev;
}
#endif
