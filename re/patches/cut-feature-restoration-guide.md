# Test Drive 5 -- Cut Feature Restoration Guide

> Binary: `TD5_d3d.exe` (32-bit x86 PE, MSVC)
> Base address: 0x00400000 | .text starts at file offset 0x1000
> File offset formula: `file_offset = VA - 0x400000` (verified: .text VA 0x401000, file 0x1000)
> All patches are for the unmodified retail binary unless noted otherwise.

---

## 1. SNOW RENDERING RESTORATION

### Background

Weather type is stored at `DAT_004c3de8`, read from `gTrackEnvironmentConfig + 0x28`:
- 0 = Rain (fully functional: init + render)
- 1 = Snow (init runs but render is skipped)

The snow particle init (`InitializeWeatherOverlayParticles`, VA 0x446240) allocates
buffers and loads the `SNOWDROP` texture, but SKIPS the `BuildSpriteQuadTemplate`
call that rain gets. The render function (`RenderAmbientParticleStreaks`, VA 0x446560)
has a JNZ at 0x4465F4 that exits immediately when weather != 0, making snow invisible.

### Patch 1A: Enable Snow Rendering (REQUIRED)

**What**: NOP the JNZ that skips the entire render function when weather type != 0.

**Analysis**: The render path rebuilds the sprite quad template every frame via
`BuildSpriteQuadTemplate` before `QueueTranslucentPrimitiveBatch`. This means the
missing init-time call for snow is NOT a blocker -- the render path handles it.

| Field | Value |
|---|---|
| VA | 0x004465F4 |
| File offset | 0x0465F4 |
| Original bytes | `0F 85 67 04 00 00` (JNZ +0x467 to exit) |
| Patched bytes | `90 90 90 90 90 90` (6x NOP) |
| Size | 6 bytes |

### Patch 1B: Snow Init BuildSpriteQuadTemplate (OPTIONAL, defense-in-depth)

The render path rebuilds quads each frame, so this is not strictly required.
However, if any code reads the quad template before the first render tick, having
it initialized prevents garbage data.

**Strategy**: Hijack the snow init loop's JNZ at VA 0x44648D to a code cave that
calls `BuildSpriteQuadTemplate` before looping.

In the snow init loop, after storing the Z coordinate:
```
00446487: FSTP float ptr [ESI - 0xC0]  ; store particle Z (ESI+0xffffff40 = ESI-0xC0)
0044648d: JNZ 0x0044642a              ; loop if --EDI != 0
```

We need to insert a `BuildSpriteQuadTemplate` call. The rain path uses:
```
LEA EAX, [ESP + local_offset]   ; pointer to quad template setup struct
PUSH EAX
CALL BuildSpriteQuadTemplate    ; VA 0x432bd0
ADD ESP, 4
```

But the snow loop doesn't set up the struct that `BuildSpriteQuadTemplate` expects.
The struct includes UV coordinates, visibility flags, and bounds -- all of which
the render path computes per-frame anyway.

**Recommendation**: Apply only Patch 1A. The render path fully initializes each
particle's quad before submitting it. Patch 1B would require ~40 bytes of cave
code to replicate the rain struct setup, for no practical benefit.

### Risk Assessment

**MEDIUM RISK**: Snow particles will be rendered using rain's streak-based rendering
path. This means:
- Snow will appear as short diagonal streaks (velocity-based) instead of floating
  flakes
- The visual effect looks like "heavy sleet" rather than gentle snowfall
- The snow init uses wider positional spread (+/-8000 vs +/-4000 for rain) and
  positive-only Z (200..8119 vs random Z for rain), which partially compensates
- Motion delta calculations apply to snow the same way as rain (wind+velocity streaks)

**The original developers likely cut snow precisely because the streak renderer
looked wrong for snow. A proper snowflake renderer would need billboard-style
rendering (point sprites or screen-aligned quads) instead of velocity streaks.**

