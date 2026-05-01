#!/usr/bin/env python3
"""
td5_texture_catalog.py - Extract and classify ALL textures from the original game.

Produces a clean named directory tree with every texture identified by its game
purpose, plus a catalog.json with full metadata.

Output layout:
  td5_textures/
    catalog.json                    Full metadata index
    cars/<code>/carskin0.png        Car skin textures (4 per car)
    cars/<code>/carpic0.png         Car selection pictures (4 per car)
    cars/<code>/carhub0.png         Car hub textures (4 per car)
    levels/level001/tex_000.png     Paletted 64x64 track textures
    levels/level001/forwsky.png     Forward sky panorama
    levels/level001/backsky.png     Backward sky panorama
    static/pages/tpage0.png         Full atlas pages (for reference)
    static/sprites/SPEEDO.png       Individual sprites cropped from atlas
    traffic/skin0.png               Traffic vehicle skins
    environs/ATRE1.png              Environment billboards
    frontend/TEXTURE.png            Frontend UI textures
    loading/LOAD01.png              Loading screens
    legals/LEGALS0.png              Legal screens
    tracks/TRACK01.png              Track preview images
    mugshots/MUG01.png              Driver mugshots
    extras/EXTRA01.png              Extra images

Usage:
  python td5_texture_catalog.py --assets-dir ../assets --output-dir ../../td5_textures
  python td5_texture_catalog.py --assets-dir ../assets --output-dir ../../td5_textures --dry-run
"""

import argparse
import json
import os
import struct
import sys

try:
    from PIL import Image
except ImportError:
    print("ERROR: Pillow required.  pip install Pillow")
    sys.exit(1)


# ---------------------------------------------------------------------------
# static.hed parser
# ---------------------------------------------------------------------------

def parse_static_hed(hed_path):
    """Parse static.hed -> (entries, page_meta).

    Returns:
        entries: list of dicts {name, pos_x, pos_y, width, height, slot}
        page_meta: dict of page_index -> {trans, type, src_w, src_h}
    """
    with open(hed_path, 'rb') as f:
        data = f.read()

    if len(data) < 8:
        return [], {}

    page_count, entry_count = struct.unpack_from('<II', data, 0)
    entries = []
    off = 8
    for _ in range(entry_count):
        if off + 64 > len(data):
            break
        rec = data[off:off + 64]
        name = rec[:44].split(b'\0')[0].decode('ascii', errors='replace').strip()
        pos_x, pos_y, w, h, slot = struct.unpack_from('<iiiii', rec, 44)
        if name:
            entries.append({
                'name': name,
                'pos_x': pos_x, 'pos_y': pos_y,
                'width': w, 'height': h,
                'slot': slot,
            })
        off += 64

    page_meta = {}
    for i in range(page_count):
        if off + 16 > len(data):
            break
        trans, img_type, src_w, src_h = struct.unpack_from('<iiii', data, off)
        page_meta[i] = {
            'transparency_flag': trans,
            'image_type': img_type,
            'source_width': src_w,
            'source_height': src_h,
        }
        off += 16

    return entries, page_meta


# ---------------------------------------------------------------------------
# tpage decoder
# ---------------------------------------------------------------------------

def decode_tpage(dat_path, meta):
    """Decode a raw tpage*.dat file into a PIL Image (RGBA).

    All .dat files are 256x256x4 = 262144 bytes (4 bpp).
    - Pages from static.zip (trans=0): BGRA byte order [B,G,R,A]
    - Pages from runtime dump (trans!=0): ARGB byte order [A,R,G,B]
    """
    with open(dat_path, 'rb') as f:
        raw = f.read()

    trans = meta.get('transparency_flag', 0)
    w = meta.get('source_width', 256)
    h = meta.get('source_height', 256)
    if w <= 0: w = 256
    if h <= 0: h = 256

    expected = w * h * 4
    if len(raw) < expected:
        return None

    img = Image.new('RGBA', (w, h))
    pixels = []
    if trans != 0:
        # Runtime-captured pages: ARGB byte order [A, R, G, B]
        for i in range(0, expected, 4):
            a, r, g, b = raw[i], raw[i+1], raw[i+2], raw[i+3]
            pixels.append((r, g, b, a))
    else:
        # static.zip pages: BGRA byte order [B, G, R, A]
        for i in range(0, expected, 4):
            b, g, r, a = raw[i], raw[i+1], raw[i+2], raw[i+3]
            pixels.append((r, g, b, a))
    img.putdata(pixels)

    return img


