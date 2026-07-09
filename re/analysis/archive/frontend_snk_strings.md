# TD5 Language.dll — SNK_* frontend label string extraction

- DLL: `C:/Users/maria/Desktop/Proyectos/TD5RE/original/Language/English/Language.dll`
- ImageBase: 0x10000000  SizeOfImage: 0xD000
- Total exports: 171
- SNK_* exports: 171  resolved: 171  unresolved: 0
- Method: PE export-directory parse; per export read word at RVA; VA-pointer heuristic (>= ImageBase and in image) -> follow as ptr/array, else inline C string. RESEARCH ONLY.

## All SNK_* exports

| SNK_symbol | RVA | type | value(s) |
|---|---|---|---|
| SNK_3dCollisionsButTxt | 0x00746C | string | "3D COLLISIONS" |
| SNK_AutoButTxt | 0x0072B0 | string | "AUTOMATIC" |
| SNK_AvgTxt | 0x0079B4 | string | "AVERAGE" |
| SNK_BackButTxt | 0x0072C8 | string | "BACK" |
| SNK_BackwardsButTxt | 0x0074EC | string | "BACKWARDS" |
| SNK_BeastTxt | 0x007A30 | string | "BEAST" |
| SNK_BeautyTxt | 0x007A28 | string | "BEAUTY" |
| SNK_BestTxt | 0x007994 | string | "BEST" |
| SNK_BlockSavedOK | 0x0079C8 | string | "BLOCK SAVED OK" |
| SNK_ButtonTxt | 0x0076BC | string | "BUTTON" |
| SNK_CCResultsTxt | 0x007840 | array[4] (char[4][32]) | [0]="ARRESTS"; [1]="AVERAGE SPEED"; [2]="TOP SPEED"; [3]="POINTS" |
| SNK_CameraDampingButTxt | 0x007488 | string | "CAMERA DAMPING" |
| SNK_CarButTxt | 0x00729C | string | "CAR" |
| SNK_CarLongNames | 0x006B9C | matrix[185 ptrs / 5 slots] | [0.0]="1999 ASTON MARTIN PROJECT VANTAGE"; [1.0]="1970 CHEVROLET CHEVELLE SS 454 LS6"; [2.0]="1997 CHEVROLET CAMARO Z28 SS LT4"; [3.0]="1998 DODGE VIPER GTS-R"; [4.0]="1994 JAGUAR XJ220"; [5.0]="1968 FORD MUSTANG 428CJ"; [6.0]="1998 POLICE TVR CERBERA"; [7.0]="1998 DODGE VIPER"; [8.0]="1998 TVR SPEED 12"; [9.0]="1969 CHEVROLET CORVETTE ZL1"; [10.0]="???"; [11.0]="1969 CHEVROLET CAMARO ZL1"; [12.0]="CATERHAM SUPER 7"; [13.0]="1966 SHELBY COBRA 427SC"; [14.0]="1969 DODGE CHARGER"; [15.0]="1971 PLYMOUTH HEMI CUDA"; [16.0]="???"; [17.0]="1998 SALEEN MUSTANG S351"; [18.0]="1967 PONTIAC GTO"; [19.0]="???"; [20.0]="HOT DOG"; [21.0]="CHRIS'S BEAST !"; [22.0]="1998 NISSAN SKYLINE"; [23.0]="BEHOLD! THE MIGHTY MAUL!"; [24.0]="FEAR FACTORY WAGON - KILLER MAN!"; [25.0]="1998 FORD MUSTANG GT"; [26.0]="NISSAN R390 GT-1"; [27.0]="1998 POLICE FORD MUSTANG"; [28.0]="1969 POLICE DODGE CHARGER"; [29.0]="1997 POLICE CHEVROLET CAMARO"; [30.0]="PITBULL SPECIAL"; [31.0]="1998 TVR CERBERA"; [32.0]="1998 ASTON MARTIN V8 VANTAGE"; [33.0]="1998 CHEVROLET CORVETTE"; [34.0]="1998 JAGUAR XKR" |
| SNK_CarSelect_Ex | 0x00616C | array[2] (char[2][32]) | [0]="CLICK TO GO BACK TO MAIN MENU"; [1]="CLICK TO GO BACK TO PLAYER ONE" |
| SNK_CarSelect_MT1 | 0x006078 | string | " " |
| SNK_CarTxt | 0x0079B0 | string | "CAR" |
| SNK_CarsUnlocked | 0x007A48 | string | "CARS UNLOCKED" |
| SNK_CatchupTxt | 0x0074B0 | string | "CATCHUP" |
| SNK_ChallengeButTxt | 0x007588 | string | "CHALLENGE" |
| SNK_ChampionshipButTxt | 0x007594 | string | "CHAMPIONSHIP" |
| SNK_ChangeCarButTxt | 0x007340 | string | "CHANGE CAR" |
| SNK_ChangeTrackButTxt | 0x0074C8 | string | "CHANGE TRACK" |
| SNK_CheckpointTimersButTxt | 0x007430 | string | "CHECKPOINT TIMERS" |
| SNK_ChooseConnectionButTxt | 0x0072D8 | string | "CHOOSE CONNECTION" |
| SNK_ChooseSessionButTxt | 0x0072EC | string | "CHOOSE SESSION" |
| SNK_CircuitLapsTxt | 0x0074B8 | string | "CIRCUIT LAPS" |
| SNK_ConfFt | 0x007ABC | string | " FT" |
| SNK_ConfMph | 0x007AAC | string | " MPH" |
| SNK_ConfRpm | 0x007AC0 | string | " RPM" |
| SNK_ConfSec | 0x007AB4 | string | " SEC" |
| SNK_ConfSpeed | 0x007AA4 | string | " SPEED" |
| SNK_ConfUnknown | 0x007AC8 | string | "UNKNOWN" |
| SNK_ConfigButTxt | 0x0072A8 | string | "STATS" |
| SNK_Config_Hdrs | 0x006E80 | array[20] | [0]="LAYOUT:"; [1]="GEARS:"; [2]="PRICE:"; [3]="TIRES:"; [4]="TOP SPEED:"; [5]="0 to 60 MPH:"; [6]="60 to 0 MPH:"; [7]="1/4 MILE:"; [8]="ENGINE:"; [9]="COMPRESSION:"; [10]="DISPLACEMENT:"; [11]="LATERAL ACC:"; [12]="TORQUE:"; [13]="HP:"; [14]="ACCELERATION"; [15]="TORQUE"; [16]="HORSEPOWER"; [17]="TOP SPEED"; [18]="GRIP"; [19]="BHP" |
| SNK_ConfigurationTxt | 0x0076E4 | string | "CONFIGURATION FOR THAT BUTTON" |
| SNK_ConfigureButTxt | 0x0073E0 | string | "CONFIGURE" |
| SNK_CongratsTxt | 0x007A00 | string | "CONGRATULATIONS!" |
| SNK_ContCupButTxt | 0x007554 | string | "CONTINUE CUP" |
| SNK_ControlOptionsButTxt | 0x007380 | string | "CONTROL OPTIONS" |
| SNK_ControlText | 0x0075E0 | array[12] (char[12][16]) | [0]="LEFT"; [1]="RIGHT"; [2]="ACCELERATE"; [3]="BRAKE"; [4]="HANDBRAKE"; [5]="HORN/SIREN"; [6]="GEAR UP"; [7]="GEAR DOWN"; [8]="CHANGE VIEW"; [9]="REAR VIEW"; [10]="NONE"; [11]="ACCEL/BRAKE" |
| SNK_ControllerTxt | 0x0075D4 | string | "CONTROLLER" |
| SNK_CopChaseButTxt | 0x00757C | string | "COP CHASE" |
| SNK_CopsButTxt | 0x00744C | string | "POLICE" |
| SNK_CreditsText | 0x007AD0 | array[10] (char[10][24]) | [0]="TEST DRIVE 5"; [1]=" "; [2]="(C) ACCOLADE"; [3]="1998"; [4]=" "; [5]=" "; [6]=" "; [7]="DEVELOPED BY"; [8]="THE PITBULL SYNDICATE"; [9]=" " |
| SNK_CtrlOptions_Ex | 0x006388 | string | "CLICK TO GO BACK TO MAIN MENU" |
| SNK_CtrlOptions_MT | 0x006278 | string | "USE LEFT/RIGHT TO CHANGE" |
| SNK_Ctrl_Modes | 0x0071F0 | array[5] | [0]="KEYBOARD"; [1]="JOYSTICK"; [2]="JOYPAD"; [3]="WHEEL"; [4]="<NONE>" |
| SNK_CupRaceButTxt | 0x007548 | string | "CUP RACE" |
| SNK_DRResultsTxt | 0x0078C0 | array[3] (char[3][32]) | [0]="TOTAL TIME"; [1]="TOP SPEED"; [2]="FINISH POSITION" |
| SNK_DifficultyButTxt | 0x007454 | string | "DIFFICULTY" |
| SNK_DifficultyTxt | 0x007214 | array[3] | [0]="EASY"; [1]="NORMAL"; [2]="HARD" |
| SNK_DownTxt | 0x0076AC | string | "DOWN" |
| SNK_DragRaceButTxt | 0x007570 | string | "DRAG RACE" |
| SNK_DynamicsButTxt | 0x007460 | string | "DYNAMICS" |
| SNK_DynamicsTxt | 0x007220 | array[2] | [0]="ARCADE"; [1]="SIMULATION" |
| SNK_Engine_Types | 0x006EE8 | array[19] | [0]="V10 ALUMINIUM"; [1]="V8 ALUMINIUM BLOCK"; [2]="V8 SUPERCHARGED"; [3]="V8 ALUMINIUM"; [4]="DOHC TWIN TURBO"; [5]="V8"; [6]="FORD IRON BLOCK"; [7]="PONTIAC IRON BLOCK"; [8]="V8 IRON BLOCK"; [9]="V8 IRON BLOCK HEMI"; [10]="V12"; [11]="ALUMINIUM BLOCK"; [12]="4 CYLINDER"; [13]="V6 SUPERCHARGED"; [14]="V10 SUPERCHARGED"; [15]="V8 DOHC"; [16]="V8 TWIN TURBO"; [17]="V12 IRON BLOCK"; [18]="UNKNOWN" |
| SNK_EnterNewSessionNameButTxt | 0x0072FC | string | "ENTER NEW SESSION NAME" |
| SNK_EnterPlayerNameButTxt | 0x007314 | string | "ENTER PLAYER NAME" |
| SNK_EraButTxt | 0x0075B4 | string | "ERA" |
| SNK_ExitButTxt | 0x007360 | string | "EXIT" |
| SNK_FailedToSave | 0x0079D8 | string | "SAVE FAILED" |
| SNK_FoggingButTxt | 0x007498 | string | "FOGGING" |
| SNK_ForwardsButTxt | 0x0074E0 | string | "FORWARDS" |
| SNK_GameOptionsButTxt | 0x007370 | string | "GAME OPTIONS" |
| SNK_GameOptions_MT | 0x006470 | string | "USE LEFT/RIGHT TO CHANGE" |
| SNK_GameTypeTxt | 0x007038 | array[9] (char[9][16]) | [0]="SINGLE"; [1]="CHALNG"; [2]="CHAMP"; [3]="PITBULL"; [4]="MASTERS"; [5]="ERA"; [6]="ULTIMATE"; [7]="DRAGRACE"; [8]="COPCHASE" |
| SNK_GfxOptions_MT | 0x0065A8 | string | "USE LEFT/RIGHT TO CHANGE" |
| SNK_GraphicsOptionsButTxt | 0x0073A0 | string | "GRAPHICS OPTIONS" |
| SNK_HiScoreButTxt | 0x007354 | string | "HIGH SCORES" |
| SNK_Info_Values | 0x006F34 | array[10] | [0]="MODIFICATIONS:"; [1]=" "; [2]="THIS MODEL HAS SHORTER GEARING AND IMPROVED"; [3]="HEADERS FOR MORE LOW END, GIVING IT BETTER"; [4]="ACCELERATION. HOWEVER IT HAS A LOWER TOP SPEED."; [5]=" "; [6]=" "; [7]="IT IS WELL SUITED FOR THE KYOTO AND MOSCOW"; [8]="TRACKS."; [9]=" " |
| SNK_LangDLL | 0x006030 | string | "LANGDLL 0 : ENGLISH/US" |
| SNK_LapTxt | 0x0079A4 | string | "LAP" |
| SNK_Layout_Types | 0x006ED0 | array[6] | [0]="FRONT/REAR"; [1]="FRONT/4-WHEEL"; [2]="FRONT/FRONT"; [3]="MID/4-WHEEL"; [4]="MID/REAR"; [5]="UNKNOWN" |
| SNK_LockedTxt | 0x007984 | string | "LOCKED" |
| SNK_MainMenu_Ex | 0x0069B0 | string | "CLICK TO GO TO OPTIONS SCREEN" |
| SNK_MainMenu_MT | 0x0068FC | string | " " |
| SNK_ManualButTxt | 0x0072BC | string | "MANUAL" |
| SNK_MastersButTxt | 0x0075AC | string | "MASTERS" |
| SNK_MenuStrings | 0x007A68 | array[15] | [0]="MAIN MENU"; [1]="RACE MENU"; [2]="INFO"; [3]="QUICK RACE"; [4]="NO CONTROLLER"; [5]="NET PLAY"; [6]="OPTIONS"; [7]="HIGH SCORES"; [8]="ARE YOU SURE?"; [9]="SELECT CAR"; [10]="SELECT TRACK"; [11]="SELECT CAR PLAYER 1"; [12]="SELECT CAR PLAYER 2"; [13]="SELECT COMPUTER CAR"; [14]="RACE RESULTS" |
| SNK_MessageWindowButTxt | 0x007328 | string | "MESSAGE WINDOW" |
| SNK_MsgBufferFull | 0x006FF8 | string | " <Message Buffer Full>" |
| SNK_MusicTestButTxt | 0x007414 | string | "MUSIC TEST" |
| SNK_MusicTest_MT | 0x006898 | string | "USE LEFT/RIGHT TO SELECT TRACK" |
| SNK_MusicVolumeButTxt | 0x007404 | string | "MUSIC VOLUME" |
| SNK_MustPlayer1Txt | 0x006F7C | string | "FOR PLAYER 1" |
| SNK_MustPlayer2Txt | 0x006F8C | string | "FOR PLAYER 2" |
| SNK_MustSelectTxt | 0x006F5C | string | "YOU MUST SELECT A CONTROLLER" |
| SNK_NameTxt | 0x00798C | string | "NAME" |
| SNK_NetErrString1 | 0x0070F4 | array[2] (char[2][40]) | [0]="NOT ALL PLAYERS ARE WAITING"; [1]="THERE ARE NO PLAYERS WAITING" |
| SNK_NetErrString2 | 0x007144 | array[2] (char[2][32]) | [0]="TO START. ARE YOU SURE ?"; [1]="TO START." |
| SNK_NetErrString3 | 0x007184 | string | "ARE YOU SURE YOU WANT TO EXIT" |
| SNK_NetErrString4 | 0x0071A4 | string | "THIS SESSION ?" |
| SNK_NetPlayButTxt | 0x007528 | string | "NET PLAY" |
| SNK_NetPlayStatMsg | 0x0071B4 | array[4] (char[4][10]) | [0]="Chat"; [1]="Busy"; [2]="Wait"; [3]="Play" |
| SNK_NewSessionTxt | 0x0070C8 | string | "< New Session >" |
| SNK_NextCupRace | 0x007974 | string | "NEXT CUP RACE" |
| SNK_NowHostMsg | 0x006FD0 | string | " is now the HOST." |
| SNK_NowPlayingTxt | 0x0075C4 | string | "NOW PLAYING:" |
| SNK_NoxButTxt | 0x00736C | string | "NO!" |
| SNK_OkButTxt | 0x0072C4 | string | "OK" |
| SNK_OnOffTxt | 0x00720C | array[2] | [0]="OFF"; [1]="ON" |
| SNK_OptionsButTxt | 0x007534 | string | "OPTIONS" |
| SNK_Options_MT | 0x0061AC | string | " " |
| SNK_PaintButTxt | 0x0072A0 | string | "PAINT" |
| SNK_PedalsTxt | 0x0076B4 | string | "PEDALS" |
| SNK_PitbullButTxt | 0x0075A4 | string | "PITBULL" |
| SNK_Player1ButTxt | 0x0073C8 | string | "PLAYER 1" |
| SNK_Player2ButTxt | 0x0073D4 | string | "PLAYER 2" |
| SNK_PressKeyTxt | 0x007704 | string | "PRESS THE KEY TO USE FOR" |
| SNK_PressingTxt | 0x0076C4 | string | "PRESS A BUTTON TO CHANGE THE" |
| SNK_PtsTxt | 0x0079A8 | string | "POINTS" |
| SNK_QRTxtCar | 0x007228 | string | "CAR" |
| SNK_QRTxtTrack | 0x00722C | string | "TRACK" |
| SNK_QuickRaceButTxt | 0x007504 | string | "QUICK RACE" |
| SNK_QuickRaceMenu_MT | 0x006744 | string | "USE LEFT/RIGHT TO CHANGE" |
| SNK_Quit | 0x007958 | string | "QUIT" |
| SNK_RaceAgain | 0x007920 | string | "RACE AGAIN" |
| SNK_RaceMen2_MT | 0x006A74 | string | " " |
| SNK_RaceMenuButTxt | 0x0074F8 | string | "RACE MENU" |
| SNK_RaceMenu_MT | 0x0069D0 | string | " " |
| SNK_RaceTypeText | 0x006048 | array[12] | [0]="SINGLE RACE"; [1]="CHAMPIONSHIP CUP"; [2]="ERA CUP"; [3]="CHALLENGE CUP"; [4]="PITBULL CUP"; [5]="MASTERS CUP"; [6]="ULTIMATE CUP"; [7]="DRAG RACING"; [8]="COP CHASE"; [9]="TIME TRIALS"; [10]="CUP RACE"; [11]="CONTINUE CUP" |
| SNK_ResolutionButTxt | 0x00747C | string | "RESOLUTION" |
| SNK_ResultsTxt | 0x007720 | array[9] (char[9][32]) | [0]="CUP POSITION"; [1]="CUP POINTS"; [2]="AVERAGE SPEED"; [3]="TOP SPEED"; [4]="FINISH POSITION"; [5]="HIGHEST POSITION"; [6]="TOTAL TIME"; [7]="CHECKPOINT TIMERS"; [8]="CUP TIME" |
| SNK_SFX_Modes | 0x0071E4 | array[3] | [0]="MONAURAL"; [1]="STEREO"; [2]="3D SOUND" |
| SNK_SaveRaceStatus | 0x007960 | string | "SAVE RACE STATUS" |
| SNK_SelectNewCar | 0x007948 | string | "SELECT NEW CAR" |
| SNK_SelectTrackButTxt | 0x0072D0 | string | "TRACK" |
| SNK_SeshCreateMsg | 0x006FE4 | string | " Session Created." |
| SNK_SeshJoinMsg | 0x006F9C | string | " has joined the session." |
| SNK_SeshLeaveMsg | 0x006FB8 | string | " has left the session." |
| SNK_SeshLockedTxt | 0x007A38 | string | "SESSION LOCKED" |
| SNK_SfxModeButTxt | 0x0073EC | string | "SFX MODE" |
| SNK_SfxOptions_MT | 0x0063A8 | string | "USE LEFT/RIGHT TO CHANGE" |
| SNK_SfxVolumeButTxt | 0x0073F8 | string | "SFX VOLUME" |
| SNK_SingleRaceButTxt | 0x00753C | string | "SINGLE RACE" |
| SNK_SorryTxt | 0x0079E4 | string | "SORRY" |
| SNK_SoundOptionsButTxt | 0x007390 | string | "SOUND OPTIONS" |
| SNK_SpdTxt | 0x0079C0 | string | "SPEED" |
| SNK_SpeedReadTxt | 0x007204 | array[2] | [0]="MPH"; [1]="KPH" |
| SNK_SpeedReadoutButTxt | 0x007420 | string | "SPEED READOUT" |
| SNK_SplitScreenButTxt | 0x0074A0 | string | "SPLIT SCREEN" |
| SNK_Split_Modes | 0x0071DC | array[2] | [0]="LEFT/RIGHT"; [1]="UP/DOWN" |
| SNK_StartButTxt | 0x00734C | string | "START" |
| SNK_StatusButTxt | 0x007338 | string | "STATUS" |
| SNK_TimeDemoButTxt | 0x00751C | string | "TIME DEMO" |
| SNK_TimeTrialsButTxt | 0x007564 | string | "TIME TRIALS" |
| SNK_TimeTxt | 0x00799C | string | "TIME" |
| SNK_ToWinTxt | 0x0079F8 | string | "TO WIN" |
| SNK_TopTxt | 0x0079BC | string | "TOP" |
| SNK_TrackButTxt | 0x0074D8 | string | "TRACK" |
| SNK_TrackNames | 0x007234 | array[26] | [0]="KESWICK, ENGLAND"; [1]="SAN FRANCISCO, CA, USA"; [2]="BERN, SWITZERLAND"; [3]="KYOTO, JAPAN"; [4]="WASHINGTON, DC, USA"; [5]="MUNICH, GERMANY"; [6]="HONOLULU, HAWAII, USA"; [7]="SYDNEY, AUSTRALIA"; [8]="TOKYO, JAPAN"; [9]="EDINBURGH, SCOTLAND"; [10]="BLUE RIDGE PARKWAY, NC, USA"; [11]="MOSCOW, RUSSIA"; [12]="CHEDDAR CHEESE, ENGLAND"; [13]="JARASH, JORDAN"; [14]="COURMAYEUR, ITALY"; [15]="MAUI, HAWAII, USA"; [16]="NEWCASTLE, ENGLAND"; [17]="HOUSE OF BEZ, ENGLAND"; [18]="MONTEGO BAY, JAMAICA"; [19]="DRAG STRIP"; [20]="CHAMPIONSHIP CUP"; [21]="ERA CUP"; [22]="CHALLENGE CUP"; [23]="PITBULL CUP"; [24]="MASTERS CUP"; [25]="ULTIMATE CUP" |
| SNK_TrackSel_Ex | 0x006858 | array[2] (char[2][32]) | [0]="CLICK TO PLAY TRACK BACKWARDS"; [1]="CLICK TO PLAY TRACK FORWARDS" |
| SNK_TrackSel_MT1 | 0x0067F4 | string | " " |
| SNK_TracksUnlocked | 0x007A58 | string | "TRACKS UNLOCKED" |
| SNK_TrafficButTxt | 0x007444 | string | "TRAFFIC" |
| SNK_TwoOptions_MT | 0x0066E0 | string | "USE LEFT/RIGHT TO CHANGE SPLIT" |
| SNK_TwoPlayerButTxt | 0x007510 | string | "TWO PLAYER" |
| SNK_TwoPlayerOptionsButTxt | 0x0073B4 | string | "TWO PLAYER OPTIONS" |
| SNK_TxtBhostB | 0x0070EC | string | "(HOST)" |
| SNK_TxtPlayer | 0x0070E4 | string | "Player:" |
| SNK_TxtSession | 0x0070D8 | string | "Session:" |
| SNK_UltimateButTxt | 0x0075B8 | string | "ULTIMATE" |
| SNK_UpDownTxt | 0x0076A0 | string | "UP/DOWN" |
| SNK_UpTxt | 0x0076A8 | string | "UP" |
| SNK_ViewRaceData | 0x007938 | string | "VIEW RACE DATA" |
| SNK_ViewReplay | 0x00792C | string | "VIEW REPLAY" |
| SNK_WaitForHostMsg | 0x007010 | string | " WAITING FOR HOST TO START THE GAME." |
| SNK_YesButTxt | 0x007368 | string | "YES" |
| SNK_YouFailedTxt | 0x0079EC | string | "YOU FAILED" |
| SNK_YouHaveWonTxt | 0x007A14 | string | "YOU HAVE WON THE" |

