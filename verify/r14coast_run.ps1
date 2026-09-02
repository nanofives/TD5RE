# [R14 COAST] One instrumented autotrack run: regenerate level090 for a seed,
# park at a span, optionally dump a frame, keep race.log.
#
# level090 is CACHED and only rebuilt when the directory is ABSENT, and every
# TD5RE_* knob PERSISTS inside one PowerShell session, so both are cleared here
# rather than trusted. Port/title/out-dir are r14-coast specific because sibling
# agents run at the same time; the process is killed BY PID, never by name.
param(
    [Parameter(Mandatory=$true)][int]$Seed,
    [int]$Span = 0,
    [Parameter(Mandatory=$true)][string]$Tag,
    [int]$Wait = 400,
    [int]$Port = 37091,
    [string]$Cam = "",
    [switch]$NoFrame,
    [hashtable]$Knobs = @{}
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$out  = Join-Path $root "verify\out_r14coast"
New-Item -ItemType Directory -Force -Path $out | Out-Null
$png = Join-Path $out "$Tag.png"
Remove-Item $png -ErrorAction SilentlyContinue

Get-ChildItem Env: | Where-Object { $_.Name -like "TD5RE_*" } |
    ForEach-Object { Remove-Item -Path "Env:\$($_.Name)" -ErrorAction SilentlyContinue }

$lvl = Join-Path $root "re\assets\levels\level090"
if (Test-Path $lvl) { Remove-Item $lvl -Recurse -Force }

$env:TD5RE_CONTROL_PORT       = "$Port"
$env:TD5RE_AUTOTRACK_SEED     = "$Seed"
$env:TD5RE_WINDOW_TITLE       = "R14COAST-$Tag"
$env:TD5RE_R14_COAST_REPORT   = "1"
if (-not $NoFrame) {
    $env:TD5RE_FRAMEDUMP     = $png
    $env:TD5RE_D3D12_CAPTURE = "1"
}
if ($Cam -ne "") { Set-Item -Path "Env:\$Cam" -Value "1" }
foreach ($k in $Knobs.Keys) { Set-Item -Path "Env:\$k" -Value $Knobs[$k] }

$p = Start-Process -FilePath (Join-Path $root "td5re.exe") -WorkingDirectory $root -PassThru `
     -ArgumentList "--SkipIntro=1","--AutoRace=1","--DefaultTrack=60",
                   "--StartSpanOffset=$Span","--PlayerIsAI=0","--Opponents=0"
Write-Output "PID=$($p.Id) seed=$Seed span=$Span tag=$Tag port=$Port"
Start-Sleep -Seconds 5
Add-Type @"
using System;using System.Runtime.InteropServices;
public class W14C {
 [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h,IntPtr a,int x,int y,int cx,int cy,uint f);
}
"@ -ErrorAction SilentlyContinue
$p.Refresh()
# HWND_BOTTOM + NOACTIVATE: a launch must never steal focus from the user.
if ($p.MainWindowHandle -ne 0) { [W14C]::SetWindowPos($p.MainWindowHandle,[IntPtr]1,0,0,0,0,0x0013) | Out-Null }

$deadline = (Get-Date).AddSeconds($Wait)
while ((Get-Date) -lt $deadline -and -not $p.HasExited) { Start-Sleep -Milliseconds 500 }

# Clean shutdown ONLY: race.log flushes on WM_CLOSE, a force-kill loses it.
if (-not $p.HasExited) {
    Add-Type @"
using System;using System.Runtime.InteropServices;
public class W14M { [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h,uint m,IntPtr w,IntPtr l); }
"@ -ErrorAction SilentlyContinue
    $p.Refresh()
    if ($p.MainWindowHandle -ne 0) { [W14M]::PostMessage($p.MainWindowHandle,0x0010,[IntPtr]::Zero,[IntPtr]::Zero) | Out-Null }
    if (-not $p.WaitForExit(60000)) { Stop-Process -Id $p.Id -Force }
}
Copy-Item (Join-Path $root "log\race.log") (Join-Path $out "$Tag-race.log") -ErrorAction SilentlyContinue
$md = Join-Path $root "re\assets\levels\level090\MODELS.DAT"
if (Test-Path $md) {
    $h = (Get-FileHash $md -Algorithm SHA256).Hash
    Write-Output ("MODELS.DAT bytes={0} sha={1}" -f (Get-Item $md).Length, $h)
}
$sd = Join-Path $root "re\assets\levels\level090\STRIP.DAT"
if (Test-Path $sd) {
    $h = (Get-FileHash $sd -Algorithm SHA256).Hash
    Write-Output ("STRIP.DAT  bytes={0} sha={1}" -f (Get-Item $sd).Length, $h)
}
Write-Output "DONE $Tag"
