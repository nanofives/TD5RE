# Race Trace Harness

The port-side trace harness writes a CSV file to `log/race_trace.csv` when enabled.
Each row is keyed by `(frame, sim_tick, stage, kind, id)` so the original EXE and
the port can be compared at the same simulation checkpoint.

Workflow:
1. Run the same short race scenario in the retail binary and in `td5re.exe`.
2. Capture both traces from `log/race_trace.csv`.
3. Compare them with `tools/compare_race_trace.py`.
4. Fix the first stage that diverges, then repeat.

Use `--fields` to narrow the comparison to a small set of state columns when you
already know which subsystem is unstable. Use `--float-tol` when comparing render
or camera values that differ by minor rounding noise.

If the full trace contains repeated countdown or render-only emissions, use
`--stage`, `--kind`, and `--key-fields` to compare only one canonical stage
slice. If that canonical slice still contains repeated rows per sim tick, add
`--dedupe first` to keep the earliest row for each filtered key.

## Capturing from the original binary

Use the Frida script to hook `TD5_d3d.exe` at the same stage boundaries:

```bash
# Attach to a running TD5_d3d.exe process
frida -p <pid> -l tools/frida_race_trace.js

# Or launch it under Frida
frida -f TD5_d3d.exe -l tools/frida_race_trace.js
```

The script writes `log/race_trace_original.csv` with the same CSV schema.

Edit the top of the script to change:
- `TRACE_SLOT` — which actor slot to trace (-1 = all)
- `TRACE_MAX_FRAMES` — auto-stop limit (default 600)
- `ENABLE_INNER_TICK_HOOKS` — keep this enabled for timing / post_progress validation
- `OUTPUT_PATH` — output file path

### Stage mapping (original vs port)

The original binary bundles physics+AI+track into one dispatcher (`UpdateRaceActors`
at `0x436A70`). The Frida script maps stages as follows:

| Port stage | Original hook point |
|---|---|
| `frame_begin` | `RunRaceFrame` entry (`0x42B580`) |
| `pre_physics` | `UpdateRaceActors` entry (`0x436A70`) |
| `post_physics` | `UpdateRaceActors` return |
| `post_ai` | `ResolveVehicleContacts` return (`0x409150`) |
| `post_track` | `UpdateRaceOrder` return (`0x42F5B0`) |
| `post_progress` | `NormalizeWrapState` last call return (`0x443FB0`) |
| `frame_end` | `EndRaceScene` return (`0x40AE00`) |

### Limitations

- `pause_menu` and `countdown` stages are not yet instrumented in the Frida script
- Actor race metrics (checkpoint, timer, display_position, etc.) are zeroed because
  the original metrics table address is not yet confirmed
- `view_target` in actor rows is always -1 (g_actorSlotForView address unconfirmed)
- captures made with `ENABLE_INNER_TICK_HOOKS = false` are not valid for timing validation;
  they only provide coarse frame-begin / frame-end sanity checks

These can be filled in as more globals are mapped.

## Comparing traces

```bash
python tools/compare_race_trace.py log/race_trace_original.csv log/race_trace.csv --float-tol 0.001
```

Use `--fields` to narrow the comparison:
```bash
# Compare only position and velocity
python tools/compare_race_trace.py orig.csv port.csv --fields world_x,world_y,world_z,vel_x,vel_y,vel_z
```

Canonical sim-tick comparison:

```bash
# Compare only post_progress actor rows, keyed by sim tick instead of frame
python tools/compare_race_trace.py orig.csv port.csv ^
  --stage post_progress ^
  --kind actor ^
  --key-fields sim_tick,stage,kind,id ^
  --dedupe first ^
  --fields span_norm,span_accum,span_high,metric_checkpoint,metric_mask,finish_time,race_pos
```
