# Frontend PORT-vs-ORIGINAL Diff — Screens 0–4

Sources: ORIGINAL spec `re/analysis/frontend_screens/screens_00.md` (Ghidra VAs);
engine model `re/analysis/frontend_rendering_model.md` + `frontend_flow_model.md`;
PORT `td5mod/src/td5re/td5_frontend.c` (worktree `fix-1780170781-137846-13319`).

Class legend: MATCH / BUG / DIVERGENCE / MISSING / EXTRA / ARCH-DIV.
Cross-cut tags: [FONT] [DECOUPLED] [ANIM] [LABEL] [ASSET].

Architecture note (applies to all 5): the original draws via a per-frame
`FlushFrontendSpriteBlits` that drains queued overlay rects, then UNCONDITIONALLY runs
`UpdateExtrasGalleryDisplay` (gallery cover-art cross-fade) + `RenderFrontendDisplayModeHighlight`
(4-bar selection outline), then the color-keyed sprite list. The PORT replaces this with an
immediate-mode `td5_frontend_render_ui_rects` (td5_frontend.c:5489) that draws a BG quad, an
optional gallery slideshow (`frontend_render_bg_gallery`), a per-screen overlay switch (5547),
then buttons (5620). There is NO `case` for screens 0/1/2/3 in that switch, so those screens emit
no screen-specific overlay; only the BG quad + gallery + buttons + cursor appear. This is the root
of most MISSING rows below. The port's only text font is a single 24×24 BodyText atlas
(FONT_CELL=24, td5_frontend.c:552; `fe_draw_text` 5137) — the original's 12×12 SmallText/BodyText
and 36×36 MenuFont paths are collapsed into it ([FONT], affects every text element).

---

## Screen 0 — ScreenLocalizationInit (orig 0x004269D0 / port Screen_LocalizationInit td5_frontend.c:6010)

Pure data/config init. Original draws NOTHING (spec line 37): no surface load, no fill, no text,
no overlay/sprite enqueue; exits via SetFrontendScreen on its only active frame.

| element/behavior | ORIGINAL (addr) | PORT (line) | CLASS | FIX |
|---|---|---|---|---|
| 3-state boot gate | `g_frontendBootDispatchMode` 0/1/2 (spec 25-32, @0x0042717a) | `s_attract_mode_ctrl` 0/1/2 (6015/6024/6031) | MATCH | — |
| boot==2 → RACE_RESULTS | PUSH 0x18 @0x00427190, sets `g_replayFileAvailable=1`, reset boot→1 (spec 29-32) | sets `s_results_skip_display=1`, `s_attract_mode_ctrl=1`, set_screen(RACE_RESULTS) (6015-6021) | MATCH | — |
| boot==2 volume re-quantize | DXSound::GetVolume/CDGetVolume → `g_persistedMaster/CdVolumePercent` rounded to /10, re-applied (spec 30-32) | absent (6015-6021) | MISSING | low-impact (volume already persisted via Config.td5); add master+CD volume requantize on resume-cup path or annotate as intentional-skip |
| boot==1 → MAIN_MENU | PUSH 0x5 @0x00427173 (spec 28) | set_screen(MAIN_MENU) (6024-6028) | MATCH | — |
| boot==0 full init then →MAIN_MENU | full localization/config init, set boot=1, PUSH 0x5 (spec 26-27) | `td5_frontend_init_resources()`, `s_attract_mode_ctrl=1`, set_screen(MAIN_MENU) (6033-6061) | MATCH (ARCH-DIV internals) | — |
| per-car localization string tables | per-car `config.eng` parse, sscanf 17 tokens, SNK_ LUT substitution, UNKNOWN fallbacks (spec 38) | done lazily in td5_asset / `frontend_load_car_spec_fields` per comment (6037-6047) | ARCH-DIV | none — relocated to asset layer, not this fn |
| Joystick binding copy / device-desc reset / display-mode list build | spec 40-42, all boot==0 data | inside `td5_frontend_init_resources` / td5_input / td5_render per comments (6048-6054) | ARCH-DIV | none |
| draws anything | NONE (spec 37,50) | NONE (no overlay case) | MATCH | — |
| input | non-interactive (spec 55) | non-interactive | MATCH | — |

