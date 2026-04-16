# Render Pipeline, Scene Setup & Translucent Primitives — Deep Dive

> Decompiled from `TD5_d3d.exe` via Ghidra (port 8195), 2026-03-20

---

## 1. Race Scene Lifecycle

The race renderer follows a strict **Init -> Begin -> Render -> End** lifecycle each frame.

### 1.1 One-Time Initialization

#### `InitializeRaceRenderGlobals` (0x40ae10)
Called once at startup. Sets up:
- **FPU control**: Forces single-precision rounding mode (`_controlfp(0x100, 0x300)`), saves both the clean and modified FPU control words at `0x48db90`/`0x48db94`
- **8-byte-aligned transform buffer**: `malloc(0x38)` aligned to 8 bytes, stored at `DAT_004bf6b8` — this is the **current render transform** (3x4 matrix = 48 bytes)
- **Sin/cos lookup tables**: `BuildSinCosLookupTables()` for 12-bit angle trig
- **Guard flag**: `DAT_0048dba8` prevents re-initialization

Returns 0 if already initialized, 1 on success.

#### `InitializeRaceRenderState` (0x40ae80)
Called per-race-start. Initializes:
1. `InitializeTranslucentPrimitivePipeline()` — linked-list sort buckets
2. `InitializeProjectedPrimitiveBuckets()` — HUD/overlay depth sort
3. `ResetProjectedPrimitiveWorkBuffer()` — work buffer clear
4. Sets `bClearScreen_exref = 1` (force first-frame backbuffer clear)
5. Guard flag `DAT_0048dba0 = 1`

Returns 0 if already active (re-entrant guard).

#### `ReleaseRaceRenderResources` (0x40aec0)
Teardown at race end:
- `DXD3DTexture::ClearAll()` — releases all D3D texture objects
- Clears fog enable flag at `d3d_exref + 0xa34`
- Resets active flag `DAT_0048dba0 = 0`

### 1.2 Per-Frame Scene Brackets

#### `BeginRaceScene` (0x40ade0)
```
DXD3D::BeginScene();
DAT_0048da04 = 0xFFFFFFFF;  // previous texture page (force rebind)
DAT_0048da00 = 0xFFFFFFFF;  // current texture page (force rebind)
```
Resets the texture-page sort keys so the first DrawPrimitive call will always bind a fresh texture. This is the **only** setup before rendering begins — no render state reset here, that happens in the preset system.

#### `EndRaceScene` (0x40ae00)
```
DXD3D::EndScene();
AdvanceTexturePageUsageAges();
```
After the D3D EndScene, the texture cache aging system runs to track which pages were used this frame.

---

## 2. Viewport Layout

#### `InitializeRaceViewportLayout` (0x42c2b0)
Builds three viewport layout entries at `0x4AAE14` from `gRenderWidth`/`gRenderHeight`:

| Layout | Description | Computed Values |
|--------|-------------|-----------------|
| 0 (fullscreen) | Single view | origin=(0,0), size=(width, height), center at half |
| 1 (horiz split) | Top/bottom | Top: y=[0, centerY-1], Bottom: y=[centerY+1, height]. 1px gap. Scale flag=2 |
| 2 (vert split) | Left/right | Left: x=[0, centerX-1], Right: x=[centerX+1, width]. 1px gap. Scale flag=2 |

All values are **fully dynamic** — derived from `gRenderWidth`, `gRenderHeight`, `gRenderCenterX`, `gRenderCenterY`. The widescreen patch requires no additional fixup here.

Each entry stores: origin X/Y, extents, center offsets (0.5x / 1.5x for split halves), a scale flags field, and padding.

---

## 3. Fog Configuration

#### `ConfigureRaceFogColorAndMode` (0x40af10)
```c
void ConfigureRaceFogColorAndMode(uint color, int enable) {
    d3d_exref[0x18] = color & 0xFFFFFF;  // strip alpha, store RGB
    if (DXD3D::CanFog())
        d3d_exref[0xa34] = enable;       // fog mode flag
    else
        d3d_exref[0xa34] = 0;            // force off if hardware can't fog
}
```

#### `ApplyRaceFogRenderState` (0x40af50)
Two modes via `param_1`:
- **param=0**: Disables fog — `SetRenderState(D3DRENDERSTATE_FOGENABLE=0x1c, FALSE)`
- **param=1**: Applies full fog pipeline via IDirect3DDevice3::SetRenderState (vtable+0x58):

