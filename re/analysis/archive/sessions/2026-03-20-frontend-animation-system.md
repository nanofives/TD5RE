# Frontend Animation System Analysis

## Overview

The TD5 frontend uses a **frame-count-based state machine** architecture where each screen function is a `switch` on `g_frontendInnerState`. Animations are driven by `DAT_0049522c` (the global frame/tick counter), incremented once per frame in `RunFrontendDisplayLoop` (0x414b50). There are **no easing functions** -- all motion is **strictly linear** with per-frame constant offsets. There is **no crossfade between screens** -- transitions use slide-in/slide-out of button sprites against a static background.

---

## 1. Screen Transition Architecture

### SetFrontendScreen (0x414610)
The canonical screen-change function:
```c
void SetFrontendScreen(int screenIndex) {
    g_frontendInnerState = 0;       // reset state machine
    DAT_0049522c = 0;               // reset frame counter
    g_frontendScreenFn = g_frontendScreenFnTable[screenIndex];
    g_frontendFrameTimestamp = timeGetTime();
}
```
Every screen change resets both the inner state and the tick counter. There is **no crossfade or wipe** between screens. The old screen simply stops running and the new one starts at state 0.

### Shared State Machine Pattern (all menu screens)
Every screen follows this template:

| State | Purpose | Frames |
|-------|---------|--------|
| 0 | **Setup**: Load TGA background, create button surfaces, set inline string table, play sound effect #5 ("whoosh in") | 1 frame |
| 1-2 | **Stabilization**: `PresentPrimaryFrontendBufferViaCopy()`, reset `DAT_0049522c = 0` | 2 frames |
| 3 | **Slide-in animation**: Buttons slide from off-screen to final positions | 39 frames (counter 0x00..0x27) |
| 4-5 | **Render preparation**: Draw option value surfaces, present overlays | 2 frames |
| 6 | **Interactive**: Accept input, handle button presses, left/right arrows for options | indefinite |
| 7 | **Slide-out setup**: Restore background, reset counter, play sound #5, set color key to white (0xFFFFFF) | 1 frame |
| 8 | **Slide-out animation**: Buttons scatter off-screen with varied velocities | 16 frames (counter 0x00..0x10) |
| (end) | Release surfaces, call `SetFrontendScreen(g_returnToScreenIndex)` | 1 frame |

---

## 2. Button Slide-In Animation (State 3)

All menu screens use the same counter-driven linear slide. `DAT_0049522c` counts from 0 to 0x27 (39 frames). Each button has its own velocity multiplier and start offset, creating a **staggered cascade** effect.

### ScreenOptionsHub example (0x41D890):
```c
// Buttons alternate direction: odd-indexed slide from right, even from left
MoveFrontendSpriteRect(0, DAT_0049522c * 0x10 - 0x266 + baseX, halfH - 0x8F);  // left, +16/frame
MoveFrontendSpriteRect(1, baseX + DAT_0049522c * -0x10 + 0x27A, halfH - 0x67); // right, -16/frame
MoveFrontendSpriteRect(2, DAT_0049522c * 0x10 - 0x266 + baseX, halfH - 0x3F);  // left, +16/frame
MoveFrontendSpriteRect(3, baseX + DAT_0049522c * -0x10 + 0x27A, halfH - 0x17); // right, -16/frame
MoveFrontendSpriteRect(4, DAT_0049522c * 0x10 - 0x266 + baseX, halfH + 0x11);  // left, +16/frame
MoveFrontendSpriteRect(5, halfW - 0x68, baseY + DAT_0049522c * -0x10 + 0x398); // OK: slides UP from below
```

**Pattern**: Buttons start approximately 614px (`0x266`) off-screen. At 16px/frame for 39 frames = 624px of travel, which puts them at their final centered position. The "OK" button slides vertically from below.

