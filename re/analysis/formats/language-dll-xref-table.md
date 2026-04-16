# Language.dll SNK_ Import Cross-Reference Table

**Date:** 2026-03-19
**Binary:** TD5_d3d.exe (port 8193)
**IAT range:** 0x45D128 -- 0x45D3C0 (167 entries)
**Total xrefs:** 370 READ references across ~30 distinct functions

## IAT Layout

Language.dll import descriptor in PE import directory:
- **Import Name Table (INT):** VA 0x45F8D8
- **First Thunk (IAT):** VA 0x45D128
- **DLL name string:** VA 0x461D24

Each IAT slot is 4 bytes containing a pointer resolved by the PE loader at startup. Code references these as `[0x45Dxxx]` to dereference to Language.dll `.data` section.

---

## Complete Cross-Reference Table

### 1. LOCALIZATION / STARTUP (ScreenLocalizationInit, 0x4269D0)

| IAT Addr | Symbol | Xrefs | Sites |
|---|---|---|---|
| 0x45D1E4 | SNK_LangDLL | 9 | CreateMenuStringLabelSurface(0x412e36), InitializeFrontendResourcesAndState(0x41498d), DrawFrontendLocalizedStringPrimary(0x4242b3), DrawFrontendLocalizedStringSecondary(0x424393), DrawFrontendLocalizedStringToSurface(0x424563), MeasureOrCenterFrontendLocalizedString(0x424a50), ScreenLocalizationInit(0x426a3e, 0x426a76), InitializeRaceVideoConfiguration(0x42a950) |
| 0x45D14C | SNK_Layout_Types | 1 | ScreenLocalizationInit(0x426b55) |
| 0x45D150 | SNK_Engine_Types | 1 | ScreenLocalizationInit(0x426bc2) |
| 0x45D154 | SNK_ConfSpeed | 1 | ScreenLocalizationInit(0x426c35) |
| 0x45D158 | SNK_ConfMph | 1 | ScreenLocalizationInit(0x426ca9) |
| 0x45D15C | SNK_ConfSec | 1 | ScreenLocalizationInit(0x426d12) |
| 0x45D160 | SNK_ConfFt | 1 | ScreenLocalizationInit(0x426d8f) |
| 0x45D164 | SNK_ConfRpm | 1 | ScreenLocalizationInit(0x426e61) |
| 0x45D168 | SNK_ConfUnknown | 1 | ScreenLocalizationInit(0x426f63) |

**Notes:** SNK_LangDLL is the most widely-referenced Language.dll symbol (9 xrefs). It provides the language index byte at position [8] for locale selection, and is also used by all four text-rendering functions to validate the DLL is loaded. The ConfSpeed/Mph/Sec/Ft/Rpm/Unknown unit suffixes are only used during car config parsing at startup.

---

### 2. MAIN MENU (ScreenMainMenuAnd1PRaceFlow, 0x415490)

| IAT Addr | Symbol | Xrefs | Sites |
|---|---|---|---|
| 0x45D230 | SNK_MainMenu_MT | 2 | ScreenMainMenuAnd1PRaceFlow(0x41592c, 0x415e49) |
| 0x45D21C | SNK_MainMenu_Ex | 1 | ScreenMainMenuAnd1PRaceFlow(0x41651f) |
| 0x45D250 | SNK_RaceMenuButTxt | 1 | ScreenMainMenuAnd1PRaceFlow(0x415682) |
| 0x45D24C | SNK_QuickRaceButTxt | 1 | ScreenMainMenuAnd1PRaceFlow(0x41569c) |
| 0x45D248 | SNK_TwoPlayerButTxt | 1 | ScreenMainMenuAnd1PRaceFlow(0x4156d8) |
| 0x45D244 | SNK_TimeDemoButTxt | 1 | ScreenMainMenuAnd1PRaceFlow(0x4158a5) |
| 0x45D240 | SNK_NetPlayButTxt | 1 | ScreenMainMenuAnd1PRaceFlow(0x4158b0) |
| 0x45D23C | SNK_OptionsButTxt | 1 | ScreenMainMenuAnd1PRaceFlow(0x4158cd) |
| 0x45D238 | SNK_HiScoreButTxt | 1 | ScreenMainMenuAnd1PRaceFlow(0x4158e7) |
| 0x45D234 | SNK_ExitButTxt | 2 | ScreenMainMenuAnd1PRaceFlow(0x415900), RunFrontendNetworkLobby(0x41c505) |
| 0x45D22C | SNK_YesButTxt | 2 | ScreenMainMenuAnd1PRaceFlow(0x415cab), RunFrontendNetworkLobby(0x41cdfe) |
| 0x45D228 | SNK_NoxButTxt | 2 | ScreenMainMenuAnd1PRaceFlow(0x415cdc), RunFrontendNetworkLobby(0x41ce23) |
| 0x45D218 | SNK_MustSelectTxt | 2 | ScreenMainMenuAnd1PRaceFlow(0x416616, 0x416629) |
| 0x45D224 | SNK_MustPlayer1Txt | 2 | ScreenMainMenuAnd1PRaceFlow(0x416138, 0x41615d) |
| 0x45D220 | SNK_MustPlayer2Txt | 1 | ScreenMainMenuAnd1PRaceFlow(0x416178) |
| 0x45D1E0 | SNK_MenuStrings | 2 | CreateMenuStringLabelSurface(0x412f85, 0x412fcc) |

