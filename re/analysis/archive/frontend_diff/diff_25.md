# Frontend PORT-vs-ORIGINAL Diff — Screens 25–29

Authoritative faithfulness gap list for the 5 dialog/name-entry screens.
ORIGINAL = `re/analysis/frontend_screens/screens_25.md` (+ rendering/flow models).
PORT = `td5mod/src/td5re/td5_frontend.c` (paths in this worktree).
CLASS ∈ {MATCH, BUG, DIVERGENCE, MISSING, EXTRA, ARCH-DIV}.
Cross-cutting tags: [FONT] [DECOUPLED] [ANIM] [LABEL] [ASSET].

Architectural note (applies to all 5): the port is a D3D11 immediate-mode renderer. The
original "screen-fn primes / per-frame FLUSH draws" two-phase model
(`FlushFrontendSpriteBlits 0x00425540` + DECOUPLED gallery/highlight) does not exist; the port
calls `frontend_render_*_overlay` live from `td5_frontend_render_ui_rects` each frame. Baked
DDraw label/panel/button surfaces (`CreateTrackedFrontendSurface` + `Draw*StringToSurface`)
are replaced by live `fe_draw_text`/`fe_draw_quad`. Treat the whole INLINE-bake-vs-live-draw
substitution as one ARCH-DIV (not re-listed per element). The two unconditional DECOUPLED draws
(`UpdateExtrasGalleryDisplay 0x40d830`, `RenderFrontendDisplayModeHighlight 0x4263e0`) and the
LOOP mouse cursor run on every original screen but are handled generically in the port
[DECOUPLED] — selection-highlight edge bars (`0xc000`, 4-edge inset) are not reproduced as such.

---

## Screen 25 — ScreenPostRaceNameEntry @0x00413bc0 (PORT Screen_PostRaceNameEntry, td5_frontend.c:10018)

