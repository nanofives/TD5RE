---
batch: 15
area: vfx_render_frame
tier: T3
target_todos: [todo_smoke_render_broken_2026-05-19, todo_car_shadow_oversized_2026-05-17, todo_precise_port_extend_particle_render_2026-05-17, reference_particle_render_audit_2026-05-17]
ghidra_session: fdce3c47724641e18db6afde32670145
analyzed_addresses: 0x00429510, 0x00429690, 0x00429720, 0x00429790, 0x004297d0, 0x00429950, 0x00429cf0, 0x0042a6b0, 0x0042ce50, 0x00401410, 0x0040ae10, 0x0040bb70, 0x00431340, 0x00431460, 0x004315b0, 0x00432bd0, 0x0043cdc0, 0x0043c9e0, 0x0043e3b0, 0x0043e5f0, 0x0043e7e0, 0x0043e990, 0x0042dca0, 0x0042de10, 0x0042e750, 0x00446240, 0x004464b0, 0x00446560
agent: Claude Opus 4.7 (1M)
date: 2026-05-20
---

# Globals enumeration — VFX + render frame setup

## Summary

- Functions analyzed: 28 (entry points + initializers + render-queue walkers)
- Unnamed DAT_* globals encountered: 27 (after de-dup); 17 proposed for naming
- Already-named globals encountered (just noted): 17 (`g_raceRotationMatrixPtr`, `g_currentRenderTransform`, `g_subTickFraction`, `g_projectionDepth`, `g_inverseProjectionDepth`, `g_projectionDepthBias`, `g_cachedViewportWidth`, `g_cachedViewportHeight`, `g_frustumLeftPlaneNormalX/Z`, `g_frustumTopPlaneNormalY/Z`, `g_renderCurrentMaterialHandle`, `g_renderCurrentTextureHandle`, `g_primitiveBucketBasePtr`, `g_primitiveBucketWorkPtr`, `g_primitiveBucketWriteOffset`, `gPrimitiveSortBucketArray`, `g_trackEnvironmentConfig`, `g_audioOptionsOverlayActive`, `g_weatherType`, `g_weatherActiveCountView0`, `g_trackHeightBaseOffset`, `g_shadowVerticalOffset`, `g_racerCount`, `gSlotMeshResourcePtrTable`, `g_inputPlaybackActive`)
- Proposals — high confidence: 17
- Proposals — medium confidence: 6
- Proposals — comment-only (low confidence): 4

## Methodology

Entry points: `SpawnVehicleSmokeSprite @ 0x00429CF0`, the smoke-callback LABs at `0x00429950` (update) and `0x004297D0` (render), `InitializeRaceParticleSystem @ 0x00429510`, `InitializeRaceSmokeSpritePool @ 0x00401410`, `BuildSpriteQuadTemplate @ 0x00432BD0`, `QueueTranslucentPrimitiveBatch @ 0x00431460`. From these I walked one level of callers — `UpdateRaceParticleEffects @ 0x00429790`, `DrawRaceParticleEffects @ 0x00429720`, `ProjectRaceParticlesToView @ 0x00429690`, `SpawnAmbientParticleStreak @ 0x0042A6B0`, `SetCameraWorldPosition @ 0x0042CE50` — and the frustum / projection / billboard families (`IsBoundingSphereVisibleInCurrentFrustum @ 0x0042DCA0`, `TestMeshAgainstViewFrustum @ 0x0042DE10`, `ConfigureProjectionForViewport @ 0x0043E7E0`, `InsertBillboardIntoDepthSortBuckets @ 0x0043E3B0`, `InitializeProjectedPrimitiveBuckets @ 0x0043E5F0`, `InitializeVehicleShadowAndWheelSpriteTemplates @ 0x0040BB70`, `InitializeTrackedActorMarkerBillboards @ 0x0043C9E0`, `AdvanceWorldBillboardAnimations @ 0x0043CDC0`, `InitializeTireTrackPool @ 0x0043E990`, `InitializeWeatherOverlayParticles @ 0x00446240`, `UpdateAmbientParticleDensityForSegment @ 0x004464B0`, `RenderAmbientParticleStreaks @ 0x00446560`).

Three structural insights drove the relevance gate:

