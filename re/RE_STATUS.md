# RE Status

Last updated: 2026-04-09

This is the current project-wide status page for the TD5 reverse-engineering and
source-port effort. It is intentionally high-level and points to the deeper
evidence packs under `re/analysis/`.

## Current Baseline

- Live Ghidra inventory baseline: `876` total functions, `876` named, `0` `FUN_...`, `843` `USER_DEFINED`
- Machine-readable function snapshots are currently in sync with the live export
- The standalone source port builds successfully
- The largest remaining gameplay-fidelity risks are concentrated in the race path, not the frontend

## Subsystem Confidence

### High

- Frontend/menu flow
- Asset archive and file-format coverage
- Actor layout modeling
- Collision formula mapping
- Core render resource structures

### Medium

- Main race loop staging
- Core player/AI dynamics formulas
- Track geometry traversal and wrap normalization
- Camera state machine and preset coverage
- Save/config persistence

### Medium-Low

- AI route/script alignment behavior
- Race progression ownership
- Split-screen camera coupling during race incidents
- Weather/particle timing parity

### Low

- Suspension-to-attitude coupling
- Recovery / out-of-bounds behavior
- Point-to-point checkpoint progression
- Exact trig / angle lookup fidelity in some source paths
- FMV behavioral parity beyond current stub boundaries

## Current Sources Of Truth

- Live inventory / naming state:
  - `temp_analysis/td5_inventory_live_t5_renamed.tsv`
- Derived function snapshots:
  - `re/analysis/exe_func_bytes.json`
  - `re/analysis/exe_func_mnemonics.json`
  - `re/analysis/exe_func_mnem20.json`
- Race RE quality pack:
  - `re/analysis/race-re-quality-pack-2026-04-09.md`
- Race contradiction ledger:
  - `re/analysis/race-path-contradiction-ledger-2026-04-09.md`
- Race fix tickets:
  - `re/analysis/race-fix-tickets-2026-04-09.md`
- Known deviations ledger:
  - `re/analysis/source-port-known-deviations-2026-04-09.md`

## Required Audit Workflow

Before trusting a new round of notes or source changes:

1. Refresh or confirm the live function inventory export.
2. Run `python tools/check_re_sync.py`.
3. Check the known deviations ledger before triaging a bug as a pure RE miss.
4. For race bugs, use the deterministic validation suite and trace harness before changing broad subsystems.

See:

- `re/analysis/re-sync-audit-workflow-2026-04-09.md`
- `re/analysis/deterministic-validation-suite-2026-04-09.md`

## Most Important Open Questions

1. What is the exact authoritative timing model for the race loop fields in `td5_game.c`?
2. Which source module should own lap/checkpoint completion state?
3. Which exact binary behaviors were disabled or approximated in the suspension/recovery path, and what is the smallest faithful restoration?
4. How much of the remaining “physics feel” gap is actually camera/order/timing drift rather than force-model drift?
5. Which next runtime structs and globals need typed ownership so field usage stops leaking across modules?

## Immediate Next Work

1. Execute the three tickets in `re/analysis/race-fix-tickets-2026-04-09.md` in order.
2. Expand deterministic trace comparison around standing start, lap transition, and wall scrape.
3. Continue typed/global ownership work for the next runtime structs listed in `re/analysis/runtime-struct-global-ownership-pass-2026-04-09.md`.
