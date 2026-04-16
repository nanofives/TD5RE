# Level ZIP File Formats (level%03d.zip)

Each track in Test Drive 5 is packaged as `level%03d.zip` (e.g., `level001.zip`).
The master loader is `LoadTrackRuntimeData` at **VA 0x42FB90**, called from
`InitializeRaceSession` (0x42AA10).

---

## Complete File Inventory

Files loaded from `level%03d.zip` during race init:

| File | Loader | Direction Variants | Heap? |
|------|--------|--------------------|-------|
| STRIP.DAT / STRIPB.DAT | LoadTrackRuntimeData | Yes (B = reverse) | Yes |
| LEFT.TRK / LEFTB.TRK | LoadTrackRuntimeData | Yes | Yes |
| RIGHT.TRK / RIGHTB.TRK | LoadTrackRuntimeData | Yes | Yes |
| TRAFFIC.BUS / TRAFFICB.BUS | LoadTrackRuntimeData | Yes | Yes |
| CHECKPT.NUM | LoadTrackRuntimeData | No (shared) | No (static buf) |
| LEVELINF.DAT | LoadTrackRuntimeData | No (shared) | Yes |
| MODELS.DAT | InitializeRaceSession | No | Yes (32-aligned) |

Files from companion archives:

| File | Archive | Loader |
|------|---------|--------|
| SKY.PRR / sky.prr | STATIC.ZIP / static.zip | InitializeRaceSession |
| TEXTURES.DAT | level%03d.zip | LoadTrackTextureSet |
| tpage%d.dat | level%03d.zip | LoadRaceTexturePages |
| load%02d.tga | LOADING.ZIP | InitializeRaceSession |

### Direction-Variant Filename Tables

Four pointer tables at VA 0x4673B8..0x4673C4 select forward/reverse filenames.
Index `[0]` = forward, index `[gReverseTrackDirection * 4]` = reverse (B suffix).

| Table Base | Forward | Reverse |
|------------|---------|---------|
| 0x4673B8 | LEFT.TRK | LEFTB.TRK |
| 0x4673BC | RIGHT.TRK | RIGHTB.TRK |
| 0x4673C0 | TRAFFIC.BUS | TRAFFICB.BUS |
| 0x4673C4 | STRIP.DAT | STRIPB.DAT |

---

## TRAFFIC.BUS / TRAFFICB.BUS -- Traffic Spawn Table

### Overview
Defines the sequence of ambient traffic vehicle spawn points along the track.
Each record specifies a track span where a traffic vehicle should appear, which
lane it occupies, and which direction it faces. The runtime consumes records
sequentially as a FIFO queue.

### Global Pointers
- `DAT_004aed8c` (VA 0x4AED8C): Heap pointer to raw file data
- `DAT_004b08b8` (VA 0x4B08B8): Running read cursor (short*) into the data

### File Structure

```
Offset  Size  Type     Description
------  ----  ----     -----------
0x00    2     int16    Span index of first traffic spawn (-1 = end sentinel)
0x02    1     byte     Flags: bit 0 = direction polarity (0=forward, 1=oncoming)
0x03    1     byte     Lane index (0-based, must be < lane count of target span)
0x04    ...            Next record (repeating 4-byte records)
...
N*4     2     int16    -1 (0xFFFF) = end-of-list sentinel
```

**Record size: 4 bytes** (2-byte span index + 1-byte flags + 1-byte lane).

### Field Details

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| +0x00 | int16 | span_index | Track span where this traffic vehicle spawns. Signed short; -1 marks end of data. |
| +0x02 | byte | flags | Bit 0: direction polarity. 0 = same direction as player (forward). 1 = oncoming traffic (heading flipped 180 degrees via +0x80000 to heading). |
| +0x03 | byte | lane | Lane index within the span. Compared against `(strip_record[span*0x18 + 3] & 0x0F)` (lane count from STRIP.DAT). If lane >= lane_count, the vehicle is placed on the alternate route table instead. |

### Runtime Consumption

