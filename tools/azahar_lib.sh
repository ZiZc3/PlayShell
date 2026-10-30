#!/usr/bin/env bash
# Builds Azahar's libretro core (Nintendo 3DS, GPL-2.0+, github.com/azahar-emu/azahar, unified source 2126.2) as one
# static library for the PS5:   AZAHAR=/path/to/azahar tools/azahar_lib.sh   -> $WORK/libazahar.a
# Software renderer + the CPU JIT (dynarmic); no OpenGL / Vulkan. CMake with the payload SDK's toolchain (FreeBSD).
#   STEP=configure | build (default: both); KEEP_GOING=1: compile everything, list the errors
set -e
AZAHAR="${AZAHAR:-${REF:-$HOME/ps5/ref}/azahar}" # (REF: where the emulator sources are)
SDK="${PS5_PAYLOAD_SDK:-$HOME/ps5/wallpaper/sdk-upstream}"
WORK="${WORK:-$HOME/ps5/azahar-build}"
HERE="$(cd "$(dirname "$0")" && pwd)"
mkdir -p "$WORK/compat"
cp "$HERE/compat/ps5_compat.h" "$HERE/compat/malloc.h" "$WORK/compat/" 2>/dev/null || cp "$HERE/compat/ps5_compat.h" "$WORK/compat/"

if [ "${STEP:-all}" != build ]; then
	rsync -a --delete --exclude .git "$AZAHAR/" "$WORK/src/"
	# PlayShell's changes to the copy (the source stays as it is):
	#  - the core as a static library (one archive for playshell.elf), not a shared one
	sed -i 's|add_library(citra_libretro SHARED|add_library(citra_libretro STATIC|' "$WORK/src/src/citra_libretro/CMakeLists.txt"
	grep -q 'add_library(citra_libretro STATIC' "$WORK/src/src/citra_libretro/CMakeLists.txt" || { echo "patch: STATIC"; exit 1; }
	FLAGS="-march=znver2 -ffunction-sections -fdata-sections -w -I$WORK/compat -include $WORK/compat/ps5_compat.h"
	FLAGS="$FLAGS -Dmmap=ps_mmap -Dmprotect=ps_mprotect -DPS5_COMPAT_NO_GETAUXVAL"
	rm -rf "$WORK/build"
	cmake -S "$WORK/src" -B "$WORK/build" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$SDK/toolchain/prospero.cmake" \
		-DCMAKE_BUILD_TYPE=Release -DCMAKE_VERBOSE_MAKEFILE=OFF \
		-DCMAKE_C_FLAGS="$FLAGS" -DCMAKE_CXX_FLAGS="$FLAGS" \
		-DENABLE_LIBRETRO=ON -DENABLE_OPENGL=OFF -DENABLE_VULKAN=OFF -DENABLE_SOFTWARE_RENDERER=ON \
		-DENABLE_TESTS=OFF -DENABLE_LTO=OFF -DCITRA_WARNINGS_AS_ERRORS=OFF -DCITRA_USE_PRECOMPILED_HEADERS=OFF \
		-DENABLE_DISCORD_RPC=OFF -DENABLE_MICROPROFILE=OFF -DUSE_DISCORD_PRESENCE=OFF \
		> "$WORK/configure.log" 2>&1 || { grep -E "Error|error" -A4 "$WORK/configure.log" | head -60; exit 1; }
	echo "configured"
fi
[ "${STEP:-all}" = configure ] && exit 0

cmake --build "$WORK/build" --target citra_libretro -j"$(nproc)" -- ${KEEP_GOING:+-k 0} > "$WORK/build.log" 2>&1 || true
echo "errors: $(grep -c 'error:' "$WORK/build.log")"
grep -E "error:" "$WORK/build.log" | sed -E 's/^[^ ]+ error: //' | sort | uniq -c | sort -rn | head -30
grep -E "error:" "$WORK/build.log" | awk -F: '{print $1}' | sort -u | head -30
[ -n "$KEEP_GOING" ] && exit 0
# one archive: the core + every library it was built with (an MRI script: same-named objects kept apart), then every
# global name prefixed n3ds_ (retro_run -> n3ds_retro_run; its own zstd, fmt, ... apart from the other cores')
[ -f "$WORK/build/src/citra_libretro/azahar_libretro.a" ] || { echo "not built"; exit 1; }
{
	echo "create $WORK/libazahar.a"
	find "$WORK/build" -name '*.a' -size +1k | sort | sed 's/^/addlib /'
	echo save; echo end
} > "$WORK/merge.mri"
rm -f "$WORK/libazahar.a"
"$SDK/bin/llvm-ar" -M < "$WORK/merge.mri"
bash "$HERE/prefix_lib.sh" "$WORK/libazahar.a" n3ds_
ls -la "$WORK/libazahar.a"
