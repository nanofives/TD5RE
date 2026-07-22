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
- [ ] Add the screen enum + table entry + decl (`td5_types.h`,
      `td5_frontend.c` table, `td5_frontend_internal.h`).
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
