# Frontend 3-Layer Faithfulness Fix List — Screens 0–4

Method: every element/behavior verified against ALL THREE layers, NOT just creation.
- **L1 CREATION** = button created / global primed with correct label (real SNK_ string), pos, size, gating.
- **L2 RENDERING** = element actually DRAWN. Port render paths checked:
  - overlay dispatch `switch(s_current_screen)` @ td5_frontend.c:5588 → `frontend_render_<screen>_overlay`.
  - arrow dispatch `switch` @ td5_frontend.c:5784 → `fe_draw_option_arrows(idx,...)` (gated on `s_anim_complete`).
  - button loop @ :5661 (draws every active, non-hidden button).
  - shared/decoupled flush: `frontend_render_bg_gallery` :5586, mouse-hover highlight :5768.
- **L3 BEHAVIOR/INPUT** = the interactive inner-state reacts correctly; correct global written; wrap-vs-clamp; nav target. KEY PATTERN for arrow-cycle selectors: `int active_button = (s_button_index >= 0) ? s_button_index : s_selected_button;` then gate on `active_button` (because `s_button_index == -1` for keyboard-arrow input). NOTE: **none of screens 0–4 has an ◄► arrow-cycle selector** (verified vs screens_00.md), so the `active_button` fallback bug class does NOT apply to any of these 5 screens; the `s_button_index == N` gates here are plain button-press gates, which is correct.

Input-state facts (verified):
- `s_button_index` (:187) = -1 except the frame a button is confirmed (set =`s_selected_button` on Enter @:2262, or =hit button on mouse-confirm @:2311). `s_input_ready` set true the same frame.
- `frontend_load_tga` (:819) sets `s_background_surface` to the LAST background-like load and FREES the previously-loaded background surface (:884-891). Consequence used below for screen 3.
- Arrow dispatch (:5784) and overlay dispatch (:5588) have **NO case** for screens 0,1,2,3. Screen 4 LEGAL_COPYRIGHT has an overlay case @:5640.

SNK source strings (re/analysis/frontend_snk_strings.md): no `SNK_` string exists for "LANGUAGE SELECT" or "TEST DRIVE 5 COPYRIGHT 1998" — these are **hardcoded LANGUAGE.DLL .data strings** (@0x4667c0 / @0x466808), NOT SNK_ exports. So the L1 "label must be a real SNK_ string" rule resolves to "use the confirmed @addr literal" for screens 3/4 (there is no SNK_ symbol to route through).

---

### Screen 0 — ScreenLocalizationInit  [interactive N]
Flow (spec screens_00.md:22-59; port Screen_LocalizationInit :6058): pure data/config init, draws NOTHING. 3-state boot gate on `s_attract_mode_ctrl` (orig `g_frontendBootDispatchMode` @0x4a2c8c): ==2 → set skip_display, route RACE_RESULTS (0x18); ==1 → route MAIN_MENU (5); ==0 → full init, set ctrl=1, route MAIN_MENU. No surface/fill/text/overlay. Render model: not in any dispatch switch — correct, since it primes no draw globals.

| element/behavior | L1 creation | L2 render | L3 behavior | VERDICT | FIX |
|---|---|---|---|---|---|
| boot gate 0/1/2 | ctrl 0/1/2 @:6063/6072/6079 = orig @0x42717a | n/a (no draw) | routes match: 2→RACE_RESULTS @:6068, 1→MAIN_MENU @:6075, 0→init+MAIN_MENU @:6108 | FAITHFUL | — |
| boot==2 → RACE_RESULTS + skip flag | `s_results_skip_display=1`, ctrl→1 @:6066-67 = orig PUSH 0x18 @0x427190, g_replayFileAvailable=1 | n/a | set_screen(RACE_RESULTS) @:6068 | FAITHFUL | — |
| boot==2 volume re-quantize | absent @:6063-69; orig re-quantizes master+CD volume to /10 (spec 30-32) | n/a | n/a | DIVERGENCE (low-impact) | Optional: in boot==2 path (:6063) re-read+round master/CD volume to nearest 10 and re-apply, OR annotate as intentional-skip (volume already persists via Config.td5). NOT counted (cosmetic, non-visual). |
| draws anything | NONE = orig (spec 37) | NONE (no overlay case — correct) | n/a | FAITHFUL | — |
| per-car localization tables / joystick copy / display-mode list | relocated to td5_asset/td5_input/td5_render (:6093-6100) | n/a | n/a | ARCH-DIV | none — engine-layer reorg, not this fn |
| input | non-interactive = orig (spec 55) | n/a | reads no input | FAITHFUL | — |

