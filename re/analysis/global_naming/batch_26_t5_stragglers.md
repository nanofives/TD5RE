---
batch: 26
area: t5_stragglers
tier: T6
target_todos: []
ghidra_session: 790080f879bf45409a2efe2fb4377463
analyzed_addresses: 0x00412030, 0x00414740, 0x00414a90, 0x00446240, 0x004464b0, 0x00446560, 0x00440b00, 0x00441a80, 0x00428880, 0x004285b0, 0x00428a60, 0x00430150
agent: Claude Opus 4.7 (1M context)
date: 2026-05-20
---

# Globals enumeration — T5 stragglers (TGA-config, weather View1 companions, lighting/FF false alarms)

## Summary

- Functions analyzed: 12 (4 cluster entry points + caller-traced writers/readers + lighting cross-check)
- Unnamed DAT_* globals encountered in target functions: 18 (after de-dup with T1-T5)
- Already-named globals encountered: 14 (noted, not renamed)
- Proposals — high confidence: 10
- Proposals — medium confidence: 7
- Proposals — comment-only (low confidence): 0
- **Clusters that turned out to be already-named**: 2 of 4 (per-slot lighting + FF per-player block)

## Methodology

T5.25 identified 4 small straggler clusters as out-of-scope. The methodology was: for each
cluster (a) probe the base address with `reference_to`/`listing_code_unit_containing` to enumerate
xrefs and check for an existing symbol; (b) walk into the primary writers via `decomp_function`
to identify the field semantics; (c) classify as new proposal vs already-named.

**Cluster verdicts**:

1. **TGA-config cluster at `0x00495xxx`** — REAL, named below. `LoadFrontendTgaSurfaceFromArchive @ 0x00412030` and 3 sibling variants (`LoadTgaToFrontendSurfaceFromArchive @ 0x004122F0`, `LoadTgaToFrontendSurface16bpp @ 0x004125B0`, `LoadTgaToFrontendSurface16bppVariant @ 0x004127B0`) all read the same 4 globals (`DAT_004951d8`, `DAT_00495250`, `DAT_00495230`, `DAT_0049525c`) and write them into the `Image_exref` DXIMAGELINE struct fields `+0x30/+0x34/+0x38` before calling `DX::ImageProTGA`. `InitializeFrontendResourcesAndState @ 0x00414740` (line `0x00414841..0x004148A6`) sets them from a 3×3 mask LUT at `0x0046563c..0x0046565f` based on the DDraw pixel format (15-bit / 16-bit / 24+-bit). Plus the failure latch `DAT_004962a4` (15 writers) and the frontend-init-complete latch pair `DAT_00495244` + `DAT_004962b8`.

2. **Weather particle pool at `0x004C3DAC` area** — partially named in T2.7 / T2.9 (view0 entries) but view1 companions remained `DAT_004c3db0`, `DAT_004c3dbc`, `DAT_004c3dc0`, etc. Named below; these are the view1 slots of existing per-view 2-element arrays. `InitializeWeatherOverlayParticles @ 0x00446240`, `UpdateAmbientParticleDensityForSegment @ 0x004464B0`, `RenderAmbientParticleStreaks @ 0x00446560` all index them as `(&g_weatherParticleBufferPerView)[view]` / `(&g_weatherSegmentTargetDensity + view*4)` etc.

3. **Per-slot lighting state at `0x004C38B8/C4`** — **MISIDENTIFIED IN T5.25**. `listing_code_unit_containing` shows 0x004C38B8 is already `g_trackedVehicleAudioFadeLevel` and 0x004C38C4 is `g_listenerPrevFramePositionByView_Y`. Both named in T2.9 audio sweep. No work needed.

4. **FF per-player state block at `0x004A2CC4`** — **ALREADY NAMED**. 0x004A2CC4 is `g_ffControllerAssignment[6]` (named in T3.14) and 0x004A2CDC is `g_ffCollisionEffectActive[6]` (also named). All 12 references resolve to those two arrays. The actual lighting cluster `s_light_dir[]` lives at 0x00467338 (T2.9) and `ApplyTrackLightingForVehicleSegment @ 0x00430150` writes to its own scratch at 0x004C38A0 — but that's only one DAT and is a transient stack-projection buffer.

## Proposals (globals)

