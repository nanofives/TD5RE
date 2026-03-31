# g_frontendScreenFnTable -- Complete 30-Entry Reference

Table base address: `0x4655C4` (30 x 4-byte function pointers)

State variable: `g_frontendInnerState` (reset to 0 on screen entry via `SetFrontendScreen`)

## Index / Address / Function / Purpose

### Entry 0 -- 0x004269D0 -- ScreenLocalizationInit
- **Purpose:** Bootstrap/localization initialization. Loads LANGUAGE.DLL string table, car ZIP paths, config.td5 settings, enumerates display modes, seeds controller/input state from DXInput hardware detection.
- **States:** 1 (single-pass, state 0 only)
- **Entry condition:** First screen on startup (set by `ScreenStartupInit` entry 28) or re-entry after attract mode demo
- **Exit:** `SetFrontendScreen(5)` -> Main Menu; or `SetFrontendScreen(0x18)` -> Post-Race High Score if `DAT_004a2c8c == 2`
- **Key strings:** SNK_LangDLL, SNK_ConfMph, SNK_ConfSec, SNK_ConfRpm, SNK_ConfUnknown

### Entry 1 -- 0x00415030 -- ScreenPositionerDebugTool
- **Purpose:** Developer debug tool -- "SNK Positioner" for placing UI elements. Loads `Front_End/Positioner.tga`, displays a grid of character cells, allows cursor movement with arrow keys, and writes `positioner.txt` with coordinate/character data. Unreachable in shipping flow.
- **States:** 6 (0=load TGA, 1=present, 2=init grid, 3=navigate grid, 4=edit cell values, 5=write positioner.txt)
- **Entry condition:** No known caller in normal flow (debug-only)
- **Exit:** File write to `positioner.txt`, then returns
- **Key strings:** `s_Front_End_Positioner_tga`, `s_positioner_txt`, `s____Created_by_SNK_Positioner`

### Entry 2 -- 0x004275A0 -- RunAttractModeDemoScreen
- **Purpose:** Attract mode / demo playback screen. Fades to black, then launches the race simulation with `InitializeRaceSeriesSchedule` + `InitializeFrontendDisplayModeState`.
- **States:** 6 (0=set flag, 1=release buttons, 2-3=present, 4=init fade, 5=execute fade + launch)
- **Entry condition:** Timeout from main menu idle
- **Exit:** `InitializeRaceSeriesSchedule()` -> enters race state (game state 2)

### Entry 3 -- 0x00427290 -- ScreenLanguageSelect
- **Purpose:** Language selection screen. Loads `Front_End/Language.tga` and `Front_End/LanguageScreen.tga`, displays 4 flag-icon menu rectangles for language choice, uses string "LANGUAGE_SELECT".
- **States:** 7 (0=load assets, 1-2=tick/present, 3-5=interaction, 6=release + exit)
- **Entry condition:** `SetFrontendScreen(3)` from legal screen flow (entry 4)
- **Exit:** `SetFrontendScreen(4)` -> Legal Screen
- **Key strings:** SNK_LANGUAGE_SELECT

### Entry 4 -- 0x004274A0 -- ScreenLegalCopyright
- **Purpose:** Legal/copyright splash screen. Loads `Front_End/LegalScreen.tga`, draws "TEST_DRIVE_5_COPYRIGHT_1998" text, displays for 3 seconds with fade-in/fade-out.
- **States:** 4 (0=load + draw, 1=fade in, 2=3-second timer, 3=fade out + exit)
- **Entry condition:** `SetFrontendScreen(4)` from Language Select (entry 3)
- **Exit:** `SetFrontendScreen(5)` -> Main Menu
- **Key strings:** SNK_TEST_DRIVE_5_COPYRIGHT_1998

