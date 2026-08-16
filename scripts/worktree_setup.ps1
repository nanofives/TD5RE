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
#
# [2026-08-15] The old guard was `-not (Test-Path $assetDst)`, which silently
# never fired: re\assets is gitignored EXCEPT for two tracked files
# (frontend\lang\es_AR.txt, frontend\menu.ttf), so `git worktree add` always
# creates re\assets to check them out. The directory therefore always existed
# before this ran, the copy was always skipped, and every new worktree got 2
# asset files instead of ~11.8k -- i.e. it could build the game but never run
# it. Test for CONTENT, not for the directory, and let robocopy top up an
# incomplete tree (it skips same-size/same-time files, so a repeat run is cheap
# and this is self-healing for worktrees that were set up while it was broken).
$assetSrc = Join-Path $parent 're\assets'
$assetDst = Join-Path $wt 're\assets'
if (-not (Test-Path $assetSrc)) {
    Write-Host "  WARN parent re\assets missing at $assetSrc -- this worktree will not be able to run the game"
} else {
    # NEVER overwrite the tracked files: they are branch content, and the parent
    # tree is usually on a DIFFERENT branch, so copying its versions over would
    # dirty the worktree with changes that silently came from another branch.
    Push-Location $wt
    $trackedRel = @(git ls-files 're/assets' 2>$null)
    Pop-Location
    $trackedExcl = @()
    foreach ($rel in $trackedRel) {
        if ($rel) { $trackedExcl += (Join-Path $parent ($rel -replace '/', '\')) }
    }

    $before = @(Get-ChildItem -LiteralPath $assetDst -Recurse -File -Force -ErrorAction SilentlyContinue).Count
    $srcCount = @(Get-ChildItem -LiteralPath $assetSrc -Recurse -File -Force -ErrorAction SilentlyContinue).Count
    if ($before -ge ($srcCount - $trackedExcl.Count)) {
        Write-Host "  ok   re\assets ($before files, complete)"
    } else {
        if (-not (Test-Path $assetDst)) { New-Item -ItemType Directory -Path $assetDst -Force | Out-Null }
        $rcArgs = @($assetSrc, $assetDst, '/E', '/NFL', '/NDL', '/NJH', '/NJS', '/NC', '/NS', '/NP')
        if ($trackedExcl.Count) { $rcArgs += '/XF'; $rcArgs += $trackedExcl }
        robocopy @rcArgs | Out-Null
        $global:LASTEXITCODE = 0   # robocopy's exit codes are bitflags; 0-7 all mean success, not an error
        $after = @(Get-ChildItem -LiteralPath $assetDst -Recurse -File -Force -ErrorAction SilentlyContinue).Count
        Write-Host "  +    re\assets (copied, not junctioned): $before -> $after files, $($trackedExcl.Count) tracked file(s) left alone"
    }
}

# 1b. td5mod\deps — DO NOT JUNCTION. Worktree auto-cleanup follows junctions and
# DELETES the parent's mingw toolchain (twice in one session, 2026-05-16).
#
# [2026-08-15] This used to REWRITE the worktree's build_standalone.bat and
# ddraw_wrapper\build.bat with the parent's absolute mingw path. That worked,
# but both files are TRACKED, so every worktree permanently showed two modified
# build scripts -- noise on every `git status`, and a real hazard: an absolute
# C:\Users\... path is one careless `git add` away from breaking CI and every
# other checkout. Instead write an UNTRACKED pointer file at the worktree root;
# both build scripts read it (see the resolution note in build_standalone.bat).
# Still no junction, so the parent's deps stay safe from cleanup-cascade.
$parentMingw64 = Join-Path $parent 'td5mod\deps\mingw64\mingw64\bin'
$mingwPointer  = Join-Path $wt '.td5re_mingw'
if (Test-Path (Join-Path $parentMingw64 'gcc.exe')) {
    Set-Content -Path $mingwPointer -Value $parentMingw64 -NoNewline -Encoding ascii
    Write-Host "  +    .td5re_mingw -> $parentMingw64 (untracked pointer; no tracked file touched)"
} else {
    Write-Host "  WARN parent mingw64 not found at $parentMingw64 -- builds in this worktree will fail loudly"
}

# Undo the historic in-place patch if this worktree was set up by an older
# version of this script, so the tracked build scripts go back to pristine.
foreach ($bat in @('td5mod\src\td5re\build_standalone.bat',
                   'td5mod\ddraw_wrapper\build.bat')) {
    $p = Join-Path $wt $bat
    if ((Test-Path $p) -and ((Get-Content $p -Raw) -match [regex]::Escape($parentMingw64))) {
        Push-Location $wt
        git checkout -- $bat 2>$null
        Pop-Location
        Write-Host "  fix  $bat (reverted stale absolute-path patch)"
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
