# Frontend PORT-vs-ORIGINAL Faithfulness Diff — Screens 20–24

RE basis: `re/analysis/frontend_screens/screens_20.md` (spec, Ghidra VAs),
`frontend_rendering_model.md`, `frontend_flow_model.md`.
PORT: `td5mod/src/td5re/td5_frontend.c` (+ `td5_frontend_button_cache.c`).
CLASS ∈ {MATCH, BUG, DIVERGENCE, MISSING, EXTRA, ARCH-DIV}.
Cross-cutting tags: [FONT] [DECOUPLED] [ANIM] [LABEL] [ASSET].
All PORT line numbers are `td5_frontend.c` unless noted.

---

## Screen 20 — CarSelectionScreenStateMachine (0x0040DFC0)

| element/behavior | ORIGINAL (spec+addr) | PORT (line) | CLASS | FIX |
|---|---|---|---|---|
| CarPic%d.tga preview panel | 408×280 pre-rendered 2D TGA, bottom-right; loaded per-car from car zip (state 0xc) | `frontend_render_car_selection_preview` :4364, surf load :774/:1152; drawn at (232,124,408,280) :4451 | MATCH | — |
| Preview slide-in (state 0xe/14) | `x = canvasW + anim*-0x40 + 0x4a8` over 25 frames (commit anim==0x19) | :4444 time-based `1832→232` over 800ms | DIVERGENCE [ANIM] | frame-count drive ok; orig is 25 frames @ counter, port uses ms — speed = monitor-rate-independent (acceptable ARCH-DIV) |
| Preview slide-out (state 0xb/11) | `x = 232 + anim*0x20` (out right) | :4436 time-based 433ms | MATCH (DIVERGENCE [ANIM]) | — |
| Blue fill behind car (FillPrimaryFrontendRect 0x5c) | orig fills DDraw primary 0x5c; preserved between frames | port re-fills each frame :4376–4424 (BGRA 0xFF00005C) | ARCH-DIV | clear-per-frame renderer requires explicit fill; faithful color |
| 3 intro strips (CarSelBar1/CarSelCurve/CarSelTopBar) slide-in (state 2) | bar from right `canvasW − anim*8`; topbar from slide x; 8px/frame | :4397–4411 time-based `636→36` / `−532→0` over 2500ms | DIVERGENCE [ANIM][ASSET] | strip positions/dims match; speed ms-driven |
| GraphBars.tga stat-bar atlas | loaded state 0; consumed for stat BARS | loaded :8579; **never drawn** — stats are text-only (:4277) | MISSING (matches orig: spec notes orig states 0xf/0x11 also draw text only) | leave; orig UNCERTAIN on bar render |
| Car name text | "%s %s" manuf+model, centered y=cy−0x97=89 | :4457 `frontend_draw_value_text(...,232,89,...)` | MATCH | — |
| Beast/Beauty tag (gametype 2) | `DrawFrontendLocalizedStringPrimary(Beast/Beauty)` at (cx−0xea,cy−0x77); car<8→Beauty | **absent** in port | MISSING | add Beast/Beauty label when `s_selected_game_type==2` at (86,121-ish) gated on car<8 |
| Locked tag | (cx−0xea,cy−0x77); locked && !cheat && gametype∉{8,5} | :4458–4462 "LOCKED" at (86,121), red 0xFFFF4444 | DIVERGENCE [LABEL] | orig uses SNK_LockedTxt (white, localized); port hardcodes red English. Gate matches (excl. 8/5 via lock check; note port omits the `gametype!=5` carve-out in the *render* gate but OK enforces it) |
| Header label (car-select title) | `CreateMenuStringLabelSurface(0xb/0xc/0xd)` by 1P/2P/championship; menu 36×36 font, slides anim*5 | port has NO baked menu-font header surface; relies on button/text labels | MISSING [LABEL][FONT] | port lacks the large MenuFont title strip on screens 20–24 (see cross-cutting) |
| "Car" selector slot0 + ◄► | `CreateFrontendDisplayModeButton(SNK_CarButTxt,...)`+arrows | :8694 "Car" + arrows via fe_draw_option_arrows | MATCH [LABEL] | label English (no Language.dll) — accepted ARCH-DIV |
| "Paint" slot1 + ◄► | SNK_PaintButTxt; cycles 0..3; disabled for cop cars | :8695 "Paint"; :8753 case1 wrap 0..3, cop-car guard 0x1C–0x24 | MATCH | — |
| "Config" slot2 → CONFIG stat panel | SNK_ConfigButTxt; press/◄► → state 0xf rebake | :8696 **"Stats"** label; case2 :8767 → state 15 | DIVERGENCE [LABEL] | orig label is "Config"; port shows "Stats". Also orig cycles `g_carSelectWheelSchemeTransient` (wheel scheme 0..3) on ◄►; port treats it as a press-only sub-screen toggle (no wheel-scheme cycle) → MISSING wheel-config cycling |
| Transmission slot3 (Auto/Manual) | SNK_AutoButTxt normal / SNK_ManualButTxt preview if gametype==7; toggle ^1 + rebuild | :8697 "Automatic"/"Manual"; case3 :8772 toggles, drag(gt9)+TT(gt7) lock | MATCH [LABEL] | label "Automatic" (orig "Auto") — cosmetic |
| OK slot4 | SNK_OkButTxt 64×32; locked→Play(10) | :8699 / case4 :8788 lock→sfx10 | MATCH | — |
| BACK slot5 | only if `restart==0 && flowPhase==0`; 96×32 | :8700 only if `!s_network_active` | DIVERGENCE | orig gate is restart/menu-flow, port gates on network only — different hide condition (likely benign but not faithful) |
| CONFIG stat panel (state 0xf) | SNK_Config_Hdrs + per-car values into secondary surface; '*'=skip; GraphBars | :4277 `frontend_render_car_stats_overlay` 14 rows from config.nfo; English hdrs | DIVERGENCE [FONT][LABEL] | functional; headers hardcoded English (Language.dll SNK_Config_Hdrs unavailable). Faithful enough |
| INFO stat panel (state 0x11) | 10 value rows from SNK_Info_Values (Language.dll) | case 17 :8883 **stubbed** (falls through, draws nothing) | MISSING | Language.dll export unavailable; orig INFO sub-mode not reproduced |
| Selection highlight edge bars | `RenderFrontendDisplayModeHighlight` color 0xc000, 4 bars | generic button highlight in render_ui_rects | ARCH-DIV [DECOUPLED] | port uses unified highlight path |
| Masters roster (gametype 5) | scans saved-car/mastersEncounter flags | :8613/:8735 s_masters_roster + flags | MATCH | — |
| Cup-mode skip TrackSelection (state 0x1a) | gametype 1..6 → InitializeRaceSeriesSchedule, no TrackSel | :8992 confirmed branch | MATCH | — |

