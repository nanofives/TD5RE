# Frontend PORT-vs-ORIGINAL diff — screens 10–14

Authoritative gap list driving a faithfulness fix pass. ORIGINAL = `re/analysis/frontend_screens/screens_10.md`
(Ghidra VAs) + `frontend_rendering_model.md` + `frontend_flow_model.md`. PORT = `td5mod/src/td5re/td5_frontend.c`
(+ `td5_input.c` for device source). All PORT line numbers are from the worktree copy.

CLASS ∈ {MATCH, BUG, DIVERGENCE, MISSING, EXTRA, ARCH-DIV}. Cross-cutting tags: [FONT] [DECOUPLED] [ANIM] [LABEL] [ASSET].

Whole-frontend architectural divergence that colors every screen below: the original PRIMES (bakes
surfaces / queues rects) in the screen-fn and FLUSH-draws via `FlushFrontendSpriteBlits`. The port
draws everything LIVE each frame in `td5_frontend_render_ui_rects` (button loop + per-screen overlay
dispatch at td5_frontend.c:5547). This is an accepted [ARCH-DIV] for the whole module; it is NOT re-flagged
per element. What IS flagged is missing/wrong CONTENT (panels not rendered, values not displayed, input
not dispatched, labels wrong).

---

## Screen 10 — RunFrontendCreateSessionFlow (0x41A7B0)  [PORT: Screen_CreateSession td5_frontend.c:7061]

DXPTYPE/DirectPlay — whole-screen [ARCH-DIV] is expected. But the port collapses far more than the network
handshake: it drops the entire player-name sub-flow and the host-vs-join branching, and never renders the
header label.

| element/behavior | ORIGINAL (spec+addr) | PORT (line) | CLASS | FIX |
|---|---|---|---|---|
| Two parallel name-entry sub-flows (session-name host path states 0-8 vs player-name join path 0x10-0x13) selected by `g_networkLobbyEntryPhase`/`g_connBrowserCursorIndex` | screens_10.md inner-states; 0x41a7b0 | Single linear flow: state0 init → 1 slide-in → 2 input → 3 → 4..15 collapse to `set_screen(return)` (7108) | ARCH-DIV | Document. DXPTYPE unreachable end-to-end; the join/host split is moot without a real peer. Keep collapse. |
| Header label "Multiplayer" `CreateMenuStringLabelSurface(5)` re-queued every frame | spec ELEMENTS row1; 0x412e30, N=5 | Title-texture path keyed on `s_current_screen`; CREATE_SESSION has no title entry → likely blank header | MISSING | Add a title surface for CREATE_SESSION (or confirm `frontend_ensure_title_texture` covers it). [LABEL] |
| "Enter New Session Name" big button + Back button | btn(-0x1c0,0,0x1c0,0x40) slot0; Back(-0x70,0,0x70,0x20) slot1 | `create_button("Enter Name",-300,0,300,0x20)` + `("Back",-100,0,100,0x20)` (7067-68) | DIVERGENCE | Width 300 vs 0x1c0(448), height 0x20 vs 0x40; label "Enter Name" vs SNK_EnterNewSessionNameButTxt. [LABEL] |
| Text-input field + typed string baked into slot0 (`RenderFrontendCreateSessionNameInput` 0x41a530), fill 0x392152 idle | spec ELEMENTS; helper 0x41a530 | `frontend_render_text_input()` (7084 → 2479): live 448×80 panel at (96,280), title "ENTER PLAYER NAME", grey box, dark field | DIVERGENCE | Functionally present; geometry/colors differ; title text hardcoded "ENTER PLAYER NAME" even on the session-name path (should be session-name prompt). [LABEL][FONT] |
| Blinking green caret, suppressed when editState==2 / width>0x195 | spec; `(anim&0x20)==0 && editState!=2 && focus` | White caret, 350ms blink, gated `state==1` (2535) | DIVERGENCE | Color white vs green 0xff00; blink is wall-clock (350ms) not `anim&0x20` frame-count. [ANIM] |
| Empty-name → `g_localComputerName` fallback | spec state2 `iVar6==-2` | Port preseeds `"New Session"` (7070); no computer-name fallback | DIVERGENCE | Minor; preseed is a reasonable stand-in. |
| Mouse cursor + selection highlight bars | LOOP + `RenderFrontendDisplayModeHighlight` 0x4263e0 | mouse-hover green border in button loop (5727) | MATCH (approx) | — |

