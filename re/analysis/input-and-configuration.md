# Input System, Controller Binding, and Configuration Deep-Dive

> Generated: 2026-03-20
> Sources: TD5_d3d.exe (port 8195), M2DX.dll (port 8193)

---

## Table of Contents
1. [Options Hub Screen](#options-hub-screen)
2. [Control Options Screen](#control-options-screen)
3. [Controller Binding Page](#controller-binding-page)
4. [Game Options Screen](#game-options-screen)
5. [Sound Options Screen](#sound-options-screen)
6. [Display Options Screen](#display-options-screen)
7. [Input Bitmask Architecture](#input-bitmask-architecture)
8. [DXInput Joystick Reading](#dxinput-joystick-reading)
9. [DXInput Keyboard Reading](#dxinput-keyboard-reading)
10. [DXInput Mouse Reading](#dxinput-mouse-reading)
11. [DXInput::SetConfiguration](#dxinput-setconfiguration)
12. [Joystick Enumeration](#joystick-enumeration)
13. [Force Feedback System](#force-feedback-system)
14. [M2DX Startup Configuration](#m2dx-startup-configuration)
15. [M2DX Window Initialization](#m2dx-window-initialization)
16. [M2DX Window Message Handler](#m2dx-window-message-handler)
17. [ConfigureGameTypeFlags](#configuregametypeflags)

---

## Options Hub Screen

**Function**: `ScreenOptionsHub` at `0x41d890` (TD5_d3d.exe)
**Frontend Screen Index**: 0x0C

### Layout
6 buttons in standard frontend state machine (states 0-8):

| Button | Label (SNK export) | Action |
|--------|-------------------|--------|
| 0 | `SNK_GameOptionsButTxt` | Navigate to screen 0x0D (Game Options) |
| 1 | `SNK_ControlOptionsButTxt` | Navigate to screen 0x0E (Control Options) |
| 2 | `SNK_SoundOptionsButTxt` | Navigate to screen 0x0F (Sound Options) |
| 3 | `SNK_GraphicsOptionsButTxt` | Navigate to screen 0x10 (Display Options) |
| 4 | `SNK_TwoPlayerOptionsButTxt` | Navigate to screen 0x11 (Two-Player Options) |
| 5 | `SNK_OkButTxt` | Accept + return to screen 0x05 (main menu) |

### OK Button Side Effects (state 6, button 5)
When OK is pressed, the hub copies shadow options into live runtime globals:
```
DAT_00463188 = DAT_00466018 ^ 1    (3D collisions: inverted)
gDifficultyEasy = DAT_00466014      (dynamics mode)
gTrafficActorsEnabled = DAT_00466008 (traffic on/off)
gRaceDifficultyTier = DAT_00466010   (difficulty tier)
DAT_004b0fa8 = DAT_00466004         (checkpoint timers on/off)
```

### State Machine Pattern
All options screens share the same 9-state pattern:
- **State 0**: Load resources, create buttons, set inline string table
- **States 1-2**: Present primary buffer, reset tick counter
- **State 3**: Slide-in animation (tick 0x00 to 0x27), buttons fly in from alternating sides
- **States 4-5**: Render option value surface (state 4 redraws text, state 5 skips)
- **State 6**: Active input loop -- arrow keys/clicks modify option shadows
- **State 7**: Prepare slide-out (restore secondary buffer, reset strings)
- **State 8**: Slide-out animation (tick 0x00 to 0x10), then navigate to `g_returnToScreenIndex`

---

## Control Options Screen

**Function**: `ScreenControlOptions` at `0x41df20` (TD5_d3d.exe)
**Frontend Screen Index**: 0x12

### Input Device Type Detection
The screen reads `DAT_00465660[device_index]` for each player to determine device type. The low byte encodes the device class:

| Low byte | Meaning | Display |
|----------|---------|---------|
| 0x03 | Keyboard | `local_84 = 0` |
| 0x04 | Joystick (various sub-types) | `local_84 = 1-3` |
| 0xFF | No device / disconnected | `local_84 = 4` |

For joystick sub-types, the high byte (`& 0xFF00`) selects:
- `0x0400` -> display index 2 (gamepad-style)
- `0x0600` -> display index 3 (wheel/pedals)
- Otherwise -> display index 1 (standard joystick)

### Layout
5 buttons:

| Button | Label | Action |
|--------|-------|--------|
| 0 | `SNK_Player1ButTxt` | Cycle P1 input source (arrow keys) |
| 1 | `SNK_ConfigureButTxt` | Open binding page for P1 (`DAT_004974b8=0`, screen 0x12) |
| 2 | `SNK_Player2ButTxt` | Cycle P2 input source (arrow keys) |
| 3 | `SNK_ConfigureButTxt` | Open binding page for P2 (`DAT_004974b8=1`, screen 0x12) |
| 4 | `SNK_OkButTxt` | Return to options hub (screen 0x0C) |

### Device Cycling Logic (state 6)
Player source variables: `DAT_00497a58` (P1), `DAT_00465ff4` (P2).
- Cycles through indices 0-7 via `(current + direction) & 7`
- Skips entries where `DAT_00465660[index] == 0` (no device at that slot)
- Skips if the chosen index matches the OTHER player's source (prevents both players using same device)

### Controller Icon Display
`Controllers.tga` from `FrontEnd.zip` contains 4 stacked 64x32 sprites:
- Row 0 (`local_84=0`): Keyboard icon
- Row 1 (`local_84=1`): Joystick icon
- Row 2 (`local_84=2`): Gamepad icon
- Row 3 (`local_84=3`): Wheel icon

Source rect = `(0, local_84 << 5, 0x40, 0x20)`.

---

## Controller Binding Page

**Function**: `ScreenControllerBindingPage` at `0x40fe00` (TD5_d3d.exe)
**Frontend Screen Index**: 0x12 (sub-screen, accessed from Control Options)

### Overview
This is the most complex options screen. It has TWO completely different flows depending on the input device type:

1. **Joystick binding** (device low byte != keyboard): States 0, 9, 10, 11
2. **Keyboard binding** (device byte == 0x03): States 0, 0x13, 0x14, 0x19, 0x1A, 0x1B

### Key Globals

| Address | Name | Purpose |
|---------|------|---------|
| `DAT_004974b8` | Player index | 0 = P1, 1 = P2 |
| `DAT_00490b94` | Device source index | Copied from P1/P2 source at state 0 |
| `DAT_00490ba4` | Axis/button count | Number of bindable axes on the device (0-8) |
| `DAT_00490b8c` | Current joystick state | Bitmask from `DXInput::GetJS()` |
| `DAT_00490b88` | Previous-previous JS state | Two-frame-ago state (for edge detection) |
| `DAT_00490b90` | Previous JS state | One-frame-ago state |
| `DAT_00463fc4` | Per-player config flag | `[player*9]` -- set to 1 when configuring |
| `DAT_00463fc8` | Binding table base | `[player*9 + axis]` -- 9 slots per player, stores action index |
| `DAT_00490b84` | Keyboard scan code counter | 0-9 during keyboard binding |
| `DAT_00464054-00464060` | Keyboard scan code buffer | 16 bytes storing bound scan codes |

### Binding Table Structure (`DAT_00463fc8`)

Each player has a 9-entry array at `DAT_00463fc8[player*9]`. Each entry is an action index into `SNK_ControlText`, which is a localized string array with 16-byte stride entries:

| Index | SNK_ControlText offset | Probable action |
|-------|----------------------|-----------------|
| 0 | +0x00 | (none/unbound) |
| 1 | +0x10 | Steer Left |
| 2 | +0x20 | Steer Up (axis negative) |
| 3 | +0x30 | Steer Down (axis positive) |
| 4 | +0x40 | Accelerate |
| 5 | +0x50 | Brake |
| 6 | +0x60 | Handbrake |
| 7 | +0x70 | Horn/Siren |
| 8 | +0x80 | Gear Up |
| 9 | +0x90 | Gear Down |
| 10 | +0xA0 | Rear View |
| 11 | +0xB0 | (Up/Down / blank label) |

Values cycle 2-10 (wraps from 11 back to 2) when the player presses a joystick button in the binding screen.

### Joystick Binding Flow (state 10)

1. **Read joystick**: `DAT_00490b8c = DXInput::GetJS(DAT_00490b94 - 1)`
2. **Three-frame edge detection**: `DAT_00490b90` (prev-prev), `DAT_00490b88` (prev AND'd with current)
3. **Button lights**: For each axis (0 to `DAT_00490ba4`), renders a 16x16 `ButtonLights.tga` indicator. The light is ON (`srcY=0x10`) if the corresponding bit in `DAT_00490b8c` is set, OFF (`srcY=0`) otherwise
4. **Action cycling**: When a button transitions from released to held (detected via `DAT_00490b90 & bit` AND `DAT_00490b8c & bit`), the action index in the binding table increments. Values wrap: `if (value+1 > 10) value = 2`
5. **Two-axis swap**: When `DAT_00490ba4 == 2`, pressing either button swaps the two binding entries (for steering axis inversion)

### Axis Header Variants
The header text above the binding rows depends on which axis types (2=up, 3=down) are present in the binding table:

| Bitmask | Function | Header text |
|---------|----------|-------------|
| 0 (neither) | `RenderControllerBindingPageUpDownHeader` (0x41043c) | SNK_UpDownTxt + SNK_ControlText+0xB0 |
| 1 (down only) | `RenderControllerBindingPageDownHeader` (0x4104b2) | SNK_DownTxt + SNK_ControlText+0x30 |
| 2 (up only) | `RenderControllerBindingPageUpHeader` (0x410527) | SNK_UpTxt + SNK_ControlText+0x20 |
| 3 (both) | `RenderControllerBindingPageBlankOrRearViewHeader` (0x410599) | Blank (normal) or SNK_ControlText+0xA0 (wheel/pedals 0x600) |

### Wheel/Pedals Special Case
When `(DAT_00465660[device] & 0xFF00) == 0x600`:
- The header text shows `SNK_PedalsTxt` instead of Up/Down
- The blank/rearview header shows the Rear View label (offset 0xA0) instead of clearing
- The Up/Down header variants skip the BltColorFill + text redraw

### Keyboard Binding Flow (states 0x13-0x1B)

1. **State 0x13-0x14**: Slide-in animation, then display `SNK_PressKeyTxt` ("Press Key")
2. **State 0x19**: Show the current action label from `SNK_ControlText[DAT_00490b84 * 0x10]`
3. **State 0x1A**: Scan all 256 keyboard scan codes via `DXInput::CheckKey(scancode)`:
   - Skips codes already bound in `DAT_00464054-00464060` (prevents duplicate bindings)
   - Uses a 16-byte packed byte buffer -- each bound scan code stored as a byte
   - On successful bind: plays SFX 3 (confirm), advances `DAT_00490b84`
   - After 10 bindings complete, transitions to slide-out
4. **State 0x1B**: Slide-out animation

### Helper Functions

- **`DrawControlBindingTextWithOkButton`** (0x4100ce): Dispatch table -- selects control text by axis bitmask (0-3), then creates OK button
- **`DrawControlBindingText1WithOkButton`** (0x4100fa): Hardcoded to show SNK_ControlText+0x30 (Down) + OK button
- **`DrawControlBindingText2WithOkButton`** (0x410111): Hardcoded to show SNK_ControlText+0x20 (Up) + OK button
- **`RenderControllerBindingMenuPage`** (0x410380): Main render loop body for joystick binding rows
- **`RenderControllerBindingPageRows`** (0x410613): Shared row renderer + input handler tail

---

## Game Options Screen

**Function**: `ScreenGameOptions` at `0x41f990` (TD5_d3d.exe)
**Frontend Screen Index**: 0x0D

### Layout
8 buttons (7 options + OK):

| Button | Label | Variable | Values | Display source |
|--------|-------|----------|--------|----------------|
| 0 | `SNK_CircuitLapsTxt` | `DAT_00466000` | 0-3 | Integer format string |
| 1 | `SNK_CheckpointTimersButTxt` | `DAT_00466004` | 0-1 | `SNK_OnOffTxt[value]` |
| 2 | `SNK_TrafficButTxt` | `DAT_00466008` | 0-1 | `SNK_OnOffTxt[value]` |
| 3 | `SNK_CopsButTxt` | `gSpecialEncounterConfigShadow` | 0-1 | `SNK_OnOffTxt[value]` |
| 4 | `SNK_DifficultyButTxt` | `DAT_00466010` | 0-2 (wraps) | `SNK_DifficultyTxt[value]` |
| 5 | `SNK_DynamicsButTxt` | `DAT_00466014` | 0-1 | `SNK_DynamicsTxt[value]` |
| 6 | `SNK_3dCollisionsButTxt` | `DAT_00466018` | 0-1 | `SNK_OnOffTxt[value]` |
| 7 | `SNK_OkButTxt` | - | - | Returns to screen 0x0C |

### Option Semantics

- **Circuit Laps** (0-3): Clamped integer, displayed raw. Used as `gCircuitLapCount` at race start.
- **Checkpoint Timers** (toggle): Copied to `DAT_004b0fa8` at hub OK. Controls checkpoint countdown overlay.
- **Traffic** (toggle): Copied to `gTrafficActorsEnabled`. Disables traffic actor spawning.
- **Cops** (toggle): Controls `gSpecialEncounterConfigShadow` -> `gSpecialEncounterEnabled`. Enables encounter actor (slot 9) triggers.
- **Difficulty** (3 values): Likely Easy/Medium/Hard. Copied to `gRaceDifficultyTier` (0/1/2). Controls AI rubber-band scaling.
- **Dynamics** (toggle): Copied to `gDifficultyEasy`. Toggles between simplified and full physics model.
- **3D Collisions** (toggle): Stored XOR'd: `DAT_00463188 = value ^ 1`. Controls vehicle-to-vehicle collision resolution.

---

## Sound Options Screen

**Function**: `ScreenSoundOptions` at `0x41ea90` (TD5_d3d.exe)
**Frontend Screen Index**: 0x13

### Layout
5 buttons:

| Button | Label | Variable | Action |
|--------|-------|----------|--------|
| 0 | `SNK_SfxModeButTxt` | `DAT_00465fe8` | Cycle SFX mode (0-2 if 3D capable, 0-1 otherwise) |
| 1 | `SNK_SfxVolumeButTxt` | `DAT_00465fec` | SFX volume 0-100, step 10 |
| 2 | `SNK_MusicVolumeButTxt` | `DAT_00465ff0` | Music volume 0-100, step 10 |
| 3 | `SNK_MusicTestButTxt` | - | Navigate to screen 0x13 (music test) |
| 4 | `SNK_OkButTxt` | - | Return to screen 0x0C |

### Volume Bar Rendering
- Uses `VolumeBox.tga` (224x12) as background frame and `VolumeFill.tga` as the fill bar
- Fill width = `(volume * 0xDE) / 100`, max 0xDE (222px)
- During slide-in, fill is interpolated: `(volume * tick / 0x27 * 0xDE) / 100`

### SFX Mode
- `DXSound::CanDo3D()` gates whether mode 2 (3D positional audio) is available
- `DXSound::SetPlayback(mode)` applies the mode
- Display text: `SNK_SFX_Modes[DAT_00465fe8]`

### Volume Application
- SFX: `DXSound::SetVolume((volume * 0xFC00) / 100 & 0xFC00)` -- scaled to DirectSound range
- Music: `DXSound::CDSetVolume((volume * 0xFC00) / 100 & 0xFC00)`

### Controller Icon
Reuses `Controllers.tga` to show the SFX mode icon at `(DAT_00465fe8 + 4) * 0x20` -- offset by 4 rows past the controller icons, so the Controllers.tga sprite sheet also contains sound mode icons in rows 4-6.

---

## Display Options Screen

**Function**: `ScreenDisplayOptions` at `0x420400` (TD5_d3d.exe)
**Frontend Screen Index**: 0x10

### Layout
5 buttons:

| Button | Label | Variable | Values |
|--------|-------|----------|--------|
| 0 | `SNK_ResolutionButTxt` | `gConfiguredDisplayModeOrdinal` | Cycles through available display modes |
| 1 | `SNK_FoggingButTxt` | `DAT_00466024` | 0-1 toggle (only if `DXD3D::CanFog() == 1`) |
| 2 | `SNK_SpeedReadoutButTxt` | `DAT_00466028` | 0-1 toggle |
| 3 | `SNK_CameraDampingButTxt` | `DAT_0046602c` | 0-9 range |
| 4 | `SNK_OkButTxt` | - | Return to screen 0x0C |

### Resolution Cycling
- Mode names stored in 32-byte entries at `DAT_004974bc` (stride 0x20, max table at 0x4978bc = 50 entries)
- Wraps: if index goes negative, counts populated entries and sets to last; if entry is null string, wraps to 0
- Display: `MeasureOrCenterFrontendLocalizedString(DAT_004974bc + ordinal * 0x20, ...)`

### Fogging
- Conditionally created as an active button (`CreateFrontendDisplayModeButton`) only if `DXD3D::CanFog() == 1`
- Otherwise created as a grayed-out preview button (`CreateFrontendDisplayModePreviewButton`)
- Display: `SNK_OnOffTxt[DAT_00466024]`

### Speed Readout
- Display: `SNK_SpeedReadTxt[DAT_00466028]` -- likely MPH/KPH toggle

### Camera Damping
- Integer range 0-9, displayed as a number format string
- Controls chase camera interpolation smoothing factor

---

## Input Bitmask Architecture

The game uses a 32-bit bitmask (`controlBits`) to represent all player input state. Both keyboard and joystick paths produce the same bitmask format:

### Bit Assignments

| Bit(s) | Hex Mask | Action |
|--------|----------|--------|
| 0 | 0x00000001 | Steer Left |
| 1 | 0x00000002 | Steer Right |
| 9 | 0x00000200 | Accelerate |
| 10 | 0x00000400 | Brake |
| 18+ | 0x00040000+ | Joystick button 0, 1, 2... (bit 18 + button_index) |
| 20 | 0x00100000 | Handbrake (custom keyboard action 1) |
| 21 | 0x00200000 | Horn/Siren (custom keyboard action 2) |
| 22 | 0x00400000 | Gear Up (custom keyboard action 3) |
| 23 | 0x00800000 | Gear Down (custom keyboard action 4) |
| 24 | 0x01000000 | Change View (custom keyboard action 5) |
| 25 | 0x02000000 | Rear View (custom keyboard action 6) |
| 27 | 0x08000000 | Y-axis analog packed mode flag |
| 29 | 0x20000000 | Escape key held |
| 30 | 0x40000000 | Enter key held |
| 31 | 0x80000000 | X-axis analog packed mode flag |

### Analog Packed Modes
When a joystick axis is in "packed analog" mode (single-axis-per-direction mapping), the raw axis value is OR'd into the bitmask:
- **X-axis**: `(rawX + range) | 0x80000000` -- the lower bits carry the analog position
- **Y-axis**: `(rawY + range) * 0x200 | 0x08000000` -- shifted left by 9 bits

The EXE's `UpdatePlayerVehicleControlState` (0x402e60) unpacks these to produce continuous steering/throttle values.

---

## DXInput Joystick Reading

**Function**: `DXInput::GetJS` at `0x1000a1e0` (M2DX.dll)

### Flow
1. Poll via `IDirectInputDevice::Poll()` (vtable+0x64)
2. If device lost (DIERR_INPUTLOST or DIERR_NOTACQUIRED), reacquire via `Acquire()` (vtable+0x1C)
3. After successful reacquire, if force-feedback is enabled for this device AND `CreateEffects` callback is set, invoke it
4. Read state via `GetDeviceState(0x50, &buffer)` (vtable+0x24) -- 80-byte DIJOYSTATE structure
5. Joystick axis dead zone: threshold `0x50` (80 out of 1000 range, i.e. 8%)

### Default Mode (`g_isCustomInputConfigEnabled == 0`)
- Buttons 0-N mapped linearly to bits `0x40000 << button_index`
- X-axis > +80 -> bit 0x02 (right), X < -80 -> bit 0x01 (left)
- Y-axis > +80 -> bit 0x400 (brake), Y < -80 -> bit 0x200 (accel)

### Custom Mode
- Determines player index: if `g_player1InputSource == joystick+1` -> player 0, else player 1
- Uses per-player 16-entry tables:
  - `g_customJoystickButtonBitTable[player*16 + button]` -- maps physical button to action bit
  - `g_customJoystickXAxisPackedModeTable[player*16]` -- if `0x80000000`, use analog X packing
  - `g_customJoystickYAxisPackedModeTable[player*16]` -- if `0x08000000`, use analog Y packing
  - `g_customJoystickX/YNegative/PositiveBitTable[player*16]` -- discrete axis-to-bit maps

### Global Key Overlay
Regardless of mode, two keyboard keys are always checked:
- Scan code `0x19` (offset `DAT_1005af2d`, which is `DAT_1005af14 + 0x19`): bit 0x20000000 (Escape)
- Scan code `0x01` (offset `DAT_1005af15`, which is `DAT_1005af14 + 0x01`): bit 0x40000000 (Enter)

### Per-Joystick Data (stride 0x70 = 112 bytes)
- `DXInput::js[index*0x1C]`: IDirectInputDevice pointer (0x1C = 28 dwords stride)
- `DAT_1005ad04 + index*0x70`: Force-feedback capability flag (0 or 1)
- `DAT_1005acd4 + index*0x70`: Axis range value (default 0xFA = 250)
- `DAT_1005acd8 + index*0x70`: Axis dead zone value (default 0x19 = 25 = 2.5%)

---

## DXInput Keyboard Reading

**Function**: `DXInput::GetKBStick` at `0x1000a400` (M2DX.dll)

### Default Mode (non-custom)
Uses hardcoded scan code offsets from `DAT_1005af14` (256-byte DirectInput keyboard state buffer):

| Offset from buffer | Hex | Likely Scan Code | Action | Bit |
|-------|-----|-----------------|--------|-----|
| `DAT_1005afdc` byte (high bit) | 0xC8 | Up Arrow | Accelerate | 0x200 |
| `DAT_1005afe4` byte | 0xD0 | Down Arrow | Brake | 0x400 |
| `DAT_1005afdc` dword (high bit of high byte) | 0xCB | Left Arrow | Steer Left | 0x01 |
| `DAT_1005afe1` byte | 0xCD | Right Arrow | Steer Right | 0x02 |
| `DAT_1005af30` byte | 0x1C | Enter/Return | Button (bit 18) | 0x40000 |

### Custom Mode
10 configurable scan code variables:

| Variable | Action | Bit |
|----------|--------|-----|
| `g_customKeyboardUpScanCode` | Accelerate | 0x200 |
| `g_customKeyboardDownScanCode` | Brake | 0x400 |
| `g_customKeyboardLeftScanCode` | Steer Left | 0x01 |
| `g_customKeyboardRightScanCode` | Steer Right | 0x02 |
| `g_customKeyboardAction1ScanCode` | Handbrake | 0x100000 |
| `g_customKeyboardAction2ScanCode` | Horn/Siren | 0x200000 |
| `g_customKeyboardAction3ScanCode` | Gear Up | 0x400000 |
| `g_customKeyboardAction4ScanCode` | Gear Down | 0x800000 |
| `g_customKeyboardAction5ScanCode` | Change View | 0x1000000 |
| `g_customKeyboardAction6ScanCode` | Rear View | 0x2000000 |

### Global Keys (always active)
- Scan code 0x19 -> bit 0x20000000 (Escape)
- Scan code 0x01 -> bit 0x40000000 (Enter)

---

## DXInput Mouse Reading

**Function**: `DXInput::GetMouse` at `0x10009790` (M2DX.dll)

### Behavior
1. Reads relative mouse movement via `GetDeviceState(0x10, &state)` -- 16-byte DIMOUSESTATE
2. If device lost, reacquires and resets accumulators
3. Converts relative deltas to absolute cursor position:
   ```
   mouseX = (windowWidth/2 - accumulatorX) + rawDeltaX
   mouseY = (windowHeight/2 - accumulatorY) + rawDeltaY
   ```
4. In fullscreen mode, clamps cursor to `[0, width-1]` x `[0, height-1]`
5. Exports to:
   - `DAT_10061bd0`: Mouse X position
   - `DAT_10061bd4`: Mouse Y position
   - `DAT_10061bd8`: Mouse button state (bit 7 of first button byte)

### Accumulator Reset
On reacquire, `g_mouseAccumulatorX/Y` are set to the raw state, effectively centering the logical cursor.

---

## DXInput::SetConfiguration

**Function**: `DXInput::SetConfiguration` at `0x100098f0` (M2DX.dll)
**Signature**: `int SetConfiguration(int player1Source, int player2Source)`

### Purpose
Called at race start to compile the frontend binding configuration into the runtime input tables used by `GetJS` and `GetKBStick`.

### Source Values
- `0` = Keyboard
- `1` or `2` = Joystick 1 or 2 (maps to `js[source-1]`)

### Keyboard Path (source == 0)
Copies 16 dwords from `Configure[player*16]` directly to `g_customKeyboardLeftScanCode` et al. The `Configure` array holds the 10 scan codes + padding bound in the keyboard binding flow.

### Joystick Path (source == 1 or 2)
For each player, processes the binding configuration tables:

1. **Zeros** all 16 entries in `g_customJoystick*Table[player*16]`
2. **Analog axis detection**: Checks if exactly ONE entry maps to direction value 1 (left) and exactly ONE to value 2 (right):
   - If so AND the first two config entries are (1, 2): sets `g_customJoystickXAxisPackedModeTable = 0x80000000` (analog X)
   - Otherwise: copies discrete bit mappings to `g_customJoystickXNegative/PositiveBitTable`
3. **Same logic for Y-axis**: Checks for values 0x200 (up) and 0x400 (down):
   - If paired: sets `g_customJoystickYAxisPackedModeTable = 0x08000000` (analog Y)
   - Otherwise: discrete bit mappings
4. **Button table**: Copies 10 entries from `g_configJoystickButtonBindingTable[player*16]` to `g_customJoystickButtonBitTable[player*16]`

Finally sets `g_isCustomInputConfigEnabled = 1`.

---

## Joystick Enumeration

**Function**: `EnumerateJoystickDeviceCallback` at `0x10009d70` (M2DX.dll)

### Device Discovery
Called by DirectInput's `EnumDevices()`. Accepts up to **2 joystick devices**.

### Per-Device Setup
1. Checks if device GUID already registered (skips duplicates)
2. Records force-feedback capability from `param_2[0]`: 0 = no FF, 1 = FF capable
3. Copies instance GUID (16 bytes) to `g_joystickInstanceGuidTable[index*0x1C]`
4. Creates device via `IDirectInput::CreateDevice()`
5. Sets data format to joystick (`c_dfDIJoystick` at `0x1001dc80`)
6. Sets cooperative level with app window handle
7. Configures axis properties:
   - **Range**: Sets X and Y axis range to 0xFA (250) in each direction. Stored at `DAT_1005acd4 + index*0x70`
   - **Dead zone**: Sets to 0x19 (25 = 2.5%) per axis. Stored at `DAT_1005acd8 + index*0x70`
8. Reads device type from DIDEVCAPS (`offset + 0x24`) -> `DXInput::JoystickType[index]`
9. Reads button count from DIDEVCAPS -> `DXInput::JoystickButtonC[index]`, capped at 10
10. If FF-capable: sets auto-center property (property ID 9, value 0x14 = 20)
11. Increments `DXInput::JoystickC`, stops enumeration when count reaches 2

### Error Handling
On any property/format/cooperative-level failure:
- Logs specific error message
- Releases the device
- Returns false (stop enumeration) or true (continue) depending on error severity

---

## Force Feedback System

### PlayEffect (2 overloads)

**`DXInput::PlayEffect(joystick, effect, gain)`** at `0x10009b10`:
- Looks up `g_joystickEffectHandleTable[joystick*0x1C + effect]` -- IDirectInputEffect pointer
- Builds a 52-byte (0x34) DIEFFECT structure, zeroed, with size header
- Sets gain from `param_3` via `__ftol()` (float-to-long conversion)
- Calls `IDirectInputEffect::SetParameters()` with flags=4 (DIEP_GAIN)
- Calls `IDirectInputEffect::Start(1, 0)` -- single iteration, no flags
- Marks `g_joystickEffectActiveTable[joystick*0x1C + effect] = 1`

**`DXInput::PlayEffect(joystick, effect, gain, iterations)`** at `0x10009bb0`:
- Same as above but also sets `DIEFFECT.dwDuration = param_4`
- Uses flags=5 (DIEP_GAIN | DIEP_DURATION)
- Allows repeated playback

### SetEffect

**`DXInput::SetEffect(joystick, effect, gain, param4)`** at `0x10009ca0`:
- Same parameter setup as PlayEffect but **does NOT call Start()**
- Only calls `SetParameters()` with flags=4 -- updates the effect in place without triggering it
- Used for dynamic real-time force updates (e.g., road surface rumble) while the effect is already running

### Effect Table Layout
- `g_joystickEffectHandleTable`: Array indexed by `[joystick * 0x1C + effectSlot]` -- max 28 effect slots per joystick
- `g_joystickEffectActiveTable`: Parallel array tracking which effects are currently playing
- The EXE's `CreateRaceForceFeedbackEffects` (0x4285b0) installs 3 effects per joystick

---

## M2DX Startup Configuration

### ParseStartupConfiguration (0x10012540)

**Entry point** for all startup option parsing. Called with the command line string.

### Parse Order
1. Look for a command file argument (file path token)
2. If found AND logging is not already preferred: open file, parse via `ParseStartupConfigFile`
3. If no command file found: try `TD5.ini` as default config file
4. Parse remaining command-line tokens via `ApplyStartupOptionToken` in a loop
5. If logging was enabled (`DXWin::bLogPrefered`), emit system info header and re-parse (allows log token to take effect before other tokens)

### ParseStartupConfigFile (0x10012680)

Reads file contents, null-terminates, tokenizes with separator characters `;`, `\`, `//`, `rem`, `#`, and feeds each token to `ApplyStartupOptionToken`. If a token returns 0, it skips to the next line.

### ApplyStartupOptionToken (0x10012750) -- The 26 Tokens

The token table is a 26-entry string pointer array at `0x10027bf8`. Tokens are matched case-insensitively:

| Index | Token | Effect |
|-------|-------|--------|
| 0 | `rem` | Comment -- returns 0 (skip line) |
| 1 | `//` | Comment -- returns 0 (skip line) |
| 2 | `;` | Comment -- returns 0 (skip line) |
| 3 | `\` | Comment -- returns 0 (skip line) |
| 4 | `#` | Comment -- returns 0 (skip line) |
| 5 | `Log` | `DXWin::bLogPrefered = 1` -- enables TD5.log output |
| 6 | `TimeDemo` | `DAT_10061c40 = 1` -- benchmark/timedemo mode |
| 7 | `FullScreen` | `DAT_10061c1c = 1` -- force fullscreen at startup |
| 8 | `Window` | `DAT_10061c1c = 0` -- force windowed at startup |
| 9 | `NoTripleBuffer` | `g_tripleBufferState = 4` -- disable triple buffering |
| 10 | `DoubleBuffer` | `g_tripleBufferState = 4` -- same effect as NoTripleBuffer |
| 11 | `NoWBuffer` | `g_worldTransformState = 4` -- disable W-buffer depth |
| 12 | `Primary` | `g_adapterSelectionOverride = 1` -- force primary display adapter |
| 13 | `DeviceSelect` | `g_adapterSelectionOverride = -1` -- show device selection dialog |
| 14 | `NoMovie` | `DAT_10061c0c = 4` -- skip intro movie |
| 15 | `FixTransparent` | `DAT_100294b4 = 1` -- workaround for transparency rendering bugs |
| 16 | `FixMovie` | `DAT_10061c44 = 1` -- workaround for movie playback issues |
| 17 | `NoAGP` | `DAT_1003266c = 4` -- disable AGP texture memory |
| 18 | `NoPrimaryTest` | `g_skipPrimaryDriverTestState = 1` -- skip primary surface validation |
| 19 | `NoMultiMon` | `g_ddrawEnumerateExState = 4` -- disable multi-monitor enumeration |
| 20 | `NoMIP` | `g_mipFilterState = 4` -- disable mipmapping |
| 21 | `NoLODBias` | `g_lodBiasState = 4` -- disable LOD bias adjustment |
| 22 | `NoZBias` | `g_zBiasState = 4` -- disable Z-bias for coplanar polygons |
| 23 | `MatchBitDepth` | `g_matchBitDepthState = 1` -- match desktop bit depth |
| 24 | `WeakForceFeedback` | `DXInput::FFGainScale = 0.5` -- half-strength FF effects |
| 25 | `StrongForceFeedback` | `DXInput::FFGainScale = 1.0` -- full-strength FF effects |

### Token Categories

**Comment tokens** (0-4): Cause the parser to skip the rest of the current line.

**Display mode tokens** (7-8, 12-13, 19, 23):
- `FullScreen` / `Window` -- mutually exclusive startup mode
- `Primary` / `DeviceSelect` -- adapter selection override
- `NoMultiMon` -- disables DirectDraw EnumerateEx for multi-monitor
- `MatchBitDepth` -- forces render bit depth to match desktop

**Rendering quality tokens** (9-11, 15, 17, 20-22):
- `NoTripleBuffer` / `DoubleBuffer` -- same effect, force double buffering
- `NoWBuffer` -- disable W-buffer (use Z-buffer instead)
- `FixTransparent` -- enables a transparency rendering fix
- `NoAGP` -- disable AGP texture acceleration
- `NoMIP` -- disable mipmapping entirely
- `NoLODBias` -- disable LOD bias
- `NoZBias` -- disable Z-bias for overlapping geometry

**Movie tokens** (14, 16):
- `NoMovie` -- skip intro movie playback
- `FixMovie` -- workaround for movie decoder issues

**Diagnostics** (5-6, 18):
- `Log` -- enable logging to TD5.log
- `TimeDemo` -- benchmark mode
- `NoPrimaryTest` -- skip surface compatibility test

**Force feedback** (24-25):
- `WeakForceFeedback` -- 50% gain scale
- `StrongForceFeedback` -- 100% gain scale

### State Value Convention
Many token globals use `4` as the "disabled" sentinel rather than simple booleans. This is likely a tri-state: 0 = default/auto, 1 = force-enabled, 4 = force-disabled.

---

## M2DX Window Initialization

**Function**: `DXWin::Initialize` at `0x10012a10` (M2DX.dll)

### Window Class
- Class name: `"Test Drive 5"`
- Window title: `"Test Drive 5"`
- Style: `CS_HREDRAW | CS_VREDRAW` (0x03)
- Icon: resource ID 0x7C (124)
- Extended style: `WS_EX_APPWINDOW` (0x40000)
- Window style: `WS_POPUP` (0x80000000)
- Initial size: `g_windowedRenderWidth` x `g_windowedRenderHeight`
- Initial position: `CW_USEDEFAULT` (-0x80000000)
- Background: `WHITE_BRUSH` (stock object 4)
- Accelerator table: resource ID 0x71 (113)

### Initialization Steps
1. Load standard arrow cursor (`IDC_ARROW`)
2. Register window class via `RegisterClassExA`
3. Load accelerator table
4. Set `g_isChangingCooperativeLevel = 1` (suppresses WM_ACTIVATE processing)
5. Create window with `CreateWindowExA`
6. Hide hardware cursor via `ShowCursor(0)`
7. Query OS version via `GetVersionExA`
8. Clear cooperative level flag
9. Capture desktop bit depth: `GetDeviceCaps(hdc, BITSPIXEL) * GetDeviceCaps(hdc, PLANES)`
10. Copy initial render dimensions to windowed backup
11. Set `g_isFullscreenCooperative = 0`
12. Show and update window

---

## M2DX Window Message Handler

**Function**: `MainWindowProc` at `0x10012cd0` (M2DX.dll)

### Message Routing

1. **DXDraw messages**: First delegates to `HandleDXDrawWindowMessage`. If it returns 0, proceed to destruction path.
2. **WM_DESTROY (0x02)** and **DXDraw rejection**: Calls pre-destroy callback (`DAT_10061bfc`), then post-destroy callback (`DAT_10061c00`), then tears down all subsystems in order:
   - `DXSound::CDStop()` + `DXSound::Destroy()`
   - `DXPlay::Destroy()`
   - `DXInput::Destroy()`
   - `DXD3D::Destroy()`
   - `DXDraw::Destroy()`
   - `CoUninitialize()`
   - `PostQuitMessage(0)`
3. **WM_CLOSE (0x10)**: Calls close callback (`DAT_10061c08`), sets quit flag `DAT_10061c5c = 1`
4. **WM_CHAR (0x102)**: If `DXInput::bAnsi` is set, stores character in 256-byte ring buffer `DXInput::AnsiBuffer[AnsiP++]`
5. **WM_SETCURSOR (0x20)**: In fullscreen + not paused: hides cursor (`SetCursor(NULL)`), returns 1
6. **WM_TIMER (0x113), timer ID 1**: Triggers `DXPlay::EnumerateSessions()` if not already busy (network session browser timer)
7. **WM_APP+0x3B9 (0x3B9), wParam 1**: CD audio replay notification -> `DXSound::CDReplay()`

### Subsystem Teardown Order
The destructor chain is deterministic: Sound -> Network -> Input -> 3D -> Draw -> COM. This prevents dangling references between subsystems.

---

## ConfigureGameTypeFlags

**Function**: `ConfigureGameTypeFlags` at `0x410ca0` (TD5_d3d.exe)

Maps `g_selectedGameType` (1-9) to runtime mode flags:

| Type | `gRaceRuleVariant` | `gRaceDifficultyTier` | Traffic | Encounters | Special |
|------|--------------------|-----------------------|---------|------------|---------|
| 1 (Single Race) | 0 | 0 | Yes | Yes | `DAT_004aaf74=2` |
| 2 (Cup/Championship) | 5 | 0 | Yes | Yes | 4 laps, `DAT_004aaf74=2` |
| 3 (Arcade) | 1 | 1 | Yes | Yes | `DAT_004aaf74=2` |
| 4 (Tournament) | 2 | 1 | Yes | Yes | `DAT_004aaf74=2` |
| 5 (World Tour) | 3 | 1 | Yes | Yes | Random track order (15 tracks) |
| 6 (Elite) | 4 | 2 | Yes | Yes | `DAT_004aaf74=2` |
| 7 (Time Trial) | - | 2 | No | No | `gTimeTrialModeEnabled=1`, `DAT_004aaf74=3` |
| 8 (Cop Chase) | - | - | Yes | No | `gWantedModeEnabled=1`, `DAT_004aaf74=4` |
| 9 (Drag Race) | - | - | - | - | `gDragRaceModeEnabled=1`, `DAT_004aaf74=1` |

### Two-Player Mode
`DAT_004aaf74 = -(DAT_004962a0 != 0) & 5` -- if 2-player active, sets to 5 (overridden per game type).

### World Tour Track Randomization (type 5, first race)
Generates a random permutation of 27 tracks (indices 0-26), then randomly marks 5 of the 15 slots as "active" rounds.

### Anti-Cheat
`DAT_00490ba8` is loaded from a per-game-type/per-race lookup table at `s_Steve_Snake_says___No_Cheating__00464084` (offset by `g_raceWithinSeriesIndex + g_selectedGameType * 0x10 + 0x14`). If the value equals 99, the game calls a function pointer table at `DAT_00464104[g_selectedGameType]` -- likely a cheat detection or special handling path.

---

## Key Data Structure Summary

### Frontend Option Shadow Variables

| Address | Name | Options Screen | Range | Purpose |
|---------|------|----------------|-------|---------|
| `0x466000` | Circuit laps | Game | 0-3 | Number of laps for circuit races |
| `0x466004` | Checkpoint timers | Game | 0-1 | On/off |
| `0x466008` | Traffic | Game | 0-1 | On/off |
| `0x46600C` | Cops (encounters) | Game | 0-1 | On/off |
| `0x466010` | Difficulty | Game | 0-2 | Easy/Medium/Hard |
| `0x466014` | Dynamics | Game | 0-1 | Arcade/Simulation |
| `0x466018` | 3D collisions | Game | 0-1 | On/off |
| `0x466024` | Fogging | Display | 0-1 | On/off |
| `0x466028` | Speed readout | Display | 0-1 | MPH/KPH |
| `0x46602c` | Camera damping | Display | 0-9 | Smoothing factor |
| `0x465fe8` | SFX mode | Sound | 0-2 | Mono/Stereo/3D |
| `0x465fec` | SFX volume | Sound | 0-100 | Percentage |
| `0x465ff0` | Music volume | Sound | 0-100 | Percentage |
| `0x497a58` | P1 input source | Control | 0-7 | Device slot index |
| `0x465ff4` | P2 input source | Control | 0-7 | Device slot index |

### Input Device Descriptor Table

`DAT_00465660[slot]` -- 8 entries, one per device slot:

| Low byte | Device type |
|----------|-------------|
| 0x00 | Empty/no device |
| 0x03 | Keyboard |
| 0x04 | Joystick |
| 0xFF | Disconnected |

High byte (for joystick type 0x04):
| Value | Sub-type |
|-------|----------|
| 0x00 | Standard joystick |
| 0x04 | Gamepad |
| 0x06 | Wheel with pedals |

### Controller Binding Table

`DAT_00463fc8[player*9 + axis]` -- 2 players x 9 axes:
- Values 2-10 map to game actions via `SNK_ControlText[value * 0x10]`
- Written by joystick binding UI (cycling) or keyboard binding UI (sequential scan code capture)
- Read by `DXInput::SetConfiguration` at race start to compile runtime input maps
