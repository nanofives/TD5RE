# Widescreen 640x480 Hardcoded Constants Audit
## TD5_d3d.exe -- Comprehensive Function-by-Function Analysis
### Date: 2026-03-19

---

## METHODOLOGY

Searched for:
- Immediate `0x280` (640) in .text -- 26 hits
- Immediate `0x1E0` (480) in .text -- 36 hits
- Immediate `0x140` (320) in .text -- 24 hits
- Float `44200000` (640.0) -- 1 hit (InitializeRaceVideoConfiguration)
- Float `43F00000` (480.0) -- 1 hit (InitializeRaceVideoConfiguration)
- Xrefs to `DAT_004aaf08` (gRenderWidth float) -- 24 refs
- Xrefs to `DAT_004aaf0c` (gRenderHeight float) -- 23 refs
- Xrefs to `DAT_00495228` (frontend virtual width) -- 100+ refs
- Xrefs to `DAT_00495200` (frontend virtual height) -- 100+ refs
- Xrefs to `.rdata` float 0.5625 at `0x45d78c` -- 1 ref
- Xrefs to `.rdata` float 0.75 at `0x45d780` -- 1 ref

---

## CATEGORY 1: ALREADY PATCHED (skip)

| Function | Address | What | Status |
|---|---|---|---|
| GameWinMain | 0x00430a90 | WinMain width/height params (0x280, 0x1E0) | PATCHED (file 0x030AAC, 0x030ABC) |
| InitializeRaceVideoConfiguration | 0x0042a950 | `gRenderWidth = 640.0; gRenderHeight = 480.0` + `ConfigureProjectionForViewport(0x280, 0x1E0)` | PATCHED (file 0x02A998/9D/A7/B1) |
| InitializeFrontendResourcesAndState | 0x00414720 | Sets `DAT_00495228=640, DAT_00495200=480` (frontend virtual canvas) | PATCHED |
| ConfigureProjectionForViewport | 0x0043e7e0 | `focal = width * 0.5625` (0x45d78c) -- 4:3 projection assumption | PATCHED (projection focal) |
| Scale-at-Flip cave | 0x45C330 | Blt scaling for frontend | PATCHED |
| BltFast trampoline | 0x45C229 | Frontend sprite positioning | PATCHED |

---

## CATEGORY 2: CONFIRMED NON-ISSUES (no patch needed)

### 2A. Fully Dynamic (read globals, no hardcoded 640/480)

| Function | Address | Analysis |
|---|---|---|
| **InitializeRaceHudLayout** | 0x00437ba0 | Reads `gRenderWidth` / `gRenderHeight` dynamically. Computes `scale_x = width * factor`, `scale_y = height * factor` using .rdata float tables. All HUD element positions are edge-relative. **NO hardcoded 640/480.** WIDESCREEN-CORRECT. |
| **InitializeRaceViewportLayout** | 0x0042c2b6 | Builds single/split viewport tables entirely from `gRenderWidth`/`gRenderHeight`/`gRenderCenterX`/`gRenderCenterY`. **Fully dynamic.** |
| **InitializeRaceSession** | 0x0042aa10 | After `FullScreen()`, reads `app_exref+0xBC/0xC0` dynamically into gRenderWidth/gRenderHeight, then calls `ConfigureProjectionForViewport`, `InitializeRaceHudLayout`, `InitializeRaceViewportLayout`. **The race init path is already dynamic** -- the only hardcoded 640/480 is in `InitializeRaceVideoConfiguration` (already patched). |
| **RunRaceFrame** | 0x0042b580 | Uses gRenderWidth/gRenderHeight throughout for clip bounds, fade calculations, viewport setup. **No hardcoded resolution.** |
| **RenderRaceHudOverlays** | 0x004395fd | All HUD positions from pre-computed DAT_004b0fa4 layout tables (set by InitializeRaceHudLayout). Uses `gRenderWidth`/`gRenderHeight` for SetProjectedClipRect. **Dynamic.** |
| **FlushProjectedPrimitiveBuckets** | 0x0043e2f0 | 4096-bucket depth sort. No resolution constants at all. Pure rendering dispatch. |
| **FlushQueuedTranslucentPrimitives** | 0x00431340 | Translucent batch flush. No resolution constants. Pure rendering dispatch. |
| **InitializeMinimapLayout** | 0x0043b116 | Builds minimap from `DAT_004b1138` (gRenderWidth * scale) and `DAT_004b113c` (gRenderHeight * scale). **Dynamic.** Uses `gRenderHeight` directly for one offset. |
| **UpdateWantedDamageIndicator** | 0x0043d501 | Reads `gRenderWidth` to compute indicator size. **Dynamic.** |
| **ApplyMeshProjectionEffect** | 0x0043e014 | References 0x45d780 (0.75) as a UV/projection offset for mesh effect mode 3. This is a texture-space constant, NOT a screen resolution constant. **NOT affected by widescreen.** |

