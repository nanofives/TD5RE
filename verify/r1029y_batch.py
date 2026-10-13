"""r1029y_batch.py -- round 1029 Y matrix: TD5RE_RACER_SAT=1 vs 2 over tracks x seeds, deterministic shipped-trace
races (Traffic 0, 5 AI, fixed seed), every run with TD5RE_V2V_GAPLOG=2 (contact lines incl. REJ).

    python -I verify/r1029y_batch.py <exe> <name> <ticks> <tracks,csv> <seeds,csv> [extra knobs "A=1|B=2"]

Each (track, seed, mode) -> verify/out/shipped_<name>_m<mode>_s<seed>_t<track>/ ; then per pair:
shipped_trace_cmp first divergence and r1029y_fate on both logs.
"""
import os
import subprocess
import sys

WT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main():
    exe, name, ticks, tracks, seeds = sys.argv[1:6]
    extra = sys.argv[6] if len(sys.argv) > 6 else ""
    for t in tracks.split(","):
        for s in seeds.split(","):
            for mode in ("1", "2"):
                knobs = "TD5RE_V2V_GAPLOG=2|TD5RE_RACER_SAT=" + mode + ("|" + extra if extra else "")
                tag = "%s_m%s_s%s" % (name, mode, s)
                subprocess.run([sys.executable, "-I", os.path.join(WT, "verify", "r1029y_run.py"),
                                exe, tag, t, ticks, s, knobs], cwd=WT, check=False)
            d1 = os.path.join(WT, "verify", "out", "shipped_%s_m1_s%s_t%s" % (name, s, t))
            d2 = os.path.join(WT, "verify", "out", "shipped_%s_m2_s%s_t%s" % (name, s, t))
            print("#### track %s seed %s" % (t, s), flush=True)
            subprocess.run([sys.executable, "-I", os.path.join(WT, "verify", "shipped_trace_cmp.py"), d1, d2], cwd=WT)
    print("batch %s done" % name)


if __name__ == "__main__":
    main()
