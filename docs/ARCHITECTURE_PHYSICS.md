# TD5RE Physics Architecture

Vehicle simulation for the TD5 source port. Core file: `td5mod/src/td5re/td5_physics.c` (~11.7k lines, listing-faithful port of the original's physics functions — address map in the file header, lines 8–36). Supporting files: `td5_track.c` (ground/wall geometry queries), `td5_types.h` (fixed-point + angle math, `TD5_TrackProbe`), `re/include/td5_actor_struct.h` (full 0x388-byte `TD5_Actor`), `td5re.h` (timing fields), `td5_game.c` (tick loop). All paths below are relative to `td5mod/src/td5re/` unless noted.

## Numeric model — integer fixed-point only

The sim loop is **pure integer math**; no floats in simulation arithmetic (td5_physics.c:4–6). Three conventions:

- **24.8 fixed-point** world coordinates/velocities: `TD5_FP_SHIFT=8`, `TD5_FP_ONE=256`, macros `TD5_FP_TO_FLOAT` / `TD5_FP_MUL` / `TD5_FP_DIV` (td5_types.h:20–44). `world_pos` is 24.8; `render_pos` is float, derived only for rendering.
- **12-bit angles**: full circle = `TD5_ANGLE_FULL = 0x1000` (td5_types.h:51). Display angles (`display_angles.roll/yaw/pitch`) are 12-bit; the 24.8 *accumulators* (`euler_accum`, actor +0x1F0) are display<<8. Helpers `td5_angle12_signed(x)` = `((x-0x800)&0xFFF)-0x800` (maps 0x800 to −0x800, matching the original register dance) and `td5_angle12_delta(a,b)` (td5_types.h:634–644). Trig via integer LUT wrappers `cos_fixed12`/`sin_fixed12`/`atan2_fixed12` (td5_physics.c:347–367).
- **SAR-RZ division idiom** (td5_physics.c:66–91): the original MSVC compiler emits `CDQ; AND EDX,mask; ADD; SAR` for signed divide-by-2^N — **round-toward-zero**. Plain C `>>` rounds toward −inf and is off by 1 LSB for negative odd values. Any site sourced from that asm idiom must use `SAR_RZ_6/8/12/15` or the local `sar8_rz`-family inlines (e.g. lines 3331, 3686, 9475). These 1-LSB differences cascade (audits D1/D2/D5/D10/D14 in the player-dynamics header, lines 1475–1483) — never "simplify" them to plain shifts.

## Tick & determinism

Fixed **30 Hz** simulation. `td5_game_update_frame_timing` (td5_game.c:6201) computes `frame_dt_normalized = dt_seconds * 30` and adds it to the 16.16 accumulator `g_td5.sim_time_accumulator` (`TD5_TICK_ACCUMULATOR_ONE = 0x10000`, spiral cap `TD5_MAX_SIM_BUDGET = 4.0` ticks/frame, td5_types.h:60–61; fields in td5re.h:189–196). The race-frame drain loop consumes one tick per iteration; per sub-tick order (td5_game.c:4248–4328): `td5_ai_tick()` → race-end auto-brake → **`td5_physics_tick()`** → wanted-tracker → `td5_vfx_tick()` → weather density → `update_race_order()` → `td5_camera_solve_tick_all()`.

`td5_physics_tick` (td5_physics.c:849) snapshots `prev_world_pos`, runs `td5_physics_update_vehicle_actor` for every actor slot, then `td5_physics_resolve_vehicle_contacts()` **unconditionally** (matches RunRaceFrame @0x0042B580 — no paused gate; the positional pushes separate the spawn-grid overlap during countdown, lines 881–895). During countdown, skipped middle sub-ticks still run `td5_physics_run_paused_engine_step` / `run_paused_traffic_step` (td5_game.c:4047–4067). After the drain, `g_subTickFraction = accumulator/0x10000` feeds `td5_physics_apply_render_interpolation` (td5_physics.c:814) which lerps `render_pos` **X/Z only** — Y is deliberately not interpolated (listing-anchored rationale at lines 832–844).

**Determinism is a hard requirement**: netplay is strict input-lockstep with no state correction (td5_net.c:2–6) and replays replay inputs only (td5_input.c), so the sim must be bit-identical across peers/runs. `td5_msvc_rand.c` overrides mingw `rand()/srand()` with the MSVC 6.0 LCG (`x*0x343FD+0x269EC3`, `>>16 & 0x7FFF`) because a different RNG sequence cascades into different AI behaviour. Do not add, remove, or reorder `rand()` calls in the sim path, and do not parallelize per-actor physics (per-actor order and RNG order are load-bearing). `[Trace] RaceTrace=1` fixes the seed for A/B runs.

## Actor classes & dispatch

Actors live in a flat table at `g_actor_table_base`, stride `TD5_ACTOR_STRIDE = 0x388`. Racer slots `0..g_traffic_slot_base-1`, traffic above that (legacy boundary 6; port supports up to 16 racers + 6 traffic, td5_types.h:89–102). Dispatcher `td5_physics_update_vehicle_actor` (line 1070, orig UpdateVehicleActor @0x406650) routes each actor:

- **Traffic** (`slot >= 6`, `vehicle_mode==0`): `td5_physics_update_traffic` (line 3210, orig IntegrateVehicleFrictionForces @0x4438F0) — simplified friction/steering integrator, runs every sub-tick **including countdown**.
- **Stunned recovery** (`wheel_contact_bitmask==0x0F && airborne_frame_counter>=3`): `td5_physics_state0f_damping` (line 9077).
- **Human player** (`g_race_slot_state[slot]==1`, strict): `td5_physics_update_player` (line 1502, orig @0x404030) — full 4-wheel model: surface probes → drag → body-frame trig → load transfer → drivetrain dispatch → slip-circle.
- **AI racer**: `td5_physics_update_ai` (line 2742, orig @0x404EC0) — 2-axle bicycle model (same outer phases, axle-level grip solve).
- **`vehicle_mode==1`** (scripted crash recovery): `td5_physics_refresh_scripted_vehicle_transforms` → `td5_physics_integrate_scripted_motion` chain (lines 1183–1232; traffic never enters this in the port — writer side not ported, documented there).

After dynamics, racers (`slot < 6`) get wall containment: `td5_track_resolve_reverse_contacts` / `resolve_forward_contacts` / `resolve_wall_contacts` (line 1408–1411) — runs in recovery mode too (OOB-slide fix rationale at 1390–1401).

## Tuning tables (carparam.dat)

Each car ships a 268-byte `carparam.dat`: bytes `0x00..0x8B` = **cardef** (bounding box, wheel positions, collision mass) → `actor->car_definition_ptr`; bytes `0x8C..0x10B` = **physics tuning** (0x80 bytes) → `actor->tuning_data_ptr` (`td5_physics_load_carparam`, line 11018). Access via `get_phys`/`get_cardef` + `PHYS_S/PHYS_I/CDEF_S` byte-offset macros and the **named offsets** at lines 685–716: `PHYS_INERTIA_YAW 0x20` (i32), `PHYS_HALF_WHEELBASE 0x24` (i32), `PHYS_FRONT/REAR_WEIGHT 0x28/0x2A`, `PHYS_TIRE_GRIP_COEFF 0x2C`, `PHYS_GEAR_RATIO_BASE 0x2E` (i16[gear]), `PHYS_GEAR_UP/DOWNSHIFT_BASE 0x3E/0x4E`, suspension `PHYS_SUSP_POS_DAMP/VEL_DAMP/SPRING/TRAVEL_LIM/LOAD_SCALE 0x5E..0x66`, `PHYS_DRIVE_TORQUE_MULT 0x68`, `PHYS_DAMP_COEFF_TURN/BASE 0x6A/0x6C`, `PHYS_BRAKE_FRONT/REAR 0x6E/0x70`, `PHYS_REDLINE_RPM 0x72`, `PHYS_TOP_SPEED 0x74`, `PHYS_DRIVETRAIN_TYPE 0x76`, `PHYS_SPEED_SCALE 0x78`, `PHYS_HANDBRAKE_MOD 0x7A`, `PHYS_SLIP_COUPLING 0x7C`; cardef: `CDEF_FRONT_Z_EXTENT 0x04`, `CDEF_HALF_WIDTH 0x08`, `CDEF_REAR_Z_EXTENT 0x14` (negative), `CDEF_WHEEL_Y_BASE 0x42` (stride 8), `CDEF_COLLISION_RADIUS 0x80`, `CDEF_SUSP_REF_HEIGHT 0x82`, `CDEF_HEIGHT_OFFSET 0x86`.

Binding (`bind_default_vehicle_tuning`, line 10540): tables are **per-actor copies** (`s_default_tuning/cardef` — addressing-scheme divergence only, bytes faithful, line 259). Player slot 0 uses its own carparam; **AI racer slots get the global AI physics template** (orig DAT_00473DB0 — Wf/Wr/inertia/wheelbase/grip; sourcing those from carparam flips the bicycle-determinant sign and spins the car, lines 10548–10643). With `AIAccelFromCar=1` only top-speed (+0x74) and drive-torque (+0x68) are re-sourced from each AI car's carparam, scaled by `k_ai_tier_top_pct`/`k_ai_tier_torque_pct[difficulty_tier]` (lines 10537–10655). Traffic keeps carparam but cardef+0x88 mass is forced to 0x20 at init (line 10207).

`td5_physics_init_vehicle_runtime` (line 10144, orig @0x42F140) **rewrites tuning in place at race init**, keyed off `g_difficulty_easy` — which is the **Dynamics** option, not the Easy/Normal/Hard difficulty (`td5_physics_set_dynamics`, line 10923: arcade→easy=0, simulation→easy=1; `g_difficulty_hard` has no writers, its branch is dead): gravity 1500/1900/2048 (easy/normal/hard); "Normal" (=arcade) scales `DRIVE_TORQUE_MULT *0x168>>8`, `TIRE_GRIP_COEFF *300>>8`, `SPEED_SCALE <<1`; the dead Hard branch is also ported (lines 10218–10255). Per-slot championship handicap then rescales gear ratios, top speed, damping and grip (lines 10268–10356). Surface grip/drag tables (`short[32]`, indices 16–31 = off-strip/grass) and the per-gear torque table are seeded in `td5_physics_init` (lines 725–775).

## Collisions

**Track contacts (probes)**: each actor carries `wheel_probes[4]` + `body_probes[4]` of `TD5_TrackProbe` (16 bytes: span_index, normalized/accumulated span, contact vertices, sub_lane — td5_types.h:434). `td5_physics_refresh_wheel_contacts` (line 7915, orig @0x403720) seeds each probe from the actor span, transforms wheel positions to world, then calls the td5_track walkers: `td5_track_update_probe_position` (span walking per probe), `td5_track_compute_contact_height_with_normal/_bounded` (ground Y at span/sub-lane), `td5_track_get_surface_type(actor, probe_idx)`, `td5_track_probe_height` (raw world-XZ height query) — interface in td5_track.h:155–230. `td5_track_update_actor_position` keeps the chassis span current.

**Wall response**: `td5_physics_wall_response` (line 392, orig ApplyTrackSurfaceForceToActor @0x406980), called by td5_track.c's wall/forward/reverse resolvers. Push-out along the wall normal by `(penetration−4)>>4`; velocity decomposed into wall-tangent/normal with SAR-RZ-12; early-out when separating (`iVar11<0`); impulse from `V2W_INERTIA_K=1,500,000`, numerator scale `0x1100`, angular divisor `0x28C`; yaw kick `impulse*lever/2300`; SFX/FF above `v_perp>0x3200`. The tangential-damping clamp has history: `EXPECTED_BEHAVIOR.md` ("Intentional Divergences") documents a magnitude-only **soft clamp** on the `v_para<=0` branch, but the live code **reverted to the faithful hard-zero on sign-flip** on 2026-05-10 after a Frida pass showed the soft clamp made AI cars stall against walls (td5_physics.c:498–540; the `"soft-clamp fired"` log name at line 637 survives, now reporting the faithful clamp). Treat the EXPECTED_BEHAVIOR.md entry as stale on this point.

**V2V**: `td5_physics_resolve_vehicle_contacts` (line 4649, orig @0x409150). Phase 1: span-bucketed AABB grid (racers + traffic). Phase 2: pair walk → `resolve_collision_pair` (line 4952) dispatches scripted/contact-saturated actors to `collision_detect_simple` (sphere) and the rest to `collision_detect_full` (line 4310, orig @0x408A60): AABB pre-test (`<=` rejects at equality), `obb_corner_test` of 8 corners at end-of-tick pose, then a **7-iteration TOI bisection** halving the per-axis velocity/yaw steps in raw 24.8 (truncating `/2`, lines 4382–4429), yielding `impactForce`. `apply_collision_response` (orig ApplyVehicleCollisionImpulse @0x4079C0) splits side vs front/rear by corner position, computes the impulse (`V2V_INERTIA_K=500,000`, `NUM_CONST=(K>>8)*0x1100` — the `>>8` is load-bearing scaling, line 3920–3935), performs **TOI rollback** of pos/yaw by `sar8_rz((0x100−impactForce)*vel)` before committing new velocities and re-advances after (lines 4054–4091). Heavy impacts (`impact_mag>90000`, collisions ON) add angular scatter + vertical lift to racers and scripted crash-spin to traffic (line 4181); a human rear-ended victim's spin/lift is scaled by `RearImpactResponse` (lines 3898–3911, 4193). Phase 2.5: **AntiTunnel** (port-only, line 4817) — O(n²) position-only depenetration over live positions, relax rounds, never touches velocities, leaves `AntiTunnelSlop` units of OBB overlap so cars settle at mesh contact (`v2v_depenetrate_pair`, line 4570).

## Physics-relevant INI knobs (`[GameOptions]`, main.c:674–702)

- `Dynamics` (default 0): 0=arcade / 1=simulation → `td5_physics_set_dynamics` → `g_difficulty_easy` → gravity + init-time stat scaling (above).
- `Collisions` (default 1): 3D collisions toggle → `td5_physics_set_collisions`; stored **inverted** (`g_collisions_enabled==0` means ON, line 10912 + 4179); also selects attitude-clamp mode in `td5_physics_clamp_attitude` (line 8595).
- `RearImpactResponse` (default 45, 0–100): % of angular/lift response a human keeps when hit on the rear face; 100 = byte-faithful.
- `AntiTunnel` (default 1) / `AntiTunnelSlop` (default 40, 0–256): port-only V2V depenetration pass + allowed resting overlap.
- `CatchupAssist` (default −1 = use persisted save value; 0=off, 1–9 override): resolved by `td5_ai_get_catchup_level()` (td5_ai.c:1450); gates AI rubber-banding.
- `AIAccelFromCar` (default 1): AI top-speed/accel sourced per-car with tier scaling; 0 = faithful template constants.
- `PhysicsLOD` (default 1): distant cars run heavy phases (span-walk, ground-snap, suspension, wall/edge resolvers) every 2nd tick (td5re.h:288–293).

## Key entry points

| Function (td5_physics.c line) | Original addr | Role |
|---|---|---|
| `td5_physics_tick` (849) | RunRaceFrame body | Per-sub-tick driver: snapshot, per-actor update, V2V |
| `td5_physics_update_vehicle_actor` (1070) | 0x406650 | Per-actor dispatcher (player/AI/traffic/recovery) |
| `td5_physics_update_player` (1502) | 0x404030 | 4-wheel player dynamics |
| `td5_physics_update_ai` (2742) | 0x404EC0 | 2-axle AI bicycle dynamics |
| `td5_physics_update_traffic` (3210) | 0x4438F0 | Simplified traffic friction integrator |
| `td5_physics_wall_response` (392) | 0x406980 | Wall impulse + push-out (called from td5_track.c resolvers) |
| `td5_physics_resolve_vehicle_contacts` (4649) | 0x409150 | V2V broadphase + dispatch + anti-tunnel |
| `collision_detect_full` (4310) | 0x408A60 | OBB test + 7-iter TOI bisection |
| `td5_physics_refresh_wheel_contacts` (7915) | 0x403720 | Wheel/body probe pass against the strip |
| `td5_physics_integrate_pose` (6785) | 0x405E80 | Pose/contact integration |
| `td5_physics_integrate_suspension` (5021) | 0x403A20 | Per-wheel spring/damper using `PHYS_SUSP_*` |
| `td5_physics_compute_drive_torque` (9703) | 0x42F030 | Torque-curve LUT × `PHYS_DRIVE_TORQUE_MULT` × gear ratio |
| `td5_physics_clamp_attitude` (8576) | 0x405B40 | Roll/pitch nudge+clamp (mode by collisions flag) |
| `td5_physics_init_vehicle_runtime` (10144) | 0x42F140 | Race-init tuning bind + difficulty/handicap rewrite |
| `td5_physics_load_carparam` (11018) | — | carparam.dat → per-slot cardef/tuning staging |
| `td5_physics_apply_render_interpolation` (814) | — | Sub-tick render lerp (X/Z only) |
