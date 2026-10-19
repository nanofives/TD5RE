#!/usr/bin/env python
"""GEO FREE ROAM F1-T7: coverage and seam audit over the casco ROAMTILE files.

Reads every ROAMTILE_<tx>_<tz>.BIN under --tiles. The structure gate and the section
layout come from validate_tiles.py (T5's reader, imported, not copied). Then:

  (1) COVERAGE. Every drivable way of the ROADS tiles whose polyline meets the casco
      polygon must own >= 1 quad (an EDGE record with span_count >= 1, key >> 16 = way id)
      or lie inside a junction patch (every sample point inside a TRI of some tile),
      or [F1-T9] be an absorbed connector: its centreline, sampled every metre, within
      COVER_TOL (1 m) of the drivable union (any tile's ribbon quads or patch triangles).
      Drivable = the td5_roam_world.c filter: td5_roam_way_drop_reason (:56-69: no id,
      class bus_stop/razed/services/raceway, tags.area=yes, service parking_aisle /
      drive-through), the dup-id rule (:340-349, first kept) and the degenerate rule
      (:367-381: collapse equal 10-unit cells, < 2 points left). Tag facts follow
      td5_roam_cache.c:166-186.
  (2) SEAMS. Two checks. (a) Lattice line: every row vertex within SEAM_TOL of a tile side
      (lattice u or v) must have an identical x,y,z partner in the built neighbour across
      that side, when the neighbour holds any vertex within REACH_TOL of it ([F1-T10]: a
      near miss is a crack; a vertex the neighbour has nothing near -- a kerb lying ON the
      border, a quad straddling it that this tile owns -- has nothing to share and is
      counted as informational). (b) Closure: an open edge whose two ends are both held by another tile must
      be open in that tile too (topology mismatch). Quads straddle the lattice line, so (a)
      is strict and (b) is the real seam test.
  (3) SHORT EDGES. No open edge (count 1 in one tile's mesh, kerb walls included) shorter
      than 1 cm = 4.3 units (TD5_ROAM_UPM = 430 units per metre, td5_roam_world.h:65).
  (4) OPEN GAPS. An open edge with no partner in any other tile, that is not a kerb (BEDG),
      not on the outline of a HOLE cluster (NODE flag 0x800, T3b, walled), and not on the
      outer edge of the built set, is a gap. Each gap is classified by the EDGE end it sits
      on (DEAD / JOIN / CLUSTER, EDGE.end_kind). Also every ARM cut row (one of the two rows
      of its cut span, all L segments) must be an edge of the patch triangulation or a kerb.
  (4b) REPORTED, NOT IN TOTAL. Non-seam open edges whose midpoint lies strictly inside a
      different drivable prim (a lane quad or patch triangle that does not contain the edge):
      overlapping carriageways and side-by-side split rows.

  (4c) [F1-T9] COVERED GAPS, NOT IN TOTAL. An open gap edge whose far side (probed a few
      units across, away from its own prim) is drivable surface of another prim within
      STEP_TOL in height: the wall rule is the boundary of the UNION, so this is no hole.
  [F1-T9] A gap whose partner may live in a tile that was not built (a tile with an
  unbuilt neighbour) counts as "edge of the built set", like `outer`. --inner x0,z0,x1,z1
  counts gaps / arm rows in those tiles only: build a one-tile ring round the set and audit
  with --inner = the original set to prove those are only the edge.
  [F1-T9] A dead-end cap is ONE BEDG across the whole row; it walls every lane segment.

Outputs audit_coverage.txt and casco_coverage_audit.png under --out (stdlib PNG writer). The
PNG draws drivable ways grey, uncovered red, gaps orange, (4b) purple, seam mismatches magenta x.

Usage:  python -I verify/roam/audit_coverage.py [--tiles DIR] [--roads DIR] [--grid FILE]
                                              [--poly FILE] [--out DIR] [--no-png]
                                              [--inner x0,z0,x1,z1]
Exit 0 when every check is 0 and the structure gate is clean, else 1.
"""
import glob
import json
import math
import os
import struct
import sys
import zlib
from collections import Counter, defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import validate_tiles as vt  # noqa: E402  T5 reader: HDR, ORDER, check() structure gate

REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
MAIN = "C:/Users/maria/Desktop/Proyectos/TD5RE"  # _archive/ exists in the main checkout only
PLACE_DIR = os.path.join("re", "assets", "geo", "la_plata_partido")
UPM = 430.0              # TD5_ROAM_UPM, world units per metre (td5_roam_world.h:65)
CM = UPM / 100.0         # 1 cm in units
SEAM_TOL = 2.0           # units: the rounded lattice corner moves a border row by <= 0.5 unit
REACH_TOL = 430.0        # [F1-T10] units: a neighbour vertex this near a border vertex = a crack
SAMPLE_TOL = 2.0         # units: ROADS points (float) against integer patch triangles
CELL = 21500             # spatial index cell, 50 m
PROBE = 8.0              # (4c) units across an open edge (~2 cm)
COVER_TOL = 430.0        # (1) an absorbed way's centreline within 1 m of the drivable union
STEP_TOL = 43.0          # (4c) a covering surface within 10 cm of the edge is continuous
DROP_CLASS = ("bus_stop", "razed", "services", "raceway")
DROP_SERVICE = ("parking_aisle", "drive-through")
TOP = 10


def ekey(p, q):
    return (p, q) if p <= q else (q, p)


def elen(e):
    return math.dist(e[0], e[1])


class Tile:
    pass


