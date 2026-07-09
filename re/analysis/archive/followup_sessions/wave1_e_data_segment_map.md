# Wave 1 Agent E — .data Segment Map

Read-only audit of mutable globals in TD5_d3d.exe. Goal: enumerate clusters, identify highest-traffic globals, and shortlist struct-typing candidates for Wave 2.

Tooling: Ghidra MCP `ghidra_eval` against TD5_pool10 (read-only); helper data in `/tmp/wave1e/`.

---

## A. Section overview

TD5_d3d.exe collapses `.data` and `.bss` into one PE section:

| Block      | Start       | End         | Size       | Permissions |
|------------|-------------|-------------|-----------:|-------------|
| `Headers`  | 0x00400000  | 0x00400fff  |    4096 B  | R           |
| `.text`    | 0x00401000  | 0x0045cfff  |  376832 B  | R+X+W       |
| `.rdata`   | 0x0045d000  | 0x00462fff  |   24576 B  | R           |
| **`.data`** | **0x00463000** | **0x004d0d2b** | **449836 B** | **R+W**     |
| `IDCT_DAT` | 0x004d1000  | 0x004d2fff  |    8192 B  | R+W         |
| `UVA_DATA` | 0x004d3000  | 0x004d7fff  |   20480 B  | R+W         |
| `.rsrc`    | 0x004d8000  | 0x004d9fff  |    8192 B  | R           |

There is no separate `.bss` block — the MSVC 6 linker merged uninitialized into `.data` (size on disk matches virtual size). `IDCT_DAT` (M2DX FMV inverse-DCT tables) and `UVA_DATA` (likely M2DX video-decoder LUT) are vendor look-ups, not game state.

