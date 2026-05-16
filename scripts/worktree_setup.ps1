# worktree_setup.ps1
#
# Bootstrap a fresh agent worktree so td5re.exe + the replay-diff harness can
# build and run. Junctions parent's gitignored directories (td5mod/deps,
# re/assets) and copies the gitignored tools/ scripts the harness depends on.
#
# Run from the worktree root:
#   powershell -NoProfile -ExecutionPolicy Bypass -File `
#       C:\Users\maria\Desktop\Proyectos\TD5RE\scripts\worktree_setup.ps1
#
# Also optionally copies per-scenario snapshot bins if -Scenarios is passed:
#   ... worktree_setup.ps1 -Scenarios "honolulu_ai_viper,edinburgh_ai_viper"

param(
    [string]$Scenarios = ""
)

$ErrorActionPreference = "Continue"
$parent = "C:\Users\maria\Desktop\Proyectos\TD5RE"
$wt     = (Get-Location).Path

if ($wt -eq $parent) {
    Write-Host "Setup must be run from a worktree, not the main repo." -ForegroundColor Red
    exit 1
}

Write-Host "Setting up worktree at $wt"

# 1. Junctions for parent's gitignored directories
foreach ($sub in @('td5mod\deps','re\assets')) {
    $src = Join-Path $parent $sub
    $dst = Join-Path $wt $sub
    if (-not (Test-Path $src)) {
        Write-Host "  skip $sub (parent missing)"
        continue
    }
    if (Test-Path $dst) {
        Write-Host "  ok   $sub (already linked)"
        continue
    }
    $parentDir = Split-Path $dst -Parent
    if (-not (Test-Path $parentDir)) {
        New-Item -ItemType Directory -Path $parentDir -Force | Out-Null
    }
    cmd /c "mklink /J `"$dst`" `"$src`"" | Out-Null
    if (Test-Path $dst) {
        Write-Host "  +    $sub (junctioned)"
    } else {
        Write-Host "  FAIL $sub" -ForegroundColor Red
    }
}

# 2. Copy gitignored tools/ scripts the harness needs
$toolsDir = Join-Path $wt 'tools'
$csvDir   = Join-Path $toolsDir 'frida_csv'
New-Item -ItemType Directory -Path $csvDir -Force | Out-Null

$wantedTools = @(
    'diff_replay_frames.py',
    'diff_whole_state.py',
    'td5_actor_offsets.py',
    'run_state_replay.py',
    'frida_state_snapshot.js',
    'frida_whole_state_snapshot.js',
    'run_whole_state_diff.py'
)
foreach ($f in $wantedTools) {
    $src = Join-Path $parent "tools\$f"
    $dst = Join-Path $toolsDir $f
    if ((Test-Path $src) -and -not (Test-Path $dst)) {
        Copy-Item $src $dst
        Write-Host "  +    tools\$f"
    } elseif (Test-Path $dst) {
        Write-Host "  ok   tools\$f"
    } else {
        Write-Host "  miss tools\$f (parent doesn't have it)"
    }
}

# 3. Copy scenario snapshot bins
if ($Scenarios -ne "") {
    foreach ($sc in $Scenarios.Split(',')) {
        $sc = $sc.Trim()
        if ($sc -eq "") { continue }
        $src = Join-Path $parent "tools\frida_csv\state_snapshot_$sc.bin"
        $dst = Join-Path $csvDir "state_snapshot_$sc.bin"
        if ((Test-Path $src) -and -not (Test-Path $dst)) {
            Copy-Item $src $dst
            Write-Host "  +    tools\frida_csv\state_snapshot_$sc.bin"
        }
    }
}

# Always also copy the canonical Honolulu snapshot used by default
$honolulu = Join-Path $parent 'tools\frida_csv\state_snapshot_original.bin'
$honoluluDst = Join-Path $csvDir 'state_snapshot_original.bin'
if ((Test-Path $honolulu) -and -not (Test-Path $honoluluDst)) {
    Copy-Item $honolulu $honoluluDst
    Write-Host "  +    tools\frida_csv\state_snapshot_original.bin"
}

Write-Host "Worktree setup complete."
