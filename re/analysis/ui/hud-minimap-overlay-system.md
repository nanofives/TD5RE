# HUD & Minimap Overlay System

## Overview

The race HUD is a fully dynamic 2D overlay system rendered after the 3D scene via the translucent
primitive pipeline. All positions scale from `gRenderWidth / 640` and `gRenderHeight / 480` base
factors, making the system widescreen-correct with edge-relative positioning.

**Render order in RunRaceFrame (0x42b580):**
1. BeginRaceScene
2. Sky -> Track spans -> Actors -> Tire tracks -> Particles
3. `FlushQueuedTranslucentPrimitives` (0x431340) -- 3D translucents
4. `FlushProjectedPrimitiveBuckets` (0x43e2f0) -- 4096-bucket depth sort
5. `DrawRaceStatusText` (0x439b70) -- per-view text (position, timer, wanted messages)
6. *[repeat for view 2 in split-screen]*
7. `RenderRaceHudOverlays` (0x4388a0) -- all sprite-based HUD elements + minimap
8. EndRaceScene

---

## Initialization Chain

### InitializeRaceOverlayResources (0x437800)
- Allocates 0x1148 bytes per view for HUD primitive storage (`DAT_004b0c00`)
- Sets HUD visibility bitmask based on race mode (`DAT_004aaf74`):

| Bit | Hex | Element | Condition |
|-----|-----|---------|-----------|
| 0 | 0x001 | Race position label ("1ST".."6TH") | Most race modes |
| 1 | 0x002 | Lap timers (per-racer splits) | Most race modes |
| 2 | 0x004 | Speedometer (analog needle + digits) | Always in race |
| 3 | 0x008 | -- reserved (always set) | -- |
| 4 | 0x010 | U-turn warning icon | Always in race |
| 5 | 0x020 | -- reserved (always set) | -- |
| 6 | 0x040 | Metric digit readout (speed/FPS/etc) | Always in race |
| 7 | 0x080 | Total timer "%s %d" | Checkpoint/time-attack modes |
| 8 | 0x100 | Lap/checkpoint counter "%s %d/%d" | Checkpoint/time-attack modes |
| 9 | 0x200 | Circuit lap count on circuits | Circuit tracks only |
| 31 | 0x80000000 | "REPLAY" banner | Replay mode |

- Loads `FADEWHT` sprite for screen-fade overlays (5 quads)
- In split-screen: loads `COLOURS` sprite for divider bars

### InitializeRaceHudLayout (0x437BA0)
- Computes two scale factors:
  - `scale_x` (DAT_004b1138) = `gRenderWidth * (1.0/640.0)` = `gRenderWidth * 0.0015625`
  - `scale_y` (DAT_004b113c) = `gRenderHeight * (1.0/480.0)` = `gRenderHeight * 0.0020833334`
- Split-screen modes (param_1 == 1 or 2) halve both scale factors
- Computes viewport bounds (left/right/top/bottom) for each view
- Loads these sprite atlas entries from the track ZIP:
  - `SPEEDO` -- speedometer dial background (96x96 scaled)
  - `SPEEDOFONT` -- speed digit font (15-wide glyphs, 23-tall, up to 3 digits)
  - `GEARNUMBERS` -- gear indicator (16x16 per glyph)
  - `numbers` -- generic number atlas (5x2 grid, 16x24 per digit)
  - `SEMICOL` -- colon/separator glyph for timers
  - `UTURN` -- wrong-way warning icon (64x64 scaled, centered on screen)
  - `REPLAY` -- replay banner (60x60 scaled)
- Positions are computed as offsets from viewport edges * scale factors
- Calls `InitializeMinimapLayout` at the end

---

## Speedometer System

### Layout (in InitializeRaceHudLayout)
- **Position:** Bottom-right corner, offset from viewport edge
  - X: `viewport_right - scale_x * 96 - scale_x * 16`
  - Y: `viewport_bottom - scale_y * 96 - scale_y * 8`
- **Dial:** 96*scale_x by 96*scale_y sprite quad (`SPEEDO` atlas)
- **Needle:** Rendered as a rotated quad in `RenderRaceHudOverlays`
  - Angle computed from vehicle speed / max RPM: `angle = (speed * 0xA5A) / maxRPM + 0x400`
  - Uses `CosFloat12bit` / `SinFloat12bit` (12-bit fixed-point angle, 4096 = full circle)
  - Needle drawn as 4-vertex quad: tip at 45 units from center, base at 9 units back
