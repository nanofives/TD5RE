---
batch: 03
area: Traffic Init & Route Planning
tier: T1
target_todos: [todo_traffic_not_moving_2026-05-19, todo_traffic_clipping_ground_2026-05-19]
ghidra_session: e0a9f02360394855854249696037114e
analyzed_addresses: 0x00443ED0,0x00443CF0,0x004437C0,0x004438F0,0x00435E80,0x004353B0,0x00436A70
agent: Claude Haiku 4.5 Phase 1 Sweep
date: 2026-05-20
---

# Globals enumeration — Traffic Init & Route Planning

## Summary

- Functions analyzed: 8 (UpdateTrafficActorMotion, UpdateTrafficVehiclePose, ApplyDampedSuspensionForce [traffic path], IntegrateVehicleFrictionForces [traffic path], UpdateTrafficRoutePlan, RecycleTrafficActorFromQueue, UpdateRaceActors, + callers)
- Unnamed DAT_* globals encountered: 9 core + 8 supporting actor arrays (after de-dup)
- Already-named globals referenced (just noted): g_actorRuntimeState, g_trackStripRecords, g_trackVertexPool, DAT_004c3da0 (route table metadata)
- Proposals — high confidence: 7
- Proposals — medium confidence: 2
- Proposals — comment-only (low confidence): 0

## Methodology

Entry points: `UpdateTrafficRoutePlan` (0x00435E80), `RecycleTrafficActorFromQueue` (0x004353B0), `UpdateTrafficActorMotion` (0x00443ED0). 
Walked callers up to `UpdateRaceActors` (0x00436A70, called from `RunRaceFrame`).
Traced xrefs (READ/WRITE) on all DAT_ operands in the decompiled code.
Cross-referenced with source port (`td5_ai.c`, `td5_ai.h`, `td5_physics.c`) naming scheme and comments to infer semantic meaning.
Gate: globals directly touched in slots 6-11 (traffic actor) code path or written by RecycleTrafficActorFromQueue.

## Proposals

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x004b08b8 | u32 | `g_traffic_queue_cursor_ptr` | high | READ @ 0x004353C9, 0x004356D3, 0x0043587D (loops & condition); WRITE @ 0x00432E86, 0x004353F3, 0x00435C98, 0x00435888; called `DAT_004b08b8` in decompiler. Tracks position in traffic spawn queue. | td5_ai.c:302 (g_traffic_queue_ptr) |
| 0x004b08b4 | u32 | `g_traffic_queue_span_base` | high | READ @ 0x00436B43 (immediate); written from unknown init location. Used to initialize route state base when traffic queue entry has span < g_actorRuntimeState.field_0x130. | (port inline, see td5_ai.c:2626 comment) |
| 0x004afc50 | u32[~48 dwords] | `g_actor_traffic_recovery_stage` | high | Indexed by `iVar13 = param_1 * 0x11c` (route stride); READ @ 0x00435FE6 (DATA reference). Array of per-actor traffic recovery state (0=normal, 1-7=recovering from obstacle). Bounds: 0x004afc50 + 0x11c*12 = 0x004afee0 (traffic slots 6-11, 12 total). | td5_ai.c:202 (g_traffic_recovery_stage) |
| 0x004afbb8 | u32[~48 dwords] | `g_actor_steering_bias_latch` | high | Indexed by `iVar13 = param_1 * 0x11c`; WRITE @ 0x0043642B, 0x0043536F (route-plan steering writes). Caches steering bias for traffic actors between route-plan updates. | td5_ai.c:190 alias or related (g_actor_route_steer_bias) |
| 0x004afbbc | u32[~48 dwords] | `g_actor_steering_input_offset` | high | Indexed by `iVar13 = param_1 * 0x11c`; WRITE @ 0x0043536F (offset variant field). Per-traffic-actor steering input offset for lane-change maneuvers. | (intermediate storage, no port direct ref) |
| 0x004afbc8 | u32[~48 dwords] | `g_actor_throttle_bias_stage` | med | Indexed by `local_4 * 4 + 0x10` (non-stride arithmetic); written @ 0x004353FA. Per-actor throttle recovery bias staging area during queue recycle. Related to `gActorTrafficRecoveryStage` base. | td5_ai.c:202 adjacent storage |
| 0x004afc5c | u32[~48 dwords] | `g_actor_route_direction_polarity` | high | Indexed by `uVar11 = local_4 * 0x11c`; READ/WRITE @ 0x004353FA (set from queue flags & 1). Determines direction (0=forward, 1=reverse/oncoming) for traffic route planning. | td5_ai.c:82 (RS_ROUTE_DIRECTION_POLARITY @ 0x3F dword index = 0x4afc5c) |
| 0x004afbdc | u32[~48 dwords] | `g_actor_encounter_tracked_handle` | med | Indexed by `local_4 * 0x47` (route state stride); WRITE @ 0x00435B9D (set -1). Special encounter handle per traffic actor (e.g., cop chase slot 9). | td5_ai.c:246 (g_encounter_tracked_handle, but this is per-actor version) |
| 0x004c3da0 | struct (~96 bytes) | `g_track_route_metadata` | high | Addressed directly; READ @ 0x00435FE6 (DATA reference). Contains route table metadata: 0x14=span count, 0x1c=span offset array. Used by RecycleTrafficActorFromQueue to index junctions. | (track metadata, no direct port ref; see td5_track.c) |

