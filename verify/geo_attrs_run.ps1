# geo_attrs_run.ps1 -- [ROUND 1011 C3] run a geo race and dump the OSM
# attribute census (lit / maxspeed / street names).
#
#   pwsh verify/geo_attrs_run.ps1 -Tag base
#   pwsh verify/geo_attrs_run.ps1 -Tag litdefault -Extra @{TD5RE_GEO_LIT_DEFAULT='1'}
#   pwsh verify/geo_attrs_run.ps1 -Tag off        -Extra @{TD5RE_GEO_ATTRS='0'}
#
# CLEAN SHUTDOWN WITHOUT A SOCKET. race.log -- which is where LOG_TAG "geo"
# lands -- only flushes on a clean shutdown, and this round may not pass
# --Control=1, so the graceful `quit` verb is unavailable. --RaceTraceMaxSimTicks
# is the way out: it quits the game on its own tick budget even with RaceTrace=0
# (it is gated on its own value), which IS a clean shutdown, so the log flushes.
# A PID-scoped kill is only the backstop.
#
# Standing launch rules, all applied: --Windowed=1, never fullscreen, never
# steal focus, RT off, minimum graphics, no networking, PID-scoped kill.
param([string]$Tag = "run",
      [string]$Place = "la_plata",
      [int]$Slot = 61,
      [hashtable]$Extra = @{},
      [int]$GenWait = 1200,
      [int]$Ticks = 2500,
      [switch]$Regen)

$wt = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)

# Clear EVERY TD5RE_* first: they persist across runs in a shell and a stale
# knob silently invalidates an A/B.
Get-ChildItem env: | Where-Object { $_.Name -like 'TD5RE_*' } |
    ForEach-Object { Remove-Item "env:$($_.Name)" }

$env:TD5RE_GEO_PLACE    = $Place
$env:TD5RE_RT           = "0"
$env:TD5RE_WINDOW_TITLE = "TD5RE geoattrs $Tag"
if ($Regen) { $env:TD5RE_AUTOTRACK_REUSE = "0" }
foreach ($k in $Extra.Keys) { Set-Item "env:$k" $Extra[$k] }

$gfx = @("--Windowed=1","--VSync=0","--CarDamage=0","--PlayerIsAI=1",
         "--Lighting=0","--Quality=0","--SunShadows=0","--Reflections=0",
         "--WetRoads=0","--StreetLights=0","--CarLights=0","--LegacyShadows=0",
         "--GIQuality=0","--ShadowRays=0","--ReflectionQuality=0",
         "--CarShadows=0","--VFX=0","--WorldBillboards=0","--FoliageAA=0")

$lvl = Join-Path $wt "re\assets\levels\level$(($Slot - 61 + 91).ToString('000'))"
foreach ($f in @("race.log","engine.log","frontend.log")) {
    $p0 = Join-Path $wt "log\$f"
    for ($t = 0; $t -lt 20 -and (Test-Path $p0); $t++) {
        try { Remove-Item $p0 -Force -ErrorAction Stop } catch { Start-Sleep -Milliseconds 500 }
    }
}

Write-Host "place=$Place slot=$Slot tag=$Tag level=$lvl"
$p = Start-Process -FilePath (Join-Path $wt "td5re.exe") `
      -ArgumentList (@("--AutoRace=1","--SkipIntro=1","--DefaultTrack=$Slot",
                       "--RaceTraceMaxSimTicks=$Ticks") + $gfx) `
      -WorkingDirectory $wt -PassThru
Write-Host "pid=$($p.Id)"

# Send the window to the back: a long generate must not sit over the user's
# work. Occluded is fine, nothing here measures frame time.
Start-Sleep -Seconds 2
try {
    Add-Type -Name W2 -Namespace N2 -MemberDefinition '
      [DllImport("user32.dll")] public static extern bool SetWindowPos(
        IntPtr h, IntPtr a, int x, int y, int cx, int cy, uint f);' -ErrorAction Stop
    if ($p.MainWindowHandle -ne 0) {
        [void][N2.W2]::SetWindowPos($p.MainWindowHandle, [IntPtr]1, 0,0,0,0, 0x0013)
    }
} catch { }

for ($i = 0; $i -lt $GenWait -and -not $p.HasExited; $i++) { Start-Sleep -Seconds 1 }
if (-not $p.HasExited) {
    Write-Host "did not exit on its own after ${GenWait}s -- PID-scoped kill"
    Stop-Process -Id $p.Id -Force; Start-Sleep -Seconds 2
} else {
    Write-Host "exited cleanly after ~${i}s (rc=$($p.ExitCode))"
}

foreach ($f in @("race.log","engine.log")) {
    $src = Join-Path $wt "log\$f"
    if (Test-Path $src) { Copy-Item $src (Join-Path $wt "log\geoattrs_${Tag}_$f") -Force }
}

Write-Host "`n### roads reader census"
$rl = Join-Path $wt "log\geoattrs_${Tag}_race.log"
foreach ($pat in @("roads .* loaded", "roads surface", "roads tags",
                   "\[GEO ATTRS\]", "\[GEO LAMP\]", "\[GEO SPEED\]")) {
    if (Test-Path $rl) {
        Select-String -Path $rl -Pattern $pat | Select-Object -First 8 |
            ForEach-Object { Write-Host ("  " + $_.Line.Trim()) }
    }
}
Write-Host "`n### level artefacts"
foreach ($f in @("MODELS.DAT","STRIP.DAT","TEXTURES.DAT")) {
    $q = Join-Path $lvl $f
    if (Test-Path $q) {
        Write-Host ("  {0,-13} {1,10} bytes" -f $f, (Get-Item $q).Length)
    } else { Write-Host ("  {0,-13} MISSING" -f $f) }
}
