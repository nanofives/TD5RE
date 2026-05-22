---
batch: 18
area: track_data_parsers
tier: T4
target_todos: [todo_precise_port_extend_geometry_render_2026-05-17, todo_newcastle_span_216_invisible_wall_2026-05-19, reference_geometry_render_audit_2026-05-17]
ghidra_session: d34e6e5fac5140b8b4dff7eb8f785e65
analyzed_addresses: 0x0042FAD0, 0x0042FB90, 0x0042FB40, 0x00431190, 0x00431260, 0x00444070, 0x00442670, 0x00442560, 0x0042F990, 0x00430150, 0x0043B0A0, 0x00443FB0, 0x00435940, 0x00446240, 0x0040CBD0, 0x0040CD10
agent: Claude Opus 4.7 (1M) — T4.18
date: 2026-05-20
---

# Globals enumeration — Track data parsers (STRIP / LEVELINF / LIGHT / MODELS)

## Summary

- Functions analyzed: 16 (5 primary parser entry points + 11 reachable producers/consumers)
- Unnamed DAT_* globals encountered: 17 (after de-dup)
- Already-named globals encountered: 22 (`g_trackStripRecords`, `g_trackVertexPool`, `g_trackTotalSpanCount`, `g_trackStripAttributeBasePtr`, `g_trackEnvironmentConfig`, `g_trackStartSpanIndex`, `gTrackIsCircuit`, `gCircuitLapCount`, `g_trackLightingZoneTablePtr`, `g_raceCheckpointTablePtr`, `gReverseTrackDirection`, `g_trackPoolIndex`, `g_trackTextureIndex`, `g_trackLevelZipPathBuffer`, `gModelsDatEntryCount`, `gModelsDatEntryTable`, `gSkyMeshResource`, `gTrackTextureCount`, `gStaticHedEntryArray`, `gStaticHedEntryCount`, `gStaticHedTextureData`, `gTracksideCameraProfiles`)
- Proposals — high confidence: 13
- Proposals — medium confidence: 5
- Proposals — comment-only (low confidence): 2
- RENAME proposals (existing names misleading): 1 — `g_trackStripAttributeBasePtr` is **not** a pointer
- DEFERRED-NOT-APPLIED carryovers from prior batches: 7 (T2.9 + batch_10 names confirmed present in source but not yet on binary)

## Methodology

Entry points: `InitializeTrackStripMetadata @ 0x0042FAD0` (track environment metadata header), `LoadTrackRuntimeData @ 0x0042FB90` (STRIP/LEFT/RIGHT/TRAFFIC.BUS/LEVELINF/CHECKPT loader), `ApplyTrackStripAttributeOverrides @ 0x0042FB40` (per-track strip-attribute table applier), `ParseModelsDat @ 0x00431190` (MODELS.DAT relocator + display-list builder), `BindTrackStripRuntimePointers @ 0x00444070` (STRIP.DAT header binder). From these I walked one level into consumers (`InitializeRaceSession`, `LoadEnvironmentTexturePages`, `LoadTrackTextureSet`, `LoadStaticTrackTextureHeader`, `ApplyTrackLightingForVehicleSegment`, `ConfigureActorProjectionEffect`, `UpdateActorTrackLightState`, `InitializeMinimapLayout`, `InitializeTrafficActorsFromQueue`, `NormalizeActorTrackWrapState`, `InitializeWeatherOverlayParticles`, `GetTrackSpanDisplayListEntry`) to recover header offsets and writer/reader sites for every track-archive global. Four structural insights drove the relevance gate:

