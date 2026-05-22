---
batch: 07
area: frontend_orchestrators
tier: T2
target_todos: [reference_arch_run_frontend_display_loop_2026-05-18, reference_arch_play_intro_movie_2026-05-18]
ghidra_session: e4d8557d51d14389a76925c93a8427ab
analyzed_addresses: 0x00414B50, 0x0043C440, 0x0042C8E0, 0x0043C3C0
agent: Claude Opus 4.7 (1M context)
date: 2026-05-20
---

# Globals enumeration -- Frontend orchestrators (loop / FMV / legal screens)

## Summary

- Functions analyzed: 4
  - `RunFrontendDisplayLoop @ 0x00414B50` (996 bytes) -- per-frame frontend dispatcher
  - `PlayIntroMovie @ 0x0043C440` (1206 bytes) -- TGQ playback orchestrator
  - `RequestIntroMovieShutdown @ 0x0043C3C0` (123 bytes) -- intro-movie cleanup callback (FMV co-owner)
  - `ShowLegalScreens @ 0x0042C8E0` (286 bytes) -- legal-screen blit loop
- Unnamed `DAT_*` globals encountered: **28** (after de-dup)
- Already-named globals encountered (noted, not re-named): 12 (`g_frontendRedrawCount`, `g_startRaceRequestFlag`, `g_frontendInputEdgeBits`, `g_frontendInputPreviousBits`, `g_frontendInputCurrentBits`, `g_frontendMouseEdgeBits`, `g_frontendMouseMovedFlag`, `g_frontendMousePrevX`, `g_frontendMousePrevY`, `g_frontendMouseCursorEnabled`, `g_currentScreenFnPtr`, `g_startRaceConfirmFlag`, `g_frontendCursorOverlayHidden`, `g_frontendCursorTextureId`, `g_frontendHardwareFlipEnabled`, `g_frontendBackSurfacePtr`, `g_frontendFrameToggle`, `g_frontendEscKeyButtonIndex`, `g_frontendButtonIndex`, `g_frontendButtonPressedFlag`, `g_npcRacerGroupTable`, `gNpcRacerCheatFlags`, `g_cheatCodeActivated`, `g_frontendAnimFrameCounter`, `g_frontendFrameTimestamp_ms`, `g_frontendInnerState`, `g_attractModeIdleCounter`, `g_attractModeTrackIndex`, `g_frontendScreenTransitionFlag`, `g_attractModeControlEnabled`)
- Proposals -- high confidence: **18**
- Proposals -- medium confidence: **7**
- Proposals -- comment-only (low confidence): **3**

## Methodology

Entry points were the three functions called out by the user prompt. For each, decompiled (`decomp_function`), then walked every `DAT_*` token, verified it was still `DEFAULT` source (i.e. unnamed) via `symbol_by_name`, and reverse-traced via `reference_to` to confirm:

1. Writer set is confined to the frontend / FMV module (not a misclassified physics global).
2. Read sites tell a coherent story about the semantic role.
3. Initial value (via `listing_data_at` + `memory_read`) supports the proposed name.

Relevance gate for inclusion in this batch: a `DAT_*` is in-scope iff it is written or referenced inside `RunFrontendDisplayLoop / PlayIntroMovie / RequestIntroMovieShutdown / ShowLegalScreens`. Globals that overlap but are owned by other areas (e.g. config-persistence cheat flags read by physics) are listed in the "Out-of-scope finds" section.

## Proposals

