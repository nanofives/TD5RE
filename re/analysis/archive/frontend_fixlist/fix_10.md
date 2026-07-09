# Frontend 3-layer faithfulness fix list — screens 10–14

RESEARCH ONLY. Implementation-ready. Verifies all 3 layers against PORT code, not just CREATION
(per `frontend_diff_blindspot_postmortem.md`: CREATION ≠ RENDERING ≠ BEHAVIOR).

- **L1 CREATION** — correct label (real SNK_ string, `frontend_snk_strings.md`), position, size, gating.
- **L2 RENDERING** — element actually DRAWN? overlay dispatch `switch(s_current_screen)` @5588 →
  `frontend_render_<screen>_overlay`; arrow dispatch `switch(s_current_screen)` @5784 →
  `fe_draw_option_arrows(idx)`; header via `frontend_ensure_title_texture` @5858/720-778;
  value-display overlays; decoupled/flush.
- **L3 BEHAVIOR/INPUT** — interactive `case 6` reacts? selector input must use
  `active_button = (s_button_index>=0)?s_button_index:s_selected_button;` not raw `s_button_index==N`.

PORT = `td5mod/src/td5re/td5_frontend.c` (worktree copy). Screen enum @ `td5_types.h:313-343`.
Netplay 10/11 lobby-PANEL rendering = ARCH-DIV (DXPTYPE) — noted, NOT built; input/button-set/header
bugs ARE in scope. ARCH-DIV items excluded from fix counts.

KEY CROSS-SCREEN CORRECTION to `diff_10.md`: it claimed the header label is **MISSING** on screens 10
and 11. **FALSE.** `frontend_get_title_tga_for_screen` @737-739 maps both CREATE_SESSION(10) and
NETWORK_LOBBY(11) → `"NetPlayText.TGA"` (page BASE+8), and `frontend_render_ui_rects` @5858 draws it.
The header IS rendered (a baked title TGA, not the per-char `CreateMenuStringLabelSurface(5)`="NET PLAY"
path, but visually faithful). Downgrade those rows from MISSING to MATCH(approx).

---

### Screen 10 — RunFrontendCreateSessionFlow  [interactive Y]
Flow: `Screen_CreateSession` @7109. state0 init (load MainMenu.tga, create "Enter Name"+"Back" buttons,
begin text input, preseed "New Session") → 1 slide-in → 2 name input (`frontend_render_text_input`
@7132; Back idx1 → SESSION_PICKER; confirm → NETWORK_LOBBY) → 3 → 4-15 collapse to
`set_screen(return)` (DXPTYPE ARCH-DIV) → default → NETWORK_LOBBY. Two parallel host/join sub-flows
(orig states 0-8 vs 0x10-0x13) collapsed to one linear flow.

| element/behavior | L1 creation | L2 render | L3 behavior | VERDICT | FIX (file:line + change) |
|---|---|---|---|---|---|
| Header "NET PLAY" | maps to NetPlayText.TGA @737 (BASE+8) | DRAWN via title path @5858 | n/a | MATCH(approx) | none — diff's MISSING is wrong |
| Two host/join sub-flows + DXPlay handshake | orig states 0-8 / 0x10-0x15 | collapsed @7156-7160 | unreachable vs orig peers | ARCH-DIV | document only; keep collapse |
| "Enter New Session Name" button | label `"Enter Name"`, x=-300 w=300 h=0x20 @7115 | button loop @5661 | idx? (note below) | DIVERGENCE (L1) | 7115: label→`"ENTER NEW SESSION NAME"` (SNK_EnterNewSessionNameButTxt); orig w=0x1c0(448) h=0x40 — but port reuses this slot as a plain button while the text panel is a separate live overlay, so width is cosmetic |
| "Back" button | `"Back"` x=-100 w=100 h=0x20 @7116 | button loop | Back idx1 @7133 → SESSION_PICKER | MATCH(approx) | label→`"BACK"` (SNK_BackButTxt) for case-consistency [LABEL] |
| Text-input field + typed string | live panel 448×80 @(96,280) `frontend_render_text_input` @2492; called @7132 | DRAWN (live, not overlay-switch) | typed via `frontend_handle_text_input_key` @2494 | DIVERGENCE | functional; geometry/colors differ from orig baked 0x1c0×0x40 — accept |
| **Text-input title text** | hardcoded `"ENTER PLAYER NAME"` @2527 | DRAWN | — | **BUG (L1/L2)** | 2527: on the SESSION-NAME path this must read `"ENTER NEW SESSION NAME"`. `frontend_render_text_input` is shared (also lobby/player-name); parameterize the title (add a field to `s_text_input_ctx` or pass screen) so screen 10 shows the session prompt, not the player prompt. [LABEL] |
| Blinking caret | white, wall-clock 350ms blink, gated state==1 @2548 | DRAWN | — | DIVERGENCE | orig green `0xff00`, frame-count `anim&0x20`; cosmetic [ANIM] — optional |
| Empty-name → computer-name fallback | preseed "New Session" @7118 | — | — | DIVERGENCE | acceptable stand-in |
| Mouse cursor + hover highlight | green border @5768 | DRAWN | hover idx | MATCH(approx) | — |

