# Race RE Quality Pack — 2026-04-09

This pack consolidates the next source-side RE quality tasks for the race path:

1. contradiction ledger against the current RE notes
2. arithmetic fidelity audit
3. confidence / evidence dossier for race-critical functions
4. bug-driven validation pack for differential tracing

It is meant to complement, not replace, the existing system notes in `re/analysis/`.

---

## 1. Contradiction Ledger

### High severity

#### 1.1 Camera update ordering in the source port does not match the documented RunRaceFrame staging

Source:

- `td5mod/src/td5re/td5_game.c:1238-1242`
- `td5mod/src/td5re/td5_game.c:1372`

Current source order inside the sim loop:

1. `td5_input_poll_race_session()`
2. `td5_camera_tick()`
3. player control decode
4. `td5_physics_tick()`
5. `td5_ai_tick()`
6. `td5_track_tick()`
7. `td5_vfx_tick()`
8. `update_race_order()`

RE notes:

- `re/analysis/race-frame-hook-points.md`

Documented ordering in the binary separates:

1. early camera angle caching
2. actor / collision / race-order update
3. later chase-camera update

Why it matters:

- The port currently folds early cache and full chase-camera behavior into `td5_camera_tick()`.
- If the original binary updates chase camera after race order / collision resolution, the port can produce one-tick camera drift during collisions, race-order transitions, countdown release, and split-screen camera handoff.
- This is a strong candidate for race-pipeline bugs that present as “physics feels wrong” when the underlying divergence is actually stage order.

#### 1.2 Collision sequencing is implicit in `td5_physics_tick()` instead of an explicit post-actor-update phase

Source:

- `td5mod/src/td5re/td5_game.c:1359-1365`
- `td5mod/src/td5re/td5_physics.c:1735`

RE notes:

- `re/analysis/race-frame-hook-points.md`
- `re/analysis/collision-system.md`

Current source behavior:

- `td5_game.c` runs `td5_physics_tick()` before `td5_ai_tick()`, then `td5_track_tick()`.
- The documented binary pipeline treats collision resolution as a distinct phase after actor update dispatch.

Why it matters:

- The source port may still be behaviorally correct if `td5_physics_tick()` internally mirrors the binary’s dispatch boundaries, but the staging is currently opaque.
- This is a major auditability problem: when race behavior diverges, it is harder to prove whether the first error is control, actor update, collision, or track normalization.
- Differential tracing should expose the exact point, but the source should also document where collision actually happens relative to AI/track state.

#### 1.3 Point-to-point checkpoint progression is still placeholder logic

Source:

- `td5mod/src/td5re/td5_track.c:2212-2317`

Current source behavior:

- `td5_track_check_checkpoint()` explicitly says the point-to-point path is a proxy.
- It uses `track_state[3] > track_state[2]` as a stand-in for true checkpoint progression.

Why it matters:

- This is a direct gameplay-model gap, not just a naming gap.
- Any point-to-point race bug around checkpoint timing, missed gates, or finish conditions can be legitimate source-port incompleteness rather than a subtle arithmetic issue.
- This is one of the clearest “low-confidence but race-critical” subsystems remaining.

### Medium severity

#### 1.4 Circuit lap tracking is implemented, but the gating logic is simplified relative to the documented system

Source:

- `td5mod/src/td5re/td5_track.c:2338-2475`

RE notes:

- `re/analysis/race-progression-system.md`

Current source behavior:

- Route heading is approximated from `s_route_left[1] << 4`.
- Start-line acquisition uses a small fixed heading tolerance and a simplified checkpoint state model.

Why it matters:

- The port has a real implementation, but it still looks more heuristic than binary-faithful.
- If lap transitions or wrong-way resets are flaky, this area is a likely root cause.

#### 1.5 Countdown / paused-tick behavior is narrower than the documented frame pipeline

Source:

- `td5mod/src/td5re/td5_game.c:1330-1344`

RE notes:

- `re/analysis/race-frame-hook-points.md`

Current source behavior during countdown pause:

- player inputs still update
- physics and track tick
- AI, VFX, race order, and the rest of the sim loop do not run

Why it matters:

- This may be correct for movement lockout, but it is a likely source of countdown-release edge cases if the original still advances more state during the paused countdown window.

### Explicitly acknowledged source divergences

These are already called out in the code and should be treated as known fidelity gaps:

- `td5mod/src/td5re/td5_physics.c:1968`  
  Suspension-to-chassis pitch/roll torques disabled.
- `td5mod/src/td5re/td5_physics.c:2201`  
  Out-of-bounds recovery disabled.
- `td5mod/src/td5re/td5_physics.c:2480`  
  Recovery mode disabled because the simplified suspension model was unstable.

These are not accidental bugs. They are controlled deviations, and they deserve first-class tracking in race bug triage.

---

## 2. Arithmetic Fidelity Audit

### Highest-risk arithmetic drift

#### 2.1 Physics trig helpers use `double` trig instead of the original 12-bit lookup behavior

Source:

- `td5mod/src/td5re/td5_physics.c:155-181`
- `td5mod/src/td5re/td5_ai.c:229-236`

