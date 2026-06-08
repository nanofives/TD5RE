# Frontend Behavior + Port-Parity Harvest — Part 20 (table indices 20–24)

Source: `TD5_d3d.exe` (image base 0x00400000), Ghidra read-only session (reused open pool10 session for TD5_d3d.exe — slot lock contention on fresh acquire). Port = `td5mod/src/td5re/td5_frontend.c`.

DISPATCH-MODEL NOTE (all 5 screens): the original frontend has NO per-button userdata callback. `CreateFrontendDisplayModeButton @0x00425de0(label,x,y,w,h,ud)` is always called with `ud=0`; the screen FSM dispatches on `g_frontendButtonIndex` (the focused/clicked button, by CREATION ORDER) and `g_postRaceRacerCardNavDirection` (◄►/arrow nav = the global `DAT_0049b690`, ±1). The port mirrors this exactly: `frontend_create_button(label,x,y,w,h)` (NO ud param), and an `s_button_index` / `frontend_option_delta()` (reads `s_arrow_input` → ±1) switch. So "userdata" below = "N/A — index dispatch" for every button on both sides. `CreateFrontendMenuRectEntry @0x004258f0` is NOT used by any of these 5 screens.

LABEL NOTE: button labels are `SNK_*ButTxt` C++ symbol exrefs resolved at runtime from LANGUAGE.DLL (English text not in the exe). Port hardcodes English literals. Where the literal differs from the symbol stem it is flagged.

---

### Screen 20 @ 0x0040dfc0 — CarSelectionScreenStateMachine  [interactive: Y]
Port handler: td5_frontend.c:8500 (Screen_CarSelection, registered td5_frontend.c:111 case TD5_SCREEN_CAR_SELECTION)
DISPATCH MODEL: index-switch @0x0040e6xx (case 7) — buttons created in state 4 @0x0040e0c8; interaction reads g_frontendButtonIndex 0..5 + g_postRaceRacerCardNavDirection. Working car index lives in the SHARED scratch global g_quickRaceSelectedTrackId (NOT the track), committed to g_selectedCarIndex/g_player1Selected* only at state 0x18/0x1a.

BUTTONS (creation order, state 4 @0x0040e0c8):
| # | label (SNK / English literal) | userdata | action | gating | PORT status | port ref | gap |
|---|---|---|---|---|---|---|---|
| 0 | SNK_CarButTxt / "Car" | N/A index | ◄► cycles working car index (g_quickRaceSelectedTrackId += dir); Masters(gt5) skips taken slots; wrap per game-type (gt2 0..0xf, gt8 0x21..0x24, else 0..maxUnlocked); →state 10 reload preview | always | Implemented | td5_frontend.c:8670 (case 0) | Faithful: roster bounds + Masters skip + wrap replicated (:8552-8585, :8670-8688) |
| 1 | SNK_PaintButTxt / "Paint" | N/A index | ◄► cycles g_carSelectPaintSchemeTransient 0..3; INERT when car is cop (0x1c..0x24); →state 10 | always | Implemented | td5_frontend.c:8690 (case 1) | Faithful (cop-car gate :8695) |
| 2 | SNK_ConfigButTxt / **port label "Stats"** | N/A index | OPENS spec/config sub-screen: anim→state 0xf (config headers + per-car config.nfo values from g_localization*Scratch tables); ◄► here also re-enters 0xf | always | Partial | td5_frontend.c:8704 (case 2)→state 15 | Label literal "Stats" ≠ symbol stem "Config" (cosmetic). Functional sub-screen state 15 loads spec fields (:8806); the SNK_Info_Values sub-panel (orig state 0x11) is a NO-OP stub (:8814 "Language.dll unavailable") |
| 3 | SNK_AutoButTxt"Automatic" / SNK_ManualButTxt"Manual" (preview/greyed when gt7 TimeTrials) | N/A index | TOGGLES g_carSelectManualTransmissionToggle ^=1, RebuildFrontendButtonSurface(3) relabels | created as PreviewButton (greyed) when gt==7 | Implemented | td5_frontend.c:8634 create, :8709 (case 3) toggle | Faithful: gt7 forces Manual + greyed (:8634 ternary, :8710 gate) |
| 4 | SNK_OkButTxt / "OK" | N/A index | ACCEPT: lock-check (cop/cheat/gt8/gt5 bypass) → g_returnToScreenIndex=-1, state 0x14 slide-out; Masters marks slot taken=2; locked plays sfx(10) | always | Implemented | td5_frontend.c:8719 (case 4) | Faithful incl. lock enforcement (:8724) + Masters taken-flag (:8732) |
| 5 | SNK_BackButTxt / "Back" | N/A index | BACK: g_returnToScreenIndex=6, state 0x14 | created ONLY when g_postRaceRestartSelectedRace==0 && g_mainMenuFlowPhase==0 (else esc=btn4, no Back) | Implemented | td5_frontend.c:8637 create, :8740 (case 5) | Port gates create on `!s_network_active` (:8637); orig gates on restart/flow phase — DIVERGENT gate condition (see GAPS). Back routes RACE_TYPE_MENU not screen 6 directly |

