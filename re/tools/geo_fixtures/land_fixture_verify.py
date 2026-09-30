"""[GEO LAND] Match every fixture building to its mesh in MODELS.DAT and
check it came out the shape it was tagged.

WHY NOT JUST LOOK AT A FRAMEDUMP. The roof page (R3_BLOCK+3) is shared with
the procedural houses, and from above a tiled roof texture hides the
geometry: a top-down capture of the fixture cannot tell a gabled roof from a
pyramid, and a generic audit over that page picks up every procedural house
in the entry. This reads the fixture's own BUILDINGS.JSON, finds the mesh
whose vertices sit on each footprint, and reports its SHAPE SIGNATURE.

Per case it prints:
  rise      roof height above the eaves
  ntop      distinct XZ points at the top of the roof -- 1 = apex,
            2 = a ridge, n = an inset ring (mansard)
  topext    how far the top set spans, against the footprint's own extent
  offset    how far the top set's centre sits off the footprint's centre --
            a skillion's top is at the far EDGE, so this is ~half the length
  y_lo/y_hi the mass's vertical extent, which is what proves a building:part
            stack starts where the part below it stops
  lat       nearest approach of ANY vertex to the route centreline, against
            the carriageway half-width: this is the "does not intersect the
            road" test, done on the geometry rather than on the guard's word

  python land_fixture_verify.py <MODELS.DAT> [--place land_test] [--spe 4]
"""
import json
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from land_pages import page_table                       # noqa: E402
from land_models_probe import walk                      # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))


def dedup(pts, eps=2.0):
    out = []
    for p in pts:
        if not any(abs(p[0] - q[0]) < eps and abs(p[1] - q[1]) < eps for q in out):
            out.append(p)
    return out


def extent(pts):
    best = 0.0
    for i in range(len(pts)):
        for j in range(i + 1, len(pts)):
            d = math.hypot(pts[i][0] - pts[j][0], pts[i][1] - pts[j][1])
            best = max(best, d)
    return best


def centre(pts):
    if not pts:
        return (0.0, 0.0)
    return (sum(p[0] for p in pts) / len(pts), sum(p[1] for p in pts) / len(pts))


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    models = sys.argv[1]
    place, spe = "land_test", 4
    i = 2
    while i < len(sys.argv):
        if sys.argv[i] == "--place":
            i += 1
            place = sys.argv[i]
        elif sys.argv[i] == "--spe":
            i += 1
            spe = int(sys.argv[i])
        i += 1

    gdir = os.path.join(ROOT, "re", "assets", "geo", place)
    blds = json.load(open(os.path.join(gdir, "BUILDINGS.JSON"),
                          encoding="utf-8"))["buildings"]
    route = json.load(open(os.path.join(gdir, "ROUTE.JSON"), encoding="utf-8"))
    rpts = [(q["x"], q["z"]) for q in route["points"]]
    upm = route.get("units_per_metre") or 430.0
    pt = page_table()
    roof_page = pt["TD5_TG_PAGE_R3_BLOCK"] + 3
    # TD5_TG_PAGE_LM_BASE is defined with a line continuation, which the
    # macro scraper skips, so rebuild it from its own parts.
    lm_base = pt.get("TD5_TG_PAGE_LM_BASE")
    if lm_base is None:
        lm_base = (pt["TD5_TG_PAGE_RS_BASE"]
                   + pt["TD5_TG_RS_CLASSES"] * pt["TD5_TG_RS_PER_CLASS"])

    # every mesh, with its vertices split into roof-page and other
    meshes = {}
    for entry, span0, mi, page, flags, faces in walk(models, spe):
        m = meshes.setdefault((entry, mi), dict(span0=span0, roof=[], other=[],
                                                pages=set()))
        m["pages"].add(page)
        for p, _t in faces:
            (m["roof"] if page == roof_page else m["other"]).extend(p)

    print("%-22s %6s %6s %5s %8s %8s %8s %8s %9s %9s %7s"
          % ("case", "span", "rooffc", "ntop", "rise", "topext", "footext",
             "offset", "y_lo", "y_hi", "lat_m"))
    rc = 0
    for b in blds:
        ring = b["points"][:-1]
        bcx = sum(q["x"] for q in ring) / len(ring)
        bcz = sum(q["z"] for q in ring) / len(ring)
        brad = max(math.hypot(q["x"] - bcx, q["z"] - bcz) for q in ring)
        # the mesh whose vertices sit on THIS footprint: closest XZ centre,
        # and it has to be within the footprint's own radius to count.
        best, bestd = None, 1e18
        for key, m in meshes.items():
            allv = m["roof"] + m["other"]
            if not allv:
                continue
            cx = sum(v[0] for v in allv) / len(allv)
            cz = sum(v[2] for v in allv) / len(allv)
            d = math.hypot(cx - bcx, cz - bcz)
            if d < bestd:
                best, bestd = (key, m), d
        name = (b.get("name") or "?").replace("LAND ", "")
        if best is None or bestd > brad:
            print("%-22s %6s  -- no mesh on this footprint (refused) --"
                  % (name, "-"))
            continue
        key, m = best
        allv = m["roof"] + m["other"]
        ylo = min(v[1] for v in allv)
        yhi = max(v[1] for v in allv)
        lat = min(math.hypot(v[0] - rx, v[2] - rz)
                  for v in allv for rx, rz in rpts) / upm
        if m["roof"]:
            ytop = max(v[1] for v in m["roof"])
            ybot = min(v[1] for v in m["roof"])
            top = dedup([(v[0], v[2]) for v in m["roof"] if ytop - v[1] < 2.0])
            foot = dedup([(v[0], v[2]) for v in m["other"]]) or top
            te, fe = extent(top), extent(foot)
            off = math.hypot(*[a - c for a, c in zip(centre(top), centre(foot))])
            rise, ntop, rooffc = ytop - ybot, len(top), len(m["roof"]) // 3
        else:
            te = fe = off = rise = 0.0
            ntop = rooffc = 0
        if lm_base in m["pages"]:
            rooffc = -1               # a stamped set piece, not an extrusion
        print("%-22s %6d %6d %5d %8.0f %8.0f %8.0f %8.0f %9.0f %9.0f %7.1f"
              % (name, m["span0"], rooffc, ntop, rise, te, fe, off, ylo, yhi,
                 lat))
    return rc


if __name__ == "__main__":
    sys.exit(main())
