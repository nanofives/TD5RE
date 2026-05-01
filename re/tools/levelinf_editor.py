#!/usr/bin/env python3
"""
levelinf_editor.py -- CLI tool for reading, editing, and converting
Test Drive 5 levelinf.dat files (100 = 0x64 bytes).

Structure: single flat 100-byte block. Packed inside each level%03d.zip
and loaded by LoadTrackRuntimeData (0x42fb90) into gTrackEnvironmentConfig
at 0x4aee20. See re/analysis/formats/levelinf-dat-format.md.

Every byte is classified (see `kind` attribute):
  - "live"    : read at runtime
  - "dead"    : VESTIGIAL field with no PC-code reader
  - "padding" : always 0x00; no readers

Round-trip discipline: all bytes are preserved in JSON/TOML export so that
re-import produces an identical file.

Usage:
  python levelinf_editor.py read <file>
  python levelinf_editor.py export <file> --json  <out.json>
  python levelinf_editor.py export <file> --toml  <out.toml>
  python levelinf_editor.py import <in.json|.toml> --out <file>
  python levelinf_editor.py set <file> <field> <value> [<field> <value> ...]
  python levelinf_editor.py diff <a.dat> <b.dat>
  python levelinf_editor.py fields [--verbose]
"""

import argparse
import json
import struct
import sys
import tomllib
from collections import OrderedDict
from dataclasses import dataclass
from pathlib import Path
from typing import List

try:
    import tomli_w
    HAS_TOMLI_W = True
except ImportError:
    HAS_TOMLI_W = False


# ---------------------------------------------------------------------------
# Field descriptor (matches carparam_editor shape; `table` always = "level")
# ---------------------------------------------------------------------------

KIND_LIVE    = "live"
KIND_DEAD    = "dead"
KIND_PADDING = "padding"

KIND_NOTE = {
    KIND_LIVE:    "read at runtime",
    KIND_DEAD:    "vestigial; no PC-code reader — edits have no runtime effect",
    KIND_PADDING: "alignment/reserved padding; always 0x00 in shipping files",
}


@dataclass
class FieldDef:
    name: str
    file_offset: int
    size: int            # 1, 2, or 4 bytes
    count: int
    signed: bool
    fp_scale: int        # unused for levelinf (kept for schema parity)
    table: str           # always "level" here
    table_offset: int
    description: str
    confidence: str
    kind: str


def _u32(name, off, desc, conf="confirmed", kind=KIND_LIVE):
    return FieldDef(name, off, 4, 1, False, 0, "level", off, desc, conf, kind)

def _u32_arr(name, off, count, desc, conf="confirmed", kind=KIND_LIVE):
    return FieldDef(name, off, 4, count, False, 0, "level", off, desc, conf, kind)

def _i16_arr(name, off, count, desc, conf="confirmed", kind=KIND_LIVE):
    return FieldDef(name, off, 2, count, True, 0, "level", off, desc, conf, kind)

def _u8(name, off, desc, conf="confirmed", kind=KIND_LIVE):
    return FieldDef(name, off, 1, 1, False, 0, "level", off, desc, conf, kind)

def _u8_arr(name, off, count, desc, conf="confirmed", kind=KIND_LIVE):
    return FieldDef(name, off, 1, count, False, 0, "level", off, desc, conf, kind)


# ---------------------------------------------------------------------------
# Full field table — every byte of the 0x64 file accounted for.
# Source: re/analysis/formats/levelinf-dat-format.md
# ---------------------------------------------------------------------------

LEVELINF_SIZE = 0x64   # 100 bytes

