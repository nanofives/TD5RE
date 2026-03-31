# Special Encounter & Actor Track Scripting System

> Deep-dive reverse engineering of TD5's scripted traffic encounter and actor track behavior systems.

## Overview

TD5 has a **special encounter system** that spawns a scripted NPC vehicle (actor slot 9) during normal races. This is separate from both the regular traffic system (slots 6-11) and the police/wanted mode (game type 8). The encounter actor uses a small bytecode interpreter (`AdvanceActorTrackScript`) to execute lane-change and steering maneuvers, creating dynamic obstacles.

---

## 1. Game Type Configuration (`ConfigureGameTypeFlags` @ 0x410CA0)

The encounter system is gated by `gSpecialEncounterEnabled` (a global flag), set per game type:

| Game Type | `gSpecialEncounterEnabled` | `gTrafficActorsEnabled` | `gWantedModeEnabled` | Notes |
|-----------|---------------------------|------------------------|---------------------|-------|
| 1 (Single Race) | **1** | 1 | 0 | Encounters active |
| 2 (Circuit) | **1** | 1 | 0 | 4-lap circuit |
| 3 (Point-to-Point) | **1** | 1 | 0 | |
| 4 | **1** | 1 | 0 | |
| 5 (Cup Sprint) | **1** | 1 | 0 | |
| 6 (Cup Advanced) | **1** | 1 | 0 | |
| 7 (Time Trial) | **0** | 0 | 0 | No traffic, no encounters |
| 8 (Cops/Wanted) | **0** | 1 | 1 | Wanted mode replaces encounters |
| 9 (Drag Race) | N/A | 0 | 0 | Drag mode, no encounter logic |

Key insight: **encounters and wanted mode are mutually exclusive**. Game type 8 (cops) disables the encounter system entirely and uses a different mechanism (`gWantedModeEnabled`).

---

## 2. Encounter Trigger Logic (`UpdateSpecialTrafficEncounter` @ 0x434DA0)

Called from `UpdateRaceActors` at the end of the traffic update loop, only when `gSpecialEncounterEnabled == 1`.

### Spawn Conditions (all must be true simultaneously):

1. **`gSpecialEncounterTrackedActorHandle == -1`** (0xFFFFFFFF) -- no encounter currently active
2. **`DAT_004b064c == 0`** -- cooldown timer has expired (counts down from 300 frames = ~5 seconds at 60fps)
3. **Span proximity**: Player's current span minus encounter reference span == 2 (player is exactly 2 spans ahead)
4. **Speed threshold**: Player speed (`slot[0].gap_0310._4_4_`) >= 0x15639 (~87,577 in fixed-point = significant forward velocity)
5. **Forward track component > 0**: Player must be moving forward along the track
6. **Route table match**: `gActorRouteTableSelector == DAT_004b0568` (player must be on the correct route/lane for encounter)
7. **Span distance check**: Absolute span delta between two reference values < 0x10 (16 spans)

### Spawn Sequence:
1. `gSpecialEncounterTrackedActorHandle` is set to the target actor index (typically 0, meaning encounter targets player 0)
2. Track progress and offset are computed via `ComputeTrackSpanProgress` and `ComputeSignedTrackOffset`
3. `gSpecialEncounterRouteTable` is synced to the target's route state
4. `ResetVehicleActorState` is called on actor slot 9's data (`DAT_004ad0d0`)
5. If wanted mode is not active, `StartTrackedVehicleAudio(9)` starts the encounter vehicle's engine sound

### Despawn Conditions:
- **Span distance > 0x40** (64 spans): Encounter actor has fallen too far behind
- AND `DAT_004ad449 == '\0'` (no lock flag)

### Despawn Sequence:
1. `_DAT_004b0658 = 0`
2. `gSpecialEncounterTrackedActorHandle = 0xFFFFFFFF` (invalid handle)
3. `DAT_004b05e4 = 0`
4. `DAT_004b064c = 300` (reset cooldown timer -- 300 frames before next encounter can spawn)
5. `StopTrackedVehicleAudio()` silences the encounter vehicle
6. `ResetVehicleActorState` clears actor slot 9

