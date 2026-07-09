# Split-screen / multiplayer render-performance diagnostic — 2026-06-08

## What was built (dev tooling for this investigation)

A **Quick Race "AI Screens" selector** (dev-only) and matching **`[Game] SpectateScreens=N`**
AutoRace knob render the first N AI cars (slots 1..N) each in its own split-screen
viewport pane on top of the player's pane. Total panes = `1 + N`. This decouples the
viewport count from the human-player count so the N-way split render path can be
stress-tested with a single (or AI) driver.

- `g_td5.num_spectate_screens` feeds `td5_game_init_viewport_layout`
  (`viewport_count = num_human_players + num_spectate_screens`). The existing
  viewport→actor map (`g_actorSlotForView[vp] = vp`) already points pane `vp` at slot
  `vp`, so panes 1..N follow AI-driven cars. **Input still goes only to the human(s)**
  (`td5_input_set_active_players(num_human_players)`), so the spectator panes are pure
  cameras. 0 = off = byte-identical to the legacy single-view path.
- Profiler additions (permanent, profile-gated, ~zero cost when off):
  - `td5_profile.c` now logs a true per-FRAME `frame=avg/max fps=N` (the per-sub-tick
    trace stages like `post_physics` read inflated and are NOT a frame metric).
  - `td5_render_end_scene` logs `RENDERSTAT views/draws/tris/binds/texmiss/texevict/spanmesh`
    once/second when `[Logging] Profile=1`.

Harness: `log/spectate_sweep.ps1` (AutoRace + PlayerIsAI + VSync off, holds the track
constant, sweeps `SpectateScreens` 0..5, parses the PROFILE lines).

## Measurements (track held constant, 1920×1009, VSync off, AI driving)

40-sample-per-point sweep averages:

| Panes | AI screens | Render avg (ms) | Frame avg (ms) | FPS avg | FPS min |
|------:|-----------:|----------------:|---------------:|--------:|--------:|
| 1 | 0 | 2.9 | 1.9 | 555 | 350 |
| 2 | 1 | 4.4 | 4.5 | 276 | 38 |
| 3 | 2 | 5.1 | 5.7 | 237 | 22 |
| 4 | 3 | 7.6 | 9.8 | 157 | 12 |
| 5 | 4 | 63.6 | 79.9 | 32 | 10 |
| 6 | 5 | 79.9 | 96.9 | 29 | 8 |

The averages look like a clean cliff at 5 panes — but a second, independent capture
**flipped it**: 4 panes measured 101 ms / 10 fps while 5 panes measured 9 ms / 112 fps,
with *similar* draw counts (2200 vs 3170). So the slowdown is **not** a deterministic
function of pane count. Decisive numbers from that capture:

- 4-pane stalled frame: 101 ms / 2200 draws = **46 µs per draw**
- 5-pane smooth frame:    9 ms / 3170 draws = **2.8 µs per draw**

Same code path, **16× difference in per-draw time** → the CPU is stalling *inside the
draw call*, waiting on the GPU. It is not submission-count-bound (more draws was faster).

`RENDERSTAT` also shows the renderer issues **~500–700 draw calls per pane**
(≈2200–3500/frame at 4–6 panes). `texmiss=0 texevict=0` everywhere → the texture-page
cache (600 slots) is **not** the problem.

## Root cause

`ddraw_wrapper/src/device3.c : Dev3_DrawIndexedPrimitive` (the D3D3→D3D11 draw path)
does, **for every single mesh/primitive batch**:

```
Map(dynamic_vb, WRITE_DISCARD) → memcpy verts → Unmap     // 128 KB buffer
Map(dynamic_ib, WRITE_DISCARD) → memcpy idx   → Unmap     //  32 KB buffer
IASetVertexBuffers / IASetIndexBuffer / topology / layout / VS
Backend_ApplyStateCache()
DrawIndexed(...)
```

