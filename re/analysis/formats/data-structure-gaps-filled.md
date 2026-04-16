# Data Structure Gaps Filled (Items 8-17)

Completed 2026-03-19 via Ghidra decompilation and memory analysis.

---

## 1. STRIP.DAT Span Record (24 bytes / 0x18 per span)

Runtime base pointer: `DAT_004c3d9c` (resolved by `BindTrackStripRuntimePointers` at 0x444070).

```c
struct StripSpanRecord {  // 24 bytes, indexed: DAT_004c3d9c + span_index * 0x18
    /* +0x00 */ uint8_t  span_type;            // 0-11, selects wall geometry variant
    /* +0x01 */ uint8_t  surface_attribute;     // Low nibble = surface type (road/grass/gravel/etc)
                                                // High nibble = alternate surface (used when lane bit set)
    /* +0x02 */ uint8_t  lane_bitmask;          // Bit N=1 means lane N uses alternate surface
    /* +0x03 */ uint8_t  geometry_metadata;     // Low nibble = vertex_count_minus_one (lane width)
                                                //   used as: (byte & 0xF) for vertex delta offsets
                                                // High nibble = reserved/unused
    /* +0x04 */ uint16_t left_vertex_start;     // Index into coordinate table (DAT_004c3d98) for left edge
    /* +0x06 */ uint16_t right_vertex_start;    // Index into coordinate table for right edge
    /* +0x08 */ int16_t  unknown_08;            // Not directly accessed in decompiled span consumers
    /* +0x0A */ int16_t  link_span;             // Forward/backward link (set to span_count-1 at first, 0 at last)
    /* +0x0C */ int32_t  origin_x;              // World-space X origin of span
    /* +0x10 */ int32_t  origin_y;              // World-space Y origin (height/elevation)
    /* +0x14 */ int32_t  origin_z;              // World-space Z origin
};
```

### Field Evidence

**+0x00 span_type**: Read by `UpdateActorTrackSegmentContacts` (0x406cfd) as `*pbVar1` and used to index `DAT_004631A0[type*8]` for wall vertex offsets. Also read by `ProcessActorSegmentTransition` (0x4073af). Values 0-11 define wall/barrier configurations.

**+0x01 surface_attribute**: Read by `GetTrackSegmentSurfaceType` (0x42f100):
```c
if ((byte_at_02 & (1 << lane_index)) != 0)
    return (byte_at_01 >> 4) | 0x10;   // alternate surface + flag
return byte_at_01 & 0xF;               // primary surface
```
Written by `ApplyTrackStripAttributeOverrides` (0x42fb6d) at `+1` of each span record.

**+0x02 lane_bitmask**: Used by `GetTrackSegmentSurfaceType` as a per-lane alternate-surface selector. Bit N being set means lane N uses the high nibble surface from +0x01.

**+0x03 geometry_metadata**: Low nibble read as `(byte & 0xF)`:
- In `ProcessActorSegmentTransition`: determines the "width" of the span in vertex indices
- In `ProcessActorForwardCheckpointPass`: calculates right-edge vertex as `left_vertex + (low_nibble)`
- In `ComputeTrackSpanProgress`: combined with `DAT_00473C6C[type*8]` offset for projected vertex
- In `ApplyTrackLightingForVehicleSegment`: used to find right-side lighting vertex

**+0x04/+0x06 vertex indices**: Core coordinate references. Every function that accesses spans reads these to get left/right edge vertex positions from `DAT_004c3d98 + index * 6` (3 shorts per vertex: x, y, z).

**+0x08/+0x0A**: Offset +0x0A is written by `BindTrackStripRuntimePointers` for first/last spans (link patching). Offset +0x08 is not directly accessed in any decompiled function -- may be padding or used only by the STRIP.DAT file generator.

**+0x0C/+0x10/+0x14 origin**: Read as `int32_t` by `RenderTrackSegmentNearActor`, `BlendTrackLightEntryFromStart`, `ApplyTrackLightingForVehicleSegment`, and track contact functions. World-space 3D origin of the span center.

---

## 2. Span Type Vertex Offset Tables

Two parallel int32 tables at stride 8, indexed by span_type (0-11):

### DAT_004631A0: Left Edge Vertex Offset
### DAT_004631A4: Right Edge Vertex Offset

```c
// Usage: left_vertex = left_vertex_start + span_width + DAT_004631A0[type * 2]
//        right_vertex = right_vertex_start + span_width + DAT_004631A4[type * 2]
struct SpanTypeVertexOffsets {   // at 0x4631A0, stride 8 bytes (2 ints)
    int32_t left_offset;         // +0x00
    int32_t right_offset;        // +0x04
};
```

