# Car Selection, Track Selection, and Race Type Menu -- Deep Analysis

> Functions: `CarSelectionScreenStateMachine` (0x40DFC0), `DrawCarSelectionPreviewOverlay` (0x40DDC0),
> `TrackSelectionScreenStateMachine` (0x427630), `RaceTypeCategoryMenuStateMachine` (0x4168B0),
> `ScreenMainMenuAnd1PRaceFlow` (0x415490), inline string table helpers (0x4183B0, 0x418410, 0x417B74)

---

## 1. Navigation Flow: Main Menu to Race

The frontend screen system uses a table of 30 function pointers at `g_frontendScreenFnTable` (0x4655C4).
`SetFrontendScreen(index)` resets `g_frontendInnerState = 0`, sets the active screen function pointer,
and timestamps the frame. Each screen function is a state machine driven by `g_frontendInnerState`.

### 1.1 Flow Paths

```
Main Menu (screen 5)
  |
  +-- [Race Menu] --> RaceTypeCategoryMenu (screen 6) --> CarSelection (screen 0x14) --> TrackSelection (screen 0x15) --> Race
  |
  +-- [Quick Race] --> QuickRaceMenu (screen 7) --> Race
  |
  +-- [Two Player] --> CarSelection (screen 0x14, DAT_004962a0=1) --> P2 CarSelection --> TrackSelection (screen 0x15) --> Race
  |
  +-- [Net Play] --> ConnectionBrowser (screen 8) --> SessionPicker (screen 9) --> Lobby (screen 11) --> ...
  |
  +-- [Options] --> OptionsHub (screen 0xC) --> sub-screens 0xD-0x11
  |
  +-- [Hi-Score] --> PostRaceHighScore (screen 0x17)
  |
  +-- [Exit] --> Yes/No dialog --> PostQuitMessage(0)
```

### 1.2 Context Variable: `DAT_004962d4`

This variable tracks which flow context brought the user to the current screen:

| Value | Meaning | Set by |
|-------|---------|--------|
| 1 | Race Menu (1P) | Main Menu button 0 |
| 2 | Quick Race | Main Menu button 1 |
| 3 | Two Player | Main Menu button 2 |
| 4 | Network Play | Main Menu button 3 |
| 5 | Options | Main Menu button 4 |
| 6 | Hi-Score | Main Menu button 5 |

### 1.3 Screen Index Map (Selection-Related)

| Index | Function | Purpose |
|-------|----------|---------|
| 5 | `ScreenMainMenuAnd1PRaceFlow` (0x415490) | Main menu hub |
| 6 | `RaceTypeCategoryMenuStateMachine` (0x4168B0) | Race type picker |
| 7 | `ScreenQuickRaceMenu` (0x4213D0) | Quick race setup |
| 0x14 (20) | `CarSelectionScreenStateMachine` (0x40DFC0) | Car selection |
| 0x15 (21) | `TrackSelectionScreenStateMachine` (0x427630) | Track selection |

---

## 2. Main Menu State Machine (0x415490)

### States: 24 (0x00 - 0x17)

| State | Purpose |
|-------|---------|
| 0 | Init: configure controller bindings, apply saved options, create 7 buttons, load MainMenu.tga |
| 1-2 | Present + tick (two frames to stabilize surface) |
| 3 | Slide-in animation: 7 buttons alternating left/right, title string descends. Completes at tick 0x27 (39 frames) |
| 4 | **Main interaction loop**: waits for button press |
| 5-6 | Exit confirm dialog (Yes/No for quit) |
| 7 | Cancel exit, return to state 4 |
| 8 | Slide-out prep: blit secondary, restore surfaces |
| 9 | Slide-out animation: buttons scatter in all directions, 16 frames |
| 10-11 | Exit game confirm flow (post-Yes) |
| 12 | Scatter buttons for exit transition |
| 0x14 | "Must select controller" error message screen |
| 0x15-0x16 | Controller-required message display |
| 0x17 | Final cleanup, transitions to screen 0xE (controller options) |

### Button Dispatch (State 4)

