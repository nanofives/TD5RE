#!/usr/bin/env python3
"""
strip_viewer.py -- Test Drive 5 STRIP.DAT track geometry parser and visualizer.

Parses the binary STRIP.DAT format used by TD5's track engine, which stores
track geometry as a ring of 24-byte span records with associated vertex tables.

Format reference:
  - BindTrackStripRuntimePointers (0x444070) in TD5_d3d.exe
  - re/analysis/ai-routing-and-track-geometry.md
  - re/analysis/level-zip-file-formats.md

Usage:
  python strip_viewer.py parse   <level.zip|strip.dat> [--limit N]
  python strip_viewer.py ascii   <level.zip|strip.dat> [--width W] [--height H]
  python strip_viewer.py svg     <level.zip|strip.dat> [--out FILE] [--width W] [--height H] [--route]
  python strip_viewer.py stats   <level.zip|strip.dat>
  python strip_viewer.py route   <level.zip|strip.dat> [--left FILE] [--right FILE]
"""

import argparse
import math
import os
import struct
import sys
import zipfile
from collections import Counter
from dataclasses import dataclass, field
from typing import Optional


# ---------------------------------------------------------------------------
# Data structures
# ---------------------------------------------------------------------------

@dataclass
class Vertex:
    """6-byte relative vertex: 3x int16 offset from span origin."""
    x: int  # int16
    y: int  # int16
    z: int  # int16


@dataclass
class SpanRecord:
    """24-byte span record from STRIP.DAT."""
    index: int           # span index (not stored in record, but tracked)
    span_type: int       # +0x00: byte, 0-11
    attribute: int       # +0x01: byte, strip attribute
    byte2: int           # +0x02: byte (padding / flags)
    packed_subspan: int  # +0x03: byte, low nibble = sub-span count, high = height offset
    left_vi: int         # +0x04: uint16, left vertex index
    right_vi: int        # +0x06: uint16, right vertex index
    fwd_link: int        # +0x08: int16, forward link span index
    bwd_link: int        # +0x0A: int16, backward link span index
    origin_x: int        # +0x0C: int32, world X
    origin_y: int        # +0x10: int32, world Y
    origin_z: int        # +0x14: int32, world Z

    @property
    def subspan_count(self) -> int:
        return self.packed_subspan & 0x0F

    @property
    def height_offset(self) -> int:
        return (self.packed_subspan >> 4) & 0x0F

    def world_left(self, vertices: list) -> tuple:
        """Absolute world position of left vertex (float)."""
        v = vertices[self.left_vi]
        return (
            (self.origin_x + v.x * 256) / 256.0,
            (self.origin_y + v.y * 256) / 256.0,
            (self.origin_z + v.z * 256) / 256.0,
        )

    def world_right(self, vertices: list) -> tuple:
        """Absolute world position of right vertex (float)."""
        v = vertices[self.right_vi]
        return (
            (self.origin_x + v.x * 256) / 256.0,
            (self.origin_y + v.y * 256) / 256.0,
            (self.origin_z + v.z * 256) / 256.0,
        )


@dataclass
class RouteData:
    """LEFT.TRK or RIGHT.TRK: 3 bytes per span."""
    offset: list   # byte per span: lateral offset 0-255 (left-to-right)
    heading: list  # byte per span: heading 0-255
    flags: list    # byte per span


@dataclass
class StripData:
    """Parsed STRIP.DAT contents."""
    span_count: int
    secondary_count: int
    auxiliary_count: int
    spans: list           # list[SpanRecord]
    vertices: list        # list[Vertex]
    raw_header: tuple     # 5 dwords
    left_route: Optional[RouteData] = None
    right_route: Optional[RouteData] = None


# ---------------------------------------------------------------------------
# STRIP.DAT span type descriptions
# ---------------------------------------------------------------------------

SPAN_TYPE_NAMES = {
    0: "unused/default",
    1: "standard-quad",
    2: "standard-quad",
    3: "alt-diagonal",
    4: "alt-diagonal",
    5: "standard-quad",
    6: "reversed-winding",
    7: "reversed-winding",
    8: "forward-junction",
    9: "backward-sentinel",
    10: "forward-sentinel",
    11: "backward-junction",
}


