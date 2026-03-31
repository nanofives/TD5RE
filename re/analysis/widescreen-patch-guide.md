# Widescreen Patch Guide -- M2DX.dll + TD5_d3d.exe

> Complete reference for enabling widescreen resolutions in Test Drive 5.
> Date: 2026-03-28

---

## Overview

TD5 uses a two-binary architecture: `TD5_d3d.exe` (game logic, rendering) and `M2DX.dll` (DirectX 6 engine, surface/mode management). Widescreen support requires patches in both binaries.

The M2DX.dll display mode enumeration callback (`RecordDisplayModeIfUsable`) contains a hardcoded 4:3 aspect ratio filter that rejects all non-4:3 display modes. This is the primary blocker. Additional patches update default mode preferences and EXE-side rendering constants.

---

## M2DX.dll Patch Sites (5 patches)

### Patch 1: Remove 4:3 Aspect Ratio Filter (CRITICAL)

| Field | Value |
|-------|-------|
| File offset | `0x80B3` |
| Virtual address | `0x100080B3` |
| Function | `RecordDisplayModeIfUsable` (0x10008020) |
| Original bytes | `74 08` (JZ +8) |
| Patched bytes | `EB 08` (JMP +8) |
| Instruction context | After `CMP EAX,EDX` comparing `width*3` vs `height*4` |

**What it does**: The original code computes `width * 3` and `height * 4`. If they are equal (4:3 aspect ratio), the JZ branches to the "accept" path. Non-4:3 modes fall through to `MOV EAX,1; RET 8` (reject). Changing JZ to JMP makes the branch unconditional -- all modes >= 15bpp are accepted regardless of aspect ratio.

**Full instruction sequence at the patch site**:
```
100080AB: C1 E2 02        SHL EDX, 0x2          ; EDX = height * 4
100080AE: 8D 04 40        LEA EAX, [EAX+EAX*2]  ; EAX = width * 3
100080B1: 3B C2           CMP EAX, EDX           ; width*3 == height*4 ?
100080B3: 74 08           JZ  0x100080BD  <<<--- PATCH THIS BYTE (74 -> EB)
100080B5: B8 01 00 00 00  MOV EAX, 0x1           ; return true (reject)
100080BA: C2 08 00        RET 0x8
100080BD: 41              INC ECX                ; accept: g_displayModeCount++
```

### Patch 2: Default Fullscreen Width in EnumerateDisplayModes

| Field | Value |
|-------|-------|
| File offset | `0x7F93` |
| Virtual address | `0x10007F93` |
| Function | `EnumerateDisplayModes` (0x10007EC0) |
| Original bytes | `BB 80 02 00 00` (MOV EBX, 0x280) |
| Patched bytes | `BB` + target_width_LE32 |

**What it does**: After sorting the mode table, `EnumerateDisplayModes` scans for a 640x480x16 mode to use as the default fullscreen mode. This patch changes the width comparison from 640 to the target width. If no exact match is found, the engine falls back to the windowed default (desktop resolution).

### Patch 3: Default Fullscreen Height in EnumerateDisplayModes

| Field | Value |
|-------|-------|
| File offset | `0x7FA2` |
| Virtual address | `0x10007FA2` |
| Function | `EnumerateDisplayModes` (0x10007EC0) |
| Original bytes | `81 38 E0 01 00 00` (CMP [EAX], 0x1E0) |
| Patched bytes | `81 38` + target_height_LE32 |

**What it does**: Companion to Patch 2. Changes the height in the 640x480x16 default scan.

### Patch 4: Preferred Mode Width in SelectPreferredDisplayMode

| Field | Value |
|-------|-------|
| File offset | `0x2BBD` |
| Virtual address | `0x10002BBD` |
| Function | `SelectPreferredDisplayMode` (0x10002AE0) |
| Original bytes | `81 7E F8 80 02 00 00` (CMP [ESI-8], 0x280) |
| Patched bytes | `81 7E F8` + target_width_LE32 |

