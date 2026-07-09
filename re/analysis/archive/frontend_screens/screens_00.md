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
