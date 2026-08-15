"""server.py -- MCP server for AI-assisted TD5RE track AUTHORING.

Distinct from scripts/td5re_mcp (which drives a RUNNING game): this server wraps
the OFFLINE authoring engine in re/tools (td5_trackgen, td5_scenery,
td5_texture_reuse, td5_describe) so an AI can build/densify custom tracks
conversationally, then race them via td5re_mcp or `--DefaultTrack=<slot>`.

Design: PRIMITIVE tools + the model does the spatial reasoning. The server holds
one in-memory WORKING TRACK (a neutral centerline spec + scenery list + texture
plan). You mutate it with small verbs, then `build()` flushes the whole thing to
re/assets/levels/levelNNN/ and registers it in custom_tracks.json -- no recompile.
`build()` is deterministic: it re-applies the full session each time, so editing
then rebuilding the same slot is stable (imported pages + scenery are not lost).

Register (project-local .mcp.json in the TD5RE repo root):

    {
      "mcpServers": {
        "track_editor": { "command": "python",
                          "args": ["scripts/track_editor_mcp/server.py"] }
      }
    }

Requires: `pip install mcp`, plus numpy+pillow for the engine (mesh/texture I/O).
The engine modules can also be driven directly from a Python REPL (see README).
"""
import math
import os
import sys
from typing import Any, Dict, List, Optional

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
TOOLS_DIR = os.path.join(REPO_ROOT, "re", "tools")
sys.path.insert(0, TOOLS_DIR)

from mcp.server.fastmcp import FastMCP

import td5_trackgen as tg
import td5_scenery as sc
import td5_texture_reuse as tr
import td5_describe as desc
import td5_maptrace as mtr
import td5_assetlib as al

mcp = FastMCP("track_editor")

ASPHALT_PAGES = 1          # write_road_textures emits one asphalt page (id 0)


# --- working-session state ------------------------------------------------
def _new_session(name: str = "CUSTOM TRACK", circuit: bool = True) -> Dict[str, Any]:
    return {
        "name": name.upper()[:30], "circuit": bool(circuit),
        "lane_width": tg.DEFAULT_LANE_WIDTH, "default_lanes": tg.DEFAULT_LANES,
        "default_surface": tg.DEFAULT_SURFACE,
        "weather": 2, "smoke": 1, "traffic_enable": 0,
        "fog": {"enabled": 0, "r": 0, "g": 0, "b": 0},
        "checkpoints": "auto:4",
        "nodes": [], "branches": [],
        "texture_imports": [], "road_texture": 0, "span_textures": {},
        "ground_fill": None,               # {page, half_width} continuous ground
        "road_elevation": None,            # {amplitude, waves, max_grade, intensity}
        "guardrails": None,                # {page, height, offset, left, right} OPT-IN barriers
        "scenery": [], "mov_instances": [], "prop_meshes": [],
        "prototypes": {}, "proto_next": 0,   # reusable-geometry library
        "tex_map": {},                       # (source_level, source_page) -> target page id (dedup)
        "slot": None, "level": None,       # remembered after first build
    }


def _import_page(source_level, source_page):
    """Record a source-page import (deferred to build) and return its projected
    target id, deduping repeats of the same (source_level, source_page)."""
    key = (int(source_level), int(source_page))
    if key not in _S["tex_map"]:
        nid = ASPHALT_PAGES + len(_S["texture_imports"])
        _S["texture_imports"].append({"source_level": int(source_level),
                                      "source_page": int(source_page)})
        _S["tex_map"][key] = nid
    return _S["tex_map"][key]


_S: Dict[str, Any] = _new_session()


def _spec_raw() -> Dict[str, Any]:
    """Assemble the full spec dict the engine consumes from the session."""
    keys = ("name", "circuit", "lane_width", "default_lanes", "default_surface",
            "weather", "smoke", "traffic_enable", "fog", "checkpoints", "nodes",
            "branches", "texture_imports", "road_texture", "span_textures",
            "ground_fill", "road_elevation", "guardrails", "scenery",
            "mov_instances", "prop_meshes")
    return {k: _S[k] for k in keys}


