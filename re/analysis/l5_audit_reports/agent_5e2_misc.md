# Phase 5(e) Part 2 — HUD / Asset / Input / Sound L5 Audit

**Date:** 2026-05-21
**Scope:** 15 functions across td5_hud.c (6), td5_asset.c (6), td5_input.c (1), td5_sound.c (2)
**Method:** Ghidra pool TD5_pool12 read-only decomp vs port; precision-keyword
comments added within 30 lines of address citation.

## Totals

- Total in scope: 15
- Promoted L5 (CONFIRMED): 4
- Promoted L5 (ARCH-DIVERGENCE): 11
- Skipped (left at L4): 0
- Suspected regressions: 1 (RenderHudRadialPulseOverlay — visible HUD effect missing)

## Promoted L5 (CONFIRMED) — 4

### td5_input.c
- **0x00402E40** `ResetPlayerVehicleControlBuffers` — orig 6×4-byte memset
  loop on &gPlayerControlBuffer matches port TD5_MAX_RACER_SLOTS loop.

### td5_asset.c
- **0x0042FD70** `RemapCheckpointOrderForTrackDirection` — orig walks the
  16-byte g_checkpointNumTable_PROVISIONAL @ 0x4aedb0 reading
  gTrackDirectionSwitchFlag @ 0x4aee00 (= table+0x50 = port `cols[20]`).
  4-col variant (== -1) pairs [c0,c4]+[c1,c3]; 5-col variant pairs
  [c0,c5]+[c1,c4]+[c2,c3]. Port `td5_asset_apply_reverse_texture_swap`
  matches exactly.

### td5_hud.c
- **0x004377B0** `InitializeRaceOverlayResources` — 0x1148 heap alloc, +0x894
  split, bit dispatch from g_raceOverlayPresetMode/specialEncounter/circuit
  all match port `td5_hud_init_overlay_resources`. FADEWHT 5-quad + COLOURS
  divider builds match.
- **0x00437BA0** `InitializeRaceHudLayout` — scale (W/640, H/480) +
  3-mode viewport dispatch + per-view derivation + 7-atlas lookup order
  + SPEEDO/SPEEDOFONT/GEARNUMBERS/UTURN/REPLAY quad offsets all match
  port `td5_hud_init_layout` and master dispatcher's flag-bit-to-offset
  table.

## Promoted L5 (ARCH-DIVERGENCE) — 11

### td5_asset.c
- **0x0040B530** `SwapIndexedRuntimeEntries` — orig swaps two parallel
  tables (4-byte index table + 8-byte descriptor table under DAT_0048DC40);
  port has no separate cache header, swaps only s_page_remap[]. Documented
  at file-header lines 67-83.
- **0x0040B590** `UploadRaceTexturePage` — D3D3 DXD3DTexture::LoadRGBS24/32
  dispatch with format_mode==4 alpha-keying pass; port uses
  td5_plat_render_upload_texture with the slot-4 alpha-rebake preserved
  inline + transparency table preserved via td5_asset_set_page_transparency.
  d3d_exref+0xa5c driver-caps branch folded.
- **0x0042F990** `LoadEnvironmentTexturePages` — DDraw ReadArchiveEntry +
  ResampleTexturePageToEntryDimensions + UploadRaceTexturePage pipeline →
  PNG decode from re/assets/environs + direct D3D11 upload.