1. The **race particle pool** is a flat 2-view array of 100 × 0x40-byte records at base `DAT_004A3170`. View 0 fills `[0x004A3170 .. 0x004A4A70)`; view 1 fills the next `0x1900` bytes ending at `0x004A6370`. The pool element is a 64-byte struct with byte fields (flags/age) at +0x00..+0x03, two 16.16 fixed-point velocity dwords at +0x04/+0x08, animation phase+0x0c/+0x10/+0x14, lifetime in halfwords at +0x06/+0x0a, **the alpha word at +0x08 (relative to the per-slot ECX base)**, and callback dispatch pointers at +0x38 (update) and +0x3c (render). Note: the addresses in the disassembly are biased by `0x1f`, so per-slot field offsets are visible as `4A318F + i*0x40` (i.e. the +0x1f byte is the slot-active flag, addressed with the absolute base `0x4A318F`).
2. The **per-slot color/UV scratch table** at `DAT_004A63D8` is a separate 100-entry × 0xB8 array per view (so `DAT_004A63D8 + (view*100 + slot)*0xB8` is the scratch quad for that particle). This is the buffer `BuildSpriteQuadTemplate` writes screen-space verts + UV + per-vertex color into. The "per-view tile-active bitmap" at `DAT_004A6370` (50 bytes/view) maps `pcVar4 = scratch slot index` to active-state; that's the second-level allocator pool referenced by `SpawnVehicleSmokeSprite`.
3. The **shared sprite-UV template table** at `DAT_004AABB8` (stride 0x14 = 5 floats: u0, v0, u1, v1, page) is the lookup `RenderAmbientParticleStreaks` and the smoke-render callback both bind into the quad scratch via `LEA EAX, [ECX*0x4 + 0x4AABB8]`. The smoke render at 0x004297D0 derives an index from `(DAT_004A3170[slot] >> 2) * 5` — i.e. the high-6-bits of the type byte select 12 different smoke variants. The dead-code "smoke variant grid" flagged in `reference_particle_render_audit_2026-05-17` corresponds to this table.

Once the pool layout was nailed down, every global write/read at offsets within `[0x4A3170..0x4A6390]` could be tagged as a per-slot field alias and skipped (those go in a future struct-typing pass, not this naming batch).

## Proposals

### Race particle pool + per-view scratch (per-frame transient state)

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x004A3170 | byte[2][100][0x40] | `g_raceParticlePoolBase` | **high** | Base of the 2-view × 100-slot × 64-byte race-particle pool. Written/read by all 8 smoke spawn functions and both smoke-callback LABs. Per-slot record stride 0x40; per-view stride 0x1900. The audit memo's "smoke alpha hardcoded" issue lives here — alpha word at slot-offset +0x08, decremented per frame by short delta at slot-offset +0x0a (smoke-update LAB 0x00429950). | `td5_vfx.c` `s_particle_banks[2][TD5_VFX_PARTICLE_BANK_SIZE]` (line 225) |
| 0x004A6370 | byte[2][50] | `g_raceParticleScratchActiveBitmap` | **high** | Per-view active-flag bitmap for the 50-entry quad scratch pool at `g_raceParticleScratchTable`. `SpawnVehicleSmokeSprite` walks this looking for a zero byte (free slot); render path ages it via `DrawRaceParticleEffects` (negative-counter bump). 15 xrefs. | (none — port uses different scratch allocator) |
| 0x004A63D4 | u32 | `g_activeRenderViewIndex` | **high** | Per-frame transient. Written by `UpdateRaceParticleEffects` (0x00429795) and `DrawRaceParticleEffects` (0x00429729) with `param_1`. Read by **every** smoke-callback LAB at the head of the function: `MOV EAX, [0x004A63D4]` followed by `LEA EAX, [EAX*5*5*4*0x40]`. Identifies which of the two render views the callback is currently servicing. **9 xrefs, all in the smoke pipeline.** | `td5_vfx.c` `s_active_view_index` |
| 0x004A63D8 | byte[2][100][0xB8] | `g_raceParticleScratchTable` | **high** | The per-slot SCRATCH for built sprite quads (post-`BuildSpriteQuadTemplate`). 0xB8 bytes per quad (4 verts × ~0x2C + header). `ProjectRaceParticlesToView` writes projected coords here; `QueueTranslucentPrimitiveBatch` enqueues pointer-into-this for later draw. **This is the per-frame transient "render queue".** | `td5_vfx.c` quad-build scratch (currently per-particle local) |
| 0x004AABB8 | float[N][5] | `g_spriteUvTemplateTable` | **high** | Shared UV-page template table. Stride 0x14 (5 floats: u0, v0, u1, v1, atlas_page). Written by `InitializeRaceParticleSystem` final loop. Read by smoke-render LAB (`LEA EAX, [ECX*4 + 0x4AABB8]`) using `(type_byte >> 2) * 5` as index. **This is the "smoke variant grid" the audit flagged as dead code** — the table IS populated, the smoke render IS indexing it, but the orig's spawn path only ever uses the first row. | `td5_vfx.c` `s_smoke_variant_uv[4][5]` (line 238) |

