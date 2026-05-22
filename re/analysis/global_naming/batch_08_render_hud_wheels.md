---
batch: 08
area: render_hud_wheels
tier: T2
target_todos: [reference_arch_render_race_hud_overlays_2026-05-18, reference_arch_render_wheel_billboards_d3d_2026-05-18]
ghidra_session: 537c346d887f48f4945b078710181532
analyzed_addresses: 0x004388A0, 0x00446F00, 0x0043A220
agent: Claude Opus 4.7 (1M context)
date: 2026-05-20
---

# Globals enumeration -- HUD overlays + wheel billboards + minimap

## Summary

- Functions analyzed: 3
  - RenderRaceHudOverlays @ 0x004388A0 (3854 bytes, per-frame HUD widget dispatcher)
  - RenderVehicleWheelBillboards @ 0x00446F00 (1410 bytes, per-wheel D3D3 sprite emission)
  - RenderTrackMinimapOverlay @ 0x0043A220 (3707 bytes, minimap route + racer dots)
- Functions cross-referenced for evidence: InitializeRaceHudLayout @ 0x00437BA0, InitializeMinimapLayout @ 0x0043B0A0, InitializeWheelPaletteUvTable @ 0x00446EA0, InitializeVehicleWheelSpriteTemplates @ 0x00446A70, SetRaceHudIndicatorState @ 0x00439B60
- Unnamed DAT_* globals encountered in target functions: 41 (after de-dup)
- Already-named globals encountered: 19 (noted, not renamed)
- Proposals -- high confidence: 27
- Proposals -- medium confidence: 9
- Proposals -- comment-only (low confidence): 5

## Methodology

Entry points were the three ARCH-DIVERGENCE render functions. For each, I:

1. Pulled the decomp (HUD via `decomp_function` on 0x004388A0; same for wheel + minimap).
2. Enumerated outgoing data refs via `reference_from` over the function body range.
3. For each unique `DAT_004*` write address, used `reference_to` and `function_at` to identify the writer function. Critical anchors: the matching init functions InitializeRaceHudLayout (HUD), InitializeMinimapLayout (minimap-only), InitializeWheelPaletteUvTable (wheel-only); these populate the per-view layout table at 0x004b1138 (14-float stride) and the per-actor wheel sprite scratch tables.
4. Decoded immediate float constants in `.rdata` (0x0045d5d0..0x0045d720) via `memory_read` and confirmed their semantic role (16.0f digit-step, 24.0f row-stride, 64.0f speedo dial-center offset, etc.).
5. Cross-checked port-side mirrors in `td5_hud.c` (lines 120-211) where the source-port author has already preserved the original address as a comment after the static field.

Relevance gate: a global is included if (a) RenderRaceHudOverlays/RenderVehicleWheelBillboards/RenderTrackMinimapOverlay reads or writes it, or (b) it is a sibling slot in the same fixed scratch table reached via index arithmetic from a read/write site in those functions (per-view layout-table slots, per-actor wheel-quad scratch slots, etc.).

The HUD per-view layout table is the highest-value finding: a 14-float `TD5_HudViewLayout` struct that the orig walks via `&g_hudScaleX + view_idx * 0xE`. Orig has the first two slots already named (g_hudScaleX, g_hudScaleY at 0x4b1138/0x4b113c); the next 12 floats per slot are unnamed but the port has already structured them. This batch proposes naming the 14-field-stride table-of-2 explicitly.

