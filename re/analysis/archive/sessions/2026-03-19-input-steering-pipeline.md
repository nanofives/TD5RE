# Test Drive 5: Input & Steering Pipeline

## Overview

The input system has three layers:
1. **Hardware polling** (M2DX.DLL / DXInput) -- reads DirectInput keyboard/joystick state
2. **Control bitmask** (TD5_d3d.exe / PollRaceSessionInput) -- packs device state into a per-player DWORD
3. **Vehicle control interpreter** (TD5_d3d.exe / UpdatePlayerVehicleControlState) -- decodes bitmask into steering_command, throttle, brake, gear changes

## Layer 1: Hardware Polling (M2DX.DLL)

### Keyboard: `DXInput::GetKBStick` (0x1000a400)

Reads 256-byte DirectInput keyboard state buffer at `0x1005af14` via `IDirectInputDevice::GetDeviceState`.

**Default layout** (non-custom mode, arrow keys):
| Scan Code | Key | Bit |
|-----------|-----|-----|
| 0xC8 | Up Arrow | 0x200 (accelerate) |
| 0xD0 | Down Arrow | 0x400 (brake) |
| 0xCB | Left Arrow | 0x01 (steer left) |
| 0xCD | Right Arrow | 0x02 (steer right) |
| 0x1C | Enter | 0x40000 (button) |

**Custom mode** (10 configurable actions):
| Bit | Action |
|-----|--------|
| 0x01 | Steer Left |
| 0x02 | Steer Right |
| 0x200 | Accelerate |
| 0x400 | Brake |
| 0x100000 | Handbrake |
| 0x200000 | Horn / Siren |
| 0x400000 | Gear Up |
| 0x800000 | Gear Down |
| 0x1000000 | Change Camera |
| 0x2000000 | Rear View |

**Always-on keys** (regardless of custom mode):
| Scan Code | Key | Bit |
|-----------|-----|-----|
| 0x19 | P | 0x20000000 (pause) |
| 0x01 | Escape | 0x40000000 (escape/menu) |

Keyboard input is **purely digital** -- no analog values, just on/off per key.

### Joystick: `DXInput::GetJS` (0x1000a1e0)

Reads `DIJOYSTATE` (0x50 bytes) via `IDirectInputDevice::GetDeviceState`. Supports up to **2 joysticks** (hard limit in `EnumerateJoystickDeviceCallback`).

**Default mode** (non-custom):
- X axis: threshold +/-0x50 maps to digital RIGHT (bit 0x02) / LEFT (bit 0x01)
- Y axis: threshold +/-0x50 maps to digital BRAKE (bit 0x400) / ACCEL (bit 0x200)
- Buttons: linear mapping from bit 0x40000 upward (button 0 = 0x40000, button 1 = 0x80000, etc.)

**Custom mode** has two sub-modes per axis:

1. **Analog packed mode** (X axis): When X-negative binding == 0x01 and X-positive binding == 0x02 (the standard left/right pair), and both appear exactly once in the config, the axis value is packed into **bits 0-8** of the DWORD with bit 0x80000000 set:
   ```
   value = (raw_X + calibration_center) | 0x80000000
   ```
   This gives 9-bit signed analog steering (-0xFA to +0xFA range centered at 0xFA).

2. **Digital threshold mode**: If the axis is NOT in analog mode, it uses the same +/-0x50 threshold as default mode but maps to user-configured bits.

3. **Analog packed mode** (Y axis): Same logic but packed into **bits 9-17** with bit 0x08000000 set:
   ```
   value = (raw_Y + calibration_center) * 0x200 | 0x08000000
   ```
   This gives 9-bit signed analog throttle/brake.

**Joystick deadzone:** Fixed threshold of **0x50** (decimal 80 out of +/-250 range = ~32% deadzone) for digital mode. No deadzone in analog packed mode (raw value passed through).

**Axis calibration:** Center value stored at `DAT_1005acd4 + joystick_index * 0x70`, initialized to **0xFA** (250) during enumeration. This is the midpoint offset added to raw axis values.

**Button cap:** Max 10 buttons per joystick (clamped in enumeration callback).

