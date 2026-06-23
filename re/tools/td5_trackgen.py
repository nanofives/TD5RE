#!/usr/bin/env python3
"""td5_trackgen.py -- build a Test Drive 5 custom track from a centerline path.

This is the front-half + emitter of the custom-track pipeline.  It turns a
neutral, game-agnostic *centerline spec* (CSV or JSON -- the simplest shape any
other game's track reduces to) into the exact ``levelNNN/`` editable-source set
that the TD5RE engine loads:

    strip.json        collision / logic ribbon (spans + vertices)   [td5_src_encode_strip]
    left.trk.csv      forward AI route table (lane,heading,brake)    [build_routes]
    right.trk.csv     forward AI route table (right corridor)        [build_routes]
    levelinf.json     per-track params (circuit flag, checkpoints,   [td5_src_encode_levelinf]
                      weather, fog, traffic-enable, span count)
    checkpt.json      96-byte reverse page-swap record (minimal)     [td5_src_encode_checkpt]
    traffic.json      ambient-traffic spawn queue                    [td5_src_encode_traffic]

and registers the track in ``re/assets/levels/custom_tracks.json`` -- the runtime
manifest the engine's td5_track_registry module reads at boot, so the track
becomes selectable in the frontend and AutoRace-able with no recompile.

The geometry/format facts encoded here were derived empirically from native
TD5 tracks (see re/include/td5_level_formats.h and strip_tool.py):

  * STRIP.DAT header = 5 u32  [span_off, ring_len, vtx_off, vtx_count, total_spans].
  * pre-span jump-table region is 196 bytes (u32 jump_count + 32 reserved 6-byte
    entries); zero-filled when the track has no branches.
  * span = 24 bytes, fields [type,surface,lane_bitmask,packed(lanes|height<<4),
    lvi,rvi,link_next,link_prev,ox,oy,oz]; lvi = this span's NEAR row start,
    rvi = the FAR row start (forward edge).  A span owns a row of (lanes+1)
    vertices laid LEFT->RIGHT across the road width.
  * vertices = int16 (x,y,z) LOCAL offsets from the span origin.  Each generated
    span uses origin = its near centerline node, so offsets always fit int16 and
    seams match by construction (the engine tolerates non-shared seam indices --
    native tracks duplicate the seam row at every 32768-tile boundary).
  * lateral left direction = (tz,-tx) of the forward tangent (verified against
    Moscow span 0).
  * levelinf track_type @0x00: 1=circuit (lap counter), 0=point-to-point.

NEUTRAL SPEC SCHEMA (JSON)
--------------------------
{
  "name": "TEST OVAL",          # display name (uppercased for the menu)
  "circuit": true,               # true=closed loop, false=point-to-point
  "lane_width": 1500,            # world units per lane (default 1500, ~Moscow)
  "default_lanes": 4,            # lanes when a node doesn't override
  "default_surface": 0,          # 0 dry, 1 wet, 2 dirt, 3 gravel
  "nodes": [                     # ordered centerline; >=3 points (circuit) / >=2 (p2p)
     {"x": 0, "z": 0},           # x,z required; y (elevation) optional, default 0
     {"x": 0, "z": 6000, "lanes": 6, "width": 9000, "surface": 0},
     ...
  ],
  "checkpoints": "auto:4",       # "auto:N" evenly-spaced, or explicit [span,...]
  "weather": 2,                  # 0 rain, 1 snow(cut), 2 clear (default 2)
  "fog": {"enabled": 0, "r": 0, "g": 0, "b": 0},
  "smoke": 1,
  "traffic_enable": 0,
  "traffic": [ {"span": 100, "lane": 1, "oncoming": false}, ... ]   # optional
}

CSV form: header `x,z[,y,width,lanes,surface]` (or bare columns), one node/row;
all params take defaults.  ``--circuit`` / ``--p2p`` pick the topology.

USAGE
-----
  td5_trackgen.py build  <spec.json|spec.csv> [--slot N] [--level NNN] [--name STR]
                         [--circuit|--p2p] [--assets DIR]
  td5_trackgen.py sample <oval|figure8|straight> [--slot N] [--level NNN] [...]
  td5_trackgen.py list                                    # show the manifest

The auto-detect/override "hybrid" knobs (branches, lanes, checkpoints) live in
the spec: lane counts auto-default from `default_lanes` but a node may override;
checkpoints auto-space via "auto:N" or are listed explicitly; branches are taken
from an explicit "branches" array (auto branch-from-geometry is a follow-on).
"""

import argparse
import importlib.util
import json
import math
import os
import struct
import sys

# --------------------------------------------------------------------------
# Reuse the existing, proven tools by path-import (both have __main__ guards).
# --------------------------------------------------------------------------
_THIS_DIR = os.path.dirname(os.path.abspath(__file__))


