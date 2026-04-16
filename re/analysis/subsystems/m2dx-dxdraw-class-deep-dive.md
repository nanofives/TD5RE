# DXDraw Class Deep Dive -- M2DX.DLL

> Binary: M2DX.DLL | Ghidra session: 8775ed7da68b4330916a9a2b9f5df03f
> Analysis date: 2026-03-28
> Address range: 0x100061E0 - 0x10009380

DXDraw is the DirectDraw subsystem of M2DX.DLL. It owns the primary display surface, back buffer, flip chain, gamma control, dirty-rectangle tracking, frame rate measurement, diagnostic overlays, adapter enumeration, and display mode management. Like all M2DX classes, it uses static `__cdecl` methods with no vtable -- the "class" is a pure namespace.

---

## 1. tagDX_dd Structure Layout (at 0x10030FF0)

The global `DXDraw::dd` is a 56-byte `tagDX_dd` structure. Ghidra's Demangler created this as a placeholder; the full layout recovered from decompilation:

| Offset | Size | Type | Name | Notes |
|--------|------|------|------|-------|
| +0x00 | 4 | IDirectDraw4* | `pDirectDraw4` | Core DDraw interface |
| +0x04 | 4 | IDirectDrawSurface4* | `pPrimarySurface` | Front buffer / flip chain primary |
| +0x08 | 4 | IDirectDrawSurface4* | `pRenderTargetSurface` | Back buffer / render target |
| +0x0C | 4 | IDirectDrawSurface4* | `pFrameRateOverlaySurface` | FPS overlay surface (field_0x0c) |
| +0x10 | 4 | IDirectDrawSurface4* | `pZBufferSurface` | Z-buffer |
| +0x14 | 4 | IDirectDrawSurface4* | `pDisplayInfoOverlaySurface` | Display info overlay (field_0x14) |
| +0x18 | 4 | IDirectDrawGammaControl* | `pGammaControl` | Gamma ramp interface |
| +0x1C | 8 | SIZE | `fpsOverlayTextExtent` | Text extent of FPS string |
| +0x24 | 8 | SIZE | `infoOverlayTextExtent` | Text extent of info string |
| +0x2C | 4 | IDirectDrawClipper* | `pWindowClipper` | Windowed-mode clipper |
| +0x30 | 4 | IDirectDrawPalette* | `pOverlaySurface` | Palette object (DAT_10031020) |
| +0x34 | 4 | int | `displayModeWidthTable` | Start of display mode table |

There are also aliased global pointers that reference the same objects at explicit addresses:

| Address | Type | Alias For |
|---------|------|-----------|
| 0x10030FF0 | IDirectDraw4* | dd.pDirectDraw4 |
| 0x10030FF4 | IDirectDrawSurface4* | dd.pPrimarySurface |
| 0x10030FF8 | IDirectDrawSurface4* | dd.pRenderTargetSurface |
| 0x10031008 | IDirectDrawGammaControl* | dd.pGammaControl |
| 0x1003101C | IDirectDrawClipper* | dd.pWindowClipper |
| 0x10031020 | IDirectDrawPalette* | dd.pOverlaySurface |

---

## 2. Exported Methods (Complete)

| VA | Export | Signature | Purpose |
|----|--------|-----------|---------|
| 0x100061E0 | `Environment` | `int __cdecl()` | Version check (DX6+), zero-init 0x5D6 dwords of dd workspace |
| 0x100062A0 | `Create` | `int __cdecl()` | Enumerate adapters, select adapter, create IDirectDraw4, get caps, enumerate display modes |
| 0x10006840 | `Destroy` | `int __cdecl()` | Release all surfaces, restore display mode, release IDirectDraw4 |
| 0x10006930 | `ResetFrameRate` | `void __cdecl()` | Zero all FPS counters, mark overlay dirty |
| 0x100069E0 | `CalculateFrameRate` | `int __cdecl()` | Sample FPS every 10 frames via timeGetTime, update overlay |
| 0x10006AF0 | `Print` | `int __cdecl(char*)` | Draw text on primary surface via GDI (FPS overlay) |
| 0x10006BF0 | `PrintTGA` | `int __cdecl(char*,int,int,TGAHEADER*,uchar*)` | Stamp text into TGA screenshot buffer |
| 0x10006EE0 | `GammaControl` | `int __cdecl(float,float,float)` | Apply per-channel gamma ramp (256 entries) |
| 0x10006FE0 | `Flip` | `int __cdecl(ulong)` | Present frame: windowed Blt or fullscreen Flip |
| 0x100076D0 | `ClearBuffers` | `int __cdecl()` | Blt-fill primary and back buffer to black |
| 0x10007850 | `GetDDSurfaceDesc` | `int __cdecl(DDSURFACEDESC2*,IDirectDrawSurface4*)` | Zero+fill surface descriptor |
| 0x10008710 | `CreateDDSurface` | `long __cdecl(DDSURFACEDESC2*,IDirectDrawSurface4**,IUnknown*)` | Wrapper: optionally adds DDSCAPS_SYSTEMMEMORY |
| 0x10008960 | `CreateSurfaces` | `int __cdecl(int,int)` | Create primary + back buffer + clipper + gamma |
| 0x10008DF0 | `DestroySurfaces` | `int __cdecl()` | Release gamma, primary, render target, clipper |
| 0x10008ED0 | `ApplyDirectDrawDisplayMode` | `int __cdecl(int,int,int)` | IDirectDraw4::SetDisplayMode wrapper |

