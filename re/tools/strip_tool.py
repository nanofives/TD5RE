#!/usr/bin/env python3
"""
strip_tool.py -- export/import Test Drive 5 STRIP.DAT / STRIPB.DAT track
geometry to/from an editable, layout-faithful JSON, for the pack-on-load
retirement (td5_assetsrc.c, Tier 2).

File layout (little-endian), per re/include/td5_level_formats.h:
  header: 5 x uint32 [span_table_off, span_count, vertex_table_off,
                      secondary_count, auxiliary_count]
  jump region: bytes[20 .. span_table_off)  (optional jump-entry count + 6B
               entries; preserved verbatim as hex -- geometry stays editable)
  spans:    span_table_off .. vertex_table_off, 24 bytes each:
            u8 type,u8 surf,u8 reserved,u8 packed, u16 lvi,u16 rvi,
            u16 fwd,u16 bwd, i32 ox,i32 oy,i32 oz
  vertices: vertex_table_off .. EOF, 6 bytes each: i16 x,y,z

Round-trip is byte-exact by construction: the sections exactly tile [0,size),
and any non-conforming remainder is captured as pre_vertex_hex / tail_hex.

Usage:
  python strip_tool.py export <strip.dat> <out.json>
  python strip_tool.py import <in.json>  <out.dat>
"""

import json
import struct
import sys


def export(data: bytes) -> str:
    if len(data) < 20:
        raise ValueError(f"strip too small ({len(data)} bytes)")
    n = len(data)
    hdr = list(struct.unpack_from("<5I", data, 0))
    span_off, vtx_off = hdr[0], hdr[2]
    if not (20 <= span_off <= vtx_off <= n):
        raise ValueError(f"unexpected offsets span_off={span_off} "
                         f"vtx_off={vtx_off} size={n}")

    pre_span = data[20:span_off]

    span_region = data[span_off:vtx_off]
    nspans = len(span_region) // 24
    spans = []
    for i in range(nspans):
        o = i * 24
        st, sa, rs, pk = (span_region[o], span_region[o + 1],
                          span_region[o + 2], span_region[o + 3])
        lvi, rvi, fwd, bwd = struct.unpack_from("<4H", span_region, o + 4)
        ox, oy, oz = struct.unpack_from("<3i", span_region, o + 12)
        spans.append([st, sa, rs, pk, lvi, rvi, fwd, bwd, ox, oy, oz])
    pre_vertex = span_region[nspans * 24:]

    vtx_region = data[vtx_off:]
    nverts = len(vtx_region) // 6
    verts = [list(struct.unpack_from("<3h", vtx_region, i * 6))
             for i in range(nverts)]
    tail = vtx_region[nverts * 6:]

    # Hand-format: one span / vertex per line for editability.
    sp = ",\n".join(json.dumps(s, separators=(",", ":")) for s in spans)
    vx = ",\n".join(json.dumps(v, separators=(",", ":")) for v in verts)
    return (
        "{\n"
        f'"_format":"td5_strip","_version":1,\n'
        f'"header":{json.dumps(hdr)},\n'
        f'"pre_span_hex":"{pre_span.hex()}",\n'
        f'"spans":[\n{sp}\n],\n'
        f'"pre_vertex_hex":"{pre_vertex.hex()}",\n'
        f'"vertices":[\n{vx}\n],\n'
        f'"tail_hex":"{tail.hex()}"\n'
        "}\n"
    )


def imp(text: str) -> bytes:
    obj = json.loads(text)
    out = bytearray()
    out += struct.pack("<5I", *[int(x) for x in obj["header"]])
    out += bytes.fromhex(obj["pre_span_hex"])
    for s in obj["spans"]:
        out += struct.pack("<4B4H3i", s[0], s[1], s[2], s[3],
                           s[4], s[5], s[6], s[7], s[8], s[9], s[10])
    out += bytes.fromhex(obj["pre_vertex_hex"])
    for v in obj["vertices"]:
        out += struct.pack("<3h", v[0], v[1], v[2])
    out += bytes.fromhex(obj["tail_hex"])
    return bytes(out)


def main():
    if len(sys.argv) != 4 or sys.argv[1] not in ("export", "import"):
        print(__doc__)
        sys.exit(2)
    mode, src, dst = sys.argv[1:4]
    if mode == "export":
        with open(src, "rb") as fh:
            data = fh.read()
        with open(dst, "w", encoding="utf-8") as fh:
            fh.write(export(data))
    else:
        with open(src, "r", encoding="utf-8") as fh:
            data = imp(fh.read())
        with open(dst, "wb") as fh:
            fh.write(data)


if __name__ == "__main__":
    main()
