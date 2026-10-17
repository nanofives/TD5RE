# f4a_tour.ps1 -- GEO FREE ROAM F4a: framedumps of the roam scenery at one spot, any number of poses.
#   pwsh verify/roam/f4a_tour.ps1 -Tag moreno -Lat -34.9214 -Lon -57.9545 `
#        -Poses "cath:-34.9214,-57.9545,8,230,-8;top:-34.9225,-57.9558,200,0,-89"
# Pose = name:lat,lon,height_above_ground_m,yaw_deg,pitch_deg (yaw 0 = north on the map).
# Boots --Roam=1 spawned at -Lat/-Lon (the stream builds the road + scenery tiles round the camera),
# poses in the roam frame from td5re.exe's TD5RE_ROAM_RENDER_POSES generator. Same launch rules as
# f2c_tour.ps1: windowed, sent to the back without activation, RT off + min graphics, PID kill.
# -Strip also writes <Out>/f4a_<Tag>_strip_poses.txt: the SAME poses in the level091 route frame,
# for verify/geo_tour.ps1 -Poses (the strip geo track side by side).
param([string]$Tag = "spot", [double]$Lat, [double]$Lon, [string]$Poses = "",
      [string]$Out = "", [string]$Extra = "", [int]$Dwell = 240, [int]$Timeout = 300,
      [int]$Width = 1280, [int]$Height = 720, [string]$Exe = "td5re.exe", [switch]$Strip)
$ErrorActionPreference = "Stop"
$wt = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path))
Get-ChildItem env: | Where-Object { $_.Name -like 'TD5RE_*' } | ForEach-Object { Remove-Item "env:$($_.Name)" }
if (-not $Out) { $Out = Join-Path $wt "log" }
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$inv = [Globalization.CultureInfo]::InvariantCulture
$lat = $Lat.ToString($inv); $lon = $Lon.ToString($inv)
$pose = Join-Path $Out "f4a_${Tag}_poses.txt"
$lines = @($Poses -split ';' | Where-Object { $_.Trim() } | ForEach-Object { "${Tag}_" + $_.Trim() })
Set-Content -Path $pose -Encoding ASCII -Value $lines
$names = @($lines | ForEach-Object { $_.Split(':')[0] })
if ($Strip) {
    # route frame (no SPAWN): the numbers verify/geo_tour.ps1 -Poses takes on level091
    $env:TD5RE_ROAM_RENDER_POSES = $pose
    & (Join-Path $wt $Exe) | Out-Null
    Copy-Item "$pose.tour.txt" (Join-Path $Out "f4a_${Tag}_strip_poses.txt") -Force
    Write-Host "strip poses -> $(Join-Path $Out "f4a_${Tag}_strip_poses.txt")"
    Remove-Item env:TD5RE_ROAM_RENDER_POSES
}
$env:TD5RE_ROAM_RENDER_POSES = $pose
$env:TD5RE_ROAM_RENDER_POSES_SPAWN = "${lat},${lon}"
& (Join-Path $wt $Exe) | Out-Null
$tour = "$pose.tour.txt"
if (-not (Test-Path $tour)) { throw "pose generator wrote nothing" }
Get-Content $tour | ForEach-Object { Write-Host "pose $_" }
Remove-Item env:TD5RE_ROAM_RENDER_POSES, env:TD5RE_ROAM_RENDER_POSES_SPAWN
$manifest = Join-Path $Out "f4a_${Tag}_manifest.csv"
foreach ($n in $names) { $q = Join-Path $Out "f4a_$n.png"; if (Test-Path $q) { Remove-Item $q -Force } }
if (Test-Path $manifest) { Remove-Item $manifest -Force }
$env:TD5RE_RT = "0"; $env:TD5RE_D3D12_CAPTURE = "1"; $env:TD5RE_WINDOW_TITLE = "TD5RE f4a $Tag"
$env:TD5RE_FREECAM_TOUR_FAST = "1"; $env:TD5RE_FREECAM_TOUR_NOWAIT = "1"
$env:TD5RE_FREECAM_TOUR_FILE = $tour
$env:TD5RE_FREECAM_TOUR_PATH = (Join-Path $Out "f4a_%s.png").Replace([char]92, [char]47)
$env:TD5RE_FREECAM_TOUR_MANIFEST = $manifest.Replace([char]92, [char]47)
$env:TD5RE_FREECAM_TOUR_DWELL = "$Dwell"
foreach ($kv in ($Extra -split '[|;]')) { $p = $kv.Split('=', 2); if ($p.Count -eq 2 -and $p[0].Trim()) { Set-Item "env:$($p[0].Trim())" $p[1].Trim() } }
$gfx = @("--Lighting=0","--Quality=0","--SunShadows=0","--Reflections=0","--WetRoads=0","--StreetLights=0","--CarLights=0",
         "--LegacyShadows=0","--GIQuality=0","--ShadowRays=0","--ReflectionQuality=0","--CarShadows=0","--VFX=0",
         "--WorldBillboards=0","--FoliageAA=0")
$gameArgs = @("--Roam=1","--RoamSpawn=${lat},${lon}","--SkipIntro=1","--CarDamage=0","--Windowed=1","--VSync=0",
              "--Width=$Width","--Height=$Height") + $gfx
foreach ($f in @("race.log","engine.log","frontend.log")) { $p0 = Join-Path $wt "log\$f"; if (Test-Path $p0) { try { Remove-Item $p0 -Force } catch { } } }
$sw = [Diagnostics.Stopwatch]::StartNew()
$p = Start-Process -FilePath (Join-Path $wt $Exe) -ArgumentList $gameArgs -WorkingDirectory $wt -PassThru
Write-Host "pid=$($p.Id) tag=$Tag"
try { Add-Type -Name W6 -Namespace N6 -MemberDefinition '[DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr a, int x, int y, int cx, int cy, uint f);' -ErrorAction Stop } catch { }
$sent = $false
while (-not $p.HasExited -and $sw.Elapsed.TotalSeconds -lt $Timeout) {
    Start-Sleep -Milliseconds 500
    if (-not $sent) { $p.Refresh(); if ($p.MainWindowHandle -ne 0) { try { [void][N6.W6]::SetWindowPos($p.MainWindowHandle, [IntPtr]1, 0,0,0,0, 0x0013) } catch { }; $sent = $true } }
}
$killed = $false
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force; $killed = $true; Start-Sleep -Seconds 2 }
Write-Host ("game {0} after {1} s" -f $(if ($killed) { "KILLED at timeout" } else { "exited cleanly" }), [int]$sw.Elapsed.TotalSeconds)
foreach ($f in @("engine.log","race.log")) { $src = Join-Path $wt "log\$f"; if (Test-Path $src) { Copy-Item $src (Join-Path $Out "f4a_${Tag}_$f") -Force } }
foreach ($n in $names) {
    $q = Join-Path $Out "f4a_$n.png"
    Write-Host ("  {0}: {1}" -f $n, $(if (Test-Path $q) { "$((Get-Item $q).Length) bytes" } else { "MISSING" }))
}