---

## 3. Internal (Non-Exported) Functions

| VA | Name | Purpose |
|----|------|---------|
| 0x100078A0 | `ConfigureDirectDrawCooperativeLevel` | Set DDSCL_EXCLUSIVE or DDSCL_NORMAL |
| 0x10007960 | `ProbeDirectDrawCapabilities` | GetCaps, detect gamma ramp support, palette caps |
| 0x10007AE0 | `EnumerateDirectDrawAdapterExCallback` | DDEnumExA callback: fill 0x8C-byte adapter record |
| 0x10007BE0 | `EnumerateDirectDrawAdapterCallback` | DDEnumA callback (non-Ex variant) |
| 0x10007CD0 | `EnumerateDirectDrawAdapterCallback` | DDEnumA callback (4-arg variant) |
| 0x10007DD0 | `EnumerateDirectDrawAdapterExCallback` | DDEnumExA callback (Ex variant) |
| 0x10007EC0 | `EnumerateDisplayModes` | EnumDisplayModes -> sort -> select defaults |
| 0x10008020 | `RecordDisplayModeIfUsable` | EnumDisplayModes callback: 4:3 filter, 50-mode cap |
| 0x100080D0 | `CompareDisplayModeRows` | qsort comparator: bit depth > width > height |
| 0x10008130 | `RestoreFrameRateOverlay` | Repaint FPS overlay surface with current stats |
| 0x100082A0 | `RestoreDisplayInfoOverlay` | Repaint display info overlay |
| 0x10008470 | `CreateDiagnosticOverlaySurfaces` | Create FPS + info overlay surfaces, select font |
| 0x10008740 | `UpdatePauseDisplayState` | Pause/unpause: snapshot palette, flip to GDI |
| 0x10008E50 | `RefreshRenderClientRect` | Update cached client-area origin and render dimensions |
| 0x10008F60 | `ApplyWindowedRenderSize` | Resize window, update g_activeRenderWidth/Height |
| 0x10009040 | `ReattachPrimarySurfacePalette` | Re-attach palette to primary surface |
| 0x10009080 | `RestoreLostDisplaySurfaces` | Restore primary/back/Z if DDERR_SURFACELOST |
| 0x10009170 | `RestoreLostOverlaySurfaces` | Restore FPS/info overlay surfaces |
| 0x100091F0 | `AdapterSelectDialogProc` | WM_INITDIALOG/WM_COMMAND for adapter picker |
| 0x100092E0 | `TimeDemoDialogProc` | Dialog for TimeDemo filename/caption entry |
| 0x100072B0 | `ResetPresentRegionTracking` | Init dirty rect lists A/B/C to full-screen rect |
| 0x10007320 | `MergeDirtyRectLists` | Merge + coalesce two rect lists for windowed present |

---

## 4. Key Globals

### Core State

| Address | Type | Name | Purpose |
|---------|------|------|---------|
| 0x10061C20 | int | `g_isFullscreenCooperative` | 0 = windowed, 1 = exclusive fullscreen |
| 0x10061C4C | int | `g_isChangingCooperativeLevel` | Guard flag during mode transitions |
| 0x10061C18 | int | `DAT_10061c18` | System memory surface preference flag |
| 0x10061BF4 | int | `g_clientOriginX` | Client-area screen X offset (windowed) |
| 0x10061BF8 | int | `g_clientOriginY` | Client-area screen Y offset (windowed) |
| 0x10061C5C | int | `DAT_10061c5c` | Suppress surface restore flag |

### Render Dimensions

| Address | Type | Name |
|---------|------|------|
| 0x10061BBC | int | `g_activeRenderWidth` (also written from mode table) |
| 0x10061BC0 | int | `g_activeRenderHeight` |
| 0x100314EC | int | `g_surfaceAllocationWidth` |
| 0x100314E8 | int | `g_surfaceAllocationHeight` |
| 0x10033838 | int | `g_ddSurfaceDescScratch.dwHeight` (in DDSURFACEDESC2 scratch) |
| 0x1003383C | int | `g_ddSurfaceDescScratch.dwWidth` |

