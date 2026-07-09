# Test Drive 5 -- 2-Player Split-Screen Rendering System

> Session: 2026-03-20 | Binary: TD5_d3d.exe | Ghidra port 8193

## Overview

The 2-player split-screen system is activated when `DAT_004962a0 != 0` (the two-player mode
state). It supports two split orientations controlled by `gSplitScreenMode` (0x00497A5C):
- **Mode 0 (horizontal):** top/bottom halves, stored as `gRaceViewportLayoutMode = 1`
- **Mode 1 (vertical):** left/right halves, stored as `gRaceViewportLayoutMode = 2`

The mapping is: `gRaceViewportLayoutMode = (char)DAT_00497a5c + 1` (set in
`InitializeRaceSession` at 0x42AE1E). Value 0 means single-player (no split).

---

## 1. Viewport Layout Table

### InitializeRaceViewportLayout (0x42C2B0)

Builds a **three-entry layout table** at `gRaceViewportLayouts` (0x4AAE14), each entry 0x40
bytes (16 floats). The table is indexed by `gRaceViewportLayoutMode * 0x10` (i.e., stride
0x40 bytes).

Each entry contains:
| Offset | Field | Description |
|--------|-------|-------------|
| +0x00 | clipLeft_p1 | Left clip bound, player 1 |
| +0x04 | centerX_p1 | Projection center X, player 1 |
| +0x08 | clipRight_p1 | Right clip bound, player 1 |
| +0x0C | clipLeft_p2 | Left clip bound, player 2 |
| +0x10 | centerX_p2 | Projection center X, player 2 |
| +0x14 | clipRight_p2 | Right clip bound, player 2 |
| +0x18 | clipTop_p1 | Top clip bound, player 1 |
| +0x1C | centerY_p1 | Projection center Y, player 1 |
| +0x20 | clipBottom_p1 | Bottom clip bound, player 1 |
| +0x24 | clipTop_p2 | Top clip bound, player 2 |
| +0x28 | centerY_p2 | Projection center Y, player 2 |
| +0x2C | cullWindow | Track span cull window |
| +0x30 | adjustedCullWindow | Adjusted cull window (runtime) |
| +0x34 | viewCount | Number of views to render (1 or 2) |
| +0x38-3C | reserved | Zero-initialized |

#### Layout 0 (1P fullscreen):
- Clip: [0, renderWidth] x [0, renderHeight]
- Center: (renderCenterX, renderCenterY)
- Cull window: 0x40 (64 spans)
- View count: 1

#### Layout 1 (2P horizontal split -- top/bottom):
- **Player 1 (top half):** clip [0, renderWidth] x [0, renderCenterY - 1]
- **Player 2 (bottom half):** clip [0, renderWidth] x [renderCenterY + 1, renderHeight]
- Center X for both: renderCenterX
- Center Y: renderCenterY * 0.5 (P1), renderCenterY * 1.5 (P2)
- Cull window: 0x20 (32 spans -- halved!)
- View count: 2

#### Layout 2 (2P vertical split -- left/right):
- **Player 1 (left half):** clip [0, renderCenterX - 1] x [0, renderHeight]
- **Player 2 (right half):** clip [renderCenterX + 1, renderWidth] x [0, renderHeight]
- Center X: renderCenterX * 0.5 (P1), renderCenterX * 1.5 (P2)
- Center Y for both: renderCenterY
- Cull window: 0x20 (32 spans -- halved!)
- View count: 2

**Key observation:** There is a 1-pixel gap between the two halves (the `-1` / `+1` offsets),
which is where the divider line is rendered.

### Viewport Dimensions Per Player

At 640x480 native:
- **Horizontal split:** each player gets 640x240 (full width, half height)
- **Vertical split:** each player gets 320x480 (half width, full height)

At 2560x1440 (patched):
- **Horizontal split:** each player gets 2560x720
- **Vertical split:** each player gets 1280x1440

