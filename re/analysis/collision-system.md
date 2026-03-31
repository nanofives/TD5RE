# Test Drive 5 -- Collision Detection & Response System

## Overview

TD5 has two distinct collision subsystems:

1. **Vehicle-to-Vehicle (V2V)** -- Oriented bounding box (OBB) corner-in-box test with impulse-based response
2. **Vehicle-to-World (V2W)** -- Track segment boundary edge tests using signed distance (dot-product) against span edge normals

Both operate in **fixed-point 12-bit arithmetic** (values shifted by 12 = 4096 scale factor). There is no continuous collision detection; all tests are discrete per-tick.

---

## Integration in RunRaceFrame (0x42B580)

The collision phases occur in this order within the per-tick simulation loop:

```
RunRaceFrame tick:
  1. UpdateRaceActors (0x436A70)
     -- per-actor: ComputeAIRubberBandThrottle, UpdateActorTrackBounds, etc.
     -- per-actor: UpdateVehicleActor (0x406650)
        a. ClampVehicleAttitudeLimits (reads DAT_00463188 collisions toggle)
        b. UpdatePlayerVehicleDynamics / UpdateAIVehicleDynamics
        c. IntegrateVehiclePoseAndContacts (0x405E80) -- position integration + gravity
        d. UpdateActorTrackSegmentContactsReverse (0x4070E0) -- V2W rear boundary
        e. UpdateActorTrackSegmentContactsForward (0x406F50) -- V2W front boundary
        f. UpdateActorTrackSegmentContacts (0x406CC0) -- V2W lateral boundaries
     -- traffic actors: UpdateTrafficActorMotion -> IntegrateScriptedVehicleMotion
        -> ComputeActorWorldBoundingVolume (V2W ground collision for scripted vehicles)

  2. ResolveVehicleContacts (0x409150) -- V2V broadphase + narrowphase + impulse
  3. UpdateTireTrackPool (0x43EB50)
  4. ... camera, render, present ...
```

Key: V2W contact runs **inside** each actor's per-frame update. V2V contact runs **after** all actors have integrated, as a global pairwise pass.

---

## Vehicle-to-Vehicle Collision

### Functions

| Address | Name | Role |
|---------|------|------|
| 0x409150 | `ResolveVehicleContacts` | Top-level: builds broadphase, dispatches pairs |
| 0x408A60 | `ResolveVehicleCollisionPair` | Per-pair: broadphase AABB check, narrowphase OBB, binary-search TOI, dispatch impulse |
| 0x408570 | `CollectVehicleCollisionContacts` | Narrowphase: 8-corner OBB penetration test, returns contact bitmask + records |
| 0x4079C0 | `ApplyVehicleCollisionImpulse` | Per-contact: impulse computation, velocity update, damage, sound |
| 0x408F70 | `ResolveSimpleActorSeparation` | Fallback: sphere-based push for non-standard actors |
| 0x409520 | `CheckAndUpdateActorCollisionAlignment` | Post-collision: detects if vehicle is stuck/flipped and resets |
| 0x428A60 | `PlayVehicleCollisionForceFeedback` | Force feedback dispatch based on contact side |

### Broadphase: Spatial Grid + AABB

`ResolveVehicleContacts` (0x409150):

1. **Builds per-actor AABBs** at `DAT_00483050` (stride 20 bytes per actor, 5 ints):
   - `[min_x, min_z, max_x, max_z, chain_link]`
   - Bounds = world position (>>8) +/- bounding radius from car definition at `cardef+0x80`
2. **Spatial bucket hash**: Actors are inserted into a grid at `DAT_00483554` indexed by `(span_heading >> 2)`. Each bucket is a linked list via chain bytes.
3. **Pair enumeration**: For each actor, walks its bucket's linked list (up to 17 entries). For each candidate pair where `actor_i < actor_j`:
   - If **both** actors have `gap_0376[3] == 0` (normal state) AND both have `gap_0376[6] < 0x0F` (not in damage-lockout):
     - Calls `ResolveVehicleCollisionPair` (full OBB collision)
   - Otherwise:
     - Calls `ResolveSimpleActorSeparation` (sphere push-apart)

