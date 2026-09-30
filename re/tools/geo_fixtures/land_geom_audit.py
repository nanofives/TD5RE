"""[GEO LAND] Offline geometry audit of the real-building meshes in MODELS.DAT.

Checks the things a framedump cannot see, per mesh:

  DEGEN     faces whose three (or four) points are collinear -- they render as
            nothing and are the usual symptom of a roof builder folding a ring
            onto itself.
  FLIP      roof-page faces whose normal disagrees in sign of Y with the rest
            of the same mesh. A roof is a height function over its footprint,
            so every one of its SLOPING faces must face the same way up; a mix
            means the builder wound one of them backwards.
            VERTICAL roof faces are counted separately and never as a flip:
            a gable END and a shed's high wall are vertical by definition, and
            an earlier cut of this audit read every one of them as an
            inversion (5 false positives on the 20-case fixture).
  GAP       the roof's lowest vertex against the wall's highest. They must
            meet: a positive gap is daylight between the wall top and the
            eaves, a negative one is a roof sunk into its own walls.
  SKEW      a quad whose four points are not coplanar (walls only).

--roof-sig prints a SHAPE SIGNATURE per building instead, which is what
actually settles "did this roof come out gabled or was it another pyramid":
the set of roof vertices at the top height, its extent, and where its centre
sits against the footprint's. A framedump of a tiled roof page from above
cannot separate those; these numbers can.

  apex      1 top point, extent 0                 pyramidal / dome / onion
  gabled    2 top points, extent ~ the long axis  gabled / half-hipped
  hipped    2 top points, extent ~ long - width   hipped
  skillion  top points ON the footprint edge, centre offset ~ half the length
  mansard   a whole inset ring at the top, extent ~ 0.35..0.9 of the footprint
  flat      no top set above the wall

Usage:
  python land_geom_audit.py <MODELS.DAT> [--spans 200,240,...] [--spe 4]
                            [--roof-page N] [-v] [--roof-sig]

With --spans it reports each named span's meshes one by one, which is what
the fixture set (land_fixture_build.py) wants -- one test building per span.
Without it, it reports totals over the whole file.
"""
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from land_pages import page_table                       # noqa: E402
from land_models_probe import walk                      # noqa: E402

TRI_MIN = 4.0          # matches TD5_TG_GEO_TRI_MIN in td5_tg_city.c


def normal(p0, p1, p2):
    ax = p1[0] - p0[0]; ay = p1[1] - p0[1]; az = p1[2] - p0[2]
    bx = p2[0] - p0[0]; by = p2[1] - p0[1]; bz = p2[2] - p0[2]
    return (ay * bz - az * by, az * bx - ax * bz, ax * by - ay * bx)


def norm_len(n):
    return math.sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2])


def _dedup(pts, eps=2.0):
    out = []
    for p in pts:
        if not any(abs(p[0] - q[0]) < eps and abs(p[1] - q[1]) < eps
                   for q in out):
            out.append(p)
    return out


def _extent(pts):
    best = 0.0
    for i in range(len(pts)):
        for j in range(i + 1, len(pts)):
            d = math.hypot(pts[i][0] - pts[j][0], pts[i][1] - pts[j][1])
            if d > best:
                best = d
    return best


def _centre(pts):
    if not pts:
        return (0.0, 0.0)
    return (sum(p[0] for p in pts) / len(pts), sum(p[1] for p in pts) / len(pts))


