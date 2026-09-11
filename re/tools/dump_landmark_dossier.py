#!/usr/bin/env python3
"""Build a LANDMARK DOSSIER: everything needed to identify a set piece and judge
where it should be filled.

The split this serves: deterministic code READS (topology, symmetry, rims,
exposure) and an orchestrating model DETERMINES (what building is this, which
proposals are real, what a missing piece means). Coordinates alone cannot say
that a cathedral's chapel ring is radial while its perimeter wall is not, so the
dossier pairs the measurements with things a model can actually look at:
elevations from four sides plus a top-down, and the texture pages themselves.

    python re/tools/dump_landmark_dossier.py --level 23 --idx 0 --out DIR

Writes DIR/dossier.json, DIR/view_*.png and DIR/pages/page_NNN.png.
Add --no-render to skip Playwright and emit the analysis only.
"""
import argparse
import json
import math
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "track_studio"))
sys.path.insert(0, HERE)

import numpy as np


def _jsonable(o):
    if isinstance(o, (np.floating, float)):
        return round(float(o), 2)
    if isinstance(o, (np.integer, int)):
        return int(o)
    if isinstance(o, np.ndarray):
        return [_jsonable(v) for v in o.tolist()]
    if isinstance(o, (list, tuple)):
        return [_jsonable(v) for v in o]
    if isinstance(o, dict):
        return {k: _jsonable(v) for k, v in o.items()}
    return o


def shell_table(T, axis, k, tol=300.0, unit="comp"):
    """unit="comp" groups by welded shell; unit="prim" by the ORIGINAL authoring
    unit. Prim granularity matters: welding merges a chapel into the plaza it
    stands on (on L23.lm00 the west drum, prim 5, ends up inside a 14-prim blob),
    and no per-shell score can separate them because the chapel->base join shares
    as many edges as a legitimate chapel tier stack. The shipped prims already
    carry the segmentation the artists authored, so proposals are made on those.
    """
    """Per-shell facts, including how strongly each shell OBEYS the symmetry.

    Participation is the discriminator the whole-model score cannot give: the
    chapel shells have rotational siblings, the plaza and perimeter walls do not.
    Without it the detector happily proposes rotating a ground slab by 90 degrees.
    """
    import td5_holefill as hf
    C = hf._face_centroids(T)
    pages = np.array([f["page"] for f in T["F"]])
    R = hf._rot(C, axis, 2 * math.pi / k)
    D0 = np.linalg.norm(C[None, :, :] - R[:, None, :], axis=2)
    D = np.where(pages[None, :] == pages[:, None], D0, np.inf)
    matched = D.min(axis=1) <= tol

    # Page-strict matching UNDERSTATES a chapel's participation whenever the art
    # was deliberately varied per sibling -- on St Basil's the four cardinal drums
    # each carry a different dome page, so a real sibling never matches. Dropping
    # the page requirement fixes that but lets any flat ground quad match any
    # other under rotation (a slab measured 0.00 -> 1.00). Restricting BOTH sides
    # to non-ground faces above the podium is what makes geometry-only matching
    # safe: the plaza is excluded by role and height, not by texture.
    ys = C[:, 1]
    podium = float(ys.min() + 0.15 * (ys.max() - ys.min()))
    elig = np.array([f["role"] != "ground" for f in T["F"]]) & (ys > podium)
    Dg = np.where(elig[None, :], D0, np.inf)
    matched_geo = (Dg.min(axis=1) <= tol) & elig

    out = []
    for c in sorted({f[unit] for f in T["F"]}):
        idx = [i for i, f in enumerate(T["F"]) if f[unit] == c]
        if not idx:
            continue
        P = np.array([p for i in idx for p in T["F"][i]["pos"]], float)
        cen = P.mean(0)
        rad = math.hypot(cen[0] - axis[0], cen[2] - axis[1])
        az = (math.degrees(math.atan2(cen[2] - axis[1], cen[0] - axis[0])) + 360) % 360
        roles = {}
        pg = {}
        for i in idx:
            roles[T["F"][i]["role"]] = roles.get(T["F"][i]["role"], 0) + 1
            pg[int(T["F"][i]["page"])] = pg.get(int(T["F"][i]["page"]), 0) + 1
        out.append({
            "shell": c, "faces": len(idx),
            "participation": round(float(matched[idx].mean()), 3),
            "participation_geo": (round(float(matched_geo[idx].sum())
                                        / max(1, int(elig[idx].sum())), 3)
                                  if elig[idx].any() else 0.0),
            "eligible": int(elig[idx].sum()),
            "centre": cen, "radius_from_axis": rad, "azimuth": az,
            "y_band": [float(P[:, 1].min()), float(P[:, 1].max())],
            "footprint": [float(P[:, 0].max() - P[:, 0].min()),
                          float(P[:, 2].max() - P[:, 2].min())],
            "roles": roles,
            "pages": sorted(pg, key=lambda p: -pg[p]),
        })
    out.sort(key=lambda s: -s["faces"])
    return out