| element/behavior | ORIGINAL (spec+addr) | PORT (line) | CLASS | FIX |
|---|---|---|---|---|
| FSM state count | 13 states 0–0xc (qualify→slide-in→edit→slide-out→insert→present→table-slide-in→static→OK→restore→slide-out→main-menu) | states 0–12 present, 0xb collapsed (10018-10276) | MATCH (shape) | — |
| Qualify gate | `score!=0 && g_raceParticlePoolBase[0x1f2].size_rate!=2 && g_twoPlayerModeEnabled==0`; mode bits drive points/lap/time metric (0x413bcf–c7e) | computes group_type, score, worst-entry compare, 2P + DNF gate (10038-10106) | MATCH | — |
| Default name when entry empty | orig empties buffer; on commit if len==-2 copies `g_localComputerName` (computer name) as default (spec 91) | port pre-fills `"PLAYER"` (10110-10111); no empty→computer-name fallback on commit | BUG [LABEL] | seed buffer empty; on confirm, if name empty copy host/computer name (GetComputerNameA) not literal "PLAYER" |
| Name input model | `DXInput::GetString(&buffer)` middleware edit-box; `DXInput::SetAnsi(1)`; ENTER sets edit-state(0x004969d0)=2 | Win32 `GetAsyncKeyState` poll loop, VK 0x30–0x5A + SPACE + BACK + RETURN (2419-2477) | ARCH-DIV | document only; no faithful port (no DXInput middleware) |
| Caret | green 2×16 bar (`BltColorFillToSurface 0xff00`) drawn AFTER text pixel-WIDTH; blink = `g_frontendAnimFrameCounter & 0x20` (every 32 frames); suppressed if textwidth≥0x195; focused field bg `0x392152`, unfocused `0` (spec 50-51) | white 2px bar at `caret_index*11.2px` monospace assumption; blink 350ms wall-clock; color 0xFFFFFFFF (2534-2541) | BUG [ANIM][FONT] | caret should sit after measured text WIDTH (use fe_measure_text, not index*11.2 monospace); color green (0x00FF00); blink tied to anim frame counter not ms; add field-focus bg color toggle |
| Name box | baked `CreateFrontendDisplayModeButton(SNK_EnterPlayerNameButTxt,-0x1c0,0,0x1c0,0x40,0)` = 448×64 button labeled "ENTER PLAYER NAME", field inset x=0x14, slid via MoveFrontendSpriteRect (spec 48-49) | generic two-tone panel 448×80 at (96,280) drawn live; title text live; field inset 24px (2496-2532) | DIVERGENCE [LABEL] | acceptable port substitution; label text matches; box height 0x40=64 vs port 80, position (20,104) orig vs (96,280) port |
| Name-edit slide-in/out timing | counter target `0x20`=32 (state1 in, state3 out) (spec 73-76) | `s_anim_tick += 2; >= 0x10`=16 (10118-10122, 10135-10137) — half the duration | BUG [ANIM] | change threshold 0x10→0x20 in case 1 and case 3 (orig settles at 32, port at 16 → slide twice as fast) |
| Table slide-in timing | counter `0x27`=39, 3-frame `AdvanceFrontendTickAndCheckReady` settle gate (spec 74) | `s_anim_tick += 2; >= 0x12`=18 (10241-10244); no 3-frame settle | BUG [ANIM] | threshold 0x12→0x27; add 3-frame settle gate |
| Table slide-out timing | counter `0x10`=16 (state 0xc) (spec 76) | `s_anim_tick += 2; >= 16` (10263-10264) — MATCHES | MATCH | — |
| High-score table panel | `CreateTrackedFrontendSurface(0x208,0x90)` 520×144, black-filled, blitted at center; 2 sprite slots in slide (spec 52-54) | 520×144 panel at (115,177) with 1px white border + dark fill (4683-4697) | MATCH (geometry) | — |
| Inserted-row highlight (THE KNOWN BUG) | orig: `if (rowIndex == g_postRaceQualifyingScore) font = SmallTextb (alt/bold)` — the player's just-inserted row drawn in bold font (spec 60-61, "DrawPostRaceHighScoreEntry") | port hard-codes `(i==0)?gold:gray` (4743); `s_score_insert_pos` written (10208) but NEVER read by overlay | BUG [FONT] | same highlight-row bug as screen 23: highlight `i==s_score_insert_pos` (the inserted row), not `i==0`; use bold/alt font emphasis not gold-by-rank |
| Middle column header (mode-dependent) | `&3`: 0→TIME, 1→LAP, 2→PTS; BEST drawn at top (0x80) unless mode==2 (spec 61) | `score_type` switch: PTS / BEST+TIME / BEST+LAP (4724-4729) | MATCH | — |
| Column headers / X-bands | NAME 0x10, BEST/score 0x80, CAR 0xe4, AVG 0x160, TOP 0x1bc; SPD×2 under Avg/Top (spec 61) | NAME +16, score +128(0x80), CAR +228, AVG +352, TOP +444 (4700-4735) | DIVERGENCE | port CAR/AVG/TOP X-bands differ from orig (0xe4=228 OK; AVG 0x160=352 OK; TOP 0x1bc=444 OK) — actually matches; SPD suffix MATCH |
| Empty-row rendering | orig draws blank/space rows | port draws "---" gray for empty name (4747-4750) | EXTRA | minor; orig leaves blank — cosmetic |
| Name clip width | `DrawFrontendClippedStringToSurface(...,clip 0x60)`=96px (spec 62) | name copied 13 chars, no pixel clip to 0x60 (4756-4762) | DIVERGENCE | add 96px clip to NAME column |
| Car column source | `(&g_localizationCarManufScratch)[gExtCarIdToTypeIndex[car]*0xcc]` manufacturer string, clip 0xe4..0x150 (108px) (spec 62) | `frontend_get_car_display_name(cid)` clipped to 108px (4768-4783) | MATCH | — |
| Speed format | `%dMPH`/`%dKPH` per `gSpeedReadoutUnitsConfigShadow` (spec 62) | suffix in header only; rows print bare number (4786-4791) | DIVERGENCE | orig prints unit per cell; port shows unit in header band — acceptable layout choice |
| OK button | `CreateFrontendDisplayModeButton(SNK_OkButTxt,-0x130,0,0x60,0x20,0)` + blank 0x208×0x20 spacer; EscKeyButtonIndex=1, ButtonIndex=1 (spec 55) | OK button created via shared flow (case 4/5 path); spacer not reproduced | DIVERGENCE | OK x=-0x130 (orig) vs generic; spacer button missing (cosmetic) |
| sfx on commit | `DXSound::Play(4)` at state1 slide-in; (no explicit play on edit commit in spec) | `frontend_play_sfx(5)` on confirm (10129) | DIVERGENCE | verify sfx id; orig plays 4 at slide-in |
| Persist | orig writes via Config.td5 (`WritePackedConfigTd5`) as part of NPC group table | `td5_save_write_config(NULL)` at state12 end (10271) | MATCH | — |
| Exit target | `SetFrontendScreen(MAIN_MENU)` (spec 39) | `td5_frontend_set_screen(MAIN_MENU)` (10273) | MATCH | — |

