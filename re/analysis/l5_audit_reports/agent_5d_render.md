# Phase 5(d) — td5_render.c + td5_render.h L4→L5 audit report

Date: 2026-05-21
Scope: `td5mod/src/td5re/td5_render.c` (17 entries) + `td5mod/src/td5re/td5_render.h` (4 entries) = 21 total.
Method: Ghidra pool slot 12, decomp-per-function, comment-only additions, no executable code changes.

## Totals

- In scope: 21
- Promoted L5 (CONFIRMED — byte-faithful): 4
- Promoted L5 (ARCH-DIVERGENCE — class manifest + per-site): 11
- Skipped (left at L4 honestly): 6
- Suspected regressions: 0 new (one pre-existing UNCERTAIN reconfirmed)

---

## Promoted L5 (CONFIRMED — byte-faithful)

| Addr | Name | Port site | Notes |
|------|------|-----------|-------|
| 0x0040AE80 | InitializeRaceRenderState | `td5_render_init` (td5_render.c:~987) | One-shot gate + 3 sub-inits folded into inline reset; sentinel collapses to s_globals_initialized |
| 0x0040BAA0 | QueryRaceTextureCapacity | header-only ARCH (D3D11 wrapper reports caps at init) | Manifest-only |
| 0x0042E9C0 | LoadGlobalOrientationToRenderState | folded into callers of `td5_render_load_rotation` | 1-call wrapper around 9-float copy; same `LoadRenderRotationMatrix(&g_raceRotationMatrix)` semantics |
| 0x00431270 | RenderTrackSpanDisplayList | `td5_render_span_display_list` (td5_render.c:~1522) | Core loop matches orig 1:1; port-side defensive NaN/Inf guards + ptr-in-blob validation only reject would-crash records. Has pre-existing `[CONFIRMED @ 0x42dcad]` and `[CONFIRMED @ 0x00431296]` inline. |

Inline citations added at td5_render_init and td5_render_load_rotation.

---

## Promoted L5 (ARCH-DIVERGENCE — class manifests at file footer + per-site comments)

Two class manifests added at the td5_render.c footer:

### Class: D3D3 -> D3D11 rasterizer pipeline

Original used IDirect3DDevice3::DrawIndexedPrimitive (FVF 0x1C4) on a DDraw-bound device, with Sutherland-Hodgman screen-axis clipping split across three separate functions (X clipper, Y clipper, fan emitter). Port collapses these into a single `clip_and_submit_polygon()` + `td5_plat_render_draw_tris()` (D3D11 immediate command list). D3D11 handles screen-edge clipping internally so the X/Y stages are deliberately absent.

| Addr | Name | Port site |
|------|------|-----------|
| 0x004317F0 | ClipAndSubmitProjectedPolygon (3030B) | `clip_and_submit_polygon` (td5_render.c:~664) |
| 0x004323D0 | RenderTrackSegmentBatch (X clipper) | folded into D3D11 viewport |
| 0x004326D0 | RenderTrackSegmentBatchVariant (Y clipper) | folded into D3D11 viewport |
| 0x00432AB0 | AppendClippedPolygonTriangleFan | inline tail of clip_and_submit_polygon |
| 0x004329E0 | FlushImmediateDrawPrimitiveBatch | `flush_immediate_internal` (td5_render.c:~549) |
| 0x00431340 | FlushQueuedTranslucentPrimitives | `td5_render_flush_translucent` (td5_render.c:~2483) |
| 0x00431750 | EmitTranslucentTriangleStrip | `dispatch_tristrip` (td5_render.c:~804) |
| 0x0043DCB0 | TransformAndQueueTranslucentMesh | `td5_render_transform_mesh_vertices` + `td5_render_prepared_mesh` |

Inline ARCH-DIVERGENCE citations added at clip_and_submit_polygon, flush_immediate_internal, td5_render_flush_translucent, dispatch_tristrip, td5_render_prepared_mesh.

### Class: D3D3 sprite-template scratch -> D3D11 vertex stream

Original used `BuildSpriteQuadTemplate` (0x00432BD0) + `WriteTransformedShortVector` to pre-bake quad templates into static scratch buffers, submitted via `QueueTranslucentPrimitiveBatch` for D3D3 batching. Port re-emits geometry per-frame as raw TD5_D3DVertex through `td5_plat_render_draw_tris`. No template scratch in port.