PORT-WIRING CHECK: render-overlay dispatch? N (no CREATE_SESSION case @5588 — text input drawn directly
in handler @7132, header via title path); arrow dispatch? N (no selector rows — correct, orig has none);
input uses active_button fallback? N — but state2 only tests `s_button_index==1` for Back; no ◄►
selector exists so the fallback is **not required** here.

SCREEN VERDICT: Structurally faithful given DXPTYPE collapse + correct header. One real BUG (text-input
title hardcoded to player-name prompt). Ordered fixes: (1) 2527 parameterize text-input title →
"ENTER NEW SESSION NAME" on the session-name path; (2) 7115 button label → SNK_EnterNewSessionNameButTxt;
(3) 7116 "Back"→"BACK" [cosmetic]. ARCH-DIV: host/join split + DirectPlay handshake (excluded).

---

### Screen 11 — RunFrontendNetworkLobby  [interactive Y]
Flow: `Screen_NetworkLobby` @7180. 18 states (0..0x11). state0 init (load MainMenu.tga; 6 buttons: chat
strip, "Messages", "Status", "Change Car", "Start", "Exit") → 1 slide-in → 2 enable text input → 3 main
interactive (button dispatch + chat confirm) → 4 chat send → 5 host ready-check → 6-11 error/confirm
dialog → 0x0C-0x11 seal/config-poll/settings/DXPSTART host+client handshake. FSM ported in full
structure; DXPTYPE wire fmt incompatible → unreachable vs orig peers.