Counts: ARCH-DIV 2, MISSING 1, DIVERGENCE 4, MATCH 1.

---

## Screen 11 — RunFrontendNetworkLobby (0x41C330)  [PORT: Screen_NetworkLobby td5_frontend.c:7132]

The FSM is ported in structural detail (18 states, seal/config-poll/settings/start handshake). But ZERO
lobby panel content renders: there is no `NETWORK_LOBBY` case in the overlay dispatch (td5_frontend.c:5547),
so chat history, player roster, status, session header, chat input field, and error dialog are all unbacked.
Only the 6 generic buttons + background draw. This is the heaviest content gap of the 5 screens.

| element/behavior | ORIGINAL (spec+addr) | PORT (line) | CLASS | FIX |
|---|---|---|---|---|
| Whole network handshake (seal/kick/config/settings/DXPSTART) | spec states 0xc-0x11; 0x41c330 | Ported faithfully in structure (7361-7473) but DXPTYPE wire fmt incompatible | ARCH-DIV | Document; cannot complete vs orig peers. |
| Header label "Multiplayer" CreateMenuStringLabelSurface(5) | spec ELEMENTS; 0x412e30 | title-texture path, no NETWORK_LOBBY title entry | MISSING | Add title surface. [LABEL] |
| **Chat/message window panel** (0x200×0x80) | btn(SNK_MessageWindowButTxt,-0x200,0,0x200,0x80) slot1; `RenderFrontendLobbyChatPanel` 0x41bd00 bakes ≤6 chat rows | `create_button("Messages",-0x200,0,0x200,0x80)` (7158) frame only; NO chat-line rendering | MISSING | Render chat ring (`s_chat_*`) into the panel rect; up to 6 rows, name 0x96w + wrapped body. [FONT] |
| **Status / player-roster panel** (0xe0×0x86) + per-slot rows/status | btn(SNK_StatusButTxt) slot2; `RenderFrontendLobbyStatusPanel` 0x41b420 per-slot name + `SNK_NetPlayStatMsg[status*10]`; local row highlighted | `create_button("Status",-0xE0,0,0xE0,0x86)` (7159) frame only; NO roster rows | MISSING | Render `s_participant_flags[]`/`s_per_slot_status[]` rows + status text into the panel. [FONT][LABEL] |
| Session header bar (Session:/Player: + host name + "(host)") 0x1e0×0x20 | `g_lobbySessionHeaderSurface`; `RenderFrontendLobbyStatusPanel` | not rendered | MISSING | Render session/player header line at (cx-0xb0, cy-0x97). [LABEL] |
| Chat input strip + field + caret (0x1d0×0x18, max 0x3c) | label-less btn slot0; `RenderFrontendLobbyChatInput` 0x41a670 | `create_button("",-0x1D0,0,0x1D0,0x18)` (7157) frame only; `s_chat_input_buffer` filled but never drawn; `frontend_render_text_input` NOT called for lobby | MISSING | Draw the live chat input + caret into slot0; wire `frontend_render_text_input`-style field. [FONT] |
| Error/confirm dialog box (0x1e2×0x40) + Yes/No or OK | `g_lobbyErrorDialogSurface`; SNK_NetErrString1-4 by err code; states 6-10 | Buttons created (7300-04) but NO dialog box/text rendered; labels "Yes"/"No"/"Ok" guessed | MISSING + DIVERGENCE | Render dialog panel + message text; verify Yes/No vs single-OK gating (port: dialog_mode 0/2→Yes/No, 1→OK; orig gates on returnIdx). [LABEL] |
| "Change Car"/"Start"/"Exit" buttons | btn slots3/4/5; widths 200/0x78/0x78 | (7160-62) matching widths/heights | MATCH | — |
| Emote-token glyph substitution + `*team/*all/*kick/*ban` slash commands | `NormalizeFrontendChatTokens` 0x41c030 | comment only (7248); not implemented | MISSING | Implement token normalize before send (4). |
| Slide-in commit anim==0x14 (20); dialog 0x18 (24) | spec ANIM | `update_timed_animation(0x14,333)` (7175); dialog uses `s_anim_tick+=2` to 0xC (7354) ad-hoc | DIVERGENCE | Dialog out-anim is 12-frame hand-rolled vs orig 0x18 slide. [ANIM] |