### Joystick Enumeration: `EnumerateJoystickDeviceCallback` (0x10009d70)

- Uses `IDirectInput::EnumDevices` with callback
- Detects force-feedback capability from `dwDevType` parameter
- Per-joystick record: 0x70 bytes at `0x1005acc8 + index * 0x70`
  - +0x00..0x0F: Instance GUID
  - +0x0C: Calibration center (0xFA default)
  - +0x10: Unknown
  - +0x3C: Force-feedback flag (0 or 1)
  - +0x40..0x6B: Effect handles (up to 0x1C = 28 slots, 10 per joystick used)
  - +0x6C: Effect active flags
- Sets cooperative level, data format (DIJOYSTATE), axis range (-250 to +250), deadzone (20%)
- Max 2 joysticks accepted

### Mouse: `DXInput::GetMouse` (0x10009790)

Reads `DIMOUSESTATE` (0x10 bytes) -- relative X/Y + button. Used for **frontend cursor only**, NOT for vehicle control.

## Layer 2: Control Bitmask (TD5_d3d.exe)

### `PollRaceSessionInput` (0x0042c470)

Called each frame from `RunRaceFrame`. Bridges hardware input to the game's per-player control DWORD array.

**Global:** `gPlayerControlBits` at **0x00482ffc** (DWORD[2], one per player)

**Device source selection:**
- `DAT_00497a58` = Player 1 input source (0 = keyboard, 1+ = joystick index+1)
- `DAT_00465ff4` = Player 2 input source

**Flow:**
1. For each active player (1 or 2 based on `DAT_004962a0`):
   - If source == 0: call `DXInputGetKBStick(0)` -> keyboard bitmask
   - If source > 0: call `DXInputGetJS(source - 1)` -> joystick bitmask
   - Store result in `gPlayerControlBits[player]`
2. If player has pending nitro flag (`DAT_0048f378/0048f37c`), OR in bit 0x10000000
3. Handle camera change (bit 0x1000000) with 10-frame cooldown
4. Handle rear view (bit 0x2000000)
5. In network play, exchange bitmasks via `DXPlay::HandlePadHost/Client`
6. Update force feedback via `UpdateControllerForceFeedback`
7. Record input via `DXInput::Write` (delta-compressed timeline)
8. Check F3/F4 keys (scan codes 0x3E/0x3F) for keyboard camera cycling
9. Check Escape (bit 0x40000000) to trigger race exit fade

**Input playback mode** (`g_inputPlaybackActive != 0`):
- Reads pre-recorded input via `DXInput::Read(&gPlayerControlBits)` instead of polling hardware
- Still checks Escape via keyboard for manual abort

## Complete Bitmask Format

```
Bit 31 (0x80000000): Analog X-axis flag (joystick only)
  Bits 0-8: Packed 9-bit X-axis value (0x00-0x1F4, center=0xFA)
Bit 27 (0x08000000): Analog Y-axis flag (joystick only)
  Bits 9-17: Packed 9-bit Y-axis value (0x00-0x1F4, center=0xFA)

When analog flags are CLEAR (keyboard or digital joystick):
  Bit 0  (0x01):       Steer Left
  Bit 1  (0x02):       Steer Right
  Bit 9  (0x200):      Accelerate
  Bit 10 (0x400):      Brake / Reverse
  Bit 18 (0x40000):    Button 0 (default joystick)
  Bit 19 (0x80000):    Button 1
  Bit 20 (0x100000):   Handbrake
  Bit 21 (0x200000):   Horn / Siren / NOS
  Bit 22 (0x400000):   Gear Up
  Bit 23 (0x800000):   Gear Down
  Bit 24 (0x1000000):  Change Camera View
  Bit 25 (0x2000000):  Rear View
  Bit 28 (0x10000000): NOS / Nitro (set by game logic, not input)
  Bit 29 (0x20000000): Pause (P key, always from keyboard)
  Bit 30 (0x40000000): Escape / Menu
```

## Layer 3: Vehicle Control Interpreter

### `UpdatePlayerVehicleControlState` (0x00402e60)

