---
batch: 22
area: attract_mode_intro
tier: T5
target_todos: []
ghidra_session: 882edf51e09f4946ac60d0774f2d637f
analyzed_addresses: 0x00442170, 0x00414B50, 0x004275A0, 0x004274A0, 0x00415370, 0x004269D0, 0x00427290, 0x0042AA10, 0x00411780, 0x00411A70, 0x00411750, 0x00411A50, 0x00411710, 0x0040D750, 0x0040D830, 0x0040D590, 0x0040D6A0, 0x0040D120, 0x00417D50, 0x00415490
agent: Claude Opus 4.7 (1M context)
date: 2026-05-20
---

# Globals enumeration — Attract mode / intro / demo (T5 batch 22)

## Summary

- Functions analyzed: **20** primary entry points (4-state FSM intro path + frontend screens involved in attract-mode entry/exit)
  - `RunMainGameLoop @ 0x00442170` — top-level FSM (GAMESTATE_INTRO branch)
  - `RunFrontendDisplayLoop @ 0x00414B50` — attract-mode trigger gate + cheat decoder
  - `RunAttractModeDemoScreen @ 0x004275A0` — screen-fn that launches the attract-mode race
  - `ScreenLegalCopyright @ 0x004274A0` — post-Pitbull copyright screen (3s timer)
  - `ScreenStartupInit @ 0x00415370` — startup "frame container" before any frontend screen
  - `ScreenLocalizationInit @ 0x004269D0` — boot-time LANGUAGE.DLL loader + return dispatcher
  - `ScreenLanguageSelect @ 0x00427290` — language picker (called when discriminator = 2)
  - `ScreenMainMenuAnd1PRaceFlow @ 0x00415490` — main menu (writes attract-mode trigger control)
  - `InitializeRaceSession @ 0x0042AA10` — race-init (attract-mode branch sets `g_replayModeFlag=1`)
  - `RenderFrontendFadeEffect @ 0x00411780` — fade-in animation walker
  - `RenderFrontendFadeOutEffect @ 0x00411A70` — fade-out animation walker
  - `InitFrontendFadeColor @ 0x00411750` — fade init (seed dither channels)
  - `ResetFrontendFadeState @ 0x00411A50` — fade reset
  - `BuildFrontendDitherOffsetTable @ 0x00411710` — 32×32 dither LUT builder
  - `AdvanceExtrasGallerySlideshow @ 0x0040D750` — slideshow phase advancer (mis-shares "attract idle" counter)
  - `UpdateExtrasGalleryDisplay @ 0x0040D830` — slideshow per-frame updater
  - `LoadExtrasGalleryImageSurfaces @ 0x0040D590` — extras-gallery slide loader (5 TGAs)
  - `LoadExtrasBandGalleryImages @ 0x0040D6A0` — band-gallery slide loader (5 TGAs)
  - `AdvanceCrossFadeTransition @ 0x0040D120` — frontend cross-fade clear
  - `ScreenExtrasGallery @ 0x00417D50` — credits scroller (also uses the "idle" counter)
- Unnamed `DAT_*` globals encountered in scope: **18** (after de-dup)
- Already-named globals encountered (verified, not re-renamed): **12** (`g_attractModeIdleCounter`, `g_attractModeTrackIndex`, `g_attractModeControlEnabled`, `g_attractModeDemoActive`, `g_introMoviePendingFlag`, `g_gameState`, `g_replayModeFlag`, `g_inputPlaybackActive`, `g_replayOrBenchmarkActive`, `g_initialScreenFnPtr`, `g_frontendScreenFnTable`, `g_currentScreenFnPtr`)
- Proposals — high confidence: **12**
- Proposals — medium confidence: **5**
- Proposals — comment-only (low confidence): **1**
- **STRUCTURAL renames flagged** (existing names misleading): **2** (see Key Discoveries §1 and §2)

## Methodology

