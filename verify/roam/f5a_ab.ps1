# f5a_ab.ps1 -- GEO FREE ROAM F5-A: mode-off race-trace A/B (this build vs the base exe).
#   pwsh verify/roam/f5a_ab.ps1 -Track 0   -Tag t0  [-Exe td5re.exe] [-OutDir log\f5a_ab_t0_new]
#   pwsh verify/roam/f5a_ab.ps1 -Track 61  -Tag l91 -Geo -Seed 2054677019 -Exe td5re_base.exe -OutDir log\f5a_ab_l91_base
# Runs one AutoRace with RaceTrace=1 (all modules) for -SimTicks sim ticks, the process quits itself
# (RaceTraceMaxSimTicks), then copies log\race_trace_*.csv to -OutDir. Compare two dirs with
#   python -I verify/roam/f2b_tracecmp.py <dirA> <dirB>
# Windowed, no focus steal, RT off + min graphics, no network, PID-scoped kill at the timeout.
param([int]$Track = 0, [string]$Tag = "run", [string]$Exe = "td5re.exe", [string]$OutDir = "",
      [switch]$Geo, [string]$Seed = "2054677019", [int]$SimTicks = 900, [int]$Timeout = 600)
$ErrorActionPreference = "Stop"
$wt = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path))
Get-ChildItem env: | Where-Object { $_.Name -like 'TD5RE_*' } | ForEach-Object { Remove-Item "env:$($_.Name)" }
$logd = Join-Path $wt "log"
New-Item -ItemType Directory -Force -Path $logd | Out-Null
Get-ChildItem (Join-Path $logd "*") -Include "race_trace_*", "*.log" -File -ErrorAction SilentlyContinue | ForEach-Object { try { Remove-Item $_.FullName -Force } catch { } }
$env:TD5RE_RT = "0"; $env:TD5RE_WINDOW_TITLE = "TD5RE f5a ab $Tag"
if ($Geo) {
    $env:TD5RE_AUTOTRACK_SEED = $Seed; $env:TD5RE_TG_DOUBLE_BUILD = "1"; $env:TD5RE_AUTOTRACK_REUSE = "0"
    $env:TD5RE_GEO_PLACE = "la_plata"
    # [F11b] single player no longer builds the strip geo track: the dev door keeps the strip A/B arm working
    $env:TD5RE_GEO_STRIP_BUILD = "1"
}
$gfx = @("--Windowed=1","--Lighting=0","--Quality=0","--SunShadows=0","--Reflections=0","--WetRoads=0","--StreetLights=0",
         "--CarLights=0","--LegacyShadows=0","--GIQuality=0","--ShadowRays=0","--ReflectionQuality=0","--CarShadows=0",
         "--VFX=0","--WorldBillboards=0","--FoliageAA=0","--RenderScale=50","--VSync=0")
$args2 = @("--AutoRace=1","--SkipIntro=1","--DefaultTrack=$Track","--DefaultOpponents=3","--RaceTrace=1","--RaceTraceSlot=0",
           "--RaceTraceMaxFrames=40000","--RaceTraceMaxSimTicks=$SimTicks","--CarDamage=0") + $gfx
$sw = [Diagnostics.Stopwatch]::StartNew()
$p = Start-Process -FilePath (Join-Path $wt $Exe) -ArgumentList $args2 -WorkingDirectory $wt -PassThru
Write-Host "pid=$($p.Id) exe=$Exe track=$Track geo=$Geo"
try { Add-Type -Name W6 -Namespace N6 -MemberDefinition '[DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr a, int x, int y, int cx, int cy, uint f);' -ErrorAction Stop } catch { }
$sent = $false
while (-not $p.HasExited -and $sw.Elapsed.TotalSeconds -lt $Timeout) {
    Start-Sleep -Milliseconds 500
    if (-not $sent) { $p.Refresh(); if ($p.MainWindowHandle -ne 0) { try { [void][N6.W6]::SetWindowPos($p.MainWindowHandle, [IntPtr]1, 0,0,0,0, 0x0013) } catch { }; $sent = $true } }
}
$killed = $false
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force; $killed = $true; Start-Sleep -Seconds 2 }
Write-Host ("game {0} after {1} s" -f $(if ($killed) { "KILLED at timeout" } else { "exited cleanly" }), [int]$sw.Elapsed.TotalSeconds)
if (-not $OutDir) { $OutDir = Join-Path $logd "f5a_ab_${Tag}" }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
Get-ChildItem (Join-Path $logd "race_trace_*.csv") -File | ForEach-Object { Copy-Item $_.FullName (Join-Path $OutDir $_.Name) -Force }
Write-Host ("traces: {0} file(s) -> {1}" -f (Get-ChildItem (Join-Path $OutDir "race_trace_*.csv")).Count, $OutDir)
