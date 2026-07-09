# Session 04 — Visual side-by-side capture (floating cars + HUD transparency)

## Goal
Use RenderDoc to capture matching frames from `TD5_d3d.exe` (orig) and
`td5re.exe` (port). Diff the GPU command streams to localize:
- Why cars appear "floating" at race start (item 1)
- Why HUD speedometer and minimap have different transparency vs orig

## Context — read these first

- `memory/reference_feedback_batch_2026-05-21.md` — item 3 HUD analysis
- `memory/reference_fix_10_items_2026-05-22.md` — item 1 floating analysis
- `memory/reference_feedback_batch_2_2026-05-21.md` — confirmed world_y matches orig at countdown, so floating is a render-side issue

## Setup

1. **Install RenderDoc** (`https://renderdoc.org/`) — free.
2. **Install or set up DDraw shim** for orig (already in `original/` directory):
   - `original/ddraw.dll` (DDrawCompat shim) — already present
3. **Verify both exes launch independently** before capturing.

## Approach

### Part A: Floating cars

1. Launch `original/TD5_d3d.exe` in RenderDoc (Inject or Launch with RenderDoc Wrapper).
2. Drive into a race (Edinburgh, Viper, single race) — let the countdown finish.
3. At the moment cars are at start grid (mid-countdown), press F12 in RenderDoc to capture a frame.
4. Save the .rdc capture.
5. Launch `td5re.exe` from project root in RenderDoc (same scenario).
6. Capture same moment.
7. Open both captures side-by-side in RenderDoc.
8. Locate the **car mesh draw call** (identifiable by vertex buffer matching cardef geometry).
9. Diff the **world transform matrix** passed to the vertex shader:
   - Y translation (world_pos.y) — already verified identical via Frida
   - **Cardef height_offset application** — orig may apply `cardef+0x86 << 8` to chassis center Y; port only does this for traffic
10. If different: trace back to port mesh transform writer.

### Part B: HUD transparency

1. In the same captures, locate the **minimap draw call** (textured quad at screen corner).
2. Diff the **blend state**:
   - Orig: likely `D3DBLEND_SRCALPHA / D3DBLEND_INVSRCALPHA` with some alpha
   - Port: per `td5_hud.c:2475` uses `td5_render_submit_translucent_low_ref` (alpha_ref=1)
3. Compare effective alpha output.
4. Same for **speedometer dial** draw call.
5. Adjust port blend setup to match.

## Tools

- RenderDoc 1.30+ (Windows)
- DDrawCompat shim already in `original/`
- Port `td5re.exe` at project root

## Success criteria

- Cars sit at the SAME visual Y as orig at race start (no perceived floating)
- HUD speedometer and minimap have matching alpha/blend as orig

## Files likely touched

- `td5mod/src/td5re/td5_render.c` (mesh transform; possibly add chassis cardef +0x86 to player Y too)
- `td5mod/src/td5re/td5_hud.c` (blend state for minimap/speedometer)

## Risk
LOW for diagnosis (read-only RenderDoc capture). Fix risk depends on findings.

## Estimated time
2-4 hours (RenderDoc setup + capture + diff + fix iteration).
