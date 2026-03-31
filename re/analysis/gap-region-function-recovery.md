# Gap Region Function Recovery

Analysis of ~49KB of gap regions in TD5_d3d.exe where Ghidra failed to auto-create functions.
Session: `50589c8be1e74a2ca357bf035d019805` (read-write)

## Summary

- **Total functions recovered:** 139 (134 created by script + 5 pre-existing)
- **Functions renamed:** 43 (FUN_ prefix functions that needed naming)
- **Functions already named:** 96 (from previous analysis or switch-case fragments)
- **Gap regions processed:** 3 major regions

## Key Findings

These gap regions contain **frontend screen handler functions** implementing a state-machine pattern:
- Each screen is a function that switches on `DAT_00495204` (screen state counter)
- State 0 = initialization (load textures, create buttons)
- States 1-2 = animation/transition in
- State 3+ = interactive states (handle input, render)
- Final states = cleanup and transition out

The functions do NOT use standard `PUSH EBP; MOV EBP,ESP` prologues -- they use optimized calling conventions with `PUSH ECX`/`SUB ESP` or no frame pointer at all. This is why Ghidra's auto-analysis missed them.

## Gap Region 1: 0x414F40-0x4183AF (~13KB, Frontend/Main Menu)

| Address | Name | Description |
|---------|------|-------------|
| 0x414F40 | RenderPositionerGlyphStrip | (pre-existing) Renders positioned glyph strips for UI |
| 0x415030 | ScreenPositionerDebugTool | (pre-existing) Debug tool for screen positioning |
| 0x4150B8 | caseD_2 | Switch case fragment of ScreenPositionerDebugTool |
| 0x4150F4 | caseD_3 | Switch case fragment |
| 0x415370 | **ScreenOkDialogHandler** | OK dialog box with 4-state flow: create surface, render, animate, cleanup |
| 0x4153F5 | caseD_1 | Switch case fragment |
| 0x41542F | caseD_4 | Switch case fragment - cleanup, calls FUN_00411E30 to free surface |
| 0x415490 | MainMenuHandler | (pre-existing) Main menu screen - huge 24-case switch |
| 0x415976 | caseD_1 | Main menu case fragment |
| 0x415998 | caseD_3 | Main menu case fragment |
| 0x415AF9 | caseD_4 | Main menu case fragment |
| 0x415BA5 | caseD_1 | Main menu case fragment |
| 0x415C13 | caseD_3 | Main menu case fragment |
| 0x415C67 | caseD_4 | Main menu case fragment |
| 0x415D15 | caseD_5 | Main menu case fragment |
| 0x415D9D | caseD_6 | Main menu case fragment |
| 0x415E77 | **MainMenu_SetScreenState10** | Sets screen counter to state 10 (transition) |
| 0x415E8F | caseD_7 | Main menu case fragment |
| 0x415F0A | caseD_8 | Main menu case fragment |
| 0x415FBC | caseD_9 | Main menu case fragment |
| 0x41615D | caseD_3 | Main menu case fragment |
| 0x4161B2 | **MainMenu_RenderCarPreview** | Renders car preview sprite on main menu |
| 0x4161F2 | caseD_a | Main menu case fragment |
| 0x41630F | caseD_b | Main menu case fragment |
| 0x416390 | caseD_c | Main menu case fragment |
| 0x4164F5 | caseD_14 | Main menu case fragment |
| 0x416691 | caseD_16 | Main menu case fragment |
| 0x416731 | caseD_17 | Main menu case fragment |
| 0x416A81 | caseD_1 | Main menu case fragment |
| 0x416A94 | **MainMenu_AnimateIntro** | Animates button positions during menu intro (7 buttons) |
| 0x416BEB | caseD_2 | Main menu case fragment |
| 0x416C4B | caseD_3 | Main menu case fragment |
| 0x416CCA | **MainMenu_HandleButtonPress** | Handles main menu button selection, dispatches to sub-screens |
| 0x416D2A | caseD_6 | Main menu case fragment |
| 0x416D56 | caseD_2 | Main menu case fragment |
| 0x416D80 | caseD_4 | Main menu case fragment |
| 0x416F16 | caseD_5 | Main menu case fragment |
| 0x417121 | **ScreenChampionshipMenu** | Championship mode selection (Era, Challenge, Pitbull, Masters) |
| 0x4174CE | caseD_8 | Championship case fragment |
| 0x41754D | **ChampionshipMenu_HandleInput** | Processes input on championship menu |
| 0x417584 | **ChampionshipMenu_AnimateEntry** | Animates championship menu button entry |
| 0x4175BC | caseD_9 | Championship case fragment |
| 0x417700 | caseD_a | Championship case fragment |
| 0x4177C7 | **ChampionshipMenu_AnimateButtons** | Championship menu button animation |
| 0x417918 | caseD_b | Championship case fragment |
| 0x4179BF | **ScreenSingleRaceMenu** | Single Race mode selection (Single Race, Cup Race, Cont Cup, Time Trials) |
| 0x417CBA | caseD_14 | Single race case fragment |
| 0x417DAC | caseD_1 | Single race case fragment |
| 0x41807F | caseD_2 | Single race case fragment |
| 0x41808D | caseD_6 | Single race case fragment |
| 0x4180B3 | caseD_7 | Single race case fragment |
| 0x4182A1 | **ScreenCreditsRoll** | Credits screen with scrolling text |
| 0x418380 | **CreditsRoll_Cleanup** | Credits cleanup stub |
| 0x41838D | **CreditsRoll_Finalize** | Credits finalization stub |