### Broadphase AABB Test

`ResolveVehicleCollisionPair` (0x408A60) starts with 4 axis-aligned overlap checks:
```
if (A.max_x <= B.min_x) return;  // no X overlap
if (B.max_x <= A.min_x) return;
if (A.max_z <= B.min_z) return;  // no Z overlap
if (B.max_z <= A.min_z) return;
```
Only if all 4 pass does it proceed to narrowphase.

### Narrowphase: 8-Corner OBB Penetration Test

`CollectVehicleCollisionContacts` (0x408570):

Takes two actor pointers and their current poses (position + heading angles). The car definition at `actor+0x1B8` provides:
- `cardef+0x04` = half-width (short)
- `cardef+0x08` = half-length (short)
- `cardef+0x14` = rear offset (short, typically negative)

The function:
1. Computes the heading delta between the two vehicles
2. Transforms vehicle B's 4 corners into vehicle A's local frame, tests each against A's box extents
3. Transforms vehicle A's 4 corners into vehicle B's local frame, tests each against B's box extents
4. Returns a **bitmask** (bits 0-7) indicating which of the 8 corners are penetrating
5. For each penetrating corner, stores a 4-short contact record: `[local_x, local_z, delta_x, delta_z]`

The bitmask encoding:
- Bits 0-3: B's corners inside A's box (A is "struck")
- Bits 4-7: A's corners inside B's box (B is "struck")

### Binary-Search Time-of-Impact Refinement

`ResolveVehicleCollisionPair` performs a **7-iteration binary search** to refine the contact moment:

```c
// Start at half-step back from current positions
pos_A -= vel_A / 2;   pos_B -= vel_B / 2;
heading_A -= omega_A / 2;  heading_B -= omega_B / 2;
elasticity = 0x80;

for (7 iterations) {
    step /= 2;
    contacts = CollectVehicleCollisionContacts(A, B, ...);
    if (contacts != 0) {
        // Still overlapping -- step backward
        pos -= step;  heading -= step;  elasticity -= step;
    } else {
        // No overlap -- step forward
        pos += step;  heading += step;  elasticity += step;
    }
}
```

This finds the approximate moment of first contact with 7-bit precision (1/128 of a tick).

### Impulse Model

`ApplyVehicleCollisionImpulse` (0x4079C0):

The impulse uses a **2D rigid-body collision formula** in the plane defined by the contact angle:

1. **Rotate velocities** into the contact-normal frame using `CosFixed12bit(angle)` / `SinFixed12bit(angle)`
2. **Determine contact axis** (lateral vs. longitudinal) based on which box face was penetrated:
   - Compares the contact point's position against the struck vehicle's half-extents
   - Chooses the axis with smaller penetration depth
3. **Compute contact-frame relative velocity** including angular contribution:
   ```
   v_rel = (corner_offset_B * omega_B / 0x28C - corner_offset_A * omega_A / 0x28C) - v_A + v_B
   ```
4. **Impulse magnitude** via moment-of-inertia formula:
   ```
   mass_A = cardef_A[0x88]  (short -- vehicle mass parameter)
   mass_B = cardef_B[0x88]

   denominator = (offset_B^2 + K) * mass_A + (offset_A^2 + K) * mass_B
   // where K = DAT_00463204 = 500,000

   impulse = (K >> 8) * 0x1100 / (denominator >> 8) * v_rel
   ```
5. **Guard**: If impulse direction would pull vehicles together (sign mismatch), returns 0
6. **Apply**: Updates both linear and angular velocities for both actors
7. **Friction damping**: Subtracts `(0x100 - elasticity_param) * velocity` before impulse, adds back after -- this simulates energy loss

