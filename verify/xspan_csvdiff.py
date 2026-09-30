"""xspan_csvdiff.py -- compare two directories of race-trace CSVs.

Used to prove a shipped track is unchanged across two builds.

ROWS ARE KEYED, NOT ZIPPED. The obvious implementation -- strip the leading
`frame` column and compare row i to row i -- was tried first and is wrong. The
harness races for a fixed WALL time, so the two runs render a different number
of FRAMES (45.5 vs 50.0 fps was measured on Moscow), which changes how many
per-frame rows land in the window. One extra row on one side then misaligns
everything after it and the diff reports tens of thousands of "differences"
that are one insertion. Keying on (sim_tick, stage, slot) -- the identity of
the simulation step that produced the row -- compares like with like and is
immune to frame cadence entirely.

SIM MODULES ARE THE GATE; frame and view are NOT. race_trace_frame.csv carries
instant_fps, frame_dt and the tick budget, and race_trace_view.csv carries the
interpolated camera: both are functions of the real frame rate by design, so
they differ between any two runs of the SAME exe and cannot show a code change.
They are still compared and printed, just reported separately rather than
counted as failures -- hiding them would be worse than explaining them.

  python verify/xspan_csvdiff.py log/ab_moscow-a log/ab_moscow-b
"""
from __future__ import annotations

import os
import sys

# Modules whose rows are pure simulation state: identical inputs must give
# identical output, tick for tick.
SIM_FILES = {
    "race_trace_pose.csv", "race_trace_motion.csv", "race_trace_track.csv",
    "race_trace_controls.csv", "race_trace_progress.csv",
    "race_trace_rotation.csv", "race_trace_sound.csv", "race_trace_driver.csv",
}


def keyed(path: str) -> dict[tuple, str]:
    """{(sim_tick, stage, slot): rest-of-row}, skipping truncated rows.

    The last row of a trace is routinely half-written (the process is quit
    while the writer is mid-row), so a short row is dropped.
    """
    with open(path, encoding="utf-8", errors="replace") as f:
        lines = f.read().splitlines()
    if not lines:
        return {}
    head = lines[0].split(",")
    width = len(head)
    idx = {name: i for i, name in enumerate(head)}
    kcols = [idx[c] for c in ("sim_tick", "stage", "slot") if c in idx]
    if not kcols:
        return {}
    # Everything except `frame` (render-dependent counter) and the key columns.
    vcols = [i for i in range(width) if i != idx.get("frame", -1) and i not in kcols]
    out: dict[tuple, str] = {}
    for ln in lines[1:]:
        p = ln.split(",")
        if len(p) != width:
            continue
        out[tuple(p[i] for i in kcols)] = ",".join(p[i] for i in vcols)
    return out


def main(argv: list[str]) -> int:
    if len(argv) != 3:
        raise SystemExit("usage: xspan_csvdiff.py <dir_a> <dir_b>")
    da, db = argv[1], argv[2]
    names = sorted({n for n in os.listdir(da) + os.listdir(db) if n.endswith(".csv")})
    if not names:
        print("NO CSVs in either directory -- nothing was compared")
        return 1

    bad = 0
    sim_rows = 0
    for n in names:
        pa, pb = os.path.join(da, n), os.path.join(db, n)
        is_sim = n in SIM_FILES
        tag = "" if is_sim else "   [render-dependent, not a gate]"
        if not os.path.exists(pa) or not os.path.exists(pb):
            print("  %-28s MISSING on one side%s" % (n, tag))
            bad += is_sim
            continue
        ka, kb = keyed(pa), keyed(pb)
        common = ka.keys() & kb.keys()
        only = len(ka) - len(common), len(kb) - len(common)
        diffs = sorted(k for k in common if ka[k] != kb[k])
        note = "" if only == (0, 0) else "  (%d/%d rows only on one side)" % only
        if is_sim:
            sim_rows += len(common)
        if diffs:
            print("  %-28s %6d keyed rows, %d DIFFER%s%s"
                  % (n, len(common), len(diffs), note, tag))
            k = diffs[0]
            print("      key %s" % (k,))
            print("      A: %s" % ka[k][:150])
            print("      B: %s" % kb[k][:150])
            bad += is_sim
        else:
            print("  %-28s %6d keyed rows, identical%s%s"
                  % (n, len(common), note, tag))

    if sim_rows == 0:
        print("VERDICT: NO SIM ROWS compared -- the races produced no usable "
              "trace, this proves nothing")
        return 1
    print("VERDICT: sim state %s (%d keyed rows across %d sim module(s))"
          % ("IDENTICAL" if not bad else "DIFFERS in %d module(s)" % bad,
             sim_rows, len(SIM_FILES & set(names))))
    return 0 if not bad else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