# ---------------------------------------------------------------------------
# Parsing
# ---------------------------------------------------------------------------

def parse_strip_dat(data: bytes) -> StripData:
    """Parse a STRIP.DAT binary blob into structured data."""
    if len(data) < 20:
        raise ValueError(f"STRIP.DAT too small: {len(data)} bytes (need >= 20 for header)")

    # Header: 5 x uint32
    hdr = struct.unpack_from('<5I', data, 0)
    span_offset = hdr[0]
    span_count = hdr[1]
    vertex_offset = hdr[2]
    secondary_count = hdr[3]
    auxiliary_count = hdr[4]

    # Parse span records (24 bytes each)
    spans = []
    for i in range(span_count):
        off = span_offset + i * 24
        if off + 24 > len(data):
            print(f"WARNING: span {i} at offset {off} exceeds file size {len(data)}, truncating", file=sys.stderr)
            break
        (stype, attr, b2, packed,
         lvi, rvi, fwd, bwd,
         ox, oy, oz) = struct.unpack_from('<BBBBHHhhiii', data, off)
        spans.append(SpanRecord(
            index=i, span_type=stype, attribute=attr, byte2=b2,
            packed_subspan=packed, left_vi=lvi, right_vi=rvi,
            fwd_link=fwd, bwd_link=bwd,
            origin_x=ox, origin_y=oy, origin_z=oz
        ))

    # Parse vertex table (6 bytes each = 3x int16)
    # Vertex count: from vertex_offset to end of file (or up to max index used)
    vertex_data_len = len(data) - vertex_offset
    vertex_count = vertex_data_len // 6
    vertices = []
    for i in range(vertex_count):
        voff = vertex_offset + i * 6
        vx, vy, vz = struct.unpack_from('<hhh', data, voff)
        vertices.append(Vertex(vx, vy, vz))

    return StripData(
        span_count=span_count,
        secondary_count=secondary_count,
        auxiliary_count=auxiliary_count,
        spans=spans,
        vertices=vertices,
        raw_header=hdr,
    )