## Non-SNK_* exports (resolved, for reference)

| symbol | RVA | type | value(s) |
|---|---|---|---|

---

## Shared menu text path (port) — how these strings reach the screen [S02 (a), 2026-06-04]

All of the SNK_* labels above (and port-added literals) are drawn through a
**single shared text helper**, not per-screen baked art:

- `fe_draw_text(x, y, text, color, sx, sy)` and `fe_draw_text_centered(...)` in
  `td5_frontend.c` are the one menu text path. Every screen — main menu, options,
  Quick Race, Track/Car select, High Scores, and the newer port screens
  (MULTIPLAYER lobby, controller binding) — calls them. Adding a new screen needs
  **no new font art**; it just calls `fe_draw_text`.
- One font backs all of it: **BodyText** (`re/assets/frontend/BodyText.*`).
  - VectorUI on (default, `[Frontend] VectorUI=1`): MSDF atlas `BodyText_msdf.png`
    via `s_ps_msdf` (crisp at any resolution).
  - VectorUI off: the `BodyText.tga` bitmap atlas.
  - The baked button cache (`td5_frontend_button_cache.c`) composites from the
    **same** BodyText atlas + the **same** `s_font_glyph_advance` metrics, so a
    cached button caption and a live `fe_draw_text` value share one set of
    letterforms.
- `fe_draw_small_text` (smalltext atlas) is the only deliberate **second** font,
  for the dense High-Scores / Race-Results tables (true lowercase + descenders).
- Screen TITLE logos (e.g. `QuickRaceText.tga`) are legacy baked images; new
  screens render their title with `fe_draw_text` instead (see
  `SNK_MultiplayerTitleTxt`).

Gotcha (the source of the missing Quick Race captions, S02 (b)): the VectorUI
procedural-button path only auto-draws a caption for **non-selector** buttons, so
a selector row that wants a visible caption must either not set `is_selector`
(Quick Race / Track Selection option rows) or draw its label in a per-screen
overlay (controller binding). See the doc block above `fe_measure_text_width` in
`td5_frontend.c`.
