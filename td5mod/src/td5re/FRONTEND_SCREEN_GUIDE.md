# TD5RE Frontend Screen Creation Routine

**Follow this for EVERY new frontend screen.** It exists so new screens match the
main-menu / standard-screen look automatically (titles, buttons, background,
input, sounds). Established 2026-06-22; the MP game-mode screens
(`Screen_MpModeVote`, `Screen_MpModeConfig`, `Screen_CupWinners` in
`td5_fe_race.c`) are the reference implementation.

## The hard rules (non-negotiable)

1. **Title MUST be at the top**, aligned and coloured exactly like the main
   menu / standard screens:
   - Position: `FE_TITLE_LEFT_X (126) * sx`, `17.0f * sy` (top-left).
   - Colour: **`#E3D708` = `0xFFE3D708`** (the canonical frontend gold). Never
     use any other gold (e.g. `0xFFFFE060` is wrong).
   - Draw via `frontend_draw_screen_title(text, FE_TITLE_LEFT_X*sx, 17*sy,
     0xFFE3D708u, sx, sy)` (static in `td5_frontend.c`). For screens in the MP /
     `td5_fe_race.c` flow — where the global title path is suppressed because
     `s_mp_simul` is set — use the file-local `fe_race_draw_screen_title(...)`
     with the **same** args (it mirrors the standard title; the MP screens use
     the `MP_TITLE_GOLD` / `MP_TITLE_LEFT_X` / `MP_TITLE_TOP_Y` constants).

2. **Buttons MUST use the main-menu style.** Create them with
   `frontend_create_button(label, x, y, w, h)` and let the shared render
   (`td5_frontend_render_ui_rects` button loop) draw the standard 9-slice
   frame + gold highlight. Do NOT hand-roll buttons with `td5_vui_roundrect`.
   You get keyboard nav, **mouse** hover/click, the highlight ramp, and the
   confirm/"locked" sound for free.
   - Selector (slider) rows: after creating, set `s_buttons[idx].is_selector =
     1`; read `frontend_option_delta()` for LEFT/RIGHT value changes.

### Button column alignment — THE routine (don't re-derive X every time)

This is the recurring miss: a new screen invents its own button X and the column
ends up not lining up under the title. The values are **fixed**, so just use them:

| Element | Design X | Constant |
|---|---|---|
| Screen **title** text (first letter) | `126` | `FE_TITLE_LEFT_X` (`td5_frontend.c`) |
| Left-column menu **button frame** left edge | `120` | `FE_MENU_BTN_X` (`td5_frontend_internal.h`) |
| Left-column menu **button width** | `0xE0` (224) | `FE_MENU_BTN_W` |
| Left-column menu **button height** | `0x20` (32) | `FE_MENU_BTN_H` |

**How to get it right, mechanically:**
1. Draw the title at `frontend_draw_screen_title(text, FE_TITLE_LEFT_X*sx, 17*sy, 0xFFE3D708u, sx, sy)`.
2. Create every button in the left column with **`x = FE_MENU_BTN_X`, `w = FE_MENU_BTN_W`** (pick your own `y` start + row pitch). RE basis: the original race menu (`@0x004168B0`) rests its buttons at `g_frontendCanvasW/2 − 200 = 120` and the shared title creator (`@0x00412E30`) blits the title at the *same* `120`, so in the original the title and column align exactly. The port draws its title at `126` (a value eyeballed from `MainMenu.png`; a 126 constant scan over the frontend region returned zero hits), so the `~6 px` gap is a port-title artefact. Match `120` (`FE_MENU_BTN_X`) — do **not** try to hit `126`, and do **not** invent another X.
3. **Labels are CENTRED inside the frame**, not left-aligned — the button loop draws `fe_draw_text(bx + (bw - text_w)*0.5f, ...)`. So a label does **not** start at `120`; the *frame* does. If a label is too long for the 224px frame, **shorten the label** (a right-side description panel can carry the full wording) — never widen the column past `~228` (panels start at `x=348`) and never move the column left to fit text.
4. Reference implementation to copy: **`Screen_RaceTypeCategory`** (`td5_fe_menu.c`); `Screen_MpPostRace` (`td5_fe_race.c`) is a second example. Both use `FE_MENU_BTN_X` / `FE_MENU_BTN_W`.
5. Verify with a screenshot: the button column's left edge should sit just under the title's first letter. If it's visibly off (more than a few px), you used the wrong X — go back to `FE_MENU_BTN_X`.

