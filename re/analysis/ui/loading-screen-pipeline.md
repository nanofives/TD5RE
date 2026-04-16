# Loading Screen Pipeline

> Full reverse engineering of TD5's loading screen system: image selection, ZIP extraction,
> TGA decoding, surface blit, and the two distinct use-sites (legal screens and race loading).

---

## Table of Contents

1. [Overview](#1-overview)
2. [Game State Machine Context](#2-game-state-machine-context)
3. [Loading Image Selection (Race)](#3-loading-image-selection-race)
4. [Legal / Splash Screens](#4-legal--splash-screens)
5. [DisplayLoadingScreenImage (Core Renderer)](#5-displayloadingscreenimage-core-renderer)
6. [TGA Decode via DX::ImageProTGA](#6-tga-decode-via-dximageprotga)
7. [Archive Extraction Path](#7-archive-extraction-path)
8. [Loading Screen Timing and Interactivity](#8-loading-screen-timing-and-interactivity)
9. [Key Addresses](#9-key-addresses)

---

## 1. Overview

TD5 has **two distinct loading screen paths** that share a single rendering function:

| Path | Function | ZIP Archive | Images | Trigger |
|------|----------|-------------|--------|---------|
| **Legal/splash** | `ShowLegalScreens` (0x42C8E0) | `LEGALS.ZIP` | `legal1.tga`, `legal2.tga` | Game startup (GAMESTATE_INTRO) |
| **Race loading** | `InitializeRaceSession` (0x42AA10) | `LOADING.ZIP` | `load00.tga` .. `load19.tga` | Transition from menu to race |

Both paths converge on **`DisplayLoadingScreenImage`** (0x42CA00), which decodes a TGA from
memory and blits it directly to the DirectDraw primary surface at a fixed 640x480 resolution.

There is **no progress bar, no animation, and no incremental updates** during loading. The
loading image is displayed exactly once at the start of `InitializeRaceSession`, and the screen
remains static while the entire race session initializes (track data, vehicle assets, textures,
audio, actors, HUD, etc.). The screen is only replaced when the first race frame renders.

---

## 2. Game State Machine Context

The top-level game loop lives in **`RunMainGameLoop`** (0x442170), which is a `switch` on
`g_gameState`:

```
GAMESTATE_INTRO (0):
    1. Play intro movie (if enabled and pending)
    2. DXD3D::InitializeMemoryManagement()
    3. DXD3D::SetRenderState()
    4. ShowLegalScreens()          <-- legal splash screens
    5. g_gameState = GAMESTATE_MENU

GAMESTATE_MENU (1):
    1. Initialize frontend resources if pending
    2. RunFrontendDisplayLoop()    <-- main menu / car select / etc.
    3. If start-race confirmed:
       a. g_frontendResourceInitPending = 1
       b. InitializeRaceSession()  <-- loading screen shown here
       c. g_gameState = GAMESTATE_RACE

GAMESTATE_RACE (2):
    1. RunRaceFrame() each tick
    2. On race end -> back to GAMESTATE_MENU

GAMESTATE_BENCHMARK (3):
    1. Separate benchmark image display path
    2. Uses same surface blit pattern but from FPS screenshot file
```

The critical transition is `GAMESTATE_MENU -> GAMESTATE_RACE`. When the player confirms a race
start, `RunMainGameLoop` calls `InitializeRaceSession()` **synchronously**. This function:
1. Displays a random loading image immediately
2. Performs all heavy asset loading (track, vehicles, textures, audio, HUD)
3. Returns, and the game loop enters `GAMESTATE_RACE`

The loading screen is visible for the entire duration of step 2.

---

## 3. Loading Image Selection (Race)

Inside `InitializeRaceSession` (0x42AA10), the loading image is chosen as follows:

```
// At 0x42AA73..0x42AA8B (assembly):
CALL  _rand                    // eax = rand()
CDQ                             // sign-extend eax into edx:eax
MOV   ECX, 0x14                // ecx = 20
IDIV  ECX                       // edx = eax % 20 (remainder)
PUSH  EDX                       // push image index
PUSH  "load%02d.tga"           // format string at 0x4672B0
LEA   EDX, [ESP+0x1C]          // local buffer
PUSH  EDX
CALL  sprintf_game              // -> "load00.tga" .. "load19.tga"
```

**The image index is `rand() % 20`**, giving a uniform random selection among 20 loading
images: `load00.tga` through `load19.tga`.

The random seed used here comes from the race session random seed chain:
1. `g_randomSeedForRace` is set during frontend
2. Copied to `g_raceSessionRandomSeed`
3. `__set_new_handler(g_raceSessionRandomSeed)` reseeds the CRT RNG
4. A table of random values (`g_raceRandomSeedTable`) is filled with `_rand()` calls
5. One additional `_rand()` call produces the loading image index

So the loading image is **not related to the track or game mode** -- it is purely random,
seeded from the session seed.

### Image Loading Sequence

```c
// Pseudocode from InitializeRaceSession (0x42AA10):
sprintf_game(filename, "load%02d.tga");                       // e.g. "load07.tga"
size = GetArchiveEntrySize(filename, "LOADING.ZIP");          // query ZIP for entry size
buffer = malloc(size + 0x96000);                              // alloc TGA + decode space
ReadArchiveEntry(filename, "LOADING.ZIP", buffer, size);      // extract from ZIP
DisplayLoadingScreenImage(buffer, buffer + size);             // decode & display
free(buffer);                                                  // release immediately
```

The extra `0x96000` bytes (614,400 = 640 * 480 * 2) appended to the allocation serve as the
decoded 16bpp pixel buffer. The TGA compressed data sits at the start of the buffer, and the
decoded pixels are written starting at `buffer + size`.

---

## 4. Legal / Splash Screens

**`ShowLegalScreens`** (0x42C8E0) displays two legal notice images at startup:

```c
void ShowLegalScreens(void) {
    // --- Legal Screen 1 ---
    size = GetArchiveEntrySize("legal1.tga", "LEGALS.ZIP");
    buf = malloc(size + 0x96000);
    ReadArchiveEntry("legal1.tga", "LEGALS.ZIP", buf, size);
    DisplayLoadingScreenImage(buf, buf + size);
    free(buf);

    startTime = timeGetTime();
    done = false;
    do {
        elapsed = timeGetTime() - startTime;
        if (elapsed > 5000) done = true;           // 5-second timeout
        if (elapsed > 400) {                        // 400ms grace before accepting input
            DXInput::GetKB();
            for (key = 0; key < 256; key++) {
                if (DXInput::CheckKey(key)) done = true;
            }
        }
    } while (!done);

    // --- Legal Screen 2 --- (identical pattern)
    size = GetArchiveEntrySize("legal2.tga", "LEGALS.ZIP");
    buf = malloc(size + 0x96000);
    ReadArchiveEntry("legal2.tga", "LEGALS.ZIP", buf, size);
    DisplayLoadingScreenImage(buf, buf + size);
    free(buf);

    // Same 5s timeout / any-key-to-skip loop
}
```

Key details:
- Each legal screen displays for **up to 5 seconds**
- After a **400ms grace period**, any keyboard key press skips to the next screen
- There is no mouse or gamepad input check -- only keyboard via `DXInput::CheckKey`
- Both images come from `LEGALS.ZIP` (separate from `LOADING.ZIP`)

---

## 5. DisplayLoadingScreenImage (Core Renderer)

**`DisplayLoadingScreenImage`** (0x42CA00) is the shared function that decodes and presents
a TGA image to the screen.

```c
void __cdecl DisplayLoadingScreenImage(void *tga_data, int decoded_pixel_buf) {
    // 1. Determine pixel format masks based on display bit depth
    int bpp = *(int*)(dd_exref + 0x16A0);  // current display bits-per-pixel
    uint rMask, gMask, bMask;

    if (bpp == 15) {        // 5-5-5 format
        rMask = 0x7C00;  gMask = 0x3E0;   bMask = 0x1F;
    } else if (bpp == 16) { // 5-6-5 format
        rMask = 0xF800;  gMask = 0x7E0;   bMask = 0x1F;
    } else {                // 32-bit (unlikely for loading screen)
        rMask = 0xFF0000; gMask = 0xFF00; bMask = 0xFF;
    }

    // 2. Set up Image_exref decode descriptor (tagDXIMAGELINE structure)
    *(void**)(Image_exref + 0x00) = tga_data;         // source TGA data
    *(int*)  (Image_exref + 0x08) = decoded_pixel_buf; // output pixel buffer
    *(void**)(Image_exref + 0x10) = local_palette;     // 1KB palette buffer (on stack)
    *(int*)  (Image_exref + 0x40) = 1;                 // decode flag
    *(int*)  (Image_exref + 0x24) = 0;                 // x offset
    *(int*)  (Image_exref + 0x20) = 0;                 // y offset
    *(int*)  (Image_exref + 0x2C) = 5;                 // decode mode (16bpp TGA)
    *(uint*) (Image_exref + 0x30) = rMask;             // red channel mask
    *(uint*) (Image_exref + 0x34) = gMask;             // green channel mask
    *(uint*) (Image_exref + 0x38) = bMask;             // blue channel mask

    // 3. Decode TGA to 16bpp pixel buffer
    DX::ImageProTGA((tagDXIMAGELINE*)Image_exref);

    // 4. Lock primary DirectDraw surface
    DDSURFACEDESC ddsd = {0};   // 0x6C bytes, zeroed
    ddsd.dwSize = 0x6C;
    hr = primarySurface->Lock(NULL, &ddsd, DDLOCK_WAIT, NULL);
    if (hr != DD_OK) {
        Msg("Lock failed in DisplayLoadingPicture\n%s", DXErrorToString(hr));
        return;
    }

    // 5. Blit decoded pixels to surface (hardcoded 640x480 @ 16bpp)
    int srcOffset = 0;
    int dstRow = 0;
    for (int y = ddsd.dwHeight; y > 0; y--) {
        for (int x = 0; x < ddsd.dwWidth; x++) {
            if (x < 0x280 && srcOffset < 0x4B000) {   // 640 wide, 640*480=307200 pixels
                dst[dstRow + x] = src[srcOffset + x];  // copy 16-bit pixel
            } else {
                dst[dstRow + x] = 0;                   // black padding
            }
        }
        srcOffset += 0x280;              // 640 pixels per source row
        dstRow += ddsd.lPitch / 2;       // surface pitch in 16-bit words
    }

    // 6. Unlock and flip
    primarySurface->Unlock(NULL);
    DXDraw::Flip(isWindowed);
}
```

### Key Observations

- **Hardcoded 640x480**: The source image is always 640 pixels wide (0x280) and 307,200 pixels
  total (0x4B000 = 640 * 480). If the display surface is larger, excess pixels are filled black.
- **16bpp output**: The TGA is decoded directly to 16-bit color (5-5-5 or 5-6-5 depending on
  the adapter's reported bit depth).
- **Direct surface lock**: Uses `IDirectDrawSurface::Lock` / `Unlock` with `DDLOCK_WAIT` flag,
  bypassing any 3D rendering pipeline.
- **Single flip**: After the blit, `DXDraw::Flip` presents the frame. This is the only frame
  rendered for the loading screen -- there is no animation loop.
- **Widescreen patch site**: The `DXDraw::Flip` call at the end of this function is noted as a
  widescreen patch target. The mod at `0x45C330` redirects this call to apply aspect-ratio
  correction before flipping.

---

## 6. TGA Decode via DX::ImageProTGA

The TGA decoding is handled by `DX::ImageProTGA` in the M2DX.DLL engine. It uses a descriptor
structure (`tagDXIMAGELINE`) configured by the caller:

| Offset | Field | Value in Loading Screen Context |
|--------|-------|---------------------------------|
| 0x00 | Source data pointer | Raw TGA file bytes from ZIP |
| 0x08 | Destination pixel pointer | Allocated buffer past TGA data |
| 0x10 | Palette buffer | 1024-byte stack buffer |
| 0x20 | Y offset | 0 |
| 0x24 | X offset | 0 |
| 0x2C | Decode mode | 5 (16bpp output) |
| 0x30 | Red mask | Depends on display bpp |
| 0x34 | Green mask | Depends on display bpp |
| 0x38 | Blue mask | Depends on display bpp |
| 0x40 | Decode flag | 1 (enabled) |

The decode mode `5` produces 16-bit pixels with the channel masks matching the current
DirectDraw surface format. This avoids any per-pixel format conversion during the surface blit.

---

## 7. Archive Extraction Path

Both loading paths use the same ZIP extraction pipeline:

```
GetArchiveEntrySize(filename, zipname)
    |
    +-- Try fopen(filename) directly (loose file override)
    |   If found: fseek to end, return size
    |
    +-- ParseZipCentralDirectory(basename, zipname)
        Scans ZIP central directory for matching entry
        Returns uncompressed size

ReadArchiveEntry(filename, zipname, dest, maxsize)
    |
    +-- Try fopen(filename) directly (loose file override)
    |   If found: fread into dest, return bytes read
    |
    +-- ParseZipCentralDirectory(basename, zipname)
    |   Finds the entry and sets DAT_004cf988 = local header offset
    |
    +-- fopen(zipname) -> DAT_004cf97c
    +-- fseek to local header offset
    +-- DecompressZipEntry()
    |   Uses DEFLATE (inflate) decompressor
    +-- fclose
    +-- Return decompressed size
```

**Loose file override**: Both `GetArchiveEntrySize` and `ReadArchiveEntry` first attempt to
open the filename directly via `fopen`. If a loose file exists on disk with the same name
(e.g., `load07.tga` in the working directory), it is used instead of the ZIP archive entry.
This provides a simple modding path for custom loading screens.

### ZIP Files Used

| Archive | Contents | Used By |
|---------|----------|---------|
| `LOADING.ZIP` | `load00.tga` through `load19.tga` (20 images) | `InitializeRaceSession` |
| `LEGALS.ZIP` | `legal1.tga`, `legal2.tga` | `ShowLegalScreens` |

---

## 8. Loading Screen Timing and Interactivity

### Race Loading Screen
- **Not interactive**: Displayed once, no input polling during load
- **Duration**: Entirely determined by how long `InitializeRaceSession` takes to complete
  (typically several seconds, depending on hardware and the amount of asset loading)
- **No progress indication**: No progress bar, percentage, or animation
- **Single static frame**: One `DXDraw::Flip` call, then the surface stays visible

### Legal Screens
- **Semi-interactive**: 400ms grace period, then any keyboard key advances
- **Timeout**: Automatically advances after 5 seconds
- **No gamepad/mouse skip**: Only keyboard input is checked

### What Happens During Race Loading

After the loading image is displayed, `InitializeRaceSession` performs all of the following
before returning (approximate order):

1. Loading screen image display (immediate)
2. `ResetGameHeap()` -- clear game memory pool
3. Race slot state initialization (player/AI assignments)
4. Track/game mode configuration
5. `LoadTrackRuntimeData()` -- track geometry and checkpoint data
6. `BindTrackStripRuntimePointers()` -- track strip setup
7. `LoadTrackTextureSet()` -- track textures from level ZIP
8. `ParseModelsDat()` -- 3D model loading from level ZIP
9. Sky mesh loading from `STATIC.ZIP`
10. `LoadRaceVehicleAssets()` -- vehicle meshes and textures
11. `InitializeRaceVehicleRuntime()` -- vehicle physics init
12. `InitializeRaceActorRuntime()` -- all 6 actor slots
13. Vehicle shadow/wheel sprite initialization
14. Actor track position initialization
15. `LoadRaceAmbientSoundBuffers()` -- audio assets
16. Particle system, tire tracks, weather initialization
17. Force feedback controller configuration
18. `DXInput::SetConfiguration()` -- input binding
19. `DXSound::CDPlay()` -- start CD audio track
20. HUD, overlay, pause menu layout initialization
21. 3D render state, viewport, projection setup
22. `LoadRaceTexturePages()` -- upload textures to GPU
23. Return to game loop -> first race frame renders

---

## 9. Key Addresses

### Functions

| Address | Name | Purpose |
|---------|------|---------|
| 0x42CA00 | `DisplayLoadingScreenImage` | Core: decode TGA + blit to primary surface + flip |
| 0x42C8E0 | `ShowLegalScreens` | Display legal1.tga and legal2.tga with timeout/skip |
| 0x42AA10 | `InitializeRaceSession` | Master race init; shows random loading image at start |
| 0x442170 | `RunMainGameLoop` | Top-level state machine calling the above |
| 0x440790 | `ReadArchiveEntry` | Extract file from ZIP archive to memory |
| 0x4409B0 | `GetArchiveEntrySize` | Query uncompressed size of ZIP entry |
| 0x43FC80 | `ParseZipCentralDirectory` | Find entry in ZIP central directory |
| 0x4405B0 | `DecompressZipEntry` | DEFLATE decompression of ZIP entry |

### Strings

| Address | Value | Used In |
|---------|-------|---------|
| 0x4672A4 | `"LOADING.ZIP"` | Race loading screen extraction |
| 0x4672B0 | `"load%02d.tga"` | Loading image filename format |
| 0x46730C | `"Lock failed in DisplayLoadingPicture\n%s"` | Error message in DisplayLoadingScreenImage |
| 0x4672E8 | `"legal2.tga"` | Legal screen 2 filename |
| 0x4672F4 | `"legal1.tga"` | Legal screen 1 filename |
| 0x467300 | `"LEGALS.ZIP"` | Legal screens archive |

### Globals

| Address/Symbol | Purpose |
|----------------|---------|
| `dd_exref + 0x16A0` | Display bits-per-pixel (15, 16, or 32) |
| `dd_exref + 0x08` | Pointer to primary DirectDraw surface interface |
| `Image_exref` | Global TGA decode descriptor (`tagDXIMAGELINE`) |
| `g_appExref + 0x15C` | Windowed mode flag (determines flip behavior) |
| `g_gameState` | Top-level state machine variable |

### Constants

| Value | Meaning |
|-------|---------|
| 0x280 (640) | Hardcoded loading image width in pixels |
| 0x4B000 (307,200) | Hardcoded loading image total pixels (640 * 480) |
| 0x96000 (614,400) | Decoded pixel buffer size (640 * 480 * 2 bytes @ 16bpp) |
| 0x14 (20) | Number of loading images in LOADING.ZIP |
| 5000 | Legal screen timeout in milliseconds |
| 400 | Legal screen input grace period in milliseconds |
