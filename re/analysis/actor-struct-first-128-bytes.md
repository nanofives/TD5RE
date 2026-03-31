# TD5_Actor Struct: First 128 Bytes (0x000-0x07F) -- Contact Probe Track State

## Summary

The first 128 bytes of the actor struct contain **8 contact probe track-position entries**, each 16 bytes (0x10). These are NOT bounding box corners or model data as previously hypothesized. They are per-probe track segment state records used for boundary collision detection, ground contact computation, and wheel contact frame updates.

- **Entries 0-3** (0x00-0x3F): Correspond to the 4 wheel contact probes (FL, FR, RL, RR).
- **Entries 4-7** (0x40-0x7F): Correspond to 4 body corner/bumper contact probes (front-left, front-right, rear-left, rear-right bumper corners).

Each entry mirrors the same layout as the main track position block at 0x80-0x8F, and is passed as `short *` to `UpdateActorTrackPosition` and `ComputeActorTrackContactNormal`.

## Per-Entry Layout (16 bytes each)

Each 16-byte entry is a `TrackProbeState` with the following fields:

| Offset (in entry) | Byte offset (entry N) | Type     | Size | Name                      | Description |
|---|---|---|---|---|---|
| +0x00 | N*0x10 + 0x00 | int16_t  | 2 | `span_index`              | Current STRIP.DAT span index for this probe |
| +0x02 | N*0x10 + 0x02 | int16_t  | 2 | `span_normalized`         | Span index modulo ring length (inferred, same as main entry) |
| +0x04 | N*0x10 + 0x04 | int16_t  | 2 | `span_accumulated`        | Monotonic forward span counter |
| +0x06 | N*0x10 + 0x06 | int16_t  | 2 | `span_high_water`         | High-water mark for forward progress |
| +0x08 | N*0x10 + 0x08 | int16_t  | 2 | `contact_vertex_index_A`  | Track vertex index A (left edge), written by ComputeActorTrackContactNormal |
| +0x0A | N*0x10 + 0x0A | int16_t  | 2 | `contact_vertex_index_B`  | Track vertex index B (right edge), written by ComputeActorTrackContactNormal |
| +0x0C | N*0x10 + 0x0C | int8_t   | 1 | `sub_lane_index`          | Sub-lane within current span (0..N-1) |
| +0x0D | N*0x10 + 0x0D | uint8_t  | 1 | `_pad_0D`                 | Padding / unused |
| +0x0E | N*0x10 + 0x0E | int16_t  | 2 | `_pad_0E`                 | Padding / unused |

## Full 0x00-0x7F Map

| Byte Offset | Type | Name | Entry | Confidence |
|---|---|---|---|---|
| 0x000-0x001 | int16_t | `wheel_probe_FL.span_index` | Wheel FL (entry 0) | CONFIRMED |
| 0x002-0x003 | int16_t | `wheel_probe_FL.span_normalized` | Wheel FL | INFERRED |
| 0x004-0x005 | int16_t | `wheel_probe_FL.span_accumulated` | Wheel FL | CONFIRMED |
| 0x006-0x007 | int16_t | `wheel_probe_FL.span_high_water` | Wheel FL | CONFIRMED |
| 0x008-0x009 | int16_t | `wheel_probe_FL.contact_vertex_A` | Wheel FL | CONFIRMED |
| 0x00A-0x00B | int16_t | `wheel_probe_FL.contact_vertex_B` | Wheel FL | CONFIRMED |
| 0x00C       | int8_t  | `wheel_probe_FL.sub_lane_index` | Wheel FL | CONFIRMED |
| 0x00D       | uint8_t | `wheel_probe_FL._pad` | Wheel FL | INFERRED |
| 0x00E-0x00F | int16_t | `wheel_probe_FL._reserved` | Wheel FL | INFERRED |
| 0x010-0x01F | TrackProbeState | `wheel_probe_FR` | Wheel FR (entry 1) | CONFIRMED |
| 0x020-0x02F | TrackProbeState | `wheel_probe_RL` | Wheel RL (entry 2) | CONFIRMED |
| 0x030-0x03F | TrackProbeState | `wheel_probe_RR` | Wheel RR (entry 3) | CONFIRMED |
| 0x040-0x04F | TrackProbeState | `body_probe_FL`  | Body FL corner (entry 4) | CONFIRMED |
| 0x050-0x05F | TrackProbeState | `body_probe_FR`  | Body FR corner (entry 5) | CONFIRMED |
| 0x060-0x06F | TrackProbeState | `body_probe_RL`  | Body RL corner (entry 6) | CONFIRMED |
| 0x070-0x07F | TrackProbeState | `body_probe_RR`  | Body RR corner (entry 7) | CONFIRMED |

## Evidence by Function

