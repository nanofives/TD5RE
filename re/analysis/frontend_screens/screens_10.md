# Frontend screens 10–14 — complete per-screen element + behavior spec

RE target: `TD5_d3d.exe` (image base `0x00400000`). All addresses Ghidra VAs. Literal
decompilation only; each claim carries its address. Hex AND decimal where ambiguous.

Cross-cutting facts that apply to ALL 5 screens (so the per-screen ELEMENTS tables stay short):

- **Every menu button is INLINE-built but FLUSH-drawn.** `CreateFrontendDisplayModeButton`
  (`0x00425de0`) bakes a tracked surface (frame from `DAT_00496268` ButtonBits.tga + label text
  via `DrawFrontendLocalizedStringToSurface` 24×24 / body 12×12) and adds a slot to
  `g_connBrowserListOriginX_PROVISIONAL[]` (`0x00499c78`, stride 0x34). The pixels reach screen via
  the per-frame `RenderFrontendUiRects` (`0x00425a30`) → `QueueFrontendSpriteBlit` → flush
  sprite-loop (`FlushFrontendSpriteBlits 0x00425540`). Button negative X (e.g. `-0x130`) = "auto-lay
  out, width follows"; the slide-in/out states reposition slots each frame with `MoveFrontendSpriteRect`.
- **Header title** = a baked label surface `g_currentScreenIndex` (misnamed; it is the header-label
  surface ptr) from `CreateMenuStringLabelSurface(N)` (`0x00412e30`): N selects `SNK_MenuStrings[N]`;
  N=5 for the net screens, N=6 for the options screens. It is re-queued every frame at an animated X
  via `QueueFrontendOverlayRect` (FLUSH-drawn). `g_menuHeaderLabelSurfaceWidth/Height/YOffset` set by
  the same fn.
- **Selection / hover highlight** = `RenderFrontendDisplayModeHighlight 0x004263e0` (DECOUPLED, runs
  inside flush): 4 edge bars color `0xc000` around the selected slot, gated on
  `g_frontendOverlayRectArrayTail != -1 && g_frontendCursorOverlayHidden==0`.
- **Mouse cursor sprite** queued by LOOP when `g_frontendCursorOverlayHidden==0 &&
  g_frontendMouseCursorEnabled==1` (snkmouse, `g_frontendCursorTextureId`). FLUSH-drawn.
- **Slide animation** is frame-count driven: position = `base ± g_frontendAnimFrameCounter*step`;
  transition fires on exact equality (mostly `0x20`=32 in/out, `0x27`=39 options slide-in,
  `0x10`=16 options slide-out, `0x18`=24 lobby dialog).
- **g_returnToScreenIndex sentinel**: `~TD5_SCREEN_LOCALIZATION_INIT` = `~0` = `0xFFFFFFFF` (-1) is
  the "entered from the in-race PAUSE options" path → exit via `InitializeFrontendDisplayModeState()`
  (resume race) instead of `SetFrontendScreen()`. (`TD5_SCREEN_LOCALIZATION_INIT`=0 = idx0.)
- Screen enum equates seen: MAIN_MENU=5, RACE_TYPE=6, SESSION_PICKER=9, OPTIONS_HUB=12,
  GAME_OPTIONS=13, CONTROL_OPTIONS=14, SOUND_OPTIONS=15, DISPLAY_OPTIONS=16,
  TWO_PLAYER_OPTIONS=17, CONTROLLER_BINDING=18, CAR_SELECTION=20, SESSION_LOCKED=29.

---

### Screen 10 @0x0041a7b0 — RunFrontendCreateSessionFlow  [interactive Y]
Net session create/join name-entry flow (DXPTYPE arch-divergent — DirectPlay).

Inner states (`switch(g_frontendInnerState)`): TWO parallel name-entry sub-flows selected by
`g_networkLobbyEntryPhase` (set 1 by lobby/picker, read at state 0):
- **0** init "enter session name" (host path): if `g_networkLobbyEntryPhase==1` re-bakes header
  `CreateMenuStringLabelSurface(5)`; if `g_connBrowserCursorIndex != 0` jumps directly to state
  **0x10** (player-name path) else builds session-name buttons. Edit target = `&DAT_00497068`
  (session-name buf), max len 0x10.
- **1** slide-in (counter==0x20 → play snd4, clear entryPhase, Deactivate cursor, ++state).
- **2** interactive: `RenderFrontendCreateSessionNameInput()`; on edit-commit (`g_postRaceNameEditState==2`)
  copies the typed name (or, if empty `iVar6==-2`, falls back to `&g_localComputerName`) into
  `&DAT_00497068`, sets `g_returnToScreenIndex=0`, ++state. Back button (idx1) → state 3 with
  `g_returnToScreenIndex=SESSION_PICKER(9)`.
