# Visual Effects Systems — Deep Dive

> Covers tire tracks, particles, smoke, wheel rendering, vehicle visual effects, HUD overlays, and sky rotation in TD5_d3d.exe.

---

## 1. Tire Track System

### Pool Architecture

The tire track emitter pool is managed through a global allocation at `DAT_004c375c` (pool base pointer) with search cursor `DAT_004c3758`.

- **Pool capacity**: 80 slots (0x50)
- **Slot stride**: 0xEC bytes (236 bytes per emitter record)
- **Control byte offset**: +0xD8 within each slot (bit 0 = active flag)
- **World anchor**: +0xDC/+0xE0/+0xE4 (X/Y/Z world position copied from wheel hardpoint)
- **Lifetime counter**: +0xDA (16-bit, starts at 0)
- **Intensity byte**: +0xD9 (initial value from spawn param, modulated by speed)

### Emitter Descriptor Table

Per-wheel emitter descriptors (7 bytes each) stored at `DAT_004c3720`, indexed by `(wheelIndex + DAT_004aadec * 4) * 7`:

| Offset | Field | Description |
|--------|-------|-------------|
| +0 | actorSlot | Source vehicle actor index |
| +1 | wheelId | Wheel hardpoint ID (0-3) |
| +2 | active | 1 = emitting, 0 = dead |
| +3 | poolSlot | Index into the 80-slot pool |
| +4 | initialAlpha | Starting opacity (0x28 front, 0x37 rear) |
| +5 | width | Track mark width (0x1A for all) |
| +6 | width2 | Duplicate of width |

### `AcquireTireTrackEmitter` (0x43F030)

Allocator uses a **roving cursor** starting at `DAT_004c3758`. Scans forward from cursor looking for first inactive slot (bit 0 of +0xD8 clear). If cursor reaches 0x50, wraps to slot 0 and restarts. Returns -1 if no free slot found.

On allocation:
1. Writes 7-byte emitter descriptor at `DAT_004c3720[index*7]`
2. Copies wheel world position from actor hardpoint into pool slot +0xDC/+0xE0/+0xE4
3. Sets control byte +0xD8 to 1 (active), clears lifetime counter +0xDA to 0
4. Seeds intensity byte +0xD9 from caller parameter

### `UpdateTireTrackEmitters` (0x43FAE0)

Master dispatcher per vehicle actor. Reads **drivetrain layout** from `*(actor+0x1BC)+0x76`:

| Mode | Meaning | Action |
|------|---------|--------|
| 1 | RWD | Calls `UpdateRearTireEffects` (wheels 2,3) |
| 2 | FWD | Calls `UpdateFrontTireEffects` (wheels 0,1) |
| 3 | AWD | Calls both front + rear |

Also unconditionally calls `UpdateRearWheelSoundEffects` and `UpdateFrontWheelSoundEffects` for all vehicles.

**Guard**: `DAT_004aad60 == 0` gates all tire/sound updates (disabled during replays or pause).

### `UpdateFrontTireEffects` (0x43F960) / `UpdateRearTireEffects` (0x43F7E0)

Mirror functions for front (wheels 0,1) and rear (wheels 2,3). Gated by surface flag bitmask at actor+0x376 (bit 1 = front slip, bit 0 = rear slip).

Behavior when active:
1. **Emitter allocation**: If wheel's track byte (actor+0x371..0x374) is 0xFF, calls `AcquireTireTrackEmitter` with params (wheelId, actorSlot, wheelId, 0x37, 0x1A)
2. **Smoke spawn**: Random probability test `rand() % 50 < lateralSlip/2`. Picks random left/right wheel, spawns dual smoke variants (type 0 and type 1) at wheel position
3. **Intensity modulation**: Sets `DAT_004c3725[slot*7]` to `lateralSlip >> 2` (proportional to slip magnitude at actor+0x33C)
4. **Surface reduction**: On surface types 2,4,5,6,9: intensity halved (right-shift by 1) -- these are hard surfaces that produce less visible marks

### `UpdateFrontWheelSoundEffects` (0x43F420) / `UpdateRearWheelSoundEffects` (0x43F600)

Mirror functions for front (wheels 0,1, threshold 15001) and rear (wheels 2,3, threshold 10001).

**Speed thresholds**:
- Front wheels: speed >= 0x3A99 (15001) to activate
- Rear wheels: speed >= 0x2711 (10001) to activate

