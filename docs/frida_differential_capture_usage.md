# Frida Differential Capture -- Usage

User-driven Frida tool that captures runtime state from BOTH
`original/TD5_d3d.exe` and `td5re.exe` on the SAME scenario, then emits a
merged CSV showing where the port diverges from the original.

Closes:
* #7 chassis launches  -- `--target chassis-launch` (Edinburgh AI Viper)
* #8 cascade / yaw-spin -- `--target cascade` (Moscow AI Viper)

## Prerequisites

* Windows desktop (the games pause when window loses focus -- this script
  must run interactively, not headless).
* Python 3.10+ with the `frida` package: `pip install frida`.
* `original/TD5_d3d.exe` and `td5re.exe` must both exist (build the port
  per `CLAUDE.md` if needed).
* `re/tools/quickrace/td5_quickrace.py` must be present (it ships with the
  repo; used to spawn the original with the right race config).
* No frida-server install required -- we use frida's in-process attach API.

## Invocation

From the project root, in a PowerShell terminal with focus on the game
windows (NOT VS Code's integrated terminal -- the game must be able to
take focus):

```
# Chassis-launch probe (Edinburgh AI Viper, 30 s capture per binary)
python scripts/frida_differential_capture.py --target chassis-launch

# Cascade probe (Moscow AI Viper, 30 s capture per binary)
python scripts/frida_differential_capture.py --target cascade

# Custom duration
python scripts/frida_differential_capture.py --target chassis-launch --duration 60

# Dry-run -- print planned spawn commands without launching anything
python scripts/frida_differential_capture.py --target chassis-launch --dry-run

# Capture only one binary (if you already have the other from a prior run)
python scripts/frida_differential_capture.py --target chassis-launch --skip-orig
python scripts/frida_differential_capture.py --target chassis-launch --skip-port
```

## What the script does

1. Reads the target configuration (track / car / PlayerIsAI / game_type).
2. **Orig capture phase:**
   * Builds a per-run Frida JS by patching `tools/frida_<target>_probe.js`
     with `BINARY_LABEL="orig"` and the absolute output path.
   * Shells out to `re/tools/quickrace/td5_quickrace.py` which spawns
     `TD5_d3d.exe`, focuses its window, attaches Frida, and injects both
     the quickrace race-config hook and our probe script.
   * Waits `--duration + 60s` for the capture to complete, then kills
     any straggling `TD5_d3d.exe`.
3. **Port capture phase:**
   * Patches `td5re.ini` for the scenario (`AutoRace=1`,
     `DefaultTrack`, `DefaultCar`, `PlayerIsAI`).
   * Spawns `td5re.exe`, focuses its window, attaches Frida via
     `device.attach(pid)` with the same poll-after-spawn retry pattern
     used by `scripts/capture_snapshot_pool.py`.
   * Loads the probe script with `BINARY_LABEL="port"`.
   * Captures for `--duration` seconds, then kills `td5re.exe` and
     restores the original `td5re.ini`.
4. **Merge phase:** Both per-binary CSVs are loaded; rows sorted by
   `(slot, sub_tick)`, with orig row before port row at each key;
   merged CSV written to `log/diff_<target>_<timestamp>.csv`.

## Output files

```
log/diff_<target>_orig_<timestamp>.csv   -- orig-only raw rows
log/diff_<target>_port_<timestamp>.csv   -- port-only raw rows
log/diff_<target>_<timestamp>.csv        -- MERGED, sort by (slot, sub_tick)
```

## CSV schemas

### `--target chassis-launch`

```
binary,slot,sub_tick,world_x,world_y,world_z,vx,vy,vz,ground_FL,ground_FR,ground_RL,ground_RR,wcb,scf
```

* `binary` -- `orig` or `port`
* `slot`   -- racer 0..5
* `sub_tick` -- monotonic per-binary call count for this hook
* `world_x/y/z` -- actor world position (fp8, 24.8 fixed-point)
* `vx/vy/vz` -- linear velocity (fp8)
* `ground_FL/FR/RL/RR` -- per-wheel `wheel_contact_pos[i].y` after the call (fp8)
* `wcb` -- `wheel_contact_bitmask` snapshot (u8)
* `scf` -- `surface_contact_flags` (u8)

### `--target cascade`