| Button | Action | Destination |
|--------|--------|-------------|
| 0 (Race Menu) | `DAT_004962d4 = 1`, `g_returnToScreenIndex = 6` | RaceTypeCategoryMenu |
| 1 (Quick Race) | `DAT_004962d4 = 2`, `g_returnToScreenIndex = 7` | QuickRaceMenu |
| 2 (Two Player) | `DAT_004962d4 = 3`, `DAT_004962a0 = 1`, `g_selectedGameType = 0` | CarSelection (2P mode) |
| 3 (Net Play) | `DAT_004962d4 = 4`, `g_returnToScreenIndex = 8` | ConnectionBrowser |
| 4 (Options) | `DAT_004962d4 = 5`, `g_returnToScreenIndex = 0xC` | OptionsHub |
| 5 (Hi-Score) | `DAT_004962d4 = 6`, `g_returnToScreenIndex = 0x17` | PostRaceHighScore |
| 6 (Exit) | Shows Yes/No dialog with `SNK_YesButTxt`/`SNK_NoxButTxt` | Quit or cancel |

**Dev build detection**: If `iRam000600fc != 0` (address 0x600FC mapped), button 2 shows "Time Demo" instead of "Two Player" and activates benchmark mode.

### Controller Validation (State 9 exit)

After slide-out, the system checks if the required joystick(s) are connected:
- 1P modes (`DAT_004962d4` = 1/2/4): checks `DAT_00497a58` (P1 input source)
- 2P mode (`DAT_004962d4` = 3): checks `DAT_00465ff4` (P2 input source)
- If joystick index == 7 (none), shows "Must select controller" message and redirects to controller options

---

## 3. Race Type Category Menu (0x4168B0)

### States: 21 (0x00 - 0x14)

**Top-level menu** (states 0-5): Race category selection
**Cup sub-menu** (states 6-12): Cup tier selection
**Return transition** (state 0x14): Reset to state 1

### Top-Level States

| State | Purpose |
|-------|---------|
| 0 | Init: create 7 buttons (Single Race, Cup Race, Continue Cup, Time Trials, Drag Race, Cop Chase, Back). Load MainMenu.tga. Create 0x110x0xB4 description surface. Set `g_selectedGameType = -1`. |
| 1 | Slide-in animation: 7 buttons, title label. Completes at tick 0x20 (32 frames). |
| 2 | Tick until `AdvanceFrontendTickAndCheckReady()` returns true. |
| 3 | **Main interaction loop**: render buttons + description preview. |
| 4 | Description preview update: when user highlights a different button, re-render the description surface with `SNK_RaceTypeText[gameType]`. |
| 5 | Slide-out: buttons scatter. Leads to Car Selection or cup sub-menu. |

### Top-Level Button Dispatch (State 3)

| Button | g_selectedGameType | Destination |
|--------|--------------------|-------------|
| 0 (Single Race) | 0 | `ConfigureGameTypeFlags()` -> CarSelection (screen 0x14) |
| 1 (Cup Race) | -- | Enters cup sub-menu (state 6) |
| 2 (Continue Cup) | -- | `LoadContinueCupData()` -> PostRaceResults (screen 0x18) |
| 3 (Time Trials) | 9 (!) | `ConfigureGameTypeFlags()` -> CarSelection |
| 4 (Drag Race) | 7 | `ConfigureGameTypeFlags()` -> CarSelection |
| 5 (Cop Chase) | 8 | `ConfigureGameTypeFlags()` -> CarSelection |
| 6 (Back) | -- | Returns to Main Menu (screen 5 or 10) |

**Continue Cup**: Greyed out (`CreateFrontendDisplayModePreviewButton`) if `ValidateCupDataChecksum()` returns false (no valid `CupData.td5` save file). The save file is XOR-encrypted with the key "Steve Snake says : No Cheating!".

### Cup Sub-Menu (States 6-12)

When user selects "Cup Race" (button 1), the buttons slide out (state 6) and new cup tier buttons slide in:

| Button | Cup Tier | g_selectedGameType | Tracks |
|--------|----------|--------------------|--------|
| 0 (Championship) | Always available | 1 | 4 races |
| 1 (Era) | Always available | 2 | 6 races |
| 2 (Challenge) | Locked if `DAT_004962a8 == 0` | 3 | 6 races |
| 3 (Pitbull) | Locked if `DAT_004962a8 < 1` | 4 | 8 races |
| 4 (Masters) | Locked if `DAT_004962a8 < 2` | 5 | 10 races |
| 5 (Ultimate) | Locked if `DAT_004962a8 < 2` | 6 | 12 races |
| 6 (Back) | -- | Returns to top-level menu |

**Cup unlock progression** (`DAT_004962a8`):
- 0 = Only Championship + Era available (Challenge-Ultimate greyed out)
- 1 = Championship + Era + Challenge + Pitbull unlocked
- 2+ = All 6 cups unlocked