def _drive_cmd(slot: int) -> str:
    return "td5re.exe --AutoRace=1 --SkipIntro=1 --DefaultTrack=%d" % slot


def _projected_texture_id() -> int:
    """Deterministic id an about-to-be-recorded import will get in the built
    pool: asphalt page(s) first, then imports in call order."""
    return ASPHALT_PAGES + len(_S["texture_imports"])


def _level_dir(level: int) -> str:
    return os.path.join(tg.levels_dir(tg._default_assets_root()),
                        "level%03d" % int(level))


# --- session / track ------------------------------------------------------
@mcp.tool()
def new_track(name: str = "CUSTOM TRACK", circuit: bool = True) -> Dict[str, Any]:
    """Start a fresh working track (clears the session). `circuit`=True is a
    closed loop; False is point-to-point. Add centerline with add_node."""
    global _S
    _S = _new_session(name, circuit)
    return {"ok": True, "name": _S["name"], "circuit": _S["circuit"]}


@mcp.tool()
def load_sample(kind: str = "oval", name: Optional[str] = None) -> Dict[str, Any]:
    """Seed the session from a built-in sample centerline: oval | figure8 |
    straight. A good starting point to then edit/densify."""
    try:
        spec = tg.sample_spec(kind, name)
    except ValueError as e:
        return {"ok": False, "error": str(e)}
    global _S
    _S = _new_session(spec["name"], spec["circuit"])
    _S["nodes"] = spec["nodes"]
    _S["checkpoints"] = spec["checkpoints"]
    _S["weather"] = spec["weather"]
    return {"ok": True, "name": _S["name"], "nodes": len(_S["nodes"]),
            "circuit": _S["circuit"]}


@mcp.tool()
def import_track(level: int, name: Optional[str] = None) -> Dict[str, Any]:
    """Load an existing levelNNN into the session as an editable centerline (for
    densifying/extending a map). Scenery/props are NOT pulled in -- use
    describe_track to see what's already there, then add around it."""
    try:
        spec, warnings = tg.extract_track(tg._default_assets_root(), int(level), name)
    except Exception as e:
        return {"ok": False, "error": str(e)}
    global _S
    _S = _new_session(spec["name"], spec["circuit"])
    _S["nodes"] = spec["nodes"]
    _S["branches"] = spec.get("branches", [])
    _S["checkpoints"] = spec.get("checkpoints", "auto:4")
    _S["weather"] = spec.get("weather", 2)
    _S["fog"] = spec.get("fog", _S["fog"])
    _S["traffic_enable"] = spec.get("traffic_enable", 0)
    return {"ok": True, "name": _S["name"], "nodes": len(_S["nodes"]),
            "circuit": _S["circuit"], "branches": len(_S["branches"]),
            "warnings": warnings}


@mcp.tool()
def get_spec() -> Dict[str, Any]:
    """Return the current working track: node count, environment, and counts of
    scenery / texture imports / breakable props / span-texture overrides."""
    return {"ok": True, "name": _S["name"], "circuit": _S["circuit"],
            "nodes": len(_S["nodes"]), "branches": len(_S["branches"]),
            "checkpoints": _S["checkpoints"], "weather": _S["weather"],
            "fog": _S["fog"], "traffic_enable": _S["traffic_enable"],
            "road_texture": _S["road_texture"],
            "texture_imports": _S["texture_imports"],
            "span_textures": _S["span_textures"],
            "scenery_meshes": len(_S["scenery"]),
            "breakable_instances": len(_S["mov_instances"]),
            "prop_meshes": len(_S["prop_meshes"]),
            "prototypes": len(_S["prototypes"]),
            "slot": _S["slot"], "level": _S["level"]}


@mcp.tool()
def list_tracks() -> Dict[str, Any]:
    """List registered custom tracks (manifest) and all importable levelNNN."""
    root = tg._default_assets_root()
    man = tg.load_manifest(root)
    ld = tg.levels_dir(root)
    levels = sorted(int(n[5:8]) for n in os.listdir(ld)
                    if n.startswith("level") and n[5:8].isdigit())
    return {"ok": True, "custom_tracks": man.get("tracks", []), "levels": levels}


