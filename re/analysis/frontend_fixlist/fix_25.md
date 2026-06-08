# Frontend 3-Layer Faithfulness Fix List — Screens 25–29

Implementation-ready. Each element verified against **all three layers** in the PORT
(`td5mod/src/td5re/td5_frontend.c` in worktree `fix-1780170781-137846-13319`):

- **L1 CREATION** — correct SNK_ label, position, size, gating in the screen FSM body.
- **L2 RENDERING** — element actually DRAWN: wired into the overlay dispatch
  `switch(s_current_screen)` @:5588 → `frontend_render_*_overlay`, the arrow dispatch
  `switch` @:5784 → `fe_draw_option_arrows`, or the button-draw loop @:5661. Missing from a
  dispatch = **L2 BROKEN** (the diff-blindspot bug class).
- **L3 BEHAVIOR/INPUT** — interactive case reacts correctly: `s_input_ready`/`active_button`
  fallback, ESC routing, caret, default-name, inserted-row highlight, exit target.

Refs: ORIG spec `re/analysis/frontend_screens/screens_25.md`; diff `frontend_diff/diff_25.md`;
SNK strings `re/analysis/frontend_snk_strings.md`; blindspot `frontend_diff_blindspot_postmortem.md`.

**Verified SNK_ strings (Language.dll, English):** SorryTxt="SORRY", YouFailedTxt="YOU FAILED",
ToWinTxt="TO WIN", CongratsTxt="CONGRATULATIONS!" (note trailing **!**), YouHaveWonTxt="YOU HAVE
WON THE" (note trailing **THE**), SeshLockedTxt="SESSION LOCKED", OkButTxt="OK",
EnterPlayerNameButTxt="ENTER PLAYER NAME", CarsUnlocked="CARS UNLOCKED",
TracksUnlocked="TRACKS UNLOCKED", NameTxt="NAME", CarTxt="CAR", BestTxt="BEST", TimeTxt="TIME",
LapTxt="LAP", PtsTxt="POINTS", AvgTxt="AVERAGE", TopTxt="TOP", SpdTxt="SPEED",
SpeedReadTxt[2]={"MPH","KPH"}. RaceTypeText[12]={"SINGLE RACE","CHAMPIONSHIP CUP","ERA CUP",
"CHALLENGE CUP","PITBULL CUP","MASTERS CUP","ULTIMATE CUP","DRAG RACING","COP CHASE",
"TIME TRIALS","CUP RACE","CONTINUE CUP"}.

> **CRITICAL SNK correction for cup names (S26/S27):** the port's `k_cup_type_names[1..6]`
> (@:5397) = {CHAMPIONSHIP CUP, ERA CUP, CHALLENGE CUP, PITBULL CUP, MASTERS CUP, ULTIMATE CUP}.
> But the original indexes `SNK_RaceTypeText + gametype*4`, and `RaceTypeText[1..6]` =
> {CHAMPIONSHIP CUP, **ERA CUP**, **CHALLENGE CUP**, PITBULL CUP, MASTERS CUP, ULTIMATE CUP}.
> RaceTypeText[2]="ERA CUP", [3]="CHALLENGE CUP". The port array matches by value at 1..6 — so
> the cup-name LABELS are correct. [CONFIRMED via SNK table.] The only L1 LABEL bugs are the
> trailing punctuation on the fixed message lines (see S26/S27 rows).

---

### Screen 25 — ScreenPostRaceNameEntry  [interactive Y]

Flow: state0 qualify-check (group/score/2P/DNF gates) → if qualify: prefill name + slide-in(1) →
edit(2, Win32 key poll) → slide-out(3) → insert into 5-row table(4, sets `s_score_insert_pos`,
`s_score_category_index`, `s_anim_complete=1`) → present(5/6) → table slide-in(7) → static(8/9) →
OK wait(10) → prep(11) → slide-out(12) → persist Config.td5 → MAIN_MENU. The score TABLE is drawn
by `frontend_render_high_score_overlay` (shared with Records/S23), dispatched for NAME_ENTRY @:5631.
The NAME-EDIT box is drawn by `frontend_render_text_input` @:2492, called inline from case 2 @:10198.