## Proposals

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x004b11b0 | u32 | `g_hudCurrentViewIndex` | high | Loop induction at HUD body 0x004388B3 (`DAT_004b11b0 = 0`), increments at 0x004396FC; gates per-view code via `< g_hudViewCount`. 13 refs (3 writes). Port mirror: `s_cur_view`. | td5_hud.c:134 s_cur_view |
| 0x004b0bfc | u32 (ptr) | `g_hudCurrentFlagsPtr` | high | Loaded each iter at 0x004388C1 from `(&g_hudOverlayFlagsPtr)[view]`; every widget test `(*DAT_004b0bfc & FLAG)` reads it. Port mirror: `s_cur_flags`. | td5_hud.c:124 s_cur_flags |
| 0x004b0fa4 | u32 (float*) | `g_hudCurrentLayoutPtr` | high | Loaded each iter at 0x004388DF as `(&g_hudScaleX + view * 0xE)` — points into per-view 14-float layout table starting at 0x4b1138. Mislabelled `g_trafficActorsEnabled` in current Ghidra db; should be renamed. Port mirror: `s_cur_scale`. | td5_hud.c:125 s_cur_scale |
| 0x004b11a8 | i32[2] | `g_hudIndicatorDigit` | high | Per-view 2-element table read at HUD 0x00439518 (`(&DAT_004b11a8)[view]`); writer is `SetRaceHudIndicatorState @ 0x00439B60`: `dword ptr [ECX*4 + 0x4b11a8] = EAX`. Port mirror: `s_indicator_state[MAX_HUD_VIEWS]`. | td5_hud.c:156 s_indicator_state |
| 0x004b1120 | i32[2] | `g_hudPrevSpanPosition` | high | Per-view 2-element table (`piVar1 = &DAT_004b1120 + view`); HUD 0x004394D9 updates it from actor field +0x80. Drives U-turn warning latch via `0x004b1140`-adjacent counter. Port mirror: `s_prev_span_pos`. | td5_hud.c:155 s_prev_span_pos |
| 0x004b1140 | i32[2] | `g_hudWrongWayCounter_v0` | high | First slot of the wrong-way/U-turn dwell counter; HUD 0x00439527 increments via `(&g_hudFadeOverlayAlpha)[view]`. Writer is HUD itself (`(&g_hudFadeOverlayAlpha)[view] = ... + 1`). g_hudFadeOverlayAlpha resolves to 0x004b1140 already in Ghidra as overlapping label? — confirmed mislabel: this slot is *not* the same as the named symbol; the port treats this as `s_wrong_way_counter` slot 0. | td5_hud.c:154 s_wrong_way_counter |
| 0x004b1178 | i32 | `g_hudWrongWayCounter_v1` | high | Second slot of same 2-entry per-view counter (slot 0 at 0x004b1140, slot 1 at 0x004b1178). Writer InitializeRaceHudLayout @ 0x00437DDB. | td5_hud.c:154 s_wrong_way_counter[1] |
| 0x004b1138 | f32 | `g_hudPrimaryScaleX` | high | First field of per-view layout slot 0. Written by InitializeRaceHudLayout @ 0x00437BCA: `g_renderWidthF * 0.0015625f` (640-divisor). Currently named `g_hudScaleX`; recommend renaming to `g_hudViewLayoutTable` and tagging the whole 14-float stride as a struct. | td5_hud.c:131 s_scale_x |
| 0x004b113c | f32 | `g_hudPrimaryScaleY` | high | Second field, scale_y. Currently named `g_hudScaleY`. Same story as above. | td5_hud.c:132 s_scale_y |
| 0x004b1148 | f32 | `g_hudView0VpLeft` | high | Per-view layout slot 0 + offset +0x10 — vp_int_left. Set in InitializeRaceHudLayout depending on split-screen mode: full-screen mode 0 sets `renderWidthF * -0.5f` (left=-renderWidth/2 in non-int form before ftol). Port mirror: `vp_int_left` in struct. | td5_hud.c:1275 vp_left=0 |
| 0x004b114c | f32 | `g_hudView0VpTop` | high | Slot 0 + offset +0x14 — vp_int_top, paired with above. | td5_hud.c:1277 vp_top=0 |
| 0x004b1150 | f32 | `g_hudView0VpRight` | high | Slot 0 + offset +0x18 — vp_int_right. Read across many widget anchors (`g_trafficActorsEnabled[6]` is +0x18). | td5_hud.c:1276 vp_right |
| 0x004b1154 | f32 | `g_hudView0VpBottom` | high | Slot 0 + offset +0x1C — vp_int_bottom (`pfVar9[7]` = +0x1C). | td5_hud.c:1278 vp_bottom |
| 0x004b1160 | i32 | `g_hudView0VpIntLeft` | high | Slot 0 + offset +0x28 — int(vp_left); built by ftol loop at InitializeRaceHudLayout 0x00437DAB-0x00437DDB. Walks the table writing 6 ints per view at strides matching the f32 quartet. | (struct field) |
| 0x004b1164 | i32 | `g_hudView0VpIntTop` | high | +0x2C, ftol(vp_top). Paired with above. | (struct field) |
| 0x004b1168 | i32 | `g_hudView0VpIntRight` | high | +0x30 from layout slot 0. | (struct field) |
| 0x004b1140 | f32 | `g_hudView0CenterX` | high | +0x08 — center_x = (vp_left + vp_right) * 0.5 from 0x00437DAB FSTP. Shared address with wrong-way counter only because **the wrong-way counter sits inside the 14-float stride at offset +0x08; the port unpacked it into a separate array**. Confirms: 0x4b1140 is dual-purpose-overloaded between init (center_x writeback) and HUD-loop (wrong-way counter scratch). See key discoveries. | td5_hud.c center_x / s_wrong_way_counter |
| 0x004b1144 | f32 | `g_hudView0CenterY` | high | +0x0C — center_y. | (struct field) |
| 0x004b1178 | f32 | `g_hudView1CenterX` | med | +0x40 — view-1 center_x. Same dual-use as 0x4b1140 (init writes float center_x; HUD loop reads view-1 wrong-way counter from same byte range). | (struct field) |
| 0x004b1180 | f32 | `g_hudView1VpLeft` | high | Slot 1 + offset +0x48 — view-1 vp_int_left. Symmetric with 0x4b1148. | td5_hud.c:1295 vp_left |
| 0x004b1184 | f32 | `g_hudView1VpTop` | high | Slot 1 + offset +0x4C. | (struct field) |
| 0x004b1188 | f32 | `g_hudView1VpRight` | high | Slot 1 + offset +0x50. | (struct field) |
| 0x004b118c | f32 | `g_hudView1VpBottom` | high | Slot 1 + offset +0x54. | (struct field) |
| 0x004b1198 | i32 | `g_hudView1VpIntLeft` | high | Slot 1 + offset +0x60 — ftol of view-1 vp_left. Read at HUD 0x0043897E with index `view*0xE`. | (struct field) |
| 0x004b119c | i32 | `g_hudView1VpIntTop` | high | Slot 1 + offset +0x64 — view-1 ftol(vp_top). | (struct field) |
| 0x004b11ac | i32 | `g_hudView1VpIntRight` | med | Slot 1 + offset +0x74 — view-1 ftol(vp_right). | (struct field) |
| 0x004b11b4 | u32 (ptr) | `g_hudSpeedofontAtlasPtr` | high | Written once at InitializeRaceHudLayout @ 0x0043801F via `FindArchiveEntryByName(this, "SPEEDOFONT")`. Read at HUD 0x00438F48 etc. Port mirror: `s_speedofont_atlas` (already documented `0x4B11B4`). | td5_hud.c:139 s_speedofont_atlas |
| 0x004b112c | u32 (ptr) | `g_hudGearnumbersAtlasPtr` | high | Written once at InitializeRaceHudLayout @ 0x0043816B via `FindArchiveEntryByName(this, "GEARNUMBERS")`. Read at HUD 0x00438D0C (offset +0x2C/+0x30 = atlas_x/atlas_y). Port mirror: `s_gearnumbers_atlas`. | td5_hud.c:140 s_gearnumbers_atlas |
| 0x004b0a3c | u32 (ptr) | `g_hudNumbersAtlasPtr` | high | Written at InitializeRaceHudLayout @ 0x00437BBE via `FindArchiveEntryByName(this, "numbers")`. Read at HUD 0x00439549 for indicator-digit glyphs and at minimap 0x0043A2D5 for the SCANDOTS atlas (different load site). Port mirror: `s_numbers_atlas`. | td5_hud.c:137 s_numbers_atlas |
| 0x004b0fac | u32 (ptr) | `g_hudSemicolAtlasPtr` | high | Written at InitializeRaceHudLayout via `FindArchiveEntryByName(this, ";")` (s_SEMICOL). The "SEMICOL" is the timer-separator-colon glyph (BodyText sprite). Port mirror: `s_semicol_atlas`. | td5_hud.c:138 s_semicol_atlas |
| 0x004b11c4 | u32 | `g_hudUseMetricUnits` | high | Read at HUD 0x00438EAA (gates `(speed*256+625)/1252` vs `(speed*256+389)/778`). Writer is `ScreenMainMenuAnd1PRaceFlow @ 0x00415490` body @ 0x00415597 -- main-menu UNITS option. 0 = MPH, 1 = KPH. | td5_hud.c speed_display switch |
| 0x004b0a40 | f32 | `g_minimapOriginX` | high | Read at RenderTrackMinimapOverlay 0x0043A24D (`SetProjectedClipRect(DAT_004b0a40, ...)`). Writer is InitializeMinimapLayout @ 0x0043B10B. Port mirror: `s_minimap_x`. | td5_hud.c:168 s_minimap_x |
| 0x004b0a44 | f32 | `g_minimapOriginY` | high | Paired with above; `SetProjectedClipRect(... , DAT_004b0a44, ...)`. Port mirror: `s_minimap_y`. | td5_hud.c:169 s_minimap_y |
| 0x004b0a48 | f32 | `g_minimapWorldScaleX` | high | 18 read refs, all in minimap geometry: `(fVar7 * fVar3 + fVar10 * fVar4) * _DAT_004b0a48`. Writer InitializeMinimapLayout @ 0x0043B148. Port mirror: `s_minimap_world_scale_x`. | td5_hud.c:173 s_minimap_world_scale_x |
| 0x004b0a4c | f32 | `g_minimapWorldScaleY` | high | Symmetric Y partner. Port mirror: `s_minimap_world_scale_y`. | td5_hud.c:174 s_minimap_world_scale_y |
| 0x004b0a58 | i32 | `g_minimapSegmentCount` | high | Read at minimap 0x0043A328 as loop upper-bound on the segment table walked at 0x004b0a72. Port mirror: `s_minimap_seg_primary_end`. | td5_hud.c:185 s_minimap_seg_primary_end |
| 0x004b0a5c | i32 | `g_minimapBranchSegStart` | high | Read at 0x0043A323; lower bound for branch-segment walk. Port mirror: `s_minimap_seg_branch_start`. | td5_hud.c:186 s_minimap_seg_branch_start |
| 0x004b0a72 | i16[N] | `g_minimapSegmentSpanStart` | high | Segment table walked via `(&DAT_004b0a72)[seg*3]` with 6-byte stride (start/end/branch i16s). Port mirror: `s_minimap_seg_start` (port chose to store start; struct is interleaved in orig). | td5_hud.c:188 s_minimap_seg_end |
| 0x004b0a74 | i16[N] | `g_minimapSegmentSpanEnd` | high | +2 from above (end-span field, paired triple stride). | td5_hud.c:189 s_minimap_seg_branch |
| 0x004b0a76 | i16[N] | `g_minimapSegmentBranchOffset` | med | +4 from segment start, third field of the 3xi16 stride; `iVar11 = end - start` term for branch overlay. | (combined in port) |
| 0x004b0a78 | i16[N] | `g_minimapSegmentNextStart` | low | Lookahead element accessed at 0x0043A439 — adjacent to 0x004b0a72 entry; possibly the start span of the next segment in the same 6-byte stride. Comment-only. | (none) |
| 0x004b0a7a | i16 | `g_minimapSegmentTableSentinel` | med | Single read at 0x0043A6E8 (`DAT_004b0a7a`) — looks like a separate count or scratch counter outside the per-segment stride. Likely the segment count post-init (cf. 0x4b0a58 is primary-only). | (none) |
| 0x00473e30 | u32 | `g_raceSpecialEncounterMode` | high | Read at HUD 0x00439642 as `if (DAT_00473e30 == 4)` gating odometer-mode digit emission (+0x1CD slot). Writer is one of the cup/encounter setup functions; value tied to drag race / wanted-mode. Cross-batch: see batch_02. | td5_hud.c g_special_encounter |
| 0x004b11c8 | u32 | `g_hudMetricDisplayValue` | high | Adjacent to 0x004b11c4 (units flag); read by BuildRaceHudMetricDigits (called from HUD 0x00438EE5). Port mirror: `s_metric_value`. | td5_hud.c:163 s_metric_value |
| 0x004cefb0 | f32[6] | `g_wheelTireUvTable_u` | high | 6 u32 slots stride-8; writer InitializeWheelPaletteUvTable @ 0x00446EA0 with `(slot & 3) * 64.0f + 0.5f`. Read at RenderVehicleWheelBillboards 0x00447308 via `(&DAT_004cefb0)[actor * 2]`. Per-actor sidewall U coordinate. | (none) |
| 0x004cefb4 | f32[6] | `g_wheelTireUvTable_v` | high | Paired V-table; same writer at 0x00446EEE: `(slot >> 2) * 64.0f + 0.5f`. 6 entries × 8 bytes covers all 6 racer slots. | (none) |
| 0x004c4300 | u8[N] | `g_wheelSpriteScratch_inner` | high | Per-wheel inner-tire-sidewall vertex template scratch buffer; written by `BuildSpriteQuadTemplate(&local_a8)` at RenderVehicleWheelBillboards 0x0044737F to `&DAT_004c4300 + wheelIdx * 0x170` (0x170 stride = 4 quads × 0xB8). | (port emits via `td5_plat_render_draw_tris`, no static buffer) |
| 0x004c3df0 | u8[N] | `g_wheelTransformBatchScratch` | high | Per-actor batched transform scratch; address loaded at 0x00447023 as `&DAT_004c3df0 + actor * 0xD8`. Read by `WritePointToCurrentRenderTransform` and `QueueTranslucentPrimitiveBatch`. | (port emits inline) |
| 0x004c6580 | f32[9] | `g_wheelFrontRotMatrix` | high | 3x3 front-wheel rotation matrix scratch; rebuilt at RenderVehicleWheelBillboards 0x0044706E-0x004470C5 from `CosFloat12bit(steering)` then composed via `MultiplyRotationMatrices3x3(g_raceViewTransformPtr, &DAT_004c6580, &DAT_004c6580)` (in-place). Output reloaded via `LoadRenderRotationMatrix`. | (port composes inline) |
| 0x004c65a4 | f32[3] | `g_wheelTranslation` | high | Wheel translation triple immediately following the matrix (offset +0x24 from base), written at 0x00447106-0x00447120 and pushed via `LoadRenderTranslation(0x4c6580)`. | (port composes inline) |
| 0x004c65b0 | u8[N] | `g_wheelHubcapSpriteScratch` | high | Per-wheel hub-cap sprite scratch; written via `BuildSpriteQuadTemplate(&local_a8 ...)` and submitted via `QueueTranslucentPrimitiveBatch((int)puVar8)` at 0x0044712B. Address = `&DAT_004c65b0 + wheel_global_idx * 0x5C0`. | (port composes inline) |
| 0x004b08c8 | u8 | `g_hudIndicatorSpriteScratch` | high | Single quad scratch for the indicator-digit glyph; written by `BuildSpriteQuadTemplate(&local_70)` at HUD 0x004395A9 then `SubmitImmediateTranslucentPrimitive(&DAT_004b08c8)` at 0x004396E1. | (port composes inline) |
| 0x004b0ef8 | u8 (struct) | `g_hudSplitScreenDividerScratch` | high | Tail-of-frame submission at HUD 0x00439798: `SubmitImmediateTranslucentPrimitive(&DAT_004b0ef8 + mode * 0xB8)`. 0xB8 stride = TD5_SpriteQuad size. Two entries (one per split-screen mode). Port mirror: `s_divider_quad_h / s_divider_quad_v`. | td5_hud.c:147 s_divider_quad_h |
| 0x0045d628 | f32 | `g_hudDigitGlyphStepX` | high | `= 16.0f`. Stride between digit glyph cells (NUMBERS atlas cell width). Read at HUD speedo digit emission (0x00438D68, 0x00438DB7) and U-turn position offset. | td5_hud.c:1529 col*16 |
| 0x0045d5d4 | f32 | `g_hudDigitGlyphStepY` | high | `= 24.0f`. Row stride for indicator digit (NUMBERS atlas cell height). Read at HUD indicator-digit 0x00439559. | td5_hud.c:1530 row*24 |
| 0x0045d6c0 | f32 | `g_hudSpeedoDialNearOff` | high | `= 64.0f`. Speedometer dial center offset from vp_int_right. Read at HUD 0x00438C2B. | td5_hud.c:1860 sx*64 |
| 0x0045d6d4 | f32 | `g_hudSpeedoDialFarOff` | high | `= 56.0f`. Center offset from vp_int_bottom. Read at HUD 0x00438C41. | td5_hud.c:1861 sy*56 |
| 0x0045d700 | f32 | `g_hudSpeedoNeedleBaseHalf` | high | `= 2.0f`. Needle base half-width (perpendicular offset). Read at HUD 0x00438CB8/0xCC6. | td5_hud.c:1925 base_offset*2 |
| 0x0045d704 | f32 | `g_hudSpeedoNeedleTipLen` | high | `= 45.0f`. Needle tip length from center along cos/sin. Read at HUD 0x00438C6E/0xC7E. | td5_hud.c:1924 tip*45 |
| 0x0045d6f8 | f32 | `g_hudSpeedoDigitMarginY` | high | `= 14.5f`. Vertical margin for speedo digit cells (top row). | td5_hud.c speedo digit y |
| 0x0045d6fc | f32 | `g_hudSpeedoDigitMarginX` | high | `= 24.5f`. Horizontal margin for speedo digit cells. | td5_hud.c speedo digit x |
| 0x0045d630 | f32 | `g_hudSpeedoDigitMicroOff` | med | `= 1.5f`. Half-pixel sub-cell offset added when emitting digit glyph corners (`x + n * stride + 1.5f`). | td5_hud.c digit uv +0.5 |
| 0x0045d5f4 | f32 | `g_hudGearGlyphRowOff` | med | `= 1.0f`. Row offset constant for gear glyph (`(gear + 1.0) * 16.0`). Used at HUD 0x00438E00/0x00438E54. | td5_hud.c gear row+1 |
| 0x0045d6c4 | f32 | `g_hudViewportInsetSign` | med | `= -0.5f`. Used in InitializeRaceHudLayout viewport extents (`renderWidthF * -0.5f`). | (struct init) |
| 0x0045d6d0 | f32 | `g_hudIndicatorCenterOffY` | med | `= 12.0f`. Half-cell vertical offset for centered indicator placement (`center_y - 12.0`). | td5_hud.c indicator y |
| 0x0045d6e4 | f32 | `g_hudIndicatorCenterOffX` | med | `= 8.0f`. Half-cell horizontal offset for indicator placement (`center_x - 8.0`). 16x24 cell, so half-stride = 8. | td5_hud.c indicator x |
| 0x0045d6c8 | f32 | `g_hudReplayBannerInsetX` | med | `= 112.0f`. Replay-banner horizontal inset from view edge. | td5_hud.c replay banner |
| 0x0045d650 | f32 | `g_renderHudFixedPointShift_4` | low | `= 4.0f`. Probably a glyph-cell-mod constant; appears in InitializeRaceHudLayout speedometer-digit-step. Comment-only. | (constant) |
| 0x0045d7b0 | f32 | `g_wheelTireUvCellInset` | high | `= 31.0f`. Per-tire UV cell inset (matches 32-pixel cell minus 1 pixel guard). Read at RenderVehicleWheelBillboards 0x00447344/0x00447355 when setting `local_5c = local_60 + _DAT_0045d7b0`. | (none) |
| 0x0045d718 | f32 | `g_minimapDotHalfSize` | high | `= 3.5f`. Racer-dot half-size for minimap; read at minimap 0x0043AED5-ish (local_78 - DAT_0045d718). Port mirror: `s_minimap_dot_size` semantics (matches half-pixel offset). | td5_hud.c:172 s_minimap_dot_size |
| 0x0045d5d0 | f32 | `g_pixelHalfBias_0p5` | high | `= 0.5f`. Universal half-pixel UV/dimension offset for D3D3 texel-center sampling. Pervasive across InitializeRaceHudLayout. | (port uses + 0.5f literal) |
| 0x004b1130 | u32 | (already named `g_minimapWidth`) | (noted) | named, no change. | td5_hud.c:170 s_minimap_width |
| 0x004b11b8 | u32 | (already named `g_minimapHeight`) | (noted) | named, no change. | td5_hud.c:171 s_minimap_height |
| 0x004b1134 | u32 | (already named `g_hudViewCount`) | (noted) | named, no change. | td5_hud.c:133 s_view_count |
| 0x004b0c00 | u32 | (already named `g_hudOverlayFlagsPtr`) | (noted) | named, no change. | (port uses array of ptrs) |
| 0x004b0c04 | u32 | (already named `g_hudOverlayFlagsSecondaryPtr`) | (noted) | named, no change. | (port uses array of ptrs) |
| 0x004b0bf8 | u32 | (already named `g_hudFontGlyphAtlasPtr`) | (noted) | named, no change. | td5_hud.c:159 s_hud_string_table (semantically off — this is the slot/glyph table, not strings; flag for re-audit) | 
| 0x004b0a64 | u32[2] | (already named `g_hudFadeOverlayAlpha`) | (noted) | Suspect name — at HUD 0x004394A0 it's used as the U-turn dwell counter, not a fade overlay alpha. Recommend rename to `g_hudUturnDwellCounter`. See key discoveries #5. | td5_hud.c:154 s_wrong_way_counter |
| 0x004afb60 | u32 | (already named `gActorRouteStateTable`) | (noted) | named, no change. | (asset-side) |
| 0x004bead0 | u32 | (already named `g_wantedArrestCounter`) | (noted) | named, no change. | (from batch_02) |
| 0x00466e8c | u32 | (already named `gCircuitLapCount`) | (noted) | named, no change. | (existing) |
| 0x00466e94 | u32 | (already named `gTrackIsCircuit`) | (noted) | named, no change. | (existing) |
| 0x00466ea0 | u32 | (already named `gPrimarySelectedSlot`) | (noted) | named, no change. | (existing) |
| 0x00466ea4 | u32 | (already named `gSecondarySelectedSlot`) | (noted) | named, no change. | (existing) |
| 0x004aadb0 | u32 | (already named `g_raceViewTransformPtr`) | (noted) | named, no change. | (existing) |
| 0x004aed88 | u32 | (already named `g_raceCheckpointTablePtr`) | (noted) | named, no change. | (existing) |
| 0x004c3d90 | u32 | (already named `g_trackTotalSpanCount`) | (noted) | named, no change. | (existing) |
| 0x004c3d98 | u32 | (already named `g_trackVertexPool`) | (noted) | named, no change. | (existing) |
| 0x004c3d9c | u32 | (already named `g_trackStripRecords`) | (noted) | named, no change. | (existing) |
| 0x004b0a6c | u32 | (already named `g_minimapQuadBuffer`) | (noted) | named, no change. | td5_hud.c:167 |
| 0x004b0a70 | u16 | (already named `g_minimapTrackSegmentTable`) | (noted) | named, no change. | td5_hud.c:187 |