### Encounter Activation:
When an encounter actor is within 1 span of the tracked player AND on the matching route AND `DAT_004ad449 == '\0'`:
- `gActorSpecialEncounterActive[handle]` is set to 1
- If wanted mode is off, `StopTrackedVehicleAudio()` is called (encounter transitions from "approaching" to "active interaction")

---

## 3. Encounter Control (`UpdateSpecialEncounterControl` @ 0x434BA0)

Called from `UpdatePlayerVehicleControlState` (0x402E60) when `gActorSpecialEncounterActive[player] != 0`. This overrides the player's normal control state processing during an active encounter.

### Parameters:
- `param_1`: Player slot index (0 or 1)

### Logic:
1. Computes heading delta between the encounter actor and its route reference (12-bit angle system, 0x1000 = 360 degrees)
2. Computes heading delta for the tracked encounter target (`DAT_004b0630` slot)
3. Sets `gSpecialEncounterControlActiveLatch = 1` and `gSpecialEncounterSteerBiasLatch = 0xFF00`

### Alignment Check (encounter stays active if ALL true):
- Forward track component > 8 (actor moving forward at minimum speed)
- Both heading deltas are within tolerance (< 0x400 or > 0xC00, i.e., within ~90 degrees of route direction)

### Steering Override:
If the encounter is active and `gSpecialEncounterMinForwardTrackComponentThreshold` is reached AND span delta < 3:
- `encounter_steering_override = -0x100` (hard brake)
- `gap_0346[0x27] = 1` (brake flag)

Otherwise: `encounter_steering_override = 0` (no override)

### Teardown (alignment/spacing failed):
When the encounter deactivates:
- `gActorSpecialEncounterActive[player] = 0`
- `gActorRouteDirectionPolarity[player] = 0`
- `gSpecialEncounterSteerBiasLatch = 0`
- `gSpecialEncounterControlActiveLatch = 0`
- `gSpecialEncounterTrackedActorHandle = 0xFFFFFFFF`
- `DAT_004b064c = 300` (cooldown reset)
- `special_encounter_state` incremented (encounter completion counter)

---

## 4. Actor Track Scripting Engine (`AdvanceActorTrackScript` @ 0x4370A0)

A bytecode interpreter that drives AI actor lane-change and steering behaviors. Called from `UpdateActorTrackBehavior` for actors that have an active script pointer.

### Script State (per-actor, stored in ActorRouteState array at stride 0x11C):
| Offset | Field | Purpose |
|--------|-------|---------|
| `[0x35]` | Slot index | Which RuntimeSlotActor this script controls |
| `[0x3A]` | Script base pointer | Start of current script program |
| `[0x3B]` | Script instruction pointer | Current position in script |
| `[0x3C]` | Speed parameter | Set by opcode 1 |
| `[0x3D]` | Flag bitfield | Accumulated by opcodes 3-6, 8, 10, 11 |
| `[0x3E]` | (cleared on reset) | |
| `[0x43]` | (cleared on reset) | |
| `[0x45]` | Countdown timer | Decremented each frame; triggers script cycling at < 0 |

### Script Cycling:
When the countdown timer (`[0x45]`) reaches -1, the script cycles through 4 program banks in round-robin:
- `DWORD_00473cd4` -> `DAT_00473cec` -> `DAT_00473d00` -> `DAT_00473d18` -> back to start
Timer resets to 0x96 (150 frames = ~2.5 seconds) each cycle.

### Script Programs (hardcoded in .rdata):

**Program A** @ 0x473CD4 (24 bytes):
```
08 00 00 00  02 00 00 00  E0 FF FF FF  05 00 00 00  08 00 00 00  00 00 00 00
opcode=8     opcode=2, arg=-32 (0xFFFFFFE0)  opcode=5     opcode=8     opcode=0 (end)
```

**Program B** @ 0x473CEC (20 bytes):
```
08 00 00 00  02 00 00 00  40 00 00 00  06 00 00 00  00 00 00 00
opcode=8     opcode=2, arg=+64 (0x40)        opcode=6     opcode=0 (end)
```

