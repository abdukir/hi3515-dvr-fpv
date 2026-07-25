/* hello.c — proves our own OABI C program (with main + args) runs on the DVR. */
#include "oabi.h"

int main(int argc, char **argv) {
    struct utsname_ u;
    puts_("=== our own program, built with a modern cross-gcc, OABI runtime ===\n");
    puts_("argc="); putu(argc); puts_("  argv0="); puts_(argv[0]); puts_("\n");
    if (sys_uname(&u) == 0) {
        puts_("sysname="); puts_(u.s[0]);
        puts_("  release="); puts_(u.s[2]);
        puts_("  machine="); puts_(u.s[4]); puts_("\n");
    }
    /* prove filesystem syscalls work: read the record log size */
    int fd = sys_open("/proc/uptime", O_RDONLY, 0);
    if (fd >= 0) {
        char buf[64]; long n = sys_read(fd, buf, sizeof(buf)-1);
        if (n > 0) { puts_("uptime="); sys_write(1, buf, n); }
        sys_close(fd);
    }
    return 0;
}