---

## 2. Projection Setup

### ConfigureProjectionForViewport (0x43E7E0)

Called once at session init with full (width, height). The projection focal length is:
```
focal = width * 0.5625
```
This is **NOT reconfigured per-view** -- the same focal is used for both players. The
viewport difference comes from `SetProjectedClipRect` and `SetProjectionCenterOffset`, which
are set per-view during each render pass.

**Implication for split-screen:** In horizontal split, each player sees the same horizontal
FOV but reduced vertical FOV (vert- scaling). In vertical split, horizontal FOV is halved
(hor- scaling). This is the native 4:3 assumption at work.

---

## 3. Render Loop (RunRaceFrame, 0x42B580)

The frame loop is structured as follows:

### Simulation Phase (single pass, shared by both players)
```
while (g_simTimeAccumulator > 0xFFFF):
    CacheVehicleCameraAngles(player1, 0)
    CacheVehicleCameraAngles(player2, 1)
    if (DAT_004962a0 != 0):  # 2P only
        UpdatePlayerSteeringWeightBalance()
    for each active slot:
        UpdatePlayerVehicleControlState(slot)
    UpdateRaceActors()          # ALL actors, once
    ResolveVehicleContacts()    # ALL contacts, once
    UpdateTireTrackPool()       # once
    UpdateRaceOrder()           # once
    for viewIndex in range(viewCount):
        UpdateChaseCamera(player[viewIndex], 1, viewIndex)
    UpdateRaceParticleEffects(0)   # view 0 particles
    UpdateRaceParticleEffects(1)   # view 1 particles (always, even in 1P)
    UpdateAmbientParticleDensityForSegment(player1, 0)
    UpdateAmbientParticleDensityForSegment(player2, 1)
```

**Physics is fully shared.** Both players run in the same simulation tick. There is no
separate physics thread or separate timestep.

### Render Phase -- Player 1
```
BeginRaceScene()
SetProjectedClipRect(layout[mode].p1_clip)
SetProjectionCenterOffset(layout[mode].p1_center)
UpdateCamera(player1, view=0)
RenderSky()
RenderTrackSpans(spanList_p1)
RenderRaceActorsForView(0)
RenderTireTrackPool()
RenderAmbientParticleStreaks(player1, 0)
DrawRaceParticleEffects(0)
FlushQueuedTranslucentPrimitives()
FlushProjectedPrimitiveBuckets()
DrawRaceStatusText(player1, 0)
```

### Render Phase -- Player 2 (only if gRaceViewportLayoutMode != 0)
```
SetProjectedClipRect(layout[mode].p2_clip)
SetProjectionCenterOffset(layout[mode].p2_center)
UpdateCamera(player2, view=1)
RenderSky()                           # re-rendered!
RenderTrackSpans(spanList_p2)         # separate span list
RenderRaceActorsForView(1)
RenderTireTrackPool()                 # re-rendered
RenderAmbientParticleStreaks(player2, 1)
DrawRaceParticleEffects(1)
FlushQueuedTranslucentPrimitives()
FlushProjectedPrimitiveBuckets()
DrawRaceStatusText(player2, 1)
```

### Post-Render (shared)
```
SetProjectedClipRect(fullscreen)
SetProjectionCenterOffset(fullCenter)
RenderRaceHudOverlays()          # draws HUD for all views + divider
FlushQueuedRaceHudText()
RenderHudRadialPulseOverlay()
if (gRaceViewportLayoutMode != 0):
    SubmitImmediateTranslucentPrimitive(dividerQuad)
EndRaceScene()
```

**Critical finding:** The entire 3D world (sky, track, actors, particles, translucent
primitives, projected buckets, status text) is rendered **TWICE** within a **single D3D
scene** (one `BeginScene`/`EndScene` pair). The viewport clipping rectangles prevent the two
passes from overlapping. The HUD overlays and divider line are rendered last, after both views.

