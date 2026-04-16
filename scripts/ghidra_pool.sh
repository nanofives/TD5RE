#!/bin/bash
# ghidra_pool.sh — Manage a pool of Ghidra project clones for parallel sessions
#
# Ghidra locks .gpr files at the project level, so multiple Claude Code sessions
# cannot share a single project. This script maintains N clones of the master
# TD5.gpr/TD5.rep so each session can acquire its own slot.
#
# Usage:
#   ghidra_pool.sh init [N]      — Create N slots (default: 4) from master
#   ghidra_pool.sh acquire       — Print the first unlocked slot name (e.g. TD5_pool2)
#   ghidra_pool.sh release [N]   — Clean lock files for slot N (or all if omitted)
#   ghidra_pool.sh sync          — Refresh all unlocked slots from master TD5.rep
#   ghidra_pool.sh status        — Show lock status of all slots
#   ghidra_pool.sh cleanup       — Remove stale .lock/.lock~ files for all slots

set -euo pipefail

PROJECT_ROOT="C:/Users/maria/Desktop/Proyectos/TD5RE"
MASTER_GPR="$PROJECT_ROOT/TD5.gpr"
MASTER_REP="$PROJECT_ROOT/TD5.rep"
POOL_DIR="$PROJECT_ROOT/ghidra_pool"
DEFAULT_SLOTS=4

# ---- Helpers ----

slot_gpr() { echo "$POOL_DIR/TD5_pool${1}.gpr"; }
slot_rep() { echo "$POOL_DIR/TD5_pool${1}.rep"; }
slot_lock() { echo "$POOL_DIR/TD5_pool${1}.lock"; }

is_locked() {
    local lock="$(slot_lock "$1")"
    # Ghidra creates both .lock and .lock~ — check the main lock file
    [[ -f "$lock" ]] && return 0
    return 1
}

# ---- Commands ----

cmd_init() {
    local n="${1:-$DEFAULT_SLOTS}"
    mkdir -p "$POOL_DIR"

    if [[ ! -f "$MASTER_GPR" ]]; then
        echo "ERROR: Master project $MASTER_GPR not found"
        exit 1
    fi

    echo "Creating $n pool slots from master TD5 project..."
    for i in $(seq 0 $((n - 1))); do
        local gpr="$(slot_gpr "$i")"
        local rep="$(slot_rep "$i")"

        # Create .gpr marker (empty file — Ghidra matches by filename)
        touch "$gpr"

        # Clone .rep directory (full copy — 37MB each)
        if [[ -d "$rep" ]]; then
            echo "  Slot $i: refreshing existing clone..."
            rm -rf "$rep"
        else
            echo "  Slot $i: creating new clone..."
        fi
        cp -r "$MASTER_REP" "$rep"

        # Clean any stale locks
        rm -f "$(slot_lock "$i")" "$(slot_lock "$i")~"
    done

    echo "Pool initialized: $n slots in $POOL_DIR"
    cmd_status
}

cmd_acquire() {
    if [[ ! -d "$POOL_DIR" ]]; then
        echo "ERROR: Pool not initialized. Run: ghidra_pool.sh init" >&2
        exit 1
    fi

    # Find first unlocked slot
    for gpr in "$POOL_DIR"/TD5_pool*.gpr; do
        [[ -f "$gpr" ]] || continue
        local slot=$(echo "$gpr" | sed 's/.*pool\([0-9]*\).*/\1/')
        if ! is_locked "$slot"; then
            # Output in a machine-readable format for /fix to parse
            echo "TD5_pool${slot}"
            exit 0
        fi
    done

    echo "ERROR: All pool slots are locked. Run: ghidra_pool.sh release" >&2
    exit 1
}

cmd_release() {
    if [[ -n "${1:-}" ]]; then
        # Release specific slot
        rm -f "$(slot_lock "$1")" "$(slot_lock "$1")~"
        echo "Released slot $1"
    else
        # Release all
        rm -f "$POOL_DIR"/*.lock "$POOL_DIR"/*.lock~
        echo "Released all pool slots"
    fi
}

cmd_sync() {
    if [[ ! -d "$POOL_DIR" ]]; then
        echo "ERROR: Pool not initialized." >&2
        exit 1
    fi

    echo "Syncing unlocked pool slots from master..."
    local synced=0
    local skipped=0

    for gpr in "$POOL_DIR"/TD5_pool*.gpr; do
        [[ -f "$gpr" ]] || continue
        local slot=$(echo "$gpr" | sed 's/.*pool\([0-9]*\).*/\1/')
        local rep="$(slot_rep "$slot")"

        if is_locked "$slot"; then
            echo "  Slot $slot: LOCKED (skipping)"
            ((skipped++))
        else
            echo "  Slot $slot: syncing..."
            rm -rf "$rep"
            cp -r "$MASTER_REP" "$rep"
            ((synced++))
        fi
    done

    # Also clean master lock files (stale from previous sessions)
    rm -f "$PROJECT_ROOT/TD5.lock" "$PROJECT_ROOT/TD5.lock~"
    rm -f "$PROJECT_ROOT/TD5_headless.lock" "$PROJECT_ROOT/TD5_headless.lock~"

    echo "Sync complete: $synced refreshed, $skipped skipped (locked)"
}

cmd_status() {
    if [[ ! -d "$POOL_DIR" ]]; then
        echo "Pool not initialized."
        exit 0
    fi

    echo "Ghidra Pool Status:"
    echo "  Master: $MASTER_GPR"
    echo "  Pool:   $POOL_DIR"
    echo ""

    for gpr in "$POOL_DIR"/TD5_pool*.gpr; do
        [[ -f "$gpr" ]] || continue
        local slot=$(echo "$gpr" | sed 's/.*pool\([0-9]*\).*/\1/')
        if is_locked "$slot"; then
            echo "  Slot $slot: LOCKED"
        else
            echo "  Slot $slot: available"
        fi
    done

    # Also show master/headless status
    echo ""
    [[ -f "$PROJECT_ROOT/TD5.lock" ]] && echo "  Master TD5.gpr: LOCKED" || echo "  Master TD5.gpr: available"
    [[ -f "$PROJECT_ROOT/TD5_headless.lock" ]] && echo "  TD5_headless.gpr: LOCKED" || echo "  TD5_headless.gpr: available"
}

cmd_cleanup() {
    echo "Cleaning all stale Ghidra lock files..."
    rm -f "$POOL_DIR"/*.lock "$POOL_DIR"/*.lock~ 2>/dev/null
    rm -f "$PROJECT_ROOT/TD5.lock" "$PROJECT_ROOT/TD5.lock~"
    rm -f "$PROJECT_ROOT/TD5_headless.lock" "$PROJECT_ROOT/TD5_headless.lock~"
    rm -f "$PROJECT_ROOT/TD5_mcp.lock" "$PROJECT_ROOT/TD5_mcp.lock~"
    echo "Done."
}

# ---- Main ----

case "${1:-status}" in
    init)     cmd_init "${2:-}"   ;;
    acquire)  cmd_acquire         ;;
    release)  cmd_release "${2:-}" ;;
    sync)     cmd_sync            ;;
    status)   cmd_status          ;;
    cleanup)  cmd_cleanup         ;;
    *)
        echo "Usage: ghidra_pool.sh {init|acquire|release|sync|status|cleanup}"
        exit 1
        ;;
esac
