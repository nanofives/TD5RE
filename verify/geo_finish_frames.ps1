# geo_finish_frames.ps1 -- before/after framedumps of the FINISH on a geo track
# (round 1013 F3: "i see there's no proper finish line"), over Mariano's own
# saved La Plata route (slot 61 / level091).
#
# The player slot is driven by the AI (--PlayerIsAI=1 --AutoThrottle=1) and
# dropped near the end of the route with --StartSpanOffset, because full gas from
# the grid plows into traffic and walls after ~70 spans (a late span is never
# reached any other way). The framedump fires when the PLAYER's span reaches each
# listed span, so list the spans around the finish you want to look at.
#
#   pwsh verify/geo_finish_frames.ps1 -Tag before -Spans 930,940,945,950 -StartOffset 915
#   pwsh verify/geo_finish_frames.ps1 -Tag after  -Spans 920,940,951,960,1000 -StartOffset 900
#
# Standing launch rules, all applied: --Windowed=1, --VSync=0, RT off and
# minimum graphics, no sockets, the window sent to the back, and a hard
# wall-clock kill BY PID (sibling /fix sessions run td5re.exe on this machine).
param([string]$Tag = "after",
      [string]$Spans = "940,945,950",
      [int]$StartOffset = 915,
      [int]$GenWait = 900,
      [int]$RaceSecs = 240,
      # 1 = let the race run until the exe quits on its own (race end + results);
      # 0 = stop as soon as every listed frame exists.
      [int]$RunToEnd = 0,
      [string]$Exe = "td5re.exe")

$wt = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)

# Clear EVERY TD5RE_* first: these persist across runs in a shell and a stale
# knob silently invalidates an A/B.
Get-ChildItem env: | Where-Object { $_.Name -like 'TD5RE_*' } |
    ForEach-Object { Remove-Item "env:$($_.Name)" }

$env:TD5RE_GEO_PLACE           = "la_plata"
$env:TD5RE_AUTOTRACK_REUSE     = "0"
$env:TD5RE_RT                  = "0"
$env:TD5RE_WINDOW_TITLE        = "TD5RE finish $Tag"
$env:TD5RE_D3D12_CAPTURE       = "1"
$env:TD5RE_FRAMEDUMP_SPANS     = $Spans
$env:TD5RE_FRAMEDUMP_SPAN_PATH = "log/finish_${Tag}_span_%d.png"

$gfx = @("--Windowed=1","--VSync=0","--CarDamage=0","--Lighting=0","--Quality=0",
         "--SunShadows=0","--Reflections=0","--WetRoads=0","--StreetLights=0",
         "--CarLights=0","--LegacyShadows=0","--GIQuality=0","--ShadowRays=0",
         "--ReflectionQuality=0","--CarShadows=0","--VFX=0","--WorldBillboards=0",
         "--FoliageAA=0")

$lvl = Join-Path $wt "re\assets\levels\level091"
if (Test-Path $lvl) { Remove-Item $lvl -Recurse -Force }
$want = ($Spans -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ })
foreach ($s in $want) {
    $q = Join-Path $wt "log\finish_${Tag}_span_$s.png"
    if (Test-Path $q) { Remove-Item -LiteralPath $q -Force -ErrorAction SilentlyContinue }
}
foreach ($f in @("race.log","engine.log","frontend.log")) {
    $p0 = Join-Path $wt "log\$f"
    for ($t = 0; $t -lt 20 -and (Test-Path $p0); $t++) {
        try { Remove-Item $p0 -Force -ErrorAction Stop } catch { Start-Sleep -Milliseconds 500 }
    }
}

Write-Host "tag=$Tag spans=$Spans startOffset=$StartOffset exe=$Exe"
$p = Start-Process -FilePath (Join-Path $wt $Exe) `
      -ArgumentList (@("--AutoRace=1","--SkipIntro=1","--DefaultTrack=61",
                       "--PlayerIsAI=1","--AutoThrottle=1",
                       "--StartSpanOffset=$StartOffset","--Logging=1") + $gfx) `
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

for ($t = 0; $t -lt $RaceSecs; $t += 5) {
    Start-Sleep -Seconds 5
    if ($p.HasExited) { Write-Host "exe exited on its own at ${t}s"; break }
    $have = @($want | Where-Object { Test-Path (Join-Path $wt "log\finish_${Tag}_span_$_.png") })
    if (-not $RunToEnd -and $have.Count -eq $want.Count) {
        Write-Host "all $($want.Count) frame(s) captured at ${t}s"; break
    }
}
# PID-scoped, never a name-wide kill: parallel sessions share this machine.
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force; Start-Sleep -Seconds 2 }

Write-Host "### $Tag"
foreach ($s in $want) {
    $q = Join-Path $wt "log\finish_${Tag}_span_$s.png"
    if (Test-Path $q) { Write-Host ("  span {0,-5} {1,9} bytes  {2}" -f $s, (Get-Item $q).Length, $q) }
    else              { Write-Host ("  span {0,-5} MISSING" -f $s) }
}
foreach ($f in @("race.log","engine.log")) {
    $src = Join-Path $wt "log\$f"
    if (Test-Path $src) { Copy-Item $src (Join-Path $wt "log\finish_${Tag}_$f") -Force }
}
Write-Host "### finish lines of the log"
foreach ($f in @("race","engine")) {
    $lg = Join-Path $wt "log\finish_${Tag}_$f.log"
    if (Test-Path $lg) {
        Select-String -Path $lg -Pattern "finish span|registry finish|Actor finish|Checkpoint record|race complete|RACE OVER|ai_finish_stop|\[GEO\] road follows" -Encoding utf8 |
            Select-Object -First 12 | ForEach-Object { Write-Host ("  " + $_.Line.Trim()) }
    }
}
