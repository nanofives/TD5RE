#!/usr/bin/env python3
"""
carparam_editor.py -- CLI tool for reading, editing, and converting
Test Drive 5 carparam.dat files (0x10C = 268 bytes).

Structure:
  - Tuning table:  0x00..0x8B  (0x8C = 140 bytes)
  - Physics table: 0x8C..0x10B (0x80 = 128 bytes)

Every byte is classified (see `kind` attribute):
  - "live"            : read as-is at runtime
  - "runtime_written" : file bytes OVERWRITTEN at load by ComputeVehicleSuspensionEnvelope
                        (edits to these bytes have no runtime effect)
  - "traffic_only"    : envelope writes only when slot >= 6
  - "dead"            : mutated or written but never consumed
  - "padding"         : uniformly 0x0000; no readers

Round-trip discipline: all bytes are preserved in JSON/TOML export so that
re-import produces an identical file. Editing `dead`/`runtime_written`/`padding`
fields is allowed but flagged with warnings in the serialized format.

Usage:
  python carparam_editor.py read <file>
  python carparam_editor.py export <file> --json  <out.json>
  python carparam_editor.py export <file> --toml  <out.toml>
  python carparam_editor.py import <in.json|.toml> --out <file>
  python carparam_editor.py set <file> <field> <value> [<field> <value> ...]
  python carparam_editor.py diff <a.dat> <b.dat>
  python carparam_editor.py fields [--verbose]
"""

import argparse
import json
import struct
import sys
import tomllib
from collections import OrderedDict
from dataclasses import dataclass
from pathlib import Path
from typing import List, Optional

try:
    import tomli_w
    HAS_TOMLI_W = True
except ImportError:
    HAS_TOMLI_W = False


# ---------------------------------------------------------------------------
# Field descriptor
# ---------------------------------------------------------------------------

# Kind values
KIND_LIVE            = "live"
KIND_RUNTIME_WRITTEN = "runtime_written"
KIND_TRAFFIC_ONLY    = "traffic_only"
KIND_DEAD            = "dead"
KIND_PADDING         = "padding"

KIND_NOTE = {
    KIND_LIVE:            "read at runtime",
    KIND_RUNTIME_WRITTEN: "file bytes OVERWRITTEN at load from mesh data — edits have no runtime effect",
    KIND_TRAFFIC_ONLY:    "envelope writes only when slot >= 6 (traffic vehicles)",
    KIND_DEAD:            "mutated/written but never consumed — edits have no runtime effect",
    KIND_PADDING:         "alignment padding; always 0x0000 in shipping files",
}


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
    kind: str           # live / runtime_written / traffic_only / dead / padding


def _short(name, file_off, tbl, tbl_off, desc, conf="confirmed",
           kind=KIND_LIVE, fp=256, signed=True):
    return FieldDef(name, file_off, 2, 1, signed, fp, tbl, tbl_off, desc, conf, kind)

def _int32(name, file_off, tbl, tbl_off, desc, conf="confirmed",
           kind=KIND_LIVE, fp=0, signed=True):
    return FieldDef(name, file_off, 4, 1, signed, fp, tbl, tbl_off, desc, conf, kind)

def _short_arr(name, file_off, tbl, tbl_off, count, desc, conf="confirmed",
               kind=KIND_LIVE, fp=0, signed=True):
    return FieldDef(name, file_off, 2, count, signed, fp, tbl, tbl_off, desc, conf, kind)


# ---------------------------------------------------------------------------
# Full field table — every byte of the 0x10C file accounted for.
# Revised 2026-04-23 after full coverage sweep; see
# re/analysis/formats/carparam-physics-table.md.
# ---------------------------------------------------------------------------

CARPARAM_SIZE = 0x10C       # 268 bytes
TUNING_SIZE   = 0x8C        # 140 bytes
PHYSICS_SIZE  = 0x80        # 128 bytes