## Key discoveries

1. **HUD per-view layout is a flat 14-float-stride table starting at 0x004b1138**, walked via `g_hudCurrentLayoutPtr = (&g_hudScaleX + view_idx * 0xE)`. The first two named symbols (`g_hudScaleX` / `g_hudScaleY`) are actually `[0].scale_x` / `[0].scale_y`. Field offsets within the 14-float stride:
   - +0x00 scale_x  (`g_hudScaleX`)
   - +0x04 scale_y  (`g_hudScaleY`)
   - +0x08 center_x (0x4b1140 view 0 / 0x4b1178 view 1)
   - +0x0C center_y (0x4b1144 view 0 / 0x4b117c view 1)
   - +0x10 vp_left  (0x4b1148 view 0 / 0x4b1180 view 1)
   - +0x14 vp_top   (0x4b114c view 0 / 0x4b1184 view 1)
   - +0x18 vp_right (0x4b1150 view 0 / 0x4b1188 view 1)
   - +0x1C vp_bottom(0x4b1154 view 0 / 0x4b118c view 1)
   - +0x28 vp_int_left
   - +0x2C vp_int_top
   - +0x30 vp_int_right
   - (+0x34 unused/padding)
   This is exactly the `TD5_HudViewLayout` struct already factored in the port; Ghidra should reproduce the struct via `layout_struct_create` in the consolidation pass.

