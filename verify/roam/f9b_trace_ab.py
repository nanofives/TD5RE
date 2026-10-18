#!/usr/bin/env python3
"""roam F9b: mode-off race-trace A/B, this tree's td5re.exe vs td5re_base.exe (main's exe) on AutoRace tracks.

  python -I verify/roam/f9b_trace_ab.py <out dir> [track ...]      (default tracks: 0 61)

Each run goes through verify/roam/f2b_run.py (windowed, pushed back, RT off + minimum graphics, TD5RE_* cleared),
5 opponents, auto throttle, RaceTrace capped at 400 sim ticks; the race_trace_*.csv files are compared byte for byte
(sha256) and with f2b_tracecmp.py (render-frame column ignored)."""
import glob, hashlib, os, subprocess, sys

here = os.path.dirname(os.path.abspath(__file__))
root = os.path.abspath(os.path.join(here, "..", ".."))
out = os.path.abspath(sys.argv[1])
tracks = sys.argv[2:] or ["0", "61"]
ok_all = True
for t in tracks:
    dirs = {}
    for name, exe in (("new", "td5re.exe"), ("base", "td5re_base.exe")):
        d = os.path.join(out, "ab_%s_%s" % (t, name))
        subprocess.call([sys.executable, "-I", os.path.join(here, "f2b_run.py"), "--exe", os.path.join(root, exe), "--out", d,
                         "--timeout", "300", "--", "--AutoRace=1", "--SkipIntro=1", "--DefaultTrack=" + t, "--DefaultOpponents=5",
                         "--AutoThrottle=1", "--RaceTrace=1", "--RaceTraceMaxFrames=40000", "--RaceTraceMaxSimTicks=400"], cwd=root)
        dirs[name] = d
    files = sorted(set(os.path.basename(f) for n in dirs for f in glob.glob(os.path.join(dirs[n], "race_trace_*.csv"))))
    same = bool(files)
    for f in files:
        h = {}
        for n in dirs:
            p = os.path.join(dirs[n], f)
            h[n] = hashlib.sha256(open(p, "rb").read()).hexdigest()[:16] if os.path.exists(p) else None
        s = h["new"] == h["base"] and h["new"] is not None
        same &= s
        print("track %s %-28s new=%s base=%s %s" % (t, f, h["new"], h["base"], "SAME" if s else "DIFF"))
    subprocess.call([sys.executable, "-I", os.path.join(here, "f2b_tracecmp.py"), dirs["new"], dirs["base"]], cwd=root)
    print("RESULT track %s: %s (%d trace files)" % (t, "IDENTICAL" if same else "DIFFERENT/EMPTY", len(files)))
    ok_all &= same
sys.exit(0 if ok_all else 1)
