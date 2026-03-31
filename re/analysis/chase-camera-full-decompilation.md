# UpdateChaseCamera (0x401590) -- Full Decompilation & Time-Delta Scaling Plan

> Date: 2026-03-20
> Binary: TD5_d3d.exe (base 0x400000)
> Function: UpdateChaseCamera @ VA 0x401590-0x40194D (957 bytes, 287 instructions)
> Signature: `void __cdecl UpdateChaseCamera(int actorPtr, int doTrackHeading, int viewIndex)`
> Called from: RunRaceFrame (0x42B580) inside physics loop at VA 0x42B9F4
> Calls: SinFloat12bit, CosFloat12bit, CosFixed12bit, SinFixed12bit, UpdateActorTrackPosition,
>        ComputeActorTrackContactNormal, AngleFromVector12, BuildRotationMatrixFromAngles,
>        LoadRenderRotationMatrix, ConvertFloatVec3ToShortAngles

---

## 1. Parameters

| Param | Name | Type | Description |
|-------|------|------|-------------|
| param_1 | actorPtr | int (ptr) | Pointer to actor struct (0x388 bytes/slot) |
| param_2 | doTrackHeading | int | 1 = update orbit angle tracking vehicle heading; 0 = freeze |
| param_3 | viewIndex | int | Viewport index (0 or 1 for split-screen) |

---

## 2. Camera Preset Table

**Base address**: 0x00463098 (first entry mode field)
**Stride**: 16 bytes per preset
**Count**: 7 presets (indices 0-6, cycled by CycleRaceCameraPreset at 0x402E00)

### Preset Record Layout (16 bytes)

| Offset | Size | Field | In LoadCameraPresetForView |
|--------|------|-------|---------------------------|
| +0x00 | short | mode | 0=chase orbit, 1=bumper/in-car |
| +0x02 | short | elevation_angle | Stored to DAT_00482e0c[view] as `elev << 8` |
| +0x04 | short | orbit_radius_raw | Multiplied by 256.0 -> float DAT_00482ed0[view], DAT_00482f00[view] |
| +0x06 | short | height_target_raw | Multiplied by 256.0 -> float DAT_00482f10[view], DAT_00482eb0[view] |
| +0x08 | int | extra_param_1 | Stored to DAT_00482ea0[view*2] |
| +0x0C | int | extra_param_2 | Stored to DAT_00482ea4[view*2] |

### Decoded Presets

| # | Mode | Elevation | Radius*256 | Height*256 | Notes |
|---|------|-----------|------------|------------|-------|
| 0 | 0 (chase) | 600 | 2100*256=537600 | 510*256=130560 | Far high, most elevated |
| 1 | 0 (chase) | 550 | 1710*256=437760 | 110*256=28160 | Far low |
| 2 | 0 (chase) | 475 | 1500*256=384000 | 310*256=79360 | Medium high |
| 3 | 0 (chase) | 400 | 1350*256=345600 | 110*256=28160 | Medium low |
| 4 | 0 (chase) | 325 | 1200*256=307200 | 240*256=61440 | Close medium |
| 5 | 0 (chase) | 240 | 1550*256=396800 | 110*256=28160 | Low far |
| 6 | 1 (bumper) | 0 | 0 | 0 | In-car / bumper cam |

**Preset selector**: `gRaceCameraPresetId` array, indexed by viewIndex, cycled mod 7.
**Mode stored to**: `gRaceCameraPresetMode[viewIndex]` -- if mode != 0, UpdateChaseCamera is skipped.

### Fly-in threshold

`DAT_00463090` = computed as `DAT_0046602c * 7 + 5` at frontend init (ScreenMainMenuAnd1PRaceFlow case 0).
- DAT_0046602c is a frontend option (camera speed setting, values 0-5).
- Default value in .data: 0x28 = 40 (i.e. setting=5: 5*7+5=40).
- This is the **target frame count** for the fly-in counter.

---

## 3. Per-View State Arrays (all indexed by `viewIndex * 4` unless noted)

All arrays are in .bss at 0x00482E__. Stride between view 0 and view 1 is always 4 bytes (int/float) unless otherwise noted.

