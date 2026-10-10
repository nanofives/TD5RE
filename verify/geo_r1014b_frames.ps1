# geo_r1014b_frames.ps1 -- chase-cam framedumps on La Plata (slot 61 / level091)
# for round 1014 group B (cross streets, intersections, plaza ring road).
#
# One launch = one AI drive from -Offset, dumping a PNG as the player span
# passes each number in -Spans. Run it once per exe (-Exe) so a before/after
# pair is two launches of two binaries over the same route.
#
#   pwsh verify/geo_r1014b_frames.ps1 -Arm before -Exe td5re_parent.exe -Offset 20 -Spans 58,62,66
#   pwsh verify/geo_r1014b_frames.ps1 -Arm after  -Exe td5re.exe        -Offset 20 -Spans 58,62,66
#
# Standing launch rules: --Windowed=1, --VSync=0, RT off and minimum graphics,
# no sockets, window sent to the back, hard wall-clock kill BY PID. Extra
# TD5RE_* knobs for the arm go in -Env as "NAME=VALUE;NAME2=VALUE2".
param([string]$Arm = "after",
      [string]$Exe = "td5re.exe",
      [int]$Offset = 20,
      [string]$Spans = "58,62,66",
      [string]$Env = "",
      [int]$GenWait = 900,
      [int]$RaceSecs = 240,
      [int]$Regen = 1)

$wt = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
New-Item -ItemType Directory -Force -Path (Join-Path $wt "log") | Out-Null

# Clear EVERY TD5RE_* first: these persist across runs in a shell and a stale
# knob silently invalidates an A/B.
Get-ChildItem env: | Where-Object { $_.Name -like 'TD5RE_*' } |
    ForEach-Object { Remove-Item "env:$($_.Name)" }

$env:TD5RE_GEO_PLACE        = "la_plata"
$env:TD5RE_AUTOTRACK_REUSE  = "0"
$env:TD5RE_TG_DOUBLE_BUILD  = "1"
$env:TD5RE_RT               = "0"
$env:TD5RE_WINDOW_TITLE     = "TD5RE r1014b frames $Arm"
$env:TD5RE_D3D12_CAPTURE    = "1"
$env:TD5RE_FRAMEDUMP_SPANS  = $Spans
$env:TD5RE_FRAMEDUMP_SPAN_PATH = "log/r1014b_${Arm}_%d.png"
foreach ($kv in ($Env -split ';' | Where-Object { $_ })) {
    $i = $kv.IndexOf('=')
    if ($i -gt 0) { Set-Item -Path ("env:" + $kv.Substring(0, $i)) -Value $kv.Substring($i + 1) }
}

$gfx = @("--Windowed=1","--VSync=0","--CarDamage=0","--Lighting=0","--Quality=0",
         "--SunShadows=0","--Reflections=0","--WetRoads=0","--StreetLights=0",
         "--CarLights=0","--LegacyShadows=0","--GIQuality=0","--ShadowRays=0",
         "--ReflectionQuality=0","--CarShadows=0","--VFX=0","--WorldBillboards=0",
         "--FoliageAA=0")

$lvl = Join-Path $wt "re\assets\levels\level091"
if ($Regen -and (Test-Path $lvl)) { Remove-Item $lvl -Recurse -Force }
Get-ChildItem (Join-Path $wt "log") -Filter "r1014b_${Arm}_*.png" -ErrorAction SilentlyContinue |
    Remove-Item -Force -ErrorAction SilentlyContinue
foreach ($f in @("race.log","engine.log","frontend.log")) {
    $p0 = Join-Path $wt "log\$f"
    for ($t = 0; $t -lt 20 -and (Test-Path $p0); $t++) {
        try { Remove-Item $p0 -Force -ErrorAction Stop } catch { Start-Sleep -Milliseconds 500 }
    }
}

Write-Host "arm=$Arm exe=$Exe offset=$Offset spans=$Spans"
$p = Start-Process -FilePath (Join-Path $wt $Exe) `
      -ArgumentList (@("--AutoRace=1","--SkipIntro=1","--DefaultTrack=61",
                       "--PlayerIsAI=1","--AutoThrottle=1","--StartSpanOffset=$Offset",
                       "--Width=1600","--Height=900") + $gfx) `
      -WorkingDirectory $wt -PassThru
Write-Host "pid=$($p.Id)"

Start-Sleep -Seconds 2
try {
    Add-Type -Name W2 -Namespace N2 -MemberDefinition '
      [DllImport("user32.dll")] public static extern bool SetWindowPos(
        IntPtr h, IntPtr a, int x, int y, int cx, int cy, uint f);' -ErrorAction Stop
    if ($p.MainWindowHandle -ne 0) {
        [void][N2.W2]::SetWindowPos($p.MainWindowHandle, [IntPtr]1, 0,0,0,0, 0x0013)
    }
} catch { }

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
for ($t = 0; $t -lt $RaceSecs; $t += 3) {
    Start-Sleep -Seconds 3
    if ($p.HasExited) { break }
    $have = @($want | Where-Object { Test-Path (Join-Path $wt "log\r1014b_${Arm}_$_.png") })
    if ($have.Count -eq $want.Count) { Write-Host "all $($want.Count) frame(s) captured at ${t}s"; break }
}
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force; Start-Sleep -Seconds 2 }

Write-Host "### $Arm"
foreach ($s in $want) {
    $q = Join-Path $wt "log\r1014b_${Arm}_$s.png"
    if (Test-Path $q) { Write-Host ("  span {0,-5} {1}" -f $s, $q) }
    else              { Write-Host ("  span {0,-5} MISSING" -f $s) }
}
if (Test-Path $lvl) {
    foreach ($f in @("MODELS.DAT","STRIP.DAT","NETWORK.JSON","MESHTAG.BIN")) {
        $q = Join-Path $lvl $f
        if (Test-Path $q) { Write-Host ("  {0,-13} {1,10} bytes" -f $f, (Get-Item $q).Length) }
    }
    Copy-Item (Join-Path $lvl "NETWORK.JSON") (Join-Path $wt "log\r1014b_${Arm}_NETWORK.JSON") -Force -ErrorAction SilentlyContinue
}
foreach ($f in @("race.log","engine.log")) {
    $src = Join-Path $wt "log\$f"
    if (Test-Path $src) { Copy-Item $src (Join-Path $wt "log\r1014b_${Arm}_$f") -Force }
}