Locked cups use `CreateFrontendDisplayModePreviewButton` (greyed-out style).

### Cup Series Schedule Table (0x4640A4)

Each cup type has a track schedule terminated by 0x63 (99):

| Cup | g_selectedGameType | Track indices | Race count |
|-----|--------------------|---------------|------------|
| Championship | 1 | 0, 1, 2, 3 | 4 |
| Era | 2 | 4, 16, 6, 7, 5, 17 | 6 |
| Challenge | 3 | 0, 1, 2, 3, 15, 8 | 6 |
| Pitbull | 4 | 0, 1, 2, 3, 15, 8, 11, 13 | 8 |
| Masters | 5 | 0, 1, 2, 3, 15, 8, 11, 13, 10, 12 | 10 |
| Ultimate | 6 | 0, 1, 2, 3, 15, 8, 11, 13, 10, 12, 9, 14 | 12 |

Track indices are schedule indices that map through `gScheduleToPoolIndex` (0x466894) to pool indices, then through the pool-to-ZIP table (0x466D50) to actual level ZIP files.

### Description Preview System (State 4/9)

When the user highlights a button, state 4 (or 9 in cup sub-menu) fires:
1. Clears the 0x110 x 0xB4 description surface with `BltColorFillToSurface`
2. Looks up `SNK_RaceTypeText[g_selectedGameType]` -- a NUL-separated multi-line string
3. Renders the title line centered with `DrawFrontendLocalizedStringToSurface`
4. Walks past the first NUL to find description lines, renders each at y += 0xC (12px) with `DrawFrontendSmallFontStringToSurface`
5. Stops when it hits a double-NUL or y > 0xAF

---

## 4. Car Selection Screen (0x40DFC0)

### States: 27 (0x00 - 0x1A)

This is the largest frontend FSM. It handles single-player car selection, two-player sequential car picking, network car selection, car preview image loading, spec sheet display, and the config sub-screen.

### State Map

| State | Purpose |
|-------|---------|
| 0 | **Init**: determine car roster range by game type, load UI assets (CarSelBar1.tga, CarSelCurve.tga, CarSelTopBar.tga, GraphBars.tga), create label surface |
| 1 | Reset tick counter |
| 2 | **Sidebar slide-in**: CarSelBar1 bar slides from right edge at 8px/frame + CarSelCurve + CarSelTopBar from left. State 2 is skipped if `(DAT_004962a0 & 4) != 0` or network (`DAT_0048f380 != 0`). |
| 3 | Present + copy primary to secondary |
| 4 | **Button creation**: Creates 5-6 buttons (Car, Paint, Config, Auto/Manual, OK, Back). Sets up left/right arrows for Car and Paint rows. Loads inline string table from `SNK_CarSelect_MT1`. |
| 5 | **Button slide-in**: buttons fly from right side (0x308 offset) over 0x18 (24) frames at 0x20 (32px) per frame |
| 6 | Tick until ready |
| 7 | **Main interaction loop**: renders car preview, handles input |
| 8 | Blit cached rect, wait 2 frames then return to state 7 |
| 10 | Clear car preview area, prepare for new car image load |
| 11 | Car preview cross-fade transition (shared with state 0x15) |
| 12 | **Load car image**: releases old surface, loads `CarPic%d.tga` from car ZIP, renders car name, shows "Locked" / "Beauty" / "Beast" labels |
| 13 | Car preview fade-in (direction 1) |
| 14 | Car preview slide-in from right, 0x19 (25) frames |
| 15 | **Config sub-screen**: renders car spec headers + values (SNK_Config_Hdrs), graph bars |
| 16 | Return from config: set `DAT_0048f360 = 1`, back to state 7 |
| 17 | **Info sub-screen**: renders SNK_Info_Values (10 entries) |
| 18 | Return from info: set `DAT_0048f360 = 2`, back to state 7 |
| 20 (0x14) | Prepare slide-out: deactivate cursor, fill rect, play sound |
| 21 (0x15) | Cross-fade (shared with 0xB) |
| 22-23 (0x16-0x17) | Release car surface, clear to secondary, play sound |
| 24 (0x18) | **Button slide-out**: buttons scatter over 0x18 (24) frames |
| 25 (0x19) | **Screen wipe**: vertical bar sweep clearing from bottom to top |
| 26 (0x1A) | **Exit dispatch**: release all resources, determine next screen |

### Car Roster by Game Type

