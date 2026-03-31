# Language.dll Full Analysis -- Test Drive 5 Localization DLL

## Overview

Language.dll is a localization/string-table DLL for Test Drive 5 (Pitbull Syndicate / Accolade, 1998). It is a 32-bit Windows DLL compiled with MSVC 6.0. The DLL exports **171 named data symbols** (plus the `entry` function) that contain all user-facing strings for the game's menus, HUD, results screens, credits, multiplayer messages, and configuration labels. The game links against these exported symbols at load time.

The DLL contains **no resource tables** (no `LoadString`, `FindResource`, etc.). All strings are stored as plain initialized C data in the `.data` section. The game uses C++ decorated name exports (`?SNK_*@@3PADA` = `char* SNK_*`, `?SNK_*@@3PAPADA` = `char** SNK_*`, `?SNK_*@@3PAY0...DA` = `char(*)[N] SNK_*`) to directly reference the string variables.

**Identifier string:** `"LANGDLL 0 : ENGLISH/US"` at `0x10006030` (the `SNK_LangDLL` export).

---

## DLL Interface: Exports (172 total)

### Single export function

| Address | Name | Purpose |
|---------|------|---------|
| `0x100010b8` | `entry` (DllMain) | Standard DLL entry point |

### Exported data symbols (171 SNK_* variables)

All 171 data exports follow the naming convention `SNK_<Name>`. They fall into these categories:

#### Simple string pointers (`char* SNK_*` / `@@3PADA`)

These are single null-terminated strings. Examples:

| Symbol | Address | Value |
|--------|---------|-------|
| `SNK_LangDLL` | `0x10006030` | `"LANGDLL 0 : ENGLISH/US"` |
| `SNK_AutoButTxt` | `0x100072b0` | `"AUTOMATIC"` |
| `SNK_ManualButTxt` | `0x100072bc` | `"MANUAL"` |
| `SNK_BackButTxt` | `0x100072c8` | `"BACK"` (contextual) |
| `SNK_ExitButTxt` | `0x10007360` | `"EXIT"` |
| `SNK_StartButTxt` | `0x1000734c` | `"START"` |
| `SNK_YesButTxt` | `0x10007368` | `"YES"` |
| `SNK_OkButTxt` | `0x100072c4` | `"OK"` |
| `SNK_LockedTxt` | `0x10007984` | `"LOCKED"` |
| `SNK_CongratsTxt` | `0x10007a00` | `"CONGRATULATIONS!"` |
| `SNK_YouHaveWonTxt` | `0x10007a14` | `"YOU HAVE WON THE"` |
| `SNK_YouFailedTxt` | `0x100079ec` | `"YOU FAILED"` |
| `SNK_SorryTxt` | `0x100079e4` | `"SORRY"` |
| `SNK_BlockSavedOK` | `0x100079c8` | `"BLOCK SAVED OK"` |
| `SNK_FailedToSave` | `0x100079d8` | `"SAVE FAILED"` |
| `SNK_RaceAgain` | `0x10007920` | `"RACE AGAIN"` |
| `SNK_ViewReplay` | `0x1000792c` | `"VIEW REPLAY"` |
| `SNK_ViewRaceData` | `0x10007938` | `"VIEW RACE DATA"` |
| `SNK_SelectNewCar` | `0x10007948` | `"SELECT NEW CAR"` |
| `SNK_SaveRaceStatus` | `0x10007960` | `"SAVE RACE STATUS"` |
| `SNK_NextCupRace` | `0x10007974` | `"NEXT CUP RACE"` |
| `SNK_Quit` | `0x10007958` | `"QUIT"` |
| `SNK_CarsUnlocked` | `0x10007a48` | `"CARS UNLOCKED"` |
| `SNK_TracksUnlocked` | `0x10007a58` | `"TRACKS UNLOCKED"` |
| `SNK_SeshLockedTxt` | `0x10007a38` | `"SESSION LOCKED"` |
| `SNK_BeautyTxt` | `0x10007a28` | `"BEAUTY"` |
| `SNK_BeastTxt` | `0x10007a30` | `"BEAST"` |
| `SNK_NowPlayingTxt` | `0x100075c4` | `"NOW PLAYING:"` |
| `SNK_ControllerTxt` | `0x100075d4` | `"CONTROLLER"` |
| `SNK_PressKeyTxt` | `0x10007704` | `"PRESS THE KEY TO USE FOR"` |
| `SNK_PressingTxt` | `0x100076c4` | `"PRESS A BUTTON TO CHANGE THE"` |
| `SNK_ConfigurationTxt` | `0x100076e4` | `"CONFIGURATION FOR THAT BUTTON"` |
| `SNK_MustSelectTxt` | `0x10006f5c` | `"YOU MUST SELECT A CONTROLLER"` |
| `SNK_MustPlayer1Txt` | `0x10006f7c` | `"FOR PLAYER 1"` |
| `SNK_MustPlayer2Txt` | `0x10006f8c` | `"FOR PLAYER 2"` |

