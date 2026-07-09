# Frontend 3-Layer Faithfulness Fix List — Screens 20–24

3 layers verified per element against PORT code (`td5mod/src/td5re/td5_frontend.c` unless noted):
- **L1 CREATION** — label (real SNK_ string from `frontend_snk_strings.md`, not guessed English), position, size, gating.
- **L2 RENDERING** — element actually DRAWN? overlay dispatch `switch(s_current_screen)` @:5588 → `frontend_render_<screen>_overlay`; arrow dispatch `switch(s_current_screen)` @:5784 → `fe_draw_option_arrows(idx)`; value/preview/title overlays; flush/decoupled.
- **L3 BEHAVIOR/INPUT** — interactive case reacts correctly? arrow/selector input MUST use `active_button = (s_button_index>=0)?s_button_index:s_selected_button;` (not raw `s_button_index==N`); correct global, wrap-vs-clamp, nav, inserted-rank highlight.

GROUND TRUTH this pass: Ghidra re-decompiled `CarSelectionScreenStateMachine 0x0040DFC0` and `DrawPostRaceHighScoreEntry 0x00413010` (read_only pool). Two diff_20.md "facts" FALSIFIED — corrected inline:
- ❌ diff_20 "S20 Config label orig='Config'": SNK_ConfigButTxt = **"STATS"** → port "Stats" is CORRECT (L1 MATCH).
- ❌ diff_20 "S21 dir label orig='Reverse'": SNK_BackwardsButTxt = **"BACKWARDS"** → port "Backwards" is CORRECT (L1 MATCH).
- ❌ diff_20 cross-cutting "no MenuFont title strip on 20/21/23/24": port DOES render title art (`frontend_get_title_tga_for_screen` @:725 → SelectCarText/TrackSelectText/HighScoresText/ResultsText.TGA, drawn @:5858). Original `CreateMenuStringLabelSurface` also bakes these as art — NOT MISSING. (S22 returns NULL = correct, gallery has no title.)

ARCH-DIV (excluded from fix counts): ms-driven animations vs frame-count equality [ANIM]; unified button/hover highlight vs `RenderFrontendDisplayModeHighlight 0x4263e0` [DECOUPLED]; per-frame live text vs baked offscreen surfaces; S22 paged-slideshow vs scroll-reel (see S22).

---

### Screen 20 — CarSelectionScreenStateMachine (0x0040DFC0)  [interactive Y]
Flow: 0 init/clamp roster · 2 intro-strip slide · 4 build 6 buttons (Car/Paint/Stats/Auto|Manual/OK/Back) · 5 panel slide-in · 6 settle→7 · **7 interactive** · 10/11/12/14 car-swap slide · 0xf STATS bake · 0x14..0x1a commit+exit-route. Port `Screen_CarSelection` @:8628; overlay `frontend_render_car_selection_preview` @:4405; stats `frontend_render_car_stats_overlay` @:4318.