When above threshold:
1. Subtract base (15000 front / 10000 rear), check remainder > 0x5000 (20480)
2. If so: spawn smoke from wheel position (variant 0 on certain surface types + always variant 1)
3. Allocate tire track emitter if not already active
4. Set track intensity to `(speed - base) >> 11`
5. Surface-type reduction: halve on hard surfaces (types 2,4,5,6,9)

When below threshold or guards active:
- Release emitter: set active flag to 0, mark wheel byte as 0xFF (unallocated)

Guard flags:
- actor+0x379: nonzero forces release (vehicle airborne or special state)
- actor+0x376 bit 1/0: surface contact flags for front/rear axle
- actor+0x37C bitmask: per-wheel grounded flags

---

## 2. Particle System

### Pool Architecture

- **Base address**: `DAT_004a318f` (particle slot array)
- **Slots per view**: 100 (loop counter `iVar2 < 100` in UpdateRaceParticleEffects)
- **2 independent banks**: stride 0x1900 (6400 bytes) per view, selected by `param_1` (view index)
- **Total**: 200 slots across 2 views (100 per player in split-screen)
- **Slot stride**: 0x40 bytes (64 bytes per particle record)

### Slot Layout

| Offset | Field | Description |
|--------|-------|-------------|
| -0x1D (relative to callback ptr) | flags | Bit 7 (0x80) = active |
| +0x00 | callback | Function pointer to per-frame update |
| -0x1F (slot+0x00) | type byte | Particle type identifier |

### `UpdateRaceParticleEffects` (0x429790)

Simple dispatcher:
1. Sets `DAT_004a63d4 = param_1` (current view index)
2. Iterates 100 slots (stride 0x40 = 64 bytes between callback pointers)
3. For each: if `flags & 0x80` set, calls `callback(slotIndex)`
4. Callback is a **code pointer** stored at slot+0x1D relative offset -- true callback-driven architecture

### Overlay Sprite Pool

Separate from particle slots, a **sprite batch pool** at `DAT_004a6370`:
- 50 sprite entries per view (0x32)
- Stride 0xB8 (184 bytes) per sprite batch, stored at `DAT_004a63d8 + entryIndex * 0xB8`
- Each smoke particle allocates one sprite batch entry as its visual representation

### Smoke Spawn Functions

All smoke spawners share the same pattern:

1. **Find free particle slot**: Linear scan of 100 slots looking for inactive (flags byte == 0)
2. **Find free sprite batch**: Linear scan of 50 entries at `DAT_004a6370 + view*50`
3. **Bind SMOKE texture**: `FindArchiveEntryByName("SMOKE")` fetches the shared smoke sprite sheet
4. **Initialize sprite quad**: Calls `BuildSpriteQuadTemplate` with UV coords from texture atlas
5. **Set velocity**: Derived from vehicle heading and speed

#### `SpawnVehicleSmokeVariant` (0x429A30)
- Direct spawn at explicit position with variant type (0 = white smoke, 1 = dark/tire smoke)
- No probability gate -- always spawns if slot available

#### `SpawnVehicleSmokeSprite` (0x429CF0)
- 50% probability gate: `rand() % 10 > 5`
- Seeds velocity from actor heading vector

#### `SpawnVehicleSmokePuffAtPoint` (0x429FD0)
- No probability gate; spawns at arbitrary world position
- Used by `SpawnRandomVehicleSmokePuff` for random body smoke

#### `SpawnVehicleSmokePuffFromHardpoint` (0x42A290)
- Speed-proportional probability: `rand() % 1000 < speed/200`
- Reads hardpoint position from actor struct based on hardpoint index
- Used by `SpawnRearWheelSmokeEffects` for tire smoke on states 10 (0xA) and 12 (0xC)

#### `SpawnRandomVehicleSmokePuff` (0x401370)
- **Guards**: speed < 4000, `actor+0x33E` (lateral slip) > 200, speed > 0
- Probability: `rand() % speed < 500` -- higher speed = lower chance per frame
- Position: midpoint of two body corners (actor+0xA8..0xBC) offset up by 0x7800

#### `SpawnRearWheelSmokeEffects` (0x401330)
- Triggered on surface state 10 or 12 (actor+0x370)
- Spawns from hardpoints 2 and 3 (rear left/right wheels)

