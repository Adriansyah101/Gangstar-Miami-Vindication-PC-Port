# Compiles and links the host for 32-bit Linux from Windows, with clang and lld, as a check that the Linux code still
# builds: the same sources as host\linux\build.sh (or build_stage1.sh with -Stage1).  The program it makes cannot be
# run here.  Needs tools\get_linux_sysroot.py <folder> first, for -Sysroot; clang.exe and ld.lld.exe on the PATH.
#   -Game     the folder under host\games (default template)
#   -Stage1   only the loader, with the stubs in host\linux (stops after verifying)
param([Parameter(Mandatory)][string]$Sysroot, [string]$Out, [string]$Game = 'template', [switch]$Stage1)
$ErrorActionPreference = 'Continue'
$hostDir = Join-Path $PSScriptRoot '..\host'
if (-not $Out) { $Out = Join-Path $env:TEMP 'gmv-linux-check' }
New-Item -ItemType Directory -Force $Out | Out-Null
$Out = (Resolve-Path $Out).Path
Get-ChildItem $Out -Filter *.o | Remove-Item -Force -Confirm:$false    # a run for another game leaves its own
$Sysroot = (Resolve-Path $Sysroot).Path
Push-Location $hostDir
try {
    $name = (Select-String -Path "games/$Game/layout.h" -Pattern '#define GMV_GAME_NAME\s+"([^"]+)"').Matches[0].Groups[1].Value
    $gameDirs = @("games/$Game") + @(Get-ChildItem "games/$Game" -Directory -Recurse | ForEach-Object { (Resolve-Path -Relative $_.FullName) -replace '^\.[\\/]', '' -replace '\\', '/' })
    $dirs = if ($Stage1) { @('core') } else { @('core', 'shims', 'shims/gl') + $gameDirs }
    $src = @($dirs | ForEach-Object { $d = $_; Get-ChildItem $d -File | Where-Object { $_.Extension -in '.c', '.S' } | ForEach-Object { "$d/$($_.Name)" } })
    if ($Stage1) { $src += 'linux/stage1_stubs.c', "games/$Game/settings.c"; $name += '-stage1' }
    $incArgs = @(@('core', 'shims', 'shims/gl') + $gameDirs | ForEach-Object { '-I', $_ }) + @('-isystem', 'third_party/SDL3/include', '-isystem', 'vendor', '-isystem', "$Sysroot/usr/include/freetype2")
    $objs = @()
    $warned = 0
    foreach ($s in $src) {
        $o = Join-Path $Out (($s -replace '[/\\]', '_') + '.o')
        # -Wno-unknown-attributes: clang does not know callee_pop_aggregate_return (Linux's default anyway) or optimize.
        # -D_FILE_OFFSET_BITS=64: 32-bit stat/readdir fail with EOVERFLOW on 64-bit inode numbers (WSL's /mnt/c, btrfs, xfs)
        $text = & clang --target=i386-linux-gnu --sysroot=$Sysroot -std=gnu11 -O2 -Wall -Wextra -Wno-unknown-attributes -msse2 `
            -fno-pic -freg-struct-return -D_FILE_OFFSET_BITS=64 @incArgs -c $s -o $o 2>&1 | ForEach-Object { "$_" }
        $code = $LASTEXITCODE
        if ($text) { $text | Write-Host; $warned++ }
        if ($code) { throw "$s does not compile" }
        $objs += $o
    }
    $exe = Join-Path $Out $name
    $libs = if ($Stage1) { @('-lpthread', '-ldl') } else { @('-lSDL3', '-lGL', '-lm', '-lpthread', '-ldl') }
    $text = & clang --target=i386-linux-gnu --sysroot=$Sysroot -fuse-ld=lld -no-pie '-Wl,--image-base=0x08048000' @objs @libs -o $exe 2>&1 | ForEach-Object { "$_" }
    $code = $LASTEXITCODE
    if ($text) { $text | Write-Host }
    if ($code) { throw 'it does not link' }
    # The game's range has to stay free for the program to map at its start: lld's own default base is inside it.
    $low = & llvm-readelf -lW $exe | Select-String '^\s*LOAD\s+\S+\s+0x([0-9a-f]+)' | ForEach-Object { [Convert]::ToInt64($_.Matches[0].Groups[1].Value, 16) } | Measure-Object -Minimum
    if ($low.Minimum -lt 0x00D00000) { throw ('the program is linked at 0x{0:x}, inside the game''s range' -f [long]$low.Minimum) }
    "linked $exe, $($src.Count) sources, $warned with warnings"
} finally { Pop-Location }