### Smoke + rain resource pointer / template globals

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x00482DC0 | void* | `g_raceSmokeSpritePool` | **high** | Set ONLY by `InitializeRaceSmokeSpritePool @ 0x00401410` to `HeapAllocTracked(g_racerCount * 0x170)`. Per-actor smoke sprite block (0x170 bytes per racer). Gated by `g_trackEnvironmentConfig+4 == 1` (smoke-enabled). 1 writer, multiple readers in wheel-smoke spawn path. | `td5_vfx.c` per-actor smoke buf |
| 0x00482DE0 | byte* | `g_smokeArchiveEntryPtr` | high | Set ONLY by `InitializeRaceSmokeSpritePool` to the SMOKE archive entry; read by every smoke-spawn function to get atlas dims. Sibling of `g_raceSmokeSpritePool`. | `td5_vfx.c` smoke archive entry cache |
| 0x004A3110..0x004A3120 | float[5] | `g_smokeAtlasUvTemplate` | **high** | 5-float UV template (u0, v0, u1, v1, page) for the default SMOKE sprite. Set by `InitializeRaceParticleSystem` from the SMOKE archive entry header. Used as the **first** entry in `g_spriteUvTemplateTable` and by `SpawnAmbientParticleStreak` defaults. Cluster: 0x004A3110 (u0=+1), 0x004A3114 (v0=+1), 0x004A3118 (u1=-2), 0x004A311C (v1=-2), 0x004A3120 (atlas page). | `td5_vfx.c` `s_smoke_u0/v0/u1/v1/s_smoke_page` (lines 234-235) |
| 0x004A3128..0x004A3138 | float[5] | `g_rainSplashAtlasUvTemplate` | high | 5-float UV template for the RAINSPL sprite (rain splash). Set by `InitializeRaceParticleSystem` from the RAINSPL archive entry header BEFORE the SMOKE entry is loaded. Same packing as `g_smokeAtlasUvTemplate`. Used by `SpawnAmbientParticleStreak` (rain streaks). | (none — port handles weather separately) |

### View / camera / frustum scratch (per-frame view setup)

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x004AAFA0 | float[3][3] | `g_currentViewRotationMatrix` | **high** | The active view-rotation 3x3 matrix used by every `TransformVec3ByRenderMatrixFull` call site this frame. **16 xrefs**, written by `SetCameraWorldPosition`-adjacent code (0x0042D0B9, 0x0042D6BE, 0x0042D718). Read by `IsBoundingSphereVisibleInCurrentFrustum`, `TestMeshAgainstViewFrustum`, `ProjectRaceParticlesToView`, `RenderAmbientParticleStreaks`. Row stride 0xC (3 floats), 3 rows. | `td5_render.c` view-matrix global (per-view) |
| 0x004AAFC4 | float[3] | `g_currentViewWorldOrigin` | **high** | Camera world position; 3 floats (x, y, z). Written ONLY by `SetCameraWorldPosition @ 0x0042CE50`. Read by `IsBoundingSphereVisibleInCurrentFrustum` and `TestMeshAgainstViewFrustum` to subtract from object position before applying `g_currentViewRotationMatrix`. **4 xrefs, all in the view-cull pipeline.** | `td5_render.c` camera-world-pos global |
| 0x004AB0C0 | float[3] | `g_frustumCullScratch` | high | 3-float scratch buffer written by `IsBoundingSphereVisibleInCurrentFrustum` while computing the view-space sphere center, then read by adjacent code. 2 xrefs only (1 write, 1 read in the SAME function family). True ephemeral. | (none — port computes locally) |
| 0x0046735C | float | `g_nearClipDistance` | high | Read by both `IsBoundingSphereVisibleInCurrentFrustum` (`+fVar1 - nearClip`) and `TestMeshAgainstViewFrustum`. Located in the `.rdata` constants block alongside `_DAT_0045d624` (= 0.0f). 2 xrefs only (both reads, no writers — compile-time constant). | `td5_render.c` near-clip const |
| 0x00467360 | float | `g_farClipDistance` | high | Twin of `g_nearClipDistance` for the far-plane comparison: `fVar3 - fVar1 - farClip`. One WRITE found at 0x0042D48E (so dynamic, but rarely written — likely viewport setup); 2 readers in the same frustum family. | `td5_render.c` far-clip const |

