# DAT Retirement — editable asset formats (pack-on-load)

Every binary `.DAT` asset in `re/assets/` has been retired in favour of an editable
source file. At load time the runtime reads the source and encodes it to the exact
binary `.DAT` layout **in memory**, then feeds the existing verified parsers — so the
on-disk binaries are gone but nothing in the parse/render path changed.

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

All formats round-trip **byte-exact** (self-test 463/463).

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

## Known gaps (non-blocking)

- **5 peripheral `.dat` kept in `re/assets`:** `tpage{4,5,12}.dat` (static atlases
  with no PNG sibling — the special "corrupted GRXB" dumps; retire by generating PNGs)
  and `trak_markers.dat` / `trak_markers_td6.dat` (port-generated frontend track-preview
  dots, loaded via direct `fopen`; retire via an editable form + a source-aware loader).
- The frontend track-selector / reverse-toggle existence checks were made source-aware
  (`frontend_track_level_exists`, `td5_asset_track_has_reverse`). Any *future* code that
  probes a retired `.DAT` directly on disk must do the same.
