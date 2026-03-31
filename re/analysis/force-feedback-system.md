# Force Feedback Effect System

## Overview

TD5 implements force feedback through DirectInput's `IDirectInputDevice2::CreateEffect` /
`IDirectInputEffect` COM interfaces. The game creates up to 4 effect objects per
force-feedback-capable joystick and triggers them in response to steering, collisions, and
terrain changes. The framework layer (`M2DX.dll`) manages device enumeration and low-level
effect playback, while the EXE contains the race-specific logic that decides _when_ and
_how strongly_ to fire each effect.

## Architecture

```
 EXE (Race Logic)                          M2DX.dll (Framework)
 +---------------------------------+       +----------------------------------+
 | CreateRaceForceFeedbackEffects  |------>| DXInput::EnumerateEffects        |
 |   (0x4285B0)                   |       |   (0x10009D20)                   |
 |                                 |       |   -> IDirectInputDevice2::       |
 |                                 |       |      EnumEffects (vtable+0x4c)  |
 | ConfigureForceFeedbackControllers|      |                                  |
 |   (0x428880)                   |       | DXInput::PlayEffect (2 overloads)|
 |                                 |       |   (0x10009B10 / 0x10009BB0)     |
 | UpdateControllerForceFeedback   |------>| DXInput::SetEffect               |
 |   (0x4288E0)                   |       |   (0x10009CA0)                   |
 |                                 |       |                                  |
 | PlayAssignedControllerEffect    |------>| DXInput::StopEffects             |
 |   (0x428A10)                   |       |   (0x10009C60)                   |
 |                                 |       +----------------------------------+
 | PlayVehicleCollisionForceFeedback|
 |   (0x428A60)                   |
 +---------------------------------+
```

## Device Enumeration and Selection

### Joystick Discovery (`EnumerateJoystickDeviceCallback` at M2DX 0x10009D70)

The callback is invoked by `IDirectInput::EnumDevices`. Key behavior:

1. **Duplicate filtering**: Compares the incoming device instance GUID against all
   previously accepted joysticks in `g_joystickInstanceGuidTable` (stride 0x1c DWORDs =
   0x70 bytes per slot). Duplicates are silently skipped.

2. **Force-feedback detection**: The second callback parameter (`param_2`) indicates FF
   capability. When `*param_2 != 0`, the per-joystick flag at offset `+0x3C` in the
   0x70-byte metadata record (`DAT_1005ad04`) is set to `1`; otherwise `0`.
   - Log message: `"Joystick device %d found: ForceFeedback"` vs `"Not ForceFeedback"`.

3. **Device setup sequence**:
   - `IDirectInput::CreateDevice` (vtable+0x0c)
   - `SetDataFormat` (vtable+0x2c) with the standard joystick format (`c_dfDIJoystick`)
   - `SetCooperativeLevel` (vtable+0x34) using the application HWND (`DX::app.hWnd`)
   - `SetProperty` (vtable+0x18) called 4 times:
     - Property 4 (DIPROP_RANGE): axis range min/max
     - Property 4 again: second axis range
     - Property 5 (DIPROP_DEADZONE): applied twice (both axes)
   - `GetCapabilities` (vtable+0x0c): reads button count, capped at 10

4. **FF-specific property**: For FF-capable devices, an additional `SetProperty` call with
   property ID `9` (DIPROP_FFGAIN) and value `0x14` (= 20, i.e. 20% gain) is issued.

5. **Device limit**: Enumeration stops after accepting **2 joysticks** (`JoystickC + 1 < 2`
   return condition).

### Per-Joystick Data Layout (0x70-byte record at `g_joystickMetadata`)

| Offset | Size | Field |
|--------|------|-------|
| +0x00  | 16   | Instance GUID (copied from DIDEVICEINSTANCE) |
| +0x0C  | 4    | Axis range value 1 (default 0xFA = 250) |
| +0x10  | 4    | Axis range value 2 (default 0x19 = 25) |
| +0x3C  | 4    | Force-feedback capable flag (0 or 1) |
| +0x40  | 28   | Effect handle slots (7 x 4 bytes = IDirectInputEffect*) |
| +0x6C  | 4    | Active effect flags (parallel to handle slots) |

