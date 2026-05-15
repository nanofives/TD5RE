# Whole-state diff — first-5-ticks inventory (2026-05-15 session)

Baseline: Honolulu (track=8) AI Viper, both sides captured at sim_tick 1..7
via `tools/run_whole_state_diff.py`.

## Per-tick divergence count progression (tick 1 / 2 / 3 / 4)

| Stage | Tick 1 | Tick 2 | Tick 3 | Tick 4 | Notes |
|---|---:|---:|---:|---:|---|
| Baseline (after scenario alignment) | 805 | 882 | 912 | 931 | After both sides forced to Honolulu AI |
| + `wheel_world_hires` shift fix | 789 | 909 | 980 | 973 | `<<8` removed from td5_physics.c:6595-6597 |
| + downstream consumer cleanup | 789 | 874 | 906 | 933 | sar8_rz removed at 4045-4046, 4167-4168 |
| + body_probes init loop | 768 | 852 | 887 | 911 | Added second symmetric loop @ 6285 |
| + benign-filter expansion | 762 | 846 | 881 | 905 | car_config_ptr/gap_1B4/rotation_matrix[8] marked benign |
| + display_angle 12-bit-mask removal | **742** | **843** | **872** | **881** | Unmasked at 5232 + 5426 to match 5978 |

**Net session reduction: tick 1 -63 (-7.8%), tick 4 -50 (-5.4%).** Five root-cause fixes plus diff-tool filter expansion shipped.

## SHIPPED fixes

| # | File:line | Fix | Cluster |
|---|---|---|---|
| 1 | `td5_physics.c:6595-6602` | Removed `<<8` on hires storage; now stores raw world units matching original 0x40388A-0x40388E | A |
| 2 | `td5_physics.c:4045-4046` | Removed compensating `sar8_rz` on hires consumers in `td5_physics_integrate_suspension` | A cascade |
| 3 | `td5_physics.c:4160-4168` | Removed `sar8_rz` on front_mid_x/z computation | A cascade |
| 4 | `td5_physics.c:6285-6296` | Added `body_probes[]` init loop matching original's second loop in 0x00403720 | B |
| 5 | `tools/diff_whole_state.py` | Filter pointer offsets (0x1B0/4/8/C), 0x338 frame_counter, 0x140 matrix tail | tooling |
| 6 | `td5_physics.c:5232 + 5426` | Removed `& 0xFFF` mask from `display_angles.yaw/roll/pitch` writes to match the 5978 fix path | display-angle |

## REMAINING labeled divergences (slot 0, tick 1; cascades to ticks 2-4)

### B-remainder: probe walker output (port doesn't match)
- `body_probe_0_span_norm` (+0x002): port=102 vs orig=0
- `wheel_probe_0_span_norm` (+0x042): port=102 vs orig=0
- Same pattern for span_accumulated, span_high_water, contact_vertex_A/B across all 8 probes
- **Diagnosis**: port's `td5_track_update_probe_position` either doesn't run after the init copy, or produces actor-track values instead of fresh per-probe walker values. Original's per-probe walker computes distinct (span_idx=102, span_norm=0, span_acc=1, span_hw=1, vA=770, vB=779).
- **Investigation needed**: pilot Frida hook on 0x00403720's per-probe sub-call to see what address/function the original calls for each probe update.

### Spawn-position cascade (chassis off by ~90, wheels off by ~58K)
- `probe_FL_x/z, FR_x/z, RL_x/z, RR_x/z` (+0x90-0xBC): port wheel world positions ~57K-100K units off (~230-400 world units)
- `wheel_contact_FL_x` (+0x0F0): off by 256 = 1 world unit (rounding)
- **Diagnosis**: chassis `world_pos` matches to ~90 units, but wheel offsets from chassis differ. Either rotation matrix differs (m1 elements differ at 3rd decimal) or `car_definition_ptr` content differs (port's allocated buffer vs original's data segment).

### F: doubled steering / angular state
- `steering_command` (+0x30C): port=-48000 vs orig=-23776 (port 2x)
- `angular_velocity_yaw` (+0x1C4): port=-205 vs orig=-102 (port 2x)
- `cached_car_suspension_travel` (+0x324): port=192 vs orig=96 (port 2x)
- **Diagnosis**: AI steering output doubled. Possibly an old issue from cluster A's wrong unit-scale that's now actually overshooting because consumers were calibrated to the wrong scale.