FIELDS: List[FieldDef] = [
    _u32("track_type", 0x00,
         "0 = circuit, 1 = point-to-point. Read at VA 0x42ae6b — sets "
         "DAT_00466e94=1 (circuit-mode CP tracking) when == 1."),

    _u32("smoke_enable", 0x04,
         "1 = enable tire smoke sprites, 0 = disabled. Read at VA 0x401410 "
         "by InitializeRaceSmokeSpritePool; gates smoke-sprite load + "
         "per-actor pool allocation (racer_count * 0x170 bytes)."),

    _u32("checkpoint_count", 0x08,
         "Number of checkpoint stages (1..7). Read at VA 0x40a04d by "
         "CheckRaceCompletionState — drives P2P race completion logic."),

    _u32_arr("checkpoint_spans", 0x0C, 7,
             "Array of up to 7 track span indices marking checkpoint "
             "positions. Zero-padded beyond checkpoint_count."),

    _u32("weather_type", 0x28,
         "0 = Rain (full particle + audio), 1 = Snow (CUT — buffers alloc'd "
         "but render skipped), 2 = Clear. Read at VA 0x446245 by "
         "InitializeWeatherOverlayParticles; copied to DAT_004c3de8."),

    _u32("density_pair_count", 0x2C,
         "Number of active (segment, density) pairs (0..6). Read at "
         "VA 0x4464c8 by UpdateAmbientParticleDensityForSegment — "
         "bounds the scan over density_pairs below."),

    _u32("is_circuit", 0x30,
         "1 = circuit (lapped), 0 = P2P. Read at VA 0x42ae7b. When 0, "
         "clears DAT_004aad8c (disables certain race features for P2P)."),

    _i16_arr("density_pairs", 0x34, 12,
             "Interleaved (segment_id, density) pairs — 6 pairs × (int16, int16). "
             "Even indices = segment_id (track segment triggering density change); "
             "odd indices = target active particle count (clamped to 128 at runtime). "
             "Rain tracks typically ramp 400 → 1600 → 400 over the 6 pairs."),

    _u8_arr("_pad_4C", 0x4C, 8,
            "Reserved padding between density_pairs and sky_animation_index. "
            "All shipping files contain 0x00.",
            kind=KIND_PADDING),

    _u32("sky_animation_index", 0x54,
         "VESTIGIAL: 36 for circuits, 0xFFFFFFFF for P2P in shipping data, "
         "but no PC code reads this field. Likely PSX-leftover.",
         kind=KIND_DEAD),

    _u32("total_span_count", 0x58,
         "Total spans in track (9999 sentinel for P2P). Various race-logic "
         "sites read this for lap-wrap / span-bounds calculations."),

    _u32("fog_enabled", 0x5C,
         "0 = no fog, 1 = fog active. Read at VA 0x42b443; when set AND "
         "DAT_00466e98 (fog-capable flag) is set, passes fog_color + "
         "distance to ConfigureRaceFogColorAndMode (0x40af10)."),

    _u8("fog_color_r", 0x60,
        "Fog red component (0..255). Packed at VA 0x42b448-0x42b464 into "
        "0xFFRRGGBB and handed to the D3D fog setup."),

    _u8("fog_color_g", 0x61,
        "Fog green component (0..255)."),

    _u8("fog_color_b", 0x62,
        "Fog blue component (0..255)."),

    _u8("_pad_63", 0x63,
        "Final alignment byte to round struct to 0x64. Always 0x00.",
        kind=KIND_PADDING),
]

FIELD_BY_NAME = OrderedDict((f.name, f) for f in FIELDS)

TRACK_TYPE_NAMES    = {0: "circuit", 1: "point_to_point"}
WEATHER_TYPE_NAMES  = {0: "rain", 1: "snow_CUT", 2: "clear"}


# ---------------------------------------------------------------------------
# Binary I/O helpers
# ---------------------------------------------------------------------------

def _struct_fmt(f: FieldDef) -> str:
    if f.size == 1: return "b" if f.signed else "B"
    if f.size == 2: return "h" if f.signed else "H"
    if f.size == 4: return "i" if f.signed else "I"
    raise ValueError(f"Unsupported size {f.size}")


def read_field(data: bytes, f: FieldDef):
    fmt = f"<{f.count}{_struct_fmt(f)}"
    values = struct.unpack_from(fmt, data, f.file_offset)
    return values[0] if f.count == 1 else list(values)


def write_field(data: bytearray, f: FieldDef, value):
    fmt = f"<{f.count}{_struct_fmt(f)}"
    if f.count == 1:
        struct.pack_into(fmt, data, f.file_offset, value)
    else:
        if not isinstance(value, (list, tuple)):
            raise ValueError(f"{f.name} expects an array of {f.count}")
        if len(value) != f.count:
            raise ValueError(f"{f.name} expects {f.count} values, got {len(value)}")
        struct.pack_into(fmt, data, f.file_offset, *value)


def read_all_fields(data: bytes) -> OrderedDict:
    return OrderedDict((f.name, read_field(data, f)) for f in FIELDS)


def write_all_fields(d: dict) -> bytearray:
    out = bytearray(LEVELINF_SIZE)
    for f in FIELDS:
        if f.name in d:
            write_field(out, f, d[f.name])
    return out


def _pad_or_trim(data: bytes, path: str = "") -> bytes:
    if len(data) == LEVELINF_SIZE:
        return data
    msg = f"file is {len(data)} bytes, expected {LEVELINF_SIZE} (0x{LEVELINF_SIZE:X})"
    if path:
        msg = f"{path}: {msg}"
    print(f"WARNING: {msg}", file=sys.stderr)
    if len(data) < LEVELINF_SIZE:
        return data + b'\x00' * (LEVELINF_SIZE - len(data))
    return data[:LEVELINF_SIZE]