Current implementation:

- `cos_fixed12()`, `sin_fixed12()`, and `atan2_fixed12()` are computed from `cos()`, `sin()`, and `atan2()`.

Why this is high risk:

- The original binary used quantized lookup behavior.
- Continuous floating trig changes angle quantization, especially near small-angle steering, wall response, AI route heading, and OBB collision transforms.
- The port may be “smoother” but less exact, which is the wrong direction for a fidelity port.

#### 2.2 Track boundary normals and wall angles use float normalization plus `atan2`

Source:

- `td5mod/src/td5re/td5_track.c:526-544`
- `td5mod/src/td5re/td5_track.c:573-589`
- `td5mod/src/td5re/td5_track.c:2107`

Current implementation:

- boundary normals are normalized with `sqrtf`
- wall angles are derived via `atan2`

Why this is high risk:

- Track edge contact is one of the most bug-sensitive parts of the race pipeline.
- Small differences in wall normal and reflected heading can produce visible scrape, bounce, or spin divergence within a few frames.
- This is exactly the kind of math that should stay quantized if the original was quantized.

#### 2.3 Camera interpolation uses `+0.5f` rounding in many signed paths

Source:

- `td5mod/src/td5re/td5_camera.c:753`
- `td5mod/src/td5re/td5_camera.c:792`
- `td5mod/src/td5re/td5_camera.c:822-829`
- `td5mod/src/td5re/td5_camera.c:877-894`
- `td5mod/src/td5re/td5_camera.c:1415-1485`

Why this is high risk:

- `+0.5f` is only symmetric for positive values.
- For negative deltas, the original integer math may truncate or bias differently.
- This can create systematic camera drift, jitter, or off-by-one interpolation errors that are easy to misdiagnose as physics bugs.

### Medium-risk arithmetic drift

#### 2.4 `td5_track_compute_q1_angle` is explicitly approximate

Source:

- `td5mod/src/td5re/td5_track.c:2006-2010`

Why it matters:

- The source comments already admit this is an approximation.
- Any system using this for route heading, checkpoint alignment, or boundary classification inherits that approximation.

#### 2.5 Several suspension / wheel contact paths convert back through float world-space intermediates

Source:

- `td5mod/src/td5re/td5_physics.c:2055-2074`
- `td5mod/src/td5re/td5_physics.c:2250-2267`
- `td5mod/src/td5re/td5_physics.c:2359-2361`

Why it matters:

- Some of these are render-facing and harmless.
- Others feed wheel contact positions or derived suspension references.
- Any float round-trip in a live simulation path is a fidelity risk until proven otherwise.

#### 2.6 Negative wrap / division semantics around track normalization deserve direct binary validation

Source:

- `td5mod/src/td5re/td5_track.c:2133-2165`

Why it matters:

- The code intentionally compensates for C’s truncation-toward-zero behavior on negative modulo.
- That is reasonable, but it should still be checked against the original to confirm the exact lap / wrap semantics for reverse travel and wrong-way edge cases.

### Low-to-medium risk but worth tracking

#### 2.7 32-bit intermediate products may be hiding overflow-sensitive drift

Representative source:

- `td5mod/src/td5re/td5_physics.c:227-228`
- `td5mod/src/td5re/td5_physics.c:860-861`
- `td5mod/src/td5re/td5_ai.c:744-761`

Why it matters:

- The original x86 code often relies on very specific operand widths.
- In a port, an innocent-looking 32-bit multiply can drift from the original if the decompilation missed an implicit widening or a signed divide convention.
- This is a secondary audit target after the bigger trig / rounding issues above.

---

## 3. Confidence / Evidence Dossier

This is the race-critical confidence map, grouped by subsystem.

### High confidence

| Subsystem | Functions / areas | Why |
|---|---|---|
| Race loop timing | `td5_game_update_frame_timing`, sim accumulator flow in `td5_game.c` | Strongly documented in `main-game-loop-decomposition.md` and `tick-rate-dependent-constants.md`; the source structure is clear and explicit. |
| Core vehicle dynamics | `UpdatePlayerVehicleDynamics`, `UpdateAIVehicleDynamics`, `IntegrateVehiclePoseAndContacts`, `ResolveVehicleContacts` in `td5_physics.c` | Heavy inline documentation, named original addresses, and matching physics notes in `vehicle-dynamics-complete.md` / `collision-system.md`. |
| Actor layout | `re/include/td5_actor_struct.h` | The actor struct is one of the best-evidenced parts of the repo, and the tree already contains compile-time offset assertions. |
| Race HUD / indicator plumbing | `td5_hud.c` paths already called out in prior mapping notes | High confidence and not a current race-pipeline blocker. |

### Medium confidence

| Subsystem | Functions / areas | Why |
|---|---|---|
| Race loop staging | `td5_game_run_race_frame` | Overall flow is known, but the exact placement of camera/collision/order updates is still ambiguous relative to the binary notes. |
| AI routing / threshold behavior | `td5_ai.c` route following and threshold updates | Strong documentation exists, but the AI remains sensitive to small arithmetic / ordering drift. |
| Circuit lap tracking | `td5_track_update_circuit_lap` | Implemented and reasoned, but still more heuristic than fully proven. |
| Camera chase path | `UpdateChaseCamera` and `td5_camera_tick()` | Documented, but the source comments themselves acknowledge convention bugs and disabled branches in nearby camera code. |

