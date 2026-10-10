# geo_r1015b_build.ps1 -- rebuild La Plata (slot 61 / level091) with chosen knobs, wait for the
# streamed MODELS.DAT to settle, close the window CLEANLY (race.log flushes only on a clean
# shutdown) and keep the generated files under log/r1015b_<Tag>/ for the offline audit.
#
#   pwsh verify/geo_r1015b_build.ps1 -Tag before -Exe td5re_parent.exe
#   pwsh verify/geo_r1015b_build.ps1 -Tag after  -Env "TD5RE_GEO_NET_DIAG=1"
#
# Standing launch rules: --Windowed=1, --VSync=0, RT off and minimum graphics, no sockets,
# window sent to the back, kill BY PID only. Every TD5RE_* is cleared first.
param([string]$Tag = "run",
      [string]$Exe = "td5re.exe",
      [string]$Env = "",            # "NAME=VALUE;NAME=VALUE"
      [string]$GeoPlace = "la_plata",
      [int]$Track = 61,
      [int]$GenWait = 1500,
      [int]$PostSettle = 40)

$wt = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
Get-ChildItem env: | Where-Object { $_.Name -like 'TD5RE_*' } | ForEach-Object { Remove-Item "env:$($_.Name)" }
$env:TD5RE_GEO_PLACE = $GeoPlace
$env:TD5RE_AUTOTRACK_REUSE = "0"
$env:TD5RE_TG_DOUBLE_BUILD = "1"
$env:TD5RE_RT = "0"
$env:TD5RE_WINDOW_TITLE = "TD5RE r1015b build $Tag"
foreach ($kv in ($Env -split ';' | Where-Object { $_ })) {
    $i = $kv.IndexOf('=')
    if ($i -gt 0) { Set-Item -Path ("env:" + $kv.Substring(0, $i)) -Value $kv.Substring($i + 1) }
}
$gfx = @("--Windowed=1","--VSync=0","--CarDamage=0","--Lighting=0","--Quality=0",
         "--SunShadows=0","--Reflections=0","--WetRoads=0","--StreetLights=0",
         "--CarLights=0","--LegacyShadows=0","--GIQuality=0","--ShadowRays=0",
         "--ReflectionQuality=0","--CarShadows=0","--VFX=0","--WorldBillboards=0",
         "--FoliageAA=0","--Width=1280","--Height=720","--Logging=1")
$lvlName = "level0" + ($Track + 30).ToString()
$lvl = Join-Path $wt "re\assets\levels\$lvlName"
if (Test-Path $lvl) { Remove-Item $lvl -Recurse -Force }
New-Item -ItemType Directory -Force -Path (Join-Path $wt "log") | Out-Null
foreach ($f in @("race.log","engine.log","frontend.log")) {
    $p0 = Join-Path $wt "log\$f"
    for ($t = 0; $t -lt 20 -and (Test-Path $p0); $t++) {
        try { Remove-Item $p0 -Force -ErrorAction Stop } catch { Start-Sleep -Milliseconds 500 }
    }
}
$p = Start-Process -FilePath (Join-Path $wt $Exe) `
      -ArgumentList (@("--AutoRace=1","--SkipIntro=1","--DefaultTrack=$Track") + $gfx) `
      -WorkingDirectory $wt -PassThru
Write-Host "pid=$($p.Id) tag=$Tag exe=$Exe"
Start-Sleep -Seconds 2
try {
    Add-Type -Name W3 -Namespace N3 -MemberDefinition '
      [DllImport("user32.dll")] public static extern bool SetWindowPos(
        IntPtr h, IntPtr a, int x, int y, int cx, int cy, uint f);' -ErrorAction Stop
    if ($p.MainWindowHandle -ne 0) { [void][N3.W3]::SetWindowPos($p.MainWindowHandle, [IntPtr]1, 0,0,0,0, 0x0013) }
} catch { }
$models = Join-Path $lvl "MODELS.DAT"
$last = -1; $stable = 0; $i = 0; $ok = $false
for ($i = 0; $i -lt $GenWait; $i++) {
    Start-Sleep -Seconds 1
    if ($p.HasExited) { break }
    if (Test-Path $models) {
        $len = (Get-Item $models).Length
        if ($len -gt 0 -and $len -eq $last) { $stable++ } else { $stable = 0 }
        $last = $len
        if ($stable -ge 8) { $ok = $true; break }
    }
}
Write-Host "models settled=$ok after ${i}s"
if (-not $p.HasExited) {
    # the main thread is still inside level load when MODELS.DAT settles: a WM_CLOSE sent now
    # is not pumped, so give it time to reach the race before closing (clean close = log flush)
    Start-Sleep -Seconds $PostSettle
    [void]$p.CloseMainWindow()
    for ($j = 0; $j -lt 90 -and -not $p.HasExited; $j++) { Start-Sleep -Seconds 1 }
    if (-not $p.HasExited) { Write-Host "clean close failed, killing pid $($p.Id)"; Stop-Process -Id $p.Id -Force; Start-Sleep -Seconds 2 }
}
$out = Join-Path $wt "log\r1015b_$Tag"
New-Item -ItemType Directory -Force -Path $out | Out-Null
foreach ($f in @("MODELS.DAT","STRIP.DAT","NETWORK.JSON","MESHTAG.BIN","TEXTURES.DAT")) {
    $q = Join-Path $lvl $f
    if (Test-Path $q) { Copy-Item $q $out -Force; Write-Host ("  {0,-13} {1,10} bytes" -f $f, (Get-Item $q).Length) }
}
foreach ($f in @("race.log","engine.log")) {
    $src = Join-Path $wt "log\$f"
    if (Test-Path $src) { Copy-Item $src $out -Force }
}
Write-Host "kept in $out"