### Controller Assignment Table (`DAT_004A2CC4`, 6 entries)

A 6-element array mapping player slot index (0-5) to joystick number (1-based; 0 = no
controller). Written by `ConfigureForceFeedbackControllers`:
- **Local play**: Copies the assignment array directly from the race configuration.
- **Network play**: Only the local participant's slot (read from `DXPlay::dpu + 0xBE8`) is
  assigned.

All FF functions check `DAT_004A2CC4[playerSlot] != 0` and the FF capability flag at
`js_exref + (joystickIdx * 0x70) + 0x44` before dispatching effects.

## Effect Slot Definitions

`CreateRaceForceFeedbackEffects` (0x4285B0) creates effects into 4 slots per joystick,
stored at offsets +0x1C, +0x20, +0x24, and +0x28 within the per-joystick record in
`js_exref`.

### Joystick Type Detection

The function checks `(JoystickType[param_1] & 0xFF00) == 0x0600` to determine if the
device is a **wheel** (DI8DEVTYPE_DRIVING subtype). This flag modifies the axis count
used in effect creation:
- Wheel: `flags = 0x12` (single-axis, 18 = DIEFF_CARTESIAN with 1 axis)
- Gamepad: `flags = 0x22` (two-axis, 34 = DIEFF_CARTESIAN with 2 axes)

### Slot 0 -- Steering Resistance (Constant Force)

Created via `IDirectInputDevice2::CreateEffect` (vtable+0x48) with the first enumerated
effect GUID (from `EnumerateEffects` with `DIEFT_CONSTANTFORCE` = 3 as the type filter).

**DIEFFECT parameters:**
| Field | Value |
|-------|-------|
| dwSize | 0x34 (52 bytes, sizeof DIEFFECT) |
| dwFlags | 0x22 or 0x12 (DIEFF_CARTESIAN, axis count depends on device type) |
| dwDuration | 5000 us (5 ms) |
| dwGain | DI_FFNOMINALMAX (not explicitly set, defaults) |
| dwTriggerButton | DIEB_NOTRIGGER (0xFFFFFFFF) |
| dwTriggerRepeatInterval | 0 |
| cAxes | 2 (gamepad) or 1 (wheel) |
| dwDirection | 0 |
| lpEnvelope | Envelope structure (attack 1500 us, fade 50000 us) |
| cbTypeSpecificParams | 4 |
| lpvTypeSpecificParams | -> DICONSTANTFORCE (magnitude set dynamically) |

**Envelope:**
| Field | Value |
|-------|-------|
| dwSize | 0x10 (16 bytes) |
| dwAttackLevel | 0 |
| dwAttackTime | 0 |
| dwFadeLevel | 0x5DC (1500) |
| dwFadeTime | 50000 us |

### Slot 1 -- Collision Impact (Constant Force)

Created with the constant-force GUID (same as slot 0), stored at `js_exref + offset + 0x20`.

**DIEFFECT parameters:**
| Field | Value |
|-------|-------|
| dwSize | 0x34 |
| dwFlags | 0x22 / 0x12 (same device-type logic) |
| dwDuration | 0x30D40 (200,000 us = 200 ms) |
| dwGain | DI_FFNOMINALMAX |
| cAxes | device-dependent |
| rgdwAxes | 4 axes specification |
| lpvTypeSpecificParams | DICONSTANTFORCE magnitude (set at play time) |

### Slot 2 -- Side Impact (Constant Force)

Created with the same constant-force GUID, stored at `js_exref + offset + 0x24`.

**DIEFFECT parameters:**
| Field | Value |
|-------|-------|
| dwSize | 0 (minimal, inherits from device defaults) |
| cAxes | 4 |
| lpvTypeSpecificParams | DICONSTANTFORCE |

### Slot 3 -- Spring/Centering Effect

Created with `GUID_Spring` (`{13541C2A-8E33-11D0-9AD0-00A0C9A06E35}`) read from
`DAT_0045ea28`. Stored at `js_exref + offset + 0x28`.

**DIEFFECT parameters:**
| Field | Value |
|-------|-------|
| dwSize | 0x18 (condition-specific struct) |
| dwDuration | 0 (infinite) |
| dwGain | 0x1388 (5000) |
| dwSamplePeriod | 0x2710 (10000) |
| lpvTypeSpecificParams | DICONDITION struct |