### Testing Instructions

1. Apply Patch 1A (NOP the JNZ)
2. Load a track with weather type 1 (snow environment)
   - If no vanilla track has weather==1, force it: at the start of
     `InitializeWeatherOverlayParticles`, the value is read from
     `gTrackEnvironmentConfig + 0x28`. You can set `DAT_004c3de8` to 1 via
     a debugger breakpoint at VA 0x446252 (the MOV that writes it)
3. Enter a race on that track
4. Observe: streak-style particles should appear, falling in the general
   direction of vehicle motion
5. Verify no crashes -- the SNOWDROP texture must exist in the track archive

---

## 2. VEHICLE DAMAGE WOBBLE RESTORATION (_rand)

### Background

`GetDamageRulesStub` (VA 0x42C8D0) is a 3-byte stub: `XOR EAX,EAX; RET`
(always returns 0). It is called from 4 functions, 22 total call sites:

- `ApplyVehicleCollisionImpulse` (0x4079C0) -- 12 calls: random collision wobble
  for player cars (rotation perturbation) and traffic actors (body deformation)
- `ApplyRandomWheelJitterHighSpeed` (0x43D830) -- 4 calls: speed-proportional
  wheel shimmy on each wheel (front L/R, rear L/R)
- `ApplyRandomWheelJitterLowSpeed` (0x43D910) -- 4 calls: half-intensity version
- `ApplyRandomWheelJitterSynchronized` (0x43D9F0) -- 2 calls: paired axle wobble

When the stub returns 0, all modulo operations produce 0, and all `(rand % N) - N/2`
expressions evaluate to `-N/2` (fixed negative bias instead of random). This makes
collision wobble deterministic and lopsided, and wheel jitter nonexistent.

The original function was almost certainly `_rand()` (VA 0x448157, CRT random
number generator). The callers push a parameter (actor slot index) before calling,
but `_rand` ignores parameters (0-arg cdecl). The pushed value is cleaned up by
the caller's `ADD ESP` -- safe for both the stub and `_rand`.

### The Fix: Redirect GetDamageRulesStub to _rand

| Field | Value |
|---|---|
| VA | 0x0042C8D0 |
| File offset | 0x02C8D0 |
| Original bytes | `33 C0 C3 90 90` (XOR EAX,EAX; RET; NOP; NOP) |
| Patched bytes | `E9 82 B8 01 00` (JMP _rand) |
| Size | 5 bytes |

**JMP offset calculation**:
```
target = 0x448157 (_rand)
source = 0x42C8D0 (GetDamageRulesStub)
rel32  = target - (source + 5) = 0x448157 - 0x42C8D5 = 0x1B882
Encoding: E9 82 B8 01 00
```

This is a single 5-byte patch. The 2 extra bytes overwrite NOPs (alignment padding
after the original RET), which is safe.

### What Changes

With the patch applied:
- **Collision wobble**: After high-energy vehicle collisions (impact > 90000),
  player cars get random angular perturbation proportional to impact force.
  Rotation axes (yaw, pitch, roll) each get `rand() % maxDelta - maxDelta/2`.
  Traffic actors get random body-frame rotation matrix perturbation.
- **Wheel shimmy**: At all speeds, each wheel gets independent random displacement
  of `(rand() & 7) - 4` scaled by speed. This creates visible wheel vibration
  on damaged vehicles.
- **Paired axle wobble**: Front and rear axle pairs get synchronized jitter with
  `(rand() & 0xF) - 8` scaling, creating a rocking motion.

### Risk Assessment

**LOW RISK**: This is the safest patch in the guide. `_rand()` is used extensively
throughout the codebase (weather particles, AI, etc.) and is a drop-in replacement.
The stub was clearly a deliberate kill-switch -- the surrounding code is fully intact
and expects random values.

The only concern is reproducibility: with random damage wobble, two playthroughs
of the same crash will look slightly different. This is the intended behavior.

