#!/bin/bash
# Build a DVR (Hi3515, OABI) program from the device/ scaffold, inside WSL.
# Usage (from WSL):  bash build.sh <main.c> [out]
# The device kernel is OABI-only, so we use -nostdlib + our own OABI runtime
# (crt0.S + oabi.h). Any modern EABI arm cross-gcc works (it only emits ARMv5
# instructions); the OABI syscall convention lives in oabi.h.
set -e
GCC="${GCC:-$HOME/arm-uc/bin/arm-linux-gcc}"     # bootlin armv5 uclibc gcc 14
SRC="${1:-hello.c}"
OUT="${2:-${SRC%.c}}"
HERE="$(cd "$(dirname "$0")" && pwd)"
"$GCC" -march=armv5te -mfloat-abi=soft -static -nostdlib -O2 \
    -ffreestanding -fno-stack-protector -I"$HERE" \
    -o "$OUT" "$HERE/crt0.S" "$SRC"
"$(dirname "$GCC")/arm-linux-strip" "$OUT" 2>/dev/null || true
echo "built $OUT"
file "$OUT" 2>/dev/null || true