NON-BUTTON ELEMENTS:
| element | role/behavior | global reflected | PORT status | port ref | gap |
|---|---|---|---|---|---|
| Car preview render panel (CarPic_%d.tga, 408×280) | per-car image, slide-out/in on cycle (states 10/11/0xf via DrawCarSelectionPreviewOverlay 0x40ddc0) | g_carSelectionFrameAccumulator, car=g_quickRaceSelectedTrackId, paint transient | Implemented | td5_frontend.c:8772 (state 11 slide) / frontend_load_car_preview_surface | Faithful slide timing (13f out / 25f in) |
| CarSelBar1/CarSelCurve/CarSelTopBar overlays | static sidebar chrome, slide-in state 2 | s_carsel_*_surface | Implemented | td5_frontend.c:8520-8522 | Faithful |
| Blue car-bg fill (orig FillPrimaryFrontendRect 0x5c) | solid blue behind preview | — | Implemented (arch-div impl) | td5_frontend.c:8530-8548 (1×1 BGRA 0xFF00005C tex) | Renderer clears per frame; orig fills primary directly. Equivalent |
| Car NAME text strip ("%s %s") | centered car name, redrawn on cycle | local_80 from car name table | Implemented | frontend_render path | Faithful |
| Beast/Beauty/Locked tag (gt2 / locked) | "Beauty" if car<8 else "Beast" (Era); "Locked" overlay | SNK_Beast/Beauty/LockedTxt | [UNCERTAIN] | not located in case 7/15 port | Era Beast/Beauty tag + Locked watermark not clearly reproduced in port draw |
| Spec/Config stat sheet (state 0xf) | SNK_Config_Hdrs[] column + per-car config.nfo value columns | g_localizationCupTitleScratch / TrackNameScratch / OptionLabelScratch | Implemented | td5_frontend.c:8806 (state 15) frontend_load_car_spec_fields | Faithful enough; header/value layout from config.nfo |
| Info_Values sub-panel (state 0x11) | 10 LANGUAGE.DLL strings, centered | SNK_Info_Values | Missing (stub) | td5_frontend.c:8814 (case 17 no-op) | LANGUAGE.DLL strings unavailable → panel empty |

PARITY VERDICT: Faithful — full 26-state FSM, button order/gating, selector cycling, lock enforcement, Masters/2P/drag/cup exit routing all ported.
GAPS (actionable):
- Tab-3 button label literal is "Stats" in port but underlying orig symbol stem is "Config" (`SNK_ConfigButTxt`) — verify intended UI text vs LANGUAGE.DLL English.
- Back-button CREATE gate differs: orig = `restart==0 && flowPhase==0`; port = `!s_network_active` (td5_frontend.c:8637). Could create/omit Back in the wrong contexts (post-race restart / main-menu-flow).
- State 0x11 Info_Values sub-panel is a no-op stub (LANGUAGE.DLL) — empty info page.
- Era Beast/Beauty + Locked overlay tags ([UNCERTAIN]) not clearly drawn.
Confidence: [CONFIRMED @ 0x0040dfc0 main + 0x0040ddc0 sub] / [UNCERTAIN: Beast/Beauty/Locked tag port draw path; exact Back-gate behavioral impact]

---

### Screen 21 @ 0x00427630 — TrackSelectionScreenStateMachine  [interactive: Y]
Port handler: td5_frontend.c:9019 (Screen_TrackSelection, case TD5_SCREEN_TRACK_SELECTION)
DISPATCH MODEL: index-switch @0x004279xx (case 4) — buttons created state 0; reads g_frontendButtonIndex 0..3 + g_postRaceRacerCardNavDirection. Selected track = g_selectedScheduleIndex; direction = _g_selectedTrackDirection.