- **Digital readout:** 1-3 digits from `SPEEDOFONT` atlas
  - Font: 16px wide, 24px tall per glyph (in atlas coordinates)
  - Speed value: `vehicle_speed >> 8`, then unit-converted:
    - MPH mode: `(speed * 256 + 625) / 1252`
    - KPH mode: `(speed * 256 + 389) / 778`
  - Digits rendered right-to-left (ones, tens, hundreds)

### Gear Indicator
- **Glyph source:** `GEARNUMBERS` atlas
- **Position:** Below speedometer dial, offset left
- **Size:** 32*scale_x by 16*scale_y
- Gear value read from actor state byte at offset +0x36B

---

## Metric Digit Display (BuildRaceHudMetricDigits, 0x4397B0)

Cyclable 3-4 digit readout in the top-center area. Mode controlled by `gHudMetricMode`:

| Mode | Value Source | Digits | Description |
|------|-------------|--------|-------------|
| 0 | `pending_finish_timer >> 8` | 3 | Race countdown/finish timer |
| 1 | `ROUND(g_instantFPS)` | 3 | Frame rate display |
| 2 | `actor.gap_0000._128_2_ % 10000` | 4 | Distance/odometer |
| 3 | `actor.gap_0346[0x2a]` | 3 | Unknown byte metric |

- Uses `numbers` atlas (5 columns x 2 rows, 16x24 per digit cell)
- Each digit UV: `column = value % 5`, `row = value / 5`

---

## Text Rendering Pipeline

### InitializeRaceHudFontGlyphAtlas (0x428240)
- Allocates 0xB800 bytes for text quad buffer (256 quads max)
- Allocates 0x404 bytes for glyph UV/size table (64 glyphs + 1 texture pointer)
- Loads `font` texture from archive
- Builds 4x16 glyph grid: 10px column spacing, 16px row spacing
- Each glyph record: 4 floats = {atlas_u, atlas_v, width, height}
- Default glyph size: 8x12 pixels
- Special overrides: glyphs 0x22, 0xB6, 0xE6 = 4px wide; glyph 0x26 = 7px wide

### Character Mapping Table (0x4669F4)
- 128-byte ASCII remap table:
  - 0x00-0x1F: all map to 0x1F (space/undefined)
  - 0x20-0x39: ASCII printable mapped to glyph indices 0x20-0x39
  - 0x40-0x5A: uppercase A-Z mapped to 0x00-0x19
  - 0x60-0x7A: lowercase a-z mapped to 0x00-0x19 (same as uppercase -- case-insensitive)

### QueueRaceHudFormattedText (0x428320)
- `void QueueRaceHudFormattedText(int fontIndex, int x, int y, int centered, LPCSTR fmt, ...)`
- Uses `wvsprintfA` for printf-style formatting
- Remaps each character through the glyph table
- For each glyph: builds a `BuildSpriteQuadTemplate` quad with atlas UV coordinates
- If `centered` != 0: offsets x by `-totalWidth / 2`
- Advances x cursor by `glyphWidth + 1.0` per character
- Queues up to 256 glyphs total per frame

### FlushQueuedRaceHudText (0x428570)
- Iterates all queued glyph quads
- Calls `SubmitImmediateTranslucentPrimitive` for each
- Resets queue counter to 0

---

## Position / Race Order Display

The race position display uses a string pointer table at `PTR_DAT_00473e38`:
- Index 0: "6TH", 1: "5TH", 2: "4TH", 3: "3RD", 4: "2ND", 5: "1ST"
- Index 6: "DEMO", 7: "LAP", 8: "COMPUTER", 9: "PLAYER", 10: "DEMO MODE"
- Index 11: "TIME", 12: "LAPS", 13: "ARRESTS", 14: "POINTS"

A second string table (offset +0x0D entries) adds variant labels for cop-chase mode.

**Position rendering** (in RenderRaceHudOverlays, flag bit 0):
```
QueueRaceHudFormattedText(0, viewport_left+8, viewport_top+8, 0, positionStrings[actor.racePosition])
```

**Timer rendering** (flag bit 1):
- Iterates up to 6 racers
- Racer 0 (player): `"%s %02d:%02d.%02d"` format at top-left
- Racers 1-5: `"%02d:%02d.%02d"` format stacked below

**Lap/checkpoint counter** (flag bits 7-9):
- Bit 7: `"%s %d"` -- total race timer
- Bit 8: `"%s %d/%d"` -- lap/checkpoint progress
- Bit 9: circuit lap count for circuit tracks

---

## U-Turn Warning Icon

