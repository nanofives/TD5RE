# Frontend Behavior Harvest — Part 05 (screen indices 5–9)

Source: TD5_d3d.exe via Ghidra (read-only, slot TD5_pool10). Port: `td5mod/src/td5re/td5_frontend.c`.
Position harvest companion: `part_05.md` (draw inventory). This file = BEHAVIOR + parity.

Shared dispatch facts:
- Original button factory `CreateFrontendDisplayModeButton @0x00425de0(label,x,y,w,h,userdata)`. **Every call in these 5 screens passes userdata = 0** (literal 6th arg `0` in every decomp call). The original does NOT dispatch on userdata — it dispatches on **`g_frontendButtonIndex`** (insertion order index) compared/switched in the interaction state. So the port's index-switch model is faithful to the original's model; userdata is dead in these screens.
- Greyed/disabled variant = `CreateFrontendDisplayModePreviewButton` (same signature). Port equivalent = `frontend_create_preview_button(...)`.
- `g_frontendButtonPressedFlag` = press edge; `g_frontendButtonIndex` = hovered/selected index; `g_frontendEscKeyButtonIndex` = which button ESC maps to.
- Port screen enum (td5_types.h:312): MAIN_MENU=5, RACE_TYPE_MENU=6, QUICK_RACE=7, CONNECTION_BROWSER=8, SESSION_PICKER=9, CREATE_SESSION=10, NETWORK_LOBBY=11, OPTIONS_HUB=12, CONTROL_OPTIONS=14, CAR_SELECTION=20, EXTRAS_GALLERY=22, HIGH_SCORE=23, RACE_RESULTS=24. `~TD5_SCREEN_LOCALIZATION_INIT == ~0 == -1` = "launch race / restart race flow" sentinel.

---

### Screen 5 @ 0x00415490 — ScreenMainMenuAnd1PRaceFlow  [interactive: Y]
Port handler: td5_frontend.c:6265 (Screen_MainMenu, case TD5_SCREEN_MAIN_MENU)
DISPATCH MODEL: index-switch @0x004155F0 (case 4 `if (g_frontendButtonPressedFlag != 0) switch(g_frontendButtonIndex)`) — sets g_returnToScreenIndex + g_mainMenuButtonHint_PROVISIONAL then slides out (state 8→9) and SetFrontendScreen(g_returnToScreenIndex). Button 6 (Exit) opens an in-place Yes/No sub-dialog (state 4→5→6). Port mirrors with `s_button_index` switch.

