# AI Rubber-Banding / Catch-Up System Deep Dive

> Complete analysis of the dynamic AI speed adjustment system in TD5.
> Session: 2026-03-28

---

## 1. System Overview

TD5 uses a classic **rubber-banding** system that dynamically adjusts AI throttle based on each AI racer's distance from the player. The system has three layers:

1. **Per-race initialization** (`InitializeRaceActorRuntime` at `0x432E60`) -- sets four global rubber-band parameters based on difficulty, tier, circuit/traffic/mode flags.
2. **Per-tick computation** (`ComputeAIRubberBandThrottle` at `0x432D60`) -- runs every frame, computes per-slot throttle bias from span distance to player.
3. **Per-actor consumption** (`UpdateActorRouteThresholdState` at `0x434AA0`) -- applies the bias as the AI's throttle command during acceleration phases.

### Call Chain

```
RunRaceFrame (0x42B580)
  |
  +-- UpdatePlayerSteeringWeightBalance (0x4036B0)   [2-player only]
  |
  +-- UpdateRaceActors (0x436A70)
  |     |
  |     +-- ComputeAIRubberBandThrottle (0x432D60)   [first thing, every tick]
  |     |
  |     +-- for each AI slot (state==0x00):
  |     |     +-- UpdateActorTrackBehavior (0x434FE0)
  |     |           +-- UpdateActorTrackOffsetBias (0x434900)
  |     |           +-- UpdateActorRouteThresholdState (0x434AA0)  [consumes rubber-band bias]
  |     |           +-- UpdateActorSteeringBias (0x4340C0)
  |     |
  |     +-- UpdateVehicleActor -> UpdateAIVehicleDynamics (0x404EC0)
  |
  +-- UpdateRaceOrder (0x42F5B0)
```

---

## 2. The Rubber-Band Algorithm

### 2.1 Parameter Setup (Once Per Race)

`InitializeRaceActorRuntime` (0x432E60) writes four globals that control rubber-band intensity:

| Address | Name | Purpose |
|---|---|---|
| `0x473D9C` | `behind_scale` | Numerator for catch-up modifier when AI is behind player |
| `0x473DA4` | `behind_range` | Denominator / max span distance for behind modifier |
| `0x473DA0` | `ahead_scale` | Numerator for slow-down modifier when AI is ahead of player |
| `0x473DA8` | `ahead_range` | Denominator / max span distance for ahead modifier |

These are set from a hardcoded decision tree indexed by `{circuit, traffic, tier, mode}` (see Section 4).

Additionally, a **default throttle table** at `0x473D64` (6 dwords, one per racer slot) provides baseline throttle values. At race init, these are:

```
Slot 0: 0x0100  (256)
Slot 1: 0x0100  (256)
Slot 2: 0x0140  (320)
Slot 3: 0x0118  (280)
Slot 4: 0x0122  (290)
Slot 5: 0x0140  (320)
```

Slots 2 and 5 start with a 25% throttle bonus, slots 3/4 with ~10-13% bonus. This creates baseline speed stratification so that AI racers do not all perform identically even before rubber-banding.

### 2.2 Per-Tick Computation

`ComputeAIRubberBandThrottle` (0x432D60) runs at the start of every `UpdateRaceActors` tick:

```c
// Step 1: Copy default throttle array to live array
memcpy(DAT_00473D2C, DAT_00473D64, 0x0E * 4);  // 14 dwords (6 slots + padding)

// Step 2: For each AI racer (state == 0x00, slots 0..min(racerCount,6)):
for (int i = 0; i < min(racerCount, 6); i++) {
    if (slotState[i] != AI) continue;

    int delta = AI_span[i] - player0_span;

    if (delta < 0) {
        // AI is BEHIND player
        if (delta < behind_range)    // clamp to max range (behind_range is negative)
            delta = behind_range;
        modifier = (behind_scale * delta) / behind_range;
    } else {
        // AI is AHEAD of player
        if (delta > ahead_range)     // clamp to max range
            delta = ahead_range;
        modifier = (ahead_scale * delta) / ahead_range;
    }

    gActorDefaultRouteSteerBias[i] = 0x100 - modifier;
}
```

