# Gap Functions Naming Wave 2

Second pass naming of FUN_ functions from the gap region recovery.
Session: `3bcaea0e0618455eb9cf66533dba6994`

## Summary

- **Functions renamed this wave:** 32 (all remaining FUN_ prefix functions from gap regions)
- **Previous wave (wave 1):** 43 named functions + switch-case fragments
- **Total named functions across both waves:** 75
- **Remaining unnamed:** ~64 switch-case fragments (caseD_ prefixed) -- these are not independent functions but rather compiler-generated fragments of named parent functions

## Methodology

Each FUN_ function was decompiled and analyzed for:
1. What APIs/globals it accesses
2. What it calls and who calls it
3. String references and data patterns
4. Position in the code relative to known screen handlers

## Renames Applied

### Save/Load System (0x410CA0-0x411704)

| Address | Old Name | New Name | Rationale |
|---------|----------|----------|-----------|
| 0x410CA0 | FUN_00410ca0 | Race_SetupGameMode | Configures game mode globals (DAT_0049635c) for race types 1-9, sets AI/lap/progression flags |
| 0x411120 | FUN_00411120 | SaveData_PackToBuffer | Packs game state (track, car, progress, settings) into save buffer at DAT_00490bac with CRC32 checksum |
| 0x4112C0 | FUN_004112c0 | SaveData_UnpackFromBuffer | Unpacks save buffer back into game state globals, verifies CRC32 before restoring |
| 0x4114F0 | FUN_004114f0 | SaveData_WriteToFile | XOR-encrypts save buffer with "Steve Snake says - No Cheating!" key, writes to CupData.td5 |
| 0x411590 | FUN_00411590 | SaveData_ReadFromFile | Reads CupData.td5, XOR-decrypts, calls SaveData_UnpackFromBuffer to restore state |
| 0x411630 | FUN_00411630 | SaveData_VerifyChecksum | Reads CupData.td5, decrypts, verifies CRC32 checksum matches expected value |

### Bar Fade / Screen Transition System (0x411710-0x411DDE)

| Address | Old Name | New Name | Rationale |
|---------|----------|----------|-----------|
| 0x411710 | FUN_00411710 | BuildFadeLookupTable | Builds 1KB lookup table at DAT_00494bc0 for 5-bit color channel fade interpolation |
| 0x411750 | FUN_00411750 | BarFade_SetTargetColor | Sets target fade color (shifts RGB888 to RGB555), resets fade counter |
| 0x411780 | FUN_00411780 | BarFade_ToColor | Progressive bar-wipe fade to solid color using lookup table, 16-bit surface operations |
| 0x411A50 | FUN_00411a50 | BarFade_ToBlack | Sets up fade-to-black (target=0), resets fade counter |
| 0x411A70 | FUN_00411a70 | BarFade_ToImage | Progressive bar-wipe crossfade between front and back buffers using lookup table |

### Surface/Texture Management (0x411DE0-0x412D50)

| Address | Old Name | New Name | Rationale |
|---------|----------|----------|-----------|
| 0x411DE0 | FUN_00411de0 | SurfaceTable_ClearAll | Zeros out the surface tracking table (DAT_00494fd0, 256 entries) |
| 0x411E00 | FUN_00411e00 | SurfaceTable_GetId | Looks up a surface pointer in the table, returns its generation ID |
| 0x411E30 | FUN_00411e30 | SurfaceTable_Release | Finds surface in table, restores it (IDirectDrawSurface::Restore), releases via Release(), zeros entry |
| 0x411E90 | FUN_00411e90 | SurfaceTable_ReleaseAll | Iterates all surface table entries, restores and releases each non-null surface |
| 0x411F00 | FUN_00411f00 | Surface_Create | Creates a DirectDraw offscreen surface (width, height), registers in surface table with generation ID |
| 0x412030 | FUN_00412030 | Texture_LoadTGA | Loads TGA from ZIP, converts via ImageProTGA, creates surface, copies pixel data -- returns surface ptr |
| 0x4122F0 | FUN_004122f0 | Texture_LoadTGA_NoTransparent | Same as Texture_LoadTGA but replaces pixel value 0 with 1 (avoids transparent holes) |
| 0x4125B0 | FUN_004125b0 | Texture_LoadTGA_ToFrontBuffer | Loads TGA directly into DAT_00496260 (front fade buffer) |
| 0x4127B0 | FUN_004127b0 | Texture_LoadTGA_ToBackBuffer | Loads TGA directly into DAT_00496264 (back fade buffer) |
| 0x4129B0 | FUN_004129b0 | Surface_InvertColors | Inverts all RGB channels of a 16-bit surface (0x1F - channel for each of R, G, B) |
| 0x412B00 | FUN_00412b00 | Surface_SetColorKey | Converts RGB888 to surface pixel format, calls IDirectDrawSurface::SetColorKey |
| 0x412B90 | FUN_00412b90 | Surface_DimWithColorKey | Dims surface pixels (halves each channel) except those matching a color key |

