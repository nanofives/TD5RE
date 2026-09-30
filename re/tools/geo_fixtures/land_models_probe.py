"""[GEO LAND] Offline MODELS.DAT texel-density probe.

Answers the one question a framedump can only answer by eye: how many WORLD
UNITS one texture repeat covers on a face, and how ANISOTROPIC that is (a
stretched strip is a face whose two principal texel densities disagree).

Method, per face: fit the affine map from the face's own plane to UV from its
first triangle, take the two singular values of the 2x2 Jacobian. Their
reciprocals are world-units-per-repeat along the two principal directions;
their ratio is the stretch factor. A face that tiles isotropically reads
stretch 1.0; the synthetic park lawn is the reference at 1500 units/repeat
both ways (td5_tg_streets.c tg_block_emit_park).

  python land_models_probe.py <MODELS.DAT> [--pages 65,44,2,66] [--spe 4]
                              [--span-min N] [--span-max N] [--per-mesh]

MODELS.DAT format: see verify/r12geom_dumpmodels.py.
"""
import math
import struct
import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from land_pages import page_table                       # noqa: E402

HDR, CMD, VTX = 0x38, 16, 44


def _svals(j):
    """Singular values of a 2x2 matrix [[a,b],[c,d]]."""
    a, b, c, d = j
    e = (a * a + b * b + c * c + d * d) * 0.5
    f = math.sqrt(max(0.0, ((a * a + b * b - c * c - d * d) * 0.5) ** 2
                      + (a * c + b * d) ** 2))
    s1 = math.sqrt(max(0.0, e + f))
    s2 = math.sqrt(max(0.0, e - f))
    return s1, s2


def face_density(p, t):
    """(units_per_repeat_min, units_per_repeat_max) for one triangle, or None."""
    (x0, y0, z0), (x1, y1, z1), (x2, y2, z2) = p[0], p[1], p[2]
    ax, ay, az = x1 - x0, y1 - y0, z1 - z0
    bx, by, bz = x2 - x0, y2 - y0, z2 - z0
    la = math.sqrt(ax * ax + ay * ay + az * az)
    if not la > 1e-6:
        return None
    ex, ey, ez = ax / la, ay / la, az / la
    nx = ay * bz - az * by
    ny = az * bx - ax * bz
    nz = ax * by - ay * bx
    ln = math.sqrt(nx * nx + ny * ny + nz * nz)
    if not ln > 1e-9:
        return None
    nx, ny, nz = nx / ln, ny / ln, nz / ln
    fx = ny * ez - nz * ey
    fy = nz * ex - nx * ez
    fz = nx * ey - ny * ex
    # plane coords of the two edge vectors
    m = (la, bx * ex + by * ey + bz * ez, 0.0, bx * fx + by * fy + bz * fz)
    det = m[0] * m[3] - m[1] * m[2]
    if abs(det) < 1e-9:
        return None
    inv = (m[3] / det, -m[1] / det, -m[2] / det, m[0] / det)
    n = (t[1][0] - t[0][0], t[2][0] - t[0][0],
         t[1][1] - t[0][1], t[2][1] - t[0][1])
    j = (n[0] * inv[0] + n[1] * inv[2], n[0] * inv[1] + n[1] * inv[3],
         n[2] * inv[0] + n[3] * inv[2], n[2] * inv[1] + n[3] * inv[3])
    s1, s2 = _svals(j)
    if not s2 > 1e-12:
        return None
    return 1.0 / s1, 1.0 / s2          # min, max units per repeat


