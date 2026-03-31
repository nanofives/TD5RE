# Text Rendering Pipeline -- Complete Analysis

## Overview

TD5 has **four independent text rendering systems**, each with different font assets, rendering
backends, and scalability characteristics:

| System | Context | Font Source | Rendering Backend | Scalable? |
|--------|---------|-------------|-------------------|-----------|
| **Race HUD Text** | In-race overlays | `FONT` atlas in static.hed page 5 | D3D textured quads (pre-transformed) | No (bitmap) |
| **Pause Menu Text** | In-race pause overlay | `PAUSETXT` atlas in static.hed page 12 | D3D textured quads (pre-transformed) | No (bitmap) |
| **Frontend Large Font** | Menu titles, button labels | `MenuFont.tga` (36x36 glyphs) | DirectDraw BltFast to offscreen surface | No (bitmap) |
| **Frontend Small Font** | Body text, options, labels | `BodyText.tga` / `SmallText.tga` / `SmallText2.tga` | DirectDraw BltFast to offscreen surface | No (bitmap) |

Additionally, the **non-English localization path** (SNK_LangDLL[8] != '0') replaces several
dynamically-rendered text elements with **pre-rendered TGA images** from FrontEnd.zip, making
the non-English path even harder to scale.

---

## System 1: Race HUD Text

### Architecture

The race HUD text system renders text as D3D pre-transformed textured quads, composited into
the translucent primitive pipeline after 3D rendering. It operates at the game's runtime
resolution and benefits from the existing widescreen scale factors.

### Font Atlas: `FONT` (static.hed entry #31)

- **Location:** Texture page 5 (256x256 ARGB32 with alpha)
- **Atlas region:** X=96, Y=192, W=160, H=64
- **Grid:** 4 rows x 16 columns = 64 glyph slots
- **Glyph cell:** 10px column spacing, 16px row spacing
- **Default glyph render size:** 8px wide x 12px tall
- **Width overrides:** Glyphs 0x22, 0xB6, 0xE6 = 4px; glyph 0x26 = 7px

### Character Mapping (0x4669F4, 128 bytes)

```
0x00-0x1F -> 0x1F (control chars -> space)
0x20      -> 0x1F (space)
0x21-0x39 -> 0x20-0x39 (!"#$%&'()*+,-./0123456789)
0x40      -> 0x1F (@ -> space)
0x41-0x5A -> 0x00-0x19 (A-Z -> glyph indices 0-25)
0x5B-0x60 -> 0x1F ([\]^_` -> space)
0x61-0x7A -> 0x00-0x19 (a-z -> SAME indices as A-Z, case-insensitive)
0x7B-0x7F -> 0x1F
```

The font is **UPPERCASE ONLY** -- lowercase maps to the same glyphs as uppercase.

### Glyph Atlas Grid Layout

The 64 glyph slots in the 4x16 grid map as follows:
- Row 0 (indices 0x00-0x0F): A B C D E F G H I J K L M N O P
- Row 1 (indices 0x10-0x1F): Q R S T U V W X Y Z [space] [space] ... [space]
- Row 2 (indices 0x20-0x2F): [space] ! " # $ % & ' ( ) * + , - . /
- Row 3 (indices 0x30-0x3F): 0 1 2 3 4 5 6 7 8 9 [unused slots]

### Key Functions

#### InitializeRaceHudFontGlyphAtlas (0x428240)
```
- Allocates 0xB800 bytes for text quad buffer (256 quads max, each 0xB8 bytes)
- Allocates 0x404 bytes for glyph UV/size table (64 glyphs x 16 bytes + 4 bytes texture ptr)
- Looks up "font" entry via FindArchiveEntryByName
- Stores texture slot pointer at glyph_table[0x100]
- Seeds 4x16 grid: each glyph record = {atlas_u, atlas_v, width, height} as 4 floats
  - atlas_u = archive_entry.pos_x + 1.5 + (column * 10.0)
  - atlas_v = archive_entry.pos_y + 2.5 + (row * 16.0)
  - width = 8.0 (default), height = 12.0 (default)