**Key insight:** The modifier is computed as a **signed linear interpolation**:
- When AI is behind: `delta` is negative, `behind_range` is negative -> `modifier` is negative -> `0x100 - (negative)` = value **above** 0x100 = **extra throttle**
- When AI is ahead: `delta` is positive -> `modifier` is positive -> `0x100 - (positive)` = value **below** 0x100 = **reduced throttle**

The maximum catch-up boost is `0x100 + behind_scale` = e.g., `0x100 + 0xA0 = 0x1A0` (162% throttle).
The maximum slow-down is `0x100 - ahead_scale` = e.g., `0x100 - 0x96 = 0x6A` (41% throttle).

### 2.3 Network Mode Override

When `g_networkRaceActive != 0`, the rubber-band system is **completely disabled**. All AI slots are forced to a flat `0x100` throttle bias:

```c
if (g_networkRaceActive != 0) {
    for (int i = 0; i < min(racerCount, 6); i++) {
        if (slotState[i] == AI) {
            DAT_00473D2C[i] = 0x100;
            gActorDefaultRouteSteerBias[i] = 0x100;
        }
    }
}
```

### 2.4 Consumption by Throttle Controller

`UpdateActorRouteThresholdState` (0x434AA0) is where the rubber-band bias becomes actual throttle. It reads the route speed-threshold byte for the current track span and operates in three modes:

| Threshold | Forward component vs threshold | Action |
|---|---|---|
| `0x00` (emergency) | forward > 0x80 AND speed < 0x10000 | Full brake (0xFF00), return 1 |
| `0x01-0xFE` | forward >= scaled threshold | Coast: steering=0, no brake, return 0 |
| `0x01-0xFE` | forward < scaled threshold | **Accelerate**: steering = `gActorDefaultRouteSteerBias[slot]`, return 0 |

The critical line is:
```c
actor->encounter_steering_cmd = (short)gActorDefaultRouteSteerBias[slot];
```

This writes the rubber-band-modified value directly as the actor's throttle command. This value flows into `UpdateAIVehicleDynamics` where it scales the drive torque computation.

### 2.5 How Throttle Bias Becomes Speed

In `UpdateAIVehicleDynamics` (0x404EC0), the throttle command (`encounter_steering_cmd`, aliased here as `local_48`) controls the AI's acceleration:

- When `brake_flag == 0` and `local_48 != 0`: calls `ComputeDriveTorqueFromGearCurve` to get engine torque, which is proportional to the throttle value. Higher bias = more torque = faster acceleration.
- When `brake_flag != 0` or `local_48 == 0`: AI coasts or brakes with `local_48 = -0x20`.
- A hard speed cap at `carparam +0x74` (rev limiter) zeroes torque when exceeded, preventing unlimited speed regardless of rubber-band boost.

---

## 3. Tuneable Parameters Reference

### 3.1 Rubber-Band Control Globals

| Address | Type | Name | Default (ROM) | Description |
|---|---|---|---|---|
| `0x473D9C` | dword | `behind_scale` | 0x80 | Catch-up intensity when AI trails player |
| `0x473DA0` | dword | `ahead_scale` | 0x80 | Slow-down intensity when AI leads player |
| `0x473DA4` | dword | `behind_range` | 0x80 | Max span distance (behind) for full effect |
| `0x473DA8` | dword | `ahead_range` | 0x80 | Max span distance (ahead) for full effect |

Note: ROM defaults (0x80) are overwritten by `InitializeRaceActorRuntime` at race start.

### 3.2 Throttle Bias Arrays

| Address | Type | Name | Description |
|---|---|---|---|
| `0x473D64` | dword[6+] | Default throttle table | Per-slot baseline throttle (race-init constant) |
| `0x473D2C` | dword[6+] | Live throttle table | Copied from default each tick, then rubber-band modified |
| `gActorDefaultRouteSteerBias` | dword[6] (stride 0x47 dwords) | Per-actor throttle output | Final rubber-banded throttle consumed by route threshold state |

### 3.3 AI Physics Template