Cross-cutting tags touched: none (no draw).
Counts: BUG 0 / DIVERGENCE 0 / MISSING 1 / EXTRA 0 / ARCH-DIV 3.

---

## Screen 1 — ScreenPositionerDebugTool (orig 0x00415030 / port Screen_PositionerDebugTool td5_frontend.c:6074)

Original = a glyph-offset positioner dev tool with a live editable glyph ruler. No exit-to-menu in
its body (spec 112). PORT reimplements it as a generic 2-button screen and is essentially a stub.

| element/behavior | ORIGINAL (addr) | PORT (line) | CLASS | FIX |
|---|---|---|---|---|
| Positioner.tga load | state 0: `LoadFrontendTgaSurfaceFromArchive(Positioner.tga,FrontEnd.zip)` → ptr stashed in `g_frontendButtonIndex`, reused as caret sprite src (spec 89, @0x00415030) | `frontend_load_tga("Positioner.tga")` only — auto-set as bg, never used as caret src (6078) | DIVERGENCE [ASSET] | tool is dev-only/unreachable; acceptable, but caret-sprite reuse is gone |
| Backbuffer clear color 0 | `ClearBackbufferWithColor(0)` state 0 (spec 90) | absent (6076-6083) | MISSING | dev tool; low priority |
| 2 reference scanlines y=276,y=268 | `FillPrimaryFrontendScanline(0x114/0x10c, 0xff)` (spec 91) | absent | MISSING | add 2 white full-width rows if tool is restored |
| Glyph ruler strip (±8, 36×36 menu-font cells, scrolling) | `RenderPositionerGlyphStrip` 0x00414f40 → QueueFrontendOverlayRect per glyph (spec 92) | absent — no glyph strip, no overlay case for screen 1 (whole tool) | MISSING [DECOUPLED] | restore glyph ruler if tool wanted; else mark dev-stub |
| Per-glyph caret bar (y=220, 4×40) | RenderPositionerGlyphStrip (spec 93) | absent | MISSING | — |
| End-of-strip / state-4 marker glyphs (y=312 / y=348) | RenderPositionerGlyphStrip + QueueFrontendOverlayRect (spec 94-95) | absent | MISSING | — |
| Save buttons | NONE — orig has no buttons; advance via 0x40000 confirm bit (spec 84,111) | EXTRA "Save"(120,400,96,32)+"Back"(232,400,112,32) buttons (6079-6080) | EXTRA | remove invented buttons if matching orig; harmless for unreachable dev tool |
| Glyph-index/offset nav (±1 / ±8) | edge bits 0x1/0x2/0x200/0x400 move `g_positionerSelectedGlyphIndex` / nudge offsets; 0x40000 steps 3→4→5 (spec 106-111) | `frontend_option_delta()` bumps `s_anim_tick` (meaningless), button 0/1 jump states (6092-6116) | BUG | offset tables `g_positionerGlyphRects*` not modeled; nav edits nothing |
| positioner.txt write | state 5: fopen 0x465930, dump both 0x25-row tables, loop back to state 3 (spec 82) | logs a message, set_screen(MAIN_MENU) (6117-6120) | MISSING | no file written; loops to menu not back to nav |
| exit-to-menu | NONE in body (spec 112) | set_screen(MAIN_MENU) on button 1 / state 5 (6101,6119) | EXTRA | port invented an exit (acceptable since orig tool is otherwise a dead-end) |
| ANIM | input-driven scroll only, NOT counter (spec 102) | `s_anim_tick` misused as edit value (6094) | DIVERGENCE [ANIM] | — |

Cross-cutting tags touched: [ASSET], [DECOUPLED], [ANIM].
Counts: BUG 1 / DIVERGENCE 2 / MISSING 5 / EXTRA 2 / ARCH-DIV 0.
(Screen is a dev tool, normally unreachable; faithfulness impact low — flag as stub rather than fix.)

---

## Screen 2 — RunAttractModeDemoScreen (orig 0x004275A0 / port Screen_AttractModeDemo td5_frontend.c:6144)