@mcp.tool()
def describe_track(level: int) -> Dict[str, Any]:
    """Read-only inventory of an existing levelNNN: world extents, per-span
    center/width/lanes/surface, checkpoints, existing props, texture pages, and
    which pages the model already uses. Use this to reason about where to add
    geometry when densifying a map."""
    try:
        return {"ok": True, **desc.describe_track(None, int(level))}
    except Exception as e:
        return {"ok": False, "error": str(e)}


@mcp.tool()
def trace_geometry(level: int, span_start: Optional[int] = None,
                   span_end: Optional[int] = None,
                   max_objects: int = 80) -> Dict[str, Any]:
    """Deep spatial trace of a level's world geometry: a summary (mesh/object
    counts, class histogram, world extents, pages), a per-span-bucket PROFILE
    (how much scenery lines each stretch + its median height/setback/footprint +
    pages used — the CONTEXT for matching new geometry), and the clustered scenery
    OBJECTS (each with class, world center, footprint, height, texture pages,
    nearest span, side, setback). Native cities have thousands of meshes, so pass
    span_start/span_end to focus on a stretch; objects are capped at max_objects."""
    r = mtr.trace_geometry(None, int(level))
    if not r.get("ok"):
        return r
    objs = r["objects"]
    if span_start is not None or span_end is not None:
        lo = span_start if span_start is not None else -1
        hi = span_end if span_end is not None else 10 ** 9
        objs = [o for o in objs if lo <= o["span"] <= hi]
    r["object_total"] = len(objs)
    r["objects"] = objs[:max_objects]
    return r


@mcp.tool()
def trace_textures(level: int) -> Dict[str, Any]:
    """Decode + classify every texture page of a level and render a labeled
    MONTAGE contact-sheet PNG (grid of all pages tagged with id + class:
    asphalt/concrete/wall/foliage/sky/sign/...). Returns the montage path and the
    per-page classification list. READ the montage image to SEE the available art
    and pick pages that match a surface you're extending (then import_texture)."""
    return mtr.trace_textures(None, int(level))


@mcp.tool()
def analyze_gaps(level: int, min_gap_spans: int = 8) -> Dict[str, Any]:
    """Find EMPTY roadside stretches to fill (span ranges + side + length) plus a
    STYLE profile (typical building height/footprint/setback + common pages) so
    new geometry matches its surroundings. Feed this + trace_geometry to decide
    where and how to fill, then place matching scenery via place_box/place_model/
    add_billboard (reuse the profile's pages via import_texture)."""
    return mtr.analyze_gaps(None, int(level), min_gap_spans=int(min_gap_spans))


@mcp.tool()
def detect_voids(level: int, far_cull: int = 195000) -> Dict[str, Any]:
    """Measure ACTUAL ground/floor coverage around a track (not roadside object
    density like analyze_gaps): for distance bands out to far_cull, report the %
    of the floor that's covered by geometry, split inward (loop interior) vs
    outward (outfield), and the distance where coverage first drops below 95% (=
    where void floor begins). Use it to see void regions and to know how far /
    which direction to extend ground so the world isn't empty out to the view
    range. Complements analyze_gaps (which is blind to floor voids)."""
    try:
        return {"ok": True, **mtr.detect_ground_voids(None, int(level),
                                                      far_cull=int(far_cull))}
    except Exception as e:
        return {"ok": False, "error": str(e)}


@mcp.tool()
def build(slot: Optional[int] = None, level: Optional[int] = None) -> Dict[str, Any]:
    """Flush the working track to re/assets/levels/levelNNN/ and register it.
    Omit slot/level to auto-assign (first build) or reuse the session's remembered
    slot/level (rebuild). Returns the slot, level, span count and a drive command.
    Needs >=3 nodes for a circuit, >=2 for point-to-point."""
    if len(_S["nodes"]) < (3 if _S["circuit"] else 2):
        return {"ok": False, "error": "need >=3 nodes for a circuit, >=2 for "
                "point-to-point (have %d)" % len(_S["nodes"])}
    use_slot = slot if slot is not None else _S["slot"]
    use_level = level if level is not None else _S["level"]
    try:
        res = tg.build_track(_spec_raw(), slot=use_slot, level=use_level)
    except Exception as e:
        return {"ok": False, "error": str(e)}
    _S["slot"], _S["level"] = res["slot"], res["level"]
    return {"ok": True, "slot": res["slot"], "level": res["level"],
            "spans": res["spans"], "circuit": res["circuit"],
            "dir": res["dir"], "drive_cmd": _drive_cmd(res["slot"])}


