# TD5RE self-test suite runner (see td5mod/src/td5re/td5_selftest.c).
#
# Launches td5re.exe --SelfTest=1 [--SelfTestSuite=1], waits for the suite to
# self-terminate, prints the report summary, and exits with the game's exit
# code (0 = all steps passed, 1 = failures) so CI / scripts can gate on it.
#
# Usage:
#   pwsh scripts/selftest.ps1                 # smoke tier (~40 s)
#   pwsh scripts/selftest.ps1 -Suite full     # full matrix (~5-10 min)
#   pwsh scripts/selftest.ps1 -TimeoutSec 900 -Exe .\td5re.exe
#
# The suite forces its own harness baseline (SkipIntro, logging, muted SFX,
# AutoThrottle, MAIN_MENU boot), so no INI preparation is needed. Reports land
# in log/selftest_report.csv and log/selftest_report.md.
param(
    [ValidateSet("smoke", "full")]
    [string]$Suite = "smoke",
    [string]$Exe = ".\td5re.exe",
    [int]$TimeoutSec = 900
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

if (-not (Test-Path $Exe)) {
    Write-Error "exe not found: $Exe (run from the repo root or pass -Exe)"
    exit 2
}

Remove-Item "log\selftest_report.csv", "log\selftest_report.md" -ErrorAction SilentlyContinue

$args = "--SelfTest=1"
if ($Suite -eq "full") { $args += " --SelfTestSuite=1" }

$env:TD5RE_WINDOW_TITLE = "TD5RE SELFTEST ($Suite)"
# Non-interactive: suppress crash / fatal-init modals so a failure terminates
# cleanly instead of blocking on an unclickable dialog (which would leak the
# process holding the GPU/window and make the next launch collide). Inherited
# by any child process the game spawns (net loopback). See main.c/td5_headless_run.
$env:TD5RE_NO_DIALOG = "1"

# Live progress: the game logs "[INF][selftest] === step N/M: name ===" when a
# step starts and "step name PASS  (Nms, ...)" when it ends, into log/engine.log.
# Echo both as "[N/M] ..." lines while the suite runs (the Happy app turns them
# into a progress bar) instead of staying silent until the report.
# The game rotates engine.log at startup, so open the old file at its end now
# and switch to the fresh one once the path is shorter than our offset.
$logPath = Join-Path $root "log\engine.log"
$share = [System.IO.FileShare]::ReadWrite -bor [System.IO.FileShare]::Delete
$log = $null
if (Test-Path $logPath) {
    $log = [System.IO.File]::Open($logPath, 'Open', 'Read', $share)
    [void]$log.Seek(0, 'End')
}
$pending = ""
$stepTag = ""
# Dot-sourced below, so its assignments land in this script's scope.
$echoProgress = {
    if (Test-Path $logPath) {
        $len = (Get-Item $logPath).Length
        if (-not $log -or $len -lt $log.Position) {
            if ($log) { $log.Dispose() }
            $log = [System.IO.File]::Open($logPath, 'Open', 'Read', $share)
            $pending = ""
        }
    }
    if ($log -and $log.Length -gt $log.Position) {
        $buf = New-Object byte[] ($log.Length - $log.Position)
        $n = $log.Read($buf, 0, $buf.Length)
        $lines = ($pending + [System.Text.Encoding]::UTF8.GetString($buf, 0, $n)) -split "`n"
        $pending = $lines[-1]
        # (0..-1 would count down, so guard the no-newline-yet case.)
        $complete = if ($lines.Count -gt 1) { $lines[0..($lines.Count - 2)] } else { @() }
        foreach ($line in $complete) {
            if ($line -match '\[selftest\] === step (\d+)/(\d+): (.+?) ===') {
                $stepTag = "[$($Matches[1])/$($Matches[2])]"
                Write-Host "$stepTag $($Matches[3]) ..."
            } elseif ($line -match '\[selftest\] step (\S+)\s+(PASS|WARN|FAIL)\s+\((\d+)ms') {
                Write-Host "$stepTag $($Matches[1]) $($Matches[2]) ($($Matches[3]) ms)"
            }
        }
    }
}

Write-Host "Launching $Exe $args (timeout ${TimeoutSec}s)..."
$p = Start-Process -FilePath $Exe -ArgumentList $args -PassThru
$deadline = (Get-Date).AddSeconds($TimeoutSec)

# PID-scoped watchdog: kill ONLY the process we launched (never by name --
# parallel sessions run their own td5re.exe).
while (-not $p.WaitForExit(1000)) {
    . $echoProgress
    if ((Get-Date) -gt $deadline) {
        Write-Warning "suite exceeded ${TimeoutSec}s -- killing PID $($p.Id)"
        $p.Kill()
        $p.WaitForExit()
        break
    }
}
. $echoProgress
if ($log) { $log.Dispose() }

Write-Host ""
if (Test-Path "log\selftest_report.md") {
    # Summary line + per-step table for the console.
    Get-Content "log\selftest_report.md"
} else {
    Write-Warning "no report written (crash before PH_REPORT?) -- check log/engine.log"
}

Write-Host ""
Write-Host "exit code: $($p.ExitCode)"
exit $p.ExitCode
