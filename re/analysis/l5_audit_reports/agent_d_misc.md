# Agent D — L4→L5 misc-modules audit (2026-05-21)

## Scope

48 L4 functions across 10 smaller TD5RE modules. Manifests at
`re/analysis/l5_audit_manifests/<file>.csv`. Auditing principle: byte-faithful
→ `[CONFIRMED @ ...]`, audited divergence → `[ARCH-DIVERGENCE: ...]`, uncertain
→ leave L4. Physics, track, and save fidelity-critical → extra conservative.

Ghidra slot used: `TD5_pool3` (read-only). Diff: pure comment-addition.

## Outcome summary

| Category | Count |
|---|---|
| Promoted L5 (CONFIRMED byte-faithful) | 2 |
| Promoted L5 (ARCH-DIVERGENCE documented) | 23 |
| Left at L4 (uncertain / fidelity-critical) | 23 |
| Suspected regressions surfaced | 1 |

## Promoted L5 — byte-faithful (CONFIRMED)

### td5_save.c
- `0x00411630 ValidateCupDataChecksum` — Open file → fread up to 0x4000 → XOR-decrypt
  with TD5_CUPDATA_XOR_KEY → CRC32 over full buffer → compare to expected.
  Identical algorithm, same buffer cap, same key. Orig reads expected_crc from a
  stack slot (cdecl caller-push), port reads it as an explicit parameter — same
  effect.

### td5_track.c
- `0x0040B1C0 BuildTrackTextureCache` — 11-byte thunk that drops param_1/param_2
  and tail-calls BuildTrackTextureCacheImpl(param_3). Port callers invoke the
  impl directly; same drop, same effect, no wrapper needed.

## Promoted L5 — ARCH-DIVERGENCE (documented)

### td5_game.c
- `0x00428D20 InitializeBenchmarkFrameRateCapture` — orig allocates 1 MB sample buffer;
  port samples timing directly via td5_plat_query_perf_counter. Sibling collapse to
  existing ARCH-DIVERGENCE block for 0x00428D40/0x00428D80.

### td5_frontend_button_cache.c
- `0x00425B60 DrawFrontendButtonBackground` — orig calls DDraw BltFast vtable
  for every 9-slice sub-rect; port composites into a malloc'd BGRA32 buffer
  via blit_rect with a state==0 transparency-fill prepass (orig's SYSMEM was
  pre-cleared by DDraw, port's malloc isn't). Same constants
  (tl=13/tr=9/bl=9/br=13/BB_TILE=4), same iteration order.

### td5_ai.c
- `0x0043D4E0 UpdateWantedDamageIndicator` — HUD overlay routed through td5_hud
  generic path; mislabeled in manifest as AI because it reads ai-owned table.
- `0x0043FB90 ReadCompressedTrackStreamChunk` — orig inflate-callback fwrite;
  port uses td5_inflate_mem_to_mem and removes streaming.

### td5_save.c
- `0x00411120 SerializeRaceStatusSnapshot` — byte-faithful within first 0x32A6 B
  (header/schedule/results/actor/slot/tail-misc offsets all preserved) plus a
  port-only TD5RECUP overlay at +0x32A6 carrying 6 car_index entries for
  pointer-resolution on race re-init. Extended size 12998 vs orig 12966.
- `0x004112C0 RestoreRaceStatusSnapshot` — symmetric reader. Byte-faithful
  unpack of all orig offsets + optional overlay-magic recognition path that
  also defensively scrubs racer actor pointer slots (+0x1B0/+0x1B8/+0x1BC) so
  stale pointers cannot be dereferenced before LoadRaceVehicleAssets re-fills.

### td5_sound.c
- `0x00414640 LoadFrontendSoundEffects` — same 10-entry path/slot table
  byte-faithfully (incl. repeated Whoosh/Crash1 entries, slots 1..10), DXSound::
  LoadBuffer → td5_plat_audio_load_wav, OpenArchiveFileForRead → td5_asset
  open-and-read with loose-file fallback.
- `0x00441D50 ReleaseRaceSoundChannels` — same 0x2c..0x57 stop range, same
  0..0x2b remove range. DXSound::Stop/Remove → slot_stop + td5_plat_audio_free.

### td5_input.c
- `0x00428880 ConfigureForceFeedbackControllers` — bulk-clear + bulk-copy path
  byte-faithful. Network-mode single-slot splice moved to caller per explicit
  port comment.
