"""
td5_offline_dump.py - Generate R5G6B5 texture dumps offline from extracted assets.

Simulates the game's texture loading pipeline: reads TGA/DAT files, converts
to R5G6B5 (same as the D3D wrapper's 16-bit surfaces), computes CRC32, and
saves PNGs in the same WxH_CRC.png format as the runtime dump.

This covers all 1:1 textures (sky, track textures, tpages, environs, loading
screens). Composited textures (car skin pages, traffic pages) require the
runtime dump since their layout depends on M2DX's compositing logic.

Output goes to the same _dump/ directory used by the runtime dump, so both
sources merge seamlessly.

Usage:
  python td5_offline_dump.py --assets-dir data/assets --dump-dir data/td5_png/_dump
"""
import argparse
import glob
import os
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


def image_to_r5g6b5_bytes(img):
    """Convert PIL Image to R5G6B5 byte buffer (little-endian)."""
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
    return bytes(buf)


def r5g6b5_to_rgb_image(r5g6b5_bytes, w, h):
    """Convert R5G6B5 buffer back to RGB PIL Image (for PNG output)."""
    img = Image.new('RGB', (w, h))
    pixels = img.load()
    for y in range(h):
        for x in range(w):
            idx = (y * w + x) * 2
            val = r5g6b5_bytes[idx] | (r5g6b5_bytes[idx + 1] << 8)
            r = ((val >> 11) & 0x1F) * 255 // 31
            g = ((val >> 5) & 0x3F) * 255 // 63
            b = (val & 0x1F) * 255 // 31
            pixels[x, y] = (r, g, b)
    return img


def process_tga(tga_path, dump_dir, manifest, existing_crcs):
    """Process a single TGA file: decode, convert to R5G6B5, dump."""
    try:
        img = Image.open(tga_path)
    except Exception:
        return False

    w, h = img.size
    r5g6b5 = image_to_r5g6b5_bytes(img)
    crc = zlib.crc32(r5g6b5) & 0xFFFFFFFF

    if crc in existing_crcs:
        return False

    filename = f"{w}x{h}_{crc:08X}.png"
    out_path = os.path.join(dump_dir, filename)

    if os.path.exists(out_path):
        existing_crcs.add(crc)
        return False

    # Save as R5G6B5-quantized RGB PNG (matches runtime dump format)
    out_img = r5g6b5_to_rgb_image(r5g6b5, w, h)
    out_img.save(out_path)
    existing_crcs.add(crc)

    rel_path = out_path.replace('\\', '/')
    manifest.append(f"{crc:08X} {w:4d} {h:4d} {rel_path}")
    return True


def process_textures_dat(dat_path, dump_dir, manifest, existing_crcs):
    """Process TEXTURES.DAT: extract palettized textures, convert to R5G6B5."""
    with open(dat_path, 'rb') as f:
        data = f.read()

    offset = 0
    count = 0
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

        # Expand to RGB
        img = Image.new('RGB', (64, 64))
        img_data = []
        for idx_byte in pixels:
            if idx_byte >= palette_count:
                img_data.append((0, 0, 0))
                continue
            r = palette[idx_byte * 3]
            g = palette[idx_byte * 3 + 1]
            b = palette[idx_byte * 3 + 2]
            img_data.append((r, g, b))
        img.putdata(img_data)

        # Convert to R5G6B5 and dump
        w, h = 64, 64
        r5g6b5 = image_to_r5g6b5_bytes(img)
        crc = zlib.crc32(r5g6b5) & 0xFFFFFFFF

        if crc not in existing_crcs:
            filename = f"{w}x{h}_{crc:08X}.png"
            out_path = os.path.join(dump_dir, filename)
            if not os.path.exists(out_path):
                out_img = r5g6b5_to_rgb_image(r5g6b5, w, h)
                out_img.save(out_path)
                rel_path = out_path.replace('\\', '/')
                manifest.append(f"{crc:08X} {w:4d} {h:4d} {rel_path}")
                count += 1
            existing_crcs.add(crc)

    return count


def process_tpage(tpage_path, meta, dump_dir, manifest, existing_crcs):
    """Process a tpage*.dat file: read raw pixels, convert to R5G6B5."""
    with open(tpage_path, 'rb') as f:
        data = f.read()

    trans_flag = meta.get('transparency_flag', 0)
    src_dim = meta.get('source_dimension', 256)
    if src_dim <= 0:
        src_dim = 256

    if trans_flag != 0:
        bpp = 4  # ARGB
        expected = src_dim * src_dim * bpp
        if len(data) < expected:
            return False
        img = Image.new('RGB', (src_dim, src_dim))
        px = []
        for i in range(0, expected, 4):
            px.append((data[i + 1], data[i + 2], data[i + 3]))  # skip alpha
        img.putdata(px)
    else:
        bpp = 3  # RGB
        expected = src_dim * src_dim * bpp
        if len(data) < expected:
            return False
        img = Image.new('RGB', (src_dim, src_dim))
        px = []
        for i in range(0, expected, 3):
            px.append((data[i], data[i + 1], data[i + 2]))
        img.putdata(px)

    w, h = img.size
    r5g6b5 = image_to_r5g6b5_bytes(img)
    crc = zlib.crc32(r5g6b5) & 0xFFFFFFFF

    if crc in existing_crcs:
        return False

    filename = f"{w}x{h}_{crc:08X}.png"
    out_path = os.path.join(dump_dir, filename)
    if os.path.exists(out_path):
        existing_crcs.add(crc)
        return False

    out_img = r5g6b5_to_rgb_image(r5g6b5, w, h)
    out_img.save(out_path)
    existing_crcs.add(crc)
    rel_path = out_path.replace('\\', '/')
    manifest.append(f"{crc:08X} {w:4d} {h:4d} {rel_path}")
    return True