# ---------------------------------------------------------------------------
# TEXTURES.DAT parser (paletted 64x64 track textures)
# ---------------------------------------------------------------------------

def parse_textures_dat(dat_path):
    """Yield (index, format_type, PIL.Image) from a TEXTURES.DAT file."""
    with open(dat_path, 'rb') as f:
        data = f.read()

    if len(data) < 4:
        return

    num_entries = struct.unpack_from('<I', data, 0)[0]
    if num_entries == 0 or num_entries > 10000:
        return

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

        format_type = data[offset + 3]  # 0/3=opaque, 1=keyed, 2=semi
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
            if format_type == 1:
                a = 0 if idx_byte == 0 else 255
            elif format_type == 2:
                a = 128
            else:
                a = 255
            img_data.append((r, g, b, a))

        img.putdata(img_data)
        yield tex_idx, format_type, img


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

FORMAT_NAMES = {0: 'opaque', 1: 'color_key', 2: 'semi_transparent', 3: 'opaque'}

# Sprite classification by static.hed slot
SLOT_CATEGORIES = {
    0: 'car_page', 1: 'car_page', 2: 'car_page',
    3: 'sky',
    4: 'hud_sprites',
    5: 'hud_sprites',
    6: 'traffic', 7: 'traffic', 8: 'traffic',
    9: 'traffic', 10: 'traffic', 11: 'traffic',
    12: 'pause_menu',
    13: 'environment', 14: 'environment', 15: 'environment', 16: 'environment',
}


def save_png(img, path, dry_run=False):
    """Save PIL image as PNG, creating directories as needed."""
    if dry_run:
        return
    os.makedirs(os.path.dirname(path), exist_ok=True)
    img.save(path, 'PNG')


def convert_tga(tga_path):
    """Load a TGA and return as RGBA PIL Image."""
    try:
        img = Image.open(tga_path).convert('RGBA')
        return img
    except Exception as e:
        print(f"  WARNING: failed to load {tga_path}: {e}")
        return None


