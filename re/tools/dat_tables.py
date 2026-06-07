#!/usr/bin/env python3
"""
dat_tables.py -- export/import the small Test Drive 5 "table" assets to/from
editable sources, for the pack-on-load retirement (td5_assetsrc.c).

Formats (every byte round-trip preserved):
  checkpt : CHECKPT.NUM  -- 96 bytes = 4 rows x 6 cols int32 LE  -> JSON
  traffic : TRAFFIC.BUS  -- N x 4-byte {int16 span, u8 flags, u8 lane} -> JSON
            (includes the trailing 0xFFFF sentinel record verbatim)
  routes  : LEFT/RIGHT.TRK -- N x 3 raw bytes/span -> CSV (one "b0,b1,b2" row)

Usage:
  python dat_tables.py export <checkpt|traffic|routes> <in.dat> <out>
  python dat_tables.py import <checkpt|traffic|routes> <in.src> <out.dat>

The C encoders in td5_assetsrc.c must produce byte-identical output; the runtime
self-test (td5re.exe --selftest-assetsrc) is the authoritative gate.
"""

import json
import struct
import sys


# ---------------------------------------------------------------------------
# CHECKPT.NUM  (4 x 6 int32)
# ---------------------------------------------------------------------------

def checkpt_export(data: bytes) -> str:
    if len(data) != 96:
        raise ValueError(f"checkpt.num must be 96 bytes, got {len(data)}")
    vals = list(struct.unpack("<24i", data))
    rows = [vals[i * 6:(i + 1) * 6] for i in range(4)]
    return json.dumps(
        {"_format": "td5_checkpt", "_version": 1, "_size": 96, "rows": rows},
        indent=2) + "\n"


def checkpt_import(text: str) -> bytes:
    obj = json.loads(text)
    rows = obj["rows"]
    if len(rows) != 4 or any(len(r) != 6 for r in rows):
        raise ValueError("checkpt rows must be 4 x 6")
    flat = [int(v) for r in rows for v in r]
    return struct.pack("<24i", *flat)


# ---------------------------------------------------------------------------
# TRAFFIC.BUS  (N x {int16 span, u8 flags, u8 lane})
# ---------------------------------------------------------------------------

def traffic_export(data: bytes) -> str:
    if len(data) % 4 != 0:
        raise ValueError(f"traffic.bus size {len(data)} not a multiple of 4")
    recs = []
    for off in range(0, len(data), 4):
        span, flags, lane = struct.unpack_from("<hBB", data, off)
        recs.append([span, flags, lane])
    return json.dumps(
        {"_format": "td5_traffic", "_version": 1, "records": recs},
        indent=2) + "\n"


def traffic_import(text: str) -> bytes:
    obj = json.loads(text)
    out = bytearray()
    for span, flags, lane in obj["records"]:
        out += struct.pack("<hBB", int(span), int(flags), int(lane))
    return bytes(out)


# ---------------------------------------------------------------------------
# LEFT/RIGHT.TRK  (N x 3 raw bytes)
# ---------------------------------------------------------------------------

def routes_export(data: bytes) -> str:
    if len(data) % 3 != 0:
        raise ValueError(f"route .trk size {len(data)} not a multiple of 3")
    lines = ["# td5 route table -- one row per span: lane0,lane1,lane2 (raw u8)"]
    for off in range(0, len(data), 3):
        b0, b1, b2 = data[off], data[off + 1], data[off + 2]
        lines.append(f"{b0},{b1},{b2}")
    return "\n".join(lines) + "\n"


def routes_import(text: str) -> bytes:
    out = bytearray()
    for line in text.splitlines():
        s = line.strip()
        if not s or s.startswith("#"):
            continue
        parts = [p for p in s.replace(",", " ").split() if p != ""]
        if len(parts) != 3:
            raise ValueError(f"route row must have 3 values: {line!r}")
        out += bytes(int(p) & 0xFF for p in parts)
    return bytes(out)


# ---------------------------------------------------------------------------

EXPORTERS = {"checkpt": checkpt_export, "traffic": traffic_export, "routes": routes_export}
IMPORTERS = {"checkpt": checkpt_import, "traffic": traffic_import, "routes": routes_import}


def main():
    if len(sys.argv) != 5 or sys.argv[1] not in ("export", "import"):
        print(__doc__)
        sys.exit(2)
    mode, kind, src, dst = sys.argv[1:5]
    if kind not in EXPORTERS:
        print(f"unknown kind '{kind}' (checkpt|traffic|routes)", file=sys.stderr)
        sys.exit(2)

    if mode == "export":
        with open(src, "rb") as fh:
            data = fh.read()
        text = EXPORTERS[kind](data)
        with open(dst, "w", encoding="utf-8") as fh:
            fh.write(text)
    else:
        with open(src, "r", encoding="utf-8") as fh:
            text = fh.read()
        data = IMPORTERS[kind](text)
        with open(dst, "wb") as fh:
            fh.write(data)


if __name__ == "__main__":
    main()
