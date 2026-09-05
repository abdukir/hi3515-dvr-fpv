/* ioctl_trace.c — LD_PRELOAD lib to capture app.out's (Hi3520_MPP_V3.0.6.2) exact MPP
 * setup ioctl sequence, so we can replay it in our own recorder (no matching SDK).
 * Uses ONLY raw open/write for logging (no stdio/malloc) to avoid crashing app.out's
 * early init. Build: arm-hisi-linux-gcc -shared -fPIC -O2 -o ioctl_trace.so ioctl_trace.c -ldl
 * Run:  cd /root && LD_PRELOAD=/root/rec/a1/ioctl_trace.so ./app.out
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdarg.h>
#include <fcntl.h>
#include <string.h>

static int   (*r_open)(const char*, int, ...);
static int   (*r_ioctl)(int, unsigned long, void*);
static void* (*r_mmap)(void*, unsigned long, int, int, int, long);
static long  (*r_write)(int, const void*, unsigned long);
static int   (*r_getpid)(void);
static int   logfd = -1;
static char  fdp[4096][48];

__attribute__((constructor)) static void init_hooks(void){
    r_open   = dlsym(RTLD_NEXT, "open");
    r_ioctl  = dlsym(RTLD_NEXT, "ioctl");
    r_mmap   = dlsym(RTLD_NEXT, "mmap");
    r_write  = dlsym(RTLD_NEXT, "write");
    r_getpid = dlsym(RTLD_NEXT, "getpid");
}
static void ensure(void){
    if(logfd >= 0 || !r_open) return;
    /* per-PID log so exec'd children (webs etc.) don't O_TRUNC over app.out's setup */
    int pid = r_getpid ? r_getpid() : 0;
    char fn[64], num[12]; int i=11;
    num[11]=0; if(!pid) num[--i]='0'; else { int u=pid; while(u){num[--i]="0123456789"[u%10];u/=10;} }
    strcpy(fn, "/root/rec/a1/tr_"); strcat(fn, &num[i]); strcat(fn, ".log");
    logfd = r_open(fn, O_WRONLY|O_CREAT|O_TRUNC, 0644);
}
static void ws(const char *s){ if(logfd>=0 && r_write) r_write(logfd, s, strlen(s)); }
static void wx(unsigned v){ char b[9]; int i; for(i=7;i>=0;i--){ b[i]="0123456789abcdef"[v&0xf]; v>>=4; } b[8]=0; ws(b); }
static void wd(int v){ char b[12]; int i=11,n=0; unsigned u = v<0?-v:v; b[11]=0; if(!u){ws("0");return;} while(u){b[--i]="0123456789"[u%10];u/=10;n++;} if(v<0)b[--i]='-'; ws(&b[i]); }
static void wb(const unsigned char *p, unsigned n){ char t[4]; unsigned i; if(!p||n>512) return; for(i=0;i<n;i++){ t[0]=' '; t[1]="0123456789abcdef"[p[i]>>4]; t[2]="0123456789abcdef"[p[i]&0xf]; t[3]=0; ws(t);} }

int open(const char *path, int flags, ...){
    va_list ap; va_start(ap,flags); int mode = va_arg(ap,int); va_end(ap);
    if(!r_open) init_hooks();
    int fd = r_open(path, flags, mode);
    if(fd>=0 && fd<4096){ strncpy(fdp[fd],path,47); fdp[fd][47]=0; }
    ensure();
    if(logfd>=0 && strncmp(path,"/dev/",5)==0){ ws("OPEN "); ws(path); ws(" = "); wd(fd); ws("\n"); }
    return fd;
}
int ioctl(int fd, unsigned long req, ...){
    va_list ap; va_start(ap,req); void *arg = va_arg(ap,void*); va_end(ap);
    if(!r_ioctl) init_hooks();
    ensure();
    unsigned sz = (req>>16)&0x3fff;
    const char *p = (fd>=0 && fd<4096 && fdp[fd][0]) ? fdp[fd] : "?";
    /* arg is a pointer only if it looks like one (>= 0x10000); small values are
     * passed-by-value integers (e.g. tl_R9508 0xc00456d3 gets 0x64) — do NOT deref. */
    int isptr = ((unsigned long)arg >= 0x10000);
    if(logfd>=0){ ws("IOCTL fd="); wd(fd); ws("["); ws(p); ws("] cmd=0x"); wx(req); ws(" sz="); wd(sz);
                  if(isptr){ ws(" IN:"); wb(arg,sz); } else { ws(" val=0x"); wx((unsigned)(long)arg); } }
    int r = r_ioctl(fd, req, arg);
    if(logfd>=0){ ws(" => "); wd(r); if(isptr){ ws(" OUT:"); wb(arg,sz); } ws("\n"); }
    return r;
}
void *mmap(void *addr, unsigned long len, int prot, int flags, int fd, long off){
    if(!r_mmap) init_hooks();
    void *r = r_mmap(addr,len,prot,flags,fd,off);
    ensure();
    const char *p = (fd>=0 && fd<4096 && fdp[fd][0]) ? fdp[fd] : "?";
    if(logfd>=0 && fd>=0){ ws("MMAP fd="); wd(fd); ws("["); ws(p); ws("] len=0x"); wx(len); ws(" prot="); wd(prot); ws(" off=0x"); wx(off); ws(" => 0x"); wx((unsigned)(long)r); ws("\n"); }
    return r;
}
