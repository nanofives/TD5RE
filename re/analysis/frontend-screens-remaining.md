# Remaining Frontend Screens -- Deep Dive Analysis

> Decompiled from TD5_d3d.exe (port 8195) on 2026-03-20.
> All addresses are virtual addresses in the loaded PE image (base 0x00400000).

---

## Table of Contents

1. [ShowLegalScreens (0x42c8e0)](#1-showlegalscreens-0x42c8e0)
2. [RunAttractModeDemoScreen (0x4275A0)](#2-runattractmodedemoscreen-0x4275a0)
3. [ScreenPostRaceHighScoreTable (0x413580)](#3-screenpostracehighscoretable-0x413580)
4. [ScreenPostRaceNameEntry (0x413bc0)](#4-screenpostracenameentry-0x413bc0)
5. [RunRaceResultsScreen (0x422480)](#5-runraceresultsscreen-0x422480)
6. [ScreenCupFailedDialog (0x4237f0)](#6-screencupfaileddialog-0x4237f0)
7. [ScreenCupWonDialog (0x423a80)](#7-screencupwondialog-0x423a80)
8. [AwardCupCompletionUnlocks (0x421da0)](#8-awardcupcompletionunlocks-0x421da0)
9. [ScreenQuickRaceMenu (0x4213d0)](#9-screenquickracemenu-0x4213d0)
10. [ScreenDisplayOptions (0x420400)](#10-screendisplayoptions-0x420400)
11. [ScreenExtrasGallery (0x417d50)](#11-screenextrasgallery-0x417d50)
12. [ScreenMusicTestExtras (0x418460)](#12-screenmusictestextras-0x418460)
13. [LoadExtrasGalleryImageSurfaces (0x40d590)](#13-loadextrasgalleryimagesurfaces-0x40d590)
14. [LoadExtrasBandGalleryImages (0x40d6a0)](#14-loadextrasbandgalleryimages-0x40d6a0)
15. [ScreenSessionLockedDialog (0x41d630)](#15-screensessionlockeddialog-0x41d630)

---

## 1. ShowLegalScreens (0x42c8e0)

**Purpose:** Displays the two legal/copyright splash screens at game startup.

**NOT a state machine** -- this is a synchronous blocking function, unlike all other frontend screens. It runs entirely before the main loop starts.

### Flow

1. Reads `legal1.tga` from `LEGALS.ZIP`, allocates buffer (file size + 0x96000 scratch), decodes via `DisplayLoadingScreenImage`, then frees.
2. Enters a polling loop:
   - Auto-advances after **5000ms** (5 seconds).
   - Keyboard skip available after **400ms** grace period (scans all 256 key codes via `DXInput::CheckKey`).
3. Repeats identically for `legal2.tga`.
4. Returns to caller (WinMain proceeds to frontend init).

### Key Details

- Uses `timeGetTime()` for timing -- one of the few time-based mechanisms in the frontend.
- Fixed 640x480 TGA path (no dynamic resolution).
- No mouse input check -- keyboard only for skip.
- The 400ms grace period prevents accidental skip from keys held during startup.

---

## 2. RunAttractModeDemoScreen (0x4275A0)

**Purpose:** Attract mode / demo playback transition. Activated when the player idles on the main menu for 50 seconds.

### State Machine (6 states)

| State | Action |
|-------|--------|
| 0 | Set `DAT_00495254 = 1` (attract mode flag), present buffer, activate cursor overlay |
| 1 | Release frontend buttons (cleanup from main menu) |
| 2-3 | Present primary buffer via copy (2 frames of setup) |
| 4 | Call `InitFrontendFadeColor(0)` -- initialize fade-to-black effect |
| 5 | Call `RenderFrontendFadeEffect()` each frame until `DAT_00494fc0 == 0` (fade complete), then call `InitializeRaceSeriesSchedule()` + `InitializeFrontendDisplayModeState()` to start the demo race |

### Key Details

- **Trigger**: `RunFrontendDisplayLoop` checks 50s idle timeout, randomly selects a track, then calls `SetFrontendScreen(2)` to reach this function.
- **Demo playback**: After fade completes, `InitializeRaceSeriesSchedule()` sets up a race with `g_inputPlaybackActive = 1` (pre-recorded input replay).
- **The ONLY frontend transition that uses the bar-sweep dither fade** -- all other screen changes use slide-in/slide-out animations.
- **No recorded demo files** -- the attract mode uses the same replay recording system as the post-race replay viewer (`g_replayModeFlag` at 0x4AAF64).
- Very simple: just 6 states, most of which are single-frame setup. The actual demo race runs via the normal race loop.

---

## 3. ScreenPostRaceHighScoreTable (0x413580)

**Purpose:** Displays the high score leaderboard after a race. Entry 24 in the frontend screen table.

### State Machine (9 states)

| State | Action |
|-------|--------|
| 0 | **Init**: Load MainMenu.tga background, create title label surface (menu string 7), create 0x208x0x90 score panel surface (black fill), create invisible nav button + "OK" button, activate cursor, play whoosh SFX |
| 1-2 | Present buffer, reset tick counter, rebuild button surface, init arrows, draw score entry for index 0 |
| 3 | **Slide-in animation**: Title slides from left, score panel slides from right. 39 frames (0x27 ticks). Arrow navigation surfaces slide in. On completion: deactivate cursor, play ready SFX |
| 4-5 | Static display frames (2 frames) |
| 6 | **Interactive state**: Display score table, handle left/right arrow navigation to browse different score categories. Arrow input (`DAT_0049b690`) cycles `DAT_00497a68` through entries. Handles wrap-around (0-25 range, with special handling for `DAT_00466840` boundary). "OK" button (index 1) triggers exit. |
| 7 | Prepare for slide-out: restore secondary buffer, activate cursor, reset string table, play whoosh SFX |
| 8 | **Slide-out animation**: Title slides left, buttons scatter. 16 frames (0x10 ticks). On completion: release buttons and surfaces, either return to caller screen or init race series. |

### Score Table Format (from `DrawPostRaceHighScoreEntry` at 0x413370)

The score panel (0x208 x 0x90 pixels) displays columns:
- **Name** (0x10-0x70)
- **Best Time/Lap/Pts** (0x80-0xD4) -- column header depends on race type byte in `gNpcRacerGroupTable`
- **Car** (0xE4-0x150)
- **Avg** speed (0x160-0x1AC)
- **Top** speed (0x1BC-0x208)

Each score category has 5 slots, 0x20 bytes per entry (at base `0x4643BC`):
- Bytes 0x00-0x0E: Player name (null-terminated string)
- Byte 0x0F: Padding
- Bytes 0x10-0x13: Score/time value
- Byte 0x14: Car index
- Bytes 0x18-0x1B: Top speed
- Bytes 0x1C-0x1F: Average speed

Score types (from `gNpcRacerGroupTable[entry*0x29] & 3`):
- **0**: Time-based (lower is better) -- "Time" column header
- **1**: Lap-based -- "Lap" column header
- **2**: Points-based (higher is better) -- "Pts" column header

### Navigation

- Left/right arrows cycle through 26 score categories (0x00-0x19)
- With `DAT_00496298 == 0` (locked mode): wraps 0x13 <-> `DAT_00466840`
- With `DAT_00496298 != 0` (unlocked): wraps 0x00 <-> 0x19

---

## 4. ScreenPostRaceNameEntry (0x413bc0)

**Purpose:** Name entry screen for new high scores. Appears after a race if the player's score qualifies.

### State Machine (13 states: 0-12)

| State | Action |
|-------|--------|
| 0 | **Qualification check**: Determines score type (time/lap/points), compares player's result against the worst entry in the 5-slot table. If score doesn't qualify OR 2-player mode OR player was disqualified (`companion_state_2 == 2`), skips to state 4 (display only). Special conditions: quick race requires all of `DAT_00466008`, `DAT_00466004`, `gSpecialEncounterConfigShadow` to be non-zero. Creates text input button (`SNK_EnterPlayerNameButTxt`), sets up keyboard input context (`DAT_004969c0 = &DAT_00496ff8`, max 0x10 chars). |
| 1 | **Slide-in**: Title + name entry button animate in over 0x20 (32) frames |
| 2 | **Text input active**: Calls `RenderFrontendCreateSessionNameInput()` for keyboard input. When `DAT_004969d0 == 2` (user pressed Enter), copies entered name (or falls back to default name from `DAT_004970ac` if empty). Plays whoosh SFX, advances. |
| 3 | **Slide-out of input**: Name entry button animates off-screen over 0x20 frames. Releases button surface. |
| 4 | **Insert score**: Determines insertion position by scanning the 5-slot table. Shifts lower entries down (`memcpy` 0x20 bytes per slot). Writes new entry: name (from `DAT_00496ff8`), score value, car index (`DAT_0048f364`), speed stats. For cup races, average speed = total/race count. Creates score display buttons. |
| 5-6 | Present buffer, draw score table for the relevant category |
| 7 | **Score table slide-in**: 39 frames, same animation as ScreenPostRaceHighScoreTable |
| 8-9 | Static display (2 frames) |
| 10 | **Interactive display**: Auto-selects "OK" button. Waits for confirm. |
| 11 | Prepare slide-out |
| 12 | **Slide-out**: 16 frames. On completion: release all surfaces, transition to screen 5 (results). For cup game types (1-7): resets `DAT_004a2c90 = 0`. |

### Score Insertion Logic

For time/lap (types 0 and 1): scans table from top, inserts where `player_score < table_score` (lower is better for time; for laps, finds minimum non-zero lap time across all 6 racer slots from address range ending at 0x4AB466, cap at 0x2B818).

For points (type 2): scans from top, inserts where `table_score < player_score` (higher is better).

### Name Input

- Max 16 characters (`_DAT_004969c8 = 0x10`)
- Buffer at `DAT_00496ff8`
- Default fallback name at `DAT_004970ac` (used if player enters empty string)
- Uses `RenderFrontendCreateSessionNameInput()` -- same text input widget as network session creation

---

## 5. RunRaceResultsScreen (0x422480)

**Purpose:** The main post-race results FSM. This is the central hub after every race, handling score display, replay, save, and progression.

### State Machine (22 states: 0x00-0x15)

| State | Action |
|-------|--------|
| **0** | **Init & routing**: Complex branching based on game type. Sorts results (by secondary metric desc for types 1/6, by primary metric asc for types 2-5). For network games: checks `dpu_exref+0xBE8` for host slot. If player was disqualified or DNF, routes to cup failed dialog (screen 0x1A). Creates 0x198 x 0x188 results panel. Draws column headers from `SNK_ResultsTxt`, `SNK_CCResultsTxt`, `SNK_DRResultsTxt` depending on game type. Skips inactive slots (state == 3). |
| **1-2** | Present buffer, reset counter |
| **3** | **Slide-in**: Results panel + buttons animate in. 39 frames. Special: if `DAT_00497a74 != 0` OR player disqualified/DNF, jumps to state 0xC (cleanup). |
| **4-5** | Static display (2 frames) |
| **6** | **Interactive browsing**: Left/right arrows cycle through racer slots (0-5), skipping inactive ones. Confirm button triggers state 0xB (exit). For drag race (type 7): only 2 slots (mask & 1). |
| **7-8** | **Slide left animation**: Panel slides right 0x20px/frame, 17 frames. Draws new racer data on completion. |
| **9-10** | **Slide right animation**: Panel slides left, same timing. |
| **0xB** | **Exit slide-out**: Title + panel + buttons animate off-screen. 17 frames. |
| **0xC** | **Cleanup**: Release all surfaces and buttons. |
| **0xD** | **Post-results menu**: Creates context-dependent button set: |
|  | *Quick Race / Drag Race (type < 1 or 7)*: "Race Again", "View Replay" (greyed if unavailable), "View Race Data" (greyed if DNF), "Select New Car", "Quit" -- 5 buttons |
|  | *Cup Race (types 1-6)*: "Next Cup Race", "View Replay", "View Race Data", "Save Race Status", "Quit" -- 5 buttons. If next race config fails (`ConfigureGameTypeFlags() == 0`), "Next Cup Race" and "Save" are greyed out, and `DAT_00497a70 = 1` flags cup complete. |
|  | *Masters Series (type 5)*: Special handling -- if `DAT_00497a78 == 0` and not final race (index != 9), jumps to state 0x15. |
|  | *Network*: Skips to screen 5 or screen 0xB directly. |
| **0xE** | **Menu slide-in**: 5 buttons animate in from alternating sides. 32 frames. |
| **0xF** | **Menu interaction**: Button handlers: |
|  | Button 0 ("Race Again" / "Next Cup Race"): Restores race snapshot if `DAT_00497a78 != 0`, starts new race. For Masters series, handles slot progression. |
|  | Button 1 ("View Replay"): Sets `g_inputPlaybackActive = 1`, starts replay. |
|  | Button 2 ("View Race Data"): Goes to screen 0x18 (high score table). |
|  | Button 3 ("Save Race Status"): Goes to state 0x11. |
|  | Button 4 ("Quit"): For quick race -> goes to screen 0x19 (name entry). For cup -> checks cup completion, routes to won (0x1A) or failed (0x1B) dialog. |
| **0x10** | **Menu slide-out**: 32 frames. On completion: dispatches based on `DAT_00497a64` (stored button index). |
| **0x11** | **Save cup data**: Calls `WriteCupData()`, shows "Block Saved OK" or "Failed to Save" message. Creates OK button. |
| **0x12** | **Save confirmation slide-in**: 32 frames. |
| **0x13** | **Save confirmation wait**: Forces button index to 1, waits for confirm. |
| **0x14** | **Save confirmation slide-out**: 32 frames. Returns to state 0xD (post-results menu). |
| **0x15** | **Masters series progression**: Release buttons, set `DAT_00497a78 = 1` (re-race flag), `DAT_00497a74 = 1`, `DAT_0048f380 = 0`. Go to screen 0x14 (car select). |

### Race Snapshot System

When `DAT_00497a78 == 1` (first entry), the results screen saves the current car/track/options state:
- `DAT_00497a7c` = car index (`DAT_0048f364`)
- `DAT_00497a80` = car paint (`DAT_0048f368`)
- `DAT_00497a84` = transmission (`DAT_0048f370`)
- `DAT_00497a88` = `DAT_0048f378`
- `DAT_00497a8c` = opponent car (`DAT_00463e08`) -- drag race only
- `DAT_00497a90-98` = opponent paint/trans/config

This snapshot is restored when "Race Again" is selected, ensuring the same setup.

---

## 6. ScreenCupFailedDialog (0x4237f0)

**Purpose:** Shows the "cup failed" message when the player doesn't meet cup requirements.

### State Machine (6 states)

| State | Action |
|-------|--------|
| 0 | **Init**: Only activates for cup game types (1-6). Otherwise redirects to screen 5. Creates 0x198 x 0x70 dialog surface. Draws 4 lines of centered text: `SNK_SorryTxt` / `SNK_YouFailedTxt` / `SNK_ToWinTxt` / race type name from `SNK_RaceTypeText[g_selectedGameType]`. Creates "OK" button. |
| 1-3 | Present buffer (3 frames of setup) |
| 4 | **Slide-in**: Dialog slides from right (24px/frame), button slides from left. 32 frames. |
| 5 | **Wait for confirm**: Displays dialog. On button press: release surface, release buttons, go to `g_returnToScreenIndex`. |

### Dialog Text

```
Sorry
You Failed
To Win
[Race Type Name]
```

Simple, no animations beyond the slide-in. No slide-out -- instant transition on confirm.

---

## 7. ScreenCupWonDialog (0x423a80)

**Purpose:** Congratulations dialog when the player wins a cup series.

### State Machine (6 states)

Same structure as CupFailedDialog but with larger surface (0x198 x 0xC4) and additional unlock info.

| State | Action |
|-------|--------|
| 0 | **Init**: Only for cup types 1-6. **Deletes `CupData.td5`** (the saved cup progress file -- no longer needed). Creates dialog surface. Draws: `SNK_CongratsTxt` (y=0), `SNK_YouHaveWonTxt` (y=0x38), race type name (y=0x54). If `DAT_00494bb0 != 0`: draws unlocked car info at y=0x8C (format: `"%d %s"`). If `DAT_00494bb4 != 0`: draws unlocked track info at y=0xA8. Creates "OK" button. |
| 1-3 | Present buffer |
| 4 | **Slide-in**: 32 frames |
| 5 | **Wait for confirm**: On press -> release, go to `g_returnToScreenIndex` |

### Key Details

- `DAT_00494bb0` and `DAT_00494bb4` are populated by `AwardCupCompletionUnlocks` before this screen is shown.
- Format string `"%d %s"` at 0x4660e0 -- displays the unlock index and name.
- The cup data file deletion is significant: once you see this screen, your mid-cup save is gone.

---

## 8. AwardCupCompletionUnlocks (0x421da0)

**Purpose:** Determines and applies car/track unlocks when the player wins 1st place in a cup.

### Prerequisites

- Player must have finished 1st: `gRaceSlotStateTable.slot[0].companion_state_2 == 1`
- Player must NOT have been caught by cops: `gRuntimeSlotActorTable.slot[0].gap_0376[0xD] == 0`

### Unlock Logic

Reads `gNpcRacerGroupTable[DAT_004a2c90 * 0x29]` (first byte = race category):

**Category 0 (standard cup):**
- Sets `DAT_004a2c9c[DAT_004a2c90] = 1` -- marks this cup as completed in the completion table.
- No car/track unlock.

**Category 1 (special cups -- car unlock cups):**

| Cup Index (`DAT_004a2c90`) | Unlocked Car/Track Index | Likely Unlock |
|---|---|---|
| 4 | 0x15 (21) | Car slot 21 |
| 5 | 0x11 (17) | Car slot 17 |
| 6 | 0x18 (24) | Car slot 24 |
| 7 | 0x19 (25) | Car slot 25 |
| 0x10 (16) | 0x17 (23) | Car slot 23 |
| 0x11 (17) | 0x1A (26) | Car slot 26 |
| 0x12 (18) | **SPECIAL**: If car index == 0x20 -> sets `_DAT_004962b0 = 1` (easter egg flag!), else unlocks car 0x20 (32) |

The unlock clears the lock byte: `DAT_00463e4c[unlock_index] = 0` and updates `DAT_00463e0c` (total unlocked car count) if needed.

### Easter Egg: Cup 0x12 + Car 0x20

When cup index 0x12 (18) is won AND `DAT_0048f364 == 0x20` (the player is already driving car 32 -- one of the easter egg cars), instead of unlocking a car, it sets `_DAT_004962b0 = 1`. This is a **hidden flag** that likely enables additional secret content. Since car 0x20 is in the easter egg car range (FEAR FACTORY WAGON, THE MIGHTY MAUL, CHRIS'S BEAST, HOT DOG), winning the final special cup while driving a secret car triggers this flag.

---

## 9. ScreenQuickRaceMenu (0x4213d0)

**Purpose:** Quick race setup screen -- choose car and track before a one-off race.

### State Machine (7 states)

| State | Action |
|-------|--------|
| 0 | **Init**: Reset race state. Validate car/track indices against max counts. Create title label (menu string 3), info panel (0x208 x 200px). Draw car name (y=0) and track name (y=0x78) using format string at `DAT_004660b0`. If car is locked (`DAT_00463e4c[car_index] != 0`) and not in network mode: draw "Locked" text. Same for track (`DAT_004668b0[track_index]`). Create 4 buttons: "Change Car", "Change Track", "OK", "Back". Init left/right arrows on buttons 0 and 1. |
| 1-2 | Present buffer, reset counter |
| 3 | **Slide-in**: 39 frames. All 4 buttons + arrows animate from alternating sides. |
| 4 | **Interactive**: Arrow input on button 0 cycles car (`DAT_0048f31c`), on button 1 cycles track (`DAT_004a2c90`). Car range depends on mode: network w/ unlocks = 0-36 (0x25), cheats enabled = 0-32 (0x21), else `DAT_00463e0c`. Track range: `DAT_00466840` or 0x13 (network). Redraws car/track name and "Locked" status on change. **"OK" blocked if car or track is locked** (plays error SFX 10). "OK" sets `g_returnToScreenIndex = -1` (start race), "Back" sets it to 5 (return to menu). |
| 5 | Prepare slide-out |
| 6 | **Slide-out**: 16 frames. On completion: either start race (`InitializeRaceSeriesSchedule`) or return to menu. |

### Car/Track Lock System

- `DAT_00463e4c[]` -- car lock table. Non-zero = locked.
- `DAT_004668b0[]` -- track lock table. Non-zero = locked.
- `DAT_00496298` -- cheat/unlock-all flag. When non-zero, lock checks are bypassed.
- `DAT_00463e6d` -- another unlock modifier (when `'\0'`, limits car range to 0x20).

---

## 10. ScreenDisplayOptions (0x420400)

**Purpose:** Display/graphics options screen.

### State Machine (9 states)

| State | Action |
|-------|--------|
| 0 | **Init**: Create title (menu string 6), options panel (0xE0 x 0x118). Create buttons: "Resolution" (always active with arrows), "Fogging" (active with arrows only if `DXD3D::CanFog() == 1`, otherwise greyed preview), "Speed Readout", "Camera Damping", "OK". Init arrows on all active option buttons. |
| 1-2 | Present, reset counter |
| 3 | **Slide-in**: 39 frames. 5 buttons from alternating sides. |
| 4-5 | **Draw current values** (state 4 only): Resolution name from `DAT_004974bc[mode * 0x20]` (0x20-byte mode name strings, up to 50 modes at 0x4974BC-0x4978BC). Fogging on/off from `SNK_OnOffTxt[DAT_00466024]`. Speed readout from `SNK_SpeedReadTxt[DAT_00466028]`. Camera damping as integer from `DAT_0046602c`. Display options panel at right side of screen. |
| 6 | **Interactive**: Arrow handlers per button: |
|  | Button 0: Cycle `gConfiguredDisplayModeOrdinal` through available modes (scans string table for non-empty entries, wraps around) |
|  | Button 1: Toggle `DAT_00466024` (fog on/off, bitwise AND 1) |
|  | Button 2: Toggle `DAT_00466028` (speed readout MPH/KPH) |
|  | Button 3: Adjust `DAT_0046602c` in range [0, 9] (camera damping) |
|  | Any change resets to state 4 (redraw values). "OK" triggers exit to screen 0xC. |
| 7 | Prepare slide-out |
| 8 | **Slide-out**: 16 frames. |

### Options Summary

| Option | Global | Values |
|--------|--------|--------|
| Resolution | `gConfiguredDisplayModeOrdinal` | Index into mode name table at 0x4974BC (32-byte strings) |
| Fogging | `DAT_00466024` | 0=Off, 1=On (only if hardware supports it) |
| Speed Readout | `DAT_00466028` | 0/1 (MPH/KPH via `SNK_SpeedReadTxt`) |
| Camera Damping | `DAT_0046602c` | 0-9 integer range |

---

## 11. ScreenExtrasGallery (0x417d50)

**Purpose:** The extras/credits gallery viewer. Scrolls developer mugshots with credits text in a vertically-scrolling cylinder.

### State Machine (8 states)

| State | Action |
|-------|--------|
| 0 | **Fade transition**: Waits for `DAT_0048f2fc < -15` to advance. Clamps to range [-15, 64]. Handles wrap from music test (if `> 0xC0`, maps to `0x100 - value`). |
| 1 | **Load all resources**: Loads **22 developer mugshot TGAs** from `Extras\Mugshots.zip` into surfaces `DAT_004962E0-DAT_00496334`. Loads **5 legal page TGAs** (`Legals1-5.tga`) into `DAT_00496338-DAT_00496348`. Resets scroll position. |
| 2-5 | Skip frames (4 frames of setup) |
| 6 | Fill secondary surface with black, set scroll counter to 0x27F (639). |
| 7 | **Main scrolling loop**: Renders a **cylindrical scroll** of the current mugshot surface (`DAT_00496264`). The 320x320 image wraps vertically -- split into two BltFast calls when the scroll position crosses the image boundary. Every 32 pixels of scroll (`DAT_0049522c & 0x1F == 0`), renders one line of credits text. |

### Credits Text Rendering

Credits text comes from `SNK_CreditsText` (Language.dll export), with entries at 0x18 (24) byte stride.

**Special prefix characters:**
- `'#'` (0x23): **Mugshot reference**. The second character is an index into the mugshot surface table (`DAT_004961DC + char * 4`). Renders the mugshot via BltFast. Draws up to 7 sub-lines per mugshot entry (`DAT_00496364` counts 0-6).
- `'*'` (0x2A): **Section separator**. Increments `DAT_00496354` (section counter). When section counter reaches **0x0B (11)**, calls `DXWin::CleanUpAndPostQuit()` -- **exits the game!**
- Regular text: Centered on the scroll surface via `MeasureOrCenterFrontendLocalizedString` + `DrawFrontendLocalizedStringToSurface`.

### Exit Conditions

- ESC key (`DAT_004951f8 & 0x40000`)
- Any click (`DAT_00495258 != 0`)
- 11 section separators reached (`DAT_00496354 == 0xB`) -- natural end of credits

All three call `DXWin::CleanUpAndPostQuit()` -- the credits gallery **exits the game** when complete or cancelled. This is the "end credits" viewer.

### Developer Team (22 Mugshots)

From the TGA filenames in `Extras\Mugshots.zip`:
1. Bob, 2. Gareth, 3. Snake, 4. MikeT, 5. Chris, 6. Headley, 7. Steve, 8. Rich,
9. Mike, 10. Bez, 11. Les, 12. TonyP, 13. JohnS, 14. DavidT, 15. TonyC,
16. DaveyB, 17. ChrisD, 18. Slade, 19. Matt, 20. Marie, 21. JFK, 22. Daz

Plus 5 legal pages (Legals1-5.tga) for scrolling legal text at the end.

---

## 12. ScreenMusicTestExtras (0x418460)

**Purpose:** Music test / jukebox screen. Lets the player browse and play the game's CD audio soundtrack.

### State Machine (9 states)

| State | Action |
|-------|--------|
| 0 | **Fade transition + init**: Same fade handling as gallery. Calls `ReleaseExtrasGalleryImageSurfaces()` then `LoadExtrasBandGalleryImages()` (replaces gallery pics with band photos). Creates title (menu string 6), track name surface (0x170 x 0x28), now-playing surface (0x170 x 0x78). Draws initial track number (`"%d. %s"` format) and now-playing info (band name + song title). Background: MainMenu.tga. Buttons: "Select Track" (with arrows), "OK". |
| 1-2 | Present, reset counter |
| 3 | **Slide-in**: 39 frames |
| 4-5 | Static display (2 frames) |
| 6 | **Interactive**: |
|  | Arrow on button 0: Cycles `DAT_00465e14` through 0-11 (12 tracks). Redraws track name. |
|  | Confirm on button 0 ("Select Track"): Calls `DXSound::CDPlay(DAT_00465e14 + 2, 1)` -- plays CD audio track (offset by 2 because track 1 is data). Updates "Now Playing" panel with band name and song title. |
|  | Confirm on button 1 ("OK"): Sets `DAT_0048f2fc = 0x40` (fade value for transition), exits to screen 0xF. |
| 7 | Prepare slide-out |
| 8 | **Slide-out**: 32 frames. Releases surfaces. Calls `ReleaseExtrasGalleryImageSurfaces()` then `LoadExtrasGalleryImageSurfaces()` (restore normal gallery images). |

### Complete Soundtrack (12 CD Audio Tracks)

Reconstructed from the band name table at `0x465E1C` and track name table at `0x465E58`:

| # | Band | Song |
|---|------|------|
| 1 | GRAVITY KILLS | FALLING |
| 2 | KMFDM | ANARCHY |
| 3 | PITCHSHIFTER | GENIUS |
| 4 | PITCHSHIFTER | WYSIWYG |
| 5 | JUNKIE XL | DEF BEAT |
| 6 | FEAR FACTORY | 21ST CENTURY |
| 7 | FEAR FACTORY | GENETIC BLUEPRINT |
| 8 | GRAVITY KILLS | FALLING (DUB) |
| 9 | KMFDM | MEGALOMANIAC (DUB) |
| 10 | PITCHSHIFTER | GENIUS (DUB) |
| 11 | PITCHSHIFTER | MICROWAVED (DUB) |
| 12 | PITCHSHIFTER | WYSIWYG (DUB) |

CD audio offset: track index + 2 (track 1 is the data track on the mixed-mode CD).

### Band Gallery Images

When entering the music test, the normal extras gallery images (pic1-5.tga) are replaced with band photos:
- Fear Factory.tga
- Gravity Kills.tga
- Junkie XL.tga
- KMFDM.tga
- PitchShifter.tga

All loaded from `Front End\Extras\Extras.zip`. The gallery slideshow continues cycling these band photos while the music test is active.

---

## 13. LoadExtrasGalleryImageSurfaces (0x40d590)

**Purpose:** Loads the standard extras gallery slideshow images.

### Logic

- Checks `DAT_00495234 != 0` (extras unlocked flag).
- If unlocked: loads 5 images (`pic1.tga` through `pic5.tga`) from `Front End\Extras\Extras.zip` into surfaces `DAT_0048f2D4-DAT_0048f2E4`.
- Sets `DAT_0048f300 = 5` (image count), resets scroll/transition counters.
- Sets `DAT_004951dc = 0` (normal gallery mode).
- If locked: sets `DAT_0048f300 = 0` (no images).

---

## 14. LoadExtrasBandGalleryImages (0x40d6a0)

**Purpose:** Replaces gallery images with band photos for music test mode.

### Logic

Loads 5 band promotional photos into the same surface slots as the regular gallery:
1. `Fear Factory.tga` -> `DAT_0048f2D4`
2. `Gravity Kills.tga` -> `DAT_0048f2D8`
3. `Junkie XL.tga` -> `DAT_0048f2DC`
4. `KMFDM.tga` -> `DAT_0048f2E0`
5. `PitchShifter.tga` -> `DAT_0048f2E4`

Sets `DAT_004951dc = 2` (band gallery mode), `DAT_0048f300 = 5`, `DAT_00463c8c = -1`.

The mode flag (`DAT_004951dc`) values:
- **0**: Normal extras gallery (pic1-5)
- **1**: Developer credits gallery (mugshots) -- set in ScreenExtrasGallery
- **2**: Band photo gallery (music test)

---

## 15. ScreenSessionLockedDialog (0x41d630)

**Purpose:** Network error dialog shown when trying to join a locked multiplayer session.

### State Machine (6 states)

Identical structure to ScreenCupFailedDialog.

| State | Action |
|-------|--------|
| 0 | Load MainMenu.tga, create 0x198 x 0x70 dialog. Draw: `SNK_SorryTxt` (y=0) + `SNK_SeshLockedTxt` (y=0x38). Create "OK" button. |
| 1-3 | Present (3 frames) |
| 4 | Slide-in: 32 frames |
| 5 | Wait for confirm -> release, go to screen 5 (main menu) |

### Dialog Text

```
Sorry
[Session Locked message]
```

---

## Summary of Hidden Features and Easter Eggs

### Easter Egg: Secret Unlock Flag (AwardCupCompletionUnlocks)

Winning cup 0x12 (18) -- the final special cup -- while driving car 0x20 (32, an easter egg car) sets `_DAT_004962b0 = 1`. This is distinct from the normal car unlock path and represents a hidden achievement. The easter egg cars (FEAR FACTORY WAGON, THE MIGHTY MAUL, CHRIS'S BEAST, HOT DOG) must already be unlocked to use them, so this is a "bonus unlock" for dedicated players.

### Credits Gallery Exits the Game

The extras gallery (`ScreenExtrasGallery`) is actually the end-of-game credits sequence. Reaching the end (11 `*` separator markers in SNK_CreditsText) or pressing ESC/clicking calls `DXWin::CleanUpAndPostQuit()` which terminates the application. This is unusual -- most games return to the menu after credits.

### Dub Remix Tracks

6 of the 12 soundtrack entries are "(DUB)" remix versions, suggesting these are alternate mixes created specifically for the game or included as bonus content. Pitchshifter has the most representation with 5 tracks (3 regular + 2 dub).

### Band Photos in Gallery

The music test dynamically swaps the gallery slideshow images from developer artwork to band promotional photos, creating an integrated music video / jukebox experience. The same surface slots are reused, and they're swapped back when exiting.

---

## Global Variable Reference

| Address | Name | Purpose |
|---------|------|---------|
| `0x465E14` | `DAT_00465e14` | Current music test track index (0-11) |
| `0x465E18` | `DAT_00465e18` | Currently playing track index |
| `0x465E1C` | Band name ptr table | 12 pointers to band name strings |
| `0x465E58` | Track name ptr table | 12 pointers to track/song name strings |
| `0x4962E0-496348` | Mugshot surfaces | 22 developer mugshots + 5 legal pages |
| `0x48f2D4-48f2E4` | Gallery surfaces | 5 slideshow image surfaces (shared between modes) |
| `0x4951DC` | Gallery mode | 0=normal, 1=credits, 2=band photos |
| `0x48f300` | Image count | Number of loaded gallery images |
| `0x48f2FC` | Fade/scroll value | Crossfade position for gallery transitions |
| `0x496360` | Credits text index | Current line in SNK_CreditsText |
| `0x496364` | Credits sub-line | 0-6 counter for mugshot entries |
| `0x496354` | Section counter | Counts `*` separators in credits (exits at 11) |
| `0x4643BC` | High score table | 5 entries x 0x20 bytes per score category |
| `0x466840` | Max track count | Number of unlocked tracks |
| `0x463E4C` | Car lock table | Per-car lock bytes (0=unlocked) |
| `0x4668B0` | Track lock table | Per-track lock bytes (0=unlocked) |
| `0x496298` | Cheat/unlock flag | When non-zero, bypasses all lock checks |
| `0x4962B0` | Secret unlock flag | Set by winning cup 18 with easter egg car |
| `0x497A60-A98` | Race snapshot | Saved car/track/options for "Race Again" |
| `0x497A64` | Last button index | Stored menu choice in results screen |
| `0x497A6C` | Replay available | Non-zero if replay data exists |
| `0x497A70` | Cup complete flag | Set when no more cup races available |
| `0x497A74` | Skip results flag | Bypasses score display |
| `0x497A78` | Re-race flag | Tracks "Race Again" state for snapshot restore |
| `0x494BB0` | Unlocked car info | Non-zero if a car was just unlocked (for CupWon dialog) |
| `0x494BB4` | Unlocked track info | Non-zero if a track was just unlocked (for CupWon dialog) |