- **Sprite:** `UTURN` (64x64 scaled)
- **Position:** Screen center
- **Logic** (in RenderRaceHudOverlays, flag bit 4):
  - Calls `ComputeActorRouteHeadingDelta` (0x434040) to detect if player is facing backward
  - If heading delta is in range 0x400-0xC00 (facing >90 degrees wrong) AND wrong-way counter > 2:
    - Flashes at ~8Hz (tick counter modulo 32, visible when > 8)
  - Wrong-way counter increments when actor's track span decreases, resets on forward progress

---

## Minimap System

### InitializeMinimapLayout (0x43B0A0 / 0x43B116)
- Computes minimap bounds from scale factors:
  - Width: `scale_x * 100` (DAT_004b1130)
  - Height: `scale_y * 100` (DAT_004b11b8)
  - Map dot size: `scale_x * 7` (DAT_004b1128)
  - Scale per world unit: `scale_x * 0.0009765625` and `scale_y * 0.0009765625`
- **Position:** Bottom-left, at `y = gRenderHeight - height - scale_y * 8`, `x = scale_x * 8`
- Allocates 0x5C00 bytes for minimap quads (DAT_004b0a6c)
- Loads sprite assets:
  - `scandots` -- racer dot markers (color-coded: player=row0, AI=row1, other=row2)
  - `semicol` -- track segment connector dots
  - `scanback` -- minimap background tile (4x4 grid, quarter-size tiles)

**Track segment table (DAT_004b0a70..DAT_004b0a74):**
- Parses track strip data to build route segment ranges
- Handles track branches (type 0x08 = segment end, type 0x0B = branch connector)
- Supports both circuit and point-to-point tracks

### RenderTrackMinimapOverlay (0x43A220)
- **Only renders for point-to-point tracks** (`gTrackIsCircuit == 0`)
- Called from `RenderRaceHudOverlays` only in single-player (non-split) mode when bit 4 is set

**Rendering steps:**
1. Sets minimap clip rect and projection center offset
2. Computes player heading rotation:
   - `angle = 0x800 - (actor.heading >> 8)` (12-bit, player-up orientation)
   - `cos/sin` for 2D rotation matrix
3. Computes world-to-minimap offset: `-actor.worldX * worldScale`, `-actor.worldZ * worldScale`
4. **Background tiles** (0x30 iterations):
   - Submits pre-built `scanback` tile quads from the allocated buffer
5. **Track strip geometry** (main loop, up to 0x30 segments):
   - For each visible track span around the player:
     - Reads span origin + vertex data from `DAT_004c3d9c` (track strip records, 0x18 stride)
     - Reads vertex positions from `DAT_004c3d98` (vertex pool, 6-byte entries)
     - Transforms 2 pairs of vertices (4 corners) through player-relative rotation
     - Builds quad from the 4 road-edge vertices
     - Calls `BuildSpriteQuadTemplate` + `SubmitImmediateTranslucentPrimitive`
   - Also renders branch/alternate route quads when present
   - Handles checkpoint markers from `DAT_004aed88` array
6. **Racer dot markers** (per-actor loop):
   - For each racer within +-0x90 spans of the player:
     - Transforms world position through same rotation matrix
     - Renders `scandots` sprite quad at transformed position
     - Dot size: `scale_x * 7` square

**Circuit tracks:** The minimap is completely disabled for circuit tracks (early return
when `gTrackIsCircuit == 0` check fails -- note the inverted logic: the function only
executes when the track is NOT a circuit).

---

## Wanted/Cop Chase HUD

### InitializeWantedHudOverlays (0x43D2D0)
- Only active when `gWantedModeEnabled` (DAT_004aaf68) != 0
- Loads two sprites: `DAMAGE` and `DAMAGEB1` (damage indicator background + bar)
- Initializes damage counters: 0x1000 per tracked suspect (max health)

### UpdateWantedDamageIndicator (0x43D4E0)
- Renders damage indicator at the tracked suspect's screen position
- Projects suspect's 3D world position to 2D via `WritePointToCurrentRenderTransform`
- Damage bar height: `(0x1000 - damageValue) * scale_x * scale_y * 0.00024414063`
- Bar drawn adjacent to the damage background sprite

### AwardWantedDamageScore (0x43D690)
- Called on collision events
- Impact > 10000: -0x200 health (light hit, +10 points)
- Impact > 20000: -0x400 health (heavy hit, +20 points)
- On health reaching 0: +50 bonus points, `DAT_004bead0` arrest counter increments

### DrawRaceStatusText Wanted Messages (0x439B70)
- When `DAT_004bf50c < 300`: displays random wanted message for ~10 seconds
- Message pair from table at `PTR_s_SUSPECT_IS_WANTED_FOR_00474038`:
  - "SUSPECT IS WANTED FOR" / "FOR 1ST DEGREE MURDER."
  - "SUSPECT IS WANTED" / "ARMED ROBBERY."
  - etc. (8 random pairs, selected by `DAT_004bf508`)