### UpdateActorTrackPosition (0x4440F0)
- Receives `short *param_1` pointing to a probe entry
- Reads `param_1[0]` as span index (+0x00)
- Reads/writes `param_1[2]` as accumulated span (+0x04)
- Reads/writes `param_1[3]` as high-water mark (+0x06)
- Reads/writes `(char)param_1[6]` as sub-lane index (+0x0C, byte access)
- **Confidence: HIGH** -- this is the primary consumer of these fields

### ComputeActorTrackContactNormal (0x445450)
- Receives `short *param_1` pointing to a probe entry
- Reads `param_1[0]` as span index
- Reads `(char)param_1[6]` as sub-lane index
- **Writes** `param_1[4]` with left-edge vertex index (+0x08)
- **Writes** `param_1[5]` with right-edge vertex index (+0x0A)
- **Confidence: HIGH** -- writes the vertex index fields

### RenderVehicleActorModel (0x4092D0)
- First loop (4 iterations, stride 0x10, starting at actor+0x00 = entries 0-3):
  - Writes `*(short *)pRVar7 = sVar3` (copies main track_span_raw to entry's span_index)
  - Writes `pRVar7->field_0xc = uVar2` (copies main track_sub_lane_index to entry's sub_lane_index)
  - Calls `UpdateActorTrackPosition((short *)pRVar7, piVar8)` for each wheel
  - Calls `ComputeActorTrackContactNormal((short *)pRVar7, piVar8, piVar8)` for each wheel
- Second loop (4 iterations, stride 0x10, starting at actor+0x40 = entries 4-7):
  - Same pattern: writes span_index and sub_lane_index from main actor state
  - Calls `UpdateActorTrackPosition` and `ComputeActorTrackContactNormal`
- **Confidence: HIGH** -- directly demonstrates 8 entries at stride 0x10

### RefreshVehicleWheelContactFrames (0x403720)
- First loop (4 iterations, stride 0x10, starting at actor+0x40 = entries 4-7):
  - Wait -- NOTE: This function casts `actor = (RuntimeSlotActor *)&actor->field_0x40` then uses 0x10 stride
  - Actually, careful re-reading shows:
    - `actor = (RuntimeSlotActor *)&actor->field_0x40;` -- but this is the *second* loop pointer
    - The first loop iterates entries 0-3 at 0x00-0x3F with stride 0x10
  - Writes span_index and sub_lane_index from the main actor track state
  - Calls UpdateActorTrackPosition and ComputeActorTrackContactNormalExtended for wheel entries
- Second loop (4 iterations, stride 0x10, starting at pRVar7+0x00, reusing pRVar7 as actor base for entries 4-7):
  - Same pattern for body corner probes
  - Calls UpdateActorTrackPosition and ComputeActorTrackContactNormal
- **Confidence: HIGH**

### UpdateActorTrackSegmentContacts (0x406CC0)
- Iterates 8 times using byte index table at 0x467384 or 0x46738c
- For each index `iVar6`:
  - Reads `*(short *)(&actor->field_0x0 + iVar6 * 0x10)` as span index
  - Reads `(char)(&actor->field_0xc)[iVar6 * 0x10]` as sub_lane_index
  - Uses probe world position from `actor->probe_FL_x + iVar6 * 3` (at 0x90+iVar6*0xC)
- Normal mode index table (0x467384): `{0, 1, 2, 3, 0xFF}` -- only wheel probes
- Scripted mode index table (0x46738c): `{0, 1, 2, 3, 4, 5, 6, 7}` -- all 8 probes
- **Confidence: HIGH** -- demonstrates indexed access to all 8 entries

### UpdateActorTrackSegmentContactsForward (0x406F50)
- Same pattern as above, iterates 8 probe entries with stride 0x10
- Reads `*(short *)param_1` (span_index) and advances param_1 by 0x10 each iteration
- **Confidence: HIGH**

### InitializeRaceActorRuntime (0x432E60)
- Writes bounding box corner shorts at DAT_004ad288+0x40 through +0x5C, and +0x80 through +0x84
  - These are offset from a traffic actor's base, writing into entries 4-7's region
  - Values are signed int16 coordinates (e.g., 0xFF10=-240, 0xF0=240, 0x208=520, 0xFE52=-430)
  - These appear to be hardcoded bumper corner offsets for traffic vehicles
- Also initializes per-actor route state in gActorRouteStateTable (separate from actor struct)
- **Confidence: MEDIUM** -- the +0x40..+0x84 writes to DAT_004ad288 area are to the actor body probe entries, but the values look like bounding box coordinates being stored as initial probe offsets

### ComputeActorWorldBoundingVolume (0x4096B0)
- Does NOT directly access 0x00-0x7F range
- Reads probe positions at 0x90-0xBF (probe_FL through probe_RR) and world_pos
- Reads/writes recovery_target_matrix at 0x180
- **Confidence: N/A for 0x00-0x7F range**

### RenderRaceActorForView (0x40C120)
- Reads probe positions at 0x90-0xBC for shadow/wheel rendering
- Does not directly access 0x00-0x7F
- **Confidence: N/A for 0x00-0x7F range**

## Relationship to Existing Struct Fields

The existing header describes 0x080-0x08F as the "track position state" block with:
- `track_span_raw` at 0x080
- `track_span_normalized` at 0x082
- `track_span_accumulated` at 0x084
- `track_span_high_water` at 0x086
- gap at 0x088
- `track_sub_lane_index` at 0x08C

This is the **main body** (chassis center) track position. The 8 entries at 0x00-0x7F are per-probe copies that track each wheel/corner's individual position on the track strip network. Before each contact update frame, the main span_index and sub_lane_index are copied into each entry, then each entry is independently updated by `UpdateActorTrackPosition` based on that probe's world position.

The existing header also describes 0x0F0-0x11F as "per-wheel track contact data (4 wheels x 6 shorts = 48 bytes)". This is SEPARATE from entries 0-3 -- those 0x0F0 fields are likely the extended contact normal output data, while entries 0-3 hold the track position state for each wheel. The `contact_vertex_index_A/B` at +0x08/+0x0A in each entry correspond to fields previously attributed to the 0x0F0 region.

## Proposed Sub-structure Definition

```c
typedef struct TD5_TrackProbeState {
    int16_t  span_index;            /* +0x00: current STRIP.DAT span index */
    int16_t  span_normalized;       /* +0x02: span modulo ring length */
    int16_t  span_accumulated;      /* +0x04: monotonic forward span counter */
    int16_t  span_high_water;       /* +0x06: high-water mark for ordering */
    int16_t  contact_vertex_A;      /* +0x08: left-edge track vertex index */
    int16_t  contact_vertex_B;      /* +0x0A: right-edge track vertex index */
    int8_t   sub_lane_index;        /* +0x0C: sub-lane within span */
    uint8_t  _pad_0D;              /* +0x0D: padding */
    int16_t  _pad_0E;             /* +0x0E: padding/unused */
} TD5_TrackProbeState;  /* 16 bytes */
```

Replacement for `uint8_t model_data[0x80]` in TD5_Actor:

```c
/* === CONTACT PROBE TRACK STATE (0x000-0x07F) ================
 *
 * 8 per-probe track position entries, each 16 bytes.
 * Entries 0-3: wheel probes (FL, FR, RL, RR)
 * Entries 4-7: body corner probes (FL, FR, RL, RR bumper)
 *
 * Before each contact frame, main span_index (0x080) and
 * sub_lane_index (0x08C) are copied into each entry. Each
 * probe is then independently updated by UpdateActorTrackPosition
 * based on that probe's world position (0x090-0x0BF for wheels,
 * 0x0C0-0x0EF for body corners).
 *
 * Written by: RenderVehicleActorModel, RefreshVehicleWheelContactFrames
 * Read by: UpdateActorTrackSegmentContacts[Forward/Reverse]
 */
TD5_TrackProbeState  wheel_probe[4];    /* +0x000: FL=0, FR=1, RL=2, RR=3 */
TD5_TrackProbeState  body_probe[4];     /* +0x040: FL=4, FR=5, RL=6, RR=7 */
```

## Index Tables

Two constant byte arrays control which probes are checked during track boundary contact resolution:

- **Normal physics mode** (0x467384): `{0, 1, 2, 3, 0xFF}` -- only 4 wheel probes checked
- **Scripted/recovery mode** (0x46738c): `{0, 1, 2, 3, 4, 5, 6, 7}` -- all 8 probes checked

This explains why the full 8 probes exist: during scripted collision recovery mode, the body corners are also checked for track boundary contacts, providing more comprehensive collision response.

## Confidence Summary

| Field | Confidence |
|---|---|
| 8 x 16-byte entry layout | HIGH -- confirmed by stride 0x10 in 5+ functions |
| span_index at +0x00 | HIGH -- read/written by UpdateActorTrackPosition |
| span_accumulated at +0x04 | HIGH -- read/written by UpdateActorTrackPosition |
| span_high_water at +0x06 | HIGH -- read/written by UpdateActorTrackPosition |
| contact_vertex_A at +0x08 | HIGH -- written by ComputeActorTrackContactNormal |
| contact_vertex_B at +0x0A | HIGH -- written by ComputeActorTrackContactNormal |
| sub_lane_index at +0x0C | HIGH -- read/written as char in 5+ functions |
| span_normalized at +0x02 | MEDIUM -- inferred by analogy with main entry at 0x82 |
| padding at +0x0D..+0x0F | MEDIUM -- no observed accesses |
| Entries 0-3 = wheels | HIGH -- loop count 4, matches wheel ordering |
| Entries 4-7 = body corners | HIGH -- second loop of 4, car_definition_ptr+0x20 offsets |