Class counts (25): MATCH 9, BUG 5, DIVERGENCE 6, ARCH-DIV 1, EXTRA 1, MISSING 0.

---

## Screen 26 — ScreenCupFailedDialog @0x004237f0 (PORT Screen_CupFailed, td5_frontend.c:10285; overlay :5366)

| element/behavior | ORIGINAL (spec+addr) | PORT (line) | CLASS | FIX |
|---|---|---|---|---|
| Game-type gate | `0 < gametype < 7`, else immediate `SetFrontendScreen(MAIN_MENU)` (spec 121) | `<1 || >6` → MAIN_MENU (10289-10292) | MATCH | — |
| Background | `LoadTgaToFrontendSurface16bpp(MainMenu.tga,FrontEnd.zip)`→primary + copy secondary (spec 108) | `frontend_load_tga("Front_End/MainMenu.tga",...)` (10296); secondary copy implicit | MATCH | — |
| Dialog panel | `CreateTrackedFrontendSurface(0x198,0x70)` 408×112, black fill, centered (152,97) (spec 109) | live `fe_draw_quad` 408×112 at (152,97), 0xCC000000 translucent (5374-5382) | DIVERGENCE [ARCH] | orig panel is OPAQUE black-fill; port is 0xCC (80%) translucent — make opaque for parity |
| Message text (4 lines) | `SNK_SorryTxt` y0, `SNK_YouFailedTxt` y0x1c, `SNK_ToWinTxt` y0x38, race-type `*(SNK_RaceTypeText+gametype*4)` y0x54 (spec 110) | "SORRY"/"YOU FAILED"/"TO WIN"/cup-name at y 0/28/56/84 (5386-5395) | MATCH [LABEL] | — |
| Cup-type names | from Language.dll race-type table indexed gametype | k_cup_type_names[1..6] literals (5356-5364) | MATCH [LABEL] | — |
| OK button | `CreateFrontendDisplayModeButton(SNK_OkButTxt,-0x120,0,0x60,0x20,0)`; EscKeyButtonIndex=0 (spec 112) | `frontend_create_button("OK",-0x120,0,0x60,0x20)` (10307) | MATCH | — |
| Present/settle | states 1/2/3 = 3-frame present (spec 103) | case 1/2/3 present (10312-10315) | MATCH | — |
| Slide-in | counter `0x20`=32 → Deactivate cursor; X=`uVar1+anim*-0x18+600` (spec 111,119) | `s_anim_tick++; >=0x20` (10317-10322); no per-frame X slide of panel (overlay drawn static centered) | DIVERGENCE [ANIM] | port has no horizontal slide-in motion of the dialog; orig slides from right at 24px/frame |
| Wait OK | state5 `if(g_frontendButtonPressedFlag)` → `SetFrontendScreen(g_returnToScreenIndex)` (spec 125) | case5 `if(s_input_ready)` → MAIN_MENU hardcoded (10325-10330) | DIVERGENCE | orig exits to `g_returnToScreenIndex`; port hardcodes MAIN_MENU |
| Art | NONE (text-only) (spec 117) | none | MATCH | — |

Class counts (26): MATCH 6, DIVERGENCE 3, BUG 0, ARCH-DIV 0 (folded), MISSING 0, EXTRA 0.

---

## Screen 27 — ScreenCupWonDialog @0x00423a80 (PORT Screen_CupWon, td5_frontend.c:10340; overlay :5421)