**DICONDITION parameters:**
| Field | Value |
|-------|-------|
| lOffset | 0xFFFFEC78 (-5000, center offset) |
| lPositiveCoefficient | 10000 |
| lNegativeCoefficient | 0 |
| dwPositiveSaturation | 10000 |
| dwNegativeSaturation | 10000 |
| lDeadBand | 5000 |

## Effect Triggering Logic

### Continuous Steering Feedback (`UpdateControllerForceFeedback` at 0x4288E0)

Called every frame for each active player slot. Reads vehicle state from
`g_actorRuntimeState.slot.field_0x318` (steering force value).

**Logic:**
1. Read steering force from actor state at offset `+0x318` per player (stride 0x388).
2. If negative, negate and double it (asymmetric steering feel).
3. Clamp to maximum 300,000.
4. Convert to effect magnitude via `__ftol()` (float-to-long).
5. **First-time play vs update**:
   - If `js_exref + offset + 0x48 == 0` (effect not yet started): call
     `DXInput::PlayEffect(joystickIdx, 0, magnitude)` -- starts slot 0.
   - Otherwise: call `DXInput::SetEffect(joystickIdx, 0, magnitude, 0)` -- updates
     parameters of already-running effect.

**Terrain-dependent vibration (slot 3):**
1. Read terrain/surface type from actor state at offset `+0x370` per player.
2. Look up terrain coefficient from table at `DAT_00466AFC`:

| Surface Index | Coefficient |
|---------------|-------------|
| 0 | 1 |
| 1 | 200 |
| 2 | 260 |
| 3 | 180 |
| 4 | 100 |
| 5 | 600 |
| 6 | 50 |
| 7 | 240 |
| 8 | 400 |
| 9 | 400 |
| 10 | 200 |
| 11 | 360 |
| 12 | 190 |

3. Compute vibration magnitude: `(30 - steeringForce/10000) * terrainCoefficient`.
4. If effect slot 3 not yet started (`js_exref + 0x54 == 0`): play via
   `DXInput::PlayEffect(joystickIdx, 3, magnitude)`.
5. If already active and `DAT_004A2CDC[playerSlot] != 0` (collision recently active):
   override magnitude to **400** (dampen terrain vibration during collision recovery).
6. Otherwise: update via `DXInput::SetEffect(joystickIdx, 3, magnitude, 0)`.

### Collision Effects (`PlayVehicleCollisionForceFeedback` at 0x428A60)

Called from the physics collision resolver. Parameters:
- `param_1`: Contact side bitmask
- `param_2`: Pointer to vehicle A actor (player slot at offset +0x375)
- `param_3`: Pointer to vehicle B actor (player slot at offset +0x375)
- `param_4`: Raw collision magnitude

**Magnitude processing:**
- `magnitude = param_4 / 10`
- Clamped to maximum 10,000

**Contact-side dispatch (param_1 bitmask):**

| Bitmask | Meaning | Effect on Vehicle A | Effect on Vehicle B |
|---------|---------|--------------------|--------------------|
| 0x01, 0x02, 0x04, 0x08 | Front/rear contact sides | Slot 1 (frontal hit) | Slot 2 (side impact) |
| 0x10, 0x20, 0x40, 0x80 | Left/right contact sides | Slot 2 (side impact on A) | Slot 1 (frontal hit on B) |
| Default (other) | General collision | Slot 2 on both vehicles | Slot 2 on both vehicles |

Both vehicles involved in a collision receive feedback if their respective player slots
have assigned FF controllers.

### Generic Effect Dispatch (`PlayAssignedControllerEffect` at 0x428A10)

A thin helper used by other game systems to fire arbitrary effects:
```c
void PlayAssignedControllerEffect(int playerSlot, int effectSlot, int magnitude, int repeatCount)
```
- Validates `playerSlot < 6` and that a controller is assigned.
- Checks FF capability flag.
- Delegates to `DXInput::PlayEffect(joystickIdx, effectSlot, magnitude, repeatCount)`.

## M2DX.dll DirectInput Integration

### Effect Handle Table (`g_joystickEffectHandleTable`)

