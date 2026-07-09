# TD5 Frontend — Complete Element & Behavior Spec (corrected-methodology read)

**Generated 2026-05-31.** The authoritative, exhaustive read of the original Test Drive 5
frontend, built from the **provably-complete 171-function call-graph closure** (not a
scoped helper-scan). Supersedes the earlier per-screen harvests (`part_*.md`,
`behavior_*.md`), which were produced with a flawed method that missed flush-drawn
elements (see `frontend_re_methodology_postmortem.md`).

## How this was read (so nothing is missed)

The original frontend is **deferred/retained-mode**: a screen function mostly **primes**
queues + global state; the actual drawing happens in a shared **per-frame flush**
(`FlushFrontendSpriteBlits @0x425540`) that no screen function calls. Therefore this spec
classifies every element as **INLINE** (drawn into a surface by the screen) vs
**FLUSH/DECOUPLED** (primed by the screen, drawn later by the flush /
`UpdateExtrasGalleryDisplay` / `RenderFrontendDisplayModeHighlight` / the sprite list).

Engine-model companion docs (read these for the shared mechanisms each screen relies on):
- **`frontend_call_graph_closure.md`** — all 171 functions (BFS exhausted), categorized + the global contract; the 5 decoupled flush-draws + 22 pointer-dispatched handlers.
- **`frontend_rendering_model.md`** — per-frame flush order, every decoupled draw, draw/text/fill/fade helpers, the 3 glyph atlases (12px small = button labels), asset loaders + surface registry, the element-source map.
- **`frontend_flow_model.md`** — main loop order (screen PRIMES, flush DRAWS), screen FSM, the universal inner-state pattern, the frame-counter animation model, input routing, pointer-dispatch tables, persistence.

## Previously-missed elements now captured (the point of this pass)

- **Music Test (19) album cover art** — a FLUSH/decoupled element (`UpdateExtrasGalleryDisplay @0x40d830`), one of 5 band covers selected by the track→band LUT `@0x465e4c`, drawn at (118,140) with a crossfade on track change. (Was mislabeled "cosmetic gallery, not ported.")
- **Selection highlight bars** — FLUSH-drawn on every menu screen (`RenderFrontendDisplayModeHighlight @0x4263e0`), reading `g_connBrowserListOriginX[]`. Invisible to a screen-scoped scan.
- **List browsers (8/9)** — rows baked inline into a panel surface; scroll indicators + selection bar are FLUSH-queued sprites.
- **Network lobby (11)** — chat-history / player-roster-status / chat-input panels, all FLUSH-drawn (16 elements).
- **Slide-in/out animations** — frame-counter-driven (`g_frontendAnimFrameCounter`), transitions on exact counter equality → frame-rate-coupled (the "abrupt transitions" symptom).

## Key corrections to earlier assumptions