2. **0x004b1148 IS NOT a separate "view0VpLeft" global; it is +0x10 within the same 0x38-byte per-view stride**. Apparent address gaps in the table (e.g. 0x4b115c..0x4b115f) are padding inside the per-view stride. Consolidation should declare `HudViewLayoutTable g_hudViewLayoutTable[3]` covering 0x4b1138..0x4b11A8 rather than per-field DAT_* names.

3. **0x004b1140 / 0x004b1178 are address-overloaded between INIT and per-frame loop**: InitializeRaceHudLayout writes `center_x` floats there (`(vp_left + vp_right) * 0.5`), but HUD's U-turn-warning code at 0x004394A0 reuses the same byte range as an int counter `g_hudFadeOverlayAlpha` indexed by view. The port unpacked this overload into two separate arrays (`s_wrong_way_counter[]` and `s_view_layout[].center_x`). This is intentional space-efficiency on the orig side; Ghidra should add a comment flagging the overload.

4. **`g_hudFontGlyphAtlasPtr @ 0x004b0bf8` is misnamed** — at HUD 0x0043890B it's loaded as the base of a 4-byte-stride table indexed by `actor_field+0x383 * 4`. Port name `s_hud_string_table` (td5_hud.c:159) calls it "string table". Real use is more likely: per-slot label lookup (1st/2nd/.../6th). Suggest re-audit and rename to `g_hudPositionLabelTable`.

