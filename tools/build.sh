#!/usr/bin/env bash
# Build PlayShell (Linux / WSL), the same way as LiveShell: copies src/ into the OnionHEN v0.9.0-beta2 tree as
# source/playshell, builds the "playshell" target and copies playshell.elf to OUT (default: this repo's parent).
#   ONIONHEN=~/onionHEN-beta2 PS5_PAYLOAD_SDK=~/ps5-payload-sdk tools/build.sh
set -e
HERE="$(cd "$(dirname "$0")/.." && pwd)"
TREE="${ONIONHEN:-$HOME/ps5/wallpaper/onionHEN-beta2}"
SDK="${PS5_PAYLOAD_SDK:-$HOME/ps5/wallpaper/sdk-upstream}"
OUT="${OUT:-$(dirname "$HERE")}"
BUILD="$TREE/build"
DEST="$TREE/source/playshell"

[ -d "$TREE/source" ] || { echo "OnionHEN tree not found: $TREE (set ONIONHEN=...)"; exit 1; }
[ -d "$SDK" ] || { echo "PS5 payload SDK not found: $SDK (set PS5_PAYLOAD_SDK=...)"; exit 1; }

python3 "$HERE/tools/make_bezels.py" > /dev/null
python3 "$HERE/tools/make_icons.py" > /dev/null
mkdir -p "$DEST"
rm -f "$DEST"/*.cpp "$DEST"/*.c "$DEST"/*.h
grep -q '^add_subdirectory(playshell)' "$TREE/source/CMakeLists.txt" || echo 'add_subdirectory(playshell)' >> "$TREE/source/CMakeLists.txt"
cp "$HERE"/src/*.cpp "$HERE"/src/*.c "$HERE"/src/*.h "$HERE/src/CMakeLists.txt" "$DEST/"
rm -rf "$DEST/xbrz" # (xBRZ: PSP only - gone)

[ -d "$HOME/ps5/wallpaper/bin" ] && export PATH="$HOME/ps5/wallpaper/bin:$PATH"
[ -d /usr/lib/llvm-18/bin ] && export PATH="/usr/lib/llvm-18/bin:$PATH" && export LLVM_CONFIG=/usr/lib/llvm-18/bin/llvm-config
export PS5_PAYLOAD_SDK="$SDK"

cmake -S "$TREE/source" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DONIONHEN_KSTUFF_ELF="$TREE/.cache/dependencies/kstuff.elf" -DPS5_PAYLOAD_SDK="$SDK" \
    > /tmp/playshell_configure.log 2>&1 || { tail -30 /tmp/playshell_configure.log; exit 1; }
if ! cmake --build "$BUILD" -j"$(nproc)" --target playshell > /tmp/playshell_build.log 2>&1; then
    grep -E "error:|FAILED|undefined symbol" /tmp/playshell_build.log | sort -u | head -40
    exit 1
fi
sha256sum "$BUILD/bin/playshell.elf"
cp "$BUILD/bin/playshell.elf" "$OUT/playshell.elf"
echo "-> $OUT/playshell.elf"
