# M2DX.dll Resolution Hardcodes — Complete Analysis

> DLL: M2DX.dll (DirectX 6 engine, 438 functions)
> Ghidra instance: port 8193
> Base address: 0x10000000

## Resolution Table Data Structure

The engine stores enumerated display modes in a flat array of 20-byte (0x14) rows:

| Offset in row | Global array base     | Field               |
|---------------|-----------------------|----------------------|
| +0x00         | `g_displayModeWidthTable`     (0x10031024) | Width (DWORD)        |
| +0x04         | `g_displayModeHeightTable`    (0x10031028) | Height (DWORD)       |
| +0x08         | `g_displayModeBitDepthTable`  (0x1003102C) | Bit depth (DWORD)    |
| +0x0C         | `DAT_10031030`                (0x10031030) | Refresh rate (DWORD) |
| +0x10         | `g_displayModeCompatibilityFlagTable` (0x10031034) | D3D driver compat flag |

- **Stride**: 20 bytes (5 DWORDs)
- **Count**: `g_displayModeCount` at 0x10032678
- **Max entries**: 50 (0x32) — hardcoded in `RecordDisplayModeIfUsable`
- **Sort order**: Bit depth descending, then width descending, then height descending (`CompareDisplayModeRows`)

### Key Index Variables
| Address      | Global Name                           | Purpose |
|-------------|---------------------------------------|---------|
| 0x10032678  | `g_displayModeCount`                  | Number of valid entries in table |
| 0x1003267C? | `g_activeDisplayModeIndex`            | Currently selected mode table index |
| (BSS)       | `g_defaultFullscreenDisplayModeIndex` | Preferred fullscreen mode (640x480x16) |
| (BSS)       | `g_defaultWindowedDisplayModeIndex`   | Preferred windowed mode (matches desktop) |

---

## Global Width/Height Variables

| Address      | Name                              | Set by                        | Read by | Notes |
|-------------|-----------------------------------|-------------------------------|---------|-------|
| (BSS)       | `g_activeRenderWidth`             | FullScreen, RefreshRenderClientRect, ApplyWindowedRenderSize, HandleRenderWindowResize | Flip, ResetPresentRegionTracking, BeginScene, RestoreDisplayInfoOverlay | **Primary render width** used everywhere |
| (BSS)       | `g_activeRenderHeight`            | (same as above)               | (same as above) | **Primary render height** |
| (BSS)       | `g_windowedRenderWidth`           | ApplyDirectDrawDisplayMode, HandleRenderWindowResize, ResizeWindowedD3DDevice | EnumerateDisplayModes, BeginScene | Windowed client size |
| (BSS)       | `g_windowedRenderHeight`          | (same)                        | (same) | |
| (BSS)       | `g_windowedRenderBitDepth`        | ApplyDirectDrawDisplayMode, DXDraw::Create | EnumerateDisplayModes | |
| (BSS)       | `g_surfaceAllocationWidth`        | CreateSurfaces                | HandleRenderWindowResize, ResizeWindowedD3DDevice | Actual surface allocation size |
| (BSS)       | `g_surfaceAllocationHeight`       | CreateSurfaces                | HandleRenderWindowResize | |
| (BSS)       | `g_windowedRestoreWidth`          | FullScreen, InitializeD3DDriverAndMode | ResizeWindowedD3DDevice | Saved size before going fullscreen |
| (BSS)       | `g_windowedRestoreHeight`         | (same)                        | (same) | |
| (BSS)       | `g_cachedActiveDisplayModeRow`    | ApplyDirectDrawDisplayMode, RefreshRenderClientRect, EnumerateDisplayModes | HandleDXDrawWindowMessage, RestoreDisplayInfoOverlay | Cached width from SetDisplayMode |
| (BSS)       | `g_cachedActiveDisplayModeHeight` | (same)                        | (same) | Cached height |
| (BSS)       | `g_cachedActiveDisplayModeBitDepth` | ApplyDirectDrawDisplayMode, DXDraw::Create | ResolveCompatibleD3DDriverModePair | |
| 0x100314E8  | `DAT_100314E8`                    | DXDraw::Create                | HandleDXDrawWindowMessage (WM_GETMINMAXINFO) | Max tracking width (windowed) |
| 0x100314EC  | `DAT_100314EC`                    | DXDraw::Create                | HandleDXDrawWindowMessage | Max tracking height (windowed) |