| g_selectedGameType | Car index range | Notes |
|--------------------|-----------------|-------|
| 0 (Single Race) | 0 .. `DAT_00463e0c`-1 (default 23) | Wraps at boundary |
| 2 (Era cup) | 0 .. 15 (0x0F) | Split: 0-7 = "Beauty", 8-15 = "Beast" |
| 5 (Masters cup) | 15 random from 0-26 | Special roster: 15 slots randomized at `ConfigureGameTypeFlags`, 6 marked as AI opponents |
| 7 (Time Trials) | Same as Single Race | Auto transmission forced (`SNK_ManualButTxt` greyed) |
| 8 (Cop Chase) | 33-36 (0x21-0x24) | Police vehicles only (4 cars) |
| Network (no extras) | 0 .. 32 (0x20) | Extended roster |
| Network (with extras) | 0 .. 36 (0x24) | Full roster including cop cars |

### Locked Car Table (0x463e4c)

37 bytes, one per car index:

```
Index:  0  1  2  3  4  5  6  7  8  9 10 11 12 13 14 15
Locked: 0  0  0  0  0  0  0  0  0  0  0  0  0  0  0  0  (cars 0-15: unlocked)

Index: 16 17 18 19 20 21 22 23 24 25 26 27 28 29 30 31
Locked: 1  1  1  1  1  1  1  1  1  1  1  1  1  1  1  1  (cars 16-31: locked)

Index: 32 33 34 35 36
Locked: 1  1  1  1  0  (cars 32-35: locked, car 36: unlocked)
```

The first 16 cars are always available. Cars 16-35 must be unlocked through cup race wins (via `AwardCupCompletionUnlocks` at 0x421DA0). Car 36 is pre-unlocked (likely an easter egg).

**Lock enforcement**: In state 7, when the user presses OK (button 4):
```c
if (locked_table[DAT_0048f31c] == 0 || network_mode || gameType == 8 || gameType == 5) {
    // Accept selection
} else {
    DXSound::Play(10);  // "Uh-Oh" rejection sound
    // Stay on current screen
}
```

Locked cars can be viewed but not selected in single-player modes. Network and Cop Chase modes bypass the lock.

### Car Navigation (State 7)

**Button 0 (Car)**: Left/right arrows (`DAT_0049b690`) cycle `DAT_0048f31c` (selected car index):
- Normal modes: wraps around at roster boundaries
- Era cup (type 2): wraps within 0-15
- Cop Chase (type 8): wraps within 0x21-0x24
- Masters (type 5): cycles through the 15-car random roster, skipping slots already marked as AI opponents
- Each car change triggers state 10 -> 12 (new image load)

**Button 1 (Paint)**: Left/right arrows cycle `DAT_0048f308` (paint job index):
- 4 paint jobs per car (indices 0-3), wraps at boundaries
- **Disabled for cop cars**: cars in range 0x1C-0x24 have no paint variation
- Triggers state 10 for re-render

**Button 2 (Config)**: Toggles to the spec sheet sub-screen (state 0xF)

**Button 3 (Auto/Manual)**: Toggles transmission (`DAT_0048f338`):
- 0 = Automatic (`SNK_AutoButTxt`)
- 1 = Manual (`SNK_ManualButTxt`)
- Time Trials (type 7): forced to Manual (button greyed out with `CreateFrontendDisplayModePreviewButton`)

**Button 4 (OK)**: Accept car, transition to slide-out. For Masters (type 5), marks the selected car slot as "taken" (value 2) in the roster assignment table.

**Button 5 (Back)**: Returns to previous screen (`g_returnToScreenIndex = 6`)

### Car Preview Image Loading (State 12)

```c
sprintf(buffer, "CarPic%d.tga", ...);  // format: CarPic0.tga through CarPic3.tga (by paint index)
surface = LoadFrontendTgaSurfaceFromArchive(buffer, gCarZipPathTable[gExtCarIdToTypeIndex[carIndex]]);
```

Each car has its own ZIP archive containing 4 preview TGAs (one per paint job). The `gExtCarIdToTypeIndex` table (byte array) maps the external car selection index to an internal car type, which indexes into `gCarZipPathTable` for the ZIP path.

Preview surface size: 0x198 x 0x12C (408 x 300 pixels).

### Spec Sheet Sub-Screen (State 0xF)

Renders car specifications using `SNK_Config_Hdrs` (header labels) and per-car data:

The spec data is organized in 4 columns, each displayed at different Y positions:
- Column 0 (entries 0-3): leftmost, at y = iVar16
- Column 1 (entries 4-7): at y = uVar6 - 0x11C
- Single entry 8: at y = uVar6 - 0xE0
- Column 2 (entries 9-11): at y = uVar6 - 0xC8
- Column 3 (entries 12-13): at y = uVar6 - 0x98

Headers starting with `*` are skipped (hidden/unused fields).

Car spec values come from `DAT_0049b6fc + (fieldIndex + carTypeIndex * 0x11) * 0x30` -- a 2D array of 48-byte strings, 17 fields per car type.

### Info Sub-Screen (State 0x11)

Renders `SNK_Info_Values` -- 10 entries (0x28 / 4 = 10), centered on the preview surface. These appear to be general car information strings (manufacturer, origin, etc.).

### Two-Player Car Selection Flow

`DAT_004962a0` controls the 2-player car picking sequence:

| Value | Meaning |
|-------|---------|
| 0 | Single player (normal) |
| 1 | 2P: Player 1 selecting |
| 2 | 2P: Player 2 selecting |
| 3+ | 2P: Combined (bit flags) |

**State 0 init** handles 2P:
- `(DAT_004962a0 & 3) == 1`: P1 selecting -- creates label surface 0xB, copies P1's previous selection from save slots
- `(DAT_004962a0 & 3) == 2`: P2 selecting -- creates label surface 0xC, copies P2's previous selection from save slots

**State 0x1A exit** handles 2P transitions:
- After P1 selects (DAT_004962a0 == 1): saves P1 choices to `DAT_0048f364/f368/f370/f378`, then sets `DAT_004962a0 = 6` and calls `SetFrontendScreen(0x14)` again for P2
- After P2 selects (DAT_004962a0 == 2): saves P2 choices to `DAT_00463e08/DAT_0048f36c/f374/f37c`, then proceeds to track selection

**2P car selection data slots:**

| Variable | P1 | P2 |
|----------|----|----|
| Car index | `DAT_0048f364` | `DAT_00463e08` |
| Paint job | `DAT_0048f368` | `DAT_0048f36c` |
| Config preset | `DAT_0048f370` | `DAT_0048f374` |
| Transmission | `DAT_0048f378` | `DAT_0048f37c` |

### Network Car Selection

When `DAT_004962bc != 0` (network mode), the FSM calls `ProcessFrontendNetworkMessages()` at the end of every state tick. If `DAT_00497328 != 0` (disconnect detected), it sets `DAT_004962bc = 2` and calls `DXPlay::Destroy()`.

Network mode also enables forced exit: `if (DAT_004962bc == 2) { g_returnToScreenIndex = -1; g_frontendInnerState = 0x14; }` -- forces slide-out when disconnected.

---

## 5. DrawCarSelectionPreviewOverlay (0x40DDC0)

Signature: `void DrawCarSelectionPreviewOverlay(int x, int y, int previewX, int previewY, int animPhase)`

The `animPhase` parameter controls transition effects:

| animPhase | Behavior |
|-----------|----------|
| 0 | Static: draw car preview at (previewX, previewY) with size 0x198 x 0x118 |
| 0x0B | Slide-in from left: `DAT_0049522c * 0x20` horizontal offset |
| 0x0E | Slide-in from right: `DAT_0049522c * -0x40 + 0x4A8` horizontal offset |
| 0x18 | Slide-out: `DAT_0049522c * 0x28 + 10` horizontal offset |
| any | Title bar label overlay always drawn at (x+10, y - DAT_004962cc - 0x40) |

When `DAT_0048f360 != 0` (config/info overlay active), the car image is replaced with the overlay surface (`DAT_00496264`) rendered as a full 0x198 x 300 rect with white color key (0xFFFFFF).

When `DAT_0048f360 == 0` and a car surface exists (`DAT_0048f358`), the car TGA is rendered with priority 0x5A.

---

## 6. Track Selection Screen (0x427630)

### States: 9 (0x00 - 0x08)

| State | Purpose |
|-------|---------|
| 0 | **Init**: validate track index for cup modes (skip locked/invalid groups via `gNpcRacerGroupTable`), create buttons (Track, Forwards, OK, Back), create 0x128 x 0xB8 info surface, load TrackSelect.tga |
| 1-2 | Present + tick |
| 3 | **Slide-in**: buttons slide in over 0x27 (39) frames. Direction button hidden if track has no reverse (`DAT_004a2c9c[trackIndex] == 0`). |
| 4 | **Main interaction loop**: render track preview + info, handle navigation |
| 5 | **Track change**: clear info surface, render track name (split at comma for city/country), load track preview TGA, check locked status |
| 6 | Slide-out prep: blit secondary, play sound |
| 7 | **Slide-out animation**: buttons scatter, preview slides. Completes at tick 0x27. |
| 8 | **Preview transition**: track preview image slides into position over 0x10 (16) frames |