1. **STRIP.DAT header is a 5-dword block** at the start of the loaded blob. `BindTrackStripRuntimePointers @ 0x00444070` extracts [0]=strip_records_offset, [1]=total_span_count, [2]=vertex_pool_offset, [3]=unknown_offset (write-only, see Key Discovery 4), [4]=strip_attribute_span_count (NOT a pointer — see RENAME below). The blob itself, after binding, is held both at `DAT_004aed90` and at the alias `DAT_004c3da0` which is later read as a JUNCTION-TABLE HEADER (+0x14 count, +0x1c entries, stride 6 bytes).
2. **MODELS.DAT top-level entry table is span-indexed via `gModelsDatEntryTable[span * 8]`.** Each top-level entry is an 8-byte pair (two dwords). Only the FIRST dword is consumed at runtime (by `GetTrackSpanDisplayListEntry @ 0x00431260`). The second dword has no known reader. This is the **per-span display-list dispatch table**.
3. **Track environment metadata blob (the `.STR` file)** lives at `DAT_004aee10` (NOT the LEVELINF.DAT — that is `g_trackEnvironmentConfig` at 0x4aee20). The `.STR` is loaded by `InitializeTrackStripMetadata @ 0x0042FAD0` from a per-track-pool address table at `DAT_0046bb1c` (24 dwords). Its layout: +0x00=env_texture_page_count, +0x04..+0x10=projection-effect-index array, +0x14..+0x20=projection-mode array (3=sun, 1=else, written by the metadata initializer), +0x40..+0xc0=texture-name slots (stride 0x20, 4 entries).
4. **All five per-track archives are loaded sequentially by `LoadTrackRuntimeData`** into a contiguous globals cluster at 0x4aed8c..0x4aee1c: TRAFFIC.BUS, STRIP.DAT, LEFT.TRK, CHECKPT.NUM[0x60], (6-dword checkpoint copy area), LEVELINF.DAT, gTracksideCameraProfiles, RIGHT.TRK. Three of the buffer pointers (`DAT_004aed8c`, `DAT_004aed90`, `DAT_004aed94`, `DAT_004aee1c`) match the batch_10 T2 proposals and remain unnamed in the binary.

## Proposals

### STRIP.DAT runtime archive pointers (deferred carryover from batch_10 + new finds)

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x004aed90 | void* | `g_trackStripDataBlob` | high | `LoadTrackRuntimeData @ 0x0042FBD0` allocates STRIP.DAT here via `HeapAllocTracked`; consumed by `BindTrackStripRuntimePointers @ 0x00444070` which extracts [0]=strip_records, [1]=total_span_count, [2]=vertex_pool, [3]=(written to DAT_004c3d8c), [4]=strip_attr_span_count. **Deferred from batch_10**. | td5_track.c STRIP.DAT loader |
| 0x004aed94 | void* | `g_leftRouteTableBlob` | high | `LoadTrackRuntimeData @ 0x0042FC0B` allocates LEFT.TRK here (`&PTR_s_LEFT_TRK_004673b8`); seeds `g_activeRouteTablePtrA_left` (0x004afb58) via `InitializeRaceActorRuntime`. **Deferred from batch_10**. | td5_ai.c LEFT.TRK buffer |
| 0x004aee1c | void* | `g_rightRouteTableBlob` | high | `LoadTrackRuntimeData @ 0x0042FC44` allocates RIGHT.TRK (`&PTR_s_RIGHT_TRK_004673bc`); seeds `g_activeRouteTablePtrB_right` (0x004b08b4). **Deferred from batch_10**. | td5_ai.c RIGHT.TRK buffer |
| 0x004aed8c | void* | `g_trafficBusTableBlob` | high | `LoadTrackRuntimeData @ 0x0042FC79` allocates TRAFFIC.BUS (`&PTR_s_TRAFFIC_BUS_004673c0`); seeds `g_activeTrafficBusCursor` (0x004b08b8). **Deferred from batch_10**. | td5_ai.c TRAFFIC.BUS table |
| 0x004aedb0 | byte[0x60] | `g_checkpointNumTable` | med | `LoadTrackRuntimeData @ 0x0042FBFA` reads CHECKPT.NUM (0x60 bytes) directly into this address. Referenced in `todo_drag_checkpt_num_sentinel`. **Deferred from batch_10**. | td5_track.c CHECKPT.NUM |
| 0x004aed98 | int[6] | `g_raceCheckpointTableLive` | high | `LoadTrackRuntimeData @ 0x0042FD3E` copies 6 dwords from `&DAT_0046cf6c + track*4`-pointed array into this address; then assigned to `g_raceCheckpointTablePtr`. Back-store for checkpoint table. **Deferred from batch_10**. | (none) |

