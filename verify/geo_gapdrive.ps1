# geo_gapdrive.ps1 -- drive the player car ACROSS a real median gap of a geo route (round 1023 D).
#
#   pwsh verify/geo_gapdrive.ps1 -Tag after                    # every gap run listed in log/race.log
#   pwsh verify/geo_gapdrive.ps1 -Tag after -Runs 0,4,11       # some of them
#   pwsh verify/geo_gapdrive.ps1 -Tag before -Extra @{TD5RE_GEO_FORK_GAP_ENGINE="0"}   # the rails stay (old behaviour)
#
# One launch per gap run. The player (PlayerIsAI=0) is steered by td5_track_dev_gap_drive_bits (dev builds,
# TD5RE_GAP_DRIVE=<run>): pure pursuit to the main road's outer lane, then across the paved opening to the
# corridor lane, at TD5RE_GAP_DRIVE_SPEED units/tick, through the real physics + walker + wall resolver. The game
# logs one [GAPDRIVE] line per tick near the gap and a RESULT line, then quits cleanly. Needs a race.log from a
# launch of the SAME exe on this place (geo_tour.ps1 makes one) to learn the runs ("median gap: run k main a..b").
# Knobs only through -Extra (every inherited TD5RE_* is cleared first).
param([string]$Tag = "gd",
      [string]$Runs = "",
      [object]$Extra = @{},
      [string]$Exe = "td5re.exe",
      [string]$GeoPlace = "",
      [int]$Track = 61,
      [int]$Lead = 18,            # spans before the gap the car starts
      [int]$GridAhead = 15,       # the grid puts the car this many spans past StartSpanOffset
      [int]$Speed = 110,          # units per tick (110 = 28 km/h)
      [int]$Width = 640,
      [int]$Height = 360,
      [int]$Timeout = 180)

$ErrorActionPreference = "Stop"
$wt = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
Get-ChildItem env: | Where-Object { $_.Name -like 'TD5RE_*' } | ForEach-Object { Remove-Item "env:$($_.Name)" }
if (-not $GeoPlace) {
    $sel = Join-Path $wt "re\assets\geo\SELECTED.TXT"
    $GeoPlace = if (Test-Path $sel) { (Get-Content -Raw $sel).Trim() } else { "la_plata_partido" }
}
$log = Join-Path $wt "log"
$racelog = Join-Path $log "race.log"
$rk = (Get-FileHash -Algorithm MD5 (Join-Path $wt "re\assets\geo\$GeoPlace\_route\ROUTE.JSON")).Hash.Substring(0, 8)   # keyed by the route: a cache of another route would aim at the wrong spans
$cache = Join-Path $wt "verify\out\gapruns_${GeoPlace}_$rk.txt"      # the runs, kept so a launch with a broken log still works
New-Item -ItemType Directory -Force -Path (Join-Path $wt "verify\out") | Out-Null
$gaps = @{}
$src = @()
if (Test-Path $racelog) { $src += @(Get-Content $racelog) }
foreach ($l in $src) {
    if ($l -match 'median gap: run (\d+) main (\d+)\.\.(\d+) <-> corridor (\d+)\.\.(\d+) side (\d)') {
        $gaps[[int]$Matches[1]] = @{ m0 = [int]$Matches[2]; m1 = [int]$Matches[3]; c0 = [int]$Matches[4]; c1 = [int]$Matches[5]; side = [int]$Matches[6] }
    }
}
if ($gaps.Count -gt 0) {
    Set-Content -Path $cache -Value (@($gaps.Keys | Sort-Object | ForEach-Object { $g = $gaps[$_]; "$_ $($g.m0) $($g.m1) $($g.c0) $($g.c1) $($g.side)" })) -Encoding ASCII
} elseif (Test-Path $cache) {
    foreach ($l in (Get-Content $cache)) { $t = $l.Split(' '); if ($t.Count -ge 6) { $gaps[[int]$t[0]] = @{ m0 = [int]$t[1]; m1 = [int]$t[2]; c0 = [int]$t[3]; c1 = [int]$t[4]; side = [int]$t[5] } } }
}
if ($gaps.Count -eq 0) { throw "no median-gap runs: log/race.log has no 'median gap: run' lines and there is no cache (launch the exe once on this place first: verify/geo_tour.ps1)" }
$runList = if ($Runs) { @($Runs -split "[,\s]+" | Where-Object { $_ } | ForEach-Object { [int]$_ }) } else { @($gaps.Keys | Sort-Object) }

