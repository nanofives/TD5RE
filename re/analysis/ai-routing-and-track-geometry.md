# AI Actor Routing & Track Geometry System — Deep Analysis

> **Binary**: TD5_d3d.exe | **44 functions** decompiled from Ghidra (port 8195)
> **Date**: 2026-03-20

---

## Table of Contents
1. [STRIP.DAT Track Data Format](#1-stripdat-track-data-format)
2. [Track Vertex & Segment Geometry](#2-track-vertex--segment-geometry)
3. [Barycentric Contact Resolution](#3-barycentric-contact-resolution)
4. [Segment Transition & Boundary Logic](#4-segment-transition--boundary-logic)
5. [AI Routing Pipeline](#5-ai-routing-pipeline)
6. [Track Collision System](#6-track-collision-system)
7. [Track Rendering](#7-track-rendering)
8. [Track Lighting](#8-track-lighting)
9. [Global Data Tables](#9-global-data-tables)

---

## 1. STRIP.DAT Track Data Format

### BindTrackStripRuntimePointers (0x444070)

Parses the loaded STRIP.DAT blob into runtime pointers. The file is a single allocation at `DAT_004aed90` with self-relative offsets:

```
DAT_004c3da0 = raw_blob_ptr                     // header pointer
DAT_004c3d9c = blob + blob[0]                   // span record table (24 bytes/record)
DAT_004c3d98 = blob + blob[2]                   // vertex coordinate table (6 bytes/vertex = 3x short XYZ)
DAT_004c3d90 = blob[1]                          // total span count (ring length)
DAT_004c3d94 = blob[4]                          // auxiliary count
_DAT_004c3d8c = blob[3]                         // secondary count
```

After extraction, the function patches sentinel records:
- First span: type byte set to `9`, link field at offset +10 set to `span_count - 1`
- Last span: type byte set to `10`, link field at +0x10 cleared to `0`

### Span Record Layout (24 bytes = 0x18)

| Offset | Size | Field |
|--------|------|-------|
| +0x00  | 1    | **Span type** (0-11): controls triangle topology and neighbor lookup |
| +0x01  | 1    | Strip attribute byte (set by `ApplyTrackStripAttributeOverrides`) |
| +0x03  | 1    | **Packed sub-span count**: low nibble = sub-span count, high nibble = height offset |
| +0x04  | 2    | **Left vertex index** into vertex table |
| +0x06  | 2    | **Right vertex index** into vertex table |
| +0x08  | 2    | Forward link span index (used by types 8-11) |
| +0x0A  | 2    | Backward link span index (used by types 9-11) |
| +0x0C  | 4    | **X world origin** (int, absolute position) |
| +0x10  | 4    | **Y world origin** (int, vertical) |
| +0x14  | 4    | **Z world origin** (int, absolute position) |

### Vertex Table

Each vertex is 6 bytes (3 x `short`): relative X, Y, Z offsets from the span's world origin. Vertices are indexed from span records via the left/right index fields. Sub-spans use consecutive vertex indices starting from the base.

### Span Types (0-11)

The type byte controls triangle subdivision topology. A dispatch table at `DAT_00474e40` / `DAT_00474e41` provides per-type left/right vertex offsets, and `DAT_00474e28` / `DAT_00474e29` provides per-type edge mask bits for boundary tests.

| Type | Topology | Notes |
|------|----------|-------|
| 1,2,5 | Standard quad: left+right vertices, diagonal split | Most common road surface |
| 3,4   | Alternate diagonal: uses shifted vertex pairs | Transition geometry |
| 6,7   | Reversed winding: swapped left/right references | Opposite-facing surfaces |
| 8     | Forward junction: links to next span via +0x08, sub-span conditional | T-junctions |
| 9     | Backward sentinel: patched at load, links via +0x0A | Circuit wrap-around start |
| 10    | Forward sentinel: patched at load, cleared link | Circuit wrap-around end |
| 11    | Backward junction: conditional link via +0x0A | Reverse T-junctions |

---

## 2. Track Vertex & Segment Geometry

### InitActorTrackSegmentPlacement (0x445f10)

Places an actor at the centroid of its current track quad. Computes position by averaging 4 corner vertices (left pair + right pair for the sub-span), then adds the span's world origin scaled by 0x100 (8-bit fixed point):

```
pos.x = avg(v_left[0].x, v_left[1].x, v_right[0].x, v_right[1].x) * 256 + origin.x * 256
pos.y = avg(...y...) * 256 + origin.y * 256
pos.z = avg(...z...) * 256 + origin.z * 256
```

Clamps the sub-span index to `(nibble_count - 1)` if out of range.

### ComputeActorTrackHeading (0x435ce0) / ComputeActorHeadingFromTrackSegment (0x445b90)

Both derive a heading angle from track geometry. The heading is computed as a full 12-bit angle (0-4095 = 0-360 degrees) from the difference vectors of span corner vertices, dispatched by span type:

- **Types 1,2,5**: `dx = (left[+3] - right[+3]) - right[0] + left[0]`, `dz = left[+5] - right[+5] - right[+2] + left[+2]`, divide by 4
- **Types 3,4**: Uses shifted right vertex pairs (right[+6], right[+8])
- **Types 6,7**: Reversed: `dx = (left[+6] - right[+3]) + left[+3] - right[0]`

The result is converted to a 12-bit angle via `AngleFromVector12Full`.

### InterpolateTrackSegmentNormal (0x445e30)

Computes a surface normal for a triangle defined by 3 vertex indices:
1. Forms two edge vectors from vertex A to B and A to C
2. Takes cross product
3. Right-shifts components by 12 bits
4. Normalizes via `ConvertFloatVec3ToShortAnglesB` -> output is short[3] (nx, ny, nz)
5. If ny == 0, forces ny = 1 (prevents divide-by-zero in height projection)

### TransformTrackVertexByMatrix A/B/C (0x446030 / 0x446140 / 0x4461c0)

Three variants that transform a track vertex through a 4x4-style matrix and extract angle components via `AngleFromVector12`. Used for projecting track geometry into view space:

- **A** (0x446030): Computes both X and Z angle components -> `param_1[0]` and `param_1[2]`
- **B** (0x446140): Computes only X angle -> `param_1[0]`
- **C** (0x4461c0): Computes only Z angle -> `param_1[2]` (stored at param_1+4)

---

## 3. Barycentric Contact Resolution

### ComputeTrackTriangleBarycentrics (0x4456d0)

Given 3 vertex indices (triangle) and a world position, computes the Y (height) at that XZ position using barycentric interpolation:

1. Forms edge vectors from vertex `param_1` to `param_2` and `param_1` to `param_3`
2. Cross product -> normal (right-shifted by 4 bits)
3. `normal.y` used as denominator (forced to 1 if zero)
4. Height = `(vertex.x - pos.x) * normal.x + (vertex.z - pos.z) * normal.z) / normal.y + vertex.y`
5. Returns `height * 0x100` (8-bit fixed point)

### ComputeTrackTriangleBarycentricsWithNormal (0x445a70)

Extended version that also outputs the surface normal to `param_5` (short[3]). Cross product is right-shifted by 12 bits (higher precision normalization). Same height formula but uses the normalized normal components directly.

### ComputeActorTrackContactNormal (0x445450)

Master contact-normal resolver. Given an actor's segment state (span + sub-span index) and world position:

1. Computes local position relative to span origin
2. Dispatches on span type to select which triangle the point falls in
3. For types 1,2,5,8-11: performs a cross-product edge test between left and right diagonal vertices
4. Based on the test result, increments vertex indices to select the correct triangle half
5. Calls `ComputeTrackTriangleBarycentrics` for the selected triangle
6. Writes the resulting height into `param_3`

### ComputeActorTrackContactNormalExtended (0x4457e0)

Same logic as above but also outputs the surface normal via `ComputeTrackTriangleBarycentricsWithNormal` and stores it in `param_3` (short[3]). Additionally writes corner vertex indices to `param_1[4]` and `param_1[5]`.

---

## 4. Segment Transition & Boundary Logic

### UpdateActorTrackPosition (0x4440f0)

The core track-position update function. Uses **cross-product boundary tests** to determine if an actor has crossed any of the 4 edges of its current track quad:

```
For each of the 4 edges (controlled by bitmask bVar10):
  cross = (edge_end.x - edge_start.x) * (pos.z - edge_start.z)
        - (edge_end.z - edge_start.z) * (pos.x - edge_start.x)
  if cross > 0: actor is outside this edge -> set bit in result byte
```

The 4-bit result (`bVar9`) encodes which boundaries were crossed:
- **Bit 0** (0x01): Crossed left-to-right edge (forward boundary)
- **Bit 1** (0x02): Crossed right diagonal edge
- **Bit 2** (0x04): Crossed back-right edge
- **Bit 3** (0x08): Crossed back-left edge

Edge masks (`bVar10`) at sub-span boundaries are loaded from `DAT_00474e28`/`DAT_00474e29` tables, masking out edges that don't exist at the first or last sub-span.

On boundary crossing (cases 1, 2, 4, 8 in the switch):
- **Case 1** (forward): Advances to next span. For types 8/10/11, follows the link pointer at +0x08/+0x0A. Updates the cumulative span counter (`param_1[2]`, `param_1[3]` tracks high-water mark).
- **Case 2** (right): Advances span index. Type 8/10 follow forward link; otherwise increments by 1. Adjusts sub-span index using the high-nibble height offset.
- **Case 4** (backward): Decrements span. Similar link-following for types 9/11.
- **Case 8** (left): Decrements span, adjusts sub-span.

After transition, recursively re-evaluates boundaries in the new span.

### NormalizeActorTrackWrapState (0x443fb0)

Normalizes the accumulated span counter at `actor+0x84` into a wrapped position modulo `DAT_004c3d90` (total span count):

```c
if (raw_span >= 0)
    wrapped = raw_span % ring_length
    laps = raw_span / ring_length
else
    wrapped = (raw_span % ring_length) + ring_length  // handle negative wrap
    laps = raw_span / ring_length
```

Stores the normalized span in `actor+0x82`, returns lap count. This is the circuit lap counter mechanism.

### ResolveActorSegmentBoundary (0x443ff0)

Handles segment boundary discontinuities from a jump table stored at `DAT_004c3da0+0x14` (count) and `DAT_004c3da0+0x18` (6-byte entries). Each entry defines a span range and a remapping offset:

```
for each jump entry:
    if entry.start <= span <= entry.end:
        span += (entry.target - entry.source)  // remap
```

Stores the remapped span in both `actor+0x82` and `actor+0x84`. Used for tracks with non-contiguous span numbering (branching paths, shortcuts).

---

## 5. AI Routing Pipeline

### Architecture Overview

The AI routing system uses two parallel **route tables** stored per-actor. Each route table is a byte array indexed by span number, providing a lateral offset byte (0x00-0xFF = left-to-right across the track width). The two tables represent LEFT and RIGHT routes (loaded from `LEFT.TRK` / `RIGHT.TRK` in level ZIPs).

Key globals (stride 0x11C = 284 bytes per actor, base at `gActorRouteStateTable` = 0x4AFB58 area):

| Offset in per-actor block | Description |
|---------------------------|-------------|
| +0x00 | Current active route table pointer |
| +0x03 | Route group ID (must match for peer detection) |
| +0x09 | Planned track offset |
| +0x0E-0x13 | Cached route limits per table |
| +0x14-0x15 | Track offset min/max for current route |
| +0x18 | Forward span progress cache |
| +0x35 | Back-reference to actor slot index |

### AngleFromVector12Full (0x433fc0)

Full 4-quadrant angle computation from a 2D vector. Extends `AngleFromVector12` (which only handles quadrant 1) to all quadrants:

| Quadrant | X >= 0, Z >= 0 | Return |
|----------|----------------|--------|
| Q1 | x>=0, z>=0 | `AngleFromVector12(x, z)` |
| Q2 | x<0, z>=0  | `AngleFromVector12(z, -x) + 0x400` |
| Q3 | x<0, z<0   | `AngleFromVector12(-x, -z) + 0x800` |
| Q4 | x>=0, z<0  | `AngleFromVector12(-z, x) + 0xC00` |

Result: 12-bit angle (0x000-0xFFF = 0-360 degrees).

### ComputeActorRouteHeadingDelta (0x434040)

Computes the angular difference between the actor's current heading and the route-prescribed heading at the current span:

```c
route_heading = route_table[current_span * 3 + 1] * 0x102C  // scale byte to 12-bit angle
delta = ((actor_heading >> 8) - (route_heading >> 8) - 0x800) & 0xFFF) - 0x800
return (-delta) & 0xFFF
```

The `* 0x102C` scaling converts a byte (0-255) to a full-circle 12-bit heading. The result is the signed angular deviation from the ideal route heading, wrapped to [0, 0xFFF].

### ComputeTrackSpanProgress (0x4345b0)

Projects an actor's world position onto the current span's forward axis and returns a normalized progress value (0x00-0xFF = start-to-end of span):

1. Gets the span's forward edge vertices from the vertex table
2. Computes a dot product: `dot = (pos - span_origin - vertex_start) . (vertex_end - vertex_start)`
3. Divides by the squared edge length
4. Returns as 8-bit fixed point (0x100 = fully traversed)

### ComputeSignedTrackOffset (0x434670)

Computes the signed lateral distance between the actor and a route sample point. Interpolates the route byte across the span's progress:

- If `progress < route_byte`: offset is negative (left of route)
- If `progress >= route_byte`: offset is positive (right of route)

Result is in world-space units, used for steering correction.

### SampleTrackTargetPoint (0x434800)

Generates a world-space target point along the track for an actor to steer toward:

1. Gets the span's left-edge start and end vertices
2. Linearly interpolates between them using `param_2` (progress fraction, 0x00-0xFF)
3. Computes a perpendicular direction via `ConvertFloatVec4ToShortAngles` on the edge tangent
4. Offsets the interpolated point laterally by `param_4` (the route offset bias)

This is the primary target-point generator for AI steering.

### ComputeRouteForwardOvershootScalar (0x434740)

Computes a forward-overshoot correction factor. Checks if the actor has overshot the current target point by taking a dot product of the actor-to-target vector against the span's forward direction. Returns a correction scalar if the dot product is positive (actor is behind the target), 0 otherwise. The current caller (`UpdateActorTrackBehavior`) appears to discard the return value.

### UpdateActorSteeringBias (0x4340c0)

Core steering actuator for AI actors. Adjusts `steering_command` based on the heading delta:

**Two heading regions** (0x800 = 180 degrees):
- **Delta > 0x800** (facing away): Hard correction of +/- 0x4000
- **Delta 0x401-0x800** (large deviation): Direct additive correction
- **Delta 0x100-0x400** (moderate): Proportional correction with `param_2` scaled by sin
- **Delta < 0x100** (small): Gradual ramp with speed-based clamping

**Speed-based clamping**:
```c
speed_sq = (forward_speed >> 8)^2
ramp_rate = 0xC0000 / ((abs_speed * 0x400) / (speed_sq + 0x400) + 0x40)
clamp = 0x1800000 / ((abs_speed * 0x10000) / (speed_sq + 0x10000) + 0x100)
```
Higher speed = tighter clamp on steering rate. A progressive counter at `gap_0338+2` ramps up by 0x40 per tick (max 0x100) to smooth steering onset.

Final steering is hard-clamped to [-0x18000, +0x18000].

### UpdateActorTrackOffsetBias (0x434900)

Manages the lateral offset bias for peer avoidance:

1. Calls `FindActorTrackOffsetPeer` to find nearest same-lane actor ahead
2. **If no peer found**: Decays offset bias toward 0 at rate 8/tick
3. **If peer found**: Computes span distance (clamped to 1-40), adds `(41 - distance)` to offset bias — closer peers cause stronger avoidance
4. A mode flag `DAT_004b08b0` (0 or 1) toggles between two avoidance strategies based on whether the peer is in the same sub-lane

### FindActorTrackOffsetPeer (0x4337e0)

Complex peer-selection function. Scans all active actors (up to 6 racers, then traffic slots 6+) looking for the nearest forward actor in the same route group whose lateral extent overlaps the current actor's planned offset:

1. **Route matching**: Only considers actors in the same route group (`piVar3[3] == piVar10[3]`)
2. **Forward check**: Peer span must be >= actor span
3. **Route switching**: If the peer uses a different route table, switches the current actor's active route to match (ensures consistent offset comparison)
4. **Lateral overlap test**: Uses `ClassifyTrackOffsetClamp` and `ComputeSignedTrackOffset` to check if the peer's min/max offset band overlaps the actor's planned offset +/- 0x20
5. **Distance scoring**: Selects the peer with minimum span distance (< 0x28 spans)
6. Returns the peer actor index, or the actor itself if no qualifying peer found

### ClassifyTrackOffsetClamp (0x4368a0)

Determines if a planned lateral offset would fall outside the track boundaries at a lookahead span (+4 spans ahead):

1. Samples a target point at the lookahead span using the planned offset
2. Tests the target point via `ComputeTrackSpanProgress`
3. Returns:
   - **0**: In range (offset is safe)
   - **1**: Clamped against the far boundary
   - **2**: Clamped against the near boundary

Handles segment boundary remapping using the jump table at `DAT_004c3da0`.

### RefreshActorTrackProgressOffset (0x4342e0)

Simple refresh that recomputes and caches an actor's span progress and signed track offset after a pose or route change:

```c
progress = ComputeTrackSpanProgress(actor_span, actor_position)
gActorTrackSpanProgress[actor * 0x47] = progress
offset = ComputeSignedTrackOffset(actor_span, progress, route_byte)
DAT_004afb84[actor * 0x47] = offset
```

### InitializeActorTrackPose (0x434350)

Full initialization of an actor on the track:

1. Sets the span index and lane byte in the actor record
2. Calls `InitActorTrackSegmentPlacement` to position at quad centroid
3. Computes heading from span geometry (type-dispatched, same as `ComputeActorTrackHeading`)
4. Converts to 12-bit angle via full-quadrant `AngleFromVector12`
5. Scales by 0x100 and adds 0x80000 (180 degrees) if `param_4` is set (reverse direction)
6. Calls `ResetVehicleActorState` to initialize physics
7. Refreshes span progress and signed offset

### UpdateActorTrackBehavior (0x434fe0)

The main per-frame AI routing update. Orchestrates the full pipeline:

1. **U-turn detection**: If heading delta is > 0x800 and < 0xCE0 (roughly 180-290 degrees), activates a recovery script (`DAT_00473cc8`)
2. **Script execution**: If an active script exists, runs `AdvanceActorTrackScript`; on completion, refreshes progress/offset and returns
3. **Route table selection**: Handles LEFT vs RIGHT route based on current active table; resolves segment boundary jumps
4. **Offset bias**: Calls `UpdateActorTrackOffsetBias` for peer avoidance
5. **Target sampling**: Calls `SampleTrackTargetPoint` at +4 spans ahead with the current route byte and offset bias
6. **Forward overshoot**: Calls `ComputeRouteForwardOvershootScalar` (result currently unused)
7. **Heading computation**: Converts target vector to 12-bit angle via full-quadrant dispatch
8. **Steering delta**: Computes angular difference between current heading+steering and target angle
9. **Threshold state**: Calls `UpdateActorRouteThresholdState` (the 3-state throttle controller)
10. **Steering bias**: Calls `UpdateActorSteeringBias` with delta 0x10000 or 0x20000 depending on threshold result

### UpdateActorTrackBounds (0x4366e0)

Samples 4 track-contact points (the actor's corner positions at offsets +0x90, +0x9C, +0xA8, +0xB4) and computes:
- Per-corner span progress via `ComputeTrackSpanProgress`
- Min/max span progress across all 4 corners
- Signed offsets against both LEFT and RIGHT route tables
- Stores results in the per-actor route state block for use by peer detection

---

## 6. Track Collision System

### ClearTrackSegmentVisibilityTable (0x406950)

Initializes two tables:
- `DAT_00483554`: 256-entry dword table, all set to 0xFFFFFFFF (invalid span markers)
- `DAT_00483060`: 20-byte stride entries (up to span 0x483178), each entry: byte[0]=0xFF, byte[1]=0

These are per-frame visibility/contact tables cleared before collision processing.

### UpdateActorTrackSegmentContacts (0x406cc0)

Generic segment-contact handler. Iterates through a list of contact point indices (up to 8, terminated by negative value) and tests each against the track boundary edges:

For each contact point at `actor.gap_0000[index * 0xC + 0x90]`:
1. **Left boundary test**: If sub-span index < 1, tests against left edge (vertices from left/right base indices)
2. **Right boundary test**: If sub-span index >= count-1, tests against right edge (uses `DAT_004631a0`/`DAT_004631a4` offset tables)
3. **Cross-product penetration**: `pen = (pos - edge_start) . edge_normal`; if negative, the point has penetrated
4. On penetration:
   - Calls `DecayUltimateVariantTimer` (damage/timer effect)
   - Computes wall heading via `AngleFromVector12`
   - Calls `ApplyTrackSurfaceForceToActor` with penetration depth (shifted >>12)
   - Invokes callback `param_2` (function pointer for custom response)
   - Sets contact flag at `gap_0376[5]`

### UpdateActorTrackSegmentContactsForward (0x406f50)

Specialized forward-boundary contact test. Only activates when the actor's span is near `DAT_00483954` (the forward boundary span index). Tests up to 8 contact points against the forward edge of the boundary span. Same penetration/response pattern as the generic handler.

### UpdateActorTrackSegmentContactsReverse (0x4070e0)

Mirror of the forward test for `DAT_00483550` (the reverse boundary span). Tests contact points against the reverse edge.

### ApplySimpleTrackSurfaceForce (0x407270)

Simplified wall-bounce for traffic/AI actors (vs the full `ApplyTrackSurfaceForceToActor` used for player vehicles):

1. Rotates penetration depth into wall-normal coordinates using sin/cos of wall heading
2. Applies positional impulse: `pos += (depth/16) * normal`
3. Projects velocity into wall-tangent/wall-normal: `v_n = vel . normal`, `v_t = vel . tangent`
4. If `v_t >= 0` (moving away from wall), applies friction:
   - `v_t = -(v_t / 2)` (halve and reverse tangential velocity)
   - Clamp normal velocity to +/- 0x180 dead zone
5. Rotates back to world space and writes velocity
6. Clears a state flag at `actor+0x37B`

### ProcessActorSegmentTransition (0x407390)

Handles collision response at span boundaries for traffic/AI actors. Two passes:

1. **First sub-span (iVar13 < 2)**: Tests left boundary edge with heading-angle-scaled width offset from the actor's bounding box dimensions. The width offset uses `sin(heading_delta) * bbox_width + cos(heading_delta) * bbox_depth` to account for the actor's orientation.

2. **Last sub-span (iVar13 >= count-2)**: Same test against right boundary edge, using the per-type vertex offset tables.

On penetration, calls `ApplySimpleTrackSurfaceForce` and `UpdateTrafficVehiclePose`.

### ProcessActorForwardCheckpointPass (0x4076c0)

Tests if an actor is crossing the forward checkpoint boundary (span == `DAT_00483954 + 1`). Same heading-scaled edge test as `ProcessActorSegmentTransition`. Response: `ApplySimpleTrackSurfaceForce` + `UpdateTrafficVehiclePose`.

### ProcessActorRouteAdvance (0x407840)

Tests if an actor is crossing the reverse route boundary (span == `DAT_00483550 - 1`). Same pattern. These two functions prevent traffic from escaping through checkpoint/route boundaries.

---

## 7. Track Rendering

### RenderTrackSegmentBatch (0x4323d0)

Clips and submits a polygon batch against the near clip plane (`DAT_004afb20` on X axis). Uses Sutherland-Hodgman-style clipping:

1. Iterates vertices in `DAT_004af288` (source, 8 floats/vertex: x, y, z, w, u, v, r, g?)
2. Tests `DAT_004afb20 - vertex.x` sign bit for inside/outside
3. On edge crossing, linearly interpolates all 8 components
4. UV coordinates are integer-rounded during interpolation
5. Output to `DAT_004af28c`, count in `local_c`

### RenderTrackSegmentBatchVariant (0x4326d0)

Identical clipping logic but clips against `DAT_004afb28` on the **Y axis** (vertex[1]) instead of X. Used for the second clip plane (vertical/lateral depending on orientation).

### RenderTrackSegmentNearActor (0x433ce0)

Computes a local-space position for rendering track geometry near an actor. Dispatches by span type to compute offset vectors between the actor's position and the span's corner vertices, halved for LOD purposes. This positions the track mesh relative to the camera for the near-field detail rendering pass.

### InitializeTrackStripMetadata (0x42fad0)

Sets up per-strip metadata from the level data table at `DAT_0046bb1c[param_1]`. Iterates 4 strips (stride 0x20), comparing each strip's name against `"DAT_00473b30"` (3-char comparison). Sets type flag to 3 (match) or 1 (default) at stride-4 offsets.

### ApplyTrackStripAttributeOverrides (0x42fb40)

Applies a compact override table to the strip attribute byte (+0x01) of each span record. The table format is pairs of `(span_index, attribute_value)` at 8-byte stride. Between override entries, the default attribute is used:

```c
for each span 0..count:
    if span == override_table[i].index:
        current_attr = override_table[i].value
        advance i
    span_record[span].attribute = current_attr
```

### RemapCheckpointOrderForTrackDirection (0x42fd70)

Remaps checkpoint order tables based on `DAT_004aee00` (track direction, -1 for reverse). Uses `SwapIndexedRuntimeEntries` to transpose pairs of entries. The checkpoint table at `DAT_004aedb0` has 16 entries with forward/reverse pairs at different offsets depending on the direction flag:
- Direction -1: Swaps entries at offsets [0,+0x10] and [+4,+0xC]
- Other: Swaps entries at offsets [0,+0x14], [+4,+0x10], [+8,+0xC]

---

## 8. Track Lighting

### SetTrackLightDirectionContribution (0x42e130)

Configures one of 3 directional light slots (param_1 = 0,1,2):

- If all RGB weights are 0, disables the slot: `DAT_004aafd0[slot] = 0`
- Otherwise, enables the slot and computes a direction vector:
  ```c
  intensity = (r + g + b) / 3.0
  light_dir[slot].x = direction.x * intensity * 0.0009765625  // 1/1024
  light_dir[slot].y = direction.y * intensity * 0.0009765625
  light_dir[slot].z = direction.z * intensity * 0.0009765625
  ```
  Direction comes from `param_2` (short[3]), scaled by average intensity.

Light direction vectors stored at `DAT_00467338` with 0x0C stride.

### BlendTrackLightEntryFromStart (0x42fe20)

Blends between two lighting entries during a segment transition, interpolating from the start side:

1. Computes wall-normal distance from the actor to the span's left edge
2. Uses this as an interpolation factor `t` (clamped to [0, param_3])
3. Blends ambient color: `ambient = (entry_A * (1-t) + entry_B * t)`
4. Sets directional light 0 with weight `(1-t)`, clears lights 1 and 2
5. Calls `UpdateActiveTrackLightDirections` to apply

### BlendTrackLightEntryFromEnd (0x42ffc0)

Same as above but interpolates from the end side of the transition. Uses the next span's vertices and the second lighting entry block (offset +0x18 in the span record). The interpolation factor is positive for the end direction.

### ApplyTrackLightingForVehicleSegment (0x430150)

Master per-vehicle lighting selector. Walks the lighting entry table at `DAT_004aee14` (0x24 bytes per entry) to find the entry covering the current span:

1. Binary searches backward/forward through the span-indexed lighting entries
2. Detects entry changes (triggers a transition flag at `actor+0x367`)
3. Stores the entry's material flag at `DAT_004c38a0[actor_index]`
4. Dispatches on the entry's mode byte (at entry+0x18) to select the blend function

---

## 9. Global Data Tables

### Key Addresses

| Address | Type | Description |
|---------|------|-------------|
| `DAT_004c3d9c` | byte* | Span record table (24B/span) |
| `DAT_004c3d98` | short* | Vertex coordinate table (6B/vertex) |
| `DAT_004c3d90` | int | Total span count (ring length) |
| `DAT_004c3d94` | int | Auxiliary span count |
| `DAT_004c3d8c` | int | Secondary count |
| `DAT_004c3da0` | int* | STRIP.DAT header (jump table at +0x14) |
| `DAT_004aed90` | int* | Raw STRIP.DAT blob pointer |
| `DAT_004aee14` | void* | Track lighting entry table (0x24B stride) |
| `DAT_004aee10` | int* | Strip metadata block pointer |
| `DAT_00474e28` | byte[] | Per-span-type left edge mask LUT (stride 2) |
| `DAT_00474e29` | byte[] | Per-span-type right edge mask LUT (stride 2) |
| `DAT_00474e40` | byte[] | Per-span-type left vertex offset LUT (stride 2) |
| `DAT_00474e41` | byte[] | Per-span-type right vertex offset LUT (stride 2) |
| `DAT_004631a0` | int[] | Per-span-type far-left vertex offset (stride 8) |
| `DAT_004631a4` | int[] | Per-span-type far-right vertex offset (stride 8) |
| `DAT_00473c6c` | int[] | Per-span-type forward link offset (stride 8) |
| `DAT_00473c68` | int[] | Per-span-type backward link offset (stride 8) |
| `DAT_00483954` | int | Forward boundary span index |
| `DAT_00483550` | int | Reverse boundary span index |
| `DAT_00483554` | int[256] | Segment visibility table |
| `DAT_00483060` | byte[] | Segment contact state table (20B stride) |
| `gActorRouteStateTable` (0x4AFB58 area) | int[] | Per-actor route state (stride 0x47 dwords = 0x11C bytes) |
| `gActorTrackSpanProgress` | int[] | Per-actor cached span progress (stride 0x47) |
| `DAT_004afb84` | int[] | Per-actor signed track offset (stride 0x47) |
| `DAT_004b08b0` | int | Peer avoidance mode flag (0 or 1) |
| `DAT_004b08b4` | int | Alternate route table pointer |
| `DAT_004afb58` | int | Primary route table pointer (LEFT.TRK) |
| `DAT_004af288` | float* | Track render vertex source buffer (8 floats/vert) |
| `DAT_004af28c` | float* | Track render vertex clipped output buffer |
| `DAT_004af280` | int | Track render vertex count |
| `DAT_004afb20` | float | Near clip plane X threshold |
| `DAT_004afb28` | float | Near clip plane Y threshold |
| `DAT_004aafd0` | int[3] | Directional light enable flags |
| `DAT_00467338` | float[] | Directional light vectors (3 slots x 3 floats, stride 0x0C) |
| `gTimeTrialModeEnabled` | int | Disables AI behavior updates when nonzero |

### Per-Actor Route State Block (0x11C bytes, indexed by `actor_index * 0x47`)

| Offset | Size | Field |
|--------|------|-------|
| +0x00  | 4    | Active route table pointer (LEFT or RIGHT) |
| +0x0C  | 4    | Route group ID |
| +0x24  | 4    | Planned track offset |
| +0x38-0x4C | 4x4 | Cached route limits (min/max per table) |
| +0x50-0x54 | 2x4 | Active offset limits |
| +0x60  | 4    | Forward span progress |
| +0xD4  | 4    | Back-reference to slot index |

---

## Summary

The track geometry system is built on a ring buffer of 24-byte span records, each defining a trapezoidal road section via 4 corner vertices. 12 span types handle straights, transitions, junctions, and circuit wrap-around sentinels. Position tracking uses cross-product edge tests to detect boundary crossings, with recursive neighbor traversal for multi-span transitions.

The AI routing pipeline samples a target point 4 spans ahead on the chosen route (LEFT/RIGHT), computes heading delta, and feeds it through a speed-scaled steering controller. Peer avoidance biases the lateral offset to prevent same-lane bunching. The 3-state throttle controller (documented in `ai-system-deep-dive.md`) uses the heading delta magnitude to decide coast/brake/accelerate.

Contact resolution uses barycentric interpolation on the selected triangle half of each span quad to compute ground height, supporting both flat height queries and full surface normal extraction for physics.
