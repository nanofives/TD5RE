# CANNOT_DETERMINE investigation — 2026-05-24

4 rows from `orig_vs_port_verdict_2026-05-24.csv` that agents couldn't classify on first pass. Re-investigated by direct Ghidra MCP decompile of orig + re-read of port code at file:line, with focus on shared helper (`traffic_edge_pen`) and the port's own self-classification comments.

## Results
| Final verdict | Count |
|---|---:|
| TRULY_CANNOT_DETERMINE | 4 |
| NOW_FAITHFUL | 0 |
| NOW_INTENTIONAL | 0 |
| NOW_OVERSIGHT | 0 |

All 4 reclassifications confirm the original `CANNOT_DETERMINE` verdict. The first three (traffic edge tests at 0x407390 / 0x4076c0 / 0x407840) share a common helper `traffic_edge_pen` whose port semantics genuinely differ from orig (parallel projection vs rotated outward normal); the question "does this sign-flip pen in actual gameplay" cannot be answered statically. The fourth (ScreenGameOptions @ 0x41f990) is a storage-layout question (port static vars + INI vs orig fixed RDATA globals at 0x466000-0x466018) — the port already wires labels to the correct semantic globals (g_td5.traffic_enabled / .checkpoint_timers_enabled / .special_encounter_enabled), but address-level mapping requires runtime memory inspection.

## Per-row findings

### 0x00407390 ProcessActorSegmentTransition
- **Original agent block**: helper traffic_edge_pen uses atan2-tangent parallel projection vs orig rotated-outward-normal `local_8 = (B.z-A.z, 0, A.x-B.x)` + ConvertFloatVec4ToShortAngles normalization; also vertex pair swap on outer test, reference-point swap.
- **Re-read finding**: Re-decompile of orig 0x00407390 confirms `local_8` is the outward normal (cross-product 90° rotation of edge tangent A→B), x87-normalized to magnitude 4096 via `ConvertFloatVec4ToShortAngles` (4 `__ftol()` calls). Port (td5_physics.c:5244 `traffic_edge_pen`) computes `tan = (cos(atan2(tdz,tdx)), sin(atan2(tdz,tdx)))` — this is the **tangent** unit vector, not the normal. `(rel . tan)` measures parallel projection along the edge, whereas `(rel . normal)` measures perpendicular distance — sign of `pen` can differ for the same geometry. Port comments at td5_physics.c:5281-5337 already document this as "[L4 — Frida-pending]" with explicit unblock criterion.
- **New verdict**: TRULY_CANNOT_DETERMINE
- **Runtime trace needed**: Frida hook at orig `0x004073D8` (entry to inner-edge pen computation) and orig `0x00407920` (entry to outer-edge pen) capturing `(slot, span_idx, sub_lane, type, A.x, A.z, B.x, B.z, rel.x, rel.z, pen_orig)`; matching port-side instrumentation at `traffic_edge_pen` returning `pen_port`. Run on Edinburgh + San Francisco scenarios (San Francisco for slot 6+ traffic per `reference_san_francisco_ai_completion_2026-05-16.md`). If `sign(pen_orig) == sign(pen_port)` across all contact ticks, divergence is benign → upgrade to ARCH-DIVERGENCE / INTENTIONAL. Otherwise file as fix-required regression.

### 0x004076c0 ProcessActorForwardCheckpointPass
- **Original agent block**: same divergent `traffic_edge_pen` helper; also adds `sub_lane` to `li_idx` where orig psVar2=vertex[uVar6] with no sub addition; Frida sign(pen) trace required.
- **Re-read finding**: Re-decompile of orig 0x004076c0 confirms it uses identical `local_8` outward-normal pattern as 0x407390 — and identical `ConvertFloatVec4ToShortAngles` normalization. Port td5_physics.c:5580 `process_traffic_forward_checkpoint_pass` reuses the same divergent `traffic_edge_pen` helper. Additionally port at td5_physics.c:5617 reads `sub` from `actor->track_sub_lane_index` and adds it to `li_idx = sp->left_vertex_index + sub`, whereas orig at 0x00407708-0x4076FF computes `psVar2 = vertex[strip.left_vertex_index]` (NO sub_lane addition) and `psVar3 = vertex[strip.left_vertex_index + lane_count]`. So the port has TWO divergences here: (a) the shared `traffic_edge_pen` semantic + (b) extra sub_lane indexing that orig doesn't do. Both are documented as L4-pending.
- **New verdict**: TRULY_CANNOT_DETERMINE
- **Runtime trace needed**: Same Frida sign(pen) trace as 0x407390. Plus add `(li_idx, ri_idx)` to the capture tuple — if `vl/vr` selection visibly diverges from orig psVar2/psVar3 the sub_lane addition is a real bug independent of the dot-product semantic.