# ---------------------------------------------------------------------------
# Display
# ---------------------------------------------------------------------------

def format_value(f: FieldDef, value) -> str:
    if f.count > 1:
        return "[" + ", ".join(str(v) for v in value) + "]"
    extra = ""
    if f.name == "track_type" and value in TRACK_TYPE_NAMES:
        extra = f" ({TRACK_TYPE_NAMES[value]})"
    elif f.name == "weather_type" and value in WEATHER_TYPE_NAMES:
        extra = f" ({WEATHER_TYPE_NAMES[value]})"
    return f"{value}{extra}"


KIND_TAG = {
    KIND_LIVE:    "[live]",
    KIND_DEAD:    "[DEAD]",
    KIND_PADDING: "[pad]",
}


def display_fields(data: bytes, verbose: bool = True):
    print(f"\n{'='*78}")
    print(f"  LEVELINF.DAT  (100 bytes = 0x64)")
    print(f"{'='*78}")
    for f in FIELDS:
        value = read_field(data, f)
        kind_tag = KIND_TAG.get(f.kind, f"[{f.kind}]")
        off_str = f"0x{f.file_offset:02X}"
        print(f"  {off_str} {kind_tag:8s} {f.name:25s} = {format_value(f, value)}")
        if verbose:
            print(f"  {'':>7s} [{f.confidence}] {f.description}")


# ---------------------------------------------------------------------------
# Serialization — JSON and TOML share the same logical structure
# ---------------------------------------------------------------------------

FORMAT_ID = "td5_levelinf"
FORMAT_VERSION = 1


def _field_entry(f: FieldDef, value) -> OrderedDict:
    entry = OrderedDict()
    entry["value"] = value
    entry["offset"] = f"0x{f.file_offset:02X}"
    type_str = f"{'s' if f.signed else 'u'}int{f.size*8}"
    if f.count > 1:
        type_str += f"[{f.count}]"
    entry["type"] = type_str
    entry["kind"] = f.kind
    entry["confidence"] = f.confidence
    entry["description"] = f.description
    return entry


def export_dict(data: bytes) -> dict:
    fields = read_all_fields(data)
    result = OrderedDict()
    result["_format"] = FORMAT_ID
    result["_version"] = FORMAT_VERSION
    result["_size"] = LEVELINF_SIZE
    result["_description"] = (
        "Test Drive 5 levelinf.dat — 100-byte per-track configuration "
        "(weather, fog, checkpoints, particle density). All values are "
        "raw integers as stored in the binary file. Every byte is round-trip "
        "preserved: editing 'dead' or 'padding' fields will change the "
        "on-disk bytes but will have no runtime effect."
    )
    result["_kind_legend"] = dict(KIND_NOTE)
    for f in FIELDS:
        result[f.name] = _field_entry(f, fields[f.name])
    return result


def import_dict(parsed: dict) -> bytearray:
    fields = OrderedDict()
    for f in FIELDS:
        if f.name not in parsed:
            raise ValueError(f"Missing field '{f.name}' in input")
        entry = parsed[f.name]
        val = entry["value"] if isinstance(entry, dict) else entry
        if f.count > 1:
            if not isinstance(val, list):
                raise ValueError(f"{f.name} should be a list of {f.count}")
            if len(val) != f.count:
                raise ValueError(f"{f.name} expects {f.count} values, got {len(val)}")
            fields[f.name] = [int(v) for v in val]
        else:
            fields[f.name] = int(val)
    return write_all_fields(fields)


# ---------------------------------------------------------------------------
# JSON
# ---------------------------------------------------------------------------

def export_json(data: bytes) -> str:
    return json.dumps(export_dict(data), indent=2) + "\n"


def import_json(text: str) -> bytearray:
    return import_dict(json.loads(text))


# ---------------------------------------------------------------------------
# TOML (hand-rolled emission for readability; per-field warning comments)
# ---------------------------------------------------------------------------

