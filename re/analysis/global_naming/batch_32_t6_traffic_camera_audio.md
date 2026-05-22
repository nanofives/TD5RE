---
batch: 32
area: traffic_camera_audio
tier: T6
target_todos: [todo-traffic-not-moving-2026-05-19, todo-traffic-clipping-ground-2026-05-19]
ghidra_session: TD5_pool3
analyzed_addresses: 0x00435a30, 0x00433800, 0x00401800, 0x00402780, 0x00440b50, 0x0043f100, 0x0043ea60
agent: tier1-a-t6
date: 2026-05-22
---

# Globals enumeration — traffic queue, trackside camera, vehicle audio (T6)

## Summary

- Functions analyzed: InitializeTrafficActorsFromQueue, RecycleTrafficActorFromQueue, FindActorTrackOffsetPeer, UpdateChaseCamera, UpdateTracksideCamera, UpdateTracksideOrbitCamera, UpdateStaticTracksideCamera, UpdateSplineTracksideCamera, UpdateVehicleAudioMix, PlayVehicleSoundAtPosition, AcquireTireTrackEmitter, InitializeTireTrackPool, UpdateTireTrackPool, UpdateFrontWheelSoundEffects, UpdateRearWheelSoundEffects, UpdateFrontTireEffects, UpdateRearTireEffects
- Unnamed DAT_* targeted: 18
- Already-named neighbors noted: g_tireTrackQuadPool (0x004c375c)
- Proposals — high: 12
- Proposals — medium: 4
- Proposals — comment-only: 2

## Methodology

Traffic queue cluster identified by reading InitializeTrafficActorsFromQueue (writers at 0x00435a30) — pointers walked via ESI from a fixed base. Camera presets indexed via `[reg*8 + base]` confirmed as 8-byte preset records. Tire-track emitter uses `[ECX + 0x4c3722]` offset pattern (per-emitter struct fields just like particle struct).

## Proposals (globals)

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x004ac6b8 | u16 | `g_trafficQueueCursor` | high | InitializeTrafficActorsFromQueue/RecycleTrafficActorFromQueue MOV [ESI],DI writes; cursor index for next traffic slot (18 refs). Matches W1-E hypothesis. | `(none) — port has td5_physics traffic cursor but offset uncertain` |
| 0x004ac6ba | u16 | `g_trafficQueueRecycleHead` | high | Adjacent +2, accessed `[ESI+2]` in InitializeTrafficActorsFromQueue (10 refs) | `(none)` |
| 0x004ac82c | u32 | `g_trafficSpawnTickAccumulator` | medium | RecycleTrafficActorFromQueue / InitializeTrafficActorsFromQueue (8 refs); `[ESI+0x174]` rel to a base | `(none)` |
| 0x004ac834 | u32 | `g_trafficSpawnTransform_PROVISIONAL` | medium | LEA `[ESI+0x17c]` pattern; receives traffic spawn transform copy (13 refs) | `(none)` |
| 0x004b0208 | u32 | `g_trafficPeerCandidatePtr` | medium | FindActorTrackOffsetPeer + InitializeTrafficActorsFromQueue (5 refs); ptr passed via EBP-relative | `(none)` |
| 0x004b0214 | u32 | `g_trafficPeerCandidateState` | medium | Same callers (5 refs); used as state-machine flag in peer-find | `(none)` |
| 0x00482e1a | u16 | `g_chaseCameraPresetSinXLut` | high | `MOVSX [ESI*8 + 0x482e1a]` in UpdateVehicleRelativeCamera/UpdateTracksideCamera/CacheVehicleCameraAngles (5 refs); part of 8-byte camera-preset record indexed by slot | `(none)` |
| 0x00482e1c | u16 | `g_chaseCameraPresetSinYLut` | high | Same pattern, +2 from above (5 refs) | `(none)` |
| 0x00482f34 | u32 | `g_tracksideCameraState_lookAhead` | high | `[EDI + 0x482f34]` writers UpdateTracksideCamera (11 refs) — part of TracksideCamera struct | `(none)` |
| 0x00482f38 | u32 | `g_tracksideCameraState_lookBehind` | high | `[EDI + 0x482f38]` adjacent +4 (11 refs) | `(none)` |
| 0x00482f52 | u16 | `g_tracksideCameraPresetAngle` | medium | `[EAX*8 + 0x482f52]` in UpdateTracksideOrbitCamera/UpdateChaseCamera (4 refs) — preset entry for orbit camera | `(none)` |
| 0x00482f54 | u16 | `g_tracksideCameraPresetDistance` | medium | `[ESI*8 + 0x482f54]` UpdateTracksideOrbitCamera/UpdateChaseCamera (6 refs) | `(none)` |
| 0x004c3722 | u8 (offset) | `g_tireTrackEmitter_offset_active` | high | `[ECX + 0x4c3722] = 0x1` in AcquireTireTrackEmitter init, =0 in UpdateFrontWheelSoundEffects clear (6 refs) — emitter-active byte | `(none)` |
| 0x004c3723 | u8 (offset) | `g_tireTrackEmitter_offset_kind` | high | `[ECX + 0x4c3723]` written from AL by AcquireTireTrackEmitter; read by emitter walkers (11 refs) | `(none)` |
| 0x004c3725 | u8 (offset) | `g_tireTrackEmitter_offset_phase` | high | `[ECX + 0x4c3725]` written by AcquireTireTrackEmitter/UpdateFrontWheelSoundEffects/UpdateFrontTireEffects; SHR [EAX],0x1 = decay (22 refs) | `(none)` |
| 0x004c3726 | u8 (offset) | `g_tireTrackEmitter_offset_slot` | high | `[ECX + 0x4c3726]` write site only (8 refs) | `(none)` |
| 0x004c3890 | u32 | `g_vehicleAudioMixState_PROVISIONAL` | medium | UpdateVehicleAudioMix + PlayVehicleSoundAtPosition (6 refs); audio-mix scalar | `(none)` |
| 0x004c3d14 | u32[6] | `g_vehicleShadowResourceTable` | medium | InitializeRaceVehicleRuntime + InitializeVehicleShadowAndWheelSpriteTemplates + LoadRaceVehicleAssets stores `[ESI*4 + 0x4c3d10]` (6 refs) — 6-slot shadow surface table | `(none)` |
| 0x004cf288 | u32 | (CRT/no callers) | low | Skip — likely vendor static init | `(none)` |
| 0x00497ae0 | u32 | `g_frontendSpriteBlitCursor` | medium | FlushFrontendSpriteBlits + QueueFrontendSpriteBlit + ResetFrontendOverlayState (6 refs) | `(none)` |