| Global Address | Type | Name | Purpose |
|----------------|------|------|---------|
| `0x482EB0[v*4]` | float | `smoothedHeight` | Smoothed camera height offset (damped toward target) |
| `0x482ED0[v*4]` | float | `orbitRadiusScale` | Orbit distance scale factor (from preset, used by orbit camera) |
| `0x482ED8[v*12]` | int[3] | `cameraOrbitOffset` | Camera orbit position vector {X,Y,Z} relative to vehicle |
| `0x482E0C[v*4]` | int | `elevationAngleFP` | Elevation angle in fixed-point (preset elev << 8) |
| `0x482EA0[v*8]` | int[2] | `extraParams` | Extra preset params (from table offset +8,+0C) |
| `0x482EF0[v*4]` | int | `flyInCounter` | Fly-in transition counter (ramps toward DAT_00463090) |
| `0x482EF8[v*4]` | int | `cameraRotationSlot` | Rotation slot index (always 0 in chase cam; set for orbit modes) |
| `0x482F00[v*4]` | float | `currentRadius` | **INTEGRATOR**: Current orbit radius (damped spring) |
| `0x482F10[v*4]` | float | `targetHeight` | Target height from preset (static per preset change) |
| `0x482F18[v*4]` | int | `orbitAngleFP` | **INTEGRATOR**: Orbit angle in 20-bit fixed-point (tracks heading) |
| `0x482F28[v*4]` | int | `presetChangeFlag` | Cleared on preset load |
| `0x482F30[v*12]` | int[3] | `cameraWorldPos` | Final camera world position (used by orbit cam) |
| `0x482F50[v*8]` | short[4] | `cameraOrientShort` | Camera orientation as short angles {pitch, yaw, roll, ?} |
| `0x482F60[v*4]` | int | `headingDelta20` | Heading delta (vehicle heading - orbit angle), 20-bit masked |
| `0x482F70[v*4]` | int | `headingBiasAngle` | Heading offset/bias for orbit placement |
| `0x482F78[v*4]` | int | `presetExtraFlag` | Cleared on preset load |
| `0x482FA0[v*4]` | int | `heightAngleFP` | **INTEGRATOR**: Height/pitch angle smoothing accumulator |

---

## 4. Global Constants Referenced

| Address | Hex Value | Float Value | Name / Purpose |
|---------|-----------|-------------|----------------|
| `0x45D5D8` | 0x43800000 | **256.0** | Preset raw-to-float scale (radius, height) |
| `0x45D5DC` | 0x42000000 | **32.0** | Radius spring target scale (`radiusSpringScale`) |
| `0x4749C8` | 0x3E000000 | **0.125** | Damping weight for radius and height (`dampWeight`) |
| `0x4749D0` | 0x3B800000 | **0.00390625** | Radius scale for orbit camera (1/256) |
| `0x463090` | 0x00000028 | 40 (int) | Fly-in threshold frame count (`flyInThreshold`) |

---

## 5. Actor Struct Fields Read

| Offset | Type | Purpose |
|--------|------|---------|
| +0x0080 | short | Track segment base ID |
| +0x008C | byte | Track segment attribute |
| +0x0120 | float[12] | Rotation matrix (used by orbit cam, not chase) |
| +0x01CC | int | Vehicle velocity X |
| +0x01D0 | int | Vehicle velocity Y |
| +0x01D4 | int | Vehicle velocity Z |
| +0x01FC | int | Vehicle world position X |
| +0x0200 | int | Vehicle world position Y |
| +0x0204 | int | Vehicle world position Z |
| +0x020A | short | Vehicle heading angle (12-bit, 0-4095) |
| +0x0379 | byte | Flag: nonzero = "special mode" (reverse fly-in behavior) |

---

## 6. Detailed Code Walkthrough

### 6.1 Fly-In Counter (lines ~1-20 of decompilation)

```c
// param_1+0x379 == 0: NORMAL mode
// flyInCounter ramps UP toward flyInThreshold (DAT_00463090 = 40)
if (actor->flag_0x379 == 0) {
    if (flyInCounter[view] < flyInThreshold) {
        flyInCounter[view]++;          // INCREMENT per call
    }
    if (flyInCounter[view] > flyInThreshold) goto decrement;
} else {
    // SPECIAL mode: counter decrements toward 6
    if (flyInCounter[view] <= 5) goto done;
}
decrement:
    flyInCounter[view]--;
```

**Integrator**: `flyInCounter[view]` at `0x482EF0[view*4]`
- **Formula**: `flyInCounter += 1` per call (clamped at flyInThreshold)
- **Frame-rate dependent**: YES -- increments once per physics tick (30Hz)
- **For dt scaling**: `flyInCounter += dt_ticks` (where dt_ticks = elapsed / physics_tick_period)
- **Threshold**: configurable, default 40 = ~1.33 seconds at 30fps

### 6.2 Orbit Angle Setup & Camera Position Computation

```c
iVar7 = viewIndex * 4;  // array stride
local_10 = currentRadius[view];  // float from 0x482F00[v*4]

// Compute orbit angle for camera placement:
// orbit_visual_angle = headingBias[view] - (orbitAngleFP[view] >> 8)
local_18 = headingBias[view] - (orbitAngleFP[view] >> 8);

cameraRotationSlot[view] = 0;  // always zero for chase cam

// Camera position = sin/cos of orbit angle * radius
cameraOrbitOffset[view].X = ROUND(sin(orbit_visual_angle) * currentRadius);
cameraOrbitOffset[view].Y = heightAngle[view];  // from 0x482FA0
cameraOrbitOffset[view].Z = ROUND(-cos(orbit_visual_angle) * currentRadius);
```