def export_toml(data: bytes) -> str:
    if not HAS_TOMLI_W:
        raise RuntimeError("tomli_w is required for TOML export — pip install tomli_w")
    fields = read_all_fields(data)
    lines = []
    lines.append(f"# {FORMAT_ID} v{FORMAT_VERSION}")
    lines.append(f"# Test Drive 5 levelinf.dat — {LEVELINF_SIZE} bytes total")
    lines.append("# Kinds: live = read at runtime; dead = vestigial (no reader);")
    lines.append("#   padding = reserved/alignment bytes, always 0x00.")
    lines.append("")
    lines.append(f'_format = "{FORMAT_ID}"')
    lines.append(f"_version = {FORMAT_VERSION}")
    lines.append(f"_size = {LEVELINF_SIZE}")
    lines.append("")

    for f in FIELDS:
        if f.kind != KIND_LIVE:
            lines.append(f"# {f.kind.upper()}: {KIND_NOTE.get(f.kind, f.kind)}")
        desc = f.description
        if len(desc) <= 110:
            lines.append(f"# {desc}")
        else:
            words, line = desc.split(), ""
            for w in words:
                if len(line) + len(w) + 1 > 100:
                    lines.append(f"# {line.strip()}")
                    line = w
                else:
                    line += (" " + w) if line else w
            if line:
                lines.append(f"# {line.strip()}")

        lines.append(f"[level.{f.name}]")
        value = fields[f.name]
        if f.count > 1:
            joined = ", ".join(str(v) for v in value)
            if len(joined) <= 80:
                lines.append(f"value = [{joined}]")
            else:
                lines.append("value = [")
                for i in range(0, len(value), 8):
                    chunk = value[i:i+8]
                    comma = "," if (i + 8) < len(value) else ""
                    lines.append("    " + ", ".join(str(v) for v in chunk) + comma)
                lines.append("]")
        else:
            lines.append(f"value = {value}")
        lines.append(f'offset = "0x{f.file_offset:02X}"')
        type_str = f"{'s' if f.signed else 'u'}int{f.size*8}"
        if f.count > 1:
            type_str += f"[{f.count}]"
        lines.append(f'type = "{type_str}"')
        lines.append(f'kind = "{f.kind}"')
        lines.append(f'confidence = "{f.confidence}"')
        if f.name == "track_type" and isinstance(value, int) and value in TRACK_TYPE_NAMES:
            lines.append(f'enum_name = "{TRACK_TYPE_NAMES[value]}"')
        if f.name == "weather_type" and isinstance(value, int) and value in WEATHER_TYPE_NAMES:
            lines.append(f'enum_name = "{WEATHER_TYPE_NAMES[value]}"')
        lines.append("")
    return "\n".join(lines) + "\n"


def import_toml(text: str) -> bytearray:
    parsed = tomllib.loads(text)
    flat = {}
    for f in FIELDS:
        if "level" in parsed and isinstance(parsed["level"], dict) \
                and f.name in parsed["level"]:
            flat[f.name] = parsed["level"][f.name]
        elif f.name in parsed:
            flat[f.name] = parsed[f.name]
        else:
            raise ValueError(f"Missing field '{f.name}' in TOML input "
                             f"(looked in [level.{f.name}] and top-level)")
    return import_dict(flat)


# ---------------------------------------------------------------------------
# CLI commands
# ---------------------------------------------------------------------------

def _load_binary(path: str) -> bytes:
    with open(path, "rb") as fh:
        return _pad_or_trim(fh.read(), path)


def _detect_format(path: str) -> str:
    ext = Path(path).suffix.lower()
    if ext == ".toml": return "toml"
    if ext == ".json": return "json"
    raise ValueError(f"Cannot detect format from extension: {path}")


def cmd_read(args):
    display_fields(_load_binary(args.file), verbose=not args.brief)


def cmd_export(args):
    data = _load_binary(args.file)
    out_path = args.json or args.toml
    fmt = "json" if args.json else ("toml" if args.toml else None)
    if out_path and fmt is None:
        fmt = _detect_format(out_path)
    if fmt is None:
        fmt = "json"
    text = export_toml(data) if fmt == "toml" else export_json(data)
    if out_path:
        with open(out_path, "w", encoding="utf-8") as fh:
            fh.write(text)
        print(f"Exported to {out_path}")
    else:
        sys.stdout.write(text)


def cmd_import(args):
    with open(args.file, "r", encoding="utf-8") as fh:
        text = fh.read()
    fmt = _detect_format(args.file)
    data = import_toml(text) if fmt == "toml" else import_json(text)
    if len(data) != LEVELINF_SIZE:
        print(f"ERROR: produced {len(data)} bytes, expected {LEVELINF_SIZE}", file=sys.stderr)
        sys.exit(1)
    with open(args.out, "wb") as fh:
        fh.write(data)
    print(f"Wrote {len(data)} bytes to {args.out}")