### Cheat-code octet decoder (RunFrontendDisplayLoop @ 0x00414DD0-0x00414E70)

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x004654a4 | u8[6][40] | `g_cheatCodeKeySequenceTable` | high | 6 entries x 0x28-byte stride; each row is a VK_* sequence terminated by 0xFF (e.g. row0 = `17 39 23 1E 2F 12 39 14 23 12 39 25 12 15 FF`); indexed `[slot * 0x28 + progress[slot]]` in 0x00414de1; orig comment "key sequences" | port replaces with `frontend_match_cheat_code("OPENALL")` style char matcher at td5_frontend.c:2443 (semantic, not table-byte) |
| 0x004951f0 | u8[6] | `g_cheatCodeKeyProgressTable` | high | 6 per-slot byte counters, incremented on key-match in 0x00414e07; reset to 0 on either consume-success path or non-options-screen path; orig comment "6-byte progress array" | port uses `s_cheat_key_history[32]` ring at td5_frontend.c:471 (rebuilt, not byte-mirror) |
| 0x00465594 | void*[6] | `g_cheatCodeTargetFlagPtrTable` | high | 6 pointers to flag dwords. Bytes confirm: `{0x00496298, 0x004962a8, 0x004962ac, 0x004aaf7c, 0x0049629c, 0x004962b4}`. Read at 0x00414e26 as `*(uint*)(table[i])`. Already auto-labeled `PTR_DAT_00465594` -- rename to a real symbol. | (none -- port resolves cheat actions directly, no pointer table) |
| 0x004655ac | u32[6] | `g_cheatCodeXorMaskTable` | high | 6 dwords `{1, 8, 2, 1, 1, 1}`; XOR'd into the targeted cheat flag at 0x00414e26 when sequence completes. The `8` at idx=1 means cheat #1 toggles bit 3 of `DAT_004962a8`. | (none) |

### NPC cheat flag block (referenced via cheat table at 0x00465594)

These four flag dwords are written from inside `RunFrontendDisplayLoop`'s cheat decoder, but also read by code outside the frontend. They are listed here because the writer lives in this area.

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x00496298 | u32 | `g_cheatFlagUnlockAll` | high | Cheat #0 target. Read by `CarSelectionScreenStateMachine` (0x0042142e), `WritePackedConfigTd5` (0x0040fa1c) -- persisted in Config.td5. Port mirrors as `s_cheat_unlock_all` at td5_frontend.c:261 (commented "DAT_00496298"). | td5_frontend.c:261 `s_cheat_unlock_all` |
| 0x004962a8 | u32 | `g_cheatFlagBitfieldGameModes` | high | Cheat #1 target, XOR mask = 8 (toggles bit 3). Read/written by `LoadPackedConfigTd5` (0x0040fda9 W, 0x0040fa1c R) -- persisted. Also read by `CarSelectionScreenStateMachine` (0x00410f89..) -- gates car-selection branches. | (none -- not modeled) |
| 0x004962ac | u32 | `g_cheatFlagBitfieldExtras` | high | Cheat #2 target, XOR mask = 2 (bit 1). Read by `CarSelectionScreenStateMachine` (0x0042140b) and `RunRaceResultsScreen` (0x00421990) and adjacent extras-gallery code at 0x0040e07b, 0x0040e8f8. | (none) |
| 0x0049629c | u32 | `g_cheatFlagBitfieldNetwork` | medium | Cheat #4 target, XOR mask = 1 (bit 0). Read by `UpdatePlayerVehicleControlState` (0x004034dd), `ProcessFrontendNetworkMessages` (0x0041b77d), `RunFrontendNetworkLobby` (0x0041d2d0). The network readers suggest a "network features unlock" cheat. | (none) |

### Attract-mode track selector (RunFrontendDisplayLoop @ 0x00414F08)

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x004668b0 | u8[19] | `g_attractModeTrackDisableMask` | high | Read as `(&DAT_004668b0)[g_attractModeTrackIndex]` in a `do { idx = rand() % 0x13; } while (mask[idx] != 0);` loop. Initial bytes `{00, 00, 00, 00, 00, 00, 00, 00, 01, 01, 01, ...}` confirm tracks 0-7 = playable (mask=0), tracks 8-18 = disabled (mask=1). Matches the memo's "Port's 8-track filter is the same set after filtering." | port shortcut: `rand() % 8` at td5_frontend.c:3130 (mask dropped, set hardcoded) |