BUTTONS (original order — all userdata=0):
| # | label (SNK_/literal) | userdata | action (orig addr) | gating | PORT status | port ref | gap |
|---|---|---|---|---|---|---|---|
| 0 | SNK_RaceMenuButTxt ("Race Menu") | 0 | g_returnToScreenIndex=RACE_TYPE_MENU(6); hint=1; →state8 slide-out (0x415C0E) | none | Implemented | :6343 case 0 | none |
| 1 | SNK_QuickRaceButTxt ("Quick Race") | 0 | g_returnToScreenIndex=QUICK_RACE(7); hint=2 (0x415C28) | none | Implemented | :6349 case 1 | Port also calls ConfigureGameTypeFlags()+game_type=0 here (port addition; orig does NOT — orig screen-7 sets game_type=-1 in its own init). Behavior benign (screen 7 re-inits) |
| 2 | SNK_TwoPlayerButTxt ("Two Player") OR SNK_TimeDemoButTxt | 0 | if `*(app+0x170)!=0`: benchmark path (g_benchmarkRequestPending=1, InitializeRaceSeriesSchedule, InitializeFrontendDisplayModeState, return). else: g_returnToScreenIndex=CAR_SELECTION(20); g_twoPlayerModeEnabled=1; g_frontendSelectedGameType=0; hint=3 (0x415C3C) | label & branch gated on `*(g_appExref+0x170)` (never written in binary → always TwoPlayer) | Implemented (ARCH-DIVERGENCE) | :6367 case 2 | Port label hardcoded "Two Player"; benchmark path exposed via INI `enable_benchmark` instead of app+0x170. Faithful given app+0x170 is dead in shipped binary |
| 3 | SNK_NetPlayButTxt ("Net Play") | 0 | g_returnToScreenIndex=CONNECTION_BROWSER(8); hint=4; blits exit-dialog region then →state8 (0x415C5A) | none | Implemented | :6401 case 3 | none |
| 4 | SNK_OptionsButTxt ("Options") | 0 | g_returnToScreenIndex=OPTIONS_HUB(12); hint=5 (0x415CB6) | none | Implemented | :6407 case 4 | none |
| 5 | SNK_HiScoreButTxt ("High Scores") | 0 | g_returnToScreenIndex=HIGH_SCORE(23); hint=6 (0x415CCA) | none | Implemented | :6413 case 5 | none |
| 6 | SNK_ExitButTxt ("Exit") | 0 | opens Yes/No sub-dialog: BeginFrontendDisplayModePreviewLayout, creates SNK_Yes/SNK_Nox buttons, →state5 (0x415CDE). EscKeyButtonIndex=6 | none | Implemented | :6419 case 6 →state5 | none |
| 7 (dlg) | SNK_YesButTxt ("Yes") | 0 | state6: index==0 → state10 (scatter) → SetFrontendScreen(EXTRAS_GALLERY=22) i.e. credits/quit (0x415F33) | sub-dialog only | Partial | :6431/6446/6458 | Port "Yes" → SCREEN_EXTRAS_GALLERY then frontend_post_quit (state7/10). Orig case 0xc routes EXTRAS_GALLERY as the credits-roll exit. Acceptable |
| 8 (dlg) | SNK_NoxButTxt ("No") | 0 | state6: index==1 → release dialog buttons, restore layout, EscKeyButtonIndex=5, →state7→state4 (0x415F5B) | sub-dialog only | Implemented | :6447 (No → trims buttons to 7, refocus 6, state4) | none |

NON-BUTTON ELEMENTS:
| element | role/behavior | global reflected | PORT status | port ref | gap |
|---|---|---|---|---|---|
| Header/title label | CreateMenuStringLabelSurface(0) blit; slides in state3, static state4 | g_currentScreenIndex (surface handle) | Implemented | title-tex path :5783 | Rendered via per-screen title texture |
| 7 button slide sprites (slot 0..6) | MoveFrontendSpriteRect intro(3)/outro(9)/extras-outro(0xc) animation | sprite slots | Implemented | timed-animation :6326/:6470 | Anim positions approximated by frontend_update_timed_animation, not per-slot orig formula |
| "Exit?" sub-caption | QueueFrontendOverlayRect of g_oneRaceFlowSelectedCarId (CreateMenuStringLabelSurface(8)) in state 5/6/10 | g_oneRaceFlowSelectedCarId | [UNCERTAIN port] | — | Port shows a generic Yes/No dialog; the localized "Exit?" caption surface (orig CreateMenuStringLabelSurface(8)) not separately drawn |
| "Must select controller" dialog (states 0x14-0x17) | SNK_MustSelectTxt + SNK_MustPlayer1/2Txt; OK button; triggered when input source==7 (no joystick) | g_player1/2InputSource, g_frontendMustSelectMessage | Missing (ARCH-DIVERGENCE) | :6472 note | Intentionally dropped — port is keyboard-first, joystick==7 gate unreachable |

PARITY VERDICT: Faithful — all 7 buttons + Yes/No dispatch correct; only the controller-required dialog (input-source gated) and the localized "Exit?" caption surface are absent (both intentional/keyboard-first).
GAPS (actionable):
- "Must select controller" dialog (orig states 0x14-0x17) absent — intentional ARCH-DIVERGENCE, document only.
- Button-2 label is hardcoded "Two Player"; benchmark branch moved to INI (app+0x170 is dead in binary, so faithful).
- Port adds ConfigureGameTypeFlags() on Quick-Race press (orig does not); benign because screen 7 re-inits game_type.
Confidence: [CONFIRMED @ 0x00415490 case-4 switch, dispatch addrs 0x415C0E..0x415F5B] / [UNCERTAIN: exact "Exit?" caption surface presence in port render path]

