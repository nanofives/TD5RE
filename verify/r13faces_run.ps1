# [R13 FACES] One autotrack generation run, A/B-able on TD5RE_R13_FACES_WATERGEOM.
#
# The numbers this round is judged on (MISSING_SIDE_FACES, FLAT_RUNS,
# buildings_dropped, span_sides) are printed by tg_r13_faces_report at GENERATION
# time, so this does not need to reach the race -- it waits for level090 to
# appear and settle, then closes the window CLEANLY, because race.log is only
# flushed on a clean shutdown.
#
# Two things here are not optional and cost a whole round each when skipped:
#   * every TD5RE_* var is cleared first -- they PERSIST across launches inside
#     one PowerShell session, so a knob from the previous run would silently
#     ride along;
#   * re/assets/levels/level090 is deleted first -- it is CACHED and only
#     rebuilt when the directory is ABSENT, so without this you measure the
#     PREVIOUS track and the A/B compares a file with itself.
# The process is killed BY PID, never by name: sibling agents run concurrently.
param(
    [Parameter(Mandatory=$true)][int]$Seed,
    [Parameter(Mandatory=$true)][string]$Tag,
    [ValidateSet("on","off")][string]$Geom = "on",
    [int]$Wait = 260,
    [int]$Port = 37093,
    [switch]$Verbose1,
    [int]$Span = -1
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$out  = Join-Path $root "verify\out"
New-Item -ItemType Directory -Force -Path $out | Out-Null

Get-ChildItem Env: | Where-Object { $_.Name -like "TD5RE_*" } |
    ForEach-Object { Remove-Item -Path "Env:\$($_.Name)" -ErrorAction SilentlyContinue }

$lvl = Join-Path $root "re\assets\levels\level090"
if (Test-Path $lvl) { Remove-Item $lvl -Recurse -Force }

$env:TD5RE_CONTROL_PORT   = "$Port"
$env:TD5RE_AUTOTRACK_SEED = "$Seed"
$env:TD5RE_WINDOW_TITLE   = "R13FACES-$Tag"
$env:TD5RE_R9_BRIDGE_REPORT = "1"     # the over-water audit only runs opted-in
if ($Geom -eq "off") { $env:TD5RE_R13_FACES_WATERGEOM = "0" }
if ($Verbose1) {
    $env:TD5RE_R13_FACES_REPORT = "1"
    if ($Span -ge 0) { $env:TD5RE_R13_FACES_SPAN = "$Span" }
}

$p = Start-Process -FilePath (Join-Path $root "td5re.exe") -WorkingDirectory $root -PassThru `
     -ArgumentList "--SkipIntro=1","--AutoRace=1","--DefaultTrack=60","--PlayerIsAI=1","--Opponents=0"
Write-Output "PID=$($p.Id) seed=$Seed geom=$Geom tag=$Tag port=$Port"
Start-Sleep -Seconds 5
Add-Type @"
using System;using System.Runtime.InteropServices;
public class W13F {
 [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h,IntPtr a,int x,int y,int cx,int cy,uint f);
 [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h,uint m,IntPtr w,IntPtr l);
}
"@ -ErrorAction SilentlyContinue
$p.Refresh()
# HWND_BOTTOM + NOACTIVATE: a launch must never steal focus from the user.
if ($p.MainWindowHandle -ne 0) { [W13F]::SetWindowPos($p.MainWindowHandle,[IntPtr]1,0,0,0,0,0x0013) | Out-Null }

$models = Join-Path $lvl "MODELS.DAT"
$deadline = (Get-Date).AddSeconds($Wait)
while ((Get-Date) -lt $deadline -and -not $p.HasExited) {
    if (Test-Path $models) { Start-Sleep -Seconds 12; break }
    Start-Sleep -Seconds 2
}
if (-not (Test-Path $models)) { Write-Output "WARN: MODELS.DAT never appeared for $Tag" }

if (-not $p.HasExited) {
    $p.Refresh()
    if ($p.MainWindowHandle -ne 0) { [W13F]::PostMessage($p.MainWindowHandle,0x0010,[IntPtr]::Zero,[IntPtr]::Zero) | Out-Null }
    $p.WaitForExit(60000) | Out-Null
}
try { if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force -ErrorAction Stop } } catch {}
Start-Sleep -Seconds 1

Copy-Item (Join-Path $root "log\race.log") (Join-Path $out "$Tag-race.log") -Force -ErrorAction SilentlyContinue
Copy-Item $models (Join-Path $out "$Tag-MODELS.DAT") -Force -ErrorAction SilentlyContinue
Copy-Item (Join-Path $lvl "STRIP.DAT") (Join-Path $out "$Tag-STRIP.DAT") -Force -ErrorAction SilentlyContinue
Write-Output "--- $Tag ---"
Select-String -Path (Join-Path $out "$Tag-race.log") -Pattern "r13faces SUMMARY|R9BRIDGE|over-water audit" |
    ForEach-Object { $_.Line }