Counts: ARCH-DIV 1, MISSING 7, DIVERGENCE 2, MATCH 1.

---

## Screen 12 — ScreenOptionsHub (0x41D890)  [PORT: Screen_OptionsHub td5_frontend.c:7483]

Plain 6-button nav menu; no value rows, no arrows (matches). Two notable points: the OK-commits-shadows
behavior and the cheat-code FSM gating.

| element/behavior | ORIGINAL (spec+addr) | PORT (line) | CLASS | FIX |
|---|---|---|---|---|
| 6 buttons Game/Control/Sound/Graphics/TwoPlayer/OK, widths 0x130 (OK 0x60) | spec ELEMENTS; 0x41d890 | All 6 at `-0x130,0,0x130,0x20` (7491-96) | DIVERGENCE | OK button width 0x130 in port but orig OK is 0x60 (`SNK_OkButTxt,-0x130,0,0x60,0x20`). Minor sizing. |
| Button dispatch idx0-5 → GAME/CONTROL/SOUND/DISPLAY/TWO_PLAYER/MAIN_MENU | spec input dispatch; 0x41d890 state6 | (7521-43) identical routing | MATCH | — |
| **OK commits in-race shadows** (camera=collisions^1, dynamics, traffic/cops, gRaceDifficultyTier) at 0x41dc8e | spec primed-globals; 0x41dc82/8e/9f | Port DEFERS: option screens edit `s_game_option_*` directly; `ConfigureGameTypeFlags` applies at race launch (7528-39 note) | DIVERGENCE (documented, faithful-equivalent) | Leave as-is. Single consumer (race launch); adding a commit risks double-apply. CONFIRMED design choice. |
| Header "Options" CreateMenuStringLabelSurface(6) | spec; 0x412e30 N=6 | title-texture path | MATCH (approx) | — |
| Slide-in anim==0x27 (39) + settle; slide-out==0x10 (16) | spec ANIM | `update_timed_animation(0x27,650)` (7509); out `(16,267)` (7555) | MATCH | — |
| **Cheat-code FSM runs only while OptionsHub active** | flow-model PART1 step15: gated `g_currentScreenFnPtr==PTR_ScreenOptionsHub` | `frontend_update_cheat_codes()` called UNCONDITIONALLY every frame (3275); separate A-Z history scanner, not the 6-slot scancode FSM gated to this screen | DIVERGENCE | Port runs cheats on every screen, not just OptionsHub. Functionally the same codes work but the orig restriction is lost. Consider gating to OptionsHub for fidelity. |

Counts: MATCH 3, DIVERGENCE 3.

---

## Screen 13 — ScreenGameOptions (0x41F990)  [PORT: Screen_GameOptions td5_frontend.c:7572; overlay :3986]

7 value rows + OK. Laps clamp fix CONFIRMED present. Value panel rendered live (overlay :3986). Main gaps:
row count mismatch (port has the row but the original Difficulty WRAP vs port matches), label "Police" vs
orig "POLICE"/Cops, and the value panel is per-row live text rather than the single re-baked surface.

