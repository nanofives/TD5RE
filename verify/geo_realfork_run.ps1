# geo_realfork_run.ps1 -- round 1013 F2 harness: race Mariano's La Plata route
# (geo slot 61 / level091) with an all-AI field and keep the per-tick trace, so
# the driveable forks can be MEASURED (fork entries per fork, stalls, contacts)
# instead of looked at.
#
#   pwsh verify/geo_realfork_run.ps1 -Tag after
#   pwsh verify/geo_realfork_run.ps1 -Tag before -Exe td5re_base.exe
#   pwsh verify/geo_realfork_run.ps1 -Tag gen -GenOnly 1        (log only, no race)
#   pwsh verify/geo_realfork_run.ps1 -Tag nofork -Extra "TD5RE_GEO_REAL_FORKS=0"
#
# Standing launch rules, all applied: --Windowed=1, --VSync=0, RT off and minimum
# graphics, car damage OFF (damage ON freezes racers like a stall), no sockets,
# every TD5RE_* cleared first, a hard wall-clock kill BY PID.
param([string]$Tag = "run",
      [string]$Exe = "td5re.exe",
      [int]$GenOnly = 0,
      [int]$RaceSecs = 420,
      [int]$GenWait = 900,
      [int]$Traffic = 0,
      [int]$Opponents = 5,
      [int]$PlayerIsAI = 1,
      [int]$MaxSimTicks = 7000,
      [string]$Modules = "track,driver,motion,progress",   # add "pose" for world x/z/yaw
      [int]$Track = 61,
      [string]$GeoPlace = "la_plata",      # "" = a synthetic track (-Track 60)
      [string]$Seed = "",
      [string]$FramedumpSpans = "",
      [string]$FramedumpPath = "",
      [int]$AutoThrottle = 0,
      [double]$FastForward = 4.0,
      [int]$IdleStop = 40,                 # [R1016 K] seconds of a silent trace that end the run (0 = off)
      # "NAME=VALUE,NAME=VALUE" extra env, NOT a hashtable (pwsh -File stringifies).
      [string]$Extra = "")

$wt = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
if ($GeoPlace -eq "none") { $GeoPlace = "" }   # [R1016 K] "none" = a synthetic track (-Track 60)

Get-ChildItem env: | Where-Object { $_.Name -like 'TD5RE_*' } | ForEach-Object { Remove-Item "env:$($_.Name)" }
if ($GeoPlace -ne "") { $env:TD5RE_GEO_PLACE = $GeoPlace }
$env:TD5RE_AUTOTRACK_REUSE = "0"
$env:TD5RE_RT              = "0"
$env:TD5RE_WINDOW_TITLE    = "TD5RE realfork $Tag"
if ($Seed -ne "") { $env:TD5RE_RACE_SEED = $Seed }
if ($FramedumpSpans -ne "") {
    $env:TD5RE_FRAMEDUMP_SPANS = $FramedumpSpans
    $env:TD5RE_D3D12_CAPTURE   = "1"
    if ($FramedumpPath -eq "") { $FramedumpPath = "log/rf_${Tag}_span_%d.png" }
    $env:TD5RE_FRAMEDUMP_SPAN_PATH = $FramedumpPath
}
if ($Extra -ne "") {
    foreach ($kv in $Extra.Split(',')) {
        $p = $kv.Split('=', 2)
        if ($p.Count -eq 2 -and $p[0].Trim() -ne "") { Set-Item "env:$($p[0].Trim())" $p[1] }
    }
}

$gfx = @("--Lighting=0","--Quality=0","--SunShadows=0","--Reflections=0",
         "--WetRoads=0","--StreetLights=0","--CarLights=0","--LegacyShadows=0",
         "--GIQuality=0","--ShadowRays=0","--ReflectionQuality=0",
         "--CarShadows=0","--VFX=0","--WorldBillboards=0","--FoliageAA=0")

$log = Join-Path $wt "log"
New-Item -ItemType Directory -Force -Path $log | Out-Null
$keep = @("race.log","engine.log","frontend.log","race_trace_track.csv",
          "race_trace_driver.csv","race_trace_motion.csv","race_trace_progress.csv",
          "race_trace_pose.csv")
foreach ($f in $keep) {
    $p0 = Join-Path $log $f
    for ($t = 0; $t -lt 20 -and (Test-Path $p0); $t++) {
        try { Remove-Item $p0 -Force -ErrorAction Stop } catch { Start-Sleep -Milliseconds 500 }
    }
}
$lvl = Join-Path $wt ("re\assets\levels\level{0:D3}" -f ($Track + 30))
if (Test-Path $lvl) { Remove-Item $lvl -Recurse -Force }

