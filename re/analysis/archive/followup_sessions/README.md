# TD5RE follow-up session briefs (2026-05-22)

Eight self-contained session briefs, each ready to paste as the first message
of a fresh Claude Code session. Pick one per session — they're independent
unless noted.

## Suggested order (highest impact first)

1. **session_01_ai_stuck_span_137.md** — port AI locks at steer=-16384 from sim_tick 440 (HIGH; blocks AI race completion)
2. **session_02_rotation_matrix_fpu.md** — root cause of countdown contact_delta cascade (DEEP; multi-session; cascades to launches at all jump spans)
3. **session_03_rand_call_sequence.md** — port `InitializeRaceSeriesSchedule` rand pattern diverges from orig (CASCADE; fixes AI car selection parity and ripple effects)
4. **session_04_visual_capture.md** — RenderDoc capture for "cars floating" + HUD transparency (UX; needs RenderDoc setup)
5. **session_05_suspension_fr_asymmetry.md** — car tilted right on flat surface (UX; Frida wheel_y probe)
6. **session_06_invisible_walls.md** — Edinburgh 350 + Newcastle 216 wall-synth holes (BUGS; localized)
7. **session_07_wall_recovery_exit.md** — AI recovery never exits after wall hit (depends on #1)
8. **session_08_deferred_triage.md** — older deferred items (checkpoint timer, police chase audio, view-race-data, Edinburgh 539, AI recovery 360, camera slope) — short triage

## Common setup

Every session runs in:
```
C:\Users\maria\Desktop\Proyectos\TD5RE
```

Build with:
```
cd td5mod/src/td5re && cmd.exe //c ".\\build_standalone.bat"
```
(produces `td5re.exe` at repo root)

Read these first in every session:
- `CLAUDE.md` (project conventions)
- `memory/MEMORY.md` (cross-session index)
- `memory/reference_root_cause_chain_2026-05-22.md` (latest state)

Use `RaceTrace=1` (port) + `--seed-crt true` (orig quickrace) when capturing
paired diffs.

## Parallel-friendly groups

- **Sequential dependency chain:** #1 → #7 (AI recovery exit depends on first identifying why AI gets stuck)
- **Parallel candidates:** #2, #3, #4, #5, #6, #8 can all proceed independently
- **Cascade beneficiaries:** #3 closing also closes slot N RNG-driven divergences (~8 columns in pool13 diff)
