"""
td5_texture_converter.py - Convert TD5 textures (TEXTURES.DAT, tpage*.dat, TGA) to PNG.

Reads from the extracted assets/ tree (produced by td5_asset_extractor.py) and
writes PNGs with proper 8-bit alpha to td5_png/. Generates index.dat for the
ddraw wrapper's PNG loader.

Texture categories and alpha rules:
  TEXTURES.DAT (levels/) - 64x64 paletted
    format_type 0/3: fully opaque (A=255)
    format_type 1:   palette index 0 -> transparent (A=0), rest opaque (A=255)
    format_type 2:   semi-transparent (A=128)

  tpage%d.dat (static/ + levels/) - raw pixel data
    RGB24 (transparency_flag=0): opaque (A=255)
    RGBA32 (transparency_flag!=0): preserve existing alpha (ARGB byte order)
    Requires static.hed for metadata lookup

  TGA files (cars/, levels/, frontend/) - standard TGA
    Default: opaque (A=255)
    CARHUB*.TGA: black pixels -> transparent (A=0)

  Traffic skins (traffic/) - TGA, fully opaque (A=255)

Usage:
  python td5_texture_converter.py --assets-dir data/assets --output-dir data/td5_png
  python td5_texture_converter.py --assets-dir data/assets --output-dir data/td5_png --dry-run
  python td5_texture_converter.py --assets-dir data/assets --output-dir data/td5_png --update-index

Requires: Python 3, Pillow (pip install Pillow)
"""
import argparse
import glob
import json
import os
import struct
import sys
import zlib

try:
    from PIL import Image
except ImportError:
    print("ERROR: Pillow is required. Install with: pip install Pillow")
    sys.exit(1)


# =============================================================================
# TEXTURES.DAT Parser
# =============================================================================

def parse_textures_dat(data):
    """Parse TEXTURES.DAT and yield (index, format_type, Image) tuples."""
    offset = 0
    tex_idx = 0

    while offset + 8 < len(data):
        # 3 bytes padding + 1 byte format_type
        if offset + 4 > len(data):
            break
        format_type = data[offset + 3]
        offset += 4

        # 4-byte palette count
        if offset + 4 > len(data):
            break
        palette_count = struct.unpack_from('<I', data, offset)[0]
        offset += 4

        # Palette: N * 3 bytes (RGB)
        pal_size = palette_count * 3
        if offset + pal_size > len(data):
            break
        palette = data[offset:offset + pal_size]
        offset += pal_size

        # Pixel data: 64x64 = 4096 palette indices
        if offset + 4096 > len(data):
            break
        pixels = data[offset:offset + 4096]
        offset += 4096

        # Expand to RGBA
        img = Image.new('RGBA', (64, 64))
        img_data = []
        for idx_byte in pixels:
            if idx_byte >= palette_count:
                img_data.append((0, 0, 0, 0))
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
        yield (tex_idx, format_type, img)
        tex_idx += 1


# =============================================================================
# static.hed Parser (texture page metadata)
# =============================================================================

def parse_static_hed(data):
    """Parse static.hed and return dict of {page_index: metadata}."""
    if len(data) < 8:
        return {}

    page_count, named_count = struct.unpack_from('<II', data, 0)
    offset = 8

    # Skip named entries (64 bytes each)
    offset += named_count * 64

    # Read per-page metadata (16 bytes each)
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


# =============================================================================
# tpage*.dat Parser
# =============================================================================

def parse_tpage(data, metadata):
    """Parse a tpage*.dat file into an RGBA Image using metadata from static.hed."""
    trans_flag = metadata.get('transparency_flag', 0)
    src_dim = metadata.get('source_dimension', 256)

    if src_dim <= 0:
        src_dim = 256

    if trans_flag != 0:
        # RGBA32: A R G B byte order per pixel
        bpp = 4
        expected = src_dim * src_dim * bpp
        if len(data) < expected:
            return None
        img = Image.new('RGBA', (src_dim, src_dim))
        px = []
        for i in range(0, expected, 4):
            a = data[i]
            r = data[i + 1]
            g = data[i + 2]
            b = data[i + 3]
            px.append((r, g, b, a))
        img.putdata(px)
    else:
        # RGB24: R G B byte order per pixel
        bpp = 3
        expected = src_dim * src_dim * bpp
        if len(data) < expected:
            return None
        img = Image.new('RGBA', (src_dim, src_dim))
        px = []
        for i in range(0, expected, 3):
            r = data[i]
            g = data[i + 1]
            b = data[i + 2]
            px.append((r, g, b, 255))
        img.putdata(px)

    return img