**Program C** @ 0x473D00 (24 bytes):
```
08 00 00 00  02 00 00 00  E0 FF FF FF  06 00 00 00  08 00 00 00  00 00 00 00
opcode=8     opcode=2, arg=-32              opcode=6     opcode=8     opcode=0 (end)
```

**Program D** @ 0x473D18 (20 bytes):
```
08 00 00 00  02 00 00 00  40 00 00 00  05 00 00 00  00 00 00 00
opcode=8     opcode=2, arg=+64             opcode=5     opcode=0 (end)
```

### Opcode Table (12 opcodes, switch table at 0x437780):

| Opcode | Args | Action |
|--------|------|--------|
| **0** | none | **Terminate/reset**: Clears script state, steering, resets track span. Returns 1 (script complete). |
| **1** | speed_param | **Set speed**: `[0x3C] = arg`. Sets flag bit 0x01. |
| **2** | signed_offset | **Set lateral offset target**: Sets flag bit 0x02. Stores signed value in `[0x1B]`. Actor will steer toward this lane offset. |
| **3** | flag_bits | **Set flags**: `[0x3D] |= arg` |
| **4** | flag_bits | **Clear flags**: `[0x3D] &= ~arg` |
| **5** | none | **Steer left**: Sets flag bit 0x04. Enables leftward heading-tracking steering. |
| **6** | none | **Steer right**: Sets flag bit 0x08. Enables rightward heading-tracking steering. |
| **7** | none | **Force brake**: Sets `encounter_steering_override = -0x100`, brake flag `gap_0346[0x27] = 1`. Advances IP by 4 bytes. |
| **8** | none | **Stop and wait**: Sets flag bit 0x10. Vehicle decelerates to near-zero before proceeding. |
| **9** | none | **Auto-select program**: Evaluates heading vs route angle and track surface data to choose one of the 4 programs (A-D). Resets timer to 0xFA (250 frames). Uses road width data from track geometry. |
| **10** | none | **Set flag 0x40**: Latent/annotation flag (no known consumer). |
| **11** | none | **Set flag 0x80**: Latent/annotation flag (no known consumer). |

### Steering Execution:
- **Flag 0x04 (steer left)**: Computes heading delta to route direction. If within threshold (< 0x201), clears flag and optionally zeros steering. Otherwise, applies `steering_command += 0x4000` per frame, clamped to 0x19000 max.
- **Flag 0x08 (steer right)**: Mirror of left. Applies `steering_command -= 0x4000`, clamped to -0x19000.
- **Flag 0x10 (stop)**: Checks if forward speed is near zero (abs < 0x100). If so, clears flag and steering override. Otherwise maintains brake.
- **Flag 0x02 (lateral offset)**: Applies `encounter_steering_override` of -0x100 or +0xFF depending on sign of `[0x1B]`, compared against current forward speed.

### Initial Script Selection (Opcode 9):
Uses track geometry data (`DAT_004c3d9c + span*0x18 + 3`) to determine road width/lane count. Compares heading delta ranges against lane availability:
- Heading 0x900-0xF00 + lane fits -> Program A (steer left, stop, steer left)
- Heading > 0x6FF + lane exceeds actor width -> Program B (steer right)
- Heading 0x100-0x700 + lane fits -> Program C (steer right, stop, steer right)
- Default -> Program D (steer right back)

---

## 5. Actor Track Behavior (`UpdateActorTrackBehavior` @ 0x434FE0)

The high-level AI steering dispatcher for non-player actors. Called from `UpdateRaceActors` for:
- AI racers (slots 0-5) in non-wanted mode
- The encounter actor (slot 9) when `gSpecialEncounterTrackedActorHandle != -1`