### Testing Instructions

1. Apply the 5-byte patch at file offset 0x02C8D0
2. Start any race with traffic or AI opponents
3. Collide with another vehicle at high speed
4. Observe: the car should visibly wobble/rotate on impact, and wheels should
   shimmy noticeably at speed after taking damage
5. Compare with unpatched: collisions feel "flat" and impacts have no visible
   angular perturbation

### Individual Call Sites Reference

For documentation purposes, here are all 22 call sites. Each is a 5-byte
`CALL 0x0042C8D0` instruction. No patches needed at these sites -- the single
JMP at the stub is sufficient.

**ApplyVehicleCollisionImpulse (0x4079C0) -- 12 calls:**

| # | VA | File Offset | Original Bytes |
|---|---|---|---|
| 1 | 0x4082EA | 0x082EA | E8 E1 45 02 00 |
| 2 | 0x40830A | 0x0830A | E8 C1 45 02 00 |
| 3 | 0x40832A | 0x0832A | E8 A1 45 02 00 |
| 4 | 0x408382 | 0x08382 | E8 49 45 02 00 |
| 5 | 0x40839A | 0x0839A | E8 31 45 02 00 |
| 6 | 0x4083B2 | 0x083B2 | E8 19 45 02 00 |
| 7 | 0x408442 | 0x08442 | E8 89 44 02 00 |
| 8 | 0x40845B | 0x0845B | E8 70 44 02 00 |
| 9 | 0x408474 | 0x08474 | E8 57 44 02 00 |
| 10 | 0x4084C4 | 0x084C4 | E8 07 44 02 00 |
| 11 | 0x4084DC | 0x084DC | E8 EF 43 02 00 |
| 12 | 0x4084F4 | 0x084F4 | E8 D7 43 02 00 |

**ApplyRandomWheelJitterHighSpeed (0x43D830) -- 4 calls:**

| # | VA | File Offset | Original Bytes |
|---|---|---|---|
| 13 | 0x43D84F | 0x3D84F | E8 7C F0 FE FF |
| 14 | 0x43D87D | 0x3D87D | E8 4E F0 FE FF |
| 15 | 0x43D8AB | 0x3D8AB | E8 20 F0 FE FF |
| 16 | 0x43D8D9 | 0x3D8D9 | E8 F2 EF FE FF |

**ApplyRandomWheelJitterLowSpeed (0x43D910) -- 4 calls:**

| # | VA | File Offset | Original Bytes |
|---|---|---|---|
| 17 | 0x43D92F | 0x3D92F | E8 9C EF FE FF |
| 18 | 0x43D95D | 0x3D95D | E8 6E EF FE FF |
| 19 | 0x43D98B | 0x3D98B | E8 40 EF FE FF |
| 20 | 0x43D9B9 | 0x3D9B9 | E8 12 EF FE FF |

**ApplyRandomWheelJitterSynchronized (0x43D9F0) -- 2 calls:**

| # | VA | File Offset | Original Bytes |
|---|---|---|---|
| 21 | 0x43DA0F | 0x3DA0F | E8 BC EE FE FF |
| 22 | 0x43DA45 | 0x3DA45 | E8 86 EE FE FF |

---

## 3. BENCHMARK MODE RESTORATION

### Background

The benchmark/TimeDemo system is fully intact in the binary:

| Function | VA | Purpose |
|---|---|---|
| InitializeBenchmarkFrameRateCapture | 0x428D20 | Allocates 1M float sample buffer |
| RecordBenchmarkFrameRateSample | 0x428D40 | Stores per-frame FPS value |
| FormatBenchmarkReportText | 0x428D60 | sprintf wrapper for report strings |
| WriteBenchmarkResultsTgaReport | 0x428D80 | Generates 640x480 TGA with system info + FPS graph |

