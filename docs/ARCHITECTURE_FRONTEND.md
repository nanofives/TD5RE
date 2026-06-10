# TD5RE Frontend Architecture

How the menu system works and how to add/modify screens. All paths relative to
`td5mod/src/td5re/`. Verified against source on this branch (2026-06); function
names are stable, line numbers drift.

## 1. Screen table & TU ownership

The frontend is one dispatch table of per-screen state machines.
`s_screen_table[TD5_SCREEN_COUNT]` (`td5_frontend.c`, ~line 81) mirrors the
original binary's 30-entry table at `0x4655C4`; the port extends it to **34**
entries (`TD5_SCREEN_COUNT = 34`, enum `TD5_ScreenIndex` in `td5_types.h`
~line 337). Indices 30–33 are port additions (MultiplayerLobby, LanMenu,
DirectConnect, NetNickname).

Since the 2026-06 split, screen handlers live in three cluster TUs; the core
helpers stay in `td5_frontend.c`. **`td5_frontend_internal.h` is the ONLY
shared-state seam** between these four TUs — every symbol there used to be a
file-scope static of the monolithic `td5_frontend.c`. Modules *outside* the
frontend must use only the public `td5_frontend.h` (init/shutdown,
`td5_frontend_set_screen/get_screen`, `td5_frontend_display_loop`, render-flush
helpers).

| Screens (index) | TU |
|---|---|
| Core: table, display loop, `set_screen`, input poll, draw/text/button/surface helpers, race schedule, titles, `render_ui_rects` | `td5_frontend.c` (~9.2k lines) |
| 0 LocalizationInit, 1 PositionerDebug, 2 AttractMode, 3 LanguageSelect, 4 LegalCopyright, 5 MainMenu, 6 RaceTypeCategory, 12 OptionsHub, 13–17 Game/Control/Sound/Display/TwoPlayer Options, 18 ControllerBinding, 19 MusicTest, 22 ExtrasGallery, 28 StartupInit | `td5_fe_menu.c` |
| 7 QuickRace, 20 CarSelection (incl. simultaneous-MP grid), 21 TrackSelection, 23 PostRaceHighScore, 24 RaceResults, 25 PostRaceNameEntry, 26 CupFailed, 27 CupWon | `td5_fe_race.c` |
| 8 ConnectionBrowser, 9 SessionPicker, 10 CreateSession, 11 NetworkLobby, 29 SessionLocked, 30 MultiplayerLobby, 31 LanMenu, 32 DirectConnect, 33 NetNickname | `td5_fe_net.c` |
| CPU-baked 224×64 main-menu button surfaces | `td5_frontend_button_cache.c` |

`td5_frontend_display_loop()` (core) runs once per frame while game state ==
MENU: anim pacing → input poll → `s_screen_table[s_current_screen]()` →
`td5_frontend_render_ui_rects()` + `td5_frontend_flush_sprite_blits()` →
present → ESC handling → cheat codes → attract timeout. Returns 1 when
`g_td5.race_requested`, −1 on quit.

## 2. Per-screen state machine convention

Each handler is `void Screen_X(void)` switching on the shared `s_inner_state`.
Canonical shape (verified in `Screen_OptionsHub`, `td5_fe_menu.c`):

- **state 0 (init)**: `frontend_init_return_screen(SELF)` (sets
  `s_return_screen` from the static parent map `frontend_get_parent_screen`),
  `frontend_load_tga(...)` background, `frontend_create_button(...)` per row,
  `frontend_begin_timed_animation()`, advance.
- **states 1–2**: `frontend_present_buffer()` warm-up frames.
- **slide-in state**: `frontend_update_timed_animation(max_tick, duration_ms)`
  drives `s_anim_tick`/`s_anim_t`; when it returns ≥ 1.0 set
  **`s_anim_complete = 1`** and advance. ESC and the attract timer are ignored
  until `s_anim_complete`; the universal slide-in chime fires at settle.
