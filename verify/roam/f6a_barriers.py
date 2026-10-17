#!/usr/bin/env python
"""f6a_barriers.py -- GEO FREE ROAM F6-A helper for the route-barrier census.

  python verify/roam/f6a_barriers.py plot  <json> <png> [--zoom lat,lon,metres]
      top-down map of a roam_barrier_<route>.json (route polyline + barriers, PLACE frame)
  python verify/roam/f6a_barriers.py poses <json> <out_poses.txt> [--n 6] [--ring]
      free-cam tour poses (name:lat,lon,height_m,yaw_deg,pitch) at junctions the route passes:
      a chase pose 22 m before each junction looking along the route and a top-down pose over it.
      --ring prefers junctions where the route follows a roundabout (>= 3 barriers in a small cluster)

The json is written by `TD5RE_ROAM_BARRIER_TEST=1 td5re.exe` (td5_roam_barriers_dev_test).
"""
import json, math, os, sys, collections

PLACE = os.path.join(os.path.dirname(__file__), '..', '..', 're', 'assets', 'geo', 'la_plata_partido', 'PLACE.JSON')
UPM = 430.0


def projection():
    txt = open(PLACE, encoding='utf-8').read()
    j = json.loads(txt)
    pr = j['projection']
    return pr['lat0'], pr['lon0'], pr['m_per_deg_lat'], pr['m_per_deg_lon']


def to_latlon(x, z):
    lat0, lon0, mlat, mlon = projection()
    return lat0 + z / UPM / mlat, lon0 + x / UPM / mlon


def clusters(d):
    byc = collections.defaultdict(list)
    for b in d['barriers']:
        byc[b['ck']].append(b)
    return byc


def centre(bs):
    xs = [(b['x0'] + b['x1']) / 2 for b in bs]
    zs = [(b['z0'] + b['z1']) / 2 for b in bs]
    return sum(xs) / len(xs), sum(zs) / len(zs)


def route_dir_near(route, x, z):
    best, bi = 1e30, 0
    for i in range(len(route) - 1):
        mx, mz = (route[i][0] + route[i + 1][0]) / 2, (route[i][1] + route[i + 1][1]) / 2
        dd = (mx - x) ** 2 + (mz - z) ** 2
        if dd < best:
            best, bi = dd, i
    dx, dz = route[bi + 1][0] - route[bi][0], route[bi + 1][1] - route[bi][1]
    l = math.hypot(dx, dz) or 1.0
    return dx / l, dz / l, bi


def cmd_plot(a):
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    d = json.load(open(a[0]))
    fig, ax = plt.subplots(figsize=(11, 11), dpi=110)
    r = d['route']
    ax.plot([p[0] / UPM for p in r], [p[1] / UPM for p in r], '-', color='#1b6fd8', lw=2.2, label='route')
    cols = {0: ('#2ca02c', 'arm taken by route'), 2: ('#9467bd', 'arm linked (contained)'), 3: ('#000000', 'arm OPEN')}
    seen = set()
    for a_ in d.get('armdbg', []):
        if a_['s'] in cols:
            col, lab = cols[a_['s']]
            ax.plot([a_['x0'] / UPM, a_['x1'] / UPM], [a_['z0'] / UPM, a_['z1'] / UPM], '-o' if a_['x0'] == a_['x1'] else '-', color=col, lw=2.0 if a_['s'] == 0 else 3.0, ms=8,
                    label=None if a_['s'] in seen else lab)
            seen.add(a_['s'])
    first = {0: True, 1: True}
    for b in d['barriers']:
        col = '#e8401c' if b['kind'] == 0 else '#e0a800'
        lab = ('mouth barrier' if b['kind'] == 0 else 'turn board') if first[b['kind']] else None
        first[b['kind']] = False
        ax.plot([b['x0'] / UPM, b['x1'] / UPM], [b['z0'] / UPM, b['z1'] / UPM], '-', color=col, lw=3.2 if b['kind'] == 0 else 4.5, label=lab)
    if len(a) > 3 and a[2] == '--zoom':
        lat, lon, m = [float(v) for v in a[3].split(',')]
        lat0, lon0, mlat, mlon = projection()
        cx, cz = (lon - lon0) * mlon, (lat - lat0) * mlat
        ax.set_xlim(cx - m, cx + m)
        ax.set_ylim(cz - m, cz + m)
    ax.set_aspect('equal')
    ax.grid(alpha=0.25)
    ax.set_title('%s  route %.0f m, %d edges, %d barriers' % (os.path.basename(a[0]), d['length_m'], d['edges'], len(d['barriers'])))
    ax.legend(loc='upper right')
    fig.savefig(a[1])
    print('wrote', a[1])


def cmd_poses(a):
    d = json.load(open(a[0]))
    n = 6
    if '--n' in a:
        n = int(a[a.index('--n') + 1])
    byc = clusters(d)
    items = []
    for ck, bs in byc.items():
        cx, cz = centre(bs)
        dx, dz, bi = route_dir_near(d['route'], cx, cz)
        items.append((bi, ck, cx, cz, dx, dz, len(bs), sum(1 for b in bs if b['kind'] == 1)))
    items.sort()
    if '--ring' in a:
        items.sort(key=lambda t: (-t[6], t[0]))
    # spread the picks along the route
    pick = []
    if '--ring' in a:
        pick = items[:n]
    else:
        step = max(1, len(items) // n)
        pick = items[::step][:n]
    out = []
    name = os.path.splitext(os.path.basename(a[0]))[0].replace('roam_barrier_', '')
    for k, (bi, ck, cx, cz, dx, dz, nb, nt) in enumerate(pick):
        # the route point nearest the cluster, then 22 m back along the route direction
        bx, bz = cx - dx * 22 * UPM, cz - dz * 22 * UPM
        yaw = math.degrees(math.atan2(dx, dz))          # 0 = +Z (north), toward +X
        la, lo = to_latlon(bx, bz)
        out.append('%s_j%d_chase:%.7f,%.7f,3.2,%.2f,-7' % (name, k, la, lo, yaw))
        la, lo = to_latlon(cx, cz)
        out.append('%s_j%d_top:%.7f,%.7f,70,%.2f,-89' % (name, k, la, lo, yaw))
        print('junction %d: cluster %s barriers %d boards %d at %.6f,%.6f dir (%.2f,%.2f) route pt %d' % (k, ck, nb, nt, la, lo, dx, dz, bi))
    open(a[1], 'w').write('\n'.join(out) + '\n')
    print('wrote', a[1], len(out), 'poses')


if __name__ == '__main__':
    if len(sys.argv) < 4:
        print(__doc__)
        sys.exit(1)
    {'plot': cmd_plot, 'poses': cmd_poses}[sys.argv[1]](sys.argv[2:])