**Activation**: The flag `g_benchmarkModeActive` at VA 0x4AAF58 controls everything.
When set to 1:
- `InitializeRaceSession` (0x42AA10): forces track 0x17 (pool index 23), disables
  traffic, disables player input, uses predefined car assignments, calls
  `InitializeBenchmarkFrameRateCapture`
- `RunRaceFrame` (0x42B580): records per-frame samples, writes TGA on race end
- `RunMainGameLoop` (0x442170): transitions to state 3 (TGA viewer) instead of
  state 1 (menu) when race ends. State 3 loads and displays the FPS report TGA,
  waiting for any key to return to menu

**Original activation**: The `-TimeDemo` command-line flag parsed by M2DX.DLL
(string at M2DX VA 0x10027D54). The flag is written at exe VA 0x415BFC:
`MOV [0x4AAF58], EAX` where EAX=1.

The dead symbol `SNK_TimeDemoButTxt` in LANGUAGE.DLL confirms this was once a
menu button. The menu button was removed but all backend code remains.

### Option A: Command-Line Activation (Simplest, NO BINARY PATCH)

The `-TimeDemo` flag may already work if M2DX.DLL's command-line parser is intact.

**Testing**: Run `TD5_d3d.exe -TimeDemo` from command line.

If M2DX.DLL's parser is intact, the flag flows through the config system to
`g_benchmarkModeActive`. The game should auto-start a benchmark race on track 23,
record FPS, and display the result TGA.

### Option B: Force Flag via Code Patch

**IMPORTANT**: `g_benchmarkModeActive` (VA 0x4AAF58) is in the BSS region of
the .data section. The .data section's VirtualSize is 0x6DD2C but its raw
(on-disk) size is only 0x20000. The flag is at offset 0x47F58 within the
section -- beyond the raw data boundary. It is zero-initialized by the OS
loader and does NOT exist in the PE file on disk. A data-section patch is
impossible.

Instead, we hijack the code that first reads the flag. At the very start of
`InitializeRaceSession` (VA 0x42AA10), the first instruction loads the flag
into EAX for a subsequent CMP. We replace this with a CALL to a code cave
that both writes 1 to the flag in memory AND loads 1 into EAX (so the
following CMP sees the correct value). Multiple other functions also read
`g_benchmarkModeActive` from memory, so the flag must actually be set --
simply NOPing the branch is insufficient.

**Code cave (16 bytes at VA 0x45C390):**
```
45C390: C7 05 58 AF 4A 00 01 00 00 00   MOV DWORD PTR [0x4AAF58], 1   ; set flag in memory
45C39A: B8 01 00 00 00                   MOV EAX, 1                     ; mirror into EAX
45C39F: C3                               RET
```

**Hijack site (5 bytes at VA 0x42AA10):**
```
Original: A1 58 AF 4A 00    MOV EAX, [0x4AAF58]       ; load flag
Patched:  E8 7B 19 03 00    CALL 0x45C390              ; cave: set + load flag
```

Offset calculation: `0x45C390 - (0x42AA10 + 5) = 0x3197B`

| Field | Value |
|---|---|
| **Hijack site** | |
| VA | 0x0042AA10 |
| File offset | 0x02AA10 |
| Original bytes | `A1 58 AF 4A 00` |
| Patched bytes | `E8 7B 19 03 00` |
| Size | 5 bytes |
| **Code cave** | |
| VA | 0x0045C390 |
| File offset | 0x05C390 |
| Original bytes | `00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00` |
| Patched bytes | `C7 05 58 AF 4A 00 01 00 00 00 B8 01 00 00 00 C3` |
| Size | 16 bytes |

**WARNING**: This makes EVERY race a benchmark race. The game will auto-drive
with no player input, record FPS, and show a report. This is a developer tool,
not a gameplay feature. Use Option A (command-line) for on-demand activation.
To revert, restore the original 5 bytes at the hijack site.

### Option C: Add Menu Button (HARD, not recommended)

