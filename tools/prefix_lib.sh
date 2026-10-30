#!/usr/bin/env bash
# Gives every global name a static library defines a prefix (definitions and uses inside it alike), so several
# emulator cores - each with its own copies of zstd, libchdr, libretro-common, ... - live in one playshell.elf:
#   tools/prefix_lib.sh libpcsx.a pcsx_      (retro_run -> pcsx_retro_run, ...)
# Weak names (inline C++ shared with the C++ library) are left as they are; names the library only uses (libc,
# PlayShell's ps_mmap, ...) too.
set -e
LIB="$1"; PREFIX="$2"
SDK="${PS5_PAYLOAD_SDK:-$HOME/ps5/wallpaper/sdk-upstream}"
"$SDK/bin/llvm-nm" -g --defined-only --format=posix "$LIB" 2>/dev/null |
	awk -v p="$PREFIX" 'NF >= 2 && $2 ~ /^[A-Z]$/ && $2 != "W" && $2 != "V" && $1 !~ /:$/ { print $1 " " p $1 }' |
	sort -u > "$LIB.map"
"$SDK/bin/llvm-objcopy" --redefine-syms="$LIB.map" "$LIB"
echo "$LIB: $(wc -l < "$LIB.map") names prefixed $PREFIX"
