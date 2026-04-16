# Race Function Evidence Dossier — 2026-04-09

Purpose: keep the high-value race-path functions in one place with their current confidence, supporting evidence, and the next proof step needed to raise certainty.

## Timing / Race Flow

| Function | Current Source Owner | Confidence | Evidence | Open Question | Best Next Proof |
|---|---|---:|---|---|---|
| `RunRaceFrame` (`0x42B580`) | `td5_game.c` | Medium | Mapped in `main-game-loop-decomposition.md`, `race-frame-hook-points.md`, live source loop is easy to follow | Source stage split differs from hook-map writeup | Diff one trace against the binary at the tick-stage level |
| `CheckRaceCompletionState` (`0x409E80`) | `td5_game.c` + `td5_track.c` | Low | Strong docs in `race-progression-system.md`; source has a two-phase completion concept | Finish/lap ownership is split across multiple source functions | Trace per-slot completion state and compare first finished lap/race frame |
| `BuildRaceResultsTable` (`0x40A8C0`) | `td5_game.c` | Medium | Good naming and dedicated doc references | Needs validation against post-finish ordering and tiebreak fields | Compare results table against binary after a known race finish |
| `td5_game_update_frame_timing` | `td5_game.c` | Low | Clear source, clear docs, direct contradiction on units | Which fields should remain in 30 Hz normalized space vs wall-clock seconds? | Reconstruct the exact binary timing formula and align all dependent call sites |

## Vehicle / Collision

| Function | Current Source Owner | Confidence | Evidence | Open Question | Best Next Proof |
|---|---|---:|---|---|---|
| `UpdatePlayerVehicleDynamics` (`0x404030`) | `td5_physics.c` | High | Deep notes in `vehicle-dynamics-complete.md`; source uses resolved actor fields | Exact trig / LUT fidelity still open | Differential trace on player speed, yaw rate, slip, and gear across 10-20 seconds |
| `UpdateAIVehicleDynamics` (`0x404EC0`) | `td5_physics.c` | High | Mapped in docs and source comments, AI grip asymmetry documented | Steering/rubber-band interplay needs more validation | Compare AI yaw / throttle / route bias on one course segment |
| `IntegrateVehiclePoseAndContacts` (`0x405E80`) | `td5_physics.c` | High | Central function is well documented and source comments are detailed | Exact suspension/ground-snap math still fragile | Per-tick trace of pose, grounded mask, suspension positions |
| `ResolveVehicleContacts` (`0x409150`) | `td5_physics.c` | High | `collision-system.md` and source match at the subsystem level | Impulse magnitude and angle exactness may still differ | Binary/port compare for a controlled collision scenario |
| `ApplyTrackSurfaceForceToActor` (`0x406980`) | `td5_physics.c` | Medium | Source and collision notes both identify wall response semantics | Trig/angle helper fidelity and damping exactness still open | Hit wall at fixed angle/speed and compare rebound vector |

## Track / Progression

| Function | Current Source Owner | Confidence | Evidence | Open Question | Best Next Proof |
|---|---|---:|---|---|---|
| `UpdateActorTrackPosition` (`0x4440F0`) | `td5_track.c` | High | Strong docs, clear source walk of boundary transitions | Exact junction behavior on edge cases | Trace span/sub-lane transitions through a complex junction |
| `NormalizeActorTrackWrapState` (`0x443FB0`) | `td5_track.c` | High | Straightforward and documented in `race-progression-system.md` | None major beyond signed wrap edge cases | Unit-style replay over negative/overflowed accumulated spans |
| `td5_track_update_circuit_lap` | `td5_track.c` | Low | Source is clear, but it conflicts with the doc-owned progression model and `advance_pending_finish_state()` | Should this function own lap logic at all? | Decide canonical owner, then delete or subordinate the duplicate path |
| `td5_track_update_race_order` | `td5_track.c` | Low | Function name is known | Current implementation is placeholder-backed and not authoritative | Remove from live tick path or fully wire to actor data |
| `td5_track_tick` | `td5_track.c` | Low | Function exists and is called every tick | Module comment still says it corresponds to `UpdateRaceActors`, but it does not | Reassign module comments and strip dead/placeholder responsibility |

## AI / Traffic

| Function | Current Source Owner | Confidence | Evidence | Open Question | Best Next Proof |
|---|---|---:|---|---|---|
| `ComputeAIRubberBandThrottle` (`0x432D60`) | `td5_ai.c` | Medium | Dedicated AI notes and readable source | Needs gameplay validation more than naming | Compare throttle bias over relative span distance |
| `UpdateActorSteeringBias` (`0x4340C0`) | `td5_ai.c` | Medium | Well documented and mostly matched in source | Sensitive to heading-source fidelity | Trace steering command vs route deviation on one AI slot |
| `AdvanceActorTrackScript` (`0x4370A0`) | `td5_ai.c` | Low | Opcode VM is identified and documented | Script turn-alignment still uses simplified route-heading data in source | Direct binary/source compare on a lane-change or recovery script event |
| `UpdateTrafficRoutePlan` (`0x435E80`) | `td5_ai.c` | Medium | Traffic routing is reasonably mapped | Interacts with simplified traffic dynamics | Validate one recycle/spawn/overtake sequence against the binary |

## Camera

| Function | Current Source Owner | Confidence | Evidence | Open Question | Best Next Proof |
|---|---|---:|---|---|---|
| `UpdateChaseCamera` (`0x401590`) | `td5_camera.c` | High | Dedicated camera docs and named live source | Exact `__ftol`-style rounding and LUT trig are still open | Diff camera world pos / orientation over a short replay |
| `CacheVehicleCameraAngles` (`0x402A80`) | `td5_camera.c` | High | Small, well-identified helper | None major | Include in per-tick camera trace |
| `UpdateRaceCameraTransitionState` (`0x401E10`) | `td5_camera.c` | Medium | Well identified, but coupled to countdown/fade timing | Depends on timing-unit cleanup | Compare countdown/timer transitions on race start |
| Trackside camera family | `td5_camera.c` | Medium | Good subsystem docs, less validation density | Visual correctness may hide arithmetic drift | Capture one deterministic replay with camera-mode cycling |
