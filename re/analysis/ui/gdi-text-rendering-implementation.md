# GDI Text Rendering Implementation Notes

## Goal
Replace TD5's bitmap glyph atlas text rendering with Windows GDI TextOut for scalable, crisp text and configurable font size.

## Architecture

### Overview
- **DLL** (`td5_gdi_text.dll`): Contains GDI font creation, text rendering replacements, width table updates, and JMP hook installation
- **Code cave** (78B at 0x45C580): Injected into `InitializeFrontendResourcesAndState`, calls `LoadLibraryA("td5_gdi_text.dll")` then `TD5_InitGdiFonts()`
- **Patch script** (`patch_gdi_text.py`): Writes the code cave and redirects the CALL at 0x414A14

### Source Files
- `_organization/build-and-patching/gdi-text/td5_gdi_text.c` — DLL source
- `_organization/build-and-patching/gdi-text/do_build.bat` — MSVC x86 build script
- `_organization/build-and-patching/patch_gdi_text.py` — Binary patcher (apply/verify/revert)

---

## Key Findings During Implementation

### 1. The ACTUALLY-CALLED text functions (verified via E8 CALL search)

The inner drawing functions (`DrawFrontendFontStringPrimary` at 0x424180, etc.) have **ZERO callers** — they are dead code for English. The game calls the **localized wrappers** exclusively:

| Function | Address | Callers | Signature |
|---|---|---|---|
| `DrawFrontendLocalizedStringPrimary` | 0x4242B0 | 4 (0x40EC2F, 0x40EC9C, 0x40ED98, 0x427309) | `int __cdecl (byte *str, uint x, int y)` |
| `DrawFrontendLocalizedStringSecondary` | 0x424390 | 1 (0x4274F8) | `int __cdecl (byte *str, uint x, int y)` |
| `DrawFrontendLocalizedStringToSurface` | 0x424560 | 5 (0x40FF5B, 0x40FF88, 0x41005C, 0x410092, 0x4100AB) | `int __cdecl (byte *str, int x, int y, int *destSurface)` |
| `MeasureOrDrawFrontendFontString` | 0x412D50 | 2 (0x412F9B, 0x412FDB) | `int __cdecl (byte *str, uint x, uint y, int *destSurf)` |
| `MeasureOrCenterFrontendLocalizedString` | 0x424A50 | 10 (0x40EC23, 0x40ED89, 0x40FF4C, 0x40FF79, 0x4108C3, 0x4109 7D, 0x416624, 0x416651, 0x416E6C, 0x417656) | `int __cdecl (byte *str, int startX, int totalWidth)` |
| `DrawFrontendWrappedStringLine` | 0x4248E0 | 1 (0x41BFF7) | `byte* __cdecl (byte *str, int x, int y, int *destSurf, uint maxW)` |

### 2. English path uses INLINE rendering, not the inner functions

For English (`SNK_LangDLL[8] == '0'`), the localized wrappers contain **inline** glyph rendering code (mod/div grid lookup + BltFast) rather than calling `DrawFrontendFontStringPrimary`. This is why hooking the inner functions had no visible effect.

The dispatch in assembly:
```
CMP byte ptr [EAX+8], 0x30    ; SNK_LangDLL[8] == '0'?
JZ  inline_english_path        ; YES -> inline 24x24(?) glyph rendering
; fall through: NOT English -> CALL DrawFrontendFontStringPrimary
```

### 3. None of these functions are in function pointer tables

All 5 target functions have zero DATA xrefs (not stored in tables). They're called exclusively via E8 relative CALLs. Ghidra's xref system missed them because it only indexes addresses stored as data or direct CALL targets in known functions.

### 4. Function prologues (for JMP hook sizing)

All three localized draw wrappers start with the same 8-byte pattern:
```asm
SUB ESP, 0x10          ; 83 EC 10     (3 bytes)
MOV EAX, [0x45D1E4]   ; A1 E4 D1 45 00  (5 bytes)
```
A 5-byte JMP (E9 rel32) cleanly overwrites the SUB ESP + first 2 bytes of MOV. Safe because no internal jumps target bytes 0-4.

`MeasureOrCenterFrontendLocalizedString` starts with:
```asm
MOV EAX, [0x45D1E4]   ; A1 E4 D1 45 00  (5 bytes)
```
Exact 5-byte overwrite, cleanest possible.

### 5. InitializeFrontendResourcesAndState entry point

The function Ghidra labels at 0x414790 actually starts at **0x414740** (Ghidra had a sub-function boundary). It's called from `RunMainGameLoop` at **0x4421FB**.

The injection point (CALL at 0x414A14) is near the end of this function, AFTER all font TGA surfaces are loaded to DDraw surfaces (0x49626C, 0x496270, 0x496274, 0x496278, 0x49627C).

Original bytes at 0x414A14: `E8 27 FC FF FF` → CALL 0x414640

---

## Code Cave Layout

Located at 0x45C580 in .rdata zero-padding (after existing widescreen caves ending at ~0x45C577).