### Track environment metadata (`.STR` file at DAT_004aee10)

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x004aee10 | void* | `g_trackEnvironmentMetadataBlob` | **high** | `InitializeTrackStripMetadata @ 0x0042FADD` writes `DAT_004aee10 = (&DAT_0046bb1c)[track_pool * 4]` (per-track environment-metadata pointer). Read by `LoadEnvironmentTexturePages @ 0x0042F990` (texture page count at +0, names at +0x40 stride 0x20), `ConfigureActorProjectionEffect @ 0x0040CBE2` (mode codes at +0x4 and +0x14). Distinct from `g_trackEnvironmentConfig` (LEVELINF.DAT) at 0x4aee20. 11 xrefs. | td5_track.c per-track .STR metadata |
| 0x0046bb1c | u32 ptr[24] | `g_perTrackEnvironmentMetadataPtrs` | **high** | Indexed once in `InitializeTrackStripMetadata @ 0x0042FAD4` as `(&DAT_0046bb1c)[param_1 * 4]` (stride 16 = 4 dwords per entry, but only the first dword is read — apparent 16-byte alignment of a pointer-table). First entry at idx 0 is NULL (idx 0 reserved), idx 1..23 hold metadata pointers per track pool. | td5_track.c track-pool→.STR-blob lookup |
| 0x0046cb10 | void* ptr[24] | `g_perTrackStripAttributeOverrides` | high | Indexed in `ApplyTrackStripAttributeOverrides @ 0x0042FB5A` as `(&PTR_DAT_0046cb10)[track_pool-1]` (stride 4); each entry is a pointer to a packed (strip_index, attribute_byte) override list terminated by reaching the supplied default count. 24 dwords array. | td5_track.c per-track strip attribute override tables |

### MODELS.DAT parser globals

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x004aee54 | void* | `g_modelsDatPostEntryTableBlob` | med | `ParseModelsDat @ 0x0043119E` writes `_DAT_004aee54 = gModelsDatEntryTable + gModelsDatEntryCount * 2` (offset to whatever follows the 8-byte entry-pair table in MODELS.DAT). **Write-only globally — no known reader.** Likely an unused header trailer pointer or reserved future hook. | td5_render.c MODELS.DAT trailer (currently unused) |

### STRIP.DAT decoded header fields (new finds + RENAME candidate)

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x004c3da0 | void* | `g_trackStripBlobAlias` | **high** | `BindTrackStripRuntimePointers @ 0x00444075` writes `DAT_004c3da0 = DAT_004aed90` (alias of strip-blob ptr). Read by `InitializeTrafficActorsFromQueue @ 0x004359D8` as the **junction-table header**: `*(int *)(iVar10 + 0x14)` = junction count; `(ushort *)(iVar10 + 0x1c)` = junction entries (stride 6 bytes = 3 ushorts per entry). Multiple traffic-recycle/route-walker reads at 0x00435ade, 0x00436adb, 0x00436c51, 0x004351b3, 0x004351fa, 0x00436020, 0x00436083, 0x004361bc, 0x00436215, 0x004354b5, 0x004355e2, 0x0043691b, 0x00436966, 0x00443fff. 16 xrefs. **Deferred from batch_10.** | td5_track.c strip-blob alias (junction-table holder) |
| 0x004c3d94 | int | `g_trackStripValidSpanCount` (RENAME from `g_trackStripAttributeBasePtr`) | **high** | **RENAME**: existing name suggests "pointer" but usage is unambiguously a count. Three reads: `InitializeRaceSession @ 0x0042AEDD` passes it as `param_2` to `ApplyTrackStripAttributeOverrides` (used as `iVar1 < (int)param_2` loop bound iterating strip records); `InitializeMinimapLayout @ 0x0043B686, 0x0043B71F` uses it as `0 < g_trackStripAttributeBasePtr` and `iVar3 < g_trackStripAttributeBasePtr` (strip-record loop bound). Sourced from `DAT_004aed90[4]` (STRIP header word 4) — the number of strip records that have valid attribute overrides. Companion to `g_trackTotalSpanCount` at 0x004c3d90. | td5_track.c valid strip count (loop bound) |
| 0x004c3d8c | int | `g_trackStripHeaderField3` | low | `BindTrackStripRuntimePointers @ 0x004440A5` writes `DAT_004c3d8c = DAT_004aed90[3]` (STRIP header word 3). **Write-only globally — no known reader.** Header field is parsed but never consumed; comment-only flag. Possibly junction-table offset that was superseded by the +0x1c walker inside the strip-blob alias path. | (none — vestigial/debug header field) |

