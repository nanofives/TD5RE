# Frontend Rendering Internals — Deep Dive

> 57 functions decompiled from TD5_d3d.exe (port 8195)
> Covers: dual-surface system, presentation pipeline, sprite overlay queue,
> button creation, string rendering (font system + localization), gallery slideshow,
> surface registry, and fade/dither system.

---

## Architecture Overview

The TD5 frontend rendering system is built on a **dual off-screen surface** architecture with a
**deferred sprite overlay queue** and a **software presentation path**. All UI is composited in
16-bit color (RGB555 or RGB565) on two work surfaces before being presented to the DirectDraw
back-buffer.

### Key Globals

| Address | Type | Name | Description |
|---------|------|------|-------------|
| `0x00496260` | IDirectDrawSurface* | **Primary work surface** | Main frontend composition target |
| `0x00496264` | IDirectDrawSurface* | **Secondary work surface** | Snapshot/scratch buffer for transitions |
| `0x00495220` | IDirectDrawSurface* | **Back-buffer** (presentation target) | DirectDraw back-buffer surface |
| `0x00495228` | int | **Virtual width** | Frontend virtual resolution width (640) |
| `0x00495200` | int | **Virtual height** | Frontend virtual resolution height (480) |
| `0x0049525c` | int | **Pixel format** | Bit depth: 0xF = RGB555 (15-bit), 0x10 = RGB565 (16-bit) |
| `0x004951ec` | int | **HW blit flag** | If nonzero, use hardware BltFast; if 0, use software copy loop |
| `0x004951fc` | int | **Double-buffer index** | Alternates 0/1 for dirty-rect double-buffering |
| `0x0049626c` | IDirectDrawSurface* | **Font atlas surface** | 12x12 glyph atlas (21 columns, ASCII 0x20+) |
| `0x0049627c` | IDirectDrawSurface* | **Small font atlas** | Secondary small font (same 12x12 layout) |
| `0x00496278` | IDirectDrawSurface* | **Large menu font atlas** | 36x36 glyph atlas (7 columns) for menu labels |
| `0x00496268` | IDirectDrawSurface* | **Button frame atlas** | 9-slice button frame source (ButtonBits.tga) |
| `0x00496284` | IDirectDrawSurface* | **Arrow sprite atlas** | Left/right arrow sprites for display-mode buttons |

### Font Metrics Tables

| Address | Description |
|---------|-------------|
| `0x004662d0` | char[128] — Per-character advance width (12x12 font) |
| `0x004662f0` | char[128] — Per-character trailing space (12x12 font) |
| `0x004663e4` | char[128] — Per-character vertical offset (12x12 font) |
| `0x004664f8` | char[128] — Per-character advance width (36x36 menu font) |
| `0x00466518` | char[128] — Per-character trailing space (36x36 menu font) |
| `0x004660c8` | char[128] — Per-character advance width (24x24 localized font) |
| `0x004660e8` | char[128] — Per-character trailing space (24x24 localized font) |

---

## 1. Color Conversion — RGB888 to Native 16-bit

Every fill/color function shares the same inline color conversion block. The frontend pixel
format stored at `DAT_0049525c` selects between two pack modes:

- **0x0F (RGB555)**: `packed = ((rgb >> 3 & 0x1F0000 | rgb & 0xF800) >> 3 | rgb & 0xF8) >> 3`
  - R[23:19] -> bits[14:10], G[15:11] -> bits[9:5], B[7:3] -> bits[4:0]
- **0x10 (RGB565)**: `packed = ((rgb >> 3 & 0x1F0000 | rgb & 0xF800) >> 2 | rgb & 0xF8) >> 3`
  - R[23:19] -> bits[15:11], G[15:11] -> bits[10:5], B[7:3] -> bits[4:0]

The input `param_1` is always a 24-bit 0x00RRGGBB value. The packed result is stored in a
DDBLTFX structure at offset 0x50 (dwFillColor) for COLORFILL Blt operations.

---

## 2. Surface Fill Functions (0x423DB0 -- 0x424050)

All five fill functions share identical structure: convert color, build a DDBLTFX, call
`IDirectDrawSurface::Blt` with `DDBLT_COLORFILL` (flag 0x400).