Called from `RunRaceFrame` at 0x0042b97d. Consumes `gPlayerControlBits[player]` and writes to actor struct fields.

**Two completely different code paths based on bit 31:**

### Path A: Digital Input (keyboard or digital joystick, bit 31 clear)

**Steering model -- progressive ramp with speed-dependent limits:**

1. **Speed-dependent steering rate** (`iVar5`):
   ```
   speed = abs(velocity >> 8)
   max_turn_rate_denominator = speed * 0x40 / (speed_sq + 0x40)   [when not stopped]
   steer_rate = 0xC0000 / (max_turn_rate_denominator + 0x40)
   ```
   At low speed, `steer_rate` is large (fast steering). At high speed, steering rate decreases.

2. **Speed-dependent steering limit** (`local_10` / `iVar6`):
   ```
   steer_limit = 0xC00000 / (speed + 0x80)
   ```
   Clamped to +/- 0x18000 (absolute max). Higher speed = smaller max steering angle.

3. **Path correction bias** (`iVar7`):
   Computes angle between current heading and track path vector, scaled by a speed-dependent factor. This provides an auto-alignment tendency.

4. **Left key pressed (bit 0x02)**:
   - Ramp-up counter at `gap_0338+2` increments by 0x40 per frame, caps at 0x100
   - If already steering right (`steering_command > 0`): snap-correct by `steer_rate * -2`
   - If steering left or neutral: increment `steering_command` by `(ramp * steer_rate) >> 8`
   - Clamp to negative limit

5. **Right key pressed (bit 0x01)**: Mirror of left

6. **No steering input (bits 0-1 both clear)**:
   - Ramp-up counter decays by 0x40 per frame
   - Auto-centering at **4x steer_rate**: if `abs(steering_command) < steer_rate * 4`, snap to 0
   - Otherwise decay toward zero at `steer_rate * 4`

**This means digital steering has:**
- Acceleration ramp (takes ~4 frames = 0x100/0x40 to reach full rate)
- Speed-dependent rate limiting (slower steering at higher speeds)
- Speed-dependent maximum angle
- Fast auto-centering at 4x the turn rate
- Track path correction bias (subtle auto-steer toward road)

### Path B: Analog Input (joystick with packed X-axis, bit 31 set)

```c
raw_x = (bitmask & 0x1FF) - 0xFA;  // Signed: -250 to +250, center=0
// Deadzone: none in decoding (raw value used directly)
steering_command = raw_x * steer_limit / large_divisor;  // Proportional to axis position
```

The analog path uses the **same speed-dependent steering limit** but maps the axis position directly to a proportional steering angle. No ramp-up, no auto-centering -- pure proportional control.

### Throttle/Brake (analog Y-axis, bit 27 set)

```c
raw_y = ((bitmask >> 9) & 0x1FF) - 0xFA;  // Signed
// Deadzone: values in range (-10, +10) are treated as zero
if (raw_y >= 10): throttle only (positive = brake)
if (raw_y <= -10): brake only (negative = accelerate) [signs inverted from steering]
```

When bit 27 is clear (digital mode):
- Bit 0x200 = Accelerate (on/off)
- Bit 0x400 = Brake (on/off)

**Throttle is binary in digital mode** -- no analog throttle from keyboard. The encounter steering override at `encounter_steering_override` and the special encounter system can override normal control.

### Gear System

Gear changes use a **5-frame debounce buffer** (`gPlayerControlBuffer[player]`):
- Bit 0x400000 (Gear Up): increment gear index (caps at max defined in car params)
- Bit 0x800000 (Gear Down): decrement gear index, with RPM-based downshift protection
- Gear 0 = reverse, gear 1 = first, etc.
- At gear 2+, downshift is blocked if RPM would exceed redline
- `ApplyReverseGearThrottleSign` flips the throttle sign when in reverse gear

### Handbrake

Bit 0x100000: Sets encounter state flags that modify the brake/grip model.
Only activates when specific conditions met (not in reverse, speed < 10, not stopped).

### Horn/Siren/NOS