### Entry 5 -- 0x00415490 -- ScreenMainMenuAnd1PRaceFlow
- **Purpose:** Main menu hub and 1-player race configuration flow. Presents 7 buttons: Race Menu, Quick Race, Two Player (or Time Demo in dev build), Net Play, Options, Hi-Score, Exit. Also handles controller configuration setup for both player slots and applies game settings from options shadows into live globals.
- **States:** 24 (0=init+config, 1-2=present, 3=slide-in anim, 4=main interaction, 5-6=exit confirm dialog, 7=confirm exit, 8-onwards=slide-out transitions to sub-screens)
- **Entry condition:** From ScreenLocalizationInit (entry 0), from Back actions on sub-screens
- **Exit destinations by button:**
  - Button 0 (Race Menu): `g_returnToScreenIndex = 6` -> RaceTypeCategoryMenu
  - Button 1 (Quick Race): `g_returnToScreenIndex = 7` -> QuickRaceMenu
  - Button 2 (Two Player): `g_returnToScreenIndex = 0x14 (20)` -> Car Selection (sets `DAT_004962a0 = 1`)
  - Button 3 (Net Play): `g_returnToScreenIndex = 8` -> ConnectionBrowser
  - Button 4 (Options): `g_returnToScreenIndex = 0xC (12)` -> OptionsHub
  - Button 5 (Hi-Score): `g_returnToScreenIndex = 0x17 (23)` -> PostRaceHighScore
  - Button 6 (Exit): Yes/No confirm dialog -> `PostQuitMessage(0)`
- **Key strings:** SNK_RaceMenuButTxt, SNK_QuickRaceButTxt, SNK_TwoPlayerButTxt, SNK_NetPlayButTxt, SNK_OptionsButTxt, SNK_HiScoreButTxt, SNK_ExitButTxt, SNK_YesButTxt, SNK_NoxButTxt, SNK_MainMenu_MT

### Entry 6 -- 0x004168B0 -- RaceTypeCategoryMenuStateMachine
- **Purpose:** Race type selection (Single Race, Cup Race, Continue Cup, Time Trials, Drag Race, Cop Chase) plus category sub-menu for Cup Race. Validates `CupData.td5` checksum for Continue Cup availability.
- **States:** ~10 (0=init+buttons, 1=slide-in, 2=tick, 3=interaction, 4=category preview, 5=slide-out, 6=cup category sub-menu, 7+=sub-state transitions)
- **Entry condition:** `SetFrontendScreen(6)` from Main Menu button 0
- **Exit destinations by selection:**
  - Single Race (btn 0): `g_selectedGameType=0` -> `g_returnToScreenIndex = 0x14 (20)` Car Selection
  - Cup Race (btn 1): enters category sub-states (6-12) -> types 1-6, then Car Selection
  - Continue Cup (btn 2): `LoadContinueCupData()` -> `g_returnToScreenIndex = 0x18 (24)` Race Results
  - Time Trials (btn 3): `g_selectedGameType=7` -> Car Selection
  - Drag Race (btn 4): `g_selectedGameType=9` -> Car Selection
  - Cop Chase (btn 5): `g_selectedGameType=8` -> Car Selection
  - Back (btn 6): `g_returnToScreenIndex = 5 or 10` depending on `DAT_004962c0`
- **Key strings:** SNK_SingleRaceButTxt, SNK_CupRaceButTxt, SNK_ContCupButTxt, SNK_TimeTrialsButTxt, SNK_DragRaceButTxt, SNK_CopChaseButTxt, SNK_BackButTxt, SNK_RaceMenu_MT, SNK_RaceTypeText

### Entry 7 -- 0x004213D0 -- ScreenQuickRaceMenu
- **Purpose:** Quick Race setup screen. Presents car/track selection with Change Car and Change Track buttons plus OK/Back. Displays NPC group info and locked status. Uses `SNK_QuickRaceMenu_MT` inline string table.
- **States:** ~8 (0=init, 1-2=present, 3=slide-in, 4=interaction with car/track selection arrows, 5=slide-out prep, 6=slide-out, 7+=exit transitions)
- **Entry condition:** `SetFrontendScreen(7)` from Main Menu button 1
- **Exit:**
  - OK (btn 2): `g_returnToScreenIndex = -1` -> launches race (`InitializeRaceSeriesSchedule`)
  - Back (btn 3): `g_returnToScreenIndex = 5` -> Main Menu