### Flow:
1. **Time trial check**: If `gTimeTrialModeEnabled`, skip entirely (no AI track behavior in time trial)
2. **Script check** (`DAT_004afc48 + slot*0x11C`): If a script pointer is set, call `AdvanceActorTrackScript`. If it returns 0 (still running), compute track progress/offset and return.
3. **Heading misalignment trigger**: If no script and heading delta is between 0x320 and 0xCE0 (significant misalignment from route), assign initial script program (`DAT_00473cc8` = opcode 8 + opcode 9 + terminate).
4. **Normal AI path**: If no script active, runs the standard AI route-following:
   - `UpdateActorTrackOffsetBias` -- lateral offset targeting
   - `SampleTrackTargetPoint` -- look-ahead waypoint
   - `ComputeRouteForwardOvershootScalar` -- overshoot correction
   - Angle-from-vector steering calculation (4-quadrant atan2)
   - `UpdateActorRouteThresholdState` -- throttle/brake decision
   - `UpdateActorSteeringBias` -- final steering weight (0x10000 if threshold active, 0x20000 otherwise)

### Script Activation by Heading:
The initial script at `0x473CC8` is `[8, 9, 0]`:
- Opcode 8: Stop the vehicle (decelerate to zero)
- Opcode 9: Auto-select a lane-change program based on current geometry
- Opcode 0: End script

This means actors auto-correct when they become significantly misaligned with the track -- they stop, pick a recovery maneuver, execute it, then resume normal AI.

---

## 6. Scripted Vehicle Motion Pipeline

### Vehicle Update Dispatch (`UpdateVehicleActor` @ 0x406780)

The `gap_0376[3]` byte selects the vehicle's motion mode:

| Value | Mode | Pipeline |
|-------|------|----------|
| `0x00` | **Normal physics** | `UpdatePlayerVehicleDynamics` or `UpdateAIVehicleDynamics` -> `IntegrateVehiclePoseAndContacts` -> track contact callbacks |
| `0x01` | **Scripted/repositioning** | `RefreshScriptedVehicleTransforms` -> `IntegrateScriptedVehicleMotion` -> track contact callbacks (different callback table at 0x46738C) |

### `RefreshScriptedVehicleTransforms` (0x409BF0)
- Extracts Euler angles from the cached rotation matrix at actor+0x120
- Copies and re-extracts from the secondary matrix at actor+0x180
- Every 4th frame (`DAT_004aac5c & 3 == 0`): rebuilds rotation matrices from extracted angles (prevents drift accumulation)
- Copies rebuilt matrices to interpolation slots at actor+0x150 and actor+0x180

### `IntegrateScriptedVehicleMotion` (0x409D20)
- **Velocity damping**: Each axis velocity is reduced by `velocity - (velocity >> 8)` per frame (~0.4% damping per tick)
- **Gravity**: Y-axis velocity has `gGravityConstant` subtracted each frame
- **Position integration**: Position += velocity (simple Euler)
- **Rotation**: `MultiplyRotationMatrices3x3` applies incremental rotation from actor+0xC0 basis to actor+0xA8
- **Track position update**: `UpdateActorTrackPosition` keeps the actor snapped to the track span system
- **Surface contact**: `ComputeActorTrackContactNormalExtended` for ground following
- **World position**: Converts fixed-point position to float for rendering
- **Model render**: `RenderVehicleActorModel` + `ComputeActorWorldBoundingVolume` (collision)
- **Timeout**: If `param_1[0x19C] > 59` (0x3B), calls `ResetVehicleActorState` -- scripted motion auto-terminates after ~1 second

### `ComputeActorWorldBoundingVolume` (0x4096B0)
- Iterates 8 corner points (OBB vertices from DAT_00483958)
- Computes centroid offset from actor center of mass
- Finds maximum ground penetration depth (`local_30`)
- If ground contact exists (penetration > 0): applies rotation correction to align vehicle with terrain normal
- Velocity correction: pushes vehicle out of ground along surface normal
- Calls `CheckAndUpdateActorCollisionAlignment` for collision-based resets

### `CheckAndUpdateActorCollisionAlignment` (0x409520)
- Only active when `param_1[0x19C] > 2` (scripted mode counter > 2 frames)
- Computes orientation deltas (heading, pitch) from collision normal
- If both deltas are within tolerance (< 0x18 = ~8.4 degrees): checks angular velocity
- If angular velocity is also small (< 0x20) AND orientation is in a "safe" quadrant (0 or 10): calls `ResetVehicleActorState` to transition back to normal physics mode

---

## 7. Actor Slot Architecture