| Type | Left Offset | Right Offset | Description |
|------|------------|--------------|-------------|
| 0    |  0 |  0 | Standard road (no wall shift) |
| 1    |  0 |  0 | Standard variant |
| 2    | -1 |  0 | Left wall inset by 1 vertex |
| 3    | -1 |  0 | Left wall variant |
| 4    | -2 |  0 | Left wall inset by 2 vertices |
| 5    |  0 | -1 | Right wall inset by 1 vertex |
| 6    |  0 | -1 | Right wall variant |
| 7    |  0 | -2 | Right wall inset by 2 vertices |
| 8    |  0 |  0 | Open span |
| 9-11 |  0 |  0 | Special/sentinel types |

Consumer: `UpdateActorTrackSegmentContacts` (0x406cfd), `ProcessActorSegmentTransition` (0x4073af).

---

## 3. Span Type Suspension Response Table (0x46318C)

```c
// 12-byte table: one byte per span type (0-11)
uint8_t gSpanTypeSuspensionFlag[12] = {
    1, 1, 1, 0,   // types 0-3
    1, 0, 0, 0,   // types 4-7
    1, 0, 0, 0    // types 8-11
};
```

Consumer: `IntegrateVehiclePoseAndContacts` (0x405e80):
```c
if (gSpanTypeSuspensionFlag[vehicle_span_type] != 0 &&
    (contact_mask & (vehicle_span_type ^ 0xF)) == 0) {
    // Apply full suspension correction (gravity-compensated vertical force)
    actor->velocity_y = (avg_suspension_delta - prev_height) - gGravityConstant;
    UpdateVehicleSuspensionResponse(actor);
}
```

When the flag is 0, the vehicle is considered "off-road" for that span type and suspension correction is skipped, causing bouncy/rough handling.

---

## 4. Per-Track Brightness Offset Table (0x466D98)

```c
int32_t gTrackBrightnessOffset[20];  // at 0x466D98, indexed by track pool ID (1-18)
```

| Pool | Hex Value | Decimal | Meaning |
|------|-----------|---------|---------|
| 0    | 0x00000027 | +39    | (unused/base) |
| 1    | 0x00000040 | +64    | Bright |
| 2    | 0x00000040 | +64    | Bright |
| 3    | 0x00000040 | +64    | Bright |
| 4    | 0x00000040 | +64    | Bright |
| 5    | 0x00000000 |  0     | Neutral |
| 6    | 0x00000040 | +64    | Bright |
| 7    | 0x00000040 | +64    | Bright |
| 8    | 0xFFFFFFE0 | -32    | Dark (night/tunnel) |
| 9    | 0x00000000 |  0     | Neutral |
| 10   | 0x00000040 | +64    | Bright |
| 11   | 0xFFFFFFE8 | -24    | Dark |
| 12   | 0x00000040 | +64    | Bright |
| 13   | 0x00000000 |  0     | Neutral |
| 14   | 0x00000000 |  0     | Neutral |
| 15   | 0x00000000 |  0     | Neutral |
| 16   | 0x00000000 |  0     | Neutral |
| 17   | 0x00000000 |  0     | Neutral |
| 18   | 0xFFFFFFE0 | -32    | Dark |
| 19   | 0xFFFFFFE0 | -32    | Dark |

Consumer: `InitializeRaceSession` (0x42aa10) passes `*(int*)(0x466D98 + pool_id * 4)` to `ParseModelsDat` (0x431190), which clamps `vertex_color + offset` to [0, 255] for every vertex in every track model mesh. Positive values brighten daytime tracks; negative values darken night/tunnel tracks.

---

## 5. Per-Track Lighting Zone Pointer Table (0x469C78)

```c
int32_t* gTrackLightingZoneTable[20];  // at 0x469C78, indexed by track pool ID
```

Each entry is a pointer to an array of **TrackLightZone** records (0x24 bytes each) stored in static .data:

```c
struct TrackLightZone {        // 36 bytes (0x24)
    /* +0x00 */ int16_t  start_span;       // First span index for this zone
    /* +0x02 */ int16_t  end_span;         // Last span index for this zone
    /* +0x04 */ int16_t  direction[3];     // Light direction vector (short x,y,z)
    /* +0x0A */ uint8_t  padding_0a;       //
    /* +0x0B */ uint8_t  wave_period;      // Period for oscillating light (case 1 mode)
    /* +0x0C */ uint8_t  ambient_r;        // Ambient red
    /* +0x0D */ uint8_t  ambient_g;        // Ambient green
    /* +0x0E */ uint8_t  ambient_b;        // Ambient blue (but accessed at +7 from shorts)
    /* +0x0F */ uint8_t  padding_0f;       //
    /* +0x10 */ uint8_t  diffuse_r;        // Diffuse red
    /* +0x11 */ uint8_t  diffuse_g;        // Diffuse green
    /* +0x12 */ uint8_t  diffuse_b;        // Diffuse blue
    /* +0x13 */ uint8_t  padding_13;       //
    /* +0x14 */ int16_t  elevation_offset; // Y-axis elevation bias
    /* +0x16 */ int16_t  padding_16;       //
    /* +0x18 */ int16_t  light_mode;       // 0=constant, 1=blended/oscillating, 2=custom
    /* +0x1A */ uint8_t  wave_amplitude_a; // oscillation parameter A (case 1)
    /* +0x1B */ uint8_t  wave_amplitude_b; // oscillation parameter B (case 1)
    /* +0x1C */ int32_t  projection_type;  // ConfigureActorProjectionEffect type code
    /* +0x20 */ int32_t  projection_param; // ConfigureActorProjectionEffect parameter
};
```

Consumer: `ApplyTrackLightingForVehicleSegment` (0x4302e8) walks zones by `+0x24` stride, selects zone by span range, then switches on `light_mode`:
- **Mode 0**: Direct lighting -- sets ambient + diffuse + direction in one shot
- **Mode 1**: Blended/oscillating -- interpolates between span endpoints, applies wave modulation using `wave_period` and `wave_amplitude_a/b`
- **Mode 2**: Extended blended -- similar to mode 1 with end-side interpolation

`UpdateActorTrackLightState` (0x40cdaa) reads `+0x1C` (projection_type) per zone.

### Example: Track 1 (pool index 1, pointer at 0x469C7C -> 0x4673D8)

Raw data at 0x4673D8 (first zone, 36 bytes):
```
00 00  B2 01  34 F6 00 08  CC 09 00 00
40 40 40 00  40 40 40 00  00 00 00 00
00 00 00 00  00 00 00 00
```
Decoded: start_span=0, end_span=434, direction=(0xF634,0x0800,0x09CC), ambient=RGB(64,64,64), diffuse=RGB(64,64,64), mode=0, projection=0

---

## 6. Per-Track Checkpoint Pointer Table (0x46CF6C)

```c
CheckpointData* gCheckpointPtrTable[20];  // at 0x46CF6C, indexed by track pool ID
```

Each pointer leads to a variable-size `CHECKPT.NUM` structure loaded from the level ZIP:

```c
struct CheckpointData {
    /* +0x00 */ uint8_t  checkpoint_count;  // Number of checkpoint stages (1-7)
    /* +0x01 */ uint8_t  padding;
    /* +0x02 */ uint16_t initial_time;      // Initial timer value (difficulty-scaled)
    /* +0x04 */ struct {
                    uint16_t span_threshold; // Span index the actor must reach
                    uint16_t time_bonus;     // Timer added on reaching checkpoint
                } checkpoints[];            // checkpoint_count entries
};
// Total size: 4 + checkpoint_count * 4 bytes
// Copied to DAT_004aed98 as 24 bytes (6 dwords) by LoadTrackRuntimeData
```

### Difficulty Scaling (AdjustCheckpointTimersByDifficulty, 0x40a530)

| Difficulty | initial_time multiplier | time_bonus multiplier |
|-----------|------------------------|----------------------|
| Easy (0)  | 12/10 = 1.2x          | 12/10 = 1.2x        |
| Normal (1)| 11/10 = 1.1x          | 11/10 = 1.1x        |
| Hard (2)  | 1.0x (unchanged)       | 1.0x (unchanged)     |

### Example: Track Pool 1 (pointer at 0x46CF70 -> 0x46CBB0)

```
Byte dump: 05 00 3B 64 65 03 00 3C E7 05 00 2D 0D 08 00 3C 3A 0A 00 28 02 0C 00 00
```

| Field | Value | Notes |
|-------|-------|-------|
| count | 5 | 5 checkpoint stages |
| pad | 0 | |
| initial_time | 0x003B = 59 | Wait-- see below |

Note: The uint16 LE reading of bytes [3B, 64] gives 0x643B = 25659. At 30 fps, that is ~855 seconds (~14 min). This is consistent with P2P race times at hard difficulty.