RENDER_HTML = """<!doctype html><meta charset="utf-8"><title>dossier</title>
<style>html,body{margin:0;background:#141821;overflow:hidden}canvas{display:block}</style>
<script type="importmap">{"imports":{
 "three":"./vendor/three.module.js","three/addons/":"./vendor/jsm/"}}</script>
<script type="module">
import * as THREE from 'three';
import { GLTFLoader } from 'three/addons/loaders/GLTFLoader.js';
const q=new URLSearchParams(location.search), W=900,H=760;
const rend=new THREE.WebGLRenderer({antialias:true,preserveDrawingBuffer:true});
rend.setSize(W,H); document.body.appendChild(rend.domElement);
const scene=new THREE.Scene(); scene.background=new THREE.Color(0x141821);
const cam=new THREE.PerspectiveCamera(40,W/H,10,600000);
const cache={};
function tex(p){ if(cache[p])return cache[p];
  const t=new THREE.TextureLoader().load(`./pages/page_${String(p).padStart(3,'0')}.png`);
  t.colorSpace=THREE.SRGBColorSpace; t.wrapS=t.wrapT=THREE.RepeatWrapping;
  t.flipY=false; t.magFilter=THREE.NearestFilter; return cache[p]=t; }
const ax=+q.get('ax'), az=+q.get('az'), a=+q.get('azim')*Math.PI/180;
const r=+q.get('r'), ey=+q.get('ey'), ty=+q.get('ty');
cam.position.set(ax+r*Math.cos(a), ey, az+r*Math.sin(a)); cam.lookAt(ax,ty,az);
new GLTFLoader().load('./model.glb',(g)=>{
  g.scene.traverse(o=>{ if(!o.isMesh)return;
    const pg=o.userData.page ?? (o.parent&&o.parent.userData.page);
    o.material=new THREE.MeshBasicMaterial({map:pg!=null?tex(pg):null,
      color:pg!=null?0xffffff:0x8b9099, side:THREE.DoubleSide, alphaTest:0.5}); });
  scene.add(g.scene);
  const wait=()=>{ const pend=Object.values(cache).some(t=>!t.image||!t.image.width);
    rend.render(scene,cam); if(pend) return setTimeout(wait,60);
    rend.render(scene,cam); document.title='READY'; }; wait(); });
</script>
"""


