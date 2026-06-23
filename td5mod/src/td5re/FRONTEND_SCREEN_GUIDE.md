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

3. **Two-line buttons:** make the button **taller** (e.g. `h = 54`) and draw
   BOTH lines **block-centred vertically** on the button with a small gap
   between them (reference: name at `y+11`, description at `y+37` on a 54-tall
   button). The label text must be drawn in the **post-button render pass** (see
   §Render order) so it composites on top of the frame.

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
- [ ] On-button text/values/arrows drawn in the **post-button** render switch.
- [ ] Two lines block-centred with a gap.
- [ ] `frontend_play_sfx(3)` on select/lock; `(2)` on nav; `(5)` on back.
- [ ] Mouse works (it does automatically with standard buttons).
- [ ] Back = confirm prompt → correct parent screen (lobby for MP setup).
- [ ] Register the render fn in the post-button `switch` in `td5_frontend.c`.
- [ ] Add the screen enum + table entry + decl (`td5_types.h`,
      `td5_frontend.c` table, `td5_frontend_internal.h`).
