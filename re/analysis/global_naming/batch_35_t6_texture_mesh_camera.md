---
batch: 35
area: texture_mesh_camera
tier: T6
target_todos: []
ghidra_session: TD5_pool3
analyzed_addresses: 0x0040ba00, 0x0040b200, 0x0040b500, 0x0040c6c0, 0x0040cb00, 0x0040ae20, 0x0040ae70, 0x00401800, 0x00401a00, 0x004018a0, 0x0043dd00, 0x0043e600
agent: tier1-a-t6
date: 2026-05-22
---

# Globals enumeration — texture cache, mesh transform constants, camera basis tail (T6)

## Summary

- Functions analyzed: BuildTrackTextureCacheImpl, GetTextureSlotStatus, QueryRaceTextureCapacity, SwapIndexedRuntimeEntries, AdvanceTextureStreamingScheduler, AdvanceTexturePageUsageAges, ResetTexturePageCacheState, BindRaceTexturePage, ApplyMeshProjectionEffect, SetProjectionEffectState, TransformAndQueueTranslucentMesh, ComputeMeshVertexLighting, TransformMeshVerticesToView, ClipAndSubmitProjectedPolygon, InitializeRaceRenderGlobals, RenderRaceActorForView, BuildSpecialActorOverlayQuads, UpdateChaseCamera, RenderTrackMinimapOverlay, InitializeMinimapLayout
- Unnamed DAT_* targeted: 25+
- Already-named neighbors noted: g_currentRenderTransform (0x004bf6b8)
- Proposals — high: 15
- Proposals — medium: 6
- Proposals — comment-only: 3

## Methodology

Walked `0x0048dXXX` cluster identifying texture-cache state. The `BuildTrackTextureCacheImpl` writes a runtime entry table head + count + capacity. The `0x0048dc6X..0x0048ddXX` stride of 0x2c looks like an array of 4-float records (8 entries of 4 floats = 0x80 bytes). For each: FADD+FSTP pattern with `[ESI + 0x18/0x44/0x70/0x9c]` suggests a 4×4 matrix being accumulated into mesh vertices. Camera tail (0x004ab040..0x004ab07c) confirms 3×3 basis matrix.

