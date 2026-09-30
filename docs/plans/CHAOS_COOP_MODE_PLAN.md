# CHAOS CO-OP — one car, many drivers (local multiplayer mode)

Status: **PLAN / not implemented**. Written 2026-09-29.
Scope: PORT-ONLY. No original-binary counterpart, so no Ghidra research is required.

Decisions locked 2026-09-29:

| # | Decision | Where |
|---|---|---|
| 1 | Brake goes with the throttle in teams of 2 and 3; split only at 4 | §2.2 |
| 2 | Controllers only, one per seat. No multi-seat keyboard | §5 |
| 3 | Roles rotate inside the team only, triggered by track progress, never across teams | §2.4 |

Decision 3 forced a redesign: **circuits have no checkpoints at all** in this engine, so the
trigger had to be generalised. See §2.5.

---

## 1. The mode in one paragraph

Two teams, one car each, head to head. Every human holds exactly **one control axis** of
their team's car. Nobody can drive alone: the car only moves if the throttle owner presses
and only turns if the steering owner(s) press. Every N seconds the roles **rotate inside
each team** with a 3-2-1 countdown, so the person who was steering left becomes the one on
the brake. Local only, 4 / 6 / 8 players, always even.

Why even-only and minimum 4: the mode is 1v1 between two teams, and a team needs at least
two people to split "turn" from "go". 4 players = 2 teams of 2, 6 = 2 teams of 3,
8 = 2 teams of 4.

---

## 2. Rules

### 2.1 Player count

| Total players | Teams | Per team | Legal |
|---|---|---|---|
| < 4 | - | - | **no** |
| odd | - | - | **no** |
| 4 | 2 | 2 | yes |
| 6 | 2 | 3 | yes |
| 8 | 2 | 4 | yes |

Upper bound is 8, not 9: `TD5_MAX_HUMAN_PLAYERS` is 9 (`td5_types.h:94`) and
`TD5_PLAT_MAX_JS_SLOTS` is 9 (`td5_platform_win32.c:107`), so 8 seats fit with a slot to
spare. 10 would not, and "2 teams of 5" adds no new role anyway.

### 2.2 Role sets

Roles are a property of the **row** a player occupies in their team panel, not of the
player. Row 1 is always the first role in the list.

| Team size | Row 1 | Row 2 | Row 3 | Row 4 |
|---|---|---|---|---|
| 2 | `STEER` (left + right) | `PEDALS` (throttle + brake) | - | - |
| 3 | `LEFT` | `RIGHT` | `PEDALS` (throttle + brake) | - |
| 4 | `LEFT` | `RIGHT` | `THROTTLE` | `BRAKE` |

**Decided (2026-09-29): brake is bundled with throttle at sizes 2 and 3.** The original
request listed size 3 as "izquierda, derecha, aceleración" with no brake owner, but a car
with no brake at all is not chaotic, it is broken. At sizes 2 and 3 the pedal owner owns
both pedals; at size 4 they split as requested.

### 2.3 Shared (unowned) actions

| Action | Owner |
|---|---|
| Handbrake | the `BRAKE` owner (size 4) / the `PEDALS` owner (sizes 2-3) |
| Horn | any seat on the team (OR of all seats) |
| Camera change / rear view | any seat on the team |
| Pause / Escape | **any seat, either team** |
| Gear up / down | nobody. Gearbox is forced to AUTO in this mode |

Forcing auto gearbox avoids a fifth role that nobody asked for and that would starve the
size-2 team.

### 2.4 Rotation

**Decided (2026-09-29): roles rotate inside the team only, triggered by track progress
(checkpoints), not by a wall clock.** Nobody ever changes team.

- Roles shift **down one row** each rotation. The bottom row wraps to row 1.
- Trigger is a mode option, default `CHECKPOINT`. See 2.5 for why this is not trivial.
- The trigger **arms a 3 second countdown**; the swap happens when it reaches zero. That is
  what makes the countdown possible at all (a checkpoint crossing is instantaneous and
  cannot be predicted from a timer).
- At the swap: role cards flash for 1.5 s, an SFX plays, and every seat's pad gets a short
  rumble so you feel your role change without reading the screen.
- Each team rotates on **its own** progress, so the two cars generally swap at different
  moments. Over a full race both cross the same milestones, so the count is equal.
- Steering ramp state is **not** reset on rotation. Resetting it would jerk the car at the
  worst possible moment.

### 2.5 What "checkpoint" actually means here

Researched 2026-09-29. The two track classes are not symmetric:

| Track class | Checkpoints | Source |
|---|---|---|
| Point to point (shipped) | **5**, last one is the finish | hardcoded `k_checkpoint_table[40][12]`, `td5_game.c:478-519` |
| Point to point (AUTO / generated) | **4** (2 if the ring is under 200 spans) | LEVELINF.DAT `+0x08`, written by `td5_trackgen.c:2681-2712` |
| **Circuit** | **none at all** | `advance_pending_finish_state` branches circuits into a lap/sector path at `td5_game.c:9229`; the checkpoint code is the `else` at `:9476` |
| Drag | 1 (the finish) | forced record 30, `td5_game.c:4982` |