---

### Screen 6 @ 0x004168b0 — RaceTypeCategoryMenuStateMachine  [interactive: Y]
Port handler: td5_frontend.c:6528 (Screen_RaceTypeCategory, case TD5_SCREEN_RACE_TYPE_MENU)
DISPATCH MODEL: index-switch, TWO pages.
- Top page @0x00416B3D (state 3 `if (g_frontendButtonPressedFlag) switch(g_frontendButtonIndex)`).
- Cup sub-page @0x00417230 (state 8 `if (g_frontendButtonPressedFlag && idx>=0)`; idx<6 → game_type=idx+1, idx==6 → Back).
- Description preview redrawn on hover-change (state 3→4 / 8→9) into g_lobbyErrorDialogSurface.

BUTTONS — TOP PAGE (states 0/0xb; original order, all userdata=0):
| # | label | userdata | action (orig addr) | gating | PORT status | port ref | gap |
|---|---|---|---|---|---|---|---|
| 0 | SNK_SingleRaceButTxt ("Single Race") | 0 | g_frontendSelectedGameType=0; ConfigureGameTypeFlags(); g_returnToScreenIndex=CAR_SELECTION(20); →state5 (0x416B5F) | none | Implemented | :6585 case 0 | none |
| 1 | SNK_CupRaceButTxt ("Cup Race") | 0 | →state6 (cup sub-menu) (0x416BBD) | none | Implemented | :6592 case 1 →state6 | none |
| 2 | SNK_ContCupButTxt ("Continue Cup") | 0 | LoadContinueCupData(); g_replayFileAvailable=0; g_returnToScreenIndex=RACE_RESULTS(24); →state5 (0x416BCF) | **greyed (Preview) if `ValidateCupDataChecksum()==0`** | Implemented | :6552 (preview gate) / :6596 case 2 | Port: if valid → load+go RACE_RESULTS; else play sfx(10). Matches |
| 3 | SNK_TimeTrialsButTxt ("Time Trials") | 0 | g_frontendSelectedGameType=9 (orig enum: 9=TimeTrial); ConfigureGameTypeFlags(); →CAR_SELECTION (0x416B9D via i+3 override) | none | Implemented (enum remap) | :6606 case 3 → port game_type=7 | Port uses port-enum 7=TIME_TRIAL (deliberate value remap, see :6506 note). Semantics preserved |
| 4 | SNK_DragRaceButTxt ("Drag Race") | 0 | g_frontendSelectedGameType=7 (orig 7=Drag); →CAR_SELECTION (0x416B90, i+3=7) | none | Implemented (enum remap) | :6613 case 4 → port game_type=9 | Port-enum 9=DRAG_RACE. Semantics preserved |
| 5 | SNK_CopChaseButTxt ("Cop Chase") | 0 | g_frontendSelectedGameType=8 (orig 8=CopChase); →CAR_SELECTION (0x416B90, i+3=8) | none | Implemented (enum remap) | :6621 case 5 → port game_type=8 | Matches |
| 6 | SNK_BackButTxt ("Back", narrow w=0x70) | 0 | g_returnToScreenIndex = (g_networkLobbyEntryPhase==1 ? CREATE_SESSION(10) : MAIN_MENU(5)); →state5 (0x416BFA) | none; EscKeyButtonIndex=6 | Implemented | :6628 case 6 | Port: `s_network_active ? CREATE_SESSION : MAIN_MENU`. Matches (gate var renamed) |