### Lighting subsystem carryover (deferred from batch_09 / T2.9 — names not applied)

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x00469c78 | void* ptr[24] | `g_perTrackLightZoneTables` | high | `LoadTrackRuntimeData @ 0x0042FD07` indexes as `(&DAT_00469c78)[track]` to write `g_trackLightingZoneTablePtr`. Stride-4 array of 24 dwords. Idx 0 = NULL, idx 1..23 = per-track lighting-zone table pointers. **Deferred from batch_09 (T2.9 named in markdown but not applied to binary).** | td5_light_zones_table.inc (port flattens this 24×ptr table) |
| 0x004c38a0 | int[16] | `g_perSlotActiveLightZoneTexturePage` | high | `ApplyTrackLightingForVehicleSegment @ 0x004301E2` writes `*(undefined4 *)(&DAT_004c38a0 + slot * 4) = psVar9[0x10]` (texture-page id from current light-zone row, indexed by `slot_index`). Per-slot cached texture page id used by the render-time lighting blend. 1 xref but per-slot stride 4. | td5_render.c per-slot light texture page |

### Per-track lookup tables (config + camera + lighting indexed by track pool)

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x00473820 | int[24][2] | `g_perTrackCameraConfigPair` | med | Two interleaved per-track configs (stride 8 = 2 dwords each). `LoadTrackRuntimeData @ 0x0042FCFC` reads `(&DAT_00473820)[(track-1)*8]` into `DAT_00483954`. Companion lookup at +4 written to `DAT_00483550`. First dword visible content: 0x24, 0x22, 1, 0x18, 0x18, 0x24, 0x28, 0x1b, 0x24, ... — small enums or angle codes. | (none — per-track camera tuning constants) |
| 0x00483550 | u32 | `g_currentTrackCameraConfigA` | low | Twin sink with 0x00483954; written by `LoadTrackRuntimeData` from `&DAT_00473824 + (track-1)*8`. Out-of-scope consumer; comment-flag only. | (none) |
| 0x00483954 | u32 | `g_currentTrackCameraConfigB` | low | Twin sink with 0x00483550; written by `LoadTrackRuntimeData` from `&DAT_00473820 + (track-1)*8`. Out-of-scope consumer; comment-flag only. | (none) |
| 0x00473780 | void* ptr[24] | `g_perTrackTracksideCameraProfilePtrs` | high | `LoadTrackRuntimeData @ 0x0042FD52` writes `gTracksideCameraProfiles = (&PTR_DAT_00473780)[track-1]`. 24-entry pointer table, each pointing to a per-track trackside cam profile blob (starts at 0x46d010). | td5_camera.c trackside profiles |
| 0x0046cf6c | int* ptr[24] | `g_perTrackCheckpointTableSourcePtrs` | high | `LoadTrackRuntimeData @ 0x0042FD2A` indexes as `(&DAT_0046cf6c)[track]` → pointer to source array; then copies 6 dwords from there into `g_raceCheckpointTableLive` @ 0x4aed98. | td5_track.c per-track checkpoint table source |

### Static texture & projection-effect flags

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x004c3cf4 | int | `g_dualTextureSetSecondHalfBase` | med | `LoadTrackTextureSet @ 0x00442702` writes `DAT_004c3cf4 = gTrackTextureCount - 4` only when `g_vehicleProjectionEffectMode == 2`. Read by `ConfigureActorProjectionEffect @ 0x0040CBF7` as base offset added to the projection-effect index when in mode 2. Holds the second-set offset for the dual-page texture layout. | td5_render.c dual texture base offset |
| 0x004c3d04 | int | `g_textureUsesTallPageFormat` | med | Read at 7 sites in `LoadStaticTrackTextureHeader`, `BuildTrackTextureCache`, `LoadTrackTextureSet`, and other texture upload paths. Controls whether (width, height) are read directly from static.hed (when nonzero) or swapped (when zero — `if uVar1 <= uVar6 then uVar6 = uVar1`). **Never written via instruction — always 0 in this binary.** Likely an artifact of a removed alternate page layout mode. | (none) |

## Key discoveries

