#!/usr/bin/env python3
"""
extract_td6_lightzones.py — dump TD6's per-track AMBIENT + DIRECTIONAL lighting
zones out of TD6.exe into a per-level LIGHTZONES.BIN the source port can load.

Background (RE'd 2026-06-19, ghidra_td6/TD6):
  TD6 applies a per-area scene light (ambient + ONE directional "sun") chosen by
  the player's span. Each TRACK SELECTION has its own zone table, compiled into
  TD6.exe (NOT in the level zip). Resolution chain, all driven by the menu/career
  selection index `sel` (DAT_0065c484):

    track_table @0x0049b5f8 : 8-byte entries { u32 file_num, u32 direction }   (sel -> file/dir)
    setter_array @0x0049b9b8 : u32 stub VA per sel
    stub (C7 05 <&DAT_005e7bf0> <descriptor VA> C3) : descriptor VA at stub+6
    descriptor + 0x10        : zone-table VA
    zone table               : 36-byte records, terminated by a record whose
                               end_span >= 9999 (0x270F)

  Zone record (0x28 = 40 bytes), little-endian:
    0x00 s16 start_span
    0x02 s16 end_span
    0x04 s16 dir_x   (/4096 -> float)
    0x06 s16 dir_y
    0x08 s16 dir_z
    0x0C u8  rgbA (grey)  -> directional sun intensity = rgbA / 128.0
    0x10 u8  rgbB (grey)  -> scene ambient            = rgbB / 256.0
    0x18 u8  type (0=solid, 1/2=gradient, 3=fade)   [informational only]

  Validated: file0/sel0 table @0x00487b28 == the (mislabelled) "k_td6_london_zones"
  shipped in td5_render.c; it is actually PARIS. London (file4) is @0x0048b330.

Output (per converted level dir re/assets/levels/levelNNN/LIGHTZONES.BIN):
    magic  'TD6Z'                       (4)
    u32    version = 1
    u16    fwd_count
    u16    bwd_count
    fwd_count records, then bwd_count records, each 24 bytes:
        s16 start, s16 end, f32 dir_x, f32 dir_y, f32 dir_z, f32 dir_i, f32 amb
    (byte-identical to the C `TD6LightZone` struct on x86 LE)

Usage:
    python re/tools/extract_td6_lightzones.py [--exe "Test Drive 6/TD6.exe"]
"""
import argparse, os, struct, sys

IMAGE_BASE      = 0x00400000
TRACK_TABLE_VA  = 0x0049b5f8   # { u32 file, u32 dir } * sel
SETTER_ARRAY_VA = 0x0049b9b8   # u32 stub VA * sel
ZONE_REC_SIZE   = 0x28         # 40 bytes per record (matches TD6_StripPostLoadFixup stride)
TERMINATOR      = 9999         # end_span >= this ends the table

# TD6 file level number -> converted TD5RE level dir number (convert_td6_tracks.py convention)
FILE_TO_LEVEL = {0: 8, 1: 9, 2: 10, 3: 11, 4: 12,
                 10: 7, 11: 18, 15: 19, 16: 20, 17: 21, 18: 22}
CITY_NAME = {0: "Paris", 1: "New York", 2: "Rome", 3: "Hong Kong", 4: "London",
             10: "Pelton", 11: "Ireland", 15: "Tahoe", 16: "Cape Hatteras",
             17: "Switzerland", 18: "Egypt"}

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
ASSETS_LEVELS = os.path.join(ROOT, "re", "assets", "levels")


class PE:
    """Minimal PE reader: map virtual address -> raw file bytes."""
    def __init__(self, data):
        self.data = data
        e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
        assert data[e_lfanew:e_lfanew + 4] == b"PE\0\0", "not a PE"
        nsect = struct.unpack_from("<H", data, e_lfanew + 6)[0]
        opt_sz = struct.unpack_from("<H", data, e_lfanew + 20)[0]
        sect_off = e_lfanew + 24 + opt_sz
        self.sections = []
        for i in range(nsect):
            o = sect_off + i * 40
            vsize, vaddr, rawsz, rawptr = struct.unpack_from("<IIII", data, o + 8)
            self.sections.append((vaddr, max(vsize, rawsz), rawptr, rawsz))

    def read(self, va, n):
        rva = va - IMAGE_BASE
        for vaddr, vspan, rawptr, rawsz in self.sections:
            if vaddr <= rva < vaddr + vspan:
                off = rawptr + (rva - vaddr)
                return self.data[off:off + n]
        raise ValueError(f"VA {va:#010x} not in any section")

    def u32(self, va):
        return struct.unpack("<I", self.read(va, 4))[0]