def load_tile(path, problems):
    if vt.check(path, True, problems) is None:
        return None
    with open(path, "rb") as f:
        buf = f.read()
    _, _, _, tx, tz, ox, oz, _ = struct.unpack_from("<IIIiiiiI", buf, 8)
    sec = {}
    for i, tag in enumerate(vt.ORDER):
        _, off, nb, _ = struct.unpack_from("<4sIII", buf, vt.HDR + 16 * i)
        sec[tag] = (off, nb)

    def recs(tag, fmt):
        off, nb = sec[tag]
        return list(struct.iter_unpack(fmt, buf[off:off + nb]))

    t = Tile()
    t.name = os.path.basename(path)
    t.key = (tx, tz)
    t.ox, t.oz = ox, oz
    t.verts = recs("VERT", "<hhh")
    t.spans = recs("SPAN", "<BB2sHHhhiii")
    t.tris = recs("TRI ", "<HHHHBBiii")
    t.bedg = recs("BEDG", "<HHHBB")
    t.edges = recs("EDGE", "<Qqq4BbBHiHHH2BBBH")
    t.arms = recs("ARM ", "<QhHBBBB")
    t.nodes = recs("NODE", "<qiiiBBHHHHH")
    return t


def vw(t, i, sx, sy, sz):
    """World (place-frame) position of VERT i under an owner origin (sx, sy, sz)."""
    x, y, z = t.verts[i]
    return (t.ox + sx + x, sy + y, t.oz + sz + z)


def tile_mesh(t):
    t.mesh = defaultdict(int)
    t.tri_e = set()
    t.bed = set()
    t.pts = set()
    t.span_geo = []
    t.faces = []         # drawable triangles (each lane quad split in two, plus patch tris)
    # Candidate int16 overflow: an offset stored at the int16 limit. A wrapped value
    # (census "int16 offset overflow") is not visible in the file, so this is a hint only.
    t.ovf_idx = {i for i, v in enumerate(t.verts) if any(c >= 32767 or c <= -32768 for c in v)}
    t.ovf_pts = set()
    for s in t.spans:
        L = s[2][1] & 0x0F
        o = (s[7], s[8], s[9])
        rowL = [vw(t, s[3] + k, *o) for k in range(L + 1)]
        rowR = [vw(t, s[4] + k, *o) for k in range(L + 1)]
        t.span_geo.append((L, rowL, rowR))
        t.pts.update(rowL)
        t.pts.update(rowR)
        for base, row in ((s[3], rowL), (s[4], rowR)):
            t.ovf_pts.update(p for k, p in enumerate(row) if base + k in t.ovf_idx)
        for k in range(L):
            a, b, c, d = rowL[k], rowL[k + 1], rowR[k + 1], rowR[k]
            for p, q in ((a, b), (b, c), (c, d), (d, a)):
                t.mesh[ekey(p, q)] += 1
            t.faces.append((a, b, c))
            t.faces.append((a, c, d))
    t.tri_w = []
    for tr in t.tris:
        o = (tr[6], tr[7], tr[8])
        p = [vw(t, tr[i], *o) for i in range(3)]
        t.tri_w.append(p)
        t.pts.update(p)
        t.faces.append(tuple(p))
        t.ovf_pts.update(p[j] for j in range(3) if tr[j] in t.ovf_idx)
        for x, y in ((p[0], p[1]), (p[1], p[2]), (p[2], p[0])):
            e = ekey(x, y)
            t.mesh[e] += 1
            t.tri_e.add(e)
    # T3b: a HOLE cluster's outline (unpaved island, walled) is a legit boundary.
    t.hole_pts = set()
    for n in t.nodes:
        if n[6] & 0x800:
            for i in range(n[7], n[7] + n[8]):
                t.hole_pts.update(t.tri_w[i])
    # Ribbon rows: which span/side owns a row segment, and which EDGE run a span belongs to.
    t.seg_owner = defaultdict(list)
    for si, (L, rowL, rowR) in enumerate(t.span_geo):
        for side, row in (("L", rowL), ("R", rowR)):
            for k in range(L):
                t.seg_owner[ekey(row[k], row[k + 1])].append((si, side))
    t.span_run = {}
    for rec in t.edges:
        for si in range(rec[11], rec[11] + rec[12]):
            t.span_run[si] = rec
    for b in t.bedg:
        prim = b[2]
        if prim & 0x8000:
            tr = t.tris[prim & 0x7FFF]
            o = (tr[6], tr[7], tr[8])
        else:
            s = t.spans[prim]
            o = (s[7], s[8], s[9])
            # [T9] A dead-end cap is ONE BEDG across the whole row (row[0] -> row[L],
            # td5_roam_tile.c slice_tile), while the mesh holds the row per lane.
            # Wall every lane segment the cap spans, or each one reads as a gap.
            L = s[2][1] & 0x0F
            lo, hi = min(b[0], b[1]), max(b[0], b[1])
            for base in (s[3], s[4]):
                if base <= lo and hi <= base + L and hi - lo > 1:
                    for k in range(lo, hi):
                        t.bed.add(ekey(vw(t, k, *o), vw(t, k + 1, *o)))
        t.bed.add(ekey(vw(t, b[0], *o), vw(t, b[1], *o)))


class Lattice:
    """ROAMGRID.JSON: u = c*(x-ox) + s*(z-oz), v = c*(z-oz) - s*(x-ox) (design 1.8)."""

    def __init__(self, path):
        with open(path, encoding="utf-8") as f:
            g = json.load(f)
        self.ox, self.oz = float(g["origin"][0]), float(g["origin"][1])
        self.c, self.s = float(g["c"]), float(g["s"])
        self.S = float(g["tile_units"])

    def uv(self, x, z):
        dx, dz = x - self.ox, z - self.oz
        return self.c * dx + self.s * dz, self.c * dz - self.s * dx

    def xz(self, u, v):
        return (self.ox + self.c * u - self.s * v, self.oz + self.s * u + self.c * v)

    def sides(self, tx, tz, p):
        """Tile sides of tile (tx,tz) that point p lies on: side name -> neighbour key."""
        u, v = self.uv(p[0], p[2])
        S = self.S
        out = {}
        if abs(u - tx * S) <= SEAM_TOL:
            out["U0"] = (tx - 1, tz)
        if abs(u - (tx + 1) * S) <= SEAM_TOL:
            out["U1"] = (tx + 1, tz)
        if abs(v - tz * S) <= SEAM_TOL:
            out["V0"] = (tx, tz - 1)
        if abs(v - (tz + 1) * S) <= SEAM_TOL:
            out["V1"] = (tx, tz + 1)
        return out