Bit 0x200000: When NOS is enabled (`DAT_004aaf7c != 0`), triggers a one-shot velocity boost:
- Doubles path vectors (X and Y velocity components)
- Adds 0x6400 to vertical component
- 15-frame cooldown (`gap_0376[6]`)
- In cop mode (`DAT_0049629c`), zeroes all OTHER actors' velocities (freeze opponents)

## Layer 4: Steering to Physics

### `UpdatePlayerVehicleDynamics` (0x00404030)

Consumes `steering_command` from the actor struct:
- `param_1[0xFA]` = steering angle in fixed-point
- Used to compute wheel contact frame rotation via `CosFixed12bit`/`SinFixed12bit`
- Applied to front wheel force vectors to create lateral grip forces
- Differential torque applied through drivetrain type (RWD/FWD/AWD)

### `ApplySteeringTorqueToWheels` (0x0042eea0)

Called during gear shifts. Applies a torque impulse to all four wheels:
- Front wheels: +torque
- Rear wheels: -torque
- Torque = `steering_command * car_param_0x68 * 0x1A / 256 * drivetrain_factor`

### `UpdateActorSteeringBias` (0x004340c0)

Used by **AI actors** (not human players). Provides speed-scaled steering with:
- Same progressive ramp-up system as digital player steering
- Optional sine-smoothed small-angle corrections
- Used for AI pathfinding corrections and encounter steering

### `UpdatePlayerSteeringWeightBalance` (0x004036b0)

Two-player mode only. Computes per-player steering weight at `DAT_0046317c/7e`:
- Based on relative positions of the two players
- Trailing player gets enhanced steering authority (up to 0x100 + offset)
- Leading player gets reduced steering (0x100 - offset)

## Force Feedback System

### Setup: `CreateRaceForceFeedbackEffects` (0x004285b0)

Installed as the `CreateEffects` callback pointer in M2DX.DLL. Called when joystick is first acquired. Creates three DirectInput force-feedback effect objects per joystick:

1. **Effect slot 0** (js_exref + 0x20): Constant force -- steering resistance
2. **Effect slot 1** (js_exref + 0x24): Constant force -- secondary
3. **Effect slot 3** (js_exref + 0x28): Spring/damper -- collision feedback

### Runtime: `UpdateControllerForceFeedback` (0x004288e0)

Called every frame from `PollRaceSessionInput`:

**Steering resistance** (effect slot 0):
```c
lateral_force = abs(actor.gap_0310 + 0x08);  // Lateral velocity
if (lateral_force > 300000) lateral_force = 300000;
magnitude = float(lateral_force);  // -> PlayEffect or SetEffect
```
This creates resistance proportional to lateral velocity (cornering force feedback).

**Collision feedback** (effect slot 3):
```c
// gap_0346[0x2a] = collision state index
base_strength = lookup_table[collision_state];
magnitude = (30 - lateral_force / 10000) * base_strength;
```
Pulsed when collisions occur. If the effect is already playing, uses a fixed magnitude of 400.

### `PlayAssignedControllerEffect` (0x00428a10)

Dispatches one-shot effects (e.g., collision impacts) to the assigned controller. Validates the assignment table at `DAT_004a2cc4[player]` and capability flag before calling `DXInput::PlayEffect`.

### `ConfigureForceFeedbackControllers` (0x00428880)

Stores per-player controller assignments for force feedback. In network play, only the local participant slot gets an assignment.

## Input Configuration & Persistence

### `DXInput::SetConfiguration` (0x100098f0)

Called at race start. Takes player 1 and player 2 source indices, builds the live custom input map:
- Source 0 = keyboard: copies 16 scan code bindings from `Configure` block
- Source 1-2 = joystick: builds axis mode tables + button bit tables from frontend config arrays
- Sets `g_isCustomInputConfigEnabled = 1`

### `DXInput::ResetConfiguration` (0x10009b00)

Disables custom configuration, reverting to default arrow-key/threshold mapping. Used during modal overlays and shutdown.

### Controller Binding Menu

`ScreenControlOptions` (0x0041df20) presents:
- Player 1 source selection (Keyboard / Joystick 1 / Joystick 2 / None)
- Player 2 source selection
- Configure button -> `RenderControllerBindingMenuPage` for detailed per-action binding
- Source encoding in `DAT_00465660[source_index]`:
  - 0x03 = Keyboard
  - 0x04xx = Joystick (xx encodes specific stick)
  - 0xFFFFFFFF = None/disabled

