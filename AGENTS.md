# TD5RE — Test Drive 5 Reverse Engineering & Source Port

## Project type
Reverse engineering + clean-room C source port of **Test Drive 5** (Pitbull Syndicate / Accolade, 1999).
Derived from a full Ghidra decompilation of `TD5_d3d.exe` (864 named functions).
Original binary: Win32 x86, DirectDraw/Direct3D 3.

Build target: `td5re.exe` — standalone source port (D3D11 backend, no original DLLs needed).

## Build — ALWAYS use this to verify your changes compile

```
cd C:\Users\maria\Desktop\Proyectos\TD5RE\td5mod\src\td5re
build_standalone.bat
```

**Compiler:** `td5mod\deps\mingw\mingw32\bin\gcc.exe` (MinGW i686, 32-bit, C11)  
**Output:** `C:\Users\maria\Desktop\Proyectos\TD5RE\td5re.exe`  
**Build time:** ~20 seconds  
**Key flags:** `-m32 -O2 -DTD5_INFLATE_USE_ZLIB -I<zlib> -I<wrapper_src>`

After any `.c` or `.h` edit, run the build and fix all errors before finishing.

## Directory layout

```
TD5RE/
├── td5re.exe                # Built output
├── original/                # Game data (working dir for td5re.exe)
│   ├── level*.zip           # Track data
│   ├── cars/                # Vehicle ZIPs
│   ├── Front End/           # Frontend assets
│   └── sound/               # Audio archives
├── td5mod/
│   ├── src/td5re/           # ← ALL SOURCE FILES ARE HERE
│   │   ├── build_standalone.bat
│   │   ├── td5_game.c       td5_physics.c   td5_track.c
│   │   ├── td5_ai.c         td5_render.c    td5_frontend.c
│   │   ├── td5_hud.c        td5_sound.c     td5_input.c
│   │   ├── td5_asset.c      td5_save.c      td5_net.c
│   │   ├── td5_camera.c     td5_vfx.c       td5_fmv.c
│   │   ├── td5_inflate.c    main.c
│   │   └── build/           # .o files and td5re.exe here during build
│   └── ddraw_wrapper/       # DirectDraw → D3D11 translation layer
├── re/                      # RE analysis, extracted assets, PNG textures
└── tools/                   # Local MCP helper tools
```

## Module responsibilities

| File | What it does |
|------|-------------|
| `main.c` | Entry point, D3D11 bootstrap, main loop |
| `td5_game.c` | 4-state FSM: intro → menu → race → benchmark |
| `td5_physics.c` | 4-wheel player + 2-axle AI + traffic dynamics |
| `td5_track.c` | STRIP.DAT parser, span contacts, segment walking |
| `td5_ai.c` | Routing (LEFT/RIGHT.TRK), rubber-banding, script VM |
| `td5_camera.c` | 7 chase presets, trackside, spline, orbit modes |
| `td5_render.c` | Software transform, frustum cull, mesh dispatch |
| `td5_vfx.c` | Particles, tire tracks, smoke, weather, billboards |
| `td5_frontend.c` | 30-entry screen table, navigation FSM |
| `td5_hud.c` | Speedometer, minimap, timers, text overlay |
| `td5_asset.c` | ZIP archive, TGA decode, mesh prepare, static atlas |
| `td5_save.c` | Config.td5, CupData.td5, XOR encryption |
| `td5_sound.c` | DXSound wrapper, vehicle audio, ambient, CD |
| `td5_input.c` | Polling, controller config, force feedback |
| `td5_net.c` | DirectPlay lockstep, DXPTYPE protocol |
| `td5_fmv.c` | FMV stub (replaces EA TGQ codec) |
| `td5_inflate.c` | zlib-backed DEFLATE decompression |

## Key RE constants

| Constant | Value | Meaning |
|----------|-------|---------|
| Actor stride | `0x388` = 904 bytes | `sizeof(Actor)` in original binary |
| Max racers | 6 | Player + AI cars |
| Max traffic | 6 | Background vehicles |
| Fixed-point shift | 8 | 24.8 format: divide raw ints by 256.0f |
| Angle full circle | `0x1000` = 4096 | Internal angle units |
| Game heap | 24 MB | Initial allocation |
| Frontend screens | 30 | Screen table entries |
| `g_worldToRenderScale` | `1/256 = 0.00390625f` | World→render coordinate scale |

## Coding conventions

- **Coordinate scale:** All actor/mesh positions are stored as 24.8 fixed-point. Divide by 256.0f (`g_worldToRenderScale`) before passing to the render system.
- **Color format:** Original uses BGRA byte order (not RGBA). D3D11 format is `B8G8R8A8_UNORM`.
- **Angles:** 12-bit angle system: 0..4095 maps to 0..360°. Use `angle * (2π / 4096.0f)` to convert.
- **Calling convention:** Original functions are `__stdcall`; source port functions are `__cdecl`.
- **Ghidra addresses:** When referencing original binary functions, use hex addresses like `0x004012AB`.
- **Function naming:** `td5_<module>_<verb>_<noun>`, matching Ghidra names where possible.
- **Structs:** Defined in `td5_types.h`. Verify offsets against `0x388` actor stride.
- **ZIP reading:** Use `td5_asset_open_archive` / `td5_asset_read_entry` / `td5_asset_close_archive`.
- **Texture upload:** `td5_plat_render_upload_texture(page_idx, bgra_data, w, h, 2)` — bpp=2 means BGRA32.
- **Static atlas:** Pages 700–730 are reserved for `static.hed` sprites. Do not use these page IDs for track/vehicle textures.
- **Log macros:** `TD5_LOG_I(tag, fmt, ...)`, `TD5_LOG_W(...)`, `TD5_LOG_D(...)`.

## Platform API quick reference

```c
// File I/O
TD5_File *td5_plat_file_open_read(const char *path);
int64_t   td5_plat_file_size(TD5_File *f);
int       td5_plat_file_read(TD5_File *f, void *buf, int size);
void      td5_plat_file_close(TD5_File *f);
int       td5_plat_file_exists(const char *path);

// Memory
void *td5_plat_heap_alloc(size_t size);
void  td5_plat_heap_free(void *ptr);

// Rendering
int   td5_plat_render_upload_texture(int page, const void *data, int w, int h, int bpp_mode);
void  td5_plat_render_draw_tris(TD5_D3DVertex *verts, int nverts, uint16_t *idx, int nidx);
void  td5_plat_present(int flags);
```

## Asset loading patterns

```c
// Load a ZIP entry into a malloc'd buffer (caller frees)
void *data = td5_asset_open_and_read("FILENAME.DAT", "path/to/archive.zip", &out_size);

// Atlas lookup (always returns non-NULL, fallback to zeroed entry on miss)
TD5_AtlasEntry *e = td5_asset_find_atlas_entry(NULL, "SPEEDO");
// e->atlas_x, e->atlas_y, e->width, e->height, e->texture_page
```

## Known working state

- Race runs for extended sessions (~88 seconds observed, 2664 frames)
- Loading screen inflate: fixed (zlib path enabled via `-DTD5_INFLATE_USE_ZLIB`)
- Static atlas: implemented — 51 entries from `assets/static/static.hed`, tpage0-2 loaded, pages 3+ have magenta placeholders
- Geometry: 2648 span display lists submitted per frame
- Physics, AI, camera: all active

## Remaining known issues (as of 2026-04-01)

- HUD sprites on atlas pages 3-17 show as magenta (tpage art not in dataset)
- Button 9-slice UV corners may be cosmetically off
- Race/cup details may appear before entry animation completes
- Car/track selector default order needs verification
- Vehicle sound banks not loaded
