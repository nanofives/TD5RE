# Language.dll Analysis -- UI String Localization DLL

**Date:** 2026-03-19
**Binary:** Language.dll (English/US variant, 49152 bytes)
**Ghidra port:** 8195

## Summary

Language.dll is a **data-only DLL** exporting ~170 named global variables containing all user-visible UI strings for Test Drive 5. It has no meaningful code beyond the MSVC CRT DllMain boilerplate. All strings are hardcoded in the `.data` section as plain ASCII. The EXE imports these symbols via the standard PE import mechanism (no LoadLibrary/GetProcAddress at runtime).

## DLL Architecture

### Code (53 functions, all CRT boilerplate)
- `entry` (0x100010b8): Standard MSVC DllMain CRT entry point
- `FUN_10001000`: CRT init/shutdown (GetVersion, GetCommandLineA, heap init)
- `FUN_1000193f`: Calls `DisableThreadLibraryCalls` on DLL_PROCESS_ATTACH
- Remaining functions: `_malloc`, `_strlen`, `_strncpy`, `_memset`, `__exit`, SEH unwinding, etc.
- **No localization logic, no string lookup functions, no locale selection code**

### Exports (514 total, ~170 unique symbols with C++ decorated + plain + ordinal triples)
Every export is a `char*`, `char**`, or `char[][]` global variable. Naming convention: `SNK_<Name>` with `@@3PADA` (char*), `@@3PAPADA` (char**), `@@3PAY0CA@DA` (char[][32]), etc.

## Import Mechanism in TD5_d3d.exe

The EXE declares all SNK_ symbols as **PE imports from Language.dll**. The Windows loader resolves them at load time. In the EXE code, they appear as `SNK_<Name>_exref` -- IAT pointers that dereference directly to Language.dll's `.data` section.

**Key implication:** Swapping Language.dll requires the replacement DLL to export *all* symbols the EXE imports, or the EXE will fail to load (import resolution error). The German Language.dll has only 123 exports (vs. 170 in English) and is missing SNK_LangDLL, so it was likely built for a different EXE revision.

## Language Index System

### SNK_LangDLL Identifier
```
SNK_LangDLL = "LANGDLL 0 : ENGLISH/US"
```
- Character at index 8 (`SNK_LangDLL[8]`) = `'0'` (ASCII 0x30 = 48 decimal)
- This byte is used as a **language index** in `ScreenLocalizationInit` (0x004269d0)

### Config File Selection
The language index selects which car configuration file to read from car ZIPs:

| Index (char) | Stack slot | Config file    | Language  |
|---|---|---|---|
| (slot 0)     | ESP+0x0c   | `config.nfo`   | Info/fallback |
| (slot 1)     | ESP+0x10   | `config.eng`   | English   |
| (slot 2)     | ESP+0x14   | `config.fre`   | French    |
| (slot 3)     | ESP+0x18   | `config.ger`   | German    |
| (slot 4)     | ESP+0x1c   | `config.ita`   | Italian   |
| (slot 5)     | ESP+0x20   | `config.spa`   | Spanish   |

**Note:** The assembly at 0x426a83 uses `MOVSX ECX, byte ptr [EAX+8]` then indexes `[ESP + ECX*4 + offset]`. With `'0'` = 48, the effective index resolves to `config.eng` (English).

### ScreenLocalizationInit (0x004269d0)

This is `g_frontendScreenFnTable[0]` -- the **very first frontend screen** executed at startup. It:

1. Iterates over all 10 car ZIPs via `gCarZipPathTable`
2. For each car, reads the language-specific `config.<lang>` file from the ZIP using `GetArchiveEntrySize` / `ReadArchiveEntry`
3. Parses 17 fields via sscanf (`%s\n%s\n...` x17): car name, stats, engine type, layout, etc.
4. Replaces coded byte indices with localized strings from `SNK_Layout_Types`, `SNK_Engine_Types`, `SNK_ConfSpeed/Mph/Sec/Ft/Rpm`
5. Replaces underscores with spaces in display strings
6. Falls back to `SNK_ConfUnknown` ("UNKNOWN") if data is missing or file not found
7. Loads saved config via `LoadPackedConfigTd5()`
8. Detects joystick hardware and initializes controller state
9. Sets up display mode enumeration (searches for 640x480x16 default)
10. Transitions to frontend screen 5 (Main Menu)