**Notes:** SNK_ExitButTxt is reused in the network lobby (for "EXIT" session button). SNK_YesButTxt/NoxButTxt are shared between main menu exit confirmation and network lobby. SNK_MenuStrings provides screen title strings used by the generic menu label renderer.

---

### 3. RACE TYPE / CATEGORY MENU (RaceTypeCategoryMenuStateMachine, 0x416920)

| IAT Addr | Symbol | Xrefs | Sites |
|---|---|---|---|
| 0x45D28C | SNK_SingleRaceButTxt | 2 | RaceTypeCategoryMenuStateMachine(0x41694a, 0x417a7d) |
| 0x45D288 | SNK_CupRaceButTxt | 2 | RaceTypeCategoryMenuStateMachine(0x41696c, 0x417a98) |
| 0x45D284 | SNK_ContCupButTxt | 4 | RaceTypeCategoryMenuStateMachine(0x4169a3, 0x4169b1, 0x417ad0, 0x417ade) |
| 0x45D280 | SNK_TimeTrialsButTxt | 2 | RaceTypeCategoryMenuStateMachine(0x4169bc, 0x417ae9) |
| 0x45D27C | SNK_DragRaceButTxt | 2 | RaceTypeCategoryMenuStateMachine(0x4169d9, 0x417b08) |
| 0x45D278 | SNK_CopChaseButTxt | 2 | RaceTypeCategoryMenuStateMachine(0x4169f3, 0x417b24) |
| 0x45D268 | SNK_EraButTxt | 1 | RaceTypeCategoryMenuStateMachine(0x417201) |
| 0x45D264 | SNK_ChallengeButTxt | 3 | RaceTypeCategoryMenuStateMachine(0x417229, 0x4172b3, 0x417316) |
| 0x45D260 | SNK_PitbullButTxt | 3 | RaceTypeCategoryMenuStateMachine(0x417243, 0x4172be, 0x417322) |
| 0x45D25C | SNK_MastersButTxt | 3 | RaceTypeCategoryMenuStateMachine(0x41725e, 0x4172da, 0x41733e) |
| 0x45D258 | SNK_UltimateButTxt | 3 | RaceTypeCategoryMenuStateMachine(0x41727a, 0x4172f6, 0x417359) |
| 0x45D26C | SNK_ChampionshipButTxt | 1 | RaceTypeCategoryMenuStateMachine(0x4171e6) |
| 0x45D270 | SNK_RaceTypeText | 6 | RaceTypeCategoryMenuStateMachine(0x416e58, 0x417642), 0x423901, 0x423929, 0x423b76, 0x423b9e |
| 0x45D274 | SNK_RaceMenu_MT | 2 | RaceTypeCategoryMenuStateMachine(0x416a26, 0x417b5f) |
| 0x45D254 | SNK_RaceMen2_MT | 1 | RaceTypeCategoryMenuStateMachine(0x417398) |
| 0x45D1A0 | SNK_BackButTxt | 11 | CarSelectionScreenStateMachine(0x40e4b2), RaceTypeCategoryMenuStateMachine(0x416a0c, 0x417378, 0x417b3f), RunFrontendConnectionBrowser(0x418f0c), RunFrontendSessionPicker(0x419df9), RunFrontendCreateSessionFlow(0x41a8b1, 0x41ab89, 0x41af36), 0x42161a, TrackSelectionScreenStateMachine(0x4277c6) |

**Notes:** SNK_BackButTxt has the **highest xref count** of any button label (11 references) -- it appears in virtually every navigable screen. SNK_ContCupButTxt has 4 xrefs (displayed in both race type menus, checked twice for state). Cup type buttons (Challenge through Ultimate) have 3 xrefs each because they appear in the category sub-menu, are read for layout, and again for state transitions.

---

### 4. CAR SELECTION (CarSelectionScreenStateMachine, 0x40E3C0)