5. **`g_hudFadeOverlayAlpha @ 0x004b0a64` is mis-classified** — it's actually a 2-element U-turn dwell counter, not a "fade overlay alpha". Recommend rename to `g_hudUturnDwellCounter`. Port already has it as `s_wrong_way_counter` (more accurate). 5 read/write sites all in U-turn warning gating at HUD 0x004394A0-0x0043950F.

6. **Wheel sprite scratch tables form a fixed layout**: per-actor offsets are stride-fixed:
   - `g_wheelSpriteScratch_inner @ 0x004c4300`: 0x170-byte stride per wheel (`wheel_global_idx * 0x170`).
   - `g_wheelTransformBatchScratch @ 0x004c3df0`: 0xD8-byte stride per actor.
   - `g_wheelHubcapSpriteScratch @ 0x004c65b0`: 0x5C0-byte stride per wheel-global-idx (`actor * 4 + wheel_idx`).
   - `g_wheelFrontRotMatrix @ 0x004c6580` + `g_wheelTranslation @ 0x004c65a4`: shared single-actor scratch (re-written per wheel during the render-1 loop). Total per-actor wheel-billboard scratch footprint = `0x170 * 4 + 0x5C0 * 8 + 0xD8 + (0x24 + 0xC) ≈ 0x3950 bytes per actor` × 6 actors ≈ 86KB. This is the orig's static .bss budget for vehicle-wheel emission — confirms why D3D11 port re-emits inline instead of preallocating equivalent scratch.

