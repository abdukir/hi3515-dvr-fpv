#!/usr/bin/env bash
# publish.sh — refresh the PUBLIC mirror from this (private) working repo.
#
#   this repo   abdukir/hi3515-dvr-fpv-archive   PRIVATE   full history, incl. dump/ + capture/
#   the mirror  abdukir/hi3515-dvr-fpv           PUBLIC    our own work only, single commit
#
# The mirror deliberately has NO history: it is rebuilt from the current tracked
# tree each time and force-pushed as one commit. That keeps vendor firmware out
# of the public object store permanently, rather than relying on a .gitignore to
# have been right on every past commit.
#
# Excluded from the mirror (manufacturers' property — see README "Legal and scope"):
#   dump/          U-Boot, kernel, rootfs, app.out, Ghidra input, ActiveX DLL,
#                  and the mtd0 config blob, which holds this unit's MAC and its
#                  stored admin password in the clear
#   capture/       raw 8670 protocol logs (device MAC + login exchanges)
#   device/sdk/    HiSilicon MPP SDK
#   hi3515v100.pdf SoC datasheet
#
# Usage:  bash tools/publish.sh [--dry-run]
set -euo pipefail

MIRROR_REPO="abdukir/hi3515-dvr-fpv"
EXCLUDE='^(dump|capture|re)/|^hi3515v100\.pdf$|^device/sdk/'
DRY=0
[ "${1:-}" = "--dry-run" ] && DRY=1

cd "$(dirname "$0")/.."
ROOT=$(pwd)

if [ -n "$(git status --porcelain)" ]; then
    echo "! working tree is dirty — commit first, the mirror is built from tracked files" >&2
    exit 1
fi

STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT

git ls-files -z | grep -zvE "$EXCLUDE" > "$STAGE/files.z"
COUNT=$(tr '\0' '\n' < "$STAGE/files.z" | grep -c . || true)

TREE="$STAGE/tree"
mkdir -p "$TREE"
while IFS= read -r -d '' f; do
    mkdir -p "$TREE/$(dirname "$f")"
    cp "$ROOT/$f" "$TREE/$f"
done < "$STAGE/files.z"

# Paranoia: never let vendor material through, even if EXCLUDE is edited badly.
# Matches vendor *binaries* by basename — our own scripts are allowed to be named
# after the partitions they write (flash/device/flash_mtd2.sh).
if (cd "$TREE" && find . -type f | grep -Ei '/(app\.out|NetDvr2[^/]*|[^/]*\.ko|[^/]*\.pdf|mtd[0-9][^/]*\.bin|rootfs[^/]*\.(bin|ext2|tar\.gz|tgz))$'); then
    echo "! refusing to publish: vendor artefact reached the staging tree" >&2
    exit 1
fi

SRC_HEAD=$(git rev-parse --short HEAD)
cd "$TREE"
git init -q -b main
git add -A
git commit -q -m "hi3515-dvr-fpv: custom firmware for a NetDVR RR104P

Public mirror of the working tree at $SRC_HEAD, rebuilt as a single
commit. Our own code and documentation only — no vendor firmware, SDK or
datasheet is redistributed; see README.md 'Legal and scope'.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"

echo "staged $COUNT files from $SRC_HEAD -> $MIRROR_REPO"
if [ "$DRY" = "1" ]; then
    echo "(dry run — not pushed)"
    git -C "$TREE" ls-files | sed 's/^/  /'
    exit 0
fi

git remote add origin "https://github.com/$MIRROR_REPO.git"
git push -q --force origin main
echo "published: https://github.com/$MIRROR_REPO"