# =============================================================================
# TGA Parser
# =============================================================================

def parse_tga(data):
    """Parse a TGA file into an RGBA Image."""
    if len(data) < 18:
        return None

    id_len = data[0]
    cmap_type = data[1]
    img_type = data[2]
    cmap_origin = struct.unpack_from('<H', data, 3)[0]
    cmap_length = struct.unpack_from('<H', data, 5)[0]
    cmap_depth = data[7]
    width = struct.unpack_from('<H', data, 12)[0]
    height = struct.unpack_from('<H', data, 14)[0]
    pixel_depth = data[16]
    descriptor = data[17]

    if width == 0 or height == 0:
        return None

    offset = 18 + id_len

    # Read color map if present
    palette = None
    if cmap_type == 1 and cmap_length > 0:
        cmap_bpp = cmap_depth // 8
        pal_data = data[offset:offset + cmap_length * cmap_bpp]
        offset += cmap_length * cmap_bpp
        palette = []
        for i in range(cmap_length):
            if cmap_bpp == 3:
                b, g, r = pal_data[i*3], pal_data[i*3+1], pal_data[i*3+2]
                palette.append((r, g, b, 255))
            elif cmap_bpp == 4:
                b, g, r, a = pal_data[i*4], pal_data[i*4+1], pal_data[i*4+2], pal_data[i*4+3]
                palette.append((r, g, b, a))
            elif cmap_bpp == 2:
                val = struct.unpack_from('<H', pal_data, i*2)[0]
                r = ((val >> 10) & 0x1F) * 255 // 31
                g = ((val >> 5) & 0x1F) * 255 // 31
                b = (val & 0x1F) * 255 // 31
                palette.append((r, g, b, 255))

    pixels = []

    if img_type in (1, 2):  # Uncompressed
        bpp = pixel_depth // 8
        for y in range(height):
            for x in range(width):
                if offset + bpp > len(data):
                    pixels.append((0, 0, 0, 0))
                    continue

                if img_type == 1 and palette:
                    idx = data[offset]
                    offset += 1
                    if idx < len(palette):
                        pixels.append(palette[idx])
                    else:
                        pixels.append((0, 0, 0, 0))
                elif bpp == 2:
                    val = struct.unpack_from('<H', data, offset)[0]
                    offset += 2
                    r = ((val >> 10) & 0x1F) * 255 // 31
                    g = ((val >> 5) & 0x1F) * 255 // 31
                    b = (val & 0x1F) * 255 // 31
                    a = 255 if (val & 0x8000) else 255  # 1-bit alpha unused in TD5
                    pixels.append((r, g, b, a))
                elif bpp == 3:
                    b, g, r = data[offset], data[offset+1], data[offset+2]
                    offset += 3
                    pixels.append((r, g, b, 255))
                elif bpp == 4:
                    b, g, r, a = data[offset], data[offset+1], data[offset+2], data[offset+3]
                    offset += 4
                    pixels.append((r, g, b, a))
                else:
                    offset += bpp
                    pixels.append((0, 0, 0, 255))

    elif img_type in (9, 10):  # RLE compressed
        bpp = pixel_depth // 8
        total = width * height
        while len(pixels) < total and offset < len(data):
            packet = data[offset]
            offset += 1
            count = (packet & 0x7F) + 1

            if packet & 0x80:  # RLE packet
                if img_type == 9 and palette:
                    idx = data[offset]; offset += 1
                    color = palette[idx] if idx < len(palette) else (0, 0, 0, 0)
                elif bpp == 3:
                    b, g, r = data[offset], data[offset+1], data[offset+2]
                    offset += 3
                    color = (r, g, b, 255)
                elif bpp == 4:
                    b, g, r, a = data[offset], data[offset+1], data[offset+2], data[offset+3]
                    offset += 4
                    color = (r, g, b, a)
                else:
                    offset += bpp
                    color = (0, 0, 0, 255)
                pixels.extend([color] * count)
            else:  # Raw packet
                for _ in range(count):
                    if offset >= len(data):
                        pixels.append((0, 0, 0, 0))
                        continue
                    if img_type == 9 and palette:
                        idx = data[offset]; offset += 1
                        pixels.append(palette[idx] if idx < len(palette) else (0, 0, 0, 0))
                    elif bpp == 3:
                        b, g, r = data[offset], data[offset+1], data[offset+2]
                        offset += 3
                        pixels.append((r, g, b, 255))
                    elif bpp == 4:
                        b, g, r, a = data[offset], data[offset+1], data[offset+2], data[offset+3]
                        offset += 4
                        pixels.append((r, g, b, a))
                    else:
                        offset += bpp
                        pixels.append((0, 0, 0, 255))
    else:
        return None

    if len(pixels) < width * height:
        pixels.extend([(0, 0, 0, 0)] * (width * height - len(pixels)))

    img = Image.new('RGBA', (width, height))
    img.putdata(pixels[:width * height])

    # Handle orientation
    top_to_bottom = (descriptor >> 5) & 1
    right_to_left = (descriptor >> 4) & 1
    if not top_to_bottom:
        img = img.transpose(Image.FLIP_TOP_BOTTOM)
    if right_to_left:
        img = img.transpose(Image.FLIP_LEFT_RIGHT)

    return img


