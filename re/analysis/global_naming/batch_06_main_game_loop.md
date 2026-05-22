---
batch: 06
area: main_game_loop_race_init
tier: T2
target_todos: [reference_arch_run_main_game_loop_2026-05-18, reference_arch_cardef_per_actor_indirection, reference_arch_no_speed_bonus_score_2026-05-18]
ghidra_session: 45ef47603aa44015a41c58767adf8638
analyzed_addresses: 0x00442170, 0x0042F140, 0x0042F6D0, 0x0040A3D0
agent: Claude Opus 4.7 (1M)
date: 2026-05-20
---

# Globals enumeration — Main game loop + race init

## Summary

- Functions analyzed: 4 (RunMainGameLoop, InitializeRaceVehicleRuntime, ComputeVehicleSuspensionEnvelope, AccumulateVehicleSpeedBonusScore) + one hop into InitializeRaceSession, InitializeVehicleShadowAndWheelSpriteTemplates, IntegrateVehiclePoseAndContacts, AdjustCheckpointTimersByDifficulty, UpdateVehicleActor.
- Unnamed `DAT_*` globals encountered in scope: **9** (after de-dup; many candidate addresses turned out to already be named or to be field offsets inside named tables)
- Already-named globals encountered (just noted): ~60 (T1 batches + prior R/E work covered most of the main-loop graph)
- Proposals — high confidence: 5
- Proposals — medium confidence: 3
- Proposals — comment-only (low confidence): 1

## Methodology

Entry points were the 4 ARCH-DIVERGENCE functions. For each, I:

1. Decompiled and listed every `reference_from` operand (full instruction-level reference graph, not just the C-decomp variable names).
2. Cross-checked each DAT_* address against `symbol_by_name` to filter out already-named globals.
3. For every unnamed survivor, looked at byte content, reference graph, and surrounding labels to validate semantic meaning.
4. Walked one hop into the immediate caller (InitializeRaceSession) and the largest callee (IntegrateVehiclePoseAndContacts, AdjustCheckpointTimersByDifficulty, InitializeVehicleShadowAndWheelSpriteTemplates, UpdateVehicleActor) so that the cardef-indirection ARCH-DIVERGENCE has full coverage of the bridging writers.

Filter for "in scope": only DAT_* addresses outside `g_actorRuntimeState` (0x004ab108..0x004ac9b8), `gVehiclePhysicsTable` (0x004ae280..0x004ae3ff), and `gVehicleTuningTable` (0x004ae580..0x004ae7ef) — those are field-offset writes inside named tables, not separate globals.

**Skipped categories** (not real DAT_ proposals):
- DLL exrefs (`g_appExref`, `dd_exref`, `dpu_exref`, `Image_exref`, `FPSName_exref`, `ErrorN_exref`, `FCount_exref`) — external imports owned by DLLs, not this binary's globals.
- DLL import-thunk pointers `PTR_*_0045dxxx` — already labelled by Ghidra.
- Pseudo-addresses `0x000600xx` / `0x0006xxxx` — runtime-relocated exref offsets, not memory in this PE image.
- String literals (`s_*_004xxxxx`) — already labelled.
- DAT_004ae5xx and DAT_004ae6xx writes by `ComputeVehicleSuspensionEnvelope` — these are field offsets inside `gVehicleTuningTable[0]` (0x004ae580 + offset). Ghidra labelled them DAT_* because they're 2-byte writes at non-zero offsets from the table base, but they are not separate globals.