- **3** slide-out → release buttons/surface, restore small-text colorkeys, → state 4 (if returnIdx!=0)
  via LAB_0041b1f3, else state 8.
- **4** init "enter player name" buttons (`SNK_EnterPlayerNameButTxt`); target = `&g_postRacePlayerNameEntryBuffer`.
- **5** slide-in (counter==0x20 → snd4, Deactivate, ++state).
- **6** interactive player-name edit; commit copies name (computer-name fallback). Back(idx1) →
  returnIdx=SESSION_PICKER, ++state.
- **7** slide-out → release, if returnIdx!=0 reset to state 0 else ++state.
- **8** **JoinSession bridge / car-select handoff**: release header surface, set
  `g_networkLobbyEntryPhase=1`, clear flags, `g_frontendSelectedGameType=0`,
  `SetFrontendScreen(CAR_SELECTION=20)`.
- **0x10/0x11/0x12/0x13** = mirror of 4/5/6/7 but the "join" variant (entered when
  g_connBrowserCursorIndex!=0 at state 0); state 0x13 exit does `SetFrontendScreen(g_returnToScreenIndex)`.
- **0x14** `DXPlay::JoinSession`: init `g_lobbySlotStatusTable[6]=0x7f`, clear host/status; on fail
  → `SetFrontendScreen(SESSION_LOCKED=29)`; on success seed `g_randomSeedForRace`, ++state.
- **0x15** build the `*SeshJoinMsg` packet (player name into `g_lobbyNetMessageDispatchFlag`),
  `QueueFrontendNetworkMessage(1,…)`, `DXPlay::SendMessageA(1,…)`, `SetFrontendScreen(NETWORK_LOBBY=11)`.

ELEMENTS:
| element | produced by (fn/global/asset) | INLINE/FLUSH | dest pos | gating | notes |
|---|---|---|---|---|---|
| Header label "Multiplayer" | CreateMenuStringLabelSurface(5)→g_currentScreenIndex | INLINE bake; FLUSH draw (QueueFrontendOverlayRect every state) | (cx-200, cy-0x9f-hdrYoff-0x40) static / animated X in slide states | always | re-baked at state0 only if entryPhase==1 |
| "Enter New Session Name" button (big) | CreateFrontendDisplayModeButton(SNK_EnterNewSessionNameButTxt,-0x1c0,0,0x1c0,0x40) → slot0, g_postRaceNameButtonSurfacePtr | INLINE bake; FLUSH draw | slot0, slide via MoveFrontendSpriteRect | state 0 | host path. Doubles as the text-input panel surface |
| "Enter Player Name" button (big) | CreateFrontendDisplayModeButton(SNK_EnterPlayerNameButTxt,-0x1c0,0,0x1c0,0x40) | INLINE bake; FLUSH | slot0 | states 4/0x10 | join path |
| Back button | CreateFrontendDisplayModeButton(SNK_BackButTxt,-0x70,0,0x70,0x20) → slot1 | INLINE bake; FLUSH | slot1 | states 0/4/0x10 | ESC target (g_frontendEscKeyButtonIndex=1) |
| Text-input field + typed string | RenderFrontendCreateSessionNameInput 0x0041a530: BltColorFillToSurface fill + DrawFrontendClippedStringToSurface(g_postRaceNameEditTargetPtr) onto button surface | INLINE bake into slot0 surface; FLUSH draw | inside slot0 (x=0x14,w=0x198,h=0x10) | states 2/6/0x12 | fill color 0x392152 idle / 0 when redraw-dirty bit set |
| Blinking text caret | RenderFrontendCreateSessionNameInput: BltColorFillToSurface(0xff00, caretX, …,2,0x10) | INLINE bake; FLUSH | after measured string width | (g_frontendAnimFrameCounter&0x20)==0 && editState!=2 && focused(buttonIndex==0) | green 2px caret; suppressed if width>0x195 |
| Mouse cursor | LOOP QueueFrontendOverlayRect(cursorTexId) | FLUSH | mouse pos | cursorOverlayHidden==0 | Activated states 0(host)/4/0x10; Deactivated in slide states |
| Selection highlight bars | RenderFrontendDisplayModeHighlight 0x004263e0 | FLUSH (decoupled) | around selected slot | tail!=-1 && !hidden | |

Primed contract globals: `g_postRaceNameEditTargetPtr` (edit buffer), `_g_postRaceNameEditMaxLength`
(0x10), `g_postRaceNameEditState` (1=editing,2=committed), `g_frontendEscKeyButtonIndex=1`,
`g_currentScreenIndex` (header surf), `g_postRaceNameButtonSurfacePtr_PROVISIONAL` (input-panel surf),
`g_networkLobbyEntryPhase` (which sub-flow), `g_connBrowserCursorIndex` (host vs join),
`g_returnToScreenIndex`, `g_connBrowserRedrawDirty` (caret/fill toggle), `g_randomSeedForRace`.

