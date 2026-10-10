#!/usr/bin/env python3
"""geo_bld_map.py -- ROUND 1014 C: top-down (XZ) map of a generated geo level around
a point, straight from MODELS.DAT + MESHTAG.BIN. Every face of every mesh in the
window is drawn, coloured by emitter kind, so "is this building standing in the
other carriageway" is a picture, not an argument.

  python verify/geo_bld_map.py re/assets/levels/level091 --pos 23861,-16713 --half 60 out.png
  (--half in METRES, 430 world units per metre)
Colour: road = dark grey, branchroad (opposite carriageway / corridor) = blue,
  building (procedural frontage) = red, city = orange, cross = magenta,
  block = brown, prop = green, terrain/skirt = pale, rail = black line.
"""
import argparse, os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import geo_bld_audit as A
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import Polygon

COL = {"road": "#555555", "branchroad": "#3b6fd4", "branchside": "#8fb0f0",
       "building": "#d62728", "city": "#ff9f1c", "cross": "#c71fc7",
       "block": "#8c564b", "prop": "#2ca02c", "terrain": "#e8efe0",
       "skirt": "#f3f3e8", "rail": "#000000", "deck": "#777777",
       "decal": "#ffffff", "other": "#bbbbbb", "water": "#88ccff",
       "coast": "#ccddff", "flora": "#66aa66", "parktree": "#66aa66"}
ORDER = ["terrain", "skirt", "water", "coast", "road", "branchroad", "branchside",
         "deck", "decal", "other", "block", "city", "cross", "building", "prop",
         "flora", "parktree", "rail"]


