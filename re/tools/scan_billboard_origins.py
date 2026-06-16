#!/usr/bin/env python3
"""scan_billboard_origins.py — STATIC diagnosis of camera-facing billboard
(tree) meshes in a track's models.bin (== MODELS.DAT byte-exact passthrough).

Authoritative parse: mirrors td5_track_parse_models_dat (td5_track.c:5335) +
the render billboard gate (td5_render.c:2517).

  Directory: DWORD[0]=entry_count, then (offset,size) u32 pairs (format A,
  table@4); or table@0 with count=DWORD[1]/8 (format B). entry_offset=f0
  (monotonic), entry_size=f1. Each block @offset: [sub_mesh_count u32]
  [mesh_off u32 * sub_count], mesh_off relative to block start (fallback:
  blob-relative). Each mesh = 0x38 header.

  Billboard gate (renderer): int16 texture_page_id @ +2 == 1 or == 2.
  Frustum CULL uses bounding_center (+0x10/14/18), NOT origin (+0x1C). The
  TD6 origin-fold bug bakes folded verts so BOTH center and origin collapse
  to ~0 -> billboards pile at world origin -> culled everywhere but start.

Read-only; never writes. Usage:
  python scan_billboard_origins.py <models.bin> [<models.bin> ...]
  python scan_billboard_origins.py --all
"""
import os, sys, struct, glob

# int16 render_type, int16 tex/btag, i32 cmd, i32 vcount, f radius,
# f bc(x,y,z), f origin(x,y,z), u32 resv, u32 cmds_off, verts_off, norms_off
MESH_HDR = "<hhii fffffff IIII"
assert struct.calcsize(MESH_HDR) == 0x38


def parse_meshes(models: bytes):
    """Return list of mesh dicts using the runtime's directory walk."""
    out = []
    if not models or len(models) < 8:
        return out
    d0, d1 = struct.unpack_from("<2I", models, 0)
    size = len(models)
    # Format detection (matches td5_track.c)
    if 0 < d0 < 10000 and d1 == 4 + d0 * 8 and 4 + d0 * 8 <= size:
        count, tbl = d0, 4
    elif (0 < d0 < 10000 and 4 + d0 * 8 <= size and
          d1 >= 4 + d0 * 8 and d1 < size and d1 + 4 <= size and
          1 <= struct.unpack_from("<I", models, d1)[0] <= 256):
        count, tbl = d0, 4
    elif d1 > 0 and (d1 & 7) == 0 and d1 <= size:
        count, tbl = d1 // 8, 0
    else:
        return out
    table_end = tbl + count * 8
    for i in range(count):
        tb = tbl + i * 8
        if tb + 8 > size:
            break
        f0, f1 = struct.unpack_from("<2I", models, tb)
        # entry_offset = f0 (runtime: f0 is 100% monotonic/valid)
        entry_offset, entry_size = f0, f1
        if entry_offset == 0 or entry_offset >= size:
            continue
        if entry_size == 0 or entry_offset + entry_size > size:
            # estimate from next entry
            nxt = size
            if i + 1 < count and tb + 8 + 8 <= size:
                nf0, nf1 = struct.unpack_from("<2I", models, tb + 8)
                cand = nf0 if (nf0 > entry_offset and nf0 <= size) else (
                    nf1 if (nf1 > entry_offset and nf1 <= size) else size)
                nxt = cand
            entry_size = nxt - entry_offset
        if entry_size < 8 or entry_offset + entry_size > size:
            continue
        block_base = entry_offset
        sub = struct.unpack_from("<I", models, block_base)[0]
        if sub == 0 or sub > 256 or entry_size < 4 + sub * 4:
            continue
        for j in range(sub):
            mo = struct.unpack_from("<I", models, block_base + 4 + j * 4)[0]
            if mo == 0:
                continue
            if mo + 0x38 <= entry_size:
                mabs = block_base + mo
            elif mo + 0x38 <= size:
                mabs = mo
            else:
                continue
            if mabs + 0x38 > size:
                continue
            (rt, btag, ncmd, nv, rad, bcx, bcy, bcz,
             ox, oy, oz, resv, oc, ov, on) = struct.unpack_from(MESH_HDR, models, mabs)
            # runtime validity gate
            if ncmd < 0 or ncmd > 4096 or nv < 0 or nv > 65536:
                continue
            out.append(dict(off=mabs, rt=rt, btag=btag, ncmd=ncmd, nv=nv,
                            bc=(bcx, bcy, bcz), origin=(ox, oy, oz)))
    return out