So "rotate at every checkpoint" gives 4 rotations on a point-to-point race and **zero** on a
circuit. The mode needs a milestone that exists on both. Resolution:

**ROTATE AT** (mode option):

| Value | Point to point | Circuit | Drag |
|---|---|---|---|
| `CHECKPOINT` *(default)* | each real checkpoint crossing (4-5) | each **quarter lap** arc (4 per lap) | falls back to `TIME` |
| `HALF LAP` | every other checkpoint | each half lap arc | falls back to `TIME` |
| `LAP` | at the finish only (so: never, in practice) | each lap crossing | falls back to `TIME` |
| `TIME` | every N seconds (`10..60`, default 25) | same | same |
| `OFF` | no rotation | no rotation | no rotation |

The circuit arcs are computed by quantizing the actor's span into K equal arcs of the ring,
**after** passing it through `td5_track_branch_to_main_span()` (`td5_track.c:9099`). That
remap is mandatory on AUTO tracks: raw `span % ring` aliases a fork or corridor span onto an
unrelated low main-ring span, which would fire spurious rotations every time a car took a
branch.

`TIME` stays in the list because it is the only trigger that works on a drag strip, and
because it is the trivially testable one.

---

## 3. The architectural problem, and the fix

Today the port assumes a hard identity chain:

```
local player index  ==  input slot  ==  racer slot  ==  viewport pane
```

- `td5_game.c:2832` marks racer slots `1..num_human_players-1` human.
- `td5_game.c:4822` computes `humans = (split_screen_mode > 0) ? num_human_players : 1`
  and calls `td5_input_set_active_players(humans)` — this is the line that decides **how
  many pads get polled**.
- `td5_game.c:10981` computes `views = num_human_players + num_spectate_screens`.
- `td5_input.c:841` polls `s_control_bits[i]` by player index; `td5_input.c:1221` decodes
  `s_control_bits[slot]` by racer slot. Same array, two index spaces, currently identical.

CHAOS CO-OP breaks that identity on purpose. Introduce a third index space:

| Space | Meaning here | Count |
|---|---|---|
| **seat** | one human holding one pad / one keyboard key set | N (4, 6, 8) |
| **input slot** | `s_control_bits[]` entry | N for seats, then folded into 2 |
| **racer slot** | a car in the race | 2 |
| **viewport** | a split-screen pane | 2 (one per team car) |

Concretely:

- `g_td5.num_human_players = 2` (two cars, two panes, two result rows). Everything
  downstream of it keeps working untouched: `views = 2`, slots 0 and 1 are human,
  `g_actorSlotForView[]` is identity.
- `td5_input_set_active_players(N)` so the poll loop fills `s_control_bits[0..N-1]` with the
  **seat** words. This requires changing `td5_game.c:4822` to ask the mode for the pad count
  instead of deriving it from `num_human_players`.
- A fold step converts the N seat words into 2 car words, written back over
  `s_control_bits[0]` and `s_control_bits[1]`.
- Seat s binds device `td5_input_set_input_source(s, dev)` exactly as a normal player would,
  so exclusive DirectInput acquisition and per-device force feedback come for free
  (`s_ff[TD5_PLAT_MAX_JS_SLOTS]`, `td5_platform_win32.c:241`).

The only genuinely new invariant: **input slots 2..N-1 must never be decoded.** They are
seats, not cars. That already holds, because `td5_game.c:7432` gates the decode on
`s_slot_state[i].state == 1` and only slots 0-1 are human when `num_human_players == 2`.

---

## 4. The input fold (the engineering core)

### 4.1 Why it cannot be a bitwise OR

When a joystick is bound, the platform **replaces** the word rather than ORing:
`out->buttons = jbits;` at `td5_platform_win32.c:972`. And before that, at
`td5_platform_win32.c:926-935`, it has already collapsed both steering directions into a
single packed axis:

```c
int steer = left - right;                                  /* :926 */
int thr   = accel - brake;                                 /* :927 */
ax_x = CENTER + (steer * CENTER) / 256;                    /* :928  above centre = LEFT     */
ax_y = CENTER - (thr   * CENTER) / 256;                    /* :929  below centre = THROTTLE */
jbits |= (ax_x & 0x1FF) | TD5_INPUT_ANALOG_X_FLAG;         /* :934 */
jbits |= ((ax_y & 0x1FF) << 9) | TD5_INPUT_ANALOG_Y_FLAG;  /* :935 */
```

So a pad seat that owns only `LEFT` still emits a full signed X axis, and the digital
`STEER_LEFT` / `STEER_RIGHT` bits are not even present. ORing the eight seat words together
would produce garbage. The fold has to work in the **axis domain**.