### Menu title label animation:
```c
// Title label slides down from above; moves 4px/frame
QueueFrontendOverlayRect(halfW - 200,
    (DAT_0049522c * 4 - 0xDC - titleOffsetY) + baseY,
    0, 0, titleWidth, titleHeight, 0, titleSurface);
```
The title starts 220px (`0xDC`) above its final position and descends at 4px/frame.

### Termination:
When `DAT_0049522c == 0x27` (39), the counter is clamped to 0x26 and `AdvanceFrontendTickAndCheckReady()` returns true after 2+ additional ticks, advancing to the interactive state.

---

## 3. Button Slide-Out Animation (State 8)

Slide-out is faster (16 frames vs 39) and uses varied per-button velocities creating a "scatter" effect:

### ScreenOptionsHub slide-out:
```c
// Each button has unique speed multipliers -- they don't all leave at the same rate
MoveFrontendSpriteRect(0, baseX + counter * -8 + 10,  baseY + counter * -0x30 + 0x10);  // slow X, fast Y
MoveFrontendSpriteRect(1, baseX + counter * -8 + 10,  baseY + counter * -0x20 + 0x38);  // slow X, medium Y
MoveFrontendSpriteRect(2, baseX + counter * -6 + 10,  baseY + counter * -0x18 + 0x60);  // slower X, slower Y
MoveFrontendSpriteRect(3, (halfW-200) + counter * 6,  baseY + counter * -0x18 + 0x88);  // rightward scatter
MoveFrontendSpriteRect(4, baseX + counter * -6 + 10,  baseY + counter * -0x18 + 0xB0);  // leftward scatter
MoveFrontendSpriteRect(5, (halfW-0x68) + counter * 6, counter * 0x30 + 0x128 + baseY);  // OK: drops downward
```

**Key insight**: Slide-out buttons move in **all directions** (not just back the way they came). Buttons scatter to upper-left, upper-right, and the OK button drops downward. This gives the exit animation a more dynamic "exploding" feel than the orderly slide-in.

### ScreenMainMenuAnd1PRaceFlow slide-out (state 9):
Uses an even wider variety of velocities (8, 10, 16, 20px/frame) and buttons scatter in 6+ different directions. Some move diagonally while the title label slides off to the upper-left at 24px/frame horizontally.

---

## 4. Car Selection Screen Animations

### Sidebar slide-in (state 2):
The car selection screen has a unique **scrolling sidebar** animation where a vertical bar (`CarSelBar1.tga`) and curve element (`CarSelCurve.tga`) slide in from the right edge:
```c
// Bar slides 8px/frame from right edge
QueueFrontendOverlayRect(screenWidth + counter * -8 - 4, 0, ...); // vertical bar
QueueFrontendOverlayRect(screenWidth + counter * -8 - 4, 0x198, ...); // curve element
```
Simultaneously, a top bar (`CarSelTopBar.tga`) slides from the left:
```c
if (counter * 8 < 0x215) offset = counter * 8 - 0x214; else offset = 0;
QueueFrontendOverlayRect(offset, 0x2D, 0, 0, 0x214, 0x24, ...);
```

### Button fly-in (state 5):
Car selection buttons slide from the far right with variable speeds:
```c
// All 6 buttons slide left at 32px/frame (0x20) from off-screen starting positions
MoveFrontendSpriteRect(0, baseX + counter * -0x20 + 0x308, ...);  // 32px/frame
```
Vertical positions also animate with different rates (16px, 8px/frame) creating a fan-out from a clustered start.

Terminates at counter == 0x18 (24 frames).

---

## 5. Fade System

### Bar-Fade Effect (RenderFrontendFadeEffect, 0x411780)
A **scanline-sweep fade** that processes the screen in horizontal bands of 64 lines:

