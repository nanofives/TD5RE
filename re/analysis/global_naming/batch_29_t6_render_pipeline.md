---
batch: 29
area: render_pipeline
tier: T6
target_todos: []
ghidra_session: TD5_pool3
analyzed_addresses: 0x004316f0, 0x00431500, 0x0043e6a0, 0x0043dd00, 0x00432170, 0x00432300, 0x0040ae20, 0x0040b200
agent: tier1-a-t6
date: 2026-05-22
---

# Globals enumeration — render/clipper/translucent pipeline (T6)

## Summary

- Functions analyzed: ClipAndSubmitProjectedPolygon, RenderTrackSegmentBatch[Variant], SubmitProjectedPolygon/Triangle/Quad, SetProjectedClipRect, SetClipBounds, EmitTranslucentQuad/Triangle, AppendClippedPolygonTriangleFan, FlushImmediateDrawPrimitiveBatch, RenderPreparedMeshResource, FlushQueuedTranslucentPrimitives, SubmitImmediateTranslucentPrimitive, BuildSpriteQuadTemplate callers, InitializeTranslucentPrimitivePipeline, InitializeRaceRenderGlobals, BuildTrackTextureCacheImpl
- Unnamed DAT_* targeted: 30+
- Already-named neighbors noted: g_currentDrawCallVertexBuffer/IndexBuffer/VertexCount/IndexCount, g_currentRenderTransform
- Proposals — high: 20
- Proposals — medium: 7
- Proposals — comment-only: 3

## Methodology

Walked clipper/submit primitive functions backward from known render entry points. Identified W1-E cluster C.6 (clipper draw-call state 0x004afb14..0x004afb50) and adjacent geometry batcher state (0x004af260..0x004af2a0). Used FLD/FSTP vs MOV/CMP instruction patterns to discriminate float vs int. Camera basis matrix (0x004ab040..0x004ab07c) identified as 3x3 floats via `0x3f800000` (1.0f) init pattern at orthonormal diagonal entries.