Post-impulse effects:
- Calls `UpdateVehiclePoseFromPhysicsState` for both actors
- **Wanted mode scoring**: If one actor is the player in cop-chase mode, calls `AwardWantedDamageScore`
- **Traffic recovery**: If impact > 50,000 on a traffic vehicle (slot >= 6), advances its recovery state
- **Sound**: Impact thresholds at 12,800 and 51,200 trigger different collision SFX
- **Visual damage** (impact > 90,000, and `DAT_00463188 == 0` i.e. collisions enabled):
  - For racer vehicles (slot < 6): random angular perturbation to euler angles (cosmetic wobble)
  - For traffic vehicles (slot >= 6): full rotation matrix perturbation + sets recovery flag

### Fallback: Sphere Separation

`ResolveSimpleActorSeparation` (0x408F70):

Used when either actor is in a non-normal state (scripted motion, recovery, damage lockout).

1. Computes distance vector between centers
2. Computes combined radius = `3 * (cardef_A[0x80] + cardef_B[0x80])` (bounding radii times 3)
3. If `distance < combined_radius / 4`:
   - Projects relative velocity onto separation axis
   - If closing (dot product < 0): applies equal-and-opposite velocity impulse along separation axis

### Force Feedback

`PlayVehicleCollisionForceFeedback` (0x428A60):

