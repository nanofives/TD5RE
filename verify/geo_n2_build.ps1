# geo_n2_build.ps1 -- round 1022 N2: build a geo ROUTE in a PRIVATE run dir (never the shared _route/) and keep the
# resulting _route/ + the generator's fork / loop lines, so a route with both real forks and block loops can be
# found and then raced with verify/geo_fork_validate.ps1 -RouteDir.
#
#   pwsh verify/geo_n2_build.ps1 -Name a1 -Pts "-34.9230,-57.9778;-34.9244,-57.9500;-34.9230,-57.9386"
#
# Output: verify/out/n2_routes/<Name>/_route (the derived frame), summary.txt (forks, loop candidates, kept loops).
# Zero outbound requests (TD5RE_GEO_TILES_OFFLINE=1), PID-scoped stop, --Windowed=1, RT off, min graphics.
param([Parameter(Mandatory)][string]$Name, [Parameter(Mandatory)][string]$Pts,
      [string]$Place = "la_plata_partido", [int]$WaitSecs = 600, [string]$Extra = "")
$ErrorActionPreference = "Stop"
$wt = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$outRoot = Join-Path $wt "verify\out\n2_routes"
$rd = Join-Path $outRoot "run_$Name"
$keep = Join-Path $outRoot $Name
New-Item -ItemType Directory -Force -Path $outRoot | Out-Null
$assets = Join-Path $wt "re\assets"

function New-Junction([string]$link, [string]$target) { New-Item -ItemType Junction -Path $link -Target $target | Out-Null }
function Remove-Tree([string]$p) {
    if (-not [IO.Directory]::Exists($p)) { return }
    foreach ($e in [IO.Directory]::GetFileSystemEntries($p)) {
        $attr = [IO.File]::GetAttributes($e)
        $isDir = ($attr -band [IO.FileAttributes]::Directory) -ne 0
        $isLink = ($attr -band [IO.FileAttributes]::ReparsePoint) -ne 0
        if ($isLink) { if ($isDir) { [IO.Directory]::Delete($e, $false) } else { [IO.File]::Delete($e) } }
        elseif ($isDir) { Remove-Tree $e }
        else { if ($attr -band [IO.FileAttributes]::ReadOnly) { [IO.File]::SetAttributes($e, [IO.FileAttributes]::Normal) }; [IO.File]::Delete($e) }
    }
    [IO.Directory]::Delete($p, $false)
}
if (Test-Path $rd) { Remove-Tree $rd }
if (Test-Path $keep) { Remove-Tree $keep }
New-Item -ItemType Directory -Force -Path (Join-Path $rd "re\assets\levels") | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $rd "re\assets\geo\$Place") | Out-Null
Copy-Item (Join-Path $wt "td5re.exe") (Join-Path $rd "td5re.exe") -Force
$ra = Join-Path $rd "re\assets"
foreach ($c in Get-ChildItem $assets -Force) {
    if ($c.Name -in @("levels", "geo")) { continue }
    if ($c.PSIsContainer) { New-Junction (Join-Path $ra $c.Name) $c.FullName } else { Copy-Item $c.FullName (Join-Path $ra $c.Name) }
}
foreach ($c in Get-ChildItem (Join-Path $assets "levels") -Force) {
    if ($c.Name -in @("level090", "level091", "level092")) { continue }
    if ($c.PSIsContainer) { New-Junction (Join-Path $ra "levels\$($c.Name)") $c.FullName } else { Copy-Item $c.FullName (Join-Path $ra "levels\$($c.Name)") }
}
Set-Content -Path (Join-Path $ra "geo\SELECTED.TXT") -Value $Place -NoNewline
$src = Join-Path $assets "geo\$Place"; $dst = Join-Path $ra "geo\$Place"
foreach ($c in Get-ChildItem $src -Force) {
    if ($c.Name -eq "_route") { Copy-Item $c.FullName (Join-Path $dst "_route") -Recurse }
    elseif ($c.PSIsContainer) { New-Junction (Join-Path $dst $c.Name) $c.FullName }
    else { New-Item -ItemType HardLink -Path (Join-Path $dst $c.Name) -Target $c.FullName | Out-Null }
}
Get-ChildItem env: | Where-Object { $_.Name -like 'TD5RE_*' } | ForEach-Object { Remove-Item "env:$($_.Name)" }
$env:TD5RE_GEO_PLACE = $Place
$env:TD5RE_GEO_SEED_LATLON = $Pts
$env:TD5RE_GEO_TILES_OFFLINE = "1"
$env:TD5RE_GEO_AUTOBUILD = "1"
$env:TD5RE_GEO_FORK_DIAG = "1"
$env:TD5RE_RT = "0"
$env:TD5RE_WINDOW_TITLE = "TD5RE n2-build $Name"
foreach ($kv in $Extra.Split(',')) { $p = $kv.Split('=', 2); if ($p.Count -eq 2 -and $p[0].Trim() -ne "") { Set-Item "env:$($p[0].Trim())" $p[1] } }
$gfx = @("--Windowed=1","--VSync=0","--Lighting=0","--Quality=0","--SunShadows=0","--Reflections=0","--WetRoads=0","--StreetLights=0",
         "--CarLights=0","--LegacyShadows=0","--GIQuality=0","--ShadowRays=0","--ReflectionQuality=0","--CarShadows=0","--VFX=0",
         "--WorldBillboards=0","--FoliageAA=0")