FIELDS: List[FieldDef] = [
    # ===== TUNING TABLE (file 0x00..0x8B) =====

    # Chassis AABB corners (0x00..0x3F) — overwritten at load by ComputeVehicleSuspensionEnvelope
    _short_arr("chassis_corners_front", 0x00, "tuning", 0x00, 16,
               "8 corner vertices (x,y,z,w-pad; short[16]) written by "
               "ComputeVehicleSuspensionEnvelope from mesh AABB. Read at runtime by "
               "CollectVehicleCollisionContacts, ApplyVehicleCollisionImpulse, "
               "ProcessActorSegmentTransition, ProcessActorForwardCheckpointPass. "
               "File bytes are always overwritten before any reader fires.",
               kind=KIND_RUNTIME_WRITTEN),
    _short_arr("chassis_corners_rear", 0x20, "tuning", 0x20, 16,
               "Second corner-vertex region. Envelope writes here, but no runtime reader "
               "accesses this range. On-disk values are rotated duplicates of 0x00-0x1F.",
               kind=KIND_DEAD),

    # Wheel positions (0x40..0x5F) — live, copied to actor+0x210 at init
    _short_arr("wheel_pos_FL", 0x40, "tuning", 0x40, 4,
               "Front-left wheel local position (x, y, z, w-pad=0). "
               "InitializeRaceVehicleRuntime copies all 4 wheel positions to actor+0x210.",
               kind=KIND_LIVE),
    _short_arr("wheel_pos_FR", 0x48, "tuning", 0x48, 4,
               "Front-right wheel local position (x, y, z, w-pad=0).",
               kind=KIND_LIVE),
    _short_arr("wheel_pos_RL", 0x50, "tuning", 0x50, 4,
               "Rear-left wheel local position (x, y, z, w-pad=0).",
               kind=KIND_LIVE),
    _short_arr("wheel_pos_RR", 0x58, "tuning", 0x58, 4,
               "Rear-right wheel local position (x, y, z, w-pad=0).",
               kind=KIND_LIVE),

    # Traffic-only alternate wheel templates (0x60..0x7F)
    _short_arr("traffic_alt_wheel_A", 0x60, "tuning", 0x60, 8,
               "Traffic-only alternate wheel template A (2 vertices x [x,y,z,w-pad]). "
               "ComputeVehicleSuspensionEnvelope writes this region only when slot >= 6. "
               "Player/AI carparam.dat values for this range are ignored.",
               kind=KIND_TRAFFIC_ONLY),
    _short_arr("traffic_alt_wheel_B", 0x70, "tuning", 0x70, 8,
               "Traffic-only alternate wheel template B. Envelope writes only for slot >= 6.",
               kind=KIND_TRAFFIC_ONLY),

    # Chassis top + suspension reference (0x80..0x87)
    _short("chassis_top_y", 0x80, "tuning", 0x80,
           "Chassis max-Y (envelope-written from mesh). Read by ResolveSimpleActorSeparation "
           "(separation radius = (A.top + B.top) * 3/4) and ProcessActorForwardCheckpointPass.",
           kind=KIND_RUNTIME_WRITTEN, fp=0),
    _short("suspension_height_ref", 0x82, "tuning", 0x82,
           "Vertical offset for suspension preload. Scaled by 0xB5/256 in "
           "RefreshVehicleWheelContactFrames; used in IntegrateVehiclePoseAndContacts.",
           kind=KIND_LIVE, fp=256),
    _short("envelope_reference_y", 0x84, "tuning", 0x84,
           "Per-vehicle Y reference consumed by ComputeVehicleSuspensionEnvelope "
           "@ 0x42F7CC to compute a running-max Y delta against wheel_pos_FL.y "
           "(tun+0x40). Paired with tun+0x82/0x42 in a matching block at 0x42F893.",
           kind=KIND_LIVE, fp=0),
    _short("traffic_y_offset", 0x86, "tuning", 0x86,
           "Per-traffic vertical pose offset. Envelope writes unconditionally. Read at "
           "UpdateTrafficVehiclePose @ 0x443D82: pose_y += val << 8. "
           "Player/AI path never reads.",
           kind=KIND_RUNTIME_WRITTEN, fp=0),

    # Collision mass + padding
    _short("collision_mass", 0x88, "tuning", 0x88,
           "Inverse mass in collision impulse. ApplyVehicleCollisionImpulse @ 0x4079C0: "
           "impulse = (DAT/256 * 0x1100) / ((r1^2+DAT)*mass1 + (r2^2+DAT)*mass2) / 256. "
           "Higher value = heavier = less knockback. Traffic override = 0x20.",
           kind=KIND_LIVE, fp=0),
    _short("_pad_8A", 0x8A, "tuning", 0x8A,
           "Alignment padding rounding tuning table to 0x8C stride. "
           "All shipping files contain 0x0000; no readers.",
           kind=KIND_PADDING, fp=0),

    # ===== PHYSICS TABLE (file 0x8C..0x10B) =====

    _short_arr("torque_curve", 0x8C, "physics", 0x00, 16,
               "Engine torque curve (8.8). 16 samples covering engine speed 0-8192 "
               "(512 RPM per entry). Interpolated in ComputeDriveTorqueFromGearCurve.",
               kind=KIND_LIVE, fp=256),

    _int32("vehicle_inertia", 0xAC, "physics", 0x20,
           "Yaw moment of inertia (int32, 24.8). Divisor in yaw_torque / (inertia / 0x28C). "
           "Also used in weight transfer: (rear_weight^2 + inertia) >> 10.",
           kind=KIND_LIVE),
    _int32("half_wheelbase", 0xB0, "physics", 0x24,
           "Half wheelbase distance (int32, 24.8 world units). Full wheelbase = half * 2.",
           kind=KIND_LIVE),

    _short("front_weight_dist", 0xB4, "physics", 0x28,
           "Front axle weight numerator. front_load = (front<<8)/(front+rear). "
           "Dimensionless relative value.",
           kind=KIND_LIVE, fp=0),
    _short("rear_weight_dist", 0xB6, "physics", 0x2A,
           "Rear axle weight numerator. Together with front defines CG bias.",
           kind=KIND_LIVE, fp=0),

    _short("drag_coefficient", 0xB8, "physics", 0x2C,
           "Aerodynamic drag coefficient (8.8). Difficulty-scaled: "
           "normal *300/256, hard *380/256.",
           kind=KIND_LIVE, fp=256),

    _short_arr("gear_ratio_table", 0xBA, "physics", 0x2E, 8,
               "Gear ratios (8.8). [0]=reverse, [1]=neutral, [2-7]=forward gears 1-6. "
               "Used as multiplier/256 in torque/speed.",
               kind=KIND_LIVE, fp=256),

    _short_arr("upshift_rpm_table", 0xCA, "physics", 0x3E, 8,
               "Upshift RPM thresholds (raw rpm). 9999 = \"never upshift\".",
               kind=KIND_LIVE, fp=0),
    _short_arr("downshift_rpm_table", 0xDA, "physics", 0x4E, 8,
               "Downshift RPM thresholds (raw rpm).",
               kind=KIND_LIVE, fp=0),

    _short("suspension_damping", 0xEA, "physics", 0x5E,
           "Suspension damping coefficient (8.8). displacement * damping / 256.",
           kind=KIND_LIVE, fp=256),
    _short("suspension_spring_rate", 0xEC, "physics", 0x60,
           "Suspension spring rate (8.8). velocity * spring / 256.",
           kind=KIND_LIVE, fp=256),
    _short("suspension_feedback", 0xEE, "physics", 0x62,
           "Cross-axis coupling (8.8). Couples lateral/vertical forces into wheel travel: "
           "cross_term * feedback / 256.",
           kind=KIND_LIVE, fp=256),
    _short("suspension_travel_limit", 0xF0, "physics", 0x64,
           "Max wheel displacement (24.8, symmetrical +/-). Clamped on bottom-out.",
           kind=KIND_LIVE, fp=0),
    _short("suspension_response_factor", 0xF2, "physics", 0x66,
           "Velocity response (8.8). prev_vel * response / 256.",
           kind=KIND_LIVE, fp=256),

    _short("drive_torque_multiplier", 0xF4, "physics", 0x68,
           "Drive torque scaling (8.8). Difficulty: normal *360/256, hard *650/256.",
           kind=KIND_LIVE, fp=256),
    _short("damping_low_speed", 0xF6, "physics", 0x6A,
           "Low-speed velocity damping (8.8). Active when frame_counter < 0x20 or gear < 2.",
           kind=KIND_LIVE, fp=256),
    _short("damping_high_speed", 0xF8, "physics", 0x6C,
           "High-speed velocity damping (8.8). Active when frame_counter >= 0x20 and gear >= 2.",
           kind=KIND_LIVE, fp=256),
    _short("brake_force", 0xFA, "physics", 0x6E,
           "Brake deceleration (8.8 speed-units/tick^2). Hard mode: *450/256.",
           kind=KIND_LIVE, fp=256),
    _short("engine_brake_force", 0xFC, "physics", 0x70,
           "Engine braking force (8.8). Hard mode: *400/256.",
           kind=KIND_LIVE, fp=256),
    _short("max_rpm", 0xFE, "physics", 0x72,
           "Engine redline (raw rpm). Torque returns 0 above max_rpm - 50.",
           kind=KIND_LIVE, fp=0),
    _short("top_speed_limit", 0x100, "physics", 0x74,
           "Hard speed cap (raw 16.0; compared *256 against 24.8 internal speed).",
           kind=KIND_LIVE, fp=0),
    _short("drivetrain_type", 0x102, "physics", 0x76,
           "Drivetrain enum: 1=RWD, 2=FWD, 3=AWD.",
           kind=KIND_LIVE, fp=0),
    _short("speed_scale_factor", 0x104, "physics", 0x78,
           "WRITE-ONLY / DEAD: mutated at init (<<=1 normal, <<=2 hard) but never read. "
           "2026-04-23 exhaustive sweep found zero consumers. File values: 9 distinct "
           "(32..144, multiples of 4); no correlation with torque/redline/top-speed. "
           "Likely a designer rating whose consumer was cut before ship.",
           kind=KIND_DEAD, fp=0),
    _short("handbrake_grip_modifier", 0x106, "physics", 0x7A,
           "Rear grip multiplier when handbrake active (8.8, always <256 => reduces grip).",
           kind=KIND_LIVE, fp=256),
    _short("lateral_slip_stiffness", 0x108, "physics", 0x7C,
           "Slip-circle speed coupling (aka slip_circle_speed_coupling). Single reader "
           "at UpdatePlayerVehicleDynamics 0x404185. Weights the along-axle speed component "
           "into the slip-circle hypotenuse. Higher -> slip grip saturates faster at speed.",
           kind=KIND_LIVE, fp=256),
    _short("_pad_7E", 0x10A, "physics", 0x7E,
           "Alignment padding rounding physics table to 0x80 stride. "
           "All shipping files contain 0x0000; no readers.",
           kind=KIND_PADDING, fp=0),
]

