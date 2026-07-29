# Trial x86_64 compile of the port -- enumerate ARCHITECTURE breakage at compile
# time, without building or running anything.
#
# Why this exists: the x64 retarget (docs/plans/X64_DXR_ROADMAP.md) needs a
# worklist that is MEASURED rather than estimated, and one that stays available
# even when the pre-existing nvwgf2um.dll fault blocks golden verification.
# This script runs nothing, so it is immune to that.
#
# The toolchain at td5mod/deps/mingw64/ is deliberately GCC 16.1.0 -- the SAME
# version as the bundled i686 one, same packager (winlibs) -- so every
# diagnostic is an arch effect with zero compiler-version noise.
#
# Compile-only (-c): no linking, so no x64 zlib or import libs are needed.
# zlib's two headers are arch-neutral and get staged from the i686 sysroot.
#
# WHAT THIS CANNOT FIND: storing a truncated pointer in a uint32_t (the mesh
# header fields) is valid C on x86_64. It fails silently at RUNTIME above 4 GB.
# That class needs the golden traces, not the compiler.
#
# Usage:  pwsh -NoProfile -File scripts/x64_trial.ps1 [-WorkDir <dir>]

param(
    [string]$WorkDir = (Join-Path ([System.IO.Path]::GetTempPath()) 'td5re_x64_trial')
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$gcc  = "$root\td5mod\deps\mingw64\mingw64\bin\gcc.exe"
$src  = "$root\td5mod\src\td5re"
$wrap = "$root\td5mod\ddraw_wrapper\src"

if (-not (Test-Path $gcc)) {
    Write-Error @"
x86_64 toolchain not found at:
  $gcc
It is gitignored (753 MB). Fetch winlibs-x86_64-posix-seh-gcc-16.1.0-*.zip and
unpack it so that path exists. Must be GCC 16.1.0 to match the i686 toolchain,
otherwise the diagnostics below mix arch effects with version differences.
"@
}

$zinc   = Join-Path $WorkDir 'zinc'
$outdir = Join-Path $WorkDir 'x64obj'
New-Item -ItemType Directory -Force $zinc, $outdir | Out-Null

# stage arch-neutral zlib headers (the x64 toolchain ships none)
foreach ($h in 'zlib.h', 'zconf.h') {
    $s = "$root\td5mod\deps\mingw\mingw32\i686-w64-mingw32\include\$h"
    if (Test-Path $s) { Copy-Item $s $zinc -Force }
}

# cflags.txt minus the 32-bit-only arch flags (-m32/-msse2/-mfpmath=sse; x86_64
# mandates SSE2). Keep the -Werror= classes: int-conversion and
# incompatible-pointer-types ARE the pointer-truncation detectors.
$flags = @('-c', '-O2', '-fwrapv', '-Wall', '-Wextra', '-DWIN32', '-DTD5_INFLATE_USE_ZLIB',
           '-Werror=implicit-function-declaration', '-Werror=return-type',
           '-Werror=int-conversion', '-Werror=incompatible-pointer-types',
           '-m64', '-I', $src, '-I', $wrap, '-I', $zinc)

$mods = Get-Content "$src\srcs.txt" | Where-Object { $_ -notmatch '^\s*#' -and $_.Trim() }
$log = Join-Path $WorkDir 'x64_trial.log'
Remove-Item $log -ErrorAction SilentlyContinue

$okCount = 0; $failed = @()
foreach ($m in $mods) {
    $m = $m.Trim()
    $path = if (Test-Path "$src\$m") { "$src\$m" } else { "$root\td5mod\$m" }
    if (-not (Test-Path $path)) { Add-Content $log "=== MISSING SOURCE: $m"; continue }
    $obj = Join-Path $outdir ((Split-Path $m -Leaf) -replace '\.c$', '.o')
    $out = & $gcc @flags -o $obj $path 2>&1
    if ($LASTEXITCODE -eq 0) { $okCount++ }
    else { $failed += $m; Add-Content $log "=== FAILED: $m"; $out | ForEach-Object { Add-Content $log $_ } }
}

"modules: $($mods.Count)   compiled OK: $okCount   FAILED: $($failed.Count)"
""
"=== failing modules ==="
$failed | ForEach-Object { "  $_" }
""
"=== error lines by GCC diagnostic class ==="
if (Test-Path $log) {
    Get-Content $log | Where-Object { $_ -match 'error:' } |
        ForEach-Object { if ($_ -match '\[-W([a-z0-9-]+)\]') { "-W$($matches[1])" } else { 'error (no -W class)' } } |
        Group-Object | Sort-Object Count -Descending | Select-Object Count, Name | Format-Table -AutoSize
    "total error lines: $((Get-Content $log | Where-Object { $_ -match 'error:' }).Count)"
    "log: $log"
}
