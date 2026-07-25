/* net.h — minimal OABI TCP sockets for the Hi3515 DVR (kernel 2.6.24, ARM).
 *
 * On this kernel ARM has NO direct socket syscalls — everything goes through the
 * socketcall(102) multiplexer: socketcall(call, unsigned long args[]). We only need
 * server-side primitives (socket/setsockopt/bind/listen/accept); once a client fd is
 * accepted we use the normal read()/write() syscalls (they work on sockets).
 * Non-blocking is done with ioctl(FIONBIO) so the server folds into the record loop.
 *
 * Requires oabi.h (for sys_ioctl and SYS_BASE).
 */
#ifndef DVR_NET_H
#define DVR_NET_H

#include "oabi.h"

#define __NR_socketcall 102

/* socketcall "call" selectors (linux/net.h) */
#define SC_SOCKET     1
#define SC_BIND       2
#define SC_LISTEN     4
#define SC_ACCEPT     5
#define SC_SEND       9
#define SC_SETSOCKOPT 14

#define MSG_NOSIGNAL  0x4000   /* don't raise SIGPIPE on a broken connection */

#define AF_INET       2
#define SOCK_STREAM   1
#define SOL_SOCKET    1
#define SO_REUSEADDR  2
#define FIONBIO       0x5421

static inline long sys_socketcall(int call, unsigned long *args){
    register long r0 asm("r0") = call;
    register long r1 asm("r1") = (long)args;
    asm volatile("swi %[s]" : "+r"(r0)
                 : [s]"i"(SYS_BASE+__NR_socketcall), "r"(r1) : "memory");
    return r0;
}

/* struct sockaddr_in, 16 bytes, all fields network/byte order as noted */
struct sock_in { unsigned short family; unsigned short port_be; unsigned int addr_be; unsigned char zero[8]; };

static inline int net_listen(int port){
    unsigned long a[6];
    int fd, one = 1;
    a[0]=AF_INET; a[1]=SOCK_STREAM; a[2]=0;
    fd = (int)sys_socketcall(SC_SOCKET, a);
    if(fd < 0) return fd;
    /* SO_REUSEADDR so quick restarts don't hit TIME_WAIT */
    a[0]=fd; a[1]=SOL_SOCKET; a[2]=SO_REUSEADDR; a[3]=(unsigned long)&one; a[4]=sizeof(one);
    sys_socketcall(SC_SETSOCKOPT, a);
    { struct sock_in sin;
      sin.family = AF_INET;
      sin.port_be = (unsigned short)((port<<8)|(port>>8));   /* htons */
      sin.addr_be = 0;                                       /* INADDR_ANY */
      sin.zero[0]=sin.zero[1]=sin.zero[2]=sin.zero[3]=0;
      sin.zero[4]=sin.zero[5]=sin.zero[6]=sin.zero[7]=0;
      a[0]=fd; a[1]=(unsigned long)&sin; a[2]=sizeof(sin);
      if(sys_socketcall(SC_BIND, a) < 0){ sys_close(fd); return -1; }
    }
    a[0]=fd; a[1]=8;   /* backlog */
    if(sys_socketcall(SC_LISTEN, a) < 0){ sys_close(fd); return -1; }
    /* non-blocking accept */
    { int nb = 1; sys_ioctl(fd, FIONBIO, &nb); }
    return fd;
}

/* returns client fd, or -EAGAIN (-11) if none pending, or other -errno */
static inline int net_accept(int lfd){
    unsigned long a[6];
    a[0]=lfd; a[1]=0; a[2]=0;   /* don't care about peer addr */
    return (int)sys_socketcall(SC_ACCEPT, a);
}

static inline void net_nonblock(int fd){ int nb = 1; sys_ioctl(fd, FIONBIO, &nb); }

/* send() with MSG_NOSIGNAL — a write to a disconnected client returns -EPIPE
 * instead of raising SIGPIPE (which would kill our no-libc process). */
static inline long net_send(int fd, const void *buf, unsigned len){
    unsigned long a[6]; a[0]=fd; a[1]=(unsigned long)buf; a[2]=len; a[3]=MSG_NOSIGNAL;
    return sys_socketcall(SC_SEND, a);
}

#endif