```
Offset  Bytes  Purpose
0       5      CALL 0x414640 (original target)
5       1      PUSHAD
6       5      PUSH str_dll_name (0x45C5AC)
11      6      CALL [LoadLibraryA IAT 0x45D02C]
17      2      TEST EAX, EAX
19      2      JZ .done
21      5      PUSH str_init_name (0x45C5BD)  ← was off-by-one bug, fixed
26      1      PUSH EAX (hModule)
27      6      CALL [GetProcAddress IAT 0x45D0E0]
33      2      TEST EAX, EAX
35      2      JZ .done
37      2      CALL EAX (TD5_InitGdiFonts)
39      1      POPAD
40      1      RET
41      3      NOP padding
44      17     "td5_gdi_text.dll\0"
61      17     "TD5_InitGdiFonts\0"
Total: 78 bytes
```

### Bug found and fixed
Initial version had `STR_INIT_VA = CAVE_VA + 62` but the string starts at offset 61 (right after the 17-byte DLL name string at offset 44). This caused `GetProcAddress` to receive `"D5_InitGdiFonts"` (missing leading 'T'), silently failing. Fixed to `CAVE_VA + 44 + 17 = CAVE_VA + 61`.

---

## DLL Implementation Details

### GDI Approach
- `CreateFontA` with configurable height, weight, and face name
- `IDirectDrawSurface::GetDC()` (vtable offset 0x44) to get HDC from DDraw surfaces
- `SetBkMode(TRANSPARENT)` + `SetTextColor(white)` + `TextOutA`
- `IDirectDrawSurface::ReleaseDC()` (vtable offset 0x68)
- `GetTextExtentPoint32A` for width measurement

### DDraw COM vtable offsets (confirmed)
- BltFast: vtable + 0x1C (index 7)
- GetDC: vtable + 0x44 (index 17)
- ReleaseDC: vtable + 0x68 (index 26)

### GetDC works on dgVoodoo2 surfaces
Confirmed working: `SurfGetDC` on dgVoodoo2-wrapped DDraw surfaces returns valid HDC. GDI TextOut renders correctly to these surfaces.

### Hook installation
Uses `VirtualProtect(PAGE_EXECUTE_READWRITE)` to temporarily make .text writable, writes 5-byte `JMP rel32` at function entry, restores original protection, flushes instruction cache.

### Width table updates
Updates 5 tables (0x4662D0, 0x4662F0, 0x4663E4, 0x4664F8, 0x466518) with per-character GDI-measured widths using `GetTextExtentPoint32A`. Tables are `signed char[128]`, values clamped to ±120. Uses `VirtualProtect` to ensure writability (they're in .data, should already be RW).

### Configuration (in td5_gdi_text.c)
```c
#define SMALL_FONT_HEIGHT   -14     /* body text */
#define LARGE_FONT_HEIGHT   -28     /* menu titles */
#define FONT_WEIGHT         FW_BOLD
#define FONT_FACE           "Arial"
#define TEXT_COLOR           RGB(255, 255, 255)
```

---

## IAT Addresses (TD5_d3d.exe KERNEL32 imports)
| Function | IAT VA |
|---|---|
| LoadLibraryA | 0x45D02C |
| GetProcAddress | 0x45D0E0 |
| VirtualAlloc | 0x45D0B4 |
| VirtualFree | 0x45D0B8 |

Note: `VirtualProtect` is NOT in the EXE's IAT. The DLL imports it from its own PE import table.

---

## Test Results

### What worked
- DLL loads successfully via code cave
- GDI fonts created (small 14px bold Arial, large 28px bold Arial)
- GetDC on dgVoodoo2 DDraw surfaces returns valid HDC
- Width tables updated with GDI metrics
- JMP hooks installed at all 5 function entry points
- **Menu text renders correctly with GDI fonts** (all button labels, menu titles, car/track names)
- Text is visibly different (Arial bold instead of bitmap font)

### What crashed
- Game crashed when transitioning from menu to race (on the loading splash screen)
- Investigation showed the crash was NOT caused by the GDI hooks — the last logged draw call completed successfully, and no hooked functions were called during the crash
- The widescreen patches in the same EXE were **heavily corrupted** by another Claude instance working in parallel, which is the actual cause of the crash

### Remaining limitation
- Text is rendered at 640×480 virtual resolution (then upscaled by widescreen patch). For truly crisp native-res text, a future "Phase 1b" would render text post-upscale at 2560×1440.

---

## Widescreen Cave Layout Reference (for conflict avoidance)

```
0x45C1A1  62B   dead (old centering cave)
0x45C1F5  52B   COLORFILL cave
0x45C229  34B   BltFast trampoline
0x45C250  16B   Blt bg destRect
0x45C260  16B   Blt bg srcRect
0x45C270  56B   menu-bg alternate Blt cave
0x45C2B0  33B   cave_E (surface pre-alloc)
0x45C2D3  35B   dead (old car-bg cave)
0x45C2F6  25B   d3d190 cave
0x45C310  16B   srcRect data
0x45C320  16B   destRect data
0x45C330  92B   cave_A code (scale pre-flip, path A)
0x45C38C  93B   cave_B code (scale pre-flip, path B)
0x45C3E9  92B   cave_C code (scale pre-flip, path C)
0x45C445  92B   cave_D code (scale pre-flip, path D)
0x45C500  119B  cave_C_new (lazy-alloc loading screen)
--- GDI text cave starts here ---
0x45C580  78B   GDI DLL loader cave + strings
0x45C5CE  ...   FREE SPACE → 0x45D000
```