The dynamic VB/IB are sized for **one draw's worth** of geometry (128 KB ≈ 4000 verts,
32 KB ≈ 16000 indices), so each draw `WRITE_DISCARD`-renames the *entire* buffer. The TD5
renderer flushes a draw on every texture-page / render-state change, so a single screen
already emits ~500–700 draws/frame. **Split-screen re-renders the whole scene once per
pane** (each pane walks its own ~64-span track window — the port does NOT shrink the
track window for split, unlike the original's halved split window), multiplying draws to
2200–3500/frame ⇒ **4400–7000 `Map(WRITE_DISCARD)` calls per frame**.

`Map(WRITE_DISCARD)` only returns a fresh region while the driver has a spare rename
buffer. At thousands of DISCARDs/frame the rename ring is exhausted and the call **blocks
the CPU until the GPU drains an in-flight copy**. Whenever the GPU is even slightly behind
(a pane looking down a long sightline, cars bunched at the grid → overdraw), every
subsequent Map serializes CPU↔GPU and the frame balloons to 50–100 ms. Hence the behavior
is **bimodal/bursty** (≈9 ms when the GPU keeps up, 50–100 ms when it doesn't), and the
*frequency* of the stalled frames — not a hard threshold — rises with pane count, which is
why the 40-sample averages trend up monotonically while individual runs are noisy.

## How to improve the multiplayer experience (prioritized)

1. **Streaming dynamic buffer — the real fix (wrapper).** Replace per-draw
   `Map(WRITE_DISCARD)` with a large ring buffer: `Map(WRITE_NO_OVERWRITE)` appends at a
   running offset and the draw uses `DrawIndexed(..., StartIndexLocation, BaseVertex)`;
   `Map(WRITE_DISCARD)` only when the offset wraps (a handful of times per frame, not
   thousands). Grow `DYNAMIC_VB_SIZE`/`DYNAMIC_IB_SIZE` to hold a frame's worth (e.g.
   4–8 MB / 1–2 MB). This collapses thousands of DISCARDs to ~1–4 per frame and removes
   the serialization stall — expected to make 6-pane split-screen run at the per-draw
   cost (~2.8 µs), i.e. roughly the linear 6× of single-screen instead of the 20–40×
   stalled cost. Touches the universal draw path (frontend, HUD, race), so it needs
   careful correctness verification, but it is the single highest-leverage change.

2. **Cut draw calls via texture-page batching (renderer).** The opaque track/actor path
   flushes on every texture-page change. Sorting a pane's opaque primitives by page (as
   the translucent pipeline already does) coalesces consecutive same-page meshes into one
   draw, cutting the 500–700 draws/pane materially — compounding with #1.