| D3DRENDERSTATE | Value | Meaning |
|----------------|-------|---------|
| 0x23 (FOGTABLEMODE) | 3 (D3DFOG_LINEAR) | Linear table fog |
| 0x22 (FOGCOLOR) | `d3d_exref+0x18` | Stored RGB color |
| 0x24 (FOGSTART) | `d3d_exref+0x20` | Fog start distance (float as DWORD) |
| 0x25 (FOGEND) | `d3d_exref+0x24` | Fog end distance |
| 0x26 (FOGDENSITY) | `d3d_exref+0x28` | Fog density |
| 0x1c (FOGENABLE) | 1 | Enable |

Each SetRenderState failure is logged via `DXErrorToString` + `Msg()`. The fog parameters at `d3d_exref+0x20..+0x28` are set elsewhere (track load), confirmed as start=0.01, end=0.5, density=0.99 from M2DX analysis.

---

## 4. D3D Render State Presets

#### `SetRaceRenderStatePreset` (0x40b070)
Four presets controlling texture filtering (0x17 = D3DRENDERSTATE_TEXTUREMAG) and alpha enable (0x0e = D3DRENDERSTATE_ALPHABLENDENABLE):

| Preset | Filter (0x17) | Alpha (0x0e) | Use Case |
|--------|---------------|--------------|----------|
| 0 | 8 (ANISOTROPIC) | OFF | Opaque track geometry |
| 1 | 4 (LINEAR) | ON | Translucent surfaces with linear filter |
| 2 | 8 (ANISOTROPIC) | ON | Translucent surfaces with aniso filter |
| 3 | 4 (LINEAR) | OFF | Opaque geometry with linear filter |

All calls go through the raw IDirect3DDevice3 vtable at slot 0x58 (SetRenderState). The device pointer is read from `d3d_exref + 0x38`.

---

## 5. Texture Page Cache System

#### `GetTextureSlotStatus` (0x40b9f0)
Returns the status byte for a texture slot index. Accesses `DAT_0048dc40` (texture cache control block):
- `+0x04`: slot count (bounds check)
- `+0x0c`: base of 8-byte slot descriptors; status byte at offset +7 within each 8-byte entry

Returns 0 if index out of range.

#### `AdvanceTexturePageUsageAges` (0x40ba10)
Called from `EndRaceScene`. Per texture page slot:
- If the page was **used this frame** (flag at `DAT_0048dc40[0]` array): reset its age counter to 0, clear used flag
- If **not used**: increment age counter (at descriptor+5) unless already at 0xFF (saturate)
- Always clears the per-frame used flag

This LRU aging drives texture eviction when the cache is full.

#### `ResetTexturePageCacheState` (0x40ba60)
Full cache reset before loading a new track's textures:
- Clears `+0x18` (active count)
- Zeros all slot status/age bytes (descriptor+4 and +5 for each slot)
- Zeros the device-side texture handle array (`+0x08`, `+0x10` entries)

#### `QueryRaceTextureCapacity` (0x40baa0)
Queries the M2DX texture budget:
```c
maxTex = DXD3D::GetMaxTextures(0x40);  // max 64 textures
DAT_0048dc40[0x10] = maxTex;           // store in cache control
agpStr = DX::GetStateString(dd_exref + 0x167c);
LogReport("AGP Usage: %s", agpStr);
```

#### `GetEnvironmentTexturePageMode` (0x40aef0)
Returns 2 if `d3d_exref+0xa5c` is zero (normal mode), 0 if nonzero (environment mapping mode). This drives the texture coordinate generation path for environment-mapped surfaces.

---

## 6. Render Transform Stack

The engine uses a **software 3x4 transform** stored at `DAT_004bf6b8` (8-byte aligned, 48 bytes = 12 floats). Layout: 3x3 rotation matrix (indices 0-8) + 3-component translation (indices 9-11, byte offsets 0x24-0x2C).

#### `LoadRenderRotationMatrix` (0x43da80)
Copies 9 floats (3x3 rotation) from source into `DAT_004bf6b8[0..8]`. Translation is **not** touched.