> Historical note: `FE_LOBBY_X` is `116` and the title is `126`; these small
> per-screen eyeballed offsets are why this kept getting flagged. For any **new**
> left-column menu, ignore those and use `FE_MENU_BTN_X` (120).

3. **Two-line buttons:** make the button **taller** (e.g. `h = 54`) and draw
   BOTH lines **block-centred vertically** on the button with a small gap
   between them (reference: name at `y+11`, description at `y+37` on a 54-tall
   button). The label text must be drawn in the **post-button render pass** (see
   §Render order) so it composites on top of the frame.

## Spacing rules (defined 2026-07-03 — demonstrated live on the UI GUIDE screen)

- **Stacked buttons:** vertical gap between consecutive buttons is **at most
  6px** (row pitch = `h + 6`; e.g. 32-tall rows at 97, 135, 173, ...).
- **Two-column items:** horizontal gap between side-by-side items is the same
  **6px** (the 224 column splits as `109 + 6 + 109`).
- **Arrowed (selector) rows:** keep **6px clear to the right** of the button
  before the next element. The row's **value text starts at X=348,
  LEFT-justified** (never centred in a floating value column).
- **BACK button:** placed **after the last box** on the screen with the same
  <=6px gap. It may move down as content grows but **never past Y=460** —
  nothing is drawn below the Y460 content floor.
- **Vertical keyboard nav stops at the nearest row** (frontend_spatial_pick is
  nearest-row-first) — a half-width pair right below a full row is landed on,
  not skipped; the spanned-row rule then picks the pair's leftmost item.
- The dev **UI GUIDE** screen (`--StartScreen=1`, or CHANGELOG → UI GUIDE →
  MP TOOLS for the MP widgets) renders these rules with visible margin lines,
  6px gap markers and per-button WxH labels — screenshot it after frontend
  changes to verify the canon.

## Render order (critical)

The per-frame frontend draw (`td5_frontend.c`) is:
1. background quad (auto, if a full-canvas TGA is loaded)
2. **pre-button** per-screen overlay `switch (s_current_screen)` (~line 9168)
   — draws UNDER the buttons.
3. **button loop** — frames + labels + highlight + mouse hover (~line 9311).
4. **post-button** per-screen overlay `switch` inside `if (s_anim_complete)`
   (~line 9440, "Option arrows drawn AFTER buttons") — draws ON TOP of frames.

→ Anything that sits **on** a button (two-line labels, option values, ◄►
arrows, vote markers) goes in the **post-button** switch. The title (top, no
overlap) can go either place; the reference screens draw everything in the
post-button render fn.

## Background

Call `frontend_load_tga("Front_End/MainMenu.tga", "Front_End/FrontEnd.zip")` in
the screen's init state. The shared render draws it full-canvas automatically —
do **not** draw your own dim/opacity overlay over the whole screen (that was the
old VectorUI look and is wrong).

## Input + sounds

- Standard nav (`frontend_poll_input`) drives `s_selected_button` (UP/DOWN +
  mouse hover) and sets `s_input_ready` / `s_button_index` on confirm — for
  keyboard, mouse, and the primary pad.
- Play the **confirm / "locked" sound** with `frontend_play_sfx(3)` on a
  lock/continue. `frontend_play_sfx(2)` = nav beep; `frontend_play_sfx(5)` =
  back/cancel whoosh; `frontend_play_sfx(10)` = error/disabled.
- A **gamepad host** (or split-screen players) reads its own device with
  `mp_simul_player_nav(player)` (bits: `1`=L `2`=R `4`=U `8`=D `0x10`=A `0x20`=B);
  the host is always player 0 (`s_mp_join_device[0]`). The reference screens use
  the `mp_host_input()` helper to unify keyboard/mouse/gamepad-0.

## Back / leave

Use a **confirm prompt** before leaving a setup screen (don't drop out on a
single press). Reference: `mp_confirm_modal_render()` + an armed flag, drawn in
the post-button pass. In the local-MP flow, "leave" returns to the lobby via
`mp_simul_back_to_lobby(n)` (→ `TD5_SCREEN_MP_LOBBY`), NOT the main menu.