### Track Span Culling

Each view maintains its own span display list:
- Player 1: `&DAT_004aac60` (base)
- Player 2: `&DAT_004aace0` (base + cullWindow offset)

The cull window is halved in split-screen (32 vs 64 spans), providing a performance
optimization since each player only renders half the track depth.

---

## 4. HUD Layout

### InitializeRaceHudLayout (0x437BA0)

Takes a `param_1` argument that is `gRaceViewportLayoutMode`:
- **0 (1P):** `scale_x = renderWidth / 640`, `scale_y = renderHeight / 480`, full bounds
- **1 (horizontal split):** `scale_x *= 0.5`, `scale_y *= 0.5`, P1 bounds = top half, P2 bounds = bottom half
- **2 (vertical split):** `scale_x *= 0.5`, `scale_y *= 0.5`, P1 bounds = left half, P2 bounds = right half

The HUD iterates `DAT_004b1134` times (1 for 1P, 2 for 2P), rendering each player's HUD
elements (speedometer, gear, lap/position text, u-turn indicator, replay badge) with their
own scale factors and clip bounds.

**Layout data per view (stride 0x38 = 14 floats):**
| Index | Field |
|-------|-------|
| [0] | scale_x (width / 640, halved in 2P) |
| [1] | scale_y (height / 480, halved in 2P) |
| [2] | centerX of this view |
| [3] | centerY of this view |
| [4] | left edge |
| [5] | top edge |
| [6] | right edge |
| [7] | bottom edge |
| [8-13] | P2-specific bounds (only populated when viewCount=2) |

### Minimap

The minimap is **DISABLED in split-screen**:
```c
// In RenderRaceHudOverlays (0x4388A0):
if ((gRaceViewportLayoutMode == '\0') && ((*DAT_004b0c00 & 0x10) != 0) && (g_raceEndFadeState == 0)) {
    RenderTrackMinimapOverlay(player1);
}
```
The check `gRaceViewportLayoutMode == '\0'` explicitly excludes both split-screen modes.
This is presumably a performance/screen-space decision.

### Divider Line

`InitializeRaceOverlayResources` (0x4377B0) creates two divider quad primitives when
`gRaceViewportLayoutMode != 0`:
- **DAT_004b0fb0:** Vertical divider (full-height, 2px wide) -- used in vertical split
- **DAT_004b1068:** Horizontal divider (full-width, 2px tall) -- used in horizontal split

Both are built from the "COLOURS" texture sprite. The appropriate divider is submitted via
`SubmitImmediateTranslucentPrimitive` at the end of `RenderRaceHudOverlays` based on the
active `gRaceViewportLayoutMode`.

---

## 5. Camera System

### Fully Independent Cameras

Each player has their own camera state:
- **gRaceCameraPresetId[0]** / **gRaceCameraPresetId[1]:** independent preset selection (7 presets each)
- **gRaceCameraPresetMode[0]** / **gRaceCameraPresetMode[1]:** trackside vs vehicle-relative
- Camera view key F3 cycles P1, F4 cycles P2 (scancodes 0x3D, 0x3E checked in `PollRaceSessionInput`)
- Per-pad camera-change button (bit 0x1000000) also cycles per-player

The camera update calls in the sim loop and render loop both pass the view index:
```c
UpdateChaseCamera(player[viewIndex], 1, viewIndex);
```

Players can look in completely different directions. The cameras are 100% independent.

### Camera Transition

`UpdateRaceCameraTransitionState` is called separately for each view (0 and 1), with
the appropriate selected slot actor. The transition timer is shared
(`gRaceCameraTransitionGate`, `g_cameraTransitionActive`).

---

## 6. Input System

### Device Assignment

Two global variables control input device assignment:
- **DAT_00497a58:** Player 1 input source
- **DAT_00465ff4:** Player 2 input source