if ($Extra -is [string]) {
    $h = @{}
    foreach ($kv in (($Extra -replace '^\s*@\{|\}\s*$', '') -split '[|;]')) {
        $p = $kv.Split('=', 2)
        if ($p.Count -eq 2 -and $p[0].Trim() -ne "") { $h[$p[0].Trim()] = $p[1].Trim().Trim('"') }
    }
    $Extra = $h
}
$gfx = @("--Lighting=0","--Quality=0","--SunShadows=0","--Reflections=0",
         "--WetRoads=0","--StreetLights=0","--CarLights=0","--LegacyShadows=0",
         "--GIQuality=0","--ShadowRays=0","--ReflectionQuality=0",
         "--CarShadows=0","--VFX=0","--WorldBillboards=0","--FoliageAA=0")
$outDir = Join-Path $wt "verify\out\gapdrive_$Tag"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$summary = @()
Add-Type -Name W6 -Namespace N6 -MemberDefinition '
  [DllImport("user32.dll")] public static extern bool SetWindowPos(
    IntPtr h, IntPtr a, int x, int y, int cx, int cy, uint f);' -ErrorAction SilentlyContinue

foreach ($k in $runList) {
    if (-not $gaps.ContainsKey($k)) { Write-Host "run ${k}: not in the log, skipped"; continue }
    $g = $gaps[$k]
    $start = [Math]::Max(10, $g.m0 - $Lead - $GridAhead)
    Get-ChildItem env: | Where-Object { $_.Name -like 'TD5RE_*' } | ForEach-Object { Remove-Item "env:$($_.Name)" }
    $env:TD5RE_GEO_PLACE = $GeoPlace
    $env:TD5RE_RT = "0"
    $env:TD5RE_WINDOW_TITLE = "TD5RE gapdrive $Tag $k"
    $env:TD5RE_GAP_DRIVE = "$k"
    $env:TD5RE_GAP_DRIVE_SPEED = "$Speed"
    foreach ($key in $Extra.Keys) {
        $name = if ("$key" -like 'TD5RE_*') { "$key" } else { "TD5RE_$key" }
        Set-Item "env:$name" "$($Extra[$key])"
    }
    foreach ($f in @("race.log","engine.log","frontend.log")) {
        $p0 = Join-Path $log $f
        for ($t = 0; $t -lt 20 -and (Test-Path $p0); $t++) { try { Remove-Item $p0 -Force -ErrorAction Stop } catch { Start-Sleep -Milliseconds 500 } }
    }
    $gameArgs = @("--AutoRace=1","--SkipIntro=1","--DefaultTrack=$Track","--DefaultOpponents=0","--Traffic=0",
                  "--PlayerIsAI=0","--CarDamage=0","--Windowed=1","--VSync=0","--Width=$Width","--Height=$Height",
                  "--StartSpanOffset=$start") + $gfx
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $p = Start-Process -FilePath (Join-Path $wt $Exe) -ArgumentList $gameArgs -WorkingDirectory $wt -PassThru
    $sent = $false
    while (-not $p.HasExited -and $sw.Elapsed.TotalSeconds -lt $Timeout) {
        Start-Sleep -Milliseconds 500
        if (-not $sent) {
            $p.Refresh()
            if ($p.MainWindowHandle -ne 0) {
                try { [void][N6.W6]::SetWindowPos($p.MainWindowHandle, [IntPtr]1, 0,0,0,0, 0x0013) } catch { }
                $sent = $true
            }
        }
    }
    $killed = $false
    if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force; $killed = $true; Start-Sleep -Seconds 2 }
    $dst = Join-Path $outDir "run$k.log"
    $res = "NO RESULT"
    if (Test-Path $racelog) {
        $gd = @(Select-String -Path $racelog -Pattern '\[GAPDRIVE\]' | ForEach-Object { $_.Line })
        Set-Content -Path $dst -Value $gd -Encoding ASCII
        $rr = @($gd | Where-Object { $_ -match 'RESULT' })
        if ($rr.Count) { $res = ($rr[0] -replace '^.*\[GAPDRIVE\] RESULT ', '') }
        # wall events on slot 0 near the gap
        $w = @(Select-String -Path $racelog -Pattern 'wall_contact: slot=0' | ForEach-Object { $_.Line }).Count
        $res += "; wall_contact log lines=$w"
    }
    if ($killed) { $res += " [KILLED at timeout]" }
    Write-Host ("run {0} (main {1}..{2}, start span {3}): {4}" -f $k, $g.m0, $g.m1, $start, $res)
    $summary += ("run {0} main {1}..{2} corr {3}..{4}: {5}" -f $k, $g.m0, $g.m1, $g.c0, $g.c1, $res)
}
Set-Content -Path (Join-Path $outDir "summary.txt") -Value $summary -Encoding ASCII
$ok = @($summary | Where-Object { $_ -match 'crossed=1' }).Count
Write-Host ("gapdrive {0}: {1}/{2} run(s) crossed to the corridor" -f $Tag, $ok, $summary.Count)
if ($ok -lt $summary.Count) { exit 1 }