### FMV playback control block -- PlayIntroMovie state (0x004BEA7C..0x004BEACC)

These dwords are the TGQ-engine-facing state mirror; populated at function entry, consumed when building the TGQ control block.

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x004bea7c | u32 | `g_fmvUseDdrawSurfaceFlag` | high | Inverted boolean: when 0, the SetCooperativeLevel(0x1000400) dance runs (capture DDraw7 + Surface7 via QI) and the Sleep(2000) resync fires. Memo step 2 calls this "DDraw mode flag". Read at 0x0043c50c (entry-gate), 0x0043c6de (TGQ flag-builder), 0x0043c8ae (teardown). | (none) |
| 0x004bea80 | u32 | `g_fmvTgqControlBlockPtr` | medium | Read only once at 0x0043c5fc into `_DAT_004bcba0` (TGQ control-block field). Likely a pre-allocated TGQ context pointer set up elsewhere (not in this function). Initial value 0. | (none) |
| 0x004bea84 | u32 | `g_fmvShouldPlayFlag` | high | Set to 1 at 0x0043c492 unconditionally at function entry. Implied write-only here; consumed by the TGQ async pipeline. Cosmetic "should-play arm" latch. | (none) |
| 0x004bea88 | u32 | `g_fmvAppModeSnapshot` | high | At 0x0043c48c: `DAT_004bea88 = *(int *)(g_appExref + 0x174);`. The memo identifies `g_appExref + 0x174` as "input/backend mode flag"; this is the cached snapshot at FMV-entry time. Re-read at 0x0043c71c to choose `_DAT_004bcb98 = 7` (no audio) vs `1` (with audio). | (none) |
| 0x004bea8c | u32 | `g_fmvTgqStreamConfigSelectFlag` | medium | Read once at 0x0043c6a2 to choose `DAT_004bcb94 \|= 0x11140c` vs `\|= 0x91140c`. Single read; no writer in this module -- set during DXSound/DXDraw init elsewhere. The memo cites these as "TGQ stream-config flags". | (none) |
| 0x004bea90 | u32 | `g_fmvHasFloatingParamA` | medium | Cleared at 0x0043c467; read at 0x0043c663 -- if nonzero, sets TGQ flag bit 0x80 and copies `DAT_004bea9c` into `_DAT_004bcc00`. Functions as a "feature A enabled" gate. | (none) |
| 0x004bea94 | u32 | `g_fmvHasFloatingParamB` | medium | Cleared at 0x0043c4ce; read at 0x0043c655 -- if nonzero, sets TGQ flag bit 0x20 (and clears bit 0x10). Functions as a "feature B enabled" gate. | (none) |
| 0x004bea9c | u32 | `g_fmvFloatingParamA` | medium | Cleared at 0x0043c45b; read at 0x0043c66b (only consumed when `DAT_004bea90 != 0`). Companion to `DAT_004bea90`. | (none) |
| 0x004beaa0 | u32 | `g_fmvFeatureFlagA` | medium | Cleared at 0x0043c4b8; read at 0x0043c64a -- if nonzero, sets TGQ flag bit 0x10. | (none) |
| 0x004beaa4 | u32 | `g_fmvFeatureFlagB` | high | Set to 1 at 0x0043c4b2 at entry; read at 0x0043c617 -- if nonzero, sets TGQ flag bit 0x40 (combined with 0x600000 base). Default-on feature. | (none) |
| 0x004beaa8 | u32 | `g_fmvUnusedStateD` | low | Cleared at 0x0043c4d4 at entry. No reader anywhere in the binary -- single writer in this function. Dead field in the TGQ context init. | (none -- comment-only) |
| 0x004beab0 | HINSTANCE | `g_fmvGameModuleHandle` | high | At 0x0043c4f2: written with `GetModuleHandleA(NULL)`. No reader anywhere in the binary -- TGQ engine expected to read it via the control-block pointer chain (`_DAT_004bcba0`/etc), not directly. | (none) |
| 0x004beab4 | u32 | `g_fmvFloatingParamB` | medium | Cleared at 0x0043c461; read at 0x0043c684 (only consumed when `DAT_004bd344 != 0`). Companion to `DAT_004bd344`. | (none) |
| 0x004beab8 | i32 | `g_fmvVolume` | high | Set to 100 at 0x0043c479 (entry default). Read/written by VK_ADD / VK_SUBTRACT switch (0x0043c7fb, 0x0043c80b, 0x0043c81a) with `+= 10` / `-= 10` (clamped >= 0). Passed to `SetStreamVolume(stream, vol)`. The "intro-movie volume" axis the memo flags as not implemented in port. | (none -- volume keys NOT IMPLEMENTED in port; see memo step 5) |
| 0x004beabc | LPDIRECTSOUND | `g_fmvDsObjectPtr` | high | Out-param of `DXSound::GetDSObject(&DAT_004beabc, &DAT_004beac0)` at 0x0043c517. Captured DirectSound device pointer used by TGQ audio mixer. Copied into `_DAT_004bcbe4` (TGQ control-block field). | (none -- D3D11 path) |
| 0x004beac0 | LPDIRECTSOUNDBUFFER | `g_fmvDsPrimaryBufferPtr` | high | Second out-param of the same `DXSound::GetDSObject` call. Captured primary buffer used by TGQ for raw PCM output. Copied into `_DAT_004bcbe8`. | (none) |
| 0x004beac8 | int* | `g_fmvActiveStreamHandle` | high | Written by `OpenAndStartMediaPlayback(...)` return at 0x0043c790. Read by `IsStreamPlaying`, `StopStreamPlayback`, `SetStreamVolume` throughout the loop. Cleared to NULL at 0x0043c8a6 / 0x0043c8ba (teardown). Read by `RequestIntroMovieShutdown` at 0x0043c3f7 for IsStreamPlaying gate. Port equivalent is `IMFSourceReader*` -- not byte-faithful. | port `s_fmv.reader` IMFSourceReader\*, td5_fmv.c |
| 0x004beacc | u32 | `g_fmvShutdownLatch` | high | Set to 1 at 0x0043c857 (skip path), 0x0043c42f (RequestIntroMovieShutdown), 0x0043c8e6 (return path). Read at 0x0043c5d8 as the only loop-gate -- if 1, the whole TGQ playback loop is skipped. The "shutdown latch" the memo names explicitly. | port returns 0/1 from `fmv_play_with_source_reader`; semantic equivalent |