## Per-player / host indicators (MP screens)

Show a clear **host indicator** (the reference uses a P1 colour swatch +
`"P1 = HOST"`). Per-player markers use `k_mp_player_colors[slot]` (mask
`0x00FFFFFF | 0xFF000000`); keep them **square** (equal w/h) and vertically
centred on their row.

## New-screen checklist

- [ ] Title at top, `FE_TITLE_LEFT_X`/`17`, colour `0xFFE3D708`.
- [ ] `frontend_load_tga(MainMenu.tga)` in init; NO full-screen dim overlay.
- [ ] Buttons via `frontend_create_button` (taller if two lines).
- [ ] Left-column buttons at `x = FE_MENU_BTN_X` (120), `w = FE_MENU_BTN_W`
      (0xE0) so they align under the title; shorten labels rather than widen.
- [ ] On-button text/values/arrows drawn in the **post-button** render switch.
- [ ] Two lines block-centred with a gap.
- [ ] `frontend_play_sfx(3)` on select/lock; `(2)` on nav; `(5)` on back.
- [ ] Mouse works (it does automatically with standard buttons).
- [ ] Back = confirm prompt → correct parent screen (lobby for MP setup).
- [ ] Register the render fn in the post-button `switch` in `td5_frontend.c`.
- [ ] Add the screen enum + descriptor row + handler decl:
      - `td5_types.h`: **append** the enum value (never renumber 0..N — those
        numbers are referenced by `StartScreen`, logs, inputscripts, docs), bump
        `TD5_SCREEN_COUNT`.
      - `td5_frontend.c`: add one `s_screens[]` `ScreenDesc` row `{ "NAME",
        handler }` — `name` is the dev screen-ID badge's identity label for this
        screen (retired slots use `{ NULL, NULL }`). This is the single source of
        truth for the badge; it is **separate** from the on-screen header
        (`frontend_get_title_text_for_screen`), which several self-titling screens
        deliberately leave `NULL` — do not conflate them, or you'll double-draw a
        title.
      - `td5_frontend_internal.h`: handler decl if the body lives in a `td5_fe_*.c`.
- [ ] **Screens with internal modes** (one table entry that renders several
      visually distinct layouts off `s_inner_state` / a game-mode / an overlay
      toggle) must add a `case` to `frontend_screen_substate()` in
      `td5_frontend.c`, returning a 0-based sub-index (+ optional variant label),
      so the dev badge reads `N.sub label` instead of one ambiguous number for
      all the variants. If a variant is really its own navigational screen
      (own back-target, reached via `set_screen`), promote it to its own enum
      instead and share the handler body — see `TD5_SCREEN_SELECT_CUP` /
      `TD5_SCREEN_CUP_TRACK_SELECT` for the shared-handler pattern.
- [ ] **[I18N 2026-07-21]** Wrap every player-facing literal in `TR(...)`
      (`td5_i18n.h`) and add its es-AR value to
      `re/assets/frontend/lang/es_AR.txt` (run
      `python re/tools/gen_i18n_catalog.py` to sync missing keys). Static
      `const` tables can't hold `TR()` — translate at the draw site with
      `td5_tr(...)` instead. Screen titles translate automatically via the
      `frontend_get_title_text_for_screen` chokepoint, but the title string
      itself needs a catalog entry. The LANGUAGE screen (46,
      `Screen_LanguageOptions` in `td5_fe_menu.c`) is the selector; labels
      refresh on screen re-entry (they're copied at `frontend_create_button`
      time), and `fe_fit_text_scale` condenses over-wide captions.