$a = @("--SkipIntro=1","--StartScreen=56","--StartScreenDirect=1","--AutoThrottle=1","--SFXVolume=0","--MusicVolume=0","--RaceTraceMaxSimTicks=60") + $gfx
$p = Start-Process -FilePath (Join-Path $rd "td5re.exe") -ArgumentList $a -WorkingDirectory $rd -PassThru
Get-ChildItem env: | Where-Object { $_.Name -like 'TD5RE_*' } | ForEach-Object { Remove-Item "env:$($_.Name)" }
$sw = [Diagnostics.Stopwatch]::StartNew()
$log = Join-Path $rd "log\race.log"
$done = $false
while (-not $p.HasExited -and $sw.Elapsed.TotalSeconds -lt $WaitSecs) {
    Start-Sleep -Seconds 3
    if (Test-Path $log) {
        try {
            $t = [IO.File]::ReadAllText($log)   # may be locked briefly
            if ($t -match 'VARIABLE LENGTH|GEO BUILD\] emitted') { Start-Sleep -Seconds 4; $done = $true; break }
        } catch { }
    }
}
if (-not $p.HasExited) {
    try { [void]$p.CloseMainWindow() } catch { }
    for ($i = 0; $i -lt 30 -and -not $p.HasExited; $i++) { Start-Sleep -Milliseconds 500 }
    if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force; Start-Sleep -Seconds 2 }
}
New-Item -ItemType Directory -Force -Path $keep | Out-Null
$rt = Join-Path $dst "_route"
if (Test-Path $rt) { Copy-Item $rt (Join-Path $keep "_route") -Recurse }
if (Test-Path $log) { Copy-Item $log (Join-Path $keep "race.log") -Force }
$sum = @()
if (Test-Path $log) {
    $sum += Select-String -Path $log -Pattern 'route fingerprint|trackgen: fork \d+ |CANDIDATE|candidate loop|OVERLAP kept|dropped:|refused by the verdict|LEFT avenue|BUILT' |
            ForEach-Object { ($_.Line -replace '\s+', ' ') } | Where-Object { $_ -notmatch 'REJECT|SLID' }
}
Set-Content -Path (Join-Path $keep "summary.txt") -Value $sum
"{0}: done={1} elapsed={2:N0}s" -f $Name, $done, $sw.Elapsed.TotalSeconds
$sum | Select-Object -First 40 | ForEach-Object { $_.Substring(0, [Math]::Min(230, $_.Length)) }
Remove-Tree $rd