---

## 3. Wheel Rendering

### `InitializeVehicleWheelSpriteTemplates` (0x446A70)

Sets up wheel billboard geometry for one vehicle:

1. **Texture sources**: Loads `WHEELS` and `INWHEEL` entries from archive (outer rim + inner hub)
2. **Billboard geometry**: Creates 8 quad strips per wheel (8 segments around the rim), each 0xB8 bytes
3. **Outer wheel quads**: 4 wheels x 8 segments = 32 quad batches at `DAT_004c65b0` (stride 0x5C0 per wheel)
4. **Inner wheel quads**: 4 wheels x 4 quads at `DAT_004c43b8`
5. **Shadow quads**: 4 wheels x 4 quads at `DAT_0048dd08`
6. **Rim profile**: 9 points generated around a circle using `SinFloat12bit`/`CosFloat12bit` at 0x2000 increments (45-degree steps), scaled by wheel radius from car config (+0x82) and a factor of 0.76171875
7. **Color palette**: `COLOURS` texture loaded for paint-dependent wheel tint; UV anchored at `texCoord+1`

### `InitializeWheelPaletteUvTable` (0x446EA0)

Builds a UV lookup table at `DAT_004cefb4` for 64x64 wheel color tiles:

- Layout: 4 columns x N rows in a texture atlas
- For tile index `i`: `U = (i & 3) * 64.0 + 0.5`, `V = (i >> 2) * 64.0 + 0.5`
- Half-pixel offset (+0.5) for proper texel centering on D3D6

### `RenderVehicleWheelBillboards` (0x446F00)

Per-frame wheel rendering for one vehicle:

**Camera-facing orientation**:
1. Reads vehicle yaw from `actor+0x340` and steer angle from `actor+0x342`
2. Converts to fixed-point rotation using `CosFixed12bit`/`SinFixed12bit` with `angle * -4`
3. Front wheels (0,1) apply additional steering rotation matrix via `MultiplyRotationMatrices3x3`
4. Each wheel's billboard orientation is built from vehicle rotation + camera-relative transform

**Spin animation**:
- Speed value from `actor+0x314` (absolute speed)
- Frame index: `abs(speed) >> 8`, then `>> 6` to get 4-frame index (0-3)
- Clamped to range 0-3 (4 spin frames)
- UV selection: `frame & 1` gives column (0 or 1), `frame >> 1` gives row (0 or 1)
- Each frame selects a 32x32 tile from the wheel texture atlas: `U = col*32 + base_U`, `V = row*32 + base_V`

**Rendering pipeline**:
1. Transform 4 wheel hardpoints from model space to view space via `TransformShortVec3ByRenderMatrixRounded`
2. For each wheel: build 8 outer rim quads + write vertex positions via `WriteTransformedShortVector`
3. Both sides rendered: vertices negated for back-face quad
4. All quads submitted via `QueueTranslucentPrimitiveBatch` for depth-sorted rendering

**Left/right mirroring**: Odd-numbered wheels (1,3) use negated orientation vectors to mirror the billboard for the opposite side of the vehicle.

### `InitializeVehicleShadowAndWheelSpriteTemplates` (0x40BB70)

Master initialization for all vehicles at race start:

1. Iterates `DAT_004aaf00` (racer count) vehicles
2. Copies car config pointer from `DAT_004c3d10` table to actor+0x1B0
3. Calls `ComputeVehicleSuspensionEnvelope` per vehicle
4. Loads `shadow` texture from archive
5. **Shadow quad geometry**: Two template sizes per vehicle:
   - Inner shadow: UV bounds 0x42840000 (66.0) -- close shadow
   - Outer shadow: UV bounds 0x42fc0000 (126.0) -- extended penumbra
6. Shadow color: 0xFFFFFFFF (white, multiplicative blend via translucent pipeline)
7. `DAT_0048f070` = shadow Y offset: -36 (normal) or -18 (replay mode, `g_inputPlaybackActive`)
8. `DAT_0048dc48` = shadow float offset: -22.0 (normal) or -4.0 (replay)

---

## 4. Wheel Physics

### `RefreshVehicleWheelContactFrames` (0x403720)

Builds per-wheel contact frames used by the suspension integrator:

1. Loads vehicle render matrix from actor orientation
2. For each of 4 wheels:
   - Reads wheel vertical offset from suspension travel table
   - Applies suspension compression: subtracts `(suspTravel * 0xB5) >> 8` (~70.7% scaling factor)
   - Transforms wheel position from model space to world space
   - Calls `UpdateActorTrackPosition` to locate wheel on track
   - Calls `ComputeActorTrackContactNormalExtended` for terrain normal + ground height
   - Computes wheel velocity delta (world position change since last frame)
   - **Ground contact test**: if `abs(verticalForce) < 0x200` (512), clamp to 0 (dead zone)
   - If force < 0x801 (2049): wheel is grounded, snap to terrain height
   - If force >= 0x801: wheel is airborne, set bit in `actor+0x1BE` contact mask, apply 12000 gravity
3. Writes contact mask to `actor+0x1BE` (4 bits, one per wheel)
4. Second pass: 4 additional sample points for suspension envelope (no gravity, just contact normals)

### `IntegrateWheelSuspensionTravel` (0x403A20)

Spring-damper integrator for 4-wheel suspension + central roll/pitch:

**Parameters** from car config at `param_2`:
| Offset | Name | Purpose |
|--------|------|---------|
| +0x5E | spring_k | Spring stiffness coefficient |
| +0x60 | damping | Viscous damping coefficient |
| +0x62 | response | Track-following response gain |
| +0x64 | travel_max | Maximum suspension travel (+/-) |
| +0x66 | velocity_damp | Velocity-proportional damping |

**Per-wheel integration** (4 iterations):
1. Compute track error = weighted sum of lateral + longitudinal deviation (`param_3`, `param_4` weights)
2. Scale by response gain (+0x62)
3. Add velocity damping: `travel_vel * sVar4 >> 8`
4. Add current force accumulator (actor+0x2DC..+0x2E8, stride 4)
5. Apply spring: subtract `position * spring_k >> 8`
6. Apply damping: subtract `force * damping >> 8`
7. Dead zone: if |force| < 16, zero it out
8. Integrate: `position += force`
9. Clamp: position to [-travel_max, +travel_max], zero force at limits

**Central body roll/pitch** (5th integration, actor+0x2CC/+0x2D0):
- Uses average of front and rear axle positions
- Same spring-damper formula
- Controls overall vehicle body lean

---

## 5. HUD Effects

### `RenderHudRadialPulseOverlay` (0x439E60)

The checkpoint pulse effect -- an expanding translucent pentagon:

**Timer**: `DAT_004b0fa0` (float, starts >= 0 when triggered)
- Advances by `deltaTime * 4.2` per frame until reaching 3000.0
- Controls both expansion and fade

**Alpha**: `(int)DAT_004b0fa0`, clamped to [0, 255]
- Color: `alpha * 0x10101 | 0xFF000000` (uniform gray, fully opaque base)

**Geometry** -- 5 vertices forming a regular pentagon:
1. Radius = `renderWidth * timer * 0.0015625` (expands proportionally to screen width)
2. For each of 5 segments, computes inner (0.5x radius) and outer (1.0x radius) vertices
3. Uses `CosFloat12bit`/`SinFloat12bit` for vertex positions
4. Angular step: `-0x33332` per segment (72 degrees in fixed-point = 360/5)
5. Initial angle from `(int)DAT_004b0fa0` -- pentagon **rotates** as it expands

**Rendering**:
- 5 trapezoidal quads, each connecting inner and outer vertex pairs
- All quads have z-depth 0x4300199A (~128.1) -- near the camera
- Submitted to `DAT_004b0c08`..`DAT_004b0ee8` (5 pre-allocated primitive batches, stride 0xB8)
- Each batch submitted via `SubmitImmediateTranslucentPrimitive` (bypasses depth sort queue)

**Sky rotation coupling**: `DAT_004b08c0 += deltaTime * 3328.0` -- the checkpoint pulse also advances the sky dome rotation counter, creating a visual "time flash" effect.

### `UpdateWantedDamageIndicator` (0x43D4E0)

3D-projected damage bar for cop chase mode:

**Guards**: `gWantedModeEnabled != 0` AND `param_1 == DAT_004bf504` (tracked suspect vehicle)

