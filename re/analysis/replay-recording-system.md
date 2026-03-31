# Replay/Demo Recording & Playback System

## Overview

Test Drive 5 uses an **in-memory input recording** system implemented in M2DX.DLL's
`DXInput` class. During every race, the player's control inputs are delta-compressed
into a timeline buffer. After the race, the "View Replay" option re-initializes the
race with the same RNG seed and replays the recorded inputs through the deterministic
physics simulation. The same mechanism powers the **attract-mode demo** shown on the
main menu after an idle timeout.

**There is no file format.** Recordings are never saved to disk. The entire replay
buffer lives in M2DX.DLL's BSS segment and is lost when the next race begins or
the process exits.

---

## M2DX.DLL Recording API

Six exported functions in M2DX.DLL (all `__cdecl`, return `int`):

| Export | VA | Ordinal | Purpose |
|---|---|---|---|
| `DXInput::WriteOpen(int trackIndex)` | `0x1000A640` | 164 | Reset buffer, store track index, clear cursor |
| `DXInput::Write(void* input8, ulong mode)` | `0x1000A660` | 162 | Append one frame of input (delta-compressed) |
| `DXInput::WriteClose()` | `0x1000A740` | 163 | Finalize: store last frame index |
| `DXInput::ReadOpen(int trackIndex)` | `0x1000A760` | 133 | Reset read cursor for playback |
| `DXInput::Read(void* output8)` | `0x1000A780` | 131 | Reconstruct one frame of input from timeline |
| `DXInput::ReadClose()` | `0x1000D370` | 132 | Stub (returns 1). No-op cleanup. Shares VA with `DXSound::LoadComplete`. |

### Recording lifecycle

```
WriteOpen(trackIndex)     -- called from InitializeRaceSession (0x42B27E)
  |
  v
Write(&controlBits, 1)   -- called every sim tick from PollRaceSessionInput (0x42C630)
  |                          mode=1 strips camera/escape bits
  v
WriteClose()              -- called from RunRaceFrame fade-out (0x42B84E)
```

### Playback lifecycle

```
ReadOpen(trackIndex)      -- called from InitializeRaceSession when g_inputPlaybackActive!=0
  |
  v
Read(&controlBits)        -- called every sim tick from PollRaceSessionInput (0x42C482)
  |                          returns 0 when all entries exhausted
  v
ReadClose()               -- called from RunRaceFrame fade-out
```

---

## In-Memory Buffer Layout

All data resides in M2DX.DLL's BSS/data segments.

### Header region (`0x10033B90`)

| Offset | VA | Size | Name | Description |
|---|---|---|---|---|
| +0x00 | `0x10033B90` | 4 | `trackIndex` | `WriteOpen` parameter (track pool index) |
| +0x04 | `0x10033B94` | 4 | `entryCount` | Number of timeline entries (starts at 2) |
| +0x08 | `0x10033B98` | 4 | `lastFrameIndex` | `WriteClose` stores `frameCursor - 1` here |

### Timeline entry array (`0x10033BA8`)

Array of 8-byte entries, maximum **19996** (`0x4E1C`) entries:

```c
struct InputTimelineEntry {     // 8 bytes
    uint32_t frameAndChannel;   // bits [14:0] = frame index, bit 15 = channel flag
    uint32_t value;             // 32-bit control bitmask snapshot
};
```

- **Channel 0** (bit 15 clear): Player control word 0 (player 1 or single-player)
- **Channel 1** (bit 15 set):   Player control word 1 (player 2 in split-screen)
- Frame index range: `0` to `0x7FEE` (32750), giving a maximum recording of ~18 minutes at 30fps

The first two entries are always the initial state:

| Index | frameAndChannel | value | Meaning |
|---|---|---|---|
| 0 | `0x0000` | word0 initial | Frame 0, channel 0: initial player 1 state |
| 1 | `0x8000` | word1 initial | Frame 0, channel 1: initial player 2 state |

Subsequent entries are emitted only when a value **changes** from the previous
value on that channel (delta compression).

### Playback state globals

| VA | Name | Description |
|---|---|---|
| `0x1005ADC0` | `frameCursor` | Current frame index (shared write/read) |
| `0x1005ADB8` | `lastWord0` | Last recorded value for channel 0 (write) |
| `0x1005ADB0` | `lastWord1` | Last recorded value for channel 1 (write) |
| `0x1005ADB4` | `readIndex` | Current read position in entry array |
| `0x1005ADA0` | `replayWord0` | Current reconstructed channel 0 value (read) |
| `0x1005ACB0` | `replayWord1` | Current reconstructed channel 1 value (read) |

### Buffer capacity

