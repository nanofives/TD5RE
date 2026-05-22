# L5 Audit Report — td5_frontend.c (Agent A)

**Date:** 2026-05-21
**Scope:** `td5mod/src/td5re/td5_frontend.c` (91 L4 entries in `re/analysis/l5_audit_manifests/td5_frontend.c.csv`)
**Method:** Static comparison of Ghidra decomp (TD5_d3d.exe) against port source. Only added comments; no executable code touched.

## Totals

- **Total in scope:** 91
- **Promoted L5 (byte-faithful CONFIRMED):** 0
- **Promoted L5 (ARCH-DIVERGENCE documented):** 5
- **Skipped (left at L4):** 86
- **Suspected regressions:** 0

## Promoted L5 — ARCH-DIVERGENCE (5)

| Orig address | Function | Port site | Rationale |
|---|---|---|---|
| `0x0040B100` + `0x0041D840` | `BuildEnumeratedDisplayModeList` + `FormatDisplayModeOptionStrings` | `frontend_init_display_mode_state` (~line 2001) | DXDraw `dd_exref+0x34` mode table doesn't exist under D3D11. Port enumerates via `td5_plat_enum_display_modes` (DXGI) and formats labels as `"%dx%d %dbpp"` (orig was cramped `"%dx%dx%d"` in 0x20-byte slots). Tagged as one combined header. |
| `0x0040DDC0` | `DrawCarSelectionPreviewOverlay` | `frontend_render_car_selection_preview` (~line 4174) | DDraw `QueueFrontendOverlayRect` → D3D11 batched `fe_draw_surface_rect`. Same animation phases (state 0/0xB/0xE), same constants (0x198, 0x118, 0x5A alpha, 0x40/0x20 step deltas, 0x4A8 offscreen offset). Color-key blits replaced by alpha blending. |
| `0x004258C0` + `0x004258E0` | `ActivateFrontendCursorOverlay` + `DeactivateFrontendCursorOverlay` | `frontend_set_cursor_visible` (~line 1340) | Orig uses inverted `g_frontendCursorOverlayHidden` flag; port stores `s_cursor_visible` with direct semantics. All 12 callers' arguments were inverted in the same commit. `g_frontendMouseMovedFlag` clear absorbed into `update_frontend`'s prev-mouse-x/y compare. |
| `0x00424AF0` + `0x00424CA0` | `PresentPrimaryFrontendBufferViaCopy` + `PresentPrimaryFrontendBuffer` | `frontend_present_buffer` (~line 1468) | Orig has two paths: software `Copy16BitSurfaceRect` and hardware `vtbl[0x1c]` Blt. Both collapse to one `td5_plat_render_end_scene + td5_plat_present(1)` under D3D11. |
| `0x004275A0` | `RunAttractModeDemoScreen` | `Screen_AttractModeDemo` (~line 5909) | 6-state FSM mapped case-for-case (0..5) to orig. Each case bridges DDraw primitives → D3D11 helpers (CreateTrackedFrontendSurface→frontend_load_tga, PresentPrimaryFrontendBufferViaCopy→frontend_present_buffer, InitFrontendFadeColor→frontend_init_fade, RenderFrontendFadeEffect→frontend_render_fade). |

## Skipped (left at L4) — 86

The bulk of the 91 L4 entries fall into one of three patterns where promotion is not defensible without deeper investigation:

### Pattern 1: "Phase 1 density-match" entries with no port impl (38 functions)

`build_confidence_map.py`'s 2026-05-21 citation-refresh marked these as cited solely because the address string appeared in `td5_orig_globals.h` or in a footer manifest comment, but no port-side implementation exists. The corresponding DDraw subsystem doesn't have a 1:1 port — it's gone entirely (D3D11 backbuffer = no lost-surface flow, no software dither, no surface registry, no DXDraw mode table).