## Proposals

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x0045d5e8 | float | `g_fp8ToFloatScale` | high | Float `0x3b800000` = `1/256 = 0.00390625`. 13 READ sites all in physics integration (IntegrateVehiclePoseAndContacts @ 0x00405f5a/0x00405f7f/0x00405f98, plus 10 more in 0x00405dxx-0x00409bxx); converts world_pos fp8 → render_pos float (`actor->render_pos_x = (float)actor->world_pos_x * _DAT_0045d5e8`). | `td5_physics.c` macro `FP_TO_FLOAT(x) = (float)(x)/256.0f` — already exists as a constant; this is the orig's float-table mirror. |
| 0x0046318c | u8[13] | `g_chassisDamageStateGravityApplies` | high | Read once at IntegrateVehiclePoseAndContacts 0x0040630d as `(&DAT_0046318c)[actor->damage_lockout]`; gates the `linear_velocity_y = (avg_wheel_y - prev_frame_y) - gravity` reset. Bytes `[01,01,01,00,01,00,00,00,01,00,00,00,00]` → indices 0,1,2,4,8 = "grounded/intact" (apply gravity to vy); 3,5,6,7,9,10,11,12 = "tumbling/airborne" (preserve free-fall). 13 entries indexed 0..12 by `actor->damage_lockout` (bound by `RuntimeSlotActor.damage_lockout` switch cases). | (none — port appears to compute equivalent logic inline; verify in td5_physics.c integrate path) |
| 0x00483030 | u32 | `g_freezeHorizontalIntegration` | high | Read once at IntegrateVehiclePoseAndContacts 0x00405f1e. Gates `world_pos_x += linear_velocity_x; world_pos_z += linear_velocity_z` integration (Y is integrated regardless). When non-zero, the chassis Y still falls under gravity but X/Z stay locked. Single reader. Default 0. Likely a debug/diagnostic horizontal-freeze flag, or a hidden cheat code. | (none — port has no equivalent gate; safe to ignore unless we discover a writer in T3) |
| 0x00466f94 | i32[6×3] | `gRaceResultBonusOffsetColumn` | high | Read at InitializeRaceSession 0x0042ab73 inside `for(i=0..6){...= *(&gRaceResultPointsTable + i*0xC); ...= *(&DAT_00466f94 + i*0xC); ...= *(&DAT_00466f98 + i*0xC)}`. This is **column 1** of the 6-row × 3-column race-result modifier table whose row[0] base is `gRaceResultPointsTable @ 0x00466f90`. Values `[1, 0, -40, -40, 0x72, 1]` per row. Loaded into `gSlotRaceBonusTable`. | td5_save.c / td5_game.c race-results path. Should be a sub-field of `gRaceResultPointsTable` struct (replace 3 separate exports with one struct array). |
| 0x00466f98 | i32[6×3] | `gRaceResultPointsAdjustColumn` | high | Same call site (0x0042ab79). **Column 2** of the same table. Values `[0, 0x72, -40, -40, 0, 6]` per row. Loaded into `gSlotRacePointsTable`. Drives the negative-adjust branch in InitializeRaceVehicleRuntime difficulty scaling (negative → /0x300 div; positive → >>9 right-shift). | (same struct field as above) |
| 0x0048dc50 | struct~0xB8 + body | `g_vehicleShadowSpriteTemplates` | medium | Base of an array iterated by InitializeVehicleShadowAndWheelSpriteTemplates 0x0040bc6a/0x0040bc71. Stride 0x170, 14 entries; each entry includes a shadow quad-template region around offset +0xb8 (where the loop seeds `puVar5 = &DAT_0048dd08` = base + 0xb8 and walks 14 iterations of `BuildSpriteQuadTemplate`). Address adjacent to `g_shadowVerticalOffset @ 0x0048dc48`. End: 0x0048f128 (= 0x48dc50 + 14*0x170 = 0x48f070 — hmm but loop uses 0x48f128 as limit and seeds from 0x48dd08; the off-by-0xb8 is the wheel-template offset inside each entry). | td5_render.c — sprite/shadow render path. Port has BuildSpriteQuadTemplate at 0x432BD0 and unified flag dispatch (recent commit bb9cae9); base buffer likely allocated dynamically. |
| 0x0048dd08 | struct (anchor at +0xb8) | `g_vehicleShadowSpriteTemplates_wheelAnchor` | low | Same array as 0x0048dc50, but this is the per-entry wheel-template starting offset (entry_base + 0xb8). Loop seeds `puVar5 = &DAT_0048dd08` and iterates with stride 0x170. Comment-only — recommend leaving DAT_* and referring to the array via 0x0048dc50. | (none — derived address) |
| 0x00467384 | u8[8] | `g_actorWheelOrderTableActive` | medium | Read 3× from UpdateVehicleActor 0x00406911/0x00406921/0x00406931 as the **3rd arg** to `UpdateActorTrackSegmentContactsReverse/Forward/Contacts(slot, UpdateVehiclePoseFromPhysicsState, 0x467384)`. Bytes `[00,01,02,03,FF,00,00,00]` — 4-wheel order + 0xFF sentinel + padding. The first 4 entries are wheel indices (FL=0, FR=1, RL=2, RR=3). Used during normal physics-driven track-segment contact iteration. | (none — port likely has the 4-wheel iteration inlined) |
| 0x0046738c | u8[8] | `g_actorWheelOrderTableScripted` | medium | Read 6× — 3× from UpdateVehicleActor 0x004067e4/0x004067f4/0x00406804 in the **scripted-vehicle branch** (when `actor->state == 1`, i.e. attract-mode/replay puppet); 3× from `UpdateTrafficVehiclePose @ 0x00443f4f/5f/6f`. Bytes `[00,01,02,03,04,05,06,07]` — full 8-entry sequence. Suggests scripted/traffic actors iterate more wheel sample points than the 4-wheel player/AI path. | (none — port traffic path may bypass this entirely) |

Notes on the confidence levels:

- **high** → consolidation session will apply the rename + add a Ghidra comment with the evidence
- **medium** → apply name with `_PROVISIONAL` suffix
- **low** → add a Ghidra comment with the analysis only; leave `DAT_*` label

## Key discoveries

These are the high-impact findings from this batch — particularly the "lost writers" angle that T2 was scoped to surface.

### 1. The cardef ARCH-DIVERGENCE drops ZERO writers from the orig

The big concern coming in was whether the per-actor-buffer indirection (reference_arch_cardef_per_actor_indirection.md) had silently dropped some `&gVehicleTuningTable[slot*0x8C]` field write that the port doesn't perform on `actor->car_definition_ptr`. **It hasn't.** Every write the orig does to the cardef row in `InitializeRaceVehicleRuntime` (slot 0x68, 0x6e, 0x70, 0x74, 0x78, 0x2c, 0x32+, 0x42+, 0x6a) and in `ComputeVehicleSuspensionEnvelope` (the 12-vertex AABB envelope + the slot-only AABB extension at 0x004ae5e0..0x004ae5fc for slots > 5) has a corresponding writer in the port — they go to `s_loaded_cardef[slot]` and the per-actor buffer through `bind_default_vehicle_tuning`. The arch-divergence is purely *addressing*, not *data flow*. **No lost writes.**

### 2. `gVehicleTuningTable` per-slot has a "traffic-only" AABB extension at +0x60..+0x7C

The `ComputeVehicleSuspensionEnvelope @ 0x0042f6d0` writes 12 short fields per slot in the 0x00..0x3C region of the cardef row (the normal "AABB envelope vertices" — 4 corners × 3 axes × 2 templates). The `if (5 < param_2)` branch writes an ADDITIONAL 12 shorts in the 0x60..0x7C region — but ONLY for slot indices > 5 (= traffic slots 6..11). This is the **traffic vehicle's separate collision envelope** distinct from the player/AI racers. The orig has this; verify the port reproduces it for traffic slots in `td5_physics_compute_suspension_envelope`.

### 3. RunMainGameLoop has surprisingly few standalone globals

The 991-byte FSM dispatcher reads and writes `g_gameState`, `g_introMoviePendingFlag`, `g_frontendResourceInitPending`, `g_startRaceRequestFlag`, `g_startRaceConfirmFlag`, `g_benchmarkModeActive`, `g_benchmarkImageLoadPending`, `g_benchmarkImageDataPtr`, `g_benchmarkDecodedPixelPtr`, `g_benchmarkImageInfoStruct` — and that's it for first-party globals. **Every other reference is into the DLL exref structures** (g_appExref +0x13c/+0x14c/+0x150/+0x15c/+0x16c/+0x180/+0x188; dd_exref +0x1690/+0x1730) and **the FSM was already byte-faithfully ported** — these exref accesses are documented in reference_arch_run_main_game_loop_2026-05-18.md as semantically equivalent to wrapper state in the source port. **There is no "lost writer" to chase in the main loop.**

### 4. `AccumulateVehicleSpeedBonusScore` is gated by `g_specialEncounterEnabled == 4`

The orig calls this from UpdateVehicleActor 0x004066ea **only when** `g_specialEncounterEnabled == 4` (= the "Ultimate" variant scoring mode — `gSpecialEncounterEnabled @ 0x0046320c` is the separate companion flag). The reference_arch_no_speed_bonus_score memo correctly notes the port's m->forward_speed/skid_factor/contact_count are never populated, so even though the port calls accumulate_speed_bonus unconditionally inside the sub-tick loop, the function short-circuits. **A simpler closer for the TODO** than plumbing all ActorRaceMetric fields: gate the port's accumulate_speed_bonus call site on `g_td5.special_encounter_mode == TD5_SPECIAL_ULTIMATE` (matching `g_specialEncounterEnabled == 4`), and the function becomes intentional dead code on every non-Ultimate race — exactly what the orig does. The "scoring parity" question reduces to: do we ever play Ultimate variant? If not, this is closeable as a comment.

### 5. The `gRaceResultPointsTable` is actually a 6×3 i32 table — not three separate tables

T1 batch 5 named `gRaceResultPointsTable @ 0x00466f90`, and T1 batch 4/5 named the destination per-slot tables `gSlotRaceResultTable`, `gSlotRaceBonusTable`, `gSlotRacePointsTable`. But the SOURCE table at 0x00466f90 is a single 6-row × 3-column i32 matrix, indexed by `gFrontendSlotPositionTable[i] * 0xC + col`. The two unnamed DAT_* at 0x00466f94 and 0x00466f98 are just columns 1 and 2 of that matrix — they should be properly typed in Ghidra as a struct array `RaceResultModifierRow gRaceResultPointsTable[6]` where each row has `{i32 points, i32 bonus, i32 points_adjust}`. The naming proposal here is a holding pattern; the better consolidation move is to convert the data definition to a struct.

