# Hardcoded 640x480 Resolution Audit — TD5_d3d.exe

> Comprehensive audit of every hardcoded resolution value in the EXE.
> All addresses from Ghidra on port 8195.

---

## CRITICAL: Primary Resolution Setters

These are the root causes — they SET gRenderWidth/gRenderHeight to 640x480.

### 1. InitializeRaceVideoConfiguration (0x0042A950) — RACE BOOTSTRAP
**Context**: Race code. Called when entering a race from the frontend.
```c
gRenderWidth = 640.0;          // float literal
gRenderHeight = 480.0;         // float literal
ConfigureProjectionForViewport(0x280, 0x1e0);  // int 640, 480
gRenderCenterX = gRenderWidth * 0.5;
gRenderCenterY = gRenderHeight * 0.5;
```
**Impact**: This is the PRIMARY hardcoded 640x480 for race rendering. However, note that `InitializeRaceSession` (see below) OVERWRITES these values with the actual display mode resolution later. So this function's hardcoded values serve as defaults/fallback.

### 2. InitializeRaceVideoConfiguration (0x00442160) — DUPLICATE
**Context**: Identical duplicate of the above at a different address. Same hardcoded values.

### 3. InitializeRaceSession (0x0042AA10) — RACE SESSION INIT
**Context**: Race code. The real resolution is set here from the selected display mode:
```c
gRenderWidth = (float)iRam00060048;    // reads actual display width
gRenderHeight = (float)iRam0006004c;   // reads actual display height
gRenderCenterX = gRenderWidth * 0.5;
gRenderCenterY = gRenderHeight * 0.5;
ConfigureProjectionForViewport((int)gRenderHeight, (int)gRenderWidth);  // NOTE: reversed args in decompilation
```
**Impact**: This reads the display mode dimensions from the DX::app struct (0x60048, 0x6004c). If the display mode is 640x480, these will be 640x480. If a higher resolution mode is selected, these WILL be higher. This is the key dynamic path for race rendering.

---

## CRITICAL: Frontend Virtual Resolution (Always 640x480)

### 4. InitializeFrontendResourcesAndState (0x00414740) — FRONTEND BOOTSTRAP
**Context**: Frontend/menu code.
```c
DAT_00495228 = 0x280;   // frontend width = 640
DAT_00495200 = 0x1e0;   // frontend height = 480
```
Then creates two work surfaces at that size:
```c
DAT_00496260 = CreateTrackedFrontendSurface(DAT_00495228, DAT_00495200);  // 640x480
DAT_00496264 = CreateTrackedFrontendSurface(DAT_00495228, DAT_00495200);  // 640x480
```
**Impact**: The entire frontend menu system operates at a fixed 640x480 virtual resolution. ALL menu/screen functions reference `DAT_00495228` (width) and `DAT_00495200` (height) rather than hardcoded literals — so patching these two stores would propagate everywhere in the frontend.

### 5. RunFrontendDisplayLoop (0x00414B50) — FRONTEND MAIN LOOP
**Context**: Frontend/menu code.
```c
BlitFrontendCachedRect(0, 0, 0x280, 0x1e0);  // Blt rect: 640 wide, 480 tall
```
**Impact**: Hardcoded 640x480 Blt rectangle for refreshing the cached frontend background when surfaces are lost. Uses literal `0x280` and `0x1e0`.

---

## CRITICAL: Projection / Aspect Ratio

### 6. ConfigureProjectionForViewport (0x0043E7E0) — PROJECTION SETUP
**Context**: Race 3D rendering.
```c
_DAT_004c3718 = fVar1 * 0.5625;   // projection scale = width * 0.5625
```
The constant `0.5625` is `9/16` — this encodes a 4:3 field-of-view assumption. For 640x480: `640 * 0.5625 = 360`, which gives a vertical FOV relationship. This is NOT a resolution hardcode per se, but an aspect ratio assumption that needs adjusting for widescreen.

**Impact**: If you change render resolution to widescreen (e.g. 1920x1080), this `0.5625` constant will produce incorrect (stretched) perspective. For 16:9, this should be `0.5625 * (4/3) / (16/9) = 0.421875`, or better: computed as `height/width * (4/3) * 0.5625`.

---

## CRITICAL: Loading Screen / Benchmark Image Blits