- Max entries: 19996 (header starts at 2, so 19994 delta entries)
- Max frames: 32750 (~18.2 minutes at 30 fps)
- Buffer memory: 19996 * 8 = **~156 KB** (in M2DX.DLL BSS)
- Whichever limit is hit first terminates recording (new inputs are silently dropped)

---

## Delta Compression Algorithm

### Write (recording)

```
function Write(input[2], mode):
    word0 = input[0]
    word1 = input[1]

    if mode == 1:                         // strip camera/escape bits
        word0 &= 0xBBFFFFFF
        word1 &= 0xBBFFFFFF

    if frameCursor == 0:                  // first frame: store initial state
        entry[0] = { frame=0,        value=word0 }
        entry[1] = { frame=0x8000,   value=word1 }
        entryCount = 2
        lastWord0 = word0
        lastWord1 = word1
        frameCursor = 1
        return

    if entryCount < 0x4E1C and frameCursor < 0x7FEE:
        if word0 != lastWord0:            // channel 0 changed
            entry[entryCount] = { frame=frameCursor, value=word0 }
            entryCount++
            lastWord0 = word0

        if word1 != lastWord1:            // channel 1 changed
            entry[entryCount] = { frame=frameCursor|0x8000, value=word1 }
            entryCount++
            lastWord1 = word1

    frameCursor++
```

### Read (playback)

```
function Read(output[2]):
    if frameCursor == 0:                  // first frame: load initial state
        replayWord0 = entry[0].value
        replayWord1 = entry[1].value
        readIndex = 2
    else:
        if readIndex >= entryCount:       // recording exhausted
            output = {0, 0}
            return 0                      // signals end of replay

        // Check channel 0
        if frameCursor == entry[readIndex].frame:
            replayWord0 = entry[readIndex].value
            readIndex++

        // Check channel 1 (bit 15 set)
        if (entry[readIndex].frame & 0x8000) != 0:
            if frameCursor == (entry[readIndex].frame & 0x7FFF):
                replayWord1 = entry[readIndex].value
                readIndex++

    output[0] = replayWord0
    output[1] = replayWord1
    frameCursor++
    return 1
```

---

## Input Bitmask Format

The control state is a pair of 32-bit words (`gPlayerControlBits` at `0x00482FFC`
and `0x00483000`). Each word encodes one player's input:

### Direction/steering bits

| Bit | Hex | Meaning |
|---|---|---|
| 0 | `0x00000001` | Steer left |
| 1 | `0x00000002` | Steer right |
| 9 | `0x00000200` | Accelerate / throttle |
| 10 | `0x00000400` | Brake / reverse |

### Action bits (custom keyboard mapping)

| Bit | Hex | Meaning |
|---|---|---|
| 18 | `0x00040000` | Joystick button 0 (default: handbrake/action) |
| 19 | `0x00080000` | Joystick button 1 |
| 20 | `0x00100000` | Action 1 (handbrake) |
| 21 | `0x00200000` | Action 2 (horn/siren) |
| 22 | `0x00400000` | Action 3 (gear up) |
| 23 | `0x00800000` | Action 4 (gear down) |
| 24 | `0x01000000` | Change camera view |
| 25 | `0x02000000` | Rear-view camera |

### System bits (stripped from recording)

| Bit | Hex | Meaning | Stripped? |
|---|---|---|---|
| 26 | `0x04000000` | Camera cycle (keyboard F-key) | YES (mask 0xBBFFFFFF) |
| 28 | `0x10000000` | Mouse steering active | No |
| 29 | `0x20000000` | F12 key (keyboard scan 0x58) | No |
| 30 | `0x40000000` | Escape / abort race (scan 0x01) | YES (mask 0xBBFFFFFF) |
| 31 | `0x80000000` | Analog X-axis payload flag (joystick) | No |
| 27 | `0x08000000` | Analog Y-axis payload flag (joystick) | No |

When bit 31 is set, bits [8:0] encode the packed analog X-axis position (offset
from joystick center). When bit 27 is set, bits [17:9] encode the packed analog
Y-axis position. Both use the formula `(axis + center_offset) | flag_bit` in
`DXInput::GetJS` custom mode (`g_customJoystickXAxisPackedModeTable`).

The `0xBBFFFFFF` mask applied during Write(mode=1) strips bits 26 and 30
(`0x44000000`), preventing camera switches and escape presses from being
recorded. During playback, the ESC key is re-checked live from the keyboard
and OR'd back in, allowing the player to exit the replay at any time.

---

## Replay Determinism

The replay system achieves deterministic physics by:

1. **RNG seed preservation**: At race start (`InitializeRaceSession`, VA `0x42AA10`):
   - Normal race: saves current `srand()` seed to `DAT_004AAD64`
   - Replay: restores seed from `DAT_004AAD64` and calls `srand(seed)`