- **Key strings:** SNK_ChangeCarButTxt, SNK_ChangeTrackButTxt, SNK_OkButTxt, SNK_BackButTxt, SNK_LockedTxt, SNK_QuickRaceMenu_MT

### Entry 8 -- 0x00418D50 -- RunFrontendConnectionBrowser
- **Purpose:** Network connection type browser. Enumerates DirectPlay service providers (`DXPlay::ConnectionEnumerate`), displays connection list with scroll, gets computer name for player identity. Gateway to multiplayer.
- **States:** ~10 (0=init+enum, 1=build list, 2=slide-in, 3=tick+render, 4=flash, 5=selection interaction, 6=highlight browse, 7=scroll, 8=slide-out)
- **Entry condition:** `SetFrontendScreen(8)` from Main Menu button 3
- **Exit:**
  - OK (btn 1): `g_returnToScreenIndex = 9` -> Session Picker
  - Back (btn 2): `g_returnToScreenIndex = 5` -> Main Menu
- **Key strings:** SNK_ChooseConnectionButTxt, SNK_OkButTxt, SNK_BackButTxt, `s_Clint_Eastwood` (default name)

### Entry 9 -- 0x00419CF0 -- RunFrontendSessionPicker
- **Purpose:** Network session browser. After connection is established, enumerates available game sessions (`DXPlay::ConnectionPick`). Shows session list with scroll arrows, allows joining or creating sessions.
- **States:** ~8 (0=init+pick, 1=build list, 2=slide-in, 3=interaction, 4=highlight browse, 5=slide-out confirm, 6=exit)
- **Entry condition:** `SetFrontendScreen(9)` from ConnectionBrowser OK
- **Exit:**
  - Create (btn 0): `g_frontendInnerState = 4` (create sub-flow)
  - OK/Join (btn 1): `g_returnToScreenIndex = 10` -> CreateSessionFlow
  - Back (btn 2): `g_returnToScreenIndex = 8` -> ConnectionBrowser
- **Key strings:** SNK_ChooseSessionButTxt, SNK_OkButTxt, SNK_BackButTxt

### Entry 10 -- 0x0041A7B0 -- RunFrontendCreateSessionFlow
- **Purpose:** Network session creation. Prompts for session name with text input widget (`RenderFrontendCreateSessionNameInput`), then creates/joins the DirectPlay session.
- **States:** ~18 (0=init, 1=slide-in, 2=name input, 3=slide-out, 4-15=session setup, 16+=fallback)
- **Entry condition:** `SetFrontendScreen(10)` from SessionPicker OK/join
- **Exit:**
  - After name entry: `g_returnToScreenIndex = 0` -> transitions to further network setup
  - Back: `g_returnToScreenIndex` varies
- **Key strings:** SNK_EnterNewSessionNameButTxt, SNK_BackButTxt

### Entry 11 -- 0x0041C330 -- RunFrontendNetworkLobby
- **Purpose:** Network multiplayer lobby. Renders player name rows from `dpu_exref+0xa64`, manages host/client status, ready state, and "sealed session" state. Drives host-only actions like starting the race.
- **States:** ~14 (0=init, 1-5=lobby interaction, 6+=transitions to car selection/race start)
- **Entry condition:** Network flow after session creation/joining
- **Exit:**
  - Host start: triggers race launch
  - Disconnect: `SetFrontendScreen(0x1D (29))` -> SessionLockedDialog
  - Back to car select: `SetFrontendScreen(0x14 (20))`
- **Key strings:** SNK_SorryTxt, SNK_SeshLockedTxt (via session locked path)

