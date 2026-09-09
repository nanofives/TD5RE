# prefab_frame.ps1 -- capture a frame of the auto track next to a PREFAB.
#
# Reuses the level090 already on disk rather than regenerating: the env below
# must therefore match the build's GENSTAMP env hash exactly, or the generator
# decides the cached build is stale and rebuilds it (slow, and it would then
# place the prefabs somewhere else).
#
#   pwsh verify/prefab_frame.ps1 -Span 190 -Tag greathall
#
# Frame capture notes that are easy to get wrong:
#   * TD5RE_FRAMEDUMP republishes roughly every 30 presented frames, so the file
#     must be polled until its SIZE STOPS CHANGING before it is read -- reading
#     mid-write gives a truncated PNG.
#   * boot takes tens of seconds (the generator runs at level load even on a
#     reuse), so an early poll looks like a hang.
#   * the kill is PID-scoped. Never /IM or -Name: other sessions run this exe.
param([int]$Span = 190, [string]$Tag = "prefab", [int]$Seed = 20260901,
      [int]$Wait = 75, [string]$Exe = "td5re.exe", [int]$Force = 1)

$wt = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
Set-Location $wt

Get-ChildItem env: | Where-Object { $_.Name -like 'TD5RE_*' } |
    ForEach-Object { Remove-Item "env:$($_.Name)" }
$env:TD5RE_AUTOTRACK_SEED    = "$Seed"
$env:TD5RE_AUTOTRACK_STREAM  = "0"
$env:TD5RE_AUTOTRACK_REUSE   = "1"
$env:TD5RE_R8_BIOME_SNOW     = "0"
$env:TD5RE_PREFAB_FORCE_ALL  = "$Force"
$env:TD5RE_CONTROL_PORT      = "37199"
$env:TD5RE_WINDOW_TITLE      = "TD5RE prefab $Tag"

$out = Join-Path $wt "log\prefab_$Tag.png"
Remove-Item $out -ErrorAction SilentlyContinue
$env:TD5RE_FRAMEDUMP = $out

$p = Start-Process -FilePath (Join-Path $wt $Exe) `
      -ArgumentList "--AutoRace=1","--SkipIntro=1","--Control=1",
                    "--DefaultTrack=60","--StartSpanOffset=$Span",
                    "--DefaultOpponents=0" `
      -PassThru
Write-Output "launched pid $($p.Id), span $Span, waiting up to ${Wait}s"

# WAIT FOR THE RACE, not just for a file. The first version of this polled the
# dump for size stability and captured after 4s -- the LOADING SCREEN, because
# during the generator's load pass nothing is presented, so the file legitimately
# stops changing. Ask the game what state it is in instead.
$py = @"
import sys, time
sys.path.insert(0, r'$wt\scripts\td5re_mcp')
from game_client import GameClient
c = GameClient(port=37199)
for _ in range($Wait):
    try:
        s = c.get_state()
        if s.get('race') and int(s.get('present_count', 0)) > 60:
            print('RACING present=%s' % s.get('present_count')); sys.exit(0)
    except Exception:
        pass
    time.sleep(1)
print('NEVER_RACED'); sys.exit(1)
"@
$py | Out-File -Encoding ascii "$env:TEMP\pf_wait.py"
$state = & python "$env:TEMP\pf_wait.py" 2>&1
Write-Output "state: $state"

# Only now is a dump trustworthy. Drop the loading-screen frame and take the
# next one the race publishes.
Remove-Item $out -ErrorAction SilentlyContinue
$last = -1; $stable = 0
for ($i = 0; $i -lt 40; $i++) {
    Start-Sleep -Seconds 1
    if ($p.HasExited) { Write-Output "process exited early"; break }
    if (-not (Test-Path $out)) { continue }
    $sz = (Get-Item $out).Length
    if ($sz -eq $last -and $sz -gt 0) { $stable++ } else { $stable = 0 }
    $last = $sz
    if ($stable -ge 2) { Write-Output "in-race frame settled at $sz bytes"; break }
}
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
if (Test-Path $out) { Write-Output "wrote $out ($((Get-Item $out).Length) bytes)" }
else { Write-Output "NO FRAME captured" }
