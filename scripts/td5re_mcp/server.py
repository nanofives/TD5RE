"""server.py -- MCP server that drives a running td5re.exe.

Exposes the TD5RE live-control verbs (see td5_control.c) as MCP tools over
stdio. Uses the official `mcp` Python SDK (FastMCP). The game side must be a
DEV build launched with --Control=1 (this server's launch_game does that).

Register (project-local .mcp.json in the TD5RE repo root):

    {
      "mcpServers": {
        "td5re": { "command": "python",
                   "args": ["scripts/td5re_mcp/server.py"] }
      }
    }

Requires: `pip install mcp` (stdlib otherwise). If MCP registration is
awkward, drive game_client.GameClient directly from a Python REPL instead --
every tool below is a thin wrapper over it.
"""
from __future__ import annotations

import os
import subprocess
import time
from pathlib import Path
from typing import Any, Dict, Optional

from mcp.server.fastmcp import FastMCP

from game_client import GameClient, ControlError
import launcher
import td5re_maps as maps

REPO_ROOT = launcher.REPO_ROOT
GAME_EXE = launcher.GAME_EXE                # DEV build (has the control surface)
CAPTURE_PS1 = REPO_ROOT / "tools" / "capture_window.ps1"
LOG_DIR = REPO_ROOT / "log"

mcp = FastMCP("td5re")

# --- module state ---------------------------------------------------------
_client: Optional[GameClient] = None
_proc: Optional[subprocess.Popen] = None


def _get_client() -> GameClient:
    global _client
    if _client is None:
        _client = GameClient()
    return _client


def _cmd(cmd: str, args: Optional[Dict[str, Any]] = None) -> Dict[str, Any]:
    try:
        return _get_client().command(cmd, args)
    except ControlError as exc:
        return {"ok": False, "error": str(exc)}


# --- process lifecycle ----------------------------------------------------
@mcp.tool()
def launch_game(extra_args: str = "", wait_seconds: float = 20.0) -> Dict[str, Any]:
    """Launch the DEV td5re.exe with the control socket enabled and wait for it
    to answer ping. `extra_args` is appended verbatim (e.g. "--DefaultTrack=5").
    """
    global _proc
    _proc, info = launcher.launch(extra_args, wait_seconds, client=_get_client())
    return info


@mcp.tool()
def stop_game() -> Dict[str, Any]:
    """Ask the game to quit cleanly (flushes logs); escalate to a PID-scoped
    taskkill only if it does not exit. Never uses /IM (parallel-safe)."""
    global _proc
    info = launcher.stop(_proc, client=_get_client())
    _proc = None
    return info


@mcp.tool()
def game_status() -> Dict[str, Any]:
    """Is the tracked process alive, and does the control socket answer?"""
    alive_proc = _proc is not None and _proc.poll() is None
    state: Dict[str, Any]
    try:
        state = _get_client().get_state()
    except ControlError as exc:
        state = {"ok": False, "error": str(exc)}
    return {"process_alive": alive_proc,
            "pid": (_proc.pid if _proc else None),
            "state": state}


# --- game control ---------------------------------------------------------
@mcp.tool()
def get_state(racers: bool = True) -> Dict[str, Any]:
    """Current game_state, screen, paused flag, and race info when racing.

    While racing the reply's `race` object carries countdown/wanted_mode/
    cop_actor/arcade_active/victory_position plus (unless racers=False) a
    `racers` array: per-slot position, lap, speed, finished/finish_position,
    damage_health/damage_accum (once damage init'd), is_cop/is_suspect
    (cop-chase only) and arcade_effect/arcade_frames (arcade only)."""
    return _cmd("get_state", {"racers": racers})


@mcp.tool()
def start_race(track: Optional[int] = None, car: Optional[int] = None,
               game_type: Optional[int] = None, laps: Optional[int] = None,
               opponents: Optional[int] = None, traffic: Optional[int] = None,
               cops: Optional[int] = None, dynamics: Optional[int] = None,
               reverse: Optional[int] = None, players: Optional[int] = None,
               spectate: Optional[int] = None, player_is_ai: Optional[int] = None,
               auto_throttle: Optional[int] = None,
               abort_current: bool = False) -> Dict[str, Any]:
    """Launch a race. All fields optional (unset keeps the current INI value).
    Requires MENU state unless abort_current=True (then it ends the current
    race and launches once back at the menu). auto_throttle defaults ON."""
    args: Dict[str, Any] = {}
    for k, v in dict(track=track, car=car, game_type=game_type, laps=laps,
                     opponents=opponents, traffic=traffic, cops=cops,
                     dynamics=dynamics, reverse=reverse, players=players,
                     spectate=spectate, player_is_ai=player_is_ai,
                     auto_throttle=auto_throttle).items():
        if v is not None:
            args[k] = v
    if abort_current:
        args["abort_current"] = True
    return _cmd("start_race", args)


@mcp.tool()
def end_race() -> Dict[str, Any]:
    """Abort the current race cleanly back to the menu."""
    return _cmd("end_race")


@mcp.tool()
def set_screen(screen: str) -> Dict[str, Any]:
    """Jump to a frontend screen by friendly name or index (MENU state only)."""
    try:
        idx = maps.resolve_screen(screen)
    except KeyError as exc:
        return {"ok": False, "error": str(exc)}
    return _cmd("set_screen", {"screen": idx})


@mcp.tool()
def get_param(name: str) -> Dict[str, Any]:
    """Read a whitelisted INI knob (see list_params)."""
    return _cmd("get_param", {"name": name})