- Applies width overrides for specific narrow glyphs
- Resets queued glyph count to 0
```

#### QueueRaceHudFormattedText (0x428320)
```
void QueueRaceHudFormattedText(int fontIndex, int x, int y, int centered, LPCSTR fmt, ...)
```
- Formats string via `wvsprintfA` (printf-style, up to 256 chars)
- Remaps each char through the 128-byte table at 0x4669F4
- If `centered != 0`, offsets X by `-totalWidth / 2`
- For each glyph:
  - Reads {atlas_u, atlas_v, width, height} from glyph table
  - Sets screen quad corners: {x, y} to {x+width, y+height}
  - Sets UV corners: {atlas_u, atlas_v} to {atlas_u+width, atlas_v+height}
  - Calls `BuildSpriteQuadTemplate` to fill a 0xB8-byte quad struct
  - Advances X cursor by `width + 1.0`
- Enforces 256 total glyph limit per frame (DAT_004a2cc0)

#### FlushQueuedRaceHudText (0x428570)
- Iterates all queued glyph quads (0xB8 bytes each, stride 0x5C words)
- Calls `SubmitImmediateTranslucentPrimitive` for each
- Resets queue counter to 0

### Callers
- `DrawRaceStatusText` (0x439B70): position labels, timers, wanted messages, replay banner
- `RenderRaceHudOverlays` (0x4388A0): race position ("1ST"-"6TH"), time/lap text
- Various format strings like `"%02d:%02d.%02d"`, `"%s %d/%d"`, `"%d %s"`

### Key Data
- Glyph UV/size table: `DAT_004a2cb8` (heap-allocated 0x404 bytes)
- Text quad buffer: `DAT_004a2cbc` (heap-allocated 0xB800 bytes)
- Queued glyph count: `DAT_004a2cc0`

---

## System 2: Pause Menu Text

### Architecture

The pause menu overlays D3D textured quads on top of the race scene, similar to the HUD system
but using the `PAUSETXT` atlas. It renders at the center of the screen with fixed-size layout.

### Font Atlas: `PAUSETXT` (static.hed entry #42)

- **Location:** Texture page 12 (256x256 ARGB32 with alpha)
- **Atlas region:** X=0, Y=32, W=256, H=224
- **Character layout:** 16 columns x 14 rows glyph grid (16x16 per cell)
- **Width per glyph:** Per-character variable widths from table at 0x4660C8+char
  - Scaled: `(charWidth * 2) / 3` for rendered width
- **UV mapping:** `u = (char & 0x0F) * 16 + 0.5`, `v = (char >> 4) * 16 + 0.5`

### Key Function: InitializePauseMenuOverlayLayout (0x43B7C0)

- Takes a `param_1` index (0-7) selecting a pause menu page preset
- Builds quad primitives for BLACKBOX (background panel), SELBOX (selection highlight),
  SLIDER (volume/option bars), BLACKBAR (option row backgrounds)
- For each text line in the selected page's string table:
  - Iterates characters, computes glyph UV from the 16x14 atlas
  - Glyph width = `(PTR_DAT_004660c8[char] * 2) / 3`
  - Centered text: computes total width first, starts at `-totalWidth/2`
  - Left-aligned text: starts at `4.0 - halfPanelWidth`
  - Builds one sprite quad per glyph via `BuildSpriteQuadTemplate`

### Page Presets (0x4744B8 string table)
The pause menu has 8 page configurations with predefined title/option strings
(e.g., "PAUSED", "SOUND OPTIONS", etc.).

---

## System 3: Frontend Large Font (Menu Titles)

### Architecture

The frontend text system renders to **DirectDraw offscreen surfaces** using `BltFast` (vtable
offset +0x1C). All frontend rendering happens at the fixed virtual resolution of 640x480 in
16-bit color (RGB555 or RGB565). The composited surfaces are then Blt-stretched to the display
in the widescreen scale cave.

### Font Asset: `MenuFont.tga`

- **File:** `Front End\MenuFont.tga` (291,408 bytes on disc)
- **Also in FrontEnd.zip** as `mainfont.tga` (same 291,408 bytes)
- **Loaded to:** `DAT_00496278` (IDirectDrawSurface pointer)
- **Grid layout:** 7 columns, rows of 36px (0x24) height
- **Glyph cell:** Variable width per character, 36px tall (0x24)
- **Character mapping:** `(char - 0x20) % 7` = column, `(char - 0x20) / 7` = row
- **Width table:** `DAT_004664F8[char]` -- per-character advance widths (signed byte)

### Key Function: MeasureOrDrawFrontendFontString (0x412D50)

```
int MeasureOrDrawFrontendFontString(byte* str, uint x, uint y, int* destSurface)
```
- If `destSurface == NULL`: measure-only mode (returns pixel width)
- If `destSurface != NULL`: renders glyphs via BltFast to the surface
- For each character:
  - Source rect: column = `(c-0x20) % 7 * 0x24`, row = `(c-0x20) / 7 * 0x24`
  - Width from `DAT_004664F8[char]` (most are 0x1F = 31px for uppercase)
  - BltFast with flag 0x11 (transparent, respects destX/destY)
  - Advance X by `width - 1`

### Usage
- `CreateMenuStringLabelSurface` (0x412E36): Creates a standalone surface with a menu title
  string rendered onto it. English path renders dynamically; non-English path loads a
  pre-rendered TGA (e.g., `SelectCompCarText.tga` from FrontEnd.zip).

---

## System 4: Frontend Small Font (Body Text, Buttons, Options)

### Architecture

Same DirectDraw BltFast approach as System 3, but uses smaller font atlases. There are TWO
small font atlases used for different purposes.

### Font Assets

| Global | TGA File | Role | Grid |
|--------|----------|------|------|
| `DAT_0049626c` | `BodyText.tga` (English) or `SmallText2.tga` (non-English) | Primary body text | See below |
| `DAT_00496270` / `DAT_0049627c` | `SmallText.tga` | Secondary / clickable text | 21x cols, 12px cells |
| `DAT_00496274` | `SmallTextb.tga` | Alternative small text | Same layout |

#### Primary Font (BodyText.tga / SmallText2.tga)
- **English path:** `BodyText.tga` loaded to `DAT_0049626c`
  - Grid: 21 columns x N rows, 12px (0x0C) cell size
  - Character mapping: `(c-0x20) % 21` = column, `(c-0x20) / 21` = row
  - Width table: `DAT_004662D0[char]` (signed byte, per-character)
  - Used by: `DrawFrontendFontStringPrimary`, `DrawFrontendFontStringSecondary`

- **Non-English path (language index != '0'):** Uses a 24x24 glyph grid with width table at
  `PTR_DAT_004660C8[char]`. Character mapping: `(c-0x20) % 10` = column * 0x18,
  `(c-0x20) / 10` = row * 0x18.

#### Secondary Font (SmallText.tga)
- Grid: 21 columns, 12px (0x0C) cell size
- Width table: `DAT_004662D0[char]` (same as primary)
- Y-offset table: `DAT_004663E4[char]` (per-char vertical offset for baseline alignment)
- Used by: `DrawFrontendSmallFontStringToSurface`, `DrawFrontendClippedStringToSurface`,
  `DrawFrontendWrappedStringLine`

### Character Width Tables (all at fixed addresses in .data section)

| Address | Size | Purpose |
|---------|------|---------|
| `0x4660C8` + char offset | 128 bytes | Non-English large glyph widths (24px cells) |
| `0x4662D0` | 128 bytes | English small font advance widths |
| `0x4662F0` | 128 bytes | English small font display widths (for extent measurement) |
| `0x4663E4` | 128 bytes | English small font Y-offset per character |
| `0x4664F8` | 128 bytes | MenuFont (large) advance widths |
| `0x466518` | 128 bytes | MenuFont (large) display widths |

### Key Rendering Functions

| Function | Address | Font | Target Surface |
|----------|---------|------|---------------|
| `DrawFrontendFontStringPrimary` | 0x424180 | `DAT_0049626c` | `DAT_00496260` (primary) |
| `DrawFrontendFontStringSecondary` | 0x424240 | `DAT_0049626c` | `DAT_00496264` (secondary) |
| `DrawFrontendFontStringToSurface` | (inlined) | `DAT_0049626c` | arbitrary surface |
| `DrawFrontendSmallFontStringToSurface` | 0x4246B0 | `DAT_0049627c` | arbitrary surface |
| `DrawFrontendClippedStringToSurface` | 0x424770 | `DAT_0049627c` | arbitrary surface |
| `DrawFrontendWrappedStringLine` | 0x4248E0 | `DAT_0049627c` | arbitrary surface |
| `MeasureOrDrawFrontendFontString` | 0x412D50 | `DAT_00496278` | arbitrary / measure |

### Localized Wrapper Functions

These four functions branch on `SNK_LangDLL[8] == '0'`:

| Wrapper | Address | English path | Non-English path |
|---------|---------|-------------|-----------------|
| `DrawFrontendLocalizedStringPrimary` | 0x4242B3 | `DrawFrontendFontStringPrimary` | 24x24 glyph BltFast |
| `DrawFrontendLocalizedStringSecondary` | 0x424393 | `DrawFrontendFontStringSecondary` | 24x24 glyph BltFast |
| `DrawFrontendLocalizedStringToSurface` | 0x424563 | `DrawFrontendFontStringToSurface` | 24x24 glyph BltFast |
| `MeasureOrCenterFrontendLocalizedString` | 0x424A50 | Width via `DAT_004662D0` table | Width via `PTR_DAT_004660C8` |

---

## System 5: Pre-Rendered Text TGA Images (Non-English Localization)

For non-English languages, several UI elements use **pre-rendered TGA textures** with baked-in
text from `FrontEnd.zip`:

| TGA in FrontEnd.zip | Size | Purpose |
|---------------------|------|---------|
| `MainMenuText.TGA` | 6,064 B | Main menu title text |
| `RaceMenuText.TGA` | 6,304 B | Race menu title text |
| `QuickRaceText.tga` | 6,344 B | Quick race title |
| `OptionsText.tga` | 5,104 B | Options title |
| `SelectCarText.tga` | 6,532 B | Car selection title |
| `SelectCompCarText.tga` | 11,612 B | Computer car selection title |
| `TrackSelectText.TGA` | 8,064 B | Track selection title |
| `HighScoresText.TGA` | 7,264 B | High scores title |
| `ResultsText.tga` | 7,784 B | Results title |
| `NetPlayText.TGA` | 5,344 B | Network play title |
| `Player1Text.TGA` | 5,052 B | Player 1 label |
| `Player2Text.TGA` | 5,052 B | Player 2 label |
| `AreYouSureText.TGA` | 8,504 B | Confirmation dialog text |
| `NoControllerText.TGA` | 8,624 B | "No controller" warning |

The `CreateMenuStringLabelSurface` function (0x412E36) selects between dynamic rendering
(English, via `MeasureOrDrawFrontendFontString`) and pre-rendered TGA loading (non-English,
via `LoadFrontendTgaSurfaceFromArchive`) based on `SNK_LangDLL[8]`.

---

## Background TGA Images With Baked-In Text

Several full-screen 640x480 background TGA images loaded from `FrontEnd.zip` have
**text baked into the artwork**:

| TGA File | Size | Baked Text Content |
|----------|------|--------------------|
| `MainMenu.tga` | 308,304 B | "TEST DRIVE 5" logo, decorative text |
| `RaceMenu.tga` | 308,304 B | Race menu background with decorative text |
| `NetMenu.tga` | 308,304 B | Network menu background |
| `TrackSelect.tga` | 308,304 B | Track selection background |
| `LanguageScreen.TGA` | 308,304 B | Language selection screen (ALL text baked) |
| `LegalScreen.TGA` | 308,304 B | Legal/copyright screen (ALL text baked) |
| `Logo.tga` | 307,986 B | Publisher/developer logos |
| `Language.tga` | 91,216 B | Language flags/labels |

These 308 KB TGAs are 640x480x16bpp full-screen images. Their text is part of the image
data and cannot be replaced without re-rendering the images.

---

## Race HUD Sprite-Based Elements (NOT Text)

These use dedicated sprite atlases from static.hed, not the text rendering pipeline:

| Element | Atlas Entry | Format | Text Content |
|---------|------------|--------|-------------|
| Speed digits | `SPEEDOFONT` (page 5) | 160x32, 10 digit glyphs | Digits 0-9 only |
| Gear indicator | `GEARNUMBERS` (page 5) | 128x16, number glyphs | Gear numbers |
| Timer digits | `numbers` (page 5) | 80x48, 5x2 grid | Digits 0-9 only |
| Position labels | `POSITION` (page 5) | 192x16 | "1ST" through "6TH" (baked) |
| MPH/KPH label | `MPH` (page 5) | 16x8 | "MPH" or "KM/H" (baked) |
| Replay banner | `REPLAY` (page 5) | 32x32 | "REPLAY" (baked) |
| U-turn warning | `UTURN` (page 5) | 64x64 | Arrow icon (no text) |

---

## Frontend Surface Architecture

The frontend uses two 640x480 16-bit DirectDraw offscreen surfaces as double buffers:

| Global | Role |
|--------|------|
| `DAT_00496260` | Primary work surface (current frame composition) |
| `DAT_00496264` | Secondary work surface (for effects like fade) |

**Frontend frame composition flow:**
1. Background TGA loaded to primary surface via `LoadTgaToFrontendSurface16bpp`
2. Text/button surfaces BltFast'd onto primary surface
3. `FlushFrontendSpriteBlits` copies sprite regions from source surfaces to primary
4. `DXDraw::Flip` presents the result (or software Blt for non-flip modes)

**Button creation flow (`CreateFrontendDisplayModeButton`, 0x425DE0):**
1. `CreateTrackedFrontendSurface(width, height)` -- allocates DDraw surface for button
2. `DrawFrontendButtonBackground` -- renders 9-slice button frame from `ButtonBits.tga`
3. `MeasureOrCenterFrontendLocalizedString` -- computes centered X position
4. `DrawFrontendLocalizedStringToSurface` -- renders text onto button surface
5. Button descriptor registered in table at `0x499C78` (13 dwords per entry, max ~40)
6. Each frame, `RenderFrontendUiRects` BltFast's all active button surfaces to primary

---

## What Would Need to Change for Scalable Text

### Option A: Replace Bitmap Font Atlases With GDI-Rendered Surfaces

**Concept:** At atlas initialization time, render a complete glyph atlas to a DirectDraw
surface using Win32 `CreateFont` + `TextOut` / `DrawText`, then use it exactly as the
existing bitmap atlas is used.

**Race HUD Text (System 1) -- Medium difficulty:**

1. **Hook `InitializeRaceHudFontGlyphAtlas` (0x428240):**
   - Instead of looking up `"font"` in static.hed, create a new texture surface
   - Use GDI to render all 64 characters into a 4x16 grid at desired resolution
   - Populate the glyph UV/size table with the actual rendered glyph metrics
   - Register the surface as a D3D texture via M2DX texture pipeline

2. **Challenges:**
   - The atlas is stored as a D3D texture page (in the M2DX texture system), not a DDraw surface
   - Would need to either: (a) render to a DDraw surface and upload via the existing texture
     pipeline, or (b) create a standalone D3D texture
   - Glyph UV coordinates are in texture-space (atlas pixel coordinates), so the atlas
     dimensions must be known and consistent
   - Max 256 glyph quads per frame -- sufficient for current usage

3. **Changes needed:**
   - Replace `InitializeRaceHudFontGlyphAtlas` body (~40 bytes of code cave needed)
   - Update glyph width table entries to match new font metrics
   - No changes needed to `QueueRaceHudFormattedText` or `FlushQueuedRaceHudText`

**Pause Menu Text (System 2) -- Medium difficulty:**

1. **Hook `InitializePauseMenuOverlayLayout` (0x43B7C0):**
   - Replace `PAUSETXT` atlas lookup with a GDI-rendered texture
   - Update per-character width computation
   - The pause menu quad builder already handles variable widths

2. **Challenge:** The pause menu atlas is on texture page 12 (shared with SLIDER, BLACKBOX,
   SELBOX, BLACKBAR). Replacing just the text portion requires careful UV coordination.

**Frontend Text (Systems 3+4) -- Easiest:**

1. **Hook font surface loading in `InitializeFrontendResourcesAndState` (0x414790):**
   - Instead of loading `BodyText.tga` / `SmallText.tga` / `MenuFont.tga` from
     `FrontEnd.zip`, render new font atlases to DDraw surfaces using GDI
   - The frontend already uses DDraw surfaces with BltFast, so no API change needed

2. **Per-surface replacement:**
   - `DAT_0049626c` (BodyText.tga): 12px grid, 21 columns -- re-render with GDI font
   - `DAT_00496278` (MenuFont.tga): 36px grid, 7 columns -- re-render with GDI font
   - `DAT_00496270` (SmallText.tga): 12px grid, 21 columns -- re-render with GDI font

3. **Update width tables:**
   - `DAT_004662D0` (small font widths): Must match new glyph metrics
   - `DAT_004664F8` (large font widths): Must match new glyph metrics
   - These are in `.data` section and can be patched directly

4. **Advantage:** Frontend font surfaces are standard DDraw surfaces. GDI can render directly
   to a DDraw surface via `GetDC`/`ReleaseDC`.

### Option B: Per-String Dynamic Rendering (Bypass Atlas Entirely)

**Concept:** Instead of building glyph quads from an atlas, render each text string directly
to a temporary DDraw surface using GDI, then use that surface as the texture source.

**Race HUD Text -- High difficulty:**
- Would require replacing `QueueRaceHudFormattedText` entirely
- Each text string would need a temporary D3D texture created per-frame (expensive)
- Not recommended due to per-frame texture creation overhead

**Frontend Text -- Medium difficulty:**
- Frontend already renders to DDraw surfaces; could replace BltFast glyph-by-glyph rendering
  with a single `DrawText` call per string
- Would bypass the atlas entirely
- More flexible but requires rewriting all 7+ text rendering functions

### Option C: High-Resolution Replacement Assets

**Concept:** Keep the existing code unchanged but replace the font TGA files with
higher-resolution versions that are downscaled to atlas dimensions.

**Limitations:**
- Atlas dimensions are fixed by static.hed entries (e.g., FONT = 160x64 on a 256x256 page)
- Cannot increase glyph resolution beyond the fixed atlas cell sizes
- Would require changing static.hed entries AND texture page dimensions
- **Not viable for meaningful quality improvement**

### Recommended Approach

**Phase 1 (Frontend text -- Easiest wins):**
1. Hook `InitializeFrontendResourcesAndState` to intercept font surface loading
2. Create DDraw surfaces at the same dimensions as existing TGAs
3. Use `GetDC` + `CreateFont` + `TextOut` to render glyph atlases with a chosen TTF font
4. Update the 6 character width tables in .data to match new metrics
5. The rest of the frontend rendering pipeline works unchanged

**Phase 2 (Race HUD text):**
1. Hook `InitializeRaceHudFontGlyphAtlas`
2. Create a D3D texture with the glyph atlas rendered via GDI
3. Upload via M2DX texture pipeline (LoadRGBS32 path for ARGB support)
4. Update glyph UV/size table to match new metrics
5. `QueueRaceHudFormattedText` and `FlushQueuedRaceHudText` work unchanged

**Phase 3 (Baked text in background TGAs):**
1. Re-render MainMenu.tga, TrackSelect.tga, etc. without text overlays
2. Render text dynamically using the existing button/label system
3. This is the hardest part -- requires artwork modification AND new text placement logic

**Phase 4 (HUD sprite digits -- SPEEDOFONT, numbers, GEARNUMBERS, POSITION, MPH):**
1. These are small fixed-purpose atlases on texture page 5
2. Could be replaced with GDI-rendered versions at init time
3. Requires careful UV matching since they share the page with other sprites

---

## Key Global Variables Summary

### Race HUD Text
| Address | Type | Description |
|---------|------|-------------|
| `0x4A2CB8` | float* | Glyph UV/size table (64 entries x 4 floats + 1 texture ptr) |
| `0x4A2CBC` | void* | Text quad buffer (0xB800 bytes, 256 quads max) |
| `0x4A2CC0` | int | Queued glyph count (reset each frame) |
| `0x4669F4` | byte[128] | ASCII-to-glyph-index remap table |

### Frontend Surfaces
| Address | Type | Description |
|---------|------|-------------|
| `0x496260` | IDirectDrawSurface* | Primary work surface (640x480x16) |
| `0x496264` | IDirectDrawSurface* | Secondary work surface (640x480x16) |
| `0x49626C` | IDirectDrawSurface* | Body/primary font atlas surface |
| `0x496270` | IDirectDrawSurface* | SmallText font atlas surface |
| `0x496274` | IDirectDrawSurface* | SmallTextb font atlas surface |
| `0x496278` | IDirectDrawSurface* | MenuFont (large) atlas surface |
| `0x49627C` | IDirectDrawSurface* | Active secondary font (= SmallText) |
| `0x496268` | IDirectDrawSurface* | ButtonBits.tga (button frame pieces) |
| `0x496284` | IDirectDrawSurface* | ArrowButtonz.tga |
| `0x496288` | IDirectDrawSurface* | ArrowExtras.tga (control icons) |
| `0x496280` | IDirectDrawSurface* | snkmouse.tga (cursor) |

### Character Width Tables
| Address | Size | Font System |
|---------|------|-------------|
| `0x4660C8+char` | 128 B | Non-English large glyph widths |
| `0x4662D0` | 128 B | English small font advance widths |
| `0x4662F0` | 128 B | English small font display widths |
| `0x4663E4` | 128 B | English small font Y-offsets |
| `0x4664F8` | 128 B | MenuFont (large) advance widths |
| `0x466518` | 128 B | MenuFont (large) display widths |

### Frontend Virtual Resolution
| Address | Value | Description |
|---------|-------|-------------|
| `0x495228` | 0x280 (640) | Frontend virtual width (hardcoded) |
| `0x495200` | 0x1E0 (480) | Frontend virtual height (hardcoded) |
| `0x49525C` | 15 or 16 | Frontend pixel format (RGB555 or RGB565) |

---

## FrontEnd.zip Complete Font/Text Asset Inventory

| File | Dimensions | bpp | Grid | Purpose |
|------|-----------|-----|------|---------|
| `mainfont.tga` | 640x454 | 16 | 7 cols, 36px rows | Large menu title font (=MenuFont.tga) |
| `smalfont.TGA` | 504x352 | 16 | ? | Alternative small font (unused?) |
| `BodyText.tga` | 252x530 | 16 | 21 cols, 12px rows | English body text primary |
| `OldBodyText.tga` | 252x530 | 16 | 21 cols, 12px rows | Previous version of BodyText |
| `smalltext.tga` | 252x136 | 16 | 21 cols, 12px rows | Small clickable text |
| `smalltextb.tga` | 252x136 | 16 | 21 cols, 12px rows | Small text variant B |
| `SmallText2.tga` | (varies) | 16 | 10 cols, 24px rows | Non-English body text |
| `InfoText.tga` | small | 16 | ? | Car info display text |
| `ButtonBits.tga` | 90x74 | 16 | 9-slice | Button frame artwork |
| `ButtonLights.tga` | small | 16 | ? | Button highlight effects |

---

## Architectural Insights

1. **The English path is already "dynamic"** -- all button labels and body text are rendered
   character-by-character from font atlases using SNK_ string data from Language.dll. Only the
   non-English path uses pre-rendered TGA text images.

2. **The atlas approach is efficient** -- rather than creating textures per-string, the game
   pre-loads font atlases and BltFast's individual glyph rectangles. This is the right pattern
   to preserve when replacing fonts.

3. **Resolution independence already exists for race HUD** -- the HUD text system uses D3D
   pre-transformed vertices with scale factors from `gRenderWidth/640` and `gRenderHeight/480`.
   Increasing glyph resolution in the atlas would directly improve visual quality.

4. **Frontend is fixed at 640x480** -- all frontend rendering happens at 640x480 regardless
   of display resolution. The widescreen patch stretches this via Blt. Higher-resolution
   frontend text would require either: (a) increasing the frontend virtual resolution, or
   (b) rendering text at display resolution in a post-processing pass.

5. **The font is uppercase-only** in the race HUD (64 glyphs, A-Z maps same as a-z). The
   frontend fonts support full ASCII printable range (0x20-0x7E = 95 characters).

6. **No anti-aliasing** -- all existing font rendering uses 1-bit alpha (transparent or opaque
   color key). GDI `ClearType` or manual alpha blending would significantly improve quality
   but requires ARGB surfaces (already used for race HUD textures on page 5, but frontend
   uses 16-bit RGB555/565 without alpha channel -- would need format upgrade).