(Plus ~90 more button labels, status messages, multiplayer strings, etc.)

#### String arrays (`char** SNK_*` / `@@3PAPADA`)

These are arrays of `char*` pointers, used for enumerated choices:

| Symbol | Address | Content |
|--------|---------|---------|
| `SNK_OnOffTxt` | `0x1000720c` | `{"OFF", "ON"}` |
| `SNK_DifficultyTxt` | `0x10007214` | Difficulty level names |
| `SNK_DynamicsTxt` | `0x10007220` | Dynamics mode names |
| `SNK_SpeedReadTxt` | `0x10007204` | Speed readout type names |
| `SNK_TrackNames` | `0x10007234` | Array of track name strings |
| `SNK_CarLongNames` | `0x10006b9c` | Car long names array (`char(*)[5][]`) |
| `SNK_RaceTypeText` | `0x10006048` | Race type label array |
| `SNK_Ctrl_Modes` | `0x100071f0` | Controller mode names |
| `SNK_SFX_Modes` | `0x100071e4` | SFX mode names |
| `SNK_Split_Modes` | `0x100071dc` | Split-screen mode names |
| `SNK_Config_Hdrs` | `0x10006e80` | Configuration header strings |
| `SNK_Layout_Types` | `0x10006ed0` | Layout type names |
| `SNK_Engine_Types` | `0x10006ee8` | Engine type names |
| `SNK_MenuStrings` | `0x10007a68` | General menu string array |

#### Fixed-size 2D string arrays (`char(*)[N] SNK_*` / `@@3PAY0...DA`)

Used for multi-line/multi-column display:

| Symbol | Address | Mangled Type | Element Size |
|--------|---------|-------------|-------------|
| `SNK_ResultsTxt` | `0x10007720` | `PAY0CA@DA` | `char[32][]` -- results column headers |
| `SNK_CCResultsTxt` | `0x10007840` | `PAY0CA@DA` | `char[32][]` -- cop chase results |
| `SNK_DRResultsTxt` | `0x100078c0` | `PAY0CA@DA` | `char[32][]` -- drag race results |
| `SNK_GameTypeTxt` | `0x10007038` | `PAY0BA@DA` | `char[16][]` -- game type labels |
| `SNK_ControlText` | `0x100075e0` | `PAY0BA@DA` | `char[16][]` -- control button labels |
| `SNK_CreditsText` | `0x10007ad0` | `PAY0BI@DA` | `char[24][]` -- credits scroll text |
| `SNK_NetErrString1` | `0x100070f4` | `PAY0CI@DA` | `char[40][]` -- net error messages |
| `SNK_NetErrString2` | `0x10007144` | `PAY0CA@DA` | `char[32][]` -- net error messages |
| `SNK_NetPlayStatMsg` | `0x100071b4` | `PAY09DA` | `char[9][]` -- net play status |
| `SNK_Info_Values` | `0x10006f34` | `PAY109PADA` | `char*[10][2]` -- info values table |
| `SNK_CarSelect_Ex` | `0x1000616c` | `PAY0CA@DA` | `char[32][]` -- car select help text |
| `SNK_TrackSel_Ex` | `0x10006858` | `PAY0CA@DA` | `char[32][]` -- track select help text |

#### Menu text arrays (single concatenated strings separated in data)

