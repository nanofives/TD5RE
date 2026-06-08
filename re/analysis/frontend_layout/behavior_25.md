# Frontend BEHAVIOR Harvest — Slice 25 (g_frontendScreenFnTable indices 25–29)

Source: deterministic decompilation of `TD5_d3d.exe` via Ghidra (read_only, session reuse of an
already-open TD5_d3d.exe pool program). Companion to the POSITION harvest in `part_25.md`; this file
records button ACTIONS, dispatch model, non-button element behavior, and port parity.

Conventions / shared facts (all CONFIRMED at the cited addresses):
- `CreateFrontendDisplayModeButton @ 0x00425de0 (label, x, y, w, h, userdata)` — every call in these 5
  screens passes `userdata = 0`. There is NO per-button userdata dispatch anywhere in slice 25. The
  original selects the active button by INDEX (`g_frontendButtonIndex`) and signals a press via the
  global `g_frontendButtonPressedFlag` (single bool, set by the shared input/cursor layer, NOT a
  per-button callback pointer). So the dispatch model for the whole slice is **index/press-flag**, not
  userdata and not callback-ptr. The port mirrors this: `frontend_create_button(label,x,y,w,h)` (NO
  userdata param, td5_frontend.c:1315) + per-screen `switch(s_inner_state)` reading `s_input_ready`
  (td5_frontend.c:180, the port's `g_frontendButtonPressedFlag` analog).
- SNK_* labels are C++ mangled symbols (e.g. `?SNK_OkButTxt@@3PADA` @ 0x00460b96) → runtime pointers
  into LANGUAGE.DLL, not literals in the exe. English values are taken from the project-confirmed
  LANGUAGE.DLL SNK_ tables (see MEMORY.md keyboard-rebind note for the SNK→English provenance).
- `g_frontendButtonPressedFlag` press → on dialogs (26/27/29) navigates to `g_returnToScreenIndex`
  (26/27) or hard-coded MAIN_MENU (29). NameEntry(25) advances its own inner FSM.

g_frontendScreenFnTable @ 0x004655C4 indices 25–29:
25 → 0x00413bc0 ScreenPostRaceNameEntry
26 → 0x004237f0 ScreenCupFailedDialog
27 → 0x00423a80 ScreenCupWonDialog
28 → 0x00415370 ScreenStartupInit
29 → 0x0041d630 ScreenSessionLockedDialog

---

### Screen 25 @ 0x00413bc0 — ScreenPostRaceNameEntry  [interactive: Y]
Port handler: td5_frontend.c:9929 (case TD5_SCREEN_NAME_ENTRY, Screen_PostRaceNameEntry)
DISPATCH MODEL: index/press-flag @0x00413bc0 — 13-case inner FSM (g_frontendInnerState 0–0xC). Input
is a FREE-FORM TEXT WIDGET (NOT an alphabet grid), then a one-button OK confirm on the score table.

INPUT MODEL (the load-bearing behavioral finding): the name is captured by
`RenderFrontendCreateSessionNameInput @ 0x0041a530` (called every frame in case 2). It is a DirectInput
ANSI string capture: `DXInput::SetAnsi(1)` + `DXInput::GetString(&g_postRaceNameEditTargetPtr)` writing
directly into `g_postRacePlayerNameEntryBuffer` (max len 0x10/16, set at case 0 via
`_g_postRaceNameEditMaxLength=0x10`, `g_postRaceNameEditTargetPtr=&g_postRacePlayerNameEntryBuffer`).
There is NO on-screen letter grid and NO per-letter selection/confirm. A blinking caret is drawn by
filling a 2×0x10 green (0xff00) rect at the measured text-width x, gated on
`(g_frontendAnimFrameCounter & 0x20)==0` and `g_postRaceNameEditState!=2`. Confirm = the input layer
sets `g_postRaceNameEditState==2` (Enter), which case 2 detects → advances to slide-out (case 3) →
insert (case 4). If the entered buffer is empty (length wraps to -2 in the do/while strlen at case 2),
the code copies `g_localComputerName` into the name buffer as the default.

FSM flow: case0 qualification check → if not qualifying, jumps straight to case4 (silent insert,
score=0 path) → main menu; if qualifying, case1 slide-in → case2 type name → case3 slide-out → case4
insert into per-schedule high-score table (find pos, shift, write name/score/car/avg/top) → case5/6
build score-table surface via DrawPostRaceHighScoreEntry(g_selectedScheduleIndex) → case7 table
slide-in → case8/9/10 settle + wait OK → case0xB rebuild → case0xC slide-out → MAIN_MENU.

BUTTONS (original order):
| # | label | userdata | action | gating | PORT status | port ref | gap |
|---|---|---|---|---|---|---|---|
| 1 | SNK_EnterPlayerNameButTxt ("ENTER PLAYER NAME") | 0 | NOT a press target — it is the baked title strip of the name-input surface (CreateFrontendDisplayModeButton(-0x1c0,0,0x1c0,0x40); g_postRaceNameButtonSurfacePtr). Confirm comes from text-widget Enter, not a click. | case 0, only if score!=0 & not 2P & qualifies | Partial | td5_frontend.c:2499-2506 (live "ENTER PLAYER NAME" title) + 2470 frontend_render_text_input | Port draws title as live text above a live input box (not a baked button surface); functionally equivalent, cosmetically different. |
| 2 | SNK_OkButTxt ("OK") | 0 | Confirm score-table display → begin slide-out (case0xC) → MAIN_MENU. Selected by index (g_frontendButtonIndex=1); press via g_frontendButtonPressedFlag at case 10. | case 4 onward (score-table phase) | Implemented | td5_frontend.c:10162 (case 10 waits s_input_ready) | Port has no explicit "OK" button object for this phase — it advances on s_input_ready (any confirm). Faithful in effect. |
| – | (NULL backing strip) -0x208×0x20 | 0 | Pure visual backing rect behind OK (RebuildFrontendButtonSurface). Not interactive. | case 4 | Missing (cosmetic) | — | Port omits the 520×32 backing strip behind OK; minor visual. |

NON-BUTTON ELEMENTS:
| element | role/behavior | global reflected | PORT status | port ref | gap |
|---|---|---|---|---|---|
| Name text field + caret | DirectInput ANSI capture into 16-byte buffer; blinking caret | g_postRacePlayerNameEntryBuffer / g_postRaceNameEditState | Partial | td5_frontend.c:2410 frontend_handle_text_input_key (GetAsyncKeyState, not DXInput) | Port re-implements with Win32 GetAsyncKeyState + MapVirtualKey instead of DXInput::GetString; default-on-empty uses "PLAYER" (10022) not g_localComputerName (orig). |
| High-score table surface | 520×144 black surface, 5 rows + headers (NAME/BEST-TIME/LAP/PTS/CAR/AVG SPD/TOP SPD) drawn by DrawPostRaceHighScoreEntry @0x413010; highlighted row = g_postRaceQualifyingScore (the just-inserted index) using bold font g_smallTextbSurface | g_lobbyErrorDialogSurface / g_postRaceQualifyingScore | Partial | td5_frontend.c:4649 frontend_render_high_score_overlay (dispatched at 5563) | Port renders live and faithfully columns; BUT highlight is hard-coded row 0 (4716 `i==0`) instead of orig's inserted-position row (g_postRaceQualifyingScore). WRONG if score lands at rank 2–5. |
| Slide sprites (slot 0/1) | header/decoration sprites animated via MoveFrontendSpriteRect | sprite slots | Missing (cosmetic) | — | Port has no equivalent sprite-slide decoration; slide is abstracted to s_anim_tick. |
| Header label surface | g_currentScreenIndex screen-title strip (CreateMenuStringLabelSurface(7)) | g_currentScreenIndex | Partial | td5_frontend.c title-page path (730+) | Title page index handled but header-strip blit is abstracted. |

PARITY VERDICT: Partial — FSM, qualification math, table insert/shift, and confirm flow are faithfully
ported (cases 0/4 heavily CONFIRMED-annotated); divergences are (a) text widget re-implemented via
Win32 not DXInput, (b) empty-name default "PLAYER" vs orig g_localComputerName, (c) high-score highlight
row hard-coded to 0 instead of the inserted rank.
GAPS (actionable):
- Highlight WRONG: td5_frontend.c:4716 uses `i==0`; should highlight `s_score_insert_pos` row (orig
  g_postRaceQualifyingScore) so a rank-3 finish bolds row 3, not row 1.
- Empty-name default: orig copies g_localComputerName (machine name) when the field is left blank;
  port uses literal "PLAYER" (td5_frontend.c:10022). Cosmetic divergence.
- Input backend: orig DXInput::GetString ANSI; port GetAsyncKeyState. Behaviorally close; no IME / no
  non-A–Z0–9/space chars in port (td5_frontend.c:2434 loops VK 0x30–0x5A only).
Confidence: [CONFIRMED @ 0x00413bc0, sub 0x00413010, input widget 0x0041a530]. [UNCERTAIN: exact
LANGUAGE.DLL English for SNK_EnterPlayerNameButTxt — assumed "ENTER PLAYER NAME".]

---

### Screen 26 @ 0x004237f0 — ScreenCupFailedDialog  [interactive: Y]
Port handler: td5_frontend.c:10196 (case TD5_SCREEN_CUP_FAILED, Screen_CupFailed)
DISPATCH MODEL: index/press-flag @0x004237f0 — 6-case FSM; ONE button (OK). Dialog visible states 4–5.

BUTTONS (original order):
| # | label | userdata | action | gating | PORT status | port ref | gap |
|---|---|---|---|---|---|---|---|
| 1 | SNK_OkButTxt ("OK") @ x=-0x120 | 0 | case 5: if g_frontendButtonPressedFlag → release dialog surface + buttons → SetFrontendScreen(g_returnToScreenIndex). g_frontendEscKeyButtonIndex=0 (Esc also confirms). | dialog shown only if 0<gameType<7; else case0 → MAIN_MENU immediately | Implemented | td5_frontend.c:10218 (create) + 10236 (case 5 → MAIN_MENU on s_input_ready) | Port navigates to hard MAIN_MENU; orig goes to g_returnToScreenIndex. For this dialog return screen is initialized to MAIN_MENU's value at entry (frontend_init_return_screen), so equivalent in practice. |

NON-BUTTON ELEMENTS:
| element | role/behavior | global reflected | PORT status | port ref | gap |
|---|---|---|---|---|---|
| Dialog surface 408×112 | black-fill surface, 4 centered text lines | g_lobbyErrorDialogSurface | Implemented | td5_frontend.c:5339 frontend_render_cup_failed_overlay | Live-drawn quad+text vs baked surface; faithful position (cx-0xa8, cy-0x8f). |
| "SORRY" (y0) / "YOU FAILED" (y0x1c) / "TO WIN" (y0x38) | SNK_SorryTxt / SNK_YouFailedTxt / SNK_ToWinTxt | — | Implemented | td5_frontend.c:5359-5363 | Faithful y offsets 0/28/56. |
| cup-type name (y0x54) | *(SNK_RaceTypeText + gameType*4) → 1=CHAMPIONSHIP..6=ULTIMATE CUP | g_frontendSelectedGameType | Implemented | td5_frontend.c:5365-5368, k_cup_type_names[] | Faithful; cup names CONFIRMED from Language.dll. |
| slide sprite slot 0 | decoration slide-in | sprite slot 0 | Missing (cosmetic) | — | Port abstracts to s_anim_tick; no sprite. |

PARITY VERDICT: Faithful — single-OK dialog, gating (cup types 1–6 only), text lines, and confirm→exit
all match. Only cosmetic sprite-slide and baked-vs-live surface differ.
GAPS (actionable):
- Return target: port hard-codes MAIN_MENU (10239) vs orig g_returnToScreenIndex; harmless because
  this screen's return is always MAIN_MENU, but a literal-faithful port would use g_returnToScreenIndex.
Confidence: [CONFIRMED @ 0x004237f0]. [UNCERTAIN: none material.]

---

### Screen 27 @ 0x00423a80 — ScreenCupWonDialog  [interactive: Y]
Port handler: td5_frontend.c:10251 (case TD5_SCREEN_CUP_WON, Screen_CupWon)
DISPATCH MODEL: index/press-flag @0x00423a80 — 6-case FSM; ONE button (OK). Dialog 408×196.

SIDE EFFECTS at case 0 (CONFIRMED): `_unlink(g_cupDataTd5Filename)` deletes CupData.td5 (cup
progress wiped on win). Then loads MainMenu bg, allocs 0x198×0xc4 surface, draws text.

BUTTONS (original order):
| # | label | userdata | action | gating | PORT status | port ref | gap |
|---|---|---|---|---|---|---|---|
| 1 | SNK_OkButTxt ("OK") @ x=-0x120 | 0 | case 5: g_frontendButtonPressedFlag → release surface/buttons → SetFrontendScreen(g_returnToScreenIndex). Esc also confirms (g_frontendEscKeyButtonIndex=0). | dialog only if 0<gameType<7 | Implemented | td5_frontend.c:10313 (create) + 10330 (case 5 → MAIN_MENU on s_input_ready) | Same MAIN_MENU-vs-g_returnToScreenIndex note as 26. |

NON-BUTTON ELEMENTS:
| element | role/behavior | global reflected | PORT status | port ref | gap |
|---|---|---|---|---|---|
| Dialog surface 408×196 | black-fill, taller than 26/29 for extra lines | g_lobbyErrorDialogSurface | Implemented | td5_frontend.c:5394 frontend_render_cup_won_overlay | Faithful 0xc4 height. |
| "CONGRATULATIONS" (y0) / "YOU HAVE WON" (y0x38) | SNK_CongratsTxt / SNK_YouHaveWonTxt | — | Implemented | td5_frontend.c:5412-5414 | Faithful y 0/56. |
| cup-type name (y0x54) | *(SNK_RaceTypeText + gameType*4) | g_frontendSelectedGameType | Implemented | td5_frontend.c:5416-5419 | Faithful. |
| car-unlock line (y0x8c=140) | sprintf "%d %s" (count + SNK noun); gated on DAT_00494bb0 (car count) != 0 | DAT_00494bb0 | Partial | td5_frontend.c:5421-5424 ("%d NEW CARS UNLOCKED", s_cup_won_car_count) | Gating + count CONFIRMED-correct (disasm 0x423bb1 TEST/JZ on [0x494bb0]); only the SNK noun wording is UNCERTAIN. |
| track-unlock line (y0xa8=168) | sprintf "%d %s"; gated on DAT_00494bb4 (track count) != 0 | DAT_00494bb4 | Partial | td5_frontend.c:5426-5429 ("%d NEW TRACKS UNLOCKED", s_cup_won_track_count) | Same: count/gate correct, wording UNCERTAIN. |
| slide sprite slot 0 | decoration slide-in (settle y = ch+0x61 vs 26's ch+0x31) | sprite slot 0 | Missing (cosmetic) | — | No sprite in port. |

PARITY VERDICT: Faithful (functional) — FSM, CupData.td5 deletion (frontend_delete_cup_data
td5_frontend.c:10261), unlock-progression apply (10279), and the two count-gated unlock lines all
match the orig. The decompiler's `g_cupSchedule_currentCup/currentRound` variable names are MISNAMES;
disassembly proves the gates are on car/track counts ([0x494bb0]/[0x494bb4]) exactly as the port does.
GAPS (actionable):
- Unlock-line wording: orig `"%d %s"` with %s = SNK localized noun ([0x45d174]/[0x45d178]); port uses
  literal "NEW CARS UNLOCKED"/"NEW TRACKS UNLOCKED". Exact orig nouns UNCERTAIN (Language.dll not
  decompiled) — confirm against Language.dll for byte-exact wording.
- Return target MAIN_MENU vs g_returnToScreenIndex (same harmless note as 26).
Confidence: [CONFIRMED @ 0x00423a80; unlock gating disasm 0x423bb1/0x423c04]. [UNCERTAIN: SNK unlock-line
nouns at 0x0045d174 / 0x0045d178.]

---

### Screen 28 @ 0x00415370 — ScreenStartupInit  [interactive: N]
Port handler: td5_frontend.c:10343 (case TD5_SCREEN_STARTUP_INIT, Screen_StartupInit)
DISPATCH MODEL: index/press-flag @0x00415370 — 5-case bootstrap FSM. NOT interactive: the OK button it
creates is NEVER consumed (no g_frontendButtonPressedFlag check anywhere); case 4 self-advances by tick
count, releases the surface, and hands control to table[0] then re-seeds g_frontendInnerState with
timeGetTime(). It is a 4-tick blank-dialog transition placeholder, not a user prompt.

BUTTONS (original order):
| # | label | userdata | action | gating | PORT status | port ref | gap |
|---|---|---|---|---|---|---|---|
| 1 | SNK_OkButTxt ("OK") @ x=-0x120 (port -100) | 0 | INERT — created at case0 but never read; auto-advance ignores it. | always | Implemented (inert, matches orig inertness) | td5_frontend.c:10349 frontend_create_button("OK",-100,...) | Port uses x=-100/w=100 vs orig x=-0x120(-288)/w=0x60(96). Cosmetically off, but button is invisible/inert so impact = none. |

NON-BUTTON ELEMENTS:
| element | role/behavior | global reflected | PORT status | port ref | gap |
|---|---|---|---|---|---|
| Blank dialog surface 408×112 | black-fill box, NO text drawn, shown 3 ticks | g_lobbyErrorDialogSurface | Partial | td5_frontend.c:10353-10355 (frontend_present_buffer, no quad) | Orig blits a visible black box for 3 frames; port presents buffer without drawing the box. Sub-frame flash; negligible. |
| Control hand-off | case4: g_currentScreenFnPtr = table[0]; g_frontendInnerState = timeGetTime() | g_currentScreenFnPtr | Wrong-target | td5_frontend.c:10360 → TD5_SCREEN_LOCALIZATION_INIT | Orig jumps to table[0] (g_frontendScreenFnTable base = index 0). Port redirects to TD5_SCREEN_LOCALIZATION_INIT instead. [UNCERTAIN whether table[0] == LocalizationInit; if index 0 is a different screen this is a divergence.] |

PARITY VERDICT: Partial — bootstrap timing/inert-OK behavior matches, but the case-4 hand-off target
differs (orig table[0] vs port TD5_SCREEN_LOCALIZATION_INIT) and the visible blank box is dropped.
GAPS (actionable):
- Verify table[0] identity: orig sets g_currentScreenFnPtr = g_frontendScreenFnTable (index 0). Confirm
  whether port's index 0 == TD5_SCREEN_LOCALIZATION_INIT; if not, td5_frontend.c:10360 is a wrong target.
- Orig seeds g_frontendInnerState = timeGetTime() after the hand-off (an RNG/timing seed reuse of the
  inner-state field); port does not replicate this seed write. Likely inert but a literal divergence.
Confidence: [CONFIRMED @ 0x00415370]. [UNCERTAIN: identity of g_frontendScreenFnTable[0] vs the port's
TD5_SCREEN_LOCALIZATION_INIT redirect.]

---

### Screen 29 @ 0x0041d630 — ScreenSessionLockedDialog  [interactive: Y]
Port handler: td5_frontend.c:10378 (case TD5_SCREEN_SESSION_LOCKED, Screen_SessionLocked)
DISPATCH MODEL: index/press-flag @0x0041d630 — 6-case FSM; ONE button (OK). Structurally = screen 26
minus one text line; ALWAYS returns to MAIN_MENU (hard-coded, not g_returnToScreenIndex).

BUTTONS (original order):
| # | label | userdata | action | gating | PORT status | port ref | gap |
|---|---|---|---|---|---|---|---|
| 1 | SNK_OkButTxt ("OK") @ x=-0x120 | 0 | case 5: g_frontendButtonPressedFlag → release surface/buttons → SetFrontendScreen(TD5_SCREEN_MAIN_MENU). Esc confirms (g_frontendEscKeyButtonIndex=0). | always (dialog) | Implemented | td5_frontend.c:10392 (create) + 10409 (case 5 → MAIN_MENU on s_input_ready) | Faithful — orig itself hard-codes MAIN_MENU here. |

NON-BUTTON ELEMENTS:
| element | role/behavior | global reflected | PORT status | port ref | gap |
|---|---|---|---|---|---|
| Cached full-canvas bg blit | case0: BlitFrontendCachedRect(0,0,W,H) | cached bg | Partial | td5_frontend.c:10383 frontend_load_tga | Port reloads MainMenu.tga instead of blitting cached rect; visual result equivalent. |
| Dialog surface 408×112 | black-fill, 2 text lines | g_lobbyErrorDialogSurface | Implemented | td5_frontend.c:5441 frontend_render_session_locked_overlay | Faithful pos/size. |
| "SORRY" (y0) / "SESSION LOCKED" (y0x38) | SNK_SorryTxt / SNK_SeshLockedTxt | — | Implemented | td5_frontend.c:5455-5457 | Faithful y 0/56. |
| slide sprite slot 0 | decoration slide-in | sprite slot 0 | Missing (cosmetic) | — | No sprite in port. |

PARITY VERDICT: Faithful — identical FSM to 26, two-line text, OK→MAIN_MENU. Note (ARCH-DIVERGENCE,
documented at td5_frontend.c:10370): in the port this screen is reachable only by manually setting the
kicked flag because no compatible DXPTYPE peer exists; the FSM itself is faithful.
GAPS (actionable):
- None functional. Cosmetic: cached-rect bg vs tga reload; missing decoration sprite.
Confidence: [CONFIRMED @ 0x0041d630]. [UNCERTAIN: none material; SNK_SeshLockedTxt English assumed
"SESSION LOCKED".]