- **0x00430D30** `ParseAndDecodeCompressedTrackData` — orig is a software
  RGBA→packed-pixel quantizer for D3D3 surface formats (RGB565/ARGB1555/
  RGBA8888) using channel-mask bit extraction; port replaces with box-filter
  mipmap builder. The per-level outer loop shape is shared but the inner
  pixel conversion is gone — D3D11 backend uses fixed BGRA8 swap chain.
  Port comment "Mipmap Builder" was misleading (orig isn't a mipmap builder).
- **0x00442670** `LoadTrackTextureSet` — orig branches on
  g_vehicleProjectionEffectMode for dual-texture-set bookkeeping and walks
  gStaticHedEntryCount × 0x10-stride entries adding 0x400 to rebase data
  pointers, then calls BuildTrackTextureCache. Port retains the TEXTURES.DAT
  page-count + reverse-swap (steps conceptually 1-2) but skips static.hed
  pointer rebase (no cache header in port) and direct GPU upload happens
  later in load_race_texture_pages.

### td5_hud.c
- **0x00439B70** `DrawRaceStatusText` — replay-mode/wanted-message/time-trial
  text emission matches semantically (port lines 1735-1816) but uses
  td5_hud_queue_text instead of QueueRaceHudFormattedText (D3D3 deferred
  queue). Per-view layout indexed by named struct fields vs orig's
  14-float-per-view DAT_004B1158 stride.
- **0x00439E60** `RenderHudRadialPulseOverlay` — orig builds 5-segment
  translucent pulse ring via CosFloat12bit/SinFloat12bit + 5×
  BuildSpriteQuadTemplate + 5× SubmitImmediateTranslucentPrimitive; port
  `td5_render_radial_pulse` is a stub. **Visible HUD effect missing** —
  see Suspected Regressions below.
- **0x0043B7C0** `InitializePauseMenuOverlayLayout` — BLACKBOX/SELBOX/
  BLACKBAR/SLIDER/PAUSETXT geometry verified byte-faithful, but orig emits
  in centered coords consumed by BuildSpriteQuadTemplate; port emits in
  pixel-space with explicit cx/cy offset added inline.
- **0x0043E750** `SetClipBounds` — orig is the projection-clip-rect setter
  (4 floats, pairwise mins, stored to DAT_004afb38..0x44); port's
  td5_render_set_clip_rect (td5_render.c:5570) is the actual equivalent
  (D3D11 scissor push). The port's td5_hud_draw_race_fade attached to this
  comment header is **unrelated** — it's a directional FADEWHT bar render
  that orig builds inside RenderRaceHudOverlays bit 0x80000000. Comment
  header at td5_hud.c:3344 had a misleading address citation.

### td5_sound.c
- **0x00440A30** `UpdateVehicleLoopingAudioState` — wanted-mode/cop branch
  byte-faithful (4 tracked-vehicle-audio writes + AdvanceGlobalSkyRotation
  + siren flags). Non-cop fall-through diverges: orig polls
  DXSound::Status(slotIndex*3+3) and writes skid-loop state on idle; port
  polls slot_is_playing on local engine slots and resets engine_state on
  idle. Different state-machine semantics — DXSound queue model vs port's
  consolidated s_skid_playing/s_engine_state arrays.
- **0x00441A80** `LoadVehicleSoundBank` — already had inline ARCH-DIVERGENCE
  for the 4 "99" markers; verified Ghidra-equivalent for the per-slot state
  zeroing pass (engine_loop/skid_loop/traffic_engine/reverb_mode at orig
  vs s_engine_state/s_horn_state/s_traffic_engine_state/s_reverb_flag at
  port). WAV-load sequence (Drive/Rev|Reverb/Horn) byte-faithful.

## Suspected Regressions

1. **RenderHudRadialPulseOverlay missing** — td5_render.c:5752 is a stub
   `void td5_render_radial_pulse(float dt) { (void)dt; }`. Port's HUD-side
   state (`s_radial_pulse_progress` at td5_hud.c:1227) is updated but never
   consumed. Orig draws a 5-segment ring at race-mode transitions (likely
   countdown / boost / wanted-mode visual cue). Quick fix path: implement
   the 5-segment ring using td5_cos_12bit/td5_sin_12bit (already in port)
   + hud_build_quad to FADEWHT atlas + hud_submit_quad — geometry pattern
   matches Ghidra orig 0x00439E60 exactly. Out of audit scope.

## Notes

- Ghidra pool: TD5_pool12 (read-only). Slot 0 stale on entry; released cleanly.
- No build run per phase instructions.
- Diff is pure comment additions (no executable code changes).
- td5_sound.c:LoadVehicleSoundBank already qualified L5 via inline
  ARCH-DIVERGENCE; re-validated against fresh Ghidra decomp during this pass.