### Buttons

| Index | Label | Function |
|-------|-------|----------|
| 0 | Track | Left/right arrows cycle through tracks |
| 1 | Forwards/Backwards | Toggle direction (only shown if reverse available) |
| 2 | OK | Accept track selection |
| 3 | Back | Return to previous screen |

**Quick Race mode** (`DAT_004962d4 == 2`): Back button is NOT created (only 3 buttons: Track, Direction, OK). `DAT_004654a0 = 2` instead of 3.

### Track Navigation (State 4)

Left/right arrows on button 0 cycle `DAT_004a2c90` (selected track schedule index):

**Non-network mode (`DAT_00496298 == 0`)**:
- `DAT_004962a0 == 0` (normal): wraps in range [0, `DAT_00466840`-1] where `DAT_00466840` = 16
- `DAT_004962a0 != 0` (2P or special): wraps in range [-1, `DAT_00466840`-1], index -1 = "Random Track"

**Network mode (`DAT_00496298 != 0`)**:
- Wraps in range [0, 18] (0x12), i.e. 19 tracks total

**Cup modes (`g_selectedGameType > 7`)**: Skips tracks whose `gNpcRacerGroupTable` entry has bits 0-1 set (non-playable groups). Continues cycling in the chosen direction until finding a valid track.

### Direction Toggle (State 4, Button 1)

`DAT_004a2c98` toggles between 0 (Forwards) and 1 (Backwards). The button label is rebuilt via `RebuildFrontendButtonSurface(1)` -- this swaps between `SNK_ForwardsButTxt` and `SNK_BackwardsButTxt`.

Direction toggle is only available if `DAT_004a2c9c[trackIndex] != 0` (track supports reverse). If the track doesn't support reverse, the direction button is moved off-screen (`iVar8 = -0xE0`).

### Locked Track Table (0x4668B0)

26 bytes, one per track schedule index:

```
Index:  0  1  2  3  4  5  6  7
Locked: 0  0  0  0  0  0  0  0  (tracks 0-7: unlocked)

Index:  8  9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25
Locked: 1  1  1  1  1  1  1  1  1  1  0  0  ...
```

First 8 tracks unlocked, tracks 8-17 locked, tracks 18-19 unlocked (if present).

**Lock enforcement** (state 4, button 2 = OK):
```c
if (trackIndex >= 0 && locked_track_table[trackIndex] != 0 && !network_mode) {
    DXSound::Play(10);  // rejection sound
    return;  // cannot select
}
```

### Track Preview Image Loading (State 5)

```c
sprintf(buffer, "Front End\\Tracks\\%s.tga", trackName);
surface = LoadFrontendTgaSurfaceFromArchive(buffer, "Front End\\Tracks\\Tracks.zip");
```

The track name comes from `SNK_TrackNames[gScheduleToPoolIndex[trackIndex]]` (the pool index maps schedule index to the canonical track identifier). The name is truncated at the first comma for the display label.

Preview surface size: 0x98 x 0xE0 (152 x 224 pixels).

If the track is locked, "Locked" text is rendered centered on the preview surface at y=100.

### Track Info Surface

A 0x128 x 0xB8 (296 x 184) surface displays:
- **Top area (0x40 height)**: Track name (truncated at comma, centered)
- **Bottom area (0x78 height, at y=0x40)**: Track description from `SNK_TrackSel_Ex` via inline string table, position string via `sprintf("%s", ...)`

### "Random Track" (Index -1)

When `DAT_004962a0 != 0` (2P or special mode), the track index can be -1:
- No track preview image is loaded
- Info surface shows `"%s"` with empty track name at y=0x20
- This selects a random track at race launch

### Exit Dispatch (State 7, tick 0x27)

| Condition | Destination |
|-----------|-------------|
| `DAT_004962d4 == 2` (Quick Race) | Screen 7 (QuickRaceMenu) |
| `DAT_004962d4 == 4` (Network) | `CreateFrontendNetworkSession()` |
| Normal (g_returnToScreenIndex == -1) | `InitializeRaceSeriesSchedule()` + `InitializeFrontendDisplayModeState()` -> **launches race** |
| Back pressed (g_returnToScreenIndex != -1) | `SetFrontendScreen(g_returnToScreenIndex)` |