Class counts S20: MATCH 9 · DIVERGENCE 7 · MISSING 4 · ARCH-DIV 3 · BUG 0

---

## Screen 21 — TrackSelectionScreenStateMachine (0x00427630)

| element/behavior | ORIGINAL (spec+addr) | PORT (line) | CLASS | FIX |
|---|---|---|---|---|
| Track preview TGA "Front End\Tracks\%s.tga" | 152×224 at (cx+0x5c,cy−0x69)≈(412,135); reloaded per cycle | :4499 (412,135,152,224); load :9142/:9242 | MATCH [ASSET] | — |
| Track name panel (city/country split on ',') | 296×184 panel, name from SNK_TrackNames split on comma; DAT_004658e4 subtitle | :4466 splits on ',', city @ y81 country @ y113, centered x=492 | MATCH [LABEL] | port renders both halves as two lines; orig replaces the comma-remainder with a format template — port keeps the raw country string (DIVERGENCE-lite, but visually closer) |
| Header label (track-select title) | `CreateMenuStringLabelSurface(10)` menu 36×36, slides anim*4 | **absent** | MISSING [LABEL][FONT] | no MenuFont title strip (cross-cutting) |
| "Track" slot0 + ◄► | SNK_TrackButTxt 224×32; cycle skips disabled/locked | :9129 "Track"; cycle :9172 frontend_cycle_track | MATCH | — |
| Direction toggle slot1 (reverse-capable gate) | only on-screen when `gNpcRacerCheatFlags[idx]!=0` (reverse-capable) && idx>=0; else parked x=−0xe0; toggles Forwards↔Reverse | :9130 "Forwards"; :9086 `frontend_update_direction_button_visibility` hides/disables via `td5_asset_track_has_reverse`; toggle :9208 | MATCH (ARCH-DIV) | orig gates on per-track unlock byte; port gates on reverse-data presence — equivalent for "circuit tracks never reverse" |
| Direction label | SNK_ForwardsButTxt / Reverse | :9211 "Forwards"/"**Backwards**" | DIVERGENCE [LABEL] | orig label is "Reverse"; port uses "Backwards" |
| OK slot2 | 96×32; locked→Play(10); else exit | :9131 / :9217 lock→sfx10 | MATCH | — |
| BACK slot3 | hidden when `g_mainMenuButtonHint==2` (QuickRace) | :9133 hidden when `s_flow_context==2` | MATCH | — |
| Slide-in (state 3) | 39 frames (counter pinned 0x26 awaiting tick) | :9158 `update_timed_animation(0x27,650)` ms | DIVERGENCE [ANIM] | ms-driven |
| Track-switch slide-in (state 8/9) | 16 frames; preview from right, text from above; play(4) at end | :9278 16-tick; :4473 img_x_off / txt_y_off; :9285 sfx4 | MATCH [ANIM] | — |
| Locked tag | SNK_LockedTxt if locked | :4507 "LOCKED" red at (412,375) | DIVERGENCE [LABEL] | hardcoded red English vs localized |
| 2P "?" random track (-1) | supported | :9124/:9178 -1 handling | MATCH | — |

