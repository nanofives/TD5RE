"""Repro harness for the autotrack mid-race RESTART device-lost TDR.

One trial: launch straight into a generated race, wait until it is actually
RACING and has moved, then issue start_race(abort_current=1) and watch for
either a clean second race or a device-lost death.

Usage: python repro_tdr.py <exe_dir> <label> <trial_no> <port>
Prints one RESULT line. Exit code 0 always (the caller reads the line).
"""
import os
import subprocess
import sys
import time

exe_dir, label, trial, port = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4])
sys.path.insert(0, os.path.join(exe_dir, "scripts", "td5re_mcp"))
from game_client import GameClient  # noqa: E402

out_path = os.path.join(exe_dir, "log", "_tdr_%s_%s.txt" % (label, trial))
env = dict(os.environ)
env["TD5RE_AUTOTRACK_SEED"] = "20260827"
env["TD5RE_CONTROL_PORT"] = str(port)
env["TD5RE_WINDOW_TITLE"] = "TDR_%s_%s" % (label, trial)

def read_out():
    try:
        with open(out_path, "r", errors="replace") as fh:
            return fh.read()
    except OSError:
        return ""

def dead_reason(txt):
    if "UNRECOVERABLE" in txt:
        return "TDR_UNRECOVERABLE"
    if "device lost" in txt:
        return "DEVICE_LOST_RECOVERING"
    return None

with open(out_path, "w") as fh:
    proc = subprocess.Popen(
        [os.path.join(exe_dir, "td5re.exe"),
         "--AutoRace=1", "--SkipIntro=1", "--DefaultTrack=60",
         "--PlayerIsAI=1", "--Control=1"] + os.environ.get("TDR_EXTRA_ARGS","").split(),
        cwd=exe_dir, env=env, stdout=fh, stderr=subprocess.STDOUT)

result = "UNKNOWN"
try:
    c = GameClient(port=port)

    # Phase 1: reach a moving race.
    span0 = None
    for _ in range(60):
        time.sleep(1)
        if proc.poll() is not None:
            result = "DIED_BEFORE_RESTART:%s" % (dead_reason(read_out()) or "exit%s" % proc.returncode)
            raise SystemExit
        try:
            s = c.get_state()
        except Exception:
            continue
        if s.get("game_state_name") == "RACE":
            rs = s.get("race", {}).get("racers", [])
            if rs and rs[0].get("span", 0) > 60:
                span0 = rs[0]["span"]
                break
    if span0 is None:
        result = "NEVER_RACED"
        raise SystemExit

    # Phase 2: the restart under test.
    try:
        c.command("start_race", {"track": 60, "player_is_ai": 1,
                                 "abort_current": True}, )
    except Exception as exc:
        result = "RESTART_CMD_FAILED:%s" % type(exc).__name__
        raise SystemExit

    # Phase 3: did it survive and race again?
    ok_ticks = 0
    for _ in range(75):
        time.sleep(1)
        if proc.poll() is not None:
            result = "TDR_ON_RESTART:%s" % (dead_reason(read_out()) or "exit%s" % proc.returncode)
            raise SystemExit
        txt = read_out()
        if "UNRECOVERABLE" in txt:
            result = "TDR_ON_RESTART:UNRECOVERABLE_LOGGED"
            raise SystemExit
        try:
            s = c.get_state()
        except Exception:
            continue
        if s.get("game_state_name") == "RACE":
            rs = s.get("race", {}).get("racers", [])
            if rs and rs[0].get("span", 0) > 40:
                ok_ticks += 1
                if ok_ticks >= 6:
                    result = "SURVIVED_RESTART"
                    raise SystemExit
except SystemExit:
    pass
finally:
    if proc.poll() is None:
        subprocess.run(["taskkill", "/PID", str(proc.pid)],
                       capture_output=True)
        for _ in range(10):
            time.sleep(1)
            if proc.poll() is not None:
                break
        if proc.poll() is None:
            subprocess.run(["taskkill", "/F", "/PID", str(proc.pid)],
                           capture_output=True)
    txt = read_out()
    dl = txt.count("device lost")
    print("RESULT label=%s trial=%s outcome=%s device_lost_lines=%d" %
          (label, trial, result, dl))