Re-adding the "Time Demo" button to the main menu requires:
1. Adding an entry to the menu button table in `ScreenMainMenuAnd1PRaceFlow`
2. Hooking the button press to set `g_benchmarkModeActive = 1`
3. Triggering race start (set `DAT_00495248 = 1`)
4. Adding the localized string from LANGUAGE.DLL

This requires significant code cave space and menu layout knowledge. The
command-line flag is the practical approach.

### Risk Assessment

**LOW RISK** for Option A (command-line): all backend code is fully intact and
tested by the developers. The flag was just hidden, not removed.

**MEDIUM RISK** for Option B (code cave): makes the game unplayable for normal
use (every race becomes a benchmark). Only useful for dedicated benchmarking.
The code cave at 0x45C390 is in confirmed free space within .text section bounds.

### Testing Instructions

1. Try Option A first: `TD5_d3d.exe -TimeDemo`
2. If that doesn't work, apply the Option B code cave patch (2 sites)
3. Expected behavior: game auto-starts race on a specific track, AI drives all
   cars, FPS counter runs, race ends after track completion, 640x480 TGA appears
   showing system info (CPU, RAM, OS, screen mode, texture memory) and a min/max/avg
   FPS graph
4. Press any key to return to menu
5. Check for output file: the TGA is written via `FPSName_exref`
   (default filename likely "FPS.tga" or configurable)

---

## 4. HIDDEN DEBUG FEATURES

### 4A. NoOpHookStub (VA 0x418450) -- Debug Instrumentation Hook

**What it is**: A single-byte `RET` (0xC3) at VA 0x418450, followed by 7 NOP
alignment bytes. Called from 9 distinct functions:

| Caller | VA | Context |
|---|---|---|
| InitializeFrontendResourcesAndState | 0x414740 | Frontend startup |
| RunFrontendDisplayLoop | 0x414B50 | Per-frame frontend update |
| RunRaceFrame | 0x42B580 | Per-frame race update |
| RenderRaceActorForView | 0x40C120 | Per-actor render |
| LoadEnvironmentTexturePages | 0x42F990 | Texture loading |
| RunFrontendConnectionBrowser | 0x418D50 | Network lobby (5 sites) |
| RenderFrontendSessionBrowser | 0x419B30 | Network session list |
| RenderFrontendCreateSessionNameInput | 0x41A530 | Network name input |
| RenderFrontendLobbyChatInput | 0x41A670 | Network chat |

**What it was**: This was a debug instrumentation/callback hook. In a debug build,
it likely performed one or more of:
- Frame-by-frame state logging (called from both frontend and race loops)
- Texture page validation (called during texture loading)
- Developer overlay rendering (called per-frame in frontend and race)
- Network debugging (called from 5 networking screens)

The consistent placement at key lifecycle points (init, per-frame, per-actor,
network events) suggests a generic debug callback system where a single function
pointer could be swapped to enable different debug modes.

**Restoration potential**: LOW. The original debug function body was completely
stripped. Only the call sites remain. You could use this as a hook point for
custom instrumentation by replacing the `RET` with a `JMP` to your own code.

| Field | Value |
|---|---|
| VA | 0x00418450 |
| File offset | 0x018450 |
| Original bytes | `C3` (RET) |
| Available space | 8 bytes (C3 + 7x 90 NOPs) |

**Example: FPS counter hook**: Replace the `RET` with a JMP to a code cave that
calls `timeGetTime()` and increments a counter. Since `RunRaceFrame` calls this
every frame, you get a free hook point for any per-frame instrumentation.

### 4B. Positioner Tool (Developer Layout Tool Remnant)

Three strings reference an internal layout tool:

| VA | String |
|---|---|
| 0x465910 | `// Created by SNK_Positioner\n\n` |
| 0x465930 | `positioner.txt` |
| 0x465944 | `Front End\Positioner.tga` |