**InitializeTrafficActorsFromQueue** (VA 0x435930):
- Called once during `InitializeRaceSession`
- Fills traffic actor slots 6..11 (max 12 actors total, 6 racer + 6 traffic)
- For each slot: reads one record from the queue, initializes position/heading
- Advances `DAT_004b08b8 += 2` (4 bytes) per record consumed

**RecycleTrafficActorFromQueue** (VA 0x435310):
- Called per-frame during race simulation
- Scans ahead in queue to find a spawn point sufficiently ahead of player (>= 0x28 spans)
- Finds the traffic slot furthest behind player (>= 0x29 spans behind) and recycles it
- Reads direction flag and lane from the record
- Slot 9 is protected if a special encounter actor is active

### Spawn Logic
1. File is pre-sorted by ascending span index
2. Records with span < player_span or within 0x28 spans ahead are skipped
3. When lane < strip lane_count: vehicle placed on primary route table
4. When lane >= strip lane_count: vehicle placed on alternate/secondary route
5. Heading computed from strip geometry, then flipped if direction flag is set

### Actor Limits
- `DAT_004aaf00` = total racer count: 6 (normal) or 2 (time trial)
- Traffic actors occupy slots 6..11 (up to 6 traffic vehicles)
- Max total actors capped at 12 (`if (0xc < DAT_004aaf00) iVar = 0xc`)
- Traffic disabled in: time trial, benchmark, 2-player, network replay

---

## CHECKPT.NUM -- Checkpoint Remapping Matrix

### Overview
A fixed 96-byte file containing span-index swap pairs used to remap track strip
data when running a track in reverse direction. This is NOT the checkpoint timer
data (that comes from a hardcoded per-track table in the EXE).

### Global Storage
- Loaded directly into static buffer at `DAT_004aedb0` (VA 0x4AEDB0), size 0x60 (96 bytes)
- No heap allocation; overwritten each track load

### File Structure

```
96 bytes = 24 x int32 = 4 rows x 6 columns matrix

Column layout (stride = 16 bytes = 4 dwords between columns):
  Col 0: bytes 0x00..0x0F  (4 ints at offsets 0, 4, 8, 12)
  Col 1: bytes 0x10..0x1F  (4 ints at offsets 16, 20, 24, 28)
  Col 2: bytes 0x20..0x2F  (4 ints at offsets 32, 36, 40, 44)
  Col 3: bytes 0x30..0x3F  (4 ints at offsets 48, 52, 56, 60)
  Col 4: bytes 0x40..0x4F  (4 ints at offsets 64, 68, 72, 76)
  Col 5: bytes 0x50..0x5F  (4 ints at offsets 80, 84, 88, 92)

Each int32 is either:
  - A strip span index (>= 0): identifies a span in STRIP.DAT
  - -1 (0xFFFFFFFF): unused/skip (no swap needed)
```

### Matrix Layout (row-major, 4 rows x 6 columns)

```
        Col0    Col1    Col2    Col3    Col4    Col5
Row 0:  [0x00]  [0x10]  [0x20]  [0x30]  [0x40]  [0x50]
Row 1:  [0x04]  [0x14]  [0x24]  [0x34]  [0x44]  [0x54]
Row 2:  [0x08]  [0x18]  [0x28]  [0x38]  [0x48]  [0x58]
Row 3:  [0x0C]  [0x1C]  [0x2C]  [0x3C]  [0x4C]  [0x5C]
```

### Remapping Algorithm

**RemapCheckpointOrderForTrackDirection** (VA 0x42FD70):
- Called from `InitializeRaceSession` only when `gReverseTrackDirection != 0`
- Iterates 4 rows (piVar1 from 0x4AEDB0 to < 0x4AEDC0)
- Two remapping modes selected by `DAT_004aee00` (= int at file offset 0x50, Col5 Row0):

**Mode A** (DAT_004aee00 == -1, simple reverse):
```
For each row:
  Swap strip entries at Col0 <-> Col4  (offsets [row] <-> [row+0x40])
  Swap strip entries at Col1 <-> Col3  (offsets [row+0x10] <-> [row+0x30])
```

