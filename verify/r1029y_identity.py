"""r1029y_identity.py -- round 1029 Y knob identity: the candidate exe with a knob value must reproduce the reference
exe (td5re_base.exe = integ-1028 bcea1edc) trace byte for byte.

    python -I verify/r1029y_identity.py

Cases (exe, knobs) vs (td5re_base.exe, knobs), deterministic shipped-trace races, no GAPLOG:
  new RACER_SAT=1    vs base default (1)       -- knob off = the old default
  new RACER_SAT=0    vs base RACER_SAT=0       -- master detectors
  new default (2)    vs base RACER_SAT=2       -- the new default is the old opt-in level 2
"""
import os
import subprocess
import sys

WT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CASES = [
    # (name, track, ticks, seed, new knobs, base knobs)
    ("k1_syd",  "2", "3000", "12345", "TD5RE_RACER_SAT=1", ""),
    ("k1_hon",  "8", "3000", "4242",  "TD5RE_RACER_SAT=1", ""),
    ("k1_new",  "5", "2200", "12345", "TD5RE_RACER_SAT=1", ""),
    ("k1_mosF", "0", "3000", "12345", "TD5RE_RACER_SAT=1|TD5RE_AI_FOLLOW_DIST=0", "TD5RE_AI_FOLLOW_DIST=0"),
    ("k0_syd",  "2", "3000", "12345", "TD5RE_RACER_SAT=0", "TD5RE_RACER_SAT=0"),
    ("k2_syd",  "2", "3000", "12345", "", "TD5RE_RACER_SAT=2"),
    ("k2_mosF", "0", "3000", "12345", "TD5RE_AI_FOLLOW_DIST=0", "TD5RE_RACER_SAT=2|TD5RE_AI_FOLLOW_DIST=0"),
]


def run(exe, tag, track, ticks, seed, knobs):
    subprocess.run([sys.executable, "-I", os.path.join(WT, "verify", "r1029y_run.py"), exe, tag, track, ticks, seed, knobs],
                   cwd=WT, check=False)
    return os.path.join(WT, "verify", "out", "shipped_%s_t%s" % (tag, track))


def main():
    for name, track, ticks, seed, kn, kb in CASES:
        dn = run("td5re.exe", "id_%s_new" % name, track, ticks, seed, kn)
        db = run("td5re_base.exe", "id_%s_base" % name, track, ticks, seed, kb)
        print("#### %s: new[%s] vs base[%s] track %s seed %s ticks %s" % (name, kn or "default", kb or "default", track, seed, ticks), flush=True)
        subprocess.run([sys.executable, "-I", os.path.join(WT, "verify", "shipped_trace_cmp.py"), db, dn], cwd=WT)
    print("identity done")


if __name__ == "__main__":
    main()