Animation: slide-in states 1/5/0x11 slot0 X = `iVar8 + counter*-0x18 + 0x30a`, slot1 X =
`(cx-0x32a)+counter*0x18`, commit at counter==0x20. Slide-out states 3/7/0x13 slot0 X =
`iVar8 + counter*-0x20 + 10`, slot1 X = `counter*0x20 + 0xa8 + iVar8`, commit at counter==0x20.
Caret blink = `g_frontendAnimFrameCounter & 0x20`.

Conditional elements: name-entry sub-flow chosen by `g_networkLobbyEntryPhase` /
`g_connBrowserCursorIndex`; empty-name → computer-name fallback (`g_localComputerName`); JoinSession
failure → SESSION_LOCKED dialog; the whole screen is DXPTYPE/DirectPlay (ARCH-DIVERGENCE — port
mirrors at td5_frontend.c:10322).

Input dispatch: DXInput text capture in RenderFrontendCreateSessionNameInput
(`DXInput::SetAnsi(1)/GetString` when buttonIndex==0, else SetAnsi(0)); commit detected via
`g_postRaceNameEditState==2`; Back = `g_frontendButtonIndex==1 && g_frontendButtonPressedFlag`;
ESC mapped to idx1.

Confidence: [CONFIRMED @ 0x0041a7b0 body; helper 0x0041a530; CreateMenuStringLabelSurface 0x00412e30].
[UNCERTAIN: SNK_MenuStrings[5] literal text not dumped — "Multiplayer"/"Network" by context, not asserted.]

---

### Screen 11 @0x0041c330 — RunFrontendNetworkLobby  [interactive Y]
Network lobby (chat + player roster + status + host/client start handshake). DXPTYPE arch-divergent.
Heaviest of the 5; almost all panel pixels are FLUSH-drawn from tracked surfaces baked by 3 helper
fns each interactive frame.

Inner states: 0 init; 1 slide-in (commit counter==0x14); 2 prime chat-edit / set ESC→5; 3 main
interactive (chat + button handling: ChangeCar idx3, Start idx4, Exit idx5); 4 send-chat then →2;
5 host "all ready?" tally → 0xc or ++; 6 slide-in error/confirm dialog (commit==0x18); 7 build
dialog buttons (Yes/No or OK) via preview-layout; 8 dialog interactive; 9 dialog teardown / route;
10 dialog slide-out → 2; 0xc host SealSession + send role-assign msgs (0x12); 0xd wait config receipts
(0x13 resend, timeout 0xfa ms); 0xe seed schedule (`InitializeRaceSeriesSchedule`); 0xf push race
settings (msg 0x15, 0x80 bytes, 0xa5 ms beat); 0x10 final go (msg type 4 at counter==8) →
`InitializeFrontendDisplayModeState` (start race); 0x11 client wait for type-4 go msg via
`DXPlay::ReceiveMessage` → `InitializeFrontendDisplayModeState`.