### 2B. Frontend Functions Using DAT_00495228/DAT_00495200 (Virtual Canvas)

These all read the frontend virtual canvas globals (currently hardcoded 640x480 in `InitializeFrontendResourcesAndState`). They compute `half_w = DAT_00495228 >> 1` and `half_h = DAT_00495200 >> 1` for centering. Since the frontend uses scale-at-Flip (already patched), these are all correct -- they render to a 640x480 virtual canvas which gets scaled to fullscreen.

| Function | Address | What | Verdict |
|---|---|---|---|
| **RunFrontendDisplayLoop** | 0x00414b50 | `BlitFrontendCachedRect(0,0,0x280,0x1E0)` -- blits full 640x480 frontend canvas | NON-ISSUE: operates on virtual canvas |
| **ScreenMainMenuAnd1PRaceFlow** | 0x00415490 | Uses `half_w - offsets` for button centering | NON-ISSUE: virtual canvas coordinates |
| **RunFrontendConnectionBrowser** | 0x00418D50 | Network connection screen, all coordinates relative to half_w/half_h | NON-ISSUE: virtual canvas |
| **RenderFrontendSessionBrowser** | 0x00419CF0 | Network session list redraw | NON-ISSUE: virtual canvas |
| **RenderFrontendLobbyStatusPanel** | 0x0041B4F0 | `BltColorFillToSurface(0,0,0,0x1E0,0x20,...)` -- fills 480-wide status panel | NON-ISSUE: virtual canvas surface size |
| **RunFrontendNetworkLobby** | 0x0041C354 | Network lobby, creates surfaces `0x1E0` wide | NON-ISSUE: virtual canvas |
| All other frontend screens (100+ xrefs) | Various | Read DAT_00495228/DAT_00495200 for positioning | NON-ISSUE: virtual canvas + scale-at-Flip |

### 2C. Non-Display Functions (CRT / Data)

| Function | Address | Analysis |
|---|---|---|
| **FUN_0044ca49** | 0x0044ca49 | CRT `sscanf` implementation. 0x1E0 is coincidental byte pattern. |
| **FUN_00450dfc** | 0x00450dfc | CRT floating-point string parser. 0x1E0 is coincidental. |
| **FUN_004590bf** / FUN_004592ad / FUN_0045949b / FUN_00459689 | 0x459000+ | IDCT/decoder functions in `IDCT_DAT` section. 0x280/0x140 are DCT block dimensions. **NOT display.** |
| **WriteBenchmarkResultsTgaReport** | 0x00428e66 | TGA screenshot writer. Hardcodes 640x480 for benchmark capture image. **Non-display** (writes file, not screen). |
| **Data addresses** (0x464418, 0x474834, 0x4821cc, etc.) | .data/.rdata | Static data containing 640/480 as stored values. Not code. |

---

## CATEGORY 3: NEEDS INVESTIGATION / POTENTIAL PATCHES

### 3A. DisplayLoadingScreenImage (0x0042CA20) -- NEEDS PATCH

**Hardcoded values:**
- `0x280` (640) at VA 0x42CB4F: pixel copy loop width clamp `if (iVar4 < 0x280)`
- `0x280` (640) at VA 0x42CB8A: row stride `iVar6 = iVar6 + 0x280`
- `0x4B000` (640*480 = 307200) at VA 0x42CB52: total pixel count `if (iVar6 < 0x4b000)`
- `0x140` (320) at VA 0x42CA7F: related to the same function's TGA allocation

**What it does:** Copies a decoded TGA loading screen image into the primary DirectDraw surface. The inner loop hardcodes width=640 and total size=640*480 pixels. If the DD surface is wider than 640, only a 640-pixel strip gets copied, with the rest zeroed.

**Widescreen impact:** Loading screen will display as a 640-pixel-wide strip on the left side of a widescreen surface. However, since loading screens are 640x480 TGA assets, the correct behavior is to either:
1. Keep the 640x480 copy and let scale-at-Flip handle presentation (if this path goes through Flip with scale cave active), OR
2. Center the 640x480 image in the widescreen surface

