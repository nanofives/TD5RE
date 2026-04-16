# Traffic AI System -- Complete Analysis

> Full lifecycle of ambient traffic vehicles: spawn, drive, collision, recovery, despawn.
> Session: 2026-03-20

---

## 1. Overview

Test Drive 5 supports up to **6 ambient traffic actors** occupying actor slots **6--11** in the `gRuntimeSlotActorTable` (0x388 bytes/slot, 12 total slots). Traffic uses a **simplified physics model** distinct from both the player and AI racers, with its own friction integrator, pose update, and route planner.

### Enable/Disable Logic

Traffic is controlled by `gTrafficActorsEnabled` (checked in `InitializeRaceActorRuntime` at 0x432E60):

```c
// InitializeRaceActorRuntime (0x432E60)
DAT_004aaf00 = (-(uint)(gTrafficActorsEnabled != 0) & 6) + 6;
//  gTrafficActorsEnabled=0 -> DAT_004aaf00 = 6  (6 racers, 0 traffic)
//  gTrafficActorsEnabled=1 -> DAT_004aaf00 = 12 (6 racers, 6 traffic)
if (gTimeTrialModeEnabled != 0) DAT_004aaf00 = 2; // time trial: 2 only
```

Traffic is **disabled** in: Time Trial, Benchmark, 2-Player, Network modes.
Traffic is **enabled** in: Championship, Era, Challenge, Pitbull, Masters, Ultimate, Cop Chase, and Drag Race modes (when the frontend "Traffic" option is ON).

---

## 2. Data Source: TRAFFIC.BUS

### Loading

Loaded by `LoadTrackRuntimeData` (0x42F140) from the per-track `level%03d.zip` archive:

- Forward direction: `TRAFFIC.BUS` (string at 0x473AF4)
- Reverse direction: `TRAFFICB.BUS` (string at 0x473AC0)

Selected via `gReverseTrackDirection` index into file-name pointer table at 0x4673C0.

The raw data is stored at `DAT_004aed8c` and the current queue pointer is `DAT_004b08b8`.

### Record Format

Each record is **4 bytes**:

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| +0 | int16 | span_index | Track strip span where traffic spawns (-1 = end sentinel) |
| +2 | byte | flags | Bit 0: direction polarity (0=same as player, 1=oncoming) |
| +3 | byte | lane | Lane index within the span's lane count |

Records are sorted by span_index in ascending order. The queue is consumed linearly; `DAT_004b08b8` advances by 4 bytes (2 shorts) per consumed record.

---

## 3. Spawn: Initial Population

### `InitializeTrafficActorsFromQueue` (0x435940)

Called at the end of `InitializeRaceActorRuntime`. Fills slots 6--11 from the head of the traffic queue.

For each slot:
1. **Direction polarity** is set from `flags & 1` -> `gActorRouteDirectionPolarity[slot]`
2. **Route table** selection:
   - If `lane < (stripRecord.laneCount & 0xF)`: use primary route table (LEFT.TRK), `gActorRouteTableSelector = 0`
   - Otherwise: use alternate route table (RIGHT.TRK), `gActorRouteTableSelector = 1`, with lane remapped through the route junction table at `DAT_004c3da0`
3. **Track placement**: `InitActorTrackSegmentPlacement` positions the actor on the specified span and lane
4. **Heading** computed from the track span geometry vectors using `AngleFromVector12Full`, then rotated 180 degrees (`+0x80000`) if oncoming (flags bit 0 set)
5. **State reset**: `ResetVehicleActorState` zeroes velocities, angular state, and sets default suspension
6. **Track progress** computed and normalized

### Physics Parameter Block

Traffic actors get the physics block at `0x473BE8` (128 bytes) rather than the per-car `carparam.dat`. This is a simplified vehicle with:
- Fixed bounding box, weight, and friction parameters
- No difficulty scaling (AI racers get scaling per tier/traffic/circuit)

Racers (slots 0--5) get the block at `0x473DB0` with extensive difficulty tier scaling.

---

## 4. Respawn: Per-Frame Recycling

### `RecycleTrafficActorFromQueue` (0x4353B0)

Called once per frame from `UpdateTrafficRoutePlan` for each traffic actor. Checks if any slot has fallen far enough behind the player to be recycled, and if the next queue entry is far enough ahead.

**Despawn condition** (which slot to recycle):
- Scans slots 6 through `min(DAT_004aaf00, 12)`
- Finds the slot with the **greatest span distance behind the player** (player span - traffic span)
- Must be >= **0x29 (41) spans behind** the player