### 6.3 Heading Tracking / Orbit Angle Integrator (when doTrackHeading != 0)

This is the **core damped angle follower**:

```c
if (doTrackHeading != 0) {
    // Compute angular delta between vehicle heading and current orbit angle
    // Both in 20-bit fixed-point (heading * 0x100 gives 20-bit from 12-bit heading)
    uint headingDelta = (actor->heading * 0x100 - orbitAngleFP[view]) & 0xFFFFF;

    // Wrap to signed range [-0x80000, +0x7FFFF]
    if (headingDelta > 0x7FFFF) {
        headingDelta -= 0x100000;
    }

    // Compute tracking speed: absolute delta magnitude + 5, minimum 15
    int absDelta = abs((int)headingDelta >> 11);  // coarse magnitude
    int speed = absDelta + 5;
    headingDeltaStore[view] = headingDelta;

    int effectiveSpeed = max(speed, 15);

    // But during fly-in, use lower speed (clamped by flyInCounter)
    int flyIn = flyInCounter[view];
    if (flyIn < effectiveSpeed) {
        // Fly-in still in progress: use flyInCounter as speed limiter
        effectiveSpeed = speed;  // use raw (possibly < 15)
        if (speed < 15) effectiveSpeed = 15;
    }
    // Else: flyIn >= effectiveSpeed, use speed (possibly clamped to 15)

    // UPDATE ORBIT ANGLE:
    orbitAngleFP[view] += (effectiveSpeed * headingDelta) >> 8;
}
```

**Integrator**: `orbitAngleFP[view]` at `0x482F18[view*4]`
- **Formula**: `orbitAngleFP += (speed * delta) >> 8`
- **Effective**: `orbitAngleFP += delta * speed/256` -- proportional tracking
- **speed** ranges from 15 (slow, during fly-in) to `absDelta + 5` (faster when turning hard)
- **Frame-rate dependent**: YES -- accumulates once per 30Hz tick
- **For dt scaling**: `orbitAngleFP += ((speed * delta) >> 8) * dt_ticks`
  - OR: convert speed/256 to a per-second rate and multiply by dt_seconds
  - The `>> 8` is a fixed 1/256 per-tick gain. At 30fps: ~1/8.53 per second.

### 6.4 Terrain-Following (Ground Contact Normal Sampling)

The camera uses **3-point terrain sampling** to determine ground slope at the camera orbit position:

```c
// Compute 3 sample points around vehicle, each offset by 0x20 (32 world units)
// using the current orbit angle (cos/sin from the combined angle)
uVar6 = (orbitAngleFP[view] >> 8) + cameraRotationSlot[view] * 0x800;

cosAngle = CosFixed12bit(uVar6);
sinAngle = SinFixed12bit(uVar6);

// Point A: forward-right of vehicle heading
pointA.X = vehicle.X + sinAngle * 0x20;
pointA.Z = vehicle.Z + cosAngle * 0x20;

// Point B: forward-left
pointB.X = vehicle.X - cosAngle * 0x20;
pointB.Z = vehicle.Z + sinAngle * 0x20;

// Point C: backward
pointC.X = vehicle.X + cosAngle * 0x20;
pointC.Z = vehicle.Z - sinAngle * 0x20;

// For each point: find track segment, then compute contact normal
UpdateActorTrackPosition(segA, &pointA);
ComputeActorTrackContactNormal(segA, &pointA, &normalA_Y);  // Y component

UpdateActorTrackPosition(segB, &pointB);
ComputeActorTrackContactNormal(segB, &pointB, &normalB_Y);

UpdateActorTrackPosition(segC, &pointC);
ComputeActorTrackContactNormal(segC, &pointC, &normalC_Y);

// Compute pitch from normals:
// Average of B and C normals vs A normal -> pitch angle
int avgBC_Y = (normalB_Y + normalC_Y) / 2;
int pitchDelta = -(normalA_Y - avgBC_Y) >> 8;
short pitchAngle = AngleFromVector12(pitchDelta, 0x200);

// Compute roll from normals:
// Difference of B and C normals -> roll angle
int rollDelta = -(normalB_Y - normalC_Y) >> 8;
short rollAngle = AngleFromVector12(rollDelta, 0x400);

// Build rotation matrix from {pitchAngle, orbitAngle, rollAngle}
// This orients the camera to follow terrain slope
BuildRotationMatrixFromAngles(matrix, &{pitchAngle, orbitAngle, rollAngle});
```

**NOT an integrator** -- this is a **per-frame geometric computation** (no state carried forward).
No dt scaling needed for terrain following itself, but it affects the camera orientation output.

### 6.5 Camera Look-Direction Computation