BUTTONS (creation order, state 0):
| # | label (SNK / English) | userdata | action | gating | PORT status | port ref | gap |
|---|---|---|---|---|---|---|---|
| 0 | SNK_TrackButTxt / "Track" | N/A index | ◄► cycles g_selectedScheduleIndex ±dir; skips locked NPC groups (g_npcRacerGroupTable &3) for gt>7; wrap [0..maxTrack]/(2P -1..); →state 5 reload preview | always | Implemented | td5_frontend.c:9083 (case 4 sel==0) | Faithful: cycle + skip-absent-level + 2P random(-1). Lock-group skip approximated via level-exists check (:9093) |
| 1 | SNK_ForwardsButTxt / "Forwards"↔"Backwards" | N/A index | TOGGLES _g_selectedTrackDirection ^=1, RebuildFrontendButtonSurface(1); ONLY if track has reverse flag (gNpcRacerCheatFlags[idx]!=0) | INERT/sprite-hidden (x=-0xe0) when track lacks reverse | Implemented | td5_frontend.c:9119 (case 4 sel==1) + :8997 visibility helper | Faithful: port hides+disables on forward-only tracks via td5_asset_track_has_reverse (:9002); orig gates on per-track reverse byte |
| 2 | SNK_OkButTxt / "OK" | N/A index | LAUNCH: lock-check (locked→sfx10); else g_returnToScreenIndex=~LOCALIZATION (=-1 launch), state 6 slide-out | esc-button when flow context==2 (QuickRace) | Implemented | td5_frontend.c:9128 (case 4 idx==2) | Faithful incl. lock check (:9130) + g_td5.reverse_direction commit (:9136) |
| 3 | SNK_BackButTxt / "Back" | N/A index | BACK: g_returnToScreenIndex=CAR_SELECTION, state 6 | created ONLY when g_mainMenuButtonHint != 2 (QuickRace context omits Back, esc=btn2) | Implemented | td5_frontend.c:9044 create gate (s_flow_context!=2), :9142 (idx==3) | Faithful gate (:9044) |

NON-BUTTON ELEMENTS:
| element | role/behavior | global reflected | PORT status | port ref | gap |
|---|---|---|---|---|---|
| Track thumbnail (Tracks/%s.tga, 152×224) | per-track preview image, slide on cycle | DAT_004a2c94 | Implemented | td5_frontend.c:9053/9153 frontend_load_selected_track_preview | Faithful |
| Track name plate (296×64) | track name text (parsed up to ',' from SNK_TrackNames) | g_lobbyErrorDialogSurface | Implemented | frontend_get_track_name | Faithful |
| Track description box (224×120, src_y=0x40) | second region of dialog surface | g_lobbyErrorDialogSurface | Implemented | render path | Faithful |
| "Locked" overlay (locked track) | drawn into 0x98-wide region when track locked & no cheat | SNK_LockedTxt | [UNCERTAIN] | lock enforced on OK (:9130) but overlay-text draw not confirmed | Lock TEXT overlay on preview may be missing (lock still enforced functionally) |

PARITY VERDICT: Faithful — 9-state FSM, 4 buttons w/ correct order+gating, track cycler, reverse-toggle hide-on-forward-only, OK/Back routing all ported.
GAPS (actionable):
- "Locked" text overlay on the track preview ([UNCERTAIN]) may not draw (functional lock OK).
- Orig skips locked NPC race-groups via g_npcRacerGroupTable bit test (gt>7 cup modes); port substitutes a level-zip-exists check — close but not identical for cup track schedules.
Confidence: [CONFIRMED @ 0x00427630] / [UNCERTAIN: Locked-text overlay draw; NPC-group-skip vs level-exists equivalence]

---

### Screen 22 @ 0x00417d50 — ScreenExtrasGallery  [interactive: Y (any-key/ESC advances/quits)]
Port handler: td5_frontend.c:9226 (Screen_ExtrasGallery, case TD5_SCREEN_EXTRAS_GALLERY)
DISPATCH MODEL: callback/edge-poll @0x00418062 (case 7) — NO buttons. Exit on (g_frontendInputEdgeBits & 0x40000) || g_frontendMouseEdgeBits || credit-page-counter DAT_00496354==0xb → DXWin::CleanUpAndPostQuit() (quits the GAME). Scroll driven by g_frontendAnimFrameCounter 0..0x27f wrapping a 0x280-tall two-half credits surface.