**Spawn condition** (where to place new traffic):
- Scans the queue starting at `DAT_004b08b8`
- Skips entries whose span is **less than 0x28 (40) spans ahead** of the player
- The first entry >= 40 spans ahead becomes the new spawn point

**Slot 9 protection**: If slot 9 is in use by the special encounter system (`gSpecialEncounterTrackedActorHandle != -1`), it is never recycled.

After recycling, the slot is fully reinitialized with the same placement logic as initial spawn, and all recovery/state fields are zeroed:
- `gActorTrafficRecoveryStage = 0`
- `steering_command = 0`
- `path_vec_x = 0`, `path_vec_y = 0`
- Frame counter, heading overrides, and collision state all cleared

---

## 5. Driving: Route Following

### `UpdateTrafficRoutePlan` (0x435E80)

The primary traffic AI brain, called per-frame from `UpdateRaceActors` for each traffic slot. It:

1. **Calls RecycleTrafficActorFromQueue** to handle the respawn queue
2. **Heading misalignment check**: Computes the angular delta between the actor's current heading and the route tangent direction. If the actor is traveling more than 90 degrees off-course (heading delta in range 0x400--0xC00 on the 12-bit circle), it triggers **recovery mode** -- sets `gActorTrafficRecoveryStage` to a random nonzero value (1--7, from low bits of a frame counter). This detects spun-out vehicles.
3. **Edge-of-track / recovery bail-out**: If the actor is near span 0/end, on the alternate route, already recovering, or in a special state, it sets the **brake flag** (`gap_0346[0x27] = 1`, `encounter_steering_override = 0`) and returns immediately
4. **Normal driving**: Sets `encounter_steering_override = 0x3C` (60 decimal) -- this is the **constant traffic speed command**
5. **Next target point**: Computes the world position of the next span ahead (or behind for oncoming), accounting for lane changes at junctions and route table transitions
6. **Steering**: Calls `UpdateActorSteeringBias(routeState, 0x8000)` with a large direct bias, which produces smooth speed-dependent steering corrections toward the next target
7. **Peer avoidance** (see below)

### Oncoming Traffic (flags bit 0 = 1)

Oncoming vehicles have `gActorRouteDirectionPolarity = 1`. This causes:
- **Heading flip**: +0x80000 (180 degrees) at spawn
- **Route planning reversal**: `UpdateTrafficRoutePlan` looks at `span - 1` instead of `span + 1` for the next target
- **Peer search direction**: `FindNearestRoutePeer` searches in the opposite direction
- **The heading check** applies a 0x800 offset before comparing, accounting for the inverted travel direction

Oncoming traffic does **NOT** have any special player-avoidance logic. They follow their lane on the opposite side of the road. Any head-on collision is handled by the same physics collision system as any other vehicle pair.

### Traffic Speed

Traffic speed is **constant** -- `encounter_steering_override = 0x3C` (60). This is consumed by `IntegrateVehicleFrictionForces` (0x4438F0) at offset +0x33E as a lateral force multiplied by 4:

```
iVar9 = *(short *)(param_1 + 0x33e) * 4;  // = 60 * 4 = 240
```

This value feeds into the simplified tire force model as a forward drive force. Traffic does **not** rubber-band, does **not** respond to the player's speed, and does **not** vary with difficulty.

When braking/yielding: `encounter_steering_override = -0x100` (-256), producing a strong deceleration force of -1024.

---

## 6. Peer Avoidance (Traffic-to-Traffic)

### `FindNearestRoutePeer` (0x433680)

Searches all actors (0 through 11) for the nearest one:
- On the **same lane** (`gap_0000[0x8c]` match)
- On the **same route table** (`gActorRouteTableSelector` match)
- **Ahead** of the current actor (for same-direction) or **behind** (for oncoming)
- Within **0x21 (33) spans**

Returns the peer's slot index, or the actor's own index if no peer found.

### Yield/Block Behavior in UpdateTrafficRoutePlan

After finding the nearest peer, the traffic planner computes:

1. **Closing rate**: Uses a weighted formula based on `gActorForwardTrackComponent` (forward speed projected onto track tangent) of both actors and their span distance
2. **Time-to-collision**: `(spanDelta * speed_peer * 0x5DC) / speedDelta + spanDelta * 0x5DC) / speed_self`
3. **Brake decision**: If the actor's current speed (shifted right by 10) is within 8 units of the time-to-collision estimate AND the closing rate is positive, the actor **brakes**:
   ```c
   gap_0346[0x27] = 1;  // brake flag
   encounter_steering_override = -0x100;  // strong deceleration
   ```

