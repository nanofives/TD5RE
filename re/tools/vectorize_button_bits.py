#!/usr/bin/env python3
"""
vectorize_button_bits.py - Vectorize ButtonBits.png for resolution-independent rendering.

Reads the original 56x100 ButtonBits.png, traces the button shapes,
and renders a scaled version at any target resolution using PIL/Pillow
with antialiased drawing.

Usage:
    python vectorize_button_bits.py [scale_factor]
    python vectorize_button_bits.py 2    # 2x = 112x200
    python vectorize_button_bits.py 4    # 4x = 224x400
"""

import sys
from PIL import Image, ImageDraw, ImageFilter
import math

def load_and_analyze(path):
    """Load the original ButtonBits.png and extract color/shape data."""
    img = Image.open(path).convert('RGBA')
    print(f"Original: {img.size[0]}x{img.size[1]}")
    return img

def trace_outline(img, y_start, y_end):
    """Trace the left and right edges of content in a row range."""
    edges = []
    for y in range(y_start, y_end):
        left = None
        right = None
        for x in range(img.width):
            r, g, b, a = img.getpixel((x, y))
            if r > 5 or g > 5 or b > 5:
                if left is None:
                    left = x
                right = x
        edges.append((left, right))
    return edges

def get_dominant_colors(img, y_start, y_end):
    """Extract the key colors from a button section."""
    colors = {}
    for y in range(y_start, y_end):
        for x in range(img.width):
            r, g, b, a = img.getpixel((x, y))
            if r < 5 and g < 5 and b < 5:
                continue
            key = (r, g, b)
            colors[key] = colors.get(key, 0) + 1

    sorted_colors = sorted(colors.items(), key=lambda x: -x[1])
    return sorted_colors[:10]

