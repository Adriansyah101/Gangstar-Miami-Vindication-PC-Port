#!/bin/sh
# The host on Linux: core/, shims/ and one game.  The build itself is CMakeLists.txt; this keeps the old arguments and
# calls CMake (build folder build/cmake/linux-<game>-<kind>).
# It needs 32-bit gcc, CMake (Ninja if present) and the 32-bit SDL3 and GL development files (Debian, Ubuntu:
# dpkg --add-architecture i386, then gcc-multilib cmake ninja-build libsdl3-dev:i386 libgl-dev:i386 libfreetype-dev:i386).
# FreeType's headers are for gangstar's pad pictures, which open libfreetype.so.6 and libfontconfig.so.1 when they run.
# The intro movie and the custom radio's m4a, aac and wma songs run the system's ffmpeg and ffprobe (any version, 64-bit
# is fine); without them the game skips the intro and leaves those songs out (mp3, ogg, flac and wav need neither).
#   host/linux/build.sh [game]      game: the folder under host/games, default template
#   NOICON=1 host/linux/build.sh    leave out the game's app.ico even if it is present
#   BUILD=dev|release host/linux/build.sh   no -g, symbols stripped (release: GMV_RELEASE), into <game>/dev|release
# The program goes to build/linux/<game>; gmv_patch.bin goes next to it, the same file as on Windows.  The link
# address keeps the program above the game's address range, which it maps when it starts.
# games/<game>/app.ico is made from the game's own icon art, so it is not in the public repo (see app.rc); when it
# is present it is built into the program and becomes the window's icon.
set -e
cd "$(dirname "$0")/.."
game=${1:-template}
kind=${BUILD:-plain}
for tool in gcc cmake; do
    command -v $tool >/dev/null || { echo "no $tool (Debian, Ubuntu: apt install gcc-multilib cmake ninja-build)"; exit 1; }
done
if command -v ninja >/dev/null; then gen=Ninja; bin=../build/cmake/linux-$game-$kind
else gen="Unix Makefiles"; bin=../build/cmake/linux-$game-$kind-make; fi
icon=OFF
[ -n "$NOICON" ] && icon=ON
cmake -S . -B "$bin" -G "$gen" -DGMV_GAME="$game" -DGMV_BUILD="$kind" -DGMV_NOICON=$icon -DGMV_OUT_DIR= -DGMV_STAGE1=OFF
cmake --build "$bin"