### FMV TGQ control-block master + flag scratch (0x004BCB80..0x004BD350)

These are the master TGQ control-block memory area (size 0x7B4 from base at 0x004BCB90) and four scratch fields immediately after it.

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x004bcb80 | u32 | `g_fmvUseRgbOverlayFlag` | medium | Cleared at 0x0043c498; read at 0x0043c6c0 -- if nonzero, sets TGQ flag bit 0x4000 and copies `DAT_004bd350` into `_DAT_004bcc0c`. RGB-overlay mode select. | (none) |
| 0x004bcb84 | u32 | `g_fmvOutputBitsPerPixel` | high | Written with `0x10` (16) at 0x0043c4be. Only consumed via the TGQ control-block triple-copy. Matches the memo's "16bpp output" comment. | (none -- port uses A8R8G8B8 32bpp at td5_fmv.c) |
| 0x004bcb88 | u32 | `g_fmvAudioEnabledFlag` | high | Set to 1 at 0x0043c47f at entry. Read at 0x0043c6f2 -- gates whether DAT_004bea7c==0 branch picks audio-on TGQ mode (`uVar4 \|= 0xb000000`). Default-on audio. | (none -- port audio is silent; memo step 6) |
| 0x004bcb90 | u32[493] | `g_fmvTgqControlBlockMaster` | high | Zeroed by `for (i = 0x1ed; ...)` at 0x0043c570; then populated field-by-field; then triple-copied to `DAT_004bd358`, `DAT_004be2c8`, `DAT_004bdb10`. Master TGQ context. Sub-field accesses `_DAT_004bcba0/a4/9c/e8/e4/c40/c00/c04/c08/c0c` (already labeled with `_` prefix overlap) are offsets into this block. Size = 0x1ED * 4 = 0x7B4 bytes = 1972. | (none -- TGQ replaced by MF) |
| 0x004bcb94 | u32 | `g_fmvTgqContextFlagWord` | high | Built up bit-by-bit (`0x600000 \| 0x40 \| 0x10 \| 0x20 \| 0x80 \| 0x100 \| 0x8000 \| 0x11140c \| 0x91140c \| 0x4000 \| 0x3000000 \| 0xb000000 \| 0x200`); written multiple times in the flag-builder block 0x0043c600-0x0043c701. Master "TGQ feature flag word" -- inside the master block but pulled out as a named field because it has many writers. | (none) |
| 0x004bcb98 | u32 | `g_fmvTgqAudioConfig` | high | Written 7 (no audio) at 0x0043c724 when `DAT_004bea88 != 0`, else written 1 at 0x0043c721. Inside the master block (offset 8). Audio-config mode select. | (none) |
| 0x004bd344 | u32 | `g_fmvHasLineParamA` | medium | Cleared at 0x0043c46d; read at 0x0043c67c -- if nonzero, sets TGQ flag bit 0x100 and copies `DAT_004beab4` into `_DAT_004bcc04`. Companion to floating-param chain. | (none) |
| 0x004bd348 | u32 | `g_fmvTgqEngineHandle` | high | Read at 0x0043c5e8 (via `uVar2 = DAT_004bd348;`) and at 0x0043c53a as DATA-reference (likely a vtbl call site). Initial value 0. Acts as the master TGQ engine pointer; populated by DXSound/DXDraw init. Copied into `_DAT_004bcb9c`. | (none) |
| 0x004bd34c | u32 | `g_fmvForceFullscreenFlag` | medium | Cleared at 0x0043c4c8; read at 0x0043c695 -- if nonzero, sets TGQ flag bit 0x8000 (fullscreen). | (none) |
| 0x004bd350 | u32 | `g_fmvRgbOverlayParam` | low | Read once at 0x0043c6c8 (only consumed when `g_fmvUseRgbOverlayFlag != 0`). Single reader, no in-module writer. | (none -- comment-only) |

