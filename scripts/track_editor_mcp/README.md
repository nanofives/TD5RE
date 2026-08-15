# track_editor_mcp — AI-assisted track authoring MCP server

An MCP server that drives the **offline** TD5RE track-authoring engine so an AI
(or you, via the CLI) can build and **densify** custom tracks conversationally:
lay a centerline, **reuse existing game textures**, fold in **scenery**
(buildings, billboards, props), and register a drivable level — **no recompile**.

This is the authoring counterpart to `scripts/td5re_mcp/` (which drives a
*running* game). Build a track here, then race it there — or with
`td5re.exe --AutoRace=1 --DefaultTrack=<slot>`.

## How it works

```
Claude (MCP client, stdio)
  └── server.py            (FastMCP; holds ONE in-memory working track)
        ├── re/tools/td5_trackgen.py       road spec -> levelNNN/ + registry
        ├── re/tools/td5_scenery.py        scenery meshes + models.bin compose + props
        ├── re/tools/td5_texture_reuse.py  copy texture pages between levels
        └── re/tools/td5_describe.py       read-only level inventory
```

**Session model.** The server keeps one *working track*: a neutral centerline
spec (nodes + environment), a scenery list, a texture-import plan, and a
breakable-prop layer. You mutate it with small **primitive** verbs; **you** do
the spatial reasoning. `build()` flushes the whole session to
`re/assets/levels/levelNNN/` and registers it in `custom_tracks.json`.
`build()` is **deterministic** — it re-applies the full session every call, so
editing then rebuilding the same slot never loses imported pages or scenery.

## Requirements

- Python 3.10+ with `pip install mcp`, plus `numpy` + `pillow` (mesh/texture I/O).
- The engine writes into `re/assets/levels/` — the same editable-source the game
  packs on load, so built tracks are drivable immediately (DEV or RELEASE).

## Register (project-local `.mcp.json` in the repo root)

```json
{ "mcpServers": {
    "track_editor": { "command": "python",
      "args": ["scripts/track_editor_mcp/server.py"] } } }
```

Restart the session so the tools load.

## Tools

| Group | Tools |
|-------|-------|
| Session/track | `new_track` · `load_sample` · `import_track` · `get_spec` · `list_tracks` · `describe_track` · `build` · `reset_session` |
| Trace / complete | `trace_geometry` · `trace_textures` · `analyze_gaps` · `detect_voids` · `capture_terrain` |
| Ground / terrain | `set_ground_fill` · `mimic_terrain` · `set_road_elevation` |
| Road geometry | `add_node` · `add_nodes` · `insert_node` · `move_node` · `delete_node` · `set_node` · `add_branch` · `set_checkpoints` · `set_environment` |
| Texture reuse | `list_textures` · `import_texture` · `set_road_texture` · `set_span_texture` |
| Scenery (static) | `place_box` · `place_ground_quad` · `add_billboard` · `place_model` · `list_scenery` · `remove_scenery` |
| Geometry reuse | `extract_prototype` · `list_prototypes` · `place_prototype` |
| Breakable props | `define_prop_box` · `add_breakable` |

### Texture reuse — how ids work

`import_texture(source_level, source_page)` records a page copy and returns the
page id it **will** get in the built pool. The generated asphalt page is id `0`;
imports become `1, 2, …` in call order. Use the returned id in
`set_road_texture` / `set_span_texture` / `place_*(page=…)`. Pages are 64×64
palettized and self-contained (each carries its own palette), so any page from
any level can be reused. See `list_textures(level)` for the menu (type: 0 opaque,
1 colorkey/cutout, 2 semi, 3 additive — use 1/3 for billboard cutouts).

### Scenery — how it renders

Static scenery (boxes, ground quads, imported models) is folded into the level's
`models.bin` and attached to the road **entry nearest** its position, because the
engine walks display-list entries in a span-window around the player. Billboards
(`add_billboard`, header tag 1/2) are camera-facing; give them a colorkey/additive
page for tree/sign cutouts. Opaque meshes carry world-space geometry; the mesh's
sampled texture is the per-command page (an imported page id), never the header
tag — so reusing a page whose id happens to be 1 or 2 is safe.