| element/behavior | L1 creation | L2 render | L3 behavior | VERDICT | FIX (file:line + change) |
|---|---|---|---|---|---|
| CarPic%d.tga preview panel | MATCH 408×280 @(232,124), per-car blue colorkey :788 | MATCH drawn :4471/4488/4492 | MATCH slide-in/out states 11/14 | **MATCH** | — |
| Car name "%s %s" | MATCH centered (232,89) :4498 | MATCH :4498 | MATCH redrawn each swap | **MATCH** | — |
| Car selector slot0 "CAR" + ◄► | MATCH SNK_CarButTxt="CAR" @:8766; arrows InitArrows(0,1) | MATCH overlay arrows @:5803 + :4546 | MATCH cycle gametype-wrap :8804-8822 | **MATCH** (minor: see "paint/config reset" row) | — |
| **Car cycle resets paint+config to 0** | n/a | n/a | **orig 0x40E8xx sets `g_carSelectPaintSchemeTransient=0; g_carSelectWheelSchemeTransient=0` on EVERY car change** [CONFIRMED]; port leaves `s_selected_paint`/`s_selected_config` unchanged | **BUG (L3)** | :8821 (case 0, after car cycle) before `s_inner_state=10`: add `s_selected_paint = 0; s_selected_config = 0;` |
| Paint selector slot1 "PAINT" + ◄► | MATCH SNK_PaintButTxt="PAINT" :8767; arrows InitArrows(1,1) | MATCH arrows @:5804 + :4547 | MATCH wrap 0..3, cop-car gate `<0x1C||>0x24` :8830 | **MATCH** | — |
| **Stats selector slot2 "STATS" + ◄►** | MATCH SNK_ConfigButTxt="STATS" :8768 (diff_20 "Config" WRONG) | L2 arrows: NOT drawn — slot2 absent from arrow dispatch @:5801-5805 (only 0,1) | **L3 BROKEN: orig slot2 ◄► CYCLES `g_carSelectWheelSchemeTransient` 0..3 wrap THEN enters state 0xf** [CONFIRMED case 7 `g_frontendButtonIndex==2`]; press also enters 0xf. Port case2 @:8839 is press-ONLY (enters state15), never cycles config; `active_button` fallback @:8800-8802 EXCLUDES btn2 so arrows ignored on Stats | **BUG (L2+L3)** | (a) input @:8800 add `2` to fallback set OR add explicit btn2 arrow handler: on `delta!=0` do `s_selected_config += delta; wrap 0..3;` then `s_inner_state=15`. (b) arrow dispatch @:5801 add `fe_draw_option_arrows(2, sx, sy);`. (c) commit `s_player1/2 wheel = s_selected_config` already snapshotted via s_snap_config but never set from live cycle — ensure stats sub-screen reads `s_selected_config`. |
| Transmission slot3 Auto/Manual | MATCH SNK_AutoButTxt="AUTOMATIC"/SNK_ManualButTxt="MANUAL" :8769 (port "Automatic"/"Manual"=MATCH) | MATCH button label | MATCH toggle :8844, drag(gt9)+TT(gt7) lock; fallback incl btn3 :8801 | **MATCH** | — |
| OK slot4 "OK" | MATCH SNK_OkButTxt :8771 | MATCH | MATCH commit+lock-reject sfx10 :8860-8877 | **MATCH** | — |
| BACK slot5 "BACK" | DIVERGENCE gate: orig `g_postRaceRestartSelectedRace==0 && g_mainMenuFlowPhase==0` [CONFIRMED case 4]; port gates `!s_network_active` :8772 | MATCH drawn when created | MATCH back→RaceType :8881 | **DIVERGENCE (L1)** | :8772 replace `if (!s_network_active)` with the orig restart/menu-flow gate (port lacks restart/flow-phase globals → [UNCERTAIN] if surfaceable; if not, leave as documented ARCH-DIV). Low priority. |
| **Beast/Beauty tag (gametype 2)** | **MISSING** — orig case0xc: gt==2 → `g_quickRaceSelectedTrackId<8`→Beauty else Beast at (cx−0xea=86, cy−0x77=163) [CONFIRMED]; SNK_BeautyTxt="BEAUTY"/SNK_BeastTxt="BEAST" | not drawn | n/a | **MISSING (L1+L2)** | :4499 area (after car name): `if (s_selected_game_type==2) frontend_draw_value_text(sx,sy,86,163, actual_car<8?"BEAUTY":"BEAST", 0xFFFFFFFF);` (mutually-excl with LOCKED) |
| Locked tag | DIVERGENCE: orig SNK_LockedTxt="LOCKED" white localized at (cx−0xea=86,cy−0x77=163); port "LOCKED" red 0xFFFF4444 at (86,121) :4502 | MATCH drawn | MATCH gate (locked&&!cheat&&!net, OK enforces gt∉{8,5}) | **DIVERGENCE (L1)** | :4502 change color 0xFFFF4444→0xFFFFFFFF and y 121→163 (cy−0x77). Content "LOCKED" already matches SNK. |
| STATS panel (state 0xf) | DIVERGENCE [LABEL][FONT]: 14 rows from config.nfo; English hdrs vs SNK_Config_Hdrs (content matches: LAYOUT/GEARS/PRICE/TIRES/...) | MATCH drawn :4318 | MATCH press enters :8947 | **DIVERGENCE (L1, content-faithful)** | leave; SNK_Config_Hdrs content already replicated. |
| INFO panel (state 0x11) | MISSING — orig draws 10 SNK_Info_Values rows | case17 stub :8955 | unreachable (no input path enters 0x11) | **MISSING (ARCH-DIV)** | orig INFO sub-mode (`g_carSelectionPreviewFrameIndex` 1/2) not reachable in port; SNK_Info_Values is car-specific prose. Exclude (low value). |
| Header title strip | MATCH SelectCarText.TGA :731 (diff_20 "MISSING" WRONG) | MATCH drawn :5858 | n/a | **MATCH** | — |