| element/behavior | L1 creation | L2 render | L3 behavior | VERDICT | FIX (file:line + change) |
|---|---|---|---|---|---|
| FSM shape (13 states) | MATCH (0–12 present @10090–10348) | n/a | MATCH | MATCH | — |
| Qualify gate | MATCH: score!=0 + worst-compare + 2P + DNF (10152–10168) [CONFIRMED 0x413C5E] | n/a | MATCH | MATCH | — |
| Default name when entry empty | **BUG**: `strcpy(s_post_race_name,"PLAYER")` @10183; orig empties buffer, on commit if len-test==-2 copies `g_localComputerName` (spec 91) | n/a | **BUG**: no empty→computer-name fallback on confirm (case 2 @10199 just plays sfx, never checks empty) | **BUG (L1+L3)** | td5_frontend.c:10182-10183 seed buffer EMPTY (`memset` only, drop `strcpy "PLAYER"`); in case 2 @10199-10203 on confirm if `s_post_race_name[0]=='\0'` call `GetComputerNameA(s_post_race_name,&len)` (or store host name) before slide-out |
| Name-edit box (panel) | DIVERGENCE: orig baked button 448×64 (`-0x1c0,0,0x1c0,0x40`) at (20,104); port live panel 448×80 at (96,280) (@2510-2513) | DRAWN @2492 (live, called from case2 @10198) | n/a | DIVERGENCE [ARCH] | acceptable port substitution; optional: h 80→64, move to (20,104). Title label "ENTER PLAYER NAME" @2527 = correct SNK_EnterPlayerNameButTxt |
| Caret position | **BUG**: `caret_x = text_x + caret*11.2f*sx*ts` @2550 — monospace 11.2px assumption; orig draws 2×16 bar AFTER measured pixel WIDTH | DRAWN @2548-2554 | — | **BUG (L2)** | td5_frontend.c:2550 replace `caret*11.2f` with measured prefix width: `fe_measure_text(prefix,sx*ts_text)` where prefix = buffer truncated to caret |
| Caret color | **BUG**: white `0xFFFFFFFF` @2553; orig green `0xff00` (`BltColorFillToSurface 0xff00`, spec 50) | DRAWN | — | **BUG (L2)** | td5_frontend.c:2553 caret color 0xFFFFFFFF → 0xFF00FF00 (green) |
| Caret blink | **BUG**: wall-clock `(ms-blink_tick)/350 &1` @2549; orig `g_frontendAnimFrameCounter & 0x20` (bit5, ~32-frame) | DRAWN | — | **BUG (L2)** | td5_frontend.c:2548-2549 tie blink to anim-frame counter not 350ms wall-clock (use a per-frame frame counter & 0x20) |
| Field focus bg toggle | DIVERGENCE: orig focused field bg `0x392152`(off 0x68), unfocused `0`(off 0x28), toggles via `g_connBrowserRedrawDirty&1` (spec 51); port fixed `0xFF101018` @2538 | DRAWN | — | DIVERGENCE [cosmetic] | optional: tint field bg when `s_button_index==0` focused |
| Caret suppress on overflow | MISSING: orig suppresses caret if textwidth≥0x195 (spec 50); port has no width gate | DRAWN | — | DIVERGENCE [minor] | optional: skip caret if measured width ≥ field_w |
| Name input model | ARCH-DIV: Win32 `GetAsyncKeyState` poll @2432-2490 vs orig `DXInput::GetString` middleware | n/a | ARCH-DIV | **ARCH-DIV** | document only; no DXInput middleware in port |
| Edit slide-in/out timing | **BUG**: case1/case3 `+=2; >=0x10`(=16) @10191,10208; orig settles at `0x20`(=32) → port 2× too fast | n/a | — | **BUG (L1/ANIM)** | td5_frontend.c:10191 and :10208 threshold `0x10`→`0x20` |
| Table slide-in timing | **BUG**: case7 `+=2; >=0x12`(=18) @10314; orig `0x27`(=39) + 3-frame `AdvanceFrontendTickAndCheckReady` settle (spec 74) | n/a | — | **BUG (L1/ANIM)** | td5_frontend.c:10314 threshold `0x12`→`0x27`; add 3-frame settle gate |
| Table slide-out timing | MATCH: case12 `+=2; >=16` @10336 = orig 0x10 | n/a | — | MATCH | — |
| Score table panel | MATCH: 520×144 at (115,177), border+fill (@4724-4738) | DRAWN @5631 | — | MATCH | — |
| **Inserted-row highlight (THE KNOWN BUG, == S23)** | **BUG**: `row_color=(i==0)?gold:gray` @4784 hard-codes row 0; `s_score_insert_pos` IS set @10280 but NEVER read by overlay | DRAWN | **BUG**: player's just-inserted row not emphasized | **BUG (L2/L3)** | td5_frontend.c:4784 `(i==0)` → `(i==s_score_insert_pos)`; orig emphasizes via bold alt font `SmallTextb` (spec 60-61) — use a bold/brighter color or bold font for that row only. (Same fix applies to S23.) NOTE: NAME_ENTRY and HIGH_SCORE share this fn; ensure `s_score_insert_pos` is reset/sentinel (-1) for the plain Records view so it does not gold a stale row there |
| Middle column header (mode) | MATCH: PTS / BEST+TIME / BEST+LAP per `score_type` @4765-4770 | DRAWN | — | MATCH | — |
| Column X-bands | MATCH: NAME 16, score 128(0x80), CAR 228(0xe4), AVG 352(0x160), TOP 444(0x1bc) @4741-4745 | DRAWN | — | MATCH | — |
| Name clip width | DIVERGENCE: copies 13 chars @4800, no 96px(0x60) pixel clip; orig `DrawFrontendClippedStringToSurface clip 0x60` (spec 62) | DRAWN | — | DIVERGENCE [minor] | optional: clip NAME to 96px like CAR is clipped @4819 |
| Empty-row rendering | EXTRA: port draws "---" @4789; orig leaves blank | DRAWN | — | EXTRA [cosmetic] | optional: leave blank |
| Speed per-cell unit | DIVERGENCE: unit in header band only @4774,4776; rows bare number @4827-4832; orig `%dMPH`/`%dKPH` per cell | DRAWN | — | DIVERGENCE [layout] | acceptable |
| OK button | DIVERGENCE: orig `SNK_OkButTxt,-0x130,0,0x60,0x20` + blank 0x208×0x20 spacer, ButtonIndex=1; port OK via shared flow, spacer absent | DRAWN (button loop) | n/a | DIVERGENCE [cosmetic] | optional: OK x=-0x130, add spacer |
| OK wait / confirm | MATCH (shape): case10 `if(s_input_ready)` @10324 | n/a | **PARTIAL**: single OK button — `s_input_ready` set only when a button is selected+ENTER (2259-2263) or clicked. Verify OK is auto-selected (`s_selected_button=0`) on entry so keyboard ENTER works; ESC routes to parent RACE_RESULTS (frontend_init_return_screen @10094) not MAIN_MENU | **PARTIAL (L3)** | confirm `s_selected_button` lands on the OK button after table build (case 4/5); if not, set it. ESC-vs-OK exit divergence is minor (both leave the dialog) |
| sfx on commit | DIVERGENCE: `frontend_play_sfx(5)` @10201; orig `DXSound::Play(4)` at slide-in | n/a | — | DIVERGENCE [minor] | verify sfx id 5 vs orig 4 |
| Persist + exit | MATCH: `td5_save_write_config(NULL)` @10343 then `set_screen(MAIN_MENU)` @10345 | n/a | MATCH | MATCH | — |