### TGA-decode config (mask register block + failure/init latches)

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x004951d8 | u32 | `g_tgaDecodeRedMask` | **high** | Written at `0x0041485f`, `0x0041487d`, `0x0041489b` in `InitializeFrontendResourcesAndState` — sourced from `DAT_0046563c` (RGB555: 0x7C00), `DAT_00465648` (RGB565: 0xF800), or `DAT_00465654` (BGR888: 0x00FF0000) per the DDraw pixel-format check `(dd_exref + 0x16a0) == 0xf/0x10/else`. Read in all 4 `LoadTga*FromArchive` variants and written into the DXIMAGELINE struct (`Image_exref + 0x30`) before `DX::ImageProTGA`. | (port does its own TGA decode via PNG; no equivalent) |
| 0x00495250 | u32 | `g_tgaDecodeGreenMask` | **high** | Companion to the red mask. Sources: `DAT_00465640` (RGB555: 0x03E0), `DAT_0046564c` (RGB565: 0x07E0), `DAT_00465658` (BGR888: 0x0000FF00). Written into `Image_exref + 0x34`. 3 writers + 4 readers. | (none) |
| 0x00495230 | u32 | `g_tgaDecodeBlueMask` | **high** | Companion to red/green. Sources: `DAT_00465644` (RGB555: 0x001F), `DAT_00465650` (RGB565: 0x001F), `DAT_0046565c` (BGR888: 0x000000FF). Written into `Image_exref + 0x38`. 1 writer + 4 readers. | (none) |
| 0x0049525c | u32 | `g_frontendSurfaceBitDepth` | **high** | Seeded once in `InitializeFrontendResourcesAndState` (`0x00414841`) from `*(int *)(dd_exref + 0x16a0)` (the DDraw `DDSURFACEDESC.ddpfPixelFormat.dwRGBBitCount`). Read in 24 sites that gate blit/fade/dither paths on 15-bit vs 16-bit vs 24-bit modes (e.g. `Copy16BitSurfaceRect`, fade-effect path, `CreateMenuStringLabelSurface`). 25 xrefs total. **Already flagged in batch 22 out-of-scope (T5.22 line 222) — confirming here.** | (port uses BGRA8 unconditionally) |
| 0x0046563c | u32[9] | `g_tgaDecodeMaskLut` | **high** | 36-byte static R/G/B mask triple-row LUT indexed by pixel-format: row 0 = 15-bit RGB555 (`0x7C00,0x03E0,0x001F`), row 1 = 16-bit RGB565 (`0xF800,0x07E0,0x001F`), row 2 = 24/32-bit BGR888 (`0x00FF0000,0x0000FF00,0x000000FF`). Source data for the 3 mask registers above. | (port baked into shader) |
| 0x004962a4 | u8 | `g_tgaLoadFailureLatch` | **high** | Set to 1 in 15 distinct error paths across all 4 `LoadTga*FromArchive` variants (`Cannot_Open`, `Cannot_Alloc_Conversion_Buffer`, `Lock Failed`). Write-only — never explicitly reset within the TGA family. Single-bit indicator for "at least one frontend TGA failed to load this session". Likely consulted by a launcher error path that wasn't traced here. | (port uses zlib/PNG errors propagated up the call chain; no latch) |
| 0x00495244 | u32 | `g_frontendInitInProgressLatch` | **high** | One-shot latch: set to 1 at the top of `InitializeFrontendResourcesAndState @ 0x004147AB`; checked by `InitializeFrontendDisplayModeState @ 0x00414AD4` (`if (DAT_00495244 == 1) { ReleaseTrackedFrontendSurfaces(); g_startRaceConfirmFlag = 1; DAT_00495244 = 0; }`). Gates the post-init surface-release + race-confirm transition so it fires exactly once per frontend bootstrap. | (none) |
| 0x004962b8 | u32 | `g_attractCdTrackPickDone` | medium | Set to 0 at the top of `InitializeFrontendResourcesAndState`, then checked at the bottom: `if (DAT_004962b8 == 0) { DAT_004962b8 = 1; do { _rand...g_selectedCdTrackIndex... } while (...); }`. Also reset to 0 inside `InitializeFrontendDisplayModeState @ 0x00414AF6` alongside `g_frontendInitInProgressLatch`. One-shot guard that prevents the attract-mode CD-track randomization from re-running on every frontend re-init. | (port doesn't ship the attract-mode CD playback) |

### Weather View1 companion slots (existing arrays — covered by `(&...)[view]` indexing)

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x004c3db0 | void * | `g_weatherParticleBufferPerView_View1` | **high** | Companion to `g_weatherParticleBufferPerView @ 0x004c3dac` (view0). Written `HeapAllocTracked(0x6400)` at `0x00446293` (rain) and `0x0044640c` (snow) in `InitializeWeatherOverlayParticles`; read indirectly via `(&g_weatherParticleBufferPerView)[view]` in `UpdateAmbientParticleDensityForSegment` and `RenderAmbientParticleStreaks`. The init loop continues `while ((int)puVar3 < 0x4c3db4)` — i.e. covers `0x4c3dac..0x4c3db3` = 2 view slots. | (port doesn't use 2-view splitscreen) |
| 0x004c3dbc | float | `g_weatherStreakPrevCameraPosView0_Y` | medium | View0 .y component of `g_weatherStreakPrevCameraPos_PROVISIONAL` (which is just .x at 0x004c3db8). `RenderAmbientParticleStreaks @ 0x00446560` reads/writes `(&g_weatherStreakPrevCameraPos + view*0xc)` for x, `(&DAT_004c3dbc + view*0xc)` for y, `(&DAT_004c3dc0 + view*0xc)` for z. Cluster is 2×(float[3]) = 24 bytes (`0x004c3db8..0x004c3dcf`). | (none) |
| 0x004c3dc0 | float | `g_weatherStreakPrevCameraPosView0_Z` | medium | Companion to the .y above. | (none) |
| 0x004c3dc4 | float[3] | `g_weatherStreakPrevCameraPosView1` | medium | View1 of the .x/.y/.z trio. Indexed via `iVar10 = view * 0xc`. | (none) |
| 0x004c3dd4 | float | `g_weatherStreakParam2CacheView1_PROVISIONAL` | medium | View1 of the param-2 cache (`g_weatherStreakParam2CachePerView_PROVISIONAL @ 0x004c3dd0`). Used by `RenderAmbientParticleStreaks` to cache the per-view streak-blend factor while the freeze overlay is active. | (none) |
| 0x004c3ddc | i32 | `g_weatherSegmentTargetDensityView1` | medium | View1 of `g_weatherSegmentTargetDensity @ 0x004c3dd8`. Read/written as `*(int *)(&g_weatherSegmentTargetDensity + view*4)` in `UpdateAmbientParticleDensityForSegment`. | (none) |
| 0x004c3de4 | dword | `g_weatherActiveCountView1` | medium | View1 of `g_weatherActiveCountView0 @ 0x004c3de0`. Read/written as `(&g_weatherActiveCountView0)[view]` in `UpdateAmbientParticleDensityForSegment`, `UpdateVehicleAudioMix`, and `RenderAmbientParticleStreaks`. | (none) |

## Key discoveries

- **The "Per-slot lighting state at 0x004C38B8/C4" cluster identified by T5.25 is a misidentification.** Both addresses are already named: `g_trackedVehicleAudioFadeLevel @ 0x004C38B8` and `g_listenerPrevFramePositionByView_Y @ 0x004C38C4`. Both were named during the T2.9 audio sweep. There is no per-slot lighting state at those addresses. The actual track-lighting writer `ApplyTrackLightingForVehicleSegment @ 0x00430150` writes its scratch projection vectors to `0x004C38A0..0x004C38AF`, but those are just function-local stack-projection sentries — not a per-slot persistent state.
- **The "FF per-player state block at 0x004A2CC4" cluster is fully covered by existing names.** `g_ffControllerAssignment[6] @ 0x004A2CC4` (T3.14) is the 0-based joystick assignment table (0 = disabled, else `jsidx+1`), and `g_ffCollisionEffectActive[6] @ 0x004A2CDC` is the per-slot collision-FF-active byte. Together they're 48 bytes covering `0x004A2CC4..0x004A2CF3` — no holes, no orphans.
- **TGA decode uses a 3×3 RGB-mask LUT at 0x0046563c** (15-bit RGB555 / 16-bit RGB565 / 24-bit BGR888 mask triples). The 3 mask registers (`g_tgaDecodeRedMask/GreenMask/BlueMask`) are seeded once at frontend init from this LUT based on the DDraw pixel format, then re-read by every TGA load for the rest of the session. This explains why all 4 `LoadTga*FromArchive` variants share the exact same mask-write pattern — it's a single configuration block, not per-call.
- **`g_tgaLoadFailureLatch @ 0x004962a4` is write-only across the 15 error paths.** I could not find a reader within the TGA family. Likely consulted by a launcher-side error message dispatcher that wasn't traced here; flagged for future investigation. Could also be a dead diagnostic (sticky bit that never gets read in shipping).
- **`g_frontendInitInProgressLatch` + `g_attractCdTrackPickDone` form a paired re-entry guard.** Together they ensure `InitializeFrontendResourcesAndState`'s `attract CD track` randomization and `InitializeFrontendDisplayModeState`'s `surface-release + race-confirm` transition each fire exactly once per frontend re-entry, even though both functions can be called multiple times during option-screen navigation. Useful for understanding any race-results→main-menu transitions in the port.

## Out-of-scope finds

None remaining from the original T5.25 list. All 4 straggler clusters have been resolved: 2 named, 2 confirmed-already-named.

| address | brief note | probable area |
|---|---|---|
| 0x004c376c | 1-byte flag written by both `LoadVehicleSoundBank` and `UpdateVehicleAudioMix` (audio-mix init sentinel; never read in those functions) | T2.9 audio re-sweep (long-tail orphan, very low priority) |
| Image_exref + 0x30/+0x34/+0x38 layout | DXIMAGELINE struct field offsets for R/G/B masks — likely DDraw-era external linkage | structural promotion candidate if Image_exref is brought into the project |

## TODO impact

No active TODOs in scope. The Newcastle wall TODO (`todo_newcastle_span_216_invisible_wall_2026-05-19`) was not advanced by this batch — TGA loaders are pure asset-decode and don't touch span/wall geometry. Weather View1 companions are visual-only and similarly orthogonal to wall collision.
