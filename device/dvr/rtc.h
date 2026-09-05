/* rtc.h — read the battery-backed PCF8563 RTC for dated recording filenames.
 * Verified: /dev/rtc RTC_RD_TIME returns a real struct rtc_time (9 x int32, 36 B).
 * Requires oabi.h. Div by 10/1000 pulls in libgcc (link with -lgcc, already set).
 */
#ifndef DVR_RTC_H
#define DVR_RTC_H

#include "oabi.h"

#define RTC_RD_TIME  0x80247009   /* _IOR('p', 0x09, struct rtc_time) */
#define RTC_SET_TIME 0x4024700a   /* _IOW('p', 0x0a, struct rtc_time) */

struct rtc_time_ {
    int tm_sec, tm_min, tm_hour, tm_mday, tm_mon /*0-11*/, tm_year /*+1900*/,
        tm_wday, tm_yday, tm_isdst;
};

static int rtc_read(struct rtc_time_ *t){
    int fd = (int)sys_open("/dev/rtc", O_RDONLY, 0);
    if(fd < 0) return -1;
    int r = (int)sys_ioctl(fd, RTC_RD_TIME, t);
    sys_close(fd);
    return r;
}
static int rtc_set(const struct rtc_time_ *t){
    int fd = (int)sys_open("/dev/rtc", O_WRONLY, 0);
    if(fd < 0) return -1;
    int r = (int)sys_ioctl(fd, RTC_SET_TIME, (void*)t);
    sys_close(fd);
    return r;
}
static void two(char *b, int v){ b[0]='0'+((v/10)%10); b[1]='0'+(v%10); }
/* "YYYYMMDD_HHMMSS" into buf[15]+NUL; falls back to epoch on RTC error */
static void rtc_stamp(char *buf){
    struct rtc_time_ t;
    if(rtc_read(&t) != 0 || t.tm_year < 100 || t.tm_year > 200){
        t.tm_year=70; t.tm_mon=0; t.tm_mday=1; t.tm_hour=0; t.tm_min=0; t.tm_sec=0;
    }
    int y = t.tm_year + 1900;
    buf[0]='0'+((y/1000)%10); buf[1]='0'+((y/100)%10); buf[2]='0'+((y/10)%10); buf[3]='0'+(y%10);
    two(&buf[4], t.tm_mon+1);
    two(&buf[6], t.tm_mday);
    buf[8]='_';
    two(&buf[9],  t.tm_hour);
    two(&buf[11], t.tm_min);
    two(&buf[13], t.tm_sec);
    buf[15]=0;
}

#endif