# Build lookup by name
FIELD_BY_NAME = OrderedDict((f.name, f) for f in FIELDS)

# Drivetrain enum for display
DRIVETRAIN_NAMES = {1: "RWD", 2: "FWD", 3: "AWD"}


# ---------------------------------------------------------------------------
# Binary I/O helpers
# ---------------------------------------------------------------------------

def _struct_fmt(field: FieldDef) -> str:
    if field.size == 1:
        return "b" if field.signed else "B"
    elif field.size == 2:
        return "h" if field.signed else "H"
    elif field.size == 4:
        return "i" if field.signed else "I"
    raise ValueError(f"Unsupported field size {field.size}")


def read_field(data: bytes, field: FieldDef):
    fmt_char = _struct_fmt(field)
    fmt = f"<{field.count}{fmt_char}"
    values = struct.unpack_from(fmt, data, field.file_offset)
    if field.count == 1:
        return values[0]
    return list(values)


def write_field(data: bytearray, field: FieldDef, value):
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
    result = OrderedDict()
    for f in FIELDS:
        result[f.name] = read_field(data, f)
    return result


def write_all_fields(fields_dict: dict) -> bytearray:
    data = bytearray(CARPARAM_SIZE)
    for f in FIELDS:
        if f.name in fields_dict:
            write_field(data, f, fields_dict[f.name])
    return data