### Slot Allocation:
| Slots | Purpose | Update Path |
|-------|---------|------------|
| 0-5 | **Racers** (player + AI) | `UpdateActorTrackBehavior` -> `UpdateVehicleActor` |
| 6-8, 10-11 | **Traffic** | `UpdateTrafficRoutePlan` -> `UpdateTrafficActorMotion` |
| **9** | **Special encounter** (when active) | `UpdateActorTrackBehavior` -> `UpdateVehicleActor` (same as racers!) |

### Slot 9 Dual-Purpose:
In `UpdateRaceActors`, the traffic loop (slots 6-11) has a special check:
```
if (slot == 9 && gSpecialEncounterTrackedActorHandle != -1) {
    UpdateActorTrackBehavior(9);  // AI racer-style update
    UpdateVehicleActor(9);
    skip to slot 10;
} else {
    UpdateTrafficRoutePlan(slot);  // Normal traffic update
    UpdateTrafficActorMotion(slot);
}
```

This means slot 9 is **hijacked from the traffic pool** when an encounter is active. It receives the full racer AI treatment (track behavior scripting, physics-based vehicle update) instead of the simplified traffic motion model.

### Track Offset Computation:
In the track-state loop of `UpdateRaceActors`, slot 9 also gets special treatment:
- Racers (< 6) and encounter slot 9 (when tracked): use direct `gActorTrackSpanProgress` for offset computation
- Traffic (6-11, excluding active encounter): recompute offset from alternate route table

---

## 8. Encounter-Related Global Variables

| Address | Name | Purpose |
|---------|------|---------|
| `0x4B0630` | `DAT_004b0630` | Encounter reference slot index for heading comparison |
| `0x4B064C` | `DAT_004b064c` | **Cooldown timer**: Counts down from 300; encounter cannot spawn until 0 |
| `0x4B05E4` | `DAT_004b05e4` | Encounter phase flag (0 = acquisition phase, nonzero = tracking phase) |
| `0x4B0658` | `_DAT_004b0658` | Cleared on despawn |
| `0x4B05C4` | `_DAT_004b05c4` | Cleared on encounter teardown |
| `0x4B0568` | `DAT_004b0568` | Required route table selector for encounter spawn |
| `gSpecialEncounterEnabled` | | Master gate: 1 = encounters can happen |
| `gSpecialEncounterTrackedActorHandle` | | -1 = no encounter; else = target actor index |
| `gSpecialEncounterRouteTable` | | Route state pointer for the encounter actor |
| `gSpecialEncounterControlActiveLatch` | | 1 while encounter is actively overriding player control |
| `gSpecialEncounterSteerBiasLatch` | | 0xFF00 during active encounter, 0 otherwise |
| `gSpecialEncounterMinForwardTrackComponentThreshold` | | Forward speed threshold for braking override |
| `gSpecialEncounterTrackProgress` | | Encounter actor's computed track progress |
| `gSpecialEncounterSignedTrackOffset` | | Encounter actor's lateral offset from centerline |
| `gActorSpecialEncounterActive[slot*0x11C]` | | Per-actor encounter active flag |
| `gActorRouteDirectionPolarity[slot*0x11C]` | | Per-actor route direction flag |
| `gActorForwardTrackComponent[slot*0x47]` | | Per-actor forward velocity projected onto track direction |

---

## 9. Encounter Lifecycle Summary

```
[IDLE]
  |
  | cooldown timer (DAT_004b064c) counts down from 300 to 0
  v
[READY] -- waiting for spawn conditions
  |
  | Player on correct route, 2 spans ahead, speed > threshold,
  | span delta < 16, forward component > 0
  v
[SPAWNED] -- gSpecialEncounterTrackedActorHandle = target
  |          Slot 9 hijacked from traffic pool
  |          ResetVehicleActorState(slot 9)
  |          StartTrackedVehicleAudio(9)
  |
  | Actor 9 approaches player; route tables synced each frame
  v
[APPROACHING] -- span distance monitored
  |
  | Span delta < 1, same route -> gActorSpecialEncounterActive = 1
  v
[ACTIVE] -- UpdateSpecialEncounterControl called per-frame
  |          Player control state overridden
  |          Steering/brake override based on heading alignment
  |
  | Heading misalignment OR spacing too large
  v
[TEARDOWN]
  |  gActorSpecialEncounterActive = 0
  |  gSpecialEncounterTrackedActorHandle = -1
  |  DAT_004b064c = 300 (cooldown restart)
  |  special_encounter_state++ (lifetime counter)
  |  ResetVehicleActorState(slot 9)
  |  Slot 9 returns to traffic pool
  v
[IDLE] -- cycle repeats
```

