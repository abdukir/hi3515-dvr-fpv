#!/bin/bash
# Build our DVR (Hi3515/Hi3520, OABI) from device/dvr/, inside WSL.
# The device kernel is OABI-only -> -nostdlib + our own OABI runtime (oabi.h) and
# the shared crt0.S entry (device/crt0.S). A modern EABI arm cross-gcc is fine (it
# only emits ARMv5 instructions); the OABI syscall convention lives in oabi.h.
#
# BUILD FROM ext4 (~/dvrbuild), NOT /mnt/c — the old-gcc path breaks on DrvFs.
# Usage (from WSL):  bash build.sh [dvr.c] [out]
set -e
GCC="${GCC:-$HOME/arm-uc/bin/arm-linux-gcc}"     # bootlin armv5 uclibc gcc 14
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="${1:-$HERE/dvr.c}"
OUT="${2:-$HERE/dvr}"
CRT0="$HERE/../crt0.S"
# libgcc provides the ARM EABI runtime helpers (__aeabi_uidivmod / __aeabi_idivmod /
# __aeabi_uidiv etc.) for integer division; it's freestanding-safe (no libc). Plain
# `-lgcc` can't resolve libgcc's OWN circular deps (e.g. __divsi3.o -> __aeabi_uidiv
# in __udivsi3.o), so we pass the archive by absolute path inside --start-group.
LIBGCC="$("$GCC" -march=armv5te -mfloat-abi=soft -print-libgcc-file-name)"
"$GCC" -march=armv5te -mfloat-abi=soft -static -nostdlib -O2 \
    -ffreestanding -fno-stack-protector -fno-builtin -I"$HERE" \
    -o "$OUT" "$CRT0" "$SRC" \
    -Wl,--start-group "$LIBGCC" -Wl,--end-group
"$(dirname "$GCC")/arm-linux-strip" "$OUT" 2>/dev/null || true
echo "built $OUT"
file "$OUT" 2>/dev/null || true
"$(dirname "$GCC")/arm-linux-readelf" -h "$OUT" 2>/dev/null | grep -E "Machine|Flags" || true