PORT-WIRING CHECK: in render-overlay dispatch? N (correct — draws nothing). In arrow dispatch? N (correct — no selector). Input uses active_button fallback? N/A (non-interactive).
SCREEN VERDICT: **Faithful** (0 real fixes). 1 noted DIVERGENCE (boot==2 volume re-quantize, optional/non-visual) + 3 ARCH-DIV. No L1/L2/L3 functional fix required.

---

### Screen 1 — ScreenPositionerDebugTool  [interactive Y — dev tool, normally unreachable]
Flow (spec screens_00.md:63-115; port Screen_PositionerDebugTool :6122): orig is a glyph-offset positioner DEV tool: loads Positioner.tga (ptr stashed in g_frontendButtonIndex, reused as caret sprite src), clears backbuffer to black, draws 2 white reference scanlines (y=276/268), then in states 3/4 draws a scrolling 36×36 menu-font glyph ruler + caret bar + marker glyphs via `RenderPositionerGlyphStrip` (0x414f40, FLUSH/overlay path). Edge keys (0x1/0x2/0x200/0x400) move glyph index / nudge selected glyph's x/y offset; confirm bit 0x40000 steps 3→4→5; state 5 writes positioner.txt then loops to state 3. **No exit-to-menu in orig body.** Port reimplements as a generic 2-button ("Save"/"Back") screen that edits nothing.

| element/behavior | L1 creation | L2 render | L3 behavior | VERDICT | FIX |
|---|---|---|---|---|---|
| Positioner.tga load + caret-sprite reuse | loaded @:6126 but auto-set as plain bg, NOT stashed as caret src (orig spec 89) | drawn as full-screen bg quad @:5568 (no glyph case in overlay switch) | n/a | DIVERGENCE [ASSET] | Dev-tool: if restoring, load to a sprite handle (not bg) and pass as caret texid. |
| Backbuffer clear to black (state 0) | absent @:6124-31 (orig `ClearBackbufferWithColor(0)` spec 90) | not drawn | n/a | MISSING | Dev-tool, low pri. |
| 2 reference scanlines y=276/268 | absent (orig spec 91) | not drawn (no overlay case) | n/a | MISSING [L2] | Dev-tool: add 2 white full-width 1px rows if tool restored. |
| Glyph ruler strip (±8, 36×36, scrolling) + caret bar + marker glyphs | absent (orig `RenderPositionerGlyphStrip` spec 92-95) | **NO overlay case for screen 1** @:5588 → never rendered | n/a | MISSING [L2][DECOUPLED] | Dev-tool: add `case TD5_SCREEN_POSITIONER_DEBUG: frontend_render_positioner_overlay()` + impl, only if tool wanted. |
| Save/Back buttons | EXTRA "Save"(120,400,96,32)+"Back"(232,400,112,32) @:6127-28; orig has NO buttons (spec 84) | button loop draws them @:5661 | press → state5 / MAIN_MENU @:6146-49 | EXTRA | Dev-tool: remove invented buttons for parity; harmless otherwise. |
| Glyph index/offset nav (±1/±8) | `frontend_option_delta()` bumps `s_anim_tick` (meaningless) @:6142; offset tables `g_positionerGlyphRects*` not modeled | n/a (nothing rendered) | edits NOTHING — orig nudges selected-glyph x/y offset (spec 106-111) | BUG [L3] | Dev-tool: model `s_positioner_glyph[]` x/y offset arrays + index, nudge on edge bits. |
| positioner.txt write | logs only @:6166, no file (orig fopen+fprintf both tables spec 82) | n/a | set_screen(MAIN_MENU) instead of loop→state3 (spec 82) | MISSING [L3] | Dev-tool: write positioner.txt, then `s_inner_state=3`. |
| exit-to-menu | port invents MAIN_MENU exit @:6149/6167; orig has none (spec 112) | n/a | works | EXTRA | acceptable (orig is a dead-end). |

