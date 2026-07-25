/* oabi.h — OABI (old-ABI) runtime for our own DVR on the RR104P / Hi3515.
 *
 * Extended copy of device/oabi.h. The device kernel (2.6.24, ARM926/ARMv5TE) is
 * OABI-only: syscalls MUST use `swi #(0x900000+nr)` with args in r0-r6 (the modern
 * EABI `svc 0` + r7 convention SIGILLs here). We compile -nostdlib and provide our
 * own syscall stubs — no libc. Syscall number = the SWI *immediate* (needs "i" asm
 * constraint). Build: device/dvr/build.sh.
 *
 * Adds over device/oabi.h: nanosleep, gettimeofday, mkdir (for the recorder's
 * segment rotation / timestamps). Networking syscalls are added in M2.
 */
#ifndef DVR_OABI_H
#define DVR_OABI_H

typedef unsigned int   size_t;
typedef int            ssize_t;
typedef long           off_t;

#define SYS_BASE   0x900000
#define __NR_exit    1
#define __NR_read    3
#define __NR_write   4
#define __NR_open    5
#define __NR_close   6
#define __NR_unlink  10
#define __NR_mknod   14
#define __NR_mkdir   39
#define __NR_lseek   19
#define __NR_gettimeofday 78
#define __NR_ioctl   54
#define __NR_munmap  91
#define __NR_statfs  99
#define __NR_nanosleep 162
#define __NR_mmap2   192
#define __NR_getdents64 217

#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR   2
#define O_CREAT 0100
#define O_TRUNC 01000
#define O_APPEND 02000
#define O_NONBLOCK 04000
#define O_SYNC  010000   /* 0x1000 — uncached mapping for hardware/DMA memory */
#define O_DIRECTORY 040000  /* 0x4000 */

#define PROT_READ 1
#define PROT_WRITE 2
#define MAP_SHARED 1

/* --- OABI syscall stubs (swi immediate = SYS_BASE+nr) --- */
static inline long sys_write(int fd, const void *buf, size_t n) {
    register long r0 asm("r0") = fd;
    register long r1 asm("r1") = (long)buf;
    register long r2 asm("r2") = n;
    asm volatile("swi %[s]" : "+r"(r0)
                 : [s]"i"(SYS_BASE+__NR_write), "r"(r1), "r"(r2) : "memory");
    return r0;
}
static inline long sys_read(int fd, void *buf, size_t n) {
    register long r0 asm("r0") = fd;
    register long r1 asm("r1") = (long)buf;
    register long r2 asm("r2") = n;
    asm volatile("swi %[s]" : "+r"(r0)
                 : [s]"i"(SYS_BASE+__NR_read), "r"(r1), "r"(r2) : "memory");
    return r0;
}
static inline long sys_open(const char *path, int flags, int mode) {
    register long r0 asm("r0") = (long)path;
    register long r1 asm("r1") = flags;
    register long r2 asm("r2") = mode;
    asm volatile("swi %[s]" : "+r"(r0)
                 : [s]"i"(SYS_BASE+__NR_open), "r"(r1), "r"(r2) : "memory");
    return r0;
}
static inline long sys_close(int fd) {
    register long r0 asm("r0") = fd;
    asm volatile("swi %[s]" : "+r"(r0) : [s]"i"(SYS_BASE+__NR_close) : "memory");
    return r0;
}
static inline long sys_ioctl(int fd, unsigned long req, void *arg) {
    register long r0 asm("r0") = fd;
    register long r1 asm("r1") = req;
    register long r2 asm("r2") = (long)arg;
    asm volatile("swi %[s]" : "+r"(r0)
                 : [s]"i"(SYS_BASE+__NR_ioctl), "r"(r1), "r"(r2) : "memory");
    return r0;
}
static inline long sys_lseek(int fd, off_t off, int whence) {
    register long r0 asm("r0") = fd;
    register long r1 asm("r1") = off;
    register long r2 asm("r2") = whence;
    asm volatile("swi %[s]" : "+r"(r0)
                 : [s]"i"(SYS_BASE+__NR_lseek), "r"(r1), "r"(r2) : "memory");
    return r0;
}
static inline long sys_mkdir(const char *path, int mode) {
    register long r0 asm("r0") = (long)path;
    register long r1 asm("r1") = mode;
    asm volatile("swi %[s]" : "+r"(r0)
                 : [s]"i"(SYS_BASE+__NR_mkdir), "r"(r1) : "memory");
    return r0;
}
static inline long sys_getdents64(int fd, void *buf, unsigned n) {
    register long r0 asm("r0") = fd;
    register long r1 asm("r1") = (long)buf;
    register long r2 asm("r2") = n;
    asm volatile("swi %[s]" : "+r"(r0)
                 : [s]"i"(SYS_BASE+__NR_getdents64), "r"(r1), "r"(r2) : "memory");
    return r0;
}
static inline long sys_mknod(const char *path, int mode, int dev) {
    register long r0 asm("r0") = (long)path;
    register long r1 asm("r1") = mode;
    register long r2 asm("r2") = dev;
    asm volatile("swi %[s]" : "+r"(r0)
                 : [s]"i"(SYS_BASE+__NR_mknod), "r"(r1), "r"(r2) : "memory");
    return r0;
}
static inline long sys_unlink(const char *path) {
    register long r0 asm("r0") = (long)path;
    asm volatile("swi %[s]" : "+r"(r0) : [s]"i"(SYS_BASE+__NR_unlink) : "memory");
    return r0;
}
/* mmap2: offset is in 4096-byte pages. 6 args -> r0-r5. Returns addr or -errno. */
static inline void *sys_mmap2(void *addr, size_t len, int prot, int flags, int fd, unsigned long pgoff) {
    register long r0 asm("r0") = (long)addr;
    register long r1 asm("r1") = len;
    register long r2 asm("r2") = prot;
    register long r3 asm("r3") = flags;
    register long r4 asm("r4") = fd;
    register long r5 asm("r5") = pgoff;
    asm volatile("swi %[s]" : "+r"(r0)
                 : [s]"i"(SYS_BASE+__NR_mmap2), "r"(r1), "r"(r2), "r"(r3), "r"(r4), "r"(r5) : "memory");
    return (void*)r0;
}
static inline long sys_munmap(void *addr, size_t len) {
    register long r0 asm("r0") = (long)addr;
    register long r1 asm("r1") = len;
    asm volatile("swi %[s]" : "+r"(r0) : [s]"i"(SYS_BASE+__NR_munmap), "r"(r1) : "memory");
    return r0;
}
/* struct statfs (32-bit ARM): f_type,f_bsize,f_blocks,f_bfree,f_bavail,... (7 u32 then fsid) */
struct statfs_ { unsigned f_type, f_bsize, f_blocks, f_bfree, f_bavail, f_files, f_ffree;
                 unsigned f_fsid[2], f_namelen, f_frsize, f_flags, f_spare[4]; };
