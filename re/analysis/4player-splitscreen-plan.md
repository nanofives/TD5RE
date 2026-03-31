# 4-Player Split-Screen Implementation Plan

> Status: PLANNED
> Prerequisite: All 31 widescreen patches applied, DDrawCompat v0.6.0

## Overview

Enable 4-player local split-screen (quad split) in TD5. The engine already supports 6 actor
slots and shared physics simulation -- the main barriers are the 2-player input layer in
M2DX.DLL, the hardcoded 2-view render loop, and the absence of 4-view viewport/HUD/particle
data structures.

Strategy: bypass M2DX for extra controllers, inject a generalized N-view render loop via code
cave, relocate expanded data tables, and activate 4P via config flag (skip frontend rework).

---

## Phase 1: Input Layer (M2DX.DLL + EXE hook)

### 1A. Expand DXInput joystick enumeration (M2DX.DLL)

**Location**: `EnumerateJoystickDeviceCallback` (0x10009D70)

The callback returns `JoystickC + 1 < 2` to stop after 2 devices.

- Patch `return iVar4 + 1 < 2` -> `return iVar4 + 1 < 4` (change CMP immediate from 2 to 4)
- Expand per-joystick data arrays from 2 to 4 slots:
  - `g_joystickInstanceGuidTable` (stride 0x1C dwords = 0x70 bytes) -- needs 2 more 0x70-byte slots
  - `DXInput::js` device handle array (stride 0x1C) -- needs 2 more slots
  - `DXInput::JoystickType` / `DXInput::JoystickButtonC` arrays -- extend to 4
  - Force-feedback flag at `DAT_1005ad04 + JoystickC * 0x70` -- verify 4 slots fit
- Verify all loops bounded by `DXInput::JoystickC` (dynamic) will work with values 3-4

**Risk**: DLL .data section may not have room for expanded arrays. May need to relocate to a
new section or use a DLL wrapper.

### 1B. Expand DXInput::SetConfiguration (M2DX.DLL)

**Location**: `DXInput::SetConfiguration` (0x100098F0)

Currently takes 2 int params (P1 source, P2 source), writes to `g_player1InputSource` /
`g_player2InputSource`, loops with stride 0x40 over 2 config slots.

- Option A (DLL patch): Add `g_player3InputSource` / `g_player4InputSource` globals, change
  loop termination bound from `0x1005acaf` to allow 4 iterations, extend config tables
  (`Configure`, `g_customJoystick*` tables) by 2 * 0x40 bytes each
- Option B (EXE bypass): Keep SetConfiguration for P1/P2. For P3/P4, poll DirectInput
  directly from an EXE-side hook and write results into `gPlayerControlBits[2]` / `[3]`.
  This avoids M2DX API changes but duplicates some input logic.

**Recommendation**: Option B is safer -- less DLL surgery, and the EXE already has the
control bit format defined. We just need to translate raw DirectInput axis/button state
into the game's 32-bit control bitmask format.

### 1C. Expand EXE input polling (TD5_d3d.exe)

**Location**: `PollRaceSessionInput` (0x42C470)

```c
local_8[0] = DAT_00497a58;  // P1 input source
local_8[1] = DAT_00465ff4;  // P2 input source
loop_count = (DAT_004962a0 != 0) + 1;  // max 2
```

Changes needed:
- Add P3/P4 input source globals (or use a 4-element array)
- Change loop count expression to support 1-4 players
- Extend camera-cycle key handling (indices 0-3 instead of 0-1)
- Extend force-feedback ownership handling (currently checks iVar1 == 0 or == 1)

**Addresses to patch**:
- Loop bound calculation near 0x42C4A0
- Camera cycle: `(&gPrimarySelectedSlot)[iVar1]` access (already indexed, just needs bound)
- Cooldown array `(&DAT_004aaf98)[iVar1]` -- verify space for indices 2,3
- `DAT_0048f378` / `DAT_0048f37c` (escape key flags) -- add P3/P4 equivalents or share

---

## Phase 2: Viewport & Projection

### 2A. Add quad-split viewport layout

**Location**: `InitializeRaceViewportLayout` (0x42C2B0)

Current layout table at 0x4AAE14: 3 modes * 0x40 bytes = 0xC0 bytes. No room to append
mode 3 in-place (globals at 0x4AAED4+ are referenced by RunRaceFrame).