@mcp.tool()
def reset_session() -> Dict[str, Any]:
    """Discard the working track and start empty."""
    global _S
    _S = _new_session()
    return {"ok": True}


# --- road geometry --------------------------------------------------------
def _node(x, z, y=0.0, lanes=None, width=None, surface=None):
    n: Dict[str, Any] = {"x": float(x), "z": float(z), "y": float(y)}
    if lanes is not None:
        n["lanes"] = int(lanes)
    if width is not None:
        n["width"] = float(width)
    if surface is not None:
        n["surface"] = int(surface)
    return n


@mcp.tool()
def add_node(x: float, z: float, y: float = 0.0, lanes: Optional[int] = None,
             width: Optional[float] = None, surface: Optional[int] = None) -> Dict[str, Any]:
    """Append a centerline node (world units; a lane is ~1500 wide). Optional
    per-node lanes/width/surface (0 dry,1 wet,2 dirt,3 gravel)."""
    _S["nodes"].append(_node(x, z, y, lanes, width, surface))
    return {"ok": True, "nodes": len(_S["nodes"]), "index": len(_S["nodes"]) - 1}


@mcp.tool()
def add_nodes(points: List[List[float]]) -> Dict[str, Any]:
    """Append many centerline nodes at once. `points` is a list of [x, z] or
    [x, z, y]. Convenient for laying a whole path in one call."""
    for p in points:
        _S["nodes"].append(_node(p[0], p[1], p[2] if len(p) > 2 else 0.0))
    return {"ok": True, "nodes": len(_S["nodes"])}


@mcp.tool()
def insert_node(index: int, x: float, z: float, y: float = 0.0) -> Dict[str, Any]:
    """Insert a node before `index`."""
    _S["nodes"].insert(int(index), _node(x, z, y))
    return {"ok": True, "nodes": len(_S["nodes"])}


@mcp.tool()
def move_node(index: int, x: float, z: float, y: Optional[float] = None) -> Dict[str, Any]:
    """Move node `index` to (x, z[, y])."""
    try:
        n = _S["nodes"][int(index)]
    except IndexError:
        return {"ok": False, "error": "index out of range"}
    n["x"], n["z"] = float(x), float(z)
    if y is not None:
        n["y"] = float(y)
    return {"ok": True}


@mcp.tool()
def delete_node(index: int) -> Dict[str, Any]:
    """Delete node `index`."""
    try:
        _S["nodes"].pop(int(index))
    except IndexError:
        return {"ok": False, "error": "index out of range"}
    return {"ok": True, "nodes": len(_S["nodes"])}


@mcp.tool()
def set_node(index: int, lanes: Optional[int] = None, width: Optional[float] = None,
             surface: Optional[int] = None) -> Dict[str, Any]:
    """Set lanes / width / surface on node `index` (leave a field None to keep)."""
    try:
        n = _S["nodes"][int(index)]
    except IndexError:
        return {"ok": False, "error": "index out of range"}
    if lanes is not None:
        n["lanes"] = int(lanes)
    if width is not None:
        n["width"] = float(width)
    if surface is not None:
        n["surface"] = int(surface)
    return {"ok": True}