## Proposals (globals)

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x004af280 | u32 | `g_clipPolyOutVertexCount` | high | ClipAndSubmitProjectedPolygon writer (37 refs); compared/incremented as polygon out-vertex counter | `(none)` |
| 0x004af268 | u32 | `g_currentPrimitiveTopology` | high | SubmitProjectedQuad/Triangle/Polygon writer; ClipAndSubmit reader (10 refs) — primitive topology enum | `(none)` |
| 0x004af278 | u32 | `g_currentPrimitiveVertexStride` | high | SubmitProjectedQuad writes `0x1` (=4 verts/quad/triangle stride code); ClipAndSubmit reader (8 refs) | `(none)` |
| 0x004af27c | u32 | `g_currentPrimitiveIndexStride` | high | SubmitProjectedQuad writes `0x4`; ClipAndSubmit reader (7 refs) — index stride per primitive | `(none)` |
| 0x004af288 | u32 | `g_clipPolyScratchVertexCursor` | high | ClipAndSubmitProjectedPolygon writes/reads (7 refs); polygon out-vertex cursor | `(none)` |
| 0x004af28c | u32 | `g_clipPolyScratchVertexBase` | high | ClipAndSubmitProjectedPolygon writes/reads (9 refs) — scratch base ptr | `(none)` |
| 0x004af290 | u32* | `g_clipPolyScratchPtr` | medium | ClipAndSubmit uses LEA EAX [0x4af290] then dereferences as pointer (5 refs) | `(none)` |
| 0x004afb20 | float | `g_clipRectMinX` | high | SetProjectedClipRect writer; ClipAndSubmit FLD/FSUB (9 refs) | `(none)` |
| 0x004afb24 | float | `g_clipRectMaxX` | high | SetProjectedClipRect writer; ClipAndSubmit FLD/FSUB (9 refs) | `(none)` |
| 0x004afb28 | float | `g_clipRectMinY` | high | SetProjectedClipRect FSTP (9 refs) | `(none)` |
| 0x004afb2c | float | `g_clipRectMaxY` | high | SetProjectedClipRect FST (9 refs) | `(none)` |
| 0x004afb38 | float | `g_clipBoundsMinX` | high | SetClipBounds + SetProjectedClipRect FCOMP (5 refs) — pre-projection clip bound | `(none)` |
| 0x004afb3c | float | `g_clipBoundsMaxX` | high | SetClipBounds + SetProjectedClipRect (5 refs) | `(none)` |
| 0x004afb40 | float | `g_clipBoundsMinY` | high | SetClipBounds + SetProjectedClipRect (5 refs) | `(none)` |
| 0x004afb44 | float | `g_clipBoundsMaxY` | high | SetClipBounds + SetProjectedClipRect (5 refs) | `(none)` |
| 0x004af314 | u32* | `g_immediateDrawScratchHeader` | medium | PUSH 0x4af314 in FlushImmediateDrawPrimitiveBatch / SubmitImmediateTranslucentPrimitive / RenderPreparedMeshResource (7 refs) — header for queued cmds | `(none)` |
| 0x004aee68 | u32* | `g_translucentQueueHeader` | medium | AppendClippedPolygonTriangleFan / SubmitImmediateTranslucentPrimitive / RenderPreparedMeshResource / Flush... (7 refs) — translucent queue base | `(none)` |
| 0x004aaf60 | u32 | `g_translucentPipelineInitialized` | low | InitializeTranslucentPrimitivePipeline ref pattern (gated init) | `(none)` |
| 0x004ab040 | float | `g_cameraBasisMatrix_m00` | high | BuildCameraBasisFromAngles writes `0x3f800000` (=1.0f); FinalizeCameraProjectionMatrices FLD/FSTP. Diag entry of 3x3 view-basis matrix (9 refs) | `(none)` |
| 0x004ab044 | float | `g_cameraBasisMatrix_m01` | high | Same writer, value 0.0f — m[0][1] (5 refs) | `(none)` |
| 0x004ab070 | float | `g_cameraBasisMatrix_m11` | high | Same writer family — 1.0f diag (15 refs) — m[1][1] | `(none)` |
| 0x004ab074 | float | `g_cameraBasisMatrix_m12` | high | Same writer, 0.0f (6 refs) — m[1][2] | `(none)` |
| 0x004ab010 | float* | `g_cameraBasisMatrix_basePtr_PROVISIONAL` | medium | ApplyMeshResourceRenderTransform/QueryRaceSharedPointer pointer-load (6 refs) | `(none)` |
| 0x004aafa4 | float | `g_viewProjScratchA0` | medium | BuildCameraBasisFromAngles writes 0; OrientCameraTowardTarget writes 0 (6 refs) — view-proj scratch slot | `(none)` |
| 0x004aafa8 | float | `g_viewProjScratchA1` | medium | OrientCameraTowardTarget FSTP, BuildCameraBasis writes 0 (5 refs) | `(none)` |
| 0x004aafe0 | float | `g_viewProjScratchB0` | medium | FinalizeCameraProjectionMatrices FLD/FSTP, ApplyMeshRenderBasis* readers (14 refs) | `(none)` |
| 0x004aafe4 | float | `g_viewProjScratchB1` | medium | Same callers (8 refs) | `(none)` |
| 0x004aafe8 | float | `g_viewProjScratchB2` | medium | Same callers (5 refs) | `(none)` |
| 0x004aafec | float | `g_viewProjScratchB3` | medium | Same callers (5 refs) | `(none)` |
| 0x004aaff0 | float | `g_viewProjScratchC0` | medium | Same callers (5 refs) | `(none)` |
| 0x004aaff4 | float | `g_viewProjScratchC1` | medium | Same callers (5 refs) | `(none)` |
| 0x0048db90 | u16 | `g_renderSavedFpuControlWord` | high | InitializeRaceRenderGlobals FSTCW (saves orig CW) — 7 refs incl TransformAndQueueTranslucentMesh, ComputeMeshVertexLighting | `(none)` |
| 0x0048db94 | u16 | `g_renderActiveFpuControlWord` | high | InitializeRaceRenderGlobals FLDCW (restored on entry); FPU PC=64 RC=down mode for render — paired w/ 0x0048db90 (7 refs) | `(none)` |
| 0x0048dba0 | u32 | `g_raceRenderInitialized` | high | InitializeRaceRenderGlobals 0; InitializeRaceRenderState compares ==1 then writes 1; ReleaseRaceRenderResources (5 refs) | `(none)` |
| 0x004ae2ac | u16 | `g_vehicleRenderViewCount` | medium | InitializeRaceVehicleRuntime writer (8 refs) — count of vehicle render views | `(none)` |
| 0x004ae2b4 | u16 | `g_vehicleRenderViewFlags` | medium | InitializeRaceVehicleRuntime writer (7 refs); paired w/ 0x004ae2ac | `(none)` |
| 0x004ae2f8 | u32 | `g_vehicleRenderLodMode` | medium | InitializeRaceVehicleRuntime writer (5 refs) | `(none)` |
| 0x004bf504 | u32 | `g_wantedDamageHudOverlayCount` | medium | AwardWantedDamageScore/UpdateWantedDamageIndicator/InitializeWantedHudOverlays writer (5 refs) | `(none)` |
| 0x004bf528 | u32 | `g_meshProjectionEffectMode` | medium | SetProjectionEffectState + ApplyMeshProjectionEffect writer/reader (6 refs) | `(none)` |
| 0x004bec50 | u32 | `g_trackedActorMarkerCount` | medium | InitializeTrackedActorMarkerBillboards + RenderTrackedActorMarker (6 refs) | `(none)` |
| 0x004bed08 | u32 | `g_trackedActorMarkerSurface` | medium | Same callers (6 refs) | `(none)` |
| 0x004beb98 | u32 | `g_trackedActorMarkerListBase` | low | Initialize + Render callers (5 refs) | `(none)` |
| 0x004b1158 | u32 | `g_raceHudStatusTextPtr` | medium | InitializeRaceHudLayout + DrawRaceStatusText writers (6 refs) | `(none)` |
| 0x004ab070 | float | (dup) | | already listed | |

