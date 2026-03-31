#!/usr/bin/env python3
"""
td5_dump_to_png.py - Convert dumped game assets to PNG

Processes the td5_dump/ directory created by the ASI asset dump module.
Converts image files (TGA, texture pages, TEXTURES.DAT) to PNG format.

Supported formats:
  - TGA files (.tga) — standard TGA via Pillow
  - tpage*.dat — raw RGB24/RGBA32 texture pages (dimensions from static.hed)
  - TEXTURES.DAT — paletted 64x64 track textures
  - FORWSKY.TGA / BACKSKY.TGA — sky textures (standard TGA)

Non-image files (wav, dat without image data, prr, etc.) are skipped.

Usage:
  python td5_dump_to_png.py --dump-dir data/td5_dump --output-dir data/td5_png
  python td5_dump_to_png.py --dump-dir data/td5_dump --output-dir data/td5_png --dry-run
"""

import argparse
import os
import struct
import sys

try:
    from PIL import Image
except ImportError:
    print("ERROR: Pillow required. pip install Pillow")
    sys.exit(1)


def convert_tga_to_png(tga_path, png_path):
    """Convert a TGA file to PNG using Pillow."""
    try:
        img = Image.open(tga_path)
        img.save(png_path, "PNG")
        return True
    except Exception as e:
        print(f"  WARNING: failed to convert {tga_path}: {e}")
        return False


def parse_textures_dat(dat_path):
    """
    Parse TEXTURES.DAT — paletted 64x64 track textures.

    File format:
      - First dword: number of texture entries (N)
      - Next N dwords: byte offsets to each texture entry within the file
      - Each texture entry at offset:
          +0x00: 4 bytes header (byte[3] = format_type: 0/3=opaque, 1=keyed, 2=semi)
          +0x04: 4 bytes palette_count (uint32, typically < 256)
          +0x08: palette_count * 3 bytes (RGB palette)
          +0x08+pal_size: 4096 bytes (64x64 pixel indices)

    Yields (index, Image) pairs.
    """
    with open(dat_path, 'rb') as f:
        data = f.read()

    if len(data) < 4:
        return

    num_entries = struct.unpack_from('<I', data, 0)[0]
    if num_entries == 0 or num_entries > 10000:
        return

    # Read offset table (starts at byte 4)
    offsets = []
    for i in range(num_entries):
        table_off = 4 + i * 4
        if table_off + 4 > len(data):
            break
        entry_off = struct.unpack_from('<I', data, table_off)[0]
        offsets.append(entry_off)

    for tex_idx, offset in enumerate(offsets):
        if offset + 8 > len(data):
            continue

        format_type = data[offset + 3]
        palette_count = struct.unpack_from('<I', data, offset + 4)[0]

        if palette_count == 0 or palette_count > 256:
            continue

        pal_start = offset + 8
        pal_size = palette_count * 3
        if pal_start + pal_size + 4096 > len(data):
            continue

        palette = data[pal_start:pal_start + pal_size]
        pixels = data[pal_start + pal_size:pal_start + pal_size + 4096]

        img = Image.new('RGBA', (64, 64))
        img_data = []
        for idx_byte in pixels:
            if idx_byte >= palette_count:
                img_data.append((0, 0, 0, 255))
                continue
            r = palette[idx_byte * 3]
            g = palette[idx_byte * 3 + 1]
            b = palette[idx_byte * 3 + 2]

            if format_type == 0 or format_type == 3:
                a = 255
            elif format_type == 1:
                a = 0 if idx_byte == 0 else 255
            elif format_type == 2:
                a = 128
            else:
                a = 255
            img_data.append((r, g, b, a))

        img.putdata(img_data)
        yield tex_idx, img


def parse_static_hed(hed_path):
    """
    Parse static.hed to get texture page dimensions.
    Returns dict: {page_index: (width, height, has_alpha)}.
    """
    pages = {}
    try:
        with open(hed_path, 'rb') as f:
            data = f.read()

        # Header: first 4 bytes = entry count (or format version)
        if len(data) < 4:
            return pages

        # Each entry is 16 bytes: 4 unknown + 4 unknown + 4 width + 4 height
        # Actually the format varies. Try to extract from known offsets.
        entry_size = 0x10  # 16 bytes per entry
        num_entries = len(data) // entry_size

        for i in range(num_entries):
            off = i * entry_size
            if off + entry_size > len(data):
                break
            w = struct.unpack_from('<I', data, off + 8)[0]
            h = struct.unpack_from('<I', data, off + 12)[0]
            if 0 < w <= 1024 and 0 < h <= 1024:
                has_alpha = (struct.unpack_from('<I', data, off + 4)[0] & 0x1) != 0
                pages[i] = (w, h, has_alpha)
    except Exception:
        pass
    return pages