@mcp.tool()
def set_param(name: str, value: float) -> Dict[str, Any]:
    """Set a whitelisted INI knob; reply notes immediate vs next-race effect."""
    return _cmd("set_param", {"name": name, "value": value})


@mcp.tool()
def list_params() -> Dict[str, Any]:
    """List the whitelisted INI knob names."""
    return _cmd("list_params")


@mcp.tool()
def press_key(key: str, down: bool = True) -> Dict[str, Any]:
    """Inject a raw key down/up (friendly name or DIK scancode)."""
    try:
        dik = maps.resolve_key(key)
    except KeyError as exc:
        return {"ok": False, "error": str(exc)}
    return _cmd("inject_key", {"dik": dik, "down": down})


@mcp.tool()
def tap_key(key: str) -> Dict[str, Any]:
    """Tap a key (short auto-released press) -- e.g. menu nav."""
    try:
        dik = maps.resolve_key(key)
    except KeyError as exc:
        return {"ok": False, "error": str(exc)}
    return _cmd("tap_key", {"dik": dik})


@mcp.tool()
def hold_key(key: str, frames: int = 10) -> Dict[str, Any]:
    """Hold a key for N game frames, then auto-release (e.g. accelerate)."""
    try:
        dik = maps.resolve_key(key)
    except KeyError as exc:
        return {"ok": False, "error": str(exc)}
    return _cmd("hold_key", {"dik": dik, "frames": frames})


@mcp.tool()
def hold_action(action: str, slot: int = 0, frames: int = 60) -> Dict[str, Any]:
    """Hold a race-action bit on a racer slot (throttle|brake|handbrake|horn|
    gearup|geardown|camera|rearview|left|right|pause|escape). Flows through
    the same downstream paths as real input, works per split-screen slot and
    with the window unfocused. frames=0 holds until release_action; otherwise
    auto-releases after N game frames (max 600)."""
    try:
        act = maps.resolve_action(action)
    except KeyError as exc:
        return {"ok": False, "error": str(exc)}
    return _cmd("hold_action", {"slot": slot, "action": act, "frames": frames})


@mcp.tool()
def release_action(slot: int = 0, action: Optional[str] = None) -> Dict[str, Any]:
    """Release a held race action on a slot; omit `action` to release all."""
    args: Dict[str, Any] = {"slot": slot}
    if action is not None:
        try:
            args["action"] = maps.resolve_action(action)
        except KeyError as exc:
            return {"ok": False, "error": str(exc)}
    return _cmd("release_action", args)


# --- observability --------------------------------------------------------
@mcp.tool()
def screenshot_game(out_name: str = "ctrl_frame.png",
                    wait_seconds: float = 2.0) -> Dict[str, Any]:
    """In-engine backbuffer PNG via the `framedump` control verb — full-color
    even when the window is occluded or headless (PREFER this over
    `screenshot`). Waits for the file to appear/stabilize (dump happens at the
    game's next present)."""
    LOG_DIR.mkdir(exist_ok=True)
    out_path = LOG_DIR / out_name
    before = out_path.stat().st_mtime_ns if out_path.exists() else None
    res = _cmd("framedump", {"path": f"log/{out_name}"})
    if not res.get("ok"):
        return res
    deadline = time.time() + wait_seconds
    last_size = -1
    while time.time() < deadline:
        if out_path.exists() and out_path.stat().st_mtime_ns != before:
            size = out_path.stat().st_size
            if size > 0 and size == last_size:      # size stable -> fully written
                return {"ok": True, "path": str(out_path), "bytes": size}
            last_size = size
        time.sleep(0.1)
    if out_path.exists() and out_path.stat().st_mtime_ns != before:
        return {"ok": True, "path": str(out_path), "bytes": out_path.stat().st_size,
                "note": "written but size-stability window expired"}
    return {"ok": False, "error": "framedump acknowledged but no file appeared "
            "(is the game presenting frames?)"}


@mcp.tool()
def screenshot(out_name: str = "td5re_shot.png") -> Dict[str, Any]:
    """Screenshot the game window by PID via tools/capture_window.ps1.
    NOTE: the window must be visible (not occluded / not headless) -- an
    off-screen or headless run returns a black frame (known gotcha)."""
    if _proc is None:
        return {"ok": False, "error": "no tracked process (launch_game first)"}
    out_path = (LOG_DIR / out_name)
    LOG_DIR.mkdir(exist_ok=True)
    res = subprocess.run(
        ["pwsh", "-NoProfile", "-File", str(CAPTURE_PS1),
         "-ProcessId", str(_proc.pid), "-Out", str(out_path)],
        capture_output=True, text=True)
    ok = res.returncode == 0 and out_path.exists()
    return {"ok": ok, "path": str(out_path) if ok else None,
            "stdout": res.stdout.strip(), "stderr": res.stderr.strip()}


@mcp.tool()
def read_log(which: str = "frontend", tail_n: int = 40) -> Dict[str, Any]:
    """Tail one of the runtime logs: frontend | race | engine."""
    names = {"frontend": "frontend.log", "race": "race.log", "engine": "engine.log"}
    fn = names.get(which)
    if not fn:
        return {"ok": False, "error": f"unknown log '{which}' (frontend|race|engine)"}
    path = LOG_DIR / fn
    if not path.exists():
        return {"ok": False, "error": f"{path} not found"}
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        lines = f.readlines()
    return {"ok": True, "path": str(path), "lines": [l.rstrip("\n") for l in lines[-tail_n:]]}


if __name__ == "__main__":
    mcp.run()
