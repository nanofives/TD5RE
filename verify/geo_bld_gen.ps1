# geo_bld_gen.ps1 -- regenerate La Plata (slot 61 / level091) with the worktree exe
# and stop as soon as MODELS.DAT has settled, so verify/geo_bld_audit.py can read
# it. ROUND 1014 C. No race is driven; the process is killed BY PID.
#
#   pwsh verify/geo_bld_gen.ps1 -Tag before
#   pwsh verify/geo_bld_gen.ps1 -Tag after -Frames 1 -Spans 536,540
#
# Standing rules applied: --Windowed=1, --VSync=0, RT off + minimum graphics,
# window sent to the back, all TD5RE_* cleared first (they persist across runs),
# no sockets, hard wall-clock kill by PID.
param([string]$Tag = "after",
      [string]$Spans = "",
      [int]$StartOffset = 0,
      [int]$Throttle = 0,
      [int]$GenWait = 900,
      [int]$RaceSecs = 120,
      [string]$Exe = "td5re.exe",
      [string]$Env = "")

$wt = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)

Get-ChildItem env: | Where-Object { $_.Name -like 'TD5RE_*' } |
    ForEach-Object { Remove-Item "env:$($_.Name)" }
$env:TD5RE_GEO_PLACE       = "la_plata"
$env:TD5RE_AUTOTRACK_REUSE = "0"
$env:TD5RE_RT              = "0"
$env:TD5RE_WINDOW_TITLE    = "TD5RE bld $Tag"
foreach ($kv in ($Env -split ';' | Where-Object { $_ })) {
    $k, $v = $kv -split '=', 2
    Set-Item -Path "env:$k" -Value $v
}
if ($Spans) {
    $env:TD5RE_D3D12_CAPTURE       = "1"
    $env:TD5RE_FRAMEDUMP_SPANS     = $Spans
    $env:TD5RE_FRAMEDUMP_SPAN_PATH = "log/bld_${Tag}_span_%d.png"
}

$gfx = @("--Windowed=1","--VSync=0","--CarDamage=0","--Lighting=0","--Quality=0",
         "--SunShadows=0","--Reflections=0","--WetRoads=0","--StreetLights=0",
         "--CarLights=0","--LegacyShadows=0","--GIQuality=0","--ShadowRays=0",
         "--ReflectionQuality=0","--CarShadows=0","--VFX=0","--WorldBillboards=0",
         "--FoliageAA=0")

$lvl = Join-Path $wt "re\assets\levels\level091"
if (Test-Path $lvl) { Remove-Item $lvl -Recurse -Force }
foreach ($f in @("race.log","engine.log","frontend.log")) {
    $p0 = Join-Path $wt "log\$f"
    for ($t = 0; $t -lt 20 -and (Test-Path $p0); $t++) {
        try { Remove-Item $p0 -Force -ErrorAction Stop } catch { Start-Sleep -Milliseconds 500 }
    }
}
$want = @()
if ($Spans) {
    $want = ($Spans -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ })
    foreach ($s in $want) {
        $q = Join-Path $wt "log\bld_${Tag}_span_$s.png"
        if (Test-Path $q) { Remove-Item -LiteralPath $q -Force -ErrorAction SilentlyContinue }
    }
}

$args2 = @("--AutoRace=1","--SkipIntro=1","--DefaultTrack=61","--Logging=1")
if ($Throttle) { $args2 += @("--PlayerIsAI=1","--AutoThrottle=1") }
if ($StartOffset -gt 0) { $args2 += "--StartSpanOffset=$StartOffset" }
Write-Host "tag=$Tag spans=$Spans startOffset=$StartOffset exe=$Exe"
$p = Start-Process -FilePath (Join-Path $wt $Exe) -ArgumentList ($args2 + $gfx) `
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
        if ($stable -ge 6) { break }
    }
}
Write-Host "models settled after ${i}s ($last bytes)"

if ($want.Count -gt 0) {
    for ($t = 0; $t -lt $RaceSecs; $t += 5) {
        Start-Sleep -Seconds 5
        if ($p.HasExited) { Write-Host "exe exited at ${t}s"; break }
        $have = @($want | Where-Object { Test-Path (Join-Path $wt "log\bld_${Tag}_span_$_.png") })
        if ($have.Count -eq $want.Count) { Write-Host "all frames at ${t}s"; break }
    }
}
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force; Start-Sleep -Seconds 2 }

$out = Join-Path $wt "log\bld_$Tag"
New-Item -ItemType Directory -Force $out | Out-Null
foreach ($f in @("MODELS.DAT","MESHTAG.BIN","GENSTAMP.TXT","STRIP.DAT","LEFT.TRK","RIGHT.TRK","TRAFFIC.BUS")) {
    $s = Join-Path $lvl $f
    if (Test-Path $s) { Copy-Item $s (Join-Path $out $f) -Force }
}
foreach ($f in @("race.log","engine.log")) {
    $s = Join-Path $wt "log\$f"
    if (Test-Path $s) { Copy-Item $s (Join-Path $out $f) -Force }
}
foreach ($s in $want) {
    $q = Join-Path $wt "log\bld_${Tag}_span_$s.png"
    if (Test-Path $q) { Write-Host ("  span {0,-5} {1,9} bytes" -f $s, (Get-Item $q).Length) }
    else              { Write-Host ("  span {0,-5} MISSING" -f $s) }
}
Write-Host "copied level files to $out"