- `DAT_00494fc8` = current scanline position (advances +2/frame)
- Processes a 64-line band starting at `max(0, position - 64)`
- Uses a **dither lookup table** (`DAT_00494bc0`, built by `BuildFrontendDitherOffsetTable` at 0x411710)
- The table is a 32x32 grid where each entry moves a 5-bit color channel one step toward the target color
- Supports both RGB555 (mode 0xF) and RGB565 (mode 0x10) pixel formats
- `InitFrontendFadeColor(color)` extracts the target color's 5-bit channels as `color >> 3 & 0x1F1F1F`
- The sweep moves top-to-bottom, fading each band by one dither step per frame
- Terminates when `DAT_00494fc8` passes the screen height, clearing `DAT_00494fc0`

**Character**: This is NOT a standard alpha-blend fade. It's a **progressive dither** -- each pixel's RGB channels are moved one step per sweep pass toward the target color. The 32-entry dither table ensures this converges over ~32 frames, giving a grainy, stepped fade look characteristic of 1990s 16-bit software rendering.

### Bar-Fade-Out Effect (RenderFrontendFadeOutEffect, 0x411A70)
Same sweep mechanism but **cross-blends two surfaces** (DAT_00496260 and DAT_00496264) using the dither table, reading both surfaces' pixels and combining channels via the lookup. Used for fading one image into another with the same band-sweep progression.

### Usage:
- `RunAttractModeDemoScreen` (0x4275A0): state 4 calls `InitFrontendFadeColor(0)` (fade to black), state 5 calls `RenderFrontendFadeEffect()` each frame until complete
- Legal screens do NOT fade -- they display with instant cut and wait for timeout/keypress

---

## 6. Cross-Fade System

### CrossFade16BitSurfaces (0x40CDC0 / 0x40D190)
A **MMX-accelerated weighted pixel blend** between two surfaces:

- `param_1` = blend weight (0..32), clamped to range [0, 0x20]
- Processes 4 pixels at once using MMX `paddusw`/`psllw` intrinsics
- Blend formula per channel: `result = (src1_channel * weight1 + src2_channel * weight2) / 32`
- Three surfaces locked: DAT_00496260 (surface A), DAT_00496264 (surface B), DAT_00495220 (back-buffer / output)
- Handles both RGB555 and RGB565 by adjusting shift amounts (5 vs 6 for green channel)

The second variant at 0x40D190 takes an additional source surface pointer and handles transparent pixels (zero = key color) by substituting the other surface's pixel unchanged.

### AdvanceCrossFadeTransition (0x40D120)
```c
undefined4 AdvanceCrossFadeTransition(void) {
    DAT_004951dc = 1;
    CrossFade16BitSurfaces(DAT_0049522c, 0, 0, screenWidth, screenHeight);
    if (DAT_0049522c == 0x22) {  // 34 frames for full crossfade
        DAT_004951dc = 0;
        BlitSecondaryFrontendRectToPrimary(0, 0, screenWidth, screenHeight);
        DAT_0048f2fc = 0;
        return 1;  // done
    }
    return 0;  // still in progress
}
```
**Duration**: 34 frames (counter 0..0x22). Weight ramps linearly from 0 to 34, but is clamped to 32, so the last 2 frames are fully blended.

### Usage in Extras Gallery:
The `UpdateExtrasGalleryDisplay` (0x40D830) uses cross-fade with a **half-life decay curve** for the blend weight:
```c
DAT_0048f2fc = DAT_0048f2fc / 2;  // halves each frame
// Compute blend weights from decayed counter:
if (counter > 0x60) weight = 0x80 - counter;  // ramp up
if (counter < 0x20) weight = counter * 2 - 0x20;  // ramp down
```
This creates a **smooth ease-in/ease-out** for the gallery image transitions by halving the counter each frame (exponential decay) and mapping it to blend weights via clamped linear ramps.

---

## 7. Button Rendering System

### Button Surface Layout
Each button is a single DirectDraw surface containing **two frames vertically stacked**:
- **Top half (y=0)**: Highlighted state (bright)
- **Bottom half (y=height)**: Normal state (dark, with half-brightness color shift)

