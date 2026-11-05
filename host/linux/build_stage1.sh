#!/bin/sh
# The loader on its own, on Linux: core/ with the stubs next to this script.  The build itself is CMakeLists.txt
# (GMV_STAGE1); this keeps the old argument and calls CMake.  It needs 32-bit gcc and CMake (Debian, Ubuntu:
# gcc-multilib cmake ninja-build).  Run the result with the game next to it, or with --app:
#   ./build/linux/GangstarMV-stage1 --verify-only --app /path/to/GangstarMV.app
# gmv_patch.bin goes next to it, the same file as on Windows.  The link address keeps the program above the game's range.
#   host/linux/build_stage1.sh [game]      game: the folder under host/games, default gangstar
set -e
cd "$(dirname "$0")/.."
game=${1:-gangstar}
for tool in gcc cmake; do
    command -v $tool >/dev/null || { echo "no $tool (Debian, Ubuntu: apt install gcc-multilib cmake ninja-build)"; exit 1; }
done
if command -v ninja >/dev/null; then gen=Ninja; bin=../build/cmake/linux-$game-stage1
else gen="Unix Makefiles"; bin=../build/cmake/linux-$game-stage1-make; fi
cmake -S . -B "$bin" -G "$gen" -DGMV_GAME="$game" -DGMV_STAGE1=ON -DGMV_OUT_DIR=
cmake --build "$bin"
