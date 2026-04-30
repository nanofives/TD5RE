# re/tools/

Helper scripts for reverse-engineering and asset work on Test Drive 5.

Group purposes inferred from each tool's docstring; run `<tool>.py --help`
(where applicable) for full usage. None of these are required to run the
source port at runtime — they are RE-side utilities.

## Asset extraction & conversion

| Tool | Purpose |
|---|---|
| `td5_asset_extractor.py` | Extract all TD5 ZIP archives into organized directory tree |
| `td5_auto_dump.py` | Cycle through tracks/cars to dump every texture variant |
| `td5_offline_dump.py` | Generate R5G6B5 texture dumps offline from extracted assets |
| `td5_dump_to_png.py` | Convert dumped game assets to PNG |
| `td5_organize_dump.py` | Reorganize flat texture dump into categorized folders |
| `td5_upgrade_dump.py` | Upgrade R5G6B5-quantized dump PNGs to full 8-bit color |
| `td5_build_index.py` | Build `index.dat` from dumped/edited texture PNGs |
| `td5_texture_catalog.py` | Extract and classify all textures from the original game |
| `td5_texture_converter.py` | Convert TD5 textures (`TEXTURES.DAT`, `tpage*.dat`, TGA) to PNG |
| `extract_tgas.py` | Rebuild `re/assets/**/*.tga` from `original/*.zip` |
| `td5_add_alpha.py` | Automatically add alpha channels to textures that need transparency |
| `bake_transparency.py` | Pre-process: bake every runtime colorkey into PNG alpha |
| `dump_tpages.py` | Dump runtime-assembled texture pages from running `TD5_d3d.exe` |

## Save / config editors

| Tool | Purpose |
|---|---|
| `save_editor.py` | TD5 save file editor |
| `gen_unlocked_save.py` | Generate unlocked `Config.td5` |
| `carparam_editor.py` | CLI for reading, editing, converting `carparam.dat` |
| `levelinf_editor.py` | CLI for reading, editing, converting `level.inf` |

## RE / static analysis

| Tool | Purpose |
|---|---|
| `analyze_carparam.py` | Analyze `carparam.dat` across cars to identify padding vs data |
| `extract_environs_table.py` | Rip per-track environs descriptor table |
| `extract_light_zones_table.py` | Rip per-track light-zone records |
| `strip_viewer.py` | `STRIP.DAT` track geometry parser + visualizer |

## Live process / runtime

| Tool | Purpose |
|---|---|
| `frida_windowed.py` | Force `TD5_d3d.exe` into windowed mode via Frida |
| `frida_trace_fullscreen.py` | Frida trace of fullscreen DDraw calls |
| `trace_window_init.py` | Trace every call that affects window initialization |
| `apply_cwd_patch.py` | `SetCurrentDirectory("data")` trampoline for `TD5_d3d.exe` |
| `fix_scale_cave_v6.py` | Scale cave v6: `Blt back→front` + `COLORFILL` + `WaitForVerticalBlank` |

## Tooling bridges

| Tool | Purpose |
|---|---|
| `bridge_stdio_x64dbg_mcp.py` | MCP stdio-to-HTTP bridge for the x64dbg MCP plugin |
| `ghidra_call.py` / `ghidra_call.sh` | Wrapper to call ghidra-headless-mcp tools via stdio JSON-RPC |

## Quickrace launcher (subdirectory)

`quickrace/` — Frida-based race launcher used by `/diff-race` and `/fix`.
See `td5_quickrace.py --help`.

## Archived

`research-archive/` — older one-off experiments retained for reference.