## Key discoveries

- 0x0048db90/0x0048db94 are the **paired FPU control-word save/restore** — relevant to `reference_fpu_control_word_arch_divergence_2026-05-20`. Confirms the render code is supposed to enter with PC=64 RC=down before fixed-point transforms.
- The 3x3 camera-basis matrix at 0x004ab040..0x004ab07c lives **separately** from the 4x4 render-transform at 0x004bf6b8. Two distinct matrices — Wave 2 should rename `g_cameraBasisMatrix_m00..m22` and re-type as float[3][3].
- The clipper draw-call state spans **two clusters**: 0x004afb20..0x004afb2c (post-projection clip rect) and 0x004afb38..0x004afb44 (pre-projection clip bounds). Suggests Wave 2 should type both as `Rect2DF` structs (8 bytes each).
- The polygon-clip out-vertex scratch (0x004af280..0x004af28c) and topology hint (0x004af268..0x004af27c) form a single 0x40-byte draw-call submission state.

## Out-of-scope finds

| address | brief note | probable area |
|---|---|---|
| 0x004cf098 | u32 | locale/MSVC __mbctype — ARCH-COLLAPSED, skip |
| 0x004cfce0 | u32 | __pioinfo MSVC — ARCH-COLLAPSED, skip |
| 0x004cf990/9a0/9ac | u32 | _setmbcp init flags — ARCH-COLLAPSED |

## TODO impact

- No directly closed TODOs. But naming the clipper/projection state shrinks unknown surface in render code paths under active cascade investigation (UpdateRaceActors → Render*).
