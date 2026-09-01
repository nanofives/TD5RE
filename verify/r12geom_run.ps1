# [R12 GEOM] One pinned-seed run: force a track REGENERATE, park the car at a
# span, dump one frame, and keep the generated level + race.log for byte
# attribution. Everything this round is verified against the pair of runs this
# script produces (knob on / knob off), never against one build vs another.
param(
    [int]$Seed = 20260901,
    [Parameter(Mandatory=$true)][int]$Span,
    [Parameter(Mandatory=$true)][string]$Tag,
    [int]$Wait = 55,
    [int]$Port = 37068,
    [hashtable]$Knobs = @{}
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$out  = Join-Path $root "verify\r12geom"
New-Item -ItemType Directory -Force -Path $out | Out-Null
$png = Join-Path $out "$Tag.png"
Remove-Item $png -ErrorAction SilentlyContinue

# TD5RE_* knobs PERSIST across launches inside one PowerShell session.
Get-ChildItem Env: | Where-Object { $_.Name -like "TD5RE_*" } |
    ForEach-Object { Remove-Item -Path "Env:\$($_.Name)" -ErrorAction SilentlyContinue }

# level090 is CACHED: it only regenerates when the directory is absent.
$lvl = Join-Path $root "re\assets\levels\level090"
if (Test-Path $lvl) { Remove-Item $lvl -Recurse -Force }

$env:TD5RE_CONTROL_PORT   = "$Port"
$env:TD5RE_AUTOTRACK_SEED = "$Seed"
$env:TD5RE_WINDOW_TITLE   = "R12GEOM-$Tag"
$env:TD5RE_FRAMEDUMP      = $png
$env:TD5RE_D3D12_CAPTURE  = "1"
foreach ($k in $Knobs.Keys) { Set-Item -Path "Env:\$k" -Value $Knobs[$k] }

$p = Start-Process -FilePath (Join-Path $root "td5re.exe") -WorkingDirectory $root -PassThru `
     -ArgumentList "--SkipIntro=1","--AutoRace=1","--DefaultTrack=60","--StartSpanOffset=$Span","--PlayerIsAI=0","--Opponents=0"
Write-Output "PID=$($p.Id) seed=$Seed span=$Span tag=$Tag"
Start-Sleep -Seconds 4
Add-Type @"
using System;using System.Runtime.InteropServices;
public class W3R12 {
 [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h,IntPtr a,int x,int y,int cx,int cy,uint f);
 [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h,uint m,IntPtr w,IntPtr l);
}
"@ -ErrorAction SilentlyContinue
$p.Refresh()
if ($p.MainWindowHandle -ne 0) { [W3R12]::SetWindowPos($p.MainWindowHandle,[IntPtr]1,0,0,0,0,0x0013) | Out-Null }

$deadline = (Get-Date).AddSeconds($Wait)
while ((Get-Date) -lt $deadline -and -not $p.HasExited) { Start-Sleep -Milliseconds 500 }
if (-not $p.HasExited) {
    $p.Refresh()
    # Clean shutdown: race.log only flushes on a real WM_CLOSE.
    if ($p.MainWindowHandle -ne 0) { [W3R12]::PostMessage($p.MainWindowHandle,0x0010,[IntPtr]::Zero,[IntPtr]::Zero) | Out-Null }
    $p.WaitForExit(30000) | Out-Null
}
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }   # OUR pid only
Start-Sleep -Seconds 1

if (Test-Path $png) { Write-Output "frame -> $png ($((Get-Item $png).Length) bytes)" }
else { Write-Output "NO FRAME produced for $Tag" }
Copy-Item (Join-Path $root "log\race.log") (Join-Path $out "$Tag-race.log") -Force -ErrorAction SilentlyContinue
foreach ($d in @("MODELS.DAT","STRIP.DAT","LEVELINF.DAT")) {
    $src = Join-Path $lvl $d
    if (Test-Path $src) { Copy-Item $src (Join-Path $out "$Tag-$d") -Force }
}
Get-ChildItem $out -Filter "$Tag-*.DAT" | ForEach-Object {
    "{0}  {1,10}  {2}" -f (Get-FileHash $_.FullName -Algorithm SHA256).Hash.Substring(0,16), $_.Length, $_.Name
}
