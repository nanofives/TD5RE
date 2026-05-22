# Agent B — Render Module L4→L5 Audit (2026-05-21)

## Scope
- Files: `td5mod/src/td5re/td5_render.c` (5613 lines) and `td5_render.h` (239 lines, pure header)
- Work lists: `re/analysis/l5_audit_manifests/td5_render.c.csv` (52) + `td5_render.h.csv` (8)
- Total in scope: 60 functions

## Results summary

| Bucket | Count |
|--------|-------|
| Promoted L5 (CONFIRMED byte-faithful) | 3 |
| Promoted L5 (ARCH-DIVERGENCE) | 10 |
| Skipped (left at L4) | 43 |
| Suspected regressions flagged | 4 |

## Promoted L5 — Byte-faithful (3)

| Address | Orig name | Port impl | Rationale |
|---------|-----------|-----------|-----------|
| 0x0040BA10 | AdvanceTexturePageUsageAges | `td5_render_advance_texture_ages` | Same LRU sweep (used→reset+clear / unused→inc with 0xFF saturate), struct-array vs raw-byte pool but identical semantics and loop order. |
| 0x0042DCA0 | IsBoundingSphereVisibleInCurrentFrustum | `td5_render_is_sphere_visible` | 5-plane sphere-vs-frustum test. Matches FPU ordering, plane normal layout, near/far/horiz/vert test polarities. Port adds debug counters (no behavioral effect). |
| 0x0043DC50 / 0x0042E370 | TransformVec3ByRenderMatrixFull / TransformVector3ByRenderRotation | `mat3x3_transform_dir` | Row-major 3x3 direction-vector multiply using m[0..8]. Both orig helpers are identical 3x3 mults; port consolidates into one shared inline helper. |

## Promoted L5 — ARCH-DIVERGENCE (10)

All driven by the project's documented D3D3→D3D11 backend swap, with secondary helper-collapse/struct-vs-raw-byte reshuffles:

| Address | Orig name | Port impl | Divergence |
|---------|-----------|-----------|-----------|
| 0x0040ADE0 | BeginRaceScene | `td5_render_begin_scene` | D3D11 platform abstraction + texture-page invalidation via -1 sentinel (orig: 0xffffffff handle); adds per-frame debug counter resets. |
| 0x0040AE00 | EndRaceScene | `td5_render_end_scene` | D3D11 platform abstraction; same EndScene+AdvanceAges sequence + debug logging. |
| 0x0040AEC0 | ReleaseRaceRenderResources | `td5_render_shutdown` | D3D11 abstracted texture teardown; orig's DAT_0048dba0 gate absorbed into s_state_active/s_globals_initialized. |
| 0x0040AF10 | ConfigureRaceFogColorAndMode | `td5_render_configure_fog` | Same `color & 0xFFFFFF` mask; orig's DXD3D::CanFog() probe removed because D3D11 universally supports fog. |
| 0x0040AF50 | ApplyRaceFogRenderState | `td5_render_set_fog` | Orig 6 IDirect3DDevice::SetRenderState calls (FOGENABLE, color, start, end, density, commit) → single `td5_plat_render_set_fog` call (D3D11 constant-buffer state). |
| 0x0040BA60 | ResetTexturePageCacheState | `td5_render_reset_texture_cache` | Struct-array vs raw-byte pool; same per-slot reset semantics; port adds explicit current/previous-page invalidation (orig does it in BeginRaceScene). |
| 0x004312E0 | InitializeTranslucentPrimitivePipeline | `td5_render_init_translucent_pipeline` | Color-LUT init split into `td5_render_init` (same `0x10101` step formula); 512-entry pool layout via typed struct array instead of raw 8-byte records. |
| 0x00431690 | SubmitProjectedQuadPrimitive | `dispatch_projected_quad` | Globals (DAT_004af268/278/27c + material handle) → parameter list `(verts, 4, tex_page)`; same vertex source and texture-page semantics. |
| 0x004316F0 | SubmitProjectedTrianglePrimitive | `dispatch_projected_tri` | Same as above with vert count = 3 instead of 4. |
| 0x0043DAF0 / 0x0043DB70 | Push/PopRenderTransform | `td5_render_push_transform` / `_pop_transform` | Orig single backup slot at _DAT_004c36c8..f4 → port depth-N stack to support nesting (billboard sub-passes etc.). Depth-1 behavior identical. |

## Skipped — left at L4 (43)

### Out of render.c scope (impl in another file) (3)
- 0x004011C0 RenderVehicleTaillightQuads — impl in `td5_vfx.c` (initializes per-actor taillight quads)
- 0x00401330 SpawnRearWheelSmokeEffects — impl in `td5_vfx.c`
- 0x004092D0 RenderVehicleActorModel — actually a physics/wheel position update; impl spread across `td5_physics.c`/`td5_track.c`