This creates a basic **follow distance** system where trailing traffic slows down to avoid rear-ending the vehicle ahead. There is **no lane-change avoidance** -- traffic stays in its assigned lane.

If no peer is found or the peer is far away, the default time-to-collision is `0x2EE00` (192000), which never triggers braking.

---

## 7. Collision Response

### Normal Collision Path

Traffic vehicles participate in the **same collision system** as all other vehicles. In `ResolveVehicleContacts` (0x409150):

1. **Broadphase**: AABB overlap test using bounding boxes at `DAT_00483050`
2. **Mode check**: If BOTH actors have `gap_0376[3] == 0` (normal physics mode) AND `gap_0376[6] < 0xF` (not in damage state), use full collision: `ResolveVehicleCollisionPair`
3. **Fallback**: Otherwise use `ResolveSimpleActorSeparation` -- a radius-based push-apart that only adjusts velocities without angular impulse

### `ResolveVehicleCollisionPair` (0x408A60)

Full narrow-phase collision:
1. `CollectVehicleCollisionContacts` transforms both vehicle bounding volumes into each other's frame, tests 8 corner penetrations (4 per vehicle), returns a bitmask
2. Binary search (8 iterations) refines the exact contact time
3. `ApplyVehicleCollisionImpulse` for each active contact point: mass-weighted impulse exchange with angular moment

### Traffic-Specific Collision Effects in `ApplyVehicleCollisionImpulse` (0x4079C0)

```c
bVar2 = *(byte *)(param_1 + 0x375);  // slot_index
if ((5 < bVar2) && (50000 < iVar10)) {  // slot >= 6 (traffic) AND impact > 50000
    iVar11 = gActorTrafficRecoveryStage[bVar2];
    if ((0 < iVar11) && (iVar11 < 7)) {
        gActorTrafficRecoveryStage[bVar2] = iVar11 + 1;  // escalate recovery
    }
}
```

For **heavy impacts** (magnitude > 90000):
- **Racers (slot < 6)**: receive random angular deformation to velocity components (+0xe0/+0xe2/+0xe4) and a vertical bounce (+0xe8 = impact/6)
- **Traffic (slot >= 6)**: receive a **random rotation matrix** stored at +0xC0 (the "spin matrix"), `gap_0376[3]` is set to `1` (scripted/recovery mode), and `gap_0338[8]` counter is zeroed

This is the transition from **normal physics** to **scripted recovery mode**.

---

## 8. Recovery and Reposition

### Recovery Mode (gap_0376[3] == 1)

When a traffic vehicle takes a heavy collision, it enters scripted mode. In `UpdateTrafficActorMotion` (0x443ED0):

```c
if (cVar3 == 0) {
    // NORMAL: friction forces -> pose update -> route advance -> segment transition
    IntegrateVehicleFrictionForces(pRVar2);
    UpdateTrafficVehiclePose(pRVar2);
    ProcessActorRouteAdvance(param_1);
    ProcessActorForwardCheckpointPass(param_1);
    ProcessActorSegmentTransition(param_1);
} else if (cVar3 == 1) {
    // SCRIPTED RECOVERY: simplified transform refresh + damped motion
    RefreshScriptedVehicleTransforms(pRVar2);
    IntegrateScriptedVehicleMotion(pRVar2);
    // + track contact updates
}
```

In scripted mode (`IntegrateScriptedVehicleMotion` at 0x409D20):
- Velocities are **damped** by 1/256 per frame (exponential decay)
- Gravity is applied
- The rotation matrix from the collision spin is applied each frame
- Track contact normals keep the vehicle grounded
- If the scripted timer (`gap_0338[8]`, which is `*(short*)(actor + 0x338)`) exceeds **0x3B (59 frames, ~2 seconds)**, `ResetVehicleActorState` is called, which snaps the vehicle back to a clean state on the track

### `gActorTrafficRecoveryStage` State Machine

| Value | Meaning |
|-------|---------|
| 0 | Normal driving |
| 1--7 | Recovery stages (set by heading misalignment check or collision escalation) |

When nonzero, `UpdateTrafficRoutePlan` treats the actor as uncontrollable:
- Sets brake flag and zero throttle
- Skips all route planning and peer avoidance

