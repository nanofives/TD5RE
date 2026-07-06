# lint_structure.ps1 — structural ratchets for td5mod/src/td5re.
#
# Guards the architecture cleanups (2026-07-06 structure batch) against decay.
# Metrics only ever ratchet DOWN: the checked-in baseline records the current
# debt; any INCREASE fails, any decrease prints a hint to lower the baseline.
#
#   pwsh scripts/lint_structure.ps1                      # compare vs baseline (exit 1 on regression)
#   pwsh scripts/lint_structure.ps1 -ReportOnly          # print, never fail (local builds)
#   pwsh scripts/lint_structure.ps1 -UpdateBaseline      # rewrite baseline from current tree
#   pwsh scripts/lint_structure.ps1 -BuildLog <path>     # also ratchet compiler warnings from a build log
#
# Metrics:
#   1. extern_in_c    — `extern` declarations inside .c files (API bypass;
#                       cross-module symbols belong in the owning module's header).
#   2. game_h_includers — the set of files including "td5_game.h". The race-FSM
#                       header is the repo's dependency magnet (32 includers at
#                       baseline); leaf modules must migrate to td5_race_state.h
#                       and NEVER join this list.
#   3. warnings       — per -W<flag> counts from the build log (optional).
#
# Vendored code (cJSON) is excluded from all metrics.