## Language Files on Disc

```
Language.dll                     -- 49152 bytes (English/US, active)
Language/English/Language.dll     -- 49152 bytes (identical to root)
Language/German/Language.dll      -- 41472 bytes (fewer exports, different EXE build)
```

The installer likely copies the appropriate Language.dll to the game root based on the user's language selection. Only English and German variants exist on this ISO.

## Export Catalog

### Self-Identification
| Export | Value |
|---|---|
| `SNK_LangDLL` | `"LANGDLL 0 : ENGLISH/US"` |

### Main Menu & Navigation
| Export | Value |
|---|---|
| `SNK_MainMenu_MT` | Menu text table (multi-line: "CLICK TO GO TO QUICK RACE MENU", etc.) |
| `SNK_MainMenu_Ex` | "CLICK TO GO TO OPTIONS SCREEN" |
| `SNK_RaceMenuButTxt` | "RACE MENU" |
| `SNK_QuickRaceButTxt` | "QUICK RACE" |
| `SNK_TwoPlayerButTxt` | "TWO PLAYER" |
| `SNK_TimeDemoButTxt` | "TIME DEMO" |
| `SNK_NetPlayButTxt` | "NET PLAY" |
| `SNK_OptionsButTxt` | "OPTIONS" |
| `SNK_HiScoreButTxt` | "HIGH SCORES" |
| `SNK_ExitButTxt` | "EXIT" |
| `SNK_YesButTxt` | "YES" |
| `SNK_NoxButTxt` | "NO!" |
| `SNK_OkButTxt` | "OK" |
| `SNK_BackButTxt` | "BACK" |
| `SNK_StartButTxt` | "START" |

### Race Types & Cups
| Export | Value |
|---|---|
| `SNK_SingleRaceButTxt` | "SINGLE RACE" |
| `SNK_CupRaceButTxt` | "CUP RACE" |
| `SNK_ContCupButTxt` | "CONTINUE CUP" |
| `SNK_TimeTrialsButTxt` | "TIME TRIALS" |
| `SNK_DragRaceButTxt` | "DRAG RACE" |
| `SNK_CopChaseButTxt` | "COP CHASE" |
| `SNK_ChampionshipButTxt` | "CHAMPIONSHIP" |
| `SNK_EraButTxt` | "ERA" |
| `SNK_ChallengeButTxt` | "CHALLENGE" |
| `SNK_PitbullButTxt` | "PITBULL" |
| `SNK_MastersButTxt` | "MASTERS" |
| `SNK_UltimateButTxt` | "ULTIMATE" |
| `SNK_RaceTypeText` | char*[] array: "SINGLE", "CHALNG", "CHAMP", "PITBULL", "MASTERS", "ULTIMATE", "DRAGRACE", "COPCHASE" |

### Race Type Descriptions (SNK_RaceTypeText array -- multi-line descriptions)
- **SINGLE RACE**: "DEFEAT A COURSE OR CIRCUIT AT NORMAL DIFFICULTY BY COMING IN FIRST TO UNLOCK A REVERSE TRACK OR SECRET CAR"
- **CUP RACE**: "GO UP AGAINST THE WORLDS BEST RACERS IN A NON-SANCTIONED TOURNAMENT."
- **TIME TRIALS**: "TAKE ON THE CLOCK WITH TRAFFIC TO SEE HOW YOU FARE IN A RACE AGAINST YOUR OWN SKILL."
- **COP CHASE**: "CHOOSE ONE OF THE COP CARS AND PULL OVER THOSE LAWLESS HOOLIGANS WHO ARE SPEEDING THROUGH YOUR TOWN."
- **DRAG RACING**: "CHOOSE YOUR CAR..... THEN THROW DOWN THE HAMMER AND WATCH THE SMOKE FLY!"
- **CHAMPIONSHIP**: "TOTAL POINTS OVER 4 COURSES: POINTS BASED ON YOUR FINISHING POSITION IN EACH RACE."
- **ERA CUP**: "TOTAL TIME OVER ALL 6 CIRCUITS. YOUR OPPONENTS AND YOUR CAR WILL BE FROM EITHER THE BEAUTY (NEW CARS) OR BEAST (OLD CARS) DIVISIONS."
- **CHALLENGE CUP**: "TOTAL TIME OVER 6 COURSES."
- **PITBULL CUP**: "YOU MUST PLACE FIRST ON EACH OF 8 COURSES ON NORMAL DIFFICULTY TO MOVE ON TO THE NEXT CUP RACE."
- **MASTERS CUP**: "TOTAL TIME OVER 10 COURSES. 10 RANDOMLY CHOSEN CARS WILL BE AT YOUR DISPOSAL. AFTER YOU USE A VEHICLE IN YOUR MOTOR POOL, YOU MAY NOT USE IT AGAIN FOR THE DURATION OF THE CUP."
- **ULTIMATE CUP**: "TOTAL POINTS OVER 12 COURSES. POINTS ARE TABULATED FOR AVERAGE SPEED OVER THE LENGTH OF THE COURSE."
- **CONTINUE CUP**: "YOU MAY SAVE A CUP RACE AND CONTINUE IT LATER."

