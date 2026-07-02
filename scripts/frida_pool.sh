#!/bin/bash
# frida_pool.sh — Manage a pool of isolated TD5_d3d.exe working directories
#
# Each slot has its own working dir under frida_pool/slot<N>/ so multiple
# TD5_d3d.exe instances + Frida sessions can run concurrently. Read-only
# game assets are junctioned/hardlinked back to original/ to keep disk cost
# minimal; per-slot writable files (Config.td5, log/) are copied or fresh.
#
# Usage:
#   frida_pool.sh init [N]        — Create N slots (default 16)
#   frida_pool.sh acquire         — Print first unlocked slot dir
#   frida_pool.sh release [N|all] — Clear lock for slot N (or all)
#   frida_pool.sh status          — Show lock state of all slots
#   frida_pool.sh cleanup         — Remove stale .lock files
#   frida_pool.sh destroy [N|all] — Remove slot directories

set -euo pipefail

# Derive the repo root from this script's location (works on any checkout);
# override with TD5RE_ROOT if needed.
PROJECT_ROOT="${TD5RE_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
ORIGINAL_DIR="$PROJECT_ROOT/original"
POOL_DIR="$PROJECT_ROOT/frida_pool"
DEFAULT_SLOTS=16

slot_dir() { echo "$POOL_DIR/slot$1"; }
slot_lock() { echo "$POOL_DIR/slot$1.lock"; }
is_locked() { [[ -f "$(slot_lock "$1")" ]]; }

cmd_init() {
    local n="${1:-$DEFAULT_SLOTS}"
    mkdir -p "$POOL_DIR"
    if [[ ! -d "$ORIGINAL_DIR" ]]; then
        echo "ERROR: $ORIGINAL_DIR not found" >&2; exit 1
    fi

    # Delegate the per-slot link work to PowerShell (much faster than
    # cmd //c per file under MSYS bash). The PS script takes (origDir, poolDir, n).
    local orig_w
    local pool_w
    orig_w="$(cygpath -w "$ORIGINAL_DIR")"
    pool_w="$(cygpath -w "$POOL_DIR")"

    powershell -NoProfile -ExecutionPolicy Bypass -Command "
        \$orig = '$orig_w'
        \$pool = '$pool_w'
        \$n    = $n
        # Binaries we COPY (not hardlink) so each slot can be windowed-patched
        # independently. Per-slot disk cost = ~770KB (TD5_d3d.exe) + 184KB
        # (M2DX.dll) = ~1MB per slot; 16 slots = ~16 MB. Acceptable.
        \$copyBins = @('TD5_d3d.exe','M2DX.dll')
        # Binaries we hardlink (no patching required). Must include ALL DLLs
        # the game loads -- especially the ddraw.dll/winmm.dll DDrawCompat
        # shims that prevent Windows from triggering a desktop-mode change
        # for legacy DirectDraw apps. Missing any of these here = game falls
        # back to Windows' default ddraw, which forces a 640x480 desktop
        # mode switch even with the windowed M2DX patches applied.
        \$linkBins = @('M2DX.dll.bak','M2DXFX.dll','Language.dll','Cup.zip',
                       'ddraw.dll','ddraw_real.dll',
                       'winmm.dll','winmm_real.dll',
                       'Settings.exe','TD5.ini')
        for (\$i = 0; \$i -lt \$n; \$i++) {
            \$d = Join-Path \$pool \"slot\$i\"
            if (-not (Test-Path \$d)) { New-Item -ItemType Directory -Path \$d -Force | Out-Null }
            foreach (\$f in \$copyBins) {
                \$src = Join-Path \$orig \$f
                \$dst = Join-Path \$d \$f
                if ((Test-Path \$src) -and -not (Test-Path \$dst)) {
                    Copy-Item \$src \$dst
                }
            }
            foreach (\$f in \$linkBins) {
                \$src = Join-Path \$orig \$f
                \$dst = Join-Path \$d \$f
                if ((Test-Path \$src) -and -not (Test-Path \$dst)) {
                    New-Item -ItemType HardLink -Path \$dst -Target \$src -Force | Out-Null
                }
            }
            Get-ChildItem (Join-Path \$orig 'level*.zip') -File | ForEach-Object {
                \$dst = Join-Path \$d \$_.Name
                if (-not (Test-Path \$dst)) {
                    New-Item -ItemType HardLink -Path \$dst -Target \$_.FullName -Force | Out-Null
                }
            }
            foreach (\$sub in @('Front End','sound','movie','cars','README','Language')) {
                \$src = Join-Path \$orig \$sub
                \$dst = Join-Path \$d \$sub
                if ((Test-Path \$src) -and -not (Test-Path \$dst)) {
                    New-Item -ItemType Junction -Path \$dst -Target \$src -Force | Out-Null
                }
            }
            \$cfgSrc = Join-Path \$orig 'Config.td5'
            \$cfgDst = Join-Path \$d 'Config.td5'
            if ((Test-Path \$cfgSrc) -and -not (Test-Path \$cfgDst)) {
                Copy-Item \$cfgSrc \$cfgDst
            }
            \$logDir = Join-Path \$d 'log'
            if (-not (Test-Path \$logDir)) { New-Item -ItemType Directory -Path \$logDir | Out-Null }
            Write-Host \"Slot \$i ready\"
        }
    "

    # Apply the windowed patches to each slot's TD5_d3d.exe + M2DX.dll.
    # Both patches accept --exe/--dll overrides so we can target per slot.
    echo "Applying windowed patches to each slot..."
    for i in $(seq 0 $((n - 1))); do
        local d="$(slot_dir "$i")"
        local exe="$d/TD5_d3d.exe"
        local dll="$d/M2DX.dll"
        # Verify the slot files exist before patching
        [[ -f "$exe" ]] || { echo "  Slot $i: TD5_d3d.exe missing, skipping"; continue; }
        [[ -f "$dll" ]] || { echo "  Slot $i: M2DX.dll missing, skipping"; continue; }
        # Run the patch scripts; silence verbose output (only show errors)
        python "$PROJECT_ROOT/re/patches/patch_windowed_td5exe.py" --exe "$exe" > /dev/null 2>&1 \
            && python "$PROJECT_ROOT/re/patches/patch_windowed_m2dx.py" --dll "$dll" > /dev/null 2>&1 \
            && echo "  Slot $i: windowed-patched"  \
            || echo "  Slot $i: PATCH FAILED"
    done

    # Clear all locks (fresh init = all available)
    rm -f "$POOL_DIR"/*.lock 2>/dev/null || true
    echo "Pool initialized: $n slots."
}

cmd_acquire() {
    [[ -d "$POOL_DIR" ]] || { echo "ERROR: pool not initialized" >&2; exit 1; }
    for d in "$POOL_DIR"/slot*; do
        [[ -d "$d" ]] || continue
        local n="${d##*/slot}"
        if ! is_locked "$n"; then
            touch "$(slot_lock "$n")"
            echo "$d"
            exit 0
        fi
    done
    echo "ERROR: all slots locked" >&2; exit 1
}

