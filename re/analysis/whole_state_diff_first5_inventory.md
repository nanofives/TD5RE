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