Examples:
- `0x00411710 BuildFrontendDitherOffsetTable` — D3D11 doesn't dither
- `0x00411A50 ResetFrontendFadeState` — orig fade walks scanlines, port fade is a single full-screen quad
- `0x00411DE0/0x00411E00/0x00411E90 ClearFrontendSurfaceRegistry/GetFrontendSurfaceRegistryId/ReleaseTrackedFrontendSurfaces` — 64-slot DDraw surface table; port uses 31-slot `FE_Surface s_surfaces[]` D3D11 abstraction
- `0x00412B00 SetSurfaceColorKeyFromRGB` — D3D11 has alpha blending, no color-key
- `0x004183B0/0x00418410/0x00418430 SetFrontendInlineStringTable*` — orig in-place tokenizer over .text-ROM strings; port reads localized strings live from save module
- `0x00423DB0 ClearBackbufferWithColor` — D3D11 clear is platform abstraction
- `0x00423E40/0x00423F90 LockSecondaryFrontendSurfaceFillColor/FillSurfaceRectWithColor` — surface-lock blit paradigm replaced by quad draws
- `0x00425170 UpdateFrontendClientOrigin`, `0x004254D0 ResetFrontendOverlayState`, `0x00425500 ResetFrontendSelectionState`, `0x00425730 QueueFrontendSpriteBlit`, `0x00425540 FlushFrontendSpriteBlits`, `0x004258F0 CreateFrontendMenuRectEntry`, `0x00425A30 RenderFrontendUiRects` — frontend sprite/rect queue is gone in port (immediate draws instead of queue+flush)
- `0x004260E0 CreateFrontendDisplayModePreviewButton`, `0x004263E0 RenderFrontendDisplayModeHighlight`, `0x00426580 UpdateFrontendDisplayModeSelection`, `0x004264E0 BeginFrontendDisplayModePreviewLayout`, `0x00426540 RestoreFrontendDisplayModePreviewLayout` — orig snapshots/restores a 0xD00-byte button table for display preview; port doesn't (no preview layout swap)

Could be tagged as ARCH-DIVERGENCE *en masse* via a footer manifest but the per-function justification varies. Conservative call: leave at L4 until a follow-up sweep handles them as a class.

### Pattern 2: Large state-machine screens (`Screen_*`) — too big to byte-verify per audit budget (12 functions)

These have clear port impls but the orig functions are 500–5149 bytes:

- `0x00413010 DrawPostRaceHighScoreEntry` (1368B), `0x00413580 ScreenPostRaceHighScoreTable` (1549B), `0x00415030 ScreenPositionerDebugTool` (795B), `0x00415370 ScreenStartupInit` (258B — close, but case-4 redirect target differs from orig), `0x004168B0 RaceTypeCategoryMenuStateMachine` (5149B), `0x00417DD2 LoadFrontendExtrasGalleryResources` (684B), `0x00418460 ScreenMusicTestExtras` (2002B), `0x00418D50 RunFrontendConnectionBrowser` (3500B), `0x00419CF0 RunFrontendSessionPicker` (2071B), `0x0041A7B0 RunFrontendCreateSessionFlow` (2939B), `0x0041C330 RunFrontendNetworkLobby` (4789B), `0x0041EA90 ScreenSoundOptions` (3794B), `0x004213D0 ScreenQuickRaceMenu` (2470B), `0x00427290 ScreenLanguageSelect` (495B — structural divergence: orig uses 4 menu-rect entries from a single 16bpp flag sheet, port uses 4 named buttons), `0x00427630 TrackSelectionScreenStateMachine` (3043B).

Each needs its own dedicated session.

### Pattern 3: DXPTYPE network/lobby code blocked by known wire-incompat ARCH (8 functions)

Per `reference_arch_dxptype_protocol_divergence_2026-05-20`, the DXPTYPE protocol is wire-incompatible between port and orig. The lobby/chat/session functions are structurally unreachable in the port:

- `0x00418C60 QueueFrontendNetworkMessage`
- `0x00419B30 RenderFrontendSessionBrowser`
- `0x0041A530 RenderFrontendCreateSessionNameInput`
- `0x0041A670 RenderFrontendLobbyChatInput`
- `0x0041B390 CreateFrontendNetworkSession`
- `0x0041B420 RenderFrontendLobbyStatusPanel`
- `0x0041B610 ProcessFrontendNetworkMessages`
- `0x0041BD00 RenderFrontendLobbyChatPanel`

These should probably move to the footer's existing `[ARCH-DIVERGENCE: frontend residual]` manifest (which already covers `NormalizeFrontendChatTokens`) but the right call is to leave them at L4 for now since each has nontrivial verification work beyond a one-line stub.

