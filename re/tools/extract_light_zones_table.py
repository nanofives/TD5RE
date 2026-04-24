#!/usr/bin/env python3
"""
extract_light_zones_table.py -- Rip the per-track light-zone records from
original/TD5_d3d.exe for the port's environs-projection AND track-lighting
subsystems.

Source data:
  * 0x00469c78  per-track pointer table (parallel to the environs table @
                0x0046bb1c). Each non-null pointer leads to a contiguous array
                of 0x24-byte zone records. Per-track zone count is derived
                from the pointer delta (next - curr) / 0x24.
  * Last populated track runs up to the environs-struct pool start @
    0x00469d20 (next data section).

Per-zone struct (0x24 bytes) -- ALL fields used by the original lighting code:

  +0x00  int16   span_lo            zone covers [span_lo, span_hi] inclusive
  +0x02  int16   span_hi
  +0x04  int16   dir_x              directional-light vector (world space)
  +0x06  int16   dir_y                  scaled by avg_weight * (1/1024) inside
  +0x08  int16   dir_z                  SetTrackLightDirectionContribution
  +0x0a  int8    pad_0a             unused
  +0x0b  int8    pad_0b
  +0x0c  uint8   weight_r           per-channel directional weight bytes
  +0x0d  uint8   weight_g               (avg = directional intensity)
  +0x0e  uint8   weight_b
  +0x0f  uint8   pad_0f
  +0x10  uint8   amb_r              per-channel ambient bytes
  +0x11  uint8   amb_g                  (avg = scalar ambient added to dot)
  +0x12  uint8   amb_b
  +0x13  uint8   pad_13
  +0x14  int16   pos_off_x          XY world-space offset added to track
  +0x16  int16   pos_off_y              vertex samples in cases 1/2 (track
                                        sampling for blend modes)
  +0x18  uint8   blend_mode         0 = static dir, 1 = transition w/ start/end
                                    blend, 2 = midspan multi-sample loop,
                                    3 = full-zone half/half blend
  +0x19  uint8   spacing            stride for vertex sampling (cases 1/2)
  +0x1a  uint8   sub_mode           case-2 vertex selection (0/1/2)
  +0x1b  uint8   multiplier         case-1/2 angle dampening multiplier
  +0x1c  uint32  state_key          state-machine trigger; low byte (0..3) is
                                    also used as the environs light_index
  +0x20  uint32  slot_color         written to DAT_004c38a0[slot] (consumer is
                                    not currently wired in port)

The port currently uses span_lo, span_hi and (state_key & 0xFF) as
light_index for environs projection. The new fields drive
ApplyTrackLightingForVehicleSegment @ 0x00430150, which sets the active
3-light + ambient basis used by ComputeMeshVertexLighting.
"""
import os
import struct
import sys

try:
    import pefile
except ImportError:
    print("ERROR: pefile required. pip install pefile", file=sys.stderr)
    sys.exit(1)

EXE_PATH           = "original/TD5_d3d.exe"
OUT_PATH           = "td5mod/src/td5re/td5_light_zones_table.inc"
POINTER_TABLE_VA   = 0x00469c78
ZONE_POOL_START_VA = 0x004673d8      # first zone-pool pointee (track 1)
ZONE_POOL_END_VA   = 0x00469d20      # environs struct pool starts here
ZONE_STRUCT_SIZE   = 0x24
TRACK_SLOT_MAX     = 42              # matches TD5_ENVIRONS_TRACK_COUNT


def va_to_fo(pe, va):
    rva = va - pe.OPTIONAL_HEADER.ImageBase
    return pe.get_offset_from_rva(rva)


def parse_zone(buf, off):
    span_lo, span_hi = struct.unpack_from("<hh", buf, off + 0x00)
    dir_x, dir_y, dir_z = struct.unpack_from("<hhh", buf, off + 0x04)
    weight_r, weight_g, weight_b = struct.unpack_from("BBB", buf, off + 0x0c)
    amb_r, amb_g, amb_b = struct.unpack_from("BBB", buf, off + 0x10)
    pos_off_x, pos_off_y = struct.unpack_from("<hh", buf, off + 0x14)
    blend_mode = buf[off + 0x18]
    spacing    = buf[off + 0x19]
    sub_mode   = buf[off + 0x1a]
    multiplier = buf[off + 0x1b]
    state_key  = struct.unpack_from("<I", buf, off + 0x1c)[0]
    slot_color = struct.unpack_from("<I", buf, off + 0x20)[0]
    return {
        "span_lo": span_lo, "span_hi": span_hi,
        "dir_x": dir_x, "dir_y": dir_y, "dir_z": dir_z,
        "weight_r": weight_r, "weight_g": weight_g, "weight_b": weight_b,
        "amb_r": amb_r, "amb_g": amb_g, "amb_b": amb_b,
        "pos_off_x": pos_off_x, "pos_off_y": pos_off_y,
        "blend_mode": blend_mode, "spacing": spacing,
        "sub_mode": sub_mode, "multiplier": multiplier,
        "state_key": state_key, "slot_color": slot_color,
    }


def is_plausible(z):
    if not (-1 <= z["span_lo"] <= 20000):
        return False
    if not (z["span_lo"] <= z["span_hi"] <= 20000):
        return False
    # blend_mode is documented 0..3 in the dispatch switch
    if z["blend_mode"] > 3:
        return False
    # state_key low byte is the environs light_index (0..3); upper bytes seen 0
    if (z["state_key"] & 0xFF) > 3:
        return False
    if (z["state_key"] >> 8) != 0:
        return False
    return True


