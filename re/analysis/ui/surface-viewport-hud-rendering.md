# Surface Creation, Viewport Setup, HUD & Font Rendering Deep Dive

> Reverse-engineered from TD5_d3d.exe via Ghidra (port 8195).
> All addresses are base-0x400000 virtual addresses.

---

## 1. Render Resolution Globals (0x4AAF00-0x4AAF90 cluster)

### Core Resolution Storage

| Address | Name | Type | Description |
|---------|------|------|-------------|
| 0x4AAF08 | `gRenderWidth` | float | Active render width (set to 640.0 by default) |
| 0x4AAF0C | `gRenderHeight` | float | Active render height (set to 480.0 by default) |
| 0x4AAF10 | `gRenderCenterX` | float | = gRenderWidth * 0.5 (320.0) |
| 0x4AAF14 | `gRenderCenterY` | float | = gRenderHeight * 0.5 (240.0) |
| 0x4AAF60 | `g_subTickFraction` | float | 0-1 interpolation factor (47 xrefs, all READ from camera/render) |
| 0x4AAF00 | `DAT_004aaf00` | int | Actor count (used by minimap marker loop) |

### Writers of gRenderWidth/gRenderHeight

Only **two functions** write to these globals:

1. **`InitializeRaceVideoConfiguration` (0x42A990)** - The primary writer:
   ```c
   gRenderWidth = 640.0;
   gRenderHeight = 480.0;
   ConfigureProjectionForViewport(0x280, 0x1e0);  // 640, 480
   gRenderCenterX = gRenderWidth * 0.5;
   gRenderCenterY = gRenderHeight * 0.5;
   InitializeRaceViewportLayout();
   ```

2. **`InitializeRaceSession` (0x42A530)** - Also writes these (likely redundant or for network sessions).

### Readers of gRenderWidth (0x4AAF08) - 24 xrefs

Key consumers:
- `InitializeRaceViewportLayout` (0x42C2B0) - 3 reads
- `InitializeRaceHudLayout` (0x437BA0) - 7 reads
- `InitializeRaceOverlayResources` (0x437A20) - 2 reads
- `RenderRaceHudOverlays` (0x4388A0) - 1 read
- `RenderHudRadialPulseOverlay` (0x439E60) - 1 read
- `RunRaceFrame` (0x42B580) - 4 reads (fade/letterbox)
- `SpawnAmbientParticleStreak` - 1 read
- `UpdateWantedDamageIndicator` - 1 read

### Readers of gRenderHeight (0x4AAF0C) - 23 xrefs

Key consumers:
- `InitializeRaceViewportLayout` (0x42C2B0) - 4 reads
- `InitializeRaceHudLayout` (0x437BA0) - 7 reads
- `InitializeRaceOverlayResources` (0x437A20) - 2 reads
- `RenderRaceHudOverlays` (0x4388A0) - 1 read
- `InitializeMinimapLayout` (0x43B0A0) - 1 read
- `RunRaceFrame` (0x42B580) - 4 reads

**Critical finding**: ALL downstream code reads from `gRenderWidth`/`gRenderHeight` dynamically. The 640x480 hardcode is only in `InitializeRaceVideoConfiguration`. Patching those two writes + the `ConfigureProjectionForViewport(0x280, 0x1e0)` call is the **single choke point** for widescreen race rendering.

---

## 2. Viewport Layout Table (0x4AAE14)

### Structure

`InitializeRaceViewportLayout` (0x42C2B0) populates a table of **3 viewport layout entries** at base address `gRaceViewportLayouts` (0x4AAE14). Each entry is 0x40 bytes (16 floats/ints).

The table is indexed by `gRaceViewportLayoutMode`:
- **0** = Single-screen (full viewport)
- **1** = Left/right split-screen
- **2** = Top/bottom split-screen

### Per-Entry Fields (reconstructed from InitializeRaceViewportLayout)

For layout mode 0 (single screen), using `W = gRenderWidth`, `H = gRenderHeight`, `CX = gRenderCenterX`, `CY = gRenderCenterY`:

| Offset | Address | Value | Description |
|--------|---------|-------|-------------|
| +0x00 | 0x4AAE14 | `gRaceViewportLayouts` = 0 | Clip left (viewport min X) |
| +0x04 | 0x4AAE18 | `CX` (320.0) | Projection center X |
| +0x08 | 0x4AAE1C | ? | |
| +0x0C | 0x4AAE20 | `W` (640.0) | Clip right (viewport max X) |
| +0x14 | 0x4AAE28 | 0 | Clip top |
| +0x1C | 0x4AAE30 | `CY` (240.0) | Projection center Y |
| +0x24 | 0x4AAE38 | `H` (480.0) | Clip bottom |
| +0x2C | 0x4AAE40 | 0x40 (64) | Track span cull window size |
| +0x30 | 0x4AAE44 | 0x40 (64) | Computed track span count |
| +0x34 | 0x4AAE48 | 1 | Number of viewports in this mode |

For layout mode 1 (left/right split):
- Left viewport: clip 0 to CX-1, center at CX*0.5
- Right viewport: clip CX+1 to W, center at CX*1.5
- Track span cull window = 0x20 (32), viewport count = 2

For layout mode 2 (top/bottom split):
- Top viewport: clip 0 to CY-1, center at CY*0.5
- Bottom viewport: clip CY+1 to H, center at CY*1.5
- Track span cull window = 0x20 (32), viewport count = 2

### Usage in RunRaceFrame (0x42B580)

RunRaceFrame reads the viewport layout table each frame:
```c
iVar2 = (int)gRaceViewportLayoutMode;
SetProjectedClipRect(
    gRaceViewportLayouts[iVar2*16],    // clip left
    DAT_004aae20[iVar2*16],            // clip right
    DAT_004aae28[iVar2*16],            // clip top
    DAT_004aae38[iVar2*16]             // clip bottom
);
SetProjectionCenterOffset(
    DAT_004aae18[iVar2*16],            // center X
    DAT_004aae30[iVar2*16]             // center Y
);
```

For split-screen player 2, it uses the second entry in the layout:
```c
SetProjectedClipRect(
    *(float *)(&DAT_004aae14 + iVar2),  // 0x40 offset for mode 1/2
    *(float *)(&DAT_004aae24 + iVar2),
    *(float *)(&DAT_004aae2c + iVar2),
    *(float *)(&DAT_004aae3c + iVar2)
);
```

**This entire layout system is already dynamic** - it derives everything from gRenderWidth/gRenderHeight.

---

## 3. Projection System

### ConfigureProjectionForViewport (0x43E7E0)

The core projection setup function. Called with integer viewport pixel dimensions:

```c
void ConfigureProjectionForViewport(int width, int height)
{
    float fW = (float)width;
    float fH = (float)height;

    // THE 4:3 ASSUMPTION: projection depth = width * 0.5625
    // 0.5625 = 9/16 ... this maps a 640-wide viewport to projection depth 360
    // For 4:3: tan(HFOV/2) = 320/360 => HFOV ~ 83.6 degrees
    _DAT_004c3718 = fW * 0.5625;      // projection depth (focal length analog)
    _DAT_004c371c = 1.0 / _DAT_004c3718;  // inverse projection depth

    // Frustum clip plane normals for left/right
    float diag = sqrt(fW*fW*0.25 + _DAT_004c3718*_DAT_004c3718);
    _DAT_004ab0b0 = _DAT_004c3718 / diag;  // cos(half-HFOV)
    _DAT_004ab0b8 = -(fW / (diag*2));      // sin(half-HFOV)

    // Frustum clip plane normals for top/bottom
    float diagV = sqrt(fH*fH*0.25 + _DAT_004c3718*_DAT_004c3718);
    _DAT_004ab0a4 = _DAT_004c3718 / diagV;
    _DAT_004ab0a8 = -(fH / (diagV*2));

    _DAT_004c3700 = fW;   // cached viewport width
    _DAT_004c3704 = fH;   // cached viewport height

    SetClipBounds(0.0, fW, 0.0, fH);
    SetProjectedClipRect(0.0, fW, 0.0, fH);
}
```

**Key constant**: `0.5625` (the projection scale) assumes 4:3. For widescreen, this ratio should change to maintain correct vertical FOV while widening the horizontal FOV.

### Supporting Projection Functions

| Address | Function | Purpose |
|---------|----------|---------|
| 0x43E640 | `SetProjectedClipRect` | Clamps clip rect to clip bounds, caches half-extents |
| 0x43E8E0 | `SetProjectionCenterOffset` | Stores projection center (0x4C3710, 0x4C3714) |
| 0x43E900 | `RecomputeTracksideProjectionScale` | Re-derives frustum from cached viewport dims + modified projection depth |
| 0x42D410 | `FinalizeCameraProjectionMatrices` | Normalizes camera basis by depth factor 0x467368 |