- **[RT2 P8] LIGHTING OPTIONS (screen 51, `Screen_LightingOptions` in
  `td5_fe_menu.c`)** — the RT-lighting per-feature sub-screen, reached from the
  **GRAPHICS OPTIONS** (16) row 6 "LIGHTING OPTIONS ->" (which replaced the old
  LIGHTING QUALITY toggle; QUALITY is now row 0 here). 8 ◄► selector rows —
  LIGHTING QUALITY, SHADOW QUALITY, SHADOW DETAIL, REFLECTIONS, REFLECTION
  RANGE, GLOBAL ILLUM., SUN & SKY, LIGHTS — each defaulting to its highest
  tier, persisted to `[Lighting]` INI keys (`ShadowRays`/`ShadowRes`/
  `ReflectionQuality`/`ReflectionRange`/`GIQuality`/`SunProbe`/`LightQuality` +
  the existing `Quality`). Row 0 is live whenever a DXR device exists; rows 1–7
  are HIGH-only and render greyed/inert at LOW. Tiers map onto the `TD5RE_RT_*`
  env knobs via `td5_rt_apply_lighting_options()` (env override still wins). BACK
  returns to GRAPHICS OPTIONS. Adding this screen touched the 12 standard
  new-screen sites (enum + `TD5_SCREEN_COUNT`, `s_screens[]`, title, parent-of,
  is-options, 2 button-anim switches, value-overlay + arrow dispatch,
  prototypes, handler, and the GRAPHICS-OPTIONS nav row).

## [CHAOS CO-OP] CHAOS TEAMS (screen 54) + the six-row MP MODE VOTE column

Added 2026-09-29/30. Mode plan: `docs/plans/CHAOS_COOP_MODE_PLAN.md` §6.
Behaviour spec: `EXPECTED_BEHAVIOR.md` → "CHAOS CO-OP".

### CHAOS TEAMS — screen 54, `Screen_ChaosTeams` in `td5_fe_chaos.c`

The seat/role claim board for `TD5_MP_MODE_CHAOS_COOP`. Reached from **MP MODE
CONFIG (36)** when the locked mode is CHAOS CO-OP (`td5_fe_race.c`), and BACK
returns there (`frontend_get_parent_screen`). On START the board is committed as
a `TD5_ChaosConfig` via `td5_chaos_commit_config()` and the flow continues to the
**MP car grid (49)** with exactly TWO pickers, one per team, each driven by the
device of that team's row-0 seat.

It is **self-titling**: like every screen in the MP setup chain it runs with
`s_mp_simul` set, which suppresses the global title path, so
`frontend_get_title_text_for_screen` deliberately has **no** case for 54 and the
board draws its own header. Adding one would double-draw the title.

Header title goes through `fe_race_draw_screen_title_fit()` (new), not the plain
`fe_race_draw_screen_title()`: the board measures the right-aligned player-count
badge first and hands the title whatever width is left minus
`CT_TITLE_BADGE_GAP` (12 design px), and the helper **condenses horizontally**
(cap height untouched, floor `FE_RACE_TITLE_MIN_HSCALE` = 0.55) to fit. A short
fixed string alone is not enough here: title width scales with `sy` while the
canvas scales with `sx`, so 4:3 is ~2.2x tighter than 16:9, and the es-AR header
is a character longer again.

Geometry (640x480 design px, `CT_*` constants in `td5_fe_chaos.c`). MP bands
apply: left margin 112, right edge 628, nothing below y=460.

| Element | x | y | w | h |
|---|---|---|---|---|
| Team panel A (RED) | 112 | 92 | 248 | 198 |
| Team panel B (BLUE) | 380 | 92 | 248 | 198 |
| Panel header band | +0 | +0 | 248 | 24 |
| Seat row `k` (0-based) | +4 | 120 + 42k | 240 | 38 |
| WAITING strip | 112 | 300 | 516 | 42 |
| ROTATE AT selector (button 0) | 112 | 352 | 516 | 28 |
| Hint line (small, centred) | 370 | 388 | - | - |
| START (button 1) | 322 | 404 | 96 | 32 |

Panels are drawn at a **fixed** height for all team sizes (`CT_MAX_ROWS` 4) so
the layout does not jump with the player count. Rows past `team_size` are greyed
(`CT_EMPTY_GREY` `0xFF6A6A6A`) and inert. Only two real buttons exist
(`CT_BTN_ROTATE` = 0 with `is_selector = 1`, `CT_BTN_START` = 1), so the board is
**not** a left-column menu and `FE_MENU_BTN_X` does not apply to it.

