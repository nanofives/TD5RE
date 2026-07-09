# Frontend Screens 25–29 — Complete Per-Screen Element + Behavior Spec

RE target: `TD5_d3d.exe` (image base `0x00400000`). All addresses are Ghidra VAs.
Source: literal decompilation of each screen fn + every helper it calls, grounded in the
engine model (`frontend_rendering_model.md`, `frontend_flow_model.md`,
`frontend_call_graph_closure.md`). Values hex AND decimal.

**Cross-screen drawing model (applies to all 5; from rendering_model PART 1):** a screen fn
only PRIMES — it bakes label/panel/button surfaces, builds slot-table entries, and per frame
calls `QueueFrontendOverlayRect`/`MoveFrontendSpriteRect`. The actual on-screen pixels are
emitted by the per-frame **FLUSH** (`FlushFrontendSpriteBlits 0x00425540`, called by the LOOP
after the screen fn) which drains the overlay/sprite queues into `g_primaryWorkSurface`, and
unconditionally runs the two DECOUPLED draws (`UpdateExtrasGalleryDisplay 0x0040d830` +
`RenderFrontendDisplayModeHighlight 0x004263e0`). None of screens 25–29 calls those two decoupled
draws itself, but **both run every frame on every screen** — the gallery cross-fade no-ops here
(`g_extrasGallerySlideSurfaces`/transition!=2 gate), and the selection-highlight bars draw the
4-edge outline around the active OK button whenever `g_frontendOverlayRectArrayTail != -1` and
the cursor overlay is in Deactivate(==0) state. That highlight + the LOOP-queued mouse cursor are
the only flush-decoupled elements that actually produce pixels on these dialog screens.

Common geometry: `uVar13 = g_frontendCanvasW>>1` (=320 on 640×480), `uVar8 = g_frontendCanvasH>>1`
(=240). All dialog panels are queued centered; dialog surface is the shared
`g_lobbyErrorDialogSurface` tracked surface.

---

### Screen 25 @0x00413bc0 — ScreenPostRaceNameEntry  [interactive Y]
Inner states (`switch(g_frontendInnerState)`):
- **0** = qualify-check + init (decides if player even gets a name-entry; bakes header + name button).
- **1** = slide-in (header + name-input button slide on; counter target `0x20`=32).
- **2** = INTERACTIVE name edit (runs `RenderFrontendCreateSessionNameInput`; waits for edit-state==2 = ENTER).
- **3** = slide-out of the name button (counter `0x20`=32), then releases button + name surface.
- **4** = insert score into the per-schedule high-score table (array shuffle + write the new row), build the 0x208×0x90 results-table dialog surface + OK button.
- **5/6** = present + reset anim, set racer-card index 0, call `DrawPostRaceHighScoreEntry` (bakes the table rows).
- **7** = slide-in of the high-score table dialog (two sprite slots; counter `0x27`=39 → settle gate `AdvanceFrontendTickAndCheckReady`).
- **8/9** = static present of table dialog.
- **10 (0xa)** = INTERACTIVE: wait for OK press → set return screen 5, blit secondary→primary, advance.
- **0xb** = restore bg + re-color-key text surfaces (key 0xffffff), play sound.
- **0xc (12)** = slide-out (header + 2 dialog slots; counter `0x10`=16) → release all → `SetFrontendScreen(MAIN_MENU)`.
- `joined_r0x004145bf` (fall-through from state 0 when NOT qualifying, and tail of 0xc): zero schedule index for game-type 1..7, `SetFrontendScreen(TD5_SCREEN_MAIN_MENU)`.