def _pad_or_trim(data: bytes, path: str = "") -> bytes:
    if len(data) == CARPARAM_SIZE:
        return data
    msg = f"file is {len(data)} bytes, expected {CARPARAM_SIZE} (0x{CARPARAM_SIZE:X})"
    if path:
        msg = f"{path}: {msg}"
    print(f"WARNING: {msg}", file=sys.stderr)
    if len(data) < CARPARAM_SIZE:
        return data + b'\x00' * (CARPARAM_SIZE - len(data))
    return data[:CARPARAM_SIZE]


# ---------------------------------------------------------------------------
# Display
# ---------------------------------------------------------------------------

def format_value(field: FieldDef, value) -> str:
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


KIND_TAG = {
    KIND_LIVE:            "[live]",
    KIND_RUNTIME_WRITTEN: "[runtime]",
    KIND_TRAFFIC_ONLY:    "[traffic]",
    KIND_DEAD:            "[DEAD]",
    KIND_PADDING:         "[pad]",
}


def display_fields(data: bytes, verbose: bool = True):
    current_table = None
    for f in FIELDS:
        if f.table != current_table:
            current_table = f.table
            header = "TUNING TABLE" if current_table == "tuning" else "PHYSICS TABLE"
            rng = "(file 0x00..0x8B)" if current_table == "tuning" else "(file 0x8C..0x10B)"
            print(f"\n{'='*78}")
            print(f"  {header}  {rng}")
            print(f"{'='*78}")

        value = read_field(data, f)
        val_str = format_value(f, value)
        off_str = f"0x{f.file_offset:03X}"
        tbl_off = f"{f.table[:3]}+0x{f.table_offset:02X}"
        kind_tag = KIND_TAG.get(f.kind, f"[{f.kind}]")

        print(f"  {off_str} {tbl_off:10s} {kind_tag:10s} {f.name:35s} = {val_str}")
        if verbose:
            print(f"  {'':>16s} [{f.confidence}] {f.description}")