def roof_signature(path, spans, spe, roof_page):
    """One row per mesh that carries roof-page faces: what its top set looks
    like. See the module docstring for how to read it."""
    want = set(s // spe for s in spans) if spans else None
    per = {}
    for entry, span0, mi, page, flags, faces in walk(path, spe):
        if want is not None and entry not in want:
            continue
        m = per.setdefault((entry, mi), dict(span0=span0, roof=[], wall=[]))
        for p, _t in faces:
            for q in p:
                (m["roof"] if page == roof_page else m["wall"]).append(q)

    print("%-6s %-5s %6s %6s %8s %9s %9s %9s  %s"
          % ("span", "mesh", "rooffc", "ntop", "rise", "topext", "footext",
             "offset", "reads as"))
    for key in sorted(per):
        m = per[key]
        if not m["roof"] or not m["wall"]:
            continue
        ytop = max(q[1] for q in m["roof"])
        ybot = min(q[1] for q in m["roof"])
        top = _dedup([(q[0], q[2]) for q in m["roof"] if ytop - q[1] < 2.0])
        foot = _dedup([(q[0], q[2]) for q in m["wall"]])
        te, fe = _extent(top), _extent(foot)
        tc, fc = _centre(top), _centre(foot)
        off = math.hypot(tc[0] - fc[0], tc[1] - fc[1])
        rise = ytop - ybot
        if rise < 2.0:
            reads = "flat"
        elif len(top) == 1:
            reads = "apex"
        elif len(top) == 2:
            reads = "ridge %.2f of the footprint" % (te / fe if fe else 0.0)
        elif te / (fe or 1.0) > 0.25 and off < fe * 0.12:
            reads = "inset ring (%d pts, %.2f of the footprint)" % (len(top), te / fe)
        else:
            reads = "%d top pts, offset %.2f of the footprint" % (len(top), off / (fe or 1.0))
        print("%-6d %-5d %6d %6d %8.0f %9.0f %9.0f %9.0f  %s"
              % (m["span0"], key[1], len(m["roof"]) // 3, len(top), rise,
                 te, fe, off, reads))
    return 0


def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        return 2
    path = args[0]
    pt = page_table()
    roof_page = pt["TD5_TG_PAGE_R3_BLOCK"] + 3
    spans, spe, verbose, sig = None, 4, False, False
    i = 1
    while i < len(args):
        if args[i] == "--roof-sig":
            sig = True
        elif args[i] == "--spans":
            i += 1
            spans = [int(x) for x in args[i].split(",")]
        elif args[i] == "--spe":
            i += 1
            spe = int(args[i])
        elif args[i] == "--roof-page":
            i += 1
            roof_page = int(args[i])
        elif args[i] in ("-v", "--verbose"):
            verbose = True
        i += 1

    if sig:
        return roof_signature(path, spans, spe, roof_page)

    want_entries = None
    if spans is not None:
        want_entries = set(s // spe for s in spans)

    # mesh key -> accumulated facts
    meshes = {}
    for entry, span0, mi, page, flags, faces in walk(path, spe):
        if want_entries is not None and entry not in want_entries:
            continue
        m = meshes.setdefault((entry, mi), dict(
            span0=span0, pages=[], degen=0, tris=0, quads=0, skew=0,
            roof_up=0, roof_dn=0, roof_vert=0, roof_ylo=None, wall_yhi=None,
            ylo=None, yhi=None))
        m["pages"].append(page)
        for p, t in faces:
            for q in p:
                m["ylo"] = q[1] if m["ylo"] is None else min(m["ylo"], q[1])
                m["yhi"] = q[1] if m["yhi"] is None else max(m["yhi"], q[1])
            n = normal(p[0], p[1], p[2])
            ln = norm_len(n)
            if len(p) == 3:
                m["tris"] += 1
            else:
                m["quads"] += 1
                # coplanarity: the 4th point against the first triangle's plane
                if ln > 1e-9:
                    d = abs(sum(n[k] * (p[3][k] - p[0][k]) for k in range(3))) / ln
                    if d > 4.0:
                        m["skew"] += 1
            if ln <= TRI_MIN:
                m["degen"] += 1
                continue
            if page == roof_page:
                if abs(n[1]) / ln < 0.05:
                    m["roof_vert"] += 1       # gable end / shed high wall
                elif n[1] > 0.0:
                    m["roof_up"] += 1
                else:
                    m["roof_dn"] += 1
                lo = min(q[1] for q in p)
                m["roof_ylo"] = lo if m["roof_ylo"] is None \
                    else min(m["roof_ylo"], lo)
            else:
                hi = max(q[1] for q in p)
                m["wall_yhi"] = hi if m["wall_yhi"] is None \
                    else max(m["wall_yhi"], hi)

    tot = dict(meshes=0, degen=0, flip=0, skew=0, gap=0)
    print("%-6s %-5s %-16s %5s %5s %6s %5s %5s %5s %9s %9s %8s"
          % ("span", "mesh", "pages", "tris", "quads", "degen", "up", "down",
             "vert", "y_lo", "y_hi", "roofgap"))
    for (entry, mi) in sorted(meshes):
        m = meshes[(entry, mi)]
        flip = min(m["roof_up"], m["roof_dn"])
        gap = None
        if m["roof_ylo"] is not None and m["wall_yhi"] is not None:
            gap = m["roof_ylo"] - m["wall_yhi"]
        tot["meshes"] += 1
        tot["degen"] += m["degen"]
        tot["skew"] += m["skew"]
        if flip:
            tot["flip"] += flip
        if gap is not None and abs(gap) > 1.0:
            tot["gap"] += 1
        if verbose or m["degen"] or flip or (gap is not None and abs(gap) > 1.0):
            print("%-6d %-5d %-16s %5d %5d %6d %5d %5d %5d %9.0f %9.0f %8s"
                  % (m["span0"], mi, ",".join(str(p) for p in m["pages"][:4]),
                     m["tris"], m["quads"], m["degen"], m["roof_up"],
                     m["roof_dn"], m["roof_vert"], m["ylo"], m["yhi"],
                     ("%.0f" % gap) if gap is not None else "-"))
    print("TOTAL meshes=%d  DEGEN=%d  FLIP=%d  SKEWQUAD=%d  ROOFGAP=%d"
          % (tot["meshes"], tot["degen"], tot["flip"], tot["skew"], tot["gap"]))
    return 1 if (tot["degen"] or tot["flip"] or tot["gap"]) else 0


if __name__ == "__main__":
    sys.exit(main())