```c
// Compute look-at angle = orbit angle - vehicle heading + headingBias
uint lookAngle = (orbitAngleFP[view] >> 8) - actor->heading + headingBias[view];

// Camera forward vector (terrain-relative):
cameraForward.X = (short)(ROUND(sin(lookAngle) * orbitRadiusScale[view]) >> 8);
cameraForward.Y = (short)(elevationAngleFP[view] >> 8);
cameraForward.Z = -(short)(ROUND(cos(lookAngle) * orbitRadiusScale[view]) >> 8);

LoadRenderRotationMatrix(terrainMatrix);
ConvertFloatVec3ToShortAngles(&cameraForward, &outputAngles);
// Store final orientation
cameraOrientShort[view] = outputAngles;
```

### 6.6 Height/Pitch Angle Smoother (INTEGRATOR)

```c
// Height angle accumulator -- smoothly follows the terrain-derived pitch
int heightTarget = cameraForward.Y * 0x100;  // scale up
int heightDelta = heightTarget - heightAngleFP[view];

// Exponential smoothing with weight 1/8:
heightAngleFP[view] += (heightDelta + (heightDelta >> 31 & 7)) >> 3;
```

**Integrator**: `heightAngleFP[view]` at `0x482FA0[view*4]`
- **Formula**: `heightAngleFP += (target*256 - heightAngleFP) >> 3`
  - This is: `heightAngleFP = heightAngleFP + (target - heightAngleFP) * (1/8)`
  - Exponential decay with weight **alpha = 1/8 = 0.125 per tick**
- **Frame-rate dependent**: YES -- at 30fps, convergence rate = 1-(7/8)^30 per second
- **For dt scaling**: `alpha_dt = 1 - (1 - 1/8)^dt_ticks = 1 - 0.875^dt_ticks`
  - `heightAngleFP += (target*256 - heightAngleFP) * alpha_dt`
  - Integer approximation: multiply by `dt_ticks` then shift (loses accuracy at high dt)

### 6.7 Radius Spring (INTEGRATOR)

```c
// Compute desired radius from camera forward vector magnitude
short fx = cameraOrientShort[view].pitch;  // X component
short fz = cameraOrientShort[view].roll;   // Z component
float desiredMagnitude = sqrt((float)(fx*fx + fz*fz));

// Damped spring toward desired radius:
// currentRadius += desiredMagnitude * 32.0 - currentRadius * 0.125
currentRadius[view] = currentRadius[view]
    + desiredMagnitude * 32.0f       // spring pull toward target
    - currentRadius[view] * 0.125f;  // damping (drag toward zero)
```

Wait -- let me re-read the disassembly more carefully:

```asm
FILD dword [EBP+0x10]         ; push int(fx*fx + fz*fz) as float
FSQRT                         ; sqrt(magnitude^2) = magnitude
FMUL float [0x45d5dc]         ; * 32.0 = desired_pull
FLD  float [0x4749c8]         ; load 0.125
FMUL float [ESI+0x482f00]     ; * currentRadius = drag
FSUBP                         ; desired_pull - drag
FADD float [ESI+0x482f00]     ; + currentRadius
FSTP float [ESI+0x482f00]     ; store back
```

So the actual formula is:
```
currentRadius += sqrt(fx*fx + fz*fz) * 32.0 - currentRadius * 0.125
```

Which simplifies to:
```
currentRadius = currentRadius * (1 - 0.125) + sqrt(mag) * 32.0
currentRadius = currentRadius * 0.875 + sqrt(mag) * 32.0
```

**Integrator**: `currentRadius[view]` at `0x482F00[view*4]`
- **Formula**: `R_new = R_old * 0.875 + sqrt(|forward|) * 32.0`
  - Exponential decay with alpha = 0.125, target = sqrt(|forward|) * 32.0 / 0.125 = sqrt(|forward|) * 256
  - Actually: `R_new = R_old + (sqrt(|forward|) * 32.0 - R_old * 0.125)`
  - This is: `R += target_pull - R * drag`
  - Spring with: `drag_coeff = 0.125/tick`, `target_force = sqrt(|fwd|) * 32.0/tick`
- **Constants**: 32.0 at 0x45D5DC, 0.125 at 0x4749C8
- **Frame-rate dependent**: YES -- both the pull and drag are per-tick
- **For dt scaling**:
  - `R_new = target + (R_old - target) * (1 - 0.125)^dt_ticks`
  - Where `target = sqrt(|fwd|) * 32.0 / 0.125 = sqrt(|fwd|) * 256.0`
  - Or more precisely: `R += (sqrt(|fwd|) * 32.0 - R * 0.125) * dt_ticks`

### 6.8 Height Smoothing (INTEGRATOR)

```asm
FLD  float [ESI+0x482f10]     ; targetHeight (from preset)
FSUB float [ESI+0x482eb0]     ; - smoothedHeight
FMUL float [0x4749c8]         ; * 0.125
FADD float [ESI+0x482eb0]     ; + smoothedHeight
FSTP float [ESI+0x482eb0]     ; store
```

```c
smoothedHeight[view] = smoothedHeight[view] + (targetHeight[view] - smoothedHeight[view]) * 0.125;
```

