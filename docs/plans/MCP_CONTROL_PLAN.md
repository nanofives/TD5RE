# TD5RE Live-Control MCP Server — Implementation Plan (APPROVED 2026-07-21, NOT EXECUTED)

> Hand-off: execute on **claude2 (account2)**. claude2 CANNOT run the game — verification is
> build-only (`build_all.bat` clean + lint ratchets green). Final deliverable includes
> `scripts/td5re_mcp/TESTING_HANDOFF.md`, a self-contained testing prompt for a claude3 session.
>
> Suggested claude2 kickoff prompt:
> "Read MCP_CONTROL_PLAN.md at the repo root and implement it in order (steps 1-6 under
> 'Implementation order'). You cannot launch the game — verify by building DEV+RELEASE via
> td5mod/src/td5re/build_all.bat and keeping the structure-lint ratchets green. Commit in
> per-step slices with path-scoped commits. Finish by writing scripts/td5re_mcp/TESTING_HANDOFF.md
> exactly as the plan's Verification section specifies."

## Context

Goal: an MCP server that lets a Claude session drive a **running** td5re.exe — launch a specific race (track / car / gamemode / laps / traffic / cops / AI opponents), change parameters, jump frontend screens, inject input, take screenshots, query state, and manage the game process itself. Purpose: fast interactive testing ("run a cop-chase on track 5 with 3 AI and show me") and enabling future automated QA loops.

Feasibility is confirmed — the game already contains a proven in-process control layer (the dev self-test director). **Everything the MCP needs verb-wise already exists; only the transport is missing.**

- `st_apply_scenario()` (`td5mod/src/td5re/td5_selftest.c:967`) starts a race by writing `g_td5.ini.*` fields (`default_track/default_car/default_game_type/laps/traffic/cops/dynamics/default_opponents/default_players/default_reverse/spectate_screens/player_is_ai/auto_throttle`) then `g_td5.ini.auto_race = 1`; next frame `td5_game_tick()` (`td5_game.c:1293`) calls `td5_frontend_auto_race_setup()` (`td5_frontend.c:3049`).
- `td5_game_selftest_end_race()` (`td5_game.c:1559`) aborts a race back to menu cleanly.
- `td5_frontend_set_screen()` / `td5_frontend_get_screen()` (`td5_frontend.c:4752/4886`) jump/read frontend screens.
- `td5_plat_input_inject_key()` (`td5_platform_win32.c:990`) injects DIK keys (works unfocused); `td5_inputscript.c`'s `s_held_bits` shows how to hold race-action bits.
- Threading: **all game calls are main-thread-only**. `td5_net.c` provides the pattern to copy: worker thread `recvfrom` → ring buffer → drained once per frame from `td5_game_tick()` (`td5_net.c:2285/3736/2502`). `td5_jobs` is NOT usable for marshaling (barrier fork/join only).
- Dev gating pattern: whole module in `#ifndef TD5RE_RELEASE`, header exposes no-op macro stubs in release (`td5_selftest.h` style), call sites unguarded.

## Architecture

```
Claude (MCP client, stdio)
   └── scripts/td5re_mcp/server.py        (Python, mcp SDK — like pending_viewer.py precedent)
         └── JSON-over-UDP, localhost:37060
               └── td5_control.c listener thread → command ring buffer
                     └── drained at top of td5_game_tick() (main thread) → selftest verbs
               ← JSON reply datagrams (ack / state snapshots)
```

Two deliverables:

### A. Game side — new module `td5mod/src/td5re/td5_control.c` + `td5_control.h`

Dev-only (`#ifndef TD5RE_RELEASE` + stub macros in header, selftest pattern). **Opt-in at runtime**: `[Control] Enabled=1` INI key / `--Control=1` CLI (default OFF) so a normal dev launch doesn't open a socket.