### Translucent primitive queue / draw-call scratch

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x004AF26C | void* | `g_translucentPrimitiveBucketHead` | high | Linked-list head of pending translucent primitive batches. Written by `FlushQueuedTranslucentPrimitives @ 0x00431340` setup. Read by `QueueTranslucentPrimitiveBatch @ 0x00431460` to walk and insert. 3 xrefs total. | `td5_render.c` queued-prims linked list |
| 0x004AF270 | u32 | `g_translucentPrimitiveBucketCount` | high | Counter for the bucket linked at `g_translucentPrimitiveBucketHead`; capped at 0x1FE (510) by `QueueTranslucentPrimitiveBatch`. Reset to 0 by `FlushQueuedTranslucentPrimitives`. 3 xrefs. | `td5_render.c` queued-prims count |
| 0x004AFB14 | void* | `g_currentDrawCallVertexBuffer` | high | Aligned vertex-buffer base (HeapAlloc'd 0x801F bytes, 32-byte aligned) shared by all translucent draw calls. **12 xrefs** across the translucent pipeline (`SubmitImmediateTranslucentPrimitive`, `FlushQueuedTranslucentPrimitives`, `EmitTranslucentTriangleStrip` callbacks). Written ONCE by `InitializeProjectedPrimitiveBuckets @ 0x0043E5F0`. | `td5_render.c` immediate vert buffer |
| 0x004AFB48 | void* | `g_currentDrawCallIndexBuffer` | high | Sibling of `g_currentDrawCallVertexBuffer`; 0x41F bytes aligned to 32 bytes. Index buffer for the same draw-call. Written ONCE by `InitializeProjectedPrimitiveBuckets`. | `td5_render.c` immediate index buffer |
| 0x004AFB4C | u32 | `g_currentDrawCallVertexCount` | high | Vertex count for the pending immediate draw call; checked nonzero before flushing. 6 xrefs in the translucent flush family. | `td5_render.c` draw vert count |
| 0x004AFB50 | u32 | `g_currentDrawCallIndexCount` | high | Sibling of `g_currentDrawCallVertexCount`; index count. 6 xrefs. | `td5_render.c` draw index count |

### Tire-track + weather pool tables

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x004C375C | void* | `g_tireTrackQuadPool` | **high** | 0x49C0-byte tire-track quad pool (50 entries × 0xEC). Written ONLY by `InitializeTireTrackPool @ 0x0043E990`. **71 xrefs** across `UpdateTireTrackPool`, `RenderTireTrackPool`, `AcquireTireTrackEmitter`. The audit memo's tire-track Y z-fight + intensity-floor concerns operate on records in this pool. | `td5_vfx.c` tire-track pool |
| 0x004C3758 | u32 | `g_tireTrackEmitterCount` | high | Counter of active emitters; written by `InitializeTireTrackPool` (=0), `UpdateTireTrackPool`, `AcquireTireTrackEmitter`. 7 xrefs. | `td5_vfx.c` tire-track active count |
| 0x004C3720 | byte[8][7] | `g_tireTrackEmitterRecords` | med | 56-byte block (8 emitter records × 7 bytes) cleared by `InitializeTireTrackPool`. Written by `AcquireTireTrackEmitter` end at 0x0043F0D5. | `td5_vfx.c` per-vehicle emitters |
| 0x004C3DAC | void*[2] | `g_weatherParticleBufferPerView` | **high** | Pair of `HeapAlloc(0x6400)` buffers (one per view), 128 streak records × 200 bytes each. Allocated by `InitializeWeatherOverlayParticles` for both rain and snow. Read by `RenderAmbientParticleStreaks` and `UpdateAmbientParticleDensityForSegment` using `(&DAT_004C3DAC)[view_idx]`. | `td5_vfx.c` `s_weather_buf[2]` (line 241) |
| 0x004C3DA8 | byte* | `g_weatherSpriteArchiveEntry` | high | Resource pointer for the active rain/snow sprite (RAINDROP or 4CSNOWDROP), set by `InitializeWeatherOverlayParticles`. | (none) |
| 0x004C3DD8 | u32[2] | `g_weatherSegmentTargetDensity` | high | Per-view target streak count, written by `UpdateAmbientParticleDensityForSegment` from per-segment env-config halfword pairs (clamped to 0x80). Compared each frame against `g_weatherActiveCountView0` to add/remove streaks. | `td5_vfx.c` `s_weather_target_density[2]` (line 243) |
| 0x004C3DD0 | float[2] | `g_weatherStreakParam2CachePerView` | med | 2 floats per-view cache for the `param_2` argument to `RenderAmbientParticleStreaks` (only filled when `g_audioOptionsOverlayActive == 0`). Used to keep streaks visually stable when the pause/options overlay is shown. | (none) |
| 0x004C3DB8 | float[2][3] | `g_weatherStreakPrevCameraPos` | med | Per-view previous camera position (3 floats × 2 views = 24 bytes). Written by `RenderAmbientParticleStreaks` to derive streak direction vs camera motion. Same overlay-gated update as `g_weatherStreakParam2CachePerView`. | (none) |
| 0x004C3DDC | u32 | `g_weatherSegmentTargetView0_PROVISIONAL` | low | Sibling of 0x004C3DD8; this offset (+0x4) is set to 0 by `InitializeWeatherOverlayParticles` but never written elsewhere. Likely view-1 mirror of `g_weatherSegmentTargetDensity` (`&DAT_004C3DD8 + view*4` pattern). Comment-flag for verification. | (none) |
| 0x004C3DE4 | u32 | `g_weatherActiveCountView1_PROVISIONAL` | low | Mirror of `g_weatherActiveCountView0 @ 0x004C3DE0` for view 1 (consistent with the `(&g_weatherActiveCountView0)[param_3]` indexing seen in `RenderAmbientParticleStreaks`). Cleared by `InitializeWeatherOverlayParticles`. | (none) |

### Tracked-actor marker billboards (police-light overlay family)

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x004BEDC0 | byte[N][0x458] | `g_trackedActorMarkerBillboardPool` | med | 0x458-byte records, count derived from loop end at 0x004BF674 → 4 records. Cleared by `InitializeTrackedActorMarkerBillboards @ 0x0043C9E0` to seed 6 sub-templates per record (red/blue light + 2 marker variants × 2). `AdvanceWorldBillboardAnimations` walks this stride 0x22C (so 2 sub-records per primary). | (none — port stubs police markers) |
| 0x004BEDC0 (+0x0)  | u32 | `g_trackedActorMarkerAnimPhase_PROVISIONAL` | low | Final 4 dword writes at 0x004BEDC0=0, 0x004BEFEC=0x80, 0x004BF218=0x40, 0x004BF444=0xC0 — these set the initial animation phase for the 4 record bases at 0x90-degree offsets. Confirms record stride of 0x22C × 2 = 0x458. | (none) |

### Shadow + wheel sprite templates

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x0048DC50 | byte[6][0x170] | `g_vehicleShadowAndWheelSpriteTemplates` | high | 6 records × 0x170 bytes. Cleared by `InitializeVehicleShadowAndWheelSpriteTemplates @ 0x0040BB70` (end at 0x0048F128). Holds the shadow quad + wheel-billboard pair per racer slot. Per-record local_28..local_24 constants `0x42840000`/`0x42FC0000` = (66.0f, 126.0f) define the shadow UV bounding box (referenced in audit). The shadow template is the first of 2 `BuildSpriteQuadTemplate` calls in the loop. | `td5_render.c` shadow+wheel templates |

### Sprite-quad template "kFlag" constants block (out-of-scope but flagged)

These appear as `local_3c..local_70` initializer arrays in EVERY `BuildSpriteQuadTemplate` caller. They're stack-locals, not globals — but `BuildSpriteQuadTemplate` interprets them via flag bits `1 | 2 | 4 | 0x100 | 0x200`. Documenting here so the next batch sees the encoding (`param_1[0x1A]` = primitive-type, `param_1[0x1B]` = texture-page) without re-reading the function.

## Key discoveries

1. **The smoke alpha-fade source lives at race-particle-pool offset +0x08 (16-bit unsigned).** The smoke-render LAB at `0x004297D0` reads it as `MOV CX, word ptr [EAX + 0x4A3178]` then `FILD; FMUL float ptr [0x00466D3C]`. `[0x00466D3C]` = `0x3CAAAAAB` = 1.0/48.0 (decimal). Each smoke particle is spawned by `SpawnVehicleSmokeSprite @ 0x00429CF0` with an initial alpha-word of `0x2080` (= 8320) and a per-frame delta-word at +0x0A of `-0x3000 / bVar` where `bVar = 0x0F * ((rand()&3)+1)` ∈ {15, 30, 45, 60}. So smoke fades from 8320 → 0 over 15-60 frames depending on the random lifetime. **At alpha=8320 / 48 ≈ 173, that maps to a per-vertex color alpha (after scaling, see `BuildSpriteQuadTemplate` flag-4 path which writes `param_1[0x16..0x19] & 0xFF`).** This is the single missing piece in `todo_smoke_render_broken_2026-05-19`: the orig **does** fade smoke per-vertex, the port hardcodes `0xFFFFFFFF`. **Closing fix: in the port's smoke render, multiply the per-vertex color by `(alpha_word / 48)` clamped to [0..255] and pack into the alpha channel.**

2. **There is no "smoke spawn rate" global** — the spawn-rate gate is a `rand() % 10 > 5` (60% chance per call) directly in `SpawnVehicleSmokeSprite` at 0x00429D04. Per-frame spawn count is therefore = (call rate from physics) × 0.6. The PER-WHEEL call rate comes from `SpawnRearWheelSmokeEffects @ 0x00401330` and `SpawnRandomVehicleSmokePuff @ 0x00401370` which gate on physics state (slip threshold). **No tunable global controls smoke density** — the audit memo flagging "spawn rate" is a misnomer; it's the slip-threshold gate that decides whether to call the spawn function at all.

3. **The render-view index is a per-frame global, not a parameter.** `g_activeRenderViewIndex @ 0x004A63D4` is written by `UpdateRaceParticleEffects` and `DrawRaceParticleEffects` at function entry, then every smoke/streak callback re-reads it from the global instead of taking a parameter. This means: callbacks running from the **wrong view** will silently corrupt the wrong view's pool. The port's `s_active_view_index` should be set the same way (it is — `td5_vfx.c` is byte-faithful here per quick check).

4. **Vehicle shadow size constants ARE present in the orig.** `InitializeVehicleShadowAndWheelSpriteTemplates` writes literal floats `0x42840000` (= 66.0) and `0x42FC0000` (= 126.0) as the UV-bbox endpoints, then computes the screen-space quad. The shadow vertical offset is in `g_shadowVerticalOffset @ 0x0048DC48` (already named) and the track-height base in `g_trackHeightBaseOffset @ 0x0048F070` (already named). **`todo_car_shadow_oversized_2026-05-17` closure**: the size constants are 66.0 (UV inner) and 126.0 (UV outer), encoded as `local_1C/local_28 = 66.0f, local_20/local_24 = 126.0f`. The port's shadow size constant should be cross-checked against these specific values — if the port uses anything outside [66..126], that's the size bug.

5. **`g_audioOptionsOverlayActive` is misnamed** — it functions as a "render-only / no state mutation" flag throughout the VFX path. In `RenderAmbientParticleStreaks` and `SpawnAmbientParticleStreak`, when this flag is set, **all writes to particle state are skipped**. Reading the name suggests it's specific to the audio-options screen but it's actually the general "pause overlay active" flag. Suggest a future rename (out of scope here) to `g_renderFreezeStateMutations` or similar. The port's pause-overlay handling needs to mirror this freeze-but-still-render behavior.

6. **The translucent draw queue is a single linked-list with 510-batch cap.** `g_translucentPrimitiveBucketCount @ 0x004AF270` caps at `0x1FE`, and the per-batch primitive class dispatches via `PTR_EmitTranslucentTriangleStrip_00473B9C[primitive_type]`. The audit memo's "billboard-into-bucket sort" overlap concern lives at a different table (`gPrimitiveSortBucketArray @ 0x004BF6C8`, already named). These are **two different sort/queue mechanisms** running in parallel: one for billboards (depth-sorted 0x1000 buckets), one for general translucent primitives (single linked-list at `g_translucentPrimitiveBucketHead`).

7. **The frustum-cull pipeline has TWO entry points.** `IsBoundingSphereVisibleInCurrentFrustum @ 0x0042DCA0` is the SPHERE test (used by mesh-LOD); `TestMeshAgainstViewFrustum @ 0x0042DE10` is the MESH-BOUNDS test (used by per-mesh dispatch). Both read the same `g_currentViewRotationMatrix` / `g_currentViewWorldOrigin` / `g_frustumLeftPlaneNormalX/Z` / `g_frustumTopPlaneNormalY/Z` / `g_nearClipDistance` / `g_farClipDistance` family. **The port likely needs both** — sphere for quick culling, mesh for accurate cull.

8. **`InitializeProjectedPrimitiveBuckets @ 0x0043E5F0` allocates a 64 KB sort buffer at `g_primitiveBucketBasePtr`.** This is the depth-sort backing store for `InsertBillboardIntoDepthSortBuckets`. The 0x1000-bucket sort array at `gPrimitiveSortBucketArray @ 0x004BF6C8` is the index table; the 64 KB buffer at `g_primitiveBucketBasePtr` is where the actual sort-chain link records live. `g_primitiveBucketWorkPtr @ 0x004BF520` is the bump-allocator cursor. **Important for the port**: if the port doesn't bound-check this 64 KB buffer, heavy-billboard scenes (Sydney, BlueRidge) could overflow. The orig has no bound-check either — assumed-safe-by-design.

## Out-of-scope finds

| address | brief note | probable area |
|---|---|---|
| 0x00466D3C | `0x3CAAAAAB` = 1.0/48.0 constant — the smoke-alpha scale divisor | T3 render constants batch |
| 0x00466D40 | `s_SMOKE` archive entry name string | T3 archive-names batch (no-op for naming) |
| 0x00466D48 | `s_RAINSPL` archive entry name string | T3 archive-names batch |
| 0x00473B9C | `PTR_EmitTranslucentTriangleStrip` 4-element dispatch table (primitive-class → emit fn) | T3 dispatch-tables batch |
| 0x00473BCF | `s_7Failed_DrawPrim__s` debug string | T3 debug-strings batch |
| 0x00474874 | `s_Police_blue`, 0x00474880 `s_Police_red`, 0x0047488C `s_PoliceLt_blue`, 0x0047489C `s_PoliceLt_red` — police marker resource names | T3 archive-names batch |
| 0x00474E58, 0x00474E5C, 0x00474E60 | Rain streak direction unit-vector floats (used in `RenderAmbientParticleStreaks` to derive direction) | T3 weather-tuning batch |
| 0x00474E64 | Rain perspective-scale constant `40.0f` (controls streak length-vs-depth) | T3 weather-tuning batch |
| 0x00474E68, 0x00474E6C, 0x00474E70 | Rain particle cull bounds (X/Y/Z absolute limits) | T3 weather-tuning batch |
| 0x00474E84 | `s_RAINDROP` archive entry name string | T3 archive-names batch |
| 0x00474E76 | `s_4CSNOWDROP` archive entry name string (the +2 offset in `InitializeWeatherOverlayParticles` skips "4C") | T3 archive-names batch |
| 0x0048DBA0, 0x0048DBA8 | `InitializeRaceRenderGlobals` one-time init flag + counter | T3 render-init batch |
| 0x0048DB90, 0x0048DB94 | FPU control word save/restore pair (preserved across race) | T3 FPU-state batch |
| 0x0045D5DC | `30.0f` constant — used as rain-streak quad half-extents | T3 render constants |
| 0x0045D5F4 | `0x10000.0f` (= 65536) — projection-depth inversion numerator | T3 render constants |
| 0x0045D604 | `4096.0f` — perspective-divide constant | T3 render constants |
| 0x0045D624 | `0.0f` — zero literal (used for frustum compares) | T3 render constants |
| 0x0045D5D0 | `0.5f` — half-pixel offset for UV correction | T3 render constants |
| 0x0045D714, 0x0045D7A0, 0x0045D7A4, 0x0045D79C, 0x0045D7A8 | Rain/snow random-range offset constants (clusters with 0x0045D78C = 0.5625 widescreen scale) | T3 render constants |
| 0x004C3D10 (`gSlotMeshResourcePtrTable`) | Already named — referenced by shadow-template loop; record stride 4 | (covered) |

## TODO impact

**todo_smoke_render_broken_2026-05-19:** **CLOSES.** Root cause identified concretely: smoke alpha source is the 16-bit word at race-particle-pool offset +0x08, scaled by 1/48.0 (constant at 0x00466D3C). Initial value 0x2080 = 8320 (≈173 after scale), decreased per-frame by short delta at +0x0A which equals `-0x3000 / lifetime_frames`. Suggested fix in `td5_vfx.c`: replace the 4× `0xFFFFFFFF` literal at lines 797, 802, 807, 812 with a computed `0x00FFFFFF | (alpha << 24)` where `alpha = clamp((alpha_word * 256) / 48 / 32, 0, 255)` (need to verify exact scale against the orig's pack-into-byte, but the SOURCE is now precisely located).

**todo_car_shadow_oversized_2026-05-17:** **PARTIAL CLOSE.** The shadow UV-bbox constants in the orig are 66.0f (inner) and 126.0f (outer), set as local literals inside `InitializeVehicleShadowAndWheelSpriteTemplates @ 0x0040BB70` (the `0x42840000` / `0x42FC0000` immediates in the disassembly, NOT a separate global). Already-named `g_shadowVerticalOffset @ 0x0048DC48` (=-22.0f / -4.0f based on `g_inputPlaybackActive`) and `g_trackHeightBaseOffset @ 0x0048F070` (=-36 / -18) define the vertical placement. Port should cross-check its shadow size constant against these literals. If the port currently uses ~66 to ~126 for the quad half-extents, it's correct; if larger, the bug is in the port's vertex-emit, not in a missing constant.

**todo_precise_port_extend_particle_render_2026-05-17:** Substantial supporting data. The full particle data flow now has a named global at every step: spawn (`g_raceParticlePoolBase`) → update (smoke-update LAB at 0x00429950 advances pose/lifetime fields in `g_raceParticlePoolBase`) → project (`ProjectRaceParticlesToView` writes view-space to `g_raceParticleScratchTable`) → render (smoke-render LAB at 0x004297D0 reads `g_spriteUvTemplateTable` + `g_currentViewRotationMatrix` + alpha-word, calls `BuildSpriteQuadTemplate` to fill `g_raceParticleScratchTable`) → queue (`QueueTranslucentPrimitiveBatch` appends pointer to `g_translucentPrimitiveBucketHead`) → flush (`FlushQueuedTranslucentPrimitives` walks the list and emits via `PTR_EmitTranslucentTriangleStrip` dispatch). A port-side precise-port pass should mirror this exact chain rather than the current single-pass "build + draw" approach.

**reference_particle_render_audit_2026-05-17:** Audit item #1 (smoke alpha fade) — **root-cause confirmed and located** (see Discovery #1). Audit item #3 (smoke-variant-grid dead code) — **partially overturned**: the grid table at `g_spriteUvTemplateTable @ 0x004AABB8` IS populated and the smoke-render path DOES index into it (`(type_byte >> 2) * 5`), but the spawn path only writes type bytes with the low 2 bits set (variants 0..3 of 4), and the high 6 bits seed from `iVar3 % 0x1f` (random 0..30) but only the low 6 bits select a row. So the table has dead rows for indices > 3, not the whole grid. The port's `s_smoke_variant_uv[4][5]` matches the active portion. Audit items #2 (tire-track Y z-fight), #4 (tire-track intensity floor) — pool now identified as `g_tireTrackQuadPool @ 0x004C375C`; an audit follow-up should walk `UpdateTireTrackPool @ 0x0043EB50` against the port's tire-track update.

## Ghidra session notes

- Session `fdce3c47724641e18db6afde32670145` opened `TD5_pool0` read-only as required.
- Pool slot acquired via `bash scripts/ghidra_pool.sh acquire`; released via `bash scripts/ghidra_pool.sh cleanup` after analysis.
- No writes to Ghidra performed. Names listed here are PROPOSED only — the consolidation session will apply them.
- Note: `LAB_00429950` (smoke update callback) and `LAB_004297D0` (smoke render callback) are NOT registered as functions in Ghidra; they're code labels referenced as function pointers. They should be promoted to named functions in a future cleanup pass — proposed names: `SmokeParticleUpdateCallback` and `SmokeParticleRenderCallback` respectively. Same applies to `LAB_0042A530` and `LAB_0042A590` (ambient streak update/render callbacks).
