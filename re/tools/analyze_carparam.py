#!/usr/bin/env python3
"""Analyze carparam.dat across all car directories to identify padding vs data."""
import os
import glob
import struct

CARS_DIR = r"C:\Users\maria\Desktop\Proyectos\TD5RE\re\assets\cars"

def main():
    files = sorted(glob.glob(os.path.join(CARS_DIR, "*", "carparam.dat")))
    print(f"Found {len(files)} carparam.dat files")
    if not files:
        return

    # Load all files
    buffers = []
    names = []
    for fp in files:
        with open(fp, "rb") as f:
            data = f.read()
        if len(data) != 0x10C:
            print(f"WARN: {fp} has size {len(data):#x}, expected 0x10C")
            continue
        names.append(os.path.basename(os.path.dirname(fp)))
        buffers.append(data)

    if not buffers:
        return

    size = len(buffers[0])
    print(f"File size: {size:#x}")

    # For each short offset in tuning table (0x00..0x8B), report values
    def dump_region(start, end, label):
        print(f"\n=== {label} (0x{start:02X}..0x{end-1:02X}) ===")
        print(f"{'off':>4}  {'min':>6}  {'max':>6}  {'uniq':>4}  sample values (first 6 cars hex)")
        for off in range(start, end, 2):
            vals = [struct.unpack_from("<h", b, off)[0] for b in buffers]
            uvals = [struct.unpack_from("<H", b, off)[0] for b in buffers]
            uniq = len(set(vals))
            sample = " ".join(f"{v:04X}" for v in uvals[:6])
            print(f"0x{off:02X}  {min(vals):>6}  {max(vals):>6}  {uniq:>4}  {sample}")

    # Tuning regions per question
    dump_region(0x00, 0x20, "Tuning 0x00-0x1F bbox/envelope")
    dump_region(0x20, 0x40, "Tuning 0x20-0x3F (middle region)")
    dump_region(0x40, 0x60, "Tuning 0x40-0x5F wheel_pos[4]")
    dump_region(0x60, 0x88, "Tuning 0x60-0x87 (extended)")
    dump_region(0x88, 0x8C, "Tuning 0x88-0x8B (coll_mass + unknown_8A)")

    # Also dump last 2 bytes (0x8A) per car
    print("\n=== Per-car Tuning 0x8A values ===")
    for name, b in zip(names, buffers):
        v = struct.unpack_from("<H", b, 0x8A)[0]
        print(f"  {name}: 0x{v:04X}")

    # Also dump 0x20-0x3F as raw dwords
    print("\n=== Per-car Tuning 0x20-0x3F as int32[8] ===")
    for name, b in zip(names, buffers[:5]):
        ints = struct.unpack_from("<8i", b, 0x20)
        print(f"  {name}: " + " ".join(f"{x:>11d}" for x in ints))

if __name__ == "__main__":
    main()