| Checkpoint | Span Threshold | Time Bonus |
|------------|---------------|------------|
| 0 | 0x0365 = 869 | 0x3C00 = 15360 |
| 1 | 0x05E7 = 1511 | 0x2D00 = 11520 |
| 2 | 0x080D = 2061 | 0x3C00 = 15360 |
| 3 | 0x0A3A = 2618 | 0x2800 = 10240 |
| 4 | 0x0C02 = 3074 | 0x0000 = 0 (finish) |

Consumer: `CheckRaceCompletionState` (0x40a047) checks if actor's current span >= span_threshold to advance to next checkpoint and add time_bonus.

---

## 7. LEVELINF.DAT sky_animation_index (+0x54)

**Status: VESTIGIAL / UNUSED in PC version.**

After exhaustive search of all 6 READ xrefs to `gTrackEnvironmentConfig` (0x4AEE20):
- `InitializeRaceSmokeSpritePool` reads +0x04 (smoke_enable)
- `CheckRaceCompletionState` reads +0x08 (checkpoint_count)
- `InitializeRaceSession` reads +0x00 (track_type), +0x0C (traffic), +0x17*4=+0x5C (fog), +0x60-62 (fog color)
- `InitializeWeatherOverlayParticles` reads +0x28 (weather_type)
- `UpdateAmbientParticleDensityForSegment` reads +0x2C (density_pair_count), +0x36 (density pairs)
- `LoadTrackRuntimeData` writes the pointer

**No code reads offset +0x54.** The field contains 36 (0x24) for circuits and 0xFFFFFFFF for P2P tracks, but the PC version's sky rendering uses only `AdvanceGlobalSkyRotation` (fixed 0x400 increment per tick) and `gSkyMeshResource` (loaded from SKY.PRR). The sky_animation_index was likely used by the PlayStation version's sky system and was never ported to the PC renderer.

---

## 8. static.hed File Format

Loaded from `static.zip` by `LoadStaticTrackTextureHeader` (0x442520) and `LoadTrackTextureSet` (0x4426c5).

### File Layout

```c
struct StaticHedFile {
    /* +0x00 */ uint32_t texture_page_count;     // Number of texture pages
    /* +0x04 */ uint32_t named_entry_count;       // Number of named entries
    /* +0x08 */ StaticHedEntry entries[];          // named_entry_count entries, 0x40 bytes each
    /* +0x08 + count*0x40 */ TexturePageMeta pages[]; // texture_page_count entries, 0x10 bytes each
};
```

### Named Entry (0x40 bytes / 64 bytes)

```c
struct StaticHedEntry {        // 64 bytes
    /* +0x00 */ char name[16];  // Zero-terminated asset name (e.g., "SUN.TGA", "KTRE1.TGA", "wheels")
    /* +0x10 */ char pad1[16];  // Padding/reserved
    /* +0x20 */ uint32_t width_a;   // Texture width variant A
    /* +0x24 */ uint32_t height_a;  // Texture height variant A
    /* +0x28 */ uint32_t width_b;   // Texture width variant B
    /* +0x2C */ uint32_t height_b;  // Texture height variant B
    /* +0x30 */ uint32_t reserved[3]; // Padding/unused
    /* +0x3C */ int32_t  page_index;  // Base page index (adjusted +0x400 at runtime by LoadTrackTextureSet)
};
```

### Texture Page Metadata (0x10 bytes / 16 bytes)

```c
struct TexturePageMeta {       // 16 bytes, per texture page
    /* +0x00 */ int32_t data_offset;   // Offset into texture data blob (or page ID)
    /* +0x04 */ int32_t has_data;      // Non-zero = texture page has pixel data
    /* +0x08 */ int32_t width;         // Texture page width in pixels
    /* +0x0C */ int32_t height;        // Texture page height in pixels
};
```

Consumer: `LoadStaticTrackTextureHeader` builds `Texture_exref` entries (+0x24=width, +0x28=height per 0x30-byte slot). `LoadRaceTexturePages` (0x442770) iterates texture pages, loads from `tpage%d.dat`, optionally resamples, and uploads to GPU.

Named entry lookup in `LoadRaceTexturePages` uses `stricmp_game()` to match names like "SKY", "CAR_%d", "wheels" against the entry array.

---

## 9. Translucent Pipeline Greyscale-to-D3DCOLOR LUT (0x4AEE68)

