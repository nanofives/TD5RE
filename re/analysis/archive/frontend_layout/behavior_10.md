# Frontend Behavior Harvest — Part 10 (screen-table indices 10–14)

Source: `TD5_d3d.exe` via Ghidra (read-only), full-function decompile of each entry.
Companion to `part_10.md` (POSITIONS). This file = BEHAVIOR + PORT PARITY.

Common dispatch facts:
- All 5 screens are FSMs on `g_frontendInnerState` (port mirror: `s_inner_state`).
- Original button click model: `BeginFrontendDisplayModePreviewLayout`/`CreateFrontendDisplayModeButton` assigns each button an implicit ORDER index. The runtime sets `g_frontendButtonIndex` (= which button is highlighted/clicked) and `g_frontendButtonPressedFlag` (= click) elsewhere (input dispatcher). Screen code reads `g_frontendButtonIndex` in a `switch`/`if`-ladder → **INDEX-based dispatch, NO userdata** (every CFDMB userdata arg = literal `0` on all 5 screens). Option arrows set `g_postRaceRacerCardNavDirection` (= -1/+1), read by option screens (13/14) per row.
- Port mirror: `frontend_create_button(label,x,y,w,h)` (no userdata), dispatch via `switch(s_button_index)` / `s_selected_button`; arrows via `frontend_option_delta()`. Port `s_button_index` ↔ orig `g_frontendButtonIndex`; port `frontend_option_delta()` ↔ orig `g_postRaceRacerCardNavDirection`.
- ESC handling: orig writes `g_frontendEscKeyButtonIndex` = index of the Back/OK button. Port has equivalent `s_button_index`==back path.

---

### Screen 10 @ 0x0041a7b0 — RunFrontendCreateSessionFlow  [interactive: Y]
Port handler: td5_frontend.c:7034 (Screen_CreateSession, case TD5_SCREEN_CREATE_SESSION)
DISPATCH MODEL: index-switch @0x0041a7b0 — `switch(g_frontendInnerState)` (DXPlay name-entry/join FSM); per-state buttons read `g_frontendButtonIndex` + name editor `g_postRaceNameEditState`.

This is NOT a row-of-options screen. It is a text-entry + DirectPlay session-create/join state machine. Buttons appear only in entry states.

BUTTONS (original order):
| # | label (SNK_/literal) | userdata | action | gating | PORT status | port ref | gap |
|---|---|---|---|---|---|---|---|
| 1 | SNK_EnterNewSessionNameButTxt ("ENTER NEW SESSION NAME") | 0 | name-entry field button (state 0); target buf DAT_00497068 max 0x10; confirm (g_postRaceNameEditState==2) → advance to join flow | state 0 only, when g_connBrowserCursorIndex==0 | Partial | :7040 frontend_create_button("Enter Name",...) | port label "Enter Name" not SNK string; collapses both session-name and player-name into one input |
| 2 | SNK_BackButTxt ("BACK") | 0 | btn idx 1 + pressed → g_returnToScreenIndex=SESSION_PICKER, slide out | state 0 | Implemented | :7041 frontend_create_button("Back",...); :7058 if(s_button_index==1)→SESSION_PICKER | faithful target |
| 3 | SNK_EnterPlayerNameButTxt ("ENTER PLAYER NAME") | 0 | player-name field (states 4 & 0x10); target g_postRacePlayerNameEntryBuffer max 0x10 | state 4 (from session-create path) / state 0x10 (g_connBrowserCursorIndex!=0 join path) | Missing | — | port has no separate player-name entry state; states 4-15 collapsed (ARCH-DIVERGENCE DXPTYPE @:7074) |
| 4 | SNK_BackButTxt ("BACK") | 0 | back from player-name (states 4/0x10) → g_returnToScreenIndex=SESSION_PICKER | states 4,0x10 | Missing | — | collapsed with player-name state |

