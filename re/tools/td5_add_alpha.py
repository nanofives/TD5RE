#!/usr/bin/env python3
"""
td5_add_alpha.py - Automatically add alpha channels to textures that need transparency.

Scans td5_png_clean/ and applies alpha rules based on texture category:

1. CARHUB*.png (cars/*/CARHUB*.png):
   - Black pixels (R=G=B=0) -> alpha 0 (transparent)
   - All other pixels -> alpha 255 (opaque)

2. Environs (environs/*.png):
   - Black pixels (R=G=B=0) -> alpha 0 (transparent)
   - All other pixels -> alpha 128 (semi-transparent, for reflections)

3. TEXTURES.DAT palette type 1 (levels/*/textures/tex_*.png):
   - Already handled by the converter (format_type in TEXTURES.DAT)
   - This script re-checks: if a texture was type 1, palette index 0 = transparent
   - Skipped here since converter already applies alpha.

4. Frontend/HUD textures with black backgrounds:
   - Detected heuristically: if >25% of border pixels are black, apply black=transparent

Only modifies PNGs that don't already have an alpha channel with non-255 values.

Usage:
  python td5_add_alpha.py --png-dir data/td5_png_clean
  python td5_add_alpha.py --png-dir data/td5_png_clean --dry-run
  python td5_add_alpha.py --png-dir data/td5_png_clean --threshold 5  (near-black tolerance)
"""

import argparse
import os
import sys

try:
    from PIL import Image
except ImportError:
    print("ERROR: Pillow required. pip install Pillow")
    sys.exit(1)


def already_has_alpha(img):
    """Check if image already has meaningful alpha (any pixel < 255)."""
    if img.mode != 'RGBA':
        return False
    alpha = img.split()[3]
    extrema = alpha.getextrema()
    return extrema[0] < 255  # min alpha < 255 means some transparency exists


def is_near_black(r, g, b, threshold=5):
    """Check if a pixel is near-black (within threshold of 0,0,0)."""
    return r <= threshold and g <= threshold and b <= threshold


def apply_black_transparent(img, threshold=5, alpha_value=255):
    """
    Apply black-is-transparent rule.
    Black pixels -> alpha 0, others -> alpha_value.
    Returns new RGBA image.
    """
    img = img.convert('RGBA')
    pixels = img.load()
    w, h = img.size
    changed = 0

    for y in range(h):
        for x in range(w):
            r, g, b, a = pixels[x, y]
            if is_near_black(r, g, b, threshold):
                if a != 0:
                    pixels[x, y] = (r, g, b, 0)
                    changed += 1
            else:
                if a != alpha_value:
                    pixels[x, y] = (r, g, b, alpha_value)
                    changed += 1

    return img, changed


def border_black_ratio(img, threshold=5):
    """Calculate what fraction of border pixels are near-black."""
    img_rgb = img.convert('RGB')
    pixels = img_rgb.load()
    w, h = img_rgb.size
    black_count = 0
    total = 0

    # Top and bottom rows
    for x in range(w):
        for y in [0, h - 1]:
            r, g, b = pixels[x, y]
            if is_near_black(r, g, b, threshold):
                black_count += 1
            total += 1

    # Left and right columns (excluding corners already counted)
    for y in range(1, h - 1):
        for x in [0, w - 1]:
            r, g, b = pixels[x, y]
            if is_near_black(r, g, b, threshold):
                black_count += 1
            total += 1

    return black_count / total if total > 0 else 0


def process_carhub(png_path, threshold, dry_run):
    """CARHUB: black -> transparent, others -> fully opaque."""
    img = Image.open(png_path)
    if already_has_alpha(img):
        return False, "already has alpha"

    img_alpha, changed = apply_black_transparent(img, threshold, alpha_value=255)
    if changed == 0:
        return False, "no black pixels"

    if not dry_run:
        img_alpha.save(png_path)
    return True, f"{changed} pixels"


def process_environ(png_path, threshold, dry_run):
    """Environs: black -> transparent, others -> semi-transparent (128)."""
    img = Image.open(png_path)
    if already_has_alpha(img):
        return False, "already has alpha"

    img_alpha, changed = apply_black_transparent(img, threshold, alpha_value=128)
    if changed == 0:
        return False, "no changes needed"

    if not dry_run:
        img_alpha.save(png_path)
    return True, f"{changed} pixels (semi-transparent)"


