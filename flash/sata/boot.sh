#!/bin/sh
# /root/rec/a1/boot.sh — SATA-resident launcher for our DVR.
#
# `run_app.sh` in flash exec's this instead of app.out (see flash/rootfs_overlay).
# Everything here lives on the writable SATA disk, so it can be replaced over the
# network (web Files tab / telnet) without ever touching the flash again.
#
# Escape hatches, in order of precedence:
#   /root/rec/a1/noboot   present -> do nothing, just idle (telnet in and work by hand)
#   /root/rec/a1/stock    present -> run the stock app.out instead of our DVR
#   /root/rec/a1/dvr      missing -> same as `stock` (never leave the box with no DVR)
#
# Tunables come from /root/rec/a1/dvr.conf (the same file dvr.c reads) plus dvr.args
# for the argv overrides; nothing is hard-coded here except the safe defaults.

D=/root/rec/a1
LOG=$D/dvr.log

# ---- singleton ------------------------------------------------------------------
# Only ever one launcher. Two boot.sh loops both respawn the DVR when it exits, so you end
# up with several `dvr` instances fighting over the MPP hardware — which shows up as a
# stalled encoder or a frozen display, not as an obvious error. Easy to cause by hand while
# debugging (`nohup boot.sh &` twice), so guard it here rather than remembering.
#
# Use a pidfile checked against /proc, NOT `ps | grep boot.sh`: every command substitution
# and pipeline in this script forks a copy of the shell that still carries boot.sh's
# command line, so a grep-based check sees "another instance" that is really itself and
# the one legitimate launcher exits. That shipped once and left the box with no DVR and no
# network on the next boot — recoverable only over the serial console.
LOCK=$D/boot.pid
if [ -f $LOCK ]; then
    OLD=`cat $LOCK 2>/dev/null`
    if [ -n "$OLD" ] && [ "$OLD" != "$$" ] && [ -d /proc/$OLD ] &&
       grep -q boot.sh /proc/$OLD/cmdline 2>/dev/null ; then
        echo "[boot.sh] another launcher is running (pid $OLD) — exiting" >> $LOG
        exit 0
    fi
fi
echo $$ > $LOCK

# keep the log from growing without bound across reboots (RAM is 43 MB, SATA is 28 GB,
# but an unbounded appender is still a bad idea on a device that runs for days)
if [ -f $LOG ]; then
    SZ=`wc -c < $LOG 2>/dev/null`
    if [ "$SZ" -gt 4000000 ] 2>/dev/null; then
        mv -f $LOG $LOG.1
    fi
fi

echo "[boot.sh] `date` starting" >> $LOG

# ---- serial console -------------------------------------------------------------
# run_app.sh exec'd into us, so /etc/init.d/rcS never returns — which means init never
# gets past ::sysinit and never starts the getty from /etc/inittab. Without this the
# serial port is dead silent, and the serial console is our recovery path. Start it
# ourselves, in a respawn loop so exiting the shell gives you a new one.
if [ ! -f $D/noconsole ] ; then
    ( while true; do
          /sbin/getty -L ttyS000 115200 vt100 -n root -I "auto login as root" >/dev/null 2>&1
          sleep 1
      done ) &
    echo "[boot.sh] serial getty started on ttyS000" >> $LOG
fi

# ---- network --------------------------------------------------------------------
# net.sh brings eth0 up on the flash default (192.168.1.114). The stock app.out used to
# override that from its config blob in mtd0 at 0x0e0000; we don't run app.out, so set
# the address here instead. Put a different one in /root/rec/a1/ip to change it, or
# `dhcp` on a line by itself to use udhcpc.
IP=`cat $D/ip 2>/dev/null`
[ -z "$IP" ] && IP=192.168.1.108
if [ "$IP" = "dhcp" ] ; then
    udhcpc -i eth0 -b -q >/dev/null 2>&1 &
    echo "[boot.sh] eth0 via dhcp" >> $LOG
else
    ifconfig eth0 "$IP" netmask 255.255.255.0
    route add default gw 192.168.1.1 2>/dev/null
    echo "[boot.sh] eth0 $IP" >> $LOG
fi

# manual-override: idle so a human can take over
if [ -f $D/noboot ] ; then
    echo "[boot.sh] noboot present — idling" >> $LOG
    while [ ! -f /root/stop ]; do sleep 5; done
    exit 0
fi

# stock DVR app requested (or our binary is missing) -> restore vendor behaviour
if [ -f $D/stock ] || [ ! -x $D/dvr ] ; then
    echo "[boot.sh] running stock app.out" >> $LOG
    cd /root
    [ -f $D/stock/app.tgz ]  && tar -zxf $D/stock/app.tgz  -C /root
    [ -f $D/stock/data.tgz ] && tar -zxf $D/stock/data.tgz -C /root
    chmod -R 777 /root/app.out 2>/dev/null
    ./mydaemon.out &
    exec ./app.out
fi

# our DVR: argv = <secs> <dispch> <r|d> <vifps> <vosync> <votol_ms>
ARGS="0 0 r 30 9 1"
[ -f $D/dvr.args ] && ARGS=`cat $D/dvr.args`

# dvr.c reads its config from /root/rec/dvr.conf (RAM disk, volatile). Seed it from the
# SATA copy so settings actually survive a reboot.
[ -f $D/dvr.conf ] && cp -f $D/dvr.conf  /root/rec/dvr.conf
[ -f $D/std ]      && cp -f $D/std       /root/rec/std
[ -f $D/dispch ]   && cp -f $D/dispch    /root/rec/dispch
[ -f $D/snd ]      && cp -f $D/snd       /root/rec/snd

# Our own front-panel matrix scan needs the vendor driver out of the way — two scanners
# driving the same rows would fight. Only unload it when panel_own=1 is configured.
# file server for the web UI (Files tab / clip download); harmless if it fails
pidof httpd >/dev/null || httpd -h / -p 8081

while [ ! -f /root/stop ]; do
    echo "[boot.sh] exec dvr $ARGS" >> $LOG
    $D/dvr $ARGS >> $LOG 2>&1
    # persist any runtime setting the UI wrote to the RAM disk back onto SATA
    [ -f /root/rec/std ]    && cp -f /root/rec/std    $D/std
    [ -f /root/rec/dispch ] && cp -f /root/rec/dispch $D/dispch
    [ -f /root/rec/snd ]    && cp -f /root/rec/snd    $D/snd
    sleep 1
done
echo "[boot.sh] /root/stop present — exiting" >> $LOG
