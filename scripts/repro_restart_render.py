"""Hunt the post-restart 'renderer stuck on the loading splash' symptom.

Classifies on FRAME CONTENT, not process liveness -- the flaw in v1. A run where
the sim ticks while the same image is re-presented is exactly the reported bug,
and v1 scored that as SURVIVED.

Outcomes:
  TDR                 device lost / unrecoverable
  RENDER_FROZEN       sim_tick advancing but every framedump hash IDENTICAL
  PRESENT_STALLED     present_count not advancing
  SIM_DEAD            frames change but sim_tick stuck
  SURVIVED            frames change and sim advances

Usage: python hunt_splash.py <exe_dir> <label> <trial> <port>
"""
import hashlib
import os
import subprocess
import sys
import time

exe_dir, label, trial, port = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4])
sys.path.insert(0, os.path.join(exe_dir, "scripts", "td5re_mcp"))
from game_client import GameClient  # noqa: E402

log = os.path.join(exe_dir, "log")
out_path = os.path.join(log, "_hunt_%s_%s.txt" % (label, trial))
dump_name = "_hunt_%s_%s.png" % (label, trial)
dump_path = os.path.join(log, dump_name)

env = dict(os.environ)
env["TD5RE_AUTOTRACK_SEED"] = "20260827"
env["TD5RE_CONTROL_PORT"] = str(port)
env["TD5RE_D3D12_CAPTURE"] = "1"
env["TD5RE_WINDOW_TITLE"] = "HUNT_%s_%s" % (label, trial)


def txt():
    try:
        return open(out_path, errors="replace").read()
    except OSError:
        return ""


def grab(c):
    """One framedump -> sha256, or None."""
    before = os.path.getmtime(dump_path) if os.path.exists(dump_path) else None
    try:
        r = c.command("framedump", {"path": "log/%s" % dump_name})
    except Exception:
        return None
    if not r.get("ok"):
        return None
    for _ in range(50):
        time.sleep(0.2)
        if not os.path.exists(dump_path):
            continue
        if before is not None and os.path.getmtime(dump_path) == before:
            continue
        s1 = os.path.getsize(dump_path)
        if s1 == 0:
            continue
        time.sleep(0.25)
        if os.path.getsize(dump_path) == s1:
            return hashlib.sha256(open(dump_path, "rb").read()).hexdigest()
    return None


with open(out_path, "w") as fh:
    proc = subprocess.Popen(
        [os.path.join(exe_dir, "td5re.exe"), "--AutoRace=1", "--SkipIntro=1",
         "--DefaultTrack=60", "--PlayerIsAI=1", "--Control=1"]
        + os.environ.get("HUNT_EXTRA_ARGS", "").split(),
        cwd=exe_dir, env=env, stdout=fh, stderr=subprocess.STDOUT)

outcome = "UNKNOWN"
detail = ""
try:
    c = GameClient(port=port)
    for _ in range(70):
        time.sleep(1)
        if proc.poll() is not None:
            outcome, detail = "DIED_BEFORE_RESTART", "exit%s" % proc.returncode
            raise SystemExit
        try:
            s = c.get_state()
        except Exception:
            continue
        if s.get("game_state_name") == "RACE":
            rs = s.get("race", {}).get("racers", [])
            if rs and rs[0].get("span", 0) > 60:
                break
    else:
        outcome = "NEVER_RACED"
        raise SystemExit

    c.command("start_race", {"track": 60, "player_is_ai": 1, "abort_current": True})

    # Sample frames + sim across the post-restart window.
    hashes, ticks, presents = [], [], []
    for _ in range(7):
        time.sleep(4)
        if proc.poll() is not None or "UNRECOVERABLE" in txt():
            outcome, detail = "TDR", "unrecoverable"
            raise SystemExit
        try:
            s = c.get_state()
            ticks.append(s.get("race", {}).get("sim_tick"))
            presents.append(s.get("present_count"))
        except Exception:
            ticks.append(None)
            presents.append(None)
        h = grab(c)
        if h:
            hashes.append(h)

    uniq_frames = len(set(hashes))
    tv = [t for t in ticks if isinstance(t, int)]
    pv = [p for p in presents if isinstance(p, int)]
    sim_moved = len(set(tv)) > 1 and (max(tv) - min(tv)) > 30
    present_moved = len(set(pv)) > 1

    detail = "frames=%d uniq=%d ticks=%s present_delta=%s" % (
        len(hashes), uniq_frames,
        (max(tv) - min(tv)) if tv else "na",
        (max(pv) - min(pv)) if pv else "na")

    if not present_moved:
        outcome = "PRESENT_STALLED"
    elif uniq_frames <= 1 and len(hashes) >= 3:
        outcome = "RENDER_FROZEN" if sim_moved else "FULLY_FROZEN"
    elif not sim_moved:
        outcome = "SIM_DEAD"
    else:
        outcome = "SURVIVED"
except SystemExit:
    pass
finally:
    if proc.poll() is None:
        subprocess.run(["taskkill", "/PID", str(proc.pid)], capture_output=True)
        for _ in range(8):
            time.sleep(1)
            if proc.poll() is not None:
                break
        if proc.poll() is None:
            subprocess.run(["taskkill", "/F", "/PID", str(proc.pid)], capture_output=True)
    t = txt()
    rq = "quality_high=?"
    for line in t.splitlines():
        if "quality_high" in line:
            rq = line.strip()[-40:]
            break
    print("RESULT label=%s trial=%s outcome=%s %s device_lost=%d"
          % (label, trial, outcome, detail, t.count("device lost")))