cmd_release() {
    local arg="${1:-all}"
    if [[ "$arg" == "all" ]]; then
        rm -f "$POOL_DIR"/*.lock 2>/dev/null || true
        echo "Released all slots"
    else
        rm -f "$(slot_lock "$arg")"
        echo "Released slot $arg"
    fi
}

cmd_status() {
    [[ -d "$POOL_DIR" ]] || { echo "Pool not initialized"; return; }
    echo "Frida Pool Status:"
    local locked=0 avail=0
    for d in "$POOL_DIR"/slot*; do
        [[ -d "$d" ]] || continue
        local n="${d##*/slot}"
        if is_locked "$n"; then
            echo "  Slot $n: LOCKED"
            ((locked++))
        else
            echo "  Slot $n: available"
            ((avail++))
        fi
    done
    echo "Total: $((locked + avail)) slots ($locked locked, $avail available)"
}

cmd_cleanup() {
    rm -f "$POOL_DIR"/*.lock 2>/dev/null || true
    echo "All locks cleared."
}

cmd_destroy() {
    local arg="${1:-}"
    [[ -n "$arg" ]] || { echo "Usage: destroy N|all" >&2; exit 1; }
    if [[ "$arg" == "all" ]]; then
        rm -rf "$POOL_DIR"
        echo "Destroyed entire pool"
    else
        rm -rf "$(slot_dir "$arg")" "$(slot_lock "$arg")"
        echo "Destroyed slot $arg"
    fi
}

case "${1:-status}" in
    init)    cmd_init "${2:-}"     ;;
    acquire) cmd_acquire           ;;
    release) cmd_release "${2:-}"  ;;
    status)  cmd_status            ;;
    cleanup) cmd_cleanup           ;;
    destroy) cmd_destroy "${2:-}"  ;;
    *)       echo "Usage: frida_pool.sh {init|acquire|release|status|cleanup|destroy}"; exit 1 ;;
esac