def _load_module(name, filename):
    path = os.path.join(_THIS_DIR, filename)
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


_strip_tool = _load_module("strip_tool", "strip_tool.py")
_td6 = _load_module("convert_td6_tracks", "convert_td6_tracks.py")
build_routes = _td6.build_routes  # (strip_bytes, circuit, spline=None) -> (left,right)

# --------------------------------------------------------------------------
# Format constants (see module docstring / td5_level_formats.h).
# --------------------------------------------------------------------------
STRIP_HEADER_WORDS = 5
SPAN_STRIDE = 24
VERT_STRIDE = 6
PRE_SPAN_BYTES = 196          # native no-branch reserve: u32 jump_count + 32*6 reserved
JUMP_ENTRY_STRIDE = 6         # u16 start, u16 end, i16 remap
MAX_JUMP_ENTRIES = 32
SPAN_TYPE_QUAD = 1
U16 = 0xFFFF
INT16_MIN, INT16_MAX = -32768, 32767

DEFAULT_LANE_WIDTH = 1500     # world units per lane (matches Moscow ~1500)
DEFAULT_LANES = 4
DEFAULT_SURFACE = 0           # 0 dry asphalt
DEFAULT_SPAN_LENGTH = 1500    # target span length; the centerline is resampled to
                              # this so spans match engine assumptions (AI lookahead,
                              # grid spacing, minimap, render window are tuned for
                              # ~1500-unit Moscow-like spans). Set span_length=0 to
                              # keep the authored nodes verbatim.

# Point-to-point grid run-up: the engine's staggered starting grid spawns cars
# BEHIND the start line (offsets down to -18 spans for a 6-car grid). A custom
# P2P track starting at span 0 would push every car to a negative span, which
# the engine collapses to span 1 -> the whole grid stacks. So a P2P start span
# sits this many spans into the track, leaving room for the grid behind it
# (matches how the shipped P2P tracks start ~76 spans in). Circuits don't need
# this — negative grid spans wrap onto the ring.
P2P_GRID_RUNUP = 18

# Custom levels start above the 19 native + 11 TD6 levels (max existing = 39).
DEFAULT_CUSTOM_LEVEL_BASE = 40
# Frontend track tables are sized [37]; custom selectable slots start here.
CUSTOM_SLOT_BASE = 37

MANIFEST_NAME = "custom_tracks.json"


# ==========================================================================
# Spec loading / normalization
# ==========================================================================
def load_spec(path, circuit_override=None):
    """Load a centerline spec from .json or .csv into a normalized dict."""
    ext = os.path.splitext(path)[1].lower()
    if ext == ".json":
        with open(path, "r", encoding="utf-8") as f:
            raw = json.load(f)
    elif ext in (".csv", ".txt"):
        raw = _parse_csv_spec(path)
    else:
        raise ValueError("spec must be .json or .csv (got %s)" % ext)
    return normalize_spec(raw, circuit_override)


def _parse_csv_spec(path):
    """Parse a bare centerline CSV: x,z[,y,width,lanes,surface] per row."""
    nodes = []
    cols = None
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = [p.strip() for p in line.replace("\t", ",").split(",") if p.strip() != ""]
            # Header row?
            if cols is None and any(c.lower() in ("x", "z") for c in parts):
                cols = [c.lower() for c in parts]
                continue
            if cols is None:
                cols = ["x", "z", "y", "width", "lanes", "surface"][: len(parts)]
            node = {}
            for c, v in zip(cols, parts):
                if c in ("lanes", "surface"):
                    node[c] = int(float(v))
                else:
                    node[c] = float(v)
            nodes.append(node)
    return {"nodes": nodes}