1. **STRIP.DAT header is a 5-dword block at the start of the loaded blob, NOT a separate header file.** `BindTrackStripRuntimePointers @ 0x00444070` decodes:
   - `[0]` → `strip_records_offset` (rebased to `g_trackStripRecords @ 0x004c3d9c`)
   - `[1]` → `total_span_count` (`g_trackTotalSpanCount @ 0x004c3d90`)
   - `[2]` → `vertex_pool_offset` (rebased to `g_trackVertexPool @ 0x004c3d98`)
   - `[3]` → **write-only** sink at `DAT_004c3d8c` (vestigial)
   - `[4]` → `strip_attribute_valid_span_count` (currently misnamed `g_trackStripAttributeBasePtr @ 0x004c3d94` — see RENAME below)

   The strip-record stride is 24 bytes (0x18). Records contain: +0 type byte (1/2/3/4/5/6/7/8/11/...), +1 attribute byte (default written by `ApplyTrackStripAttributeOverrides`), +3 high-nibble lane count, +4 first_vertex_index (ushort), +6 second_vertex_index (ushort), +0xa span-count short (final record gets `total_span_count - 1` patched in), +0xc..+0x14 (x,y,z) origin offsets, +0x18 next record. Type 8 = junction-start, type 11 = junction-end (per `InitializeMinimapLayout` walker).

2. **`g_trackStripAttributeBasePtr @ 0x004c3d94` is misnamed — it's a COUNT, not a pointer.** Three independent call sites use it as a loop bound (`0 < g_trackStripAttributeBasePtr`, `iVar3 < g_trackStripAttributeBasePtr`, as `param_2` to `ApplyTrackStripAttributeOverrides` which iterates while `iVar1 < (int)param_2`). The value is sourced from `DAT_004aed90[4]` (STRIP header word 4) and represents the count of strip records with valid attribute overrides — a subset of `g_trackTotalSpanCount`. **RENAME proposal**: `g_trackStripValidSpanCount` (or `g_trackStripAttributeSpanCount`). This rename eliminates a confusing dereference pattern in any reader that treats it as `*(int *)g_trackStripAttributeBasePtr`.

3. **`g_trackStripBlobAlias @ 0x004c3da0` IS the junction-table holder for forks/branches.** `InitializeTrafficActorsFromQueue @ 0x004359D8` and (per batch_10) many other route-walker functions read `*(int *)(iVar10 + 0x14)` as **junction count** and `(ushort *)(iVar10 + 0x1c)` as **junction entry array** (stride 6 bytes = 3 ushorts). Junction entry structure (per the if-condition at 0x004359F4):
   - `entry[0]` = target_span_index (ushort)
   - `entry[-1]` = exit_distance_lower_bound (ushort, accessed as `puVar11[-1]`)
   - `entry[-2]` = exit_distance_upper_bound (ushort, accessed as `puVar11[-2]`)

   The check is `entry.lo <= cur_span <= (entry.lo - entry.up - 1 + entry.hi)`. When matched, the traffic recycler swaps span via `(*entry-vertex-offset + cur_span_y)` heading-vertex lookup. **This is the engine's branch/fork detector** — directly relevant to `todo_newcastle_span_216_invisible_wall_2026-05-19` (see TODO impact #2 below).

4. **MODELS.DAT is span-indexed: `gModelsDatEntryTable[span * 8]` returns the display-list pointer for that span.** Confirmed by `GetTrackSpanDisplayListEntry @ 0x00431260`:
   ```c
   undefined4 GetTrackSpanDisplayListEntry(int param_1) {
       return *(undefined4 *)(gModelsDatEntryTable + param_1 * 8);
   }
   ```
   Called only from `RunRaceFrame @ 0x0042B580`. The 8-byte stride means each top-level entry is a PAIR of dwords, but only the FIRST dword (the display-list pointer) is consumed at runtime. The second dword is read once (by `ParseModelsDat` itself for the relocator) but never accessed afterward. **The MODELS.DAT relocator at `ParseModelsDat @ 0x00431190` rebases ONLY the first-dword pointer in each pair**, leaving the second-dword unrebased and unread. The port should preserve this asymmetry: any "approximated" port-side code that touches both dwords symmetrically is divergent.

5. **Track environment metadata blob (`DAT_004aee10`, the `.STR` file) is DISTINCT from LEVELINF.DAT (`g_trackEnvironmentConfig @ 0x4aee20`).** The `.STR` file is loaded by `InitializeTrackStripMetadata @ 0x0042FAD0` from a separate per-track-pool address table at `DAT_0046bb1c` (24 dwords). Its primary role is the **per-track texture-page mode table**:
   - At offset 0x40 + i*0x20 (for i=0..3): the texture file name (8 chars, stride 0x20)
   - At offset 0x14 + i*0x4 (for i=0..3): a mode code written by the metadata initializer (3 = "sun"/sky pixel, 1 = standard) based on `strnicmp(name, "sun", 3)`
   - At offset 0x4 + i*0x4 (for i=0..3): a 4-byte projection-effect-index value read at 0x0040CBE2

   Total of 4 environment texture pages per track. The mode codes drive `ConfigureActorProjectionEffect`'s sky-vs-normal branch.

