#!/bin/sh
# run_app.sh — RR104P boot script, FPV build.
#
# DIFF vs stock (dump/livedump_20260723/boot/run_app.sh): one hand-off hook, inserted
# after the stock module/app unpack and BEFORE mydaemon.out/app.out.  The hook mounts
# SATA a1 and, if /root/rec/a1/boot.sh exists, exec's it and never returns.  Everything
# our project runs then lives on the SATA disk, which is writable over the network — so
# the flash never has to be rewritten again after this one reflash.
#
# If the SATA disk is missing, unmountable, or has no boot.sh, we unmount and fall
# through to the *unmodified* stock path (mydaemon.out + app.out).  net.sh has already
# run at that point, so telnetd is up either way: a broken boot.sh can never lock us out.

cd /root/
./net.sh
./init.sh

rm -rf /lib/modules/2.6.24-rt1-hi3515v100/kernel
rm -rf /root/tl_modules_r9508

mv -f /tmp2/app.tgz .
tar -zxf app.tgz 2>/dev/null
rm -f app.tgz

chmod -R 777 app.out 2>/dev/null

mv -f /tmp2/data.tgz .
tar -zxf data.tgz 2>/dev/null
rm -f data.tgz

hostname 192.168.1.255

#udevstart

# ---------------- FPV hand-off hook ----------------
# app.out is normally the thing that mounts /dev/sdaN on /root/rec/aN, so we have to do
# it ourselves before we can look for boot.sh.  ahci/sd_mod were modprobe'd by init.sh
# but disk probing is asynchronous — wait (bounded) for the node to appear.
for i in 1 2 3 4 5 6 7 8 9 10 11 12; do
    [ -b /dev/sda1 ] && break
    sleep 1
done
mkdir -p /root/rec/a1
if mount -t vfat /dev/sda1 /root/rec/a1 2>/dev/null ; then
    if [ -x /root/rec/a1/boot.sh ] ; then
        mkdir -p /root/rec/a2 /root/rec/a3 /root/rec/a4
        mount -t vfat /dev/sda2 /root/rec/a2 2>/dev/null
        mount -t vfat /dev/sda3 /root/rec/a3 2>/dev/null
        mount -t vfat /dev/sda4 /root/rec/a4 2>/dev/null
        echo "[boot] FPV hand-off -> /root/rec/a1/boot.sh"
        exec /root/rec/a1/boot.sh
    fi
    echo "[boot] no /root/rec/a1/boot.sh — falling back to stock app.out"
    umount /root/rec/a1 2>/dev/null
else
    echo "[boot] SATA a1 not mountable — falling back to stock app.out"
fi
# -------------- end FPV hand-off hook --------------

echo "do you want to run app.out(y or n)?"
read -t 1 -n 1 char

umount /tmp2
mount tmpfs /tmp2 -t tmpfs -o size=9M

#webs &
./mydaemon.out &

if [ "$char" == "n" ];then
echo "don't run app.out"
else
./app.out
fi
