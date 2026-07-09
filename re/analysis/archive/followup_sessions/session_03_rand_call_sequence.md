# Session 03 — InitializeRaceSeriesSchedule rand call sequence alignment

## Goal
Make port and orig produce identical AI car selection given identical
`srand(0x1A2B3C4D)` seed. Currently MSVC rand override (LCG constants
0x343FD/0x269EC3) is shipped, BUT port's `InitializeRaceSeriesSchedule` calls
`rand()` in a different pattern than orig `0x0000dac0`, so AI cars still
differ.

## Context — read these first

- `memory/reference_msvc_rand_override_2026-05-21.md` — LCG already MSVC-compatible
- `memory/reference_slot2_cardef_rng_divergence_2026-05-21.md` — diagnosis that proved 3 different cardefs across configs
- `memory/reference_racetrace_rng_alignment_2026-05-21.md` — flags needed for aligned capture
- `td5mod/src/td5re/td5_frontend.c:1662-1820` — port's `InitializeRaceSeriesSchedule` implementation with detailed comments about expected rand sequence

## Observable symptom

With `RaceTrace=1` on port + `--seed-crt true` on orig (Edinburgh AI Viper):
- Slot 1 active_lower_bound diverges by 24u
- Slot 2 active_lower_bound diverges by 24u
- Slot 3/4/5 similarly
- All trace back to different cardef (= different car loaded)

## Approach

1. **Open Ghidra**, decompile orig `0x0000dac0` (InitializeRaceSeriesSchedule).
2. **Map exact rand() call sequence**: per slot per iteration, count calls. Port already documents this in td5_frontend.c:1722-1740 and 1763-1802, with three paths (game_type 2 quick race, game_type 5 cup, default single race).
3. **Identify divergence** — port comment mentions "Prior port version placed slot_variant[i] = rand() & 3 OUTSIDE the do/while". Port now claims byte-faithful. Verify against orig disasm. Likely candidates for residual divergence:
   - `frontend_ai_ext_id_taken` dedup function may scan in different order
   - `s_difficulty_tier_cars` table may have different values than orig's tier table
   - Preamble rand() burns before the loop (the comment at td5_frontend.c:1665-1700 admits this is uncertain)
4. **Add port-side log** of every rand() call result during `InitializeRaceSeriesSchedule` (call index + return value). Build, run.
5. **Add Frida probe on orig** hooking `0x0044814c` (rand). Log every call with return value. Run quickrace with `--seed-crt true`.
6. **Diff the two sequences** (each list of rand outputs). Find first divergence.
7. **Fix** by adjusting port's loop structure / dedup / table to match.

## Tools

- Ghidra MCP for orig disasm
- New Frida probe: `tools/_probes/orig_rand_trace.js`
- Port-side TD5_LOG_I in td5_frontend.c

## Success criteria

After fix:
- Port and orig with same seed produce identical AI car indices for slots 1-5
- Pool13 paired diff shows slot 1/2/3/4/5 active_bound divergences all close
- Slot 0 11u bias residue (from earlier session) closes as side-effect

## Files likely touched

- `td5mod/src/td5re/td5_frontend.c:1662-1820` (rand call site adjustments)
- Possibly `s_difficulty_tier_cars` table values (in same file)
- `tools/_probes/orig_rand_trace.js` (new)

## Risk
MEDIUM. Each rand() call shifts the entire downstream sequence. Test carefully — wrong fix shifts cars differently without converging.

## Estimated time
2-3 hours.
