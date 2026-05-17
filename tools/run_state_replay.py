#!/usr/bin/env python3
"""
run_state_replay.py -- Orchestrator for the snapshot-replay harness.

Capture + diff workflow (Honolulu AI baseline, same scenario as
run_whole_state_diff.py):

  python tools/run_state_replay.py --capture-orig
    Spawn original/TD5_d3d.exe under Frida with tools/frida_state_snapshot.js.
    Captures every sub-tick (countdown + race) into
    tools/frida_csv/state_snapshot_original.bin.

  python tools/run_state_replay.py --capture-port-dump
    Launch td5re.exe in StateReplayMode=dump (no inject) to capture the
    port's natural sub-tick trajectory into log/port_state_snapshot.bin.

  python tools/run_state_replay.py --capture-port-both
    Launch td5re.exe in StateReplayMode=both (inject orig_frames at each
    tick AFTER dumping). This is the differential mode -- port_frames[N]
    is the port's result of running sub-tick N starting from orig[N-1].

  python tools/run_state_replay.py --diff
    Diff the two captured .bin files via tools/diff_replay_frames.py.

  python tools/run_state_replay.py --gate1
    Frame-0 no-inject parity: --capture-orig + --capture-port-dump + diff
    at sub_tick=0. Validates whether init alone is byte-exact.

  python tools/run_state_replay.py --gate3
    One-step gate: --capture-orig + --capture-port-both + per-sub-tick
    summary. Each diverging sub_tick = a function bug isolated to that
    sub-tick's transform.

Outputs:
  port:     log/port_state_snapshot.bin
  original: tools/frida_csv/state_snapshot_original.bin
"""
from __future__ import annotations

import argparse
import os
import subprocess
import sys
import time
from pathlib import Path

HERE       = Path(__file__).resolve().parent
REPO_ROOT  = HERE.parent
PORT_BIN   = REPO_ROOT / "log"        / "port_state_snapshot.bin"
ORIG_BIN   = HERE      / "frida_csv"  / "state_snapshot_original.bin"
DIFF_PY    = HERE      / "diff_replay_frames.py"
SNAP_JS    = HERE      / "frida_state_snapshot.js"
TD5RE_EXE  = REPO_ROOT / "td5re.exe"
TD5RE_INI  = REPO_ROOT / "td5re.ini"

# Honolulu AI baseline -- identical scenario between sides.
BASELINE_DUMP = {
    "Game.DefaultTrack": "8",
    "Game.DefaultCar":   "0",
    "Game.AutoRace":     "1",
    "Game.StartSpanOffset": "0",
    "GameOptions.PlayerIsAI": "1",
    "Trace.WholeState":  "0",          # don't fight the older whole-state path
    "Trace.RaceTrace":   "0",
    "Trace.AutoThrottle": "0",
    "Trace.RaceTraceMaxSimTicks": "20",
    "Trace.StateReplayMode": "dump",
    "Trace.StateReplayStartFrame": "0",
    "Trace.StateReplayEndFrame":   "0",
    "Trace.StateReplayMaxFrames":  "200",
    "Logging.Enabled":   "1",
}
BASELINE_BOTH = dict(BASELINE_DUMP, **{"Trace.StateReplayMode": "both"})


def ini_patch(ini_path: Path, overrides):
    """Apply [Section] Key=Val overrides in place. Returns originals."""
    text  = ini_path.read_text()
    lines = text.splitlines(keepends=True)
    by_sec = {}
    current = None
    for idx, line in enumerate(lines):
        s = line.strip()
        if s.startswith("[") and s.endswith("]"):
            current = s[1:-1]
            by_sec.setdefault(current, [])
        elif current is not None:
            by_sec[current].append(idx)

    originals = {}
    for key, new_val in overrides.items():
        section, ikey = key.split(".", 1)
        sec_lines = by_sec.get(section, [])
        replaced = False
        for idx in sec_lines:
            ln = lines[idx]
            s = ln.strip()
            if not s or s.startswith(";") or "=" not in s:
                continue
            lhs = s.split("=", 1)[0].strip()
            if lhs == ikey:
                originals[key] = s.split("=", 1)[1].strip()
                indent = ln[: len(ln) - len(ln.lstrip())]
                lines[idx] = f"{indent}{ikey} = {new_val}\n"
                replaced = True
                break
        if not replaced:
            if section in by_sec:
                insert_at = (max(by_sec[section]) + 1) if by_sec[section] else len(lines)
                lines.insert(insert_at, f"{ikey} = {new_val}\n")
                originals[key] = "__UNSET__"
            else:
                lines.append(f"\n[{section}]\n{ikey} = {new_val}\n")
                originals[key] = "__UNSET__"

    ini_path.write_text("".join(lines))
    return originals