BUTTONS — CUP SUB-PAGE (state 6 init; original order, all userdata=0):
| # | label | userdata | action (orig addr) | gating (orig `g_cheatFlagBitfieldGameModes`) | PORT status | port ref | gap |
|---|---|---|---|---|---|---|---|
| 0 | SNK_ChampionshipButTxt | 0 | state8: idx<6 → g_frontendSelectedGameType=idx+1=1; g_raceWithinSeriesIndex=0; ConfigureGameTypeFlags(); g_selectedScheduleIndex=g_attractModeTrackIndex; g_returnToScreenIndex=CAR_SELECTION; →state10 (0x417230) | always normal | Implemented | :6696 cup_type=1 | none |
| 1 | SNK_EraButTxt | 0 | idx+1=2 → game_type=2 (0x417230) | always normal | Implemented | :6697 cup_type=2 | none |
| 2 | SNK_ChallengeButTxt | 0 | game_type=3 | **Preview(greyed) if cheatGameModes==0; normal if >=1** | Implemented | :6651 gate / :6698 | Port gate var `s_cup_unlock_tier>=1`. Matches threshold |
| 3 | SNK_PitbullButTxt | 0 | game_type=4 | **Preview if cheatGameModes==0; normal if >=1** | Implemented | :6657 / :6702 | Matches |
| 4 | SNK_MastersButTxt | 0 | game_type=5 | **Preview unless cheatGameModes>=2** | Implemented | :6663 / :6706 | Matches |
| 5 | SNK_UltimateButTxt | 0 | game_type=6 | **Preview unless cheatGameModes>=2** | Implemented | :6669 / :6710 | Matches |
| 6 | SNK_BackButTxt (absolute pos) | 0 | state8 idx==6 → →state0xb (rebuild top page) (0x4172B6) | EscKeyButtonIndex=6 | Implemented | :6714 case 6 →state11 | none |

NON-BUTTON ELEMENTS:
| element | role/behavior | global reflected | PORT status | port ref | gap |
|---|---|---|---|---|---|
| Description box (0x110×0xb4) | g_lobbyErrorDialogSurface; on hover-change (state4/9) blits **localized** SNK_RaceTypeText[selectedGameType] title (large) + wrapped body (small font, y=0x20 step 0xc) | g_frontendSelectedGameType indexes SNK_RaceTypeText (orig 0x416C..) | **Wrong-source** | frontend_render_race_type_description :4158, dispatch :5522 | Port draws **hardcoded English** lines (k_race_type_lines/k_cup_lines), NOT the localized LANGUAGE.DLL SNK_RaceTypeText strings. Diverges from original text + breaks localization |
| Header/title label | CreateMenuStringLabelSurface(1) | g_currentScreenIndex | Implemented | title-tex :5783 | none |
| Button slide sprites slot0..6 | intro/outro MoveFrontendSpriteRect | sprite slots | Implemented | timed-animation | Anim positions approximated |

PARITY VERDICT: Partial — all 13 buttons (top 7 + cup 7, one shared Back) and full dispatch + gating are faithful (incl. enum-value remap and cheat-tier gating). The hover description panel text is the one real divergence: port renders fabricated English instead of localized SNK_RaceTypeText.
GAPS (actionable):
- Description preview uses hardcoded English text, not localized `SNK_RaceTypeText[game_type]` (LANGUAGE.DLL). Wrong content + no localization. (1 Wrong-behavior)
- Port `s_cup_unlock_tier` is the mapped equivalent of orig `g_cheatFlagBitfieldGameModes`; thresholds (>=1 / >=2) match — verify the source global is fed identically.
Confidence: [CONFIRMED @ 0x004168b0 state-3 switch 0x416B3D, state-8 switch 0x417230, cheatGameModes gating 0x417096-0x4171C7, SNK_RaceTypeText index expr] / [UNCERTAIN: whether s_cup_unlock_tier is sourced from the same cheat bitfield as orig 0x...GameModes]

---

### Screen 7 @ 0x004213d0 — ScreenQuickRaceMenu  [interactive: Y]
Port handler: td5_frontend.c:6762 (Screen_QuickRaceMenu, case TD5_SCREEN_QUICK_RACE)
DISPATCH MODEL: index-switch @0x00421A61 (state 4). Buttons 0/1 are **◄► selectors** (cycle car/track via g_postRaceRacerCardNavDirection, NOT a press-dispatch); buttons 2/3 are press buttons (OK/Back via g_frontendButtonPressedFlag). OK launches race (g_returnToScreenIndex = -1 sentinel → state6 → InitializeRaceSeriesSchedule).