@mcp.tool()
def add_branch(nodes: List[List[float]], lanes: int = 3,
               surface: Optional[int] = None, width: Optional[float] = None) -> Dict[str, Any]:
    """Add a fork/branch: `nodes` is a list of [x, z] or [x, z, y] the branch
    passes through; it peels off the right side and rejoins where it ends."""
    bn = [{"x": float(p[0]), "z": float(p[1]), "y": float(p[2]) if len(p) > 2 else 0.0}
          for p in nodes]
    b: Dict[str, Any] = {"nodes": bn, "lanes": int(lanes)}
    if surface is not None:
        b["surface"] = int(surface)
    if width is not None:
        b["width"] = float(width)
    _S["branches"].append(b)
    return {"ok": True, "branches": len(_S["branches"])}


@mcp.tool()
def set_checkpoints(checkpoints: str = "auto:4") -> Dict[str, Any]:
    """Set checkpoints: "auto:N" for N evenly spaced, or a comma list of span
    indices like "40,120,240"."""
    if checkpoints.startswith("auto:"):
        _S["checkpoints"] = checkpoints
    else:
        try:
            _S["checkpoints"] = [int(x) for x in checkpoints.replace(" ", "").split(",") if x]
        except ValueError:
            return {"ok": False, "error": "use 'auto:N' or a comma list of span indices"}
    return {"ok": True, "checkpoints": _S["checkpoints"]}


@mcp.tool()
def set_environment(weather: Optional[int] = None, circuit: Optional[bool] = None,
                    traffic: Optional[int] = None, smoke: Optional[int] = None,
                    fog_enabled: Optional[int] = None, fog_r: Optional[int] = None,
                    fog_g: Optional[int] = None, fog_b: Optional[int] = None) -> Dict[str, Any]:
    """Set track environment: weather (0 rain,1 snow,2 clear), circuit flag,
    traffic enable, tire smoke, and fog (enabled + RGB)."""
    if weather is not None:
        _S["weather"] = int(weather)
    if circuit is not None:
        _S["circuit"] = bool(circuit)
    if traffic is not None:
        _S["traffic_enable"] = int(traffic)
    if smoke is not None:
        _S["smoke"] = int(smoke)
    if fog_enabled is not None:
        _S["fog"]["enabled"] = int(fog_enabled)
    for k, v in (("r", fog_r), ("g", fog_g), ("b", fog_b)):
        if v is not None:
            _S["fog"][k] = int(v)
    return {"ok": True, "weather": _S["weather"], "circuit": _S["circuit"],
            "fog": _S["fog"]}


# --- texture reuse --------------------------------------------------------
@mcp.tool()
def list_textures(level: int) -> Dict[str, Any]:
    """List a level's texture pages ([{id, type, palette_len, size}]) so you can
    pick pages to reuse. Pages are 64x64 palettized; type: 0 opaque, 1 colorkey,
    2 semi, 3 additive."""
    try:
        return {"ok": True, "level": int(level), "pages": tr.list_pages(_level_dir(level))}
    except Exception as e:
        return {"ok": False, "error": str(e)}


@mcp.tool()
def import_texture(source_level: int, source_page: int) -> Dict[str, Any]:
    """Plan a texture-page reuse: copy page `source_page` from `source_level`
    into this track's pool at build time. Returns the page id it WILL get (use
    that id in set_road_texture / place_* / set_span_texture). Imports are applied
    in call order after the asphalt page (id 0)."""
    new_id = _import_page(source_level, source_page)
    return {"ok": True, "page_id": new_id,
            "note": "referenced now; materialized on build()"}


@mcp.tool()
def set_road_texture(page: int) -> Dict[str, Any]:
    """Set the texture page the whole road samples (default 0 = generated
    asphalt). Use an id returned by import_texture to pave the road with a reused
    surface."""
    _S["road_texture"] = int(page)
    return {"ok": True, "road_texture": _S["road_texture"]}


