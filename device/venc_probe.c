/* venc_probe.c — our own program pulls live H.264 stream metadata from the
 * Hi3515 VENC hardware, via the reversed /dev/venc ioctl ABI (magic 'E'=0x45).
 * See docs/FIRMWARE_RE.md. Built with the OABI runtime (device/), no SDK/libc.
 *
 * Flow (from HI_MPI_VENC_GetStream = FUN_0015c5f0):
 *   fd=open("/dev/venc"); ioctl(fd,0x4004451b,&chn);      // bind channel
 *   ioctl(fd,0x40044512,&flag);                            // prepare
 *   stream={pstPack=<our buf>, packCount=CAP, seq=0};
 *   ioctl(fd,0xc00c450c,&stream);                          // GetStream (fills our buf)
 * Each VENC_PACK_S is 44 B (11 words); H.264 bytes live at phyaddr+0x40 (needs mmap to read).
 */
#include "oabi.h"

#define VENC_SEL   0x4004451b
#define VENC_PREP  0x40044512
#define VENC_GET   0xc00c450c
#define VENC_REL   0x400c450d

#define CAP 32
struct venc_pack { unsigned int w[11]; };          /* 44 bytes */
struct venc_stream { struct venc_pack *pack; unsigned int count; unsigned int seq; };

static int myatoi(const char *s){ int v=0; while(*s>='0'&&*s<='9') v=v*10+(*s++-'0'); return v; }

int main(int argc, char **argv) {
    int chn = argc > 1 ? myatoi(argv[1]) : 0;
    static struct venc_pack packs[CAP];
    struct venc_stream st;

    puts_("venc_probe: channel "); putu(chn); puts_("\n");
    int fd = sys_open("/dev/venc", O_RDWR, 0);
    if (fd < 0) { puts_("open /dev/venc FAILED rc="); putu((unsigned)-fd); puts_("\n"); return 1; }
    puts_("opened /dev/venc fd="); putu(fd); puts_("\n");

    unsigned int c = chn;
    long r = sys_ioctl(fd, VENC_SEL, &c);
    puts_("  select(0x4004451b) rc="); puthex((unsigned)r); puts_("\n");

    unsigned int flag = 0;                     /* 0 = wait for frame (we poll; app.out selects first) */
    r = sys_ioctl(fd, VENC_PREP, &flag);
    puts_("  prep(0x40044512) rc="); puthex((unsigned)r); puts_("\n");

    int got = 0;
    for (int tries = 0; tries < 20 && got < 3; tries++) {
        st.pack = packs; st.count = CAP; st.seq = 0;
        r = sys_ioctl(fd, VENC_GET, &st);
        if (r == 0 && st.count > 0) {
            got++;
            puts_("  GetStream ok: packs="); putu(st.count); puts_(" seq="); putu(st.seq); puts_("\n");
            for (unsigned int i = 0; i < st.count && i < 2; i++) {
                puts_("    pack["); putu(i); puts_("] raw words:\n");
                for (int w = 0; w < 11; w++) {
                    puts_("      w["); putu(w); puts_("]="); puthex(packs[i].w[w]);
                    puts_(" ("); putu(packs[i].w[w]); puts_(")\n");
                }
            }
            sys_ioctl(fd, VENC_REL, &st);      /* ReleaseStream (required) */
        } else {
            /* no frame yet; brief sleep */
            struct { long s; long ns; } ts = { 0, 50*1000*1000 };
            sys_ioctl(-1, 0, 0); /* noop to keep it simple */
            for (volatile int k=0;k<200000;k++);
        }
    }
    if (!got) { puts_("  no frames pulled (rc="); puthex((unsigned)r);
                puts_(", count="); putu(st.count); puts_(")\n"); }
    sys_close(fd);
    puts_("done\n");
    return got ? 0 : 2;
}
