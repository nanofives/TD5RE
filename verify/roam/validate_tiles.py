#!/usr/bin/env python
"""GEO FREE ROAM F1-T5: independent structural check of ROAMTILE files.

A second reader of the format (Python, from the layout in td5_roam_tile.h), so a
disagreement with the C reader shows up. Per file: header + section table, the
FNV-1a trailer, record counts against section sizes, every index in range (span
vertex rows, links and their reciprocity, triangle vertices, wall edges and their
primitive, EDGE spans, NODE triangles and arms, ARM cut span, GRID start[] and
items), NAME table. Prints a one-line verdict per problem class and a total.

Usage:  python -I verify/roam/validate_tiles.py <dir of ROAMTILE_*.BIN> [--no-trailer]
Exit 0 when every file is clean.
"""
import os
import struct
import sys

HDR = 64
ORDER = ["SPAN", "VERT", "TRI ", "BEDG", "EDGE", "NODE", "ARM ", "GRID", "SIGN", "NAME", "STMP"]
SIZE = {"SPAN": 24, "VERT": 6, "TRI ": 22, "BEDG": 8, "EDGE": 48, "NODE": 32, "ARM ": 16, "SIGN": 12}


def fnv1a32(buf):
    h = 2166136261
    for b in buf:
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return h


def check(path, do_trailer, problems):
    with open(path, "rb") as f:
        buf = f.read()
    name = os.path.basename(path)

    def bad(msg):
        problems.setdefault(msg.split(":")[0], []).append(name + ": " + msg)

    if len(buf) < HDR + 4 or buf[:8] != b"TD5ROAM1":
        bad("header: bad magic or too short")
        return None
    version, rules, ihash, tx, tz, ox, oz, nsect = struct.unpack_from("<IIIiiiiI", buf, 8)
    if nsect != len(ORDER):
        bad("header: %d sections" % nsect)
        return None
    sec = {}
    for i, tag in enumerate(ORDER):
        t, off, nb, cnt = struct.unpack_from("<4sIII", buf, HDR + 16 * i)
        t = t.decode("latin1")
        if t != tag:
            bad("table: section %d is %r, want %r" % (i, t, tag))
            return None
        if off % 16 or off + nb > len(buf) - 4:
            bad("table: section %s offset %d bytes %d out of range" % (tag, off, nb))
            return None
        if tag in SIZE and cnt * SIZE[tag] != nb:
            bad("table: section %s count %d x %d != %d" % (tag, cnt, SIZE[tag], nb))
            return None
        sec[tag] = (off, nb, cnt)
    if do_trailer:
        tr = struct.unpack_from("<I", buf, len(buf) - 4)[0]
        if tr != fnv1a32(buf[:-4]):
            bad("trailer: fnv mismatch")

    def recs(tag, fmt):
        off, nb, cnt = sec[tag]
        return list(struct.iter_unpack(fmt, buf[off:off + nb]))

    spans = recs("SPAN", "<BB2sHHhhiii")
    verts = sec["VERT"][2]
    tris = recs("TRI ", "<HHHHBBiii")
    bedg = recs("BEDG", "<HHHBB")
    edges = recs("EDGE", "<Qqq4BbBHiHHH2BBBH")
    nodes = recs("NODE", "<qiiiBBHHHHH")
    arms = recs("ARM ", "<QhHBBBB")
    signs = recs("SIGN", "<qBBH")
    noff, nbytes, ncnt = sec["NAME"]
    names = [] if nbytes == 0 else buf[noff:noff + nbytes].split(b"\0")[:-1]
    if len(names) != ncnt:
        bad("names: %d names, table says %d" % (len(names), ncnt))
    nspan, ntri, nbed = len(spans), len(tris), len(bedg)

    # spans: vertex rows, links, reciprocity
    for i, s in enumerate(spans):
        stype, surf, pad, lv, rv, ln, lp = s[:7]
        L = pad[1] & 0x0F
        if stype != 1:
            bad("span: %d has type %d" % (i, stype))
        if L < 1 or lv + L >= verts or rv + L >= verts:
            bad("span: %d vertex rows %d/%d lanes %d beyond %d verts" % (i, lv, rv, L, verts))
        if ln != -1:
            if not 0 <= ln < nspan:
                bad("link: span %d next %d out of range" % (i, ln))
            elif spans[ln][6] != i:
                bad("link: span %d next %d but its prev is %d" % (i, ln, spans[ln][6]))
        if lp != -1:
            if not 0 <= lp < nspan:
                bad("link: span %d prev %d out of range" % (i, lp))
            elif spans[lp][5] != i:
                bad("link: span %d prev %d but its next is %d" % (i, lp, spans[lp][5]))
    for i, t in enumerate(tris):
        if max(t[0:3]) >= verts:
            bad("tri: %d vertex %d beyond %d" % (i, max(t[0:3]), verts))
        if t[3] >= len(nodes):
            bad("tri: %d patch %d beyond %d nodes" % (i, t[3], len(nodes)))
    for i, b in enumerate(bedg):
        if max(b[0:2]) >= verts:
            bad("bedge: %d vertex beyond table" % i)
        prim = b[2]
        if prim & 0x8000:
            if (prim & 0x7FFF) >= ntri:
                bad("bedge: %d triangle %d beyond %d" % (i, prim & 0x7FFF, ntri))
        elif prim >= nspan:
            bad("bedge: %d span %d beyond %d" % (i, prim, nspan))
        if b[3] not in (1, 2):
            bad("bedge: %d kind %d" % (i, b[3]))
    for i, e in enumerate(edges):
        key, ek0, ek1, kl, ln, ow, sf, layer, fl, nid, length, sfirst, scount, row0 = e[:14]
        if sfirst + scount > nspan or scount < 1:
            bad("edge: %d spans %d+%d beyond %d" % (i, sfirst, scount, nspan))
        if nid != 0xFFFF and nid >= len(names):
            bad("edge: %d name_id %d beyond %d names" % (i, nid, len(names)))
        if ln < 1 or ln > 12:
            bad("edge: %d lanes %d" % (i, ln))
    tri_cover = 0
    arm_cover = 0
    for i, n in enumerate(nodes):
        tf, tc, af = n[7], n[8], n[9]      # q key, i x y z, B narm, B pad, H flags, then tri/arm ranges
        if tf + tc > ntri:
            bad("node: %d triangles %d+%d beyond %d" % (i, tf, tc, ntri))
        tri_cover += tc
        if af + n[4] > len(arms):
            bad("node: %d arms %d+%d beyond %d" % (i, af, n[4], len(arms)))
        arm_cover += n[4]
    if nodes and tri_cover != ntri:
        bad("node: triangle ranges cover %d of %d" % (tri_cover, ntri))
    if arm_cover != len(arms):
        bad("node: arm ranges cover %d of %d" % (arm_cover, len(arms)))
    run_of = {}
    for e in edges:
        for s in range(e[11], e[11] + e[12]):
            run_of[s] = e[0]
    for i, a in enumerate(arms):
        if a[2] != 0xFFFF and a[2] >= nspan:
            bad("arm: %d cut_span %d beyond %d" % (i, a[2], nspan))
        elif a[2] != 0xFFFF and run_of.get(a[2]) != a[0]:
            bad("arm: %d cut_span %d belongs to edge %s, the arm is on edge %d" % (i, a[2], run_of.get(a[2]), a[0]))
        if not 0 <= a[1] < 4096:
            bad("arm: %d bearing %d" % (i, a[1]))
    for i, s in enumerate(signs):
        if s[1] >= 64:
            bad("sign: %d arm %d" % (i, s[1]))

    # GRID
    goff, gbytes, gcnt = sec["GRID"]
    words = struct.unpack_from("<%dI" % (gbytes // 4), buf, goff)
    nx, nz, cell, margin = words[0], words[1], words[2], words[3]
    ncell = nx * nz
    start = words[5:5 + ncell + 1]
    items = words[5 + ncell + 1:]
    if nx != 32 + 2 * margin or nz != nx or cell != 3440:
        bad("grid: nx %d nz %d cell %d margin %d" % (nx, nz, cell, margin))
    if len(items) != gcnt or start[-1] != len(items):
        bad("grid: %d items, table %d, start[] ends at %d" % (len(items), gcnt, start[-1]))
    if any(start[i] > start[i + 1] for i in range(ncell)):
        bad("grid: start[] not monotonic")
    lim = (nspan, ntri, nbed)
    for it in items:
        k, idx = it >> 30, it & 0x3FFFFFFF
        if k > 2 or idx >= lim[k]:
            bad("grid: item kind %d index %d beyond %d" % (k, idx, lim[k] if k < 3 else -1))
            break
    return dict(tx=tx, tz=tz, nspan=nspan, nvert=verts, ntri=ntri, nbedge=nbed, nedge=len(edges),
                nnode=len(nodes), narm=len(arms), nsign=len(signs), nnames=len(names), margin=margin,
                nitem=len(items), bytes=len(buf), hash=ihash)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    do_trailer = "--no-trailer" not in sys.argv
    if not args:
        print(__doc__)
        return 2
    d = args[0]
    files = sorted(f for f in os.listdir(d) if f.startswith("ROAMTILE_") and f.endswith(".BIN"))
    problems = {}
    tot = {}
    n_ok = 0
    for f in files:
        r = check(os.path.join(d, f), do_trailer, problems)
        if r:
            n_ok += 1
            for k, v in r.items():
                if k not in ("tx", "tz", "hash"):
                    tot[k] = tot.get(k, 0) + v
    print("%d file(s) parsed of %d in %s" % (n_ok, len(files), d))
    print("totals:", ", ".join("%s %d" % kv for kv in sorted(tot.items())))
    nprob = sum(len(v) for v in problems.values())
    for k, v in sorted(problems.items()):
        print("PROBLEM %-8s %d  e.g. %s" % (k, len(v), v[0]))
    print("trailer checked" if do_trailer else "trailer NOT checked", "-> %s" % ("CLEAN" if not nprob and n_ok == len(files) else "%d problem(s)" % nprob))
    return 0 if not nprob and n_ok == len(files) else 1


if __name__ == "__main__":
    sys.exit(main())