Created by `CreateFrontendDisplayModeButton` (0x425DE0):
1. Surface allocated at 2x height (normal mode) or 1x height (preview mode)
2. `DrawFrontendButtonBackground` called twice: once with `style=1` (highlighted) at y=0, once with `style=0` (normal) at y=height
3. Text drawn at both offsets
4. For preview-mode buttons, a half-brightness filter is applied via `pixel >> 1 & mask` + tint

### DrawFrontendButtonBackground (0x425B60)
Draws a **9-slice frame** from `ButtonBits.tga` (DAT_00496268):
- Tiles consist of corner pieces, horizontal fill strips (4px wide, repeated), and vertical borders
- Three style variants: style 0 = normal (dark), style 1 = highlighted (bright), style 2 = preview/disabled
- Sprite pieces are blitted via BltFast (vtable+0x1C) with source rects computed from `style * 0x20` offsets

### Button State Switching (RenderFrontendUiRects, 0x425A30)
The highlight effect works by **shifting the source rectangle** into the button surface:
```c
if (DAT_0049b68c == 0 && buttonIndex == g_frontendButtonIndex) {
    sourceYOffset = srcBottomY - srcTopY;  // = button height
} else {
    sourceYOffset = 0;
}
// Blit from: srcTopY + sourceYOffset  (shifts to highlighted half when selected)
```
When `DAT_0049b68c == 1` (cursor overlay active = animation in progress), ALL buttons show their normal state. Only when the cursor overlay is deactivated (interactive state) does the selected button show its highlighted frame.

### Highlight Rectangle (RenderFrontendDisplayModeHighlight, 0x4263E0)
Additionally draws a **colored rectangular frame** (color 0xC000 = bright green in RGB565) around the currently hovered button using 4x `BltColorFillToSurface` calls (2px-wide lines).

---

## 8. Button Hover Animation Counter

`DAT_00499ca8[buttonIndex * 0xD]` stores a per-button animation counter:
- **When hovered** (`buttonIndex == g_frontendButtonIndex`): increments toward 6
- **When not hovered**: decrements toward 0
- Updated each frame in `UpdateFrontendDisplayModeSelection` (0x426580)
- This counter is NOT currently used for rendering interpolation in the decompiled code -- it may have been intended for a smooth transition between normal/highlight states but the actual state switch is binary (source rect flip)

---

## 9. Cursor/Mouse System

### ActivateFrontendCursorOverlay (0x4258C0)
```c
DAT_0049b68c = 1;   // cursor overlay active (suppresses button highlighting)
DAT_00498714 = 0;   // clear mouse-movement flag
```

### DeactivateFrontendCursorOverlay (0x4258E0)
Simply sets `DAT_0049b68c = 0`, re-enabling button highlight rendering.

### Mouse cursor rendering (in RunFrontendDisplayLoop):
```c
if (DAT_0049b68c == 0 && mouseInputActive == 1) {
    QueueFrontendOverlayRect(mouseX, mouseY, 0, 0, 0x16, 0x1E, 0xFF0000, cursorSurface);
}
```
The cursor is a 22x30 pixel sprite (`snkmouse.tga`), drawn as a frontend overlay at the current mouse position. Color key = red (0xFF0000).

### Mouse/keyboard input mode detection:
```c
if (mouseDeltaX + mouseDeltaY > 8 || mouseButtonChanged) {
    DAT_00498714 = 1;  // mouse is active
}
```
The system dynamically switches between keyboard and mouse input modes based on detected movement.

---

## 10. TGA Background Loading Pipeline

### LoadFrontendTgaSurfaceFromArchive (0x412030)
1. `OpenArchiveFileForRead(tgaName, zipName)` -- reads from ZIP archive
2. `DX::Allocate(0x1D4C00)` -- allocates 1.9MB conversion buffer
3. `DX::ImageProTGA()` -- decodes TGA to 16-bit pixel data using the active pixel format masks
4. `CreateTrackedFrontendSurface(width, height)` -- allocates DirectDraw surface
5. Lock surface, copy decoded pixels row-by-row, unlock
6. Set color key for transparency

