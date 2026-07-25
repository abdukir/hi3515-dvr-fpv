/* venc_rec.c — minimal "our own DVR recorder": pull live H.264 from VENC and
 * write raw bytes to a file, using only our OABI runtime + the reversed ioctl ABI.
 * Reads frame payload from physical MMZ memory via mmap(/dev/mem).
 *   raw VENC_PACK_S: w[0]=phys addr (of 0x40 pack header), w[4]=total len.
 *   H.264 Annex-B data = [phys+0x40 .. phys+w[4]).
 * (No ReleaseStream yet -> short capture only; TODO: pair release ioctl.)
 * Usage on device:  venc_rec <chn> <max_frames> <outfile>
 */
#include "oabi.h"

#define VENC_SEL  0x4004451b
#define VENC_PREP 0x40044512
#define VENC_GET  0xc00c450c
#define VENC_REL  0x400c450d
#define PGSZ 4096u
#define CAP 32
struct venc_pack { unsigned int w[11]; };
struct venc_stream { struct venc_pack *pack; unsigned int count; unsigned int seq; };

static int myatoi(const char *s){ int v=0; while(*s>='0'&&*s<='9') v=v*10+(*s++-'0'); return v; }
static void spin(volatile int n){ while(n--) ; }

int main(int argc, char **argv) {
    int chn = argc>1 ? myatoi(argv[1]) : 0;
    int maxf = argc>2 ? myatoi(argv[2]) : 100;
    const char *out = argc>3 ? argv[3] : "/root/rec/a1/our.h264";
    static struct venc_pack packs[CAP];
    struct venc_stream st;

    int fdv = sys_open("/dev/venc", O_RDWR, 0);
    /* Frame phys (w[2]) is real DDR (base 0xc0000000). Map /dev/mem UNCACHED (O_SYNC) —
     * the MMZ is DMA-written by the encoder; a cached map reads stale filler (0x80).
     * (This is exactly how HI_MPI_VENC_CreateChn maps the stream buffer: open O_SYNC + mmap phys.) */
    int fdm = sys_open("/dev/mem", O_RDWR|O_SYNC, 0);
    int fdo = sys_open(out, O_WRONLY|O_CREAT|O_TRUNC, 0644);
    if (fdv<0||fdm<0||fdo<0){ puts_("open failed venc="); putu(fdv); puts_(" mem="); putu(fdm);
        puts_(" out="); putu(fdo); puts_("\n"); return 1; }

    unsigned int c=chn; sys_ioctl(fdv, VENC_SEL, &c);
    unsigned int flag=0; sys_ioctl(fdv, VENC_PREP, &flag);

    unsigned int frames=0, bytes=0; int iters=0, diag=1;
    while (frames < (unsigned)maxf && iters < 3000) {
        iters++;
        st.pack=packs; st.count=CAP; st.seq=0;
        long r = sys_ioctl(fdv, VENC_GET, &st);
        if (r!=0 || st.count==0){ spin(30000); continue; }
        for (unsigned int i=0;i<st.count && i<CAP;i++){
            /* phys MMZ addr of frame = w[2] (DDR@0xc0000000; MMZ is above Linux's 43MB) */
            unsigned int phys=packs[i].w[2], total=packs[i].w[4];
            if (total<=0x40) continue;
            unsigned int base=phys & ~(PGSZ-1);
            unsigned int off =phys & (PGSZ-1);
            unsigned int maplen=(off+total+PGSZ-1)&~(PGSZ-1);
            /* map the DDR page(s) UNCACHED; virt = map + (phys & 0xfff). */
            unsigned char *p=(unsigned char*)sys_mmap2(0, maplen, PROT_READ, MAP_SHARED, fdm, base>>12);
            if (diag){ diag=0; puts_("diag: phys="); puthex(phys); puts_(" total="); putu(total);
                puts_(" mmap="); puthex((unsigned)(long)p); puts_("\n"); }
            if ((long)p==-1 || (long)p<0){ continue; }
            unsigned int dlen=total-0x40;
            sys_write(fdo, p+off+0x40, dlen);
            bytes+=dlen; frames++;
            sys_munmap(p, maplen);
        }
        sys_ioctl(fdv, VENC_REL, &st);   /* ReleaseStream (required, else channel stalls) */
        spin(50000);
    }
    sys_close(fdo); sys_close(fdm); sys_close(fdv);
    puts_("recorded frames="); putu(frames); puts_(" bytes="); putu(bytes);
    puts_(" -> "); puts_(out); puts_("\n");
    return frames?0:2;
}
