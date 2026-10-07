# geo_tags_identity.ps1 -- the two byte-identity gates for the OSM tag round
# (docs/plans/GEO_TAG_AUDIT.md, round 1007).
#
#   SYNTHETIC   seed 99991, slot 60 (level090), no place loaded. Every geo code
#               path is gated on td5_geo_loaded(), so this MUST be
#               byte-identical to master. It is the regression gate.
#   GEO         La Plata, slot 61 (level091). Expected to CHANGE -- the point is
#               that the change is attributable, which is what the three A/B
#               knobs are for (TD5RE_GEO_LM_TAGS / _AREA_TAGS / _ROAD_TAGS).
#
#   pwsh verify/geo_tags_identity.ps1 -Arm synthetic -Tag base
#   pwsh verify/geo_tags_identity.ps1 -Arm geo -Tag new
#   pwsh verify/geo_tags_identity.ps1 -Arm geo -Tag noknobs `
#        -Extra @{TD5RE_GEO_LM_TAGS="0"; TD5RE_GEO_AREA_TAGS="0"; TD5RE_GEO_ROAD_TAGS="0"}
#
# Standing launch rules, all applied here: --Windowed=1 (never fullscreen,
# never steal focus), RT off and minimum graphics, no networking.
#
# TD5RE_TG_DOUBLE_BUILD=1 + TD5RE_AUTOTRACK_REUSE=0 are NOT optional. A
# STREAMED build remove()s MODELS.DAT and the scenery worker writes it at the
# end (td5_tg_pages.c), so a run that quits on a tick cap races that worker and
# can hash nothing at all; and the trackgen counters read 0 on a reused second
# regenerate. See commit 400c9751 and the memory note on slot 60.
param([ValidateSet("synthetic","geo")][string]$Arm = "synthetic",
      [string]$Tag = "run",
      [string]$Seed = "99991",
      [int]$Port = 37231,
      [hashtable]$Extra = @{},
      [int]$GenWait = 900,
      [int]$RaceSecs = 10)

$wt = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)

# Clear EVERY TD5RE_* first: these persist across runs in a shell and a stale
# knob silently invalidates an A/B (memory: env_vars_persist_across_runs).
Get-ChildItem env: | Where-Object { $_.Name -like 'TD5RE_*' } |
    ForEach-Object { Remove-Item "env:$($_.Name)" }

$env:TD5RE_AUTOTRACK_SEED   = $Seed
$env:TD5RE_TG_DOUBLE_BUILD  = "1"
$env:TD5RE_AUTOTRACK_REUSE  = "0"
$env:TD5RE_CONTROL_PORT     = "$Port"
$env:TD5RE_WINDOW_TITLE     = "TD5RE idgate $Arm $Tag"
$env:TD5RE_RT               = "0"

if ($Arm -eq "geo") {
    $env:TD5RE_GEO_PLACE = "la_plata"
    $slot = 61; $lvlName = "level091"
} else {
    $slot = 60; $lvlName = "level090"
}
foreach ($k in $Extra.Keys) { Set-Item "env:$k" $Extra[$k] }

$gfx = @("--Windowed=1","--Lighting=0","--Quality=0","--SunShadows=0",
         "--Reflections=0","--WetRoads=0","--StreetLights=0","--CarLights=0",
         "--LegacyShadows=0","--GIQuality=0","--ShadowRays=0",
         "--ReflectionQuality=0","--CarShadows=0","--VFX=0",
         "--WorldBillboards=0","--FoliageAA=0","--RenderScale=50")

$lvl = Join-Path $wt "re\assets\levels\$lvlName"
if (Test-Path $lvl) { Remove-Item $lvl -Recurse -Force }
foreach ($f in @("race.log","engine.log","frontend.log")) {
    $p0 = Join-Path $wt "log\$f"
    for ($t = 0; $t -lt 20 -and (Test-Path $p0); $t++) {
        try { Remove-Item $p0 -Force -ErrorAction Stop } catch { Start-Sleep -Milliseconds 500 }
    }
}

Write-Host "arm=$Arm tag=$Tag seed=$Seed slot=$slot level=$lvlName"
$p = Start-Process -FilePath (Join-Path $wt "td5re.exe") `
      -ArgumentList (@("--AutoRace=1","--SkipIntro=1","--Control=1",
                       "--DefaultTrack=$slot") + $gfx) `
      -WorkingDirectory $wt -PassThru
Write-Host "pid=$($p.Id)"

# Send the window to the back so a long generate cannot sit over the user's
# work. Occluded is fine here: nothing in this gate measures frame time.
Start-Sleep -Seconds 2
try {
    Add-Type -Name W -Namespace N -MemberDefinition '
      [DllImport("user32.dll")] public static extern bool SetWindowPos(
        IntPtr h, IntPtr a, int x, int y, int cx, int cy, uint f);' -ErrorAction Stop
    if ($p.MainWindowHandle -ne 0) {
        [void][N.W]::SetWindowPos($p.MainWindowHandle, [IntPtr]1, 0,0,0,0, 0x0013)
    }
} catch { }

# MODELS.DAT present and stable for 5 s. race.log only flushes on a clean
# shutdown, so it cannot be polled for progress.
$models = Join-Path $lvl "MODELS.DAT"
$last = -1; $stable = 0; $ok = $false; $i = 0
for ($i = 0; $i -lt $GenWait; $i++) {
    Start-Sleep -Seconds 1
    if ($p.HasExited) { break }
    if (Test-Path $models) {
        $len = (Get-Item $models).Length
        if ($len -gt 0 -and $len -eq $last) { $stable++ } else { $stable = 0 }
        $last = $len
        if ($stable -ge 5) { $ok = $true; break }
    }
}
Write-Host "models settled=$ok after ${i}s"
if (-not $p.HasExited) { Start-Sleep -Seconds $RaceSecs }
if (-not $p.HasExited) {
    $u = New-Object System.Net.Sockets.UdpClient
    $ep = New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Loopback, $Port)
    try { $b = [Text.Encoding]::ASCII.GetBytes("quit"); [void]$u.Send($b, $b.Length, $ep) } catch { }
    $u.Close()
    for ($j = 0; $j -lt 40 -and -not $p.HasExited; $j++) { Start-Sleep -Seconds 1 }
    # PID-scoped, never a name-wide kill: parallel sessions share this machine.
    if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force; Start-Sleep -Seconds 2 }
}

foreach ($f in @("race.log","engine.log")) {
    $src = Join-Path $wt "log\$f"
    if (Test-Path $src) { Copy-Item $src (Join-Path $wt "log\idgate_${Arm}_${Tag}_$f") -Force }
}

Write-Host "### $Arm/$Tag  $lvlName"
foreach ($f in @("MODELS.DAT","STRIP.DAT","TEXTURES.DAT")) {
    $q = Join-Path $lvl $f
    if (Test-Path $q) {
        $h = (Get-FileHash $q -Algorithm SHA256).Hash
        Write-Host ("  {0,-13} {1,10} bytes  {2}" -f $f, (Get-Item $q).Length, $h.Substring(0,32))
    } else { Write-Host ("  {0,-13} MISSING" -f $f) }
}
$rl = Join-Path $wt "log\idgate_${Arm}_${Tag}_race.log"
if (Test-Path $rl) {
    Select-String -Path $rl -Pattern "geob:|roads surface|landmarks by deciding|real street census" |
        Select-Object -First 14 | ForEach-Object { Write-Host ("  [race] " + $_.Line) }
}