Per-seat colour is `td5_chaos_seat_color(seat)` (`td5_chaos.h`), **not**
`s_mp_player_accent` and **not** `hud_filler_slot_color`. It is the single source
shared with the in-race role strip: 8 distinct hues carrying the literal
`k_mp_player_colors[0..7]` values. `hud_filler_slot_color` is a 6-colour wheel
and at 8 seats gave seats 0 and 6 the same colour, which the alternating
RED/BLUE seating puts on the same team. See `EXPECTED_BEHAVIOR.md`.

Interaction is per-device, like MP MODE VOTE and MP TEAM SELECT: every joined
device drives its own cursor through `mp_simul_player_nav(player)`. Up/Down moves
within a panel, Left/Right crosses to the other team, A takes an empty row /
unlocks your own / **swaps** with another player, B returns to WAITING, and the
host (first joined device) owns START and ESC (with a "BACK TO MODE OPTIONS?"
confirm modal). On entry players are pre-seated alternating RED/BLUE in join
order and all locked, so a group that does not care can press START immediately.

**Verification, and its hard limit.** Control-socket `tap_key` / `inject_key` do
**not** reach frontend menus, so this screen cannot be self-verified by the MCP
driver. Layout: `--StartScreen=54` + `TD5RE_FRAMEDUMP=<path.png>` and read the
PNG. Screen 54 has **no `k_ssw_*` nav route** and jumps direct, on purpose
(`--StartScreenDirect=1` is accepted and equivalent) — the faithful walk needs 4
to 8 real press-to-join device edges in the lobby, which `--StartScreen` cannot
fabricate, and the CHAOS row on MODE VOTE stays greyed below 4 joins. See the
comment beside the `k_ssw_*` tables in `td5_game.c` for why a partial route
would be worse than none. On direct entry `chaos_screen_init` seeds a dev
**FAKE ROSTER of 4** (dev builds only, with a "DEV: FAKE ROSTER" footer) so the
board renders a legal 2x2 layout. Seat claiming and swapping need real pads and
are a manual check (`pending_to_test.csv`).

### MP MODE VOTE (35) — the SIX-row column

`TD5_MP_MODE_COUNT` went 5 → 6 (CHAOS CO-OP), which no longer fit between the
host banner and the Y460 floor. Final geometry, all 640x480 design px
(`MV_*` in `td5_fe_race.c`):

| Element | Value | Note |
|---|---|---|
| `MV_BX` / `MV_BW` | 170 / 300 | unchanged |
| `MV_BH` | **48** (was 50) | two-line button |
| `MV_GAP` | 64 | row pitch, unchanged |
| `MV_Y0` | **88** (was 76, originally 96) | first row top |
| `MV_BANNER_Y` | **48** (was 72, then 56) | host badge + "OTHERS PRESS A" |
| banner box | 48 .. 71 | extends ~23 px below the y passed, not ~13 |
| rows | 88, 152, 216, 280, 344, 408 | `MV_Y0 + 64k`, height 48 |
| bottom edge | 456 | 4 px clear of the Y460 floor |
| banner → first button | 17 px | |
| per-voter ring band | `(64 − 48) / 2` = **8 px** each side | was 7 |

Two things to keep in mind if this column is touched again. **Trim the BUTTON,
not the PITCH**: `MV_GAP − MV_BH` is what the concentric per-voter vote rings
live in, so shrinking `MV_BH` to 48 actually GREW the ring band from 14 to 16 px
while buying the 10 px the sixth row needed. And the **banner is ~23 design px
tall**, not ~13: the first attempt at `MV_BANNER_Y = 56` still overlapped the
first button's top border, which only a 1920x1080 framedump showed.

Label lines are unchanged at `+5` (mode name, `td5_vui_text_centered`) and `+29`
(description, `mp_pos_small_centered`). A **greyed** row (CHAOS CO-OP when
`frontend_chaos_mode_selectable()` says no) dims the name and **replaces** the
description with the amber reason `0xFFFFC060` on that same `+29` line, drawn
through `mp_pos_small_centered_fit()` so a long translation cannot overrun the
300 px frame. Stacking the reason under the description at `+39` does **not**
work: the small-text box is ~13 design px tall, so `+29` already runs to `+42`
and the two collide and spill past the button. One line per row is all a 48-tall
frame holds. Reason strings are kept short in English **and** es-AR for the same
reason. A confirm attempt on the greyed row plays `frontend_play_sfx(10)`.
