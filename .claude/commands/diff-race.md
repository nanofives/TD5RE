# TD5RE Race Diff Workflow

Capture race traces from the original `TD5_d3d.exe` and the `td5re.exe` source
port for the same scenario, diff them with the canonical comparator, and
report the first divergence so a `/fix` can target it.

**Usage:** `/diff-race [car=N] [track=N] [game_type=N] [laps=N] [frames=N] [fields=a,b,c] [stage=post_physics] [kind=actor] [float-tol=0.001]`

All arguments are optional. Defaults:
- `car=0` (1998 Dodge Viper), `track=0` (Moscow, rain+fog), `game_type=0` (single race), `laps=1`
- `frames=300` — 160 tick countdown (0xA000/0x100 @ 30 Hz = 5.33 s) + ~3 s race window + margin
- Comparator defaults to diffing everything with `float-tol=0.001`

## Workflow

### Step 1: Run the orchestrator

All heavy lifting lives in `tools/diff_race.py`. Call it directly, forwarding
the parsed arguments. Do NOT reimplement the INI patching, spawn, or compare
logic — it already:

- Snapshots `td5re.ini` and `re/tools/quickrace/td5_quickrace.ini`, patches
  them for `skip_frontend=true`, `AutoThrottle=1`, `RaceTrace=1`,
  `RaceTraceSlot=-1`, and the requested scenario. The port's auto-race path
  mirrors the original's Frida quickrace hook exactly (ConfigureGameTypeFlags
  → InitializeRaceSeriesSchedule → InitializeFrontendDisplayModeState) and
  skips the wall-clock srand reseed whenever `RaceTrace=1`.
- Spawns `original/TD5_d3d.exe` via `re/tools/quickrace/td5_quickrace.py
  --trace --trace-auto-exit` (both the quickrace hook and
  `tools/frida_race_trace.js` load into the same Frida session).
- Spawns `td5re.exe` from the repo root (**never** from `original/`), waits
  for `log/race_trace.csv` to stop growing, then kills it.
- Runs `tools/compare_race_trace.py log/race_trace_original.csv log/race_trace.csv`
  with the forwarded `--fields` / `--stage` / `--kind` / `--float-tol` filters.
- Restores both INI snapshots no matter what.

Parse `$ARGUMENTS` into `--car`, `--track`, `--game-type`, `--laps`,
`--frames`, `--fields`, `--stage`, `--kind`, `--float-tol` and invoke:

```bash
python tools/diff_race.py --car N --track N [--frames N] [--fields a,b,c] [--stage S] [--kind K] [--float-tol F]
```

### Step 2: Interpret the output

The comparator prints two kinds of lines:

**`Mismatch: key=(sim_tick, stage, kind, id) field=X ...=A ...=B`** — real
divergences. Report the **earliest** `sim_tick` with a mismatch. For each,
report the field, both values, and the delta. Values in `world_*`/`vel_*`
columns are 24.8 fixed point; divide by 256 to get world units.

**`Missing from race_trace_*.csv: key=(sim_tick, ...)`** — mostly noise,
report but don't treat as bugs. Expected sources:

- `sim_tick=0` rows missing from one side: the original emits `post_progress`
  for frames where the port is still in countdown (port only increments
  `simulation_tick_counter` after countdown ends; Frida does not have that
  gate).
- `sim_tick >= ~385` (or wherever the port's `raceFrameCount` caps first)
  missing from the port: the original's Frida hook runs the sim-loop more
  aggressively per render frame (we clamp `g_simTickBudget >= 1.0`), so the
  original reaches higher sim_ticks per frame-count cap. Ignore unless both
  sides reached the same sim_tick and one is still missing a row.

**Mapping a divergence back to source-port modules** — look at the `stage`:
- `frame_begin` / `frame_end` → `td5_game.c` (state FSM boundary)
- `pre_physics` → state BEFORE the physics step — bug is upstream in input poll / AI command
- `post_physics` → `td5_physics.c` (the physics step itself just ran)
- `post_ai` → `td5_ai.c`
- `post_track` → `td5_track.c`
- `post_camera` → `td5_camera.c`
- `post_progress` → `td5_game.c` (wrap normalization, lap accounting, race-metric sync)

Report to the user: "first mismatch at sim_tick=N, field=X differs by Δ;
module to investigate: `td5_Y.c`." Then let them decide whether to run
`/fix`.

### Step 3: Do NOT run /fix yourself

`/diff-race` is read-only. It sets up the comparison, reports the first
mismatch, and stops. The user decides whether to invoke `/fix` next.

## Rules

- Never edit `td5re.ini` or `td5_quickrace.ini` by hand — `diff_race.py`
  snapshot/restores them. If you hit a pre-existing leftover patched INI
  (e.g. from a crashed earlier run), restore it from git before rerunning.
- Never copy `td5re.exe` into `original/`. It lives at the repo root and
  reads game data out of `original/` via relative paths.
- Run `diff_race.py` through bash from the repo root (`cd` is handled
  inside the script via `REPO = HERE.parent`).
- If `diff_race.py` fails with `original trace missing` or `port trace
  missing`, the run died before the trace flushed. Check:
  - Is the windowed-mode patch applied to the original? (`re/patches/patch_windowed_*.py`)
  - Is `original/DDraw.dll` renamed to `DDraw.dll.td5re_wrapper`?
  - Does `log/engine.log` show `Shared INI: ... skip_frontend='true' -> 1`
    and `AutoRace=1`? If not, the shared INI wasn't picked up.
- If the comparator exits 0, report "no divergence in the captured window"
  — don't fabricate a finding.

## Extending

- Add `slot=N` to the orchestrator if you need to diff a specific AI actor
  only. Defaults compare all 6 slots (`RaceTraceSlot=-1` on both sides).
- Add `--trace-stages` if the Frida script gains support for more than the
  currently mapped stages (see `re/analysis/race-trace-harness.md`).
  Candidate hook points at `0x42B991` (pre_physics), `0x42B9A4` (physics+AI+track),
  `0x42BA13` (post_camera), `0x42BAA1` (post_progress) have all been verified
  safe in Ghidra [CONFIRMED] — wire them in when you need more stage coverage.

## Known limitations (as of 2026-04-10)

- The Frida trace script's per-render-frame `post_progress` emission is
  coarser than the port's per-sim-tick emission. `--dedupe first` hides
  the redundancy but the comparison is only meaningful at `post_progress`
  granularity right now.
- `g_simTickBudget` is clamped to `>= 1.0` by the Frida trace hook each
  frame to prevent the original's sim-tick loop from stalling under hook
  overhead. If you see countdown taking slightly longer on the original
  than expected, that's the clamp at work — it's not a bug.
- `view_target` in actor rows is always `-1` on the Frida side
  (`g_actorSlotForView` address is not yet confirmed). Don't include
  `view_target` in `--fields`.
- Cars `17` and `24` have `*(null)*` car-name entries — pick other cars
  for scenarios.