Values:
- **0:** Keyboard (uses `DXInputGetKBStick`)
- **1+:** Joystick index (uses `DXInputGetJS(source - 1)`)

These are configured in `ScreenControlOptions` (0x41DF20), which presents:
- "Player 1" with left/right arrows to cycle devices
- "Configure" button to bind keys/buttons
- "Player 2" with left/right arrows to cycle devices
- "Configure" button for P2 bindings
- "OK" button

Device types are looked up via `&DAT_00465660[source]`, which encodes device type
(3=keyboard, 4=joystick with subtype).

### Input Polling (PollRaceSessionInput, 0x42C470)

```c
for (iVar1 = 0; iVar1 < (DAT_004962a0 != 0) + 1; iVar1++) {
    source = local_8[iVar1];  // [DAT_00497a58, DAT_00465ff4]
    gPlayerControlBits[iVar1] = 0;
    if (source == 0)
        gPlayerControlBits[iVar1] = DXInputGetKBStick(0);
    else
        gPlayerControlBits[iVar1] = DXInputGetJS(source - 1);
    // rear-view flag from per-player state
    if (iVar1 == 0 && DAT_0048f378)  gPlayerControlBits |= 0x10000000;
    if (iVar1 == 1 && DAT_0048f37c)  DAT_00483000 |= 0x10000000;
}
```

The control bits array is:
- `gPlayerControlBits` (0x482FFC): Player 1
- `DAT_00483000` (0x483000): Player 2

**Both players can use keyboard OR joystick.** The assignment is fully configurable. Default
is likely P1=keyboard, P2=joystick, but this can be swapped.

### DXInput::SetConfiguration

Called at race start:
```c
DXInput::SetConfiguration(DAT_00497a58, DAT_00465ff4);
```
This commits the frontend control configuration into the live DXInput custom-map tables.

### Force Feedback

`ConfigureForceFeedbackControllers` (0x428880) stores the controller assignments for
per-player force feedback. In local 2P, both slots are populated from the passed array.
In network play, only the local participant's slot is configured.

---

## 7. 2P-Specific Physics

### Shared Simulation

Physics runs **once** per tick for all actors. Both human players share the same simulation
timestep and update order. There is no separate physics pass per player.

### UpdatePlayerSteeringWeightBalance (0x4036B0)

Called ONLY when `DAT_004962a0 != 0` (2P mode). Computes a relative weight based on the
distance between player 1 and player 2 track positions:
```c
delta = abs(player1.trackPos - player2.trackPos) * 2;
delta = min(delta, DAT_0048301c);  // cap
if (player1 behind) {
    weight_p1 = 0x100 + delta;   // boost for trailing player
    weight_p2 = 0x100 - delta;   // reduce for leading player
} else {
    weight_p1 = 0x100 - delta;
    weight_p2 = 0x100 + delta;
}
```

These weights are stored at `DAT_0046317C` (P1) and `DAT_0046317E` (P2), and used by
`UpdatePlayerVehicleControlState` when bit 0x200 is set in the control bits, to scale the
encounter/lane steering override. This is a **rubber-banding mechanism for 2P** -- the
trailing player gets slightly stronger lane control.

---

## 8. Car Selection for 2P

### State Machine at DAT_004962a0

The two-player mode state (DAT_004962a0) drives the car selection flow:
| Value | State |
|-------|-------|
| 0 | 1-player mode |
| 1 | Entering 2P: selecting cars for P1 |
| 2 | Selecting cars for P2 |
| 5 | Network mode (host) |
| 6 | Network mode (client) |

In `CarSelectionScreenStateMachine` (0x40E120):
- When `(DAT_004962a0 & 3) == 1`: first entry for 2P, creates "Player 2" label, copies P1's
  selection as P2's default, resets P2 state
- When `(DAT_004962a0 & 3) == 2`: second pass for P2 car selection
- After P2 selection completes, the state machine advances to track selection