| element/behavior | L1 creation | L2 render | L3 behavior | VERDICT | FIX (file:line + change) |
|---|---|---|---|---|---|
| Header "NET PLAY" | NetPlayText.TGA @738 | DRAWN @5858 | n/a | MATCH(approx) | none — diff's MISSING is wrong |
| Net handshake (seal/kick/config/settings/DXPSTART) | states 0x0C-0x11 @7409-7521 | n/a | structurally faithful, DXPTYPE incompatible | ARCH-DIV | document only |
| Chat strip / "Messages" / "Status" frames | labels `""`/`"Messages"`/`"Status"`, widths 0x1d0/0x200/0xe0, h 0x18/0x80/0x86 @7205-07 | button loop draws frames only | — | DIVERGENCE (L1) | labels: "Messages"→`"MESSAGE WINDOW"` (SNK_MessageWindowButTxt), "Status"→`"STATUS"` (SNK_StatusButTxt) @7206-07 [LABEL] |
| **Chat history lines** (≤6 rows) | — | NOT drawn — no NETWORK_LOBBY case @5588; `s_chat_*` ring filled but unrendered | — | MISSING (panel content) | ARCH-DIV-adjacent: render is part of the lobby-panel system that has no peers to populate it. Document; LOW priority (no live data without DXPTYPE) |
| **Player roster rows + status** | — | NOT drawn | — | MISSING (panel content) | same — orig text `SNK_NetPlayStatMsg`=["Chat","Busy","Wait","Play"]; document |
| Session header bar (Session:/Player:/(HOST)) | — | NOT drawn | — | MISSING (panel content) | orig `SNK_TxtSession`="Session:", `SNK_TxtPlayer`="Player:", `SNK_TxtBhostB`="(HOST)"; document |
| **Chat input field + caret** | `s_chat_input_buffer` filled @7213; `frontend_render_text_input` NOT called for lobby | NOT drawn | typed input has no visible field | MISSING | If lobby is to be visually usable: call a text-input render into the chat strip (slot0) in state 3. Reuses screen-10 path. In-scope (input affordance), but moot without peers — MEDIUM |
| "Change Car"/"Start"/"Exit" | widths 200/0x78/0x78 @7208-10 | button loop | idx3/4/5 @7251/7256/7265 | MATCH | labels: "Change Car"→`"CHANGE CAR"`, "Start"→`"START"`, "Exit"→`"EXIT"` (SNK_*) [LABEL] |
| Error/confirm dialog box + Yes/No/OK | buttons created @7348-51, labels "Yes"/"No"/"Ok" | NO dialog box/text drawn (no overlay case) | dialog_mode gating @7347 | MISSING + DIVERGENCE | labels: "Yes"→`"YES"`,"No"→`"NO!"` (SNK_NoxButTxt),"Ok"→`"OK"`; orig dialog text `SNK_NetErrString1-4`. Render dialog panel+text (live, like session_locked overlay @5509). MEDIUM [LABEL] |
| Emote tokens + *team/*all/*kick/*ban | comment only @7296 | — | not implemented | MISSING | implement `NormalizeFrontendChatTokens` before send @7298; LOW (moot without peers) |
| **Lobby button input (idx3/4/5)** | — | — | `if (s_input_ready && s_button_index >= 0)` @7249 — raw index, no active_button | **DIVERGENCE (L3)** | @7249: change to `int active_button=(s_button_index>=0)?s_button_index:s_selected_button;` and switch on it, so keyboard/pad selection (not just mouse) can press Change Car/Start/Exit. Same pattern as GameOptions @7662 |

PORT-WIRING CHECK: render-overlay dispatch? N (no NETWORK_LOBBY case @5588 — all panel content unbacked);
arrow dispatch? N (no selector rows — correct); input uses active_button fallback? **N (@7249 uses raw
s_button_index — L3 bug)**.

SCREEN VERDICT: Heaviest content gap, but the bulk (chat/roster/session/dialog panels) is ARCH-DIV-adjacent
(DXPTYPE — no peers to populate, cannot be field-validated). Two genuinely in-scope, non-ARCH fixes:
(1) **@7249 active_button fallback** for the 3 lobby buttons (keyboard nav currently dead); (2) button
labels → SNK_ strings @7206-10/7348-51. Panel-content rendering (chat/roster/header/dialog text +
chat-input field) documented MISSING but LOW/MEDIUM since unreachable end-to-end. ARCH-DIV: net handshake
+ all DXPTYPE-fed panel data (excluded from counts).

---

### Screen 12 — ScreenOptionsHub  [interactive Y]
Flow: `Screen_OptionsHub` @7531. state0 init (MainMenu.tga; 6 buttons Game/Control/Sound/Graphics/Two
Player/OK) → 1/2 present → 3 slide-in(0x27) → 4/5 settle → 6 interactive (idx0-5 → sub-screens / MAIN_MENU)
→ 7 prep → 8 slide-out(16) → 9 exit. Plain nav menu, no value rows, no arrows.

| element/behavior | L1 creation | L2 render | L3 behavior | VERDICT | FIX (file:line + change) |
|---|---|---|---|---|---|
| Header "OPTIONS" | OptionsText.tga @730 (BASE+3) | DRAWN @5858 | n/a | MATCH(approx) | — |
| 6 buttons Game/Control/Sound/Graphics/Two Player/OK | labels @7539-44, all `-0x130,0,0x130,0x20` | button loop | — | DIVERGENCE (L1) | (a) OK width: orig `SNK_OkButTxt,-0x130,0,0x60,0x20` — port has w=0x130; change @7544 to w=0x60. (b) labels: SNK_GameOptionsButTxt="GAME OPTIONS", SNK_ControlOptionsButTxt="CONTROL OPTIONS", SNK_SoundOptionsButTxt="SOUND OPTIONS", SNK_GraphicsOptionsButTxt="GRAPHICS OPTIONS", SNK_TwoPlayerOptionsButTxt="TWO PLAYER OPTIONS", SNK_OkButTxt="OK" — port labels close (need uppercase for parity) [LABEL] |
| Button dispatch idx0-5 → GAME/CONTROL/SOUND/DISPLAY/TWO_PLAYER/MAIN_MENU | — | — | switch @7570-90 | MATCH | routing identical |
| OK commits in-race shadows | orig @0x41dc8e commits camera/dynamics/traffic/cops/tier | — | port DEFERS to `ConfigureGameTypeFlags` @2854-56 at race launch | DIVERGENCE (faithful-equivalent) | LEAVE AS-IS. Documented @7576-87; single consumer (race launch), adding a commit risks double-apply. CONFIRMED design choice |
| Slide-in 0x27 / slide-out 16 | @7557 / @7603 | — | — | MATCH | — |
| **L3 selector fallback** | — | — | `if (s_input_ready && s_button_index >= 0)` @7569 — raw index | DIVERGENCE (L3, minor) | @7569: these are press-only buttons (no ◄►), so raw `s_button_index>=0` works for mouse; for keyboard-nav parity with other screens, optionally switch on `active_button`. LOW — Enter on a kbd-selected row currently relies on `s_button_index` being set from `s_selected_button` upstream @2262 (it is), so this is OK in practice |
| Cheat-code FSM gating | — | — | `frontend_update_cheat_codes()` unconditional @~3275 vs orig gated to OptionsHub | DIVERGENCE | port runs cheats on every screen; functionally codes still work, orig screen-restriction lost. LOW |

PORT-WIRING CHECK: render-overlay dispatch? N (no OPTIONS_HUB case @5588 — correct, plain nav menu has no
overlay content); arrow dispatch? N (correct — no selector rows); input uses active_button fallback? N
(@7569 raw index — acceptable since press-only + `s_button_index` is seeded from `s_selected_button` @2262).

SCREEN VERDICT: Faithful navigation + correct deferred-commit. Ordered fixes: (1) @7544 OK button width
0x130→0x60; (2) @7539-44 labels → uppercase SNK_ strings [cosmetic]; (3) optional: gate cheat FSM to this
screen. No functional/L2/L3 blockers. DIVERGENCE: deferred OK-commit (documented faithful-equivalent,
keep). ARCH-DIV: none.

---

### Screen 13 — ScreenGameOptions  [interactive Y]
Flow: `Screen_GameOptions` @7620. state0 init (8 buttons: 7 value rows + OK) → 1/2 present → 3
slide-in(0x27) → 4/5 value redraw → 6 interactive (per-row ◄► cycle + OK idx7) → 7 prep → 8
slide-out(16) → 9 exit. Values rendered live in `frontend_render_game_options_overlay` @3999, wired @5596.
Arrows wired @5789-90 (rows 0-6).

| element/behavior | L1 creation | L2 render | L3 behavior | VERDICT | FIX (file:line + change) |
|---|---|---|---|---|---|
| Header "OPTIONS" | maps OptionsText.tga (BASE+3 — note: screen 13 falls to `default` @741/759 → no title!) | see FIX | n/a | **DIVERGENCE (L2)** | `frontend_get_title_tga_for_screen` @725 has NO case for GAME_OPTIONS → returns NULL → no header drawn. Orig uses `CreateMenuStringLabelSurface(6)`="OPTIONS". Add `case TD5_SCREEN_GAME_OPTIONS: return "OptionsText.tga";` @730 + page entry @749. [verify against running screen — UNCERTAIN whether another path supplies it] |
| Row0 Circuit Laps: display `laps+1` "%d", clamp[0,3] | "Circuit Laps" w=0x128 @7629 | `snprintf("%d",laps+1)` @4010 DRAWN | `+=delta` clamp[0,3] no wrap @7667-72 | MATCH (laps fix CONFIRMED, BOTH layers) | label→`"CIRCUIT LAPS"` (SNK_CircuitLapsTxt) [LABEL] |
| Row1 Checkpoint Timers ON/OFF | @7630 | on_off[&1] @4012 | `^=1` @7677 | MATCH | label→`"CHECKPOINT TIMERS"` |
| Row2 Traffic ON/OFF | @7631 | @4013 | `^=1` @7680 | MATCH | label→`"TRAFFIC"` |
| Row3 Cops/Police ON/OFF | label `"Police"` @7632 | @4014 | `^=1` @7683 | MATCH | **SNK_CopsButTxt = "POLICE"** (confirmed) — port "Police" is CORRECT content, just case; →`"POLICE"` [LABEL]. diff's "verify" resolved: orig label IS POLICE |
| Row4 Difficulty EASY/NORMAL/HARD wrap 0↔2 | @7633 | difficulty[%3] @4015 | `+=delta` wrap @7686-88 | MATCH | labels confirmed SNK_DifficultyTxt; button label→`"DIFFICULTY"` |
| Row5 Dynamics | @7634 | `dynamics[]={"SIMULATION","ARCADE"}` idx `s_game_option_dynamics&1` @4002/4016 | `^=1` @7691 | **BUG (L2)** | `s_game_option_dynamics` 0=arcade,1=sim (`s_dynamics_mode` td5_physics.c:164 + `td5_physics_set_dynamics` @10457). SNK_DynamicsTxt[0]="ARCADE",[1]="SIMULATION". Port array is INVERTED → shows wrong label. @4002: change to `{"ARCADE","SIMULATION"}`. Button label→`"DYNAMICS"` |
| Row6 3D Collisions ON/OFF | @7635 | @4017 | `^=1` @7694 | MATCH | label→`"3D COLLISIONS"` (SNK_3dCollisionsButTxt) |
| OK button idx7 w=0x60 → OPTIONS_HUB | @7636 | button loop | `s_button_index==7` @7698 | MATCH | label→`"OK"` |
| Value panel render | — | per-row live @4011-17 (overlay wired @5596) | re-bake state4 on change @7675 etc. | ARCH-DIV (live vs single baked surface) | accepted live-render; document |
| ◄► arrows rows 0-6 | `frontend_create_button` (selector) | `for i 0..6 fe_draw_option_arrows` @5790 DRAWN | per-row cycle | MATCH (BOTH layers wired) | — |

PORT-WIRING CHECK: render-overlay dispatch? **Y @5596** (`frontend_render_game_options_overlay`); arrow
dispatch? **Y @5789-90** (rows 0-6); input uses active_button fallback? **Y @7662**
(`active_button=(s_button_index>=0)?s_button_index:s_selected_button`) — CORRECT.

SCREEN VERDICT: Best-wired of the 5 (overlay+arrows+active_button all present; laps clamp+display
CONFIRMED on both layers). Two real fixes: (1) **@4002 Dynamics array inverted** → `{"ARCADE","SIMULATION"}`
(currently displays the wrong word for each state); (2) **@725/730 missing GAME_OPTIONS title entry** →
no "OPTIONS" header (verify on running screen — [UNCERTAIN]). Then (3) uppercase SNK_ labels @7629-36.
ARCH-DIV: live value-panel (excluded).

---

### Screen 14 — ScreenControlOptions  [interactive Y]
Flow: `Screen_ControlOptions` @7727. state0 init (load Controllers.TGA; 5 buttons: Player1[preview],
Configure, Player2[preview], Configure, OK) → 1/2 present → 3 slide-in(0x27) → 4/5 settle → 6 interactive
→ 7 prep → 8 slide-out(16) → 9 exit. Device ICONS drawn in `frontend_render_control_options_overlay`
@4557 (wired @5617). Arrows wired @5815-16 (rows 0,2).

| element/behavior | L1 creation | L2 render | L3 behavior | VERDICT | FIX (file:line + change) |
|---|---|---|---|---|---|
| Header "OPTIONS" | falls to `default` @741 → no title (no CONTROL_OPTIONS case) | NOT drawn | n/a | DIVERGENCE (L2) | add `case TD5_SCREEN_CONTROL_OPTIONS: return "OptionsText.tga";` @730 + page @749 (same gap as S13). [UNCERTAIN — verify on running screen] |
| **Device-source ◄► cycling** (idx0→P1 source, idx2→P2 source; +dir &7, skip empty desc, skip ==other) | rows are PREVIEW buttons @7735/7737 | arrows DRAWN @5815-16 | **case6 @7760-75 handles ONLY idx1/3 (Configure) + idx4 (OK). NO `frontend_option_delta()`, NO idx0/idx2 cycle** | **MISSING (P0, L3)** | @7760 add: `int delta=frontend_option_delta(); int active_button=(s_button_index>=0)?s_button_index:s_selected_button; if(delta){ if(active_button==0){ cycle P1 } else if(active_button==2){ cycle P2 } s_inner_state=4; }`. Cycle = advance source via `td5_input_set_input_source(p,newsrc)` (@td5_input.c:266) with &7 wrap over `td5_input_enumerate_devices()` (:1845), skipping empty + the other player's source. NEEDS a getter: add `td5_input_get_input_source(int p)` (td5_input.c — `s_input_source[]` :141 currently has setter @266 only). Also arrows currently show but do nothing |
| **Device-NAME value panel** (P1+P2 text) | — | **NOT drawn — overlay @4557 draws ICONS only, no device-name text** | — | **MISSING (P0, L2)** | in `frontend_render_control_options_overlay` @4557, after icons, draw P1 name at ~(394,97-ish header) and P2 at (394,217) via `td5_input_get_device_name(td5_input_get_input_source(p))` (:1855) or SNK_Ctrl_Modes=["KEYBOARD","JOYSTICK","JOYPAD","WHEEL","<NONE>"] indexed by class. orig panel at (cx+0x4a,cy-0x8f)/(..-0x17). [LABEL][FONT] |
| Device ICON P1/P2 | Controllers.TGA → `s_control_options_surface` @7733 | DRAWN @4579-82, row=`td5_input_get_device_type` (0/1/2) | — | DIVERGENCE | (a) NO hide-when-none: orig hides icon when desc==0xffffffff (class 4); add gate. (b) port class map 0/1/2 (kbd/joypad/joystick) vs orig 5-class (kbd/js/wheel/wheel+pedals/none) — SNK_Ctrl_Modes has 5 entries; [ASSET]. (c) P2 y=217 vs spec cy-0x17≈223 (minor) |
| Row0 "Player 1" + ◄►; Row2 "Player 2" + ◄► | `frontend_create_preview_button` @7735/7737 | arrows @5815-16 | cycle targets (see P0 above) | DIVERGENCE | orig builds NORMAL label buttons (the cycle targets); port uses PREVIEW (half-bright) → rows look inert. Change @7735/7737 to `frontend_create_button` so they read as live selectors. labels→`"PLAYER 1"`/`"PLAYER 2"` (SNK_Player1/2ButTxt) [LABEL] |
| Row1/Row3 "Configure" → CONTROLLER_BINDING (slot 0/1) | @7736/7738 | button loop | idx1/3 set `s_ctrl_player`=0/1 @7765-69 | MATCH | label→`"CONFIGURE"` (SNK_ConfigureButTxt) [LABEL] |
| OK idx4 w=0x60 → OPTIONS_HUB | @7739 | button loop | idx4 @7771-73 | MATCH | label→`"OK"` |
| In-race entry: extra line SNK_CtrlOptions_Ex + OK resume route | — | — | not handled, always OPTIONS_HUB @7772 | MISSING | orig: if returnIdx==~0 (in-race pause) add `SNK_CtrlOptions_Ex`="CLICK TO GO BACK TO MAIN MENU" and OK routes back to in-race options. LOW (port pause-options path differs) [LABEL] |
| Controllers.tga load path | `"Controllers.TGA","Front End/frontend.zip"` @7733 (note: MainMenu uses `"Front_End/FrontEnd.zip"` @7732 — different archive string) | — | — | MATCH(approx) | verify both archive name forms resolve [ASSET] |
| **L3 selector fallback** | — | — | case6 @7761 uses raw `s_button_index` for idx1/3/4 | DIVERGENCE (L3) | fold into the P0 fix: use `active_button` for ALL of idx0/1/2/3/4 so keyboard nav drives Configure/OK and cycling |

PORT-WIRING CHECK: render-overlay dispatch? **Y @5617** (`frontend_render_control_options_overlay`) — but
it renders ICONS only, **NOT device-name text**; arrow dispatch? **Y @5815-16** (rows 0,2) — but arrows
are inert (no L3 handler); input uses active_button fallback? **N (@7761 raw s_button_index, AND no
idx0/2 cycle at all — P0)**.

SCREEN VERDICT: The two P0 gaps are real and confirmed in code. Ordered fix list:
1. **(P0, L3) @7760-75 device-source ◄► cycling** — add `frontend_option_delta()` + `active_button`
   handling for idx0/idx2; advance `s_input_source[player]` (&7 wrap, skip empty + duplicate) →
   `td5_input_set_input_source`; needs new getter `td5_input_get_input_source(int p)`; set state=4 to
   re-bake. Without it the device selectors are completely inert despite arrows showing.
2. **(P0, L2) @4557 device-NAME panel** — render P1/P2 device-name text via `td5_input_get_device_name`
   (or SNK_Ctrl_Modes by class) in the overlay; add class-4 "none" icon-hide.
3. @7735/7737 PREVIEW→normal label buttons (cycle affordance); labels → SNK_ strings @7735-39.
4. @730/749 add CONTROL_OPTIONS title entry [verify].
ARCH-DIV: none (device pipeline plumbing exists in td5_input.c).

---

## Roll-up (ARCH-DIV excluded)

| screen | interactive | L1 fixes | L2 fixes | L3 fixes | top fix |
|---|---|---|---|---|---|
| 10 CreateSession | Y | 2 (labels) | 1 (text-input title hardcoded) | 0 | text-input title → "ENTER NEW SESSION NAME" (@2527) |
| 11 NetworkLobby | Y | ~5 (labels) | 4 panels MISSING (ARCH-adjacent/LOW) + dialog text | 1 (active_button @7249) | @7249 active_button fallback for lobby buttons |
| 12 OptionsHub | Y | 1 (OK width 0x130→0x60) + labels | 0 | 0 (acceptable) | @7544 OK button width 0x60 |
| 13 GameOptions | Y | labels | 2 (Dynamics array inverted @4002; missing title) | 0 | @4002 Dynamics `{"ARCADE","SIMULATION"}` (currently shows wrong word) |
| 14 ControlOptions | Y | labels + preview→normal | 2 (device-name panel P0; missing title) | 1 P0 (device cycle @7760) | @7760 device-source ◄► cycling (P0) |

ARCH-DIV (excluded): S10 host/join split + DirectPlay handshake; S11 net handshake + all DXPTYPE-fed lobby
panel DATA (chat/roster/session/dialog text — render is moot without peers); S13 live value-panel model.

GLOBAL CORRECTION to diff_10.md: headers on S10/S11 are NOT missing (NetPlayText.TGA wired @737-758,
drawn @5858); but S13/S14 headers ARE missing (no GAME_OPTIONS/CONTROL_OPTIONS case in
`frontend_get_title_tga_for_screen` @725-741) — the inverse of what the diff implied.

Refs: spec `re/analysis/frontend_screens/screens_10.md`; SNK strings `re/analysis/frontend_snk_strings.md`;
postmortem `re/analysis/frontend_diff_blindspot_postmortem.md`; diff `re/analysis/frontend_diff/diff_10.md`.
Dynamics semantics: MEMORY.md dynamics note + `td5_physics.c:164,10457`.