PORT-WIRING CHECK: in render-overlay dispatch? **N** (:5588 — should be Y if tool restored). In arrow dispatch? N (orig uses raw edge bits, not the selector arrow path — correct to be absent). Input uses active_button fallback? N/A (button-press + edge-bit nav, no arrow-cycle selector).
SCREEN VERDICT: **needs 0 fixes (dev-stub)** — screen is an unreachable dev tool (not in normal nav graph). Functionally it is a stub: L2 (no glyph ruler) and L3 (nav edits nothing) are broken, but faithfulness impact is ~nil. **Recommendation: mark as intentional dev-stub, do NOT fix unless the positioner tool is explicitly wanted.** If restored, ordered: (1) add overlay case + glyph-strip render [L2], (2) model glyph offset tables + nav [L3], (3) clear+scanlines [L2], (4) positioner.txt write + loop-to-3 [L3], (5) drop the 2 invented buttons.

---

### Screen 2 — RunAttractModeDemoScreen  [interactive N]
Flow (spec screens_00.md:119-149; port Screen_AttractModeDemo :6192): non-interactive. 6-state FSM (0..5): state0 set attract flag + present + cursor; state1 release menu buttons; states2/3 settle-present ×2; state4 arm fade-to-black; state5 run fade, on complete launch in-engine attract race. FSM is case-for-case faithful. Render: not in overlay/arrow dispatch — correct, it draws only the held main-menu frame + fade.

| element/behavior | L1 creation | L2 render | L3 behavior | VERDICT | FIX |
|---|---|---|---|---|---|
| state0 flag+present+cursor | `s_attract_demo_active=1`@:6197 = orig @0x4275b1; present @:6198 | present draws held frame | n/a | FAITHFUL | — |
| state1 release buttons | `frontend_reset_buttons()` @:6205 = orig ReleaseFrontendDisplayModeButtons | n/a | n/a | FAITHFUL | — |
| states2/3 settle present ×2 | @:6210/6215 = orig ×2 | present | n/a | FAITHFUL | — |
| state4 arm fade | `frontend_init_fade(0)` @:6220 = orig InitFrontendFadeColor(0) | n/a | n/a | FAITHFUL | — |
| state5 fade→launch | `frontend_render_fade()` then init_race_schedule+init_display_mode_state @:6225-28 = orig spec 126-130 | fade IS drawn (queues rect @:1613) | launch on fade done | FAITHFUL | — |
| fade visual | uniform alpha ramp +16/frame, done@256 (`frontend_render_fade` :1605) | drawn | n/a | DIVERGENCE [ANIM] | orig is a 64-row top→bottom dither WIPE band +2 rows/frame. Cosmetic; shared fade-primitive fix (affects screens 2/4), fix once in `frontend_render_fade`. Counted as 1 shared [ANIM] fix (see screen 4). |
| attract track pick range | LOOP idle picks `rand()%8` @:3306 (comment @:3221 deliberately narrows from orig `rand()%0x13`/19 to skip disabled tracks) | n/a | track set @:6801 (s_selected_track=s_attract_track) | DIVERGENCE | orig picks `rand()%0x13` (mod 19) skipping disabled entries (flow_model). Port narrows to %8 to avoid the disabled-skip table. Low-impact (still a valid attract track); widen to %19 + disabled-skip only if exact attract variety matters. NOT counted (behaviorally valid). |
| demo = engine replay vs live AI race | `frontend_init_race_schedule()` launches a normal AI race on s_attract_track (no input-playback codec) | n/a | n/a | ARCH-DIV | input-playback/replay codec not ported; acceptable. |
| input | non-interactive = orig | n/a | reads none | FAITHFUL | — |

PORT-WIRING CHECK: in render-overlay dispatch? N (correct — no screen-specific overlay; held frame + fade only). In arrow dispatch? N (correct). Input uses active_button fallback? N/A.
SCREEN VERDICT: **needs 1 fix (shared [ANIM], L2)** — the fade primitive (`frontend_render_fade` :1605) is a uniform alpha ramp, not the orig 64-row dither wipe band; this is shared with screen 4 (fix once). Plus 1 noted DIVERGENCE (attract track range %8 vs %19, behaviorally valid, not counted) and 1 ARCH-DIV (replay codec). Ordered: (1) replace uniform alpha with top→bottom dither band in `frontend_render_fade`.