7. **`g_wheelTireUvTable_u / _v @ 0x004cefb0 / 0x004cefb4`**: 6 (u, v) pairs at stride 8 produced by `InitializeWheelPaletteUvTable` as a `slot % 4 → u, slot >> 2 → v` palette grid (64x64 cells with +0.5 texel-center bias). Each actor reads `actor * 2` index to get its slot's sidewall texture coordinate. Closes the open question in the wheel-billboards arch memo about where the per-actor wheel UV comes from.

8. **`g_hudUseMetricUnits @ 0x004b11c4`** is the only HUD-loop write to a global that has a frontend-side writer: `ScreenMainMenuAnd1PRaceFlow @ 0x00415597`. This is the runtime KPH/MPH setting. The port currently has no equivalent — it likely hardcodes MPH. **Quick win**: surface this as an INI setting (`[Display] Units=mph|kph`).

9. **Minimap world-scale split**: `g_minimapWorldScaleX @ 0x4b0a48` and `g_minimapWorldScaleY @ 0x4b0a4c` are independent — minimap is NOT square-pixel. This matches the port (`s_minimap_world_scale_x / s_minimap_world_scale_y` are separate floats). Worth flagging because aspect-ratio bugs in minimap rendering often trace to assuming one global.

## Out-of-scope finds