### Projection Globals

| Address | Name | Description |
|---------|------|-------------|
| 0x4C3718 | Projection depth | = viewportWidth * 0.5625 |
| 0x4C371C | Inverse projection depth | = 1.0 / projDepth |
| 0x4C3700 | Cached viewport width | From ConfigureProjectionForViewport |
| 0x4C3704 | Cached viewport height | From ConfigureProjectionForViewport |
| 0x4C3710 | Projection center X | Set by SetProjectionCenterOffset |
| 0x4C3714 | Projection center Y | Set by SetProjectionCenterOffset |
| 0x4AFB20-3C | Clip rect | Left/Right/Top/Bottom from SetProjectedClipRect |
| 0x4AFB38-44 | Clip bounds | Outer bounds from SetClipBounds |

### BuildSpriteQuadTemplate (0x432BD0)

All HUD/overlay sprites go through this function. It uses `_DAT_004c371c` (1/projectionDepth) to transform screen-space sprite coordinates into the software projection space:
```c
vertex.x = screenX * invProjDepth * alpha;
vertex.y = invProjDepth * screenY * alpha;
vertex.z = alpha;
```

This means sprites are already scaled by the projection depth. When the viewport changes, sprite positioning adapts automatically **IF** the sprite coordinates fed in are computed from gRenderWidth/gRenderHeight.

---

## 4. Font Rendering Pipeline

### Two Completely Separate Systems

#### A. Frontend Font System (Menu/UI - DirectDraw Blit)

The frontend uses **bitmap font atlases** loaded as DirectDraw surfaces from TGA files in `FrontEnd.zip`:

| Global | Surface | Usage |
|--------|---------|-------|
| 0x49626C | `DAT_0049626c` | Primary font (BodyText.tga or SmallText2.tga depending on locale) |
| 0x496270 | `DAT_00496270` | SmallText.tga |
| 0x496274 | `DAT_00496274` | SmallTextb.tga |
| 0x496278 | `DAT_00496278` | MenuFont.tga (large decorative) |
| 0x49627C | `DAT_0049627c` | Secondary reference (= DAT_00496270) |

**Rendering path**: Characters are blitted using DirectDraw `Blt` (vtable offset 0x1C on the surface COM interface). The per-character advance widths are stored at:
- `DAT_004662D0` + charcode: advance width per character (variable width, 4-13 pixels)
- `DAT_004663E4` + charcode: vertical offset per character (baseline adjustments)
- `DAT_004662F0` + charcode: trailing width for last-char measurement

The font atlas is laid out as a **21-column grid** with 12x12 pixel cells:
```c
srcX = ((charcode - 0x20) % 21) * 12;
srcY = ((charcode - 0x20) / 21) * 12;
```

**Fixed pixel size**: 12x12 cells, variable advance width per glyph. These fonts are NOT scalable - they are fixed-pixel bitmap blits at the DirectDraw surface level.

Frontend font functions:
| Address | Function | Target Surface |
|---------|----------|----------------|
| 0x424110 | `DrawFrontendFontStringPrimary` | Primary work surface (DAT_00496260) |
| 0x4241E0 | `DrawFrontendFontStringSecondary` | Secondary work surface (DAT_00496264) |
| 0x424470 | `DrawFrontendFontStringToSurface` | Arbitrary surface |
| 0x424660 | `DrawFrontendSmallFontStringToSurface` | Arbitrary surface (small font) |
| 0x424740 | `DrawFrontendClippedStringToSurface` | Clipped, with control codes |
| 0x412D50 | `MeasureOrDrawFrontendFontString` | MenuFont atlas (0x496278), 36x36 cells, 7-column grid |

#### B. Race HUD Text System (In-Race - Software Projected Sprites)

The race HUD uses a completely different system: **sprite-based text** rendered through the software projection pipeline.

**`InitializeRaceHudFontGlyphAtlas` (0x428240)**:
- Allocates 0xB800 bytes for glyph primitive storage (DAT_004a2cbc)
- Allocates 0x404 bytes for glyph UV/size table (DAT_004a2cb8)
- Seeds a **4-row x 16-column** glyph grid with:
  - Cell UV origin from archive entry offsets
  - Glyph width = 8.0, height = 12.0 (most glyphs)
  - Column spacing = 10.0 pixels in the texture atlas
  - Row spacing = 16.0 pixels in the texture atlas
  - Special overrides: space = 4.0 width, some punctuation = 7.0

