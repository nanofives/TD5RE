# AI System Deep Dive

> Covers grip tuning, torque modification, steering/path-following, and race strategy.
> Session: 2026-03-19

---

## 1. AI Grip Clamp Asymmetry

### Finding: Hardcoded, intentional balance -- NOT difficulty-dependent

**AI path** (`UpdateAIVehicleDynamics` 0x404EC0):
- Uses **single** surface probe at actor center (offset +0x80)
- Computes front/rear grip from weight-distribution formula using `wheelbase_front` / `wheelbase_rear` ratios
- Clamps **both** front and rear grip to **[0x70, 0xA0]** (112-160 in fixed-point)

```c
// AI grip clamp (front axle = local_38, rear axle = local_20)
if (local_38 < 0x70) local_38 = 0x70;
else if (0xa0 < local_38) local_38 = 0xa0;
if (local_20 < 0x70) local_20 = 0x70;
else if (0xa0 < local_20) local_20 = 0xa0;
```

**Player path** (`UpdatePlayerVehicleDynamics` 0x404030):
- Uses **five** surface probes: center + 4 individual wheel contact points
- Computes per-wheel grip, each modulated by per-surface grip table at `DAT_004748C0`
- Clamps **all four** wheel grip values to **[0x38, 0x50]** (56-80 in fixed-point)
- Additionally applies a handbrake modifier (carparam offset +0x7A) to rear wheels when active

```c
// Player grip clamp (4 wheels: local_5c, local_58, local_10, local_3c)
if (local_5c < 0x38) local_5c = 0x38;
else if (0x50 < local_5c) local_5c = 0x50;
// ... same for all 4 wheels
```

### Analysis

| Property | AI | Player |
|---|---|---|
| Grip range | 0x70-0xA0 (112-160) | 0x38-0x50 (56-80) |
| Range width | 48 units | 24 units |
| Surface probes | 1 (center) | 5 (center + 4 wheels) |
| Per-wheel resolution | No (2 axles) | Yes (4 wheels) |
| Per-surface grip table | `DAT_00474900` only | `DAT_00474900` + `DAT_004748C0` |
| Handbrake modifier | None | Yes (carparam +0x7A) |
| Difficulty-affected | **No** | **No** |

**The AI has ~2x higher grip than the player.** This is hardcoded and not modified by difficulty (easy/normal/hard) or race tier (0/1/2). The grip clamp constants are immediate values in the instructions, not loaded from any table.

**Why:** The AI uses a vastly simplified 2-axle dynamics model vs the player's 4-wheel model. The higher grip compensates for the AI's inability to manage individual wheel slip, making it stable on corners where a per-wheel model would naturally self-correct. Without the elevated grip floor, the simplified AI model would spin out on every sharp turn.

### Surface Grip Tables

- `DAT_00474900` (shared): Short[32] -- base friction per surface type, read by both paths. Values range 0x00 to 0x20 (most surfaces = 0x20).
- `DAT_004748C0` (player-only): Short[32] -- per-wheel surface grip multiplier. Values range 0xB4-0x100 (0xC0 for most surfaces, 0x100 for road). This table adds per-surface penalty granularity that the AI path skips entirely.

---

## 2. AI Torque Curve Per-Player Modification

### Data Layout

Three parallel arrays at `DAT_004AED40`, `DAT_004AED58`, `DAT_004AED28`, each with 6 dword entries (one per player slot):

| Array | Purpose | Effect |
|---|---|---|
| `DAT_004AED40[i]` | Torque curve shape modifier | Scales each gear's torque point progressively from top gear downward |
| `DAT_004AED58[i]` | Top speed / rev limiter modifier | Scales max speed (`carparam +0x74`) and adjusts gear ceiling thresholds |
| `DAT_004AED28[i]` | Wheelbase/traction modifier | Scales base grip factor (`carparam +0x2C`) and traction accumulator |