### Display Mode Table

| Address | Type | Name |
|---------|------|------|
| 0x10031024 | int[50*5] | Display mode table (stride 0x14 = 20 bytes) |
| 0x10031024 +0x00 | int | `g_displayModeWidthTable[i*5]` |
| 0x10031024 +0x04 | int | `g_displayModeHeightTable[i*5]` |
| 0x10031024 +0x08 | int | `g_displayModeBitDepthTable[i*5]` |
| 0x10031024 +0x0C | int | `g_displayModeRefreshRate[i*5]` |
| 0x10031024 +0x10 | int | `g_displayModeCompatibilityFlagTable[i*5]` |

Max 50 entries (0x32). Sorted by bit depth descending, then width, then height.

### Display Mode Selection

| Address | Name |
|---------|------|
| 0x10032xxx | `g_displayModeCount` |
| 0x10032xxx | `g_activeDisplayModeIndex` |
| 0x10032xxx | `g_defaultWindowedDisplayModeIndex` |
| 0x10032xxx | `g_defaultFullscreenDisplayModeIndex` |
| 0x10032xxx | `g_cachedActiveDisplayModeRow` (5-dword copy of active mode) |

### Adapter Table

| Address | Type | Name |
|---------|------|------|
| 0x100314BC | int | `g_adapterRecordCount` |
| 0x100314C0 | int | `g_selectedAdapterIndex` |
| 0x100314C4 | int[N*0x23] | `g_adapterRecordTable` (stride 0x8C = 140 bytes) |

Adapter record layout (0x8C bytes per entry):
- +0x00: `isWindowedUnsupported` (0 = supports windowed, 1 = fullscreen only)
- +0x3C: display name string (for combo box)
- +0x78: GUID (16 bytes)

### Flip / Dirty Rect State

| Address | Type | Name |
|---------|------|------|
| 0x10033994 | uint | `g_flipRectListACount` |
| 0x10033998 | RECT[30] | `g_flipRectListA` |
| 0x1003399C | uint | `g_flipRectListBCount` |
| 0x100339A0+ | RECT[30] | `g_flipRectListB` |
| 0x10032948 | RECT | Third dirty rect (list C) |
| 0x10032xxx | int | `g_isRenderTargetInVideoMemory` |
| 0x10032xxx | int | `g_fpsOverlayDirty` |

### Gamma Control

| Address | Type | Name |
|---------|------|------|
| 0x10032B38 | DDGAMMARAMP | `g_savedGammaRamp` (768 bytes: 256 * 3 WORDs) |
| 0x10033230 | DDGAMMARAMP | `g_activeGammaRamp` (applied ramp) |
| 0x10022CCC | int | `g_gammaRampNeedsSave` (1 = first call, save original) |
| 0x10032xxx | int | `g_gammaControlCapabilityState` |

### Frame Rate

| Address | Type | Name |
|---------|------|------|
| 0x10032xxx | int | `g_fpsFrameCounter` (counts to 10) |
| 0x10032xxx | int | `g_fpsAccumulatedFrames` |
| 0x10032xxx | DWORD | `g_fpsLastSampleTime` (timeGetTime value) |
| 0x1003270C | float | `g_currentFPSFloat` |
| 0x10032710 | int | `g_currentFPSInt` |
| 0x10032714 | int | `g_currentTrianglesPerSec` |

---

## 5. Surface Creation -> Flip -> Restore Lifecycle

### 5.1 Environment (0x100061E0)

First function called. Validates that DirectX 6+ is installed by calling `DirectDrawCreate` and `QueryInterface` for IID_IDirectDraw4. If the QI fails, sets `ErrorN = 0x6E` and displays "Requires DirectX 6.0 or later". On success, zero-initializes the entire 0x5D6-dword dd workspace and resets `ScreenX`, `ScreenY` to 0.

### 5.2 Create (0x100062A0)

The main startup sequence:

1. **DX6 probe**: `LoadLibraryA("ddraw.dll")`, check for `DirectDrawEnumerateExA` export. If absent, flags `g_ddrawEnumerateExState |= 2` (fall back to non-Ex enumeration).

2. **System palette snapshot**: `GetSystemPaletteEntries` into `DAT_10031730` (256 entries).

3. **Adapter enumeration**: Either `DirectDrawEnumerateExA` or `DirectDrawEnumerateA`, populating the 0x8C-byte adapter records. Each callback creates a temporary IDirectDraw, calls `GetCaps` to check if DDSCL_NORMAL is supported, and records the adapter name + GUID.

4. **Adapter selection**:
   - `g_adapterSelectionOverride == -1` (DeviceSelect token): show `AdapterSelectDialogProc` combo box
   - `g_adapterSelectionOverride == 1` (Primary token): force adapter 0
   - Otherwise: pick adapter 1 if multiple adapters exist

