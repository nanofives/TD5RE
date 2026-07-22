# TD5RE live-control MCP — testing hand-off (run on claude3)

> **Superseded for day-to-day use** by [TESTING_WORKFLOW.md](TESTING_WORKFLOW.md)
> (the durable two-account testing loop + scenario library). This file remains
> as the one-shot v1 bring-up checklist.

This is a self-contained prompt for a **claude3** session (an account that
**can launch the game**). The implementation is done and builds clean on
claude2 (DEV + RELEASE, structure-lint ratchets unchanged) but was **never run**
— claude2 cannot launch td5re.exe. Your job is to exercise it end-to-end.

Prereqs: a **DEV** `td5re.exe` at the repo root (`cd td5mod\src\td5re &&
build_standalone.bat`). The RELEASE build has no control surface by design.

> Parallel-safety: if other td5re.exe windows may be up, keep this the only one
> using port 37060 (or set `TD5RE_CONTROL_PORT` on both the game and the client),
> and only ever kill by **PID** (`stop_game()` does this) — never `/IM`.

## 1. Register the server (or drive the client directly)

Add to the TD5RE repo-root `.mcp.json` (merge — don't clobber the existing
`ghidra`/`windbg` entries):

```json
"td5re": { "command": "python", "args": ["scripts/td5re_mcp/server.py"] }
```

Then `pip install mcp` and restart the session so the tools load. **If MCP
registration is awkward**, drive `game_client.py` directly instead:

```python
import sys; sys.path.insert(0, "scripts/td5re_mcp")
from game_client import GameClient
c = GameClient()
```

## 2. Smoke

- `launch_game()` → expect `ok:true` with a pid and a `ping` reply
  (`proto:1`, `build:"dev"`).
- `game_status()` / `get_state()` → `game_state_name` should reach `MENU`
  (screen 5 = main_menu) within a few seconds of boot.

## 3. Race matrix

For each case: `get_state()` should show `game_state_name:"RACE"` with the
expected `race.track` / `race.game_type`, then `screenshot()` (window must be
visible), then `end_race()` → state back to `MENU`.

- `start_race(track=5, opponents=3, cops=1)`
- `game_type` variants: circuit, a TD6 track (index ≥ 26), drag, arcade — set
  `game_type=` / `track=` per the frontend indices you use elsewhere.
- Mid-race relaunch: while racing, `start_race(track=2, abort_current=True)` →
  reply `pending:true`; poll `get_state()` until it is racing on track 2 again.

## 4. Screens / params / input

- `set_screen("track_selection")`, `set_screen("options_hub")`,
  `set_screen("high_score")` → `get_state().screen` matches each (MENU only).
- `list_params()` then `set_param("trace_fast_forward", 4)` → `applies` reports
  `next_race`; `get_param("debug_overlay")`, `set_param("debug_overlay", 1)`
  → `immediate`.
- In a menu, `tap_key("down")` / `tap_key("up")` / `tap_key("enter")` move the
  cursor; confirm via screenshot or `read_log("frontend")`.
- In a race, `hold_key("up", 30)` should drive (if keyboard throttle is bound).

## 5. Regression net (MUST pass)

- Full self-test suite still green and **goldens byte-identical** — the control
  module must not perturb the sim when idle:
  `pwsh scripts/selftest.ps1 -Suite full` (exit code 0; golden 'G' rows PASS).
  Run this with the control socket OFF (no `--Control=1`) — that is the true
  "does the added module change anything?" check.
- RELEASE build has **zero** control surface: launch `td5re_release.exe
  --Control=1` and confirm **no** socket opens on 37060 (e.g. a `GameClient().ping()`
  times out) and the flag is silently ignored.

## 6. Log failures back into pending_to_test.csv

Anything that misbehaves → add a row to `td5mod/src/td5re/pending_to_test.csv`
(and note it in the changelog) per the /fix-end logging routine, so the next
session picks it up.

## Known v1 limitations — ALL CLOSED by v2 (2026-07-21)

- ~~`get_state` race info minimal~~ → now carries `racers[]` (per-slot
  position/lap/speed/damage/cop-role/arcade fields), `num_racers`,
  `countdown`, `tutorial`, `wanted_mode`, `cop_actor`, `victory_position`.
- ~~no per-slot race-action injection~~ → `hold_action`/`release_action`
  verbs (same OR-overlay contract as td5_inputscript).
- ~~GDI screenshot black-frame gotcha~~ → `framedump` verb /
  `screenshot_game()` MCP tool dump the backbuffer in-engine.

## Driving gotchas (learned the hard way)

- The **tutorial overlay re-arms EVERY race** with human slots and freezes
  the countdown until each human presses a key. Poll `race.tutorial` and
  `tap_key ENTER` to dismiss (scenarios/_lib.py does this automatically).
- `race.countdown` reads false for a few frames at race entry BEFORE the
  countdown arms — wait for `sim_tick > 60` too, not countdown alone.
- The main thread doesn't drain commands during the blocking track load —
  expect several seconds of reply timeouts after `start_race`; keep polling.
- `dynamics=0` is ARCADE, `1` is SIMULATION (td5_arcade.c gates on ==0).
- `is_cop`/`is_suspect` are MP role queries (need a cop SLOT); SP wanted
  mode reports `cop_slot=-1` so they stay false — use `wanted_mode` +
  `cop_actor` there.
- `sfx_volume`/`music_volume` set via the whitelist are runtime-only: the
  SAVE layer re-applies its own copy at race transitions. For a persistence
  probe use `laps` (written to [GameOptions] on clean quit).