### `ClearBackbufferWithColor` (0x423DB0)
- Target: **Primary work surface** (`DAT_00496260`)
- Rect: NULL (entire surface)
- Converts RGB888 to native 16-bit fill color

### `LockSecondaryFrontendSurfaceFillColor` (0x423E40)
- Target: **Secondary work surface** (`DAT_00496264`)
- Rect: NULL (entire surface)
- Returns the DDBLTFX lock result (surface pitch from `uStack_2c`)

### `FillPrimaryFrontendRect` (0x423ED0)
- Target: **Primary work surface**
- Rect: `(x, y, x+w, y+h)` from params 2-5
- Returns surface pitch

### `FillSurfaceRectWithColor` (0x423F90)
- Target: **Secondary work surface**
- Rect: `(x, y, x+w, y+h)` from params 2-5
- Returns surface pitch

### `BltColorFillToSurface` (0x424050)
- Target: **Arbitrary surface** passed as `param_6`
- Rect: `(x, y, x+w, y+h)` from params 2-5
- Used for button highlight frames, menu framing, etc.

---

## 3. String Rendering — Font System

The frontend has **three font sizes** and **two rendering modes** (standard English vs localized):

### Font Atlases

| Font | Glyph Size | Atlas Layout | Surface |
|------|-----------|--------------|---------|
| Standard (12x12) | 12x12 px | 21 columns, row = `(ch-0x20)/21`, col = `(ch-0x20)%21` | `DAT_0049626c` |
| Localized (24x24) | 24x24 px | 10 columns, row = `(ch-0x20)/10`, col = `(ch-0x20)%10` | `DAT_0049626c` (same surface!) |
| Large menu (36x36) | 36x36 px | 7 columns, row = `(ch-0x20)/7`, col = `(ch-0x20)%7` | `DAT_00496278` |
| Small (12x12) | 12x12 px | 21 columns (same layout as standard) | `DAT_0049627c` |

All rendering uses `IDirectDrawSurface::BltFast` (vtable offset +0x1C) with flag 0x11
(DDBLTFAST_SRCCOLORKEY | DDBLTFAST_WAIT).

### Localization Switch

The localization functions check `SNK_LangDLL_exref[8]` (the character at index 8 in the
language DLL string). If it equals `'0'` (0x30), the **24x24 localized glyph layout** is used.
Otherwise, they fall through to the standard 12x12 English layout. This allows non-English
builds to use wider glyphs for accented characters.

### String Drawing Functions

| Address | Function | Target Surface | Font |
|---------|----------|---------------|------|
| 0x424110 | `DrawFrontendFontStringPrimary` | Primary (0x496260) | 12x12 standard |
| 0x4241E0 | `DrawFrontendFontStringSecondary` | Secondary (0x496264) | 12x12 standard |
| 0x4242B0 | `DrawFrontendLocalizedStringPrimary` | Primary | 12x12 or 24x24 |
| 0x424390 | `DrawFrontendLocalizedStringSecondary` | Secondary | 12x12 or 24x24 |
| 0x424470 | `DrawFrontendFontStringToSurface` | Any (param_4) | 12x12 + vertical offsets |
| 0x424560 | `DrawFrontendLocalizedStringToSurface` | Any (param_4) | 12x12 or 24x24 + width clamp |
| 0x424660 | `DrawFrontendSmallFontStringToSurface` | Any (param_4) | 12x12 small (0x49627c) |
| 0x412D50 | `MeasureOrDrawFrontendFontString` | Any or NULL (measure-only) | 36x36 menu |

**Return value**: All draw functions return the total pixel width of the rendered string, including
a trailing padding term (typically +0x18 for 24x24 or +0xC for 12x12).

### `DrawFrontendFontStringToSurface` (0x424470) — Vertical Offsets

This variant applies a per-character **vertical offset** from `DAT_004663e4[ch]`, adding it to
the Y position. This is used for baseline-aligned rendering where characters like lowercase
letters need different vertical positioning.

### `DrawFrontendLocalizedStringToSurface` (0x424560) — Width Clamping

The localized-to-surface variant clamps source rect width to 0x18 (24px) maximum per glyph,
preventing oversized characters from causing visual artifacts.

### String Measurement Functions