| element/behavior | ORIGINAL (spec+addr) | PORT (line) | CLASS | FIX |
|---|---|---|---|---|
| Game-type gate | `0 < gametype < 7` else MAIN_MENU (spec 158) | `<1||>6`→MAIN_MENU (10343-10346) | MATCH | — |
| Delete CupData | `_unlink(g_cupDataTd5Filename)` (spec 133) | `frontend_delete_cup_data()` (10350) | MATCH | — |
| Unlock progression | orig awards unlocks (AwardCupCompletionUnlocks 0x421da0, gated finished) | `td5_save_apply_cup_unlocks_ex`, gated `slot_is_finished(0)` (10363-10395) | MATCH | — |
| Dialog panel | `CreateTrackedFrontendSurface(0x198,0xc4)` 408×196 (taller for unlock lines), centered (spec 139) | live quad 408×196 (0xC4) at (152,97), 0xCC000000 (5427-5436) | DIVERGENCE [ARCH] | panel translucent vs orig opaque — make opaque |
| Fixed message lines | `SNK_CongratsTxt` y0, `SNK_YouHaveWonTxt` y0x38, race-type y0x54 (spec 140) | "CONGRATULATIONS" y0, "YOU HAVE WON" y56, cup-name y84 (5439-5446) | MATCH [LABEL] | — |
| Unlock line 1 | `sprintf("%d %s")` gated `g_cupSchedule_currentCup!=0` at y0x8c=140 (spec 141) | `"%d NEW CARS UNLOCKED"` gated `s_cup_won_car_count!=0` at y140 (5447-5451) | DIVERGENCE [LABEL] | orig uses `"%d %s"` (number + unlocked-item NAME, not the static "NEW CARS UNLOCKED" string); port invented label. UNCERTAIN orig varargs (spec 151-154 unresolved) — but format template differs |
| Unlock line 2 | `sprintf("%d %s")` gated `g_cupSchedule_currentRound!=0` at y0xa8=168 (spec 142) | `"%d NEW TRACKS UNLOCKED"` gated `s_cup_won_track_count!=0` at y168 (5452-5456) | DIVERGENCE [LABEL] | same as line 1; gate is currentCup/currentRound in orig, not car/track counts |
| OK button | `SNK_OkButTxt,-0x120,0,0x60,0x20`; EscKeyButtonIndex=0 (spec 144) | `frontend_create_button("OK",-0x120,...)` (10402) | MATCH | — |
| Slide-in | counter `0x20`=32 (spec 156) | `>=0x20` (10412-10416); no horizontal slide motion | DIVERGENCE [ANIM] | no panel slide (same as 26) |
| Wait OK | `SetFrontendScreen(g_returnToScreenIndex)` (spec 161) | `s_input_ready`→MAIN_MENU hardcoded (10419-10423) | DIVERGENCE | exits to returnIndex in orig |
| Trophy/medal art | NONE (text-only) (spec 150) | none | MATCH | — |

Class counts (27): MATCH 6, DIVERGENCE 5, BUG 0, MISSING 0, EXTRA 0.

---

## Screen 28 — ScreenStartupInit @0x00415370 (PORT Screen_StartupInit, td5_frontend.c:10432)

| element/behavior | ORIGINAL (spec+addr) | PORT (line) | CLASS | FIX |
|---|---|---|---|---|
| Blank dialog panel | `CreateTrackedFrontendSurface(0x198,0x70)` 408×112, black-fill, centered (152,97), NEVER filled with text (spec 178) | NO panel drawn; loads MainMenu.tga + creates OK button only (10437-10438) | MISSING | orig shows an empty centered box for 3 frames; port shows none (functionally inert bootstrap, low priority) |
| Background image | orig: NO bg image load (spec 191) | port loads MainMenu.tga (10437) | EXTRA | orig does not load a bg here; remove for parity (minor) |
| OK button | `CreateFrontendDisplayModeButton(SNK_OkButTxt,-0x120,0,0x60,0x20,0)`; EscKeyButtonIndex=0 but never read (spec 180) | `frontend_create_button("OK",-100,0,100,0x20)` (10438) — x=-100, w=100 | DIVERGENCE | orig x=-0x120(-288), w=0x60(96); port -100/100 (cosmetic; button never interactive) |
| States 1–3 present | 3-frame static re-queue, zero anim counter, no animation (spec 187) | case 1/2/3 present (10442-10445) | MATCH | — |
| State 4 hand-off | `g_currentScreenFnPtr = g_frontendScreenFnTable[0]` = ScreenLocalizationInit @0x004269d0, `g_frontendInnerState = timeGetTime()` (spec 174) | `td5_frontend_set_screen(TD5_SCREEN_LOCALIZATION_INIT)` (10449) | MATCH | — |
| Input dispatch | NONE — auto-advances, no button read (spec 193) | no input read; auto-advances | MATCH | — |
| Cursor overlay | `ActivateFrontendCursorOverlay()` called (spec 181) | not explicitly activated | DIVERGENCE [DECOUPLED] | minor; transient screen |

Class counts (28): MATCH 3, DIVERGENCE 2, MISSING 1, EXTRA 1, BUG 0.

---

