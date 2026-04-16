# TD5RE — Test Drive 5 Reverse Engineering & Source Port

## What this is

Reverse engineering project and clean-room C source port of **Test Drive 5** (Pitbull Syndicate / Accolade, 1999), derived from a full Ghidra decompilation of `TD5_d3d.exe` (822 functions analyzed, 864 named).

Build target: **td5re.exe** — standalone source-port executable (D3D11 backend, no original DLLs needed).

The original binary is a Win32 x86 DirectDraw/Direct3D 3 game. The source port replaces the DDraw layer with a D3D11 wrapper (`ddraw_wrapper/`).

## Directory layout

```
TD5RE/
├── td5re.exe             # Built source port output
├── original/             # Clean unmodified game files (backup, not used at runtime)
│   ├── TD5_d3d.exe       # Original binary (RE target)
│   ├── M2DX.dll          # Original M2DX middleware
│   ├── level*.zip        # Track data archives
│   ├── cars/             # Car data archives
│   ├── Front End/        # Frontend assets (frontend.zip, TGAs)
│   ├── sound/            # Audio archives
│   └── movie/            # FMV files
├── td5mod/
│   ├── src/td5re/        # Source port modules (15 .c files)
│   ├── ddraw_wrapper/    # DirectDraw → D3D11 translation layer
│   └── deps/mingw/       # Bundled MinGW-w64 i686 toolchain
├── re/                   # RE analysis, extracted assets, tools
│   ├── assets/           # All game data — pre-extracted PNGs, DATs, WAVs, meshes (runtime asset directory)
│   ├── tools/            # RE helper scripts (extractor, alpha tool, etc.)
│   ├── analysis/         # RE analysis notes
│   └── sessions/         # RE session logs
├── tools/                # Cloned MCP helper repos
└── ghidra_12.0.3_PUBLIC/ # Ghidra installation
```

## Build commands

### Source port (td5re.exe)
```bash
# From td5mod/src/td5re/
GCC="../../deps/mingw/mingw32/bin/gcc.exe"
AR="../../deps/mingw/mingw32/bin/ar.exe"
WRAPPER_SRC="../../ddraw_wrapper/src"
WRAPPER_BUILD="../../ddraw_wrapper/build"
ZLIB_INC="../../deps/mingw/mingw32/i686-w64-mingw32/include"
ZLIB_LIB="../../deps/mingw/mingw32/i686-w64-mingw32/lib"

# Compile each module: td5_game.c td5_physics.c td5_track.c etc.
"$GCC" -m32 -O2 -Wall -std=c11 -I. -I"$WRAPPER_SRC" -I"$ZLIB_INC" -DTD5_INFLATE_USE_ZLIB -c td5_game.c -o build/td5_game.o

# Archive
"$AR" rcs build/libtd5re.a build/td5re.o build/td5_*.o

# Link
"$GCC" -m32 -mwindows -static -o build/td5re.exe build/main.o build/td5re_stubs.o build/libtd5re.a "$WRAPPER_BUILD"/*.o -L"$ZLIB_LIB" -lz -lkernel32 -luser32 -lgdi32 -ld3d11 -ldxgi -ldinput8 -ldsound -lwinmm -lole32 -lshell32 -luuid
```

## Source port modules

| File | Responsibility |
|------|---------------|
| `main.c` | Entry point, D3D11 bootstrap, main loop |
| `td5_game.c` | Main loop, 4-state FSM (intro/menu/race/benchmark) |
| `td5_physics.c` | 4-wheel player + 2-axle AI + simplified traffic dynamics |
| `td5_track.c` | STRIP.DAT parser, span contacts, segment walking |
| `td5_ai.c` | Routing (LEFT/RIGHT.TRK), rubber-banding, script VM |
| `td5_camera.c` | 7 chase presets, trackside, spline, orbit modes |
| `td5_render.c` | Software transform, frustum cull, mesh dispatch |
| `td5_vfx.c` | Particles, tire tracks, smoke, weather, billboards |
| `td5_frontend.c` | 30-entry screen table, navigation FSM |
| `td5_hud.c` | Speedometer, minimap, timers, text overlay |
| `td5_asset.c` | ZIP archive, TGA decode, mesh prepare |
| `td5_save.c` | Config.td5, CupData.td5, XOR encryption |
| `td5_sound.c` | DXSound wrapper, vehicle audio, ambient, CD |
| `td5_input.c` | Polling, controller config, force feedback |
| `td5_net.c` | DirectPlay lockstep, DXPTYPE protocol |
| `td5_fmv.c` | FMV stub (replaces EA TGQ codec) |
| `td5_inflate.c` | Decompression utility |

