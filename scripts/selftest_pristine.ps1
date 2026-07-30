# Run the self-test suite against a PRISTINE td5re.ini, then restore yours.
#
# Why: the working copy of td5re.ini routinely carries tuning changes
# (Difficulty, Dynamics, CarToughness, ...) that the suite does NOT override in
# its harness baseline. Those change the SIM, so the golden traces mismatch and
# report a FAIL that has nothing to do with the code under test. This has cost
# real debugging time more than once.
#
# Also drops the render resolution (default 1280x676 -- same 1.894 aspect as
# 2560x1351, so the render goldens see the same projection). There is a
# pre-existing nvwgf2um.dll fault in Present at race-moscow-base that is
# LOAD-SENSITIVE; lower resolution reduces its probability. That is a
# mitigation, not a fix -- see docs/plans/X64_DXR_ROADMAP.md.
#
# The restore runs in a finally block, so Ctrl-C / a crashing suite still gives
# you your INI back.
#
# Usage:
#   pwsh -NoProfile -File scripts/selftest_pristine.ps1 [-Suite full] [-Width N -Height N]
#   pwsh -NoProfile -File scripts/selftest_pristine.ps1 -Exe .\other.exe   # alternate build
#
# -Exe forwards to selftest.ps1 (kept from the x64-retarget era, when the
# 32- and 64-bit exes shared one td5re.ini and both needed the pristine-INI
# treatment; td5re.exe IS the x86_64 build since the 2026-07-30 retirement).

param(
    [ValidateSet("smoke", "full")]
    [string]$Suite = "full",
    [int]$Width = 1280,
    [int]$Height = 676,
    [int]$TimeoutSec = 1200,
    [string]$Exe = ".\td5re.exe"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

$ini    = "td5re.ini"
$backup = "td5re.ini.userbak"

if (Test-Path $backup) {
    Write-Error "$backup already exists -- a previous run may have died before restoring. Inspect and remove it before continuing (it holds YOUR settings)."
}

Copy-Item $ini $backup -Force
Write-Host "backed up $ini -> $backup"

try {
    # pristine = whatever is committed, not whatever is in the working tree
    & git checkout -- $ini
    if ($LASTEXITCODE -ne 0) { throw "git checkout -- $ini failed" }

    $text = Get-Content $ini -Raw
    $text = $text -replace '(?m)^(Width\s*=\s*)\d+', "`${1}$Width"
    $text = $text -replace '(?m)^(Height\s*=\s*)\d+', "`${1}$Height"
    Set-Content $ini $text -NoNewline
    Write-Host "pristine INI staged at ${Width}x${Height}"

    & pwsh -NoProfile -File "$root\scripts\selftest.ps1" -Suite $Suite -TimeoutSec $TimeoutSec -Exe $Exe
    $code = $LASTEXITCODE
}
finally {
    Copy-Item $backup $ini -Force
    Remove-Item $backup -Force
    Write-Host "restored your $ini"
}

Write-Host ""
if ($code -ne 0) {
    Write-Host "suite exit $code -- on a degrade-private-bytes FAIL, read log/gpu_device_lost.log FIRST:"
    Write-Host "  device-lost recovery retains ~64 MB per recreation and fakes a per-race leak."
}
exit $code