# =============================================================================
# Alpha Rules
# =============================================================================

def apply_carhub_alpha(img):
    """CARHUB textures: black pixels become transparent."""
    px = img.load()
    w, h = img.size
    for y in range(h):
        for x in range(w):
            r, g, b, a = px[x, y]
            if r == 0 and g == 0 and b == 0:
                px[x, y] = (0, 0, 0, 0)
    return img


# =============================================================================
# Index.dat Generator (for ddraw wrapper PNG loader)
# =============================================================================

def rgba_to_r5g6b5(r, g, b):
    """Convert RGB888 to R5G6B5 (16-bit, little-endian)."""
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def compute_r5g6b5_crc32(img):
    """Compute CRC32 of the full texture converted to R5G6B5 format.

    The ddraw wrapper sees textures as R5G6B5 surfaces. To match at runtime,
    the index CRC must be computed from the same R5G6B5 representation.
    Uses the full image (not just first scanline) to avoid collisions.
    """
    w, h = img.size
    pixels = img.load()
    buf = bytearray(w * h * 2)
    idx = 0
    for y in range(h):
        for x in range(w):
            px = pixels[x, y]
            val = rgba_to_r5g6b5(px[0], px[1], px[2])
            buf[idx] = val & 0xFF
            buf[idx + 1] = (val >> 8) & 0xFF
            idx += 2
    return zlib.crc32(bytes(buf)) & 0xFFFFFFFF


def write_index_dat(entries, output_path):
    """Write index.dat for the PNG loader.

    Format:
      "TD5PNG\x01\x00" (8 bytes: magic + version)
      uint32 entry_count
      per entry:
        uint32 width, height, first_row_crc32
        uint16 path_length
        char path[] (null-terminated, relative to CWD)
    """
    with open(output_path, 'wb') as f:
        f.write(b'TD5PNG\x01\x00')
        f.write(struct.pack('<I', len(entries)))
        for entry in entries:
            f.write(struct.pack('<II', entry['width'], entry['height']))
            f.write(struct.pack('<I', entry['crc32']))
            path_bytes = entry['path'].encode('utf-8') + b'\x00'
            f.write(struct.pack('<H', len(path_bytes)))
            f.write(path_bytes)


# =============================================================================
# Conversion Pipeline
# =============================================================================

def convert_textures_dat(assets_dir, output_dir, dry_run=False):
    """Convert all TEXTURES.DAT files from level directories."""
    entries = []
    level_dirs = sorted(glob.glob(os.path.join(assets_dir, 'levels', 'level*')))

    for level_dir in level_dirs:
        tex_path = os.path.join(level_dir, 'textures.dat')
        if not os.path.exists(tex_path):
            # Try uppercase
            tex_path = os.path.join(level_dir, 'TEXTURES.DAT')
            if not os.path.exists(tex_path):
                continue

        level_name = os.path.basename(level_dir)
        out_subdir = os.path.join(output_dir, 'levels', level_name)

        with open(tex_path, 'rb') as f:
            data = f.read()

        count = 0
        for tex_idx, fmt_type, img in parse_textures_dat(data):
            out_file = os.path.join(out_subdir, f'texture_{tex_idx:03d}.png')
            rel_path = os.path.relpath(out_file, os.path.dirname(output_dir))
            rel_path = rel_path.replace('\\', '/')

            if not dry_run:
                os.makedirs(out_subdir, exist_ok=True)
                img.save(out_file)

            entries.append({
                'width': img.size[0],
                'height': img.size[1],
                'crc32': compute_r5g6b5_crc32(img),
                'path': rel_path,
                'source': f'{level_name}/textures.dat#{tex_idx}',
            })
            count += 1

        action = "would convert" if dry_run else "converted"
        print(f"  {level_name}/textures.dat: {count} textures {action}")

    return entries