```c
uint32_t gGreyscaleToD3DColorLUT[256];  // at 0x4AEE68, 1024 bytes
// Initialized by InitializeTranslucentPrimitivePipeline (0x4312d0)
// Entry[i] = (i * 0x10101) - 0x01000000
//          = 0xFF000000 | (i << 16) | (i << 8) | i    (for i=0..255)
// Result: ARGB greyscale ramp with alpha=0xFF-(i>>8 borrow)
```

| Index | Value (hex)  | Color |
|-------|-------------|-------|
| 0     | 0xFF000000  | Black (alpha=FF) |
| 64    | 0xFF404040  | Dark grey |
| 128   | 0xFF808080  | Mid grey |
| 192   | 0xFFC0C0C0  | Light grey |
| 255   | 0xFEFFFFFF  | Near-white (alpha=FE due to overflow) |

Consumers: `FlushQueuedTranslucentPrimitives` (0x431340), `RenderPreparedMeshResource` (0x431490), `SubmitImmediateTranslucentPrimitive` (0x4315e0), `FlushImmediateDrawPrimitiveBatch` (0x432970), `AppendClippedPolygonTriangleFan` (0x432b00). Used as a fast vertex-intensity-to-packed-ARGB lookup for all translucent/alpha-blended geometry.

---

## 10. YCbCr-to-RGB16 Conversion LUTs (0x4D3930 / 0x4D3B30 / 0x4D3D30)

All three tables are **runtime-computed** by `BuildYCbCrToRGB16LUT` (0x453500). Static binary contains all zeros.

```c
uint32_t gYLumaLUT[128];   // 0x4D3930 - 0x4D3B30 (512 bytes)
uint32_t gCrLUT[128];      // 0x4D3B30 - 0x4D3D30 (512 bytes)
uint32_t gCbLUT[128];      // 0x4D3D30 - 0x4D3F30 (512 bytes)
```

### Build Algorithm

```c
void BuildYCbCrToRGB16LUT(pixel_format_params...) {
    // Y luma: pure greyscale at 128 levels
    for (i = 0; i < 128; i++) {
        val = i * 2 + 1 + 128;  // range 129..385 (clamped)
        gYLumaLUT[i] = PackToRGB16(val, val, val);
    }

    // Cb: blue-difference channel
    for (i = 0; i < 128; i++) {
        signed_val = (signed char)(i * 2 + 1);
        r_delta = 0;
        g_delta = signed_val * -0x5816 / 65536;  // -0.3441 * Cb
        b_delta = signed_val * 0x1C5A1 / 65536;  // +1.7720 * Cb
        gCbLUT[i] = PackToRGB16(r_delta, g_delta, b_delta);
    }

    // Cr: red-difference channel
    for (i = 0; i < 128; i++) {
        signed_val = (signed char)(i * 2 + 1);
        r_delta = signed_val * 0x166E9 / 65536;   // +1.4020 * Cr
        g_delta = signed_val * -0xB6CF / 65536;    // -0.7141 * Cr
        b_delta = 0;
        gCrLUT[i] = PackToRGB16(r_delta, g_delta, b_delta);
    }
}
```

The packed format depends on the display mode (RGB555/RGB565) passed as parameters. Each LUT entry is a pre-shifted 16-bit color value that can be directly ORed together:
```c
uint16_t rgb16 = gYLumaLUT[Y] + gCrLUT[Cr] + gCbLUT[Cb];  // additive composition
```

Consumer: `YCbCrToPackedPixel16` (0x457400) and related video decoding functions in the EA TGQ multimedia engine. Used for FMV/cutscene playback.

---

## Summary of Span Type Lookup Table (DAT_00473C6C)

Also used by `ComputeTrackSpanProgress` for edge vertex lookup. Same structure as `DAT_004631A0` but with different semantic:

```c
// at 0x473C6C, stride 8 (2 ints per span type)
struct SpanTypeEdgeIndexOffset {
    int32_t forward_offset;   // vertex index delta for forward edge
    int32_t lateral_offset;   // vertex index delta for lateral edge
};
```

| Type | Forward | Lateral |
|------|---------|---------|
| 0 | 0 | 0 |
| 1 | 0 | 0 |
| 2 | 0 | -1 |
| 3 | -1 | 0 |
| 4 | -1 | 0 |
| 5 | -2 | 0 |
| 6 | 0 | 0 |
| 7 | -1 | 0 |
| 8 | -1 | 0 |
| 9 | -2 | 0 |
| 10 | 0 | 0 |
| 11 | 0 | 0 |