Started from `RunMainGameLoop` (the EXE-wide 4-state FSM) and the GAMESTATE_INTRO branch. From there walked the actual screen functions in `g_frontendScreenFnTable` that are part of the intro/title sequence (`ScreenStartupInit`, `ScreenLocalizationInit`, `ScreenLegalCopyright`, `ScreenLanguageSelect`, `RunAttractModeDemoScreen`, `ScreenExtrasGallery`).

For the attract-mode trigger itself, decompiled `RunFrontendDisplayLoop` and tracked the wall-clock + signed-counter gate at 0x00414EBA-0x00414F1E. For each `DAT_*` encountered, ran `symbol_by_name`, `reference_to`, and cross-decompiled the writers/readers in this area.

**Special focus on misnamed-existing-name cases**, per T3.11 (`g_cameraTransitionActive`-style mistakes) and prior tier instructions. Two of those surfaced here (see Key Discoveries §1 and §2).

**Relevance gate**: a `DAT_*` is in-scope iff it is referenced inside one of the 20 attract/intro entry points, OR it is in the extras-gallery cross-fade pipeline that drives the very counter the attract trigger reads (so the "what is this counter actually" investigation lands in scope).

## Proposals

### Misnamed-existing-name STRUCTURAL renames (highest impact)

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x0048f2fc | i32 | **`g_extrasGalleryCrossFadePhase`** (existing `g_attractModeIdleCounter` is MISLEADING — propose **rename**) | high | Used as the per-frame phase counter for `CrossFade16BitSurfaces`-driven slideshow transitions, NOT as an idle clock. Set to `0x100` (256, max-phase) at `AdvanceExtrasGallerySlideshow @ 0x0040D77E` and at the gallery-state-2 transition in `UpdateExtrasGalleryDisplay @ 0x0040D87A`. Halved each frame (`counter / 2`) during cross-fade, then decremented by 1 per tick, clamped to `-0x40` (-64) at idle. Cleared to 0 by `AdvanceCrossFadeTransition @ 0x0040D17B`, `LoadExtrasGalleryImageSurfaces @ 0x0040D5BC`, `LoadExtrasBandGalleryImages @ 0x0040D6A2`. RunFrontendDisplayLoop's `< -0xF` gate at 0x00414EBA is a SECONDARY consumer — it just checks "has the gallery been idle long enough to start attract". The orig binary in fact only ever fires attract when the extras-gallery (or credits) was previously displayed and let drift. Renaming this clarifies why attract trigger gate is harder to satisfy than the memo's "50000ms wall clock" alone suggests. 22 xrefs. | port: `td5_frontend.c:471` `s_cheat_key_history[32]` is unrelated; port's attract-mode trigger has no analogous gallery-phase counter and uses a pure 60s timeout. **TODO impact**: explains why port can fire attract earlier than orig — orig's secondary gate IS the gallery counter, port dropped it entirely (intentional simplification, but documented now). |
| 0x004a2c8c | i32 | **`g_frontendBootDispatchMode`** (existing `g_attractModeControlEnabled` is MISLEADING — propose **rename**) | high | This is a 3-state DISCRIMINATOR for `ScreenLocalizationInit @ 0x004269D0`, not an attract-mode toggle. Decompile shows: `== 0` → first-time boot path (load LANGUAGE.DLL via 17 carZipPath entries, parse joystick state, write Config.td5, then set `=1`); `== 1` → call `SetFrontendScreen(5)` (main menu); `== 2` → call `SetFrontendScreen(0x18)` (language-select screen) then set `=1`. Writers: `RunFrontendDisplayLoop @ 0x00414F27` writes `=1` after attract trigger (so post-attract returns to main menu skipping language-select); `ScreenMainMenuAnd1PRaceFlow @ 0x0041551E` writes `=2` at state 0 (so re-entry from race goes through language-select); `RunFrontendNetworkLobby @ 0x0041C7E2 / 0x0041D58E` and `RunRaceResultsScreen @ 0x004232FB / 0x004233FB` also write. 10 xrefs. | port: not modeled byte-faithfully; port's `s_frontend_initialized` flag covers the boot path. |