**`QueueRaceHudFormattedText` (0x428320)**:
- Uses `wvsprintfA` for printf-style formatting
- Maps characters through a remap table at `DAT_004669f4`
- For each glyph: reads UV rect from glyph table, builds a sprite quad via `BuildSpriteQuadTemplate`
- Queues up to 0x101 glyphs per frame
- Glyph sprites use screen-space coordinates (pixel positions from the HUD layout)

**`FlushQueuedRaceHudText` (0x428570)**:
- Iterates all queued glyph sprites
- Calls `SubmitImmediateTranslucentPrimitive` for each (0xB8 bytes per primitive)
- Resets queue counter

Race HUD text is **NOT** resolution-independent by itself - but the pixel positions are computed from scaled layout parameters, so it scales correctly if the layout is rebuilt with new dimensions.

---

## 5. HUD Element Positioning

### InitializeRaceHudLayout (0x437BA0) - The Master HUD Layout Builder

This function computes ALL HUD element positions from two base scale factors:

```c
DAT_004b1138 = gRenderWidth * 0.0015625;   // = width/640 = horizontal scale
DAT_004b113c = gRenderHeight * 0.0020833334; // = height/480 = vertical scale
```

For **single-screen** (param_1 == 0):
```c
DAT_004b1148 = gRenderWidth * -0.5;   // left edge
DAT_004b1150 = gRenderWidth * 0.5;    // right edge
DAT_004b114c = gRenderHeight * -0.5;  // top edge
DAT_004b1154 = gRenderHeight * 0.5;   // bottom edge
```

For **left/right split** (param_1 == 1): Horizontal scale halved, split at center.
For **top/bottom split** (param_1 == 2): Vertical scale halved, split at center.

The function then computes derived positions by multiplying the scale factors by fixed pixel offsets designed for 640x480. Example for the speedometer:
```c
speedo_left = (rightEdge - hScale * 96.0) - hScale * 16.0;
speedo_top  = (bottomEdge - vScale * 96.0) - vScale * 8.0;
speedo_right = speedo_left + hScale * 96.0;
speedo_bottom = speedo_top + vScale * 96.0;
```

### HUD Elements Built by InitializeRaceHudLayout

Each element is a pre-built sprite quad template:

| Element | Archive Name | Size (in scale units) | Position anchor |
|---------|-------------|----------------------|-----------------|
| Speedometer | "SPEEDO" | 96x96 | Bottom-right |
| Speed digits (x3) | "SPEEDOFONT" | 15xN each | Right of speedo |
| Gear indicator | "GEARNUMBERS" | 32x16 | Below speedo |
| Metric digits (x4) | "numbers" | 15x23 each | Top center |
| Semicolon separator | "SEMICOL" | dynamic | Between digits |
| U-turn indicator | "UTURN" | 64x64 | Center screen |
| Replay indicator | "REPLAY" | 60x60 | Top-left offset |

All positions are in **projection-space coordinates** (centered on 0,0), not absolute pixel coordinates. The scale factors (width/640, height/480) make everything proportional.

### RenderRaceHudOverlays (0x4388A0)

Per-frame HUD render pass:
1. Iterates each viewport (1 or 2 depending on split-screen)
2. For each viewport, sets `DAT_004b0fa4` = pointer to that viewport's layout data
3. Renders position labels, lap times, split times based on bitmask flags in `DAT_004b0bfc`
4. Renders speedometer needle, gear indicator, speed digits via `SubmitImmediateTranslucentPrimitive`
5. Renders U-turn warning (flashing), replay indicator
6. After all viewports: renders minimap (single-screen only), flushes text queue, renders radial pulse overlay
7. For split-screen: renders the divider bar primitive

HUD flag bits (in `*DAT_004b0bfc`):
| Bit | Purpose |
|-----|---------|
| 0x001 | Position label |
| 0x002 | Lap times |
| 0x004 | Speedometer + needle + gear + digits |
| 0x008 | ??? |
| 0x010 | U-turn warning check |
| 0x020 | ??? |
| 0x040 | Metric digits (FPS/speed/etc) |
| 0x080 | Format string line 1 |
| 0x100 | Format string line 2 |
| 0x200 | Format string line 3 |
| 0x80000000 | Replay indicator |