PORT-WIRING CHECK: render-overlay dispatch? **Y** @:5610 `frontend_render_car_selection_preview`; arrow dispatch? **Y** @:5801 but **only btn 0,1** (btn2 Stats MISSING); input uses active_button fallback? **PARTIAL** @:8800 (`s_button_index` else `s_selected_button`) but fallback set excludes btn2 → Stats arrows dead.

SCREEN VERDICT: 1 BUG-L2+L3 (Stats ◄► cycle + arrow render), 1 BUG-L3 (paint/config reset on car cycle), 1 MISSING-L1+L2 (Beast/Beauty), 1 DIVERGENCE-L1 (Locked color/pos), 1 DIVERGENCE-L1 (Back gate). MISSING INFO + STATS-hdr-lang are ARCH-DIV.
Ordered fixes: (1) Stats ◄► cycle wheel-scheme 0..3 + add `fe_draw_option_arrows(2)` to dispatch :5801. (2) Reset paint+config=0 on car cycle :8821. (3) Beast/Beauty tag :4499. (4) Locked tag color→white, y→163 :4502. (5) Back gate :8772 (low/UNCERTAIN).

---

### Screen 21 — TrackSelectionScreenStateMachine (0x00427630)  [interactive Y]
Flow: 0 init/clamp+build 4 buttons (Track/Forwards/OK/Back)+TrackSelect.tga+dir-visibility · 3 slide-in · **4 interactive** · 5 reload preview · 9 16-tick track-switch slide · 7/8 exit. Port `Screen_TrackSelection` @:9180; overlay `frontend_render_track_selection_preview` @:4507.