---

### Screen 3 — ScreenLanguageSelect  [interactive Y]
Flow (spec screens_00.md:153-205; port Screen_LanguageSelect :6239): interactive flag-picker. Orig: load flag-sheet Language.tga + full-screen bg LanguageScreen.tga; draw header "LANGUAGE SELECT" (@0x4667c0); build FOUR 176×128 (0xb0×0x80) flag-IMAGE buttons in the 4 corners, each sourcing a different src-y row (0/0x80/0x100/0x180) of the flag sheet; layout uVar1/uVar2 (spec 155). Press ANY flag = NON-COMMITTAL (writes NO locale global) → exit to LEGAL_COPYRIGHT. Port: loads both TGAs but renders no header, no flag images; creates four 180×32 TEXT buttons "English/French/German/Spanish" stacked at left.

| element/behavior | L1 creation | L2 render | L3 behavior | VERDICT | FIX |
|---|---|---|---|---|---|
| full-screen bg LanguageScreen.tga | loaded @:6244, auto-set as bg (load_tga :891) | drawn by BG quad @:5568 | n/a | FAITHFUL [ASSET] | — |
| flag-sheet Language.tga | loaded @:6243 then `s_background_surface` overwritten by the LanguageScreen.tga load @:6244; **and the Language.tga surface is FREED @:889** if it's background-like (last-bg-wins). Never used as sprite src | NOT rendered (no overlay case for screen 3 @:5588; not a button src) | n/a | BUG [L2][ASSET] | Load Language.tga to a NON-bg sprite handle (use a dedicated loader / colorkey path so it is not treated as bg and not freed), keep the handle, source 4 flag rects from it (src-y 0/0x80/0x100/0x180, each 176×128). |
| header "LANGUAGE SELECT" | absent — no string drawn (orig DrawFrontendLocalizedStringPrimary @0x4667c0, x=uVar2, y=(H/5-0x1c)>>1, spec 175) | **NO overlay case for screen 3** @:5588 → never rendered | n/a | MISSING [L2][LABEL] | Add `case TD5_SCREEN_LANGUAGE_SELECT: frontend_render_language_select_overlay(sx,sy)` @:5588 + impl drawing "LANGUAGE SELECT" (literal @0x4667c0; no SNK_ symbol exists) at x=((640>>1)-0xb0)>>1 *sx region per spec. |
| 4 flag-IMAGE buttons (176×128, corners) | 4 generic 180×32 TEXT buttons stacked at (120,180/220/260/300) @:6245-48; orig = 4 image rects at TL/TR/BL/BR (spec 176-179) | text buttons drawn by button loop :5661; flag images NOT drawn | press → advance (works) | BUG [L1][L2] | Replace the 4 text buttons with 4 flag-image rects (176×128) at the 4 corners: TL=(uVar2, H/5+uVar1), TR=(W-uVar2-0xb0, …), BL=(uVar2, H-uVar1-0x80), BR=(W-uVar2-0xb0, …), sourcing Language.tga rows. uVar2=((W>>1)-0xb0)>>1, uVar1=((H-H/5)>>1-0x80)>>1 (spec 155). |
| per-flag English labels | hardcoded "English/French/German/Spanish" @:6245-48; orig flags are IMAGES with NO text (spec 176) | text drawn | n/a | EXTRA [LABEL] | Remove the text labels when switching to flag images. |
| flag-count/order | port lists 4 (Eng/Fr/Ger/Spa); orig sheet has 4 rows (src-y 0/128/256/384) — likely Eng/Fr/Ger/Ita (LangDLL byte 0x34=Italian per :6087) | n/a | n/a | DIVERGENCE [LABEL] | Confirm 4th-row flag against Language.tga sheet (port says Spanish; LANGUAGE.DLL lang-byte map @:6087 has 0x34=Italian, 0x35=Spanish → 4 rows are likely Eng/Fr/Ger/Ita). [UNCERTAIN until sheet visually inspected.] |
| selection highlight | port mouse-hover green border @:5768 on its buttons; orig RenderFrontendDisplayModeHighlight 4-bar around hovered flag (spec 180) | drawn on text buttons | n/a | DIVERGENCE [DECOUPLED] | acceptable port equiv; lands on flag rects once those exist. |
| present/settle gate | state1 present; state2 `s_anim_tick+=2` until≥16 @:6258-64; orig AdvanceFrontendTickAndCheckReady 3-frame + DeactivateCursor (spec 164) | n/a | n/a | DIVERGENCE [ANIM] | timing approximated; cursor-deactivate not mirrored. Low pri. |
| flag press = NO locale write | `s_flow_context=s_button_index` @:6268 (harmless), advances; orig writes NO locale (spec 196-201) | n/a | press button 0-3 → state4→6 → exit @:6267-83 | FAITHFUL | — (non-committal, matches orig) |
| input dispatch | state3 reads `s_button_index 0..3` @:6267 (plain button-press gate — NOT an arrow-cycle, so no active_button fallback needed) | n/a | advances to exit; all 4 → LEGAL_COPYRIGHT | FAITHFUL | — |
| exit to LEGAL_COPYRIGHT | set_screen(LEGAL_COPYRIGHT) state6 @:6283 = orig PUSH 0x4 | n/a | works | FAITHFUL | — |