| IAT Addr | Symbol | Xrefs | Sites |
|---|---|---|---|
| 0x45D19C | SNK_CarSelect_MT1 | 1 | CarSelectionScreenStateMachine(0x40e4f0) |
| 0x45D198 | SNK_CarSelect_Ex | 2 | CarSelectionScreenStateMachine(0x40e514, 0x40e51d) |
| 0x45D1B8 | SNK_CarButTxt | 1 | CarSelectionScreenStateMachine(0x40e3ec) |
| 0x45D1B4 | SNK_PaintButTxt | 1 | CarSelectionScreenStateMachine(0x40e40d) |
| 0x45D1B0 | SNK_ConfigButTxt | 1 | CarSelectionScreenStateMachine(0x40e426) |
| 0x45D1AC | SNK_ManualButTxt | 2 | CarSelectionScreenStateMachine(0x40e45b, 0x40ea5a) |
| 0x45D1A8 | SNK_AutoButTxt | 2 | CarSelectionScreenStateMachine(0x40e476, 0x40ea72) |
| 0x45D1A4 | SNK_OkButTxt | 24 | **MOST REFERENCED SNK_ SYMBOL** -- CarSelectionScreenStateMachine, OpenControllerBindingPageNoneHeader, ScreenStartupInit, ScreenMainMenuAnd1PRaceFlow, 0x418655, RunFrontendConnectionBrowser, RunFrontendSessionPicker, RunFrontendNetworkLobby, 0x41d70d, ScreenOptionsHub, ScreenControlOptions, ScreenSoundOptions, ScreenGameOptions, ScreenDisplayOptions, ScreenTwoPlayerOptions, 0x421601, RunRaceResultsScreen(x3), 0x42393c, 0x423c54, TrackSelectionScreenStateMachine |
| 0x45D194 | SNK_BeautyTxt | 1 | CarSelectionScreenStateMachine(0x40ec4a) |
| 0x45D190 | SNK_BeastTxt | 1 | CarSelectionScreenStateMachine(0x40ec5b) |
| 0x45D18C | SNK_LockedTxt | 6 | CarSelectionScreenStateMachine(0x40ec8e), 0x42157e, 0x4215b0, 0x421a4d, TrackSelectionScreenStateMachine(0x427e59, 0x427e6f) |
| 0x45D188 | SNK_Config_Hdrs | 8 | CarSelectionScreenStateMachine(0x40ee67..0x40f0e0) |
| 0x45D184 | SNK_Info_Values | 2 | CarSelectionScreenStateMachine(0x40f255, 0x40f267) |

**Notes:** SNK_OkButTxt ("OK") is the **single most referenced** Language.dll symbol with 24 xrefs across the entire codebase -- every screen that has a confirmation/proceed button uses it. SNK_LockedTxt is used in both car and track selection to mark locked content. SNK_Config_Hdrs has 8 xrefs mapping to the 8 stat category headers (LAYOUT, GEARS, TIRES, etc.) displayed in car info panels.

---

### 5. TRACK SELECTION (TrackSelectionScreenStateMachine, 0x427730)

| IAT Addr | Symbol | Xrefs | Sites |
|---|---|---|---|
| 0x45D20C | SNK_TrackNames | 8 | 0x4136b0, 0x413928, 0x41415e, 0x42152e, 0x421904, TrackSelectionScreenStateMachine(0x427cc8, 0x427d19, 0x427d60) |
| 0x45D138 | SNK_TrackButTxt | 1 | TrackSelectionScreenStateMachine(0x427768) |
| 0x45D13C | SNK_ForwardsButTxt | 3 | TrackSelectionScreenStateMachine(0x427782, 0x427b21, 0x427c37) |
| 0x45D140 | SNK_TrackSel_MT1 | 1 | TrackSelectionScreenStateMachine(0x4277f6) |
| 0x45D144 | SNK_TrackSel_Ex | 1 | TrackSelectionScreenStateMachine(0x427803) |
| 0x45D148 | SNK_BackwardsButTxt | 1 | TrackSelectionScreenStateMachine(0x427c1a) |

**Notes:** SNK_TrackNames has 8 xrefs: 3 in TrackSelectionScreenStateMachine (display), plus 5 in unnamed functions around 0x4136b0-0x421904. The unnamed sites are likely the high-score table (near DrawPostRaceHighScoreEntry), results screen, and quick race preview functions that need to display track names.

---

### 6. CONTROLLER / INPUT OPTIONS (ScreenControlOptions + binding pages)

