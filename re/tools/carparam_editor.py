#!/usr/bin/env python3
"""
carparam_editor.py -- CLI tool for reading, editing, and converting
Test Drive 5 carparam.dat files (0x10C = 268 bytes).

Structure:
  - Tuning table:  0x00..0x8B  (0x8C = 140 bytes)
  - Physics table: 0x8C..0x10B (0x80 = 128 bytes)

Usage:
  python carparam_editor.py read <file>
  python carparam_editor.py export <file> --json <out.json>
  python carparam_editor.py import <in.json> --out <file>
  python carparam_editor.py set <file> <field> <value> [<field> <value> ...]

All integer values are stored as-is (no implicit fixed-point conversion).
The display shows both raw integer and a /256 scaled float for fields
that use 8.8 fixed-point scaling in the engine.
"""

import argparse
import json
import struct
import sys
from collections import OrderedDict
from dataclasses import dataclass
from typing import List, Optional

# ---------------------------------------------------------------------------
# Field descriptor
# ---------------------------------------------------------------------------

@dataclass
class FieldDef:
    name: str           # human-readable identifier (used on CLI)
    file_offset: int    # byte offset within carparam.dat
    size: int           # 1, 2, or 4 bytes
    count: int          # >1 means array
    signed: bool        # signed vs unsigned
    fp_scale: int       # if >0, display raw/scale as float (e.g. 256)
    table: str          # "tuning" or "physics"
    table_offset: int   # offset relative to table start
    description: str    # human description
    confidence: str     # confirmed / strongly-suspected / hypothesis


def _short(name, file_off, tbl, tbl_off, desc, conf="confirmed",
           count=1, fp=256, signed=True):
    return FieldDef(name, file_off, 2, count, signed, fp, tbl, tbl_off, desc, conf)

def _int32(name, file_off, tbl, tbl_off, desc, conf="confirmed",
           count=1, fp=0, signed=True):
    return FieldDef(name, file_off, 4, count, signed, fp, tbl, tbl_off, desc, conf)

def _short_arr(name, file_off, tbl, tbl_off, count, desc, conf="confirmed",
               fp=0, signed=True):
    return FieldDef(name, file_off, 2, count, signed, fp, tbl, tbl_off, desc, conf)


# ---------------------------------------------------------------------------
# Full field table -- every byte of the 0x10C file is accounted for
# ---------------------------------------------------------------------------

CARPARAM_SIZE = 0x10C       # 268 bytes
TUNING_SIZE   = 0x8C        # 140 bytes
PHYSICS_SIZE  = 0x80        # 128 bytes

