# RunRaceFrame Hook Point Map

Comprehensive reference for hooking the main race simulation loop at `0x42B580` (RunRaceFrame).
Based on full Ghidra decompilation and disassembly of the function and its major callees.

---

## Overview: RunRaceFrame Execution Pipeline

RunRaceFrame is the per-frame entry point called by the game state dispatcher (`RunMainGameLoop` at 0x442170) when `g_gameState == 2`. It returns 0 to continue racing, or 1 to signal race-over (triggers transition back to frontend). The function runs a fixed-timestep simulation loop (inner while-loop) interleaved with a render pass.

**Frame structure at a glance:**

```
1. Disabled-slot zero-out pass
2. Sim time accumulator setup
3. Replay timeout / finish fade logic
4. CheckRaceCompletionState (race-end detection)
5. Fade-out progression + resource teardown on completion
6. === FIXED-TIMESTEP SIM LOOP (while simTime > 0xFFFF) ===
   a. PollRaceSessionInput
   b. CacheVehicleCameraAngles (P1, P2)
   c. UpdatePlayerSteeringWeightBalance (2P only)
   d. UpdatePlayerVehicleControlState (per active slot)
   e. DecayTrackedActorMarkerIntensity (wanted mode)
   f. UpdateRaceActors (AI + route + track state)
   g. ResolveVehicleContacts (collision)
   h. UpdateTireTrackPool
   i. UpdateRaceOrder (position bubble sort)
   j. UpdateChaseCamera (per view)
   k. UpdateRaceParticleEffects (view 0, view 1)
   l. UpdateAmbientParticleDensityForSegment (P1, P2)
   m. UpdateRaceCameraTransitionTimer
   n. g_simTickBudget -= 1.0; g_simTimeAccumulator -= 0x10000
   o. NormalizeActorTrackWrapState (all actors)
7. === RENDER PASS (View 0 / Primary) ===
   a. SetProjectedClipRect / SetProjectionCenterOffset
   b. BeginRaceScene
   c. Camera update (chase/trackside/replay)
   d. Sky mesh render
   e. Track span display list render
   f. RenderRaceActorsForView(0)
   g. RenderTireTrackPool
   h. RenderAmbientParticleStreaks
   i. DrawRaceParticleEffects(0)
   j. FlushQueuedTranslucentPrimitives
   k. FlushProjectedPrimitiveBuckets
   l. DrawRaceStatusText (view 0)
8. === RENDER PASS (View 1 / Split-screen, if active) ===
   [mirrors View 0 pipeline for P2]
9. Timing / FPS calculation
10. RenderRaceHudOverlays (HUD: speedo, timers, minimap)
11. Audio options overlay (if open)
12. Sim budget accumulation + benchmark sampling
13. EndRaceScene
14. Vehicle audio mix update
15. NoOpHookStub (lifecycle stub)
16. Frame counter increment
17. Return 0
```

---

## Hook Points: Simulation Loop

### Stage 1: Input Polling — `PollRaceSessionInput`

| Property | Value |
|----------|-------|
| **Callee address** | `0x0042C470` |
| **CALL site (network path)** | `0x0042B8E4` (`CALL 0x0042C470`) |
| **CALL site (local path)** | `0x0042B916` (`CALL 0x0042C470`) |
| **Context** | Called at the start of each sim tick. In network mode, called once before the loop; in local mode, called at the top of each tick iteration. `g_simTimeAccumulator` still holds current tick's time budget. |
| **Available state** | `gPlayerControlBits[0..1]` = stale (previous tick). DXInput device state not yet polled. |
| **Safe to modify (pre-call)** | Inject synthetic input bits into `gPlayerControlBits` before the call (will be overwritten). |
| **Safe to modify (post-call)** | `gPlayerControlBits[0]` (P1) and `gPlayerControlBits[1]` (P2) are freshly filled. Can mask/add bits for input injection, steering assist, etc. Force-feedback ownership is set. |
| **Suggested hook** | **MinHook the callee** (`0x42C470`). In your detour, call the original then modify `gPlayerControlBits`. This is the cleanest input injection point. |
| **Notes** | Replay mode uses `DXInput::Read` path instead of live polling. Check `g_inputPlaybackActive` before modifying. |

### Stage 2: Camera Angle Caching — `CacheVehicleCameraAngles`

