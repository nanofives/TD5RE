# stall160_run.ps1 -- P1 round 1008b harness: race La Plata (geo slot 61 /
# level091) with an AI field and keep the per-tick trace, so the span ~160
# pile-up can be measured instead of guessed.
#
#   pwsh verify/stall160_run.ps1 -Tag base [-Seed 0x1A2B3C4D] [-RaceSecs 180]
#                                [-Traffic 4] [-Opponents 5] [-PlayerIsAI 0]
#                                [-Extra @{TD5RE_X="1"}]
#
# No control socket (the round forbids it): the run is killed by PID on a hard
# wall clock. RT off + minimum graphics + VSync off, per the standing rule for
# every non-selftest launch (monitors may be off).
param([string]$Tag = "run",
      [string]$Seed = "",            # TD5RE_RACE_SEED override; "" = trace default
      [int]$RaceSecs = 180,
      [int]$GenWait = 900,
      [int]$Traffic = 4,
      [int]$Opponents = 5,
      [int]$PlayerIsAI = 0,
      [int]$StartSpanOffset = 0,
      [int]$CarDamage = 1,
      [int]$Track = 61,
      [string]$Exe = "td5re.exe",
      [string]$FramedumpSpans = "",
      [hashtable]$Extra = @{})

$wt = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)

Get-ChildItem env: | Where-Object { $_.Name -like 'TD5RE_*' } | ForEach-Object { Remove-Item "env:$($_.Name)" }
$env:TD5RE_RT           = "0"
$env:TD5RE_WINDOW_TITLE = "TD5RE stall160 $Tag"
if ($Seed -ne "") { $env:TD5RE_RACE_SEED = $Seed }
if ($FramedumpSpans -ne "") {
    $env:TD5RE_FRAMEDUMP_SPANS  = $FramedumpSpans
    $env:TD5RE_D3D12_CAPTURE    = "1"
}
foreach ($k in $Extra.Keys) { Set-Item "env:$k" $Extra[$k] }   # -Extra wins

$gfx = @("--Lighting=0","--Quality=0","--SunShadows=0","--Reflections=0",
         "--WetRoads=0","--StreetLights=0","--CarLights=0","--LegacyShadows=0",
         "--GIQuality=0","--ShadowRays=0","--ReflectionQuality=0",
         "--CarShadows=0","--VFX=0","--WorldBillboards=0","--FoliageAA=0")

$log = Join-Path $wt "log"
New-Item -ItemType Directory -Force -Path $log | Out-Null
foreach ($f in @("race.log","engine.log","frontend.log",
                 "race_trace_track.csv","race_trace_driver.csv",
                 "race_trace_motion.csv","race_trace_progress.csv")) {
    $p0 = Join-Path $log $f
    for ($t = 0; $t -lt 20 -and (Test-Path $p0); $t++) {
        try { Remove-Item $p0 -Force -ErrorAction Stop } catch { Start-Sleep -Milliseconds 500 }
    }
}

$args = @("--AutoRace=1","--SkipIntro=1","--DefaultTrack=$Track",
          "--DefaultOpponents=$Opponents","--Traffic=$Traffic",
          "--PlayerIsAI=$PlayerIsAI","--AutoThrottle=0","--CarDamage=$CarDamage",
          "--StartSpanOffset=$StartSpanOffset",
          "--Windowed=1","--VSync=0",
          "--RaceTrace=1","--RaceTraceSlot=-1","--RaceTraceMaxSimTicks=0",
          "--RaceTraceMaxFrames=400000") + $gfx

$p = Start-Process -FilePath (Join-Path $wt $Exe) -ArgumentList $args `
                   -WorkingDirectory $wt -PassThru
Write-Host "pid=$($p.Id) tag=$Tag seed=$Seed traffic=$Traffic opp=$Opponents playerAI=$PlayerIsAI"

# Hard wall clock: generation budget + race budget, then kill BY PID.
$deadline = (Get-Date).AddSeconds($GenWait + $RaceSecs)
while (-not $p.HasExited -and (Get-Date) -lt $deadline) { Start-Sleep -Seconds 5 }
if (-not $p.HasExited) {
    Stop-Process -Id $p.Id -Force
    for ($j = 0; $j -lt 20 -and -not $p.HasExited; $j++) { Start-Sleep -Seconds 1 }
}
Write-Host "killed pid=$($p.Id)"

foreach ($f in @("race.log","engine.log",
                 "race_trace_track.csv","race_trace_driver.csv",
                 "race_trace_motion.csv","race_trace_progress.csv")) {
    $src = Join-Path $log $f
    if (Test-Path $src) {
        $dst = Join-Path $log ("s160_{0}_{1}" -f $Tag, $f)
        Copy-Item $src $dst -Force -ErrorAction SilentlyContinue
        Write-Host ("  {0,-26} {1,10} bytes" -f $f, (Get-Item $src).Length)
    } else {
        Write-Host ("  {0,-26} MISSING" -f $f)
    }
}