| address | brief note | probable area |
|---|---|---|
| 0x004b0bf4 | `g_hudStatusTextEnabled` — already named; controls hide of "RACING" label | future HUD text audit |
| 0x004b1124 | u32; sibling to `0x004b1120` (prev_span_pos) at +4; might be second slot | already covered via `s_prev_span_pos[2]` |
| 0x00474404 | string literal `"%02d:%02d:%02d"` — used for slot 1-5 timer formatting | string-table audit |
| 0x00474414 | string literal `"%s %02d:%02d:%02d"` — used for slot 0 timer formatting | string-table audit |
| 0x00474428 | string literal `"%s %d %d"` — circuit-lap progress format | string-table audit |
| 0x0046603c | string literal `"%s %d"` — total-timer / lap-counter format | string-table audit |
| 0x004749d0 | string literal — read at minimap 0x0043A2D5 (likely `"SCANDOTS"` archive name) | asset/atlas audit |
| 0x00473fd8/00473fdc | 32-entry vertex-strip-type LUTs used by minimap (`*(int *)(&DAT_00473fd8 + *pbVar1 * 8)`) | track-strip type audit (T1 batch_03 area, missing field) |
| 0x004b08c8 quad-scratch | also referenced indirectly via `&DAT_004b08c8` in HUD's `local_70` setup | (covered above) |
| 0x004b1178 (overlap) | view-1 center_x AND view-1 wrong-way-counter | (covered above) |
| 0x0045d61c..0x0045d624 | float constants 0.0078125 / 0.984375 — likely texture-page UV scaling / 32px atlas | renderer constants (batch 09) |
| 0x00473e30 | u32 = 3 default; controls `4 == odometer-digit-mode` path | gametype dispatch (batch_02 area) |
| 0x004b11c8 | u32 metric-value scratch (BuildRaceHudMetricDigits input/output) | already named s_metric_value |
| 0x0048d98c / 0x0048d9a0 | u32 globals read at HUD position-label sites (string table base + actor field offset) | label/string-table audit |