#### `LoadRenderTranslation` (0x43dc20)
Copies 3 floats from `param_1 + 0x24/0x28/0x2C` into `DAT_004bf6b8 + 0x24/0x28/0x2C`. Only the translation column.

#### `PushRenderTransform` (0x43daf0)
Saves all 12 floats (full 3x4) to backup slot at `0x4C36C8`. **Single-depth stack** — only one push allowed before a pop.

#### `PopRenderTransform` (0x43db70)
Restores all 12 floats from the backup slot at `0x4C36C8` back to `DAT_004bf6b8`.

#### `TransformVec3ByRenderMatrixFull` (0x43dc50)
Standard 3x3 matrix-vector multiply (rotation only, no translation):
```
out[0] = in[0]*M[0] + in[1]*M[1] + in[2]*M[2]
out[1] = in[0]*M[3] + in[1]*M[4] + in[2]*M[5]
out[2] = in[0]*M[6] + in[1]*M[7] + in[2]*M[8]
```
Used for direction vectors (normals, light directions) that should not be translated.

---

## 7. Translucent Primitive Pipeline

#### `InitializeTranslucentPrimitivePipeline` (0x4312e0)
Sets up the sorted translucent batch system:

1. **Color lookup table** at `0x4AEE68` (1024 entries, 4 bytes each): pre-computed packed ARGB values. Initialized as `i * 0x10101 - 0x1000000` for i=0..1023. This maps a byte luminance to a grey ARGB with alpha=0xFF.

2. **Linked-list pool**: `HeapAllocTracked(0x1000)` — 4096 bytes = 512 entries x 8 bytes each. Each entry has:
   - 2-byte flags (init to 0)
   - 2-byte sort key (init to 0xFFFF)
   - 4-byte next pointer (linked to sequential next)

3. `DAT_004af270 = 0` — active batch count reset

The 510-batch limit mentioned in MEMORY.md comes from this 512-entry pool (2 reserved for head/tail sentinels).

#### `FlushImmediateDrawPrimitiveBatch` (0x4329e0)
Submits accumulated vertices to D3D:

1. **Texture bind**: `BindRaceTexturePage(DAT_0048da00)` — current page
2. **Color fixup**: For each vertex, replaces the low byte of the diffuse color with a lookup from the `0x4AEE68` table (converts luminance index to packed ARGB)
3. **DrawPrimitive call**: `IDirect3DDevice3::DrawPrimitive` (vtable+0x74) with:
   - Type: 4 (D3DPT_TRIANGLELIST)
   - FVF: 0x1C4 (D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1) — pre-transformed, lit
   - Vertex buffer at `DAT_004afb14`
   - Index buffer at `DAT_004af314`
   - Flags: 0xC
4. **Reset**: vertex count (`DAT_004afb4c`) and index count (`DAT_004afb50`) zeroed. Current texture page (`DAT_0048da00`) reset to previous (`DAT_0048da04`).

#### `GetTrackSpanDisplayListEntry` (0x431260)
Simple accessor: `return gModelsDatEntryTable[param_1 * 8]` — returns the display list pointer for a track span from the MODELS.DAT entry table. 8-byte stride entries (pointer + metadata).

#### `BuildSpriteQuadTemplate` (0x432bd0)
Builds a 4-vertex quad from a parameter block. The param block `param_1` has bitfield flags at `[1]`:
- **Bit 0 (0x01)**: Write screen-space XY positions (perspective-divided by W) and Z/W depths for all 4 corners
- **Bit 1 (0x02)**: Write UV texture coordinates (scaled by `_DAT_004749d0` = world-to-texel scale)
- **Bit 2 (0x04)**: Write vertex colors (masked to low byte = luminance index)
- **Bit 8 (0x100)**: Set primitive type: 3 (triangle strip) if `[0x1a]!=0`, else 5 (triangle fan)
- **Bit 9 (0x200)**: Set texture page index from `[0x1b]`

Output goes to the short* at `param_1[0]` — a pre-allocated vertex buffer region. The quad is written as 4 vertices at fixed offsets (strides of 0x16 shorts = 44 bytes per vertex).

---

## 8. Actor Rendering

#### `RenderRaceActorsForView` (0x40bd20)
Master per-view actor renderer. Two code paths:

**Normal mode** (`gDragRaceModeEnabled == 0`):
- Iterates slots 0..min(DAT_004aaf00, 6), calls `RenderRaceActorForView(slot, viewIndex)` for each