1. **Transport**: UDP socket bound to `127.0.0.1:37060` (`TD5RE_CONTROL_PORT` env override; register the port in `Proyectos/PORTS.md`). Dedicated listener thread modeled on `start_worker()`/`worker_thread_proc` (`td5_net.c:2285-2308`): `recvfrom` → parse JSON (vendored `cJSON.c` is already in the build) → push `{cmd, args, reply_addr}` into a mutex-guarded ring (16 slots). Replies are sent as JSON datagrams back to the sender's addr — sending from any thread is fine; only *game* calls are main-thread.
2. **Drain hook**: `td5_control_tick()` called at the top of `td5_game_tick()` (`td5_game.c:~1248`), right next to `td5_selftest_tick()`. Executes queued commands on the main thread, sends reply.
3. **Commands** (v1 protocol, one JSON object per datagram, `{"id":N,"cmd":"...","args":{...}}` → `{"id":N,"ok":true,...}`):
   - `ping` → version, build type, pid.
   - `get_state` → `game_state`, current screen index+name, race info when racing (track, mode, tick, per-racer position/lap/speed — read from `td5_race_state.h` query surface), paused flag.
   - `start_race {track, car, game_type, laps, opponents, traffic, cops, dynamics, reverse, players, spectate, player_is_ai, auto_throttle}` — all optional with sane defaults; refuse (error reply) unless `game_state==MENU` with frontend initialized (same guard as `td5_selftest.c:1479`); implementation = the `st_apply_scenario` field-write + `auto_race=1` recipe. If currently racing, caller must `end_race` first (or accept an `abort_current:true` arg that chains end_race → wait menu → launch; keep a tiny internal state machine like the selftest's for the wait).
   - `end_race` → `td5_game_selftest_end_race()`.
   - `set_screen {screen}` → `td5_frontend_set_screen()` (guard: MENU state).
   - `set_param {name, value}` / `get_param {name}` → **whitelist table** of `g_td5.ini` fields (name string → offset/type/min/max), not arbitrary poking. Seed the table with the scenario axes plus proven-live knobs the selftest flips (`trace_fast_forward`, `debug_overlay`, `car_damage`, `difficulty`, `lane_assist`, sound/music volume). Reply notes whether it applies immediately or at next race launch.
   - `inject_key {dik, down}` and `tap_key {dik}` → `td5_plat_input_inject_key()`; `hold_action {slot, action, frames}` → OR into a small held-bits array merged where inputscript merges its bits (`td5_input.c:839` neighborhood).
   - `quit` → clean shutdown via the normal WM_CLOSE path (so logs flush).
4. Init/shutdown wired in `main.c` beside `td5_selftest_boot()`; add module to `srcs.txt` (single-source build config — never touch per-build copies).

### B. MCP server — `scripts/td5re_mcp/` (Python, runs on this machine, no game rebuild to iterate)

`server.py` using the official `mcp` Python SDK (stdio transport), plus `game_client.py` (UDP request/reply with timeout+retry, matching ids). MCP tools exposed:

| Tool | Maps to |
|------|---------|
| `launch_game(args?)` | `subprocess.Popen` of `td5re.exe --Control=1 --SkipIntro=1 [+extra CLI overrides]` from repo root; store PID; wait for `ping` to succeed |
| `stop_game()` | try `quit` command, escalate to PID-scoped `taskkill /PID` (never /IM — parallel-safe invariant) |
| `game_status()` | process alive? + `ping`/`get_state` |
| `start_race(...)`, `end_race()`, `set_screen(name_or_index)` | control commands (screen names resolved from a bundled `TD5_ScreenIndex` map) |
| `get_state()` | state snapshot |
| `set_param(name, value)` / `list_params()` | whitelist |
| `press_key(key)`, `hold_action(action, frames, slot=0)` | input injection (friendly names → DIK codes) |
| `screenshot()` | by-PID capture reusing `tools/capture_window.ps1` logic (window must be visible — black-capture gotcha); returns image path/content |
| `read_log(which, tail_n)` | tail `log/frontend.log` / `race.log` / `engine.log` — cheap observability |

Registration snippet documented in `scripts/td5re_mcp/README.md`:
`{"td5re": {"command": "python", "args": ["scripts/td5re_mcp/server.py"]}}` (project-local `.mcp.json` in TD5RE root).

## Implementation order (for claude2)

1. `td5_control.h` + `td5_control.c`: socket/thread/ring + `ping`/`get_state` only; wire into `main.c`, `td5_game.c`, `srcs.txt`; INI key + CLI override; build both DEV and RELEASE (`build_all.bat`) — release must compile the module out cleanly.
2. Add `start_race` / `end_race` / `set_screen` (+ the small wait state machine for `abort_current`).
3. Add `set_param`/`get_param` whitelist, input injection, `quit`.
4. Python MCP server + game client + README with `.mcp.json` registration.
5. Register port 37060 in `Proyectos/PORTS.md`; add a line to TD5RE `CLAUDE.md` (MCP servers table) and to the runtime-logs/dev-harness section.
6. Update `pending_to_test.csv`/changelog per the /fix-end logging routine.

Constraints claude2 must respect:
- Never call game/frontend functions from the listener thread — only from `td5_control_tick()`.
- `start_race` only from MENU state; reply an error otherwise (don't crash-launch).
- Follow the extern ratchet: declarations go in producer headers, no new `extern` in .c files (`scripts/lint_structure.ps1` gates CI); avoid new `td5_game.h` includers — use `td5_race_state.h` for race queries.
- cJSON is already vendored — no new deps on the C side. Python side: only stdlib + `mcp` package.
- claude2 CANNOT run the game — build-only verification (`build_all.bat` clean, lint ratchets green), then hand off.

## Verification (claude3 hand-off — claude2 must write this file)

claude2's final deliverable includes `scripts/td5re_mcp/TESTING_HANDOFF.md`, a self-contained prompt for a claude3 session. Contents:

1. Register the MCP server (`.mcp.json` snippet) and restart the session so tools load, or drive `game_client.py` directly from Python if MCP registration is awkward.
2. Smoke: `launch_game()` → `game_status()` shows MENU; `ping` returns build info.
3. Race matrix: `start_race(track=5, opponents=3, cops=1)` → `get_state()` confirms RACE + correct track/mode → `screenshot()` → `end_race()` → state back to MENU. Repeat with `game_type` variants (circuit, TD6, drag, arcade) and `abort_current:true` mid-race relaunch.
4. `set_screen` walk over a few screens; `set_param('trace_fast_forward',1)` immediate-effect check; `press_key` nav in a menu.
5. Regression net: full self-test suite still passes (`pwsh scripts/selftest.ps1 -Suite full`, goldens must match — control module must not perturb the sim when idle), and RELEASE build runs with zero control surface (no socket opened).
6. Failure log → back into `pending_to_test.csv`.