### Menu Font & UI Helpers (0x412D50-0x413568)

| Address | Old Name | New Name | Rationale |
|---------|----------|----------|-----------|
| 0x412D50 | FUN_00412d50 | MenuFont_RenderString | Renders text string using bitmap font glyphs (7x36 grid), character widths from DAT_004664f8 |
| 0x412E30 | FUN_00412e30 | GetMenuSprite | Gets menu text sprite by index -- either renders from string table or loads localized TGA (Japanese) |
| 0x413010 | FUN_00413010 | RenderTrackRecordsOverlay | Renders track records overlay: Name, Best, Car, Avg, Top headers + 5 record rows with times/speeds |

### Front-End Core (0x414610-0x414F34)

| Address | Old Name | New Name | Rationale |
|---------|----------|----------|-----------|
| 0x414610 | FUN_00414610 | NavigateToScreen | Resets screen state to 0, loads screen handler from dispatch table PTR_LAB_004655c4[param] |
| 0x414640 | FUN_00414640 | LoadFrontEndSounds | Loads 10 UI sound effects from Front_End/Sounds/Sounds.zip (ping, whoosh, crash, uh-oh) |
| 0x414740 | FUN_00414740 | FrontEnd_Init | Full front-end initialization: loads all textures, fonts, buttons, sets 640x480 mode, starts CD music |
| 0x414A90 | FUN_00414a90 | FrontEnd_Tick | App tick callback -- handles CD music rotation with genre-exclusion rules, polls input |
| 0x414B50 | FUN_00414b50 | FrontEnd_MainLoop | Main render/input loop: surface recovery, input polling, mouse cursor, cheat code detection, screen dispatch, flip |

### Marquee/Ticker Text (0x4183B0-0x418444)

| Address | Old Name | New Name | Rationale |
|---------|----------|----------|-----------|
| 0x4183B0 | FUN_004183b0 | Marquee_SetText | Parses null-delimited multiline text into line array (DAT_00496374), sets display params |
| 0x418410 | FUN_00418410 | Marquee_SetLine | Sets a specific line in the marquee text array by index |
| 0x418430 | FUN_00418430 | Marquee_Clear | Clears marquee: sets line count to 0, resets scroll position to -1 |

### Multiplayer Networking (0x418C60-0x41C320)