Class counts S21: MATCH 7 · DIVERGENCE 4 · MISSING 1 · ARCH-DIV (folded into MATCH on dir-gate)

---

## Screen 22 — ScreenExtrasGallery (0x00417D50)

ARCHITECTURAL MISMATCH: orig is a **vertical scroll reel** of credit text + dev mugshots + 5 Legals pages baked into `g_secondaryWorkSurface` and scrolled by `g_frontendAnimFrameCounter` (wrap mod 0x280). PORT is a **paged slideshow** advancing one full-screen TGA every 4000ms.

| element/behavior | ORIGINAL (spec+addr) | PORT (line) | CLASS | FIX |
|---|---|---|---|---|
| Scrolling credits viewport (2 wrap halves) | two QueueFrontendOverlayRect halves at (204,96) 320×320; vertical scroll seam at 0x140 | :5090 `frontend_render_extras_gallery_overlay` draws ONE full-screen TGA scaled to fit, no scroll | DIVERGENCE (ARCH-DIV) | port = paged slideshow, NOT scroll reel |
| Credit text lines (SNK_CreditsText 0x18-stride) | localized text baked into reel every 0x20 scroll; '#'/'*'/text record types | **absent** — port shows only image TGAs (Legals + mugshots) | MISSING | no per-line credit TEXT rendered; orig interleaves text + mugshots in one scroll |
| Mugshot image rows | 22 dev mugshots, 7 per row group, color-keyed blit into reel | :5306 list of 22 mugshot TGAs shown one-per-page | DIVERGENCE | content present, layout wrong (full-screen pages not 320-wide rows) |
| 5 Legals surfaces | Legals1-5 loaded state 1, shown in reel | :5307 Legals5..1 as first 5 pages | DIVERGENCE [ASSET] | present but as separate full-screen pages |
| Per-image / scroll duration | per-frame scroll; group counter DAT_00496354==0xb (11 groups) → quit | :9345 4000ms/page; quit after 27 pages :9354 | DIVERGENCE [ANIM][UNCERTAIN] | port duration is a guess (commented UNCERTAIN); orig is frame-scroll |
| Input dispatch → quit | any masked key OR mouse OR credits-finished → `CleanUpAndPostQuit` | global ESC handler + :9354 post-quit at end | MATCH | "ESC always exits game" — preserved |
| Band-gallery slideshow | NOT this screen (music-test 19 path) | n/a | MATCH | correctly not here |

Class counts S22: MATCH 2 · DIVERGENCE 4 · MISSING 1 (whole screen is an ARCH-DIV reskin)

---

## Screen 23 — ScreenPostRaceHighScoreTable (0x00413580)

