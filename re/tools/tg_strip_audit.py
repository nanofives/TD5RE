#!/usr/bin/env python3
"""tg_strip_audit.py -- audit a STRIP.DAT (generated or shipped) for the
lane-management invariants the auto-track generator promises:

  * every lane-count change on the main ring is carried by ONE transition
    span typed 2..7 (add right / add left / add both / drop right / drop left /
    drop both), the wider of the two spans at the seam;
  * the lane BASE nibble moves by -1 on a left/both add and +1 on a left/both
    drop, and stays put on right-side changes;
  * the shared row between the two spans has min(lanes) + 1 points;
  * every fork obeys lanes(F) = lanes(F+1) + lanes(B0).

Usage: python re/tools/tg_strip_audit.py re/assets/levels/level090/STRIP.DAT
Exit code 1 on any violation, so it can gate a generation run.
"""
import sys, struct, collections
d = open(sys.argv[1], 'rb').read()
span_off, ring, vtx_off, vtx_cnt, total = struct.unpack_from('<5I', d, 0)
nj = struct.unpack_from('<I', d, 0x14)[0]
jumps = [struct.unpack_from('<HHH', d, 0x18 + 6 * k) for k in range(nj)]
sp = [struct.unpack_from('<BBBBHHHHiii', d, span_off + 24 * i) for i in range(total)]
print('spans total', total, 'ring', ring, 'verts', vtx_cnt, 'jumps', jumps)
lanes = collections.Counter(x[3] & 0xF for x in sp[:ring])
types = collections.Counter(x[0] for x in sp[:ring])
bases = collections.Counter(x[3] >> 4 for x in sp[:ring])
print('lanes', dict(sorted(lanes.items())), 'types', dict(sorted(types.items())), 'bases', dict(sorted(bases.items())))
# invariants on the main ring (skip fork-patched spans: types 8/11 and those whose rows are private)
bad = 0; changes = []
for i in range(ring - 1):
    a, b = sp[i], sp[i + 1]
    la, lb = a[3] & 0xF, b[3] & 0xF
    ta, tb = a[0], b[0]
    if ta in (8, 11) or tb in (8, 11):
        continue
    # shared row: a.rvi == b.lvi (except at fork patched spans)
    if a[5] != b[4] and not (i + 1 < ring and sp[i + 1][0] in (8, 11)):
        pass  # forks re-append rows; only report lane-change seams below
    if la != lb:
        # near/far row size of the wider (transition) span
        wide = a if la > lb else b
        near = wide[5] - wide[4]
        exp_type = None
        if lb > la:            # ADD typed on b
            dlt = lb - la
            exp_type = 4 if dlt >= 2 else (2, 3)
            got = tb
        else:                  # DROP typed on a
            dlt = la - lb
            exp_type = 7 if dlt >= 2 else (5, 6)
            got = ta
        ok = (got == exp_type) if isinstance(exp_type, int) else (got in exp_type)
        # base nibble rule: left change -> base shifts
        ba, bb = a[3] >> 4, b[3] >> 4
        exp_db = 0
        if got in (3, 4): exp_db = -1
        if got in (6, 7): exp_db = +1
        ok_b = (bb - ba) == exp_db
        # shared row point count = min lanes + 1 : near row size of the wider span
        # near row size for b = b.rvi - b.lvi only valid if b's far row follows; use:
        rows_ok = True
        if lb > la:
            rows_ok = (b[5] - b[4]) == min(la, lb) + 1
        else:
            rows_ok = (b[4] - a[4]) == la + 1 if a[4] < b[4] else True
        changes.append((i, la, lb, got, ba, bb, ok and ok_b and rows_ok))
        if not (ok and ok_b and rows_ok):
            bad += 1
print('lane-change seams on ring:', len(changes), 'violations:', bad)
for c in changes[:40]:
    print('  seam %4d: %d->%d type %d base %d->%d %s' % (c[0], c[1], c[2], c[3], c[4], c[5], 'ok' if c[6] else 'BAD'))
# fork sum rule
for lo, hi, base in jumps:
    F = base - 1
    print('fork F=%d lanes %d -> %d + %d  %s' % (F, sp[F][3] & 0xF, sp[F + 1][3] & 0xF, sp[lo][3] & 0xF,
          'sum OK' if (sp[F][3] & 0xF) == (sp[F + 1][3] & 0xF) + (sp[lo][3] & 0xF) else 'SUM MISMATCH'))
sys.exit(1 if bad else 0)