| IAT Addr | Symbol | Xrefs | Sites |
|---|---|---|---|
| 0x45D1C4 | SNK_ControlText | 11 | OpenControllerBindingPageRearViewHeader(0x4100e4), FUN_004100fa(0x4100ff), FUN_00410111(0x410117), OpenControllerBindingPageNoneHeader(0x410149), RenderControllerBindingMenuPage(0x41039f), RenderControllerBindingPageUpDownHeader(0x410441), RenderControllerBindingPageDownHeader(0x4104b8), RenderControllerBindingPageUpHeader(0x41052d), RenderControllerBindingPageBlankOrRearViewHeader(0x4105d6), DrawControlOptionsBindingHeader(0x410968, 0x410988) |
| 0x45D1C8 | SNK_UpDownTxt | 2 | 0x4100bb, RenderControllerBindingPageUpDownHeader(0x41049a) |
| 0x45D1CC | SNK_DownTxt | 2 | 0x4100a1, RenderControllerBindingPageDownHeader(0x41050e) |
| 0x45D1D0 | SNK_UpTxt | 2 | 0x410088, RenderControllerBindingPageUpHeader(0x410583) |
| 0x45D1D4 | SNK_PedalsTxt | 1 | 0x410053 |
| 0x45D1D8 | SNK_ConfigurationTxt | 2 | 0x40ff66, 0x40ff82 |
| 0x45D1DC | SNK_PressingTxt | 2 | 0x40ff37, 0x40ff55 |
| 0x45D1BC | SNK_PressKeyTxt | 2 | 0x4108af, 0x4108c8 |
| 0x45D1C0 | SNK_ButtonTxt | 1 | 0x41028d |
| 0x45D320 | SNK_CtrlOptions_MT | 1 | ScreenControlOptions(0x41e11d) |
| 0x45D31C | SNK_CtrlOptions_Ex | 1 | ScreenControlOptions(0x41e153) |
| 0x45D318 | SNK_Ctrl_Modes | 4 | ScreenControlOptions(0x41e400, 0x41e424, 0x41e472, 0x41e496) |
| 0x45D328 | SNK_ConfigureButTxt | 2 | ScreenControlOptions(0x41e08f, 0x41e0c9) |
| 0x45D32C | SNK_Player1ButTxt | 1 | ScreenControlOptions(0x41e073) |
| 0x45D324 | SNK_Player2ButTxt | 1 | ScreenControlOptions(0x41e0ad) |

**Notes:** SNK_ControlText (the 10-entry action name array) has 11 xrefs -- one of the highest counts. It's read by every controller binding page variant to display action labels (LEFT, RIGHT, ACCELERATE, BRAKE, etc.). SNK_Ctrl_Modes has 4 xrefs (read twice for each of the two player columns).

---

### 7. OPTIONS SCREENS

#### 7a. Options Hub (ScreenOptionsHub, 0x41D890)

| IAT Addr | Symbol | Xrefs | Sites |
|---|---|---|---|
| 0x45D314 | SNK_GameOptionsButTxt | 1 | ScreenOptionsHub(0x41d8e3) |
| 0x45D310 | SNK_ControlOptionsButTxt | 1 | ScreenOptionsHub(0x41d8fe) |
| 0x45D30C | SNK_SoundOptionsButTxt | 1 | ScreenOptionsHub(0x41d91a) |
| 0x45D308 | SNK_GraphicsOptionsButTxt | 1 | ScreenOptionsHub(0x41d936) |
| 0x45D304 | SNK_TwoPlayerOptionsButTxt | 1 | ScreenOptionsHub(0x41d954) |
| 0x45D210 | SNK_Options_MT | 3 | 0x413648, 0x413d7c, ScreenOptionsHub(0x41d99b) |

#### 7b. Game Options (ScreenGameOptions, 0x41F9E0)

| IAT Addr | Symbol | Xrefs | Sites |
|---|---|---|---|
| 0x45D370 | SNK_CircuitLapsTxt | 1 | ScreenGameOptions(0x41fa0e) |
| 0x45D36C | SNK_CheckpointTimersButTxt | 1 | ScreenGameOptions(0x41fa29) |
| 0x45D368 | SNK_TrafficButTxt | 1 | ScreenGameOptions(0x41fa48) |
| 0x45D364 | SNK_CopsButTxt | 1 | ScreenGameOptions(0x41fa64) |
| 0x45D360 | SNK_DifficultyButTxt | 1 | ScreenGameOptions(0x41fa7f) |
| 0x45D35C | SNK_DynamicsButTxt | 1 | ScreenGameOptions(0x41fa9e) |
| 0x45D358 | SNK_3dCollisionsButTxt | 1 | ScreenGameOptions(0x41faba) |
| 0x45D354 | SNK_GameOptions_MT | 1 | ScreenGameOptions(0x41fb42) |
| 0x45D350 | SNK_OnOffTxt | 10 | ScreenGameOptions(0x41fdbe..0x41ff26), ScreenDisplayOptions(0x42078f, 0x4207ad) |
| 0x45D34C | SNK_DifficultyTxt | 2 | ScreenGameOptions(0x41fe7c, 0x41fea7) |
| 0x45D2C4 | SNK_DynamicsTxt | 2 | ScreenGameOptions(0x41fec0, 0x41feee) |

#### 7c. Display/Graphics Options (ScreenDisplayOptions, 0x420400)

| IAT Addr | Symbol | Xrefs | Sites |
|---|---|---|---|
| 0x45D388 | SNK_ResolutionButTxt | 1 | ScreenDisplayOptions(0x42047e) |
| 0x45D33C | SNK_FoggingButTxt | 2 | ScreenDisplayOptions(0x4204b7, 0x4204d1) |
| 0x45D380 | SNK_SpeedReadoutButTxt | 1 | ScreenDisplayOptions(0x4204e0) |
| 0x45D37C | SNK_CameraDampingButTxt | 1 | ScreenDisplayOptions(0x4204fb) |
| 0x45D374 | SNK_SpeedReadTxt | 2 | ScreenDisplayOptions(0x4207cd, 0x4207f2) |
| 0x45D12C | SNK_GfxOptions_MT | 1 | ScreenDisplayOptions(0x42055d) |

