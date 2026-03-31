# Debug & Diagnostics System Deep Dive

> Hidden developer tools, benchmark mode, FPS overlay, and system information reporting.
> Session: 2026-03-20

---

## Overview

Test Drive 5 ships with several **hidden debug and diagnostic subsystems** that were used during development for QA benchmarking, UI layout positioning, system profiling, and runtime performance monitoring. These tools span both the EXE and M2DX.dll:

| Subsystem | Location | Activation |
|---|---|---|
| Positioner Debug Tool | EXE 0x415030 | Frontend screen table index 1 (no runtime trigger) |
| Benchmark/TimeDemo | EXE 0x428d80 + M2DX | `TimeDemo` startup token in TD5.ini |
| FPS Overlay | M2DX 0x10008130 | `g_isFPSOverlayEnabled` flag (written by EXE) |
| Display Info Overlay | M2DX 0x10008300 | Created alongside FPS overlay |
| System Info Report | M2DX 0x1000e420 | Called during startup when `Log` token active |
| CPU Detection | M2DX 0x1000eab0 | Called by ExtractSystemInfo |
| Log Header | M2DX 0x10011320 | Emitted once during ParseStartupConfiguration |

---

## 1. ScreenPositionerDebugTool (EXE 0x415030)

### Purpose

A **hidden frontend screen** for visually positioning UI elements on a 7-column x 37-row character grid. It loads a reference image (`Front End\Positioner.tga` from `FrontEnd.zip`), lets the developer nudge character X/Y positions with directional input, and exports the final coordinates to `positioner.txt` as C source code arrays.

### Frontend Table Registration

The positioner is entry **index 1** in `g_frontendScreenFnTable` at `0x4655c4`:

```
Index 0: 0x4269D0  ScreenLocalizationInit (boot screen)
Index 1: 0x415030  ScreenPositionerDebugTool  <-- THIS
Index 2: 0x4275A0  ScreenLanguageSelect
Index 3: 0x427290  ...
```

### Activation

**There is no runtime trigger in the shipping EXE.** The function has **zero cross-references** -- no caller, no function pointer reference outside the table. During development, it could be activated by:
- Calling `SetFrontendScreen(1)` from a debug hook
- Patching the boot screen transition in `ScreenLocalizationInit` (which normally calls `SetFrontendScreen(5)`)

### State Machine (6 states)

| State | Action |
|---|---|
| 0 | Load `Front End\Positioner.tga` from `FrontEnd.zip`; clear backbuffer; draw horizontal guide lines at Y=0x10C and Y=0x114 |
| 1 | Present buffer, advance to state 2 |
| 2 | Initialize 37x7 character grid from `DAT_00466518` (default glyph indices); zero all Y-offsets; advance |
| 3 | **Grid selection mode**: D-pad/arrows move the selected row index (`_DAT_0049521c`); Up/Down = +/-1, PgUp/PgDn = +/-8. Press confirm (bit 0x40000 of `DAT_004951f8`) to enter state 4 |
| 4 | **Position edit mode**: Renders selected character as highlighted overlay; D-pad moves the X-offset (`DAT_00495260[idx*2]`) and Y-offset (`DAT_00495264[idx*2]`) for the selected row. Confirm advances to state 5 |
| 5 | **Export to file**: Opens `positioner.txt` in write mode; outputs two C arrays (`NAMEX[]` and `NAMEY[]`) with `// Created by SNK_Positioner` header |

### Character Grid Layout

- **37 rows x 7 columns** (0x25 * 7 = 259 character slots)
- Character map at `0x465960`: printable ASCII ` !"#$%&.()...` through `z{|}`
- Each grid cell maps to a glyph index from `DAT_00466518`
- Two coordinate arrays: X-offsets at `DAT_00495260` (stride 8 bytes) and Y-offsets at `DAT_00495264` (stride 8 bytes)

### Input Bitmask (`DAT_004951f8`)