5. **DirectDraw creation**: `DirectDrawCreate` with selected adapter GUID, `QueryInterface` for IDirectDraw4.

6. **Device identifier**: `GetDeviceIdentifier` to read vendor/device IDs. Special case: NVIDIA (vendor 0x10DE) with device IDs 0x20, 0x28, 0x2C forces `g_matchBitDepthState = 1`.

7. **Cooperative level**: If adapter doesn't support windowed mode (`isWindowedUnsupported == 0`), forces fullscreen. Otherwise sets `DDSCL_NORMAL` (windowed) or `DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN`.

8. **Capabilities probe**: `ProbeDirectDrawCapabilities` queries gamma ramp support (`DDCAPS2_PRIMARYGAMMA`), palette caps, and 3D overlay capability.

9. **Current mode snapshot**: `GetDisplayMode` to cache desktop resolution/bit depth.

10. **Display mode enumeration**: `EnumerateDisplayModes` -> `RecordDisplayModeIfUsable` callback -> sort -> select default 640x480x16 mode.

### 5.3 CreateSurfaces (0x10008960)

Called by `InitializeD3DDeviceSurfaces` (in the DXD3D layer). Receives width and height (clamped to minimum 50).

**Windowed path** (`g_isFullscreenCooperative == 0`):
1. Create primary surface: `DDSCAPS_PRIMARYSURFACE` (caps 0x200). If `DAT_10061c18` is set, adds `DDSCAPS_SYSTEMMEMORY` (0x800) making caps 0xA00.
2. Create separate back buffer: `DDSCAPS_OFFSCREENPLAIN | DDSCAPS_3DDEVICE` (caps 0x2040, or 0x2840 with sysmem). Width/height set to requested dimensions.
3. Query back buffer's surface descriptor.
4. Create clipper: `IDirectDraw4::CreateClipper`.
5. Associate clipper with window: `IDirectDrawClipper::SetHWnd`.
6. Attach clipper to primary: `IDirectDrawSurface4::SetClipper`.

**Fullscreen path** (`g_isFullscreenCooperative != 0`):
1. Determine back buffer count: if `DAT_10032720 != 0` and triple buffering not disabled, use 2 (triple buffer); otherwise 1 (double buffer).
2. Create complex flip chain: `DDSCAPS_PRIMARYSURFACE | DDSCAPS_FLIP | DDSCAPS_COMPLEX | DDSCAPS_3DDEVICE` (caps 0x2218, or 0x2A18 with sysmem). `dwBackBufferCount` set.
3. If triple-buffer creation fails, retry with `dwBackBufferCount = 1`.
4. Get attached back buffer: `IDirectDrawSurface4::GetAttachedSurface` with `DDSCAPS_BACKBUFFER`.

**Common tail**:
- Detect if render target is in video memory: `(caps >> 14) & 1`.
- Determine effective bit depth format: if 16bpp and green mask is 0x7E0 (RGB565), record as 16; otherwise 15.
- `ClearBuffers()` to fill both surfaces black.
- If gamma control not disabled: `QueryInterface` for `IDirectDrawGammaControl` on primary surface.
- On any failure: release all partially-created surfaces and return 0.

### 5.4 Flip (0x10006FE0) -- Present Path

The main per-frame present function. Takes a `flags` parameter.

**Guard checks**:
- If `g_isRendererReady == 0`: error "Cannot call DXDraw::Flip while [not ready]"
- If `g_isDrawPaused != 0`: return 1 (success, no-op)

**Fullscreen path**:
1. `IDirectDrawSurface4::Flip(NULL, DDFLIP_WAIT)` (vtable offset +0x2C, flag 1).
2. If `DDERR_SURFACELOST` (0x887601C2): restore both primary and render target, then `ClearBuffers`.
3. If `g_isRenderTargetInVideoMemory != 0`: swap dirty rect lists A and B (triple-buffer tracking). This ensures that when flipping between 3 buffers, the correct set of dirty rects is used for the buffer that becomes visible.

**Windowed path**:
1. If `flags & 1` (force full redraw): create a single rect covering the entire render area, offset by client origin.
2. Otherwise: `MergeDirtyRectLists(outCount, outRects, listACount, listA, listBCount, listB)` to combine the two dirty rect lists.
3. Offset all merged rects by `(g_clientOriginX, g_clientOriginY)` to map from client to screen coordinates.
4. For each rect: `IDirectDrawSurface4::Blt(destRect, renderTarget, srcRect, DDBLT_WAIT)`.
5. If `DDERR_SURFACELOST`: restore both surfaces and clear.

**Common tail** (both paths):
- Copy list B to list A (previous frame becomes current).
- Set `g_flipRectListACount = g_flipRectListBCount`.
- Clear `g_fpsOverlayDirty`.

