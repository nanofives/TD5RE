# geo_world_frames.ps1 -- N4 WORLD (round 1008): race the La Plata geo route on
# slot 61 and dump a frame each time the player's span first reaches one of the
# listed targets. No sockets (no --Control, no td5re MCP). RT off + minimum
# graphics, windowed, VSync off, hard wall-clock kill BY PID.
#
#   pwsh verify/geo_world_frames.ps1 -Tag before -Spans "66,238,411,617,793,976,1193,1387"
#
# Frames land in log/<Tag>_span_<N>.png. The run's race/engine logs are copied
# to log/<Tag>_race.log / <Tag>_engine.log so a later run cannot overwrite them.
param(
    [string]$Tag        = "run",
    [string]$Spans      = "66,238,411,617,793,976,1193,1387",
    [string]$Place      = "la_plata",
    [int]   $Track      = 61,
    [int]   $MaxSecs    = 900,
    [int]   $RenderScale= 100,
    [hashtable]$Extra   = @{}
)

$wt = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)

# TD5RE_* knobs persist across launches in a shell -- wipe them first.
Get-ChildItem env: | Where-Object { $_.Name -like 'TD5RE_*' } | ForEach-Object { Remove-Item "env:$($_.Name)" }

$env:TD5RE_RT                     = "0"
$env:TD5RE_GEO_PLACE              = $Place
$env:TD5RE_WINDOW_TITLE           = "TD5RE geo $Tag"
$env:TD5RE_FRAMEDUMP_SPANS        = $Spans
$env:TD5RE_FRAMEDUMP_SPAN_PATH    = "log/${Tag}_span_%d.png"
$env:TD5RE_D3D12_CAPTURE          = "1"
foreach ($k in $Extra.Keys) { Set-Item "env:$k" $Extra[$k] }

$gfx = @("--Lighting=0","--Quality=0","--SunShadows=0","--Reflections=0",
         "--WetRoads=0","--StreetLights=0","--CarLights=0","--LegacyShadows=0",
         "--GIQuality=0","--ShadowRays=0","--ReflectionQuality=0",
         "--CarShadows=0","--VFX=0","--WorldBillboards=0","--FoliageAA=0",
         "--RenderScale=$RenderScale")

New-Item -ItemType Directory -Force (Join-Path $wt "log") | Out-Null
foreach ($f in @("race.log","engine.log","frontend.log")) {
    $p0 = Join-Path $wt "log\$f"
    if (Test-Path $p0) { try { Remove-Item $p0 -Force -ErrorAction Stop } catch {} }
}
Get-ChildItem (Join-Path $wt "log") -Filter "${Tag}_span_*.png" -ErrorAction SilentlyContinue |
    Remove-Item -Force -ErrorAction SilentlyContinue

$args = @("--AutoRace=1","--SkipIntro=1","--PlayerIsAI=1","--Windowed=1","--VSync=0",
          "--DefaultTrack=$Track") + $gfx
Write-Host "launch: td5re.exe $($args -join ' ')"
$p = Start-Process -FilePath (Join-Path $wt "td5re.exe") -ArgumentList $args `
      -WorkingDirectory $wt -PassThru
Write-Host "pid=$($p.Id) tag=$Tag maxsecs=$MaxSecs"

# Send the window to the back so it never steals focus.
Start-Sleep -Seconds 3
try {
    Add-Type -Namespace W -Name N -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr a, int x, int y, int cx, int cy, uint f);
'@ -ErrorAction SilentlyContinue
    $p.Refresh()
    if ($p.MainWindowHandle -ne 0) {
        [W.N]::SetWindowPos($p.MainWindowHandle, [IntPtr]1, 0, 0, 0, 0, 0x0013) | Out-Null
    }
} catch {}

$want = ($Spans -split ',' | Where-Object { $_ -ne '' }).Count
$t0 = Get-Date
while (-not $p.HasExited) {
    $el = ((Get-Date) - $t0).TotalSeconds
    $have = @(Get-ChildItem (Join-Path $wt "log") -Filter "${Tag}_span_*.png" -ErrorAction SilentlyContinue).Count
    if ($have -ge $want) { Write-Host "all $want frames captured at ${el}s"; break }
    if ($el -ge $MaxSecs) { Write-Host "WALL CLOCK $MaxSecs s reached ($have/$want frames)"; break }
    Start-Sleep -Seconds 5
}

# Hard kill BY PID (never by name -- parallel sessions run their own exes).
if (-not $p.HasExited) {
    Start-Sleep -Seconds 6     # let the last dump land
    try { Stop-Process -Id $p.Id -Force -ErrorAction Stop } catch {}
}
Start-Sleep -Seconds 2
foreach ($f in @("race.log","engine.log","frontend.log")) {
    $p0 = Join-Path $wt "log\$f"
    if (Test-Path $p0) { Copy-Item $p0 (Join-Path $wt "log\${Tag}_$f") -Force -ErrorAction SilentlyContinue }
}
Get-ChildItem (Join-Path $wt "log") -Filter "${Tag}_span_*.png" -ErrorAction SilentlyContinue |
    ForEach-Object { Write-Host ("  {0}  {1} bytes" -f $_.Name, $_.Length) }
Write-Host "done tag=$Tag"
