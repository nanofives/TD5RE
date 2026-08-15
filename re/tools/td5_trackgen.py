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
    # Optional scenery layers (see td5_scenery.py). Pass through so write_level
    # can fold them into models.bin / emit the breakable-prop files.
    #   scenery       : list of mesh dicts (mesh_tool shape) folded into models.bin
    #   mov_instances : list of breakable-prop instances -> LEVEL.MOV
    #   prop_meshes   : list of <=8 prop mesh dicts       -> PROPMESH.BIN
    spec["scenery"] = raw.get("scenery", [])
    spec["mov_instances"] = raw.get("mov_instances", [])
    spec["prop_meshes"] = raw.get("prop_meshes", [])
    spec["texture_imports"] = raw.get("texture_imports", [])
    spec["span_textures"] = raw.get("span_textures", {})
    spec["ground_fill"] = raw.get("ground_fill")          # {page, half_width?} or None
    spec["road_elevation"] = raw.get("road_elevation")    # {amplitude,waves,max_grade,intensity} or None
    spec["guardrails"] = raw.get("guardrails")            # {page, height?, offset?, left?, right?} or None (OPT-IN)
    if "road_texture" in raw:
        spec["road_texture"] = int(raw.get("road_texture"))
    if "textured" in raw:
        spec["textured"] = bool(raw.get("textured"))

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
    # Guarantee the road is never wider than the curve it sits on, so per-span quads
    # never fold on the inside of a bend (root cause of phantom inner-lane walls +
    # AI stall/reverse on tight custom corners). Runs on the final resampled nodes
    # (widths resolved) and BEFORE elevation so the Y profile maps onto the final XZ.
    # No-op on gentle geometry.
    limit_centerline_curvature(nodes, spec["circuit"],
                               safety=float(raw.get("curve_safety", CURVE_SAFETY_DEFAULT)))
    # Road elevation: synthesize a smooth, drivable, (for circuits) periodic Y
    # profile onto the resampled centerline so the road climbs/descends like a
    # real track. Applied AFTER resampling so it maps onto the final spans.
    re_cfg = raw.get("road_elevation")
    if re_cfg:
        apply_road_elevation(nodes, spec["circuit"],
                             amplitude=float(re_cfg.get("amplitude", 0.0)),
                             waves=float(re_cfg.get("waves", 0.0)),
                             max_grade=float(re_cfg.get("max_grade", 0.12)),
                             intensity=float(re_cfg.get("intensity", 1.0)))
    spec["nodes"] = nodes
    return spec


# --------------------------------------------------------------------------- #
# Auto-detection: lanes/sublanes from road width, branches from extra paths.
# Works on external centerline files (CSV/JSON, multi-path) and, via
# redetect_lanes, on any already-loaded spec (e.g. an imported level).
# --------------------------------------------------------------------------- #

def lanes_from_width(width, lane_width, default_lanes=DEFAULT_LANES, max_lanes=12):
    """Auto lane count from road width: round(width / lane_width), clamped >=1."""
    try:
        width = float(width); lane_width = float(lane_width)
    except (TypeError, ValueError):
        return default_lanes
    if width <= 0 or lane_width <= 0:
        return default_lanes
    return max(1, min(max_lanes, int(round(width / lane_width))))


def redetect_lanes(spec, lane_width=None):
    """In-place: recompute each node's lane count from its width (sublane detection).
    Nodes/branches keep their width; lanes follow it. Returns the spec."""
    lw = float(lane_width or spec.get("lane_width") or DEFAULT_LANE_WIDTH)
    dl = int(spec.get("default_lanes", DEFAULT_LANES))
    for n in spec.get("nodes", []):
        if n.get("width"):
            n["lanes"] = lanes_from_width(n["width"], lw, dl)
    for br in spec.get("branches", []):
        ws = [p["width"] for p in br.get("nodes", []) if isinstance(p, dict) and p.get("width")]
        if ws:
            br["lanes"] = lanes_from_width(max(ws), lw, dl)
    return spec


def _norm_points(pts):
    out = []
    for p in pts:
        if isinstance(p, dict):
            out.append({k: p[k] for k in ("x", "y", "z", "width", "lanes", "surface") if k in p})
        elif isinstance(p, (list, tuple)) and len(p) >= 2:
            d = {"x": p[0], "z": p[1]}
            if len(p) >= 3:
                d["y"] = p[2]
            if len(p) >= 4:
                d["width"] = p[3]
            out.append(d)
    return out


def _paths_from_json(obj):
    meta = {}
    if isinstance(obj, dict):
        if "nodes" in obj:                         # a full spec -> main path + its branches
            meta = {k: obj[k] for k in ("name", "lane_width", "circuit", "default_lanes",
                                        "weather", "fog", "traffic_enable", "checkpoints") if k in obj}
            paths = [obj["nodes"]] + [br["nodes"] for br in obj.get("branches", []) if br.get("nodes")]
            return [_norm_points(p) for p in paths], meta
        if "paths" in obj:
            meta = {k: obj[k] for k in ("name", "lane_width", "circuit") if k in obj}
            return [_norm_points(p) for p in obj["paths"]], meta
        return [_norm_points(v) for v in obj.values() if isinstance(v, list)], meta
    if isinstance(obj, list):
        if obj and isinstance(obj[0], list):       # list of paths
            return [_norm_points(p) for p in obj], meta
        return [_norm_points(obj)], meta           # single point list
    raise ValueError("unrecognised JSON track structure")


def _paths_from_csv(text):
    paths = [[]]
    cols = None
    for line in text.splitlines():
        s = line.strip()
        if not s or s.startswith("#"):
            if not s and paths[-1]:                # blank line separates paths
                paths.append([])
            continue
        parts = [p.strip() for p in s.replace("\t", ",").split(",") if p.strip() != ""]
        if cols is None and any(c.lower() in ("x", "z") for c in parts):
            cols = [c.lower() for c in parts]
            continue
        use = cols or (["x", "z", "y", "width", "lanes", "surface"][:len(parts)])
        rec, pid = {}, None
        for c, v in zip(use, parts):
            if c == "path":
                pid = int(float(v))
            elif c in ("lanes", "surface"):
                rec[c] = int(float(v))
            else:
                rec[c] = float(v)
        if "x" not in rec or "z" not in rec:
            continue
        if pid is not None:
            while len(paths) <= pid:
                paths.append([])
            paths[pid].append(rec)
        else:
            paths[-1].append(rec)
    return [p for p in paths if len(p) >= 2]


def parse_paths_text(text, filename=""):
    """Parse a centerline file -> (paths, meta). Paths is a list of point lists
    {x,z,y?,width?,lanes?,surface?}. JSON (spec / {"paths"} / list-of-paths / point
    list) or CSV/TSV (x,z[,y,width,lanes,surface]; 'path' column or blank line splits
    paths; '#' comments + header skipped)."""
    text = text.lstrip("﻿")
    if filename.lower().endswith(".json") or text.lstrip()[:1] in "{[":
        return _paths_from_json(json.loads(text))
    return _paths_from_csv(text), {}


