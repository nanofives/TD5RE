# TD5RE Race Diff Workflow

Capture race traces from the original `TD5_d3d.exe` and the `td5re.exe` source
port for the same scenario, diff them with the canonical comparator, and
report the first divergence so a `/fix` can target it.

**Usage:**
- Live capture + diff (single track, interactive):
  `/diff-race [profile=NAME[,NAME...]] [car=N] [track=N] [game_type=N] [laps=N] [frames=N] [fields=a,b,c] [stage=post_physics] [kind=actor] [float-tol=0.001] [hooks=PATH.yaml]`
- Diff an existing `/fix` CSV bundle (no capture — reuses what /fix already produced):
  `/diff-race bundle=<SESSION_TAG> [profile=NAME] [track=N] [fields=a,b,c] [stage=S] [kind=K] [float-tol=F]`
- List the profile catalog (no race run):
  `/diff-race list-profiles`

All arguments are optional. Defaults:
- `profile=core` — minimal baseline pulse: position + race progress per actor at `post_progress`. Excludes harness noise (frame counter, slot_state asymmetry, view_target unset on Frida, sim_accum drift). Override per session with `profile=physics` (or `physics,track`, etc.).
- `car=0` (1998 Dodge Viper), `track=0` (Moscow, rain+fog), `game_type=0` (single race), `laps=1`
- `frames=300` — 160 tick countdown (0xA000/0x100 @ 30 Hz = 5.33 s) + ~3 s race window + margin
- `float-tol=0.001` (or whatever the chosen profile pins)
- `bundle=<tag>` — no live capture; reads CSVs from `tools/frida_csv/<tag>/`. Defaults to diffing ALL three tracks in the bundle; narrow with `track=N`.

## Profile catalog

Profiles are curated `(fields, stage, kind)` slices that target one subsystem. Source of truth: `tools/diff_profiles.py`. Run `python tools/diff_profiles.py list` for the live catalog. Multiple profiles compose: `profile=physics,track` unions their fields and joins their stages/kinds.

| Profile | Lens | Kind | Stage |
|---|---|---|---|
| `core` (default) | "Did anything fundamental drift?" — pos + race progress | actor | post_progress |
| `physics` | Per-tick physics: pos/vel/angles/slip/contact | actor | post_physics |
| `physics_full` | Physics + driver commands + display angles | actor | post_physics |
| `vehicle` | Drivetrain + ground contact (gear, mode, mask, speeds) | actor | post_physics |
| `steering` | Steer command vs body/display yaw + lat speed | actor | post_ai |
| `ai` | AI command output (steer, engine, span, race_pos) | actor | post_ai |
| `track` | Span (raw/norm/accum/high) + contact + mask | actor | post_track |
| `progress` | Race position, lap metrics, finish accounting | actor | post_progress |
| `hud` | What the speedo/minimap/timer reads | actor | post_progress |
| `spawn` | Tick-0 grid snapshot | actor | frame_begin |
| `slot_state` | Slot bookkeeping (state, comp1/2, view_target) | actor | post_progress |
| `view` | Camera world + camera local pos | view | frame_end |
| `display` | Render-time display angles (smoothed) | actor | frame_end |
| `frontend` | Game-state FSM: state, paused, fade, countdown | frame | frame_begin |
| `viewport` | Split-screen layout: count, mode | frame | frame_begin |

**To extend the library:** add an entry to `PROFILES` in `tools/diff_profiles.py`. Pick fields lean — the comparator's first-mismatch break (line 192 of `compare_race_trace.py`) hides every field after the first divergent one, so noisy columns mask real signal.

**Explicit overrides still work:** `--fields`/`--stage`/`--kind`/`--float-tol` (the underlying CLI flags) and the corresponding `fields=`/`stage=`/`kind=`/`float-tol=` skill arguments override the profile-resolved values for that one run. Use this when probing a single column inside a broader profile (e.g. `profile=physics fields=world_x` to isolate one signal at the physics-stage slice).

