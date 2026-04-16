# Ghidra Pool Sync

Propagate Ghidra project changes (function renames, type definitions, comments) across all pool slots. Run this after any session modifies the master TD5 project, or proactively before starting parallel work.

**Usage:** `/ghidra-sync`

## Workflow

### Step 1: Clean all stale locks

```bash
bash "C:/Users/maria/Desktop/Proyectos/TD5RE/scripts/ghidra_pool.sh" cleanup
```

### Step 2: Sync pool from master

```bash
bash "C:/Users/maria/Desktop/Proyectos/TD5RE/scripts/ghidra_pool.sh" sync
```

This copies the master `TD5.rep` to every unlocked pool slot. Locked slots (actively used by another session) are skipped — they'll pick up changes on next sync.

### Step 3: Verify

```bash
bash "C:/Users/maria/Desktop/Proyectos/TD5RE/scripts/ghidra_pool.sh" status
```

Report the result: how many slots synced, how many were skipped (locked).

### Step 4: Cross-account sync (if applicable)

If the `/sync` skill exists and multiple Claude accounts are in use, remind the user:
```
Pool is synced. If you're using multiple Claude accounts, run /sync to propagate memory and session data too.
```
