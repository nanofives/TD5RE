# xspan_leg.ps1 -- drive ONE leg of a self-crossing geo route through its
# crossing, and keep the evidence.
#
# [OPTION B 2026-09-30] xspan_run.ps1 races the whole route from the start line
# and asks "did anything jump". That is the right smoke test and the wrong
# measurement for a GRADE SEPARATION, for two reasons:
#
#   * the route is 650+ spans, so a run long enough to reach the SECOND leg is
#     long enough for an AI racer to have crashed, spun or been rubber-banded
#     somewhere else first, and then the second leg is never tested at all;
#   * the two legs have to be tested SEPARATELY. The claim is "a car on the
#     deck stays on the deck AND a car underneath stays underneath", which is
#     two runs with two different hints, not one run that happens to pass both.
#
# So this starts the grid a short way BEFORE one leg's crossing
# (--StartSpanOffset) and races just long enough to drive through it.
#
#   pwsh verify/xspan_leg.ps1 -Route re/tools/geo_fixtures/figure8_ROUTE.json `
#        -StartSpan 480 -Legs "131,135,560,569" -Tag over -Port 37192
#
# -Dump <file.png> adds a frame capture; -TopDown <alt> / -Oblique frame the
# crossing from above so the deck and the road under it are both in shot.
#
# RT off + minimum graphics, per the standing rule for every non-selftest run.
param([string]$Route = "", [int]$StartSpan = 0, [string]$Legs = "",
      [string]$Tag = "leg", [int]$Port = 37192, [string]$Exe = "td5re.exe",
      [string]$Seed = "20260901", [int]$RaceSecs = 70, [int]$GenWait = 600,
      [string]$Dump = "", [double]$TopDown = 0, [switch]$Oblique,
      [int]$RenderScale = 50, [int]$Width = 0, [int]$Height = 0,
      [hashtable]$Extra = @{})

$wt = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
if ($Route -eq "") { throw "-Route is required (path to a conditioned ROUTE.JSON)" }
if (-not (Test-Path $Route)) { throw "route not found: $Route" }

Get-ChildItem env: | Where-Object { $_.Name -like 'TD5RE_*' } | ForEach-Object { Remove-Item "env:$($_.Name)" }
$env:TD5RE_AUTOTRACK_SEED   = $Seed
$env:TD5RE_AUTOTRACK_STREAM = "0"
$env:TD5RE_AUTOTRACK_REUSE  = "0"
$env:TD5RE_CONTROL_PORT     = "$Port"
$env:TD5RE_WINDOW_TITLE     = "TD5RE xspan-leg $Tag"
$env:TD5RE_RT               = "0"
$env:TD5RE_GEO_ROUTE        = (Resolve-Path $Route).Path
if ($Dump -ne "")   { $env:TD5RE_FRAMEDUMP = $Dump }
if ($TopDown -gt 0) {
    $env:TD5RE_CAM_TOPDOWN = "$TopDown"
    # BACKDIV=1 backs the camera off by the altitude itself, which turns the
    # plan view into an OBLIQUE one -- the only view in which a deck and the
    # road under it are both visible at once (straight down, the deck hides it).
    if ($Oblique) { $env:TD5RE_CAM_TOPDOWN_BACKDIV = "1" }
}
foreach ($k in $Extra.Keys) { Set-Item "env:$k" $Extra[$k] }   # -Extra wins

$gfx = @("--Lighting=0","--Quality=0","--SunShadows=0","--Reflections=0",
         "--WetRoads=0","--StreetLights=0","--CarLights=0","--LegacyShadows=0",
         "--GIQuality=0","--ShadowRays=0","--ReflectionQuality=0",
         "--CarShadows=0","--VFX=0","--WorldBillboards=0","--FoliageAA=0",
         "--RenderScale=$RenderScale")
# RT stays off and every quality flag stays at 0 -- the standing rule is about
# LOAD, and this is the one knob that only buys pixels. A framedump meant to
# show 3 m of clearance at 320x240 shows a grey smudge, so a visual check gets
# -RenderScale 100 and a window size; a driving check keeps the default.
if ($Width  -gt 0) { $gfx += "--Width=$Width" }
if ($Height -gt 0) { $gfx += "--Height=$Height" }

$lvl = Join-Path $wt "re\assets\levels\level090"
if (Test-Path $lvl) { Remove-Item $lvl -Recurse -Force }
foreach ($f in @("race.log","race_trace_track.csv")) {
    $p0 = Join-Path $wt "log\$f"
    for ($t = 0; $t -lt 20 -and (Test-Path $p0); $t++) {
        try { Remove-Item $p0 -Force -ErrorAction Stop } catch { Start-Sleep -Milliseconds 500 }
    }
}

$p = Start-Process -FilePath (Join-Path $wt $Exe) `
      -ArgumentList (@("--AutoRace=1","--SkipIntro=1","--Control=1","--DefaultTrack=60",
                       "--StartSpanOffset=$StartSpan",
                       "--RaceTrace=1","--RaceTraceSlot=-1","--RaceTraceMaxSimTicks=0") + $gfx) `
      -WorkingDirectory $wt -PassThru
Write-Host "pid=$($p.Id) exe=$Exe tag=$Tag port=$Port startspan=$StartSpan"

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

Copy-Item (Join-Path $wt "log\race.log") (Join-Path $wt "log\xspanleg_$Tag.log") -Force -ErrorAction SilentlyContinue
Copy-Item (Join-Path $wt "log\race_trace_track.csv") (Join-Path $wt "log\xspanleg_track_$Tag.csv") -Force -ErrorAction SilentlyContinue

Write-Host "### $Tag"
$lg = Join-Path $wt "log\xspanleg_$Tag.log"
if (Test-Path $lg) {
    # RESPAWN and OOB RESCUE are part of the claim, not decoration: "spans
    # continuous" is only evidence if nothing teleported the car to make it so.
    Select-String -Path $lg -Pattern "\[XSPAN\]|\[GEO XSEP\]|respawn|Respawn|OOB RESCUE|walker_nonconverge" |
        Select-Object -First 40 | ForEach-Object { Write-Host ("  " + $_.Line) }
}
if ($Dump -ne "") {
    # STAT the file rather than trusting the log line -- a framedump path that
    # was never written still reads as configured.
    $dp = Join-Path $wt $Dump
    if (Test-Path $dp) {
        $fi = Get-Item $dp
        Write-Host ("  framedump    {0} bytes  {1}" -f $fi.Length, $fi.LastWriteTime)
    } else {
        Write-Host "  framedump    MISSING at $dp"
    }
}
$csv = Join-Path $wt "log\xspanleg_track_$Tag.csv"
if ((Test-Path $csv) -and $Legs -ne "") {
    & python (Join-Path $wt "verify\xspan_spans.py") $csv --legs $Legs
}
