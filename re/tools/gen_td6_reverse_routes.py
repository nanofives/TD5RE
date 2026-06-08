#!/usr/bin/env python3
"""Generate reverse AI routes (LEFTB.TRK / RIGHTB.TRK) for migrated TD6 tracks
from their already-converted STRIPB.DAT, using the SAME build_routes() logic the
forward routes use. Idempotent; only touches the 11 TD6 output levels (faithful
TD5 tracks ship their own authored reverse routes and are left alone).

Run with one or more target asset roots, e.g.:
  python gen_td6_reverse_routes.py /path/to/main/re/assets/levels /path/to/wt/re/assets/levels
"""
import os, sys, struct, math

SPAN_STRIDE = 24
VERT_STRIDE = 6
SHOULDER_FRAC = 0.25

# TD6 output level -> is_circuit (single source of truth = k_td6_menu_slots in
# td5_asset.c: finish_span>0 => P2P, finish_span==0 => circuit).
TD6_LEVELS = {
    7: True,  18: True, 19: True, 20: True, 21: True, 22: True,   # circuits
    8: False, 9: False, 10: False, 11: False, 12: False,          # point-to-point
}


def build_routes(strip: bytes, circuit: bool = True):
    """EXACT copy of convert_td6_tracks.build_routes (kept in sync) so the
    reverse routes are byte-identical to what a full reconversion would emit."""
    span_off, span_cnt, vtx_off, vtx_cnt, total = struct.unpack_from("<5I", strip, 0)

    def vtx(vi):
        x, y, z = struct.unpack_from("<hhh", strip, vtx_off + vi * VERT_STRIDE)
        return x, z

    def span_fields(i):
        o = span_off + i * SPAN_STRIDE
        lv, rv = struct.unpack_from("<HH", strip, o + 4)
        lanes = strip[o + 3] & 0x0F
        ox, oy, oz = struct.unpack_from("<iii", strip, o + 12)
        return lv, rv, lanes, ox, oz

    def center(i):
        lv, rv, lanes, ox, oz = span_fields(i)
        lx, lz = vtx(lv)
        rx, rz = vtx(rv)
        return (ox + (lx + rx) / 2.0, oz + (lz + rz) / 2.0)

    left = bytearray(span_cnt * 3)
    right = bytearray(span_cnt * 3)
    K = 3

    def heading_byte(s):
        if circuit:
            ax, az = center((s - K) % span_cnt)
            bx, bz = center((s + K) % span_cnt)
        else:
            lo = s - K if s >= K else 0
            hi = s + K if s + K <= span_cnt - 1 else span_cnt - 1
            if hi == lo:
                return 4
            ax, az = center(lo)
            bx, bz = center(hi)
        dx, dz = bx - ax, bz - az
        if dx == 0 and dz == 0:
            return 4
        h12 = int(round((math.atan2(dx, dz) / (2 * math.pi)) * 4096)) & 0xFFF
        return max(4, min(253, int(round(h12 * 256 / 0x102C))))

    for s in range(span_cnt):
        b = heading_byte(s)
        _lv, _rv, lanes, _ox, _oz = span_fields(s)
        L = lanes if lanes >= 1 else 1
        sh = 0 if lanes <= 2 else max(1, int(round(lanes * SHOULDER_FRAC)))
        left_byte = 128
        right_byte = max(left_byte + 8, min(255, int(round((L - sh) / L * 255))))
        left[s * 3 + 0] = left_byte
        left[s * 3 + 1] = b
        left[s * 3 + 2] = 255
        right[s * 3 + 0] = right_byte
        right[s * 3 + 1] = b
        right[s * 3 + 2] = 255
    return bytes(left), bytes(right)


def process_root(levels_root: str):
    if not os.path.isdir(levels_root):
        print(f"  SKIP (not a dir): {levels_root}")
        return 0
    n = 0
    for lv, circuit in sorted(TD6_LEVELS.items()):
        d = os.path.join(levels_root, f"level{lv:03d}")
        spb = os.path.join(d, "STRIPB.DAT")
        if not os.path.isfile(spb):
            print(f"  level{lv:03d}: no STRIPB.DAT -> skip")
            continue
        data = open(spb, "rb").read()
        left, right = build_routes(data, circuit=circuit)
        open(os.path.join(d, "LEFTB.TRK"), "wb").write(left)
        open(os.path.join(d, "RIGHTB.TRK"), "wb").write(right)
        kind = "circuit" if circuit else "p2p"
        print(f"  level{lv:03d} ({kind}): LEFTB/RIGHTB.TRK <- STRIPB ({len(left)} B each)")
        n += 1
    return n


if __name__ == "__main__":
    roots = sys.argv[1:] or ["re/assets/levels"]
    total = 0
    for r in roots:
        print(f"== {r} ==")
        total += process_root(r)
    print(f"Done: {total} level(s) updated across {len(roots)} root(s).")
