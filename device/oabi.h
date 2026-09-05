/* oabi.h — minimal OABI (old-ABI) runtime for the RR104P / Hi3515 DVR.
 *
 * The device kernel (2.6.24) is OABI-only: syscalls MUST use `swi #(0x900000+nr)`
 * with args in r0-r6 (the modern EABI `svc 0` + r7 convention SIGILLs here).
 * A modern EABI cross-toolchain emits valid ARMv5 instructions, so we compile
 * with `-nostdlib` and provide our own syscall stubs below — no libc needed.
 *
 * The syscall number goes in the SWI *immediate* (compile-time), so each stub
 * uses an "i" asm constraint. Build: see device/build.sh.
 */
#ifndef OABI_H
#define OABI_H

typedef unsigned int   size_t;
typedef int            ssize_t;
typedef long           off_t;

#define SYS_BASE   0x900000
#define __NR_exit    1
#define __NR_read    3
#define __NR_write   4
#define __NR_open    5
#define __NR_close   6
#define __NR_lseek   19
#define __NR_ioctl   54
#define __NR_munmap  91
#define __NR_uname   122
#define __NR_mmap2   192
#define __NR_nanosleep 162

#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR   2

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
static inline long sys_uname(void *buf) {
    register long r0 asm("r0") = (long)buf;
    asm volatile("swi %[s]" : "+r"(r0) : [s]"i"(SYS_BASE+__NR_uname) : "memory");
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
#define PROT_READ 1
#define PROT_WRITE 2
#define MAP_SHARED 1
#define O_CREAT 0100
#define O_TRUNC 01000
#define O_SYNC  010000   /* 0x1000 — uncached mapping for hardware/DMA memory */

static inline void sys_exit(int code) {
    register long r0 asm("r0") = code;
    asm volatile("swi %[s]" : : "r"(r0), [s]"i"(SYS_BASE+__NR_exit) : "memory");
    __builtin_unreachable();
}

/* --- tiny freestanding helpers --- */
static inline size_t k_strlen(const char *s){ const char*p=s; while(*p)p++; return p-s; }
static inline void puts_(const char *s){ sys_write(1, s, k_strlen(s)); }
/* unsigned int -> decimal, printed */
static inline void putu(unsigned int v){
    char b[12]; int i=12; if(!v){ sys_write(1,"0",1); return; }
    while(v){ b[--i]="0123456789"[v%10]; v/=10; }
    sys_write(1,&b[i],12-i);
}
/* unsigned int -> hex */
static inline void puthex(unsigned int v){
    char b[8]; int i=8; if(!v){ sys_write(1,"0",1); return; }
    while(v){ b[--i]="0123456789abcdef"[v&0xf]; v>>=4; }
    sys_write(1,"0x",2); sys_write(1,&b[i],8-i);
}

struct utsname_ { char s[6][65]; };  /* sysname/nodename/release/version/machine/domain */

#endif
