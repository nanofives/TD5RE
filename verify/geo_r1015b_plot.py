#!/usr/bin/env python3
"""geo_r1015b_plot.py -- top-down plot of a generated geo level (round 1015 B).

MODELS.DAT triangles (coloured by page / MESHTAG kind, drawn lowest first so the surface a
straight-down camera sees ends up on top) over the real OSM ways (ROADS.JSON, black lines) and
the street network (NETWORK.JSON edges, magenta) and the race spans (cyan).

  python verify/geo_r1015b_plot.py <level dir> <x> <z> <half-size> <out.png> [--px 1400]
         [--geo re/assets/geo/la_plata] [--mark x,z ...]

Units are world units (430 per metre, a span is 1500). Page colours: ROAD grey, SIDEWALK
light yellow, cross-street (R4_CROSS = 101) orange, ground green-brown, lawn bright green,
building/facade kinds dark red, everything else mid blue.
"""
import json, math, os, struct, sys
from PIL import Image, ImageDraw

HDR, CMD, VTX = 0x38, 16, 44
GK = ["other", "skirt", "road", "branchroad", "tunnel", "gantry", "endwall",
      "deck", "water", "coast", "decal", "city", "block", "cross", "flora",
      "parktree", "terrain", "building", "prop", "rail", "branchside"]


def load_level(lvl):
    b = open(os.path.join(lvl, 'MODELS.DAT'), 'rb').read()
    tag = open(os.path.join(lvl, 'MESHTAG.BIN'), 'rb').read()
    _m, _v, _s, _n, stride = struct.unpack_from('<IIIII', tag, 0)
    tg = tag[20:]
    (n,) = struct.unpack_from('<I', b, 0)
    out = []
    for e in range(n):
        eo, _es = struct.unpack_from('<II', b, 4 + e * 8)
        if eo <= 0 or eo >= len(b):
            continue
        (nm,) = struct.unpack_from('<I', b, eo)
        for s in range(nm):
            (mo,) = struct.unpack_from('<I', b, eo + 4 + s * 4)
            m = eo + mo
            _mg, flags, ncmd, nvtx = struct.unpack_from('<HHII', b, m)
            cmdoff, = struct.unpack_from('<I', b, m + 0x2C)
            vtxoff, = struct.unpack_from('<I', b, m + 0x30)
            cmds = []
            for c in range(ncmd):
                _d, page, _z, tri, quad, _z2 = struct.unpack_from(
                    '<HHIHHI', b, m + cmdoff + c * CMD)
                cmds.append((page, tri, quad))
            vs = [struct.unpack_from('<fff', b, m + vtxoff + v * VTX) for v in range(nvtx)]
            k = tg[e * stride + s] if s < stride and e * stride + s < len(tg) else 255
            out.append(dict(e=e, s=s, flags=flags, cmds=cmds, v=vs,
                            kind=GK[k] if k < len(GK) else 'k%d' % k))
    return out


def tris(m):
    out, i = [], 0
    for page, t, q in m['cmds']:
        for _ in range(t):
            out.append((page, m['v'][i:i + 3]))
            i += 3
        for _ in range(q):
            v = m['v'][i:i + 4]
            i += 4
            out.append((page, [v[0], v[1], v[2]]))
            out.append((page, [v[0], v[2], v[3]]))
    return out


def colour(page, kind):
    if kind in ('building', 'city', 'block', 'cross') and page not in (44, 101, 45, 0, 46):
        return (150, 60, 50)
    if page == 0 or kind in ('road', 'branchroad'):
        return (110, 110, 120)
    if page == 44:
        return (235, 225, 150)
    if page == 45:
        return (255, 255, 255)
    if page == 101:
        return (240, 150, 40)
    if page == 65:
        return (60, 200, 60)
    if page in (5, 2):
        return (120, 130, 80)
    if page == 46:
        return (200, 120, 200)
    return (90, 110, 200)


def main():
    a = [x for x in sys.argv[1:] if not x.startswith('--')]
    opt = {}
    marks = []
    argv = sys.argv[1:]
    i = 0
    while i < len(argv):
        if argv[i] == '--px':
            opt['px'] = int(argv[i + 1]); i += 2; a.remove(argv[i - 1])
        elif argv[i] == '--geo':
            opt['geo'] = argv[i + 1]; i += 2; a.remove(argv[i - 1])
        elif argv[i] == '--mark':
            marks.append(tuple(float(t) for t in argv[i + 1].split(','))); i += 2; a.remove(argv[i - 1])
        else:
            i += 1
    lvl, cx, cz, half, out = a[0], float(a[1]), float(a[2]), float(a[3]), a[4]
    px = opt.get('px', 1400)
    geo = opt.get('geo', 're/assets/geo/la_plata')
    sc = px / (2 * half)

    def P(x, z):
        return ((x - (cx - half)) * sc, (z - (cz - half)) * sc)

    img = Image.new('RGB', (px, px), (30, 30, 30))
    d = ImageDraw.Draw(img, 'RGBA')
    items = []
    for m in load_level(lvl):
        if m['flags'] & 0xff:
            continue
        for page, t in tris(m):
            xs = [p[0] for p in t]; zs = [p[2] for p in t]
            if max(xs) < cx - half or min(xs) > cx + half or max(zs) < cz - half or min(zs) > cz + half:
                continue
            items.append((sum(p[1] for p in t) / 3.0, page, m['kind'], t))
    items.sort(key=lambda q: q[0])
    for y, page, kind, t in items:
        d.polygon([P(p[0], p[2]) for p in t], fill=colour(page, kind) + (255,))
    try:
        roads = json.load(open(os.path.join(geo, 'ROADS.JSON')))['roads']
        for r in roads:
            pts = [P(p['x'], p['z']) for p in r['points']]
            if len(pts) > 1:
                d.line(pts, fill=(0, 0, 0, 255), width=1)
    except Exception as ex:
        print('roads:', ex)
    nj = os.path.join(lvl, 'NETWORK.JSON')
    if os.path.exists(nj):
        n = json.load(open(nj))
        for e in n['edges']:
            d.line([P(p[0], p[1]) for p in e['poly']], fill=(255, 0, 255, 255), width=2)
        for s in n['spans']:
            x, z = P(s[1], s[2])
            d.ellipse([x - 2, z - 2, x + 2, z + 2], outline=(0, 255, 255, 255))
            if (cx - half) < s[1] < (cx + half) and (cz - half) < s[2] < (cz + half) and s[0] % 4 == 0:
                d.text((x + 4, z), str(s[0]), fill=(0, 255, 255, 255))
    for (mx, mz) in marks:
        x, z = P(mx, mz)
        d.ellipse([x - 7, z - 7, x + 7, z + 7], outline=(255, 0, 0, 255), width=2)
    # 100 m scale bar
    d.line([(10, px - 10), (10 + 43000 * sc, px - 10)], fill=(255, 255, 255, 255), width=3)
    d.text((12, px - 24), '100 m', fill=(255, 255, 255, 255))
    img.save(out)
    print('wrote', out, 'tris', len(items))


if __name__ == '__main__':
    main()
