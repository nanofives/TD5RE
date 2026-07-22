"""_lib.py -- shared harness for TD5RE live-control test scenarios.

Each scenario is a standalone script (python scenarios/<name>.py) that drives
a DEV td5re.exe through the control socket (game_client.GameClient, stdlib
only -- no MCP dependency), asserts on get_state predicates, and exits 0
(PASS) or 1 (FAIL) after printing a one-line verdict + per-check detail.
Evidence (framedumps) lands in log/scenarios/.

Timing rule: NEVER assert on wall-clock sleeps -- poll state predicates with
wait_until (sim speed diverges from wall time under trace_fast_forward).
"""
from __future__ import annotations

import sys
import time
from pathlib import Path
from typing import Any, Callable, Dict, Optional

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))   # scripts/td5re_mcp

from game_client import GameClient, ControlError               # noqa: E402
import launcher                                                # noqa: E402

REPO_ROOT = launcher.REPO_ROOT
EVIDENCE_DIR = REPO_ROOT / "log" / "scenarios"

GAMETYPE_SINGLE_RACE = 0
GAMETYPE_TIME_TRIAL = 7
GAMETYPE_COP_CHASE = 8
GAMETYPE_DRAG_RACE = 9

STATE_MENU = 1
STATE_RACE = 2


class Scenario:
    """Launch/attach + check-accumulator + teardown for one scenario run."""

    def __init__(self, name: str, extra_args: str = ""):
        self.name = name
        self.checks: list[tuple[bool, str]] = []
        self.client = GameClient(timeout=2.0, retries=2)
        self.proc = None
        self._owns_proc = False
        if self.client.is_alive():
            print(f"[{name}] attached to already-running game")
        else:
            self.proc, info = launcher.launch(extra_args, client=self.client)
            if not info.get("ok"):
                print(f"[{name}] FAIL: launch failed: {info.get('error')}")
                sys.exit(1)
            self._owns_proc = True
            print(f"[{name}] launched pid={info.get('pid')}")

    # -- driving helpers ----------------------------------------------------
    def cmd(self, cmd: str, args: Optional[Dict[str, Any]] = None) -> Dict[str, Any]:
        return self.client.command(cmd, args)

    def state(self) -> Dict[str, Any]:
        try:
            return self.client.command("get_state")
        except ControlError:
            return {"ok": False, "game_state": -1}

    def wait_until(self, pred: Callable[[Dict[str, Any]], bool], timeout: float,
                   what: str, poll: float = 0.25) -> Optional[Dict[str, Any]]:
        """Poll get_state until pred(state) is truthy; None on timeout."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            st = self.state()
            try:
                if pred(st):
                    return st
            except (KeyError, TypeError, IndexError):
                pass
            time.sleep(poll)
        print(f"[{self.name}]   timeout after {timeout:.0f}s waiting for: {what}")
        return None

    def start_race_and_wait(self, timeout: float = 60.0, **fields) -> bool:
        """start_race + wait until RACE state with the countdown finished."""
        if not self.wait_until(lambda s: s.get("game_state") == STATE_MENU,
                               30, "MENU state"):
            return False
        res = self.cmd("start_race", fields or None)
        if not res.get("ok"):
            print(f"[{self.name}]   start_race refused: {res.get('error')}")
            return False
        # Wait for RACE state, dismissing the tutorial overlay along the way:
        # it re-arms EVERY race for human slots and freezes the countdown
        # until each human presses a key (ENTER counts for slot 0).
        # countdown=False alone is not enough either: at race entry there are
        # a few frames BEFORE the countdown activates where it reads false —
        # require the sim to be visibly past the launch (tick > 60) too.
        deadline = time.time() + timeout
        while time.time() < deadline:
            st = self.state()
            race = st.get("race", {})
            if st.get("game_state") == STATE_RACE:
                if race.get("tutorial"):
                    self.cmd("tap_key", {"dik": 0x1C})          # ENTER
                elif not race.get("countdown", True) and race.get("sim_tick", 0) > 60:
                    return True
            time.sleep(0.5)
        print(f"[{self.name}]   timeout after {timeout:.0f}s waiting for: "
              "race running (countdown over, sim ticking)")
        return False

    def racer(self, st: Dict[str, Any], slot: int) -> Dict[str, Any]:
        for r in st.get("race", {}).get("racers", []):
            if r.get("slot") == slot:
                return r
        return {}

    def framedump(self, tag: str) -> Optional[str]:
        """In-engine backbuffer PNG into log/scenarios/ (evidence)."""
        EVIDENCE_DIR.mkdir(parents=True, exist_ok=True)
        rel = f"log/scenarios/{self.name}_{tag}.png"
        out = REPO_ROOT / rel
        before = out.stat().st_mtime_ns if out.exists() else None
        try:
            res = self.cmd("framedump", {"path": rel})
        except ControlError as exc:
            print(f"[{self.name}]   framedump failed: {exc}")
            return None
        if not res.get("ok"):
            print(f"[{self.name}]   framedump refused: {res.get('error')}")
            return None
        deadline = time.time() + 3.0
        while time.time() < deadline:
            if out.exists() and out.stat().st_mtime_ns != before and out.stat().st_size > 0:
                return str(out)
            time.sleep(0.1)
        print(f"[{self.name}]   framedump acknowledged but no file appeared")
        return None

    # -- verdict ------------------------------------------------------------
    def check(self, cond: bool, label: str) -> bool:
        self.checks.append((bool(cond), label))
        print(f"[{self.name}]   {'ok  ' if cond else 'FAIL'} {label}")
        return bool(cond)

    def finish(self) -> None:
        """Tear down (quit the game if we launched it) and exit 0/1."""
        if self._owns_proc:
            launcher.stop(self.proc, client=self.client)
        self.client.close()
        failed = [label for ok, label in self.checks if not ok]
        total = len(self.checks)
        if failed:
            print(f"[{self.name}] FAIL ({total - len(failed)}/{total} checks): "
                  + "; ".join(failed))
            sys.exit(1)
        print(f"[{self.name}] PASS ({total}/{total} checks)")
        sys.exit(0)
