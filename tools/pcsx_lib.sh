#!/usr/bin/env bash
# Builds PCSX-ReARMed (libretro core, GPL-2.0+, github.com/libretro/pcsx_rearmed) as a static library for the PS5:
#   PCSX=/path/to/pcsx_rearmed tools/pcsx_lib.sh      -> $WORK/pcsx_rearmed_libretro.a
# lightrec (the JIT: PS1 code -> x86-64) included; its memory mapping trick off (LIGHTREC_CUSTOM_MAP=0).
set -e
PCSX="${PCSX:-${REF:-$HOME/ps5/ref}/pcsx_rearmed}" # (REF: where the emulator sources are)
SDK="${PS5_PAYLOAD_SDK:-$HOME/ps5/wallpaper/sdk-upstream}"
WORK="${WORK:-$HOME/ps5/pcsx-build}"
mkdir -p "$WORK"
rsync -a --delete --exclude .git "$PCSX/" "$WORK/src/"
cd "$WORK/src"
# (the SDK is FreeBSD's libc: time.h / pthread_np.h first, before files that ask for strict POSIX hide BSD names)
# mmap / mprotect -> PlayShell's ps_mmap / ps_mprotect (playshell_ps1.c): runnable memory on the PS5 comes through the
# kernel (kernel_mprotect), the way the payload loader makes every payload's code runnable - lightrec's JIT needs it
FLAGS="-march=znver2 -fPIC -fPIE -ffunction-sections -fdata-sections -w -include time.h -include pthread.h -include pthread_np.h"
FLAGS="$FLAGS -include sys/mman.h -Dmmap=ps_mmap -Dmprotect=ps_mprotect"
make -f Makefile.libretro -j"$(nproc)" platform=unix STATIC_LINKING=1 \
	CC="$SDK/bin/prospero-clang $FLAGS" \
	CXX="$SDK/bin/prospero-clang++ $FLAGS" \
	AR="$SDK/bin/llvm-ar" DYNAREC=lightrec LIGHTREC_CUSTOM_MAP=0 HAVE_PHYSICAL_CDROM=0 "$@" > "$WORK/build.log" 2>&1 || {
	grep -E "error|Error" "$WORK/build.log" | head -30; exit 1; }
# (with STATIC_LINKING the archive keeps the .so name)
cp "$WORK/src/pcsx_rearmed_libretro.so" "$WORK/libpcsx.a"
# its names prefixed (retro_run -> pcsx_retro_run, its miniz, zstd, ...): several cores in one playshell.elf
bash "$(cd "$(dirname "$0")" && pwd)/prefix_lib.sh" "$WORK/libpcsx.a" pcsx_
ls -la "$WORK/libpcsx.a"
