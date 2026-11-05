# Fetches the official SDL3 build for the 32-bit host into host\third_party\SDL3 (headers, import library, DLL, licence).
# One pinned version, checked by hash: a new SDL3 is a deliberate change here.
$ErrorActionPreference = 'Stop'
$version = '3.4.20'
$sha256 = '5d321d3d2d499cfabad3023921e4b56bbec03558060f4e195ce626d200126442'
$dest = Join-Path (Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)) 'host\third_party\SDL3'
$url = "https://github.com/libsdl-org/SDL/releases/download/release-$version/SDL3-devel-$version-mingw.zip"

$zip = Join-Path ([IO.Path]::GetTempPath()) "SDL3-devel-$version-mingw.zip"
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$ProgressPreference = 'SilentlyContinue'
Invoke-WebRequest -Uri $url -OutFile $zip -UseBasicParsing
try {
    $got = (Get-FileHash $zip -Algorithm SHA256).Hash
    if ($got -ne $sha256) { throw "SDL3 download has the wrong hash: $got" }

    if (Test-Path $dest) { Remove-Item $dest -Recurse -Force }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $root = "SDL3-$version/"
    $arch = "${root}i686-w64-mingw32/"
    $z = [IO.Compression.ZipFile]::OpenRead($zip)
    try {
        foreach ($e in $z.Entries) {
            $rel = $null
            if ($e.FullName -eq "${root}LICENSE.txt") { $rel = 'LICENSE.txt' }
            elseif ($e.FullName -eq "${arch}bin/SDL3.dll") { $rel = 'bin\SDL3.dll' }
            elseif ($e.FullName -eq "${arch}lib/libSDL3.dll.a") { $rel = 'lib\libSDL3.dll.a' }
            elseif ($e.FullName.StartsWith("${arch}include/SDL3/") -and $e.Name) { $rel = "include\SDL3\$($e.Name)" }
            if (-not $rel) { continue }
            $to = Join-Path $dest $rel
            New-Item -ItemType Directory -Force (Split-Path $to) | Out-Null
            [IO.Compression.ZipFileExtensions]::ExtractToFile($e, $to, $true)
        }
    } finally { $z.Dispose() }
} finally { Remove-Item $zip -ErrorAction SilentlyContinue }
Write-Host "SDL3 $version in $dest"
