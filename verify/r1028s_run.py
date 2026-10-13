"""r1028s_run.py -- round 1028 S: race a 6-car ALL-AI field and keep the contact log + pace trace.

    python -I verify/r1028s_run.py --tag base --tracks 0,2,5 --seeds 11,22,33 --extra AI_FOLLOW_DIST=0
    python -I verify/r1028s_run.py --tag fix  --tracks 61    --seeds 11,22,33

(A PowerShell runner would be the house style, but writing .ps1 is blocked by the managed
policy on this account, so the same launch recipe lives here.)

Every race: slot 0 is driven by the AI too (--PlayerIsAI=1), no traffic, no cops, damage off,
TD5RE_V2V_GAPLOG=2 so EVERY contact (racer pairs included) is logged with its true body gap.
Per run it keeps
    log/r1028s_<tag>_t<track>_s<seed>_race.log                 (contacts)
    verify/out/r1028s_<tag>/t<track>_s<seed>/race_trace_*.csv  (pace)
Standing launch rules: windowed, sent to the back, VSync off, RT off, minimum graphics, audio
off, no sockets.  Every inherited TD5RE_* is cleared; knobs reach the game only via --extra.
"""
import argparse
import ctypes
import os
import shutil
import subprocess
import sys
import time

WT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GFX = ["--Lighting=0", "--Quality=0", "--SunShadows=0", "--Reflections=0", "--WetRoads=0",
       "--StreetLights=0", "--CarLights=0", "--LegacyShadows=0", "--GIQuality=0",
       "--ShadowRays=0", "--ReflectionQuality=0", "--CarShadows=0", "--VFX=0",
       "--WorldBillboards=0", "--FoliageAA=0"]


def send_to_back(title):
    try:
        u = ctypes.windll.user32
        h = u.FindWindowW(None, title)
        if h:
            u.SetWindowPos(h, 1, 0, 0, 0, 0, 0x0013)   # HWND_BOTTOM | NOSIZE|NOMOVE|NOACTIVATE
            return True
    except Exception:
        pass
    return False


def clean_env(extra, seed, place, title, gaplog):
    e = {k: v for k, v in os.environ.items() if not k.startswith("TD5RE_")}
    e["TD5RE_RT"] = "0"
    e["TD5RE_RACE_SEED"] = str(seed)
    e["TD5RE_GEO_PLACE"] = place
    e["TD5RE_WINDOW_TITLE"] = title
    if gaplog:
        e["TD5RE_V2V_GAPLOG"] = "2"
    for kv in extra.replace(";", "|").split("|"):
        if "=" not in kv:
            continue
        k, v = kv.split("=", 1)
        k = k.strip()
        if not k:
            continue
        if not k.startswith("TD5RE_"):
            k = "TD5RE_" + k
        e[k] = v.strip().strip('"')
    return e


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tag", default="run")
    ap.add_argument("--tracks", default="61")
    ap.add_argument("--seeds", default="11,22,33")
    ap.add_argument("--exe", default="td5re.exe")
    ap.add_argument("--extra", default="")
    ap.add_argument("--ticks", type=int, default=3500)
    ap.add_argument("--opponents", type=int, default=5)
    ap.add_argument("--traffic", type=int, default=0)
    ap.add_argument("--ff", default="8")
    ap.add_argument("--timeout", type=int, default=420)
    ap.add_argument("--place", default="la_plata_partido")
    ap.add_argument("--difficulty", type=int, default=1)
    ap.add_argument("--width", type=int, default=640)
    ap.add_argument("--height", type=int, default=360)
    ap.add_argument("--renderscale", type=int, default=50)
    ap.add_argument("--no-gaplog", action="store_true")
    a = ap.parse_args()

    log = os.path.join(WT, "log")
    out_root = os.path.join(WT, "verify", "out", "r1028s_" + a.tag)
    os.makedirs(log, exist_ok=True)
    os.makedirs(out_root, exist_ok=True)

    for trk in [int(x) for x in a.tracks.split(",") if x.strip()]:
        for seed in [int(x) for x in a.seeds.split(",") if x.strip()]:
            name = f"t{trk}_s{seed}"
            for f in ("race.log", "engine.log", "frontend.log"):
                p = os.path.join(log, f)
                for _ in range(20):
                    if not os.path.exists(p):
                        break
                    try:
                        os.remove(p)
                    except OSError:
                        time.sleep(0.3)
            for f in os.listdir(log):
                if f.startswith("race_trace_") and f.endswith(".csv"):
                    try:
                        os.remove(os.path.join(log, f))
                    except OSError:
                        pass
            title = f"TD5RE r1028s {a.tag} {name}"
            env = clean_env(a.extra, seed, a.place, title, not a.no_gaplog)
            args = [os.path.join(WT, a.exe),
                    "--AutoRace=1", "--SkipIntro=1", f"--DefaultTrack={trk}",
                    f"--DefaultOpponents={a.opponents}", f"--Traffic={a.traffic}", "--Cops=0",
                    f"--Difficulty={a.difficulty}", "--PlayerIsAI=1", "--AutoThrottle=0",
                    "--CarDamage=0", "--Collisions=1", "--Windowed=1", "--VSync=0",
                    "--SFXVolume=0", "--MusicVolume=0",
                    f"--Width={a.width}", f"--Height={a.height}",
                    f"--RenderScale={a.renderscale}",
                    "--LogEnabled=1", "--LogRace=1",
                    "--RaceTrace=1", "--RaceTraceSlot=-1",
                    f"--RaceTraceMaxSimTicks={a.ticks}", "--RaceTraceMaxFrames=4000000",
                    "--TraceModules=track", "--TraceStages=post_track",
                    f"--TraceFastForward={a.ff}"] + GFX
            t0 = time.time()
            p = subprocess.Popen(args, cwd=WT, env=env)
            sent = False
            killed = False
            while p.poll() is None:
                time.sleep(0.5)
                if not sent:
                    sent = send_to_back(title)
                if time.time() - t0 > a.timeout:
                    p.kill()
                    killed = True
                    time.sleep(2)
                    break
            dst = os.path.join(log, f"r1028s_{a.tag}_{name}_race.log")
            src = os.path.join(log, "race.log")
            if os.path.exists(src):
                shutil.copyfile(src, dst)
            rd = os.path.join(out_root, name)
            os.makedirs(rd, exist_ok=True)
            for f in os.listdir(log):
                if f.startswith("race_trace_") and f.endswith(".csv"):
                    shutil.copyfile(os.path.join(log, f), os.path.join(rd, f))
            sz = os.path.getsize(dst) if os.path.exists(dst) else 0
            print(f"{a.tag} {name} {'KILLED(timeout)' if killed else 'exited'} "
                  f"after {int(time.time() - t0)}s  race.log {sz} bytes", flush=True)
    print(f"logs -> {log}/r1028s_{a.tag}_*_race.log ; traces -> {out_root}")


if __name__ == "__main__":
    main()
