#!/usr/bin/env python3
"""roam F2-B launcher: one td5re.exe run, windowed, pushed to the back, RT off +
minimum graphics, TD5RE_* env cleared except what -e passes, then the log dir is
copied to <out>. Exit code = the game's exit code (or 124 on timeout).

  python verify/roam/f2b_run.py --exe td5re.exe --out _archive/roam/F2-B/run1 \
      -e TD5RE_ROAM_MIRROR=1 -- --AutoRace=1 --DefaultTrack=0 ...
"""
import argparse, ctypes, os, shutil, subprocess, sys, time

MIN_GFX = ("--Windowed=1 --Width=960 --Height=540 --Lighting=0 --Quality=0 --SunShadows=0 "
           "--Reflections=0 --WetRoads=0 --StreetLights=0 --CarLights=0 --LegacyShadows=0 "
           "--GIQuality=0 --ShadowRays=0 --ReflectionQuality=0 --CarShadows=0 --VFX=0 "
           "--WorldBillboards=0 --FoliageAA=0 --RenderScale=50 --LogEnabled=1").split()


def push_back(title, deadline):
    user32 = ctypes.windll.user32
    while time.time() < deadline:
        h = user32.FindWindowW(None, title)
        if h:
            user32.SetWindowPos(h, 1, 0, 0, 0, 0, 0x13)   # HWND_BOTTOM, NOMOVE|NOSIZE|NOACTIVATE
            return True
        time.sleep(0.2)
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--timeout", type=float, default=300.0)
    ap.add_argument("-e", action="append", default=[], help="KEY=VALUE env for the game")
    ap.add_argument("args", nargs="*")
    a = ap.parse_args()
    exe = os.path.abspath(a.exe)
    root = os.path.dirname(exe)
    logdir = os.path.join(root, "log")
    env = {k: v for k, v in os.environ.items() if not k.upper().startswith("TD5RE_")}
    title = "td5re-f2b-%d" % os.getpid()
    env["TD5RE_WINDOW_TITLE"] = title
    env["TD5RE_RT"] = "0"
    for kv in a.e:
        k, v = kv.split("=", 1)
        env[k] = v
    if os.path.isdir(logdir):
        for f in os.listdir(logdir):
            p = os.path.join(logdir, f)
            if os.path.isfile(p):
                try:
                    os.remove(p)
                except OSError:
                    pass
    cmd = [exe] + MIN_GFX + a.args
    t0 = time.time()
    pr = subprocess.Popen(cmd, cwd=root, env=env)
    push_back(title, time.time() + 30)
    try:
        rc = pr.wait(timeout=a.timeout)
    except subprocess.TimeoutExpired:
        pr.kill()
        rc = 124
    dt = time.time() - t0
    os.makedirs(a.out, exist_ok=True)
    if os.path.isdir(logdir):
        for f in os.listdir(logdir):
            p = os.path.join(logdir, f)
            if os.path.isfile(p):
                shutil.copy2(p, os.path.join(a.out, f))
    with open(os.path.join(a.out, "RUN.txt"), "w") as f:
        f.write("cmd: %s\nenv: %s\nrc: %d\nsecs: %.1f\n" % (" ".join(cmd), " ".join(a.e), rc, dt))
    print("rc=%d secs=%.1f out=%s" % (rc, dt, a.out))
    return rc


if __name__ == "__main__":
    sys.exit(main())
