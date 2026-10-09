# geo_f1_frames.ps1 -- before/after framedumps for round 1013 F1 (the second
# Diagonal 73 run, and buildings standing on plazas) on the La Plata geo track
# (slot 61 / level091), over Mariano's own saved route.
#
# Both arms run the SAME exe. What differs between them is exactly the fix:
#
#   item A  the ROUTE. AVENUES.JSON is written at commit, so the "before" arm is
#           the route as the round-1012 commit wrote it (2 avenues, the second
#           Diagonal 73 run missing) and the "after" arm is the re-commit. Pass
#           -RouteSrc <dir> to install a _route/ into re/assets/geo/la_plata/
#           first (it is DERIVED data; the SOURCE JSON next to it is never
#           touched).
#   item B  TD5RE_GEO_PLAZA_RING=0 + TD5RE_GEO_PLAZA_BLD=0 on the "before" arm:
#           a named plaza ring is not open space and a footprint in a plaza is
#           emitted, as on master bc143d22.
#
# Standing launch rules, all applied: --Windowed=1, --VSync=0, RT off and
# minimum graphics, no sockets, a hard wall-clock kill BY PID.
#
#   pwsh verify/geo_f1_frames.ps1 -Arm before -RouteSrc <archive>\_route
#   pwsh verify/geo_f1_frames.ps1 -Arm after  -RouteSrc <recommitted _route>
param([ValidateSet("before","after")][string]$Arm = "after",
      # 8        the start line, Parque Manuel Alberti on the left
      # 150      first Diagonal 73 run, divided (control, built before and after)
      # 270/295/320  Plaza Miguel de Azcuenaga, a named ring with NO polygon
      # 345/450/560  the SECOND Diagonal 73 run (spans 337..588 after the fix)
      # 540      Plazoleta Ingeniero Urbanista della Paolera, beside the route
      # 640/670  Plaza Mariano Moreno, a 291 x 241 m park polygon
      # 925      Plaza Maximo Paz, a 137 m park polygon at the end of the route
      [string]$Spans = "8,150,270,295,320,345,450,560,540,640,670,925",
      [string]$RouteSrc = "",
      [int]$GenWait = 900,
      [int]$RaceSecs = 900)

$wt = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)

# Clear EVERY TD5RE_* first: these persist across runs in a shell and a stale
# knob silently invalidates an A/B.
Get-ChildItem env: | Where-Object { $_.Name -like 'TD5RE_*' } |
    ForEach-Object { Remove-Item "env:$($_.Name)" }

if ($RouteSrc) {
    $dst = Join-Path $wt "re\assets\geo\la_plata\_route"
    if (-not (Test-Path (Join-Path $RouteSrc "ROUTE.JSON"))) { throw "no ROUTE.JSON in $RouteSrc" }
    robocopy $RouteSrc $dst /E /NFL /NDL /NJH /NJS /NC /NS /NP | Out-Null
    $global:LASTEXITCODE = 0
}
$av = Join-Path $wt "re\assets\geo\la_plata\_route\AVENUES.JSON"
if (Test-Path $av) {
    $j = Get-Content $av -Raw -Encoding utf8 | ConvertFrom-Json
    Write-Host ("route: AVENUES.JSON has {0} avenue(s):" -f @($j.avenues).Count)
    foreach ($a in @($j.avenues)) { Write-Host ("  {0}  spans {1}..{2}" -f $a.name, $a.s0, $a.s1) }
} else { Write-Host "route: no AVENUES.JSON" }

$env:TD5RE_GEO_PLACE           = "la_plata"
$env:TD5RE_AUTOTRACK_REUSE     = "0"
$env:TD5RE_RT                  = "0"
$env:TD5RE_WINDOW_TITLE        = "TD5RE f1 $Arm"
$env:TD5RE_D3D12_CAPTURE       = "1"
$env:TD5RE_FRAMEDUMP_SPANS     = $Spans
$env:TD5RE_FRAMEDUMP_SPAN_PATH = "log/f1_${Arm}_span_%d.png"

if ($Arm -eq "before") {
    $env:TD5RE_GEO_PLAZA_RING = "0"
    $env:TD5RE_GEO_PLAZA_BLD  = "0"
}

$gfx = @("--Windowed=1","--VSync=0","--CarDamage=0","--Lighting=0","--Quality=0",
         "--SunShadows=0","--Reflections=0","--WetRoads=0","--StreetLights=0",
         "--CarLights=0","--LegacyShadows=0","--GIQuality=0","--ShadowRays=0",
         "--ReflectionQuality=0","--CarShadows=0","--VFX=0","--WorldBillboards=0",
         "--FoliageAA=0")

$lvl = Join-Path $wt "re\assets\levels\level091"
if (Test-Path $lvl) { Remove-Item $lvl -Recurse -Force }
New-Item -ItemType Directory -Force (Join-Path $wt "log") | Out-Null
# Delete ONLY the spans this run is about to capture (a blanket wildcard delete
# once threw away frames that were still the evidence for a different span).
foreach ($s in ($Spans -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ })) {
    $q = Join-Path $wt "log\f1_${Arm}_span_$s.png"
    if (Test-Path $q) { Remove-Item -LiteralPath $q -Force -ErrorAction SilentlyContinue }
}
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

# Send the window to the back, without activating it, so a long race cannot sit
# over the user's work and never takes focus.
Start-Sleep -Seconds 2
try {
    Add-Type -Name W3 -Namespace N3 -MemberDefinition '
      [DllImport("user32.dll")] public static extern bool SetWindowPos(
        IntPtr h, IntPtr a, int x, int y, int cx, int cy, uint f);' -ErrorAction Stop
    if ($p.MainWindowHandle -ne 0) {
        [void][N3.W3]::SetWindowPos($p.MainWindowHandle, [IntPtr]1, 0,0,0,0, 0x0013)
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

$want = ($Spans -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ })
for ($t = 0; $t -lt $RaceSecs; $t += 5) {
    Start-Sleep -Seconds 5
    if ($p.HasExited) { break }
    $have = @($want | Where-Object { Test-Path (Join-Path $wt "log\f1_${Arm}_span_$_.png") })
    if ($have.Count -eq $want.Count) { Write-Host "all $($want.Count) frame(s) captured at ${t}s"; break }
}
# PID-scoped, never a name-wide kill: parallel sessions share this machine.
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force; Start-Sleep -Seconds 2 }

Write-Host "### $Arm"
foreach ($s in $want) {
    $q = Join-Path $wt "log\f1_${Arm}_span_$s.png"
    if (Test-Path $q) { Write-Host ("  span {0,-5} {1,9} bytes  {2}" -f $s, (Get-Item $q).Length, $q) }
    else              { Write-Host ("  span {0,-5} MISSING" -f $s) }
}
foreach ($f in @("race.log","engine.log")) {
    $src = Join-Path $wt "log\$f"
    if (Test-Path $src) { Copy-Item $src (Join-Path $wt "log\f1_${Arm}_$f") -Force }
}
Write-Host "### the lines this round is about"
Select-String -Path (Join-Path $wt "log\f1_${Arm}_engine.log") `
    -Pattern "plaza veto|named plaza ring|stand in a plaza|geo route: avenue" -Encoding utf8 |
    ForEach-Object { Write-Host ("  " + $_.Line.Trim()) }
Select-String -Path (Join-Path $wt "log\f1_${Arm}_race.log") -Pattern "plaza veto" -Encoding utf8 |
    ForEach-Object { Write-Host ("  " + $_.Line.Trim()) }
