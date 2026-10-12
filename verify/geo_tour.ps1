# geo_tour.ps1 -- FAST free-cam tour of a geo track (round 1019 T). ONE launch captures every
# pose of a list, anywhere along the route, then the game quits cleanly.
#
#   pwsh verify/geo_tour.ps1 -Tag after -Poses poses.txt
#   pwsh verify/geo_tour.ps1 -Tag picks -Picks picks.txt            # "name x y z" per line
#   pwsh verify/geo_tour.ps1 -Tag loops -Spans 561,922 -Back 4 -TopDown
#   pwsh verify/geo_tour.ps1 -Tag ab -Poses p.txt -Extra @{TD5RE_GEO_FORK_MERGE="0"}
#
# Poses (one per line or ';'-separated, '#' comments):
#   name:x,y,z,yawdeg,pitchdeg            world pose, the numbers a pick prints (yaw 0 = +Z)
#   name:@span,lateral,height,yawrel,pitch   span-relative: the eye sits at the centre of strip
#                                         span `span` (a corridor span >= ring works), `lateral`
#                                         world units to the RIGHT of the road, `height` above it,
#                                         yawrel degrees off the road direction
# -Picks: one pick per line, "name x y z" OR the raw log line "pick: copied level091 L91 e10 s7 ... pos x,y,z ..."
#         (prefix optional); each yields a behind-and-above view and a
#         top-down view (name_top), via verify/geo_r1014a_poses.py.
# -Spans: span numbers; each yields sN at span N-Back, 1800 up, looking along the road
#         (-TopDown adds sN_top, 9000 up looking straight down).
#
# What makes it fast (td5_camera.c tour_step, dev builds only):
#   * no regeneration when the built level matches: the GENSTAMP no longer hashes the tour
#     knobs, and this script does not delete the level dir or set AUTOTRACK_REUSE=0. The first
#     launch after an exe/knob change builds once; every later launch reuses it. The stamp does
#     not cover the geo SOURCE files (roads, footprints): after changing data without rebuilding
#     the exe pass -Rebuild once.
#   * no stream waiting: the tour starts only when the scenery is COMPLETE (a reused level loads
#     the whole MODELS.DAT from disk, a cold one waits exactly for the stream worker).
#   * the track walk is centred on each POSE's span, not on the car, so a far pose still draws its
#     road, fork corridors and scenery. Sim frozen, render-only.
#   * the manifest (<Out>/<Tag>_manifest.csv) says per pose whether the scenery was complete at
#     capture time; the report flags any that was not, and any PNG that is missing.
#
# Knobs reach the game ONLY through -Extra: this script CLEARS every TD5RE_* first (a stale knob
# silently invalidates an A/B). A knob that changes the BUILD changes the stamp and rebuilds once.
#
# Standing launch rules, all applied: --Windowed=1, --VSync=0, sent to back without activation,
# RT off and minimum graphics, car damage off, no sockets/network, a hard wall-clock kill BY PID.
# Frames land in <Out>/tour_<Tag>_<name>.png (default Out = log).
param([string]$Tag = "run",
      [string]$Poses = "",
      [string]$Picks = "",
      [string]$Spans = "",       # "100,561,922" (comma or space separated)
      [int]$Back = 4,
      [switch]$TopDown,
      [string]$Out = "",
      [object]$Extra = @{},        # @{TD5RE_KNOB="v"} or, from `pwsh -File`, the string "TD5RE_KNOB=v|OTHER=w"
      [string]$Exe = "td5re.exe",
      [string]$GeoPlace = "",
      [int]$Track = 61,
      [int]$Dwell = 6,
      [int]$Width = 1280,
      [int]$Height = 720,
      [int]$Opponents = 0,
      [int]$AutoThrottle = 1,
      [int]$StartSpanOffset = 0,
      [switch]$Rebuild,           # force one regeneration (geo SOURCE data changed without a code change)
      [int]$Timeout = 900)

$ErrorActionPreference = "Stop"
$wt = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
Get-ChildItem env: | Where-Object { $_.Name -like 'TD5RE_*' } | ForEach-Object { Remove-Item "env:$($_.Name)" }

if (-not $GeoPlace) {
    $sel = Join-Path $wt "re\assets\geo\SELECTED.TXT"
    $GeoPlace = if (Test-Path $sel) { (Get-Content -Raw $sel).Trim() } else { "la_plata_partido" }
}
if (-not $Out) { $Out = Join-Path $wt "log" }
elseif (-not [IO.Path]::IsPathRooted($Out)) { $Out = Join-Path $wt $Out }
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$log = Join-Path $wt "log"
New-Item -ItemType Directory -Force -Path $log | Out-Null