### LoadTgaToFrontendSurface16bpp (0x4125B0)
Loads a TGA directly into the primary frontend surface (DAT_00496260) for background images. This is used for the menu background (`MainMenu.tga`).

### RenderTgaToFrontendSurface (0x4129B0)
**Inverts** a TGA surface's colors (per-channel `31 - value`) -- used to prepare the font texture for the color-keyed text rendering system. Called after loading `SmallText.tga` to create the inverted variant.

### DisplayLoadingScreenImage (0x42CA00)
Fixed-resolution path that decodes TGA directly into the DXDraw primary surface at 640x480, with hardcoded width 0x280 and pixel count 0x4B000. Used only for legal screens and loading screens.

---

## 11. Legal/Splash Screens

### ShowLegalScreens (0x42C8E0)
Completely different from the menu system -- no state machine, no animation:
1. Load `legal1.tga` from `LEGALS.ZIP`
2. `DisplayLoadingScreenImage` -- instant full-screen blit + flip
3. Busy-wait loop: `timeGetTime()` polling, 5-second timeout or keypress (with 400ms dead zone)
4. Repeat for `legal2.tga`

**No fade in or out**. Pure instant cut.

---

## 12. Attract Mode / Demo Screen

### RunAttractModeDemoScreen (0x4275A0)
Triggered from the main menu after 50 seconds of inactivity:
1. States 0-3: Present current buffer, release buttons, stabilize (2 frames)
2. State 4: `InitFrontendFadeColor(0)` -- initialize fade to black
3. State 5: `RenderFrontendFadeEffect()` each frame until the sweep completes
4. Then `InitializeRaceSeriesSchedule()` + `InitializeFrontendDisplayModeState()` to enter demo race

The attract mode is the ONLY frontend transition that uses the bar-fade effect. All other screen changes use slide-in/slide-out.

---

## 13. Extras Gallery Image Slideshow

### UpdateExtrasGalleryDisplay (0x40D830)
Runs as a background process during the main menu (called from `FlushFrontendSpriteBlits`):

- Uses `CrossFade16BitSurfaces` with an exponential-decay weight curve
- `DAT_0048f2fc` halved each frame (`counter / 2`) creating smooth ease-in/ease-out
- Random position and timing: `AdvanceExtrasGallerySlideshow` picks a random image, random position (modulo 0x150/0x1F4 ranges), and random display duration (0x8C..0x18C frames)
- Cross-fades between current and new gallery image with per-frame blend weight updates
- When `DAT_0048f2fc` reaches -0x18, triggers next slideshow advance

### Extras Gallery Screen (0x417D50)
State 0 waits for the slideshow to settle (`DAT_0048f2fc < -0xF`), then loads developer mugshot TGAs from `Extras_Mugshots.zip` (Bob, Gareth, Snake, MikeT, Chris, Headley, Steve).

---

## 14. Timing Model

All frontend animations are **frame-count based**, not time-based:

- `DAT_0049522c` incremented by 1 at the end of each `RunFrontendDisplayLoop` call
- No delta-time compensation -- animations run faster on faster machines
- Target frame rate appears to be ~30fps based on the presentation timing calibration in `InitializeFrontendPresentationState`
- `AdvanceFrontendTickAndCheckReady` (0x4259B0): simple counter that requires 3+ ticks before returning true, providing a minimum delay between states

### Frame counts for key animations:
| Animation | Frames | ~Duration @30fps |
|-----------|--------|-------------------|
| Slide-in (menus) | 39 | 1.3 sec |
| Slide-out (menus) | 16 | 0.53 sec |
| Cross-fade (full) | 34 | 1.13 sec |
| Bar-fade sweep | ~screenHeight/2 | ~8 sec for 480 lines |
| Car selection bar slide | screenWidth/8 | ~2.7 sec |
| Car selection button fly-in | 24 | 0.8 sec |
| Extras gallery hold | 140-396 | 4.7-13.2 sec |
| Legal screen hold | 5000ms (time-based!) | 5 sec |