def convert_tpage_dat(dat_path, width, height, has_alpha, png_path):
    """
    Convert a raw texture page (tpage*.dat) to PNG.
    RGB24 = 3 bytes/pixel, RGBA32 = 4 bytes/pixel.
    """
    try:
        with open(dat_path, 'rb') as f:
            data = f.read()

        expected_rgb = width * height * 3
        expected_rgba = width * height * 4

        if len(data) >= expected_rgba and has_alpha:
            img = Image.frombytes('RGBA', (width, height), data[:expected_rgba])
        elif len(data) >= expected_rgb:
            img = Image.frombytes('RGB', (width, height), data[:expected_rgb])
        else:
            return False

        img.save(png_path, "PNG")
        return True
    except Exception as e:
        print(f"  WARNING: failed to convert tpage {dat_path}: {e}")
        return False


def process_dump_dir(dump_dir, output_dir, dry_run=False):
    """Walk the dump directory and convert all image files to PNG."""
    converted = 0
    skipped = 0
    errors = 0

    for root, dirs, files in os.walk(dump_dir):
        rel_root = os.path.relpath(root, dump_dir)
        out_root = os.path.join(output_dir, rel_root) if rel_root != '.' else output_dir

        # Check for static.hed in this directory (for tpage dimensions)
        hed_path = os.path.join(root, 'static.hed')
        if not os.path.exists(hed_path):
            hed_path = os.path.join(root, 'STATIC.HED')
        tpage_dims = {}
        if os.path.exists(hed_path):
            tpage_dims = parse_static_hed(hed_path)

        for fname in sorted(files):
            fpath = os.path.join(root, fname)
            fname_lower = fname.lower()

            # TGA files → PNG
            if fname_lower.endswith('.tga'):
                png_name = os.path.splitext(fname)[0] + '.png'
                png_path = os.path.join(out_root, png_name)

                if os.path.exists(png_path):
                    skipped += 1
                    continue

                if dry_run:
                    print(f"  [TGA] {os.path.join(rel_root, fname)} -> {png_name}")
                    converted += 1
                    continue

                os.makedirs(out_root, exist_ok=True)
                if convert_tga_to_png(fpath, png_path):
                    converted += 1
                else:
                    errors += 1

            # TEXTURES.DAT → multiple PNGs
            elif fname_lower == 'textures.dat':
                if dry_run:
                    print(f"  [TEXDAT] {os.path.join(rel_root, fname)}")
                    converted += 1
                    continue

                tex_out = os.path.join(out_root, 'textures')
                os.makedirs(tex_out, exist_ok=True)
                for idx, img in parse_textures_dat(fpath):
                    png_path = os.path.join(tex_out, f'tex_{idx:03d}.png')
                    if not os.path.exists(png_path):
                        img.save(png_path, "PNG")
                        converted += 1
                    else:
                        skipped += 1

            # tpage*.dat → PNG (needs dimensions from static.hed)
            elif fname_lower.startswith('tpage') and fname_lower.endswith('.dat'):
                # Extract page index from filename
                try:
                    page_idx = int(fname_lower.replace('tpage', '').replace('.dat', ''))
                except ValueError:
                    continue

                if page_idx not in tpage_dims:
                    # Try default 256x256 RGB24
                    fsize = os.path.getsize(fpath)
                    if fsize == 256 * 256 * 3:
                        w, h, has_alpha = 256, 256, False
                    elif fsize == 256 * 256 * 4:
                        w, h, has_alpha = 256, 256, True
                    elif fsize == 128 * 128 * 3:
                        w, h, has_alpha = 128, 128, False
                    else:
                        continue
                else:
                    w, h, has_alpha = tpage_dims[page_idx]

                png_name = os.path.splitext(fname)[0] + '.png'
                png_path = os.path.join(out_root, png_name)

                if os.path.exists(png_path):
                    skipped += 1
                    continue

                if dry_run:
                    print(f"  [TPAGE] {os.path.join(rel_root, fname)} ({w}x{h} {'RGBA' if has_alpha else 'RGB'})")
                    converted += 1
                    continue

                os.makedirs(out_root, exist_ok=True)
                if convert_tpage_dat(fpath, w, h, has_alpha, png_path):
                    converted += 1
                else:
                    errors += 1

    return converted, skipped, errors


def main():
    parser = argparse.ArgumentParser(
        description="Convert dumped TD5 assets to PNG")
    parser.add_argument('--dump-dir', required=True,
                        help="Path to td5_dump/ directory")
    parser.add_argument('--output-dir', required=True,
                        help="Path to output PNG directory")
    parser.add_argument('--dry-run', action='store_true',
                        help="Show what would be converted without writing")
    args = parser.parse_args()

    dump_dir = os.path.abspath(args.dump_dir)
    output_dir = os.path.abspath(args.output_dir)

    if not os.path.isdir(dump_dir):
        print(f"ERROR: dump directory not found: {dump_dir}")
        sys.exit(1)

    print(f"Source: {dump_dir}")
    print(f"Output: {output_dir}")
    if args.dry_run:
        print("(DRY RUN)")
    print()

    converted, skipped, errors = process_dump_dir(dump_dir, output_dir, args.dry_run)

    print(f"\nResults:")
    print(f"  {converted} files converted")
    print(f"  {skipped} files skipped (already exist)")
    print(f"  {errors} errors")


if __name__ == '__main__':
    main()