| Address | Function | Description |
|---------|----------|-------------|
| 0x424890 | `MeasureOrCenterFrontendString` | If param_3=0: returns pixel width. If param_3!=0: returns centered X within width. |
| 0x424A50 | `MeasureOrCenterFrontendLocalizedString` | Same, with localization glyph width switch. |
| 0x412D50 | `MeasureOrDrawFrontendFontString` | If surface=NULL: measure only. Otherwise: draw + measure. Uses 36x36 font. |

### `DrawFrontendWrappedStringLine` (0x4248E0) — Word Wrapping

Implements word-by-word text wrapping:
1. Extracts next word (space-delimited) into a 256-byte local buffer
2. Measures word width using standard advance table
3. If word fits in remaining `param_5` budget, draws via `DrawFrontendClippedStringToSurface`
4. If word doesn't fit, either returns the current position (word break) or truncates mid-word
5. Returns pointer to next undrawn character in source string

### `CreateMenuStringLabelSurface` (0x412E30)

Creates a dedicated surface for a menu string label:
- **English path** (lang != '0'): Measures string with 36x36 font, creates surface of exact
  width x 36px, fills black, draws text. Sets `DAT_004962C4` = width, `DAT_004962C8` = 0x24 (36),
  `DAT_004962CC` = 0x10 (color key type).
- **Localized path** (lang == '0'): Loads pre-rendered TGA from `Front_End/*.tga` inside
  `FrontEnd.zip`. Locks surface to validate, sets height = 0x14 (20), color key = 0.

### `DrawPostRaceHighScoreEntry` (0x413010)

Renders the post-race high score table header into surface `DAT_0049628C`:
- Clears surface with black (520x144 px)
- Draws column headers: Name (0x10-0x70), Car (0xE4-0x150), Avg (0x160-0x1AC), Top (0x1BC-0x208)
- Conditionally draws Time, Lap, or Points column based on race type:
  - Type 0 (point-to-point): Time header
  - Type 1 (circuit): Lap header
  - Type 2 (knockout): Points header
- All headers use `DrawFrontendSmallFontStringToSurface` with centered X positions

### `SetSurfaceColorKeyFromRGB` (0x412B00)

Sets the transparent color key on a surface:
- Converts RGB888 to native 16-bit format
- Calls `IDirectDrawSurface::SetColorKey` (vtable +0x74) with flag 8 (DDCKEY_SRCBLT)
- Color key range = single value (low == high)

---

## 4. Presentation Pipeline (0x424AF0 -- 0x424D90)

The frontend uses a three-tier surface hierarchy:

```
Primary Work Surface (0x496260)  <-->  Secondary Work Surface (0x496264)
           |                                       |
           +-----------> Back-buffer (0x495220) <---+
                          (presentation target)
```

### Presentation Functions

| Address | Function | Source | Dest | Scope |
|---------|----------|-------|------|-------|
| 0x424AF0 | `PresentPrimaryFrontendBufferViaCopy` | Primary | Back-buffer | Full (via `Copy16BitSurfaceRect`) |
| 0x424CA0 | `PresentPrimaryFrontendBuffer` | Primary | Back-buffer | Full (via BltFast) |
| 0x424D40 | `PresentPrimaryFrontendRect` | Primary | Back-buffer | Rect (via BltFast) |
| 0x424CF0 | `PresentSecondaryFrontendRect` | Secondary | Back-buffer | Rect (via BltFast) |
| 0x424C10 | `PresentSecondaryFrontendRectViaCopy` | Secondary | Back-buffer | Rect (via `Copy16BitSurfaceRect`) |

### Inter-Surface Copy Functions

| Address | Function | Source | Dest | Scope |
|---------|----------|-------|------|-------|
| 0x424B30 | `CopyPrimaryFrontendBufferToSecondary` | Primary | Secondary | Full |
| 0x424BC0 | `CopyPrimaryFrontendRectToSecondary` | Primary | Secondary | Rect |
| 0x424C50 | `BlitSecondaryFrontendRectToPrimary` | Secondary | Primary | Rect |
| 0x424B80 | `BlitFrontendCachedRect` | Primary | Back-buffer | Rect (via `Copy16BitSurfaceRect`) |

### `FillPrimaryFrontendScanline` (0x424D90)

Fills a 1-pixel-high horizontal strip across the full primary surface width at the given Y
coordinate. Used for the split-screen divider line.