### FMV resolution + ownership latch (0x00474834..0x0047483C)

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x00474834 | u32 | `g_fmvOutputWidth` | high | Written `0x280` (640) at 0x0043c49e. Hardcoded fixed 640x480 output, confirmed by memo. | port uses platform window size via `td5_plat_get_window_size`; not byte-mirrored |
| 0x00474838 | u32 | `g_fmvOutputHeight` | high | Written `0x1E0` (480) at 0x0043c4a8. Companion to width above. | (same as above) |
| 0x0047483c | u32 | `g_fmvOwnershipClaimed` | high | Set externally (not in this function) before PlayIntroMovie is called; checked at 0x0043c8a0 / cleared at 0x0043c8b4 to gate teardown work. Same gate in RequestIntroMovieShutdown @ 0x0043c3c0 (set 1 -> clear). This is the "did we install the shutdown callback" latch. Closing path: avoids double-shutdown when both the main thread and the callback try to release surfaces. | (none) |

### DDraw7 QueryInterface IID constants (read-only data, 0x0045DF08 + 0x0045DF38)

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x0045df08 | GUID | `IID_IDirectDraw7` | high | Bytes `E0 F3 A6 B3 43 2B CF 11 A2 DE 00 AA 00 B9 33 56` = `B3A6F3E0-2B43-11CF-A2DE-00AA00B93356`. This is the standard `IID_IDirectDraw7` published by Microsoft in `ddraw.h`. Passed as 2nd arg to QI at 0x0043c563. | port uses `IID_IDXGIFactory` chain in ddraw_wrapper |
| 0x0045df38 | GUID | `IID_IDirectDrawSurface7` | high | Bytes `85 58 80 57 EC 6E CF 11 94 41 A8 23 03 C1 0E 27` = `57805885-6EEC-11CF-9441-A82303C10E27` = `IID_IDirectDrawSurface7`. Passed at 0x0043c577 and 0x0043c580. | (same) |

