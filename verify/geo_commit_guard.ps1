# geo_commit_guard.ps1 -- round 1008, N1 CACHE.
#
# Proves the commit GUARD actually fires, rather than only compiling. Feeds
# td5_geo_route_commit the one input that produced the round-1007 disaster -- a
# PLACE.JSON whose bbox has collapsed to a point -- and checks that it REFUSES,
# writes nothing, and names the reason.
#
# Under the old code this exact input is what silently produced a 2x2 grid and
# 76-byte rasters. There was no guard, so the only symptom was a city made of
# water several minutes later.
#
# SAFETY. It edits the real PLACE.JSON of the real place, because the
# TD5RE_GEO_ROUTE_TEST harness is hardcoded to la_plata. The original is copied
# aside first and restored at the end, and the restore is verified by hash --
# the script fails loudly if the source does not come back byte-identical.
#   -Mode bbox    collapse PLACE.JSON's bbox to a point. Refuses, but EARLY:
#                 the router filters clicks on the same bbox, so routing fails
#                 before the commit is reached ("NO USABLE ROUTE TO SAVE").
#                 A real refusal, just not the grid guard.
#   -Mode height  blow up HEIGHT.R16's cell size in the 8-byte header field at
#                 offset 36. The ROUTER never reads HEIGHT.R16, so routing
#                 still succeeds and the commit is reached with a grid that
#                 sizes to 2x2 -- the exact shape of the round-1007 collapse.
param([string]$Slug = "la_plata",
      [ValidateSet("bbox","height")][string]$Mode = "height",
      [int]$WaitSecs = 300)

$ErrorActionPreference = "Stop"
$wt    = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$src   = Join-Path $wt "re\assets\geo\$Slug"
$drv    = Join-Path $src "_route"
$target = Join-Path $src $(if ($Mode -eq "bbox") { "PLACE.JSON" } else { "HEIGHT.R16" })
$bak    = Join-Path $env:TEMP "$(Split-Path -Leaf $target).n1guard.bak"

$origHash = (Get-FileHash $target -Algorithm SHA256).Hash
Copy-Item $target $bak -Force
Write-Host "mode=$Mode  saved $(Split-Path -Leaf $target) ($($origHash.Substring(0,24))) to $bak"

try {
    if ($Mode -eq "bbox") {
        $j = Get-Content $target -Raw | ConvertFrom-Json
        $c = $j.centre
        $j.bbox.north = $c.lat; $j.bbox.south = $c.lat
        $j.bbox.east  = $c.lon; $j.bbox.west  = $c.lon
        $j | ConvertTo-Json -Depth 20 | Set-Content $target -Encoding UTF8
        Write-Host "bbox collapsed to the centre point ($($c.lat), $($c.lon))"
    } else {
        # TD5GEOR1 header: cell is the float64 at byte offset 36. Everything
        # else about the raster stays valid, so the ONLY thing that can catch
        # this is the grid guard.
        $b = [IO.File]::ReadAllBytes($target)
        $was = [BitConverter]::ToDouble($b, 36)
        [Array]::Copy([BitConverter]::GetBytes([double]1e9), 0, $b, 36, 8)
        [IO.File]::WriteAllBytes($target, $b)
        Write-Host "HEIGHT.R16 cell $was -> 1e9 units (grid must size to 2x2)"
    }

    if (Test-Path $drv) { Remove-Item $drv -Recurse -Force }
    Write-Host "derived frame removed; a refusal must leave it absent"

    Get-ChildItem env: | Where-Object { $_.Name -like 'TD5RE_*' } |
        ForEach-Object { Remove-Item "env:$($_.Name)" }
    $env:TD5RE_GEO_PLACE       = $Slug
    $env:TD5RE_GEO_ROUTE_TEST  = "3"      # route La Plata and COMMIT it
    $env:TD5RE_RT              = "0"
    $env:TD5RE_WINDOW_TITLE    = "TD5RE n1 guard"

    $log = Join-Path $env:TEMP "n1_guard_stdout.txt"
    $p = Start-Process -FilePath (Join-Path $wt "td5re.exe") `
            -ArgumentList @("--Windowed=1","--VSync=0","--SkipIntro=1","--Lighting=0","--Quality=0") `
            -WorkingDirectory $wt -PassThru -RedirectStandardOutput $log -NoNewWindow
    Write-Host "pid=$($p.Id)"
    $sw = [Diagnostics.Stopwatch]::StartNew()
    while (-not $p.HasExited -and $sw.Elapsed.TotalSeconds -lt $WaitSecs) { Start-Sleep -Seconds 3 }
    if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force; Start-Sleep -Seconds 2 }

    Write-Host "`n=== did it refuse? ==="
    foreach ($f in @((Join-Path $wt "log\engine.log"), $log)) {
        if (Test-Path $f) {
            Select-String -Path $f -Pattern "COMMIT REFUSED|geo route: committed" |
                ForEach-Object { "  " + $_.Line }
        }
    }
    Write-Host "`n=== did it write anything? ==="
    if (Test-Path $drv) {
        Get-ChildItem $drv -File | ForEach-Object { "  WROTE $($_.Name) ($($_.Length) bytes)" }
        if (Test-Path (Join-Path $drv "DERIVED.OK")) { Write-Host "  *** STAMPED -- readers would pick this up ***" }
        else { Write-Host "  no stamp: readers ignore this dir entirely" }
    } else { Write-Host "  nothing written -- no derived frame exists" }
}
finally {
    Copy-Item $bak $target -Force
    $back = (Get-FileHash $target -Algorithm SHA256).Hash
    if ($back -eq $origHash) { Write-Host "`n$(Split-Path -Leaf $target) restored byte-identical ($($back.Substring(0,24)))" }
    else { Write-Host "`n*** RESTORE FAILED: $back != $origHash -- recover from $bak ***" }
}