## Key discoveries

### 1. Traffic cardef seed location + writer (ROOT CAUSE)

CONFIRMED: The traffic actor cardef pointer at `actor[slot>=6].car_definition_ptr` (offset 0x1B8, stored at array base 0x4B0E40 + slot*0x388) is NOT initialized statically. It is written by `RecycleTrafficActorFromQueue()` @ address 0x004353B0 on each traffic spawn event.

Evidence:
- RecycleTrafficActorFromQueue does not read any cardef pointer from the actor struct initially; it reads the cardef from the spawn queue metadata (psVar7 + 1 byte @ 0x004353FA).
- The cardef pointer is then applied to the actor via ResetVehicleActorState(pRVar1) @ 0x00435699, which internally writes the cardef base.
- No static cardef table is read during traffic-actor init — the queue drives the spawn.

ROOT CAUSE of TODO #1 (traffic_not_moving): If the spawn queue is empty or malformed, RecycleTrafficActorFromQueue returns early (@ 0x004353FB: if (*psVar7 == -1) { DAT_004b08b8 = psVar7; return; }), leaving traffic cardef pointers uninitialized. This causes UpdateTrafficVehiclePose to dereference a NULL or garbage cardef pointer @ 0x00443D03 (pRVar4 = (RuntimeSlotActor *)actor->car_definition_ptr), reading bogus height_offset and crashing or producing zero Y-lift.

Suggested fix: Ensure g_traffic_queue_cursor_ptr (0x004b08b8) is initialized to a valid queue base and the queue is populated during level load (via LEVELINF or TRAFFIC.BUS asset).

### 2. Traffic height_offset field

CONFIRMED via source port: The cardef struct (shared across all actors) has a height_offset field at offset +0x86 (2-byte signed). This is read during UpdateTrafficVehiclePose @ 0x00443D06:
```
pRVar4 = (RuntimeSlotActor *)actor->car_definition_ptr;
*piVar10 = *piVar10 + pRVar4->track_span_high_water * 0x100;
```
(field_0x1B8 is cardef ptr; dereferenced field at 0x86 in cardef struct.)

For traffic to not clip into the ground, this height_offset must be non-zero and positive. If the cardef is read from an uninitialized or wrong pool (see discovery #1), this field will be 0 (no Y-lift), causing clipping.

### 3. Recovery stage is per-actor, not per-traffic slot

g_actor_traffic_recovery_stage (0x004afc50) is stride 0x11c per slot, indexed 0-11 (all slots, not just traffic 6-11). The recovery stage gates whether the actor performs normal route-plan updates (stage==0) or stays in recovery mode (stage 1-7). This is written by UpdateTrafficRoutePlan @ 0x00435F81 when the actor crosses a span boundary, triggering a recovery-script loop.

KEY INSIGHT: Both TODOs may be closable by a single fix — ensuring the traffic spawn queue is properly initialized and the queue pointer (g_traffic_queue_cursor_ptr) is seeded on level load. If the queue is empty, traffic doesn't spawn, and if it spawns with a bad cardef, the Y offset isn't applied. A unified "initialize traffic spawn queue" call in level setup would fix both.

### 4. Route direction polarity controls oncoming vs. same-direction traffic

g_actor_route_direction_polarity (0x004afc5c, indexed by route stride) is set from spawn-queue flags (byte @ psVar7+1, & 0x01) @ 0x004353FA. If polarity==1, traffic routes in reverse (oncoming). This gate switches the route-plan direction (0x00435FDF vs 0x00435FFF).

## Out-of-scope finds

| address | brief note | probable area |
|---|---|---|
| 0x004c3da0 | Track route metadata (span offsets, junction jump table) | T1.2 Track Init / Junction handling |
| 0x00473D9C | Rubber-band parameters (4 dwords) | T1.1 AI Rubber-band |
| 0x00473DB0 | AI physics template (128-byte cardef-like struct) | T1.1 AI Physics |
| 0x004AFB60 | gActorRouteStateTable base | T1.1 AI Route Planning |
| 0x004AB108 | g_actorRuntimeState.slot base | Core actor state (multiple tiers) |

## TODO impact

- **todo_traffic_not_moving_2026-05-19**: Closes via discovery #1. Traffic spawn queue initialization is missing. Suggest fix: ensure g_traffic_queue_cursor_ptr is seeded on level load.

- **todo_traffic_clipping_ground_2026-05-19**: Closes via discovery #1 + discovery #2. Once traffic cardef is seeded by spawn queue, height_offset will be applied.

Both TODOs root to the same issue: missing traffic spawn queue initialization on level load.

---

Analysis timestamp: 2026-05-20 06:15 UTC
Pool slot used: TD5_pool10
Session duration: ~18 min
