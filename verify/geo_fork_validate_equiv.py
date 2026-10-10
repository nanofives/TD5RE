"""geo_fork_validate_equiv.py -- are two race speeds the same race? (round 1016 H)

  python verify/geo_fork_validate_equiv.py verify/out/ff1/fork_s11 verify/out/ff8/fork_s11 [...more dirs]

Compares the first dir (the reference, normally 1x) against every other: the per-tick
world position of every car (race_trace_pose.csv, the frame column is dropped because it
is the one thing that depends on speed) and reports the first sim tick where any car
differs, how far apart they are there, and how many ticks match bit-for-bit. Then the
per-fork numbers of geo_fork_validate.py for each dir, side by side.
"""
import csv
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import geo_fork_validate as G   # noqa: E402


def load_pose(d):
    out = {}
    path = os.path.join(d, "log", "race_trace_pose.csv")
    with open(path, newline="", encoding="utf-8", errors="replace") as f:
        for r in csv.DictReader(f):
            if r.get("stage") != "post_physics":
                continue
            try:
                out[(int(r["sim_tick"]), int(r["slot"]))] = (int(r["world_x"]), int(r["world_z"]), int(r["ang_yaw"]))
            except (TypeError, ValueError):
                continue
    return out


def main():
    dirs = sys.argv[1:]
    if len(dirs) < 2:
        print(__doc__)
        return 2
    ref = load_pose(dirs[0])
    rt = sorted({t for t, _s in ref})
    print("reference %s: %d ticks, last %d" % (dirs[0], len(rt), rt[-1] if rt else 0))
    for d in dirs[1:]:
        cur = load_pose(d)
        common = sorted({t for t, _s in ref} & {t for t, _s in cur})
        first = None
        same = 0
        for t in common:
            ok = all(ref.get((t, s)) == cur.get((t, s)) for s in range(6))
            if ok:
                same += 1
            elif first is None:
                first = t
        if first is None:
            print("%-40s IDENTICAL over %d common ticks (last %d)" % (d, len(common), common[-1] if common else 0))
        else:
            diffs = [(abs(ref[(first, s)][0] - cur[(first, s)][0]) + abs(ref[(first, s)][1] - cur[(first, s)][1])) / 256.0
                     for s in range(6) if (first, s) in ref and (first, s) in cur]
            print("%-40s first divergence at tick %d (max %.1f world units apart there); %d/%d ticks identical"
                  % (d, first, max(diffs) if diffs else 0, same, len(common)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