PORT-WIRING CHECK: in render-overlay dispatch? **N** (:5588 — MUST add for header + flag images). In arrow dispatch? N (correct — orig has no ◄► selector here, it's a hover-pick). Input uses active_button fallback? N/A (plain button-press, correct as-is).
SCREEN VERDICT: **needs 3 fixes** (L1: 1 — flag-button creation; L2: 2 — flag images + header overlay; L3: 0). **MOST IMPORTANT: add the LANGUAGE_SELECT overlay case @:5588 and render the 4 corner flag IMAGES sourced from Language.tga (currently loaded-then-freed, drawn nowhere) + the "LANGUAGE SELECT" header — the entire visual identity of the screen is missing.** Ordered: (1) add overlay dispatch case + impl; (2) load Language.tga as a non-bg sprite handle (stop it being freed @:889); (3) replace the 4 text buttons with 4 corner flag-image rects (drop the English labels); (4 optional) confirm 4th flag = Italian vs Spanish.

---

### Screen 4 — ScreenLegalCopyright  [interactive N]
Flow (spec screens_00.md:209-246; port Screen_LegalCopyright :6293): non-interactive 3-second copyright splash. Orig 4-state: state0 load LegalScreen.tga into SECONDARY surface + tile "TEST DRIVE 5 COPYRIGHT 1998" (@0x466808) ~14 rows down (x=W/10, y=32 step 32), arm fade; state1 fade-OUT cross (reveal, primary→secondary, 64-row dither band); on done stamp dwell timer; state2 dwell 3000 ms (>2999); state3 fade-IN to black, on done → MAIN_MENU. Port renders the tiled copyright via overlay case (good) but BOTH fades are no-op frame counters.

| element/behavior | L1 creation | L2 render | L3 behavior | VERDICT | FIX |
|---|---|---|---|---|---|
| LegalScreen.tga bg | loaded @:6297 auto-bg; orig → SECONDARY surface (concept dropped) | drawn by BG quad @:5568 | n/a | FAITHFUL (ARCH-DIV: secondary-surface concept) [ASSET] | — |
| tiled "TEST DRIVE 5 COPYRIGHT 1998" | `frontend_render_legal_copyright_overlay` :5360: x=64*sx, y=32 step 32, 14 rows = orig (spec 228) | **rendered** — overlay case present @:5640 | n/a | FAITHFUL (L2 wired) [LABEL][FONT] | string hardcoded literal @:5362 = orig @0x466808 (no SNK_ symbol exists for it — correct to hardcode the confirmed literal). Font 24px vs orig 12×12 = shared [FONT] foundation, not screen-specific. |
| copyright string source | hardcoded "TEST DRIVE 5 COPYRIGHT 1998" @:5362 = orig LANGUAGE.DLL @0x466808 literal | drawn | n/a | FAITHFUL | — (no SNK_ export; literal is the faithful source) |
| Fade-OUT cross (reveal, state1) | state1 just `s_anim_tick+=2` until≥16 then stamps dwell @:6307-13; **NO fade render call** | **NOT drawn** — `frontend_render_fade()` never called in state1 (orig ResetFrontendFadeState+RenderFrontendFadeOutEffect, spec 229) | counts to 16 then advances | BUG [L2][ANIM] | In state1 call `frontend_init_fade` (state0) + `frontend_render_fade()` each frame; advance to state2 (stamp dwell) only when fade returns done. The reveal cross-fade is entirely missing. |
| 3000 ms dwell | state2 `(timeGetTime()-s_anim_tick)>2999` @:6317 = orig (spec 230,238) | n/a | works | FAITHFUL | — |
| Fade-IN to black (exit, state3) | state3 `s_anim_tick+=2` until≥16 @:6324-26; **NO fade render call** (orig InitFrontendFadeColor(0)+RenderFrontendFadeEffect, spec 230) | **NOT drawn** | counts to 16 → MAIN_MENU | BUG [L2][ANIM] | In state3 call `frontend_init_fade(0)` (on entry) + `frontend_render_fade()` each frame; exit to MAIN_MENU only when fade done. Currently pops with no transition. |
| exit to MAIN_MENU | set_screen(MAIN_MENU) @:6326 = orig PUSH 0x5 | n/a | works | FAITHFUL | — |
| input | non-interactive = orig (spec 242) | n/a | reads none | FAITHFUL | — |
| fade primitive style | shared `frontend_render_fade` uniform alpha ramp not 64-row dither band | — | — | DIVERGENCE [ANIM] | shared with screen 2 — fix once in `frontend_render_fade` :1605 (dither band). |

PORT-WIRING CHECK: in render-overlay dispatch? **Y** @:5640 (copyright tiling rendered — good). In arrow dispatch? N (correct — non-interactive). Input uses active_button fallback? N/A (non-interactive).
SCREEN VERDICT: **needs 2 fixes (both L2/[ANIM])** + 1 shared fade-primitive [ANIM] (same fix as screen 2). **MOST IMPORTANT: wire `frontend_render_fade()` into states 1 and 3 — both transitions are currently invisible no-op frame counters (the screen pops in/out with no fade).** Ordered: (1) state1 arm+run fade-out cross; (2) state3 arm+run fade-in-to-black; (3 shared) make `frontend_render_fade` a top→bottom dither band (covers screens 2+4).

---

## Roll-up (real fixes by layer; ARCH-DIV excluded)

- **Screen 0 LocalizationInit** [N]: L1 0 / L2 0 / L3 0. **Faithful.** Noted: 1 DIVERGENCE (boot==2 volume re-quantize, non-visual, optional) + 3 ARCH-DIV (per-car localization / joystick / display-mode list relocated to engine layer).
- **Screen 1 PositionerDebugTool** [Y, dev-stub]: L1 0 / L2 0 / L3 0 counted. **Dev-stub — recommend no fix.** If restored: L2 2 (glyph ruler + overlay case, scanlines), L3 2 (offset nav, positioner.txt), +remove 2 EXTRA buttons. 0 ARCH-DIV.
- **Screen 2 AttractModeDemo** [N]: L1 0 / L2 1 (shared fade dither) / L3 0. Noted: attract-track %8-vs-%19 DIVERGENCE (behaviorally valid). 1 ARCH-DIV (replay codec).
- **Screen 3 LanguageSelect** [Y]: L1 1 (4 flag-image buttons replace text) / L2 2 (flag images + "LANGUAGE SELECT" header — add overlay case) / L3 0. 0 ARCH-DIV. (Selection-highlight + present-gate DIVERGENCEs noted, acceptable.)
- **Screen 4 LegalCopyright** [N]: L1 0 / L2 2 (wire fade into state1 + state3) / L3 0, + shares the fade-primitive [ANIM] fix with screen 2. 0 ARCH-DIV.

**Shared/fix-once foundations:** [ANIM] `frontend_render_fade` (:1605) uniform-ramp → 64-row dither band (screens 2,4). [FONT] all text 24px vs orig 12×12 SmallText (screen 4 copyright + screen 3 header) — `fe_draw_text`/font setup, not per-screen, EXCLUDED from per-screen counts.