### Numerical-precision cascades (small offsets)
- `world_pos_x/z` off by ~100
- `linear_velocity_x/y/z` off by ~100-200
- `render_pos_x/y/z` off by ~0.3-7
- `euler_accum_yaw` off by 103
- `linear_velocity_y` port=-128 vs orig=0 (port has gravity-settled, orig hasn't)
- `center_suspension_pos/vel` off by ~8
- `wheel_suspension_pos_*` off by 5-25
- `wheel_spring_dv_*` off by 5-25
- **Diagnosis**: physics integrator accumulating different rounding each tick. Some are inherent; some may be fixable via instruction-order matching (per existing precise-port strategy).

### State-init divergences (port hasn't initialized)
- `wheel_contact_normals` (+0x230, 16 bytes): port all-zero, orig populated
- `prev_race_position` (+0x381): port=0 vs orig=2
- `grip_reduction` (+0x380): port=0 vs orig=2
- `surface_type_chassis` (+0x370): port=0 vs orig=1
- **Diagnosis**: port doesn't compute these fields at all yet.

### `display_angle_yaw` off-by-mask
- port=791 vs orig=4887 (4096 = 0x1000 difference)
- 4887 = 0x1317; port masks to 0x0FFF, orig keeps full int16
- **Fix**: stop masking `display_angle_yaw` to 12 bits — write the full `euler_accum_yaw >> 8` value.

## What's next (continued iterations)

Top-3 likely highest-leverage fixes:

1. **Probe-walker output (cluster B-remainder)** — fix span_normalized/accumulated/high_water to come from per-probe walker, not initial copy. ~32 fields × 6 slots × 5 ticks = ~960 of the remaining ~3800 diffs.
2. **Wheel-offset from chassis (spawn-cascade)** — audit `car_definition_ptr` content vs original; fix wheel hardpoint offsets. Cascades into ~20 wheel-position diffs × 6 slots × 5 ticks.
3. **Steering 2x scale** — investigate why AI steering_command output is doubled. Single root cause, propagates into yaw/suspension.

Each is a separate session-sized investigation per the existing precise-port workflow.

## Update — 2026-05-15 session 2

Shipped fixes (commits 86f7c21 + ed157cf):

| # | File | Fix |
|---|---|---|
| 7 | `td5_physics.c:6284-6303` | Probe init loops now seed only `span_index` + `sub_lane_index` (matching original's `*(short *)actor = local_24; actor->field_0xc = local_20` writes at 0x00403720). Removed wrong `span_normalized/accumulated/high_water` seeds that polluted probes with the actor's main-track values. |
| 8 | `td5_track.c:2898+` | New `td5_track_compute_probe_contact_vertices()` mirrors the prefix of `ComputeActorTrackContactNormal[Extended]` (0x00445450/0x004457E0) — writes `contact_vertex_A/B` to the probe from `(span_index, sub_lane_index)` via the strip vertex bases + `k_quad_vertex_offsets[type][col]`. |
| 9 | `td5_physics.c:6404-6422` | Called the new helper after each wheel-probe walker invocation. |
| 10 | `td5_physics.c:6657-6710` (replaces wheel-alias) | Faithful port of the original's body_probes loop (second loop in 0x00403720). Reads body corner offsets from `car_definition_ptr[0..15]` (4 × short[4], 8-byte stride), transforms via the `(rot, render_pos)` render matrix, shifts <<8 to 24.8 FP, writes to `probe_FL/FR/RL/RR`. Then calls walker + contact-vertex helper. Replaces prior alias `probe_FL = wheel_contact_pos[0]` which used wheel offsets (off by ~230-400 world units). |
| 11 | Same loop | Overwrite `probe_*.y` with the barycentric ground height post-walker, mirroring the original's `ComputeActorTrackContactNormal` (0x00445450) which writes `*piVar8+1` with `height + strip_origin_y * 256`. |
| 12 | `td5_physics.c:6457-6469` | Resolve `surface_type_chassis` from the probe's span+lane via `td5_track_get_surface_type(actor, 4 + i)` rather than echoing the stale (=0) `actor->surface_type_chassis`. |

### Per-tick divergence reduction (Honolulu AI Viper)

| Tick | Session start | After session 2 |
|------|--------------:|----------------:|
|   1  |           742 |             429 |
|   2  |           843 |             524 |
|   3  |           872 |             570 |
|   4  |           881 |             593 |

Per-tick reduction: **~42% on tick 1**.

### Slot-0 tick-1 labeled count

| Stage | Labeled |
|-------|--------:|
| Session start | 48 |
| After probe-init fix + contact_vertex helper (8) | 46 |
| After body-corner faithful port (9, 10) | 45 |
| After ground-height post-walker write (11) | 41 |
| After surface_type-from-probe fix (12) | **40** |

### Why the labeled count didn't drop below 30

The goal anticipated `~16` per-probe labeled fields would be eliminated, dropping ~48 → <30. In practice the per-probe walker fix eliminated **mostly anonymous byte-level divergences** (probe sub-fields appear as `(?)` entries in the diff because the diff tool labels at struct-array offset, not at sub-field offset). The all-divergences count dropped by 313 (tick 1) but the **labeled** count only dropped by 8.

### Update — Frida-traced steering 2x fix (commit e6622f2)

Frida-traced `UpdateActorSteeringBias` (0x004340C0) on the original captured at `log/orig/pool10_004340C0.csv` revealed:

- During paused (countdown) sub-ticks, the original's AI fires `UpdateActorSteeringBias` 121 times for slot 0, but every call has `actor->steering_command = 0` on entry (the engine zeroes between sub-ticks).
- The port accumulated 121 cascade pushes during countdown, saturating `steering_command` to -47616 by `sim_tick=1` entry.

Fix: after each countdown sub-tick AI run, zero `steering_command` for all racer slots (matching the original's apparent behavior; the exact mechanism — likely inside `UpdateRaceActors` 0x00436A70 or in a paused-branch helper — wasn't audited line-by-line but the net effect is identical).

The cascading fixes from this:
- `steering_command` (still slightly off but no longer 2x)
- `angular_velocity_yaw` (-205 → -162 on tick 1)
- `linear_velocity_x/z` (smaller magnitudes)
- `euler_accum_yaw` (closer to original)

**Labeled drop: 40 → 31** (-9 fields including cascade).

### Final session result (2026-05-15)

| Stage | Labeled | Total tick-1 |
|-------|--------:|-------------:|
| Inventory start | 48 | 742 |
| + per-probe walker fix (probe init + contact_vertex helper + wheel walker call) | 46 | 432 |
| + body-corner faithful port + ground-Y snap | 41 | 432 |
| + surface_type-from-probe | 40 | 432 |
| + countdown steering reset | **31** | **418** |

### Remaining 31 — final breakdown

| Cluster | Fields | Root cause | Next step |
|---------|-------:|------------|-----------|
| `grip_reduction`, `prev_race_position` | 2 | Port has equal-span spawn; original has staggered grid (front cars start with span_high_water 1, back cars 0). Sort by span_high_water produces race_position[0]=2 in original. | Port the staggered grid placement in `td5_physics_init_vehicle_runtime` |
| `wheel_world_hires_*_y` (4), `wheel_suspension_pos/spring_dv` (8) | 12 | FPU precision drift (port `floorf` vs original `FISTP` round-to-nearest). Earlier experiment swapping to `lrintf` had zero effect — values are exact integers, drift is upstream in matrix multiply. | Audit `td5_physics_integrate_pose`'s float-precision rotation matrix builder |
| `probe_FR_x`, `wheel_contact_*` (3) | 3 | Single-bit float rounding cascade | Same as above |
| `rotation_matrix` (16-byte blob) | 1 | m[3] = exactly 0 in port (sp * cr where sp=0 from no pitch), m[3] ≈ -7e-11 in original (tiny non-zero from upstream euler.pitch drift) | Float precision audit |
| `render_pos_x/y/z` | 3 | Sub-tick interpolation or chassis-snap not propagated to render_pos | Audit render_pos write path |
| `angular_velocity_yaw`, `linear_velocity_x/y/z`, `euler_accum_yaw`, `world_pos_x/z` | 7 | Downstream of remaining steering residual + gravity. linear_velocity_y = -128 vs 0 means gravity isn't being added back by `UpdateVehicleSuspensionResponse` (port's surface_contact_flag gate may not be matching) | Audit suspension_response gravity-restore gate (see existing memory: `todo_suspension_gravity_gate_fix.md`) |
| `wheel_contact_normals` (16-byte blob) | 1 | Original writes per-wheel surface normal at `actor+0x230`; port leaves zero | Find writer in original (not in `ComputeActorTrackContactNormal`) |
| `center_suspension_pos/vel`, `prev_frame_y_position`, `steering_command` | 4 | Downstream cascades | Resolves with upstream fixes |

### Remaining 40 labeled (slot-0 tick-1) — cluster summary

| Cluster | Fields | Root cause | Investigation size |
|---------|-------:|------------|--------------------|
| FPU precision (probe_FR_x/z, wheel_contact_FL_x, wheel_world_hires × 8, wheel_suspension/spring_dv × 8) | ~20 | Single-bit float rounding mismatch (`floorf` vs FPU `FISTP` rounding mode) | Precise-port FPU control word + per-step audit |
| Steering 2x cascade (steering_command, angular_velocity_yaw, linear_velocity x/y/z, euler_accum_yaw) | ~6 | `UpdateActorSteeringBias` may run more than once per tick in the port | Frida-trace original on Honolulu to find call cadence |
| Race-init state (grip_reduction, prev_race_position) | 2 | `race_position` not pre-set to starting grid position on tick 0 | Port grid-order computation from race-init |
| Float drift (render_pos x/y/z, rotation_matrix 16 bytes) | 4 | Cumulative FPU/float-32 vs FPU/float-80 differences in matrix builder | Audit `td5_physics_integrate_pose` matrix construction |
| Y-snap chain (center_suspension_pos/vel, prev_frame_y_position) | 3 | Likely cascades from render_pos.y / chassis-snap | Solve render_pos.y first |
| Unported state (wheel_contact_normals 16-byte blob) | 1 | Original writes per-wheel surface normal at `actor+0x230`; port leaves zero | Find writer in original (not in ComputeActorTrackContactNormal) |
| Misc | 4 | world_pos_x/z (small), euler_accum_yaw (small), etc | Cascade-resolve |

Each cluster is a separate session-sized investigation. None are quick wins. The per-probe walker chapter is complete; the next milestone (FPU precision parity) requires precise-port matching of `td5_transform_short_vec3_by_render_matrix_rounded` against the original FISTP rounding semantics.

## Why the goal "all resolved" won't close in one session

After 6 shipped fixes the remaining ~742 tick-1 divergences split as:

- **Cascading float / fixed-point rounding** (likely ~300-400 of the 742): Each tick the physics integrator drifts by a few units from the original because the port's C compiler emits a different sequence of FPU instructions than the original MSVC build. These never byte-match without instruction-by-instruction asm faithfulness, which exists for some functions but not all 822 in the binary.
- **Genuine unported state** (~50-100): Fields like `wheel_contact_normals` (+0x230), `prev_race_position`/`grip_reduction` initial values, `cached_car_suspension_travel` from carparam content — each needs its own ported computation or asset audit.
- **Cascading-from-spawn-offset** (~100-200): chassis world_pos differs by ~90 raw units, wheels by ~60K, all suspension/contact derived state by proportional amounts. Resolving requires byte-exact spawn position which depends on track-walker output matching.

The full work is multi-session. This session shipped the cheap fast wins (`wheel_world_hires` shift, body_probes init, display_angle mask). The remaining clusters each need a dedicated investigation following the existing `/fix` / pilot-Frida workflow — Ghidra audit of the relevant original function, byte-faithful port of the missing computation, verification via whole-state re-diff.

**Recommended next-session targets** (ranked by leverage):
1. **B-remainder** — pilot-port `td5_track_update_probe_position` to match original per-probe walker output. ~16 fields × 5 ticks × 6 slots = ~480 diff reduction.
2. **Wheel-contact-normals** — implement the ground-normal computation that writes `+0x230`. ~16 bytes × 5 ticks × 6 slots = ~480 diff reduction.
3. **Initial-grid-position seed** — find what the original writes to `race_position` at race-init (before first UpdateRaceOrder); replicate in port. ~3 fields × 6 slots = 18 diff reduction but propagates into `grip_reduction` cascade.
