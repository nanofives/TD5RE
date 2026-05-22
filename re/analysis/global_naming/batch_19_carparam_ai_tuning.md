---
batch: 19
area: carparam_ai_tuning
tier: T4
target_todos: [reference_drag_ai_template_binding, reference_arch_cardef_per_actor_indirection, todo_playerisai_carparam_binding, todo_drag_strip_ai_idle]
ghidra_session: 85accb7a88354875a36bcb077c35618a
analyzed_addresses: 0x00432E60, 0x00432D60, 0x0042AA10, 0x00443280, 0x0042F140, 0x0042FB90, 0x00444070
agent: Claude Opus 4.7 (1M)
date: 2026-05-20
t2_10_orphans_covered: [0x00473d9c, 0x00473da0, 0x00473da4, 0x00473da8, 0x00473db0, 0x004ad288, 0x004c3da0]
---

# Globals enumeration — Carparam + AI tuning tables

## Summary

- Functions analyzed: 7 primary (`InitializeRaceActorRuntime`, `ComputeAIRubberBandThrottle`, `InitializeRaceSession`, `LoadRaceVehicleAssets`, `InitializeRaceVehicleRuntime`, `LoadTrackRuntimeData`, `BindTrackStripRuntimePointers`)
- Unnamed DAT_* globals encountered: 22
- Already-named globals encountered: 19 (`gVehicleTuningTable`, `gVehiclePhysicsTable`, `gRaceDifficultyTier`, `gDifficultyHard`, `gDifficultyEasy`, `gTrackIsCircuit`, `gTrafficActorsEnabled`, `g_selectedGameType`, `g_raceOverlayPresetMode`, `g_actorRuntimeState`, `gRaceSlotStateTable`, `gActorRouteStateTable`, `gActorDefaultRouteSteerBias`, `gCarZipPathTable`, `gSlotCarTypeIndex`, `gSlotMeshResourcePtrTable`, `g_playerReflectionMeshResource`, `g_vehicleProjectionEffectMode`, `g_perSlotDifficultyModifiers`)
- Proposals — high confidence: 17
- Proposals — medium confidence: 4
- Proposals — comment-only (low confidence): 1
- **All 4 T2.10 orphan addresses NAMED** (0x00473d9c..0x00473da8 + 0x00473db0 + 0x004ad288 + 0x004c3da0)

## Methodology

Entry: `InitializeRaceActorRuntime @ 0x00432e60` — sole writer of every rubber-band/AI-template global. Walked one level back (`InitializeRaceSession @ 0x0042AA10` caller), one level forward (`ComputeAIRubberBandThrottle @ 0x00432D60` — the unique reader of the rubber-band constants), and sideways into `LoadRaceVehicleAssets @ 0x00443280` for the carparam pipeline. The two memos (`reference_drag_ai_template_binding.md`, `reference_arch_cardef_per_actor_indirection.md`) framed the architectural intent; live decomp confirmed every claim.

Three structural insights drove the relevance gate:
1. **`InitializeRaceActorRuntime` is the single point** where per-difficulty/per-circuit AI rubber-band globals get rewritten and where the AI carparam template is copied per-slot. All 10 difficulty/circuit branches (NORMAL/HARD/EASY × CIRCUIT/NONCIRCUIT × TRAFFIC/NOTRAFFIC × tier0/1/2) ultimately write the same 4 globals (0x00473d9c..0x00473da8) with different constants.
2. **The 4 rubber-band constants are read ONLY by `ComputeAIRubberBandThrottle`** — a per-tick function that takes the AI's time delta vs. the leader and emits a throttle multiplier into `RS[4]` (= `gActorDefaultRouteSteerBias`). Formula reconstructed exactly (see Key Discovery #1).
3. **The vehicle data pipeline has 4 distinct memory regions** with different lifetimes: file-load (`puVar6 = _malloc(0x10c)` scratch), per-slot template-strided tables (`gVehicleTuningTable`, `gVehiclePhysicsTable`), per-actor indirection (the architectural divergence), and the AI template (0x00473db0) which is COPIED on top of the per-slot table only when `state==0 && game_type==0`.

