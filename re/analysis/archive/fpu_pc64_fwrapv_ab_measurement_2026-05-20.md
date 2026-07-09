# A/B measurement — FPU PC=64 + `-fwrapv` impact on whole-state diff

**Date:** 2026-05-20
**Method:** Stashed the three shipped edits, rebuilt pre-fix, captured port bin; unstashed, rebuilt post-fix; diffed both against `tools/frida_csv/whole_state_original.bin` (orig binary unchanged since May 15 capture).

## Scenario

Whole-state diff harness as currently configured (`tools/run_whole_state_diff.py` BASELINE):
- Track 8 (Honolulu), Car 0 (Viper), PlayerIsAI=1
- `WholeStateMaxTicks=8`, `RaceTraceMaxSimTicks=8`
- 8 sim ticks captured, 7 matched (last tick out of range)

## Result

| Sim tick | Pre-fix divergent fields | Post-fix divergent fields | Delta |
|---|---|---|---|
| sim_tick=1 | **442** | **404** | **-38 (-8.6%)** |
| sim_tick=2..7 | 0 | 0 | 0 |

(The "0" rows are suspicious — sim_tick numbering may not align 1:1 with the historical "sub121" residue baselines that referenced 174/243/189 fields per scenario. The current 8-tick capture only reaches the moments immediately around race-start. Steady-state physics fields don't appear in this slice.)

## Interpretation

**The fixes are doing real work but not closing the bulk of the residue.**

- 38 fields out of 442 are now byte-faithful that weren't before.
- Most divergence at sim_tick=1 is dominated by countdown/scenario-state mismatches (e.g., `engine_speed_accum`: port=400 idle vs orig=5985 revving; `steering_command`: port=0 vs orig=-23776). These are SEQUENCING bugs, not arithmetic precision — the FPU/IMUL fixes don't help them.
- The 38 fields that DID converge are presumably the ones where FPU intermediate precision (PC=64 vs default PC=53) or signed multiply wraparound mattered to the bit-level result.

## Deeper measurement (not run here)

To replicate the historical "sub121" baselines (174/243/189 for Honolulu_Human/Edinburgh/Moscow), the BASELINE in `run_whole_state_diff.py:51` would need `WholeStateMaxTicks` and `RaceTraceMaxSimTicks` bumped to ~130 sim ticks, and the scenario varied across the three tracks. Each scenario needs both an orig recapture and a port recapture.

Not done in this measurement pass — out of scope for the A/B check. User can run it manually:

```bash
# 1. Edit tools/run_whole_state_diff.py BASELINE:
#    "Trace.WholeStateMaxTicks": "130",
#    "Trace.RaceTraceMaxSimTicks": "130",
# 2. Capture deep tick:
python tools/run_whole_state_diff.py --capture-original
python tools/run_whole_state_diff.py --capture-port
python tools/run_whole_state_diff.py --summary
```

Repeat per scenario (track 8 = Honolulu AI, track 10 = Edinburgh, track 11 = Moscow — check `td5_quickrace.ini` for IDs).

## Conclusion

The PC=64 + `-fwrapv` ship is a **net positive but small win** at the 8-tick window measured. The remaining ~400 fields of residue at sim_tick=1 are dominated by *scenario-state divergences* (engine, steering, suspension all uninitialized in port) not by FP/integer arithmetic precision. The two known open TODOs about countdown state ([[todo-camera-lags-post-race-start]], state-3 frontend gates) are likely better leverage now.

## Status of recommendations

1. **Measure cluster reduction from shipped fixes** — DONE. 8.6% at sim_tick=1.
2. **PageHeap for uninit-reads** — REQUIRES USER. `appverif.exe` GUI + admin. Procedure in `re/analysis/imul_wrapv_audit_2026-05-20.md`.
3. **Per-callsite FISTP audit via Ghidra** — BLOCKED. Another Ghidra session is active. Pool slots locked. Defer until next session.

Files preserved for future comparison:
- `log/whole_state_port_prefix.bin` — port without PC=64 + `-fwrapv`
- `log/whole_state_port_postfix.bin` — port with both fixes
- `tools/frida_csv/whole_state_original.bin` — orig baseline (unchanged since May 15)