### 4.2 Extraction

Per seat `s`, with `w = s_control_bits[s]`, `ax = s_analog_x[s]` (signed, + = left),
`ay = s_analog_y[s]` (signed, + = brake, because `ay = ax_y - CENTER = -thr`):

```
steer_of(s) = (w & ANALOG_X_FLAG) ? (ax * 256 / CENTER)
            : (w & STEER_LEFT ? 256 : 0) - (w & STEER_RIGHT ? 256 : 0)

pedal_of(s) = (w & ANALOG_Y_FLAG) ? (-ay * 256 / CENTER)
            : (w & THROTTLE ? 256 : 0) - (w & BRAKE ? 256 : 0)
```

Both normalized to `-256 .. +256`, positive = left / throttle.

### 4.3 Per-role contribution

| Role | Contribution |
|---|---|
| `STEER` | `steer_of(owner)` |
| `LEFT` | `max(steer_of(owner), 0)` — pushing right does nothing |
| `RIGHT` | `min(steer_of(owner), 0)` — pushing left does nothing |
| `PEDALS` | `pedal_of(owner)` |
| `THROTTLE` | `max(pedal_of(owner), 0)` |
| `BRAKE` | `min(pedal_of(owner), 0)` |

```
team_steer = clamp(sum of steering contributions, -256, +256)
team_pedal = clamp(sum of pedal    contributions, -256, +256)
```

Two people fighting over the wheel cancel out. That is the mode working as intended, not a
bug, and it belongs in `EXPECTED_BEHAVIOR.md`.

### 4.4 Re-encoding

**Decision D1 — encode digital when every contributor to that axis is digital, analog
otherwise.** Each axis decides independently (the X and Y flags are independent bits).

- All-digital steering re-emits `STEER_LEFT` / `STEER_RIGHT` bits, so the existing digital
  steering ramp (Path A, `td5_input.c:1347-1395`, `s_steer_ramp[]`) applies and a
  keyboard-only team does not get instant full lock.
- Any analog contributor re-emits the packed axis, so Path B
  (`td5_input.c:1474-1504`) applies:
  ```
  out_ax = CENTER + team_steer * CENTER / 256
  out_ay = CENTER - team_pedal * CENTER / 256
  bits  |= (out_ax & 0x1FF) | ANALOG_X_FLAG | ((out_ay & 0x1FF) << 9) | ANALOG_Y_FLAG
  ```
  and `s_analog_x[car] = out_ax - CENTER`, `s_analog_y[car] = out_ay - CENTER`.
- Mixed teams (pad LEFT owner + keyboard RIGHT owner) take the analog path with the digital
  contributor counting as a full 256. Document it; do not try to be clever.

Then OR in the shared bits from section 2.3.

### 4.5 Where it runs

`td5_input_poll_race_session` (`td5_input.c:776`) currently does poll **and** per-player
post-processing (gear debounce, camera cooldown, horn edge latch) in one loop body
(`td5_input.c:841-1076`). The fold has to sit between them, otherwise the camera and horn
edge latches for car slot 0 would only ever see seat 0's pad.

**Restructure:** split that loop into

1. **Pass 1 — raw poll**, `i < s_active_players`: exactly the current lines 845-872
   (poll, inputscript overlay, control-socket overlay). Nothing else.
2. **Fold**, no-op unless chaos is active: snapshot the N seat words, compute two car words,
   write them into `s_control_bits[0..1]` / `s_analog_*[0..1]`.
3. **Pass 2 — post-processing**, over the car slots (`2` in chaos, `s_active_players`
   otherwise): the rest of the current loop body.

When chaos is off, pass 2 iterates the identical range over identical values, so the
restructure must be behaviour-preserving. **Prove it with a fixed-seed RaceTrace A/B**:
the same pinned race on the contract-commit exe and on the split exe must produce
byte-identical `log/race_trace_*.csv`. (The selftest golden-hash check was retired by design
on 2026-08-07 in favour of the `st_invariants_*` checker, so it no longer asserts bit-exact
determinism; the A/B diff is the only instrument that does.) Any byte of difference means
the split changed single-player input.

Two existing precedents for overlaying into the polled word at this seam:
`td5_inputscript_race_bits(i)` (`td5_input.c:866`) and `td5_control_race_bits(i)`
(`td5_input.c:872`).

### 4.6 Module boundary

New `td5_chaos.c` / `td5_chaos.h` owns the seat table, the role map, the rotation timer and
the fold maths. To avoid a new `extern` in a `.c` (the structure ratchet in
`scripts/lint_structure.ps1` fails CI on those), the fold is a **pure function** taking
buffers, called from `td5_input.c`:

```c
int td5_chaos_fold_inputs(const uint32_t *seat_bits, const int16_t *seat_ax,
                          const int16_t *seat_ay, int seat_count,
                          uint32_t *out_bits, int16_t *out_ax, int16_t *out_ay);
/* returns 0 (inert) when the mode is not active */
```