Non-interactive (spec 146). Fades the held main-menu frame to black, then launches the in-engine
attract demo race. PORT FSM is case-for-case faithful; the only gap is the replay/demo nature.

| element/behavior | ORIGINAL (addr) | PORT (line) | CLASS | FIX |
|---|---|---|---|---|
| state 0: attract flag + present + activate cursor | `g_attractModeDemoActive=1`@0x004275b1, PresentPrimaryFrontendBuffer, ActivateFrontendCursorOverlay (spec 121) | `s_attract_demo_active=1`, present, `frontend_set_cursor_visible(0)` (6147-6152) | MATCH | — |
| state 1: release menu buttons | `ReleaseFrontendDisplayModeButtons` (spec 123) | `frontend_reset_buttons()` (6157) | MATCH | — |
| states 2,3: 2-frame settle present | PresentPrimaryFrontendBufferViaCopy ×2 (spec 124) | `frontend_present_buffer()` ×2 (6162-6168) | MATCH | — |
| state 4: arm fade-to-black | `InitFrontendFadeColor(0)` (spec 125) | `frontend_init_fade(0)` (6172) | MATCH | — |
| state 5: fade then launch | RenderFrontendFadeEffect each frame; on done InitializeRaceSeriesSchedule + InitializeFrontendDisplayModeState (spec 126-130) | `frontend_render_fade()` then `frontend_init_race_schedule()`+`frontend_init_display_mode_state()` (6176-6182) | MATCH | — |
| fade visual = 64-row dither wipe band, +2 rows/frame | RenderFrontendFadeEffect 0x00411780 (spec 136, model 158-173) | full-screen translucent black quad, alpha += 16/frame, done at 256 (`frontend_render_fade` 1592) | DIVERGENCE [ANIM] | port fade is a uniform alpha ramp not a top-to-bottom dither band; speed differs (16 steps vs ~240-row scan). Cosmetic; align if dither look wanted |
| demo race itself = engine replay / input-playback | engine drives replay after launch; `g_attractModeTrackIndex` preset by LOOP (spec 130,146) | `frontend_init_race_schedule()` launches a NORMAL race on `s_attract_track` (rand%8, 3293); no input playback | ARCH-DIV | input-playback/replay codec not ported; attract "demo" is a live AI race — acceptable divergence, annotate |
| attract track pick range | LOOP picks `rand()%0x13` (mod 19) skipping disabled entries (flow 41) | `s_attract_track = rand()%8` (3293) | DIVERGENCE | widen to rand()%19 w/ disabled-entry skip for parity if desired |
| input | non-interactive (spec 146) | non-interactive | MATCH | — |

Cross-cutting tags touched: [ANIM].
Counts: BUG 0 / DIVERGENCE 3 / MISSING 0 / EXTRA 0 / ARCH-DIV 1.

---

## Screen 3 — ScreenLanguageSelect (orig 0x00427290 / port Screen_LanguageSelect td5_frontend.c:6191)

Interactive. Original shows a flag-sheet (Language.tga) with FOUR 176×128 flag-IMAGE buttons in
the screen corners over LanguageScreen.tga, plus a localized "LANGUAGE SELECT" header. Pressing any
flag is non-committal (no locale write) and exits to LEGAL_COPYRIGHT (spec 196-201). PORT renders
no header, no flag images, and instead creates four 180×32 TEXT buttons stacked at the left.