def parse_route(data: bytes, span_count: int) -> RouteData:
    """Parse a LEFT.TRK or RIGHT.TRK file (3 bytes per span)."""
    expected = span_count * 3
    if len(data) < expected:
        print(f"WARNING: route file {len(data)} bytes, expected {expected} (3 * {span_count})", file=sys.stderr)
    offsets = []
    headings = []
    flags = []
    for i in range(min(span_count, len(data) // 3)):
        base = i * 3
        offsets.append(data[base])
        headings.append(data[base + 1])
        flags.append(data[base + 2])
    return RouteData(offset=offsets, heading=headings, flags=flags)


def load_from_zip(zip_path: str) -> StripData:
    """Load STRIP.DAT + optional route files from a level ZIP."""
    with zipfile.ZipFile(zip_path) as z:
        names_lower = {n.lower(): n for n in z.namelist()}

        # Find STRIP.DAT (case-insensitive)
        strip_name = None
        for candidate in ['strip.dat', 'STRIP.DAT']:
            if candidate.lower() in names_lower:
                strip_name = names_lower[candidate.lower()]
                break
        if strip_name is None:
            raise FileNotFoundError("No STRIP.DAT found in ZIP. Available: " + ", ".join(z.namelist()))

        strip_data = parse_strip_dat(z.read(strip_name))

        # Try to load route files
        for route_name, attr in [('left.trk', 'left_route'), ('right.trk', 'right_route')]:
            if route_name in names_lower:
                rd = parse_route(z.read(names_lower[route_name]), strip_data.span_count)
                setattr(strip_data, attr, rd)

        return strip_data


def load_input(path: str) -> StripData:
    """Load from either a ZIP or a raw STRIP.DAT file."""
    if path.lower().endswith('.zip'):
        return load_from_zip(path)
    else:
        with open(path, 'rb') as f:
            data = f.read()
        return parse_strip_dat(data)


# ---------------------------------------------------------------------------
# Commands
# ---------------------------------------------------------------------------

def cmd_parse(strip: StripData, args):
    """Print parsed span records."""
    limit = args.limit or strip.span_count
    print(f"STRIP.DAT: {strip.span_count} spans, {len(strip.vertices)} vertices")
    print(f"Header: {' '.join(hex(x) for x in strip.raw_header)}")
    print()
    fmt = "{:>5s} {:>4s} {:>4s} {:>3s} {:>4s} {:>6s} {:>6s} {:>5s} {:>5s} {:>12s} {:>12s} {:>12s}  {}"
    print(fmt.format("Span", "Type", "Attr", "Sub", "HOff", "L.VI", "R.VI", "Fwd", "Bwd",
                      "Origin.X", "Origin.Y", "Origin.Z", "TypeName"))
    print("-" * 120)
    for span in strip.spans[:limit]:
        print(fmt.format(
            str(span.index),
            str(span.span_type),
            str(span.attribute),
            str(span.subspan_count),
            str(span.height_offset),
            str(span.left_vi),
            str(span.right_vi),
            str(span.fwd_link),
            str(span.bwd_link),
            str(span.origin_x),
            str(span.origin_y),
            str(span.origin_z),
            SPAN_TYPE_NAMES.get(span.span_type, "unknown"),
        ))
    if limit < strip.span_count:
        print(f"... ({strip.span_count - limit} more spans)")


def cmd_stats(strip: StripData, args):
    """Print track statistics."""
    print(f"=== STRIP.DAT Statistics ===")
    print(f"Total spans:      {strip.span_count}")
    print(f"Total vertices:   {len(strip.vertices)}")
    print(f"Secondary count:  {strip.secondary_count}")
    print(f"Auxiliary count:  {strip.auxiliary_count}")
    print()

    # Type distribution
    type_counts = Counter(s.span_type for s in strip.spans)
    print("Span type distribution:")
    for t in sorted(type_counts.keys()):
        name = SPAN_TYPE_NAMES.get(t, "unknown")
        count = type_counts[t]
        pct = 100.0 * count / strip.span_count
        bar = "#" * int(pct / 2)
        print(f"  Type {t:2d} ({name:20s}): {count:5d} ({pct:5.1f}%) {bar}")
    print()

    # Junction count
    junction_types = {8, 9, 10, 11}
    junctions = sum(1 for s in strip.spans if s.span_type in junction_types)
    print(f"Junction spans:   {junctions}")

    # Attribute distribution
    attr_counts = Counter(s.attribute for s in strip.spans)
    print(f"Unique attributes: {len(attr_counts)}")
    for a in sorted(attr_counts.keys()):
        print(f"  Attr 0x{a:02X}: {attr_counts[a]:5d} spans")
    print()

    # Sub-span distribution
    sub_counts = Counter(s.subspan_count for s in strip.spans)
    print("Sub-span count distribution:")
    for sc in sorted(sub_counts.keys()):
        print(f"  {sc} sub-spans: {sub_counts[sc]:5d} spans")
    print()

    # Track length estimate (sum of distances between consecutive span origins)
    total_dist = 0.0
    for i in range(1, len(strip.spans)):
        dx = strip.spans[i].origin_x - strip.spans[i - 1].origin_x
        dz = strip.spans[i].origin_z - strip.spans[i - 1].origin_z
        total_dist += math.sqrt(dx * dx + dz * dz)
    print(f"Approx track length (origin-to-origin): {total_dist:.0f} units")
    # Convert: game uses 256 units per world unit in some contexts
    # Origins are in raw int32; vertices are int16 offsets scaled by 256
    # Just report raw units
    print()

    # Bounding box
    if strip.spans:
        min_x = min(s.origin_x for s in strip.spans)
        max_x = max(s.origin_x for s in strip.spans)
        min_z = min(s.origin_z for s in strip.spans)
        max_z = max(s.origin_z for s in strip.spans)
        min_y = min(s.origin_y for s in strip.spans)
        max_y = max(s.origin_y for s in strip.spans)
        print(f"Bounding box (origins):")
        print(f"  X: {min_x} .. {max_x}  (range {max_x - min_x})")
        print(f"  Y: {min_y} .. {max_y}  (range {max_y - min_y})")
        print(f"  Z: {min_z} .. {max_z}  (range {max_z - min_z})")
    print()

    # Route info
    if strip.left_route:
        print(f"LEFT.TRK:  {len(strip.left_route.offset)} entries, "
              f"offset range [{min(strip.left_route.offset)}-{max(strip.left_route.offset)}]")
    else:
        print("LEFT.TRK:  not loaded")
    if strip.right_route:
        print(f"RIGHT.TRK: {len(strip.right_route.offset)} entries, "
              f"offset range [{min(strip.right_route.offset)}-{max(strip.right_route.offset)}]")
    else:
        print("RIGHT.TRK: not loaded")


def _compute_track_points(strip: StripData):
    """Compute left/right/center world XZ points for all spans."""
    lefts = []
    rights = []
    centers = []
    for span in strip.spans:
        if span.left_vi < len(strip.vertices) and span.right_vi < len(strip.vertices):
            lw = span.world_left(strip.vertices)
            rw = span.world_right(strip.vertices)
            lefts.append((lw[0], lw[2]))
            rights.append((rw[0], rw[2]))
            centers.append(((lw[0] + rw[0]) / 2, (lw[2] + rw[2]) / 2))
        else:
            # Fallback to origin only
            ox, oz = float(span.origin_x), float(span.origin_z)
            lefts.append((ox, oz))
            rights.append((ox, oz))
            centers.append((ox, oz))
    return lefts, rights, centers


def _compute_route_points(strip: StripData, route: RouteData):
    """Compute world XZ points for a route overlay.

    Route offset byte 0-255 interpolates between left (0) and right (255) vertices.
    """
    points = []
    for i, span in enumerate(strip.spans):
        if i >= len(route.offset):
            break
        t = route.offset[i] / 255.0
        if span.left_vi < len(strip.vertices) and span.right_vi < len(strip.vertices):
            lw = span.world_left(strip.vertices)
            rw = span.world_right(strip.vertices)
            px = lw[0] + t * (rw[0] - lw[0])
            pz = lw[2] + t * (rw[2] - lw[2])
            points.append((px, pz))
        else:
            points.append((float(span.origin_x), float(span.origin_z)))
    return points


def cmd_ascii(strip: StripData, args):
    """Render an ASCII top-down view of the track."""
    width = args.width or 120
    height = args.height or 50

    lefts, rights, centers = _compute_track_points(strip)
    all_pts = lefts + rights

    if not all_pts:
        print("No points to render.")
        return

    xs = [p[0] for p in all_pts]
    zs = [p[1] for p in all_pts]
    min_x, max_x = min(xs), max(xs)
    min_z, max_z = min(zs), max(zs)

    range_x = max_x - min_x or 1.0
    range_z = max_z - min_z or 1.0

    # Build character grid
    grid = [[' '] * width for _ in range(height)]

    def plot(px, pz, ch):
        col = int((px - min_x) / range_x * (width - 1))
        row = int((pz - min_z) / range_z * (height - 1))
        col = max(0, min(width - 1, col))
        row = max(0, min(height - 1, row))
        # Flip row so Z+ is up
        row = height - 1 - row
        if grid[row][col] == ' ':
            grid[row][col] = ch

    # Plot left edge
    for p in lefts:
        plot(p[0], p[1], '.')

    # Plot right edge
    for p in rights:
        plot(p[0], p[1], '.')

    # Plot center line
    for p in centers:
        plot(p[0], p[1], '-')

    # Mark start
    if centers:
        plot(centers[0][0], centers[0][1], 'S')

    # Mark junctions
    for span in strip.spans:
        if span.span_type in (8, 9, 10, 11):
            if span.left_vi < len(strip.vertices) and span.right_vi < len(strip.vertices):
                lw = span.world_left(strip.vertices)
                rw = span.world_right(strip.vertices)
                cx = (lw[0] + rw[0]) / 2
                cz = (lw[2] + rw[2]) / 2
                plot(cx, cz, 'J')

    # Route overlay
    if strip.left_route:
        route_pts = _compute_route_points(strip, strip.left_route)
        for p in route_pts:
            plot(p[0], p[1], 'L')
    if strip.right_route:
        route_pts = _compute_route_points(strip, strip.right_route)
        for p in route_pts:
            plot(p[0], p[1], 'R')

    # Print
    print(f"Track top-down view ({strip.span_count} spans, {width}x{height} chars)")
    print(f"X: {min_x:.0f}..{max_x:.0f}  Z: {min_z:.0f}..{max_z:.0f}")
    print("Legend: S=start, .=edge, -=center, J=junction, L=left route, R=right route")
    border = '+' + '-' * width + '+'
    print(border)
    for row in grid:
        print('|' + ''.join(row) + '|')
    print(border)


def _escape_svg(s):
    return s.replace('&', '&amp;').replace('<', '&lt;').replace('>', '&gt;').replace('"', '&quot;')


def cmd_svg(strip: StripData, args):
    """Generate an SVG image of the track layout."""
    svg_width = args.width or 1200
    svg_height = args.height or 900
    out_path = args.out or "track.svg"
    show_route = args.route

    lefts, rights, centers = _compute_track_points(strip)
    all_pts = lefts + rights

    if not all_pts:
        print("No points to render.")
        return

    xs = [p[0] for p in all_pts]
    zs = [p[1] for p in all_pts]
    min_x, max_x = min(xs), max(xs)
    min_z, max_z = min(zs), max(zs)

    range_x = max_x - min_x or 1.0
    range_z = max_z - min_z or 1.0

    margin = 40
    draw_w = svg_width - 2 * margin
    draw_h = svg_height - 2 * margin

    # Maintain aspect ratio
    scale = min(draw_w / range_x, draw_h / range_z)

    def tx(wx):
        return margin + (wx - min_x) * scale

    def tz(wz):
        # Flip Z so Z+ is up in SVG (SVG Y goes down)
        return margin + (max_z - wz) * scale

    lines = []
    lines.append(f'<?xml version="1.0" encoding="UTF-8"?>')
    actual_w = int(2 * margin + range_x * scale)
    actual_h = int(2 * margin + range_z * scale)
    lines.append(f'<svg xmlns="http://www.w3.org/2000/svg" width="{actual_w}" height="{actual_h}" '
                 f'viewBox="0 0 {actual_w} {actual_h}">')
    lines.append(f'<rect width="100%" height="100%" fill="#1a1a2e"/>')

    # Title
    input_name = os.path.basename(args.input)
    lines.append(f'<text x="{actual_w//2}" y="20" text-anchor="middle" fill="#ccc" '
                 f'font-family="monospace" font-size="14">'
                 f'TD5 Track: {_escape_svg(input_name)} ({strip.span_count} spans)</text>')

    # Build track surface as a filled polygon (left edge forward + right edge backward)
    left_path = ' '.join(f'{tx(p[0]):.1f},{tz(p[1]):.1f}' for p in lefts)
    right_path = ' '.join(f'{tx(p[0]):.1f},{tz(p[1]):.1f}' for p in reversed(rights))
    lines.append(f'<polygon points="{left_path} {right_path}" '
                 f'fill="#2a2a4a" stroke="none" opacity="0.6"/>')

    # Left edge polyline
    lp = ' '.join(f'{tx(p[0]):.1f},{tz(p[1]):.1f}' for p in lefts)
    lines.append(f'<polyline points="{lp}" fill="none" stroke="#4488ff" stroke-width="1" opacity="0.8"/>')

    # Right edge polyline
    rp = ' '.join(f'{tx(p[0]):.1f},{tz(p[1]):.1f}' for p in rights)
    lines.append(f'<polyline points="{rp}" fill="none" stroke="#4488ff" stroke-width="1" opacity="0.8"/>')

    # Center line (dashed)
    cp = ' '.join(f'{tx(p[0]):.1f},{tz(p[1]):.1f}' for p in centers)
    lines.append(f'<polyline points="{cp}" fill="none" stroke="#888" stroke-width="0.5" '
                 f'stroke-dasharray="4,4" opacity="0.5"/>')

    # Junction markers
    for span in strip.spans:
        if span.span_type in (8, 9, 10, 11):
            if span.left_vi < len(strip.vertices) and span.right_vi < len(strip.vertices):
                lw = span.world_left(strip.vertices)
                rw = span.world_right(strip.vertices)
                cx = (lw[0] + rw[0]) / 2
                cz = (lw[2] + rw[2]) / 2
                lines.append(f'<circle cx="{tx(cx):.1f}" cy="{tz(cz):.1f}" r="3" '
                             f'fill="#ff4444" opacity="0.8"/>')

    # Start marker
    if centers:
        sx, sz = centers[0]
        lines.append(f'<circle cx="{tx(sx):.1f}" cy="{tz(sz):.1f}" r="6" '
                     f'fill="#00ff88" stroke="#fff" stroke-width="1.5"/>')
        lines.append(f'<text x="{tx(sx) + 10:.1f}" y="{tz(sz) + 4:.1f}" fill="#00ff88" '
                     f'font-family="monospace" font-size="11">START</text>')

    # Route overlays
    if show_route:
        if strip.left_route:
            route_pts = _compute_route_points(strip, strip.left_route)
            rtp = ' '.join(f'{tx(p[0]):.1f},{tz(p[1]):.1f}' for p in route_pts)
            lines.append(f'<polyline points="{rtp}" fill="none" stroke="#ffaa00" '
                         f'stroke-width="1.5" opacity="0.7"/>')
            lines.append(f'<text x="{actual_w - margin}" y="{actual_h - 30}" text-anchor="end" '
                         f'fill="#ffaa00" font-family="monospace" font-size="10">LEFT.TRK</text>')
        if strip.right_route:
            route_pts = _compute_route_points(strip, strip.right_route)
            rtp = ' '.join(f'{tx(p[0]):.1f},{tz(p[1]):.1f}' for p in route_pts)
            lines.append(f'<polyline points="{rtp}" fill="none" stroke="#ff44ff" '
                         f'stroke-width="1.5" opacity="0.7"/>')
            lines.append(f'<text x="{actual_w - margin}" y="{actual_h - 16}" text-anchor="end" '
                         f'fill="#ff44ff" font-family="monospace" font-size="10">RIGHT.TRK</text>')

    # Legend
    ly = actual_h - 14
    lines.append(f'<text x="{margin}" y="{ly}" fill="#666" font-family="monospace" font-size="9">'
                 f'Blue=track edge  Gray=center  Red=junction  Green=start</text>')

    # Span index markers every N spans
    marker_interval = max(1, strip.span_count // 20)
    for i in range(0, len(centers), marker_interval):
        px, pz = centers[i]
        lines.append(f'<circle cx="{tx(px):.1f}" cy="{tz(pz):.1f}" r="2" fill="#aaa" opacity="0.6"/>')
        lines.append(f'<text x="{tx(px) + 4:.1f}" y="{tz(pz) - 4:.1f}" fill="#aaa" '
                     f'font-family="monospace" font-size="8" opacity="0.6">{i}</text>')

    lines.append('</svg>')

    with open(out_path, 'w') as f:
        f.write('\n'.join(lines))
    print(f"SVG written to {out_path} ({actual_w}x{actual_h}px, {strip.span_count} spans)")


def cmd_route(strip: StripData, args):
    """Display AI route overlay data."""
    # Load external route files if provided
    if args.left:
        with open(args.left, 'rb') as f:
            strip.left_route = parse_route(f.read(), strip.span_count)
    if args.right:
        with open(args.right, 'rb') as f:
            strip.right_route = parse_route(f.read(), strip.span_count)

    if not strip.left_route and not strip.right_route:
        print("No route data available. Provide --left / --right or use a level ZIP that contains them.")
        return

    print(f"=== AI Route Data ({strip.span_count} spans) ===")
    print()

    for label, route in [("LEFT", strip.left_route), ("RIGHT", strip.right_route)]:
        if route is None:
            print(f"{label}.TRK: not loaded")
            continue
        print(f"{label}.TRK: {len(route.offset)} entries")
        print(f"  Offset range:  {min(route.offset):3d} - {max(route.offset):3d} "
              f"(mean {sum(route.offset)/len(route.offset):.1f})")
        print(f"  Heading range: {min(route.heading):3d} - {max(route.heading):3d} "
              f"(mean {sum(route.heading)/len(route.heading):.1f})")
        flag_counts = Counter(route.flags)
        print(f"  Flag values:   {dict(sorted(flag_counts.items()))}")
        print()

    # Print per-span detail (first 40 + last 10)
    show_count = min(40, strip.span_count)
    fmt = "{:>5s}  {:>6s} {:>7s} {:>5s}  {:>6s} {:>7s} {:>5s}"
    print(fmt.format("Span", "L.Off", "L.Hdg", "L.Flg", "R.Off", "R.Hdg", "R.Flg"))
    print("-" * 55)

    def row(i):
        lo = strip.left_route.offset[i] if strip.left_route and i < len(strip.left_route.offset) else -1
        lh = strip.left_route.heading[i] if strip.left_route and i < len(strip.left_route.heading) else -1
        lf = strip.left_route.flags[i] if strip.left_route and i < len(strip.left_route.flags) else -1
        ro = strip.right_route.offset[i] if strip.right_route and i < len(strip.right_route.offset) else -1
        rh = strip.right_route.heading[i] if strip.right_route and i < len(strip.right_route.heading) else -1
        rf = strip.right_route.flags[i] if strip.right_route and i < len(strip.right_route.flags) else -1
        print(fmt.format(
            str(i),
            str(lo) if lo >= 0 else "-",
            str(lh) if lh >= 0 else "-",
            f"0x{lf:02X}" if lf >= 0 else "-",
            str(ro) if ro >= 0 else "-",
            str(rh) if rh >= 0 else "-",
            f"0x{rf:02X}" if rf >= 0 else "-",
        ))

    for i in range(show_count):
        row(i)

    if strip.span_count > 50:
        print(f"  ... ({strip.span_count - 50} more spans) ...")
        for i in range(max(show_count, strip.span_count - 10), strip.span_count):
            row(i)


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(
        description="Test Drive 5 STRIP.DAT track geometry parser and visualizer",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    sub = parser.add_subparsers(dest='command', required=True)

    # parse
    p_parse = sub.add_parser('parse', help='List all span records')
    p_parse.add_argument('input', help='Level ZIP or raw STRIP.DAT file')
    p_parse.add_argument('--limit', type=int, default=None, help='Max spans to print')

    # ascii
    p_ascii = sub.add_parser('ascii', help='ASCII top-down visualization')
    p_ascii.add_argument('input', help='Level ZIP or raw STRIP.DAT file')
    p_ascii.add_argument('--width', type=int, default=120, help='Character width (default: 120)')
    p_ascii.add_argument('--height', type=int, default=50, help='Character height (default: 50)')

    # svg
    p_svg = sub.add_parser('svg', help='SVG track layout export')
    p_svg.add_argument('input', help='Level ZIP or raw STRIP.DAT file')
    p_svg.add_argument('--out', default='track.svg', help='Output SVG path (default: track.svg)')
    p_svg.add_argument('--width', type=int, default=1200, help='SVG width (default: 1200)')
    p_svg.add_argument('--height', type=int, default=900, help='SVG height (default: 900)')
    p_svg.add_argument('--route', action='store_true', help='Show AI route overlays')

    # stats
    p_stats = sub.add_parser('stats', help='Track statistics summary')
    p_stats.add_argument('input', help='Level ZIP or raw STRIP.DAT file')

    # route
    p_route = sub.add_parser('route', help='AI route data viewer')
    p_route.add_argument('input', help='Level ZIP or raw STRIP.DAT file')
    p_route.add_argument('--left', default=None, help='External LEFT.TRK file')
    p_route.add_argument('--right', default=None, help='External RIGHT.TRK file')

    args = parser.parse_args()
    strip = load_input(args.input)

    if args.command == 'parse':
        cmd_parse(strip, args)
    elif args.command == 'ascii':
        cmd_ascii(strip, args)
    elif args.command == 'svg':
        cmd_svg(strip, args)
    elif args.command == 'stats':
        cmd_stats(strip, args)
    elif args.command == 'route':
        cmd_route(strip, args)


if __name__ == '__main__':
    main()
