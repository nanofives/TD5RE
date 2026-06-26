# ttd_record.ps1
#
# Time Travel Debugging recorder for TD5_d3d.exe / td5re.exe.
# Wraps Microsoft's TTD.exe (bundled inside the WinDbg store-app install).
#
# Usage examples:
#   # Record the original game from clean launch (recording starts on spawn)
#   .\scripts\ttd_record.ps1 -Target original -OutDir traces -Tag sydney-ai-1
#
#   # Record the port (td5re.exe) for a quickrace scenario
#   .\scripts\ttd_record.ps1 -Target port -OutDir traces -Tag honolulu-ai-baseline
#
#   # Record an existing running process by PID
#   .\scripts\ttd_record.ps1 -Pid 12345 -OutDir traces -Tag attached
#
# Trace output: <OutDir>\td5_<tag>_<YYYYMMDD-HHMMSS>.run (+ .idx auto-built on first replay)
#
# Performance: TTD records at ~10-20x slowdown. A full 3-lap race may take
# 30-60 minutes wall time. Tag the trace tightly to the scenario you need.
# Stop the target window with ALT-F4 or "taskkill /F /PID <pid>" (the PID this
# script prints on launch) when the bug-reproducing moment has passed -- TTD
# writes the trace file on process exit. Do NOT "taskkill /F /IM <name>": it
# kills every concurrent session's instance of that exe, not just yours.
#
# Trace size: typically 100-500 MB per minute of recorded execution. Plan disk.

param(
    [ValidateSet('original','port','custom')]
    [string]$Target = 'port',
    [string]$CustomExe = '',
    [int]$ProcessId = 0,
    [string]$OutDir = 'traces',
    [string]$Tag = 'race',
    [string]$ProjectRoot = 'C:\Users\maria\Desktop\Proyectos\TD5RE'
)

$ErrorActionPreference = "Stop"

# Locate TTD.exe inside the WinDbg store-app install.
$wdbg = Get-AppxPackage Microsoft.WinDbg | Select-Object -First 1
if (-not $wdbg) {
    Write-Host "ERROR: WinDbg (Microsoft.WinDbg appx) not installed. winget install Microsoft.WinDbg first." -ForegroundColor Red
    exit 1
}
$ttd64 = Join-Path $wdbg.InstallLocation 'amd64\ttd\TTD.exe'
$ttd86 = Join-Path $wdbg.InstallLocation 'amd64\ttd\wow64\TTD.exe'
if (-not (Test-Path $ttd64)) {
    Write-Host "ERROR: TTD.exe not found at $ttd64" -ForegroundColor Red
    exit 1
}

# Resolve target
$targetExe = ""
$targetCwd = ""
switch ($Target) {
    'original' {
        $targetExe = Join-Path $ProjectRoot 'original\TD5_d3d.exe'
        $targetCwd = Join-Path $ProjectRoot 'original'
    }
    'port' {
        $targetExe = Join-Path $ProjectRoot 'td5re.exe'
        $targetCwd = $ProjectRoot
    }
    'custom' {
        if (-not $CustomExe) { Write-Host "ERROR: -CustomExe required when -Target custom" -ForegroundColor Red; exit 1 }
        $targetExe = $CustomExe
        $targetCwd = Split-Path $CustomExe -Parent
    }
}
if ($ProcessId -eq 0 -and -not (Test-Path $targetExe)) {
    Write-Host "ERROR: target exe not found: $targetExe" -ForegroundColor Red; exit 1
}

# Resolve output dir + trace path
$outDirAbs = Join-Path $ProjectRoot $OutDir
if (-not (Test-Path $outDirAbs)) { New-Item -ItemType Directory -Path $outDirAbs -Force | Out-Null }
$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$tracePath = Join-Path $outDirAbs ("td5_" + $Tag + "_" + $stamp + ".run")

# TTD is 64-bit and uses WoW64 injection automatically for 32-bit targets.
# TD5_d3d.exe + td5re.exe are both 32-bit Win32 -- TTD handles this transparently.
Write-Host "=== TTD recorder ===" -ForegroundColor Cyan
Write-Host "TTD.exe:    $ttd64"
Write-Host "Target:     $targetExe"
Write-Host "CWD:        $targetCwd"
Write-Host "Trace out:  $tracePath"
Write-Host ""
Write-Host "Recording will start now. Close the target window (or taskkill) to stop." -ForegroundColor Yellow
Write-Host "TTD overhead: ~10-20x slowdown during recording."
Write-Host ""

# Compose the TTD command.
# -out <path>          : trace output file
# -launch <exe>        : spawn the target and record from process start
# -attach <pid>        : attach to running process (no -launch)
# -accepteula          : skip EULA prompt
# -children            : NOT recording child processes (default; uncomment if needed)
if ($ProcessId -gt 0) {
    & "$ttd64" -accepteula -out $tracePath -attach $ProcessId
} else {
    # Use Push-Location so the target's relative paths (level zips, cars\, etc) resolve.
    Push-Location $targetCwd
    try {
        & "$ttd64" -accepteula -out $tracePath -launch $targetExe
    } finally {
        Pop-Location
    }
}

if (Test-Path $tracePath) {
    $size = (Get-Item $tracePath).Length / 1MB
    Write-Host ""
    Write-Host "=== Trace saved ===" -ForegroundColor Green
    Write-Host ("File: " + $tracePath)
    Write-Host ("Size: {0:N1} MB" -f $size)
    Write-Host ""
    Write-Host "Replay with:"
    Write-Host "  cdbX86.exe -z `"$tracePath`"   (then '!tt 0' to jump to start)"
    Write-Host "  OR via the windbg-mcp: load_dump $tracePath"
} else {
    Write-Host "WARNING: trace file not created at $tracePath" -ForegroundColor Yellow
}