FIELDS: List[FieldDef] = [
    # ===== TUNING TABLE (file 0x00..0x8B) =====

    # Bounding box / suspension envelope (0x00-0x1F) -- overwritten at runtime
    _short_arr("bbox_extents_front", 0x00, "tuning", 0x00, 6,
               "Front bounding box extents (short[6], 3 axis pairs). "
               "Overwritten by ComputeVehicleSuspensionEnvelope from mesh data.",
               "strongly-suspected"),
    _short_arr("bbox_extents_rear", 0x0C, "tuning", 0x0C, 6,
               "Rear bounding box extents (short[6]).",
               "strongly-suspected"),
    _short_arr("bbox_vertical", 0x18, "tuning", 0x18, 4,
               "Vertical bounding extent data (short[4], min/max Y).",
               "hypothesis"),

    # Partially unknown region 0x20-0x3F
    _short_arr("unknown_20", 0x20, "tuning", 0x20, 16,
               "Unknown tuning data between bbox and wheel positions (32 bytes).",
               "hypothesis"),

    # Wheel positions (0x40-0x5F)
    _short_arr("wheel_pos_FL", 0x40, "tuning", 0x40, 4,
               "Front-left wheel local position (x,y,z,flags).",
               "confirmed"),
    _short_arr("wheel_pos_FR", 0x48, "tuning", 0x48, 4,
               "Front-right wheel local position (x,y,z,flags).",
               "confirmed"),
    _short_arr("wheel_pos_RL", 0x50, "tuning", 0x50, 4,
               "Rear-left wheel local position (x,y,z,flags).",
               "confirmed"),
    _short_arr("wheel_pos_RR", 0x58, "tuning", 0x58, 4,
               "Rear-right wheel local position (x,y,z,flags).",
               "confirmed"),

    # Extended wheel/contact geometry (0x60-0x81)
    _short_arr("wheel_contact_geometry", 0x60, "tuning", 0x60, 17,
               "Extended wheel contact geometry data (34 bytes). "
               "Per-wheel vertical offsets and suspension height params.",
               "strongly-suspected"),

    # Suspension height reference
    _short("suspension_height_ref", 0x82, "tuning", 0x82,
           "Vertical offset for suspension preload. Scaled by 0xB5/256 in "
           "RefreshVehicleWheelContactFrames.", "confirmed", fp=256),

    # Extended wheel data (unknown)
    _short_arr("unknown_84", 0x84, "tuning", 0x84, 2,
               "Extended wheel data (4 bytes), likely contact probe offsets.",
               "hypothesis"),

    # Collision mass
    _short("collision_mass", 0x88, "tuning", 0x88,
           "Inverse mass for collision impulse. Higher = heavier = less knockback. "
           "Traffic vehicles use 0x20.", "confirmed", fp=0),

    # Last 2 bytes of tuning
    _short("unknown_8A", 0x8A, "tuning", 0x8A,
           "Last 2 bytes of tuning table (unknown/padding).", "hypothesis", fp=0),

    # ===== PHYSICS TABLE (file 0x8C..0x10B) =====

    # Torque curve (16 entries)
    _short_arr("torque_curve", 0x8C, "physics", 0x00, 16,
               "Engine torque curve (short[16]). 16 samples covering engine speed "
               "0-8192 (512 RPM per entry). Interpolated in ComputeDriveTorqueFromGearCurve."),

    # Chassis geometry
    _int32("vehicle_inertia", 0xAC, "physics", 0x20,
           "Yaw moment of inertia (int32). Divisor in yaw_torque / (inertia/0x28C). "
           "Also used in weight transfer: (rear_weight^2 + inertia) >> 10."),
    _int32("half_wheelbase", 0xB0, "physics", 0x24,
           "Half the wheelbase distance (int32). Used for front/rear axle load "
           "distribution. Full wheelbase = half_wheelbase * 2."),

    # Weight distribution
    _short("front_weight_dist", 0xB4, "physics", 0x28,
           "Front axle weight distribution numerator. "
           "front_load = (front_dist << 8) / (front + rear).", fp=0),
    _short("rear_weight_dist", 0xB6, "physics", 0x2A,
           "Rear axle weight distribution numerator. "
           "Together with front_weight_dist defines CG bias.", fp=0),

    # Aero drag
    _short("drag_coefficient", 0xB8, "physics", 0x2C,
           "Aerodynamic drag coefficient. Difficulty-scaled: "
           "normal *300/256, hard *380/256."),

    # Gear ratios (8 entries: R, N, 1-6)
    _short_arr("gear_ratio_table", 0xBA, "physics", 0x2E, 8,
               "Gear ratios (short[8]): [0]=reverse, [1]=neutral, [2-7]=gears 1-6. "
               "Used as multiplier/256 in torque and speed calculations."),

    # Shift thresholds
    _short_arr("upshift_rpm_table", 0xCA, "physics", 0x3E, 8,
               "Upshift RPM thresholds (short[8]). Upshift when engine_speed > table[gear]."),
    _short_arr("downshift_rpm_table", 0xDA, "physics", 0x4E, 8,
               "Downshift RPM thresholds (short[8]). Downshift when engine_speed < table[gear]."),

    # Suspension parameters
    _short("suspension_damping", 0xEA, "physics", 0x5E,
           "Suspension damping coefficient. displacement * damping / 256."),
    _short("suspension_spring_rate", 0xEC, "physics", 0x60,
           "Suspension spring rate. velocity * spring / 256."),
    _short("suspension_feedback", 0xEE, "physics", 0x62,
           "Suspension feedback coupling. Couples lateral/vertical forces "
           "into wheel travel: cross_term * feedback / 256."),
    _short("suspension_travel_limit", 0xF0, "physics", 0x64,
           "Max wheel displacement (symmetrical +/-). Clamped when exceeded "
           "(bottoming out).", fp=0),
    _short("suspension_response_factor", 0xF2, "physics", 0x66,
           "Suspension velocity response. prev_vel * response / 256."),

    # Drive force
    _short("drive_torque_multiplier", 0xF4, "physics", 0x68,
           "Drive torque multiplier. torque_curve[i] * mult / 256. "
           "Difficulty-scaled: normal *360/256, hard *650/256."),
    _short("damping_low_speed", 0xF6, "physics", 0x6A,
           "Velocity damping at low speed (frame_counter < 0x20 or gear < 2). "
           "speed -= (speed/256) * (surface*256 + damp_low) / 4096."),
    _short("damping_high_speed", 0xF8, "physics", 0x6C,
           "Velocity damping at high speed. Same formula but used when "
           "frame_counter >= 0x20 and gear >= 2. Aerodynamic drag at speed."),
    _short("brake_force", 0xFA, "physics", 0x6E,
           "Brake deceleration force. brake_decel = brake_force * throttle / 256. "
           "Hard mode: *450/256."),
    _short("engine_brake_force", 0xFC, "physics", 0x70,
           "Engine braking force. engine_decel = engine_brake * throttle / 256. "
           "Hard mode: *400/256."),
    _short("max_rpm", 0xFE, "physics", 0x72,
           "Engine redline RPM. Torque returns 0 above max_rpm - 50. "
           "Engine speed clamped to this value.", fp=0),
    _short("top_speed_limit", 0x100, "physics", 0x74,
           "Hard speed cap. Drive force zeroed when top_speed*256 < speed. "
           "Difficulty-adjusted per NPC modifier.", fp=0),

    # Drivetrain & traction
    _short("drivetrain_type", 0x102, "physics", 0x76,
           "Drivetrain type: 1=RWD, 2=FWD, 3=AWD. Determines which wheels "
           "receive drive torque.", fp=0),
    _short("speed_scale_factor", 0x104, "physics", 0x78,
           "Speed-to-RPM or display multiplier. Difficulty-scaled: "
           "normal <<1 (x2), hard <<2 (x4).", fp=0),
    _short("handbrake_grip_modifier", 0x106, "physics", 0x7A,
           "Rear grip multiplier when handbrake active. "
           "grip *= handbrake_grip / 256. Values < 256 reduce grip."),
    _short("lateral_slip_stiffness", 0x108, "physics", 0x7C,
           "Cornering slip sensitivity. slip_speed = speed * stiffness / 256. "
           "Determines how quickly lateral grip saturates.",
           "strongly-suspected"),
    _short("unknown_7E", 0x10A, "physics", 0x7E,
           "Last 2 bytes of physics table. No access found; likely padding.",
           "hypothesis", fp=0),
]