def normalize_spec(raw, circuit_override=None):
    spec = {}
    spec["name"] = str(raw.get("name", "CUSTOM TRACK")).upper()[:30]
    if circuit_override is not None:
        spec["circuit"] = bool(circuit_override)
    else:
        spec["circuit"] = bool(raw.get("circuit", True))
    spec["lane_width"] = float(raw.get("lane_width", DEFAULT_LANE_WIDTH))
    spec["default_lanes"] = int(raw.get("default_lanes", DEFAULT_LANES))
    spec["default_surface"] = int(raw.get("default_surface", DEFAULT_SURFACE))
    spec["weather"] = int(raw.get("weather", 2))           # 2 = clear
    spec["smoke"] = int(raw.get("smoke", 1))
    spec["traffic_enable"] = int(raw.get("traffic_enable", 0))
    fog = raw.get("fog", {}) or {}
    spec["fog"] = {
        "enabled": int(fog.get("enabled", 0)),
        "r": int(fog.get("r", 0)), "g": int(fog.get("g", 0)), "b": int(fog.get("b", 0)),
    }
    spec["checkpoints"] = raw.get("checkpoints", "auto:4")
    spec["traffic"] = raw.get("traffic", [])
    spec["branches"] = raw.get("branches", [])

    nodes = []
    for n in raw.get("nodes", []):
        nodes.append({
            "x": float(n["x"]),
            "z": float(n["z"]),
            "y": float(n.get("y", 0.0)),
            "lanes": int(n.get("lanes", spec["default_lanes"])),
            "width": float(n["width"]) if "width" in n else None,   # resolved later
            "surface": int(n.get("surface", spec["default_surface"])),
        })
    if len(nodes) < (3 if spec["circuit"] else 2):
        raise ValueError("need >=3 nodes for a circuit, >=2 for point-to-point (got %d)" % len(nodes))
    # Resolve per-node width from lane count when not explicit.
    for n in nodes:
        if n["width"] is None:
            n["width"] = n["lanes"] * spec["lane_width"]

    # Resample the centerline to ~span_length spacing so spans match the engine's
    # ~1500-unit assumptions (sparse authored nodes otherwise make wide, coarse
    # spans that break AI lookahead / grid spacing / the minimap).
    spec["span_length"] = float(raw.get("span_length", DEFAULT_SPAN_LENGTH))
    if spec["span_length"] > 0:
        nodes = resample_centerline(nodes, spec["span_length"], spec["circuit"])
    spec["nodes"] = nodes
    return spec


def resample_centerline(nodes, target_len, circuit):
    """Resample the centerline to ~target_len spacing along its arc length.

    Position (x,z,y) is linearly interpolated; the discrete attributes
    (lanes/width/surface) are carried from the nearer source node. A circuit is
    treated as a closed loop (the last sample does not duplicate the first).
    """
    n = len(nodes)
    pts = [(nd["x"], nd["z"]) for nd in nodes]
    seq = list(range(n)) + [0] if circuit else list(range(n))

    seg = []          # (a_idx, b_idx, length)
    seg_start = []    # arc length at each segment start
    total = 0.0
    for i in range(len(seq) - 1):
        a, b = pts[seq[i]], pts[seq[i + 1]]
        d = math.hypot(b[0] - a[0], b[1] - a[1])
        seg_start.append(total)
        seg.append((seq[i], seq[i + 1], d))
        total += d
    if total < 1e-6 or not seg:
        return nodes

    count = max(3 if circuit else 2, int(round(total / target_len)))
    step = total / count
    sample_ts = [k * step for k in range(count if circuit else count + 1)]

    out = []
    for t in sample_ts:
        if t >= total:
            t = total - 1e-3
        j = 0
        while j < len(seg) - 1 and seg_start[j + 1] <= t:
            j += 1
        a_idx, b_idx, d = seg[j]
        local = (t - seg_start[j]) / d if d > 1e-9 else 0.0
        a, b = nodes[a_idx], nodes[b_idx]
        src = a if local < 0.5 else b
        out.append({
            "x": a["x"] + (b["x"] - a["x"]) * local,
            "z": a["z"] + (b["z"] - a["z"]) * local,
            "y": a["y"] + (b["y"] - a["y"]) * local,
            "lanes": src["lanes"],
            "width": a["width"] + (b["width"] - a["width"]) * local,
            "surface": src["surface"],
        })
    return out


# ==========================================================================
# Geometry: centerline -> spans + vertices
# ==========================================================================
def _tangent(nodes, i, circuit):
    """Unit forward tangent at node i (central difference; wraps for circuit)."""
    n = len(nodes)
    if circuit:
        a = nodes[(i - 1) % n]; b = nodes[(i + 1) % n]
    else:
        a = nodes[max(0, i - 1)]; b = nodes[min(n - 1, i + 1)]
    dx = b["x"] - a["x"]; dz = b["z"] - a["z"]
    m = math.hypot(dx, dz)
    if m < 1e-6:
        return (0.0, 1.0)
    return (dx / m, dz / m)


SPAN_TYPE_JUNCTION_FWD = 8        # fork: outer lanes peel off via link_next
SPAN_TYPE_SENTINEL_START = 9      # branch corridor first span (minimap corridor detect)
SPAN_TYPE_SENTINEL_END = 10       # branch corridor last span; link_next -> rejoin


def _row_points_offset(node, tangent, lane_count, total_width, lateral_center):
    """lane_count+1 rail points across `total_width`, whose lateral midpoint is
    shifted `lateral_center` along left_dir from the node (used to widen a fork
    junction toward the branch side). Returns world (x,y,z) tuples."""
    tx, tz = tangent
    lx, lz = (tz, -tx)
    half = total_width * 0.5
    pts = []
    for k in range(lane_count + 1):
        lat = lateral_center + half - (total_width * k / lane_count)
        pts.append((node["x"] + lx * lat, node["y"], node["z"] + lz * lat))
    return pts