def walk(path, spe=4):
    """Yield (entry, span0, mesh_index, page, faces) with faces as
    (points[list of (x,y,z)], uvs[list of (u,v)])."""
    b = open(path, 'rb').read()
    (nent,) = struct.unpack_from('<I', b, 0)
    for e in range(nent):
        eoff, esize = struct.unpack_from('<II', b, 4 + e * 8)
        if esize == 0:
            continue
        (nmesh,) = struct.unpack_from('<I', b, eoff)
        for i in range(nmesh):
            (mo,) = struct.unpack_from('<I', b, eoff + 4 + i * 4)
            m = eoff + mo
            magic, flags, ncmd, nvtx = struct.unpack_from('<HHII', b, m)
            if magic != 259:
                continue
            (cmdoff,) = struct.unpack_from('<I', b, m + 0x2C)
            (vtxoff,) = struct.unpack_from('<I', b, m + 0x30)
            vp, vt = [], []
            for v in range(nvtx):
                o = m + vtxoff + v * VTX
                vx, vy, vz = struct.unpack_from('<fff', b, o)
                uu, vv = struct.unpack_from('<ff', b, o + 28)
                vp.append((vx, vy, vz))
                vt.append((uu, vv))
            cur = 0
            for c in range(ncmd):
                co = m + cmdoff + c * CMD
                _d, page, _z, tri, quad, _z2 = struct.unpack_from('<HHIHHI', b, co)
                faces = []
                for k in range(tri):
                    faces.append(([vp[cur], vp[cur + 1], vp[cur + 2]],
                                  [vt[cur], vt[cur + 1], vt[cur + 2]]))
                    cur += 3
                for k in range(quad):
                    faces.append(([vp[cur], vp[cur + 1], vp[cur + 2], vp[cur + 3]],
                                  [vt[cur], vt[cur + 1], vt[cur + 2], vt[cur + 3]]))
                    cur += 4
                if faces:
                    yield (e, e * spe, i, page, flags, faces)


def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        return 2
    path = args[0]
    pt = page_table()
    want = None
    spe, smin, smax, per_mesh = 4, -1, 1 << 30, False
    i = 1
    while i < len(args):
        a = args[i]
        if a == "--pages":
            i += 1
            want = set(int(x) for x in args[i].split(","))
        elif a == "--spe":
            i += 1
            spe = int(args[i])
        elif a == "--span-min":
            i += 1
            smin = int(args[i])
        elif a == "--span-max":
            i += 1
            smax = int(args[i])
        elif a == "--per-mesh":
            per_mesh = True
        i += 1

    names = {}
    for k, v in pt.items():
        if k.startswith("TD5_TG_PAGE_"):
            names.setdefault(v, k[len("TD5_TG_PAGE_"):])
    names[pt["TD5_TG_PAGE_R3_BLOCK"] + 0] = "R3_BLOCK+0 lawn"
    names[pt["TD5_TG_PAGE_R3_BLOCK"] + 1] = "R3_BLOCK+1 hedge"
    names[pt["TD5_TG_PAGE_R3_BLOCK"] + 3] = "R3_BLOCK+3 roof"

    per_page = {}
    for entry, span0, mi, page, flags, faces in walk(path, spe):
        if span0 + spe - 1 < smin or span0 > smax:
            continue
        if want is not None and page not in want:
            continue
        for p, t in faces:
            d = face_density(p, t)
            if d is None:
                continue
            lo, hi = d
            rec = per_page.setdefault(page, [])
            rec.append((lo, hi, hi / lo if lo > 0 else 0.0, span0, mi))

    print("MODELS.DAT %s" % path)
    print("%-22s %6s | %10s %10s | %10s %10s | %8s %8s"
          % ("page", "faces", "u/rep p50", "u/rep p95", "stretch p50",
             "p95", "worst", "at span"))
    for page in sorted(per_page):
        rec = per_page[page]
        rec_lo = sorted(r[0] for r in rec)
        rec_hi = sorted(r[1] for r in rec)
        st = sorted(r[2] for r in rec)
        worst = max(rec, key=lambda r: r[2])
        n = len(rec)
        def pc(a, q):
            return a[min(n - 1, int(q * n))]
        print("%-22s %6d | %10.0f %10.0f | %10.2f %10.2f | %8.1f %8d"
              % ("%d %s" % (page, names.get(page, "")), n,
                 pc(rec_lo, 0.5), pc(rec_hi, 0.95), pc(st, 0.5), pc(st, 0.95),
                 worst[2], worst[3]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
