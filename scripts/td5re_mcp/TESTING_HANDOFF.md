# TD5RE live-control MCP — testing hand-off (run on claude3)

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

## Known v1 limitations (candidate follow-ups, not bugs)

- `get_state` race info is track/mode/sim_tick/actor-count/player-slot only —
  no per-racer position/lap/speed yet (would need actor-field plumbing that
  stays within the `td5_race_state.h` read-only surface).
- `hold_key`/`tap_key` drive keyboard DIK scancodes (auto-released after N
  frames); there is no per-slot race-action-bit injection for split-screen
  players yet.
- `screenshot()` inherits the desktop-capture "black frame if not visible"
  gotcha (see `reference_screenshot_capture_black`); prefer the in-engine
  `TD5RE_FRAMEDUMP=<path>` backbuffer dump for reliable full-color frames.