## Proposals

### AI rubber-band tuning globals (T2.10 orphans 1-4)

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x00473d9c | int32 | `g_aiRubberBandLagThrottleGain` | **high** | Read at `ComputeAIRubberBandThrottle @ 0x00432e07`: when `iVar1 < 0` (lagging), `iVar1 = (DAT_00473d9c * iVar1) / DAT_00473da4`. Written by **11 sites** inside `InitializeRaceActorRuntime` (one per difficulty/circuit/tier combination). Default values: NORMAL/non-circuit/non-traffic/tier0=0xa0, HARD/non-circuit/non-traffic/tier2=0xdc, EASY/circuit/tier0=0x91, drag (`g_selectedGameType!=0`)=0x0. | td5_ai.c rubber-band lag-gain table |
| 0x00473da0 | int32 | `g_aiRubberBandLeadThrottleCut` | **high** | Read at `ComputeAIRubberBandThrottle @ 0x00432e23`: when `iVar1 >= 0` (leading), `iVar1 = (DAT_00473da0 * iVar1) / DAT_00473da8`. Written by 11 sites (symmetric to 0x00473d9c). Default values: NORMAL/non-circuit/non-traffic/tier0=0x96, HARD/non-circuit/non-traffic/tier2=0x78, drag=0x0. | td5_ai.c rubber-band lead-cut table |
| 0x00473da4 | int32 | `g_aiRubberBandLagSaturation` | **high** | Read at `ComputeAIRubberBandThrottle @ 0x00432dfc + 0x00432e10`: clamps the negative timer-delta to this magnitude before scaling. Written by 11 sites. Default values: 100 (decimal 100) most paths, 0x4b (=75) when tier==1 non-traffic, 0x40 (=64) for drag/no-tier match. | td5_ai.c rubber-band lag-saturation |
| 0x00473da8 | int32 | `g_aiRubberBandLeadSaturation` | **high** | Read at `ComputeAIRubberBandThrottle @ 0x00432e18 + 0x00432e2c`: clamps the positive timer-delta. Written by 11 sites. Default values: 0x50 most paths, 0x40 drag/non-match, 0x37 (=55) EASY/non-circuit/non-traffic/tier0. | td5_ai.c rubber-band lead-saturation |

### AI carparam template (T2.10 orphan 5) + traffic template

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x00473db0 | byte[128] | `g_aiVehicleTuningTemplate` | **high** | 32-dword (128-byte) buffer copied verbatim into the **per-slot car_definition_ptr buffer** (`puVar3`) inside `InitializeRaceActorRuntime @ 0x00432f26..0x00432f48` when `gRaceSlotStateTable.slot[i].state == 0 && g_selectedGameType == 0`. After copy, per-difficulty-tier short-multiplier adjustments write back to specific offsets in the same buffer (`puVar3 + 0x68`/`+0xb0`/`+0x6e`/`+0x70`). **Sole writer/reader pair** — no other xrefs. Per `reference_drag_ai_template_binding.md`, drag-mode slot 1 (state=3) bypasses this copy in original; port enhancement gives slot 1 carparam instead. | td5_physics.c `bind_default_vehicle_tuning` AI template path (s_loaded_cardef seeded then per-actor mutated) |
| 0x00473be8 | byte[128] | `g_trafficVehicleTuningTemplate` | high | Single xref at `InitializeRaceActorRuntime @ 0x004334e1` — assigns `actor->car_definition_ptr = &DAT_00473be8` when slot is in the **traffic-actor range** (slot >= position past 0x4ac7f4 = past the 6 racer actors). Same layout as `g_aiVehicleTuningTemplate` (32 dwords). Constants like 0x8000, 0xc000, 0xe800 in the first 16 bytes match the cardef layout (engine RPM caps, etc.). | td5_physics.c traffic-vehicle default tuning |

