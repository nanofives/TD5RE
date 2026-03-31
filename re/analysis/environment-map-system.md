# Environment Map / Environment Texture System

The environment map system in Test Drive 5 provides per-vehicle reflection
textures and per-track billboard textures (trees, bridges, tunnels, sun sprites)
loaded from `environs.zip`. These are distinct from the sky dome texture (which
comes from the level ZIP) and serve two purposes: environment-mapped reflections
on vehicle bodywork, and trackside scenery billboards.

---

## Table of Contents

1. [Overview: What Environment Textures Are](#overview)
2. [Source Archives and Files](#source-files)
3. [Per-Track Environment Configuration Table](#per-track-config)
4. [Loading Pipeline: LoadEnvironmentTexturePages](#loading-pipeline)
5. [Texture Page Slots and Upload](#texture-page-slots)
6. [Rendering: Vehicle Reflection Projection](#rendering-projection)
7. [Rendering: Projection Effect Modes](#projection-modes)
8. [Sky Dome System (Related)](#sky-dome)
9. [Hardware Capability Gating](#hardware-gating)
10. [M2DXFX.dll Environment Functions](#m2dxfx)
11. [Relationship to Weather/Fog System](#weather-relationship)
12. [Key Functions Reference](#key-functions)
13. [Key Globals Reference](#key-globals)

---

## Overview: What Environment Textures Are {#overview}

Environment textures in TD5 are **NOT** cubemap-style environment maps in the
modern sense. They are 2D textures projected onto vehicle geometry to simulate
reflections of the surrounding environment (sky, scenery). The system has two
components:

1. **Environment reflection maps (ENV1-ENV4):** Up to 4 texture pages (slots
   13-16 in `static.hed`) that hold reflection textures. These are projected
   onto a duplicate of the player's car mesh (`g_playerReflectionMeshResource`)
   using UV coordinates computed from vertex normals and a per-track light/
   projection configuration. This creates a fake reflection effect on the car
   body.

2. **Billboard textures:** TGA files in `environs.zip` (`TREE.TGA`,
   `BRIDGE.TGA`, `SUN.TGA`, tunnel textures, etc.) that are loaded into
   environment texture page slots and used by trackside 3D model rendering
   for scenery objects.

The environment texture system is tightly coupled to:
- The **per-track lighting table** (VA 0x469C78), which defines projection
  effect parameters per track segment
- The **projection effect mode** (`g_vehicleProjectionEffectMode`), which
  determines whether reflections are rendered at all
- The **per-track environment data table** (VA 0x46BB1C), which specifies
  which TGA files to load from `environs.zip` for each track

---

## Source Archives and Files {#source-files}

### environs.zip

Located at `data\environs.zip`. Contains 27 TGA files -- environment/billboard
textures for per-track scenery:

| File | Size | Purpose |
|------|------|---------|
| `TREE.TGA` | 9,004 B | Generic tree billboard |
| `BRIDGE.TGA` | 9,004 B | Generic bridge texture |
| `SUN.TGA` | 17,196 B | Sun/lens flare sprite |
| `MSUN.TGA` | 17,196 B | Alternate sun sprite |
| `SUNBKP.TGA` | 17,196 B | Sun backup/alternate |
| `ATRE1.TGA` | 9,004 B | Track A tree variant 1 |
| `KTRE1.TGA` | 9,004 B | Track K tree variant 1 |
| `MTRE1.TGA` | 9,004 B | Track M tree variant 1 |
| Various `*TUN*.TGA` | 9,004 B | Tunnel entrance/exit textures |
| Various `*BRD*.TGA` | 9,004 B | Bridge texture variants |
| Various `*BWL*.TGA` | 9,004 B | Barrier/wall textures |
| `YSHP1.TGA` | 9,004 B | Track Y shape variant |

### static.zip

Contains the default/fallback environment texture:

| File | Size | Purpose |
|------|------|---------|
| `environ0.tga` | 196,652 B | Default environment map (TGA 256x256 @ 24bpp) |

### static.hed -- ENV Texture Page Entries

Four named entries in `static.hed` define the environment map texture page slots:

| # | Name | Pos | Size | Page | Pixel Format |
|---|------|-----|------|------|-------------|
| 47 | ENV1 | 0,0 | 128x128 | 13 | Opaque RGB24 |
| 48 | ENV2 | 0,0 | 128x64 | 14 | Opaque RGB24 |
| 49 | ENV3 | 0,0 | 128x64 | 15 | Opaque RGB24 |
| 50 | ENV4 | 0,0 | 128x64 | 16 | Opaque RGB24 |

Note the asymmetric dimensions: ENV1 is 128x128, while ENV2-4 are 128x64. This
likely reflects different texture usage (ENV1 for a larger reflection map, ENV2-4
for smaller billboard textures or secondary reflection angles).

---

## Per-Track Environment Configuration Table {#per-track-config}

### Table Pointer Array (VA 0x46BB1C)

`DAT_004aee10` is set by `InitializeTrackStripMetadata` from a static pointer
table at VA 0x46BB1C, indexed by track pool ID:

```c
DAT_004aee10 = *(int *)(&DAT_0046bb1c + trackPoolIndex * 4);
```

This gives a per-track data structure whose layout is:

```
Offset  Size    Type        Description
+0x00   4       int32       environment_page_count  (number of ENV pages to load)
+0x04   N*4     int32[]     projection_effect_index_table (indexed by light entry)
+0x14   N*4     int32[]     projection_mode_table (mode per environment entry)
+0x40   N*0x20  records[]   environment_page_records (one per env page)
```

Each environment page record (0x20 = 32 bytes) contains:
- A TGA filename (null-terminated string) for the texture to load from
  `environs.zip`
- The runtime classifies each filename: if it starts with `"SUN"` (checked
  via `strnicmp(..., "SUN", 3)` at VA 0x42FB08), the projection mode is set
  to 3; otherwise it is set to 1

### Projection Mode Classification

```c
// InitializeTrackStripMetadata (0x42FAD0)
for each env page entry at offset +0x40, +0x60, +0x80, +0xA0:
    if strnicmp(filename, "SUN", 3) == 0:
        projection_mode_table[i] = 3   // Point-source projection (sun position)
    else:
        projection_mode_table[i] = 1   // Directional/heading-based projection
```

This means environment textures that represent the sun use a different projection
mode (world-space point source) than regular environment reflections (heading-based
cylindrical mapping).

---

## Loading Pipeline: LoadEnvironmentTexturePages {#loading-pipeline}

**Function:** `LoadEnvironmentTexturePages` (VA 0x42F990)

**Called from:** `LoadRaceTexturePages` (VA 0x442770), at the end of the texture
loading sequence, after all car skins, wheel hubs, track textures, and traffic
skins have been loaded.

**Call chain:**
```
InitializeRaceSession (0x42AA10)
  -> InitializeRaceVideoConfiguration (0x42A950)
       -> SetRaceTexturePageLoader(LoadRaceTexturePages)
  ...
  -> LoadRaceTexturePages (0x442770)   [called via function pointer]
       -> LoadEnvironmentTexturePages (0x42F990)
       -> PreloadLevelTexturePages (0x40BAE0)
```

### Algorithm

```c
void LoadEnvironmentTexturePages(void) {
    byte *buffer = malloc(0x20000);  // 128 KB working buffer

    if (g_vehicleProjectionEffectMode == 1) {
        // Mode 1: raw binary upload (no decode, no resampling)
        // Iterates *DAT_004aee10 environment pages
        for (i = 0; i < *DAT_004aee10; i++) {
            // Read TGA filename from per-track table at offset 0x40 + i*0x20
            ReadArchiveEntry(
                filename_from_table,
                "environs.zip",
                buffer, 0x20000
            );
            NoOpHookStub();  // dead hook point
        }
    }
    else if (g_vehicleProjectionEffectMode == 2) {
        // Mode 2: full decode + upload path
        byte *decoded = buffer + 0x10000;  // second half of buffer

        for (i = 0; i < *DAT_004aee10; i++) {
            // Look up "ENV%d" entry in static.hed
            sprintf(name, "ENV%d", i+1);
            entry = FindArchiveEntryByName(name);

            // Read TGA from environs.zip
            ReadArchiveEntry(
                filename_from_table,
                "environs.zip",
                buffer, 0x20000
            );

            // Decode TGA to raw RGB24
            DecodeArchiveImageToRgb24(buffer, decoded);

            // Resample if needed (software renderer path)
            if (DAT_004c3d04 == 0) {
                ResampleTexturePageToEntryDimensions(
                    decoded, entry->texture_slot - 0x400
                );
            }

            // Upload to D3D texture page
            UploadRaceTexturePage(
                decoded,
                entry->texture_slot - 0x400,
                0,   // opaque (RGB24)
                1    // flag: mark as environment/projection texture
            );
        }
    }

    free(buffer);
}
```

### Key Observations

1. **Mode 1 reads but does not upload:** In mode 1, `ReadArchiveEntry` is called
   but no `UploadRaceTexturePage` follows. The data is read into a buffer then
   discarded. This is likely a vestigial code path from the software renderer.

2. **Mode 2 is the active D3D path:** Full decode -> resample -> upload pipeline.
   The `param_4 = 1` flag passed to `UploadRaceTexturePage` marks these pages
   with a special transparency mode (2 or 3) in the `DAT_0048dbac` table,
   enabling the texture wrap/clamp behavior needed for environment projection.

3. **Buffer layout:** The 128 KB buffer is split: first 64 KB for raw archive
   data, second 64 KB for decoded RGB data.

---

## Texture Page Slots and Upload {#texture-page-slots}

### UploadRaceTexturePage (VA 0x40B590)

Handles the final upload of decoded pixel data to D3D texture surfaces:

```c
void UploadRaceTexturePage(void *pixels, uint pageIndex, int format, int isEnvPage) {
    // Special alpha processing for page 4 (HUD effects page)
    if (pageIndex == 4) { ... }

    // Upload based on format
    if (format == 0) DXD3DTexture::LoadRGBS24(pixels, pageIndex, 0);
    if (format == 1) DXD3DTexture::LoadRGBS32(pixels, pageIndex, 7, 0);
    if (format == 2) DXD3DTexture::LoadRGBS32(pixels, pageIndex, 8, 0);

    // Record transparency mode for BindRaceTexturePage
    if (pageIndex < 0x20) {
        if (isEnvPage != 0) {
            if (d3d_exref->multiTextureSupport != 0)
                transparencyTable[pageIndex] = 2;  // env with multitexture
            else
                transparencyTable[pageIndex] = 3;  // env without multitexture
        } else {
            transparencyTable[pageIndex] = (format != 0) ? 1 : 0;
        }
    }
}
```

### Transparency/Blend Modes in BindRaceTexturePage (VA 0x40B660)

When binding a texture page for rendering, the transparency mode controls D3D
render state:

| Mode | Meaning | Render State |
|------|---------|-------------|
| 0 | Opaque | Alpha blending off, texture clamp on |
| 1-2 | Alpha blended | Alpha blending on, src=5 dst=6 (SrcAlpha/InvSrcAlpha), clamp on |
| 3 | Environment wrap | Alpha blending on, src=2 dst=2 (special blend), **clamp OFF** (wrap) |

Mode 3 (no clamp / texture wrap) is specifically for environment reflection
textures when multitexture is not available -- the reflection UV coordinates can
exceed 0..1 range and need to wrap around the texture.

---

## Rendering: Vehicle Reflection Projection {#rendering-projection}

### The Reflection Mesh

When `g_vehicleProjectionEffectMode == 2`, `LoadRaceVehicleAssets` (VA 0x443280)
allocates and loads a **duplicate copy** of the player's car model into
`g_playerReflectionMeshResource` (VA 0x4C3D40):

```c
if (g_vehicleProjectionEffectMode == 2) {
    // Allocate space for reflection mesh right after main meshes
    g_playerReflectionMeshResource = nextFreePtr;
    nextFreePtr += meshSizes[0];

    // Later: load same himodel.dat again
    ReadArchiveEntry("himodel.dat", carZipPath,
                     g_playerReflectionMeshResource, size);

    // Patch UVs for left-half only (slot 0 mapping)
    for each vertex: u = u * 0.5;

    // Force texture page to 0x0404 (ENV1 slot + 0x400 base)
    commandHeader->texturePage = 0x0404;

    // Set command count to 1 (single submesh for reflection overlay)
    g_playerReflectionMeshResource->commandCount = 1;
}
```

Key points:
- The reflection mesh uses the **same geometry** as the player's car
- Its texture page is hardcoded to **0x0404** (which is ENV texture page with
  the 0x400 offset applied)
- UV coordinates are patched to map only the left half (slot 0 pattern)
- The submesh count is forced to 1 (only the outer body shell, not windows/
  interior sub-parts)

### Rendering the Reflection Overlay

In `RenderRaceActorForView` (VA 0x40C120), the reflection mesh is rendered
as a **second pass** over the player's car:

```c
// After rendering the main car mesh:
RenderPreparedMeshResource(mainMesh);

// Reflection overlay (player car only, mode 2 only)
if (g_vehicleProjectionEffectMode == 2 && actorSlot == 0) {
    TransformMeshVerticesToView(g_playerReflectionMeshResource);
    ApplyMeshProjectionEffect(g_playerReflectionMeshResource, projectionSlot=0);
    RenderPreparedMeshResource(g_playerReflectionMeshResource);
}
```

The reflection is **only rendered for the player's car** (slot 0). Other vehicles
do not get environment reflections.

---

## Rendering: Projection Effect Modes {#projection-modes}

### SetProjectionEffectState (VA 0x43E210)

Stores per-slot projection parameters into a global table at VA 0x4BF520
(stride 0x20 = 32 bytes per slot):

| Mode | Name | UV Computation | Use Case |
|------|------|----------------|----------|
| 1 | Heading accumulator | UV.u = vertex.x * scale + offset; UV.v = (accumulated_height + vertex.z) * scale | Standard env reflection (scrolls with vehicle heading) |
| 2 | Heading basis | UV.u = cos(heading)*x - sin(heading)*z + center; UV.v = sin(heading)*x + cos(heading)*z + center | Rotational env reflection (rotates with vehicle yaw) |
| 3 | World-space anchor | Stores world position of light source; UV computed from vertex normal offset relative to light | Sun/point-source projection (directional highlight) |

### ApplyMeshProjectionEffect (VA 0x43DEC0)

Called per-frame on the reflection mesh. Rewrites the projection UV coordinates
(vertex offsets +0x1C and +0x20 in the per-face normal data) based on the
active projection mode:

**Mode 1 (heading accumulator):**
- U = vertex.x * fixedScale + fixedOffset
- V = (accumulatedHeading + vertex.z) * fixedScale
- Creates a cylindrical reflection that scrolls as the vehicle moves

**Mode 2 (heading basis):**
- U = (cos * x - sin * z + meshCenter.x) * uScale
- V = (sin * x + cos * z + meshCenter.z) * vScale
- Creates a rotation-locked reflection that follows vehicle yaw

**Mode 3 (world-space anchor / sun):**
- Computes offset from vertex to light world position
- Transforms through view basis matrix
- UV = vertexNormal * normalScale + (faceNormal - lightOffset) * depthScale + center
- Creates a specular-highlight-like projection from a point source

After UV computation, the texture page for the mesh command is set to:
```c
commandHeader->texturePage = projectionSlot.texturePageIndex + 0x400;
```

### ConfigureActorProjectionEffect (VA 0x40CBD0)

Called from `UpdateActorTrackLightState` per-frame. Selects which projection mode
to use based on the per-track lighting table entry for the actor's current
track segment:

```c
void ConfigureActorProjectionEffect(RuntimeSlotActor *actor, int lightIndex) {
    actor->projectionLightIndex = lightIndex;

    int envPageIndex = DAT_004aee10[1 + lightIndex];  // from per-track table
    if (g_vehicleProjectionEffectMode == 2)
        envPageIndex += DAT_004c3cf4;  // offset for mode-2 page numbering

    int projMode = DAT_004aee10[5 + envPageIndex];  // 1, 2, or 3

    // Build parameters based on mode and pass to SetProjectionEffectState
    switch (projMode) {
        case 1: SetProjectionEffectState(slot, heading, velocity, 1, envPageIndex);
        case 2: SetProjectionEffectState(slot, -eulerYaw>>8, velocity, 2, envPageIndex);
        case 3: SetProjectionEffectState(slot, 0, worldPos, 3, envPageIndex);
    }
}
```

The lighting table at `DAT_004aee14` (VA 0x4AEE14, per-track, 0x24 bytes per
entry) provides the `lightIndex` at offset +0x1C which selects the projection
effect for each track segment.

---

## Sky Dome System (Related) {#sky-dome}

The sky dome is a **separate system** from environment maps but shares some
infrastructure:

### Sky Mesh
- `sky.prr` loaded from `STATIC.ZIP` (or `static.zip`)
- 1 submesh, 1152 vertices
- Texture page forced to 0x0403 (SKY entry, page 3 in static.hed)

### Sky Texture
- Comes from **level ZIP** (not environs.zip)
- `FORWSKY.TGA` (forward direction) or `BACKSKY.TGA` (reverse direction)
- 256x256 @ 24bpp TGA
- Selected by `gReverseTrackDirection`

### Sky Rotation
`AdvanceGlobalSkyRotation` (VA 0x43D7C0) increments a global rotation counter
by 0x400 per frame (unless paused), providing slow sky rotation during gameplay.
This is called from `UpdateVehicleLoopingAudioState` (piggybacks on the per-frame
audio update).

### Sky vs Environment Maps
| Aspect | Sky Dome | Environment Maps |
|--------|----------|-----------------|
| Source archive | level ZIP | environs.zip |
| Texture page | Page 3 (SKY) | Pages 13-16 (ENV1-4) |
| Geometry | sky.prr (dome mesh) | Player car mesh duplicate |
| Per-track | Different TGA per track | Different TGAs per track |
| Purpose | Background sky rendering | Vehicle body reflections + billboards |

---

## Hardware Capability Gating {#hardware-gating}

### GetEnvironmentTexturePageMode (VA 0x40AEF0)

Returns 0 or 2 based on D3D hardware capability:

```c
int GetEnvironmentTexturePageMode(void) {
    // d3d_exref + 0xa5c: multitexture capability flag
    if (*(int *)(d3d_exref + 0xa5c) != 0)
        return 0;   // Multitexture supported -> mode 0 (no extra pages needed)
    else
        return 2;   // No multitexture -> mode 2 (separate reflection pass)
}
```

This value is queried during `InitializeRaceVideoConfiguration` via
`QueryRaceSharedPointer(4, &g_vehicleProjectionEffectMode, 4)` and stored in
`g_vehicleProjectionEffectMode` (VA 0x4C3D44).

### Mode Implications

| g_vehicleProjectionEffectMode | Meaning |
|------|---------|
| 0 | Multitexture available; environment reflections handled by hardware multitexture (no separate reflection mesh needed) |
| 1 | Software/limited path; env textures read but not uploaded (vestigial) |
| 2 | No multitexture; full software projection with duplicate mesh + second render pass |

In mode 2:
- `LoadTrackTextureSet` adjusts texture page count: `DAT_004c3cf4 = pageCount - 4`
- `LoadRaceVehicleAssets` allocates `g_playerReflectionMeshResource`
- `LoadEnvironmentTexturePages` performs full decode + upload
- `RenderRaceActorForView` renders the reflection overlay pass

---

## M2DXFX.dll Environment Functions {#m2dxfx}

### DXDraw::Environment (VA 0x10001000)

Initialization function for the DirectDraw subsystem. Zeroes out the `dd`
structure (0x5D6 dwords = 5976 bytes), resets screen coordinates, and enables
character override printing. This is a **system initialization** function, not
related to environment textures.

### DXWin::Environment (VA 0x1000C790)

Top-level M2DX environment initialization. Calls in sequence:
1. `DXDraw::Environment()` -- DirectDraw init
2. `DXInput::Environment()` -- DirectInput init
3. `DXSound::Environment()` -- DirectSound init
4. `DXPlay::Environment()` -- DirectPlay init
5. `ParseCommandLineAndIniFile()` -- Config parsing

These functions establish the M2DX runtime environment (DirectX subsystems)
and are unrelated to environment mapping textures. The naming collision with
"environment" is coincidental -- in the M2DX framework, "Environment" means
"initialize the runtime environment/context."

---

## Relationship to Weather/Fog System {#weather-relationship}

Environment maps and weather/fog are **independent but spatially correlated**
systems:

### Shared Configuration Source
Both use data from `LEVELINF.DAT` (loaded to `gTrackEnvironmentConfig` at
VA 0x4AEE20):
- Weather type (rain/snow) and density zones affect particle rendering
- Fog distance and color affect visibility
- Environment textures are selected per-track independently

### Per-Track Coupling
The per-track lighting table (`DAT_004aee14`) drives both:
- **Fog color overrides** at offset +0x10 per lighting zone entry
- **Projection effect selection** at offset +0x1C per lighting zone entry

This means environment reflections and fog color can change together as the
player drives through different track segments (e.g., entering a tunnel changes
both the fog/lighting and the projection effect used for reflections).

### No Dynamic Weather-to-Reflection Link
There is no code path that modifies environment map textures based on weather
state. A rainy track uses the same environment reflection textures as a dry
track; only the sky TGA differs per-track. Weather effects (rain/snow particles,
fog) are purely additive overlay systems.

---

## Key Functions Reference {#key-functions}

| Function | VA | Purpose |
|----------|-----|---------|
| `LoadEnvironmentTexturePages` | 0x42F990 | Main loader: reads TGAs from environs.zip, uploads to ENV pages |
| `InitializeTrackStripMetadata` | 0x42FAD0 | Sets DAT_004aee10 from per-track table, classifies SUN vs normal |
| `GetEnvironmentTexturePageMode` | 0x40AEF0 | Returns 0 or 2 based on multitexture hardware support |
| `LoadRaceTexturePages` | 0x442770 | Parent loader: sky, car skins, hubs, traffic, then env textures |
| `LoadTrackTextureSet` | 0x442670 | Loads static.hed + TEXTURES.DAT, calls InitializeTrackStripMetadata |
| `UploadRaceTexturePage` | 0x40B590 | Uploads decoded pixels to D3D, sets blend mode in transparency table |
| `BindRaceTexturePage` | 0x40B660 | Binds texture page for rendering, applies blend/wrap state |
| `ConfigureActorProjectionEffect` | 0x40CBD0 | Per-frame: selects projection mode from lighting table for actor |
| `SetProjectionEffectState` | 0x43E210 | Stores projection slot parameters (heading, position, mode) |
| `ApplyMeshProjectionEffect` | 0x43DEC0 | Per-frame: rewrites projection UVs on reflection mesh vertices |
| `UpdateActorTrackLightState` | 0x40CD10 | Light state machine, calls ConfigureActorProjectionEffect |
| `RenderRaceActorForView` | 0x40C120 | Renders actor + reflection overlay (slot 0, mode 2 only) |
| `LoadRaceVehicleAssets` | 0x443280 | Allocates/loads g_playerReflectionMeshResource in mode 2 |
| `AdvanceGlobalSkyRotation` | 0x43D7C0 | Increments sky rotation angle by 0x400/frame |
| `InitializeRaceVideoConfiguration` | 0x42A950 | Queries projection mode, sets up texture loader callback |
| `QueryRaceSharedPointer` | 0x42E9D0 | ID=4 returns GetEnvironmentTexturePageMode result |

---

## Key Globals Reference {#key-globals}

| Global | VA | Type | Description |
|--------|----|------|-------------|
| `g_vehicleProjectionEffectMode` | 0x4C3D44 | int | 0=multitex, 1=soft, 2=projection pass |
| `g_playerReflectionMeshResource` | 0x4C3D40 | void* | Duplicate car mesh for reflection overlay |
| `DAT_004aee10` | 0x4AEE10 | int* | Per-track environment config (page count, filenames, modes) |
| `DAT_004aee14` | 0x4AEE14 | int* | Per-track lighting zone table (0x24 bytes/entry) |
| `DAT_004c3cf4` | 0x4C3CF4 | int | Texture page count offset for mode-2 indexing |
| `DAT_0048dbac` | 0x48DBAC | int[] | Per-page transparency/blend mode table |
| `DAT_004bf520+` | 0x4BF520 | struct[] | Per-slot projection effect state (0x20 bytes/slot) |
| Per-track env table | 0x46BB1C | int*[] | 18 pointers to per-track environment config structs |
| `gStaticHedEntryArray` | dynamic | byte* | Parsed static.hed named entry array |
| `gStaticHedEntryCount` | dynamic | uint | Number of named entries in static.hed |
| `gTrackTextureCount` | dynamic | int | Number of texture pages for current track |

---

## Summary

The environment map system is a **per-vehicle fake reflection** technique that:

1. Loads 1-4 environment textures from `environs.zip` based on a per-track
   configuration table embedded in the EXE at VA 0x46BB1C
2. Uploads them to texture pages 13-16 (ENV1-ENV4 in static.hed)
3. Creates a duplicate of the player's car mesh with UVs remapped for
   environment projection
4. Each frame, computes projection UVs from vertex normals/positions using
   one of three modes (heading scroll, heading rotation, or world-space sun
   point source)
5. Renders the reflection mesh as a transparent overlay on top of the player's
   car, using either multitexture hardware (mode 0) or a separate render pass
   (mode 2)

The system is gated by hardware capabilities -- if the D3D device supports
multitexture (`d3d_exref + 0xa5c != 0`), a simpler code path is used. Otherwise,
the full software projection pipeline with a duplicate mesh is employed.

Per-track variation comes from the 18-entry table at VA 0x46BB1C, which selects
different TGA files from `environs.zip` and classifies them as either
directional reflections (modes 1/2) or sun highlights (mode 3) based on
whether the filename starts with "SUN".