BUTTONS (original order, all userdata=0):
| # | label | userdata | action (orig addr) | gating | PORT status | port ref | gap |
|---|---|---|---|---|---|---|---|
| 0 | SNK_ChangeCarButTxt ("Change Car", w=0x100 selector) | 0 | selector: on g_postRaceRacerCardNavDirection!=0 → g_quickRaceSelectedTrackId += dir (this is the CAR index here), wrap by g_savedMaxUnlockedCar (or 0x21/0x25 in cheat), redraw car name into panel (0x4218A2) | wrap bounds gated by g_cheatPostRaceHighScoreUnlock / g_cheatAttractModeOverride / DAT_00463e6d | Implemented | :6786 is_selector / :6816 cycle car | Port wraps via s_total_unlocked_cars (or 32 cheat / 36 net). Names differ but logic equivalent |
| 1 | SNK_ChangeTrackButTxt ("Change Track", selector) | 0 | selector: g_selectedScheduleIndex += dir, wrap by g_savedMusicTrackIndex (or 0x13 cheat), redraw track name (0x42191C) | cheat-gated wrap | Implemented | :6788 / :6827 cycle track | Port wraps via s_total_unlocked_tracks (or 0x13 net). Equivalent |
| 2 | SNK_OkButTxt ("OK") | 0 | press: if car/track locked && !cheat → DXSound::Play(10) reject; else g_selectedCarIndex=g_quickRaceSelectedTrackId; paint=0; wheel=0; g_returnToScreenIndex=-1 (~LOCALIZATION_INIT); →state5 (0x421A6E) | locked-content gate | Implemented | :6840 case 2 | none |
| 3 | SNK_BackButTxt ("Back") | 0 | press: g_selectedCarIndex=...; g_returnToScreenIndex=MAIN_MENU(5); →state5 (0x421B0C). EscKeyButtonIndex=3 | none | Implemented | :6855 case 3 | none |

NON-BUTTON ELEMENTS:
| element | role/behavior | global reflected | PORT status | port ref | gap |
|---|---|---|---|---|---|
| Info panel (0x208×200) | g_lobbyErrorDialogSurface; holds car name (top half), track name (bottom half), "Locked" caption when locked | g_quickRaceSelectedTrackId (car), g_selectedScheduleIndex (track) | Implemented | frontend_render_quick_race_overlay :3928, dispatch :5525 | Port draws car/track names + "LOCKED" live (no offscreen surface). Text source = port name tables, not the orig surface blit, but content correct |
| ◄► arrow sprites on btn 0/1 | InitializeFrontendDisplayModeArrows(0,1)/(1,1); ArrowButtonz cycle arrows | selector widgets | Implemented | fe_draw_option_arrows :3948, dispatch :5717 | none |
| Header/title label | CreateMenuStringLabelSurface(3) | g_currentScreenIndex | Implemented | title-tex :5783 | none |

PARITY VERDICT: Faithful — 2 selectors + OK + Back all correct, lock-gating on OK present, race-launch sentinel (-1) handled. Panel content drawn live instead of via offscreen surface (rendering-arch difference, content equivalent).
GAPS (actionable):
- None functional. Panel/name text comes from port tables vs orig localized surface blit (cosmetic/localization, same as other screens).
Confidence: [CONFIRMED @ 0x004213d0 state-4 dispatch 0x421A61, selector handling 0x42188C/0x42191C, OK launch sentinel 0x421A6E]

---

### Screen 8 @ 0x00418d50 — RunFrontendConnectionBrowser  [interactive: Y]  [ARCH-DIVERGENCE: DXPTYPE]
Port handler: td5_frontend.c:6888 (Screen_ConnectionBrowser, case TD5_SCREEN_CONNECTION_BROWSER)
DISPATCH MODEL: index-switch @0x004191D0 (state 5) for the 3 buttons; a SEPARATE list-navigation sub-state @0x00419240 (state 6) handles up/down scroll of the provider list (g_frontendInputEdgeBits 0x200=up / 0x400=down, mouse-row hit-test). Original enumerates DirectPlay connection providers (DXPlay::ConnectionEnumerate) into `dpu_exref+0x1e0` count, draws up to 4 rows into the panel surface.