| Bit | Action (State 3) | Action (State 4) |
|---|---|---|
| 0x001 | Row index -= 1 | X-offset -= 1 |
| 0x002 | Row index += 1 | X-offset += 1 |
| 0x200 | Row index += 8 | Y-offset -= 1 |
| 0x400 | Row index -= 8 | Y-offset += 1 |
| 0x40000 | Advance to next state | Advance to next state |

### Output Format (`positioner.txt`)

```c
// Created by SNK_Positioner

LONG	NAMEX[]=
{
		%2d, %2d, %2d, %2d, %2d, %2d, %2d,		// 'A','B','C','D','E','F','G'
		...
};

LONG	NAMEY[]=
{
		%2d, %2d, %2d, %2d, %2d, %2d, %2d,		// 'A','B','C','D','E','F','G'
		...
};
```

Each row emits 7 integer coordinate values followed by the 7 character labels as a comment.

---

## 2. Benchmark / TimeDemo System (EXE 0x428d80)

### Purpose

A complete **automated benchmarking pipeline** that records per-frame timing during a race, then generates a 640x480 TGA screenshot report with FPS graph, system specs, and hardware capability flags.

### Activation: The `TimeDemo` Startup Token

The benchmark is activated by the **`TimeDemo`** token (index 6 in M2DX's startup option table):

```ini
; In TD5.ini or command line:
TimeDemo
```

**Token processing chain:**
1. `ParseStartupConfiguration` (M2DX 0x10012490) reads TD5.ini or command line
2. `ApplyStartupOptionToken` (M2DX 0x10012780) matches "TimeDemo" at index 6
3. Sets `DAT_10061c40 = 1` (TimeDemo flag in M2DX)
4. During `DXDraw::Create` (M2DX 0x100062a0), if flag is set, shows `IDD_DIALOG1` benchmark configuration dialog
5. EXE side: `ScreenMainMenuAnd1PRaceFlow` (0x415490) replaces the "Two Player" main menu button with "Time Demo" (`SNK_TimeDemoButTxt`) when `iRam000600fc != 0`

### Main Menu Integration

When `TimeDemo` is active, the main menu button at index 2 changes behavior:

```c
case 2:  // Button index 2
    if (iRam000600fc != 0) {  // TimeDemo token active
        DAT_0049636c = 1;
        g_benchmarkModeActive = 1;   // 0x4aaf58
        InitializeRaceSeriesSchedule();
        InitializeFrontendDisplayModeState();
        return;
    }
    // Normal path: Two Player mode...
```

### Frame Recording

During the race loop (`RunRaceFrame` at 0x42b580), when `g_benchmarkModeActive` (0x4aaf58) is set:

```c
// At 0x42c20f in RunRaceFrame:
if (g_benchmarkModeActive != 0) {
    if (g_benchmarkFirstFrame == 0) {
        g_benchmarkFirstFrame = 1;  // skip first frame
    } else {
        RecordBenchmarkFrameRateSample(currentFrameTime);  // 0x428d40
    }
    g_raceTimeAccumulator = 3.0;  // force fixed timestep
}
```

`RecordBenchmarkFrameRateSample` (0x428d40) appends each frame's timing value to a linear buffer:

```c
void RecordBenchmarkFrameRateSample(int sample) {
    g_benchmarkSampleBuffer[g_benchmarkSampleCount] = sample;  // DAT_004a2cf4 + count*4
    g_benchmarkSampleCount++;                                    // DAT_004a2cf8
}
```

### Race End -> Report Generation

When the race finishes, the game state dispatcher (`RunMainGameLoop` at 0x442170) transitions:

```c
case 2:  // Race state
    result = RunRaceFrame();
    if (result != 0) {  // race ended
        // If benchmark active -> state 3, else -> state 1 (menu)
        DAT_004c3ce8 = (-(g_benchmarkModeActive != 0) & 2) + 1;
        // g_benchmarkModeActive=1 -> state 3; =0 -> state 1
    }
```

At the race-end boundary (0x42b864), `WriteBenchmarkResultsTgaReport` is called:

```c
if (g_benchmarkModeActive != 0) {
    WriteBenchmarkResultsTgaReport();  // 0x428d80
}
```

### WriteBenchmarkResultsTgaReport (0x428d80) -- TGA Report Generator

This function:
1. **Allocates a 640x480x8bpp canvas** (0x96000 = 614,400 bytes)
2. **Sets up a TGA header** with 256-color indexed palette (type 1)
3. **Defines a 4-color palette**: black (background), white (text), green+orange (graph)
4. **Draws the FPS graph**:
   - Y-axis: 0-150 FPS range (ticks every 10), mapped to pixel rows 0x1B7 (bottom) to 0 (top)
   - X-axis: 600 pixels divided proportionally across all samples
   - Each sample column is colored: index 2 (green) for FPS bars, index 3 for tick marks
   - Horizontal guide lines for min/max/average FPS
5. **Prints system information** via `DXDraw::PrintTGA()`:

| Field | X | Y | Source |
|---|---|---|---|
| "Test Drive 5" | 2 | 2 | Hardcoded |
| "DirectX %d APIs" | 160 | 2 | `FormatBenchmarkReportText` |
| "Build" | 300 | 2 | Hardcoded |
| Build number | 350 | 2 | `DXDecimal(10)` |
| FPS caption | 400 | 2 | `FPSCaption_exref` |
| Driver/version | 2 | 22 | Formatted from M2DX driver info |
| "Processor" | 42 | 62 | `info_exref + 0xF0` |
| "Physical Memory" | 42 | 82 | `info_exref + 0x1E0` |
| "Operating System" | 42 | 102 | `info_exref + 0x3C0` |
| "Screen Mode" | 42 | 122 | `info_exref + 0x4B0` |
| "Texture Memory" | 42 | 142 | `DXDecimal(textureMemory)` |
| "Minimum FPS:" | 42 | 42 | `min_fps / 3` |
| "Maximum FPS:" | 202 | 42 | `max_fps / 3` |
| "Average FPS:" | 362 | 42 | `avg_fps / 3` |

6. **Prints hardware flags** (conditionally, when feature is DISABLED):
   - "WBuffer" at (42, 242) -- if W-buffer not available
   - "Triple Buffer" at (142, 242) -- if no triple buffering
   - "Mip Mapping" at (242, 242) -- if mip-mapping disabled

7. **Logs summary** to the M2DX log: `"Min FPS: %d Max FPS: %d Avg FPS: %d"`

8. **Compresses and writes the TGA**:
   - Uses `DX::TGACompress` for RLE compression
   - Output filename from `FPSName_exref`, or default `"timedemo"` with `.tga` extension
   - Written via `fopen/fwrite/fclose`

### State 3: Benchmark Result Display

After writing the TGA, the game enters state 3 in `RunMainGameLoop`, which:
1. Loads the generated TGA file from disk
2. Decodes it via `DX::ImageProTGA`
3. Blits the decoded 640x480 image to the backbuffer each frame
4. Waits for any keyboard input (`DXInputGetKBStick(0)`)
5. On keypress, frees resources and returns to state 1 (main menu)

---

## 3. FPS Overlay System (M2DX.dll)

### Architecture

The FPS overlay uses **dedicated DirectDraw surfaces** rendered with GDI text, blitted over the game's render target. Two independent overlays exist:

| Overlay | Surface Global | Content |
|---|---|---|
| Frame Rate | `g_pFrameRateOverlaySurface` | `"XX.XX fps XXXX tps XXXX mpps"` |
| Display Info | `g_pDisplayInfoOverlaySurface` | `"WIDTHxHEIGHTxBPP MONO/STEREO"` |

### CreateDiagnosticOverlaySurfaces (0x10008470)

Called from `InitializeDriverFeatureStates` (0x10003440) whenever the viewport is (re)created.

**Flow:**
1. Release existing overlay surfaces if any
2. Create an Arial font via `CreateFontA`:
   - Size 24px for width <= 600, or 12px for width > 600
   - Weight 400, charset 0, pitch VARIABLE
3. Measure text extents for template strings:
   - FPS template: `"000 00 fps 00000000 00 tps 0000 "` (40 chars)
   - Info template: `"000x000x00  MONO  0000"` (23 chars)
4. Create two offscreen plain DirectDraw surfaces (DDSCAPS_OFFSCREENPLAIN, optionally SYSTEMMEMORY if `DAT_10061c18` is set)
5. Set color key on FPS surface (transparent black)
6. Call `RestoreFrameRateOverlay()` and `RestoreDisplayInfoOverlay()` to paint initial text

### RestoreFrameRateOverlay (0x10008130)

Repaints the FPS overlay surface text. Called:
- From `CalculateFrameRate` every 10 frames (when overlay enabled)
- From `RestoreLostOverlaySurfaces` after surface loss recovery
- From `CreateDiagnosticOverlaySurfaces` during initialization

**Text assembly:**
```c
// FPS value (from _DAT_1003270c, float)
if (fps > 0.0)
    sprintf(fps_str, "%d.%02d fps", (int)fps/100, (int)fps%100);

// Triangles per second (from g_currentFPSInt)
if (tps > 0)
    sprintf(tps_str, "%ld tps", tps);

// Million pixels per second (from DAT_10032714)
if (mpps > 0)
    sprintf(mpps_str, "%ld mpps", mpps);

sprintf(output, "%s %s %s", fps_str, tps_str, mpps_str);
```

**GDI rendering:**
- GetDC on overlay surface
- Select Arial font, yellow text (0x00FFFF), black background, opaque mode
- `ExtTextOutA` with ETO_OPAQUE flag
- ReleaseDC

### RestoreDisplayInfoOverlay (0x10008300)

Repaints the display info overlay. Shows current resolution and mono/stereo mode:

```c
if (fullscreen) {
    width  = g_cachedActiveDisplayModeRow;
    height = g_cachedActiveDisplayModeHeight;
    bpp    = g_cachedActiveDisplayModeBitDepth;
} else {
    width  = g_activeRenderWidth;
    height = g_activeRenderHeight;
    bpp    = g_windowedRenderBitDepth;
}
// DAT_10028a84 == 1 -> "STEREO", else "MONO"
sprintf(output, "%dx%dx%d  %s ", width, height, bpp, mono_or_stereo);
```

### CalculateFrameRate (DXDraw, 0x100069e0)

Called from `BeginScene` every frame. Accumulates frame count; every 10 frames:

1. Computes elapsed ms since last sample via `timeGetTime()`
2. Gets D3D stats via `DXD3D::GetStats` (triangle count for TPS)
3. `FPS = accumulated_frames / (elapsed_ms * 0.001)`
4. Converts to integer TPS and MPPS
5. If `g_isFPSOverlayEnabled` (0x10032728) is nonzero:
   - Calls `RestoreFrameRateOverlay()` to repaint
   - Sets `g_fpsOverlayDirty = 1` to trigger full-buffer Flip

### Overlay Blit Path

The overlay blit is managed through the dirty-rect Flip system:
- `g_fpsOverlayDirty` (0x10061c2c) signals that the overlay surface changed
- `EndScene` calls `DXDraw::Flip(g_fpsOverlayDirty != 0)`
- When `param_1 == 1` (dirty), Flip forces a full-rectangle blit instead of dirty-rect merge
- The overlay surfaces are blitted as hardware overlays or composited by the DirectDraw surface chain

### Enabling the FPS Overlay

`g_isFPSOverlayEnabled` at address `0x10032728` in M2DX.dll is only **read** by M2DX -- never written. The EXE must set this flag. In the shipping game, the EXE's widescreen/debug patches use `Shift+F12` to toggle it. The flag is exported from M2DX as part of the DXDraw data segment, accessible by the EXE through the `dd_exref` import bridge.

### Surface Loss Recovery

`RestoreLostOverlaySurfaces` (called from `BeginScene`) checks both overlay surfaces for `DDERR_SURFACELOST` (-0x7789fe3e). On loss:
1. Calls `IDirectDrawSurface::Restore()` on the lost surface
2. Resets FPS counters to zero
3. Calls the appropriate `RestoreXxxOverlay()` to repaint

---

## 4. System Information Extraction (M2DX.dll)

### ExtractSystemInfo (0x1000e420)

Comprehensive hardware profiling function called during startup by `Environment` (0x100124c9). Populates the `info_exref` report buffer with formatted diagnostic strings.

**Data collected:**

| Report Section | Offset in `info_exref` | API Used | Content |
|---|---|---|---|
| System Name | +0x000 | `GetComputerNameA` | Machine hostname (fallback: empty) |
| Processor | +0x0F0 | CPUID + `GetSystemInfo` | Model string + clock speed |
| Physical Memory | +0x1E0 | `GlobalMemoryStatus` | Total/available RAM |
| Page File | +0x2D0 | `GlobalMemoryStatus` | Page file info |
| Operating System | +0x3C0 | `GetVersionExA` | OS name + version + build |
| Screen Mode | +0x4B0 | `IDirectDraw::GetDisplayMode` | Resolution + bit depth |

**CPU Detection Flow:**

1. `GetSystemInfo` for processor type (386/486/Pentium)
2. Tests EFLAGS bit 21 (CPUID support) by toggling and reading back
3. If CPUID available:
   - `CPUID(0)` -> vendor string (stored in `g_cpuVendorString`)
   - `CPUID(1)` -> family/model/stepping + feature flags
4. Feature flag extraction:
   - Bit 23: MMX (`DAT_10061ac0`)
   - Bit 4: RDTSC (`DAT_10061ac8`)
   - Bit 25: SSE (`DAT_10061ac4`)
5. If RDTSC available, measures clock speed:
   - Reads TSC + `QueryPerformanceCounter` before/after a ~1M iteration busy-wait
   - Computes MHz from TSC delta / QPC delta

**Multi-processor support:** When `GetSystemInfo` reports > 1 processor, the report uses format `"%d x %s   %d MHz"` (e.g., "2 x Pentium III 450 MHz").

### ExtractScreenInfo (0x1000ea00)

Simple function called by `GetMaxTextures` (0x10001d43) and by the benchmark report generator:

```c
void DX::ExtractScreenInfo(void) {
    DDSURFACEDESC ddsd;
    hr = IDirectDraw::GetDisplayMode(dd, &ddsd);
    if (SUCCEEDED(hr))
        AppendFormattedReportLine(info+0x4B0, "%dx%dx%d   %d", w, h, bpp, refresh);
    else
        AppendFormattedReportLine(info+0x4B0, "%dx%dx%d", w, h, bpp);
}
```

### GetProcessorModelString (0x1000eab0)

Returns a human-readable CPU model name based on CPUID vendor + family + model:

```c
char* GetProcessorModelString(int processorType) {
    if (processorType == 0x182) return "Intel 386";
    if (processorType == 0x1E6) return "Intel 486";

    // Extract family/model from cached CPUID(1) result
    uint model  = (DAT_10061ab8 >> 4) & 0xF;
    uint type   = (DAT_10061ab8 >> 12) & 0x3;
    uint family = (DAT_10061ab8 >> 8) & 0xF;

    if (vendor == "GenuineIntel" && type == 0) {
        if (family == 5) {
            if (model == 1 || model == 2) return "Pentium";
            if (model == 4)               return "Pentium MMX";
        }
        if (family == 6) {
            if (model < 2)  return "Pentium Pro";
            if (model < 6)  return "Pentium II";
            if (model == 6) return "Celeron A";
            if (model == 7) return "Pentium III";
        }
    }
    if (vendor == "AuthenticAMD" && type == 0 && family == 5) {
        if (model < 4) return "AMD K5";
        if (model < 8) return "AMD K6";
        if (model == 8) return "AMD K6-2";
        if (model == 9) return "AMD K6-3";
    }
    return g_cpuVendorString;  // fallback: raw CPUID vendor
}
```

**Supported CPUs (1999 era):**

| Vendor | Family | Model(s) | Label |
|---|---|---|---|
| Intel | 5 | 1, 2 | Pentium |
| Intel | 5 | 4 | Pentium MMX |
| Intel | 6 | 0, 1 | Pentium Pro |
| Intel | 6 | 3, 4, 5 | Pentium II |
| Intel | 6 | 6 | Celeron A |
| Intel | 6 | 7 | Pentium III |
| AMD | 5 | 0-3 | AMD K5 |
| AMD | 5 | 4-7 | AMD K6 |
| AMD | 5 | 8 | AMD K6-2 |
| AMD | 5 | 9 | AMD K6-3 |

When SSE is detected (`DAT_10061ac4`), the suffix `" SSE"` (from `DAT_10023fdc`) is appended to the model string.

---

## 5. Log Report System (M2DX.dll)

### LogSystemInfoReportHeader (0x10011320)

Emits a fixed-format header block to the M2DX log file. Called **once** during `ParseStartupConfiguration` after the `Log` startup token enables logging.

```c
void LogSystemInfoReportHeader(void) {
    LogReport("Test Drive 5");
    LogReport("DirectX APIs %d");
    LogReport("Build %d");
    LogReport("Operating System %s");
    LogReport("Processor %s");
    LogReport("Physical Memory %s");
    LogReport("Page File %s");
    LogReport("System Name %s");
}
```

Each `LogReport` call formats the string with data from the `info_exref` report buffer (populated by `ExtractSystemInfo`) and writes to the log output.

### Log Token

Activated by the `Log` startup token (index 5 in the option table):

```ini
; In TD5.ini:
Log
```

Sets `DXWin::bLogPrefered = 1`, which:
1. Enables `LogReport()` output (otherwise calls are no-ops)
2. Triggers `LogSystemInfoReportHeader()` after all tokens are parsed
3. Throughout the session, `LogReport()` calls from rendering, texture loading, etc. are captured

---

## 6. Startup Token Table (M2DX.dll)

The complete 26-token table at `0x10027BF8` (pointers into `0x10027C60` string block):

| Index | Token | Flag/Effect |
|---|---|---|
| 0 | `//` | Comment (ignored) |
| 1 | `rem` | Comment (ignored) |
| 2 | `;` | Comment (ignored) |
| 3 | `\` | Comment (ignored) |
| 4 | `#` | Comment (ignored) |
| 5 | `Log` | `bLogPrefered = 1` (enable logging) |
| 6 | `TimeDemo` | `DAT_10061c40 = 1` (benchmark mode) |
| 7 | `FullScreen` | `DAT_10061c1c = 1` (force fullscreen) |
| 8 | `Window` | `DAT_10061c1c = 0` (force windowed) |
| 9 | `DoubleBuffer` | `g_tripleBufferState = 4` (disable triple) |
| 10 | `NoTripleBuffer` | `g_tripleBufferState = 4` (same as above) |
| 11 | `NoWBuffer` | `g_worldTransformState = 4` |
| 12 | `Primary` | `g_adapterSelectionOverride = 1` |
| 13 | `DeviceSelect` | `g_adapterSelectionOverride = -1` |
| 14 | `NoMovie` | `DAT_10061c0c = 4` |
| 15 | `FixTransparent` | `DAT_100294b4 = 1` |
| 16 | `FixMovie` | `DAT_10061c44 = 1` |
| 17 | `NoAGP` | `DAT_1003266c = 4` |
| 18 | `NoPrimaryTest` | `g_skipPrimaryDriverTestState = 1` |
| 19 | `NoMultiMon` | `g_ddrawEnumerateExState = 4` |
| 20 | `NoMIP` | `g_mipFilterState = 4` |
| 21 | `NoLODBias` | `g_lodBiasState = 4` |
| 22 | `NoZBias` | `g_zBiasState = 4` |
| 23 | `MatchBitDepth` | `g_matchBitDepthState = 1` |
| 24 | `WeakForceFeedback` | `DXInput::FFGainScale = 0.5` |
| 25 | `StrongForceFeedback` | `DXInput::FFGainScale = 1.0` |

Tokens are parsed from TD5.ini (or a command file specified on the command line). Comment prefixes (0-4) allow INI-style annotations.

---

## 7. Data Flow Diagram

```
                    TD5.ini
                      |
                      v
         ParseStartupConfiguration (M2DX)
          |                        |
          v                        v
    "Log" token              "TimeDemo" token
          |                        |
     bLogPrefered=1          DAT_10061c40=1
          |                        |
          v                        v
  LogSystemInfoReportHeader  DXDraw::Create shows
  ExtractSystemInfo            IDD_DIALOG1 dialog
  ExtractScreenInfo              |
          |                      v
          v                Main Menu shows
   info_exref buffer       "Time Demo" button
   (Processor, RAM,              |
    OS, Screen mode)             v
          |              g_benchmarkModeActive=1
          |              InitializeRaceSeriesSchedule()
          |                      |
          v                      v
  Used by benchmark         Race loop records
  TGA report (0x428d80)    frame samples via
                           RecordBenchmarkFrameRateSample
                                 |
                                 v
                        Race ends -> State 3
                        WriteBenchmarkResultsTgaReport
                                 |
                                 v
                         timedemo.tga written
                         Result displayed on screen
                         Any key -> back to menu
```

---

## 8. Key Globals Summary

### EXE (TD5_d3d.exe)

| Address | Type | Name | Purpose |
|---|---|---|---|
| `0x4AAF58` | int | `g_benchmarkModeActive` | Benchmark mode flag |
| `0x4A2CF4` | int* | `g_benchmarkSampleBuffer` | Frame timing sample array |
| `0x4A2CF8` | int | `g_benchmarkSampleCount` | Number of recorded samples |
| `0x4A2CFC` | char[256] | (scratch) | `FormatBenchmarkReportText` output buffer |
| `0x4AAF40` | int | `g_benchmarkFirstFrame` | Skip first frame flag |
| `0x4951F8` | uint | `DAT_004951f8` | Positioner input bitmask |
| `0x49521C` | int | `_DAT_0049521c` | Positioner selected row |
| `0x495260` | int[37*2] | X-offset array | Positioner character X positions |
| `0x495264` | int[37*2] | Y-offset array | Positioner character Y positions |
| `0x4655C4` | ptr[28+] | `g_frontendScreenFnTable` | Screen function pointer table |

### M2DX.dll

| Address | Type | Name | Purpose |
|---|---|---|---|
| `0x10032728` | int | `g_isFPSOverlayEnabled` | FPS overlay enable flag (written by EXE) |
| `0x10061C2C` | int | `g_fpsOverlayDirty` | Triggers full-buffer Flip |
| `0x10061C40` | int | TimeDemo flag | Set by `TimeDemo` startup token |
| `0x1003270C` | float | `_DAT_1003270c` | Current FPS (float) |
| `0x10032710` | int | `g_currentFPSInt` | Current TPS (triangles/sec) |
| `0x10032714` | int | `DAT_10032714` | Current MPPS (megapixels/sec) |
| `0x10061AB8` | uint | `DAT_10061ab8` | Cached CPUID(1) EAX (family/model/stepping) |
| `0x10061ABC` | uint | `_DAT_10061abc` | Cached CPUID(1) EDX (feature flags) |
| `0x10061AC0` | int | `_DAT_10061ac0` | MMX detected flag |
| `0x10061AC4` | int | `DAT_10061ac4` | SSE detected flag |
| `0x10061AC8` | int | `DAT_10061ac8` | RDTSC detected flag |
| `0x10061AEC` | HFONT | `DAT_10061aec` | Arial font handle for overlays |
