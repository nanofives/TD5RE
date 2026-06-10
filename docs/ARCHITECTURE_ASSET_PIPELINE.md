# TD5RE Asset Pipeline Architecture

How td5re.exe finds, decodes, and uploads game assets, and what a modder edits to change them. Everything below is verified against the source as of 2026-06: the single chokepoint is `td5_asset.c`, the editable-source ("pack-on-load") layer is `td5_assetsrc.c`, and the runtime asset tree is `re/assets/` resolved relative to the current working directory. There is no TGA decoding left (`td5_asset.c:1427` — "TGA decoder removed — all assets now loaded as PNG"); image decode is stb_image PNG only (`STBI_ONLY_PNG`, `td5_asset.c:42`), and binary `Config.td5` is retired in favour of INI files (`td5_save.c:46`, one-time legacy import).

## Runtime asset directory and path resolution

All asset paths are **CWD-relative** — launch from the repo root (or unzip root for the portable release). The repo root contains no game ZIPs; originals live in `original/` as backup only, so in practice every asset is served from `re/assets/`.

Loaders address assets by `(entry_name, zip_path)` pairs using the *original* archive paths (`"level%03d.zip"`, `"cars/vip.zip"`, `"traffic.zip"`, `"Front End/frontend.zip"`). Two resolvers map those onto `re/assets/` subfolders:

- `build_extracted_level_path` (`td5_asset.c:147`): `levelNNN.zip` → `re/assets/levels/level%03d/<entry>` (also probes `level_num+1` and a secondary root `re/td5_dump/levels`).
- `build_extracted_asset_path` (`td5_asset.c:191`): `cars/<code>.zip` → `re/assets/cars/<code>/`, `static.zip` → `static/`, `traffic.zip` → `traffic/`, `environs.zip` → `environs/`, `loading.zip`/`legals.zip`/`cup.zip` likewise, `sound*` → `sound/`, `Front End/*` → `frontend|extras|mugshots|sounds|tracks`.

`td5_asset_resolve_png_path` (`td5_asset.c:2052`) applies the same archive map, strips the entry's extension, and builds `re/assets/<sub>/<stem>.png` — this is how legacy `.tga` entry names resolve to the pre-extracted PNGs.

The track index → level number mapping is `td5_asset_level_number` (`td5_asset.c:1612`): the 19 native tracks go through the original schedule→pool→zip tables; menu slots 26–36 are migrated TD6 tracks resolved via `k_td6_menu_slots` (`td5_asset.c:1473`, also sets `g_active_td6_level`); drag race hardcodes level 30.

## Load flow

`td5_asset_open_and_read(entry, zip, &size)` (`td5_asset.c:1318`) is the chokepoint. Step order:

0. **Pack-on-load** — `td5_assetsrc_pack` (`td5_assetsrc.c:698`): if an editable source (JSON/CSV/`.bin`) is registered for the bare entry name and exists where the `.DAT` would live (located via `td5_asset_resolve_source_path`, `td5_asset.c:281`), it is encoded to the exact original binary layout **in RAM** and returned. The registry (`td5_assetsrc.c:670`): `LEVELINF.DAT`←`levelinf.json` (100 B struct), `CARPARAM.DAT`←`carparam.json` (268 B), `CHECKPT.NUM`←`checkpt.json` (4×6 int32), `TRAFFIC(B).BUS`←`traffic(b).json`, `LEFT/RIGHT(B).TRK`←`*.trk.csv`, `STRIP(B).DAT`←`strip(b).json`, `TEXTURES.DAT`←`textures.src/textures.json` + `indices.bin`, `MODELS.DAT`/`HIMODEL.DAT`←`models.bin`/`himodel.bin` (byte-exact passthrough). JSON parsing is vendored cJSON (`td5mod/src/td5re/deps/cjson/`); integers are read via rounded `cJSON_GetNumberValue`, never `valueint` (`td5_assetsrc.c:14-17`). The size query `td5_asset_get_entry_size_from_path` (`td5_asset.c:1271`) has the same step 0.
1. **Loose file** with the literal entry name (`try_loose_file_*`, `td5_asset.c:1145`).
2. **Extracted level folder** (`try_extracted_level_file_*`, `td5_asset.c:1170`).
3. **Extracted asset folder** (`try_extracted_asset_file_*`, `td5_asset.c:258`).
4. **ZIP** — own central-directory parser (`td5_asset_open_archive`, `td5_asset.c:821`); DEFLATE via `td5_inflate_mem_to_mem` (`td5_inflate.c`), built with `-DTD5_INFLATE_USE_ZLIB` (raw inflate, `build_standalone.bat`).

