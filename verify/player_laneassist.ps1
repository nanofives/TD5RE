# player_laneassist.ps1 -- [1019 V] a HUMAN-slot drive (PlayerIsAI=0, AutoThrottle, LaneAssist) alone on the geo route, with the walker audit on:
# the scenario of docs/plans/AUTOTRACK_BRANCH_REWORK.md (a lane-assisted player through a LEFT fork). 0 opponents, so every [WALKAUD] tick is the player's.
#   pwsh verify/player_laneassist.ps1 -Tag t1 [-Env "NAME=VALUE|NAME=VALUE"] [-Ticks 3500]
param([string]$Tag = "run", [string]$Exe = "td5re.exe", [string]$Env = "", [int]$Ticks = 3500, [int]$GenWait = 600, [string]$GeoPlace = "la_plata_partido", [int]$Track = 61)
$wt = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
Get-ChildItem env: | Where-Object { $_.Name -like 'TD5RE_*' } | ForEach-Object { Remove-Item "env:$($_.Name)" }
$env:TD5RE_GEO_PLACE = $GeoPlace; $env:TD5RE_AUTOTRACK_REUSE = "0"; $env:TD5RE_RT = "0"; $env:TD5RE_RACE_SEED = "11"
$env:TD5RE_WALKER_AUDIT = "1"; $env:TD5RE_TG_DOUBLE_BUILD = "0"; $env:TD5RE_WINDOW_TITLE = "TD5RE player $Tag"
if ($Env -ne "") { foreach ($kv in $Env.Split('|')) { $p = $kv.Split('=', 2); if ($p.Count -eq 2 -and $p[0].Trim() -ne "") { Set-Item "env:$($p[0].Trim())" $p[1] } } }
$gfx = @("--Lighting=0","--Quality=0","--SunShadows=0","--Reflections=0","--WetRoads=0","--StreetLights=0","--CarLights=0","--LegacyShadows=0",
         "--GIQuality=0","--ShadowRays=0","--ReflectionQuality=0","--CarShadows=0","--VFX=0","--WorldBillboards=0","--FoliageAA=0")
$log = Join-Path $wt "log"
foreach ($f in @("race.log","engine.log","frontend.log")) { Remove-Item (Join-Path $log $f) -Force -ErrorAction SilentlyContinue }
Get-ChildItem $log -Filter "race_trace_*.csv" -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue
$lvl = Join-Path $wt ("re\assets\levels\level{0:D3}" -f ($Track + 30)); if (Test-Path $lvl) { Remove-Item $lvl -Recurse -Force }
$a = @("--AutoRace=1","--SkipIntro=1","--DefaultTrack=$Track","--DefaultOpponents=0","--Traffic=0","--PlayerIsAI=0","--AutoThrottle=1","--LaneAssist=1",
       "--CarDamage=0","--Windowed=1","--VSync=0","--SFXVolume=0","--MusicVolume=0","--RaceTrace=1","--RaceTraceSlot=0","--RaceTraceMaxSimTicks=$Ticks",
       "--RaceTraceMaxFrames=4000000","--TraceModules=pose,track","--TraceStages=post_physics,post_track","--TraceFastForward=4") + $gfx
$p = Start-Process -FilePath (Join-Path $wt $Exe) -ArgumentList $a -WorkingDirectory $wt -PassThru
$deadline = (Get-Date).AddSeconds($GenWait + 300)
while (-not $p.HasExited -and (Get-Date) -lt $deadline) { Start-Sleep -Seconds 3 }
if (-not $p.HasExited) { [void]$p.CloseMainWindow(); Start-Sleep -Seconds 5; if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force } }
$out = Join-Path $wt "verify\out\player_$Tag"; New-Item -ItemType Directory -Force -Path $out | Out-Null
Copy-Item (Join-Path $log "race.log") $out -Force -ErrorAction SilentlyContinue
Get-ChildItem $log -Filter "race_trace_*.csv" | Copy-Item -Destination $out -Force
Write-Host "wrote $out"