PORT-WIRING CHECK: render-overlay dispatch? **Y** — `case TD5_SCREEN_NAME_ENTRY: frontend_render_high_score_overlay` @5631 (reuses S23 table; gated `s_inner_state>=6 && s_anim_complete` @4718). Name-edit box drawn inline via `frontend_render_text_input` @2492 (called from case2 @10198), NOT via the overlay switch. arrow dispatch? **N/A** — no ◄► selectors on this screen (correct; not in @:5784 switch). input uses active_button fallback? **N/A** — single OK button; uses `s_input_ready` (acceptable for 1-button dialog).

SCREEN VERDICT: 6 BUGs (default-name L1+L3, caret pos L2, caret color L2, caret blink L2, edit-slide timing, table-slide timing) + 1 KNOWN inserted-row BUG (L2/L3), 1 PARTIAL (L3 OK-select), several cosmetic DIVERGENCE. 1 ARCH-DIV (Win32 input).
Ordered fix list:
1. **Inserted-row highlight** @4784 `(i==0)`→`(i==s_score_insert_pos)` + bold/bright that row (fixes carried-over S23 bug). Guard stale insert_pos in plain Records view.
2. **Default name** @10182-10183 + case2 @10199: empty buffer, on confirm copy computer name (GetComputerNameA) not "PLAYER".
3. **Caret** @2549-2553: green (0xFF00FF00), positioned after `fe_measure_text(prefix)`, blink via anim-frame `&0x20`.
4. **Slide timing** @10191/@10208 `0x10`→`0x20`; @10314 `0x12`→`0x27` + 3-frame settle.