### `Copy16BitSurfaceRect` (0x4251A0) — Software Blit Path

The software presentation fallback. Behavior depends on `DAT_004951EC`:

- **If `DAT_004951EC != 0`** (hardware path): Delegates directly to
  `IDirectDrawSurface::BltFast` on the back-buffer. Single call, returns.
- **If `DAT_004951EC == 0`** (software path):
  1. Locks the back-buffer (`DAT_00495220`) with `IDirectDrawSurface::Lock`
  2. Locks the source surface with `Lock`
  3. Computes row pointers: `dest = lockPtr + (pitch/2 * destY + destX) * 2`
  4. Copies rect pixel-by-pixel in a tight inner loop
  5. If flag bit 0 clear: straight copy (`*dst = *src`)
  6. If flag bit 0 set: skips source pixels matching the color key (transparency)
  7. Unlocks both surfaces

This software path exists because some DirectDraw drivers (particularly software renderers)
don't support BltFast or have broken color-key blitting.

---

## 5. Sprite Overlay Queue (0x4254D0 -- 0x4258E0)

The frontend uses a **deferred sprite blit system** with dirty-rect tracking for efficient
partial-screen updates. Up to **64 sprites** can be queued per frame.

### Data Structures

**Sprite blit queue** at `DAT_00497AD0`:
- 64 entries, 0x30 (48) bytes each, stride 0xC dwords
- Fields per entry: destX, destY, destX2, destY2, srcX, srcY, srcX2, srcY2, colorKey[2],
  surfacePtr, surfaceRegistryId

**Overlay rect queue** at `DAT_00498F40`:
- 64 entries, 0x34 (52) bytes each, stride 0xD dwords
- Fields per entry: destX, destY, destX2, destY2, srcX, srcY, srcX2, srcY2, color, surfacePtr,
  surfaceRegistryId

**Dirty-rect save buffer** at `DAT_00498720`:
- Double-buffered (index `DAT_004951FC`), 0x410 bytes per bank
- Stores up to 64 rect entries (x, y, x2, y2) = 16 bytes each
- Sentinel: first entry with x == -1 marks end of list

### `ResetFrontendOverlayState` (0x4254D0)

Clears all dirty-rect and overlay state:
- Sets sentinel -1 in both dirty-rect banks and the sprite queue
- Zeros: overlay count (`DAT_00498718`), sprite count (`DAT_0049B680`),
  background-restore defer counter (`DAT_00498704`)

### `ResetFrontendSelectionState` (0x425500)

Clears all button/selection state:
- Iterates button table at `DAT_00499C78` (stride 0xD dwords), sets all active-flags to -1
- Zeros: preview mode flag, pending-selection, button index, last-highlight, tick counter,
  selection-ready, and button-confirm global

### `QueueFrontendOverlayRect` (0x425660)

Adds an overlay rectangle to the queue:
- Stores dest rect (x,y,x+w,y+h), source rect, color, surface pointer
- Calls `GetFrontendSurfaceRegistryId` to tag the surface
- Max 64 entries; overrun triggers `"FE: Out of Sprites"` message

### `QueueFrontendSpriteBlit` (0x425730)

Adds a clipped sprite blit to the queue:
- **Clips** against virtual screen bounds (`DAT_00495228` x `DAT_00495200`):
  adjusts source/dest rects for negative coords and overflow
- Converts color param from RGB888 to native 16-bit (for color key)
- Stores dual color key values (low/high = same value for exact match)
- Max 64 entries; overrun triggers `"FE: Out of Sprites"` warning

### `FlushFrontendSpriteBlits` (0x425540) — The Core Flush

Called once per frame to execute all queued operations:

1. **Restore phase**: If not deferred (`DAT_00498704 == 0`), iterates the previous frame's
   dirty-rect save buffer (selected by `DAT_004951FC`). For each saved rect, copies from the
   primary work surface to the back-buffer via `Copy16BitSurfaceRect`. This erases the previous
   frame's sprites.

2. **Swap dirty-rect bank**: Clears current bank with sentinel -1.

3. **Gallery update**: Calls `UpdateExtrasGalleryDisplay()` (renders gallery cross-fade if active).

4. **Highlight update**: Calls `RenderFrontendDisplayModeHighlight()` (draws button selection
   highlight).