The existing per-mode struct stores data for 2 sub-views using interleaved fields:
```
+0x00: view0_left    +0x04: view0_proj_cx   +0x08: view1_proj_cx
+0x0C: view0_right   +0x10: view1_right     +0x14: view0_top
+0x18: view1_top     +0x1C: view0_proj_cy   +0x20: view1_proj_cy
+0x24: view0_bottom  +0x28: view1_bottom    +0x2C: cull_window_max
+0x30: cull_window   +0x34: view_count
```

**Approach**: Allocate a NEW quad-split viewport structure (can be in a code cave or
injected .data section). For mode 3 (quad), store 4 sets of clip rects + projection
centers. The render loop (Phase 3) will read from this new structure when mode == 3.

Quad layout values (for resolution W x H):
```
View 0 (top-left):     left=0,          right=W/2-1,   top=0,          bottom=H/2-1
View 1 (top-right):    left=W/2+1,      right=W,       top=0,          bottom=H/2-1
View 2 (bottom-left):  left=0,          right=W/2-1,   top=H/2+1,      bottom=H
View 3 (bottom-right): left=W/2+1,      right=W,       top=H/2+1,      bottom=H
Projection centers:    (W/4, H/4), (3W/4, H/4), (W/4, 3H/4), (3W/4, 3H/4)
Cull window:           16 spans per view
View count:            4
```

### 2B. Extend gRaceViewportLayoutMode range

**Location**: `InitializeRaceSession` (0x42AA10)

Currently: `gRaceViewportLayoutMode = (char)DAT_00497a5c + 1` when DAT_004962a0 != 0.
DAT_00497a5c is the split mode (0=horiz, 1=vert).

- Add mode value 3 for quad-split
- This value gates rendering, HUD, culling, traffic disabling throughout the EXE
- All checks of `gRaceViewportLayoutMode` must be audited:
  - `!= 0` checks (split-screen active) -- still correct for mode 3
  - `== 1` / `== 2` checks (specific split mode) -- need `== 3` handling
  - Race end fade direction switch (0x42B670) -- add case for mode 3

---

## Phase 3: Render Loop Generalization

### 3A. Rewrite RunRaceFrame render passes

**Location**: `RunRaceFrame` (0x42B580), specifically the render section (~lines 250-450)

Current structure:
```
BeginRaceScene()
  [view 0: camera -> sky -> track -> actors -> particles -> translucent -> HUD]
  if (gRaceViewportLayoutMode != 0):
    [view 1: camera -> sky -> track -> actors -> particles -> translucent -> HUD]  (copy-paste)
  [final HUD overlay on view 0 rect]
EndRaceScene()
```

Target structure:
```
BeginRaceScene()
  for viewIndex = 0 to viewCount-1:
    SetProjectedClipRect(layout[viewIndex])
    SetProjectionCenterOffset(layout[viewIndex])
    UpdateCamera(slot[selectedSlot[viewIndex]], viewIndex)
    RenderSky()
    RenderTrackSpans(viewIndex)
    RenderRaceActorsForView(viewIndex)
    RenderTireTrackPool()
    RenderAmbientParticleStreaks(slot[selectedSlot[viewIndex]], tickBudget, viewIndex)
    DrawRaceParticleEffects(viewIndex)
    FlushQueuedTranslucentPrimitives()
    FlushProjectedPrimitiveBuckets()
    DrawRaceStatusText(slot[selectedSlot[viewIndex]], viewIndex)
  SetProjectedClipRect(fullScreenRect)
  RenderRaceHudOverlays(tickBudget)
EndRaceScene()
```

**Implementation**: This is too large to patch inline. Inject a new function in a code cave
(or new PE section) that implements the generalized loop. Replace the original render section
with a CALL to the new function + NOP sled.

The new function needs access to:
- Viewport layout data (new quad struct from Phase 2A)
- `gPrimarySelectedSlot` array (expanded in Phase 4A)
- `gRaceCameraPresetMode` array (expanded in Phase 4A)
- All existing render functions (sky, track, actors, etc.) -- called by address

### 3B. Generalize simulation tick camera updates

In the simulation tick loop (before rendering), camera caching is hardcoded for 2:
```c
CacheVehicleCameraAngles(slot[gPrimarySelectedSlot], 0);
CacheVehicleCameraAngles(slot[gSecondarySelectedSlot], 1);
```

