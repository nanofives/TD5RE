# [R12 OVERPASS] One instrumented run: regenerate level090 for a seed, park at a
# span, dump a frame, keep race.log. Deletes level090 first (it is CACHED and only
# rebuilt when the directory is ABSENT) and clears every TD5RE_* knob (they persist
# across launches inside one PowerShell session).
param(
    [Parameter(Mandatory=$true)][int]$Seed,
    [int]$Span = 0,
    [Parameter(Mandatory=$true)][string]$Tag,
    [int]$Wait = 40,
    [int]$Port = 37072,
    [string]$Cam = "",
    [switch]$NoRegen,
    [hashtable]$Knobs = @{}
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$out  = Join-Path $root "verify\out"
New-Item -ItemType Directory -Force -Path $out | Out-Null
$png = Join-Path $out "$Tag.png"
Remove-Item $png -ErrorAction SilentlyContinue

Get-ChildItem Env: | Where-Object { $_.Name -like "TD5RE_*" } |
    ForEach-Object { Remove-Item -Path "Env:\$($_.Name)" -ErrorAction SilentlyContinue }

$lvl = Join-Path $root "re\assets\levels\level090"
if (-not $NoRegen) { if (Test-Path $lvl) { Remove-Item $lvl -Recurse -Force } }

$env:TD5RE_CONTROL_PORT   = "$Port"
$env:TD5RE_AUTOTRACK_SEED = "$Seed"
$env:TD5RE_WINDOW_TITLE   = "R12OP-$Tag"
$env:TD5RE_FRAMEDUMP      = $png
$env:TD5RE_D3D12_CAPTURE  = "1"
if ($Cam -ne "") { Set-Item -Path "Env:\$Cam" -Value "1" }
foreach ($k in $Knobs.Keys) { Set-Item -Path "Env:\$k" -Value $Knobs[$k] }

$p = Start-Process -FilePath (Join-Path $root "td5re.exe") -WorkingDirectory $root -PassThru `
     -ArgumentList "--SkipIntro=1","--AutoRace=1","--DefaultTrack=60","--StartSpanOffset=$Span","--PlayerIsAI=0","--Opponents=0"
Write-Output "PID=$($p.Id) seed=$Seed span=$Span tag=$Tag"
Start-Sleep -Seconds 5
Add-Type @"
using System;using System.Runtime.InteropServices;
public class W12 {
 [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h,IntPtr a,int x,int y,int cx,int cy,uint f);
 [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h,uint m,IntPtr w,IntPtr l);
}
"@ -ErrorAction SilentlyContinue
$p.Refresh()
if ($p.MainWindowHandle -ne 0) { [W12]::SetWindowPos($p.MainWindowHandle,[IntPtr]1,0,0,0,0,0x0013) | Out-Null }

$deadline = (Get-Date).AddSeconds($Wait)
while ((Get-Date) -lt $deadline -and -not $p.HasExited) { Start-Sleep -Milliseconds 500 }
if (-not $p.HasExited) {
    $p.Refresh()
    if ($p.MainWindowHandle -ne 0) { [W12]::PostMessage($p.MainWindowHandle,0x0010,[IntPtr]::Zero,[IntPtr]::Zero) | Out-Null }
    $p.WaitForExit(30000) | Out-Null
}
try { if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force -ErrorAction Stop } } catch {}
Start-Sleep -Seconds 1
if (Test-Path $png) { Write-Output "frame -> $png ($((Get-Item $png).Length) bytes)" }
else { Write-Output "NO FRAME produced for $Tag" }
Copy-Item (Join-Path $root "log\race.log") (Join-Path $out "$Tag-race.log") -Force -ErrorAction SilentlyContinue
Copy-Item (Join-Path $root "re\assets\levels\level090\MODELS.DAT") (Join-Path $out "$Tag-MODELS.DAT") -Force -ErrorAction SilentlyContinue
Copy-Item (Join-Path $root "re\assets\levels\level090\STRIP.DAT") (Join-Path $out "$Tag-STRIP.DAT") -Force -ErrorAction SilentlyContinue