# Build lookup by name
FIELD_BY_NAME = OrderedDict((f.name, f) for f in FIELDS)

# Drivetrain enum for display
DRIVETRAIN_NAMES = {1: "RWD", 2: "FWD", 3: "AWD"}


# ---------------------------------------------------------------------------
# Binary I/O helpers
# ---------------------------------------------------------------------------

def _struct_fmt(field: FieldDef) -> str:
    """Return struct format char for one element."""
    if field.size == 1:
        return "b" if field.signed else "B"
    elif field.size == 2:
        return "h" if field.signed else "H"
    elif field.size == 4:
        return "i" if field.signed else "I"
    raise ValueError(f"Unsupported field size {field.size}")


def read_field(data: bytes, field: FieldDef):
    """Read a field (scalar or array) from raw carparam.dat bytes."""
    fmt_char = _struct_fmt(field)
    fmt = f"<{field.count}{fmt_char}"
    values = struct.unpack_from(fmt, data, field.file_offset)
    if field.count == 1:
        return values[0]
    return list(values)


def write_field(data: bytearray, field: FieldDef, value):
    """Write a field (scalar or array) into a carparam.dat bytearray."""
    fmt_char = _struct_fmt(field)
    fmt = f"<{field.count}{fmt_char}"
    if field.count == 1:
        struct.pack_into(fmt, data, field.file_offset, value)
    else:
        if not isinstance(value, (list, tuple)):
            raise ValueError(f"Field {field.name} expects an array of {field.count} values")
        if len(value) != field.count:
            raise ValueError(f"Field {field.name} expects {field.count} values, got {len(value)}")
        struct.pack_into(fmt, data, field.file_offset, *value)