---

### Screen 26 — ScreenCupFailedDialog  [interactive Y]

Flow: state0 gate (1≤gametype≤6 else MAIN_MENU) + load MainMenu.tga + create OK(-0x120) →
present(1/2/3) → slide-in(4, `>=0x20`) → wait OK(5) → exit. Dialog text drawn by
`frontend_render_cup_failed_overlay` @5407 (gated `s_inner_state>=4`), dispatched @5646.

| element/behavior | L1 creation | L2 render | L3 behavior | VERDICT | FIX (file:line + change) |
|---|---|---|---|---|---|
| Game-type gate | MATCH: `<1||>6`→MAIN_MENU @10361 | n/a | MATCH | MATCH | — |
| Background | MATCH: MainMenu.tga @10368 | DRAWN (bg quad) | — | MATCH | — |
| Dialog panel | MATCH geom 408×112 at (152,97) @5415-5418 | DRAWN @5423 | — | DIVERGENCE [ARCH] | panel fill `0xCC000000` translucent @5423 vs orig OPAQUE black-fill surface — make opaque (`0xFF000000`) for parity |
| Line "SORRY" | MATCH: "SORRY" y0 @5427 = SNK_SorryTxt | DRAWN | — | MATCH | — |
| Line "YOU FAILED" | MATCH: "YOU FAILED" y28(0x1c) @5429 = SNK_YouFailedTxt | DRAWN | — | MATCH | — |
| Line "TO WIN" | MATCH: "TO WIN" y56(0x38) @5431 = SNK_ToWinTxt | DRAWN | — | MATCH | — |
| Line 4 cup name | MATCH: `k_cup_type_names[gametype]` y84(0x54) @5434 = RaceTypeText[1..6] by value | DRAWN | — | MATCH [LABEL] | — (confirmed against SNK_RaceTypeText) |
| OK button | MATCH: `"OK",-0x120,0,0x60,0x20` @10379 = SNK_OkButTxt | DRAWN (button loop) | n/a | MATCH | — |
| Present/settle | MATCH: case1/2/3 @10384-10387 | n/a | — | MATCH | — |
| Slide-in motion | DIVERGENCE: `s_anim_tick++ >=0x20` @10390-10394 but panel drawn STATIC centered (no per-frame X slide); orig X=`uVar1+anim*-0x18+600` slides from right | DRAWN static | — | DIVERGENCE [ANIM] | optional: animate dialog_x during case4 from right edge to center over 32 frames |
| Wait OK / exit | DIVERGENCE: case5 `if(s_input_ready)`→**MAIN_MENU hardcoded** @10398-10400; orig `SetFrontendScreen(g_returnToScreenIndex)` | n/a | **L3 DIVERGENCE**: orig exits to returnIndex (RACE_RESULTS-flow), port to MAIN_MENU; ESC routes to parent RACE_RESULTS @2059 (mismatched with OK) | DIVERGENCE (L3) | td5_frontend.c:10400 exit to `s_return_screen` to match orig `g_returnToScreenIndex` (orig parent here is the post-race flow). [UNCERTAIN exact orig returnIndex — likely RACE_RESULTS or MAIN_MENU per caller; MAIN_MENU is benign] |
| Art | MATCH: none (text-only) | n/a | — | MATCH | — |