def detect_spec_from_paths(paths, name="IMPORTED TRACK", lane_width=DEFAULT_LANE_WIDTH,
                           circuit=None, default_lanes=DEFAULT_LANES, meta=None):
    """Centerline paths -> spec, auto-detecting lanes from per-point width and
    branches from extra paths (longest path = main road; the rest become branches,
    attached by the converter to the nearest fork/rejoin)."""
    meta = meta or {}
    lane_width = float(meta.get("lane_width", lane_width)) or DEFAULT_LANE_WIDTH
    dl = int(meta.get("default_lanes", default_lanes))
    paths = [p for p in paths if p and len(p) >= 2]
    if not paths:
        raise ValueError("no usable paths (need >=2 points each)")
    main = max(paths, key=len)
    rest = [p for p in paths if p is not main]

    def _node(p):
        n = {"x": float(p["x"]), "z": float(p["z"]), "y": float(p.get("y", 0.0))}
        if "width" in p:
            n["width"] = float(p["width"])
        if "lanes" in p:
            n["lanes"] = int(p["lanes"])
        elif "width" in p:
            n["lanes"] = lanes_from_width(p["width"], lane_width, dl)
        else:
            n["lanes"] = dl
        if "surface" in p:
            n["surface"] = int(p["surface"])
        return n

    nodes = [_node(p) for p in main]
    branches = []
    for bp in rest:
        bl = [lanes_from_width(p["width"], lane_width, dl) if "width" in p
              else int(p.get("lanes", dl)) for p in bp]
        branches.append({"nodes": [{"x": float(p["x"]), "z": float(p["z"]),
                                    "y": float(p.get("y", 0.0))} for p in bp],
                         "lanes": max(bl) if bl else dl})

    spec = {"name": meta.get("name", name), "lane_width": lane_width, "default_lanes": dl,
            "nodes": nodes, "branches": branches,
            "checkpoints": meta.get("checkpoints", "auto:4"),
            "weather": int(meta.get("weather", 2)),
            "traffic_enable": int(meta.get("traffic_enable", 0)),
            "fog": meta.get("fog", {"enabled": 0, "r": 0, "g": 0, "b": 0})}
    if circuit is not None:
        spec["circuit"] = bool(circuit)
    elif "circuit" in meta:
        spec["circuit"] = bool(meta["circuit"])
    else:                                          # auto: closed loop if ends meet
        a, b = nodes[0], nodes[-1]
        spec["circuit"] = math.hypot(a["x"] - b["x"], a["z"] - b["z"]) < lane_width * 2
    return spec


CURVE_SAFETY_DEFAULT = 1.5      # R_min = half_width * safety
CURVE_MAX_ITERS      = 200


def _node_radius(a, b, c):
    """Circumradius of the polyline corner a-b-c; +inf if ~collinear."""
    abx, abz = b["x"] - a["x"], b["z"] - a["z"]
    cbx, cbz = b["x"] - c["x"], b["z"] - c["z"]
    acx, acz = a["x"] - c["x"], a["z"] - c["z"]
    area2 = abs(abx * cbz - abz * cbx)                  # 2*triangle area
    if area2 < 1e-6:
        return float("inf")
    la = math.hypot(abx, abz); lb = math.hypot(cbx, cbz); lc = math.hypot(acx, acz)
    return (la * lb * lc) / (2.0 * area2)


def limit_centerline_curvature(nodes, circuit, safety=CURVE_SAFETY_DEFAULT,
                               req_half=None, only=None):
    """Iteratively relax the centerline so every corner's radius of curvature is
    >= half_width*safety, guaranteeing no per-span road quad folds on the inside of
    a bend. That fold is the ONE root cause of the phantom inner-lane wall + AI
    stall/reverse on tight custom corners. Laplacian smoothing is applied ONLY at
    violating nodes, so gentle geometry is untouched. Positions mutate in place;
    widths preserved. `req_half[i]` overrides the effective half-width required at
    node i (used for the WIDENED branch-junction run-up, which extends past the main
    half-width); `only` restricts relaxation to that set of node indices. Returns
    the residual (0.0 == fully satisfied)."""
    n = len(nodes)
    if n < 3:
        return 0.0

    def nb(i):
        if circuit:
            return nodes[(i - 1) % n], nodes[(i + 1) % n]
        return nodes[max(0, i - 1)], nodes[min(n - 1, i + 1)]

    worst = 0.0
    for _ in range(CURVE_MAX_ITERS):
        worst = 0.0; moved = False
        lo = 0 if circuit else 1
        hi = n if circuit else n - 1
        for i in range(lo, hi):
            if only is not None and i not in only:
                continue
            a, c = nb(i); b = nodes[i]
            hw = req_half[i] if (req_half and i in req_half) else 0.5 * b["width"]
            r_min = hw * safety
            r = _node_radius(a, b, c)
            if r < r_min:
                worst = max(worst, r_min - r)
                mx = 0.5 * (a["x"] + c["x"]); mz = 0.5 * (a["z"] + c["z"])
                b["x"] += 0.35 * (mx - b["x"]); b["z"] += 0.35 * (mz - b["z"])
                moved = True
        if not moved:
            break
    if worst and worst > 1.0:
        sys.stderr.write(
            "WARNING: track has corners too tight for the road width even after "
            "curvature relaxation (residual %.0fu). Widen those corners, reduce "
            "lanes there, or raise span density; inner-lane phantom walls / AI "
            "stalls may persist.\n" % worst)
    return worst


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


def apply_road_elevation(nodes, circuit, amplitude, waves, max_grade=0.12,
                         intensity=1.0, seed=1337):
    """Set each node's Y from a smooth elevation profile so the road climbs and
    descends. The profile is a sum of two sines over the track's XZ arc-length
    with INTEGER cycle counts, so for a circuit it closes seamlessly (Y[0]==Y[-1],
    no steep seam span). Amplitude is `amplitude*intensity`; afterwards the whole
    profile is scaled down if any per-node grade (|dY/darc|) exceeds `max_grade`,
    guaranteeing a drivable road regardless of the captured amplitude. Mutates
    `nodes` in place. No-op if amplitude<=0."""
    n = len(nodes)
    amp = float(amplitude) * float(intensity)
    if n < 2 or amp <= 0.0:
        return nodes
    # cumulative XZ arc-length + total (closed for circuits)
    arc = [0.0] * n
    for i in range(1, n):
        arc[i] = arc[i-1] + math.hypot(nodes[i]["x"] - nodes[i-1]["x"],
                                       nodes[i]["z"] - nodes[i-1]["z"])
    total = arc[-1]
    if circuit:
        total += math.hypot(nodes[0]["x"] - nodes[-1]["x"],
                            nodes[0]["z"] - nodes[-1]["z"])
    if total < 1e-6:
        return nodes
    w = max(1, int(round(waves)))
    # Choose the LONGEST wavelength (fewest waves) that still lets `amp` survive
    # the grade cap, so hills stay PRONOUNCED instead of being flattened. The
    # profile's peak grade ~ amp*(2π/total)*1.4*w; solve w so that <= max_grade at
    # full amplitude. This makes a noisy captured wave count harmless (a city with
    # 26 tiny bumps becomes a few big drivable hills of the same total range).
    denom = amp * 2.0 * math.pi * 1.4
    if denom > 1e-6:
        w = max(1, min(w, int(max_grade * total / denom)))
    # deterministic phases from seed (so a track's hills are stable across builds)
    h = (int(seed) * 2654435761) & 0xFFFFFFFF
    ph1 = (h & 0xFFFF) / 65535.0 * 2 * math.pi
    ph2 = ((h >> 16) & 0xFFFF) / 65535.0 * 2 * math.pi

    def profile(f):                    # f in [0,1): periodic (integer freqs)
        return 0.6 * math.sin(2*math.pi*w*f + ph1) + 0.4 * math.sin(2*math.pi*2*w*f + ph2)

    ys = [amp * profile(arc[i] / total) for i in range(n)]
    # measure worst grade and scale to the cap
    worst = 0.0
    for i in range(n):
        j = (i + 1) % n if circuit else min(i + 1, n - 1)
        if j == i:
            continue
        da = (arc[j] - arc[i]) if j > i else (total - arc[i] + arc[j])
        if da > 1e-6:
            worst = max(worst, abs(ys[j] - ys[i]) / da)
    if worst > max_grade and worst > 0:
        s = max_grade / worst
        ys = [y * s for y in ys]
    for i in range(n):
        nodes[i]["y"] = ys[i]
    return nodes


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