def read_all_fields(data: bytes) -> OrderedDict:
    """Read all fields from carparam.dat bytes into an ordered dict."""
    result = OrderedDict()
    for f in FIELDS:
        result[f.name] = read_field(data, f)
    return result


def write_all_fields(fields_dict: dict) -> bytearray:
    """Write all fields from a dict back to a carparam.dat bytearray."""
    data = bytearray(CARPARAM_SIZE)
    for f in FIELDS:
        if f.name in fields_dict:
            write_field(data, f, fields_dict[f.name])
    return data


# ---------------------------------------------------------------------------
# Display
# ---------------------------------------------------------------------------

def format_value(field: FieldDef, value) -> str:
    """Format a value for human display."""
    if field.count > 1:
        items = []
        for v in value:
            if field.fp_scale > 0:
                items.append(f"{v} ({v / field.fp_scale:.4f})")
            else:
                items.append(str(v))
        return "[" + ", ".join(items) + "]"
    else:
        extra = ""
        if field.name == "drivetrain_type" and value in DRIVETRAIN_NAMES:
            extra = f" ({DRIVETRAIN_NAMES[value]})"
        elif field.fp_scale > 0:
            extra = f" ({value / field.fp_scale:.4f})"
        return f"{value}{extra}"


def display_fields(data: bytes, verbose: bool = True):
    """Pretty-print all fields from carparam.dat."""
    current_table = None
    for f in FIELDS:
        if f.table != current_table:
            current_table = f.table
            header = "TUNING TABLE" if current_table == "tuning" else "PHYSICS TABLE"
            print(f"\n{'='*72}")
            print(f"  {header}  (file 0x{0x00 if current_table == 'tuning' else 0x8C:02X}..0x{0x8B if current_table == 'tuning' else 0x10B:02X})")
            print(f"{'='*72}")

        value = read_field(data, f)
        val_str = format_value(f, value)
        conf_tag = f"[{f.confidence}]" if verbose else ""

        # Compact display: name, offset, value
        off_str = f"0x{f.file_offset:03X}"
        tbl_off = f"{f.table[:3]}+0x{f.table_offset:02X}"
        print(f"  {off_str} {tbl_off:10s}  {f.name:35s} = {val_str}")
        if verbose:
            print(f"  {'':>16s}  {conf_tag} {f.description}")


# ---------------------------------------------------------------------------
# JSON export / import
# ---------------------------------------------------------------------------

def export_json(data: bytes) -> dict:
    """Export carparam.dat to a JSON-serializable dict with metadata."""
    fields = read_all_fields(data)
    result = OrderedDict()
    result["_format"] = "td5_carparam"
    result["_version"] = 1
    result["_size"] = CARPARAM_SIZE
    result["_description"] = (
        "Test Drive 5 carparam.dat -- tuning (0x8C bytes) + physics (0x80 bytes). "
        "All values are raw integers as stored in the binary file."
    )

    # Add field metadata alongside values
    for f in FIELDS:
        entry = OrderedDict()
        entry["value"] = fields[f.name]
        entry["file_offset"] = f"0x{f.file_offset:03X}"
        entry["table"] = f.table
        entry["table_offset"] = f"0x{f.table_offset:02X}"
        entry["type"] = f"{'s' if f.signed else 'u'}int{f.size*8}"
        if f.count > 1:
            entry["type"] += f"[{f.count}]"
        if f.fp_scale > 0:
            entry["fp_scale"] = f.fp_scale
            if f.count > 1:
                entry["scaled"] = [v / f.fp_scale for v in fields[f.name]]
            else:
                entry["scaled"] = fields[f.name] / f.fp_scale
        entry["confidence"] = f.confidence
        entry["description"] = f.description
        result[f.name] = entry

    return result