### 5.5 RestoreLostDisplaySurfaces (0x10009080)

Called from `DXD3D::BeginScene` when the renderer is not ready. Checks each surface in order:

1. **Primary**: `IsLost()` -> if DDERR_SURFACELOST -> `Restore()`.
2. **Render target**: same pattern.
3. **Z-buffer**: same pattern.

If any surface was restored, calls `ClearBuffers()` to re-fill. The guard `DAT_10061c5c` suppresses restoration during certain mode transitions.

### 5.6 DestroySurfaces (0x10008DF0)

Simple cleanup: releases GammaControl, Primary, RenderTarget, and Clipper in that order (COM Release via vtable+8), nulling each pointer.

### 5.7 Destroy (0x10006840)

Full teardown:
1. Release ZBuffer, OverlaySurface (palette), Clipper, RenderTarget, GammaControl, Primary.
2. If fullscreen: `IDirectDraw4::RestoreDisplayMode()`, then `ConfigureDirectDrawCooperativeLevel(hWnd, 0)` to set DDSCL_NORMAL.
3. Release IDirectDraw4 itself.

---

## 6. Dirty Rectangle Optimization System

### 6.1 Overview

The dirty rect system optimizes windowed-mode presents by tracking which screen regions changed between frames, avoiding full-surface blits. It uses a **double-buffered** rect list scheme:

- **List A** (`g_flipRectListA`, count at `g_flipRectListACount`): Current frame's dirty rects
- **List B** (`g_flipRectListB`, count at `g_flipRectListBCount`): Previous frame's dirty rects
- **List C** (`DAT_10032948`): Third auxiliary rect (used during triple buffering)

Each list holds up to **30 RECT structures** (30 * 16 = 480 bytes).

### 6.2 ResetPresentRegionTracking (0x100072B0)

Initializes all three lists to a single full-screen rect `(0, 0, g_activeRenderWidth, g_activeRenderHeight)`. Called on mode changes and when unpausing.

### 6.3 MergeDirtyRectLists (0x10007320)

The core merge algorithm:

1. **Allocate** two temp buffers: a 0x3C0-byte rect array (60 rects max = 30 from A + 30 from B) and a 0xF0-byte validity flag array.

2. **Copy + validate** both input lists into the temp array. A rect is **invalid** if:
   - Both left==0 and right==0 (zero width)
   - Both top==0 and bottom==0 (zero height)
   - right < left (inverted X)
   - bottom < top (inverted Y)

3. **Coalesce overlapping rects**: O(n^2) scan. For any pair where the rects overlap (checked via AABB intersection), merge rect[j] into rect[i] by expanding i's bounds, and mark j as deleted. Restart the inner scan from index 0 after each merge.

4. **Output**:
   - If the valid count <= 30: copy valid rects to output array, return count.
   - If the valid count > 30: compute a single bounding rect encompassing all valid rects, return count=1. This prevents pathological cases from creating too many small blits.

5. **Free** temp buffers.

### 6.4 Flip Double-Buffering

During fullscreen with video-memory render targets (`g_isRenderTargetInVideoMemory != 0`), Flip swaps lists A and B after each present. This is necessary because triple-buffered flip chains cycle through 3 surfaces, so the "two frames ago" dirty rects (now in list A) correspond to the buffer that just became the front buffer.

In windowed mode, after each frame: list B is copied to list A (shift one frame back), and list B is reset for the next frame's dirty rects.

---

## 7. Gamma Control System

### 7.1 GammaControl (0x10006EE0)

Signature: `int __cdecl GammaControl(float red, float green, float blue)`

The three float parameters are per-channel gamma multipliers applied to the original desktop gamma ramp.

**First-call behavior**: If `g_gammaRampNeedsSave != 0` (initially 1), calls `IDirectDrawGammaControl::GetGammaRamp(0, &g_savedGammaRamp)` to snapshot the current desktop gamma. Then clears the flag so subsequent calls skip the save.

**Ramp computation**: Iterates 256 entries (iVar1 = 0 to 0x200 stepping by 2, i.e., 256 WORD entries):
```
g_activeGammaRamp.red[i]   = (short)ftol(g_savedGammaRamp.red[i] * red)
g_activeGammaRamp.green[i] = (short)ftol(g_savedGammaRamp.green[i] * green)
g_activeGammaRamp.blue[i]  = (short)ftol(g_savedGammaRamp.blue[i] * blue)
```

Each channel is independently scaled by its float multiplier. A value of 1.0 preserves the original ramp; 0.5 would halve brightness; 0.0 would black out that channel.

**Apply**: `IDirectDrawGammaControl::SetGammaRamp(0, &g_activeGammaRamp)`.

**Guard**: If `g_gammaControlCapabilityState != 0` (gamma not supported by hardware), the entire function is a no-op returning 1.

