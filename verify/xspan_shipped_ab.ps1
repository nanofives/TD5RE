# xspan_shipped_ab.ps1 -- prove a SHIPPED track is untouched, directly.
#
# The crossing-safe localiser (Option B) edits td5_track.c, which every shipped
# track runs through, so "shipped tracks behave identically" has to be measured
# rather than argued. The golden-trace hash net is retired by design (no row in
# k_races sets .trace_golden any more), so this does the same job by hand: run
# the SAME fixed-seed race twice, once per exe, and diff the per-tick CSVs.
#
#   pwsh verify/xspan_shipped_ab.ps1 -Track 0  -Tag moscow -Port 37184
#   pwsh verify/xspan_shipped_ab.ps1 -Track 32 -Tag td6    -Port 37184
#
# --RaceTrace=1 pins the CRT seed, so two runs of one exe are bit-identical and
# any difference between the two exes is the code change. The `frame` column is
# render-dependent (it counts real frames, which vary with scheduling) and is
# stripped before comparing -- exactly what the golden hash did.
param([int]$Track = 0, [string]$Tag = "moscow", [int]$Port = 37184,
      [string]$ExeA = "td5re_base.exe", [string]$ExeB = "td5re.exe",
      [int]$RaceSecs = 45)

$wt = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)

function Run-One([string]$exe, [string]$tag) {
    Get-ChildItem env: | Where-Object { $_.Name -like 'TD5RE_*' } |
        ForEach-Object { Remove-Item "env:$($_.Name)" }
    $env:TD5RE_CONTROL_PORT = "$Port"
    $env:TD5RE_WINDOW_TITLE = "TD5RE shipped-ab $tag"
    $env:TD5RE_RT           = "0"
    Get-ChildItem (Join-Path $wt "log") -Filter "race_trace_*.csv" -ErrorAction SilentlyContinue |
        ForEach-Object { try { Remove-Item $_.FullName -Force -ErrorAction Stop } catch {} }

    $gfx = @("--Lighting=0","--Quality=0","--SunShadows=0","--Reflections=0",
             "--WetRoads=0","--StreetLights=0","--CarLights=0","--LegacyShadows=0",
             "--GIQuality=0","--ShadowRays=0","--ReflectionQuality=0",
             "--CarShadows=0","--VFX=0","--WorldBillboards=0","--FoliageAA=0",
             "--RenderScale=50")
    $p = Start-Process -FilePath (Join-Path $wt $exe) `
          -ArgumentList (@("--AutoRace=1","--SkipIntro=1","--Control=1",
                           "--DefaultTrack=$Track","--RaceTrace=1","--RaceTraceSlot=-1",
                           "--RaceTraceMaxSimTicks=0") + $gfx) `
          -WorkingDirectory $wt -PassThru
    Write-Host "  pid=$($p.Id) exe=$exe track=$Track"
    Start-Sleep -Seconds $RaceSecs
    if (-not $p.HasExited) {
        $u = New-Object System.Net.Sockets.UdpClient
        $ep = New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Loopback, $Port)
        try { $b = [Text.Encoding]::ASCII.GetBytes("quit"); [void]$u.Send($b, $b.Length, $ep) } catch { }
        $u.Close()
        for ($j = 0; $j -lt 30 -and -not $p.HasExited; $j++) { Start-Sleep -Seconds 1 }
        if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force; Start-Sleep -Seconds 2 }
    }
    $dst = Join-Path $wt "log\ab_$tag"
    New-Item -ItemType Directory -Force -Path $dst | Out-Null
    Get-ChildItem $dst -Filter "*.csv" -ErrorAction SilentlyContinue | Remove-Item -Force
    Get-ChildItem (Join-Path $wt "log") -Filter "race_trace_*.csv" -ErrorAction SilentlyContinue |
        ForEach-Object { Copy-Item $_.FullName (Join-Path $dst $_.Name) -Force }
    return $dst
}

Write-Host "### shipped A/B track=$Track ($Tag)"
$a = Run-One $ExeA "$Tag-a"
$b = Run-One $ExeB "$Tag-b"
Write-Host "A: $a"
Write-Host "B: $b"
& python (Join-Path $wt "verify\xspan_csvdiff.py") $a $b
exit $LASTEXITCODE