ELEMENTS:
| element | produced by (fn/global/asset) | INLINE/FLUSH | dest pos | gating | notes |
|---|---|---|---|---|---|
| Header label "Multiplayer" | CreateMenuStringLabelSurface(5)→g_currentScreenIndex | INLINE bake; FLUSH | (cx-200,…) animated | state 0 | |
| MainMenu.tga full-screen bg | LoadTgaToFrontendSurface16bpp(MainMenu.tga) + CopyPrimaryFrontendBufferToSecondary | INLINE (decode into work surf) | full 640×480 | state 0 | cached for restore |
| Chat input strip (decoration) | CreateFrontendDisplayModeButton(NULL label,-0x1d0,0,0x1d0,0x18)→g_lobbyDecorationStripSurface (slot0) | INLINE bake; FLUSH | slot0 | state 0 | label-less strip; chat input drawn into it |
| **Message/chat window panel** | CreateFrontendDisplayModeButton(SNK_MessageWindowButTxt,-0x200,0,0x200,0x80)→g_postRaceNameButtonSurfacePtr (slot1) | INLINE bake; FLUSH | slot1 | state 0 | the big 0x200×0x80 chat-history surface |
| **Status / player-list panel** | CreateFrontendDisplayModeButton(SNK_StatusButTxt,-0xe0,0,0xe0,0x86)→g_lobbyPlayerRosterSurface (slot2) | INLINE bake; FLUSH | slot2 | state 0 | roster baked by RenderFrontendLobbyStatusPanel |
| "Change Car" button | CreateFrontendDisplayModeButton(SNK_ChangeCarButTxt,-200,0,200,0x20) slot3 | INLINE bake; FLUSH | slot3 | state 0 | idx3 |
| "Start" button | CreateFrontendDisplayModeButton(SNK_StartButTxt,-200,0,0x78,0x20) slot4 | INLINE bake; FLUSH | slot4 | state 0 | idx4 (host start) |
| "Exit" button | CreateFrontendDisplayModeButton(SNK_ExitButTxt,-200,0,0x78,0x20) slot5 | INLINE bake; FLUSH | slot5 | state 0 | idx5; ESC target (set 5 at state 2) |
| Session header bar (Session:/Player: + names) | g_lobbySessionHeaderSurface=CreateTrackedFrontendSurface(0x1e0,0x20); baked by RenderFrontendLobbyStatusPanel | INLINE bake; FLUSH (QueueFrontendOverlayRect every state) | (cx-0xb0, cy-0x97), 0x1e0×0x20 | state 0+ | SNK_TxtSession/SNK_TxtPlayer + host name + "(host)" SNK_TxtBhostB |
| **Chat history lines** | RenderFrontendLobbyChatPanel 0x0041bd00: per-msg DrawFrontendClippedStringToSurface(name 0x96w) + DrawFrontendWrappedStringLine onto chat panel | INLINE bake into slot1; FLUSH | inside slot1 (x0x18,y0x1a step 0x10) | state 3, when g_lobbyChatMessageRingCursor!=0 | up to 6 rows; scrolls via surface Lock+memmove (vtable+0x64/+0x80); emote token glyphs |
| **Player roster rows + status** | RenderFrontendLobbyStatusPanel 0x0041b420: per-slot DrawFrontendClippedStringToSurface(name 0x96w) + status text SNK_NetPlayStatMsg[status*10] onto roster surf | INLINE bake into slot2; FLUSH | inside slot2 (name x0x10, status x0xae, y step 0x10) | states 3/8 | local player drawn in smalltextB (highlight); fill clear each pass |
| **Chat text-input field + caret** | RenderFrontendLobbyChatInput 0x0041a670 onto g_lobbyDecorationStripSurface | INLINE bake; FLUSH | inside slot0 (x0x18,w0x1a0) | state 3 | green caret same as scr10; max len 0x3c |
| Error/confirm dialog box | g_lobbyErrorDialogSurface=CreateTrackedFrontendSurface(0x1e2,0x40); SNK_NetErrString1-4 centered | INLINE bake; FLUSH | (cx-0xba, cy-0x5f), 0x1e2×0x40 | states 6-10 | text chosen by g_returnToScreenIndex (err code) |
| Yes/No or OK dialog buttons | CreateFrontendDisplayModeButton(SNK_Yes/No/OkButTxt) under BeginFrontendDisplayModePreviewLayout | INLINE bake; FLUSH | centered | state 7 | preview-layout snapshot/restore |
| Mouse cursor + highlight bars | LOOP + RenderFrontendDisplayModeHighlight | FLUSH | — | cursorOverlayHidden==0 | |

Primed contract globals: `g_currentScreenIndex` (header), `g_postRaceNameButtonSurfacePtr` (chat
window), `g_lobbyDecorationStripSurface` (chat input strip), `g_lobbyPlayerRosterSurface` (status),
`g_lobbySessionHeaderSurface`, `g_lobbyErrorDialogSurface` (dialog), `g_lobbySlotStatusTable[6]`,
`g_lobbyRoleAcceptedTable[6]`, `g_lobbyConfigReceiptTable[6]`, `g_lobbyPlayerStatus` (0 idle/1 wait/2
ready/3 launch), `g_lobbyChatMessageRingBuffer`/`g_lobbyChatMessageRingCursor`/`g_lobbyChatInputCursor`,
`g_chatTokenizerCharClass` (chat edit buf), `g_postRaceNameEditState`, `g_frontendEscKeyButtonIndex=5`,
`g_lobbyAbortRequestLatch`, `g_mainMenuFlowPhase`, `g_networkSessionActive`, `g_lobbyHeartbeatTimestamp`,
`g_dxpdataRaceSettings*`, dpu_exref slot mirror fields (+0xbe4 host slot, +0xbe8 local slot, +0xbcc
slot-active table, +0xa64 name table).

Animation: state1 panels slide in (slot0 X = (cx-0x1a0)+counter*0xc; slots1-5 Y ramps), commit
counter==0x14(20). State6 dialog slide-in commit==0x18(24). State10 dialog slide-out commit==0x18.
Chat caret blink = counter&0x20.

Conditional elements: error dialog text 1/2 vs 3/4 by `g_returnToScreenIndex` (net error code);
Yes/No vs single OK by `g_returnToScreenIndex!=1 && !=2`; "(host)" suffix only when local==host slot;
local-player roster row highlighted (smalltextB); host-only states 0xc-0x10 vs client-only 0x11;
SealSession path. `g_lobbyAbortRequestLatch` at multiple states → tear down to SESSION_LOCKED.
Emote-token glyph substitution in chat (`:)`→0x1b, `:(`→0x1c, etc.) by NormalizeFrontendChatTokens.

