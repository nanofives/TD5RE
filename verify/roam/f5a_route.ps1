# f5a_route.ps1 -- GEO FREE ROAM F5-A: run one InputScript-driven route-planner scenario in roam.
#   pwsh verify/roam/f5a_route.ps1 -Script inputscripts/roam_route_f5a_plan.txt -Tag plan [-Lat -34.9214 -Lon -57.9545]
#                                  [-Extra "TD5RE_X=1|TD5RE_Y=2"] [-Timeout 240] [-Drive 1] [-DriveSecs 300]
# Boots --Roam=1 at the spot, optional street autopilot (TD5RE_ROAM_DRIVE=street), runs the InputScript
# (the script ends with `quit`). Windowed, no focus steal, RT off + min graphics, PID-scoped kill.
# Output: log\*.log (cleared first), log\roam_route_*.txt and log\roam_route_shot_NN.png from the run,
# copied to log\f5a_<Tag>\ at the end.
param([Parameter(Mandatory)][string]$Script, [string]$Tag = "run",
      [double]$Lat = -34.9214, [double]$Lon = -57.9545, [string]$Extra = "",
      [int]$Timeout = 240, [int]$Drive = 1, [int]$DriveSecs = 300, [int]$Width = 1280, [int]$Height = 720,
      [string]$Exe = "td5re.exe")
$ErrorActionPreference = "Stop"
$wt = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path))
Get-ChildItem env: | Where-Object { $_.Name -like 'TD5RE_*' } | ForEach-Object { Remove-Item "env:$($_.Name)" }
$logd = Join-Path $wt "log"
New-Item -ItemType Directory -Force -Path $logd | Out-Null
Get-ChildItem (Join-Path $logd "*") -Include "roam_route_*", "*.log" -File -ErrorAction SilentlyContinue | ForEach-Object { try { Remove-Item $_.FullName -Force } catch { } }
$env:TD5RE_RT = "0"; $env:TD5RE_D3D12_CAPTURE = "1"; $env:TD5RE_WINDOW_TITLE = "TD5RE f5a $Tag"
if ($Drive -ne 0) { $env:TD5RE_ROAM_DRIVE = "street"; $env:TD5RE_ROAM_DRIVE_QUIT = "0"; $env:TD5RE_ROAM_DRIVE_SECS = "$DriveSecs" }
foreach ($kv in ($Extra -split '[|;]')) { $p = $kv.Split('=', 2); if ($p.Count -eq 2 -and $p[0].Trim()) { Set-Item "env:$($p[0].Trim())" $p[1].Trim() } }
$lat = $Lat.ToString([Globalization.CultureInfo]::InvariantCulture); $lon = $Lon.ToString([Globalization.CultureInfo]::InvariantCulture)
$gfx = @("--Lighting=0","--Quality=0","--SunShadows=0","--Reflections=0","--WetRoads=0","--StreetLights=0","--CarLights=0",
         "--LegacyShadows=0","--GIQuality=0","--ShadowRays=0","--ReflectionQuality=0","--CarShadows=0","--VFX=0",
         "--WorldBillboards=0","--FoliageAA=0")
$gameArgs = @("--Roam=1","--RoamSpawn=${lat},${lon}","--SkipIntro=1","--CarDamage=0","--Windowed=1","--VSync=0",
              "--Width=$Width","--Height=$Height","--InputScript=$Script") + $gfx
$sw = [Diagnostics.Stopwatch]::StartNew()
$p = Start-Process -FilePath (Join-Path $wt $Exe) -ArgumentList $gameArgs -WorkingDirectory $wt -PassThru
Write-Host "pid=$($p.Id) tag=$Tag script=$Script"
try { Add-Type -Name W5 -Namespace N5 -MemberDefinition '[DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr a, int x, int y, int cx, int cy, uint f);' -ErrorAction Stop } catch { }
$sent = $false
while (-not $p.HasExited -and $sw.Elapsed.TotalSeconds -lt $Timeout) {
    Start-Sleep -Milliseconds 500
    if (-not $sent) { $p.Refresh(); if ($p.MainWindowHandle -ne 0) { try { [void][N5.W5]::SetWindowPos($p.MainWindowHandle, [IntPtr]1, 0,0,0,0, 0x0013) } catch { }; $sent = $true } }
}
$killed = $false
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force; $killed = $true; Start-Sleep -Seconds 2 }
Write-Host ("game {0} after {1} s" -f $(if ($killed) { "KILLED at timeout" } else { "exited cleanly" }), [int]$sw.Elapsed.TotalSeconds)
$dst = Join-Path $logd "f5a_$Tag"
New-Item -ItemType Directory -Force -Path $dst | Out-Null
Get-ChildItem $logd -File | Where-Object { $_.Name -like 'roam_route_*' -or $_.Name -like '*.log' } | ForEach-Object { Copy-Item $_.FullName (Join-Path $dst $_.Name) -Force }
Write-Host "copied to $dst"