## Workflow

### Step 1: Run the orchestrator

All heavy lifting lives in `tools/diff_race.py`. Call it directly, forwarding
the parsed arguments. Do NOT reimplement the INI patching, spawn, or compare
logic — it already:

- Forwards scenario as first-class CLI flags to `td5_quickrace.py`
  (`--track`, `--car`, `--game-type`, `--laps`, `--start-span-offset`,
  `--player-is-ai`). The Frida hook receives them via
  `script.post({type:'cfg', cfg})`. **`td5_quickrace.ini` is NEVER mutated**
  so two parallel `/diff-race` (or `/fix`) sessions can run different
  scenarios concurrently without colliding on shared launcher state.
- Launches `original/TD5_d3d.exe` via `re/tools/quickrace/td5_quickrace.py
  --trace --trace-auto-exit --csv-out <per-session-path>` (both the
  quickrace hook and `tools/frida_race_trace.js` load into the same Frida
  session; the trace script's `OUTPUT_PATH` constant is rewritten on
  injection so each session writes to its own CSV).
  **Win11-safe launch path:** `td5_quickrace.py` no longer uses
  `frida.spawn()` — that path triggers an M2DX.dll NULL-deref at +0x144B
  during DirectSound init on this box (see memory:
  `reference_m2dx_crash_2026-04-29.md`). Default behavior is now
  `cmd /c start "" TD5_d3d.exe` followed by a 10 ms-interval poll on
  `frida.get_local_device().enumerate_processes()` until the PID appears,
  then `frida.attach(pid)`. This catches the process before
  `RunMainGameLoop` reaches `GAMESTATE_INTRO`, so the hook's no-op
  replacement of `ShowLegalScreens` / `PlayIntroMovie` actually fires
  (otherwise the legals/intro plays before Frida attaches). Pass
  `--spawn` to opt back into the historical `frida.spawn()` path.
- Spawns `td5re.exe` from the repo root (**never** from `original/`) with
  `--Key=N` CLI overrides carrying the full scenario + trace knobs
  (`--DefaultCar`, `--DefaultTrack`, `--DefaultGameType`, `--Laps`,
  `--StartSpanOffset`, `--AutoRace=1`, `--SkipIntro=1`, `--PlayerIsAI=1`,
  `--RaceTrace=1`, `--RaceTraceSlot=-1`, `--RaceTraceMaxFrames=N`,
  `--RaceTraceMaxSimTicks=N`, `--AutoThrottle=1`, `--TraceFastForward=4`).
  Both binaries run with PlayerIsAI ON by default: the port routes slot 0
  through `td5_physics_update_ai`, and the Frida hook windows
  `gRaceSlotStateTable.slot[0].state=0` inside `UpdateRaceActors` so the
  original's slot 0 runs AI too. This keeps the diff focused on physics/AI
  parity instead of input-poll divergence. Pass `--PlayerIsAI=0` via the
  orchestrator only when specifically probing the human-input path.
  The port's auto-race path mirrors the original's Frida quickrace hook
  exactly (ConfigureGameTypeFlags → InitializeRaceSeriesSchedule →
  InitializeFrontendDisplayModeState) and skips the wall-clock srand reseed
  whenever `RaceTrace=1`. Waits for `log/race_trace.csv` to stop growing,
  then kills it. `td5re.ini` is never mutated.
- Runs `tools/compare_race_trace.py log/race_trace_original.csv log/race_trace.csv`
  with the forwarded `--fields` / `--stage` / `--kind` / `--float-tol` filters.

Parse `$ARGUMENTS` into `--profile`, `--car`, `--track`, `--game-type`,
`--laps`, `--frames`, `--fields`, `--stage`, `--kind`, `--float-tol`,
`--hooks` and invoke:

```bash
python tools/diff_race.py [--profile NAME[,NAME]] --car N --track N [--frames N] [--fields a,b,c] [--stage S] [--kind K] [--float-tol F] [--hooks PATH]
```