### Input Recording/Playback

**Recording** (`DXInput::Write` at 0x1000a660):
- Delta-compressed timeline: only emits entries when DWORD changes
- Two independent channels (word 0 = player 1, word 1 = player 2)
- Frame index in bits 0-14, channel flag in bit 15
- Max ~20,000 entries / ~32,000 frames
- Strips camera/escape bits (AND 0xBBFFFFFF) to avoid replaying UI commands

**Playback** (`DXInput::Read` at 0x1000a780):
- Steps through timeline, reconstructing per-frame DWORD pair
- Returns 0 when record is exhausted
- Used for demo/attract mode and "time trial ghost" replay

## Complete Frame Pipeline

```
RunRaceFrame (0x42b580)
  |
  +-- PollRaceSessionInput (0x42c470)
  |     |-- DXInputGetKBStick() or DXInputGetJS()  [M2DX.DLL]
  |     |     |-- IDirectInputDevice::Acquire + GetDeviceState
  |     |     |-- Pack into 32-bit bitmask
  |     |-- Store in gPlayerControlBits[0..1]
  |     |-- Network exchange (HandlePadHost/Client) if multiplayer
  |     |-- UpdateControllerForceFeedback()
  |     |-- DXInput::Write() for recording
  |     |-- Camera change / escape / pause handling
  |
  +-- UpdatePlayerVehicleControlState (0x402e60) [per human player]
  |     |-- Decode bitmask -> steering_command (digital ramp OR analog proportional)
  |     |-- Decode throttle/brake (digital on/off OR analog signed)
  |     |-- Gear up/down with 5-frame debounce
  |     |-- Handbrake logic
  |     |-- NOS boost logic
  |     |-- Special encounter control override
  |
  +-- UpdatePlayerVehicleDynamics (0x404030) [per human player]
        |-- Read steering_command -> wheel angle via sin/cos fixed-point
        |-- Compute per-wheel grip/slip from surface type + steering
        |-- Apply drive torque from gear curve through drivetrain
        |-- Integrate chassis forces
```

## Key Constants

| Value | Meaning |
|-------|---------|
| 0x50 | Joystick digital threshold (32% of range) |
| 0xFA (250) | Joystick axis center/calibration value |
| 0x18000 | Absolute max steering_command |
| 0xC0000 | Steering rate numerator |
| 0xC00000 | Steering limit numerator |
| 0x40 | Steering ramp-up step per frame |
| 0x100 | Max steering ramp-up accumulator |
| 4 | Auto-centering rate multiplier (vs turn rate) |
| 5 | Gear change debounce frames |
| 10 | Camera change cooldown frames |
| 0xFA | Analog Y-axis deadzone threshold (+/-10 raw) |

## Key Globals

| Address | Name | Description |
|---------|------|-------------|
| 0x00482ffc | gPlayerControlBits | DWORD[2], per-player packed input bitmask |
| 0x00482fe4 | gPlayerControlBuffer | int[6], gear-change debounce counters |
| 0x00483014 | DAT_00483014 | Per-player NOS latch accumulator |
| 0x00497a58 | DAT_00497a58 | Player 1 input source (0=KB, 1+=JS) |
| 0x00465ff4 | DAT_00465ff4 | Player 2 input source |
| 0x004a2cc4 | DAT_004a2cc4 | Force feedback controller assignment table [6] |
| 0x0046317c | DAT_0046317c | Player 1 steering weight (2P mode) |
| 0x0046317e | DAT_0046317e | Player 2 steering weight (2P mode) |
| 0x1005af14 | (M2DX) | 256-byte DirectInput keyboard state buffer |
| 0x1005b130 | g_pKeyboardDevice | IDirectInputDevice pointer |
| 0x1005b13c | g_isCustomInputConfigEnabled | Custom input config active flag |
| 0x1005add4 | g_customKeyboardLeftScanCode | First of 16 custom KB scan code slots |
