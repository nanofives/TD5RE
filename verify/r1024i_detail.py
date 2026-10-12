#!/usr/bin/env python3
"""[R1024 I] For chosen building meshes: which surface MESHES (e,s,kind,page) does the roof footprint /
wall base overlap, and by how many m^2 (roof raster) or wall samples.

  python verify/r1024i_detail.py LEVELDIR e,s [e,s ...]"""
import sys, os, math, collections
from PIL import Image, ImageDraw
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import geo_bld_audit as A
import r1023c_bld_intrude as R

lvl = sys.argv[1]
want = [tuple(int(t) for t in s.split(",")) for s in sys.argv[2:]]
b, meshes = A.load_level(lvl)
surfs = []   # (q, y, e, s, kind, page, cls)
for m in meshes:
    if R.is_billboard(b, m):
        continue
    for typ, page, vs in A.faces(b, m):
        if len(vs) != 4:
            continue
        h = R.horizontal(vs)
        if not h:
            continue
        if page in R.CARR_PAGES and m["kind"] not in R.SKIP_SURF_KINDS:
            surfs.append((h[0], h[1], m["e"], m["s"], m["kind"], page, "C"))
        elif page == R.PAVE_PAGE and R.narrow(h[0]):
            surfs.append((h[0], h[1], m["e"], m["s"], m["kind"], page, "P"))
RAS = 50.0
for key in want:
    m = next(mm for mm in meshes if (mm["e"], mm["s"]) == key)
    base = min(v[1] for v in A.verts(b, m))
    roofs = []
    for typ, page, vs in A.faces(b, m):
        ys = [v[1] for v in vs]; pts = [(v[0], v[2]) for v in vs]
        if len(vs) in (3, 4) and min(ys) >= base + 1500 and max(ys) - min(ys) < 400 and abs(A.poly_area(pts)) > 1:
            roofs.append(pts)
    print("== e%d s%d %s %s nv=%d cmds=%s c=%s r=%d base=%d" % (m["e"], m["s"], m["kind"], R.bclass(m), m["nv"], m["cmds"][:3], [round(v) for v in m["c"]], m["r"], base))
    if not roofs:
        print("   no roof faces"); 
    if roofs:
        xs = [p[0] for r in roofs for p in r]; zs = [p[1] for r in roofs for p in r]
        x0, z0, x1, z1 = min(xs), min(zs), max(xs), max(zs)
        print("   footprint bbox x[%d,%d] z[%d,%d] = %.1f x %.1f m" % (x0, x1, z0, z1, (x1 - x0) / 430, (z1 - z0) / 430))
        W = int((x1 - x0) / RAS) + 2; H = int((z1 - z0) / RAS) + 2
        foot = Image.new("L", (W, H), 0); d = ImageDraw.Draw(foot)
        for r in roofs:
            d.polygon([((p[0] - x0) / RAS, (p[1] - z0) / RAS) for p in r], fill=1)
        fb = foot.tobytes(); area = sum(fb) * RAS * RAS / 430 / 430
        print("   roof area %.1f m2" % area)
        per = collections.defaultdict(float)
        union = {"C": Image.new("L", (W, H), 0), "P": Image.new("L", (W, H), 0)}
        for (q, y, e, s, kind, page, cl) in surfs:
            xq = [p[0] for p in q]; zq = [p[1] for p in q]
            if max(xq) < x0 or min(xq) > x1 or max(zq) < z0 or min(zq) > z1:
                continue
            if not (base - 500 <= y <= base + R.GRADE):
                continue
            mk = Image.new("L", (W, H), 0); ImageDraw.Draw(mk).polygon([((p[0] - x0) / RAS, (p[1] - z0) / RAS) for p in q], fill=1)
            kb = mk.tobytes()
            n = sum(1 for i in range(len(fb)) if fb[i] and kb[i])
            if n:
                union[cl].paste(1, mask=mk.point(lambda v: 255 if v else 0))
                per[(cl, e, s, kind, page)] += n * RAS * RAS / 430 / 430
        for k, v in sorted(per.items(), key=lambda kv: -kv[1]):
            print("   overlap %-1s e%-4d s%-3d %-10s p%-4d %.1f m2" % (k[0], k[1], k[2], k[3], k[4], v))
        import numpy as np
        from scipy.ndimage import distance_transform_edt
        F = np.frombuffer(foot.tobytes(), np.uint8).reshape(H, W) > 0
        for cl in "CP":
            U = np.frombuffer(union[cl].tobytes(), np.uint8).reshape(H, W) > 0
            both = F & U
            if not both.any():
                continue
            # penetration depth = largest distance (into the overlap) from its boundary
            dt = distance_transform_edt(np.pad(both, 1))
            print("   penetration %s: %.2f m (max inscribed radius of the overlap), area %.1f m2" % (cl, dt.max() * RAS / 430.0, both.sum() * RAS * RAS / 430 / 430))
