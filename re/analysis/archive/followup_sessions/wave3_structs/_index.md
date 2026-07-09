# Tier1-B — Struct Recovery Index (2026-05-22)

Wave 3 struct field-analysis. Source data: W1-E data segment map (re/analysis/followup_sessions/wave1_e_data_segment_map.md §C/E). Ghidra session: TD5_pool3 read-only. All struct definitions sit in this directory as one `.c` file each; apply via ghidra-apply Wave 2E phase (or use `type_define_c` against `TD5.gpr` master).

## Summary

| Struct | Status | Size | Fields | Consumers | Notes |
|---|---|---:|---:|---:|---|
| RaceParticleSpawnState | **APPLY** | 0x40 | 17 | 6 | High confidence — stride-based array, all field offsets cross-validated by 3 spawn fns + 2 update/draw fns + 1 project fn. |
| PolygonClipperDrawState | **APPLY** | 0x40 | 13 | 5 | High confidence — coherent clip-pipeline state, all consumers in clip/project family. |
| ConnBrowserListLayout | **APPLY** | 0x34 | 13 | 5 | High confidence — 52-byte struct used as 64-entry array. W1-E mis-sized as 76 B (only first entry's fields had hot refs). |
| FrontendUiState | **SKIP** | 148 B | (~26 named) | 40+ | Heterogeneous globals cluster — not a struct. See §A. |
| FrontendRenderResources | **SKIP** | 124 B | (~24 named) | 35+ | Heterogeneous globals cluster — not a struct. See §A. |

Final tally: **3 confident structs ready for Phase B apply** (54 fields total).

---

## A. Why FrontendUiState and FrontendRenderResources are SKIP

Both clusters are large (124–148 B) and were flagged by W1-E as struct candidates because adjacent fields are hot. However, structural decompilation requires more than adjacency — it requires that consumers access the fields **through a common base pointer**. Audit of 15 top consumers of `FrontendUiState` shows every single one uses **absolute addresses** (e.g., `g_frontendButtonPressedFlag = 1` not `state->button_pressed_flag = 1`). Defining a Ghidra struct over these bytes would **not** change any decompile output and would force unrelated fields under a single namespace.

Concrete proof of heterogeneity from the field inventory:
- `FrontendUiState` (0x004951d0..0x00495264) interleaves: TGA decode bitmasks (`g_tgaDecodeRedMask/Green/Blue`), CPU detection (`g_cpuHasTsc`), cheat code progress (`g_cheatCodeKeyProgress`), frontend FSM (`g_frontendInnerState`, `g_frontendAnimFrameCounter`), and attract-mode flags. Five distinct subsystems.
- `FrontendRenderResources` (0x00496260..0x004962dc) interleaves: 8 DDraw surface pointers, cheat-completion flags (`g_cheatPostRaceHighScoreUnlock`, `g_savedAllCarsUnlocked`), two-player mode flag, language selection state, and post-race banner state. Four distinct subsystems.

The fields are already individually named after T1-T5 sweeps (24/26 named in FrontendUiState; 22/24 in FrontendRenderResources). Structification would not improve decomp readability; it would actively harm it by suggesting a false logical grouping.

**Recommendation:** leave both clusters as named globals. Apply the 3 confident structs (RaceParticleSpawnState, PolygonClipperDrawState, ConnBrowserListLayout) at Phase B.

---

## B. RaceParticleSpawnState (APPLY)

File: `RaceParticleSpawnState.c`
- Pool base: `g_raceParticlePoolBase` @ 0x004a3170
- Per-entry stride: 0x40 B (64) — **confirmed 4× independently**:
  - `pcVar3 += 0x40` in spawn-side slot search (3 functions)
  - `puVar2 += 0x10` (4-byte units) in `DrawRaceParticleEffects`
- Per-bank stride: 0x1900 B (100 entries × 0x40)
- Full pool: 6 view banks × 0x1900 = 0x12C00 B
- Consumers (6): SpawnVehicleSmokeSprite (0x00429cf0), SpawnVehicleSmokeVariant (0x00429a30), SpawnVehicleSmokePuffFromHardpoint (0x0042a290), ProjectRaceParticlesToView (0x00429690), DrawRaceParticleEffects (0x00429720), UpdateRaceParticleEffects (0x00429790)

17 fields recovered. Cross-validated across 3 spawn functions for fields +0x00..+0x14 and +0x1f..+0x3c. Per-particle "default constants" differ across spawn paths (size 0x9000/0x7000/0x4000, lifetime 0x600/0x1800/0x2000, life_mult 10/15) — consistent with sprite vs. variant vs. puff dispatch. Flag-byte semantics inferred from gate predicates (`(flags & 0x80) != 0`, `(flags & 0x20) == 0`, `(flags & 0xc0) == 0xc0`).

---

## C. PolygonClipperDrawState (APPLY)

File: `PolygonClipperDrawState.c`
- Base: `g_currentDrawCallVertexBuffer` @ 0x004afb14
- Size: 0x40 B
- Consumers (5): ClipAndSubmitProjectedPolygon (0x004317f0), SetProjectedClipRect (0x0043e640), RenderTrackSegmentBatch, RenderTrackSegmentBatchVariant, AppendClippedPolygonTriangleFan

13 fields recovered (12 named + 1 padding). SetProjectedClipRect uniquely identifies clip_left/right/top/bottom semantics: param_1/_2 are clamped against `raw_clip_x_min/max` (at +0x24/+0x28), param_3/_4 against `raw_clip_y_min/max` (+0x2c/+0x30). The half-width/height cache fields (+0x1c/+0x20) read back during the early-out ABS test in the polygon clipper's main loop confirm the geometry: `(clip_half_height - ABS(tmp_y_scratch)) >> 31` — a sign-bit-based "outside?" test.

Note: same caveat as A applies — references are absolute. But unlike FrontendUiState, all 13 fields belong to **one logical clip-pipeline state**, so the struct grouping is semantically real. Applying improves decomp readability without false aggregation.

---

## D. ConnBrowserListLayout (APPLY)

File: `ConnBrowserListLayout.c`
- Base: `g_connBrowserListOriginX_PROVISIONAL` @ 0x00499c78
- Stride: 0x34 B (52)
- Array length: 64 entries (table size 0xD00)
- Consumers (5): CreateFrontendDisplayModeButton (0x00425de0), UpdateFrontendDisplayModeSelection (0x00426580), RenderFrontendDisplayModeHighlight (0x004263e0), MoveFrontendSpriteRect, RenderFrontendUiRects

13 fields recovered. **W1-E size finding was wrong**: it reported 76 B because that's the address range with hot refs, but the actual per-entry struct is 52 B and there are 64 entries. Confirmed by:
- Int-stride `iVar8 * 0xd` (13 ints = 52 bytes) for int-typed fields
- Byte-stride `iVar8 * 0x34` for byte-typed flag field at +0x2c
- Sentinel walk in UpdateFrontendDisplayModeSelection: `while (*(piVar3 + 0xd) != -1)`
- Upper-bound check: `(int)piVar4 < 0x49a988` ⇒ (0x49a988 - 0x499c88) / 0x34 = 64 exact

Two W1-E provisional names corrected:
- `g_connBrowserListRowStride_PROVISIONAL` (+0x30) is NOT a row stride; it's a per-button **hover-press progress counter** (0..6) incremented while button is hovered.
- "ConnBrowser" naming is fine as the legacy hint, but this layout is used for ALL frontend menu button tables, not just the network browser. Display-mode selection (graphics settings) is the primary writer observed. Consider rename to `FrontendButtonSlot` at apply time.

---

## E. Apply procedure (Phase B)

For each APPLY struct:
1. Acquire pool slot (write-mode required for `type_define_c`).
2. `type_define_c` with the typedef as written.
3. `type_apply_at(addr, type_name)` at the base address (and for ConnBrowserListLayout, as `<type>[64]` array).
4. Re-decomp the consumer list (above) to verify the struct-typed access pattern looks right.
5. Save program. Run `ghidra-sync` to propagate to pool slots.

Estimated apply cost: ~15 min for all 3 structs (no contention with existing types — none of these addresses are inside `RuntimeSlotStateTable`, `RuntimeSlotActorArray`, or other existing-named structs).

---

## F. Method notes & honesty caveats

- All struct definitions verified against ≥3 consumer decompilations except where a struct has only 4-5 consumers total (in which case all were checked).
- Field types inferred from access width: byte access ⇒ uchar; word access ⇒ ushort/short; dword load with constants like 0x2000 ⇒ uint; dword load with `(int *)`/`(int *)& ...` ⇒ int; arithmetic with `_g_fixedPointToFloatScale` ⇒ fp8-int; arithmetic via FP ops ⇒ float; written `&LAB_xxx` ⇒ function pointer.
- Padding fields are real (no refs in the range); they're not invented to round size up to 0x40.
- W1-E "size" column was based on hot-address range only. For struct-array layouts (ConnBrowserListLayout), the **per-entry** size is what matters; W1-E's range was an underestimate of total table size but an overestimate of the struct unit.
- The FrontendUiState/FrontendRenderResources SKIPS are honest: structification would not improve decompiles since the consumers don't use base+offset. The naming wins on those clusters have already been captured by T1-T5 sweeps.
- Bonus singleton renames (DAT_00496358 → g_currentScreenIndex, etc.) called out by W1-E §D are out of scope for this struct-typing task. Address them in a separate naming pass.