```
binary,self_slot,sub_tick,route_ptr,bias,span_norm,fwd_track,active_lo,active_hi,right_a,right_b,lat_dir,peer_returned
```

* `binary` -- `orig` or `port`
* `self_slot` -- racer 0..5
* `sub_tick` -- monotonic per-binary call count for this hook
* `route_ptr` -- `RS[0x00]` route_table_ptr
* `bias` -- `RS_TRACK_OFFSET_BIAS` (`RS[0x24]`)
* `span_norm` -- `actor->track_span_normalized` (`actor[0x82]`)
* `fwd_track` -- `FWD_TRACK_COMP` (`RS[0x18]`)
* `active_lo/hi` -- `ACTIVE_LOWER_BOUND/UPPER_BOUND` (`RS[0x16/17]`)
* `right_a/b` -- `RIGHT_BOUNDARY_A/B` (`RS[0x15/14]`)
* `lat_dir` -- `g_lateral_avoidance_direction` global
* `peer_returned` -- the return value (-1 = no-peer in port,
  self_slot = no-peer in orig per `reference_arch_find_offset_peer_return_minus_one.md`)

## How to interpret the output

1. Open the merged CSV in your editor or spreadsheet.
2. Filter by `slot=0` (or the suspect slot from the bug report).
3. Walk down `sub_tick` and compare orig vs port columns at each pair
   of rows. The FIRST sub-tick at which any field differs is the
   divergence point.
4. The diverging field tells you where to look:
   * Different `bias` at same `sub_tick` -> upstream cascade write
     diverged before `find_offset_peer` was entered (look at the
     `ClassifyTrackOffsetClamp` pre-loop in `UpdateRaceActors @
     0x00436A70`).
   * Different `ground_FL` at same world_pos -> per-wheel transform
     diverges (look at `0x00403720` body or the transform helpers it
     calls).
   * Same inputs, different `peer_returned` -> the function body itself
     branches differently (very unlikely after the
     `reference_arch_find_offset_peer_return_minus_one.md` audit).

## Sharing results

After running, attach the merged CSV plus its two per-binary inputs to
the relevant TODO file:

* `chassis-launch` -> `memory/todo_edinburgh_chassis_launch_proximate_cause.md`
* `cascade`        -> `memory/reference_steering_cascade_root_cause_find_offset_peer.md`

A follow-up agent can then point its bisection at the first divergent
sub_tick instead of guessing where to start.

## Troubleshooting

* **"failed to detect spawned td5re.exe pid"** -- the port didn't appear
  within the detection window (slow boot, or it crashed on launch). The
  script snapshots pre-existing td5re.exe pids and only claims the NEW one,
  so a parallel session's instance won't trip it. If you need to clear a
  stray instance from a PREVIOUS run of THIS script, close only that pid
  (`taskkill /F /PID <pid>`) -- never `taskkill /F /IM td5re.exe`, which
  would kill other sessions' games mid-capture.
* **`ERROR: cannot resolve td5_ai_find_offset_peer in td5re.exe`** --
  the port build did not export internal symbols. Rebuild with
  `-Wl,--export-all-symbols` or add a `.def` file listing the two hook
  targets. Without exports the port-side CSV will be empty (orig-side
  still works -- you can capture orig, fix the build, then re-run with
  `--skip-orig`).
* **Empty orig CSV** -- check that `re/tools/quickrace/td5_quickrace.py`
  exists; if quickrace itself crashed look at its stdout (echoed under
  `[orig:qr]` prefix in this script's output).
* **CSV stops growing partway through** -- the game lost focus (alt-tab,
  notification popup). Run with `--duration` reduced and keep the game
  window on top during the entire capture.

## See also

* `memory/reference_frida_differential_capture_2026-05-18.md` -- this tool's
  design notes and hook addresses.
* `memory/reference_steering_cascade_root_cause_find_offset_peer.md` --
  cascade root cause analysis that motivated `--target cascade`.
* `memory/todo_edinburgh_chassis_launch_proximate_cause.md` -- chassis-launch
  TODO that motivated `--target chassis-launch`.
* `tools/frida_wheel_contact_probe.js` -- template the chassis probe was
  derived from.
* `tools/frida_ai_probe.js` -- template the cascade probe was derived from.