### Entry 12 -- 0x0041D890 -- ScreenOptionsHub
- **Purpose:** Options category selection hub. 6 buttons: Game Options, Control Options, Sound Options, Graphics Options, Two Player Options, OK.
- **States:** ~10 (0=init, 1-2=present, 3=slide-in, 4-5=anim, 6=interaction, 7=slide-out, 8+=exit)
- **Entry condition:** `SetFrontendScreen(0xC)` from Main Menu button 4
- **Exit by button:**
  - Game Options (btn 0): `g_returnToScreenIndex = 0xD (13)` -> ScreenGameOptions
  - Control Options (btn 1): `g_returnToScreenIndex = 0xE (14)` -> ScreenControlOptions
  - Sound Options (btn 2): `g_returnToScreenIndex = 0xF (15)` -> ScreenSoundOptions
  - Graphics Options (btn 3): `g_returnToScreenIndex = 0x10 (16)` -> ScreenDisplayOptions
  - Two Player Options (btn 4): `g_returnToScreenIndex = 0x11 (17)` -> ScreenTwoPlayerOptions
  - OK (btn 5): `g_returnToScreenIndex = 5` -> Main Menu (applies settings)
- **Key strings:** SNK_GameOptionsButTxt, SNK_ControlOptionsButTxt, SNK_SoundOptionsButTxt, SNK_GraphicsOptionsButTxt, SNK_TwoPlayerOptionsButTxt, SNK_OkButTxt, SNK_Options_MT

### Entry 13 -- 0x0041F990 -- ScreenGameOptions
- **Purpose:** Game options sub-screen. 7 toggle/arrow options: Circuit Laps, Checkpoint Timers, Traffic, Cops, Difficulty, Dynamics, 3D Collisions. Each row has left/right arrows for cycling values.
- **States:** ~10 (standard options screen pattern: init, present, slide-in, interaction, slide-out)
- **Entry condition:** `SetFrontendScreen(0xD)` from OptionsHub button 0
- **Exit:** OK -> returns to `g_returnToScreenIndex` (OptionsHub = 12)
- **Key strings:** SNK_CircuitLapsTxt, SNK_CheckpointTimersButTxt, SNK_TrafficButTxt, SNK_CopsButTxt, SNK_DifficultyButTxt, SNK_DynamicsButTxt, SNK_3dCollisionsButTxt, SNK_OkButTxt, SNK_GameOptions_MT

### Entry 14 -- 0x0041DF20 -- ScreenControlOptions
- **Purpose:** Controller options sub-screen. Displays current player 1 and player 2 input device assignments (keyboard, joystick type detection), with sub-flow branching to controller binding page.
- **States:** ~10 (standard options pattern)
- **Entry condition:** `SetFrontendScreen(0xE)` from OptionsHub button 1
- **Exit:** OK -> returns to `g_returnToScreenIndex` (OptionsHub = 12)
- **Key strings:** SNK_ControlText (multiple offsets for different controller types)

### Entry 15 -- 0x0041EA90 -- ScreenSoundOptions
- **Purpose:** Sound options sub-screen. 4 rows: SFX Mode, SFX Volume, Music Volume, Music Test. Volume bars rendered with VolumeBox.tga / VolumeFill.tga sprites. Music Test branches to entry 19 (ScreenMusicTestExtras).
- **States:** ~10 (standard options pattern)
- **Entry condition:** `SetFrontendScreen(0xF)` from OptionsHub button 2
- **Exit:**
  - Music Test (btn 3): -> `SetFrontendScreen(0x13 (19))` ScreenMusicTestExtras
  - OK (btn 4): returns to OptionsHub
- **Key strings:** SNK_SfxModeButTxt, SNK_SfxVolumeButTxt, SNK_MusicVolumeButTxt, SNK_MusicTestButTxt, SNK_OkButTxt, SNK_SfxOptions_MT