# ---------------------------------------------------------------------------
# Main extraction
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(description="Extract and classify all TD5 textures")
    parser.add_argument('--assets-dir', required=True,
                        help="Path to extracted assets (from td5_asset_extractor.py)")
    parser.add_argument('--output-dir', required=True,
                        help="Output directory for classified textures")
    parser.add_argument('--dry-run', action='store_true',
                        help="Print what would be done without writing files")
    args = parser.parse_args()

    assets = os.path.abspath(args.assets_dir)
    outdir = os.path.abspath(args.output_dir)
    dry_run = args.dry_run

    if not os.path.isdir(assets):
        print(f"ERROR: assets directory not found: {assets}")
        sys.exit(1)

    if not dry_run:
        os.makedirs(outdir, exist_ok=True)

    catalog = []
    stats = {}

    def add_entry(category, name, rel_path, width, height, **extra):
        entry = {
            'category': category,
            'name': name,
            'path': rel_path,
            'width': width,
            'height': height,
        }
        entry.update(extra)
        catalog.append(entry)
        stats[category] = stats.get(category, 0) + 1

    # =======================================================================
    # 1. CAR TEXTURES (TGA: carskin, carpic, carhub)
    # =======================================================================
    print("=== Cars ===")
    cars_dir = os.path.join(assets, 'cars')
    if os.path.isdir(cars_dir):
        for car_code in sorted(os.listdir(cars_dir)):
            car_path = os.path.join(cars_dir, car_code)
            if not os.path.isdir(car_path):
                continue
            for tga_name in sorted(os.listdir(car_path)):
                if not tga_name.lower().endswith('.tga'):
                    continue
                tga_path = os.path.join(car_path, tga_name)
                img = convert_tga(tga_path)
                if img is None:
                    continue
                png_name = os.path.splitext(tga_name)[0].lower() + '.png'
                rel = f"cars/{car_code}/{png_name}"
                save_png(img, os.path.join(outdir, rel), dry_run)

                # Classify car texture type
                base = tga_name.lower()
                if 'carskin' in base:
                    tex_type = 'car_skin'
                elif 'carpic' in base:
                    tex_type = 'car_picture'
                elif 'carhub' in base:
                    tex_type = 'car_hub'
                else:
                    tex_type = 'car_other'

                add_entry('cars', f"{car_code}/{png_name}", rel,
                          img.width, img.height,
                          car_code=car_code, texture_type=tex_type)

        print(f"  {stats.get('cars', 0)} car textures")

    # =======================================================================
    # 2. LEVEL TEXTURES (TEXTURES.DAT + sky TGAs)
    # =======================================================================
    print("=== Levels ===")
    levels_dir = os.path.join(assets, 'levels')
    level_tex_count = 0
    level_sky_count = 0
    if os.path.isdir(levels_dir):
        for level_name in sorted(os.listdir(levels_dir)):
            level_path = os.path.join(levels_dir, level_name)
            if not os.path.isdir(level_path):
                continue

            # TEXTURES.DAT
            for dat_name in ['textures.dat', 'TEXTURES.DAT']:
                dat_path = os.path.join(level_path, dat_name)
                if os.path.exists(dat_path):
                    for tex_idx, fmt_type, img in parse_textures_dat(dat_path):
                        png_name = f"tex_{tex_idx:03d}.png"
                        rel = f"levels/{level_name}/{png_name}"
                        save_png(img, os.path.join(outdir, rel), dry_run)
                        add_entry('level_textures', f"{level_name}/tex_{tex_idx:03d}",
                                  rel, 64, 64,
                                  level=level_name, texture_index=tex_idx,
                                  format=FORMAT_NAMES.get(fmt_type, f'unknown_{fmt_type}'))
                        level_tex_count += 1
                    break

            # Sky TGAs
            for sky_name in ['forwsky', 'backsky', 'FORWSKY', 'BACKSKY']:
                for ext in ['.tga', '.TGA']:
                    sky_path = os.path.join(level_path, sky_name + ext)
                    if os.path.exists(sky_path):
                        img = convert_tga(sky_path)
                        if img:
                            png_name = sky_name.lower() + '.png'
                            rel = f"levels/{level_name}/{png_name}"
                            save_png(img, os.path.join(outdir, rel), dry_run)
                            add_entry('level_sky', f"{level_name}/{sky_name.lower()}",
                                      rel, img.width, img.height,
                                      level=level_name, sky_type=sky_name.lower())
                            level_sky_count += 1
                        break

        print(f"  {level_tex_count} track textures, {level_sky_count} sky textures")

    # =======================================================================
    # 3. STATIC ATLAS (tpage*.dat with sprite cropping from static.hed)
    # =======================================================================
    print("=== Static Atlas ===")
    static_dir = os.path.join(assets, 'static')
    hed_path = None
    for name in ['static.hed', 'STATIC.HED']:
        p = os.path.join(static_dir, name)
        if os.path.exists(p):
            hed_path = p
            break

    hed_entries = []
    page_meta = {}
    if hed_path:
        hed_entries, page_meta = parse_static_hed(hed_path)

    # Decode all tpage images.
    # Pages 0-2 ship in static.zip as true BGRA32 source data.
    # Pages 4,5 were captured at runtime via dump_tpages.py, but then
    # re-encoded through R5G6B5 quantization (td5_png_clean pipeline),
    # introducing dithering.  Mark them accordingly.
    # Pages 3,6-11,12-16 are composited at runtime from other source textures.
    ORIGINAL_TPAGES = {0, 1, 2}  # from static.zip, true source quality
    tpage_images = {}
    for fname in sorted(os.listdir(static_dir)):
        if not fname.lower().startswith('tpage') or not fname.lower().endswith('.dat'):
            continue
        idx_str = fname.lower().replace('tpage', '').replace('.dat', '')
        try:
            slot = int(idx_str)
        except ValueError:
            continue
        dat_path = os.path.join(static_dir, fname)
        meta = page_meta.get(slot, {'transparency_flag': 0, 'source_width': 256, 'source_height': 256})
        img = decode_tpage(dat_path, meta)
        if img:
            tpage_images[slot] = img
            # Save full page for reference
            is_original = slot in ORIGINAL_TPAGES
            rel = f"static/pages/tpage{slot}.png"
            save_png(img, os.path.join(outdir, rel), dry_run)
            add_entry('static_page', f"tpage{slot}", rel,
                      img.width, img.height,
                      slot=slot,
                      transparency=meta.get('transparency_flag', 0),
                      image_type=meta.get('image_type', 0),
                      source_quality='original' if is_original else 'r5g6b5_quantized',
                      note=None if is_original else
                          'Runtime-captured then R5G6B5-quantized; re-dump from game for true quality')

    print(f"  {len(tpage_images)} full atlas pages saved")

    # Crop individual sprites from atlas pages
    sprite_count = 0
    for entry in hed_entries:
        slot = entry['slot']
        if slot not in tpage_images:
            continue
        page_img = tpage_images[slot]
        x, y, w, h = entry['pos_x'], entry['pos_y'], entry['width'], entry['height']
        if w <= 0 or h <= 0:
            continue
        # Clamp to page bounds
        pw, ph = page_img.size
        if x + w > pw or y + h > ph:
            continue

        sprite = page_img.crop((x, y, x + w, y + h))
        sprite_name = entry['name']
        category = SLOT_CATEGORIES.get(slot, 'unknown')
        rel = f"static/sprites/{sprite_name}.png"
        save_png(sprite, os.path.join(outdir, rel), dry_run)
        is_original = slot in ORIGINAL_TPAGES
        add_entry('static_sprite', sprite_name, rel, w, h,
                  slot=slot, atlas_x=x, atlas_y=y,
                  slot_category=category,
                  source_quality='original' if is_original else 'r5g6b5_quantized')
        sprite_count += 1

    print(f"  {sprite_count} individual sprites cropped from atlas")

    # Record runtime-composited sprites that have no tpage file
    runtime_count = 0
    for entry in hed_entries:
        slot = entry['slot']
        if slot in tpage_images:
            continue  # Already handled above
        sprite_name = entry['name']
        w, h = entry['width'], entry['height']
        category = SLOT_CATEGORIES.get(slot, 'unknown')

        # Map to source texture
        if slot == 3:
            source = "level forwsky.tga (loaded per-level at runtime)"
        elif 6 <= slot <= 11:
            source = f"traffic/ TGAs (composited by M2DX at runtime into slot {slot})"
        elif slot == 12:
            source = "runtime-generated pause menu page (no source file)"
        elif 13 <= slot <= 16:
            source = f"environs/ TGAs (loaded at runtime into slot {slot})"
        else:
            source = f"runtime-composited (slot {slot})"

        add_entry('runtime_composited', sprite_name, None, w, h,
                  slot=slot, slot_category=category,
                  note=source)
        runtime_count += 1

    if runtime_count:
        print(f"  {runtime_count} runtime-composited sprites cataloged (no file extraction)")

    # Also handle environ0.tga from static
    for env_name in ['environ0.tga', 'ENVIRON0.TGA']:
        env_path = os.path.join(static_dir, env_name)
        if os.path.exists(env_path):
            img = convert_tga(env_path)
            if img:
                rel = "static/environ0.png"
                save_png(img, os.path.join(outdir, rel), dry_run)
                add_entry('static_environ', 'environ0', rel, img.width, img.height)

    # =======================================================================
    # 4. SIMPLE TGA DIRECTORIES (traffic, environs, frontend, loading, etc.)
    # =======================================================================
    simple_dirs = [
        ('traffic',   'traffic'),
        ('environs',  'environs'),
        ('frontend',  'frontend'),
        ('loading',   'loading'),
        ('legals',    'legals'),
        ('tracks',    'tracks'),
        ('mugshots',  'mugshots'),
        ('extras',    'extras'),
    ]

    for category, subdir in simple_dirs:
        src_dir = os.path.join(assets, subdir)
        if not os.path.isdir(src_dir):
            continue
        count = 0
        for fname in sorted(os.listdir(src_dir)):
            if not fname.lower().endswith('.tga'):
                continue
            tga_path = os.path.join(src_dir, fname)
            img = convert_tga(tga_path)
            if img is None:
                continue
            png_name = os.path.splitext(fname)[0] + '.png'
            rel = f"{subdir}/{png_name}"
            save_png(img, os.path.join(outdir, rel), dry_run)
            add_entry(category, os.path.splitext(fname)[0], rel,
                      img.width, img.height)
            count += 1
        if count > 0:
            print(f"=== {category.title()} === {count} textures")

    # =======================================================================
    # Summary & catalog output
    # =======================================================================
    print(f"\n{'=' * 60}")
    print(f"TOTAL: {len(catalog)} textures cataloged")
    print(f"{'=' * 60}")
    for cat in sorted(stats.keys()):
        print(f"  {cat:25s} {stats[cat]:5d}")

    if not dry_run:
        catalog_path = os.path.join(outdir, 'catalog.json')
        with open(catalog_path, 'w') as f:
            json.dump({
                'version': 1,
                'total_textures': len(catalog),
                'categories': stats,
                'textures': catalog,
            }, f, indent=2)
        print(f"\nCatalog written to: {catalog_path}")
    else:
        print("\n[DRY RUN — no files written]")


if __name__ == '__main__':
    main()