These are help/tooltip text for menu items, stored as contiguous strings:

| Symbol | Address | Description |
|--------|---------|-------------|
| `SNK_CarSelect_MT1` | `0x10006078` | Car selection menu text |
| `SNK_Options_MT` | `0x100061ac` | Options menu text |
| `SNK_CtrlOptions_MT` | `0x10006278` | Control options menu text |
| `SNK_CtrlOptions_Ex` | `0x10006388` | Control options extended text |
| `SNK_SfxOptions_MT` | `0x100063a8` | SFX options menu text |
| `SNK_GameOptions_MT` | `0x10006470` | Game options menu text |
| `SNK_GfxOptions_MT` | `0x100065a8` | Graphics options menu text |
| `SNK_TwoOptions_MT` | `0x100066e0` | Two player options text |
| `SNK_QuickRaceMenu_MT` | `0x10006744` | Quick race menu text |
| `SNK_TrackSel_MT1` | `0x100067f4` | Track selection menu text |
| `SNK_MusicTest_MT` | `0x10006898` | Music test menu text |
| `SNK_MainMenu_MT` | `0x100068fc` | Main menu text |
| `SNK_MainMenu_Ex` | `0x100069b0` | Main menu extended text |
| `SNK_RaceMenu_MT` | `0x100069d0` | Race menu text |
| `SNK_RaceMen2_MT` | `0x10006a74` | Race menu page 2 text |

#### HUD/confirmation format strings

| Symbol | Address | Value |
|--------|---------|-------|
| `SNK_ConfSpeed` | `0x10007aa4` | `" SPEED"` |
| `SNK_ConfMph` | `0x10007aac` | (mph unit) |
| `SNK_ConfSec` | `0x10007ab4` | (seconds unit) |
| `SNK_ConfFt` | `0x10007abc` | (feet unit) |
| `SNK_ConfRpm` | `0x10007ac0` | (RPM unit) |
| `SNK_ConfUnknown` | `0x10007ac8` | `"UNKNOWN"` |

#### Multiplayer strings

| Symbol | Address | Value |
|--------|---------|-------|
| `SNK_SeshJoinMsg` | `0x10006f9c` | `" has joined the session."` |
| `SNK_SeshLeaveMsg` | `0x10006fb8` | `" has left the session."` |
| `SNK_NowHostMsg` | `0x10006fd0` | `" is now the HOST."` |
| `SNK_SeshCreateMsg` | `0x10006fe4` | `" Session Created."` |
| `SNK_MsgBufferFull` | `0x10006ff8` | `" <Message Buffer Full>"` |
| `SNK_WaitForHostMsg` | `0x10007010` | `" WAITING FOR HOST TO START THE GAME."` |
| `SNK_NewSessionTxt` | `0x100070c8` | `"< New Session >"` |
| `SNK_TxtSession` | `0x100070d8` | `"Session:"` |
| `SNK_TxtPlayer` | `0x100070e4` | `"Player:"` |
| `SNK_TxtBhostB` | `0x100070ec` | `"(HOST)"` |

#### Credits text

`SNK_CreditsText` at `0x10007ad0` contains the full credits scroll: "TEST DRIVE 5", "(C) ACCOLADE", "DEVELOPED BY", "THE PITBULL SYNDICATE", programmer names (Bob Troughton, Gareth Briggs, Steve Snake, etc.), artists, QA, and producers.

---

## Function Analysis (53 functions total)

### Game-Specific Function (1)

| Address | Proposed Name | Description |
|---------|--------------|-------------|
| `0x1000193f` | `DllMain_UserInit` | The actual user DllMain. On `DLL_PROCESS_ATTACH` (reason=1), calls `DisableThreadLibraryCalls()` if no user DllMain callback is set. Always returns 1. This is the only game-specific logic -- the DLL has no runtime behavior, it just exposes data. |

### CRT Entry and Initialization (14 functions)