| element/behavior | ORIGINAL (spec+addr) | PORT (line) | CLASS | FIX |
|---|---|---|---|---|
| Row0 Circuit Laps: display `shadow+1` "%d", clamp [0,3] no wrap | spec row0; 0x41fd78 (+1, no *2); input clamp | `snprintf("%d", laps+1)` (3997); input clamp [0,3] (7623-24) | MATCH (laps fix CONFIRMED) | Confirmed correct. |
| Row1 Checkpoint Timers ON/OFF toggle (&1) | spec row1; SNK_OnOffTxt | `^=1` (7629); displays on_off (3999) | MATCH | — |
| Row2 Traffic ON/OFF (&1) | spec row2 | `^=1` (7632); (4000) | MATCH | — |
| Row3 Cops ON/OFF (&1) — orig label via SNK_CopsButTxt | spec row3 | label `"Police"` (7584); `^=1`; displays on_off (4001) | DIVERGENCE | Label "Police" vs orig "Cops"/POLICE. [LABEL] — verify exact LANGUAGE.DLL string. |
| Row4 Difficulty EASY/NORMAL/HARD, WRAP 0↔2 | spec row4; SNK_DifficultyTxt; wrap | `+=delta` wrap 0↔2 (7638-40); difficulty[] (3988,4002) | MATCH | — |
| Row5 Dynamics SIMULATION/ARCADE (&1) — SNK_DynamicsTxt | spec row5 | `^=1` (7643); dynamics[] (3989,4003) | MATCH | — |
| Row6 3D Collisions ON/OFF (&1) | spec row6 | `^=1` (7646); (4004) | MATCH | — |
| OK button idx7 width 0x60 → OPTIONS_HUB | spec; 0x41fae3 | `create_button("OK",-0x60,0,0x60,0x20)` (7588); idx7→HUB (7650-52) | MATCH | — |
| Value panel = single shared `g_lobbyErrorDialogSurface` (0xe0×0x118) re-baked whole in state4, FLUSH-drawn as one rect | spec ELEMENTS row "Value-display panel" | Per-row live `frontend_draw_value_centered` at each `s_buttons[i].y+6` (3998-4004) | ARCH-DIV | Accepted live-render model; values correct. Document. |
| ◄► arrows rows 0-6 | spec; `InitializeFrontendDisplayModeArrows(N,1)` | `fe_draw_option_arrows(0..6)` (5749) | MATCH | — |
| State4 re-bake on any change | spec; jump to state4 | each change sets `s_inner_state=4` (7627 etc.) | MATCH | — |

Counts: MATCH 9, DIVERGENCE 1, ARCH-DIV 1.

---

## Screen 14 — ScreenControlOptions (0x41DF20)  [PORT: Screen_ControlOptions td5_frontend.c:7679; overlay :4516]

P0 GAP CONFIRMED: the device-source ◄► cycling is NOT dispatched, and the device-NAME panel is NOT rendered.
The port draws device ICONS (overlay :4516) but the row arrows on Player1/Player2 do nothing, and there is
no text panel showing the selected device's name. The screen is structurally a 5-button stub.

