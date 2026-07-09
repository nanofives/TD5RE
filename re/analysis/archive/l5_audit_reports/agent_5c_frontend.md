# L5 Audit Report — td5_frontend.c (Agent 5(c))

**Date:** 2026-05-21
**Scope:** `td5mod/src/td5re/td5_frontend.c` — 22 remaining L4 entries from
`re/analysis/l5_audit_manifests/td5_frontend.c.csv` after Phases 4(a) / 5(a).
**Method:** Static comparison of Ghidra decomp (TD5_d3d.exe via pool slot 12)
against port `Screen_*` impls. Only added comments; no executable code touched.

## Totals

- **Total in scope:** 22
- **Promoted L5 (CONFIRMED byte-faithful):** 0
- **Promoted L5 (ARCH-DIVERGENCE documented):** 18
- **Skipped (left at L4):** 4
- **Suspected regressions surfaced:** 2

## Promoted L5 — ARCH-DIVERGENCE (18)

### Class-manifest extensions (new entries added to existing Phase 4(a)/5(a) footer blocks):

| Orig address | Function | Class | Justification |
|---|---|---|---|
| `0x00424B30` | `CopyPrimaryFrontendBufferToSecondary` | SurfBlit | DDraw 16bpp primary→secondary Blt(0x1c) call; port has no secondary surface (clears each frame). |
| `0x00424BC0` | `CopyPrimaryFrontendRectToSecondary` | SurfBlit | Same — rect variant of primary→secondary Blt. |
| `0x004242B0` | `DrawFrontendLocalizedStringPrimary` | FontStr | 24x24 BodyText DDraw per-glyph Blt(0x11); port → fe_draw_text glyph-strip. |
| `0x00424740` | `DrawFrontendClippedStringToSurface` | FontStr | 12x12 SmallText per-glyph Blt + control-code subglyphs <0x20; port → fe_draw_text. |
| `0x004248E0` | `DrawFrontendWrappedStringLine` | FontStr | Word-wrap helper that calls Clipped variant; port consolidated. |
| `0x00424A50` | `MeasureOrCenterFrontendLocalizedString` | FontStr | Per-glyph advance sum; port → fe_measure_text reads same s_font_glyph_advance table. |
| `0x00418D50` | `RunFrontendConnectionBrowser` | DXPTYPE | Network screen; port has placeholder Provider button list, no DirectPlay enumeration. |
| `0x0041A7B0` | `RunFrontendCreateSessionFlow` | DXPTYPE | Network screen; port states 4-15 collapse into single dispatch to NETWORK_LOBBY. |

### New Phase 5(c) class manifests (new footer blocks):

| Orig address | Function | Class | Justification |
|---|---|---|---|
| `0x0040D590` | `LoadExtrasGalleryImageSurfaces` | Gallery | Orig batch-loads 5 pic*.tga at init; port loads sequential on-demand. |
| `0x0040D750` | `AdvanceExtrasGallerySlideshow` | Gallery | Orig: random non-dup picker + 256-tick cross-fade; port: fixed 4000ms sequential advance. |
| `0x0040D830` | `UpdateExtrasGalleryDisplay` | Gallery | Orig: CrossFade16BitSurfaces per-scanline pixel arithmetic; port: full-viewport fe_draw_quad. |
| `0x00417DD2` | `LoadFrontendExtrasGalleryResources` | Gallery | Orig batch-loads 21 mugshots + 5 Legals at init; port: on-demand via s_gallery_names[]. |

### Per-screen FSM manifest (Phase 5(c) new block):

| Orig address | Function | Verdict |
|---|---|---|
| `0x00413010` | `DrawPostRaceHighScoreEntry` | Column geometry byte-faithful (col_name=+16, +0x80, +0xe4, +0x160, +0x1bc), 5-row loop matches stride=0x10, score-type mask &3 matches orig. |
| `0x00413580` | `ScreenPostRaceHighScoreTable` | 9 states (0..8) map case-for-case; wrap [0..0x19] with cheat at 0x1a matches orig. |
| `0x00418460` | `ScreenMusicTestExtras` | 9 states (0..8) map; idx clamp 0..11, CDPlay(idx+2,1), y=0/0x28/0x50 layout all byte-faithful (multiple [CONFIRMED @ ...] tags already in port). |
| `0x004213D0` | `ScreenQuickRaceMenu` | 7 states (0..6) map; button positions byte-faithful (120,137 / 120,257 / 120,377 / 232,377); cheat-mode 0x24 wrap matches. |
| `0x00427630` | `TrackSelectionScreenStateMachine` | 9 orig states + 1 port-only refactor state for the track-switch slide-in. NPC-group skip mask &3 byte-faithful. flow_context 2/4 branches mirror g_mainMenuButtonHint==2/4. |
| `0x00427290` | `ScreenLanguageSelect` | 7 states map; 4 named buttons replace orig's 4 menu-rect entries from a single flag sheet — quadrant indices preserved. |

## Skipped (left at L4) — 4

