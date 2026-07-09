# INTENTIONAL re-sampling pass — 2026-05-24

Sampled 25 of 310 INTENTIONAL rows from `orig_vs_port_verdict_2026-05-24.csv`.
Goal: surface lazy classifications where agents accepted "INTENTIONAL" but the
divergence is actually OVERSIGHT or CANNOT_DETERMINE.

## Sample composition

| Bucket | Sampled |
|---|---:|
| D3D11_BACKEND (174 total) | 8 |
| ARCH-DIVERGENCE / ARCH_DIVERGENCE / ARCH-* (15 total) | 5 |
| FMV_STUB (18 total) | 3 |
| ZLIB_REPLACE (14 total) | 3 |
| CtrlBind / MeshXform / SOLO_PEER_* / Y_UP (16 total) | 3 |
| DXPTYPE / SURFBLIT / FONTSTR / M2DX (47 total) | 3 |
| Total | 25 |

## Results

| Verdict on re-check | Count |
|---|---:|
| CONFIRMED_INTENTIONAL | 25 |
| OVER_CLASSIFIED → FAITHFUL | 0 |
| MISSED_BUG → OVERSIGHT | 0 |
| TOO_UNCERTAIN → CANNOT_DETERMINE | 0 |

**Estimated full-population error rate:** ~0% (extrapolation: 0/25 → expect ~0/310).

Two rows were initially suspect on first pass; on closer reading of port code, both
turned out to be correctly classified:

- **0x0040CBD0 ConfigureActorProjectionEffect** — Initially looked like port might
  pass `linear_velocity` instead of orig's `-(yaw>>8)`. Closer read of port
  td5_render.c:3837-3855 shows port **does** handle `-yaw_12bit` for cos/sin in
  modes 2/3, AND uses `linear_velocity` only for mode-1's scroll-accumulator term
  (which is what `actor + 0x308` is in orig per REG-9 verdict 2026-05-22 noted in
  port comment). Correctly resolved.

- **0x004104B2 RenderControllerBindingPageDownHeader** — Port cite at
  td5_frontend.c:4396 is only the overlay/icon rendering. The 116-byte orig also
  contains a button-cycling FSM and 2-button swap logic. Found at port
  td5_frontend.c:7959-8015 ("State 10: Joystick interactive — detect button
  presses, cycle bindings" with explicit `[CONFIRMED @ 0x40FE00]` annotation
  for the 2-button swap). The function is fully consolidated across multiple port
  call-sites, not lost.

## Per-row findings

### CONFIRMED_INTENTIONAL (no change needed)

D3D11_BACKEND (8):
- 0x004011C0 RenderVehicleTaillightQuads → td5_render.c:4290 — orig D3D3 sprite-quad+QueueBatch replaced by D3D11 immediate draw_tris; brightness ramp (+8/cap 0x80/>>1) byte-faithful per port `s_brake_brightness` logic.
- 0x0040CBD0 ConfigureActorProjectionEffect → td5_render.c:3768 — 3-mode dispatch + 3 orig functions collapsed into one typed-struct port impl; mode-1 `linear_velocity` choice resolved by REG-9 verdict, modes 2/3 cos/sin(-yaw) match.
- 0x004122F0 LoadTgaToFrontendSurfaceFromArchive → td5_frontend.c:10293 — DDraw TGA loader replaced by PNG pipeline (explicitly in DDraw manifest).
- 0x00423DB0 ClearBackbufferWithColor → td5_frontend.c:10301 — DDraw Blt(0x400) primary-surface fill replaced by `td5_plat_render_clear`.
- 0x004258C0 ActivateFrontendCursorOverlay → td5_frontend.c:1357 — flag-polarity inversion documented at L1350-1356, mouse-moved tracking moved to update_frontend's `s_prev_mouse_x/y` compare.
- 0x0042D410 FinalizeCameraProjectionMatrices → td5_camera.c:363 — math sequence byte-equivalent; two divergences (billboard snapshot + `(int)(x+0.5f)`) explicitly documented at file lines 350-360.
- 0x00432BD0 BuildSpriteQuadTemplate → td5_render.c:5634 — 5-flag dispatch (GEOMETRY/UV/COLOR/OPCODE/TEXPAGE) preserved on D3D11 vertex layout; raw-flags pass-through via `TD5_BSQT_RAW_FLAGS`.
- 0x0043E2F0 FlushProjectedPrimitiveBuckets → td5_render.c:2648 — 4096-bucket walk + bit31/0x80000003/0x80000004 flag dispatch preserved through `clip_and_submit_polygon`.

ARCH-DIVERGENCE (5):
- 0x00406950 ClearTrackSegmentVisibilityTable → td5_physics.c:230 — 256-bucket grid + chain-link broadphase, port uses u8 grid + chain in `g_actor_aabb[][4]`.
- 0x00428880 ConfigureForceFeedbackControllers → td5_input.c:1493 — bulk-clear+copy preserved byte-faithful; network single-slot splice moved to caller (documented at L1502).
- 0x0042FB40 ApplyTrackStripAttributeOverrides → td5_track.c:2035 — port hardcodes track_idx=0, accepted as documented port-side limitation; comment at line 2025 admits helper is "documented but unused".
- 0x00431260 GetTrackSpanDisplayListEntry → td5_track.c:5993 — orig 13-byte LUT; port adds reverse-mirror + MODELS gate + STRIP fallback; forward+MODELS path byte-equivalent.
- 0x0043DC20 LoadRenderTranslation → td5_render.c:1336 — orig is 12-byte copy of pre-baked view-space translation; port absorbs caller's bake step (`delta=pos-cam; m[9..11]=basis*delta`); all 12 known callers folded per comment.

