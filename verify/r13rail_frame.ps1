# [R13 RAIL] Launch one instrumented run and capture an IN-RACE frame through
# the live-control socket (see r13rail_frame.py for why the env framedump alone
# lands on the loading screen). Kills ONLY the PID it started.
param(
    [Parameter(Mandatory=$true)][int]$Seed,
    [int]$Span = 0,
    [Parameter(Mandatory=$true)][string]$Tag,
    [int]$Port = 37084,
    [int]$Timeout = 260,
    [string]$Cam = "",
    [hashtable]$Knobs = @{}
)
$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent $PSScriptRoot
$out  = Join-Path $root "verify\out_r13rail"
New-Item -ItemType Directory -Force -Path $out | Out-Null
$png  = Join-Path $out "$Tag.png"
Remove-Item $png -ErrorAction SilentlyContinue

Get-ChildItem Env: | Where-Object { $_.Name -like "TD5RE_*" } |
    ForEach-Object { Remove-Item -Path "Env:\$($_.Name)" -ErrorAction SilentlyContinue }

$lvl = Join-Path $root "re\assets\levels\level090"
if (Test-Path $lvl) { Remove-Item $lvl -Recurse -Force }

$env:TD5RE_CONTROL_PORT    = "$Port"
$env:TD5RE_AUTOTRACK_SEED  = "$Seed"
$env:TD5RE_WINDOW_TITLE    = "R13RAILF-$Tag"
$env:TD5RE_D3D12_CAPTURE   = "1"   # arms the framedump control verb
$env:TD5RE_R13_RAIL_REPORT = "1"
if ($Cam -ne "") { Set-Item -Path "Env:\$Cam" -Value "1" }
foreach ($k in $Knobs.Keys) { Set-Item -Path "Env:\$k" -Value $Knobs[$k] }

$p = Start-Process -FilePath (Join-Path $root "td5re.exe") -WorkingDirectory $root -PassThru `
     -ArgumentList "--SkipIntro=1","--AutoRace=1","--DefaultTrack=60","--Control=1",
                   "--StartSpanOffset=$Span","--PlayerIsAI=0","--Opponents=0"
Write-Output "PID=$($p.Id) seed=$Seed span=$Span tag=$Tag port=$Port"
Start-Sleep -Seconds 6
Add-Type @"
using System;using System.Runtime.InteropServices;
public class W13F {
 [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h,IntPtr a,int x,int y,int cx,int cy,uint f);
 [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h,uint m,IntPtr w,IntPtr l);
}
"@ -ErrorAction SilentlyContinue
$p.Refresh()
if ($p.MainWindowHandle -ne 0) { [W13F]::SetWindowPos($p.MainWindowHandle,[IntPtr]1,0,0,0,0,0x0013) | Out-Null }

python (Join-Path $PSScriptRoot "r13rail_frame.py") $Port $png $Timeout

if (-not $p.HasExited) {
    $p.Refresh()
    if ($p.MainWindowHandle -ne 0) { [W13F]::PostMessage($p.MainWindowHandle,0x0010,[IntPtr]::Zero,[IntPtr]::Zero) | Out-Null }
    $p.WaitForExit(30000) | Out-Null
}
try { if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force -ErrorAction Stop } } catch {}
Copy-Item (Join-Path $root "log\race.log") (Join-Path $out "$Tag-race.log") -Force -ErrorAction SilentlyContinue
Copy-Item (Join-Path $root "re\assets\levels\level090\TEXTURES.DAT") (Join-Path $out "$Tag-TEXTURES.DAT") -Force -ErrorAction SilentlyContinue
if (Test-Path $png) { Write-Output "frame -> $png ($((Get-Item $png).Length) bytes)" }