### 0x00407840 ProcessActorRouteAdvance
- **Original agent block**: vertex selection byte-faithful (left base + lane_nibble) but wires divergent traffic_edge_pen; same Frida sign(pen) trace blocker as 0x407390.
- **Re-read finding**: Re-decompile of orig 0x00407840 confirms it uses the same `local_8 = (psVar3.z-psVar2.z, 0, psVar2.x-psVar3.x)` outward-normal + `ConvertFloatVec4ToShortAngles` normalization as the other two. Port td5_physics.c:5498 `process_traffic_route_advance`: the vertex-pair selection at td5_physics.c:5538-5539 (`li = left+lane_nibble`, `ri = left`) is correct (port comment at 5529-5537 documents the byte-faithful mapping). The ONLY divergence is the shared `traffic_edge_pen` helper. Sign-of-pen verdict therefore depends entirely on whether parallel-projection and perpendicular-distance happen to share a sign across the actual contact geometry observed in races.
- **New verdict**: TRULY_CANNOT_DETERMINE
- **Runtime trace needed**: Same Frida sign(pen) trace as 0x407390 — this site is the simplest of the three (only one pen computation, no inner/outer split), so it's the best target for the first trace pass. If sign matches here, it likely matches the other two as well.

### 0x0041f990 ScreenGameOptions
- **Original agent block**: orig case-6 button writers target `g_specialEncounterUnlockA/B_PROVISIONAL` (btn 1/2) and `gSpecialEncounterConfigShadow` (btn 3); port writes dedicated `s_game_option_checkpoint_timers/traffic/cops` globals; whether port storage maps to same orig DAT addresses or to independent shadows requires address-level verification of PROVISIONAL global identities.
- **Re-read finding**: Confirmed orig globals at fixed RDATA addresses 0x00466000-0x00466018 (Laps, UnlockA, UnlockB, SpecialEncounterConfig, DifficultyTier, Dynamics, 3dCollisions — 7 contiguous DWORDs). td5_orig_globals.h maps only 3 of these (0x466000=Laps, 0x466014=Dynamics, 0x466018=3dCollisions); the 4 middle slots (UnlockA/B, SpecialEncounterConfig, DifficultyTier) are NOT in the orig_globals header. Port (td5_frontend.c:7357 `Screen_GameOptions`) uses dedicated static C vars `s_game_option_*` that ARE seeded from INI at td5_frontend.c:1870-1872 and copied OUT into `g_td5.traffic_enabled / .special_encounter_enabled / .checkpoint_timers_enabled` at td5_frontend.c:2717-2721. Semantically the port already wires labels "Checkpoint Timers / Traffic / Cops" to the correct game-state fields — but whether each orig PROVISIONAL global drives the same downstream race behavior as the port's `g_td5.*_enabled` field is the actual unknown. The port comment at td5_frontend.c:7401 confirms "Each row cycles its respective global on arrow input" — these are independent shadow storage layouts, NOT a guaranteed byte-faithful mapping.
- **New verdict**: TRULY_CANNOT_DETERMINE
- **Runtime trace needed**: A-B comparison. Setup: (1) In orig TD5_d3d.exe, attach Frida; write `1` to `0x00466004` (UnlockA), `0x00466008` (UnlockB), `0x0046600C` (SpecialEncounterConfig) one at a time via `Memory.writeS32`, then start an Edinburgh race and observe HUD/AI behavior (checkpoint timer visible? traffic spawned? cops siren?). (2) In port td5re.exe, set `g_td5.checkpoint_timers_enabled / .traffic_enabled / .special_encounter_enabled` to 1 independently via INI and observe same race. If the in-race effects match 1:1 (UnlockA == checkpoint_timers, UnlockB == traffic, SpecialEncounterConfig == cops), reclassify as INTENTIONAL/STORAGE_LAYOUT. If any of the orig PROVISIONAL writes triggers a different effect (e.g., UnlockA actually unlocks bonus content rather than enabling timers), reclassify the corresponding row as OVERSIGHT.

## Recommended CSV updates
None — all 4 rows retain `CANNOT_DETERMINE,L4-frida-pending` (or empty tag for ScreenGameOptions which had no tag — recommend adding `L4-frida-pending,td5_frontend.c:7357` consistency tag).