5. **Blit phase**: Iterates all queued sprite entries:
   - For each sprite, looks up its surface in the **surface presentation table**
     (`DAT_00494FD4`, pairs of [surfacePtr, registryId])
   - Saves the destination rect into the dirty-rect bank for next-frame restore
   - Sets the source surface color key via `SetColorKey` (vtable +0x74, flag 8)
   - Blits via `Copy16BitSurfaceRect` with transparency flag 0x11

6. **Reset**: Clears sprite queue sentinel, overlay count, and sprite count.

### `DeferFrontendBackgroundRestore` (0x4258B0)

Sets `DAT_00498704 = 2`, causing `FlushFrontendSpriteBlits` to skip the restore phase for 2
frames. Used during screen transitions to prevent restoring stale background content.

### `ActivateFrontendCursorOverlay` (0x4258C0) / `DeactivateFrontendCursorOverlay` (0x4258E0)

Toggle `DAT_0049B68C` (cursor overlay flag):
- When active (=1): suppresses button highlight rendering
- Both clear `DAT_00498714` (mouse-movement flag)

---

## 6. Button/Menu System (0x4258F0 -- 0x426580)

### Button Table

The button descriptor table lives at `DAT_00499C78`, with **40 slots** (each 0x34 = 52 bytes,
13 dwords). End of table at `0x49A988`. The active-flag field is at offset +0x10 (dword index 4);
value -1 means inactive.

Fields per button entry (13 dwords):
| Offset | Field |
|--------|-------|
| +0x00 | destX |
| +0x04 | destY |
| +0x08 | destX + width |
| +0x0C | destY + height |
| +0x10 | srcX (active flag; -1 = inactive) |
| +0x14 | srcY |
| +0x18 | srcX + width |
| +0x1C | srcY + height |
| +0x20 | color/style |
| +0x24 | surface pointer |
| +0x28 | surface registry ID |
| +0x2C | flags (bit 0 = owns surface) |
| +0x30 | animation counter (0-6) |

### `CreateFrontendMenuRectEntry` (0x4258F0)

Finds the first inactive slot (active == -1) in the button table and populates it:
- Stores dest rect, source rect, color, surface pointer, registry ID
- Sets flags and animation counter to 0
- Returns 1 on success, 0 if table full

### `AdvanceFrontendTickAndCheckReady` (0x4259B0)

Simple debounce: increments `DAT_0049A978` and returns `true` when count > 2. Prevents
input from registering during the first few frames after a screen transition.

### `MoveFrontendSpriteRect` (0x4259D0)

Repositions button entry `param_1` to new (x, y) while preserving width/height:
- Computes w = x2-x1, h = y2-y1
- Sets new origin, recomputes x2/y2

### `DrawFrontendButtonBackground` (0x425B60)

Draws a 9-slice button frame into a surface using tiles from the button frame atlas
(`DAT_00496268`). Three style variants selected by `param_5`:
- 0 = Normal (dark fill)
- 1 = Highlighted (green-tinted fill, 0x1C8A/0x390A in 15/16-bit)
- 2 = Preview variant

The function tiles the center section in 4px-wide vertical strips from the atlas, with
left/right edge caps at the boundaries.

### `CreateFrontendDisplayModeButton` (0x425DE0)

Full button creation pipeline:
1. Allocates surface via `CreateTrackedFrontendSurface(width, height)` -- height doubled if
   not in preview mode (to hold normal + highlight frames stacked vertically)
2. COLORFILL the surface black (normal half) then green (highlight half, 0x1C8A/0x390A)
3. Draws button frame background for both halves via `DrawFrontendButtonBackground`
4. Measures and centers the label text via `MeasureOrCenterFrontendLocalizedString`
5. Draws localized text on both halves via `DrawFrontendLocalizedStringToSurface`
6. Preview mode: single-height surface with grey text overlay

### `CreateFrontendDisplayModePreviewButton` (0x4260E0)

Wrapper: sets `DAT_0049B694 = 1` (preview flag), calls `CreateFrontendDisplayModeButton`,
then clears the flag. Forces single-height preview styling.

### `RebuildFrontendButtonSurface` (0x426120)

Rebuilds an existing button in-place for the current pixel format:
- Reads width/height from existing button entry
- Re-fills with COLORFILL (black for normal, green for highlight)
- Redraws frame backgrounds