---

## 5. Device requirements: one controller per seat

**Decided (2026-09-29): controllers only. Multi-seat keyboard is OUT of scope.**

The mode needs **N physical controllers for N players**. The keyboard can hold at most one
seat, exactly as it does in normal split-screen today.

This is a deliberate limit, and it has a cost worth stating plainly: a 8-player game needs 8
pads, and the mode cannot be exercised at all by anyone who does not own N pads. The port
would in principle support several keyboard seats (`s_kb_bindings[2][11]` at
`td5_platform_win32.c:88-93` still carries a second WASD row), but the poll hardcodes
`s_kb_bindings[0]` at `td5_platform_win32.c:815`, `td5_plat_input_set_device` rejects
`player > 1` at `:623-625`, and the keyboard branch of `td5_plat_input_poll` ignores its
`slot` argument entirely. Reopening that is a separate change and is not planned here.

Direct consequences for the rest of this plan:

- `td5_platform_win32.c` drops off the file list in section 8.
- The `TD5RE_CHAOS_FAKE_SEATS` dev knob in section 11 stops being a convenience and becomes
  **the only way** to exercise the mode without a pad wall. It is required, not optional.
- The teams screen must reject a second keyboard claim with `frontend_play_sfx(10)` rather
  than silently seating two players on the same key set.

---

## 6. UI

### 6.1 Flow

```
MAIN MENU
  └─ TWO PLAYER  →  MP LOBBY (30)            press-to-join, N devices claim a slot
        └─ MP MODE VOTE (35)                 new row: CHAOS CO-OP
              └─ MP MODE CONFIG (36)         rotation period, laps, AI count
                    └─ CHAOS TEAMS (NEW)     the board below
                          └─ MP CAR GRID (49)  2 cars, one per team
                                └─ RACE
```

The lobby already does exactly the join step this mode needs: `td5_plat_input_scan_join()`
edge-detection with per-HID de-duplication at `td5_fe_net.c:382-440`, storing devices in
`s_mp_join_device[]`. The joined count becomes the **seat** count instead of
`num_human_players`.

### 6.2 Validation gate

The CHAOS CO-OP row on MP MODE VOTE is **disabled** (`s_buttons[i].disabled = 1`, greyed via
`td5_frontend.c:10627-10628`) when any of:

- joined count < 4
- joined count is odd
- joined count > 8
- `network_active` (local only; see section 9)

with the reason printed under the mode description, and `frontend_play_sfx(10)` on a
confirm attempt. Precedents: lighting-options greyed rows `td5_fe_menu.c:1872-1877, 1955`,
cup-tier gating `td5_fe_menu.c:1383-1397`.

The joined count is live, so the row un-greys the moment a 5th player becomes a 6th.

### 6.3 The CHAOS TEAMS screen

640x480 virtual space. MP bands apply: left margin 112, right edge 628, nothing below y=460
(`td5_frontend_internal.h:461-475`, `FRONTEND_SCREEN_GUIDE.md:61-72`).

```
      CHAOS CO-OP  -  TEAMS AND ROLES                        8 PLAYERS

  TEAM RED                             TEAM BLUE
 +-----------------------------+      +-----------------------------+
 | 1  LEFT      P1  Pad 1    * |      | 1  LEFT      P2  Pad 2    * |
 | 2  RIGHT     P4  Pad 4    * |      | 2  RIGHT     P5  Pad 5    * |
 | 3  THROTTLE  P7  Pad 7      |      | 3  THROTTLE  P6  Pad 6    * |
 | 4  BRAKE     --             |      | 4  BRAKE     P8  Keyboard * |
 +-----------------------------+      +-----------------------------+

  WAITING:   [ P3  Pad 3 ]

  ROTATE AT                                  <   CHECKPOINT   >

  Roles shift down one row. Row 1 wraps to the bottom. Team only.

                        [  START  ]      1 seat still empty
```

Geometry:

| Element | x | y | w | h |
|---|---|---|---|---|
| Title (`fe_race_draw_screen_title`) | 126 | 17 | - | - | 
| Player-count badge (right aligned) | ~560 | 17 | - | - |
| Team panel A | 112 | 92 | 248 | 198 |
| Team panel B | 380 | 92 | 248 | 198 |
| Panel header band | +0 | +0 | 248 | 24 |
| Seat row `k` (0-based) | +4 | 120 + 42k | 240 | 38 |
| WAITING strip | 112 | 300 | 516 | 42 |
| ROTATE AT selector row | 112 | 352 | 516 | 28 |
| Hint line (small text, centered) | 370 | 388 | - | - |
| START button | 322 | 404 | 96 | 32 |

Panels are drawn at a fixed height for all team sizes so the layout does not jump when the
player count changes; unused rows are greyed and inert.