A flat array indexed by `effectSlot + joystickIdx * 0x1C` (28 effect slots per joystick,
though only ~4 are used by TD5). Each entry holds an `IDirectInputEffect*` pointer or NULL.

### Parallel Active Table (`g_joystickEffectActiveTable`)

Same indexing as the handle table. Set to `1` when an effect is playing, `0` when stopped.

### `DXInput::PlayEffect` (0x10009B10 -- 3 params)

Starts an effect with computed magnitude:
1. Looks up `IDirectInputEffect*` from handle table.
2. Builds a `DIEFFECT` struct (size 0x34), zeroed, with magnitude converted via `__ftol()`.
3. Calls `IDirectInputEffect::SetParameters` (vtable+0x18) with flags `DIEP_TYPESPECIFICPARAMS` (4).
4. Calls `IDirectInputEffect::Start` (vtable+0x1C) with iteration count = 1.
5. Marks the active table entry.

### `DXInput::PlayEffect` (0x10009BB0 -- 4 params)

Same as above but accepts an explicit `repeatCount` parameter:
- Sets `DIEFFECT.dwDuration = param_4` (repeat/duration override).
- Calls `SetParameters` with flags `DIEP_TYPESPECIFICPARAMS | DIEP_DURATION` (5).
- Calls `Start` with iteration count = 1.

### `DXInput::SetEffect` (0x10009CA0)

Updates parameters of an already-running effect _without_ restarting it:
1. Looks up `IDirectInputEffect*` from handle table.
2. Builds zeroed `DIEFFECT`, sets magnitude.
3. Calls `IDirectInputEffect::SetParameters` (vtable+0x18) with flags 4.
4. Does NOT call `Start` -- the effect continues with updated parameters.

### `DXInput::StopEffects` (0x10009C60)

Iterates all joystick slots, and for each of 10 effect sub-slots:
- If active flag is set, calls `IDirectInputEffect::Stop` (vtable+0x20).
- Clears the active flag.
- Continues until address exceeds `0x1005ADE8` (covering 2 joystick slots x 10 effects
  each, matching the 2-device limit).

### `DXInput::EnumerateEffects` (0x10009D20)

Wraps `IDirectInputDevice2::EnumEffects` (vtable+0x4c):
- `param_3` is the DirectInput effect type filter (`DIEFT_CONSTANTFORCE = 3`,
  `DIEFT_CONDITION = 1`).
- Uses `CopyEffectGuidCallback` (0x1000A1B0) which copies the 16-byte GUID from the
  `DIEFFECTINFO` structure (offset +4) into the caller's buffer.

## Summary of All Effects

| Slot | Type | GUID | Purpose | Trigger | Duration |
|------|------|------|---------|---------|----------|
| 0 | Constant Force | Enumerated (DIEFT_CONSTANTFORCE) | Steering resistance | Every frame, from vehicle steering state | 5 ms (re-triggered continuously) |
| 1 | Constant Force | Same | Frontal collision impact | On collision, front/rear contact | 200 ms |
| 2 | Constant Force | Same | Side collision impact | On collision, side contact or general | Inherited |
| 3 | Spring (Condition) | GUID_Spring {13541C2A-8E33-11D0-9AD0-00A0C9A06E35} | Terrain vibration / centering | Every frame, modulated by surface type | Infinite (updated continuously) |

## Key Constants

| Address | Value | Meaning |
|---------|-------|---------|
| 0x466AFC | int[13] | Terrain surface FF coefficient table |
| 0x45EA28 | GUID | GUID_Spring |
| 0x4A2CC4 | int[6] | Player-to-joystick assignment table |
| 0x4A2CDC | int[6] | Per-player collision-active flag (dampens terrain FF) |

## Device Type Adaptation

The `JoystickType` value (read via `GetCapabilities` during enumeration) is checked against
`0x0600` (DI8DEVTYPE_DRIVING / wheel). This adapts effect creation:
- **Wheels**: Single-axis effects (flags = 0x12), centering spring is meaningful.
- **Gamepads**: Two-axis effects (flags = 0x22), both X and Y axes receive force.

The FF gain is set to 20% (`DIPROP_FFGAIN = 0x14`) for all devices, keeping effects
relatively subtle.