BUTTONS: NONE.

NON-BUTTON ELEMENTS:
| element | role/behavior | global reflected | PORT status | port ref | gap |
|---|---|---|---|---|---|
| Continuous credits SCROLL (320×320 viewport @ x=0xcc y=0x60) | vertically scrolling text + mugshot rows, wrapping two 0x140 halves of g_secondaryWorkSurface; advances g_extrasGalleryPage per row, mugshot every 7th | g_frontendAnimFrameCounter, g_extrasGalleryPage, g_extrasGalleryAssetHandle | Wrong (structural) | td5_frontend.c:9251 (case 2) | Port shows ONE full-screen still image at a time, auto-advancing every 4000ms (:9256) — NOT the orig's continuous vertical credit scroll w/ interleaved text rows + 7-mugshot blocks. [UNCERTAIN duration: port comment flags 4000ms as guessed] |
| 27 mugshot TGAs (Bob..Daz) + 5 Legals TGAs | mugshot row blit (color-keyed 0x11) interleaved with credit text rows; loaded ALL at init | _DAT_004962e0.._DAT_00496348 | Partial | td5_frontend.c:9217 s_gallery_names[27] | Port loads on-demand one at a time (VRAM save) + drops the 5 Legals as separate scroll frames into the same 27-list; credit TEXT rows (SNK_CreditsText) entirely absent |
| Credit text rows (SNK_CreditsText[page*0x18]) | per-line localized credit text, '#'=mugshot marker, '*'=page-end | SNK_CreditsText | Missing | — | LANGUAGE.DLL credit strings not drawn; port is image-only |
| ESC / any-key / page==0xb → quit game | exit handler | g_frontendInputEdgeBits 0x40000 | Partial | td5_frontend.c:9265 frontend_post_quit | Port quits after last IMAGE (index>=27) or via global ESC handler; orig quits on page-counter 0xb OR any input edge |

PARITY VERDICT: Divergent — port reimplements the credits roll as a paged still-image slideshow; the original's continuous scrolling text+mugshot credit roll is not reproduced.
GAPS (actionable):
- No scrolling credit TEXT (SNK_CreditsText) — port shows only mugshot/legal images.
- Auto-advance interval (4000ms) is a guess (port comment [UNCERTAIN]); orig uses frame-counter scroll, not a fixed per-image timer.
- Image ordering differs (port linear list vs orig text-driven interleave w/ '#' mugshot markers).
Confidence: [CONFIRMED @ 0x00417d50] / [UNCERTAIN: orig per-image dwell time in real units; whether credit-text omission is intentional ARCH-DIV]

---

### Screen 23 @ 0x00413580 — ScreenPostRaceHighScoreTable  [interactive: Y]
Port handler: td5_frontend.c:9290 (Screen_PostRaceHighScore, case TD5_SCREEN_HIGH_SCORE)
DISPATCH MODEL: index-switch @0x004138xx (case 6) — 2 buttons; reads g_frontendButtonIndex (0=nav-bar, 1=OK) + g_postRaceRacerCardNavDirection. Browsed category = g_postRaceRacerCardIndex.

BUTTONS (creation order, state 0):
| # | label (SNK / English) | userdata | action | gating | PORT status | port ref | gap |
|---|---|---|---|---|---|---|---|
| 0 | NULL label (520×32) | N/A index | NAV BAR: ◄► cycles g_postRaceRacerCardIndex ±dir across score categories (wrap, cheat-gated unlock range: normal [0..musicTrackIdx)/[0x13..0x19], cheat [0..0x19]); RebuildButtonSurface(0) + DrawPostRaceHighScoreEntry(idx) | always; idx-0 dispatch only when nav != 0 | Implemented | td5_frontend.c:9299 create (NULL), :9327 (case 6 delta) | Port wrap is simple [0..0x19] (:9334); orig has cheat-gated dual-range wrap (locked-cup skip). DIVERGENT wrap range — port lets you browse locked categories |
| 1 | SNK_OkButTxt / "OK" | N/A index | EXIT: g_returnToScreenIndex=MAIN_MENU, blit secondary→primary, state 7 slide-out | always | Implemented | td5_frontend.c:9337 (idx==1) | Faithful; esc=btn1 |