**What it does**: When the engine needs to select a preferred display mode (e.g. after a driver change or initial startup), it searches the sorted mode table for a 640x480x16 match. This changes the width comparison.

### Patch 5: Preferred Mode Height in SelectPreferredDisplayMode

| Field | Value |
|-------|-------|
| File offset | `0x2BC6` |
| Virtual address | `0x10002BC6` |
| Function | `SelectPreferredDisplayMode` (0x10002AE0) |
| Original bytes | `81 7E FC E0 01 00 00` (CMP [ESI-4], 0x1E0) |
| Patched bytes | `81 7E FC` + target_height_LE32 |

**What it does**: Companion to Patch 4. Changes the height in the preferred mode search.

---

## EXE-Side Patches (handled by companion scripts)

These are applied by `td5_widescreen.py` and `patch_menu_widescreen.py`:

### Already Patched by td5_widescreen.py

| # | File offset | VA | What | Original | Patched |
|---|-------------|-----|------|----------|---------|
| E1 | 0x030AAC | 0x00430AAC | WinMain width (MOV [app+0xBC], imm32) | 640 | target_w |
| E2 | 0x030ABC | 0x00430ABC | WinMain height (MOV [app+0xC0], imm32) | 480 | target_h |
| E3 | 0x02A998 | 0x0042A998 | PUSH height (viewport setup) | 480 | target_h |
| E4 | 0x02A99D | 0x0042A99D | PUSH width (viewport setup) | 640 | target_w |
| E5 | 0x02A9A7 | 0x0042A9A7 | gRenderWidth float global | 640.0 | target_w as float |
| E6 | 0x02A9B1 | 0x0042A9B1 | gRenderHeight float global | 480.0 | target_h as float |
| E7 | 0x0422B1 | 0x004422B1 | Skip FullScreen on race end (JE -> JMP) | 74 15 | EB 15 |
| E8 | 0x02B48A | 0x0042B48A | Skip FullScreen on race start (JE -> JMP) | 74 1B | EB 1B |

### Already Patched by patch_menu_widescreen.py

| What | Location | Effect |
|------|----------|--------|
| Scale-at-Flip caves A/B/C/D | 0x45C330+ | Stretch 640x480 frontend to full resolution before Flip |
| BltFast centering zeroed | 0x45C235, 0x45C23C | Remove offset-based centering (superseded by scale) |
| Sprite loop flag | 0x42557C | PUSH 0x10 -> 0x11 for correct BltFast coordinate handling |
| Surface pre-allocation cave E | 0x45C2B0 | Create intermediate surface before legal screens |
| Force-Flip path | 0x424F0C | NOP JNZ to ensure Flip presentation path is active |

### Projection Focal Constant (EXE .rdata)

| Field | Value |
|-------|-------|
| File offset | `0x05D78C` |
| VA | `0x0045D78C` |
| Function consumer | `ConfigureProjectionForViewport` (0x0043E7E0) |
| Original value | `0x3F100000` (float 0.5625 = 9/16) |
| Correct value for hor+ | Compute `height * 0.75 / width` equivalent |

**What it does**: The projection setup multiplies the viewport width by 0.5625 to derive the focal length. The constant 0.5625 = 9/16 encodes a 4:3 assumption: `480/640 * (4/3) * (9/16) = 0.5625`. For widescreen, this produces a horizontally stretched image.

**Hor+ fix**: Change the computation to `focal = height * 0.75` instead of `focal = width * 0.5625`. For 16:9 at 1920x1080: `1080 * 0.75 = 810`, which gives the correct wider FOV. This requires either:
- Patching the .rdata float to `height/width * 0.75` (resolution-specific), or
- A code cave that computes the correct value at runtime

---

## Display Mode Enumeration Flow

