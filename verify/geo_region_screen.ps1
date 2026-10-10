# geo_region_screen.ps1 -- round 1015 F: drive the GEOSPATIAL TRACK GENERATOR on a
# whole-area (tiled) place at a given spot, with ZERO outbound requests.
#
#   pwsh verify/geo_region_screen.ps1 -Tag citybell -Pts "-34.8699,-58.0495;-34.8681,-58.0307" -Mode shot
#   pwsh verify/geo_region_screen.ps1 -Tag citybell -Pts "..." -Mode build -Spans "50,300,600"
#
# Mode shot  : open screen 56 centred on the first point, framedump the map, stop.
# Mode build : press BUILD TRACK once (TD5RE_GEO_AUTOBUILD), race the slot it
#              registers, framedump the given spans, stop.
# TD5RE_GEO_TILES_OFFLINE=1 starts no tile worker, so the map is drawn over
# placeholders and the test never touches a tile server. Standing launch rules:
# --Windowed=1, --VSync=0, RT off, min graphics, every TD5RE_* cleared, a hard
# wall-clock kill BY PID.
param([string]$Slug = "la_plata_partido", [string]$Tag = "scr", [string]$Pts,
      [ValidateSet("shot","build")][string]$Mode = "shot",
      [string]$Spans = "50,300", [int]$WaitSecs = 420, [int]$SimTicks = 3000,
      [int]$ShotSecs = 25)
$wt = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
Get-ChildItem env: | Where-Object { $_.Name -like 'TD5RE_*' } | ForEach-Object { Remove-Item "env:$($_.Name)" }
$env:TD5RE_GEO_PLACE = $Slug
$env:TD5RE_GEO_SEED_LATLON = $Pts
$env:TD5RE_GEO_TILES_OFFLINE = "1"
$env:TD5RE_RT = "0"
$env:TD5RE_D3D12_CAPTURE = "1"
$env:TD5RE_WINDOW_TITLE = "TD5RE geo-region $Tag"
$out = Join-Path $wt "log"
New-Item -ItemType Directory -Force -Path $out | Out-Null
$gfx = @("--Windowed=1","--VSync=0","--Lighting=0","--Quality=0","--SunShadows=0","--Reflections=0",
         "--WetRoads=0","--StreetLights=0","--CarLights=0","--LegacyShadows=0","--GIQuality=0",
         "--ShadowRays=0","--ReflectionQuality=0","--CarShadows=0","--VFX=0","--WorldBillboards=0","--FoliageAA=0")
$shot = Join-Path $out "${Tag}_map.png"
if ($Mode -eq "shot") {
    $env:TD5RE_FRAMEDUMP = $shot
    Remove-Item $shot -ErrorAction SilentlyContinue
} else {
    $env:TD5RE_GEO_AUTOBUILD = "1"
    $env:TD5RE_FRAMEDUMP_SPANS = $Spans
    $env:TD5RE_FRAMEDUMP_SPAN_PATH = "log/${Tag}_span_%d.png"
    Remove-Item (Join-Path $wt "re\assets\levels\level062") -Recurse -Force -ErrorAction SilentlyContinue
}
foreach ($f in @("race.log","engine.log","frontend.log")) {
    $p0 = Join-Path $out $f
    for ($t = 0; $t -lt 20 -and (Test-Path $p0); $t++) { try { Remove-Item $p0 -Force -ErrorAction Stop } catch { Start-Sleep -Milliseconds 500 } }
}
$a = @("--SkipIntro=1","--StartScreen=56","--StartScreenDirect=1","--AutoThrottle=1","--RaceTraceMaxSimTicks=$SimTicks") + $gfx
$p = Start-Process -FilePath (Join-Path $wt "td5re.exe") -ArgumentList $a -WorkingDirectory $wt -PassThru
$sw = [Diagnostics.Stopwatch]::StartNew()
if ($Mode -eq "shot") {
    while (-not $p.HasExited -and $sw.Elapsed.TotalSeconds -lt $ShotSecs) { Start-Sleep -Seconds 1 }
} else {
    $want = $Spans -split ',' | ForEach-Object { Join-Path $wt ("log/${Tag}_span_{0}.png" -f $_.Trim()) }
    while (-not $p.HasExited -and $sw.Elapsed.TotalSeconds -lt $WaitSecs) {
        Start-Sleep -Seconds 3
        if (@($want | Where-Object { Test-Path $_ }).Count -eq $want.Count) { break }
    }
}
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force; Start-Sleep -Seconds 3 }
"{0}: mode={1} elapsed={2:N0}s" -f $Tag, $Mode, $sw.Elapsed.TotalSeconds
Get-ChildItem $out -Filter "${Tag}_*.png" | ForEach-Object { "  {0,-28} {1,9} bytes" -f $_.Name, $_.Length }
foreach ($f in @("frontend.log","race.log","engine.log")) {
    $q = Join-Path $out $f
    if (-not (Test-Path $q)) { continue }
    Copy-Item $q (Join-Path $out "${Tag}_$f") -Force
    Select-String -Path $q -Pattern "AUTOBUILD|geo route: committed|COMMIT REFUSED|tiled graph window|geo tiles:|SEED_LATLON|NO MAP DATA|outside|geo: loaded" |
        Select-Object -First 14 | ForEach-Object { "  [$f] " + $_.Line.Substring(0, [Math]::Min(230, $_.Line.Length)) }
}
