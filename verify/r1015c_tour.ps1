# r1015c_tour.ps1 (copy of geo_r1014a_tour.ps1 + -StartSpan) -- round 1014 A: LOOK at a geo-track report from the spot it was
# made. Boots Mariano's La Plata route (geo slot 61 / level091), puts the free
# camera at each pose of -PosesFile and dumps one PNG per pose, then kills the game.
#
# A pose line is "name:x,y,z,yawdeg,pitchdeg" (see td5_camera.c tour_step); poses are
# separated by ';' or newlines in the file. Make them from a pick string with
#   python verify/geo_r1014a_poses.py name x y z [name x y z ...]
#
#   pwsh verify/geo_r1014a_tour.ps1 -Tag after -PosesFile poses.txt
#   pwsh verify/geo_r1014a_tour.ps1 -Tag before -PosesFile poses.txt -Env "TD5RE_GEO_FORK_MERGE=0"
#
# Standing launch rules, all applied: --Windowed=1, --VSync=0, RT off and minimum
# graphics, car damage off, no sockets, every TD5RE_* cleared first, a hard
# wall-clock kill BY PID. Frames land in log/tour_<Tag>_<name>.png.
param([string]$Tag = "run",
      [string]$Exe = "td5re.exe",
      [Parameter(Mandatory = $true)][string]$PosesFile,
      [string]$Env = "",            # "NAME=VALUE|NAME=VALUE" (poses contain commas)
      [string]$GeoPlace = "la_plata",
      [int]$Track = 61,
      [int]$Dwell = 90,
      [int]$StartSpan = 0,          # [R1015 C] --StartSpanOffset: park the car beside the geometry so it is streamed in
      [int]$GenWait = 900,
      [int]$RaceSecs = 240)

$wt = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
Get-ChildItem env: | Where-Object { $_.Name -like 'TD5RE_*' } | ForEach-Object { Remove-Item "env:$($_.Name)" }

$poses = ((Get-Content -Raw $PosesFile) -replace "[\r\n]+", ";").Trim(';')
$names = @($poses.Split(';') | Where-Object { $_ } | ForEach-Object { $_.Split(':')[0].Trim() })
$env:TD5RE_GEO_PLACE         = $GeoPlace
$env:TD5RE_AUTOTRACK_REUSE   = "0"
$env:TD5RE_RT                = "0"
$env:TD5RE_D3D12_CAPTURE      = "1"
$env:TD5RE_WINDOW_TITLE      = "TD5RE tour $Tag"
$env:TD5RE_FREECAM_TOUR      = $poses
$env:TD5RE_FREECAM_TOUR_PATH = "log/tour_${Tag}_%s.png"
$env:TD5RE_FREECAM_TOUR_DWELL = "$Dwell"
if ($Env -ne "") {
    foreach ($kv in $Env.Split('|')) {
        $p = $kv.Split('=', 2)
        if ($p.Count -eq 2 -and $p[0].Trim() -ne "") { Set-Item "env:$($p[0].Trim())" $p[1] }
    }
}

$gfx = @("--Lighting=0","--Quality=0","--SunShadows=0","--Reflections=0",
         "--WetRoads=0","--StreetLights=0","--CarLights=0","--LegacyShadows=0",
         "--GIQuality=0","--ShadowRays=0","--ReflectionQuality=0",
         "--CarShadows=0","--VFX=0","--WorldBillboards=0","--FoliageAA=0")
$log = Join-Path $wt "log"
New-Item -ItemType Directory -Force -Path $log | Out-Null
foreach ($f in @("race.log","engine.log","frontend.log")) {
    $p0 = Join-Path $log $f
    for ($t = 0; $t -lt 20 -and (Test-Path $p0); $t++) {
        try { Remove-Item $p0 -Force -ErrorAction Stop } catch { Start-Sleep -Milliseconds 500 }
    }
}
foreach ($n in $names) {
    $q = Join-Path $log "tour_${Tag}_$n.png"
    if (Test-Path $q) { Remove-Item -LiteralPath $q -Force -ErrorAction SilentlyContinue }
}
$lvl = Join-Path $wt ("re\assets\levels\level{0:D3}" -f ($Track + 30))
if (Test-Path $lvl) { Remove-Item $lvl -Recurse -Force }

$args = @("--AutoRace=1","--SkipIntro=1","--DefaultTrack=$Track","--DefaultOpponents=0",
          "--Traffic=0","--PlayerIsAI=1","--AutoThrottle=1","--CarDamage=0",
          "--Windowed=1","--VSync=0") + $(if ($StartSpan -gt 0) { @("--StartSpanOffset=$StartSpan") } else { @() }) + $gfx
$p = Start-Process -FilePath (Join-Path $wt $Exe) -ArgumentList $args -WorkingDirectory $wt -PassThru
Write-Host "pid=$($p.Id) tag=$Tag exe=$Exe poses=$($names.Count)"
Start-Sleep -Seconds 2
try {
    Add-Type -Name W5 -Namespace N5 -MemberDefinition '
      [DllImport("user32.dll")] public static extern bool SetWindowPos(
        IntPtr h, IntPtr a, int x, int y, int cx, int cy, uint f);' -ErrorAction Stop
    if ($p.MainWindowHandle -ne 0) { [void][N5.W5]::SetWindowPos($p.MainWindowHandle, [IntPtr]1, 0,0,0,0, 0x0013) }
} catch { }

$deadline = (Get-Date).AddSeconds($GenWait + $RaceSecs)
while (-not $p.HasExited -and (Get-Date) -lt $deadline) {
    Start-Sleep -Seconds 4
    $have = @($names | Where-Object { Test-Path (Join-Path $log "tour_${Tag}_$_.png") })
    if ($have.Count -eq $names.Count) { Write-Host "all $($names.Count) frame(s) captured"; Start-Sleep -Seconds 2; break }
}
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force; Start-Sleep -Seconds 2 }
Write-Host "stopped pid=$($p.Id)"
foreach ($n in $names) {
    $q = Join-Path $log "tour_${Tag}_$n.png"
    Write-Host ("  {0,-14} {1}" -f $n, $(if (Test-Path $q) { (Get-Item $q).Length } else { "MISSING" }))
}