6. **`DAT_004c3d8c` is write-only globally — STRIP.DAT header word [3] is parsed but never consumed.** `BindTrackStripRuntimePointers @ 0x004440A5` writes `DAT_004c3d8c = DAT_004aed90[3]` but no other instruction reads from this address. Likely a vestigial header field (possibly an older junction-table offset that was superseded by the `+0x1c` walker inside the strip-blob alias). The port can safely omit this field without risk.

7. **`DAT_004c3d04` (`g_textureUsesTallPageFormat`) is read at 7 sites but never written via instruction.** Initial-value-only flag, always 0 in this binary. Controls a (width, height) swap heuristic in `LoadStaticTrackTextureHeader @ 0x004425D9`: when 0, the loader takes `min(prev, cur)` and `max(prev, cur)` as the new dimensions; when nonzero, the loader takes `cur` directly. Likely an artifact of a removed alternate page layout mode. The port can hardcode the 0-path.

8. **Per-track environment-metadata pointer table at `DAT_0046bb1c` uses stride-16 (4-dword) entries but only the FIRST dword is consumed.** Indexed in `InitializeTrackStripMetadata @ 0x0042FAD4` as `(&DAT_0046bb1c)[track_pool * 4]`. This is the same 4-byte-pointer-with-12-byte-padding pattern that historically suggests the original layout reserved 16 bytes per track for richer metadata that was reduced to a single pointer late in development. Cross-reference: `gScheduleToPoolIndex` (per batch_07) translates schedule index → track pool index, then track pool index indexes this metadata table.

## Out-of-scope finds

| address | brief note | probable area |
|---|---|---|
| 0x00466d50 | `gTrackPoolSpanCountTable` — name is misleading; actually a 24-entry int table mapping schedule index → track pool index | T3 schedule batch (revisit naming) |
| 0x00466e3c | `gTrackPoolReverseSpanCountTable` — same caveat; reverse-direction schedule→pool mapping with -1 sentinels for tracks without reverse | T3 schedule batch |
| 0x00466d98 | `gTrackModelVariantCountTable` — name correct; 24-entry int table of MODELS.DAT lighting-bias adjustment values per track | T3 models batch |
| 0x00466f6f | `gTrackPoolStartSpanTable` — byte table of start-span indices per track pool (24 entries) | T3 race-init batch |
| 0x004ad288 | `g_dragRaceLaneStripPtr` — drag race lane geometry block; 15 short writes at +0x40..+0x84 in `InitializeRaceActorRuntime` | T3 drag-mode batch (already deferred in batch_10) |
| 0x00473d2c | `g_defaultCarIdTable` — default car-id constant table (6 entries) | T3 frontend batch (already deferred in batch_10) |
| 0x004c38b8 | adjacent per-slot lighting state block neighbor — 4-byte field with 3 writers, 1 reader | T3 light-state batch |
| 0x004c38c4 | another per-slot lighting state neighbor — 1 reader, 1 writer | T3 light-state batch |
| 0x004c3dac, 0x004c3db0, 0x004c3da8 | weather particle pool pointers seeded by `InitializeWeatherOverlayParticles` | T3 vfx batch |
| 0x004c3ddc, 0x004c3dd8, 0x004c3de4, `g_weatherActiveCountView0` | weather particle counters | T3 vfx batch |
| 0x0046bb1c entry stride padding bytes (3 dwords per 16-byte slot) | unused header padding in per-track metadata pointer table | comment-only |

## TODO impact

### `todo_precise_port_extend_geometry_render_2026-05-17`

This batch directly supports the geometry-render audit:

1. **Confirms `gModelsDatEntryTable[span * 8]` as the per-span display-list dispatch (Key Discovery 4).** Any port-side approximation that maps span→display-list via a different lookup (e.g., nearest-neighbor heuristic, span-bucket-table rebuild, fallback path) is by definition divergent. Port should implement `td5_render_get_span_display_list(span)` as a direct array dereference of the relocated MODELS.DAT entry table at `[span * 8]`, returning the first dword unchanged. The second dword is unread and should be ignored (NOT zeroed or modified).

