/* spsfix_tool.c - one-off repair for recordings written with the malformed SPS.
 *
 * The firmware briefly wrote vui_parameters() with NINE trailing flags instead of eight,
 * which pushed rbsp_stop_one_bit to 0. ffmpeg shrugs; the DVR hardware decoder refuses,
 * so those files play black on the device itself (and generate no thumbnails).
 *
 * The corrected SPS is the SAME LENGTH - only the final byte changes - so the file can be
 * repaired in place without touching TS packet framing at all. Not part of the firmware;
 * push it, run it over the affected recordings, delete it.
 */
#include "oabi.h"
void *memset(void *d,int c,size_t n){unsigned char*p=d;while(n--)*p++=(unsigned char)c;return d;}
void *memcpy(void *d,const void *s,size_t n){unsigned char*a=d;const unsigned char*b=s;while(n--)*a++=*b++;return d;}
int raise(int s){(void)s;return 0;}

/* NTSC 704x240, SAR 5:11 */
static const unsigned char BAD_N[15]={0x67,0x42,0xe0,0x15,0xdb,0x02,0xc1,0xf3,0xff,0x00,0x05,0x00,0x0b,0x00,0x40};
/* PAL 704x288, SAR 6:11 */
static const unsigned char BAD_P[15]={0x67,0x42,0xe0,0x15,0xdb,0x02,0xc5,0xf3,0xff,0x00,0x06,0x00,0x0b,0x00,0x40};

static unsigned char buf[262144];

static void pu(unsigned v){ char t[12]; int i=0; if(!v){sys_write(1,"0",1);return;}
    while(v){t[i++]="0123456789"[v%10]; v/=10;} while(i) sys_write(1,&t[--i],1); }

int main(int argc, char **argv){
    int f;
    if(argc<2){ sys_write(2,"usage: spsfix_tool <file.ts>...\n",32); return 1; }
    for(f=1; f<argc; f++){
        int fd=(int)sys_open(argv[f], 2 /*O_RDWR*/, 0);
        if(fd<0){ sys_write(1,"open failed: ",13); sys_write(1,argv[f],(unsigned)k_strlen(argv[f])); sys_write(1,"\n",1); continue; }
        unsigned long long off=0; unsigned fixed=0; long n;
        int carry=0;                        /* bytes kept from the previous block */
        while((n=sys_read(fd, buf+carry, sizeof(buf)-carry))>0){
            long total=n+carry, i;
            for(i=0; i+15<=total; i++){
                int hitN=1, hitP=1, k;
                for(k=0;k<15;k++){ if(buf[i+k]!=BAD_N[k]) { hitN=0; break; } }
                if(!hitN) for(k=0;k<15;k++){ if(buf[i+k]!=BAD_P[k]) { hitP=0; break; } }
                if(hitN||hitP){
                    unsigned long long at = off + (unsigned long long)i + 14;
                    unsigned char fixb = 0x80;
                    sys_lseek(fd,(off_t)at,0);
                    sys_write(fd,&fixb,1);
                    fixed++;
                    sys_lseek(fd,(off_t)(off+total),0);   /* back to where we were reading */
                }
            }
            off += (unsigned long long)(total-14);
            /* keep the last 14 bytes so a pattern spanning the boundary is still found */
            for(i=0;i<14;i++) buf[i]=buf[total-14+i];
            carry=14;
        }
        sys_close(fd);
        sys_write(1,argv[f],(unsigned)k_strlen(argv[f]));
        sys_write(1," -> fixed ",10); pu(fixed); sys_write(1," SPS\n",5);
    }
    return 0;
}