| Addr | Name | Port site |
|------|------|-----------|
| 0x00446A70 | InitializeVehicleWheelSpriteTemplates (1071B) | `wheel_lookup_static_hed` + `render_vehicle_wheel_billboards` (per-frame) |
| 0x0040C7E0 | BuildSpecialActorOverlayQuads (1000B) | `render_vehicle_shadow_quad` (td5_render.c:~3946) |
| 0x004011C0 | RenderVehicleTaillightQuads | `render_vehicle_brake_lights` (td5_render.c:~4106) (+ vfx orchestrator) |

Inline ARCH-DIVERGENCE citations added at all three port sites.

### Other ARCH per-site

| Addr | Name | Port site | Notes |
|------|------|-----------|-------|
| 0x0043E210 | SetProjectionEffectState | `td5_render_update_projection_effect` (td5_render.c:~3615) | Raw-byte slot array (0x20-stride) -> typed ProjectionEffectState struct |

---

## Skipped (left at L4 — honest deferral)

| Addr | Name | Reason |
|------|------|--------|
| 0x0040CBD0 | ConfigureActorProjectionEffect | Has [UNCERTAIN] inline comment about which 3-float vector orig passes to mode-1 SetProjectionEffectState; port picks linear_velocity for the forward-scroll semantic but orig binary disasm is ambiguous at this call site. Not byte-faithful by construction. |
| 0x00401330 | SpawnRearWheelSmokeEffects | Out-of-scope: body lives in td5_vfx.c (`td5_vfx_spawn_rear_wheel_smoke`); only call-site dispatch in render.c at line ~2214. |
| 0x00429CF0 | SpawnVehicleSmokeSprite | Out-of-scope: body lives in td5_vfx.c (`td5_vfx_spawn_smoke`); call-site only in render.c. |
| 0x004092D0 | RenderVehicleActorModel | Out-of-scope: body lives in td5_physics.c (wheel-probe transforms — `TransformShortVec3ByRenderMatrixRounded` chain). Render.c only references the orig address in audit-header comments. Physics audit owns this. |
| 0x0042E4F0 | WritePointToCurrentRenderTransform | Citation-sweep header entry; no dedicated port body. Single-point matrix*vec3+translation operation is folded into `mat3x3_transform_vec3` / `td5_render_transform_mesh_vertices` across many call sites. Too diffused to point to a single port site for byte-faithful confirmation. |
| 0x0042E750 | BuildWorldToViewMatrix | Citation-sweep header entry; no dedicated port body in td5_render.c. The pitch/yaw + forward-vector to 3x3 builder is replaced by per-frame camera-basis composition in td5_camera.c. Render-side audit cannot byte-verify. |

These six are not regressions — they are out-of-scope citations or honest "no clean port site to verify" cases. None should ever promote without owning-module audit.

---

## Suspected regressions

None new in this pass.

One pre-existing UNCERTAINty reconfirmed but not new: the mode-1 param_3 vector in ConfigureActorProjectionEffect's call to SetProjectionEffectState (port chose linear_velocity for the forward-scroll semantic; orig is binary-ambiguous at the call site). Documented inline at td5_render.c:~3685.

The clip_and_submit_polygon backface-cull skip-by-zero-area-only is intentional (D3D11 CullMode=NONE) and already documented inline at td5_render.c:~748.

The deferred additive batch path in flush_immediate_internal (type-3 page deferred until after opaque pass) is a port-side enhancement not present in orig — invariant: same draw order eventually, just relocated. Documented in the new flush_immediate_internal ARCH comment.

---

## Files touched

- `td5mod/src/td5re/td5_render.c` — comment additions only (no executable change):
  - 3 new Phase 5(d) class manifests appended at file footer (after Phase 5(a) manifests)
  - 9 inline ARCH-DIVERGENCE / CONFIRMED citations placed near orig-address citations at: `td5_render_init`, `td5_render_load_rotation`, `clip_and_submit_polygon`, `flush_immediate_internal`, `td5_render_flush_translucent`, `dispatch_tristrip`, `td5_render_prepared_mesh`, `td5_render_update_projection_effect`, `wheel_lookup_static_hed`, `render_vehicle_shadow_quad`, `render_vehicle_brake_lights`

td5_render.h was not modified — its 4 entries are all orig-address declarations (`InitializeRaceRenderState` 0x40AE80 was already cited in the .c file footer; the X/Y/fan-emit trio and `QueryRaceTextureCapacity` are folded class-level under the Phase 5(d) manifests in the .c file). The header itself is mostly a Ghidra-name→port-name mapping comment block; promotion comments belong in the .c file with the actual port impls.

## Audit budget

~50 minutes spent total. Decomped 12 orig functions, cross-read ~15 port sites, added 12 commented citations (3 class manifests + 9 inline).