Seat card contents, left to right inside the 240px row:

1. 4px colour bar in the player colour (`k_mp_player_colors[]`, `td5_frontend_internal.h:299`)
2. row number `1..4`
3. role badge, small caps
4. player token `P3`
5. device label, truncated with `fe_fit_text_scale` (`Pad 3`, `Keyboard`)
6. ready tick on the right when locked

Empty rows show `--` in grey `0xFF6A6A6A`.

Drawing primitives, all existing: `fe_draw_quad` (`td5_frontend.c:9829`) for panel fills and
4-quad borders, `fe_draw_text` / `fe_draw_small_text` / `fe_draw_text_centered`,
`fe_draw_button_frame_fill_scaled` for the START button,
`fe_draw_option_arrows` (`td5_frontend.c:6848`) for the rotation selector. The nearest
existing screen to copy wholesale is `frontend_mp_position_render`
(`td5_fe_mp_setup.c:1727-1827`) — grid of cells, one owner per cell, swap on collision,
per-cell ready lock.

### 6.4 Interaction

Each physical device drives **its own cursor**, exactly like MP MODE VOTE
(`td5_fe_race.c:3840-3864`) and MP TEAM SELECT (`td5_fe_race.c:4944-4950`). Per-device nav
comes from `td5_plat_input_device_nav(enum_index)` (`td5_platform.h:481`), wrapped today by
the static `mp_simul_player_nav(player)` at `td5_fe_race.c:1573`. Promote that helper to
`td5_frontend_internal.h` rather than duplicating it.

| Input | Effect |
|---|---|
| Up / Down | move your cursor between rows within the current panel |
| Left / Right | move your cursor to the other team's panel |
| A on an empty row | take that seat, lock it |
| A on your own locked row | unlock (go back to WAITING) |
| A on a row owned by someone else | **swap** both players, both stay locked |
| B | unlock and return to WAITING |
| START (host only) | begin, if every seat is filled |
| ESC (host only) | back to MP MODE CONFIG, with the usual "GO BACK?" confirm |

Host = the first joined device, same convention as the existing MP flow.

Auto-seating: on entry, players are pre-seated in join order (P1 → RED row 1, P2 → BLUE row
1, P3 → RED row 2, alternating) and all locked. So a group that does not care can press
START immediately; a group that does can rearrange. This matters, because a board that
starts empty with 8 people fighting over cursors is a bad first 30 seconds.

**Non-negotiable caveat for verification:** control-socket `tap_key` / `inject_key` do **not**
reach frontend menus. This screen cannot be self-verified by the MCP driver. Verify layout
by `--StartScreen` + `TD5RE_FRAMEDUMP` and read the PNG; verify seat claiming by hand with
real pads.

### 6.5 In-race HUD

Two panes (one per team car). Each pane draws a **role strip** along its bottom edge:

```
 P1 LEFT    P4 RIGHT    P7 THR    P3 BRAKE       SWAP AT CP 3/5
```

- Each entry in that player's colour, role name in white.
- The right-hand readout depends on the trigger: `SWAP AT CP 3/5` for checkpoints,
  `SWAP AT LAP 2` for laps, `SWAP 0:12` for the time trigger.
- Once the trigger fires, the strip pulses and a big `3` / `2` / `1` is drawn at the pane
  centre using `hud_draw_centered_ttf_char` (`td5_hud.c:4080`) at the countdown scale,
  reusing the look of the race-start countdown without reusing its timer.
- When it reaches zero: `SWAP!` plus enlarged role cards for 1.5 s, one SFX, and a rumble
  pulse to every seat's device.

Closest template: `td5_hud_draw_brokedown_prompt` (`td5_hud.c:2071-2116`) for a per-pane
pulsing prompt with TTF-first and a bitmap fallback; `td5_hud.c:2040-2056` for the
`ENDS %d:%02d` battle timer, which is the same shape as `SWAP 0:12`.

Note the minimap already draws checkpoint markers (`minimap_emit_checkpoint_dash`,
`td5_hud.c:6621`), so on a point-to-point track the players can see the next rotation point
coming without any new art.

The pre-race controller tutorial (`td5_tutorial.c`) is the precedent for an all-players-ready
overlay if a "here is your starting role" card is wanted before the lights: it already holds
the race countdown (`td5_game.c:7337`), does per-player edge input and an all-ready mask
(`td5_tutorial.c:389-400`), and draws in its own uniform-scale space.

---

## 7. Rotation implementation

### 7.1 Detect the milestone on the sim clock, not the render clock

The obvious hook is the crossing site itself, `td5_game.c:9572-9587`, which is per-actor,
one-shot, and already logs `"Checkpoint crossed: slot=%d cp=%d"`. **Do not use it.** Its
caller `advance_pending_finish_state` (`td5_game.c:9128`) runs **once per rendered frame**,
not per sim tick, and says so at `:9124-9126`. Arming a countdown there makes the rotation
frame-rate dependent.