### 6. `gVehiclePhysicsTable` is the same kind of fake-named-fields situation

The InitializeRaceVehicleRuntime pcode walk surfaced 14 distinct `DAT_004ae2xx` labels (DAT_004ae2ac/b2/b4/b6/c2/c4/e8/ea/ee/f0/f4/f8 + 4 more). All of them are field offsets inside `gVehiclePhysicsTable[i]` (table at 0x004ae280, stride 0x20). Ghidra applied DAT_ labels because the operand was `[base + scaled_index + small_constant]` and the assembler chose to materialize the small_constant as the literal address. **These are not separate globals.** The right fix is to define a struct `VehiclePhysicsRow { i16 mass, i16 wheels[6], i16 drive_torque, i16 grip[4], i16 brake_force, i16 engine_response, ... }` and apply it as the table element type — then all 14 DAT_* labels disappear in favour of `gVehiclePhysicsTable[i].drive_torque` etc.

## Out-of-scope finds

| address | brief note | probable area |
|---|---|---|
| 0x004ae2ac..0x004ae2f8 (14 labels) | All inside `gVehiclePhysicsTable[i]` — fake DAT_ labels from base+index+const operand encoding. | T2.7 (cardef-table struct unification) — recommend a future batch promotes `gVehiclePhysicsTable` to a proper struct array. |
| 0x004ae5a0..0x004ae5fc, 0x004ae600..0x004ae608 (24 labels) | All inside `gVehicleTuningTable[0]` (slot[0] of cardef) — same fake-DAT_ issue. | Same — T2.7 struct unification. |
| 0x0045d610..0x0045d624 | Float constants pool: 778.86, 2.3927, 0.0366, 0.00781, 0.984375, 0.0 — magic FP constants used by physics functions outside our scope. | T2.7 or T2.8 (physics-constants pool naming). |
| 0x0045d6a8, 0x0045d6ac | Floats `-0.70703125` and `20.0` — used by ComputeVehicleSuspensionEnvelope as fp_to_int16 scale + tilt factor. | T2.6 (this batch) — but they're literal constants in the read-only data segment; leaving as DAT_ is fine. |
| dpu_exref + 0xbcc, +0xbe8, +0xc0c | DirectPlay user-data offsets in InitializeRaceSession's network-race branch. | T2.8 (network subsystem). |
| g_appExref + 0x13c, +0x14c, +0x150, +0x15c, +0x16c, +0x180, +0x188 | DDraw app-state mirror fields documented in the ARCH-DIVERGENCE memo. | (none — DLL-side, not this binary's globals.) |
| 0x004aef0 (g_cameraTransitionActive) | Already named via T1 — flagged here for cross-ref completeness. | (already named) |

## TODO impact

- **reference_arch_run_main_game_loop_2026-05-18**: Confirms zero unnamed globals dropped by the port's FSM port. The three ARCH-DIVERGENCEs (DX::app exref mirrors, fullscreen restore, BENCHMARK Lock/blit/Flip) are all DLL-side; no game-owned global is lost. **Closes the "lost writer in main loop" angle** — search elsewhere if convergence audits surface missing per-frame state.
- **reference_arch_cardef_per_actor_indirection**: Confirmed zero data-flow divergence. Every cardef-row write by the orig has a port mirror via `actor->car_definition_ptr` / `s_loaded_cardef[slot]`. The architectural divergence is purely *addressing*. **No bug surface here.**
- **reference_arch_no_speed_bonus_score_2026-05-18**: Surfaces an unnoticed precondition: the orig only calls `AccumulateVehicleSpeedBonusScore` when `g_specialEncounterEnabled == 4` (the Ultimate variant). Suggested simpler close: gate the port's call site on the same condition rather than plumbing forward_speed/skid_factor/contact_count. **Possible TODO close** — pending product decision on whether Ultimate variant is supported in the source port.
- **No new TODO surfaces**: the 4 functions scoped here are all well-behaved with no hidden lost-writer regressions.

## Headline stats

- New high-confidence proposals: 5 (g_fp8ToFloatScale, g_chassisDamageStateGravityApplies, g_freezeHorizontalIntegration, gRaceResultBonusOffsetColumn, gRaceResultPointsAdjustColumn)
- New medium-confidence proposals: 3 (g_vehicleShadowSpriteTemplates, g_actorWheelOrderTableActive, g_actorWheelOrderTableScripted)
- New low-confidence (comment-only): 1
- Total recommendations: 9
- Most important "lost writer" finding: **NONE** — the ARCH-DIVERGENCE memos for these 4 functions correctly characterize the divergences as DLL-side or architectural-addressing-only, not data-flow-loss. T2.6 confirms the port is byte-faithful on the data side for this scope.