@mcp.tool()
def set_ground_fill(page: int, half_width: float = 0.0, hills: float = 1.0,
                    valley: float = 1.0, flat_radius: float = 22000.0) -> Dict[str, Any]:
    """Fill the world with continuous textured GROUND (topology-agnostic height
    field) so there's no 'void floor' — works for any track shape. TERRAIN
    RELIEF knobs: `hills` scales rolling-hill amplitude (0 = flat plains,
    1 = default, >1 = taller/craggier), `valley` scales how fast the ground rises
    away from the road into valley walls / a bowl that also occludes the far
    horizon (0 = flat, 1 = default), `flat_radius` is the flat drivable zone
    around the road (world units). `half_width` = coverage reach (0 = auto to the
    view range). Use a page whose trace_textures role is 'ground'. page<0 disables."""
    if int(page) < 0:
        _S["ground_fill"] = None
        return {"ok": True, "ground_fill": None}
    _S["ground_fill"] = {
        "page": int(page), "half_width": float(half_width),
        "flat_radius": float(flat_radius),
        "slope": 0.14 * float(valley), "max_rise": 22000.0 * float(valley),
        "hill_amp": 14000.0 * float(hills),
    }
    return {"ok": True, "ground_fill": _S["ground_fill"]}


@mcp.tool()
def set_guardrails(page: int, height: float = 750.0, offset: float = 120.0,
                   left: bool = True, right: bool = True) -> Dict[str, Any]:
    """Enable OPTIONAL guardrails: continuous barrier walls along the road's rails,
    sitting just outside the drivable edge (where the engine's rail-wall collision
    already is) so the road edge reads as a solid barrier instead of an invisible
    wall. `page` textures the barrier (use a wall/barrier page via import_texture);
    `height` world units tall, `offset` pushes it outward from the edge, `left`/
    `right` toggle each side. OFF by default -- call this to turn them on; page<0
    disables. Emitted per-span so they render continuously as you drive."""
    if int(page) < 0:
        _S["guardrails"] = None
        return {"ok": True, "guardrails": None}
    _S["guardrails"] = {"page": int(page), "height": float(height),
                        "offset": float(offset), "left": bool(left), "right": bool(right)}
    return {"ok": True, "guardrails": _S["guardrails"]}


@mcp.tool()
def set_road_elevation(amplitude: float = 0.0, waves: float = 2.0,
                       max_grade: float = 0.12, intensity: float = 1.0) -> Dict[str, Any]:
    """Make the ROAD climb and descend: synthesize a smooth Y profile onto the
    centerline (for circuits it closes seamlessly). `amplitude` = peak rise/fall
    in world units (e.g. 8000-20000), `waves` = how many hills around the track,
    `max_grade` = drivable steepness cap (~0.12 = 12%; the profile is scaled down
    to stay under it), `intensity` 0..1 scales amplitude. amplitude<=0 disables."""
    if float(amplitude) <= 0.0:
        _S["road_elevation"] = None
        return {"ok": True, "road_elevation": None}
    _S["road_elevation"] = {"amplitude": float(amplitude), "waves": float(waves),
                            "max_grade": float(max_grade), "intensity": float(intensity)}
    return {"ok": True, "road_elevation": _S["road_elevation"]}


@mcp.tool()
def capture_terrain(source_level: int) -> Dict[str, Any]:
    """Measure a REAL track's terrain profile — how its ground rises/falls with
    distance from the road (flat_radius, slope, max_rise), how rough/bumpy it is
    (hill_amp), whether it dips below the road, and how much the road itself
    climbs (road_elev_range) — plus a per-distance rise table. Feed the returned
    flat_radius/slope/max_rise/hill_amp into set_ground_fill, or use mimic_terrain
    to apply them in one step, so a generated track's terrain behaves like this
    real one instead of a generic bowl."""
    try:
        return {"ok": True, **mtr.capture_terrain(None, int(source_level))}
    except Exception as e:
        return {"ok": False, "error": str(e)}


