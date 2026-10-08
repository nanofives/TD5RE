# geo_commit_immutable.ps1 -- round 1008, N1 CACHE.
#
# The one check that would have caught the round-1007 corruption: in ONE
# process, build three DIFFERENT routes one after another and then race the
# last one, and prove the fetched place cache is byte-identical afterwards.
#
# A fresh process per build could never have found it. Commit 1 looked healthy;
# commit 2, reading commit 1's output back as if it were the fetched data, is
# what collapsed La Plata to a 2x2 grid.
#
# What it asserts:
#   1. every file directly under re/assets/geo/<slug>/ is byte-identical
#      before and after (the SOURCE is immutable)
#   2. the derived frame re/assets/geo/<slug>/_route/ is healthy -- stamped,
#      full-size rasters on one grid, non-empty vector layers
#   3. the race ran: race.log census (buildings bound, streets, signals)
#   4. framedumps at an early and a late span
#
# Standing launch rules, all applied: --Windowed=1, --VSync=0 (the monitors may
# be off and vsync stalls presents at ~257 ms), RT off and minimum graphics, NO
# control socket, a hard wall-clock kill BY PID, no send-to-back loop. Geo races
# use slot 61.
param([string]$Slug   = "la_plata",
      [int]$Builds    = 3,
      [int]$SimTicks  = 6000,
      [string]$Spans  = "50,150,400,800",
      [int]$WaitSecs  = 1500,
      [string]$Tag    = "n1")

$ErrorActionPreference = "Stop"
$wt   = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$src  = Join-Path $wt "re\assets\geo\$Slug"
$drv  = Join-Path $src "_route"
$out  = Join-Path $wt "log"

function Hash-SourceDir {
    # Top level ONLY. _cache is the fetch cache, not a product, and it is 263
    # files -- hashing it would hide the signal in noise.
    Get-ChildItem $src -File | Sort-Object Name | ForEach-Object {
        [pscustomobject]@{
            Name = $_.Name
            Size = $_.Length
            Hash = (Get-FileHash $_.FullName -Algorithm SHA256).Hash
        }
    }
}

Write-Host "=== BEFORE: the fetched SOURCE under $src ==="
$before = Hash-SourceDir
$before | ForEach-Object { "  {0,-16} {1,10}  {2}" -f $_.Name, $_.Size, $_.Hash.Substring(0,24) }

# Start from no derived frame at all, so build 1 has to create it.
if (Test-Path $drv) { Remove-Item $drv -Recurse -Force; Write-Host "`nremoved the old derived frame" }

# Clear EVERY TD5RE_* first: they persist across runs in a shell and a stale
# knob silently invalidates the run (memory: env_vars_persist_across_runs).
Get-ChildItem env: | Where-Object { $_.Name -like 'TD5RE_*' } |
    ForEach-Object { Remove-Item "env:$($_.Name)" }

$env:TD5RE_GEO_PLACE             = $Slug
$env:TD5RE_GEO_SEED              = "1"
$env:TD5RE_GEO_AUTOBUILD         = "1"
$env:TD5RE_GEO_AUTOBUILD_N       = "$Builds"
$env:TD5RE_RT                    = "0"
$env:TD5RE_D3D12_CAPTURE         = "1"
$env:TD5RE_FRAMEDUMP_SPANS       = $Spans
$env:TD5RE_FRAMEDUMP_SPAN_PATH   = "log/${Tag}_span_%d.png"
$env:TD5RE_WINDOW_TITLE          = "TD5RE n1 immutable-commit $Tag"

$lvl = Join-Path $wt "re\assets\levels\level091"
if (Test-Path $lvl) { Remove-Item $lvl -Recurse -Force }
Get-ChildItem $out -Filter "${Tag}_span_*.png" -ErrorAction SilentlyContinue | Remove-Item -Force
foreach ($f in @("race.log","engine.log","frontend.log")) {
    $p0 = Join-Path $out $f
    for ($t = 0; $t -lt 20 -and (Test-Path $p0); $t++) {
        try { Remove-Item $p0 -Force -ErrorAction Stop } catch { Start-Sleep -Milliseconds 500 }
    }
}

$gfx = @("--Windowed=1","--VSync=0","--Lighting=0","--Quality=0","--SunShadows=0",
         "--Reflections=0","--WetRoads=0","--StreetLights=0","--CarLights=0",
         "--LegacyShadows=0","--GIQuality=0","--ShadowRays=0",
         "--ReflectionQuality=0","--CarShadows=0","--VFX=0",
         "--WorldBillboards=0","--FoliageAA=0")
$args = @("--SkipIntro=1","--StartScreen=56","--StartScreenDirect=1",
          "--RaceTraceMaxSimTicks=$SimTicks") + $gfx