| element/behavior | L1 creation | L2 render | L3 behavior | VERDICT | FIX |
|---|---|---|---|---|---|
| Track preview TGA "Front End\Tracks\%s.tga" | MATCH 152×224 @(412,135) :4540 | MATCH drawn :4541 + slide :4514 | MATCH reload on cycle :9314 | **MATCH** | — |
| Track name panel (city/country split ',') | MATCH SNK_TrackNames split @comma, city y81 / country y113 centered x492 :4521-4533 | MATCH drawn | MATCH | **MATCH** (orig replaces comma-remainder via DAT_004658e4 template; port keeps raw — visually closer, minor) | — |
| Track selector slot0 "TRACK" + ◄► | MATCH SNK_TrackButTxt="TRACK" :9201; arrows InitArrows(0,1) | MATCH arrows @:5811 + :4546 | MATCH cycle skip-missing-zip :9244-9272, dir reset + visibility refresh | **MATCH** | — |
| Direction toggle slot1 "FORWARDS"/"BACKWARDS" | MATCH SNK_ForwardsButTxt="FORWARDS"/SNK_BackwardsButTxt="BACKWARDS" :9202/9284 (diff_20 "Reverse" WRONG); reverse-capable gate `td5_asset_track_has_reverse` :9217/9270 | MATCH arrows @:5806 NOT drawn for slot1 (only slot0); but :4547 draws slot1 arrows pre-fill — **slot1 has NO selector arrows in orig (toggle, not cycler)** so this is actually OVER-drawing | **L3: toggle MATCH** :9280-9287 (`!hidden && (delta||press)`); but orig slot1 is a PRESS toggle, NOT a ◄►-cycler — orig calls InitArrows(0,1) ONLY (slot0). Port draws arrows on slot1 too :4547 | **DIVERGENCE (L2 minor)** | :4547 remove `fe_draw_option_arrows(1, sx, sy);` — orig only arms arrows on the Track selector (slot0), not the Forwards/Backwards toggle. (Keep the toggle responding to press; remove the ◄► affordance.) [UNCERTAIN whether orig also responds to ◄► on slot1 — decomp shows toggle on button press; safest = press-only + no arrows.] |
| OK slot2 "OK" | MATCH SNK_OkButTxt :9203 | MATCH | MATCH lock-reject sfx10 :9289-9301 | **MATCH** | — |
| BACK slot3 "BACK" | MATCH hidden when `s_flow_context==2` (QuickRace) :9205 ↔ orig `g_mainMenuButtonHint==2` | MATCH | MATCH →CarSelection :9303 | **MATCH** | — |
| Locked tag | DIVERGENCE: port "LOCKED" red 0xFFFF4444 @(412,375) :4553 vs orig SNK_LockedTxt white | MATCH drawn | MATCH gate | **DIVERGENCE (L1)** | :4553 color 0xFFFF4444→0xFFFFFFFF (content already "LOCKED"). |
| Header title strip | MATCH TrackSelectText.TGA :732 | MATCH :5858 | n/a | **MATCH** | — |

PORT-WIRING CHECK: render-overlay dispatch? **Y** @:5613; arrow dispatch? **Y** @:5806 slot0 (post-fill) + overlay :4546-4547 (slot0+slot1 pre-fill); input uses active_button fallback? **Y** @:9243 `selected_button=(s_button_index>=0)?s_button_index:s_selected_button` ✓ correct on both Track cycle and Direction toggle.