**Current status:** The scale cave activates when `state==1` (menu/frontend). Loading happens during transition from state 1 to state 2. Need to verify if scale cave is active during `DisplayLoadingScreenImage`.

**Recommendation:** MEDIUM PRIORITY. If scale cave handles this already, no patch needed. If not, the 0x280 stride and 0x4B000 limit should be changed to use the actual surface dimensions.

**File offsets:**
- 0x2CB4F: the `0x280` width clamp
- 0x2CB8A: the `0x280` row stride
- 0x2CB52: the `0x4B000` total pixel limit

---

### 3B. RunMainGameLoop State 3 -- Benchmark Image Display (0x00442170) -- LOW PRIORITY

**Hardcoded values:**
- `0x280` at VA 0x44248C: same pixel copy pattern as DisplayLoadingScreenImage
- `0x280` at VA 0x4424CD: row stride
- `0x4B000`: total pixel limit

**What it does:** State 3 of the game loop displays a static benchmark result TGA image. Same 640x480 hardcoded copy logic.

**Widescreen impact:** Benchmark image will appear as a 640-pixel strip. Very rarely triggered (only after benchmark mode).

**Recommendation:** LOW PRIORITY. Benchmark mode is a debug/developer feature.

---

### 3C. ScreenExtrasGallery (0x00417FA0) -- LOW PRIORITY

**Hardcoded values:**
- `0x280` at VA 0x4180E5/0x4180EC: credits scroll surface dimensions (0x280 wide, 0x280 height)
- `0x140` at VA 0x4180FC/0x41813D/0x4182CD: half-width for credits scroll split

**What it does:** The credits/extras gallery screen creates a 640x640 scrolling credits surface and scrolls text through it. The scroll logic uses `0x280` as the wrap-around point and `0x140` as the split point.

**Widescreen impact:** Credits run inside the frontend virtual canvas (640x480), so scale-at-Flip handles presentation. The 0x280/0x140 constants are internal to the credits surface, not screen dimensions.

**Recommendation:** LOW PRIORITY / NON-ISSUE. The credits surface is self-contained and gets presented through the frontend Flip path.

---

### 3D. PlayIntroMovie (0x0043C3F0) -- LOW PRIORITY

**Hardcoded values:**
- `0x280` at VA 0x43C4A4: `_DAT_00474834 = 0x280` (movie width)
- `0x1E0` at VA 0x43C4AE: `_DAT_00474838 = 0x1E0` (movie height)

**What it does:** Sets the intro movie playback dimensions to 640x480. These are passed to the TGQ movie decoder.

**Widescreen impact:** The movie is a 640x480 asset. The movie player creates its own DirectDraw surfaces. Stretching/centering depends on the movie player's surface presentation path.

**Recommendation:** LOW PRIORITY. Movie plays before frontend is initialized. The movie player likely handles its own surface management. Could cause letterboxing issues but the movie only plays once at startup.

---

### 3E. ScreenLocalizationInit (0x00426990) -- NON-ISSUE / SKIP

**Hardcoded values:**
- `0x280` at VA 0x4270DC: display mode search `if (width == 0x280 && height == 0x1E0 && bpp == 0x10)`
- `0x1E0` at VA 0x4270F1: same conditional

**What it does:** When the display mode table has changed since last boot, searches for 640x480x16 as a safe default fallback. If not found, picks the largest mode smaller than 640.

**Widescreen impact:** This only triggers on first boot or when the display mode table changes. After the M2DX 4:3 filter patch allows widescreen modes, this fallback might pick a non-ideal default, but the user will select their preferred mode in the options menu anyway.

**Recommendation:** NON-ISSUE for normal operation. The fallback heuristic is acceptable -- it just sets the initial default, which the user overrides.

---

## CATEGORY 4: .rdata FLOAT CONSTANTS

| Address | Value | Used By | Purpose | Needs Patch? |
|---|---|---|---|---|
| `0x45d78c` | 0.5625 (9/16) | ConfigureProjectionForViewport | `focal = width * 0.5625` | **YES** -- ALREADY IDENTIFIED. Change to `focal = height * 0.75` for hor+ |
| `0x45d780` | 0.75 (3/4) | ApplyMeshProjectionEffect (mode 3) | UV offset for mesh projection effect. NOT resolution-related. | NO |
| `0x45d784` | 0.625 | (check needed) | Unknown | Likely not resolution-related |
| `0x45d788` | 0.375 | (check needed) | Unknown | Likely not resolution-related |
| `0x45d5d0` | 0.5 | Many functions | Generic half multiplier | NO |