## Key discoveries

- 0x004ac6b8/ba confirms W1-E P2 hypothesis (`g_trafficQueueCursor`) as a paired 4-byte head/recycle-head structure. Wave 2 should type as `struct { uint16_t cursor; uint16_t recycle; }` for traffic dispatch.
- 0x004c3720..0x004c3728 is the **per-tire-track-emitter struct** (8 bytes). Like particles (batch 30), the absolute addresses are MSVC-emitted *field offsets* into a per-emitter base — not real storage. `phase` byte is decayed by `SHR [EAX],0x1` each tick.
- The trackside-camera preset LUT at 0x00482e1a..0x00482f54 has multiple sub-tables — chase-cam preset (4-byte stride), trackside-orbit (8-byte stride). Distinct from `g_cameraPresetTable` at 0x00463098.
- `g_vehicleShadowResourceTable` at 0x004c3d14 is a 6-entry (6 racers × pointer) surface array stored with `[ESI*4 + base]` stride. Should be typed as DDSurface*[6] in Wave 2.

## Out-of-scope finds

| address | brief note | probable area |
|---|---|---|
| 0x004ad150/52 | UpdateSpecialEncounterControl + UpdateSpecialTrafficEncounter | police chase / cop |
| 0x004bea98/aac | PlayIntroMovie + RequestIntroMovieShutdown | FMV |
| 0x004c3d2c | (3 refs) | shadow init |
| 0x0048f765, 0x0048f5d9, 0x0048f451 | save-config inner state | T6 save |

## TODO impact

- **todo-traffic-not-moving-2026-05-19**: Names of `g_trafficQueueCursor` and `g_trafficSpawnTickAccumulator` clarify ownership of the traffic dispatch path. Combined w/ Phase 3 cardef seed already shipped, no direct fix surfaced here but data layout becomes traceable.
- **todo-traffic-clipping-ground-2026-05-19**: Same surface — Phase 3 already addresses primary cause. Names will help port-side `td5_physics_traffic_*` cleanup.