NON-BUTTON ELEMENTS:
| element | role/behavior | global reflected | PORT status | port ref | gap |
|---|---|---|---|---|---|
| High-score table panel (520×144) | per-category name/time rows, filled by DrawPostRaceHighScoreEntry @0x413010 | g_lobbyErrorDialogSurface | Implemented | td5_frontend.c render + score-entry draw | Faithful (DrawPostRaceHighScoreEntry not deep-decompiled; row layout assumed faithful) |
| Header label (title surface) | "High Scores" screen title, slide-in | g_currentScreenIndex (CreateMenuStringLabelSurface(7)) | Implemented | td5_frontend.c:737 (FE_TITLE_PAGE_BASE+6) | Faithful |
| State-7 exit flush | writes config (high-score table) when return==-1 | — | Implemented | td5_frontend.c:9356 td5_save_write_config | Faithful (orig InitializeFrontendDisplayModeState → WritePackedConfigTd5) |

PARITY VERDICT: Faithful — 9-state FSM, 2 buttons (NULL nav bar + OK), category browse, config flush on exit.
GAPS (actionable):
- Category-browse wrap range differs: port = plain [0..0x19] (td5_frontend.c:9334); orig = cheat-gated dual range that skips locked cups. Port exposes locked categories in the browser.
Confidence: [CONFIRMED @ 0x00413580; DrawPostRaceHighScoreEntry 0x413010 not deep-decompiled (one-level budget)]

---

### Screen 24 @ 0x00422480 — RunRaceResultsScreen  [interactive: Y]
Port handler: td5_frontend.c:9375 (Screen_RaceResults, case TD5_SCREEN_RACE_RESULTS)
DISPATCH MODEL: index-switch — TWO distinct button sets in two FSM phases. Phase A (results table, states 4-0xb): 2 buttons (NULL panel + OK) + ◄► slot browse. Phase B (post-race menu, state 0xd build → 0xf dispatch): 5 context-dependent buttons dispatched via g_postRaceMenuButtonChoice = g_frontendButtonIndex in state 0x10. Browsed racer = g_postRaceRacerCardIndex.

BUTTONS — Phase A (state 0 @0x004227xx):
| # | label / English | userdata | action | gating | PORT status | port ref | gap |
|---|---|---|---|---|---|---|---|
| 0 | NULL (520×32) | N/A index | ◄► browse racer slots 0-5 (skip state==3 disabled; drag gt7 mask &1): right→state 7, left→state 9 slide-out, cycle mid-slide, re-fill via DrawRaceDataSummaryPanel | always | Implemented | td5_frontend.c:9519 (case 6 arrow) | Faithful incl. slot-skip + drag mask (:9567) |
| 1 | SNK_OkButTxt / "OK" | N/A index | CONFIRM (idx 0..1) → state 0x0b exit slide-out | always; esc=btn0 | Implemented | td5_frontend.c:9545 (idx<2) | Faithful |