```
DXDraw::Create
  -> IDirectDraw4::EnumDisplayModes(callback=RecordDisplayModeIfUsable)
       For each hardware mode:
         1. Store width/height/bpp/refresh/compat in mode table at g_displayModeCount
         2. Check bpp >= 15 (skip 8bpp/4bpp) -- keep this filter
         3. Check width*3 == height*4 (4:3 only) -- PATCH 1 REMOVES THIS
         4. Increment g_displayModeCount (max 50)
  -> QuickSortRows (bitdepth desc, width desc, height desc)
  -> Scan for desktop-matching mode -> g_defaultWindowedDisplayModeIndex
  -> Scan for 640x480x16 mode -> g_defaultFullscreenDisplayModeIndex -- PATCHES 2-3
  -> Copy selected mode row to g_cachedActiveDisplayModeRow

SelectPreferredDisplayMode (called on driver change)
  -> Scan for 640x480x16 in mode table -- PATCHES 4-5
  -> If not found, return sentinel -100
```

---

## Key Globals Reference

| Global | Address | Type | Purpose |
|--------|---------|------|---------|
| g_displayModeCount | 0x10032678 | int | Number of valid mode table entries |
| g_activeDisplayModeIndex | 0x1003267C | int | Currently selected mode index |
| g_defaultFullscreenDisplayModeIndex | 0x10032680 | int | Preferred fullscreen mode |
| g_defaultWindowedDisplayModeIndex | 0x10032684 | int | Preferred windowed mode |
| g_displayModeWidthTable | 0x10031024 | int[50*5] | Mode table (stride 20 bytes) |
| g_activeRenderWidth | 0x10061BBC | int | Active render width |
| g_activeRenderHeight | 0x10061BC0 | int | Active render height |
| gRenderWidth (EXE) | 0x004AAF08 | float | EXE-side render width |
| gRenderHeight (EXE) | 0x004AAF0C | float | EXE-side render height |
| DAT_00495228 (EXE) | 0x00495228 | int | Frontend virtual width (always 640) |
| DAT_00495200 (EXE) | 0x00495200 | int | Frontend virtual height (always 480) |

---

## Known Limitations

1. **Max 50 display modes**: The mode table is capped at 50 entries (hardcoded at VA 0x100080C0, `CMP ECX, 0x32`). With the 4:3 filter removed, systems with many unique resolutions might exceed this. Modes are sorted highest-first, so the lowest resolutions would be dropped. File offset for this limit: `0x80C1` (the `0x32` immediate in the `CMP ECX, 0x32` instruction).

2. **Frontend stays 640x480 virtual**: The frontend/menu system always renders to a 640x480 virtual canvas. The scale-at-Flip caves (from `patch_menu_widescreen.py`) stretch this to the target resolution. Native-resolution frontend rendering would require extensive frontend coordinate rewrites (100+ references to `DAT_00495228`/`DAT_00495200`).

3. **Loading screen TGA assets**: Loading screen images are 640x480 TGA files with hardcoded stride (`0x280`) and pixel count (`0x4B000`) at EXE offsets `0x2CB4F`, `0x2CB8A`, `0x2CB52`. The scale caves handle presentation, but the source images remain 640x480. Native-resolution loading screens require both new assets and code patches.

4. **Projection focal**: The 0.5625 constant in `.rdata` is a single float shared by all resolutions. A static patch only works for one specific aspect ratio. A proper fix requires a runtime code cave that computes `height * 0.75` (hor+ projection). Without this patch, widescreen will show a horizontally stretched 3D view.

5. **Bit depth preference**: Patches 2-5 keep the 16bpp preference (`CMP EDI, 0x10` at VA 0x10002BCF / `CMP [EAX+4], EDI` at VA 0x10007FAA). On modern systems via dgVoodoo2, 32bpp is typical. If no exact match at the target resolution + 16bpp is found, the fallback logic selects the desktop resolution mode, which is usually correct.

6. **Intro movie**: `PlayIntroMovie` (EXE VA 0x0043C3F0) hardcodes 640x480 for movie dimensions. The movie is a fixed asset; it will display in its original resolution. Low priority.

---