### 7.2 Gamma Capability Detection

In `ProbeDirectDrawCapabilities` (0x10007960):
```
g_gammaControlCapabilityState |= (~DDCAPS2_PRIMARYGAMMA >> 16) & 2
```
If the hardware does not report `DDCAPS2_PRIMARYGAMMA`, the state gets bit 1 set, disabling gamma control.

### 7.3 Gamma Ramp Data Layout

| Address | Size | Content |
|---------|------|---------|
| 0x10032B38 | 768 | `g_savedGammaRamp` -- original desktop ramp (DDGAMMARAMP: 3 arrays of 256 WORDs) |
| 0x10033230 | 256*2 | `g_activeGammaRamp.red` |
| 0x10033430 | 256*2 | `g_activeGammaRamp.green` |
| 0x10033630 | 256*2 | `g_activeGammaRamp.blue` |

### 7.4 Gamma and Surface Lifecycle

The gamma control interface is obtained in `CreateSurfaces` via `QueryInterface` on the primary surface for `IID_IDirectDrawGammaControl` (GUID at 0x1001D400). It is released in `DestroySurfaces` and `Destroy`. When surfaces are rebuilt (mode switch, fullscreen toggle), the gamma interface is re-acquired and the game must re-apply the gamma ramp.

---

## 8. Display Mode Enumeration and Filtering

### 8.1 EnumerateDisplayModes (0x10007EC0)

Called at the end of `Create`. Drives the entire mode discovery:

1. `IDirectDraw4::EnumDisplayModes(0, NULL, NULL, RecordDisplayModeIfUsable)`.
2. `QuickSortRows` sorts the mode table by (bitDepth descending, width descending, height descending).
3. Scans for the mode matching current desktop resolution -> `g_defaultWindowedDisplayModeIndex`.
4. Scans for 640x480x16 -> `g_defaultFullscreenDisplayModeIndex` and `g_activeDisplayModeIndex`.
5. Fallback: if no 640x480x16 found, uses the windowed default; if that's also missing, uses mode 0.
6. Copies the selected mode's 5-dword row into `g_cachedActiveDisplayModeRow`.

### 8.2 RecordDisplayModeIfUsable (0x10008020)

The EnumDisplayModes callback. For each enumerated mode:

1. Records width, height, bit depth, refresh rate, and compatibility flag (initially 0) into the mode table at index `g_displayModeCount`.
2. **Rejects** modes with bit depth < 15 (no 8-bit palettized modes for 3D).
3. **4:3 aspect ratio filter**: `width * 3 != height * 4` -> reject. This is the widescreen blocker.
4. Increments `g_displayModeCount`; stops at 50 (0x32) entries.

**Patch sites**: The 4:3 check (JE -> JMP at "Widescreen Patch 27") and the 640x480 defaults (imm32 patches at "Widescreen Patch 28/29") are documented widescreen mod targets.

### 8.3 Mode Table Format

Each display mode occupies 20 bytes (5 dwords):

| Offset | Field |
|--------|-------|
| +0x00 | Width |
| +0x04 | Height |
| +0x08 | Bit Depth |
| +0x0C | Refresh Rate |
| +0x10 | Compatibility Flag (set by `InitializeMemoryManagement` VRAM check) |

---

## 9. Adapter Selection Dialog

### 9.1 AdapterSelectDialogProc (0x100091F0)

A standard Win32 dialog procedure for `IDD_DEVICESELECT`:

**WM_INITDIALOG (0x110)**:
- Loops through `g_adapterRecordCount` adapter records.
- Sends `CB_ADDSTRING` (0x180) to combo box control ID 0x3F9, using the display name string at `g_adapterRecordTable + i*0x8C + 0x3C` (offset to `DAT_10031538`).
- Selects item 0 via `CB_SETCURSEL` (0x186).
- Stores combo handle in `DAT_10032940`.

**WM_COMMAND (0x111)**:
- IDOK (1): Gets current selection via `CB_GETCURSEL` (0x188) -> `g_selectedAdapterIndex`. Calls `EndDialog`.
- IDCANCEL (2): Also ends dialog.

### 9.2 TimeDemoDialogProc (0x100092E0)

A secondary dialog (`IDD_DIALOG1`) for the TimeDemo feature. On OK (control ID 0x3FE), reads filename and caption from edit controls 0x3FC and 0x3FD into `DXDraw::FPSName` and `DXDraw::FPSCaption` (60-char buffers each). This is triggered by the `TimeDemo` command-line token.

---

## 10. Diagnostic Overlay Surfaces

### 10.1 CreateDiagnosticOverlaySurfaces (0x10008470)

Creates two offscreen surfaces for FPS and display info overlays:

1. **Font creation**: `CreateFontA("Arial", ...)` with height 24px (or 12px if render width <= 600).
2. **Text measurement**: Uses `GetTextExtentPointA` to measure format strings:
   - FPS format: `"000.00 fps 00000000.00 tps 0000 mpps"` (40 chars)
   - Info format: `"000x000x00  MONO  0000"` (23 chars)
3. **FPS overlay surface**: Width/height from measured text extent. System memory offscreen plain, optionally with DDSCAPS_SYSTEMMEMORY if `DAT_10061c18` is set. Color-keyed (color key set to black).
4. **Info overlay surface**: Same creation pattern with its own measured dimensions.
5. Calls `RestoreFrameRateOverlay()` and `RestoreDisplayInfoOverlay()` to paint initial text.

### 10.2 RestoreFrameRateOverlay (0x10008130)

Paints the current stats onto the FPS overlay surface:

1. Formats: `"%d.%02d fps"` (from `g_currentFPSFloat`), `"%ld tps"` (triangles/sec), `"%ld mpps"` (megapixels/sec).
2. Gets DC from overlay surface.
3. Uses GDI: yellow text (0xFFFF), black background, opaque mode.
4. `ExtTextOutA` with `ETO_OPAQUE` flag to paint.
5. Releases DC.

### 10.3 Print (0x10006AF0)

Draws text directly on the **primary** surface (not the overlay):
- Gets DC from primary surface.
- Black background, yellow text (0xFFFF).
- If `bChrOvrPrn == 0`: transparent mode; else opaque mode.
- In windowed mode: adjusts coordinates by `ClientToScreen` offset.
- Controlled by `g_isFPSOverlayEnabled`.

### 10.4 PrintTGA (0x10006BF0)

Stamps text into a TGA image buffer for screenshots. Only works with image type 1 (paletted TGA):
1. Creates a temporary 512x64 offscreen surface.
2. Gets DC, renders text in white on transparent background.
3. Locks the surface.
4. Scans pixels: any non-zero pixel sets the corresponding byte in the TGA data to 1 (marks as text overlay).
5. Releases and destroys temp surface.

---

## 11. Cooperative Level and Display Mode Management

### 11.1 ConfigureDirectDrawCooperativeLevel (0x100078A0)

| param_2 | Action |
|---------|--------|
| 0 | `SetCooperativeLevel(hWnd, DDSCL_NORMAL)` -> `g_isFullscreenCooperative = 0` |
| 1 | `SetCooperativeLevel(hWnd, flags)` -> `g_isFullscreenCooperative = 1` |

The fullscreen flags are 0x11 (`DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN`) on Win9x, or 0x51 (`DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN | DDSCL_ALLOWREBOOT`) on NT.

**Patch site**: `M2DX_OFF_COOPLEVEL_JZ` -- the JZ that tests `param_2 != 0` can be patched to JMP (0xEB) to always take the DDSCL_NORMAL path, forcing windowed mode.

### 11.2 ApplyDirectDrawDisplayMode (0x10008ED0)

Simple wrapper around `IDirectDraw4::SetDisplayMode(width, height, bpp, 0, 0)`. Also caches the values into:
- `g_cachedActiveDisplayModeRow` (width)
- `g_cachedActiveDisplayModeHeight`
- `g_cachedActiveDisplayModeBitDepth`
- `g_windowedRenderWidth/Height/BitDepth`

Sets `g_isChangingCooperativeLevel = 1` during the call as a re-entrancy guard.

### 11.3 RefreshRenderClientRect (0x10008E50)

Updates the cached render dimensions:
- **Windowed**: `ClientToScreen` + `GetClientRect` -> `g_activeRenderWidth/Height`.
- **Fullscreen**: Copies from `g_cachedActiveDisplayModeRow/Height`.

### 11.4 ApplyWindowedRenderSize (0x10008F60)

Resizes the application window for windowed mode:
1. Sends `WM_SIZE` to the window (or uses `SetRect` for non-resize case).
2. `SetWindowPos` with `SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE`.
3. `SetWindowPos` with `HWND_NOTOPMOST`.
4. Updates `g_clientOriginX/Y` via `ClientToScreen`.
5. Sets `g_activeRenderWidth/Height`.

---

## 12. Pause Display System

### 12.1 UpdatePauseDisplayState (0x10008740)

Implements a **nested pause** mechanism with `g_pauseNestingCount`:

**Pause (param_1 != 0)**:
- If already paused: increment nest count and return.
- If fullscreen with palette:
  1. `GetEntries` from palette object -> save to `DAT_10031B30`.
  2. Copy 236 entries (indices 10-245) from palette backup.
  3. `SetEntries` with system palette entries (restores desktop palette).
- If adapter requires it: `FlipToGDISurface()` to show desktop.
- `RedrawWindow` to repaint.
- Sets `g_isDrawPaused = 1`.

