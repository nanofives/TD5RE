#!/usr/bin/env python3
"""Synthesize reverse-direction track data (stripb.json + leftb.trk.csv +
rightb.trk.csv) for a native TD5 CIRCUIT from its forward strip.json.

A circuit is a closed ring, so its reverse geometry is derivable from the
forward strip -- the original game only shipped authored STRIPB for
point-to-point tracks, leaving the 7 native circuits forward-only. This tool
fills that gap using the transform reverse-engineered from the native P2P
forward/reverse pairs (see re/tools/analyze_reverse_strip.py) and the engine's
span geometry (td5_track.c:785-818):

  rev[k] = fwd[(R - k) mod R]           # reverse the main ring (R = header[1])
                                        # rev[0]=fwd[0] keeps the start/finish
                                        # line; matches runtime spawn formula
                                        # rev_ss = rev_ring - fwd_ss (td5_game.c)
  swap left_vertex_index <-> right_vertex_index   # near<->far rows: reverses
                                        # travel within the span. Boundary rows
                                        # stay shared (far(rev k)==near(rev k+1)).
  type swap 2<->3, 5<->6, 8<->11        # direction-encoding span types
                                        # (transition entry/exit, junction
                                        # fwd/bwd). 1/4/7/9/10 are symmetric.
  surf / packed(lanes) / origin: unchanged.  vertex table: copied verbatim.

Routes (LEFTB/RIGHTB.TRK as .csv) are built by the SAME build_routes() logic the
forward/TD6 routes use, run over the reversed ring -- center() is swap-invariant
so the reversed span order alone yields reverse-direction headings.

Only the main ring [0, R) is reversed; appended branch spans [R, nspans) are
copied verbatim. Circuits with junctions (type 8/11) or explicit fwd/bwd links
get a WARNING -- their branch links are not remapped here.

Usage:
  python gen_circuit_reverse.py re/assets/levels/level025 [more dirs...]
  python gen_circuit_reverse.py --no-rail-swap ...   # A/B the rail-swap choice
"""
import json, os, sys, math

# span field indices in the JSON 11-tuple
T, SURF, RS, PK, LVI, RVI, FWD, BWD, OX, OY, OZ = range(11)

TYPE_SWAP = {2: 3, 3: 2, 5: 6, 6: 5, 8: 11, 11: 8}


