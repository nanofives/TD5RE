#!/usr/bin/env python3
"""regen_td6_clean_strip.py — regenerate a TD6 track's strip + routes as a CLEAN
CONTINUOUS road from the NATIVE TD6 strip.dat (683 spans, type-1, width ~1163-1971),
instead of the broken junction-synthesised 2763-span version that fabricates
narrow(558)/wide(2328) "junction zones" the car wedges in (London span-317).

Native TD6 has NO strip junctions — routing is the 4 spline*.td6 files (RE'd in
TD6.exe: spline loader FUN_0042a370, route-walker FUN_0042b430). So a single
continuous pass of the native strip is the correct geometry for a one-route AI race.

Emits the dat-retirement editable-source format (td5_assetsrc.c td5_src_encode_strip):
  header[5] | pre_span_hex | spans[11-field->24B] | pre_vertex_hex | vertices[3->6B] | tail_hex
Writes strip.json/stripb.json + left/right/leftb/rightb.trk.csv + patches levelinf
total_span_count. Run from repo root."""
import os, sys, json, struct, zipfile, importlib.util

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
TD6_LEVELS = os.path.join(ROOT, "Test Drive 6", "LEVELS")
ASSETS = os.path.join(ROOT, "re", "assets", "levels")

# reuse the converter's geometry-derived route builder
_spec = importlib.util.spec_from_file_location("conv", os.path.join(os.path.dirname(__file__), "convert_td6_tracks.py"))
conv = importlib.util.module_from_spec(_spec); _spec.loader.exec_module(conv)


def build_clean_strip_json(strip: bytes):
    span_off, span_cnt, vtx_off, vtx_cnt, total = struct.unpack_from("<5I", strip, 0)
    spans = []
    for i in range(span_cnt):
        o = span_off + i * 24
        b1, b2, lane = strip[o + 1], strip[o + 2], strip[o + 3]
        vl, vr = struct.unpack_from("<HH", strip, o + 4)
        ox, oy, oz = struct.unpack_from("<iii", strip, o + 12)
        # CLEAN continuous: type-1 road; type-9 start + type-10 end sentinels; links walk +/-1
        t, ln, lp = 1, i + 1, i - 1
        if i == 0:           t, lp = 9, -1
        if i == span_cnt - 1: t, ln = 10, -1
        spans.append([t, b1, b2, lane, vl, vr, ln, lp, ox, oy, oz])
    verts = [list(struct.unpack_from("<hhh", strip, vtx_off + i * 6)) for i in range(vtx_cnt)]
    pre_span = strip[20:span_off].hex()           # native metadata block, kept verbatim
    new_vtx_off = span_off + 24 * span_cnt         # no expansion -> tighter offset
    header = [span_off, span_cnt, new_vtx_off, vtx_cnt, span_cnt]  # ring = total = span_cnt
    return {"_format": "td5_strip", "_version": 1, "header": header,
            "pre_span_hex": pre_span, "spans": spans,
            "pre_vertex_hex": "", "vertices": verts, "tail_hex": ""}, span_cnt


def write_csv(table: bytes, path: str):
    n = len(table) // 3
    with open(path, "w") as f:
        f.write("# td5 route table -- one row per span: lane0,lane1,lane2 (raw u8)\n")
        for i in range(n):
            f.write(f"{table[i*3]},{table[i*3+1]},{table[i*3+2]}\n")


def regen(level_num: int, td6_level: int, circuit: bool):
    zp = os.path.join(TD6_LEVELS, f"level{td6_level:03d}.zip")
    z = zipfile.ZipFile(zp)
    outdir = os.path.join(ASSETS, f"level{level_num:03d}")
    for src_name, json_name, l_csv, r_csv in [
        ("strip.dat", "strip.json", "left.trk.csv", "right.trk.csv"),
        ("stripb.dat", "stripb.json", "leftb.trk.csv", "rightb.trk.csv")]:
        strip = z.read(src_name)
        sj, ncnt = build_clean_strip_json(strip)
        with open(os.path.join(outdir, json_name), "w") as f:
            json.dump(sj, f)
        left, right = conv.build_routes(strip, circuit=circuit)
        write_csv(left, os.path.join(outdir, l_csv))
        write_csv(right, os.path.join(outdir, r_csv))
        print(f"  {json_name}: {ncnt} spans (was junction-expanded); routes {len(left)//3} rows")
    # patch levelinf total_span_count -> new span count; clear checkpoints (old spans invalid)
    lip = os.path.join(outdir, "levelinf.json")
    if os.path.exists(lip):
        li = json.load(open(lip, encoding="utf-8"))
        strip = z.read("strip.dat"); span_cnt = struct.unpack_from("<I", strip, 4)[0]
        if isinstance(li.get("total_span_count"), dict):
            li["total_span_count"]["value"] = span_cnt
        if isinstance(li.get("checkpoint_count"), dict):
            li["checkpoint_count"]["value"] = 0
        if isinstance(li.get("checkpoint_spans"), dict):
            li["checkpoint_spans"]["value"] = [0]*7
        json.dump(li, open(lip, "w", encoding="utf-8"))
        print(f"  levelinf: total_span_count={span_cnt}, checkpoints cleared")


if __name__ == "__main__":
    # London = asset level012, TD6 level 12, P2P (circuit=False)
    targets = [(12, 12, False)]  # extend to all TD6 tracks once verified
    if len(sys.argv) > 1:
        targets = [(int(sys.argv[1]), int(sys.argv[2]), sys.argv[3] == "1")]
    for (lvl, td6, circ) in targets:
        print(f"level{lvl:03d} (TD6 {td6}, circuit={circ}):")
        regen(lvl, td6, circ)
    print("done.")
