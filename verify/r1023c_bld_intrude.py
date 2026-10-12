#!/usr/bin/env python3
"""[R1023 C] Every building / facade mesh of a generated geo level that INTRUDES on a carriageway or
a pavement band, measured offline from MODELS.DAT + MESHTAG.BIN only.

  python verify/r1023c_bld_intrude.py LEVELDIR [--json out.json] [--top 30] [--hits 2] [--mesh e,s ...]

SURFACES (plan-view quads, horizontal only):
  carriageway  page 0 (ROAD) or 101 (R4_CROSS side street) of ANY kind except the ground skirt,
               i.e. the race road, a fork corridor / branch road, every laid side street and ring ribbon
  pavement     page 44 (SIDEWALK) of ANY kind, horizontal faces only (slab tops, not kerb faces)

BUILDINGS: every mesh of kind city / block / cross / building that owns a VERTICAL face taller than
1000 units (2.3 m). Sample points:
  wall   the bottom edge of every vertical face, every ~400 units
  roof   vertices + centroid of every horizontal face that sits >= 1500 units above the lowest wall base

A sample is a HIT when it lies INSIDE a surface quad (plan view, `inset` units from its edge) and the
surface height passes through the wall's vertical extent (ymin - 500 <= y_surface <= ymax - 1000), so a
bridge deck over a low building, or a building over an underpass, does not count.
A mesh INTRUDES when it has >= --hits (default 2) distinct hit samples on that surface class.

Output: counts per (kind x class) and the worst offenders as pick-style strings, plus a JSON ledger.
"""
from PIL import Image, ImageDraw
import argparse, collections, json, math, os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import geo_bld_audit as A

CELL = 6000.0
BLD_KINDS = ("city", "block", "cross", "building")
CARR_PAGES = (0, 101)
PAVE_PAGE = 44
SKIP_SURF_KINDS = ("skirt",)
STEP = 400.0
GRADE = 2500.0                 # a surface within this of the wall base is 'at grade' under the building
MIN_WALL_AREA = 3.0e6          # units^2 (~16 m^2 of wall)
MINH = 1000.0


def inside_quad(q, x, z, inset):
    """Point-in-convex-quad (any winding) with an inset margin from every edge."""
    ar = 0.0
    for i in range(4):
        ar += q[i][0] * q[(i + 1) % 4][1] - q[(i + 1) % 4][0] * q[i][1]
    sgn = 1.0 if ar >= 0 else -1.0
    for i in range(4):
        ax, az = q[i]; bx, bz = q[(i + 1) % 4]
        ex, ez = bx - ax, bz - az
        L = math.hypot(ex, ez)
        if L < 1e-6:
            continue
        c = sgn * (ex * (z - az) - ez * (x - ax)) / L      # signed distance from the edge line, + = inside
        if c < inset:
            return False
    return True


