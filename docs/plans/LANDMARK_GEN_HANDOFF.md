# Landmark hole-fill → generative geometry (account3 handoff)

## Why this exists

Account2 (Accenture) is web- and model-restricted, so the fill work there is
deterministic: `re/tools/td5_geomlib.py` closes landmark holes by
`fill_landmark_holes` (a tiled grass plane over the footprint) and
`fill_landmark_walls` (bridging FREE EDGES — edges used by one face — with quads
whose corners are existing vertices and whose UVs are read from the neighbour
face's affine world→uv map). Toggle it in the studio: LIBRARY → Browse catalogue
→ **Fill holes**, on a SET PIECE (e.g. L23.lm00, St Basil's).

That approach is faithful and in-format, but it can only **connect what is
already there**. It cannot invent missing structure — a whole absent tower, the
mirror half of a symmetric façade, an ornate gable. The user wants that
invented. This account can't run a generative model, so the generative pass runs
on **account3** (personal, unrestricted, web access).

## The data (regenerable on any account, no dump needed)

- Segmented set pieces: `td5_track_studio._landmarks(level)` → list of objects,
  each `{prims, pages, aabb, extent, nface, ...}`. Each prim is
  `{mesh:{vertices:[{pos,tex,light}], commands:[{texture_page_id,tri,quad}]}, role, pages}`.
- Per-face dump helper pattern is in this session's scratchpad
  (`lm00/faces.json` + copied `page_NNN.png`); regenerate for any landmark.
- Texture pages: `re/assets/levels/level023/textures.src/pages/page_NNN.png`
  (64×64 indexed, **V=0 is the TOP of the image** — this bit the deterministic
  fill as an inverted-texture bug; any generated UVs must respect it).
- Format invariants (whole corpus, measured): vertices de-indexed, consumed by a
  running cursor **tris before quads**; `texture_page_id` on the command IS the
  material; no per-face data; buildings are open single-sided quad shells (no
  backs, no caps).

## Verification (same on either account)

Studio at `re/tools/track_studio/` (`python td5_track_studio.py --port 8766`),
`/api/library/prefab?id=L23.lm00&fill=1` builds the filled GLB. Render with
Playwright + headless Chrome (swiftshader) as this session did. The generated
geometry must round-trip through `mesh_tool.build_dat()` and land in the local
prefab frame (`_prefab_from_landmark`: centred in XZ, base y=0).

## Candidate generative approaches (pick one — they are very different)

1. **Symmetry completion (no external model).** Most landmarks are symmetric —
   St Basil's is radial, the Kremlin wall repeats. Detect the symmetry
   plane/axis from the existing faces, reflect/rotate the present faces to fill
   the absent ones. Generates real new geometry, guaranteed in-format and
   texture-matched because it copies existing faces. Runs anywhere; account3 not
   strictly required, but its web access helps confirm the true symmetry from
   reference imagery. **Recommended default: faithful, cheap, low risk.**

2. **Claude authoring with web reference (account3).** An unrestricted session
   looks up reference images of the real building, reasons about the missing
   structure, and authors TD5-format quads by hand (as this session did for the
   edge bridges, but able to *invent*, not just connect). In-format, faithful,
   slow, quality bounded by reasoning.

3. **External image/text-to-3D API (account3 + credentials).** Meshy / Tripo /
   Rodin / Hunyuan3D from a render or prompt → dense textured mesh → **retopo +
   re-page into TD5's 64×64 paged quad shells**. The retopo/retexture step is
   large and lossy and the output will not match the shipped art unless heavily
   constrained. Needs API keys and budget. Highest effort, most "generative",
   least faithful to the original look.

## CHOSEN APPROACH: Claude authoring with web reference (built)

The contract account3 works against is in place:

1. **Inspect** a landmark:
   `python re/tools/dump_landmark.py --level 23 --idx 0 --out DIR`
   → `DIR/faces.json` (every face: role, page, world verts, uv, light, plus
   `free_edges` = the hole boundaries) and `DIR/page_NNN.png` for every page.
2. **Reference**: WebSearch / WebFetch the real building (this is why it runs on
   account3 — account2 cannot). Reason about the missing structure.
3. **Author** new faces into `re/assets/library/authored_fills.json` under
   `fills["L23.lm00"].faces` — world coords, 3 or 4 verts each, `V=0 is the top`
   of the page. A worked one-quad example sits under `_example` (inert).
4. **Preview**: the studio always loads authored fills
   (`td5_geomlib.load_authored_fills`, wired into `build_landmark_glb`), so open
   LIBRARY → the set piece and it shows — no toggle needed. The deterministic
   grass/wall fill is still behind the **Fill holes** checkbox, additive.
5. **Verify**: it must round-trip through `mesh_tool.build_dat()`; render with
   Playwright as this session did.

Authored fills are curated truth kept OUT of code, so re-running the segmenter
never discards them — the same rule as `tags.json` / `selections.json`.

## Handoff mechanics

1. This session's context + memory sync to account3 via `/sync 2 3`.
2. Open a Claude Code session in this folder with
   `CLAUDE_CONFIG_DIR=C:\Users\maria\.claude-account3` and continue there.