## Gap Region 2: 0x41C320-0x41D840 (~5KB, Multiplayer Lobby)

| Address | Name | Description |
|---------|------|-------------|
| 0x41C320 | **MultiplayerLobby_RetStub** | Single RET instruction (stub/alignment) |
| 0x41C330 | **ScreenMultiplayerLobbyCore** | Main multiplayer lobby handler - 18 states, DXPlay calls |
| 0x41C3ED | **MultiplayerLobby_InitUI** | Initializes lobby UI: buttons (ChangeCar, Start, Exit), message window, status |
| 0x41C564 | **MultiplayerLobby_HandleChat** | Handles chat input in lobby |
| 0x41C58A | caseD_1 | Lobby case fragment |
| 0x41C77D | caseD_3 | Lobby case fragment - main interactive state |
| 0x41C7F6 | **MultiplayerLobby_UpdateButtons** | Updates button states based on network status |
| 0x41C8E9 | **MultiplayerLobby_WaitForHost** | Displays "Wait for host" message, uses SNK_WaitForHostMsg |
| 0x41CA29 | caseD_4 | Lobby case fragment - text input handling |
| 0x41CAC4 | caseD_5 | Lobby case fragment - player ready check |
| 0x41CB50 | **MultiplayerLobby_RenderError** | Renders network error messages (NetErrString1-4) |
| 0x41CB83 | caseD_6 | Lobby case fragment |
| 0x41CD64 | caseD_7 | Lobby case fragment - shows Yes/No/Ok buttons |
| 0x41CDFE | **MultiplayerLobby_ShowYesNo** | Shows Yes/No confirmation dialog |
| 0x41CE4F | caseD_8 | Lobby case fragment - input handling for dialog |
| 0x41CF41 | **MultiplayerLobby_ProcessNetEvents** | Processes network events, handles disconnects |
| 0x41CF81 | **MultiplayerLobby_CleanupDisconnect** | Cleanup after network disconnect |
| 0x41CF9B | caseD_9 | Lobby case fragment |
| 0x41D00D | **MultiplayerLobby_SyncPlayers** | Syncs player data, seals session, sends messages |
| 0x41D0A1 | **MultiplayerLobby_WaitSync** | Waits for all players to sync (timeGetTime polling) |
| 0x41D0C2 | caseD_a | Lobby case fragment |
| 0x41D195 | caseD_c | Lobby case fragment - session seal and player data |
| 0x41D327 | caseD_d | Lobby case fragment - timed sync polling |
| 0x41D3D5 | **MultiplayerLobby_SendPlayerData** | Broadcasts player data to all peers |
| 0x41D458 | caseD_f | Lobby case fragment |
| 0x41D50E | **MultiplayerLobby_BroadcastState** | Broadcasts game state to network |
| 0x41D540 | caseD_10 | Lobby case fragment |
| 0x41D5B1 | caseD_11 | Lobby case fragment - ReceiveMessage loop |
| 0x41D5E6 | **MultiplayerLobby_WaitReady** | Waits for ready confirmation |
| 0x41D746 | caseD_1 | Lobby case fragment |
| 0x41D763 | caseD_4 | Lobby case fragment |
| 0x41D7D2 | caseD_5 | Lobby case fragment |
| 0x41D840 | **FormatDisplayModeString** | Formats "%dx%dx%d" display mode strings |

