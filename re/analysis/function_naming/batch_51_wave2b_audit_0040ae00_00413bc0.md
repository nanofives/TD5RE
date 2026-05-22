---
batch: 51
area: wave2b_audit_comment_sync
tier: T6
target_todos: []
ghidra_session: (per-batch, ghidra-apply opens master)
analyzed_addresses: 0x0040ae00,0x0040aec0,0x0040aef0,0x0040af10,0x0040af50,0x0040b0d8,0x0040b0e1,0x0040b170,0x0040b580,0x0040b6b0,0x0040b6cc,0x0040b6e8,0x0040ba60,0x0040bae0,0x0040c5cc,0x0040c7e0,0x0040cd10,0x0040cdc0,0x0040d120,0x0040d190,0x0040d590,0x0040d750,0x0040d830,0x0040dac0,0x0040daf0,0x0040df4a,0x0040f184,0x0040f744,0x0040fe00,0x004100c0,0x004100ce,0x004100d7,0x004100de,0x004100fa,0x00410111,0x00410129,0x00410165,0x00410380,0x0041043c,0x004104b2,0x00410527,0x00410599,0x00410613,0x00410940,0x00411710,0x00411a50,0x00411a70,0x00411de0,0x00411e00,0x00411e90,0x00412030,0x004122f0,0x004127b0,0x004129b0,0x00412b00,0x00412d50,0x00413010,0x00413580,0x00413b60,0x00413bc0
agent: claude-opus-4-7
date: 2026-05-22
---

# Wave 2B audit-comment sync — batch 51 (60 addresses)

## Summary

- Addresses in this batch: 60
- All proposals are confidence=low (comment-only, no rename)
- Each address receives a consolidated PLATE comment derived from port-source audit headers

## Methodology

Pure derived from `re/analysis/followup_sessions/wave1_f_comment_sync_plan.csv` —
port [CONFIRMED @ ...] / [ARCH-DIVERGENCE: ...] audit-header references extracted
and deduped per address. Comment text combines unique header summaries.

## Proposals (functions)