### `InitializeFrontendDisplayModeArrows` (0x426260)

Adds left/right navigation arrows to a button entry:
- Sources arrow sprites from `DAT_00496284` (arrow atlas)
- Draws at x = srcX + 7, vertically centered
- `param_2`: 0 = left arrows (src y=0), nonzero = right arrows (src y=0x12)
- Marks the button entry as arrow-capable in flags

### `ReleaseFrontendDisplayModeButtons` (0x426390)

Iterates all button entries, releases owned surfaces (flag bit 0 set) via
`ReleaseTrackedFrontendSurface`, marks all slots inactive, resets button index and selection.

### `BeginFrontendDisplayModePreviewLayout` (0x4264E0)

Saves the entire button table (40 entries) to a backup buffer at `DAT_0049A980`:
- Copies 0xD00 bytes (40 * 52)
- Saves current button index to `DAT_0049870C`
- Clears live table for preview-only buttons
- Sets preview flag `DAT_00498708 = 1`

### `RestoreFrontendDisplayModePreviewLayout` (0x426540)

Restores the saved button table from `DAT_0049A980`:
- Copies 0xD00 bytes back
- Restores button index
- Clears preview flag

### `UpdateFrontendDisplayModeSelection` (0x426580)

Per-frame button navigation and selection handler:

1. If cursor overlay active (`DAT_0049B68C != 0`): clear selection/activation, return
2. If mouse moved (`DAT_00498714`): set input-active flag, record timestamp
3. Iterate all active button entries:
   - Focused button: increment animation counter (max 6) -- used for highlight fade-in
   - Unfocused buttons: decrement animation counter (min 0) -- fade-out
4. Check input flags (`DAT_004951F8`):
   - Bit 18 (0x40000) = confirm: set `DAT_004951E8 = 1`, play SFX 3 (confirm sound)
   - Bits 0-1,9 (0x603) = navigation: clear input-active, record timestamp
   - Keyboard up/down: adjust `g_frontendButtonIndex`, play SFX 2 (navigate), wrap around

---

## 7. Surface Registry (0x411DE0 -- 0x411E00)

A flat lookup table at `DAT_00494FD0` tracking all created frontend surfaces. Used by the
sprite system to validate surface identity across frames.

Structure: Array of pairs `[surfacePtr, registryId]` (2 dwords each), extending from
`0x494FD0` to `0x4951D0` = **256 entries**.

### `ClearFrontendSurfaceRegistry` (0x411DE0)

Zeros all 256 entries (both pointer and ID fields).

### `GetFrontendSurfaceRegistryId` (0x411E00)

Linear search: given a surface pointer, returns its registry ID. Returns 0 if not found.

---

## 8. Fade/Dither System (0x411710 -- 0x411A50)

### `BuildFrontendDitherOffsetTable` (0x411710)

Builds a 32x32 (1024-entry) signed offset table at `DAT_00494BC0`:
- For each (row, col) in 0..31:
  - If row == col: value = col
  - If col > row: value = col - 1
  - If col < row: value = col + 1
- Used by `RenderFrontendFadeEffect` (0x411780) as a scanline-sweep dither pattern for the
  bar-sweep fade-to-black effect

### `InitFrontendFadeColor` (0x411750)

Sets the target fade color:
- `DAT_00494FC4` = `param_1 >> 3 & 0x1F1F1F` (extracts 5-bit R, G, B channels)
- `DAT_00494FC8` = 0 (fade progress reset)
- `DAT_00494FC0` = 1 (fade active)

### `ResetFrontendFadeState` (0x411A50)

Clears all fade state:
- Progress = 0, color = 0 (black), active = 1

---

## 9. Gallery Slideshow System (0x40D590 -- 0x40D830)

The Extras gallery displays 5 pre-rendered images with cross-fade transitions.

### Key Globals

| Address | Type | Description |
|---------|------|-------------|
| `0x0048F2D4` | IDirectDrawSurface*[5] | Gallery image surfaces (pic1-pic5.tga) |
| `0x0048F300` | int | Number of loaded gallery images (0 or 5) |
| `0x0048F304` | IDirectDrawSurface* | Currently displayed image |
| `0x0048F2FC` | int | Cross-fade blend weight (256 = full opacity, decays) |
| `0x0048F2F4` | int | Display X position (randomized) |
| `0x0048F2F8` | int | Display Y position (randomized) |
| `0x004951DC` | int | Gallery display mode: 0=slideshow, 1=released, 2=car-selection gallery |
| `0x00495234` | int | Hardware cross-fade capability flag |