@mcp.tool()
def mimic_terrain(page: int, source_level: int,
                  min_flat_radius: float = 22000.0) -> Dict[str, Any]:
    """Configure the working track's ground fill to MIMIC a real track's terrain:
    captures `source_level`'s profile (capture_terrain) and applies its slope /
    max_rise / hill_amp / dips to this track's ground, textured with `page` (use a
    trace_textures role='ground' page). `min_flat_radius` keeps the drivable/
    frontage zone flat regardless of the source (so buildings aren't buried)."""
    prof = mtr.capture_terrain(None, int(source_level))
    if not prof.get("ok"):
        return {"ok": False, "error": prof.get("error", "capture failed")}
    _S["ground_fill"] = {
        "page": int(page), "half_width": 0.0,
        "flat_radius": max(float(min_flat_radius), float(prof["flat_radius"])),
        "slope": float(prof["slope"]), "max_rise": float(prof["max_rise"]),
        "hill_amp": float(prof["hill_amp"]),
    }
    # also mimic the source's ROAD climbs/descents (capped drivable)
    _S["road_elevation"] = {
        "amplitude": float(prof.get("road_elev_range", 0.0)),
        "waves": float(prof.get("road_waves", 2)),
        "max_grade": min(0.12, float(prof.get("road_max_grade", 0.12)) or 0.12),
        "intensity": 1.0,
    }
    return {"ok": True, "ground_fill": _S["ground_fill"],
            "road_elevation": _S["road_elevation"],
            "captured": {k: prof[k] for k in ("flat_radius", "slope", "max_rise",
                                              "hill_amp", "road_elev_range",
                                              "road_max_grade", "road_waves", "has_dips")}}


@mcp.tool()
def set_span_texture(span: int, page: int) -> Dict[str, Any]:
    """Override the texture page for a single road span (e.g. a patch of a reused
    surface). Span indices come from describe_track."""
    _S["span_textures"][str(int(span))] = int(page)
    return {"ok": True, "span_textures": _S["span_textures"]}


# --- scenery (static geometry folded into models.bin) ---------------------
@mcp.tool()
def place_box(x: float, y: float, z: float, w: float, h: float, d: float,
              page: int, floor_y: Optional[float] = None,
              roof_page: Optional[int] = None,
              base_page: Optional[int] = None) -> Dict[str, Any]:
    """Place an axis-aligned box (building block) centred at (x,y,z) with size
    (w,h,d). `page` textures the 4 WALLS; pass `roof_page` to texture the TOP
    separately (use a page whose trace_textures role is 'roof') and `base_page`
    for the bottom. If `floor_y` is given the box sits ON that ground Y. Returns
    the scenery index."""
    m = sc.make_box_mesh((x, y, z), (w, h, d), page, floor_y=floor_y,
                         roof_page=roof_page, base_page=base_page)
    _S["scenery"].append(m)
    return {"ok": True, "index": len(_S["scenery"]) - 1, "scenery": len(_S["scenery"])}


@mcp.tool()
def place_ground_quad(corners: List[List[float]], page: int) -> Dict[str, Any]:
    """Place a flat quad (plaza/apron/terrain fill). `corners` = 4 world points
    [x,y,z] in order near-left, near-right, far-right, far-left."""
    m = sc.make_ground_quad(corners, page)
    _S["scenery"].append(m)
    return {"ok": True, "index": len(_S["scenery"]) - 1}


@mcp.tool()
def add_billboard(x: float, y: float, z: float, w: float, h: float, page: int,
                  tag: int = 1) -> Dict[str, Any]:
    """Place a camera-facing billboard (tree/sign) anchored at (x,y,z) sized
    (w,h), textured with page `page`. `tag` is 1 or 2 (billboard class; use a
    colorkey/additive page for cutouts)."""
    try:
        m = sc.make_billboard_mesh((x, y, z), (w, h), page, tag=int(tag))
    except ValueError as e:
        return {"ok": False, "error": str(e)}
    _S["scenery"].append(m)
    return {"ok": True, "index": len(_S["scenery"]) - 1}


@mcp.tool()
def place_model(path: str, x: float, y: float, z: float, scale: float = 1.0,
                page: Optional[int] = None) -> Dict[str, Any]:
    """Import a real 3D model (glTF/.glb) and place it at (x,y,z), uniform-scaled
    by `scale`, forcing texture page `page` if given. TD5 units are large (~1500/
    lane) so a metre-scale model usually needs scale ~500-1500. Adds one scenery
    mesh per model sub-mesh; returns how many."""
    try:
        meshes = sc.load_model_meshes(path, translate=(x, y, z), scale=scale, page=page)
    except Exception as e:
        return {"ok": False, "error": str(e)}
    _S["scenery"].extend(meshes)
    return {"ok": True, "added": len(meshes), "scenery": len(_S["scenery"])}


