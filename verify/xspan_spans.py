"""xspan_spans.py -- read a race-trace track CSV and judge span continuity.

The question Option B exists to answer is "did the car stay on the span it was
driving on when the road crossed itself". That is a property of the SEQUENCE of
span_raw values, not of any single one, so this reports:

  * every tick-to-tick jump larger than --tol spans (a jump is the symptom: a
    car cannot drive 400 spans in 1/30 s),
  * whether the car actually reached each crossing leg, which is what makes a
    "no jumps" result meaningful rather than vacuous -- a car that never got
    near the crossing proves nothing,
  * the span range covered, so a truncated run is visible rather than silent.

  python verify/xspan_spans.py log/xspan_track_base.csv --legs 131,135,562,566
"""
from __future__ import annotations

import argparse
import csv
import sys


def slots_in(path: str, stage: str = "post_track") -> list[int]:
    """Every slot that has at least one row, in ascending order."""
    seen = set()
    with open(path, newline="", encoding="utf-8", errors="replace") as f:
        rd = csv.reader(f)
        head = next(rd, None)
        if not head:
            return []
        col = {name: i for i, name in enumerate(head)}
        width = len(head)
        for r in rd:
            if len(r) < width or r[col["stage"]] != stage:
                continue
            seen.add(int(r[col["slot"]]))
    return sorted(seen)


def read_spans(path: str, slot: int = 0, stage: str = "post_track"):
    """(sim_tick, span_raw) for one slot/stage, skipping truncated rows.

    The last line of a trace is routinely half-written -- the process is quit
    while the writer is mid-row -- so a short row is dropped rather than
    treated as a parse error.
    """
    out = []
    with open(path, newline="", encoding="utf-8", errors="replace") as f:
        rd = csv.reader(f)
        head = next(rd, None)
        if not head:
            return out
        col = {name: i for i, name in enumerate(head)}
        need = ("sim_tick", "slot", "stage", "span_raw")
        if any(k not in col for k in need):
            raise SystemExit("%s: not a track trace (columns: %s)" % (path, head))
        width = len(head)
        for r in rd:
            if len(r) < width:
                continue            # truncated tail row
            if r[col["slot"]] != str(slot) or r[col["stage"]] != stage:
                continue
            out.append((int(r[col["sim_tick"]]), int(r[col["span_raw"]])))
    return out


def report_slot(path: str, slot: int, legs: list[tuple[int, int]], tol: int) -> int:
    rows = read_spans(path, slot=slot)
    if not rows:
        print("  slot %d: NO ROWS" % slot)
        return 1
    spans = [s for _, s in rows]
    print("  slot %d" % slot)
    print("  ticks        : %d (sim_tick %d..%d)" % (len(rows), rows[0][0], rows[-1][0]))
    print("  span_raw     : %d .. %d" % (min(spans), max(spans)))

    jumps = [(rows[i - 1][0], rows[i - 1][1], rows[i][0], rows[i][1])
             for i in range(1, len(rows))
             if abs(rows[i][1] - rows[i - 1][1]) > tol]
    print("  jumps > %-4d : %d" % (tol, len(jumps)))
    for a_t, a_s, b_t, b_s in jumps[:20]:
        print("      tick %6d span %5d  ->  tick %6d span %5d   (delta %+d)"
              % (a_t, a_s, b_t, b_s, b_s - a_s))
    if len(jumps) > 20:
        print("      ... %d more" % (len(jumps) - 20))

    reached = True
    for lo, hi in legs:
        hit = [t for t, s in rows if lo <= s <= hi]
        if hit:
            print("  leg %4d-%-4d : reached, ticks %d..%d" % (lo, hi, hit[0], hit[-1]))
        else:
            print("  leg %4d-%-4d : NOT REACHED -- this run does not test the crossing"
                  % (lo, hi))
            reached = False

    ok = not jumps and reached
    print("  VERDICT      : %s" % ("CONTINUOUS through every leg" if ok else
                                   "JUMPS PRESENT" if jumps else
                                   "inconclusive (crossing not reached)"))
    return 0 if ok else 1


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("csv", nargs="+")
    ap.add_argument("--legs", default="",
                    help="crossing legs as lo,hi[,lo,hi...] span indices")
    ap.add_argument("--tol", type=int, default=3,
                    help="tick-to-tick span delta treated as a jump (default 3)")
    ap.add_argument("--slot", default="all",
                    help="'all' (default), or a slot index")
    a = ap.parse_args(argv)

    nums = [int(x) for x in a.legs.split(",") if x.strip()] if a.legs else []
    if len(nums) % 2:
        raise SystemExit("--legs needs an even count of span indices")
    legs = list(zip(nums[0::2], nums[1::2]))

    rc = 0
    for p in a.csv:
        print("=== %s" % p)
        slots = slots_in(p) if a.slot == "all" else [int(a.slot)]
        if not slots:
            print("  NO ROWS")
            rc |= 1
        for s in slots:
            rc |= report_slot(p, s, legs, a.tol)
            print()
    return rc


if __name__ == "__main__":
    sys.exit(main())