Instead **poll for the change from the 30 Hz sim tick**, the same way `battle_chase_tick`
does (`td5_game.c:11780`, called from the fixed-step block at `td5_game.c:7668`):

```c
/* td5_chaos.c — one entry per team car */
static int32_t s_last_milestone[2];
static int32_t s_swap_ticks_left[2];
static int32_t s_rotation_step[2];

static int32_t milestone_of(int car) {
    switch (trigger) {
    case CHECKPOINT:
        if (track is point-to-point) return td5_game_get_player_lap(car);
        /* circuit: K equal arcs of the ring, branch-safe */
        return lap * K + (td5_track_branch_to_main_span(span) * K) / ring;
    case LAP:  return td5_game_get_player_lap(car);
    case TIME: return elapsed_ticks / (period_secs * 30);
    }
}

void td5_chaos_tick(void) {                  /* once per FIXED sim tick */
    for (int car = 0; car < 2; car++) {
        int32_t m = milestone_of(car);
        if (m > s_last_milestone[car]) {     /* strictly forward only */
            s_last_milestone[car] = m;
            s_swap_ticks_left[car] = 3 * 30; /* arm the 3-2-1 */
        }
        if (s_swap_ticks_left[car] > 0 && --s_swap_ticks_left[car] == 0) {
            s_rotation_step[car]++;
            notify_seats(car);               /* rumble + sfx + HUD flash */
        }
    }
}
```

`td5_game_get_player_lap(slot)` (`td5_game.c:1688`, declared in `td5_race_state.h:68`)
returns `s_metrics[].checkpoint_index`, which is **laps on circuits and checkpoints passed
on point-to-point**. That overload is normally a trap; here it is exactly the quantity
wanted, and it means one accessor covers both track classes for the `CHECKPOINT` and `LAP`
triggers. Say so in a comment, or the next reader will "fix" it.

Three traps the research turned up, all of which the `m > s_last_milestone` guard handles:

- `track_span_normalized` **wraps** every lap, and also wraps to near `ring-1` when a car
  reverses across span 0. That wrap once ended races after a 2 s reverse off the line
  (`td5_game.c:9485-9503`). Strictly-forward comparison plus the branch remap contains it.
- `track_span_accumulated` is `int16_t` and **overflows past 32767** (about 10 laps of a
  3000-span ring), and goes negative in reverse races. Do not use it.
- `td5_game_get_slot_progress` (`td5_game.c:1745`) multiplies the lap accessor by the ring
  length, so on a point-to-point track it jumps by a whole ring at every checkpoint. Safe on
  multi-lap circuits only. Do not use it here.

### 7.2 Applying the rotation

Role lookup is pure arithmetic, no stored permutation:

```c
role_index = (base_row_of_seat + s_rotation_step[car]) % team_size;
```

Rotation state is per car, since the two cars hit their milestones at different times.

Sim rate is a fixed 30 Hz (`td5_game.c:10818`), so `secs * 30` is exact. Do **not** reuse
`g_cameraTransitionActive` (the 3-2-1 start countdown) for the swap countdown: it is coupled
to `g_td5.paused` at `td5_game.c:8635` and to camera/AI gating, and re-arming it mid-race
would freeze the grid.

Reset `s_last_milestone` / `s_swap_ticks_left` / `s_rotation_step` per race next to
`td5_game.c:2248-2251`, and seed `s_last_milestone` when the start countdown clears so the
grid line itself does not read as a milestone.

---

## 8. Files to touch

### New

| File | Contents |
|---|---|
| `td5mod/src/td5re/td5_chaos.c` | seat/team table, role map, rotation timer, input fold, read-only queries |
| `td5mod/src/td5re/td5_chaos.h` | public API (fold, tick, queries, activation) |
| `td5mod/src/td5re/td5_fe_chaos.c` | `Screen_ChaosTeams` handler + `frontend_chaos_teams_render` |

### Modified