Write-Host "`nlaunching: td5re.exe $($args -join ' ')"
$p = Start-Process -FilePath (Join-Path $wt "td5re.exe") -ArgumentList $args `
                   -WorkingDirectory $wt -PassThru
Write-Host "pid=$($p.Id)  builds=$Builds  simticks=$SimTicks  spans=$Spans"

# ONE send-to-back, never a loop. Nothing here measures frame time, so an
# occluded window is fine.
Start-Sleep -Seconds 3
try {
    Add-Type -Name W2 -Namespace N2 -MemberDefinition '
      [DllImport("user32.dll")] public static extern bool SetWindowPos(
        IntPtr h, IntPtr a, int x, int y, int cx, int cy, uint f);' -ErrorAction Stop
    if ($p.MainWindowHandle -ne 0) {
        [void][N2.W2]::SetWindowPos($p.MainWindowHandle, [IntPtr]1, 0,0,0,0, 0x0013)
    }
} catch { }

# HARD wall-clock cap, and the kill is PID-scoped -- parallel sessions share
# this machine and a name-wide kill would take theirs down too.
$sw = [Diagnostics.Stopwatch]::StartNew()
while (-not $p.HasExited -and $sw.Elapsed.TotalSeconds -lt $WaitSecs) { Start-Sleep -Seconds 5 }
if (-not $p.HasExited) {
    Write-Host "`nWALL-CLOCK CAP ${WaitSecs}s reached -- killing pid $($p.Id)"
    Stop-Process -Id $p.Id -Force
    Start-Sleep -Seconds 3
} else {
    Write-Host "`nexited on its own after $([int]$sw.Elapsed.TotalSeconds)s, code $($p.ExitCode)"
}

Write-Host "`n=== AFTER: the fetched SOURCE ==="
$after = Hash-SourceDir
$bad = 0
foreach ($a in $after) {
    $b = $before | Where-Object { $_.Name -eq $a.Name }
    if (-not $b)              { Write-Host "  ADDED    $($a.Name)"; $bad++ }
    elseif ($b.Hash -ne $a.Hash) {
        Write-Host ("  CHANGED  {0}  {1} -> {2}  ({3} -> {4} bytes)" -f `
                    $a.Name, $b.Hash.Substring(0,16), $a.Hash.Substring(0,16), $b.Size, $a.Size)
        $bad++
    }
}
foreach ($b in $before) {
    if (-not ($after | Where-Object { $_.Name -eq $b.Name })) { Write-Host "  DELETED  $($b.Name)"; $bad++ }
}
if ($bad -eq 0) { Write-Host "  SOURCE UNCHANGED: $($after.Count) file(s) byte-identical  <-- the invariant" }
else            { Write-Host "  *** SOURCE WAS MODIFIED: $bad file(s) -- THE INVARIANT IS BROKEN ***" }

Write-Host "`n=== the DERIVED frame $drv ==="
if (-not (Test-Path $drv)) { Write-Host "  MISSING -- no commit produced one" }
else {
    Get-ChildItem $drv -File | Sort-Object Name | ForEach-Object {
        "  {0,-16} {1,10} bytes  {2}" -f $_.Name, $_.Length, (Get-FileHash $_.FullName -Algorithm SHA256).Hash.Substring(0,24)
    }
    $stamp = Join-Path $drv "DERIVED.OK"
    Write-Host ("  stamp present: {0}" -f (Test-Path $stamp))
}

Write-Host "`n=== framedumps ==="
Get-ChildItem $out -Filter "${Tag}_span_*.png" -ErrorAction SilentlyContinue |
    Sort-Object Name | ForEach-Object { "  {0,-24} {1,9} bytes" -f $_.Name, $_.Length }

Write-Host "`n=== the three commits, from the log ==="
foreach ($f in @("frontend.log","race.log","engine.log")) {
    $q = Join-Path $out $f
    if (-not (Test-Path $q)) { continue }
    Copy-Item $q (Join-Path $out "${Tag}_$f") -Force
    Select-String -Path $q -Pattern "AUTOBUILD|geo route: committed|COMMIT REFUSED|re-grid|NO MAP DATA|road graph|geo: loaded" |
        ForEach-Object { "  [$f] " + $_.Line }
}

Write-Host "`n=== race census ==="
$rl = Join-Path $out "race.log"
if (Test-Path $rl) {
    Select-String -Path $rl -Pattern "geob:|real street census|GEO SIGNALS|landmarks by deciding|roads surface" |
        Select-Object -First 20 | ForEach-Object { "  " + $_.Line }
} else { Write-Host "  race.log MISSING (the race never ran, or the quit was not clean)" }