PORT-WIRING CHECK: render-overlay dispatch? **Y** @5646 `case TD5_SCREEN_CUP_FAILED`. arrow dispatch? **N/A** (no selectors). input uses active_button fallback? **N/A** — single OK; `s_input_ready` @10398 (OK button is index 0, auto-active).

SCREEN VERDICT: 0 BUG, 3 DIVERGENCE (panel opacity ARCH, no slide-in motion, exit target/ESC mismatch L3). All low-severity.
Ordered fix list:
1. (L3) exit target @10400 use `s_return_screen` to mirror orig returnIndex (and align ESC route).
2. (ARCH) panel opaque @5423 `0xCC000000`→`0xFF000000`.
3. (ANIM) optional horizontal slide-in motion in case4.

---

### Screen 27 — ScreenCupWonDialog  [interactive Y]

Flow: state0 gate + `frontend_delete_cup_data()` + apply unlocks (latch `s_cup_won_car_count`/
`_track_count`) + persist + create OK → present(1/2/3) → slide-in(4) → wait OK(5) → MAIN_MENU.
Text+unlocks drawn by `frontend_render_cup_won_overlay` @5462, dispatched @5650.

| element/behavior | L1 creation | L2 render | L3 behavior | VERDICT | FIX (file:line + change) |
|---|---|---|---|---|---|
| Game-type gate | MATCH: `<1||>6`→MAIN_MENU @10415 | n/a | MATCH | MATCH | — |
| Delete CupData | MATCH: `frontend_delete_cup_data()` @10422 (orig `_unlink`) | n/a | — | MATCH | — |
| Unlock progression | MATCH: `td5_save_apply_cup_unlocks_ex` gated `slot_is_finished(0)` @10439 | n/a | — | MATCH | — |
| Dialog panel | MATCH geom 408×196(0xC4) at (152,97) @5469-5472 | DRAWN @5477 | — | DIVERGENCE [ARCH] | `0xCC000000` translucent @5477 → opaque `0xFF000000` |
| Line "CONGRATULATIONS" | DIVERGENCE [LABEL]: port "CONGRATULATIONS" @5480; SNK_CongratsTxt = "CONGRATULATIONS**!**" (trailing !) | DRAWN | — | DIVERGENCE (L1) | td5_frontend.c:5480 "CONGRATULATIONS" → "CONGRATULATIONS!" |
| Line "YOU HAVE WON" | DIVERGENCE [LABEL]: port "YOU HAVE WON" @5482; SNK_YouHaveWonTxt = "YOU HAVE WON **THE**" | DRAWN | — | DIVERGENCE (L1) | td5_frontend.c:5482 "YOU HAVE WON" → "YOU HAVE WON THE" (orig draws cup name on next line, reads "YOU HAVE WON THE / [cup]") |
| Cup name line | MATCH: `k_cup_type_names[gametype]` y84 @5485 = RaceTypeText[1..6] | DRAWN | — | MATCH [LABEL] | — |
| Unlock line 1 (cars) | DIVERGENCE [LABEL]: port `"%d NEW CARS UNLOCKED"` gated `s_cup_won_car_count!=0` y140 @5489-5491; orig `sprintf("%d %s")` gated `g_cupSchedule_currentCup!=0` (spec 141). SNK_CarsUnlocked = "CARS UNLOCKED" (no "NEW") | DRAWN | — | DIVERGENCE (L1) | td5_frontend.c:5490 use `"%d %s"` with count + SNK_CarsUnlocked ("CARS UNLOCKED") → "N CARS UNLOCKED"; drop invented "NEW". [UNCERTAIN orig %s arg is item name vs the "CARS UNLOCKED" label — spec 151-154 varargs unresolved; "CARS UNLOCKED" matches the SNK string so use it] |
| Unlock line 2 (tracks) | DIVERGENCE [LABEL]: port `"%d NEW TRACKS UNLOCKED"` gated `s_cup_won_track_count!=0` y168 @5494-5496; SNK_TracksUnlocked = "TRACKS UNLOCKED" | DRAWN | — | DIVERGENCE (L1) | td5_frontend.c:5495 `"%d %s"` + SNK_TracksUnlocked → "N TRACKS UNLOCKED"; drop "NEW" |
| OK button | MATCH: `"OK",-0x120,0,0x60,0x20` @10474 | DRAWN | n/a | MATCH | — |
| Present/settle | MATCH: case1/2/3 @10479 | n/a | — | MATCH | — |
| Slide-in motion | DIVERGENCE: `>=0x20` @10486 but no horizontal slide of panel | DRAWN static | — | DIVERGENCE [ANIM] | optional slide-in motion |
| Wait OK / exit | DIVERGENCE: case5 `s_input_ready`→MAIN_MENU @10493; orig `g_returnToScreenIndex` | n/a | **L3 DIVERGENCE** (same as S26); ESC routes to parent RACE_RESULTS @2059 | DIVERGENCE (L3) | exit to `s_return_screen` to match orig |
| Trophy/medal art | MATCH: none (text-only) | n/a | — | MATCH | — |