| Address | Size | Name | Description |
|---|---|---|---|
| `0x473DB0` | 128 bytes | AI physics template | Copied to each AI actor at race init; contains base tuning |

Key offsets within the template (at `puVar3` in init code):

| Offset | Size | Field | What it controls |
|---|---|---|---|
| `+0x68` (0x1A*4) | short | Steering factor | AI cornering aggressiveness; difficulty-scaled |
| `+0x2C` (0x0B*4) | short | Base grip/traction | AI grip factor; difficulty-scaled |
| `+0x6E` | short | Brake force | Braking deceleration; hard-mode scaled |
| `+0x70` (0x1C*4) | short | Low-speed brake coefficient | Set to 1000 in most configurations |
| `+0x74` (0x1D*4) | short | Max speed / rev limiter | Tier-dependent; ranges 0x3A1-0x433 |

### 3.4 Difficulty Globals

| Address | Name | Used by |
|---|---|---|
| `gDifficultyEasy` | Easy mode flag | `InitializeRaceActorRuntime` |
| `gDifficultyHard` | Hard mode flag | `InitializeRaceActorRuntime` |
| `gRaceDifficultyTier` | Tier 0/1/2 | `InitializeRaceActorRuntime`, `AdjustCheckpointTimersByDifficulty` |
| `gTrackIsCircuit` | Circuit vs point-to-point | `InitializeRaceActorRuntime` |
| `gTrafficActorsEnabled` | Traffic on/off | `InitializeRaceActorRuntime` |
| `g_raceOverlayPresetMode` | Special mode (4=Pitbull) | `InitializeRaceActorRuntime` |
| `g_selectedGameType` | 0=race, else=time trial | `InitializeRaceActorRuntime`, `ComputeAIRubberBandThrottle` |
| `g_networkRaceActive` | Network flag | `ComputeAIRubberBandThrottle` |

### 3.5 Two-Player Steering Weight Balance

| Address | Type | Name | Description |
|---|---|---|---|
| `0x46317C` | short | Player 1 steering weight | Increases when P1 is behind; floor 0x100 |
| `0x46317E` | short | Player 2 steering weight | Increases when P2 is behind; floor 0x100 |
| `0x48301C` | dword | Max steering weight delta | Clamps the maximum weight adjustment; default 0 in ROM |

---

## 4. Difficulty Scaling -- Complete Decision Tree

### 4.1 First Layer: Global Difficulty

Applied to the AI physics template before any mode/tier branching:

| Difficulty | Steering scale | Grip scale | Brake scale | Extra |
|---|---|---|---|---|
| Easy | No change | No change | No change | -- |
| Normal | `* 0x168/256` (88%) | `* 300/256` (117%) | No change | -- |
| Hard | `* 0x28A/256` (255%) | `* 0x17C/256` (148%) | `* 0x1C2/256` (177%) | Low-speed grip `* 400/256` |

Hard mode AI has 2.55x the steering factor and 1.48x the grip of Easy. This makes AI cornering dramatically faster on Hard.

### 4.2 Second Layer: Mode/Circuit/Traffic/Tier Matrix

Complete table of rubber-band parameters. Top-speed scale applies to the AI physics template steering field at `+0x68`. The grip field at `+0x2C` is also scaled in some configurations. Rev limiter (`+0x74`) is set to 0x3A1 for tiers 0-1 and 0x433 for tier 2 in most modes.

#### Point-to-Point, No Traffic

| Tier | Top speed scale | Grip scale | behind_scale | behind_range | ahead_scale | ahead_range |
|---|---|---|---|---|---|---|
| 0 | 0xAA/256 (67%) | 0x100/256 (100%) | 0xA0 (160) | 100 | 0x96 (150) | 0x50 (80) |
| 1 | 0xB4/256 (70%) | 0x100/256 (100%) | 0xC8 (200) | 0x4B (75) | 0xC0 (192) | 0x4B (75) |
| 2 | 0xDC/256 (86%) | 0x10E/256 (106%) | 0x10E (270) | 0x41 (65) | 0x96 (150) | 0x50 (80) |

#### Point-to-Point, With Traffic