---

## Hardcoded 640x480 References

### 1. SelectPreferredDisplayMode (0x10002AE0)
**Hex values**: `0x280` (640), `0x1E0` (480), `0x10` (16bpp)
```c
// Searches for exact 640x480x16 match as preferred fullscreen mode
if (((param_2 & uVar1) != 0) && (piVar6[-2] == 0x280) && (piVar6[-1] == 0x1E0)
    && (iVar4 = iVar3, *piVar6 == 0x10)) break;
```
**Controls**: Default fullscreen mode selection when engine starts or driver changes.
**Affects**: Both menu and race — this picks the initial fullscreen resolution.
**Patch priority**: HIGH — change to desired default resolution or remove preference.

### 2. EnumerateDisplayModes (0x10007EC0)
**Hex values**: `0x280` (640), `0x1E0` (480), `0x10` (16bpp)
```c
// Fullscreen default: scan for 640x480x16
if ((piVar1[-1] == 0x280) && (*piVar1 == 0x1E0) && (piVar1[1] == 0x10)) {
    g_activeDisplayModeIndex = uVar2;
    g_defaultFullscreenDisplayModeIndex = uVar2;
}
```
**Controls**: Sets the default fullscreen display mode index after enumeration + sort.
**Affects**: Both menu and race — first fullscreen mode attempted.
**Patch priority**: HIGH — change to select a different default or highest-available mode.

