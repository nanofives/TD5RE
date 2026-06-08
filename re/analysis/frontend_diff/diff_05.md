# Frontend PORT-vs-ORIGINAL Difference List — Screens 5/6/7/8/9

Sources: spec `re/analysis/frontend_screens/screens_05.md`; engine model
`frontend_rendering_model.md` / `frontend_flow_model.md`; port
`td5mod/src/td5re/td5_frontend.c` (worktree `fix-1780170781-137846-13319`).

CLASS ∈ {MATCH, BUG, DIVERGENCE, MISSING, EXTRA, ARCH-DIV}.
Cross-cutting tags: [FONT] [DECOUPLED] [ANIM] [LABEL] [ASSET].

Global note (applies to all 5): the port has NO offscreen `g_primaryWorkSurface`
compositor, no `CreateMenuStringLabelSurface` font-baked header, no
`RenderFrontendDisplayModeHighlight` 4-edge-bar decoupled highlight, and no
`UpdateExtrasGalleryDisplay`/FLUSH pipeline. It draws every element live each
frame via `fe_draw_quad`/`fe_draw_text`. Headers are TGA strips
(`frontend_get_title_tga_for_screen` :719) instead of menu-font label surfaces
[LABEL][FONT]. Button labels are hardcoded English string literals instead of
`SNK_*ButTxt` LANGUAGE.DLL lookups [LABEL]. The selection highlight is a per-
button color ramp baked in the button-draw loop (:5620+), not the orig 0xc000
edge bars — visually different but ARCH-DIV (no FLUSH layer to port). These are
recorded once here and only re-flagged per-screen where the behavior diverges.

---

## Screen 5 — ScreenMainMenuAnd1PRaceFlow (orig 0x00415490 / port Screen_MainMenu :6292)

| element/behavior | ORIGINAL (spec+addr) | PORT (td5_frontend.c:line) | CLASS | FIX |
|---|---|---|---|---|
| Full-screen background | MainMenu.tga → primary work surface (state 0) | `frontend_load_tga("MainMenu.tga")` :6297 | MATCH | — |
| Header "MAIN MENU" | `CreateMenuStringLabelSurface(0)`, anim X slide, menu-font [FONT] | MainMenuText.TGA strip :721, anim Y via `frontend_get_title_render_y` :3909 | DIVERGENCE [LABEL][FONT] | Acceptable TGA substitution; keep, but note label is a baked-image not localized font. |
| 7 menu buttons (Race/QuickRace/2P/Net/Options/HiScore/Exit) | 7× `CreateFrontendDisplayModeButton(label,-0xe0,0,0xe0,0x20)`, SNK_ labels | 7× `frontend_create_button` hardcoded EN :6324-6330 | DIVERGENCE [LABEL] | Replace literals with SNK_RaceMenuButTxt/QuickRaceButTxt/TwoPlayerButTxt/NetPlayButTxt/OptionsButTxt/HiScoreButTxt/ExitButTxt. |
| Button 2 label conditional (`app+0x170==0`→"Two Player" else "Time Demo") | state0 @0x415d3a | Always "Two Player"; benchmark gated on `ini.enable_benchmark` :6326,6413 | DIVERGENCE | Spec confirms field never written → "Two Player" always correct; port's INI path is an EXTRA test hook (benign). [UNCERTAIN field name] |
| Button-2 action (2P car-select vs benchmark) | state4 case2 | INI-gated benchmark / else 2P `s_two_player_mode=1` :6413-6425 | MATCH (behavior) | — |
| Slide-in anim commit @ anim==0x27(39), SoundEffect 4 | states 3 | `frontend_update_timed_animation(0x27,650)` + sfx 4 :6354-6356 | MATCH [ANIM] | — |
| Slide-in SoundEffect 5 (settle) on state-0 build | state 0 plays settle | `frontend_play_sfx(5)` :6338 | MATCH | — |
| Input dispatch btn0-6 → returnIndex+hint | state4 cases, NetPlay(btn3) skips device check | :6368-6451; btn3→CONNECTION_BROWSER :6428 | MATCH | — |
| Exit-confirm "Are you sure" prompt label | `CreateMenuStringLabelSurface(8)`→ dialog label, queued each frame | NOT rendered — only Yes/No buttons created :6454-6465 | MISSING [LABEL] | Render an "Are you sure?" prompt (live `fe_draw_text` near Exit button) in state 5/6, mirroring the orig confirm label. |
| Exit Yes/No buttons (cx-0xc2 / cx-0x42, cy+0xa5, 0x60×0x20) | state6 layout via BeginPreviewLayout | `frontend_create_button("Yes"/"No",...)` relative to Exit btn :6458-6459, sizes 96×32 | DIVERGENCE [LABEL] | Use SNK_YesButTxt/SNK_NoxButTxt; position is relative-to-exit (acceptable) but width 96 vs orig 0x60(96) OK, gap 100 vs orig 0x80(128). Tighten to orig layout if pixel-faithful needed. |
| Exit "Yes" → EXTRAS_GALLERY(22) (credits), multi-state slide | states 10/0xb/0xc | state7 → `set_screen(EXTRAS_GALLERY)` :6485-6488 (no slide) | DIVERGENCE [ANIM] | Faithful target; orig adds exit slide-out (0x10/0xc states). Optional: add slide before credits. |
| Controller-required dialog (states 0x14-0x17): "MUST SELECT PLAYER 1/2 CONTROLLER" panel + OK btn, divert to CONTROL OPTIONS | device check on state9 (player1/2 input source==7) | DROPPED — port is keyboard-first; state9 navigates directly :6497-6510 | ARCH-DIV | Documented divergence (keyboard always available). Keep; no fix unless joystick-required parity wanted. |
| Selection highlight (0xc000 edge bars) | `RenderFrontendDisplayModeHighlight` [DECOUPLED] | per-button color ramp :5620+ | ARCH-DIV | No FLUSH layer; keep ramp. |
| Mouse cursor sprite | LOOP queues snkmouse 22×30 | port draws software cursor (`frontend_set_cursor_visible`) :6337,6355 | MATCH | — |