### Options Screens
| Export | Purpose |
|---|---|
| `SNK_Options_MT` | Options hub menu text |
| `SNK_GameOptions_MT` | Game options screen help text |
| `SNK_CtrlOptions_MT` | Control options help text |
| `SNK_SfxOptions_MT` | Sound options help text |
| `SNK_GfxOptions_MT` | Graphics options help text |
| `SNK_TwoOptions_MT` | Two-player options help text |
| `SNK_GameOptionsButTxt` | "GAME OPTIONS" |
| `SNK_ControlOptionsButTxt` | "CONTROL OPTIONS" |
| `SNK_SoundOptionsButTxt` | "SOUND OPTIONS" |
| `SNK_GraphicsOptionsButTxt` | "GRAPHICS OPTIONS" |
| `SNK_TwoPlayerOptionsButTxt` | "TWO PLAYER OPTIONS" |

### Game Options Row Labels
| Export | Value |
|---|---|
| `SNK_CircuitLapsTxt` | "CIRCUIT LAPS" |
| `SNK_CheckpointTimersButTxt` | "CHECKPOINT TIMERS" |
| `SNK_TrafficButTxt` | "TRAFFIC" |
| `SNK_CopsButTxt` | "POLICE" |
| `SNK_DifficultyButTxt` | "DIFFICULTY" |
| `SNK_DynamicsButTxt` | "DYNAMICS" |
| `SNK_3dCollisionsButTxt` | "3D COLLISIONS" |
| `SNK_CatchupTxt` | "CATCHUP" |
| `SNK_SpeedReadoutButTxt` | "SPEED READOUT" |
| `SNK_FoggingButTxt` | "FOGGING" |
| `SNK_ResolutionButTxt` | "RESOLUTION" |
| `SNK_CameraDampingButTxt` | "CAMERA DAMPING" |
| `SNK_SplitScreenButTxt` | "SPLIT SCREEN" |

### Option Value Arrays
| Export | Values |
|---|---|
| `SNK_OnOffTxt` | char*[]: "ON", "OFF" (used for toggles) |
| `SNK_DifficultyTxt` | char*[]: difficulty level names |
| `SNK_DynamicsTxt` | char*[]: "SIMULATION", "ARCADE" |
| `SNK_SpeedReadTxt` | char*[]: speed readout type names |
| `SNK_SFX_Modes` | char*[]: "3D SOUND", "STEREO", "MONAURAL" |
| `SNK_Ctrl_Modes` | char*[]: "<NONE>", "WHEEL", "JOYPAD", "JOYSTICK", "KEYBOARD" |
| `SNK_Split_Modes` | char*[]: split screen type names |

### Car Selection
| Export | Type | Description |
|---|---|---|
| `SNK_CarSelect_MT1` | char* | Car select help text (multi-line) |
| `SNK_CarSelect_Ex` | char[][32] | Extra car select strings ("CLICK TO GO BACK TO MAIN MENU", etc.) |
| `SNK_CarButTxt` | char* | "CAR" |
| `SNK_PaintButTxt` | char* | "PAINT" |
| `SNK_ConfigButTxt` | char* | "STATS" |
| `SNK_AutoButTxt` | char* | "AUTOMATIC" |
| `SNK_ManualButTxt` | char* | "MANUAL" |
| `SNK_CarLongNames` | char*[][5] | Full car names with 5 paint variants per car |