#### 7d. Sound Options (ScreenSoundOptions, 0x41EB30)

| IAT Addr | Symbol | Xrefs | Sites |
|---|---|---|---|
| 0x45D344 | SNK_SfxModeButTxt | 1 | ScreenSoundOptions(0x41eb55) |
| 0x45D340 | SNK_SfxVolumeButTxt | 1 | ScreenSoundOptions(0x41eb70) |
| 0x45D2A8 | SNK_MusicVolumeButTxt | 1 | ScreenSoundOptions(0x41eb8c) |
| 0x45D338 | SNK_MusicTestButTxt | 1 | ScreenSoundOptions(0x41eba8) |
| 0x45D334 | SNK_SfxOptions_MT | 1 | ScreenSoundOptions(0x41ec0c) |
| 0x45D330 | SNK_SFX_Modes | 2 | ScreenSoundOptions(0x41f09b, 0x41f0bc) |

#### 7e. Two-Player Options (ScreenTwoPlayerOptions, 0x420C70)

| IAT Addr | Symbol | Xrefs | Sites |
|---|---|---|---|
| 0x45D398 | SNK_SplitScreenButTxt | 1 | ScreenTwoPlayerOptions(0x420cff) |
| 0x45D394 | SNK_CatchupTxt | 1 | ScreenTwoPlayerOptions(0x420d1a) |
| 0x45D390 | SNK_TwoOptions_MT | 1 | ScreenTwoPlayerOptions(0x420d76) |
| 0x45D348 | SNK_Split_Modes | 2 | ScreenTwoPlayerOptions(0x420f74, 0x420f9f) |

**Notes:** SNK_OnOffTxt is the most heavily used option value array (10 xrefs) -- it provides "ON"/"OFF" strings for every boolean toggle in both Game Options and Display Options. Each toggle reads it twice (once per possible state).

---

### 8. RACE HUD / HIGH SCORES (DrawPostRaceHighScoreEntry, 0x413010)

| IAT Addr | Symbol | Xrefs | Sites |
|---|---|---|---|
| 0x45D208 | SNK_NameTxt | 2 | DrawPostRaceHighScoreEntry(0x41303d, 0x413057) |
| 0x45D204 | SNK_BestTxt | 2 | DrawPostRaceHighScoreEntry(0x413062, 0x4130aa) |
| 0x45D200 | SNK_CarTxt | 2 | DrawPostRaceHighScoreEntry(0x4130c2, 0x4130db) |
| 0x45D1FC | SNK_AvgTxt | 2 | DrawPostRaceHighScoreEntry(0x4130f0, 0x41310c) |
| 0x45D1F8 | SNK_TopTxt | 2 | DrawPostRaceHighScoreEntry(0x413121, 0x41313d) |
| 0x45D1F4 | SNK_TimeTxt | 2 | DrawPostRaceHighScoreEntry(0x41316d, 0x413189) |
| 0x45D1F0 | SNK_LapTxt | 2 | DrawPostRaceHighScoreEntry(0x4131a6, 0x4131c2) |
| 0x45D1EC | SNK_PtsTxt | 2 | DrawPostRaceHighScoreEntry(0x4131df, 0x4131fc) |
| 0x45D1E8 | SNK_SpdTxt | 4 | DrawPostRaceHighScoreEntry(0x413210, 0x41322d, 0x41323e, 0x41325e) |

**Notes:** All 9 HUD label symbols are exclusively used by DrawPostRaceHighScoreEntry, each read twice (once for value and once for formatting). SNK_SpdTxt has 4 xrefs because it's used both as a column header and as a unit suffix in multiple contexts. Despite the names, these are NOT used in the live race HUD (which uses QueueRaceHudFormattedText with hardcoded format strings) -- they are high-score / results table column headers.

---

### 9. RACE RESULTS (RunRaceResultsScreen, 0x422640)

| IAT Addr | Symbol | Xrefs | Sites |
|---|---|---|---|
| 0x45D3B4 | SNK_ResultsTxt | 6 | RunRaceResultsScreen(0x422721, 0x42276e, 0x4227c5, 0x4227f0, 0x422807, 0x42282f) |
| 0x45D2A4 | SNK_CCResultsTxt | 1 | RunRaceResultsScreen(0x4226ab) |
| 0x45D3C0 | SNK_DRResultsTxt | 1 | RunRaceResultsScreen(0x4226e5) |
| 0x45D3A4 | SNK_SaveRaceStatus | 2 | RunRaceResultsScreen(0x422fde, 0x4230a2) |
| 0x45D3A8 | SNK_ViewRaceData | 4 | RunRaceResultsScreen(0x422fbf, 0x423083, 0x423156, 0x423174) |
| 0x45D3AC | SNK_RaceAgain | 1 | RunRaceResultsScreen(0x4230f0) |
| 0x45D3B0 | SNK_Quit | 2 | RunRaceResultsScreen(0x4230be, 0x4231ae) |
| 0x45D3B8 | SNK_NextCupRace | 2 | RunRaceResultsScreen(0x422f6a, 0x423032) |
| 0x45D3BC | SNK_ViewReplay | 6 | RunRaceResultsScreen(0x422fa6, 0x422fb4, 0x42306a, 0x423078, 0x423128, 0x423136) |
| 0x45D38C | SNK_SelectNewCar | 1 | RunRaceResultsScreen(0x423190) |
| 0x45D378 | SNK_FailedToSave | 1 | RunRaceResultsScreen(0x4235c5) |
| 0x45D384 | SNK_BlockSavedOK | 1 | RunRaceResultsScreen(0x4235bc) |