BUTTONS — Phase B post-race menu (state 0xd @0x004231d6, 5 buttons; labels differ cup vs single):
| # | label single (gt0/7/9) / label cup (gt1-6) | userdata | action (state 0x10 dispatch on choice) | gating | PORT status | port ref | gap |
|---|---|---|---|---|---|---|---|
| 0 | SNK_RaceAgain"Race Again" / SNK_NextCupRace"Next Cup Race" | N/A index | choice0: restore snapshot, cup→race_within_series++, InitializeFrontendDisplayModeState (re-race) | NextCupRace is PreviewButton(greyed) when ConfigureGameTypeFlags()==0 (cup complete) | Implemented | td5_frontend.c:9714 label, :9763 dispatch | Faithful |
| 1 | SNK_ViewReplay / "View Replay" | N/A index | choice1: g_inputPlaybackActive=1, replay mode, re-init race | PreviewButton(greyed) when g_replayFileAvailable==0 | Implemented | td5_frontend.c:9721 create, :9777 dispatch | Faithful (sets replay+playback+game replay flag :9791-9793) |
| 2 | SNK_ViewRaceData / "View Race Data" | N/A index | choice2: SetFrontendScreen(0x18=self) — re-enter screen 24 from state 0 | PreviewButton(greyed) when no race data (slot 0x1f2 size_rate==2 \|\| pool empty) | Implemented | td5_frontend.c:9722 create, :9797 dispatch self-jump | Faithful self-jump |
| 3 | SNK_SelectNewCar"Select New Car" / SNK_SaveRaceStatus"Save Race Status" | N/A index | choice3: cup→state 0x11 WriteCupData; single→CAR_SELECTION | SaveRaceStatus PreviewButton(greyed) when cup complete path | Implemented | td5_frontend.c:9715 label, :9822 dispatch | Faithful |
| 4 | SNK_Quit"Quit" / SNK_OkButTxt"OK"(cup-complete) | N/A index | choice4: gt<1→AwardCupUnlocks+NAME_ENTRY(0x19); cup mid→MAIN_MENU; cup-done won(pos==0)→CUP_WON(0x1b); failed→CUP_FAILED(0x1a) | label swaps Quit→OK when cup complete (next_valid==0) | Implemented | td5_frontend.c:9718 label, :9830 dispatch | Faithful 4-branch Quit (:9855-9866) |

Phase B-aux: state 0x11 save dialog = 2 buttons (msg SNK_BlockSavedOK/SNK_FailedToSave + OK) td5_frontend.c:9887. State 0x13 OK exits to menu.

NON-BUTTON ELEMENTS:
| element | role/behavior | global reflected | PORT status | port ref | gap |
|---|---|---|---|---|---|
| Results data panel (408×392) | per-racer position/time/speed rows filled by DrawRaceDataSummaryPanel @0x421e90; slides L/R on browse | g_lobbyErrorDialogSurface, g_postRaceRacerCardIndex | Implemented | td5_frontend.c:4770 frontend_render_race_results_overlay | Faithful slide formulas (states 7/8/9/10 confirmed in port :9554-9642); panel rendered fresh per-frame (arch-div: not a tracked surface) |
| Per-game-type result label ladder (case 0 loops) | DrawFrontendLocalizedStringToSurface y-loops differ by gt (0/7/8/9 + cup 1-6) | gt-dependent | Implemented | td5_frontend.c:4848 label ladder | Faithful per-gt ladder (CONFIRMED comment :4848) |
| Header/title (CreateMenuStringLabelSurface(0xe)) | "Results" title slide | g_currentScreenIndex | Implemented | td5_frontend.c:738 / title gating :5779 | Faithful (P1 title-strip gating to states 3..0xb :5779) |
| Cup-fail early-route (state 0) | gt4 + flag → CUP_FAILED before table | g_raceParticlePoolBase[0x20c] flag | Implemented | td5_frontend.c:9398 | Faithful early-route |

PARITY VERDICT: Faithful — full 22-state FSM (0x00-0x15), dual button phases, racer-slot browser w/ skip+drag-mask, context menu w/ greyed PreviewButtons, 4-branch Quit, cup-save dialog, replay/view-data self-jump, snapshot save/restore all ported.
GAPS (actionable):
- DrawRaceDataSummaryPanel 0x421e90 + DrawPostRaceHighScoreEntry internal row layouts not deep-decompiled (one-level budget) — assumed faithful, not byte-verified.
- 2P-mode result backup/restore block (orig g_postRaceCarSelectionBackup, big struct copy) is large; port has s_snap_* — coverage looks present but the gt7 secondary-field copy (orig case 0xf gt7 branch) is dense and not line-verified.
Confidence: [CONFIRMED @ 0x00422480 main] / [UNCERTAIN: summary-panel internal layout; 2P/gt7 backup-struct field-by-field parity]

---

## Cross-screen summary
- Interactive: 20 Y, 21 Y, 22 Y(no buttons, edge-exit), 23 Y, 24 Y.
- Dispatch model: ALL index-switch (g_frontendButtonIndex by creation order) + ◄► via g_postRaceRacerCardNavDirection — NO userdata, NO callback ptr. Port mirrors exactly (frontend_create_button has no ud; s_button_index/frontend_option_delta switch).
- Worst parity: Screen 22 (Divergent — paged still-image slideshow vs continuous credit scroll, no SNK_CreditsText).
- Best parity: Screens 23, 24, 20, 21 (Faithful FSMs).