static inline long sys_statfs(const char *path, struct statfs_ *buf) {
    register long r0 asm("r0") = (long)path;
    register long r1 asm("r1") = (long)buf;
    asm volatile("swi %[s]" : "+r"(r0) : [s]"i"(SYS_BASE+__NR_statfs), "r"(r1) : "memory");
    return r0;
}
/* free space in MB for the filesystem holding `path` (0 on error) */
static inline unsigned free_mb(const char *path) {
    struct statfs_ s;
    if(sys_statfs(path, &s) != 0 || s.f_bsize == 0) return 0;
    unsigned long long b = (unsigned long long)s.f_bavail * s.f_bsize;
    return (unsigned)(b >> 20);
}
/* timespec {sec, nsec}. nanosleep(&req, NULL). */
struct k_timespec { long tv_sec; long tv_nsec; };
static inline long sys_nanosleep(const struct k_timespec *req, struct k_timespec *rem) {
    register long r0 asm("r0") = (long)req;
    register long r1 asm("r1") = (long)rem;
    asm volatile("swi %[s]" : "+r"(r0)
                 : [s]"i"(SYS_BASE+__NR_nanosleep), "r"(r1) : "memory");
    return r0;
}
/* timeval {sec, usec}. gettimeofday(&tv, NULL). */
struct k_timeval { long tv_sec; long tv_usec; };
static inline long sys_gettimeofday(struct k_timeval *tv, void *tz) {
    register long r0 asm("r0") = (long)tv;
    register long r1 asm("r1") = (long)tz;
    asm volatile("swi %[s]" : "+r"(r0)
                 : [s]"i"(SYS_BASE+__NR_gettimeofday), "r"(r1) : "memory");
    return r0;
}
static inline void sys_exit(int code) {
    register long r0 asm("r0") = code;
    asm volatile("swi %[s]" : : "r"(r0), [s]"i"(SYS_BASE+__NR_exit) : "memory");
    __builtin_unreachable();
}
static inline void msleep(unsigned ms) {
    struct k_timespec ts; ts.tv_sec = ms/1000; ts.tv_nsec = (long)(ms%1000)*1000000L;
    sys_nanosleep(&ts, 0);
}
static inline long now_sec(void) {
    struct k_timeval tv; tv.tv_sec = 0; sys_gettimeofday(&tv, 0); return tv.tv_sec;
}
/* free-running millisecond clock (wraps ~every 49 days — fine for rate-limiting) */
static inline unsigned now_ms(void) {
    struct k_timeval tv; tv.tv_sec=0; tv.tv_usec=0; sys_gettimeofday(&tv, 0);
    return (unsigned)tv.tv_sec*1000u + (unsigned)(tv.tv_usec/1000);
}

/* --- tiny freestanding helpers --- */
static inline size_t k_strlen(const char *s){ const char*p=s; while(*p)p++; return p-s; }
static inline void puts_(const char *s){ sys_write(1, s, k_strlen(s)); }
static inline void putu(unsigned int v){
    char b[12]; int i=12; if(!v){ sys_write(1,"0",1); return; }
    while(v){ b[--i]="0123456789"[v%10]; v/=10; }
    sys_write(1,&b[i],12-i);
}
static inline void puthex(unsigned int v){
    char b[8]; int i=8; if(!v){ sys_write(1,"0x0",3); return; }
    while(v){ b[--i]="0123456789abcdef"[v&0xf]; v>>=4; }
    sys_write(1,"0x",2); sys_write(1,&b[i],8-i);
}
static inline int k_atoi(const char *s){ int v=0,n=1; if(!s)return 0; if(*s=='-'){n=-1;s++;} while(*s>='0'&&*s<='9'){v=v*10+(*s-'0');s++;} return v*n; }

#endif