### Entry 16 -- 0x00420400 -- ScreenDisplayOptions
- **Purpose:** Display/graphics options sub-screen. 4 rows: Resolution (arrow-cycled from mode table), Fogging (toggle, greyed if HW can't fog), Speed Readout (MPH/KPH), Camera Damping.
- **States:** ~10 (standard options pattern)
- **Entry condition:** `SetFrontendScreen(0x10)` from OptionsHub button 3
- **Exit:** OK -> returns to `g_returnToScreenIndex` (OptionsHub = 12)
- **Key strings:** SNK_ResolutionButTxt, SNK_FoggingButTxt, SNK_SpeedReadoutButTxt, SNK_CameraDampingButTxt, SNK_OkButTxt, SNK_GfxOptions_MT

### Entry 17 -- 0x00420C70 -- ScreenTwoPlayerOptions
- **Purpose:** Two-player options sub-screen. 2 rows: Split Screen mode (horizontal/vertical), Catch-Up assist toggle. Loads `Front_End/SplitScreen.tga` for split mode preview.
- **States:** ~10 (standard options pattern)
- **Entry condition:** `SetFrontendScreen(0x11)` from OptionsHub button 4
- **Exit:** OK -> returns to `g_returnToScreenIndex` (OptionsHub = 12)
- **Key strings:** SNK_SplitScreenButTxt, SNK_CatchupTxt, SNK_OkButTxt, SNK_TwoOptions_MT

### Entry 18 -- 0x0040FE00 -- ScreenControllerBindingPage
- **Purpose:** Controller button binding/mapping sub-screen. Reads current joystick configuration, shows binding labels (Up/Down/Button text), supports multi-axis pedal detection (`SNK_PedalsTxt`), and writes mapped buttons back to the configuration array. Deep hardware-level input via `DXInput::GetJS`.
- **States:** ~20 (0=init+detect, 1-8=binding page setup, 9=slide-in, 10=interactive binding poll, 11-13=axis mapping, 14-18=confirm/exit, 19=special pedal sub-flow)
- **Entry condition:** From ScreenControlOptions (entry 14) when user selects "Configure" for a device
- **Exit:** Returns to `g_returnToScreenIndex` (ControlOptions = 14)
- **Key strings:** SNK_PressingTxt, SNK_ConfigurationTxt, SNK_ButtonTxt, SNK_UpDownTxt, SNK_DownTxt, SNK_UpTxt, SNK_PedalsTxt, SNK_ControlText (multiple offsets), SNK_OkButTxt

### Entry 19 -- 0x00418460 -- ScreenMusicTestExtras
- **Purpose:** Music test / CD audio jukebox screen. Loads band gallery images (`LoadExtrasBandGalleryImages`), displays track number with artist name (from `PTR_s_GRAVITY_KILLS` table), song title (from `PTR_s_FALLING` table), and "Now Playing" label. Supports "Select Track" and OK buttons. Plays CD audio tracks via `DXSound::CDPlay`.
- **States:** ~10 (0=transition gate, 1-2=present, 3=slide-in, 4-5=anim, 6=interaction with track cycling, 7=slide-out, 8+=exit)
- **Entry condition:** `SetFrontendScreen(0x13)` from ScreenSoundOptions Music Test button
- **Exit:** Returns to `g_returnToScreenIndex` (SoundOptions = 15)
- **Key strings:** SNK_SelectTrackButTxt, SNK_OkButTxt, SNK_NowPlayingTxt, SNK_MusicTest_MT, PTR_s_GRAVITY_KILLS (artist names), PTR_s_FALLING (song titles), `s__d___s` (format string for track number)

### Entry 20 -- 0x0040DFC0 -- CarSelectionScreenStateMachine
- **Purpose:** Car selection screen. Displays car model preview, handles locked/unlocked car cycling, supports both 1P and 2P car picking. Masters mode (type 5) has special car roster filtering. Loads per-car TGA/model assets.
- **States:** ~15+ (0=init+car roster, then multi-state car browsing, network 2P sub-flows)
- **Entry condition:** `SetFrontendScreen(0x14)` from RaceTypeCategory or MainMenu (2P/Quick Race paths)
- **Exit:** OK -> Track Selection (entry 21) or race launch depending on flow
- **Key strings:** Car asset paths from `gCarZipPathTable`

### Entry 21 -- 0x00427630 -- TrackSelectionScreenStateMachine
- **Purpose:** Track selection screen. Cycles through available tracks with NPC group validation (skips non-circuit groups for certain modes). Displays track name, preview. Supports locked track gating via NPC group table byte checks.
- **States:** ~10+ (0=init+track validation, then browse/select states)
- **Entry condition:** `SetFrontendScreen(0x15)` from Car Selection
- **Exit:** OK -> launches race; Back -> Car Selection

### Entry 22 -- 0x00417D50 -- ScreenExtrasGallery
- **Purpose:** Extras/Credits gallery. Loads 20+ developer mugshot TGAs from `Extras_Mugshots.zip` (Bob, Gareth, Snake, MikeT, Chris, Headley, Steve, Rich, Mike, Bez, Les, TonyP, JohnS, DavidT, TonyC, DaveyB, ChrisD, Slade, Matt, Marie, JFK, Daz). Scrollable team member gallery with photo display.
- **States:** ~10 (0=transition gate, 1=load all mugshots, 2+=browse gallery)
- **Entry condition:** `SetFrontendScreen(0x16)` from Main Menu Extras flow
- **Exit:** Back -> Main Menu (entry 5)
- **Key strings:** `s_Front_End_Extras_*_tga` (20+ developer names), `s_Front_End_Extras_Mugshots_zip`

### Entry 23 -- 0x00413580 -- ScreenPostRaceHighScoreTable
- **Purpose:** High score / records table. Shows track-by-track best times/scores with left/right browsing. Uses `DrawPostRaceHighScoreEntry` to render per-track records. Supports both normal track range and unlocked range via `DAT_00466840`.
- **States:** ~10 (0=init, 1-2=present, 3=slide-in, 4-5=anim, 6=interaction with track cycling, 7=slide-out, 8-9+=exit transition)
- **Entry condition:** `SetFrontendScreen(0x17)` from Main Menu Hi-Score button (button 5)
- **Exit:**
  - OK (btn 1): `g_returnToScreenIndex = 5` -> Main Menu
  - Also: `g_returnToScreenIndex = -1` -> `InitializeRaceSeriesSchedule()` (race launch, if from post-race flow)
- **Key strings:** SNK_OkButTxt, SNK_Options_MT

### Entry 24 -- 0x00422480 -- RunRaceResultsScreen
- **Purpose:** Post-race results screen. Sorts results by game type (primary metric ascending for cups 2-5, secondary metric descending for championship/ultimate types 1&6), displays race data summary with per-racer statistics. Handles win/loss routing to cup series progression or failure screens.
- **States:** ~22 (0=init+sort, 1-2=present, 3=slide-in, 4-5=anim, 6=interaction with racer browsing, 7-10=scroll animation, 11=slide-out, 12=cleanup, 13+=series progression logic including continue cup, save snapshot, next race routing)
- **Entry condition:** `SetFrontendScreen(0x18)` from race completion or Continue Cup (entry 6 btn 2)
- **Exit:**
  - Series lost: `SetFrontendScreen(0x1A (26))` -> CupFailedDialog
  - Win cup: `SetFrontendScreen(0x1B (27))` -> CupWonDialog
  - Back to main: `SetFrontendScreen(5)` (for single race / time trial)
  - Next race in series: `InitializeRaceSeriesSchedule()` -> race
  - Network: `SetFrontendScreen(0xB (11))` -> NetworkLobby
- **Key strings:** SNK_ResultsTxt, SNK_CCResultsTxt, SNK_DRResultsTxt, SNK_MusicTest_MT, SNK_RaceTypeText

### Entry 25 -- 0x00413BC0 -- ScreenPostRaceNameEntry
- **Purpose:** Post-race high score name entry. Triggered when player achieves a new record (DAT_004951d0 != 0). Shows name entry text input with `RenderFrontendCreateSessionNameInput`, validates against NPC group threshold tables to determine if the score qualifies.
- **States:** ~8 (0=init+threshold check, 1=slide-in, 2=name input, 3=slide-out, 4=NPC insertion + ranking, 5+=exit)
- **Entry condition:** From RunRaceResultsScreen (entry 24) when a qualifying score is achieved
- **Exit:** `SetFrontendScreen(g_returnToScreenIndex)` -- typically 0 (ScreenLocalizationInit for re-init) or other post-race screen
- **Key strings:** SNK_EnterPlayerNameButTxt, SNK_LockedTxt, SNK_Options_MT

### Entry 26 -- 0x004237F0 -- ScreenCupFailedDialog
- **Purpose:** "Sorry, You Failed To Win" dialog for cup/series mode. Only activates for game types 1-6 (cup modes). Shows SNK_SorryTxt + SNK_YouFailedTxt + SNK_ToWinTxt + race type name. Single OK button.
- **States:** 6 (0=init or redirect, 1-3=present, 4=slide-in, 5=wait for OK + exit)
- **Entry condition:** `SetFrontendScreen(0x1A)` from RunRaceResultsScreen when player loses cup series
- **Exit:**
  - OK: `SetFrontendScreen(g_returnToScreenIndex)` -- typically 5 (Main Menu)
  - For non-cup types: immediately `SetFrontendScreen(5)` -> Main Menu
- **Key strings:** SNK_SorryTxt, SNK_YouFailedTxt, SNK_ToWinTxt, SNK_RaceTypeText, SNK_OkButTxt

### Entry 27 -- 0x00423A80 -- ScreenCupWonDialog
- **Purpose:** "Congratulations, You Have Won" dialog for cup/series victory. Only activates for game types 1-6. Shows SNK_CongratsTxt + SNK_YouHaveWonTxt + race type name. Deletes `CupData.td5` save (`_unlink`). May show bonus unlocked car/track messages.
- **States:** 6 (0=init+delete save, 1-3=present, 4=slide-in, 5=wait for OK + exit)
- **Entry condition:** `SetFrontendScreen(0x1B)` from RunRaceResultsScreen when player wins cup series
- **Exit:**
  - OK: `SetFrontendScreen(g_returnToScreenIndex)` -- typically 5 (Main Menu)
  - For non-cup types: immediately `SetFrontendScreen(5)` -> Main Menu
- **Key strings:** SNK_CongratsTxt, SNK_YouHaveWonTxt, SNK_RaceTypeText, SNK_OkButTxt, `s_CupData_td5` (deleted), `s__d__s` (bonus unlock format)

### Entry 28 -- 0x00415370 -- ScreenStartupInit
- **Purpose:** Very first screen -- minimal startup initialization. Creates a small surface, shows an OK button, then transfers control to entry 0 (ScreenLocalizationInit) by setting `g_frontendScreenFn = g_frontendScreenFnTable[0]`.
- **States:** 5 (0=create surface+button, 1-3=present blank, 4=release+redirect to entry 0)
- **Entry condition:** Game launch (set as initial screen by frontend dispatcher)
- **Exit:** Directly sets `g_frontendScreenFn = g_frontendScreenFnTable` (entry 0), resets `g_frontendInnerState = 0`, timestamps frame
- **Key strings:** SNK_OkButTxt

### Entry 29 -- 0x0041D630 -- ScreenSessionLockedDialog
- **Purpose:** "Sorry, Session Locked" network error dialog. Shown when player tries to join a sealed/locked multiplayer session. Displays SNK_SorryTxt + SNK_SeshLockedTxt, single OK button.
- **States:** 6 (0=init+draw, 1-3=present, 4=slide-in, 5=wait for OK + exit)
- **Entry condition:** `SetFrontendScreen(0x1D)` from RunFrontendNetworkLobby on disconnect/locked
- **Exit:** `SetFrontendScreen(5)` -> Main Menu
- **Key strings:** SNK_SorryTxt, SNK_SeshLockedTxt, SNK_OkButTxt


## Navigation Flow Summary

```
 [28] ScreenStartupInit
   |
   v
 [0] ScreenLocalizationInit --> [5] MainMenu (or [24] if resume cup)
                                  |
          +---------+-------------+-----------+-----------+-----------+
          |         |             |           |           |           |
         [6]       [7]          [20]        [8]        [12]        [23]
     RaceType  QuickRace     CarSelect  Connection  OptionsHub  HiScore
      Menu      Menu            |       Browser
       |         |              v           |
       |         +---> race   [21]         [9]
       |                    TrackSelect  SessionPick
       +---[20]-->[21]-->race   |           |
       |                        v          [10]
      [24]<--- race end ---- race       CreateSesh
    Results                                 |
       |                                   [11]
       +--->[26] CupFailed               Lobby
       +--->[27] CupWon                    |
       +--->[25] NameEntry          [20]->[21]->race
       |
      [12] OptionsHub
       +--->[13] GameOptions
       +--->[14] ControlOptions --> [18] ControllerBinding
       +--->[15] SoundOptions   --> [19] MusicTest
       +--->[16] DisplayOptions
       +--->[17] TwoPlayerOptions

  Standalone/Special:
   [1]  PositionerDebugTool (unreachable in shipping)
   [2]  AttractModeDemo (idle timeout)
   [3]  LanguageSelect --> [4] LegalCopyright --> [5] MainMenu
   [22] ExtrasGallery
   [29] SessionLockedDialog
```

## Index Quick-Reference

| Idx | Hex | Address    | Function Name                       | Category      |
|-----|-----|------------|-------------------------------------|---------------|
|  0  | 00  | 0x4269D0   | ScreenLocalizationInit              | Bootstrap     |
|  1  | 01  | 0x415030   | ScreenPositionerDebugTool           | Debug         |
|  2  | 02  | 0x4275A0   | RunAttractModeDemoScreen            | Attract       |
|  3  | 03  | 0x427290   | ScreenLanguageSelect                | Bootstrap     |
|  4  | 04  | 0x4274A0   | ScreenLegalCopyright                | Bootstrap     |
|  5  | 05  | 0x415490   | ScreenMainMenuAnd1PRaceFlow         | Main Menu     |
|  6  | 06  | 0x4168B0   | RaceTypeCategoryMenuStateMachine    | Race Setup    |
|  7  | 07  | 0x4213D0   | ScreenQuickRaceMenu                 | Race Setup    |
|  8  | 08  | 0x418D50   | RunFrontendConnectionBrowser        | Network       |
|  9  | 09  | 0x419CF0   | RunFrontendSessionPicker            | Network       |
| 10  | 0A  | 0x41A7B0   | RunFrontendCreateSessionFlow        | Network       |
| 11  | 0B  | 0x41C330   | RunFrontendNetworkLobby             | Network       |
| 12  | 0C  | 0x41D890   | ScreenOptionsHub                    | Options       |
| 13  | 0D  | 0x41F990   | ScreenGameOptions                   | Options       |
| 14  | 0E  | 0x41DF20   | ScreenControlOptions                | Options       |
| 15  | 0F  | 0x41EA90   | ScreenSoundOptions                  | Options       |
| 16  | 10  | 0x420400   | ScreenDisplayOptions                | Options       |
| 17  | 11  | 0x420C70   | ScreenTwoPlayerOptions              | Options       |
| 18  | 12  | 0x40FE00   | ScreenControllerBindingPage         | Options       |
| 19  | 13  | 0x418460   | ScreenMusicTestExtras               | Options       |
| 20  | 14  | 0x40DFC0   | CarSelectionScreenStateMachine      | Race Setup    |
| 21  | 15  | 0x427630   | TrackSelectionScreenStateMachine    | Race Setup    |
| 22  | 16  | 0x417D50   | ScreenExtrasGallery                 | Extras        |
| 23  | 17  | 0x413580   | ScreenPostRaceHighScoreTable        | Post-Race     |
| 24  | 18  | 0x422480   | RunRaceResultsScreen                | Post-Race     |
| 25  | 19  | 0x413BC0   | ScreenPostRaceNameEntry             | Post-Race     |
| 26  | 1A  | 0x4237F0   | ScreenCupFailedDialog               | Post-Race     |
| 27  | 1B  | 0x423A80   | ScreenCupWonDialog                  | Post-Race     |
| 28  | 1C  | 0x415370   | ScreenStartupInit                   | Bootstrap     |
| 29  | 1D  | 0x41D630   | ScreenSessionLockedDialog           | Network       |