And the UpdateChaseCamera loop already uses `viewCount` from the layout table:
```c
if (iVar2 < *(int*)(&DAT_004aae48 + gRaceViewportLayoutMode * 0x40))  // view_count
```

- Extend CacheVehicleCameraAngles to call for views 2 and 3
- The UpdateChaseCamera loop should work if `view_count = 4` in the new layout

### 3C. Track span display list allocation

Track span visibility is stored per-view in a flat array at `DAT_004aac60`:
```c
(&DAT_004aac60)[cull_window * viewIndex + spanIndex] = displayListEntry;
```

With cull_window=64 (fullscreen) or 32 (split), and 2 views max, current allocation is
64 * 2 = 128 entries (512 bytes). With 4 views * 16 spans = 64 entries -- fits in existing
space. Verify the allocation doesn't overflow at 0x4AAC60 + 64*4 = 0x4AAD60.

---

## Phase 4: Per-View Data Expansion

### 4A. Camera target and preset arrays

**Locations**:
- `gPrimarySelectedSlot` (0x466EA0) -- 2-element int array, extend to 4
- `gRaceCameraPresetMode` (0x482FD8) -- 2-element int array, extend to 4

Check adjacent globals:
- 0x466EA8: `DAT_00466ea8` (fog setting) -- would be overwritten if we extend in-place
- 0x482FE0: verify what's there

If no room, relocate to new addresses and patch all xrefs:
- gPrimarySelectedSlot: 31+ xrefs (all in EXE)
- gRaceCameraPresetMode: ~10 xrefs

### 4B. Particle system expansion

**Location**: 2 banks starting at ~0x4A318F, stride 0x1900, 100 slots each

Total current: 2 * 0x1900 = 0x3200 bytes
Need: 4 * 0x1900 = 0x6400 bytes (additional 0x3200 = 12,800 bytes)

- Check if there's free space after bank 1 (0x4A318F + 0x3200 = 0x4A638F onwards)
- If not, allocate new banks and patch `UpdateRaceParticleEffects` / `DrawRaceParticleEffects`
  base address calculation: `&DAT_004a31ac + param_1 * 0x1900`
- Add calls for banks 2 and 3 in both simulation tick and render pass

### 4C. Ambient particle streaks

`UpdateAmbientParticleDensityForSegment` and `RenderAmbientParticleStreaks` are called with
viewIndex 0 and 1. Extend to 0-3. Check if their internal data arrays support 4 indices.

### 4D. Tire track pool

`RenderTireTrackPool` is called once per view with no view parameter -- it renders globally.
Should work as-is for 4 views (renders same track marks in all views).

---

## Phase 5: HUD System

### 5A. Extend InitializeRaceHudLayout (0x437BA0)

Add case for `param_1 == 3` (quad split):
```c
case 3:
  DAT_004b1138 *= 0.5;  // scale_x halved
  DAT_004b113c *= 0.5;  // scale_y halved
  DAT_004b1134 = 4;     // view_count = 4
  // Set 4 view rects (top-left, top-right, bottom-left, bottom-right)
  // Each at quarter-screen bounds
```

The HUD element placement loop iterates `DAT_004b1134` times -- if we set it to 4 and
provide 4 sets of per-view rect data, it should generalize.

Per-view HUD data fields (currently 2 sets):
- View 0: `DAT_004b1148`-`DAT_004b1154` (left, top, right, bottom)
- View 1: `DAT_004b1180`-`DAT_004b118c`
- Need: Views 2-3 require 2 more sets of 4 floats = 32 bytes

### 5B. Extend RenderRaceHudOverlays (0x4388A0)

This function dispatches HUD rendering per-view. Currently handles view 0 and conditionally
view 1. Needs to iterate over 4 views for quad mode.

### 5C. Minimap

Minimap is disabled when `gRaceViewportLayoutMode != 0`. Stays disabled in 4P -- fine.

### 5D. Divider rendering

Currently renders a 1px divider line between split views. For quad, render a cross
(horizontal + vertical dividers). Hook into the divider rendering in
`InitializeRaceOverlayResources` (0x437800).

---

## Phase 6: Slot State & Activation

### 6A. Mark 4 slots as human players

**Location**: `InitializeRaceSession` (0x42AA10)

Current logic:
```c
uVar1 = (DAT_004962a0 != 0) + 1;  // 1 or 2 human players
// Set slots 0..uVar1-1 to state 0x01 (human)
// Remaining slots to state 0x00 (AI) or 0x03 (disabled)
```