### Vehicle Names (from SNK_CarLongNames, 30+ cars)
- 1999 ASTON MARTIN PROJECT VANTAGE
- 1998 JAGUAR XKR
- 1998 CHEVROLET CORVETTE
- 1998 ASTON MARTIN V8 VANTAGE
- 1998 TVR CERBERA / TVR SPEED 12
- 1998 DODGE VIPER / VIPER GTS-R
- 1998 FORD MUSTANG GT / 1998 SALEEN MUSTANG S351
- 1998 NISSAN SKYLINE / NISSAN R390 GT-1
- 1997 CHEVROLET CAMARO Z28 SS LT4
- 1970 CHEVROLET CHEVELLE SS 454 LS6
- 1969 CHEVROLET CAMARO ZL1 / CORVETTE ZL1
- 1971 PLYMOUTH HEMI CUDA
- 1969 DODGE CHARGER
- 1968 FORD MUSTANG 428CJ
- 1967 PONTIAC GTO
- 1966 SHELBY COBRA 427SC
- 1994 JAGUAR XJ220
- CATERHAM SUPER 7
- PITBULL SPECIAL
- Police variants: POLICE CHEVROLET CAMARO, POLICE DODGE CHARGER, POLICE FORD MUSTANG, POLICE TVR CERBERA
- Easter eggs: FEAR FACTORY WAGON, THE MIGHTY MAUL, CHRIS'S BEAST, HOT DOG

### Paint Variants (per car)
(RED), (BLACK), (GREY), (YELLOW), (BLUE), (GREEN), (WHITE), (WHITE/ORANGE), (RED/YELLOW), (RED/YELLOW FIRE), (BLUE/WHITE), (BLUE/YELLOW)

### Track Selection
| Export | Type | Description |
|---|---|---|
| `SNK_TrackSel_MT1` | char* | Track select help text |
| `SNK_TrackSel_Ex` | char[][32] | Track select extra strings |
| `SNK_TrackButTxt` | char* | "TRACK" |
| `SNK_ForwardsButTxt` | char* | "FORWARDS" |
| `SNK_BackwardsButTxt` | char* | "BACKWARDS" |
| `SNK_ChangeTrackButTxt` | char* | "CHANGE TRACK" |
| `SNK_SelectTrackButTxt` | char* | "SELECT TRACK" (listed in SNK_TrackButTxt referencing the track select button) |
| `SNK_TrackNames` | char*[] | 26-entry track name table |

### Track Names (from SNK_TrackNames, 26 entries)
1. DRAG STRIP
2. MONTEGO BAY, JAMAICA
3. HOUSE OF BEZ, ENGLAND
4. NEWCASTLE, ENGLAND
5. MAUI, HAWAII, USA
6. COURMAYEUR, ITALY
7. JARASH, JORDAN
8. CHEDDAR CHEESE, ENGLAND
9. MOSCOW, RUSSIA
10. BLUE RIDGE PARKWAY, NC, USA
11. EDINBURGH, SCOTLAND
12. TOKYO, JAPAN
13. SYDNEY, AUSTRALIA
14. HONOLULU, HAWAII, USA
15. MUNICH, GERMANY
16. WASHINGTON, DC, USA
17. KYOTO, JAPAN
18. BERN, SWITZERLAND
19. SAN FRANCISCO, CA, USA
20. KESWICK, ENGLAND
21-26: Likely reverse variants or duplicates