---

## 6. Minimap System

### InitializeMinimapLayout (0x43B0A0)

Minimap dimensions are derived from the HUD scale:
```c
DAT_004b1130 = DAT_004b1138 * 100.0;  // minimap width = hScale * 100
DAT_004b11b8 = DAT_004b113c * 100.0;  // minimap height = vScale * 100
_DAT_004b1128 = DAT_004b1138 * 7.0;   // marker size

DAT_004b0a40 = DAT_004b1138 * 8.0;    // minimap X origin (left margin)
DAT_004b0a44 = (gRenderHeight - minimapH) - DAT_004b113c * 8.0;  // Y origin (bottom-anchored)

// Minimap coordinate scale factors
_DAT_004b0a48 = DAT_004b1138 * 0.0009765625;  // hScale / 1024
_DAT_004b0a4c = DAT_004b113c * 0.0009765625;  // vScale / 1024
```

The minimap is fully dynamic - position, size, and coordinate mapping all derive from the render dimensions.

### RenderTrackMinimapOverlay (0x43A220)

- Sets a dedicated clip rect for the minimap area
- Renders background tiles (16 tiles, 4x4 grid)
- Renders track strip geometry as transformed quads
- Renders per-actor markers with rotation-compensated positions
- Only renders in single-screen mode (`gRaceViewportLayoutMode == 0`)

---

## 7. DirectDraw Surface Creation

### Frontend Surfaces

**`InitializeFrontendResourcesAndState` (0x414740)** hard-codes frontend virtual resolution:
```c
DAT_00495228 = 0x280;  // 640 = frontend virtual width
DAT_00495200 = 0x1e0;  // 480 = frontend virtual height
// ...
DAT_00496260 = CreateTrackedFrontendSurface(640, 480);  // primary work surface
DAT_00496264 = CreateTrackedFrontendSurface(640, 480);  // secondary work surface
```

These two globals (0x495228, 0x495200) are referenced by **121+ and 109+** xrefs respectively - they are the frontend's virtual screen dimensions used by every frontend rendering function for clipping and layout.

**`CreateTrackedFrontendSurface` (0x411F00)**:
- Creates a DirectDraw off-screen surface via `IDirectDraw::CreateSurface` (vtable +0x18)
- Uses `DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH` flags
- Falls back to system memory caps (0x840) if video memory fails
- Registers surface in a tracking table at 0x494FD0 (max ~256 entries)

### Race/Video Surfaces

**`CreateVideoSurfaces` (0x4552B0)**:
- Creates the main video playback surface
- Supports multiple paths: overlay surface (YUV), DDrawSurface32 (ARGB), DDrawSurface16
- Falls back through overlay -> 32-bit -> 16-bit

**`CreateVideoOutputSurface` (0x453380)**:
- Creates the final output presentation surface
- Uses `IDirectDrawSurface::UpdateOverlay` (vtable +0x84) for video overlay path

**`CreateDDrawSurface32` (0x454170)** / **`CreateDDrawSurface16` (0x455770)**:
- Create off-screen surfaces with specific pixel formats
- 32-bit: DDPF_RGB | DDPF_ALPHAPIXELS (32bpp ARGB)
- 16-bit: DDPF_FOURCC with UYVY/YUY2 format codes

### Surface Utility Functions

| Address | Function | Purpose |
|---------|----------|---------|
| 0x453C10 | `QuerySurfaceDimensions` | Reads DDSURFACEDESC.dwWidth/dwHeight |
| 0x453F70 | `QuerySurfacePixelFormat` | Reads DDPIXELFORMAT |
| 0x456210 | `DDrawGetSurfaceCaps` | Reads DDSCAPS |
| 0x4562A0 | `DDrawReleaseSurface` | Calls IDirectDrawSurface::Release |
| 0x455870 | `RestoreLostSurfaces` | Calls IsLost/Restore on primary + secondaries |

---

## 8. Screen Clearing and Fill Operations

### ClearBackbufferWithColor (0x423DB0)

Fills the entire primary work surface:
```c
void ClearBackbufferWithColor(uint color)
{
    // Convert 24-bit RGB to 15-bit or 16-bit depending on DAT_0049525c
    DDBLTFX fx = { .dwSize = 100, .dwFillColor = converted };
    DAT_00496260->Blt(NULL, NULL, NULL, DDBLT_COLORFILL, &fx);
}
```