### Initialization (in `InitializeRaceSession` 0x42AA10)

```c
do {
    index = DAT_00497298[i];       // per-slot car selection index
    lookup = index * 0xC;          // 12 bytes per car entry
    DAT_004AED40[i] = DAT_00466F90[lookup + 0];   // torque shape
    DAT_004AED58[i] = DAT_00466F90[lookup + 1];   // top speed
    DAT_004AED28[i] = DAT_00466F90[lookup + 2];   // traction
    i++;
} while (i < 6);
```

The source table at `0x466F90` contains per-car performance balancing triplets. The car selection index (from the frontend) selects which triplet to use.

### Application (in `InitializeRaceVehicleRuntime` 0x42F420)

Only applied to **player-controlled** slots (`gRaceSlotStateTable.slot[i].state == 0x01`):

1. **Torque curve shaping** (`DAT_004AED40`):
   - Iterates gear entries from `carparam +0x32` (3rd gear) up to the top gear
   - If modifier < 0: scales each point by `(point * modifier / 0x300)`, weighted linearly so top gear gets full effect, lower gears get less
   - If modifier > 0: scales by `(point * modifier) >> 9`, same linear weighting
   - Effect: Makes the player's car faster or slower relative to AI depending on car selection

2. **Top speed scaling** (`DAT_004AED58`):
   - Scales `carparam +0x74` (rev limiter / max speed) by `(modifier + 0x800) >> 11`
   - Adjusts top-gear and sub-top-gear thresholds inversely
   - Scales low-speed grip (`carparam +0x6A`) by `(0x200 - modifier) >> 9`

3. **Traction modifier** (`DAT_004AED28`):
   - If < 0: adjusts base grip at `carparam +0x2C` downward by `value / 0x300`
   - If > 0: adjusts base grip upward by `value >> 9`, also scales traction accumulator

### Algorithm Classification

This is **per-car balancing**, NOT rubber-banding. The values are determined entirely by which car the player selected, set once at race init, and never change during the race. It exists to level the playing field when the player picks a car that would otherwise be overpowered or underpowered relative to the AI's fixed physics table at `DAT_00473DB0`.

The source data at `0x466F90`:
```
Car 0:  torque=0x00, speed=0x00, traction=0x00     (baseline/no adjustment)
Car 1:  torque=0x72, speed=0xFFFFFF9A, traction=0xFFFFFFD8  (faster torque, lower speed/traction)
Car 2:  torque=0xFFFFFFD8, speed=0x133, traction=0xFFFFFFD8
...etc (9 cars total, 12 bytes each)
```

---

## 3. AI Steering / Path Following

### Architecture Overview

The AI steering system is a 4-layer pipeline:

```
Layer 1: Route Selection (LEFT.TRK / RIGHT.TRK)
   |
Layer 2: Target Point Sampling (SampleTrackTargetPoint)
   |
Layer 3: Heading Delta & Steering Bias (UpdateActorSteeringBias)
   |
Layer 4: Track Script Override (AdvanceActorTrackScript)
```

### Layer 1: Route Data

Two route tables loaded from LEFT.TRK and RIGHT.TRK (pointed to by `DAT_004AFB58` and `DAT_004B08B4`). Each is a byte-array indexed by track span number, with 3 bytes per span:

- `route[span*3 + 0]`: Lane index (lateral position target, 0 = rightmost)
- `route[span*3 + 1]`: Heading byte (desired heading at this span, scaled by 0x102C to get 12-bit angle)
- `route[span*3 + 2]`: Speed threshold byte (0x00 = emergency stop, 0x01-0xFE = scaled speed limit, 0xFF = no limit)

Actors alternate between LEFT and RIGHT routes based on their slot index (even slots = RIGHT, odd = LEFT), stored in `gActorRouteTableSelector`.

### Layer 2: Target Point Sampling

`UpdateActorTrackBehavior` (0x434FE0) is the main AI path-following tick, called for each non-player racer:

1. Looks ahead 4 spans from current position
2. Handles junction remapping via the junction table at `DAT_004C3DA0`
3. Calls `SampleTrackTargetPoint` with the look-ahead span, the route lane byte, and a lateral offset bias
4. Computes a world-space delta vector from actor to target point
5. Converts delta to a 12-bit angle via `AngleFromVector12`

### Layer 3: Heading Correction & Steering Bias

`UpdateActorSteeringBias` (0x4340C0) is called with the route state and a steering weight parameter:

**Inputs:**
- `param_1 + 0x58`: Left-side deviation distance
- `param_1 + 0x5C`: Right-side deviation distance
- `param_2`: Steering delta magnitude (0x10000 for normal, 0x20000 for braking, 0x4000 from script)

**Speed-dependent steering:**
- Computes a rate clamp: `rate = 0xC0000 / (speed_scaled * 0x400 / (speed_sq + 0x400) + 0x40)`
- Computes an absolute clamp: `max_steer = 0x1800000 / (speed_scaled * 0x10000 / (speed_sq + 0x10000) + 0x100)`
- At low speed: large correction allowed (up to 0x18000 = ~96 degrees)
- At high speed: small corrections only, asymptotically approaching zero

**Three steering modes based on deviation thresholds:**

| Left deviation | Right deviation | Action |
|---|---|---|
| < 0x400 | - | Apply `param_2` as direct steering delta (sin-scaled if < 0x100) |
| 0x401-0x7FF | - | Ramp-up steering with accumulator at `gap_0338+2` (0x40 per tick, max 0x100) |
| >= 0x800 | - | Emergency snap: add 0x4000 per tick toward left |
| - | < 0x400 | Mirror of above, steering right |
| - | 0x401-0x7FF | Mirror ramp-up |
| - | >= 0x800 | Emergency snap: subtract 0x4000 per tick |

Final clamp: steering_command is hard-limited to [-0x18000, +0x18000].

### Layer 4: Track Script Override

`AdvanceActorTrackScript` (0x4370A0) is a bytecode interpreter with 12 opcodes:

| Opcode | Effect |
|---|---|
| 0 | Wait for heading alignment within +/-0x40 of route, then reset script |
| 1 | Set countdown timer |
| 2 | Set speed target + enable speed-tracking mode (flag 0x02) |
| 3 | Set flag bits |
| 4 | Clear flag bits |
| 5 | Enable "steer to align with route" mode (flag 0x04) |
| 6 | Enable "steer opposite to route" mode (flag 0x08) |
| 7 | Force full brake (-0x100 throttle) |
| 8 | Enable "wait for speed near zero" mode (flag 0x10) |
| 9 | Lane-change decision: evaluates heading vs route, picks one of 4 script sequences |
| 10 | Set flag 0x40 (latent, no known consumer) |
| 11 | Set flag 0x80 (latent, no known consumer) |

Four pre-built script sequences at `0x473CD4-0x473D28`:
- Sequence A (0x473CD4): Set speed target -0x20, steer-to-align, wait, set speed target +0x40, steer-opposite, wait
- Sequence B (0x473CEC): Mirror of A
- Sequence C (0x473D00): Variant with different speed targets
- Sequence D (0x473D18): Variant with different speed targets

These implement scripted lane-change and recovery maneuvers.

### Obstacle Avoidance (Lateral Offset)

`UpdateActorTrackOffsetBias` (0x434900) and `FindActorTrackOffsetPeer` (0x4337E0) implement peer avoidance:

1. `FindActorTrackOffsetPeer` scans all actors on the same route band, finds the nearest one ahead within 0x28 spans whose lateral range overlaps
2. If a peer is found, `UpdateActorTrackOffsetBias` pushes the lateral offset:
   - If the peer is on the outer lane: offset += (0x29 - distance) (push inward)
   - If the peer is on the inner lane: offset -= (0x29 - distance) (push outward)
   - Distance is clamped to [1, 0x28] spans