def reverse_strip(spans, verts, ring, type_swap=True):
    """Build a self-contained reversed-ring strip by RE-EMITTING vertices in
    reverse order -- the transform the original TD5 level tools used (the native
    P2P STRIPB has its own independent vertex table, not an index remap).

    A pure index swap of left/right_vertex_index reverses neither the lateral
    (left<->right) nor longitudinal (near<->far) vertex order, so the engine's
    surface-containment walker (td5_track.c UpdateActorTrackPosition @0x004440F0)
    drops the car through the road. A true 180 deg reversal needs BOTH flips, so
    we emit fresh vertex rows.

    Vertex layout is TYPE-INDEPENDENT (verified against the data): a span's NEAR
    row is verts [lvi, rvi) and its FAR row is the NEXT span's near row
    [lvi(j+1), rvi(j+1)). Widths can differ (lane-transition trapezoids:
    e.g. a 3-vertex near widening to a 5-vertex far). We honour the actual
    per-row widths so transitions aren't twisted into invisible walls.

      rev[k] = fwd[j],  j = (ring - k) mod R          # reverse the ring order
      new NEAR row = fwd FAR row  (= near row of j+1), lateral-reversed
      new FAR  row = fwd NEAR row (= [lvi(j),rvi(j))), lateral-reversed

    Boundaries coincide exactly: far(rev k) == near(rev k+1) are the same forward
    boundary row. lane nibble = wider row's lane count; type encodes widen
    (near<far) vs narrow (near>far) vs uniform, with offset-free types (2 widen /
    5 narrow / 1 uniform) so the emitted-at-base rows are read without a LUT
    shift.
    """
    new_spans, new_verts = [], []

    def near_row(j):
        """forward near-row vertex index range [lvi, rvi) of span j."""
        return spans[j][LVI], spans[j][RVI]          # (start, end-exclusive)

    def emit_reversed(lo, hi):
        """Append verts forward[hi-1 .. lo] (lateral-reversed); return new base
        index and the row width."""
        start = len(new_verts)
        for v in range(hi - 1, lo - 1, -1):
            new_verts.append(list(verts[v]))
        return start, hi - lo

    for k in range(ring):
        j = (ring - k) % ring
        s = list(spans[j])                       # copy span record
        n_lo, n_hi = near_row(j)                  # fwd near row of j
        f_lo, f_hi = near_row((j + 1) % ring)     # fwd far row of j (= near of j+1)
        new_li, w_near = emit_reversed(f_lo, f_hi)   # new near = fwd far (reversed)
        new_ri, w_far  = emit_reversed(n_lo, n_hi)   # new far  = fwd near (reversed)
        s[LVI] = new_li
        s[RVI] = new_ri
        # lane nibble = wider row's lane count; preserve the span's other PK bits
        lanes = max(w_near, w_far) - 1
        s[PK] = (s[PK] & 0xF0) | (lanes & 0x0F)
        # offset-free type: 1 uniform, 2 widen (near<far), 5 narrow (near>far)
        if   w_near == w_far: s[T] = 1
        elif w_near <  w_far: s[T] = 2
        else:                 s[T] = 5
        s[FWD] = 0xFFFF                          # sequential links (clean ring)
        s[BWD] = 0xFFFF
        new_spans.append(s)

    # closing span: duplicate of rev[0] (mirrors the forward strip's span[R]).
    new_spans.append(list(new_spans[0]))
    return new_spans, new_verts


def build_routes(spans, verts, ring, circuit=True):
    """Reverse-route LEFTB/RIGHTB byte rows -- mirrors
    convert_td6_tracks.build_routes / gen_td6_reverse_routes, operating on the
    parsed JSON spans/verts of the (already reversed) ring."""
    SHOULDER_FRAC = 0.25
    K = 3

    def center(i):
        s = spans[i]
        lv = verts[s[LVI]]; rv = verts[s[RVI]]
        return (s[OX] + (lv[0] + rv[0]) / 2.0, s[OZ] + (lv[2] + rv[2]) / 2.0)

    def heading_byte(s):
        if circuit:
            ax, az = center((s - K) % ring)
            bx, bz = center((s + K) % ring)
        else:
            lo = s - K if s >= K else 0
            hi = s + K if s + K <= ring - 1 else ring - 1
            if hi == lo:
                return 4
            ax, az = center(lo); bx, bz = center(hi)
        dx, dz = bx - ax, bz - az
        if dx == 0 and dz == 0:
            return 4
        h12 = int(round((math.atan2(dx, dz) / (2 * math.pi)) * 4096)) & 0xFFF
        return max(4, min(253, int(round(h12 * 256 / 0x102C))))

    left, right = [], []
    for s in range(ring):
        b = heading_byte(s)
        lanes = spans[s][PK] & 0x0F
        L = lanes if lanes >= 1 else 1
        sh = 0 if lanes <= 2 else max(1, int(round(lanes * SHOULDER_FRAC)))
        left_byte = 128
        right_byte = max(left_byte + 8, min(255, int(round((L - sh) / L * 255))))
        left.append((left_byte, b, 255))
        right.append((right_byte, b, 255))
    return left, right