def _row_points(node, tangent, lane_count):
    """(lane_count+1) rail points across the road, LEFT edge -> RIGHT edge.

    left_dir = (tz, -tx) of the forward tangent (verified vs Moscow span 0).
    Returns world (x,y,z) tuples.
    """
    tx, tz = tangent
    lx, lz = (tz, -tx)            # left unit direction in the x/z plane
    half = node["width"] * 0.5
    pts = []
    for k in range(lane_count + 1):
        # k=0 -> +half (left edge); k=lane_count -> -half (right edge)
        lat = half - (node["width"] * k / lane_count)
        pts.append((node["x"] + lx * lat, node["y"], node["z"] + lz * lat))
    return pts


BRANCH_WIDEN_SPANS = 3            # spans before the fork that widen so the
                                 # driver can steer into the peel-off lanes


def _emit_span_rows(vertices, near_pts, far_pts, origin, err_idx):
    """Append a span's near then far vertex rows; return (lvi, rvi)."""
    ox, oy, oz = origin
    lvi = len(vertices)
    for (wx, wy, wz) in near_pts:
        vertices.append(_local_vtx(wx, wy, wz, ox, oy, oz, err_idx))
    rvi = len(vertices)
    for (wx, wy, wz) in far_pts:
        vertices.append(_local_vtx(wx, wy, wz, ox, oy, oz, err_idx))
    return lvi, rvi


def _nearest_node(main_nodes, p):
    best, bestd = 0, None
    for i, nd in enumerate(main_nodes):
        d = (nd["x"] - p["x"]) ** 2 + (nd["z"] - p["z"]) ** 2
        if bestd is None or d < bestd:
            bestd, best = d, i
    return best


