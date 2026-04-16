# Race Validation Pack — 2026-04-09

Goal: convert vague race bugs into deterministic, replayable checks that can be compared against the original EXE and the source port.

Use together with:
- `re/analysis/race-trace-harness.md`
- `tools/compare_race_trace.py`

## Core Trace Fields To Prioritize

Per frame or per tick:
- `frame`
- `sim_tick`
- `stage`
- player/AI slot id
- `world_pos`
- `linear_velocity_x/y/z`
- `angular_velocity_yaw`
- `track_span_normalized`
- `track_span_accumulated`
- `wheel_contact_bitmask`
- `track_contact_flag`
- `steering_command`
- `longitudinal_speed`
- `lateral_speed`
- `current_gear`
- `race_position`
- `checkpoint_index`
- `checkpoint_bitmask`
- camera world position per viewport

## Scenario Set

### 1. Standing Start

Purpose:
- validate countdown, pause-to-live transition, first throttle response, first-gear selection, and first camera update

Expected first divergence to isolate:
- timing units
- countdown state ownership
- initial grip / launch torque

### 2. Straight-Line Acceleration

Purpose:
- validate engine-speed accumulation, gear shifts, longitudinal acceleration, and drag

Watch:
- `engine_speed_accum`
- `longitudinal_speed`
- `current_gear`
- yaw drift on a nominally straight section

### 3. Heavy Braking Into A Corner

Purpose:
- validate brake force, downshift logic, steering rate clamp, and front/rear slip behavior

Watch:
- `steering_command`
- `front_axle_slip_excess`
- `rear_axle_slip_excess`
- `wheel_contact_bitmask`

### 4. Curb / Edge Contact

Purpose:
- validate wheel-surface probes, track contact height, suspension travel, and wall/edge response

Watch:
- per-wheel contact mask changes
- `track_contact_flag`
- pose `y`
- roll/pitch/yaw response

### 5. Wall Scrape

Purpose:
- validate `ApplyTrackSurfaceForceToActor` path, collision angle, damping, and post-contact heading

Watch:
- pre/post impact `vel_x`, `vel_z`
- `angular_velocity_yaw`
- `track_contact_flag`

### 6. AI Overtake

Purpose:
- validate rubber-band throttle, route threshold state, steering bias, and race-order updates

Watch:
- AI throttle bias
- steering command
- route / checkpoint state
- `race_position`

### 7. Traffic Merge / Pass

Purpose:
- validate traffic route planning, simplified traffic dynamics, and incidental collision handling

Watch:
- traffic slot span deltas
- encounter steering override
- relative speed
- collision impulses

### 8. Lap / Checkpoint Transition

Purpose:
- validate the current race-progression owner and expose duplicated lap logic

Watch:
- `checkpoint_index`
- `checkpoint_bitmask`
- `post_finish_metric_base`
- `race_end_fade_state`

## Comparison Strategy

At the start of a bug investigation:

1. Reproduce one short scenario only.
2. Capture trace output from the original EXE and the port.
3. Compare by stage, not just by final frame.
4. Stop at the first divergent stage.
5. Fix only the earliest divergence.
6. Re-run the same scenario plus at least two unaffected scenarios.

## Recommended First Pass

If race bugs are the main problem, do this order:

1. Standing start
2. Straight-line acceleration
3. Lap/checkpoint transition
4. Heavy braking into a corner
5. Wall scrape

That order isolates timing/progression problems before deeper tire-force and collision work.
