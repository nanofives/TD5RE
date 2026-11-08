"""rt_bench.py -- frame-time percentiles + GPU per-pass cost for one RT config.

[round UA 2026-11-08] Launches td5re.exe (dev build) into an AutoRace with the
player on full throttle, records TD5RE_FRAMESTATS frames, and prints the block
the game appends to log/framestats.txt (CPU frame-time p50/p95/p99/max, spikes,
load time, GPU per-pass ms via TD5RE_RT_TIMESTAMPS=2, VRAM usage vs budget).

Every launch is windowed, sent to the back without activation, VSync off
(asleep monitors make vsync read 257 ms). All TD5RE_* env vars of the caller
are cleared so a stale knob cannot leak into a measurement.

  python verify/rt_bench.py --tag high_1080 --res 1920x1080 --preset HIGH
  python verify/rt_bench.py --tag custom --set ShadowRays=1 --set GIQuality=0
  python verify/rt_bench.py --tag off --preset LOW

--preset maps to the PERFORMANCE > LIGHTING tier table (main.c k_light_tier),
expressed as the same CLI overrides the game accepts (--Quality, --ShadowRays,
...), so the run never depends on (or writes) a user's td5re.ini choice.
"""
import argparse
import ctypes
import os
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# PERFORMANCE > LIGHTING tiers as CLI overrides. Keep in sync with
# td5mod/src/td5re/main.c k_light_tier (columns qual rays res refl rng gi probe lights).
PRESETS = {
    "LOW":    dict(Quality=0, ShadowRays=1, ShadowRes=0, ReflectionQuality=0, ReflectionRange=0, GIQuality=0, SunProbe=0, LightQuality=0),
    "MEDIUM": dict(Quality=1, ShadowRays=1, ShadowRes=0, ReflectionQuality=1, ReflectionRange=0, GIQuality=0, SunProbe=1, LightQuality=0),
    "HIGH":   dict(Quality=1, ShadowRays=2, ShadowRes=1, ReflectionQuality=2, ReflectionRange=1, GIQuality=1, SunProbe=1, LightQuality=1),
    "ULTRA":  dict(Quality=1, ShadowRays=8, ShadowRes=1, ReflectionQuality=2, ReflectionRange=1, GIQuality=2, SunProbe=1, LightQuality=1),
}


def send_to_back(title, timeout_s=60.0):
    user32 = ctypes.windll.user32
    t0 = time.time()
    while time.time() - t0 < timeout_s:
        h = user32.FindWindowW(None, title)
        if h:
            # HWND_BOTTOM, SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE
            user32.SetWindowPos(h, 1, 0, 0, 0, 0, 0x13)
            return True
        time.sleep(0.2)
    return False


def last_block(path):
    if not os.path.exists(path):
        return None
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        txt = f.read()
    i = txt.rfind("=== FRAMESTATS")
    return txt[i:] if i >= 0 else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tag", required=True)
    ap.add_argument("--res", default="1920x1080")
    ap.add_argument("--track", type=int, default=0, help="DefaultTrack (0 = Moscow)")
    ap.add_argument("--frames", type=int, default=1800)
    ap.add_argument("--skip", type=int, default=120)
    ap.add_argument("--preset", default="HIGH", choices=sorted(PRESETS) + ["NONE"])
    ap.add_argument("--set", action="append", default=[], help="extra CLI override Key=Value")
    ap.add_argument("--env", action="append", default=[], help="extra env TD5RE_X=V")
    ap.add_argument("--exe", default=os.path.join(ROOT, "td5re.exe"))
    ap.add_argument("--timeout", type=int, default=600)
    ap.add_argument("--row", default="", help="run a selftest row (e.g. roam-traffic-smoke) instead of AutoRace")
    a = ap.parse_args()

    w, h = a.res.lower().split("x")
    env = {k: v for k, v in os.environ.items() if not k.upper().startswith("TD5RE_")}
    title = "td5re_bench_" + a.tag
    env.update({
        "TD5RE_WINDOW_TITLE": title,
        "TD5RE_FRAMESTATS": str(a.frames),
        "TD5RE_FRAMESTATS_SKIP": str(a.skip),
        "TD5RE_FRAMESTATS_TAG": a.tag,
        "TD5RE_RT_TIMESTAMPS": "2",
        "TD5RE_AUTO_PERF": "0",
    })
    for kv in a.env:
        k, v = kv.split("=", 1)
        env[k] = v

    opts = dict(PRESETS.get(a.preset, {}))
    opts["Lighting"] = 1 if a.preset != "NONE" else opts.get("Lighting", 1)
    for kv in a.set:
        k, v = kv.split("=", 1)
        opts[k] = v
    if a.row:
        env["TD5RE_SELFTEST_ROWS"] = a.row
        args = [a.exe, "--SelfTest=1", "--SelfTestSuite=1"]
    else:
        args = [a.exe, "--AutoRace=1", "--SkipIntro=1", "--AutoThrottle=1", "--DefaultTrack=%d" % a.track]
    args += ["--Windowed=1", "--VSync=0", "--MinLevel=1", "--Width=%s" % w, "--Height=%s" % h]
    args += ["--%s=%s" % (k, v) for k, v in opts.items()]
    print("[rt_bench] %s" % " ".join(args[1:]), flush=True)
    t0 = time.time()
    p = subprocess.Popen(args, cwd=ROOT, env=env)
    send_to_back(title)
    try:
        rc = p.wait(timeout=a.timeout)
    except subprocess.TimeoutExpired:
        p.kill()
        print("[rt_bench] %s TIMEOUT after %d s" % (a.tag, a.timeout))
        return 2
    print("[rt_bench] %s exit=%d wall=%.1f s" % (a.tag, rc, time.time() - t0), flush=True)
    blk = last_block(os.path.join(ROOT, "log", "framestats.txt"))
    if not blk or ("tag=%s " % a.tag) not in blk.splitlines()[0]:
        print("[rt_bench] %s: no FRAMESTATS block for this tag" % a.tag)
        return 1
    print(blk, flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