ELEMENTS:
| element | produced by (fn/global/asset) | INLINE/FLUSH | dest pos | gating | notes |
|---|---|---|---|---|---|
| Full-screen menu background | `LoadTgaToFrontendSurface16bpp(s_Front_End_MainMenu_tga, FrontEnd.zip)` (state 0) → `g_primaryWorkSurface`; copied to secondary | INLINE | full 640×480 | only if qualifies | MainMenu.tga |
| Header label "ENTER PLAYER NAME" surface | `CreateMenuStringLabelSurface(7)` → `g_currentScreenIndex` (state 0); fed `SNK_Options_MT` inline-string table | INLINE bake | — | qualifies | label surface id reused as the slide-in header |
| Header label on screen | `QueueFrontendOverlayRect(anim-driven X, …, g_menuHeaderLabelSurfaceWidth/Height, g_currentScreenIndex)` every interactive/anim state | FLUSH (overlay drain) | states 1–0xc, X animates | — | state1 X=`anim*0x10-0x1f6+iVar14`; states 2–0xb X=`uVar13-200`(=120) |
| Name-input button (label "ENTER PLAYER NAME") | `CreateFrontendDisplayModeButton(SNK_EnterPlayerNameButTxt,-0x1c0,0,0x1c0,0x40,0)` → `g_postRaceNameButtonSurfacePtr_PROVISIONAL` (state 0); width 0x1c0=448 h 0x40=64 | INLINE bake; FLUSH on screen | slot 0, slid via `MoveFrontendSpriteRect` | qualifies | the box that holds the text field |
| Name-edit text field (live string) | `RenderFrontendCreateSessionNameInput 0x0041a530`: `BltColorFillToSurface` field bg + `DrawFrontendClippedStringToSurface(g_postRaceNameEditTargetPtr,…)` into the button surface | INLINE bake (per frame) | inside name button, x=0x14 | state 2 only | text = `g_postRacePlayerNameEntryBuffer` (16-char, `_g_postRaceNameEditMaxLength=0x10`) |
| Blinking caret | `RenderFrontendCreateSessionNameInput`: `BltColorFillToSurface(0xff00, textwidth+0x14, …, 2,0x10,…)` — 2×16 green bar after the text | INLINE bake | after last glyph | `buttonIndex==0 && (anim & 0x20)==0 && editState!=2` | blink = bit5 of `g_frontendAnimFrameCounter`; suppressed if text width ≥0x195 |
| Field highlight color | same fn: focused (`buttonIndex==0`) field bg = `0x392152`/offset 0x68; unfocused = `0`/offset 0x28; toggles each call via `g_connBrowserRedrawDirty&1` | INLINE bake | y=0x14 | — | gives the field its hilite when selected |
| High-score table dialog panel | `g_lobbyErrorDialogSurface = CreateTrackedFrontendSurface(0x208,0x90)` + `BltColorFillToSurface(0,…)` (state 4); 520×144 | INLINE bake | centered | post-edit | re-uses shared dialog surface var |
| High-score table CONTENT (headers + 5 ranked rows) | `DrawPostRaceHighScoreEntry(g_selectedScheduleIndex) 0x00413010` (state 5/6) bakes into `g_lobbyErrorDialogSurface` | INLINE bake; FLUSH on screen | see below | post-edit | full table layout, detailed in its own row group |
| Table dialog on screen | `QueueFrontendOverlayRect(…,0x208,0x90,g_lobbyErrorDialogSurface)` states 7–0xc | FLUSH | center, anim X | — | second sprite slot too (states 7/0xc move slot 0+1) |
| OK button | `CreateFrontendDisplayModeButton(SNK_OkButTxt,-0x130,0,0x60,0x20,0)` (state 4); plus a blank 0x208×0x20 spacer button `CreateFrontendDisplayModeButton(0,-0x208,0,0x208,0x20,0)` then `RebuildFrontendButtonSurface(0)` | INLINE bake; FLUSH | slot 1 | post-edit | `g_frontendButtonIndex=1; g_frontendEscKeyButtonIndex=1` |
| Selection highlight bars (around OK) | `RenderFrontendDisplayModeHighlight 0x004263e0` (4 edge bars 0xc000 into `g_frontendBackSurfacePtr`) | FLUSH (decoupled) | around active slot | `tail!=-1 && cursorOverlay==0` | runs every frame |
| Mouse cursor sprite | LOOP `QueueFrontendOverlayRect(mouseX,mouseY,…,g_frontendCursorTextureId)` | FLUSH | mouse pos | `cursorOverlayHidden==0 && mouseEnabled` | snkmouse.tga 22×30 |