## Key discoveries

1. **Cheat-code XOR mask `8` at slot 1 is a 4-bit packed flag, not a single boolean.** The XOR-mask table at 0x004655ac is `{1, 8, 2, 1, 1, 1}`. The non-unit `8` for cheat #1 (which targets `DAT_004962a8`) means that cheat toggles BIT 3, not the LSB. The other 5 cheats are simple booleans (XOR with 1, 2, 1, 1, 1). The port's `s_cheat_unlock_all` is a single int -- if the port ever adds cheat #1, it MUST preserve the bit-3 semantics or `LoadPackedConfigTd5 / WritePackedConfigTd5` round-trips will corrupt the saved bitfield.

2. **The cheat decoder has a side-effect that runs even when the OPENALL cheat is OFF.** Inside the cheat-completion branch at 0x00414e26-0x00414e80, AFTER xoring the target flag, the code walks `g_npcRacerGroupTable` (stride 0xA4) and sets `gNpcRacerCheatFlags[i] |= 2` (cheat ON) or `&= 1` (cheat OFF) on every NPC racer with `*pcVar4 == '\0'`. This is the "broadcast cheat state to NPCs" side effect, and the port's `frontend_match_cheat_code("OPENALL")` block at td5_frontend.c:2474 only refreshes lock-tables -- it does NOT propagate the cheat to NPC racers. **Possible missing-writer for cheat-mediated NPC behaviour in port.**

3. **The 50000 ms attract-mode timer in orig uses `g_attractModeIdleCounter < -0xF` as the SECOND gate.** The signed counter must drop below -15 in addition to the wall-clock 50 s timeout. The memo lists only the 50 s timeout; port uses only 60 s and drops the negative-counter gate. This means port's attract trigger can fire 10 s earlier in some scenarios where orig would still be holding (counter not yet < -15). Cosmetic but documented.

4. **`g_fmvOwnershipClaimed @ 0x0047483c` is the double-shutdown guard.** Set externally before PlayIntroMovie is called (likely by `td5_game_play_intro_movie` or its caller). Both the main thread (PlayIntroMovie exit at 0x0043c8b4) and the async shutdown callback (RequestIntroMovieShutdown at 0x0043c3d0) check + clear it. Without this gate, calling `RequestIntroMovieShutdown` after PlayIntroMovie returns would double-release the DDraw7/Surface7 interfaces. Port does not have this concern (MF teardown is idempotent), but it's worth knowing if any port path ever re-installs the orig callback.

5. **The TGQ context is THREE complete copies of a 1972-byte master block.** Master at `g_fmvTgqControlBlockMaster @ 0x004bcb90`, copies at `0x004bd358`, `0x004be2c8`, `0x004bdb10`. The TGQ engine runs a triple-buffer / three-stream design (likely one for compressed-input, one for decoded-frame, one for audio-mix). Each copy is identical at function entry. The port has no analog; this is dead architecture once TGQ is gone.

6. **`g_fmvOutputBitsPerPixel = 0x10` (16bpp) is a hard constant.** The TGQ codec only emits RGB565 / RGB555. Modern D3D11 path uses 32bpp (X8R8G8B8) -- already noted as ARCH-DIV in the memo, but the actual byte value (16 == orig) is documented here for completeness.