def render_views(out, axis_local, height, radius):
    """Four elevations plus a top-down, so the model can SEE the silhouette."""
    import functools
    import http.server
    import socketserver
    import threading
    try:
        from playwright.sync_api import sync_playwright
    except Exception as e:
        print("  (render skipped: %s)" % e)
        return []
    H = functools.partial(http.server.SimpleHTTPRequestHandler, directory=out)

    class Q(socketserver.TCPServer):
        allow_reuse_address = True

        def log_message(self, *a):
            pass

    srv = Q(("127.0.0.1", 0), H)
    port = srv.server_address[1]
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    views = [("east", 0, radius * 2.1, height * 0.55, height * 0.42),
             ("north", 270, radius * 2.1, height * 0.55, height * 0.42),
             ("west", 180, radius * 2.1, height * 0.55, height * 0.42),
             ("south", 90, radius * 2.1, height * 0.55, height * 0.42),
             ("aerial", 35, radius * 2.4, height * 1.5, height * 0.35)]
    made = []
    with sync_playwright() as pw:
        br = pw.chromium.launch(args=["--use-gl=angle", "--use-angle=swiftshader",
                                      "--enable-unsafe-swiftshader", "--hide-scrollbars"])
        pg = br.new_page(viewport={"width": 900, "height": 760})
        for name, azim, r, ey, ty in views:
            pg.goto("http://127.0.0.1:%d/render.html?ax=%f&az=%f&azim=%d&r=%f&ey=%f&ty=%f"
                    % (port, axis_local[0], axis_local[1], azim, r, ey, ty))
            try:
                pg.wait_for_function("document.title==='READY'", timeout=40000)
            except Exception:
                print("  (timeout on %s)" % name)
            p = os.path.join(out, "view_%s.png" % name)
            pg.screenshot(path=p)
            made.append(os.path.basename(p))
        br.close()
    srv.shutdown()
    return made


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--level", type=int, required=True)
    ap.add_argument("--idx", type=int, required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--no-render", action="store_true")
    args = ap.parse_args()

    import td5_track_studio as st
    import td5_holefill as hf

    lms = st._landmarks(args.level)
    if not (0 <= args.idx < len(lms)):
        sys.exit("no landmark %d on level %d (have %d)" % (args.idx, args.level, len(lms)))
    o = lms[args.idx]
    lm_id = "L%d.lm%02d" % (args.level, args.idx)
    os.makedirs(args.out, exist_ok=True)
    os.makedirs(os.path.join(args.out, "pages"), exist_ok=True)

    T = hf.topology(o["prims"])
    sym = hf.find_axis_and_order(T)
    axis, k = sym["axis"], sym["k"]
    C = hf._face_centroids(T)
    pages_arr = np.array([f["page"] for f in T["F"]])
    orders = [{"k": kk, "match": round(hf._match_score(C, pages_arr, axis, kk, sym["tol"]), 3)}
              for kk in (2, 3, 4, 5, 6, 8)]
    rims = hf.boundary_loops(T)
    tris = hf.triangles(T)
    expo = hf.backface_exposure(tris)
    shells = shell_table(T, axis, k, sym["tol"])
    props = hf.missing_copies(T, axis, k, tol=sym["tol"], min_faces=3)
    part = {s["shell"]: s["participation"] for s in shells}
    for p in props:
        p["participation"] = part.get(p["shell"], 0.0)
        p.pop("face_idx", None)

    P = np.array([p for f in T["F"] for p in f["pos"]], float)
    lo, hi = P.min(0), P.max(0)
    pg_use = {}
    for f in T["F"]:
        pg_use.setdefault(int(f["page"]), {"faces": 0, "shells": set()})
        pg_use[int(f["page"])]["faces"] += 1
        pg_use[int(f["page"])]["shells"].add(f["comp"])

    doc = {
        "id": lm_id, "level": args.level,
        "_note": ("Read the views to identify the building, then judge each proposal. "
                  "participation = how strongly that shell obeys the detected symmetry; "
                  "a plaza or perimeter wall scores low and its proposals are false."),
        "geometry": {"faces": len(T["F"]), "welded_verts": int(len(T["V"])),
                     "shells": T["ncomp"], "nonmanifold_edges": T["nonmanifold"],
                     "free_edges": len(T["free"]),
                     "aabb": [lo, hi], "extent": (hi - lo)},
        "symmetry": {"axis_xz": axis, "order_k": k, "score": round(sym["score"], 3),
                     "tolerance": sym["tol"], "by_order": orders},
        "backface_exposure": {"frac": round(expo["frac"], 4),
                              "rays": expo["rays"], "backface": expo["backface"],
                              "_note": "fraction of camera rays whose nearest hit is a "
                                       "BACK face -- 0 = interior never visible"},
        "rims": [{"verts": r["n"], "closed": r["closed"],
                  "planar_resid": round(r["planar_resid"], 4),
                  "extent": r["extent"], "centre": r["centre"]} for r in rims],
        "shells": shells,
        "pages": [{"page": p, "faces": v["faces"], "shells": sorted(v["shells"])}
                  for p, v in sorted(pg_use.items(), key=lambda kv: -kv[1]["faces"])],
        "proposals": props,
    }

    pdir = os.path.join(HERE, "..", "assets", "levels",
                        "level%03d" % args.level, "textures.src", "pages")
    for p in pg_use:
        src = os.path.join(pdir, "page_%03d.png" % p)
        if os.path.isfile(src):
            shutil.copy(src, os.path.join(args.out, "pages", "page_%03d.png" % p))

    views = []
    if not args.no_render:
        gl = st._lib()
        glb = st.build_landmark_glb(args.level, args.idx, fill=False)
        open(os.path.join(args.out, "model.glb"), "wb").write(glb)
        open(os.path.join(args.out, "render.html"), "w", encoding="utf-8").write(RENDER_HTML)
        vend = os.path.join(HERE, "track_studio", "vendor")
        dst = os.path.join(args.out, "vendor")
        if os.path.isdir(vend) and not os.path.isdir(dst):
            shutil.copytree(vend, dst)
        # prefab frame: centred on the aabb in XZ, base y=0
        cx, cz = (lo[0] + hi[0]) / 2.0, (lo[2] + hi[2]) / 2.0
        views = render_views(args.out, (axis[0] - cx, axis[1] - cz),
                             float(hi[1] - lo[1]),
                             float(max(hi[0] - lo[0], hi[2] - lo[2])) / 2.0)
    doc["views"] = views

    with open(os.path.join(args.out, "dossier.json"), "w", encoding="utf-8") as f:
        json.dump(_jsonable(doc), f, indent=1)
    print("%s: %d faces, %d shells, k=%d (%.3f), exposure %.3f, %d proposals, %d views -> %s"
          % (lm_id, len(T["F"]), T["ncomp"], k, sym["score"], expo["frac"],
             len(props), len(views), args.out))


if __name__ == "__main__":
    main()
