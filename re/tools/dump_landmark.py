#!/usr/bin/env python3
"""Dump one segmented landmark's faces + texture pages for authoring.

The account3 'Claude authoring with web reference' pass reads this to see what
geometry exists, where the holes are, and which textures are available, then
writes new faces into re/assets/library/authored_fills.json (see
td5_geomlib.load_authored_fills for the schema).

    python re/tools/dump_landmark.py --level 23 --idx 0 --out DIR

Writes DIR/faces.json (every face: role, page, world verts, uv, light, plus the
free-edge list that marks the holes) and copies DIR/page_NNN.png for each page.
Reminder baked into the notes: in these pages V=0 is the TOP of the image.
"""
import argparse
import json
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "track_studio"))
sys.path.insert(0, HERE)


def _faces_and_edges(o):
    from collections import defaultdict
    faces = []
    edge_count = defaultdict(int)

    def key(p):
        return (round(p[0]), round(p[1]), round(p[2]))

    for pi, p in enumerate(o["prims"]):
        vs = p["mesh"]["vertices"]
        cur = 0
        for c in p["mesh"]["commands"]:
            tri, quad = int(c["tri"]), int(c["quad"])
            page = c["texture_page_id"]
            groups = [range(cur + t * 3, cur + t * 3 + 3) for t in range(tri)]
            qb = cur + tri * 3
            groups += [range(qb + q * 4, qb + q * 4 + 4) for q in range(quad)]
            for g in groups:
                idx = list(g)
                verts = [{"pos": [round(vs[k]["pos"][m], 1) for m in range(3)],
                          "uv": [round(vs[k]["tex"][m], 4) for m in range(2)],
                          "light": vs[k]["light"] & 0xFFFFFFFF} for k in idx]
                faces.append({"prim": pi, "role": p.get("role"),
                              "page": page, "n": len(idx), "v": verts})
                ids = [key(vs[k]["pos"]) for k in idx]
                for i in range(len(ids)):
                    a, b = ids[i], ids[(i + 1) % len(ids)]
                    if a != b:
                        edge_count[frozenset((a, b))] += 1
            cur += tri * 3 + quad * 4
    free = [sorted(list(e)) for e, n in edge_count.items() if n == 1]
    return faces, free


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--level", type=int, required=True)
    ap.add_argument("--idx", type=int, required=True)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    import td5_track_studio as st
    lms = st._landmarks(args.level)
    if not (0 <= args.idx < len(lms)):
        sys.exit("no landmark %d on level %d (have %d)" % (args.idx, args.level, len(lms)))
    o = lms[args.idx]
    lm_id = "L%d.lm%02d" % (args.level, args.idx)

    os.makedirs(args.out, exist_ok=True)
    faces, free = _faces_and_edges(o)
    pages = sorted({p["pages"][0] for p in o["prims"]})
    doc = {
        "id": lm_id,
        "note": "world coords; V=0 is the TOP of each page; write new faces into "
                "re/assets/library/authored_fills.json keyed by this id",
        "aabb": [round(v, 1) for v in o["aabb"]],
        "extent": [round(v, 1) for v in o["extent"]],
        "pages": pages,
        "nface": len(faces),
        "faces": faces,
        "free_edges": free,     # edges used by ONE face -- the hole boundaries
    }
    with open(os.path.join(args.out, "faces.json"), "w", encoding="utf-8") as f:
        json.dump(doc, f, indent=0)

    pdir = os.path.join(HERE, "..", "assets", "levels",
                        "level%03d" % args.level, "textures.src", "pages")
    copied = 0
    for pg in pages:
        src = os.path.join(pdir, "page_%03d.png" % pg)
        if os.path.isfile(src):
            shutil.copy(src, os.path.join(args.out, "page_%03d.png" % pg))
            copied += 1
    print("%s: %d faces, %d free edges, %d/%d pages -> %s"
          % (lm_id, len(faces), len(free), copied, len(pages), args.out))


if __name__ == "__main__":
    main()