7. **`g_fmvUnusedStateD @ 0x004beaa8` is dead even in orig.** Single writer (clear at 0x0043c4d4), zero readers anywhere in the binary. Likely a TGQ feature flag that was stripped from the shipping build. No port impact.

## Out-of-scope finds

| address | brief note | probable area |
|---|---|---|
| 0x00496298 | `g_cheatFlagUnlockAll` -- writer in frontend, but readers in `CarSelectionScreenStateMachine` and `WritePackedConfigTd5/LoadPackedConfigTd5` (Config.td5 persistence) | Config persistence (T2 batch) |
| 0x004962a8 | `g_cheatFlagBitfieldGameModes` -- car-selection state machine reader | Car-selection state (T2 batch) |
| 0x004962ac | `g_cheatFlagBitfieldExtras` -- extras-gallery reader | Frontend extras gallery |
| 0x0049629c | `g_cheatFlagBitfieldNetwork` -- network lobby reader | Network frontend (T3 batch) |
| 0x004aaf7c | Cheat #3 target -- read by `UpdatePlayerVehicleControlState` and `InitializeRaceSession` (the latter via 0x0042b3b8) | Race-init / physics cheats |
| 0x00465594 (PTR_DAT_) | Already labeled as PTR_DAT_ -- rename as part of this batch but the auto-label exists | (handled here) |
| g_appExref+0x138 | RequestIntroMovieShutdown callback hook slot -- struct member, not a global | App-context struct (T3 layout) |
| g_appExref+0x174 | App-mode flag, read once into `g_fmvAppModeSnapshot` | App-context struct (T3 layout) |
| g_appExref+0x100/+0x104/+0x108 | Mouse state in app-context struct | App-context struct (T3 layout) |

## TODO impact

**reference_arch_run_frontend_display_loop_2026-05-18:**
- Adds 4 named cheat-decoder tables (`g_cheatCodeKeySequenceTable @ 0x004654a4`, `g_cheatCodeKeyProgressTable @ 0x004951f0`, `g_cheatCodeTargetFlagPtrTable @ 0x00465594`, `g_cheatCodeXorMaskTable @ 0x004655ac`) and 1 attract-mode mask (`g_attractModeTrackDisableMask @ 0x004668b0`).
- Surfaces a possible LOST WRITER: the cheat-completion branch at 0x00414e26 broadcasts cheat state to every NPC racer via `gNpcRacerCheatFlags[i] |= 2` / `&= 1`. Port's td5_frontend.c:2474 OPENALL handler does NOT do this. If cheat-affected NPC behaviour is observed missing in the port (e.g. NPCs not unlocking secret race tier), this is the lever.
- No code change to `RunFrontendDisplayLoop` itself -- the divergences flagged in the memo (DDraw surface-lost, HW/SW flip) remain intentional ARCH-DIV.

**reference_arch_play_intro_movie_2026-05-18:**
- Adds 23 named FMV-state globals across 0x004BEA7C..0x004BEACC + 0x004BCB80..0x004BD350 + 0x00474834..0x0047483C + 2 IID constants. None of these need a port mirror since the entire TGQ pipeline is ARCH-DIV-replaced by Media Foundation.
- Surfaces no LOST WRITER -- the TGQ-side state is intentionally collapsed away; port behaviour is correct (skip on TGQ filename, play through MF on MP4).
- One follow-up: `g_fmvVolume @ 0x004beab8` and its VK_ADD/VK_SUBTRACT keybinds are NOT implemented in port (memo step 5). If FMV-volume control is ever wanted, the keybinds are: VK_ADD = +10, VK_SUBTRACT = -10, clamp >= 0, default = 100. Single-edit candidate once an MF volume path lands.

**No related TODO for ShowLegalScreens @ 0x0042c8e0** -- the function contains NO unnamed `DAT_*` globals (all string refs are already named s_legal1_tga_004672f4 / s_legal2_tga_004672e8 / s_LEGALS_ZIP_00467300). Zero proposals from this function; included for completeness in the analyzed_addresses list.