- **interactive state**: act on `s_input_ready && s_button_index >= 0`
  (confirm) and `s_arrow_input` (value cycling); set `s_return_screen` to the
  destination and jump to the slide-out states.
- **exit states**: `frontend_play_sfx(5)`, slide-out animation, then
  `td5_frontend_set_screen((TD5_ScreenIndex)s_return_screen)`.

`td5_frontend_set_screen(index)` is the single transition choke point: it
resets `s_inner_state = 0`, `s_anim_tick/s_anim_t/s_anim_complete`,
`frontend_reset_buttons()`, frees recyclable surfaces (keeping shared pages and
LRU-cached backgrounds), **clears `s_text_input_state`**, mirrors the screen
into `g_td5.frontend_*`, and arms default fade-out/fade-in SFX (de-duped against
screens that play their own). Frame-rate independence: per-frame animation
counters multiply by `s_fe_logic_ticks` (fixed 60 Hz reference computed in
`frontend_update_anim_pacing`).

## 3. Input

`frontend_poll_input()` (core) populates per-frame outputs: `s_input_ready`,
`s_button_index`, `s_arrow_input` (bits 1/2/4/8 = L/R/U/D),
`s_selected_button`, mouse state.

- **Keyboard nav = WM_KEYDOWN FIFO**: presses queue in the window proc; the
  poll drains `td5_plat_input_nav_pop()` (codes `TD5_NAVKEY_*`,
  `td5_platform.h`) through `frontend_apply_nav_event()` — exactly one move per
  press, in order. The immediate keyboard reads are activity-tracking ONLY;
  feeding them into moves would double-count. Gamepad d-pad/stick uses
  edge-detection through the same `frontend_apply_nav_event`.
- **`frontend_check_escape()`**: one-shot per frame — ESC edge OR gamepad B OR
  one queued WM_KEYDOWN ESC via `td5_plat_input_esc_taken()`. The display loop
  also runs a global ESC → `s_return_screen` pop, but only when
  `s_anim_complete` is set; screens that never set it need an explicit
  per-screen `frontend_check_escape()` call.
- **Text input**: `frontend_begin_text_input(buf, cap)` sets
  `s_text_input_state = 1`; the screen handler must call
  `frontend_handle_text_input_key()` (drains the WM_CHAR queue; Enter →
  state 2 = confirmed, tested via `frontend_text_input_confirmed()`). The
  widget renders from the render path (`frontend_render_text_input`), not the
  handler. **Gotcha**: while `s_text_input_state != 0` the nav FIFO is
  *flushed*, not drained — queued Enter/arrows are dead. State 2 persists until
  the next `set_screen` clears it, so a screen that continues into button
  interaction after a confirmed field must zero `s_text_input_state` itself
  (precedent: `Screen_PostRaceNameEntry` case 4).
- Mouse: first click selects, second confirms; clicks within 20 px of a
  selected button's edges synthesize LEFT/RIGHT arrow input.

## 4. Drawing

Design space is a 640×480 canvas (`FE_CANVAS_W/H`); helpers take
`sx = screen_w/640`, `sy = screen_h/480` and draw in window pixels.

- **`fe_draw_quad(x,y,w,h,color,tex_page,u0,v0,u1,v1)`** — immediate textured
  quad (file-static in `td5_frontend.c`; tex_page −1 = white page). There is
  also a small *deferred* queue (`s_draw_queue`, `frontend_fill_rect`/
  `frontend_blit`) drained by `td5_frontend_flush_sprite_blits()`, which applies
  the canvas scale **at flush time** — never pass pre-scaled coords into it.
- **Text**: `fe_draw_text` / `fe_draw_text_centered` (uppercased by default).
  Path priority: native TTF (`td5_font_ready()`) → MSDF atlas (when
  `[Frontend] VectorUI=1`, page `SHARED_PAGE_FONT_MSDF`) → BodyText.tga bitmap
  atlas. `fe_draw_small_text` is the only second font (lowercase/descenders).
  Never bake per-screen text art; screen headers use
  `frontend_draw_screen_title` with names from
  `frontend_get_title_text_for_screen`.
