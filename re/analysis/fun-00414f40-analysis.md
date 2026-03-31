# FUN_00414f40 Analysis

## Address
`0x00414F40` - `0x0041502B`

## Proposed Name
**RenderPositionerGlyphStrip**

## Decompiled Code (Cleaned Up)

```c
// Renders a horizontal strip of font glyphs for the Screen Positioner debug tool.
// Draws a window of up to 16 characters centered on g_positionerCursorIndex,
// with each glyph sourced from the menu font spritesheet (7x7 grid, cell size 0x24).
// A separator rect is inserted between each glyph. The cursor glyph gets a
// special "arrow" overlay appended at the end.
//
// param_1: initial x-offset for the cursor highlight (overridden internally)
// Returns:  the final x-position of the cursor glyph (used by caller to draw highlight)

int __fastcall RenderPositionerGlyphStrip(int param_1)
{
    int charIndex;
    int *entryPtr;
    int width;
    int xPos;
    int cursorX;

    charIndex = g_positionerCursorIndex - 8;   // start 8 entries before cursor
    xPos = 0;
    cursorX = param_1;

    if (charIndex < g_positionerCursorIndex + 8) {
        entryPtr = &g_positionerCharData[charIndex * 2];  // DAT_00495260, stride-8 array

        do {
            if ((int)entryPtr < (int)&g_positionerCharData[0]) {
                // Before start of array -- skip, just advance x by 0x28
                xPos += 0x28;
            }
            else {
                if (charIndex == g_positionerCursorIndex) {
                    cursorX = xPos;     // record cursor glyph position
                }

                // Draw the glyph from the font spritesheet
                // Source rect: column = (charIndex % 7) * 0x24, row = (charIndex / 7) * 0x24
                // Dest y = entryPtr[1] + 0xF0
                QueueFrontendOverlayRect(
                    xPos,
                    entryPtr[1] + 0xF0,         // y position (char y-offset + 240)
                    (charIndex % 7) * 0x24,      // src x in spritesheet
                    (charIndex / 7) * 0x24,      // src y in spritesheet
                    0x24, 0x24,                  // 36x36 cell
                    0,
                    g_menuFontSurface
                );

                // Draw a vertical separator bar from the button index surface
                width = *entryPtr;               // per-char width
                QueueFrontendOverlayRect(
                    xPos + width,
                    0xDC,                        // y = 220
                    0, 0,                        // src 0,0
                    4, 0x28,                     // 4x40 separator
                    0,
                    g_frontendButtonIndex
                );

                xPos += width + 4;
            }

            charIndex++;
            entryPtr += 2;
        } while (charIndex < g_positionerCursorIndex + 8);
    }

    // Draw cursor/arrow indicator at the recorded cursor position
    QueueFrontendOverlayRect(
        cursorX,
        0x138,              // y = 312
        0x24, 0,            // src rect offset
        0x24, 0x24,         // 36x36
        0,
        g_menuFontSurface
    );

    return cursorX;
}
```

## What It Does

This function renders a horizontal strip of font character glyphs for the **Screen Positioner debug tool**. It is a rendering helper that:

1. **Iterates over a window of 16 characters** (cursor index +/- 8) from the positioner's character data array at `DAT_00495260`.
2. **Draws each visible glyph** by computing its source rectangle in a 7-column font spritesheet (`g_menuFontSurface`), using `QueueFrontendOverlayRect`.
3. **Inserts a 4-pixel-wide vertical separator** between glyphs using the button index surface.
4. **Tracks the cursor position**: when the loop reaches the character at `g_positionerCursorIndex` (`DAT_0049521c`), it records the current x-offset.
5. **Draws a cursor/arrow indicator** below the strip at the cursor's x-position (y=0x138=312).
6. **Returns the cursor x-position** so the caller can draw additional selection overlays.

## Key Globals

| Address | Proposed Name | Role |
|---------|--------------|------|
| `DAT_0049521c` | `g_positionerCursorIndex` | Currently selected character index in the positioner |
| `DAT_00495260` | `g_positionerCharData` | Array of (width, y-offset) pairs per character slot (stride 8 bytes) |
| `g_menuFontSurface` | (already named) | Surface handle for the menu font spritesheet |
| `g_frontendButtonIndex` | (already named) | Surface handle for the button/UI element spritesheet |

## How It Fits Into the Frontend System

The **ScreenPositionerDebugTool** (0x415030) is a developer/debug screen that allows manual positioning of font characters on-screen. It operates through a state machine (`g_frontendInnerState`):

- **State 0**: Loads the "Front_End_Positioner.tga" surface and clears the screen.
- **State 1**: Presents the buffer.
- **State 2**: Initializes `g_positionerCharData` from `g_largeFontDisplay` -- copies each character's default width, sets y-offsets to 0.
- **State 3**: Calls `RenderPositionerGlyphStrip(0)` to display the glyph strip, then processes left/right/up/down input to **move the cursor** through the character set. This is the "character selection" mode.
- **State 4**: Calls `RenderPositionerGlyphStrip(0)` again, uses the returned cursor position to draw a highlight, and processes input to **adjust individual character width and y-offset** values.
- **State 5**: Exports the final positioning data to `positioner.txt` (labeled "Created by SNK Positioner"), writing width and y-offset tables.

`RenderPositionerGlyphStrip` is the core rendering routine for states 3 and 4, providing the visual representation of the character strip and communicating the cursor position back to the caller for overlay rendering. The full character set is a 7x37 grid (259 characters corresponding to the ASCII-like font map at `0x465960`), and this function shows a scrolling 16-character window around the selected character.

This is a **debug/development tool** for fine-tuning font character positioning -- the output file format ("Created by SNK Positioner") confirms it was used by the original developers at SNK (the studio) for authoring font layout data.