Class counts S5: MATCH 6, DIVERGENCE 5, MISSING 1, ARCH-DIV 2, EXTRA(noted) —.

---

## Screen 6 — RaceTypeCategoryMenuStateMachine (orig 0x004168b0 / port Screen_RaceTypeCategory :6555)

| element/behavior | ORIGINAL (spec+addr) | PORT (line) | CLASS | FIX |
|---|---|---|---|---|
| Full-screen background MainMenu.tga | state0 | `frontend_load_tga("MainMenu.tga")` :6566 | MATCH | — |
| Header "RACE MENU" | `CreateMenuStringLabelSurface(1)` [FONT] | RaceMenuText.TGA :722 | DIVERGENCE [LABEL][FONT] | TGA substitution (keep). |
| Hover-description panel (0x110×0xb4, title + wrapped body) | INLINE bake into `g_lobbyErrorDialogSurface` from `SNK_RaceTypeText[gameType]`, rebaked on hover (states 4/9) | `frontend_render_race_type_description` :4185 — live, hardcoded English 4-line arrays `k_race_type_lines`/`k_cup_lines` :4186-4203 | DIVERGENCE [LABEL] | KNOWN ISSUE confirmed: hover text is hardcoded English (port-authored copy), NOT `SNK_RaceTypeText`. Replace with localized SNK_RaceTypeText[gameType] strings; the orig wraps via `DrawFrontendWrappedStringLine`. |
| Hover-description: index by hovered button | state4 remap btn→{0,7,8,9,10,0xb} into SNK_RaceTypeText | `desc_index = s_selected_button` (0..6) into local arrays :4205,4212 | DIVERGENCE | Port indexes the WRONG namespace (per-button row, not orig's gameType remap). When localizing, apply the state-4 button→SNK index remap (btn0→0,btn3→9,btn4→7,btn5→8,btn1→10,btn2→0xb). |
| Hover-description panel position | (cx+0x26, cy-0x5f)=(358,97) 0x110×0xb4 | panel_x=358, panel_y=145 :4206-4209 | DIVERGENCE | Y off: orig cy-0x5f=240-95=145 ✓ x cx+0x26=320+38=358 ✓ — actually MATCHES; reclassify MATCH. |
| Decoupled hover-rebake state (4/9) | single-frame BltColorFill + redraw | folded into per-frame live render; no state 4/9 in port (states 3 interactive only) | ARCH-DIV | Acceptable (live render makes rebake unnecessary). |
| TOP 7 buttons (SingleRace/CupRace/ContCup/TimeTrials/DragRace/CopChase/Back) | 6×Button + Back(0x70), ContCup=Preview if `ValidateCupDataChecksum()==0` | :6576-6586; ContCup preview-gated on `frontend_validate_cup_checksum()` :6579-6582 | MATCH (gating) / DIVERGENCE [LABEL] | Labels hardcoded; Back uses 0xE0 width not orig 0x70 (narrower). Use SNK_ labels + Back width 0x70. |
| TOP button press dispatch (gameType + ConfigureGameTypeFlags + return) | state3 | :6610-6660 | MATCH | game_type enum remap is documented ARCH-DIV (port 7=TimeTrial/8=Cop/9=Drag) :6533-6553 — internally consistent. |
| Back(btn6) return = CREATE_SESSION vs MAIN_MENU by lobby phase | state3 | `s_network_active ? CREATE_SESSION : MAIN_MENU` :6656 | MATCH | — |
| CUP-TIER 7 buttons (Championship/Era/Challenge/Pitbull/Masters/Ultimate/Back) | state6 built @anim==0x23, preview-greyed by cheat tier | :6674-6701 preview-gated on `s_cup_unlock_tier` | MATCH (gating logic) / DIVERGENCE [LABEL] | Labels hardcoded; use SNK_ cup names. |
| Cup-tier gating thresholds | cheat&7 ==0→Champ+Era only; ==1→+Challenge+Pitbull; >=2→all | Challenge/Pitbull `>=1`, Masters/Ultimate `>=2` :6678-6699; tier is bitfield (|=1,|=2) :2973-2988 | DIVERGENCE [UNCERTAIN] | Bitfield encoding ≈ spec integer but edge cases differ (bit1-only=value2 would unlock Masters w/o champ). Verify against `g_cheatFlagBitfieldGameModes` integer semantics; align if needed. |
| Cup press → gameType=tier, schedule=attract track, ConfigureGameTypeFlags | state8 | :6720-6757, `s_selected_track=s_attract_track` :6753 | MATCH | — |
| Slide-in commit anim==0x20(32) + sfx4 | states1/7 | `frontend_update_timed_animation(0x20,533)` :6594,6708 | MATCH [ANIM] | sfx4 on settle not explicitly called in state1/7 — verify chime plays. |
| Slide-out commit anim==0x23(35) | states5/6/0xb/0xc | port state 0x14 uses `(16,267)` :6772-6775 | DIVERGENCE [ANIM] | Slide-out commits at 16 frames not orig 0x23(35); shorter exit anim. |

Class counts S6: MATCH 8, DIVERGENCE 6, ARCH-DIV 2, UNCERTAIN 1.

---

## Screen 7 — ScreenQuickRaceMenu (orig 0x004213d0 / port Screen_QuickRaceMenu :6789)

| element/behavior | ORIGINAL (spec+addr) | PORT (line) | CLASS | FIX |
|---|---|---|---|---|
| Background MainMenu.tga | state0 | :6796 | MATCH | — |
| Header "QUICK RACE" | `CreateMenuStringLabelSurface(3)` [FONT] | QuickRaceText.tga :723 | DIVERGENCE [LABEL][FONT] | TGA substitution (keep). |
| Car+Track info panel (0x208×200, car name top, track name bottom), rebaked per ◄► | INLINE `BltColorFillToSurface`+`DrawFrontendLocalizedStringToSurface` | `frontend_render_quick_race_overlay` :3942 live `frontend_draw_value_text` :3956-3957 | DIVERGENCE | No panel backing fill (orig clears panel surface); text-only. Acceptable but the panel frame/background is MISSING. |
| Car name / Track name positions | car cx-200/top-half, track bottom-half; panel at (cx-200,cy-0x8f)=(120,97) | car (140,106), track (140,226) :3956-3957 | DIVERGENCE | Positions are port-tuned (140 vs orig panel-origin 120). Verify alignment vs orig screenshot. |
| "LOCKED" overlay when car/track locked & !cheat | `DrawFrontendLocalizedStringToSurface` lock message into panel | `frontend_draw_value_text(...,"LOCKED",...)` :3958-3959 | DIVERGENCE [LABEL] | Hardcoded "LOCKED"; orig uses localized lock string. |
| Button 0 "CHANGE CAR" + ◄► arrows (cx-200,cy-0x67,0x100×0x20) | `CreateFrontendDisplayModeButton`+`InitializeFrontendDisplayModeArrows(0,1)` | `frontend_create_button("Change Car",120,137,256,32)` + `is_selector=1` :6813-6814 | MATCH (layout) / DIVERGENCE [LABEL] | Use SNK_ChangeCarButTxt; arrows via `fe_draw_option_arrows` :3962 OK. |
| Button 1 "CHANGE TRACK" + arrows (cx-200,cy+0x11) | `InitializeFrontendDisplayModeArrows(1,1)` | :6815-6816 (120,257) | MATCH / DIVERGENCE [LABEL] | Use SNK_ChangeTrackButTxt. |
| Button 2 "OK" (cx-200,cy+0x89,0x60×0x20) | — | :6817 (120,377,96,32) | MATCH / [LABEL] | SNK_OkButTxt. |
| Button 3 "BACK" (cx-0x58,cy+0x89,0x70×0x20), ESC=3 | — | :6818 (232,377,112,32) | MATCH / [LABEL] | SNK_BackButTxt. |
| Slide-in commit anim==0x27(39) | state3 | `frontend_update_timed_animation(0x27,650)` :6833 | MATCH [ANIM] | — |
| ◄► cycle dispatch (btn0→car, btn1→track, wrap by cheat bounds) | state4 | :6839-6866; car_max 32/36/unlocked, track_max 0x13/unlocked | MATCH | wrap bounds match cheat/network cases. |
| OK locked-block (sfx 10 + no commit unless cheat) | state4 btn2 | :6867-6881 plays sfx 10 if locked :6876 | MATCH | — |
| OK→start race (return=~LOCALIZATION_INIT sentinel) / Back→MAIN_MENU | state4/6 | `s_return_screen=-1`→`init_race_schedule` :6878,6896-6904; Back→MAIN_MENU :6883 | MATCH | sentinel modeled as -1. |
| Slide-out commit anim==0x10(16) | state6 | `(16,267)` :6897 | MATCH [ANIM] | — |

Class counts S7: MATCH 10, DIVERGENCE 6 (mostly [LABEL]).

---

## Screen 8 — RunFrontendConnectionBrowser (orig 0x00418d50 / port Screen_ConnectionBrowser :6915) — LIST BROWSER

| element/behavior | ORIGINAL (spec+addr) | PORT (line) | CLASS | FIX |
|---|---|---|---|---|
| Background MainMenu.tga + BlitFrontendCachedRect | state0 | `frontend_load_tga` :6921 | MATCH | — |
| Header "CHOOSE CONNECTION" | `CreateMenuStringLabelSurface(5)` [FONT] | NetPlayText.TGA :733 (shared net title) | DIVERGENCE [LABEL][FONT] | TGA shows generic "NET PLAY" art, not "CHOOSE CONNECTION". Add per-state net subtitle if faithful text wanted. |
| List-panel button (big, SNK_ChooseConnectionButTxt,-0x1f0,cy-0x2f,0x1f0×0x80)=slot0 | state1 | `frontend_create_button("Provider",120,160,256,32)` :6923 | DIVERGENCE [LABEL] | Wrong size (256×32 vs 0x1f0×0x80=496×128) + label "Provider" not SNK_ChooseConnectionButTxt; it's not a list container. |
| **List rows (connection names)** ≤4, `DrawFrontendClippedStringToSurface` from dpu+(off+row)*0x3c | states 3/4/5/6 INLINE bake into slot-0 panel | NOT RENDERED — no overlay case for CONNECTION_BROWSER in dispatch (:5547 switch) | MISSING (ARCH-DIV net) | Net browser has no enumerable peer list rendering. ARCH-DIV (DXPTYPE) but rows/scroll/highlight entirely absent — note for any LAN support. |
| **Up/Down scroll indicators ▲▼** (slot0.x+0xf2, ±) | scrollOffset gating | absent | MISSING (ARCH-DIV) | — |
| **List selection bar** (per-row, blinks anim&0x10) | states5/6 | absent | MISSING (ARCH-DIV) | — |
| OK button (SNK_OkButTxt,-0x1f0,cy+0x89,0x60×0x20)=slot1 | state1 | `frontend_create_button("OK",-100,0,100,0x20)` :6924 | DIVERGENCE [LABEL] | Size 100 vs 0x60(96); off-screen seed -100 vs -0x1f0; label literal. |
| BACK button (slot2, 0x70×0x20, ESC=2) | state1 | `frontend_create_button("Back",-100,0,100,0x20)` :6925 | DIVERGENCE [LABEL] | width 100 vs 0x70(112); label literal. |
| State 5 dispatch btn0→list-nav(6), btn1(OK)→SESSION_PICKER(9), btn2(BACK)→MAIN_MENU(5) | state5 | :6951-6962 btn0→sfx2 only, btn1→SESSION_PICKER, btn2→MAIN_MENU | DIVERGENCE | btn0 (the would-be list) just plays sfx2 — no list-nav state entered (state6 is a no-op fallthrough :6971). Targets OK/Back match. |
| State 6 list-nav (up/down edges, mouse row-hit, re-bake) | full nav | states 6/7 are no-op→state5 :6971-6974 | MISSING (ARCH-DIV) | List navigation unimplemented (no peers). Documented. |
| Computer-name seed (GetComputerNameA / "Clint Eastwood" fallback) | state0 | `frontend_net_enumerate()` :6920 (no name seed) | MISSING | Minor: no local computer-name display. |
| Slide-in commit anim==0x14(20) + sfx4 | state2 | port `(0x10,267)` :6935 | DIVERGENCE [ANIM] | Commits at 16 not 0x14(20). |
| Slide-out commit anim==0x18(24) | state9 | `(16,267)` :6982 | DIVERGENCE [ANIM] | 16 vs 0x18(24). |
| State 0xb cross-fade to MAIN_MENU | branch on return==5 | folded into generic slide-out :6982-6983 | DIVERGENCE [ANIM] | No cross-fade variant. |

Class counts S8: MATCH 1, DIVERGENCE 7, MISSING 5 (4 are ARCH-DIV net layer).

---

## Screen 9 — RunFrontendSessionPicker (orig 0x00419cf0 / port Screen_SessionPicker :6994) — LIST BROWSER

| element/behavior | ORIGINAL (spec+addr) | PORT (line) | CLASS | FIX |
|---|---|---|---|---|
| Background MainMenu.tga | (port adds) state0 | `frontend_load_tga` :6999 | DIVERGENCE | Orig screen 9 does NOT reload bg (reuses screen-8 primary); port reloads. Benign. |
| Header "CHOOSE SESSION" | inherits `g_currentScreenIndex` (NOT rebuilt) | NetPlayText.TGA :733 (same as screen 8) | MATCH (inherit) / [LABEL] | Port reuses net title for both — matches "does not rebuild" behavior. |
| List-panel button (SNK_ChooseSessionButTxt,-0x1f0,0,0x1f0×0x80)=slot0 | state1 | `frontend_create_button("Session",120,160,256,32)` :7000 | DIVERGENCE [LABEL] | Wrong size + label; not a list container. |
| **List rows (sessions)**: `RenderFrontendSessionBrowser` 0x00419b30 — per row name/host/gametype/"d/d" count; row0="NEW SESSION" | states1/2/3/4 INLINE | NOT RENDERED — no overlay case for SESSION_PICKER in dispatch (:5547) | MISSING (ARCH-DIV net) | No session list, no "NEW SESSION" row, no host/gametype/count columns. ARCH-DIV but entirely absent. |
| **Scroll indicators / selection bar** | states3/4 | absent | MISSING (ARCH-DIV) | — |
| Live session refresh (dpu+0xc04 net flag re-renders list) | conditional | absent | MISSING (ARCH-DIV) | The only screen whose list mutates from network mid-frame — unimplemented. |
| OK/New button (SNK_OkButTxt,-0x60,0,0x60×0x20)=slot1 | state1 | port creates **"Create"** (slot1) AND **"OK"** (slot2) — TWO buttons :7001-7002 | EXTRA + BUG [LABEL] | KNOWN ISSUE confirmed: orig has only slot1=OK and slot2=Back (3 buttons total). Port added a spurious "Create" button → 4 buttons, shifting OK to slot2 and Back to slot3. Remove "Create"; OK should be slot1, Back slot2. |
| BACK button (slot2, 0x70×0x20, ESC=2) | state1 | `frontend_create_button("Back",-100,0,100,0x20)` slot3 :7003 | DIVERGENCE [LABEL] | Now slot3 due to spurious Create; ESC mapping off. |
| State 3: btn1(OK/Join)→write chosen session(cursor-1) to dpu+0xc00, return=CREATE_SESSION(10), EnumSessionTimer(0) | state3 | port btn1→state4 "create sub-flow"; btn2(OK)→CREATE_SESSION; btn3(Back)→CONNECTION_BROWSER :7019-7032 | BUG | KNOWN ISSUE confirmed: session-JOIN is lost. Orig "OK" JOINS the highlighted session (writes chosen index then →CREATE_SESSION which acts as the lobby entry). Port's OK (slot2) blindly goes to CREATE_SESSION with no chosen-index write, and the spurious "Create" (slot1) also routes to CREATE_SESSION. There is no join-by-row path. Fix: drop Create; OK(slot1) writes selected session row and joins; Back(slot2)→CONNECTION_BROWSER. |
| State 4 list-nav (up/down/mouse row-hit) | full nav | port state4 = "create sub-flow"→CREATE_SESSION :7035-7038 | MISSING/BUG (ARCH-DIV) | List-nav state repurposed as create redirect; no row navigation. |
| Slide-in commit anim==0x20(32) + sfx4 + DeactivateCursorOverlay | state2 | port `(0x10,267)` :7013 | DIVERGENCE [ANIM] | Commits at 16 not 0x20(32). |
| Slide-out commit anim==0x18(24) | state7 | `(16,267)` :7046 | DIVERGENCE [ANIM] | 16 vs 0x18(24). |
| Back return target | CONNECTION_BROWSER(8) | :7029 →CONNECTION_BROWSER | MATCH | — |

Class counts S9: MATCH 2, DIVERGENCE 5, MISSING 4 (ARCH-DIV net), BUG 2, EXTRA 1.

---

## Cross-cutting summary

- [LABEL]: Every button + header + hover/lock text on all 5 screens is hardcoded
  English, not `SNK_*` LANGUAGE.DLL strings. Most impactful on S6 hover-description
  (port-authored copy, not `SNK_RaceTypeText`) and S5 exit prompt.
- [FONT]: Headers are TGA strips, not `CreateMenuStringLabelSurface` menu-font
  bakes (all 5). Visually close; not localizable.
- [ANIM]: Slide-out (and some slide-in) commit frame counts are uniformly 16 in
  the port vs orig per-screen values (0x14/0x18/0x20/0x23). Exit/cross-fade
  variants (S5 0xc, S8 0xb) are folded into generic slide-out.
- [DECOUPLED]: No FLUSH compositor → no 0xc000 edge-bar highlight, no
  `UpdateExtrasGalleryDisplay` cover-art (inert on these screens anyway per spec),
  no overlay double-buffer. All ARCH-DIV.
- [ASSET]: NetPlayText.TGA shared by S8/S9/10/11/29 — no per-state "CHOOSE
  CONNECTION"/"CHOOSE SESSION" text.

## Top 3 fixes (highest faithfulness impact)

1. **S9 spurious "Create" button + lost session-join (BUG/EXTRA, :7001-7038)** —
   remove the extra "Create" button (orig has 3 buttons: list/OK/Back); make OK
   join the highlighted session row (write chosen index, →CREATE_SESSION); fix
   Back/ESC slot indices. This restores the only broken *behavioral* path.
2. **S6 hover-description hardcoded English (DIVERGENCE [LABEL], :4185-4225)** —
   replace `k_race_type_lines`/`k_cup_lines` with localized `SNK_RaceTypeText`
   indexed via the orig state-4 button→SNK remap; wrap with the small font.
3. **S5 missing "Are you sure?" exit-confirm prompt (MISSING [LABEL], :6454-6483)**
   — render the confirm prompt label (orig `CreateMenuStringLabelSurface(8)`) above
   the Yes/No buttons; secondarily swap all 5 screens' literal button labels to
   `SNK_*ButTxt`.

File: `re/analysis/frontend_diff/diff_05.md`