### `LoadExtrasGalleryImageSurfaces` (0x40D590)

Loads 5 TGA images from `Front_End/Extras/Extras.zip`:
- pic1.tga through pic5.tga
- Only loads if `DAT_00495234` (cross-fade capability) is nonzero
- Initializes blend weight, position, and mode to 0

### `ReleaseExtrasGalleryImageSurfaces` (0x40D640)

Releases all loaded gallery surfaces:
- Iterates loaded count, calls `ReleaseTrackedFrontendSurface` for each
- If cross-fade not supported (`DAT_00495234 == 0`): just sets mode = 1, returns
- Sets `DAT_004951DC = 1` (released mode)

### `AdvanceExtrasGallerySlideshow` (0x40D750)

Selects a new random gallery image:
1. Loops `rand()` until a different image than current is selected
2. Locks the surface to validate it (reports `"Lock failed in SNK_StartRandomFa..."` on error)
3. Randomizes display position:
   - X: `rand() % (500 - imageWidth) + 140`
   - Y: `rand() % 336 + 84`
4. Sets blend weight to 256 (full opacity, will decay via `UpdateExtrasGalleryDisplay`)

### `UpdateExtrasGalleryDisplay` (0x40D830)

Called every frame from `FlushFrontendSpriteBlits`. Two display modes:

**Mode 2 (Car-selection gallery)**:
- Tied to car-selection screen; displays gallery image matching selected car index
- Fixed position at (0x76, 0x8C)
- Exponential-decay cross-fade: `weight = weight / 2`
- Blend parameters computed from weight:
  - weight > 0x60: src_alpha = min(0x80 - weight, 0x20), dst_alpha = 0x20 - src_alpha
  - weight < 0x20: src_alpha = weight*2 - 0x20 (clamped >= 0), dst_alpha = 0x20 - src_alpha
- Calls `CrossFade16BitSurfaces` if hardware capable, else `Copy16BitSurfaceRect`
- Weight decays toward terminal value, clamped to minimum -0x40

**Mode 0 (Slideshow)**:
- Same exponential decay with slightly different blend curve
- src_alpha max 0x18, dst_alpha max 0x14
- Falls back to straight copy if hardware unavailable

---

## 10. Architectural Summary

### Rendering Pipeline (per frame)

```
1. Screen function writes to Primary Work Surface (0x496260)
   - Fill rects, draw strings, blit TGA images

2. CopyPrimaryFrontendBufferToSecondary()  [snapshot for transitions]

3. QueueFrontendSpriteBlit() / QueueFrontendOverlayRect()  [cursor, buttons]

4. FlushFrontendSpriteBlits()
   a. Restore previous frame's dirty rects from save buffer
   b. Render gallery cross-fade (UpdateExtrasGalleryDisplay)
   c. Render button highlights (RenderFrontendDisplayModeHighlight)
   d. Execute all queued sprite blits with color-key transparency
   e. Save new dirty rects for next frame

5. Present to back-buffer:
   - PresentPrimaryFrontendBuffer() [hardware BltFast path]
   - PresentPrimaryFrontendBufferViaCopy() [software Lock/copy path]
```

### Dual-Surface Roles

- **Primary** (`0x496260`): Active composition target. All drawing goes here. Also serves as
  the sprite restore source (background behind sprites is read back from here).
- **Secondary** (`0x496264`): Snapshot buffer. Used to save the "clean" background state before
  animated elements are overlaid. Also used as source for cross-fade transitions between screens.

### Why Software Blit?

The `Copy16BitSurfaceRect` function's software path (Lock both surfaces, copy pixels manually)
exists because:
1. Some DirectDraw drivers don't support BltFast between off-screen surfaces
2. The color-key transparency path needs pixel-level control
3. DDrawCompat and other wrappers may have unreliable BltFast behavior

The `DAT_004951EC` flag selects hardware vs software at runtime based on driver capabilities
detected during initialization.
