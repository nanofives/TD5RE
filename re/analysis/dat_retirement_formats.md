# DAT Retirement — editable asset formats (pack-on-load)

Every binary `.DAT` asset in `re/assets/` has been retired in favour of an editable
source file. At load time the runtime reads the source and encodes it to the exact
binary `.DAT` layout **in memory**, then feeds the existing verified parsers — so the
on-disk binaries are gone but nothing in the parse/render path changed.

The runtime tree (`re/assets/`) now contains **zero** loose `.dat` / `.num` / `.bus` /
`.trk` binaries — every one is either a pack-on-load editable source (below) or was a
dead orphan that has been moved to `re/_retired_dats/`.

- **Interception:** `td5_assetsrc_pack()` (`td5_assetsrc.c`) runs as "step 0" of
  `td5_asset_open_and_read()` (`td5_asset.c`). If an editable source exists where the
  `.DAT` would live, it is packed and returned; otherwise the loader falls through to
  the legacy loose-file / extracted-folder / ZIP paths.
- **JSON parsing in C:** vendored cJSON (`td5mod/src/td5re/deps/cjson/`). Integers are
  read via rounded `cJSON_GetNumberValue` (never `valueint`, which clamps to INT_MAX).
- **Verify:**
  - `td5re.exe --selftest-assetsrc` — packs each source via the real runtime encoder
    and byte-compares vs the `.DAT` (writes `assetsrc_selftest.log`).
  - `make verify-assets` (from `td5mod/`) — Python re-pack vs the retired originals in
    `re/_retired_dats/`, plus the glTF mesh round-trip check.
- **Retired binaries:** moved to `re/_retired_dats/` (gitignored). `original/` ZIPs are
  the pristine upstream backup. Editable sources (`re/assets/**`) are also gitignored
  and regenerated locally by the tools below.

## Formats

| `.DAT` | Editable source | Tool | Notes |
|---|---|---|---|
| LEVELINF.DAT | `levelinf.json` | `levelinf_editor.py` | 100-byte struct, every field named |
| CARPARAM.DAT | `carparam.json` | `carparam_editor.py` | 268-byte tuning+physics struct |
| CHECKPT.NUM | `checkpt.json` | `dat_tables.py` | 4×6 int32 matrix |
| TRAFFIC.BUS (+B) | `traffic.json` / `trafficb.json` | `dat_tables.py` | `[span,flags,lane]` records incl. sentinel |
| LEFT/RIGHT.TRK (+B) | `*.trk.csv` | `dat_tables.py` | one `b0,b1,b2` row per span |
| STRIP/STRIPB.DAT | `strip.json` / `stripb.json` | `strip_tool.py` | layout-faithful: header[5] + spans + vertices (+hex gaps) |
| TEXTURES.DAT (+tpages) | `textures.src/` | `texture_tool.py` | `textures.json` (manifest) + `indices.bin` (authoritative) + `pages/*.png` (editable preview) |
| MODELS.DAT | `models.bin` | `mesh_tool.py` | byte-exact passthrough; edit via glTF below |
| HIMODEL.DAT | `himodel.bin` | `mesh_tool.py` | byte-exact passthrough; edit via glTF below |
| trak_markers(.dat / _td6.dat) | `trak_markers.json` / `trak_markers_td6.json` | `track_preview_render.py` | port-generated track-preview start/finish dots; read directly by the frontend (not via the chokepoint) |

All TD5-game formats round-trip **byte-exact** (self-test 463/463). The
`trak_markers` files are port-generated overlays, not original game data, so they
have no byte-exact contract — the JSON preserves the floats to 6 decimals
(sub-pixel-identical dot placement; migration verified lossless within 1e-5).

The `trak_markers` files do **not** flow through `td5_asset_open_and_read` — the
frontend reads them with its own `fopen`+cJSON loaders (`frontend_load_track_markers`
/ `_td6` in `td5_frontend.c`). They are placed by an explicit `pool` (TD5, 0..19)
or `tga` (TD6, >=90) field, so entries are reorder-safe and hand-editable. The
generator now emits JSON directly; `track_preview_render.py dat2json` did the
one-time migration from the legacy `TMK1` binary (kept in `re/_retired_dats/tracks/`).

## Editing workflows

