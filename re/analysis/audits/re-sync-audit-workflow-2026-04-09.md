# RE Sync Audit Workflow — 2026-04-09

Goal: make drift between live Ghidra state, derived artifacts, and maintained notes
obvious and cheap to detect.

## Canonical Inputs

- Live function export:
  - `temp_analysis/td5_inventory_live_t5_renamed.tsv`
- Derived JSON snapshots:
  - `exe_func_bytes.json`
  - `exe_func_mnemonics.json`
  - `exe_func_mnem20.json`
- Maintained count summaries:
  - `function-mapping-confidence-audit-2026-04-08.md`
  - `low-confidence-function-pass-2026-04-08.md`

## Audit Command

Run:

```bash
python tools/check_re_sync.py
```

The checker currently enforces:

- inventory exists and is non-empty
- JSON snapshot key sets match the inventory addresses exactly
- names match between the live export and all three JSON snapshots
- sizes match between the live export and all three JSON snapshots
- headline counts in `function-mapping-confidence-audit-2026-04-08.md` still match the live export

## What Should Fail The Audit

Fail immediately when any of these happen:

1. total address set mismatch
2. renamed function present in inventory but stale in a JSON snapshot
3. size drift between the inventory and a snapshot
4. live inventory contains new `FUN_...` names that the docs claim are gone
5. maintained headline counts drift from the live inventory baseline

## What Should Not Block The Audit

These are worth noting, but they should not fail the sync check by themselves:

- new prose docs under `re/analysis/`
- new scenario packs
- confidence or evidence notes that add context without changing machine-readable invariants

## Recommended Use

Run the sync audit:

1. after any Ghidra rename pass
2. after regenerating `exe_func_*.json`
3. before writing or updating summary docs that quote live counts
4. before claiming a naming or inventory milestone in the repo

## Next Improvements

1. Add a second checker mode for “expected docs present”:
   - `re/RE_STATUS.md`
   - known deviations ledger
   - race quality pack
2. Add a light check for stale hardcoded counts inside old archive notes.
3. Add a “last verified against live inventory” footer convention for high-value docs.
