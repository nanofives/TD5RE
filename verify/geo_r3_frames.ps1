# geo_r3_frames.ps1 -- before/after framedumps for round 1009 R3 (items 6, 8, 11)
# on the La Plata geo track (slot 61 / level091).
#
# Both arms run the SAME exe; the "before" arm turns every one of this round's
# changes off through its own knob, so the only difference between the two sets
# of PNGs is the fix:
#
#   item 6  TD5RE_GEO_NET_SKEW_TANGENT=1  (the sin/cos crossing split)
#           TD5RE_GEO_NET_SKEW_MAX_DEG=65 (the 80-degree arm cap)
#           TD5RE_GEO_NET_DEPTH=0         (the drawn-ground depth cap)
#   item 8  TD5RE_GEO_STRUCT_OSM=0        (the invented underpass)
#   item 11 TD5RE_TG_SKYLINE_RIDGE=1      (the painted skyline ridge)
#
# Standing launch rules, all applied: --Windowed=1, --VSync=0, RT off and
# minimum graphics, no sockets, a hard wall-clock kill BY PID.
#
#   pwsh verify/geo_r3_frames.ps1 -Arm before
#   pwsh verify/geo_r3_frames.ps1 -Arm after
param([ValidateSet("before","after")][string]$Arm = "after",
      [string]$Spans = "80,400,670,805,1100",
      [int]$GenWait = 900,
      [int]$RaceSecs = 420)

$wt = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)

# Clear EVERY TD5RE_* first: these persist across runs in a shell and a stale
# knob silently invalidates an A/B.
Get-ChildItem env: | Where-Object { $_.Name -like 'TD5RE_*' } |
    ForEach-Object { Remove-Item "env:$($_.Name)" }

$env:TD5RE_GEO_PLACE        = "la_plata"
$env:TD5RE_AUTOTRACK_REUSE  = "0"
$env:TD5RE_TG_DOUBLE_BUILD  = "1"
$env:TD5RE_RT               = "0"
$env:TD5RE_WINDOW_TITLE     = "TD5RE r3frames $Arm"
$env:TD5RE_D3D12_CAPTURE    = "1"
$env:TD5RE_FRAMEDUMP_SPANS  = $Spans
$env:TD5RE_FRAMEDUMP_SPAN_PATH = "log/r3_${Arm}_span_%d.png"

if ($Arm -eq "before") {
    $env:TD5RE_GEO_NET_SKEW_TANGENT  = "1"
    $env:TD5RE_GEO_NET_SKEW_MAX_DEG  = "65"
    $env:TD5RE_GEO_NET_DEPTH         = "0"
    $env:TD5RE_GEO_STRUCT_OSM        = "0"
    $env:TD5RE_TG_SKYLINE_RIDGE      = "1"
}

$gfx = @("--Windowed=1","--VSync=0","--CarDamage=0","--Lighting=0","--Quality=0",
         "--SunShadows=0","--Reflections=0","--WetRoads=0","--StreetLights=0",
         "--CarLights=0","--LegacyShadows=0","--GIQuality=0","--ShadowRays=0",
         "--ReflectionQuality=0","--CarShadows=0","--VFX=0","--WorldBillboards=0",
         "--FoliageAA=0")

$lvl = Join-Path $wt "re\assets\levels\level091"
if (Test-Path $lvl) { Remove-Item $lvl -Recurse -Force }
Get-ChildItem (Join-Path $wt "log") -Filter "r3_${Arm}_span_*.png" -ErrorAction SilentlyContinue |
    Remove-Item -Force -ErrorAction SilentlyContinue
foreach ($f in @("race.log","engine.log","frontend.log")) {
    $p0 = Join-Path $wt "log\$f"
    for ($t = 0; $t -lt 20 -and (Test-Path $p0); $t++) {
        try { Remove-Item $p0 -Force -ErrorAction Stop } catch { Start-Sleep -Milliseconds 500 }
    }
}

Write-Host "arm=$Arm spans=$Spans"
$p = Start-Process -FilePath (Join-Path $wt "td5re.exe") `
      -ArgumentList (@("--AutoRace=1","--SkipIntro=1","--DefaultTrack=61",
                       "--PlayerIsAI=1","--AutoThrottle=1") + $gfx) `
      -WorkingDirectory $wt -PassThru
Write-Host "pid=$($p.Id)"

# Send the window to the back so a long race cannot sit over the user's work.
Start-Sleep -Seconds 2
try {
    Add-Type -Name W2 -Namespace N2 -MemberDefinition '
      [DllImport("user32.dll")] public static extern bool SetWindowPos(
        IntPtr h, IntPtr a, int x, int y, int cx, int cy, uint f);' -ErrorAction Stop
    if ($p.MainWindowHandle -ne 0) {
        [void][N2.W2]::SetWindowPos($p.MainWindowHandle, [IntPtr]1, 0,0,0,0, 0x0013)
    }
} catch { }

# Wait for the generate (MODELS.DAT present and stable), then let the AI drive.
$models = Join-Path $lvl "MODELS.DAT"
$last = -1; $stable = 0; $i = 0
for ($i = 0; $i -lt $GenWait; $i++) {
    Start-Sleep -Seconds 1
    if ($p.HasExited) { break }
    if (Test-Path $models) {
        $len = (Get-Item $models).Length
        if ($len -gt 0 -and $len -eq $last) { $stable++ } else { $stable = 0 }
        $last = $len
        if ($stable -ge 5) { break }
    }
}
Write-Host "models settled after ${i}s"

# Race until every span target has produced a PNG, or the wall clock runs out.
$want = ($Spans -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ })
for ($t = 0; $t -lt $RaceSecs; $t += 5) {
    Start-Sleep -Seconds 5
    if ($p.HasExited) { break }
    $have = @($want | Where-Object { Test-Path (Join-Path $wt "log\r3_${Arm}_span_$_.png") })
    if ($have.Count -eq $want.Count) { Write-Host "all $($want.Count) frame(s) captured at ${t}s"; break }
}
# PID-scoped, never a name-wide kill: parallel sessions share this machine.
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force; Start-Sleep -Seconds 2 }

Write-Host "### $Arm"
foreach ($s in $want) {
    $q = Join-Path $wt "log\r3_${Arm}_span_$s.png"
    if (Test-Path $q) { Write-Host ("  span {0,-5} {1,9} bytes  {2}" -f $s, (Get-Item $q).Length, $q) }
    else              { Write-Host ("  span {0,-5} MISSING" -f $s) }
}
if (Test-Path $lvl) {
    foreach ($f in @("MODELS.DAT","NETWORK.JSON")) {
        $q = Join-Path $lvl $f
        if (Test-Path $q) { Write-Host ("  {0,-13} {1,10} bytes" -f $f, (Get-Item $q).Length) }
    }
    Copy-Item (Join-Path $lvl "NETWORK.JSON") (Join-Path $wt "log\r3_${Arm}_NETWORK.JSON") -Force -ErrorAction SilentlyContinue
}
foreach ($f in @("race.log","engine.log")) {
    $src = Join-Path $wt "log\$f"
    if (Test-Path $src) { Copy-Item $src (Join-Path $wt "log\r3_${Arm}_$f") -Force }
}