param(
    [switch]$ReportOnly,
    [switch]$UpdateBaseline,
    [string]$BuildLog = ""
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$srcDir = Join-Path $repo 'td5mod\src\td5re'
$baselinePath = Join-Path $PSScriptRoot 'lint_structure_baseline.json'
$vendored = @('cJSON.c', 'cJSON.h')

$fail = @()
$lowered = @()

# ---------------------------------------------------------------- metric 1: extern in .c
$externPerFile = @{}
Get-ChildItem $srcDir -Filter '*.c' | Where-Object { $vendored -notcontains $_.Name } | ForEach-Object {
    $n = (Select-String -Path $_.FullName -Pattern '^\s*extern\s+[A-Za-z_]' -AllMatches).Count
    if ($n -gt 0) { $externPerFile[$_.Name] = $n }
}
$externTotal = ($externPerFile.Values | Measure-Object -Sum).Sum
if (-not $externTotal) { $externTotal = 0 }

# ---------------------------------------------------------------- metric 2: td5_game.h includers
$gameIncluders = Get-ChildItem $srcDir -Include '*.c','*.h' -Recurse |
    Where-Object { $vendored -notcontains $_.Name -and $_.Name -ne 'td5_game.h' } |
    Where-Object { Select-String -Path $_.FullName -Pattern '#include\s+"td5_game\.h"' -Quiet } |
    ForEach-Object { $_.Name } | Sort-Object

# ---------------------------------------------------------------- metric 3: warnings (optional)
$warnPerFlag = $null
$warnTotal = $null
if ($BuildLog -and (Test-Path $BuildLog)) {
    $warnLines = Select-String -Path $BuildLog -Pattern 'warning:' | ForEach-Object { $_.Line }
    $warnTotal = $warnLines.Count
    $warnPerFlag = @{}
    foreach ($l in $warnLines) {
        if ($l -match '\[-W([a-z0-9=_-]+)\]') { $flag = $Matches[1] } else { $flag = '(unflagged)' }
        $warnPerFlag[$flag] = 1 + ($warnPerFlag[$flag] ?? 0)
    }
}

# ---------------------------------------------------------------- baseline handling
$current = [ordered]@{
    extern_in_c_total  = $externTotal
    extern_in_c        = $externPerFile
    game_h_includers   = @($gameIncluders)
}
if ($null -ne $warnTotal) {
    $current.warnings_total    = $warnTotal
    $current.warnings_per_flag = $warnPerFlag
}

if ($UpdateBaseline) {
    if ($null -eq $warnTotal) {
        # Preserve any existing warning baseline when no log was supplied.
        if (Test-Path $baselinePath) {
            $old = Get-Content $baselinePath -Raw | ConvertFrom-Json
            if ($null -ne $old.warnings_total) {
                $current.warnings_total = $old.warnings_total
                $current.warnings_per_flag = @{}
                $old.warnings_per_flag.PSObject.Properties | ForEach-Object { $current.warnings_per_flag[$_.Name] = $_.Value }
            }
        }
    }
    $current | ConvertTo-Json -Depth 5 | Set-Content $baselinePath -Encoding UTF8
    Write-Host "baseline written: $baselinePath"
    Write-Host ("  extern_in_c_total = {0}, game_h_includers = {1}, warnings_total = {2}" -f
        $current.extern_in_c_total, $current.game_h_includers.Count, ($current.warnings_total ?? 'n/a'))
    exit 0
}

if (-not (Test-Path $baselinePath)) {
    Write-Error "No baseline at $baselinePath — run with -UpdateBaseline first."
    exit 2
}
$base = Get-Content $baselinePath -Raw | ConvertFrom-Json

# extern ratchet (total + per-file so a new bypass in a clean file is caught
# even when another file was cleaned up in the same change)
if ($externTotal -gt $base.extern_in_c_total) {
    $fail += "extern-in-.c total rose: $($base.extern_in_c_total) -> $externTotal"
} elseif ($externTotal -lt $base.extern_in_c_total) {
    $lowered += "extern_in_c_total $($base.extern_in_c_total) -> $externTotal"
}
foreach ($f in $externPerFile.Keys) {
    $b = $base.extern_in_c.PSObject.Properties[$f]
    $bv = if ($b) { [int]$b.Value } else { 0 }
    if ($externPerFile[$f] -gt $bv) {
        $fail += "extern decls in $f rose: $bv -> $($externPerFile[$f]) (declare in the owning module's header instead)"
    }
}

# td5_game.h includer set ratchet
$baseIncluders = @($base.game_h_includers)
$newIncluders = @($gameIncluders | Where-Object { $baseIncluders -notcontains $_ })
$goneIncluders = @($baseIncluders | Where-Object { $gameIncluders -notcontains $_ })
if ($newIncluders) {
    $fail += "NEW td5_game.h includer(s): $($newIncluders -join ', ') — use td5_race_state.h (narrow query API) instead"
}
if ($goneIncluders) { $lowered += "game_h_includers dropped: $($goneIncluders -join ', ')" }

# warning ratchet (per-flag + total)
if ($null -ne $warnTotal -and $null -ne $base.warnings_total) {
    if ($warnTotal -gt $base.warnings_total) {
        $fail += "build warnings rose: $($base.warnings_total) -> $warnTotal"
    } elseif ($warnTotal -lt $base.warnings_total) {
        $lowered += "warnings_total $($base.warnings_total) -> $warnTotal"
    }
    foreach ($flag in $warnPerFlag.Keys) {
        $b = $base.warnings_per_flag.PSObject.Properties[$flag]
        $bv = if ($b) { [int]$b.Value } else { 0 }
        if ($warnPerFlag[$flag] -gt $bv) {
            $fail += "warning class -W$flag rose: $bv -> $($warnPerFlag[$flag])"
        }
    }
}

# ---------------------------------------------------------------- report
Write-Host "lint_structure: extern_in_c=$externTotal (baseline $($base.extern_in_c_total)), game_h_includers=$($gameIncluders.Count) (baseline $($baseIncluders.Count))$(if ($null -ne $warnTotal) { ", warnings=$warnTotal (baseline $($base.warnings_total))" })"

if ($lowered) {
    Write-Host "IMPROVED (consider -UpdateBaseline to lock in):" -ForegroundColor Green
    $lowered | ForEach-Object { Write-Host "  + $_" -ForegroundColor Green }
}
if ($fail) {
    Write-Host "STRUCTURE REGRESSIONS:" -ForegroundColor Red
    $fail | ForEach-Object { Write-Host "  ! $_" -ForegroundColor Red }
    if ($ReportOnly) {
        Write-Host "(report-only mode: not failing the build — CI will)" -ForegroundColor Yellow
        exit 0
    }
    exit 1
}
Write-Host "structure lint OK"
exit 0