BUTTONS (original order, all userdata=0):
| # | label | userdata | action (orig addr) | gating | PORT status | port ref | gap |
|---|---|---|---|---|---|---|---|
| 0 | SNK_ChooseConnectionButTxt ("Provider"/list panel, w=0x1f0 h=0x80) | 0 | press idx==0 → state6 (enter list-scroll mode) (0x4191E8) | none | Partial | :6896 "Provider" / :6926 case 0 | Port: idx0+arrow → sfx(2) only. NO scrollable provider list (state 6/7 collapsed to no-op → state5, :6944). DirectPlay enumeration absent |
| 1 | SNK_OkButTxt ("OK") | 0 | press idx==1 → g_returnToScreenIndex=9 (SESSION_PICKER); →state8 slide-out (0x4191F8) | none | Implemented | :6928 case 1 | none |
| 2 | SNK_BackButTxt ("Back") | 0 | press idx==2 → g_returnToScreenIndex=5 (MAIN_MENU); →state8 (0x419208). EscKeyButtonIndex=2 | none | Implemented | :6931 case 2 | none |

NON-BUTTON ELEMENTS:
| element | role/behavior | global reflected | PORT status | port ref | gap |
|---|---|---|---|---|---|
| Provider list rows (≤4) | DrawFrontendClippedStringToSurface from `dpu_exref + (scrollOff+i)*0x3c`, into panel surface (x=0x20, y step 0x10) | dpu_exref+0x1e0 (provider count), g_connBrowserScrollOffset | **Missing** | (no render case for screen 8 in :5520 switch) | Port draws no provider rows; the panel is an empty "Provider" button. No DirectPlay providers exist in port |
| Up/Down scroll arrows | g_browserScrollIndicatorSprite at panel origin +0xf2 / +0x1e (up if scrollOff!=0), +0x6c (down if scrollOff+4<count) | g_connBrowserScrollOffset, count | Missing | — | No list → no scroll indicators |
| Selection bar | g_browserSelectionBarSprite at row (cursor-scroll)*0x10+0x2f; blinks (anim&0x10) in state6 | g_connBrowserCursorIndex | Missing | — | No list → no selection bar |
| Header/title label | CreateMenuStringLabelSurface(5) | g_currentScreenIndex | Implemented | title-tex :5783 | none |

PARITY VERDICT: Partial (ARCH-DIVERGENCE) — OK/Back navigation faithful; the entire DirectPlay provider-list browser (rows, scroll arrows, selection bar, list-scroll sub-state 6) is absent because the port has no DXPTYPE peer enumeration. Button 0's press just plays a sound.
GAPS (actionable):
- No provider list, scroll indicators, or selection bar (3 Missing elements) — DXPTYPE ARCH-DIVERGENCE, documented.
- Button-0 list-scroll sub-state (orig state 6/7) collapsed to no-op (:6944). Document only.
Confidence: [CONFIRMED @ 0x00418d50 state-5 dispatch 0x4191D0, list-scroll 0x419240, DXPlay::ConnectionEnumerate @ case 0]

---

### Screen 9 @ 0x00419cf0 — RunFrontendSessionPicker  [interactive: Y]  [ARCH-DIVERGENCE: DXPTYPE]
Port handler: td5_frontend.c:6967 (Screen_SessionPicker, case TD5_SCREEN_SESSION_PICKER)
DISPATCH MODEL: index-switch @0x00419E80 (state 3) for 3 buttons (orig has NO separate "Create" button — slot0 panel + OK + Back). List-scroll handled in state 4 @0x00419F50 (same 0x200/0x400 edge model as screen 8). Sessions enumerated via RenderFrontendSessionBrowser into dpu_exref+0xbfc (session count).