PORT-WIRING CHECK: render-overlay dispatch? **Y** @5650 `case TD5_SCREEN_CUP_WON`. arrow dispatch? **N/A**. input uses active_button fallback? **N/A** — single OK; `s_input_ready` @10492.

SCREEN VERDICT: 0 BUG, 5 DIVERGENCE — 4 are L1 LABEL (Congrats!, "WON THE", drop "NEW" ×2) + panel opacity + exit target (L3). No L2-broken (overlay wired).
Ordered fix list:
1. (L1 LABEL) @5480 "CONGRATULATIONS!"; @5482 "YOU HAVE WON THE"; @5490/@5495 drop "NEW", use SNK_CarsUnlocked/TracksUnlocked.
2. (L3) exit target @10493 → `s_return_screen`.
3. (ARCH) panel opaque @5477.

---

### Screen 28 — ScreenStartupInit  [interactive N]

Flow: state0 create OK button → present(1/2/3) → state4 redirect to LOCALIZATION_INIT (table[0]).
No input read (auto-advances). Bootstrap/transition screen.

| element/behavior | L1 creation | L2 render | L3 behavior | VERDICT | FIX (file:line + change) |
|---|---|---|---|---|---|
| Blank dialog panel | MISSING: orig bakes 408×112 black box at (152,97), shown 3 frames (spec 178); port draws NO panel @10504-10511 (no overlay case in @:5588 switch for STARTUP_INIT) | **L2 ABSENT** (no dispatch case) | n/a | MISSING (low pri) | td5_frontend.c: optional — add `case TD5_SCREEN_STARTUP_INIT` drawing an empty 408×112 box. Functionally inert (auto-advances in 4 frames); skip unless visual parity wanted |
| Background image | EXTRA: port loads MainMenu.tga @10509; orig loads NO bg here (spec 191) | DRAWN (bg quad) | — | EXTRA [minor] | optional: remove `frontend_load_tga` @10509 for parity |
| OK button | DIVERGENCE: port `"OK",-100,0,100,0x20` @10510; orig `-0x120,0,0x60,0x20` (never read) | DRAWN | n/a (never interactive) | DIVERGENCE [cosmetic] | optional: x=-0x120, w=0x60 |
| Present states 1–3 | MATCH @10514-10517 | n/a | — | MATCH | — |
| State4 hand-off | MATCH: `set_screen(LOCALIZATION_INIT)` @10521; `TD5_SCREEN_LOCALIZATION_INIT==0` = orig table[0] (CONFIRMED td5_types.h:313) | n/a | MATCH | MATCH | — (bootstrap target faithful) |
| Input dispatch | MATCH: NONE — auto-advances (spec 193); ESC also excluded @3275 (STARTUP_INIT guard) | n/a | MATCH | MATCH | — |
| Cursor overlay | DIVERGENCE: orig `ActivateFrontendCursorOverlay()` (spec 181); port not activated | n/a | — | DIVERGENCE [decoupled, minor] | — |