**Drag race mode**:
- Same iteration but skips slots where `gRaceSlotStateTable.slot[i].state == 0` (inactive)

**Traffic actors** (slots 6+, if `gTrafficActorsEnabled != 0`):
For each traffic slot:
1. Interpolate position using velocity * `_DAT_004aaf60` (sub-tick fraction)
2. Compute span distance from camera, with ring-length wraparound
3. **Cull test**: Skip if absolute span distance >= `gRaceTrackSpanCullWindow`
4. If visible:
   - `ApplyTrackLightingForVehicleSegment` — set lighting from track segment
   - `ApplyMeshRenderBasisFromTransform` — build model-to-world matrix
   - `TestMeshAgainstViewFrustum` — frustum cull
   - If passes: `UpdateActiveTrackLightDirections`, `TransformMeshVerticesToView`, `ComputeMeshVertexLighting`, `RenderPreparedMeshResource`
5. Special encounter actor (slot 9): renders tracked actor marker if `gSpecialEncounterTrackedActorHandle != -1`
6. Wheel billboard rendering: builds 2 translucent quads (front pair + rear pair) via `WritePointToCurrentRenderTransform` and `QueueTranslucentPrimitiveBatch`
7. Smoke sprites: `SpawnVehicleSmokeSprite` if traffic actor is in recovery mode

#### `BuildSpecialActorOverlayQuads` (0x40c7e0)
Builds oriented billboard quads for special actors (encounter vehicles, etc.):
1. Reads actor heading (12-bit angle at +0x20a), position (+0x1fc, +0x204), velocity (+0x1cc, +0x1d4)
2. Interpolates position by sub-tick fraction `_DAT_004aaf60`
3. Computes sin/cos of heading via `SinFloat12bit`/`CosFloat12bit`
4. Iterates 8 corner points (at actor+0x98, stride 12 bytes) to find the max oriented bounding extents
5. If actor has overlay dimensions (+0x292 nonzero):
   - Builds 4 rotated corners using the bounding extents and heading rotation
   - Scales by overlay parameters (+0x290, +0x292, +0x294)
   - Transforms into view space via `WritePointToCurrentRenderTransform`
   - Adds Y offset (`_DAT_0048dc48`) to all 4 corners
   - Queues 2 translucent primitive batches (front face + back face)

#### `InitializeRaceActorRuntime` (0x432e60)
Pre-race actor setup:
1. Copies base AI parameters from `DAT_004aee1c`/`DAT_004aed8c`/`DAT_004aed94`
2. Zeros the entire `gActorRouteStateTable` (0x354 dwords = 3408 bytes)
3. Sets actor count: `6 + (gTrafficActorsEnabled ? 6 : 0)`, or 2 for time trial
4. For each AI slot (state == 0): copies 0x20 dwords of base AI config from `0x473db0`
5. **Difficulty scaling** (multiply-and-shift-by-256 pattern):
   - **Easy**: throttle threshold *= 360/256, brake threshold *= 300/256
   - **Hard**: throttle *= 650/256, brake *= 380/256, steering *= 450/256, top speed *= 400/256
   - **Point-to-point (Ultimate variant=4)**: aggressive throttle scaling (0x91/256), fixed steering/brake overrides
   - **No-traffic tiers 0/1/2**: progressively stronger AI with custom grip/brake/steering tables

---

## 9. Race Flow & Timing

#### `AdvancePendingFinishState` (0x40a2b0)
Post-finish countdown processor. When a racer's slot state is 1 (racing) and the finish gate is clear:
- Decrements the timer at actor+0x344 (byte pair: seconds:frames at 59 fps wrap)
- When timer reaches zero: stores finish metrics at +0x328/+0x334/+0x336, promotes state to 2 (finished)
- `+0x334`: effective average speed (from total distance / time)
- `+0x336`: derived scoring metric (sub-progress * 1500 / speed)
- Network play gate: `DAT_004b0fa8` controls whether the 2-frame-per-tick decrement applies

#### `AccumulateVehicleSpeedBonusScore` (0x40a3d0)
Per-frame speed bonus accumulator at actor+0x2c8:
- Only accumulates when not finished (+0x328 == 0), not crashed (+0x376 == 0), speed > 0 (+0x318), and every 4th frame (`DAT_004aac5c & 3 == 0`)
- Score increment: `(speed >> 15) - (damage >> 1)`, clamped to 0 if negative or damage > 15
- Player 0 only: zeroed if behind the wrong-way checkpoint threshold