| File | Change |
|---|---|
| `srcs.txt` | add the two new `.c` modules (single source of truth for all build paths) |
| `td5_types.h` | `TD5_MP_MODE_CHAOS_COOP = 5`, `TD5_MP_MODE_COUNT` 5→6; `TD5_SCREEN_CHAOS_TEAMS`, `TD5_SCREEN_COUNT` 54→55; new `TD5_ChaosRole` and `TD5_ChaosTrigger` enums. **Nothing is appended to `TD5_MpModeConfig`**: it is embedded in `TD5_NetRaceConfig`, whose size is pinned by a `_Static_assert` in `td5_net.h` (484 bytes, wire format). The seat table is `TD5_ChaosConfig`, owned by `td5_chaos.c` (found while building the contract, 2026-09-29) |
| `td5_frontend.c` | screen-table row `:102`; title text `:1573`; parent screen `:3764`; button-anim state `:6060` + has-anim list `:6176`; post-button render switch `:10744` |
| `td5_fe_race.c` | `k_mp_mode_names[]` `:3562` + `k_mp_mode_desc[]` `:3573` (both sized by `TD5_MP_MODE_COUNT`, so they fail to compile until updated — good); defaults case in `mp_mode_config_apply_defaults` `:3693`; ROTATE AT / period / laps / AI rows in `mp_cfg_build` `:4004`; vote-row disable gate `:3765`; route to `CHAOS_TEAMS` after config `:4098` |
| `td5_fe_mp_setup.c` | `frontend_commit_pane_layout` `:110` → 2 panes; `frontend_init_race_schedule` `:144` → `num_human_players = 2`, per-team car identity at `:507-543`, seat table published to `td5_chaos` |
| `td5_frontend_internal.h` | promote `mp_simul_player_nav`; declare the new screen handler and render fn |
| `td5_game.c` | pad count at `:4822` (ask the mode, not `num_human_players`); race-init hook near the battle block `:2464`; per-race reset near `:2248`; `td5_chaos_tick()` in the fixed-step block `:7660`; `k_ssw_chaos_teams` route `:1138` |
| `td5_input.c` | split `td5_input_poll_race_session` `:776` into poll / fold / post-process (section 4.5) |
| `td5_hud.c` | per-pane role strip, swap countdown, swap flash |
| `td5_race_state.h` | read-only queries (`td5_chaos_active`, `role_of_seat`, `swap_secs_left`, `next_milestone_label`) |
| `td5_selftest.c` | screen-walk row `:332`; race-matrix rows `:228` |
| `EXPECTED_BEHAVIOR.md` | "opposed steering cancels" is intended, not a bug |
| `re/assets/frontend/lang/es_AR.txt` | `TR()` strings for the new screen and role names |
| `CHANGELOG` + `pending_to_test.csv` | per the `/fix` + `/end` convention |

---

## 9. Non-goals and limits

- **Local only.** Not offered when `network_active`. The mode needs 4-8 devices on one
  machine; the netcode is per-slot lockstep with no state correction, and folding 8 remote
  seats into one authoritative car word is a separate project. The config lives in the
  module-owned `TD5_ChaosConfig`, not in the replicated `TD5_MpModeConfig`, so the mode does
  not touch the net protocol version.
- **Replay does not round-trip.** `td5_input_write_frame` records slots 0 and 1 only
  (`td5_input.c:1101`). That happens to be exactly the two car slots here, so the *car*
  replays correctly; the per-seat attribution is lost. Acceptable, worth a note.
- **No per-seat car select.** One car per team, picked by the team's row-1 seat.
- **AI opponents default 0.** The mode is a duel. 0-4 allowed as an option.
- **9+ players not supported.** See section 2.1.
- **One controller per seat.** No multi-seat keyboard. See section 5.
- **No cross-team rotation.** Players never change car. See section 2.4.

---

## 10. Execution: waves, zones, and who verifies

Decided 2026-09-29: the large code changes are done by **child sessions on account `claude2`
running `claude-opus-5`**, one per code zone, in parallel where the files are disjoint. The
orchestrating session is the **master**: it owns the contract, every merge, and every
verification. A child's own "it builds" is a report, not evidence; the master rebuilds and
re-measures after each merge.

### 10.1 Wave 0 — contract (master, DONE on `feat/chaos-coop`)

The shared interface every zone codes against, landed before any child starts so the zones
cannot drift apart:

- `td5_types.h`: `TD5_MP_MODE_CHAOS_COOP`, `TD5_SCREEN_CHAOS_TEAMS`, `TD5_ChaosRole`,
  `TD5_ChaosTrigger`, `TD5_CHAOS_MAX_SEATS` / `TD5_CHAOS_TEAMS`
- `td5_chaos.h`: the complete public API (config commit, rules, activation, roles,
  lifecycle, HUD queries, fold)
- `td5_chaos.c`: rules + activation + role lookup **final**; rotation block **stubbed**
- `td5_chaos_fold.c`: fold **stubbed** (always inert)
- `srcs.txt`: both modules registered; mode name/description strings in `td5_fe_race.c`

With every stub inert the contract build behaves exactly like master. Its dev exe is kept as
the **A/B baseline** for the input-split byte-identity proof.

### 10.2 Wave 1 — four zones in parallel

Each child gets its own worktree and branch off the contract commit, and a strict file
ownership list. **A child may only edit the files its zone owns.** Needing another zone's file
is a stop-and-report, not a quiet edit.