| element/behavior | ORIGINAL (spec+addr) | PORT (line) | CLASS | FIX |
|---|---|---|---|---|
| Score panel 0x208×0x90 (520×144) | `DrawPostRaceHighScoreEntry 0x413010` bakes into surface at (cx−0xcd,cy−0x3f)≈(115,177); black-fill backdrop | :4683 panel (115,177,520,144); :4694 opaque dark fill + 1px border | MATCH [ASSET] | port backdrop fidelity fix already applied |
| Column headers (NAME/BEST/CAR/AVG/TOP + TIME/LAP/PTS) | SNK_* via DrawFrontendSmallFontStringToSurface (SmallText); set varies by event type (&3: 0/1=Time,1=Lap,2=Pts) | :4718–4735 hardcoded English; two-line BEST/TIME-LAP, PTS single; AVG/TOP + unit | DIVERGENCE [LABEL][FONT] | headers hardcoded English; event-type gating MATCHES (score_type 1=LAP,2=PTS) |
| **Highlighted (inserted) row** | bold the **inserted rank** row `g_postRaceQualifyingScore`/`s_score_insert_pos` via **SmallTextb** (bold font surface) | :4743 `row_color = (i==0) ? 0xFFFFCC44(gold) : grey` — **always highlights row 0 with GOLD color** | **BUG** [FONT] | replace with `(i == s_score_insert_pos) ? bold/highlight : normal`; orig uses bold font not gold color. `s_score_insert_pos` already exists (:317, set :10208) but is **never read** by the overlay |
| 5 score rows (rank/name/time/car/avg/top) | rank sprintf, name clipped 0x60, time `%2.2d:%2.2d.%2.2d`/lap/pts, car manuf, speed | :4740–4792 full row render; name clip 13ch, car clip 108px | MATCH | — |
| Table body font | scaled SmallText (12×12 small atlas) | :4707 `ts=0.55` scaled BodyText (single atlas) | DIVERGENCE [FONT] | port scales BodyText not a dedicated SmallText/SmallTextb atlas — no bold variant available |
| Header label (high-score title) | `CreateMenuStringLabelSurface(7)` menu 36×36, slides anim*4 | **absent** | MISSING [LABEL][FONT] | no MenuFont title strip |
| Backing nav button slot0 + ◄► | NULL-label wide bar 0x208×0x20; ◄► cycles `g_postRaceRacerCardIndex` (track) | :9388 `frontend_create_button(NULL,115,93,520,32)` | MATCH | — |
| OK slot1 | SNK_OkButTxt 96×32; esc index=1; → MainMenu | :9389 "OK"; :9426 → state7 | MATCH | — |
| ◄► browse range | wrap 0..0x19 with **disabled-round skip**; cheat extends 0..0x12 | :9420–9424 plain wrap 0..0x19, **no skip, no cheat gate** | DIVERGENCE | orig skips disabled cup rounds + cheat-widens; port browses all 26 unconditionally (matches a documented simplification comment :9421) |
| MPH/KPH | by gSpeedReadoutUnitsConfigShadow | :4680 td5_save_get_speed_units | MATCH | — |
| Config flush on exit | InitializeFrontendDisplayModeState → WritePackedConfigTd5 | :9448 td5_save_write_config | MATCH | — |

Class counts S23: MATCH 6 · DIVERGENCE 3 · MISSING 1 · **BUG 1**

---

## Screen 24 — RunRaceResultsScreen (0x00422480)

| element/behavior | ORIGINAL (spec+addr) | PORT (line) | CLASS | FIX |
|---|---|---|---|---|
| Results panel 0x198×0x188 (408×392) | `DrawRaceDataSummaryPanel 0x421e90` bakes into surface at (cx−0xa8,iVar6)≈(152,81); black color-keyed (invisible) blit | :4816 panel (152,81,408,392); :4846 NO body fill (color-key parity) | MATCH [ASSET] | — |
| Result rows / single-slot stat sheet | LEFT col = localized labels (SNK_ResultsTxt/DRResultsTxt/CCResultsTxt) drawn once; RIGHT col @x=0x118 = per-slot values | :4911 k_results_labels (English, Language.dll-extracted); left_col + right_col @ +280 | DIVERGENCE [LABEL] | labels hardcoded English (extracted-verbatim, faithful content); per-game-type ladder MATCHES |
| Per-game-type row ladders | gt8(0x48), gt7/<1(0x60), gt9 skip-rows, 1/6, 2-5 (two hdr lines), step 0x18 | :4928–4977 tables + start-Y per gt | MATCH | start-Y and step byte-faithful |
| Checkpoint-timer splits | orig loops short[] at slot+0x34e (all lap splits) until 0 or y>=0x150 | :5056 emits ONLY best-lap as one line | DIVERGENCE | per-lap split list collapsed to single value (documented) |
| Header label (results title) | `CreateMenuStringLabelSurface(0xe)` menu 36×36, slides anim*4 | **absent** (title via cached ResultsText.tga page per :9762 comment, but no MenuFont strip) | MISSING/DIVERGENCE [LABEL][FONT] | no MenuFont title; ResultsText.tga referenced in comment but not drawn in overlay |
| Backing button slot0 + ◄► | NULL wide bar 0x208×0x20; cycles racer card | :9549 `create_button(NULL,...,0x208,0x20)` | MATCH | — |
| OK slot1 | SNK_OkButTxt 96×32; esc idx=0; press→state 0xb | :9550 "OK"; :9634 idx<2 → 0xb | MATCH | — |
| Card-swap slide (states 7/8/9/10) | panel slides anim*0x20, rebuild at anim==0x11; DNF-skip (state==3) | :9643–9731 slide_x ±0x20, slot cycle + state-3 skip at 0x11 | MATCH [ANIM] | byte-faithful slide constants |
| Action menu (state 0xd) | up to 5 buttons RaceAgain/Replay/RaceData/NewCar/Quit OR NextCup/Replay/RaceData/Save/OK|Quit; 0x120×0x20; Y step 0x30 | :9784–9813 5 buttons, RR_BW=0x120, Y step 0x30; is_cup logic | MATCH [LABEL] | English labels (no Language.dll) |
| Replay/RaceData disabled-preview | rendered as **preview (greyed)** buttons when data unavailable | :9810/:9811 always normal buttons | MISSING | orig greys ViewReplay/ViewRaceData when `g_replayFileAvailable==0`/no data; port always enabled |
| Quit branch (state 0x10) | 4-way: single→AwardUnlocks+NameEntry; cup-midprog→MainMenu; cup-won→CupWon; cup-failed→CupFailed | :9919–9956 4-way confirmed | MATCH | prior P6 fix — faithful |
| Save dialog (state 0x11) | WriteCupData → SNK_BlockSavedOK/SNK_FailedToSave + OK | :9961 "Block Saved OK"/"Failed to Save" | MATCH [LABEL] | English |
| Cup-fail early route (state 0/3) | gates on slot companion_2==2 / actor finish flag | :9502/:9573 confirmed | MATCH | — |
| Sort results | gt1/6 secondary desc; gt2-5 primary asc | :9522 td5_game_sort_results | MATCH | — |
| Panel alpha | orig opaque black color-keyed (text-only visible) | port no body fill (UNCERTAIN comment :4811) | MATCH | — |
| Selection highlight | RenderFrontendDisplayModeHighlight 0xc000 | generic button highlight | ARCH-DIV [DECOUPLED] | — |