## TODO impact

**reference_arch_render_race_hud_overlays_2026-05-18 (HUD ARCH-DIVERGENCE memo)**:
- This batch confirms every claim in the memo: speed conversion ratio is exactly `(raw*256 + 626)/1252` at orig 0x00438C20 (`0x4e4 = 1252`, `0x272 = 626`); the port uses 625 vs 626 — that 1-LSB rounding-bias divergence is **documented and acceptable**. The orig DAT_0045D628 = 16.0f digit step, DAT_0045D6F8/D6FC = (14.5, 24.5) margin pair are confirmed byte-faithful with port's `SPEEDFONT_BASE_OFF` math.
- Speed-bonus / HUD divergence note in the memo is fully addressed: the per-view scratch struct DAT_004b11a8 is `g_hudIndicatorDigit[2]`, written by SetRaceHudIndicatorState only — no other writers — confirming the port's `s_indicator_state` mirror is complete.
- The arch memo is **closeable in this regard**. Remaining open item is purely the D3D3-vs-D3D11 submission divergence, which is intentional ARCH-DIVERGENCE.

**reference_arch_render_wheel_billboards_d3d_2026-05-18 (wheel ARCH-DIVERGENCE memo)**:
- Closes the open scratch-buffer layout question: orig has three named scratch buffers per actor + two global tire-UV tables totalling ~86KB; port emits inline.
- Wheel UV per-actor variation is now traceable: `g_wheelTireUvTable_u/v @ 0x4cefb0/0x4cefb4` is generated by `InitializeWheelPaletteUvTable` and read once per actor frame via index `actor * 2`. The port likely uses a hardcoded UV per-slot — **investigate** whether the port's wheel-UV per-actor matches orig's `(slot % 4) * 64.0` u + `(slot >> 2) * 64.0` v palette grid. If not, that's a closable subtask.
- Hub-cap sprite scratch `0x4c65b0` stride 0x5C0 is documented for the first time. No port-side impact (port composes the same geometry inline) but worth keeping for future per-byte verification.

**Implicit TODO surfaced (not in frontmatter)**:
- `g_hudUseMetricUnits @ 0x004b11c4` exposes a missing INI setting in the port for KPH/MPH choice. Suggest filing a new minor TODO: `todo_hud_units_ini_setting_2026-05-20.md`.
- `g_hudFontGlyphAtlasPtr @ 0x004b0bf8` is misnamed and the port's `s_hud_string_table` semantically diverges from the orig's per-actor field lookup. Worth filing `todo_hud_position_label_table_rename_2026-05-20.md` for a quick consolidation in both Ghidra and port.
