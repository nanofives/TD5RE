# Draw-call batching plan — merge consecutive same-state quads

Status: **SCOPED, NOT STARTED** (2026-07-28)

## Why

Crash-ring analysis of `log/crash.log` (an `nvwgf2um.dll` fault during `Present` in
`selftest step 25:race-moscow-base`) showed the draw stream immediately before the fault:

```
[  0..251]  IDX prim=4 v=4 i=6  srv=2fde9e8c   <- 252 identical single-quad draws
[252..253]  IDX prim=4 v=4 i=6  srv=2fc3a10c
[254]       IDX prim=4 v=8 i=12 srv=2fc3a10c
[255]       PRESENT #3270
```

252 consecutive draw calls, each submitting one quad (2 triangles), all sharing one texture.
Run total was `total_draws=1277231`.

**This is NOT a crash fix.** The `nvwgf2um.dll` fault is pre-existing (identical instruction,
stack, scenario and null-read in `log/crash.baseline_prev.log` from 2026-07-27, before any
Stage 0/1 work) and its causal link to draw-call volume is **unproven**. High draw-call counts
stress the driver; that is not evidence they cause this specific fault. Do this work for CPU
frame time, not to fix the crash.

## Verified findings (read directly, not inferred)

- **Geometry upload is already optimised.** `td5_plat_render_draw_tris`
  (`td5mod/src/td5re/td5_platform_win32.c:3393`) streams into a dynamic ring with
  `Backend_StreamUpload` using `WRITE_NO_OVERWRITE`, discarding only on wrap. A 2026-06-08
  change specifically removed the per-draw `WRITE_DISCARD` that serialised the CPU.
  **"Batch the vertex data" is largely already done.**
- **The render state cache already early-outs.** `Backend_ApplyStateCache`
  (`td5mod/ddraw_wrapper/src/d3d11_backend_pipeline.c:169`) begins with `if (!s->dirty) return;`
  and each sub-state (blend / depth-stencil / rasterizer) also compares a cached
  `current_*_idx` before calling its D3D setter. For a run of same-state quads it costs one
  boolean test after the first draw. **Eliminating redundant state application is NOT the win.**
- **What IS redundant per draw** — 7 unconditional binds before every `DrawIndexed`:
  `IASetVertexBuffers`, `IASetInputLayout`, `VSSetShader`, `VSSetConstantBuffers`,
  `PSSetConstantBuffers`, `IASetIndexBuffer`, `IASetPrimitiveTopology`. Individually cheap
  (user-mode runtime bookkeeping, partly redundancy-filtered by D3D11 itself).
- **The expensive item is `DrawIndexed` itself** — 252 where 1 would do. Draw calls carry real
  driver-side command-buffer cost; redundant setters do not, to anything like the same degree.
- **~22 call sites** submit exactly `(4 verts, 6 indices)`, spread over `td5_render_effects.c`,
  `td5_hud.c`, `td5_render_pipeline.c`, `td5_frontend.c`, `td5_fmv.c`, `td5_game.c`.
- **The wheel renderer is NOT a culprit** — `render_vehicle_wheels_unified`
  (`td5_render_effects.c:2587-2880`) contains none of the per-quad draw sites despite
  `WHEEL_SEG_HI = 24` facets per tire across racers and traffic; it already submits batched
  geometry.
- **Vehicle lights do not scale with traffic** — `render_vehicle_brake_lights` /
  `render_vehicle_headlights` are gated `if (is_racer)` at `td5_render_mesh.c:2895-2899`, so at
  most the racer count feeds them, not the 64-slot traffic pool.
- **Which emitter produced the 252 is still UNKNOWN.** Ruling out wheels and traffic-scaled
  lights narrows it to the remaining per-quad sites (`tracked_marker_emit_quad_world` cop
  strobes, HUD sprite paths, sky), but distinguishing them needs a live instrumented run.

## The change

**One change, in the platform layer.** All ~22 call sites stay untouched — that keeps the diff
small and avoids 22 opportunities to introduce a visual regression.

In `td5_plat_render_draw_tris`: do not issue `DrawIndexed` immediately. While the draw state is
unchanged, keep appending geometry to the ring and extend the pending index range; emit a single
`DrawIndexed` on flush. Order is preserved *within* a merged run, so alpha blending stays
correct — **this is not a reordering optimisation.**

Redundant-bind elimination falls out for free: the 7 binds are issued once per merged batch
rather than once per quad. Do **not** implement it as a separate pass; the merge logic makes
standalone bind-tracking redundant.

### Flush points (all mandatory)

- any render-state-cache change (i.e. whenever `s->dirty` becomes set)
- texture / page bind
- viewport or clip-rect change
- `td5_plat_render_draw_lines`, `td5_plat_render_draw_tris_flat`
- end of frame / before `Present`
- **ring wrap** — the wrap performs `WRITE_DISCARD`, which invalidates geometry already appended
  but not yet drawn. Missing this flush silently corrupts previously-batched draws. This is the
  one that will bite.

### Constraints

- **Batch state must be per-context, not global.** `WrapperRecCtx *rc = g_wrapper_rec` selects a
  per-pane bundle (`rc->dc/vb/ib/state`); a single global batch would cross split-screen panes.
- **`td5_rcmd` recording is unaffected.** `td5_plat_render_draw_tris:3399` returns early when
  `td5_rcmd_recording()`, so worker-thread recording still records individual draws; batching
  applies at replay on the main thread.

## Measurement

Instrumentation already exists in `td5_plat_render_draw_tris`: `s_frame_draw_calls`,
`s_frame_vertices`, `s_frame_indices`. Before/after draw-call counts are free.

## Acceptance criteria

1. **Correctness: render goldens pixel-identical.** A correct merge changes no pixels, so
   `rgold-*` rows in the selftest are exactly the right net. Trace goldens are unaffected
   (rendering does not feed the sim) and must also stay green.
2. **Benefit: measured reduction in `s_frame_draw_calls`** plus a frame-time delta, checked
   especially in split-screen where per-frame draw counts are highest (`race-spectate3`).
3. No new warnings; `lint_structure` at baseline.

## Blocked on

**Verification requires a working GPU.** As of 2026-07-28 the full selftest crashes reproducibly
in `nvwgf2um.dll` at `race-moscow-base` (twice consecutively), so the render-golden gate cannot be
cleared. Deferred-draw batching fails in subtle *visual* ways — a missed flush point renders
geometry with the wrong texture or blend — which inspection does not catch and pixel goldens do.
**Do not land this change without clearing acceptance criterion 1.**

## Sequencing

Implement on its own branch off `master`, isolated from the x64 Stage 0/1 work
(see the x64 staging plan) so a visual regression is attributable to one change.