| Address | Proposed Name | Signature | Description |
|---------|--------------|-----------|-------------|
| `0x100010b8` | `entry` / `_DllMainCRTStartup` | `int (HMODULE, int, void*)` | Standard MSVC DLL entry point. Dispatches to CRT init (`FUN_10001000`) and user DllMain (`FUN_1000193f`). Handles attach/detach/thread scenarios. |
| `0x10001000` | `__CRT_INIT` | `int (void*, int)` | CRT initialization/termination dispatcher. On attach (reason=1): calls `GetVersion`, creates CRT heap, gets command line, gets environment strings, sets up file handles, runs C++ constructors. On detach (reason=0): runs C++ destructors, frees file handles, destroys heap. |
| `0x10001155` | `__amsg_exit` | `void (int)` | CRT fatal error -- displays runtime error message and terminates. |
| `0x100011b5` | `__exit` | `void (int)` | CRT exit wrapper. |
| `0x10001188` | `__initterm_and_user_init` | `void (void)` | Calls optional user init callback, then runs two C initializer tables via `_initterm`. |
| `0x100011c6` | `__doexit_wrap` | `void (void)` | Wrapper that calls the exit/atexit handler with code 0. |
| `0x100011d5` | `__doexit` | `void (UINT, int, int)` | CRT exit handler. Runs atexit callbacks, calls C terminator tables, optionally calls `ExitProcess`. |
| `0x1000126e` | `_initterm` | `void (void**, void**)` | Iterates a table of function pointers between two bounds, calling each non-null entry. Standard MSVC CRT initializer table walker. |
| `0x10001288` | `__heap_init` | `int (int)` | Creates the CRT private heap via `HeapCreate`. Initializes the small-block heap allocator. |
| `0x100012c4` | `__heap_destroy` | `void (void)` | Destroys all virtual-alloc regions and the CRT heap. |
| `0x10001339` | `__ioinit` | `void (void)` | Initializes the CRT file handle table from startup info. Sets up stdin/stdout/stderr handles. |
| `0x100014e4` | `__ioterm` | `void (void)` | Frees all CRT file handle table entries. |
| `0x10001507` | `__setenvp` | `void (void)` | Parses environment strings into the `_environ` array (null-terminated `char*[]`). |
| `0x100015c0` | `__setargv` | `void (void)` | Parses the command line into `__argc` / `__argv`. Gets module filename, then tokenizes command line. |

### CRT Command Line Parser (1)

| Address | Proposed Name | Signature | Description |
|---------|--------------|-----------|-------------|
| `0x10001659` | `__parse_cmdline` | `void (byte*, void**, byte*, int*, int*)` | Parses a command line string into argv-style tokens, handling quoted arguments and backslash escaping. Standard MSVC implementation. |

### CRT Environment Handling (1)

| Address | Proposed Name | Signature | Description |
|---------|--------------|-----------|-------------|
| `0x1000180d` | `__crtGetEnvironmentStrings` | `LPSTR (void)` | Gets environment strings, preferring wide (W) version and converting to multibyte. Falls back to ANSI version. Standard MSVC CRT. |

### CRT Runtime Error Display (2)

| Address | Proposed Name | Signature | Description |
|---------|--------------|-----------|-------------|
| `0x1000195f` | `__FF_MSGBANNER` | `void (void)` | Displays the runtime error banner prefix/suffix (codes `0xFC`/`0xFF`). |
| `0x10001998` | `__NMSG_WRITE` | `void (DWORD)` | Looks up a runtime error code in a table and either writes it to stderr or shows a MessageBox with the MSVC runtime error dialog. |

### CRT Small-Block Heap Allocator (8)

These implement the MSVC 6.0 small-block heap (SBH), an optimization layer over `HeapAlloc` for small allocations:

| Address | Proposed Name | Signature | Description |
|---------|--------------|-----------|-------------|
| `0x10001aeb` | `__sbh_heap_init` | `int (void)` | Initializes the SBH region table. Allocates initial 0x140-byte region descriptor array (16 entries). |
| `0x10001b29` | `__sbh_find_block` | `uint (int)` | Searches SBH region descriptors to find which 1MB region contains a given address. |
| `0x10001b54` | `__sbh_free_block` | `void (uint*, uint)` | Frees a block within the SBH. Coalesces adjacent free blocks, updates free-list bitmaps, may decommit/release pages. |
| `0x10001e7f` | `__sbh_alloc_block` | `int* (uint*)` | Allocates a block from the SBH. Searches regions by bitmap for a free block of sufficient size. May commit new pages or create new regions. |
| `0x10002188` | `__sbh_alloc_new_region` | `void* (void)` | Allocates a new 1MB virtual region for the SBH. Reserves 1MB via `VirtualAlloc(MEM_RESERVE)`, allocates a 0x41C4-byte header via `HeapAlloc`. |
| `0x10002239` | `__sbh_alloc_new_group` | `int (int)` | Commits a 32KB page group within an SBH region. Sets up the free list for the 0xFF0-byte pages. |
| `0x10002372` | `__sbh_alloc_block_or_heap` | `void (uint*)` | Tries SBH allocation first; falls back to `HeapAlloc` for large or failed allocations. |
| `0x100023a8` | `__sbh_free_or_heap_free` | `void (LPVOID)` | Frees memory: checks if address is in SBH region, uses SBH free if so, otherwise `HeapFree`. |

### CRT String Operations (4)

| Address | Proposed Name | Signature | Description |
|---------|--------------|-----------|-------------|
| `0x100023e0` | `_strcpy` | `char* (char*, char*)` | Optimized word-aligned `strcpy` implementation. |
| `0x100023f0` | `_strcat` | `char* (char*, char*)` | Optimized word-aligned `strcat` implementation. Finds end of dest, then copies src. |
| `0x100024d0` | `_strlen` | `size_t (char*)` | Standard `strlen`. Already named by Ghidra. |
| `0x10002cf0` | `_strncpy` | `char* (char*, char*, size_t)` | Standard `strncpy`. Already named by Ghidra. |

### CRT Memory Operations (3)

| Address | Proposed Name | Signature | Description |
|---------|--------------|-----------|-------------|
| `0x10002930` | `_memcpy` | `void* (void*, void*, uint)` | Optimized `memcpy` with overlap-safe memmove behavior (copies backwards if dst > src). |
| `0x10002df0` | `_memmove` | `void* (void*, void*, uint)` | Identical overlap-safe memory move. Duplicate of the above pattern. |
| `0x100036e0` | `_memset` | `void* (void*, int, size_t)` | Standard `memset`. Already named by Ghidra. |

### CRT Heap API Wrappers (2)

| Address | Proposed Name | Signature | Description |
|---------|--------------|-----------|-------------|
| `0x10002334` | `_malloc` | `void* (size_t)` | Standard `malloc`. Already named by Ghidra. Calls `__nh_malloc`. |
| `0x10002346` | `__nh_malloc` | `void* (size_t, int)` | New-handler aware malloc. Already named by Ghidra. |

### CRT Locale / Codepage Functions (7)

| Address | Proposed Name | Signature | Description |
|---------|--------------|-----------|-------------|
| `0x1000254b` | `___initmbctable` / `_setmbcp` | `int (int)` | Sets the multibyte codepage. Looks up codepage in a built-in table of known CJK codepages (932=Japanese, 936=Simplified Chinese, 949=Korean, 950=Traditional Chinese). Falls back to `GetCPInfo` for unknown codepages. Populates the `_mbctype` table with lead-byte flags. |
| `0x100026e4` | `___get_qualified_cp` | `int (int)` | Resolves special codepage constants: `-2` = OEM codepage (`GetOEMCP`), `-3` = ANSI codepage (`GetACP`), `-4` = thread locale codepage. |
| `0x1000272e` | `___crtGetLCIDFromCodePage` | `LCID (int)` | Maps codepage to LCID. 932->0x411 (Japanese), 936->0x804 (Simplified Chinese), 949->0x412 (Korean), 950->0x404 (Traditional Chinese). Returns 0 for others. |
| `0x10002761` | `___initmbctable_default` | `void (void)` | Resets the `_mbctype` table to all zeros (single-byte mode). |
| `0x1000278a` | `___initctype` | `void (void)` | Builds the CRT ctype table (upper/lower case mappings) using `GetCPInfo`, `GetStringTypeW`/`A`, and `LCMapStringW`/`A`. |
| `0x1000290f` | `___initmbctable_once` | `void (void)` | One-time initializer: calls `_setmbcp(-3)` (ANSI codepage) on first call. |
| `0x10003364` | `__strnlen` | `int (char*, int)` | Finds the length of a string up to a maximum, for safe locale operations. |