# ---------------------------------------------------------------------------
# Serialization — JSON and TOML share the same logical structure
# ---------------------------------------------------------------------------

FORMAT_ID = "td5_carparam"
FORMAT_VERSION = 2


def _field_entry(f: FieldDef, value) -> OrderedDict:
    entry = OrderedDict()
    entry["value"] = value
    entry["offset"] = f"0x{f.file_offset:03X}"
    entry["table"] = f.table
    entry["table_offset"] = f"0x{f.table_offset:02X}"
    type_str = f"{'s' if f.signed else 'u'}int{f.size*8}"
    if f.count > 1:
        type_str += f"[{f.count}]"
    entry["type"] = type_str
    entry["kind"] = f.kind
    entry["confidence"] = f.confidence
    if f.fp_scale > 0:
        entry["fp_scale"] = f.fp_scale
        if f.count > 1:
            entry["scaled"] = [v / f.fp_scale for v in value]
        else:
            entry["scaled"] = value / f.fp_scale
    entry["description"] = f.description
    return entry


def export_dict(data: bytes) -> dict:
    """Produce a serialization-ready dict. Order matches file layout."""
    fields = read_all_fields(data)
    result = OrderedDict()
    result["_format"] = FORMAT_ID
    result["_version"] = FORMAT_VERSION
    result["_size"] = CARPARAM_SIZE
    result["_description"] = (
        "Test Drive 5 carparam.dat — tuning (0x8C bytes) + physics (0x80 bytes). "
        "All values are raw integers as stored in the binary file. Every byte is "
        "round-trip preserved: editing 'dead', 'runtime_written', or 'padding' fields "
        "will change the on-disk bytes but will have no runtime effect."
    )
    result["_kind_legend"] = dict(KIND_NOTE)
    for f in FIELDS:
        result[f.name] = _field_entry(f, fields[f.name])
    return result