### Heavily refactored / multi-orig-fn consolidations (3)
- 0x0040AE80 InitializeRaceRenderState — folded into `td5_render_init` together with multiple other init paths; not 1:1 auditable
- 0x0043E2C0 ResetProjectedPrimitiveWorkBuffer — folded into the depth-bucket reset inside `td5_render_flush_projected_buckets` and `td5_render_init`
- 0x0043E5F0 InitializeProjectedPrimitiveBuckets — folded into `td5_render_init` (heap-alloc replaced by static arrays)

### Density-match only (CITATION-SWEEP placeholder, no clean port impl) (15)
These were flagged in the existing 2026-05-21 CITATION-SWEEP block at td5_render.c:5586 as "density-match, verify in Phase 4" — but a clean per-function port body does not exist in render.c. They are referenced only as call-sites or in audit prose:
- 0x0040CBD0 ConfigureActorProjectionEffect
- 0x0040CD10 UpdateActorTrackLightState
- 0x0040CDC0 / 0x0040D190 CrossFade16BitSurfaces
- 0x0040D120 AdvanceCrossFadeTransition
- 0x00429CF0 SpawnVehicleSmokeSprite
- 0x0042D880 ApplyMeshRenderBasisFromTransform
- 0x0042E3D0 TransformShortVectorToView
- 0x0042E4F0 WritePointToCurrentRenderTransform
- 0x0042E560 TransformTriangleByRenderMatrix
- 0x0042E750 BuildWorldToViewMatrix
- 0x0042E9C0 LoadGlobalOrientationToRenderState
- 0x0042FE20 BlendTrackLightEntryFromStart (cited as L5 confirmed in adjacent comment block at line 2946; not promoted here per scope guidance)
- 0x0042FFC0 BlendTrackLightEntryFromEnd (same)
- 0x00430CF0 HeapAllocTracked — heap helper, not in render.c

### Large complex multi-step functions (rasterization core / model build) — fidelity-critical, conservative skip (14)
- 0x00431270 RenderTrackSpanDisplayList — port `td5_render_span_display_list` is heavily extended with defensive validation, NaN checks, billboard-yaw rotation path, and debug counters; semantic core (sphere-cull + queue-mesh) preserved but body ~3-4× orig size, not a safe verbatim audit
- 0x00431340 FlushQueuedTranslucentPrimitives — port `td5_render_flush_translucent` walks a sorted linked list whereas orig walks a pool drain with dispatch-table inline; semantically similar but byte-faithful claim is not defensible
- 0x00431750 EmitTranslucentTriangleStrip — folded into `dispatch_tristrip` (no standalone audit target)
- 0x004317C0 SubmitProjectedPolygon — small dispatcher folded into clip path
- 0x004317F0 ClipAndSubmitProjectedPolygon — 3030-byte rasterizer; port reimplements with Sutherland-Hodgman near-plane clip + projection + screen clip. True ARCH-DIVERGENCE (D3D3 inline pipeline → D3D11 software-clipped immediate vertex array) but the algorithm changes are extensive enough that line-for-line audit is unsafe.
- 0x004329E0 FlushImmediateDrawPrimitiveBatch — D3D11 backend submission; complex
- 0x00446A70 InitializeVehicleWheelSpriteTemplates — wheel-billboard atlas init; already documented inline with CONFIRMED citations but body is large
- 0x0043DCB0 TransformAndQueueTranslucentMesh — folded into `td5_render_span_display_list` core
- 0x0043DEC0 ApplyMeshProjectionEffect — water/envmap UV; large
- 0x0043E210 SetProjectionEffectState — projection state config
- 0x0043E2F0 FlushProjectedPrimitiveBuckets — port has it but algorithm differs significantly (different flag encoding, all routed via clip_and_submit_polygon)
- 0x0043E3B0 InsertBillboardIntoDepthSortBuckets — folded into `dispatch_billboard`
- 0x0043E550 QueueProjectedPrimitiveBucketEntry — port has `td5_render_queue_projected_entry` but the bucket layout differs (struct-pool with explicit next pointers vs raw heap-allocated 64KB scratch)
- 0x0040BA10 → already promoted CONFIRMED above

### td5_render.h manifest (8) — all density/weak matches with implementations in render.c
- 0x0040AF10 / 0x0040AF50 — already promoted ARCH-DIVERGENCE above
- 0x0040B830 AdvanceTextureStreamingScheduler — port has stub `td5_render_advance_texture_ages` for the LRU half; the full streaming scheduler is not ported as a 1:1 function (texture streaming model differs in D3D11 backend)
- 0x0040BAA0 QueryRaceTextureCapacity — port doesn't have a 1:1 counterpart
- 0x0040C7E0 BuildSpecialActorOverlayQuads — large; not cleanly ported in render.c (lives elsewhere)
- 0x004323D0 / 0x004326D0 RenderTrackSegmentBatch / Variant — X/Y-axis clipper variants; folded into clip_and_submit_polygon
- 0x00432AB0 AppendClippedPolygonTriangleFan — small helper folded into the same fan-decomposition tail in clip_and_submit_polygon

## Suspected regressions (4)

These are port-side divergences that I did NOT promote because they appear to break the orig semantics rather than re-encode it cleanly. Flagging here rather than leaving silent at L4.