**Unpause (param_1 == 0)**:
- Decrement nest count; if still > 0, return.
- If fullscreen with palette: `SetEntries` to restore game palette from backup.
- Resets dirty rect lists to full screen (same as `ResetPresentRegionTracking`).
- Sets `g_isDrawPaused = 0`.

---

## 13. ClearBuffers (0x100076D0)

Fills both primary and render target surfaces with black using `IDirectDrawSurface4::Blt`:

1. Get surface descriptor for primary.
2. Create a DDBLTFX (size 100 = 0x64, zero-filled except dwSize).
3. `Blt(fullRect, NULL, NULL, DDBLT_WAIT | DDBLT_COLORFILL, &bltfx)` -- color fill value is 0 (black).
4. Repeat for render target.

The DDBLT flags are `0x1000400` = `DDBLT_WAIT (0x01000000) | DDBLT_COLORFILL (0x00000400)`.

---

## 14. Surface Lifecycle Summary

```
Environment()
    |-- DirectDrawCreate + QI for IDirectDraw4 (version check only, released)
    |-- Zero-init dd workspace
    v
Create()
    |-- Enumerate adapters (DDraw callbacks)
    |-- Select adapter (dialog or config)
    |-- DirectDrawCreate with adapter GUID
    |-- QI for IDirectDraw4
    |-- SetCooperativeLevel
    |-- ProbeDirectDrawCapabilities
    |-- GetDisplayMode (snapshot desktop)
    |-- EnumerateDisplayModes -> RecordDisplayModeIfUsable -> sort
    v
[DXD3D layer calls:]
CreateSurfaces(width, height)
    |-- Release any existing surfaces
    |-- Fullscreen: create flip chain (primary + N back buffers)
    |-- Windowed: create primary + separate back buffer + clipper
    |-- ClearBuffers
    |-- QI for GammaControl on primary
    v
[Per frame:]
BeginScene()
    |-- RestoreLostDisplaySurfaces() if needed
    |-- Clear viewport
    |-- CalculateFrameRate()
    v
[Render...]
    v
EndScene()
    |-- Flip(flags)
    |   |-- Fullscreen: IDirectDrawSurface4::Flip
    |   |-- Windowed: MergeDirtyRectLists -> Blt per rect
    |   |-- Handle DDERR_SURFACELOST: Restore + ClearBuffers
    |   |-- Swap dirty rect lists A <-> B
    v
[Mode switch / FullScreen:]
    |-- DestroySurfaces()
    |-- ApplyDirectDrawDisplayMode()
    |-- CreateSurfaces() (new resolution)
    v
Destroy()
    |-- Release all surfaces, gamma, clipper, palette
    |-- RestoreDisplayMode (fullscreen)
    |-- SetCooperativeLevel(NORMAL)
    |-- Release IDirectDraw4
```

---

## 15. Widescreen Patch Sites in DXDraw

| Patch ID | VA | Original | Patched | Effect |
|----------|-----|----------|---------|--------|
| Patch 27 | RecordDisplayModeIfUsable | JE (skip non-4:3) | JMP (0xEB) | Allow widescreen modes |
| Patch 28 | EnumerateDisplayModes | imm32 = 0x280 (640) | target width | Default fullscreen width |
| Patch 29 | EnumerateDisplayModes | imm32 = 0x1E0 (480) | target height | Default fullscreen height |
| M2DX_OFF_ADAPTER_JNZ | Create (0x100066xx) | JNZ | JMP (0xEB) | Bypass "no windowed" check |
| M2DX_OFF_COOPLEVEL_JZ | ConfigureDirectDrawCooperativeLevel | JZ | JMP (0xEB) | Force windowed cooperative level |

---

## 16. Error Handling Pattern

All DXDraw functions follow a consistent error pattern:
1. Call DirectDraw method.
2. If `DX::LastError != 0`: call `DXErrorToString(DX::LastError)` to format, then `Msg(formatString)` to log.
3. Release any partially-created resources.
4. Return 0 (failure).

The special error code `-0x7789FE3E` (= `0x887601C2` = `DDERR_SURFACELOST`) is handled specifically in Flip and RestoreLostDisplaySurfaces by calling `Restore()` on the lost surface.

---

## 17. NVIDIA Workaround

In `Create`, after reading the device identifier:
```c
if (vendorId == 0x10DE) {  // NVIDIA
    if (deviceId == 0x20 || deviceId == 0x28 || deviceId == 0x2C) {
        // TNT, TNT2, Vanta family
        g_matchBitDepthState = 1;
    }
}
```
This forces the Z-buffer bit depth to match the display bit depth, working around a known issue with early NVIDIA TNT/TNT2 drivers where mismatched Z-buffer depths caused rendering artifacts.