def import_dict(parsed: dict) -> bytearray:
    """Reverse of export_dict. Accepts full-metadata or flat {name: value} form."""
    fields = OrderedDict()
    for f in FIELDS:
        if f.name not in parsed:
            raise ValueError(f"Missing field '{f.name}' in input")
        entry = parsed[f.name]
        val = entry["value"] if isinstance(entry, dict) else entry
        if f.count > 1:
            if not isinstance(val, list):
                raise ValueError(f"Field '{f.name}' should be a list of {f.count} values")
            if len(val) != f.count:
                raise ValueError(f"Field '{f.name}' expects {f.count} values, got {len(val)}")
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
# TOML
# ---------------------------------------------------------------------------

def export_toml(data: bytes) -> str:
    """Emit a human-friendly TOML representation.

    Top-level scalar keys come first, then [tuning.<name>] and [physics.<name>]
    tables in file-layout order. Each field table includes a comment line above
    it summarizing its kind (live/runtime_written/traffic_only/dead/padding) so
    a human editor sees the warning next to the value.
    """
    if not HAS_TOMLI_W:
        raise RuntimeError("tomli_w is required for TOML export — pip install tomli_w")

    fields = read_all_fields(data)
    lines = []
    lines.append(f'# {FORMAT_ID} v{FORMAT_VERSION}')
    lines.append(f'# Test Drive 5 carparam.dat — {CARPARAM_SIZE} bytes total')
    lines.append("# Kinds: live = read at runtime; runtime_written = overwritten at load from mesh,")
    lines.append("#   edits have no effect; traffic_only = read only for traffic vehicles;")
    lines.append("#   dead = mutated but never consumed; padding = always 0x0000.")
    lines.append('')
    lines.append(f'_format = "{FORMAT_ID}"')
    lines.append(f'_version = {FORMAT_VERSION}')
    lines.append(f'_size = {CARPARAM_SIZE}')
    lines.append('')

    current_table = None
    for f in FIELDS:
        if f.table != current_table:
            current_table = f.table
            header = "TUNING TABLE (file 0x00..0x8B)" if current_table == "tuning" \
                     else "PHYSICS TABLE (file 0x8C..0x10B)"
            lines.append(f'# ==== {header} ====')
            lines.append('')

        kind_note = KIND_NOTE.get(f.kind, f.kind)
        # Per-field warning comment
        if f.kind != KIND_LIVE:
            lines.append(f'# {f.kind.upper()}: {kind_note}')
        # Description broken across lines if long
        desc = f.description
        if len(desc) <= 110:
            lines.append(f'# {desc}')
        else:
            # Wrap every ~100 chars at word boundaries
            words, line = desc.split(), ""
            for w in words:
                if len(line) + len(w) + 1 > 100:
                    lines.append(f'# {line.strip()}')
                    line = w
                else:
                    line += (" " + w) if line else w
            if line:
                lines.append(f'# {line.strip()}')

        lines.append(f'[{f.table}.{f.name}]')
        value = fields[f.name]

        # Emit value as inline; arrays on one line if short enough, multi-line otherwise
        if f.count > 1:
            joined = ", ".join(str(v) for v in value)
            if len(joined) <= 80:
                lines.append(f'value = [{joined}]')
            else:
                lines.append('value = [')
                # Chunk 8 per line
                for i in range(0, len(value), 8):
                    chunk = value[i:i+8]
                    comma = "," if (i + 8) < len(value) else ""
                    lines.append('    ' + ", ".join(str(v) for v in chunk) + comma)
                lines.append(']')
        else:
            lines.append(f'value = {value}')

        lines.append(f'offset = "0x{f.file_offset:03X}"')
        lines.append(f'table_offset = "0x{f.table_offset:02X}"')
        type_str = f"{'s' if f.signed else 'u'}int{f.size*8}"
        if f.count > 1:
            type_str += f"[{f.count}]"
        lines.append(f'type = "{type_str}"')
        lines.append(f'kind = "{f.kind}"')
        lines.append(f'confidence = "{f.confidence}"')
        if f.fp_scale > 0:
            lines.append(f'fp_scale = {f.fp_scale}')
            if f.count > 1:
                scaled = ", ".join(f'{v / f.fp_scale:.4f}' for v in value)
                if len(scaled) <= 80:
                    lines.append(f'scaled = [{scaled}]')
            else:
                lines.append(f'scaled = {value / f.fp_scale:.4f}')
        if f.name == "drivetrain_type" and isinstance(value, int) and value in DRIVETRAIN_NAMES:
            lines.append(f'enum_name = "{DRIVETRAIN_NAMES[value]}"')

        lines.append('')

    return "\n".join(lines) + "\n"