---

## 15. Sound Effects Integration

Each animation phase plays a specific sound:
- **SFX 5**: "Whoosh" -- played at start of slide-in and slide-out
- **SFX 4**: "Click/Ready" -- played when slide-in completes (interactive state reached)
- **SFX 3**: "Confirm" -- played on button activation (Enter/click)
- **SFX 2**: "Navigate" -- played on button focus change (up/down/mouse hover)
- **SFX 8**: Special car selection screen intro sound
- **SFX 9**: Car selection ambient sound

---

## Key Globals Summary

| Address | Name | Purpose |
|---------|------|---------|
| `0x0049522c` | `DAT_0049522c` | Global frame/tick counter (animation driver) |
| `g_frontendInnerState` | | Per-screen state machine position |
| `g_frontendScreenFn` | | Current screen function pointer |
| `g_frontendButtonIndex` | | Currently selected/hovered button index |
| `DAT_0049b68c` | | Cursor overlay active (1=animating, 0=interactive) |
| `DAT_004951e8` | | Button activation flag (1=pressed this frame) |
| `DAT_0049b690` | | Arrow direction for option cycling (-1/0/1) |
| `DAT_00494fc0` | | Fade active flag |
| `DAT_00494fc4` | | Fade target color (5-bit packed RGB) |
| `DAT_00494fc8` | | Fade scanline sweep position |
| `DAT_00494bc0` | | Dither offset LUT (32x32 = 1024 bytes) |
| `DAT_004951dc` | | Cross-fade transition state |
| `DAT_0048f2fc` | | Gallery cross-fade decay counter |
| `DAT_00498704` | | Sprite blit skip counter (for background restore) |
| `DAT_004951fc` | | Double-buffer page flip index (0/1) |

---

## Key Functions Summary

| Address | Name | Role |
|---------|------|------|
| `0x414b50` | `RunFrontendDisplayLoop` | Main per-frame loop: input, screen fn, UI render, present |
| `0x414610` | `SetFrontendScreen` | Screen change: resets state + counter, sets function pointer |
| `0x411780` | `RenderFrontendFadeEffect` | Bar-sweep dither fade (to solid color) |
| `0x411a70` | `RenderFrontendFadeOutEffect` | Bar-sweep cross-blend (between two surfaces) |
| `0x40cdc0` | `CrossFade16BitSurfaces` | MMX-accelerated weighted pixel blend |
| `0x40d120` | `AdvanceCrossFadeTransition` | 34-frame full-screen crossfade driver |
| `0x4259d0` | `MoveFrontendSpriteRect` | Repositions a button rect (preserving width/height) |
| `0x425a30` | `RenderFrontendUiRects` | Iterates button/overlay tables, issues sprite blits |
| `0x425730` | `QueueFrontendSpriteBlit` | Clips and enqueues one sprite blit (max 65 sprites) |
| `0x425540` | `FlushFrontendSpriteBlits` | Restores dirty rects, renders highlight, executes blits |
| `0x425de0` | `CreateFrontendDisplayModeButton` | Creates dual-frame button surface (normal+highlight) |
| `0x425b60` | `DrawFrontendButtonBackground` | 9-slice frame renderer for button chrome |
| `0x4263e0` | `RenderFrontendDisplayModeHighlight` | Draws green rect around hovered button |
| `0x426580` | `UpdateFrontendDisplayModeSelection` | Keyboard/mouse navigation, hover detection |
| `0x412030` | `LoadFrontendTgaSurfaceFromArchive` | ZIP->TGA->16bpp surface pipeline |
| `0x4129b0` | `RenderTgaToFrontendSurface` | Color-inverts surface for font rendering |
| `0x411710` | `BuildFrontendDitherOffsetTable` | Builds 32x32 dither LUT for fade effects |