def zone_table_for_sel(pe, sel):
    """Resolve sel -> (file, dir, zone_table_VA) or None if invalid."""
    fb = pe.read(TRACK_TABLE_VA + sel * 8, 8)
    file_num, direction = struct.unpack("<II", fb)
    if direction > 1 or file_num > 60:
        return None
    stub_va = pe.u32(SETTER_ARRAY_VA + sel * 4)
    if not (0x00410000 <= stub_va < 0x00470000):
        return None
    stub = pe.read(stub_va, 11)
    if stub[0] != 0xC7 or stub[1] != 0x05:            # mov dword [imm], imm32
        return None
    descriptor_va = struct.unpack_from("<I", stub, 6)[0]
    zone_va = pe.u32(descriptor_va + 0x10)
    return file_num, direction, zone_va


def decode_zone_table(pe, va):
    """Walk 36-byte records to the 9999 terminator -> list of TD6LightZone tuples."""
    zones = []
    off = va
    for _ in range(256):  # safety cap
        rec = pe.read(off, ZONE_REC_SIZE)
        start, end, dx, dy, dz = struct.unpack_from("<hhhhh", rec, 0)
        if end >= TERMINATOR or start >= TERMINATOR:
            break
        rgbA = rec[0x0C]
        rgbB = rec[0x10]
        zones.append((start, end,
                      dx / 4096.0, dy / 4096.0, dz / 4096.0,
                      rgbA / 128.0, rgbB / 256.0))
        off += ZONE_REC_SIZE
    return zones


def pack_bin(fwd, bwd):
    out = bytearray()
    out += b"TD6Z" + struct.pack("<I", 1)
    out += struct.pack("<HH", len(fwd), len(bwd))
    for z in fwd:
        out += struct.pack("<hh5f", *z)
    for z in bwd:
        out += struct.pack("<hh5f", *z)
    return bytes(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=os.path.join(ROOT, "Test Drive 6", "TD6.exe"))
    ap.add_argument("--dry-run", action="store_true",
                    help="decode + print summary, do not write files")
    args = ap.parse_args()

    with open(args.exe, "rb") as f:
        pe = PE(f.read())

    # Gather fwd/bwd table per file_num across all selections.
    per_file = {}   # file_num -> {0: zones_fwd, 1: zones_bwd}
    for sel in range(80):
        res = zone_table_for_sel(pe, sel)
        if res is None:
            continue
        file_num, direction, zone_va = res
        zones = decode_zone_table(pe, zone_va)
        if not zones:
            continue
        per_file.setdefault(file_num, {})[direction] = (zone_va, zones)

    if not per_file:
        print("ERROR: no zone tables resolved — wrong exe?", file=sys.stderr)
        sys.exit(1)

    wrote = 0
    for file_num in sorted(per_file):
        dirs = per_file[file_num]
        fwd_va, fwd = dirs.get(0, (0, []))
        bwd_va, bwd = dirs.get(1, (0, []))
        name = CITY_NAME.get(file_num, f"file{file_num}")
        lvl = FILE_TO_LEVEL.get(file_num)
        fwd_max = max((z[1] for z in fwd), default=0)
        print(f"file{file_num:03d} {name:<14} fwd={len(fwd):2d} zones (@{fwd_va:#010x}, "
              f"max span {fwd_max})  bwd={len(bwd):2d}  -> "
              f"{'level%03d' % lvl if lvl is not None else 'UNMAPPED (skip)'}")
        if lvl is None or args.dry_run:
            continue
        od = os.path.join(ASSETS_LEVELS, f"level{lvl:03d}")
        if not os.path.isdir(od):
            print(f"    WARN: {od} missing (level not converted) — skipping")
            continue
        with open(os.path.join(od, "LIGHTZONES.BIN"), "wb") as f:
            f.write(pack_bin(fwd, bwd))
        wrote += 1

    print(f"\nwrote {wrote} LIGHTZONES.BIN file(s)")


if __name__ == "__main__":
    main()