def scan(path: str):
    with open(path, "rb") as f:
        models = f.read()
    meshes = parse_meshes(models)
    btag_hist = {}
    bb = []  # billboard meshes (btag 1/2)
    for m in meshes:
        btag_hist[m["btag"]] = btag_hist.get(m["btag"], 0) + 1
        if m["btag"] in (1, 2):
            bb.append(m)
    print(f"=== {path}  ({len(models)} bytes, {len(meshes)} valid meshes) ===")
    nz = {k: v for k, v in sorted(btag_hist.items()) if k in (0, 1, 2)}
    print(f"  btag(+2) counts for 0/1/2: {nz}   (billboard meshes: {len(bb)})")
    if not bb:
        print("  >> CLASS (c): NO billboard meshes (btag==1/2). Trees either "
              "not tagged as billboards OR rendered as solid geometry.")
        return ("c", 0, 0)
    # bounding_center is the cull field; report it + origin
    def spread(key):
        xs = [m[key][0] for m in bb]; zs = [m[key][2] for m in bb]
        at0 = sum(1 for m in bb if abs(m[key][0]) < 1.0 and abs(m[key][2]) < 1.0)
        return (min(xs), max(xs), max(xs) - min(xs),
                min(zs), max(zs), max(zs) - min(zs), at0)
    cminx, cmaxx, csx, cminz, cmaxz, csz, c_at0 = spread("bc")
    ominx, omaxx, osx, ominz, omaxz, osz, o_at0 = spread("origin")
    print(f"  bbox_center: X[{cminx:.0f}..{cmaxx:.0f}] spanX={csx:.0f}  "
          f"Z[{cminz:.0f}..{cmaxz:.0f}] spanZ={csz:.0f}  at(0,0)={c_at0}/{len(bb)}")
    print(f"  origin     : X[{ominx:.0f}..{omaxx:.0f}] spanX={osx:.0f}  "
          f"Z[{ominz:.0f}..{omaxz:.0f}] spanZ={osz:.0f}  at(0,0)={o_at0}/{len(bb)}")
    # classify on the CULL field (bounding_center)
    if c_at0 == len(bb) or (c_at0 >= len(bb) * 0.9 and csx + csz < 1000):
        v = "a"
        print(f"  >> CLASS (a) FOLDED: {c_at0}/{len(bb)} billboard centers at "
              f"(0,0). THE BUG — trees pile at start, culled elsewhere.")
    elif c_at0 > len(bb) * 0.3:
        v = "a-mixed"
        print(f"  >> CLASS (a-mixed): {c_at0}/{len(bb)} centers folded, rest spread.")
    else:
        v = "b"
        print(f"  >> CLASS (b) OK: billboard centers spread along track "
              f"({c_at0}/{len(bb)} at origin). Not the missing-tree cause.")
    return (v, len(bb), c_at0)


def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__); return
    if args[0] == "--all":
        base = os.path.join(os.path.dirname(__file__), "..", "assets", "levels")
        paths = sorted(glob.glob(os.path.join(base, "level*", "models.bin")))
    else:
        paths = args
    results = {}
    for p in paths:
        try:
            results[p] = scan(p)
        except Exception as e:
            import traceback
            print(f"=== {p}: ERROR {e} ===")
            traceback.print_exc()
        print()
    if len(paths) > 1:
        print("---- SUMMARY (class a/a-mixed = bug; b = ok; c = no billboard tag) ----")
        for p in sorted(results):
            v, n, a = results[p]
            lvl = os.path.basename(os.path.dirname(p))
            print(f"  {lvl:10s} class={v:8s} billboards={n:5d} folded={a}")


if __name__ == "__main__":
    main()