| element/behavior | ORIGINAL (addr) | PORT (line) | CLASS | FIX |
|---|---|---|---|---|
| Full-screen bg LanguageScreen.tga | `LoadTgaToFrontendSurface16bpp` → primary (spec 174, @0x4667d0) | `frontend_load_tga("LanguageScreen.tga")` auto-bg (6196), drawn by 5527 BG quad | MATCH [ASSET] | — |
| Flag sheet Language.tga | `LoadFrontendTgaSurfaceFromArchive(Language.tga)` → flag-sheet surface (spec 156) | `frontend_load_tga("Language.tga")` loaded but UNUSED as a sprite source (6195) | BUG [ASSET] | load as a sprite atlas and source the 4 flag rects from it (src-y 0/0x80/0x100/0x180) |
| Header "LANGUAGE SELECT" | `DrawFrontendLocalizedStringPrimary(@0x4667c0, uVar2, (H/5-0x1c)>>1)` baked into bg (spec 175) | absent — no LANGUAGE_SELECT case in overlay switch (5547+) | MISSING [LABEL][FONT] | add header text in a new `frontend_render_language_select_overlay`; string from SNK_ |
| 4 flag-IMAGE buttons (176×128, corners) | 4× `CreateFrontendMenuRectEntry(...,0xb0,0x80, src-y 0/128/256/384, flagsheet)` at TL/TR/BL/BR (spec 176-179) | 4 generic 180×32 TEXT buttons "English/French/German/Spanish" stacked at (120,180..300) (6197-6200) | BUG [ASSET][LABEL] | replace with 4 flag-image rects sourced from Language.tga at the 4 corner positions (layout math uVar1/uVar2 spec 155) |
| Button labels hardcoded English | flags are images (no text) (spec 176) | hardcoded "English/French/German/Spanish" strings (6197-6200) | EXTRA [LABEL] | orig has no per-flag text; remove labels when switching to flag images |
| missing 4th language | src-y rows 0/128/256/384 = 4 flags (lang 0..3) (spec 176-179) | port labels English/French/German/Spanish — orig spec lists 4 generic rows; Italian likely present in orig sheet | DIVERGENCE | confirm flag count/order against Language.tga sheet; orig is 4 rows |
| selection highlight bars | `RenderFrontendDisplayModeHighlight` 4 edge bars (spec 180) | port button highlight ramp (5629) on its own buttons | DIVERGENCE [DECOUPLED] | acceptable port equivalent; will land on flag rects once those exist |
| flag choice writes NO locale | KEY FINDING: only stores hover idx + anim counter; ALL flags → set_screen(4) (spec 196-201) | stores `s_flow_context=button_index`, advances to exit (6219-6221) — also non-committal | MATCH | — (harmless extra store) |
| present/settle gate | states 1,2: AdvanceFrontendTickAndCheckReady 3-frame, DeactivateCursor, reset anim (spec 164) | state 1 present; state 2 `s_anim_tick+=2` until >=16 (6205-6216) | DIVERGENCE [ANIM] | timing approximated; cursor-deactivate not mirrored |
| exit to LEGAL_COPYRIGHT | PUSH 0x4 @0x00427474 (spec 169) | set_screen(LEGAL_COPYRIGHT) state 6 (6234-6235) | MATCH | — |
| input dispatch | state 4 hover/press; press advances (spec 192-195) | state 3 reads button 0..3, advances (6218-6223) | MATCH | — |

Cross-cutting tags touched: [ASSET], [LABEL], [FONT], [DECOUPLED], [ANIM].
Counts: BUG 2 / DIVERGENCE 3 / MISSING 1 / EXTRA 1 / ARCH-DIV 0.

---

## Screen 4 — ScreenLegalCopyright (orig 0x004274A0 / port Screen_LegalCopyright td5_frontend.c:6245)

Non-interactive 3-second copyright splash: load LegalScreen.tga into secondary, tile the copyright
string ~14 rows down the canvas, fade-out-cross in, dwell 3000 ms, fade-in-to-black out → MAIN_MENU.

