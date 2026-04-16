# Test Drive 5: Physics Validation Test Cases

**Date:** 2026-03-19
**Binary:** TD5_d3d.exe (Ghidra port 8193)
**Purpose:** Formulas, worked examples, and testable predictions for five core systems.
All arithmetic is **fixed-point** unless stated otherwise. `>>` denotes arithmetic right shift; `/` denotes signed integer division with truncation toward zero.

---

## 1. Collision Impulse Formula

### Source: `ApplyVehicleCollisionImpulse` (0x4079C0)

The collision system uses 2D rigid-body impulse in a contact-normal-aligned frame. All values are 12-bit fixed-point (scale factor 4096).

### Step-by-Step Formula

**Inputs per contact:**
- `vel_A`, `vel_B`: linear velocities of vehicles A and B (int, 12-bit fixed)
- `omega_A`, `omega_B`: yaw angular velocities (int, at actor+0x1C4)
- `mass_A`, `mass_B`: collision mass parameters (short, from tuning_table+0x88)
- `angle`: contact angle (12-bit angle, 0-4095 = 0-360 degrees)
- `contact_record[4]`: 4 shorts = `[local_x, local_z, delta_x, delta_z]`
- `elasticity`: 0x00-0x100, from binary-search TOI refinement (typically ~0x80)
- `K = DAT_00463204 = 500,000`

**Step 1: Rotate velocities into contact-normal frame**

```
cos = CosFixed12bit(angle)    // returns cos(angle) * 4096
sin = SinFixed12bit(angle)    // returns sin(angle) * 4096

vel_A_normal  = (vel_A.x * cos - vel_A.z * sin) >> 12
vel_A_tangent = (vel_A.x * sin + vel_A.z * cos) >> 12
vel_B_normal  = (vel_B.x * cos - vel_B.z * sin) >> 12
vel_B_tangent = (vel_B.x * sin + vel_B.z * cos) >> 12
```

**Step 2: Determine contact axis and offsets**