def _row_points_taper(node, tangent, main_lanes, branch_lanes, lane_width, t):
    """A (main+branch)-lane row where the INNER main lanes are at full width and
    the OUTER branch lanes are extended to the RIGHT by factor t in [0,1]
    (t=0 -> collapsed onto the main road's right edge, so the road is effectively
    main_lanes wide; t=1 -> full branch width). Used to taper the road open over
    the run-up to a fork (and closed after a rejoin) so the widening is gradual
    instead of a sudden bulge. Vertex count is constant (main+branch+1) so
    consecutive spans still share edges cleanly. Returns world (x,y,z) tuples."""
    tx, tz = tangent
    lx, lz = (tz, -tx)
    half_main = main_lanes * lane_width * 0.5
    pts = []
    for k in range(main_lanes + branch_lanes + 1):
        if k <= main_lanes:
            lat = half_main - k * lane_width                       # main road (fixed)
        else:
            lat = -half_main - (k - main_lanes) * lane_width * t   # branch lanes (tapered)
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

    # [BRANCH FORK-FOLD FIX 2026-08-13] Pre-relax the fork/rejoin RUN-UP for the
    # WIDENED main+branch width. The curvature limiter (normalize_spec) only budgets
    # the MAIN half-width, but the fork widens one-sidedly to the right by
    # branch_lanes*lane_width, so the outer edge sits at 0.5*main_width +
    # branch_lanes*lane_width (up to ~2x the budgeted half-width) -> the widened rows
    # FOLD on a bend at the fork. Force the centerline radius over the run-up to hold
    # the widened width (like level007's fork on a wide/gentle section), then recompute
    # tangents so the branch geometry below is built on the relaxed line. [child]
    _req_half, _junc = {}, set()
    _Wj = max(1, BRANCH_WIDEN_SPANS)
    for _br in spec.get("branches", []):
        _u = _br.get("nodes", [])
        if not _u:
            continue
        _bl = int(_br.get("lanes", spec["default_lanes"]))
        _f = _nearest_node(nodes, {"x": float(_u[0]["x"]), "z": float(_u[0]["z"])})
        _r = _nearest_node(nodes, {"x": float(_u[-1]["x"]), "z": float(_u[-1]["z"])})
        for _anchor in (_f, _r):
            _outer = 0.5 * nodes[_anchor]["width"] + _bl * lane_width
            for _k in range(2 * _Wj + 2):
                for _i in ((_anchor - _k) % n if circuit else _anchor - _k,
                           (_anchor + _k) % n if circuit else _anchor + _k):
                    if 0 <= _i < n:
                        _junc.add(_i)
                        _req_half[_i] = max(_req_half.get(_i, 0.0), _outer)
    if _junc:
        limit_centerline_curvature(nodes, circuit, safety=CURVE_SAFETY_DEFAULT,
                                   req_half=_req_half, only=_junc)
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
        fork_outer = _row_points_taper(nodes[fork_exit], tangents[fork_exit],
                                       main_lanes, blanes, lane_width, 1.0)[main_lanes:]
        rejoin_outer = _row_points_taper(nodes[rejoin], tangents[rejoin],
                                         main_lanes, blanes, lane_width, 1.0)[main_lanes:]

        def _ctr(pts):
            return {"x": 0.5 * (pts[0][0] + pts[-1][0]),
                    "y": 0.5 * (pts[0][1] + pts[-1][1]),
                    "z": 0.5 * (pts[0][2] + pts[-1][2])}

        # The corridor centreline RUNS from the fork-outer centre to the
        # rejoin-outer centre. The user's first/last nodes only say WHERE to
        # attach (they set fork/rejoin above); keeping them as path points would
        # sit a near-duplicate next to the anchor and kink the corridor back on
        # itself (overlapping geometry at the ends). Use only the interior user
        # nodes for the shape.
        mid = user[1:-1] if len(user) >= 2 else []
        # [BRANCH CORRIDOR SHAPE FIX 2026-08-13] Leave the fork / arrive at the rejoin
        # TANGENT to the main road (native corridors are tangent-continuous), so the
        # first/last corridor quads don't twist and the path curves away+back smoothly
        # instead of chording across as a sharp bulge. Seed a guide point one lead
        # ahead of the fork along the main tangent and one before the rejoin.
        fc, rc = _ctr(fork_outer), _ctr(rejoin_outer)
        ftx, ftz = tangents[fork_exit]; rtx, rtz = tangents[rejoin]
        lead = max(spec["span_length"], lane_width)
        guide_out = {"x": fc["x"] + ftx * lead, "y": fc["y"], "z": fc["z"] + ftz * lead}
        guide_in = {"x": rc["x"] - rtx * lead, "y": rc["y"], "z": rc["z"] - rtz * lead}
        path = [fc, guide_out] + mid + [guide_in, rc]
        for bn in path:
            bn["lanes"] = blanes; bn["surface"] = bsurf; bn["width"] = bwidth
        if spec["span_length"] > 0:
            path = resample_centerline(path, spec["span_length"], circuit=False)
        # The corridor was never curvature-limited -> a sparse/chord path can fold too.
        limit_centerline_curvature(path, circuit=False, safety=CURVE_SAFETY_DEFAULT)
        if len(path) < 2:
            sys.stderr.write("WARNING: branch has < 2 nodes after resampling; skipped.\n")
            continue
        branches.append({"nodes": path, "fork": fork, "rejoin": rejoin, "lanes": blanes,
                         "main_lanes": main_lanes, "total": total,
                         "fork_outer": fork_outer, "rejoin_outer": rejoin_outer})

    # Per-node taper factor (0 = main width, 1 = full branch width) so the road
    # opens GRADUALLY over the run-up to a fork (and closes after a rejoin)
    # instead of a sudden bulge. node_taper[node] is shared by both rows of a
    # widened span, so consecutive spans share edges cleanly. widen_map marks the
    # widened span set; widen_map[span] = (main_lanes, branch_lanes, fork|None).
    # [BRANCH DRIVABILITY FIX 2026-08-13] The branch must be GENUINELY full width for
    # several spans before the fork (native level007 has ~4), not vertex-collapsed
    # until the single fork span. The strip walker advances sub_lane by crossing the
    # lateral edges between lane quads; if the outer branch lanes are zero-width over
    # the run-up, sub_lane never reaches main_lanes and the player can't peel off. So
    # PLATEAU t=1.0 across the fork and FULL spans before it, then taper over W.
    W = max(1, BRANCH_WIDEN_SPANS)
    FULL = max(1, BRANCH_WIDEN_SPANS)     # spans of REAL full width before the fork
    node_taper = {}
    widen_map = {}

    def _ramp(idx, t):
        node_taper[idx] = max(node_taper.get(idx, 0.0), t)

    for br in branches:
        M, B = br["main_lanes"], br["lanes"]
        fork, rejoin = br["fork"], br["rejoin"]
        fexit = (fork + 1) % n if circuit else min(fork + 1, n - 1)
        _ramp(fexit, 1.0)                                  # fork exit: full width (peel)
        for k in range(FULL + W + 2):                      # plateau full then taper each side
            t = 1.0 if k <= FULL else max(0.0, (FULL + W - k) / float(W))
            fi = (fork - k) % n if circuit else fork - k
            if 0 <= fi < n: _ramp(fi, t)
            ri = (rejoin + k) % n if circuit else rejoin + k
            if 0 <= ri < n: _ramp(ri, t)
        for k in range(FULL + W + 1):                      # widened span set (covers plateau)
            si = (fork - k) % nspans if circuit else fork - k
            if 0 <= si < nspans and si not in widen_map:
                widen_map[si] = (M, B, br if k == 0 else None)
            sj = (rejoin + k) % nspans if circuit else rejoin + k
            if 0 <= sj < nspans and sj not in widen_map:
                widen_map[sj] = (M, B, None)

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
            M, B, fork_br = wide
            t_near = node_taper.get(a_idx, 0.0)
            t_far = node_taper.get(b_idx, 0.0)
            near = _row_points_taper(a, tangents[a_idx], M, B, lane_width, t_near)
            far = _row_points_taper(b, tangents[b_idx], M, B, lane_width, t_far)
            lvi, rvi = _emit_span_rows(vertices, near, far, origin, si)
            total = M + B
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