**Level load** — `td5_asset_load_level` (`td5_asset.c:2279`) loads, per direction (forward vs reverse filenames picked from `s_strip_fwd/rev` tables, `td5_asset.c:1453`, never as fallbacks): `STRIP(B).DAT` → `td5_track_load_strip`; `LEFT/RIGHT(B).TRK` → `td5_track_load_routes`; `MODELS.DAT` → `td5_track_parse_models_dat` (`td5_track.c:5054`); `LEVELINF.DAT` (DWORD[0] = circuit flag → `g_td5.track_type`; buffer retained as `g_track_environment_config`); `CHECKPT.NUM` (96 B); `TRAFFIC(B).BUS` → `td5_ai_set_traffic_queue`. Reverse requires `STRIPB.DAT` + `LEFTB/RIGHTB.TRK` (or their editable sources) — `td5_asset_track_has_reverse` (`td5_asset.c:2196`).

**Track textures** — `td5_asset_load_track_textures` (`td5_asset.c:2642`) is metadata-only: it registers each page's transparency type (byte +3 of the page descriptor) via `td5_asset_set_page_transparency`; no GPU upload. `td5_asset_load_race_texture_pages` (`td5_asset.c:2713`) does the upload: TEXTURES.DAT layout `[u32 page_count][u32 offsets[]]` then per page `pad[3] | type | i32 pal_count | BGR palette | 4096 indices`, decoded to 64×64 **BGRA32** (type 0 opaque, 1 colour-keyed index 0, 2 uniform alpha 0x80, 3 additive), edge-dilated (`alpha_bleed_rgb`) and uploaded to GPU page == DAT page index. Reverse races permute page sources via CHECKPT.NUM pairs (`td5_asset_apply_reverse_texture_swap`, `td5_asset.c:2576`). Migrated TD6 tracks override individual pages with native-resolution loose PNGs `re/assets/levels/levelNNN/textures/tex_NNN.png` (`td5_asset.c:2780`).

**Vehicles** — `td5_asset_load_vehicle` (`td5_asset.c:3380`): `himodel.dat` is loaded through the chokepoint; a TD6 `render_type 0x104` indexed mesh is transcoded to the TD5 expanded format (`td5_asset_transcode_td6_mesh`, `td5_asset.c:3157`); `td5_track_prepare_mesh_resource` (`td5_track.c:5448`) relocates internal offsets to pointers; the mesh registers with `td5_render_set_vehicle_mesh`. `carskinN`/`carhubN` PNGs upload to per-slot pages, and every primitive command's `texture_page_id` is patched **by index** (hub ID 8 → the static-atlas CHASSIS sub-region with UV remap; everything else → the skin page) (`td5_asset.c:3561-3598`). `carparam.dat` (268 B) feeds `td5_physics_load_carparam`. Traffic uses `model%d.prr` + `skin%d.png` from `traffic.zip` (`td5_asset_load_traffic_model`, `td5_asset.c:3646`).

**Static atlas** — at `td5_asset_init`, `re/assets/static/static.hed` is parsed (`td5_asset_init_static_atlas`, `td5_asset.c:568`): 64-byte named sprite entries (name/x/y/w/h/slot) become `TD5_AtlasEntry` records looked up by `td5_asset_find_atlas_entry` (case-insensitive, never NULL); pages with `image_type==1` load `tpageN.png` (fallback `tpageN.dat` dump) onto GPU pages 700+slot. PNG decode everywhere is `td5_asset_decode_png_rgba32` (`td5_asset.c:1815`) — output is **BGRA** (R↔B swapped for D3D11 `B8G8R8A8_UNORM`) despite the legacy name (`td5_asset.h:361`), with optional colour-key modes (`TD5_ColorKeyMode`, `td5_asset.h:375`).

## Texture page numbering

GPU texture pages are a single integer namespace (wrapper limit 1024, `td5_asset.c:1462`), carved up by convention:

- **0..~600** — per-level track pages; GPU page == TEXTURES.DAT page index == mesh `tex_page` (`td5_asset.c:2713`).
- **700–731** — static HUD/UI atlas, `STATIC_ATLAS_BASE` + static.hed slot (`td5_asset.c:334`).
- **800–811** — race car textures: `800 + slot*2` skin, `+1` hub, 6 slots (`TD5_CAR_TEXTURE_PAGE_BASE`, `td5_asset.c:3484`).
- **820–825** — traffic skins (`TD5_TRAFFIC_TEXTURE_PAGE_BASE`, `td5_asset.c:3642`).
- **888–983** — frontend `SHARED_PAGE_*` range (`td5_frontend_internal.h:27-62,114`): 888–892 background gallery, 893 small font, 894–898 sprite/font sheets, 899 white, **900–931 dynamic frontend surfaces** (`FE_SURFACE_PAGE_BASE + slot`), 970–971 MSDF fonts, 972–980 title pages (`td5_frontend.c:787`), 981–983 cursor/HUD/pause SDF fonts.
- **990–993** — environment/reflection maps (`ENVMAP_TEXTURE_PAGE_BASE`, `td5_render.c:439`; loaded by `td5_asset_load_environs_pages` from `re/assets/environs/` per the table in `td5_environs_table.inc`).