def import_json(json_dict: dict) -> bytearray:
    """Import a JSON dict (as produced by export_json) back to binary.

    Accepts both the full metadata format (where each field is a dict with
    a "value" key) and a flat format (where each field is just its value).
    """
    fields = OrderedDict()
    for f in FIELDS:
        if f.name not in json_dict:
            raise ValueError(f"Missing field '{f.name}' in JSON")
        entry = json_dict[f.name]
        if isinstance(entry, dict):
            val = entry["value"]
        else:
            val = entry
        # Ensure correct types
        if f.count > 1:
            if not isinstance(val, list):
                raise ValueError(f"Field '{f.name}' should be a list of {f.count} values")
            fields[f.name] = [int(v) for v in val]
        else:
            fields[f.name] = int(val)
    return write_all_fields(fields)


# ---------------------------------------------------------------------------
# CLI commands
# ---------------------------------------------------------------------------

def cmd_read(args):
    """Read and display a carparam.dat file."""
    with open(args.file, "rb") as fh:
        data = fh.read()
    if len(data) != CARPARAM_SIZE:
        print(f"WARNING: file is {len(data)} bytes, expected {CARPARAM_SIZE} (0x{CARPARAM_SIZE:X})",
              file=sys.stderr)
        if len(data) < CARPARAM_SIZE:
            data = data + b'\x00' * (CARPARAM_SIZE - len(data))
        else:
            data = data[:CARPARAM_SIZE]
    display_fields(data, verbose=not args.brief)


def cmd_export(args):
    """Export a carparam.dat to JSON."""
    with open(args.file, "rb") as fh:
        data = fh.read()
    if len(data) != CARPARAM_SIZE:
        print(f"WARNING: file is {len(data)} bytes, expected {CARPARAM_SIZE}", file=sys.stderr)
        if len(data) < CARPARAM_SIZE:
            data = data + b'\x00' * (CARPARAM_SIZE - len(data))
        else:
            data = data[:CARPARAM_SIZE]

    result = export_json(data)
    json_str = json.dumps(result, indent=2)

    if args.json:
        with open(args.json, "w") as fh:
            fh.write(json_str + "\n")
        print(f"Exported to {args.json}")
    else:
        print(json_str)


def cmd_import(args):
    """Import from JSON back to binary carparam.dat."""
    with open(args.file, "r") as fh:
        json_dict = json.load(fh)

    data = import_json(json_dict)

    out_path = args.out
    if not out_path:
        print("ERROR: --out <path> is required for import", file=sys.stderr)
        sys.exit(1)

    with open(out_path, "wb") as fh:
        fh.write(data)
    print(f"Wrote {len(data)} bytes to {out_path}")


def cmd_set(args):
    """Edit individual fields in a carparam.dat file."""
    filepath = args.file
    with open(filepath, "rb") as fh:
        data = bytearray(fh.read())
    if len(data) != CARPARAM_SIZE:
        print(f"WARNING: file is {len(data)} bytes, expected {CARPARAM_SIZE}", file=sys.stderr)
        if len(data) < CARPARAM_SIZE:
            data.extend(b'\x00' * (CARPARAM_SIZE - len(data)))
        elif len(data) > CARPARAM_SIZE:
            data = data[:CARPARAM_SIZE]

    pairs = args.assignments
    if len(pairs) % 2 != 0:
        print("ERROR: set expects <field> <value> pairs", file=sys.stderr)
        sys.exit(1)

    for i in range(0, len(pairs), 2):
        name = pairs[i]
        raw_value = pairs[i + 1]

        if name not in FIELD_BY_NAME:
            # Check for partial matches
            matches = [n for n in FIELD_BY_NAME if name in n]
            if matches:
                print(f"ERROR: Unknown field '{name}'. Did you mean: {', '.join(matches)}?",
                      file=sys.stderr)
            else:
                print(f"ERROR: Unknown field '{name}'. Use 'read' to see all field names.",
                      file=sys.stderr)
            sys.exit(1)

        field = FIELD_BY_NAME[name]

        if field.count > 1:
            # For arrays, accept comma-separated values
            parts = raw_value.split(",")
            if len(parts) != field.count:
                print(f"ERROR: Field '{name}' expects {field.count} comma-separated values, "
                      f"got {len(parts)}", file=sys.stderr)
                sys.exit(1)
            value = [int(p.strip(), 0) for p in parts]
        else:
            value = int(raw_value, 0)

        old_value = read_field(bytes(data), field)
        write_field(data, field, value)
        print(f"  {name}: {old_value} -> {value}")

    with open(filepath, "wb") as fh:
        fh.write(data)
    print(f"Wrote {len(data)} bytes to {filepath}")