**.data inventory (Ghidra view):** 2705 defined data items.
- 545 `g_*` labels (applied from T1-T5 sweeps and Ghidra's auto-analysis)
- 776 `DAT_xxxxxxxx` stubs (auto-named placeholders, no semantic name)
- 811 unnamed (no Symbol object at all — appear as `DAT_xxxxxxxx` in decompiles via dynamic labelling)
- 429 `s_*` strings
- 144 misc (IID GUIDs, function pointer tables)

Of 3244 distinct addresses with at least one incoming reference: 1545 named, 1699 unnamed/DAT_*.

---

## B. Top 25 highest-traffic globals (by reference count)

Counted via `ReferenceManager.getReferenceCountTo(addr)` for every symbol in 0x00463000..0x004d0d2b.

| # | Refs | Address     | Name | Inferred purpose |
|--:|----:|-------------|------|------------------|
|  1 | 572 | 0x0049522c | `g_frontendAnimFrameCounter` | Frontend tick counter (incremented every loop iter) |
|  2 | 447 | 0x00495204 | `g_frontendInnerState` | Frontend inner-state FSM index |
|  3 | 262 | 0x0049628c | `g_lobbyErrorDialogSurface` | DXSurface ptr for error dialog overlay |
|  4 | **212** | **0x00496358** | **DAT_00496358** (unnamed) | **Pending frontend screen index** (next to `g_returnToScreenIndex` / `g_selectedGameType`; top callers all `Screen*Flow` / `RunFrontend*`) — high-value rename candidate |
|  5 | 152 | 0x004962c4 | `g_menuHeaderLabelSurfaceWidth` | Menu label dimensions (paired w/ +0x4, +0x8) |
|  6 | 152 | 0x004962c8 | `g_menuHeaderLabelSurfaceHeight` | (paired) |
|  7 | 152 | 0x004962cc | `g_menuHeaderLabelYOffset` | (paired) |
|  8 | 122 | 0x00495228 | `g_frontendCanvasW` | Frontend render canvas width |
|  9 | 118 | 0x00496270 | `g_smallTextSurface` | Frontend bitmap font surface |
| 10 | 110 | 0x00495200 | `g_frontendCanvasH` | Frontend render canvas height |
| 11 | 110 | 0x004c3d9c | `g_trackStripRecords` | Track strip record array ptr |
| 12 |  87 | 0x00496350 | `g_returnToScreenIndex` | Frontend return-to screen index |
| 13 |  79 | 0x004749d0 | `g_fixedPointToFloatScale` | Pure constant 1/256 (24.8 scale) |
| 14 |  74 | 0x004c3d98 | `g_trackVertexPool` | Track vertex pool base ptr |
| 15 |  73 | 0x00495240 | `g_frontendButtonIndex` | Currently-highlighted button index |
| 16 |  72 | 0x0049635c | `g_selectedGameType` | Active mode (TD5_GameType, typed) |
| 17 |  71 | 0x004c375c | `g_tireTrackQuadPool` | Tire-track effect ring buffer ptr |
| 18 |  69 | 0x0047b1cc | `g_inflateBitCount` | zlib inflate bit register |
| 19 |  62 | 0x00497314 | `g_postRaceNameButtonSurfacePtr_PROVISIONAL` | Post-race UI surface |
| 20 |  61 | 0x004aadf4 | `gRaceSlotStateTable` | 6 racer slots × 4 B (already typed) |
| 21 |  58 | 0x004bf6b8 | `g_currentRenderTransform` | Active render matrix base |
| 22 |  56 | 0x0047b1d4 | `g_zipCdReadCursor` | ZIP central-directory read cursor |
| 23 |  49 | 0x004ab108 | `g_actorRuntimeState` | 6 × Actor (904 B) array + tail data |
| 24 |  47 | 0x004a2c90 | `g_selectedScheduleIndex` | Cup schedule selector |
| 25 |  47 | 0x004aaf60 | `g_subTickFraction` | Sub-tick fraction for interpolation |

Three frontend globals (frontendAnimFrameCounter, frontendInnerState, lobbyErrorDialogSurface) dominate ref counts — they're spammed by the 30-screen FSM. The **only unnamed entry in the top 25 is `DAT_00496358` (#4, 212 refs)** — addressed in section D as a P0 naming candidate.

---

## C. Identified struct boundaries (suitable for Ghidra typing in Wave 2)

Discovery: scan all addresses with ≥5 refs; group consecutive hot addresses where neighbor gap ≤16 B. Result: 81 hot runs ≥3 addresses long.

Verified struct-like clusters (multi-reader, consistent stride):

### C.1 Frontend layout state @ 0x00496260..0x004962dc (124 B, 24 hot addresses, 1162 total refs)
- Surfaces: `g_primaryWorkSurface`, `g_secondaryWorkSurface`, `g_smallTextSurface`, `g_smallTextbSurface`, `g_smallFontSurface`, `g_lobbyErrorDialogSurface`
- Layout fields: `g_menuHeaderLabelSurfaceWidth/Height`, `g_menuHeaderLabelYOffset`
- All 24 fields named. Already de facto a "frontend renderer state" struct.

### C.2 Frontend tick state @ 0x004951d0..0x00495264 (148 B, 26 hot addresses, ~1550 total refs)
- Anchors: `g_postRaceQualifyingScore`, `g_cheatCodeKeyProgress`, `g_frontendInputEdgeBits`, `g_frontendFrameToggle`, `g_frontendCanvasH/W`, `g_frontendInnerState`, `g_frontendAnimFrameCounter`, `g_frontendButtonIndex`, etc.
- Strong "global tick state" feel — 10/10 hot fields named. Excellent type candidate.

### C.3 Race global block @ 0x004aaed0..0x004aafd8 (~264 B, 25 hot fields, ~322 refs)
- Mix of typed (TD5_GameType) and untyped scalars.
- 17 of 25 fields named (`g_simTimeAccumulator`, `g_racerCount`, `g_renderWidthF/HeightF`, `g_subTickFraction`, `g_wantedModeEnabled`, `g_selectedGameType`, `g_raceViewportLayoutMode`, etc.)
- 8 still `DAT_*` — naming gap below.

### C.4 Particle pool header @ 0x004a3170..0x004a31ac (60 B, 20 hot bytes)
- All but the head field (`g_raceParticlePoolBase`) are unnamed.
- All 20 fields read by the SAME 4 functions (`SpawnVehicleSmokeSprite`, `SpawnVehicleSmokeVariant`, `SpawnVehicleSmokePuffFromHardpoint`, `ProjectRaceParticlesToView`) — clear struct candidate.

### C.5 Connection-browser list layout @ 0x00499c78..0x00499cc0 (76 B, 19 hot fields)
- 3 fields named PROVISIONAL (`g_connBrowserListOriginX/Y`, `g_connBrowserListRowStride`); remaining 16 are `DAT_*` but all five top readers are the SAME 5 functions (`UpdateFrontendDisplayModeSelection`, `RenderFrontendDisplayModeHighlight`, `MoveFrontendSpriteRect`, `RenderFrontendUiRects`, `CreateFrontendDisplayModeButton`). Likely `ConnBrowserListLayout` struct.

### C.6 Polygon clipper draw-call state @ 0x004afb14..0x004afb50 (60 B)
- 8 hot fields, mostly read by `ClipAndSubmitProjectedPolygon`, `SetProjectedClipRect`, `RenderTrackSegmentBatch[Variant]`, `AppendClippedPolygonTriangleFan`.
- 3 named (`g_currentDrawCallVertexBuffer/IndexBuffer/VertexCount/IndexCount`); 5 are `DAT_*` clip-bounds and matrix fields. Tight struct candidate.

### C.7 gRaceSlotStateTable (already typed as `RuntimeSlotStateTable`, 24 B = 6 slots × 4 B)
- Visible in Ghidra as `slot[i].state`, `.companion_state_1`, `.companion_state_2`.
- Slot 0 sees most refs (state=27, c1=13, c2=11); slots 1-5 sparse — confirms 6-slot stride.

### C.8 g_actorRuntimeState (already typed as `RuntimeSlotActorArray`, 5552 B)
- Stride **904 B = 0x388** matches CLAUDE.md actor stride exactly. 6 slots × 904 B = 5424 B; trailing 128 B = additional tail at +0x1530 (refs at +0x1530 hint at shared scratch buffer).
- This type is the keystone for every cascade-unwind analysis. Already done well in earlier waves.

---

## D. Unnamed clusters — top candidates for further naming sweep

### D.1 P0 — `DAT_00496358` (#4 most-referenced, 212 refs, unnamed)
- Address: 0x00496358 (4 B, `undefined4`).
- Memory value at rest: 0x00000000.
- **Neighborhood:**
  - 0x00496350 `g_returnToScreenIndex` (87 refs)
  - 0x00496354 `DAT_00496354` (4 refs)
  - **0x00496358 DAT_00496358 (212 refs)** ← this one
  - 0x0049635c `g_selectedGameType` (72 refs, typed)
  - 0x00496368 `g_previousButtonIndex` (5 refs)
- **Top readers:** `ScreenMainMenuAnd1PRaceFlow` (16), `RunFrontendNetworkLobby` (15), `RunFrontendCreateSessionFlow` (14), `ScreenPostRaceNameEntry` (11), `ScreenControllerBindingPage` (10).
- **Top writers:** Same `Screen*` / `CarSelectionScreenStateMachine` writers as readers.
- **Inferred name:** `g_currentScreenIndex` or `g_pendingScreenIndex` (the active frontend screen ID). Sits literally between `g_returnToScreenIndex` and `g_selectedGameType` in the frontend FSM globals.

### D.2 P1 — Particle pool header (cluster C.4) 0x004a3170..0x004a31ac
- 19 unnamed adjacent bytes after `g_raceParticlePoolBase`.
- All read by the 4 smoke/particle spawn functions. Wave 2 should type this as a `RaceParticleSpawnState` struct.

### D.3 P1 — Connection-browser layout (cluster C.5) 0x00499c80..0x00499cc0
- 16 unnamed fields, all consumed by ~5 frontend functions.
- Likely a `ConnBrowserListLayout` struct with `origin_x/y, item_w/h, row_stride, scroll_offset, n_visible, render_top_y`.

### D.4 P1 — Render draw-call state (cluster C.6) 0x004afb18, 0x004afb20..0x004afb44
- 8 unnamed fields wedged between `g_currentDrawCallVertexBuffer/IndexBuffer/VertexCount/IndexCount`.
- Likely `clip_rect_min_x/y/max_x/y` (0x004afb20..0x004afb2c) and a 2-vec clip-bounds in 0x004afb38..0x004afb44.

### D.5 P2 — Race-global trailing unnamed scalars 0x004aaf9c..0x004aafd0
- 11 `DAT_*` in the C.3 race-global block. Top readers are camera / view-rotation functions → likely `g_viewRotationMatrixComponent[3][3]` field tail.

### D.6 P2 — High-traffic unnamed singletons
| Refs | Addr | Top callers | Suggested name |
|---:|-----|-------------|----------------|
| 56 | 0x004cfbe0 | `_read_nolock`, `_alloc_osfhnd`, `_ioinit` | MSVC libc stdio-handle table (probably `__pioinfo` — vendor) |
| 39 | 0x0048dc40 | `BuildTrackTextureCacheImpl`, `AdvanceTextureStreamingScheduler` | `g_textureStreamingScheduler` |
| 37 | 0x004af280 | `ClipAndSubmitProjectedPolygon`, `RenderTrackSegmentBatch[Variant]` | `g_polygonClipScratch` |
| 33 | 0x004968ad | `ProcessFrontendNetworkMessages`, `RunFrontendCreateSessionFlow` | `g_lobbyPendingNetMsgFlag` |
| 31 | 0x0047d6e6 | `_input`, `__strgtold12`, `_strtoxl` | vendor libc (`_locale` or `__lconv`) |
| 27 | 0x004cfce8 | `_strlwr`, `_wctomb`, `_mbtowc`, `_toupper`, `_tolower` | vendor libc (`__mb_cur_max`) |
| 26 | 0x0047d8e8 | `_input`, `__strgtold12`, `_strtoxl` | vendor libc |
| 22 | 0x004c3725 | `UpdateFrontTireEffects`, `UpdateRearTireEffects` | `g_tireEffectSpawnPolicy` |
| 21 | 0x00497a50 | `ScreenControlOptions`, `ScreenSoundOptions` | `g_optionsScratchState` |
| 18 | 0x004ac6b8 | `InitializeTrafficActorsFromQueue`, `RecycleTrafficActorFromQueue` | `g_trafficQueueCursor` (matches T1.04 hypothesis from memory) |
| 17 | 0x004cfce0 | (libc) | vendor |
| 15 | 0x004ab070 | (close to actor block) | `g_renderHardLodTable` candidate |

About one-third of the top unnamed singletons are MSVC 6 libc internals (`__pioinfo`, `__mb_cur_max`, locale tables) — those should be tagged ARCH-COLLAPSED so they're not retried in a future naming sweep.

---

## E. Tractable struct-typing candidates (Wave 2)

Five high-value structs that would each clean up many decompiles once typed.

| Rank | Proposed type | Addr range | Size | Hot fields | # functions improved |
|--:|-------------|-----------|-----:|----:|---:|
| 1 | `FrontendUiState` (frontend tick + canvas) | 0x004951d0..0x00495264 | 148 B | 26 | ~40 |
| 2 | `FrontendRenderResources` (surfaces + label dims) | 0x00496260..0x004962dc | 124 B | 24 | ~35 |
| 3 | `RaceParticleSpawnState` (cluster C.4) | 0x004a3170..0x004a31b0 | 64 B | 20 | 4 spawn fns (concentrated) |
| 4 | `PolygonClipperDrawState` (cluster C.6) | 0x004afb14..0x004afb50 | 60 B | 8 | 6 |
| 5 | `ConnBrowserListLayout` (cluster C.5) | 0x00499c78..0x00499cc0 | 76 B | 13 | 8 |

Bonus tractable singletons (1-shot renames):
- `DAT_00496358` → `g_currentScreenIndex` (212 refs, single field rename, huge readability win).
- `DAT_004ac6b8` → `g_trafficQueueCursor` (already aligns with prior `_PROVISIONAL` hypothesis).
- `DAT_0048dc40` → `g_textureStreamingScheduler`.

Existing high-value structs already in place (validate; don't re-do):
- `RuntimeSlotStateTable` (24 B, 6×4) at 0x004aadf4.
- `RuntimeSlotActorArray` (5552 B, 6×904 + 128 B tail) at 0x004ab108.
- `TD5_GameType` enum at 0x004aaf6c (4 B). Note: a SECOND `g_selectedGameType` label appears at 0x0049635c (72 refs); these are two different fields with the same name — name collision worth resolving.

---

## Methodology notes

- Pool slot used: TD5_pool10 (acquired/cleaned via `ghidra_pool.sh`); program opened with `read_only=true`.
- All counts via `ReferenceManager.getReferenceCountTo()` and `getReferencesTo()` — same source the decompiler uses, so they directly predict decomp impact.
- Hot-run grouping uses a 16-byte cap (typical 32-bit struct field stride) and ≥3 adjacent hot fields. Strides ≤4 B are NOT claimed as struct boundaries unless multiple readers consistently address the same offsets — every cluster in §C passes that test.
- Cluster gap threshold for §C separator detection: 128 B (chosen empirically from the histogram of inter-item gaps).
- Single-function unnamed runs (e.g., libc `_input` internals) are deliberately excluded from struct candidates — they're scratch buffers, not game state.

## Honesty caveats

- The "Pending frontend screen index" naming for `DAT_00496358` (D.1) is a strong hypothesis from caller distribution, but the actual value semantics weren't decompiled in this read-only pass. A 10-minute decomp-side check on `RunFrontendNetworkLobby` would confirm/deny.
- `g_selectedGameType` appears twice (0x4aaf6c and 0x49635c) — the second is *not* a typo, both labels exist. The Frontend-side one is likely the "selected during menu navigation"; the race-side one is the "active during race". Worth disambiguating before Wave 2 types either.
- 132 of the L3 triage "not ported" items were ARCH-COLLAPSED libc/FMV/zlib (per memory note 2026-05-21). Several of the top unnamed singletons in §D.6 belong to the same category and should not be auto-renamed.
