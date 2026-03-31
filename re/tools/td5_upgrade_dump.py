"""
td5_upgrade_dump.py - Upgrade R5G6B5-quantized dump PNGs to full 8-bit color.

The runtime dump captures textures as R5G6B5 (16-bit), losing color precision.
This script cross-references dumped textures with the original extracted assets
(TGA files) to replace the quantized pixels with full 8-bit source colors,
while keeping the CRC-based filename for runtime matching.

For 1:1 textures (sky, individual skins, track textures), the source TGA
provides full color. For composited textures (car skin pages combining
multiple TGAs), the R5G6B5 version is kept as-is.

Usage:
  python td5_upgrade_dump.py --organized-dir data/td5_png/organized --assets-dir data/assets
"""
import argparse
import glob
import os
import re
import struct
import sys
import zlib

try:
    from PIL import Image
except ImportError:
    print("ERROR: Pillow required. pip install Pillow")
    sys.exit(1)


def rgba_to_r5g6b5(r, g, b):
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def image_to_r5g6b5_crc(img):
    """Compute the R5G6B5 CRC32 that the runtime would produce for this image."""
    w, h = img.size
    pixels = img.convert('RGB').load()
    buf = bytearray(w * h * 2)
    idx = 0
    for y in range(h):
        for x in range(w):
            r, g, b = pixels[x, y]
            val = rgba_to_r5g6b5(r, g, b)
            buf[idx] = val & 0xFF
            buf[idx + 1] = (val >> 8) & 0xFF
            idx += 2
    return zlib.crc32(bytes(buf)) & 0xFFFFFFFF


def parse_tga_to_image(tga_path):
    """Load a TGA file via Pillow (handles all TGA variants)."""
    try:
        return Image.open(tga_path).convert('RGBA')
    except Exception:
        return None


def collect_source_tgas(assets_dir):
    """Build a dict of all source TGA files: {r5g6b5_crc: (path, Image)}."""
    sources = {}
    tga_patterns = [
        'levels/*/forwsky.tga', 'levels/*/FORWSKY.TGA',
        'levels/*/backsky.tga', 'levels/*/BACKSKY.TGA',
        'traffic/skin*.tga', 'traffic/SKIN*.TGA',
        'environs/*.tga', 'environs/*.TGA',
        'loading/*.tga', 'loading/*.TGA',
        'legals/*.tga', 'legals/*.TGA',
        'cars/*/*.tga', 'cars/*/*.TGA',
        'frontend/*.tga', 'frontend/*.TGA',
        'extras/*.tga', 'extras/*.TGA',
        'mugshots/*.tga', 'mugshots/*.TGA',
        'tracks/*.tga', 'tracks/*.TGA',
    ]

    for pattern in tga_patterns:
        for tga_path in glob.glob(os.path.join(assets_dir, pattern)):
            img = parse_tga_to_image(tga_path)
            if img is None:
                continue
            crc = image_to_r5g6b5_crc(img)
            rel = os.path.relpath(tga_path, assets_dir).replace('\\', '/')
            sources[crc] = (rel, img)

    return sources


def parse_textures_dat_to_images(dat_path):
    """Parse TEXTURES.DAT and yield (r5g6b5_crc, Image) pairs."""
    with open(dat_path, 'rb') as f:
        data = f.read()

    offset = 0
    while offset + 8 < len(data):
        if offset + 4 > len(data):
            break
        format_type = data[offset + 3]
        offset += 4

        if offset + 4 > len(data):
            break
        palette_count = struct.unpack_from('<I', data, offset)[0]
        offset += 4

        pal_size = palette_count * 3
        if offset + pal_size > len(data):
            break
        palette = data[offset:offset + pal_size]
        offset += pal_size

        if offset + 4096 > len(data):
            break
        pixels = data[offset:offset + 4096]
        offset += 4096

        # Expand to RGBA with format-specific alpha
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
        crc = image_to_r5g6b5_crc(img)
        yield crc, img


def main():
    parser = argparse.ArgumentParser(
        description="Upgrade R5G6B5-quantized dump PNGs to full 8-bit color")
    parser.add_argument('--organized-dir', required=True,
                        help="Path to organized PNG directory")
    parser.add_argument('--assets-dir', required=True,
                        help="Path to extracted assets/ directory")
    parser.add_argument('--dry-run', action='store_true')
    args = parser.parse_args()

    organized_dir = os.path.abspath(args.organized_dir)
    assets_dir = os.path.abspath(args.assets_dir)

    # Build CRC → source image mapping from all TGA files
    print("Scanning source TGA files...")
    sources = collect_source_tgas(assets_dir)
    print(f"  {len(sources)} unique source TGAs indexed by R5G6B5 CRC")

    # Also process TEXTURES.DAT files
    print("Scanning TEXTURES.DAT files...")
    tex_count = 0
    for dat_path in sorted(glob.glob(os.path.join(assets_dir, 'levels', 'level*', 'textures.dat')) +
                           glob.glob(os.path.join(assets_dir, 'levels', 'level*', 'TEXTURES.DAT'))):
        for crc, img in parse_textures_dat_to_images(dat_path):
            if crc not in sources:
                rel = os.path.relpath(dat_path, assets_dir).replace('\\', '/')
                sources[crc] = (rel, img)
                tex_count += 1
    print(f"  {tex_count} additional textures from TEXTURES.DAT")

    print(f"Total source images: {len(sources)}")

    # Scan organized PNGs and upgrade where possible
    upgraded = 0
    skipped = 0
    not_found = 0

    for png_path in sorted(glob.glob(os.path.join(organized_dir, '**', '*.png'), recursive=True)):
        filename = os.path.basename(png_path)
        m = re.match(r'^(\d+)x(\d+)_([0-9A-Fa-f]{8})(?:_.*)?\.png$', filename)
        if not m:
            continue

        w, h, crc = int(m.group(1)), int(m.group(2)), int(m.group(3), 16)

        if crc in sources:
            source_rel, source_img = sources[crc]
            sw, sh = source_img.size
            if sw == w and sh == h:
                if not args.dry_run:
                    source_img.save(png_path)
                upgraded += 1
            else:
                skipped += 1
        else:
            not_found += 1

    print(f"\nResults:")
    print(f"  {upgraded} PNGs upgraded to full 8-bit color")
    print(f"  {not_found} PNGs kept as R5G6B5 (composited, no source match)")
    print(f"  {skipped} PNGs skipped (dimension mismatch)")


if __name__ == '__main__':
    main()