### Car Info / Stats
| Export | Description |
|---|---|
| `SNK_Config_Hdrs` | char*[] -- info headers: "LAYOUT:", "GEARS:", "TIRES:", "TOP SPEED:", "0 to 60 MPH:", "60 to 0 MPH:", "1/4 MILE:", "ENGINE:", "COMPRESSION:", "DISPLACEMENT:", "LATERAL ACC:", "TORQUE:" |
| `SNK_Info_Values` | char*[][10] -- info value lookup tables |
| `SNK_Layout_Types` | char*[]: "FRONT/REAR", "FRONT/4-WHEEL", "FRONT/FRONT", "MID/4-WHEEL", "MID/REAR", "UNKNOWN" |
| `SNK_Engine_Types` | char*[]: "V10 ALUMINIUM", "V8 ALUMINIUM BLOCK", "V8 SUPERCHARGED", "V8 ALUMINIUM", "DOHC TWIN TURBO", "FORD IRON BLOCK", "PONTIAC IRON BLOCK", "V8 IRON BLOCK", "V8 IRON BLOCK HEMI", "ALUMINIUM BLOCK", "4 CYLINDER", "V6 SUPERCHARGED", "V10 SUPERCHARGED", "V8 DOHC", "V8 TWIN TURBO", "V12 IRON BLOCK" |

### Race Results
| Export | Description |
|---|---|
| `SNK_ResultsTxt` | char[][32]: "CUP POSITION", "CUP POINTS", "AVERAGE SPEED", "TOP SPEED", "FINISH POSITION", "HIGHEST POSITION", "TOTAL TIME", "CHECKPOINT TIMERS" |
| `SNK_CCResultsTxt` | char[][32]: Cop Chase results: "CUP TIME", "ARRESTS", "AVERAGE SPEED", "TOP SPEED", "POINTS" |
| `SNK_DRResultsTxt` | char[][32]: Drag Race results: "TOTAL TIME", "TOP SPEED", "FINISH POSITION" |

### Post-Race Menu
| Export | Value |
|---|---|
| `SNK_RaceAgain` | "RACE AGAIN" |
| `SNK_ViewReplay` | "VIEW REPLAY" |
| `SNK_ViewRaceData` | "VIEW RACE DATA" |
| `SNK_SelectNewCar` | "SELECT NEW CAR" |
| `SNK_Quit` | "QUIT" |
| `SNK_SaveRaceStatus` | "SAVE RACE STATUS" |
| `SNK_NextCupRace` | "NEXT CUP RACE" |

### HUD & Race Text
| Export | Value |
|---|---|
| `SNK_SpdTxt` | "SPEED" |
| `SNK_LapTxt` | "LAP" |
| `SNK_TimeTxt` | "TIME" |
| `SNK_TopTxt` | "TOP" |
| `SNK_AvgTxt` | "AVERAGE" |
| `SNK_CarTxt` | "CAR" |
| `SNK_BestTxt` | "BEST" |
| `SNK_NameTxt` | "NAME" |
| `SNK_PtsTxt` | "POINTS" |
| `SNK_LockedTxt` | "LOCKED" |

### Unit Suffixes
| Export | Value |
|---|---|
| `SNK_ConfSpeed` | " SPEED" |
| `SNK_ConfMph` | " MPH" |
| `SNK_ConfSec` | " SEC" |
| `SNK_ConfFt` | " FT" |
| `SNK_ConfRpm` | " RPM" |
| `SNK_ConfUnknown` | "UNKNOWN" |

### Network / Multiplayer
| Export | Value |
|---|---|
| `SNK_ChooseConnectionButTxt` | "CHOOSE CONNECTION" |
| `SNK_ChooseSessionButTxt` | "CHOOSE SESSION" |
| `SNK_EnterNewSessionNameButTxt` | "ENTER NEW SESSION NAME" |
| `SNK_EnterPlayerNameButTxt` | "ENTER PLAYER NAME" |
| `SNK_MessageWindowButTxt` | "MESSAGE WINDOW" |
| `SNK_StatusButTxt` | "STATUS" |
| `SNK_NewSessionTxt` | "< New Session >" |
| `SNK_TxtSession` | "Session:" |
| `SNK_TxtPlayer` | "Player:" |
| `SNK_TxtBhostB` | "(HOST)" |
| `SNK_SeshJoinMsg` | " has joined the session." |
| `SNK_SeshLeaveMsg` | " has left the session." |
| `SNK_NowHostMsg` | " is now the HOST." |
| `SNK_SeshCreateMsg` | " Session Created." |
| `SNK_MsgBufferFull` | " <Message Buffer Full>" |
| `SNK_WaitForHostMsg` | " WAITING FOR HOST TO START THE GAME." |
| `SNK_SeshLockedTxt` | "SESSION LOCKED" |
| `SNK_NetPlayStatMsg` | char[][10]: network status messages |
| `SNK_NetErrString1` | char[][40]: "NOT ALL PLAYERS ARE WAITING" |
| `SNK_NetErrString2` | char[][32]: "THERE ARE NO PLAYERS WAITING" |
| `SNK_NetErrString3` | "TO START. ARE YOU SURE ?" |
| `SNK_NetErrString4` | "ARE YOU SURE YOU WANT TO EXIT THIS SESSION ?" |

