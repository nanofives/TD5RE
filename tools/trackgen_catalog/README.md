# Auto-track piece + texture catalogue (dev tool, round UD)

A local web page that shows every piece and every texture page the auto-generated
track (slot 60, `td5_trackgen.c` + `td5_tg_*.c`) can produce, the code that emits
each one, and how to add more. Catalogue only: it reads the code and built levels
and changes nothing in the generator.

```
python tools/trackgen_catalog/serve.py --build      # build, then http://127.0.0.1:6700/
python tools/trackgen_catalog/serve.py              # serve the last build
python tools/trackgen_catalog/gen_levels.py         # optional: more seeds (biome coverage)
```

Port 6700 is registered for TD5RE in `Proyectos/PORTS.md`; the server binds
127.0.0.1 only. Python 3 + Pillow + numpy, nothing else, no CDN.

| File | Role |
|---|---|
| `build.py` | builds `site/` (gitignored): pieces grid, texture grid, code map, frames, guide |
| `serve.py` | stdlib `http.server` on 127.0.0.1 |
| `gen_levels.py` | builds extra seeds through `verify/geo_tags_identity.ps1`, stores them in `levels/` (gitignored), rebuilds the reference seed 99991 last |
| `tgcat_code.py` | reads the source: page slot registry, MESHTAG kind stamp sites, page macro uses, knobs, biome table |
| `tgcat_level.py` | reads a built level: MODELS.DAT (slot-preserving), MESHTAG.BIN, TEXTURES.DAT, GENSTAMP, plus the shipped-page hash index |
| `tgcat_render.py` | offline textured orthographic thumbnail renderer (numpy z-buffer) |
| `tgcat_md.py` | tiny Markdown to HTML for the guide |
| `HOWTO.md` | the "how to add a piece / a texture" guide, shown on the site |

Inputs: `re/assets/levels/level090` must hold a DEV build of the auto track
(MESHTAG.BIN is dev-only). The synthetic reference is seed 99991:
move `re/assets/geo/SELECTED.TXT` aside and run
`pwsh verify/geo_tags_identity.ps1 -Arm synthetic -Tag cat -NoControl`.

In-game context frames: put PNGs plus a `frames.json`
(`[{"file": "x.png", "title": "...", "note": "..."}]`) in `levels/frames/`
and rebuild.