- **Buttons**: pool `FE_Button s_buttons[FE_MAX_BUTTONS=16]`;
  `frontend_create_button(label, x, y, w, h)` allocates the first free slot.
  Sentinels: `w == 200` is treated as "unset" (becomes 224), `h == 32` likewise;
  negative `x` triggers center-relative auto-layout. Buttons are drawn centrally
  by `td5_frontend_render_ui_rects` (VectorUI roundrect or ButtonBits 9-slice);
  fields: `disabled`, `hidden`, `is_selector` (selectors don't auto-draw
  captions), `highlight_ramp` (0–6 fade, updated in the poll).
- **Surfaces**: `FE_Surface s_surfaces[FE_MAX_SURFACES=31]` on texture pages
  `FE_SURFACE_PAGE_BASE`(900)+slot. `frontend_load_tga(_ck)` resolves PNG from
  `re/assets` first (ZIP+TGA fallback), de-dups by (name, archive) via
  `frontend_find_surface_by_source`, and LRU-evicts background-like surfaces
  (`load_seq`) when full. Shared pages (`SHARED_PAGE_*`, 888–899 + MSDF/SDF
  pages 970–983: fonts, cursor, ButtonBits, bg gallery) survive `set_screen`;
  per-screen slots are swept.

## 5. How to add a screen

1. **Enum**: add `TD5_SCREEN_FOO = 34` to `TD5_ScreenIndex` in `td5_types.h`,
   bump `TD5_SCREEN_COUNT`.
2. **Handler**: `void Screen_Foo(void)` in the matching cluster TU
   (menu/race/net), following the state-machine convention in §2. Declare it in
   `td5_frontend_internal.h` (keep declarations above the
   `@GENERATED-SYMBOLS@` marker at the end of the file).
3. **Table**: append the pointer to `s_screen_table` in `td5_frontend.c`.
4. **Back-out**: add a case to `frontend_get_parent_screen` (in
   `td5_frontend.c`) and call `frontend_init_return_screen(TD5_SCREEN_FOO)` in
   state 0 — this is what makes ESC/B work.
5. **Settle**: set `s_anim_complete = 1` when the intro finishes; the global
   ESC handler and the default slide-in chime are gated on it. Transition SFX
   are otherwise inherited free from the `set_screen` choke point.
6. **Shared state**: any cross-TU statics get an `extern` in
   `td5_frontend_internal.h` and a single definition in one TU. Frontend-private
   things stay file-static.
7. Optional: title text in `frontend_get_title_text_for_screen`; per-screen
   render overlays go in the `switch (s_current_screen)` of
   `td5_frontend_render_ui_rects`.
8. **Test**: `td5re.exe --StartScreen=N` boots straight to the screen;
   `log/frontend.log` records `Screen transition: A -> B` and button presses
   (flushes only on clean window close).

## 6. Race launch

Screens never start a race directly. On OK they call
`frontend_init_race_schedule()` (`td5_frontend.c`): sets
`g_td5.race_requested = 1`, resolves `g_td5.car_index`/`track_index` from
`s_selected_*` (or attract state), computes human count (net lockstep > MP
flow > 1), AI opponent count, binds the active menu controller to player 0,
re-commits Traffic/Police toggles for game-type 0, and builds the 6-slot racer
schedule. Split-screen layout goes through
`frontend_commit_pane_layout(eff_humans, spectate)` — the single place that
clamps spectator panes and resolves `mp_resolve_layout` into
`g_td5.split_grid_cols/rows` + `split_screen_mode` (shared with the AutoRace
SpectateScreens override so the paths can't drift). The display loop then
returns 1 and `td5_game` leaves MENU state.