### 7. DisplayLoadingScreenImage (0x0042CA00) — LOADING SCREEN
**Context**: Loading screen display (used before every race).
```c
if ((iVar4 < 0x280) && (iVar6 < 0x4b000)) {   // 640 width, 640*480=307200 total pixels
    // copy pixel
}
iVar6 = iVar6 + 0x280;   // stride = 640
```
**Impact**: Loading screen images are 640x480 TGAs. The copy loop hardcodes width=640 (`0x280`) and total pixel count=307200 (`0x4b000`). At higher resolutions, you'd need larger TGA assets AND these constants patched.

### 8. RunMainGameLoop case 3 (0x00442170) — BENCHMARK IMAGE DISPLAY
**Context**: Benchmark image display mode.
```c
if ((iVar4 < 0x280) && (iVar5 < 0x4b000)) {   // same 640/307200 pattern
    // copy pixel
}
iVar5 = iVar5 + 0x280;   // stride = 640
```
**Impact**: Same hardcoded 640x480 blit as the loading screen, but for the benchmark capture image path.

---

## DYNAMIC: HUD Layout (Scales from gRenderWidth/gRenderHeight)

### 9. InitializeRaceHudLayout (0x00437BA0) — HUD LAYOUT
**Context**: Race HUD code. Computes scale factors from render dimensions:
```c
DAT_004b1138 = gRenderWidth * 0.0015625;     // = width/640  (scale factor)
DAT_004b113c = gRenderHeight * 0.0020833334;  // = height/480 (scale factor)
```
The constants `0.0015625` (1/640) and `0.0020833334` (1/480) encode the base resolution. These produce a scale of 1.0 at 640x480, and >1.0 at higher resolutions. The rest of the HUD layout is computed from these scale factors.

**Impact**: While the HUD positions ARE dynamic (they use `gRenderWidth`/`gRenderHeight`), these two reciprocal constants encode 640 and 480 as the baseline. They should remain as-is for proportional scaling, but if you want the HUD to occupy less screen space at higher res (as opposed to scaling up), these would need adjustment.

---

## DYNAMIC: Viewport Layout (Fully Dynamic)

### 10. InitializeRaceViewportLayout (0x0042C2B0) — VIEWPORT TABLE
**Context**: Race code. Builds single-screen and split-screen viewport rectangles.
```
All values derived from gRenderWidth, gRenderHeight, gRenderCenterX, gRenderCenterY
```
**Impact**: FULLY DYNAMIC. Once gRenderWidth/gRenderHeight are correct, this function produces correct viewports. No hardcoded resolution constants.

---

## DYNAMIC: Race Overlay Resources (Fully Dynamic)

### 11. InitializeRaceOverlayResources (0x004377B0) — SPLIT-SCREEN DIVIDERS
**Context**: Race overlay code. Split-screen divider quads use:
```c
gRenderWidth * -0.5, gRenderHeight * -0.5, gRenderWidth * 0.5, gRenderHeight * 0.5
```
**Impact**: FULLY DYNAMIC. No hardcoded resolution values.

---

## DYNAMIC: Minimap Layout (Scales from HUD factors)

### 12. InitializeMinimapLayout (0x0043B0A0) — MINIMAP
**Context**: Race minimap. Uses the HUD scale factors (DAT_004b1138 / DAT_004b113c):
```c
DAT_004b1130 = DAT_004b1138 * 100.0;     // minimap width
DAT_004b11b8 = DAT_004b113c * 100.0;     // minimap height
DAT_004b0a44 = gRenderHeight - ... ;      // position from bottom
```
**Impact**: DYNAMIC (via HUD scale factors). Will scale proportionally with resolution.

---

## DYNAMIC: Pause Menu Overlay (Center-relative)

### 13. InitializePauseMenuOverlayLayout — PAUSE MENU
**Context**: Pause menu. Uses a computed half-width `DAT_004b11d4` for centering.
All positions are relative to center. No hardcoded 640/480.
**Impact**: FULLY DYNAMIC.

---

## PARTIALLY HARDCODED: Frontend Screen Functions

