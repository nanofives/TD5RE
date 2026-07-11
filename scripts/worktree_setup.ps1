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

# 1a. re/assets -- DO NOT JUNCTION (2026-07-10: a junction here let another
# worktree's teardown cascade-delete the parent's entire re/assets tree
# TWICE in one session -- same failure class as the td5mod\deps\mingw
# junction incident below, just hitting a different shared directory).
# Copy instead: slower to set up (~11.5k files) but the worktree owns its
# own bytes, so no other worktree's cleanup can ever touch the parent's copy.
$assetSrc = Join-Path $parent 're\assets'
$assetDst = Join-Path $wt 're\assets'
if ((Test-Path $assetSrc) -and -not (Test-Path $assetDst)) {
    $assetParentDir = Split-Path $assetDst -Parent
    if (-not (Test-Path $assetParentDir)) {
        New-Item -ItemType Directory -Path $assetParentDir -Force | Out-Null
    }
    New-Item -ItemType Directory -Path $assetDst -Force | Out-Null
    robocopy $assetSrc $assetDst /E /NFL /NDL /NJH /NJS /NC /NS /NP | Out-Null
    $global:LASTEXITCODE = 0   # robocopy's exit codes are bitflags; 0-7 all mean success, not an error
    Write-Host "  +    re\assets (copied, not junctioned)"
} elseif (Test-Path $assetDst) {
    Write-Host "  ok   re\assets (already present)"
}

# 1b. td5mod\deps — DO NOT JUNCTION. Worktree auto-cleanup follows junctions and
# DELETES the parent's mingw toolchain (twice in one session, 2026-05-16).
# Instead: patch the worktree's build_standalone.bat to point at the parent's
# absolute mingw path. No junction = no cleanup-cascade = parent's deps safe.
$buildBat = Join-Path $wt 'td5mod\src\td5re\build_standalone.bat'
if (Test-Path $buildBat) {
    $batContent = Get-Content $buildBat -Raw
    $parentMingw = (Join-Path $parent 'td5mod\deps\mingw\mingw32\bin').Replace('\','\')
    $parentZlibInc = (Join-Path $parent 'td5mod\deps\mingw\mingw32\i686-w64-mingw32\include')
    if ($batContent -match '\.\.\\\.\.\\deps\\mingw\\mingw32\\bin') {
        $newContent = $batContent `
            -replace '\.\.\\\.\.\\deps\\mingw\\mingw32\\bin', $parentMingw `
            -replace '\.\.\\\.\.\\deps\\mingw\\mingw32\\i686-w64-mingw32\\include', $parentZlibInc
        Set-Content -Path $buildBat -Value $newContent -NoNewline
        Write-Host "  +    build_standalone.bat (patched to use parent's mingw at $parentMingw)"
    } else {
        Write-Host "  ok   build_standalone.bat (already absolute or different pattern)"
    }
} else {
    Write-Host "  miss td5mod\src\td5re\build_standalone.bat (not in worktree?)"
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