## Proposals (globals)

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x0048dc3c | u32 | `g_textureCacheRuntimeHead` | high | BuildTrackTextureCacheImpl + GetTextureSlotStatus + AdvanceTextureStreamingScheduler (11 refs) — head of texture entry list | `(none)` |
| 0x0048dc40 | u32 | `g_textureCacheRuntimeCount` | high | SwapIndexedRuntimeEntries + BuildTrackTextureCacheImpl + QueryRaceTextureCapacity + ResetTexturePageCacheState + AdvanceTexturePageUsageAges (39 refs) — entry count | `(none)` |
| 0x0048dc44 | u32 | `g_textureCacheRuntimeCapacity` | high | BuildTrackTextureCacheImpl writer/reader (4 refs) — array capacity | `(none)` |
| 0x0048dba0 | u32 | `g_raceRenderInitialized` | high | (dup from batch 29) — InitializeRaceRenderGlobals 0; InitializeRaceRenderState (5 refs) | `(none)` |
| 0x0048db90 | u16 | `g_renderSavedFpuControlWord` | high | (dup batch 29) | `(none)` |
| 0x0048db94 | u16 | `g_renderActiveFpuControlWord` | high | (dup batch 29) | `(none)` |
| 0x0048dc68 | float | `g_meshTransformAccumulator_row0_x` | high | `FADD/FSTP [ESI+0x18]` in RenderRaceActorForView + BuildSpecialActorOverlayQuads (4 refs); element of 4x4 transform accumulator | `(none)` |
| 0x0048dc94 | float | `g_meshTransformAccumulator_row0_y` | medium | `FADD/FSTP [ESI+0x44]` (4 refs); +0x2c from row0_x | `(none)` |
| 0x0048dcc0 | float | `g_meshTransformAccumulator_row0_z` | medium | `[ESI+0x70]` (4 refs); +0x2c | `(none)` |
| 0x0048dcec | float | `g_meshTransformAccumulator_row0_w` | medium | `[ESI+0x9c]` (4 refs); +0x2c | `(none)` |
| 0x0048dd20 | float | `g_meshTransformAccumulator_row1_x` | medium | `[ESI+0x18]` (4 refs); next row +0x34 | `(none)` |
| 0x0048dd4c | float | `g_meshTransformAccumulator_row1_y` | medium | `[ESI+0x44]` (4 refs) | `(none)` |
| 0x0048dd78 | float | `g_meshTransformAccumulator_row1_z` | medium | `[ESI+0x70]` (4 refs) | `(none)` |
| 0x0048dda4 | float | `g_meshTransformAccumulator_row1_w` | medium | `[ESI+0x9c]` (4 refs) | `(none)` |
| 0x004749e0 | float | `g_meshProjectionScaleX` | medium | ApplyMeshProjectionEffect read 3x (3 refs) | `(none)` |
| 0x004749e4 | float | `g_meshProjectionScaleY` | medium | Same context (3 refs) | `(none)` |
| 0x00474850 | float[N] | `g_minimapMarkerScaleTable` | medium | LEA `[EDX*4 + 0x474850]` then FLD/FMUL in RenderTrackedActorMarker (4 refs) — per-slot scale | `(none)` |
| 0x00474ce8 | u32[N] | `g_trafficVehicleVariantTable` | high | `[EAX*4 + 0x474ce8]` in LoadTrafficVehicleSkinTexture/GetTrafficVehicleVariantType/LoadRaceVehicleAssets (5 refs) — 4-byte stride lookup | `(none)` |
| 0x00474d74 | u32[N] | `g_trafficVehicleSkinTable` | high | Same callers (4 refs) — paired with variant table | `(none)` |
| 0x0048f30c | u8 (offset) | `g_savedCarState_base` | high | `[reg + 0x48f30c]` in 3+ CarSelection sites — struct base; see batch 33 | td5_save.c |
| 0x004b0a78 | u32 | `g_minimapLayoutWidth_PROVISIONAL` | medium | RenderTrackMinimapOverlay + InitializeMinimapLayout (9 refs) | `(none)` |
| 0x004b0a54 | u32 | `g_minimapLayoutHeight_PROVISIONAL` | low | (3 refs) | `(none)` |
| 0x004b0a50 | u32 | `g_minimapLayoutOriginX_PROVISIONAL` | low | (3 refs) | `(none)` |
| 0x004b1158 | u32 | `g_raceHudStatusTextPtr` | high | InitializeRaceHudLayout + DrawRaceStatusText (6 refs) | `(none)` |
| 0x004b0214 | u32 | `g_trafficPeerCandidateState` | medium | (dup batch 32) | `(none)` |
| 0x004bf504 | u32 | `g_wantedDamageHudOverlayCount` | high | (dup batch 29) | `(none)` |
| 0x00483958 | u32 | `g_vehicleRenderDecalSet_PROVISIONAL` | low | RenderVehicleActorModel + ComputeActorWorldBoundingVolume (5 refs); pushed as ptr | `(none)` |
| 0x00483050 | u32 | `g_vehicleCollisionPairScratchPtr_PROVISIONAL` | low | ResolveVehicleContacts + ResolveVehicleCollisionPair (5 refs) | `(none)` |

## Key discoveries

- The mesh-transform accumulator at 0x0048dc68..0x0048ddXX has a **two-row × four-column** layout (rows at stride 0x34, columns at stride 0x2c within each row). This is actually a 2×4 float matrix accumulator used by RenderRaceActorForView and BuildSpecialActorOverlayQuads — likely an interlaced layout for SIMD-style processing. Wave 2 should investigate the stride more closely; current evidence supports 2 rows × 4 cols = 8 floats (32 B), but cluster is wider, so it may be a 4-of-8 banded layout.
- 0x0048dc3c/40/44 is the texture-runtime descriptor (head, count, capacity) — separate from `g_trackVertexPool` etc.
- 0x004749e0/e4 are projection scale factors used by ApplyMeshProjectionEffect — accessed via base register + 0x4749e0, suggesting they're part of a small projection-effect struct.

## Out-of-scope finds

| address | brief note | probable area |
|---|---|---|
| 0x004cf080/088 | (4 refs each) — locale | ARCH-COLLAPSED |
| 0x004cf078 | (4 refs) — locale | ARCH-COLLAPSED |
| 0x004cf368/568 | (4 refs each) — locale | ARCH-COLLAPSED |
| 0x004ceff0/eff8 | (4 refs) — locale | ARCH-COLLAPSED |
| 0x004ad150/52 | u16 — cop chase scratch | T7 cop |

## TODO impact

- No direct closures. Texture-cache state surface confirms `g_textureCacheRuntimeCount` (39 refs) is the most-trafficked texture-cache descriptor — useful for any future texture-streaming divergence audit.