def build_geometry(spec):
    """Return (header, spans, vertices, jump_entries) ready for strip.json.

    Branches (spec['branches']) attach to the main road by geometry: each forks
    off the main span nearest its first node and rejoins nearest its last node.
    The fork span is widened (main+branch lanes) over a short run-up and marked
    type 8 (JUNCTION_FWD); an actor that steers into the outer lanes peels off
    via link_next to the branch corridor (spans appended past the ring), which
    link_next's back to the rejoin main span. A [lo,hi,base] jump entry maps the
    branch spans onto the main ring for progress/lap tracking.
    """
    nodes = spec["nodes"]
    circuit = spec["circuit"]
    n = len(nodes)
    nspans = n if circuit else (n - 1)
    lane_width = spec["lane_width"]
    tangents = [_tangent(nodes, i, circuit) for i in range(n)]

    # --- resolve branches: attach each to the main road by geometry ---
    # Branches ALWAYS peel off the RIGHT (high-sub_lane) side -- that is the side
    # the engine's type-8 check (sub_lane >= main_lanes) selects. Shipped tracks
    # (e.g. level007) build a fork as a shared-vertex split: the fork span is
    # widened to the right, and its far-row OUTER (branch+1) vertices ARE the
    # branch corridor's first row, while the INNER (main+1) vertices ARE the next
    # main span's row -- they share the boundary vertex, so there is no gap. The
    # rejoin mirrors this. We replicate that exactly here.
    branches = []
    for br in spec.get("branches", []):
        user = [{"x": float(p["x"]), "z": float(p["z"]), "y": float(p.get("y", 0.0))}
                for p in br.get("nodes", [])]
        if not user:
            continue
        blanes = int(br.get("lanes", spec["default_lanes"]))
        bsurf = int(br.get("surface", spec["default_surface"]))
        bwidth = float(br["width"]) if "width" in br else blanes * lane_width
        fork = _nearest_node(nodes, user[0])
        rejoin = _nearest_node(nodes, user[-1])
        if not circuit and rejoin <= fork:
            sys.stderr.write("WARNING: branch rejoin is at/before its fork; skipped.\n")
            continue
        main_lanes = nodes[fork]["lanes"]
        total = main_lanes + blanes
        tw = total * lane_width
        lat_c = -(blanes * lane_width * 0.5)        # widen the junction to the RIGHT

        # Shared edge rows: the branch corridor STARTS at the fork span's far-row
        # outer verts and ENDS at the rejoin span's near-row outer verts (the
        # last blanes+1 of each wide row).
        fork_exit = (fork + 1) % n if circuit else min(fork + 1, n - 1)
        fork_outer = _row_points_offset(nodes[fork_exit], tangents[fork_exit],
                                        total, tw, lat_c)[main_lanes:]
        rejoin_outer = _row_points_offset(nodes[rejoin], tangents[rejoin],
                                          total, tw, lat_c)[main_lanes:]

        def _ctr(pts):
            return {"x": 0.5 * (pts[0][0] + pts[-1][0]),
                    "y": 0.5 * (pts[0][1] + pts[-1][1]),
                    "z": 0.5 * (pts[0][2] + pts[-1][2])}

        path = [_ctr(fork_outer)] + user + [_ctr(rejoin_outer)]
        for bn in path:
            bn["lanes"] = blanes; bn["surface"] = bsurf; bn["width"] = bwidth
        if spec["span_length"] > 0:
            path = resample_centerline(path, spec["span_length"], circuit=False)
        if len(path) < 2:
            sys.stderr.write("WARNING: branch has < 2 nodes after resampling; skipped.\n")
            continue
        branches.append({"nodes": path, "fork": fork, "rejoin": rejoin, "lanes": blanes,
                         "total": total, "lat_c": lat_c,
                         "fork_outer": fork_outer, "rejoin_outer": rejoin_outer})

    # mark widened main spans: a run-up BEFORE the fork (so the driver can move
    # into the peel-off lanes) and a run-out AT/AFTER the rejoin (so the branch
    # merges back into a wide section). widen_map[span] = (total, lat_c, fork|None)
    widen_map = {}
    for br in branches:
        total, lat_c = br["total"], br["lat_c"]
        for k in range(BRANCH_WIDEN_SPANS + 1):           # fork run-up [fork-W .. fork]
            si = br["fork"] - k
            if circuit: si %= nspans
            if 0 <= si < nspans and si not in widen_map:
                widen_map[si] = (total, lat_c, br if k == 0 else None)
        for k in range(BRANCH_WIDEN_SPANS + 1):           # rejoin run-out [rejoin .. rejoin+W]
            si = br["rejoin"] + k
            if circuit: si %= nspans
            if 0 <= si < nspans and si not in widen_map:
                widen_map[si] = (total, lat_c, None)

    spans = []
    vertices = []

    # --- main ring ---
    for si in range(nspans):
        a_idx = si
        b_idx = (si + 1) % n if circuit else (si + 1)
        a = nodes[a_idx]; b = nodes[b_idx]
        origin = (int(round(a["x"])), int(round(a["y"])), int(round(a["z"])))
        wide = widen_map.get(si)
        if wide:
            total, lat_c, fork_br = wide
            tw = total * lane_width
            near = _row_points_offset(a, tangents[a_idx], total, tw, lat_c)
            far = _row_points_offset(b, tangents[b_idx], total, tw, lat_c)
            lvi, rvi = _emit_span_rows(vertices, near, far, origin, si)
            stype = SPAN_TYPE_JUNCTION_FWD if fork_br else SPAN_TYPE_QUAD
            spans.append([stype, a["surface"] & 0x0F, 0, total & 0x0F,
                          lvi & U16, rvi & U16, U16, U16, *origin])  # fork link_next patched below
        else:
            lanes = a["lanes"]
            near = _row_points(a, tangents[a_idx], lanes)
            far = _row_points(b, tangents[b_idx], lanes)
            lvi, rvi = _emit_span_rows(vertices, near, far, origin, si)
            spans.append([SPAN_TYPE_QUAD, a["surface"] & 0x0F, 0, lanes & 0x0F,
                          lvi & U16, rvi & U16, U16, U16, *origin])

    # --- branch corridors (appended past the ring) + link wiring + jump table ---
    jump_entries = []
    for br in branches:
        bnodes = br["nodes"]
        m = len(bnodes)
        btan = [_tangent(bnodes, i, circuit=False) for i in range(m)]
        branch_start = len(spans)
        for k in range(m - 1):
            a = bnodes[k]; b = bnodes[k + 1]
            lanes = a["lanes"]
            origin = (int(round(a["x"])), int(round(a["y"])), int(round(a["z"])))
            # First/last rows are the SHARED fork/rejoin outer verts (no gap at the
            # split/merge); interior rows follow the branch path.
            near = br["fork_outer"]   if k == 0     else _row_points(a, btan[k], lanes)
            far  = br["rejoin_outer"] if k == m - 2 else _row_points(b, btan[k + 1], lanes)
            lvi, rvi = _emit_span_rows(vertices, near, far, origin, branch_start + k)
            # corridor spans are type-9 (start) .. type-10 (end) so the minimap
            # corridor detector picks them up; the end span link_next's to rejoin.
            if k == m - 2:
                stype, link_next = SPAN_TYPE_SENTINEL_END, (br["rejoin"] & U16)
            elif k == 0:
                stype, link_next = SPAN_TYPE_SENTINEL_START, U16
            else:
                stype, link_next = SPAN_TYPE_QUAD, U16
            spans.append([stype, a["surface"] & 0x0F, 0, lanes & 0x0F,
                          lvi & U16, rvi & U16, link_next, U16, *origin])
        branch_end = len(spans) - 1
        spans[br["fork"]][6] = branch_start & U16            # fork -> branch start
        jump_entries.append((branch_start, branch_end, br["fork"]))

    span_off = STRIP_HEADER_WORDS * 4 + PRE_SPAN_BYTES        # 20 + 196 = 216
    vtx_off = span_off + SPAN_STRIDE * len(spans)
    ring_len = nspans                                         # main ring only
    header = [span_off, ring_len, vtx_off, len(vertices), len(spans)]
    return header, spans, vertices, jump_entries