| Tier | Top speed scale | Grip scale | behind_scale | behind_range | ahead_scale | ahead_range |
|---|---|---|---|---|---|---|
| 0 | 0xB4/256 (70%) | 0x100/256 (100%) | 0xB4 (180) | 0x4B (75) | 0xBE (190) | 100 |
| 1 | 0xBE/256 (74%) | 0x10E/256 (106%) | 0xC8 (200) | 0x3C (60) | 0xBE (190) | 100 |
| 2 | 0xDC/256 (86%) | 0x122/256 (113%) | 0xDC (220) | 0x3C (60) | 100 | 0x40 (64) |

#### Circuit

| Tier | Top speed scale | Grip scale | behind_scale | behind_range | ahead_scale | ahead_range |
|---|---|---|---|---|---|---|
| 0 | 0x91/256 (57%) | 0xC8/256 (78%) | 0x8C (140) | 100 | 0xC8 (200) | 0x37 (55) |
| 1 | 0xA0/256 (63%) | 0xEC/256 (92%) | 0x96 (150) | 100 | 0xC0 (192) | 0x40 (64) |
| 2 | 0xC3/256 (76%) | 0x104/256 (101%) | 0xC8 (200) | 100 | 0x78 (120) | 0x40 (64) |

#### Pitbull / Mode 4

| Top speed scale | Grip scale | behind_scale | behind_range | ahead_scale | ahead_range |
|---|---|---|---|---|---|
| 0x91/256 (57%) | 0xB9/256 (72%) | 0x8C (140) | 100 | 0xC0 (192) | 0x40 (64) |

#### Time Trial (`g_selectedGameType != 0`)

| behind_scale | behind_range | ahead_scale | ahead_range |
|---|---|---|---|
| 0 | 0x40 | 0 | 0x40 |

**Rubber-banding is completely disabled** in Time Trial mode. Both scales are zero, so the modifier is always zero and all AI run at flat 0x100 throttle.

### 4.3 Interpretation

**Tier progression** (0 -> 1 -> 2):
- AI top speed increases: ~67% -> ~70% -> ~86% of template base
- Catch-up (`behind_scale`) increases: ~160 -> ~200 -> ~270 (tier 2 no-traffic)
- Catch-up range (`behind_range`) **decreases**: 100 -> 75 -> 65 spans -- meaning the effect ramps up faster over shorter distances
- Ahead slow-down varies per mode but generally the AI is more reluctant to slow down at higher tiers

**Circuit vs Point-to-Point:**
- Circuit races have **lower** AI top speed (57-76% vs 67-86%)
- Circuit races have **stronger slow-down** when AI is ahead (ahead_scale up to 200)
- This keeps the pack tighter on closed circuits where lapping would be obvious
- Point-to-point races allow more spread since there is no lapping

**Traffic presence:**
- Traffic races have slightly higher AI top speed at tiers 0-1
- Catch-up is tighter (smaller range, similar scale) to compensate for traffic-induced delays
- Ahead slow-down is weaker (larger range = slower ramp) since traffic naturally impedes leaders

---

## 5. The Steering Weight Balance System (Two-Player)

`UpdatePlayerSteeringWeightBalance` (0x4036B0) is called only when `g_twoPlayerModeEnabled != 0` and implements a **split-screen catch-up mechanic for the trailing human player**.

### Algorithm

```c
void UpdatePlayerSteeringWeightBalance(void) {
    int p1_span = (short)g_actorRuntimeState.slot[0].span;   // Player 1 track span
    int p2_span = (short)g_actorRuntimeState.slot[1].span;   // Player 2 track span

    if (p1_span < p2_span) {
        // Player 1 is behind
        int delta = (p2_span - p1_span) * 2;
        if (delta > DAT_0048301C) delta = DAT_0048301C;   // clamp
        weight_P1 = 0x100 + (short)delta;   // boost trailing player
        weight_P2 = 0x100 - (short)delta;   // penalize leading player
    } else {
        // Player 2 is behind (or tied)
        int delta = (p1_span - p2_span) * 2;
        if (delta > DAT_0048301C) delta = DAT_0048301C;
        weight_P1 = 0x100 - (short)delta;
        weight_P2 = 0x100 + (short)delta;
    }
}
```