def cmd_fields(args):
    """List all known field names."""
    current_table = None
    for f in FIELDS:
        if f.table != current_table:
            current_table = f.table
            print(f"\n--- {current_table.upper()} TABLE ---")
        size_str = f"{f.size * f.count}B"
        arr_str = f"[{f.count}]" if f.count > 1 else ""
        type_str = f"{'s' if f.signed else 'u'}int{f.size*8}{arr_str}"
        print(f"  0x{f.file_offset:03X}  {f.name:35s}  {type_str:14s}  {size_str:>4s}  [{f.confidence}]")
        if args.verbose:
            print(f"         {f.description}")


def cmd_diff(args):
    """Compare two carparam.dat files field by field."""
    with open(args.file_a, "rb") as fh:
        data_a = fh.read()
    with open(args.file_b, "rb") as fh:
        data_b = fh.read()

    for d in [data_a, data_b]:
        if len(d) < CARPARAM_SIZE:
            d = d + b'\x00' * (CARPARAM_SIZE - len(d))

    fields_a = read_all_fields(data_a[:CARPARAM_SIZE])
    fields_b = read_all_fields(data_b[:CARPARAM_SIZE])

    diffs = 0
    for f in FIELDS:
        va = fields_a[f.name]
        vb = fields_b[f.name]
        if va != vb:
            diffs += 1
            print(f"  {f.name:35s}  A={format_value(f, va):>20s}  B={format_value(f, vb):>20s}")

    if diffs == 0:
        print("Files are identical.")
    else:
        print(f"\n{diffs} field(s) differ.")


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(
        description="Test Drive 5 carparam.dat editor",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""\
Examples:
  %(prog)s read carparam.dat
  %(prog)s read carparam.dat --brief
  %(prog)s export carparam.dat --json cam_params.json
  %(prog)s import cam_params.json --out carparam.dat
  %(prog)s set carparam.dat max_rpm 7500 top_speed_limit 2000
  %(prog)s set carparam.dat gear_ratio_table 150,0,380,280,220,180,150,130
  %(prog)s fields
  %(prog)s fields --verbose
  %(prog)s diff car_a/carparam.dat car_b/carparam.dat
""")

    sub = parser.add_subparsers(dest="command", required=True)

    # read
    p_read = sub.add_parser("read", help="Read and display carparam.dat fields")
    p_read.add_argument("file", help="Path to carparam.dat")
    p_read.add_argument("--brief", action="store_true",
                        help="Omit descriptions and confidence tags")

    # export
    p_export = sub.add_parser("export", help="Export carparam.dat to JSON")
    p_export.add_argument("file", help="Path to carparam.dat")
    p_export.add_argument("--json", help="Output JSON file (stdout if omitted)")

    # import
    p_import = sub.add_parser("import", help="Import JSON back to binary carparam.dat")
    p_import.add_argument("file", help="Path to JSON input")
    p_import.add_argument("--out", required=True, help="Output carparam.dat path")

    # set
    p_set = sub.add_parser("set", help="Edit fields in-place: field value [field value ...]")
    p_set.add_argument("file", help="Path to carparam.dat (modified in-place)")
    p_set.add_argument("assignments", nargs="+",
                       help="Field-value pairs: field1 value1 field2 value2 ...")

    # fields
    p_fields = sub.add_parser("fields", help="List all known field names and offsets")
    p_fields.add_argument("--verbose", "-v", action="store_true",
                          help="Show descriptions")

    # diff
    p_diff = sub.add_parser("diff", help="Compare two carparam.dat files")
    p_diff.add_argument("file_a", help="First carparam.dat")
    p_diff.add_argument("file_b", help="Second carparam.dat")

    args = parser.parse_args()

    commands = {
        "read": cmd_read,
        "export": cmd_export,
        "import": cmd_import,
        "set": cmd_set,
        "fields": cmd_fields,
        "diff": cmd_diff,
    }

    commands[args.command](args)


if __name__ == "__main__":
    main()
