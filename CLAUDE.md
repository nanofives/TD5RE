# TD5RE — Test Drive 5 Reverse Engineering & Source Port

## What this is

Reverse engineering project and clean-room C source port of **Test Drive 5** (Pitbull Syndicate / Accolade, 1999), derived from a full Ghidra decompilation of `TD5_d3d.exe` (822 functions analyzed, 864 named).

Build target: **td5re.exe** — standalone source-port executable (D3D11 backend, no original DLLs needed).

The original binary is a Win32 x86 DirectDraw/Direct3D 3 game. The source port replaces the DDraw layer with a D3D11 wrapper (`ddraw_wrapper/`).

## Directory layout

```
TD5RE/
├── td5re.exe             # DEV build output (debug affordances, trace harness)
├── td5re_release.exe     # RELEASE build output (TD5RE_RELEASE, stripped)
├── td5re.ini             # Runtime config (dev); td5re_release.ini for release
├── log/                  # Runtime logs + CSV traces (gitignored)
├── original/             # Clean unmodified game files (backup, not used at runtime)
│   ├── TD5_d3d.exe       # Original binary (RE target)
│   ├── M2DX.dll          # Original M2DX middleware
│   ├── level*.zip        # Track data archives
│   ├── cars/             # Car data archives
│   ├── Front End/        # Frontend assets (frontend.zip, TGAs)
│   ├── sound/            # Audio archives
│   └── movie/            # FMV files
├── td5mod/
│   ├── src/td5re/        # Source port modules (28 .c files, see table below)
│   ├── ddraw_wrapper/    # DirectDraw → D3D11 translation layer (static lib)
│   └── deps/mingw/       # Bundled MinGW-w64 i686 toolchain
├── re/                   # RE analysis, extracted assets, tools
│   ├── assets/           # All game data — pre-extracted PNGs, DATs, WAVs, meshes (runtime asset directory)
│   ├── ghidra_export/    # FULL decompilation as greppable text (functions/, symbols.csv, structs.h) — check BEFORE live Ghidra
│   ├── tools/            # RE helper scripts (extractor, alpha tool, etc.)
│   ├── analysis/         # RE analysis notes
│   └── sessions/         # RE session logs
├── scripts/              # selftest.ps1, ExportAllDecomp.java, deploy/worktree helpers
├── ghidra_pool/          # TD6_re side project only (TD5 pool slots retired 2026-07-03)
├── tools/                # Cloned MCP helper repos, capture_window.ps1, frida CSVs
└── ghidra_12.0.3_PUBLIC/ # Ghidra installation
```

## Build commands

```bat
cd td5mod\src\td5re
build_all.bat                 :: canonical — refresh ddraw_wrapper lib if stale, then DEV + RELEASE
build_standalone.bat          :: DEV only     -> td5re.exe
build_standalone.bat release  :: RELEASE only -> td5re_release.exe (-DTD5RE_RELEASE -DNDEBUG, stripped)
```