@mcp.tool()
def extract_prototype(source_level: int, x: float, z: float,
                      radius: Optional[float] = None) -> Dict[str, Any]:
    """Lift a WHOLE real building/structure near (x,z) on `source_level` into the
    session's reusable-geometry library — actual meshes with their real shape and
    wall/window/roof texturing. Omit `radius` to auto-grab one contiguous building
    (recommended); pass `radius` for a fixed-circle grab. Use trace_geometry to
    find good object centers first. Returns a `proto_id` for place_prototype, plus
    its mesh_count / footprint / height / pages."""
    try:
        proto = al.extract_prototype(None, int(source_level), float(x), float(z),
                                     radius=radius)
    except Exception as e:
        return {"ok": False, "error": str(e)}
    pid = _S["proto_next"]; _S["proto_next"] += 1
    _S["prototypes"][pid] = proto
    return {"ok": True, "proto_id": pid, "source_level": int(source_level),
            "mesh_count": proto["mesh_count"], "footprint": proto["footprint"],
            "height": proto["height"], "pages": proto["pages"]}


@mcp.tool()
def list_prototypes() -> Dict[str, Any]:
    """List the reusable-geometry prototypes captured this session (id, source
    level, mesh_count, footprint, height, page count)."""
    out = []
    for pid, p in _S["prototypes"].items():
        out.append({"proto_id": pid, "source_level": p.get("source_level"),
                    "mesh_count": p["mesh_count"], "footprint": p["footprint"],
                    "height": p["height"], "pages": len(p["pages"])})
    return {"ok": True, "prototypes": out}


@mcp.tool()
def place_prototype(proto_id: int, x: float, y: float, z: float,
                    angle: float = 0.0, scale: float = 1.0) -> Dict[str, Any]:
    """Stamp a captured prototype (see extract_prototype) at world (x,y,z), yaw
    `angle` degrees, uniform `scale`. Its texture pages are imported (deduped
    across the session) and remapped automatically. Set y to the road/ground Y so
    the building sits flush. Adds the prototype's meshes to the scene."""
    p = _S["prototypes"].get(int(proto_id))
    if p is None:
        return {"ok": False, "error": "unknown proto_id %s (see list_prototypes)" % proto_id}
    page_map = {int(pg): _import_page(p["source_level"], pg) for pg in p["pages"]}
    meshes = al.instance_prototype(p, (float(x), float(y), float(z)),
                                   float(angle), page_map, float(scale))
    _S["scenery"].extend(meshes)
    return {"ok": True, "added": len(meshes), "scenery": len(_S["scenery"]),
            "pages_mapped": page_map}


@mcp.tool()
def list_scenery() -> Dict[str, Any]:
    """List placed scenery meshes: index, kind (billboard/opaque), page(s),
    world center."""
    out = []
    for i, m in enumerate(_S["scenery"]):
        pages = sorted({int(c["texture_page_id"]) for c in m["commands"]})
        out.append({"index": i,
                    "kind": "billboard" if m["texture_page_id"] in (1, 2) else "opaque",
                    "pages": pages, "verts": len(m["vertices"]),
                    "center": [round(m["bounding"][1], 1), round(m["bounding"][2], 1),
                               round(m["bounding"][3], 1)]})
    return {"ok": True, "scenery": out}


@mcp.tool()
def remove_scenery(index: int) -> Dict[str, Any]:
    """Remove a placed scenery mesh by index (see list_scenery)."""
    try:
        _S["scenery"].pop(int(index))
    except IndexError:
        return {"ok": False, "error": "index out of range"}
    return {"ok": True, "scenery": len(_S["scenery"])}


# --- breakable props: REMOVED per user feedback ---------------------------
# The movable box props had a janky collision volume (shoved on hit) and read as
# obstacles in the road, so define_prop_box / add_breakable are no longer exposed
# as editor tools. (The engine's TD6 prop path still exists for native tracks;
# the editor just doesn't author custom breakables.)


if __name__ == "__main__":
    mcp.run()
