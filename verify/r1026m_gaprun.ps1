# r1026m_gaprun.ps1 -- run ONE traffic race headless-ish and keep its race.log (round 1026 M).
#
#   pwsh verify/r1026m_gaprun.ps1 -Tag base_t0 -Track 0 -Exe td5re_master.exe -Extra "TD5RE_V2V_GAPLOG=1"
#   pwsh verify/r1026m_gaprun.ps1 -Tag sat_t0  -Track 0 -Exe td5re.exe        -Extra "TD5RE_V2V_GAPLOG=1"
#
# Drives slot 0 with the AI autopilot (-Mode ai) or full throttle and no steering (-Mode throttle),
# traffic ON, a fixed RNG seed path, windowed, sent to back, RT off and minimum graphics. Quits by itself
# after -Ticks sim ticks (RaceTraceMaxSimTicks). The race.log is copied to log/r1026m_<Tag>_race.log.
# Knobs reach the game ONLY through -Extra ("A=1|B=2"); every TD5RE_* already in the shell is cleared.
param([string]$Tag = "run",
      [int]$Track = 0,
      [string]$Exe = "td5re.exe",
      [string]$Extra = "",
      [ValidateSet("ai","throttle")][string]$Mode = "ai",
      [int]$Ticks = 6000,
      [int]$Traffic = 4,
      [int]$Opponents = 5,
      [int]$StartSpanOffset = 0,
      [int]$Timeout = 600,
      [int]$FastForward = 1,
      [int]$RenderScale = 50,
      [int]$Width = 960, [int]$Height = 540,
      [string]$Frame = "")          # optional TD5RE_FRAMEDUMP path

$ErrorActionPreference = "Stop"
$wt = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
Get-ChildItem env: | Where-Object { $_.Name -like 'TD5RE_*' } | ForEach-Object { Remove-Item "env:$($_.Name)" }
if (-not (Test-Path (Join-Path $wt "td5re.ini")) -and (Test-Path (Join-Path $wt "td5re.ini.default"))) {
    Copy-Item (Join-Path $wt "td5re.ini.default") (Join-Path $wt "td5re.ini")
}
$log = Join-Path $wt "log"
New-Item -ItemType Directory -Force -Path $log | Out-Null
foreach ($f in @("race.log","engine.log","frontend.log")) {
    $p0 = Join-Path $log $f
    for ($t = 0; $t -lt 20 -and (Test-Path $p0); $t++) {
        try { Remove-Item $p0 -Force -ErrorAction Stop } catch { Start-Sleep -Milliseconds 500 }
    }
}
$env:TD5RE_RT = "0"
$env:TD5RE_WINDOW_TITLE = "TD5RE r1026m $Tag"
if ($Frame) { $env:TD5RE_FRAMEDUMP = ($Frame -replace '\\', '/') }
foreach ($kv in ($Extra -split '[|;]')) {
    $p = $kv.Split('=', 2)
    if ($p.Count -eq 2 -and $p[0].Trim() -ne "") {
        $n = $p[0].Trim(); if ($n -notlike 'TD5RE_*') { $n = "TD5RE_$n" }
        Set-Item "env:$n" $p[1].Trim().Trim('"')
    }
}
$gfx = @("--Lighting=0","--Quality=0","--SunShadows=0","--Reflections=0","--WetRoads=0","--StreetLights=0",
         "--CarLights=0","--LegacyShadows=0","--GIQuality=0","--ShadowRays=0","--ReflectionQuality=0",
         "--CarShadows=0","--VFX=0","--WorldBillboards=0","--FoliageAA=0","--RenderScale=$RenderScale")
$ai = if ($Mode -eq "ai") { 1 } else { 0 }
$gameArgs = @("--AutoRace=1","--SkipIntro=1","--DefaultTrack=$Track","--DefaultOpponents=$Opponents",
              "--Traffic=$Traffic","--Cops=0","--PlayerIsAI=$ai","--AutoThrottle=1","--CarDamage=0",
              "--Collisions=1","--Windowed=1","--VSync=0","--Width=$Width","--Height=$Height",
              "--LogEnabled=1","--LogRace=1","--LogEngine=1",
              "--TraceFastForward=$FastForward","--RaceTraceMaxSimTicks=$Ticks","--RaceTrace=0") +
            $(if ($StartSpanOffset -gt 0) { @("--StartSpanOffset=$StartSpanOffset") } else { @() }) + $gfx
Add-Type -Name W5 -Namespace N5 -MemberDefinition '
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr a, int x, int y, int cx, int cy, uint f);' -ErrorAction SilentlyContinue
$sw = [Diagnostics.Stopwatch]::StartNew()
$p = Start-Process -FilePath (Join-Path $wt $Exe) -ArgumentList $gameArgs -WorkingDirectory $wt -PassThru
Write-Host "pid=$($p.Id) tag=$Tag exe=$Exe track=$Track mode=$Mode ticks=$Ticks"
$sent = $false
while (-not $p.HasExited -and $sw.Elapsed.TotalSeconds -lt $Timeout) {
    Start-Sleep -Milliseconds 500
    if (-not $sent) {
        $p.Refresh()
        if ($p.MainWindowHandle -ne 0) {
            try { [void][N5.W5]::SetWindowPos($p.MainWindowHandle, [IntPtr]1, 0,0,0,0, 0x0013) } catch { }
            $sent = $true
        }
    }
}
$killed = $false
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force; $killed = $true; Start-Sleep -Seconds 2 }
Write-Host ("game {0} after {1} s" -f $(if ($killed) { "KILLED at timeout" } else { "exited cleanly" }), [int]$sw.Elapsed.TotalSeconds)
$src = Join-Path $log "race.log"
$dst = Join-Path $log ("r1026m_{0}_race.log" -f $Tag)
if (Test-Path $src) { Copy-Item $src $dst -Force; Write-Host "log -> $dst ($((Get-Item $dst).Length) bytes)" } else { Write-Host "NO race.log" }
if ($killed) { exit 1 }