### Frontend cross-fade / fade-effect state cluster (5 globals)

These drive `RenderFrontendFadeEffect` (used by `ScreenLegalCopyright` for fade-in/out animation between the Pitbull/Accolade and the main-menu/legal screens).

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x00494fc0 | u32 | `g_frontendFadeActive` | high | "Fade animation is running" gate. Set to 1 at `InitFrontendFadeColor @ 0x00411765` and `ResetFrontendFadeState @ 0x00411A60`. Cleared to 0 at `RenderFrontendFadeEffect @ 0x00411884` and `RenderFrontendFadeOutEffect @ 0x00411AAB` when `g_frontendFadeScanCursor` exceeds canvas height. Read at `ScreenLegalCopyright @ 0x004274FF / 0x00427527 / 0x0042757A` and `RunAttractModeDemoScreen @ 0x004275FF` (state 5 — wait-for-fade-complete). 7 xrefs. | port: `td5_frontend_button_cache.c` / fade-state — semantic match, not byte-faithful. |
| 0x00494fc4 | u8 | `g_frontendFadeChannelR` | high | One byte of the 3-byte `_DAT_00494fc4` cluster (overlaps with read-as-u32 `_DAT_00494fc4`). `InitFrontendFadeColor` writes `_DAT_00494fc4 = (color >> 3) & 0x1f1f1f` — i.e., packs 3 5-bit channel indices. This byte is the red-channel ramp index. Read at `RenderFrontendFadeEffect @ 0x00411868` etc. as the index into `g_frontendDitherRampLut`. 8 xrefs. | (none — port uses RGBA alpha lerp, not channel ramp.) |
| 0x00494fc5 | u8 | `g_frontendFadeChannelG` | high | Green-channel ramp index, byte 2 of the packed `_DAT_00494fc4` triplet. Read at `RenderFrontendFadeEffect @ 0x00411887`. 8 xrefs. | (none) |
| 0x00494fc6 | u8 | `g_frontendFadeChannelB` | high | Blue-channel ramp index, byte 3 of the packed triplet. Read at `RenderFrontendFadeEffect @ 0x004118A6`. 8 xrefs. | (none) |
| 0x00494fc8 | u32 | `g_frontendFadeScanCursor` | high | Scanline cursor for the fade band. Initialized to 0 in `InitFrontendFadeColor / ResetFrontendFadeState`. Advanced by `+2` each call in both `RenderFrontendFadeEffect` and `RenderFrontendFadeOutEffect` (so fade duration = `g_frontendCanvasH / 2 + 64` frames). The `0x40` (64) constant is the **band width** — fade is implemented as a 64-line moving gradient. 6 xrefs. | (none — port uses simple alpha lerp.) |

### Frontend dither-ramp LUT (1 global, 32×32 table)

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x00494bc0 | u8[32][32] | `g_frontendDitherRampLut` | high | 1024-byte (32×32) lookup table built by `BuildFrontendDitherOffsetTable @ 0x00411710` (called from `InitializeFrontendResourcesAndState @ 0x00414823`). Indexed `[channel_index << 5 \| pixel_5bit]` to produce the per-channel 5-bit fade-ramp output. Read in 30 places across `RenderFrontendFadeEffect`, `RenderFrontendFadeOutEffect`, and the cross-fade functions at `CrossFade16BitSurfaces`. Single writer, 30 readers. | (none — port does fade via D3D11 alpha blending.) |

### Extras-gallery slideshow state cluster (5 globals)