The function picks lateral or longitudinal impulse based on which box face was penetrated (comparing contact point against the struck vehicle's half-extents). This yields two corner offset scalars `r_A` and `r_B` from the contact record.

**Step 3: Compute relative approach velocity including angular contribution**

```
// 0x28C = 652 (angular-to-linear conversion constant)
v_rel = (r_B * omega_B / 0x28C - r_A * omega_A / 0x28C)
        - vel_A_component + vel_B_component
```

Where `vel_A_component` / `vel_B_component` are the rotated velocity components along the chosen axis (normal or tangent).

**Step 4: Compute impulse magnitude**

```
denominator = (r_B^2 + K) * mass_A + (r_A^2 + K) * mass_B
numerator   = ((K + rounding) >> 8) * 0x1100    // = K/256 * 4352

impulse_raw = (numerator / (denominator >> 8)) * v_rel
impulse     = (impulse_raw + rounding) >> 12
```

**Note:** `0x1100 = 4352`. The expression `(K >> 8) * 0x1100` = `1953 * 4352 = 8,499,456`.

**Step 5: Sign guard**

If the impulse direction would pull vehicles together (sign of impulse XOR sign of closing-delta < 0), return 0 (no impulse applied).

**Step 6: Apply linear velocity changes**

```
delta_vel_A = impulse * mass_A    // A gains velocity proportional to its own inv-mass
delta_vel_B = impulse * mass_B    // B loses (or gains in opposite direction)

vel_A_component += delta_vel_A
vel_B_component -= delta_vel_B
```

**Step 7: Apply angular velocity changes**

```
K_div = K / 0x28C    // = 500000 / 652 = 767

delta_omega_A = (impulse * mass_A * r_A) / K_div
delta_omega_B = (impulse * mass_B * r_B) / K_div
```

**Step 8: Friction damping envelope**

Before impulse is applied, the function subtracts `(0x100 - elasticity)/256 * old_velocity` from positions. After the new velocities are computed, it adds back `(0x100 - elasticity)/256 * new_velocity`. This effectively scales velocities by elasticity, simulating energy loss.

**Step 9: Rotate back to world frame**

```
new_vel_A.x = (vel_A_tangent * sin + vel_A_normal * cos) >> 12
new_vel_A.z = (vel_A_tangent * cos - vel_A_normal * sin) >> 12
// same for B
```

### Absolute impact magnitude

```
impact = abs((mass_A + mass_B) * impulse)
```

This value is returned and used for sound/damage thresholds:
- `impact < 12,800`: silent
- `12,800 <= impact < 51,200`: medium collision SFX
- `51,200 <= impact`: heavy crash SFX
- `impact > 90,000` AND collisions enabled: visual damage / deformation

### Worked Example 1: Equal-mass head-on at contact angle 0

```
Setup:
  Vehicle A: vel_x = +1000 (12-bit), vel_z = 0, omega = 0, mass = 0x40 (64)
  Vehicle B: vel_x = -1000 (12-bit), vel_z = 0, omega = 0, mass = 0x40 (64)
  Contact angle = 0 (head-on along X axis)
  Contact offsets: r_A = 0, r_B = 0 (center-to-center)
  Elasticity = 0x80

Step 1: cos=4096, sin=0
  vel_A_normal = (1000*4096 - 0) >> 12 = 1000
  vel_B_normal = (-1000*4096 - 0) >> 12 = -1000

Step 3: v_rel = (0 - 0) - 1000 + (-1000) = -2000

Step 4:
  denominator = (0 + 500000)*64 + (0 + 500000)*64 = 64,000,000
  numerator = (500000 >> 8) * 4352 = 1953 * 4352 = 8,499,456
  impulse_raw = (8,499,456 / (64000000 >> 8)) * (-2000)
              = (8,499,456 / 250000) * (-2000)
              = 33 * (-2000) = -66,000
  impulse = -66000 >> 12 = -16

Step 6:
  delta_vel_A = -16 * 64 = -1024
  delta_vel_B = -16 * 64 = -1024
  A: vel_normal = 1000 + (-1024) = -24  (slight rebound)
  B: vel_normal = -1000 - (-1024) = +24  (slight rebound)

Impact = abs((64+64) * (-16)) = 2048
  -> Below 12,800: SILENT collision
```

**Prediction:** Two equal-mass vehicles at speed 1000 (fixed-point) in a perfect head-on produce a near-elastic collision with both vehicles nearly stopping, and no collision sound effect plays.

### Worked Example 2: Asymmetric mass, offset contact

```
Setup:
  Vehicle A (heavy SUV): vel_x = +2000, omega = 0, mass = 0x60 (96)
  Vehicle B (light car):  vel_x = -500, omega = 0, mass = 0x30 (48)
  Contact angle = 0
  r_A = 200 (B strikes A's front-right corner)
  r_B = 0
  Elasticity = 0x80

Step 3: v_rel = (0 - 200*0/652) - 2000 + (-500) = -2500

Step 4:
  denominator = (0 + 500000)*96 + (200^2 + 500000)*48
              = 48,000,000 + (40000 + 500000)*48
              = 48,000,000 + 25,920,000 = 73,920,000
  numerator = 8,499,456
  impulse_raw = (8,499,456 / (73920000>>8)) * (-2500)
              = (8,499,456 / 288750) * (-2500)
              = 29 * (-2500) = -72,500
  impulse = -72500 >> 12 = -17

Step 6 (linear):
  A: vel += (-17) * 96 = -1632   -> 2000 - 1632 = 368
  B: vel -= (-17) * 48 = +816    -> -500 + 816 = 316

Step 7 (angular, A only since r_B=0):
  K_div = 767
  delta_omega_A = (-17 * 96 * 200) / 767 = -326,400 / 767 = -425

Impact = abs((96+48) * (-17)) = 2448
  -> Below 12,800: SILENT

Spin applied to A: omega_A = -425 (clockwise yaw deflection from corner hit)
```

**Prediction:** Heavy vehicle A barely slows (2000 -> 368), light vehicle B reverses direction (-500 -> +316). Vehicle A acquires yaw spin from the offset hit. No sound plays.

### Worked Example 3: High-speed side-swipe

```
Setup:
  Vehicle A: vel_x = +5000, vel_z = +200, omega = 0, mass = 0x40 (64)
  Vehicle B: vel_x = +4500, vel_z = -100, omega = 0, mass = 0x40 (64)
  Contact angle = 1024 (90 degrees, lateral contact)
  r_A = 300 (side hit at wing), r_B = 300
  Elasticity = 0x70 (slightly inelastic from TOI refinement)

Step 1: cos=0, sin=4096  (at 90 degrees)
  vel_A_normal = (5000*0 - 200*4096) >> 12 = -200
  vel_A_tangent = (5000*4096 + 200*0) >> 12 = 5000
  vel_B_normal = (4500*0 + 100*4096) >> 12 = 100
  vel_B_tangent = (4500*4096 - 100*0) >> 12 = 4500

Step 3 (tangent axis chosen for lateral contact):
  v_rel = (300*0/652 - 300*0/652) - 5000 + 4500 = -500

Step 4:
  denominator = (300^2 + 500000)*64 + (300^2 + 500000)*64
              = (90000+500000)*64 * 2 = 75,520,000
  impulse_raw = (8,499,456 / 295000) * (-500) = 28 * (-500) = -14,000
  impulse = -14000 >> 12 = -3

Impact = abs(128 * (-3)) = 384
  -> Below 12,800: SILENT (just a gentle side-swipe scrape)
```

**Prediction:** In a same-direction side-swipe where both vehicles travel roughly the same speed, the impulse is minimal. Both vehicles continue forward with small lateral perturbation.

---

## 2. Physics Difficulty Scaling

### Source: `InitializeRaceVehicleRuntime` (0x42F140)

Three difficulty levels apply **multiplicative scaling** to physics table fields. All scaling uses the pattern `val = (val * scale + rounding) >> 8` where scale is an 8.8 fixed-point multiplier.

### Difficulty Parameter Comparison Table

| Physics Field | Offset | Easy | Normal | Hard |
|---|---|---|---|---|
| Gravity constant (`gGravityConstant`) | global | **0x5DC** (1500) | **0x76C** (1900) | **0x800** (2048) |
| `drive_torque_multiplier` | +0x68 | **1.00x** (no scale) | **1.41x** (360/256) | **2.54x** (650/256) |
| `speed_scale_factor` | +0x78 | **1.00x** (no scale) | **2x** (<<1) | **4x** (<<2) |
| `drag_coefficient` | +0x2C | **1.00x** (no scale) | **1.17x** (300/256) | **1.48x** (380/256) |
| `brake_force` | +0x6E | **1.00x** (no scale) | **1.00x** (no scale) | **1.76x** (450/256) |
| `engine_brake_force` | +0x70 | **1.00x** (no scale) | **1.00x** (no scale) | **1.56x** (400/256) |

### What each parameter does in practice

| Parameter | Gameplay Effect |
|---|---|
| Gravity | Higher = stronger downforce, faster cornering grip recovery, less air time off bumps |
| Drive torque | Acceleration force. 2.54x on Hard means cars accelerate 2.5x faster |
| Speed scale | Likely affects speedometer display or RPM-to-speed conversion. 4x on Hard |
| Drag | Higher = more air resistance at speed. Counterbalances the higher torque on Normal/Hard |
| Brake force | Higher = stronger braking. Only boosted on Hard (1.76x) |
| Engine brake | Deceleration when off-throttle. Only boosted on Hard (1.56x) |

### AI-Specific Scaling (InitializeRaceActorRuntime 0x432E60)

The AI uses a **separate physics table** copied from the defaults at 0x473DB0. In addition to the same Easy/Normal/Hard torque and drag scaling as the player, the AI table gets:

**Hard mode only (extra on top of player scaling):**
| AI Field | Offset | Scale |
|---|---|---|
| `brake_force` (AI) | +0x6E | **1.76x** (450/256) |
| `engine_brake_force` (AI) | +0x70 | **1.56x** (400/256) |

These are the same values as the player gets on Hard, applied to the AI's own physics table.

### Per-Race-Tier AI Torque Scaling

Beyond difficulty, the AI drive torque (+0x68) and drag (+0x2C) get additional race-tier-specific scaling from `InitializeRaceActorRuntime`:

| Circuit? | Traffic? | Tier | Torque Scale | Drag Scale |
|---|---|---|---|---|
| Yes (circuit) | -- | 0 | 0x91/256 = 0.57x | 200/256 = 0.78x |
| Yes | -- | 1 | 0xA0/256 = 0.63x | 0xEC/256 = 0.92x |
| Yes | -- | 2 | 0xC3/256 = 0.76x | 0x104/256 = 1.02x |
| No (P2P) | No | 0 | 0xAA/256 = 0.66x | 0x100/256 = 1.00x |
| No | No | 1 | 0xB4/256 = 0.70x | 0x100/256 = 1.00x |
| No | No | 2 | 0xDC/256 = 0.86x | 0x10E/256 = 1.05x |
| No | Yes | 0 | 0xB4/256 = 0.70x | 0x100/256 = 1.00x |
| No | Yes | 1 | 0xBE/256 = 0.74x | 0x10E/256 = 1.05x |
| No | Yes | 2 | 0xDC/256 = 0.86x | 0x122/256 = 1.13x |
| Drag race | -- | -- | 0x91/256 = 0.57x | 0xB9/256 = 0.72x |

**Prediction:** On a Tier 0 circuit track with Normal difficulty, AI vehicles have only 57% of the base torque but the same gravity. The rubber-band system (Section 3) compensates for this torque deficit.

### Testable Scenario: Easy vs Hard acceleration

Given a car with base `drive_torque_multiplier = 100`:
- **Easy:** torque = 100 (no scaling), gravity = 1500
- **Normal:** torque = 100 * 360/256 = 140, gravity = 1900
- **Hard:** torque = 100 * 650/256 = 253, gravity = 2048

**Prediction:** A standing-start 0-to-max-speed time should be roughly 2.5x faster on Hard vs Easy, visibly observable in-game by timing acceleration from a dead stop.

---

## 3. AI Rubber-Band Behavior

### Source: `ComputeAIRubberBandThrottle` (0x432D60) + `InitializeRaceActorRuntime` (0x432E60)

### Core Formula

For each AI actor `i`:

```
span_delta = AI_span[i] - player_span[0]    // track progress difference in spans
// (span = segment along the track; higher = further ahead)

if (span_delta < 0) {
    // AI is BEHIND the player
    clamped = max(span_delta, behind_clamp)    // clamp to negative limit
    scale = (behind_scale * clamped) / behind_clamp
} else {
    // AI is AHEAD of the player
    clamped = min(span_delta, ahead_clamp)     // clamp to positive limit
    scale = (ahead_scale * clamped) / ahead_clamp
}

throttle_multiplier[i] = 0x100 - scale    // 0x100 = 1.0 (nominal)
```

**Result:** `throttle_multiplier` is stored in the per-actor route state table and used as a multiplier on AI speed/throttle (0x100 = 100% = nominal).

### When rubber-banding is DISABLED

When `DAT_004aadb4 != 0` (Time Trial mode), all AI throttle multipliers are set to `0x100` (100%, no modification). The rubber-band system is completely inactive.

### Configuration Parameters per Race Type

The four rubber-band parameters are set by `InitializeRaceActorRuntime` based on track type, traffic presence, and difficulty tier:

| Parameter | Meaning |
|---|---|
| `behind_scale` (0x473D9C) | Max slowdown percentage when AI is behind (higher = more boost to catch up) |
| `behind_clamp` (0x473DA4) | Distance in spans at which behind-boost saturates (negative value) |
| `ahead_scale` (0x473DA0) | Max speedup penalty when AI is ahead (higher = more slowdown) |
| `ahead_clamp` (0x473DA8) | Distance in spans at which ahead-penalty saturates |

### Complete Configuration Table

| Track Type | Tier | behind_scale | behind_clamp | ahead_scale | ahead_clamp | Max Boost | Max Penalty |
|---|---|---|---|---|---|---|---|
| **Time Trial** | -- | 0 | 64 | 0 | 64 | 0% | 0% |
| **Circuit, T0** | 0 | 0x8C (140) | 100 | 200 | 55 | 55%+ | 78%+ |
| **Circuit, T1** | 1 | 0x96 (150) | 100 | 0xC0 (192) | 64 | 59%+ | 75%+ |
| **Circuit, T2** | 2 | 0xC8 (200) | 100 | 0x78 (120) | 64 | 78%+ | 47%+ |
| **P2P no-traffic, T0** | 0 | 0xA0 (160) | 100 | 0x96 (150) | 80 | 63%+ | 59%+ |
| **P2P no-traffic, T1** | 1 | 0xC8 (200) | 75 | 0xC0 (192) | 75 | 78%+ | 75%+ |
| **P2P no-traffic, T2** | 2 | 0x10E (270) | 65 | 0x96 (150) | 80 | 106%* | 59%+ |
| **P2P with-traffic, T0** | 0 | 0xB4 (180) | 75 | 0xBE (190) | 100 | 70%+ | 74%+ |
| **P2P with-traffic, T1** | 1 | 0xC8 (200) | 60 | 0xBE (190) | 100 | 78%+ | 74%+ |
| **P2P with-traffic, T2** | 2 | 0xDC (220) | 60 | 100 | 64 | 86%+ | 39%+ |
| **Drag race** | -- | 0x8C (140) | 100 | 0xC0 (192) | 64 | 55%+ | 75%+ |

*"Max Boost" = `behind_scale / 256 * 100%` = how much the throttle multiplier can exceed 1.0 when AI is maximally behind. "Max Penalty" = `ahead_scale / 256 * 100%` = how much throttle is reduced when AI is maximally ahead.

**NOTE:** When `behind_scale > 0x100` (256), the formula can produce `throttle_multiplier > 0x200` (2.0x), meaning the AI gets more than double throttle. P2P no-traffic Tier 2 with `behind_scale = 270` can theoretically produce a multiplier of `0x100 + 270 = 0x20E` = **206% throttle**.

### Worked Example A: Circuit Tier 0, AI is 50 spans behind

```
behind_scale = 0x8C = 140
behind_clamp = 100

span_delta = -50  (AI is 50 spans behind player)
clamped = max(-50, -100) = -50  (within clamp range, so not clamped)
scale = (140 * (-50)) / (-100) = 70

throttle_multiplier = 0x100 - 70 = 0x100 + 70 = 326 (unsigned)
Wait -- let's be precise about signs:
  scale = (140 * (-50)) / (-100) = (-7000) / (-100) = +70

throttle_multiplier = 0x100 - 70 = 256 - 70 = 186 = 0xBA

Hmm, that's LESS than 1.0. Let me re-derive from the assembly.
```

**Re-derivation from disassembly (0x432DEE-0x432E39):**

```asm
SUB ECX, EAX        ; ECX = AI_span - player_span (= -50)
JNS positive         ; jump if >= 0
; -- AI is behind --
MOV EAX, [behind_clamp]   ; behind_clamp = -100 (stored as negative? Let's check)
```

Wait -- let me re-examine. The data at 0x473DA4 contains `0x80 = 128` in the initial state (from the hexdump at 0x473D9C: `80 00 00 00 80 00 00 00 80 00 00 00 80 00 00 00`). But `InitializeRaceActorRuntime` writes specific signed values per tier.

Looking at the decompiler output more carefully:

```c
if (iVar1 < 0) {
    // AI behind player
    if (DAT_00473da4 < iVar1) {    // behind_clamp is NEGATIVE
        iVar1 = DAT_00473da4;       // clamp delta to behind_clamp
    }
    iVar1 = (DAT_00473d9c * iVar1) / DAT_00473da4;
} else {
    // AI ahead of player
    if (DAT_00473da8 < iVar1) {    // ahead_clamp is POSITIVE
        iVar1 = DAT_00473da8;       // clamp delta to ahead_clamp
    }
    iVar1 = (DAT_00473da0 * iVar1) / DAT_00473da8;
}
throttle_multiplier = 0x100 - iVar1;
```

Looking at the init code: `DAT_00473da4 = 100` (positive). The decompiler shows `if (DAT_00473da4 < iVar1)` where `iVar1 < 0`, meaning the behind_clamp is stored as a **positive** value and the comparison `100 < -50` is false, so no clamping occurs.

Actually re-reading: when AI is behind, `span_delta < 0` (e.g., -50). The code does:
```c
if (behind_clamp < span_delta)  // 100 < -50 is FALSE -> no clamp (behind_clamp stored positive but used as floor comparison)
```

Wait, this doesn't make sense with positive `behind_clamp = 100`. Let me re-examine the disassembly:

```asm
00432dfc: MOV EAX, [0x00473da4]     ; EAX = behind_clamp
00432e01: CMP ECX, EAX              ; ECX = span_delta (-50)
00432e03: JLE 0x00432e07            ; if span_delta <= behind_clamp, use behind_clamp
00432e05: MOV ECX, EAX              ; clamp: ECX = behind_clamp
```

So: if `span_delta (-50) <= behind_clamp (100)` -> yes, always true when behind_clamp is positive and span_delta is negative -> ECX = behind_clamp!

Wait, that means `clamped = behind_clamp` always when `span_delta < 0` and `behind_clamp > 0`. That can't be right.

Let me re-read: `JLE` means jump if less-or-equal. If `-50 <= 100`, YES -> jump to 0x432e07 which SKIPS `MOV ECX, EAX`. So the clamping line is NOT executed and ECX remains as span_delta (-50).

Correction: JLE jumps to 0x00432e07, and the MOV at 0x00432e05 is the clamp (only reached when JLE is NOT taken). So:
- If `span_delta <= behind_clamp`: use span_delta as-is (no clamp)
- If `span_delta > behind_clamp`: clamp to behind_clamp

Since span_delta is negative (e.g., -50) and behind_clamp is positive (100), the condition `-50 <= 100` is TRUE -> no clamping -> use -50.

But this means the behind_clamp as stored is a signed comparison. When would clamping trigger? When the AI is somehow AHEAD by more than `behind_clamp` spans... but we're in the `span_delta < 0` branch!

Ah wait, I see -- the `InitializeRaceActorRuntime` code for the "behind" clamp actually stores signed values. Looking at it as written to 0x473DA4:

For Circuit T0: `DAT_00473da4 = 100` -- this is clearly a positive magnitude, but in context the comparison `JLE` with a negative span_delta means clamping would only occur if the AI were MORE than 100 spans behind (span_delta < -100 conceptually requires a negative clamp value).

Actually, on second look at the decompiler: `if (DAT_00473da4 < iVar1)` -- this is `behind_clamp < span_delta`. When `behind_clamp = 100` and `span_delta = -50`: `100 < -50` is FALSE. When `behind_clamp = 100` and `span_delta = -200`: `100 < -200` is FALSE. So the clamp NEVER triggers for negative deltas with positive behind_clamp!

But looking at `iVar1 = (DAT_00473d9c * iVar1) / DAT_00473da4`:
- `scale = (140 * (-50)) / 100 = -7000 / 100 = -70`
- `throttle_multiplier = 0x100 - (-70) = 256 + 70 = 326`

That produces **326/256 = 1.27x** boost. And at the maximum delta where clamping applies (it never does, so unbounded):
- At -100 spans: scale = (140 * -100)/100 = -140, throttle = 256+140 = **396/256 = 1.55x**
- At -200 spans: scale = (140 * -200)/100 = -280, throttle = 256+280 = **536/256 = 2.09x**

The behind_clamp appears to be a **denominator** that controls the rate of boost per span, not a hard cap!

For the "ahead" branch: `iVar1 = (ahead_scale * span_delta) / ahead_clamp` with positive values:
- AI 50 spans ahead on Circuit T0: scale = (200 * 50)/55 = 181, throttle = 256-181 = **75/256 = 0.29x**
- AI 55 spans ahead: scale = (200 * 55)/55 = 200, throttle = 256-200 = **56/256 = 0.22x**
- AI 100 spans ahead: scale = (200 * 55)/55 = 200 (clamped to ahead_clamp=55), throttle = 56 = **0.22x**

Wait, re-checking the ahead clamp from the decompiler:
```c
if (DAT_00473da8 < iVar1) {   // if ahead_clamp < span_delta
    iVar1 = DAT_00473da8;      // clamp delta to ahead_clamp
}
```
This IS a proper cap: if AI is more than `ahead_clamp` spans ahead, delta is clamped.

### Corrected Worked Examples

**Example A: Circuit Tier 0, AI is 50 spans behind**

```
behind_scale = 140, behind_clamp = 100 (denominator/rate)
span_delta = -50

scale = (140 * (-50)) / 100 = -70
throttle = 0x100 - (-70) = 326/256 = 1.273x = 127.3% throttle

PREDICTION: AI gets 27% extra throttle, accelerating noticeably faster.
```

**Example B: Circuit Tier 0, AI is 40 spans ahead**

```
ahead_scale = 200, ahead_clamp = 55
span_delta = +40 (< 55, not clamped)

scale = (200 * 40) / 55 = 145
throttle = 0x100 - 145 = 111/256 = 0.434x = 43.4% throttle

PREDICTION: AI at only 43% throttle, dramatically slowing down.
```

**Example C: Circuit Tier 0, AI is 60 spans ahead (past clamp)**

```
ahead_clamp = 55, so span_delta clamped to 55
scale = (200 * 55) / 55 = 200
throttle = 0x100 - 200 = 56/256 = 0.219x = 21.9% throttle

PREDICTION: Maximum penalty reached -- AI crawls at ~22% speed.
```

**Example D: P2P no-traffic Tier 2, AI is 100 spans behind**

```
behind_scale = 270, behind_clamp = 65
scale = (270 * (-100)) / 65 = -415
throttle = 0x100 - (-415) = 671/256 = 2.62x = 262% throttle

PREDICTION: Extreme catch-up. AI gets 2.6x normal throttle at this distance.
This is the most aggressive rubber-band configuration in the game.
```

### Summary Curve

For Circuit Tier 0 (behind_scale=140, behind_clamp=100, ahead_scale=200, ahead_clamp=55):

```
Spans behind player | Throttle multiplier
        -100        |  1.55x (155%)
         -50        |  1.27x (127%)
         -25        |  1.14x (114%)
           0        |  1.00x (100%) -- nominal
         +25        |  0.64x (64%)
         +40        |  0.43x (43%)
         +55+       |  0.22x (22%) -- capped
```

### Testable Prediction

**In-game test:** On a circuit (Tier 0), let the AI get a full-lap lead (~200+ spans ahead). The AI's throttle multiplier will be capped at 22% of nominal. The AI cars should be visibly crawling, taking corners at near-idle speed. The player should be able to catch up within 1-2 laps even at moderate speed. Conversely, if the player gets far behind, AI opponents will speed up significantly but not beyond ~1.5x on this tier.

---

## 4. Weather Particle Density

### Source: `UpdateAmbientParticleDensityForSegment` (0x4464C2)

### Data Structures

Each track's LEVELINF.DAT contains:
- `config+0x28`: weather type (0=rain, 1=snow, 2=clear)
- `config+0x2C`: density pair count (int)
- `config+0x36`: array of (segment_id, density) halfword pairs

### Algorithm

```c
void UpdateAmbientParticleDensityForSegment(int actor, int view_index) {
    current_segment = actor.span_index;   // actor+0x80 (short)
    target_density = &DAT_004c3dd8[view_index];
    active_count   = &DAT_004c3de0[view_index];

    // Walk all density pairs; last match wins
    for (i = 0; i < config->pair_count; i++) {
        if (current_segment == pairs[i].segment_id) {
            new_density = pairs[i].density;
            if (new_density > 0x80) new_density = 0x80;  // cap at 128
            *target_density = new_density;
        }
    }

    // Ramp active count toward target
    if (*target_density != 0 && *active_count < *target_density) {
        // Activate one more particle: zero its timer field
        particle_buffer[*active_count].timer = 0;
        (*active_count)++;
    }
    if (*active_count > *target_density) {
        (*active_count)--;   // Deactivate one particle
    }
}
```

### Key Characteristics

1. **Maximum density: 128 particles** (capped at 0x80)
2. **Step-function transitions**: Density only changes when the vehicle crosses an exact segment_id boundary. No interpolation between zones.
3. **Gradual ramp**: Active particle count changes by **+/- 1 per frame** (at 30 FPS, 128 particles takes ~4.3 seconds to fully spawn)
4. **Last-match-wins**: If multiple density pairs match the same segment, the last one in the array takes effect

### Level003 (Snow Track) Example

Density pairs:
- Pair 0: segment 0, density 2400 -> **clamped to 128**
- Pair 1: segment 128, density 0

**Scenario: Driving forward through segment boundaries**

```
Segment 0   -> target = min(2400, 128) = 128
Segment 1   -> no match -> target stays 128
...
Segment 127 -> no match -> target stays 128
Segment 128 -> target = 0
Segment 129 -> no match -> target stays 0
...
(If track loops, segment 0 again -> target = 128)
```

### Timing Prediction

```
At segment 0 (race start):
  target_density = 128
  active_count starts at 0
  Ramp rate: +1 per frame, 30 fps
  Time to full density: 128/30 = 4.27 seconds

At segment 128 (zone transition):
  target_density = 0
  active_count = 128 (assuming full)
  Ramp rate: -1 per frame
  Time to zero: 128/30 = 4.27 seconds
```

### Note on Snow (CUT FEATURE)

Although the density system is fully functional for level003, the rendering function `RenderAmbientParticleStreaks` (0x446560) has a hard gate `if (weather_type == 0)` that skips ALL rendering when weather_type != 0. Since snow is type 1, **no particles are ever drawn on level003** despite the density system tracking them correctly.

### Testable Prediction (Rain Tracks Only)

On rain tracks (level001, level023, level030), which have 6 density pairs each:
1. Rain particle count should be observable to increase gradually over ~4 seconds from race start
2. Driving through different track zones should cause particle count to step-change (gradually ramping to new target)
3. Maximum visible particles at any point: 128
4. In 2-player split-screen, each view has independent density tracking (separate view_index 0 and 1)

---

## 5. Steering Response Curves

### Source: `UpdatePlayerVehicleControlState` (0x402E60)

### Two Independent Paths

The function has completely separate logic for digital (keyboard) and analog (joystick) input, selected by bit 31 of the control bitmask.

### Path A: Digital Steering (Keyboard)

#### 5.1 Speed-Dependent Steering Rate

```c
raw_speed = abs(velocity >> 8);         // 8-bit downshift of forward velocity
speed_sq = (speed_display >> 8);        // additional speed term (from actor+0x314)

// Compute denominator for turn rate
if (stopped) {
    turn_rate_factor = 0;               // infinite steering when stopped
} else {
    turn_rate_factor = (raw_speed * 0x40) / (speed_sq * speed_sq + 0x40);
}

steer_rate = 0xC0000 / (turn_rate_factor + 0x40);
```

**Data points (steering rate vs speed):**

| raw_speed | speed_sq | turn_rate_factor | steer_rate | Normalized |
|---|---|---|---|---|
| 0 (stopped) | 0 | 0 | 0xC0000/64 = 12288 | 1.000 |
| 50 | 5 | 50*64/(25+64) = 35 | 0xC0000/99 = 7952 | 0.647 |
| 100 | 10 | 100*64/(100+64) = 39 | 0xC0000/103 = 7605 | 0.619 |
| 200 | 20 | 200*64/(400+64) = 27 | 0xC0000/91 = 8598 | 0.700 |
| 500 | 50 | 500*64/(2500+64) = 12 | 0xC0000/76 = 10315 | 0.839 |
| 1000 | 100 | 1000*64/(10000+64) = 6 | 0xC0000/70 = 11208 | 0.912 |
| 2000 | 200 | 2000*64/(40000+64) = 3 | 0xC0000/67 = 11680 | 0.950 |
| 5000 | 500 | 5000*64/(250000+64) = 1 | 0xC0000/65 = 12098 | 0.985 |

**Note:** The `speed_sq` value depends on `actor+0x314` (body-frame forward speed >> 8), while `raw_speed` comes from `actor+0x310+4` (world-frame speed >> 8). These can differ during slides.

The curve is non-linear: steering rate starts high when stopped, dips in the mid-speed range (50-200), then gradually recovers toward the stopped rate at very high speed. This unusual behavior comes from the `speed * 64 / (speed_sq^2 + 64)` factor that peaks and decays.

#### 5.2 Speed-Dependent Maximum Steering Angle

```c
steer_limit = 0xC00000 / (raw_speed + 0x80);
if (steer_limit > 0x18000) steer_limit = 0x18000;  // absolute max = 98304
```

**Data points:**

| raw_speed | steer_limit | Degrees (approx) | Clamped? |
|---|---|---|---|
| 0 | 0xC00000/128 = 98304 | ~8.6 deg (full-range 12-bit) | YES (= 0x18000) |
| 50 | 0xC00000/178 = 70361 | ~6.2 deg | No |
| 100 | 0xC00000/228 = 54882 | ~4.8 deg | No |
| 200 | 0xC00000/328 = 38049 | ~3.3 deg | No |
| 500 | 0xC00000/628 = 19899 | ~1.7 deg | No |
| 1000 | 0xC00000/1128 = 11085 | ~1.0 deg | No |
| 2000 | 0xC00000/2128 = 5880 | ~0.5 deg | No |

The max steering angle drops as an inverse function of speed. At stopped/crawling speed, the max angle is capped at 0x18000. At high speed, the angle is severely limited.

**Conversion note:** The full 12-bit angle circle = 4096 units = 360 degrees. The steering_command is in a higher-precision space (multiplied by 0x100 = 256). So 0x18000 = 98304 in this space = 98304/(4096*256) * 360 = ~8.6 degrees max lock.

#### 5.3 Digital Ramp-Up Rate

```c
// Ramp counter at actor.gap_0338+2 (short)
// Increments by 0x40 per frame when steering key held
// Decrements by 0x40 per frame when released
// Range: 0x00 to 0x100

// When pressing LEFT:
if (ramp < 0x100) ramp += 0x40;

if (already_steering_right) {
    steering_command += steer_rate * (-2);     // Snap reversal
} else {
    steering_command -= (ramp * steer_rate) >> 8;  // Gradual ramp
}
```

**Ramp timeline (frames from key press):**

| Frame | Ramp Counter | Effective Rate |
|---|---|---|
| 0 | 0x00 | 0% of steer_rate |
| 1 | 0x40 | 25% of steer_rate |
| 2 | 0x80 | 50% of steer_rate |
| 3 | 0xC0 | 75% of steer_rate |
| 4+ | 0x100 | 100% of steer_rate |

**At 30 FPS, full steering rate is reached in 4 frames = 133 ms.**

#### 5.4 Auto-Centering Rate

```c
// When no steering key is held:
if (ramp > 0) ramp -= 0x40;

center_rate = steer_rate * 4;   // 4x the normal turn rate

if (abs(steering_command) < center_rate) {
    steering_command = 0;        // Snap to center
} else if (steering_command > 0) {
    steering_command -= center_rate;
} else {
    steering_command += center_rate;
}
```

**Key insight:** Auto-centering is **4x faster** than the steering rate itself. This means:
- At speed 0: center_rate = 12288 * 4 = 49152 per frame. With max steer of 98304, it takes only 2 frames (67 ms) to auto-center from full lock.
- At speed 500: center_rate = 10315 * 4 = 41260. From max steer of 19899, it takes less than 1 frame to snap to center.

**Prediction:** At any speed above crawling, releasing the steering key produces an instant snap-to-center, which should feel very responsive/twitchy to the player. This is characteristic of 90s arcade racing games.

#### 5.5 Path Correction Bias (Auto-Steer)

```c
path_angle = AngleFromVector12(path_vec.x >> 8, path_vec.y >> 8);
heading_delta = ((path_angle - (heading >> 8)) - 0x800) & 0xFFF) - 0x800;  // signed wrap

// Scale by speed factor (reduces at very high speed or when flags set)
if (speed_factor < 0x800 && !encounter_flag && !handbrake_flag) {
    heading_delta = (speed_factor * heading_delta + rounding) >> 11;
}

// This heading_delta is multiplied by 0x100 and folded into the steering limits
```

This provides a subtle auto-steer toward the track centerline. The effect is strongest at low speed and fades at high speed (where `speed_factor` approaches 0x800). Players won't notice it directly but will feel the car "wanting" to stay on the road.

### Path B: Analog Steering (Joystick)

```c
raw_x = (bitmask & 0x1FF) - 0xFA;   // Signed: -250 to +250
// same steer_limit computed as Path A

if (raw_x < 0) {
    // Left turn
    steering_command = raw_x * steer_limit / LARGE_CONSTANT;
} else {
    // Right turn
    steering_command = raw_x * steer_limit / LARGE_CONSTANT;
}
```

The analog path produces a **proportional** mapping: axis position * speed-dependent limit. No ramp-up, no auto-centering, no delay. This is pure proportional control.

The divisor in the multiplication uses a magic constant `0x10624DD3` which is the fixed-point reciprocal of 250 (the axis range), ensuring full-left = full steer_limit and center = 0.

### Testable Predictions

1. **Digital steering at 0 speed:** Max steer reached in 4 frames (133 ms). Releasing key: returns to center in 2 frames (67 ms).

2. **Digital steering at high speed (~1000):** Max angle severely limited to ~11,000 (vs 98,304 at standstill). Steering ramp still takes 4 frames but the total travel is much less.

3. **Analog vs digital at mid-speed:** Analog joystick gives instant response (proportional to stick deflection), while digital keyboard takes 4 frames to reach equivalent angle. This means analog should feel significantly more responsive in quick chicanes.

4. **Auto-centering snap:** At speeds above ~200, releasing the digital steering key should result in visually instant re-centering (< 1 frame). At very low speed, there's a brief ~2-frame return animation.

5. **Path bias effect:** On a straight road, a car with zero player input should very slowly drift toward the track center if it's offset. This is observable by parking at an angle on a straight and releasing all controls -- the car should gently correct its heading.

---

## Appendix: Key Constants Reference

| Address | Value | System | Usage |
|---|---|---|---|
| 0x463204 | 500,000 | Collision | V2V inertia denominator constant K |
| 0x463200 | 1,500,000 | Collision | V2W inertia denominator constant |
| 0x463188 | 0/1 | Collision | Collisions toggle (0=on, 1=off) |
| 0x473D9C | varies | Rubber-band | behind_scale (per-tier) |
| 0x473DA0 | varies | Rubber-band | ahead_scale (per-tier) |
| 0x473DA4 | varies | Rubber-band | behind_clamp (per-tier) |
| 0x473DA8 | varies | Rubber-band | ahead_clamp (per-tier) |
| 0x4AADB4 | 0/1 | Rubber-band | Time Trial mode flag (disables rubber-band) |
| 0x4C3DD8 | varies | Weather | Target particle density (view 0) |
| 0x4C3DE0 | varies | Weather | Active particle count (view 0) |
| 0x4C3DE8 | 0/1/2 | Weather | Weather type (0=rain, 1=snow, 2=clear) |
| 0x4AAF00 | 6-12 | General | Total actor count |
| 0x482FFC | DWORD[2] | Input | Per-player control bitmask |

---

## Appendix: Physics Fixed-Point Scales

| Domain | Scale | Range | Notes |
|---|---|---|---|
| Angles | 12-bit | 0-4095 = 0-360 deg | Used by CosFixed12bit/SinFixed12bit |
| Steering command | 20-bit | +/- 0x18000 max | Angle * 256 for precision |
| Velocities | 12-bit fractional | varies | actor+0x1CC..0x1D4 |
| Positions | 8-bit fractional | varies | actor+0x1FC..0x204 |
| Throttle multiplier | 8-bit | 0x100 = 1.0x | Rubber-band output |
| Collision impulse | 12-bit | varies | Impulse magnitude after >>12 |
| Gravity | integer | 0x5DC-0x800 | Easy=1500, Normal=1900, Hard=2048 |
| Mass (collision) | integer | typically 0x20-0x60 | tuning+0x88, higher=heavier |