- **Tables / geometry:** edit the `.json` / `.csv` directly and relaunch.
- **Textures:** edit `textures.src/pages/page_NNN.png`, then
  `python re/tools/texture_tool.py pack re/assets/levels/levelNNN/textures.src`
  (re-quantizes the edited PNG into `indices.bin` against the page palette).
- **Meshes (Blender):**
  - export: `python re/tools/mesh_tool.py export <models.bin|himodel.bin> out.glb`
  - edit `out.glb` in Blender
  - import: `python re/tools/mesh_tool.py import out.glb <models.bin|himodel.bin>`

  Geometry rides in standard glTF accessors (POSITION / TEXCOORD_0 / TEXCOORD_1 /
  NORMAL); PRR-specific data (per-command `dispatch_type`/`texture_page_id`/tri+quad
  counts, per-vertex `light`, per-face `visible_flag`, mesh header, and the MODELS
  container entry→mesh map) rides in glTF `extras`. The runtime never parses glTF —
  it loads the byte-exact `.bin`; the import step regenerates that `.bin` from the
  edited `.glb`.

## TD6 cars (0x104 indexed himodel)

The 39 TD6 cars ship a `render_type 0x104` *indexed* himodel (transcoded at load by
`td5_asset_transcode_td6_mesh`). `mesh_tool.py` now decodes it — it expands the indexed
mesh exactly like the runtime transcode and presents it as a normal `0x103` mesh, so
TD6 cars **export to glTF and round-trip** (verified: `selfcheck-glb-all re/_retired_dats`
= 107/107). Editing flow is the same as TD5 cars.

Caveat: re-import writes a `0x103` expanded himodel. The runtime loads that directly
(no transcode), but it no longer carries the `0x104` marker, so an *edited* TD6 car
falls back to the cardef brake-light hardpoint instead of the authored `:CAR_LIGHTS:`
positions (minor; the reflection overlay is globally off, so no chrome misrender).
Unedited TD6 cars keep their byte-exact `0x104` `himodel.bin` (passthrough).

## Retired orphans (no editable source — they were dead files)

- **`tpage{4,5,12}.dat`** — these were **never loaded** in the shipping config, so they
  were simply moved to `re/_retired_dats/static/` (no PNG bake, no risk). They are
  GRXB runtime captures from `dump_tpages.py`, named by *slot* (4/5/12); but:
  - The static-atlas loader (`load_static_*_tpage`) addresses files by a *compacted*
    `file_idx` (`static_tpage_file_index`). `static.hed` has exactly 3 `image_type==1`
    pages — slots 4, 5, 12 → `file_idx` 0, 1, 2 — so the loader only ever opens
    `tpage0/1/2`, served by the live **`tpage0/1/2.png`** (baked from the pristine
    `static.zip` ARGB pages). Max `file_idx` requested = 2; `tpage3.dat`+ are unreachable.
  - The HUD's slot-named fallback (`td5_hud.c` reads `tpage{slot}.dat` for the font /
    speedo) is gated behind `!td5_asset_static_tpage_is_real(slot)`. Since the static
    loader set `done[4]=done[5]=done[12]=1` from the PNGs, that gate is always false, so
    the fallback never runs. `tpage4.dat`/`tpage12.dat` have **no reader at all**;
    `tpage5.dat`'s only reader is permanently gated off.
  - Verified empirically: an auto-race after removal renders the speedo/HUD/pause from
    the static-atlas PNG pages (`[INF][hud] atlas SPEEDO: found` + `speedo: rpm=...`,
    no GDI-synth/fallback lines, no errors). The pristine sources remain in
    `original/static.zip` and `re/_retired_dats/static/`.
  - The now-dead `tpage{slot}.dat` fallback branch in `td5_hud.c` could be deleted as
    cosmetic cleanup, but it is harmless.

## Known gaps (non-blocking)

- The `trak_markers.dat` / `trak_markers_td6.dat` track-preview dots were retired to
  editable JSON (see the Formats table above); their loaders read JSON via cJSON.
- The frontend track-selector / reverse-toggle existence checks were made source-aware
  (`frontend_track_level_exists`, `td5_asset_track_has_reverse`). Any *future* code that
  probes a retired `.DAT` directly on disk must do the same.
