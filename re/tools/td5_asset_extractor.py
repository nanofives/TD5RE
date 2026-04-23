"""
td5_asset_extractor.py - Extract all TD5 ZIP archives into organized directory tree.

Extracts ALL files from ALL game ZIPs into a clean folder structure suitable for
the ReadArchiveEntry hook in td5_mod.c. Original ZIPs are kept in place as fallback.

Target layout (under <game_dir>/data/ if CWD patch applied, else <game_dir>/):
  assets/
    manifest.json          mapping of (zip_path, entry_name) -> extracted_path
    levels/
      level001/ ... level039/   all files from each level ZIP
    static/                     static.zip contents
    cars/
      cam/ day/ ...             each car ZIP into its own folder
    traffic/                    traffic.zip contents
    environs/                   environs.zip contents
    loading/                    loading.zip contents (LOADING.ZIP)
    legals/                     legals.zip contents (LEGALS.ZIP)
    frontend/                   frontend.zip contents
    extras/                     Extras.zip contents
    mugshots/                   Mugshots.zip contents
    tracks/                     Tracks.zip contents
    sounds/                     Sounds.zip (frontend) contents
    sound/                      SOUND.ZIP (race ambient) contents
    cup/                        Cup.zip contents
    movie/                      intro.tgq (non-ZIP, just moved/copied)

Usage:
  python td5_asset_extractor.py --game-dir "D:/Descargas/Test Drive 5 ISO"
  python td5_asset_extractor.py --game-dir "..." --dry-run
  python td5_asset_extractor.py --game-dir "..." --data-dir data  # look for ZIPs in data/
"""
import argparse
import json
import os
import re
import sys
import zipfile


# --- ZIP -> output subfolder mapping ---
# Each entry: (zip_relative_path_pattern, output_subfolder, is_glob)
# Paths are relative to the data directory (or game root if no CWD patch).
ZIP_MAPPINGS = [
    # Level archives: level001.zip -> assets/levels/level001/
    (r"level(\d+)\.zip",                "levels/level{0}",     True),
    # Static shared assets
    ("static.zip",                      "static",              False),
    # Car archives: cars/cam.zip -> assets/cars/cam/
    (r"cars[/\\](\w+)\.zip",           "cars/{0}",            True),
    # Traffic vehicles
    ("traffic.zip",                     "traffic",             False),
    # Environment billboards
    ("environs.zip",                    "environs",            False),
    # Loading screens
    ("loading.zip",                     "loading",             False),
    # Legal screens
    ("legals.zip",                      "legals",              False),
    # Cup.zip is an installer payload (Config.td5/CupData.td5 + M2DX.dll +
    # Language.dll + TD5.ini + Uninst.isu). Neither TD5_d3d.exe nor td5re.exe
    # loads from it; savegames are read/written from the working directory.
    # Preserved in re/unused_assets/cup/ for archaeology; not extracted here.
    # Frontend UI
    ("Front End/frontend.zip",          "frontend",            False),
    # Frontend extras
    ("Front End/Extras/Extras.zip",     "extras",              False),
    ("Front End/Extras/Mugshots.zip",   "mugshots",            False),
    # Frontend sounds
    ("Front End/Sounds/Sounds.zip",     "sounds",              False),
    # Frontend track previews
    ("Front End/Tracks/Tracks.zip",     "tracks",              False),
    # Race ambient sound
    ("sound/SOUND.ZIP",                 "sound",               False),
]


def normalize_path(p):
    """Normalize path separators to forward slashes for consistent matching."""
    return p.replace('\\', '/')


def match_zip_to_subfolder(zip_rel_path):
    """Match a ZIP's relative path to its output subfolder under assets/."""
    norm = normalize_path(zip_rel_path)
    for pattern, subfolder_template, is_glob in ZIP_MAPPINGS:
        if is_glob:
            m = re.match(pattern, norm, re.IGNORECASE)
            if m:
                return subfolder_template.format(*m.groups())
        else:
            if norm.lower() == normalize_path(pattern).lower():
                return subfolder_template
    return None