def parse_static_hed(data):
    """Parse static.hed for tpage metadata."""
    if len(data) < 8:
        return {}
    page_count, named_count = struct.unpack_from('<II', data, 0)
    offset = 8 + named_count * 64
    pages = {}
    for i in range(page_count):
        if offset + 16 > len(data):
            break
        trans_flag, img_type, src_dim, tgt_dim = struct.unpack_from('<IIII', data, offset)
        pages[i] = {
            'transparency_flag': trans_flag,
            'image_type': img_type,
            'source_dimension': src_dim,
            'target_dimension': tgt_dim,
        }
        offset += 16
    return pages


def main():
    parser = argparse.ArgumentParser(
        description="Generate R5G6B5 texture dumps offline from extracted assets")
    parser.add_argument('--assets-dir', required=True)
    parser.add_argument('--dump-dir', required=True)
    args = parser.parse_args()

    assets_dir = os.path.abspath(args.assets_dir)
    dump_dir = os.path.abspath(args.dump_dir)
    os.makedirs(dump_dir, exist_ok=True)

    # Load existing CRCs from dump directory (skip already-dumped textures)
    existing_crcs = set()
    import re
    for f in os.listdir(dump_dir):
        m = re.match(r'^\d+x\d+_([0-9A-Fa-f]{8})\.png$', f)
        if m:
            existing_crcs.add(int(m.group(1), 16))
    print(f"Existing dumps: {len(existing_crcs)}")

    manifest = []
    total = 0

    # 1. TEXTURES.DAT from each level
    print("\nProcessing TEXTURES.DAT files...")
    for level_dir in sorted(glob.glob(os.path.join(assets_dir, 'levels', 'level*'))):
        for name in ['textures.dat', 'TEXTURES.DAT']:
            path = os.path.join(level_dir, name)
            if os.path.exists(path):
                n = process_textures_dat(path, dump_dir, manifest, existing_crcs)
                if n > 0:
                    print(f"  {os.path.basename(level_dir)}/textures.dat: {n} new")
                    total += n
                break

    # 2. tpage*.dat from static/ and levels/
    print("\nProcessing tpage*.dat files...")
    hed_path = None
    for name in ['static.hed', 'STATIC.HED']:
        p = os.path.join(assets_dir, 'static', name)
        if os.path.exists(p):
            hed_path = p
            break
    pages_meta = {}
    if hed_path:
        with open(hed_path, 'rb') as f:
            pages_meta = parse_static_hed(f.read())

    for tpage in sorted(glob.glob(os.path.join(assets_dir, 'static', 'tpage*.dat'))):
        idx_str = os.path.basename(tpage).lower().replace('tpage', '').replace('.dat', '')
        try:
            idx = int(idx_str)
        except ValueError:
            continue
        meta = pages_meta.get(idx, {'transparency_flag': 0, 'source_dimension': 256})
        if process_tpage(tpage, meta, dump_dir, manifest, existing_crcs):
            print(f"  static/tpage{idx}.dat: new")
            total += 1

    # 3. TGA files from levels (sky), environs, loading, legals
    print("\nProcessing TGA files...")
    tga_dirs = [
        ('levels', 'levels/*/'),
        ('environs', 'environs/'),
        ('loading', 'loading/'),
        ('legals', 'legals/'),
    ]
    for label, pattern in tga_dirs:
        count = 0
        for tga in sorted(glob.glob(os.path.join(assets_dir, pattern, '*.tga')) +
                          glob.glob(os.path.join(assets_dir, pattern, '*.TGA'))):
            if process_tga(tga, dump_dir, manifest, existing_crcs):
                count += 1
                total += 1
        if count > 0:
            print(f"  {label}: {count} new TGAs")

    # Write manifest entries (append to existing)
    manifest_path = os.path.join(dump_dir, 'manifest.txt')
    if manifest:
        with open(manifest_path, 'a') as f:
            for line in manifest:
                f.write(line + '\n')

    print(f"\nSummary: {total} new textures dumped offline")
    print(f"Total in dump directory: {len(existing_crcs)}")
    print(f"Run td5_build_index.py to rebuild index.dat")


if __name__ == '__main__':
    main()