# ==========================================================================
# In-game textured road: generate a models.bin (one road quad per span) + a
# single asphalt texture page, so the engine renders/textures the custom track
# instead of the flat strip-ribbon fallback (which only triggers when a level
# ships no MODELS.DAT). Verified format against real level meshes: render_type
# 259, command dispatch_type 0 / vptr 0, vertices pos/view/light/tex/proj;
# CULL_NONE so winding is free; entry index = span>>2.
# ==========================================================================
ROAD_TEXTURE_PAGE = 0
ROAD_VERTEX_LIGHT = 0xFFFFFFFF


def _mesh_tool():
    return _load_module("mesh_tool", os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "mesh_tool.py"))


def _scenery():
    return _load_module("td5_scenery", os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "td5_scenery.py"))


def _texture_reuse():
    return _load_module("td5_texture_reuse", os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "td5_texture_reuse.py"))


def _asphalt_page(seed=0x9e3779b1):
    """A 16-shade grey asphalt page: (BGR palette bytes, 4096 index bytes).
    Deterministic FNV-style hash noise so builds are reproducible."""
    pal = bytearray()
    for i in range(16):
        s = 0x30 + i * 2                      # 0x30..0x4E grey
        pal += bytes((s, s, s))               # B, G, R (engine reads B,G,R)
    idx = bytearray(4096)
    h = seed & 0xFFFFFFFF
    for p in range(4096):
        h = ((h ^ (p * 2654435761)) * 16777619) & 0xFFFFFFFF
        idx[p] = (h >> 13) & 0x0F
    return bytes(pal), bytes(idx)


def _decode_page_png(pal, idx):
    try:
        from PIL import Image
    except Exception:
        return None
    px = bytearray(64 * 64 * 4)
    for i in range(4096):
        ci = idx[i] * 3
        px[i * 4 + 0] = pal[ci + 2]           # R
        px[i * 4 + 1] = pal[ci + 1]           # G
        px[i * 4 + 2] = pal[ci + 0]           # B
        px[i * 4 + 3] = 255
    import io as _io
    buf = _io.BytesIO()
    Image.frombytes("RGBA", (64, 64), bytes(px)).save(buf, "PNG")
    return buf.getvalue()