Both exes deploy to the project root. Object dirs are `build\` (dev) and
`build_release\` — separate so differing `-D` flags never share a stale .o cache.
The D3D11 wrapper is a prebuilt static lib at `td5mod/ddraw_wrapper/build/libddraw_wrapper.a`;
`build_all.bat` rebuilds it automatically when `ddraw_wrapper/src` is newer
(standalone wrapper rebuild: `td5mod/ddraw_wrapper/build.bat`).

DEV vs RELEASE: same module list; `TD5RE_RELEASE` compiles out dev affordances
(trace knobs, debug overlays, net selftest) and strips symbols.

## Runtime logs & dev harness

- `[Logging] Enabled=1` in td5re.ini routes `TD5_LOG_*` to `log/`:
  `frontend.log` (frontend/hud/save/input), `race.log` (game/physics/ai/track/camera/vfx),
  `engine.log` (render/asset/platform/sound/net/fmv/main). The frontend log flushes
  on clean shutdown — close the window, don't kill the process.
- Every INI key has a `--Key=N` CLI override (CLI > INI), e.g.
  `td5re.exe --AutoRace=1 --SkipIntro=1 --DefaultTrack=5`.
- `[Game] AutoRace/SkipIntro/StartScreen` — boot straight into a race / frontend screen N.
- `[Trace] RaceTrace=1` — per-sim-tick CSV trace to `log/race_trace_*.csv`
  (modular via `Modules`/`Stages`; fixes the RNG seed for deterministic A/B runs).
- `TD5RE_WINDOW_TITLE` env var — overrides the window caption (test harnesses).
- **Self-test suite** (dev builds): `td5re.exe --SelfTest=1` (smoke, ~30 s) or
  `--SelfTest=1 --SelfTestSuite=1` (full, ~1 min; races at 8x) runs an in-session
  automated suite — frontend screen walk with nav-reachability checks + a
  multi-race matrix (repeats, circuit, reverse, TD6, drag, arcade/traffic/cops,
  spectate panes) with degradation monitoring (private bytes, GDI/handles,
  frame times) across repeated races. Report: `log/selftest_report.{csv,md}`;
  process exit code 0/1. Runner: `pwsh scripts/selftest.ps1 [-Suite full]`.
  See `td5mod/src/td5re/td5_selftest.c`; thresholds via `TD5RE_SELFTEST_*`.

## Source port modules (28 .c)

| File | Responsibility |
|------|---------------|
| `main.c` | Entry point, INI/CLI config load, D3D11 bootstrap, main loop |
| `td5re.c` | Module table, master init/shutdown |
| `td5_game.c` | Race FSM (intro/menu/race/benchmark), fixed-30Hz tick loop, viewports, results |
| `td5_physics.c` | 4-wheel player + 2-axle AI + simplified traffic dynamics, V2V/wall collisions |
| `td5_track.c` | STRIP.DAT parser, span contacts, segment walking, traffic FIFO |
| `td5_ai.c` | Routing (LEFT/RIGHT.TRK), rubber-banding, script VM, traffic recycle, SmartAI layer |
| `td5_camera.c` | Per-tick solve + per-frame interpolate; 7 chase presets, trackside, spline, orbit |
| `td5_render.c` | Software transform, frustum cull, mesh dispatch, trig LUT wrappers |
| `td5_trig_lut_data.c` | Baked 12-bit trig LUT data |
| `td5_vfx.c` | Particles, tire tracks, smoke, weather, billboards |
| `td5_frontend.c` | 30-entry screen table, navigation FSM, all menu screens |
| `td5_frontend_button_cache.c` | CPU-baked 224×64 main-menu button surfaces |
| `td5_font.c` | Runtime TTF glyph cache (stb_truetype) for vector text |
| `td5_hud.c` | Speedometer, minimap, timers, pause overlay, split-screen dividers/plates |
| `td5_asset.c` | ZIP archive, TGA decode, texture pages, mesh prepare |
| `td5_assetsrc.c` | Editable-source assets: re-encodes JSON/PNG sources to original DAT layout at load |
| `td5_inflate.c` | Decompression utility (zlib-backed) |
| `td5_save.c` | Organized INI config (td5re_input/progress/cup.ini); one-time import of legacy Config.td5/CupData.td5 |
| `td5_sound.c` | DirectSound wrapper, vehicle audio, ambient, CD audio (MCI) |
| `td5_input.c` | Polling, controller config, force feedback, race replays |
| `td5_net.c` | Winsock2 UDP lockstep netplay (DXPTYPE-derived protocol; replaces DirectPlay) |
| `td5_upnp.c` | Minimal UPnP IGD port mapping for netplay hosting |
| `td5_fmv.c` | FMV playback via Media Foundation (replaces EA TGQ codec) |
| `td5_benchmark.c` | Benchmark mode capture + report (`EnableBenchmark` INI) |
| `td5_trace.c` | Dev CSV race-trace harness (`[Trace]` knobs; inert when off) |
| `td5_profile.c` | Per-phase frame profiler (`Profile` INI knob) |
| `td5_msvc_rand.c` | MSVC CRT rand()/srand() override — netplay/replay determinism (DO NOT remove) |
| `td5_platform_win32.c` | Win32/D3D11/DInput/DSound platform layer, window, logging sinks |

Key headers: `td5_types.h` (structs, verified against 0x388 actor stride),
`td5re.h` (global state `g_td5` + INI struct), `td5_platform.h` (platform API +
`TD5_LOG_*`), `td5_camera_profiles.h` (chase-preset tables),
`td5_orig_globals.h` (Ghidra name ↔ port name mapping, documentation only).

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

### Ghidra: offline export first, live master second

**Before opening live Ghidra, grep `re/ghidra_export/`** — the full annotated decompilation as text
(`functions/0x<addr>_<Name>.c`, `symbols.csv`, `globals.csv`, `structs.h`; see its README). It answers
most "what did the original do?" questions with zero locking, and facts found there count as confirmed
decompilation data (`[CONFIRMED @ 0xADDR]`). Live Ghidra is only needed for interactive xref exploration
or writing annotations. Regenerate the export after annotation work:
`ghidra_12.0.3_PUBLIC/support/analyzeHeadless.bat . TD5 -process TD5_d3d.exe -noanalysis -readOnly -scriptPath scripts -postScript ExportAllDecomp.java`

Also: fixes/features that are **TD5RE-only** (no original counterpart — arcade, lane assist, drag mode,
selftest, MP extensions, etc.) need **no** original-binary research at all — see the triage step in `/fix`.

### Live Ghidra (rare — open the master project directly)

The pool of project clones was retired 2026-07-03 (with `re/ghidra_export/` there is no parallel live
access to arbitrate; `ghidra_pool/` now holds only the `TD6_re` side project). For the rare live session:

`project_program_open_existing(project_location="C:/Users/maria/Desktop/Proyectos/TD5RE", project_name="TD5", program_name="TD5_d3d.exe", read_only=true)`

**HARD RULE: `read_only=true` for all research.** `read_only=false` is reserved for `/ghidra-apply`
(annotation writes; single writer, one at a time), which must regenerate `re/ghidra_export/` afterwards.
If open fails with LockException and no Ghidra session is actually running, the lock is stale —
`rm -f TD5.lock TD5.lock~` at the repo root and retry.

## Debug shortcuts (in-race)

- **F12** — toggle the collision-wireframe overlay (white = wall rails, cyan = span boundaries, yellow = player's current span). Mirrors `td5re.ini [Debug] Collisions` and `--DebugCollisions=N`.

## Known issues / current work

See `td5mod/src/td5re/EXPECTED_BEHAVIOR.md` for full behavioral spec.

## Semgrep (local scan)

```bash
# Run directly — MCP tool is deprecated in OSS version
semgrep --config=auto --lang=c --quiet td5mod/src/td5re/
```

## Delegating read-only analysis (repo-fleet worker)

Workspace policy (Proyectos): read-only analysis goes to the worker account (account2/Accenture —
headless, never prompts, doesn't burn this account's quota). A user-level PreToolUse gate denies plain
local Agent/Task spawns under Proyectos; prefix an agent prompt with `[local]` only for the keep-local
cases below.

Delegate one task (result prints to stdout; add `-Model haiku|sonnet` for bulk, default is opus):

```
pwsh -NoProfile -File "C:\Users\maria\Desktop\Proyectos\.claude\skills\repo-fleet\scripts\delegate.ps1" -Prompt "<task>" -Repo TD5RE
```

**Good delegation targets here** (read-only over the working tree — the worker has Read/Grep/Glob):
- stub/TODO/FIXME/divergence audits across the 28 port modules
- cross-referencing ported code in `td5mod/src/td5re/` against `re/analysis/` exports; RVA /
  `FUN_XXXXXXXX` lookups in already-exported sources and docs
- convention sweeps (fixed-point usage, BGRA order, `__stdcall`/`__cdecl`, naming) and consistency hunts
- summarizing/drafting docs from sources; reviewing committed diffs read-only via `.git`

**Keep local** (`[local]` prefix or in-session): anything needing Ghidra/x64dbg/frida MCP, builds,
running the game/dev harness, semgrep, edits/writes, git mutations.