- `0x00428A10 PlayAssignedControllerEffect` — DXInput::PlayEffect with 4 params →
  td5_plat_ff_constant with 2 (effect_slot, magnitude). repeat_count discarded
  (no-op for port's effect cache). FF-capable flag check folded into platform
  layer. Port adds `js_idx > 1` upper-bound clamp (no-op in practice; port
  targets at most 2 FF wheels).
- `0x00428A60 PlayVehicleCollisionForceFeedback` — switch logic, clamps
  (TD5_INPUT_FF_COLLISION_DIV/MAX = 10/10000), and slot assignment
  (TD5_FF_SLOT_FRONTAL/SIDE = 1/2) byte-faithful. Takes explicit
  (actor_a_slot, actor_b_slot) instead of reading actor+0x375. Port adds
  s_ff.collision_active[slot] = 1 (port-only damping state, additive).

### td5_track.c
- `0x00406950 ClearTrackSegmentVisibilityTable` — broadphase grid array-width
  compaction (orig uint32[256] → port uint8[256] with 0xFF sentinel) +
  per-actor chain link folded into g_actor_aabb[i][4] in td5_physics.c:4041
  ResolveVehicleContacts. Same logical reset, no orig-side standalone
  equivalent in td5_track.c.
- `0x00434740 ComputeRouteForwardOvershootScalar` — dead code: orig's own
  header comment notes sole caller discards return; no side effects. Port
  omits entirely.
- `0x0043FB70 ReadTrackStaticDataChunk` — streaming inflate callback removed
  (mem-to-mem inflate in td5_inflate.c).
- `0x0043FBC0 DecompressTrackDataStream` — same: streaming bit-reader replaced
  by mem-to-mem inflate.

### td5_fmv.c (all 6 — EA TGQ stack replaced wholesale by Media Foundation)
- `0x0042CA00 DisplayLoadingScreenImage` — DDraw lock/blit/flip; replaced by
  td5_game.c:display_loading_screen_tga + D3D11.
- `0x0043C3C0 RequestIntroMovieShutdown` — EA TGQ shutdown; replaced.
- `0x00452E20 OpenAndStartMediaPlayback` — EA TGQ stream-open; replaced by
  td5_fmv_play / fmv_play_with_source_reader.
- `0x00452E60 SetStreamVolume` — orig writes volume + rebuilds YUV→RGB LUTs;
  port routes through IMFSourceReader audio attribute.
- `0x00452E80 IsStreamPlaying` — EA TGQ status query → IMFSourceReader state.
- `0x00452EC0 StopStreamPlayback` — EA TGQ stop sequence → IMFSourceReader
  release.

### td5_asset.c
- `0x0040AEF0 GetEnvironmentTexturePageMode` — D3D3 driver-caps query for
  16- vs 24-bit page mode; D3D11 uniform formats, query folded away.
- `0x0040BAE0 PreloadLevelTexturePages` — orig on-demand streaming via
  AdvanceTextureStreamingScheduler; port loads all level textures upfront in
  td5_asset_load_track_texture_set.
- `0x00412030 LoadFrontendTgaSurfaceFromArchive` — TGA + DDraw ImageProTGA →
  PNG pipeline + D3D11 texture; entire DDraw-surface object model gone.
- `0x00442D30 ResampleTexturePageToEntryDimensions` — page-resize against
  archive entry descriptor; PNG pipeline produces natural sizes for D3D11.
- `0x0043FC80 ParseZipCentralDirectory` — streaming chunked-reader → bulk
  malloc+linear-parse. ZIP-format constants byte-faithful (EOCD 0x06054B50,
  field offsets).
- `0x004405B0 DecompressZipEntry` — streaming inflate → td5_inflate_mem_to_mem.
  ZIP local-header field offsets + compression-method codes (0=stored,
  8=deflate) byte-faithful.

## Left at L4 (uncertain / fidelity-critical)

### td5_input.c (3) — semantic re-mapping unclear
- `0x00402E30 ResetPlayerVehicleControlAccumulators` — orig zeros 2 specific
  globals (DAT_00483014, DAT_00483018); port zeros 6 entries of s_nos_latch.
  Possible 4-slot mismatch. **Could be regression** (port over-zeros).
- `0x00402E40 ResetPlayerVehicleControlBuffers` — orig 6 dwords at
  gPlayerControlBuffer, port 6 entries of s_gear_debounce. Naming differs;
  matching unclear without runtime check.

### td5_sound.c (2)
- `0x00440A30 UpdateVehicleLoopingAudioState` — orig sets skidLoop flag
  (`(&g_skidLoopFlagAlternativeByActorView_PROVISIONAL)[slot*2] = 1` and
  `(&g_skidLoopStateByActorView)[slot*2] = 1`) when channel slot*3+3 isn't
  playing; port sets s_engine_state[*2] and [*2+1] to STOPPED if neither slot
  is playing. **Different state arrays touched**. Behaviorally divergent.
- `0x00441A80 LoadVehicleSoundBank` — orig sets 4 "99" init markers (across
  DAT_004c3774, g_engineRevLoopStateByActorView, g_engineLoopStateByActorView,
  g_sirenChannelPlayStateByActorView) per vehicle slot; port sets only
  s_engine_state[*2] and s_tracked_audio_state[*2] to ENGINE_STATE_STOPPED.
  **2 init markers possibly missing** — needs runtime verification before
  promoting.

### td5_asset.c (5) — too complex to byte-audit
- `0x0040B1D0 BuildTrackTextureCacheImpl` (839 B) — port has substantial
  `td5_asset_load_track_textures` + `td5_asset_load_race_texture_pages` with
  case-1/case-3 transparency rules audited inline ("Match BuildTrackTexture-
  CacheImpl @ 0x0040B1D0"), but full byte-level audit not feasible without
  more time. Many CONFIRMED inline citations already; the function as a whole
  remains L4 pending dedicated audit.
- `0x0040B530 SwapIndexedRuntimeEntries` — orig swaps paired 8-byte entries
  in one table AND 4-byte entries in a second table; port collapses to one
  s_page_remap swap. Functional intent preserved but byte-level diff.
- `0x0040B590 UploadRaceTexturePage` (204 B) — port references inline
  ("[CONFIRMED @ 0x40B590...]") but not all callsites map cleanly. Leaving L4.
- `0x0042F990 LoadEnvironmentTexturePages` (318 B) — port has comments at
  td5_asset.c:2349 mentioning the orig but no clear 1:1 port impl visible.
- `0x0042FD70 RemapCheckpointOrderForTrackDirection` — pairing structure
  matches port (4-col: (0,4)(1,3); 5-col: (0,5)(1,4)(2,3)) but orig walks
  raw dword stride and uses `gTrackDirectionSwitchFlag == -1` as
  discriminator while port uses `cols[20]` (table cell). Discriminator
  source may differ.
- `0x00430D30 ParseAndDecodeCompressedTrackData` (1113 B) — too large to
  byte-audit in budget.
- `0x00442670 LoadTrackTextureSet` (255 B) — referenced inline but not
  fully audited.

### td5_physics.c (8) — fidelity-critical, all left L4
- `0x00409520 CheckAndUpdateActorCollisionAlignment` — no clear port impl
  found by name; physics is highest-risk, leaving alone.
- `0x004096B0 ComputeActorWorldBoundingVolume` — same.
- `0x0043C9E0 InitializeTrackedActorMarkerBillboards` — HUD marker init;
  no port impl visible.
- `0x0043CDE0 RenderTrackedActorMarker` — HUD marker render; same.
- `0x0043D830 ApplyRandomWheelJitterHighSpeed` — orig writes wheel positions
  +0x2EC/+0x2F0/+0x2F4/+0x2F8 with (RNG&7 - 4) * (|long_spd|/256) deltas;
  port has td5_physics_auto_gear_select at td5_physics.c:8177 reading the
  same offsets but as drivetrain-kick during upshift (different code path).
  No port impl of the orig 0x0043D830 wheel-jitter detected — may be missing
  feature. Leaving L4 (potential regression).
- `0x0043D910 ApplyRandomWheelJitterLowSpeed` — same family as 0x0043D830;
  same status.
- `0x0043E4C0 InsertTriangleIntoDepthSortBuckets` — misclassified in manifest;
  this is a render function, lives in td5_render.c (4096 buckets), not in
  td5_physics.c.
- `0x004431C0 LoadTrafficVehicleSkinTexture` — folded into td5_asset.c
  (td5_asset.c:2694 traffic skin loader). Not in td5_physics.c per name.

## Suspected regressions / port wrongness

- **`td5_input.c:920 ResetPlayerVehicleControlAccumulators`** — orig zeros
  exactly 2 dwords (DAT_00483014 + DAT_00483018) but port loops 6 slots of
  s_nos_latch. Either the port is over-zeroing (4 extra slots cleared that
  the orig leaves preserved across runs), or the original is under-zeroing
  (2 dwords being just the head of a larger logical array). Worth a static
  trace to see if anything reads slots 2..5 of the NOS-latch array across
  run boundaries.
- **`td5_physics.c` 0x0043D830 / 0x0043D910 wheel-jitter pair** — neither
  appears to have a port equivalent. Both are small (211 bytes each) and
  apply per-wheel position jitter at high/low speed. **Potentially missing
  vehicle-physics feel feature.** Recommend a dedicated audit pass.
- **`td5_sound.c:374 LoadVehicleSoundBank`** — orig sets 4 per-vehicle "99"
  init markers across separate state arrays; port sets only 2. The other
  2 (rev-loop state + siren-channel state for that slot) might be left at
  stale values across vehicle reload, possibly causing first-frame audio
  glitches.

## Files modified

- `td5mod/src/td5re/td5_game.c` (citation-sweep header refined)
- `td5mod/src/td5re/td5_frontend_button_cache.c` (composite_half doc block)
- `td5mod/src/td5re/td5_ai.c` (citation-sweep header refined)
- `td5mod/src/td5re/td5_save.c` (3 function doc blocks)
- `td5mod/src/td5re/td5_sound.c` (2 function doc blocks)
- `td5mod/src/td5re/td5_input.c` (3 function doc blocks)
- `td5mod/src/td5re/td5_track.c` (citation-sweep header refined for all 5)
- `td5mod/src/td5re/td5_fmv.c` (file-header citation block added for 6 funcs)
- `td5mod/src/td5re/td5_asset.c` (citation-sweep header refined for 4 + 2 inline)
- `td5mod/src/td5re/td5_physics.c` — **not touched** (fidelity-critical, all L4)

All edits are pure comment-addition; no executable code changes.
