#!/usr/bin/env python3
"""Reverse-engineer the forward->reverse STRIP transform convention used by the
original TD5 level tools, by matching a native P2P track's forward strip.json to
its authored stripb.json via WORLD POSITION (the same physical road appears in
both, so nearest-center matching reveals the exact span-order / rail-swap /
type-remap / vertex convention).

Usage: python analyze_reverse_strip.py <levelNNN dir>
"""
import json, os, sys, math

def load(path):
    obj = json.load(open(path, encoding="utf-8"))
    spans = obj["spans"]          # [type,surf,rs,pk,lvi,rvi,fwd,bwd,ox,oy,oz]
    verts = obj["vertices"]       # [x,y,z] i16 relative to span origin
    return obj, spans, verts

def vtx(verts, i):
    if 0 <= i < len(verts):
        return verts[i]
    return None

def span_center(spans, verts, i):
    s = spans[i]
    lvi, rvi, ox, oy, oz = s[4], s[5], s[8], s[10], None  # use x,z plane
    oz = s[10]
    lv = vtx(verts, lvi); rv = vtx(verts, rvi)
    if lv is None or rv is None:
        return None
    cx = ox + (lv[0] + rv[0]) / 2.0
    cz = oz + (lv[2] + rv[2]) / 2.0
    return (cx, cz)

def rail_points(spans, verts, i):
    """world (x,z) of the left-rail base and right-rail base vertices."""
    s = spans[i]
    lvi, rvi, ox, oz = s[4], s[5], s[8], s[10]
    lv = vtx(verts, lvi); rv = vtx(verts, rvi)
    if lv is None or rv is None:
        return None, None
    L = (ox + lv[0], oz + lv[2])
    R = (ox + rv[0], oz + rv[2])
    return L, R

def main():
    d = sys.argv[1]
    fobj, fspans, fverts = load(os.path.join(d, "strip.json"))
    robj, rspans, rverts = load(os.path.join(d, "stripb.json"))
    print(f"== {d} ==")
    print(f"fwd header={fobj['header']}  spans={len(fspans)} verts={len(fverts)}")
    print(f"rev header={robj['header']}  spans={len(rspans)} verts={len(rverts)}")
    Nf, Nr = len(fspans), len(rspans)

    # main-ring span count = header[1]
    fring = fobj["header"][1]; rring = robj["header"][1]
    print(f"fwd ring(hdr[1])={fring}  rev ring(hdr[1])={rring}")

    # Precompute forward centers (main ring only)
    fc = []
    for i in range(min(fring, Nf)):
        c = span_center(fspans, fverts, i)
        fc.append(c)

    # For a sample of reverse spans, find nearest forward span center.
    def nearest_fwd(c):
        best, bd = -1, 1e30
        for j, cj in enumerate(fc):
            if cj is None: continue
            dx = c[0]-cj[0]; dz = c[1]-cj[1]
            dd = dx*dx+dz*dz
            if dd < bd: bd, best = dd, j
        return best, math.sqrt(bd)

    print("\nrev_idx -> nearest_fwd_idx (dist)   [expect rev k ~ fwd (ring-k)]")
    samples = [0,1,2,5,10,50,100, rring//4, rring//2, (3*rring)//4, rring-3, rring-2, rring-1]
    samples = [s for s in samples if 0 <= s < min(rring, Nr)]
    rows = []
    for k in samples:
        c = span_center(rspans, rverts, k)
        if c is None:
            print(f"  rev {k}: center None"); continue
        j, dist = nearest_fwd(c)
        rows.append((k, j, dist))
        # rail swap test: rev-left should equal fwd-right (and vice versa) if swapped
        rL, rR = rail_points(rspans, rverts, k)
        fL, fR = rail_points(fspans, fverts, j)
        def dd(a,b):
            return math.hypot(a[0]-b[0], a[1]-b[1]) if a and b else -1
        same = dd(rL,fL)+dd(rR,fR) if (rL and fL) else -1
        swap = dd(rL,fR)+dd(rR,fL) if (rL and fR) else -1
        print(f"  rev {k:5d} -> fwd {j:5d} (dist {dist:8.1f})  "
              f"types rev={rspans[k][0]} fwd={fspans[j][0]}  "
              f"surf rev={rspans[k][1]} fwd={fspans[j][1]}  "
              f"lanes rev={rspans[k][3]&0xF} fwd={fspans[j][3]&0xF}  "
              f"rail same={same:8.1f} swap={swap:8.1f}")

    # Summarize span-order relationship
    if rows:
        print("\nspan-order fit (rev k vs fwd j):")
        for k,j,dist in rows:
            print(f"  k={k:5d} j={j:5d}  ring-k={fring-k:5d}  ring-1-k={fring-1-k:5d}  Nf-k={Nf-k}")

    # type histogram
    from collections import Counter
    print("\nfwd type histogram:", dict(Counter(s[0] for s in fspans)))
    print("rev type histogram:", dict(Counter(s[0] for s in rspans)))
    print("fwd fwd/bwd link sample:", [(s[6],s[7]) for s in fspans[:4]])
    print("rev fwd/bwd link sample:", [(s[6],s[7]) for s in rspans[:4]])

if __name__ == "__main__":
    main()