### FillPrimaryFrontendRect (0x423ED0) / FillSurfaceRectWithColor (0x423F90) / BltColorFillToSurface (0x424050)

All three share the same pattern:
1. Build a RECT from x, y, width, height
2. Convert 24-bit RGB color to current pixel format (15-bit 5:5:5 or 16-bit 5:6:5)
3. Call `IDirectDrawSurface::Blt` with `DDBLT_COLORFILL` flag

The pixel format discriminator is at `DAT_0049525c`:
- 0x0F = 15-bit (5:5:5)
- 0x10 = 16-bit (5:6:5)

### LockSecondaryFrontendSurfaceFillColor (0x423E40)

Locks the secondary surface and writes a fill color directly to pixel memory (no Blt).

---

## 9. Frontend Presentation / Blit Pipeline

### Frontend Sprite System

The frontend uses a **deferred sprite blit queue**:

1. **`QueueFrontendSpriteBlit` (0x425730)**: Clips source/dest rects against frontend virtual screen (DAT_00495228 x DAT_00495200), converts colors, stores entry in table at 0x497AD0. Max 65 sprites (0x41).

2. **`FlushFrontendSpriteBlits` (0x425540)**:
   - Restores previous frame's saved backgrounds (double-buffered dirty-rect system)
   - Calls `RenderFrontendDisplayModeHighlight`
   - Iterates sprite queue, saves background, blits each sprite

3. **`PresentPrimaryFrontendBuffer` (0x424CA0)**: Blits full primary surface to presentation target via `IDirectDrawSurface::Blt`.

### Blit Geometry

**`ComputeBlitGeometry` (0x454830)**: Computes presentation blit source/dest rectangles from video output configuration flags:
- Bit 0x40: double width
- Bit 0x30: double height
- Bit 0x04: center horizontally
- Bit 0x08: center vertically
- Bit 0x80: override width
- Bit 0x100: override height

---

## 10. Loading Screen

**`DisplayLoadingScreenImage` (0x42CA00)**:

Hard-codes 640x480:
```c
if ((iVar4 < 0x280) && (iVar6 < 0x4B000)) {  // 640 and 640*480
    dest[x] = src[x];  // copy pixel
} else {
    dest[x] = 0;  // black fill
}
iVar6 += 0x280;  // advance by 640 pixels
```

This function locks the primary DirectDraw surface directly and copies a decoded TGA line-by-line with hardcoded 640-pixel row stride and 0x4B000 (307200 = 640*480) total pixel limit.

---

## 11. Key Widescreen Considerations Summary

### What's Already Dynamic
- Viewport layout table (InitializeRaceViewportLayout)
- HUD layout (InitializeRaceHudLayout) - uses width/640 and height/480 scale factors
- Minimap (InitializeMinimapLayout) - fully derived from scale factors
- Projection system (ConfigureProjectionForViewport) - derives frustum from passed dimensions
- Clip bounds and clip rects
- Split-screen divider bars
- Race status text positioning
- Radial pulse overlay
- All sprite quad positions (via BuildSpriteQuadTemplate)

### What's Hardcoded to 640x480
1. **`InitializeRaceVideoConfiguration`** (0x42A990): `gRenderWidth=640.0; gRenderHeight=480.0; ConfigureProjectionForViewport(0x280,0x1e0);` - **THE primary choke point**
2. **`InitializeFrontendResourcesAndState`** (0x414740): `DAT_00495228=0x280; DAT_00495200=0x1e0;` - Frontend virtual resolution (121+ xrefs)
3. **`DisplayLoadingScreenImage`** (0x42CA00): Hard-coded 0x280 stride and 0x4B000 size
4. **`ConfigureProjectionForViewport`**: The `0.5625` factor assumes 4:3 aspect ratio
5. **Frontend font atlases**: Fixed 12x12 pixel bitmap glyphs (cannot scale)

### What Would Need Changing for Widescreen
1. Patch gRenderWidth/gRenderHeight writes in InitializeRaceVideoConfiguration
2. Patch ConfigureProjectionForViewport call args (or adjust the 0.5625 projection scale)
3. Patch frontend virtual resolution (DAT_00495228/DAT_00495200) + re-create surfaces
4. Patch DisplayLoadingScreenImage stride/limit
5. Frontend font rendering will remain at native pixel size (acceptable, just small on high res)
6. Race HUD text uses projected sprites and will scale automatically with the HUD layout
