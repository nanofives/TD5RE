# [R8 GUARD] top-down frame at one span, for A/B of the on-road guard.
# Matched-pose A/B: both sides capture at the same elapsed race time (the
# framedump fires once the race is up), from a parked AutoRace at the same
# StartSpanOffset, on the same pinned seed.
param(
    [Parameter(Mandatory=$true)][int]$Seed,
    [Parameter(Mandatory=$true)][int]$Span,
    [Parameter(Mandatory=$true)][string]$Tag,
    [int]$Alt = 9000,
    [int]$Wait = 70,
    [hashtable]$Env = @{}
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$out  = Join-Path $root "verify\out"
New-Item -ItemType Directory -Force -Path $out | Out-Null
$png = Join-Path $out "$Tag.png"
Remove-Item $png -ErrorAction SilentlyContinue

# Clear EVERY TD5RE_* knob first. They persist across launches inside one
# PowerShell session, so without this an "isolated" A/B silently measures the
# union of every knob the session ever set. This bit us once already.
Get-ChildItem Env: | Where-Object { $_.Name -like "TD5RE_*" } |
    ForEach-Object { Remove-Item -Path "Env:\$($_.Name)" -ErrorAction SilentlyContinue }

$env:TD5RE_CONTROL_PORT   = "37061"
$env:TD5RE_AUTOTRACK_SEED = "$Seed"
$env:TD5RE_WINDOW_TITLE   = "R8GUARD-$Tag"
$env:TD5RE_FRAMEDUMP      = $png
$env:TD5RE_D3D12_CAPTURE  = "1"
$env:TD5RE_CAM_TOPDOWN    = "$Alt"
foreach ($k in $Env.Keys) { Set-Item -Path "Env:\$k" -Value $Env[$k] }

$p = Start-Process -FilePath (Join-Path $root "td5re.exe") -WorkingDirectory $root -PassThru `
     -ArgumentList "--SkipIntro=1","--AutoRace=1","--DefaultTrack=60","--StartSpanOffset=$Span","--PlayerIsAI=0","--Opponents=0"
Write-Output "PID=$($p.Id) seed=$Seed span=$Span tag=$Tag"
Start-Sleep -Seconds 4
Add-Type @"
using System;using System.Runtime.InteropServices;
public class W2 {
 [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h,IntPtr a,int x,int y,int cx,int cy,uint f);
 [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h,uint m,IntPtr w,IntPtr l);
}
"@ -ErrorAction SilentlyContinue
$p.Refresh()
if ($p.MainWindowHandle -ne 0) { [W2]::SetWindowPos($p.MainWindowHandle,[IntPtr]1,0,0,0,0,0x0013) | Out-Null }

$deadline = (Get-Date).AddSeconds($Wait)
while ((Get-Date) -lt $deadline -and -not $p.HasExited) { Start-Sleep -Milliseconds 500 }
if (-not $p.HasExited) {
    $p.Refresh()
    if ($p.MainWindowHandle -ne 0) { [W2]::PostMessage($p.MainWindowHandle,0x0010,[IntPtr]::Zero,[IntPtr]::Zero) | Out-Null }
    $p.WaitForExit(25000) | Out-Null
}
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
Start-Sleep -Seconds 1
if (Test-Path $png) { Write-Output "frame -> $png ($((Get-Item $png).Length) bytes)" }
else { Write-Output "NO FRAME produced for $Tag" }
Copy-Item (Join-Path $root "log\race.log") (Join-Path $out "$Tag-race.log") -Force -ErrorAction SilentlyContinue