If the user passed `list-profiles` (with or without other args), short-circuit:

```bash
python tools/diff_race.py --list-profiles
```

Profile resolution order inside the orchestrator:
1. The selected profile(s) provide default `fields`/`stage`/`kind` (and optionally `float-tol`).
2. Any explicit `--fields`/`--stage`/`--kind`/`--float-tol` flag overrides the profile-resolved value for that one knob.
3. The orchestrator prints the resolved slice (`profile=… stage=… kind=… fields=…`) so the user can see what was actually diffed.

### Step 1b: Bundle mode (`bundle=<SESSION_TAG>`)

When `$ARGUMENTS` contains `bundle=<tag>`, skip the live capture entirely and diff the CSV pairs that `/fix` already produced under `tools/frida_csv/<tag>/`. This is the default way to evaluate a `/fix` candidate across the Moscow + Newcastle + random trio without re-spawning either binary.

```bash
BUNDLE_DIR="C:/Users/maria/Desktop/Proyectos/TD5RE/tools/frida_csv/${BUNDLE_TAG}"
if [ ! -d "${BUNDLE_DIR}" ]; then
    echo "Bundle not found: ${BUNDLE_DIR}"
    echo "Available bundles:"
    ls -dt tools/frida_csv/fix-*/ 2>/dev/null | head -10
    exit 1
fi

# Read meta.json for the trio the bundle captured.
python - <<PY
import json, pathlib
meta = json.loads(pathlib.Path("${BUNDLE_DIR}/meta.json").read_text())
for t in meta["trio"]:
    print(f"{t['index']}\t{t['name']}\t{t['kind']}")
PY
```

For each track in the bundle (filtered by `track=N` if the user passed one):

```bash
ORIG="${BUNDLE_DIR}/original_track${TRACK_N}_${TRACK_NAME}.csv"
PORT="${BUNDLE_DIR}/fix_track${TRACK_N}_${TRACK_NAME}.csv"

# Sanity: both sides must exist AND have > 50 rows.
for f in "${ORIG}" "${PORT}"; do
    [ -f "${f}" ] || { echo "Missing: ${f}"; continue 2; }
    rows=$(wc -l < "${f}")
    [ "${rows}" -gt 50 ] || { echo "Too short (${rows} rows): ${f}"; continue 2; }
done

# If a profile was passed, resolve it via diff_profiles before invoking the comparator.
# Otherwise, fall back to whatever explicit fields/stage/kind/float-tol were passed.
if [ -n "${PROFILE:-}" ]; then
    eval "$(python - <<PY
import shlex, sys
sys.path.insert(0, "tools")
import diff_profiles
r = diff_profiles.resolve(diff_profiles.parse_profile_arg("${PROFILE}"))
print(f"PROF_FIELDS={shlex.quote(r['fields'])}")
print(f"PROF_STAGE={shlex.quote(r['stage'])}")
print(f"PROF_KIND={shlex.quote(r['kind'])}")
print(f"PROF_FLOAT_TOL={r['float_tol'] if r['float_tol'] is not None else ''}")
PY
)"
    FIELDS="${FIELDS:-$PROF_FIELDS}"
    STAGE="${STAGE:-$PROF_STAGE}"
    KIND="${KIND:-$PROF_KIND}"
    FLOAT_TOL="${FLOAT_TOL:-$PROF_FLOAT_TOL}"
fi

python tools/compare_race_trace.py "${ORIG}" "${PORT}" \
    ${FIELDS:+--fields ${FIELDS}} \
    ${STAGE:+--stage ${STAGE}} \
    ${KIND:+--kind ${KIND}} \
    ${FLOAT_TOL:+--float-tol ${FLOAT_TOL}}
```

**Report format in bundle mode:** produce a compact per-track summary, e.g.:

```
Bundle fix-1776736681-282344 (branch fix-1776736681-282344, commit de22a8a)
  Moscow    (track=0)  — earliest mismatch: sim_tick=12 field=world_pos_y Δ=234  → td5_physics.c
  Newcastle (track=5)  — no divergence in 312 rows
  Munich    (track=15) — earliest mismatch: sim_tick=7  field=vel_x        Δ=0.18 → td5_physics.c
```

Do NOT re-run the comparator against a track that's already clean; say "no divergence" once and move on.

### Custom Ghidra-function hooks (`hooks=PATH.yaml`)

When the user passes `hooks=re/trace-hooks/<spec>.yaml`, the Frida side
attaches `Interceptor.attach` hooks to each listed function and emits one
row per invocation to `log/calls_trace_original.csv`. The port emits
matching rows to `log/calls_trace.csv` whenever a `TD5_TRACE_CALL_ENTER` /
`TD5_TRACE_CALL_RET` macro fires in the C source. The orchestrator runs a
second comparator pass on those two CSVs keyed by
`(sim_tick, fn_name, call_idx)`.

**User workflow:**

1. Write a YAML spec under `re/trace-hooks/` (see
   `re/trace-hooks/README.md` + `example_suspension.yaml`). Each hook
   entry needs `name`, `original_rva`, `args` (count 0..8), optional
   `capture_return`.
2. Insert `TD5_TRACE_CALL_ENTER("<name>", arg0, arg1, ...)` at the
   entry of the port's equivalent C function — matching the spec's
   `name`. Optionally also `TD5_TRACE_CALL_RET("<name>", retval, arg0,
   arg1, ...)` at every return path. Rebuild the port.
3. Run `/diff-race hooks=re/trace-hooks/<spec>.yaml [other flags]`.
4. Inspect comparator output for per-call arg/ret divergences.

Do NOT add the macros yourself on behalf of the user — they belong in
whatever function the user is actively investigating. Your job when the
skill is invoked with `hooks=...` is only to pass it through to
`diff_race.py`.

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

- Neither `td5re.ini` nor `re/tools/quickrace/td5_quickrace.ini` is
  mutated. Both binaries receive their scenario via CLI flags (port:
  `--Key=N` overrides; Frida side: first-class flags forwarded to the
  hook via `script.post`). You can safely edit either INI in parallel
  with a diff-race run, and multiple `/diff-race` (or `/fix`) sessions
  can run concurrently with different scenarios. Pass `--session-tag
  <tag>` to also separate the four CSV outputs
  (`log/race_trace_<tag>.csv` etc.); the port honors
  `TD5RE_RACE_TRACE_PATH` / `TD5RE_CALLS_TRACE_PATH` env vars set by
  the orchestrator.
- Never copy `td5re.exe` into `original/`. It lives at the repo root and
  reads game data out of `original/` via relative paths.
- Run `diff_race.py` through bash from the repo root (`cd` is handled
  inside the script via `REPO = HERE.parent`).
- In bundle mode, do NOT mutate `tools/frida_csv/<tag>/` — it's the
  read-only artifact of a `/fix` session. If a bundle is missing a track
  or the CSVs look truncated, tell the user to re-run the `/fix` sweep;
  do NOT attempt to regenerate the bundle from `/diff-race`.
- If `diff_race.py` fails with `original trace missing` or `port trace
  missing`, the run died before the trace flushed. Check:
  - Is the windowed-mode patch applied to the original? (`re/patches/patch_windowed_*.py`)
  - Is `original/DDraw.dll` renamed to `DDraw.dll.td5re_wrapper`?
  - Does `log/engine.log` show `=== N CLI override(s) applied ===` followed
    by `AutoRace=1` and `DefaultTrack=<expected>` in the `[Game]` dump
    line? Missing CLI override log lines mean `lpCmdLine` didn't reach
    `td5_apply_cli_overrides()` — inspect the PowerShell `-ArgumentList`
    expansion in `run_port`. Needs `[Logging] Enabled=1` in the user's
    td5re.ini for the log lines to be visible.
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