**Integrator**: `smoothedHeight[view]` at `0x482EB0[view*4]`
- **Formula**: `H = H + (target - H) * 0.125`
  - Classic exponential smoothing, alpha = 0.125
  - Equivalent: `H = H * 0.875 + target * 0.125`
- **Constants**: 0.125 at 0x4749C8
- **Frame-rate dependent**: YES -- alpha applied once per tick
- **For dt scaling**: `H = target + (H - target) * 0.875^dt_ticks`
  - Or: `alpha_dt = 1 - 0.875^dt_ticks`; `H += (target - H) * alpha_dt`

---

## 7. Complete Integrator/Accumulator Summary

### 7A. Frame-Rate-Dependent Variables (NEED dt scaling)

| # | Variable | Address | Type | Formula | Constants | dt Scaling |
|---|----------|---------|------|---------|-----------|------------|
| 1 | `flyInCounter[v]` | 0x482EF0+v*4 | int | `+= 1` per tick | Threshold: 0x463090 (40) | `+= dt_ticks` |
| 2 | `orbitAngleFP[v]` | 0x482F18+v*4 | int (20-bit FP) | `+= (speed * delta) >> 8` | speed: 15 min, delta-adaptive | `+= ((speed * delta) >> 8) * dt_ticks` |
| 3 | `heightAngleFP[v]` | 0x482FA0+v*4 | int | `+= (target - current) >> 3` | weight: 1/8 (>>3) | `+= (target - current) * (1 - 0.875^dt)` |
| 4 | `currentRadius[v]` | 0x482F00+v*4 | float | `+= sqrt(mag)*32.0 - R*0.125` | 32.0 @ 0x45D5DC, 0.125 @ 0x4749C8 | `R = T + (R-T) * 0.875^dt` where T = sqrt(mag)*256 |
| 5 | `smoothedHeight[v]` | 0x482EB0+v*4 | float | `+= (target - H) * 0.125` | 0.125 @ 0x4749C8 | `H = T + (H-T) * 0.875^dt` |

### 7B. Per-Frame Computed (NO dt scaling needed)

| Variable | Address | Notes |
|----------|---------|-------|
| `cameraOrbitOffset[v]` | 0x482ED8+v*12 | Recomputed from orbitAngle + radius each frame |
| `headingDelta20[v]` | 0x482F60+v*4 | Intermediate: heading error signal |
| `cameraOrientShort[v]` | 0x482F50+v*8 | Recomputed each frame from terrain + orbit angle |
| `cameraRotationSlot[v]` | 0x482EF8+v*4 | Always set to 0 in chase cam |
| Terrain normals | stack locals | 3-point sample, no history |

---

## 8. Shared Damping Constant: 0.125 at 0x4749C8

