"""[GEO LAND] One in-race framedump of a named span on a geo track.

Launches the WORKTREE's own td5re.exe with RT off and the 16 minimum-graphics
flags (the standing run rule, plan section 7 Phase 0), parks the car at
--StartSpanOffset, waits on the live-control socket until get_state says RACE
and only then asks for the dump -- a fixed sleep lands on the loading screen,
because a geo track takes tens of seconds to build.

The window is pushed to the BOTTOM of the z-order as soon as it appears, so a
capture never steals focus. Only the PID it started is killed.

  python land_frame.py --tag gabled --span 200 --place land_test
                       [--port 37193] [--topdown 9000] [--backdiv 1]
                       [--reuse] [--out re/tools/geo_fixtures/out/x.png]
                       [--knob K=V]...

--reuse keeps an already-generated re/assets/levels/level090 instead of
rebuilding it, which is how a BEFORE/AFTER pair is captured from one exe: drop
the old level090 in place and dump it again.
"""
import argparse
import ctypes
import json
import os
import shutil
import socket
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))

MIN_GFX = ["--Lighting=0", "--Quality=0", "--SunShadows=0", "--Reflections=0",
           "--WetRoads=0", "--StreetLights=0", "--CarLights=0",
           "--LegacyShadows=0", "--GIQuality=0", "--ShadowRays=0",
           "--ReflectionQuality=0", "--CarShadows=0", "--VFX=0",
           "--WorldBillboards=0", "--FoliageAA=0", "--RenderScale=50"]


def to_back(pid):
    try:
        user32 = ctypes.windll.user32
        found = []

        @ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)
        def cb(hwnd, _l):
            p = ctypes.c_ulong()
            user32.GetWindowThreadProcessId(hwnd, ctypes.byref(p))
            if p.value == pid and user32.IsWindowVisible(hwnd):
                found.append(hwnd)
            return True
        user32.EnumWindows(cb, None)
        for h in found:
            user32.SetWindowPos(h, ctypes.c_void_p(1), 0, 0, 0, 0, 0x0013)
    except Exception:
        pass


def cmd(sock, port, name, args=None):
    msg = json.dumps({"cmd": name, "args": args or {}}).encode()
    for _ in range(3):
        try:
            sock.sendto(msg, ("127.0.0.1", port))
            return json.loads(sock.recvfrom(65535)[0].decode())
        except Exception:
            continue
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tag", required=True)
    ap.add_argument("--span", type=int, default=0)
    ap.add_argument("--place", default="la_plata")
    ap.add_argument("--port", type=int, default=37193)
    ap.add_argument("--topdown", type=float, default=0.0)
    ap.add_argument("--backdiv", type=float, default=0.0)
    ap.add_argument("--exe", default="td5re.exe")
    ap.add_argument("--seed", default="20260901")
    ap.add_argument("--reuse", action="store_true")
    ap.add_argument("--timeout", type=float, default=420.0)
    ap.add_argument("--tick", type=int, default=90,
                    help="sim tick the dump fires on (same moment for A and B)")
    ap.add_argument("--out", default="")
    ap.add_argument("--knob", action="append", default=[])
    a = ap.parse_args()

    outdir = os.path.join(HERE, "out")
    os.makedirs(outdir, exist_ok=True)
    png = a.out or os.path.join(outdir, "%s.png" % a.tag)
    if os.path.exists(png):
        os.remove(png)

    env = {k: v for k, v in os.environ.items() if not k.startswith("TD5RE_")}
    env["TD5RE_RT"] = "0"
    # The dump is driven by TD5RE_FRAMEDUMP + TD5RE_FRAMEDUMP_TICK, not by the
    # control socket: the tick form fires on the first RACE present whose sim
    # tick has reached N, so a before/after pair is captured at the SAME
    # simulated moment. Both names are excluded from the GENSTAMP env hash
    # (td5_trackgen.c tg_env_hash), so asking for a capture cannot by itself
    # change what gets generated.
    env["TD5RE_FRAMEDUMP"] = png
    env["TD5RE_FRAMEDUMP_TICK"] = str(a.tick)
    env["TD5RE_CONTROL_PORT"] = str(a.port)
    env["TD5RE_AUTOTRACK_SEED"] = a.seed
    env["TD5RE_AUTOTRACK_STREAM"] = "0"
    env["TD5RE_AUTOTRACK_REUSE"] = "1" if a.reuse else "0"
    env["TD5RE_GEO_PLACE"] = a.place
    env["TD5RE_WINDOW_TITLE"] = "LAND %s span %d" % (a.tag, a.span)
    if a.topdown > 0.0:
        env["TD5RE_CAM_TOPDOWN"] = "%g" % a.topdown
    if a.backdiv > 0.0:
        env["TD5RE_CAM_TOPDOWN_BACKDIV"] = "%g" % a.backdiv
    for kv in a.knob:
        k, _, v = kv.partition("=")
        env[k] = v

    if not a.reuse:
        lvl = os.path.join(ROOT, "re", "assets", "levels", "level090")
        if os.path.isdir(lvl):
            shutil.rmtree(lvl, ignore_errors=True)

    args = [os.path.join(ROOT, a.exe), "--SkipIntro=1", "--AutoRace=1",
            "--DefaultTrack=60", "--Control=1", "--PlayerIsAI=0",
            "--Opponents=0", "--StartSpanOffset=%d" % a.span] + MIN_GFX
    p = subprocess.Popen(args, cwd=ROOT, env=env)
    print("pid=%d tag=%s span=%d place=%s port=%d"
          % (p.pid, a.tag, a.span, a.place, a.port))
    time.sleep(5.0)
    to_back(p.pid)

    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(2.0)
    deadline = time.time() + a.timeout
    state, ok = None, False
    while time.time() < deadline:
        if p.poll() is not None:
            print("EXITED early rc=%s" % p.returncode)
            break
        r = cmd(s, a.port, "get_state")
        if r:
            state = json.dumps(r)
            if "RACE" in state.upper() and "PRERACE" not in state.upper():
                ok = True
                break
        to_back(p.pid)
        time.sleep(2.0)

    rc = 3
    if ok:
        print("in race, waiting for sim tick %d" % a.tick)
        for _ in range(120):
            if os.path.exists(png) and os.path.getsize(png) > 0:
                time.sleep(1.0)
                print("saved %s (%d bytes)" % (png, os.path.getsize(png)))
                rc = 0
                break
            time.sleep(1.0)
    else:
        print("TIMEOUT waiting for RACE; last state %s" % state)

    if p.poll() is None:
        cmd(s, a.port, "quit")
        for _ in range(30):
            if p.poll() is not None:
                break
            time.sleep(1.0)
    if p.poll() is None:
        p.kill()
    if rc != 0:
        print("NO FRAME for %s" % a.tag)
    return rc


if __name__ == "__main__":
    sys.exit(main())
