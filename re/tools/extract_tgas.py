#!/usr/bin/env python3
"""
extract_tgas.py -- Rebuild re/assets/**/*.tga from original/*.zip.

Extracts only TGA members. Mapping matches td5_asset_resolve_png_path()
in td5mod/src/td5re/td5_asset.c so the converter's assets_dir layout is
reproduced exactly.

Usage:
    python re/tools/extract_tgas.py                 # extract (skip existing)
    python re/tools/extract_tgas.py --force         # overwrite existing
    python re/tools/extract_tgas.py --verify        # byte-compare, no writes
"""

import argparse
import io
import os
import re
import sys
import zipfile

# --- Archive -> destination subfolder under re/assets/ --------------------
# Tuple: (archive_path_relative_to_original, dest_subfolder_or_None)
# None means "derive from filename" (used for per-car and per-level zips).
STATIC_MAP = [
    ("static.zip",                          "static"),
    ("traffic.zip",                         "traffic"),
    ("environs.zip",                        "environs"),
    ("loading.zip",                         "loading"),
    ("legals.zip",                          "legals"),
    ("Cup.zip",                             "cup"),
    ("Front End/frontend.zip",              "frontend"),
    ("Front End/Extras/Extras.zip",         "extras"),
    ("Front End/Extras/Mugshots.zip",       "mugshots"),
    ("Front End/Tracks/Tracks.zip",         "tracks"),
]

LEVEL_RE = re.compile(r"^level(\d+)\.zip$", re.IGNORECASE)
CAR_RE   = re.compile(r"^(.+)\.zip$",       re.IGNORECASE)

# Dev leftovers in frontend.zip — unreferenced by TD5_d3d.exe (Ghidra-verified
# 2026-04-23). Preserved in re/unused_assets/, not extracted into re/assets/.
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
    # environs.zip leftovers: shipped but never selected by the per-track
    # environs pointer table @ 0x0046bb1c (raw byte search: 0 refs in exe).
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


def iter_archive_jobs(original_dir):
    """Yield (archive_abs_path, dest_subfolder) for every zip we extract from."""
    for rel, dest in STATIC_MAP:
        p = os.path.join(original_dir, rel)
        if os.path.isfile(p):
            yield p, dest

    for fname in sorted(os.listdir(original_dir)):
        m = LEVEL_RE.match(fname)
        if m:
            yield os.path.join(original_dir, fname), "levels/level{}".format(m.group(1))

    cars_dir = os.path.join(original_dir, "cars")
    if os.path.isdir(cars_dir):
        for fname in sorted(os.listdir(cars_dir)):
            m = CAR_RE.match(fname)
            if m:
                yield os.path.join(cars_dir, fname), "cars/{}".format(m.group(1))


def safe_member_path(member_name, dest_subfolder):
    """Resolve a zip member's on-disk target under re/assets/.

    Handles loading.zip's `../load04.tga` traversal by placing it at
    re/assets/ root. Rejects anything deeper or absolute.
    """
    n = member_name.replace("\\", "/")
    if n.startswith("/") or ":" in n:
        return None
    parts = n.split("/")
    if parts[:1] == [".."]:
        # One-level escape -> re/assets/ root. Reject deeper traversal.
        if ".." in parts[1:]:
            return None
        return "/".join(parts[1:])
    if ".." in parts:
        return None
    return "{}/{}".format(dest_subfolder, n)


def extract_archive(zip_path, dest_subfolder, assets_dir, force, verify, stats):
    with zipfile.ZipFile(zip_path) as zf:
        for info in zf.infolist():
            if info.is_dir():
                continue
            if not info.filename.lower().endswith(".tga"):
                continue
            stem = os.path.splitext(os.path.basename(info.filename))[0].lower()
            if stem in UNUSED_TGA_STEMS:
                stats["skipped_unused"] = stats.get("skipped_unused", 0) + 1
                continue

            rel = safe_member_path(info.filename, dest_subfolder)
            if rel is None:
                print("  SKIP unsafe path: {}::{}".format(zip_path, info.filename))
                stats["unsafe"] += 1
                continue

            out_path = os.path.join(assets_dir, rel)
            data = zf.read(info)

            if verify:
                if not os.path.isfile(out_path):
                    print("  MISSING {} (would extract from {})".format(rel, os.path.basename(zip_path)))
                    stats["missing"] += 1
                    continue
                with open(out_path, "rb") as f:
                    if f.read() != data:
                        print("  DIFFER  {} (re-extract would replace)".format(rel))
                        stats["differ"] += 1
                    else:
                        stats["match"] += 1
                continue

            if os.path.isfile(out_path) and not force:
                stats["skipped"] += 1
                continue

            os.makedirs(os.path.dirname(out_path), exist_ok=True)
            with open(out_path, "wb") as f:
                f.write(data)
            stats["wrote"] += 1


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[1])
    ap.add_argument("--original", default="original",
                    help="Path to original/ (default: ./original)")
    ap.add_argument("--assets",   default="re/assets",
                    help="Path to re/assets/ (default: ./re/assets)")
    ap.add_argument("--force",  action="store_true", help="Overwrite existing TGAs")
    ap.add_argument("--verify", action="store_true",
                    help="Byte-compare against existing TGAs; write nothing")
    args = ap.parse_args()

    original_dir = os.path.abspath(args.original)
    assets_dir   = os.path.abspath(args.assets)

    if not os.path.isdir(original_dir):
        print("ERROR: original/ not found at {}".format(original_dir))
        sys.exit(1)

    if not args.verify:
        os.makedirs(assets_dir, exist_ok=True)

    stats = {"wrote": 0, "skipped": 0, "unsafe": 0,
             "match": 0, "differ": 0, "missing": 0}

    for zip_path, dest in iter_archive_jobs(original_dir):
        extract_archive(zip_path, dest, assets_dir,
                        force=args.force, verify=args.verify, stats=stats)

    if args.verify:
        print("\nverify: match={match} differ={differ} missing={missing} unsafe={unsafe}".format(**stats))
        sys.exit(0 if stats["differ"] == 0 and stats["missing"] == 0 else 2)
    else:
        print("\nextract: wrote={wrote} skipped={skipped} unsafe={unsafe}".format(**stats))


if __name__ == "__main__":
    main()