---

## SUMMARY TABLE

| # | Function | Address | Hardcoded | Display? | Needs Patch? | Priority |
|---|---|---|---|---|---|---|
| 1 | GameWinMain | 0x430a90 | 640, 480 | Yes | **DONE** | - |
| 2 | InitializeRaceVideoConfiguration | 0x42a950 | 640.0, 480.0, 640, 480 | Yes | **DONE** | - |
| 3 | InitializeFrontendResourcesAndState | 0x414720 | 640, 480 | Yes | **DONE** | - |
| 4 | ConfigureProjectionForViewport | 0x43e7e0 | 0.5625 | Yes | **IDENTIFIED** | P0 |
| 5 | DisplayLoadingScreenImage | 0x42CA20 | 640, 307200 | Yes | **MAYBE** | P1 |
| 6 | RunMainGameLoop (state 3) | 0x442170 | 640, 307200 | Yes | Maybe | P3 |
| 7 | ScreenExtrasGallery | 0x417FA0 | 640, 320 | Internal | No | - |
| 8 | PlayIntroMovie | 0x43C3F0 | 640, 480 | Yes | Maybe | P3 |
| 9 | ScreenLocalizationInit | 0x426990 | 640, 480 | Fallback | No | - |
| 10 | RunFrontendDisplayLoop | 0x414B50 | 640, 480 | Virtual | No | - |
| 11 | ScreenMainMenuAnd1PRaceFlow | 0x415490 | 640 | Virtual | No | - |
| 12 | InitializeRaceHudLayout | 0x437BA0 | (none) | Yes | No | - |
| 13 | InitializeRaceViewportLayout | 0x42C2B6 | (none) | Yes | No | - |
| 14 | InitializeRaceSession | 0x42AA10 | (none, reads dynamic) | Yes | No | - |
| 15 | RunRaceFrame | 0x42B580 | (none) | Yes | No | - |
| 16 | RenderRaceHudOverlays | 0x4395FD | (none) | Yes | No | - |
| 17 | FlushProjectedPrimitiveBuckets | 0x43E2F0 | (none) | Yes | No | - |
| 18 | FlushQueuedTranslucentPrimitives | 0x431340 | (none) | Yes | No | - |
| 19 | InitializeMinimapLayout | 0x43B116 | (none) | Yes | No | - |
| 20 | UpdateWantedDamageIndicator | 0x43D501 | (none) | Yes | No | - |
| 21 | All network frontend screens | Various | 480 | Virtual | No | - |
| 22 | WriteBenchmarkResultsTgaReport | 0x428E66 | 640 | File I/O | No | - |
| 23 | IDCT functions | 0x459000+ | 640, 320 | DCT math | No | - |
| 24 | ApplyMeshProjectionEffect | 0x43E014 | 0.75 ref | UV math | No | - |
| 25 | CRT sscanf/float parser | 0x44CA49/0x450DFC | coincidental | No | No | - |

---

## REMAINING WIDESCREEN PATCH CHECKLIST

### P0 -- Critical (blocks correct widescreen rendering)
1. **M2DX 4:3 filter**: file 0x80B3: `74`->`EB` -- removes aspect ratio gate
2. **Projection focal**: Change `focal = width * 0.5625` to `focal = height * 0.75` at 0x45d78c

### P1 -- Important (visual glitches)
3. **Legal screens**: Extend scale cave state check from `==1` to `<2` + NULL guard
4. **DisplayLoadingScreenImage**: Verify if scale cave covers this; if not, patch stride/limit

### P2 -- Nice to have
5. (None identified -- all race rendering is already dynamic)

### P3 -- Low priority / cosmetic
6. **RunMainGameLoop state 3**: Benchmark image display (developer feature)
7. **PlayIntroMovie**: Movie dimensions (plays once, before frontend)

---

## KEY FINDING

The race rendering pipeline (3D world, HUD, minimap, particles, translucent flush, projected primitives) is **entirely widescreen-correct** once the following are patched:
- `InitializeRaceVideoConfiguration` sets correct initial gRenderWidth/gRenderHeight (DONE)
- `ConfigureProjectionForViewport` uses hor+ focal length (P0 pending)

The frontend system operates on a 640x480 virtual canvas with scale-at-Flip (PATCHED), making all 100+ frontend coordinate references non-issues.

**No hidden resolution bottleneck was found.** The P0 projection fix and M2DX filter patch are the only blockers for correct widescreen gameplay rendering.