### CRT Locale Wrapper Functions (3)

| Address | Proposed Name | Signature | Description |
|---------|--------------|-----------|-------------|
| `0x10003140` | `___crtLCMapStringA` | `int (LCID, uint, char*, int, LPWSTR, int, UINT, int)` | CRT wrapper around `LCMapStringW`/`LCMapStringA`. Probes whether the W version works, caches result. Handles multibyte<->wide conversion. |
| `0x1000338f` | `___crtGetStringTypeA` | `BOOL (DWORD, LPCSTR, int, LPWORD, UINT, LCID, int)` | CRT wrapper around `GetStringTypeW`/`GetStringTypeA`. Same W-first probe pattern. |
| `0x10003125` | `___crtLCMapStringA_helper` | `int (void*)` | Calls an indirect function pointer (locale callback). Used internally by the locale system. |

### CRT MessageBox Wrapper (1)

| Address | Proposed Name | Signature | Description |
|---------|--------------|-----------|-------------|
| `0x10002c65` | `___crtMessageBoxA` | `int (char*, char*, uint)` | Dynamically loads `user32.dll` and calls `MessageBoxA` for runtime error dialogs. Caches the function pointer. |

### CRT SEH / Exception Handling (5)

| Address | Proposed Name | Signature | Description |
|---------|--------------|-----------|-------------|
| `0x100034d8` | `__global_unwind2` | `void (PVOID)` | SEH global unwind. Already named. Calls `RtlUnwind`. |
| `0x1000351a` | `__local_unwind2` | `void (int, int)` | SEH local unwind. Already named. Walks scope table. |
| `0x100035ae` | `__NLG_Notify` | `void (void)` | Non-Local Goto notification. Saves EAX and frame info for debugger support. |
| `0x1000368d` | `__seh_longjmp_unwind@4` | `void (int)` | SEH longjmp unwind handler. Already named. |
| `0x100036b0` | `__alloca_probe` / `__chkstk` | `void (void)` | Stack probe function. Touches stack pages in 4KB increments to ensure guard pages are hit. Used by functions with large local arrays. |

### Thunk (1)

| Address | Proposed Name | Description |
|---------|--------------|-------------|
| `0x10003738` | `RtlUnwind` (thunk) | Import thunk to `KERNEL32.DLL::RtlUnwind` |

---

## Data Structures

### String Table Organization

The DLL uses **no Win32 resource system**. All strings are statically initialized C/C++ global variables in the `.data` section (`0x10006000` - `0x1000B000` range). The data layout is:

1. **Simple strings** (`char SNK_Foo[]`): Null-terminated ASCII strings laid out consecutively. Menu items like "START", "EXIT", "BACK" are at `0x10007300+`.

2. **String pointer arrays** (`char* SNK_Foo[]`): Arrays of pointers to the simple strings. Used for enumeration displays (ON/OFF, difficulty levels, track names, car names).

3. **Fixed-size 2D arrays** (`char SNK_Foo[][N]`): Used for tabular data like results screens where each row has a fixed width (typically 16 or 32 bytes).

4. **Menu tooltip strings**: Long descriptive strings like `"USE LEFT/RIGHT TO CHANGE CURRENTLY SELECTED CAR"` are stored in the `0x10006000-0x10006C00` range, organized by menu screen.

### Codepage / Locale Tables (CRT internal)

At `0x1000AE00-0x1000AF00`, the CRT stores:
- `_mbctype` table at `0x1000B1E0` (257 bytes): Character type flags for multibyte codepage support
- Codepage descriptor table at `0x1000AE08`: Built-in data for codepages 932, 936, 949, 950 (each 0x30 bytes with lead-byte ranges)
- Case mapping table at `0x1000B0E0` (256 bytes): Upper/lower case conversion

---

## How the Game Uses This DLL

### Binding Mechanism