| Property | Value |
|----------|-------|
| **Callee address** | `0x00402A80` |
| **CALL sites** | `0x0042B938` (P1, view 0), `0x0042B95A` (P2, view 1) |
| **Context** | Captures the actor's current orientation for camera interpolation. Called for both selected view slots. |
| **Available state** | Input has been polled. Actor positions are from previous tick. |
| **Suggested hook** | Patch CALL site if you need per-view camera overrides. MinHook callee for global camera mods. |

### Stage 3: 2P Steering Balance — `UpdatePlayerSteeringWeightBalance`

| Property | Value |
|----------|-------|
| **Callee address** | `0x004036B0` |
| **CALL site** | `0x0042B96B` |
| **Context** | Only called when `DAT_004962a0 != 0` (2-player mode). Adjusts trailing player's steering weight as a rubber-band. |
| **Safe to modify** | Override or scale the rubber-band strength after this call. The steering weight is written to actor state. |
| **Suggested hook** | **MinHook callee** to intercept/modify rubber-band parameters. |

### Stage 4: Per-Actor Vehicle Control — `UpdatePlayerVehicleControlState`

| Property | Value |
|----------|-------|
| **Callee address** | `0x00402E60` |
| **CALL site** | `0x0042B97D` (in loop: `PUSH EDI` / `CALL 0x402E60`) |
| **Context** | Called for each slot where `gRaceSlotStateTable.slot[i].state == 0x01` (active player). Decodes input bitmask into steering/throttle/brake/gear commands. |
| **Available state** | `gPlayerControlBits` freshly polled. Camera angles cached. |
| **Safe to modify (pre-call)** | Modify `gPlayerControlBits[slot]` to inject/override inputs. |
| **Safe to modify (post-call)** | Actor steering command (offset +0x33C), throttle, brake are set. Can scale/clamp after. |
| **Suggested hook** | **MinHook callee** for per-player input transformation (steering assist, throttle limiter, etc.). |

### Stage 5: Wanted Mode Decay — `DecayTrackedActorMarkerIntensity`

| Property | Value |
|----------|-------|
| **Callee address** | `0x0043D7E0` |
| **CALL site** | `0x0042B99A` |
| **Context** | Only called when `gWantedModeEnabled != 0`. Decays the cop-chase marker intensity. |
| **Suggested hook** | MinHook callee to modify cop chase behavior. Low priority for most mods. |

### Stage 6: Actor Update Dispatcher — `UpdateRaceActors`

| Property | Value |
|----------|-------|
| **Callee address** | `0x00436A70` |
| **CALL site** | `0x0042B99F` (`CALL 0x00436A70`) |
| **Context** | **Core simulation dispatcher.** Internally calls: `ComputeAIRubberBandThrottle`, route state updates, `UpdateActorTrackPosition` (cross-product boundary tests), `UpdateActorRouteThresholdState` (AI throttle/brake), traffic actor updates, encounter system. All actor positions, track state, and AI decisions are computed here. |
| **Available state** | Player inputs decoded. All actors at previous-tick positions. |
| **Safe to modify (pre-call)** | Override AI parameters (rubber-band globals, route tables). |
| **Safe to modify (post-call)** | All actor track positions updated. AI steering/throttle committed. Can override actor velocities, positions, AI decisions. Traffic actors spawned/despawned. |
| **Suggested hook** | **MinHook callee** for global AI/traffic mods. For finer control, hook individual sub-functions: `ComputeAIRubberBandThrottle` (0x432D60), `UpdateActorRouteThresholdState` (0x434AA0), `UpdateSpecialTrafficEncounter` (0x434DA0). |
| **Notes** | This is the single most important hook for gameplay modification. |

### Stage 7: Collision Resolution — `ResolveVehicleContacts`

