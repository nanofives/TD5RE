# geo_d2_avenue_frames.ps1 -- before/after framedumps for round 1012 D2
# (divided avenues, median width, the 30 m avenida, plaza set pieces) on the
# La Plata geo track (slot 61 / level091), over Mariano's own saved route.
#
# Both arms run the SAME exe; the "before" arm turns each of this round's
# changes off through its own knob, so the only difference between the two sets
# of PNGs is the fix:
#
#   item 2  TD5RE_GEO_DIVIDED_LANES=0      (both carriageways back on the
#                                           round-1011 place floor, 3 lanes)
#   item 3  TD5RE_GEO_AVENUE_WIDE_X10=10   (an undivided avenida back to one
#                                           carriageway's worth of asphalt)
#   item 4  TD5RE_GEO_PREFAB_PLAZA=0       (shipped set pieces back in squares)
#
# Items 1's two gates have no knob of their own: they decide WHICH runs become
# avenues at all, which is read off the log (avenue span ranges) rather than
# off a pixel.
#
# Standing launch rules, all applied: --Windowed=1, --VSync=0, RT off and
# minimum graphics, no sockets, a hard wall-clock kill BY PID.
#
#   pwsh verify/geo_d2_avenue_frames.ps1 -Arm before
#   pwsh verify/geo_d2_avenue_frames.ps1 -Arm after
param([ValidateSet("before","after")][string]$Arm = "after",
      # 20  = Plaza Miguel de Azcuenaga, the start plaza (set-piece site)
      # 60/150/250 = Diagonal 73, inside the divided run (median)
      # 300 = Calle 14/54, an undivided calle, for contrast
      # 600 = Avenida 13 between the two divided runs (undivided avenida)
      [string]$Spans = "20,60,150,250,300,600",
      [int]$GenWait = 900,
      [int]$RaceSecs = 600)

$wt = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)

# Clear EVERY TD5RE_* first: these persist across runs in a shell and a stale
# knob silently invalidates an A/B.
Get-ChildItem env: | Where-Object { $_.Name -like 'TD5RE_*' } |
    ForEach-Object { Remove-Item "env:$($_.Name)" }

$env:TD5RE_GEO_PLACE           = "la_plata"
$env:TD5RE_AUTOTRACK_REUSE     = "0"
$env:TD5RE_RT                  = "0"
$env:TD5RE_WINDOW_TITLE        = "TD5RE d2av $Arm"
$env:TD5RE_D3D12_CAPTURE       = "1"
$env:TD5RE_FRAMEDUMP_SPANS     = $Spans
$env:TD5RE_FRAMEDUMP_SPAN_PATH = "log/d2av_${Arm}_span_%d.png"

if ($Arm -eq "before") {
    $env:TD5RE_GEO_DIVIDED_LANES    = "0"
    $env:TD5RE_GEO_AVENUE_WIDE_X10  = "10"
    $env:TD5RE_GEO_PREFAB_PLAZA     = "0"
}

$gfx = @("--Windowed=1","--VSync=0","--CarDamage=0","--Lighting=0","--Quality=0",
         "--SunShadows=0","--Reflections=0","--WetRoads=0","--StreetLights=0",
         "--CarLights=0","--LegacyShadows=0","--GIQuality=0","--ShadowRays=0",
         "--ReflectionQuality=0","--CarShadows=0","--VFX=0","--WorldBillboards=0",
         "--FoliageAA=0")

$lvl = Join-Path $wt "re\assets\levels\level091"
if (Test-Path $lvl) { Remove-Item $lvl -Recurse -Force }
# Delete ONLY the spans this run is about to capture. A blanket wildcard delete
# here threw away four earlier frames that were still the evidence for a
# different span, and the run looked clean because it reported only its own.
foreach ($s in ($Spans -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ })) {
    $q = Join-Path $wt "log\d2av_${Arm}_span_$s.png"
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

$want = ($Spans -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ })
for ($t = 0; $t -lt $RaceSecs; $t += 5) {
    Start-Sleep -Seconds 5
    if ($p.HasExited) { break }
    $have = @($want | Where-Object { Test-Path (Join-Path $wt "log\d2av_${Arm}_span_$_.png") })
    if ($have.Count -eq $want.Count) { Write-Host "all $($want.Count) frame(s) captured at ${t}s"; break }
}
# PID-scoped, never a name-wide kill: parallel sessions share this machine.
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force; Start-Sleep -Seconds 2 }

Write-Host "### $Arm"
foreach ($s in $want) {
    $q = Join-Path $wt "log\d2av_${Arm}_span_$s.png"
    if (Test-Path $q) { Write-Host ("  span {0,-5} {1,9} bytes  {2}" -f $s, (Get-Item $q).Length, $q) }
    else              { Write-Host ("  span {0,-5} MISSING" -f $s) }
}
if (Test-Path $lvl) {
    foreach ($f in @("MODELS.DAT","TEXTURES.DAT","STRIP.DAT")) {
        $q = Join-Path $lvl $f
        if (Test-Path $q) { Write-Host ("  {0,-13} {1,10} bytes" -f $f, (Get-Item $q).Length) }
    }
}
foreach ($f in @("race.log","engine.log")) {
    $src = Join-Path $wt "log\$f"
    if (Test-Path $src) { Copy-Item $src (Join-Path $wt "log\d2av_${Arm}_$f") -Force }
}
Write-Host "### the two lines this round is about"
Select-String -Path (Join-Path $wt "log\d2av_${Arm}_engine.log") -Pattern "GEO AVENUE|\[PREFAB\] \d+ placed" -Encoding utf8 |
    ForEach-Object { Write-Host ("  " + $_.Line.Trim()) }