PORT-WIRING CHECK: render-overlay dispatch? **N** — STARTUP_INIT has no case in @:5588 switch (panel not drawn; MISSING). arrow dispatch? **N/A**. input uses active_button fallback? **N/A** — non-interactive, no input read (faithful).

SCREEN VERDICT: 0 BUG, 1 MISSING (blank panel — L2 absent, inert), 1 EXTRA (bg load), 2 DIVERGENCE (OK geom, cursor). All cosmetic/inert; hand-off target is faithful. Lowest-priority screen.
Ordered fix list:
1. (EXTRA) optional remove MainMenu.tga load @10509.
2. (MISSING) optional add empty-box overlay case if visual parity desired.
3. (cosmetic) OK geometry @10510.

---

### Screen 29 — ScreenSessionLockedDialog  [interactive Y]

Flow: state0 gate-free init + MainMenu.tga + create OK → present(1/2/3) → slide-in(4) → wait OK(5)
→ **MAIN_MENU (hardwired)**. Text drawn by `frontend_render_session_locked_overlay` @5509,
dispatched @5654. Reachability is ARCH-DIV (kicked-from-lobby DXPTYPE path; `s_kicked_flag`
never set by a peer).

| element/behavior | L1 creation | L2 render | L3 behavior | VERDICT | FIX (file:line + change) |
|---|---|---|---|---|---|
| Reachability | ARCH-DIV: net-kicked path; no compatible peer (10531-10536) | n/a | ARCH-DIV | **ARCH-DIV** | document only |
| Background | DIVERGENCE: MainMenu.tga @10544; orig adds `BlitFrontendCachedRect(0,0,cW,cH)` (spec 206) | DRAWN | — | DIVERGENCE [cosmetic] | optional: cached-rect restore |
| Dialog panel | MATCH geom 408×112 at (152,97) @5513-5516 | DRAWN @5520 | — | DIVERGENCE [ARCH] | `0xCC000000` translucent @5520 → opaque `0xFF000000` |
| Line "SORRY" | MATCH: "SORRY" y0 @5523 = SNK_SorryTxt | DRAWN | — | MATCH | — |
| Line "SESSION LOCKED" | MATCH: "SESSION LOCKED" y56(0x38) @5525 = SNK_SeshLockedTxt | DRAWN | — | MATCH | — |
| OK button | MATCH: `"OK",-0x120,0,0x60,0x20` @10553 | DRAWN | n/a | MATCH | — |
| Present/settle | MATCH: case1/2/3 @10558 | n/a | — | MATCH | — |
| Slide-in motion | DIVERGENCE: `>=0x20` @10565, no horizontal panel slide | DRAWN static | — | DIVERGENCE [ANIM] | optional slide-in motion |
| Exit target | MATCH: case5 `s_input_ready`→MAIN_MENU @10572 = orig HARDWIRED main-menu (NOT returnIndex) | n/a | **PARTIAL**: OK→MAIN_MENU matches orig; BUT ESC routes via parent which for SESSION_LOCKED defaults to MAIN_MENU @2062 (so ESC also → MAIN_MENU, consistent here) | MATCH (L3 consistent) | — (note: unlike S26/S27, ESC and OK agree here) |
| Lock art | MATCH: none (text-only) | n/a | — | MATCH | — |