### 14. ScreenDisplayOptions (0x00420400) — DISPLAY OPTIONS MENU
**Context**: Frontend menu code.
```c
uVar3 = DAT_00495228 >> 1;   // half of frontend width (320 at 640)
uVar5 = DAT_00495200 >> 1;   // half of frontend height (240 at 480)
iVar4 = uVar3 - 0xd2;        // uVar3 - 210 (hardcoded pixel offset)
iVar6 = uVar5 - 0x9f;        // uVar5 - 159 (hardcoded pixel offset)
```
Also creates a fixed-size surface:
```c
CreateTrackedFrontendSurface(0xe0, 0x118);  // 224 x 280
```
**Impact**: Uses `DAT_00495228`/`DAT_00495200` for centering, but has hardcoded pixel offsets for menu item placement. These offsets assume 640x480 layout. The fixed 224x280 surface is for the options panel — it's content-sized, not resolution-dependent.

### 15. ScreenStartupInit (0x00415370) — STARTUP DIALOG
**Context**: Frontend code.
```c
CreateTrackedFrontendSurface(0x198, 0x70);   // 408 x 112 (content-sized)
QueueFrontendOverlayRect((DAT_00495228 >> 1) - 0xa8, (DAT_00495200 >> 1) - 0x8f, ...);
```
**Impact**: The surface size is content-specific (not resolution). Position uses `DAT_00495228`/`DAT_00495200` for centering.

### 16. ScreenPositionerDebugTool (0x00415030) — DEBUG TOOL
**Context**: Debug tool, not relevant for normal gameplay.
```c
FillPrimaryFrontendScanline(0x114, 0xff);   // scanline at y=276
FillPrimaryFrontendScanline(0x10c, 0xff);   // scanline at y=268
```
**Impact**: Debug only. Not relevant.

---

## DYNAMIC: Render Pipeline Functions (No hardcoded resolution)

The following functions were checked and contain NO hardcoded 640/480 values:

| Function | Address | Notes |
|----------|---------|-------|
| SetProjectedClipRect | 0x0043E640 | Takes float params, no hardcoded dims |
| SetClipBounds | 0x0043E750 | Takes float params, no hardcoded dims |
| Copy16BitSurfaceRect | 0x004251A0 | Operates on arbitrary rects |
| BltColorFillToSurface | 0x00424050 | Operates on arbitrary rects |
| FillPrimaryFrontendRect | 0x00423ED0 | Operates on arbitrary rects |
| FillSurfaceRectWithColor | 0x00423F90 | Operates on arbitrary rects |
| CreateTrackedFrontendSurface | 0x00411F00 | Takes w/h params |
| CreateVideoOutputSurface | 0x00453380 | Takes dimensions from caller |
| CreateVideoSurfaces | 0x004552B0 | Reads dimensions from DX struct |
| CreateDDrawSurface32 | 0x00454170 | Takes w/h params |
| CreateDDrawSurface16 | 0x00455770 | Takes w/h params |
| CreateDDrawOverlaySurface | 0x00455580 | Takes w/h params |
| PresentFrontendBufferSoftware | 0x00425360 | Uses DAT_00495228/DAT_00495200 (dynamic) |
| RenderFrontendFadeEffect | 0x00411780 | Uses DAT_00495228/DAT_00495200 (dynamic) |
| RenderFrontendFadeOutEffect | 0x00411A70 | Uses DAT_00495228/DAT_00495200 (dynamic) |
| FlushFrontendSpriteBlits | 0x00425540 | No hardcoded dims |
| RenderFrontendUiRects | 0x00425A30 | No hardcoded dims |
| CrossFade16BitSurfaces (both) | 0x0040CDC0, 0x0040D190 | Takes rect params |
| RenderRaceHudOverlays | 0x004388A0 | Uses HUD scale factors |
| RenderTrackMinimapOverlay | 0x0043A220 | Uses minimap layout data |
| RenderAmbientParticleStreaks | 0x00446560 | No hardcoded dims |
| InitializeWeatherOverlayParticles | 0x00446240 | No hardcoded dims |
| DrawRaceParticleEffects | 0x00429720 | No hardcoded dims |
| DrawRaceStatusText | 0x00439B70 | Uses HUD layout data |
| RenderVehicleWheelBillboards | 0x00446F00 | No hardcoded dims |
| InitializeRaceRenderState | 0x0040AE80 | No hardcoded dims |
| RunRaceFrame | 0x0042B580 | Uses gRenderWidth/gRenderHeight (dynamic) |
| ShowLegalScreens | 0x0042C8E0 | Calls DisplayLoadingScreenImage (see #7) |
| BuildEnumeratedDisplayModeList | 0x0040B100 | Reads DX mode table (dynamic) |
| SelectConfiguredDisplayModeSlot | 0x0040B170 | Maps menu ordinal to mode (dynamic) |
| QueueFrontendOverlayRect | 0x00425660 | Takes position params |

---

## Summary: What Must Be Patched

### Tier 1 — Required for native-resolution race rendering
| # | Address | What | Value | Patch Strategy |
|---|---------|------|-------|----------------|
| 1 | 0x42A950+ | gRenderWidth = 640.0 | float 640.0 | Replace with read from display mode, or remove (InitializeRaceSession overwrites) |
| 2 | 0x42A950+ | gRenderHeight = 480.0 | float 480.0 | Same as above |
| 3 | 0x42A950+ | ConfigureProjectionForViewport(0x280, 0x1e0) | int 640, 480 | Same as above |
| 4 | 0x442160+ | Duplicate of #1-3 | Same | Same |
| 5 | 0x43E7E0+ | Projection scale * 0.5625 | float 0.5625 | Adjust for non-4:3 aspect ratios |

### Tier 2 — Required for native-resolution frontend
| # | Address | What | Value | Patch Strategy |
|---|---------|------|-------|----------------|
| 6 | 0x414740+ | DAT_00495228 = 0x280 | int 640 | Set to display width |
| 7 | 0x414740+ | DAT_00495200 = 0x1e0 | int 480 | Set to display height |
| 8 | 0x414B50+ | BlitFrontendCachedRect(0,0,0x280,0x1e0) | int 640, 480 | Use DAT_00495228/DAT_00495200 |

### Tier 3 — Required for loading screens at native resolution
| # | Address | What | Value | Patch Strategy |
|---|---------|------|-------|----------------|
| 9 | 0x42CA00+ | Loading screen width stride 0x280 | int 640 | Use actual surface width |
| 10 | 0x42CA00+ | Loading screen pixel limit 0x4b000 | int 307200 | Use width*height |
| 11 | 0x442170+ | Benchmark image width stride 0x280 | int 640 | Same as #9 |
| 12 | 0x442170+ | Benchmark image pixel limit 0x4b000 | int 307200 | Same as #10 |

### Tier 4 — HUD baseline constants (optional)
| # | Address | What | Value | Notes |
|---|---------|------|-------|-------|
| 13 | 0x437BA0+ | HUD scale = width * 0.0015625 | float 1/640 | Keep for proportional scaling |
| 14 | 0x437BA0+ | HUD scale = height * 0.0020833334 | float 1/480 | Keep for proportional scaling |

---

## Key Globals Reference

| Global | Address | Type | Purpose |
|--------|---------|------|---------|
| gRenderWidth | 0x4AAF08 | float | Active render width (race) |
| gRenderHeight | 0x4AAF0C | float | Active render height (race) |
| gRenderCenterX | varies | float | gRenderWidth * 0.5 |
| gRenderCenterY | varies | float | gRenderHeight * 0.5 |
| DAT_00495228 | 0x495228 | int | Frontend virtual width (always 640) |
| DAT_00495200 | 0x495200 | int | Frontend virtual height (always 480) |
| DAT_004b1138 | 0x4B1138 | float | HUD X scale factor (width/640) |
| DAT_004b113c | 0x4B113C | float | HUD Y scale factor (height/480) |
| _DAT_004c3718 | 0x4C3718 | float | Projection scale (width * 0.5625) |

---

## Architecture Notes

1. **The race renderer is ALREADY mostly resolution-independent.** The gRenderWidth/gRenderHeight globals propagate through InitializeRaceViewportLayout, the HUD, minimap, overlays, and clipping. The 640x480 values in InitializeRaceVideoConfiguration are overwritten by InitializeRaceSession which reads the actual display mode.

2. **The frontend is FULLY 640x480-locked** through DAT_00495228/DAT_00495200. Changing these two values in InitializeFrontendResourcesAndState would propagate to all frontend code since menus reference these globals rather than hardcoded literals (except the one Blt in RunFrontendDisplayLoop).

3. **Loading screens are the most rigid** — they assume the source TGA image is exactly 640x480 pixels and hardcode both the stride and total pixel count. Higher-res loading screens require both new assets and code patches.

4. **The 0.5625 projection constant** is the most subtle issue for widescreen. It controls the FOV relationship and will cause horizontal stretching at non-4:3 aspect ratios.