### Trace & complete a track (context-aware gap-filling)

To fill a map's gaps in its own idiom, first SEE it:

1. `trace_textures(level)` → detects each page's SURFACE ROLE (road / ground /
   wall / roof / foliage / sky / sign) from how the geometry USES it (face
   orientation + height + proximity to the road, colour as a cue), and renders a
   role-labeled montage PNG. **Read the montage** to see the art and pick pages
   by role — e.g. a `wall` page for building sides, a `roof` page for tops, a
   `road` page for the street. Each page also returns `usage` fractions
   (low/high/vert/bb/on_road) so you know how confident the role is.
2. `trace_geometry(level [,span_start,span_end])` → clustered scenery objects
   (footprint/height/pages/side/setback) + a per-span-bucket profile (the local
   style). Native cities have thousands of meshes — focus with a span range.
3. `analyze_gaps(level)` → empty roadside stretches (SPAN space: roadside object
   density) + a style profile (typical building height/footprint/setback + pages).
   `detect_voids(level)` → COVERAGE space: measures actual ground/floor coverage
   out to far_cull, inward (loop interior) vs outward (outfield), and reports
   `void_begins_at` (distance where coverage drops below 95%). analyze_gaps is
   blind to floor voids; detect_voids finds them — use it to know how far / which
   way to extend ground (esp. outward) or where to ring the horizon with occluders.
4. Fill: `import_texture` the profile's pages, then `place_box`/`place_model`/
   `add_billboard` matching the nearby height/footprint/setback into the gap
   spans, and `build`.

**Reuse REAL buildings from another track (best fullness):** instead of boxes,
lift actual buildings and stamp them:
1. `trace_geometry(source_level)` → pick building object centers.
2. `extract_prototype(source_level, x, z)` → captures the whole contiguous
   building (real meshes + wall/window/roof texturing). Returns a `proto_id`.
3. `place_prototype(proto_id, x, y, z, angle)` at gap spans (y = road ground so
   it sits flush; angle = road tangent to face the street). Texture pages are
   imported + deduped automatically.
4. `build`.

## Example (build + densify a map)

```
new_track name="RIVER SPRINT" circuit=false
add_nodes points=[[0,0],[0,8000],[1500,16000],[4000,24000], ...]
import_texture source_level=2 source_page=44   -> {page_id: 1}   # a road surface
set_road_texture page=1
import_texture source_level=8 source_page=210  -> {page_id: 2}   # a building wall
place_box x=6000 y=0 z=12000 w=3000 h=6000 d=3000 page=2 floor_y=0
add_billboard x=-5000 y=0 z=9000 w=2000 h=4000 page=... tag=1     # a tree cutout
build          -> {slot: 37, level: 40, drive_cmd: "...--DefaultTrack=37"}
```

Densify an existing map: `describe_track level=5` (extents, spans, existing
props, textures) → `import_track level=5` to edit its line, or just add scenery
around its span centers and `build` to a scratch slot.

## Drive it directly (no MCP)

Every tool is a thin wrapper over the engine in `re/tools`; you can script it:

```python
import sys; sys.path.insert(0, "re/tools")
import td5_trackgen as tg, td5_scenery as sc, td5_texture_reuse as tr
spec = tg.sample_spec("oval")
# ... add spec["scenery"], spec["texture_imports"], spec["road_texture"] ...
print(tg.build_track(spec))
```

## Notes

- Netplay parity: custom tracks must be identical across peers (same constraint
  as custom cars) — single-player / hotseat unless all peers share the set.
- Breakable props reuse the TD6 prop path: ≤8 distinct prop meshes; collision
  radius comes from the engine's fixed table per model slot.
- Authoring a brand-new texture page from arbitrary imagery (vs reusing an
  existing page) needs a palette builder — a follow-on; reuse sidesteps it.
```