## Gap Region 3: 0x41D890-0x421D9F (~18KB, Options/Sub-screens)

| Address | Name | Description |
|---------|------|-------------|
| 0x41D890 | **ScreenOptionsMenu** | Options menu (Game, Control, Sound, Graphics, TwoPlayer, Ok) |
| 0x41D9DD | caseD_1 | Options case fragment |
| 0x41D9FA | caseD_3 | Options case fragment - button positioning |
| 0x41DB38 | caseD_4 | Options case fragment |
| 0x41DB78 | caseD_6 | Options case fragment - dispatches to sub-options |
| 0x41DC14 | caseD_1 | Options case fragment |
| 0x41DD6B | caseD_8 | Options case fragment - animate out |
| 0x41DECF | **OptionsMenu_GoToScreen** | Wrapper that calls FUN_00414610 to navigate to target screen |
| 0x41E185 | caseD_1 | Control options case fragment |
| 0x41E1AA | caseD_3 | Control options case fragment |
| 0x41E340 | caseD_4 | Control options case fragment |
| 0x41E51E | caseD_6 | Control options case fragment |
| 0x41E70B | **ScreenControlOptions** | Control/input options screen handler |
| 0x41EA4D | **ControlOptions_Cleanup** | Control options cleanup |
| 0x41EA61 | **ScreenSoundOptions** | Sound options (SFX Mode, SFX Volume, Music Volume, Music Test, Ok) |
| 0x41EC51 | caseD_1 | Sound options case fragment |
| 0x41EC71 | caseD_3 | Sound options case fragment - volume bars, controllers icon |
| 0x41EF19 | caseD_4 | Sound options case fragment - renders volume bars |
| 0x41F108 | caseD_6 | Sound options case fragment - handles volume changes, DXSound calls |
| 0x41F413 | **SoundOptions_HandleInput** | Processes SFX/Music volume input, calls DXSound::SetVolume/CDSetVolume |
| 0x41F620 | caseD_8 | Sound options case fragment - animate out |
| 0x41F954 | **ScreenGraphicsOptions** | Graphics options screen handler |
| 0x41FB85 | caseD_1 | Graphics options case fragment |
| 0x41FBA3 | caseD_3 | Graphics options case fragment |
| 0x41FD1B | caseD_4 | Graphics options case fragment |
| 0x41FF79 | caseD_6 | Graphics options case fragment |
| 0x420113 | caseD_7 | Graphics options case fragment |
| 0x4201CE | caseD_8 | Graphics options case fragment |
| 0x4203AA | **GraphicsOptions_HandleInput** | Processes graphics settings input |
| 0x4205A0 | caseD_1 | Two player options case fragment |
| 0x4205BE | caseD_3 | Two player options case fragment |
| 0x4206DF | caseD_4 | Two player options case fragment |
| 0x420884 | caseD_6 | Two player options case fragment |
| 0x4209FF | caseD_7 | Two player options case fragment |
| 0x420ABA | caseD_8 | Two player options case fragment |
| 0x420C2A | **ScreenTwoPlayerOptions** | Two-player split-screen options screen |
| 0x420DBA | caseD_1 | Quick race case fragment |
| 0x420DD9 | caseD_3 | Quick race case fragment |
| 0x420EEB | caseD_4 | Quick race case fragment - car/track selection with locked check |
| 0x421020 | caseD_6 | Quick race case fragment |
| 0x421159 | caseD_7 | Quick race case fragment |
| 0x421232 | caseD_8 | Quick race case fragment |
| 0x421397 | **TwoPlayerOptions_HandleInput** | Processes two-player options input |
| 0x4213A5 | **ScreenQuickRaceSetup** | Quick Race setup (car select + track select with lock checks) |
| 0x4216D0 | caseD_1 | Quick race setup case fragment |
| 0x4216F5 | caseD_3 | Quick race setup case fragment |
| 0x421827 | caseD_4 | Quick race setup case fragment |
| 0x421AE0 | **QuickRaceSetup_HandleInput** | Quick race setup input processing |
| 0x421B19 | **QuickRaceSetup_Cleanup** | Quick race setup cleanup |
| 0x421C15 | caseD_6 | Quick race setup case fragment |
| 0x421D63 | **QuickRaceSetup_Finalize** | Quick race setup finalization |