def convert_tpages(assets_dir, output_dir, dry_run=False):
    """Convert tpage*.dat files using static.hed metadata."""
    entries = []

    # Parse static.hed for metadata
    hed_path = os.path.join(assets_dir, 'static', 'static.hed')
    if not os.path.exists(hed_path):
        hed_path = os.path.join(assets_dir, 'static', 'STATIC.HED')
    pages_meta = {}
    if os.path.exists(hed_path):
        with open(hed_path, 'rb') as f:
            pages_meta = parse_static_hed(f.read())

    # Convert static/ tpages
    for tpage_file in sorted(glob.glob(os.path.join(assets_dir, 'static', 'tpage*.dat'))):
        basename = os.path.basename(tpage_file).lower()
        # Extract page index from filename
        idx_str = basename.replace('tpage', '').replace('.dat', '')
        try:
            page_idx = int(idx_str)
        except ValueError:
            continue

        meta = pages_meta.get(page_idx, {'transparency_flag': 0, 'source_dimension': 256})

        with open(tpage_file, 'rb') as f:
            data = f.read()

        img = parse_tpage(data, meta)
        if img is None:
            print(f"  WARNING: Failed to parse {tpage_file}")
            continue

        out_file = os.path.join(output_dir, 'static', f'tpage{page_idx}.png')
        rel_path = os.path.relpath(out_file, os.path.dirname(output_dir)).replace('\\', '/')

        if not dry_run:
            os.makedirs(os.path.dirname(out_file), exist_ok=True)
            img.save(out_file)

        entries.append({
            'width': img.size[0],
            'height': img.size[1],
            'crc32': compute_r5g6b5_crc32(img),
            'path': rel_path,
            'source': f'static/tpage{page_idx}.dat',
        })

    action = "would convert" if dry_run else "converted"
    print(f"  static/tpage*.dat: {len(entries)} pages {action}")

    # Convert level-specific tpages
    level_entries = []
    for level_dir in sorted(glob.glob(os.path.join(assets_dir, 'levels', 'level*'))):
        level_name = os.path.basename(level_dir)
        for tpage_file in sorted(glob.glob(os.path.join(level_dir, 'tpage*.dat'))):
            basename = os.path.basename(tpage_file).lower()
            idx_str = basename.replace('tpage', '').replace('.dat', '')
            try:
                page_idx = int(idx_str)
            except ValueError:
                continue

            # Level tpages use the same metadata lookup (page index continues from static)
            meta = pages_meta.get(page_idx, {'transparency_flag': 0, 'source_dimension': 256})

            with open(tpage_file, 'rb') as f:
                data = f.read()

            img = parse_tpage(data, meta)
            if img is None:
                continue

            out_file = os.path.join(output_dir, 'levels', level_name, f'tpage{page_idx}.png')
            rel_path = os.path.relpath(out_file, os.path.dirname(output_dir)).replace('\\', '/')

            if not dry_run:
                os.makedirs(os.path.dirname(out_file), exist_ok=True)
                img.save(out_file)

            level_entries.append({
                'width': img.size[0],
                'height': img.size[1],
                'crc32': compute_r5g6b5_crc32(img),
                'path': rel_path,
                'source': f'{level_name}/tpage{page_idx}.dat',
            })

    if level_entries:
        print(f"  levels/*/tpage*.dat: {len(level_entries)} pages {action}")

    return entries + level_entries


# Dev leftovers shipped in the 1998 frontend.zip but never referenced by
# TD5_d3d.exe (verified via Ghidra string search 2026-04-23). Kept in
# re/unused_assets/ for archaeology; do not emit PNGs into re/assets/.
UNUSED_TGA_STEMS = {
    # frontend.zip leftovers (Ghidra-verified 0 refs in TD5_d3d.exe):
    'controllerso',
    'copy of controllers',
    'oldbodytext',
    'untitled-1',
    'mainfont',
    'smalfont',
    'tick',
    '3d sound',
    'arrowbuttons',
    'logo',
    'netmenu',
    'racemenu',
    'small td5 wht',
    'explogo',
    'inprog',
    'player1text',
    'player2text',
    # environs.zip leftovers: shipped in the zip but never selected by the
    # per-track environs pointer table @ 0x0046bb1c (raw byte search: 0 refs
    # in TD5_d3d.exe). See td5_environs_table.inc for the active set.
    'atre1',
    'htun1',
    'mbrd1',
    'mbwl1',
    'msun',
    'nbrd2',
    'qtun1',
    'sbrd1',
    'sunbkp',
}