class Surf:
    def __init__(self):
        self.g = collections.defaultdict(list)

    def add(self, q, y, tag=""):
        xs = [p[0] for p in q]; zs = [p[1] for p in q]
        item = (q, y, tag)
        for gx in range(int(min(xs) // CELL), int(max(xs) // CELL) + 1):
            for gz in range(int(min(zs) // CELL), int(max(zs) // CELL) + 1):
                self.g[(gx, gz)].append(item)

    def quads_in(self, x0, z0, x1, z1):
        seen = {}
        for gx in range(int(x0 // CELL), int(x1 // CELL) + 1):
            for gz in range(int(z0 // CELL), int(z1 // CELL) + 1):
                for it in self.g.get((gx, gz), ()):
                    seen[id(it)] = it
        return seen.values()

    def hit(self, x, z, ylo, yhi, inset):
        for q, y, tag in self.g.get((int(x // CELL), int(z // CELL)), ()):
            if ylo <= y <= yhi and inside_quad(q, x, z, inset):
                return tag or True
        return False


def horizontal(vs):
    q = [(v[0], v[2]) for v in vs[:4]]
    area = abs(A.poly_area(q))
    if area < 1.0:
        return None
    ys = [v[1] for v in vs[:4]]
    if max(ys) - min(ys) > 0.25 * math.sqrt(area):
        return None
    return q, sum(ys) / 4.0


def is_billboard(b, m):
    """flags & 0xff != 0 = a camera-facing sprite whose vertices are RELATIVE to the header centre."""
    return (struct.unpack_from("<H", b, m["moff"] + 2)[0] & 0xff) != 0


PLAZA_W = 3600.0   # a pavement quad wider than this (8.4 m) is a plaza / apron FLOOR, not a sidewalk band


def narrow(q):
    """Shorter side of the quad's two opposite-edge pairs <= PLAZA_W: a sidewalk or a pavement arm."""
    d = lambda a, b: math.hypot(q[a][0] - q[b][0], q[a][1] - q[b][1])
    return min((d(0, 1) + d(2, 3)) * 0.5, (d(1, 2) + d(3, 0)) * 0.5) <= PLAZA_W


def load_surfaces(b, meshes):
    carr, pave = Surf(), Surf()
    for m in meshes:
        if is_billboard(b, m):
            continue
        for typ, page, vs in A.faces(b, m):
            if len(vs) != 4:
                continue
            if page in CARR_PAGES and m["kind"] not in SKIP_SURF_KINDS:
                h = horizontal(vs)
                if h:
                    carr.add(h[0], h[1], "%s/p%d" % (m["kind"], page))
            elif page == PAVE_PAGE:
                h = horizontal(vs)
                if h and narrow(h[0]):
                    pave.add(h[0], h[1], "%s/p44" % m["kind"])
    return carr, pave


def samples_of(b, m):
    """[(x, z, ymin, ymax, 'wall'|'roof')] for one building mesh, or None if it owns no tall wall."""
    walls, roofs = [], []
    for typ, page, vs in A.faces(b, m):
        if len(vs) not in (3, 4):
            continue
        ys = [v[1] for v in vs]
        ymin, ymax = min(ys), max(ys)
        pts = [(v[0], v[2]) for v in vs]
        ext = max(math.hypot(p[0] - pts[0][0], p[1] - pts[0][1]) for p in pts)
        if ymax - ymin >= MINH and (len(vs) == 3 or abs(A.poly_area(pts)) < 0.04 * max(ext, 1.0) ** 2 + 4.0e4):
            low = sorted(vs, key=lambda v: v[1])[:2]
            walls.append(((low[0][0], low[0][2]), (low[1][0], low[1][2]), ymin, ymax))
        elif ymax - ymin < 0.25 * max(ext, 1.0):
            roofs.append((pts, sum(ys) / len(ys)))
    if not walls:
        return None
    # one bottom edge per plan segment: the stacked rows of a facade grid share it, keep the lowest
    low = {}
    for w in walls:
        key = (round(w[0][0] / 50), round(w[0][1] / 50), round(w[1][0] / 50), round(w[1][1] / 50))
        if key not in low or w[2] < low[key][2]:
            low[key] = w
    top = max(w[3] for w in walls)
    walls = [(w[0], w[1], w[2], top) for w in low.values()]
    # a building owns real wall AREA: a lone billboard / sign quad (nv <= 4) is furniture, not a building
    wa = sum(math.hypot(w[1][0] - w[0][0], w[1][1] - w[0][1]) * (w[3] - w[2]) for w in walls)
    if m["nv"] <= 4 or wa < MIN_WALL_AREA:
        return None
    wy0 = min(w[2] for w in walls); wy1 = max(w[3] for w in walls)
    out = []
    for p0, p1, ymin, ymax in walls:
        L = math.hypot(p1[0] - p0[0], p1[1] - p0[1])
        n = max(1, int(L / STEP))
        for i in range(n + 1):
            t = i / float(n)
            out.append((p0[0] + (p1[0] - p0[0]) * t, p0[1] + (p1[1] - p0[1]) * t, ymin, ymax, "wall"))
    for pts, y in roofs:
        if y < wy0 + 1500.0:
            continue
        cx = sum(p[0] for p in pts) / len(pts); cz = sum(p[1] for p in pts) / len(pts)
        for (x, z) in pts + [(cx, cz)]:
            out.append((x, z, wy0, wy1, "roof"))
    return out


def bclass(m):
    c = A.classify(m)
    if c:
        return c
    cm = m["cmds"]
    if m["kind"] == "city" and len(cm) == 1 and 273 <= cm[0][0] <= 392:
        return "flank-box"
    if m["kind"] == "city" and len(cm) >= 2:
        return "landmark-ish"
    if m["kind"] == "cross":
        return "x-frontage"
    if m["kind"] == "block":
        return "block"
    return "other"


RAS = 100.0   # raster cell, units (0.23 m)


def footprint_overlap(b, m, base, carr, pave):
    """Plan area (m^2) of the building's ROOF (its footprint) lying on a carriageway / a pavement quad
    that stands at grade under it. None when the mesh has no roof faces (a zero-thickness facade sheet)."""
    roofs = []
    for typ, page, vs in A.faces(b, m):
        ys = [v[1] for v in vs]
        pts = [(v[0], v[2]) for v in vs]
        if len(vs) in (3, 4) and min(ys) >= base + 1500.0 and max(ys) - min(ys) < 400.0 and abs(A.poly_area(pts)) > 1.0:
            roofs.append(pts)
    if not roofs:
        return None
    xs = [p[0] for r in roofs for p in r]; zs = [p[1] for r in roofs for p in r]
    x0, z0, x1, z1 = min(xs), min(zs), max(xs), max(zs)
    W = int((x1 - x0) / RAS) + 2; H = int((z1 - z0) / RAS) + 2
    if W * H > 4.0e6:
        return None
    foot = Image.new("L", (W, H), 0); d = ImageDraw.Draw(foot)
    for r in roofs:
        d.polygon([((p[0] - x0) / RAS, (p[1] - z0) / RAS) for p in r], fill=1)
    out = []
    for surf, inset in ((carr, 100.0), (pave, 100.0)):
        mk = Image.new("L", (W, H), 0); dm = ImageDraw.Draw(mk); used = 0
        for q, y, tag in surf.quads_in(x0, z0, x1, z1):
            if not (base - 500.0 <= y <= base + GRADE):
                continue
            dm.polygon([((p[0] - x0) / RAS, (p[1] - z0) / RAS) for p in q], fill=1); used += 1
        if not used:
            out.append(0.0); continue
        f = foot.load(); k = mk.load(); n = 0
        fb = foot.tobytes(); kb = mk.tobytes()
        n = sum(1 for i in range(len(fb)) if fb[i] and kb[i])
        out.append(n * RAS * RAS / (430.0 * 430.0))
    return out


def run(lvl, minhits=2, only=None, cinset=150.0, pinset=300.0):
    b, meshes = A.load_level(lvl)
    carr, pave = load_surfaces(b, meshes)
    rows = []
    for m in meshes:
        if m["kind"] not in BLD_KINDS or is_billboard(b, m):
            continue
        if only and (m["e"], m["s"]) not in only:
            continue
        sm = samples_of(b, m)
        if not sm:
            continue
        hc = hp = ht = 0
        sk = collections.Counter()
        for (x, z, ymin, ymax, what) in sm:
            ylo, yhi = ymin - 500.0, ymin + GRADE      # the building stands ON this surface
            t = carr.hit(x, z, ylo, yhi, cinset)
            if t:
                hc += 1; sk["C:" + str(t)] += 1
                continue
            t = pave.hit(x, z, ylo, yhi, pinset)
            if t:
                hp += 1; sk["P:" + str(t)] += 1
                continue
            # an ELEVATED surface (overpass deck / ramp pavement) passing through the tower above its base
            t = carr.hit(x, z, ymin + GRADE, ymax - 1000.0, cinset) or pave.hit(x, z, ymin + GRADE, ymax - 1000.0, pinset)
            if t:
                ht += 1; sk["T:" + str(t)] += 1
        base = min(v[1] for v in A.verts(b, m))
        fo = footprint_overlap(b, m, base, carr, pave) if m["kind"] in ("city", "building") else None
        rows.append(dict(fc=(fo[0] if fo else None), fp=(fo[1] if fo else None), e=m["e"], s=m["s"], kind=m["kind"], cls=bclass(m),
                         page=m["cmds"][0][0] if m["cmds"] else -1,
                         c=[round(v) for v in m["c"]], r=round(m["r"]), v=m["nv"], nc=len(m["cmds"]),
                         samples=len(sm), carr=hc, pave=hp, thru=ht, surf=dict(sk)))
    return rows


FOOT_CARR_M2 = 2.0      # roof footprint on a carriageway (a kerb graze is < 1 m^2)
FOOT_PAVE_M2 = 8.0      # roof footprint on a pavement band


def on_carr(r, minhits):
    return r["carr"] >= minhits or (r.get("fc") or 0.0) >= FOOT_CARR_M2


def on_pave(r, minhits):
    return r["pave"] >= minhits or (r.get("fp") or 0.0) >= FOOT_PAVE_M2


def summarize(rows, minhits):
    tot = collections.Counter(); car = collections.Counter(); pav = collections.Counter()
    for r in rows:
        k = (r["kind"], r["cls"])
        tot[k] += 1
        if on_carr(r, minhits):
            car[k] += 1
        if on_pave(r, minhits):
            pav[k] += 1
    return tot, car, pav


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("level")
    ap.add_argument("--json")
    ap.add_argument("--top", type=int, default=30)
    ap.add_argument("--cinset", type=float, default=150.0, help="units a sample must be INSIDE a carriageway quad")
    ap.add_argument("--pinset", type=float, default=300.0, help="units a sample must be INSIDE a pavement quad")
    ap.add_argument("--hits", type=int, default=2)
    ap.add_argument("--mesh", nargs="*", default=[])
    a = ap.parse_args()
    only = set(tuple(int(t) for t in s.split(",")) for s in a.mesh) or None
    rows = run(a.level, a.hits, only, a.cinset, a.pinset)
    tot, car, pav = summarize(rows, a.hits)
    print("building/facade meshes with a tall wall: %d" % len(rows))
    print("  %-22s %7s %9s %9s" % ("kind/class", "meshes", "on-carr", "on-pave"))
    for k in sorted(tot):
        print("  %-22s %7d %9d %9d" % ("%s/%s" % k, tot[k], car[k], pav[k]))
    nc = sum(car.values()); npv = sum(pav.values())
    both = sum(1 for r in rows if on_carr(r, a.hits) or on_pave(r, a.hits))
    print("TOTAL intruding on a carriageway: %d   on a pavement band: %d   either: %d" % (nc, npv, both))
    bad = sorted((r for r in rows if on_carr(r, a.hits) or on_pave(r, a.hits)),
                 key=lambda r: -(r["carr"] * 3 + r["pave"] + 3 * (r.get("fc") or 0) + (r.get("fp") or 0)))
    for r in bad[:a.top]:
        print("  e%-4d s%-3d %-8s %-10s p%-4d pos %d,%d,%d r%d v%d  carr=%d pave=%d / %d samples  foot c=%s p=%s m2  %s"
              % (r["e"], r["s"], r["kind"], r["cls"], r["page"], r["c"][0], r["c"][1], r["c"][2],
                 r["r"], r["v"], r["carr"], r["pave"], r["samples"],
                 "-" if r.get("fc") is None else "%.1f" % r["fc"], "-" if r.get("fp") is None else "%.1f" % r["fp"],
                 ",".join(sorted(r["surf"]))[:50]))
    if a.json:
        json.dump(dict(hits=a.hits, rows=rows), open(a.json, "w"))


if __name__ == "__main__":
    main()