2. **Confirms the strip-blob alias (`g_trackStripBlobAlias @ 0x004c3da0`) is the junction-table holder (Key Discovery 3).** The junction count at +0x14 and 6-byte stride junction entries at +0x1c constitute the engine's branch/fork detector. The port's `rebuild_span_display_list_mapping` heuristic flagged in `reference_geometry_render_audit_2026-05-17` should consult this junction table directly rather than running its own nearest-neighbor pass. **Port action**: extract the junction-table reader from `InitializeTrafficActorsFromQueue @ 0x004359D8` (the `*(int *)(iVar10 + 0x14)` count and `(ushort *)(iVar10 + 0x1c)` walker pattern) into a shared `td5_track_find_junction_target(span)` helper used by both AI route logic and render-time span lookup.

3. **Identifies the 5-dword STRIP.DAT header layout (Key Discovery 1).** Port may already extract these but should verify field 3 (write-only in orig) is not consumed; field 4 (`g_trackStripAttributeBasePtr` RENAME) must be treated as a COUNT, not a pointer.

### `todo_newcastle_span_216_invisible_wall_2026-05-19`

**Headline finding**: The junction-table structure at `g_trackStripBlobAlias + 0x14` (count) and `+0x1c` (entries, stride 6 bytes = 3 ushorts) is the **most likely root cause site** for the invisible-wall regression at Newcastle span 216. Three concrete hypotheses (in priority order):

1. **Junction entry at span 216 may have a non-standard target_span/exit_distance triple that the port's geometry render skips.** If Newcastle's span 216 is at a fork or T-junction (or adjacent to one), the original engine resolves the next-display-list via `entry[0] = target_span` (per the matched-junction branch in `InitializeTrafficActorsFromQueue`), but a port that uses linear `span+1` lookup would dispatch the WRONG mesh, leaving the fork's "other branch" unrendered (= invisible wall to the player).

2. **Strip record at span 216 may be type 8 (junction-start) or type 11 (junction-end), which `InitializeMinimapLayout @ 0x0043B686` treats specially.** The port's render path needs the same type-8/type-11 dispatch — emit the junction's two divergent mesh chains rather than a single linear chain. Cross-reference: type 8 stores junction-anchor-index into the minimap's segment table while type 11 closes the anchor and emits a wrap-edge.

3. **Span 216 may be inside the junction's exit_distance range** (`entry[-1] <= span <= (entry[-1] - entry[-2] - 1 + entry[0])` per the matched-junction branch). The port's geometry render may skip these intermediate spans because they're "inside" a branch in the original's data structure but appear as linear spans in the port's MODELS.DAT relocator.

**Suggested investigation step**: hook `g_trackStripBlobAlias + 0x14` and `+0x1c` reads from Frida during a Newcastle race; dump the junction table for the loaded track and check whether span 216 is inside ANY junction entry's exit range. If yes, the port's render path is missing the type-8/type-11 dispatch. If no, the bug is elsewhere (e.g., model relocator skipping span 216's display-list pointer due to a NULL second-dword or some other off-by-one in `ParseModelsDat`).

### `reference_geometry_render_audit_2026-05-17`

This batch supplies the missing structural details to close audit items #1 (rebuild_span_display_list_mapping nearest-neighbor heuristic) and #3 (STRIP-generated fallback path): the port should consult `gModelsDatEntryTable[span * 8]` directly (Key Discovery 4) and the junction table at `g_trackStripBlobAlias + 0x1c` (Key Discovery 3) instead of running port-only heuristics. Item #2 (per-span 4× redundant submission) is unaffected by this batch — likely a render-loop structure issue rather than a parser-data issue.

## Ghidra session notes

- Session `d34e6e5fac5140b8b4dff7eb8f785e65` opened TD5_pool0 read-only as required by CLAUDE.md HARD RULE.
- Pool slot acquired via `bash scripts/ghidra_pool.sh acquire` (TD5_pool0); will be released via `bash scripts/ghidra_pool.sh cleanup` after deliverable write.
- No writes to Ghidra performed. Names listed here are PROPOSED only — the consolidation session will apply them.
- Two `T2.9 / batch_09 / batch_10` carryovers (`g_perTrackLightZoneTables @ 0x00469c78`, the 5 STRIP/route/traffic/checkpoint blob pointers at 0x004aed8c..0x004aedb0, and `g_trackStripBlobAlias @ 0x004c3da0`) were confirmed correct against the binary but remain unapplied — recommend prioritizing these in the next consolidation pass since they're high-confidence and unblock additional naming for T5+.
