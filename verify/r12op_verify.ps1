# [R12 OVERPASS] Multi-seed verification sweep. For each seed: one run with every
# R12 knob OFF and one with them ON, keeping STRIP.DAT / MODELS.DAT / race.log so
# the A/B is a byte comparison plus a log diff.
$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent $PSScriptRoot
$off  = @{ TD5RE_R12_UP_SHORE = "0"; TD5RE_R12_UP_PIERCLEAR = "0";
           TD5RE_R12_UP_PIERGUARD = "0"; TD5RE_R12_UP_XGATE = "0";
           TD5RE_R12_TERMCAP = "0"; TD5RE_R12_RAIL_WBEAM = "0" }
$port = 37080
foreach ($seed in 20260901, 99991, 777) {
    foreach ($mode in "off", "on") {
        $tag = "V$seed-$mode"
        $k = ($mode -eq "off") ? $off : @{}
        pwsh -NoProfile -File (Join-Path $PSScriptRoot "r12op_run.ps1") `
             -Seed $seed -Span 300 -Tag $tag -Wait 100 -Port $port -Knobs $k 2>&1 |
             Select-String -Pattern "PID=|frame ->|NO FRAME"
        $port++
    }
}