def load_poly(path):
    with open(path, encoding="utf-8") as f:
        doc = json.load(f)
    return [(float(c["x"]), float(c["z"])) for c in doc["corners"]]


def pt_in_poly(x, z, poly):
    inside = False
    j = len(poly) - 1
    for i in range(len(poly)):
        xi, zi = poly[i]
        xj, zj = poly[j]
        if (zi > z) != (zj > z) and x < (xj - xi) * (z - zi) / (zj - zi) + xi:
            inside = not inside
        j = i
    return inside


def seg_cross(a, b, c, d):
    def orient(p, q, r):
        return (q[0] - p[0]) * (r[1] - p[1]) - (q[1] - p[1]) * (r[0] - p[0])
    d1, d2 = orient(c, d, a), orient(c, d, b)
    d3, d4 = orient(a, b, c), orient(a, b, d)
    return ((d1 > 0 and d2 < 0) or (d1 < 0 and d2 > 0)) and \
           ((d3 > 0 and d4 < 0) or (d3 < 0 and d4 > 0))


def way_in_poly(pts, poly):
    if any(pt_in_poly(x, z, poly) for x, z in pts):
        return True
    n = len(poly)
    for a, b in zip(pts, pts[1:]):
        for i in range(n):
            if seg_cross(a, b, poly[i], poly[(i + 1) % n]):
                return True
    return False


def tri_prep(p):
    (ax, az), (bx, bz), (cx, cz) = p
    area = (bx - ax) * (cz - az) - (bz - az) * (cx - ax)
    if abs(area) < 1e-9:
        return None
    sgn = 1.0 if area > 0 else -1.0
    edges = []
    for (px, pz), (qx, qz) in (((ax, az), (bx, bz)), ((bx, bz), (cx, cz)), ((cx, cz), (ax, az))):
        edges.append((px, pz, qx - px, qz - pz, math.hypot(qx - px, qz - pz)))
    return sgn, edges


def tri_contains(prep, x, z):
    sgn, edges = prep
    for px, pz, ex, ez, ln in edges:
        if sgn * (ex * (z - pz) - ez * (x - px)) / ln < -SAMPLE_TOL:
            return False
    return True


def tri_contains_strict0(prep, x, z):
    """Inside or on the border (0.5 unit slack): the (4c) probe test."""
    sgn, edges = prep
    for px, pz, ex, ez, ln in edges:
        if sgn * (ex * (z - pz) - ez * (x - px)) / ln < -0.5:
            return False
    return True


def tri_strictly_inside(prep, x, z):
    """Inside by more than SAMPLE_TOL: a point on a shared border does not count."""
    sgn, edges = prep
    for px, pz, ex, ez, ln in edges:
        if sgn * (ex * (z - pz) - ez * (x - px)) / ln <= SAMPLE_TOL:
            return False
    return True


def fmt_pt(p):
    return "(%.1f, %.1f, %.1f)" % p


END_NAMES = {1: "DEAD", 2: "JOIN", 3: "CLUSTER"}   # EDGE.end_kind (td5_roam_world.h)
# [T9] gap kinds whose partner may simply live in a tile that was not built
BORDER_CATS = ("CLUSTER end, cluster not emitted in any built tile",
               "JOIN end, partner row not present in any built tile",
               "DEAD end, no wall cap (BEDG) at the row",
               "not a ribbon row (patch outline or side edge)",
               "row inside an EDGE run (neighbouring quad missing)")


def classify_gap(t, key, e, node_keys):
    """Why an unpaired open ribbon row has no partner: the EDGE end it sits on."""
    owners = t.seg_owner.get(e)
    if not owners:
        return "not a ribbon row (patch outline or side edge)"
    si, side = owners[0]
    rec = t.span_run.get(si)
    if rec is None:
        return "row of a span with no EDGE record"
    # [T9] a run that starts mid-ribbon (row0 > 0: the quad before it is owned by
    # another tile) does not start at the EDGE end, so its first row is no end row
    at_start = side == "L" and si == rec[11] and rec[13] == 0
    at_end = side == "R" and si == rec[11] + rec[12] - 1
    if not (at_start or at_end):
        return "row inside an EDGE run (neighbouring quad missing)"
    kind = rec[14] if side == "L" else rec[15]
    name = END_NAMES.get(kind, "kind %d" % kind)
    if kind == 3:
        ck = rec[1] if side == "L" else rec[2]
        if ck not in node_keys:
            return "CLUSTER end, cluster not emitted in any built tile"
        return "CLUSTER end, cluster emitted but its outline misses the row"
    if kind == 1:
        return "DEAD end, no wall cap (BEDG) at the row"
    if kind == 2:
        return "JOIN end, partner row not present in any built tile"
    return "%s end" % name