| Property | Value |
|----------|-------|
| **Callee address** | `0x00409150` |
| **CALL site** | `0x0042B9A4` (`CALL 0x00409150`) |
| **Context** | Broadphase AABB bucketing + narrowphase OBB collision for all active vehicles. Calls `ResolveVehicleCollisionPair` (0x408A60) for each overlapping pair, which in turn calls `ApplyVehicleCollisionImpulse` (0x4079C0). |
| **Available state** | All actor positions/velocities are current-tick values (post-UpdateRaceActors). |
| **Safe to modify (pre-call)** | Set actor invincibility flags (`gap_0376[3]` or `gap_0376[6]` >= 0x0F` skips collision). |
| **Safe to modify (post-call)** | Collision impulses have been applied to velocities. Can dampen/amplify. Damage values written. |
| **Suggested hook** | **MinHook callee** to disable collisions entirely (return immediately). For per-pair control, hook `ApplyVehicleCollisionImpulse` (0x4079C0) to scale impulse magnitude. |

### Stage 8: Tire Track Pool — `UpdateTireTrackPool`

| Property | Value |
|----------|-------|
| **Callee address** | `0x0043EB50` |
| **CALL site** | `0x0042B9A9` |
| **Context** | Updates skid mark / tire track visual pool. Cosmetic only. |
| **Suggested hook** | MinHook callee to disable tire marks or modify appearance. Low priority. |

### Stage 9: Race Position Tracking — `UpdateRaceOrder`

| Property | Value |
|----------|-------|
| **Callee address** | `0x0042F5B0` |
| **CALL site** | `0x0042B9AE` (`CALL 0x0042F5B0`) |
| **Context** | Bubble-sorts the race order array (`DAT_004AE278`, 6 bytes) by normalized span progress. Each actor's display position is written to `gap_0376[0xD]`. Time Trial mode uses finish-time comparison instead. |
| **Available state** | All positions, collisions, and track state are resolved. |
| **Safe to modify (post-call)** | Override `gap_0376[0xD]` per actor to force display positions. Override `DAT_004AE278` order array for custom ranking. |
| **Suggested hook** | **MinHook callee** for custom position/ranking systems. Call original, then override positions. |

### Stage 10: Chase Camera — `UpdateChaseCamera`

| Property | Value |
|----------|-------|
| **Callee address** | `0x00401590` |
| **CALL site** | `0x0042B9F4` (in loop, per view) |
| **Context** | Called per active view (1 in single-player, 2 in split-screen) when camera preset is chase mode. 7 presets. Only called if `gRaceCameraPresetMode == 0`. |
| **Suggested hook** | MinHook callee for custom camera behavior. Args: (actor_ptr, 1, view_index). |

### Stage 11: Particle Effects — `UpdateRaceParticleEffects`

| Property | Value |
|----------|-------|
| **Callee address** | `0x00429790` |
| **CALL sites** | `0x0042BA13` (view 0), `0x0042BA1A` (view 1) |
| **Context** | Updates the 100-slot particle pool per view. Smoke, sparks, debris. |
| **Suggested hook** | MinHook callee for custom particle effects or to disable particles. |

### Stage 12: Track Wrap Normalization — `NormalizeActorTrackWrapState`

| Property | Value |
|----------|-------|
| **Callee address** | `0x00443FB0` |
| **CALL site** | `0x0042BACF` (in loop, per non-disabled actor) |
| **Context** | Last operation in the sim tick. Normalizes span position modulo ring length for circuit tracks. Computes raw lap count. |
| **Suggested hook** | MinHook callee to intercept lap counting. |

---

## Hook Points: Pre-Render / Render

### Stage 13: Race Completion Check — `CheckRaceCompletionState`

| Property | Value |
|----------|-------|
| **Callee address** | `0x00409E80` |
| **CALL site** | `0x0042B639` (`CALL 0x00409E80`) |
| **Context** | Called BEFORE the sim loop, once per frame. Checks if race should end. Two-phase: Phase 1 checks per-actor finish conditions (sector bitmask for circuits, checkpoint span for P2P). Phase 2 is a 0x3FFFFF-tick cooldown before calling `BuildRaceResultsTable` and returning 1. |
| **Available state** | `g_simTimeAccumulator` holds the time delta for this frame. Actor finish states from previous tick. |
| **Safe to modify (pre-call)** | Set `DAT_00483980` to prevent/force race end. Modify actor checkpoint indices. |
| **Safe to modify (post-call)** | Return value: 0 = continue, 1 = begin fade-out. Can intercept to prevent race end (force return 0). |
| **Suggested hook** | **MinHook callee** for custom end conditions. Wrap the original: call it, inspect result, override return value. Essential for custom game modes (survival, speed trap, etc.). |

### Stage 14: Begin Scene — `BeginRaceScene`

| Property | Value |
|----------|-------|
| **Callee address** | `0x0040ADE0` |
| **CALL site** | `0x0042BCB2` |
| **Context** | Opens the D3D render scene (IDirect3DDevice3::BeginScene). All rendering happens between this and EndRaceScene. |
| **Suggested hook** | MinHook callee for pre-render state injection (custom render states, shader setup). |

### Stage 15: Render Actors — `RenderRaceActorsForView`

| Property | Value |
|----------|-------|
| **Callee address** | `0x0040BD20` |
| **CALL sites** | `0x0042BE6D` (view 0), `0x0042C0BA` (view 1) |
| **Context** | Renders up to 6 racer actors, then traffic actors within cull window. Submits vehicle meshes, overlays, smoke, wheel billboards, tire track emitters. |
| **Available state** | View matrices set. Track spans rendered. |
| **Safe to modify (pre-call)** | Modify actor mesh pointers for model swaps. |
| **Safe to modify (post-call)** | Actor rendering complete. Can submit additional primitives. |
| **Suggested hook** | **MinHook callee** for custom vehicle rendering, visibility toggling, LOD overrides. |

### Stage 16: Translucent Flush — `FlushQueuedTranslucentPrimitives`

| Property | Value |
|----------|-------|
| **Callee address** | `0x00431340` |
| **CALL sites** | `0x0042BEA5` (view 0), `0x0042C0F2` (view 1) |
| **Context** | Linked-list bucket sort of translucent primitives (max 510 batches). This is where all transparent geometry is submitted to D3D. |
| **Suggested hook** | MinHook callee to inject custom translucent geometry before flush. |

### Stage 17: HUD Status Text — `DrawRaceStatusText`

| Property | Value |
|----------|-------|
| **Callee address** | `0x00439B70` |
| **CALL sites** | `0x0042BED8` (view 0), `0x0042C125` (view 1) |
| **Context** | Renders per-view text: position indicator, lap timer, wanted messages. Called per view. |
| **Suggested hook** | MinHook callee for custom HUD text overlays or to suppress default text. |

### Stage 18: HUD Overlay — `RenderRaceHudOverlays`

| Property | Value |
|----------|-------|
| **Callee address** | `0x004388A0` |
| **CALL site** | `0x0042C1C6` (`CALL 0x004388A0`) |
| **Context** | Main HUD render: speedometer, gear indicator, minimap, timers, U-turn warning. Called once after both views are rendered. Receives `g_simTickBudgetSnapshot` for animation timing. |
| **Available state** | All 3D rendering complete. Projection reset to fullscreen. |
| **Safe to modify (post-call)** | Can overlay custom HUD elements after this. |
| **Suggested hook** | **MinHook callee** for custom HUD rendering. Call original then draw custom elements. |

### Stage 19: End Scene — `EndRaceScene`

| Property | Value |
|----------|-------|
| **Callee address** | `0x0040AE00` |
| **CALL site** | `0x0042C254` |
| **Context** | Closes D3D render scene. After this, no more D3D draw calls until next frame. |
| **Suggested hook** | MinHook callee for post-render cleanup or screenshot capture. |

### Stage 20: Vehicle Audio — `UpdateVehicleAudioMix`

| Property | Value |
|----------|-------|
| **Callee address** | `0x00440B00` |
| **CALL site** | `0x0042C27D` |
| **Context** | Updates engine sound mix based on RPM, speed, gear for all vehicles with horn held. |
| **Suggested hook** | MinHook callee for custom audio mixing. |

### Stage 21: Lifecycle Stub — `NoOpHookStub`

| Property | Value |
|----------|-------|
| **Callee address** | `0x00418450` |
| **CALL site** | `0x0042C290` |
| **Context** | Empty stub function called at 9 lifecycle points across the game. Ideal injection point for per-frame mod logic that doesn't fit elsewhere. |
| **Suggested hook** | **MinHook callee** (0x418450). This is the recommended hook for general per-frame mod tick logic. Already documented in td5_sdk.h. |

---

## Hook Points: Fade-Out & Teardown

### Fade-Out Progression

When `CheckRaceCompletionState` returns 1, `g_raceEndFadeState` is set to 1. Each subsequent frame:
- `g_raceEndFadeState` accumulates time-scaled increments
- A normalized 0-255 value is computed; `SetClipBounds` narrows the viewport (horizontal or vertical wipe)
- At 255.0 (full fade): teardown sequence begins

### Resource Teardown (at fade == 255.0)

The following calls execute in sequence at `0x0042B838` through `0x0042B8CC`:

| Address | Call | Purpose |
|---------|------|---------|
| `0x0042B838` | `ReleaseRaceSoundChannels` (0x441D50) | Free all race audio channels |
| `0x0042B83D` | `DXInput::StopEffects` (IAT 0x45D440) | Stop force-feedback effects |
| `0x0042B843` | `DXInput::ResetConfiguration` (IAT 0x45D444) | Reset input device config |
| `0x0042B849` | `ReleaseRaceRenderResources` (0x40AEC0) | Free textures, meshes, surfaces |
| `0x0042B856` | `DXInput::ReadClose` (IAT 0x45D448) | Close replay playback file |
| `0x0042B85E` | `DXInput::WriteClose` (IAT 0x45D44C) | Close input recording file |
| `0x0042B86C` | `WriteBenchmarkResultsTgaReport` (0x428D80) | Benchmark TGA output (if active) |

After teardown, the function returns 1. The game state dispatcher transitions to `g_gameState = 1` (frontend).

---

## ConfigureGameTypeFlags (0x410CA0) — Game Type Switch

This function maps `g_selectedGameType` (1-9) to runtime flags. Key outputs:

| Global | Address | Description |
|--------|---------|-------------|
| `gRaceRuleVariant` | 0x4AAF74 prefix | Scoring rule (0=Championship, 1-3=time, 4=Ultimate, 5=Era) |
| `gRaceDifficultyTier` | varies | 0=Easy, 1=Medium, 2=Hard |
| `gTrafficActorsEnabled` | 0x4B0FA4 | 1=traffic cars active |
| `gSpecialEncounterEnabled` | 0x4AAF70 | 1=encounter actor active |
| `gCircuitLapCount` | 0x4AAF04 | Lap count for circuit tracks |
| `gTimeTrialModeEnabled` | 0x4AAF6C vicinity | Time trial flag |
| `gWantedModeEnabled` | 0x4AAF68 | Cop chase mode |
| `gDragRaceModeEnabled` | 0x4AAF48 | Drag race mode |
| `DAT_004AAF74` | 0x4AAF74 | Race session type (1=drag, 2=cup, 3=TT, 4=wanted, 5=2P) |
| `DAT_00490BA8` | 0x490BA8 | NPC group index (99 = cup series finished) |

**Return value:** 1 = race should proceed, 0 = cup series is complete (group index == 99).

### Type-to-Flag Mapping

| Type | Name | Variant | Difficulty | Traffic | Encounters | Laps | Session |
|------|------|---------|------------|---------|------------|------|---------|
| 1 | Championship | 0 | Easy (0) | Yes | Yes | Options | 2 (cup) |
| 2 | Era Cup | 5 | Easy (0) | Yes | Yes | 4 (hardcoded) | 2 (cup) |
| 3 | Challenge | 1 | Medium (1) | Yes | Yes | Options | 2 (cup) |
| 4 | Pitbull | 2 | Hard (1+) | Yes | Yes | Options | 2 (cup) |
| 5 | Masters | 3 | Medium (1) | Yes | Yes | Options | 2 (cup) |
| 6 | Ultimate | 4 | Hard (2) | Yes | Yes | Options | 2 (cup) |
| 7 | Time Trial | N/A | Hard (2) | No | No | N/A | 3 (TT) |
| 8 | Cop Chase | N/A | N/A | Yes | No | N/A | 4 (wanted) |
| 9 | Drag Race | N/A | N/A | No | No | N/A | 1 (drag) |

---

## Recommended Hook Strategy Summary

For a comprehensive race-frame mod, use these five hooks:

1. **`PollRaceSessionInput` (0x42C470)** — Input injection/override
2. **`UpdateRaceActors` (0x436A70)** — AI behavior, traffic, encounters
3. **`ResolveVehicleContacts` (0x409150)** — Collision scaling/disabling
4. **`CheckRaceCompletionState` (0x409E80)** — Custom end conditions
5. **`NoOpHookStub` (0x418450)** — General per-frame mod tick

For game-mode extensions, add:
6. **`ConfigureGameTypeFlags` (0x410CA0)** — Intercept game type -> flag mapping
7. **`UpdateRaceOrder` (0x42F5B0)** — Custom position/scoring

All hooks should use **MinHook** on the callee address. Patching CALL sites is only needed when you want to intercept one specific call to a function that's called from multiple places.