| element/behavior | ORIGINAL (addr) | PORT (line) | CLASS | FIX |
|---|---|---|---|---|
| LegalScreen.tga bg | `LoadTgaToFrontendSurface16bppVariant` → SECONDARY surface (spec 227, @0x004274c0) | `frontend_load_tga("LegalScreen.tga")` auto-bg, drawn by 5527 BG quad (6249) | MATCH (ARCH-DIV: secondary surface concept dropped) [ASSET] | — |
| Tiled "TEST DRIVE 5 COPYRIGHT 1998" | `DrawFrontendLocalizedStringSecondary(@0x466808)` loop x=W/10(64), y=32 step 32, `(H-0x20)>>5`≈14 rows (spec 228) | `frontend_render_legal_copyright_overlay` (5319): x=64*sx, y=32*sy step 32, 14 rows, k_copyright literal (5328-5331) | MATCH [LABEL][FONT] | string hardcoded English (should be SNK_ localized); 24px font vs native 12×12 SmallText-class — both shared-foundation tags, not screen-specific |
| copyright string source | LANGUAGE.DLL @0x466808 (spec 228, localized) | hardcoded `"TEST DRIVE 5 COPYRIGHT 1998"` (5321) | DIVERGENCE [LABEL] | route through SNK_/Language string if non-English locales matter |
| Fade-OUT cross (reveal legal) | `ResetFrontendFadeState`(state 0)+`RenderFrontendFadeOutEffect`(state1) blends primary→secondary, 64-row dither band +2/frame (spec 229) | state 1: `s_anim_tick+=2` until >=16, no actual fade draw (6259-6266) | BUG [ANIM] | state 1 does NOT render any fade (no `frontend_render_fade` call); it just counts to 16 then stamps the dwell time. The reveal cross-fade is missing entirely |
| 3000 ms dwell | state 2: `timeGetTime()-stamp > 2999` (spec 230,238) | state 2: `(timeGetTime()-s_anim_tick) > 2999` (6268-6272) | MATCH | — |
| Fade-IN to black (exit) | `InitFrontendFadeColor(0)`(state2)+`RenderFrontendFadeEffect`(state3) 64-row band (spec 230) | state 3: `s_anim_tick+=2` until >=16, no fade draw (6275-6280) | BUG [ANIM] | state 3 also renders no fade — just counts to 16. Both fades are visually absent; screen pops in/out with no transition |
| exit to MAIN_MENU | PUSH 0x5 (spec 222) | set_screen(MAIN_MENU) (6278) | MATCH | — |
| input | non-interactive (spec 242) | non-interactive | MATCH | — |
| ANIM driver | fades by fade scan-cursor (+2/frame), real-time 3s dwell (spec 237-238) | `s_anim_tick` counts frames; dwell uses timeGetTime (6260-6276) | DIVERGENCE [ANIM] | fade-band motion not modeled |

Cross-cutting tags touched: [ASSET], [LABEL], [FONT], [ANIM].
Counts: BUG 2 / DIVERGENCE 2 / MISSING 0 / EXTRA 0 / ARCH-DIV 0.

---

## Cross-cutting summary (fix-once foundations)

- **[FONT]** Screens 3 (header), 4 (tiled copyright): port renders all text via one 24×24 BodyText
  atlas (`fe_draw_text` 5137, FONT_CELL 24 @552). Original uses 12×12 SmallText/BodyText for the
  non-LANGUAGE.DLL (default English) path and only switches to 24×24 when `SNK_LangDLL[8]==0x30`.
  Port text is ~2× native size. Fix in `fe_draw_text`/font setup, not per-screen.
- **[LABEL]** Screens 3 (flag labels + header), 4 (copyright): hardcoded English literals vs
  LANGUAGE.DLL `SNK_`/`@0x4667c0`/`@0x466808` strings. Fix via a shared string-table lookup.
- **[ANIM]** Screens 2, 3, 4: original transitions are a 64-row top-to-bottom dither WIPE band
  (+2 rows/frame); port `frontend_render_fade` (1592) is a uniform alpha ramp, and on screen 4 the
  fade is not even invoked in states 1/3. Fix the fade primitive once + wire it into screen 4.
- **[DECOUPLED]** Screens 1 (glyph ruler), 3 (selection bars): elements the original draws from the
  flush (`RenderPositionerGlyphStrip`, `RenderFrontendDisplayModeHighlight`) are absent or replaced;
  the port has no per-screen overlay case for screens 0/1/2/3.
- **[ASSET]** Screen 1 (Positioner.tga as caret), Screen 3 (Language.tga flag sheet unused).

## Per-screen counts {BUG / DIVERGENCE / MISSING / EXTRA / ARCH-DIV}
- Screen 0 LocalizationInit: 0 / 0 / 1 / 0 / 3
- Screen 1 PositionerDebugTool: 1 / 2 / 5 / 2 / 0  (dev tool, normally unreachable — low priority)
- Screen 2 AttractModeDemo: 0 / 3 / 0 / 0 / 1
- Screen 3 LanguageSelect: 2 / 3 / 1 / 1 / 0
- Screen 4 LegalCopyright: 2 / 2 / 0 / 0 / 0