def render_button_section(draw, scale, y_offset, style, w, h):
    """Render one button style section (32 rows at original scale).

    style: 'gold' or 'blue'
    """
    sw = w  # scaled width
    sh = h  # scaled section height (32 * scale)

    if style == 'gold':
        # Gold/yellow highlight button
        fill_color = (57, 33, 82, 255)       # Purple fill
        border_outer = (107, 90, 0, 255)      # Dark gold
        border_inner = (214, 198, 0, 255)     # Bright gold/yellow
        border_mid = (156, 140, 0, 255)       # Mid gold
    else:
        # Blue normal button
        fill_color = (0, 0, 0, 0)             # Transparent fill
        border_outer = (0, 24, 123, 255)      # Dark blue
        border_inner = (140, 165, 255, 255)   # Bright blue/white
        border_mid = (66, 82, 200, 255)       # Mid blue

    # Original button geometry (at 56x32):
    # - Corner radius: ~10px
    # - Border width: ~3px with gradient
    # - Top edge: rows 0-5 (6px), corners rows 6-13 (8px)
    # - Center: rows 14-18 (edges only)
    # - Bottom corners: rows 19-29, bottom edge: rows 30-31

    corner_r = int(10 * scale)
    border_w = max(2, int(3 * scale))

    # The atlas is split left (0-25) | center (26-27) | right (28-55)
    # Left piece width = 26, center = 2 (tiled), right piece = 28
    left_w = int(26 * scale)
    center_w = int(4 * scale)  # center column (tiled section)
    right_w = sw - left_w - center_w

    y0 = y_offset

    # Draw the full button shape as a rounded rectangle
    # Top-left corner, top edge, top-right corner
    # Left edge, fill, right edge
    # Bottom-left corner, bottom edge, bottom-right corner

    margin = int(1 * scale)

    if style == 'gold':
        # Fill the entire section with purple
        draw.rectangle([0, y0, sw-1, y0+sh-1], fill=fill_color)

    # Draw rounded rectangle border with gradient
    # Outer border
    draw.rounded_rectangle(
        [margin, y0 + margin, sw - margin - 1, y0 + sh - margin - 1],
        radius=corner_r,
        outline=border_outer,
        width=border_w
    )

    # Inner border (slightly smaller, brighter)
    inner_margin = margin + max(1, border_w // 3)
    inner_r = max(1, corner_r - border_w // 3)
    draw.rounded_rectangle(
        [inner_margin, y0 + inner_margin, sw - inner_margin - 1, y0 + sh - inner_margin - 1],
        radius=inner_r,
        outline=border_inner,
        width=max(1, border_w // 2)
    )

def render_edge_pieces(draw, scale, y_offset, style, w):
    """Render the edge/center tiling pieces (4 rows at original scale)."""
    sh = int(4 * scale)
    y0 = y_offset

    if style == 'gold':
        border_color = (214, 198, 0, 255)
        fill_color = (57, 33, 82, 255)
    else:
        border_color = (90, 123, 255, 255)
        fill_color = (0, 0, 0, 0)

    border_w = max(1, int(2 * scale))

    if style == 'gold':
        draw.rectangle([0, y0, w-1, y0+sh-1], fill=fill_color)

    # Left border strip
    draw.rectangle([0, y0, border_w-1, y0+sh-1], fill=border_color)
    # Right border strip
    draw.rectangle([w-border_w, y0, w-1, y0+sh-1], fill=border_color)

def render_accent_pieces(draw, scale, y_offset, w):
    """Render the small accent/arrow pieces at the bottom (4 rows)."""
    sh = int(4 * scale)
    y0 = y_offset

    # Row 96-97: purple fill with yellow and white accents
    # Row 98-99: purple fill with blue and white accents
    fill = (57, 33, 82, 255)
    draw.rectangle([0, y0, w-1, y0+sh-1], fill=fill)

    # Yellow accent marks
    yw = max(1, int(4 * scale))
    yx = int(22 * scale)
    draw.rectangle([yx, y0, yx+yw-1, y0 + int(2*scale) - 1], fill=(214, 198, 0, 255))

    # White accent marks
    ww = max(1, int(4 * scale))
    wx1 = int(32 * scale)
    wx2 = int(44 * scale)
    draw.rectangle([wx1, y0, wx1+ww-1, y0 + int(2*scale) - 1], fill=(255, 255, 255, 255))
    draw.rectangle([wx2, y0, wx2+ww-1, y0 + int(2*scale) - 1], fill=(255, 255, 255, 255))

def generate_scaled_button_bits(original_path, output_path, scale):
    """Generate a scaled ButtonBits.png with vector-quality rendering."""
    orig = load_and_analyze(original_path)

    sw = orig.width * scale
    sh = orig.height * scale

    # Create output image with transparency
    out = Image.new('RGBA', (sw, sh), (0, 0, 0, 0))
    draw = ImageDraw.Draw(out)

    section_h = int(32 * scale)  # Each button section is 32 rows
    edge_h = int(4 * scale)      # Edge pieces are 4 rows
    accent_h = int(4 * scale)    # Accent pieces are 4 rows

    # Section layout (original rows):
    # 0-31:  Gold highlight (top-corner 0-5, corner-curve 6-13, edges 14-18, corner-curve 19-29, bottom 30-31)
    # 32-35: Blue edge pieces
    # 36-63: Blue normal button (same structure as gold)
    # 64-67: Blue edge pieces (duplicate)
    # 68-95: Blue normal button (duplicate)
    # 96-99: Accent pieces

    # Render gold highlight section (rows 0-31)
    render_button_section(draw, scale, 0, 'gold', sw, section_h)

    # Render blue edge pieces (rows 32-35)
    render_edge_pieces(draw, scale, section_h, 'blue', sw)

    # Render blue normal section (rows 36-63) - actually rows 32-63 is 32 rows
    # Wait, looking at the data again: rows 32-35 are edge pieces, 36-63 is the blue button
    # Let me recalculate: section is 32 rows but includes edge pieces
    # Actually the full section for each style is:
    # Gold: rows 0-31 (32 rows total including corners + edges)
    # Blue: rows 32-63 (32 rows: 4 edge + 28 button shape)
    # Blue duplicate: rows 64-95
    # Accents: rows 96-99

    # Render blue normal section (rows 32-63)
    y2 = section_h
    render_button_section(draw, scale, y2, 'blue', sw, section_h)

    # Render blue normal section duplicate (rows 64-95)
    y3 = section_h * 2
    render_button_section(draw, scale, y3, 'blue', sw, section_h)

    # Render accent pieces (rows 96-99)
    y4 = section_h * 3
    render_accent_pieces(draw, scale, y4, sw)

    # Apply slight gaussian blur for antialiasing
    # out = out.filter(ImageFilter.GaussianBlur(radius=0.5))

    out.save(output_path)
    print(f"Generated: {output_path} ({sw}x{sh}) at {scale}x scale")
    return out

def main():
    scale = int(sys.argv[1]) if len(sys.argv) > 1 else 2

    base_dir = "D:/Descargas/Test Drive 5 ISO"
    original = f"{base_dir}/data/td5_png_clean/frontend/ButtonBits.png"
    output = f"{base_dir}/data/assets/frontend/ButtonBits_x{scale}.png"

    # Also generate a preview at the original path for comparison
    preview = f"{base_dir}/re/tools/ButtonBits_vectorized_x{scale}.png"

    generate_scaled_button_bits(original, preview, scale)
    print(f"\nPreview saved to: {preview}")
    print(f"To use in game, copy to: {base_dir}/data/assets/frontend/ButtonBits.tga")
    print(f"(or enable PNG replace pipeline)")

if __name__ == '__main__':
    main()
