# [R8 GUARD] boot the worktree exe, regenerate the auto track for one seed,
# close it cleanly (race.log only flushes on clean shutdown) and copy the log out.
param(
    [Parameter(Mandatory=$true)][int]$Seed,
    [Parameter(Mandatory=$true)][string]$Tag,
    [int]$Wait = 55,
    [hashtable]$Env = @{},
    [string]$Frame = ""
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$out  = Join-Path $root "verify\out"
New-Item -ItemType Directory -Force -Path $out | Out-Null

Remove-Item -Path (Join-Path $root "log\race.log") -ErrorAction SilentlyContinue

# Clear EVERY TD5RE_* knob first. They persist across launches inside one
# PowerShell session, so without this an "isolated" A/B silently measures the
# union of every knob the session ever set. This bit us once already.
Get-ChildItem Env: | Where-Object { $_.Name -like "TD5RE_*" } |
    ForEach-Object { Remove-Item -Path "Env:\$($_.Name)" -ErrorAction SilentlyContinue }

$env:TD5RE_CONTROL_PORT   = "37061"
$env:TD5RE_AUTOTRACK_SEED = "$Seed"
$env:TD5RE_WINDOW_TITLE   = "R8GUARD-$Tag"
if ($Frame -ne "") { $env:TD5RE_FRAMEDUMP = $Frame } else { Remove-Item Env:\TD5RE_FRAMEDUMP -ErrorAction SilentlyContinue }
foreach ($k in $Env.Keys) { Set-Item -Path "Env:\$k" -Value $Env[$k] }

Push-Location $root
$p = Start-Process -FilePath (Join-Path $root "td5re.exe") `
        -ArgumentList "--SkipIntro=1" -PassThru -WorkingDirectory $root
Pop-Location
Write-Output "PID=$($p.Id) seed=$Seed tag=$Tag"

# push it behind everything so it never steals focus
Start-Sleep -Seconds 3
Add-Type @"
using System;using System.Runtime.InteropServices;
public class W {
 [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h,IntPtr a,int x,int y,int cx,int cy,uint f);
 [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h,uint m,IntPtr w,IntPtr l);
}
"@ -ErrorAction SilentlyContinue
$p.Refresh()
if ($p.MainWindowHandle -ne 0) {
    [W]::SetWindowPos($p.MainWindowHandle,[IntPtr]1,0,0,0,0,0x0013) | Out-Null
}

$deadline = (Get-Date).AddSeconds($Wait)
while ((Get-Date) -lt $deadline -and -not $p.HasExited) { Start-Sleep -Milliseconds 500 }

if (-not $p.HasExited) {
    $p.Refresh()
    if ($p.MainWindowHandle -ne 0) { [W]::PostMessage($p.MainWindowHandle,0x0010,[IntPtr]::Zero,[IntPtr]::Zero) | Out-Null }
    $p.WaitForExit(30000) | Out-Null
}
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force; Write-Output "WARN: force-killed PID $($p.Id)" }

Start-Sleep -Seconds 1
foreach ($f in @("race.log","engine.log")) {
    $src = Join-Path $root "log\$f"
    if (Test-Path $src) { Copy-Item $src (Join-Path $out "$Tag-$f") -Force }
}
Write-Output "logs -> $out\$Tag-race.log"
