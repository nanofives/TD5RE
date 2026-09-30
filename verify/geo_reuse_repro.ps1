# geo_reuse_repro.ps1 -- item 11 repro: launch the auto track with a given env,
# let it generate + race briefly, quit cleanly, report the level files and the
# REUSE / TEXTURES lines of race.log + engine.log. Does NOT wipe level090 unless
# -Wipe, so two calls in a row exercise the GENSTAMP reuse path.
#
#   pwsh verify/geo_reuse_repro.ps1 -Tag a -Wipe -Extra @{TD5RE_AUTOTRACK_STREAM="0"}
#   pwsh verify/geo_reuse_repro.ps1 -Tag b       -Extra @{TD5RE_AUTOTRACK_STREAM="0"}
#
# RT off + minimum graphics, per the standing rule for every non-selftest run.
param([string]$Tag = "run", [int]$Port = 37194, [string]$Seed = "20260901",
      [hashtable]$Extra = @{}, [switch]$Wipe, [int]$RaceSecs = 15, [int]$GenWait = 600)

$wt = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
Get-ChildItem env: | Where-Object { $_.Name -like 'TD5RE_*' } | ForEach-Object { Remove-Item "env:$($_.Name)" }
$env:TD5RE_AUTOTRACK_SEED = $Seed
$env:TD5RE_CONTROL_PORT   = "$Port"
$env:TD5RE_WINDOW_TITLE   = "TD5RE reuse $Tag"
$env:TD5RE_RT             = "0"
foreach ($k in $Extra.Keys) { Set-Item "env:$k" $Extra[$k] }

$gfx = @("--Lighting=0","--Quality=0","--SunShadows=0","--Reflections=0",
         "--WetRoads=0","--StreetLights=0","--CarLights=0","--LegacyShadows=0",
         "--GIQuality=0","--ShadowRays=0","--ReflectionQuality=0",
         "--CarShadows=0","--VFX=0","--WorldBillboards=0","--FoliageAA=0",
         "--RenderScale=50")

$lvl = Join-Path $wt "re\assets\levels\level090"
if ($Wipe -and (Test-Path $lvl)) { Remove-Item $lvl -Recurse -Force }
foreach ($f in @("race.log","engine.log")) {
    $p0 = Join-Path $wt "log\$f"
    for ($t = 0; $t -lt 20 -and (Test-Path $p0); $t++) {
        try { Remove-Item $p0 -Force -ErrorAction Stop } catch { Start-Sleep -Milliseconds 500 }
    }
}

$p = Start-Process -FilePath (Join-Path $wt "td5re.exe") `
      -ArgumentList (@("--AutoRace=1","--SkipIntro=1","--Control=1","--DefaultTrack=60") + $gfx) `
      -WorkingDirectory $wt -PassThru
Write-Host "pid=$($p.Id) tag=$Tag"

# Wait for the build: MODELS.DAT present and stable for 4 s (race.log only
# flushes on a clean shutdown, so it cannot be polled). A reused build finds
# MODELS.DAT already there and settles at once.
$models = Join-Path $lvl "MODELS.DAT"
$last = -1; $stable = 0; $started = $false
for ($i = 0; $i -lt $GenWait; $i++) {
    Start-Sleep -Seconds 1
    if ($p.HasExited) { break }
    if (Test-Path $models) {
        $len = (Get-Item $models).Length
        if ($len -gt 0 -and $len -eq $last) { $stable++ } else { $stable = 0 }
        $last = $len
        if ($stable -ge 4) { $started = $true; break }
    }
}
Write-Host "models settled=$started after ${i}s"
if (-not $p.HasExited) { Start-Sleep -Seconds $RaceSecs }
if (-not $p.HasExited) {
    $u = New-Object System.Net.Sockets.UdpClient
    $ep = New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Loopback, $Port)
    try { $b = [Text.Encoding]::ASCII.GetBytes("quit"); [void]$u.Send($b, $b.Length, $ep) } catch { }
    $u.Close()
    for ($j = 0; $j -lt 30 -and -not $p.HasExited; $j++) { Start-Sleep -Seconds 1 }
    if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force; Start-Sleep -Seconds 2 }
}
foreach ($f in @("race.log","engine.log")) {
    $src = Join-Path $wt "log\$f"
    if (Test-Path $src) { Copy-Item $src (Join-Path $wt "log\reuse_${Tag}_$f") -Force }
}
Write-Host "### $Tag"
foreach ($f in @("race.log","engine.log")) {
    $lg = Join-Path $wt "log\reuse_${Tag}_$f"
    if (Test-Path $lg) {
        Select-String -Path $lg -Pattern "REUSED|STREAMED build|TEXTURES.DAT|parsed MODELS.DAT|no MODELS.DAT|tgstream|scenery" |
            Select-Object -First 25 | ForEach-Object { Write-Host ("  [$f] " + $_.Line) }
    }
}
foreach ($f in @("STRIP.DAT","MODELS.DAT","TEXTURES.DAT","GENSTAMP.TXT")) {
    $p2 = Join-Path $lvl $f
    if (Test-Path $p2) {
        $h = (Get-FileHash $p2 -Algorithm SHA256).Hash.Substring(0,16)
        Write-Host ("  {0,-13} {1,10} bytes  {2}  {3}" -f $f, (Get-Item $p2).Length, $h, (Get-Item $p2).LastWriteTime.ToString("HH:mm:ss"))
    } else { Write-Host ("  {0,-13} MISSING" -f $f) }
}
$gs = Join-Path $lvl "GENSTAMP.TXT"
if (Test-Path $gs) { Write-Host ("  stamp: " + (Get-Content $gs -Raw).Trim()) }