2. **Fixed-timestep simulation**: The physics loop in `RunRaceFrame` uses a fixed
   timestep accumulator (`g_simTimeAccumulator`), so the same input sequence
   produces identical results regardless of rendering frame rate.

3. **Input-only recording**: Only the player's control bitmask is recorded.
   AI behavior, physics, particle effects, etc. are all deterministic given
   the same seed and inputs.

4. **Track state**: The track index passed to WriteOpen/ReadOpen ensures the
   same track geometry is loaded for replay.

---

## Game Modes Using the Replay System

### 1. Post-Race Replay ("View Replay")

**Trigger**: In `RunRaceResultsScreen` (VA `0x422480`), state 0x0F, button index 1:
```c
g_inputPlaybackActive = 1;    // at VA 0x423405
DAT_004A2C8C = 2;             // re-enter race
```

**Flow**:
1. Player finishes race -> results screen
2. Button menu: "Race Again" / "View Replay" / "View Race Data" / ...
3. Selecting "View Replay" sets `g_inputPlaybackActive = 1`
4. Frontend re-enters `InitializeRaceSession`:
   - Restores RNG seed from `DAT_004AAD64`
   - Calls `DXInput::ReadOpen(trackIndex)` instead of `WriteOpen`
   - Sets up trackside camera profiles (`InitializeTracksideCameraProfiles`)
   - Disables HUD elements (param_1=0 to `InitializeRaceOverlayResources`)
5. During race loop, `PollRaceSessionInput` calls `Read` instead of polling devices
6. ESC key is live-checked from keyboard and OR'd into bit 30 for exit
7. When `Read` returns 0 (recording exhausted), control bits zero out -> race stalls
8. Race ends via fade -> returns to frontend
9. `InitializeFrontendResourcesAndState` detects `g_inputPlaybackActive != 0`:
   - Sets `g_returningFromReplay` (`0x4962D0`) = 1
   - Clears `g_inputPlaybackActive` = 0
   - Skips `SerializeRaceStatusSnapshot` (preserves pre-replay race state)
10. Results screen re-enters; `g_returningFromReplay == 1` triggers auto-skip
    to state 0x0C (cleanup), then rebuilds the button menu

**Replay availability**: The `g_replayButtonEnabled` flag (`0x00497A6C`) controls
whether "View Replay" appears as a clickable button or a greyed-out preview.
It is set during race-type selection in `RaceTypeCategoryMenuStateMachine` and
during `ScreenLocalizationInit`. When 0, `CreateFrontendDisplayModePreviewButton`
is used; when non-zero, `CreateFrontendDisplayModeButton` is used.

**Global**: `g_inputPlaybackActive` at VA `0x00466E9C`

### 2. Attract-Mode Demo ("DEMO MODE")

**Trigger**: Frontend screen entry 2 (VA `0x4275A0`) is the idle timeout handler.
After 45+ seconds of inactivity on the main menu, it:
```c
DAT_00495254 = 1;             // "demo wanted" flag
```

**Flow**:
1. Idle timeout fires in main menu -> sets `DAT_00495254 = 1`
2. `InitializeRaceSession` detects `DAT_00495254 != 0`:
   - Sets `g_replayModeFlag = 1` (VA `0x004AAF64`)
   - Sets `g_inputPlaybackActive = 0` (NO playback -- the AI drives!)
   - Sets `g_raceEndTimerStart = 0` (enables 45-second auto-exit timer)
   - Uses `DAT_00490BA8` for track selection (cycles through schedule)
3. Race runs with AI-only drivers, trackside cameras, no player input
4. After 45 seconds, `RunRaceFrame` auto-triggers the fade-out
5. Or any keypress triggers ESC -> immediate exit
6. Returns to main menu with `DAT_00495254` cleared

**Key difference**: Demo mode does NOT use the recording buffer at all.
`g_inputPlaybackActive` is 0, so `WriteOpen` is called (recording the AI
race for nothing), and no `Read` calls occur. The AI drives the cars.

### 3. Benchmark / TimeDemo Mode

**Trigger**: `-TimeDemo` command-line flag parsed by M2DX.DLL config system
(string at `0x10027D54`). Sets `g_benchmarkModeActive` (VA `0x004AAF58`).

**Flow**: Similar to demo mode but additionally:
- Allocates 1M sample buffer for per-frame timing
- Calls `RecordBenchmarkFrameRateSample` each frame
- On race end, calls `WriteBenchmarkResultsTgaReport` to save FPS data as TGA
- Game state 3 displays the TGA result image

---

## HUD Display During Replay/Demo

`InitializeRaceOverlayResources` (VA `0x4377B0`) controls HUD visibility via a
bitmask allocated at `DAT_004B0C00`. When called with `param_1=0` (replay/demo),
the HUD bitmask is set to suppress all normal race elements.