PORT-WIRING CHECK: render-overlay dispatch? **Y** @5654 `case TD5_SCREEN_SESSION_LOCKED`. arrow dispatch? **N/A**. input uses active_button fallback? **N/A** — single OK; `s_input_ready` @10571; exit + ESC both → MAIN_MENU (consistent with orig hardwired).

SCREEN VERDICT: 0 BUG, 3 DIVERGENCE (panel opacity ARCH, no slide-in motion, missing cached-rect), 1 ARCH-DIV (reachability). Exit/ESC L3 is faithful. Low priority (rarely reachable).
Ordered fix list:
1. (ARCH) panel opaque @5520.
2. (ANIM) optional slide-in motion.
3. (cosmetic) cached-rect restore.

---

## Cross-screen wiring audit (the blindspot class)

All 5 screens' overlay/text elements ARE wired into the render dispatch — no repeat of the
Music-Test arrow miss:
- S25 NAME_ENTRY → `frontend_render_high_score_overlay` @5631 (table) + inline
  `frontend_render_text_input` @10198 (edit box). **Wired.**
- S26 CUP_FAILED → @5646. **Wired.**
- S27 CUP_WON → @5650. **Wired.**
- S28 STARTUP_INIT → **NO case** (blank panel MISSING; inert, lowest pri).
- S29 SESSION_LOCKED → @5654. **Wired.**
- arrow dispatch @5784: none of 25–29 have ◄► selectors (correct; absence is faithful).

## Counts (ARCH-DIV excluded)

| screen | BUG | DIVERGENCE | MISSING | EXTRA | MATCH | ARCH-DIV |
|---|---|---|---|---|---|---|
| 25 | 6 (+1 known inserted-row) | 6 | 0 | 1 | 9 | 1 |
| 26 | 0 | 3 | 0 | 0 | 6 | 0 |
| 27 | 0 | 5 | 0 | 0 | 6 | 0 |
| 28 | 0 | 2 | 1 | 1 | 3 | 0 |
| 29 | 0 | 3 | 0 | 0 | 5 | 1 |

## Top 3 fixes (all screens)
1. **S25 inserted-row highlight** @4784 `(i==0)`→`(i==s_score_insert_pos)` + bold/bright the
   inserted row (the carried-over S23 bug; `s_score_insert_pos` is computed @10280 but unread).
2. **S25 name-entry** — empty default→computer-name @10182/case2; caret green + measured-width
   + anim-frame blink @2549-2553; slide thresholds @10191/@10208 `0x10`→`0x20`, @10314 `0x12`→`0x27`.
3. **S27 unlock/message labels** @5480-5496 — "CONGRATULATIONS!", "YOU HAVE WON THE",
   drop "NEW", use SNK_CarsUnlocked/TracksUnlocked; make S26/27/29 dialog panels opaque.

File: C:/Users/maria/Desktop/Proyectos/TD5RE/re/analysis/frontend_fixlist/fix_25.md
