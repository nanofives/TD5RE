#!/usr/bin/env python3
"""r1023e_minimap_phantom.py -- minimap streets vs BUILT carriageway (round 1023 E, item 11).

The in-race minimap (td5_minimap_streets.c) draws every OSM way of ROADS.JSON that lies in the
route corridor. Most of those ways have no 3D road: the generator only builds the route, the
fork corridors and the NETWORK.JSON side streets. This script counts them offline.

BUILT = a carriageway triangle in MODELS.DAT: page 0 (ROAD) or page 101 (R4_CROSS street), any
mesh kind, flags&0xff == 0. The raster is dilated by --dilate cells so an OSM centreline that
runs along a built surface counts as built.

  python verify/r1023e_minimap_phantom.py <level dir> <roads dir> [--cell 1000] [--dilate 2]
         [--window 58880] [--json out.json] [--png out.png] [--near x,z,half]

Reports, for the chords the minimap would draw inside --window of a race span:
  total / built / unbuilt chords and length, by OSM class bucket (major/street/minor, the same
  buckets mms_rank_for_osm uses), plus the unbuilt ways ranked by length.
"""
import importlib.util, json, math, os, sys
import numpy as np
from PIL import Image, ImageDraw
from scipy import ndimage

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location('r1015b_plot', os.path.join(HERE, 'geo_r1015b_plot.py'))
plot = importlib.util.module_from_spec(spec)
spec.loader.exec_module(plot)

KLASS = {'service': 1, 'living_street': 2, 'residential': 3, 'unclassified': 4, 'tertiary': 5,
         'secondary': 6, 'primary': 7, 'trunk': 8, 'motorway': 9}
CARRIAGE_PAGES = (0, 101)
MIN_CHORD = 40.0


def bucket(cls):
    k = KLASS.get(cls.replace('_link', ''), 0)
    return 'major' if k >= 6 else ('street' if k >= 3 else 'minor')


def main():
    a = [x for x in sys.argv[1:] if not x.startswith('--')]
    opt = {'cell': 1000.0, 'dilate': 2, 'window': 58880.0, 'json': None, 'png': None, 'near': None}
    argv = sys.argv[1:]
    i = 0
    while i < len(argv):
        if argv[i].startswith('--'):
            k = argv[i][2:]
            v = argv[i + 1]
            opt[k] = float(v) if k in ('cell', 'window') else (int(v) if k == 'dilate' else v)
            a.remove(v)
            i += 2
        else:
            i += 1
    lvl, roads_dir = a[0], a[1]
    cell = opt['cell']
    nj = json.load(open(os.path.join(lvl, 'NETWORK.JSON')))
    sp = [(s[0], s[1], s[2]) for s in nj['spans']]
    pad = opt['window'] + 4 * cell
    x0 = min(s[1] for s in sp) - pad
    z0 = min(s[2] for s in sp) - pad
    W = int((max(s[1] for s in sp) + pad - x0) / cell) + 2
    H = int((max(s[2] for s in sp) + pad - z0) / cell) + 2

    cov = Image.new('L', (W, H), 0)
    d = ImageDraw.Draw(cov)
    ntri = 0
    for m in plot.load_level(lvl):
        if m['flags'] & 0xff:
            continue
        for page, t in plot.tris(m):
            if page in CARRIAGE_PAGES:
                d.polygon([((p[0] - x0) / cell, (p[2] - z0) / cell) for p in t], fill=255)
                ntri += 1
    cov = np.array(cov) > 0
    built = ndimage.binary_dilation(cov, iterations=opt['dilate'])

    win = Image.new('L', (W, H), 0)
    dw = ImageDraw.Draw(win)
    r = opt['window'] / cell
    for (_i, x, z) in sp:
        cx, cz = (x - x0) / cell, (z - z0) / cell
        dw.ellipse([cx - r, cz - r, cx + r, cz + r], fill=255)
    win = np.array(win) > 0

    def cellof(x, z):
        return int((x - x0) / cell), int((z - z0) / cell)

    def inb(cx, cz):
        return 0 <= cx < W and 0 <= cz < H

    roads = json.load(open(os.path.join(roads_dir, 'ROADS.JSON'), encoding='utf-8'))['roads']
    stats = {}
    unbuilt = []
    n_ways = 0
    for ri, r_ in enumerate(roads):
        pts = [(p['x'], p['z']) for p in r_['points']]
        qx, qz = pts[0]
        way_len = way_un = 0.0
        way_chords = way_unchords = 0
        for (px, pz) in pts[1:]:
            L = math.hypot(px - qx, pz - qz)
            if L < MIN_CHORD:
                continue
            mx, mz = (qx + px) / 2, (qz + pz) / 2
            cx, cz = cellof(mx, mz)
            if inb(cx, cz) and win[cz, cx]:
                # sample every ~cell along the chord, built when >= 60% of samples are covered
                n = max(1, int(L / cell))
                hit = 0
                for k in range(n):
                    t = (k + 0.5) / n
                    sx, sz = cellof(qx + (px - qx) * t, qz + (pz - qz) * t)
                    if inb(sx, sz) and built[sz, sx]:
                        hit += 1
                ok = hit >= 0.6 * n
                b = bucket(r_['class'])
                s = stats.setdefault(b, dict(chords=0, built=0, len=0.0, built_len=0.0))
                s['chords'] += 1
                s['len'] += L
                if ok:
                    s['built'] += 1
                    s['built_len'] += L
                else:
                    way_un += L
                    way_unchords += 1
                way_len += L
                way_chords += 1
            qx, qz = px, pz
        if way_chords:
            n_ways += 1
        if way_unchords:
            unbuilt.append((way_un, ri, r_['name'], r_['class'], way_len, way_unchords, way_chords))
    out = dict(carriage_tris=ntri, ways_in_window=n_ways, by_class=stats)
    tot = dict(chords=0, built=0, len=0.0, built_len=0.0)
    for s in stats.values():
        for k in tot:
            tot[k] += s[k]
    out['total'] = tot
    unbuilt.sort(reverse=True)
    out['top_unbuilt'] = [dict(way=w[1], name=w[2], cls=w[3], unbuilt_m=round(w[0] / 430), len_m=round(w[4] / 430),
                               unbuilt_chords=w[5], chords=w[6]) for w in unbuilt[:15]]
    print(json.dumps(out, indent=1, ensure_ascii=False))
    if opt['json']:
        json.dump(out, open(opt['json'], 'w'), indent=1)


if __name__ == '__main__':
    main()