#### `DecayUltimateVariantTimer` (0x40a440)
For "Ultimate" race variant (variant == 4): decrements the score accumulator at +0x2c8 by `param_2` per frame, clamped to 0. This creates a "use it or lose it" scoring pressure.

#### `AdjustCheckpointTimersByDifficulty` (0x40a530)
Scales checkpoint time limits by difficulty:
- **Tier 0** (easiest): all checkpoint times *= 12/10 (120%)
- **Tier 1** (medium): all checkpoint times *= 11/10 (110%)
- **Tier 2+**: no scaling (100%)
- Only applies to player 0 (slot check at +0x375)
- Iterates the checkpoint table at `DAT_004aed88`: byte 0 = count, then 4-byte entries with time at offset +2
- Resets actor finish state fields: +0x344 (timer), +0x37e (flag), +0x328 (finish marker), +0x34c (lap counter)

#### `ResetPlayerVehicleControlAccumulators` (0x402e30)
Clears the 2-dword accumulator pair at `0x483014`/`0x483018` (steering and throttle integration buffers).

#### `ResetPlayerVehicleControlBuffers` (0x402e40)
Zeros the 6-dword `gPlayerControlBuffer` at `0x482fe4` — the raw input state buffer consumed by `UpdatePlayerVehicleControlState`.

---

## 10. Race Misc

#### `GetDamageRulesStub` (0x42c8d0)
Stub — always returns 0. Referenced by collision and wanted-damage code. Likely a disabled damage-rules hook (arcade vs simulation toggle that was cut).

#### `IsLocalRaceParticipantSlot` (0x42cbe0)
```c
if (network_active)  return dpu_exref[0xbcc + slot*4];  // network participant table
if (split_screen)    return (slot < 2);                   // both players local
return (slot == 0);                                       // single player
```

#### `BeginRaceFadeOutTransition` (0x42cc20)
Triggers the end-of-race fade:
- If `param_1 == 1` and race just completed (DAT_004aaee8 == 1) and not already fading: triggers the radial pulse overlay reset and sets `DAT_004aaefc = 1`
- Sets `g_raceEndFadeState = 1`
- Fade direction selection:
  - Fullscreen (layout 0): alternates 0/1 each race
  - Horizontal split (layout 1): always direction 0 (top-to-bottom)
  - Vertical split (layout 2): always direction 1 (left-to-right)

#### `StoreRoundedVector3Ints` (0x42ccd0)
Converts 3 floats (from FPU stack) to rounded integers via `__ftol()` and stores them in a 3-int output array. Used for world-position snapping.

#### `GameWinMain` (0x430a90)
Application entry point after CRT:
1. `DXWin::Environment(lpCmdLine)` — parse command-line tokens
2. Set default resolution: 640x480x16
3. Store hInstance, nCmdShow, set `iRam00060114 = 1` (main loop enabled)
4. `DXWin::Initialize()` — create window, DirectDraw
5. Main message pump loop:
   - `PeekMessage` drain (WM_QUIT check at msg 0x12)
   - Accelerator translation
   - If `DAT_00473b6c` (device-lost flag): `DXDraw::ConfirmDX6()` then `DXWin::DXInitialize()` + `InitializeRaceVideoConfiguration()`
   - Otherwise: `RunMainGameLoop()` if no errors
6. Cleanup: `DXWin::Uninitialize()`, `DestroyWindow()`
7. Returns `DAT_004aee30` (exit code)

#### `ResetGameHeap` (0x430cb0)
Destroys and recreates the game heap:
```c
if (DAT_004aee4c) HeapDestroy(gGameHeapHandle);
gGameHeapHandle = HeapCreate(0, 24000000, 0);  // 24 MB initial, growable
gGameHeapAllocTotal = 0;
```

