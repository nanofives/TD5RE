# shipped_trace.ps1 -- [1019 V] a deterministic AI race on a SHIPPED track with the per-tick pose/track/driver trace, for the
# "shipped tracks are bit-identical" gate: run it with the reference exe and with the candidate exe and diff the CSVs
# (python verify/shipped_trace_cmp.py a.dir b.dir). Windowed, RT off, min graphics, no sockets, every TD5RE_* cleared.
param([string]$Exe = "td5re.exe", [string]$Tag = "run", [int]$Track = 0, [int]$Ticks = 1500, [int]$Seed = 12345, [string]$Extra = "")
$wt = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
Get-ChildItem env: | Where-Object { $_.Name -like 'TD5RE_*' } | ForEach-Object { Remove-Item "env:$($_.Name)" }
$env:TD5RE_RT = "0"; $env:TD5RE_RACE_SEED = "$Seed"; $env:TD5RE_WINDOW_TITLE = "TD5RE shipped $Tag"
foreach ($kv in ($Extra -split '[|;]')) {   # [1027 N] knobs via -Extra "A=1|B=2" (the clear above removed every inherited TD5RE_*)
    $q = $kv.Split('=', 2)
    if ($q.Count -eq 2 -and $q[0].Trim() -ne "") { $n = $q[0].Trim(); if ($n -notlike 'TD5RE_*') { $n = "TD5RE_$n" }; Set-Item "env:$n" $q[1].Trim() }
}
$gfx = @("--Lighting=0","--Quality=0","--SunShadows=0","--Reflections=0","--WetRoads=0","--StreetLights=0","--CarLights=0",
         "--LegacyShadows=0","--GIQuality=0","--ShadowRays=0","--ReflectionQuality=0","--CarShadows=0","--VFX=0","--WorldBillboards=0","--FoliageAA=0")
$log = Join-Path $wt "log"
Get-ChildItem $log -Filter "race_trace_*.csv" -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue
$a = @("--AutoRace=1","--SkipIntro=1","--DefaultTrack=$Track","--DefaultOpponents=5","--Traffic=0","--PlayerIsAI=1","--AutoThrottle=0",
       "--CarDamage=0","--Windowed=1","--VSync=0","--SFXVolume=0","--MusicVolume=0","--RaceTrace=1","--RaceTraceSlot=-1",
       "--RaceTraceMaxSimTicks=$Ticks","--RaceTraceMaxFrames=4000000","--TraceModules=pose,motion,track,driver",
       "--TraceStages=post_physics,post_ai,post_track","--TraceFastForward=8") + $gfx
$p = Start-Process -FilePath (Join-Path $wt $Exe) -ArgumentList $a -WorkingDirectory $wt -PassThru
$deadline = (Get-Date).AddSeconds(300)
while (-not $p.HasExited -and (Get-Date) -lt $deadline) { Start-Sleep -Seconds 2 }
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
$out = Join-Path $wt "verify\out\shipped_${Tag}_t$Track"
New-Item -ItemType Directory -Force -Path $out | Out-Null
Get-ChildItem $log -Filter "race_trace_*.csv" | Copy-Item -Destination $out -Force
Write-Host "wrote $out  ($((Get-ChildItem $out).Count) files)"