The stage escalates on repeated collisions (each heavy impact increments by 1, capped at 7). Recovery clears to 0 only when the slot is **recycled** (respawned from the queue).

---

## 9. The Special Encounter System

### Overview

Slot 9 can be "promoted" from regular traffic to a **special encounter** vehicle, controlled by `UpdateSpecialTrafficEncounter` (0x434DA0) and `UpdateSpecialEncounterControl` (0x434BA0). This is used in Championship/Cup modes (gated by `DAT_0046320C`) -- not in Cop Chase (which uses `gWantedModeEnabled`).

### Acquisition Conditions

All must be true:
1. `gSpecialEncounterTrackedActorHandle == -1` (no active encounter)
2. `DAT_004b064c == 0` (300-frame cooldown expired)
3. Player is exactly 2 laps ahead of their own position metric
4. Player forward speed `>= 0x15639` (~87577 in fixed-point)
5. `gActorForwardTrackComponent > 0` (player moving forward)
6. Player on the correct route table
7. Player span delta within 16 spans

### Encounter Behavior

When acquired:
- `gSpecialEncounterTrackedActorHandle = 0` (track player slot 0)
- Slot 9 switches from `UpdateTrafficRoutePlan`/`UpdateTrafficActorMotion` to `UpdateActorTrackBehavior`/`UpdateVehicleActor` (the full AI racer path)
- `UpdateSpecialEncounterControl` forces aggressive behavior: brake flag, steering override = -0x100

### Release Conditions

The encounter is released when:
- The encounter vehicle falls **more than 0x40 (64) spans behind** the player
- OR the encounter vehicle **passes the player** (1 span ahead on same route)
- OR heading alignment fails (actor sideways)

On release: 300-frame (10-second) cooldown before next encounter, encounter state zeroed, audio stopped.

---

## 10. Motion Pipeline

### `UpdateTrafficActorMotion` (0x443ED0) -- Per-Frame

Called for each traffic slot from `UpdateRaceActors`. The pipeline is:

**Normal mode (gap_0376[3] == 0):**

1. **Frame counter increment**: `*(short*)(actor + 0x338) += 1`
2. `IntegrateVehicleFrictionForces` (0x4438F0): Simplified 2-axle tire model
   - Velocity damping: 1/256 per frame
   - Heading from `*(int*)(param_1 + 0x1F4)` (heading_12bit) and steering from `*(int*)(param_1 + 0x30C)` (steering_command)
   - Drive force from `encounter_steering_override * 4` at offset +0x33E
   - Computes front/rear axle forces, lateral grip, angular rotation
   - Updates velocity (0x1CC, 0x1D4), yaw rate (0x1C4), and world position (0x1FC, 0x204)
   - Calls `ApplyDampedSuspensionForce` for pitch
3. `UpdateTrafficVehiclePose` (0x443CF0): Converts physics state to render state
   - Updates world position via `UpdateActorTrackPosition`
   - Computes track contact normal
   - Applies gravity correction from vehicle physics table (+0x43 = gravity offset)
   - Transforms 4 bounding box corners into world space
   - Computes pitch and roll from contact normal
4. `ProcessActorRouteAdvance` (0x407840): Boundary force at end-of-track
5. `ProcessActorForwardCheckpointPass` (0x4076C0): Checkpoint counting
6. `ProcessActorSegmentTransition` (0x407390): Lane boundary forces to keep vehicle in lane

**Scripted mode (gap_0376[3] == 1):**
- Damped ballistic motion with spin matrix (see Section 8)

---

## 11. Route Data Usage

Traffic vehicles use **both** LEFT.TRK and RIGHT.TRK route data, NOT a simplified path:

- `gActorRouteTableSelector[slot] == 0`: Uses LEFT.TRK (`DAT_004afb58`)
- `gActorRouteTableSelector[slot] == 1`: Uses RIGHT.TRK (`DAT_004b08b4`)

The route table maps each span to a **route byte** containing heading direction. The traffic planner reads the byte at `routeTable + span * 3 + 1` and multiplies by `0x102C` to get a 12-bit heading angle.

Route table selection is determined by the lane index in the TRAFFIC.BUS spawn record: lanes below the span's lane count go to LEFT.TRK, lanes above go to RIGHT.TRK (with junction mapping from the `DAT_004c3da0` junction table).

---

## 12. Complete Function Map