- **Normal race** (both flags 0): Full HUD -- speedometer, position, lap, minimap.
  `param_1=1` enables all HUD bitmask bits.
- **View Replay** (`g_inputPlaybackActive=1`, `g_replayModeFlag=0`): HUD bitmask
  = `0x80000000` (all elements suppressed). `DrawRaceStatusText` early-exits
  (does nothing). No text overlay is shown -- pure trackside camera footage.
- **Demo mode** (`g_inputPlaybackActive=0`, `g_replayModeFlag=1`): HUD bitmask
  = `0x0` (all elements suppressed). `DrawRaceStatusText` shows localized
  "DEMO MODE" text via `QueueRaceHudFormattedText` (e.g., "DEMO MODUS" in
  German, "MODE DEMO" in French, sourced from `PTR_DAT_00473E38[10]`).

The "REPLAY" glyph label (string at `0x4743C4`) is pre-rendered by
`InitializeRaceHudLayout` but only displayed as a HUD element label, not as a
standalone overlay during View Replay mode.

Both modes use trackside cameras (`InitializeTracksideCameraProfiles`) with automatic
cinematic switching. Player camera controls are blocked (`g_replayModeFlag` gates
camera cycle in `PollRaceSessionInput`). Note: during View Replay,
`g_replayModeFlag` is 0, but camera controls are also blocked because
`g_inputPlaybackActive` is checked separately in the camera input path.

---

## Key Globals Summary

| VA | Name | Type | Description |
|---|---|---|---|
| `0x00466E9C` | `g_inputPlaybackActive` | int | 1 during "View Replay" playback |
| `0x004AAF64` | `g_replayModeFlag` | int | 1 during attract-mode demo |
| `0x00495254` | `g_demoModeRequested` | int | 1 when idle timeout triggers demo |
| `0x004AAF58` | `g_benchmarkModeActive` | int | 1 when -TimeDemo CLI flag set |
| `0x004AAD64` | `g_savedRngSeed` | uint | RNG seed saved at race start |
| `0x00482FFC` | `gPlayerControlBits` | uint[2] | Live 64-bit control state (P1, P2) |
| `0x004AADA4` | `g_raceEndTimerStart` | DWORD | timeGetTime() at demo start (45s timeout) |
| `0x004AAEF8` | `g_raceEndFadeState` | int | Fade progress counter |
| `0x004962D0` | `g_returningFromReplay` | uint | Set to 1 when returning from replay to results screen |
| `0x00497A6C` | `g_replayButtonEnabled` | int | Controls whether "View Replay" is active (1) or greyed (0) |

### M2DX.DLL internal globals

| VA (M2DX) | Name | Type | Description |
|---|---|---|---|
| `0x10033B90` | `trackIndex` | int | WriteOpen parameter (track pool index) |
| `0x10033B94` | `entryCount` | int | Number of timeline entries (starts at 2) |
| `0x10033B98` | `lastFrameIndex` | int | WriteClose stores `frameCursor - 1` here |
| `0x10033BA8` | `entryArray[0].frame` | uint | Start of timeline entry array (frame+channel) |
| `0x10033BAC` | `entryArray[0].value` | uint | Start of timeline entry array (input value) |
| `0x1005ADC0` | `frameCursor` | uint | Current frame index (shared write/read) |
| `0x1005ADB8` | `lastWord0` | uint | Last recorded value for channel 0 (write mode) |
| `0x1005ADB0` | `lastWord1` | uint | Last recorded value for channel 1 (write mode) |
| `0x1005ADB4` | `readIndex` | uint | Current read position in entry array (read mode) |
| `0x1005ADA0` | `replayWord0` | uint | Current reconstructed channel 0 value (read mode) |
| `0x1005ACB0` | `replayWord1` | uint | Current reconstructed channel 1 value (read mode) |

---

## Limitations

1. **No persistence**: Recordings are lost after the next race or program exit
2. **No file export**: DX::FWrite is never called; there is no save/load mechanism
3. **Single recording**: Only one recording exists at a time (the last race)
4. **No multi-player replay**: Network races cannot be replayed (DXPlay state not captured)
5. **Buffer overflow silent**: When 19996 entries or 32750 frames are exceeded,
   new inputs are silently dropped (recording truncated)
6. **45-second demo limit**: Attract-mode demos auto-exit after 45 seconds
7. **View Replay disabled on ESC/DNF**: If the player escaped/quit mid-race
   (companion_state_2 == 0x02) or didn't finish (post_finish_metric_base == 0),
   the "View Replay" button is grayed out (CreateFrontendDisplayModePreviewButton
   instead of CreateFrontendDisplayModeButton)
