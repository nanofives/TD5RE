"""launcher.py -- launch/stop a DEV td5re.exe with the control socket enabled.

Shared between server.py (MCP tools launch_game/stop_game) and the scenario
harness (scenarios/_lib.py) so both use the identical, parallel-safe process
lifecycle: PID-scoped taskkill only, never /IM.
Stdlib only.
"""
from __future__ import annotations

import subprocess
import time
from pathlib import Path
from typing import Any, Dict, Optional, Tuple

from game_client import GameClient, ControlError

REPO_ROOT = Path(__file__).resolve().parents[2]
GAME_EXE = REPO_ROOT / "td5re.exe"          # DEV build (has the control surface)


def launch(extra_args: str = "", wait_seconds: float = 20.0,
           client: Optional[GameClient] = None
           ) -> Tuple[Optional[subprocess.Popen], Dict[str, Any]]:
    """Start td5re.exe --Control=1 --SkipIntro=1 and wait for it to answer
    ping. Returns (proc, info): info carries ok/pid/ping or ok:false+error;
    proc is None only when the exe was missing."""
    if not GAME_EXE.exists():
        return None, {"ok": False,
                      "error": f"{GAME_EXE} not found (build the DEV target first)"}
    argv = [str(GAME_EXE), "--Control=1", "--SkipIntro=1"]
    if extra_args.strip():
        argv += extra_args.split()
    proc = subprocess.Popen(argv, cwd=str(REPO_ROOT))
    own_client = client is None
    c = client or GameClient()
    try:
        deadline = time.time() + wait_seconds
        while time.time() < deadline:
            if c.is_alive():
                return proc, {"ok": True, "pid": proc.pid, "ping": c.ping()}
            if proc.poll() is not None:
                return proc, {"ok": False,
                              "error": f"process exited early (code {proc.returncode})"}
            time.sleep(0.3)
        return proc, {"ok": False, "pid": proc.pid,
                      "error": "launched but control socket never answered ping"}
    finally:
        if own_client:
            c.close()


def stop(proc: Optional[subprocess.Popen],
         client: Optional[GameClient] = None) -> Dict[str, Any]:
    """Ask the game to quit cleanly (flushes logs); escalate to a PID-scoped
    taskkill only if it does not exit. Never uses /IM (parallel-safe)."""
    own_client = client is None
    c = client or GameClient()
    try:
        try:
            c.command("quit")
        except ControlError:
            pass                    # already dead / socket gone -> fall through
        if proc is None:
            return {"ok": True, "method": "quit", "note": "no tracked process handle"}
        try:
            proc.wait(timeout=8)
            return {"ok": True, "method": "quit", "exit_code": proc.returncode}
        except subprocess.TimeoutExpired:
            subprocess.run(["taskkill", "/PID", str(proc.pid), "/F"],
                           capture_output=True)
            return {"ok": True, "method": "taskkill", "pid": proc.pid}
    finally:
        if own_client:
            c.close()