def ini_restore(ini_path, originals):
    to_set = {k: v for k, v in originals.items() if v != "__UNSET__"}
    if to_set:
        ini_patch(ini_path, to_set)


def cmd_capture_orig(args):
    quickrace_py = REPO_ROOT / "re" / "tools" / "quickrace" / "td5_quickrace.py"
    if not quickrace_py.exists():
        sys.exit(f"td5_quickrace.py not found at {quickrace_py}")
    if not SNAP_JS.exists():
        sys.exit(f"snapshot script not found at {SNAP_JS}")
    ORIG_BIN.parent.mkdir(parents=True, exist_ok=True)
    if ORIG_BIN.exists():
        ORIG_BIN.unlink()

    cmd = [
        sys.executable, str(quickrace_py),
        "--track", "8",
        "--car",   "0",
        "--player-is-ai", "true",
        "--extra-script", str(SNAP_JS),
        "--trace-auto-exit",
        "--max-runtime", "60",
    ]
    print("[orig] " + " ".join(cmd))
    rc = subprocess.call(cmd, cwd=str(REPO_ROOT))
    print(f"[orig] td5_quickrace exited rc={rc}")
    if ORIG_BIN.exists():
        print(f"[orig] captured {ORIG_BIN} ({ORIG_BIN.stat().st_size} bytes)")
        return 0
    print("[orig] no snapshot produced")
    return 1


def _capture_port(baseline):
    PORT_BIN.parent.mkdir(parents=True, exist_ok=True)
    if PORT_BIN.exists():
        PORT_BIN.unlink()
    originals = ini_patch(TD5RE_INI, baseline)
    try:
        print(f"[port] launching {TD5RE_EXE}")
        t0 = time.time()
        rc = subprocess.call([str(TD5RE_EXE)], cwd=str(REPO_ROOT))
        print(f"[port] td5re.exe exited rc={rc} after {time.time() - t0:.1f}s")
    finally:
        ini_restore(TD5RE_INI, originals)
    if PORT_BIN.exists():
        print(f"[port] captured {PORT_BIN} ({PORT_BIN.stat().st_size} bytes)")
        return 0
    print("[port] no snapshot produced")
    return 1


def cmd_capture_port_dump(args):
    return _capture_port(BASELINE_DUMP)


def cmd_capture_port_both(args):
    if not ORIG_BIN.exists():
        sys.exit(f"orig snapshot missing at {ORIG_BIN}; run --capture-orig first.")
    return _capture_port(BASELINE_BOTH)


def cmd_diff(args, extra_args=None):
    missing = [p for p in (PORT_BIN, ORIG_BIN) if not p.exists()]
    if missing:
        for p in missing: print(f"missing: {p}")
        return 2
    cmd = [sys.executable, str(DIFF_PY), str(PORT_BIN), str(ORIG_BIN)]
    if extra_args:
        cmd.extend(extra_args)
    return subprocess.call(cmd)


def cmd_gate1(args):
    """Frame-0 no-inject parity.
    Tests whether port's natural state at end-of-sub-tick-0 matches orig."""
    print("==== GATE 1: Frame-0 no-inject parity ====")
    rc = cmd_capture_orig(args)
    if rc: return rc
    rc = cmd_capture_port_dump(args)
    if rc: return rc
    return cmd_diff(args, ["--sub", "0"])


def cmd_gate3(args):
    """One-step gate: inject orig[N-1] then run sub-tick N, diff vs orig[N]."""
    print("==== GATE 3: One-step gate (inject + dump differential) ====")
    if not ORIG_BIN.exists():
        rc = cmd_capture_orig(args)
        if rc: return rc
    rc = cmd_capture_port_both(args)
    if rc: return rc
    return cmd_diff(args)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[1])
    g = ap.add_mutually_exclusive_group()
    g.add_argument("--capture-orig",      action="store_true")
    g.add_argument("--capture-port-dump", action="store_true")
    g.add_argument("--capture-port-both", action="store_true")
    g.add_argument("--diff",              action="store_true")
    g.add_argument("--gate1",             action="store_true")
    g.add_argument("--gate3",             action="store_true")
    args = ap.parse_args(argv)

    if args.capture_orig:      return cmd_capture_orig(args)
    if args.capture_port_dump: return cmd_capture_port_dump(args)
    if args.capture_port_both: return cmd_capture_port_both(args)
    if args.gate1:             return cmd_gate1(args)
    if args.gate3:             return cmd_gate3(args)
    return cmd_diff(args)


if __name__ == "__main__":
    raise SystemExit(main())