Maps contact bitmask to DXInput force-feedback effect:
- Bits 0-3 (B hit A's box): Effect type 1 on A, type 2 on B
- Bits 4-7 (A hit B's box): Effect type 1 on B, type 2 on A
- Multiple bits set (multi-contact): Effect type 2 on both
- Magnitude = min(impact / 10, 10000)

---

## Vehicle-to-World Collision (Track Boundaries)

### Functions

| Address | Name | Role |
|---------|------|------|
| 0x406CC0 | `UpdateActorTrackSegmentContacts` | Lateral boundary edges (left/right walls) |
| 0x406F50 | `UpdateActorTrackSegmentContactsForward` | Forward boundary edge (end of span) |
| 0x4070E0 | `UpdateActorTrackSegmentContactsReverse` | Reverse boundary edge (start of span) |
| 0x406980 | `ApplyTrackSurfaceForceToActor` | Impulse + friction response for boundary hits |
| 0x407390 | `ProcessActorSegmentTransition` | Traffic boundary collision at span endpoints |
| 0x4096B0 | `ComputeActorWorldBoundingVolume` | Ground/terrain collision for scripted vehicles |
| 0x445450 | `ComputeActorTrackContactNormal` | Barycentric terrain normal at contact point |
| 0x4457E0 | `ComputeActorTrackContactNormalExtended` | Extended terrain normal with interpolation |

### Detection Algorithm

Track boundaries are defined by STRIP.DAT span geometry. Each span record (24 bytes) contains:
- Vertex indices for left and right edges (shorts at offset +4 and +6)
- Lane count / subdivision (byte at offset +3, lower nibble)
- Span type (byte at offset +0)
- Origin position (3 ints at offset +0x0C)

The vertex positions live in the vertex table at `DAT_004c3d98` (3 shorts per vertex: x, y, z).

**Signed-distance test**: For each of the vehicle's up-to-8 contact probe points (indexed via the table at 0x467384 = `[0,1,2,3,0xFF]` for racers):

```c
// Compute edge normal (perpendicular to edge in XZ plane)
edge_normal.x = vertex_end.z - vertex_start.z;
edge_normal.z = vertex_start.x - vertex_end.x;
normalize(edge_normal);

// Signed distance from probe point to edge
dist = (probe.x - vertex_start.x - origin.x) * normal.x
     + (probe.z - vertex_start.z - origin.z) * normal.z;

if (dist < 0) {
    // Penetrating -- apply wall force
    ApplyTrackSurfaceForceToActor(actor, probe, edge_angle, dist >> 12, side);
    UpdateVehiclePoseFromPhysicsState(actor);  // callback
}
```

The three contact functions differ only in which edges they test:
- **Lateral** (`UpdateActorTrackSegmentContacts`): Tests inner subdivision edges based on the vehicle's lane position. Two cases: left side (lane_index < 1) tests the leftmost edge; right side (lane_index >= subdivisions-1) tests the rightmost edge.
- **Forward** (`UpdateActorTrackSegmentContactsForward`): Tests the forward boundary of the span (last subdivision edge) when the vehicle is near the forward limit.
- **Reverse** (`UpdateActorTrackSegmentContactsReverse`): Tests the backward boundary when near the start.

The span type byte (offset +0) and `DAT_004631a0`/`DAT_004631a4` lookup tables control how vertex indices are computed for different span geometries (12 span types including curves, forks, merges).

### Track Surface Force Response

`ApplyTrackSurfaceForceToActor` (0x406980):

1. Computes penetration vector from signed distance
2. Pushes actor position out by `(penetration - 4)` units along edge normal
3. Rotates velocity into edge-tangent / edge-normal frame
4. **Normal response**: Impulse formula using `DAT_00463200` (1,500,000) as inertia constant:
   ```
   impulse = -(K >> 8) * 0x1100 / ((v_normal^2 + K) >> 8) * v_tangent_at_contact
   ```
5. **Tangential friction**: Damps tangential velocity toward zero with a clamped reduction
6. Applies angular impulse from contact offset
7. **Sound**: Speed thresholds at 0x3200 (~12,800) and 0x19001 (~102,401) trigger scrape/crash SFX
8. **Force feedback**: Dispatches directional FF based on heading-relative contact side (flips left/right if going backward)
9. **Gameplay**: Calls `DecayUltimateVariantTimer` (penalty in Ultimate mode)

### Ground Collision (Scripted/Traffic Vehicles)

`ComputeActorWorldBoundingVolume` (0x4096B0):

Called for vehicles in scripted motion (traffic, recovery). Uses the rendered wheel contact points:

1. Averages visible wheel ground-contact heights from `DAT_00483958` sample table
2. Computes signed penetration depth against the track surface normal (dot product with vehicle up-vector)
3. If penetrating (depth < 0):
   - For small offsets (< 128 units): Simple vertical push-out
   - For larger offsets: Rotation correction via cross-product axis + angle proportional to penetration
   - Applies velocity correction along surface normal
4. Calls `CheckAndUpdateActorCollisionAlignment` to detect and reset flipped vehicles

---

## Collisions Toggle (Options Menu)

The boolean at `DAT_00463188` (file offset 0x63188 in .data) controls collision behavior:

| Value | Meaning | Effect |
|-------|---------|--------|
| 0 | Collisions ON (default) | Full V2V damage + visual deformation; attitude clamp uses recovery-matrix mode |
| 1 | Collisions OFF | V2V still runs (separation force still applies) but: damage deformation skipped (impact > 90,000 check gated); attitude clamp uses hard-clamp mode instead of recovery basis |

**Xrefs**:
- `ApplyVehicleCollisionImpulse` (0x408295): Gates the visual damage / deformation block
- `ClampVehicleAttitudeLimits` (0x405B40): Switches between recovery-matrix vs hard-clamp attitude correction
- `ScreenMainMenuAnd1PRaceFlow` (0x4155BD): Writes the value from menu state
- `ScreenOptionsHub` (0x41DC8E): Writes the value from options screen

The options menu string is `SNK_3dCollisionsButTxt` at 0x461798. The corresponding game options variable is at `0x466018` (the "Collisions" row in the display options screen).

---

## Actor Struct Fields Used by Collision

All offsets are byte offsets within the 0x388-byte actor struct at `gRuntimeSlotActorTable.slot[i]`:

| Offset | Type | Description |
|--------|------|-------------|
| 0x080 | short | Current track span index |
| 0x082 | short | Track segment index (for route tables) |
| 0x084 | short | Track segment index copy (boundary resolution) |
| 0x08C | char | Lane index within current span |
| 0x090-0x0BF | 12 int | 4 wheel/contact probe world positions (3 ints each) |
| 0x0C0-0x0EF | 12 float | Current 3x3 rotation matrix |
| 0x0DC | int* | Pointer to car definition struct (cardef) |
| 0x0E0-0x0E5 | 3 int | Angular velocity (roll, yaw, pitch) in 12-bit fixed |
| 0x0E6-0x0EB | 3 int | Linear velocity (x, y, z) in 12-bit fixed |
| 0x0F8-0x0FD | 3 int | Euler angle accumulator (12-bit) |
| 0x0FE-0x103 | 3 int | World position (x, y, z) in 8-bit fixed |
| 0x104-0x106 | 3 short | Current euler angles (12-bit) |
| 0x120-0x14F | 12 float | Recovery basis rotation matrix |
| 0x148-0x14A | 3 short | Heading normal vector (12-bit) |
| 0x150-0x17F | 12 float | Saved orientation for recovery |
| 0x16C | int | Previous Y position (for suspension delta) |
| 0x16E-0x175 | 4 int | Wheel ground height offsets |
| 0x180-0x1AF | 12 float | Recovery orientation target |
| 0x1B8 | int* | Pointer to car definition (cardef) |
| 0x1BE | byte | Wheel-contact bitmask (4 bits for 4 wheels) |
| 0x1C0-0x1C8 | 3 int | Angular velocity (physics frame, alternate) |
| 0x1CC-0x1D4 | 2 int | Velocity components (physics, lateral/longitudinal) |
| 0x1FC-0x204 | 3 int | Position (physics frame, alternate) |
| 0x208-0x20C | 2 ushort | Pitch/roll angles (for attitude limiting) |
| 0x338 | short | Frame counter |
| 0x375 | byte | **Slot index** (0-5 = racer, 6+ = traffic) |
| 0x376+3 | byte | Actor state (0=normal, 1=scripted, etc.) |
| 0x376+5 | byte | Track contact flag (0=none, 1=left/right, 2=inner edge) |
| 0x376+6 | byte | Damage lockout counter (0x0F = max) |
| 0x379 | byte | Recovery mode flag |
| 0x19C (short) | short | Flipped-frames counter |

### Car Definition Fields (pointed to by actor+0x1B8)

| Offset | Type | Description |
|--------|------|-------------|
| 0x04 | short | Half-width (bounding box) |
| 0x08 | short | Half-length (bounding box, front) |
| 0x0C | short | Rear body length |
| 0x14 | short | Rear offset (negative = behind center) |
| 0x80 | short | Bounding sphere radius |
| 0x82 | short | Track span half-width |
| 0x88 | short | **Mass parameter** (used in impulse denominator) |

---

## Broadphase Data Structures

| Address | Size | Description |
|---------|------|-------------|
| 0x483050 | 20 bytes x N | Per-actor AABB: [min_x, min_z, max_x, max_z, chain_link] |
| 0x483554 | ~256 bytes | Spatial hash buckets (indexed by span_heading >> 2) |
| 0x483958 | 8 x 4 bytes | Ground height sample table (8 probes) |

---

## Key Constants

| Address | Value | Usage |
|---------|-------|-------|
| 0x463200 | 1,500,000 | V2W inertia constant (denominator scaling) |
| 0x463204 | 500,000 | V2V inertia constant (denominator scaling) |
| 0x463208 | ~3.338 (float) | Ground collision rotation scaling |
| 0x463188 | 0 or 1 | Collisions toggle (0=on, 1=off) |
| 0x467384 | [0,1,2,3,FF,...] | Racer contact probe index table (4 probes + sentinel) |
| 0x46738C | [0,1,2,3,4,5,6,7] | Scripted vehicle contact probe table (8 probes) |
| 0x46318C | [1,1,1,0,1,0,...] | Per-span-type suspension response enable flags |
| 0x4631A0 | lookup table | Vertex index offsets by span type (inner edge) |
| 0x4631A4 | lookup table | Vertex index offsets by span type (outer edge) |

---

## Collision Response Summary

### V2V (Vehicle-to-Vehicle)
- **Model**: 2D rigid body collision in the contact-normal plane
- **Coefficients**: Mass from cardef[0x88], inertia constant 500,000
- **Energy loss**: Elasticity parameter from binary-search depth (0-0x100 range, ~0x70 typical)
- **Angular coupling**: Full angular impulse from contact point offset
- **Damage**: Visual angle perturbation above 90,000 impact magnitude; traffic recovery state advancement above 50,000
- **Sound**: Three tiers (silent / medium scrape at 12,800 / heavy crash at 51,200)
- **Scoring**: Wanted mode awards damage points per hit above 10,000

### V2W (Vehicle-to-World)
- **Model**: Impulse + friction against planar wall segments
- **Push-out**: Direct position correction along wall normal
- **Friction**: Tangential velocity damping with clamped reduction
- **Sound**: Two tiers (scrape at ~12,800 / crash at ~102,400)
- **Feedback**: Directional force-feedback (left/right based on heading vs. wall angle)

### Recovery System
- Flipped vehicles: `CheckAndUpdateActorCollisionAlignment` counts frames with excessive pitch/roll misalignment. After threshold (0x19C > 2), if angles converge to track normal, calls `ResetVehicleActorState` to teleport back to track.
- Traffic vehicles hit hard: Recovery state increments (1->7 scale), eventually triggers recycle via `RecycleTrafficActorFromQueue`.
- Collisions-OFF mode: Hard-clamps attitude angles instead of using smooth recovery matrix interpolation.

---

## Call Graph

```
RunRaceFrame (0x42B580)
  |
  +-- UpdateRaceActors (0x436A70)
  |     |
  |     +-- per racer: UpdateVehicleActor (0x406650)
  |     |     +-- ClampVehicleAttitudeLimits (0x405B40)    [reads collisions toggle]
  |     |     +-- UpdatePlayerVehicleDynamics / UpdateAIVehicleDynamics
  |     |     +-- IntegrateVehiclePoseAndContacts (0x405E80)
  |     |     +-- UpdateActorTrackSegmentContactsReverse (0x4070E0)
  |     |     |     +-- ApplyTrackSurfaceForceToActor (0x406980)
  |     |     |     +-- UpdateVehiclePoseFromPhysicsState (callback)
  |     |     +-- UpdateActorTrackSegmentContactsForward (0x406F50)
  |     |     |     +-- ApplyTrackSurfaceForceToActor
  |     |     |     +-- UpdateVehiclePoseFromPhysicsState (callback)
  |     |     +-- UpdateActorTrackSegmentContacts (0x406CC0)
  |     |           +-- ApplyTrackSurfaceForceToActor
  |     |           +-- UpdateVehiclePoseFromPhysicsState (callback)
  |     |
  |     +-- per traffic: UpdateTrafficActorMotion (0x443ED0)
  |           +-- IntegrateScriptedVehicleMotion (0x409D20)
  |                 +-- ComputeActorWorldBoundingVolume (0x4096B0)
  |                       +-- CheckAndUpdateActorCollisionAlignment (0x409520)
  |
  +-- ResolveVehicleContacts (0x409150)            [V2V global pass]
        +-- per pair: ResolveVehicleCollisionPair (0x408A60)
        |     +-- CollectVehicleCollisionContacts (0x408570)  [8-corner OBB test]
        |     +-- ApplyVehicleCollisionImpulse (0x4079C0)     [per contact point]
        |     |     +-- UpdateVehiclePoseFromPhysicsState (both actors)
        |     |     +-- AwardWantedDamageScore (cop chase)
        |     |     +-- DecayUltimateVariantTimer
        |     |     +-- PlayVehicleSoundAtPosition
        |     |     +-- GetDamageRulesStub (DISABLED -- always returns 0)
        |     |     +-- BuildRotationMatrixFromAngles (damage deformation)
        |     +-- PlayVehicleCollisionForceFeedback (0x428A60)
        |           +-- DXInput::PlayEffect
        |
        +-- per pair (fallback): ResolveSimpleActorSeparation (0x408F70)
```