**Projection**:
1. Transforms a fixed 3D offset (from `DAT_00474868`) through the current render matrix into screen space
2. Screen-space position: `X = worldX * (128.0 / worldZ)`, `Y = worldY * (128.0 / worldZ)` -- perspective divide
3. Scale factor: `renderWidth * 0.0015625` (resolution-independent)

**Damage quad** (DAMAGE sprite):
- 16x16 scaled quad centered on projected position
- Z-depth: 0x43000000 (128.0, very close to camera)
- Queued via `QueueTranslucentPrimitiveBatch`

**Health bar** (DAMAGEB1 sprite):
- Width: `renderWidth * 0.00024414063 * (0x1000 - damageValue) * quadSize * scale`
- Where `damageValue` = `DAT_004bead4[actorIndex]` (per-actor 16-bit damage, 0x1000 = full health)
- Positioned below the damage icon with 4-pixel-scaled offset
- Height: `quadSize - healthBarHeight`

### `InitializeWantedHudOverlays` (0x43D2D0)

One-time setup when wanted mode is active:

1. Initializes damage tracking: `DAT_004bead4` = 0x10001000 (two 16-bit values, both at max health)
2. Sets `DAT_004bf504 = -1` (no tracked suspect yet)
3. Loads `DAMAGE` texture from archive, builds sprite quad at `DAT_004beae0` (0xB8 bytes)
4. Loads `DAMAGEB1` texture, builds sprite quad at `DAT_004bf448`
5. UV coordinates computed from texture atlas metadata (position + size from archive entry +0x2C..+0x38, with +0.5/-0.5 texel bias)
6. Vertex colors: all white (0xFFFFFFFF)
7. Z-depth: 0x43000000 (128.0)

---

## 6. Vehicle Visual Effects

### `RenderVehicleTaillightQuads` (0x4011C0)

Renders brake/tail light quads as translucent billboards:

**Guard**: `actor+0x346+0x27` (gap_0346[0x27]) must be nonzero (tail light capability flag)

**Brightness decay**:
- `DAT_00482dd0[actorIndex]` = current brightness (byte, 0-255)
- If brake active (`actor+0x376+6 & 0xF != 0`): increment by 8, clamp at 127
- If brake off: decay by half (right-shift by 1) until 0
- Result: smooth fade-in (16 frames to full) and exponential fade-out

**Rendering** (2 tail lights, offsets 0x60 and 0x68 in vehicle config):
1. Compute track contact normal at vehicle position
2. For each light:
   - Load 4-vertex billboard template from `DAT_00482dcc + actorIndex * 0x170`
   - Set vertex color to 0xFF909090 (gray tint)
   - Transform hardpoint position to view space
   - Offset Z by -24.0 (slight forward bias to prevent z-fighting)
   - Align quad to global orientation (camera-facing)
   - Write 4 corners from fixed offset vectors (`DAT_00463030/38/40/48`)
   - Submit via `QueueTranslucentPrimitiveBatch`

### Wheel Jitter System (CUT FEATURE)

Three functions that add speed-proportional random displacement to per-wheel suspension:

All three call `GetDamageRulesStub` (0x42C8D0) which **always returns 0** -- a stubbed-out `rand()` replacement. This effectively disables all wheel jitter, making it a **cut feature**.

**Target fields**: actor+0x2EC, +0x2F0, +0x2F4, +0x2F8 (4 per-wheel suspension force accumulators)

#### `ApplyRandomWheelJitterHighSpeed` (0x43D830)
- Speed factor: `abs(speed) >> 8` (full speed magnitude)
- Random mask: `rand() & 7` -> range [0,7], centered to [-4,+3]
- Applied independently to each of 4 wheels
- **With stub**: constant -4 bias (no randomness, deterministic lopsided pull)
- **22 call sites** across the dispatch table at 0x474948

#### `ApplyRandomWheelJitterLowSpeed` (0x43D910)
- Speed factor: `abs(speed) >> 9` (**half** the high-speed magnitude)
- Same random mask: `rand() & 7` -> [-4,+3]
- Same 4-wheel independent application
- Used for wheel state index 1 only

#### `ApplyRandomWheelJitterSynchronized` (0x43D9F0)
- Speed factor: `abs(speed) >> 7` (**double** the high-speed magnitude)
- Random mask: `rand() & 0xF` -> range [0,15], centered to [-8,+7]
- **Paired axle application**: front pair (0x2EC, 0x2F0) get same value, rear pair (0x2F4, 0x2F8) get same value
- Creates a **rocking/pitching** motion rather than per-wheel shimmy
- Used for wheel state index 8