## Modding: what to edit

Pack-on-load runs on **every** load with no caching, so for the formats below you edit the source and relaunch — nothing to rebuild or repack except where noted.

- **Track/car data tables**: edit `levelinf.json`, `carparam.json`, `checkpt.json`, `traffic.json`, `*.trk.csv`, `strip.json` in `re/assets/levels/levelNNN/` or `re/assets/cars/<code>/`. Editor helpers: `re/tools/levelinf_editor.py`, `carparam_editor.py`, `strip_tool.py`, `dat_tables.py`.
- **Track textures**: the runtime reads `textures.src/textures.json` + `indices.bin` only (`td5_src_encode_textures`, `td5_assetsrc.c:548`). Edit `textures.src/pages/page_NNN.png`, then `python re/tools/texture_tool.py pack <src_dir>` to re-quantize into the authoritative `indices.bin`. TD6 tracks: edit `textures/tex_NNN.png` directly (read at runtime, no repack).
- **UI / car-skin / frontend / loading-screen textures**: edit the PNG under the mapped `re/assets/<sub>/` folder directly (e.g. `re/assets/cars/vip/carskin0.png`, `re/assets/frontend/*.png`); decoded fresh on each load.
- **Meshes**: the runtime loads `models.bin` / `himodel.bin` byte-exact (passthrough encoder, `td5_assetsrc.c:644`). Blender flow is offline: `python re/tools/mesh_tool.py export <bin> out.glb`, edit, `mesh_tool.py import out.glb <bin>`.
- **Any ZIP entry**: a loose file with the exact entry name (steps 1–3 above) overrides archive content — the original game's own override rule (`td5_asset.h:53-56`).
- **Verify**: `td5re.exe --selftest-assetsrc` (`main.c:611`) packs every registered source found under `re/assets/levels/level000..063` and `re/assets/cars/*` and byte-compares against any `.DAT` present, writing `assetsrc_selftest.log` (`td5_assetsrc_selftest`, `td5_assetsrc.c:803`).

Background/format docs: `re/analysis/dat_retirement_formats.md` (source formats, tools, retired orphans), `re/analysis/formats/archive-and-asset-loading.md` (original-binary RE notes).

## Key entry points

| Function | File | Role |
|---|---|---|
| `td5_asset_open_and_read` | `td5mod/src/td5re/td5_asset.c:1318` | Chokepoint: pack-on-load → loose → extracted → ZIP |
| `td5_assetsrc_pack` | `td5mod/src/td5re/td5_assetsrc.c:698` | Encode editable source → original .DAT layout in RAM |
| `td5_asset_resolve_source_path` | `td5mod/src/td5re/td5_asset.c:281` | Locate editable source for a (name, zip) pair |
| `td5_asset_load_level` | `td5mod/src/td5re/td5_asset.c:2279` | STRIP/TRK/MODELS/LEVELINF/CHECKPT/TRAFFIC for a track |
| `td5_asset_load_race_texture_pages` | `td5mod/src/td5re/td5_asset.c:2713` | Decode TEXTURES.DAT pages → GPU upload |
| `td5_asset_load_vehicle` | `td5mod/src/td5re/td5_asset.c:3380` | himodel + carparam + skin/hub pages per race slot |
| `td5_asset_init_static_atlas` | `td5mod/src/td5re/td5_asset.c:568` | static.hed sprite atlas → pages 700+ |
| `td5_asset_decode_png_rgba32` | `td5mod/src/td5re/td5_asset.c:1815` | All image decode (PNG → BGRA32) |
| `td5_asset_resolve_png_path` | `td5mod/src/td5re/td5_asset.c:2052` | Legacy entry name → `re/assets/<sub>/<stem>.png` |
| `td5_track_prepare_mesh_resource` | `td5mod/src/td5re/td5_track.c:5448` | Relocate mesh offsets → pointers before render registration |
| `td5_assetsrc_selftest` | `td5mod/src/td5re/td5_assetsrc.c:803` | Byte-exact source→pack→.DAT comparison sweep |
| `td5_inflate_mem_to_mem` | `td5mod/src/td5re/td5_inflate.c` | Raw DEFLATE (zlib via `TD5_INFLATE_USE_ZLIB`) |