| Address | Old Name | New Name | Rationale |
|---------|----------|----------|-----------|
| 0x418C60 | FUN_00418c60 | NetMsg_EnqueueMessage | Enqueues a network message into 8-slot ring buffer (type, data, size), handles overflow with SNK_MsgBufferFull |
| 0x419B30 | FUN_00419b30 | MultiplayerLobby_RenderSessionList | Renders list of available multiplayer sessions (up to 4), shows "New Session" for empty slots |
| 0x41A530 | FUN_0041a530 | MultiplayerLobby_RenderChatInput | Renders chat text input box with cursor blink, plays sound on message send |
| 0x41A670 | FUN_0041a670 | MultiplayerLobby_RenderNameInput | Renders player name text input box (similar to chat but different position/surface) |
| 0x41B390 | FUN_0041b390 | Multiplayer_CreateSession | Creates new DXPlay session, initializes player slots, queues "session created" message, navigates to lobby |
| 0x41B420 | FUN_0041b420 | MultiplayerLobby_RenderPlayerList | Renders player list with names, status icons, host indicator, session/player headers |
| 0x41B610 | FUN_0041b610 | Multiplayer_ProcessNetworkTick | Core network message pump: receives/dispatches messages (player state 0x10, sync 0x11, kick 0x12, data 0x14, etc.), handles disconnects, periodic status broadcast |
| 0x41BD00 | FUN_0041bd00 | MultiplayerLobby_RenderChatHistory | Renders scrolling chat history (6 messages max), handles surface scrolling via pixel copy for overflow |
| 0x41C030 | FUN_0041c030 | Multiplayer_ParseChatCommands | Parses chat input for commands (*kick, *boot, *name) and emoticons (:) ;) %), converts to network protocol messages |

### Options/Post-Race (0x41D840-0x422465)

| Address | Old Name | New Name | Rationale |
|---------|----------|----------|-----------|
| 0x41D840 | FUN_0041d840 | FormatDisplayModeStrings | Formats display mode list as "%dx%dx%d" strings for graphics options |
| 0x421DA0 | FUN_00421da0 | UnlockTrackOrCar | Checks race completion flags, unlocks tracks/cars based on championship/game mode mapping |
| 0x421E90 | FUN_00421e90 | RenderPlayerStatsOverlay | Renders post-race player statistics: times, speeds (MPH/KPH), car names, lap records |

## Switch-Case Fragments (Not Renamed)

The ~64 remaining caseD_ fragments are compiler-generated pieces of their parent functions' switch statements. They are not independent functions but rather parts of the named state machines:

- **MainMenuHandler** (0x415490): ~20 case fragments for 24-state menu
- **ScreenChampionshipMenu** (0x417121): ~8 case fragments
- **ScreenSingleRaceMenu** (0x4179BF): ~6 case fragments
- **ScreenMultiplayerLobbyCore** (0x41C330): ~18 case fragments for lobby states
- **ScreenOptionsMenu** (0x41D890): ~6 case fragments
- **ScreenControlOptions** (0x41E70B): ~4 case fragments
- **ScreenSoundOptions** (0x41EA61): ~5 case fragments
- **ScreenGraphicsOptions** (0x41F954): ~6 case fragments
- **ScreenTwoPlayerOptions** (0x420C2A): ~6 case fragments
- **ScreenQuickRaceSetup** (0x4213A5): ~5 case fragments

These fragments exist because Ghidra splits large switch statements into separate "functions" when the compiler uses computed jumps. They should be left as-is since they are not true function boundaries.

## Key Patterns Discovered

### Save System Encryption
The save file (CupData.td5) uses XOR encryption with the key "Steve Snake says - No Cheating!" followed by CRC32 integrity verification. The save buffer (0x490BAC) contains packed race progress, car unlocks, settings, and championship state.

### Bar Fade System
A sophisticated screen transition system using a 1024-byte lookup table for 5-bit color interpolation. Supports fade-to-color, fade-to-black, and crossfade between two surfaces. The fade progresses in 2-scanline increments with a 64-line active band.

### Network Message Protocol
The multiplayer system uses a ring buffer for message queuing and supports message types including:
- 0x10: Player state update (4 bytes)
- 0x11: Host state sync (8 bytes, includes game settings)
- 0x12: Kick player
- 0x13: Request player data
- 0x14: Full player data exchange
- 0x15: Broadcast settings
- 0x16: Ready acknowledgement
- 0x17: Name change request
- 0x18: Player profile data (32 bytes)
- 0x7F: Chat/system message (variable length)

### Chat Command System
The chat supports slash-style commands prefixed with `*`:
- `*kick [player]` - Kick a player (sends message type 0x12)
- `*boot [player]` - Boot a player (sends message type 0x12)
- `*name [name]` - Change display name (sends message type 0x17)
- Emoticons: `:)` `:>` `:(` `:<` `:o` `;)` `;>` `%o` are converted to special character codes (0x1B-0x1F)