## Key constants (from RE)

| Constant | Value | Meaning |
|----------|-------|---------|
| Actor stride | `0x388` (904 bytes) | sizeof(Actor) in original binary |
| Max racers | 6 | Player + AI cars |
| Max traffic | 6 | Background vehicles |
| Fixed-point shift | 8 | 24.8 format |
| Angle full circle | `0x1000` (4096) | Internal angle units |
| Game heap | 24 MB | Initial allocation |
| Frontend screens | 30 | Screen table entries |

## RE conventions

- **Address references**: Use Ghidra virtual addresses (e.g., `0x00401234`)
- **Function naming**: Match Ghidra names; use `td5_<module>_<verb>_<noun>` for new names
- **Fixed-point**: Coordinates use 24.8 fixed-point; use `FP_TO_FLOAT(x)` macro
- **Structs**: Defined in `td5_types.h`; verify field offsets against `0x388` actor stride
- **Calling convention**: `__stdcall` for original functions, `__cdecl` for source port
- **BGRA**: Original uses BGRA color order (not RGBA)

## MCP servers (project-local)

| Server | Purpose | Auto-invoke when |
|--------|---------|-----------------|
| `ghidra` | Static decompilation, function rename, struct defs | Analyzing original binary |
| `x64dbg` | Live debugging, breakpoints, register inspection | Step-debugging TD5_d3d.exe (requires x32dbg install) |
| `microsoft-learn` | Win32 API / DirectX / DXGI docs | Any Win32/DX API lookup |
| `frida-game-hacking` | Runtime hooking, memory scan, AoB patterns | Observing runtime behavior of TD5.exe |
| `codebase-memory` | Cross-reference source port (2286 nodes indexed) | Navigation across source modules |
| `pyghidra-mcp` | Headless multi-binary Ghidra analysis | Cross-binary call tracing |
| `radare2` (pending) | Quick binary triage, 26+ tools | Fast analysis without Ghidra startup |
| `semgrep` | C static analysis (OSS, tools deprecated) | Use Bash semgrep directly |

**Frida attach:** `attach("TD5.exe")` or `attach("TD5_d3d.exe")` — process must be running.
**codebase-memory index:** `td5mod/src/td5re` only (deps/mingw crashes the indexer).

### Ghidra pool (parallel session access)

Multiple sessions share Ghidra via a pool of project clones in `ghidra_pool/`. Managed by `scripts/ghidra_pool.sh`.

**HARD RULE: Always open Ghidra projects with `read_only=true`.** Never use `read_only=false`. This ensures `/ghidra-sync` can refresh pool slots without conflicts. Any session violating this blocks sync for all others.

To open Ghidra in any context (skills, subagents, ad-hoc):
1. `bash scripts/ghidra_pool.sh acquire` → get slot name (e.g. `TD5_pool2`)
2. `project_program_open_existing(project_location=".../ghidra_pool", project_name="TD5_pool2", program_name="TD5_d3d.exe", read_only=true)`
3. After done: `bash scripts/ghidra_pool.sh cleanup`

## Known issues / current work

See `td5mod/src/td5re/EXPECTED_BEHAVIOR.md` for full behavioral spec.

## Semgrep (local scan)

```bash
# Run directly — MCP tool is deprecated in OSS version
semgrep --config=auto --lang=c --quiet td5mod/src/td5re/
```