## Common Patterns Identified

### Screen State Machine
All screen handlers follow the same pattern:
```c
switch(DAT_00495204) {  // screen state
  case 0: // Init - load textures, create buttons
  case 1: // Fade in / transition
  case 2: // Fade in continued
  case 3: // Main interactive state
  case 4+: // Sub-states, cleanup, transition out
}
```

### Key Globals
- `DAT_00495204` - Screen state counter
- `DAT_0049522c` - Animation frame counter (incremented each frame)
- `DAT_00495228` - Screen width
- `DAT_00495200` - Screen height
- `DAT_004951e8` - Input ready flag
- `DAT_00495240` - Selected button index
- `DAT_00496358` - Current background sprite handle
- `DAT_0049628c` - Current overlay sprite handle
- `DAT_004962cc` - Scroll offset

### Key Helper Functions Called
- `FUN_00412E30` - Load screen background texture
- `FUN_004125B0` - Load texture from ZIP archive (Front_End/FrontEnd.zip)
- `FUN_00411F00` - Allocate sprite surface
- `FUN_00424050` - Clear/fill sprite surface
- `FUN_00425660` - Render sprite at position (x, y, srcX, srcY, w, h, flags, surface)
- `FUN_00425DE0` - Create button (text, x, y, w, h, flags)
- `FUN_004259D0` - Position/animate button (index, x, y)
- `FUN_004258C0` - Finalize button setup
- `FUN_004258E0` - Begin button interaction mode
- `FUN_00424AF0` - Screen fade transition
- `FUN_00426390` - Cleanup screen resources
- `FUN_00411E30` - Free sprite surface
- `FUN_00414610` - Navigate to screen by index
- `FUN_00414A90` - Start game/race
- `FUN_004183B0` - Set marquee text
- `FUN_00418430` - Clear marquee text

### Multiplayer-Specific
- `DXPlay::SendMessageA` - Send network message
- `DXPlay::ReceiveMessage` - Receive network message
- `DXPlay::SealSession` - Seal/unseal multiplayer session
- `DXPlay::Destroy` - Destroy multiplayer session
- `FUN_00418C60` - Format/encode network chat message
- `FUN_0041B610` - Process network tick
- `FUN_0041BD00` - Update lobby player list
- `FUN_0041B420` - Poll network input
- `FUN_0041A670` - Network status update

### Sound Options
- `DXSound::Play` - Play sound effect
- `DXSound::SetVolume` - Set SFX volume
- `DXSound::CDSetVolume` - Set music volume
- `DXSound::SetPlayback` - Set SFX playback mode
- `DXSound::CanDo3D` - Check 3D audio support