# ---- build the pose list -------------------------------------------------------------------
$lines = New-Object System.Collections.Generic.List[string]
if ($Poses) {
    foreach ($l in ((Get-Content -Raw $Poses) -split "[;\r\n]+")) {
        if ($l.Trim() -and -not $l.Trim().StartsWith('#')) { $lines.Add($l.Trim()) }
    }
}
if ($Picks) {
    # One pick per line, either "name x y z" or the RAW string the game logs on a pick, e.g.
    # "[INF][render] pick: copied level091 L91 e10 s7 branch-road p44:SIDEWALK+121 pos 61420,-574,9475 r1214 v16 c2"
    # (with or without the "[INF][render] pick: copied" prefix). Raw lines are named pk<N>_e<entry>s<slot>.
    $a = @()
    $n = 0
    foreach ($l in (Get-Content $Picks)) {
        $t = $l.Trim()
        if (-not $t -or $t.StartsWith('#')) { continue }
        $n++
        if ($t -match '(?:^|\s)pos\s+(-?[\d.]+)\s*,\s*(-?[\d.]+)\s*,\s*(-?[\d.]+)') {
            $px = $Matches[1]; $py = $Matches[2]; $pz = $Matches[3]
            $nm = "pk$n"
            if ($t -match '(?:^|\s)e(\d+)\s+s(\d+)(?:\s|$)') { $nm = "pk${n}_e$($Matches[1])s$($Matches[2])" }
            $a += @($nm, $px, $py, $pz)
        } else {
            $tok = @($t -split "[\s,]+" | Where-Object { $_ })
            if ($tok.Count -ne 4) { throw "-Picks line $n is neither 'name x y z' nor a raw pick string with 'pos x,y,z': $t" }
            $a += $tok
        }
    }
    if ($a.Count -eq 0) { throw "-Picks file $Picks has no pick lines" }
    $env:TD5RE_GEO_PLACE = $GeoPlace    # geo_r1014a_poses.py reads the route of this place
    $errFile = Join-Path $Out "tour_${Tag}_poses_conv.err"
    $conv = & python -I (Join-Path $wt "verify\geo_r1014a_poses.py") @a 2>$errFile
    $rc = $LASTEXITCODE
    Remove-Item env:TD5RE_GEO_PLACE -ErrorAction SilentlyContinue
    $convErr = if (Test-Path $errFile) { (Get-Content -Raw $errFile) } else { "" }
    if ($rc -ne 0) { throw "geo_r1014a_poses.py failed (exit $rc) for place '$GeoPlace':`n$convErr" }
    if ($convErr -and $convErr.Trim()) { Write-Host $convErr.TrimEnd() }
    foreach ($l in (($conv -join ";") -split "[;\r\n]+")) { if ($l.Trim()) { $lines.Add($l.Trim()) } }
}
foreach ($s in @($Spans -split "[,\s]+" | Where-Object { $_ } | ForEach-Object { [int]$_ })) {
    $b = [Math]::Max(0, $s - $Back)
    $lines.Add(("s{0}:@{1},0,1800,0,-12" -f $s, $b))
    if ($TopDown) { $lines.Add(("s{0}_top:@{0},0,9000,0,-82" -f $s)) }
}
if ($lines.Count -eq 0) { throw "no poses: give -Poses, -Picks or -Spans" }
$names = @($lines | ForEach-Object { $_.Split(':')[0].Trim() })
$dup = @($names | Group-Object | Where-Object { $_.Count -gt 1 } | ForEach-Object { $_.Name })
if ($dup.Count) { throw "duplicate pose names: $($dup -join ', ')" }
$poseFile = Join-Path $Out "tour_${Tag}_poses.txt"
Set-Content -Path $poseFile -Value ($lines -join "`n") -Encoding ASCII

$png = { param($n) Join-Path $Out "tour_${Tag}_$n.png" }
$manifest = Join-Path $Out "tour_${Tag}_manifest.csv"
foreach ($n in $names) { $q = & $png $n; if (Test-Path -LiteralPath $q) { Remove-Item -LiteralPath $q -Force } }
if (Test-Path $manifest) { Remove-Item $manifest -Force }

# ---- environment -----------------------------------------------------------------------------
$env:TD5RE_GEO_PLACE            = $GeoPlace
$env:TD5RE_RT                   = "0"
$env:TD5RE_D3D12_CAPTURE        = "1"
$env:TD5RE_WINDOW_TITLE         = "TD5RE tour $Tag"
$env:TD5RE_FREECAM_TOUR_FAST    = "1"
$env:TD5RE_FREECAM_TOUR_FILE    = $poseFile
$env:TD5RE_FREECAM_TOUR_PATH    = ((Join-Path $Out "tour_${Tag}_%s.png") -replace '\\', '/')
$env:TD5RE_FREECAM_TOUR_MANIFEST = ($manifest -replace '\\', '/')
$env:TD5RE_FREECAM_TOUR_DWELL   = "$Dwell"
if ($Extra -is [string]) {
    $h = @{}
    foreach ($kv in (($Extra -replace '^\s*@\{|\}\s*$', '') -split '[|;]')) {
        $p = $kv.Split('=', 2)
        if ($p.Count -eq 2 -and $p[0].Trim() -ne "") { $h[$p[0].Trim()] = $p[1].Trim().Trim('"') }
    }
    $Extra = $h
}
foreach ($k in $Extra.Keys) {
    $name = if ("$k" -like 'TD5RE_*') { "$k" } else { "TD5RE_$k" }
    Set-Item "env:$name" "$($Extra[$k])"
}

