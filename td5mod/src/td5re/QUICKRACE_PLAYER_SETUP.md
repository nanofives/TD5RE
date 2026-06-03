# Quick Race Player / Opponent Setup — feature + debugging reference

**Status:** port enhancement (intentional divergence from the original). Infra
that will later replace the separate Two Player menu (`0x420C70`) + config.

The Quick Race screen (`Screen_QuickRaceMenu`, orig `0x4213D0`) exposes:

| Row | Control | Range | Backing state |
|-----|---------|-------|---------------|
| Car | car selector | unlocked cars | `s_selected_car` |
| Track | track selector (Drag Strip excluded) | unlocked tracks ≠ 19 | `s_selected_track` |
| Direction | Forwards / Backwards (hidden on forward-only/circuit tracks) | 0/1 | `s_track_direction` → `g_td5.reverse_direction` |
| **Players** | human player count | **1..6** | `s_num_human_players` → `g_td5.num_human_players` |
| **Opponents** | AI opponent count | **0..5** | `s_num_ai_opponents` → `g_td5.num_ai_opponents` |

Constraint: `num_human_players + num_ai_opponents ≤ TD5_MAX_RACER_SLOTS (6)`.
Default **1 human + 5 AI = 6** reproduces the legacy single-race grid.

---

## Debug knob: `DefaultOpponents`

The number of opponents can be forced **without driving the menu**, so AutoRace /
trace harnesses can launch a reduced field directly.

```
[Game]
DefaultOpponents = N      ; -1 = full grid (1 player + 5 AI). 0..5 = N AI opponents.
```
or CLI: `--DefaultOpponents=N` (CLI > INI > default).

- `-1` (default): no override — legacy 6-car grid.
- `0..5`: AutoRace launches **1 human + N AI = N+1 total racers**.
- **Release builds force `-1`** (`main.c` dev-knob clamp), so it's a dev-only tool.

### Why it's a useful debugging tool

- **Isolate one AI:** `--DefaultOpponents=1` → player + a single AI. Cleanest way
  to watch/trace one opponent's routing, rubber-band, or collision in isolation.
- **Minimal repro:** fewer actors = simpler state when bisecting a crash, a
  physics blow-up, or a standings bug. Pairs with `--AutoThrottle=1`.
- **Spawn-path coverage:** exercises the reduced-field disable/spawn paths that
  the full grid never hits (a Ghidra-correct change can look "no-op" at 6 cars).
- **Player-alone-ish:** `--DefaultOpponents=0` → player races alone. (Distinct
  from `SoloRace`, which also forces 1st place and other suppression.)

### Example

```
td5re.exe --AutoRace=1 --DefaultTrack=0 --DefaultOpponents=2 --AutoThrottle=1
```
→ 3-car Moscow race (player + 2 AI), throttle held.

### Verification log lines

`log/race.log`:
```
InitRace: single-race racer count=3 (humans=1 opponents=2)
InitRace: spawning 3 racers (humans=1 opponents=2)
Actor spawn: slot=0 ...        # only slots 0..total-1 appear
AutoRace: DefaultOpponents override -> 3 racers total (1 human + 2 AI)
```
`log/race.log` (ai tag):
```
single-race: g_slot_state[3..5] = 3 (humans=1 opponents=2)
```

---

## Data flow

1. **Menu** writes `s_num_human_players` / `s_num_ai_opponents` (clamped by
   `frontend_quickrace_clamp_counts()`).
2. **`frontend_init_race_schedule()`** commits them into `g_td5.num_*`,
   **gated to `s_current_screen == TD5_SCREEN_QUICK_RACE`**. Every other launch
   flow forces `1 human + 5 AI` → the feature is inert outside Quick Race.
   (AutoRace runs with `s_current_screen == MAIN_MENU`, then applies the
   `DefaultOpponents` override above.)
3. **`td5_game.c InitRace`** and **`td5_ai.c`** consume `g_td5.num_*` to size and
   disable racer slots (see the four application points below).

### Effective-humans cap (>2 deferred)

The engine has **only single + 2 split-screen viewport layouts**
(`InitializeRaceViewportLayout` `0x42C2B0`; selector = `gSplitScreenMode + 1`,
split mode masked `& 1`). So **effective human-driven slots are capped at 2**.
Requesting 3–6 humans records the intent but the extra human slots run **as AI**
until N-way split rendering/input is built. `frontend_init_race_schedule` emits a
`TD5_LOG_W` when `num_human_players > 2`.

---

## The FOUR places the count is applied

A reduced field must be applied **consistently in all four** spots, or you get
half-disabled slots (visible-but-dead, or driven-but-not-rendered):

1. **`td5_frontend.c frontend_init_race_schedule()`** — sets `g_td5.num_*`, drives
   `split_screen_mode` from effective humans, assigns extra-human cars (slots
   `1..eff-1` default to the player's car), AI fill proceeds for the rest.
2. **`td5_game.c InitRace` "Mark unused racer slots"** — `s_slot_state[t..5].state = 3`
   and `g_racer_count = total` (drives the minimap / race order).
3. **`td5_game.c InitRace` actor-spawn loop** — caps `racer_count` at `total` so
   slots `≥ total` never load a vehicle mesh or take a grid position.
4. **`td5_ai.c td5_ai_init_race_actor_runtime()`** — `g_slot_state[t..5] = 3`.

All four are gated `!time_trial && !drag && !wanted && num_human_players >= 1`, so
the default (`1+5=6`) and special modes are untouched.

---

## ⚠️ Gotcha: there are TWO independent slot-state tables

This is the single biggest trap (it caused the "dropped opponents spawn and drive
in circles" bug):

- **`td5_game.c s_slot_state[].state`** — game-side authority (`1`=human, `0`=AI,
  `3`=disabled). Gates render (via `td5_game_get_slot_state`), sound, standings,
  finish detection.
- **`td5_ai.c g_slot_state[]`** — a **separate** array that drives the AI tick
  (whether a slot is steered / throttled).

**Both must be set to `3` to fully disable a slot.** Setting only the game table
leaves the AI still driving it; setting only the AI table leaves it
rendering / in standings. Cop-chase / drag / solo modes set both (mirrored) — and
the reduced-field path follows that same pattern.

### Why "don't spawn" needs the spawn-loop cap (point 3), not just `state=3`

In a normal (non-drag) race, `state==3` alone does **not** hide a slot: the body
renderer only skips actors with **no loaded mesh** (`RenderRaceActorForView`
`0x40BD26`, "absent AI" semantics). The spawn loop loads a mesh for every
`racer_count` slot. So to make a dropped opponent invisible you must reduce
`racer_count` — then its mesh is never loaded and the render skips it
(`mesh == NULL`).

---

## Related Quick Race menu changes (same screen, same commits)

- **Direction toggle** — reuses TrackSelection's reverse-data gating
  (`frontend_update_direction_button_visibility`); hidden on forward-only/circuit
  tracks; commits `g_td5.reverse_direction` at OK.
- **Drag Strip removed** — schedule index `19` skipped in the Quick Race track
  cycler (`frontend_quickrace_cycle_track`, which also skips absent levels).
- **Value column** — selected values render right of each caption at the
  caption glyph size and **wrap to a second line** if they'd run off-screen
  (`frontend_draw_qr_value`).