def write_road_textures(out_dir):
    """Write textures.src/ with one opaque asphalt page (page 0), in the layout
    td5_assetsrc.c packs into TEXTURES.DAT."""
    pal, idx = _asphalt_page()
    src = os.path.join(out_dir, "textures.src")
    os.makedirs(os.path.join(src, "pages"), exist_ok=True)
    manifest = {"_format": "td5_textures", "_version": 1, "page_count": 1,
                "pages": [{"offset": 8, "pad_hex": "000000", "type": 0,
                           "palette_hex": pal.hex()}]}
    with open(os.path.join(src, "textures.json"), "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=2)
    with open(os.path.join(src, "indices.bin"), "wb") as f:
        f.write(idx)
    png = _decode_page_png(pal, idx)
    if png:
        with open(os.path.join(src, "pages", "page_000.png"), "wb") as f:
            f.write(png)


def build_road_model(header, spans, vertices, light=ROAD_VERTEX_LIGHT,
                     road_page=ROAD_TEXTURE_PAGE, span_textures=None):
    """A models 'model' (entry -> mesh-index lists) of one textured road quad per
    span, grouped 4-per-entry (entry = span>>2) so the engine's display-list walk
    finds it. `road_page` is the texture page the road samples (per-command);
    the mesh-header tag stays 0 (never a billboard) even if road_page is 1/2.
    `span_textures` is an optional {span_index: page} override for individual
    spans (e.g. a patch of a reused surface texture)."""
    span_textures = span_textures or {}
    nv = len(vertices)
    nspans = len(spans)
    n_entries = (nspans + 3) >> 2
    entries = [[] for _ in range(max(1, n_entries))]
    meshes = []
    # [WOBBLE FIX 2026-08-13] Subdivide each span's road quad into ROAD_SUBDIV
    # down-track sub-quads. A single ~1500u quad seen ~5 spans ahead foreshortens
    # into a thin screen sliver, and perspective-correct interpolation across an
    # ill-conditioned far sliver shimmers ("road markers wobbly 5 spans ahead, fine
    # up close"). Native models are finely tessellated and don't; matching that here
    # (authoring-side; the road renders from models.bin, NOT the ribbon fallback).
    ROAD_SUBDIV = int(os.environ.get("ROAD_SUBDIV", "3"))

    def _lerp(a, b, t):
        return (a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t,
                a[2] + (b[2] - a[2]) * t)

    for i in range(nspans):
        s = spans[i]
        lanes = max(1, s[3] & 0x0F)
        lvi, rvi = s[4], s[5]
        if lvi < 0 or rvi < 0 or lvi >= nv or rvi >= nv:
            continue
        rli = min(lvi + lanes, nv - 1)
        rri = min(rvi + lanes, nv - 1)
        ox, oy, oz = s[8], s[9], s[10]

        def _w(vi):
            v = vertices[vi]
            return (ox + v[0], oy + v[1], oz + v[2])

        nearL, nearR = _w(lvi), _w(rli)       # near row: left, right edge
        farL, farR = _w(rvi), _w(rri)          # far row: left, right edge
        page_i = int(span_textures.get(i, span_textures.get(str(i), road_page)))
        # ONE mesh per span carrying ROAD_SUBDIV sub-quads (one draw command with
        # quad=ROAD_SUBDIV) -- finer tessellation for the wobble fix WITHOUT tripling
        # the mesh/draw-call count.
        verts = []; allp = []
        for seg in range(ROAD_SUBDIV):
            t0 = seg / float(ROAD_SUBDIV); t1 = (seg + 1) / float(ROAD_SUBDIV)
            aL = _lerp(nearL, farL, t0); aR = _lerp(nearR, farR, t0)
            bL = _lerp(nearL, farL, t1); bR = _lerp(nearR, farR, t1)
            corners = [aL, aR, bR, bL]         # nearL, nearR, farR, farL of the sub-quad
            uvs = [(0.0, t0), (float(lanes), t0), (float(lanes), t1), (0.0, t1)]
            for p, (u, v) in zip(corners, uvs):
                verts.append({"pos": [float(p[0]), float(p[1]), float(p[2])],
                              "view": [0.0, 0.0, 0.0], "light": light,
                              "tex": [u, v], "proj": [0.0, 0.0]})
                allp.append(p)
        cx = sum(p[0] for p in allp) / len(allp)
        cy = sum(p[1] for p in allp) / len(allp)
        cz = sum(p[2] for p in allp) / len(allp)
        rad = max(math.sqrt((p[0] - cx) ** 2 + (p[1] - cy) ** 2 + (p[2] - cz) ** 2)
                  for p in allp) or 1.0
        meshes.append({"render_type": 259, "texture_page_id": 0,   # header tag: not a billboard
                       "bounding": [rad, cx, cy, cz], "origin": [0.0, 0.0, 0.0],
                       "reserved_28": 0,
                       "commands": [{"dispatch_type": 0, "texture_page_id": page_i,
                                     "reserved_04": 0, "tri": 0, "quad": ROAD_SUBDIV, "vptr": 0}],
                       "vertices": verts, "normals": None})
        entries[i >> 2].append(len(meshes) - 1)
    return {"kind": "models", "entry_count": len(entries),
            "entries": entries, "meshes": meshes}


def build_ground_apron(header, spans, vertices, page, half_width=0.0, y_drop=60.0,
                       tile=14000.0, overlap=700.0, inner=0.0, growth=45000.0,
                       light=ROAD_VERTEX_LIGHT):
    """A textured GROUND skirt following the road so there's no 'void floor'.

    Informed by the coverage detector (td5_maptrace.detect_ground_voids): the
    OUTFIELD void needs a large OUTWARD reach (to `outer` = the view range) while
    the INFIELD must be covered INWARD across to the loop interior — the opposite
    side's *outward* apron points away from the infield, so it does NOT fill it.
    So this is ASYMMETRIC: `inner` auto-sizes (<=0) to reach past the loop
    centroid (covers the infield), `outer` auto-sizes (half_width<=0) to the view
    range (covers the outfield). Lateral cells use LOD (fine near the road, coarse
    far via `growth`) so wide coverage costs few meshes; lateral + along-track
    OVERLAP hide seams. Returns scenery mesh dicts."""
    sc = _scenery()
    ring = header[1]
    nv = len(vertices)
    C = [None] * ring        # per-span world centre (y-dropped)
    U = [None] * ring        # per-span across-road unit vector, oriented OUTWARD
    valid = []
    for i in range(ring):
        s = spans[i]
        lanes = max(1, s[3] & 0x0F)
        lvi = s[4]
        if lvi < 0 or lvi >= nv:
            continue
        ri = min(lvi + lanes, nv - 1)
        L, R = vertices[lvi], vertices[ri]
        ox, oy, oz = s[8], s[9], s[10]
        lw = (ox + L[0], oy + L[1], oz + L[2])
        rw = (ox + R[0], oy + R[1], oz + R[2])
        C[i] = ((lw[0]+rw[0])*0.5, (lw[1]+rw[1])*0.5 - y_drop, (lw[2]+rw[2])*0.5)
        ux, uz = rw[0]-lw[0], rw[2]-lw[2]
        ul = math.hypot(ux, uz) or 1.0
        U[i] = (ux/ul, uz/ul)
        valid.append(i)
    if not valid:
        return []
    gx = sum(C[i][0] for i in valid) / len(valid)        # loop centroid
    gz = sum(C[i][2] for i in valid) / len(valid)
    # orient each across-vector so +t points OUTWARD (away from the centroid)
    for i in valid:
        if U[i][0]*(C[i][0]-gx) + U[i][1]*(C[i][2]-gz) < 0.0:
            U[i] = (-U[i][0], -U[i][1])
    outer = half_width if half_width > 0.0 else 130000.0  # ~ the view range + margin
    if inner <= 0.0:                                       # reach across the infield
        rad = max(math.hypot(C[i][0]-gx, C[i][2]-gz) for i in valid)
        inner = rad + 20000.0

    # LOD lateral cell edges: from -inner (inward) out to +outer, steps growing
    # with distance so far cells are big (few meshes).
    edges = [-inner]
    t = -inner
    while t < outer:
        step = tile * (1.0 + max(0.0, t) / growth)
        t = min(t + step, outer)
        edges.append(t)

    def tangent(i):
        a = C[(i-1) % ring] or C[i]; b = C[(i+1) % ring] or C[i]
        dx, dz = b[0]-a[0], b[2]-a[2]; dl = math.hypot(dx, dz) or 1.0
        return dx/dl, dz/dl

    meshes = []
    dist = 0.0
    for idx in range(len(valid)):
        i = valid[idx]
        j = valid[(idx + 1) % len(valid)]
        a, b = C[i], C[j]
        ua, ub = U[i], U[j]
        seg = math.hypot(b[0]-a[0], b[2]-a[2]) or tile
        v0, v1 = dist / tile, (dist + seg) / tile
        dist += seg
        ta, tb = tangent(i), tangent(j)
        for cidx in range(len(edges) - 1):
            t0 = edges[cidx] - overlap
            t1 = edges[cidx + 1] + overlap
            pa = (a[0]+ua[0]*t0 - ta[0]*overlap, a[1], a[2]+ua[1]*t0 - ta[1]*overlap)
            pb = (a[0]+ua[0]*t1 - ta[0]*overlap, a[1], a[2]+ua[1]*t1 - ta[1]*overlap)
            pc = (b[0]+ub[0]*t1 + tb[0]*overlap, b[1], b[2]+ub[1]*t1 + tb[1]*overlap)
            pd = (b[0]+ub[0]*t0 + tb[0]*overlap, b[1], b[2]+ub[1]*t0 + tb[1]*overlap)
            u0, u1 = t0 / tile, t1 / tile
            face = {"pts": [pa, pb, pc, pd],
                    "uv": [(u0, v0), (u1, v0), (u1, v1), (u0, v1)]}
            meshes.append(sc.make_opaque_mesh([face], int(page), light=light))
    return meshes


def _vnoise(x, z, wl):
    """Smooth value-noise in [0,1] on a lattice of wavelength wl (hash + smoothstep
    bilinear). Deterministic — same terrain every build."""
    gx = math.floor(x / wl); gz = math.floor(z / wl)
    fx = x / wl - gx; fz = z / wl - gz

    def hh(ix, iz):
        n = (int(ix) * 374761393 + int(iz) * 668265263) & 0xFFFFFFFF
        n = ((n ^ (n >> 13)) * 1274126177) & 0xFFFFFFFF
        return (n & 0xFFFF) / 65535.0
    sx = fx*fx*(3-2*fx); sz = fz*fz*(3-2*fz)
    a, b = hh(gx, gz), hh(gx+1, gz)
    c, d = hh(gx, gz+1), hh(gx+1, gz+1)
    return (a*(1-sx)+b*sx)*(1-sz) + (c*(1-sx)+d*sx)*sz


def build_ground_grid(header, spans, vertices, page, reach=0.0, step=16000.0,
                      y_drop=200.0, light=ROAD_VERTEX_LIGHT, flat_radius=22000.0,
                      slope=0.14, max_rise=22000.0, hill_amp=14000.0,
                      corridor=0.0):
    """Topology-agnostic ground fill WITH terrain relief: a seamless height-field
    grid covering everything within `reach` of the racing line. NO inward/outward
    assumption, so it closes voids for ANY shape (oval, figure-8, self-crossing,
    point-to-point). The height field is FLAT within `flat_radius` of the road (so
    the track stays drivable) then rises (valley walls, `slope`/`max_rise`) with
    value-noise hills (`hill_amp`) scaled up with distance — giving real topology
    that also OCCLUDES the far horizon (killing distant void). Heights are shared
    per grid vertex so tiles are watertight; Y follows the nearest span's road Y
    (elevation) + relief. World-tiled UVs. `reach`<=0 auto-sizes. Returns meshes."""
    sc = _scenery()
    ring = header[1]
    nv = len(vertices)
    C = []
    for i in range(ring):
        s = spans[i]
        lanes = max(1, s[3] & 0x0F)
        lvi = s[4]
        if lvi < 0 or lvi >= nv:
            continue
        ri = min(lvi + lanes, nv - 1)
        L, R = vertices[lvi], vertices[ri]
        C.append((s[8]+(L[0]+R[0])*0.5, s[9]+(L[1]+R[1])*0.5, s[10]+(L[2]+R[2])*0.5))
    if not C:
        return []
    # Keep the coarse grid OUT of the road corridor: cells within `corridor` of the
    # racing line are skipped (the fine road-edge apron covers that strip), so coarse
    # ground can never overlap/z-fight the road. Auto = widest road half + one step.
    maxhalf = 0.0
    for i in range(ring):
        s = spans[i]
        lanes = max(1, s[3] & 0x0F); lvi = s[4]
        if lvi < 0 or lvi >= nv:
            continue
        ri = min(lvi + lanes, nv - 1)
        L, R = vertices[lvi], vertices[ri]
        hw = 0.5 * math.hypot(R[0] - L[0], R[2] - L[2])
        if hw > maxhalf:
            maxhalf = hw
    if corridor <= 0.0:
        corridor = maxhalf + step
    if reach <= 0.0:
        reach = 90000.0
    sg = {}
    for c in C:
        sg.setdefault((int(c[0] // step), int(c[2] // step)), []).append(c)
    span_cells = int(reach // step) + 1

    def nearest(x, z):
        best, bd = None, 1e30
        gx, gz = int(x // step), int(z // step)
        for dx in range(-span_cells, span_cells + 1):
            for dz in range(-span_cells, span_cells + 1):
                for c in sg.get((gx + dx, gz + dz), ()):
                    d = (c[0]-x)**2 + (c[2]-z)**2
                    if d < bd:
                        bd, best = d, c
        return best, math.sqrt(bd)

    xs = [c[0] for c in C]; zs = [c[2] for c in C]
    x0, z0 = min(xs) - reach, min(zs) - reach
    nx = int((max(xs) + reach - x0) / step) + 2
    nz = int((max(zs) + reach - z0) / step) + 2
    inv = 1.0 / step
    H = {}                                   # (i,j) -> (x, z, y|None) shared vertices

    def vert(i, j):
        k = (i, j)
        v = H.get(k)
        if v is not None:
            return v
        x = x0 + i*step; z = z0 + j*step
        near, d = nearest(x, z)
        if near is None or d > reach + step:
            v = (x, z, None)
        else:
            val = d - flat_radius
            if val <= 0.0:
                relief = 0.0                                      # flat drivable zone
            else:
                trend = max(-max_rise, min(max_rise, val * slope))   # signed climb/descend
                ramp = min(1.0, val / 15000.0)                    # bumps fade in past flat edge
                # CENTERED noise => terrain goes UP and DOWN (hills + dips), mimicking
                # real tracks; two octaves for coarse + fine bumpiness.
                n = (_vnoise(x, z, 30000.0) - 0.5) + 0.5 * (_vnoise(x, z, 11000.0) - 0.5)
                relief = trend + n * hill_amp * ramp
            v = (x, z, near[1] - y_drop + relief)                 # follow road Y + relief
        H[k] = v
        return v

    meshes = []
    for i in range(nx):
        for j in range(nz):
            a, b, c, d = vert(i, j), vert(i+1, j), vert(i+1, j+1), vert(i, j+1)
            if a[2] is None or b[2] is None or c[2] is None or d[2] is None:
                continue
            cx = 0.25 * (a[0] + b[0] + c[0] + d[0])       # a = (x, z, y)
            cz = 0.25 * (a[1] + b[1] + c[1] + d[1])
            _, dc = nearest(cx, cz)
            if dc < corridor:                              # over/beside road -> apron covers it
                continue
            pts = [(a[0], a[2], a[1]), (b[0], b[2], b[1]),
                   (c[0], c[2], c[1]), (d[0], d[2], d[1])]        # (x, y, z)
            uv = [(a[0]*inv, a[1]*inv), (b[0]*inv, b[1]*inv),
                  (c[0]*inv, c[1]*inv), (d[0]*inv, d[1]*inv)]
            meshes.append(sc.make_opaque_mesh([{"pts": pts, "uv": uv}], int(page),
                                              light=light))
    return meshes


def write_textured_road(out_dir, header, spans, vertices, road_page=ROAD_TEXTURE_PAGE,
                        span_textures=None):
    """Write models.bin (textured road) + textures.src/ asphalt page. Returns the
    mesh count, or 0 if nothing was generated. `road_page` overrides the sampled
    road texture page (e.g. a page imported from another track); `span_textures`
    is an optional per-span {span: page} override."""
    model = build_road_model(header, spans, vertices, road_page=road_page,
                             span_textures=span_textures)
    if not model["meshes"]:
        return 0
    blob = _mesh_tool().build_dat(model)
    with open(os.path.join(out_dir, "models.bin"), "wb") as f:
        f.write(blob)
    write_road_textures(out_dir)
    return len(model["meshes"])


def build_guardrails(header, spans, vertices, page, *, height=750.0, offset=120.0,
                     left=True, right=True):
    """Emit per-span barrier-wall segments hugging the road's left/right rails (the
    ring only). OPT-IN visual guardrail: it sits at the road edge where the engine's
    rail-wall collision already is, turning the otherwise-invisible edge wall into a
    visible barrier. One mesh per span per side so each single-homes to its local
    road entry and renders continuously as the span-window walk advances. Returns a
    list of scenery mesh dicts (empty if both sides off)."""
    sc = _scenery()
    ring = header[1]

    def edge(si):
        sp = spans[si]
        lanes = max(1, sp[3] & 0x0F); lvi = sp[4]
        ox, oy, oz = sp[8], sp[9], sp[10]
        vl = vertices[lvi]; vr = vertices[lvi + lanes]
        L = (ox + vl[0], oy + vl[1], oz + vl[2])
        R = (ox + vr[0], oy + vr[1], oz + vr[2])
        # push each rail outward along the cross-road direction so the barrier
        # sits just outside the drivable surface (no z-fight with the road edge)
        cx, cz = L[0] - R[0], L[2] - R[2]
        d = math.hypot(cx, cz) or 1.0
        ux, uz = cx / d, cz / d
        Lo = (L[0] + ux * offset, L[1], L[2] + uz * offset)
        Ro = (R[0] - ux * offset, R[1], R[2] - uz * offset)
        return Lo, Ro

    meshes = []
    for si in range(ring):
        nj = (si + 1) % ring
        aL, aR = edge(si); bL, bR = edge(nj)
        if left:
            meshes.append(sc.make_guardrail_segment(aL, bL, page, height=height))
        if right:
            meshes.append(sc.make_guardrail_segment(aR, bR, page, height=height))
    return meshes


def build_road_edge_apron(header, spans, vertices, page, *, circuit=True,
                          reach=36000.0, outer_drop=200.0, tile=14000.0,
                          light=ROAD_VERTEX_LIGHT):
    """A FINE per-span ground ribbon whose INNER edge is the road's exact left/right
    EDGE vertex (shared XZ *and* Y) and which extends OUTWARD by `reach`. Because it
    shares the edge and only ever grows AWAY from the road, it meets the road with no
    gap and can never overlap/z-fight the drivable surface (unlike the coarse grid).
    One quad per span per side, multi-homed like ground; fills the corridor keep-out
    that build_ground_grid leaves, out to where the coarse grid resumes."""
    sc = _scenery()
    ring = header[1]
    nv = len(vertices)

    def edges(si):
        s = spans[si]
        lanes = max(1, s[3] & 0x0F); lvi = s[4]
        if lvi < 0 or lvi >= nv:
            return None
        ri = min(lvi + lanes, nv - 1)
        L, R = vertices[lvi], vertices[ri]
        ox, oy, oz = s[8], s[9], s[10]
        Lw = (ox + L[0], oy + L[1], oz + L[2])          # exact road LEFT edge (x,y,z)
        Rw = (ox + R[0], oy + R[1], oz + R[2])          # exact road RIGHT edge (x,y,z)
        cx, cz = Lw[0] - Rw[0], Lw[2] - Rw[2]           # cross-road dir (toward LEFT)
        d = math.hypot(cx, cz) or 1.0
        ux, uz = cx / d, cz / d
        Lo = (Lw[0] + ux * reach, Lw[1] - outer_drop, Lw[2] + uz * reach)   # outward + down
        Ro = (Rw[0] - ux * reach, Rw[1] - outer_drop, Rw[2] - uz * reach)
        return Lw, Rw, Lo, Ro

    meshes = []
    last = ring if circuit else ring - 1
    v = 0.0
    w = reach / tile
    for idx in range(last):
        a = edges(idx); b = edges((idx + 1) % ring)
        if a is None or b is None:
            continue
        La, Ra, Lao, Rao = a
        Lb, Rb, Lbo, Rbo = b
        seg = math.hypot(Lb[0] - La[0], Lb[2] - La[2]) or tile
        v0, v1 = v / tile, (v + seg) / tile
        v += seg
        left  = {"pts": [La, Lao, Lbo, Lb],
                 "uv":  [(0.0, v0), (w, v0), (w, v1), (0.0, v1)]}
        right = {"pts": [Ra, Rb, Rbo, Rao],
                 "uv":  [(0.0, v0), (0.0, v1), (w, v1), (w, v0)]}
        meshes.append(sc.make_opaque_mesh([left],  int(page), light=light))
        meshes.append(sc.make_opaque_mesh([right], int(page), light=light))
    return meshes


def snap_scenery_to_terrain(meshes, header, spans, vertices, ground_offset=-150.0):
    """Shift each AUTHORED scenery mesh in Y so it sits on the local terrain (the
    nearest ring span's road Y + ground_offset), instead of floating at the Y=0 it
    was authored at while the road/ground undulate with elevation (range can be
    thousands of units -> buildings/trees float or sink badly). Handles both opaque
    meshes (origin 0, world-space verts) and billboards (origin = world*256, local
    verts; header texture_page_id 1/2). Mutates in place. Ground/guardrails already
    derive Y from the road, so only pass AUTHORED scenery here."""
    ring = header[1]; nv = len(vertices)
    centers = []
    for i in range(ring):
        s = spans[i]; lanes = max(1, s[3] & 0x0F); lvi = s[4]
        if lvi < 0 or lvi >= nv:
            continue
        L = vertices[lvi]; R = vertices[min(lvi + lanes, nv - 1)]
        centers.append((s[8] + (L[0] + R[0]) * 0.5,
                        s[9] + (L[1] + R[1]) * 0.5,
                        s[10] + (L[2] + R[2]) * 0.5))
    if not centers:
        return meshes
    for m in meshes:
        bc = m["bounding"]; mx, mz = bc[1], bc[3]
        best = min(centers, key=lambda c: (c[0] - mx) ** 2 + (c[2] - mz) ** 2)
        dy = best[1] + ground_offset                     # scenery authored at Y=0
        if abs(dy) < 1.0:
            continue
        if int(m.get("texture_page_id", 0)) in (1, 2):   # billboard: origin is 24.8
            m["origin"][1] = float(m["origin"][1]) + dy * 256.0
        else:                                            # opaque: world-space verts
            for v in m["vertices"]:
                v["pos"][1] = float(v["pos"][1]) + dy
        m["bounding"][2] = float(m["bounding"][2]) + dy
    return meshes


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

    # Textured in-game road: generate models.bin + an asphalt texture page so the
    # engine renders/textures the track instead of the flat strip-ribbon fallback.
    # Default on; set spec["textured"]=False to ship a fallback-only (flat) track.
    if spec.get("textured", True):
        try:
            road_page = int(spec.get("road_texture", ROAD_TEXTURE_PAGE))
            n = write_textured_road(out_dir, header, spans, vertices, road_page=road_page,
                                    span_textures=spec.get("span_textures") or {})
            if n:
                sys.stderr.write("generated textured road: %d span meshes + asphalt page\n" % n)
        except Exception as e:                       # never block a build on the mesh gen
            for stale in ("models.bin",):
                p = os.path.join(out_dir, stale)
                if os.path.isfile(p):
                    os.remove(p)
            sys.stderr.write("textured-road generation skipped (%s); using flat fallback\n" % e)

    # Texture reuse: import pages from other levels into this level's pool, in
    # order, so they get deterministic ids (asphalt page 0, imports 1,2,...).
    # Runs after the road pool is written and BEFORE scenery, which references
    # the imported ids. Soft: never block a build.
    timports = spec.get("texture_imports") or []
    if timports:
        try:
            tr = _texture_reuse()
            for imp in timports:
                src_dir = os.path.join(levels_dir(assets_root),
                                       "level%03d" % int(imp["source_level"]))
                new_id = tr.import_texture_page(out_dir, src_dir, int(imp["source_page"]))
                sys.stderr.write("imported texture: level%03d page %d -> id %d\n"
                                 % (int(imp["source_level"]), int(imp["source_page"]), new_id))
        except Exception as e:
            sys.stderr.write("texture import skipped (%s)\n" % e)

    # Ground-fill apron: a continuous textured ground ribbon following the road
    # so there is no 'void floor' beside/under the thin road. Built here (spans/
    # vertices in hand) after texture imports so the ground page id is valid.
    apron_meshes = []
    grid_meshes = []
    gf = spec.get("ground_fill")
    if gf:
        try:
            # Fine road-edge apron (inner edge == road edge vertex, grows outward)
            # fills the road corridor with NO overlap of the drivable surface; the
            # coarse grid fills the far field with that corridor cut out. Together:
            # watertight, and grass never shares the road's space (no poke-through /
            # z-fight / disappear).
            apron = build_road_edge_apron(
                header, spans, vertices, int(gf["page"]),
                circuit=bool(spec.get("circuit", True)),
                reach=float(gf.get("apron_reach", 36000.0)),
                outer_drop=float(gf.get("apron_drop", 190.0)))     # ~10u above grid
            grid = build_ground_grid(
                header, spans, vertices, int(gf["page"]),
                reach=float(gf.get("half_width", 0.0)),
                corridor=float(gf.get("corridor", 0.0)),           # auto = maxhalf + step
                y_drop=float(gf.get("y_drop", 200.0)),
                flat_radius=float(gf.get("flat_radius", 22000.0)),
                slope=float(gf.get("slope", 0.14)),
                max_rise=float(gf.get("max_rise", 22000.0)),
                hill_amp=float(gf.get("hill_amp", 14000.0)))
            apron_meshes = apron
            grid_meshes = grid
            sys.stderr.write("ground fill: %d apron ribbon + %d grid quads (page %d)\n"
                             % (len(apron), len(grid), int(gf["page"])))
        except Exception as e:
            sys.stderr.write("ground fill skipped (%s)\n" % e)

    # Compose into models.bin. Buildings/props SINGLE-home to their nearest road
    # entry; GROUND tiles MULTI-home into every nearby entry so the span-window
    # render walk shows them from every along-track position physically near them
    # (fixes the winding-track "grass slides with the player" culling artifact --
    # a grid tile's nearest entry can be on the opposite arm of a loop). Soft:
    # never block a build on scenery.
    # Optional guardrails (opt-in): barrier strips along the rails, built from the
    # ring's rail vertices, folded in as single-homed scenery like buildings.
    gr = spec.get("guardrails")
    guardrail_meshes = []
    if gr:
        try:
            guardrail_meshes = build_guardrails(
                header, spans, vertices, int(gr["page"]),
                height=float(gr.get("height", 750.0)),
                offset=float(gr.get("offset", 120.0)),
                left=bool(gr.get("left", True)), right=bool(gr.get("right", True)))
            sys.stderr.write("guardrails: %d segments\n" % len(guardrail_meshes))
        except Exception as e:
            sys.stderr.write("guardrails skipped (%s)\n" % e)

    # Authored scenery (buildings/trees/banner) is placed at Y=0; snap it onto the
    # elevated terrain so it doesn't float/sink with the road elevation. Guardrails
    # already derive Y from the rail vertices, so they are NOT snapped.
    authored = [dict(m) for m in (spec.get("scenery") or [])]
    if authored:
        snap_scenery_to_terrain(authored, header, spans, vertices)
    # SINGLE-home the per-span-local meshes (buildings, guardrails, AND the fine
    # apron ribbon — each belongs to one local road entry, like the road itself).
    # Only the COARSE grid tiles multi-home (a grid tile's nearest entry can be on
    # the opposite arm of a loop). Multi-homing the fine apron would list each
    # segment in ~reach/span entries -> per-frame mesh count explosion -> ~12 FPS.
    static_scenery = authored + guardrail_meshes + apron_meshes
    if grid_meshes or static_scenery:
        try:
            sc = _scenery()
            models_path = os.path.join(out_dir, "models.bin")
            if os.path.isfile(models_path):
                model = sc.load_road_model(out_dir)
            else:                                     # flat track: scenery-only model
                model = {"kind": "models", "entry_count": 1,
                         "entries": [[]], "meshes": []}
            if static_scenery:
                sc.compose_models(model, [dict(m) for m in static_scenery])
            if grid_meshes:
                sc.attach_ground_meshes(
                    model, [dict(m) for m in grid_meshes],
                    reach=float((gf or {}).get("attach_reach", 48000.0)))
            total = sc.write_models_bin(out_dir, model)
            sys.stderr.write("composed %d apron + %d grid + %d scenery mesh(es) into "
                             "models.bin (total %d meshes)\n"
                             % (len(apron_meshes), len(grid_meshes),
                                len(static_scenery) - len(apron_meshes), total))
        except Exception as e:
            sys.stderr.write("scenery composition skipped (%s)\n" % e)

    # Breakable props: LEVEL.MOV instance table + PROPMESH.BIN mesh blob.
    mov = spec.get("mov_instances") or []
    pmeshes = spec.get("prop_meshes") or []
    if mov or pmeshes:
        try:
            sc = _scenery()
            if pmeshes:
                sc.write_prop_meshes(out_dir, pmeshes)
            if mov:
                sc.write_mov(out_dir, mov)
            sys.stderr.write("wrote %d breakable prop instance(s), %d prop mesh(es)\n"
                             % (len(mov), len(pmeshes)))
        except Exception as e:
            sys.stderr.write("breakable props skipped (%s)\n" % e)
    else:
        # No props this build: DELETE any stale LEVEL.MOV / PROPMESH.BIN from an
        # earlier build, or the engine still loads them (untextured model-0 props
        # render as white boxes, and they no longer align with the road after a
        # centerline change). build() is meant to be deterministic per slot.
        removed = 0
        for stale in ("LEVEL.MOV", "PROPMESH.BIN", "LEVEL.TCL"):
            p = os.path.join(out_dir, stale)
            if os.path.isfile(p):
                os.remove(p); removed += 1
        if removed:
            sys.stderr.write("removed %d stale breakable-prop file(s)\n" % removed)
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
# Programmatic API (used by the Track Studio web tool)
# ==========================================================================
def build_track(spec_raw, assets_root=None, slot=None, level=None):
    """Build a track from a neutral centerline spec (dict) and register it.
    Pass slot/level to overwrite an existing custom track (editing); omit to
    auto-assign. Returns a result dict. Used by td5_track_studio.py."""
    assets_root = assets_root or _default_assets_root()
    spec = normalize_spec(spec_raw)
    level_no = pick_level_number(assets_root, level)
    manifest = load_manifest(assets_root)
    slot_n = pick_slot(manifest, slot)
    out_dir, nspans = write_level(assets_root, level_no, spec)
    entry, manifest_path = update_manifest(assets_root, slot_n, level_no, spec, nspans)
    return {"ok": True, "slot": slot_n, "level": level_no, "dir": out_dir,
            "spans": nspans, "circuit": bool(spec["circuit"]), "entry": entry}


def extract_track(assets_root, level_no, name=None, decimate_to=72):
    """Reverse an existing levelNNN/ into an editable centerline spec (main ring).

    A centerline node is the midpoint of each span's left/right rail; lane count,
    width and surface come from the span; circuit flag, checkpoints, weather and
    fog come from levelinf.json. The dense per-span centerline is decimated to
    ~decimate_to nodes for editing (the build resamples on the way back out).
    Branch corridors (spans past the ring) are NOT extracted yet -- a warning is
    returned. Returns (spec_dict, warnings_list).
    """
    assets_root = assets_root or _default_assets_root()
    ld = os.path.join(levels_dir(assets_root), "level%03d" % int(level_no))
    with open(os.path.join(ld, "strip.json"), encoding="utf-8") as f:
        strip = json.load(f)
    hdr = strip["header"]; spans = strip["spans"]; verts = strip["vertices"]
    ring = max(1, hdr[1]); total = hdr[4]
    nv = len(verts)
    dense = []
    dense_span = []                 # parallel: which ring span each dense node came from
    for i in range(ring):
        s = spans[i]
        lanes = s[3] & 0x0F
        if lanes < 1:
            lanes = 1
        lvi = s[4]
        if lvi < 0 or lvi >= nv:
            continue
        ri = min(lvi + lanes, nv - 1)
        L = verts[lvi]; R = verts[ri]
        ox, oy, oz = s[8], s[9], s[10]
        cx = ox + (L[0] + R[0]) * 0.5
        cy = oy + (L[1] + R[1]) * 0.5
        cz = oz + (L[2] + R[2]) * 0.5
        width = math.hypot(R[0] - L[0], R[2] - L[2]) or (lanes * DEFAULT_LANE_WIDTH)
        dense.append({"x": round(cx, 1), "z": round(cz, 1), "y": round(cy, 1),
                      "lanes": lanes, "width": round(width, 1), "surface": s[1] & 0x0F})
        dense_span.append(i)
    # decimate evenly for editing (keep the shape, drop the per-span density)
    if decimate_to and len(dense) > decimate_to:
        step = len(dense) / float(decimate_to)
        pick = [int(k * step) for k in range(decimate_to)]
    else:
        pick = list(range(len(dense)))
    nodes = [dense[d] for d in pick]
    if len(nodes) < 2:
        raise ValueError("level %d has too few usable spans to extract" % level_no)

    spec = {"name": (name or ("LEVEL %d" % level_no)).upper()[:30],
            "circuit": True, "nodes": nodes, "checkpoints": "auto:4",
            "weather": 2, "fog": {"enabled": 0, "r": 0, "g": 0, "b": 0},
            "traffic_enable": 0, "branches": []}

    li_path = os.path.join(ld, "levelinf.json")
    if os.path.isfile(li_path):
        with open(li_path, encoding="utf-8") as f:
            li = json.load(f)

        def _v(key, dflt=0):
            x = li.get(key)
            if isinstance(x, dict):
                x = x.get("value", dflt)
            return dflt if x is None else x

        spec["circuit"] = (_v("track_type", 1) == 1)
        spec["weather"] = int(_v("weather_type", 2))
        spec["traffic_enable"] = int(_v("traffic_enable", 0))
        if int(_v("fog_enabled", 0)):
            spec["fog"] = {"enabled": 1, "r": int(_v("fog_color_r", 0)),
                           "g": int(_v("fog_color_g", 0)), "b": int(_v("fog_color_b", 0))}

        # map the real checkpoint span indices onto the nearest decimated node so
        # the editor's checkpoint markers sit where the track's checkpoints are.
        cps = _v("checkpoint_spans", []) or []
        cpn = int(_v("checkpoint_count", 0))
        real = [int(c) for c in (cps[:cpn] if cpn else cps) if 0 < int(c) < ring]
        if real and dense_span:
            cpnodes = []
            for sp in real:
                d = min(range(len(dense_span)), key=lambda j: abs(dense_span[j] - sp))
                k = min(range(len(pick)), key=lambda j: abs(pick[j] - d))
                cpnodes.append(k)
            cpnodes = sorted(set(i for i in cpnodes if 0 < i < len(nodes)))
            if cpnodes:
                spec["checkpoints"] = cpnodes

    # --- branch corridors: each type-9 (SENTINEL_START) .. type-10 (SENTINEL_END)
    # run in [ring, total) is a branch; its per-span rail-midpoint centreline
    # becomes an editable branch node list (decimated). The rebuild re-anchors it
    # to the nearest main node, so an imported fork survives a round-trip. ---
    warnings = []
    branches = []
    i = ring
    while i < total:
        if spans[i][0] == 9:
            j = i + 1
            while j < total and spans[j][0] != 10:
                j += 1
            end = j if j < total else total - 1
            bn = []
            for k in range(i, end + 1):
                s = spans[k]
                bl = s[3] & 0x0F or 1
                lvi = s[4]
                if lvi < 0 or lvi >= nv:
                    continue
                rr = min(lvi + bl, nv - 1)
                L = verts[lvi]; R = verts[rr]
                bn.append({"x": round(s[8] + (L[0] + R[0]) * 0.5, 1),
                           "y": round(s[9] + (L[1] + R[1]) * 0.5, 1),
                           "z": round(s[10] + (R[2] + L[2]) * 0.5, 1)})
            if len(bn) >= 2:
                if len(bn) > 24:
                    st = len(bn) / 24.0
                    bn = [bn[int(q * st)] for q in range(24)]
                branches.append({"lanes": spans[i][3] & 0x0F or 3, "nodes": bn})
            i = end + 1
        else:
            i += 1
    spec["branches"] = branches
    if total > ring and not branches:
        warnings.append("track has %d branch span(s) but no type-9/10 corridor was "
                        "found; branches not imported" % (total - ring))
    elif branches:
        warnings.append("imported %d branch(es) -- re-check the fork/rejoin after editing"
                        % len(branches))
    return spec, warnings


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