| element/behavior | ORIGINAL (spec+addr) | PORT (line) | CLASS | FIX |
|---|---|---|---|---|
| **Device-source ◄► cycling (idx0→g_player1InputSource, idx2→g_player2InputSource; +dir &7, skip empty desc, skip ==other player)** | spec input dispatch; 0x41df20 state6 | case 6 (7712-28) ONLY handles idx1/3 (Configure) + idx4 (OK). NO `frontend_option_delta()`, NO `td5_input_set_input_source` cycle | **MISSING (P0)** | Add device-cycle: on `frontend_option_delta()!=0` && btn 0/2, advance `s_input_source[player]` via `td5_input_set_input_source` with &7 wrap, skipping empty/duplicate devices; then re-bake value (state4). Plumbing exists (td5_input.c:266 set / :1860 type / :1855 name / :1845 enumerate). |
| **Device-NAME value panel** (shared `g_lobbyErrorDialogSurface` 0xe0×0xa0; 2× sprintf device label; keyboard fmt DAT_004658e4 vs joystick "%s %d") | spec ELEMENTS "Device-name value panel"; state4 bake | NOT rendered. Overlay (4516) draws only icons; no device-name text | **MISSING (P0)** | Render P1/P2 device name text (via `td5_input_get_device_name`) at panel pos (cx+0x4a, cy-0x8f) and (..-0x17). [LABEL][FONT] |
| Device ICON P1/P2 from Controllers.tga, row = device class, hidden when class==4 (none) | spec ELEMENTS; src y=class<<5, 0x40×0x20; hide if desc==0xffffffff | overlay (4538-41): P1 at (394,97), P2 at (394,217); row = `td5_input_get_device_type` (0/1/2) | DIVERGENCE | Icons drawn but: (a) NO hide-when-none (class 4) gating; (b) class map is 0/1/2 (kbd/joypad/joystick) vs orig 5-class (kbd/js/wheel/wheel+pedals/none); (c) P2 y=217 vs spec cy-0x17≈223. [ASSET] |
| Row0 "Player 1" label + ◄►; Row2 "Player 2" + ◄► | btn(SNK_Player1ButTxt,-0x100,0,0x100,0x20); arrows(0,1)/(2,1) | `create_preview_button("Player 1",...)` (7687); `("Player 2",...)` (7689); arrows on 0 & 2 (5774-75) | DIVERGENCE | Port uses PREVIEW (half-bright/disabled-look) buttons for the labels; orig builds normal label buttons that are the cycle targets. Preview styling + no cycle = rows look inert. [LABEL] |
| Row1/Row3 "Configure" (no arrows) → CONTROLLER_BINDING (slot 0/1) | btn(SNK_ConfigureButTxt) slots1/3; idx1→slot0, idx3→slot1 | `create_button("Configure",...)` (7688,7690); idx1/3 set `s_ctrl_player`=0/1 →BINDING (7717-22) | MATCH | — |
| OK idx4 width 0x60 → OPTIONS_HUB | spec; 0x41e0f1 | `create_button("OK",-0x60,...)` (7691); idx4→HUB (7723-26) | MATCH | — |
| In-race entry: extra line SNK_CtrlOptions_Ex + OK routes back to in-race options (returnIdx==~0) | spec conditional; state0 + OK route | Not handled; always OPTIONS_HUB | MISSING | Add in-race (returnIdx==-1) extra label + resume route. [LABEL] |
| Controllers.tga load path | spec: g_soundOptionsMenuVolume | `load_tga("Controllers.TGA","Front End/frontend.zip")` (7685) | MATCH (approx) | Note inconsistent archive name "Front End/frontend.zip" vs other loads "Front_End/FrontEnd.zip" (7684) — verify both resolve. [ASSET] |
| Header "Options" CreateMenuStringLabelSurface(6) | spec | title-texture path | MATCH (approx) | — |
| Slide-in==0x27 / out==0x10 | spec ANIM | (7701)/(7734) | MATCH | — |

Counts: MISSING 3 (2× P0), DIVERGENCE 3, MATCH 5.

---

## Cross-cutting summary

- **[ARCH-DIV] live-render vs prime/FLUSH** — whole module; values/buttons correct where content exists.
- **[ARCH-DIV] DXPTYPE** — screens 10 & 11 net handshake unreachable vs orig peers (accepted).
- **[LABEL]** — header titles rely on the title-texture path; net screens 10/11 likely have no title entry
  (MISSING). Several button labels guessed/differ ("Enter Name", "Police", "Yes/No/Ok"); not dumped from
  LANGUAGE.DLL. Device-name panel text absent (14).
- **[FONT]** — all panel text (chat rows, roster, device names) is live `fe_draw_text`, not baked surfaces.
- **[ANIM]** — caret blink is wall-clock 350ms not `anim&0x20`; lobby dialog out-anim hand-rolled (12) vs 0x18.
- **[ASSET]** — Controllers.tga class→row map is 3-class in port vs 5-class spec; no "none" hide.

## Top 3 fixes
1. **Screen 14 device-source ◄► cycling (P0, MISSING)** — wire `frontend_option_delta()` on buttons 0/2 to
   `td5_input_set_input_source` (+dir, &7 wrap, skip empty/dup); plumbing already in td5_input.c. Without it
   the device selectors are inert.
2. **Screen 14 device-NAME panel (P0, MISSING)** — render P1/P2 device-name text (via
   `td5_input_get_device_name`) at (cx+0x4a, cy-0x8f)/(-0x17); add class-4 "none" icon-hide.
3. **Screen 11 lobby panels (MISSING ×4)** — render chat history, player roster + status, session header,
   and chat input field/caret; currently only empty button frames draw, so the lobby is visually blank.

File: `re/analysis/frontend_diff/diff_10.md`
