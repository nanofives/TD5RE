#!/usr/bin/env python3
"""geo_pick_lookup.py -- name the mesh behind a free-cam PICK code.

The picker (td5_pick.c) copies a line like

    level091 L91 e20 s12 p446 pos 118221,320,-14346 r2020 v24 c1

and for a SHIPPED track that is all it can say, because the emitter name comes
from td5_trackgen_mesh_kind_name, which the picker only calls for the AUTO slot
(td5_trackgen_is_auto_slot == slot 60). A geo track is slot 61+, so every pick
Mariano sends from one arrives without the one field that identifies the
emitter. This reads the same MESHTAG.BIN sidecar offline and supplies it.

Look up by index, by position, or both -- position is the robust one when the
pick came from a different build, because entry/slot indices move whenever the
generator emits a different number of meshes.

  python scripts/geo_pick_lookup.py re/assets/levels/level091 --entry 20 --slot 12
  python scripts/geo_pick_lookup.py re/assets/levels/level091 --pos 118221,320,-14346
  python scripts/geo_pick_lookup.py re/assets/levels/level091 --pick "level091 L91 e20 s12 p446 pos 118221,320,-14346 r2020 v24 c1"
"""
import argparse, math, os, re, struct, sys

HDR, CMD, VTX = 0x38, 16, 44
MTAG_MAGIC = 0x4741544D

# td5_trackgen_internal.h, enum TG_GK_*
GK = ["other", "skirt", "road", "branchroad", "tunnel", "gantry", "endwall",
      "deck", "water", "coast", "decal", "city", "block", "cross", "flora",
      "parktree", "terrain", "building", "prop", "rail", "branchside"]


def kind_name(k):
    if k == 0xFF:
        return "(untagged)"
    return GK[k] if 0 <= k < len(GK) else "kind#%d" % k


def load_meshtag(lvl):
    p = os.path.join(lvl, "MESHTAG.BIN")
    if not os.path.exists(p):
        return None, 0, 0
    b = open(p, "rb").read()
    magic, ver, seed, nent, stride = struct.unpack_from("<IIIII", b, 0)
    if magic != MTAG_MAGIC:
        return None, 0, 0
    return b[20:], nent, stride


def meshes(lvl):
    """Yield (entry, slot, cx, cy, cz, radius, nvtx, ncmd, pages)."""
    b = open(os.path.join(lvl, "MODELS.DAT"), "rb").read()
    (nent,) = struct.unpack_from("<I", b, 0)
    for e in range(nent):
        eoff, esize = struct.unpack_from("<II", b, 4 + e * 8)
        if eoff <= 0 or eoff >= len(b):
            continue
        (nmesh,) = struct.unpack_from("<I", b, eoff)
        for s in range(nmesh):
            (mo,) = struct.unpack_from("<I", b, eoff + 4 + s * 4)
            m = eoff + mo
            if m + HDR > len(b):
                continue
            magic, flags, ncmd, nvtx = struct.unpack_from("<HHII", b, m)
            radius, cx, cy, cz = struct.unpack_from("<ffff", b, m + 12)
            (cmdoff,) = struct.unpack_from("<I", b, m + 0x2C)
            pages = []
            for c in range(min(ncmd, 64)):
                co = m + cmdoff + c * CMD
                if co + CMD > len(b):
                    break
                _d, page, _z, _t, _q, _z2 = struct.unpack_from("<HHIHHI", b, co)
                if page not in pages:
                    pages.append(page)
            yield e, s, cx, cy, cz, radius, nvtx, ncmd, pages


def report(rows, tags, nent, stride, label):
    print(label)
    for (e, s, cx, cy, cz, r, nv, nc, pg, d) in rows:
        k = 0xFF
        if tags is not None and e < nent and s < stride:
            k = tags[e * stride + s]
        extra = "" if d is None else "  dist %.0f" % d
        print("  e%-4d s%-4d %-12s pos %.0f,%.0f,%.0f r%.0f v%d c%d pages %s%s"
              % (e, s, kind_name(k), cx, cy, cz, r, nv, nc,
                 "+".join(str(p) for p in pg), extra))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("level")
    ap.add_argument("--entry", type=int)
    ap.add_argument("--slot", type=int)
    ap.add_argument("--pos", help="x,y,z from the pick line")
    ap.add_argument("--pick", help="the whole copied pick line; parses e/s/pos")
    ap.add_argument("--near", type=int, default=6, help="nearest N by position")
    a = ap.parse_args()

    entry, slot, pos = a.entry, a.slot, None
    if a.pos:
        pos = tuple(float(v) for v in a.pos.split(","))
    if a.pick:
        m = re.search(r"\be(\d+)\s+s(\d+)", a.pick)
        if m:
            entry, slot = int(m.group(1)), int(m.group(2))
        m = re.search(r"pos\s+(-?\d+),(-?\d+),(-?\d+)", a.pick)
        if m:
            pos = tuple(float(m.group(i)) for i in (1, 2, 3))

    tags, nent, stride = load_meshtag(a.level)
    if tags is None:
        print("no MESHTAG.BIN in %s -- kinds unavailable" % a.level, file=sys.stderr)
    all_m = list(meshes(a.level))
    print("%s: %d mesh(es), MESHTAG %d entries x %d slots"
          % (a.level, len(all_m), nent, stride))

    if entry is not None and slot is not None:
        hit = [t + (None,) for t in all_m if t[0] == entry and t[1] == slot]
        report(hit or [], tags, nent, stride,
               "-- by index e%d s%d --" % (entry, slot))
        if not hit:
            print("  (no such mesh in THIS build -- indices move between builds)")

    if pos:
        scored = []
        for t in all_m:
            d = math.dist((t[2], t[3], t[4]), pos)
            scored.append(t + (d,))
        scored.sort(key=lambda q: q[-1])
        report(scored[:a.near], tags, nent, stride,
               "-- nearest %d to pos %s --" % (a.near, ",".join("%.0f" % v for v in pos)))


if __name__ == "__main__":
    main()