def import_toml(text: str) -> bytearray:
    """Parse TOML produced by export_toml (or a compatible flat form).

    Accepts either:
      [tuning.<name>] / [physics.<name>] sections with `value = ...` keys, OR
      a flat top-level {name: value} mapping.
    """
    parsed = tomllib.loads(text)

    # Gather all field entries, searching in both nested and flat forms.
    flattened = {}
    for f in FIELDS:
        if f.table in parsed and isinstance(parsed[f.table], dict) \
                and f.name in parsed[f.table]:
            flattened[f.name] = parsed[f.table][f.name]
        elif f.name in parsed:
            flattened[f.name] = parsed[f.name]
        else:
            raise ValueError(f"Missing field '{f.name}' in TOML input "
                             f"(looked in [{f.table}.{f.name}] and top-level)")

    return import_dict(flattened)


# ---------------------------------------------------------------------------
# CLI commands
# ---------------------------------------------------------------------------

def _load_binary(path: str) -> bytes:
    with open(path, "rb") as fh:
        return _pad_or_trim(fh.read(), path)


def cmd_read(args):
    data = _load_binary(args.file)
    display_fields(data, verbose=not args.brief)


def _detect_format(path: str) -> str:
    ext = Path(path).suffix.lower()
    if ext == ".toml":
        return "toml"
    if ext == ".json":
        return "json"
    raise ValueError(f"Cannot detect format from extension: {path}. "
                     f"Use --json or --toml explicitly.")


def cmd_export(args):
    data = _load_binary(args.file)

    out_path = args.json or args.toml
    fmt = "json" if args.json else ("toml" if args.toml else None)
    if out_path and fmt is None:
        fmt = _detect_format(out_path)

    if fmt is None:
        # Default to JSON on stdout when no output file specified
        fmt = "json"

    if fmt == "toml":
        text = export_toml(data)
    else:
        text = export_json(data)

    if out_path:
        with open(out_path, "w", encoding="utf-8") as fh:
            fh.write(text)
        print(f"Exported to {out_path}")
    else:
        sys.stdout.write(text)


def cmd_import(args):
    path = args.file
    with open(path, "r", encoding="utf-8") as fh:
        text = fh.read()

    fmt = _detect_format(path)
    if fmt == "toml":
        data = import_toml(text)
    else:
        data = import_json(text)

    if len(data) != CARPARAM_SIZE:
        print(f"ERROR: produced {len(data)} bytes, expected {CARPARAM_SIZE}", file=sys.stderr)
        sys.exit(1)

    with open(args.out, "wb") as fh:
        fh.write(data)
    print(f"Wrote {len(data)} bytes to {args.out}")


