# launch_autotrack_feedback.ps1  (untracked dev helper, safe to delete)
#
# Launches td5re.exe on a PINNED auto-track seed with EVERY feature knob on,
# so geometry picks stay reproducible between sessions.
#
# WHY THIS EXISTS: the generator REUSES the on-disk build in
# re/assets/levels/level090 whenever the seed matches. A bare double-click sets
# no seed and no knobs, so it builds a DIFFERENT track and every "entry N" you
# reported stops pointing at the same mesh. Always relaunch through this script.
#
#   pwsh -NoProfile -File .\launch_autotrack_feedback.ps1            # reuse build
#   pwsh -NoProfile -File .\launch_autotrack_feedback.ps1 -Regen     # force rebuild

param([switch]$Regen)

$ErrorActionPreference = 'Stop'
Set-Location -LiteralPath $PSScriptRoot     # level090 is written RELATIVE to CWD

# ---- the pinned card -------------------------------------------------------
# Seed 5150, chosen by rolling seeds WITH the full knob set active (global water
# changes the route, so the seed had to be picked under the same conditions).
# 2209 spans. Biomes:
#   0-149 COAST | 150-299 INDUSTRIAL | 300-599 COAST | 600-749 FIELDS
#   750-899 ORIENTAL | 900-1199 CITY | 1200-1349 ORIENTAL
#   1350-1499 INDUSTRIAL | 1500-1949 COAST | 1950-2208 ORIENTAL
# Forks (as of the R20 placement fix -- these MOVED, they used to be the same
# 144/301/712/895/1106/1262 on every seed):
#   0 MAJOR  F=224  len 60   corridor 1801-1860
#   1 ISLAND F=443  len 5    corridor 1862-1866
#   2 AVENUE F=607  len 40   corridor 1868-1907
#   3 ISLAND F=806  len 6    corridor 1909-1914
#   4 WIDE   F=971  len 260  corridor 1916-2175
#   5 SLIP   F=1390 len 32   corridor 2177-2208
$env:TD5RE_AUTOTRACK_SEED = '5150'

# ---- EVERY feature knob ON -------------------------------------------------
# Previously-OFF-by-default features, all enabled by request.
$env:TD5RE_R8_BIOME_SEA              = '1'   # one-side sea -> real coastlines
$env:TD5RE_AUTOTRACK_PARKS           = '1'   # city parks
$env:TD5RE_R9_UNDERPASS              = '1'   # city underpasses
$env:TD5RE_AUTOTRACK_CROSS_JOIN      = '1'   # joined crossings
$env:TD5RE_AUTOTRACK_BLOCK_TURNS     = '1'   # block turn continuation
$env:TD5RE_R8_LONGRUN                = '1'   # longer straight runs
$env:TD5RE_R8_SHAPE_LONGBRANCH       = '1'   # long diverging branch
$env:TD5RE_R9_INFRA_PONDS            = '1'   # roadside ponds (no bank/basin yet)
$env:TD5RE_R16_CITY_SKIRT_CULL       = '1'   # drop grass hidden behind a frontage
$env:TD5RE_R16_TREELINE_VSCALE       = '1'   # 1.8x vertical tree-line tiling
$env:TD5RE_R17_GLOBAL_WATER          = '1'   # one sea height + route floor clamp
# Percentile picked by measurement, not guessed. Measured on the PRE-R20 fork
# layout, where nodes clamped flat were:
#   P10 275 (15.3%, max lift 2834)   <- the old default
#   P5  188 (10.4%, max lift 1733)
#   P3  160 ( 8.9%, max lift 1399)   <- chosen: most of the gain
#   P2  151 ( 8.4%, max lift 1287)   <- diminishing returns
# The R20 fork-placement fix changed the route, and P3 now clamps far less on
# this seed: 83/1801 nodes (4.6%, max lift 2252, sea -27603). Re-measure before
# trusting the table above on a different seed.
$env:TD5RE_R17_WATER_LEVEL_PCT       = '3'
$env:TD5RE_R17_BRIDGE_SKIRT_RUNWIDE  = '1'   # seaward skirt dropped along whole run
$env:TD5RE_R18_RUNOUT_FLATCAP        = '1'   # trim the invisible run-out apron
$env:TD5RE_R18_BRIDGE_APPROACH_SHORE = '1'   # shore drop on the bridge approach
$env:TD5RE_AUTOTRACK_TREELINE_PNG    = '1'   # native-res tree-line pages (256)

# Diagnostics, threading and double-build stay OFF: they are not features, and
# enabling the _REPORT/_LOG/_DIAG family floods the log and slows generation.

$env:TD5RE_WINDOW_TITLE = 'TD5RE FEEDBACK  seed 5150  ALL KNOBS ON  (pause -> FREE CAMERA -> hover, LMB copies)'

# Belt and braces: the sim-tick cap requests a clean QUIT gated ONLY on
# race_trace_max_sim_ticks > 0 -- it does NOT check whether RaceTrace is on. With
# the stock value of 1000 the game closes itself after ~33 s of race even with
# tracing disabled, which reads exactly like a crash.
if ($Regen) {
    $lvl = Join-Path $PSScriptRoot 're\assets\levels\level090'
    if (Test-Path $lvl) { Remove-Item -LiteralPath $lvl -Recurse -Force }
    Write-Host 'level090 removed - the track will be REBUILT (takes ~20-40 s under the splash).'
}

Write-Host "seed        : $env:TD5RE_AUTOTRACK_SEED  (COAST x3, CITY 300 spans, FIELDS, INDUSTRIAL x2, ORIENTAL x3)"
Write-Host 'features    : ALL feature knobs ON, including the four that were OFF'
Write-Host 'water       : P3 -> clamps ~5% of nodes flat (was ~15% at the old P10)'
Write-Host 'forks       : 224 443 607 806 971 1390  (R20: these now vary by seed)'
Write-Host 'Pause (Esc) -> FREE CAMERA to fly. Hover geometry, LEFT-CLICK copies one line.'

$p = Start-Process -FilePath (Join-Path $PSScriptRoot 'td5re.exe') `
                   -ArgumentList '--RaceTraceMaxSimTicks=0','--RaceTrace=0' -PassThru
Write-Host "PID=$($p.Id)"
