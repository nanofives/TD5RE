"""
td5_build_index.py - Build index.dat from dumped/edited texture PNGs.

Reads all PNG files from a directory (typically td5_png/_dump/ or a curated
override directory) and generates index.dat for the ddraw wrapper's PNG loader.

The CRC32 is extracted from the filename (WxH_CRC32.png format) so edited
PNGs match the same runtime textures they were dumped from.

Workflow:
  1. Run game with td5_png/dump_textures.flag present -> dumps to _dump/
  2. Copy textures you want to override to td5_png/overrides/
  3. Edit them (add alpha channels, fix transparency, etc.)
  4. Run this script to build index.dat
  5. Remove dump_textures.flag and run game -> overrides are applied

Usage:
  python td5_build_index.py --png-dir data/td5_png/_dump --output data/td5_png/index.dat
  python td5_build_index.py --png-dir data/td5_png/overrides --output data/td5_png/index.dat
  python td5_build_index.py --manifest data/td5_png/_dump/manifest.txt --output data/td5_png/index.dat
"""
import argparse
import glob
import os
import re
import struct
import sys


def parse_dump_filename(filename):
    """Extract width, height, CRC32 from dump filename like '256x256_A1B2C3D4.png'."""
    m = re.match(r'^(\d+)x(\d+)_([0-9A-Fa-f]{8})\.png$', filename)
    if not m:
        return None
    return int(m.group(1)), int(m.group(2)), int(m.group(3), 16)


def parse_manifest(manifest_path):
    """Parse manifest.txt from dump and return list of entries."""
    entries = []
    with open(manifest_path, 'r') as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith('#'):
                continue
            parts = line.split()
            if len(parts) >= 4:
                crc = int(parts[0], 16)
                w = int(parts[1])
                h = int(parts[2])
                path = parts[3]
                entries.append({'crc32': crc, 'width': w, 'height': h, 'path': path})
    return entries


def scan_png_dir(png_dir, base_dir=None, recursive=False):
    """Scan a directory of PNGs with dump-style filenames.
    Paths are stored relative to base_dir (or CWD if not specified).
    Supports both flat (WxH_CRC.png) and organized (WxH_CRC_name.png) filenames."""
    entries = []
    if recursive:
        png_files = sorted(glob.glob(os.path.join(png_dir, '**', '*.png'), recursive=True))
    else:
        png_files = sorted(glob.glob(os.path.join(png_dir, '*.png')))

    for png_path in png_files:
        filename = os.path.basename(png_path)
        # Match both WxH_CRC.png and WxH_CRC_name.png formats
        m = re.match(r'^(\d+)x(\d+)_([0-9A-Fa-f]{8})(?:_.*)?\.png$', filename)
        if not m:
            continue
        w, h, crc = int(m.group(1)), int(m.group(2)), int(m.group(3), 16)
        if base_dir:
            rel_path = os.path.relpath(png_path, base_dir).replace('\\', '/')
        else:
            rel_path = png_path.replace('\\', '/')
        entries.append({'crc32': crc, 'width': w, 'height': h, 'path': rel_path})
    return entries


def write_index_dat(entries, output_path):
    """Write index.dat in the format expected by png_loader.c."""
    os.makedirs(os.path.dirname(output_path), exist_ok=True)
    with open(output_path, 'wb') as f:
        f.write(b'TD5PNG\x01\x00')
        f.write(struct.pack('<I', len(entries)))
        for entry in entries:
            f.write(struct.pack('<II', entry['width'], entry['height']))
            f.write(struct.pack('<I', entry['crc32']))
            path_bytes = entry['path'].encode('utf-8') + b'\x00'
            f.write(struct.pack('<H', len(path_bytes)))
            f.write(path_bytes)


def main():
    parser = argparse.ArgumentParser(
        description="Build index.dat from dumped/edited texture PNGs")
    parser.add_argument('--png-dir',
                        help="Directory containing WxH_CRC.png files to index")
    parser.add_argument('--manifest',
                        help="Path to manifest.txt from texture dump (alternative to --png-dir)")
    parser.add_argument('--output', required=True,
                        help="Output path for index.dat")
    parser.add_argument('--base-dir', default=None,
                        help="Base directory for relative paths in index (default: parent of --png-dir)")
    parser.add_argument('--recursive', action='store_true',
                        help="Scan --png-dir recursively (for organized subdirectory layout)")
    parser.add_argument('--merge', action='append', default=[],
                        help="Additional PNG directories to merge (can specify multiple)")
    args = parser.parse_args()

    if not args.png_dir and not args.manifest:
        print("ERROR: specify --png-dir or --manifest")
        sys.exit(1)

    # Base dir for relative paths: default to parent of png-dir
    # (so td5_png/_dump/foo.png becomes td5_png/_dump/foo.png relative to data/)
    base_dir = args.base_dir
    if not base_dir and args.png_dir:
        base_dir = os.path.dirname(os.path.dirname(os.path.abspath(args.png_dir)))

    all_entries = []
    seen_crcs = set()

    # Primary source
    if args.manifest:
        print(f"Reading manifest: {args.manifest}")
        entries = parse_manifest(args.manifest)
        # Fix manifest paths to be relative to base_dir
        if base_dir:
            for e in entries:
                abs_path = os.path.abspath(e['path'])
                e['path'] = os.path.relpath(abs_path, base_dir).replace('\\', '/')
        for e in entries:
            if e['crc32'] not in seen_crcs:
                all_entries.append(e)
                seen_crcs.add(e['crc32'])
        print(f"  {len(entries)} entries from manifest")
    elif args.png_dir:
        print(f"Scanning: {args.png_dir}" + (" (recursive)" if args.recursive else ""))
        entries = scan_png_dir(args.png_dir, base_dir, args.recursive)
        for e in entries:
            if e['crc32'] not in seen_crcs:
                all_entries.append(e)
                seen_crcs.add(e['crc32'])
        print(f"  {len(entries)} PNGs found")

    # Merge additional directories (overrides take precedence)
    for merge_dir in args.merge:
        print(f"Merging: {merge_dir}")
        entries = scan_png_dir(merge_dir, base_dir)
        merged = 0
        for e in entries:
            if e['crc32'] in seen_crcs:
                # Override existing entry
                all_entries = [x if x['crc32'] != e['crc32'] else e for x in all_entries]
            else:
                all_entries.append(e)
                seen_crcs.add(e['crc32'])
            merged += 1
        print(f"  {merged} overrides merged")

    write_index_dat(all_entries, args.output)
    print(f"\nIndex written: {args.output} ({len(all_entries)} entries)")


if __name__ == '__main__':
    main()