**Mode B** (DAT_004aee00 != -1, alternate route reverse):
```
For each row:
  Swap strip entries at Col0 <-> Col5  (offsets [row] <-> [row+0x50])
  Swap strip entries at Col1 <-> Col4  (offsets [row+0x10] <-> [row+0x40])
  Swap strip entries at Col2 <-> Col3  (offsets [row+0x20] <-> [row+0x30])
```

Swaps are performed by `SwapIndexedRuntimeEntries` (VA 0x40B530), which
swaps both the 8-byte entries in the strip coordinate table and the 4-byte
entries in a secondary index table. Entries with value -1 are skipped.

### Purpose
When a track is driven in reverse, certain strip spans that define junctions,
lane merges, or checkpoint gates need their left/right or start/end geometry
swapped. This file provides up to 4 x 6 = 24 swap pairs to handle all such
cases per track. Mode A handles simple out-and-back reversal; Mode B handles
tracks with alternate routes that also need their fork/merge points adjusted.

---

## LEVELINF.DAT -- Track Environment Configuration

### Overview
A variable-length binary blob that describes the track's environment settings:
circuit vs point-to-point layout, weather, fog, traffic permission, smoke
effects, and per-segment weather density zones.

### Global Pointer
- `gTrackEnvironmentConfig` (VA 0x4AEE20): Heap pointer to loaded data

### Field Map

| Offset | Size | Type | Field | Consumers |
|--------|------|------|-------|-----------|
| +0x00 | 1 | byte | track_type | InitializeRaceSession: 1=circuit, else=point-to-point |
| +0x04 | 4 | int32 | smoke_enabled | InitializeRaceSmokeSpritePool: 1=allocate smoke pool |
| +0x08 | 4 | int32 | checkpoint_finish_enabled | CheckRaceCompletionState: 0=skip P2P completion check |
| +0x0C | 1 | byte | traffic_allowed | InitializeRaceSession: 0=force disable traffic actors |
| +0x17 | 1 | byte | fog_distance | InitializeRaceSession: 0=disable fog; passed to ConfigureRaceFogColorAndMode |
| +0x18 | 1 | byte | fog_color_r | Red component of fog color (0-255) |
| +0x28 | 4 | int32 | weather_type | InitializeWeatherOverlayParticles: 0=rain, 1=snow |
| +0x2C | 4 | int32 | weather_zone_count | Number of (segment, density) pairs in weather zone array |
| +0x36 | N*4 | array | weather_zones[] | Array of {int16 segment_id, int16 density} pairs |
| +0x61 | 1 | byte | fog_color_g | Green component of fog color (0-255) |
| +0x62 | 1 | byte | fog_color_b | Blue component of fog color (0-255) |

**Note:** Offsets +0x36 and +0x61/+0x62 imply the weather zone array has a
maximum of ~10-11 entries before overlapping the fog color fields (depending
on alignment). The fog color is packed as `(R << 16) | (G << 8) | B` by
InitializeRaceSession and passed to `ConfigureRaceFogColorAndMode`.

### Weather Density Zones

The weather zone array at +0x36 contains `weather_zone_count` entries of 4 bytes each:

```
Offset  Size  Type    Description
0x00    2     int16   segment_id  -- Track span where density changes
0x02    2     int16   density     -- Particle density (0-128, clamped to 0x80)
```

**UpdateAmbientParticleDensityForSegment** (VA 0x4464C2):
- Called per-frame per-view
- Walks the zone array; when actor's current span matches a zone's segment_id,
  updates the per-view particle density to the zone's value
- Density clamped to maximum 128 (0x80)
- Controls rain/snow intensity as the player drives through different track sections

### Track Type Effects

| track_type | Layout | Finish Condition | Lap Counter |
|------------|--------|------------------|-------------|
| 0 | Point-to-point | Reach final checkpoint | N/A |
| 1 | Circuit | Complete N laps | Uses checkpoint metadata +0x04 as finish span |

---