---

## 7. Game Type Flags (ConfigureGameTypeFlags, 0x410CA0)

Maps `g_selectedGameType` to runtime mode flags:

| g_selectedGameType | Name | gRaceRuleVariant | Difficulty | Traffic | Encounters | Special |
|--------------------|------|------------------|------------|---------|------------|---------|
| 0 | Single Race | -- | -- | user pref | user pref | -- |
| 1 | Championship | 0 | 0 | yes | yes | -- |
| 2 | Era | 5 | 0 | yes | yes | 4 laps circuit, `DAT_004aaf74=2` |
| 3 | Challenge | 1 | 1 | yes | yes | -- |
| 4 | Pitbull | 2 | 1 | yes | yes | -- |
| 5 | Masters | 3 | 1 | yes | yes | 15-car random roster |
| 6 | Ultimate | 4 | 2 | yes | yes | -- |
| 7 | Time Trials | -- | 2 | **no** | **no** | `gTimeTrialModeEnabled=1`, `DAT_004aaf74=3` |
| 8 | Cop Chase | -- | -- | yes | **no** | `gWantedModeEnabled=1`, `DAT_004aaf74=4` |
| 9 | Drag Race | -- | -- | no | no | `gDragRaceModeEnabled=1`, custom intro message |

**Masters (type 5) roster initialization**: Generates 15 unique random car indices (0-26), then randomly marks 6 of them with value 1 (reserved for AI). The player cycles through the remaining 9 available slots.

**Race schedule lookup**: For cup types 1-6, `DAT_00490BA8` is set to the track schedule byte at `offset = g_raceWithinSeriesIndex + g_selectedGameType * 0x10 + 0x14` in the schedule table. Value 99 (0x63) = end of series.

---

## 8. Inline String Table System

### SetFrontendInlineStringTable (0x4183B0)

Parses a NUL-separated string block into a pointer table at `DAT_00496374` (max 32 entries, up to address 0x4963F4).

```c
void SetFrontendInlineStringTable(char *stringBlock, int param2, int param3) {
    count = 0;
    ptr = &DAT_00496374;
    do {
        count++;
        *ptr = stringBlock;
        while (*stringBlock++ != '\0') {}  // skip to end of current string
    } while (*stringBlock != '\0' && (int)(ptr++) < 0x4963F4);
    DAT_004963f4 = count;   // entry count
    DAT_00465e10 = -1;      // invalidate cached index
    DAT_004963f8 = param2;  // companion state value 1
    DAT_004963fc = param3;  // companion state value 2
}
```

The input is a Language.dll exported symbol (e.g., `SNK_CarSelect_MT1`, `SNK_RaceMenu_MT`, `SNK_MainMenu_MT`) containing multiple NUL-terminated strings packed contiguously, terminated by a double-NUL.

### SetFrontendInlineStringEntry (0x418410)

Overrides a single slot in the pointer table:

```c
void SetFrontendInlineStringEntry(int index, char *string) {
    (&DAT_00496374)[index] = string;
    DAT_00465e10 = -1;  // invalidate cache
}
```

Used by screens to dynamically replace description text. For example, track selection sets entry 3 to `SNK_TrackSel_Ex` for the extra description line. Car selection sets entry 0xB to `SNK_CarSelect_Ex` (with different offsets for P1 vs P2).

### AdvanceFrontendInlineStringTableState (0x417B74)

A combined helper that:
1. Calls `SetFrontendInlineStringTable` with a new string block
2. Calls the current state callback with selector 5 (rebuilds label surface)
3. Resets `DAT_0049522c = 0` and increments `g_frontendInnerState`

Used by the cup sub-menu transition (state 6 -> 7) to swap string tables when entering the cup tier selection.

### String Table Usage by Screen

| Screen | SNK Symbol | Entries |
|--------|-----------|---------|
| Main Menu | `SNK_MainMenu_MT` | Menu title + button descriptions |
| Race Type | `SNK_RaceMenu_MT` | Race type category descriptions |
| Car Selection | `SNK_CarSelect_MT1` | Car selection labels; entry 0xB overridden with `SNK_CarSelect_Ex` |
| Track Selection | `SNK_TrackSel_MT1` | Track selection labels; entry 3 overridden with `SNK_TrackSel_Ex` |