### Key Properties

- The weight is stored at `0x46317C` (P1) and `0x46317E` (P2) as shorts
- The delta is `2 * span_difference`, so the effect ramps at double the rate of the AI rubber-band
- `DAT_0048301C` caps the maximum delta (default 0 in ROM, meaning this system is **inactive by default** unless initialized elsewhere)
- The weights are consumed by `UpdatePlayerVehicleControlState` when bit `0x200` is set in the control flags, where they modulate the lane/encounter control output at `DAT_004AB446`
- This is **separate** from the AI rubber-band system -- it only affects the two human players' steering responsiveness, not their engine power

### Design Intent

In two-player split-screen, the trailing player gets slightly more responsive steering (easier to corner) while the leading player gets slightly less responsive steering. This is a subtle catch-up that affects handling rather than raw speed, making it less perceptible than the AI's throttle-based rubber-banding.

---

## 6. Race Order / Progression System Integration

### Position Tracking

`UpdateRaceOrder` (0x42F5B0) runs after `UpdateRaceActors` every tick. It maintains `g_raceOrderTable` (byte array, 6 entries) as a sorted list of slot indices ordered by race position.

Sorting is by `field_0x86` (a per-actor progress metric combining span count and lap count), using bubble sort. It writes the resulting position back to each actor at `field_0x383`.

### How Rubber-Banding Uses Position

The rubber-band system does **not** use the race order table at all. It uses raw **span distance** between the AI and player 0. This means:

1. The rubber-band effect is purely distance-based, not position-based
2. An AI 50 spans behind in 2nd place gets the same boost as an AI 50 spans behind in 6th place
3. On circuit tracks, this can cause issues: an AI one full lap behind appears to be "ahead" in span space (wrapping). The span distance clamp (`behind_range` / `ahead_range`) mitigates this by capping the effect.

### Checkpoint Timer Difficulty Adjustment

`AdjustCheckpointTimersByDifficulty` (0x40A530) modifies checkpoint time limits based on `gRaceDifficultyTier`:

| Tier | Time multiplier |
|---|---|
| 0 (Easy) | `* 12/10` (120% -- more time) |
| 1 (Medium) | `* 11/10` (110% -- slightly more time) |
| 2 (Hard) | No change (base times) |

This applies to the initial checkpoint time (`g_raceCheckpointTablePtr + 2`) and all subsequent checkpoint intervals (every 4 bytes starting at offset 6). The function is called from `InitializeRaceVehicleRuntime` (0x42F140) during race setup.

A special case: if `actor +0x375 != 0` (a flag likely indicating the actor is not the primary player), checkpoint adjustment is skipped entirely.

---

## 7. Interactions and Emergent Behavior

### Rubber-Band + Lateral Offset = Pack Racing

The AI has no explicit "overtaking" logic. Instead, two systems combine:

1. **Rubber-band throttle** keeps all AI near the player in terms of span progress
2. **`UpdateActorTrackOffsetBias`** (0x434900) shifts AI laterally when a peer is ahead within 0x28 spans
3. **`FindActorTrackOffsetPeer`** (0x4337E0) searches for the nearest same-route actor ahead with overlapping lateral bounds

The lateral shift decays at 8 units/tick when no peer is detected, and increases by `(0x29 - spanDistance)` per tick when a peer blocks the path. The direction of shift depends on whether the peer occupies the inner or outer lane relative to the track center. A global at `DAT_004B08B0` tracks the avoidance direction state.

This produces "natural" overtaking: a rubber-banded AI approaches from behind, detects the actor ahead, shifts laterally, and passes alongside. No decision tree, no racing line optimization.

### Rubber-Band + Grip Asymmetry

The AI's 2x grip advantage (clamped to [0x70, 0xA0] vs player's [0x38, 0x50]) is **independent** of rubber-banding. Even at minimum rubber-band throttle, the AI corners faster than the player due to the physics model difference. The rubber-band primarily affects straight-line speed / acceleration.

### Difficulty Stacking

