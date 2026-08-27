"""Detect the post-restart RENDER WEDGE without perturbing the GPU.

WHY THIS EXISTS. The in-place restart fault is a Heisenbug: every observer that
adds GPU synchronisation hides it.

  * arming framedump capture (TD5RE_D3D12_CAPTURE=1 + a dump every 4s) dropped
    the failure rate from 8/8 to 1/8, because each backbuffer readback drains
    the queued work that causes the stall;
  * TD5RE_RT_DIAG=1 hid it too -- rt_diag does fopen/fflush/fclose per line and
    thousands of lines throttle the loading path the same way.

So the tool that can SEE the freeze is the tool that prevents it. This detector
therefore issues NO framedump and enables NO diag. It reads only counters the
game already maintains, over the control socket:

    present_count   frames actually presented
    sim_tick        simulation ticks

The reported symptom is "stuck on the loading splash while race sounds play",
i.e. the SIM and audio advance while nothing new reaches the screen. That is
exactly sim_tick climbing while present_count does not, so the wedge is visible
in telemetry alone. Measured signature of the fault: a single Present blocking
4861 ms (engine.log PROFILE frame=4880.87 render=15.6 present=4861.0), which is
what trips the 2s GPU watchdog.

Outcomes:
  WEDGED_RENDER   sim advancing, present stalled >= STALL_S seconds
  TDR             device lost / unrecoverable in the log
  SIM_STALLED     present advancing, sim stuck
  BOTH_STALLED    neither advancing
  HEALTHY         both advancing

Usage: python detect_render_wedge.py <exe_dir> <label> <trial> <port>
"""
import os
import subprocess
import sys
import time

STALL_S = 3.0          # present flat this long, while sim moves, is a wedge
SAMPLE_S = 0.5
WATCH_S = 40.0

exe_dir, label, trial, port = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4])
sys.path.insert(0, os.path.join(exe_dir, "scripts", "td5re_mcp"))
from game_client import GameClient  # noqa: E402

log = os.path.join(exe_dir, "log")
out_path = os.path.join(log, "_wedge_%s_%s.txt" % (label, trial))

env = dict(os.environ)
env["TD5RE_AUTOTRACK_SEED"] = "20260827"
env["TD5RE_CONTROL_PORT"] = str(port)
env["TD5RE_WINDOW_TITLE"] = "WEDGE_%s_%s" % (label, trial)
# Deliberately NOT set: TD5RE_D3D12_CAPTURE, TD5RE_RT_DIAG. Both mask the fault.
env.pop("TD5RE_D3D12_CAPTURE", None)
env.pop("TD5RE_RT_DIAG", None)


def logtext():
    try:
        return open(out_path, errors="replace").read()
    except OSError:
        return ""


def sample(c):
    """(present_count, sim_tick) or (None, None) if unreachable."""
    try:
        s = c.get_state()
    except Exception:
        return None, None
    return s.get("present_count"), s.get("race", {}).get("sim_tick")


with open(out_path, "w") as fh:
    proc = subprocess.Popen(
        [os.path.join(exe_dir, "td5re.exe"), "--AutoRace=1", "--SkipIntro=1",
         "--DefaultTrack=60", "--PlayerIsAI=1", "--Control=1"],
        cwd=exe_dir, env=env, stdout=fh, stderr=subprocess.STDOUT)

outcome, detail = "UNKNOWN", ""
try:
    c = GameClient(port=port)
    for _ in range(70):
        time.sleep(1)
        if proc.poll() is not None:
            outcome = "DIED_BEFORE_RESTART"
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

    t0 = time.time()
    last_present, last_sim = None, None
    present_flat_since = None
    worst_stall = 0.0
    sim_moved_during_stall = False

    while time.time() - t0 < WATCH_S:
        time.sleep(SAMPLE_S)
        if proc.poll() is not None or "UNRECOVERABLE" in logtext():
            outcome = "TDR"
            detail = "worst_present_stall=%.1fs" % worst_stall
            raise SystemExit
        p, k = sample(c)
        if p is None:
            continue
        now = time.time()
        if last_present is not None:
            if p == last_present:
                if present_flat_since is None:
                    present_flat_since = now
                stall = now - present_flat_since
                if stall > worst_stall:
                    worst_stall = stall
                if last_sim is not None and k is not None and k != last_sim:
                    sim_moved_during_stall = True
            else:
                present_flat_since = None
        last_present, last_sim = p, k

    present_ok = worst_stall < STALL_S
    detail = "worst_present_stall=%.1fs sim_moved_during_stall=%s" % (
        worst_stall, sim_moved_during_stall)
    if not present_ok and sim_moved_during_stall:
        outcome = "WEDGED_RENDER"
    elif not present_ok:
        outcome = "BOTH_STALLED"
    else:
        outcome = "HEALTHY"
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
            subprocess.run(["taskkill", "/F", "/PID", str(proc.pid)],
                           capture_output=True)
    t = logtext()
    print("RESULT label=%s trial=%s outcome=%s %s device_lost=%d"
          % (label, trial, outcome, detail, t.count("device lost")))