---

## 10. Connection to Cop/Wanted Mode (Game Type 8)

The encounter system and wanted mode are **completely separate systems** that share some infrastructure:

| Feature | Special Encounter | Wanted/Cops Mode |
|---------|------------------|-----------------|
| Game type gate | Types 1-6 | Type 8 only |
| Enable flag | `gSpecialEncounterEnabled` | `gWantedModeEnabled` |
| Actor slot | Slot 9 (hijacked from traffic) | Racers in slots 0-5 with state=2 |
| AI behavior | Script-driven lane changes | Full brake + pursue (state 0x02 in UpdateRaceActors) |
| Player effect | Steering override, brake forcing | Different control override path |
| Audio | `StartTrackedVehicleAudio`/`StopTrackedVehicleAudio` | Separate system (`DAT_004aaf68`) |
| Track behavior | `UpdateActorTrackBehavior` (AI racer path) | Skipped (`gWantedModeEnabled` check in racer loop) |

In wanted mode, racer slots with `state == 2` get hard-coded pursuit behavior directly in `UpdateRaceActors`:
- If forward track component < 0: `encounter_steering_override = 0` (no override when moving backward)
- Otherwise: brake flag + `encounter_steering_override = -0x100` (constant braking pursuit)

The encounter audio system also checks `gWantedModeEnabled` -- if wanted mode is active, encounter audio is suppressed even if the encounter technically spawns.

---

## 11. Data Files

The encounter system does **not** load external data files for its scripts. All encounter behavior is driven by:

1. **Hardcoded script programs** in .rdata (0x473CC8-0x473D30): 4 programs of 5-6 dwords each
2. **Track geometry** from the loaded level's STRIP.DAT (accessed via `DAT_004c3d9c`): road width/lane data used by opcode 9 for program selection
3. **Route tables** (`gActorRouteStateTable`, `gSpecialEncounterRouteTable`): 3-byte-per-span records from LEFT.TRK/RIGHT.TRK in level ZIPs

The encounter behavior is therefore fully deterministic given the track geometry -- the same track will produce the same encounter patterns (modulo spawn timing based on player speed and position).

---

## 12. Key Function Reference

| Address | Name | Role |
|---------|------|------|
| 0x410CA0 | `ConfigureGameTypeFlags` | Sets `gSpecialEncounterEnabled` per game type |
| 0x434DA0 | `UpdateSpecialTrafficEncounter` | Spawn/despawn logic, cooldown, proximity checks |
| 0x434BA0 | `UpdateSpecialEncounterControl` | Per-frame active encounter: heading checks, steering override |
| 0x4370A0 | `AdvanceActorTrackScript` | Bytecode interpreter (12 opcodes) |
| 0x434FE0 | `UpdateActorTrackBehavior` | AI steering dispatcher, script activation |
| 0x436A70 | `UpdateRaceActors` | Master dispatcher: slot 9 hijack, encounter call |
| 0x406780 | `UpdateVehicleActor` | gap_0376[3] dispatch: physics vs scripted |
| 0x409BF0 | `RefreshScriptedVehicleTransforms` | Matrix refresh for scripted vehicles |
| 0x409D20 | `IntegrateScriptedVehicleMotion` | Damped Euler integration + ground contact |
| 0x4096B0 | `ComputeActorWorldBoundingVolume` | OBB ground collision + terrain alignment |
| 0x409520 | `CheckAndUpdateActorCollisionAlignment` | Collision-based return-to-physics check |
| 0x402E60 | `UpdatePlayerVehicleControlState` | Calls `UpdateSpecialEncounterControl` when encounter active |