- **Screen 22 (ExtrasGallery) is the dev-credits + Legals vertical scroll reel** (22 mugshots + 5 Legals into the secondary surface) — NOT the band-cover slideshow. The band covers are screen 19's separate decoupled set.
- **Car-select (20) preview is a pre-rendered `CarPic%d.tga`** slid in via `DrawCarSelectionPreviewOverlay @0x40ddc0` (FLUSH) — NOT a 3D render-to-surface. (The stray "car" seen on the port's Music Test was a leaked preview/background from an abnormal `--StartScreen` jump that skipped surface teardown.)
- **Main menu (5) has NO 3D background** — it's a static `MainMenu.tga`.
- **Name entry (25) uses the DXInput `GetString` edit-box model** (field + blinking caret), not an alphabet grid.
- **Button labels use the 12px small-font atlas** (`smalltext`/SmallText2), not 24px BodyText; label text comes from `LANGUAGE.DLL` `SNK_` strings (e.g. the short "TRACK").

---

# Per-screen element & behavior detail

# Frontend Screens 00–04 — Complete Element + Behavior Spec

RE target: `TD5_d3d.exe` (image base 0x00400000). All addresses are Ghidra VAs from literal
decompilation. Engine model referenced (not re-derived): `frontend_rendering_model.md`,
`frontend_flow_model.md`, `frontend_call_graph_closure.md`.

**Global contract (per the engine model — applies to every screen below):**
- `g_frontendInnerState` = `[0x00495204]` (the switch selector for every screen fn).
- `g_frontendAnimFrameCounter` = `[0x0049522c]` (LOOP `++` each frame; zeroed at transitions).
- `g_frontendButtonIndex` = `[0x00495240]`; `g_frontendButtonPressedFlag` = `[0x004951e8]`.
- `g_frontendInputEdgeBits` = LOOP-computed rising-edge keyboard/stick mask.
- Per the flush model: a screen-fn PRIMES (queues overlay rects, builds slot-table entries,
  bakes/loads surfaces, drives fades inline); `FlushFrontendSpriteBlits` 0x00425540 (called by
  LOOP after the screen fn) DRAWS — drains the overlay double-buffer (plain copy into
  `g_primaryWorkSurface`), then UNCONDITIONALLY runs `UpdateExtrasGalleryDisplay` 0x0040d830 +
  `RenderFrontendDisplayModeHighlight` 0x004263e0, then the cursor-tracked sprite list. These two
  decoupled draws and the LOOP-queued cursor sprite + `RenderFrontendUiRects` slot pass affect
  EVERY screen even when its own fn never references them.

---

### Screen 0 @0x004269d0 — ScreenLocalizationInit  [interactive N]
Inner states: Single gate `if (g_frontendInnerState != 0) return;` — runs ONLY on the first frame
after entry (inner-state seeded by SetFrontendScreen to a `timeGetTime()` ms value, so re-entry is
inert until reset). Branch selector is `g_frontendBootDispatchMode` (`[0x004a2c8c]`), a 3-state gate:
- `==0`: full one-time localization/config init (the big block below), then sets
  `g_frontendBootDispatchMode=1` and `SetFrontendScreen(5=MAIN_MENU)`.
- `==1`: skip init, `SetFrontendScreen(5=MAIN_MENU)` directly (PUSH 0x5 @0x00427173 — CONFIRMED).
- `==2`: replay/boot-to-results path — `g_replayFileAvailable=1`, `SetFrontendScreen(0x18=24=
  RACE_RESULTS)` (PUSH 0x18 @0x00427190 — CONFIRMED), reset boot mode to 1, then re-quantize
  master+CD volume (DXSound::GetVolume / CDGetVolume → `g_persistedMasterVolumePercent` /
  `g_persistedCdVolumePercent`, rounded to multiples of 10, re-applied via SetVolume/CDSetVolume).

ELEMENTS:
| element | produced by (fn/global/asset) | INLINE/FLUSH | dest pos | gating | notes |
|---|---|---|---|---|---|
| (none drawn) | — | — | — | — | This screen draws NOTHING. It is a pure data/config init: NO surface load, NO fill, NO text, NO overlay-rect/sprite enqueue. It exits via SetFrontendScreen on its only active frame. |
| Car-name/spec localization strings | per-car loop over `gCarZipPathTable`..`0x466f70` (stride 0x330): `ReadArchiveEntry` keyed by `SNK_LangDLL_exref[8]` language index, `_sscanf` parse via fmt `0x00466744`, `'_'`→space fixups; localized substitution from `SNK_Layout_Types_exref` / `SNK_Engine_Types_exref` / `SNK_Conf*_exref` LUTs; fallback strings `SNK_ConfUnknown_exref` / `s_UNKNOWN_0046673c` | data only | — | `g_frontendBootDispatchMode==0` | Writes the per-car localized name/engine/spec text tables consumed later by car-select (idx20). Not a visual element of this screen. |
| Config load | `LoadPackedConfigTd5()` 0x0040fb60 | data only | — | boot==0 | reads Config.td5 |
| Joystick binding copy | `JoystickButtonC_exref`/`JoystickC_exref`/`JoystickType_exref` → `DAT_00465684`/`DAT_00465664` (7 slots) | data only | — | boot==0 | |
| Device-desc default / binding reset | `g_player1DeviceDesc=0x403`; if detected≠stored: `g_player1InputSource=0`,`g_player2InputSource=7`, copy `g_defaultControllerBindings`→`g_controllerBindings` (0x12 dwords) | data only | — | boot==0 | |
| Display-mode list | `BuildEnumeratedDisplayModeList()` 0x0040b100 → `gConfiguredDisplayModeOrdinal`/`gSelectedDisplayModeOrdinal`; `FormatDisplayModeOptionStrings()` 0x0041d840; fallback to 640×480×16 (`0x280`/`0x1e0`/`0x10`) then largest <640 16bpp; copy into `g_player2CustomBindings` (0x62 dwords) | data only | — | boot==0 | |

Primed contract globals: `g_frontendBootDispatchMode` (=1 on completion), `g_replayFileAvailable`,
`g_persistedMasterVolumePercent`, `g_persistedCdVolumePercent`, `gConfiguredDisplayModeOrdinal`,
`gSelectedDisplayModeOrdinal`, `g_player1DeviceDesc`, `g_player1/2InputSource`, `g_controllerBindings`,
`g_inputPlaybackActive=0`, the per-car localized string tables, `g_player2CustomBindings` block. It
primes NO draw/flush globals (no overlay rects, no slots, no surfaces) — the flush still runs its
unconditional gallery+highlight passes but this screen primed nothing for them.
Animation: NONE.
Conditional elements: Entire body gated on `g_frontendBootDispatchMode` (0 full-init / 1 fast-path /
2 boot-to-results). Localization parse per-car gated on `GetArchiveEntrySize` in (1..0x200] (else
fill from `s_UNKNOWN`/`SNK_ConfUnknown`). Substitution gated on each field ≠ "UNKNOWN" and first
char ≠ '-'. Volume-requantize block only in boot==2.
Input dispatch: NON-INTERACTIVE. Reads no input bits. Acts only on `g_frontendBootDispatchMode`.
Always exits via SetFrontendScreen (→5 or →24).
Confidence: [CONFIRMED @ 0x004269d0 body; PUSH 0x5 @0x00427173; PUSH 0x18 @0x00427190;
g_frontendBootDispatchMode=[0x004a2c8c] @0x0042717a]. [UNCERTAIN: the exact `SNK_*_exref` LUT
.data addresses are decompiler exref labels, not individually resolved here — unambiguous by usage].

---

### Screen 1 @0x00415030 — ScreenPositionerDebugTool  [interactive Y]
Inner states: `switch(g_frontendInnerState)`:
- **0 (init):** `g_frontendButtonIndex = LoadFrontendTgaSurfaceFromArchive("Front End\Positioner.tga",
  "Front End\FrontEnd.zip")` (the loaded surface ptr is stashed in `g_frontendButtonIndex`, reused
  as a sprite source id later); `ClearBackbufferWithColor(0)`; two reference scanlines
  `FillPrimaryFrontendScanline(0x114, 0xff)` and `(0x10c, 0xff)` (full-width white rows at y=276 / y=268).
  Falls through to 1.
- **1 (present):** `PresentPrimaryFrontendBufferViaCopy()`; `++inner-state`; return.
- **2 (load glyph table):** copies the ASCII ruler `g_largeFontDisplay` chars into
  `g_positionerGlyphRectsPrimary`/`Secondary` (x-offset/y-offset pairs, stride 2 dwords) up to
  `0x495a64`; `g_positionerSelectedGlyphIndex=0`; `++inner-state`.
- **3 (interactive — navigate glyph cursor):** `RenderPositionerGlyphStrip(0)` (primes the glyph
  ruler overlay rects); edge keys move `g_positionerSelectedGlyphIndex` (±1 / ±8). At bottom, the
  shared `& 0x40000` test advances to state 4.
- **4 (interactive — edit selected glyph offsets):** `RenderPositionerGlyphStrip(0)` then
  `QueueFrontendOverlayRect(ret, 0x15c, 0x24,0,0x24,0x24, 0, g_menuFontSurface)` (a 36×36 marker
  glyph at y=348); edge keys nudge the selected glyph's primary (x) / secondary (y) offset ±1; the
  `& 0x40000` test advances to state 5.
- **5 (save):** writes `positioner.txt` (fopen 0x465930) — dumps both 0x25-row glyph-offset tables
  as C-array text via `func_0x004488c8` (fprintf); `g_frontendInnerState=3` (loop back to navigate).
- The shared tail (`if (uVar1 & 0x40000) ++inner-state`) is reached from states 3/4 — input bit
  `0x40000` (CONFIRM/select) steps 3→4→5.

ELEMENTS:
| element | produced by (fn/global/asset) | INLINE/FLUSH | dest pos | gating | notes |
|---|---|---|---|---|---|
| Positioner background TGA | `LoadFrontendTgaSurfaceFromArchive(Positioner.tga, FrontEnd.zip)` → new tracked surface, ptr → `g_frontendButtonIndex` | INLINE load (used as sprite src later) | — | state 0 | NOTE: stored into `g_frontendButtonIndex` then used as the `texid` arg in RenderPositionerGlyphStrip's caret rect |
| Backbuffer clear | `ClearBackbufferWithColor(0)` → `g_primaryWorkSurface` | INLINE | full surface | state 0 | color 0 (black) |
| 2 reference scanlines | `FillPrimaryFrontendScanline(0x114,0xff)` / `(0x10c,0xff)` → `g_primaryWorkSurface` | INLINE | full-width 1px rows @ y=0x114(276)/0x10c(268) | state 0 | white ruler lines |
| Glyph ruler strip (±8 around cursor) | `RenderPositionerGlyphStrip` 0x00414f40 → `QueueFrontendOverlayRect` per glyph: 36×36 (`0x24`) menu-font cells from `g_menuFontSurface`, cell `(idx%7*0x24, idx/7*0x24)`, dest x accumulates `glyph_x_offset+4`, y = `glyph_y_offset+0xf0` | FLUSH (overlay-drain) | scrolling row, base y=0xf0(240) | states 3,4 | the editable glyph ruler |
| Per-glyph caret bar | `RenderPositionerGlyphStrip` → `QueueFrontendOverlayRect(x+glyph_x, 0xdc, 0,0,4,0x28, 0, g_frontendButtonIndex)` | FLUSH | y=0xdc(220), 4×40 | states 3,4 | uses the Positioner.tga surface (in g_frontendButtonIndex) as the caret sprite |
| End-of-strip marker glyph | `RenderPositionerGlyphStrip` final `QueueFrontendOverlayRect(local_4, 0x138, 0x24,0,0x24,0x24,0,g_menuFontSurface)` | FLUSH | y=0x138(312), 36×36 | states 3,4 | shows the selected glyph at the cursor X |
| State-4 selected-glyph marker | `QueueFrontendOverlayRect(ret,0x15c,0x24,0,0x24,0x24,0,g_menuFontSurface)` | FLUSH | y=0x15c(348), 36×36 | state 4 only | second marker row for edit mode |

Primed contract globals: overlay-rect double-buffer (via QueueFrontendOverlayRect — drawn by FLUSH);
`g_frontendButtonIndex` (repurposed as the Positioner.tga sprite-source id); `g_positionerSelectedGlyphIndex`;
`g_positionerGlyphRectsPrimary`/`Secondary` (the edited offset tables). The unconditional FLUSH
gallery + highlight passes also run but this screen sets neither `g_extrasGallerySlideSurfaces` nor
`g_frontendOverlayRectArrayTail` so they no-op.
Animation: NONE counter-driven. The ruler "scrolls" only as `g_positionerSelectedGlyphIndex` changes
(input-driven, not `g_frontendAnimFrameCounter`). No slide-in/out.
Conditional elements: state-4 second marker row only in state 4. Glyph rects with addr `<0x495260`
are skipped (advance x by 0x28 placeholder) — the leading off-table entries draw nothing.
Input dispatch (edge bits `g_frontendInputEdgeBits`):
- bit `0x1` = left: dec index (state 3) / dec selected glyph X-offset (state 4)
- bit `0x2` = right: inc index / inc X-offset
- bit `0x200` = down: index +8 / dec selected glyph Y-offset
- bit `0x400` = up: index −8 / inc selected glyph Y-offset
- bit `0x40000` = confirm: 3→4→5 (and state 5 writes positioner.txt then loops to 3)
No SetFrontendScreen / no ESC button registered — this debug tool has no exit-to-menu path in-body.
Confidence: [CONFIRMED @ 0x00415030 body; RenderPositionerGlyphStrip @0x00414f40].
[UNCERTAIN: `g_largeFontDisplay` / glyph-rect-table base addresses are decompiler exref labels;
the `func_0x004488c8` is the CRT fprintf-family writer (boundary, not resolved by name)].

---

### Screen 2 @0x004275a0 — RunAttractModeDemoScreen  [interactive N]
Inner states: `switch(g_frontendInnerState)` (jump table @0x00427614, cases 0..5):
- **0:** `g_attractModeDemoActive = 1` (`[0x00495254]=1` — CONFIRMED @0x004275b1); `PresentPrimaryFrontendBuffer()`
  (primary→back BltFast); `ActivateFrontendCursorOverlay()` (sets highlight-active=1); `++inner-state`.
- **1:** `ReleaseFrontendDisplayModeButtons()` (tears down any leftover menu buttons); `++inner-state`.
- **2,3:** `PresentPrimaryFrontendBufferViaCopy()`; `++inner-state` (2-frame settle double-present).
- **4:** `InitFrontendFadeColor(0)` (arm fade-to-black ramp); `++inner-state`.
- **5:** `RenderFrontendFadeEffect()` each frame (the 64-row dither wipe band); when
  `g_frontendFadeActive==0` (fade complete): `InitializeRaceSeriesSchedule()` +
  `InitializeFrontendDisplayModeState()` — the latter releases all surfaces, sets
  `g_startRaceConfirmFlag=1`, picks the attract CD/race track, and the LOOP early-returns to launch
  the in-engine attract demo race (`g_attractModeTrackIndex` was set by the LOOP before entry).

ELEMENTS:
| element | produced by (fn/global/asset) | INLINE/FLUSH | dest pos | gating | notes |
|---|---|---|---|---|---|
| Present current primary | `PresentPrimaryFrontendBuffer` 0x00424ca0 (state 0) / `PresentPrimaryFrontendBufferViaCopy` 0x00424af0 (states 2,3) | INLINE present | full | states 0,2,3 | shows whatever was already in `g_primaryWorkSurface` (the main-menu frame it transitioned from); this screen draws NO new content |
| Fade-to-black wipe | `InitFrontendFadeColor(0)` 0x00411750 (state 4) + `RenderFrontendFadeEffect` 0x00411780 (state 5) | INLINE | 64-row dither band scanning top→bottom 2 rows/frame → `g_primaryWorkSurface` | states 4,5 | fade-out before the attract race launches |

Primed contract globals: `g_attractModeDemoActive=1`; fade state (`g_frontendFadeScanCursor`,
`g_frontendFadeActive`, `_g_frontendFadeChannelR/G/B`); via InitializeFrontendDisplayModeState →
`g_startRaceConfirmFlag=1`, `g_selectedCdTrackIndex`, releases tracked surfaces. Sets NO overlay
rects / slots; the unconditional FLUSH gallery+highlight passes run but no-op (nothing primed).
Animation: the fade (state 5) is the only animation — driven by the fade scan-cursor (`+=2`/frame),
NOT `g_frontendAnimFrameCounter`. No slide-in/out, no header label.
Conditional elements: transition out of state 5 gated on `g_frontendFadeActive==0`. (The attract
demo screen itself is entered by the LOOP idle-timer @ step 17, 50s on main menu; not from a button.)
Input dispatch: NON-INTERACTIVE. Reads NO input. The actual demo race (after this screen) is
driven by the engine/replay, not the frontend. Exit is automatic on fade completion → race launch.
Confidence: [CONFIRMED @ 0x004275a0 body; g_attractModeDemoActive=[0x00495254] @0x004275b1;
jump table @0x00427614].

---

### Screen 3 @0x00427290 — ScreenLanguageSelect  [interactive Y]
Inner states: `switch(g_frontendInnerState)` (cases 0..6). Layout math: `uVar3=H/5`,
`uVar1=((H-H/5)>>1 - 0x80)>>1`, `uVar2=((W>>1)-0xb0)>>1`.
- **0 (init):** load flag-sheet `g_frontendLanguageFlagsSurface_PROVISIONAL =
  LoadFrontendTgaSurfaceFromArchive("Front End\Language.tga", FrontEnd.zip)`; load full-screen bg
  `LoadTgaToFrontendSurface16bpp("Front End\LanguageScreen.tga", FrontEnd.zip)` → `g_primaryWorkSurface`;
  draw header `DrawFrontendLocalizedStringPrimary("LANGUAGE SELECT"@0x4667c0, uVar2, (H/5-0x1c)>>1)`;
  `CopyPrimaryFrontendBufferToSecondary()`; build FOUR menu rect entries (the 4 flag buttons, each
  0xb0×0x80=176×128, sourcing different rows of the flag sheet via src-y 0/0x80/0x100/0x180); set
  `g_languageSelectButtonHoverIndex_PROVISIONAL=0`; `++inner-state`.
- **1,2 (present/settle):** `AdvanceFrontendTickAndCheckReady()` (3-frame gate);
  `PresentPrimaryFrontendBufferViaCopy()`; `DeactivateFrontendCursorOverlay()` (highlight off during
  hover-pick); `g_frontendAnimFrameCounter=0`; `++inner-state`.
- **3,4,5 (interactive — fall to shared tail):** state 4 is the live pick state (see Input).
  3 and 5 just step (++inner-state via shared tail).
- **6 (exit):** `PresentPrimaryFrontendBuffer()`; `ReleaseFrontendDisplayModeButtons()`;
  release the flag-sheet surface; `SetFrontendScreen(4=LEGAL_COPYRIGHT)` (PUSH 0x4 @0x00427474 — CONFIRMED).

ELEMENTS:
| element | produced by (fn/global/asset) | INLINE/FLUSH | dest pos | gating | notes |
|---|---|---|---|---|---|
| Full-screen background | `LoadTgaToFrontendSurface16bpp(LanguageScreen.tga)` → `g_primaryWorkSurface` | INLINE | full canvas | state 0 | decoded straight into work surface |
| Header "LANGUAGE SELECT" | `DrawFrontendLocalizedStringPrimary(@0x4667c0)` → `g_primaryWorkSurface` (or secondary 24×24 if LANG.DLL) | INLINE | x=uVar2, y=(H/5-0x1c)>>1 | state 0 | localized; baked into bg, copied to secondary |
| Flag button 0 (top-left) | `CreateFrontendMenuRectEntry(uVar2, H/5+uVar1, 0xb0,0x80, src 0,0, surf=flagsheet)` → slot in `DAT_00498f64` static-rect array | INLINE slot-build → FLUSH draw (RenderFrontendUiRects→QueueFrontendSpriteBlit→flush sprite loop) | (uVar2, H/5+uVar1) 176×128 | state 0 | src-y 0 = language 0 flag |
| Flag button 1 (top-right) | `CreateFrontendMenuRectEntry(W-uVar2-0xb0, H/5+uVar1, 0xb0,0x80, src 0,0x80, flagsheet)` | INLINE→FLUSH | (W-uVar2-0xb0, H/5+uVar1) | state 0 | src-y 0x80(128) = language 1 |
| Flag button 2 (bottom-left) | `CreateFrontendMenuRectEntry(uVar2, H-uVar1-0x80, 0xb0,0x80, src 0,0x100, flagsheet)` | INLINE→FLUSH | (uVar2, H-uVar1-0x80) | state 0 | src-y 0x100(256) = language 2 |
| Flag button 3 (bottom-right) | `CreateFrontendMenuRectEntry(W-uVar2-0xb0, H-uVar1-0x80, 0xb0,0x80, src 0,0x180, flagsheet)` | INLINE→FLUSH | (W-uVar2-0xb0, H-uVar1-0x80) | state 0 | src-y 0x180(384) = language 3 |
| Selection highlight bars | `RenderFrontendDisplayModeHighlight` 0x004263e0 (4 edge bars 0xc000 around selected slot) | FLUSH (decoupled) | around hovered flag rect | when cursor-overlay active & a rect selected | the hover outline on the flag buttons |
| Mouse cursor sprite | LOOP queues `g_frontendCursorTextureId` overlay rect | FLUSH | mouse x/y, 22×30 | `g_frontendMouseCursorEnabled==1` | standard cursor |

Primed contract globals: `DAT_00498f64` static-rect slot array (4 flag rects → consumed by
RenderFrontendUiRects → FLUSH); `g_frontendLanguageFlagsSurface_PROVISIONAL` (flag-sheet surface);
`g_languageSelectButtonHoverIndex_PROVISIONAL` (=[0x00496290], hover tracking only);
`g_frontendAnimFrameCounter` (reset for hover-change detection). NOT `g_frontendEscKeyButtonIndex`.
Animation: NO slide-in/out. `g_frontendAnimFrameCounter` is reset to 0 whenever the hovered button
changes (`g_frontendButtonIndex != hover`) — used only to time the highlight/selection animation in
`UpdateFrontendDisplayModeSelection`, not a panel slide.
Conditional elements: in state 4, `if (g_frontendButtonIndex != hover) g_frontendAnimFrameCounter=0`;
the press-commit path (`ActivateFrontendCursorOverlay` + advance) only when `g_frontendButtonPressedFlag != 0`.
Input dispatch (state 4):
- Hover: `g_languageSelectButtonHoverIndex_PROVISIONAL = g_frontendButtonIndex` every frame.
- Press (`g_frontendButtonPressedFlag != 0`): `ActivateFrontendCursorOverlay()`, advance inner-state
  (→5→6 → exit to LEGAL_COPYRIGHT).
- **KEY FINDING:** the selected flag/button index is NEVER written to any language/locale config
  global in this function. Pressing ANY of the 4 flags simply advances to the exit state; all four
  lead to `SetFrontendScreen(4)`. The flag choice is non-committal here (language is governed by the
  pre-loaded LANGUAGE.DLL / `SNK_LangDLL` config, set elsewhere). [CONFIRMED @0x00427419-0x0042744e:
  the only stores are to `g_languageSelectButtonHoverIndex_PROVISIONAL`[0x00496290] and
  `g_frontendAnimFrameCounter`[0x0049522c]; no locale write.]
Confidence: [CONFIRMED @ 0x00427290 body; PUSH 0x4 @0x00427474; case-4 stores @0x0042741f-0x00427449;
strings @0x4667c0 "LANGUAGE SELECT" / @0x4667d0 "Front End\LanguageScreen.tga"].
[UNCERTAIN: the LANGUAGE.DLL gate inside DrawFrontendLocalizedStringPrimary (24×24 vs 12×12) is
runtime — header font size depends on `SNK_LangDLL_exref[8]==0x30`, not asserted for a given build].

---

### Screen 4 @0x004274a0 — ScreenLegalCopyright  [interactive N]
Inner states: `switch(g_frontendInnerState)` (cases 0..3):
- **0 (init):** load full-screen `LoadTgaToFrontendSurface16bppVariant("Front End\LegalScreen.tga",
  FrontEnd.zip)` → `g_secondaryWorkSurface`; tile the copyright string down the canvas: loop
  `i=1..(H-0x20)>>5` calling `DrawFrontendLocalizedStringSecondary("TEST DRIVE 5 COPYRIGHT 1998"
  @0x466808, x=W/10, y=i*0x20)` (32-px row step, into `g_secondaryWorkSurface` — CONFIRMED
  @0x004274f3 PUSH 0x466808, @0x004274ef y=i*0x20); `ResetFrontendFadeState()` (arm fade-to-black);
  `++inner-state`.
- **1 (fade-out cross):** `RenderFrontendFadeOutEffect()` each frame; `g_frontendAnimFrameCounter=0`;
  when `g_frontendFadeActive==0` → stamp `g_frontendInnerState=timeGetTime()` then `+1` (dwell timer).
- **2 (dwell 3s):** `if (timeGetTime() - g_frontendInnerState > 2999)` → `InitFrontendFadeColor(0)`
  (arm fade-IN/to-color), `++inner-state`.
- **3 (fade-in):** `RenderFrontendFadeEffect()` each frame; when `g_frontendFadeActive==0` →
  `SetFrontendScreen(5=MAIN_MENU)` (PUSH 0x5 — the main-menu index, exit).

ELEMENTS:
| element | produced by (fn/global/asset) | INLINE/FLUSH | dest pos | gating | notes |
|---|---|---|---|---|---|
| Legal background TGA | `LoadTgaToFrontendSurface16bppVariant(LegalScreen.tga)` → `g_secondaryWorkSurface` | INLINE | full canvas (secondary) | state 0 | decoded into the SECONDARY work surface |
| Tiled "TEST DRIVE 5 COPYRIGHT 1998" | `DrawFrontendLocalizedStringSecondary(@0x466808)` loop → `g_secondaryWorkSurface` | INLINE | x=W/10(64), y=32,64,96,… (step 0x20) repeated `(H-0x20)>>5`≈14 rows | state 0 | tiled vertically as a watermark; localized (24×24 if LANG.DLL, else body 12×12) |
| Fade-OUT cross (in) | `ResetFrontendFadeState` 0x00411a50 (state 0) + `RenderFrontendFadeOutEffect` 0x00411a70 (state 1) | INLINE | 64-row dither band → `g_primaryWorkSurface` blending toward secondary | states 0,1 | reveals the legal screen by cross-fading primary→secondary |
| Fade-IN to black (out) | `InitFrontendFadeColor(0)` (state 2) + `RenderFrontendFadeEffect` 0x00411780 (state 3) | INLINE | 64-row dither band → `g_primaryWorkSurface` | states 2,3 | fades to black before main menu |

Primed contract globals: fade state (`g_frontendFadeScanCursor`, `g_frontendFadeActive`,
`_g_frontendFadeChannelR/G/B`); `g_frontendInnerState` reused as a `timeGetTime()` dwell timestamp in
states 1→2. Sets NO overlay rects / slots / button slots. Unconditional FLUSH gallery+highlight run
but no-op.
Animation: two fades (out-cross then in-to-black), each driven by the fade scan-cursor (`+=2`/frame),
NOT `g_frontendAnimFrameCounter` (that is reset to 0 each frame in state 1, unused for motion here).
Plus a real-time **3000 ms dwell** (`>2999`) between the two fades in state 2.
Conditional elements: state-1→2 gated on `g_frontendFadeActive==0`; state-2→3 gated on
`timeGetTime()-stamp > 2999`; state-3→exit gated on `g_frontendFadeActive==0`. The tile-loop body
only runs if `(H-0x20)&0xffffffe0 > 0x20`.
Input dispatch: NON-INTERACTIVE. Reads NO input (no ESC button registered). Fully timer/fade-driven;
exits automatically to MAIN_MENU (idx5).
Confidence: [CONFIRMED @ 0x004274a0 body; string @0x466808 "TEST DRIVE 5 COPYRIGHT 1998";
tile y=i*0x20 @0x004274ef, x=W/10 @0x004274e8-ee; dwell 2999 @ case 2; LegalScreen.tga →
LoadTgaToFrontendSurface16bppVariant=secondary @0x004274c0].
# TD5 Frontend Screens 5/6/7/8/9 — Complete Element + Behavior Spec

RE target: `TD5_d3d.exe` (image base 0x00400000). All addresses are Ghidra VAs, hex AND decimal.
Source: literal Ghidra decompilation of each screen-fn body + the verified flush/render model in
`frontend_rendering_model.md` / `frontend_flow_model.md` / `frontend_call_graph_closure.md`.

## Cross-screen mechanics (apply to all 5 — grounded, not re-derived per screen)

- **INLINE** = the screen-fn writes pixels directly into a work/secondary surface or bakes them
  into a tracked button/label surface this frame (fills, TGA decode, label-surface text bakes,
  `BltColorFillToSurface` into a panel surface, `DrawFrontend*StringToSurface`).
- **FLUSH** = the screen-fn only PRIMES (queues an overlay rect via `QueueFrontendOverlayRect`
  0x425660, builds a button slot via `CreateFrontendDisplayModeButton` 0x425de0, or moves a slot
  via `MoveFrontendSpriteRect` 0x4259d0); the actual on-screen pixels come from the per-frame
  `FlushFrontendSpriteBlits` 0x425540 + `RenderFrontendUiRects` 0x425a30 (button-slot walk) +
  the two unconditional decoupled draws `UpdateExtrasGalleryDisplay` 0x40d830 and
  `RenderFrontendDisplayModeHighlight` 0x4263e0.
- **DECOUPLED draws that run over EVERY one of these 5 screens** (no screen-fn calls them):
  1. **Selection/hover highlight** `RenderFrontendDisplayModeHighlight` 0x4263e0 — 4 edge bars
     color 0xc000 (49152) around the slot indexed by `g_frontendOverlayRectArrayTail` in
     `g_connBrowserListOriginX_PROVISIONAL[]` (0x00499c78, FrontendButtonSlot stride 0x34=52),
     drawn into `g_frontendBackSurfacePtr`. Gate: tail!=-1 AND `g_frontendCursorOverlayHidden`==0.
  2. **Extras gallery cover-art** `UpdateExtrasGalleryDisplay` 0x40d830 — no-op on these 5 unless
     `g_extrasGallerySlideSurfaces`!=0 and `g_frontendScreenTransitionFlag`==2 (band gallery) /
     extras idle; all 5 screens set transitionFlag=0 in state 0, so the gallery is inert here.
  3. **Mouse cursor sprite** — LOOP queues `QueueFrontendOverlayRect(mouseX,mouseY,0,0,0x16,0x1e,
     0xff0000,g_frontendCursorTextureId)` when `g_frontendCursorOverlayHidden`==0 &&
     `g_frontendMouseCursorEnabled`==1 (RunFrontendDisplayLoop 0x414b50 step 9).
- **Selected-button vertical offset**: `RenderFrontendUiRects` 0x425a30 offsets the src-rect of the
  slot == `g_frontendButtonIndex` by its bake height (highlight = top half of the h*2 baked surface).
- **Button-slot table** `g_connBrowserListOriginX_PROVISIONAL` @0x00499c78. Each button created by
  `CreateFrontendDisplayModeButton(label,x,y,w,h,user_data)` allocates a tracked surface of
  (w, h*2) baking BOTH a highlighted (top, state 1) and idle (bottom, state 0) frame+label via
  DrawFrontendButtonBackground 0x425b60 (9-slice from ButtonBits page DAT_00496268) +
  DrawFrontendLocalizedStringToSurface. x<0 means "auto-stack" (off-screen seed; the slide states
  move them in). **The on-screen button pixels are FLUSH-drawn** by RenderFrontendUiRects.
- **Cup-tier gating global** `g_cheatFlagBitfieldGameModes` @0x004962a8 (cheat bitfield &7).
- **Selection nav globals** (LOOP-written, screen-read): `g_frontendButtonIndex`,
  `g_frontendButtonPressedFlag`, `g_postRaceRacerCardNavDirection` (◄/► delta),
  `g_frontendEscKeyButtonIndex` (ESC→button), `g_frontendInputEdgeBits` (kb/stick rising edges;
  bit 0x200=up, 0x400=down for list browsers).
- **g_frontendAnimFrameCounter** (LOOP `++` each frame): drives every slide; transitions fire on
  exact equality. Animation speed = frame rate (no real-time interp).

---

### Screen 5 @0x00415490 — ScreenMainMenuAnd1PRaceFlow  [interactive Y]

Inner states (in-body `switch(g_frontendInnerState)`, NOT a jump table): 0 init/build,
1+2 present/settle, 3 slide-in (anim 0..0x27=39), 4 MAIN-MENU interactive (7-button dispatch),
5 EXIT-confirm-dialog settle, 6 EXIT-confirm interactive (Yes/No), 7 cancel-dialog teardown→4,
8 generic slide-out-cover→9, 9 slide-out + post-select device-check → SetFrontendScreen, 10 EXIT
"yes" slide-out commit→0xb, 0xb release/restore→0xc, 0xc final slide-out → SetFrontendScreen(EXTRAS
GALLERY idx22), 0x14 controller-required dialog build, 0x15 dialog slide-in (bakes the "MUST
SELECT" + per-mode message text), 0x16 dialog interactive (any press dismisses → 0x14 rebuild),
0x17 dialog slide-out → SetFrontendScreen(CONTROL OPTIONS idx14). (Confirmed addrs: state bodies
inline at 0x415490..0x416822.)

ELEMENTS:
| element | produced by (fn/global/asset) | INLINE/FLUSH | dest pos | gating | notes |
|---|---|---|---|---|---|
| Full-screen background | LoadTgaToFrontendSurface16bpp(MainMenu.tga, FrontEnd.zip) @state0; CopyPrimaryFrontendBufferToSecondary | INLINE | (0,0) 640×480 g_primaryWorkSurface | state 0 | s_Front_End_MainMenu_tga_00463ecc |
| Header label "MAIN MENU" | CreateMenuStringLabelSurface(0)→g_currentScreenIndex; QueueFrontendOverlayRect each frame at anim-driven X | FLUSH | (cx-200, …) header band | states 3-0xc | menu-font label surface |
| 7 menu buttons: Race, QuickRace, TimeDemo/2Player, NetPlay, Options, HiScore, Exit | CreateFrontendDisplayModeButton(label,-0xe0,0,0xe0,0x20,0) ×7 @state0 → slots 0..6 | FLUSH | x=-0xe0 seed; slid in by MoveFrontendSpriteRect(0..6) | state 0 build | label SNK_RaceMenuButTxt/QuickRaceButTxt/(TimeDemoButTxt if g_appExref+0x170==0 else TwoPlayerButTxt)/NetPlayButTxt/OptionsButTxt/HiScoreButTxt/ExitButTxt |
| Selection highlight (4 edge bars) | RenderFrontendDisplayModeHighlight 0x4263e0 | FLUSH (decoupled) | around selected slot | tail!=-1 && cursorOverlayHidden==0 | color 0xc000 |
| Mouse cursor | LOOP QueueFrontendOverlayRect(cursor) | FLUSH (decoupled) | mouse XY | cursorOverlayHidden==0 && mouseEnabled | snkmouse, 22×30 |
| EXIT confirm dialog label | CreateMenuStringLabelSurface(8)→g_oneRaceFlowSelectedCarId; QueueFrontendOverlayRect | FLUSH | (cx-200, …+0x124) | states 5/6/10 | "Are you sure" prompt |
| EXIT Yes / No buttons | CreateFrontendDisplayModeButton(SNK_YesButTxt,cx-0xc2,cy+0xa5,0x60,0x20) + (SNK_NoxButTxt,cx-0x42,cy+0xa5,0x60,0x20) @state6-from-4 | FLUSH | cy+0xa5 row | btn6 pressed on state 4 | preview layout snapshot via BeginFrontendDisplayModePreviewLayout |
| Controller-required dialog panel | CreateTrackedFrontendSurface(0x1e2,0x40)→g_lobbyErrorDialogSurface; BltColorFillToSurface clears | INLINE (panel) then FLUSH (queued) | (cx-0xc2, (H-0x6c)/2) 0x1e2×0x40 | state 0x14/0x15/0x16 | dialog backing |
| Controller-required dialog text | MeasureOrCenterFrontendLocalizedString(SNK_MustSelectTxt)+ (g_frontendMustSelectMessage @0x004962d8) → DrawFrontendLocalizedStringToSurface | INLINE bake | into dialog panel | state 0x15 anim==0x10 | message = MustPlayer1Txt or MustPlayer2Txt by mode |
| Controller dialog OK button | CreateFrontendDisplayModeButton(SNK_OkButTxt,0,-0x60,0x60,0x20) @state0x14 | FLUSH | -0x60 seed, slid in | state 0x14 | esc btn implicit |

Primed contract globals: `g_currentScreenIndex` (header label surf), `g_oneRaceFlowSelectedCarId`
(confirm/exit dialog label surf), `g_lobbyErrorDialogSurface` (0x1e2×0x40 controller dialog panel),
`g_menuHeaderLabelSurfaceWidth/Height`, `g_menuHeaderLabelYOffset`, `g_frontendEscKeyButtonIndex`
(=6 main menu / =5 after cancel), `g_returnToScreenIndex` + `g_mainMenuButtonHint_PROVISIONAL`
(1..6 deferred target for the post-slide device check), `g_frontendMustSelectMessage_PROVISIONAL`
(@0x004962d8), button slots 0..6 in g_connBrowserListOriginX[]. Also writes race-config shadows
on state-0 second pass (gFog/units/camera/difficulty/laps → live globals) and DXSound volumes.

Animation: states 3/9/0xc/0x15/0x17 slide buttons via MoveFrontendSpriteRect(slot,x(anim),y(anim))
+ re-queue header at anim*±0x18/0x10. State 3 commits at anim==0x27(39) (held to 0x26 + 3-frame
AdvanceFrontendTickAndCheckReady gate). State 9/0xc commit at anim==0x10(16). State 0x15 dialog
slide commits at anim==0x10. State 0x17 dialog slide-out commits at anim==0x18(24). Slide-in uses
SoundEffect 4 (settle), exits use SoundEffect 5.

Conditional elements:
- **Button 2 label**: `*(g_appExref+0x170)==0` → "Two Player" (TwoPlayerButTxt); else "Time Demo"
  (TimeDemoButTxt = benchmark). (state 0, @0x415d3a.)
- **Button 2 action**: if g_appExref+0x170!=0 → benchmark path (g_benchmarkRequestPending=1,
  InitializeRaceSeriesSchedule + InitializeFrontendDisplayModeState, early return to race); else
  2-player car-select (g_twoPlayerModeEnabled=1, gameType=0). (state 4 case 2.)
- **Controller-required dialog** (states 0x14-0x17): entered from state 9 only when the deferred
  hint requires a device that is unset — hint 1/2/4 check g_player1InputSource==7 (=none) →
  state 0x14; hint 3 (2-player) checks player1 then player2 ==7. If a device is missing the flow
  diverts to the "MUST SELECT PLAYER 1/2 CONTROLLER" dialog then to CONTROL OPTIONS (idx14) instead
  of the requested screen.
- **NetPlay (button 3)**: state 4 case 3 jumps straight to a wipe (state 8) without the device
  check; target = CONNECTION_BROWSER (idx8).

Input dispatch (state 4, `g_frontendButtonPressedFlag` + `g_frontendButtonIndex`):
btn0→returnIndex=RACE_TYPE_MENU(6), hint=1 → wipe state8.
btn1→QUICK_RACE(7), hint=2 → wipe state8.
btn2→(2P) CAR_SELECTION(20) hint3 / (benchmark) early race-start.
btn3→CONNECTION_BROWSER(8), hint4 → wipe state8.
btn4→OPTIONS_HUB(12), hint5 → wipe state8.
btn5→HIGH_SCORE(23), hint6 → wipe state8.
btn6→Exit-confirm dialog (build Yes/No, state→5). ESC (esc-idx 6) = Exit.
State 6 confirm: btn0(Yes)→state10 exit-slide→0xb→0xc→SetFrontendScreen(EXTRAS_GALLERY 22);
btn1(No)→restore layout→state7→state4. State 9 then resolves hint→SetFrontendScreen(returnIndex)
or controller dialog. State 0x16: any press dismisses the dialog → rebuild state 0x14.

Confidence: [CONFIRMED @0x415490 full body; flush/decoupled model @0x425540/0x4263e0/0x425a30;
g_cheatFlagBitfieldGameModes @0x004962a8; g_frontendMustSelectMessage @0x004962d8].
[UNCERTAIN: `g_appExref+0x170` is the benchmark/2-player capability flag by usage; exact field name
not resolved. The "3D background" mentioned in the harvest brief does NOT exist — the background is
the static MainMenu.tga (no live 3D scene in this screen-fn).]

---

### Screen 6 @0x004168b0 — RaceTypeCategoryMenuStateMachine  [interactive Y]

Inner states: pointer-dispatched via JMP [EAX*4 + 0x00417cd4] @0x004168e2 (12 entries, raw bytes
confirmed: 0x4168e9,0x416a81,0x416beb,0x416c4b,0x416d80,0x416f16,0x41707a,0x4173b1,0x4174ce,
0x4175bc,0x417700,0x417918 = states 0..11). PLUS an in-body re-entry state 0x14 (20) tail
(g_frontendInnerState=1, anim=-1) reached when the flow loops back. State roles:
0 build TOP menu, 1 slide-in (anim→0x20=32), 2 settle (3-frame gate), 3 TOP interactive +
hover-change detect, 4 hover-description rebake → back to 3, 5 slide-out → SetFrontendScreen(return),
6 TOP→CUP slide-out + build CUP-TIER menu (gated), 7 CUP slide-in (anim→0x20), 8 CUP interactive +
hover detect, 9 CUP hover-description rebake → back to 8, 10 CUP→exit slide-out →
SetFrontendScreen(return), 0xb CUP→TOP slide-out + rebuild TOP menu (Back), 0xc TOP re-slide-in→3.

ELEMENTS:
| element | produced by (fn/global/asset) | INLINE/FLUSH | dest pos | gating | notes |
|---|---|---|---|---|---|
| Full-screen background | LoadTgaToFrontendSurface16bpp(MainMenu.tga); CopyPrimaryFrontendBufferToSecondary | INLINE | (0,0) 640×480 | state 0 | |
| Header label "RACE MENU" | CreateMenuStringLabelSurface(1)→g_currentScreenIndex; QueueFrontendOverlayRect anim X | FLUSH | header band | all states | |
| Hover-description panel | CreateTrackedFrontendSurface(0x110,0xb4)→g_lobbyErrorDialogSurface; BltColorFillToSurface clears; DrawFrontendLocalizedStringToSurface (title) + DrawFrontendSmallFontStringToSurface (wrapped body 0x20..0xaf step 0xc) | INLINE bake | (cx+0x26, cy-0x5f) 0x110×0xb4 | states 1/3/4/5/6/7/8/9/0xb/0xc | text = SNK_RaceTypeText[gameType] |
| TOP menu 7 buttons: SingleRace, CupRace, ContCup, TimeTrials, DragRace, CopChase, Back | CreateFrontendDisplayModeButton(label,-0xe0,0,0xe0,0x20) ×6 + Back(-0xe0,0,0x70,0x20) @state0; ContCup = PreviewButton if ValidateCupDataChecksum==0 | FLUSH | slid in | state 0 | slots 0..6; Back narrower (0x70) |
| CUP-TIER 7 buttons: Championship, Era, Challenge, Pitbull, Masters, Ultimate, Back | CreateFrontendDisplayModeButton / PreviewButton ×6 + Back @state6 | FLUSH | slid in | state 6 (built at anim==0x23) | preview-greyed by cheat tier (see conditional) |
| Selection highlight | RenderFrontendDisplayModeHighlight 0x4263e0 | FLUSH (decoupled) | selected slot | tail!=-1 && overlay active | |
| Mouse cursor | LOOP queue | FLUSH (decoupled) | mouse XY | | |

Primed contract globals: `g_currentScreenIndex` (header), `g_lobbyErrorDialogSurface` (0x110×0xb4
hover-desc panel), `g_previousButtonIndex` (hover-change latch, -1 init), `g_frontendSelectedGameType`
(written per button — see dispatch), `g_returnToScreenIndex`, `g_frontendEscKeyButtonIndex`=6,
`g_raceWithinSeriesIndex`, `g_selectedScheduleIndex`=g_attractModeTrackIndex (cup path),
`g_cheatFlagBitfieldGameModes` (cup-tier preview gating). ConfigureGameTypeFlags 0x410ca0 invoked
on commit (drives the cup-schedule configurator data-table @0x00464108).

Animation: state 1/7 slide-in 6 (TOP) or 7 (with Back) sprite slots by anim-derived Y, commit
anim==0x20(32)+SoundEffect 4. State 5/6/0xb/0xc cross-slide commit anim==0x23(35). Hover-change
states 4/9 are a single-frame rebake (BltColorFillToSurface the panel, re-draw the new gameType
description) then return to the interactive state. SoundEffect 5 on exits, 4 on settle.

Conditional elements:
- **"Continue Cup" (button 2)** is a *preview* (greyed/disabled) button when
  `ValidateCupDataChecksum()`==0 (no valid CupData.td5 save); a normal selectable button otherwise.
  (state 0 @0x416a1e and state 0xb rebuild.)
- **Cup-tier buttons gated by `g_cheatFlagBitfieldGameModes` @0x004962a8** (state 6 @0x41736b):
  - ==0: Championship+Era selectable; **Challenge/Pitbull/Masters/Ultimate are PreviewButtons**
    (locked/greyed).
  - ==1: Championship+Era+Challenge+Pitbull selectable; Masters/Ultimate Preview (locked).
  - else (>=2): ALL six tiers selectable (full unlock).
- Hover-description text is per-selected-gameType from SNK_RaceTypeText[gameType*4] (state 4/9).

Input dispatch:
- State 3 (TOP): hover change (btnIndex != previousIndex) → state 4 rebake. On press:
  btn0→gameType=0 (Single); btn3→9 (Drag); btn4→7? — actually state-3 mapping: btn0→0,
  btn{3,4,5}→btnIndex+3 (btn3 special→9), then ConfigureGameTypeFlags, return=CAR_SELECTION(20),
  →state5. btn1(CupRace)→state6 (open cup tiers). btn2(ContCup)→LoadContinueCupData,
  g_replayFileAvailable=0, return=RACE_RESULTS(24), →state5. btn6(Back)→return computed
  (CREATE_SESSION vs offset by g_networkLobbyEntryPhase) →state5. ESC=btn6.
- State 4 maps the *display* gameType for the description panel: btn0→0, btn4→7, btn5→8, btn3→9,
  btn1→10, btn2→0xb (these are the SNK_RaceTypeText label indices), then →state3.
- State 8 (CUP): hover change→state9. On press btn0..5 → gameType=btnIndex+1,
  return=CAR_SELECTION(20), raceWithinSeries=0, ConfigureGameTypeFlags, schedule=attract track,
  →state10. btn6(Back)→state0xb (back to TOP). ESC=btn6.
- State 9 rebakes the cup-tier description (gameType=btnIndex+1) →state8.

Confidence: [CONFIRMED @0x004168b0 full body; jump table raw bytes @0x00417cd4;
g_cheatFlagBitfieldGameModes @0x004962a8; ConfigureGameTypeFlags 0x410ca0 + cup table 0x00464108
per flow_model §4B]. [UNCERTAIN: state-3 non-Back gameType arithmetic (`btnIndex+3`, btn3→9) yields
{0,?,?,9,7,8} via the state-4 normalizer; the exact SNK_RaceTypeText index per physical button is
the state-4 remap, decompiled literally above.]

---

### Screen 7 @0x004213d0 — ScreenQuickRaceMenu  [interactive Y]

Inner states (in-body switch): 0 init/build, 1+2 present/settle, 3 slide-in (anim→0x27=39),
4 INTERACTIVE (◄► car/track cycling + OK/Back), 5 exit settle, 6 slide-out → SetFrontendScreen /
InitializeRaceSeriesSchedule. (Confirmed @0x4213d0..0x421d76.)

ELEMENTS:
| element | produced by (fn/global/asset) | INLINE/FLUSH | dest pos | gating | notes |
|---|---|---|---|---|---|
| Full-screen background | LoadTgaToFrontendSurface16bpp(MainMenu.tga); CopyPrimaryFrontendBufferToSecondary | INLINE | (0,0) 640×480 | state 0 | |
| Header label "QUICK RACE" | CreateMenuStringLabelSurface(3)→g_currentScreenIndex; QueueFrontendOverlayRect anim X | FLUSH | header band | all | |
| Car+Track info panel | CreateTrackedFrontendSurface(0x208,200)→g_lobbyErrorDialogSurface; BltColorFillToSurface clears; sprintf_game(&g_quickRaceMenuFormatPrefix)+DrawFrontendLocalizedStringToSurface (car name top half, track name bottom half) | INLINE bake | (cx-200, cy-0x8f) 0x208×200 | states 3/4/5/6 | re-baked on each ◄► cycle (state 4) |
| "locked" overlay text | DrawFrontendLocalizedStringToSurface when (&g_savedCarLockTable)[carId]!=0 / (&g_packedConfig_displayPrefsBlock)[track]!=0 AND cheat off | INLINE bake | into panel | car/track locked & !cheat | shows lock message |
| Button 0 "CHANGE CAR" + ◄► arrows | CreateFrontendDisplayModeButton(SNK_ChangeCarButTxt,cx-200,cy-0x67,0x100,0x20); InitializeFrontendDisplayModeArrows(0,1) | FLUSH | cy-0x67 | state 0 | arrows set slot flag|=2 (two-axis) |
| Button 1 "CHANGE TRACK" + ◄► arrows | CreateFrontendDisplayModeButton(SNK_ChangeTrackButTxt,cx-200,cy+0x11,0x100,0x20); InitializeFrontendDisplayModeArrows(1,1) | FLUSH | cy+0x11 | state 0 | arrows |
| Button 2 "OK" | CreateFrontendDisplayModeButton(SNK_OkButTxt,cx-200,cy+0x89,0x60,0x20) | FLUSH | cy+0x89 | state 0 | |
| Button 3 "BACK" | CreateFrontendDisplayModeButton(SNK_BackButTxt,cx-0x58,cy+0x89,0x70,0x20) | FLUSH | cy+0x89 | state 0 | ESC=3 |
| Selection highlight | RenderFrontendDisplayModeHighlight 0x4263e0 | FLUSH (decoupled) | selected slot | active | |
| Mouse cursor | LOOP queue | FLUSH (decoupled) | mouse XY | | |

Primed contract globals: `g_currentScreenIndex` (header), `g_lobbyErrorDialogSurface` (0x208×200
car/track panel), `g_frontendEscKeyButtonIndex`=3, `g_quickRaceSelectedTrackId` (the car id, ◄►
cycled), `g_selectedScheduleIndex` (the track id, ◄► cycled), `g_selectedCarIndex` /
`g_player1SelectedPaintScheme=0` / `g_player1SelectedWheelScheme=0` (committed on OK/Back),
`g_returnToScreenIndex`, `g_frontendSelectedGameType=-1`. Arrows set slot flags|=2 via
InitializeFrontendDisplayModeArrows 0x426260 (◄► sprite from g_browserSelectionBarSprite).

Animation: state 3 slide-in 4 button slots + header + panel, commit anim==0x27 (held 0x26 +
3-frame gate). State 6 slide-out, commit anim==0x10(16) → release surfaces → SetFrontendScreen.

Conditional elements:
- **Unlock clamping (state 0)**: `iVar4` = unlock ceiling = `g_savedMaxUnlockedCar` normally; =0x20
  (32) when `DAT_00463e6d`!=0 (all-cars cheat partial); car/track ids clamped to it. With
  `g_cheatPostRaceHighScoreUnlock` set the ceiling becomes 0x24 (36) / 0x21 (33) wrap bounds.
- **Locked-item gating**: a locked car (`g_savedCarLockTable[carId]`) or locked track
  (`g_packedConfig_displayPrefsBlock[track]`) draws a lock message AND blocks OK (state 4 btn2:
  plays SoundEffect 10 = error buzzer and returns without committing) — unless
  `g_cheatPostRaceHighScoreUnlock`!=0.
- ◄► wrap bounds differ by cheat: no-cheat wraps at g_savedMaxUnlockedCar / g_savedMusicTrackIndex;
  cheat wraps at 0x20/0x24 (cars) and 0x12/0x13 (tracks).

Input dispatch (state 4):
- `g_postRaceRacerCardNavDirection`!=0 (◄/►): if btn0 → car id += dir (wrap), rebake top half of
  panel; if btn1 → track id += dir (wrap), rebake bottom half.
- `g_frontendButtonPressedFlag`: btn2(OK) → if locked&!cheat play SoundEffect 10 + return; else
  commit car/paint/wheel, return=~LOCALIZATION_INIT (sentinel → InitializeRaceSeriesSchedule +
  InitializeFrontendDisplayModeState = start race), →state5. btn3(BACK) → commit car, return=
  MAIN_MENU(5), →state5. ESC=btn3.
- State 6: if return==~LOCALIZATION_INIT → InitializeRaceSeriesSchedule + InitializeFrontendDisplay
  ModeState (race); else SetFrontendScreen(return).

Confidence: [CONFIRMED @0x004213d0 full body]. [UNCERTAIN: `DAT_00463e6d`/`g_cheatAttractMode
Override_PROVISIONAL` exact cheat semantics (all-cars vs all-tracks) not separately resolved;
the format string `g_quickRaceMenuFormatPrefix` feeds sprintf for both car and track names — the
selecting field (car vs track) is implied by which BltColorFillToSurface y-band is cleared (0 vs
0x78), decompiled literally above.]

---

### Screen 8 @0x00418d50 — RunFrontendConnectionBrowser  [interactive Y] — LIST BROWSER

Inner states (in-body switch): 0 init (computer name, ConnectionEnumerate), 1 build buttons +
list-panel surface, 2 slide-in (anim→0x14=20), 3 first list paint + ready-gate, 4 second list
paint + force btnIndex=1, 5 INTERACTIVE (button press: OK/Back), 6 LIST NAVIGATION (up/down/mouse
cursor row-hit), 8 exit settle, 9 slide-out → release / branch, 10 commit → SetFrontendScreen
(SESSION_PICKER 9), 0xb cross-fade → SetFrontendScreen(MAIN_MENU 5).

ELEMENTS:
| element | produced by (fn/global/asset) | INLINE/FLUSH | dest pos | gating | notes |
|---|---|---|---|---|---|
| Full-screen background | LoadTgaToFrontendSurface16bpp(MainMenu.tga); CopyPrimaryFrontendBufferToSecondary; BlitFrontendCachedRect | INLINE | (0,0) 640×480 | state 0 | |
| Header label "CHOOSE CONNECTION" | CreateMenuStringLabelSurface(5)→g_currentScreenIndex; QueueFrontendOverlayRect anim X | FLUSH | header band | all | |
| List-panel button (big) | CreateFrontendDisplayModeButton(SNK_ChooseConnectionButTxt,-0x1f0,cy-0x2f,0x1f0,0x80)→g_postRaceNameButtonSurfacePtr | FLUSH | cy-0x2f, 0x1f0×0x80 | state 1 | slot 0 = the list container |
| **List rows (connection names)** | BltColorFillToSurface(panel rect 0x20,iVar,0x1b0,0x5e) then DrawFrontendClippedStringToSurface(dpu_exref + (scrollOffset+row)*0x3c, x=0x20, y, panel, clip 0x1b0) for up to 4 rows | INLINE bake (into the slot-0 panel surface → then FLUSH-drawn) | rows at y=base+0x10*(row+1) inside panel | states 3/4/5/6 | row count = min(4, *(dpu_exref+0x1e0)); stride 0x3c=60 per connection record |
| **Up-scroll indicator ▲** | QueueFrontendOverlayRect(slot0.x+0xf2, slot0.y+0x1e, …,0xc,0xc,0xff0000, g_browserScrollIndicatorSprite) | FLUSH | (x+0xf2, y+0x1e) 12×12 | scrollOffset!=0 | states 5/6 |
| **Down-scroll indicator ▼** | QueueFrontendOverlayRect(slot0.x+0xf2, slot0.y+0x6c, src y 0xc,0xc,0xc,0xff0000, scrollIndicatorSprite) | FLUSH | (x+0xf2, y+0x6c) 12×12 | scrollOffset+4 < count | states 5/6 |
| **List selection bar** (row cursor) | QueueFrontendOverlayRect(slot0.x+0x10, (cursor-scroll)*0x10+0x2f+slot0.y, src y 0x1b,0xc,9,0xff0000, g_browserSelectionBarSprite) | FLUSH | per-row | state 5 always; state 6 blinks (anim&0x10) | the row highlight (distinct from the button edge-bar highlight) |
| OK button | CreateFrontendDisplayModeButton(SNK_OkButTxt,-0x1f0,cy+0x89,0x60,0x20) | FLUSH | cy+0x89 | state 1 | slot 1 |
| BACK button | CreateFrontendDisplayModeButton(SNK_BackButTxt,-0x1f0,cy+0x89,0x70,0x20) | FLUSH | cy+0x89 | state 1 | slot 2, ESC=2 |
| Button selection highlight | RenderFrontendDisplayModeHighlight 0x4263e0 | FLUSH (decoupled) | selected slot | active | |
| Mouse cursor | LOOP queue | FLUSH (decoupled) | mouse XY | | |

Primed contract globals: `g_currentScreenIndex` (header), `g_postRaceNameButtonSurfacePtr_PROVISIONAL`
(the list-panel surface = slot 0), `g_connBrowserCursorIndex` @0x004970a8 (selected row),
`g_connBrowserScrollOffset` @0x00497038 (top visible row), `g_connBrowserRedrawDirty` @0x0049730c
(alternating colour/redraw latch — even→bg 0x392152 light, odd→0 dark), `g_frontendEscKeyButtonIndex`=2,
`g_returnToScreenIndex`, `g_connBrowserListOriginX_PROVISIONAL[0]` (read for the row/scroll/cursor
sprite positions — origin_x/origin_y/select_progress fields). Connection list source =
`dpu_exref` array: count at +0x1e0, records stride 0x3c (60), picked index written to +0x1e4.

Animation: state 2 slide-in 3 button slots, commit anim==0x14(20)+SoundEffect 4. State 6 row-cursor
blinks via (anim & 0x10). State 9 slide-out commit anim==0x18(24). State 0xb cross-fade via
AdvanceCrossFadeTransition (LoadTgaToFrontendSurface16bppVariant at anim==1).

Conditional elements:
- **Computer-name seed (state 0)**: GetComputerNameA → uppercased; if empty falls back to copying
  "Clint Eastwood" (s_Clint_Eastwood_00465f84) into g_localComputerName.
- Scroll indicators only when off-top / off-bottom (scrollOffset!=0 / scrollOffset+4<count).
- State 6 list-cursor only blinks while the cursor row is within the 4 visible rows AND
  (anim&0x10)!=0.
- Row count capped at 4 (`if (3<iVar9) iVar9=4`).

Input dispatch:
- State 5 (button mode): btn0(the list panel) press → state6 (enter list-nav). btn1(OK) →
  return=9 (SESSION_PICKER), state8. btn2(BACK) → return=5 (MAIN_MENU), state8. ESC=2.
- State 6 (list-nav): edge bit 0x200 (up) → cursor--, adjust scroll, SoundEffect 2; edge bit 0x400
  (down) → cursor++, adjust scroll; mouse press → row-hit math from (mouseY - slot0.origin_y) maps
  to row 0..4 (row0 = scroll up, row5 = scroll down, mid = set cursor); leaving list (btnIndex!=0
  with mouse) → back to state5. Each nav re-bakes the rows (BltColorFillToSurface + ClippedString
  loop) and bumps redrawDirty. On press in mid: cursor committed.
- State 9 branch: if return==5 → release header surf, state 0xb (cross-fade to main menu); else
  state10 → write picked index to dpu+0x1e4 → SetFrontendScreen(SESSION_PICKER).

Confidence: [CONFIRMED @0x00418d50 full body; g_connBrowserCursorIndex @0x004970a8,
g_connBrowserScrollOffset @0x00497038, g_connBrowserRedrawDirty @0x0049730c,
g_connBrowserListOriginX_PROVISIONAL @0x00499c78 stride 0x34]. [UNCERTAIN: `dpu_exref` is the DXPlay
connection-list block; +0x1e0=count, +0x1e4=picked-index, record stride 0x3c — field meaning beyond
the displayed name string not resolved (ARCH-DIVERGENCE: DXPTYPE net layer).]

---

### Screen 9 @0x00419cf0 — RunFrontendSessionPicker  [interactive Y] — LIST BROWSER

Inner states (in-body switch): 0 ConnectionPick + reset cursor/scroll, 1 build buttons + first
RenderFrontendSessionBrowser, 2 slide-in (anim→0x20=32) + force btnIndex=1, 3 INTERACTIVE (button
press: OK=join / New / Back), 4 LIST NAVIGATION (up/down/mouse), 6 exit settle, 7 slide-out →
SetFrontendScreen. (Note: states 5 absent; the body falls through to the common
`g_connBrowserScrollOffset = uVar1` tail.)

ELEMENTS:
| element | produced by (fn/global/asset) | INLINE/FLUSH | dest pos | gating | notes |
|---|---|---|---|---|---|
| Header label "CHOOSE SESSION" | g_currentScreenIndex (inherited from screen 8's CreateMenuStringLabelSurface(5)); QueueFrontendOverlayRect anim X | FLUSH | header band | all | screen 9 does NOT rebuild it — reuses g_currentScreenIndex |
| List-panel button (big) | CreateFrontendDisplayModeButton(SNK_ChooseSessionButTxt,-0x1f0,0,0x1f0,0x80)→g_postRaceNameButtonSurfacePtr | FLUSH | slot 0 | state 1 | the session-list container |
| **List rows (sessions)** | RenderFrontendSessionBrowser 0x00419b30: per row (≤4) DrawFrontendClippedStringToSurface ×3 — session name (dpu+rec*0x84+0x164, x=0x20 clip 0xa0), host name (+0x1a0, x=0xc6 clip 0xa0), game-type label (SNK_GameTypeTxt + typeIdx*0x10, x=0x16c clip 0x3c), and player count "d/d" via sprintf (s__d__d_00465f94, x=0x1b2 clip 0x24). Row 0 special = "NEW SESSION" (SNK_NewSessionTxt) | INLINE bake (into slot-0 panel) → FLUSH-drawn | rows inside panel | states 1/2/3/4 | record stride 0x84 (132); count = *(dpu_exref+0xbfc)+1 |
| **Up/Down scroll indicators** | QueueFrontendOverlayRect(slot0.x+0xf2, …, g_browserScrollIndicatorSprite) | FLUSH | (x+0xf2, y+0x1e / y+0x6c) 12×12 | scrollOffset!=0 / scrollOffset+4<count+1 | states 3/4 |
| **List selection bar** | QueueFrontendOverlayRect(slot0.x+0x10, (cursor-scroll)*0x10+0x2f+slot0.y, src y 0x1b,0xc,9,0xff0000, g_browserSelectionBarSprite) | FLUSH | per-row | state 3 always; state 4 blinks (anim&0x10) | row highlight |
| OK / New button | CreateFrontendDisplayModeButton(SNK_OkButTxt,-0x60,0,0x60,0x20) | FLUSH | slot 1 | state 1 | |
| BACK button | CreateFrontendDisplayModeButton(SNK_BackButTxt,-0x70,0,0x70,0x20) | FLUSH | slot 2 | state 1 | ESC=2 |
| Button selection highlight | RenderFrontendDisplayModeHighlight 0x4263e0 | FLUSH (decoupled) | selected slot | active | |
| Mouse cursor | LOOP queue | FLUSH (decoupled) | mouse XY | | |

Primed contract globals: `g_postRaceNameButtonSurfacePtr_PROVISIONAL` (session-list panel = slot 0),
`g_connBrowserCursorIndex` (selected session row), `g_connBrowserScrollOffset` (top row),
`g_connBrowserRedrawDirty` (colour/redraw latch), `g_frontendEscKeyButtonIndex`=2,
`g_returnToScreenIndex`, `g_currentScreenIndex` (header, inherited). Session list source =
`dpu_exref`: count-1 at +0xbfc, dirty/refresh flag at +0xc04 (RenderFrontendSessionBrowser clears
it), record stride 0x84 (132) from base +0x164, chosen session index written to +0xc00.
SNK_GameTypeTxt table + SNK_NewSessionTxt are the per-row labels.

Animation: state 2 slide-in 3 button slots by anim-derived X/Y, commit anim==0x20(32) +
SoundEffect 4 + DeactivateFrontendCursorOverlay. State 4 row-cursor blinks (anim&0x10). State 7
slide-out, commit anim==0x18(24) → release surfaces → SetFrontendScreen(return). RenderFrontendSession
Browser re-runs whenever redrawDirty&1 OR the net-refresh flag *(dpu+0xc04)!=0 (sessions arriving).

Conditional elements:
- **Row 0 = "NEW SESSION"** (SNK_NewSessionTxt) always; data rows are real enumerated sessions.
- Live session refresh: the list re-renders when DXPlay reports new sessions (dpu+0xc04) — the only
  one of the 5 screens whose list mutates from an external (network) source mid-frame.
- Scroll/selection-bar gating identical to screen 8 but count uses +0xbfc (count-1, so +1 in tests).

Input dispatch:
- State 3 (button mode): btn0(list panel) → state4 (list-nav). btn1(OK/Join) → write chosen index
  (cursor-1) to dpu+0xc00, return=CREATE_SESSION(10), DXPlay::EnumSessionTimer(0), state6.
  btn2(BACK) → return=CONNECTION_BROWSER(8), EnumSessionTimer(0), state6. ESC=2.
- State 4 (list-nav): edge 0x200 up → cursor--, scroll adjust; edge 0x400 down → cursor++, scroll
  adjust, SoundEffect 2; mouse press → row-hit math (mouseY - slot0.origin_y) → row 0..4 (row0
  scroll-up, row5 scroll-down, mid set cursor); btnIndex!=0 w/ mouse → back to state3.
- State 7: SetFrontendScreen(return); if return==CONNECTION_BROWSER release the header surf first.

Confidence: [CONFIRMED @0x00419cf0 + RenderFrontendSessionBrowser @0x00419b30 full bodies; same
browser globals as screen 8]. [UNCERTAIN: `dpu_exref` field map (+0xbfc count-1, +0xc00 chosen,
+0xc04 refresh, record stride 0x84 with name@+0x164 / host@+0x1a0 / gametype@+0x1e4) is the DXPlay
session block — beyond display strings, semantics are the net layer (ARCH-DIVERGENCE: DXPTYPE).]
# Frontend screens 10–14 — complete per-screen element + behavior spec

RE target: `TD5_d3d.exe` (image base `0x00400000`). All addresses Ghidra VAs. Literal
decompilation only; each claim carries its address. Hex AND decimal where ambiguous.

Cross-cutting facts that apply to ALL 5 screens (so the per-screen ELEMENTS tables stay short):

- **Every menu button is INLINE-built but FLUSH-drawn.** `CreateFrontendDisplayModeButton`
  (`0x00425de0`) bakes a tracked surface (frame from `DAT_00496268` ButtonBits.tga + label text
  via `DrawFrontendLocalizedStringToSurface` 24×24 / body 12×12) and adds a slot to
  `g_connBrowserListOriginX_PROVISIONAL[]` (`0x00499c78`, stride 0x34). The pixels reach screen via
  the per-frame `RenderFrontendUiRects` (`0x00425a30`) → `QueueFrontendSpriteBlit` → flush
  sprite-loop (`FlushFrontendSpriteBlits 0x00425540`). Button negative X (e.g. `-0x130`) = "auto-lay
  out, width follows"; the slide-in/out states reposition slots each frame with `MoveFrontendSpriteRect`.
- **Header title** = a baked label surface `g_currentScreenIndex` (misnamed; it is the header-label
  surface ptr) from `CreateMenuStringLabelSurface(N)` (`0x00412e30`): N selects `SNK_MenuStrings[N]`;
  N=5 for the net screens, N=6 for the options screens. It is re-queued every frame at an animated X
  via `QueueFrontendOverlayRect` (FLUSH-drawn). `g_menuHeaderLabelSurfaceWidth/Height/YOffset` set by
  the same fn.
- **Selection / hover highlight** = `RenderFrontendDisplayModeHighlight 0x004263e0` (DECOUPLED, runs
  inside flush): 4 edge bars color `0xc000` around the selected slot, gated on
  `g_frontendOverlayRectArrayTail != -1 && g_frontendCursorOverlayHidden==0`.
- **Mouse cursor sprite** queued by LOOP when `g_frontendCursorOverlayHidden==0 &&
  g_frontendMouseCursorEnabled==1` (snkmouse, `g_frontendCursorTextureId`). FLUSH-drawn.
- **Slide animation** is frame-count driven: position = `base ± g_frontendAnimFrameCounter*step`;
  transition fires on exact equality (mostly `0x20`=32 in/out, `0x27`=39 options slide-in,
  `0x10`=16 options slide-out, `0x18`=24 lobby dialog).
- **g_returnToScreenIndex sentinel**: `~TD5_SCREEN_LOCALIZATION_INIT` = `~0` = `0xFFFFFFFF` (-1) is
  the "entered from the in-race PAUSE options" path → exit via `InitializeFrontendDisplayModeState()`
  (resume race) instead of `SetFrontendScreen()`. (`TD5_SCREEN_LOCALIZATION_INIT`=0 = idx0.)
- Screen enum equates seen: MAIN_MENU=5, RACE_TYPE=6, SESSION_PICKER=9, OPTIONS_HUB=12,
  GAME_OPTIONS=13, CONTROL_OPTIONS=14, SOUND_OPTIONS=15, DISPLAY_OPTIONS=16,
  TWO_PLAYER_OPTIONS=17, CONTROLLER_BINDING=18, CAR_SELECTION=20, SESSION_LOCKED=29.

---

### Screen 10 @0x0041a7b0 — RunFrontendCreateSessionFlow  [interactive Y]
Net session create/join name-entry flow (DXPTYPE arch-divergent — DirectPlay).

Inner states (`switch(g_frontendInnerState)`): TWO parallel name-entry sub-flows selected by
`g_networkLobbyEntryPhase` (set 1 by lobby/picker, read at state 0):
- **0** init "enter session name" (host path): if `g_networkLobbyEntryPhase==1` re-bakes header
  `CreateMenuStringLabelSurface(5)`; if `g_connBrowserCursorIndex != 0` jumps directly to state
  **0x10** (player-name path) else builds session-name buttons. Edit target = `&DAT_00497068`
  (session-name buf), max len 0x10.
- **1** slide-in (counter==0x20 → play snd4, clear entryPhase, Deactivate cursor, ++state).
- **2** interactive: `RenderFrontendCreateSessionNameInput()`; on edit-commit (`g_postRaceNameEditState==2`)
  copies the typed name (or, if empty `iVar6==-2`, falls back to `&g_localComputerName`) into
  `&DAT_00497068`, sets `g_returnToScreenIndex=0`, ++state. Back button (idx1) → state 3 with
  `g_returnToScreenIndex=SESSION_PICKER(9)`.
- **3** slide-out → release buttons/surface, restore small-text colorkeys, → state 4 (if returnIdx!=0)
  via LAB_0041b1f3, else state 8.
- **4** init "enter player name" buttons (`SNK_EnterPlayerNameButTxt`); target = `&g_postRacePlayerNameEntryBuffer`.
- **5** slide-in (counter==0x20 → snd4, Deactivate, ++state).
- **6** interactive player-name edit; commit copies name (computer-name fallback). Back(idx1) →
  returnIdx=SESSION_PICKER, ++state.
- **7** slide-out → release, if returnIdx!=0 reset to state 0 else ++state.
- **8** **JoinSession bridge / car-select handoff**: release header surface, set
  `g_networkLobbyEntryPhase=1`, clear flags, `g_frontendSelectedGameType=0`,
  `SetFrontendScreen(CAR_SELECTION=20)`.
- **0x10/0x11/0x12/0x13** = mirror of 4/5/6/7 but the "join" variant (entered when
  g_connBrowserCursorIndex!=0 at state 0); state 0x13 exit does `SetFrontendScreen(g_returnToScreenIndex)`.
- **0x14** `DXPlay::JoinSession`: init `g_lobbySlotStatusTable[6]=0x7f`, clear host/status; on fail
  → `SetFrontendScreen(SESSION_LOCKED=29)`; on success seed `g_randomSeedForRace`, ++state.
- **0x15** build the `*SeshJoinMsg` packet (player name into `g_lobbyNetMessageDispatchFlag`),
  `QueueFrontendNetworkMessage(1,…)`, `DXPlay::SendMessageA(1,…)`, `SetFrontendScreen(NETWORK_LOBBY=11)`.

ELEMENTS:
| element | produced by (fn/global/asset) | INLINE/FLUSH | dest pos | gating | notes |
|---|---|---|---|---|---|
| Header label "Multiplayer" | CreateMenuStringLabelSurface(5)→g_currentScreenIndex | INLINE bake; FLUSH draw (QueueFrontendOverlayRect every state) | (cx-200, cy-0x9f-hdrYoff-0x40) static / animated X in slide states | always | re-baked at state0 only if entryPhase==1 |
| "Enter New Session Name" button (big) | CreateFrontendDisplayModeButton(SNK_EnterNewSessionNameButTxt,-0x1c0,0,0x1c0,0x40) → slot0, g_postRaceNameButtonSurfacePtr | INLINE bake; FLUSH draw | slot0, slide via MoveFrontendSpriteRect | state 0 | host path. Doubles as the text-input panel surface |
| "Enter Player Name" button (big) | CreateFrontendDisplayModeButton(SNK_EnterPlayerNameButTxt,-0x1c0,0,0x1c0,0x40) | INLINE bake; FLUSH | slot0 | states 4/0x10 | join path |
| Back button | CreateFrontendDisplayModeButton(SNK_BackButTxt,-0x70,0,0x70,0x20) → slot1 | INLINE bake; FLUSH | slot1 | states 0/4/0x10 | ESC target (g_frontendEscKeyButtonIndex=1) |
| Text-input field + typed string | RenderFrontendCreateSessionNameInput 0x0041a530: BltColorFillToSurface fill + DrawFrontendClippedStringToSurface(g_postRaceNameEditTargetPtr) onto button surface | INLINE bake into slot0 surface; FLUSH draw | inside slot0 (x=0x14,w=0x198,h=0x10) | states 2/6/0x12 | fill color 0x392152 idle / 0 when redraw-dirty bit set |
| Blinking text caret | RenderFrontendCreateSessionNameInput: BltColorFillToSurface(0xff00, caretX, …,2,0x10) | INLINE bake; FLUSH | after measured string width | (g_frontendAnimFrameCounter&0x20)==0 && editState!=2 && focused(buttonIndex==0) | green 2px caret; suppressed if width>0x195 |
| Mouse cursor | LOOP QueueFrontendOverlayRect(cursorTexId) | FLUSH | mouse pos | cursorOverlayHidden==0 | Activated states 0(host)/4/0x10; Deactivated in slide states |
| Selection highlight bars | RenderFrontendDisplayModeHighlight 0x004263e0 | FLUSH (decoupled) | around selected slot | tail!=-1 && !hidden | |

Primed contract globals: `g_postRaceNameEditTargetPtr` (edit buffer), `_g_postRaceNameEditMaxLength`
(0x10), `g_postRaceNameEditState` (1=editing,2=committed), `g_frontendEscKeyButtonIndex=1`,
`g_currentScreenIndex` (header surf), `g_postRaceNameButtonSurfacePtr_PROVISIONAL` (input-panel surf),
`g_networkLobbyEntryPhase` (which sub-flow), `g_connBrowserCursorIndex` (host vs join),
`g_returnToScreenIndex`, `g_connBrowserRedrawDirty` (caret/fill toggle), `g_randomSeedForRace`.

Animation: slide-in states 1/5/0x11 slot0 X = `iVar8 + counter*-0x18 + 0x30a`, slot1 X =
`(cx-0x32a)+counter*0x18`, commit at counter==0x20. Slide-out states 3/7/0x13 slot0 X =
`iVar8 + counter*-0x20 + 10`, slot1 X = `counter*0x20 + 0xa8 + iVar8`, commit at counter==0x20.
Caret blink = `g_frontendAnimFrameCounter & 0x20`.

Conditional elements: name-entry sub-flow chosen by `g_networkLobbyEntryPhase` /
`g_connBrowserCursorIndex`; empty-name → computer-name fallback (`g_localComputerName`); JoinSession
failure → SESSION_LOCKED dialog; the whole screen is DXPTYPE/DirectPlay (ARCH-DIVERGENCE — port
mirrors at td5_frontend.c:10322).

Input dispatch: DXInput text capture in RenderFrontendCreateSessionNameInput
(`DXInput::SetAnsi(1)/GetString` when buttonIndex==0, else SetAnsi(0)); commit detected via
`g_postRaceNameEditState==2`; Back = `g_frontendButtonIndex==1 && g_frontendButtonPressedFlag`;
ESC mapped to idx1.

Confidence: [CONFIRMED @ 0x0041a7b0 body; helper 0x0041a530; CreateMenuStringLabelSurface 0x00412e30].
[UNCERTAIN: SNK_MenuStrings[5] literal text not dumped — "Multiplayer"/"Network" by context, not asserted.]

---

### Screen 11 @0x0041c330 — RunFrontendNetworkLobby  [interactive Y]
Network lobby (chat + player roster + status + host/client start handshake). DXPTYPE arch-divergent.
Heaviest of the 5; almost all panel pixels are FLUSH-drawn from tracked surfaces baked by 3 helper
fns each interactive frame.

Inner states: 0 init; 1 slide-in (commit counter==0x14); 2 prime chat-edit / set ESC→5; 3 main
interactive (chat + button handling: ChangeCar idx3, Start idx4, Exit idx5); 4 send-chat then →2;
5 host "all ready?" tally → 0xc or ++; 6 slide-in error/confirm dialog (commit==0x18); 7 build
dialog buttons (Yes/No or OK) via preview-layout; 8 dialog interactive; 9 dialog teardown / route;
10 dialog slide-out → 2; 0xc host SealSession + send role-assign msgs (0x12); 0xd wait config receipts
(0x13 resend, timeout 0xfa ms); 0xe seed schedule (`InitializeRaceSeriesSchedule`); 0xf push race
settings (msg 0x15, 0x80 bytes, 0xa5 ms beat); 0x10 final go (msg type 4 at counter==8) →
`InitializeFrontendDisplayModeState` (start race); 0x11 client wait for type-4 go msg via
`DXPlay::ReceiveMessage` → `InitializeFrontendDisplayModeState`.

ELEMENTS:
| element | produced by (fn/global/asset) | INLINE/FLUSH | dest pos | gating | notes |
|---|---|---|---|---|---|
| Header label "Multiplayer" | CreateMenuStringLabelSurface(5)→g_currentScreenIndex | INLINE bake; FLUSH | (cx-200,…) animated | state 0 | |
| MainMenu.tga full-screen bg | LoadTgaToFrontendSurface16bpp(MainMenu.tga) + CopyPrimaryFrontendBufferToSecondary | INLINE (decode into work surf) | full 640×480 | state 0 | cached for restore |
| Chat input strip (decoration) | CreateFrontendDisplayModeButton(NULL label,-0x1d0,0,0x1d0,0x18)→g_lobbyDecorationStripSurface (slot0) | INLINE bake; FLUSH | slot0 | state 0 | label-less strip; chat input drawn into it |
| **Message/chat window panel** | CreateFrontendDisplayModeButton(SNK_MessageWindowButTxt,-0x200,0,0x200,0x80)→g_postRaceNameButtonSurfacePtr (slot1) | INLINE bake; FLUSH | slot1 | state 0 | the big 0x200×0x80 chat-history surface |
| **Status / player-list panel** | CreateFrontendDisplayModeButton(SNK_StatusButTxt,-0xe0,0,0xe0,0x86)→g_lobbyPlayerRosterSurface (slot2) | INLINE bake; FLUSH | slot2 | state 0 | roster baked by RenderFrontendLobbyStatusPanel |
| "Change Car" button | CreateFrontendDisplayModeButton(SNK_ChangeCarButTxt,-200,0,200,0x20) slot3 | INLINE bake; FLUSH | slot3 | state 0 | idx3 |
| "Start" button | CreateFrontendDisplayModeButton(SNK_StartButTxt,-200,0,0x78,0x20) slot4 | INLINE bake; FLUSH | slot4 | state 0 | idx4 (host start) |
| "Exit" button | CreateFrontendDisplayModeButton(SNK_ExitButTxt,-200,0,0x78,0x20) slot5 | INLINE bake; FLUSH | slot5 | state 0 | idx5; ESC target (set 5 at state 2) |
| Session header bar (Session:/Player: + names) | g_lobbySessionHeaderSurface=CreateTrackedFrontendSurface(0x1e0,0x20); baked by RenderFrontendLobbyStatusPanel | INLINE bake; FLUSH (QueueFrontendOverlayRect every state) | (cx-0xb0, cy-0x97), 0x1e0×0x20 | state 0+ | SNK_TxtSession/SNK_TxtPlayer + host name + "(host)" SNK_TxtBhostB |
| **Chat history lines** | RenderFrontendLobbyChatPanel 0x0041bd00: per-msg DrawFrontendClippedStringToSurface(name 0x96w) + DrawFrontendWrappedStringLine onto chat panel | INLINE bake into slot1; FLUSH | inside slot1 (x0x18,y0x1a step 0x10) | state 3, when g_lobbyChatMessageRingCursor!=0 | up to 6 rows; scrolls via surface Lock+memmove (vtable+0x64/+0x80); emote token glyphs |
| **Player roster rows + status** | RenderFrontendLobbyStatusPanel 0x0041b420: per-slot DrawFrontendClippedStringToSurface(name 0x96w) + status text SNK_NetPlayStatMsg[status*10] onto roster surf | INLINE bake into slot2; FLUSH | inside slot2 (name x0x10, status x0xae, y step 0x10) | states 3/8 | local player drawn in smalltextB (highlight); fill clear each pass |
| **Chat text-input field + caret** | RenderFrontendLobbyChatInput 0x0041a670 onto g_lobbyDecorationStripSurface | INLINE bake; FLUSH | inside slot0 (x0x18,w0x1a0) | state 3 | green caret same as scr10; max len 0x3c |
| Error/confirm dialog box | g_lobbyErrorDialogSurface=CreateTrackedFrontendSurface(0x1e2,0x40); SNK_NetErrString1-4 centered | INLINE bake; FLUSH | (cx-0xba, cy-0x5f), 0x1e2×0x40 | states 6-10 | text chosen by g_returnToScreenIndex (err code) |
| Yes/No or OK dialog buttons | CreateFrontendDisplayModeButton(SNK_Yes/No/OkButTxt) under BeginFrontendDisplayModePreviewLayout | INLINE bake; FLUSH | centered | state 7 | preview-layout snapshot/restore |
| Mouse cursor + highlight bars | LOOP + RenderFrontendDisplayModeHighlight | FLUSH | — | cursorOverlayHidden==0 | |

Primed contract globals: `g_currentScreenIndex` (header), `g_postRaceNameButtonSurfacePtr` (chat
window), `g_lobbyDecorationStripSurface` (chat input strip), `g_lobbyPlayerRosterSurface` (status),
`g_lobbySessionHeaderSurface`, `g_lobbyErrorDialogSurface` (dialog), `g_lobbySlotStatusTable[6]`,
`g_lobbyRoleAcceptedTable[6]`, `g_lobbyConfigReceiptTable[6]`, `g_lobbyPlayerStatus` (0 idle/1 wait/2
ready/3 launch), `g_lobbyChatMessageRingBuffer`/`g_lobbyChatMessageRingCursor`/`g_lobbyChatInputCursor`,
`g_chatTokenizerCharClass` (chat edit buf), `g_postRaceNameEditState`, `g_frontendEscKeyButtonIndex=5`,
`g_lobbyAbortRequestLatch`, `g_mainMenuFlowPhase`, `g_networkSessionActive`, `g_lobbyHeartbeatTimestamp`,
`g_dxpdataRaceSettings*`, dpu_exref slot mirror fields (+0xbe4 host slot, +0xbe8 local slot, +0xbcc
slot-active table, +0xa64 name table).

Animation: state1 panels slide in (slot0 X = (cx-0x1a0)+counter*0xc; slots1-5 Y ramps), commit
counter==0x14(20). State6 dialog slide-in commit==0x18(24). State10 dialog slide-out commit==0x18.
Chat caret blink = counter&0x20.

Conditional elements: error dialog text 1/2 vs 3/4 by `g_returnToScreenIndex` (net error code);
Yes/No vs single OK by `g_returnToScreenIndex!=1 && !=2`; "(host)" suffix only when local==host slot;
local-player roster row highlighted (smalltextB); host-only states 0xc-0x10 vs client-only 0x11;
SealSession path. `g_lobbyAbortRequestLatch` at multiple states → tear down to SESSION_LOCKED.
Emote-token glyph substitution in chat (`:)`→0x1b, `:(`→0x1c, etc.) by NormalizeFrontendChatTokens.

Input dispatch: chat text via RenderFrontendLobbyChatInput DXInput::GetString (focus buttonIndex==0);
button remap at state 3 (idx2→3 ChangeCar, idx1→0); `g_frontendButtonPressedFlag` + index drives
ChangeCar(3)→CAR_SELECTION, Start(4)→host ready broadcast / state5, Exit(5)→quit-confirm dialog
(state6). Chat send: editState==2 → state4 → NormalizeFrontendChatTokens → QueueFrontendNetworkMessage(2)
+ SendMessageA(2). `*CHATCMD` slash-commands (`*team`,`*all`,`*kick`,`*ban` per the 4 _strncmp at
0x465fc4/cc/d4/dc) tokenized in NormalizeFrontendChatTokens 0x0041c030.

Confidence: [CONFIRMED @ 0x0041c330 body; helpers 0x0041bd00 chat-panel, 0x0041b420 status-panel,
0x0041a670 chat-input, 0x0041c030 tokenizer]. [UNCERTAIN: exact SNK_NetErrString / SNK_NetPlayStatMsg
literal text not dumped (LANGUAGE.DLL imports); the 4 chat slash-command literals at 0x465fc4–0x465fdc
read as 4-char compares but exact strings not dumped.]

---

### Screen 12 @0x0041d890 — ScreenOptionsHub  [interactive Y]
Options menu hub: 5 sub-screen entries + OK. No value displays / no arrows (it is a plain navigation
menu, not a settings list). NOTE: the LOOP runs the cheat-code FSM only while this screen is active
(`g_currentScreenFnPtr == PTR_ScreenOptionsHub`).

Inner states: 0 init (build 6 buttons, header(6), inline-string table SNK_Options_MT); 1/2
present+settle; 3 slide-in (commit counter==0x27 then settle via AdvanceFrontendTickAndCheckReady);
4/5 static double-present; 6 interactive (button dispatch); 7 slide-out prep; 8 slide-out → exit.

ELEMENTS:
| element | produced by (fn/global/asset) | INLINE/FLUSH | dest pos | gating | notes |
|---|---|---|---|---|---|
| Header "Options" | CreateMenuStringLabelSurface(6)→g_currentScreenIndex | INLINE bake; FLUSH | (cx-200,…) animated | state 0 | |
| MainMenu.tga bg | LoadTgaToFrontendSurface16bpp + CopyPrimaryFrontendBufferToSecondary | INLINE | full | state 0 | |
| "Game Options" button | CreateFrontendDisplayModeButton(SNK_GameOptionsButTxt,-0x130,0,0x130,0x20) slot0 | INLINE bake; FLUSH | slot0 | state 0 | idx0 → GAME_OPTIONS(13) |
| "Control Options" button | …(SNK_ControlOptionsButTxt,…) slot1 | INLINE bake; FLUSH | slot1 | state 0 | idx1 → CONTROL_OPTIONS(14) |
| "Sound Options" button | …(SNK_SoundOptionsButTxt,…) slot2 | INLINE bake; FLUSH | slot2 | state 0 | idx2 → SOUND_OPTIONS(15) |
| "Graphics Options" button | …(SNK_GraphicsOptionsButTxt,…) slot3 | INLINE bake; FLUSH | slot3 | state 0 | idx3 → DISPLAY_OPTIONS(16) |
| "Two Player Options" button | …(SNK_TwoPlayerOptionsButTxt,…) slot4 | INLINE bake; FLUSH | slot4 | state 0 | idx4 → TWO_PLAYER_OPTIONS(17) |
| "OK" button | …(SNK_OkButTxt,-0x130,0,0x60,0x20) slot5 | INLINE bake; FLUSH | slot5 | state 0 | idx5 → MAIN_MENU; ESC target (idx5) |
| Mouse cursor + highlight bars | LOOP + RenderFrontendDisplayModeHighlight | FLUSH | — | cursorOverlayHidden==0 | |

Primed contract globals: `g_currentScreenIndex`, `g_frontendEscKeyButtonIndex=5`, inline-string table
`SNK_Options_MT`, `g_returnToScreenIndex` (set per sub-screen), and at OK(idx5) it COMMITS the
in-race shadow→live copies: `g_cameraMode=g3dCollisionsConfigShadow^1`, particle-pool fields ←
`gDynamicsConfigShadow`/`g_specialEncounterUnlockB`, `gRaceDifficultyTier=g_raceDifficultyTier`,
`_g_specialEncounterType=g_specialEncounterUnlockA`. (This is the apply-options-on-exit-to-race path.)

Animation: state3 slide-in (slot0/2/4 X = counter*0x10-0x266+iVar5, slot1/3 X = …+counter*-0x10+0x27a,
slot5 Y ramp), commit at counter==0x27 (39) then clamps to 0x26 and waits AdvanceFrontendTickAndCheckReady.
State8 slide-out per-slot diverging vectors, commit counter==0x10 (16) → release + SetFrontendScreen
(or InitializeFrontendDisplayModeState if returnIdx==-1 in-race path).

Conditional elements: none beyond the in-race-resume branch (returnIdx==~0). No per-row value display,
NO ◄► arrows on this screen.

Input dispatch (state 6): `if(g_frontendButtonPressedFlag) switch(g_frontendButtonIndex)`: 0→GAME_OPTIONS,
1→CONTROL_OPTIONS, 2→SOUND_OPTIONS, 3→DISPLAY_OPTIONS, 4→TWO_PLAYER_OPTIONS, 5→MAIN_MENU(+commit
shadows). Cheat-code key sequences handled by the LOOP (step 15), not here.

Confidence: [CONFIRMED @ 0x0041d890 full body].

---

### Screen 13 @0x0041f990 — ScreenGameOptions  [interactive Y]
Game-rules settings list: 7 value rows (each label + value text + ◄► arrows) + OK. The value column is
a SINGLE shared tracked surface `g_lobbyErrorDialogSurface` (0xe0×0x118) that is re-baked whole on each
change and FLUSH-drawn as one overlay rect beside the button column.

Inner states: 0 init (build 8 buttons, 7 arrow rows, value-panel surface, header(6), inline-string
SNK_GameOptions_MT); 1/2 present+settle; 3 slide-in (commit==0x27); 4/5 static + (state4 only) re-bake
value panel; 6 interactive (arrow cycle + OK); 7 slide-out prep; 8 slide-out → exit (commit==0x10).

ELEMENTS:
| element | produced by | INLINE/FLUSH | dest pos | gating | notes |
|---|---|---|---|---|---|
| Header "Options" | CreateMenuStringLabelSurface(6) | INLINE bake; FLUSH | (cx-200,…) | state 0 | |
| Value-display panel (all 7 rows) | g_lobbyErrorDialogSurface = CreateTrackedFrontendSurface(0xe0,0x118); baked in state4 by 7× MeasureOrCenterFrontendLocalizedString + DrawFrontendLocalizedStringToSurface | INLINE bake; FLUSH (one QueueFrontendOverlayRect) | (cx+0x4a, cy-0x8f), 0xe0×0x118 | states 4-8 | re-baked only when entering state4 (after a change) |
| Row0 "Circuit Laps" label+◄►+value | CreateFrontendDisplayModeButton(SNK_CircuitLapsTxt,-0x128,0,0x128,0x20) slot0; InitializeFrontendDisplayModeArrows(0,1) | INLINE bake; FLUSH | slot0 | state 0 | value = sprintf_game(template) — laps count; global gCircuitLapsConfigShadow (0..3) |
| Row1 "Checkpoint Timers" | btn(SNK_CheckpointTimersButTxt) slot1; arrows(1,1) | INLINE; FLUSH | slot1 | state 0 | value = SNK_OnOffTxt[g_specialEncounterUnlockA] (on/off, &1) |
| Row2 "Traffic" | btn(SNK_TrafficButTxt) slot2; arrows(2,1) | INLINE; FLUSH | slot2 | state 0 | value = SNK_OnOffTxt[g_specialEncounterUnlockB] (&1) |
| Row3 "Cops" | btn(SNK_CopsButTxt) slot3; arrows(3,1) | INLINE; FLUSH | slot3 | state 0 | value = SNK_OnOffTxt[gSpecialEncounterConfigShadow] (&1) |
| Row4 "Difficulty" | btn(SNK_DifficultyButTxt) slot4; arrows(4,1) | INLINE; FLUSH | slot4 | state 0 | value = SNK_DifficultyTxt[g_raceDifficultyTier] (0..2 wrap) |
| Row5 "Dynamics" | btn(SNK_DynamicsButTxt) slot5; arrows(5,1) | INLINE; FLUSH | slot5 | state 0 | value = SNK_DynamicsTxt[gDynamicsConfigShadow] (&1) |
| Row6 "3D Collisions" | btn(SNK_3dCollisionsButTxt) slot6; arrows(6,1) | INLINE; FLUSH | slot6 | state 0 | value = SNK_OnOffTxt[g3dCollisionsConfigShadow] (&1) |
| "OK" button | btn(SNK_OkButTxt,-0x128,0,0x60,0x20) slot7 | INLINE; FLUSH | slot7 | state 0 | idx7 → OPTIONS_HUB; ESC target (idx7) |
| ◄► arrows (rows 0-6) | InitializeFrontendDisplayModeArrows(N,1) from g_browserSelectionBarSprite | INLINE bake into each slot surf (sets flags|=2); FLUSH | edges of each row slot | state 0 | right_flag=1 |
| Mouse cursor + highlight | LOOP + RenderFrontendDisplayModeHighlight | FLUSH | — | cursorOverlayHidden==0 | |

Primed contract globals (the values each row reflects): row0 `gCircuitLapsConfigShadow` (int,clamp
0..3); row1 `g_specialEncounterUnlockA_PROVISIONAL`; row2 `g_specialEncounterUnlockB_PROVISIONAL`;
row3 `gSpecialEncounterConfigShadow`; row4 `g_raceDifficultyTier` (0x00466010, wrap 0↔2); row5
`gDynamicsConfigShadow`; row6 `g3dCollisionsConfigShadow`. Plus `g_lobbyErrorDialogSurface` (value
panel), `g_currentScreenIndex`, `g_frontendEscKeyButtonIndex=7`, inline-string `SNK_GameOptions_MT`,
`g_returnToScreenIndex`. (These shadows are the ones ScreenOptionsHub OK commits to live globals.)

Animation: state3 slide-in commit==0x27(39); state8 slide-out commit==0x10(16). On any value change the
screen jumps back to state4 to re-bake the value panel.

Conditional elements: in-race-resume branch (returnIdx==~0 → InitializeRaceSeriesSchedule +
InitializeFrontendDisplayModeState). Value panel re-baked only in state4. No other conditionals.

Input dispatch (state 6): `if(g_postRaceRacerCardNavDirection!=0) switch(g_frontendButtonIndex)`:
row0 `gCircuitLapsConfigShadow += dir` clamp[0,3]; row1/2/3/5/6 `= (dir + shadow) & 1` (toggle);
row4 `g_raceDifficultyTier += dir` wrap (<0→2, >2→0); then force state=4 (re-bake). OK:
`g_frontendButtonPressedFlag && g_frontendButtonIndex==7` → returnIdx=OPTIONS_HUB(12), ++state.
`g_postRaceRacerCardNavDirection` is the ◄(−1)/►(+1) delta from the selection update.

Confidence: [CONFIRMED @ 0x0041f990 full body; value LUTs SNK_OnOffTxt(ptr@0045d350)/
SNK_DifficultyTxt(0045d34c)/SNK_DynamicsTxt(0045d2c4) are LANGUAGE.DLL pointer tables indexed *4].
[UNCERTAIN: the row0 sprintf template `g_uiFormatStringScratchTemplate_PROVISIONAL` literal not dumped
— produces the laps-count numeral string.]

---

### Screen 14 @0x0041df20 — ScreenControlOptions  [interactive Y]
Input-device options: 2 player rows (each = Player label button + device-name value + device ICON +
◄► arrows) + 2 Configure buttons + OK. The device NAME panel is the shared 0xe0×0xa0
`g_lobbyErrorDialogSurface`; the device ICONS come from Controllers.tga (loaded into
`g_soundOptionsMenuVolume` — misnamed surface).

Inner states: 0 init (build 5 buttons, arrows on rows 0 & 2 ONLY, load Controllers.tga, value panel,
header(6), inline SNK_CtrlOptions_MT, + in-race-extra string SNK_CtrlOptions_Ex if returnIdx==~0);
1/2 present+settle; 3 slide-in + device-icon overlay (commit==0x27); 4/5 static + (state4) re-bake
device-name panel; 6 interactive (cycle device / Configure / OK); 7 slide-out prep; 8 slide-out → exit
(commit==0x10).

Device class → row value index (`local_84` for P1, `local_88` for P2), computed at top of fn from
`(&g_player1DeviceDesc)[playerInputSource]`: byte==3 → 0 (keyboard); byte==4 → 1 (joystick), unless
hi-byte 0x400 → 2 (wheel) or 0x600 → 3 (wheel+pedals); desc==0xffffffff → 4 (none/disabled, icon hidden).

ELEMENTS:
| element | produced by | INLINE/FLUSH | dest pos | gating | notes |
|---|---|---|---|---|---|
| Header "Options" | CreateMenuStringLabelSurface(6) | INLINE bake; FLUSH | (cx-200,…) | state 0 | |
| Device-name value panel (P1+P2) | g_lobbyErrorDialogSurface=CreateTrackedFrontendSurface(0xe0,0xa0); state4 bakes 2× sprintf_game + MeasureOrCenterFrontendLocalizedString + DrawFrontendLocalizedStringToSurface | INLINE bake; FLUSH | (cx+0x4a, cy-0x8f), 0xe0×0xa0 | states 4-8 | fmt = DAT_004658e4 (keyboard, class&3==0) else s__s__d (joystick "%s %d") |
| **Device ICON P1** | Controllers.tga surface g_soundOptionsMenuVolume; QueueFrontendOverlayRect(src y=local_84<<5, 0x40×0x20) | INLINE-loaded asset; FLUSH | (cx+0x4a, cy-0x8f) static / animated | states 3-8, gated local_84!=4 | icon row = device class (kbd/js/wheel/pedals); src cell 0x40×0x20, row stride 0x20 |
| **Device ICON P2** | same surface; src y=local_88<<5 | FLUSH | (cx+0x4a, cy-0x17) | local_88!=4 | hidden when device==none |
| Row0 "Player 1" label+◄► | btn(SNK_Player1ButTxt,-0x100,0,0x100,0x20) slot0; arrows(0,1) | INLINE bake; FLUSH | slot0 | state 0 | idx0 cycles P1 device |
| Row1 "Configure" (P1) | btn(SNK_ConfigureButTxt,-0x100,0,0x100,0x20) slot1 | INLINE bake; FLUSH | slot1 | state 0 | idx1 → CONTROLLER_BINDING (slot=0); NO arrows |
| Row2 "Player 2" label+◄► | btn(SNK_Player2ButTxt) slot2; arrows(2,1) | INLINE bake; FLUSH | slot2 | state 0 | idx2 cycles P2 device |
| Row3 "Configure" (P2) | btn(SNK_ConfigureButTxt) slot3 | INLINE bake; FLUSH | slot3 | state 0 | idx3 → CONTROLLER_BINDING (slot=1); NO arrows |
| "OK" button | btn(SNK_OkButTxt,-0x100,0,0x60,0x20) slot4 | INLINE bake; FLUSH | slot4 | state 0 | idx4 → OPTIONS_HUB; ESC target (idx4) |
| ◄► arrows (rows 0 & 2 only) | InitializeFrontendDisplayModeArrows(0,1) + (2,1) | INLINE bake; FLUSH | row0/row2 edges | state 0 | Configure rows (1,3) have NO arrows |
| Mouse cursor + highlight | LOOP + RenderFrontendDisplayModeHighlight | FLUSH | — | cursorOverlayHidden==0 | |

Primed contract globals: `g_player1InputSource` / `g_player2InputSource` (the device-slot index the
row value reflects; wrap &7, skips desc==0 slots, can't equal the other player's slot),
`g_player1DeviceDesc[]` (0x00465660, the 8-entry device descriptor table — byte0 = type 3/4, byte1 =
sub-class 0x04/0x06), `g_soundOptionsMenuVolume` (Controllers.tga icon surface),
`g_lobbyErrorDialogSurface` (name panel), `g_controllerBindingActivePlayerSlot` (0 or 1, which player
the Configure page edits), `g_currentScreenIndex`, `g_frontendEscKeyButtonIndex=4`, inline-string
`SNK_CtrlOptions_MT` (+ `SNK_CtrlOptions_Ex` at entry slot 9 if in-race), `g_returnToScreenIndex`.

Animation: state3 slide-in commit==0x27(39), device icons slide with the rows
(X=iVar4+counter*-0x10+0x38c). state8 slide-out commit==0x10(16); icons follow row Y.

Conditional elements: device icon hidden when class index ==4 (desc==0xffffffff = no device);
name-panel format string keyboard vs joystick by `(class & 3)==0`; arrows present on rows 0/2 only;
in-race entry adds `SNK_CtrlOptions_Ex` extra line (returnIdx==~0) and OK routes back to in-race options
(`((returnIdx!=~0)-1 & 0xfffffff9) + OPTIONS_HUB` = OPTIONS_HUB normally, in-race sentinel otherwise).

Input dispatch (state 6): `if(g_postRaceRacerCardNavDirection!=0)`: idx0 → advance `g_player1InputSource`
(+dir &7, skip empty desc, skip == P2) then state4; idx2 → same for `g_player2InputSource`.
`if(g_frontendButtonPressedFlag)`: idx1 → set slot=0, returnIdx=CONTROLLER_BINDING(18), ++state;
idx3 → slot=1, returnIdx=CONTROLLER_BINDING; idx4 → OK back to OPTIONS_HUB (or in-race). Device cycle =
◄►; Configure = press; persistence of source selection only (detailed binding compiled later at race
start by DXInput::SetConfiguration).

Confidence: [CONFIRMED @ 0x0041df20 full body; device-class decode at fn head; Controllers.tga via
s_Front_End_Controllers_tga_00466044 into g_soundOptionsMenuVolume; arrows only on slots 0,2].
[UNCERTAIN: name-panel format-string literals DAT_004658e4 (keyboard) / s__s__d_0046603c ("%s %d")
not dumped char-for-char; the device-name text source feeding %s is the LANGUAGE.DLL device label —
not asserted here. The per-icon Controllers.tga row→class mapping (0 kbd,1 js,2 wheel,3 pedals) is
inferred from the local_84 decode, not from inspecting the TGA cells.]
# Frontend Screens 15-19 — Per-Element + Behavior Spec (RESEARCH ONLY)

RE target: `TD5_d3d.exe` (image base 0x00400000). All addresses are Ghidra VAs. Source: literal
decompilation of each screen fn + memory reads (LUT/strings/tables). Grounds on
`frontend_rendering_model.md`, `frontend_flow_model.md`, `frontend_call_graph_closure.md`.

Engine model recap (applies to every screen below):
- A screen fn only **PRIMES**: it bakes label/panel surfaces, queues overlay rects
  (`QueueFrontendOverlayRect` @0x425660), moves sprite slots (`MoveFrontendSpriteRect` @0x4259d0),
  builds buttons (`CreateFrontendDisplayModeButton` @0x425de0 / `…PreviewButton` @0x4260e0), and
  advances its own `g_frontendInnerState` switch. Actual on-screen pixels for queued rects/buttons,
  the **gallery album art**, and the **selection highlight bars** are emitted later that frame by the
  decoupled per-frame FLUSH (`FlushFrontendSpriteBlits` @0x425540 → unconditional
  `UpdateExtrasGalleryDisplay` @0x40d830 + `RenderFrontendDisplayModeHighlight` @0x4263e0 + sprite list).
- "INLINE" below = drawn/baked by the screen fn into a surface (`BltColorFillToSurface`,
  `DrawFrontendLocalizedStringToSurface`, `RenderTgaToFrontendSurface`, `LoadTgaToFrontendSurface16bpp`).
  "FLUSH" = a slot/rect/global the screen primes that the per-frame flush draws.
- Canonical inner-state shape (all 5 use it): 0=init/load → 1/2=present/settle → 3 (or 9)=slide-in →
  4/5=static bake+double-present → 6 (or 10)=interactive → 7 (or 0xb)=slide-out prep → 8 (or 0xb)=
  slide-out+exit. Slides are **frame-count** driven (`g_frontendAnimFrameCounter`, +1/frame by LOOP),
  fire on exact equality (0x27=39 in, 0x10=16 / 0x1c=28 / 0x20=32 out). NO real-time interpolation.
- `g_postRaceRacerCardNavDirection` = the ◄/► cycle delta (−1/+1); `g_frontendButtonPressedFlag` =
  confirm-this-frame edge; `g_frontendButtonIndex` = active row; `g_frontendEscKeyButtonIndex` = ESC target.
- The header label (screen title) is a tracked surface from `CreateMenuStringLabelSurface(6)` stored in
  `g_currentScreenIndex`, re-queued each frame at an animated X via `QueueFrontendOverlayRect` (size
  `g_menuHeaderLabelSurfaceWidth/Height`, Y bias `g_menuHeaderLabelYOffset`). Present on ALL 5 screens.
- `~TD5_SCREEN_LOCALIZATION_INIT` = -1 (0xffffffff): exit-state sentinel meaning "go start a race"
  (calls `InitializeRaceSeriesSchedule` + `InitializeFrontendDisplayModeState`) instead of `SetFrontendScreen`.

---

### Screen 15 @0x0041ea90 — ScreenSoundOptions  [interactive Y]
Inner states (plain in-body `switch(g_frontendInnerState)`): 0 init/load; 1,2 present/settle (zero
anim, ++); 3 slide-in (panels+volume bars+SFX icon slide, commit at anim==0x27→0x26 via
AdvanceFrontendTickAndCheckReady, DXSound::Play(4)); 4,5 static bake+double-present (state 4 bakes the
SFX-mode-name text into the dialog surface); 6 interactive; 7 slide-out prep (restore secondary,
ActivateFrontendCursorOverlay, swap small-text TGA colorkey 0→0xffffff, Play(5)); 8 slide-out, commit
at anim==0x10 → release all surfaces → SetFrontendScreen(g_returnToScreenIndex).

ELEMENTS:
| element | produced by (fn/global/asset) | INLINE/FLUSH | dest pos | gating | notes |
|---|---|---|---|---|---|
| Full-screen menu background | LoadTgaToFrontendSurface16bpp(MainMenu.tga, FrontEnd.zip) → g_primaryWorkSurface, then CopyPrimaryFrontendBufferToSecondary | INLINE (state 0) | (0,0) 640×480 | always | cached to secondary for restore-on-exit |
| Header title label ("SOUND OPTIONS") | CreateMenuStringLabelSurface(6)→g_currentScreenIndex; re-queued each state | INLINE bake + FLUSH | animated X, Y=(iVar5−YOff)−0x40 | always | header surface |
| Button row 0 "SFX MODE" | CreateFrontendDisplayModeButton(SNK_SfxModeButTxt,-0x100,0,0x100,0x20) + InitializeFrontendDisplayModeArrows(0,1) | INLINE bake (slot0) + FLUSH | left col | always | ◄► arrow-capable (flags\|=2) |
| Button row 1 "SFX VOLUME" | CreateFrontendDisplayModeButton(SNK_SfxVolumeButTxt,…) + arrows(1,1) | INLINE+FLUSH | right col | always | mislabeled vs orig? text=SNK_SfxVolumeButTxt; actually drives master vol (see input) |
| Button row 2 "MUSIC VOLUME" | CreateFrontendDisplayModeButton(SNK_MusicVolumeButTxt,…) + arrows(2,1) | INLINE+FLUSH | left col | always | drives CD volume |
| Button row 3 "MUSIC TEST" | CreateFrontendDisplayModeButton(SNK_MusicTestButTxt,…) | INLINE+FLUSH | right col | always | no arrows; confirm→screen 19 |
| Button row 4 "OK" | CreateFrontendDisplayModeButton(SNK_OkButTxt,-0x100,0,0x60,0x20); g_frontendEscKeyButtonIndex=4 | INLINE+FLUSH | bottom center | always | ESC target |
| **SFX-mode icon** | QueueFrontendOverlayRect(...,src_y=(g_sfxPlaybackMode+4)*0x20, 0x40×0x20, g_soundOptionsMenuVolume) | FLUSH | x=center+0x4a, y=cy−0x8f | states 3..8 | source=Controllers.tga@0x466044; cell row=(mode+4)*32, 64×32. mode∈{0,1,2} |
| **Master-volume bar BG** | QueueFrontendOverlayRect(…,0xe0×0xc, g_soundOptionsMusicVolume) | FLUSH | x=center+0x4a, y=cy−0x37 | states 3..8 | empty box = VolumeBox.tga@0x46607c (g_soundOptionsMusicVolume) |
| **Master-volume bar FILL** | QueueFrontendOverlayRect(…, w=(masterVol%*0xde)/100 clamp 0xde, 10px, g_soundOptionsSfxVolume) | FLUSH | x=center+0x4b, y=cy−0x36 | states 3..8 && fill>0 | fill = VolumeFill.tga@0x466060 (g_soundOptionsSfxVolume); width∝g_persistedMasterVolumePercent |
| **CD/music-volume bar BG** | QueueFrontendOverlayRect(…,0xe0×0xc, g_soundOptionsMusicVolume) | FLUSH | x=center+0x4a, y=cy−0xf | states 3..8 | second VolumeBox |
| **CD/music-volume bar FILL** | QueueFrontendOverlayRect(…, w=(cdVol%*0xde)/100, 10px, g_soundOptionsSfxVolume) | FLUSH | x=center+0x4b, y=cy−0xe | states 3..8 && fill>0 | width∝g_persistedCdVolumePercent |
| SFX-mode NAME text box | dialog surface g_lobbyErrorDialogSurface(0xe0×0xa0): BltColorFillToSurface clear + MeasureOrCenterFrontendLocalizedString(SNK_SFX_Modes[mode],0x4a,0xe0)+DrawFrontendLocalizedStringToSurface | INLINE bake (state 4 & state 6 on change) | x=center+0x4a, y=cy−0x8f | baked when state==4 | the readable mode name under the icon |
| Selection/hover highlight bars | RenderFrontendDisplayModeHighlight @0x4263e0 (4 edge bars 0xc000) | FLUSH (decoupled) | around g_frontendButtonIndex slot | cursor not hidden | universal |

Primed contract globals: g_currentScreenIndex(header surf), g_soundOptionsMusicVolume(=VolumeBox.tga),
g_soundOptionsSfxVolume(=VolumeFill.tga), g_soundOptionsMenuVolume(=Controllers.tga icon sheet),
g_lobbyErrorDialogSurface(=mode-name dialog 0xe0×0xa0), g_frontendEscKeyButtonIndex=4,
g_returnToScreenIndex, g_sfxPlaybackMode, g_persistedMasterVolumePercent, g_persistedCdVolumePercent,
SetFrontendInlineStringTable(SNK_SfxOptions_MT) for header layout.

Animation: slide-in state 3 panels at base±anim*0x10, header X=cx-200, bar widths SCALED by
anim/0x27 (volume bars grow in as they slide), commit at anim==0x27. Slide-out state 8: panels exit at
anim*-8 / *6, dialog at +anim*0xc, commit at anim==0x10. No gallery on this screen.

Conditional elements: SFX-mode count is **3-mode vs 2-mode** — DXSound::CanDo3D() in state 6: if 0 →
mode wraps 0/1 (2 modes), else 0/1/2 (3 modes). The icon cell row and mode-name string index follow
g_sfxPlaybackMode accordingly. Volume FILL rects suppressed when computed width==0.

Input dispatch (state 6): if g_postRaceRacerCardNavDirection!=0 (arrow): idx0→g_sfxPlaybackMode +=dir
(wrap by CanDo3D), DXSound::SetPlayback(mode), goto state 4 (re-bake name); idx1→g_persistedMaster
VolumePercent +=dir*10 clamp[0,100], DXSound::SetVolume(pct*0xfc00/100&0xfc00); idx2→g_persistedCd
VolumePercent +=dir*10 clamp[0,100], DXSound::CDSetVolume(...). if g_frontendButtonPressedFlag: idx3→
g_returnToScreenIndex=TD5_SCREEN_MUSIC_TEST(19)+restore+state 7; idx4(OK)→g_returnToScreenIndex=
TD5_SCREEN_OPTIONS_HUB+restore+state 7.

Confidence: [CONFIRMED @ 0x0041ea90 full body; Controllers.tga@0x466044, VolumeBox.tga@0x46607c,
VolumeFill.tga@0x466060 confirmed via search_defined_strings].
[UNCERTAIN: SNK_SFX_Modes table contents/length not byte-read — indexed g_sfxPlaybackMode*4 at
SNK_SFX_Modes_exref; gated 2/3 entries by CanDo3D. Missing evidence: memory_read of the pointer table.]

---

### Screen 16 @0x00420400 — ScreenDisplayOptions  [interactive Y]
Inner states (plain switch): 0 init/load (button build is CONDITIONAL on CanFog, see below); 1,2
present/settle; 3 slide-in (5 sprites, commit anim==0x27); 4,5 static bake+double-present (state 4
bakes ALL value strings into the 0xe0×0x118 dialog); 6 interactive; 7 slide-out prep; 8 slide-out,
commit anim==0x10 → release → SetFrontendScreen.

ELEMENTS:
| element | produced by (fn/global/asset) | INLINE/FLUSH | dest pos | gating | notes |
|---|---|---|---|---|---|
| Full-screen menu background + secondary cache | LoadTgaToFrontendSurface16bpp(MainMenu.tga)+CopyPrimaryFrontendBufferToSecondary | INLINE (state 0) | (0,0) | always | |
| Header title ("DISPLAY OPTIONS") | CreateMenuStringLabelSurface(6)→g_currentScreenIndex | INLINE bake+FLUSH | animated X | always | |
| Button row 0 "RESOLUTION" | CreateFrontendDisplayModeButton(SNK_ResolutionButTxt,-0x120,0,0x120,0x20)+arrows(0,1) | INLINE+FLUSH | row | always | ◄► |
| Button row 1 "FOGGING" | **CanFog branch** (see Conditional) | INLINE+FLUSH | row | always (style differs) | preview-button when !CanFog |
| Button row 2 "SPEED READOUT" | CreateFrontendDisplayModeButton(SNK_SpeedReadoutButTxt,…)+arrows(2,1) | INLINE+FLUSH | row | always | ◄► (MPH/KPH) |
| Button row 3 "CAMERA DAMPING" | CreateFrontendDisplayModeButton(SNK_CameraDampingButTxt,…)+arrows(3,1) | INLINE+FLUSH | row | always | ◄► (0..9) |
| Button row 4 "OK" | CreateFrontendDisplayModeButton(SNK_OkButTxt,-0x120,0,0x60,0x20); g_frontendEscKeyButtonIndex=4 | INLINE+FLUSH | bottom | always | ESC target |
| **Resolution value text** | dialog g_lobbyErrorDialogSurface(0xe0×0x118): MeasureOrCenterFrontendLocalizedString(&g_displayModeStringTable + gConfiguredDisplayModeOrdinal*0x20, 10,0xe0)+Draw | INLINE bake (state 4) | dialog row 0 | always | value = current display-mode string (0x20 stride table) |
| **Fogging value text (ON/OFF)** | MeasureOrCenter(*(SNK_OnOffTxt + gFoggingConfigShadow*4),10,0xe0)+Draw | INLINE bake (state 4) | dialog row | **only if CanFog==1** | omitted entirely if !CanFog |
| **Speed-readout value text** | MeasureOrCenter(*(SNK_SpeedReadTxt + gSpeedReadoutUnitsConfigShadow*4),10,0xe0)+Draw | INLINE bake (state 4) | dialog row | always | MPH/KPH |
| **Camera-damping value text** | sprintf_game(local_4, g_uiFormatStringScratchTemplate) + MeasureOrCenter(local_4,10,0xe0)+Draw | INLINE bake (state 4) | dialog row | always | numeric 0..9 (g_cameraSpeedSetting) |
| Value dialog panel | QueueFrontendOverlayRect(center+0x4a, cy−0x8f, 0xe0×0x118, g_lobbyErrorDialogSurface) | FLUSH | right col | states 4..8 | holds the 3-4 value strings above |
| Selection highlight bars | RenderFrontendDisplayModeHighlight | FLUSH (decoupled) | active slot | cursor visible | universal |

Primed contract globals: g_currentScreenIndex, g_lobbyErrorDialogSurface(0xe0×0x118 value dialog),
g_frontendEscKeyButtonIndex=4, gConfiguredDisplayModeOrdinal, gFoggingConfigShadow,
gSpeedReadoutUnitsConfigShadow, g_cameraSpeedSetting, g_displayModeStringTable (0x20-stride mode names,
NUL-terminated list ending <0x4978bc), SetFrontendInlineStringTable(SNK_GfxOptions_MT).

Animation: slide-in state 3 sprites at base±anim*0x10, commit anim==0x27. Slide-out state 8 sprites at
anim*8 / *-8, dialog +anim*0xc, commit anim==0x10. No gallery.

Conditional elements: **FOG row CanFog gate** (DXD3D::CanFog() in state 0): if ==1 → normal
CreateFrontendDisplayModeButton(SNK_FoggingButTxt) + InitializeFrontendDisplayModeArrows(1,1) (toggle
with arrows) AND a value string is baked; if !=1 → CreateFrontendDisplayModePreviewButton (preview/
half-bright DISABLED look, NO arrows) AND the ON/OFF value string is NOT baked in state 4. The CanFog
check is repeated in state 4. Display-mode value uses gConfiguredDisplayModeOrdinal table-walk wrap.

Input dispatch (state 6, switch g_frontendButtonIndex when nav!=0): idx0→gConfiguredDisplayModeOrdinal
+=dir, wrap via g_displayModeStringTable scan (down to last non-empty / 0); idx1→gFoggingConfigShadow=
(shadow+dir)&1; idx2→gSpeedReadoutUnitsConfigShadow=(shadow+dir)&1; idx3→g_cameraSpeedSetting +=dir
clamp[0,9]; then state←4 (re-bake). Confirm: g_frontendButtonPressedFlag && idx==4 → g_returnToScreen
Index=TD5_SCREEN_OPTIONS_HUB + restore + state 7. (No idx-1 confirm path; fog row idx1 still cycles
even in preview mode? — only when CanFog==1 since arrows/flags|=2 gate nav targeting.)

Confidence: [CONFIRMED @ 0x00420400 full body; CanFog branch at state0 + state4 both present].
[UNCERTAIN: SNK_OnOffTxt / SNK_SpeedReadTxt / g_uiFormatStringScratchTemplate contents not byte-read;
camera-damping shown as a number via sprintf %-template. Missing: memory_read of those tables.]

---

### Screen 17 @0x00420c70 — ScreenTwoPlayerOptions  [interactive Y]
Inner states (plain switch): 0 init/load; 1,2 present/settle; 3 slide-in (3 sprites + split-mode icon,
commit anim==0x27); 4,5 static bake+double-present (state 4 bakes split-mode name + catchup number);
6 interactive; 7 slide-out prep; 8 slide-out, commit anim==0x10 → release → SetFrontendScreen.

ELEMENTS:
| element | produced by (fn/global/asset) | INLINE/FLUSH | dest pos | gating | notes |
|---|---|---|---|---|---|
| Full-screen menu background + secondary cache | LoadTgaToFrontendSurface16bpp(MainMenu.tga)+CopyPrimaryFrontendBufferToSecondary | INLINE (state 0) | (0,0) | always | |
| Header title ("2 PLAYER OPTIONS") | CreateMenuStringLabelSurface(6)→g_currentScreenIndex | INLINE bake+FLUSH | animated X | always | |
| Button row 0 "SPLIT SCREEN" | CreateFrontendDisplayModeButton(SNK_SplitScreenButTxt,-0x100,0,0x100,0x20)+arrows(0,1) | INLINE+FLUSH | left | always | ◄► toggle |
| Button row 1 "CATCHUP" | CreateFrontendDisplayModeButton(SNK_CatchupTxt,…)+arrows(1,1) | INLINE+FLUSH | right | always | ◄► 0..9 |
| Button row 2 "OK" | CreateFrontendDisplayModeButton(SNK_OkButTxt,-0x100,0,0x60,0x20); g_frontendEscKeyButtonIndex=2 | INLINE+FLUSH | bottom | always | ESC target |
| **Split-screen MODE icon** | QueueFrontendOverlayRect(…, src_y=g_twoPlayerSplitMode<<5, 0x40×0x20, g_twoPlayerOptionsSelection) | FLUSH | x=center+0x4a, y=cy−0x8f | states 3..8 | source=SplitScreen.tga@0x466094; cell row=mode*32 (horiz/vert split preview) |
| **Split-mode NAME text** | dialog g_lobbyErrorDialogSurface(0xe0×0x78): MeasureOrCenter(*(SNK_Split_Modes+mode*4),0x4a,0xe0)+Draw | INLINE bake (state 4) | dialog row 0 | always | mode name |
| **Catchup VALUE number** | sprintf_game(local_4, g_uiFormatStringScratchTemplate)+MeasureOrCenter(local_4,0x4a,0xe0)+Draw | INLINE bake (state 4) | dialog row 1 | always | g_twoPlayerCatchupAssist 0..9 |
| Value dialog panel | QueueFrontendOverlayRect(center+0x4a, cy−0x8f, 0xe0×0x78, g_lobbyErrorDialogSurface) | FLUSH | right | states 4..8 | |
| Selection highlight bars | RenderFrontendDisplayModeHighlight | FLUSH (decoupled) | active slot | cursor visible | universal |

Primed contract globals: g_currentScreenIndex, g_twoPlayerOptionsSelection(=SplitScreen.tga icon
sheet), g_lobbyErrorDialogSurface(0xe0×0x78 dialog), g_frontendEscKeyButtonIndex=2, g_returnToScreen
Index, g_twoPlayerSplitMode (0/1), g_twoPlayerCatchupAssist (0..9), SetFrontendInlineStringTable
(SNK_TwoOptions_MT).

Animation: slide-in state 3 sprites base±anim*0x10, split icon at iVar3+anim*-0x10+0x38c, commit
anim==0x27. Slide-out state 8 sprites at anim*8/*-8, icon at +anim*0x20, dialog +anim*0xc, commit
anim==0x10. No gallery.

Conditional elements: none beyond the split-mode<<5 icon-row selection (mode∈{0,1}, AND'd &1 in input).
Catchup is numeric only. Value strings baked only when state==4.

Input dispatch (state 6, when nav!=0): idx0→g_twoPlayerSplitMode=(mode+dir)&1; idx1→g_twoPlayer
CatchupAssist +=dir clamp[0,9]; then state←4. Confirm: g_frontendButtonPressedFlag && idx==2(OK) →
g_returnToScreenIndex=TD5_SCREEN_OPTIONS_HUB + restore + state 7.

Confidence: [CONFIRMED @ 0x00420c70 full body; SplitScreen.tga@0x466094 confirmed].
[UNCERTAIN: SNK_Split_Modes table contents/length and the catchup number format template not byte-read.
Missing: memory_read of SNK_Split_Modes_exref + g_uiFormatStringScratchTemplate.]

---

### Screen 18 @0x0040fe00 — ScreenControllerBindingPage  [interactive Y]  (pointer-dispatched header table @0x00410c84)
Inner states (in-body switch, multiple branches): 0 init/device-route; 9 slide-in (commit anim==0x1c);
**10 = interactive binding-edit (joystick/wheel)**; 0xb slide-out+exit; **0x13 keyboard slide-in**
(commit anim==0x1c); 0x14 keyboard-rebind enter (clears scancode table); **0x19 keyboard rebind header
draw** (one action label, ++); **0x1a keyboard live-capture** (scan 256 scancodes); 0x1b keyboard
slide-out+exit. ALL exits → SetFrontendScreen(TD5_SCREEN_CONTROL_OPTIONS=14).
Pointer-dispatched header handlers (table @0x410c84, 5×u32 → 0x410129/0x41043c/0x4104b2/0x410527/
0x410599), plus tail-call fragments 0x4100c0/ce/de/fa/111 (each draws a localized header line + creates
OK button + sets inner-state 9). These select WHICH device-class header string is drawn into the panel.
Device routing in state 0: active device byte g_player1DeviceDesc[deviceIndex]==3 → keyboard path
(state 0x13); else g_controllerBindingButtonCount = (deviceVal<4 ? 2 : deviceVal<9 ? deviceVal : 8);
hi-byte 0x600 = wheel/pedal class (different header text). present-bindings bitmask uVar12 (bit0 if any
cache slot==2, bit1 if any==3) selects header variant.

ELEMENTS:
| element | produced by (fn/global/asset) | INLINE/FLUSH | dest pos | gating | notes |
|---|---|---|---|---|---|
| Full-screen menu background + secondary cache | LoadTgaToFrontendSurface16bpp(MainMenu.tga)+CopyPrimaryFrontendBufferToSecondary | INLINE (state 0) | (0,0) | always | |
| Header title (screen name) | CreateMenuStringLabelSurface(6)→g_currentScreenIndex | INLINE bake+FLUSH | animated X | always | |
| Header/instruction text ("PRESS…","CONFIGURATION") | MeasureOrCenterFrontendLocalizedString(SNK_PressingTxt / SNK_ConfigurationTxt,0,0x1c0)+Draw into g_lobbyErrorDialogSurface | INLINE bake (state 0) | panel 0x1c0×0xd8 | non-keyboard | per-device-class variant via PTR table |
| Big binding panel | g_lobbyErrorDialogSurface = CreateTrackedFrontendSurface(0x1c0,0xd8); BltColorFillToSurface clear | INLINE alloc + FLUSH | center, queued in state 10/0xb | always | the per-row binding list |
| State strip panel | g_controllerBindingPage_state = CreateTrackedFrontendSurface(0x1c0,0x40); cleared | INLINE alloc + FLUSH | below panel | always | holds the action-label strip |
| **Live button-capture lights** | g_controllerBindingPage_inputCursor_PROVISIONAL = LoadFrontendTgaSurfaceFromArchive(ButtonLights.tga@0x464068); per-row QueueFrontendOverlayRect(iVar11, local_20, src_y=(pressed?0x10:0), 0x10×0x10) | INLINE load + FLUSH | x=cx−200 col, y per row +0x18 | state 10, per row | src_y=0x10 when (rowBitmask & g_controllerBindingCurrentButtons)!=0 → lit; row bit starts 0x40000, <<1 per row |
| Per-action binding row labels | local_10 = SNK_ButtonTxt with digit suffix ('1'+row) via local_10[len-2]=row+'1'; DrawFrontendLocalizedStringToSurface ×2 (label + assigned-action) into panel | INLINE bake (state 10 loop) | panel rows | state 10, count rows | row count = g_controllerBindingButtonCount |
| OK button | CreateFrontendDisplayModeButton(SNK_OkButTxt,-0x128,0,0x60,0x20) (in state 0 LAB_00410165 / header fragments) | INLINE+FLUSH | bottom | always | confirm |
| **Keyboard "PRESS KEY" prompt** | MeasureOrCenterFrontendLocalizedString(SNK_PressKeyTxt,0,0x1c0)+Draw | INLINE bake (state 0x14) | center | keyboard path | |
| **Keyboard action label** (the action being bound) | BltColorFillToSurface clear strip + MeasureOrCenter(SNK_ControlText + g_keyboardBindingProgressIndex*0x10,0,0x1c0)+Draw into g_controllerBindingPage_state | INLINE bake (state 0x19) | strip | keyboard path | 10 actions: LEFT/RIGHT/ACCEL/BRAKE/HANDBRAKE/HORN/GEARUP/GEARDOWN/VIEW/REARVIEW |
| Selection highlight bars | RenderFrontendDisplayModeHighlight | FLUSH (decoupled) | active slot | cursor visible | universal |

Primed contract globals: g_currentScreenIndex, g_lobbyErrorDialogSurface(0x1c0×0xd8 binding panel),
g_controllerBindingPage_state(0x1c0×0x40 strip), g_controllerBindingPage_inputCursor_PROVISIONAL
(=ButtonLights.tga capture indicator), g_controllerBindingActivePlayerSlot, g_controllerBinding
ActiveDeviceIndex (←g_player1/2InputSource), g_controllerBindingButtonCount, g_controllerBinding
CurrentButtons/PrevButtons/EdgeMask (joystick poll via DXInput::GetJS(devIdx-1)), g_controllerBindings
Cache_PROVISIONAL/_current_PROVISIONAL (stride 9 per slot), g_keyboardScanCodeTable + DAT_00464058/5c
+ g_controllerBindingScrollOffset_PROVISIONAL (10 keyboard scancodes), g_keyboardBindingProgressIndex.

Animation: slide-in state 9 / 0x13 header X=(cx−0x368)+anim*0x18, sprite slot 0 in, commit anim==0x1c.
Slide-out state 0xb / 0x1b header X=…+anim*-0x18, panels exit, commit anim==0x1c. Frame-count driven.

Conditional elements: **keyboard vs joystick branch** — device==3 → keyboard rebind flow (states
0x13/0x14/0x19/0x1a/0x1b, live scancode capture, no joystick panel); else joystick/wheel binding-edit
(state 10, ButtonLights capture row + per-row action labels). hi-byte 0x600 (wheel/pedal) selects a
different localized header string. Row count varies 2..8 by device. The OK button is created via one
of the PTR-table header fragments (0x410129 etc.) for non-default device classes.

Input dispatch:
- State 10 (joystick): edge mask = EdgeMask & CurrentButtons. If button count==2: on physical button
  0x40000 / 0x80000 held both frames → SWAP g_controllerBindings_current[slot*9] ↔ cache[slot*9]
  (toggle the two-button assignment). Else (count!=2): for each row, on row's button rising edge
  (PrevButtons & bit && bit & Current) → cache[row+slot*9]++ wrapping >10→2 (cycle the action assigned
  to that physical button). Confirm: g_frontendButtonPressedFlag && idx==0 → anim=0, ++state (0xb).
- State 0x1a (keyboard live-capture): scan scancodes 0..0xff via DXInput::CheckKey, skip any already in
  g_keyboardScanCodeTable/DAT_00464058/5c/scrollOffset (dedup), first new pressed → write to
  g_keyboardScanCodeTable[progressIndex], DXSound::Play(3), progressIndex++; if !=10 → state 0x19
  (next action), else → commit (LAB_00410b3a: anim=0, ++state → 0x1b slide-out).
- ESC funnels through OK (idx0) via loop step 14.

Confidence: [CONFIRMED @ 0x0040fe00 full body + 0x410c84 PTR table (per flow_model appendix) +
ButtonLights.tga@0x464068 + Controllers/SNK_ControlText 10-action list (per MEMORY 0x100075E0)].
[UNCERTAIN: SNK_ButtonTxt label text + SNK_PressingTxt/ConfigurationTxt/PressKeyTxt contents not
byte-read; the exact device-desc byte values for hi-byte 0x600 wheel class. Missing: memory_read of
those SNK exrefs + g_player1DeviceDesc table.]

---

### Screen 19 @0x00418460 — ScreenMusicTestExtras  [interactive Y]   ⭐ DECOUPLED ALBUM ART
Inner states (plain in-body switch 0..8; NO external jump table — caseD_6/7/a belong to idx22/idx6 per
flow_model §4 CORRECTION): 0 init (GATED on gallery cross-fade phase wind-down; loads BAND gallery);
1,2 present/settle; 3 slide-in (2 sprites, commit anim==0x27); 4,5 static (queue track-# box +
now-playing box, ++); 6 interactive; 7 slide-out prep (resets phase=0x40); 8 slide-out, commit
anim==0x20 → release → ReleaseExtrasGalleryImageSurfaces + **LoadExtrasGalleryImageSurfaces** (restore
the random slideshow images) → SetFrontendScreen.

ELEMENTS:
| element | produced by (fn/global/asset) | INLINE/FLUSH | dest pos | gating | notes |
|---|---|---|---|---|---|
| Full-screen menu background + secondary cache | LoadTgaToFrontendSurface16bpp(MainMenu.tga)+CopyPrimaryFrontendBufferToSecondary | INLINE (state 0) | (0,0) | always | |
| Header title ("MUSIC TEST") | CreateMenuStringLabelSurface(6)→g_currentScreenIndex | INLINE bake+FLUSH | animated X | always | |
| **ALBUM / BAND COVER ART** | UpdateExtrasGalleryDisplay @0x40d830 → CrossFade16BitSurfaces @0x40d190; slide = (&g_extrasGallerySlideSurfaces)[ LUT[g_attractCdTrackCandidate] ]; LUT@0x465e4c = `01 03 04 04 02 00 00 01 03 04 04 04` (12 bytes, track→band 0..4) | **FLUSH (DECOUPLED)** — NO screen-fn call | dest (0x76,0x8c)=(118,140) | g_frontendScreenTransitionFlag==2 (band gallery) && g_extrasGallerySlideSurfaces!=0 | THE prior-pass miss. 5 band TGAs loaded by LoadExtrasBandGalleryImages (Fear_Factory/Gravity_Kills/Junkie_XL/KMFDM/PitchShifter); CROSSFADES when track's band index changes (erase old via BltFast, phase 0x100→halve→floor -0x40) |
| Button row 0 "SELECT TRACK" | CreateFrontendDisplayModeButton(SNK_SelectTrackButTxt,-0x120,0,**0xa0**,0x20)+arrows(0,1) | INLINE bake+FLUSH | left, narrow (0xa0=160px) | always | the SHORT ◄► track selector; small-ish button, ◄► arrow-capable |
| Button row 1 "OK" | CreateFrontendDisplayModeButton(SNK_OkButTxt,-0x120,0,0x60,0x20); g_frontendEscKeyButtonIndex=1 | INLINE+FLUSH | bottom | always | ESC target |
| **TRACK-# box** ("%d. %s") | g_lobbyErrorDialogSurface=CreateTrackedFrontendSurface(0x170,0x28); BltColorFillToSurface clear + sprintf_game(buf, "%d. %s"@0x465f74) + MeasureOrCenter(buf,0,0x170)+Draw | INLINE bake (state 0 + re-baked on ◄► in state 6) | x=cx−0x32, y=cy−0x8f, 0x170×0x28 | states 4..8 | shows "<n>. <song title>" (track number + title) |
| **NOW-PLAYING box** | g_musicTestSelectedTrackId=CreateTrackedFrontendSurface(0x170,0x78); BltColorFillToSurface clear + MeasureOrCenter(SNK_NowPlayingTxt)+Draw, then band name (&PTR_s_GRAVITY_KILLS_00465e1c)[track]+Draw, then song title (&PTR_s_FALLING_00465e58)[track]+Draw | INLINE bake (state 0 + re-baked on SELECT confirm in state 6) | x=cx−0xc, y=cy−0x3f, 0x170×0x78 | states 4..8 | 3 lines: "NOW PLAYING" + BAND + TITLE. PTR arrays = 12 entries each (CD track 0..0xb) |
| Selection highlight bars | RenderFrontendDisplayModeHighlight | FLUSH (decoupled) | active slot | cursor visible | universal |

Primed contract globals (album-art contract): **g_extrasGallerySlideSurfaces** (5 band TGAs, loaded by
LoadExtrasBandGalleryImages in state 0), **g_extrasGalleryCrossFadePhase** (gated entry: state 0 folds
0x100−phase / clamps 0x40 / waits until <−0xf before loading; reset to 0x40 on exit), **g_attractCd
TrackCandidate** (=g_selectedCdTrackIndex; the LUT index for which band slides in), **g_selectedCd
TrackIndex** (0..0xb track selector), g_frontendScreenTransitionFlag==2 (set by LoadExtrasBandGallery
Images), g_extrasGalleryPreviousSlideIndex_PROVISIONAL (crossfade prev-band compare), g_currentScreen
Index, g_lobbyErrorDialogSurface(track-# box), g_musicTestSelectedTrackId(now-playing box),
g_frontendEscKeyButtonIndex=1, g_returnToScreenIndex, SetFrontendInlineStringTable(SNK_MusicTest_MT).

Animation: state 3 slide-in 2 sprites base±anim*0x10/*-0x10, commit anim==0x27. State 8 slide-out
sprite0 at anim*-8, sprite1 at +anim*6/*0x30, commit anim==0x20. **Album-art crossfade (decoupled):**
UpdateExtrasGalleryDisplay transition==2 path — on band change set phase=0x100, erase old slide via
BltFast; each frame phase = phase/2, derive two blend weights uVar1/uVar3 (clamped ≤0x20=32 from the
phase band), CrossFade16BitSurfaces if enabled else plain Copy; decrement phase toward floor −0x40.

Conditional elements: album art only when transition==2 AND g_extrasGallerySlideSurfaces!=0 (else fn
no-ops). The cross-fade vs plain-copy branch keys on g_extrasGalleryEnabledFlag_PROVISIONAL (CPUID MMX
bit). Track-# box re-baked only on ◄► (nav); now-playing box re-baked only on SELECT TRACK confirm.

Input dispatch (state 6): if nav==0: if pressed: idx0(SELECT TRACK)→DXSound::CDPlay(g_selectedCdTrack
Index+2,1), g_attractCdTrackCandidate=g_selectedCdTrackIndex (THIS changes which band art slides in),
re-bake now-playing box (NOW PLAYING + band + title), return; idx1(OK)→g_returnToScreenIndex=TD5_
SCREEN_SOUND_OPTIONS(15)+restore+phase=0x40+state 7. Else (nav!=0) && idx==0: g_selectedCdTrackIndex
+=dir wrap[0,0xb], re-bake track-# box ("%d. %s"), return (does NOT change playing track until SELECT).

Confidence: [CONFIRMED @ 0x00418460 full body + 0x40d830 gallery draw + LUT@0x465e4c byte-read
`010304040200000103040404` + format "%d. %s"@0x465f74 byte-read + PTR arrays @0x465e1c (band names:
GRAVITY KILLS/FEAR FACTORY/JUNK…) and @0x465e58 (song titles: FALLING…) byte-read, 12 entries each].
[UNCERTAIN: full enumeration of all 12 band-name / 12 song-title strings not dumped (only first few
read); the band-art crossfade is driven by the decoupled flush, confirmed reachable via LOOP→FLUSH.]
# TD5 Frontend Screens 20–24 — Complete Per-Screen Element + Behavior Spec

RE target: `TD5_d3d.exe` (image base `0x00400000`). All addresses are Ghidra VAs.
Source: literal full decompilation of each screen fn + its helper(s) + grounding reads of
the referenced data tables/strings. Hex AND decimal. Every claim carries an address.

**Cross-cutting model (from `frontend_rendering_model.md` / `frontend_flow_model.md`):**
Screen fns run at LOOP step 6 and only **PRIME** state — they enqueue overlay rects via
`QueueFrontendOverlayRect 0x425660`, move sprite slots via `MoveFrontendSpriteRect 0x4259d0`,
bake label/panel surfaces, and build button slots. The actual on-screen pixels are emitted by
the per-frame **FLUSH** (`FlushFrontendSpriteBlits 0x425540`) which drains the overlay
double-buffer into `g_primaryWorkSurface`, then unconditionally runs `UpdateExtrasGalleryDisplay`
+ `RenderFrontendDisplayModeHighlight` (the selection edge-bar outline), then the cursor sprite
list. So below, an element queued via `QueueFrontendOverlayRect` / placed via `MoveFrontendSpriteRect`
is marked **FLUSH** (the screen primes, the flush draws); an element drawn straight into a
work/panel surface inline (Fill*, Draw*StringToSurface, BltColorFillToSurface, Load*16bpp,
Blit/Present*) is **INLINE**.
`g_frontendAnimFrameCounter` (++ once/frame by LOOP) drives every slide; transitions fire on
exact counter equality. `g_postRaceRacerCardNavDirection` = ◄/► cycle delta (−1/+1);
`g_frontendButtonPressedFlag` + `g_frontendButtonIndex` = confirm-this-frame edge.

---

### Screen 20 @0x0040dfc0 — CarSelectionScreenStateMachine  [interactive Y]
Inner states (`switch(g_frontendInnerState)`, in-body, no external jump table): 0 init/clamp-selection,
1 reset-anim, 2 slide-in of three CarSel TGA strips, 3 present→copy-to-secondary, 4 build buttons +
release the 3 intro strips, 5 slide-in panels/header (commit at counter==0x18=24), 6 settle
(`AdvanceFrontendTickAndCheckReady`→state 10), 7 **interactive**, 8 re-enter from preview (counter==2→7),
10 redraw + reload car pic, 0xb/0x15 generic slide-step, 0xc load CarPic + name + Beast/Beauty/Locked tag,
0xd/0xe car-swap settle, 0xf CONFIG stat-panel bake (manual transmission view), 0x10/0x11/0x12 INFO stat-panel
bake, 0x14 enter-preview clear, 0x16/0x17 leave-preview restore, 0x18 commit selection + pick next-screen
TGA, 0x19 slide-out wipe (cached-rect band), 0x1a release + branch to next screen.

ELEMENTS:
| element | produced by (fn/global/asset) | INLINE/FLUSH | dest pos | gating | notes |
|---|---|---|---|---|---|
| 3D/preview CAR image panel | `DrawCarSelectionPreviewOverlay 0x40ddc0` → `QueueFrontendOverlayRect(..,0x198,0x118,0x5a, g_carSelectionFrameAccumulator)` | FLUSH | bottom-right (canvasW−0x198, canvasH−0x164)=(640−408,480−356); 0x198×0x118 (408×280) | gated `g_carSelectionPreviewFrameIndex==0 && g_carSelectionFrameAccumulator!=0` | NOT a 3D render-to-surface — it is a **pre-rendered 2D car TGA** `CarPic%d.tga` (state 0xc, `sprintf s_CarPic_d_tga_00463f08`) loaded from the per-car zip `(&gCarZipPathTable)[gExtCarIdToTypeIndex[selectedCar]]`. Slides in from right when state==0xb (`anim*0x20`), out when state==0xe (`canvasW + anim*-0x40 + 0x4a8`). color-key 0x5a=90. |
| Car name text | state 0xc: `sprintf s_%s_%s_00463f00` ("%s %s" = manuf+model), `MeasureOrCenterFrontendLocalizedString`→`DrawFrontendLocalizedStringPrimary` | INLINE | centered, y=(canvasH/2−0x97) | always (state 0xc/0xf) | written to `g_primaryWorkSurface`, then `BlitFrontendCachedRect` to compose. Name re-drawn each car-swap. |
| Beast/Beauty tag | state 0xc: `DrawFrontendLocalizedStringPrimary(SNK_BeastTxt_exref / SNK_BeautyTxt_exref, cx−0xea, cy−0x77)` | INLINE | (canvasW/2−0xea, canvasH/2−0x77) | only `g_frontendSelectedGameType==2` (Beauty&Beast mode); `<8`→Beauty else Beast | SNK_BeastTxt@0x00460b10. |
| Locked tag | state 0xc: same call w/ `SNK_LockedTxt_exref` | INLINE | (canvasW/2−0xea, canvasH/2−0x77) | car locked (`g_savedCarLockTable[car]!=0`) && cheat off && gametype∉{8,5} | mutually-excl with Beast/Beauty. |
| Header label (car-select title) | `CreateMenuStringLabelSurface(0xb/0xc/0xd)` baked → `g_currentScreenIndex`; queued each frame at animated X | INLINE bake / FLUSH draw | top, `(cx−0x110, sy−hdrYoff−0x40)` static; slides in state 5 (`anim*5`) | label id 0xb 1P / 0xc 2P-P2 / 0xd championship-return | surface w/h = `g_menuHeaderLabelSurfaceWidth/Height`. |
| CarSelBar1 strip (right vertical bar) | state 0: `g_carSelectionConfirmStateMachine = LoadFrontendTgaSurfaceFromArchive(CarSelBar1.tga, FrontEnd.zip)`; queued state 2 | INLINE load / FLUSH draw | x = canvasW − anim*8; 0x18×0x198 (24×408) | intro only (released state 4) | despite var name it is the bar surface. |
| CarSelCurve strip | state 0: `g_carSelectionPreviewActive = Load…(CarSelCurve.tga)`; queued state 2 | INLINE load / FLUSH draw | x=canvasW−anim*8, y=0x198; 0x50×0x38 | intro only | released state 4. |
| CarSelTopBar strip | state 0: `g_carSelectionPaintIndex = Load…(CarSelTopBar.tga)`; queued state 2 | INLINE load / FLUSH draw | (slide-in x, y=0x2d); 0x214×0x24 (532×36) | intro only | released state 4. |
| GraphBars stat-bar atlas | state 0: `_DAT_0048f35c = Load…(GraphBars.tga)` | INLINE load | n/a (source atlas) | always loaded | source page for stat bars (consumed in CONFIG/INFO bakes). |
| "Car" selector button (slot 0) | state 4 `CreateFrontendDisplayModeButton(SNK_CarButTxt, -0xa8,0,0xa8,0x20,0)` + `InitializeFrontendDisplayModeArrows(0,1)` | INLINE bake / FLUSH draw | slid in state 5/0x18; 0xa8×0x20 (168×32) | always | ◄► arrow-capable (flags\|=2); cycles `g_quickRaceSelectedTrackId` (the SELECTED CAR id). |
| "Paint" selector button (slot 1) | state 4 `CreateFrontendDisplayModeButton(SNK_PaintButTxt,…)` + `InitializeFrontendDisplayModeArrows(1,1)` | INLINE bake / FLUSH draw | 0xa8×0x20 | always | ◄► cycles `g_carSelectPaintSchemeTransient` (wrap 0..3). |
| "Config" selector button (slot 2) | state 4 `CreateFrontendDisplayModeButton(SNK_ConfigButTxt,…)` | INLINE bake / FLUSH draw | 0xa8×0x20 | always | press/◄► → state 0xf (rebake CONFIG stat panel); cycles `g_carSelectWheelSchemeTransient` (wrap 0..3). |
| Transmission button (slot 3) | state 4: `CreateFrontendDisplayModeButton(SNK_AutoButTxt)` if gametype!=7, else `CreateFrontendDisplayModePreviewButton(SNK_ManualButTxt)` | INLINE bake / FLUSH draw | 0xa8×0x20 | always | press toggles `g_carSelectManualTransmissionToggle ^=1` + `RebuildFrontendButtonSurface(3)` (Auto↔Manual label). |
| OK button (slot 4) | state 4 `CreateFrontendDisplayModeButton(SNK_OkButTxt,-0x40,0,0x40,0x20,0)` | INLINE bake / FLUSH draw | 0x40×0x20 (64×32) | always | confirm→state 0x14. If car locked & not cheat & gametype∉{8,5}: `DXSound::Play(10)` reject. |
| BACK button (slot 5) | state 4 `CreateFrontendDisplayModeButton(SNK_BackButTxt,-0x60,0,0x60,0x20,0)` | INLINE bake / FLUSH draw | 0x60×0x20 | only if `g_postRaceRestartSelectedRace==0 && g_mainMenuFlowPhase==0` | present→ESC index 5 (`g_frontendEscKeyButtonIndex=5`); else esc index=4 (OK). |
| CONFIG stat panel (transmission specs) | state 0xf: loop `SNK_Config_Hdrs_exref` (header labels, '*'=skip) + per-car values `g_localizationCupTitleScratch / TrackNameScratch / OptionLabelScratch + DAT_0049b7bc` via `DrawFrontendSmallFontStringToSurface(…, g_secondaryWorkSurface)` | INLINE | grid into `g_secondaryWorkSurface`, then presented | when slot 2/3 toggled (state 0xf) | per-car index = `gExtCarIdToTypeIndex[selectedCar]`; rows stride 0x30/0x330. |
| INFO stat panel (stat values) | state 0x11: loop `SNK_Info_Values_exref` (0..0x28 step 4) `MeasureOrCenterFrontendString`→`DrawFrontendSmallFontStringToSurface(g_secondaryWorkSurface)` | INLINE | column at `uVar1`, rows y+=0xc | INFO sub-mode (`g_carSelectionPreviewFrameIndex` 1/2) | 10 value rows. |
| Selection highlight (edge bars) | `RenderFrontendDisplayModeHighlight 0x4263e0` (decoupled) | FLUSH (decoupled) | around selected slot in `g_connBrowserListOriginX[]` | `tail!=-1 && cursorOverlay==0` | color 0xc000, 4 bars, drawn to `g_frontendBackSurfacePtr`. |
| Mouse cursor | LOOP queues `g_frontendCursorTextureId` | FLUSH | mouse pos; 0x16×0x1e | `cursorOverlayHidden==0 && mouseEnabled` | shared. |

Primed contract globals: **`g_quickRaceSelectedTrackId`** = the live SELECTED-CAR index in this screen (re-used name; committed to `g_selectedCarIndex` / `DAT_00463e08` (P2) at state 0x18); `g_carSelectPaintSchemeTransient` (paint 0..3 → `g_player1/2SelectedPaintScheme`); `g_carSelectWheelSchemeTransient` (config/wheel 0..3 → `g_player1/2SelectedWheelScheme`); `g_carSelectManualTransmissionToggle` (0=Auto/1=Manual → `_g_player1ManualTransmission` / `g_player2AutoPauseLatch`); `g_carSelectionFrameAccumulator` = current CarPic preview surface; `_DAT_0048f35c` GraphBars atlas; `g_carSelectionConfirmStateMachine/PreviewActive/PaintIndex` = the 3 intro-strip surfaces (mis-named); `g_carSelectionPreviewFrameIndex` (0=car-pic view, 1/2=INFO/CONFIG stat view); `g_currentScreenIndex` = header label surface.

Animation: 8px/frame slide for intro strips (state 2, ends when `anim==(canvasW−0x20)>>3`); state 5/0x18 panel slide-in `sx + anim*-0x20 + 0x308` over **24 frames** (commit `anim==0x18`); header label X `cx−0x110 + anim*5`; CarPic slides in `anim*0x20` (state 0xb), out `anim*-0x40` (state 0xe, commit anim==0x19=25); slide-out state 0x19 = 0x18-px/frame cached-rect band wipe.

Conditional elements: BACK button only when not post-race-restart and not mid-menu-flow; Transmission button = Auto (normal) vs Manual (preview-style) by `gametype!=7`; Beast/Beauty only gametype 2; Locked tag only when locked & cheat off; car-index clamp ranges differ per gametype (5=Masters scans `g_savedCarState_base`/`mastersEncounterFlags`; 8=fixed 0x21..0x24; 2=0..0xf; else `g_savedMaxUnlockedCar` or 0x20 if cheat).

Input dispatch: slot0 ◄►→cycle car id (gametype-specific wrap), slot1 ◄►→paint, slot2 press/◄►→config (state 0xf), slot3 press→transmission toggle, slot4(OK) press→commit (state 0x14→0x18→0x1a), slot5(BACK) press→`g_returnToScreenIndex=6` exit. `g_mainMenuFlowPhase==2`→abort to state 0x14. Next-screen routing at state 0x1a keys off `g_mainMenuButtonHint_PROVISIONAL` (1/2/3/4) + gametype → TrackSelection / RaceResults / RaceTypeMenu / NetworkLobby / CreateSession.

Confidence: [CONFIRMED @ 0x0040dfc0 full body, 0x0040ddc0 preview helper, strings @0x463f00 "%s %s"/0x463f08 CarPic%d.tga/0x460b10 SNK_BeastTxt]. [UNCERTAIN: the CONFIG/INFO per-car stat *value* source arrays (`g_localizationCupTitleScratch_PROVISIONAL`, `&DAT_0049b7bc`, `SNK_Info_Values`) are decompiler-named scratch buffers populated by the localization/car-data loader elsewhere — their exact fill-site not traced here; the GraphBars.tga stat-BAR rendering (vs text) was not observed being blitted in states 0xf/0x11, which draw text only — bar-graph consumption of `_DAT_0048f35c` is not in this fn body.]

---

### Screen 21 @0x00427630 — TrackSelectionScreenStateMachine  [interactive Y]
Inner states (in-body switch, 0..8): 0 init (clamp schedule idx + load TrackSelect bg + buttons),
1/2 present/settle, 3 slide-in (commit `anim==0x27`=39 via tick-ready→state 5), 4 **interactive**,
5 build the track name-panel + load the track preview TGA (→state 8), 6 exit-transition present,
7 slide-out (`anim==0x27` release+`SetFrontendScreen`), 8 preview/name slide-in settle (`anim==0x10`=16→state 4).

ELEMENTS:
| element | produced by (fn/global/asset) | INLINE/FLUSH | dest pos | gating | notes |
|---|---|---|---|---|---|
| Track map/preview image | state 5: `sprintf s_Front_End_Tracks__s_tga_004669d8` ("Front End\Tracks\%s.tga") → `DAT_004a2c94 = LoadFrontendTgaSurfaceFromArchive(.., Tracks.zip 0x4669bc)`; queued states 4/7/8 | INLINE load / FLUSH draw | state4 (cx+0x5c, cy−0x69); 0x98×0xe0 (152×224) | always (per selected track) | the per-track preview thumbnail; reloaded on every track cycle. Released state 5(reload)/7(exit). |
| Track name + locked text panel | state 5: `g_lobbyErrorDialogSurface` (mis-named, the name panel; `CreateTrackedFrontendSurface(0x128,0xb8)` in state 0) cleared, then track name copied from `SNK_TrackNames_exref[gScheduleToPoolIndex[idx]]` (split on ',') + `DAT_004658e4` subtitle via `MeasureOrCenterFrontendLocalizedString`→`DrawFrontendLocalizedStringToSurface`; + SNK_LockedTxt if locked | INLINE bake / FLUSH draw | panel 0x128×0xb8 (296×184); queued (cx+0x18, iVar5) + lower band | always | if idx<0 only the `DAT_004658e4` subtitle drawn. |
| Header label (track-select title) | state 0 `CreateMenuStringLabelSurface(10)`→`g_currentScreenIndex`; queued each frame | INLINE bake / FLUSH draw | top (cx−200, …); slides state 3 (`anim*4`) | always | |
| "Track" selector button (slot 0) | state 0 `CreateFrontendDisplayModeButton(SNK_TrackButTxt,-0xe0,0,0xe0,0x20,0)` + `InitializeFrontendDisplayModeArrows(0,1)` | INLINE bake / FLUSH draw | 0xe0×0x20 (224×32) | always | ◄►-capable; cycles `g_selectedScheduleIndex` (skips disabled cup rounds when gametype>7). |
| Direction toggle button (slot 1) | state 0 `CreateFrontendDisplayModeButton(SNK_ForwardsButTxt,-0xe0,0,0xe0,0x20,0)` | INLINE bake / FLUSH draw | 0xe0×0x20 | **only positioned when track is reverse-capable** | press toggles `_g_selectedTrackDirection ^=1` + `RebuildFrontendButtonSurface(1)` (Forwards↔Reverse label). When not reverse-capable (`gNpcRacerCheatFlags[idx]==0` or idx<0) the slot is moved off-screen to x=−0xe0. |
| OK button (slot 2) | state 0 `CreateFrontendDisplayModeButton(SNK_OkButTxt,-0xe0,0,0x60,0x20,0)` | INLINE bake / FLUSH draw | 0x60×0x20 | always | confirm: locked track → `DXSound::Play(10)` reject; else `g_returnToScreenIndex=~0` →state 6 exit. |
| BACK button (slot 3) | state 0 `CreateFrontendDisplayModeButton(SNK_BackButTxt,-0xe0,0,0x70,0x20,0)` | INLINE bake / FLUSH draw | 0x70×0x20 | only if `g_mainMenuButtonHint_PROVISIONAL!=2` | present→esc index 3; else esc index 2 (OK). press→`g_returnToScreenIndex=CarSelection` state 6. |
| Selection highlight | `RenderFrontendDisplayModeHighlight 0x4263e0` | FLUSH (decoupled) | selected slot | tail!=-1 | color 0xc000. |
| Mouse cursor | LOOP | FLUSH | mouse pos | overlay==0 | shared. |

Primed contract globals: **`g_selectedScheduleIndex`** = live selected track/schedule index (cycled by ◄►); **`_g_selectedTrackDirection`** = 0 forward / 1 reverse (toggled by slot 1, reset 0 on every track change & state 0); `DAT_004a2c94` = track preview surface; `g_lobbyErrorDialogSurface` (re-used name) = the 0x128×0xb8 track-name panel surface; `g_currentScreenIndex` = header label; `g_returnToScreenIndex` (=~0 OK / =CarSelection BACK) decides exit target.

Animation: state 3 slide-in over 39 frames (counter pinned at 0x26 while waiting for `AdvanceFrontendTickAndCheckReady`); buttons slide `anim*0x10`/`anim*-0x10`; header X `anim*4`; state 5 name-panel build is 2-frame (anim 1 = text, anim 2 = preview load); state 7 slide-out 39 frames; state 8 re-settle 16 frames.

Conditional elements: Direction toggle is the **reverse-capable gate** — slot 1 only sits on-screen when `gNpcRacerCheatFlags[g_selectedScheduleIndex]!=0` (track flagged reverse-capable) AND idx≥0; otherwise it is parked at x=−0xe0 (off-screen). BACK button hidden when `g_mainMenuButtonHint_PROVISIONAL==2` (came from QuickRace). Schedule-index cycling skips cup rounds whose `g_npcRacerGroupTable[idx*0xa4]&3` set (when gametype>7), and skips locked entries; cheat (`g_cheatPostRaceHighScoreUnlock`) widens the range to 0..0x12.

Input dispatch: slot0 ◄►→`g_selectedScheduleIndex += dir` (with skip-disabled/locked wrap, then reset direction + rebuild slot1 + reposition + state 5 reload); slot1 press→direction toggle (only if reverse-capable); slot2(OK) press→commit (locked→reject sound 10) state 6; slot3(BACK) press→return CarSelection state 6.

Confidence: [CONFIRMED @ 0x00427630 full body; strings @0x4669d8 "Front End\Tracks\%s.tga", @0x4669bc Tracks.zip, @0x463ee4 TrackSelect.tga, @0x461cb2 SNK_TrackSel_Ex]. [UNCERTAIN: city/country split — the track name is `SNK_TrackNames[..]` copied until ',' (comma) into `local_80`; the comma-delimited remainder (likely country/subtitle) is replaced by the `DAT_004658e4` format string, so a separate country line is NOT independently rendered here — `&DAT_004658e4` content not resolved (a format template); the `gScheduleToPoolIndex` / `SNK_TrackNames` table contents not dumped.]

---

### Screen 22 @0x00417d50 — ScreenExtrasGallery  [interactive Y — but only ESC/any-key/end-of-credits → QUIT]
Inner states (computed jump `JMP [ECX*4 + 0x00418390]`, 0..7): 0 wait for gallery cross-fade phase
< −0xf (clamps phase), 1 **load 22 dev-team mugshots + 5 Legals surfaces**, 2/3/4/5 idle-advance (1 frame each),
6 = `caseD_6 @0x41808d`: `LockSecondaryFrontendSurfaceFillColor(0)` (clear secondary) + set
`g_frontendAnimFrameCounter=0x27f` (639), 7 = the **scrolling credits/mugshot reel** (vertical wrap-scroll).

ELEMENTS:
| element | produced by (fn/global/asset) | INLINE/FLUSH | dest pos | gating | notes |
|---|---|---|---|---|---|
| Scrolling credits viewport (2 wrap halves) | state 7: two `QueueFrontendOverlayRect(0xcc,0x60,…, g_secondaryWorkSurface)` reading the scrolled secondary surface; split at the 0x140 wrap seam | FLUSH | viewport at (0xcc=204, 0x60=96), 0x140×0x140 (320×320) | always (state 7) | vertical scroll position = `g_frontendAnimFrameCounter` (wraps mod 0x280=640); the reel content lives in `g_secondaryWorkSurface`, scrolled by sampling at moving y. |
| Credit text line | state 7, when `(anim & 0x1f)==0` and `SNK_CreditsText[page*0x18]!='#'`: `MeasureOrCenterFrontendLocalizedString(SNK_CreditsText + page*0x18, …)`→`DrawFrontendLocalizedStringToSurface` after `FillSurfaceRectWithColor(0,…,0x140,0x20)` clears the row | INLINE | into secondary at the wrap-row (y or y−0x140), 0x140×0x20 (320×32) row | per 0x20-scroll step | `SNK_CreditsText` is `[0x18]`-stride char array (`@@3PAY0BI@` confirms 0x18=24 B/record). End-of-group marker = next record first byte `*`(0x2a) → `DAT_00496354++` (page-group counter). |
| Mugshot image row | state 7, same gate, when `SNK_CreditsText[page*0x18]=='#'`(0x23): blit mugshot surface `(&DAT_004961dc)[CreditsText[page*0x18+1]*4]` (a pointer into the `_DAT_004962e0..0x496334` mugshot surface array) via surface vtable `+0x1c` BltFast(0x11 keyed) into secondary | INLINE (direct BltFast) | secondary at the wrap-row, 0x140 wide | mugshot record ('#') | 7 mugshots per row group (`g_extrasGalleryAssetHandle` 0..7), then `g_extrasGalleryPage++`. |
| 22 dev-team mugshot surfaces | state 1: `_DAT_004962e0.._00496334 = LoadTgaToFrontendSurfaceFromArchive(Front End\Extras\<name>.tga, Mugshots.zip 0x465df0)` — Bob, Gareth, Snake, MikeT, Chris, Headley, Steve, Rich, Mike, Bez, Les, TonyP, JohnS, DavidT, TonyC, DaveyB, ChrisD, Slade, Matt, Marie, JFK, Daz | INLINE load | n/a (source surfaces) | loaded once (state 1) | uses the 0→1 pixel-substitute loader 0x4122f0. |
| 5 Legals surfaces | state 1: `_DAT_00496338.._00496348 = LoadFrontendTgaSurfaceFromArchive(Front End\Extras\Legals1-5.tga, Mugshots.zip)` | INLINE load | n/a | loaded once | legal/copyright pages. |

Primed contract globals: `g_extrasGalleryPage` (current credit-record index, ×0x18 stride into `SNK_CreditsText`); `g_extrasGalleryAssetHandle` (0..6 mugshot column counter); `g_frontendAnimFrameCounter` repurposed as the **scroll position** (0..0x280=640, wraps); `DAT_00496354` (page-GROUP counter; ==0xb=11 → quit); `g_secondaryWorkSurface` = the off-screen reel; mugshot surfaces `_DAT_004962e0..0x496348`.

Animation: pure scroll — `g_frontendAnimFrameCounter` advances 1/frame (LOOP), wraps at 0x280. Two overlay-rect halves stitch across the 0x140 (320) seam so the reel loops seamlessly. New rows are baked into the secondary every 0x20 (32) scroll units.

Conditional elements: row type chosen per record by first byte: `#`(0x23)=mugshot, `*`(0x2a)=group-end (page-group++), else = centered localized text. No band-cover slideshow here. NOTE: this is **distinct** from the `UpdateExtrasGalleryDisplay 0x40d830` band-gallery slideshow (that decoupled flush draw uses `g_extrasGallerySlideSurfaces` band covers at (0x76,0x8c), driven by `g_attractCdTrackCandidate` + LUT@0x465e4c, and is set up by the **music-test** screen 19's `LoadExtrasBandGalleryImages`, NOT by screen 22). Screen 22 = the dev-credits + legal-pages scroll reel.

Input dispatch: state 7 — `(g_frontendInputEdgeBits & 0x40000)!=0` (any of the masked keys) OR `g_frontendMouseEdgeBits!=0` (any mouse button) OR `DAT_00496354==0xb` (credits finished) → `DXWin::CleanUpAndPostQuit()` (quits the game; per fn comment "ESC in credits always exits the game"). No menu buttons, no per-element hit-testing.

Confidence: [CONFIRMED @ 0x00417d50 full body + jump table @0x418390 idx6=0x41808d (per flow_model); string `SNK_CreditsText@@3PAY0BI@DA` @0x4611c0 confirms 0x18-byte record stride; mugshot/Legals filenames @0x465ae4..0x465dd4]. [UNCERTAIN: `&DAT_004961dc` is read as `+ char*4` to fetch a mugshot surface pointer — it sits 0x104 below `_DAT_004962e0`; treated as the base of the mugshot surface-pointer array indexed by the credit record's 2nd byte. The exact element count of `SNK_CreditsText` (number of pages) not dumped; the band-gallery path is NOT reached from this screen.]

---

### Screen 23 @0x00413580 — ScreenPostRaceHighScoreTable  [interactive Y]
Inner states (in-body switch, 0..8): 0 init (MainMenu bg + 2 buttons + score panel surface +
inline strings + dual fonts), 1/2 present/settle + draw first score table, 3 slide-in (commit
`anim==0x27`=39 via tick-ready → state 4), 4/5 static double-present, 6 **interactive** (◄► cycle
track / OK exit), 7 exit present, 8 slide-out (`anim==0x10`=16 → release + SetFrontendScreen).

ELEMENTS:
| element | produced by (fn/global/asset) | INLINE/FLUSH | dest pos | gating | notes |
|---|---|---|---|---|---|
| Score table panel (5 rows) | `DrawPostRaceHighScoreEntry 0x413010` bakes into `g_lobbyErrorDialogSurface` (`CreateTrackedFrontendSurface(0x208,0x90)`); queued each frame | INLINE bake / FLUSH draw | panel 0x208×0x90 (520×144); queued at (cx−0xcd, cy−0x3f) | always | rebuilt on every track cycle (state 6 → `DrawPostRaceHighScoreEntry(idx)`). |
| Column headers | `DrawPostRaceHighScoreEntry`: `SNK_NameTxt/BestTxt/CarTxt/AvgTxt/TopTxt/TimeTxt/LapTxt/PtsTxt/SpdTxt` via `MeasureOrCenterFrontendString`→`DrawFrontendSmallFontStringToSurface(g_lobbyErrorDialogSurface)` | INLINE | header band y=7/0xe, columns at x 0x10/0x80/0xe4/0x160/0x1bc | header set varies by event type (`g_npcRacerGroupTable[idx*0xa4]&3`: 0/1=Time, 1=Lap, 2=Pts) | flush-baked into panel. |
| 5 score rows | `DrawPostRaceHighScoreEntry`: loop 0..4 — rank number (`sprintf` g_uiFormatStringScratchTemplate), name (`DrawFrontendClippedStringToSurface`, 0x60 clip), time/lap/pts (`s_%2.2d:%2.2d.%2.2d` @0x465484 / `%2.2d-%2.2d-%3.3d` @0x465470), car manuf (`g_localizationCarManufScratch + type*0xcc`), avg+top speed (`%dMPH`/`%dKPH`) | INLINE | rows y=0x30 step 0x10 | always | highlighted row (`uVar==g_postRaceQualifyingScore`) uses `g_smallTextbSurface` (bold) else `g_smallTextSurface`. |
| Header label (high-score title) | state 0 `CreateMenuStringLabelSurface(7)`→`g_currentScreenIndex`; queued each frame | INLINE bake / FLUSH draw | top (cx−200,…); slides state 3 (`anim*4`) | always | |
| Backing button (slot 0) | state 0 `CreateFrontendDisplayModeButton(0,-0x208,0,0x208,0x20,0)` (NULL label = wide backing bar) + state 2 `RebuildFrontendButtonSurface(0)` + `InitializeFrontendDisplayModeArrows(0,1)` | INLINE bake / FLUSH draw | 0x208×0x20 wide bar | always | ◄►-capable; cycles the displayed track (`g_postRaceRacerCardIndex`). |
| OK button (slot 1) | state 0 `CreateFrontendDisplayModeButton(SNK_OkButTxt,-0x130,0,0x60,0x20,0)`; esc index=1 | INLINE bake / FLUSH draw | 0x60×0x20 | always | confirm→`g_returnToScreenIndex=MainMenu` state 7. |
| Selection highlight | `RenderFrontendDisplayModeHighlight` | FLUSH (decoupled) | selected slot | tail!=-1 | color 0xc000. |

Primed contract globals: `g_postRaceRacerCardIndex` = currently-shown track/board index (◄► cycled, skips disabled rounds, cheat-widened); `g_postRaceQualifyingScore` = which row to bold (the player's qualifying rank); `g_lobbyErrorDialogSurface` = the 0x208×0x90 score panel surface (re-used name); `g_currentScreenIndex` = header label; `g_smallFontSurface` swapped between `g_smallTextSurface`/`g_smallTextbSurface` per-row for bold; `g_returnToScreenIndex` exit target.

Animation: state 3 slide-in 39 frames (counter pinned 0x26 awaiting tick-ready), buttons `anim*0x10`, panel `anim*-0x30`; header X `anim*4`; state 8 slide-out 16 frames (header `anim*-0x18`, panel `anim*-0x38`, buttons `anim*0x30`).

Conditional elements: BEST column header only when `SNK_BestTxt` not blank and event type≠2 (not points); TIME header only event type {0,1}&3==0; LAP header only type==1; PTS header only type==2. Row time/lap/pts format switches on event type (case 0/1=time, 2=pts via second sprintf, 4=lap). MPH vs KPH by `gSpeedReadoutUnitsConfigShadow`.

Input dispatch: slot0 ◄►→`g_postRaceRacerCardIndex += dir` (wrap 0..0x19 with disabled-round skip; cheat extends to 0..0x12) → rebuild board; slot1(OK)/ESC press→exit to MainMenu (state 7). No press handling on slot0 (cycle-only).

Confidence: [CONFIRMED @ 0x00413580 full body, 0x00413010 entry helper; format strings @0x465484 "%2.2d:%2.2d.%2.2d", @0x465470 "%2.2d-%2.2d-%3.3d", @0x465468 "%dMPH"/@0x465460 "%dKPH"]. [UNCERTAIN: `g_highScoreEntryHeadPtr_PROVISIONAL`/`g_localizationCarManufScratch_PROVISIONAL` are the per-board record array and per-car manufacturer-string scratch — populated by the score-load/localization path not in this fn; the actual score VALUES come from those buffers (stride 0x20 per row, 0xa4 per board).]

---

### Screen 24 @0x00422480 — RunRaceResultsScreen  [interactive Y]
Inner states (in-body switch, 0..0x15 = 0..21): 0 init (restore snapshot + SORT results + build
finish-position panel + 2 buttons), 1/2 present/settle, 3 slide-in (commit `anim==0x27` via tick-ready),
4/5 static double-present, 6 **interactive** (◄► cycle racer card / OK→state 0xb), 7/8/9/10 racer-card
swap slide (rebuild panel at `anim==0x11`=17), 0xb confirm slide-out, 0xc release surfaces, 0xd build the
post-race action-button menu (RaceAgain/ViewReplay/ViewRaceData/SelectNewCar or NextCupRace/Save/Quit…),
0xe action-menu slide-in (commit `anim==0x20`=32), 0xf **action-menu interactive**, 0x10 action slide-out
+ dispatch choice, 0x11 WriteCupData + result dialog, 0x12/0x13/0x14 save-dialog slide/confirm, 0x15 →
re-enter CarSelection (RaceAgain). Early bailouts state 0 → CupFailed / MainMenu / NetworkLobby.

ELEMENTS:
| element | produced by (fn/global/asset) | INLINE/FLUSH | dest pos | gating | notes |
|---|---|---|---|---|---|
| Race-results summary panel (positions/times) | `DrawRaceDataSummaryPanel 0x421e90` bakes into `g_lobbyErrorDialogSurface` (`CreateTrackedFrontendSurface(0x198,0x188)`); queued each frame | INLINE bake / FLUSH draw | panel 0x198×0x188 (408×392); queued at (cx−0xa8, iVar6) | always | left half = result rows (built in state 0 by repeated `DrawFrontendLocalizedStringToSurface` loops keyed by gametype); right 0x80-wide column rebuilt per racer-card by `DrawRaceDataSummaryPanel`. |
| Result rows (per-racer line items) | state 0: gametype-specific loops of `DrawFrontendLocalizedStringToSurface` into the panel (rows y 0x30..0xf0 step 0x18) | INLINE | rows in panel | gametype 8 (rows 0x48..0xa8), 7 (0x60..0xa8), <1 (0x60..0xf0), 9 (skip 0x90/0xa8), 1/6 (0x30..0xf0), 2-5 (two header lines + 0x60..0xf0) | the flush-drawn score/result ROWS; counts differ per event type. |
| Per-racer detail column | `DrawRaceDataSummaryPanel(g_postRaceRacerCardIndex)`: `BltColorFillToSurface(0x118,0,0x80,0x188)` clears right column, then time `%2.2d:%2.2d.%2.2d` / pts `%2.2d-%2.2d-%3.3d` / speed `%3dMPH`(@0x4660d8)/`%3dKPH`(@0x4660d0) via `DrawFrontendLocalizedStringToSurface` | INLINE | right 0x80×0x188 column of panel | gametype 1..6 adds time/pts; 9 = lap-split list (reads actor lap times `g_raceParticlePoolBase + slot*0x388 + 0x82e6`) | `RebuildFrontendButtonSurface(0)` + (gametype!=9) `InitializeFrontendDisplayModeArrows(0,1)` re-arm the card cycler. |
| Header label (results title) | state 0 `CreateMenuStringLabelSurface(0xe)`→`g_currentScreenIndex`; queued each frame | INLINE bake / FLUSH draw | top (cx−200,…); slides state 3 (`anim*4`) | always | |
| Backing button (slot 0) | state 0 `CreateFrontendDisplayModeButton(0,-0x208,0,0x208,0x20,0)` (NULL = wide bar) | INLINE bake / FLUSH draw | 0x208×0x20 | always | ◄►-capable (armed via card panel) → cycles `g_postRaceRacerCardIndex` (the focused racer/slot). |
| OK button (slot 1) | state 0 `CreateFrontendDisplayModeButton(SNK_OkButTxt,-0x208,0,0x60,0x20,0)`; esc index=0 | INLINE bake / FLUSH draw | 0x60×0x20 | always | press (idx 0/1)→state 0xb (proceed to action menu). |
| Action menu buttons | state 0xd: up to 5 of `SNK_RaceAgain / ViewReplay / ViewRaceData / SelectNewCar / Quit` (single-race) OR `NextCupRace / ViewReplay / ViewRaceData / SaveRaceStatus / OkBut|Quit` (cup); each `CreateFrontendDisplayModeButton(.., -0x120,0,0x120,0x20,0)` or PreviewButton (disabled variant) | INLINE bake / FLUSH draw | 0x120×0x20 (288×32) stacked | replay/data buttons become **preview (greyed/disabled)** when `g_replayFileAvailable==0` / no race-data; NextCupRace path gated by `ConfigureGameTypeFlags()` result | esc index=4. `g_frontendButtonIndex=1` default for cup. |
| Save-result dialog | state 0x11: `WriteCupData` → label `SNK_BlockSavedOK`(ok)/`SNK_FailedToSave`(fail) + OK button | INLINE bake / FLUSH draw | dialog buttons 0x120×0x20 | only the Save action (button choice 3 in cup mode) | |
| Selection highlight | `RenderFrontendDisplayModeHighlight` | FLUSH (decoupled) | selected slot | tail!=-1 | color 0xc000. |

Primed contract globals: `g_postRaceRacerCardIndex` = focused racer/finishing slot (◄► cycled in state 6, skips DNF slots where actor state byte ==3, dir-aware → state 7/9 swap anim); `g_postRaceMenuButtonChoice` = chosen action (state 0xf→0x10 dispatch); `g_postRaceNextCupRaceAvailable`, `g_postRaceProgressionAdvanced`; `g_lobbyErrorDialogSurface` = 0x198×0x188 results panel (re-used name); `g_currentScreenIndex` = header; `g_postRaceCarSelectionBackup.*` snapshot of selected car/paint/wheel/transmission (restored on RaceAgain); results sorted by `SortRaceResultsBySecondaryMetricDesc` (gametype 1/6) or `SortRaceResultsByPrimaryMetricAsc` (2-5) at state 0.

Animation: state 3 slide-in 39 frames; card-swap states 7/8/9/10 slide the panel by `anim*0x20` and rebuild at `anim==0x11`(17); action-menu state 0xe slide-in commit `anim==0x20`(32); state 0x10 slide-out 32 frames then dispatch; save-dialog 0x12/0x14 32-frame slides.

Conditional elements: result-row layout + detail-column content fully gated by `g_frontendSelectedGameType` (8=Drag, 7=TimeTrial, 9=lap-splits, <1=cup, 1/6, 2-5); ViewReplay/ViewRaceData buttons rendered as disabled **preview** buttons when their data is unavailable; NextCupRace vs Quit depends on `ConfigureGameTypeFlags()`; CupFailed/CupWon early exits gated by `g_raceResults[0]` + `g_raceParticlePoolBase[0x1f2/0x20b]` finish-state bytes. Network mode (`g_networkSessionActive`) skips the local menu and routes to lobby/main.

Input dispatch: state 6 — slot0 ◄►→cycle racer card (DNF-skip, dir→swap anim 7/9); OK/slot press (idx 0/1)→state 0xb. state 0xf — `g_frontendButtonPressedFlag` + `g_frontendButtonIndex` → `g_postRaceMenuButtonChoice`; case0 RaceAgain (restore car backup; Masters→`MarkMastersRaceSlotCompleted`), case1 ViewReplay (`g_inputPlaybackActive=1`), case2 ViewRaceData (→state 0x15/back to results), case3 Save (state 0x11), case4 Quit/Next (state 0x10 dispatch → NameEntry/CupWon/CupFailed/MainMenu/RaceResults per choice). ESC→index 4 (Quit).

Confidence: [CONFIRMED @ 0x00422480 full body, 0x00421e90 summary-panel helper; speed format strings @0x4660d8 "%3dMPH"/@0x4660d0 "%3dKPH"]. [UNCERTAIN: the actual numeric result VALUES — the `DrawFrontendLocalizedStringToSurface` calls in state 0 / `DrawRaceDataSummaryPanel` take NO visible args in the decompile (they read a current inline-string/format-arg context set by the surrounding `sprintf_game` + `SetFrontendInlineStringTable(SNK_MusicTest_MT)` — the localized-string renderer pulls its substitution args from that table); the per-racer finish data is read from `g_raceParticlePoolBase` actor blocks (stride 0x388) at offsets 0x82c0/0x82e6 and the `g_raceResults` table (stride 0x1e). The `_pad_1c[slot*4-0x18]` reads are the per-slot finish-state bytes (3=DNF/empty).]
# Frontend Screens 25–29 — Complete Per-Screen Element + Behavior Spec

RE target: `TD5_d3d.exe` (image base `0x00400000`). All addresses are Ghidra VAs.
Source: literal decompilation of each screen fn + every helper it calls, grounded in the
engine model (`frontend_rendering_model.md`, `frontend_flow_model.md`,
`frontend_call_graph_closure.md`). Values hex AND decimal.

**Cross-screen drawing model (applies to all 5; from rendering_model PART 1):** a screen fn
only PRIMES — it bakes label/panel/button surfaces, builds slot-table entries, and per frame
calls `QueueFrontendOverlayRect`/`MoveFrontendSpriteRect`. The actual on-screen pixels are
emitted by the per-frame **FLUSH** (`FlushFrontendSpriteBlits 0x00425540`, called by the LOOP
after the screen fn) which drains the overlay/sprite queues into `g_primaryWorkSurface`, and
unconditionally runs the two DECOUPLED draws (`UpdateExtrasGalleryDisplay 0x0040d830` +
`RenderFrontendDisplayModeHighlight 0x004263e0`). None of screens 25–29 calls those two decoupled
draws itself, but **both run every frame on every screen** — the gallery cross-fade no-ops here
(`g_extrasGallerySlideSurfaces`/transition!=2 gate), and the selection-highlight bars draw the
4-edge outline around the active OK button whenever `g_frontendOverlayRectArrayTail != -1` and
the cursor overlay is in Deactivate(==0) state. That highlight + the LOOP-queued mouse cursor are
the only flush-decoupled elements that actually produce pixels on these dialog screens.

Common geometry: `uVar13 = g_frontendCanvasW>>1` (=320 on 640×480), `uVar8 = g_frontendCanvasH>>1`
(=240). All dialog panels are queued centered; dialog surface is the shared
`g_lobbyErrorDialogSurface` tracked surface.

---

### Screen 25 @0x00413bc0 — ScreenPostRaceNameEntry  [interactive Y]
Inner states (`switch(g_frontendInnerState)`):
- **0** = qualify-check + init (decides if player even gets a name-entry; bakes header + name button).
- **1** = slide-in (header + name-input button slide on; counter target `0x20`=32).
- **2** = INTERACTIVE name edit (runs `RenderFrontendCreateSessionNameInput`; waits for edit-state==2 = ENTER).
- **3** = slide-out of the name button (counter `0x20`=32), then releases button + name surface.
- **4** = insert score into the per-schedule high-score table (array shuffle + write the new row), build the 0x208×0x90 results-table dialog surface + OK button.
- **5/6** = present + reset anim, set racer-card index 0, call `DrawPostRaceHighScoreEntry` (bakes the table rows).
- **7** = slide-in of the high-score table dialog (two sprite slots; counter `0x27`=39 → settle gate `AdvanceFrontendTickAndCheckReady`).
- **8/9** = static present of table dialog.
- **10 (0xa)** = INTERACTIVE: wait for OK press → set return screen 5, blit secondary→primary, advance.
- **0xb** = restore bg + re-color-key text surfaces (key 0xffffff), play sound.
- **0xc (12)** = slide-out (header + 2 dialog slots; counter `0x10`=16) → release all → `SetFrontendScreen(MAIN_MENU)`.
- `joined_r0x004145bf` (fall-through from state 0 when NOT qualifying, and tail of 0xc): zero schedule index for game-type 1..7, `SetFrontendScreen(TD5_SCREEN_MAIN_MENU)`.

ELEMENTS:
| element | produced by (fn/global/asset) | INLINE/FLUSH | dest pos | gating | notes |
|---|---|---|---|---|---|
| Full-screen menu background | `LoadTgaToFrontendSurface16bpp(s_Front_End_MainMenu_tga, FrontEnd.zip)` (state 0) → `g_primaryWorkSurface`; copied to secondary | INLINE | full 640×480 | only if qualifies | MainMenu.tga |
| Header label "ENTER PLAYER NAME" surface | `CreateMenuStringLabelSurface(7)` → `g_currentScreenIndex` (state 0); fed `SNK_Options_MT` inline-string table | INLINE bake | — | qualifies | label surface id reused as the slide-in header |
| Header label on screen | `QueueFrontendOverlayRect(anim-driven X, …, g_menuHeaderLabelSurfaceWidth/Height, g_currentScreenIndex)` every interactive/anim state | FLUSH (overlay drain) | states 1–0xc, X animates | — | state1 X=`anim*0x10-0x1f6+iVar14`; states 2–0xb X=`uVar13-200`(=120) |
| Name-input button (label "ENTER PLAYER NAME") | `CreateFrontendDisplayModeButton(SNK_EnterPlayerNameButTxt,-0x1c0,0,0x1c0,0x40,0)` → `g_postRaceNameButtonSurfacePtr_PROVISIONAL` (state 0); width 0x1c0=448 h 0x40=64 | INLINE bake; FLUSH on screen | slot 0, slid via `MoveFrontendSpriteRect` | qualifies | the box that holds the text field |
| Name-edit text field (live string) | `RenderFrontendCreateSessionNameInput 0x0041a530`: `BltColorFillToSurface` field bg + `DrawFrontendClippedStringToSurface(g_postRaceNameEditTargetPtr,…)` into the button surface | INLINE bake (per frame) | inside name button, x=0x14 | state 2 only | text = `g_postRacePlayerNameEntryBuffer` (16-char, `_g_postRaceNameEditMaxLength=0x10`) |
| Blinking caret | `RenderFrontendCreateSessionNameInput`: `BltColorFillToSurface(0xff00, textwidth+0x14, …, 2,0x10,…)` — 2×16 green bar after the text | INLINE bake | after last glyph | `buttonIndex==0 && (anim & 0x20)==0 && editState!=2` | blink = bit5 of `g_frontendAnimFrameCounter`; suppressed if text width ≥0x195 |
| Field highlight color | same fn: focused (`buttonIndex==0`) field bg = `0x392152`/offset 0x68; unfocused = `0`/offset 0x28; toggles each call via `g_connBrowserRedrawDirty&1` | INLINE bake | y=0x14 | — | gives the field its hilite when selected |
| High-score table dialog panel | `g_lobbyErrorDialogSurface = CreateTrackedFrontendSurface(0x208,0x90)` + `BltColorFillToSurface(0,…)` (state 4); 520×144 | INLINE bake | centered | post-edit | re-uses shared dialog surface var |
| High-score table CONTENT (headers + 5 ranked rows) | `DrawPostRaceHighScoreEntry(g_selectedScheduleIndex) 0x00413010` (state 5/6) bakes into `g_lobbyErrorDialogSurface` | INLINE bake; FLUSH on screen | see below | post-edit | full table layout, detailed in its own row group |
| Table dialog on screen | `QueueFrontendOverlayRect(…,0x208,0x90,g_lobbyErrorDialogSurface)` states 7–0xc | FLUSH | center, anim X | — | second sprite slot too (states 7/0xc move slot 0+1) |
| OK button | `CreateFrontendDisplayModeButton(SNK_OkButTxt,-0x130,0,0x60,0x20,0)` (state 4); plus a blank 0x208×0x20 spacer button `CreateFrontendDisplayModeButton(0,-0x208,0,0x208,0x20,0)` then `RebuildFrontendButtonSurface(0)` | INLINE bake; FLUSH | slot 1 | post-edit | `g_frontendButtonIndex=1; g_frontendEscKeyButtonIndex=1` |
| Selection highlight bars (around OK) | `RenderFrontendDisplayModeHighlight 0x004263e0` (4 edge bars 0xc000 into `g_frontendBackSurfacePtr`) | FLUSH (decoupled) | around active slot | `tail!=-1 && cursorOverlay==0` | runs every frame |
| Mouse cursor sprite | LOOP `QueueFrontendOverlayRect(mouseX,mouseY,…,g_frontendCursorTextureId)` | FLUSH | mouse pos | `cursorOverlayHidden==0 && mouseEnabled` | snkmouse.tga 22×30 |

**DrawPostRaceHighScoreEntry 0x00413010 (table content baker) — sub-elements (all INLINE bake into `g_lobbyErrorDialogSurface`, small font `g_smallTextSurface`):**
- Clears panel (`BltColorFillToSurface 0,0,0,0x208,0x90`).
- Column headers via `DrawFrontendSmallFontStringToSurface`+`MeasureOrCenterFrontendString` at fixed X-bands: `SNK_NameTxt`(x 0x10..0x70), `SNK_CarTxt`(0xe4..0x150), `SNK_AvgTxt`(0x160..0x1ac), `SNK_TopTxt`(0x1bc..0x208), `SNK_SpdTxt`×2 (under Avg+Top). Middle column header is **mode-dependent** on `g_npcRacerGroupTable[sched*0xa4]&3`: ==0 → `SNK_TimeTxt`; ==1 → `SNK_LapTxt`; ==2 → `SNK_PtsTxt`. `SNK_BestTxt` drawn at top (0x80..0xd4) unless mode==2.
- 5 ranked rows (loop `uVar3`=0..4, rowY `iVar4` 0x30 step 0x10): rank number (`sprintf` template), **inserted-rank highlight** — `if (rowIndex == g_postRaceQualifyingScore) g_smallFontSurface = g_smallTextbSurface; else g_smallTextSurface` (the player's just-inserted row drawn in the alt/bold font `SmallTextb`). Name via `DrawFrontendClippedStringToSurface(row-0x10,0x10,…,clip 0x60)`. Middle col = time/pts formatted (`%2.2d:%2.2d.%3.3d` for time mode `s_..._00465470`, `%2.2d:%2.2d.%2.2d` `s_..._00465484`, or sprintf for pts). Car = manufacturer string `(&g_localizationCarManufScratch)[gExtCarIdToTypeIndex[row.car]*0xcc]`. Avg+Top speed `%dMPH`/`%dKPH` per `gSpeedReadoutUnitsConfigShadow`.

Primed contract globals: `g_postRaceNameEditTargetPtr=&g_postRacePlayerNameEntryBuffer`,
`_g_postRaceNameEditMaxLength=0x10`(16), `g_postRaceNameEditState`(@0x004969d0; 1=editing,
2=ENTER-committed — written to 2 by `DXInput::GetString` middleware), `g_postRaceQualifyingScore`
(the score/time used both as the qualify gate AND, after state 4, repurposed as the inserted-row
rank index consumed by the highlight), `g_selectedScheduleIndex` (=0x13 for time-trial, else
gametype+0x13), `g_currentScreenIndex`(header label surface), `g_lobbyErrorDialogSurface`(table
panel), `g_returnToScreenIndex`(=0 after edit, =5 after OK), `g_frontendEscKeyButtonIndex`
(-1 during edit, 1 at OK), `g_frontendButtonIndex=1`. `g_connBrowserRedrawDirty` flips field hilite.

Animation: pure `g_frontendAnimFrameCounter`-driven (frame-count, not ms). State1 slide-in fires at
counter `0x20`(32) (`DXSound::Play(4)`); state3 slide-out at `0x20`(32); state7 table slide-in
holds at `0x27`(39) gated by 3-frame `AdvanceFrontendTickAndCheckReady`; state0xc slide-out fires at
`0x10`(16). Caret blink = `g_frontendAnimFrameCounter & 0x20` (on/off every 32 frames). Slide
positions are linear `base ± counter*step` (steps 0x10/0x18/0x20/0x30/0x38/4/6).

Conditional elements: the WHOLE name-entry sub-flow (states 0–3) only runs if
`g_postRaceQualifyingScore != 0` AND `g_raceParticlePoolBase[0x1f2].size_rate != 2` AND
`g_twoPlayerModeEnabled==0`. Qualify computation (state 0) branches on mode bits
`g_npcRacerGroupTable[sched*0xa4]&3`: 0=points-vs-threshold, 1=min-lap-scan (`0x2b818` init,
scans pool), 2=lap_time_total≤threshold. For gametype<1, also gated off unless special-encounter
unlock flags set. If not qualifying → skips straight to score-insert path / main menu. Inserted-row
highlight (alt font) only on the player's row. Speed unit MPH/KPH per config shadow. Middle column
header/value differ per mode (Time/Lap/Pts).

Input dispatch: state 2 — name chars come from `DXInput::GetString(&g_postRaceNameEditTargetPtr)`
(only when `g_frontendButtonIndex==0`; `DXInput::SetAnsi(1)` enables it) — an OS/middleware edit
box writing into `g_postRacePlayerNameEntryBuffer`; ENTER sets edit-state→2 (boundary). On commit,
if the entered name is empty (len test `iVar9==-2`) it copies `g_localComputerName` as the default.
State 0xa — `if (g_frontendButtonPressedFlag != 0)` (OK / ESC→index1) commits and advances.
NOT alphabet-grid: it is the DXInput string-input model.

Confidence: [CONFIRMED @ 0x00413bc0, 0x00413010, 0x0041a530, edit-state @0x004969d0, fmt @0x004660e0/0x00465460–84].
[UNCERTAIN: exact transition site that sets `g_postRaceNameEditState=2` — it is inside the external
`DXInput::GetString` (middleware boundary), not in any decompiled frontend fn; inferred from the
`tagSTRINGSTRUCT` passed by address. Missing evidence: DXInput::GetString body (import).]

---

### Screen 26 @0x004237f0 — ScreenCupFailedDialog  [interactive Y]
Inner states: **0** = guard (only for cup game-types 1..6, else straight to main menu) + load bg, bake dialog panel + 4 message lines + OK button. **1/2/3** = present + reset anim (3-frame settle). **4** = slide-in (counter `0x20`=32 → Deactivate cursor). **5** = static + wait OK press → release + `SetFrontendScreen(g_returnToScreenIndex)`.

ELEMENTS:
| element | produced by | INLINE/FLUSH | dest pos | gating | notes |
|---|---|---|---|---|---|
| Full-screen bg | `LoadTgaToFrontendSurface16bpp(MainMenu.tga,FrontEnd.zip)`→primary, copy to secondary (state 0) | INLINE | full | gametype 1..6 | |
| Dialog panel | `g_lobbyErrorDialogSurface = CreateTrackedFrontendSurface(0x198,0x70)` + `BltColorFillToSurface(0,…)`; 408×112 | INLINE bake | centered (uVar1-0xa8, uVar2-0x8f)=(152,97) | — | |
| Message text (4 lines, centered) | `MeasureOrCenterFrontendLocalizedString` + `DrawFrontendLocalizedStringToSurface` into panel: `SNK_SorryTxt`, `SNK_YouFailedTxt`, `SNK_ToWinTxt`, then race-type name `*(SNK_RaceTypeText + gametype*4)` | INLINE bake | within 0x198-wide panel | — | line 4 = the failed cup's name |
| Dialog on screen | `QueueFrontendOverlayRect(…,0x198,0x70,g_lobbyErrorDialogSurface)` states 4/5 | FLUSH | center, anim X (state4) | states ≥4 only | state4 X=`uVar1+anim*-0x18+600`; state5 X=`uVar1-0xa8` |
| OK button | `CreateFrontendDisplayModeButton(SNK_OkButTxt,-0x120,0,0x60,0x20,0)`; slot 0 slid via `MoveFrontendSpriteRect` | INLINE bake; FLUSH | slid in | — | `g_frontendEscKeyButtonIndex=0` |
| Selection highlight + cursor | decoupled `RenderFrontendDisplayModeHighlight` + LOOP cursor | FLUSH | around OK / mouse | per gates | every frame |

Primed contract globals: `g_lobbyErrorDialogSurface`, `g_frontendEscKeyButtonIndex=0`,
`g_returnToScreenIndex` (exit target), `g_frontendSelectedGameType` (1..6 gate + race-type label
index). No trophy/medal art — failed dialog is text-only (NO flush/inline art element).

Animation: `g_frontendAnimFrameCounter`-driven; slide-in commits at `0x20`(32). No fade/scale.

Conditional elements: entire dialog gated on `0 < g_frontendSelectedGameType < 7` (cup modes);
otherwise immediate `SetFrontendScreen(MAIN_MENU)`. Line 4 text varies by which cup failed.
No auto-dismiss timer — waits for button.

Input dispatch: state 5 — `if (g_frontendButtonPressedFlag != 0)` (OK click or ESC→index0)
→ release surface+buttons → `SetFrontendScreen(g_returnToScreenIndex)`.

Confidence: [CONFIRMED @ 0x004237f0; SNK_RaceTypeText indirection; panel 0x198×0x70].

---

### Screen 27 @0x00423a80 — ScreenCupWonDialog  [interactive Y]
Inner states: identical shape to 26. **0** = guard (cup types 1..6) + `_unlink(g_cupDataTd5Filename)` (deletes the saved cup so it can't be re-won) + bg + bake taller panel + message lines (incl. conditional unlock lines) + OK. **1/2/3** = present/settle. **4** = slide-in (`0x20`=32 → Deactivate). **5** = static + OK → exit.

ELEMENTS:
| element | produced by | INLINE/FLUSH | dest pos | gating | notes |
|---|---|---|---|---|---|
| Full-screen bg | `LoadTgaToFrontendSurface16bpp(MainMenu.tga,FrontEnd.zip)` + copy secondary | INLINE | full | gametype 1..6 | |
| Dialog panel | `CreateTrackedFrontendSurface(0x198,0xc4)` + fill; 408×**196** (taller than the failed dialog's 0x70=112 — room for unlock lines) | INLINE bake | center (uVar1-0xa8,uVar2-0x8f) | — | |
| Message text (3 fixed lines) | `MeasureOrCenterFrontendLocalizedString`+`DrawFrontendLocalizedStringToSurface`: `SNK_CongratsTxt`, `SNK_YouHaveWonTxt`, race-type name `*(SNK_RaceTypeText+gametype*4)` | INLINE bake | in panel | — | |
| **Unlock-announcement line 1** | `sprintf_game(local_40, s__d__s_004660e0 ="%d %s")` → `MeasureOrCenterFrontendLocalizedString`+draw | INLINE bake | in panel | `if (g_cupSchedule_currentCup != 0)` | "%d %s" = number + name (the unlocked cup/car/track) |
| **Unlock-announcement line 2** | same `"%d %s"` sprintf + draw | INLINE bake | in panel | `if (g_cupSchedule_currentRound != 0)` | second unlock line |
| Dialog on screen | `QueueFrontendOverlayRect(…,0x198,0xc4,g_lobbyErrorDialogSurface)` states 4/5 | FLUSH | center, anim X | states ≥4 | |
| OK button | `CreateFrontendDisplayModeButton(SNK_OkButTxt,-0x120,0,0x60,0x20,0)`; `MoveFrontendSpriteRect` slot0 | INLINE bake; FLUSH | slid in | — | `EscKeyButtonIndex=0` |
| Selection highlight + cursor | decoupled highlight + LOOP cursor | FLUSH | per gates | — | |

Primed contract globals: `g_lobbyErrorDialogSurface`, `g_frontendEscKeyButtonIndex=0`,
`g_returnToScreenIndex`, `g_frontendSelectedGameType`, `g_cupSchedule_currentCup`,
`g_cupSchedule_currentRound` (gate the two unlock lines), `g_cupDataTd5Filename` (deleted).
**No trophy/medal/cup ART** — the "won" content is purely text (the two `"%d %s"` unlock lines);
there is no flush or inline image element for a trophy. [UNCERTAIN] the `local_40` sprintf passes
only the `"%d %s"` template literal in the decompile (varargs not recovered) — the actual number +
string args are the unlocked-item id/name, but their source globals were not resolved in this body.
Missing evidence: the variadic args to the two `sprintf_game` calls.

Animation: counter-driven, slide-in at `0x20`(32). No fade/scale.

Conditional elements: whole dialog gated `0 < gametype < 7`. Unlock line 1 only if currentCup!=0;
line 2 only if currentRound!=0 (so 3, 4, or 5 message lines total → the taller 0xc4 panel). No timer.

Input dispatch: state 5 — `if (g_frontendButtonPressedFlag != 0)` → release → `SetFrontendScreen(g_returnToScreenIndex)`.

Confidence: [CONFIRMED @ 0x00423a80; fmt "%d %s" @0x004660e0; panel 0x198×0xc4].

---

### Screen 28 @0x00415370 — ScreenStartupInit  [interactive N]
Inner states: **0** = bake a blank dialog panel + OK button (a transient placeholder/bootstrap dialog).
**1/2/3** = queue the (empty) panel for 3 frames (reset anim each). **4** = release panel+button,
reset inner-state/anim, and hand off: `g_currentScreenFnPtr = g_frontendScreenFnTable` (= entry 0,
ScreenLocalizationInit @0x004269d0) with `g_frontendInnerState = timeGetTime()`. This is the
bootstrap/transition screen: it stalls a few frames with an empty centered box then jumps to the
real first screen (idx 0).

ELEMENTS:
| element | produced by | INLINE/FLUSH | dest pos | gating | notes |
|---|---|---|---|---|---|
| Blank dialog panel | `g_lobbyErrorDialogSurface = CreateTrackedFrontendSurface(0x198,0x70)` + `BltColorFillToSurface(0,…)` (state 0) | INLINE bake | (cW>>1)-0xa8,(cH>>1)-0x8f = (152,97) | — | 408×112, never filled with text |
| Panel on screen | `QueueFrontendOverlayRect(…,0x198,0x70,g_lobbyErrorDialogSurface)` states 1–3 | FLUSH | centered, NO animation | — | static (no anim X) |
| OK button | `CreateFrontendDisplayModeButton(SNK_OkButTxt,-0x120,0,0x60,0x20,0)` (state 0) | INLINE bake; FLUSH | slot 0 | — | `g_frontendEscKeyButtonIndex=0` — but never read (no interactive state) |
| Selection highlight + cursor | decoupled highlight + LOOP cursor | FLUSH | per gates | `ActivateFrontendCursorOverlay()` called | runs but auto-advances |

Primed contract globals: `g_lobbyErrorDialogSurface`, `g_frontendEscKeyButtonIndex=0`,
`g_currentScreenFnPtr` (set to table[0] at hand-off), `g_frontendInnerState` (re-stamped to
`timeGetTime()` = the screen-enter timestamp role). No bg image load, no text lines.

Animation: none (states 1–3 just re-queue + zero `g_frontendAnimFrameCounter`); it is a 4-frame
auto-advancing pass, NOT a timed slide. No fade/scale.

Conditional elements: none — unconditional 0→1→2→3→4 march. NO message text, NO art. (It is
effectively a one-time bootstrap that shows an empty box then dispatches to ScreenLocalizationInit.)

Input dispatch: NONE — no input read; no `g_frontendButtonPressedFlag` check. State 4 hands off
unconditionally. (Despite an OK button being created, no state consumes a press.)

Confidence: [CONFIRMED @ 0x00415370; hand-off `g_currentScreenFnPtr=g_frontendScreenFnTable` = idx0].

---

### Screen 29 @0x0041d630 — ScreenSessionLockedDialog  [interactive Y]
Inner states: **0** = load bg + restore cached rect + bake panel + 2 message lines ("Sorry / Session Locked") + OK. **1/2/3** = present/settle. **4** = slide-in (`0x20`=32 → Deactivate cursor). **5** = static + OK → `SetFrontendScreen(TD5_SCREEN_MAIN_MENU)` (hardwired to main menu, unlike 26/27 which use `g_returnToScreenIndex`).

ELEMENTS:
| element | produced by | INLINE/FLUSH | dest pos | gating | notes |
|---|---|---|---|---|---|
| Full-screen bg | `LoadTgaToFrontendSurface16bpp(MainMenu.tga,FrontEnd.zip)`→primary, copy secondary, `BlitFrontendCachedRect(0,0,cW,cH)` | INLINE | full | — | extra cached-rect restore vs 26/27 |
| Dialog panel | `CreateTrackedFrontendSurface(0x198,0x70)` + fill; 408×112 | INLINE bake | center (uVar1-0xa8,uVar2-0x8f)=(152,97) | — | |
| Message text (2 lines) | `MeasureOrCenterFrontendLocalizedString`+`DrawFrontendLocalizedStringToSurface`: `SNK_SorryTxt`, `SNK_SeshLockedTxt` | INLINE bake | in 0x198 panel | — | "Sorry" / "Session Locked" |
| Dialog on screen | `QueueFrontendOverlayRect(…,0x198,0x70,g_lobbyErrorDialogSurface)` states 4/5 | FLUSH | center, anim X (state4) | states ≥4 | |
| OK button | `CreateFrontendDisplayModeButton(SNK_OkButTxt,-0x120,0,0x60,0x20,0)`; `MoveFrontendSpriteRect` slot0 | INLINE bake; FLUSH | slid in | — | `EscKeyButtonIndex=0` |
| **Lock art** | — | — | — | — | NONE: no medal/lock image element — text-only ("locked" conveyed by string). |
| Selection highlight + cursor | decoupled highlight + LOOP cursor | FLUSH | per gates | — | |

Primed contract globals: `g_lobbyErrorDialogSurface`, `g_frontendEscKeyButtonIndex=0`. NO
`g_returnToScreenIndex` use — exit is hardwired `SetFrontendScreen(TD5_SCREEN_MAIN_MENU)`. No
kicked-flag is read inside this fn; the "session locked" condition is decided by whatever net-flow
screen jumped here (this screen just shows the message). [UNCERTAIN] no kicked/lock state global is
referenced in this body; the dialog is unconditional once entered. Missing evidence: the caller
that performs the lock check and `SetFrontendScreen(29)` (a net-lobby screen, idx 8–11).

Animation: counter-driven; slide-in commits at `0x20`(32). No fade/scale, no auto-dismiss timer.

Conditional elements: none inside the fn (always shows both lines + OK). Differs from 26/27 only in
text content, the extra `BlitFrontendCachedRect`, and the hardwired main-menu exit.

Input dispatch: state 5 — `if (g_frontendButtonPressedFlag != 0)` (OK / ESC→index0) → release →
`SetFrontendScreen(MAIN_MENU)`.

Confidence: [CONFIRMED @ 0x0041d630; SNK_SorryTxt + SNK_SeshLockedTxt; hardwired main-menu exit].
