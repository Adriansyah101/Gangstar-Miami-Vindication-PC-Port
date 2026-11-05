# Builds a game's exe (32-bit) into ..\build with MSYS2's mingw32 toolchain.  The build itself is CMakeLists.txt; this
# keeps the old arguments and calls CMake (build folder ..\build\cmake\<game>-<kind>).
# Needs, in MSYS2:  pacman -S mingw-w64-i686-gcc mingw-w64-i686-cmake mingw-w64-i686-ninja
# gmv_patch.bin must sit next to the exe:  python -I ..\tools\genlow.py <binary> ..\build\gmv_patch.bin
# SDL3 comes from ..\tools\get_sdl3.ps1 (into third_party\SDL3, not in the repo); SDL3.dll is copied next to the exe.
#   -Game     the folder under games\ (default gangstar)
#   -Release  public build into ..\build\release: no -g, symbols stripped (no local paths in the exe),
#             GUI subsystem (no console window; everything still goes to cache\<game>.log unless LOG=0),
#             GMV_RELEASE (the [Diagnostics] traces and [Testing] aids left out: GMV_DEV in log.h)
#   -Dev      shareable dev build into ..\build\dev: every diagnostic kept and the console window, but
#             no -g and symbols stripped, so no local paths end up in the exe
#   -NoIcon   leave out the game's app.ico even if it is present
param([string]$Game = 'gangstar', [switch]$Release, [switch]$Dev, [switch]$NoIcon)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$env:PATH = "C:\msys64\mingw32\bin;$env:PATH"
foreach ($tool in 'gcc', 'cmake', 'ninja') {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
        throw "no $tool`: in MSYS2, pacman -S mingw-w64-i686-gcc mingw-w64-i686-cmake mingw-w64-i686-ninja"
    }
}
$kind = if ($Release) { 'release' } elseif ($Dev) { 'dev' } else { 'plain' }
$icon = if ($NoIcon) { 'ON' } else { 'OFF' }
$bin = Join-Path $here "..\build\cmake\$Game-$kind"
& cmake -S $here -B $bin -G Ninja -DCMAKE_C_COMPILER=gcc "-DGMV_GAME=$Game" "-DGMV_BUILD=$kind" "-DGMV_NOICON=$icon" `
    '-DGMV_OUT_DIR=' '-DGMV_STAGE1=OFF'
if ($LASTEXITCODE) { exit $LASTEXITCODE }
& cmake --build $bin
exit $LASTEXITCODE