if ($Rebuild) {
    # The stamp keys on seed/spec/route nodes/env/exe, not on the geo source files: drop it to force a build.
    $st = Join-Path $wt ("re\assets\levels\level{0:D3}\GENSTAMP.TXT" -f ($Track + 30))
    if (Test-Path $st) { Remove-Item $st -Force }
}
$gfx = @("--Lighting=0","--Quality=0","--SunShadows=0","--Reflections=0",
         "--WetRoads=0","--StreetLights=0","--CarLights=0","--LegacyShadows=0",
         "--GIQuality=0","--ShadowRays=0","--ReflectionQuality=0",
         "--CarShadows=0","--VFX=0","--WorldBillboards=0","--FoliageAA=0")
foreach ($f in @("race.log","engine.log","frontend.log")) {
    $p0 = Join-Path $log $f
    for ($t = 0; $t -lt 20 -and (Test-Path $p0); $t++) {
        try { Remove-Item $p0 -Force -ErrorAction Stop } catch { Start-Sleep -Milliseconds 500 }
    }
}
$gameArgs = @("--AutoRace=1","--SkipIntro=1","--DefaultTrack=$Track","--DefaultOpponents=$Opponents",
              "--Traffic=0","--PlayerIsAI=1","--AutoThrottle=$AutoThrottle","--CarDamage=0",
              "--Windowed=1","--VSync=0","--Width=$Width","--Height=$Height") + $(if ($StartSpanOffset -gt 0) { @("--StartSpanOffset=$StartSpanOffset") } else { @() }) + $gfx

# ---- run -------------------------------------------------------------------------------------
$sw = [Diagnostics.Stopwatch]::StartNew()
$p = Start-Process -FilePath (Join-Path $wt $Exe) -ArgumentList $gameArgs -WorkingDirectory $wt -PassThru
Write-Host "pid=$($p.Id) tag=$Tag exe=$Exe place=$GeoPlace poses=$($names.Count) out=$Out"
try {
    Add-Type -Name W5 -Namespace N5 -MemberDefinition '
      [DllImport("user32.dll")] public static extern bool SetWindowPos(
        IntPtr h, IntPtr a, int x, int y, int cx, int cy, uint f);' -ErrorAction Stop
} catch { }
$sent = $false
$firstFrameSecs = -1
while (-not $p.HasExited -and $sw.Elapsed.TotalSeconds -lt $Timeout) {
    Start-Sleep -Milliseconds 500
    if (-not $sent) {
        $p.Refresh()
        if ($p.MainWindowHandle -ne 0) {
            try { [void][N5.W5]::SetWindowPos($p.MainWindowHandle, [IntPtr]1, 0,0,0,0, 0x0013) } catch { }
            $sent = $true
        }
    }
    if ($firstFrameSecs -lt 0 -and (Test-Path $manifest)) { $firstFrameSecs = [int]$sw.Elapsed.TotalSeconds }
}
$killed = $false
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force; $killed = $true; Start-Sleep -Seconds 2 }
$wall = [int]$sw.Elapsed.TotalSeconds
Write-Host ("game {0} after {1} s (first pose at {2} s)" -f $(if ($killed) { "KILLED at the $Timeout s timeout" } else { "exited cleanly" }), $wall, $firstFrameSecs)

# ---- report ----------------------------------------------------------------------------------
$gen = "?"
foreach ($f in @("race.log")) {
    $lp = Join-Path $log $f
    if (Test-Path $lp) {
        $txt = Get-Content -Raw $lp
        if ($txt -match "REUSED the on-disk build") { $gen = "REUSED (no generation)" }
        elseif ($txt -match "generated track ready") { $gen = "BUILT this launch" }
    }
}
Write-Host "level: $gen"
$rows = @{}
if (Test-Path $manifest) {
    foreach ($r in (Import-Csv $manifest)) { if ($r.name -and $r.PSObject.Properties.Name -contains 'complete') { $rows[$r.name] = $r } }
}
$bad = 0
foreach ($n in $names) {
    $q = & $png $n
    $size = if (Test-Path -LiteralPath $q) { (Get-Item -LiteralPath $q).Length } else { 0 }
    $m = $rows[$n]
    $state = if ($size -eq 0) { "MISSING"; $bad++ }
             elseif (-not $m) { "NO-MANIFEST-ROW"; $bad++ }
             elseif ($m.complete -ne "1") { "INCOMPLETE-SCENERY"; $bad++ }
             else { "ok" }
    Write-Host ("  {0,-22} {1,-18} cull_span={2,-5} {3} bytes" -f $n, $state, $(if ($m) { $m.cull_span } else { "-" }), $size)
}
if ($killed) { $bad++ }
Write-Host ("tour {0}: {1}/{2} pose(s) fully textured, {3} s wall" -f $Tag, ($names.Count - $bad), $names.Count, $wall)
if ($bad -gt 0) { exit 1 }
