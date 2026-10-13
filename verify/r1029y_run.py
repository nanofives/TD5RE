"""r1029y_run.py -- round 1029 Y: one deterministic AI race on a shipped track with the per-tick trace AND the race.log
kept together, so a trace A/B and the v2v_gap: contact lines come from the SAME run.

    python -I verify/r1029y_run.py <exe> <tag> <track> <ticks> [<seed>] [KNOB=v|KNOB=v ...] [<timeout s>]

Same launch as verify/shipped_trace.ps1 (Traffic 0, 5 AI, fixed TD5RE_RACE_SEED, RT off, min graphics, windowed,
every inherited TD5RE_* cleared, knobs only from the argument) but launched directly with a longer timeout:
shipped_trace.ps1 kills the game at 300 s, which truncated the Newcastle traces AND lost the unflushed race.log tail.
Output: verify/out/shipped_<tag>_t<track>/ (race_trace_*.csv + race.log). Prints KILLED when the timeout hit.
"""
import glob
import os
import shutil
import subprocess
import sys

WT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GFX = ["--Lighting=0", "--Quality=0", "--SunShadows=0", "--Reflections=0", "--WetRoads=0", "--StreetLights=0", "--CarLights=0",
       "--LegacyShadows=0", "--GIQuality=0", "--ShadowRays=0", "--ReflectionQuality=0", "--CarShadows=0", "--VFX=0",
       "--WorldBillboards=0", "--FoliageAA=0"]


def main():
    exe, tag, track, ticks = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
    seed = sys.argv[5] if len(sys.argv) > 5 else "12345"
    extra = sys.argv[6] if len(sys.argv) > 6 else ""
    timeout = int(sys.argv[7]) if len(sys.argv) > 7 else 900
    env = {k: v for k, v in os.environ.items() if not k.upper().startswith("TD5RE_")}
    env["TD5RE_RT"] = "0"
    env["TD5RE_RACE_SEED"] = seed
    env["TD5RE_WINDOW_TITLE"] = "TD5RE r1029y " + tag
    for kv in extra.replace(";", "|").split("|"):
        if "=" in kv:
            k, v = kv.split("=", 1)
            k = k.strip()
            if not k.upper().startswith("TD5RE_"):
                k = "TD5RE_" + k
            env[k] = v.strip()
    logd = os.path.join(WT, "log")
    os.makedirs(logd, exist_ok=True)
    for f in glob.glob(os.path.join(logd, "race_trace_*.csv")) + [os.path.join(logd, "race.log")]:
        try:
            os.remove(f)
        except OSError:
            pass
    args = [os.path.join(WT, exe), "--AutoRace=1", "--SkipIntro=1", "--DefaultTrack=" + track, "--DefaultOpponents=5",
            "--Traffic=0", "--PlayerIsAI=1", "--AutoThrottle=0", "--CarDamage=0", "--Windowed=1", "--VSync=0",
            "--SFXVolume=0", "--MusicVolume=0", "--RaceTrace=1", "--RaceTraceSlot=-1", "--RaceTraceMaxSimTicks=" + ticks,
            "--RaceTraceMaxFrames=4000000", "--TraceModules=pose,motion,track,driver",
            "--TraceStages=post_physics,post_ai,post_track", "--TraceFastForward=8"] + GFX
    p = subprocess.Popen(args, cwd=WT, env=env)
    killed = False
    try:
        p.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        p.kill()
        p.wait()
        killed = True
    out = os.path.join(WT, "verify", "out", "shipped_%s_t%s" % (tag, track))
    os.makedirs(out, exist_ok=True)
    for f in glob.glob(os.path.join(logd, "race_trace_*.csv")):
        shutil.copy(f, out)
    log = os.path.join(logd, "race.log")
    if os.path.exists(log):
        shutil.copy(log, os.path.join(out, "race.log"))
    print("%s %s -> %s" % (tag, "KILLED at timeout" if killed else "exited", out), flush=True)


if __name__ == "__main__":
    main()