$traceArgs = @()
if (-not $GenOnly) {
    $traceArgs = @("--RaceTrace=1","--RaceTraceSlot=-1","--RaceTraceMaxSimTicks=$MaxSimTicks",
                   "--RaceTraceMaxFrames=400000","--TraceModules=$Modules",
                   "--TraceStages=post_track,post_ai,post_physics,post_progress")
    if ($FastForward -gt 1.0) { $traceArgs += "--TraceFastForward=$FastForward" }
}
$args = @("--AutoRace=1","--SkipIntro=1","--DefaultTrack=$Track",
          "--DefaultOpponents=$Opponents","--Traffic=$Traffic","--Difficulty=1",
          "--PlayerIsAI=$PlayerIsAI","--AutoThrottle=$AutoThrottle","--CarDamage=0",
          "--Windowed=1","--VSync=0") + $traceArgs + $gfx

$p = Start-Process -FilePath (Join-Path $wt $Exe) -ArgumentList $args `
                   -WorkingDirectory $wt -PassThru
Write-Host "pid=$($p.Id) tag=$Tag exe=$Exe genonly=$GenOnly traffic=$Traffic opp=$Opponents playerAI=$PlayerIsAI"

# Send the window to the back: a long race must not sit over the user's work.
Start-Sleep -Seconds 2
try {
    Add-Type -Name W3 -Namespace N3 -MemberDefinition '
      [DllImport("user32.dll")] public static extern bool SetWindowPos(
        IntPtr h, IntPtr a, int x, int y, int cx, int cy, uint f);' -ErrorAction Stop
    if ($p.MainWindowHandle -ne 0) {
        [void][N3.W3]::SetWindowPos($p.MainWindowHandle, [IntPtr]1, 0,0,0,0, 0x0013)
    }
} catch { }

if ($GenOnly) {
    # Wait for the generate (MODELS.DAT present and stable), then stop.
    $models = Join-Path $lvl "MODELS.DAT"
    $last = -1; $stable = 0
    for ($i = 0; $i -lt $GenWait; $i++) {
        Start-Sleep -Seconds 1
        if ($p.HasExited) { break }
        if (Test-Path $models) {
            $len = (Get-Item $models).Length
            if ($len -gt 0 -and $len -eq $last) { $stable++ } else { $stable = 0 }
            $last = $len
            if ($stable -ge 6) { break }
        }
    }
    Write-Host "models settled after ${i}s"
} else {
    $deadline = (Get-Date).AddSeconds($GenWait + $RaceSecs)
    $want = @()
    if ($FramedumpSpans -ne "") { $want = ($FramedumpSpans -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ }) }
    # [R1016 K] -IdleStop N: leave as soon as the track trace has stopped growing for
    # N seconds (the sim ended at MaxSimTicks or the race is over and the process
    # idles). Without it every run waits out the whole GenWait+RaceSecs deadline.
    $trk = Join-Path $log "race_trace_track.csv"
    $lastLen = -1; $lastGrow = Get-Date
    while (-not $p.HasExited -and (Get-Date) -lt $deadline) {
        Start-Sleep -Seconds 5
        if ($IdleStop -gt 0 -and (Test-Path $trk)) {
            $len = (Get-Item $trk).Length
            if ($len -ne $lastLen) { $lastLen = $len; $lastGrow = Get-Date }
            elseif ($len -gt 1000000 -and ((Get-Date) - $lastGrow).TotalSeconds -gt $IdleStop) {
                Write-Host "trace idle for ${IdleStop}s -> stopping"; break
            }
        }
        if ($want.Count -gt 0) {
            $have = @($want | Where-Object { Test-Path (Join-Path $wt ($FramedumpPath -replace '%d', $_)) })
            if ($have.Count -eq $want.Count) { Write-Host "all frames captured"; break }
        }
    }
}
# PID-scoped, never a name-wide kill: parallel sessions share this machine.
if (-not $p.HasExited) {
    Stop-Process -Id $p.Id -Force
    for ($j = 0; $j -lt 20 -and -not $p.HasExited; $j++) { Start-Sleep -Seconds 1 }
}
Write-Host "stopped pid=$($p.Id) exit=$($p.ExitCode)"

foreach ($f in $keep) {
    $src = Join-Path $log $f
    if (Test-Path $src) {
        $dst = Join-Path $log ("rf_{0}_{1}" -f $Tag, $f)
        Copy-Item $src $dst -Force -ErrorAction SilentlyContinue
        Write-Host ("  {0,-26} {1,10} bytes" -f $f, (Get-Item $src).Length)
    }
}
if (Test-Path $lvl) {
    foreach ($f in @("STRIP.DAT","MODELS.DAT")) {
        $q = Join-Path $lvl $f
        if (Test-Path $q) { Write-Host ("  {0,-26} {1,10} bytes" -f $f, (Get-Item $q).Length) }
    }
}
