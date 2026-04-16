# Race Top 25 Evidence Dossier — 2026-04-09

Purpose: keep the 25 highest-value race-critical functions in one evidence-backed list.

| # | Function | Source Owner | Confidence | Why It Matters | Best Next Proof |
|---|---|---|---|---|---|
| 1 | `RunRaceFrame` (`0x42B580`) | `td5_game.c` | Medium | Main stage owner for the whole race pipeline | Tick-stage diff trace |
| 2 | `CheckRaceCompletionState` (`0x409E80`) | `td5_game.c` | Low | Race-end and finish ownership are still split in source | Finish-state trace around lap/checkpoint completion |
| 3 | `BuildRaceResultsTable` (`0x40A8C0`) | `td5_game.c` | Medium | Post-race correctness and time-trial tiebreaks | Compare results after a known finish |
| 4 | `UpdateRaceOrder` (`0x42F5B0`) | `td5_game.c` | Medium | Position bugs surface everywhere in HUD and logic | AI overtake trace |
| 5 | `BeginRaceFadeOutTransition` (`0x42CC20`) | `td5_game.c` | High | End-of-race flow and pause-exit transitions | Fade-state trace |
| 6 | `UpdatePlayerVehicleDynamics` (`0x404030`) | `td5_physics.c` | High | Core player handling and slip behavior | Straight-line + corner entry trace |
| 7 | `UpdateAIVehicleDynamics` (`0x404EC0`) | `td5_physics.c` | High | AI stability and overtake behavior | AI overtake trace |
| 8 | `IntegrateVehiclePoseAndContacts` (`0x405E80`) | `td5_physics.c` | High | Pose, gravity, ground contact, and matrix rebuild | Per-tick pose/contact trace |
| 9 | `ResolveVehicleContacts` (`0x409150`) | `td5_physics.c` | High | Vehicle collision ordering and impulses | Controlled collision scenario |
| 10 | `ApplyVehicleCollisionImpulse` (`0x4079C0`) | `td5_physics.c` | High | Impact magnitude and angular response | Collision test cases |
| 11 | `ApplyTrackSurfaceForceToActor` (`0x406980`) | `td5_physics.c` | Medium | Wall scrape and rebound behavior | Fixed-angle wall scrape |
| 12 | `UpdateVehicleSuspensionResponse` (`0x4057F0`) | `td5_physics.c` | Low | Major known fidelity gap for pitch/roll coupling | Curb / landing trace |
| 13 | `ClampVehicleAttitudeLimits` (`0x405B40`) | `td5_physics.c` | Low | Recovery and out-of-bounds stability | Recovery incident trace |
| 14 | `UpdateActorTrackPosition` (`0x4440F0`) | `td5_track.c` | High | Ground truth for span and lane progress | Junction transition trace |
| 15 | `NormalizeActorTrackWrapState` (`0x443FB0`) | `td5_track.c` | High | Lap/wrap correctness and ordering | Wrap edge-case trace |
| 16 | `UpdateActorTrackSegmentContacts` (`0x406CC0`) | `td5_track.c` / `td5_physics.c` | Medium | Lateral boundary correctness | Wall scrape + curb contact |
| 17 | `ComputeActorTrackContactNormal` (`0x445450`) | `td5_track.c` | Medium | Terrain normal fidelity into physics | Crest/landing trace |
| 18 | `td5_track_update_circuit_lap` | `td5_track.c` | Low | Current live source owner conflict | Canonical-owner decision plus lap trace |
| 19 | `ComputeAIRubberBandThrottle` (`0x432D60`) | `td5_ai.c` | Medium | AI pace and comeback behavior | Relative-span vs throttle trace |
| 20 | `UpdateActorSteeringBias` (`0x4340C0`) | `td5_ai.c` | Medium | AI steering aggression and stability | Route-deviation trace |
| 21 | `AdvanceActorTrackScript` (`0x4370A0`) | `td5_ai.c` | Low | Scripted AI alignment still simplified | Lane-change / recovery trace |
| 22 | `UpdateTrafficRoutePlan` (`0x435E80`) | `td5_ai.c` | Medium | Traffic interactions and recycle behavior | Traffic merge/pass trace |
| 23 | `UpdateChaseCamera` (`0x401590`) | `td5_camera.c` | High | Camera feel and race readability | Short replay camera trace |
| 24 | `CacheVehicleCameraAngles` (`0x402A80`) | `td5_camera.c` | High | Camera interpolation staging | Include in standing-start trace |
| 25 | `UpdateRaceCameraTransitionState` (`0x401E10`) | `td5_camera.c` | Medium | Countdown and transition correctness | Countdown release trace |

## Highest-Leverage Open Questions

1. Which source module should own race progression state?
2. How much of the remaining “physics feel” gap is really suspension vs timing/order drift?
3. Which trig/angle helper should become the canonical quantized implementation for race-critical math?