Input dispatch: chat text via RenderFrontendLobbyChatInput DXInput::GetString (focus buttonIndex==0);
button remap at state 3 (idx2→3 ChangeCar, idx1→0); `g_frontendButtonPressedFlag` + index drives
ChangeCar(3)→CAR_SELECTION, Start(4)→host ready broadcast / state5, Exit(5)→quit-confirm dialog
(state6). Chat send: editState==2 → state4 → NormalizeFrontendChatTokens → QueueFrontendNetworkMessage(2)
+ SendMessageA(2). `*CHATCMD` slash-commands (`*team`,`*all`,`*kick`,`*ban` per the 4 _strncmp at
0x465fc4/cc/d4/dc) tokenized in NormalizeFrontendChatTokens 0x0041c030.

Confidence: [CONFIRMED @ 0x0041c330 body; helpers 0x0041bd00 chat-panel, 0x0041b420 status-panel,
0x0041a670 chat-input, 0x0041c030 tokenizer]. [UNCERTAIN: exact SNK_NetErrString / SNK_NetPlayStatMsg
literal text not dumped (LANGUAGE.DLL imports); the 4 chat slash-command literals at 0x465fc4–0x465fdc
read as 4-char compares but exact strings not dumped.]

---

### Screen 12 @0x0041d890 — ScreenOptionsHub  [interactive Y]
Options menu hub: 5 sub-screen entries + OK. No value displays / no arrows (it is a plain navigation
menu, not a settings list). NOTE: the LOOP runs the cheat-code FSM only while this screen is active
(`g_currentScreenFnPtr == PTR_ScreenOptionsHub`).

Inner states: 0 init (build 6 buttons, header(6), inline-string table SNK_Options_MT); 1/2
present+settle; 3 slide-in (commit counter==0x27 then settle via AdvanceFrontendTickAndCheckReady);
4/5 static double-present; 6 interactive (button dispatch); 7 slide-out prep; 8 slide-out → exit.

ELEMENTS:
| element | produced by (fn/global/asset) | INLINE/FLUSH | dest pos | gating | notes |
|---|---|---|---|---|---|
| Header "Options" | CreateMenuStringLabelSurface(6)→g_currentScreenIndex | INLINE bake; FLUSH | (cx-200,…) animated | state 0 | |
| MainMenu.tga bg | LoadTgaToFrontendSurface16bpp + CopyPrimaryFrontendBufferToSecondary | INLINE | full | state 0 | |
| "Game Options" button | CreateFrontendDisplayModeButton(SNK_GameOptionsButTxt,-0x130,0,0x130,0x20) slot0 | INLINE bake; FLUSH | slot0 | state 0 | idx0 → GAME_OPTIONS(13) |
| "Control Options" button | …(SNK_ControlOptionsButTxt,…) slot1 | INLINE bake; FLUSH | slot1 | state 0 | idx1 → CONTROL_OPTIONS(14) |
| "Sound Options" button | …(SNK_SoundOptionsButTxt,…) slot2 | INLINE bake; FLUSH | slot2 | state 0 | idx2 → SOUND_OPTIONS(15) |
| "Graphics Options" button | …(SNK_GraphicsOptionsButTxt,…) slot3 | INLINE bake; FLUSH | slot3 | state 0 | idx3 → DISPLAY_OPTIONS(16) |
| "Two Player Options" button | …(SNK_TwoPlayerOptionsButTxt,…) slot4 | INLINE bake; FLUSH | slot4 | state 0 | idx4 → TWO_PLAYER_OPTIONS(17) |
| "OK" button | …(SNK_OkButTxt,-0x130,0,0x60,0x20) slot5 | INLINE bake; FLUSH | slot5 | state 0 | idx5 → MAIN_MENU; ESC target (idx5) |
| Mouse cursor + highlight bars | LOOP + RenderFrontendDisplayModeHighlight | FLUSH | — | cursorOverlayHidden==0 | |

Primed contract globals: `g_currentScreenIndex`, `g_frontendEscKeyButtonIndex=5`, inline-string table
`SNK_Options_MT`, `g_returnToScreenIndex` (set per sub-screen), and at OK(idx5) it COMMITS the
in-race shadow→live copies: `g_cameraMode=g3dCollisionsConfigShadow^1`, particle-pool fields ←
`gDynamicsConfigShadow`/`g_specialEncounterUnlockB`, `gRaceDifficultyTier=g_raceDifficultyTier`,
`_g_specialEncounterType=g_specialEncounterUnlockA`. (This is the apply-options-on-exit-to-race path.)

