# topo_gen.ps1 -- TOPOLOGY-FIRST generator probe.
#
# Regenerates the auto track (slot 60 / level090) for one seed under the
# byte-identity protocol (STREAM=0, REUSE=0, RaceTrace on), waits for the
# generation to finish, quits the game CLEANLY over the control socket so
# race.log flushes, then prints the [WORLD]/[STRUCT]/[NET]/[R22 TRIM] lines and
# the sha256 of every level file. Run it N times with the same seed: the
# hashes must be identical (determinism gate). Compare across builds to see
# WHAT changed.
#
#   pwsh verify/topo_gen.ps1 -Seed 20260901 -Tag base [-Extra @{TD5RE_TG_WORLD_DUMP="1"}]
#                            [-Exe td5re.exe] [-Port 37151] [-Race]
#
# -Race keeps the game running the race for -RaceSecs seconds before quitting
# (for a framedump via TD5RE_FRAMEDUMP in -Extra).
#
# -MinGfx (DEFAULT ON) runs with RT off and minimum graphics, per Mariano's
# standing rule: every test that is not the selftest suite goes that way, because
# RT plus a full graphics load can lock the machine. It is a SWITCH rather than
# unconditional because this script's own -Race + TD5RE_FRAMEDUMP path exists to
# look at visuals, and minimum graphics would change what that captures. Pass
# -MinGfx:$false for a deliberate graphics or RT test, and ask Mariano first.
param([string]$Seed = "20260901", [hashtable]$Extra = @{}, [string]$Tag = "run",
      [string]$Exe = "td5re.exe", [int]$GenWait = 600, [int]$Port = 37151,
      [switch]$Race, [int]$RaceSecs = 20, [switch]$Keep, [string]$Root = "",
      [switch]$MinGfx = $true)
$wt = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
if ($Root -ne "") { $wt = $Root }   # probe another worktree (baseline exe) without copying the script

Get-ChildItem env: | Where-Object { $_.Name -like 'TD5RE_*' } | ForEach-Object { Remove-Item "env:$($_.Name)" }
$env:TD5RE_AUTOTRACK_SEED   = $Seed
$env:TD5RE_AUTOTRACK_STREAM = "0"
$env:TD5RE_AUTOTRACK_REUSE  = "0"
$env:TD5RE_CONTROL_PORT     = "$Port"
$env:TD5RE_WINDOW_TITLE     = "TD5RE topo $Tag seed $Seed"
if ($MinGfx) { $env:TD5RE_RT = "0" }   # overrides the INI; never TD5RE_AUTO_PERF=2
foreach ($k in $Extra.Keys) { Set-Item "env:$k" $Extra[$k] }   # -Extra still wins

$gfx = @()
if ($MinGfx) {
    $gfx = @("--Lighting=0","--Quality=0","--SunShadows=0","--Reflections=0",
             "--WetRoads=0","--StreetLights=0","--CarLights=0","--LegacyShadows=0",
             "--GIQuality=0","--ShadowRays=0","--ReflectionQuality=0",
             "--CarShadows=0","--VFX=0","--WorldBillboards=0","--FoliageAA=0",
             "--RenderScale=50")
}

$lvl = Join-Path $wt "re\assets\levels\level090"
if ((Test-Path $lvl) -and -not $Keep) { Remove-Item $lvl -Recurse -Force }
$log = Join-Path $wt "log\race.log"
for ($t = 0; $t -lt 20 -and (Test-Path $log); $t++) {
    try { Remove-Item $log -Force -ErrorAction Stop } catch { Start-Sleep -Milliseconds 500 }
}

$p = Start-Process -FilePath (Join-Path $wt $Exe) `
      -ArgumentList (@("--AutoRace=1","--SkipIntro=1","--Control=1","--DefaultTrack=60","--RaceTrace=1") + $gfx) `
      -WorkingDirectory $wt -PassThru
Write-Host "pid=$($p.Id) exe=$Exe seed=$Seed tag=$Tag port=$Port"

$models = Join-Path $lvl "MODELS.DAT"
$last = -1; $stable = 0; $done = $false
for ($i = 0; $i -lt $GenWait; $i++) {
    Start-Sleep -Seconds 1
    if ($p.HasExited) { break }
    if (Test-Path $models) {
        $len = (Get-Item $models).Length
        if ($len -gt 0 -and $len -eq $last) { $stable++ } else { $stable = 0 }
        $last = $len
        if ($stable -ge 4) { $done = $true; break }
    }
}
Write-Host "generated=$done after ${i}s models=$last"
if ($Race -and -not $p.HasExited) { Start-Sleep -Seconds $RaceSecs }

if (-not $p.HasExited) {
    $u = New-Object System.Net.Sockets.UdpClient
    $ep = New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Loopback, $Port)
    try { $b = [Text.Encoding]::ASCII.GetBytes("quit"); [void]$u.Send($b, $b.Length, $ep) } catch { }
    $u.Close()
    for ($j = 0; $j -lt 30 -and -not $p.HasExited; $j++) { Start-Sleep -Seconds 1 }
    if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force; Start-Sleep -Seconds 2 }
}
$out = Join-Path $wt "log\race_topo_$Tag.log"
Copy-Item $log $out -Force -ErrorAction SilentlyContinue
Write-Host "### $Tag seed=$Seed"
if (Test-Path $out) {
    Select-String -Path $out -Pattern "\[WORLD\]|\[STRUCT\]|\[NET\]|\[R22 TRIM\]|\[R21 GRADE\]|\[R8 SHAPE\]|boxed in|guard.*reject|centerline build failed|strip emit failed" |
        ForEach-Object { Write-Host ("  " + $_.Line) }
} else { Write-Host "  NO LOG" }
foreach ($f in @("STRIP.DAT","LEFT.TRK","RIGHT.TRK","LEVELINF.DAT","MODELS.DAT","TEXTURES.DAT","MESHTAG.BIN","NETWORK.JSON","GENSTAMP.TXT")) {
    $p2 = Join-Path $lvl $f
    if (Test-Path $p2) {
        $h = (Get-FileHash $p2 -Algorithm SHA256).Hash.Substring(0,16)
        Write-Host ("  {0,-13} {1,10} bytes  {2}" -f $f, (Get-Item $p2).Length, $h)
    }
}