**Notes:** SNK_ResultsTxt (the multi-row results table: "CUP POSITION", "CUP POINTS", etc.) has 6 xrefs because the results screen displays different subsets of columns depending on race type. SNK_ViewReplay also has 6 xrefs -- it's conditionally shown/hidden in multiple result screen states (single race, cup race, drag race). SNK_ViewRaceData has 4 xrefs for similar state-dependent display.

---

### 10. CREDITS / EXTRAS (ScreenExtrasGallery, 0x418170)

| IAT Addr | Symbol | Xrefs | Sites |
|---|---|---|---|
| 0x45D290 | SNK_CreditsText | 8 | ScreenExtrasGallery(0x41819e, 0x4181dc, 0x418238, 0x4182c5, 0x4182de, 0x418321, 0x41833d, 0x418350) |

**Notes:** SNK_CreditsText has 8 xrefs, all within ScreenExtrasGallery. The credits are a char[][24] array (~200 entries), and the 8 references correspond to: initial text pointer setup, scrolling loop control, line rendering (x2), column formatting (x2), and end-of-credits detection (x2). This is NOT just a simple scrolling text -- the function handles multi-column credit layout.

---

### 11. MUSIC TEST (0x418580 area)

| IAT Addr | Symbol | Xrefs | Sites |
|---|---|---|---|
| 0x45D294 | SNK_MusicTest_MT | 2 | 0x418688, RunRaceResultsScreen(0x4228d7) |
| 0x45D298 | SNK_SelectTrackButTxt | 1 | 0x418639 |
| 0x45D29C | SNK_NowPlayingTxt | 4 | 0x418585, 0x4185a2, 0x418a19, 0x418a36 |

**Notes:** SNK_NowPlayingTxt ("NOW PLAYING:") has 4 xrefs split between two unnamed functions in the music test area. SNK_MusicTest_MT surprisingly also has a reference in RunRaceResultsScreen -- possibly the results screen can transition to a music test state.

---

### 12. NETWORK / MULTIPLAYER

| IAT Addr | Symbol | Xrefs | Sites |
|---|---|---|---|
| 0x45D130 | SNK_ChooseConnectionButTxt | 1 | RunFrontendConnectionBrowser(0x418ec8) |
| 0x45D134 | SNK_GameTypeTxt | 1 | RenderFrontendSessionBrowser(0x419c4d) |
| 0x45D2AC | SNK_NewSessionTxt | 1 | RenderFrontendSessionBrowser(0x419bc7) |
| 0x45D2B0 | SNK_ChooseSessionButTxt | 1 | RunFrontendSessionPicker(0x419dc0) |
| 0x45D214 | SNK_EnterPlayerNameButTxt | 3 | 0x413dba, RunFrontendCreateSessionFlow(0x41ab70, 0x41af0e) |
| 0x45D2B8 | SNK_EnterNewSessionNameButTxt | 1 | RunFrontendCreateSessionFlow(0x41a897) |
| 0x45D2B4 | SNK_SeshJoinMsg | 1 | RunFrontendCreateSessionFlow(0x41b2a9) |
| 0x45D2BC | SNK_SeshCreateMsg | 2 | CreateFrontendNetworkSession(0x41b3bc, 0x41b3e9) |
| 0x45D2CC | SNK_NetPlayStatMsg | 1 | RenderFrontendLobbyStatusPanel(0x41b4aa) |
| 0x45D2C8 | SNK_TxtSession | 1 | RenderFrontendLobbyStatusPanel(0x41b504) |
| 0x45D128 | SNK_TxtPlayer | 1 | RenderFrontendLobbyStatusPanel(0x41b527) |
| 0x45D2C0 | SNK_TxtBhostB | 1 | RenderFrontendLobbyStatusPanel(0x41b59b) |
| 0x45D2D0 | SNK_NowHostMsg | 1 | ProcessFrontendNetworkMessages(0x41ba9f) |
| 0x45D2D4 | SNK_SeshLeaveMsg | 2 | ProcessFrontendNetworkMessages(0x41b9f8, 0x41bb5d) |
| 0x45D2E8 | SNK_WaitForHostMsg | 2 | RunFrontendNetworkLobby(0x41c912, 0x41c91a) |
| 0x45D2F8 | SNK_MessageWindowButTxt | 1 | RunFrontendNetworkLobby(0x41c486) |
| 0x45D2F4 | SNK_StatusButTxt | 1 | RunFrontendNetworkLobby(0x41c4bc) |
| 0x45D2F0 | SNK_ChangeCarButTxt | 2 | RunFrontendNetworkLobby(0x41c4cc), 0x4215ca |
| 0x45D2EC | SNK_StartButTxt | 1 | RunFrontendNetworkLobby(0x41c4eb) |
| 0x45D2E4 | SNK_NetErrString1 | 2 | RunFrontendNetworkLobby(0x41cc33, 0x41cc4f) |
| 0x45D2E0 | SNK_NetErrString2 | 2 | RunFrontendNetworkLobby(0x41cc75, 0x41cc99) |
| 0x45D2DC | SNK_NetErrString3 | 2 | RunFrontendNetworkLobby(0x41ccaf, 0x41ccc5) |
| 0x45D2D8 | SNK_NetErrString4 | 2 | RunFrontendNetworkLobby(0x41ccda, 0x41ccf3) |
| 0x45D2FC | SNK_SeshLockedTxt | 2 | 0x41d6e5, 0x41d6fd |
| 0x45D300 | SNK_SorryTxt | 4 | 0x41d6b7, 0x41d6cf, 0x423877, 0x423890 |