#### `ParseAndDecodeCompressedTrackData` (0x430d30)
Texture/mipmap generator from compressed track data. Processes a descriptor block:
- Extracts per-channel bit masks from `param_1 + 0x0C..0x18` (RGBA channel masks)
- Computes shift/width for each channel by counting trailing zeros and consecutive ones
- Determines pixel size: 16-bit if total bits <= 16, 32-bit otherwise
- Iterates mip levels from `param_1+0x24` down to `param_1+0x20`:
  - For each level, reads dimensions from `DAT_00473b70[level]`
  - Double-nested loop over width x height pixels
  - Converts floating-point color samples to integer RGBA
  - Clamps each channel to 255
  - Packs into 16-bit or 32-bit pixel using the extracted shift/mask values
- Returns total bytes written

This is the software mipmap chain builder — it converts the engine's internal float color representation into the D3D surface pixel format determined at runtime from the device caps.

---

## 11. Complete Race Render Frame Flow

```
InitializeRaceRenderGlobals()     -- once at boot (FPU, transform buffer, trig tables)
InitializeRaceRenderState()       -- once per race (translucent pipeline, projected prims)
InitializeRaceViewportLayout()    -- once per race (viewport rects from render dimensions)
InitializeRaceActorRuntime()      -- once per race (AI params, difficulty scaling)
QueryRaceTextureCapacity()        -- once per race (D3D texture budget query)

Per frame:
  ConfigureRaceFogColorAndMode()  -- set fog color/enable from track data
  BeginRaceScene()                -- D3D BeginScene + reset texture sort keys

  For each viewport (1 or 2 in split-screen):
    SetRaceRenderStatePreset(0)   -- opaque pass: aniso filter, alpha off
    [Track geometry rendering]     -- spans via GetTrackSpanDisplayListEntry

    RenderRaceActorsForView(v)    -- vehicles: mesh transform/light/render + traffic
      LoadRenderRotationMatrix()  -- set 3x3 from actor orientation
      LoadRenderTranslation()     -- set translation from actor position
      PushRenderTransform()       -- save before overlay modifications
      BuildSpecialActorOverlayQuads()  -- billboard geometry
      PopRenderTransform()        -- restore
      BuildSpriteQuadTemplate()   -- wheel/smoke sprite quads

    SetRaceRenderStatePreset(1/2) -- translucent pass: alpha on
    FlushQueuedTranslucentPrimitives()  -- sorted translucent batch dispatch
    FlushImmediateDrawPrimitiveBatch()  -- immediate vertex buffer submit
    ApplyRaceFogRenderState(0/1)  -- fog on/off per pass

  [HUD rendering via FlushProjectedPrimitiveBuckets]

  EndRaceScene()                  -- D3D EndScene + texture age advancement

ReleaseRaceRenderResources()      -- race end: release textures, clear state
```

---

## 12. Key Global Addresses

| Address | Type | Description |
|---------|------|-------------|
| `0x4BF6B8` | int* | Current render transform (3x4 float matrix, 8-byte aligned) |
| `0x4C36C8` | float[12] | Render transform push/pop backup slot |
| `0x48DA00` | uint | Current texture page sort key |
| `0x48DA04` | uint | Previous texture page sort key |
| `0x48DC3C` | int* | Texture cache control block pointer |
| `0x48DC40` | int* | Texture cache descriptor array base |
| `0x48DC48` | float | Y-offset for billboard/overlay quads |
| `0x48DBA0` | int | Race render state active flag (0/1) |
| `0x48DBA8` | byte | Render globals initialized flag |
| `0x48DB90` | uint | Saved FPU control word (clean) |
| `0x48DB94` | uint | Saved FPU control word (modified, single-precision) |
| `0x4AEE68` | uint[1024] | Luminance-to-ARGB color lookup table |
| `0x4AF26C` | int | Translucent batch linked-list pool base |
| `0x4AF270` | int | Active translucent batch count |
| `0x4AF314` | ushort[] | Immediate draw index buffer |
| `0x4AFB14` | byte[] | Immediate draw vertex buffer |
| `0x4AFB4C` | int | Immediate draw vertex count |
| `0x4AFB50` | int | Immediate draw index count |
| `0x4AAE14` | struct[3] | Viewport layout table (0x40B bytes/entry) |
| `0x473DB0` | uint[32] | Base AI config template (copied per AI slot) |
| `0x473B70` | int[] | Mip level dimension table |
| `0x4749D0` | float | World-to-texel coordinate scale |
| `0x463B70` | float | Track-relative billboard scale constant |
| `0x463B6C` | float | Billboard midpoint averaging factor (0.5) |