### 3. ResizeWindowedD3DDevice (0x10003940)
**Hex value**: `0x140` (320)
```c
// Fallback restore size when no saved windowed size and not fullscreen
if ((width == -0x19) && (width = g_windowedRestoreWidth, g_isFullscreenCooperative == 0)) {
    width = 0x140;  // 320
}
if ((height == -0x19) && (height = g_windowedRestoreHeight, g_isFullscreenCooperative == 0)) {
    height = 0x140;  // 320
}
```
**Controls**: Fallback windowed size (320x320) when restoring from fullscreen with no saved size.
**Affects**: Windowed mode only (not used by TD5 normally since it's always fullscreen).
**Patch priority**: LOW — game always runs fullscreen.

### 4. CreateDiagnosticOverlaySurfaces (0x10008470)
**Hex value**: `600` (decimal, compared with `g_activeRenderWidth`)
```c
// Font size selection for FPS overlay
DAT_10061aec = CreateFontA(
    ((600 < g_activeRenderWidth) - 1 & 0xFFFFFFF4) + 0x18,  // 24px if >600, 12px if <=600
    ...);
```
**Controls**: Debug overlay font size threshold.
**Affects**: Diagnostic overlay only, not gameplay. Font is 24px at >600 width, 12px otherwise.
**Patch priority**: NONE — cosmetic debug feature.

---

## 4:3 Aspect Ratio Filter

### RecordDisplayModeIfUsable (0x10008020) — THE CRITICAL FILTER

**Disassembly at 0x100080B3** (file offset 0x80B3):
```asm
100080AB: SHL EDX, 0x2          ; EDX = height * 4
100080AE: LEA EAX, [EAX+EAX*2] ; EAX = width * 3
100080B1: CMP EAX, EDX          ; width*3 == height*4 ?
100080B3: JZ  0x100080BD        ; if equal (4:3), accept the mode
100080B5: MOV EAX, 0x1          ; else return true (skip this mode, continue enum)
100080BA: RET 0x8
```

**Logic**: `width * 3 == height * 4` is the 4:3 test. The `JZ` at 0x100080B3 jumps to the "accept" path. Non-4:3 modes are rejected (skipped).

**Decompiled equivalent**:
```c
if (g_displayModeBitDepthTable[idx] < 0xF) return true;  // skip <15bpp
if (width * 3 != height << 2) return true;                // skip non-4:3
g_displayModeCount++;                                      // accept this mode
return (g_displayModeCount != 0x32);                       // stop at 50 entries
```

**Known patch** (already applied per widescreen_patches_applied.md):
- Change `JZ` (0x74) at offset 0x80B3 to `JMP` (0xEB) — makes the engine accept ALL aspect ratios.
- This is a 1-byte patch: `0x74` -> `0xEB` at file offset 0x80B3 (VA 0x100080B3).

**Affects**: Mode enumeration for BOTH menu and race — this is the gatekeeper that decides which display modes appear in the mode table.

---

## Additional Bit-Depth Filter

In the same function, modes with bit depth < 15 (0xF) are silently skipped:
```asm
1000808E: CMP dword ptr [EAX + 0x1003102C], 0xF
10008095: JGE 0x1000809F    ; accept if >= 15bpp
10008097: MOV EAX, 0x1      ; skip if < 15bpp
```
This filters out 8bpp and 4bpp modes. No patch needed.

---

## Resolution Flow: Initialization Sequence

```
DXDraw::Create (0x100062A0)
  |-> DirectDrawCreate + QueryInterface for IDirectDraw4
  |-> EnumerateDisplayModes (0x10007EC0)
  |     |-> IDirectDraw4::EnumDisplayModes callback = RecordDisplayModeIfUsable
  |     |     |-> For each mode: check bpp>=15, check 4:3 aspect, store in table
  |     |-> QuickSortRows by (bitdepth desc, width desc, height desc)
  |     |-> Scan for windowed default (matches desktop resolution)
  |     |-> Scan for fullscreen default (exact 640x480x16 match)
  |     |-> Cache the active mode row (5 DWORDs)
  |
  DXD3D::Create (0x100010B0)
  |-> InitializeD3DDriverAndMode (0x100034D0)
  |     |-> ResolveCompatibleD3DDriverModePair — resolves -25/-24 sentinels
  |     |-> RefreshRenderClientRect — sets g_activeRenderWidth/Height
  |     |-> ConfigureDirectDrawCooperativeLevel
  |     |-> ApplyDirectDrawDisplayMode (640, 480, 16) — calls IDirectDraw4::SetDisplayMode
  |     |-> InitializeD3DDeviceSurfaces
  |     |     |-> DXDraw::CreateSurfaces(width, height)
  |     |     |     |-> Windowed: primary surface + offscreen back buffer (width x height)
  |     |     |     |-> Fullscreen: primary flip chain with N back buffers
  |     |     |-> CreateAndAttachZBuffer(width, height, driverIdx)
  |     |-> CreateD3DDeviceForDriverRecord
  |     |-> g_pfnCreateViewportCallback(width, height, &g_pDirect3DViewport, userData)
  |     |-> SetRenderState
  |
  |-> If DAT_10061C1C: FullScreen(g_activeDisplayModeIndex) — goes fullscreen
```

## Resolution Flow: Mode Switch (FullScreen)

```
DXD3D::FullScreen(modeIndex) (0x10002170)
  |-> ResolveCompatibleD3DDriverModePair
  |-> Release viewport, device, surfaces
  |-> Read width/height/bpp from g_displayMode*Table[modeIndex]
  |-> Save g_windowedRestoreWidth/Height
  |-> Set g_activeRenderWidth/Height = mode width/height
  |-> ConfigureDirectDrawCooperativeLevel(hwnd, 1)  // exclusive
  |-> ApplyDirectDrawDisplayMode(width, height, bpp)
  |-> InitializeD3DDeviceSurfaces(driverIdx, width, height)
  |-> CreateD3DDeviceForDriverRecord
  |-> g_pfnCreateViewportCallback(width, height, ...)
  |-> SetRenderState
```

## Resolution Flow: Windowed Resize

```
HandleDXDrawWindowMessage (WM_SIZE case)
  |-> HandleRenderWindowResize (0x10004BC0)
        |-> RefreshRenderClientRect
        |-> Extract width/height from lParam
        |-> Clamp to minimum 50x50
        |-> If new size outside surface allocation reuse window:
        |     |-> Tear down device/surfaces
        |     |-> InitializeD3DDeviceSurfaces(driverIdx, newW, newH)
        |     |-> CreateD3DDeviceForDriverRecord
        |     |-> viewport callback
        |-> Else: RestoreLostDisplaySurfaces (reuse existing allocation)
        |-> g_windowedRenderWidth/Height = new size
        |-> SetRenderState + ResetPresentRegionTracking
```

---

## How Engine Communicates Resolution to Game

1. **Viewport callback**: The game registers `g_pfnCreateViewportCallback` and `g_pfnReleaseViewportCallback` via `InitializeD3DDriverAndMode`. The create callback receives `(width, height, &g_pDirect3DViewport, userData)`.

2. **Direct global reads**: The EXE reads `g_activeRenderWidth` / `g_activeRenderHeight` (exported or via known address).

3. **BeginScene viewport clear**: `BeginScene` (0x100015A0) uses `g_windowedRenderWidth` / `g_windowedRenderHeight` to set the viewport clear rectangle.

4. **Flip/present**: `DXDraw::Flip` uses `g_activeRenderWidth` / `g_activeRenderHeight` for the full-frame blit rect.

---

## Summary of ALL Hardcoded Resolution Values in M2DX.dll

| Address    | Function                      | Value      | Meaning | Patch needed? |
|-----------|-------------------------------|------------|---------|---------------|
| 0x10002B** | SelectPreferredDisplayMode   | 0x280 (640) | Preferred width | YES — change default |
| 0x10002B** | SelectPreferredDisplayMode   | 0x1E0 (480) | Preferred height | YES — change default |
| 0x10002B** | SelectPreferredDisplayMode   | 0x10 (16)   | Preferred bpp | Maybe (could prefer 32bpp) |
| 0x10007F** | EnumerateDisplayModes        | 0x280 (640) | Default fullscreen width | YES — change default |
| 0x10007F** | EnumerateDisplayModes        | 0x1E0 (480) | Default fullscreen height | YES — change default |
| 0x10007F** | EnumerateDisplayModes        | 0x10 (16)   | Default fullscreen bpp | Maybe |
| 0x100080B3 | RecordDisplayModeIfUsable    | JZ (4:3 filter) | Aspect ratio gate | YES — JZ->JMP done |
| 0x1000808E | RecordDisplayModeIfUsable    | 0x0F (15)   | Min bpp threshold | No |
| 0x100080C0 | RecordDisplayModeIfUsable    | 0x32 (50)   | Max mode count | Maybe (if >50 modes needed) |
| 0x10003974 | ResizeWindowedD3DDevice      | 0x140 (320) | Fallback windowed W | LOW |
| 0x10003986 | ResizeWindowedD3DDevice      | 0x140 (320) | Fallback windowed H | LOW |
| 0x10008*** | CreateDiagnosticOverlaySurfaces | 600       | Font size threshold | No (cosmetic) |
| 0x10008960+ | CreateSurfaces              | 0x32 (50)   | Min surface dimension | No |

---

## Widescreen Patch Checklist for M2DX.dll

To enable arbitrary resolutions:

1. **[DONE]** `RecordDisplayModeIfUsable` offset 0x80B3: `JZ` -> `JMP` (removes 4:3 filter)
2. **[TODO]** `EnumerateDisplayModes`: Change 640x480x16 default scan to either:
   - Prefer highest available resolution, or
   - Prefer a specific target (e.g., desktop resolution), or
   - Use the last enumerated mode (already sorted highest-first)
3. **[TODO]** `SelectPreferredDisplayMode`: Change 640x480x16 preference to match #2
4. **[OPTIONAL]** Increase max mode count from 50 if needed for many resolutions
5. **[OPTIONAL]** Change `ResizeWindowedD3DDevice` fallback from 320x320

The `CreateSurfaces`, `FullScreen`, `InitializeD3DDeviceSurfaces`, `HandleRenderWindowResize`, `Flip`, and `BeginScene` functions are all resolution-AGNOSTIC — they use whatever width/height is passed to them. No patches needed in those functions.