def cmd_set(args):
    with open(args.file, "rb") as fh:
        data = bytearray(_pad_or_trim(fh.read(), args.file))
    pairs = args.assignments
    if len(pairs) % 2 != 0:
        print("ERROR: set expects <field> <value> pairs", file=sys.stderr)
        sys.exit(1)
    for i in range(0, len(pairs), 2):
        name, raw = pairs[i], pairs[i + 1]
        if name not in FIELD_BY_NAME:
            matches = [n for n in FIELD_BY_NAME if name in n]
            hint = f" Did you mean: {', '.join(matches)}?" if matches else \
                " Use 'fields' to see all field names."
            print(f"ERROR: Unknown field '{name}'.{hint}", file=sys.stderr)
            sys.exit(1)
        f = FIELD_BY_NAME[name]
        if f.kind != KIND_LIVE:
            print(f"WARNING: field '{name}' is kind={f.kind} — "
                  f"edit will persist on disk but {KIND_NOTE[f.kind]}", file=sys.stderr)
        if f.count > 1:
            parts = raw.split(",")
            if len(parts) != f.count:
                print(f"ERROR: {name} expects {f.count} comma-separated values, "
                      f"got {len(parts)}", file=sys.stderr)
                sys.exit(1)
            value = [int(p.strip(), 0) for p in parts]
        else:
            value = int(raw, 0)
        old = read_field(bytes(data), f)
        write_field(data, f, value)
        print(f"  {name}: {old} -> {value}")
    with open(args.file, "wb") as fh:
        fh.write(data)
    print(f"Wrote {len(data)} bytes to {args.file}")


def cmd_fields(args):
    print("\n--- LEVELINF ---")
    for f in FIELDS:
        size_str = f"{f.size * f.count}B"
        arr_str = f"[{f.count}]" if f.count > 1 else ""
        type_str = f"{'s' if f.signed else 'u'}int{f.size*8}{arr_str}"
        kind_tag = KIND_TAG.get(f.kind, f"[{f.kind}]")
        print(f"  0x{f.file_offset:02X}  {kind_tag:8s} {f.name:25s} "
              f"{type_str:14s} {size_str:>4s}  [{f.confidence}]")
        if args.verbose:
            print(f"         {f.description}")


def cmd_diff(args):
    with open(args.file_a, "rb") as fh:
        data_a = _pad_or_trim(fh.read(), args.file_a)
    with open(args.file_b, "rb") as fh:
        data_b = _pad_or_trim(fh.read(), args.file_b)
    a, b = read_all_fields(data_a), read_all_fields(data_b)
    diffs = 0
    for f in FIELDS:
        if a[f.name] != b[f.name]:
            diffs += 1
            kind_tag = KIND_TAG.get(f.kind, f"[{f.kind}]")
            print(f"  {kind_tag:8s} {f.name:25s}  "
                  f"A={format_value(f, a[f.name]):>20s}  "
                  f"B={format_value(f, b[f.name]):>20s}")
    if diffs == 0:
        print("Files are identical.")
    else:
        print(f"\n{diffs} field(s) differ.")


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(
        description="Test Drive 5 levelinf.dat editor (JSON/TOML round-tripper)",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""\
Examples:
  %(prog)s read re/assets/levels/level001/levelinf.dat
  %(prog)s export re/assets/levels/level023/levelinf.dat --toml moscow.toml
  %(prog)s import moscow.toml --out re/assets/levels/level023/levelinf.dat
  %(prog)s set levelinf.dat weather_type 2 fog_enabled 1 fog_color_r 64
  %(prog)s set levelinf.dat density_pairs 100,400,200,800,300,1600,400,1200,500,800,600,400
  %(prog)s fields --verbose
  %(prog)s diff level001/levelinf.dat level023/levelinf.dat
""")

    sub = parser.add_subparsers(dest="command", required=True)

    p = sub.add_parser("read", help="Read and display levelinf.dat fields")
    p.add_argument("file")
    p.add_argument("--brief", action="store_true")

    p = sub.add_parser("export", help="Export to JSON or TOML")
    p.add_argument("file")
    g = p.add_mutually_exclusive_group()
    g.add_argument("--json")
    g.add_argument("--toml")

    p = sub.add_parser("import", help="Import JSON/TOML back to binary")
    p.add_argument("file")
    p.add_argument("--out", required=True)

    p = sub.add_parser("set", help="Edit fields in-place")
    p.add_argument("file")
    p.add_argument("assignments", nargs="+")

    p = sub.add_parser("fields", help="List all field names")
    p.add_argument("--verbose", "-v", action="store_true")

    p = sub.add_parser("diff", help="Compare two levelinf.dat files")
    p.add_argument("file_a")
    p.add_argument("file_b")

    args = parser.parse_args()
    {"read": cmd_read, "export": cmd_export, "import": cmd_import,
     "set": cmd_set, "fields": cmd_fields, "diff": cmd_diff}[args.command](args)


if __name__ == "__main__":
    main()
