#!/usr/bin/env bash
# Builds a libretro core (RetroArch's emulator cores) as a static library for the PS5, its names prefixed:
#   tools/core_lib.sh NAME SOURCE PREFIX [make arguments...]      -> ~/ps5/cores/NAME/libNAME.a
#   KEEP_GOING=1: a compile test (every error at once, nothing kept)
# The same recipe as pcsx_lib.sh / ppsspp_lib.sh: the PS5 SDK's clang, tools/compat/ps5_compat.h first into every file
# (FreeBSD libc vs Linux code), mmap / mprotect through PlayShell's ps_mmap / ps_mprotect (runnable JIT memory comes
# through the kernel), Mesa's OpenGL headers for GL code that compiles but never runs.
set -e
NAME="$1"; SRC="$2"; PREFIX="$3"; shift 3
SDK="${PS5_PAYLOAD_SDK:-$HOME/ps5/wallpaper/sdk-upstream}"
HERE="$(cd "$(dirname "$0")" && pwd)"
WORK="$HOME/ps5/cores/$NAME"
mkdir -p "$WORK/compat/GL" "$WORK/compat/KHR"
rsync -a --delete --exclude .git "$SRC/" "$WORK/src/"
cp "$HERE/compat/ps5_compat.h" "$HERE/compat/malloc.h" "$WORK/compat/"
cp /usr/include/GL/gl.h /usr/include/GL/glext.h "$WORK/compat/GL/" && cp /usr/include/KHR/khrplatform.h "$WORK/compat/KHR/"
FLAGS="-march=znver2 -fPIC -fPIE -ffunction-sections -fdata-sections -w -I$WORK/compat -include $WORK/compat/ps5_compat.h"
FLAGS="$FLAGS -Dmmap=ps_mmap -Dmprotect=ps_mprotect"
[ -x "$HERE/patch_$NAME.sh" ] && bash "$HERE/patch_$NAME.sh" "$WORK/src"   # PlayShell's changes to this core's copy
cd "$WORK/src/${CORE_SUBDIR:-.}"
make -f "${CORE_MAKEFILE:-Makefile}" -j"$(nproc)" ${KEEP_GOING:+-k} platform=unix STATIC_LINKING=1 \
	CC="$SDK/bin/prospero-clang $FLAGS" CXX="$SDK/bin/prospero-clang++ $FLAGS" AR="$SDK/bin/llvm-ar" "$@" \
	> "$WORK/build.log" 2>&1 || true
echo "$NAME: errors $(grep -c 'error:' "$WORK/build.log")"
grep -E "error:" "$WORK/build.log" | sed -E 's/^[^ ]+ error: //' | sort | uniq -c | sort -rn | head -15
grep -E "error:" "$WORK/build.log" | awk -F: '{print $1}' | sort -u | head -15
[ -n "$KEEP_GOING" ] && exit 0
OUT="$(ls -t "$WORK"/src/${CORE_SUBDIR:-.}/*_libretro*.a "$WORK"/src/${CORE_SUBDIR:-.}/*_libretro*.so 2>/dev/null | head -1)"
[ -n "$OUT" ] || { echo "$NAME: no library built"; exit 1; }
cp "$OUT" "$WORK/lib$NAME.a"
# LRCOMMON="dir file.c ...": libretro-common files the core's static build leaves to RetroArch - built into it here
if [ -n "$LRCOMMON" ]; then
	set -- $LRCOMMON
	LRDIR="$WORK/src/$1"; shift
	mkdir -p "$WORK/lrcommon"
	for f in "$@"; do
		"$SDK/bin/prospero-clang" -O2 $FLAGS -I"$LRDIR/include" -c "$LRDIR/$f" -o "$WORK/lrcommon/$(echo "$f" | tr / _).o"
	done
	"$SDK/bin/llvm-ar" rs "$WORK/lib$NAME.a" "$WORK"/lrcommon/*.o
	echo "$NAME: $# libretro-common files added"
fi
bash "$HERE/prefix_lib.sh" "$WORK/lib$NAME.a" "$PREFIX"
# OpenGL code that is built but never runs (CPU rendering here): the gl* it calls, for tools/glstub.sh (one shared set
# of empty stand-ins for every core)
"$SDK/bin/llvm-nm" -u "$WORK/lib$NAME.a" 2>/dev/null | awk '$1 == "U" && $2 ~ /^gl[A-Z]/ {print $2}' | sort -u > "$WORK/gl.list"
echo "$NAME: $(wc -l < "$WORK/gl.list") OpenGL names (stand-ins: tools/glstub.sh)"
ls -la "$WORK/lib$NAME.a"