The writes to `DAT_004962a0`:
- Set to **1** at `0x415954` (ScreenMainMenuAnd1PRaceFlow, entering 2P flow)
- Set to **2** at `0x40E3B4` (after P1 car selected, advancing to P2 selection)
- Set to **5/6** at `0x40F79C/0x40F815` (network mode setup)

---

## 9. Particle System in Split-Screen

### Pool Layout

The particle pool at `0x4A318F` has **two independent banks**:
- **Bank 0** (view 0): `&DAT_004a318f` base, 100 slots x 0x40 stride = 0x1900 bytes
- **Bank 1** (view 1): `&DAT_004a318f + 0x1900` base, 100 slots x 0x40 stride

`InitializeRaceParticleSystem` (0x429510) clears BOTH banks at init:
```c
puVar3 = &DAT_004a4a8f;
do {
    puVar3[-0x1900] = 0;  // bank 0
    *puVar3 = 0;           // bank 1
    puVar3 = puVar3 + 0x40;
} while (puVar3 < 0x4a638f);
```

### Update and Render

Both `UpdateRaceParticleEffects(param)` and `DrawRaceParticleEffects(param)` take a view
index and operate on the corresponding bank:
```c
puVar1 = &DAT_004a31ac + param_1 * 0x1900;  // bank offset
for (i = 0; i < 100; i++) { ... }
```

Particles are updated for BOTH views during each sim tick (in the shared simulation loop),
and rendered per-view during each render pass. Each view has its own 100-slot pool with
independent callback pointers.

### Ambient Particles

`UpdateAmbientParticleDensityForSegment` also takes a view index, storing per-view density
at `&DAT_004c3dd8[viewIndex]`. `RenderAmbientParticleStreaks` is called per-view with the
corresponding player actor and view index.

---

## 10. Audio in Split-Screen

### Vehicle Audio Mix

`UpdateVehicleAudioMix` (0x440B70) detects split-screen mode changes and reconfigures audio:
```c
if (DAT_004c38d8 != (gRaceViewportLayoutMode > 0)) {
    if (DAT_004c38d8 == 0) {
        // transitioning TO split-screen: reset distance values
        DAT_004c38d8 = 1;
    } else {
        // transitioning FROM split-screen: stop all vehicle audio channels
        for each channel: DXSound::Stop(channel);
    }
}
```

The audio mix considers both player camera positions in split-screen, computing separate
listener positions (`DAT_004c3810/14/18` for P1, `DAT_004c381C/20/24` for P2).

### 3D Sound Positioning

`PlayVehicleSoundAtPosition` (0x441DCB) checks `gRaceViewportLayoutMode` to determine
whether to use single-listener or dual-listener distance attenuation.

### IsLocalRaceParticipantSlot (0x42CBE0)

Used by audio and other systems to determine which slots are "local":
```c
if (network_mode) return dpu_participant_flags[slot];
if (gRaceViewportLayoutMode != 0) return (slot < 2);  // 2P: slots 0 and 1 are local
return (slot == 0);  // 1P: only slot 0
```

---

## 11. Race End / Fade-Out

The fade-out direction depends on the viewport layout mode:
| gRaceViewportLayoutMode | Fade behavior |
|-------------------------|---------------|
| 0 (1P) | Alternating horizontal/vertical fade (g_fadeDirectionAlternator) |
| 1 (horizontal split) | Fade direction = 0 (horizontal bars closing) |
| 2 (vertical split) | Fade direction = 1 (vertical bars closing) |

The fade compresses the clip bounds toward the center:
```c
if (g_fadeDirection == 0) {  // horizontal
    local_c = gRenderHeight * fadeProgress * 0.5;
    SetClipBounds(0, gRenderWidth, local_c, gRenderHeight - local_c);
} else {                     // vertical
    local_8 = gRenderWidth * fadeProgress * 0.5;
    SetClipBounds(local_8, gRenderWidth - local_8, 0, gRenderHeight);
}
```