- Flashes (odd frame skip) after frame 270

---

## Radial Pulse Overlay (RenderHudRadialPulseOverlay, 0x439E60)

- Animated 5-segment translucent pentagon that expands from center
- Triggered by setting `DAT_004b0fa0` to 0 (via `ResetHudRadialPulseOverlay`)
- Grows at `param_1 * 4.2` per frame until reaching 3000.0
- Radius: `gRenderWidth * progress * 0.0015625`
- Opacity: decays from 0xFF to 0x00 over the animation
- Pentagon vertices computed with 72-degree (0x33332 in 12-bit) angular steps
- Used as a visual flourish for race events (checkpoints, lap completion, etc.)

---

## Replay Banner

- **Sprite:** `REPLAY` (60x60 scaled)
- **Position:** Top-left viewport area, offset by `16*scale_x`, `16*scale_y`
- **Visibility:** HUD bitmask bit 31 (0x80000000), flashes every 32 ticks

---

## Rear-View Mirror

**No rear-view mirror exists in TD5.** Search confirmed: no mirror/rearview rendering functions.
The controller binding page has a "Rear View" header (`OpenControllerBindingPageRearViewHeader`,
`RenderControllerBindingPageBlankOrRearViewHeader`) but these are frontend UI labels only, not
in-race rendering. The game likely planned but never implemented a rear-view mirror.

---

## Split-Screen Support

- `DAT_004b1134` holds view count (1 or 2)
- Layout mode `gRaceViewportLayoutMode` (0=fullscreen, 1=left/right, 2=top/bottom)
- Each view gets its own:
  - Scale factors (halved in split dimension)
  - Viewport bounds
  - HUD element positions
  - Minimap is **disabled** in split-screen mode
- Divider bar rendered from `COLOURS` sprite at the end of `RenderRaceHudOverlays`

---

## Key Global Variables

| Address | Name | Description |
|---------|------|-------------|
| 0x4b1138 | scale_x | `gRenderWidth / 640.0` |
| 0x4b113c | scale_y | `gRenderHeight / 480.0` |
| 0x4b1134 | view_count | 1 (single) or 2 (split-screen) |
| 0x4b1130 | minimap_width | `scale_x * 100` |
| 0x4b11b8 | minimap_height | `scale_y * 100` |
| 0x4b0a6c | minimap_quad_buffer | 0x5C00 byte heap allocation |
| 0x4b0c00 | hud_flags_view0 | Bitmask controlling element visibility |
| 0x4b0c04 | hud_flags_view1 | Second view bitmask (split-screen) |
| 0x4b0fa4 | current_view_scale_ptr | Pointer to active view's scale array |
| 0x4b0bfc | current_hud_flags_ptr | Pointer to active view's flag word |
| 0x4b0a3c | numbers_atlas | Archive entry for digit atlas |
| 0x4b0fac | semicol_atlas | Archive entry for separator glyph |
| 0x4b11b4 | speedofont_atlas | Archive entry for speed digit font |
| 0x4b112c | gearnumbers_atlas | Archive entry for gear indicator |
| 0x4a2cb8 | glyph_uv_table | Per-font glyph UV/size records |
| 0x4a2cbc | text_quad_buffer | 0xB800 byte buffer for text quads |
| 0x4a2cc0 | queued_glyph_count | Current text glyph queue depth |
| 0x473e30 | gHudMetricMode | Cycling metric display mode (0-3) |
| 0x473e38 | position_string_table | Pointer array: "1ST".."6TH", labels |

---

## Sprite Rendering Primitives

All HUD elements flow through:
1. `BuildSpriteQuadTemplate` (0x432BD0) -- fills a 0xB8-byte quad struct with:
   - 4 vertex positions (mode flag bit 0): screen X/Y scaled by `_DAT_004c371c`
   - 4 UV coordinates (mode flag bit 1): atlas coords scaled by `_DAT_004749d0`
   - 4 vertex colors (mode flag bit 2)
   - Primitive type and texture binding
2. `SubmitImmediateTranslucentPrimitive` (0x4315B0) -- dispatches through
   `PTR_EmitTranslucentTriangleStrip_00473b9c` vtable, then issues D3D `DrawPrimitive`
   with vertex format 0x1C4 (transformed, lit, textured)

The HUD uses D3D **pre-transformed vertices** (screen-space), not world-space geometry.
This is consistent with the widescreen-correct finding: positions are computed at runtime
from the dynamic scale factors, not baked into texture coordinates.