def main():
    pe = pefile.PE(EXE_PATH, fast_load=True)
    with open(EXE_PATH, "rb") as f:
        raw = f.read()

    # --- Read the per-track pointer table ---
    tbl = va_to_fo(pe, POINTER_TABLE_VA)
    pointers = []
    for i in range(TRACK_SLOT_MAX):
        p = struct.unpack_from("<I", raw, tbl + 4 * i)[0]
        if p == 0:
            pointers.append(None)
        elif not (ZONE_POOL_START_VA <= p < ZONE_POOL_END_VA):
            pointers.append(None)
        else:
            pointers.append(p)

    # --- Derive per-track zone count from pointer deltas ---
    non_null = sorted({p for p in pointers if p is not None})
    next_ptr = {}
    for i, p in enumerate(non_null):
        next_ptr[p] = non_null[i + 1] if i + 1 < len(non_null) else ZONE_POOL_END_VA

    all_zones = []
    track_offsets = []
    for p in pointers:
        if p is None:
            track_offsets.append((0, 0))
            continue
        max_candidates = (next_ptr[p] - p) // ZONE_STRUCT_SIZE
        start = len(all_zones)
        kept = 0
        for z in range(max_candidates):
            zone = parse_zone(raw, va_to_fo(pe, p) + z * ZONE_STRUCT_SIZE)
            if not is_plausible(zone):
                break
            all_zones.append(zone)
            kept += 1
        track_offsets.append((start, kept))

    # --- Emit C header ---
    lines = []
    lines.append("/* td5_light_zones_table.inc -- GENERATED by re/tools/extract_light_zones_table.py")
    lines.append(" * Source: original/TD5_d3d.exe per-track light-zone table @ 0x00469c78.")
    lines.append(" * Do not edit by hand; re-run the extractor if the source changes.")
    lines.append(" *")
    lines.append(" * Each zone covers [span_lo, span_hi] inclusive on the per-track ring.")
    lines.append(" * Two consumers read these records:")
    lines.append(" *  1. environs/chrome projection (uses light_index = state_key & 0xFF)")
    lines.append(" *  2. ApplyTrackLightingForVehicleSegment @ 0x00430150 (uses every other field)")
    lines.append(" */")
    lines.append("")
    lines.append("typedef struct {")
    lines.append("    short span_lo;")
    lines.append("    short span_hi;")
    lines.append("    short dir_x, dir_y, dir_z;     /* directional light vector (world frame) */")
    lines.append("    unsigned char weight_r, weight_g, weight_b; /* per-channel directional weight */")
    lines.append("    unsigned char amb_r, amb_g, amb_b;          /* per-channel ambient */")
    lines.append("    short pos_off_x, pos_off_y;    /* track-vertex sample offset (cases 1/2) */")
    lines.append("    unsigned char blend_mode;      /* 0=static, 1=transition, 2=multi-sample, 3=half-blend */")
    lines.append("    unsigned char spacing;         /* track-vertex stride for sampling */")
    lines.append("    unsigned char sub_mode;        /* case-2 vertex pick: 0=normal,1=alt edge,2=midpoint */")
    lines.append("    unsigned char multiplier;      /* angle dampening multiplier (cases 1/2) */")
    lines.append("    unsigned int  state_key;       /* state-machine trigger; & 0xFF = environs light_index */")
    lines.append("    unsigned int  slot_color;      /* DAT_004c38a0[slot] entry (currently unused in port) */")
    lines.append("    int   light_index;             /* convenience alias = state_key & 0xFF (0..3) */")
    lines.append("} TD5_LightZone;")
    lines.append("")
    lines.append(f"#define TD5_LIGHT_ZONE_COUNT   {len(all_zones)}")
    lines.append(f"#define TD5_LIGHT_ZONE_TRACK_COUNT {len(track_offsets)}")
    lines.append("")
    lines.append("static const TD5_LightZone td5_light_zones[TD5_LIGHT_ZONE_COUNT] = {")
    for i, z in enumerate(all_zones):
        lines.append(
            "    /* {idx:4d} */ {{ {span_lo:>5}, {span_hi:>5}, "
            "{dir_x:>6}, {dir_y:>6}, {dir_z:>6}, "
            "{weight_r:>3}, {weight_g:>3}, {weight_b:>3}, "
            "{amb_r:>3}, {amb_g:>3}, {amb_b:>3}, "
            "{pos_off_x:>5}, {pos_off_y:>5}, "
            "{blend_mode}, {spacing:>3}, {sub_mode}, {multiplier:>3}, "
            "0x{state_key:08x}, 0x{slot_color:08x}, {light_index} }},"
            .format(idx=i, light_index=z["state_key"] & 0xFF, **z)
        )
    lines.append("};")
    lines.append("")
    lines.append("/* Per-track [first_zone, zone_count] offsets. */")
    lines.append("static const struct { short first; short count; } td5_light_zone_track[TD5_LIGHT_ZONE_TRACK_COUNT] = {")
    for slot, (start, count) in enumerate(track_offsets):
        lines.append(f"    /* track {slot:2d} */ {{ {start:>5}, {count:>3} }},")
    lines.append("};")
    lines.append("")

    os.makedirs(os.path.dirname(OUT_PATH), exist_ok=True)
    with open(OUT_PATH, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines))

    non_null_tracks = sum(1 for _, c in track_offsets if c > 0)
    print(f"wrote {OUT_PATH}: {len(all_zones)} zones across {non_null_tracks} tracks")


if __name__ == "__main__":
    main()