Class counts S24: MATCH 11 · DIVERGENCE 2 · MISSING 2 · ARCH-DIV 1

---

## CROSS-CUTTING

- **[FONT]** No screen 20–24 renders the large **MenuFont (36×36) title strip** baked via `CreateMenuStringLabelSurface(id)` (orig labels 0xb/0xc/0xd/10/7/0xe). Port draws no top header banner on any of the 5 screens → consistent MISSING across 20/21/23/24. Also: table bodies (23) and stat panels (20) use **scaled BodyText**, not the dedicated **SmallText/SmallTextb** atlases — so no **bold** variant exists, which directly causes the S23 highlight bug.
- **[LABEL]** Every SNK_*/Language.dll localized string (button labels "Config"/"Auto"/"Reverse", LOCKED tag, column headers, result-row labels) is hardcoded English in the port (Language.dll unavailable) — pervasive DIVERGENCE, mostly faithful in content.
- **[ANIM]** Slide-in/out timings are ms-driven (`frontend_update_timed_animation`) instead of `g_frontendAnimFrameCounter` frame-count equality. Functionally close but monitor-rate-independent (orig speed = frame rate).
- **[DECOUPLED]** Selection highlight (orig `RenderFrontendDisplayModeHighlight` 0xc000) replaced by a unified port button-highlight path on all screens (ARCH-DIV).
- **[ASSET]** Screen 22 is the biggest structural deviation: paged full-screen TGA slideshow vs orig vertical scroll-reel that interleaves SNK_CreditsText text lines + mugshot rows + Legals.

## TOP 3 FIXES
1. **S23 high-score highlight BUG** (:4743): replace `row_color = (i==0)?gold:grey` with highlighting the **inserted rank** `s_score_insert_pos` (already tracked at :317/:10208 but unread). Faithful orig bolds that row via SmallTextb; port should at minimum gate the highlight on `i == s_score_insert_pos`.
2. **S22 scroll-reel** (:5090 / :9315): port is a paged slideshow with NO credit TEXT (SNK_CreditsText lines missing entirely). Rebuild as a vertical scroll reel interleaving credit text + mugshot rows to match orig.
3. **S20 Beast/Beauty + S20 "Config"→wheel-scheme cycle** missing: add the gametype-2 Beast/Beauty tag (:4457 area) and make slot2 ◄► cycle the wheel/config scheme 0..3 (orig `g_carSelectWheelSchemeTransient`), plus rename button label "Stats"→"Config".

File: `C:/Users/maria/Desktop/Proyectos/TD5RE/re/analysis/frontend_diff/diff_20.md`