def _local_vtx(wx, wy, wz, ox, oy, oz, span_idx):
    vx = int(round(wx)) - ox
    vy = int(round(wy)) - oy
    vz = int(round(wz)) - oz
    for label, val in (("x", vx), ("y", vy), ("z", vz)):
        if val < INT16_MIN or val > INT16_MAX:
            raise ValueError(
                "span %d vertex %s offset %d exceeds int16 range; spans/road-width "
                "too large for one origin -- subdivide the centerline more densely "
                "(keep span length + half-width < ~32000 world units)."
                % (span_idx, label, val))
    return [vx, vy, vz]


# ==========================================================================
# strip.json / route emission
# ==========================================================================
def _build_pre_span(jump_entries):
    """Pre-span region: u32 jump_count + 6-byte [lo,hi,base] entries, then zero
    padding to at least the native 196-byte reserve (so span_off stays 216 for
    <=32 branches; the encoder uses the actual length regardless)."""
    jump_entries = jump_entries or []
    blob = bytearray()
    blob += struct.pack("<I", len(jump_entries))
    for (lo, hi, base) in jump_entries:
        blob += struct.pack("<HHh", lo & 0xFFFF, hi & 0xFFFF, int(base))
    if len(blob) < PRE_SPAN_BYTES:
        blob += b"\x00" * (PRE_SPAN_BYTES - len(blob))
    return bytes(blob)


def emit_strip_json(header, spans, vertices, jump_entries=None):
    pre = _build_pre_span(jump_entries)
    # header[0]=span table offset, header[2]=vertex table offset -- recompute from
    # the actual pre-span length so a non-default jump table stays self-consistent.
    span_off = STRIP_HEADER_WORDS * 4 + len(pre)
    vtx_off = span_off + SPAN_STRIDE * len(spans)
    hdr = [span_off, header[1], vtx_off, len(vertices), len(spans)]
    return {
        "_format": "td5_strip",
        "_version": 1,
        "header": hdr,
        "pre_span_hex": pre.hex(),
        "spans": spans,
        "pre_vertex_hex": "",
        "vertices": vertices,
        "tail_hex": "",
    }


def strip_json_to_bytes(strip_obj):
    """Pack strip.json -> STRIP.DAT bytes using the proven strip_tool.imp()."""
    return _strip_tool.imp(json.dumps(strip_obj))


def emit_routes(strip_bytes, circuit):
    """Return (left_csv_text, right_csv_text) via build_routes()."""
    left, right = build_routes(strip_bytes, circuit=circuit, spline=None)
    return _routes_to_csv(left), _routes_to_csv(right)


def _routes_to_csv(table):
    out = ["# td5 route table -- one row per span: lane0,lane1,lane2 (raw u8)"]
    for i in range(0, len(table), 3):
        out.append("%d,%d,%d" % (table[i], table[i + 1], table[i + 2]))
    return "\n".join(out) + "\n"


# ==========================================================================
# levelinf / checkpt / traffic emission
# ==========================================================================
def resolve_checkpoints(spec, nspans):
    """Return (count, spans[7]) for levelinf, honoring 'auto:N' or explicit list."""
    cp = spec["checkpoints"]
    spans = []
    if isinstance(cp, str) and cp.lower().startswith("auto"):
        try:
            count = int(cp.split(":", 1)[1])
        except (IndexError, ValueError):
            count = 4
        count = max(1, min(7, count))
        # Evenly space across the ring; skip span 0 (start/finish line).
        for k in range(count):
            spans.append(int(round((k + 1) * nspans / (count + 1))) % max(1, nspans))
    elif isinstance(cp, (list, tuple)):
        spans = [int(s) % max(1, nspans) for s in cp][:7]
    count = len(spans)
    spans = (spans + [0] * 7)[:7]
    return count, spans


def emit_levelinf(spec, nspans):
    cp_count, cp_spans = resolve_checkpoints(spec, nspans)
    circuit = spec["circuit"]
    return {
        "_format": "td5_levelinf",
        "_version": 1,
        "_size": 100,
        "track_type": {"value": 1 if circuit else 0},      # 1=circuit, 0=P2P
        "smoke_enable": {"value": spec["smoke"]},
        "checkpoint_count": {"value": cp_count},
        "checkpoint_spans": {"value": cp_spans},
        "weather_type": {"value": spec["weather"]},
        "density_pair_count": {"value": 0},
        "traffic_enable": {"value": spec["traffic_enable"]},
        "density_pairs": {"value": [0] * 12},
        "_pad_4C": {"value": [0] * 8},
        "sky_animation_index": {"value": 36 if circuit else 0xFFFFFFFF},
        "total_span_count": {"value": nspans},
        "fog_enabled": {"value": spec["fog"]["enabled"]},
        "fog_color_r": {"value": spec["fog"]["r"]},
        "fog_color_g": {"value": spec["fog"]["g"]},
        "fog_color_b": {"value": spec["fog"]["b"]},
        "_pad_63": {"value": 0},
    }