def discover_zips(data_dir):
    """Find all ZIP files in the data directory."""
    zips = []
    for root, dirs, files in os.walk(data_dir):
        # Skip the assets output directory and RE/tool directories
        dirs[:] = [d for d in dirs if d.lower() not in ('assets', 're', 'td5mod',
                   'ghidra_12.0.3_public', '_organization', 'scripts', 'README')]
        for f in files:
            if f.lower().endswith('.zip'):
                full = os.path.join(root, f)
                rel = os.path.relpath(full, data_dir)
                zips.append((full, rel))
    return sorted(zips, key=lambda x: x[1].lower())


def extract_zip(zip_path, zip_rel, assets_dir, subfolder, manifest, dry_run=False):
    """Extract all files from a ZIP into assets/<subfolder>/."""
    out_dir = os.path.join(assets_dir, subfolder)
    try:
        with zipfile.ZipFile(zip_path, 'r') as zf:
            entries = zf.namelist()
            extracted = 0
            for entry_name in entries:
                # Skip directory entries
                if entry_name.endswith('/'):
                    continue

                # Flatten: strip any internal directory prefix for simple archives,
                # but preserve structure for frontend (which has nested paths)
                out_name = entry_name

                out_path = os.path.join(out_dir, out_name)

                # Record in manifest
                manifest_key = f"{normalize_path(zip_rel)}|{entry_name}"
                manifest[manifest_key] = normalize_path(
                    os.path.relpath(out_path, assets_dir))

                if dry_run:
                    extracted += 1
                    continue

                os.makedirs(os.path.dirname(out_path), exist_ok=True)
                with zf.open(entry_name) as src, open(out_path, 'wb') as dst:
                    dst.write(src.read())
                extracted += 1

            return extracted, len(entries)
    except (zipfile.BadZipFile, PermissionError) as e:
        print(f"  WARNING: Failed to extract {zip_rel}: {e}")
        return 0, 0


def main():
    parser = argparse.ArgumentParser(
        description="Extract TD5 ZIP archives into organized directory tree")
    parser.add_argument('--game-dir', required=True,
                        help="Path to the game root directory")
    parser.add_argument('--data-dir', default='.',
                        help="Subdirectory containing game data files (default: . = game root)")
    parser.add_argument('--assets-dir', default='assets',
                        help="Output subdirectory name (default: assets)")
    parser.add_argument('--dry-run', action='store_true',
                        help="Show what would be extracted without writing files")
    args = parser.parse_args()

    game_dir = os.path.abspath(args.game_dir)
    data_dir = os.path.join(game_dir, args.data_dir) if args.data_dir != '.' else game_dir
    assets_dir = os.path.join(data_dir, args.assets_dir)

    if not os.path.isdir(data_dir):
        print(f"ERROR: Data directory not found: {data_dir}")
        sys.exit(1)

    print(f"Game dir:   {game_dir}")
    print(f"Data dir:   {data_dir}")
    print(f"Assets dir: {assets_dir}")
    if args.dry_run:
        print("[DRY RUN MODE]")
    print()

    # Discover ZIPs
    zips = discover_zips(data_dir)
    print(f"Found {len(zips)} ZIP archives:")

    manifest = {}
    total_extracted = 0
    total_entries = 0
    unmatched = []

    for zip_path, zip_rel in zips:
        subfolder = match_zip_to_subfolder(zip_rel)
        if subfolder is None:
            unmatched.append(zip_rel)
            print(f"  SKIP {zip_rel} (no mapping)")
            continue

        extracted, entries = extract_zip(
            zip_path, zip_rel, assets_dir, subfolder, manifest, args.dry_run)
        total_extracted += extracted
        total_entries += entries
        status = "would extract" if args.dry_run else "extracted"
        print(f"  {zip_rel} -> {args.assets_dir}/{subfolder}/ "
              f"({extracted}/{entries} files {status})")

    # Write manifest
    manifest_path = os.path.join(assets_dir, 'manifest.json')
    if not args.dry_run:
        os.makedirs(assets_dir, exist_ok=True)
        with open(manifest_path, 'w') as f:
            json.dump(manifest, f, indent=2, sort_keys=True)
        print(f"\nManifest written: {manifest_path} ({len(manifest)} entries)")

    print(f"\nSummary:")
    print(f"  Archives processed: {len(zips) - len(unmatched)}/{len(zips)}")
    print(f"  Files {'would extract' if args.dry_run else 'extracted'}: {total_extracted}")
    if unmatched:
        print(f"  Unmatched ZIPs: {unmatched}")


if __name__ == '__main__':
    main()