def main(argv):
    opt = {"tiles": os.path.join(REPO, PLACE_DIR, "roam"),
           "roads": os.path.join(REPO, PLACE_DIR, "tiles"),
           "grid": None, "poly": None, "out": os.path.join(MAIN, "_archive", "roam", "F1", "T7"),
           "png": True}
    i = 0
    while i < len(argv):
        a = argv[i]
        if a == "--no-png":
            opt["png"] = False
        elif a.startswith("--") and i + 1 < len(argv):
            opt[a[2:]] = argv[i + 1]
            i += 1
        else:
            print("unknown argument: " + a)
            return 2
        i += 1
    if opt["grid"] is None:
        opt["grid"] = os.path.join(opt["tiles"], "ROAMGRID.JSON")
    if opt["poly"] is None:
        opt["poly"] = os.path.join(MAIN, "_archive", "roam", "F0a", "casco_poly.json")

    out = []

    def say(line=""):
        out.append(line)
        print(line)

    files = sorted(glob.glob(os.path.join(opt["tiles"], "ROAMTILE_*.BIN")))
    if not files:
        say("no ROAMTILE_*.BIN under " + opt["tiles"])
        return 2
    problems = {}
    tiles = {}
    for p in files:
        t = load_tile(p, problems)
        if t is not None:
            tiles[t.key] = t
    struct_bad = sum(len(v) for v in problems.values())
    say("structure gate: %d of %d tiles parsed, %d problem(s)" % (len(tiles), len(files), struct_bad))
    for k, v in sorted(problems.items()):
        say("  PROBLEM %s x%d e.g. %s" % (k, len(v), v[0]))

    for t in tiles.values():
        tile_mesh(t)
    lat = Lattice(opt["grid"])
    poly = load_poly(opt["poly"])

    # ---- drivable ways (td5_roam_world.c filter) -----------------------------
    seen = set()
    drop = defaultdict(int)
    ways = []
    for path in sorted(glob.glob(os.path.join(opt["roads"], "ROADS_*.JSON"))):
        with open(path, encoding="utf-8") as f:
            doc = json.load(f)
        for rec in doc.get("roads", []):
            wid = rec.get("id")
            if not isinstance(wid, (int, float)) or wid < 0:
                drop["no_id"] += 1
                continue
            wid = int(wid)
            tags = rec.get("tags") or {}
            if rec.get("class") in DROP_CLASS:
                drop["class:" + rec["class"]] += 1
                continue
            if tags.get("area") == "yes":
                drop["area"] += 1
                continue
            if tags.get("service") in DROP_SERVICE:
                drop["service:" + tags["service"]] += 1
                continue
            if wid in seen:
                drop["dup_id"] += 1
                continue
            seen.add(wid)
            kept = []
            last = None
            for p in rec.get("points", []):
                x, z = float(p["x"]), float(p["z"])
                key = (round(x / 10.0), round(z / 10.0))
                if key == last:
                    continue
                kept.append((x, z))
                last = key
            if len(kept) < 2:
                drop["degenerate"] += 1
                continue
            ways.append({"id": wid, "name": rec.get("name") or "", "pts": kept,
                         "len_m": sum(math.dist(a, b) for a, b in zip(kept, kept[1:])) / UPM,
                         "file": os.path.basename(path)})
    say("drivable ways in ROADS: %d kept; dropped %s" %
        (len(ways), ", ".join("%s %d" % kv for kv in sorted(drop.items()))))

    inside = [w for w in ways if way_in_poly(w["pts"], poly)]
    say("drivable ways meeting the casco polygon: %d" % len(inside))

    # ---- (1) coverage ---------------------------------------------------------
    quad_owned = defaultdict(int)
    for t in tiles.values():
        for e in t.edges:
            if e[12] >= 1:
                quad_owned[e[0] >> 16] += e[12]

    tri_grid = defaultdict(list)
    tri_list = []
    for t in tiles.values():
        for p in t.tri_w:
            prep = tri_prep(((p[0][0], p[0][2]), (p[1][0], p[1][2]), (p[2][0], p[2][2])))
            if prep is None:
                continue
            idx = len(tri_list)
            tri_list.append(prep)
            xs = [q[0] for q in p]
            zs = [q[2] for q in p]
            for cx in range(math.floor(min(xs) / CELL), math.floor(max(xs) / CELL) + 1):
                for cz in range(math.floor(min(zs) / CELL), math.floor(max(zs) / CELL) + 1):
                    tri_grid[(cx, cz)].append(idx)

    def in_patch(x, z):
        for idx in tri_grid.get((math.floor(x / CELL), math.floor(z / CELL)), ()):
            if tri_contains(tri_list[idx], x, z):
                return True
        return False

    def samples(pts):
        for a, b in zip(pts, pts[1:]):
            yield a
            yield ((a[0] + b[0]) / 2.0, (a[1] + b[1]) / 2.0)
        yield pts[-1]

    # [T9] The union test: a way absorbed into a junction (no quads of its own) is
    # covered when its centreline, sampled every metre, stays within COVER_TOL of the
    # drivable surface (any tile's ribbon quads or patch triangles). The old test asked
    # for every sample strictly inside a PATCH, so a connector whose centreline runs
    # under a neighbouring ribbon, or 0.3 m past a patch kerb, read as uncovered.
    ugrid = defaultdict(list)
    ulist = []
    for t in tiles.values():
        for f in t.faces:
            prep = tri_prep(((f[0][0], f[0][2]), (f[1][0], f[1][2]), (f[2][0], f[2][2])))
            if prep is None:
                continue
            idx = len(ulist)
            ulist.append((f, prep))
            xs = [q[0] for q in f]
            zs = [q[2] for q in f]
            for cx in range(math.floor((min(xs) - COVER_TOL) / CELL), math.floor((max(xs) + COVER_TOL) / CELL) + 1):
                for cz in range(math.floor((min(zs) - COVER_TOL) / CELL), math.floor((max(zs) + COVER_TOL) / CELL) + 1):
                    ugrid[(cx, cz)].append(idx)

    def on_union(x, z):
        for idx in ugrid.get((math.floor(x / CELL), math.floor(z / CELL)), ()):
            f, prep = ulist[idx]
            if tri_contains(prep, x, z):
                return True
            for a, b in ((f[0], f[1]), (f[1], f[2]), (f[2], f[0])):
                dx, dz = b[0] - a[0], b[2] - a[2]
                l2 = dx * dx + dz * dz
                u = 0.0 if l2 < 1e-9 else max(0.0, min(1.0, ((x - a[0]) * dx + (z - a[2]) * dz) / l2))
                if math.hypot(x - a[0] - dx * u, z - a[2] - dz * u) <= COVER_TOL:
                    return True
        return False

    def fine_samples(pts):
        for a, b in zip(pts, pts[1:]):
            n = max(1, int(math.dist(a, b) / UPM))
            for k in range(n):
                yield (a[0] + (b[0] - a[0]) * k / n, a[1] + (b[1] - a[1]) * k / n)
        yield pts[-1]

    n_quad = n_patch = n_union = 0
    uncovered = []
    for w in inside:
        if quad_owned.get(w["id"], 0) > 0:
            n_quad += 1
            continue
        if all(in_patch(x, z) for x, z in samples(w["pts"])):
            n_patch += 1
            continue
        if all(on_union(x, z) for x, z in fine_samples(w["pts"])):
            n_union += 1
            continue
        uncovered.append(w)
    uncovered.sort(key=lambda w: -w["len_m"])
    say("")
    say("(1) COVERAGE: %d drivable ways in the casco; %d own quads; %d inside a patch; %d on the "
        "drivable union within %.0f m (absorbed connectors); %d UNCOVERED"
        % (len(inside), n_quad, n_patch, n_union, COVER_TOL / UPM, len(uncovered)))
    for w in uncovered[:TOP]:
        say("    way %d  %-28s %8.1f m  (%s)" % (w["id"], w["name"][:28], w["len_m"], w["file"]))
    if len(uncovered) > TOP:
        say("    ... %d more (full list in the output file list below)" % (len(uncovered) - TOP))
    for w in uncovered[TOP:]:
        say("    way %d  %-28s %8.1f m  (%s)" % (w["id"], w["name"][:28], w["len_m"], w["file"]))
    cover_uncovered_total = sum(w["len_m"] for w in uncovered)
    say("    uncovered length total: %.1f m" % cover_uncovered_total)

    # ---- (2) seams ------------------------------------------------------------
    band = defaultdict(set)
    for key, t in tiles.items():
        for p in t.pts:
            for side in lat.sides(key[0], key[1], p):
                band[(key, side)].add(p)
    pairs = [(("U1"), ("U0"), 1, 0), (("V1"), ("V0"), 0, 1)]

    def near_miss(nbt, p):
        """[F1-T10] True when the neighbour across holds a vertex within REACH_TOL (1 m) of p
        but not p itself: a crack in the seam. A vertex on the lattice line that the
        neighbour has nothing near is legitimately unshared -- a kerb or lane line lying on
        the border, or a quad that straddles it and is owned here (T9's 17 'lattice-line
        mismatches' were all of these kinds: the nearest neighbour vertex was >= 3.7 m
        away, F1-T10 seam17.py)."""
        for q in nbt.pts:
            if abs(q[0] - p[0]) <= REACH_TOL and abs(q[2] - p[2]) <= REACH_TOL:
                return True
        return False

    seam_one_side = []
    seam_bad = []
    seam_checked = 0
    seam_vertices = 0
    outer_vertices = 0
    for key in tiles:
        for side, opp, dx, dz in pairs:
            nb = (key[0] + dx, key[1] + dz)
            a = band.get((key, side), set())
            if nb not in tiles:
                outer_vertices += len(a)
                continue
            b = band.get((nb, opp), set())
            seam_checked += 1
            seam_vertices += len(a) + len(b)
            for p in a - b:
                if near_miss(tiles[nb], p):
                    seam_bad.append((key, nb, side, p))
                else:
                    seam_one_side.append((key, nb, side, p))
            for p in b - a:
                if near_miss(tiles[key], p):
                    seam_bad.append((nb, key, opp, p))
                else:
                    seam_one_side.append((nb, key, opp, p))
    say("")
    say("(2) SEAMS: %d shared sides checked, %d border vertices compared, %d MISMATCHED, "
        "%d border vertices on the outer edge of the built set (informational)"
        % (seam_checked, seam_vertices, len(seam_bad), outer_vertices))
    say("    [F1-T10] unpartnered lattice-line vertices the neighbour has nothing within %.0f m of "
        "(a kerb on the border, a straddling quad owned here: nothing across to share): %d (informational)"
        % (REACH_TOL / UPM, len(seam_one_side)))
    for key, nb, side, p in seam_bad[:TOP]:
        tag = " [ovf]" if p in tiles[key].ovf_pts or (nb in tiles and p in tiles[nb].ovf_pts) else ""
        say("    tile %s side %s -> %s  %s has no identical partner%s"
            % (key, side, nb, fmt_pt(p), tag))
    if len(seam_bad) > TOP:
        say("    ... %d more" % (len(seam_bad) - TOP))

    # ---- (3) and (4) edges ----------------------------------------------------
    short_open = []
    short_kerb = []
    gaps = []
    open_kerb = open_pair = open_outer = open_hole = open_border = 0
    inner_rect = None
    if opt.get("inner"):
        inner_rect = [int(v) for v in opt["inner"].split(",")]

    def in_rect(k):
        return inner_rect[0] <= k[0] <= inner_rect[2] and inner_rect[1] <= k[1] <= inner_rect[3]
    inner = []   # every open edge that is not paired or outer, with its kind (overlap check)
    open_by_edge = defaultdict(list)     # exact edge -> the tiles where it is open
    vert_tiles = defaultdict(set)        # exact vertex -> the tiles that hold it
    for key, t in tiles.items():
        for e, c in t.mesh.items():
            if c == 1:
                open_by_edge[e].append(key)
        for p in t.pts:
            vert_tiles[p].add(key)
    node_keys = set()
    for t in tiles.values():
        node_keys.update(n[0] for n in t.nodes)
    xz_ys = defaultdict(set)              # (x, z) -> every y a vertex has there
    for t in tiles.values():
        for p in t.pts:
            xz_ys[(p[0], p[2])].add(p[1])
    topo_mismatch = []
    for key, t in tiles.items():
        for b in t.bed:
            if elen(b) < CM:
                short_kerb.append((key, b, elen(b)))
        for e, c in t.mesh.items():
            if c != 1:
                continue
            ln = elen(e)
            if ln < CM:
                short_open.append((key, e, ln))
            if e in t.bed:
                open_kerb += 1
                inner.append((key, e, ln, "kerb"))
                continue
            if any(k != key for k in open_by_edge[e]):
                open_pair += 1           # the same open edge closes it in another tile
                continue
            sp = lat.sides(key[0], key[1], e[0])
            sq = lat.sides(key[0], key[1], e[1])
            if [s for s in sp if s in sq]:
                open_outer += 1          # on a tile side whose neighbour is not built
                continue
            if e[0] in t.hole_pts and e[1] in t.hole_pts:
                open_hole += 1           # T3b hole outline: a legit boundary
                inner.append((key, e, ln, "hole"))
                continue
            if any(k != key for k in vert_tiles[e[0]]) and any(k != key for k in vert_tiles[e[1]]):
                topo_mismatch.append((key, e, ln))   # both ends are shared, the edge is not
            if inner_rect and not in_rect(key):
                continue                         # [T9] --inner: ring tiles are context only
            cat = classify_gap(t, key, e, node_keys)
            if all(len(xz_ys.get((p[0], p[2]), ())) > 1 for p in e):
                # [T9] the same x,z exists at another height: a partner that is
                # there but does not meet (the JOIN-row y bug), never the border
                cat = cat.split(",")[0] + ", partner row at a different height"
            if not inner_rect and cat in BORDER_CATS and any(
                    (key[0] + dx, key[1] + dz) not in tiles
                    for dx in (-1, 0, 1) for dz in (-1, 0, 1)):
                # [T9] the partner (cluster patch, JOIN row, next quad) belongs to a tile
                # that was not built: edge of the built set, like `outer`. Proven for the
                # casco by building the one-tile ring and auditing with --inner.
                open_border += 1
                continue
            gaps.append((key, e, ln, cat))
            inner.append((key, e, ln, "gap"))
    say("")
    say("(3) SHORT EDGES: %d open edges under 1 cm (of which kerb walls: %d); %d kerb edges under 1 cm"
        % (len(short_open), sum(1 for x in short_open if x[1] in tiles[x[0]].bed), len(short_kerb)))
    for key, e, ln in (short_open + short_kerb)[:TOP]:
        say("    tile %s %s-%s  length %.3f units (%.4f cm)" % (key, fmt_pt(e[0]), fmt_pt(e[1]), ln, ln / CM))

    arm_total = 0
    arm_bad = []
    for key, t in tiles.items():
        if inner_rect and not in_rect(key):
            continue
        for a in t.arms:
            cut = a[2]
            if cut == 0xFFFF:
                continue
            arm_total += 1
            if cut >= len(t.span_geo):
                arm_bad.append((key, a[0], cut, "cut span out of range"))
                continue
            L, rowL, rowR = t.span_geo[cut]
            ok = False
            best_miss = None
            for row in (rowL, rowR):
                segs = [ekey(row[k], row[k + 1]) for k in range(L)]
                miss = [s for s in segs if not (s in t.tri_e or s in t.bed)]
                if not miss:
                    ok = True
                    break
                if best_miss is None or len(miss) < len(best_miss):
                    best_miss = miss
            if not ok:
                arm_bad.append((key, a[0], cut, "cut row not in patch outline", best_miss))
    say("")
    say("(4) OPEN GAPS: %d open edges with no partner in any tile, not kerb, hole or outer edge "
        "(closed across tiles: %d; kerb %d, hole %d, outer %d, edge of the built set %d)%s"
        % (len(gaps), open_pair, open_kerb, open_hole, open_outer, open_border,
           (" [--inner %s: gaps counted in those tiles only]" % opt["inner"]) if inner_rect else ""))
    cats = Counter(g[3] for g in gaps)
    for cat, n in cats.most_common():
        say("    %6d  %s" % (n, cat))
    say("    arm cut rows: %d checked, %d not in the patch outline" % (arm_total, len(arm_bad)))
    say("    topology mismatch (both ends shared with another tile, the edge is not): %d"
        % len(topo_mismatch))
    for key, e, ln in topo_mismatch[:TOP]:
        say("    tile %s edge %s-%s  %.1f units" % (key, fmt_pt(e[0]), fmt_pt(e[1]), ln))
    say("    int16-limit VERT entries (overflow candidates): %d in %d tiles; gap ends touching one: %d"
        % (sum(len(t.ovf_idx) for t in tiles.values()), sum(1 for t in tiles.values() if t.ovf_idx),
           sum(1 for k, e, ln, c in gaps if e[0] in tiles[k].ovf_pts or e[1] in tiles[k].ovf_pts)))
    for key, e, ln, cat in gaps[:TOP]:
        tag = " [ovf]" if e[0] in tiles[key].ovf_pts or e[1] in tiles[key].ovf_pts else ""
        say("    tile %s gap %s-%s  %.1f units  [%s]%s" % (key, fmt_pt(e[0]), fmt_pt(e[1]), ln, cat, tag))
    for key, ek, cut, why, _ in arm_bad[:TOP]:
        say("    tile %s arm edge %d cut span %d: %s" % (key, ek, cut, why))
    if len(gaps) > TOP:
        say("    ... %d more gaps" % (len(gaps) - TOP))

    # ---- (4b) boundary edges inside another drivable prim: reported, NOT in TOTAL
    face_grid = defaultdict(list)
    face_list = []
    for t in tiles.values():
        for f in t.faces:
            prep = tri_prep(((f[0][0], f[0][2]), (f[1][0], f[1][2]), (f[2][0], f[2][2])))
            if prep is None:
                continue
            idx = len(face_list)
            face_list.append((f, prep))
            xs = [q[0] for q in f]
            zs = [q[2] for q in f]
            for cx in range(math.floor(min(xs) / CELL), math.floor(max(xs) / CELL) + 1):
                for cz in range(math.floor(min(zs) / CELL), math.floor(max(zs) / CELL) + 1):
                    face_grid[(cx, cz)].append(idx)

    def inside_other(e):
        mx = (e[0][0] + e[1][0]) / 2.0
        mz = (e[0][2] + e[1][2]) / 2.0
        for idx in face_grid.get((math.floor(mx / CELL), math.floor(mz / CELL)), ()):
            f, prep = face_list[idx]
            if e[0] in f and e[1] in f:      # the edge's own prim
                continue
            if tri_strictly_inside(prep, mx, mz):
                return f
        return None

    overlap = []
    for key, e, ln, kind in inner:
        hit = inside_other(e)
        if hit is not None:
            overlap.append((key, e, ln, kind, hit))

    # [T9] (4c) A GAP COVERED BY THE UNION. The wall rule is "the boundary of the UNION
    # of the drivable surface" (T6b's locator drops a boundary edge lying inside another
    # drivable prim). An unpaired open edge is a hole only when the surface just across
    # it is NOT drivable. Probe three points (1/4, 1/2, 3/4) a few units across the edge,
    # away from the edge's own prim; when every probe lies inside some drivable face the
    # gap is covered (two carriageways that overlap at a Y split, a cut row under the
    # neighbouring ribbon). Such a gap still counts when the covering surface stands more
    # than STEP_TOL above or below the edge (a kerb-high step is not continuous road).
    def own_face(t, e):
        for f in t.faces:
            if e[0] in f and e[1] in f:
                return f
        return None

    def plane_y(f, x, z):
        (ax, ay, az), (bx, by, bz), (qx, qy, qz) = f
        den = (bz - qz) * (ax - qx) + (qx - bx) * (az - qz)
        if abs(den) < 1e-9:
            return None
        l1 = ((bz - qz) * (x - qx) + (qx - bx) * (z - qz)) / den
        l2 = ((qz - az) * (x - qx) + (ax - qx) * (z - qz)) / den
        return l1 * ay + l2 * by + (1 - l1 - l2) * qy

    def covered(t, e):
        f0 = own_face(t, e)
        if f0 is None:
            return None
        gx = sum(p[0] for p in f0) / 3.0
        gz = sum(p[2] for p in f0) / 3.0
        dx, dz = e[1][0] - e[0][0], e[1][2] - e[0][2]
        ln = math.hypot(dx, dz)
        if ln < 1e-9:
            return None
        nx, nz = -dz / ln, dx / ln
        mx, mz = (e[0][0] + e[1][0]) / 2.0, (e[0][2] + e[1][2]) / 2.0
        if nx * (gx - mx) + nz * (gz - mz) > 0:      # point away from the own prim
            nx, nz = -nx, -nz
        worst = 0.0
        for s in (0.25, 0.5, 0.75):
            x = e[0][0] + dx * s + nx * PROBE
            z = e[0][2] + dz * s + nz * PROBE
            y = e[0][1] + (e[1][1] - e[0][1]) * s
            best = None
            for idx in face_grid.get((math.floor(x / CELL), math.floor(z / CELL)), ()):
                f, prep = face_list[idx]
                if f is f0 or not tri_contains_strict0(prep, x, z):
                    continue
                py = plane_y(f, x, z)
                if py is None:
                    continue
                if best is None or abs(py - y) < best:
                    best = abs(py - y)
            if best is None:
                return None
            worst = max(worst, best)
        return worst

    covered_gaps = []
    open_gaps = []
    for g in gaps:
        step = covered(tiles[g[0]], g[1])
        if step is not None and step <= STEP_TOL:
            covered_gaps.append(g + (step,))
        else:
            open_gaps.append(g if step is None else g[:3] + (g[3] + " [covered, step %.0f units]" % step,))
    say("")
    say("(4c) GAPS COVERED BY THE DRIVABLE UNION (not in TOTAL): %d of %d; %d stay open "
        "(probe %.0f units across, step tolerance %.0f units = %.0f cm)"
        % (len(covered_gaps), len(gaps), len(open_gaps), PROBE, STEP_TOL, STEP_TOL / CM))
    if covered_gaps:
        steps = sorted(g[4] for g in covered_gaps)
        say("    covered-gap height step: median %.0f, max %.0f units" % (steps[len(steps) // 2], steps[-1]))
    for cat, n in Counter(g[3] for g in open_gaps).most_common():
        say("    %6d  open: %s" % (n, cat))
    for key, e, ln, cat in open_gaps[:TOP]:
        say("    tile %s open gap %s-%s  %.1f units  [%s]" % (key, fmt_pt(e[0]), fmt_pt(e[1]), ln, cat))
    gaps = open_gaps
    # The same rule for an arm cut row that is not on its patch outline (a ROW_COVERED
    # UNION patch: the row lies under the patch or the neighbouring carriageway).
    arm_open = []
    arm_cov = 0
    for rec in arm_bad:
        miss = rec[4] or []
        steps = [covered(tiles[rec[0]], s) for s in miss]
        if miss and all(s is not None and s <= STEP_TOL for s in steps):
            arm_cov += 1
        else:
            arm_open.append(rec)
    say("    arm cut rows off the outline: %d covered by the drivable union, %d open"
        % (arm_cov, len(arm_open)))
    for key, ek, cut, why, _ in arm_open[:TOP]:
        say("    tile %s arm edge %d cut span %d: %s (not covered)" % (key, ek, cut, why))
    arm_bad = arm_open
    by_kind = defaultdict(int)
    for x in overlap:
        by_kind[x[3]] += 1
    say("")
    say("(4b) BOUNDARY INSIDE ANOTHER PRIM (reported separately, not in TOTAL): %d edges "
        "(gap %d, kerb %d, hole %d)" % (len(overlap), by_kind["gap"], by_kind["kerb"], by_kind["hole"]))
    for key, e, ln, kind, hit in overlap[:TOP]:
        cx = sum(p[0] for p in hit) / 3.0
        cz = sum(p[2] for p in hit) / 3.0
        say("    tile %s %s edge %s-%s  %.1f units, inside prim centred (%.0f, %.0f)"
            % (key, kind, fmt_pt(e[0]), fmt_pt(e[1]), ln, cx, cz))
    if len(overlap) > TOP:
        say("    ... %d more" % (len(overlap) - TOP))

    # [T9] (5) a junction cluster with no triangles is an unpaved junction (a FAIL
    # patch that no fallback rebuilt); every one of its arms ends in the void.
    empty = [(key, n[0], n[4], n[6]) for key, t in tiles.items() for n in t.nodes
             if n[8] == 0 and (not inner_rect or in_rect(key))]
    say("")
    say("(5) EMPTY PATCHES: %d junction cluster(s) with no triangles" % len(empty))
    for key, ck, narm, fl in empty[:TOP]:
        say("    tile %s cluster %d (%d arms, flags 0x%x)" % (key, ck, narm, fl))

    total = len(uncovered) + len(seam_bad) + len(topo_mismatch) + len(short_open) + len(short_kerb) \
        + len(gaps) + len(arm_bad) + struct_bad + len(empty)
    say("")
    say("TOTAL problems: %d -> %s" % (total, "CLEAN" if total == 0 else "NOT CLEAN"))

    os.makedirs(opt["out"], exist_ok=True)
    with open(os.path.join(opt["out"], "audit_coverage.txt"), "w", encoding="utf-8") as f:
        f.write("\n".join(out) + "\n")

    if opt["png"]:
        write_png(os.path.join(opt["out"], "casco_coverage_audit.png"), lat, poly, tiles,
                  inside, uncovered, seam_bad, gaps, overlap)
    return 0 if total == 0 else 1


def write_png(path, lat, poly, tiles, inside, uncovered, seam_bad, gaps, overlap, size=1400):
    """Stdlib PNG (no matplotlib: the audit runs under python -I). Place frame in metres,
    north-up (z increases upward). Layers in paint order: tile sides, drivable ways,
    uncovered ways, gaps, overlaps, seam mismatches, casco outline."""
    xs = [p[0] for p in poly]
    zs = [p[1] for p in poly]
    pad = 0.03 * max(max(xs) - min(xs), max(zs) - min(zs))
    x0, z0 = min(xs) - pad, min(zs) - pad
    span = max(max(xs) - min(xs), max(zs) - min(zs)) + 2 * pad
    img = bytearray(b"\xff" * (size * size * 3))

    def put(c, r, rgb):
        if 0 <= c < size and 0 <= r < size:
            i = (r * size + c) * 3
            img[i:i + 3] = rgb

    def to_px(x, z):
        return ((x - x0) / span * (size - 1), (1.0 - (z - z0) / span) * (size - 1))

    def seg(a, b, rgb, width=1):
        (ca, ra), (cb, rb) = to_px(a[0], a[2] if len(a) == 3 else a[1]), \
            to_px(b[0], b[2] if len(b) == 3 else b[1])
        n = int(max(abs(cb - ca), abs(rb - ra))) + 1
        for k in range(n + 1):
            t = k / float(n)
            c = int(round(ca + (cb - ca) * t))
            r = int(round(ra + (rb - ra) * t))
            for dc in range(-(width // 2), width - width // 2):
                for dr in range(-(width // 2), width - width // 2):
                    put(c + dc, r + dr, rgb)

    def poly_line(pts, rgb, width=1):
        for a, b in zip(pts, pts[1:]):
            seg(a, b, rgb, width)

    def rgbc(hexs):
        return bytes(int(hexs[i:i + 2], 16) for i in (1, 3, 5))

    for key in tiles:
        tx, tz = key
        S = lat.S
        corners = [lat.xz(u, v) for u, v in ((tx * S, tz * S), ((tx + 1) * S, tz * S),
                                             ((tx + 1) * S, (tz + 1) * S), (tx * S, (tz + 1) * S),
                                             (tx * S, tz * S))]
        poly_line(corners, rgbc("#d0e4f5"))
    for w in inside:
        poly_line(w["pts"], rgbc("#8a9199"))
    for w in uncovered:
        poly_line(w["pts"], rgbc("#e11d1d"), 3)
    for key, e, ln, cat in gaps:
        seg(e[0], e[1], rgbc("#ff8c00"), 3)
    for key, e, ln, kind, hit in overlap:
        seg(e[0], e[1], rgbc("#7b2cbf"), 2)
    for _, _, _, p in seam_bad:
        c, r = to_px(p[0], p[2])
        for d in range(-4, 5):                       # an X marker
            put(int(round(c)) + d, int(round(r)) + d, rgbc("#c000c0"))
            put(int(round(c)) + d, int(round(r)) - d, rgbc("#c000c0"))
    poly_line(poly + [poly[0]], rgbc("#000000"), 3)

    raw = bytearray()
    for r in range(size):
        raw.append(0)
        raw += img[r * size * 3:(r + 1) * size * 3]

    def chunk(tag, data):
        body = tag + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 2, 0, 0, 0)) \
        + chunk(b"IDAT", zlib.compress(bytes(raw), 6)) + chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