def process_frontend_heuristic(png_path, threshold, dry_run):
    """Frontend: if border is mostly black, apply black=transparent."""
    img = Image.open(png_path)
    if already_has_alpha(img):
        return False, "already has alpha"

    ratio = border_black_ratio(img, threshold)
    if ratio < 0.25:
        return False, f"border only {ratio:.0%} black"

    img_alpha, changed = apply_black_transparent(img, threshold, alpha_value=255)
    if changed == 0:
        return False, "no black pixels"

    if not dry_run:
        img_alpha.save(png_path)
    return True, f"{changed} pixels (border {ratio:.0%} black)"


def main():
    parser = argparse.ArgumentParser(
        description="Add alpha channels to TD5 textures that need transparency")
    parser.add_argument('--png-dir', required=True,
                        help="Path to td5_png_clean/ directory")
    parser.add_argument('--threshold', type=int, default=5,
                        help="Near-black tolerance (0-255, default 5)")
    parser.add_argument('--dry-run', action='store_true',
                        help="Show what would be modified without saving")
    parser.add_argument('--skip-frontend', action='store_true',
                        help="Skip heuristic frontend detection")
    args = parser.parse_args()

    png_dir = os.path.abspath(args.png_dir)
    if not os.path.isdir(png_dir):
        print(f"ERROR: directory not found: {png_dir}")
        sys.exit(1)

    print(f"PNG directory: {png_dir}")
    print(f"Near-black threshold: {args.threshold}")
    if args.dry_run:
        print("(DRY RUN)")
    print()

    modified = 0
    skipped = 0
    errors = 0

    # 1. CARHUB textures: black = transparent
    print("=== Car Hub Textures (black -> transparent) ===")
    for root, dirs, files in os.walk(os.path.join(png_dir, 'cars')):
        for fname in sorted(files):
            if not fname.upper().startswith('CARHUB') or not fname.lower().endswith('.png'):
                continue
            fpath = os.path.join(root, fname)
            rel = os.path.relpath(fpath, png_dir)
            try:
                ok, msg = process_carhub(fpath, args.threshold, args.dry_run)
                if ok:
                    print(f"  [ALPHA] {rel}: {msg}")
                    modified += 1
                else:
                    skipped += 1
            except Exception as e:
                print(f"  [ERROR] {rel}: {e}")
                errors += 1

    # 2. Environs: black = transparent, others = semi-transparent (128)
    print("\n=== Environment Textures (black -> transparent, color -> 50% alpha) ===")
    environs_dir = os.path.join(png_dir, 'environs')
    if os.path.isdir(environs_dir):
        for fname in sorted(os.listdir(environs_dir)):
            if not fname.lower().endswith('.png'):
                continue
            fpath = os.path.join(environs_dir, fname)
            rel = os.path.relpath(fpath, png_dir)
            try:
                ok, msg = process_environ(fpath, args.threshold, args.dry_run)
                if ok:
                    print(f"  [ALPHA] {rel}: {msg}")
                    modified += 1
                else:
                    skipped += 1
            except Exception as e:
                print(f"  [ERROR] {rel}: {e}")
                errors += 1

    # 3. Frontend textures (heuristic: mostly-black border = transparency)
    if not args.skip_frontend:
        print("\n=== Frontend Textures (heuristic black-border detection) ===")
        for subdir in ['frontend', 'extras', 'loading']:
            sub_path = os.path.join(png_dir, subdir)
            if not os.path.isdir(sub_path):
                continue
            for fname in sorted(os.listdir(sub_path)):
                if not fname.lower().endswith('.png'):
                    continue
                fpath = os.path.join(sub_path, fname)
                rel = os.path.relpath(fpath, png_dir)
                try:
                    ok, msg = process_frontend_heuristic(fpath, args.threshold, args.dry_run)
                    if ok:
                        print(f"  [ALPHA] {rel}: {msg}")
                        modified += 1
                    else:
                        skipped += 1
                except Exception as e:
                    print(f"  [ERROR] {rel}: {e}")
                    errors += 1

    print(f"\n=== Results ===")
    print(f"  {modified} PNGs {'would be ' if args.dry_run else ''}modified with alpha")
    print(f"  {skipped} PNGs skipped (no changes needed)")
    print(f"  {errors} errors")


if __name__ == '__main__':
    main()