def write_strip_json(path, src_obj, rev_spans, rev_verts, ring):
    """Emit stripb.json in the exact strip_tool.py layout (byte-faithful
    re-pack via td5_assetsrc Tier 2). Header is recomputed because the
    re-emitted vertex table has a different vertex count than the forward
    strip; pre/post hex sections are preserved verbatim."""
    pre_span_n = len(src_obj["pre_span_hex"]) // 2
    pre_vtx_n  = len(src_obj["pre_vertex_hex"]) // 2
    nspans, nverts = len(rev_spans), len(rev_verts)
    span_off = 20 + pre_span_n
    vtx_off  = span_off + 24 * nspans + pre_vtx_n
    # header = [span_table_off, span_count(ring), vertex_table_off, nverts, nspans]
    hdr = [span_off, ring, vtx_off, nverts, nspans]
    sp = ",\n".join(json.dumps(s, separators=(",", ":")) for s in rev_spans)
    vx = ",\n".join(json.dumps(v, separators=(",", ":")) for v in rev_verts)
    text = (
        "{\n"
        '"_format":"td5_strip","_version":1,\n'
        f'"header":{json.dumps(hdr)},\n'
        f'"pre_span_hex":"{src_obj["pre_span_hex"]}",\n'
        f'"spans":[\n{sp}\n],\n'
        f'"pre_vertex_hex":"{src_obj["pre_vertex_hex"]}",\n'
        f'"vertices":[\n{vx}\n],\n'
        f'"tail_hex":"{src_obj["tail_hex"]}"\n'
        "}\n"
    )
    open(path, "w", encoding="utf-8").write(text)


def write_route_csv(path, rows, name):
    lines = [f"# td5 reverse route ({name}) -- one row per span: lane0,lane1,lane2 (raw u8)"]
    lines += [f"{a},{b},{c}" for (a, b, c) in rows]
    open(path, "w", encoding="utf-8").write("\n".join(lines) + "\n")


def process(level_dir, type_swap=True):
    sp_path = os.path.join(level_dir, "strip.json")
    if not os.path.isfile(sp_path):
        print(f"  {level_dir}: no strip.json -> skip"); return False
    obj = json.load(open(sp_path, encoding="utf-8"))
    spans = obj["spans"]; verts = obj["vertices"]
    ring = obj["header"][1]
    nspans = len(spans)

    # diagnostics / safety
    from collections import Counter
    types = Counter(s[T] for s in spans)
    junction = types.get(8, 0) + types.get(11, 0)
    expl = sum(1 for s in spans[:ring] if s[FWD] != 0xFFFF or s[BWD] != 0xFFFF)
    name = os.path.basename(level_dir)
    if junction or expl:
        print(f"  {name}: WARNING branched circuit (junction8/11={junction}, "
              f"explicit_links={expl}) -- branch links NOT remapped; main-ring "
              f"reverse only.")

    nontype1 = sum(1 for s in spans[:ring] if s[T] != 1)
    if nontype1:
        print(f"  {name}: NOTE {nontype1}/{ring} ring spans are non-type-1 "
              f"(LUT row offsets) -- re-emission assumes type-1 row width; verify.")

    rev_spans, rev_verts = reverse_strip(spans, verts, ring, type_swap=type_swap)
    left, right = build_routes(rev_spans, rev_verts, ring, circuit=True)

    write_strip_json(os.path.join(level_dir, "stripb.json"), obj,
                     rev_spans, rev_verts, ring)
    write_route_csv(os.path.join(level_dir, "leftb.trk.csv"), left, "left")
    write_route_csv(os.path.join(level_dir, "rightb.trk.csv"), right, "right")
    print(f"  {name}: stripb.json ({len(rev_spans)} spans, ring={ring}, "
          f"{len(rev_verts)} verts) + leftb/rightb.trk.csv ({ring} rows)  "
          f"type_swap={type_swap}")
    return True


def main():
    args = sys.argv[1:]
    type_swap = "--no-type-swap" not in args
    dirs = [a for a in args if not a.startswith("--")]
    if not dirs:
        print(__doc__); sys.exit(2)
    for d in dirs:
        process(d, type_swap=type_swap)


if __name__ == "__main__":
    main()