def convert_tga_files(assets_dir, output_dir, dry_run=False):
    """Convert TGA files from cars, levels, frontend, traffic, etc."""
    entries = []
    categories = [
        ('cars', 'cars'),
        ('levels', 'levels'),
        ('frontend', 'frontend'),
        ('extras', 'extras'),
        ('mugshots', 'mugshots'),
        ('tracks', 'tracks'),
        ('traffic', 'traffic'),
        ('loading', 'loading'),
        ('legals', 'legals'),
        ('environs', 'environs'),
    ]

    for src_subdir, out_subdir in categories:
        src_path = os.path.join(assets_dir, src_subdir)
        if not os.path.isdir(src_path):
            continue

        count = 0
        for root, dirs, files in os.walk(src_path):
            for fname in sorted(files):
                if not fname.lower().endswith('.tga'):
                    continue
                if os.path.splitext(fname)[0].lower() in UNUSED_TGA_STEMS:
                    continue

                tga_path = os.path.join(root, fname)
                with open(tga_path, 'rb') as f:
                    data = f.read()

                img = parse_tga(data)
                if img is None:
                    print(f"  WARNING: Failed to parse {tga_path}")
                    continue

                # Apply alpha rules based on filename
                basename_upper = fname.upper()
                if basename_upper.startswith('CARHUB'):
                    img = apply_carhub_alpha(img)

                # Build output path preserving subdirectory structure
                rel_to_src = os.path.relpath(root, src_path)
                png_name = os.path.splitext(fname)[0] + '.png'
                if rel_to_src == '.':
                    out_file = os.path.join(output_dir, out_subdir, png_name)
                else:
                    out_file = os.path.join(output_dir, out_subdir, rel_to_src, png_name)

                rel_path = os.path.relpath(out_file, os.path.dirname(output_dir)).replace('\\', '/')

                if not dry_run:
                    os.makedirs(os.path.dirname(out_file), exist_ok=True)
                    img.save(out_file)

                entries.append({
                    'width': img.size[0],
                    'height': img.size[1],
                    'crc32': compute_r5g6b5_crc32(img),
                    'path': rel_path,
                    'source': os.path.relpath(tga_path, assets_dir).replace('\\', '/'),
                })
                count += 1

        if count > 0:
            action = "would convert" if dry_run else "converted"
            print(f"  {src_subdir}/*.tga: {count} files {action}")

    return entries


# =============================================================================
# Main
# =============================================================================

def main():
    parser = argparse.ArgumentParser(
        description="Convert TD5 textures to PNG with authored alpha")
    parser.add_argument('--assets-dir', required=True,
                        help="Path to extracted assets/ directory")
    parser.add_argument('--output-dir', required=True,
                        help="Output directory for PNGs (e.g., data/td5_png)")
    parser.add_argument('--dry-run', action='store_true',
                        help="Show what would be converted without writing files")
    parser.add_argument('--update-index', action='store_true',
                        help="Only regenerate index.dat from existing PNGs (don't overwrite)")
    args = parser.parse_args()

    assets_dir = os.path.abspath(args.assets_dir)
    output_dir = os.path.abspath(args.output_dir)

    if not os.path.isdir(assets_dir):
        print(f"ERROR: Assets directory not found: {assets_dir}")
        sys.exit(1)

    print(f"Assets dir: {assets_dir}")
    print(f"Output dir: {output_dir}")
    if args.dry_run:
        print("[DRY RUN MODE]")
    print()

    all_entries = []

    # 1. TEXTURES.DAT
    print("Converting TEXTURES.DAT files...")
    all_entries.extend(convert_textures_dat(assets_dir, output_dir, args.dry_run))

    # 2. tpage*.dat
    print("\nConverting tpage*.dat files...")
    all_entries.extend(convert_tpages(assets_dir, output_dir, args.dry_run))

    # 3. TGA files
    print("\nConverting TGA files...")
    all_entries.extend(convert_tga_files(assets_dir, output_dir, args.dry_run))

    # Write index.dat
    index_path = os.path.join(output_dir, 'index.dat')
    if not args.dry_run and all_entries:
        os.makedirs(output_dir, exist_ok=True)
        write_index_dat(all_entries, index_path)
        print(f"\nIndex written: {index_path} ({len(all_entries)} entries)")

    print(f"\nSummary: {len(all_entries)} textures {'would be' if args.dry_run else ''} converted")


if __name__ == '__main__':
    main()
