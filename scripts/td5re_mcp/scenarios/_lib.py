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

CRASH_EXIT = 3          # run_all.py maps this to a distinct CRASH status


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

    # -- crash detection ----------------------------------------------------
    def _engine_crash_line(self) -> Optional[str]:
        """Last 'CRASH:' line from THIS launch's engine.log (truncated per
        launch, so any CRASH line belongs to the current instance)."""
        log = REPO_ROOT / "log" / "engine.log"
        try:
            lines = log.read_text(errors="ignore").splitlines()
        except OSError:
            return None
        for ln in reversed(lines):
            if "CRASH:" in ln and "code=" in ln:
                return ln.strip()
        return None

    def _detect_crash(self) -> Optional[str]:
        """Crash summary if THIS instance crashed, else None. A CRASH line is
        definitive; an abnormal (non-zero) process exit corroborates it
        (TD5RE_NO_DIALOG makes a fault TerminateProcess with the AV code)."""
        line = self._engine_crash_line()
        if line:
            return line
        if self.proc is not None:
            rc = self.proc.poll()
            if rc is not None and rc != 0:
                return f"process exited abnormally (code 0x{rc & 0xffffffff:08X})"
        return None

    def _report_crash(self, during: str) -> None:
        """Report a CRASH (not a silent pass / bare traceback) and snapshot the
        forensics BEFORE the next launch rotates crash.log away, then exit
        CRASH_EXIT so run_all classifies it distinctly."""
        summary = self._detect_crash() or "unknown"
        print(f"[{self.name}] CRASH during '{during}': {summary}")
        EVIDENCE_DIR.mkdir(parents=True, exist_ok=True)
        ts = time.strftime("%Y%m%d_%H%M%S")
        for src_rel in ("log/crash.log", "log/engine.log"):
            src = REPO_ROOT / src_rel
            if not src.exists():
                continue
            try:
                data = src.read_text(errors="ignore")
                if src.name == "engine.log":        # keep only the tail
                    data = "\n".join(data.splitlines()[-200:])
                dst = EVIDENCE_DIR / f"{self.name}_crash_{ts}_{src.name}"
                dst.write_text(data)
                print(f"[{self.name}]   saved {dst}")
            except OSError:
                pass
        try:                                        # free the port (PID-scoped)
            if self.proc is not None and self.proc.poll() is None:
                self.proc.kill()
        except Exception:
            pass
        self.client.close()
        sys.exit(CRASH_EXIT)

    # -- driving helpers ----------------------------------------------------
    def cmd(self, cmd: str, args: Optional[Dict[str, Any]] = None) -> Dict[str, Any]:
        try:
            return self.client.command(cmd, args)
        except ControlError:
            if self._detect_crash():
                self._report_crash(cmd)
            raise

    def end_race_best_effort(self, menu_timeout: float = 45.0) -> None:
        """Cleanup teardown that never turns a lost socket into a traceback.

        After a NATURAL finish the game may already be leaving RACE (results/
        fade) or, on the known-flaky GPU-TDR path, the process may have died —
        in which case end_race's reply never comes. Swallow ControlError and
        only bother waiting for MENU if the socket is still answering."""
        try:
            self.cmd("end_race")
        except ControlError:
            return
        self.wait_until(lambda x: x.get("game_state") == STATE_MENU,
                        menu_timeout, "back at MENU")

    def state(self) -> Dict[str, Any]:
        try:
            return self.client.command("get_state")
        except ControlError:
            # A crash surfaces here first (wait_until polls state); report it
            # promptly instead of silently timing out as a FAIL.
            if self._detect_crash():
                self._report_crash("get_state")
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