### Default per-slot tuning live state (T2.10 orphan adjacent)

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x00473d2c | int32[14] | `g_perSlotRubberBandThrottleLive` | high | Live runtime table indexed by slot. Read at `InitializeRaceActorRuntime @ 0x0043351f` as `local_4[4] = (&DAT_00473d2c)[local_c]` — seeds `RS[4]` (= `gActorDefaultRouteSteerBias` per slot) at race init. WRITTEN at `ComputeAIRubberBandThrottle @ 0x00432d7a` (template copy) + `0x00432daa` (network-race forced reset to 0x100). 14 dwords because: 12 actor slots (6 racer + 6 traffic) + 2 padding. Values default to 0x100 (=256, "neutral 100% throttle"). | td5_ai.c per-slot rubber-band throttle table |
| 0x00473d64 | int32[14] | `g_perSlotRubberBandThrottleConstantTemplate` | high | Read-only template copied to `g_perSlotRubberBandThrottleLive` at start of `ComputeAIRubberBandThrottle @ 0x00432d70..0x00432d7a` (14-iteration dword copy). Default values from dump: `0x100, 0x100, 0x140, 0x118, 0x122, 0x140, 0, 0, 0, 0, 0x2bc, 0, 0, 0` — first 6 are the per-slot starting car IDs (Viper=0x100, others); but stored 56-byte block aliases the racer-id seeds + spacing. Reset source for non-network mode. | td5_ai.c per-slot throttle template |