### Low confidence

| Subsystem | Functions / areas | Why |
|---|---|---|
| Point-to-point checkpoint progression | `td5_track_check_checkpoint` P2P path | Explicit placeholder/proxy logic. |
| Track angle / boundary approximation paths | `td5_track_compute_q1_angle`, float-normalized wall-angle helpers | Explicit approximations in a bug-prone subsystem. |
| Recovery / out-of-bounds behavior | disabled paths in `td5_physics.c` | Known fidelity deviations that directly affect weird race incidents. |
| Split-screen camera coupling under collision/order changes | `td5_camera_tick()` + `td5_game_run_race_frame` ordering | Likely correct enough for many cases, but still under-documented and easy to mis-stage. |

### Best next RE targets

1. Prove the exact binary stage order between actor update, collision resolution, race-order update, and chase-camera update.
2. Replace the remaining approximate angle / trig paths in `td5_track.c` and validate them against the original.
3. Finish point-to-point checkpoint / finish logic from the real per-track data tables.
4. Revisit disabled recovery behavior once the trace harness can isolate whether it is a genuine missing feature or a workaround masking a deeper suspension bug.

---

## 4. Bug-Driven Validation Pack

Use this with:

- `re/analysis/race-trace-harness.md`
- `tools/compare_race_trace.py`

### Golden scenarios

#### Scenario A: Countdown release into straight launch

Purpose:

- validate paused-countdown tick behavior
- validate the first two live sim ticks after release

Record:

- player slot state
- steering/throttle/brake control bits
- `world_pos`
- `linear_velocity_*`
- `engine_speed_accum`
- `current_gear`
- `g_cameraTransitionActive`

#### Scenario B: Hard brake into a low-speed corner

Purpose:

- isolate control decode, longitudinal drag, steering clamp, yaw torque, and chase-camera coupling

Record:

- `steering_command`
- `longitudinal_speed`
- `lateral_speed`
- `angular_velocity_yaw`
- `front_axle_slip_excess`
- `rear_axle_slip_excess`
- camera world position

#### Scenario C: Sustained wall scrape

Purpose:

- validate track edge normals, wall angle, wall pushback, and post-contact camera stability

Record:

- `track_contact_flag`
- `wheel_contact_bitmask`
- `linear_velocity_x/z`
- `angular_velocity_yaw`
- `track_span_normalized`
- camera world position

#### Scenario D: Curb / crest / airborne landing

Purpose:

- validate suspension, probe contacts, gravity, landing bounce, and recovery thresholds

Record:

- `wheel_contact_bitmask`
- `world_pos.y`
- `linear_velocity_y`
- `center_suspension_pos`
- `center_suspension_vel`
- `airborne_frame_counter`

#### Scenario E: AI overtake or traffic interaction

Purpose:

- validate route threshold logic, peer selection, and collision sequencing with non-player actors

Record:

- player + nearest AI actor states
- normalized span / high-water span
- AI throttle / steer decisions if exposed
- collision flags and relative world positions

#### Scenario F: Lap / checkpoint transition

Purpose:

- validate circuit lap counting or point-to-point checkpoint advancement

Record:

- `track_span_normalized`
- `track_span_high_water`
- checkpoint index / bitmask field
- lap count field
- finish time field
- race position

#### Scenario G: Split-screen camera divergence

Purpose:

- validate whether view 0 / view 1 camera states stay independently stable during simultaneous steering and collisions

Record:

- `g_actorSlotForView[0/1]`
- `g_camWorldPos[0/1]`
- camera preset / transition state
- player actor states for both local slots

### Minimum trace stages

If the trace harness must stay small, keep these stages first:

1. `frame_begin`
2. `pre_physics`
3. `post_physics`
4. `post_ai`
5. `post_track`
6. `post_progress`
7. `frame_end`

These are enough to identify whether the first divergence is control, physics, AI coupling, track/contact, race-state progression, or render/camera coupling.

---

## 5. Practical Next Actions

1. Run the local artifact sync checker before trusting any markdown or JSON snapshot.
2. Use the trace harness on Scenarios A, B, C, and F first; those give the highest race-bug yield.
3. Treat the four low-confidence areas above as the next exact-match RE targets.

---

## 6. Current Hardening Baseline

The current tree already contains useful compile-time layout hardening in:

- `re/include/td5_actor_struct.h`

That header now asserts the actor size and a set of high-value field offsets used by the race path. This is the right pattern for RE-sensitive structures: fail the build on layout drift instead of letting a silent offset mistake reach runtime.

For artifact drift, use:

- `tools/check_re_sync.py`

Current result on the live inventory baseline:

- `876` functions
- `0` auto-named `FUN_...`
- `exe_func_bytes.json`, `exe_func_mnemonics.json`, and `exe_func_mnem20.json` all match the live `td5_inventory_live_t5_renamed.tsv` export
