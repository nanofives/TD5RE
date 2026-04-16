# Race Function Confidence Map — 2026-04-09

Scope: race-critical source functions only. This is the practical confidence map
for bug-fixing, not a whole-program naming audit.

## High Confidence

These areas are strongly backed by existing docs plus coherent source behavior.

| Subsystem | Functions / Area | Why |
|---|---|---|
| Frame / race progression | `td5_game_run_race_frame`, `check_race_completion`, `update_race_order`, checkpoint/lap helpers in [`td5_game.c`](C:/Users/maria/Desktop/Proyectos/TD5RE/td5mod/src/td5re/td5_game.c) | Good doc coverage in `main-game-loop-decomposition.md`, `race-progression-system.md`, and `race-frame-hook-points.md`; current source is readable and stage-localized even where order drift exists. |
| Track runtime data | STRIP / MODELS / route loading in [`td5_track.c`](C:/Users/maria/Desktop/Proyectos/TD5RE/td5mod/src/td5re/td5_track.c) | Strong file-format documentation and explicit source commentary; data loading and pointer binding are comparatively stable. |
| Collision formulas | `ResolveVehicleContacts`, `ApplyVehicleCollisionImpulse`, wall response in [`td5_physics.c`](C:/Users/maria/Desktop/Proyectos/TD5RE/td5mod/src/td5re/td5_physics.c) | Backed by `collision-system.md` and `physics-validation-testcases.md`; the formulas are at least explicit and reviewable even if some broader integration details remain open. |

## Medium Confidence

These areas are implemented and named well enough, but they still contain inferred or partially validated semantics.

| Subsystem | Functions / Area | Why |
|---|---|---|
| Player / AI longitudinal-lateral dynamics | `UpdatePlayerVehicleDynamics`, `UpdateAIVehicleDynamics` in [`td5_physics.c`](C:/Users/maria/Desktop/Proyectos/TD5RE/td5mod/src/td5re/td5_physics.c) | Core formulas are present, but several surrounding force and damping interactions still depend on reconstructed constants and helper behavior. |
| Track contact / heading | `UpdateActorTrackPosition`, `td5_track_compute_contact_height`, `td5_track_compute_heading` in [`td5_track.c`](C:/Users/maria/Desktop/Proyectos/TD5RE/td5mod/src/td5re/td5_track.c) | Good structural coverage, but heading-angle math and some lookup behaviors are still approximate. |
| AI routing and rubber-band | threshold, steering bias, traffic routing, and encounter gating in [`td5_ai.c`](C:/Users/maria/Desktop/Proyectos/TD5RE/td5mod/src/td5re/td5_ai.c) | Major systems are identified, but the route-state semantics are dense and a few explicit simplifications remain. |
| Camera state machine | chase / trackside / spline cameras in [`td5_camera.c`](C:/Users/maria/Desktop/Proyectos/TD5RE/td5mod/src/td5re/td5_camera.c) | Backed by good documentation and stable presets, but some float/trig quantization details are still approximate. |

## Low Confidence

These are the most bug-prone race-path areas and the best next RE targets.

| Subsystem | Functions / Area | Why |
|---|---|---|
| Suspension integrator | `td5_physics_integrate_suspension` | Source comments explicitly say the original XZ projection logic is not fully decompiled and the current path is a stable substitute. |
| Suspension-to-attitude coupling | `td5_physics_update_suspension_response` | Original torque coupling is intentionally disabled; current behavior is a protective simplification. |
| Attitude recovery mode | `td5_physics_clamp_attitude` | Recovery mode is currently disabled in favor of hard clamping. |
| Route/script alignment | encounter/script steering blocks in [`td5_ai.c`](C:/Users/maria/Desktop/Proyectos/TD5RE/td5mod/src/td5re/td5_ai.c) | Uses simplified heading/threshold logic exactly where scripted AI can feel wrong. |
| Angle quantization helpers | trig / `atan2` replacements in physics, track, and camera | These feed multiple systems and are still approximate. |

## Best Next Reverse-Engineering Targets

1. Original suspension force projection and its feed into pitch/roll.
2. Exact recovery-mode entry and decay conditions.
3. Exact route-state heading / threshold semantics for AI scripts.
4. Original trig and angle lookup behavior.
5. Split-screen camera ownership of shared float state.