Animation: state3 slide-in (slot0/2/4 X = counter*0x10-0x266+iVar5, slot1/3 X = …+counter*-0x10+0x27a,
slot5 Y ramp), commit at counter==0x27 (39) then clamps to 0x26 and waits AdvanceFrontendTickAndCheckReady.
State8 slide-out per-slot diverging vectors, commit counter==0x10 (16) → release + SetFrontendScreen
(or InitializeFrontendDisplayModeState if returnIdx==-1 in-race path).

Conditional elements: none beyond the in-race-resume branch (returnIdx==~0). No per-row value display,
NO ◄► arrows on this screen.

Input dispatch (state 6): `if(g_frontendButtonPressedFlag) switch(g_frontendButtonIndex)`: 0→GAME_OPTIONS,
1→CONTROL_OPTIONS, 2→SOUND_OPTIONS, 3→DISPLAY_OPTIONS, 4→TWO_PLAYER_OPTIONS, 5→MAIN_MENU(+commit
shadows). Cheat-code key sequences handled by the LOOP (step 15), not here.

Confidence: [CONFIRMED @ 0x0041d890 full body].

---

### Screen 13 @0x0041f990 — ScreenGameOptions  [interactive Y]
Game-rules settings list: 7 value rows (each label + value text + ◄► arrows) + OK. The value column is
a SINGLE shared tracked surface `g_lobbyErrorDialogSurface` (0xe0×0x118) that is re-baked whole on each
change and FLUSH-drawn as one overlay rect beside the button column.

Inner states: 0 init (build 8 buttons, 7 arrow rows, value-panel surface, header(6), inline-string
SNK_GameOptions_MT); 1/2 present+settle; 3 slide-in (commit==0x27); 4/5 static + (state4 only) re-bake
value panel; 6 interactive (arrow cycle + OK); 7 slide-out prep; 8 slide-out → exit (commit==0x10).

ELEMENTS:
| element | produced by | INLINE/FLUSH | dest pos | gating | notes |
|---|---|---|---|---|---|
| Header "Options" | CreateMenuStringLabelSurface(6) | INLINE bake; FLUSH | (cx-200,…) | state 0 | |
| Value-display panel (all 7 rows) | g_lobbyErrorDialogSurface = CreateTrackedFrontendSurface(0xe0,0x118); baked in state4 by 7× MeasureOrCenterFrontendLocalizedString + DrawFrontendLocalizedStringToSurface | INLINE bake; FLUSH (one QueueFrontendOverlayRect) | (cx+0x4a, cy-0x8f), 0xe0×0x118 | states 4-8 | re-baked only when entering state4 (after a change) |
| Row0 "Circuit Laps" label+◄►+value | CreateFrontendDisplayModeButton(SNK_CircuitLapsTxt,-0x128,0,0x128,0x20) slot0; InitializeFrontendDisplayModeArrows(0,1) | INLINE bake; FLUSH | slot0 | state 0 | value = sprintf_game(template) — laps count; global gCircuitLapsConfigShadow (0..3) |
| Row1 "Checkpoint Timers" | btn(SNK_CheckpointTimersButTxt) slot1; arrows(1,1) | INLINE; FLUSH | slot1 | state 0 | value = SNK_OnOffTxt[g_specialEncounterUnlockA] (on/off, &1) |
| Row2 "Traffic" | btn(SNK_TrafficButTxt) slot2; arrows(2,1) | INLINE; FLUSH | slot2 | state 0 | value = SNK_OnOffTxt[g_specialEncounterUnlockB] (&1) |
| Row3 "Cops" | btn(SNK_CopsButTxt) slot3; arrows(3,1) | INLINE; FLUSH | slot3 | state 0 | value = SNK_OnOffTxt[gSpecialEncounterConfigShadow] (&1) |
| Row4 "Difficulty" | btn(SNK_DifficultyButTxt) slot4; arrows(4,1) | INLINE; FLUSH | slot4 | state 0 | value = SNK_DifficultyTxt[g_raceDifficultyTier] (0..2 wrap) |
| Row5 "Dynamics" | btn(SNK_DynamicsButTxt) slot5; arrows(5,1) | INLINE; FLUSH | slot5 | state 0 | value = SNK_DynamicsTxt[gDynamicsConfigShadow] (&1) |
| Row6 "3D Collisions" | btn(SNK_3dCollisionsButTxt) slot6; arrows(6,1) | INLINE; FLUSH | slot6 | state 0 | value = SNK_OnOffTxt[g3dCollisionsConfigShadow] (&1) |
| "OK" button | btn(SNK_OkButTxt,-0x128,0,0x60,0x20) slot7 | INLINE; FLUSH | slot7 | state 0 | idx7 → OPTIONS_HUB; ESC target (idx7) |
| ◄► arrows (rows 0-6) | InitializeFrontendDisplayModeArrows(N,1) from g_browserSelectionBarSprite | INLINE bake into each slot surf (sets flags|=2); FLUSH | edges of each row slot | state 0 | right_flag=1 |
| Mouse cursor + highlight | LOOP + RenderFrontendDisplayModeHighlight | FLUSH | — | cursorOverlayHidden==0 | |