The label surface (`DAT_00496358`) is created by `CreateMenuStringLabelSurface(index)` which renders the current string table entries into a surface for display.

---

## 9. Easter Egg / Hidden Cars

### Cop Chase Cars (Indices 33-36 / 0x21-0x24)

These 4 cars are only directly selectable in Cop Chase mode (g_selectedGameType == 8). They are police vehicles. Car index 36 (0x24) is pre-unlocked in the locked car table despite being in the "locked" range.

### Easter Egg Cars (Indices 28-31)

From Language.dll SNK_CarLongNames exports (documented in MEMORY.md):
- **FEAR FACTORY WAGON** -- has a dedicated TGA in `Front End\Extras\Fear Factory.tga`
- **THE MIGHTY MAUL**
- **CHRIS'S BEAST**
- **HOT DOG**

These are in the locked range (indices 28-31, all initially locked=1). They become accessible through:
1. Cup race unlock progression
2. Network mode with extras flag (`DAT_00463e6d != 0` extends roster to index 0x24 = 36)

### Network Extended Roster

In network mode (`DAT_00496298 != 0`):
- If `DAT_004962ac != 0` and `DAT_00496298 != 0`: car range uses `DAT_00463e0c` (23)
- If `DAT_00463e6d != 0` (extras enabled): car range extends to 32 (0x20)
- Full network with extras: car range extends to 36 (0x24), including cop cars and easter eggs

---

## 10. Key Global Variables Summary

| Address | Name | Purpose |
|---------|------|---------|
| `0x49635C` | `g_selectedGameType` | Active game type (0-9) |
| `0x496298` | `DAT_00496298` | Network mode flag |
| `0x4962A0` | `DAT_004962a0` | 2P state: 0=1P, 1=P1 selecting, 2=P2 selecting |
| `0x4962A8` | `DAT_004962a8` | Cup unlock tier (0-2+) |
| `0x4962BC` | `DAT_004962bc` | Network connection state |
| `0x4962D4` | `DAT_004962d4` | Flow context (1=RaceMenu, 2=QuickRace, 3=2P, 4=NetPlay) |
| `0x4654A0` | `DAT_004654a0` | Button count (last button index for Back) |
| `0x48F31C` | `DAT_0048f31c` | Currently selected car index |
| `0x48F308` | `DAT_0048f308` | Currently selected paint job (0-3) |
| `0x48F334` | `DAT_0048f334` | Currently selected config preset (0-3) |
| `0x48F338` | `DAT_0048f338` | Transmission: 0=auto, 1=manual |
| `0x4A2C90` | `DAT_004a2c90` | Currently selected track schedule index |
| `0x4A2C98` | `DAT_004a2c98` | Track direction: 0=forwards, 1=backwards |
| `0x463E4C` | locked car table | 37 bytes, 0=unlocked, 1=locked |
| `0x4668B0` | locked track table | 26 bytes, 0=unlocked, 1=locked |
| `0x463E0C` | `DAT_00463e0c` | Total car count (default 23 = 0x17) |
| `0x466840` | `DAT_00466840` | Total track count (default 16 = 0x10) |
| `0x48F360` | `DAT_0048f360` | Car preview overlay mode: 0=car image, 1=config sheet, 2=info sheet |
| `0x4640A4` | cup schedule table | Per-cup track sequence, terminated by 0x63 |
| `0x466894` | `gScheduleToPoolIndex` | Maps schedule index -> pool index (20 bytes) |

---

## 11. Animation Timing Summary

| Animation | Frames | Pixels/frame | Total distance |
|-----------|--------|--------------|----------------|
| Main menu slide-in | 39 (0x27) | 16px alternating L/R | ~624px |
| Main menu slide-out | 16 (0x10) | variable (20-48px) | scattered |
| Race type slide-in | 32 (0x20) | variable per button | ~530px |
| Cup sub-menu transition | 35 (0x23) | variable | button swap |
| Car selection sidebar | variable | 8px/frame | screen width |
| Car selection buttons | 24 (0x18) | 32px/frame | ~768px |
| Car preview cross-fade | 21 (0x15) | -- | fade blend |
| Track selection slide-in | 39 (0x27) | 16px | ~624px |
| Track selection slide-out | 39 (0x27) | variable | scattered |
| Track preview transition | 16 (0x10) | 16px | 256px |

All animations are frame-count based (no delta-time), so they run faster on faster machines (same behavior documented in frontend animation system analysis).