NON-BUTTON ELEMENTS:
| element | role/behavior | global reflected | PORT status | port ref | gap |
|---|---|---|---|---|---|
| Menu header label | per-state QFOR header strip (screen title surf) | g_currentScreenIndex (CreateMenuStringLabelSurface(5)) | Implemented | frontend_init_return_screen path | rendered generically |
| Name input field renderer | RenderFrontendCreateSessionNameInput (states 2,6,0x12): live text + blinking caret | g_postRaceNameEditTargetPtr buffer | Partial | :7057 frontend_render_text_input() | single input, default text "New Session" prefill (orig prefills from g_localComputerName when empty, @0x41b0c0) |
| Sprite slot 0/1 | two animated decoration sprites slide in/out (states 1,3,5,7,0x11,0x13) | g_frontendAnimFrameCounter | Partial | timed-animation only (no positioned sprites) | port has no per-sprite slide; uses generic frontend_update_timed_animation |
| DXPlay join (state 0x14/0x15) | JoinSession + SNK_SeshJoinMsg broadcast; on fail → SESSION_LOCKED | dpu_exref session table | Missing (ARCH-DIVERGENCE) | :7081 collapsed | DXPTYPE wire incompatible; intentional |

PARITY VERDICT: Partial — flow shape (name entry → lobby/back) preserved; the second player-name stage and DXPlay handshake are intentionally collapsed (ARCH-DIVERGENCE: DXPTYPE).
GAPS (actionable):
- Player-name entry state (orig states 4/0x10, SNK_EnterPlayerNameButTxt) not reproduced; port goes name→lobby in one input.
- Button labels are literals ("Enter Name"/"Back"), not resolved SNK_EnterNewSessionNameButTxt / SNK_BackButTxt.
- Empty-name fallback to g_localComputerName (orig @0x41b0c0) absent; port hardcodes "New Session".
Confidence: [CONFIRMED @ 0x0041a7b0 decomp; port td5_frontend.c:7034-7090]. ARCH-DIVERGENCE annotated in both orig comment header and port :7074.

---

### Screen 11 @ 0x0041c330 — RunFrontendNetworkLobby  [interactive: Y]
Port handler: td5_frontend.c:7105 (Screen_NetworkLobby, case TD5_SCREEN_NETWORK_LOBBY)
DISPATCH MODEL: index-switch @0x0041c330 — `switch(g_frontendInnerState)` (18-state lobby); button clicks read `g_frontendButtonIndex` (1=ChangeCar→3, 4=Start, 5=Exit) in state 3.

BUTTONS (original order, created state 0):
| # | label (SNK_/literal) | userdata | action | gating | PORT status | port ref | gap |
|---|---|---|---|---|---|---|---|
| 0 | (NULL) decoration strip | 0 | non-interactive chat-input bar background (-0x1d0 wide, h=0x18) | always | Implemented | :7130 frontend_create_button("",...) | faithful (empty label) |
| 1 | SNK_MessageWindowButTxt ("MESSAGE WINDOW") | 0 | chat/message window surface (-0x200, h=0x80); non-click backing | always | Implemented | :7131 "Messages" | label literal not SNK |
| 2 | SNK_StatusButTxt ("STATUS") | 0 | player-roster panel surface (-0xe0, h=0x86); non-click backing | always | Implemented | :7132 "Status" | label literal not SNK |
| 3 | SNK_ChangeCarButTxt ("CHANGE CAR") | 0 | state3: g_frontendButtonIndex 2→3, pressed→lobbyPlayerStatus=1, SetFrontendScreen(CAR_SELECTION) | state 3 input | Partial | :7133 "Change Car"; lobby state3 handler ~:7200+ | DXPTYPE-gated; reachable but net incomplete |
| 4 | SNK_StartButTxt ("START") | 0 | idx4 pressed → lobbyPlayerStatus=2; host (slot==host) → state5 seal flow, else SNK_WaitForHostMsg broadcast | state 3, host/peer branch | Partial (ARCH-DIVERGENCE) | :7134 "Start" | seal/handshake collapsed |
| 5 | SNK_ExitButTxt ("EXIT") | 0 | idx5 pressed → lobbyPlayerStatus=1, g_returnToScreenIndex=2, slide-out state6 → SESSION_LOCKED/MAIN_MENU | state 3 | Implemented | :7135 "Exit" | faithful exit path |
| — | SNK_OkButTxt ("OK") | 0 | error-dialog OK (state 7, error branch, EXPLICIT x/y) | state 7, g_returnToScreenIndex∉{0,2} | Partial | error-dialog path | dialog ported structurally |
| — | SNK_YesButTxt / SNK_NoxButTxt ("YES"/"NO") | 0 | error-dialog confirm (state 7 yes/no branch, EXPLICIT x/y) | state 7, g_returnToScreenIndex∈{0,2} | Partial | error-dialog path | structural only |