Primed contract globals (the values each row reflects): row0 `gCircuitLapsConfigShadow` (int,clamp
0..3); row1 `g_specialEncounterUnlockA_PROVISIONAL`; row2 `g_specialEncounterUnlockB_PROVISIONAL`;
row3 `gSpecialEncounterConfigShadow`; row4 `g_raceDifficultyTier` (0x00466010, wrap 0↔2); row5
`gDynamicsConfigShadow`; row6 `g3dCollisionsConfigShadow`. Plus `g_lobbyErrorDialogSurface` (value
panel), `g_currentScreenIndex`, `g_frontendEscKeyButtonIndex=7`, inline-string `SNK_GameOptions_MT`,
`g_returnToScreenIndex`. (These shadows are the ones ScreenOptionsHub OK commits to live globals.)

Animation: state3 slide-in commit==0x27(39); state8 slide-out commit==0x10(16). On any value change the
screen jumps back to state4 to re-bake the value panel.

Conditional elements: in-race-resume branch (returnIdx==~0 → InitializeRaceSeriesSchedule +
InitializeFrontendDisplayModeState). Value panel re-baked only in state4. No other conditionals.

Input dispatch (state 6): `if(g_postRaceRacerCardNavDirection!=0) switch(g_frontendButtonIndex)`:
row0 `gCircuitLapsConfigShadow += dir` clamp[0,3]; row1/2/3/5/6 `= (dir + shadow) & 1` (toggle);
row4 `g_raceDifficultyTier += dir` wrap (<0→2, >2→0); then force state=4 (re-bake). OK:
`g_frontendButtonPressedFlag && g_frontendButtonIndex==7` → returnIdx=OPTIONS_HUB(12), ++state.
`g_postRaceRacerCardNavDirection` is the ◄(−1)/►(+1) delta from the selection update.

Confidence: [CONFIRMED @ 0x0041f990 full body; value LUTs SNK_OnOffTxt(ptr@0045d350)/
SNK_DifficultyTxt(0045d34c)/SNK_DynamicsTxt(0045d2c4) are LANGUAGE.DLL pointer tables indexed *4].
[UNCERTAIN: the row0 sprintf template `g_uiFormatStringScratchTemplate_PROVISIONAL` literal not dumped
— produces the laps-count numeral string.]

---

### Screen 14 @0x0041df20 — ScreenControlOptions  [interactive Y]
Input-device options: 2 player rows (each = Player label button + device-name value + device ICON +
◄► arrows) + 2 Configure buttons + OK. The device NAME panel is the shared 0xe0×0xa0
`g_lobbyErrorDialogSurface`; the device ICONS come from Controllers.tga (loaded into
`g_soundOptionsMenuVolume` — misnamed surface).

Inner states: 0 init (build 5 buttons, arrows on rows 0 & 2 ONLY, load Controllers.tga, value panel,
header(6), inline SNK_CtrlOptions_MT, + in-race-extra string SNK_CtrlOptions_Ex if returnIdx==~0);
1/2 present+settle; 3 slide-in + device-icon overlay (commit==0x27); 4/5 static + (state4) re-bake
device-name panel; 6 interactive (cycle device / Configure / OK); 7 slide-out prep; 8 slide-out → exit
(commit==0x10).

Device class → row value index (`local_84` for P1, `local_88` for P2), computed at top of fn from
`(&g_player1DeviceDesc)[playerInputSource]`: byte==3 → 0 (keyboard); byte==4 → 1 (joystick), unless
hi-byte 0x400 → 2 (wheel) or 0x600 → 3 (wheel+pedals); desc==0xffffffff → 4 (none/disabled, icon hidden).