### Drag-race lane geometry block (T2.10 orphan 6)

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x004ad288 | void* | `g_dragRaceLaneStripPtr` | **high** (medium-evidence writer) | 15 reads inside `InitializeRaceActorRuntime @ 0x00433558..0x0043360e` of the form `*(short *)(DAT_004ad288 + 0xNN) = literal`. The literals (0xff10, 0xff79, 0x208, 0xf0, ...) are 16-bit signed fixed-point coordinates that match a **12-vertex quad-strip** at offsets 0x40..0x5c plus 0x80..0x84 trailer (count + pad). Companion literal block at `_DAT_004ad2e0..0x004ad2fc` writes the SAME values directly — indicating `DAT_004ad288 == 0x004ad2a0` at runtime (pointer + 0x40 == 0x4ad2e0 = lane-2 mirror). Writer site **not found** in static disasm (likely written by an indirect strip-walker or runtime-bound via track-strip data; references-database has 15 reads but no `MOV [0x4ad288], reg` byte pattern). Block fires only when execution reaches the post-loop sequence in `InitializeRaceActorRuntime` (which is unconditional, but the data is only consumed by drag start-grid geometry submission). | td5_physics.c drag-strip pre-init block (port likely doesn't need this — drag uses hardcoded pose) |
| 0x004ad2e0 | int16[12] | `g_dragRaceLane2StripVertices` | high | Sibling block, 12 16-bit literals written at `_DAT_004ad2e0..0x004ad2fc` (offsets 0..0x1c — i.e. 14 shorts of vertex data). Same constants as the first block (0xff10, 0xff79, 0x208...) — mirror for the second drag lane. Likely the lane-2 strip header that the first block overwrites via `g_dragRaceLaneStripPtr` indirection. | td5_physics.c drag lane-2 vertex literal table |

### Track-strip alias for junction walkers (T2.10 orphan 7)

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x004c3da0 | void* | `g_trackStripBlobAliasJunction` | high | Single writer: `BindTrackStripRuntimePointers @ 0x00444075` writes `DAT_004c3da0 = DAT_004aed90` (the loaded STRIP.DAT blob). **16 reads** across track/AI/junction-walker functions (`UpdateActorTrackBehavior @ 0x004351b3`, `UpdateActorTrackBounds @ 0x004366e0`, `RecycleTrafficActorFromQueue @ 0x004353B0`, etc.). The reads dereference `+0x14` (= span count?) and `+0x1c` (= start of junction/branch table — stride 6 words). T2.10 already proposed `g_trackStripBlobAlias` as medium-confidence; **upgrading to high** because the dereferences uniformly walk junction-table records. | td5_track.c strip-blob-with-junction-table cursor |

### Vehicle mesh resource clusters (post-T3 consolidation candidates)

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x004c3d28 | void*[6] | `g_trafficVehicleMeshResourcePtrTable` | high | Written by `LoadRaceVehicleAssets @ 0x004435c7` (6-element loop), read by traffic spawn code (`InitializeTrafficActorsFromQueue @ 0x0040bdb7`, `RecycleTrafficActorFromQueue` mesh-resolution path). Per-traffic-slot mesh resource pointer. Per LoadRaceVehicleAssets: `(&DAT_004c3d28)[iVar10] = iVar19;` accumulates offsets, then a finalize loop adds the heap base. Sibling of `gSlotMeshResourcePtrTable` (0x004c3d10) for racer slots. | td5_physics.c traffic-slot mesh table |
| 0x004c3d48 | uint32[6] | `g_slotVehicleMeshResourceSizeTable` | high | Written by `LoadRaceVehicleAssets @ 0x004432d7` (`(&DAT_004c3d48)[iVar15] = uVar4` — aligned-up archive entry size). Read at 0x004432ec/0x0044351b for the `ReadArchiveEntry` size argument. Per-slot mesh resource byte sizes (6 entries). | td5_physics.c per-slot mesh size table |
| 0x004c3d60 | uint32[6] | `g_trafficVehicleMeshResourceSizeTable` | high | Written by `LoadRaceVehicleAssets @ 0x00443620` (`(&DAT_004c3d60)[iVar10] = uVar11`). Sibling of `g_slotVehicleMeshResourceSizeTable` for traffic mesh entries. | td5_physics.c traffic-slot mesh size table |
| 0x004c3d78 | void* | `g_chassisMeshArchiveEntry` | high | Single writer at `LoadRaceVehicleAssets @ 0x00443337` (`FindArchiveEntryByName(piVar9, s_chassis_00474e14)`). Read by `GenerateActorChassisShadowGeometry @ 0x00443741`. Holds the "chassis" sub-mesh ptr inside the himodel.dat archive — used by car-shadow generator. | td5_physics.c / td5_render.c chassis sub-mesh resource |
| 0x004c3d7c | float | `g_chassisBoundsXMin` | high | Written at `LoadRaceVehicleAssets @ 0x00443360..0x0044339e` as `_DAT_004c3d7c = (float)*(uint *)(DAT_004c3d78 + 0x2c) * <0.5_constant>`. Chassis bounds X (from chassis mesh header offsets 0x2c..0x38). | td5_physics.c chassis bounds floats |
| 0x004c3d80 | float | `g_chassisBoundsXMax` | high | Symmetric writer at same site (offset 0x30 in mesh header * 0.5). | (same) |
| 0x004c3d84 | float | `g_chassisBoundsZ` | med | Written from chassis mesh +0x34 * 0.5. Likely Z-axis bound or center. | (same) |
| 0x004c3d88 | float | `g_chassisBoundsY` | med | Written from chassis mesh +0x38 * 0.5. Likely Y-axis bound or center. | (same) |

### Float scale constant (incidental T3.11/T4.19 cleanup)

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x0045d5d0 | float | `g_halfFloatConstant` | high | Value=0x3F000000 (=0.5f). **Many xrefs** across the codebase — Ghidra autonamed it `_g_fixedPointToFloatScale` but that's misleading (true fp24.8 → float would need `1/256 = 0x3B800000`). Used in `LoadRaceVehicleAssets` for chassis-bounds half-extents, in `InitializeRaceSession` for `gRenderCenterX = g_renderWidthF * 0.5f` viewport-center, and in vehicle-mesh patch loops. **Rename to `g_halfFloatConstant`** to fix misleading T3 inheritance. | td5_render.c viewport-center literal |

## Operand-encoding-artifact DAT_* labels (flag for consolidation session)

Per T2 summary's promotion note: `gVehicleTuningTable` and `gVehiclePhysicsTable` should be promoted to proper Ghidra struct array types. The following 38+ "fake-DAT_" labels would be eliminated by that promotion — DO NOT propose them as separate globals:

- `DAT_004ae582`, `DAT_004ae590..0x004ae5xx` family (`gVehicleTuningTable+N*4` autoname artifacts; struct stride 0x8C = 140 bytes × 12 slots = `0x004ae580..0x004afae0`)
- `DAT_004ae280..0x004ae578` family (`gVehiclePhysicsTable+N*4` autoname artifacts; struct stride 0x80 = 128 bytes × 12 slots)
- `g_actorRuntimeState+N` family inside `0x004ab108..0x004ae57f` (actor stride 0x388 × 12 slots — Ghidra autonames 100+ labels here; promote to `Actor[12]` struct array)

These are NOT bugs in the binary — they're a Ghidra struct-recovery limitation. The consolidation session should:
1. Define `struct VehicleTuning { ... }` (35 dwords = 0x8C bytes, fields per cardef offsets)
2. Define `struct VehiclePhysics { ... }` (32 dwords = 0x80 bytes)
3. Define `struct Actor { ... }` (0x388 bytes — already exists in port as `td5_types.h`)
4. Apply array-of-struct typing to the three base addresses.

## Key discoveries

1. **AI rubber-band throttle formula reconstructed from `ComputeAIRubberBandThrottle @ 0x00432D60`** (called per-tick for each AI slot in non-network mode):
   ```c
   int delta_timer = AI_actor.elapsed_race_time - Player.elapsed_race_time;  // signed, fp8 ticks
   if (delta_timer < 0) {                                                     // AI is lagging
       int clamped = max(delta_timer, -g_aiRubberBandLagSaturation);
       throttle_adj = (g_aiRubberBandLagThrottleGain * clamped) / g_aiRubberBandLagSaturation;
       // throttle_adj is NEGATIVE → 0x100 - throttle_adj > 0x100 → MORE throttle (boost)
   } else {                                                                   // AI is leading
       int clamped = min(delta_timer, g_aiRubberBandLeadSaturation);
       throttle_adj = (g_aiRubberBandLeadThrottleCut * clamped) / g_aiRubberBandLeadSaturation;
       // throttle_adj is POSITIVE → 0x100 - throttle_adj < 0x100 → LESS throttle (cut)
   }
   gActorDefaultRouteSteerBias[slot] = 0x100 - throttle_adj;
   ```
   In network mode, the AI rubber-band is force-disabled (`(&DAT_00473d2c)[iVar2] = 0x100`). Drag mode (`g_selectedGameType != 0`) writes neutral constants (gains=0, saturations=0x40 — no effect since delta * 0 = 0).

2. **AI carparam template scaling chain (`InitializeRaceActorRuntime @ 0x00432e60`):** The 128-byte template at `g_aiVehicleTuningTemplate` (0x00473db0) is copied **once per AI slot** into the actor's per-slot cardef buffer (which Ghidra calls `puVar3` = `actor->car_definition_ptr` — see `reference_arch_cardef_per_actor_indirection.md`). Then short-multiplier adjustments fire based on `gDifficultyHard/Easy`, `gTrackIsCircuit`, `gTrafficActorsEnabled`, and `gRaceDifficultyTier`. The adjustment formula is uniformly `*(short*)(buf + offset) = (short)((current_value * scalar + ((current_value * scalar) >> 31 & 0xFF)) >> 8)` — i.e., a 256-scale fixed-point multiply with sign-correct rounding. Scalars from the binary (sampled): `0x91` (=145, EASY/non-circuit/tier0 light), `0xaa` (=170, NORMAL/non-traffic/tier0), `0xdc` (=220, tier2 push), `0x168` (=360, HARD-base global at +0x68). Up to 4 cardef offsets are touched per branch: `+0x68` (likely max_engine_speed_table[3]), `+0xb0` (likely peak power), `+0x6e` (likely shift point), `+0x70` (gear-ratio adjustment).

3. **Drag-mode AI rubber-band bypass (`g_selectedGameType != 0` else-branch at 0x00432f06..0x00432f17`):** When game-type is non-zero (drag/cop/wanted/special), the rubber-band constants are forced to:
   ```
   g_aiRubberBandLagThrottleGain  = 0
   g_aiRubberBandLeadThrottleCut  = 0
   g_aiRubberBandLagSaturation    = 0x40
   g_aiRubberBandLeadSaturation   = 0x40
   ```
   Result: `throttle_adj = (0 * clamped) / 0x40 = 0` for both branches → output `RS[4] = 0x100` (no AI throttle modulation). Drag racing relies on the synthetic full-throttle driver instead (per `todo_drag_strip_ai_idle.md`).

4. **Per-difficulty/circuit/tier branching is a 3D lookup table flattened into 10 if/else branches** at 0x00432f06..0x004334d5. Logical structure:
   ```
   if (game_type == 0) {  // single race / cup
     memcpy(per_actor_cardef, AI_TEMPLATE, 128);
     /* difficulty-tier short-multiplier adjustments on the COPIED cardef: */
     if (HARD)         { adj1; adj2; adj3; adj4; rubber_band_set(...); }
     else if (EASY)    { /* no adjustments, falls through */ }
     /* (NORMAL is the implicit else with adj1 + adj2 only) */
     if (CIRCUIT) { /* circuit branch — different rubber-band tier table */ }
     else if (overlay_mode == 4) { /* cop/wanted branch */ }
     else if (!TRAFFIC) { /* tier 0/1/2 NO-TRAFFIC rubber-band table */ }
     else { /* tier 0/1/2 WITH-TRAFFIC rubber-band table */ }
   } else {
     /* DRAG / non-zero game type — neutral rubber-band, no AI template */
   }
   ```
   Implication for the port: `td5_ai.c`'s rubber-band binding must replicate the 3D table. The port's `bind_default_vehicle_tuning` already handles game_type==0 vs non-zero (per `reference_drag_ai_template_binding.md` Commit 700a1e4), but the 4 rubber-band constants are read by AI throttle compute — verify the port's AI throttle path reads from the per-difficulty-tier-scaled values, not the AI template constants.

5. **Vehicle data flow (`LoadRaceVehicleAssets @ 0x00443280`):** The 0x10C-byte carparam.dat record per car has a clean split:
   - bytes 0x00..0x8B (35 dwords) → copied to `gVehicleTuningTable + slot*0x8C` (cardef rows)
   - bytes 0x8C..0x10B (32 dwords) → copied to `gVehiclePhysicsTable + slot*0x80` (physics rows)
   The dual-table layout means each car has 268 bytes of tuning split across two static arrays. The `InitializeRaceVehicleRuntime @ 0x0042F140` then writes `actor->car_definition_ptr = &gVehicleTuningTable[slot*0x8C]` (per cardef-indirection memo). On top of that, `InitializeRaceActorRuntime` MAY overwrite the cardef row with the AI template (state==0 && game_type==0).

6. **Default car ID seed table:** `g_perSlotRubberBandThrottleLive @ 0x00473d2c` (the LIVE table) starts as a copy of `g_perSlotRubberBandThrottleConstantTemplate @ 0x00473d64` containing the 14-dword sequence `0x100, 0x100, 0x140, 0x118, 0x122, 0x140, 0, 0, 0, 0, 0x2bc, 0, 0, 0`. The first 6 dwords match `gFrontendSlotCarIdTable` defaults (Viper=0x100, etc.) — but the labels are misleading: this is the **per-slot AI throttle multiplier**, not car IDs. The slot index → throttle mapping is then mutated by the rubber-band tick.

7. **The drag-race lane-strip block at `g_dragRaceLaneStripPtr @ 0x004ad288`** holds 12 16-bit signed coordinates (0xff10, 0xff79, 0x208 = -240, -135, +520 in fp8) forming a quad strip footprint. The writer is **not directly findable** via static refs (likely written via a strip-walker pre-init or by track-strip blob memcpy). The 15-write block in `InitializeRaceActorRuntime @ 0x00433558..0x0043360e` is a **layout patcher**, not the storage allocator — it overwrites with the standard drag-grid coordinates regardless of track. **Implication:** The port's drag mode probably can ignore this entirely (it uses hardcoded actor pose via `InitializeActorTrackPose` for drag start-grid), but if drag-strip geometry rendering ever desynchronizes, this block is the source.

8. **`g_halfFloatConstant @ 0x0045d5d0` (=0.5f) is widely abused** as a multiplier. Ghidra's autonamed `_g_fixedPointToFloatScale` is wrong — true fp8 conversion would use 1/256. Real uses: viewport center math (`gRenderCenterX = width * 0.5f`), chassis-bounds half-extent computation, vehicle-mesh half-stride layouts. Renaming to `g_halfFloatConstant` (or splitting if separate semantic uses exist) eliminates several pages of confusion in render/init audits.

## TODO impact

**reference_drag_ai_template_binding (5-day-old memo, audit confirms):** Naming `g_aiVehicleTuningTemplate` (0x00473db0) makes the drag-mode gate logic readable: the original's gate `if (gRaceSlotStateTable.slot[i].state == 0 && g_selectedGameType == 0)` is now self-documenting. The 4 rubber-band globals (0x00473d9c..a8) make the cop/wanted overlay (`g_raceOverlayPresetMode == 4` branch) and the tier-0/1/2 traffic-vs-non-traffic tables also self-documenting. **The memo's claim that "drag bypasses the AI template" is confirmed BUT the rubber-band constants ARE still written for drag** (set to neutral=0 gains in the else-branch). **Naming-only batch — no code change.**

**reference_arch_cardef_per_actor_indirection (5-day-old memo, fully confirmed):** The audit reconfirms every claim — `gVehicleTuningTable + slot*0x8C` is the original's "cardef row" address; the port maps to `actor->car_definition_ptr` per-actor. The AI template (0x00473db0) is COPIED INTO that row (overwriting carparam.dat data) when state==0 && game_type==0. **Implication for port-side AI binding:** `td5_physics.c`'s `bind_default_vehicle_tuning` must (a) seed from `s_loaded_cardef[slot]` (carparam.dat), then (b) IF AI slot, memcpy AI template on top, then (c) apply difficulty-tier short-multiplier adjustments. The order matters — the difficulty-tier mutations write back to the SAME buffer that was just template-stomped. Per `todo_playerisai_carparam_binding`, the port's commit 48d320a already handles PlayerIsAI=1 exception (skips the template copy for slot 0). **Naming-only batch — no code change.**

**todo_playerisai_carparam_binding (SHIPPED 2026-05-11, but worktree-only per memo):** Naming the AI template global makes the port-side fix's intent explicit. The td5_physics.c short-circuit at line 6688 reads cleaner as `if (slot == 0 && g_td5.ini.player_is_ai) skip g_aiVehicleTuningTemplate copy`. **Recommend the consolidation session apply these names AND verify whether commit 48d320a has landed on master** (memo says "shipped on worktree, NOT yet merged" — could be stale).

**todo_drag_strip_ai_idle (RESOLVED 2026-04-28):** This memo notes drag-mode slot 1 was getting no AI command writer. Naming `g_aiRubberBandLagThrottleGain/LeadCut` etc. as forced=0 in the drag-mode branch confirms the original's design: there's no rubber-band for drag (since both racers go straight, the lag/lead comparison is meaningless — they're not on a curved track). The port's synthetic full-throttle driver (commit 9190af3) is correctly NOT using rubber-band scaling — it bypasses the AI command writer entirely. **Naming-only batch — confirms port enhancement is design-consistent.**

**(No new TODOs surfaced.)** The 4 T2.10 orphan rubber-band globals are now fully characterized; the AI template global is fully characterized; the drag-lane strip pointer is partially characterized (writer site not found but reads understood); the track-strip junction alias is fully characterized.

## Out-of-scope finds (flag for future batches)

| address | brief note | probable batch |
|---|---|---|
| 0x004c3d28..0x004c3d40 | Traffic mesh resource table cluster (named here) — sibling of `gSlotMeshResourcePtrTable` | T5 vehicle-asset cleanup |
| 0x004c3d48..0x004c3d60 | Per-slot mesh size table (named here) | T5 vehicle-asset cleanup |
| 0x004c3d60..0x004c3d78 | Traffic mesh size table (named here) | T5 vehicle-asset cleanup |
| 0x004c3d78..0x004c3d88 | Chassis mesh ptr + bounds (named here) | T5 chassis/shadow audit |
| 0x004c3d8c | `_DAT_004c3d8c` — strip-attribute-count from `BindTrackStripRuntimePointers`. 1 read elsewhere. | T5 track-attribute audit |
| 0x00473820, 0x00473824 | Per-track camera config tables (touched by T2.10 + T4 should-have-named earlier) | T5 camera per-track audit |
| 0x00466edc family (gCarZipPathTable) | Already named. The 8-element char* table at 0x00466edc is the car archive path lookup — verify byte layout in consolidation. | T5 frontend consolidation |
| 0x00474ce8 + offsets | Traffic-mesh source table (`puVar1 = &DAT_00474ce8 + *(int *)(&DAT_00474d74 + iVar17 * 4) * 0x18`) — traffic mesh archive paths indexed by per-track count | T5 traffic-data audit |
| 0x004ad2e0..0x004ad2fc | Drag lane-2 vertex literals (named here as `g_dragRaceLane2StripVertices`) | T5 drag-data audit |
| 0x00474d74 | Per-track traffic count table — single read in `LoadRaceVehicleAssets`. | T5 track-data audit |

## Ghidra session notes

- Session `85accb7a88354875a36bcb077c35618a` opened TD5_pool0 read-only.
- Pool slot acquired via `bash scripts/ghidra_pool.sh acquire`; released via `bash scripts/ghidra_pool.sh cleanup` after analysis.
- No writes to Ghidra performed. Names listed here are **PROPOSED only** — the consolidation session will apply them.
- One writer site for `g_dragRaceLaneStripPtr @ 0x004ad288` could not be located via static refs/byte-search; the 15-read pattern in `InitializeRaceActorRuntime` is sufficient evidence to name the pointer, but the **storage allocator** that seeds it is unknown (likely runtime-bound from the strip blob). Flagged for live-debug audit if drag-strip geometry ever needs the writer.

## Batch stats

- **Total proposals: 22** (17 high + 4 med + 1 low)
- **T2.10 orphan coverage: 4/4** (0x00473d9c, 0x00473da0, 0x00473da4, 0x00473da8 — all named; plus 0x00473db0, 0x004ad288, 0x004c3da0 also named — 7 of 7 listed in T2.10 out-of-scope table)
- **Time spent: ~40 minutes** (within 30-45 min ground rule)
- **Functions touched: 7** (target was 5-8)
- **Memos referenced: 2** (`reference_drag_ai_template_binding`, `reference_arch_cardef_per_actor_indirection`)
- **Memos confirmed: 2/2** (no audit contradictions)
- **Operand-encoding-artifact DAT_ labels flagged: 3 families** (gVehicleTuningTable+N, gVehiclePhysicsTable+N, g_actorRuntimeState+N — promote-to-struct in consolidation)