These run the per-frame slideshow inside `ScreenMusicTestExtras` / `ScreenExtrasGallery` / `UpdateExtrasGalleryDisplay`. **Note**: 4 are u32 pointers to `IDirectDrawSurface7` (the loaded TGA surfaces), 1 is a u32 count.

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x0048f2d4 | LPDIRECTDRAWSURFACE7[5] | `g_extrasGallerySlideSurfaces` | high | 5-element array of TGA surface pointers (offsets +0x00, +0x04, +0x08, +0x0c, +0x10 — i.e., DAT_0048f2d4/d8/dc/e0/e4). Populated by `LoadExtrasGalleryImageSurfaces` (Front End\Extras\pic1..pic5.tga) at game-start AND by `LoadExtrasBandGalleryImages` (band photos: Fear Factory, Gravity Kills, Junkie XL, KMFDM, Pitch Shifter). The two loaders OVERLAY the SAME 5 slots (band gallery replaces vendor gallery when entering the band screen). Indexed via the `DAT_00465e18` random selector. 10 xrefs total to base. | (none — port doesn't model extras gallery yet.) |
| 0x0048f300 | u32 | `g_extrasGallerySlideCount` | high | Slide count, written 5 by both loaders (or 0 if `DAT_00495234 == 0`). Read in `AdvanceExtrasGallerySlideshow @ 0x0040D773` to seed `rand() % count`. 6 xrefs. | (none) |
| 0x0048f304 | LPDIRECTDRAWSURFACE7* | `g_extrasGalleryCurrentSlidePtr` | high | Pointer to the currently-displayed slide surface (a pointer INTO `g_extrasGallerySlideSurfaces`, NOT into the array). Set to NULL or to one of the 5 slides by `AdvanceExtrasGallerySlideshow @ 0x0040D7A2`. Read by `UpdateExtrasGalleryDisplay @ 0x0040D8C0` to drive `CrossFade16BitSurfaces` / `Copy16BitSurfaceRect`. 8 xrefs. | (none) |
| 0x0048f2f4 | i32 | `g_extrasGallerySlideX` | high | X position of slide on screen. Written by `AdvanceExtrasGallerySlideshow @ 0x0040D7E4` as `rand() % (500 - some_val) + 0x8c` (= 140..640). Static value `0x76` (118) when set by gallery-state-2 init in `UpdateExtrasGalleryDisplay @ 0x0040D87E`. 4 xrefs. | (none) |
| 0x0048f2f8 | i32 | `g_extrasGallerySlideY` | high | Y position of slide on screen. Written `rand() % 0x150 + 0x54` (= 84..420) by slideshow advance. Static `0x8c` (140) in band-gallery state-2 init. 4 xrefs. | (none) |

### Extras-gallery + band-gallery selector (2 globals)

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x00465e18 | i32 | `g_frontendRandomMusicTrackIndex` | high | Random number 0..6 chosen in `InitializeFrontendResourcesAndState @ 0x00414A02-0x00414A50` for the menu CD track (excluding tracks 7 and certain pairs). Read by `ScreenExtrasGallery / UpdateExtrasGalleryDisplay / ScreenMusicTestExtras` and `LoadExtrasBandGalleryImages` paths via `(&DAT_0048f2d4)[*(char *)(DAT_00465e18 + 0x465e4c)]` indexed lookup. **Dual role**: index into music-test playlist AND seed for which band image to show. 10 xrefs. | port: `td5_sound.c` selects menu CD track via `g_td5.ini.cd_track` — not random in port. |
| 0x00463c8c | i32 | `g_extrasGalleryPreviousSlideIndex` | medium | Previously-shown slide pointer or `-1`. Read at `UpdateExtrasGalleryDisplay @ 0x0040D85C` to determine if a cross-fade is needed (`if (DAT_00463c8c == -1 \|\| different from current)` → start cross-fade). Written by `LoadExtrasBandGalleryImages @ 0x0040D720` (`= 0xFFFFFFFF` on gallery enter) and at gallery-state-2 init. Initial value 0xFFFFFFFF. 5 xrefs. | (none) |

### Misc fade/extras feature flag (1 global)

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x00495234 | u32 | `g_extrasGalleryEnabledFlag_PROVISIONAL` | medium | Set to 0 in `InitializeFrontendResourcesAndState @ 0x004147C3` and at `0x0042507C` (race-end transition). Read in `LoadExtrasGalleryImageSurfaces @ 0x0040D595` and `UpdateExtrasGalleryDisplay @ 0x0040D84C / 0x0040DA3A / 0x0040DA65` as the gate for whether to load/use extras-gallery surfaces. The dual-use as "cross-fade enabled" vs "feature enabled" is ambiguous; provisional name pending confirmation. 7 xrefs. | (none) |

### Language-select screen surface (1 global)

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x0049b698 | LPDIRECTDRAWSURFACE7 | `g_frontendLanguageFlagsSurface_PROVISIONAL` | medium | Set by `ScreenLanguageSelect @ 0x004272F3` (Language.tga from Front End\FrontEnd.zip). Used as the 4-flag overlay (one per language quadrant) at `0x00427313 / 0x00427333 / 0x00427363 / 0x00427392`. Released at `0x0042746F`. 7 xrefs. | (none — port's language is via `g_td5.ini.language`.) |

### Language-select per-screen latch (1 global)

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x00496290 | u32 | `g_languageSelectButtonHoverIndex_PROVISIONAL` | medium | Per-frame latch of `g_frontendButtonIndex` in `ScreenLanguageSelect @ 0x004273DC / 0x00427436`. Used to detect button-focus change to reset hover animation. 3 xrefs. | (none) |

### Startup-init dialog surface (1 global)

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x0049628c | LPDIRECTDRAWSURFACE7 | `g_frontendDialogPanelSurface_PROVISIONAL` | medium | Tracked frontend surface allocated in `ScreenStartupInit @ 0x00415377` (408×112) and used by `ScreenMainMenuAnd1PRaceFlow` later. Released at `0x004153D0`. Dual-use across multiple screens — semantic name pending wider audit. 4 xrefs in ScreenStartupInit alone. | (none — port renders dialogs directly.) |

### Frontend-boot mode "first boot" indicator (1 global, COMMENT-ONLY)

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x00497a6c | u32 | (`g_languageSelectPostBootFlag` — low confidence, comment-only) | low | Set to 1 only at `ScreenLocalizationInit @ 0x00427192` when `g_frontendBootDispatchMode == 2` (= the post-race re-init path). Read at `RunRaceResultsScreen @ 0x00422F8A / 0x0042304E / 0x0042310C` to influence end-of-race screen flow. Likely "indicate we just came through language-select on a re-init" but the readers are deep into RaceResults — out of attract scope. Comment-only. | (none) |

## Key discoveries

### 1. **`g_attractModeIdleCounter @ 0x0048f2fc` is NOT the attract-mode idle counter — propose rename to `g_extrasGalleryCrossFadePhase`**

This is a T3.11-style mis-named existing global. The name implies a simple "idle clock for attract trigger", but the variable is actually the per-frame phase counter for the extras-gallery slideshow cross-fade animation.

Evidence (from `UpdateExtrasGalleryDisplay @ 0x0040D830`):
- Set to `0x100` (256) at gallery-state-2 entry — the MAX phase value
- Halved each frame during cross-fade: `counter = counter / 2`
- Used to compute the alpha/scanline parameters for `CrossFade16BitSurfaces`
- Decremented by 1 per tick and clamped to `-0x40` (-64) at idle
- Triggers `AdvanceExtrasGallerySlideshow()` when counter reaches exactly `-0x18` (-24)
- Cleared to 0 at every `AdvanceCrossFadeTransition` (any screen transition)

The "attract-mode idle" reading is a SECONDARY use in `RunFrontendDisplayLoop @ 0x00414EBA`:
```
((g_currentScreenFnPtr == ScreenMainMenuAnd1PRaceFlow) &&
 (50000 < frame_ts - g_frontendInnerState) &&
 (g_extrasGalleryCrossFadePhase < -0xF))
   → fire attract mode
```

The third gate (`< -0xF`) only naturally satisfies if the player visited the extras gallery and left it idle long enough for the slideshow counter to drift below -15. **This means orig's attract-mode is gated on having visited extras at least once** — a finding the existing-name obscures.

**TODO impact**: Documents why port and orig differ on attract-mode firing time. Port (per memo) drops the negative-counter gate entirely and uses a clean 60s timeout. That's a legitimate ARCH-DIV; just confirm the rename doesn't reveal a missing port behavior.

### 2. **`g_attractModeControlEnabled @ 0x004a2c8c` is NOT attract-mode-related — propose rename to `g_frontendBootDispatchMode`**

This is the 3-state SCREEN-DISPATCH discriminator inside `ScreenLocalizationInit`, not an attract-mode control. The states are:
- `== 0` → first-time boot (load LANGUAGE.DLL for all 17 car-zip entries, init joystick, write Config.td5, then advance to `=1`)
- `== 1` → normal entry path → `SetFrontendScreen(5)` (main menu)
- `== 2` → post-race / network-lobby re-entry → `SetFrontendScreen(0x18)` (language-select screen), then advance to `=1`

The "AttractMode" in the existing name is a misnomer. The writer in `RunFrontendDisplayLoop @ 0x00414F27` (which sets `=1` when attract-mode triggers) is just one of 6 writers; it has the same SEMANTIC role as the others — "return to main menu next time we go through Localization".

**TODO impact**: Clarifies that the attract-mode trigger doesn't actually `enable` anything called "attract mode control" — it just sets the post-attract dispatch path to "skip language-select".

### 3. **`g_introMoviePendingFlag @ 0x00474c00` is the one-shot intro-movie gate, initialized to 1 in the EXE image**

Static value in the executable's `.data` section: `01 00 00 00`. So on every boot, `g_introMoviePendingFlag == 1`, and `RunMainGameLoop`'s GAMESTATE_INTRO branch will (a) attempt `PlayIntroMovie()` if `g_appExref + 0x14c != 0` and `g_appExref + 0x13c == 0`, then (b) `g_introMoviePendingFlag = 0`. After the first boot, the flag stays at 0 for the rest of the run — there's no "play intro again" path inside the binary. The flag is RAM-only (written 0 in-process) but RESETS each boot via the static initializer. This is the *real* "should we play intro this boot" latch. Already named — listed for completeness because the existing name accurately describes it; just documenting the static-init behaviour.

### 4. **Attract-mode race playback uses `g_replayModeFlag=1`, NOT `g_inputPlaybackActive=1`**

`InitializeRaceSession @ 0x0042B26F` shows the attract branch:
```
if (g_attractModeDemoActive != 0) {
    g_replayModeFlag = 1;
    g_inputPlaybackActive = 0;     // NOT inputs-from-file
    g_replayOrBenchmarkActive = 1;
    g_raceEndTimerStart = 0;
    g_trackPoolIndex = gTrackPoolSpanCountTable[gScheduleToPoolIndex[g_attractModeTrackIndex]];
}
```

The attract race plays back a saved REPLAY (full game state per frame) rather than re-running the sim from raw inputs. This means the attract-mode demos in TD5 are full state-snapshot replays — same format as the "View Replay" menu item — and they ship with the game (presumably in `tracks/level*.zip`). The port's missing replay-record path (todo_view_replay_restarts_race_2026-05-19) ALSO affects attract mode if attract is ever wired in port.

### 5. **`g_initialScreenFnPtr @ 0x00465634` = `ScreenStartupInit @ 0x00415370` (resolved)**

The frontend screen-table starts at `g_frontendScreenFnTable @ 0x004655c4`, but the FIRST screen displayed is actually `ScreenStartupInit` (a "frame container" that displays an OK button over a black 408×112 panel). `ScreenStartupInit` then dispatches to `g_frontendScreenFnTable[0]` (= `ScreenLocalizationInit`) at its case-4 exit. So the boot chain is:
```
GameWinMain → RunMainGameLoop → GAMESTATE_INTRO → PlayIntroMovie / ShowLegalScreens
            → GAMESTATE_MENU → InitializeFrontendResourcesAndState (sets g_currentScreenFnPtr = g_initialScreenFnPtr)
            → ScreenStartupInit (case 0-3 frame container)
            → ScreenLocalizationInit (langDLL load)
            → ScreenLegalCopyright (3-second copyright)
            → ScreenLanguageSelect (4-flag picker) — only if g_frontendBootDispatchMode == 2
            → ScreenMainMenuAnd1PRaceFlow
```

### 6. **Cross-fade is a `0x40`-line moving band (64-line gradient) advancing 2 lines/frame**

`RenderFrontendFadeEffect` and `RenderFrontendFadeOutEffect` both implement the fade as a single 64-scanline band that moves across the canvas at 2 lines/frame. So fade duration = `(canvas_h + 64) / 2 = (480 + 64) / 2 = 272` frames at 60Hz ≈ 4.5 seconds. The `64` is hard-coded in the `DAT_00494fc8 < 0x40` branch at function entry. This matches the visible "fade band" appearance in the original game's legal screen sequence. Single-magic-number constant worth noting if port ever re-implements byte-faithfully.

### 7. **The 16-bit fade pipeline supports both 5-6-5 and 5-5-5 surface formats via `DAT_0049525c` dispatch**

`RenderFrontendFadeEffect` has two near-identical loops gated by `DAT_0049525c == 0x10` (RGB-565) vs `== 0xF` (RGB-555). The bit-mask offsets differ (0x1f/0x3f vs 0x1f/0x1f). `DAT_0049525c` is set in `InitializeFrontendResourcesAndState @ 0x00414841` from `*(int *)(dd_exref + 0x16a0)` (the DDraw display surface's actual bit depth). Already noted in T2.x as out-of-scope here, but flagging for the consolidation: the variable is `g_frontendSurfaceBitDepth`-like — not attract-specific.

## Out-of-scope finds

Globals encountered while walking the attract chain that belong to other batches (or are intentionally separate from this batch's scope).

| address | brief note | probable area |
|---|---|---|
| 0x00474c00 | `g_introMoviePendingFlag` — already named (T2.6 batch) | (already named) |
| 0x00474840 | `s_Movie_intro_tgq_00474840` — already named | (already named) |
| 0x004BEA84 | `g_fmvShouldPlayFlag` — T2.7 PROPOSAL not yet consolidated | T2.7 FMV proposals |
| 0x004BEACC | `g_fmvShutdownLatch` — T2.7 PROPOSAL not yet consolidated | T2.7 |
| 0x004668b0 | `g_attractModeTrackDisableMask` — T2.7 PROPOSAL not yet consolidated; verified bytes `[00×8, 01×11]` confirm 8 attract-eligible tracks | T2.7 (already proposed by batch_07) |
| 0x004962d4 | Main-menu navigation state latch (20 xrefs across menu screens) | T5 frontend-menu batch (future) |
| 0x004962d8 | Main-menu navigation message pointer (5 xrefs) | T5 frontend-menu batch (future) |
| 0x004962c4, 0x004962cc | Menu-label surface width/height (100+ xrefs each) | T5 frontend-menu batch |
| 0x004962bc, 0x004962c0 | Network-lobby player-counts / per-slot active flags | T3.x network batch (already-discovered cluster) |
| 0x0049525c | `g_frontendSurfaceBitDepth` — 25 xrefs across many fade/blit functions | T5 frontend-render batch (future) |
| 0x00465fec, 0x00465ff0 | DXSound master/CD volume percents (used in Localization init) | T4.20 audio_loaders batch (overlap) |
| 0x00465680, 0x00465684 (+ siblings) | Joystick button-count / type capture tables (used in Localization init) | T5 input-config batch (future) |
| 0x00497a78 | Race-results "did we just come through localization" flag | T5 race-results batch (future) |
| 0x00497a74 | Race-series schedule "active" flag (11 xrefs) | T5 race-series batch (future) |
| 0x0049636c | "Benchmark mode entry was via main-menu time-demo button" latch (3 xrefs) | T5 benchmark batch (future) |
| 0x00496360, 0x00496354, 0x00496364, 0x00496358 | ScreenExtrasGallery credits-scroller state (4 globals — line index, page index, char-on-page index, surface ptr) | T5 frontend-credits batch (future) |
| 0x00465664..0x004656c0 | Joystick-config snapshot tables (Localization init writes for return-to-defaults gate) | T5 input-config batch |
| 0x00497330 (gConfiguredDisplayModeOrdinal + table base) | Display mode enumeration capture (already named) | (already named) |
| 0x0048f380, 0x00497328, 0x0048f324, 0x0048f30c | Race-series schedule + cup-state tables (15 globals in `InitializeRaceSeriesSchedule`) | T5 race-series batch (future) |

## TODO impact

No active TODOs in `MEMORY.md` directly touch the attract-mode / intro / demo area. The closest semantic neighbours are:

- **`todo_view_replay_restarts_race_2026-05-19`** — observation §4 of this batch confirms that attract-mode and "View Replay" share the same code path (`g_replayModeFlag=1`). Fixing the View Replay save-side (the missing replay-record callsite) would also enable attract-mode to be functional in port. No additional code change needed beyond that TODO.
- **`reference_arch_play_intro_movie_2026-05-18`** — Section §3 here documents the `g_introMoviePendingFlag` initial-value behaviour (1 in `.data`, never written 1 in-process) that complements the T2.7 FMV-state inventory.
- **`reference_arch_run_frontend_display_loop_2026-05-18`** — Section §1 and §2 here are sister findings to T2.7 batch_07's enumeration of cheat-decoder tables; together they identify TWO misnamed-existing-name globals in the attract-mode trigger path.

### General TODOs surfaced (no closures, but informative)

- **The 50000ms attract trigger has TWO gates in orig, not one.** Per memo `T2.7 key discovery #3`, the second gate (`g_extrasGalleryCrossFadePhase < -0xF`) requires the player to have visited the extras gallery at least once. Port currently uses only the 60s timeout (single-gate); this is an ARCH-DIV documented here, not a bug.
- **Attract-mode track selection uses `g_attractModeTrackDisableMask`** (`DAT_004668b0`, T2.7 proposal). Renaming this in the consolidation session will close the loop on this finding.

## Headline stats

- **Functions analyzed**: 20
- **New high-confidence proposals**: 12
- **New medium-confidence proposals**: 5
- **New low-confidence (comment-only)**: 1
- **STRUCTURAL renames recommended (misnamed-existing-name)**: 2 (`g_attractModeIdleCounter → g_extrasGalleryCrossFadePhase`; `g_attractModeControlEnabled → g_frontendBootDispatchMode`)
- **Most-frequently-referenced new global named**: `g_frontendDitherRampLut` (30 xrefs)
- **Most-impactful semantic correction**: §1 — `g_attractModeIdleCounter` is the gallery cross-fade counter, which explains why orig's attract trigger is harder to satisfy than the port's
- **Best mechanical-fix target**: §2 — `g_attractModeControlEnabled` rename eliminates a misleading "attract mode" association in 10 xrefs across 5 functions

## Ghidra session notes

- Session `882edf51e09f4946ac60d0774f2d637f` opened `TD5_pool1` read-only as required.
- Pool slot acquired via `bash scripts/ghidra_pool.sh acquire`.
- No writes to Ghidra performed. All names listed here are PROPOSED only — the consolidation session will apply them.