SCREEN VERDICT: input wiring is FAITHFUL (active_button fallback present). 1 DIVERGENCE-L2 (slot1 over-drawn arrows), 1 DIVERGENCE-L1 (Locked color).
Ordered fixes: (1) Remove `fe_draw_option_arrows(1)` :4547 (direction toggle isn't a ◄► cycler). (2) Locked color→white :4553.

---

### Screen 22 — ScreenExtrasGallery (0x00417D50)  [interactive Y — ESC/any-key/end → QUIT only]
Flow ORIG: 1 load 22 mugshots + 5 Legals · 6 clear secondary, set scroll=0x27f · **7 vertical scroll-reel** interleaving SNK_CreditsText text-lines + mugshot rows + Legals, wraps mod 0x280, quit when group-counter==0xb. Flow PORT `Screen_ExtrasGallery` @:9387: 0 load 1 TGA · 2 auto-advance one full-screen TGA / 4000ms; quit after 27 pages. Overlay `frontend_render_extras_gallery_overlay` @:5131.

| element/behavior | L1 creation | L2 render | L3 behavior | VERDICT | FIX |
|---|---|---|---|---|---|
| **Scroll-reel viewport (2 wrap halves)** | ARCH-DIV — orig two QueueFrontendOverlayRect halves (204,96) 320×320, scroll seam 0x140 | port draws ONE full-screen TGA scaled-to-fit :5147-5158, NO scroll | n/a | **ARCH-DIV** | structural reskin (paged slideshow). Faithful rebuild = secondary-surface vertical scroll reel — large, out of scope for a per-element fix. |
| **Credit text lines (SNK_CreditsText)** | MISSING — orig bakes SNK_CreditsText[10]={"TEST DRIVE 5","","(C) ACCOLADE","1998",...,"DEVELOPED BY","THE PITBULL SYNDICATE",""} into reel | absent — port shows only image TGAs | n/a | **MISSING (within ARCH-DIV)** | if keeping slideshow: at minimum render the 10 SNK_CreditsText lines as one credits page (now have exact strings from snk table). Otherwise fold into reel rebuild. |
| Mugshot images | DIVERGENCE — 22 dev mugshots present as full-screen pages vs 320-wide reel rows | drawn one-per-page :5139 | n/a | **DIVERGENCE (within ARCH-DIV)** | layout-only; content present. |
| 5 Legals surfaces | DIVERGENCE — Legals5..1 as first 5 pages :9379 | drawn | n/a | **DIVERGENCE (within ARCH-DIV)** | content present. |
| Per-image duration | DIVERGENCE [UNCERTAIN] 4000ms/page :9417 vs orig per-frame scroll | n/a | quit after 27 pages :9419 | **DIVERGENCE (within ARCH-DIV)** | duration is a documented guess. |
| Input → quit | MATCH "ESC always exits game" | n/a | MATCH global ESC + post-quit at end :9426 | **MATCH** | — |
| Title strip | MATCH none (returns NULL :740) | correct (no title) | n/a | **MATCH** | — |

PORT-WIRING CHECK: render-overlay dispatch? **Y** @:5633; arrow dispatch? **N** (no selectors — correct, screen has no menu buttons); input uses active_button fallback? **N/A** (no buttons).

SCREEN VERDICT: whole screen is **ARCH-DIV** (paged slideshow vs scroll reel). 1 MISSING-within-ARCH-DIV (SNK_CreditsText text never shown). No counted L1/L2/L3 fixes — exclude from totals. If a faithful rebuild is approved: rebuild state 7 as a vertical scroll-reel; until then add a credits-text page from SNK_CreditsText[10] (now available).

---

### Screen 23 — ScreenPostRaceHighScoreTable (0x00413580)  [interactive Y]
Flow: 0 init MainMenu.tga + nav-bar(0) + OK(1) + score panel · 3 slide-in · **6 interactive (◄► browse track, OK exit)** · 7/8 exit. Port `Screen_PostRaceHighScore` @:9451; overlay `frontend_render_high_score_overlay` @:4717; row baker reference orig `DrawPostRaceHighScoreEntry 0x00413010`.

| element/behavior | L1 creation | L2 render | L3 behavior | VERDICT | FIX |
|---|---|---|---|---|---|
| Score panel 520×144 @(115,177) | MATCH :4725 + dark fill+border :4735 | MATCH | MATCH rebuilt per cycle | **MATCH** | — |
| **Column headers** | DIVERGENCE [LABEL]: orig (0x413010, CONFIRMED) NAME@0x10, BEST/TIME/LAP/PTS@0x80, CAR@0xe4, **AVERAGE@0x160 + SPEED below**, **TOP@0x1bc + SPEED below**. Port :4762-4776 = "NAME","BEST"/"TIME"/"LAP"/"PTS","CAR","AVG"+unit,"TOP"+unit | MATCH drawn | n/a | **DIVERGENCE (L1)** | (a) :4773 "AVG"→SNK_AvgTxt="AVERAGE"; :4775 "TOP"→SNK_TopTxt="TOP" (already correct). (b) **second line is SNK_SpdTxt="SPEED" NOT the MPH/KPH unit** — orig draws "SPEED" under both AVG & TOP; the MPH/KPH choice only affects the VALUE conversion, not the header. Change :4774 & :4776 `speed_suffix`→"SPEED". |
| **Highlighted (inserted) row** | n/a | bold via SmallTextb in orig; port has no bold atlas → color highlight acceptable | **BUG (L3): orig bolds row where `row_index==g_postRaceQualifyingScore`** (=inserted rank) [CONFIRMED 0x41303x `if (uVar3 == g_postRaceQualifyingScore) g_smallFontSurface=g_smallTextbSurface`]. Port :4784 hardcodes `(i==0)?gold:grey` — ALWAYS golds row 0, ignores `s_score_insert_pos` (set @:10280, NEVER read here) | **BUG (L3)** | :4784 replace `(i == 0) ? 0xFFFFCC44 : 0xFFE0E0E0` with `(i == s_score_insert_pos) ? 0xFFFFCC44 : 0xFFE0E0E0`. NOTE: `s_score_insert_pos` is a file-static defaulting 0 and only set in NAME_ENTRY; for the Records-browse entry path it stays stale → reset it to **-1** at Screen_PostRaceHighScore case0 (:9464 area, add `s_score_insert_pos = -1;`) so the Records screen highlights NO row (orig `g_postRaceQualifyingScore` is the qualifying rank, −1/unset on pure browse). |
| 5 score rows (rank/name/score/car/avg/top) | MATCH rank, name clip 13, score fmt, car clip 108px, speed convert | MATCH :4781-4832 | MATCH | **MATCH** | — |
| Body font | DIVERGENCE [FONT]: scaled BodyText (ts=0.55) vs SmallText/SmallTextb atlas → no bold | MATCH (renders) | n/a | **DIVERGENCE (ARCH-DIV)** | no dedicated small/bold atlas in port; color highlight substitutes for bold. Exclude. |
| Nav bar slot0 (NULL label) + ◄► | MATCH `frontend_create_button(NULL,115,93,520,32)` :9460; orig InitArrows(0,1) | MATCH nav-bar text+arrows drawn @:5833 (`s_inner_state>=6`) + :5842 | see ◄► range below | **MATCH** | — |
| OK slot1 "OK" | MATCH SNK_OkButTxt :9461 | MATCH | MATCH →state7→MainMenu :9498 | **MATCH** | — |
| **◄► browse cycle (L3 input)** | n/a | n/a | **port :9490 uses RAW `frontend_option_delta()` with NO active_button gate**; orig cycles only on button0 (the sole selector). Functionally OK here (OK doesn't cycle), but violates the active_button rule + lacks orig disabled-round-skip + cheat-widen (port plain wrap 0..0x19 :9495) | **DIVERGENCE (L3)** | :9489-9497 gate cycle on `int ab=(s_button_index>=0)?s_button_index:s_selected_button; if (ab==0 && delta!=0){...}`. (Disabled-round-skip/cheat-widen is a documented simplification — lower priority; the active_button gate is the faithful-input fix.) |
| Header title strip | MATCH HighScoresText.TGA :733 | MATCH :5858 | n/a | **MATCH** | — |

PORT-WIRING CHECK: render-overlay dispatch? **Y** @:5622 (+ NAME_ENTRY reuse @:5631); arrow dispatch? **Y** nav-bar special-case @:5833/5842 (not in the :5784 switch — handled by the dedicated HIGH_SCORE block); input uses active_button fallback? **N** @:9490 (raw delta, no gate) → fix above.

SCREEN VERDICT: 1 BUG-L3 (inserted-rank highlight — the headline bug), 1 DIVERGENCE-L1 (AVG/SPEED+TOP/SPEED headers), 1 DIVERGENCE-L3 (raw delta + no skip/cheat).
Ordered fixes: (1) Highlight inserted rank :4784 `i==s_score_insert_pos` + reset `s_score_insert_pos=-1` at case0 :9464. (2) Headers: "AVG"→"AVERAGE", second lines→"SPEED" :4773-4776. (3) active_button gate on ◄► :9490.

---

### Screen 24 — RunRaceResultsScreen (0x00422480)  [interactive Y]
Flow: 0 init/sort + nav-bar(0)+OK(1) · 3 slide-in (early-exit→0xC when no race data) · **6 interactive (◄► browse racer card→7/9 slide, OK→0xB)** · 7-10 card-swap · 0xC cleanup · 0xD build action menu (5 btns) · 0xE slide-in · **0xF action-menu interactive** · 0x10 dispatch · 0x11 save dialog. Port `Screen_RaceResults` @:9536; overlay `frontend_render_race_results_overlay` @:4857.

| element/behavior | L1 creation | L2 render | L3 behavior | VERDICT | FIX |
|---|---|---|---|---|---|
| Results panel 408×392 @(152,81) | MATCH :4876-4878; no body fill (colorkey parity) :4887 | MATCH (gated states 3..0xB) | MATCH slide states 7-10 | **MATCH** | — |
| Result rows / per-game-type ladder | MATCH labels EXTRACTED from Language.dll (SNK_ResultsTxt/DRResultsTxt/CCResultsTxt) :4952; per-gt start-Y + step 0x18 :5016 byte-faithful | MATCH :5025-5128 | MATCH gt-gated | **MATCH** | — |
| Per-racer detail column @x=0x118 | MATCH right_col +280 :5002; values via td5_game accessors | MATCH | MATCH cycles with card | **MATCH** | — |
| Checkpoint-timer splits | DIVERGENCE: orig loops short[] @slot+0x34e all splits; port emits best-lap as 1 line :5103 | MATCH (1 line) | n/a | **DIVERGENCE (documented)** | low priority; multi-split list would crowd panel. |
| Nav bar slot0 (NULL) + ◄► | MATCH `create_button(NULL,...,0x208,0x20)` :9621 | MATCH arrows @:5784 switch has **no RACE_RESULTS case** → arrows NOT drawn on nav bar; but nav bar is invisible click-catcher (orig arrows armed via card panel) | **L3 cycle works** :9690 via `s_arrow_input` | **DIVERGENCE (L2 minor)** | orig arms InitArrows(0,1) on the card cycler (when gt!=9). Port draws no ◄► glyphs on the results nav bar. If the orig shows ◄► on the card browser, add `case TD5_SCREEN_RACE_RESULTS: fe_draw_option_arrows(0, sx, sy); break;` to :5784 (gated `s_inner_state==6`, gt!=9). [UNCERTAIN if orig glyphs are visible — nav bar is a wide invisible bar; low priority.] |
| OK slot1 "OK" | MATCH SNK_OkButTxt :9622 | MATCH | MATCH idx<2→0xB :9706 | **MATCH** | — |
| **◄► card cycle (L3 input)** | n/a | n/a | port :9690 uses raw `s_arrow_input` (no active_button gate). Only selector is nav bar(0); orig gates on button0. Functionally OK but not the active_button idiom | **DIVERGENCE (L3 minor)** | :9690 optionally gate `int ab=(s_button_index>=0)?s_button_index:s_selected_button;` before honoring arrow. Low priority (single selector). |
| Action menu (state 0xD) 5 buttons | MATCH dims 0x120×0x20 step 0x30 :9881; English labels (SNK_RaceAgain/ViewReplay/ViewRaceData/SelectNewCar/Quit & NextCupRace/Save/OK) content-faithful :9875-9885 | MATCH | MATCH press→0x10 dispatch :9912 | **MATCH (L1 content)** | — |
| **Replay/RaceData disabled-preview** | **MISSING** — orig renders ViewReplay/ViewRaceData as greyed `CreateFrontendDisplayModePreviewButton` when `g_replayFileAvailable==0`/no race-data; port always normal :9882-9883 | always enabled | always clickable | **MISSING (L1+L3)** | :9882-9883 create as disabled/preview buttons when replay/data unavailable (set `s_buttons[i].disabled=1` → state-2 grey render @:5707; bb_state already handles). Need a port `s_replay_available`/race-data flag; [UNCERTAIN] if surfaced — gate accordingly. |
| Save dialog (state 0x11) | MATCH SNK_BlockSavedOK="BLOCK SAVED OK"/SNK_FailedToSave="SAVE FAILED"; port "Block Saved OK"/"Failed to Save" :9961 | MATCH | MATCH | **DIVERGENCE (L1 minor)** | port "Failed to Save" vs SNK "SAVE FAILED" — cosmetic. Optional: align text. |
| Action-menu input (state 0xF) | n/a | n/a | MATCH `s_button_index>=0`→dispatch :9913 (press-only menu, no selectors — active_button N/A) | **MATCH** | — |
| Header title strip | MATCH ResultsText.tga :734; gated states 3..0xB :5854 (orig title released at 0xC) | MATCH | n/a | **MATCH** | — |

PORT-WIRING CHECK: render-overlay dispatch? **Y** @:5636; arrow dispatch? **N** (no RACE_RESULTS case in :5784 switch — nav-bar ◄► glyphs not drawn; cycle still works via s_arrow_input); input uses active_button fallback? **state6 N** (raw s_arrow_input :9690), **state0xF N/A** (press-only menu, correct).

SCREEN VERDICT: 1 MISSING-L1+L3 (Replay/RaceData disabled-preview), 1 DIVERGENCE-L2 (nav-bar ◄► glyphs absent, UNCERTAIN if orig shows them), 1 DIVERGENCE-L3 (raw arrow no active_button gate, minor), 1 DIVERGENCE-L1 (Save-fail text), checkpoint-splits documented.
Ordered fixes: (1) Grey ViewReplay/ViewRaceData when unavailable :9882-9883 (needs replay/data-available flag). (2) [if orig shows them] add RACE_RESULTS nav-bar arrow case :5784. (3) active_button gate :9690 (minor). (4) Save-fail text :9961 (cosmetic).

---

## SUMMARY (ARCH-DIV excluded from counts)

| Screen | interactive | L1 fixes | L2 fixes | L3 fixes | Top fix | ARCH-DIV |
|---|---|---|---|---|---|---|
| 20 CarSelection | Y | 2 (Beast/Beauty MISSING, Locked color/pos) +1 minor (Back gate) | 1 (Stats arrows undrawn) | 2 (Stats ◄► cycle, paint/config reset) | **Stats slot2 ◄► must cycle wheel-scheme 0..3 + draw arrows (L2+L3)** | INFO sub-mode, STATS-hdr lang, anim, highlight |
| 21 TrackSelection | Y | 1 (Locked color) | 1 (slot1 over-drawn arrows) | 0 (input faithful) | Remove ◄► glyphs from Direction toggle :4547 | dir-gate, anim, highlight |
| 22 ExtrasGallery | Y(quit) | 0 | 0 | 0 | (whole screen ARCH-DIV) add SNK_CreditsText page | paged-slideshow vs scroll-reel (entire screen) |
| 23 HighScore | Y | 1 (AVG→AVERAGE / SPEED headers) | 0 | 2 (inserted-rank highlight BUG, active_button gate) | **Highlight inserted rank `i==s_score_insert_pos` not row 0 (the BUG)** | body font (no bold atlas), disabled-skip/cheat |
| 24 RaceResults | Y | 1 (Replay/Data preview MISSING) +1 minor (Save text) | 1 (nav ◄► glyphs, UNCERTAIN) | 1 minor (active_button gate) | Grey ViewReplay/ViewRaceData when data absent | highlight, anim, checkpoint-splits, title gating |

Counted fixes (excl ARCH-DIV): **L1=6, L2=3, L3=5** across 4 interactive screens (S22 = whole-screen ARCH-DIV, 0 counted).

GLOBAL TOP FIX: **S23 inserted-rank highlight** (`td5_frontend.c:4784` `i==s_score_insert_pos`, + reset to -1 at case0) — the only true CONFIRMED BUG with a Ghidra-verified original behavior and a one-line port fix; `s_score_insert_pos` is already tracked (:323/:10280) and merely never read by the overlay.
RUNNER-UP: **S20 Stats slot2 ◄►** — must cycle `g_carSelectWheelSchemeTransient` 0..3 AND be added to the arrow render dispatch (:5801), currently dead on both L2 and L3.

File: C:/Users/maria/Desktop/Proyectos/TD5RE/re/analysis/frontend_fixlist/fix_20.md
