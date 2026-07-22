# td5re_mcp — live-control MCP server for TD5RE

An MCP server that drives a **running** `td5re.exe` (DEV build): launch/abort
races, jump/read frontend screens, get/set whitelisted INI knobs, inject
input, screenshot, and tail logs. Purpose: fast interactive testing
("run a cop-chase on track 5 with 3 AI and show me") and automated QA loops.

## How it works

```
Claude (MCP client, stdio)
  └── server.py            (FastMCP; this dir)
        └── game_client.py  (JSON-over-UDP, 127.0.0.1:37060, id-matched retry)
              └── td5_control.c in td5re.exe  (dev-only; listener thread -> ring
                    -> drained on the main thread at the top of td5_game_tick)
```

The game side (`td5mod/src/td5re/td5_control.c`) is **dev-only** and **opt-in**:
it opens the socket only when launched with `--Control=1` (or `[Control]
Enabled=1` in `td5re.ini`). It is compiled out entirely of the RELEASE build.
Port override: `TD5RE_CONTROL_PORT` env (both sides read it).

## Requirements

- A **DEV** `td5re.exe` at the repo root (`build_standalone.bat`). The RELEASE
  build has no control surface.
- Python 3.10+; `pip install mcp` for the MCP server. `game_client.py` and
  `td5re_maps.py` are stdlib-only and can be used without `mcp`.

## Register (project-local `.mcp.json` in the TD5RE repo root)

```json
{
  "mcpServers": {
    "td5re": { "command": "python", "args": ["scripts/td5re_mcp/server.py"] }
  }
}
```

Restart the Claude Code session so the tools load.

## Tools

| Tool | What it does |
|------|--------------|
| `launch_game(extra_args?, wait_seconds?)` | Popen `td5re.exe --Control=1 --SkipIntro=1 [extra]`, wait for `ping` |
| `stop_game()` | `quit` command; escalate to PID-scoped `taskkill /PID` (never `/IM`) |
| `game_status()` | process alive? + `get_state` |
| `get_state()` | game_state, screen, paused, race info (track/mode/sim_tick/actors) |
| `start_race(track?, car?, game_type?, laps?, opponents?, traffic?, cops?, dynamics?, reverse?, players?, spectate?, player_is_ai?, auto_throttle?, abort_current?)` | launch a race (MENU only, or `abort_current=True` mid-race) |
| `end_race()` | abort current race back to menu |
| `set_screen(screen)` | jump to a frontend screen by name or index (MENU only) |
| `get_param(name)` / `set_param(name,value)` / `list_params()` | whitelisted INI knobs |
| `press_key(key, down?)` / `tap_key(key)` / `hold_key(key, frames?)` | input injection (friendly names or DIK) |
| `screenshot(out_name?)` | capture the game window by PID (must be visible — headless returns black) |
| `read_log(which, tail_n?)` | tail `frontend`/`race`/`engine` log |

## Drive it directly (no MCP)

```python
import sys; sys.path.insert(0, "scripts/td5re_mcp")
from game_client import GameClient
c = GameClient()
print(c.ping())
print(c.command("start_race", {"track": 5, "opponents": 3, "cops": 1}))
print(c.get_state())
print(c.command("end_race"))
```

## Screen / key names

Friendly aliases live in `td5re_maps.py` (screens mirror `TD5_ScreenIndex` in
`td5_types.h`; keys are DIK scancodes). Unknown names fall back to raw ints, so
`set_screen(44)` and `press_key("0xC8")` both work.

## Protocol (v1)

Request:  `{"id":N,"cmd":"...","args":{...}}`
Reply:    `{"id":N,"ok":true,...}` or `{"id":N,"ok":false,"error":"..."}`

See the verb list and whitelist table in `td5_control.c`.