BUTTONS (original order, all userdata=0):
| # | label | userdata | action (orig addr) | gating | PORT status | port ref | gap |
|---|---|---|---|---|---|---|---|
| 0 | SNK_ChooseSessionButTxt ("Session"/list panel, w=0x1f0 h=0x80) | 0 | press idx==0 → state4 (list-scroll mode) (0x419EA8) | none | Partial | :6973 "Session" / :6994 case 0 | Port: idx0+arrow → sfx(2). No scrollable session list (RenderFrontendSessionBrowser absent) |
| 1 | SNK_OkButTxt ("OK") | 0 | press idx==1 → `*(dpu_exref+0xc00)=cursorIdx-1` (selected session); g_returnToScreenIndex=CREATE_SESSION(10); DXPlay::EnumSessionTimer(0); →state6 (0x419EC0) | none | **Partial/Wrong-target** | :6996 case 1 "Create"→state4 / :6998 case 2 "OK"→CREATE_SESSION | Port adds a 4th "Create" button NOT in orig; OK is port's button index 2. Both route to CREATE_SESSION. Orig OK=join-selected-session (writes selected idx); port writes no session index (no list). Functionally collapses to "go create/lobby" |
| 2 | SNK_BackButTxt ("Back") | 0 | press idx==2 → g_returnToScreenIndex=CONNECTION_BROWSER(8); DXPlay::EnumSessionTimer(0); →state6 (0x419EE8). EscKeyButtonIndex=2 | none | Implemented | :7001 case 3 (port idx3) "Back" → CONNECTION_BROWSER | Matches target; port index shifted by the added Create button |

NON-BUTTON ELEMENTS:
| element | role/behavior | global reflected | PORT status | port ref | gap |
|---|---|---|---|---|---|
| Session list rows | RenderFrontendSessionBrowser() into panel surface (called state1/2/3/4) | dpu_exref+0xbfc (session count) | **Missing** | (no render case for screen 9 in :5520 switch) | Port draws no session rows |
| Up/Down scroll arrows | g_browserScrollIndicatorSprite at panel origin +0xf2/+0x1e (up), +0x6c (down) | g_connBrowserScrollOffset | Missing | — | No list |
| Selection bar | g_browserSelectionBarSprite at (cursor-scroll)*0x10+0x2f; blink anim&0x10 (state4) | g_connBrowserCursorIndex | Missing | — | No list |
| Header/title label | uses g_currentScreenIndex from screen 8 (not re-created here) | g_currentScreenIndex | Implemented | title-tex :5783 | none |

PARITY VERDICT: Partial/Divergent (ARCH-DIVERGENCE) — Back navigation faithful; OK collapses to "create/lobby" without selecting an enumerated session; the full session-list browser (rows, scroll, selection bar, RenderFrontendSessionBrowser) is absent (DXPTYPE). Port also adds a non-original "Create" button, shifting indices.
GAPS (actionable):
- Port adds a "Create" button (idx1) absent in the original 3-button (panel/OK/Back) layout — index shift; both Create and OK route to CREATE_SESSION. (1 Wrong-behavior: extra button + OK no longer joins a selected session)
- Session list rows / scroll arrows / selection bar absent (3 Missing) — DXPTYPE ARCH-DIVERGENCE.
- Orig OK writes selected session index (`dpu+0xc00 = cursor-1`) + stops EnumSessionTimer; port does neither (no list to select from).
Confidence: [CONFIRMED @ 0x00419cf0 state-3 dispatch 0x419E80, list-scroll 0x419F50, RenderFrontendSessionBrowser calls, dpu+0xc00/0xbfc session globals]

---

## Cross-screen summary
- Original dispatch model for ALL 5 screens = index-switch on `g_frontendButtonIndex` with `g_frontendButtonPressedFlag` edge (userdata arg is literal 0 everywhere → unused). Port's `s_button_index` switch is structurally faithful.
- Screens 5/6/7 = fully interactive single-player menus, near-faithful. Screens 8/9 = network browsers, deliberately gutted (DXPTYPE).
- The recurring REAL (non-divergence) gap: **Screen 6 description panel renders hardcoded English instead of localized SNK_RaceTypeText** — only genuine wrong-behavior bug found in the interactive trio.