## Per-Track Checkpoint Metadata (hardcoded in EXE, NOT from ZIP)

### Overview
Checkpoint timing data is NOT in the level ZIP. It is stored in a static table
in the EXE at VA 0x46CF6C (18 pointer entries, one per track pool ID).
`LoadTrackRuntimeData` copies 24 bytes from this table into `DAT_004aed98`,
then sets `DAT_004aed88` (VA 0x4AED88) to point to the copy.

### Structure (24 bytes per track)

```
Offset  Size    Type    Description
0x00    1       byte    checkpoint_count -- Total number of checkpoints (1-5 typical)
0x01    1       byte    (padding)
0x02    2       uint16  initial_time -- Starting timer value (game ticks at 30fps)
0x04    N*4     array   checkpoints[] -- Array of checkpoint_count entries
```

Each checkpoint entry (4 bytes):

```
Offset  Size    Type    Description
0x00    2       uint16  span_threshold -- Strip span index that triggers this checkpoint
0x02    2       uint16  time_bonus -- Timer ticks added when checkpoint is reached
```

### Example: Track Pool 1 (VA 0x46CBB0)

```
05 00 3B 64  -- 5 checkpoints, initial_time = 0x643B (25,659 ticks = ~855s)
65 03 00 3C  -- CP0: span 0x0365 (869),  bonus 0x3C00 (15,360 ticks)
E7 05 00 2D  -- CP1: span 0x05E7 (1511), bonus 0x2D00 (11,520 ticks)
0D 08 00 3C  -- CP2: span 0x080D (2061), bonus 0x3C00 (15,360 ticks)
3A 0A 00 28  -- CP3: span 0x0A3A (2618), bonus 0x2800 (10,240 ticks)
02 0C 00 00  -- CP4: span 0x0C02 (3074), bonus 0x0000 (final checkpoint)
```

### Difficulty Scaling

**AdjustCheckpointTimersByDifficulty** (VA 0x40A530):
- Tier 0 (hard): multiply initial_time and all bonuses by 12/10 (1.2x)
- Tier 1 (medium): multiply by 11/10 (1.1x)
- Tier 2 (easy): no adjustment (base values)

### Circuit Tracks
For circuit tracks, `checkpoint_metadata[+0x04]` (first span_threshold) doubles
as the finish-line span index. The lap counter increments when an actor crosses
this span with a specific progress flag set to 0x0F.

---

## Per-Track Lighting Table (hardcoded in EXE, NOT from ZIP)

### Overview
A per-track array of lighting zone entries, pointed to by `DAT_004aee14`
(VA 0x4AEE14). Loaded from the static table at VA 0x469C78 (indexed by track ID).
Each entry is 36 bytes (0x24).

### Entry Structure (36 bytes)

```
Offset  Size  Type    Field
0x00    2     int16   span_start -- First span where this lighting zone applies
0x02    2     int16   span_end -- Last span of this lighting zone
0x04    ...   ...     Light direction vectors, color, intensity parameters
0x10    4     int32   Fog color override
0x18    1     byte    Light mode (switch cases 0, 1, 2 in ApplyTrackLightingForVehicleSegment)
0x19    1     byte    Blend distance (for mode 1 interpolation)
0x1C    4     int32   Projection effect parameter (passed to ConfigureActorProjectionEffect)
```

### Consumers
- **ApplyTrackLightingForVehicleSegment** (VA 0x430150): Per-frame per-actor lighting
- **UpdateActorTrackLightState** (VA 0x40CDA0): Light state machine transitions

---

## MODELS.DAT -- Track 3D Model Container

> **Full format documentation:** See `re/analysis/3d-asset-formats.md`

### Overview
Contains all 3D mesh resources (trackside objects, buildings, bridges, trees)
for the track. Loaded into a 32-byte-aligned heap buffer.

### Structure

```
Offset  Size        Type        Description
0x00    4           uint32      entry_count -- Number of model groups
0x04    N*8         array       entry_table[] -- Pairs of {offset, param} per group
0x04+N*8  ...       blob        Mesh data (PRR sub-resources)
```