---

### 13. CUP WIN/LOSS MESSAGES (0x423800 area)

| IAT Addr | Symbol | Xrefs | Sites |
|---|---|---|---|
| 0x45D16C | SNK_CongratsTxt | 2 | 0x423b1a, 0x423b33 |
| 0x45D170 | SNK_YouHaveWonTxt | 2 | 0x423b48, 0x423b61 |
| 0x45D174 | SNK_CarsUnlocked | 1 | 0x423bbd |
| 0x45D178 | SNK_TracksUnlocked | 1 | 0x423c0d |
| 0x45D17C | SNK_YouFailedTxt | 2 | 0x4238a5, 0x4238be |
| 0x45D180 | SNK_ToWinTxt | 2 | 0x4238d3, 0x4238ec |

**Notes:** These cup win/loss messages are in an unnamed function at ~0x423800. The "YOU FAILED / TO WIN" pair and "CONGRATULATIONS / YOU HAVE WON THE" pair are each displayed as two-line centered text overlays. SNK_CarsUnlocked and SNK_TracksUnlocked are shown after cup completion when new content is unlocked.

---

### 14. QUICK RACE MENU (0x421580 area)

| IAT Addr | Symbol | Xrefs | Sites |
|---|---|---|---|
| 0x45D39C | SNK_QuickRaceMenu_MT | 1 | 0x421689 |
| 0x45D3A0 | SNK_ChangeTrackButTxt | 1 | 0x4215e6 |

---

## Summary Statistics

### Top 10 Most Referenced Symbols
| Rank | Symbol | Xrefs | Primary Context |
|---|---|---|---|
| 1 | SNK_OkButTxt | 24 | Universal "OK" button across all screens |
| 2 | SNK_BackButTxt | 11 | Universal "BACK" button |
| 3 | SNK_ControlText | 11 | Controller action labels (10 actions) |
| 4 | SNK_OnOffTxt | 10 | Boolean toggle values in options |
| 5 | SNK_LangDLL | 9 | Language ID + DLL validation |
| 6 | SNK_Config_Hdrs | 8 | Car stat category headers |
| 7 | SNK_TrackNames | 8 | Track name lookups |
| 8 | SNK_CreditsText | 8 | Multi-column credit scroller |
| 9 | SNK_ResultsTxt | 6 | Race results column headers |
| 10 | SNK_ViewReplay | 6 | Post-race "VIEW REPLAY" button |