### Game State Messages
| Export | Value |
|---|---|
| `SNK_CongratsTxt` | "CONGRATULATIONS!" |
| `SNK_YouHaveWonTxt` | "YOU HAVE WON THE" |
| `SNK_SorryTxt` | "SORRY" |
| `SNK_YouFailedTxt` | "YOU FAILED" |
| `SNK_ToWinTxt` | "TO WIN" |
| `SNK_BeautyTxt` | "BEAUTY" |
| `SNK_BeastTxt` | "BEAST" |
| `SNK_CarsUnlocked` | "CARS UNLOCKED" |
| `SNK_TracksUnlocked` | "TRACKS UNLOCKED" |
| `SNK_BlockSavedOK` | "BLOCK SAVED OK" |
| `SNK_FailedToSave` | "SAVE FAILED" |
| `SNK_MustSelectTxt` | "YOU MUST SELECT A CONTROLLER" |
| `SNK_MustPlayer1Txt` | "FOR PLAYER 1" |
| `SNK_MustPlayer2Txt` | "FOR PLAYER 2" |

### Controller Config
| Export | Value |
|---|---|
| `SNK_ControllerTxt` | "CONTROLLER" |
| `SNK_ControlText` | char[][16]: "LEFT", "RIGHT", "ACCELERATE", "BRAKE", "HANDBRAKE", "HORN/SIREN", "GEAR UP", "GEAR DOWN", "CHANGE VIEW", "REAR VIEW" |
| `SNK_UpDownTxt` | "UP/DOWN" |
| `SNK_UpTxt` | "UP" |
| `SNK_DownTxt` | "DOWN" |
| `SNK_PedalsTxt` | "PEDALS" |
| `SNK_ButtonTxt` | "BUTTON" |
| `SNK_NowPlayingTxt` | "NOW PLAYING:" |
| `SNK_PressKeyTxt` | "PRESS THE KEY TO USE FOR" |
| `SNK_PressingTxt` | "PRESS A BUTTON TO CHANGE THE CONFIGURATION FOR THAT BUTTON" |
| `SNK_ConfigurationTxt` | "CONFIGURATION" |
| `SNK_ConfigureButTxt` | "CONFIGURE" |
| `SNK_Player1ButTxt` | "PLAYER 1" |
| `SNK_Player2ButTxt` | "PLAYER 2" |

### Menu Screen Titles (SNK_MenuStrings, char*[] with 15 entries)
"MAIN MENU", "ULTIMATE CUP", "MASTERS CUP", "PITBULL CUP", "CHALLENGE CUP", "ERA CUP", "CHAMPIONSHIP CUP", "HIGH SCORES", "NO CONTROLLER", "QUICK RACE", "RACE MENU", "OPTIONS", "NET PLAY", "SELECT CAR", "SELECT TRACK", ...

### Quick Race Menu
| Export | Value |
|---|---|
| `SNK_QuickRaceMenu_MT` | Quick race menu help text |
| `SNK_QRTxtCar` | "CAR" |
| `SNK_QRTxtTrack` | "TRACK" |
| `SNK_ChangeCarButTxt` | "CHANGE CAR" |

### Music Test
| Export | Value |
|---|---|
| `SNK_MusicTest_MT` | Music test screen help text |
| `SNK_MusicTestButTxt` | "MUSIC TEST" |
| `SNK_MusicVolumeButTxt` | "MUSIC VOLUME" |
| `SNK_SfxVolumeButTxt` | "SFX VOLUME" |
| `SNK_SfxModeButTxt` | "SFX MODE" |