The `Positioner.tga` asset is loaded at VA 0x415068 inside
`ScreenMainMenuAnd1PRaceFlow` (Entry 5). This was a developer tool for
visually positioning menu elements:

1. `Positioner.tga` was a grid/ruler overlay displayed on top of the frontend
2. The developer would use mouse/keyboard to place UI elements
3. Coordinates were exported to `positioner.txt` with the header comment
   `// Created by SNK_Positioner`

The tool's functional code was stripped -- only the asset load and output strings
remain. The TGA is still loaded as a frontend resource but never displayed.

**Restoration potential**: VERY LOW. The tool's logic is completely gone. The
strings and asset load are just leftovers. The `Positioner.tga` overlay could
theoretically be displayed by patching the rendering code, but without the
positioning logic, it would just be a static grid image.

### 4C. Attract Mode / Auto-Demo (FUNCTIONAL)

`RunAttractModeDemoScreen` (VA 0x4275A0) implements arcade-style attract mode:
- Triggered by 50 seconds of idle time on the main menu
- Randomly selects a track and starts an AI-driven demo race
- Displays localized "DEMO MODE" text (8 language variants found in strings)
- Uses trackside cameras with cinematic switching

**Status**: This feature IS functional -- it has no callers in the current
dispatch table (`RunAttractModeDemoScreen` has 0 xrefs TO it), but the function
itself is complete. The idle timer logic in `RunFrontendDisplayLoop` handles
the trigger.

NOTE: The function has zero callers in the xref analysis, which suggests the
dispatch table entry was zeroed or the call was removed. The idle timer in
`RunFrontendDisplayLoop` may still trigger the demo via a different path
(setting `DAT_00495254 = 1` and transitioning to race state).

### 4D. Other Dead/Vestigial Features

| Feature | Evidence | Restoration Difficulty |
|---|---|---|
| **High Scores** | `SNK_HiScoreButTxt` dead symbol | IMPOSSIBLE -- no save/display code |
| **Music Test** | Dead screen entry reference | HARD -- screen function stripped |
| **Credits** | Dead screen entry reference | HARD -- screen function stripped |
| **Snow** | See Section 1 above | MEDIUM -- rendering works, looks like sleet |
| **State 3 FPS viewer** | Fully intact in RunMainGameLoop | WORKS via benchmark mode |

---

## PATCH SUMMARY

### Copy-Paste-Ready Hex Patches

All patches are independent and can be applied in any combination.

#### Patch #1: Snow Rendering (6 bytes)
```
File offset: 0x0465F4
Original:    0F 85 67 04 00 00
Patched:     90 90 90 90 90 90
```

#### Patch #2: Vehicle Damage Wobble (5 bytes)
```
File offset: 0x02C8D0
Original:    33 C0 C3 90 90
Patched:     E9 82 B8 01 00
```

#### Patch #3: Benchmark Mode -- Force Flag (code cave, 2 sites)

NOTE: `g_benchmarkModeActive` is in BSS (zero-init, not on disk). A data
patch is impossible. This requires a code cave.

```
Hijack site:
  File offset: 0x02AA10
  Original:    A1 58 AF 4A 00
  Patched:     E8 7B 19 03 00

Code cave:
  File offset: 0x05C390
  Original:    00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
  Patched:     C7 05 58 AF 4A 00 01 00 00 00 B8 01 00 00 00 C3
```
WARNING: Makes every session a benchmark. Use `-TimeDemo` command-line flag
instead if M2DX.DLL's parser is functional.

### Recommended Application Order

1. **Patch #2** (Damage Wobble) -- safest, most impactful, zero risk
2. **Patch #1** (Snow Rendering) -- medium risk, visually novel
3. **Patch #3** (Benchmark) -- developer tool, try command-line first

### Reverting Patches

All patches are reversible by restoring the original bytes listed above.
Keep a backup of the unmodified `TD5_d3d.exe` before applying any patches.