**DrawPostRaceHighScoreEntry 0x00413010 (table content baker) — sub-elements (all INLINE bake into `g_lobbyErrorDialogSurface`, small font `g_smallTextSurface`):**
- Clears panel (`BltColorFillToSurface 0,0,0,0x208,0x90`).
- Column headers via `DrawFrontendSmallFontStringToSurface`+`MeasureOrCenterFrontendString` at fixed X-bands: `SNK_NameTxt`(x 0x10..0x70), `SNK_CarTxt`(0xe4..0x150), `SNK_AvgTxt`(0x160..0x1ac), `SNK_TopTxt`(0x1bc..0x208), `SNK_SpdTxt`×2 (under Avg+Top). Middle column header is **mode-dependent** on `g_npcRacerGroupTable[sched*0xa4]&3`: ==0 → `SNK_TimeTxt`; ==1 → `SNK_LapTxt`; ==2 → `SNK_PtsTxt`. `SNK_BestTxt` drawn at top (0x80..0xd4) unless mode==2.
- 5 ranked rows (loop `uVar3`=0..4, rowY `iVar4` 0x30 step 0x10): rank number (`sprintf` template), **inserted-rank highlight** — `if (rowIndex == g_postRaceQualifyingScore) g_smallFontSurface = g_smallTextbSurface; else g_smallTextSurface` (the player's just-inserted row drawn in the alt/bold font `SmallTextb`). Name via `DrawFrontendClippedStringToSurface(row-0x10,0x10,…,clip 0x60)`. Middle col = time/pts formatted (`%2.2d:%2.2d.%3.3d` for time mode `s_..._00465470`, `%2.2d:%2.2d.%2.2d` `s_..._00465484`, or sprintf for pts). Car = manufacturer string `(&g_localizationCarManufScratch)[gExtCarIdToTypeIndex[row.car]*0xcc]`. Avg+Top speed `%dMPH`/`%dKPH` per `gSpeedReadoutUnitsConfigShadow`.

Primed contract globals: `g_postRaceNameEditTargetPtr=&g_postRacePlayerNameEntryBuffer`,
`_g_postRaceNameEditMaxLength=0x10`(16), `g_postRaceNameEditState`(@0x004969d0; 1=editing,
2=ENTER-committed — written to 2 by `DXInput::GetString` middleware), `g_postRaceQualifyingScore`
(the score/time used both as the qualify gate AND, after state 4, repurposed as the inserted-row
rank index consumed by the highlight), `g_selectedScheduleIndex` (=0x13 for time-trial, else
gametype+0x13), `g_currentScreenIndex`(header label surface), `g_lobbyErrorDialogSurface`(table
panel), `g_returnToScreenIndex`(=0 after edit, =5 after OK), `g_frontendEscKeyButtonIndex`
(-1 during edit, 1 at OK), `g_frontendButtonIndex=1`. `g_connBrowserRedrawDirty` flips field hilite.

Animation: pure `g_frontendAnimFrameCounter`-driven (frame-count, not ms). State1 slide-in fires at
counter `0x20`(32) (`DXSound::Play(4)`); state3 slide-out at `0x20`(32); state7 table slide-in
holds at `0x27`(39) gated by 3-frame `AdvanceFrontendTickAndCheckReady`; state0xc slide-out fires at
`0x10`(16). Caret blink = `g_frontendAnimFrameCounter & 0x20` (on/off every 32 frames). Slide
positions are linear `base ± counter*step` (steps 0x10/0x18/0x20/0x30/0x38/4/6).

Conditional elements: the WHOLE name-entry sub-flow (states 0–3) only runs if
`g_postRaceQualifyingScore != 0` AND `g_raceParticlePoolBase[0x1f2].size_rate != 2` AND
`g_twoPlayerModeEnabled==0`. Qualify computation (state 0) branches on mode bits
`g_npcRacerGroupTable[sched*0xa4]&3`: 0=points-vs-threshold, 1=min-lap-scan (`0x2b818` init,
scans pool), 2=lap_time_total≤threshold. For gametype<1, also gated off unless special-encounter
unlock flags set. If not qualifying → skips straight to score-insert path / main menu. Inserted-row
highlight (alt font) only on the player's row. Speed unit MPH/KPH per config shadow. Middle column
header/value differ per mode (Time/Lap/Pts).

Input dispatch: state 2 — name chars come from `DXInput::GetString(&g_postRaceNameEditTargetPtr)`
(only when `g_frontendButtonIndex==0`; `DXInput::SetAnsi(1)` enables it) — an OS/middleware edit
box writing into `g_postRacePlayerNameEntryBuffer`; ENTER sets edit-state→2 (boundary). On commit,
if the entered name is empty (len test `iVar9==-2`) it copies `g_localComputerName` as the default.
State 0xa — `if (g_frontendButtonPressedFlag != 0)` (OK / ESC→index1) commits and advances.
NOT alphabet-grid: it is the DXInput string-input model.

Confidence: [CONFIRMED @ 0x00413bc0, 0x00413010, 0x0041a530, edit-state @0x004969d0, fmt @0x004660e0/0x00465460–84].
[UNCERTAIN: exact transition site that sets `g_postRaceNameEditState=2` — it is inside the external
`DXInput::GetString` (middleware boundary), not in any decompiled frontend fn; inferred from the
`tagSTRINGSTRUCT` passed by address. Missing evidence: DXInput::GetString body (import).]

---

### Screen 26 @0x004237f0 — ScreenCupFailedDialog  [interactive Y]
Inner states: **0** = guard (only for cup game-types 1..6, else straight to main menu) + load bg, bake dialog panel + 4 message lines + OK button. **1/2/3** = present + reset anim (3-frame settle). **4** = slide-in (counter `0x20`=32 → Deactivate cursor). **5** = static + wait OK press → release + `SetFrontendScreen(g_returnToScreenIndex)`.

ELEMENTS:
| element | produced by | INLINE/FLUSH | dest pos | gating | notes |
|---|---|---|---|---|---|
| Full-screen bg | `LoadTgaToFrontendSurface16bpp(MainMenu.tga,FrontEnd.zip)`→primary, copy to secondary (state 0) | INLINE | full | gametype 1..6 | |
| Dialog panel | `g_lobbyErrorDialogSurface = CreateTrackedFrontendSurface(0x198,0x70)` + `BltColorFillToSurface(0,…)`; 408×112 | INLINE bake | centered (uVar1-0xa8, uVar2-0x8f)=(152,97) | — | |
| Message text (4 lines, centered) | `MeasureOrCenterFrontendLocalizedString` + `DrawFrontendLocalizedStringToSurface` into panel: `SNK_SorryTxt`, `SNK_YouFailedTxt`, `SNK_ToWinTxt`, then race-type name `*(SNK_RaceTypeText + gametype*4)` | INLINE bake | within 0x198-wide panel | — | line 4 = the failed cup's name |
| Dialog on screen | `QueueFrontendOverlayRect(…,0x198,0x70,g_lobbyErrorDialogSurface)` states 4/5 | FLUSH | center, anim X (state4) | states ≥4 only | state4 X=`uVar1+anim*-0x18+600`; state5 X=`uVar1-0xa8` |
| OK button | `CreateFrontendDisplayModeButton(SNK_OkButTxt,-0x120,0,0x60,0x20,0)`; slot 0 slid via `MoveFrontendSpriteRect` | INLINE bake; FLUSH | slid in | — | `g_frontendEscKeyButtonIndex=0` |
| Selection highlight + cursor | decoupled `RenderFrontendDisplayModeHighlight` + LOOP cursor | FLUSH | around OK / mouse | per gates | every frame |

Primed contract globals: `g_lobbyErrorDialogSurface`, `g_frontendEscKeyButtonIndex=0`,
`g_returnToScreenIndex` (exit target), `g_frontendSelectedGameType` (1..6 gate + race-type label
index). No trophy/medal art — failed dialog is text-only (NO flush/inline art element).

Animation: `g_frontendAnimFrameCounter`-driven; slide-in commits at `0x20`(32). No fade/scale.

Conditional elements: entire dialog gated on `0 < g_frontendSelectedGameType < 7` (cup modes);
otherwise immediate `SetFrontendScreen(MAIN_MENU)`. Line 4 text varies by which cup failed.
No auto-dismiss timer — waits for button.

Input dispatch: state 5 — `if (g_frontendButtonPressedFlag != 0)` (OK click or ESC→index0)
→ release surface+buttons → `SetFrontendScreen(g_returnToScreenIndex)`.

Confidence: [CONFIRMED @ 0x004237f0; SNK_RaceTypeText indirection; panel 0x198×0x70].

---

### Screen 27 @0x00423a80 — ScreenCupWonDialog  [interactive Y]
Inner states: identical shape to 26. **0** = guard (cup types 1..6) + `_unlink(g_cupDataTd5Filename)` (deletes the saved cup so it can't be re-won) + bg + bake taller panel + message lines (incl. conditional unlock lines) + OK. **1/2/3** = present/settle. **4** = slide-in (`0x20`=32 → Deactivate). **5** = static + OK → exit.

ELEMENTS:
| element | produced by | INLINE/FLUSH | dest pos | gating | notes |
|---|---|---|---|---|---|
| Full-screen bg | `LoadTgaToFrontendSurface16bpp(MainMenu.tga,FrontEnd.zip)` + copy secondary | INLINE | full | gametype 1..6 | |
| Dialog panel | `CreateTrackedFrontendSurface(0x198,0xc4)` + fill; 408×**196** (taller than the failed dialog's 0x70=112 — room for unlock lines) | INLINE bake | center (uVar1-0xa8,uVar2-0x8f) | — | |
| Message text (3 fixed lines) | `MeasureOrCenterFrontendLocalizedString`+`DrawFrontendLocalizedStringToSurface`: `SNK_CongratsTxt`, `SNK_YouHaveWonTxt`, race-type name `*(SNK_RaceTypeText+gametype*4)` | INLINE bake | in panel | — | |
| **Unlock-announcement line 1** | `sprintf_game(local_40, s__d__s_004660e0 ="%d %s")` → `MeasureOrCenterFrontendLocalizedString`+draw | INLINE bake | in panel | `if (g_cupSchedule_currentCup != 0)` | "%d %s" = number + name (the unlocked cup/car/track) |
| **Unlock-announcement line 2** | same `"%d %s"` sprintf + draw | INLINE bake | in panel | `if (g_cupSchedule_currentRound != 0)` | second unlock line |
| Dialog on screen | `QueueFrontendOverlayRect(…,0x198,0xc4,g_lobbyErrorDialogSurface)` states 4/5 | FLUSH | center, anim X | states ≥4 | |
| OK button | `CreateFrontendDisplayModeButton(SNK_OkButTxt,-0x120,0,0x60,0x20,0)`; `MoveFrontendSpriteRect` slot0 | INLINE bake; FLUSH | slid in | — | `EscKeyButtonIndex=0` |
| Selection highlight + cursor | decoupled highlight + LOOP cursor | FLUSH | per gates | — | |

Primed contract globals: `g_lobbyErrorDialogSurface`, `g_frontendEscKeyButtonIndex=0`,
`g_returnToScreenIndex`, `g_frontendSelectedGameType`, `g_cupSchedule_currentCup`,
`g_cupSchedule_currentRound` (gate the two unlock lines), `g_cupDataTd5Filename` (deleted).
**No trophy/medal/cup ART** — the "won" content is purely text (the two `"%d %s"` unlock lines);
there is no flush or inline image element for a trophy. [UNCERTAIN] the `local_40` sprintf passes
only the `"%d %s"` template literal in the decompile (varargs not recovered) — the actual number +
string args are the unlocked-item id/name, but their source globals were not resolved in this body.
Missing evidence: the variadic args to the two `sprintf_game` calls.

Animation: counter-driven, slide-in at `0x20`(32). No fade/scale.

Conditional elements: whole dialog gated `0 < gametype < 7`. Unlock line 1 only if currentCup!=0;
line 2 only if currentRound!=0 (so 3, 4, or 5 message lines total → the taller 0xc4 panel). No timer.

Input dispatch: state 5 — `if (g_frontendButtonPressedFlag != 0)` → release → `SetFrontendScreen(g_returnToScreenIndex)`.

Confidence: [CONFIRMED @ 0x00423a80; fmt "%d %s" @0x004660e0; panel 0x198×0xc4].

---

### Screen 28 @0x00415370 — ScreenStartupInit  [interactive N]
Inner states: **0** = bake a blank dialog panel + OK button (a transient placeholder/bootstrap dialog).
**1/2/3** = queue the (empty) panel for 3 frames (reset anim each). **4** = release panel+button,
reset inner-state/anim, and hand off: `g_currentScreenFnPtr = g_frontendScreenFnTable` (= entry 0,
ScreenLocalizationInit @0x004269d0) with `g_frontendInnerState = timeGetTime()`. This is the
bootstrap/transition screen: it stalls a few frames with an empty centered box then jumps to the
real first screen (idx 0).

ELEMENTS:
| element | produced by | INLINE/FLUSH | dest pos | gating | notes |
|---|---|---|---|---|---|
| Blank dialog panel | `g_lobbyErrorDialogSurface = CreateTrackedFrontendSurface(0x198,0x70)` + `BltColorFillToSurface(0,…)` (state 0) | INLINE bake | (cW>>1)-0xa8,(cH>>1)-0x8f = (152,97) | — | 408×112, never filled with text |
| Panel on screen | `QueueFrontendOverlayRect(…,0x198,0x70,g_lobbyErrorDialogSurface)` states 1–3 | FLUSH | centered, NO animation | — | static (no anim X) |
| OK button | `CreateFrontendDisplayModeButton(SNK_OkButTxt,-0x120,0,0x60,0x20,0)` (state 0) | INLINE bake; FLUSH | slot 0 | — | `g_frontendEscKeyButtonIndex=0` — but never read (no interactive state) |
| Selection highlight + cursor | decoupled highlight + LOOP cursor | FLUSH | per gates | `ActivateFrontendCursorOverlay()` called | runs but auto-advances |

Primed contract globals: `g_lobbyErrorDialogSurface`, `g_frontendEscKeyButtonIndex=0`,
`g_currentScreenFnPtr` (set to table[0] at hand-off), `g_frontendInnerState` (re-stamped to
`timeGetTime()` = the screen-enter timestamp role). No bg image load, no text lines.

Animation: none (states 1–3 just re-queue + zero `g_frontendAnimFrameCounter`); it is a 4-frame
auto-advancing pass, NOT a timed slide. No fade/scale.

Conditional elements: none — unconditional 0→1→2→3→4 march. NO message text, NO art. (It is
effectively a one-time bootstrap that shows an empty box then dispatches to ScreenLocalizationInit.)

Input dispatch: NONE — no input read; no `g_frontendButtonPressedFlag` check. State 4 hands off
unconditionally. (Despite an OK button being created, no state consumes a press.)

Confidence: [CONFIRMED @ 0x00415370; hand-off `g_currentScreenFnPtr=g_frontendScreenFnTable` = idx0].

---

### Screen 29 @0x0041d630 — ScreenSessionLockedDialog  [interactive Y]
Inner states: **0** = load bg + restore cached rect + bake panel + 2 message lines ("Sorry / Session Locked") + OK. **1/2/3** = present/settle. **4** = slide-in (`0x20`=32 → Deactivate cursor). **5** = static + OK → `SetFrontendScreen(TD5_SCREEN_MAIN_MENU)` (hardwired to main menu, unlike 26/27 which use `g_returnToScreenIndex`).

ELEMENTS:
| element | produced by | INLINE/FLUSH | dest pos | gating | notes |
|---|---|---|---|---|---|
| Full-screen bg | `LoadTgaToFrontendSurface16bpp(MainMenu.tga,FrontEnd.zip)`→primary, copy secondary, `BlitFrontendCachedRect(0,0,cW,cH)` | INLINE | full | — | extra cached-rect restore vs 26/27 |
| Dialog panel | `CreateTrackedFrontendSurface(0x198,0x70)` + fill; 408×112 | INLINE bake | center (uVar1-0xa8,uVar2-0x8f)=(152,97) | — | |
| Message text (2 lines) | `MeasureOrCenterFrontendLocalizedString`+`DrawFrontendLocalizedStringToSurface`: `SNK_SorryTxt`, `SNK_SeshLockedTxt` | INLINE bake | in 0x198 panel | — | "Sorry" / "Session Locked" |
| Dialog on screen | `QueueFrontendOverlayRect(…,0x198,0x70,g_lobbyErrorDialogSurface)` states 4/5 | FLUSH | center, anim X (state4) | states ≥4 | |
| OK button | `CreateFrontendDisplayModeButton(SNK_OkButTxt,-0x120,0,0x60,0x20,0)`; `MoveFrontendSpriteRect` slot0 | INLINE bake; FLUSH | slid in | — | `EscKeyButtonIndex=0` |
| **Lock art** | — | — | — | — | NONE: no medal/lock image element — text-only ("locked" conveyed by string). |
| Selection highlight + cursor | decoupled highlight + LOOP cursor | FLUSH | per gates | — | |

Primed contract globals: `g_lobbyErrorDialogSurface`, `g_frontendEscKeyButtonIndex=0`. NO
`g_returnToScreenIndex` use — exit is hardwired `SetFrontendScreen(TD5_SCREEN_MAIN_MENU)`. No
kicked-flag is read inside this fn; the "session locked" condition is decided by whatever net-flow
screen jumped here (this screen just shows the message). [UNCERTAIN] no kicked/lock state global is
referenced in this body; the dialog is unconditional once entered. Missing evidence: the caller
that performs the lock check and `SetFrontendScreen(29)` (a net-lobby screen, idx 8–11).

Animation: counter-driven; slide-in commits at `0x20`(32). No fade/scale, no auto-dismiss timer.

Conditional elements: none inside the fn (always shows both lines + OK). Differs from 26/27 only in
text content, the extra `BlitFrontendCachedRect`, and the hardwired main-menu exit.

Input dispatch: state 5 — `if (g_frontendButtonPressedFlag != 0)` (OK / ESC→index0) → release →
`SetFrontendScreen(MAIN_MENU)`.

Confidence: [CONFIRMED @ 0x0041d630; SNK_SorryTxt + SNK_SeshLockedTxt; hardwired main-menu exit].
