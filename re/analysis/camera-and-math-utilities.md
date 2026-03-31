# Camera System & Math/Transform Utilities — Complete Analysis

> Decompiled from `TD5_d3d.exe` via Ghidra (port 8195), 2026-03-20

---

## Table of Contents
1. [Angle & Fixed-Point Conventions](#angle--fixed-point-conventions)
2. [Trig Lookup Tables](#trig-lookup-tables)
3. [Math Utility Library](#math-utility-library)
4. [Render Transform Pipeline](#render-transform-pipeline)
5. [Camera System Architecture](#camera-system-architecture)
6. [Camera Preset System](#camera-preset-system)
7. [Trackside Camera Profiles](#trackside-camera-profiles)
8. [Camera Transition System](#camera-transition-system)
9. [Key Globals Reference](#key-globals-reference)

---

## Angle & Fixed-Point Conventions

### 12-bit Angle System
- **Full circle = 0x1000 (4096) units** — each unit = 360/4096 = 0.087890625 degrees
- Stored as `short` (16-bit), masked with `& 0xFFF` for wraparound
- **Quadrant layout**: 0x000=North(+Z), 0x400=East(+X), 0x800=South(-Z), 0xC00=West(-X)
- Euler angles stored as `short[3]` = {pitch, yaw, roll}

### Fixed-Point 12-bit (Q12)
- Integer values with 12 fractional bits: `value_real = value_int / 4096.0`
- Used in: `CrossProduct3i_FixedPoint12`, `CosFixed12bit`, `SinFixed12bit`
- The fixed-point trig tables store `(int)(cos * 4096)` — range [-4096, +4096]

### World Coordinate Scale
- Positions stored as 32-bit integers, scaled to float via `_DAT_004749d0` (world-to-render scale factor)
- Camera positions often left-shifted by 8 (`* 0x100`) for higher precision in integer domain
- `_DAT_004aaf60` = `g_subTickFraction` — interpolation factor for smooth inter-frame motion

### 3x3 Matrix Layout (Row-Major)
```
[ m[0] m[1] m[2] ]   = row 0 (right/X)
[ m[3] m[4] m[5] ]   = row 1 (up/Y)
[ m[6] m[7] m[8] ]   = row 2 (forward/Z)
```
Stored as `float[9]`, row-major. All rotation matrices are orthonormal.

---

## Trig Lookup Tables

### `BuildSinCosLookupTables` (0x40a650)
Generates two parallel lookup tables of 5120 (0x1400) entries each:

| Table | Base Address | Type | Content |
|-------|-------------|------|---------|
| Float cosine | `0x00488984` | `float[5120]` | `cos(i * 2*PI / 4096)` |
| Fixed cosine | `0x00483984` | `int[5120]` | `(int)round(cos(i * 2*PI / 4096) * 4096)` |

The tables are 5120 entries (not 4096) to allow sin lookups via offset without wrapping: `sin(x) = cos(x - 0x400)`.

### Trig Accessors

| Function | Address | Signature | Notes |
|----------|---------|-----------|-------|
| `CosFloat12bit` | `0x40a6a0` | `float10 (uint angle)` | `table[angle & 0xFFF]` — float result |
| `SinFloat12bit` | `0x40a6c0` | `float10 (int angle)` | `table[(angle - 0x400) & 0xFFF]` — cos shifted by 1/4 turn |
| `CosFixed12bit` | `0x40a6e0` | `int (uint angle)` | Fixed-point Q12 cosine |
| `SinFixed12bit` | `0x40a700` | `int (int angle)` | Fixed-point Q12 sine (same offset trick) |

### `AngleFromVector12` (0x40a720) — atan2 Equivalent
- **Signature**: `int __cdecl AngleFromVector12(int x, int z)`
- Returns 12-bit angle (0x000..0xFFF) from a 2D direction vector
- Uses a precomputed arctan LUT at `0x00463214` (short[1024])
- Implements full 4-quadrant atan2 via octant decomposition:
  - Divides the circle into 8 sectors based on sign and magnitude comparison of x vs z
  - Within each sector, uses `(a * 0x400) / b` ratio as LUT index
  - Returns result offset by the appropriate quadrant base (0x000/0x400/0x800/0xC00)
- Returns 0 if both components are zero

---

## Math Utility Library

### Matrix Operations

#### `MultiplyRotationMatrices3x3` (0x42da10)
- **Signature**: `void __cdecl (float *A, float *B, float *out)`
- Standard 3x3 matrix multiply: `out = A * B`
- Safe for in-place: reads all inputs into locals before writing output
- `out[row*3+col] = sum(A[row*3+k] * B[k*3+col])` for k=0..2

#### `TransposeMatrix3x3` (0x42e710)
- **Signature**: `void __cdecl (uint4 *src, uint4 *dst)`
- `dst[row*3+col] = src[col*3+row]`
- NOT safe for in-place (src != dst required)

#### `BuildRotationMatrixFromAngles` (0x42e1e0)
- **Signature**: `void __cdecl (float *out, short *angles)`
- Builds a combined rotation matrix from Euler angles `{pitch, yaw, roll}` using the 12-bit trig functions
- Computes `R = Rz(roll) * Rx(pitch) * Ry(yaw)` via direct formula:
  ```
  out[0] = sin(roll)*sin(pitch)*sin(yaw) + cos(roll)*cos(yaw)
  out[1] = cos(roll)*sin(pitch)*sin(yaw) - sin(roll)*cos(yaw)
  out[2] = sin(pitch)*cos(yaw)       // note: cos(pitch) typo check
  out[3] = sin(roll)*cos(pitch)
  out[4] = cos(roll)*cos(pitch)
  out[5] = -sin(yaw)
  out[6] = sin(roll)*cos(yaw)*sin(yaw) - cos(roll)*sin(pitch)
  out[7] = cos(roll)*cos(yaw)*sin(yaw) + sin(roll)*sin(pitch)
  out[8] = cos(yaw)*cos(pitch)
  ```

#### `ExtractEulerAnglesFromMatrix` (0x42e030)
- **Signature**: `void __cdecl (int matPtr, short *angles)`
- Extracts Euler angles from a 3x3 float matrix (at byte offsets +0x08..+0x20 in the struct)
- Pitch from `asin(-m[5])` via `AngleFromVector12(-m[5]*4096, sqrt(m[4]^2+m[3]^2)*4096)`
- If pitch != +/-90deg: yaw from `atan2(m[2], m[8])`, roll from `atan2(m[3], m[4])`
- Gimbal lock case (pitch = 0x400): yaw from same atan2, roll = 0

### Vector Operations

#### `TransformVector3ByBasis` (0x42dbd0)
- **Signature**: `void __cdecl (float *matrix, float *vec, float *out)`
- `out[i] = matrix[i*3+0]*vec[0] + matrix[i*3+1]*vec[1] + matrix[i*3+2]*vec[2]`
- Standard matrix-vector multiply (no translation)

#### `CrossProduct3i` (0x42ea70)
- **Signature**: `void __cdecl (int *a, int *b, int *out)`
- `out = a x b` (standard integer cross product)

#### `CrossProduct3i_FixedPoint12` (0x42eac0)
- **Signature**: `void __cdecl (int *a, int *b, int *out)`
- Same as above but each component is right-shifted by 12: `out[i] = (a x b)[i] >> 12`
- Keeps Q12 fixed-point precision after multiplication

### Conversion Functions

#### `ConvertFloatVec3ToIntVec3` (0x42db40) / Variant B (0x42dc30)
- **Signature**: `void __cdecl (undefined4 param1, undefined4 param2, int *out)`
- Converts 3 float values to `short`-range integers via `__ftol` + truncation to `(short)`
- Both variants are identical in behavior (likely different calling conventions or register usage in original source)

#### `ConvertFloatVec3ToShortAngles` (0x42e2e0) / Variant B (0x42cd40)
- **Signature**: `void __cdecl (undefined4 param1, short *out)`
- Converts 3 float values to `short` via `__ftol` truncation
- Used to convert float angle deltas back to the engine's short angle format

#### `ConvertFloatVec4ToShortAngles` (0x42cdb0)
- **Signature**: `longlong __cdecl (undefined4 param1, short *out)`
- Same as Vec3 variant but converts 4 components, returns the 4th as longlong

---

## Render Transform Pipeline

The engine uses a **software transform pipeline** — all vertex transforms happen on CPU, not GPU. A global "current render transform" is stored at the pointer `DAT_004bf6b8`, pointing to a `float[12]` array:

```
[0..2]  = row 0 of 3x3 rotation    (right)
[3..5]  = row 1 of 3x3 rotation    (up)
[6..8]  = row 2 of 3x3 rotation    (forward)
[9..11] = translation vector        (camera world pos)
```

### Transform Functions

| Function | Address | Input | Output | Notes |
|----------|---------|-------|--------|-------|
| `TransformVector3ByRenderRotation` | `0x42e370` | `float[3]` | `float[3]` | Rotation only (no translation) |
| `TransformShortVectorToView` | `0x42e3d0` | `short[3]` | `float[3]` | Full affine: rotation + translation |
| `WriteTransformedShortVector` | `0x42e460` | `short[3]` | vertex+0x0C | Writes XYZ to vertex record at offset 0x0C |
| `WritePointToCurrentRenderTransform` | `0x42e4f0` | `float[3]` | vertex+0x0C | Float input, writes to vertex at +0x0C |
| `TransformTriangleByRenderMatrix` | `0x42e560` | `float[12]` (4 verts) | vertex record | Transforms 4 points (quad), writes at +0x0C, +0x38, +0x64, +0x90 |
| `TransformShortVec3ByRenderMatrixRounded` | `0x42eb10` | `short[3]` | `int[3]` | Full affine, result rounded to int |

**Vertex record layout** (stride 0x2C = 44 bytes per vertex):
- +0x0C: float X (view-space)
- +0x10: float Y (view-space)
- +0x14: float Z (view-space)

`TransformTriangleByRenderMatrix` processes 4 vertices (a quad) with stride 0x2C between output vertices and stride 3 floats between input vertices.

---

## Camera System Architecture

### Overview
The camera system supports two fundamentally different modes:
1. **Vehicle-relative cameras** — attached to and following a specific vehicle (player chase cameras)
2. **Trackside cameras** — fixed or semi-fixed positions along the track (replay/spectator cameras)

Both modes share the same final output pipeline: they set the camera world position and orientation, which flows into the shared projection matrix system.

### Shared Camera Globals

Three 3x3 matrices at fixed addresses form the camera output:

| Matrix | Address | Purpose |
|--------|---------|---------|
| Camera basis (view) | `0x4AAFA0` | Primary view rotation matrix |
| Secondary basis | `0x4AB070` | Used by OrientCameraTowardTarget for look-at construction |
| Tertiary basis | `0x4AB040` | Additional transform layer |

Camera world position (float): `{0x4AAFC4, 0x4AAFC8, 0x4AAFCC}` (X, Y, Z)

### `SetCameraWorldPosition` (0x42ce50)
- **Signature**: `void __cdecl (int *pos)`
- Converts integer position to float via world scale: `float_pos = int_pos * _DAT_004749d0`
- Stores to shared globals at `0x4AAFC4/C8/CC`

### `BuildCameraBasisFromAngles` (0x42d0b0)
- **Signature**: `void __cdecl (short *angles)` — angles = {pitch, yaw, roll}
- Builds THREE independent rotation matrices from the same Euler angles:
  1. **View matrix** at `0x4AAFA0` — applies yaw, then pitch, then roll via sequential `MultiplyRotationMatrices3x3` calls
  2. **Secondary matrix** at `0x4AB070` — same rotation order applied to a fresh identity
  3. **Tertiary matrix** at `0x4AB040` — same rotation order applied to a fresh identity
- Calls `FinalizeCameraProjectionMatrices()` at the end to compute derived projection state

### `OrientCameraTowardTarget` (0x42d5b0)
- **Signature**: `void __cdecl (int *targetPos, uint yawOffset)`
- Constructs a look-at camera basis that points from the current camera position toward `targetPos`
- Algorithm:
  1. Compute direction vector: `dir = target * worldScale - cameraPos`
  2. Compute distance: `dist = 1/sqrt(dir.x^2 + dir.y^2 + dir.z^2)`
  3. Normalize direction
  4. Extract horizontal length `h = sqrt(dirX^2 + dirZ^2)`
  5. If h > 0: build full basis with up-vector derivation; if h == 0: degenerate (looking straight up/down)
  6. Apply yaw offset rotation via `MultiplyRotationMatrices3x3`
  7. Apply a **coordinate system flip** matrix `{-1,0,0, 0,1,0, 0,0,-1}` to convert from world to view convention
  8. Calls `FinalizeCameraProjectionMatrices()`

### `FinalizeCameraProjectionMatrices` (0x42d410)
- Copies the primary camera basis to a working copy at `0x4AAFE0`
- Scales all three matrix copies by `1/projectionScale` (from `DAT_00467364`)
- Then scales again by `DAT_00467368 * 0.00024414063` (depth/FOV factor; 0.00024414063 = 1/4096)
- Computes `DAT_00467360 = (int)(projectionScale * 65000.0)` — integer depth range for Z-buffer
- This is the final step before vertices can be transformed through the projection path

### `BuildWorldToViewMatrix` (0x42e750)
- **Signature**: `void __cdecl (float *outMatrix, int *directionVec, int rollAngle)`
- Builds a complete 3x3 view matrix from a direction vector and roll angle
- `rollAngle` is encoded in upper bits (>> 18 gives 12-bit angle)
- Handles degenerate case (looking straight up/down) with simplified matrix
- For general case: decomposes direction into pitch/yaw, applies roll, produces final basis via two-step matrix multiply

---

## Camera Preset System

### Preset Table
- **7 presets** indexed by `gRaceCameraPresetId` (per-view, 2 views max)
- Preset data table at `0x00463090`, stride **16 bytes** per entry:

| Offset | Type | Field |
|--------|------|-------|
| +0x00 | short | (base value, copied to `0x482EF0`) |
| +0x08 | short | Mode flag (0 = chase orbit, 1 = vehicle-relative) |
| +0x0A | short | Pitch angle (12-bit) |
| +0x0C | short | Distance/radius |
| +0x0E | short | Height offset |
| +0x10 | int | Camera offset A (copied to `0x482EA0+view*8`) |
| +0x14 | int | Camera offset B |

### `LoadCameraPresetForView` (0x401450)
- **Signature**: `void __cdecl (int actorPtr, int forceReload, int viewIndex, int saveState)`
- Reads preset `gRaceCameraPresetId` from the table
- Populates per-view camera state arrays (angles, distance, height, heading offset)
- Scales short angles by 256 to get 20-bit precision: `angle_stored = short_angle << 8`
- If `forceReload` or mode changed: also resets interpolation targets
- If `saveState`: packs preset ID + mode into `DAT_00482f48/49` (7-bit ID + 1-bit mode, 2 views)

### `ResetRaceCameraSelectionState` (0x402000)
- **Signature**: `void __cdecl (int clearOrRestore)`
- `clearOrRestore == 0`: restores saved preset/mode from packed bytes
- `clearOrRestore != 0`: resets both views to preset 0, mode 0
- Always reloads presets for both views via `LoadCameraPresetForView`
- Called during camera transitions (replay start/end)

### `CacheVehicleCameraAngles` (0x402a80)
- **Signature**: `void __cdecl (int actorPtr, int viewIndex)`
- Snapshots the vehicle's current orientation angles into per-view cache:
  - `cache[view].yaw = actor.yaw + 0x800` (rotate 180 deg — look from behind)
  - `cache[view].pitch = 0x800 - actor.pitch` (invert pitch for trailing view)
  - `cache[view].roll = -actor.roll`

---

## Vehicle Camera Modes

### Mode 0: Chase/Orbit Camera
Handled by `UpdateTracksideOrbitCamera` (0x401950) when in chase mode, and by the main `UpdateChaseCamera` (0x401590, documented separately).

#### `UpdateTracksideOrbitCamera` (0x401950)
- **Signature**: `void __cdecl (int actorPtr, int isActive, int viewIndex)`
- Orbiting camera around a vehicle, used for trackside replay
- If active and actor is alive:
  - Computes orbit speed from `DAT_00482F60[view]` (distance-based), clamped to [15, max]
  - Smooths orbit counter using `g_subTickFraction` interpolation
  - Heading = orbit angle - vehicle heading + yaw offset
  - Camera X = `sin(heading) * distance`, Z = `-cos(heading) * distance`
- Transforms offset through vehicle's rotation matrix
- Sets camera position relative to vehicle, calls `OrientCameraTowardTarget`

### Mode 1: Vehicle-Relative Camera
#### `UpdateVehicleRelativeCamera` (0x401c20)
- **Signature**: `void __cdecl (int actorPtr, int viewIndex)`
- Camera is rigidly attached to vehicle with fixed offset, smoothed orientation
- Interpolates current vehicle Euler angles toward cached angles via `g_subTickFraction` (smooth follow)
- Angular delta uses wrapping arithmetic: `((target - current + 0x800) & 0xFFF) - 0x800` for shortest-path rotation
- Adds per-view yaw offset (`DAT_00482F70[view]`) for look-left/right
- Converts float offset vector through vehicle basis matrix
- Adds velocity interpolation: `pos += velocity * g_subTickFraction`
- Calls `BuildCameraBasisFromAngles` then `SetCameraWorldPosition`

---

## Trackside Camera Profiles

### Profile Table
- `gTracksideCameraProfiles` — array of 16-byte profile entries, terminated by `-1`
- `gTracksideCameraProfileCount` — number of valid entries

| Offset | Type | Field |
|--------|------|-------|
| +0x00 | short | Behavior type (0-10, see below) |
| +0x02 | short | Track span range start |
| +0x04 | short | Track span range end |
| +0x06 | short | Span index (camera anchor) |
| +0x08 | short | Span offset |
| +0x0A | short | Height parameter |
| +0x0C | short | Spline speed (for type 6) |
| +0x0E | short | Spline node count (for type 6) |

### Behavior Types (11 total)

| Type | Name | Description |
|------|------|-------------|
| 0 | Static fixed | Fixed position, look-at vehicle, 20000ms timer |
| 1 | Static elevated A | Fixed + elevated (offset 0x200, height 0xE2, Z=-0x464) |
| 2 | Static elevated B | Same as 1 but offset 0xFE00 (opposite side of track) |
| 3 | Static default | Fixed position, default projection |
| 4, 9 | Orbit A | Orbiting camera with profile-specified height |
| 5, 10 | Orbit B | Same orbit, different profile source |
| 6 | Spline | Cubic spline path between track nodes |
| 7 | Vehicle-relative A | Attached to vehicle, offset (0, -0xC8, 0), mode 1 |
| 8 | Vehicle-relative B | Same as 7, different initial state |

### `InitializeTracksideCameraProfiles` (0x4020b0)
- Counts valid profiles (scan until -1 sentinel)
- Seeds view 0 and view 1 with the first profile's camera anchor data
- Computes initial camera world position from track span geometry (span table + offsets)
- Randomizes per-view timers: `rand() % 10000 + 10000` (10-20 seconds per profile)
- If fewer than 2 profiles OR circuit track: forces behavior type 4 (orbit) for both views

### `SelectTracksideCameraProfile` (0x402200)
- **Signature**: `void __cdecl (int actorPtr, int viewIndex)`
- Checks if the followed vehicle's current span falls within a different profile's range
- If profile changed:
  - Reads new profile's camera anchor span, offset, height
  - Computes camera world position from track geometry tables (`DAT_004c3d9c` = span table, `DAT_004c3d98` = vertex table)
  - Initializes behavior-specific state via switch on type (0-10)
  - Resets projection scale to 0x1000 (default FOV)

### `UpdateStaticTracksideCamera` (0x402950)
- **Signature**: `void __cdecl (int actorPtr, int viewIndex)`
- Reads camera position from pre-computed anchor data (no movement)
- Y-position includes dynamic terrain height sampling: `vertexTable[anchorSpan + heightOffset].y + heightParam`
- Target = vehicle position + velocity * subTickFraction (interpolated)
- Calls `SetCameraWorldPosition` + `OrientCameraTowardTarget`

### `UpdateSplineTracksideCamera` (0x402ad0)
- **Signature**: `void __cdecl (int actorPtr, int viewIndex, int splineType)`
- Evaluates a cubic spline camera path defined by 4 control points
- 6 predefined spline templates (selected by `splineType`, 8 shorts per template):
  - Templates define span offsets and height modifiers for the 4 control points
  - Terminated by sentinel value 0xFFFF
- Advances spline parameter by `DAT_00482F80[view]` per frame, clamped to [0, 0xFFF]
- Calls `BuildCubicSpline3D` then `EvaluateCubicSpline3D` to get interpolated position
- Dynamically adjusts projection scale (FOV) based on distance to target:
  - `projScale = distance * 4 * baseTimer / 4096`
  - Clamped to [0x1000, 20000]
- Calls `RecomputeTracksideProjectionScale` after FOV update

---

## Camera Transition System

### `InitializeRaceCameraTransitionDuration` (0x40a480)
- Sets `g_cameraTransitionActive = 0xA000` (40960)
- This is the initial 5.3-second fly-in at race start

### `UpdateRaceCameraTransitionTimer` (0x40a490)
- Called each simulation tick
- Decrements by `0x100` (256) per tick
- Divides remaining time by `0x2800` (10240) to get HUD indicator level (1-4 → display level 2-5)
- When counter reaches 0:
  - If replay/playback mode: restores saved camera preset (from packed state)
  - If normal mode: resets to default preset 0
  - Clears both view HUD indicators
- When indicator reaches level 0: clears `gRaceCameraTransitionGate` (allows player camera control)

### `UpdateCameraTransitionHudIndicator` (0x40a260)
- **Signature**: `void __cdecl (int viewIndex, int actorIndex)`
- During time trial: always shows indicator 0 (hidden)
- Otherwise: reads actor byte at offset +0x383 (camera behavior byte) + 2 as indicator level

### Transition Timeline
```
Tick 0:     g_cameraTransitionActive = 0xA000 (40960)
Per tick:   -= 0x100 (256)
Tick 160:   counter = 0 → transition complete
Duration:   160 ticks = ~5.3 seconds at 30Hz
HUD levels: 4→3→2→1→0 (each ~1.3s)
```

---

## Key Globals Reference

### Camera State Arrays (per-view, indexed by viewIndex * stride)

| Address | Stride | Type | Description |
|---------|--------|------|-------------|
| `0x482E0C` | 4 | int | Pitch angle (20-bit, from preset) |
| `0x482E18` | 8 | short[3] | Cached vehicle orientation angles |
| `0x482EA0` | 8 | short[4] | Camera offset vector (from preset) |
| `0x482EB0` | 4 | float | Interpolated height target |
| `0x482EB8` | 4 | int | Height sample offset |
| `0x482EC0` | 4 | uint | Track span offset (profile) |
| `0x482EC8` | 4 | uint | Behavior type (profile) |
| `0x482ED0` | 4 | float | Distance (scaled from preset) |
| `0x482ED8` | 12 | int[3] | Orbit offset vector |
| `0x482EF0` | 4 | int | Smoothing/orbit base parameter |
| `0x482F00` | 4 | float | Interpolated distance |
| `0x482F08` | 4 | int | Spline parameter (0..0xFFF) |
| `0x482F10` | 4 | float | Target height (from preset) |
| `0x482F18` | 4 | int | Heading angle (20-bit) |
| `0x482F20` | 4 | uint | Track span index (profile anchor) |
| `0x482F28` | 4 | int | Reserved/zero |
| `0x482F30` | 12 | int[3] | Camera world position (integer) |
| `0x482F50` | 8 | short[4] | Transformed offset (short) |
| `0x482F60` | 4 | int | Orbit distance accumulator |
| `0x482F68` | 4 | uint | Spline node count |
| `0x482F70` | 4 | int | Yaw offset (look-left/right) |
| `0x482F78` | 4 | int | Reserved/zero |
| `0x482F80` | 4 | int | Spline advance rate |
| `0x482F88` | 4 | int | Anchor X position |
| `0x482F90` | 4 | uint | Height parameter |
| `0x482F98` | 4 | int | Anchor Z position |
| `0x482FA0` | 4 | int | Stored pitch angle |

### Scalar Globals

| Address | Type | Description |
|---------|------|-------------|
| `gRaceCameraPresetId` | int | Current preset index (view 0) |
| `gRaceCameraPresetMode` | int | Current mode: 0=chase, 1=vehicle-relative |
| `DAT_00482FD4` | int | Preset index (view 1) |
| `DAT_00482FDC` | int | Mode (view 1) |
| `DAT_00482F48` | byte | Packed save: view0 (7-bit ID + 1-bit mode) |
| `DAT_00482F49` | byte | Packed save: view1 |
| `DAT_00482FC8` | int[2] | Current profile index (per-view) |
| `gTracksideCameraProfileCount` | int | Number of valid trackside profiles |
| `g_cameraTransitionActive` | int | Transition timer (0=inactive, >0=transitioning) |
| `gRaceCameraTransitionGate` | int | Nonzero while transition blocks player input |
| `DAT_00467364` | float | Projection scale (FOV control) |
| `DAT_00467368` | int | Depth/FOV factor (integer, used by FinalizeCameraProjectionMatrices) |
| `DAT_00463088` | int[2] | Per-view trackside timer (ms-like countdown) |
| `DAT_00463080` | uint[2] | Per-view yaw offset for OrientCameraTowardTarget |

### Shared Projection Matrices

| Address | Size | Description |
|---------|------|-------------|
| `0x4AAFA0` | float[9] | Primary camera basis (view rotation) |
| `0x4AAFC4` | float[3] | Camera world position (float) |
| `0x4AAFE0` | float[9] | Working copy of primary basis (scaled for projection) |
| `0x4AB040` | float[9] | Tertiary rotation matrix (projection path) |
| `0x4AB070` | float[9] | Secondary rotation matrix (projection path) |

---

## Summary

The camera system is a two-layer architecture:
1. **High-level mode selection** — preset system (7 player presets) or trackside profile system (11 behavior types per track)
2. **Low-level camera transform** — shared `SetCameraWorldPosition` + either `BuildCameraBasisFromAngles` (vehicle-relative) or `OrientCameraTowardTarget` (look-at) → `FinalizeCameraProjectionMatrices`

The math library uses a **12-bit angle system** (4096 units per revolution) with precomputed sin/cos lookup tables in both float and Q12 fixed-point formats. All 3D transforms go through a software pipeline using the global render transform at `DAT_004bf6b8` (float[12]: 3x3 rotation + translation). Matrices are row-major float[9]. The `AngleFromVector12` function provides a full atan2 replacement using an arctan LUT.