## Screen 29 — ScreenSessionLockedDialog @0x0041d630 (PORT Screen_SessionLocked, td5_frontend.c:10467; overlay :5468)

| element/behavior | ORIGINAL (spec+addr) | PORT (line) | CLASS | FIX |
|---|---|---|---|---|
| Reachability | entered from kicked-from-lobby net path (DXPTYPE) (spec 217-219) | s_kicked_flag never set by peer; reachable only by manual test (10459-10465) | ARCH-DIV | document only (no compatible peer) |
| Background | `LoadTgaToFrontendSurface16bpp(MainMenu.tga)` + copy secondary + `BlitFrontendCachedRect(0,0,cW,cH)` (spec 206) | `frontend_load_tga(MainMenu.tga)` (10472); no cached-rect restore | DIVERGENCE | orig has extra `BlitFrontendCachedRect`; port omits (cosmetic) |
| Dialog panel | `CreateTrackedFrontendSurface(0x198,0x70)` 408×112, centered (152,97) (spec 207) | live quad 408×112 at (152,97), 0xCC000000 (5472-5479) | DIVERGENCE [ARCH] | translucent vs orig opaque — make opaque |
| Message text (2 lines) | `SNK_SorryTxt` y0, `SNK_SeshLockedTxt` y0x38=56 (spec 208) | "SORRY" y0, "SESSION LOCKED" y56 (5481-5484) | MATCH [LABEL] | — |
| OK button | `SNK_OkButTxt,-0x120,0,0x60,0x20`; EscKeyButtonIndex=0 (spec 210) | `frontend_create_button("OK",-0x120,...)` (10481) | MATCH | — |
| Present/settle | states 1/2/3 (spec 201) | case 1/2/3 present (10486-10489) | MATCH | — |
| Slide-in | counter `0x20`=32 → Deactivate (spec 221) | `>=0x20` (10491-10495); no horizontal slide motion | DIVERGENCE [ANIM] | no panel slide (same as 26/27) |
| Exit target | HARDWIRED `SetFrontendScreen(MAIN_MENU)` (NOT returnIndex, unlike 26/27) (spec 201) | `s_input_ready`→MAIN_MENU (10498-10501) | MATCH | — (port hardwired matches orig here) |
| Lock art | NONE (text-only) (spec 211) | none | MATCH | — |

Class counts (29): MATCH 5, DIVERGENCE 3, ARCH-DIV 1, BUG 0, MISSING 0.

---

## Cross-cutting summary

[ANIM] Slide-in/out durations are systematically too fast: screen 25 edit-slide threshold 0x10
vs orig 0x20 (2×), table-slide 0x12 vs 0x27; screens 26/27/29 have NO horizontal slide motion of
the dialog at all (orig slides from right ~24px/frame, settle at counter 0x20). Caret blink on
screen 25 is wall-clock 350ms vs orig anim-frame-counter bit5.

[LABEL] Screen 25 default name "PLAYER" vs orig empty→computer-name. Screen 27 unlock lines use
invented "NEW CARS/TRACKS UNLOCKED" strings vs orig `"%d %s"` (count + item name); gated on
car/track counts vs orig currentCup/currentRound.

[FONT] Screen 25 inserted-row highlight bug (same as screen 23): port highlights row 0 gold-by-rank;
orig highlights the inserted row (`rowIndex==g_postRaceQualifyingScore`) with the bold SmallTextb
font. `s_score_insert_pos` is computed but never consumed by the overlay.

[ARCH] All dialog panels (26/27/29) drawn with 0xCC translucent fill vs orig opaque black-fill
surface. Whole baked-surface→live-draw substitution is one ARCH-DIV. [DECOUPLED] selection-highlight
edge bars (0xc000) not reproduced.

## Top 3 fixes
1. Screen 25 inserted-row highlight: read `s_score_insert_pos` (not `i==0`) and emphasize the
   player's row with the alt/bold font — fixes the carried-over screen-23 bug.
2. Screen 25 caret + slide timing: position caret after measured text width (green, anim-counter
   blink), and restore slide thresholds (edit 0x10→0x20, table 0x12→0x27 + 3-frame settle).
3. Screen 27 unlock lines: replace invented "NEW CARS/TRACKS UNLOCKED" with the orig `"%d %s"`
   count+name format gated on currentCup/currentRound; make dialog panels opaque (26/27/29).

File: C:/Users/maria/Desktop/Proyectos/TD5RE/re/analysis/frontend_diff/diff_25.md
