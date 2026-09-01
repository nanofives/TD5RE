"""[R13 BAND] One instrumented run driven through the CONTROL SOCKET.

A fixed wall-clock wait cannot capture the start line: a cold track takes ~110 s
to generate and the loading slideshow runs for ~40 s more, so a short wait dumps
a menu frame and a long one dumps a car that has already driven away. This polls
get_state and dumps the frame the moment the race is up, at a pinned sim tick,
so the OFF and ON frames are the SAME POSE.

  python verify/r13band_frame.py --tag r13band-on --span 0 [--regen]
                                 [--knob NAME=VALUE ...] [--tick 2]
"""
import argparse
import os
import shutil
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "scripts", "td5re_mcp"))

PORT = "37083"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tag", required=True)
    ap.add_argument("--span", type=int, default=0)
    ap.add_argument("--seed", default="20260901")
    ap.add_argument("--tick", type=int, default=2)
    ap.add_argument("--regen", action="store_true")
    ap.add_argument("--knob", action="append", default=[])
    ap.add_argument("--boot", type=int, default=300)
    a = ap.parse_args()

    out = os.path.join(ROOT, "verify", "out")
    os.makedirs(out, exist_ok=True)
    png = os.path.join(out, a.tag + ".png")
    if os.path.exists(png):
        os.remove(png)

    env = {k: v for k, v in os.environ.items() if not k.startswith("TD5RE_")}
    env["TD5RE_CONTROL_PORT"] = PORT
    env["TD5RE_AUTOTRACK_SEED"] = a.seed
    env["TD5RE_WINDOW_TITLE"] = "R13BAND-" + a.tag
    env["TD5RE_D3D12_CAPTURE"] = "1"
    for kv in a.knob:
        k, _, v = kv.partition("=")
        env[k] = v

    lvl = os.path.join(ROOT, "re", "assets", "levels", "level090")
    if a.regen and os.path.isdir(lvl):
        shutil.rmtree(lvl)

    os.environ["TD5RE_CONTROL_PORT"] = PORT
    from game_client import GameClient, ControlError

    p = subprocess.Popen(
        [os.path.join(ROOT, "td5re.exe"), "--SkipIntro=1", "--AutoRace=1",
         "--DefaultTrack=60", "--Control=1",
         "--StartSpanOffset=%d" % a.span, "--PlayerIsAI=0", "--Opponents=0"],
        cwd=ROOT, env=env)
    print("PID=%d tag=%s span=%d" % (p.pid, a.tag, a.span))

    c = GameClient(port=int(PORT), timeout=1.0, retries=0)
    t0 = time.time()
    st = None
    while time.time() - t0 < a.boot:
        if p.poll() is not None:
            print("process exited early")
            return 1
        try:
            st = c.get_state()
        except ControlError:
            time.sleep(1.0)
            continue
        if st.get("game_state_name") == "RACE":
            race = st.get("race") or {}
            if race.get("sim_tick", 0) >= a.tick:
                break
        time.sleep(0.5)
    else:
        print("never reached RACE")
        p.terminate()
        return 1

    print("RACE at t=%.0fs tick=%s" % (time.time() - t0,
                                       (st.get("race") or {}).get("sim_tick")))
    c.command("framedump", {"path": "verify/out/%s.png" % a.tag})
    time.sleep(2.0)
    st = c.get_state()
    print("dump pose: tick=%s" % ((st.get("race") or {}).get("sim_tick")))
    try:
        c.command("quit")
    except ControlError:
        pass
    for _ in range(60):
        if p.poll() is not None:
            break
        time.sleep(0.5)
    if p.poll() is None:
        p.terminate()
    time.sleep(1.0)
    for name in ("race.log",):
        src = os.path.join(ROOT, "log", name)
        if os.path.exists(src):
            shutil.copy(src, os.path.join(out, "%s-%s" % (a.tag, name)))
    for name in ("MODELS.DAT", "STRIP.DAT"):
        src = os.path.join(lvl, name)
        if os.path.exists(src):
            shutil.copy(src, os.path.join(out, "%s-%s" % (a.tag, name)))
    print("frame -> %s (%s bytes)"
          % (png, os.path.getsize(png) if os.path.exists(png) else "NONE"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