## Patch Application Order

1. **M2DX.dll**: Run `patch_widescreen_m2dx.py <width> <height>`
   - Removes 4:3 filter, updates default mode preferences
2. **TD5_d3d.exe**: Run `td5_widescreen.py <width> <height>`
   - Updates WinMain resolution, gRenderWidth/gRenderHeight globals
3. **TD5_d3d.exe**: Run `patch_menu_widescreen.py`
   - Installs scale-at-Flip caves for frontend presentation
4. **(Optional)** Patch projection focal for correct widescreen FOV

---

## Testing Checklist

### Pre-flight
- [ ] Backup both `M2DX.dll` and `TD5_d3d.exe` before patching
- [ ] Verify dgVoodoo2 is configured for the target resolution
- [ ] Run `patch_widescreen_m2dx.py --verify` to confirm patch state

### Display Mode Enumeration
- [ ] Game starts without crash
- [ ] Display Options menu shows widescreen resolutions (e.g. 1920x1080)
- [ ] The target resolution is selected as default (not 640x480)
- [ ] Resolution list is sorted correctly (highest first)
- [ ] No duplicate or garbage entries in the mode list

### Frontend / Menu
- [ ] Main menu renders correctly (centered, no tearing)
- [ ] All menu screens navigate without visual glitches
- [ ] Options menu renders correctly
- [ ] Frontend transitions (fade in/out) work properly
- [ ] Legal/splash screens display (may be 640x480 scaled)

### Race Rendering
- [ ] Race loads without crash
- [ ] 3D scene fills the full widescreen viewport
- [ ] No horizontal stretching (requires projection focal patch)
- [ ] HUD elements display correctly (speedometer, position, lap)
- [ ] Minimap renders in correct position (bottom-right)
- [ ] Split-screen mode works (if applicable)

### Loading Screens
- [ ] Loading screen displays without crash
- [ ] Image is centered or scaled (not a left-aligned strip)

### Edge Cases
- [ ] Alt-Tab and return to game works
- [ ] Multiple resolution changes in Options menu work
- [ ] Game can restart a race without resolution issues
- [ ] Benchmark mode (state 3) doesn't crash (low priority)

---

## File Offset Quick Reference

### M2DX.dll (ImageBase 0x10000000, file offset = VA - ImageBase)

| Patch | File Offset | VA | Original | Patched | Size |
|-------|-------------|-----|----------|---------|------|
| 4:3 filter JZ->JMP | 0x80B3 | 0x100080B3 | 74 08 | EB 08 | 2 |
| Enum default W | 0x7F93 | 0x10007F93 | BB 80 02 00 00 | BB ww ww ww ww | 5 |
| Enum default H | 0x7FA2 | 0x10007FA2 | 81 38 E0 01 00 00 | 81 38 hh hh hh hh | 6 |
| Preferred W | 0x2BBD | 0x10002BBD | 81 7E F8 80 02 00 00 | 81 7E F8 ww ww ww ww | 7 |
| Preferred H | 0x2BC6 | 0x10002BC6 | 81 7E FC E0 01 00 00 | 81 7E FC hh hh hh hh | 7 |

### TD5_d3d.exe (ImageBase 0x00400000, file offset = VA - ImageBase)

| Patch | File Offset | VA | What |
|-------|-------------|-----|------|
| WinMain W | 0x030AAC | 0x00430AAC | MOV [app+0xBC], width |
| WinMain H | 0x030ABC | 0x00430ABC | MOV [app+0xC0], height |
| PUSH H | 0x02A998 | 0x0042A998 | PUSH height (viewport) |
| PUSH W | 0x02A99D | 0x0042A99D | PUSH width (viewport) |
| Float W | 0x02A9A7 | 0x0042A9A7 | gRenderWidth = float(w) |
| Float H | 0x02A9B1 | 0x0042A9B1 | gRenderHeight = float(h) |
| Projection | 0x05D78C | 0x0045D78C | float 0.5625 (focal) |
