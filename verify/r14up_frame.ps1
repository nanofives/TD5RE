# [R14 OVERPASS item 3] One instrumented autotrack run parked short of a
# crossing, with a frame dumped. Port/title are r14-overpass specific because
# sibling agents run at the same time; the process is killed BY PID only.
param(
    [Parameter(Mandatory=$true)][int]$Seed,
    [int]$Span = 0,
    [Parameter(Mandatory=$true)][string]$Tag,
    [int]$Wait = 340,
    [int]$Port = 37094,
    [string]$Cam = "",
    [hashtable]$Knobs = @{}
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$out  = Join-Path $root "verify\out_r14up"
New-Item -ItemType Directory -Force -Path $out | Out-Null
$png = Join-Path $out "$Tag.png"
Remove-Item $png -ErrorAction SilentlyContinue

Get-ChildItem Env: | Where-Object { $_.Name -like "TD5RE_*" } |
    ForEach-Object { Remove-Item -Path "Env:\$($_.Name)" -ErrorAction SilentlyContinue }

$lvl = Join-Path $root "re\assets\levels\level090"
if (Test-Path $lvl) { Remove-Item $lvl -Recurse -Force }

$env:TD5RE_CONTROL_PORT   = "$Port"
$env:TD5RE_AUTOTRACK_SEED = "$Seed"
$env:TD5RE_WINDOW_TITLE   = "R14UP-$Tag"
$env:TD5RE_FRAMEDUMP      = $png
$env:TD5RE_D3D12_CAPTURE  = "1"
if ($Cam -ne "") { Set-Item -Path "Env:\$Cam" -Value "1" }
foreach ($k in $Knobs.Keys) { Set-Item -Path "Env:\$k" -Value $Knobs[$k] }

$p = Start-Process -FilePath (Join-Path $root "td5re.exe") -WorkingDirectory $root -PassThru `
     -ArgumentList "--SkipIntro=1","--AutoRace=1","--DefaultTrack=60",
                   "--StartSpanOffset=$Span","--PlayerIsAI=0","--Opponents=0","--Control=1"
Write-Output "PID=$($p.Id) seed=$Seed span=$Span tag=$Tag port=$Port"
Start-Sleep -Seconds 5
Add-Type @"
using System;using System.Runtime.InteropServices;
public class W14U {
 [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h,IntPtr a,int x,int y,int cx,int cy,uint f);
 [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h,uint m,IntPtr w,IntPtr l);
}
"@ -ErrorAction SilentlyContinue
$p.Refresh()
if ($p.MainWindowHandle -ne 0) { [W14U]::SetWindowPos($p.MainWindowHandle,[IntPtr]1,0,0,0,0,0x0013) | Out-Null }

# Drive the control socket: only ask for the framedump once get_state says RACE.
& python (Join-Path $PSScriptRoot "r13rail_frame.py") $Port $png $Wait

$p.Refresh()
if (-not $p.HasExited) {
    if ($p.MainWindowHandle -ne 0) { [W14U]::PostMessage($p.MainWindowHandle,0x0010,[IntPtr]::Zero,[IntPtr]::Zero) | Out-Null }
    $p.WaitForExit(30000) | Out-Null
}
try { if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force -ErrorAction Stop } } catch {}
Start-Sleep -Seconds 1
if (Test-Path $png) { Write-Output "frame -> $png ($((Get-Item $png).Length) bytes)" }
else { Write-Output "NO FRAME produced for $Tag" }
Copy-Item (Join-Path $root "log\race.log") (Join-Path $out "$Tag-race.log") -Force -ErrorAction SilentlyContinue
