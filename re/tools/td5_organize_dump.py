"""
td5_organize_dump.py - Reorganize flat texture dump into categorized folders.

Reads the manifest.txt (with source tracking) and copies/moves PNGs from
the flat _dump/ directory into organized subfolders based on their source
ZIP and entry name.

Output structure:
  td5_png/
    static/          tpage0.dat, tpage1.dat, ...
    levels/
      level001/      FORWSKY.TGA, BACKSKY.TGA, textures_*
      level002/      ...
    cars/
      cam/           CARSKIN0-2.TGA, CARHUB0-2.TGA
      day/           ...
    traffic/         skin0-30.tga (individual skins)
    environs/        environment billboard textures
    loading/         loading screen textures
    frontend/        menu UI textures
    _uncategorized/  anything without source info

Usage:
  python td5_organize_dump.py --dump-dir data/td5_png/_dump --output-dir data/td5_png/organized
  python td5_organize_dump.py --dump-dir data/td5_png/_dump --output-dir data/td5_png/organized --copy
"""
import argparse
import os
import re
import shutil
import sys


def categorize_source(zip_path, entry_name):
    """Map zip|entry to an organized subfolder and friendly filename."""
    zp = zip_path.replace('\\', '/').lower()
    entry = entry_name.strip()
    entry_lower = entry.lower()

    # Static shared textures
    if 'static.zip' in zp:
        return 'static', entry_lower

    # Level-specific
    m = re.match(r'level(\d+)\.zip', zp)
    if m:
        level = f'level{m.group(1)}'
        return f'levels/{level}', entry_lower

    # Car archives
    m = re.match(r'cars[/\\](\w+)\.zip', zp)
    if m:
        car = m.group(1)
        return f'cars/{car}', entry_lower

    # Traffic
    if 'traffic.zip' in zp:
        return 'traffic', entry_lower

    # Environs
    if 'environs.zip' in zp:
        return 'environs', entry_lower

    # Loading
    if 'loading.zip' in zp:
        return 'loading', entry_lower

    # Legals
    if 'legals.zip' in zp:
        return 'legals', entry_lower

    # Sound
    if 'sound' in zp and 'sound.zip' in zp:
        return 'sound', entry_lower

    # Frontend
    if 'frontend.zip' in zp:
        return 'frontend', entry_lower
    if 'extras.zip' in zp:
        return 'frontend/extras', entry_lower
    if 'mugshots.zip' in zp:
        return 'frontend/mugshots', entry_lower
    if 'sounds.zip' in zp:
        return 'frontend/sounds', entry_lower
    if 'tracks.zip' in zp:
        return 'frontend/tracks', entry_lower

    # Cup
    if 'cup.zip' in zp:
        return 'cup', entry_lower

    return '_uncategorized', entry_lower


def main():
    parser = argparse.ArgumentParser(
        description="Reorganize flat texture dump into categorized folders")
    parser.add_argument('--dump-dir', required=True,
                        help="Path to _dump/ directory with flat PNGs")
    parser.add_argument('--output-dir', required=True,
                        help="Output directory for organized structure")
    parser.add_argument('--copy', action='store_true',
                        help="Copy files instead of moving them")
    args = parser.parse_args()

    dump_dir = os.path.abspath(args.dump_dir)
    output_dir = os.path.abspath(args.output_dir)
    manifest_path = os.path.join(dump_dir, 'manifest.txt')

    if not os.path.exists(manifest_path):
        print(f"ERROR: manifest.txt not found in {dump_dir}")
        sys.exit(1)

    # Parse manifest
    entries = []
    with open(manifest_path) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith('#'):
                continue
            parts = line.split()
            if len(parts) < 5:
                continue
            crc = parts[0]
            w, h = parts[1], parts[2]
            png_rel = parts[3]
            source = parts[4]  # zip|entry

            png_filename = os.path.basename(png_rel)
            zip_path, entry_name = source.split('|', 1) if '|' in source else (source, 'unknown')
            subfolder, friendly_name = categorize_source(zip_path, entry_name)

            entries.append({
                'crc': crc,
                'width': w,
                'height': h,
                'png_filename': png_filename,
                'subfolder': subfolder,
                'entry_name': friendly_name,
                'source': source,
            })

    print(f"Manifest: {len(entries)} entries")

    # Organize
    moved = 0
    skipped = 0
    seen_paths = set()

    for entry in entries:
        src_path = os.path.join(dump_dir, entry['png_filename'])
        if not os.path.exists(src_path):
            skipped += 1
            continue

        # Build destination: subfolder/WxH_CRC_entryname.png
        base_name = os.path.splitext(entry['entry_name'])[0]
        dst_name = f"{entry['width']}x{entry['height']}_{entry['crc']}_{base_name}.png"
        dst_dir = os.path.join(output_dir, entry['subfolder'])
        dst_path = os.path.join(dst_dir, dst_name)

        # Avoid duplicates (same CRC can appear multiple times in manifest)
        if dst_path in seen_paths:
            continue
        seen_paths.add(dst_path)

        os.makedirs(dst_dir, exist_ok=True)
        if args.copy:
            shutil.copy2(src_path, dst_path)
        else:
            shutil.copy2(src_path, dst_path)  # Always copy to organized; originals stay

        moved += 1

    # Write organized manifest
    org_manifest = os.path.join(output_dir, 'manifest.txt')
    with open(org_manifest, 'w') as f:
        f.write("# CRC32    W    H  Organized_Path                                    Source\n")
        for entry in entries:
            base_name = os.path.splitext(entry['entry_name'])[0]
            dst_name = f"{entry['width']}x{entry['height']}_{entry['crc']}_{base_name}.png"
            rel = os.path.join(entry['subfolder'], dst_name).replace('\\', '/')
            f.write(f"{entry['crc']} {entry['width']:>4s} {entry['height']:>4s} {rel} {entry['source']}\n")

    # Summary by category
    from collections import Counter
    cats = Counter(e['subfolder'] for e in entries)
    print(f"\nOrganized {moved} textures ({skipped} skipped):")
    for cat, n in sorted(cats.items()):
        print(f"  {n:4d}  {cat}/")
    print(f"\nOutput: {output_dir}")


if __name__ == '__main__':
    main()