| Zone | Branch | Owns (exclusive) | Delivers |
|---|---|---|---|
| **Z1 INPUT** | `chaos/z1-input` | `td5_input.c`, `td5_chaos_fold.c` | poll loop split into poll / fold / post-process (§4.5), the full fold (§4.2-4.4), shared-action routing (§2.3) |
| **Z2 FRONTEND** | `chaos/z2-frontend` | `td5_fe_chaos.c` (new), `td5_frontend.c`, `td5_fe_race.c`, `td5_fe_mp_setup.c`, `td5_frontend_internal.h`, `re/assets/frontend/lang/es_AR.txt`, `srcs.txt` (only to add `td5_fe_chaos.c`) | CHAOS TEAMS screen (§6.3-6.4), vote-row gate (§6.2), mode-config rows, commit via `td5_chaos_commit_config`, race-schedule special case (2 cars, 2 panes) |
| **Z3 GAME** | `chaos/z3-game` | `td5_chaos.c` (rotation block + fake-seats), `td5_game.c`, `td5_race_state.h` | active-players decoupling (`td5_game.c:4822`), seat→device binding at race start, milestone detection + swap countdown on the sim tick (§7), rumble notify, `TD5RE_CHAOS_FAKE_SEATS` |
| **Z4 HUD** | `chaos/z4-hud` | `td5_hud.c` | per-pane role strip, 3-2-1 swap countdown, SWAP flash (§6.5) |

Why this split is conflict-free: the four ownership lists are disjoint, and every
cross-zone call goes through `td5_chaos.h`, which is frozen for the wave.

### 10.3 Master verification gates (after EACH merge into `feat/chaos-coop`)

| Gate | How | Applies to |
|---|---|---|
| G1 scope | `git diff --stat` only touches the zone's owned files | all |
| G2 build | `build_all.bat` by absolute path; grep `BUILD OK` for dev **and** release, check both exe mtimes (the script exits 0 even on failure) | all |
| G3 structure | lint line at the end of `build_all`: extern / td5_game.h includers / warnings all at baseline | all |
| G4 byte identity | same pinned fixed-seed RaceTrace race on baseline exe vs merged exe; `race_trace_*.csv` must be byte-identical with the mode OFF | Z1, Z3 (both touch the shared race path) |
| G5 selftest | `pwsh scripts/selftest.ps1 -Suite full`, window visible, 0 FAIL | all |
| G6 functional | Z1: fake seats + control-socket per-seat hold → each seat moves exactly its axis. Z2: `--StartScreen=54 --StartScreenDirect=1` + framedump, read the PNG. Z3: `TIME` trigger at 10 s, role step advances on the expected tick in `race.log`. Z4: framedump mid-race with a swap pending | per zone |

Merge order is whatever finishes first; the gates run after each one, so a regression is
attributable to exactly one zone.

### 10.4 Wave 2 — integration (one child, after all four merged and green)

| Zone | Owns | Delivers |
|---|---|---|
| **Z5 INTEGRATION** | `td5_selftest.c`, `td5_control.c`, `EXPECTED_BEHAVIOR.md`, `FRONTEND_SCREEN_GUIDE.md`, `td5_game.c` (**only** the `k_ssw_*` route table), changelog + `pending_to_test.csv` | selftest screen-walk row + race-matrix rows (fake seats), `--StartScreen` route, control verb if Z1/Z3 found `hold_action` insufficient, docs |

Then master runs the full gate set once more, regenerates the module table
(`pwsh scripts/gen_module_table.ps1 -Write`), and hands the user the manual 4-pad checklist.
The mode is not called done until that manual pass happens (§11).

### 10.5 Risk order

Z1 is the risky one: it touches the input path every race in the game goes through, which is
why G4 exists. Z3's circuit arc path is second: it is the only piece that reasons about branch
spans on AUTO tracks.

## 11. Testing

- **Fixed-seed RaceTrace A/B is the gate for Z1 and Z3** (G4). Golden hashing was retired
  by design, so do not expect the selftest to catch a determinism break here.
- **`TD5RE_CHAOS_FAKE_SEATS=N` dev knob is REQUIRED, not a nicety** (dev builds only).
  Because the mode is controllers-only (section 5), without it nobody who lacks N pads can
  reach the mode at all, and no agent or CI run can ever exercise it. It fabricates N seats
  bound to device 0 so the mode can be entered, rotated and traced. Pair it with a
  control-socket verb `chaos_seat_bits <seat> <bits>` so an automated test can drive one
  seat at a time and assert the resulting car word.
- **Milestone test without driving:** with `TD5RE_CHAOS_FAKE_SEATS` plus the existing
  `--StartSpanOffset`, a race can be started near a checkpoint threshold to reach the
  trigger in seconds instead of minutes. Remember `--StartSpanOffset` drifts by +15..+40
  spans by the time a capture lands, so assert on the span actually reached, not the one
  requested.
- **Manual pass is mandatory** before calling it done: 4 real pads minimum, one full race
  with at least 3 rotations, on both a point-to-point track and a circuit.
- Remember the measurement hygiene rules that already bit this project: report the pre-fix
  and post-fix measurement in the **same** run, keep the window visible when timings matter,
  and clear `TD5RE_*` env knobs between launches in the same shell.