def cmd_set(args):
    filepath = args.file
    with open(filepath, "rb") as fh:
        data = bytearray(_pad_or_trim(fh.read(), filepath))

    pairs = args.assignments
    if len(pairs) % 2 != 0:
        print("ERROR: set expects <field> <value> pairs", file=sys.stderr)
        sys.exit(1)

    for i in range(0, len(pairs), 2):
        name = pairs[i]
        raw_value = pairs[i + 1]

        if name not in FIELD_BY_NAME:
            matches = [n for n in FIELD_BY_NAME if name in n]
            hint = f" Did you mean: {', '.join(matches)}?" if matches else \
                " Use 'fields' to see all field names."
            print(f"ERROR: Unknown field '{name}'.{hint}", file=sys.stderr)
            sys.exit(1)

        field = FIELD_BY_NAME[name]

        if field.kind != KIND_LIVE:
            print(f"WARNING: field '{name}' is kind={field.kind} — "
                  f"edit will persist on disk but {KIND_NOTE[field.kind]}", file=sys.stderr)

        if field.count > 1:
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
    current_table = None
    for f in FIELDS:
        if f.table != current_table:
            current_table = f.table
            print(f"\n--- {current_table.upper()} TABLE ---")
        size_str = f"{f.size * f.count}B"
        arr_str = f"[{f.count}]" if f.count > 1 else ""
        type_str = f"{'s' if f.signed else 'u'}int{f.size*8}{arr_str}"
        kind_tag = KIND_TAG.get(f.kind, f"[{f.kind}]")
        print(f"  0x{f.file_offset:03X}  {kind_tag:10s} {f.name:35s} "
              f"{type_str:14s} {size_str:>4s}  [{f.confidence}]")
        if args.verbose:
            print(f"         {f.description}")


def cmd_diff(args):
    with open(args.file_a, "rb") as fh:
        data_a = _pad_or_trim(fh.read(), args.file_a)
    with open(args.file_b, "rb") as fh:
        data_b = _pad_or_trim(fh.read(), args.file_b)

    fields_a = read_all_fields(data_a)
    fields_b = read_all_fields(data_b)

    diffs = 0
    for f in FIELDS:
        va = fields_a[f.name]
        vb = fields_b[f.name]
        if va != vb:
            diffs += 1
            kind_tag = KIND_TAG.get(f.kind, f"[{f.kind}]")
            print(f"  {kind_tag:10s} {f.name:35s}  "
                  f"A={format_value(f, va):>24s}  B={format_value(f, vb):>24s}")

    if diffs == 0:
        print("Files are identical.")
    else:
        print(f"\n{diffs} field(s) differ.")


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(
        description="Test Drive 5 carparam.dat editor (JSON/TOML round-tripper)",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""\
Examples:
  %(prog)s read carparam.dat
  %(prog)s read carparam.dat --brief
  %(prog)s export carparam.dat --toml cam.toml
  %(prog)s export carparam.dat --json cam.json
  %(prog)s import cam.toml --out carparam.dat
  %(prog)s set carparam.dat max_rpm 7500 top_speed_limit 2000
  %(prog)s set carparam.dat gear_ratio_table 150,0,380,280,220,180,150,130
  %(prog)s fields --verbose
  %(prog)s diff car_a/carparam.dat car_b/carparam.dat
""")

    sub = parser.add_subparsers(dest="command", required=True)

    p_read = sub.add_parser("read", help="Read and display carparam.dat fields")
    p_read.add_argument("file", help="Path to carparam.dat")
    p_read.add_argument("--brief", action="store_true",
                        help="Omit descriptions and confidence tags")

    p_export = sub.add_parser("export", help="Export carparam.dat to JSON or TOML")
    p_export.add_argument("file", help="Path to carparam.dat")
    fmt_group = p_export.add_mutually_exclusive_group()
    fmt_group.add_argument("--json", help="Output JSON file")
    fmt_group.add_argument("--toml", help="Output TOML file")

    p_import = sub.add_parser("import", help="Import JSON/TOML back to binary carparam.dat")
    p_import.add_argument("file", help="Path to .json or .toml input")
    p_import.add_argument("--out", required=True, help="Output carparam.dat path")

    p_set = sub.add_parser("set", help="Edit fields in-place: field value [field value ...]")
    p_set.add_argument("file", help="Path to carparam.dat (modified in-place)")
    p_set.add_argument("assignments", nargs="+",
                       help="Field-value pairs: field1 value1 field2 value2 ...")

    p_fields = sub.add_parser("fields", help="List all known field names and offsets")
    p_fields.add_argument("--verbose", "-v", action="store_true",
                          help="Show descriptions")

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