| address | current_name | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x0040ae00 | _unchanged_ | _unchanged_ | low | [ARCH-DIVERGENCE: D3D3 EndScene -> D3D11 platform-abstracted end-frame; L5 sweep 2026-05-21] | td5_render.c:1187 |
| 0x0040aec0 | _unchanged_ | _unchanged_ | low | [ARCH-DIVERGENCE: D3D3 ReleaseRaceRenderResources -> D3D11 abstracted shutdown; L5 sweep 2026-05-21] | td5_render.c:1112 |
| 0x0040aef0 | _unchanged_ | _unchanged_ | low | 0x0040AEF0  GetEnvironmentTexturePageMode  [ARCH-DIVERGENCE: D3D3 device-state query removed; L5 sweep 2026-05-21] | td5_asset.c:3038 |
| 0x0040af10 | _unchanged_ | _unchanged_ | low | [ARCH-DIVERGENCE: D3D3 DXD3D::CanFog() probe removed; L5 sweep 2026-05-21] Mirrors ConfigureRaceFogColorAndMode @ 0x0040AF10. Orig stores | td5_render.c:1243 |
| 0x0040af50 | _unchanged_ | _unchanged_ | low | [ARCH-DIVERGENCE: D3D3 SetRenderState calls -> D3D11 platform call; L5 sweep 2026-05-21] | td5_render.c:1222 |
| 0x0040b0d8 | _unchanged_ | _unchanged_ | low | D3DRS_ZFUNC        (0x17) = 8 (D3DCMP_ALWAYS)  [CONFIRMED @ 0x0040b0d8] D3DRS_ZWRITEENABLE (0x0E) = 0                  [CONFIRMED @ 0x0040b0e1] | td5_platform_win32.c:2397 |
| 0x0040b0e1 | _unchanged_ | _unchanged_ | low | D3DRS_ZWRITEENABLE (0x0E) = 0                  [CONFIRMED @ 0x0040b0e1] | td5_platform_win32.c:2398 |
| 0x0040b170 | _unchanged_ | _unchanged_ | low | 0x0040B170  SelectConfiguredDisplayModeSlot  [ARCH-DIVERGENCE: DispMode] | td5_frontend.c:10429 |
| 0x0040b580 | _unchanged_ | _unchanged_ | low | [ARCH-DIVERGENCE @ 0x0040B580 SetRaceTexturePageLoader] Orig is a 13-byte one-liner that writes a function pointer at | td5_game.c:156 |
| 0x0040b6b0 | _unchanged_ | _unchanged_ | low | type 0 → ALPHABLENDENABLE=0 (opaque)                  [CONFIRMED @ 0x0040B6B0] type 1 → ALPHABLENDENABLE=1, SRCALPHA/INVSRCALPHA     [CONFIRMED @ 0x00 | td5_render.c:2723 |
| 0x0040b6cc | _unchanged_ | _unchanged_ | low | type 1 → ALPHABLENDENABLE=1, SRCALPHA/INVSRCALPHA     [CONFIRMED @ 0x0040B6CC] type 2 → same D3D state as type 1, no ZWRITE write    [CONFIRMED @ 0x00 | td5_render.c:2724 |
| 0x0040b6e8 | _unchanged_ | _unchanged_ | low | type 3 → ALPHABLENDENABLE=1, ONE/ONE (additive)       [CONFIRMED @ 0x0040B6E8] | td5_render.c:2726 |
| 0x0040ba60 | _unchanged_ | _unchanged_ | low | [ARCH-DIVERGENCE: struct vs raw-byte texture-page pool; L5 sweep 2026-05-21] Mirrors ResetTexturePageCacheState @ 0x0040BA60. Orig walks raw byte arra | td5_render.c:2661 |
| 0x0040bae0 | _unchanged_ | _unchanged_ | low | 0x0040BAE0  PreloadLevelTexturePages  [ARCH-DIVERGENCE: on-demand texture streaming removed; L5 sweep 2026-05-21] | td5_asset.c:3043 |
| 0x0040c5cc | _unchanged_ | _unchanged_ | low | #define SHADOW_VIEW_Y_OFFSET    (-22.0f)   /* [CONFIRMED @ 0x40C5CC] push shadow below car in view-space */ | the car, not at wheel-probe height. [CONFIRMED @ 0x40C5CC] */ vy += SHADOW_VIEW_Y_OFFSET; | td5_render.c:3994 |
| 0x0040c7e0 | _unchanged_ | _unchanged_ | low | 0x0040C7E0  BuildSpecialActorOverlayQuads         [ARCH-DIVERGENCE: D3D3 Templates] | td5_render.c:6101 |
| 0x0040cd10 | _unchanged_ | _unchanged_ | low | 0x0040CD10  UpdateActorTrackLightState     [ARCH-DIVERGENCE: TrackLight] 0x0042FE20  BlendTrackLightEntryFromStart  [ARCH-DIVERGENCE: TrackLight] | td5_render.c:6034 |
| 0x0040cdc0 | _unchanged_ | _unchanged_ | low | 0x0040CDC0  CrossFade16BitSurfaces (variant 1)  [ARCH-DIVERGENCE: CrossFade] 0x0040D120  AdvanceCrossFadeTransition          [ARCH-DIVERGENCE: CrossFa | td5_render.c:5981 |
| 0x0040d120 | _unchanged_ | _unchanged_ | low | 0x0040D120  AdvanceCrossFadeTransition          [ARCH-DIVERGENCE: CrossFade] 0x0040D190  CrossFade16BitSurfaces (variant 2)  [ARCH-DIVERGENCE: CrossFa | td5_render.c:5982 |
| 0x0040d190 | _unchanged_ | _unchanged_ | low | 0x0040D190  CrossFade16BitSurfaces (variant 2)  [ARCH-DIVERGENCE: CrossFade] | td5_render.c:5983 |
| 0x0040d590 | _unchanged_ | _unchanged_ | low | 0x0040D590  LoadExtrasGalleryImageSurfaces       [ARCH-DIVERGENCE: Gallery] 0x0040D750  AdvanceExtrasGallerySlideshow        [ARCH-DIVERGENCE: Gallery | td5_frontend.c:10456 |
| 0x0040d750 | _unchanged_ | _unchanged_ | low | 0x0040D750  AdvanceExtrasGallerySlideshow        [ARCH-DIVERGENCE: Gallery] 0x0040D830  UpdateExtrasGalleryDisplay           [ARCH-DIVERGENCE: Gallery | td5_frontend.c:10457 |
| 0x0040d830 | _unchanged_ | _unchanged_ | low | 0x0040D830  UpdateExtrasGalleryDisplay           [ARCH-DIVERGENCE: Gallery] 0x00417DD2  LoadFrontendExtrasGalleryResources   [ARCH-DIVERGENCE: Gallery | td5_frontend.c:10458 |
| 0x0040dac0 | _unchanged_ | _unchanged_ | low | === Path 1: Quick Race (gameType == 2, Era) [CONFIRMED @ 0x0040dac0] === Original loop body consumes THREE rand() calls per iteration: | === Path 2: Cup/Masters (gameType == 5) [CONFIRMED @ 0x0040dac0] === Scans s_masters_roster_flags[] for state==1 entries, claims them (sets to 2), | === Path 3: Default (single race, all other types) [CONFIRMED @ 0x0040dac0] === Faithful port of the original's... | td5_frontend.c:1714 |
| 0x0040daf0 | _unchanged_ | _unchanged_ | low | Two-player setup [CONFIRMED @ 0x0040daf0]: Original gate: g_twoPlayerModeEnabled != 0 // g_selectedGameType == 7. | td5_frontend.c:1648 |
| 0x0040df4a | _unchanged_ | _unchanged_ | low | float x = 1832.0f - 1600.0f * t;  /* 1832 → 232 [CONFIRMED @ 0x0040DF4A] */ TD5_LOG_I(LOG_TAG, "car_sel: slide-in x=%.0f t=%.2f", x, t); | td5_frontend.c:4281 |
| 0x0040f184 | _unchanged_ | _unchanged_ | low | centered via MeasureOrCenterFrontendString [CONFIRMED @ 0x0040F184]). Language.dll is unavailable in the port; fall through to return. */ | td5_frontend.c:8624 |
| 0x0040f744 | _unchanged_ | _unchanged_ | low | Drag-race 2-pass CarSelect [CONFIRMED @ 0x0040f744]. Original: game_type==7 (=drag race there) && pass_marker==0 → re-enter | td5_frontend.c:8754 |
| 0x0040fe00 | _unchanged_ | _unchanged_ | low | Controller type icon [CONFIRMED @ 0x40FE00]: per-type TGA centered at y=120 */ int controller_type = td5_input_get_device_type(0); | RE basis: ScreenControllerBindingPage decompiled [CONFIRMED @ 0x40FE00] | Data layout [CONFIRMED @ 0x40FE00]: s_ctrl_player        — which player is being configured (0=P1, 1=P2) | td5_frontend.c:4389 |
| 0x004100c0 | _unchanged_ | _unchanged_ | low | 0x004100C0  OpenControllerBindingPageWrapper                  [ARCH-DIVERGENCE: CtrlBind] | td5_frontend.c:10341 |
| 0x004100ce | _unchanged_ | _unchanged_ | low | 0x004100CE  DrawControlBindingTextWithOkButton                [ARCH-DIVERGENCE: CtrlBind] | td5_frontend.c:10342 |
| 0x004100d7 | _unchanged_ | _unchanged_ | low | 0x004100D7  OpenControllerBindingPageNoneHeader               [ARCH-DIVERGENCE: CtrlBind] | td5_frontend.c:10343 |
| 0x004100de | _unchanged_ | _unchanged_ | low | 0x004100DE  OpenControllerBindingPageRearViewHeader           [ARCH-DIVERGENCE: CtrlBind] | td5_frontend.c:10344 |
| 0x004100fa | _unchanged_ | _unchanged_ | low | 0x004100FA  DrawControlBindingText1WithOkButton               [ARCH-DIVERGENCE: CtrlBind] | td5_frontend.c:10345 |
| 0x00410111 | _unchanged_ | _unchanged_ | low | 0x00410111  DrawControlBindingText2WithOkButton               [ARCH-DIVERGENCE: CtrlBind] | td5_frontend.c:10346 |
| 0x00410129 | _unchanged_ | _unchanged_ | low | 0x00410129  OpenControllerBindingPageNoneHeader               [ARCH-DIVERGENCE: CtrlBind] | td5_frontend.c:10347 |
| 0x00410165 | _unchanged_ | _unchanged_ | low | Create OK button [CONFIRMED @ 0x00410165]: CreateFrontendDisplayModeButton(SNK_OkButTxt, -0x128, 0, 0x60, 0x20, 0) */ | td5_frontend.c:7915 |
| 0x00410380 | _unchanged_ | _unchanged_ | low | 0x00410380  RenderControllerBindingMenuPage                   [ARCH-DIVERGENCE: CtrlBind] | td5_frontend.c:10348 |
| 0x0041043c | _unchanged_ | _unchanged_ | low | 0x0041043C  RenderControllerBindingPageUpDownHeader           [ARCH-DIVERGENCE: CtrlBind] | td5_frontend.c:10349 |
| 0x004104b2 | _unchanged_ | _unchanged_ | low | 0x004104B2  RenderControllerBindingPageDownHeader             [ARCH-DIVERGENCE: CtrlBind] | td5_frontend.c:10350 |
| 0x00410527 | _unchanged_ | _unchanged_ | low | 0x00410527  RenderControllerBindingPageUpHeader               [ARCH-DIVERGENCE: CtrlBind] | td5_frontend.c:10351 |
| 0x00410599 | _unchanged_ | _unchanged_ | low | 0x00410599  RenderControllerBindingPageBlankOrRearViewHeader  [ARCH-DIVERGENCE: CtrlBind] | td5_frontend.c:10352 |
| 0x00410613 | _unchanged_ | _unchanged_ | low | 0x00410613  RenderControllerBindingPageRows                   [ARCH-DIVERGENCE: CtrlBind] | td5_frontend.c:10353 |
| 0x00410940 | _unchanged_ | _unchanged_ | low | 0x00410940  DrawControlOptionsBindingHeader                   [ARCH-DIVERGENCE: CtrlBind] | td5_frontend.c:10354 |
| 0x00411710 | _unchanged_ | _unchanged_ | low | 0x00411710  BuildFrontendDitherOffsetTable          [ARCH-DIVERGENCE: DDraw] 0x00411A50  ResetFrontendFadeState                  [ARCH-DIVERGENCE: DDr | td5_frontend.c:10257 |
| 0x00411a50 | _unchanged_ | _unchanged_ | low | 0x00411A50  ResetFrontendFadeState                  [ARCH-DIVERGENCE: DDraw] 0x00411A70  RenderFrontendFadeOutEffect             [ARCH-DIVERGENCE: DDr | td5_frontend.c:10258 |
| 0x00411a70 | _unchanged_ | _unchanged_ | low | 0x00411A70  RenderFrontendFadeOutEffect             [ARCH-DIVERGENCE: DDraw] 0x00411DE0  ClearFrontendSurfaceRegistry            [ARCH-DIVERGENCE: DDr | td5_frontend.c:10259 |
| 0x00411de0 | _unchanged_ | _unchanged_ | low | 0x00411DE0  ClearFrontendSurfaceRegistry            [ARCH-DIVERGENCE: DDraw] 0x00411E00  GetFrontendSurfaceRegistryId            [ARCH-DIVERGENCE: DDr | td5_frontend.c:10260 |
| 0x00411e00 | _unchanged_ | _unchanged_ | low | 0x00411E00  GetFrontendSurfaceRegistryId            [ARCH-DIVERGENCE: DDraw] 0x00411E90  ReleaseTrackedFrontendSurfaces          [ARCH-DIVERGENCE: DDr | td5_frontend.c:10261 |
| 0x00411e90 | _unchanged_ | _unchanged_ | low | 0x00411E90  ReleaseTrackedFrontendSurfaces          [ARCH-DIVERGENCE: DDraw] 0x004122F0  LoadTgaToFrontendSurfaceFromArchive     [ARCH-DIVERGENCE: DDr | td5_frontend.c:10262 |
| 0x00412030 | _unchanged_ | _unchanged_ | low | 0x00412030  LoadFrontendTgaSurfaceFromArchive  [ARCH-DIVERGENCE: TGA/DDraw -> PNG/D3D11; L5 sweep 2026-05-21] | td5_asset.c:3051 |
| 0x004122f0 | _unchanged_ | _unchanged_ | low | 0x004122F0  LoadTgaToFrontendSurfaceFromArchive     [ARCH-DIVERGENCE: DDraw] 0x004127B0  LoadTgaToFrontendSurface16bppVariant    [ARCH-DIVERGENCE: DDr | td5_frontend.c:10263 |
| 0x004127b0 | _unchanged_ | _unchanged_ | low | 0x004127B0  LoadTgaToFrontendSurface16bppVariant    [ARCH-DIVERGENCE: DDraw] 0x004129B0  RenderTgaToFrontendSurface              [ARCH-DIVERGENCE: DDr | td5_frontend.c:10264 |
| 0x004129b0 | _unchanged_ | _unchanged_ | low | 0x004129B0  RenderTgaToFrontendSurface              [ARCH-DIVERGENCE: DDraw] 0x00412B00  SetSurfaceColorKeyFromRGB               [ARCH-DIVERGENCE: DDr | td5_frontend.c:10265 |
| 0x00412b00 | _unchanged_ | _unchanged_ | low | 0x00412B00  SetSurfaceColorKeyFromRGB               [ARCH-DIVERGENCE: DDraw] 0x00417B74  AdvanceFrontendInlineStringTableState   [ARCH-DIVERGENCE: DDr | td5_frontend.c:10266 |
| 0x00412d50 | _unchanged_ | _unchanged_ | low | 0x00412D50  MeasureOrDrawFrontendFontString       [ARCH-DIVERGENCE: FontStr] 0x00424470  DrawFrontendFontStringToSurface       [ARCH-DIVERGENCE: FontS | td5_frontend.c:10398 |
| 0x00413010 | _unchanged_ | _unchanged_ | low | 0x00413010  DrawPostRaceHighScoreEntry  [ARCH-DIVERGENCE: ScreenFSM] Folded into frontend_render_high_score_overlay (td5_frontend.c:4512). | td5_frontend.c:10475 |
| 0x00413580 | _unchanged_ | _unchanged_ | low | 0x00413580  ScreenPostRaceHighScoreTable  [ARCH-DIVERGENCE: ScreenFSM] Screen_PostRaceHighScore (td5_frontend.c:9029) — 9 states (0..8). | td5_frontend.c:10486 |
| 0x00413b60 | _unchanged_ | _unchanged_ | low | td5_save_write_config(NULL) [CONFIRMED @ 0x00413b60] in case file wasn't flushed. | td5_frontend.c:10493 |
| 0x00413bc0 | _unchanged_ | _unchanged_ | low | [CONFIRMED @ 0x00413BC0] ScreenPostRaceNameEntry reads these fields */ int32_t td5_game_get_result_primary(int slot);   /* finish time ticks */ | td5_game.h:105 |

## Key discoveries

- (Mechanical comment-sync batch; no new findings.)

## Out-of-scope finds

- (None — this batch only consolidates existing port audit headers.)

## TODO impact

- No TODO closure expected. This batch makes future audits faster by surfacing port-side analysis inside Ghidra.