def faces(b, m):
    """Yield (kind('t'|'q'), page, [(x,y,z)...]) in the writer's command order."""
    vs = A.verts(b, m)
    o = 0
    for page, tri, quad in m["cmds"]:
        for t in range(tri):
            yield "t", page, vs[o:o + 3]; o += 3
        for q in range(quad):
            yield "q", page, vs[o:o + 4]; o += 4


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("level"); ap.add_argument("out")
    ap.add_argument("--pos", required=True)
    ap.add_argument("--half", type=float, default=60.0)
    ap.add_argument("--upm", type=float, default=430.0)
    ap.add_argument("--kinds", default="")
    ap.add_argument("--mark", default="", help="e,s of a mesh to outline in cyan")
    ap.add_argument("--roads", default="", help="_route/ROADS.JSON: draw OSM way centrelines")
    ap.add_argument("--bld", default="", help="_route/BUILDINGS.JSON: outline source footprints")
    ap.add_argument("--areas", default="", help="_route/AREAS.JSON: outline park/plaza/common polygons [R1015 C]")
    ap.add_argument("--lm", default="", help="comma list of OSM ids to draw thick (landmark outline + parts) [R1015 C]")
    a = ap.parse_args()
    cx, cz = [float(x) for x in a.pos.split(",")]
    h = a.half * a.upm
    b, meshes = A.load_level(a.level)
    want = set(a.kinds.split(",")) if a.kinds else None
    mark = tuple(int(x) for x in a.mark.split(",")) if a.mark else None
    fig, ax = plt.subplots(figsize=(11, 11))
    byk = {}
    for m in meshes:
        if abs(m["c"][0] - cx) > h + m["r"] or abs(m["c"][2] - cz) > h + m["r"]:
            continue
        byk.setdefault(m["kind"], []).append(m)
    for k in ORDER + [k for k in byk if k not in ORDER]:
        if want and k not in want:
            continue
        for m in byk.get(k, []):
            for typ, page, vs in faces(b, m):
                p = [(v[0] / a.upm, v[2] / a.upm) for v in vs]
                # skip near-vertical faces in the fill (walls): draw outline only
                ys = [v[1] for v in vs]
                xs = [v[0] for v in vs]; zs = [v[2] for v in vs]
                wall = (max(xs) - min(xs) < 1 and max(zs) - min(zs) < 1) or \
                       abs(A.poly_area([(v[0], v[2]) for v in vs])) < 50.0
                if wall:
                    ax.plot([q[0] for q in p] + [p[0][0]], [q[1] for q in p] + [p[0][1]],
                            color=COL.get(k, "#999"), lw=0.6)
                else:
                    ax.add_patch(Polygon(p, closed=True, fc=COL.get(k, "#999"),
                                         ec="#00000030", lw=0.2, alpha=0.55))
            if mark and (m["e"], m["s"]) == mark:
                for typ, page, vs in faces(b, m):
                    p = [(v[0] / a.upm, v[2] / a.upm) for v in vs]
                    ax.plot([q[0] for q in p] + [p[0][0]], [q[1] for q in p] + [p[0][1]],
                            color="cyan", lw=1.8)
    if a.roads:
        import json
        for w in json.load(open(a.roads, encoding="utf-8"))["roads"]:
            pts = w.get("points") or []
            if not pts:
                continue
            xs = [q["x"] for q in pts]; zs = [q["z"] for q in pts]
            if max(xs) < cx - h or min(xs) > cx + h or max(zs) < cz - h or min(zs) > cz + h:
                continue
            ax.plot([x / a.upm for x in xs], [z / a.upm for z in zs], "-",
                    color="#1f77b4" if w.get("oneway") else "#00a000", lw=1.2, zorder=9)
            mid = pts[len(pts) // 2]
            if abs(mid["x"] - cx) < h and abs(mid["z"] - cz) < h:
                ax.text(mid["x"] / a.upm, mid["z"] / a.upm,
                        "%s%s" % (w.get("name") or w.get("class"), " >" if w.get("oneway") else ""),
                        fontsize=7, zorder=10)
    if a.bld:
        import json
        for bd in json.load(open(a.bld, encoding="utf-8"))["buildings"]:
            pts = bd.get("points") or []
            if not pts:
                continue
            xs = [q["x"] for q in pts]; zs = [q["z"] for q in pts]
            if max(xs) < cx - h or min(xs) > cx + h or max(zs) < cz - h or min(zs) > cz + h:
                continue
            ax.plot([x / a.upm for x in xs], [z / a.upm for z in zs], "-",
                    color="#7a00ff", lw=0.9, zorder=8)
            ax.text(sum(xs) / len(xs) / a.upm, sum(zs) / len(zs) / a.upm,
                    "%.0fm" % (bd.get("height_m") or 0), fontsize=6, color="#7a00ff", zorder=8)
    if a.areas:
        import json
        for ar in json.load(open(a.areas, encoding="utf-8"))["areas"]:
            pts = ar.get("points") or []
            if not pts:
                continue
            xs = [q["x"] for q in pts]; zs = [q["z"] for q in pts]
            if max(xs) < cx - h or min(xs) > cx + h or max(zs) < cz - h or min(zs) > cz + h:
                continue
            ax.plot([x / a.upm for x in xs] + [xs[0] / a.upm], [z / a.upm for z in zs] + [zs[0] / a.upm],
                    "--", color="#008080", lw=1.6, zorder=7)
            ax.text(sum(xs) / len(xs) / a.upm, sum(zs) / len(zs) / a.upm,
                    "%s %s" % (ar.get("kind"), ar.get("name") or ""), fontsize=7, color="#008080", zorder=11)
    if a.lm and a.bld:
        import json
        want_ids = set(int(x) for x in a.lm.split(","))
        for bd in json.load(open(a.bld, encoding="utf-8"))["buildings"]:
            if bd.get("id") in want_ids or bd.get("landmark"):
                pts = bd.get("points") or []
                xs = [q["x"] for q in pts]; zs = [q["z"] for q in pts]
                if not pts or max(xs) < cx - h or min(xs) > cx + h or max(zs) < cz - h or min(zs) > cz + h:
                    continue
                ax.plot([x / a.upm for x in xs] + [xs[0] / a.upm], [z / a.upm for z in zs] + [zs[0] / a.upm],
                        "-", color="#ff0000", lw=2.2, zorder=12)
    ax.plot([cx / a.upm], [cz / a.upm], "k+", ms=20)
    ax.set_xlim((cx - h) / a.upm, (cx + h) / a.upm)
    ax.set_ylim((cz - h) / a.upm, (cz + h) / a.upm)
    ax.set_aspect("equal")
    ax.set_title("%s  centre %.0f,%.0f  half %.0f m   kinds: %s" %
                 (os.path.basename(a.level), cx, cz, a.half, ",".join(sorted(byk))))
    handles = [plt.Line2D([0], [0], color=COL[k], lw=6, label=k) for k in ORDER if k in byk]
    ax.legend(handles=handles, loc="upper right", fontsize=8)
    fig.savefig(a.out, dpi=90)


if __name__ == "__main__":
    main()
