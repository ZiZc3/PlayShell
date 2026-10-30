#!/usr/bin/env bash
# Builds Mesen 2's emulation core (GPL-3.0, github.com/SourMesen/Mesen2) as a static library for the PS5:
#   MESEN=/path/to/Mesen2 tools/mesen_lib.sh      -> $WORK/libmesen.a
# Core + Utilities + Lua + SevenZip, compiled with the PS5 payload SDK's clang (libc++).
set -e
MESEN="${MESEN:-${REF:-$HOME/ps5/ref}/mesen2}" # (REF: where the emulator sources are)
SDK="${PS5_PAYLOAD_SDK:-$HOME/ps5/wallpaper/sdk-upstream}"
WORK="${WORK:-$HOME/ps5/mesen-build}"
CXX="$SDK/bin/prospero-clang++"
CC="$SDK/bin/prospero-clang"
AR="$SDK/bin/llvm-ar"
FLAGS="-O2 -fPIC -fPIE -march=znver2 -ffunction-sections -fdata-sections -w -DNDEBUG"
mkdir -p "$WORK/src" "$WORK/obj"
rsync -a --delete --include='*/' --include='*.cpp' --include='*.c' --include='*.h' --include='*.hpp' --include='*.inc' --include='*.ipp' \
	--exclude='*' "$MESEN/Core" "$MESEN/Utilities" "$MESEN/Lua" "$MESEN/SevenZip" "$WORK/src/"
cd "$WORK/src"
export CXX CC FLAGS WORK
find Core Utilities -name '*.cpp' > "$WORK/cpp.list"
find Lua SevenZip Utilities Core -name '*.c' > "$WORK/c.list"
build_one() {
	f="$1"; o="$WORK/obj/$(echo "$f" | tr / _).o"
	[ "$o" -nt "$f" ] && exit 0
	case "$f" in
		*.cpp) $CXX -std=c++17 $FLAGS -I. -ICore -IUtilities -c "$f" -o "$o" 2> "$o.err" || { echo "FAIL $f"; head -3 "$o.err"; } ;;
		*.c)   $CC $FLAGS -I. -ILua -c "$f" -o "$o" 2> "$o.err" || { echo "FAIL $f"; head -3 "$o.err"; } ;;
	esac
}
export -f build_one
cat "$WORK/cpp.list" "$WORK/c.list" | xargs -P"$(nproc)" -I{} bash -c 'build_one "{}"'
rm -f "$WORK/libmesen.a"
"$AR" rcs "$WORK/libmesen.a" "$WORK"/obj/*.o
ls -la "$WORK/libmesen.a"
echo "objects: $(ls "$WORK"/obj/*.o | wc -l) of $(cat "$WORK/cpp.list" "$WORK/c.list" | wc -l)"
