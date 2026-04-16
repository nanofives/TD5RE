# 3D Asset Formats: MODELS.DAT, TEXTURES.DAT, tpage%d.dat, himodel.dat, SKY.PRR

Full binary format documentation for all 3D asset files in Test Drive 5, derived
from reverse engineering of TD5_d3d.exe and M2DX.dll.

---

## Table of Contents

1. [Architecture Overview](#architecture-overview)
2. [MODELS.DAT -- Track Model Container](#modelsdat)
3. [Mesh Resource Format (PRR)](#mesh-resource-format-prr)
4. [TEXTURES.DAT -- Track Texture Definitions](#texturesdat)
5. [static.hed -- Texture Page Directory](#statiched)
6. [tpage%d.dat -- Texture Page Pixel Data](#tpageddat)
7. [himodel.dat -- Vehicle Model File](#himodeldat)
8. [SKY.PRR -- Sky Dome Mesh](#skyprr)
9. [model%d.prr -- Traffic Vehicle Models](#modeldprr)
10. [Texture Skin Files (TGA)](#texture-skin-files)
11. [Rendering Pipeline](#rendering-pipeline)

---

## Architecture Overview

### Asset Archives

| Archive | Contents | Loader |
|---------|----------|--------|
| `level%03d.zip` | MODELS.DAT, TEXTURES.DAT, tpage%d.dat, STRIP.DAT, etc. | `InitializeRaceSession` (0x42AA10) |
| `static.zip` / `STATIC.ZIP` | SKY.PRR, static.hed, tpage%d.dat (shared textures) | `LoadTrackTextureSet` (0x442670) |
| `cars\*.zip` | himodel.dat, carparam.dat, CARSKIN%d.TGA, CARHUB%d.TGA | `LoadRaceVehicleAssets` (0x443280) |
| `traffic.zip` | model%d.prr, skin%d.tga | `LoadRaceVehicleAssets` |
| `environs.zip` | ENV%d (environment texture pages) | `LoadEnvironmentTexturePages` (0x42F990) |

### Load Sequence

```
InitializeRaceSession (0x42AA10)
  |
  +-- LoadTrackRuntimeData (0x42FB90)      --> STRIP.DAT, LEFT/RIGHT.TRK, etc.
  +-- BindTrackStripRuntimePointers        --> parse track geometry
  +-- ReadArchiveEntry("MODELS.DAT")       --> load raw model blob
  +-- LoadTrackTextureSet (0x442670)       --> TEXTURES.DAT + static.hed
  +-- ParseModelsDat (0x431190)            --> relocate & prepare all mesh resources
  +-- ReadArchiveEntry("SKY.PRR")          --> sky dome mesh
  +-- PrepareMeshResource(sky)             --> fix up sky mesh
  +-- LoadRaceVehicleAssets (0x443280)     --> himodel.dat + car textures
  +-- LoadRaceTexturePages (0x442770)      --> tpage%d.dat + car skins + sky TGA
```

---

## MODELS.DAT -- Track Model Container {#modelsdat}

**Source:** `level%03d.zip`
**Loader:** `InitializeRaceSession` -> `ParseModelsDat` (VA 0x431190)
**Alignment:** Buffer allocated 32-byte aligned (`(ptr + 0x1F) & ~0x1F`)

### File Structure

```
Offset  Size        Type        Description
------  ----        ----        -----------
0x00    4           uint32      entry_count      Number of model groups
0x04    N*8         array       entry_table[N]   Per-group entry pairs
0x04+N*8  ...       blob        Sub-mesh data blocks (PRR format)
```

### Entry Table (8 bytes per entry)

Each entry in the table is a pair of uint32 values:

```
Offset  Size  Type    Description
0x00    4     uint32  block_offset   Relative offset from file start to sub-mesh block
0x04    4     uint32  reserved       Second dword (no confirmed runtime consumer)
```

### Sub-Mesh Block

Each `block_offset` points to a sub-mesh block containing:

```
Offset  Size  Type    Description
0x00    4     uint32  sub_mesh_count    Number of individual mesh resources in this block
0x04    N*4   uint32  mesh_offsets[N]   Relative offsets from block start to each PRR mesh
```

### ParseModelsDat Relocation (VA 0x431190)

At load time, `ParseModelsDat` performs in-place relocation:

1. For each entry in `entry_table`:
   - `entry_table[i*2] += (uint)file_base_ptr` -- converts relative offset to absolute pointer
2. For each sub-mesh block:
   - Each `mesh_offsets[j] += (uint)block_base_ptr` -- makes absolute
   - Calls `PrepareMeshResource(mesh_ptr)` on each resulting mesh
3. Applies per-track brightness adjustment (`param_2`) to vertex lighting at +0x18 in each vertex

### Runtime Globals

| Global | VA | Description |
|--------|----|-------------|
| `gModelsDatEntryCount` | varies | Number of model groups |
| `gModelsDatEntryTable` | varies | Pointer to relocated entry pair array |
| `gTrackSpanRingLength` | varies | Total span count (return value of ParseModelsDat) |
| `DAT_004aee54` | 0x4AEE54 | Pointer past end of entry table (start of mesh data) |

---

## Mesh Resource Format (PRR) {#mesh-resource-format-prr}

The PRR format is the unified mesh resource used for ALL 3D objects: track models
(from MODELS.DAT), vehicles (himodel.dat), sky dome (SKY.PRR), and traffic
(model%d.prr). The format name comes from the `.prr` file extension.

### Mesh Resource Header (at least 0x38 bytes)

```
Offset  Size  Type    Field              Description
------  ----  ----    -----              -----------
+0x00   2     int16   render_type        Primitive dispatch index (0-6, indexes into
                                         PTR_EmitTranslucentTriangleStrip_00473b9c)
+0x02   2     int16   texture_page_id    Texture slot index for this mesh batch
+0x04   4     int32   command_count      Number of primitive commands (sub-batches)
+0x08   4     int32   total_vertex_count Total vertices across all commands
+0x0C   4     float   bounding_radius    Bounding sphere radius (world units)
+0x10   4     float   bounding_center_x  Bounding sphere center X (world-space, fixed-point)
+0x14   4     float   bounding_center_y  Bounding sphere center Y
+0x18   4     float   bounding_center_z  Bounding sphere center Z
+0x1C   4     float   origin_x           Model origin X (world-space, fixed-point * 1/256)
+0x20   4     float   origin_y           Model origin Y
+0x24   4     float   origin_z           Model origin Z
+0x28   4     ---     (reserved/padding)
+0x2C   4     uint32  commands_offset    Offset from mesh base to command array (pre-relocation)
+0x30   4     uint32  vertices_offset    Offset from mesh base to vertex array (pre-relocation)
+0x34   4     uint32  normals_offset     Offset from mesh base to face normal array (0 = none)
```

**World-space coordinates** use fixed-point encoding: multiply by `DAT_004749d0` =
0.00390625 (1/256) to convert to floating-point world units.

### Primitive Command Array

The command array starts at `mesh_base + commands_offset`. After relocation,
`commands_offset` becomes an absolute pointer. Each command is **16 bytes (0x10)**:

```
Offset  Size  Type     Field              Description
------  ----  ----     -----              -----------
+0x00   2     int16    dispatch_type      Selects renderer function (0-6)
+0x02   2     int16    texture_page_id    Texture slot for this batch (overrides header)
+0x04   4     int32    reserved / param
+0x08   2     uint16   triangle_count     Number of triangle primitives (3 verts each)
+0x0A   2     uint16   quad_count         Number of quad primitives (4 verts each)
+0x0C   4     int32    vertex_data_ptr    Pointer to first vertex for this command
                                          (set during PrepareMeshResource)
```

### Vertex Format (0x2C = 44 bytes per vertex, stride 11 floats)

Each vertex in the vertex array is 44 bytes:

```
Offset  Size  Type    Field       Description
------  ----  ----    -----       -----------
+0x00   4     float   pos_x      Model-space X position
+0x04   4     float   pos_y      Model-space Y position
+0x08   4     float   pos_z      Model-space Z position
+0x0C   4     float   view_x     View-space X (written by TransformMeshVerticesToView)
+0x10   4     float   view_y     View-space Y (written at runtime)
+0x14   4     float   view_z     View-space Z (written at runtime)
+0x18   4     uint32  lighting   Vertex lighting intensity (0x00-0xFF, byte in low 8 bits)
                                 Computed by ComputeMeshVertexLighting; clamped [0x40..0xFF]
+0x1C   4     float   tex_u      Texture U coordinate (0.0 - 1.0)
+0x20   4     float   tex_v      Texture V coordinate (0.0 - 1.0)
+0x24   4     float   proj_u     Secondary/projection U (used by ApplyMeshProjectionEffect)
+0x28   4     float   proj_v     Secondary/projection V
```

**UV Clamping (PrepareMeshResource):**
- U and V are clamped to [0.0, 1.0] then scaled: `u = u * 0.984375 + 0.0078125`
- This provides a half-texel inset to prevent bleeding at texture edges (0.984375 = 63/64, 0.0078125 = 0.5/64 for a 64x64 texture)

### Face Normal Array (optional, at normals_offset)

Present when `normals_offset != 0`. Contains per-face normal data used for
backface culling. Two record sizes depending on primitive type:

**Triangle normal (0x30 = 48 bytes):**
```
Offset  Size  Type    Field
+0x00   4     float   normal_0_x    Vertex 0 normal X
+0x04   4     float   normal_0_y    Vertex 0 normal Y
+0x08   4     float   normal_0_z    Vertex 0 normal Z
+0x0C   4     int32   visible_flag  Backface cull flag: 0=hidden, 1=visible
                                    (set by PrepareMeshResource if all Y normals > 0.03662)
+0x10   ...          (vertex 1 normal, 12 bytes)
+0x20   ...          (vertex 2 normal, 12 bytes)
```

**Quad normal (0x40 = 64 bytes):**
Same layout but with a fourth vertex normal block.

### Primitive Dispatch Table (VA 0x473B9C)

The `dispatch_type` field indexes into a 7-entry function pointer table:

| Index | Function | VA | Description |
|-------|----------|----|-------------|
| 0 | EmitTranslucentTriangleStrip | 0x431750 | Standard triangle strip (tri + quad batches) |
| 1 | EmitTranslucentTriangleStrip | 0x431750 | Same as 0 |
| 2 | SubmitProjectedTrianglePrimitive | 0x4316F0 | Projected/HUD triangle |
| 3 | SubmitProjectedQuadPrimitive | 0x431690 | Projected/HUD quad |
| 4 | InsertBillboardIntoDepthSortBuckets | 0x43E3B0 | Billboard sprite |
| 5 | EmitTranslucentTriangleStripDirect | 0x431730 | Direct triangle strip (pre-transformed) |
| 6 | EmitTranslucentQuadDirect | 0x4316D0 | Direct quad (pre-transformed) |

### Rendering Flow for a Track Model

```
RenderTrackSpanDisplayList (0x431270)
  for each mesh in display list:
    IsBoundingSphereVisibleInCurrentFrustum(mesh)   -- frustum cull
    ApplyMeshRenderBasisFromWorldPosition(mesh)     -- set world transform
    TransformAndQueueTranslucentMesh(mesh)          -- transform verts + queue for sort
      TransformMeshVerticesToView(mesh)             -- 3x4 matrix multiply per vertex
      QueueTranslucentPrimitiveBatch(command)       -- insert into 4096-bucket depth sort

FlushQueuedTranslucentPrimitives (0x431340)         -- sorted render
  for each bucket (back to front):
    dispatch_table[command.dispatch_type](command)  -- emit primitives
    FlushImmediateDrawPrimitiveBatch()              -- D3D DrawPrimitive

FlushImmediateDrawPrimitiveBatch (0x4329E0)
  BindRaceTexturePage(texture_slot)                 -- bind D3D texture
  IDirect3DDevice2::DrawPrimitive(
    D3DPT_TRIANGLELIST,     -- primitive type = 4
    D3DFVF = 0x1C4,         -- D3DFVF_XYZRHW|D3DFVF_DIFFUSE|D3DFVF_SPECULAR|D3DFVF_TEX1
    vertex_buffer,
    vertex_count,
    index_buffer,
    index_count,
    flags = 0x0C            -- D3DDP_DONOTCLIP|D3DDP_DONOTUPDATEEXTENTS
  )
```

### D3D Submitted Vertex Format (0x20 = 32 bytes, runtime)

After transformation, vertices are written to the draw buffer as D3D FVF 0x1C4:

```
Offset  Size  Type      Field
+0x00   4     float     screen_x       Screen-space X
+0x04   4     float     screen_y       Screen-space Y
+0x08   4     float     rhw            Reciprocal homogeneous W (1/Z for perspective)
+0x0C   4     float     (z or padding)
+0x10   4     D3DCOLOR  diffuse        ARGB diffuse color (lighting mapped to color LUT)
+0x14   4     D3DCOLOR  specular       ARGB specular (fog)
+0x18   4     float     tu             Texture U
+0x1C   4     float     tv             Texture V
```

The lighting byte at vertex +0x18 is mapped through `DAT_004aee68[]` (a 256-entry
color lookup table) to produce the final D3D diffuse color.

---

## TEXTURES.DAT -- Track Texture Definitions {#texturesdat}

**Source:** `level%03d.zip`
**Loader:** `LoadTrackTextureSet` (VA 0x442670) -> `BuildTrackTextureCacheImpl` (VA 0x40B1D0)

### File Structure

TEXTURES.DAT is a concatenation of per-texture records. The file is loaded as a raw
blob, then iterated using pointers from `static.hed`. Each texture entry has this
layout within the blob:

```
Offset  Size        Type    Description
------  ----        ----    -----------
+0x00   3           bytes   (header padding / alignment)
+0x03   1           byte    format_type     Texture format/transparency mode
+0x04   4           int32   palette_count   Number of palette entries (typically 256)
+0x08   N*3         bytes   palette[N]      RGB palette (3 bytes per entry: R, G, B)
+0x08+N*3  4096     bytes   pixel_data      Palette indices (64x64 = 4096 pixels)
```

### Format Types (byte at +0x03)

| Value | Name | Alpha | Description |
|-------|------|-------|-------------|
| 0 | Opaque | No alpha byte | Standard opaque texture (ABGR with A=0) |
| 1 | Key-Transparent | Palette index 0 = transparent | Color-keyed: index 0 -> ABGR(0,0,0,0), others -> ABGR(0xFF,...) |
| 2 | Semi-Transparent | A=0x80 fixed | All pixels get alpha=0x80 (50% translucent) |
| 3 | Opaque-Alt | Same as 0 | Alternate opaque mode (treated as type 0 at runtime) |

### Palette Expansion to ARGB32

`BuildTrackTextureCacheImpl` expands each 8-bit indexed pixel into a 4-byte ARGB32
value in a scratch buffer, depending on `format_type`:

```c
// Type 0 / 3 (opaque):
output[0] = 0x00;                    // Alpha = 0 (fully opaque in D3D convention)
output[1] = palette[index * 3 + 2];  // R
output[2] = palette[index * 3 + 1];  // G
output[3] = palette[index * 3 + 0];  // B

// Type 1 (color-keyed):
if (index == 0) { output = {0, 0, 0, 0}; }  // Transparent
else { output[0] = 0xFF; output[1..3] = palette[index]; }

// Type 2 (semi-transparent):
output[0] = 0x80;                    // Alpha = 128
output[1..3] = palette[index];       // RGB
```

### Texture Dimensions

All track textures in TEXTURES.DAT are **64x64 pixels** (4096 bytes of pixel data).
This is confirmed by the M2DX.dll Manage function which sets `DAT_100295b2 = 0x40`
and `DAT_100295b4 = 0x40` (64x64) and `_DAT_100295ac = 0x1000` (4096 pixel count).

### Mipmap Chain

After palette expansion, `ParseAndDecodeCompressedTrackData` (VA 0x430D30) generates
a mipmap chain. It iterates from the source dimension down to smaller sizes (powers
of 2), averaging pixels and packing them into the target surface format using the
current D3D pixel format masks (obtained via `DXD3DTexture::GetMask`). Output can be
either 16-bit or 32-bit depending on hardware support.

### Texture Streaming

Track textures use a streaming cache system. Not all textures fit in VRAM simultaneously:

- **Cache capacity:** Up to 600 texture slots managed by `DAT_0048dc40`
- **`AdvanceTextureStreamingScheduler` (0x40B830):** LRU eviction policy
- **`PreloadLevelTexturePages` (0x40BAE0):** Attempts to preload all pages on init
- **`BindRaceTexturePage` (0x40B660):** On-demand load and D3D texture bind

---

## static.hed -- Texture Page Directory {#statiched}

**Source:** `static.zip`
**Loader:** `LoadTrackTextureSet` (VA 0x442670) / `LoadStaticTrackTextureHeader` (VA 0x442560)

### File Structure

```
Offset      Size        Type        Description
------      ----        ----        -----------
+0x00       4           int32       texture_page_count    Total tpage slots (including 4 reserved)
+0x04       4           int32       named_entry_count     Number of named texture entries
+0x08       N*0x40      array       named_entries[N]      Named texture entry descriptors
+0x08+N*64  M*0x10      array       page_metadata[M]      Per-page dimension/format records
```

The runtime subtracts 4 from `texture_page_count` for level textures (the last 4
are reserved for car skin composites).

### Named Entry Descriptor (0x40 = 64 bytes each)

```
Offset  Size  Type      Field           Description
------  ----  ----      -----           -----------
+0x00   60    char[60]  name            Null-terminated ASCII name (e.g., "CAR_0", "wheels", "SKY")
+0x3C   4     int32     texture_slot    Texture page slot index (biased: subtract 0x400 for
                                        non-streamed texture page index)
```

Known named entries:
- `"CAR_0"` through `"CAR_2"` -- car skin composite texture slots
- `"wheels"` -- wheel hub texture slot
- `"SKY"` -- sky texture slot
- `"TRAF%d"` -- traffic vehicle skin slots
- `"ENV%d"` -- environment map texture slots

The `texture_slot - 0x400` gives the physical texture page index used by
`UploadRaceTexturePage` and `BindRaceTexturePage`.

### Per-Page Metadata (0x10 = 16 bytes each)

```
Offset  Size  Type    Field              Description
------  ----  ----    -----              -----------
+0x00   4     int32   transparency_flag  0=opaque (3 bytes/pixel), nonzero=transparent (4 bytes/pixel)
+0x04   4     int32   image_type         Texture upload mode (0=LoadRGBS24, 1=LoadRGBS32/7, 2=LoadRGBS32/8)
+0x08   4     int32   source_dimension   Source texture width/height (square, typically 256)
+0x0C   4     int32   target_dimension   Target texture width/height after downscale
```

### Dimension Selection

`LoadStaticTrackTextureHeader` selects between source and target dimensions based on
`DAT_004c3d04` (texture quality flag):
- **Quality mode 0 (default):** Uses the smaller of source/target dimensions (downsample for VRAM savings)
- **Quality mode != 0:** Uses source dimension at source_dimension, target at target_dimension directly

---

## tpage%d.dat -- Texture Page Pixel Data {#tpageddat}

**Source:** `static.zip` (shared) and `level%03d.zip` (level-specific)
**Loader:** `LoadRaceTexturePages` (VA 0x442770)
**Filename:** `tpage%d.dat` where `%d` is the page index (0, 1, 2, ...)

### Format

Each tpage file contains **raw uncompressed pixel data** for one texture page.
The data format and dimensions are defined by the corresponding entry in the
`static.hed` per-page metadata array.

```
File size = source_dimension * source_dimension * bytes_per_pixel

Where:
  bytes_per_pixel = 3  (RGB24)   if transparency_flag == 0
  bytes_per_pixel = 4  (RGBA32)  if transparency_flag != 0
```

**Pixel layout:** Packed scanlines, top-to-bottom, left-to-right.

- **RGB24 pages (3 bpp):** `R G B R G B ...` (red first)
- **RGBA32 pages (4 bpp):** `A R G B A R G B ...` (alpha first)

### Load Path

```
for each page_index in [0 .. texture_page_count):
    if page_metadata[page_index].image_type != 0:
        read tpage%d.dat from static.zip into 512KB buffer
        ResampleTexturePageToEntryDimensions(buffer, page_index)
        UploadRaceTexturePage(buffer, page_index, image_type, is_car_page)
```

### Resampling

`ResampleTexturePageToEntryDimensions` (VA 0x442D30) downsamples or upsamples the
raw pixel data in-place when `source_dimension != target_dimension`. It uses
nearest-neighbor sampling with integer ratio stepping.

### Upload Path

`UploadRaceTexturePage` (VA 0x40B590) routes to M2DX.dll:

| image_type | M2DX Function | Input Format |
|------------|---------------|--------------|
| 0 | `DXD3DTexture::LoadRGBS24` | RGB24 (3 bpp, square, dimension from metadata) |
| 1 | `DXD3DTexture::LoadRGBS32(IMAGETYPE=7)` | RGBA32 (4 bpp) |
| 2 | `DXD3DTexture::LoadRGBS32(IMAGETYPE=8)` | RGBA32 (4 bpp) |

For `image_type == 0` (car composite pages), the function pre-processes the RGBA32
data to generate alpha from pixel color: `alpha = (R+G+B == 0) ? 0x00 : 0x80`.

### Typical Page Count per Level

The number of tpage files varies per track (stored in `static.hed` header as
`texture_page_count - 4`). Typical values range from 4 to 12 pages.

---

## himodel.dat -- Vehicle Model File {#himodeldat}

**Source:** `cars\*.zip` (e.g., `cars\cam.zip` for Camaro)
**Loader:** `LoadRaceVehicleAssets` (VA 0x443280)
**Alignment:** Buffer allocated 32-byte aligned

### File Format

`himodel.dat` is a **single PRR mesh resource** (same binary format as described
in the [Mesh Resource Format](#mesh-resource-format-prr) section). It is NOT a
container like MODELS.DAT -- it is one flat mesh resource blob.

### Load Sequence

```
for each racer slot (0..5, or 0..1 in time trial):
    Read himodel.dat from cars\{car}.zip into pre-computed buffer
    Read carparam.dat (0x10C bytes) for tuning + physics tables

    // UV patching for player/opponent texture split:
    vertex_count = triangle_count * 3 + quad_count * 4
    for each vertex:
        vertex.tex_u = vertex.tex_u * 0.5 + (slot & 1) * 0.5
    // This maps even slots to left half (U: 0.0-0.5)
    // and odd slots to right half (U: 0.5-1.0) of car skin composite texture

    // Assign texture page from static.hed "chassis" entry
    command.texture_page_id = (slot / 2) + chassis_entry.texture_slot

    PatchModelUVCoordsForTrackLighting(model)   // Fix secondary UVs for track lighting
    PrepareMeshResource(model)                  // Standard PRR relocation
```

### Key Fields in himodel.dat

All standard PRR header fields apply. Additional usage:

| Offset | Field | Vehicle-Specific Use |
|--------|-------|---------------------|
| +0x2C | commands_offset | Points to sub-batch command array |
| +0x30 | vertices_offset | Points to vertex data |
| +0x04 | command_count | Sub-batch count (body panels, windows, etc.) |
| +0x08 | total_vertex_count | Total vertices in model |
| +0x1C..+0x24 | origin | Vehicle center of mass (fixed-point * 1/256) |

### "chassis" Named Entry

`FindArchiveEntryByName` locates the "chassis" entry in `static.hed`. Fields at
offsets +0x2C through +0x38 of this entry provide UV transformation parameters:

```
chassis_entry + 0x2C -> scale factor (float) * DAT_004749d0 = UV_param_A
chassis_entry + 0x30 -> scale factor (float) * DAT_004749d0 = UV_param_B
chassis_entry + 0x34 -> scale factor (float) * DAT_004749d0 = UV_param_C
chassis_entry + 0x38 -> scale factor (float) * DAT_004749d0 = UV_param_D
```

These are used by `PatchModelUVCoordsForTrackLighting` (VA 0x443730) to adjust
the secondary UV channel (proj_u, proj_v at vertex offsets +0x24, +0x28) for
environment/lighting texture mapping.

### Damage State Model

When `DAT_004c3d44 == 2`, an additional copy of himodel.dat is loaded for the
damage state. This copy gets UV coordinates scaled to `U * 0.5` (left half only)
and a fixed texture page assignment of `0x0404`.

---

## SKY.PRR -- Sky Dome Mesh {#skyprr}

**Source:** `STATIC.ZIP` (uppercase) / `static.zip` (lowercase)
**Loader:** `InitializeRaceSession`

### Format

Standard PRR mesh resource (same format as track models). After loading:

1. `PrepareMeshResource(sky_mesh)` -- standard relocation and UV clamp
2. `sky_mesh.commands[0].texture_page_id = 0x0403` -- hardcoded sky texture slot

### Runtime

- `AdvanceGlobalSkyRotation` (0x43D7C0) animates sky dome rotation per frame
- Sky dome is rendered first in the draw order (before track geometry)
- Uses the FORWSKY.TGA / BACKSKY.TGA texture loaded from the level zip

---

## model%d.prr -- Traffic Vehicle Models {#modeldprr}

**Source:** `traffic.zip`
**Filename:** `model%d.prr` where `%d` is the traffic model index
**Loader:** `LoadRaceVehicleAssets`

### Format

Standard PRR mesh resource format, identical to himodel.dat. Up to 6 traffic
model variants are loaded. Each model:

1. Has its texture page assigned from the corresponding `TRAF%d` named entry in `static.hed`
2. Goes through `PatchModelUVCoordsForTrackLighting` and `PrepareMeshResource`
3. Skin texture loaded from `skin%d.tga` in `traffic.zip`

---

## Texture Skin Files (TGA) {#texture-skin-files}

Several texture assets use TGA format (decoded by `DecodeArchiveImageToRgb24` at
VA 0x442E00).

### Supported TGA Variants

`DecodeArchiveImageToRgb24` handles these TGA image types:

| TGA Type | Description | Notes |
|----------|-------------|-------|
| 1 | Color-mapped (palette indexed) | 1 byte/pixel, 3 bytes/palette entry |
| 2 | Uncompressed true-color | Supports 16bpp, 24bpp, 32bpp |
| 9 | RLE color-mapped | Run-length encoded palette indexed |
| 10 | RLE true-color | RLE encoded, 24bpp supported |

### TGA Header (standard 18-byte TGA header)

```
Offset  Size  Field
+0x00   1     id_length
+0x01   1     color_map_type     (0=no palette, 1=has palette)
+0x02   1     image_type         (1, 2, 9, 10)
+0x03   2     color_map_origin
+0x05   2     color_map_length   Number of palette entries
+0x07   1     color_map_depth    Bits per palette entry (24)
+0x08   2     x_origin
+0x0A   2     y_origin
+0x0C   2     width
+0x0E   2     height
+0x10   1     pixel_depth        Bits per pixel (16, 24, 32)
+0x11   1     image_descriptor   Bit 5: top-to-bottom. Bit 4: right-to-left
```

Output is always packed **RGB24** (3 bytes/pixel: R, G, B order, red first).
The decoder handles vertical flip (if bit 5 of descriptor is 0) and horizontal
flip (if bit 4 is set).

### Skin Files Loaded

| File Pattern | Archive | Purpose |
|-------------|---------|---------|
| `CARSKIN%d.TGA` | `cars\*.zip` | Car body skin (256x256, composite into 512x256 for 2 cars) |
| `CARHUB%d.TGA` | `cars\*.zip` | Wheel hub texture (64x64, composited into grid) |
| `FORWSKY.TGA` | `level%03d.zip` | Forward sky panorama |
| `BACKSKY.TGA` | `level%03d.zip` | Reverse sky panorama |
| `skin%d.tga` | `traffic.zip` | Traffic vehicle skins |

### Car Skin Compositing

Two car skins are composited side-by-side into a 512x256 texture page:

```
+--256px--+--256px--+
|  Car A  |  Car B  |  256px high
| (even)  |  (odd)  |
+---------+---------+

Compositing copies 32-pixel-wide strips from each 256x256 TGA into
alternating columns of the composite buffer:
  Strip width: 32 pixels (0x20), row count: 256 (0x100)
  Car A at column offset 0, stride 0x18C bytes between rows
  Car B at column offset 0x180, stride 0x18C bytes between rows
```

### Wheel Hub Compositing

Up to 6 wheel hub TGAs (one per racer slot) are composited into a grid layout
on the wheel texture page. Each hub is 64x64 pixels, arranged in a grid.
Hub pixels are converted from RGB24 to RGBA32 during compositing:
- If `R == 0 && G == 0 && B == 0`: alpha = 0x00 (transparent)
- Otherwise: alpha = 0xFF (opaque)

---

## Rendering Pipeline {#rendering-pipeline}

### Per-Frame Track Rendering

```
Race Frame (RunRaceFrame 0x42B580):
  BeginScene()

  1. Render sky dome
     AdvanceGlobalSkyRotation()
     ApplyMeshResourceRenderTransform(sky)
     RenderPreparedMeshResource(sky) or TransformAndQueueTranslucentMesh(sky)

  2. Render track segments
     RenderTrackSegmentBatch() / RenderTrackSegmentBatchVariant()
     RenderTrackSegmentNearActor()
     -- These fill the translucent primitive sort buckets

  3. Render track models (MODELS.DAT)
     RenderTrackSpanDisplayList()
       for each visible model group near camera:
         IsBoundingSphereVisibleInCurrentFrustum(mesh)
         ApplyMeshRenderBasisFromWorldPosition(mesh)
         TransformAndQueueTranslucentMesh(mesh)

  4. Render vehicles
     for each active racer slot:
       ApplyMeshRenderBasisFromTransform(rotation_matrix, actor)
       TransformMeshVerticesToView(model)
       ComputeMeshVertexLighting(model)
       [optional] ApplyMeshProjectionEffect(model, effect_slot)
       RenderPreparedMeshResource(model)

  5. Render particles, billboards
     UpdateRaceParticleEffects()

  6. Flush translucent primitives (back-to-front)
     FlushQueuedTranslucentPrimitives() -- 510 max batches, linked-list bucket sort

  7. Flush projected primitives (HUD overlays)
     FlushProjectedPrimitiveBuckets() -- 4096-bucket depth sort

  EndScene()
  Flip()
```

### Vehicle Render Detail: RenderRaceActorForView (0x40C120)

The per-actor vehicle render function performs sub-tick interpolation and a multi-stage
pipeline before submitting geometry:

```
RenderRaceActorForView(actor_index, view_index):
  // 1. Sub-tick interpolation for smooth rendering between physics ticks
  world_pos += ROUND(velocity * g_subTickFraction)     // g_subTickFraction at 0x4AAF60
  world_pos.y -= terrain_height << 8

  // 2. Rotation matrix: either from actor transform or built from Euler angles
  if (actor.is_scripted): use actor+0x120 matrix directly
  else: BuildRotationMatrixFromAngles() with interpolated angles

  // 3. Frustum cull: skip if actor too far in span distance from camera
  span_delta = abs(camera_span - actor_span)
  if span_delta >= gRaceTrackSpanCullWindow: SKIP (not visible)

  // 4. Lighting + transform setup
  ApplyTrackLightingForVehicleSegment(actor)       -- per-track-segment light params
  ApplyMeshRenderBasisFromTransform(rotation, mesh) -- 3x4 model→world→view matrix

  // 5. Frustum test
  TestMeshAgainstViewFrustum(mesh) -- 5-plane test (near/far + 4 angled)

  // 6. Core render (if visible)
  UpdateActiveTrackLightDirections()
  UpdateActorTrackLightState(actor)
  TransformMeshVerticesToView(mesh)                -- 3x4 matrix multiply per vertex
  ComputeMeshVertexLighting(mesh)                  -- 3-directional lighting model
  RenderPreparedMeshResource(mesh)                 -- dispatch to D3D DrawPrimitive

  // 7. Damage model overlay (player 0 only, when DAT_004c3d44 == 2)
  if has_damage_model:
    TransformMeshVerticesToView(damage_mesh)
    ApplyMeshProjectionEffect(damage_mesh, 0)
    RenderPreparedMeshResource(damage_mesh)

  // 8. Post-effects chain
  UpdateWantedDamageIndicator(actor)               -- damage bar projection (cop mode)
  RenderTrackedActorMarker()                       -- wanted suspect marker
  RenderVehicleTaillightQuads(actor)               -- brake light quads
  NoOpHookStub()                                   -- unused hook point
  SpawnRandomVehicleSmokePuff(actor)               -- engine smoke particles
  RenderVehicleWheelBillboards(actor)              -- camera-facing wheel sprites
  SpawnRearWheelSmokeEffects(actor)                -- tire smoke on hard cornering
```

### Frustum Culling

Two frustum test functions exist, serving different mesh types:

**`TestMeshAgainstViewFrustum` (0x42DE10)** — for vehicle meshes (reads origin from +0x1C/20/24):
- Transforms bounding sphere center into camera space
- Tests against 5 planes: near clip, far clip, left/right (horizontal FOV), top/bottom (vertical FOV)
- Horizontal half-angle tangent: `_DAT_004ab0b0` / depth scale `_DAT_004ab0b8`
- Vertical half-angle tangent: `_DAT_004ab0a4` / depth scale `_DAT_004ab0a8`
- Returns nonzero if any part of the bounding sphere is inside frustum

**`IsBoundingSphereVisibleInCurrentFrustum` (0x42DCA0)** — for track meshes (reads center from +0x10/14/18):
- Same 5-plane test but reads bounding center from different struct offsets
- Used by `RenderTrackSpanDisplayList` before queuing track objects

### Vertex Lighting Model: ComputeMeshVertexLighting (0x43DDF0)

Uses a **3-directional lighting model** with 9 coefficients. For each vertex normal
(nx, ny, nz), the lighting intensity is:

```
intensity = ROUND(
    nx * light0_x + ny * light0_y + nz * light0_z +   // direction 0
    nx * light1_x + ny * light1_y + nz * light1_z +   // direction 1
    nx * light2_x + ny * light2_y + nz * light2_z     // direction 2
) + ambient_offset

// 9 light coefficients at DAT_004ab0d0..DAT_004ab0f0 (3 directions × 3 components)
// Ambient offset at DAT_004bf6a8
// Result clamped to [0x40, 0xFF], written to vertex +0x18
```

The light direction coefficients are updated per-actor by `UpdateActiveTrackLightDirections`
based on the actor's current track segment, providing per-segment directional lighting
that varies along the track (tunnels darker, open sections brighter).

### Mesh Projection Effects: ApplyMeshProjectionEffect (0x43DEC0)

Three projection effect modes for environment/lighting texture overlay on vehicle meshes:

| Mode | Description | UV Computation |
|------|-------------|----------------|
| 1 | Planar XZ projection | `u = pos_x * 0.00067 + 0.5`, `v = (height + pos_z) * 0.00133` |
| 2 | Rotated planar projection | `u = (cos*x - sin*z + origin_x) * 0.00049`, `v = (z*cos + sin*x + origin_z) * 0.00098` |
| 3 | View-relative projection | Normal-weighted + view-space offset from light position, scaled by `16/viewport_far` |

Mode 3 is used for the damage model overlay (headlight projection). The effect overwrites
vertex `proj_u`/`proj_v` (+0x24/+0x28) and sets the texture page to `effect_slot.texture + 0x400`.

### Translucent Primitive Batching

**`QueueTranslucentPrimitiveBatch` (0x431480):**
- Maintains a linked list of primitive batch records (max 510 entries = `0x1FE`)
- Batches are keyed by texture page ID (field +0x02 in command record)
- If a batch with the same texture already exists, the new command is prepended to that batch's linked list
- Otherwise, a new slot is allocated and the command is inserted
- Linked via +0x04 pointer in each command record

**`FlushQueuedTranslucentPrimitives` (0x431340):**
- Walks the batch list (NOT depth-sorted — relies on submission order)
- For each batch: dispatches via `PTR_EmitTranslucentTriangleStrip_00473b9c[dispatch_type]`
- After emitting: applies greyscale-to-D3DCOLOR LUT (`DAT_004aee68[intensity & 0xFF]`)
- Submits via `IDirect3DDevice3::DrawPrimitive(D3DPT_TRIANGLELIST, D3DFVF=0x1C4, ...)`
- Resets batch count to 0 after flush

### Vehicle Taillight Rendering (0x4011C0)

Per-vehicle taillight quads with brightness decay:
- **Brightness byte** stored at `DAT_00482dd0[actor_index]` (0-255)
- **Braking active** (gear bits & 0xF != 0): brightness += 8 per frame (cap 0x7F→0x80 transition)
- **Braking inactive**: brightness >>= 1 per frame (exponential decay)
- Renders as billboard quads at hardcoded offsets from rear axle
- Diffuse color: `0xFF909090` (grey-white, alpha-blended with brightness)

### Texture Binding Pipeline

```
BindRaceTexturePage(slot)   -- VA 0x40B660
  |
  +-- Early-out if slot == DAT_0048dc34 (already bound)
  |
  +-- if slot < streaming_cache_count:
  |     if not resident: AdvanceTextureStreamingScheduler(slot)
  |     texture_index = cache_table[slot].texture_slot
  |     mark cache_table[slot].recently_used = 1
  +-- else:
  |     texture_index = slot - streaming_cache_count  // direct index
  |
  +-- Read transparency_mode from cache entry (+6) and clamp_mode (+7)
  |
  +-- Set D3D render state (via IDirect3DDevice3::SetRenderState, vtable+0x58):
  |     Mode 0: ALPHABLENDENABLE(0x1B)=OFF, TextureClamp(1) -- opaque
  |     Mode 1/2: ALPHABLENDENABLE=ON, SRCBLEND(0x13)=SRCALPHA(5),
  |               DESTBLEND(0x14)=INVSRCALPHA(6), TextureClamp(1) -- alpha blended
  |     Mode 3: ALPHABLENDENABLE=ON, SRCBLEND=ONE(2),
  |             DESTBLEND=ONE(2), TextureClamp(0=WRAP) -- additive blend
  |
  +-- IDirect3DDevice3::SetTexture (vtable+0x98, stage 0, texture_interface)
        // texture_interface from DXD3DTexture::Texture[index * 0x30]
```

### Texture Streaming: AdvanceTextureStreamingScheduler (0x40B830)

LRU eviction with retry-on-failure:

1. If cache is full (`current_slot == max_slots`):
   - Scan all slots for the **least recently used** entry (highest age value, or 0xFF = never used)
   - Evict that slot
2. If cache has room: advance `current_slot` pointer
3. Upload texture via `DXD3DTexture::LoadRGB` with format type derived from transparency mode:
   - Mode 0 → `IMAGETYPE 3` (16-bit, no alpha)
   - Mode 1 → `IMAGETYPE 4` (16-bit, 1-bit alpha)
   - Mode 2 → `IMAGETYPE 5` (16-bit, multi-bit alpha)
4. On upload failure: reduces `max_slots` by 1 and retries once ("Second Time Lucky" path)
5. Updates cache mapping: `cache[page_index].texture_slot = evicted_slot`

### M2DX.dll Texture Management

The M2DX.dll `DXD3DTexture::Manage` function (VA 0x10005550) handles the final
D3D surface creation:

1. Receives raw pixel data + slot index
2. Selects decode path based on `g_textureLoadSourceType`:
   - Type 1: `UploadRgb16ToScratchSurface` (16-bit indexed, from TEXTURES.DAT)
   - Type 4: `DX::DecodeRgbs24ToScratchSurface` (24-bit RGB, tpage data)
   - Type 5: `DX::DecodeRgbs32ToScratchSurface` (32-bit RGBA, tpage data)
3. Creates DirectDraw surface with appropriate caps (system/video/AGP memory)
4. Copies scratch surface to final texture surface
5. Stores `IDirect3DTexture2` interface pointer in `DXD3DTexture::Texture[slot]`

Per-texture record in the exported `DXD3DTexture::Texture` array is **0x30 (48) bytes**:

```
Offset  Size  Type                    Field
+0x00   4     IDirect3DTexture2*      texture_interface   D3D texture handle
+0x04   4     int32                   dimensions_encoded
+0x08   ...                           (source pointer, flags, format info)
+0x24   4     int32                   width
+0x28   4     int32                   height
+0x2C   4     int32                   flags
```

---

## Format Comparison: Track vs Vehicle Models

| Aspect | Track (MODELS.DAT) | Vehicle (himodel.dat) |
|--------|-------------------|----------------------|
| Container | Multi-entry (groups of sub-meshes) | Single mesh resource |
| File extension | .DAT | .dat |
| PRR format | Yes (identical) | Yes (identical) |
| Vertex format | 44 bytes, 11 floats | 44 bytes, 11 floats |
| Texture type | Streamed from cache | Non-streamed (slot >= 0x400) |
| UV space | Full [0,1] range | Half-page (U * 0.5 + side_offset) |
| Lighting | Track-segment ambient + vertex lit | Track-segment + per-vertex normals |
| LOD | No (single level) | No (single level) |
| Bounding sphere | Used for frustum culling | Not used (always visible when active) |
| Face types | Triangles + quads | Triangles + quads |
| Normal data | Optional (for backface cull) | Optional (for backface cull) |

---

## Key Constants

| Constant | Value | Meaning |
|----------|-------|---------|
| Fixed-point scale | 0.00390625 (1/256) | World coordinate -> float conversion |
| UV half-texel inset | 0.984375, 0.0078125 | 63/64 scale + 0.5/64 offset (for 64x64 textures) |
| Max streaming textures | 600 | Texture cache slot limit |
| Max translucent batches | 510 | FlushQueuedTranslucentPrimitives capacity |
| Depth sort buckets | 4096 | FlushProjectedPrimitiveBuckets |
| D3D FVF | 0x1C4 | XYZRHW + DIFFUSE + SPECULAR + TEX1 |
| D3D primitive | 4 (TRIANGLELIST) | All meshes render as indexed triangle lists |
| Vertex stride (model) | 0x2C (44 bytes) | 11 x float32 per vertex in PRR |
| Vertex stride (D3D) | 0x20 (32 bytes) | 8 x float32 per submitted vertex |
| Command stride | 0x10 (16 bytes) | Per-batch command record |
| Triangle normal stride | 0x30 (48 bytes) | 3 vertices x 16 bytes per normal |
| Quad normal stride | 0x40 (64 bytes) | 4 vertices x 16 bytes per normal |
