# [R12 TEX] One generation (+ optional chase frame) for items 1, 8b and 12b.
#
# Same two non-negotiables as every autotrack verify script:
#   1. DELETE re/assets/levels/level090 first -- the generated track is CACHED
#      and only rebuilt when the directory is absent.
#   2. Clear every TD5RE_* knob -- they persist for the life of the shell.
param(
    [Parameter(Mandatory=$true)][string]$Tag,
    [int]$Seed = 20260901,
    [int]$Span = -1,
    [int]$Wait = 45,
    [hashtable]$Env = @{}
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$out  = Join-Path $root "verify\r12tex_out"
New-Item -ItemType Directory -Force -Path $out | Out-Null

$lvl = Join-Path $root "re\assets\levels\level090"
Remove-Item -Recurse -Force $lvl -ErrorAction SilentlyContinue
if (Test-Path $lvl) { throw "level090 still present -- a stale track would be measured" }

Get-ChildItem Env: | Where-Object { $_.Name -like "TD5RE_*" } |
    ForEach-Object { Remove-Item -Path "Env:\$($_.Name)" -ErrorAction SilentlyContinue }

$env:TD5RE_CONTROL_PORT   = "37072"
$env:TD5RE_AUTOTRACK_SEED = "$Seed"
$env:TD5RE_WINDOW_TITLE   = "R12TEX-$Tag"
$png = $null
if ($Span -ge 0) {
    $png = Join-Path $out "$Tag.png"
    Remove-Item $png -ErrorAction SilentlyContinue
    $env:TD5RE_FRAMEDUMP     = $png
    $env:TD5RE_D3D12_CAPTURE = "1"
}
foreach ($k in $Env.Keys) { Set-Item -Path "Env:\$k" -Value $Env[$k] }

$a = @("--SkipIntro=1","--AutoRace=1","--DefaultTrack=60","--PlayerIsAI=0","--Opponents=0")
if ($Span -ge 0) { $a += "--StartSpanOffset=$Span" }
$p = Start-Process -FilePath (Join-Path $root "td5re.exe") -WorkingDirectory $root -PassThru -ArgumentList $a
Write-Output "PID=$($p.Id) tag=$Tag seed=$Seed span=$Span"
Start-Sleep -Seconds 4
Add-Type @"
using System;using System.Runtime.InteropServices;
public class WSR12 {
 [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h,IntPtr a,int x,int y,int cx,int cy,uint f);
 [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h,uint m,IntPtr w,IntPtr l);
}
"@ -ErrorAction SilentlyContinue
$p.Refresh()
if ($p.MainWindowHandle -ne 0) { [WSR12]::SetWindowPos($p.MainWindowHandle,[IntPtr]1,0,0,0,0,0x0013) | Out-Null }

$deadline = (Get-Date).AddSeconds($Wait)
while ((Get-Date) -lt $deadline -and -not $p.HasExited) { Start-Sleep -Milliseconds 500 }
# Snapshot the in-race frame BEFORE shutdown: TD5RE_FRAMEDUMP rewrites the same
# path every 30 frames, so after a clean exit it holds the LAST frame drawn,
# which is a frontend screen, not the race.
if ($png -and (Test-Path $png)) { Copy-Item $png (Join-Path $out "$Tag-race.png") -Force }
if (-not $p.HasExited) {
    $p.Refresh()
    if ($p.MainWindowHandle -ne 0) { [WSR12]::PostMessage($p.MainWindowHandle,0x0010,[IntPtr]::Zero,[IntPtr]::Zero) | Out-Null }
    $p.WaitForExit(25000) | Out-Null
}
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }   # PID-scoped, never by name
Start-Sleep -Seconds 1

if (Test-Path $lvl) {
    Write-Output ("level090 MODELS.DAT mtime = " + (Get-Item (Join-Path $lvl "MODELS.DAT")).LastWriteTime)
} else { Write-Output "WARNING: level090 was never written" }
if ($png -and (Test-Path $png)) { Write-Output "frame -> $png ($((Get-Item $png).Length) bytes)" }
elseif ($png) { Write-Output "NO FRAME produced for $Tag" }
Copy-Item (Join-Path $root "log\race.log") (Join-Path $out "$Tag-race.log") -Force -ErrorAction SilentlyContinue