3. If no peer is found: offset decays toward zero at 8 units/tick

This is the **only overtaking mechanism** -- there is no explicit "overtake decision." The AI simply adjusts its lateral target to go around whatever is in front of it.

---

## 4. AI Race Strategy

### Throttle/Brake Control

`UpdateActorRouteThresholdState` (0x434AA0) is the primary throttle controller, operating in 3 states based on the route speed-threshold byte:

| Threshold byte | Forward track component vs threshold | Action |
|---|---|---|
| 0x00 (emergency) | forward > 0x80 AND speed < 0x10000 | Full brake (-0x100), set brake flags |
| 0x01-0xFE | forward >= threshold * 0x400 / 0xFF | Coast: steering = 0, no brake |
| 0x01-0xFE | forward < scaled threshold | Accelerate: steering = `gActorDefaultRouteSteerBias` (rubber-band modified) |

The return value (0 or 1) determines the steering weight passed to `UpdateActorSteeringBias`:
- Return 1 (braking): passes 0x10000 (smaller corrections)
- Return 0 (coasting/accelerating): passes 0x20000 (larger corrections)

### Rubber-Band System

`ComputeAIRubberBandThrottle` (0x432D60) runs at the start of every `UpdateRaceActors` tick:

1. Copies the per-difficulty default throttle array from `0x473D64` into the live array at `0x473D2C`
2. For each AI racer (slots 0-5, state==0x00):
   - Computes span distance from player 0: `delta = AI_span - player0_span`
   - If AI is **behind** player (delta < 0): `modifier = (DAT_00473D9C * delta) / DAT_00473DA4`
   - If AI is **ahead** of player (delta > 0): `modifier = (DAT_00473DA0 * delta) / DAT_00473DA8`
   - `gActorDefaultRouteSteerBias[slot] = 0x100 - modifier`
3. This bias is consumed by `UpdateActorRouteThresholdState` as the throttle value during acceleration

**Effective behavior:**
- AI behind player: bias increases above 0x100 (extra throttle = catch-up)
- AI ahead of player: bias decreases below 0x100 (reduced throttle = slow down)

### Difficulty-Dependent Rubber Band Parameters

`InitializeRaceActorRuntime` (0x432E60) configures the rubber band via a complex decision tree:

**First layer -- global difficulty:**
- Easy: gravity=0x5DC, no additional AI scaling
- Normal: gravity=0x76C, scale AI steering factor by 0x168/256, grip by 300/256
- Hard: gravity=0x800, scale AI steering factor by 0x28A/256, grip by 0x17C/256, plus brake force scaling

**Second layer -- mode/circuit/traffic/tier matrix:**

| Circuit? | Traffic? | Tier | AI top speed scale | Rubber behind (scale/range) | Rubber ahead (scale/range) |
|---|---|---|---|---|---|
| No | No | 0 | 0xAA/256 | 0xA0 / 100 | 0x96 / 0x50 |
| No | No | 1 | 0xB4/256 | 0xC8 / 0x4B | 0xC0 / 0x4B |
| No | No | 2 | 0xDC/256 | 0x10E / 0x41 | 0x96 / 0x50 |
| No | Yes | 0 | 0xB4/256 | 0xB4 / 0x4B | 0xBE / 100 |
| No | Yes | 1 | 0xBE/256 | 0xC8 / 0x3C | 0xBE / 100 |
| No | Yes | 2 | 0xDC/256 | 0xDC / 0x3C | 100 / 0x40 |
| Yes | - | 0 | 0x91/256 | 0x8C / 100 | 0xC8 / 0x37 |
| Yes | - | 1 | 0xA0/256 | 0x96 / 100 | 0xC0 / 0x40 |
| Yes | - | 2 | 0xC3/256 | 0xC8 / 100 | 0x78 / 0x40 |
| (Pitbull/4) | - | - | 0x91/256 | 0x8C / 100 | 0xC0 / 0x40 |
| Time Trial | - | - | scale=0, range=0x40 | (no rubber band) | (no rubber band) |