---

## 12. 2P Options Screen

### ScreenTwoPlayerOptions (0x420C70)

Frontend screen (Entry 21 in screen table, via Options Hub Entry 12). Presents:
- **Split Screen** button with left/right arrows: toggles `DAT_00497a5c` between 0 (horizontal) and 1 (vertical), using a TGA sprite sheet (`Front_End_SplitScreen.tga`)
- **Catch-up** button with left/right arrows: adjusts `DAT_00465ff8` (0-9 range), the AI catch-up / rubber-band intensity for 2P
- **OK** button: confirms and returns to options hub

The split-screen preview sprite is loaded from `Front_End_FrontEnd.zip` and displayed as a
32x32 icon at `DAT_00497a5c << 5` offset (i.e., 0 or 32 pixels into the sprite sheet).

---

## 13. Disabled/Restricted Features in 2P

| Feature | Status |
|---------|--------|
| Minimap | **Disabled** (explicit gRaceViewportLayoutMode==0 check) |
| Traffic actors | **Disabled** (set in InitializeRaceSession when gRaceViewportLayoutMode != 0) |
| Special encounters | **Disabled** (gSpecialEncounterEnabled = 0 when DAT_004962a0 != 0) |
| Track span cull window | **Halved** (32 vs 64 spans) |
| HUD scale | **Halved** in both axes (scale_x *= 0.5, scale_y *= 0.5) |
| Replay mode | Available (cameras switch to trackside) |

---

## 14. Key Global Variables Summary

| Address | Name | Type | Description |
|---------|------|------|-------------|
| 0x004962A0 | DAT_004962a0 | int | 2P mode state (0/1/2/5/6) |
| 0x00497A5C | gSplitScreenMode | int | 0=horiz, 1=vertical |
| 0x004AAF89 | gRaceViewportLayoutMode | char | 0=1P, 1=horiz, 2=vert |
| 0x004AAE14 | gRaceViewportLayouts | float[3][16] | Viewport layout table (0x40/entry) |
| 0x004AAE48 | viewCount field | int | Views per layout (1 or 2) |
| 0x00482FFC | gPlayerControlBits | uint | P1 control bits |
| 0x00483000 | DAT_00483000 | uint | P2 control bits |
| 0x00497A58 | Player1InputSource | int | 0=KB, 1+=joystick |
| 0x00465FF4 | Player2InputSource | int | 0=KB, 1+=joystick |
| 0x004AAF5C | DAT_004aaf5c | int | Split-screen active flag (set at race start) |
| 0x004B1134 | DAT_004b1134 | int | HUD view count (1 or 2) |
| 0x004B1138 | DAT_004b1138 | float | HUD scale_x |
| 0x004B113C | DAT_004b113c | float | HUD scale_y |
| 0x0046317C | Weight_P1 | short | P1 steering weight (2P rubber-band) |
| 0x0046317E | Weight_P2 | short | P2 steering weight (2P rubber-band) |
| 0x00465FF8 | CatchupIntensity | int | 2P catch-up slider (0-9) |
| 0x004A318F | ParticlePool | byte[2][0x1900] | Per-view particle banks |

---

## 15. Widescreen Implications

The split-screen system is **fully dynamic** with respect to resolution:
- `InitializeRaceViewportLayout` derives all values from `gRenderWidth`/`gRenderHeight`
- `InitializeRaceHudLayout` uses `renderWidth/640` and `renderHeight/480` scaling
- `InitializeRaceOverlayResources` sizes divider quads from render dimensions
- `ConfigureProjectionForViewport` uses `width * 0.5625` (the known 4:3 focal assumption)

**No additional patches are needed** for split-screen at widescreen resolutions, beyond the
existing projection fix (`focal = height * 0.75`). The split divides the screen correctly at
any resolution. The only concern is that the halved cull window (32 spans) may not be enough
at very wide FOV in vertical split mode.
