# xspan_run.ps1 -- crossing-safe span localisation (GEO TRACK Option B) probe.
#
# Builds the auto track from a conditioned ROUTE.JSON that DELIBERATELY crosses
# itself, races it with slot 0 AI-driven so the car actually drives the whole
# route, then keeps the per-tick span trace so the crossing can be inspected.
#
#   pwsh verify/xspan_run.ps1 -Route <path to ROUTE.JSON> -Tag base -Port 37184
#                             [-Exe td5re_base.exe] [-RaceSecs 180]
#                             [-Extra @{TD5RE_XSPAN="0"}]
#
# The car under test is an AI RACER, not slot 0, and the trace covers every
# slot (--RaceTraceSlot=-1). Two reasons. A parked or full-throttle slot 0
# never reaches the crossing, so it proves nothing; and --PlayerIsAI=1, which
# would drive slot 0 properly, was measured to stop the track/pose/motion trace
# modules emitting any row at all (frame/progress/rotation keep writing), so it
# cannot be the harness for a span-continuity measurement. The AI racers drive
# the conditioned centreline, which is exactly the case under test.
#
# RT off + minimum graphics, per the standing rule for every non-selftest run.
param([string]$Route = "", [string]$Tag = "run", [int]$Port = 37184,
      [string]$Exe = "td5re.exe", [string]$Seed = "20260901",
      [int]$RaceSecs = 180, [int]$GenWait = 600, [hashtable]$Extra = @{},
      [switch]$Keep)

$wt = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
if ($Route -eq "") { throw "-Route is required (path to a conditioned ROUTE.JSON)" }
if (-not (Test-Path $Route)) { throw "route not found: $Route" }

Get-ChildItem env: | Where-Object { $_.Name -like 'TD5RE_*' } | ForEach-Object { Remove-Item "env:$($_.Name)" }
$env:TD5RE_AUTOTRACK_SEED   = $Seed
$env:TD5RE_AUTOTRACK_STREAM = "0"
$env:TD5RE_AUTOTRACK_REUSE  = "0"
$env:TD5RE_CONTROL_PORT     = "$Port"
$env:TD5RE_WINDOW_TITLE     = "TD5RE xspan $Tag"
$env:TD5RE_RT               = "0"
$env:TD5RE_GEO_ROUTE        = (Resolve-Path $Route).Path
foreach ($k in $Extra.Keys) { Set-Item "env:$k" $Extra[$k] }   # -Extra wins

$gfx = @("--Lighting=0","--Quality=0","--SunShadows=0","--Reflections=0",
         "--WetRoads=0","--StreetLights=0","--CarLights=0","--LegacyShadows=0",
         "--GIQuality=0","--ShadowRays=0","--ReflectionQuality=0",
         "--CarShadows=0","--VFX=0","--WorldBillboards=0","--FoliageAA=0",
         "--RenderScale=50")

$lvl = Join-Path $wt "re\assets\levels\level090"
if ((Test-Path $lvl) -and -not $Keep) { Remove-Item $lvl -Recurse -Force }
foreach ($f in @("race.log","race_trace_track.csv")) {
    $p0 = Join-Path $wt "log\$f"
    for ($t = 0; $t -lt 20 -and (Test-Path $p0); $t++) {
        try { Remove-Item $p0 -Force -ErrorAction Stop } catch { Start-Sleep -Milliseconds 500 }
    }
}

$p = Start-Process -FilePath (Join-Path $wt $Exe) `
      -ArgumentList (@("--AutoRace=1","--SkipIntro=1","--Control=1","--DefaultTrack=60",
                       "--RaceTrace=1","--RaceTraceSlot=-1","--RaceTraceMaxSimTicks=0") + $gfx) `
      -WorkingDirectory $wt -PassThru
Write-Host "pid=$($p.Id) exe=$Exe tag=$Tag port=$Port route=$($env:TD5RE_GEO_ROUTE)"

$models = Join-Path $lvl "MODELS.DAT"
$last = -1; $stable = 0; $done = $false
for ($i = 0; $i -lt $GenWait; $i++) {
    Start-Sleep -Seconds 1
    if ($p.HasExited) { break }
    if (Test-Path $models) {
        $len = (Get-Item $models).Length
        if ($len -gt 0 -and $len -eq $last) { $stable++ } else { $stable = 0 }
        $last = $len
        if ($stable -ge 4) { $done = $true; break }
    }
}
Write-Host "generated=$done after ${i}s models=$last"
if (-not $p.HasExited) { Start-Sleep -Seconds $RaceSecs }

if (-not $p.HasExited) {
    $u = New-Object System.Net.Sockets.UdpClient
    $ep = New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Loopback, $Port)
    try { $b = [Text.Encoding]::ASCII.GetBytes("quit"); [void]$u.Send($b, $b.Length, $ep) } catch { }
    $u.Close()
    for ($j = 0; $j -lt 30 -and -not $p.HasExited; $j++) { Start-Sleep -Seconds 1 }
    if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force; Start-Sleep -Seconds 2 }
}

Copy-Item (Join-Path $wt "log\race.log") (Join-Path $wt "log\xspan_$Tag.log") -Force -ErrorAction SilentlyContinue
Copy-Item (Join-Path $wt "log\race_trace_track.csv") (Join-Path $wt "log\xspan_track_$Tag.csv") -Force -ErrorAction SilentlyContinue
Write-Host "### $Tag"
$lg = Join-Path $wt "log\xspan_$Tag.log"
if (Test-Path $lg) {
    Select-String -Path $lg -Pattern "\[XSPAN\]|walker_nonconverge|OOB RESCUE|branch_return" |
        Select-Object -First 40 | ForEach-Object { Write-Host ("  " + $_.Line) }
}
foreach ($f in @("STRIP.DAT","MODELS.DAT","GENSTAMP.TXT")) {
    $p2 = Join-Path $lvl $f
    if (Test-Path $p2) {
        $h = (Get-FileHash $p2 -Algorithm SHA256).Hash.Substring(0,16)
        Write-Host ("  {0,-13} {1,10} bytes  {2}" -f $f, (Get-Item $p2).Length, $h)
    }
}