| Address | Function | Role |
|---------|----------|------|
| 0x432E60 | `InitializeRaceActorRuntime` | Sets DAT_004aaf00, selects physics blocks, calls initial spawn |
| 0x435940 | `InitializeTrafficActorsFromQueue` | Fills slots 6--11 from TRAFFIC.BUS queue |
| 0x4353B0 | `RecycleTrafficActorFromQueue` | Per-frame respawn: find stale slot, place from queue |
| 0x435E80 | `UpdateTrafficRoutePlan` | Route planner: heading check, target selection, steering, peer avoidance |
| 0x443ED0 | `UpdateTrafficActorMotion` | Motion dispatcher: normal or scripted path |
| 0x4438F0 | `IntegrateVehicleFrictionForces` | Simplified 2-axle tire friction integrator |
| 0x443CF0 | `UpdateTrafficVehiclePose` | Physics -> render state, track contact, bounding volume |
| 0x407840 | `ProcessActorRouteAdvance` | End-of-track boundary force |
| 0x4076C0 | `ProcessActorForwardCheckpointPass` | Checkpoint progression |
| 0x407390 | `ProcessActorSegmentTransition` | Lane boundary constraint forces |
| 0x433680 | `FindNearestRoutePeer` | Nearest same-lane/same-route actor within 33 spans |
| 0x4340C0 | `UpdateActorSteeringBias` | Speed-dependent steering correction |
| 0x407270 | `ApplySimpleTrackSurfaceForce` | Barrier/edge bounce-back force |
| 0x434DA0 | `UpdateSpecialTrafficEncounter` | Special encounter acquisition/release |
| 0x434BA0 | `UpdateSpecialEncounterControl` | Encounter override: force brake on slot 9 |
| 0x436A70 | `UpdateRaceActors` | Master dispatcher: racers + traffic + encounter |
| 0x4431C0 | `LoadTrafficVehicleSkinTexture` | Loads skin_%d.tga from traffic.zip |
| 0x443240 | `GetTrafficVehicleVariantType` | Returns vehicle type (1 or 2) from variant table |
| 0x42F140 | `LoadTrackRuntimeData` | Loads TRAFFIC.BUS from level ZIP |
| 0x409150 | `ResolveVehicleContacts` | Broadphase + pairwise collision dispatch |
| 0x408A60 | `ResolveVehicleCollisionPair` | Full narrow-phase collision |
| 0x4079C0 | `ApplyVehicleCollisionImpulse` | Impulse exchange, recovery escalation |
| 0x408F70 | `ResolveSimpleActorSeparation` | Radius push-apart fallback |
| 0x409BF0 | `RefreshScriptedVehicleTransforms` | Recovery mode transform refresh |
| 0x409D20 | `IntegrateScriptedVehicleMotion` | Damped ballistic + spin recovery |
| 0x405D70 | `ResetVehicleActorState` | Full state reset (end of recovery) |

---

## 13. Key Globals

| Address | Name | Description |
|---------|------|-------------|
| 0x4AAF00 | `DAT_004aaf00` | Active actor count (6 = no traffic, 12 = with traffic) |
| 0x4B08B8 | `DAT_004b08b8` | Current TRAFFIC.BUS queue read pointer |
| 0x4AED8C | `DAT_004aed8c` | TRAFFIC.BUS data buffer base |
| 0x4AFB58 | `DAT_004afb58` | LEFT.TRK route table pointer |
| 0x4B08B4 | `DAT_004b08b4` | RIGHT.TRK route table pointer |
| 0x4AFC48+i*0x11C | `gActorTrafficRecoveryStage` | Per-slot recovery stage (0=normal, 1-7=recovering) |
| 0x4AFC24+i*0x11C | `gActorRouteDirectionPolarity` | Per-slot: 0=same direction, 1=oncoming |
| 0x4AFB60+i*0x47*4 | `gActorRouteTableSelector` | Per-slot: 0=LEFT, 1=RIGHT route table |
| 0x4AFC50+i*0x11C | `DAT_004afc50` | Per-slot: special state flags |
| 0x473BE8 | (traffic physics) | 128-byte simplified vehicle parameter block |
| 0x473DB0 | (racer physics) | 128-byte full racer parameter block (with scaling) |

---

## 14. Actor Struct Fields Used by Traffic

Key offsets within the 0x388-byte `RuntimeSlotActor` struct:

| Byte Offset | Size | Field | Traffic Usage |
|-------------|------|-------|---------------|
| +0x80 | short | span_index (in gap_0000) | Current track span |
| +0x82 | short | node_index (in gap_0000) | Current track node |
| +0x8C | byte | lane (in gap_0000) | Lane within span |
| +0x1B8 | ptr | physics_params (in gap_0000) | -> 0x473BE8 for traffic |
| +0x1C4 | int | yaw_rate (in gap_01d8) | Angular velocity |
| +0x1CC | int | vel_x (path_vec_x-relative) | Forward velocity component |
| +0x1D4 | int | vel_z (path_vec_y-relative) | Lateral velocity component |
| +0x1F4 | int | heading_12bit | 20-bit heading (>>8 = 12-bit angle) |
| +0x1FC | int | world_x (in gap_01f8) | World position X |
| +0x200 | int | world_y (in gap_01f8) | World position Y |
| +0x204 | int | world_z (in gap_01f8) | World position Z |
| +0x30C | int | steering_command (at gap_0310+4) | Current steering angle |
| +0x314 | int | lateral_speed (gap_0310) | Forward track component cache |
| +0x338 | short | frame_counter (gap_0338) | Frames since spawn/reset |
| +0x33E | short | encounter_steering_override | **Throttle**: 0x3C=drive, 0=stopped, -0x100=brake |
| +0x36D | char | gap_0346[0x27] | **Brake flag**: 1=braking |
| +0x375 | byte | slot_index | Actor slot (6--11 for traffic) |
| +0x379 | byte | gap_0376[3] | **Mode**: 0=normal, 1=scripted/recovery |

---

## 15. Complete Lifecycle

```
TRAFFIC.BUS loaded from level ZIP
        |
        v
InitializeTrafficActorsFromQueue (race start)
  fills slots 6-11 from queue head
        |
        v
  +---> UpdateTrafficRoutePlan (per frame) <-----------+
  |       |                                              |
  |       +-- RecycleTrafficActorFromQueue               |
  |       |     if slot > 41 spans behind: recycle       |
  |       |     if next queue entry > 40 spans ahead:    |
  |       |       reinitialize slot from queue entry     |
  |       |                                              |
  |       +-- Heading misalignment check                 |
  |       |     if > 90 deg off-route: enter recovery    |
  |       |                                              |
  |       +-- Compute next target span                   |
  |       +-- UpdateActorSteeringBias (smooth steering)  |
  |       +-- FindNearestRoutePeer                       |
  |       +-- Yield/brake if closing on peer             |
  |       |                                              |
  |       v                                              |
  |   UpdateTrafficActorMotion                           |
  |     mode 0 (normal):                                 |
  |       IntegrateVehicleFrictionForces (drive)         |
  |       UpdateTrafficVehiclePose (render)              |
  |       ProcessActorSegmentTransition (lane walls)     |
  |     mode 1 (recovery):                               |
  |       IntegrateScriptedVehicleMotion (damped spin)   |
  |       if timer > 59: ResetVehicleActorState          |
  |                         -> back to mode 0            |
  |       |                                              |
  |       v                                              |
  |   ResolveVehicleContacts (shared system)             |
  |     if impact > 50000 & slot >= 6:                   |
  |       escalate gActorTrafficRecoveryStage            |
  |     if impact > 90000 & slot >= 6:                   |
  |       enter scripted mode (spin + recovery)          |
  |       |                                              |
  +-------+  (next frame)                                |
              |                                          |
              +-- if slot recycled: full reset ----------+
```

---

## 16. Key Behavioral Summary

1. **Traffic speed is constant** (throttle = 60, no rubber-banding, no player-speed response)
2. **Oncoming traffic has no special player avoidance** -- it stays in its lane; head-on collisions are pure physics
3. **Peer avoidance is same-lane only** -- no lane changes, just speed reduction when closing on the vehicle ahead
4. **Collision recovery is a 2-second ballistic spin** followed by a hard reset to clean state
5. **Repeated collisions escalate recovery** but never exceed stage 7; recovery only clears on respawn
6. **Traffic uses the full LEFT/RIGHT.TRK route data**, not a simplified path
7. **Slot 9 can be hijacked** for special encounters in cup modes, switching to full AI racer behavior
8. **The spawn window is asymmetric**: 40 spans ahead (spawn), 41 spans behind (despawn)
9. **Traffic vehicles share the same collision system** as all racers -- no collision ghosting or special rules
10. **Vehicle skins** are loaded from `traffic.zip` (skin_%d.tga), with variant type from a lookup table at `DAT_00474CE8`