The total AI advantage on Hard Tier 2 stacks multiplicatively:

1. **Steering**: template value * 2.55 (hard) * 0.86 (tier 2 speed scale) = ~2.19x
2. **Grip**: template value * 1.48 (hard) * 1.06-1.13 (tier 2 grip scale) = ~1.57-1.67x
3. **Catch-up**: behind_scale up to 270 with range 65 = very aggressive catch-up over ~65 spans
4. **Checkpoint timers**: No bonus time (100% base)

On Easy Tier 0 with no traffic:
1. **Steering**: template value * 1.0 * 0.67 = 0.67x (AI slower than template)
2. **Grip**: template value * 1.0 * 1.0 = 1.0x
3. **Catch-up**: behind_scale 160 with range 100 = moderate catch-up
4. **Checkpoint timers**: 120% bonus time

---

## 8. Global Variable Address Map

| Address | Size | Name | Written by | Read by |
|---|---|---|---|---|
| `0x473D2C` | dword[14] | Live throttle array | `ComputeAIRubberBandThrottle` | `ComputeAIRubberBandThrottle` |
| `0x473D64` | dword[14] | Default throttle array | Race init / ROM | `ComputeAIRubberBandThrottle` |
| `0x473D9C` | dword | `behind_scale` | `InitializeRaceActorRuntime` | `ComputeAIRubberBandThrottle` |
| `0x473DA0` | dword | `ahead_scale` | `InitializeRaceActorRuntime` | `ComputeAIRubberBandThrottle` |
| `0x473DA4` | dword | `behind_range` | `InitializeRaceActorRuntime` | `ComputeAIRubberBandThrottle` |
| `0x473DA8` | dword | `ahead_range` | `InitializeRaceActorRuntime` | `ComputeAIRubberBandThrottle` |
| `0x473DB0` | 128 bytes | AI physics template | ROM | `InitializeRaceActorRuntime` |
| `0x46317C` | short | P1 steering weight | `UpdatePlayerSteeringWeightBalance` | `UpdatePlayerVehicleControlState` |
| `0x46317E` | short | P2 steering weight | `UpdatePlayerSteeringWeightBalance` | `UpdatePlayerVehicleControlState` |
| `0x48301C` | dword | Max steering weight delta | Unknown init | `UpdatePlayerSteeringWeightBalance` |
| `0x4B08B0` | dword | Lateral avoidance direction | `FindActorTrackOffsetPeer` | `UpdateActorTrackOffsetBias` |
| `gActorDefaultRouteSteerBias` | dword (stride 0x11C) | Per-actor throttle bias | `ComputeAIRubberBandThrottle` | `UpdateActorRouteThresholdState` |
| `gActorForwardTrackComponent` | dword (stride 0x11C) | Forward speed projection | `UpdateRaceActors` | `UpdateActorRouteThresholdState` |
| `gRaceDifficultyTier` | dword | Race tier 0/1/2 | Frontend | `InitializeRaceActorRuntime`, `AdjustCheckpointTimersByDifficulty` |
| `g_raceCheckpointTablePtr` | ptr | Checkpoint time table | Level load | `AdjustCheckpointTimersByDifficulty` |
| `g_networkRaceActive` | dword | Network mode flag | Network init | `ComputeAIRubberBandThrottle` |

---

## 9. Summary

The TD5 rubber-banding system is a straightforward **distance-proportional throttle modifier**. It is NOT position-aware (does not use race order), NOT per-car (applies the same formula to all AI), and NOT adaptive (parameters are fixed at race init). Its apparent complexity comes entirely from the large initialization decision tree that selects four parameters based on five input variables (difficulty, tier, circuit, traffic, mode).

Key design choices:
- **Linear interpolation** with clamped range, not exponential or curve-based
- **Asymmetric**: behind/ahead have independent scale and range parameters
- **Throttle-only**: rubber-banding never affects AI steering, grip, or braking
- **Disabled in network play**: all AI run at constant throttle in multiplayer
- **Disabled in time trial**: ghost car is unaffected
- **Two-player mode** has a separate, subtler system that modifies steering weight rather than throttle