| Orig address | Function | Reason |
|---|---|---|
| `0x00415030` | `ScreenPositionerDebugTool` | Port is partial — case-5 file write is logged-only ("TD5_LOG_I writing positioner.txt"), grid editing in cases 3/4 stubbed. Orig writes full positioner.txt with two 0x25-iteration glyph-rect tables. Not promotable until port writes the file. |
| `0x00415370` | `ScreenStartupInit` | Port case-4 redirects to `TD5_SCREEN_LOCALIZATION_INIT`; orig redirects to `g_frontendScreenFnTable` (= main menu via case-0). Cases 0-3 also differ (port skips orig's BltColorFillToSurface dialog + ActivateFrontendCursorOverlay + QueueFrontendOverlayRect at center). Either intentional bootstrap divergence or a regression; needs runtime investigation. |
| `0x004168B0` | `RaceTypeCategoryMenuStateMachine` | See **regression #1** below — button→game_type mapping discrepancy needs verification before promotion. State structure (0..12, 0x14) IS correct. |
| `0x0041EA90` | `ScreenSoundOptions` | See **regression #2** below — SFX mode toggle and volume step deltas differ. State structure (0..9 vs orig 0..8) is correct. |

## Suspected regressions (surfaced — not fixed)

### Regression #1: RaceTypeCategoryMenuStateMachine button→game_type mapping

**Orig** (case-3 + case-4 dispatch combined):
- button 0 → `g_selectedGameType = 0` ("Single Race")
- button 3 → `g_selectedGameType = 9` (special case `if idx==3`) ("TimeTrials")
- button 4 → `g_selectedGameType = idx + 3 = 7` ("DragRace")
- button 5 → `g_selectedGameType = idx + 3 = 8` ("CopChase")

**Port** (td5_frontend.c:6383-6432):
- button 0 → `s_selected_game_type = 0` ("Single Race") — matches
- button 3 → `s_selected_game_type = 7` ("Time Trials") — **differs**
- button 4 → `s_selected_game_type = 9` ("Drag Race") — **differs**
- button 5 → `s_selected_game_type = 8` ("Cop Chase") — matches

The disagreement could either be (a) port intentionally renumbered the
SNK_RaceTypeText[] table so type 7=TimeTrials, type 9=Drag, OR (b) the port has
swapped the meanings of types 7 and 9 relative to the orig binary, in which
case the case-4 description preview, ConfigureGameTypeFlags, and downstream
race init would all read the wrong race-type-text/config entry.

**Triage path:** check what `td5_save_get_npc_group`, `frontend_get_track_name`,
and `td5_game_set_race_type` do for game_type in {7, 8, 9}. If they match orig's
DragRace/CopChase/TimeTrials wiring, this is just a button-label refactor and
the L5 promotion is safe. If they match port's apparent Time/Drag/Cops wiring,
the case-4 description preview likely displays the wrong text — visible bug.

### Regression #2: ScreenSoundOptions SFX mode + volume step

**Orig case-6 (0x0041EAA0+0x900):**
- SFX Mode (button 0): cycles `g_sfxPlaybackMode` in `[0..2]` if `DXSound::CanDo3D()`, else `[0..1]`. Three modes: 0=Stereo, 1=Mono, 2=3D-Surround.
- Master volume (button 1): `g_persistedMasterVolumePercent += navDirection * 10` (clamp 0..100).
- CD volume (button 2): `g_persistedCdVolumePercent += navDirection * 10` (clamp 0..100).

**Port** (td5_frontend.c:7529-7543):
- SFX Mode (button 0): `s_sound_option_sfx_mode ^= 1` — toggles 0↔1 only; no CanDo3D check; mode 2 (3D surround) unreachable.
- Master volume (button 1): `+= delta * 5` (half-step).
- CD volume (button 2): `+= delta * 5` (half-step).

**Effect:** 3D-surround SFX mode unselectable from menu (silently capped to mono/stereo). Volume slider takes 20 clicks 0→100 instead of orig's 10 clicks. Both are user-facing.

## Notes / lessons

1. **Phase 5(c) consolidated rather than per-function**: of the 22 entries, 10
   collapse cleanly into class manifests that already existed (SurfBlit, FontStr,
   DXPTYPE) and 4 form a new Gallery class. Only 8 (= 6 screen FSMs + 2 skipped)
   needed per-function audit.

2. **State-machine screens are surprisingly portable**: every screen FSM I
   verified — PostRaceHighScore, MusicTestExtras, QuickRaceMenu, TrackSelection,
   LanguageSelect — preserves the orig case-count and case-to-case state graph
   verbatim. The DDraw→D3D11 collapse is purely about how each case *renders*,
   not how it *transitions*. This validates the "byte-faithful FSM structure +
   ARCH-DIVERGENCE rendering" pattern.

3. **Two screens have regressions that would have been hidden by a blind
   promotion**: RaceTypeCategoryMenuStateMachine's button→game_type table and
   ScreenSoundOptions's SFX-mode cycle. The honest skip surfaced them. The fix
   for either is a small static edit (a table renumber or restoring the
   3-mode cycle gated by td5_sound_can_do_3d() if that exists) — both are
   tractable but should be triaged before claiming "L5 byte-faithful".

4. **Footer-class consolidation continues to pay off**: 10 of the 18 promoted
   entries needed only one-line additions to existing manifests. Per-screen
   audit budget was preserved for screens that actually needed case-by-case
   review.

5. **Pool slot 12 used; cleaned up via `scripts/ghidra_pool.sh cleanup`.** No
   leaked locks at session end. Pool slot 0 remains permanently stale per
   project memory (reboot-pending).

## Ghidra session

- Pool slot: TD5_pool12
- Session id: 31555ccb1f9f4ac0b090a90c9ba24350 (closed via program_close + cleanup)