Each entry pair (8 bytes):
- **Dword 0**: Offset from file start to a sub-mesh block (relocated at load time)
- **Dword 1**: Additional parameter (purpose unconfirmed; possibly LOD or flags)

Each sub-mesh block starts with a count (uint32) followed by that many relative
offsets to individual PRR mesh resources. PRR meshes have a 0x38-byte header
with bounding sphere, origin, and offsets to command/vertex/normal arrays.
Vertices are 44 bytes each (11 floats: XYZ position, XYZ view-space, lighting
intensity, UV texture coords, UV projection coords).

**ParseModelsDat** (VA 0x431190): Relocates all internal offsets, calls
`PrepareMeshResource` on each sub-mesh, and applies per-entry brightness
adjustment from a per-track table at `DAT_00466d98[track_id * 4]`.

### Runtime Globals
- `gModelsDatEntryCount` -- Number of model groups
- `gModelsDatEntryTable` -- Pointer to entry pair array
- `gTrackSpanRingLength` -- Return value of ParseModelsDat (track ring span count)
- `DAT_004aee54` -- Pointer past entry table (start of mesh data)

---

## TEXTURES.DAT -- Track Texture Definitions

> **Full format documentation:** See `re/analysis/3d-asset-formats.md`

### Overview
Contains palettized 64x64 texture definitions for track geometry. Each texture
has a format byte (opaque/transparent/semi-transparent), a 256-entry RGB palette,
and 4096 bytes of palette indices. Loaded by `LoadTrackTextureSet` (0x442670) and
expanded to ARGB32 by `BuildTrackTextureCacheImpl` (0x40B1D0), then uploaded through
M2DX.dll as D3D textures with mipmap generation.

### Per-Texture Record

```
+0x03  byte    format_type (0=opaque, 1=color-keyed, 2=semi-transparent, 3=opaque-alt)
+0x04  int32   palette_count (typically 256)
+0x08  N*3     RGB palette entries
+0x08+N*3      4096 bytes of palette indices (64x64 pixels)
```

---

## tpage%d.dat -- Texture Page Pixel Data

> **Full format documentation:** See `re/analysis/3d-asset-formats.md`

### Overview
Raw uncompressed pixel data for texture pages used by named entries in static.hed
(car skins, sky, wheels, traffic, environment maps). Dimensions and pixel format
are defined by the per-page metadata in static.hed.

- **RGB24 pages:** 3 bytes/pixel (R,G,B), for opaque textures
- **RGBA32 pages:** 4 bytes/pixel (A,R,G,B), for transparent textures

Typically 256x256 pixels per page. Loaded from both `static.zip` and the level
ZIP. Resampled to target dimensions if source != target, then uploaded via
`DXD3DTexture::LoadRGBS24` or `DXD3DTexture::LoadRGBS32` in M2DX.dll.

---

## Summary of level%03d.zip Contents

| File | Size | Purpose |
|------|------|---------|
| STRIP.DAT | Variable | Track geometry: 5-dword header + 24B/span records |
| STRIPB.DAT | Variable | Reverse-direction strip geometry |
| LEFT.TRK | Variable | Left track boundary data |
| LEFTB.TRK | Variable | Left boundary (reverse) |
| RIGHT.TRK | Variable | Right track boundary data |
| RIGHTB.TRK | Variable | Right boundary (reverse) |
| TRAFFIC.BUS | Variable | Forward traffic spawn table (4B records, -1 terminated) |
| TRAFFICB.BUS | Variable | Reverse traffic spawn table |
| CHECKPT.NUM | 96 bytes | Checkpoint remapping matrix (4x6 int32 swap pairs) |
| LEVELINF.DAT | ~100+ bytes | Environment config (circuit/weather/fog/traffic flags) |
| MODELS.DAT | Variable | 3D trackside model container (relocated PRR meshes) |
| TEXTURES.DAT | Variable | Track texture header/palette data |
| tpage%d.dat | Variable | Texture page pixel data (multiple files) |