This single float is used by THREE integrators:
1. Height angle smoother (#3): integer approximation via `>> 3`
2. Radius spring (#4): `- R * 0.125` drag term
3. Height smoothing (#5): `* 0.125` alpha

All three share the same convergence rate: **87.5% retention per tick** (half-life ~ 5.2 ticks ~ 173ms at 30fps).

### Other constants at 0x4749C8 neighborhood

| Address | Value | Used by |
|---------|-------|---------|
| 0x4749C8 | 0.125 | Chase camera (3 integrators) |
| 0x4749CC | 0.0078125 (1/128) | Unknown (not in chase cam) |
| 0x4749D0 | 0.00390625 (1/256) | UpdateTracksideOrbitCamera radius scale |
| 0x4749D4 | 0.000122... (1/8192) | Unknown |

---

## 9. Time-Delta Scaling Plan

### Strategy: Exponential Decay Correction

For all exponential smoothers (integrators #3, #4, #5), the pattern is:
```
value = value + (target - value) * alpha
```
where `alpha = 0.125` per tick at 30Hz.

For time-delta scaling, this becomes:
```
alpha_dt = 1 - (1 - alpha)^(dt / tick_period)
value = value + (target - value) * alpha_dt
```

At 30Hz (`dt = tick_period`): `alpha_dt = 1 - 0.875^1 = 0.125` (identical to original).
At 60Hz (`dt = tick_period/2`): `alpha_dt = 1 - 0.875^0.5 = 0.0645` (half the correction).
At 120Hz (`dt = tick_period/4`): `alpha_dt = 1 - 0.875^0.25 = 0.0330`.

### Per-Integrator Patch Notes

#### Integrator #1: flyInCounter
- Simple: multiply increment by dt_ticks (integer, round or accumulate fractional)
- Or: keep at 30Hz rate since it's just a startup transition (low priority)

#### Integrator #2: orbitAngleFP (heading tracker)
- The tracking speed adapts to the heading delta magnitude
- Scale the per-tick accumulation: `+= ((speed * delta) >> 8) * dt_ticks`
- This is a proportional controller, not exponential decay -- linear dt scaling works
- **Caution**: at very high dt_ticks, could overshoot. May need clamping.

#### Integrator #3: heightAngleFP (integer exponential)
- Currently: `+= (target - current) >> 3` (alpha = 0.125, integer math)
- For dt: need `1 - 0.875^dt` which requires float math or lookup table
- Simplest patch: `+= ((target - current) >> 3) * dt_ticks` (linear approx, close for dt <= 2)
- Better: convert to float, apply exact exponential, convert back

#### Integrator #4: currentRadius (float spring)
- Currently: `R += sqrt(mag)*32.0 - R*0.125`
- Exact: `R = T + (R - T) * 0.875^dt` where T = sqrt(mag) * 256.0
- This requires computing `0.875^dt` -- could use a precomputed table indexed by tick count
- **Implementation**: Replace the FMUL+FADD with a subroutine that takes dt

#### Integrator #5: smoothedHeight (float exponential)
- Currently: `H += (target - H) * 0.125`
- Exact: same as #4: `H = T + (H - T) * 0.875^dt`
- **Implementation**: same approach as #4, shares the `0.875^dt` factor

### Implementation Approach

**Option A: Code cave with dt parameter**
- Add a `dt_ticks` global (written once per frame from `g_simTimeAccumulator`)
- Each integrator reads dt_ticks and scales accordingly
- Pro: exact at any framerate. Con: ~200 bytes of cave code, float math.

**Option B: Tick-count multiplication (simple)**
- When running camera once per frame (after physics loop), multiply all per-tick increments by the number of physics ticks that elapsed
- `dt_ticks = ticks_this_frame` (integer, typically 1-4)
- Pro: simple integer math. Con: step-wise, not smooth at variable framerates.

**Option C: Sub-tick interpolation (smoothest)**
- Run camera at physics rate (current behavior)
- After loop, interpolate between previous and current camera state using sub-tick fraction
- `fraction = (g_simTimeAccumulator & 0xFFFF) / 0x10000`
- Pro: perfectly smooth. Con: need to store previous frame state (extra memory).

### Recommended: Option C (sub-tick interpolation)
As noted in the camera decoupling patch document, this gives the smoothest results with minimal changes to the integrator math. The 5 state variables (flyInCounter, orbitAngleFP, heightAngleFP, currentRadius, smoothedHeight) would be double-buffered, with linear interpolation applied to the camera output position.

---

## 10. Float Constant Address Summary

| Address | Section | Value | Used In | Scaling Role |
|---------|---------|-------|---------|--------------|
| 0x45D5D8 | .rdata | 256.0 | LoadCameraPresetForView | Preset raw-to-float conversion |
| 0x45D5DC | .rdata | 32.0 | UpdateChaseCamera radius spring | Spring pull strength |
| 0x4749C8 | .data | 0.125 | UpdateChaseCamera (3x), UpdateTracksideOrbitCamera | Damping coefficient per tick |
| 0x4749D0 | .data | 0.00390625 | UpdateTracksideOrbitCamera | Orbit radius scale (1/256) |
| 0x463090 | .data | 40 (int) | UpdateChaseCamera fly-in | Fly-in frame threshold |
| 0x4AAF60 | .bss | (runtime) | UpdateTracksideOrbitCamera | Time scale factor (set per frame in RunRaceFrame) |

---

## 11. Key Global Addresses (non-array)

| Address | Type | Name | Notes |
|---------|------|------|-------|
| 0x482FD4 | int | `gRaceCameraPresetId` (view 0) | Preset index 0-6. View 1 at +4. |
| 0x482FD8 | int | `gRaceCameraPresetMode` (view 0) | Mode from preset (0=chase, 1=bumper). View 1 at +4. |
| 0x482FDC | int | `gRaceCameraPresetMode` (view 1) | For split-screen. |
| 0x463090 | int | `flyInThreshold` | Frames before orbit tracking reaches full speed. |
| 0x466EA0 | int | `gPrimaryActorSlot` | Actor slot for view 0 camera. |
| 0x466EA4 | int | `gSecondaryActorSlot` | Actor slot for view 1 camera. |
| 0x4AAF60 | float | `g_simTimeFractionFloat` | `simTimeAccumulator * (1/65536)` -- fractional ticks elapsed. Written at VA 0x42B72F. Used by UpdateTracksideOrbitCamera but NOT UpdateChaseCamera. |
| 0x4AAF89 | byte | `gRaceViewportLayoutMode` | 0=single, 1=split horizontal, etc. |

---

## 12. Camera State Block Memory Map (0x482EA0 - 0x482FE0)

All per-view arrays. View 0 = base address, View 1 = base + stride.

```
0x482EA0 [v*8]  int[2]    extraParams           (16 bytes: 2 views x 2 ints)
0x482EB0 [v*4]  float     smoothedHeight         (8 bytes: 2 views) ** INTEGRATOR **
0x482EB8        (padding)
0x482EC0        (padding)
0x482ED0 [v*4]  float     orbitRadiusScale       (8 bytes: 2 views)
0x482ED8 [v*12] int[3]    cameraOrbitOffset      (24 bytes: 2 views x 3 ints)
0x482EF0 [v*4]  int       flyInCounter           (8 bytes: 2 views) ** INTEGRATOR **
0x482EF8 [v*4]  int       cameraRotationSlot     (8 bytes: 2 views)
0x482F00 [v*4]  float     currentRadius          (8 bytes: 2 views) ** INTEGRATOR **
0x482F08        (padding)
0x482F10 [v*4]  float     targetHeight           (8 bytes: 2 views)
0x482F18 [v*4]  int       orbitAngleFP           (8 bytes: 2 views) ** INTEGRATOR **
0x482F20        (padding)
0x482F28 [v*4]  int       presetChangeFlag       (8 bytes: 2 views)
0x482F30 [v*12] int[3]    cameraWorldPos         (24 bytes: 2 views x 3 ints)
0x482F48 [2]    byte      packedPresetSave       (2 bytes: one per view)
0x482F50 [v*8]  short[4]  cameraOrientShort      (16 bytes: 2 views x 4 shorts)
0x482F60 [v*4]  int       headingDelta20         (8 bytes: 2 views)
0x482F68        (padding)
0x482F70 [v*4]  int       headingBiasAngle       (8 bytes: 2 views)
0x482F78 [v*4]  int       presetExtraFlag        (8 bytes: 2 views)
0x482FA0 [v*4]  int       heightAngleFP          (8 bytes: 2 views) ** INTEGRATOR **
...
0x482FD4        int       gRaceCameraPresetId[0]
0x482FD8        int       gRaceCameraPresetMode[0]
0x482FDC        int       gRaceCameraPresetMode[1]
```

---

## 13. Cross-Reference: Functions That Write Camera State

| State Variable | Written By |
|----------------|-----------|
| flyInCounter (0x482EF0) | UpdateChaseCamera, LoadCameraPresetForView |
| orbitAngleFP (0x482F18) | UpdateChaseCamera, LoadCameraPresetForView, UpdateRaceCameraTransitionState |
| heightAngleFP (0x482FA0) | UpdateChaseCamera, LoadCameraPresetForView |
| currentRadius (0x482F00) | UpdateChaseCamera, LoadCameraPresetForView, UpdateTracksideOrbitCamera |
| smoothedHeight (0x482EB0) | UpdateChaseCamera, LoadCameraPresetForView, UpdateTracksideOrbitCamera, UpdateStaticTracksideCamera |
| orbitRadiusScale (0x482ED0) | LoadCameraPresetForView, UpdateTracksideOrbitCamera (read) |
| headingBias (0x482F70) | UpdateTracksideCamera, UpdateRaceCameraTransitionState (many writers) |

---

## 14. Annotated Decompilation (Cleaned)

```c
// UpdateChaseCamera -- chase orbit camera with terrain following
// Called once per physics tick (30Hz) from RunRaceFrame's physics loop
void __cdecl UpdateChaseCamera(Actor *actor, int doTrackHeading, int viewIndex)
{
    int v4 = viewIndex * 4;   // array stride for int/float arrays
    int v12 = viewIndex * 12; // array stride for vec3 arrays

    //=== SECTION A: Fly-In Counter ===
    // Controls camera speed during initial zoom-in after race start / preset change
    if (actor->flag_0x379 == 0) {
        // Normal: ramp up toward threshold
        if (flyInCounter[v4] < flyInThreshold)
            flyInCounter[v4]++;
        if (flyInCounter[v4] <= flyInThreshold) goto skip_dec;
    } else {
        // Special: decrement toward 5
        if (flyInCounter[v4] <= 5) goto skip_dec;
    }
    flyInCounter[v4]--;
skip_dec:

    //=== SECTION B: Camera Orbit Position ===
    float radius = currentRadius[v4];                    // 0x482F00
    int orbitVisualAngle = headingBias[v4] - (orbitAngleFP[v4] >> 8);
    cameraRotationSlot[v4] = 0;

    // Place camera at orbit position around vehicle
    cameraOrbitOffset[v12+0] = ROUND(sin12(orbitVisualAngle) * radius);  // X
    cameraOrbitOffset[v12+4] = heightAngleFP[v4];                         // Y (from 0x482FA0)
    cameraOrbitOffset[v12+8] = ROUND(-cos12(orbitVisualAngle) * radius); // Z

    //=== SECTION C: Heading Tracking (Orbit Angle Integrator) ===
    if (doTrackHeading != 0) {
        uint delta = (actor->heading * 0x100 - orbitAngleFP[v4]) & 0xFFFFF;
        if (delta > 0x7FFFF) delta -= 0x100000;   // signed wrap

        int absCoarse = abs((int)delta >> 11);     // magnitude / 2048
        int speed = absCoarse + 5;
        headingDelta20[v4] = delta;

        int effSpeed = max(speed, 15);             // minimum speed = 15

        // During fly-in: clamp to flyInCounter (slower tracking)
        int flyIn = flyInCounter[v4];
        if (flyIn < effSpeed) {
            effSpeed = max(speed, 15);
        }

        // ACCUMULATE: proportional angle tracking
        orbitAngleFP[v4] += (effSpeed * (int)delta) >> 8;   // ** INTEGRATOR #2 **
    }

    //=== SECTION D: Terrain Following (3-point normal sampling) ===
    uint combinedAngle = (orbitAngleFP[v4] >> 8) + cameraRotationSlot[v4] * 0x800;
    int cosA = CosFixed12(combinedAngle);
    int sinA = SinFixed12(combinedAngle);

    // 3 sample points offset 32 units from vehicle center
    Vec3 ptA = { vehicle.X + sinA*32, vehicle.Y, vehicle.Z + cosA*32 };
    Vec3 ptB = { vehicle.X - cosA*32, vehicle.Y, vehicle.Z + sinA*32 };
    Vec3 ptC = { vehicle.X + cosA*32, vehicle.Y, vehicle.Z - sinA*32 };

    // Find ground height at each point
    int normA_Y, normB_Y, normC_Y;
    UpdateActorTrackPosition(&segA, &ptA);
    ComputeActorTrackContactNormal(&segA, &ptA, &normA_Y);
    UpdateActorTrackPosition(&segB, &ptB);
    ComputeActorTrackContactNormal(&segB, &ptB, &normB_Y);
    UpdateActorTrackPosition(&segC, &ptC);
    ComputeActorTrackContactNormal(&segC, &ptC, &normC_Y);

    // Derive pitch and roll from terrain gradient
    short pitch = AngleFromVector12(-(normA_Y - (normB_Y+normC_Y)/2) >> 8, 0x200);
    short yaw   = (short)combinedAngle;
    short roll  = AngleFromVector12(-(normB_Y - normC_Y) >> 8, 0x400);

    float terrainMatrix[12];
    BuildRotationMatrixFromAngles(terrainMatrix, &{pitch, yaw, roll});

    //=== SECTION E: Camera Look Direction ===
    uint lookAngle = (orbitAngleFP[v4] >> 8) - actor->heading + headingBias[v4];

    short fwdX = (short)(ROUND(sin12(lookAngle) * orbitRadiusScale[v4]) >> 8);
    short fwdY = (short)(elevationAngleFP[v4] >> 8);
    short fwdZ = -(short)(ROUND(cos12(lookAngle) * orbitRadiusScale[v4]) >> 8);

    LoadRenderRotationMatrix(terrainMatrix);
    short outAngles[4];
    ConvertFloatVec3ToShortAngles(&{fwdX, fwdY, fwdZ}, outAngles);
    cameraOrientShort[v*8] = outAngles;

    //=== SECTION F: Height Angle Smoother ===
    int hTarget = (int)cameraOrientShort[v*8+2] * 0x100;  // fwdY * 256
    int hDelta = hTarget - heightAngleFP[v4];
    heightAngleFP[v4] += (hDelta + (hDelta>>31 & 7)) >> 3;  // ** INTEGRATOR #3 **
    // Equivalent to: heightAngleFP += hDelta * (1/8)
    // Integer rounding bias for negative deltas (round toward zero)

    //=== SECTION G: Radius Spring ===
    float mag = sqrt((float)(fwdX*fwdX + fwdZ*fwdZ));
    currentRadius[v4] += mag * 32.0f - currentRadius[v4] * 0.125f;  // ** INTEGRATOR #4 **
    // Steady-state: R = mag * 32.0 / 0.125 = mag * 256.0

    //=== SECTION H: Height Smoothing ===
    smoothedHeight[v4] += (targetHeight[v4] - smoothedHeight[v4]) * 0.125f;  // ** INTEGRATOR #5 **
    // Steady-state: smoothedHeight = targetHeight
}
```

---

## 15. Summary of Scaling Constants for Patching

All frame-rate-dependent constants that would need modification:

| What | Where in Code | Current Value | Meaning |
|------|---------------|---------------|---------|
| Fly-in increment | VA 0x4015C3 (`INC EAX`) | +1 per tick | Fly-in ramp speed |
| Orbit angle gain | VA 0x4016D9 (`SAR EAX, 8`) | /256 per tick | Heading tracking responsiveness |
| Height angle alpha | VA 0x4018E1 (`SAR EAX, 3`) | /8 per tick | Pitch smoothing rate |
| Radius drag | 0x4749C8 (float) | 0.125 per tick | Orbit radius damping |
| Radius pull | 0x45D5DC (float) | 32.0 per tick | Orbit radius spring force |
| Height alpha | 0x4749C8 (float) | 0.125 per tick | Height Y smoothing rate |

Note: Constants at 0x4749C8 (0.125) and 0x45D5DC (32.0) are in read-only sections. For dt-scaling
they would need to be replaced with runtime-computed values, likely via code cave redirects.