The game executable (`TD5.exe` or main game EXE) links against Language.dll at load time via the PE import table. Each `SNK_*` symbol is imported using its C++ decorated name (e.g., `?SNK_ExitButTxt@@3PADA`). The linker resolves these to direct pointers into Language.dll's data section.

### Usage Pattern

```c
// In the game executable, these are declared as:
extern "C" {
    extern char SNK_ExitButTxt[];        // "EXIT"
    extern char* SNK_TrackNames[];       // array of track name strings
    extern char SNK_ResultsTxt[][32];    // results table headers
    extern char SNK_CreditsText[][24];   // credits scroll lines
}

// The game directly reads these pointers/arrays for:
// - Drawing menu button labels
// - Populating option selection lists
// - Showing race results
// - Scrolling credits
// - Displaying multiplayer session messages
```

### Localization Strategy

To localize the game, a different Language.dll is provided with translated strings at the same symbol addresses. The `SNK_LangDLL` identifier changes (e.g., `"LANGDLL 1 : FRENCH"`) but all symbol names stay the same. The game does not need recompilation -- just a different DLL. This is a common late-1990s approach to localization.

The DLL identifier at `SNK_LangDLL` follows the format: `"LANGDLL <index> : <LANGUAGE/REGION>"`.

---

## Locale / Codepage Handling

The CRT locale code is standard MSVC 6.0 boilerplate, not game-specific. It:

1. Initializes the multibyte codepage from the system ANSI codepage (`GetACP()`) on DLL load
2. Has built-in knowledge of CJK codepages (932/936/949/950) with pre-computed lead-byte ranges
3. Maps codepages to LCIDs: 932->Japanese (0x411), 936->Chinese Simplified (0x804), 949->Korean (0x412), 950->Chinese Traditional (0x404)
4. Builds ctype/case-mapping tables using `GetStringTypeW`/`LCMapStringW` (or A fallbacks)

This locale code exists because it is statically linked into every MSVC 6.0 DLL. The game's actual localization does not use it -- the strings are plain ASCII/ANSI and the game presumably handles any codepage conversion at a higher level if needed.

---

## Win32 API Imports (35 total)

All imports are from `KERNEL32.DLL`:

| Category | APIs |
|----------|------|
| **Process/Startup** | `GetVersion`, `GetCommandLineA`, `GetStartupInfoA`, `GetModuleFileNameA`, `ExitProcess`, `TerminateProcess`, `GetCurrentProcess`, `DisableThreadLibraryCalls` |
| **Heap** | `HeapCreate`, `HeapDestroy`, `HeapAlloc`, `HeapFree`, `HeapReAlloc` |
| **Virtual Memory** | `VirtualAlloc`, `VirtualFree` |
| **File I/O** | `GetStdHandle`, `GetFileType`, `WriteFile`, `SetHandleCount` |
| **Environment** | `GetEnvironmentStrings`, `GetEnvironmentStringsW`, `FreeEnvironmentStringsA`, `FreeEnvironmentStringsW` |
| **Codepage/Locale** | `GetACP`, `GetOEMCP`, `GetCPInfo`, `MultiByteToWideChar`, `WideCharToMultiByte`, `LCMapStringA`, `LCMapStringW`, `GetStringTypeA`, `GetStringTypeW` |
| **Dynamic Loading** | `LoadLibraryA`, `GetProcAddress` (for user32.dll MessageBoxA) |
| **SEH** | `RtlUnwind` |

Note: **No** `LoadStringA/W`, `FindResource`, `LoadResource`, or any resource API is imported. The DLL does not use the Windows resource system.

---

## Summary

Language.dll is a pure data DLL with no meaningful runtime logic. Its sole purpose is to export ~170 named string variables that the TD5 game engine reads directly. The game links against the decorated C++ names at load time. All the code in the DLL (39 unnamed functions) is standard MSVC 6.0 CRT startup/shutdown boilerplate -- heap initialization, command-line parsing, environment setup, codepage tables, SEH handlers, and string/memory intrinsics. The only game-specific code is the trivial `DllMain` at `0x1000193f` which calls `DisableThreadLibraryCalls()`.

Swapping this DLL with a version containing translated strings is the intended localization mechanism for Test Drive 5.