ELEMENTS:
| element | produced by | INLINE/FLUSH | dest pos | gating | notes |
|---|---|---|---|---|---|
| Header "Options" | CreateMenuStringLabelSurface(6) | INLINE bake; FLUSH | (cx-200,…) | state 0 | |
| Device-name value panel (P1+P2) | g_lobbyErrorDialogSurface=CreateTrackedFrontendSurface(0xe0,0xa0); state4 bakes 2× sprintf_game + MeasureOrCenterFrontendLocalizedString + DrawFrontendLocalizedStringToSurface | INLINE bake; FLUSH | (cx+0x4a, cy-0x8f), 0xe0×0xa0 | states 4-8 | fmt = DAT_004658e4 (keyboard, class&3==0) else s__s__d (joystick "%s %d") |
| **Device ICON P1** | Controllers.tga surface g_soundOptionsMenuVolume; QueueFrontendOverlayRect(src y=local_84<<5, 0x40×0x20) | INLINE-loaded asset; FLUSH | (cx+0x4a, cy-0x8f) static / animated | states 3-8, gated local_84!=4 | icon row = device class (kbd/js/wheel/pedals); src cell 0x40×0x20, row stride 0x20 |
| **Device ICON P2** | same surface; src y=local_88<<5 | FLUSH | (cx+0x4a, cy-0x17) | local_88!=4 | hidden when device==none |
| Row0 "Player 1" label+◄► | btn(SNK_Player1ButTxt,-0x100,0,0x100,0x20) slot0; arrows(0,1) | INLINE bake; FLUSH | slot0 | state 0 | idx0 cycles P1 device |
| Row1 "Configure" (P1) | btn(SNK_ConfigureButTxt,-0x100,0,0x100,0x20) slot1 | INLINE bake; FLUSH | slot1 | state 0 | idx1 → CONTROLLER_BINDING (slot=0); NO arrows |
| Row2 "Player 2" label+◄► | btn(SNK_Player2ButTxt) slot2; arrows(2,1) | INLINE bake; FLUSH | slot2 | state 0 | idx2 cycles P2 device |
| Row3 "Configure" (P2) | btn(SNK_ConfigureButTxt) slot3 | INLINE bake; FLUSH | slot3 | state 0 | idx3 → CONTROLLER_BINDING (slot=1); NO arrows |
| "OK" button | btn(SNK_OkButTxt,-0x100,0,0x60,0x20) slot4 | INLINE bake; FLUSH | slot4 | state 0 | idx4 → OPTIONS_HUB; ESC target (idx4) |
| ◄► arrows (rows 0 & 2 only) | InitializeFrontendDisplayModeArrows(0,1) + (2,1) | INLINE bake; FLUSH | row0/row2 edges | state 0 | Configure rows (1,3) have NO arrows |
| Mouse cursor + highlight | LOOP + RenderFrontendDisplayModeHighlight | FLUSH | — | cursorOverlayHidden==0 | |

Primed contract globals: `g_player1InputSource` / `g_player2InputSource` (the device-slot index the
row value reflects; wrap &7, skips desc==0 slots, can't equal the other player's slot),
`g_player1DeviceDesc[]` (0x00465660, the 8-entry device descriptor table — byte0 = type 3/4, byte1 =
sub-class 0x04/0x06), `g_soundOptionsMenuVolume` (Controllers.tga icon surface),
`g_lobbyErrorDialogSurface` (name panel), `g_controllerBindingActivePlayerSlot` (0 or 1, which player
the Configure page edits), `g_currentScreenIndex`, `g_frontendEscKeyButtonIndex=4`, inline-string
`SNK_CtrlOptions_MT` (+ `SNK_CtrlOptions_Ex` at entry slot 9 if in-race), `g_returnToScreenIndex`.

Animation: state3 slide-in commit==0x27(39), device icons slide with the rows
(X=iVar4+counter*-0x10+0x38c). state8 slide-out commit==0x10(16); icons follow row Y.

Conditional elements: device icon hidden when class index ==4 (desc==0xffffffff = no device);
name-panel format string keyboard vs joystick by `(class & 3)==0`; arrows present on rows 0/2 only;
in-race entry adds `SNK_CtrlOptions_Ex` extra line (returnIdx==~0) and OK routes back to in-race options
(`((returnIdx!=~0)-1 & 0xfffffff9) + OPTIONS_HUB` = OPTIONS_HUB normally, in-race sentinel otherwise).

Input dispatch (state 6): `if(g_postRaceRacerCardNavDirection!=0)`: idx0 → advance `g_player1InputSource`
(+dir &7, skip empty desc, skip == P2) then state4; idx2 → same for `g_player2InputSource`.
`if(g_frontendButtonPressedFlag)`: idx1 → set slot=0, returnIdx=CONTROLLER_BINDING(18), ++state;
idx3 → slot=1, returnIdx=CONTROLLER_BINDING; idx4 → OK back to OPTIONS_HUB (or in-race). Device cycle =
◄►; Configure = press; persistence of source selection only (detailed binding compiled later at race
start by DXInput::SetConfiguration).

Confidence: [CONFIRMED @ 0x0041df20 full body; device-class decode at fn head; Controllers.tga via
s_Front_End_Controllers_tga_00466044 into g_soundOptionsMenuVolume; arrows only on slots 0,2].
[UNCERTAIN: name-panel format-string literals DAT_004658e4 (keyboard) / s__s__d_0046603c ("%s %d")
not dumped char-for-char; the device-name text source feeding %s is the LANGUAGE.DLL device label —
not asserted here. The per-icon Controllers.tga row→class mapping (0 kbd,1 js,2 wheel,3 pedals) is
inferred from the local_84 decode, not from inspecting the TGA cells.]