### Credits (SNK_CreditsText, char[][24], ~200 entries)
Full game credits stored as fixed-width 24-byte entries:
- "TEST DRIVE 5", "(C) ACCOLADE", "1998"
- Programming: BOB TROUGHTON, GARETH BRIGGS, STEVE SNAKE
- Additional Programming: MICHAEL TROUGHTON, CHRIS KIRBY, HEADLEY LEMARR
- 3D Art: STEVE DIETZ, JONATHAN F. KAY, RICHARD MCDONALD, MIKE PIRSO, RICHARD BESTON
- 2D Art: LES BURNEY, TONY PRINGLE, JOHN STEELE, DAVID TAYLOR
- AI and Testing: DAZ KELLY, TONY CHARLTON
- Studio Manager: DAVID BURTON
- Executive Producer: CHRIS DOWNEND
- Producer: SLADE ANDERSON
- Sound Effects: TOMMY TALLARICO STUDIOS
- (extensive Accolade QA and marketing credits follow)

### Game Type Short Labels (SNK_GameTypeTxt, char[][16])
"SINGLE", "CHALNG", "CHAMP", "PITBULL", "MASTERS", "ULTIMATE", "DRAGRACE", "COPCHASE"

## How the EXE Uses Language.dll Strings

### Direct Data Access
The EXE reads the SNK_ exported variables directly through IAT dereferences:
```c
// Simple string label -- just a char*
CreateFrontendDisplayModeButton((byte *)SNK_CopsButTxt_exref, ...);

// Array indexing for option values -- char*[]
DrawString(*(byte **)(SNK_OnOffTxt_exref + toggleValue * 4), ...);

// Multi-line help text -- passed as block to inline string renderer
SetFrontendInlineStringTable((char *)SNK_GameOptions_MT_exref, 0, 0);
```

### Car Configuration Localization
In `ScreenLocalizationInit`, the EXE uses `SNK_LangDLL[8]` as a language index to select which `config.<lang>` file to read from each car ZIP archive. The config files contain 17 newline-separated fields (car name, manufacturer, stats, engine code, layout code, etc.). Coded byte values in the layout/engine fields are resolved to display strings via `SNK_Layout_Types` and `SNK_Engine_Types` arrays.

### Frontend Screen Help Text (_MT suffix)
Each options/menu screen has an associated `_MT` (Menu Text) export containing multi-line tooltip/help text. These are displayed as scrolling help banners in the frontend, describing what each button does.

### Array Exports (_Ex suffix)
The `_Ex` exports are typically char[][32] arrays containing auxiliary screen strings like "CLICK TO GO BACK TO MAIN MENU".

## Localization Coverage

**6 planned languages** (based on config file extensions):
1. English (`config.eng`, index '0')
2. French (`config.fre`)
3. German (`config.ger`)
4. Italian (`config.ita`)
5. Spanish (`config.spa`)
6. Info/fallback (`config.nfo`)

**Only 2 Language.dll variants shipped on this ISO:** English (49152 bytes) and German (41472 bytes). The German DLL has fewer exports (123 vs. 170) and lacks SNK_LangDLL, suggesting it was built against an earlier EXE revision.

## Interesting Findings

1. **"HOUSE OF BEZ, ENGLAND"** and **"CHEDDAR CHEESE, ENGLAND"** are fictional/humorous track names
2. **Easter egg cars**: "FEAR FACTORY WAGON - KILLER MAN!", "BEHOLD! THE MIGHTY MAUL!", "CHRIS'S BEAST !", "HOT DOG"
3. **Beauty/Beast divisions**: Era Cup divides cars into "BEAUTY" (new) and "BEAST" (old) groups
4. **Cop Chase mechanic**: "HIT THE SIREN AND SPIN THE CARS OUT TO GIVE OUT THE TICKETS AND INSURANCE POINTS"
5. **"ACCEL/BRAKE" control**: Combined pedal axis support in controller config
6. **Masters Cup pool mechanic**: 10 randomly chosen cars, use-once-only rule
7. **SNK prefix**: "Snake" -- Steve Snake was one of three core programmers (Pitbull Syndicate)