NON-BUTTON ELEMENTS:
| element | role/behavior | global reflected | PORT status | port ref | gap |
|---|---|---|---|---|---|
| Session header strip | QFOR g_lobbySessionHeaderSurface (480x32) under title | g_lobbySessionHeaderSurface | Partial | lobby render | structural |
| Chat panel | RenderFrontendLobbyChatPanel (state 3) | g_lobbyMsgRing* | Partial | lobby chat render | DXPTYPE: no live peers |
| Status/roster panel | RenderFrontendLobbyStatusPanel; per-slot status (0..3) | g_lobbySlotStatusTable[6] | Partial | lobby status render | shows local slot only |
| Chat input | RenderFrontendLobbyChatInput; 60-char (0x3c) buffer g_chatTokenizerCharClass | g_postRaceNameEditState | Partial | :7138 s_chat_input_buffer | input present, net send collapsed |
| Error dialog | g_lobbyErrorDialogSurface (482x64); SNK_NetErrString1-4 (idx*0x28/*0x20) centered | g_returnToScreenIndex (err code) | Partial | error-dialog states | strings not confirmed wired to SNK table |

PARITY VERDICT: Partial — full 18-state FSM mirrored for structure; all DirectPlay paths (seal, config broadcast, DXPSTART rendezvous, peer roster) are non-functional by design (ARCH-DIVERGENCE: DXPTYPE).
GAPS (actionable):
- Button labels are literals, not SNK_* (Messages/Status/Change Car/Start/Exit vs MESSAGE WINDOW/STATUS/CHANGE CAR/START/EXIT).
- Error-dialog text (SNK_NetErrString1-4) wiring to port surface not verified.
- Multi-peer roster, seal/handshake, error Yes/No semantics structural-only (DXPTYPE).
Confidence: [CONFIRMED @ 0x0041c330 decomp; port :7105-7448]. [UNCERTAIN: exact port line of state-3 button switch and error-string wiring — handler body spans :7160-7445, not line-mapped per-button.]

---

### Screen 12 @ 0x0041d890 — ScreenOptionsHub  [interactive: Y]
Port handler: td5_frontend.c:7456 (Screen_OptionsHub, case TD5_SCREEN_OPTIONS_HUB)
DISPATCH MODEL: index-switch @0x0041d8bc — state 6 `switch(g_frontendButtonIndex)` (0..5), gated by g_frontendButtonPressedFlag. Pure navigation menu (NO option toggles on this screen).

BUTTONS (original order):
| # | label (SNK_/literal) | userdata | action | gating | PORT status | port ref | gap |
|---|---|---|---|---|---|---|---|
| 0 | SNK_GameOptionsButTxt ("GAME OPTIONS") | 0 | g_returnToScreenIndex=GAME_OPTIONS(0xd), blit, slide out | pressed | Implemented | :7496 case 0 → TD5_SCREEN_GAME_OPTIONS | faithful |
| 1 | SNK_ControlOptionsButTxt ("CONTROL OPTIONS") | 0 | g_returnToScreenIndex=CONTROL_OPTIONS(0xe) | pressed | Implemented | :7497 case 1 → CONTROL_OPTIONS | faithful |
| 2 | SNK_SoundOptionsButTxt ("SOUND OPTIONS") | 0 | g_returnToScreenIndex=SOUND_OPTIONS; blit-secondary-to-primary | pressed | Implemented | :7498 case 2 → SOUND_OPTIONS | faithful |
| 3 | SNK_GraphicsOptionsButTxt ("GRAPHICS OPTIONS") | 0 | g_returnToScreenIndex=DISPLAY_OPTIONS | pressed | Implemented | :7499 case 3 → DISPLAY_OPTIONS | faithful |
| 4 | SNK_TwoPlayerOptionsButTxt ("TWO PLAYER OPTIONS") | 0 | g_returnToScreenIndex=TWO_PLAYER_OPTIONS | pressed | Implemented | :7500 case 4 → TWO_PLAYER_OPTIONS | faithful |
| 5 | SNK_OkButTxt ("OK") | 0 | **COMMITS shadow option globals → live**: g_cameraMode=g3dCollisionsConfigShadow^1; pool[0x1f8].velocity_z=gDynamicsConfigShadow; copies g_specialEncounterUnlockB→pool[0x1f0] bytes; gRaceDifficultyTier=g_raceDifficultyTier; _g_specialEncounterType=g_specialEncounterUnlockA; returnToScreen=MAIN_MENU | pressed | **Wrong/Missing** | :7501 case 5 → just s_return_screen=MAIN_MENU | port OK does NOT commit the game-options shadow values to the live race globals; just navigates back. Orig OK on the HUB is the commit point for difficulty/dynamics/3dcollisions/special-encounter shadows. |

NON-BUTTON ELEMENTS:
| element | role/behavior | global reflected | PORT status | port ref | gap |
|---|---|---|---|---|---|
| Menu header label | screen title (CreateMenuStringLabelSurface(6)) + SetFrontendInlineStringTable(SNK_Options_MT) | g_currentScreenIndex | Implemented | header path | faithful |
| 6 sliding button sprites | slide-in (state3) / slide-out (state8) anim | g_frontendAnimFrameCounter | Partial | timed-animation | generic timed anim, not per-sprite path |

PARITY VERDICT: Divergent (one mechanism) — navigation faithful, but the OK-button shadow→live commit (the screen's only side-effecting action) is absent in the port.
GAPS (actionable):
- **OK button (idx 5) must commit option shadows**: orig @0x41dabe sets g_cameraMode (=^1 of 3dCollisions shadow), dynamics→pool velocity_z, special-encounter A/B, gRaceDifficultyTier←g_raceDifficultyTier. Port only sets s_return_screen=MAIN_MENU. Verify whether the port commits these elsewhere (e.g. on race start) — if so it's a deferred-commit divergence, else a functional gap.
- Button labels literal, not SNK_*.
Confidence: [CONFIRMED @ 0x0041d890 decomp; port :7456-7525]. [UNCERTAIN: whether port commits difficulty/dynamics/3dcollisions shadows at race start instead — MEMORY notes difficulty_tier routing exists at td5_physics/ai; need cross-check of g_td5.difficulty_tier write path vs this OK handler.]

---

### Screen 13 @ 0x0041f990 — ScreenGameOptions  [interactive: Y]
Port handler: td5_frontend.c:7534 (Screen_GameOptions, case TD5_SCREEN_GAME_OPTIONS)
DISPATCH MODEL: index-switch @0x0041ffef — state 6: arrow `switch(g_frontendButtonIndex)` (0..6) gated by g_postRaceRacerCardNavDirection (=±1); OK = idx7 gated by g_frontendButtonPressedFlag. Each row = OPTION CYCLER writing a config shadow global.

BUTTONS / OPTION ROWS (original order):
| # | label (SNK_) | userdata | option global written + value set | gating | PORT status | port ref | gap |
|---|---|---|---|---|---|---|---|
| 0 | SNK_CircuitLapsTxt ("CIRCUIT LAPS") | 0 | gCircuitLapsConfigShadow += dir; clamp [0,3] | arrow | Partial | :7581 s_game_option_laps +=delta, wrap 0..3 | port WRAPS (0→3, 3→0); **orig CLAMPS** (stays at 0 or 3). Behavior diverges at ends. |
| 1 | SNK_CheckpointTimersButTxt ("CHECKPOINT TIMERS") | 0 | g_specialEncounterUnlockA_PROVISIONAL = (dir+val)&1 (0/1 toggle) | arrow | Implemented | :7586 s_game_option_checkpoint_timers ^=1 | faithful toggle |
| 2 | SNK_TrafficButTxt ("TRAFFIC") | 0 | g_specialEncounterUnlockB_PROVISIONAL = (dir+val)&1 | arrow | Implemented | :7589 s_game_option_traffic ^=1 | faithful |
| 3 | SNK_CopsButTxt ("POLICE") | 0 | gSpecialEncounterConfigShadow = (dir+val)&1 | arrow | Implemented | :7592 s_game_option_cops ^=1 | faithful (label "Police") |
| 4 | SNK_DifficultyButTxt ("DIFFICULTY") | 0 | g_raceDifficultyTier += dir; if <0→2, if >2→0 (3-way WRAP) | arrow | Implemented | :7595 wrap 0..2 (−→2, >2→0) | faithful wrap |
| 5 | SNK_DynamicsButTxt ("DYNAMICS") | 0 | gDynamicsConfigShadow = (dir+val)&1 (0=SIMULATION,1=ARCADE) | arrow | Implemented | :7600 s_game_option_dynamics ^=1 | faithful |
| 6 | SNK_3dCollisionsButTxt ("3D COLLISIONS") | 0 | g3dCollisionsConfigShadow = (dir+val)&1 | arrow | Implemented | :7603 s_game_option_collisions ^=1 | faithful |
| 7 | SNK_OkButTxt ("OK") | 0 | g_returnToScreenIndex=OPTIONS_HUB(0xc); slide out | pressed | Implemented | :7607 idx7 → OPTIONS_HUB | faithful |

NON-BUTTON ELEMENTS:
| element | role/behavior | global reflected | PORT status | port ref | gap |
|---|---|---|---|---|---|
| Value panel (224x280) | g_lobbyErrorDialogSurface; 7 value strings drawn via MeasureOrCenter+DFLS each redraw (state 4) | per-row shadow globals | Implemented | :3972 frontend_render_game_options_overlay | faithful value text |
| Row 0 value | sprintf(g_uiFormatStringScratchTemplate) numeric laps | gCircuitLapsConfigShadow | Partial | :3979 "%d",(laps+1)*2 | **value MAPPING differs**: orig laps row uses sprintf template; port shows (laps+1)*2 (= 2,4,6,8). [UNCERTAIN orig literal lap numbers] |
| Rows 1-3,6 value | SNK_OnOffTxt[val] ("OFF"/"ON") | each toggle shadow | Implemented | :3981-3986 on_off[] | faithful |
| Row 4 value | SNK_DifficultyTxt[tier] (EASY/NORMAL/HARD) | g_raceDifficultyTier | Implemented | :3984 difficulty[] | faithful |
| Row 5 value | SNK_DynamicsTxt[val] (SIMULATION/ARCADE) | gDynamicsConfigShadow | Implemented | :3985 dynamics[] | faithful |
| Per-row ◄► arrows | InitializeFrontendDisplayModeArrows(0..6,1) — all 7 rows get arrows | — | Implemented | frontend arrow render :3972area | faithful (7 rows arrowed) |

PARITY VERDICT: Faithful (one divergence) — option rows, globals, value text, arrows all match; only Circuit Laps end-behavior differs (port wraps, orig clamps).
GAPS (actionable):
- **Circuit Laps: orig CLAMPS at [0,3]** (no wrap); port wraps 3→0 / 0→3 (:7582-7583). Fix to clamp.
- Confirm laps display mapping `(laps+1)*2` vs orig sprintf template literal numbers.
- Button labels literal, not SNK_*.
Confidence: [CONFIRMED @ 0x0041f990 decomp + port :7534/3972]. Orig case-6 clamp/wrap rules read verbatim from decomp.

---

### Screen 14 @ 0x0041df20 — ScreenControlOptions  [interactive: Y]
Port handler: td5_frontend.c:7636 (Screen_ControlOptions, case TD5_SCREEN_CONTROL_OPTIONS)
DISPATCH MODEL: index-switch @0x0041e5xx — state 6: arrow branch on g_frontendButtonIndex (0=P1 device cycle, 2=P2 device cycle) gated by g_postRaceRacerCardNavDirection; pressed branch (1=Config P1, 3=Config P2, 4=OK). Rows 0/2 are DEVICE-SOURCE CYCLERS (not navigation).

BUTTONS (original order):
| # | label (SNK_) | userdata | action | gating | PORT status | port ref | gap |
|---|---|---|---|---|---|---|---|
| 0 | SNK_Player1ButTxt ("PLAYER 1") | 0 | **arrow → cycle g_player1InputSource** (+dir &7, skip empty devices via g_player1DeviceDesc[], skip == player2 source); redraw state4 | arrow | **Missing** | :7644 frontend_create_preview_button("Player 1") | port treats row 0 as inert preview button; NO device-source cycling on arrows. Orig changes which input device P1 uses. |
| 1 | SNK_ConfigureButTxt ("CONFIGURE") | 0 | pressed → g_controllerBindingActivePlayerSlot=0, g_returnToScreenIndex=CONTROLLER_BINDING(0x12) | pressed | Implemented | :7674 idx1/3 → CONTROLLER_BINDING, s_ctrl_player=0 | faithful |
| 2 | SNK_Player2ButTxt ("PLAYER 2") | 0 | **arrow → cycle g_player2InputSource** (+dir &7, skip empty, skip == P1) | arrow | **Missing** | :7646 frontend_create_preview_button("Player 2") | same gap as row 0 for P2 |
| 3 | SNK_ConfigureButTxt ("CONFIGURE") | 0 | pressed → g_controllerBindingActivePlayerSlot=1, CONTROLLER_BINDING | pressed | Implemented | :7676 s_ctrl_player=(idx==3)?1:0 | faithful |
| 4 | SNK_OkButTxt ("OK") | 0 | pressed → g_returnToScreenIndex=OPTIONS_HUB (or self if from binding return); slide out | pressed | Implemented | :7680 idx4 → OPTIONS_HUB | faithful |

NON-BUTTON ELEMENTS:
| element | role/behavior | global reflected | PORT status | port ref | gap |
|---|---|---|---|---|---|
| P1 device icon | QFOR g_soundOptionsMenuVolume (Controllers.tga) src_y=local_84<<5, 64x32, at (cx+0x4a, cy-0x8f); local_84 derived from g_player1DeviceDesc[g_player1InputSource] (0=kbd,1=joy,2=400,3=600,4=none→hidden) | g_player1InputSource | Implemented | :4511 fe_draw_quad 394,97 row=p1_type*32 | port uses td5_input_get_device_type(0) for row; faithful icon |
| P2 device icon | same, src_y=local_88<<5, at (cx+0x4a, cy-0x17 = 217) | g_player2InputSource | Implemented | :4513 fe_draw_quad 394,217 | faithful icon |
| Device name value panel (224x160) | g_lobbyErrorDialogSurface; state4 draws P1 then P2 device-name string via MeasureOrCenter(x=0x4a)+DFLS: keyboard→"%s" (DAT_004658e4), joy/pad→"%s %d" (s__s__d_0046603c, name+index) | g_player1/2InputSource | **Missing** | — | port renders icons only; the device NAME text strings ("%s"/"%s %d") drawn into the value panel are NOT reproduced. Panel exists but no device-name text. |
| Row 0/2 ◄► arrows | InitializeFrontendDisplayModeArrows(0,1) and (2,1) only (rows 1,3,4 have no arrows) | — | **Missing** | — | port renders no selector arrows on Player1/Player2 rows AND has no cycle handler → device source cannot be changed in port |
| Header / inline ex-string | SetFrontendInlineStringEntry(9,SNK_CtrlOptions_Ex) when returning from binding | g_returnToScreenIndex==~LOCALIZATION_INIT | Partial | header path | extra hint line not confirmed |

PARITY VERDICT: Divergent — Configure/OK navigation faithful, but the screen's PRIMARY interactive feature (cycling each player's input device via ◄► on the Player1/Player2 rows) is entirely missing, along with the device-name text labels. Port can only enter the binding sub-screen; it cannot change device assignment here.
GAPS (actionable):
- **Implement P1/P2 device cycling** (orig state6 idx0/idx2): on arrow, advance g_playerNInputSource (+dir &7), skip slots where g_playerNDeviceDesc[idx]==0, skip when equal to the other player's source. Port currently has no `frontend_option_delta()` call in Control Options case 6.
- **Render device-name strings** into the value panel: keyboard → "%s" device name; joystick/joypad → "%s %d" (name + 1-based index). Port draws only the icon row.
- **Add ◄► arrows** on rows 0 and 2 (orig InitializeFrontendDisplayModeArrows(0,1)/(2,1)).
- Button labels literal, not SNK_*.
Confidence: [CONFIRMED @ 0x0041df20 decomp; device strings DAT_004658e4="%s", s__s__d_0046603c="%s %d" read from memory; port :4489 overlay + :7636 handler]. local_84/local_88 device-index = runtime (g_player1DeviceDesc table), confirmed in decomp.

---

## Cross-screen summary
- Dispatch: all 5 = index-switch on g_frontendInnerState + g_frontendButtonIndex; userdata always literal 0 (no callback/userdata model). Port mirrors with s_inner_state + s_button_index. CONFIRMED.
- Recurring port gap (cosmetic): all button labels are English literals, not the resolved SNK_*ButTxt LANGUAGE.DLL strings (non-functional but a localization fidelity gap).
- Functional Missing/Wrong:
  - S10: player-name 2nd-stage + empty-name→computer-name fallback Missing (DXPTYPE-adjacent).
  - S12: OK-button shadow→live commit Missing/Wrong (only side-effect of the hub).
  - S13: Circuit Laps clamp-vs-wrap Wrong.
  - S14: device-source cycling Missing; device-name text Missing; row 0/2 arrows Missing.
