/* nettest.c — prove OABI socketcall networking works on the DVR before integrating
 * into the recorder. Listens on TCP 8091; on each client connection, writes a known
 * marker + 4096 incrementing bytes, then closes. Verify from PC:
 *   python: connect 192.168.1.108:8091, recv all, check header "DVRNET01" + pattern.
 * Build: device/dvr/build.sh nettest.c ; run: ./nettest
 */
#include "net.h"

void *memset(void *d, int c, size_t n){ unsigned char *p=d; while(n--) *p++=(unsigned char)c; return d; }

int main(void){
    int lfd = net_listen(8091);
    puts_("[nettest] listen fd="); putu((unsigned)lfd); puts_("\n");
    if(lfd < 0){ puts_("[nettest] listen FAILED\n"); return 1; }
    puts_("[nettest] listening on :8091 (accepts 3 clients then exits)\n");

    int served = 0;
    while(served < 3){
        int c = net_accept(lfd);
        if(c < 0){ msleep(50); continue; }    /* -EAGAIN etc */
        puts_("[nettest] client fd="); putu((unsigned)c); puts_("\n");
        /* header */
        sys_write(c, "DVRNET01", 8);
        /* 4096 incrementing bytes */
        { unsigned char buf[256]; int i, k;
          for(k=0;k<16;k++){ for(i=0;i<256;i++) buf[i]=(unsigned char)((k*256+i)&0xff); sys_write(c, buf, 256); } }
        sys_close(c);
        served++;
    }
    sys_close(lfd);
    puts_("[nettest] done\n");
    return 0;
}