### 1. `dispatch_quad_direct` / `dispatch_tristrip_direct` collapsed bucket→immediate path
- **Functions**: 0x004316D0 EmitTranslucentQuadDirect, 0x00431730 EmitTranslucentTriangleStripDirect
- **Port location**: td5_render.c:907 `dispatch_quad_direct`, td5_render.c:896 `dispatch_tristrip_direct`
- **Orig behavior**: 0x4316D0 calls `QueueProjectedPrimitiveBucketEntry(param_1)` (depth-sort bucket insert). 0x431730 calls `InsertTriangleIntoDepthSortBuckets(param_1)`.
- **Port behavior**: Both call `clip_and_submit_polygon` (immediate raster) — skipping the depth-sort buckets entirely.
- **Risk**: For translucent geometry that the orig wanted depth-sorted (overlays / FX over world), the port emits immediate, which means draw order = command order, not depth order. May cause z-order glitches in HUD overlays, lens flares, etc.
- **Recommendation**: Either route through `td5_render_queue_projected_entry` for both paths, or audit the actual callers and verify the depth-sort was intentional in orig vs unused.

### 2. `td5_render_advance_billboard_anims` collapsed per-billboard pool → single global counter
- **Function**: 0x0043CDC0 AdvanceWorldBillboardAnimations
- **Port location**: td5_render.c:4607
- **Orig behavior**: Walks the world-billboard pool from `g_trackedActorMarkerBillboardPool_PROVISIONAL` to 0x4bf218 in stride `0x8b * 4` (= 0x22c bytes per entry), incrementing each entry's first int by `+0x10` (per-billboard phase).
- **Port behavior**: A single `s_billboard_anim_phase += 0x20;` global increment.
- **Risks**:
  - Step size is 0x20, not 0x10. Comment in the port says "+0x10" but code is `+= 0x20`.
  - Per-billboard phases collapsed into one global, so all animated billboards now advance in lock-step rather than each having its own phase.
- **Recommendation**: Restore per-pool-entry phase advance, fix step to 0x10.

### 3. `td5_render_load_translation` semantics inversion vs orig
- **Function**: 0x0043DC20 LoadRenderTranslation
- **Port location**: td5_render.c:1127
- **Orig behavior**: 3-float copy from `param_1+0x24..0x2c` to `g_currentRenderTransform+0x24..0x2c` (i.e. caller has already computed the view-space translation row and just hands it over).
- **Port behavior**: Treats the param as a WORLD-space position, subtracts `s_camera_pos`, then applies camera_basis to compute view-space translation inline.
- **Risk**: The port has absorbed the camera-relative transform step. This may be correct IF every caller now passes world positions (which appears to be the case in render.c:1471 where the call site does `origin = mesh_origin / 256.0f`), but the orig's caller pattern (pass a pre-baked actor frame) is gone. Behavior is semantically different from the orig API contract; potential subtle bugs in any external/legacy caller.
- **Recommendation**: Audit all callers in the port to confirm none pass pre-baked actor-frame translations.

### 4. `td5_render_test_mesh_frustum` adds `bounding_center + origin` (potential double-count)
- **Function**: 0x0042DE10 TestMeshAgainstViewFrustum
- **Port location**: td5_render.c:1327
- **Orig behavior**: Reads ONLY the resource's origin field at +0x1c..+0x24 (fp24.8 scaled by `_g_fixedPointToFloatScale`); does NOT add bounding_center.
- **Port behavior**: Reads `mesh->bounding_center_x + mesh->origin_x` for all 3 axes — adding both fields.
- **Risk**: If `bounding_center_*` is stored in world space (as confirmed for the 0x42DCA0 path per existing comment at td5_render.c:1437), then adding `origin_x` produces an offset by `+origin`, mis-positioning the cull sphere. Could cause false positives (e.g. distant meshes failing to cull) or false negatives (close meshes incorrectly culled).
- **Recommendation**: Verify the relationship between `bounding_center` and `origin` in `TD5_MeshHeader`. If both are world-space duplicates, the addition is wrong; if `bounding_center` is local to `origin`, the addition is correct and matches orig's combined world position. The orig only references `origin` so the safer port would drop the `bounding_center` add.

## Process notes

- Worked through addresses in low→high order; budget exceeded a couple of times on the large 3030-byte ClipAndSubmitProjectedPolygon and the 1071-byte InitializeVehicleWheelSpriteTemplates so I deliberately skipped the deep dive there rather than risk false promotion.
- Pool slot 0 is permanently locked (per memory note `stale-handles-from-pre-consolidation-needs-reboot 2026-05-20`); `ghidra_pool.sh acquire` still returns pool0 in some states. Worked around by trying pool12.
- The render module has an unusually rich set of pre-existing CONFIRMED audit citations scattered through comments (56 strong-keyword hits before this sweep). The L4 backlog is largely "density-match" auto-detections that have no clean port body to audit.
- All edits are pure comment additions; no executable code touched.

## Files modified
- `td5mod/src/td5re/td5_render.c` — 13 new audit header blocks inserted (pure comments)