Change: When 4P mode active, `uVar1 = 4`, mark slots 0-3 as state 0x01.

### 6B. Expand 2P mode gate

`DAT_004962a0`: Currently 0 (1P) or nonzero (2P selecting).

Options:
- Repurpose: 0=1P, 1=2P, 2=4P
- Or add a new global `g_playerCount` (1, 2, or 4)

All checks of `DAT_004962a0 != 0` throughout the codebase need auditing to ensure they
still correctly gate "split-screen is active" for 4P mode.

### 6C. Disable traffic and encounters

Already done when `gRaceViewportLayoutMode != 0` -- 4P inherits this automatically.

### 6D. Car assignment

Currently: `DAT_00497268` array maps slots 0-5 to car IDs (set in frontend car select).
For 4P: need all 4 slots to have valid car selections.

Quick approach: hardcode P3 and P4 car IDs in a config file or command-line params,
bypassing the frontend car selection flow.

---

## Phase 7: Simulation Adjustments

### 7A. UpdatePlayerVehicleControlState expansion

**Location**: 0x402E60

Already called in a loop over all slots where `state == 0x01`. Will automatically
process 4 human slots if Phase 6A marks them correctly. **No patch needed.**

### 7B. UpdatePlayerSteeringWeightBalance

**Location**: 0x4036B0

Currently applies 2P rubber-banding between P1 and P2. For 4P either:
- Disable entirely (simpler)
- Extend to balance all 4 players

### 7C. Race order / position tracking

`UpdateRaceOrder` (0x42F5B0) sorts all active slots by span progress. Already handles 6
slots. **No patch needed.**

### 7D. Post-race results

`BuildRaceResultsTable` (0x40A8C0) and `RunRaceResultsScreen` (0x422480) process all slots.
Should work, but verify the results screen layout can display 4 human entries.

---

## Phase 8: Activation Mechanism (skip frontend)

Instead of building a full 4P frontend flow, activate via:

1. **Config flag**: Add a `FourPlayer` token to the startup token system (parsed in WinMain).
   The 26 existing tokens (FullScreen, Window, etc.) are string-matched from command line.
2. **Hardcoded car/controller mapping**: Read P3/P4 car IDs and controller indices from
   `config.td5` or a new config file.
3. **Auto-enable**: When the flag is set, force `DAT_004962a0 = 2` (or new 4P value),
   `gRaceViewportLayoutMode = 3`, and populate slots 2-3 at race init.

---

## Implementation Order

```
Phase 1B (input bypass for P3/P4)   -- unblocks testing early
Phase 6  (slot activation)          -- get 4 human actors spawning
Phase 2  (viewport layout)          -- define the quad rects
Phase 3  (render loop)              -- see 4 views on screen
Phase 4  (data expansion)           -- particles, cameras, arrays
Phase 5  (HUD)                      -- per-view speedometer/HUD
Phase 1A (DXInput expansion)        -- proper 4-controller support
Phase 7  (simulation tweaks)        -- balance, results
Phase 8  (activation mechanism)     -- user-facing config
```

## Space Requirements

| Data                    | Additional bytes | Strategy                         |
|-------------------------|-----------------|----------------------------------|
| Quad viewport layout    | ~128            | Code cave / new section          |
| Camera target array     | 8               | Relocate if no adjacent room     |
| Camera preset array     | 8               | Relocate if no adjacent room     |
| Particle banks 2-3      | 12,800          | New allocation or free .data     |
| HUD per-view rects 2-3  | 32              | Adjacent to existing HUD data    |
| P3/P4 input sources     | 8               | Adjacent or new globals          |
| Render loop function    | ~400            | Code cave / new PE section       |
| **Total**               | **~13,400**     |                                  |

## Risk Assessment

- **Performance**: 4x render passes at high resolution may exceed original D3D6 pipeline
  throughput. 2560x1440 / 4 = 640x360 per view -- manageable.
- **Cull window**: 16 spans per view is tight. May need tuning (20?) at cost of performance.
- **DXInput bypass**: Custom DirectInput polling must exactly match the control bitmask
  format (32-bit, see UpdatePlayerVehicleControlState for bit definitions).
- **Memory layout**: Expanding arrays near other globals risks silent corruption. Every
  expansion site must verify adjacent memory.
- **Untested code paths**: The engine has never run with 4 human slots. Edge cases in
  collision resolution, audio mix, and results screen are likely.