def emit_checkpt():
    # 96-byte reverse page-swap record; -1 = no swap (forward-only custom track).
    return {"_format": "td5_checkpt", "_version": 1, "rows": [[-1] * 6 for _ in range(4)]}


def emit_traffic(spec, nspans):
    records = []
    for t in spec.get("traffic", []):
        span = int(t["span"]) % max(1, nspans)
        lane = int(t.get("lane", 1))
        flags = 1 if t.get("oncoming", False) else 0
        records.append([span, flags, lane])
    records.append([-1, 1, 1])   # sentinel
    return {"_format": "td5_traffic", "_version": 1, "records": records}


# ==========================================================================
# Level directory + manifest
# ==========================================================================
def levels_dir(assets_root):
    return os.path.join(assets_root, "levels")


def pick_level_number(assets_root, requested=None):
    ld = levels_dir(assets_root)
    if requested is not None:
        return int(requested)
    existing = set()
    if os.path.isdir(ld):
        for name in os.listdir(ld):
            if name.startswith("level") and name[5:8].isdigit():
                existing.add(int(name[5:8]))
    n = DEFAULT_CUSTOM_LEVEL_BASE
    while n in existing:
        n += 1
    return n


def write_level(assets_root, level_no, spec):
    """Write the full levelNNN/ source set. Returns (dir, ring_spans)."""
    header, spans, vertices, jump_entries = build_geometry(spec)
    ring = header[1]            # main racing line (checkpoints/finish/laps use this)
    nspans = ring              # branch corridor spans live past the ring
    strip_obj = emit_strip_json(header, spans, vertices, jump_entries)
    strip_bytes = strip_json_to_bytes(strip_obj)               # validates round-trip layout
    left_csv, right_csv = emit_routes(strip_bytes, spec["circuit"])

    out_dir = os.path.join(levels_dir(assets_root), "level%03d" % level_no)
    os.makedirs(out_dir, exist_ok=True)

    def _wj(name, obj):
        with open(os.path.join(out_dir, name), "w", encoding="utf-8") as f:
            json.dump(obj, f, indent=2)
            f.write("\n")

    def _wt(name, text):
        with open(os.path.join(out_dir, name), "w", encoding="utf-8") as f:
            f.write(text)

    _wj("strip.json", strip_obj)
    _wt("left.trk.csv", left_csv)
    _wt("right.trk.csv", right_csv)
    _wj("levelinf.json", emit_levelinf(spec, nspans))
    _wj("checkpt.json", emit_checkpt())
    _wj("traffic.json", emit_traffic(spec, nspans))
    return out_dir, nspans


def load_manifest(assets_root):
    path = os.path.join(levels_dir(assets_root), MANIFEST_NAME)
    if os.path.isfile(path):
        with open(path, "r", encoding="utf-8") as f:
            return json.load(f)
    return {"_format": "td5_custom_tracks", "_version": 1, "tracks": []}


def pick_slot(manifest, requested=None):
    if requested is not None:
        return int(requested)
    used = {int(t["slot"]) for t in manifest.get("tracks", [])}
    s = CUSTOM_SLOT_BASE
    while s in used:
        s += 1
    return s


def update_manifest(assets_root, slot, level_no, spec, nspans):
    manifest = load_manifest(assets_root)
    cp_count, _ = resolve_checkpoints(spec, nspans)
    if spec["circuit"]:
        start_span = 0                                    # negative grid spans wrap onto the ring
    else:
        # P2P: sit the start line a grid run-up into the track so the staggered
        # grid (down to -18 spans) lands on valid spans instead of stacking.
        start_span = min(P2P_GRID_RUNUP, max(1, nspans - 2))
        if nspans < P2P_GRID_RUNUP + 4:
            sys.stderr.write(
                "WARNING: point-to-point track is short (%d spans); the starting "
                "grid needs ~%d run-up spans, so the race may be very short and the "
                "grid may still crowd. Author more centerline nodes for a longer "
                "track.\n" % (nspans, P2P_GRID_RUNUP))
    entry = {
        "slot": int(slot),
        "level": int(level_no),
        "name": spec["name"],
        "circuit": bool(spec["circuit"]),
        "start_span": start_span,
        "finish_span": 0 if spec["circuit"] else max(0, nspans - 1),
        "sky_pitch": 0.08,
        "tga": -1,
    }
    tracks = [t for t in manifest.get("tracks", []) if int(t["slot"]) != int(slot)]
    tracks.append(entry)
    tracks.sort(key=lambda t: int(t["slot"]))
    manifest["tracks"] = tracks
    path = os.path.join(levels_dir(assets_root), MANIFEST_NAME)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=2)
        f.write("\n")
    return entry, path