Higher tiers make the AI faster (higher top speed scale) and more aggressive (stronger rubber-band catch-up, weaker slow-down).

### Game Mode Differences

**Time Trial** (`gTimeTrialModeEnabled != 0`):
- Only 2 actors spawned (player + ghost)
- Rubber band parameters zeroed (DAT_00473D9C=0, DAT_00473DA0=0)
- AI actors with `gap_0376[8] != 0` get forced to brake/steer=0 (ghost replay)

**Wanted/Cop Chase** (`gWantedModeEnabled != 0`):
- Tracked via `gSpecialEncounterTrackedActorHandle` and `gSpecialEncounterEnabled`
- Actor slot 9 is the encounter vehicle (cop/special NPC)
- AI racers (slots 0-5) with `gWantedModeEnabled` active skip `UpdateActorTrackBehavior` unless they have a non-zero entry in `DAT_004BEAD4` table
- Slot 2 state set to 0x02 = "finished/dead" behavior: if forward track component < 0, steer=0; else brake and force flag=1

**Encounter NPC (slot 9):**
- `UpdateSpecialEncounterControl` (0x434BA0) overrides normal AI when encounter is active
- Activation: player must be within 2 spans, speed > 0x15639, forward component > 0, same route band, and within 0x10 lateral offset
- While active: forced steer bias = 0xFF00, throttle = -0x100 (brake) when within minimum forward threshold
- Deactivation: heading misalignment > 90 degrees OR spacing conditions fail -> encounter tears down, 300-tick cooldown timer
- Each deactivation increments `special_encounter_state` counter

**Drag Race** (`gDragRaceModeEnabled != 0`):
- All 6 slots iterated without the normal AI/player branching
- State 0x02 actors get the same brake behavior
- No traffic actors (6 slots only)
- `UpdateActorTrackBehavior` is NOT called; drag racers follow a straight-line path

### Overtaking Logic

There is **no explicit overtaking decision tree**. Overtaking emerges from two systems:

1. **Lateral offset bias** (described in Section 3): When a peer is detected ahead, the AI shifts laterally. The direction of shift depends on which side of the track the peer occupies relative to the lane center. This naturally produces side-by-side racing and passing.

2. **Rubber-band throttle**: An AI that falls behind gets more throttle, which combined with lateral offset, produces overtaking behavior organically.

The simplicity is notable: TD5's AI never evaluates whether to overtake, never plans maneuvers, and has no concept of racing lines. It follows route data with speed thresholds, avoids whatever is directly ahead by shifting laterally, and lets the rubber band keep everyone in a pack.

---

## Key Data Structures Summary

| Address | Name | Purpose |
|---|---|---|
| `0x473DB0` | AI physics template | 128-byte template copied to each AI actor's physics slot |
| `0x473D64` | Default throttle array | 6 dwords, per-slot default bias (typically 0x100-0x140) |
| `0x473D9C-DA8` | Rubber band params | behind_scale, ahead_scale, behind_range, ahead_range |
| `0x466F90` | Per-car torque triplets | 9 cars x 12 bytes: torque_shape, top_speed, traction |
| `0x474900` | Surface friction (shared) | Short[32] per surface type |
| `0x4748C0` | Surface grip (player only) | Short[32] per-wheel surface grip multiplier |
| `0x473CD4-D28` | Lane-change scripts | 4 bytecode sequences for scripted maneuvers |
| `gActorRouteStateTable` | Route state | Per-actor route table pointer + progress tracking |
| `gActorDefaultRouteSteerBias` | Live throttle bias | Rubber-band-modified per-actor throttle output |
| `gActorForwardTrackComponent` | Forward speed | Dot product of velocity with route heading |