### Symbols With Exactly 1 Xref (73 of 167)
These symbols are used at exactly one site in the EXE:
SNK_TxtPlayer, SNK_ChooseConnectionButTxt, SNK_GameTypeTxt, SNK_TrackButTxt, SNK_TrackSel_MT1, SNK_TrackSel_Ex, SNK_BackwardsButTxt, SNK_Layout_Types, SNK_Engine_Types, SNK_ConfSpeed, SNK_ConfMph, SNK_ConfSec, SNK_ConfFt, SNK_ConfRpm, SNK_ConfUnknown, SNK_CarsUnlocked, SNK_TracksUnlocked, SNK_BeautyTxt, SNK_BeastTxt, SNK_CarSelect_MT1, SNK_ConfigButTxt, SNK_PaintButTxt, SNK_CarButTxt, SNK_ButtonTxt, SNK_PedalsTxt, SNK_MainMenu_Ex, SNK_MustPlayer2Txt, SNK_HiScoreButTxt, SNK_OptionsButTxt, SNK_NetPlayButTxt, SNK_TimeDemoButTxt, SNK_TwoPlayerButTxt, SNK_QuickRaceButTxt, SNK_RaceMenuButTxt, SNK_RaceMen2_MT, SNK_EraButTxt, SNK_ChampionshipButTxt, SNK_SelectTrackButTxt, SNK_CCResultsTxt, SNK_MusicVolumeButTxt, SNK_NewSessionTxt, SNK_ChooseSessionButTxt, SNK_SeshJoinMsg, SNK_EnterNewSessionNameButTxt, SNK_TxtBhostB, SNK_TxtSession, SNK_NetPlayStatMsg, SNK_NowHostMsg, SNK_StartButTxt, SNK_StatusButTxt, SNK_MessageWindowButTxt, SNK_TwoPlayerOptionsButTxt, SNK_GraphicsOptionsButTxt, SNK_SoundOptionsButTxt, SNK_ControlOptionsButTxt, SNK_GameOptionsButTxt, SNK_CtrlOptions_Ex, SNK_CtrlOptions_MT, SNK_Player2ButTxt, SNK_Player1ButTxt, SNK_SfxOptions_MT, SNK_MusicTestButTxt, SNK_SfxVolumeButTxt, SNK_SfxModeButTxt, SNK_GameOptions_MT, SNK_3dCollisionsButTxt, SNK_DynamicsButTxt, SNK_DifficultyButTxt, SNK_CopsButTxt, SNK_TrafficButTxt, SNK_CheckpointTimersButTxt, SNK_CircuitLapsTxt, SNK_CameraDampingButTxt, SNK_SpeedReadoutButTxt, SNK_ResolutionButTxt, SNK_SelectNewCar, SNK_TwoOptions_MT, SNK_CatchupTxt, SNK_SplitScreenButTxt, SNK_QuickRaceMenu_MT, SNK_ChangeTrackButTxt, SNK_RaceAgain, SNK_BlockSavedOK, SNK_FailedToSave, SNK_DRResultsTxt

### Unused Symbols (0 xrefs in the EXE)
**None found.** Every one of the 167 Language.dll imports has at least one READ xref in the EXE. There are no dead/unused string imports.

### Xref Distribution by Subsystem
| Subsystem | Function(s) | Total Xrefs |
|---|---|---|
| Race Results | RunRaceResultsScreen | ~38 |
| Car Selection | CarSelectionScreenStateMachine | ~30 |
| Race Type Menu | RaceTypeCategoryMenuStateMachine | ~34 |
| Main Menu | ScreenMainMenuAnd1PRaceFlow | ~20 |
| Controller Config | Various binding page fns | ~28 |
| Network/Multiplayer | 7 network functions | ~30 |
| Options Screens | 5 option screen fns | ~45 |
| Localization Init | ScreenLocalizationInit | ~10 |
| Text Rendering | 4 Draw* functions | ~6 |
| High Scores | DrawPostRaceHighScoreEntry | ~22 |
| Track Selection | TrackSelectionScreenStateMachine | ~15 |
| Credits | ScreenExtrasGallery | 8 |
| Music Test | Unnamed functions | ~7 |
| Cup Win/Loss | Unnamed function ~0x423800 | ~10 |

### Key Architectural Insights

1. **All 167 imports are live** -- zero dead imports. The English Language.dll is perfectly matched to this EXE build.

2. **SNK_OkButTxt dominates** with 24 xrefs across 15+ distinct functions. It is the universal confirmation/proceed button text, appearing on every screen from startup to results.

3. **No HUD strings during gameplay** -- the race HUD (QueueRaceHudFormattedText at 0x428320) does NOT reference any SNK_ symbols. Instead, it uses hardcoded format strings ("%d", "%02d:%02d", etc.) for speed, lap, and time displays. The SNK_ HUD labels (SpdTxt, LapTxt, etc.) are only used in the post-race high-score table.

4. **Network subsystem is the largest consumer** by function count (7 distinct functions reference SNK_ symbols), reflecting the complexity of the DirectPlay lobby UI.

5. **Text rendering goes through SNK_LangDLL validation** -- all four text-drawing functions (DrawFrontendLocalizedStringPrimary/Secondary/ToSurface, MeasureOrCenterFrontendLocalizedString) reference SNK_LangDLL, likely to verify the DLL is loaded before accessing its strings.

6. **The localization system is initialization-only** -- SNK_Layout_Types, Engine_Types, and all ConfXxx unit suffixes are referenced exclusively in ScreenLocalizationInit. Car stats are resolved to display strings once at startup and cached.

7. **Credits use multi-column layout** -- 8 xrefs to SNK_CreditsText in ScreenExtrasGallery indicates sophisticated credit rendering, not simple line-by-line scrolling.

8. **Boolean options share one toggle array** -- SNK_OnOffTxt (10 xrefs) is the single source of "ON"/"OFF" strings for checkpoints, traffic, cops, collisions, fogging, and camera damping toggles.