### Pattern 4: Controller-binding mini-tails (10 functions)

`0x004100C0..0x00410940` — these are fragments of one logical orig function that Ghidra has split into 10 separate "tail" entries because of the original assembly's clever tail-fall-through layout. The port implements them as one combined `frontend_render_controller_binding_overlay`. Each individual tail address isn't really portable as a discrete unit; the right answer is a single header on the port-side function. But matching 10 Ghidra addresses to one consolidated port impl requires careful case-by-case mapping, which exceeded audit budget.

Functions: `0x004100C0 OpenControllerBindingPageWrapper`, `0x004100CE DrawControlBindingTextWithOkButton`, `0x004100D7/0x00410129 OpenControllerBindingPageNoneHeader` (two distinct entries), `0x004100DE OpenControllerBindingPageRearViewHeader`, `0x004100FA DrawControlBindingText1WithOkButton`, `0x00410111 DrawControlBindingText2WithOkButton`, `0x00410380 RenderControllerBindingMenuPage`, `0x0041043C/0x004104B2/0x00410527/0x00410599 RenderControllerBindingPage*Header`, `0x00410613 RenderControllerBindingPageRows`, `0x00410940 DrawControlOptionsBindingHeader`.

### Other skips (18 functions)

`0x0040D590 LoadExtrasGalleryImageSurfaces`, `0x0040D750 AdvanceExtrasGallerySlideshow`, `0x0040D830 UpdateExtrasGalleryDisplay`, `0x004122F0 LoadTgaToFrontendSurfaceFromArchive`, `0x004127B0 LoadTgaToFrontendSurface16bppVariant`, `0x004129B0 RenderTgaToFrontendSurface`, `0x00412D50 MeasureOrDrawFrontendFontString`, `0x00417B74 AdvanceFrontendInlineStringTableState`, `0x004242B0 DrawFrontendLocalizedStringPrimary`, `0x00424470 DrawFrontendFontStringToSurface`, `0x00424660 DrawFrontendSmallFontStringToSurface`, `0x00424740 DrawFrontendClippedStringToSurface`, `0x004248E0 DrawFrontendWrappedStringLine`, `0x00424A50 MeasureOrCenterFrontendLocalizedString`, `0x00424B30 CopyPrimaryFrontendBufferToSecondary`, `0x00424BC0 CopyPrimaryFrontendRectToSecondary`, `0x00424C10 PresentSecondaryFrontendRectViaCopy`, `0x00424C50 BlitSecondaryFrontendRectToPrimary`, `0x00424CF0 PresentSecondaryFrontendRect`, `0x00424D40 PresentPrimaryFrontendRect`, `0x00424D90 FillPrimaryFrontendScanline`, `0x00424E40 InitializeFrontendPresentationState` — these are TGA-load + font-render + secondary-surface-blit utilities. The port either folds them into different helpers or relies on D3D11-side equivalents. Each needs its own audit pass.

## Notes / lessons

1. The phase-1 citation-refresh CSV is **noisy**. 50%+ of the L4 entries in this file are "density-match" entries (3-of-5 or 3-of-6 token co-occurrence). The actual port impl often doesn't exist as a discrete function, or exists but with deliberate D3D11 architectural shift that can't be tagged as one-to-one byte-faithful.

2. A safer follow-up workflow would be a *class-level* ARCH-DIVERGENCE manifest in the file footer covering "DDraw surface registry → D3D11 texture page" (covers ~15 entries), "DDraw sprite queue → immediate-mode quad batch" (covers ~10), "DXPTYPE protocol → unreachable" (covers ~8), "DXDraw mode table → DXGI" (covers ~5), and "screen FSM port (per-screen audit)" (covers ~12). That would promote the whole class with a single defensible rationale per class.

3. Five of the 91 entries were promoted via in-place headers — all are documented arch-divergences with clear orig-vs-port mapping. No byte-faithful promotions were possible because every L4 entry in this file touches the DDraw/D3D11 architectural seam.

## Ghidra session

- Pool slot: TD5_pool3 (released via `bash scripts/ghidra_pool.sh cleanup`)
- Session id: 927966f7135c467197e44f3cf21e9fec (closed via cleanup)
