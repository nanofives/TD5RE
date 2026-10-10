"""geo_r1015e_strip.py -- read a generated geo level's STRIP.DAT and check the plaza fork's
corridor span by span (round 1015 E).

  python verify/geo_r1015e_strip.py re/assets/levels/level091 [--fork N]

For every fork whose corridor is longer than the avenue forks' (or the one named by --fork) it prints:

  * the fork span (type 8), the main throat, the corridor spans (type 9, 1.., 10) and the rejoin
    span (type 11): index, type, lanes, link_next / link_prev;
  * CONTINUITY: the distance between one corridor span's far row centre and the next span's near
    row centre (the strip stores private rows per span, so this must be ~0), and the largest
    heading change per span along the corridor;
  * WIDTH: the corridor's width per span (must be lanes_b * lane width);
  * CLEARANCE: the smallest distance from a corridor row centre to ANY main-ring row centre that
    is not in the fork's own throats (a corridor that crosses or hugs the ring road would show up
    here), and the distance from the first/last corridor row to the main half beside it;
  * LANE ARITHMETIC: lanes(F) == lanes(F+1) + lanes(corridor 0) and lanes(R) == lanes(R-1) +
    lanes(last corridor span).

Reads files only (no game).
"""
import argparse
import math
import os
import struct
import sys

ap = argparse.ArgumentParser()
ap.add_argument('lvl')
ap.add_argument('--fork', type=int, default=-1)
a = ap.parse_args()
LW = 1500.0
UPM = 430.0

b = open(os.path.join(a.lvl, 'STRIP.DAT'), 'rb').read()
span_off, ring, vtx_off, nvtx, nspans = struct.unpack_from('<5I', b, 0)
nrec = struct.unpack_from('<I', b, 0x14)[0]
jumps = [struct.unpack_from('<HHH', b, 0x18 + i * 6) for i in range(nrec)]


def span(i):
    o = span_off + i * 24
    t = b[o]
    attr = b[o + 1]
    lanes = b[o + 3] & 0x0F
    lvi, rvi = struct.unpack_from('<HH', b, o + 4)
    ln, lp = struct.unpack_from('<hh', b, o + 8)
    ox, oy, oz = struct.unpack_from('<iii', b, o + 12)
    return dict(i=i, type=t, lanes=lanes, lvi=lvi, rvi=rvi, ln=ln, lp=lp, o=(ox, oy, oz))


def vert(vi, o):
    x, y, z = struct.unpack_from('<hhh', b, vtx_off + vi * 6)
    return (o[0] + x, o[1] + y, o[2] + z)


def row(sp, which):
    vi = sp['lvi'] if which == 0 else sp['rvi']
    pts = [vert(vi + j, sp['o']) for j in range(sp['lanes'] + 1)]
    return pts


def centre(pts):
    return ((pts[0][0] + pts[-1][0]) / 2.0, (pts[0][1] + pts[-1][1]) / 2.0, (pts[0][2] + pts[-1][2]) / 2.0)


def width(pts):
    return math.hypot(pts[0][0] - pts[-1][0], pts[0][2] - pts[-1][2])


def dist(p, q):
    return math.hypot(p[0] - q[0], p[2] - q[2])


S = [span(i) for i in range(nspans)]
print('%s: ring %d, %d spans total, %d fork record(s)' % (a.lvl, ring, nspans, nrec))
for fi, (c0, c1, base) in enumerate(jumps):
    L = c1 - c0 + 1
    F = base - 1
    R = F + 1 + L
    if a.fork >= 0 and a.fork != fi:
        continue
    print('\nfork %d: F=%d len=%d R=%d corridor spans %d..%d' % (fi, F, L, R, c0, c1))
    sF, sR = S[F], S[R]
    print('  F  type %d lanes %d link_next %d | R type %d lanes %d link_prev %d' %
          (sF['type'], sF['lanes'], sF['ln'], sR['type'], sR['lanes'], sR['lp']))
    types = [S[c]['type'] for c in range(c0, c1 + 1)]
    ok_types = types[0] == 9 and types[-1] == 10 and all(t == 1 for t in types[1:-1])
    print('  corridor span types: first %d last %d, interior all 1: %s' % (types[0], types[-1], ok_types))
    lane_a = S[F + 1]['lanes']
    lane_b0 = S[c0]['lanes']
    lane_bl = S[c1]['lanes']
    print('  lanes: F %d = F+1 %d + corridor0 %d : %s | R %d = R-1 %d + corridor-last %d : %s' %
          (sF['lanes'], lane_a, lane_b0, sF['lanes'] == lane_a + lane_b0,
           sR['lanes'], S[R - 1]['lanes'], lane_bl, sR['lanes'] == S[R - 1]['lanes'] + lane_bl))
    # continuity
    worst_gap = 0.0
    worst_turn = 0.0
    wmin, wmax = 1e9, 0.0
    heads = []
    cc = []
    for c in range(c0, c1 + 1):
        r0, r1 = row(S[c], 0), row(S[c], 1)
        wmin = min(wmin, width(r0), width(r1)); wmax = max(wmax, width(r0), width(r1))
        cc.append((centre(r0), centre(r1)))
    for k in range(len(cc) - 1):
        worst_gap = max(worst_gap, dist(cc[k][1], cc[k + 1][0]))
    for k in range(len(cc)):
        dx, dz = cc[k][1][0] - cc[k][0][0], cc[k][1][2] - cc[k][0][2]
        heads.append(math.atan2(dx, dz))
    for k in range(len(heads) - 1):
        d = heads[k + 1] - heads[k]
        while d > math.pi: d -= 2 * math.pi
        while d < -math.pi: d += 2 * math.pi
        worst_turn = max(worst_turn, abs(d))
    steps = [dist(cc[k][0], cc[k][1]) / UPM for k in range(len(cc))]
    print('  continuity: worst gap between consecutive corridor spans %.2f units; worst heading change %.3f rad/span; '
          'span length %.2f..%.2f m (mean %.2f)' % (worst_gap, worst_turn, min(steps), max(steps), sum(steps) / len(steps)))
    print('  width: %.0f..%.0f units (%.2f..%.2f lanes of %d)' % (wmin, wmax, wmin / LW, wmax / LW, LW))
    # clearance to the main ring (not this fork's throats)
    thr = 14
    main_rows = []
    for i in range(0, ring):
        if F - 8 <= i <= F + 1 + thr or R - 1 - thr <= i <= R + 2:
            continue
        t = S[i]['type']
        if t in (8, 11):
            continue
        main_rows.append((i, centre(row(S[i], 0))))
    best = (1e18, -1, -1)
    for k in range(len(cc)):
        if k < 12 or k > len(cc) - 13:
            continue
        for i, m in main_rows:
            d = dist(cc[k][0], m)
            if d < best[0]:
                best = (d, k, i)
    print('  clearance: nearest ring span centre to any corridor row (outside the throats): %.1f m (corridor step %d, ring span %d)' %
          (best[0] / UPM, best[1], best[2]))
    d0 = dist(cc[0][0], centre(row(S[F + 1], 0)))
    dl = dist(cc[-1][1], centre(row(S[R - 1], 1)))
    print('  mouths: corridor row 0 is %.2f m from the main half row at F+1; last row %.2f m from the main half at R-1' %
          (d0 / UPM, dl / UPM))