#### Dispatch Table (0x474948)

20 entries mapping wheel-state indices to jitter functions:
- **HighSpeed**: indices 0, 2-7, 9-19 (most states)
- **LowSpeed**: index 1
- **Synchronized**: index 8

#### Restoration

Patching `GetDamageRulesStub` to call `_rand` (at 0x448157) would restore:
- Per-wheel random shimmy proportional to speed (visible wheel vibration)
- Paired axle wobble creating rocking motion on damaged vehicles
- Combined with collision impulse randomization (12 additional call sites)
- See `re/patches/cut-feature-restoration-guide.md` for byte-level patch table

---

## 7. Sky/Environment

### `AdvanceGlobalSkyRotation` (0x43D7C0)

Minimal function -- advances the sky dome rotation counter:

```
if (g_audioOptionsOverlayActive == 0) {
    DAT_004bf500 += 0x400;
}
```

- `DAT_004bf500`: global sky rotation angle (fixed-point, wraps at 16-bit boundary)
- Increment: 0x400 per tick (~1 degree per frame in fixed-point)
- **Pauses** when the audio options overlay is open (`g_audioOptionsOverlayActive != 0`)
- Also advanced by `RenderHudRadialPulseOverlay` by `deltaTime * 3328.0` during checkpoint pulse events -- creates a visible sky "flash" synchronized with the pulse

---

## Summary Table

| System | Pool Size | Stride | Base Address | Render Method |
|--------|-----------|--------|--------------|---------------|
| Tire track emitters | 80 | 0xEC | DAT_004c375c | Translucent quad strips |
| Particle slots (per view) | 100 | 0x40 | DAT_004a318f | Callback-driven update |
| Smoke sprite batches (per view) | 50 | 0xB8 | DAT_004a63d8 | Textured billboard quads |
| Wheel billboards (per wheel) | 8 segments | 0xB8 | DAT_004c65b0 | Camera-facing oriented quads |
| Inner wheel quads | 4 per wheel | 0x170 | DAT_004c43b8 | Translucent textured quads |
| Shadow quads | 2 per vehicle | 0x170 | DAT_0048dd08 | Ground-projected quads |
| Taillight quads | 2 per vehicle | 0xB8 | DAT_00482dcc | Translucent billboard |
| Pentagon pulse | 5 segments | 0xB8 | DAT_004b0c08 | Immediate translucent submit |
| Wanted damage bar | 2 quads | 0xB8 | DAT_004beae0 | Queued translucent batch |

### Key Actor Struct Offsets (Visual Effects)

| Offset | Type | Description |
|--------|------|-------------|
| +0x310 | int | Current absolute speed (world units/tick) |
| +0x314 | int | Signed speed (for direction) |
| +0x33C | short | Lateral slip magnitude |
| +0x33E | short | Slip threshold for smoke |
| +0x340 | short | Vehicle yaw angle |
| +0x342 | short | Steering angle |
| +0x370 | char | Surface type code (determines smoke/track behavior) |
| +0x371-374 | char[4] | Per-wheel tire track emitter IDs (0xFF = none) |
| +0x375 | byte | Actor slot index (for emitter lookup) |
| +0x376 | byte | Surface contact flags (bit 0=rear, bit 1=front) |
| +0x379 | char | Airborne/special state flag |
| +0x37C | byte | Per-wheel grounded bitmask |
| +0x2EC-2F8 | int[4] | Per-wheel suspension force accumulators (jitter target) |
| +0x2CC | int | Central body roll accumulator |
| +0x2D0 | int | Central body roll velocity |

### Surface Type Codes (actor+0x370)

| Code | Surface | Smoke Type | Track Intensity |
|------|---------|------------|-----------------|
| 1 | Asphalt dry | Variant 0 (white) | Full |
| 2 | Asphalt wet | Variant 0 | Half |
| 3 | Dirt | Variant 0 | Full |
| 4 | Gravel | None | Half |
| 5 | Mixed | Variant 0 | Half |
| 6 | Ice | None | Half |
| 9 | Sand | None | Half |
| 10 (0xA) | Burnout start | Hardpoint smoke | N/A |
| 12 (0xC) | Burnout sustain | Hardpoint smoke | N/A |
