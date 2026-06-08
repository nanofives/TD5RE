#!/usr/bin/env python3
"""Extract synthesized checkpoint spans for migrated TD6 point-to-point tracks.

TD6.exe ships NO live checkpoint trigger data (RE'd 2026-06-04: CHECKPT.NUM is
loaded but never read; banner tex tables have zero code refs; the only per-track
span table drives fog/lighting). So the in-track numbered checkpoint-BANNER
meshes are pure decoration. To make them FUNCTIONAL in the source port we
SYNTHESIZE checkpoints from those banners' positions:

  1. find the numbered checkpoint-banner texture of each city,
  2. for each banner, take the centroid of the mesh COMMAND that uses it,
  3. map that to the nearest strip-span index,
  4. order by the banner's number -> the per-track checkpoint span list.

Each city names its banners differently (all visually confirmed against the
extracted TGAs):
  Paris/HongKong : 1one / 1two / 1three / 1four   (spelled numbers)
  New York       : 1.tga .. 4.tga                 (bare digits)
  Rome           : Check01a .. Check05a           ("Check0N")
  London         : Kstage1 .. Kstage4             ("KstageN")
NB earlier texture-NAME guesses were wrong décor: NY "ringo" = a RINGO'S
storefront, Paris "Post" = wall posters, London "flag" = national flags.

The 6 circuit tracks are lap-based and get no checkpoints. Offline analysis
only; prints a C-ready table. Output feeds td5_asset_td6_checkpoint_spans().
"""
import sys, os, re, struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import convert_td6_tracks as C

# (td6_source_level, output_level, name, start_span, finish_span)
P2P_TRACKS = [
    ( 0,  8, "PARIS",      20, 2827),
    ( 1,  9, "NEW YORK",   20, 2566),
    ( 2, 10, "ROME",       20, 2348),
    ( 3, 11, "HONG KONG",  20, 2097),
    ( 4, 12, "LONDON",     20, 2128),
]

_WORD = {"one": 1, "two": 2, "three": 3, "four": 4, "five": 5}


def checkpoint_number(texname):
    """Return the 1-based checkpoint number a banner texture encodes, or None."""
    b = texname.rsplit(".", 1)[0].lower()
    m = re.match(r"^\d*(one|two|three|four|five)$", b)      # Paris / HK : 1one..
    if m:
        return _WORD[m.group(1)]
    m = re.match(r"^([1-9])$", b)                            # New York   : 1..4
    if m:
        return int(m.group(1))
    m = re.match(r"^check0?([1-9])[ab]?$", b)                # Rome       : Check01..05
    if m:
        return int(m.group(1))
    m = re.match(r"^kstage([1-9])$", b)                      # London     : Kstage1..4
    if m:
        return int(m.group(1))
    return None


def span_centers(strip):
    span_off, span_cnt, vtx_off, vtx_cnt, total = struct.unpack_from("<5I", strip, 0)

    def vtx(vi):
        x, y, z = struct.unpack_from("<hhh", strip, vtx_off + vi * C.VERT_STRIDE)
        return x, z

    centers = []
    for i in range(span_cnt):
        o = span_off + i * C.SPAN_STRIDE
        lv, rv = struct.unpack_from("<HH", strip, o + 4)
        ox, oy, oz = struct.unpack_from("<iii", strip, o + 12)
        try:
            lx, lz = vtx(lv); rx, rz = vtx(rv)
        except struct.error:
            lx = lz = rx = rz = 0
        centers.append((ox + (lx + rx) / 2.0, oz + (lz + rz) / 2.0))
    return span_cnt, centers


def nearest_span(centers, x, z):
    best_i, best_d = 0, 1e30
    for i, (cx, cz) in enumerate(centers):
        d = (cx - x) ** 2 + (cz - z) ** 2
        if d < best_d:
            best_d, best_i = d, i
    return best_i, best_d ** 0.5


def banner_command_spans(models, strip, cp_pages):
    """For each checkpoint texture page, the span(s) of the mesh command(s)
    that use it (banner command centroid -> nearest span)."""
    span_cnt, centers = span_centers(strip)
    per = {}
    for ei, mlist in C._iter_blocks(models):
        if not mlist:
            continue
        for moff in mlist:
            if moff is None:
                continue
            cc = C._i32(models, moff + 4)
            cmds_off = moff + C._u32(models, moff + 0x2C)
            for c in range(cc):
                cmd = cmds_off + c * C.TD6_CSTRIDE
                if cmd + C.TD6_CSTRIDE > len(models):
                    break
                ic = C._i32(models, cmd + 0x0C)
                if ic <= 0 or ic % 3 != 0:
                    break
                tp = struct.unpack_from("<h", models, cmd + 2)[0] & 0xFFFF
                if tp not in cp_pages:
                    continue
                vcnt = C._i32(models, cmd + 0x08)
                voff = moff + C._i32(models, cmd + 0x10)
                if vcnt <= 0 or voff + vcnt * C.TD6_VSTRIDE > len(models):
                    continue
                sx = sz = 0.0
                for vi in range(vcnt):
                    sv = voff + vi * C.TD6_VSTRIDE
                    sx += C._f32(models, sv); sz += C._f32(models, sv + 8)
                sp, _ = nearest_span(centers, sx / vcnt, sz / vcnt)
                per.setdefault(tp, []).append(sp)
    return span_cnt, per


def cluster(spans, gap=30):
    """Collapse banner-command spans within `gap` into one checkpoint LOCATION
    (median). Robust to a banner texture being reused across several gantries
    (Rome's Check0N) — distinct gantry positions stay distinct."""
    if not spans:
        return []
    s = sorted(spans)
    groups = [[s[0]]]
    for v in s[1:]:
        if v - groups[-1][-1] <= gap:
            groups[-1].append(v)
        else:
            groups.append([v])
    return [g[len(g) // 2] for g in groups]


def main():
    print("/* td5_asset_td6_checkpoint_spans() source data — see file header */")
    for src, out, name, start, finish in P2P_TRACKS:
        zp = C.td6_level_zip(src)
        models = C.read_zip_entry(zp, "models.dat")
        strip = C.read_zip_entry(zp, "strip.dat")
        names = C._parse_textures_dir(C._td6_texture_zip(src))
        page_num = {i: checkpoint_number(n) for i, n in enumerate(names)
                    if checkpoint_number(n) is not None}
        span_cnt, per = banner_command_spans(models, strip, set(page_num))
        all_spans = [sp for sps in per.values() for sp in sps]
        # distinct checkpoint locations, interior to the race: drop anything at
        # or before the start and within ~60 spans of the finish (the latter is
        # the reverse-direction banner, e.g. Rome's Check01 at span ~2308).
        locs = [s for s in cluster(all_spans) if start < s < finish - 60]
        print(f"\n=== {name} (td6 src{src} -> out{out}, span_cnt={span_cnt}, "
              f"race {start}->{finish}) ===")
        print(f"   banner textures: " +
              ", ".join(sorted({names[p] for p in page_num})))
        for k, sp in enumerate(locs):
            print(f"   checkpoint {k + 1}: span {sp}")
        print(f"   /* case {out}: */  {{ {', '.join(map(str, locs))} }}  n={len(locs)}")


if __name__ == "__main__":
    main()