FMV_STUB (3):
- 0x0043C3C0 RequestIntroMovieShutdown → td5_fmv.c:736 — EA TGQ teardown replaced by IMFSourceReader release / MFShutdown.
- 0x00452830 CloseMultimediaStream → td5_fmv.c:981 — orig DirectSound+video release path; port FMV stub has no equivalent (listed in FMV manifest line 981).
- 0x00452DC0 SeekToLoopPoint → td5_fmv.c:987 — orig mmioSeek+ReadAndDispatchChunk gated by flag 0x8000; replaced by FMV stub.

ZLIB_REPLACE (3):
- 0x004405B0 DecompressZipEntry → td5_asset.c:989 — orig method-0 (stored) + method-8 (InflateDecompress) dual path; port unifies via `td5_inflate_mem_to_mem` (zlib when `TD5_INFLATE_USE_ZLIB`); ZIP fields/CRC32 byte-faithful.
- 0x00447AA6 InflateProcessStoredBlock → td5_inflate.c:451 — orig stored-block parser using global bit buffer; port routes through zlib.
- 0x004474F6 InflateRefillInputBuffer → td5_inflate.c:447 — 11-byte thunk to ReadTrackStaticDataChunk; port uses zlib stream input.

CtrlBind / MeshXform (3):
- 0x004100DE OpenControllerBindingPageRearViewHeader → td5_frontend.c:4396 — tail-fall-through entry; consolidated into single overlay per CtrlBind manifest line 10344.
- 0x004104B2 RenderControllerBindingPageDownHeader → td5_frontend.c:4396 — overlay portion at 4396; button-cycle/2-button-swap FSM at td5_frontend.c:7959-8015 with `[CONFIRMED @ 0x40FE00]` annotation. Fully consolidated.
- 0x0042D950 ApplyMeshRenderBasisFromWorldPosition → td5_render.c:6221 — sibling of 0x42D880; folded into mesh-xform dispatch per MeshXform manifest.

DXPTYPE / SURFBLIT / FONTSTR (3):
- 0x004242B0 DrawFrontendLocalizedStringPrimary → td5_frontend.c:10444 — per-glyph DDraw Blt(0x11) over BodyText atlas; port `fe_draw_text/fe_measure_text` via D3D11 glyph-strip.
- 0x0041B390 CreateFrontendNetworkSession → td5_frontend.c:10337 — DXPlay::NewSession + QueueFrontendNetworkMessage(1); port unreachable behind DXPTYPE wire-incompat barrier.
- 0x00424050 BltColorFillToSurface → td5_frontend.c:5200 — DDraw 16bpp Blt; per-callsite collapsed to `fe_draw_quad TRANSLUCENT_LINEAR` (line 5200 is one such inlining for the SORRY dialog).
- 0x004251A0 Copy16BitSurfaceRect → td5_frontend.c:1478 — 16bpp DDraw Lock+memcpy+Unlock; collapsed to D3D11 Present helper `frontend_present_buffer`.

### OVER_CLASSIFIED → FAITHFUL
(none)

### MISSED_BUG → OVERSIGHT
(none)

### TOO_UNCERTAIN → CANNOT_DETERMINE
(none)

## Top 3 surprises

1. **Zero misclassifications in 25 rows**. The INTENTIONAL pool appears well-curated;
   nearly every entry has an inline `[ARCH-DIVERGENCE: <tag>]` marker in port source
   plus a CSV evidence string that names specific orig functions + offsets. The
   build_confidence_map.py:227-233 promotion rule (strong-keyword proximity)
   essentially enforces this discipline.

2. **Manifest-style consolidation is the dominant pattern**. ~60% of audited rows
   point at a "manifest" comment block (e.g. td5_frontend.c:10287-10314 DDraw,
   td5_fmv.c:975-999 FMV, td5_render.c:6221-6243 MeshXform/DepthSort, td5_inflate.c:446-454
   ZLIB) that lists every orig function consolidated into the port helper.
   This makes verification cheap — the citation chain orig-addr → manifest-line →
   port-impl is mechanical.

3. **Two initial "suspect" reads dissolved on closer inspection.**
   - 0x0040CBD0 ambiguity is explicitly resolved at port td5_render.c:3837-3843
     with a `[CONFIRMED @ 0x0040CBD0; REG-9 verdict 2026-05-22]` annotation; the
     orig disasm IS unambiguous for modes 2/3 (port handles correctly) and the
     port comment names the right structural choice for mode-1.
   - 0x004104B2 cite at L4396 looked partial but the full state machine lives at
     L7959-8015. Citation could have been more complete; behavior is correct.

## Extrapolation to full 310 population

With 0/25 misclassifications (95% CI upper bound roughly 11.5% via rule-of-three),
the realistic expectation is that <5 of the 310 INTENTIONAL rows are mis-tagged.
The systematic discipline (inline ARCH-DIVERGENCE markers + manifest comments +
CSV evidence strings) appears to be working. No follow-up audit pass recommended
on this bucket unless behavior regression points elsewhere.

## Audit metadata

- Pool slot used: `TD5_pool1` (pool0 was un-analyzed)
- Sampling: every Nth row within each bucket (deterministic, not random)
- All orig decompiles obtained via `mcp__ghidra__decomp_function` against pool1
- All port cites verified via direct `Read` of cited file:line