# ==========================================================================
# Built-in sample centerlines (for quick bring-up / smoke tests)
# ==========================================================================
def sample_spec(kind, name=None):
    kind = kind.lower()
    if kind == "oval":
        nodes = []
        rx, rz = 30000.0, 18000.0
        steps = 48
        for k in range(steps):
            a = 2 * math.pi * k / steps
            nodes.append({"x": rx * math.cos(a), "z": rz * math.sin(a)})
        return normalize_spec({"name": name or "TEST OVAL", "circuit": True,
                               "default_lanes": 4, "nodes": nodes,
                               "checkpoints": "auto:4", "weather": 2})
    if kind == "figure8":
        nodes = []
        steps = 64
        for k in range(steps):
            a = 2 * math.pi * k / steps
            nodes.append({"x": 26000.0 * math.sin(2 * a), "z": 20000.0 * math.sin(a)})
        return normalize_spec({"name": name or "TEST FIGURE 8", "circuit": True,
                               "default_lanes": 4, "nodes": nodes,
                               "checkpoints": "auto:4", "weather": 2})
    if kind == "straight":
        nodes = [{"x": 0.0, "z": float(z)} for z in range(0, 80001, 2500)]
        return normalize_spec({"name": name or "TEST STRAIGHT", "circuit": False,
                               "default_lanes": 4, "nodes": nodes,
                               "checkpoints": "auto:3", "weather": 2})
    raise ValueError("unknown sample kind '%s' (oval|figure8|straight)" % kind)


# ==========================================================================
# CLI
# ==========================================================================
def _default_assets_root():
    # script lives in re/tools/ ; assets root is re/assets relative to repo root
    repo = os.path.dirname(os.path.dirname(_THIS_DIR))   # .../TD5RE
    return os.path.join(repo, "re", "assets")


def _do_build(spec, args):
    assets_root = args.assets or _default_assets_root()
    if args.name:
        spec["name"] = args.name.upper()[:30]
    level_no = pick_level_number(assets_root, args.level)
    manifest = load_manifest(assets_root)
    slot = pick_slot(manifest, args.slot)
    out_dir, nspans = write_level(assets_root, level_no, spec)
    entry, manifest_path = update_manifest(assets_root, slot, level_no, spec, nspans)
    print("Built track '%s'" % spec["name"])
    print("  level dir : %s  (%d spans, %s)" % (out_dir, nspans,
          "circuit" if spec["circuit"] else "point-to-point"))
    print("  slot      : %d   level: %d" % (slot, level_no))
    print("  manifest  : %s" % manifest_path)
    print("  checkpoints:", entry, sep=" ")
    print("\nDrive it:  td5re.exe --AutoRace=1 --SkipIntro=1 --DefaultTrack=%d" % slot)
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description="Build a TD5 custom track from a centerline.")
    sub = ap.add_subparsers(dest="cmd", required=True)

    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--slot", type=int, default=None, help="frontend track slot (default: next free >=37)")
    common.add_argument("--level", type=int, default=None, help="levelNNN number (default: next free >=40)")
    common.add_argument("--name", type=str, default=None, help="display name override")
    common.add_argument("--assets", type=str, default=None, help="re/assets root (default: repo re/assets)")
    common.add_argument("--circuit", dest="circuit", action="store_true", default=None)
    common.add_argument("--p2p", dest="circuit", action="store_false")

    b = sub.add_parser("build", parents=[common], help="build from a centerline spec (.json/.csv)")
    b.add_argument("spec", help="path to centerline spec (.json or .csv)")

    s = sub.add_parser("sample", parents=[common], help="build a built-in sample track")
    s.add_argument("kind", choices=["oval", "figure8", "straight"])

    sub.add_parser("list", help="show the custom-track manifest")

    args = ap.parse_args(argv)

    if args.cmd == "list":
        assets_root = getattr(args, "assets", None) or _default_assets_root()
        manifest = load_manifest(assets_root)
        print(json.dumps(manifest, indent=2))
        return 0

    if args.cmd == "build":
        spec = load_spec(args.spec, args.circuit)
        return _do_build(spec, args)

    if args.cmd == "sample":
        spec = sample_spec(args.kind, args.name)
        if args.circuit is not None:
            spec["circuit"] = args.circuit
        return _do_build(spec, args)

    ap.print_help()
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
