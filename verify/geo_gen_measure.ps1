# geo_gen_measure.ps1 -- round 1015 F: how long does a geo place take to GENERATE and how
# much memory does the process peak at? Launches the game exactly as
# geo_realfork_run.ps1 -GenOnly does (windowed, vsync off, RT off, min graphics, all
# TD5RE_* cleared), polls the process, and stops it BY PID once MODELS.DAT has settled.
#   pwsh verify/geo_gen_measure.ps1 -Tag new -Exe td5re.exe -GeoPlace la_plata_partido -Track 62
param([string]$Tag = "m", [string]$Exe = "td5re.exe", [string]$GeoPlace = "la_plata",
      [int]$Track = 61, [int]$MaxSecs = 300)
$wt = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
Get-ChildItem env: | Where-Object { $_.Name -like 'TD5RE_*' } | ForEach-Object { Remove-Item "env:$($_.Name)" }
$env:TD5RE_GEO_PLACE = $GeoPlace
$env:TD5RE_AUTOTRACK_REUSE = "0"
$env:TD5RE_RT = "0"
$env:TD5RE_WINDOW_TITLE = "TD5RE gen-measure $Tag"
$gfx = @("--Lighting=0","--Quality=0","--SunShadows=0","--Reflections=0","--WetRoads=0",
         "--StreetLights=0","--CarLights=0","--LegacyShadows=0","--GIQuality=0","--ShadowRays=0",
         "--ReflectionQuality=0","--CarShadows=0","--VFX=0","--WorldBillboards=0","--FoliageAA=0")
$lvl = Join-Path $wt ("re\assets\levels\level{0:D3}" -f ($Track + 30))
if (Test-Path $lvl) { Remove-Item $lvl -Recurse -Force }
$args = @("--AutoRace=1","--SkipIntro=1","--DefaultTrack=$Track","--DefaultOpponents=5","--Traffic=0",
          "--Difficulty=1","--PlayerIsAI=1","--CarDamage=0","--Windowed=1","--VSync=0") + $gfx
$t0 = Get-Date
$p = Start-Process -FilePath (Join-Path $wt $Exe) -ArgumentList $args -WorkingDirectory $wt -PassThru
$models = Join-Path $lvl "MODELS.DAT"
$peakWS = 0; $peakPB = 0; $last = -1; $stable = 0; $settled = $null
while (-not $p.HasExited -and ((Get-Date) - $t0).TotalSeconds -lt $MaxSecs) {
    Start-Sleep -Milliseconds 500
    try { $p.Refresh(); $peakWS = [Math]::Max($peakWS, $p.PeakWorkingSet64); $peakPB = [Math]::Max($peakPB, $p.PeakPagefileUsage) } catch {}
    if (Test-Path $models) {
        $len = (Get-Item $models).Length
        if ($len -gt 0 -and $len -eq $last) { $stable++ } else { $stable = 0; $settled = $null }
        $last = $len
        if ($stable -ge 8) { $settled = ((Get-Date) - $t0).TotalSeconds - 4.0; break }
    }
}
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
"{0}: place={1} models_settled_after={2:N1}s MODELS.DAT={3} peak_working_set={4:N0}MB peak_private={5:N0}MB" -f $Tag, $GeoPlace, $settled, $last, ($peakWS/1MB), ($peakPB/1MB)