3. **Faithful per-pane split-screen view reduction (renderer).** The original halved the
   split-screen span window (max 0x20 vs 0x40). The port keeps the full 64-span track
   window per pane (`VIEW_DIST_FWD_SPANS` is split-independent in
   `td5_render_actors_for_view`); the actor pop-in cull already halves
   (`cull_max = split ? 32 : 64`) but the track display-list walk does not. Restoring a
   split-scaled track window (after RE-confirming the original's split max-spans) roughly
   halves per-pane track draws with little visible loss on a small pane.

4. **UX guardrail.** Until #1 lands, ≤4 panes stays smooth (≥150 fps avg); 5–6 panes is
   playable but spiky. Consider defaulting spectator/extra panes to a reduced view
   distance, and/or surfacing a recommended cap in the selector.

## UPDATE — fix #1 implemented (streaming ring buffer) + results

Implemented the streaming dynamic buffer. `Backend_StreamUpload` (d3d11_backend.c)
appends verts (+ optional 16-bit indices) to the persistent dynamic VB/IB at a
running offset with `WRITE_NO_OVERWRITE`, falling back to `WRITE_DISCARD` only on
wrap / first append, and returns `BaseVertexLocation` / `StartIndexLocation` for the
draw. Buffers grown 128 KB/32 KB → **16 MB/4 MB** (several frames of geometry). All
draw paths route through it: `td5_plat_render_draw_tris` (race hot path),
`td5_plat_render_draw_lines`, and the DDraw vtable `Dev3_DrawPrimitive` /
`Dev3_DrawIndexedPrimitive`. Draws now use `Draw(.., StartVertex=base)` /
`DrawIndexed(.., StartIndex=start, BaseVertex=base)`. Oversized single batches are
safely skipped (the old per-draw `WRITE_DISCARD` path would have memcpy-overflowed
the 128 KB buffer).

VSync-off sweep, **full 16-car field (1 human + 15 AI)**, panes 1→9, track held:

| Panes | AI screens | Frame avg (ms) | FPS avg | FPS min | Render avg (ms) |
|------:|-----------:|---------------:|--------:|--------:|----------------:|
| 1 | 0 | 1.8 | 582 | 411 | 2.4 |
| 2 | 1 | 3.8 | 273 | 226 | 3.5 |
| 3 | 2 | 4.4 | 237 | 124 | 3.9 |
| 4 | 3 | 6.1 | 173 | 140 | 5.0 |
| 5 | 4 | 7.2 | 144 | 113 | 5.8 |
| 6 | 5 | 8.3 | 127 | 91 | 6.1 |
| 7 | 6 | 10.5 | 102 | 73 | 7.5 |
| 8 | 7 | 11.8 | 90 | 63 | 8.3 |
| 9 | 8 | 12.6 | 82 | 69 | 9.0 |

Render now scales **cleanly linearly** at ~0.8 ms/pane through 9 panes — the cliff is
gone. Before vs after at the prior 6-car field: 6 panes render 79.9 → **7.4 ms**
(~11×), frame 96.9 → **6.6 ms** (~15×), FPS 29 → **156**; 5 panes 63.6 → **6.0 ms**,
FPS 32 → **173**. No crash at 9 panes / 16 cars.

**VSync-ON confirmation** (real-world pacing, 180 Hz cap): 1 pane 179 fps / worst
13 ms; 4 panes 167 fps / 16.8 ms; 6 panes 154 fps / 21 ms — smooth, no stutter cliff.
(Under VSync the `render` *zone* reads ~120 ms because the CPU blocks on the full
swapchain queue and that wait is attributed to the zone — which is exactly why render
cost must be measured VSync-off.)

Remaining levers (#2 texture-page batching, #3 faithful per-pane split view
reduction) are now optional polish — the per-draw stall that dominated is resolved.

## UPDATE 2 — per-pass probe (pushing 9 panes toward 180 fps)

Added per-viewport sub-zone marks (`v_setup`/`v_track`/`v_actors`/`v_vfx`/`v_flush`/
`v_hud`; `v_track` = sky dome + track display-list walk, split out inside
`td5_render_actors_for_view`). At 9 panes / 16 cars (frame ~12.6 ms, 82 fps) the
breakdown overturned the intuition:

- `v_track` ≈ **0.58 ms/pane** (×9 ≈ 5.2 ms) — the #1 reliable render cost.
- `v_actors` ≈ 0.2 ms/pane; `v_vfx` / `v_flush` / `v_hud` ≈ **0** (so "skip VFX on
  spectator panes" buys nothing).
- 9 cars vs 16 cars → **same frame time** (14.2 vs 13.4 ms) ⇒ `post_physics` (~12 ms/
  sub-tick) is the known profiler artifact; the 16-car sim is **not** the bottleneck.

So after the streaming fix the per-pane render is dominated by the **track geometry
walk**, not actors/VFX/physics. The faithful lever is therefore reducing the per-pane
track window.

**Implemented — spectator-pane draw distance.** In `td5_render_actors_for_view`, panes
with `view_index >= num_human_players` (the AI spectator tiles) get
`view_dist_frac *= 0.5` — half the span window + actor cull. The player's pane(s) keep
full distance, so single / 2-player split is byte-unchanged. Result at 9 panes / 16
cars: `v_track` 0.58 → **0.33 ms/pane**, frame 12.6 → **10.1 ms**, fps 82 → **99**
(+21%) from one low-risk change.

**Reaching a sustained 180 fps at 9 panes is not realistic without aggressive
secondary-pane simplification.** Remaining per-pane fixed costs (track walk ~0.33 ms,
actor draws, per-view camera solve) sum to ~1 ms/pane; 9 × (180 fps budget 0.62 ms/pane)
leaves no room. Further gains would need: a deeper spectator-pane LOD (e.g. ×0.33 window
+ skip the per-view chase-camera terrain probes/springs, snapping spectators to a static
follow), and/or smoothing the bimodal sim sub-tick hitch (frame_max ~28 ms on the ~40 %
of frames that run a 30 Hz tick). Multithreaded pane submission (deferred contexts) is
the only true scaling lever but is a large, risky wrapper change.

## Reusable harness facts

- `frame=avg/max fps=N` and `render=avg/max` (PROFILE, tag `plat`, engine.log) are the
  trustworthy per-FRAME zones; `post_physics`/`post_ai` are **per-sub-tick** and read
  inflated — do NOT treat them as frame cost.
- `RENDERSTAT` (tag `render`) gives whole-frame draws/tris/texmiss; `draws ÷ render-ms`
  separates a submission-count problem from a per-draw stall.
- AutoRace bypasses the menu, so dev knobs (`SpectateScreens`, `DefaultOpponents`) must be
  under `[Game]` — the INI reader is section-scoped; a key appended at file end (wrong
  section) silently reads its default.
- PlayerIsAI=1 keeps the 30 Hz sim running while the window is unfocused (no focus-pause),
  so the render path can be profiled headlessly; graceful `CloseMainWindow` flushes the
  log buffer (Kill loses the tail).
