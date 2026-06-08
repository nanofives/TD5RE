# TD5 Frontend Screen Layout Spec — exact element positions from TD5_d3d.exe

**Generated 2026-05-30** by a full Ghidra harvest of all 30 frontend screen functions.
Source of truth for porting frontend element positions **faithfully** — replaces the prior "eyeball screenshots + Frida-poke" workflow.

## TL;DR — where positions live (investigation verdict)

Frontend element coordinates are **NOT** stored in a data table, **NOT** in any DLL, and **NOT** in any shipped file. They are **immediate operands inside each of the 30 per-screen draw functions** in `TD5_d3d.exe`, computed at runtime and passed as explicit `x`/`y` args to a small set of placement helpers. Ruled out (2026-05-30, two independent agents):
- **M2DX.dll / M2DXFX.dll** = low-level DX primitives only (no layout API).
- **Language.dll** = 171 `SNK_*` text-string tables only, zero numeric data.
- **Asset files** = images/audio only (`Positioner.tga` is a 4×40 cursor sprite, not a table).
- **Static `.data` table** = the button registry `@0x499c78` is BSS, runtime-filled — dumps all-zero.

So every coordinate IS visible in the decompilation; the faithful method is to **transcribe the immediates per screen** (this document), not to approximate from screenshots.

## How to read this spec

Each screen has a `### Screen N @ 0xADDR — FunctionName` block with a table of every positioned draw call. The `x`/`y` columns are the **verbatim decompiled expression** (with bare constants annotated `[signed decimal]`). Reproduce them in the port literally.

Common position idioms (confirmed across all screens):
- `cW2 = canvasW >> 1`, `cH2 = canvasH >> 1` — screen-center halves (640×480 → 320 / 240).
- **`BX = (canvasW>>1) - 0xD2`  (=cW2-210)** and **`BY = (canvasH>>1) - 0x9F`  (=cH2-159)** — the two dominant menu anchor bases. Most option/menu screens derive from these.
- **Negative `x` passed to `CreateFrontendDisplayModeButton` = auto-layout flag** (not a literal position). The button stacks vertically via the engine's auto-layout; the port mirrors this with `FE_BTN_LEFT_OFFSET 0xD2` + `s_auto_button_y_offset[]`. Record the negative x literally.
- **`g_frontendAnimFrameCounter`** (and per-screen `frame`/slide counters) drive slide-in/out animations — e.g. `frame*0x10 - 0x266 + (canvasW>>1) - 0xd2`. Port these formulas, not a single resolved frame.
- **Text x-centering** is governed by `MeasureOrCenterFrontendLocalizedString(str, left, right)` immediately before a text draw — the listed `(left,right)` are the centering extent, not pixel positions. `DrawFrontendLocalizedStringToSurface @0x424560` is `void(void)` (reads globals) — carries no own x/y.

## Placement / draw helper reference (callee addresses)

| Helper | Address | Signature | Notes |
|---|---|---|---|
| `Copy16BitSurfaceRect` | `0x004251a0` | `(dst_x, dst_y, *src_surf, *src_rect4, flags)` | `flags`: `0x10`=opaque, `0x11`=color-key. Low-level blit. |
| `CreateFrontendDisplayModeButton` | `0x00425de0` | `(label, x, y, w, h, userdata)` | **Negative x = auto-layout flag.** |
| `CreateFrontendDisplayModePreviewButton` | *(variant; addr unresolved this pass)* | greyed/preview button | Used on screens 6/16; cheat/state-gated. |
| `CreateFrontendMenuRectEntry` | `0x004258f0` | `(x, y, w, h, src_x, src_y, color, surface)` | 8-arg rect placer (same shape as QueueFrontendOverlayRect); used by ScreenLanguageSelect. |
| `QueueFrontendOverlayRect` | `0x00425660` | `(dst_x, dst_y, src_x, src_y, w, h, color, surface)` | Sprite/overlay rect queue. |
| `MoveFrontendSpriteRect` | `0x004259d0` | `(slot, x, y)` | Repositions a queued sprite slot (animations). |
| `DrawFrontendButtonBackground` | `0x00425b60` | 9-slice button frame | Surface-CONTENT bake (port: td5_frontend_button_cache.c), not screen placement. |
| `DrawFrontendLocalizedStringToSurface` | `0x00424560` | `void(void)` | No x/y operand; placement via preceding `MeasureOrCenter...`. |
| `MeasureOrCenterFrontendLocalizedString` | *(see screen rows)* | `(str, left, right)` | Computes centered text x within `[left,right]`. |
| `RenderPositionerGlyphStrip` | `0x00414f40` | glyph strip | Delegate of ScreenPositionerDebugTool. |
| `DrawCarSelectionPreviewOverlay` | `0x0040ddc0` | car preview panel `(cW-0x198, cH-0x164, …)` | Delegate of CarSelectionScreenStateMachine; additive color 0x5a. |

## Screen dispatch index map

Dispatch: `RunFrontendDisplayLoop @ 0x00414b50` → `(*fn)()` from `g_frontendScreenFnTable @ 0x004655C4` (30 LE uint32 fn pointers).

| Idx | Address | Function | Placement rows |
|---|---|---|---|
| 0 | 0x004269d0 | ScreenLocalizationInit | 0 (bootstrap, no draws) |
| 1 | 0x00415030 | ScreenPositionerDebugTool | 4 (1 direct + 3 via RenderPositionerGlyphStrip) |
| 2 | 0x004275a0 | RunAttractModeDemoScreen | 0 (state pump, no draws) |
| 3 | 0x00427290 | ScreenLanguageSelect | 5 |
| 4 | 0x004274a0 | ScreenLegalCopyright | 1 (+1 UNCERTAIN FP y) |
| 5 | 0x00415490 | ScreenMainMenuAnd1PRaceFlow | 52 |
| 6 | 0x004168b0 | RaceTypeCategoryMenuStateMachine | 89 |
| 7 | 0x004213d0 | ScreenQuickRaceMenu | 24 |
| 8 | 0x00418d50 | RunFrontendConnectionBrowser | 23 |
| 9 | 0x00419cf0 | RunFrontendSessionPicker | 22 |
| 10 | 0x0041a7b0 | RunFrontendCreateSessionFlow | 21 |
| 11 | 0x0041c330 | RunFrontendNetworkLobby | 36 |
| 12 | 0x0041d890 | ScreenOptionsHub | 23 |
| 13 | 0x0041f990 | ScreenGameOptions | 34 |
| 14 | 0x0041df20 | ScreenControlOptions | 18 |
| 15 | 0x0041ea90 | ScreenSoundOptions | 50 |
| 16 | 0x00420400 | ScreenDisplayOptions | 28 |
| 17 | 0x00420c70 | ScreenTwoPlayerOptions | 25 |
| 18 | 0x0040fe00 | ScreenControllerBindingPage | 24 |
| 19 | 0x00418460 | ScreenMusicTestExtras | 21 |
| 20 | 0x0040dfc0 | CarSelectionScreenStateMachine | 50 (+7 sub-draw) |
| 21 | 0x00427630 | TrackSelectionScreenStateMachine | 28 |
| 22 | 0x00417d50 | ScreenExtrasGallery | 12 (absolute coords, not canvas-center) |
| 23 | 0x00413580 | ScreenPostRaceHighScoreTable | 17 |
| 24 | 0x00422480 | RunRaceResultsScreen | 39 |
| 25 | 0x00413bc0 | ScreenPostRaceNameEntry | 26 (incl. DrawPostRaceHighScoreEntry @0x413010) |
| 26 | 0x004237f0 | ScreenCupFailedDialog | 10 |
| 27 | 0x00423a80 | ScreenCupWonDialog | 11 |
| 28 | 0x00415370 | ScreenStartupInit | 4 |
| 29 | 0x0041d630 | ScreenSessionLockedDialog | 9 |

---

# Per-screen placement tables

# Frontend layout harvest — part_00 (dispatch indices 0..4)

Source: TD5_d3d.exe via Ghidra pool slot TD5_pool10, read_only.
Dispatch table g_frontendScreenFnTable @ 0x004655C4. First 5 LE uint32 (verified own read):
0x004269d0, 0x00415030, 0x004275a0, 0x00427290, 0x004274a0 (matches prior reference).

## Helper-address ground truth (this build)
- CreateFrontendDisplayModeButton @ 0x00425de0 — sig `int* (byte* label, int x, int y, uint w, int h, undefined4 userdata)`. NOT CALLED by any of the 5 screens.
- QueueFrontendOverlayRect @ 0x00425660 — sig `(dst_x, dst_y, src_x, src_y, w, h, color, surface)`. Called (indirectly via RenderPositionerGlyphStrip) + via screen 1 case 4.
- MoveFrontendSpriteRect @ 0x004259d0 — `(slot, x, y)`. NOT CALLED.
- Copy16BitSurfaceRect @ 0x004251a0 — `(dst_x, dst_y, *src_surf, *src_rect4, flags)`. NOT CALLED directly by any of the 5.
- DrawFrontendLocalizedStringToSurface @ 0x00424560 — sig `void (void)` (takes no x/y args). NOT CALLED by the 5.
- DrawFrontendButtonBackground @ 0x00425b60 — NOT CALLED by the 5.
- [ADDITIONAL placement helper, NOT in task list] CreateFrontendMenuRectEntry @ 0x004258f0 — sig `(int x, int y, int w, int h, int src_x, int src_y, color, int surface)`. Same shape as QueueFrontendOverlayRect; this is the rect/button placer used by ScreenLanguageSelect (4 language-flag buttons). Recorded literally below.
- [carries position] DrawFrontendLocalizedStringPrimary @ 0x004242b0 — sig `int (byte* text, uint x, int y)`. Used by ScreenLanguageSelect (title).
- [carries position] DrawFrontendLocalizedStringSecondary @ 0x00424390 — sig `void (float sx, float sy)`. Used by ScreenLegalCopyright (tiled legal text).

---

### Screen 0 @ 0x004269d0 — ScreenLocalizationInit
Mechanism: none — localization/config bootstrap (loads LANGUAGE.DLL strings into car-zip tables, LoadPackedConfigTd5, controller/display-mode setup). Computes NO on-screen positions; no canvas refs.
States/structure: gated on g_frontendBootDispatchMode (0 = full init then SetFrontendScreen(MAIN_MENU); 1 = route to MAIN_MENU; 2 = route to RACE_RESULTS + restore volumes). Not a per-frame draw FSM.

| # | State/case | Helper @addr | x (verbatim) [dec] | y (verbatim) [dec] | w | h | tex/label/surface | blit-flag | notes |
|---|---|---|---|---|---|---|---|---|---|
| — | — | (none) | — | — | — | — | — | — | No placement/draw helper calls. Calls only LoadPackedConfigTd5, BuildEnumeratedDisplayModeList, FormatDisplayModeOptionStrings, DXSound::*, SetFrontendScreen. |

Delegated sub-draw fns followed: none (no delegation to a draw helper).
Confidence: [CONFIRMED @ 0x004269d0] — decomp contains zero calls to any positioned-draw helper.

---

### Screen 1 @ 0x00415030 — ScreenPositionerDebugTool
Mechanism: developer glyph-positioner. Hard-coded immediate pixel coords (NO canvas-half math). Per-glyph offsets read/written from runtime arrays g_positionerGlyphRectsPrimary/Secondary; the strip is rendered by delegated RenderPositionerGlyphStrip.
States/structure: switch(g_frontendInnerState), cases 0..5 (+default). case0 loads Positioner.tga + 2 scanline fills; case2 seeds glyph rects; case3 strip render + nav; case4 strip render + 1 overlay rect + per-glyph nudge; case5 writes positioner.txt.

| # | State/case | Helper @addr | x (verbatim) [dec] | y (verbatim) [dec] | w | h | tex/label/surface | blit-flag | notes |
|---|---|---|---|---|---|---|---|---|---|
| 1 | case 0 | FillPrimaryFrontendScanline @ (not in task list) | scanline 0x114 [276] | (single arg = row) 0xff [255] | — | — | — | — | first arg row=0x114, 2nd=0xff |
| 2 | case 0 | FillPrimaryFrontendScanline | scanline 0x10c [268] | 0xff [255] | — | — | — | — | row=0x10c |
| 3 | case 4 | QueueFrontendOverlayRect @ 0x00425660 | iVar2 [=RenderPositionerGlyphStrip(0) return = local_4 cursor x, runtime] | 0x15c [348] (iVar8) | 0x24 [36] (iVar9) | 0x24 [36] (iVar12) | g_menuFontSurface (iVar4) | color uVar13=0 | args order (dst_x=iVar2, dst_y=0x15c, src_x=0x24? NOTE call passes (iVar2,iVar8,iVar9,iVar10,iVar11,iVar12,uVar13,iVar4) = (x,348,0x24,0,0x24,0x24,0,surf)); src_x=0x24,src_y=0,w=0x24,h=0x24 |

Delegated sub-draw fns followed: 0x00414f40 RenderPositionerGlyphStrip (called in case3 and case4). Its QueueFrontendOverlayRect calls (all @0x00425660):
  (a) (iVar3 [runtime x accumulator], piVar2[1]+0xf0 [glyph.y + 240], (iVar4%7)*0x24 [src_x], (iVar4/7)*0x24 [src_y], 0x24 [36], 0x24 [36], 0, g_menuFontSurface) — per-glyph cell, y = glyph_rect.y + 0xf0[240].
  (b) (iVar3+iVar1 [x+glyph.x], 0xdc [220], 0,0, 4, 0x28 [40], 0, g_frontendButtonIndex) — caret bar, fixed y=0xdc.
  (c) (local_4 [selected-glyph x], 0x138 [312], 0x24 [36], 0, 0x24 [36], 0x24 [36], 0, g_menuFontSurface) — selection marker, fixed y=0x138.
Confidence: [CONFIRMED @ 0x00415030, 0x00414f40] — debug tool; coords are literal immediates, no canvas math. The runtime x args (iVar2/iVar3/local_4) are accumulated cursor positions, not constants.

---

### Screen 2 @ 0x004275a0 — RunAttractModeDemoScreen
Mechanism: none — attract/demo state pump (present buffer, cursor overlay, fade in/out, then InitializeRaceSeriesSchedule). No positioned-draw helper calls; no canvas refs.
States/structure: switch(g_frontendInnerState) cases 0..5. Only present/fade/release/init calls.

| # | State/case | Helper @addr | x (verbatim) [dec] | y (verbatim) [dec] | w | h | tex/label/surface | blit-flag | notes |
|---|---|---|---|---|---|---|---|---|---|
| — | — | (none) | — | — | — | — | — | — | Calls PresentPrimaryFrontendBuffer(ViaCopy), ActivateFrontendCursorOverlay, ReleaseFrontendDisplayModeButtons, InitFrontendFadeColor(0), RenderFrontendFadeEffect, InitializeRaceSeriesSchedule, InitializeFrontendDisplayModeState. None carry x/y. |

Delegated sub-draw fns followed: none.
Confidence: [CONFIRMED @ 0x004275a0] — no placement calls.

---

### Screen 3 @ 0x00427290 — ScreenLanguageSelect
Mechanism: canvas-relative. Precomputes 3 locals at function top:
  uVar3 = g_frontendCanvasH / 5
  uVar1 = (g_frontendCanvasH - uVar3 >> 1) - 0x80 >> 1   [(H - H/5)/2 - 128, then /2]
  uVar2 = (g_frontendCanvasW >> 1) - 0xb0 >> 1           [((W/2) - 176)/2]
Four 176x128 (0xb0 x 0x80) language-flag buttons placed at the 4 canvas corners using uVar1/uVar2/uVar3 + canvas W/H; title string drawn top-center.
States/structure: switch(g_frontendInnerState) 7 states (0..6). All placement happens in case 0. case6 advances to LEGAL_COPYRIGHT.

| # | State/case | Helper @addr | x (verbatim) [dec where const] | y (verbatim) | w | h | tex/label/surface | blit-flag | notes (anim/frame var) |
|---|---|---|---|---|---|---|---|---|---|
| 1 | case 0 | DrawFrontendLocalizedStringPrimary @ 0x004242b0 | uVar2 [=((W>>1)-0xb0)>>1] | uVar3 - 0x1c >> 1 [(H/5 - 28)/2] | — | — | label = s_LANGUAGE_SELECT_004667c0 | — | title, x=uVar2 y=(H/5-28)/2 |
| 2 | case 0 | CreateFrontendMenuRectEntry @ 0x004258f0 | uVar2 | uVar3 + uVar1 [H/5 + uVar1] | 0xb0 [176] | 0x80 [128] | surface = g_frontendLanguageFlagsSurface_PROVISIONAL; src_x=0, src_y=0, color=0 | — | top-left flag (src_y=0) |
| 3 | case 0 | CreateFrontendMenuRectEntry @ 0x004258f0 | (g_frontendCanvasW - uVar2) + -0xb0 [W - uVar2 - 176] | uVar3 + uVar1 | 0xb0 [176] | 0x80 [128] | surface = flagsSurface; src_x=0, src_y=0x80 [128], color=0 | — | top-right flag (src_y=128) |
| 4 | case 0 | CreateFrontendMenuRectEntry @ 0x004258f0 | uVar2 | (g_frontendCanvasH - uVar1) + -0x80 [H - uVar1 - 128] | 0xb0 [176] | 0x80 [128] | surface = flagsSurface; src_x=0, src_y=0x100 [256], color=0 | — | bottom-left flag (src_y=256) |
| 5 | case 0 | CreateFrontendMenuRectEntry @ 0x004258f0 | (g_frontendCanvasW - uVar2) + -0xb0 [W - uVar2 - 176] | (g_frontendCanvasH - uVar1) + -0x80 [H - uVar1 - 128] | 0xb0 [176] | 0x80 [128] | surface = flagsSurface; src_x=0, src_y=0x180 [384], color=0 | — | bottom-right flag (src_y=384) |

Other case-0 calls (no x/y): LoadFrontendTgaSurfaceFromArchive(Language.tga), LoadTgaToFrontendSurface16bpp(LanguageScreen.tga), CopyPrimaryFrontendBufferToSecondary.
Anim var: g_frontendAnimFrameCounter reset in case1/2 and on hover-change in case4 (g_frontendButtonIndex vs g_languageSelectButtonHoverIndex_PROVISIONAL) — does not appear in any x/y expression.
Delegated sub-draw fns followed: none beyond the helpers above (placement is inline in case 0).
Confidence: [CONFIRMED @ 0x00427290] for all 5 rows. [NOTE] CreateFrontendMenuRectEntry @ 0x004258f0 is the actual placer (task list omitted it); its arg order is (x,y,w,h,src_x,src_y,color,surface) — same shape as QueueFrontendOverlayRect. [UNCERTAIN] DrawFrontendLocalizedStringPrimary param names (text,x,y) inferred from signature `(byte*, uint, int)`; the 2nd/3rd args are positional x/y by signature shape, not an explicit canvas comment.

---

### Screen 4 @ 0x004274a0 — ScreenLegalCopyright
Mechanism: canvas-relative tiled legal text. Loop count = (g_frontendCanvasH - 0x20 [32]) >> 5  (i.e. (H-32)/32 rows). Each row drawn by DrawFrontendLocalizedStringSecondary(sx, sy) with FLOAT args; x = (float)(g_frontendCanvasW / 10), y = first float arg (decompiler shows raw float bits 6.465804e-39 = a denormal — almost certainly a misdecode of the per-row y; see UNCERTAIN). "TEST DRIVE 5 COPYRIGHT 1998" tiled.
States/structure: switch(g_frontendInnerState) cases 0..3. case0 loads LegalScreen.tga + draws the tiled text + ResetFrontendFadeState; case1 fade-out + capture timeGetTime; case2 wait 2999ms+ then InitFrontendFadeColor(0); case3 fade then SetFrontendScreen(MAIN_MENU).

| # | State/case | Helper @addr | x (verbatim) [dec] | y (verbatim) | w | h | tex/label/surface | blit-flag | notes (anim/frame var) |
|---|---|---|---|---|---|---|---|---|---|
| 1 | case 0 (loop, uVar2=1; while uVar2 < (H-32)>>5) | DrawFrontendLocalizedStringSecondary @ 0x00424390 | (float)(g_frontendCanvasW / 10) [W/10] | 6.465804e-39 (raw float arg #1) [UNCERTAIN — see note] | — | — | implicit legal-text string (loaded from LegalScreen.tga / localized) | — | tiled per row; loop var uVar2 increments, gate `0x20 < (H-0x20 & 0xffffffe0)` then `uVar2 < (H-0x20)>>5` |

Other case-0 call (no x/y): LoadTgaToFrontendSurface16bppVariant(LegalScreen.tga). case2 dwell = 2999ms (`2999 < DVar1 - g_frontendInnerState`).
Delegated sub-draw fns followed: none.
Confidence: [CONFIRMED @ 0x004274a0] that the single tiled draw is DrawFrontendLocalizedStringSecondary with x=(float)(canvasW/10). [UNCERTAIN]: the y/first-float argument decompiles as the denormal literal 6.465804e-39 — the float calling-convention prevented Ghidra from recovering the per-row y expression (the loop var uVar2 is the row index and should scale y by ~0x20 per row, but that is NOT visible in the C output). The function signature is `void(float sx, float sy)`; mapping of which float is x vs y is by signature order (sx first), so the (float)(canvasW/10) is sx=x and the denormal is sy=y. Needs disasm-level confirmation of the FP stack to recover the true per-row y.

---

## Summary of placement rows captured
- Screen 0: 0 rows (no placement).
- Screen 1: 1 direct (case4 QueueFrontendOverlayRect) + 3 delegated (RenderPositionerGlyphStrip) = 4 overlay-rect rows.
- Screen 2: 0 rows.
- Screen 3: 5 rows (1 title string + 4 flag buttons).
- Screen 4: 1 row (tiled legal text).
Total positioned rows: 11 (10 if delegated strip counted as one table entry).
# Frontend Layout Harvest — Part 05 (screen indices 5–9)

Source: TD5_d3d.exe via Ghidra (read-only). Dispatch table `g_frontendScreenFnTable` @ 0x004655C4.
Decode of first 10 LE uint32 verified against my own `memory_read`:
idx0=0x004269d0, idx1=0x00415030, idx2=0x004275a0, idx3=0x00427290, idx4=0x004274a0,
**idx5=0x00415490, idx6=0x004168b0, idx7=0x004213d0, idx8=0x00418d50, idx9=0x00419cf0**.

Shared canvas mechanism (all 5 screens compute, at entry):
`uVarW = g_frontendCanvasW>>1` (canvasW>>1), `uVarH = g_frontendCanvasH>>1` (canvasH>>1),
`iVarX = (canvasW>>1)-0xd2` [-210], `iVarY = (canvasH>>1)-0x9f` [-159].
In tables below: **cW2** = `canvasW>>1`, **cH2** = `canvasH>>1`, **bx** = `(canvasW>>1)-0xd2` [-210], **by** = `(canvasH>>1)-0x9f` [-159].
`anim` = `g_frontendAnimFrameCounter`. Helper signatures confirmed per task brief.
`CreateFrontendDisplayModePreviewButton` (@ unverified addr, greyed/disabled variant) shares the (label,x,y,w,h,ud) signature; recorded same as the normal button.

---

### Screen 5 @ 0x00415490 — ScreenMainMenuAnd1PRaceFlow
Mechanism: canvas refs (cW2=canvasW>>1, cH2=canvasH>>1, bx=(canvasW>>1)-0xd2, by=(canvasH>>1)-0x9f)
States/structure: switch(g_frontendInnerState) cases 0,1,2,3,4,5,6,7,8,9,10(0xa),0xb,0xc,0x14,0x15,0x16,0x17. Main-menu button list built at end of case 0; 7 vertical buttons (auto-layout x=-0xe0). Sprite slots 0..6 = the 7 buttons.

| # | State/case | Helper @addr | x (verbatim) [dec] | y (verbatim) [dec] | w | h | tex/label/surface | blit-flag | notes |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 0 (init) | CreateFrontendDisplayModeButton @0x425de0 | -0xe0 [-224] (auto) | 0 | 0xe0 [224] | 0x20 [32] | SNK_RaceMenuButTxt | n/a | btn0 Race |
| 2 | 0 | CreateFrontendDisplayModeButton @0x425de0 | -0xe0 [-224] (auto) | 0 | 0xe0 [224] | 0x20 [32] | SNK_QuickRaceButTxt | n/a | btn1 Quick Race |
| 3 | 0 | CreateFrontendDisplayModeButton @0x425de0 | -0xe0 [-224] (auto) | 0 | 0xe0 [224] | 0x20 [32] | SNK_TimeDemoButTxt OR SNK_TwoPlayerButTxt | n/a | btn2; label = TwoPlayer if *(g_appExref+0x170)==0 else TimeDemo |
| 4 | 0 | CreateFrontendDisplayModeButton @0x425de0 | -0xe0 [-224] (auto) | 0 | 0xe0 [224] | 0x20 [32] | SNK_NetPlayButTxt | n/a | btn3 NetPlay |
| 5 | 0 | CreateFrontendDisplayModeButton @0x425de0 | -0xe0 [-224] (auto) | 0 | 0xe0 [224] | 0x20 [32] | SNK_OptionsButTxt | n/a | btn4 Options |
| 6 | 0 | CreateFrontendDisplayModeButton @0x425de0 | -0xe0 [-224] (auto) | 0 | 0xe0 [224] | 0x20 [32] | SNK_HiScoreButTxt | n/a | btn5 HiScore |
| 7 | 0 | CreateFrontendDisplayModeButton @0x425de0 | -0xe0 [-224] (auto) | 0 | 0xe0 [224] | 0x20 [32] | SNK_ExitButTxt | n/a | btn6 Exit; g_frontendEscKeyButtonIndex=6 |
| 8 | 3 (intro anim) | MoveFrontendSpriteRect @0x4259d0 | (anim+0xfffffda)*0x10+bx | cH2-0x93 [-147] | – | – | slot 0 | – | x slides from left (anim*16) |
| 9 | 3 | MoveFrontendSpriteRect @0x4259d0 | bx+anim*-0x10+0x280 [+640] | cH2-0x6b [-107] | – | – | slot 1 | – | slides from right |
| 10 | 3 | MoveFrontendSpriteRect @0x4259d0 | (anim+0xfffffda)*0x10+bx | cH2-0x43 [-67] | – | – | slot 2 | – | |
| 11 | 3 | MoveFrontendSpriteRect @0x4259d0 | bx+anim*-0x10+0x280 [+640] | cH2-0x1b [-27] | – | – | slot 3 | – | |
| 12 | 3 | MoveFrontendSpriteRect @0x4259d0 | (anim+0xfffffda)*0x10+bx | cH2+0xd [+13] | – | – | slot 4 | – | |
| 13 | 3 | MoveFrontendSpriteRect @0x4259d0 | bx+anim*-0x10+0x280 [+640] | cH2+0x35 [+53] | – | – | slot 5 | – | |
| 14 | 3 | MoveFrontendSpriteRect @0x4259d0 | (anim+0xfffffda)*0x10+bx | cH2+0x5d [+93] | – | – | slot 6 | – | |
| 15 | 3 | QueueFrontendOverlayRect @0x425660 | cW2-200 [-200] | ((anim*4+-0xdc)-g_menuHeaderLabelYOffset)+by | 0,0 | g_menuHeaderLabelSurfaceWidth, ...Height | g_currentScreenIndex | color=0 | header label anim; src(0,0) |
| 16 | 4 (idle) | QueueFrontendOverlayRect @0x425660 | cW2-200 [-200] | (by-g_menuHeaderLabelYOffset)+-0x40 [-64] | 0,0 | hdrW, hdrH | g_currentScreenIndex | 0 | static header |
| 17 | 4 (case6 sub "Exit?" dialog) | CreateFrontendDisplayModeButton @0x425de0 | cW2-0xc2 [-194] | cH2+0xa5 [+165] | 0x60 [96] | 0x20 [32] | SNK_YesButTxt | n/a | Yes button (absolute pos) |
| 18 | 4 (case6) | CreateFrontendDisplayModeButton @0x425de0 | cW2-0x42 [-66] | cH2+0xa5 [+165] | 0x60 [96] | 0x20 [32] | SNK_NoxButTxt | n/a | No button |
| 19 | 5 | QueueFrontendOverlayRect @0x425660 | cW2-200 [-200] | (by-g_menuHeaderLabelYOffset)+-0x40 [-64] | 0,0 | hdrW, hdrH | g_currentScreenIndex | 0 | header |
| 20 | 5 | QueueFrontendOverlayRect @0x425660 | cW2-200 [-200] | (by-g_menuHeaderLabelYOffset)+0x124 [+292] | 0,0 | g_frontendInlineStringActiveEntry, hdrH | g_oneRaceFlowSelectedCarId | 0 | sub-caption |
| 21 | 6 | QueueFrontendOverlayRect @0x425660 | cW2-200 [-200] | (by-g_menuHeaderLabelYOffset)+-0x40 [-64] | 0,0 | hdrW, hdrH | g_currentScreenIndex | 0 | header |
| 22 | 6 | QueueFrontendOverlayRect @0x425660 | cW2-200 [-200] | (by-g_menuHeaderLabelYOffset)+0x124 [+292] | 0,0 | g_frontendInlineStringActiveEntry, hdrH | g_oneRaceFlowSelectedCarId | 0 | sub-caption |
| 23 | 7 | QueueFrontendOverlayRect @0x425660 | cW2-200 [-200] | (by-g_menuHeaderLabelYOffset)+-0x40 [-64] | 0,0 | hdrW, hdrH | g_currentScreenIndex | 0 | header |
| 24 | 8 | QueueFrontendOverlayRect @0x425660 | cW2-200 [-200] | (by-g_menuHeaderLabelYOffset)+-0x40 [-64] | 0,0 | hdrW, hdrH | g_currentScreenIndex | 0 | header |
| 25 | 9 (outro anim) | QueueFrontendOverlayRect @0x425660 | bx+anim*-0x18+10 [+10] | (by-g_menuHeaderLabelYOffset)+-0x40 [-64] | 0,0 | hdrW, hdrH | g_currentScreenIndex | 0 | header slides; SAME call repeated once if anim!=0x10 |
| 26 | 9 | MoveFrontendSpriteRect @0x4259d0 | bx+anim*-0x10+0x10 [+16] | by+anim*-0x30+0xc [+12] | – | – | slot 0 | – | |
| 27 | 9 | MoveFrontendSpriteRect @0x4259d0 | bx+anim*-10+0x10 [+16] | by+anim*-0x20+0x34 [+52] | – | – | slot 1 | – | x: anim*-10(dec) |
| 28 | 9 | MoveFrontendSpriteRect @0x4259d0 | bx+anim*-8+0x10 [+16] | by+anim*-0x18+0x5c [+92] | – | – | slot 2 | – | |
| 29 | 9 | MoveFrontendSpriteRect @0x4259d0 | (cW2-0xc2)+anim*8 | (cH2-0x1b)+anim*0x18 | – | – | slot 3 | – | |
| 30 | 9 | MoveFrontendSpriteRect @0x4259d0 | (cW2-0xc2)+anim*10 [dec10] | anim*0x20+0xac+by [+172] | – | – | slot 4 | – | |
| 31 | 9 | MoveFrontendSpriteRect @0x4259d0 | (anim+1)*0x10+bx | anim*0x30+0xd4+by [+212] | – | – | slot 5 | – | |
| 32 | 9 | MoveFrontendSpriteRect @0x4259d0 | (cW2-0xc2)+anim*0x14 | cH2+0x5d+anim*0x38 [+93] | – | – | slot 6 | – | |
| 33 | 10 (0xa) | MoveFrontendSpriteRect @0x4259d0 | bx+anim*-0x10+0x40 [+64] | anim*0x10+0x114+by [+276] | – | – | slot 0 | – | only when anim>=3 |
| 34 | 10 | MoveFrontendSpriteRect @0x4259d0 | (anim+6)*0x10+bx | anim*0x10+0x114+by [+276] | – | – | slot 1 | – | anim>=3 |
| 35 | 10 | QueueFrontendOverlayRect @0x425660 | cW2-200 [-200] | (by-g_menuHeaderLabelYOffset)+-0x40 [-64] | 0,0 | hdrW, hdrH | g_currentScreenIndex | 0 | header |
| 36 | 10 | QueueFrontendOverlayRect @0x425660 | anim*0x30+10+bx [+10] | (by-g_menuHeaderLabelYOffset)+0x124 [+292] | 0,0 | g_frontendInlineStringActiveEntry, hdrH | g_oneRaceFlowSelectedCarId | 0 | sub-caption slides |
| 37 | 0xb | QueueFrontendOverlayRect @0x425660 | cW2-200 [-200] | (by-g_menuHeaderLabelYOffset)+-0x40 [-64] | 0,0 | hdrW, hdrH | g_currentScreenIndex | 0 | header |
| 38 | 0xc (Extras outro) | QueueFrontendOverlayRect @0x425660 | bx+anim*-0x18+10 [+10] | (by-g_menuHeaderLabelYOffset)+-0x40 [-64] | 0,0 | hdrW, hdrH | g_currentScreenIndex | 0 | header slides |
| 39 | 0xc | MoveFrontendSpriteRect @0x4259d0 | anim*0x30+0x10+bx [+16] | anim*0x10+0xc+by [+12] | – | – | slot 0 | – | |
| 40 | 0xc | MoveFrontendSpriteRect @0x4259d0 | anim*0x20+0x10+bx [+16] | anim*0x20+0x34+by [+52] | – | – | slot 1 | – | |
| 41 | 0xc | MoveFrontendSpriteRect @0x4259d0 | bx+anim*-0x20+0x10 [+16] | anim*0x20+0x5c+by [+92] | – | – | slot 2 | – | |
| 42 | 0xc | MoveFrontendSpriteRect @0x4259d0 | bx+anim*-0x30+0x10 [+16] | by+anim*-0x10+0x84 [+132] | – | – | slot 3 | – | |
| 43 | 0xc | MoveFrontendSpriteRect @0x4259d0 | bx+anim*-0x20+0x10 [+16] | by+anim*-0x20+0xac [+172] | – | – | slot 4 | – | |
| 44 | 0xc | MoveFrontendSpriteRect @0x4259d0 | anim*0x20+0x10+bx [+16] | by+anim*-0x20+0xd4 [+212] | – | – | slot 5 | – | |
| 45 | 0xc | MoveFrontendSpriteRect @0x4259d0 | anim*0x30+0x10+bx [+16] | by+anim*-0x10+0xfc [+252] | – | – | slot 6 | – | |
| 46 | 0x14 ("must select" dialog init) | CreateFrontendDisplayModeButton @0x425de0 | 0 | -0x60 [-96] (auto) | 0x60 [96] | 0x20 [32] | SNK_OkButTxt | n/a | OK button; dialog surf 0x1e2x0x40 created |
| 47 | 0x15 | QueueFrontendOverlayRect @0x425660 | cW2-200 [-200] | ((anim*8+-0xc0)-g_menuHeaderLabelYOffset)+by | 0,0 | hdrW, hdrH | g_currentScreenIndex | 0 | header |
| 48 | 0x15 | MoveFrontendSpriteRect @0x4259d0 | ((g_frontendCanvasW-bx)-0xa0>>1)+0x10+bx | (g_frontendCanvasH-0x6c>>1)+anim*-0x18+0x1d0 [+464] | – | – | slot 0 | – | OK button slides; complex centering |
| 49 | 0x16 | QueueFrontendOverlayRect @0x425660 | cW2-200 [-200] | (by-g_menuHeaderLabelYOffset)+-0x40 [-64] | 0,0 | hdrW, hdrH | g_currentScreenIndex | 0 | header |
| 50 | 0x16 | QueueFrontendOverlayRect @0x425660 | cW2-0xc2 [-194] | g_frontendCanvasH-0x6c>>1 [(canvasH-108)/2] | 0,0 | 0x1e2 [482] | 0x40 [64] | g_lobbyErrorDialogSurface | 0 | dialog box |
| 51 | 0x17 | MoveFrontendSpriteRect @0x4259d0 | anim*0x20+-0x50+bx+((g_frontendCanvasW-bx)-0xa0>>1) | (g_frontendCanvasH-0x6c>>1)+0x50 [+80] | – | – | slot 0 | – | only anim>=3 |
| 52 | 0x17 | QueueFrontendOverlayRect @0x425660 | bx+anim*-0x18+10 [+10] | (by-g_menuHeaderLabelYOffset)+-0x40 [-64] | 0,0 | hdrW, hdrH | g_currentScreenIndex | 0 | header slides |

Delegated sub-draw fns followed: none (all helper calls inline).
Confidence: [CONFIRMED @ 0x00415490; helper callees CreateFrontendDisplayModeButton 0x425de0, QueueFrontendOverlayRect 0x425660, MoveFrontendSpriteRect 0x4259d0 by address]

---

### Screen 6 @ 0x004168b0 — RaceTypeCategoryMenuStateMachine
Mechanism: canvas refs (cW2, cH2, bx=(canvasW>>1)-0xd2 [-210], by=(canvasH>>1)-0x9f [-159])
States/structure: switch cases 0,1,2,3,4,5,6,7,8,9,10(0xa),0xb,0xc,0x14. Two menu pages: race-type list (case 0/0xb init) and championship/era sub-list (case 6 init). 6–7 sprite-slot buttons (auto x=-0xe0); description-box dialog surface 0x110x0xb4. **Description text** drawn into g_lobbyErrorDialogSurface via DrawFrontendLocalizedStringToSurface/DrawFrontendSmallFontStringToSurface (state 4/9) at small-font line stride 0xc starting y=0x20; not a fixed-screen blit (carries surface-local x/y only) — recorded as text-fill, not a screen placement.

| # | State/case | Helper @addr | x (verbatim) [dec] | y (verbatim) [dec] | w | h | tex/label/surface | blit-flag | notes |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 0 (init list) | CreateFrontendDisplayModeButton @0x425de0 | -0xe0 [-224] (auto) | 0 | 0xe0 [224] | 0x20 [32] | SNK_SingleRaceButTxt | n/a | btn0 |
| 2 | 0 | CreateFrontendDisplayModeButton @0x425de0 | -0xe0 [-224] (auto) | 0 | 0xe0 [224] | 0x20 [32] | SNK_CupRaceButTxt | n/a | btn1 |
| 3 | 0 | CreateFrontendDisplayMode(Preview)Button @0x425de0 | -0xe0 [-224] (auto) | 0 | 0xe0 [224] | 0x20 [32] | SNK_ContCupButTxt | n/a | btn2; Preview(greyed) if ValidateCupDataChecksum==0 else normal |
| 4 | 0 | CreateFrontendDisplayModeButton @0x425de0 | -0xe0 [-224] (auto) | 0 | 0xe0 [224] | 0x20 [32] | SNK_TimeTrialsButTxt | n/a | btn3 |
| 5 | 0 | CreateFrontendDisplayModeButton @0x425de0 | -0xe0 [-224] (auto) | 0 | 0xe0 [224] | 0x20 [32] | SNK_DragRaceButTxt | n/a | btn4 |
| 6 | 0 | CreateFrontendDisplayModeButton @0x425de0 | -0xe0 [-224] (auto) | 0 | 0xe0 [224] | 0x20 [32] | SNK_CopChaseButTxt | n/a | btn5 |
| 7 | 0 | CreateFrontendDisplayModeButton @0x425de0 | -0xe0 [-224] (auto) | 0 | 0x70 [112] | 0x20 [32] | SNK_BackButTxt | n/a | btn6 Back (narrow); EscKeyButtonIndex=6 |
| 8 | 1 (intro anim) | MoveFrontendSpriteRect @0x4259d0 | cW2-200 [-200] | anim*0x29+by+-0x510 [-1296] | – | – | slot 0 | – | iVar1=cW2-200 |
| 9 | 1 | MoveFrontendSpriteRect @0x4259d0 | cW2-200 [-200] | (cH2-0x4e7 [-1255])+anim*0x24 | – | – | slot 1 | – | |
| 10 | 1 | MoveFrontendSpriteRect @0x4259d0 | cW2-200 [-200] | anim*0x1f+-0x380+by [-896] | – | – | slot 2 | – | |
| 11 | 1 | MoveFrontendSpriteRect @0x4259d0 | cW2-200 [-200] | (cH2-0x357 [-855])+anim*0x1a | – | – | slot 3 | – | |
| 12 | 1 | MoveFrontendSpriteRect @0x4259d0 | cW2-200 [-200] | by+anim*0x15+-0x1f0 [-496] | – | – | slot 4 | – | |
| 13 | 1 | MoveFrontendSpriteRect @0x4259d0 | cW2-200 [-200] | anim*0x10+-0x128+by [-296] | – | – | slot 5 | – | |
| 14 | 1 | MoveFrontendSpriteRect @0x4259d0 | cW2-0x90 [-144] | by+anim*-0x1f+0x508 [+1288] | – | – | slot 6 | – | Back btn from right |
| 15 | 1 | QueueFrontendOverlayRect @0x425660 | bx+anim*-0x18+0x30a [+778] | (by-g_menuHeaderLabelYOffset)+-0x40 [-64] | 0,0 | hdrW, hdrH | g_currentScreenIndex | 0 | header |
| 16 | 1 | QueueFrontendOverlayRect @0x425660 | cW2+0x26 [+38] | cH2-0x5f [-95] | 0,0 | 0x110 [272] | 0xb4 [180] | g_lobbyErrorDialogSurface | 0 | description box |
| 17 | 2 | QueueFrontendOverlayRect @0x425660 | cW2-200 [-200] | (by-g_menuHeaderLabelYOffset)+-0x40 [-64] | 0,0 | hdrW, hdrH | g_currentScreenIndex | 0 | header |
| 18 | 3 | QueueFrontendOverlayRect @0x425660 | cW2-200 [-200] | (by-g_menuHeaderLabelYOffset)+-0x40 [-64] | 0,0 | hdrW, hdrH | g_currentScreenIndex | 0 | header |
| 19 | 3 | QueueFrontendOverlayRect @0x425660 | cW2+0x26 [+38] | cH2-0x5f [-95] | 0,0 | 0x110 [272] | 0xb4 [180] | g_lobbyErrorDialogSurface | 0 | desc box |
| 20 | 4 | QueueFrontendOverlayRect @0x425660 | cW2-200 [-200] | (by-g_menuHeaderLabelYOffset)+-0x40 [-64] | 0,0 | hdrW, hdrH | g_currentScreenIndex | 0 | header |
| 21 | 4 | QueueFrontendOverlayRect @0x425660 | cW2+0x26 [+38] | cH2-0x5f [-95] | 0,0 | 0x110 [272] | 0xb4 [180] | g_lobbyErrorDialogSurface | 0 | desc box (LAB_00416edd) |
| 22 | 5 (outro→car select) | QueueFrontendOverlayRect @0x425660 | bx+anim*-0x18+10 [+10] | (by-g_menuHeaderLabelYOffset)+-0x40 [-64] | 0,0 | hdrW, hdrH | g_currentScreenIndex | 0 | header slides |
| 23 | 5 | QueueFrontendOverlayRect @0x425660 | cW2+0x26 [+38] | (anim+2)*0x20+by | 0,0 | 0x110 [272] | 0xb4 [180] | g_lobbyErrorDialogSurface | 0 | desc box slides down |
| 24 | 5 | MoveFrontendSpriteRect @0x4259d0 | bx+anim*-0x40+0x8a [+138] | cH2-0x8f [-143] | – | – | slot 0 | – | anim>=3 |
| 25 | 5 | MoveFrontendSpriteRect @0x4259d0 | (cW2-0x138 [-312])+anim*0x38 | cH2-0x67 [-103] | – | – | slot 1 | – | |
| 26 | 5 | MoveFrontendSpriteRect @0x4259d0 | bx+anim*-0x30+0x6a [+106] | cH2-0x3f [-63] | – | – | slot 2 | – | |
| 27 | 5 | MoveFrontendSpriteRect @0x4259d0 | (cW2-0x118 [-280])+anim*0x28 | cH2-0x17 [-23] | – | – | slot 3 | – | |
| 28 | 5 | MoveFrontendSpriteRect @0x4259d0 | bx+anim*-0x20+0x4a [+74] | cH2+0x11 [+17] | – | – | slot 4 | – | |
| 29 | 5 | MoveFrontendSpriteRect @0x4259d0 | (cW2-0xf8 [-248])+anim*0x18 | cH2+0x39 [+57] | – | – | slot 5 | – | |
| 30 | 5 | MoveFrontendSpriteRect @0x4259d0 | bx+anim*-0x1f+0x80 [+128] (=iVar8) | cH2+0x89 [+137] | – | – | slot 6 (LAB_00417893) | – | Back btn |
| 31 | 6 (championship list init) | CreateFrontendDisplayModeButton @0x425de0 | -0xe0 [-224] (auto) | 0 | 0xe0 [224] | 0x20 [32] | SNK_ChampionshipButTxt | n/a | btn0 |
| 32 | 6 | CreateFrontendDisplayModeButton @0x425de0 | -0xe0 [-224] (auto) | 0 | 0xe0 [224] | 0x20 [32] | SNK_EraButTxt | n/a | btn1 |
| 33 | 6 | CreateFrontendDisplayMode(Preview)Button @0x425de0 | -0xe0 [-224] (auto) | 0 | 0xe0 [224] | 0x20 [32] | SNK_ChallengeButTxt | n/a | btn2; Preview if cheatGameModes==0, else normal |
| 34 | 6 | CreateFrontendDisplayMode(Preview)Button @0x425de0 | -0xe0 [-224] (auto) | 0 | 0xe0 [224] | 0x20 [32] | SNK_PitbullButTxt | n/a | btn3; gating by cheatGameModes 0/1/else |
| 35 | 6 | CreateFrontendDisplayMode(Preview)Button @0x425de0 | -0xe0 [-224] (auto) | 0 | 0xe0 [224] | 0x20 [32] | SNK_MastersButTxt | n/a | btn4; Preview unless cheatGameModes>=2 |
| 36 | 6 | CreateFrontendDisplayMode(Preview)Button @0x425de0 | -0xe0 [-224] (auto) | 0 | 0xe0 [224] | 0x20 [32] | SNK_UltimateButTxt | n/a | btn5; Preview unless cheatGameModes>=2 |
| 37 | 6 | CreateFrontendDisplayModeButton @0x425de0 | cW2-0x90 [-144] | cH2+0x89 [+137] | 0x70 [112] | 0x20 [32] | SNK_BackButTxt | n/a | btn6 Back (absolute); EscKeyButtonIndex=6 |
| 38 | 6 (intro before init) | QueueFrontendOverlayRect @0x425660 | cW2-200 [-200] (=iVar1) | (by-g_menuHeaderLabelYOffset)+-0x40 [-64] | 0,0 | hdrW, hdrH | g_currentScreenIndex | 0 | header |
| 39 | 6 | QueueFrontendOverlayRect @0x425660 | cW2+0x26 [+38] | cH2-0x5f [-95] | 0,0 | 0x110 [272] | 0xb4 [180] | g_lobbyErrorDialogSurface | 0 | desc box |
| 40 | 6 | MoveFrontendSpriteRect @0x4259d0 | bx+anim*-0x40+0x8a [+138] | cH2-0x8f [-143] | – | – | slot 0 | – | anim>=3 |
| 41 | 6 | MoveFrontendSpriteRect @0x4259d0 | (cW2-0x138 [-312])+anim*0x38 | cH2-0x67 [-103] | – | – | slot 1 | – | |
| 42 | 6 | MoveFrontendSpriteRect @0x4259d0 | bx+anim*-0x30+0x6a [+106] | cH2-0x3f [-63] | – | – | slot 2 | – | |
| 43 | 6 | MoveFrontendSpriteRect @0x4259d0 | (cW2-0x118 [-280])+anim*0x28 | cH2-0x17 [-23] | – | – | slot 3 | – | |
| 44 | 6 | MoveFrontendSpriteRect @0x4259d0 | bx+anim*-0x20+0x4a [+74] | cH2+0x11 [+17] | – | – | slot 4 | – | |
| 45 | 6 | MoveFrontendSpriteRect @0x4259d0 | (cW2-0xf8 [-248])+anim*0x18 | cH2+0x39 [+57] | – | – | slot 5 | – | |
| 46 | 7 (championship intro) | QueueFrontendOverlayRect @0x425660 | cW2-200 [-200] (=iVar10) | (by-g_menuHeaderLabelYOffset)+-0x40 [-64] | 0,0 | hdrW, hdrH | g_currentScreenIndex | 0 | header |
| 47 | 7 | QueueFrontendOverlayRect @0x425660 | cW2+0x26 [+38] | cH2-0x5f [-95] | 0,0 | 0x110 [272] | 0xb4 [180] | g_lobbyErrorDialogSurface | 0 | desc box |
| 48 | 7 | MoveFrontendSpriteRect @0x4259d0 | cW2-200 [-200] | anim*0x20+-0x3f0+by [-1008] | – | – | slot 0 | – | |
| 49 | 7 | MoveFrontendSpriteRect @0x4259d0 | cW2-200 [-200] | (cH2-999 [-999dec])+anim*0x1c | – | – | slot 1 | – | 999 dec literal |
| 50 | 7 | MoveFrontendSpriteRect @0x4259d0 | cW2-200 [-200] | (cH2-0x33f [-831])+anim*0x18 | – | – | slot 2 | – | |
| 51 | 7 | MoveFrontendSpriteRect @0x4259d0 | cW2-200 [-200] | (cH2-0x297 [-663])+anim*0x14 | – | – | slot 3 | – | |
| 52 | 7 | MoveFrontendSpriteRect @0x4259d0 | cW2-200 [-200] | by+(anim*9+0x7fffff38)*2 | – | – | slot 4 | – | overflow expr (==by+anim*18-0x190) |
| 53 | 7 | MoveFrontendSpriteRect @0x4259d0 | cW2-200 [-200] | anim*0x10+-0x128+by [-296] | – | – | slot 5 | – | |
| 54 | 8 | QueueFrontendOverlayRect @0x425660 | cW2-200 [-200] | (by-g_menuHeaderLabelYOffset)+-0x40 [-64] | 0,0 | hdrW, hdrH | g_currentScreenIndex | 0 | header |
| 55 | 8 | QueueFrontendOverlayRect @0x425660 | cW2+0x26 [+38] | cH2-0x5f [-95] | 0,0 | 0x110 [272] | 0xb4 [180] | g_lobbyErrorDialogSurface | 0 | desc box |
| 56 | 9 | QueueFrontendOverlayRect @0x425660 | cW2-200 [-200] | (by-g_menuHeaderLabelYOffset)+-0x40 [-64] | 0,0 | hdrW, hdrH | g_currentScreenIndex | 0 | header |
| 57 | 9 | QueueFrontendOverlayRect @0x425660 | cW2+0x26 [+38] | cH2-0x5f [-95] | 0,0 | 0x110 [272] | 0xb4 [180] | g_lobbyErrorDialogSurface | 0 | desc box (after text-fill) |
| 58 | 10 (0xa, outro) | QueueFrontendOverlayRect @0x425660 | bx+anim*-0x18+10 [+10] | (by-g_menuHeaderLabelYOffset)+-0x40 [-64] | 0,0 | hdrW, hdrH | g_currentScreenIndex | 0 | header slides |
| 59 | 10 | QueueFrontendOverlayRect @0x425660 | cW2+0x26 [+38] | (anim+2)*0x20+by | 0,0 | 0x110 [272] | 0xb4 [180] | g_lobbyErrorDialogSurface | 0 | desc box slides |
| 60 | 10 | MoveFrontendSpriteRect @0x4259d0 | bx+anim*-0x18+0x3a [+58] | cH2-0x8f [-143] | – | – | slot 0 | – | anim>=3 |
| 61 | 10 | MoveFrontendSpriteRect @0x4259d0 | (cW2-0x100 [-256])+anim*0x1c | cH2-0x67 [-103] | – | – | slot 1 | – | |
| 62 | 10 | MoveFrontendSpriteRect @0x4259d0 | bx+anim*-0x20+0x4a [+74] | cH2-0x3f [-63] | – | – | slot 2 | – | |
| 63 | 10 | MoveFrontendSpriteRect @0x4259d0 | anim*0x20+-0x36+bx [-54] | cH2-0x17 [-23] | – | – | slot 3 | – | |
| 64 | 10 | MoveFrontendSpriteRect @0x4259d0 | bx+anim*-0x1c+0x42 [+66] | cH2+0x11 [+17] | – | – | slot 4 | – | |
| 65 | 10 | MoveFrontendSpriteRect @0x4259d0 | (cW2-0xf8 [-248])+anim*0x18 | cH2+0x39 [+57] | – | – | slot 5 | – | |
| 66 | 10 | MoveFrontendSpriteRect @0x4259d0 | bx+anim*-0x20+0x82 [+130] (=iVar8) | cH2+0x89 [+137] | – | – | slot 6 (LAB_00417893) | – | Back |
| 67 | 0xb (back→race-type list rebuild + intro) | CreateFrontendDisplayModeButton @0x425de0 | -0xe0 [-224] (auto) | 0 | 0xe0 [224] | 0x20 [32] | SNK_SingleRaceButTxt | n/a | btn0 (rebuild) |
| 68 | 0xb | CreateFrontendDisplayModeButton @0x425de0 | -0xe0 [-224] (auto) | 0 | 0xe0 [224] | 0x20 [32] | SNK_CupRaceButTxt | n/a | btn1 |
| 69 | 0xb | CreateFrontendDisplayMode(Preview)Button @0x425de0 | -0xe0 [-224] (auto) | 0 | 0xe0 [224] | 0x20 [32] | SNK_ContCupButTxt | n/a | btn2 (Preview if cup checksum==0) |
| 70 | 0xb | CreateFrontendDisplayModeButton @0x425de0 | -0xe0 [-224] (auto) | 0 | 0xe0 [224] | 0x20 [32] | SNK_TimeTrialsButTxt | n/a | btn3 |
| 71 | 0xb | CreateFrontendDisplayModeButton @0x425de0 | -0xe0 [-224] (auto) | 0 | 0xe0 [224] | 0x20 [32] | SNK_DragRaceButTxt | n/a | btn4 |
| 72 | 0xb | CreateFrontendDisplayModeButton @0x425de0 | -0xe0 [-224] (auto) | 0 | 0xe0 [224] | 0x20 [32] | SNK_CopChaseButTxt | n/a | btn5 |
| 73 | 0xb | CreateFrontendDisplayModeButton @0x425de0 | cW2-0x90 [-144] | cH2+0x89 [+137] | 0x70 [112] | 0x20 [32] | SNK_BackButTxt | n/a | btn6 Back (absolute); EscKeyButtonIndex=6 |
| 74 | 0xb | QueueFrontendOverlayRect @0x425660 | cW2-200 [-200] (=iVar1) | (by-g_menuHeaderLabelYOffset)+-0x40 [-64] | 0,0 | hdrW, hdrH | g_currentScreenIndex | 0 | header |
| 75 | 0xb | QueueFrontendOverlayRect @0x425660 | cW2+0x26 [+38] | cH2-0x5f [-95] | 0,0 | 0x110 [272] | 0xb4 [180] | g_lobbyErrorDialogSurface | 0 | desc box |
| 76 | 0xb | MoveFrontendSpriteRect @0x4259d0 | bx+anim*-0x20+0x4a [+74] | cH2-0x8f [-143] | – | – | slot 0 | – | anim>=3 |
| 77 | 0xb | MoveFrontendSpriteRect @0x4259d0 | anim*0x20+-0x36+bx [-54] | cH2-0x67 [-103] | – | – | slot 1 | – | |
| 78 | 0xb | MoveFrontendSpriteRect @0x4259d0 | bx+anim*-0x18+0x3a [+58] | cH2-0x3f [-63] | – | – | slot 2 | – | |
| 79 | 0xb | MoveFrontendSpriteRect @0x4259d0 | (cW2-0xf8 [-248])+anim*0x18 | cH2-0x17 [-23] | – | – | slot 3 | – | |
| 80 | 0xb | MoveFrontendSpriteRect @0x4259d0 | bx+anim*-0x20+0x4a [+74] | cH2+0x11 [+17] | – | – | slot 4 | – | |
| 81 | 0xb | MoveFrontendSpriteRect @0x4259d0 | anim*0x20+-0x36+bx [-54] | cH2+0x39 [+57] | – | – | slot 5 | – | |
| 82 | 0xc (back intro race-type) | QueueFrontendOverlayRect @0x425660 | cW2-200 [-200] (=iVar10) | (by-g_menuHeaderLabelYOffset)+-0x40 [-64] | 0,0 | hdrW, hdrH | g_currentScreenIndex | 0 | header |
| 83 | 0xc | QueueFrontendOverlayRect @0x425660 | cW2+0x26 [+38] | cH2-0x5f [-95] | 0,0 | 0x110 [272] | 0xb4 [180] | g_lobbyErrorDialogSurface | 0 | desc box |
| 84 | 0xc | MoveFrontendSpriteRect @0x4259d0 | cW2-200 [-200] | by+anim*0x29+-0x510 [-1296] | – | – | slot 0 | – | |
| 85 | 0xc | MoveFrontendSpriteRect @0x4259d0 | cW2-200 [-200] | (cH2-0x4e7 [-1255])+anim*0x24 | – | – | slot 1 | – | |
| 86 | 0xc | MoveFrontendSpriteRect @0x4259d0 | cW2-200 [-200] | anim*0x1f+-0x380+by [-896] | – | – | slot 2 | – | |
| 87 | 0xc | MoveFrontendSpriteRect @0x4259d0 | cW2-200 [-200] | (cH2-0x357 [-855])+anim*0x1a | – | – | slot 3 | – | |
| 88 | 0xc | MoveFrontendSpriteRect @0x4259d0 | cW2-200 [-200] | by+anim*0x15+-0x1f0 [-496] | – | – | slot 4 | – | |
| 89 | 0xc | MoveFrontendSpriteRect @0x4259d0 | cW2-200 [-200] | anim*0x10+-0x128+by [-296] | – | – | slot 5 | – | |

Delegated sub-draw fns followed: none (inline). Description-box text-fill (state 4/9) uses DrawFrontendLocalizedStringToSurface (no x/y args in decomp) + DrawFrontendSmallFontStringToSurface(str, x=MeasureOrCenterFrontendString result, y from 0x20 step 0xc, surface) — surface-local, not screen placement.
Confidence: [CONFIRMED @ 0x004168b0; callees by address 0x425de0/0x425660/0x4259d0]. [UNCERTAIN: which exact entries use the Preview (greyed) variant 0x425de0+? — the decomp names it CreateFrontendDisplayModePreviewButton; its address not separately confirmed against task's listed helpers, gating logic recorded literally.]

---

### Screen 7 @ 0x004213d0 — ScreenQuickRaceMenu
Mechanism: canvas refs (cW2, cH2, bx=(canvasW>>1)-0xd2 [-210], by=(canvasH>>1)-0x9f [-159])
States/structure: switch cases 0,1,2,3,4,5,6. 4 buttons (ChangeCar/ChangeTrack/OK/Back). Car/track preview drawn into g_lobbyErrorDialogSurface (0x208x200). InitializeFrontendDisplayModeArrows for slots 0,1 (cycle arrows).

| # | State/case | Helper @addr | x (verbatim) [dec] | y (verbatim) [dec] | w | h | tex/label/surface | blit-flag | notes |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 0 (init) | CreateFrontendDisplayModeButton @0x425de0 | cW2-200 [-200] (=iVar4) | cH2-0x67 [-103] | 0x100 [256] | 0x20 [32] | SNK_ChangeCarButTxt | n/a | btn0 (absolute) |
| 2 | 0 | CreateFrontendDisplayModeButton @0x425de0 | cW2-200 [-200] | cH2+0x11 [+17] | 0x100 [256] | 0x20 [32] | SNK_ChangeTrackButTxt | n/a | btn1 |
| 3 | 0 | CreateFrontendDisplayModeButton @0x425de0 | cW2-200 [-200] | cH2+0x89 [+137] | 0x60 [96] | 0x20 [32] | SNK_OkButTxt | n/a | btn2 OK |
| 4 | 0 | CreateFrontendDisplayModeButton @0x425de0 | cW2-0x58 [-88] | cH2+0x89 [+137] | 0x70 [112] | 0x20 [32] | SNK_BackButTxt | n/a | btn3 Back; EscKeyButtonIndex=3 |
| 5 | 0 | MoveFrontendSpriteRect @0x4259d0 | 0xffffff20 [-224] | 0 | – | – | slot 0 | – | offscreen park (=-0xe0) |
| 6 | 0 | MoveFrontendSpriteRect @0x4259d0 | 0xffffff20 [-224] | 0 | – | – | slot 1 | – | offscreen park |
| 7 | 0 | MoveFrontendSpriteRect @0x4259d0 | 0xffffff20 [-224] | 0 | – | – | slot 2 | – | offscreen park |
| 8 | 0 | MoveFrontendSpriteRect @0x4259d0 | 0xffffff20 [-224] | 0 | – | – | slot 3 | – | offscreen park; +InitializeFrontendDisplayModeArrows(0,1)&(1,1) |
| 9 | 3 (intro anim) | MoveFrontendSpriteRect @0x4259d0 | anim*0x10+-0x266+bx [-614] | cH2-0x67 [-103] | – | – | slot 0 | – | |
| 10 | 3 | MoveFrontendSpriteRect @0x4259d0 | bx+anim*-0x10+0x27a [+634] | cH2+0x11 [+17] | – | – | slot 1 | – | |
| 11 | 3 | MoveFrontendSpriteRect @0x4259d0 | anim*0x10+-0x266+bx [-614] | cH2+0x89 [+137] | – | – | slot 2 | – | |
| 12 | 3 | MoveFrontendSpriteRect @0x4259d0 | bx+anim*-0x10+0x2ea [+746] | cH2+0x89 [+137] | – | – | slot 3 | – | |
| 13 | 3 | QueueFrontendOverlayRect @0x425660 | cW2-200 [-200] | ((anim*4+-0xdc)-g_menuHeaderLabelYOffset)+by | 0,0 | hdrW, hdrH | g_currentScreenIndex | 0 | header |
| 14 | 3 | QueueFrontendOverlayRect @0x425660 | cW2-200 [-200] | by+anim*-0x18+0x3b8 [+952] | 0,0 | 0x208 [520] | 200 [0xc8] | g_lobbyErrorDialogSurface | 0 | preview box slides up |
| 15 | 4 (idle) | QueueFrontendOverlayRect @0x425660 | cW2-200 [-200] | (by-g_menuHeaderLabelYOffset)+-0x40 [-64] | 0,0 | hdrW, hdrH | g_currentScreenIndex | 0 | header |
| 16 | 4 | QueueFrontendOverlayRect @0x425660 | cW2-200 [-200] | cH2-0x8f [-143] | 0,0 | 0x208 [520] | 200 [0xc8] | g_lobbyErrorDialogSurface | 0 | preview box |
| 17 | 5 | QueueFrontendOverlayRect @0x425660 | cW2-200 [-200] | (by-g_menuHeaderLabelYOffset)+-0x40 [-64] | 0,0 | hdrW, hdrH | g_currentScreenIndex | 0 | header |
| 18 | 5 | QueueFrontendOverlayRect @0x425660 | cW2-200 [-200] | cH2-0x8f [-143] | 0,0 | 0x208 [520] | 200 [0xc8] | g_lobbyErrorDialogSurface | 0 | preview box |
| 19 | 6 (outro) | QueueFrontendOverlayRect @0x425660 | bx+anim*-0x18+10 [+10] | (by-g_menuHeaderLabelYOffset)+-0x40 [-64] | 0,0 | hdrW, hdrH | g_currentScreenIndex | 0 | header slides |
| 20 | 6 | MoveFrontendSpriteRect @0x4259d0 | bx+anim*-8+10 [+10] | by+anim*-0x20+0xb0 [+176] | – | – | slot 0 | – | |
| 21 | 6 | MoveFrontendSpriteRect @0x4259d0 | bx+anim*-6+10 [+10] | by+anim*-0x18+0xd8 [+216] | – | – | slot 1 | – | |
| 22 | 6 | MoveFrontendSpriteRect @0x4259d0 | (cW2-200 [-200])+anim*6 | cH2+0x89+anim*0x18 [+137] | – | – | slot 2 | – | |
| 23 | 6 | MoveFrontendSpriteRect @0x4259d0 | (cW2-0x58 [-88])+anim*8 | anim*0x20+0x128+by [+296] | – | – | slot 3 | – | |
| 24 | 6 | QueueFrontendOverlayRect @0x425660 | anim*0x20+10+bx [+10] | cH2-0x8f [-143] | 0,0 | 0x208 [520] | 200 [0xc8] | g_lobbyErrorDialogSurface | 0 | preview box slides |

Delegated sub-draw fns followed: none. Preview content drawn into g_lobbyErrorDialogSurface via DrawFrontendLocalizedStringToSurface (no x/y in decomp) + BltColorFillToSurface fills (e.g. (0,0,0,0x208,0x40,...) / (0,0,0x78,0x208,0x40,...)) — surface-local, not screen placement.
Confidence: [CONFIRMED @ 0x004213d0; callees by address]. Note 0xffffff20 = signed -224 (off-screen park = -0xe0).

---

### Screen 8 @ 0x00418d50 — RunFrontendConnectionBrowser
Mechanism: canvas refs (cW2, cH2, bx=(canvasW>>1)-0xd2 [-210], by=(canvasH>>1)-0x9f [-159]). NetPlay/DirectPlay [ARCH-DIVERGENCE: DXPTYPE].
States/structure: switch cases 0,1,2,3,4,5,6,8,9,10(0xa),0xb. List browser: a big "ChooseConnection" panel button (slot0, w=0x1f0 h=0x80) + OK + Back. Scroll/selection sprites drawn from `g_connBrowserListOriginX_PROVISIONAL[0].origin_x/origin_y` (runtime-loaded panel origin, NOT a literal — flagged).

| # | State/case | Helper @addr | x (verbatim) [dec] | y (verbatim) [dec] | w | h | tex/label/surface | blit-flag | notes |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 1 (init buttons) | CreateFrontendDisplayModeButton @0x425de0 | -0x1f0 [-496] (auto) | cH2-0x2f [-47] | 0x1f0 [496] | 0x80 [128] | SNK_ChooseConnectionButTxt | n/a | slot0 list panel; ptr→g_postRaceNameButtonSurfacePtr |
| 2 | 1 | CreateFrontendDisplayModeButton @0x425de0 | -0x1f0 [-496] (auto) | cH2+0x89 [+137] | 0x60 [96] | 0x20 [32] | SNK_OkButTxt | n/a | slot1 OK |
| 3 | 1 | CreateFrontendDisplayModeButton @0x425de0 | -0x1f0 [-496] (auto) | cH2+0x89 [+137] | 0x70 [112] | 0x20 [32] | SNK_BackButTxt | n/a | slot2 Back; EscKeyButtonIndex=2 |
| 4 | 2 (intro anim) | MoveFrontendSpriteRect @0x4259d0 | anim*0x20+-0x276+bx [-630] | cH2-0x2f [-47] | – | – | slot 0 | – | |
| 5 | 2 | MoveFrontendSpriteRect @0x4259d0 | (cW2-0x2a8 [-680])+anim*0x18 | cH2+0x89 [+137] | – | – | slot 1 | – | |
| 6 | 2 | MoveFrontendSpriteRect @0x4259d0 | bx+anim*-0x18+0x25a [+602] | cH2+0x89 [+137] | – | – | slot 2 | – | |
| 7 | 2 | QueueFrontendOverlayRect @0x425660 | cW2-200 [-200] | ((anim*4+-0x90)-g_menuHeaderLabelYOffset)+by | 0,0 | hdrW, hdrH | g_currentScreenIndex | 0 | header |
| 8 | 3 | QueueFrontendOverlayRect @0x425660 | cW2-200 [-200] | (by-g_menuHeaderLabelYOffset)+-0x40 [-64] | 0,0 | hdrW, hdrH | g_currentScreenIndex | 0 | header |
| 9 | 4 | QueueFrontendOverlayRect @0x425660 | cW2-200 [-200] | (by-g_menuHeaderLabelYOffset)+-0x40 [-64] | 0,0 | hdrW, hdrH | g_currentScreenIndex | 0 | header |
| 10 | 5 | QueueFrontendOverlayRect @0x425660 | cW2-200 [-200] | (by-g_menuHeaderLabelYOffset)+-0x40 [-64] | 0,0 | hdrW, hdrH | g_currentScreenIndex | 0 | header |
| 11 | 5 | QueueFrontendOverlayRect @0x425660 | listOrigin.origin_x+0xf2 [+242] | listOrigin.origin_y+0x1e [+30] | 0,0 | 0xc [12] | 0xc [12] | g_browserScrollIndicatorSprite | color 0xff0000 | up-scroll arrow (if scrollOffset!=0); origin from runtime struct |
| 12 | 5 | QueueFrontendOverlayRect @0x425660 | listOrigin.origin_x+0xf2 [+242] | listOrigin.origin_y+0x6c [+108] | 0,0xc | 0xc [12] | 0xc [12] | g_browserScrollIndicatorSprite | 0xff0000 | down-scroll arrow; src_y=0xc |
| 13 | 5 | QueueFrontendOverlayRect @0x425660 | listOrigin.origin_x+0x10 [+16] | (cursorIdx-scrollOff)*0x10+0x2f+origin_y [+47] | 0,0x1b | 0xc [12] | 9 | g_browserSelectionBarSprite | 0xff0000 | selection bar; src_y=0x1b |
| 14 | 6 | QueueFrontendOverlayRect @0x425660 | cW2-200 [-200] | (by-g_menuHeaderLabelYOffset)+-0x40 [-64] | 0,0 | hdrW, hdrH | g_currentScreenIndex | 0 | header |
| 15 | 6 | QueueFrontendOverlayRect @0x425660 | listOrigin.origin_x+0xf2 [+242] | listOrigin.origin_y+0x1e [+30] | 0,0 | 0xc [12] | 0xc [12] | g_browserScrollIndicatorSprite | 0xff0000 | up arrow |
| 16 | 6 | QueueFrontendOverlayRect @0x425660 | listOrigin.origin_x+0xf2 [+242] | listOrigin.origin_y+0x6c [+108] | 0,0xc | 0xc [12] | 0xc [12] | g_browserScrollIndicatorSprite | 0xff0000 | down arrow; src_y=0xc |
| 17 | 6 | QueueFrontendOverlayRect @0x425660 | listOrigin.origin_x+0x10 [+16] | (cursorIdx-scrollOff)*0x10+0x2f+origin_y [+47] | 0,0x1b | 0xc [12] | 9 | g_browserSelectionBarSprite | 0xff0000 | selection bar (blink anim&0x10); src_y=0x1b |
| 18 | 8 | QueueFrontendOverlayRect @0x425660 | cW2-200 [-200] | (by-g_menuHeaderLabelYOffset)+-0x40 [-64] | 0,0 | hdrW, hdrH | g_currentScreenIndex | 0 | header |
| 19 | 9 (outro) | QueueFrontendOverlayRect @0x425660 | cW2-200 [-200] | (by-g_menuHeaderLabelYOffset)+-0x40 [-64] | 0,0 | hdrW, hdrH | g_currentScreenIndex | 0 | header |
| 20 | 9 | MoveFrontendSpriteRect @0x4259d0 | bx+anim*-0x20+10 [+10] | cH2-0x2f [-47] | – | – | slot 0 | – | |
| 21 | 9 | MoveFrontendSpriteRect @0x4259d0 | anim*0x20+10+bx [+10] | cH2+0x89 [+137] | – | – | slot 1 | – | |
| 22 | 9 | MoveFrontendSpriteRect @0x4259d0 | anim*0x30+0x7a+bx [+122] | cH2+0x89 [+137] | – | – | slot 2 | – | |
| 23 | 10 (0xa) | QueueFrontendOverlayRect @0x425660 | cW2-200 [-200] | (by-g_menuHeaderLabelYOffset)+-0x40 [-64] | 0,0 | hdrW, hdrH | g_currentScreenIndex | 0 | header |

Delegated sub-draw fns followed: none. List rows drawn via DrawFrontendClippedStringToSurface(str, x=0x20[32], y stepping 0x10 from 0x1c/0x2c, surface, 0x1b0[432]) into the panel surface (surface-local). BltColorFillToSurface panel fills use (x=0x20, y=0x1c/0x9c, w=0x1b0, h=0x5e) into the panel — surface-local.
Confidence: [CONFIRMED @ 0x00418d50; callees by address]. [UNCERTAIN: scroll/selection sprite absolute x/y depend on `g_connBrowserListOriginX_PROVISIONAL[0].origin_x/origin_y` — runtime panel origin, not an immediate; only the +offsets are literal.]

---

### Screen 9 @ 0x00419cf0 — RunFrontendSessionPicker
Mechanism: canvas refs (cW2, cH2, bx=(canvasW>>1)-0xd2 [-210], by=(canvasH>>1)-0x9f [-159]). DirectPlay [ARCH-DIVERGENCE: DXPTYPE].
States/structure: switch cases 0,1,2,3,4,6,7. ChooseSession panel button (slot0, w=0x1f0 h=0x80) + OK + Back. Scroll/selection sprites use runtime `g_connBrowserListOriginX_PROVISIONAL[0]` origin (flagged).

| # | State/case | Helper @addr | x (verbatim) [dec] | y (verbatim) [dec] | w | h | tex/label/surface | blit-flag | notes |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 0 | QueueFrontendOverlayRect @0x425660 | cW2-200 [-200] | (by-g_menuHeaderLabelYOffset)+-0x40 [-64] | 0,0 | hdrW, hdrH | g_currentScreenIndex | 0 | header |
| 2 | 1 (init buttons) | CreateFrontendDisplayModeButton @0x425de0 | -0x1f0 [-496] (auto) | 0 | 0x1f0 [496] | 0x80 [128] | SNK_ChooseSessionButTxt | n/a | slot0 panel; ptr→g_postRaceNameButtonSurfacePtr |
| 3 | 1 | CreateFrontendDisplayModeButton @0x425de0 | -0x60 [-96] (auto) | 0 | 0x60 [96] | 0x20 [32] | SNK_OkButTxt | n/a | slot1 OK |
| 4 | 1 | CreateFrontendDisplayModeButton @0x425de0 | -0x70 [-112] (auto) | 0 | 0x70 [112] | 0x20 [32] | SNK_BackButTxt | n/a | slot2 Back; EscKeyButtonIndex=2 |
| 5 | 1 | QueueFrontendOverlayRect @0x425660 | cW2-200 [-200] | (by-g_menuHeaderLabelYOffset)+-0x40 [-64] | 0,0 | hdrW, hdrH | g_currentScreenIndex | 0 | header (2nd queue this state) |
| 6 | 2 (intro) | QueueFrontendOverlayRect @0x425660 | cW2-200 [-200] | (by-g_menuHeaderLabelYOffset)+-0x40 [-64] | 0,0 | hdrW, hdrH | g_currentScreenIndex | 0 | header |
| 7 | 2 | MoveFrontendSpriteRect @0x4259d0 | cW2-200 [-200] | (cH2-0x1af [-431])+anim*0xc | – | – | slot 0 | – | |
| 8 | 2 | MoveFrontendSpriteRect @0x4259d0 | (cW2-0x1c8 [-456])+anim*8 | cH2+0x89 [+137] | – | – | slot 1 | – | |
| 9 | 2 | MoveFrontendSpriteRect @0x4259d0 | bx+anim*-0xc+0x1fa [+506] | cH2+0x89 [+137] | – | – | slot 2 | – | |
| 10 | 3 | QueueFrontendOverlayRect @0x425660 | cW2-200 [-200] | (by-g_menuHeaderLabelYOffset)+-0x40 [-64] | 0,0 | hdrW, hdrH | g_currentScreenIndex | 0 | header |
| 11 | 3 | QueueFrontendOverlayRect @0x425660 | listOrigin.origin_x+0xf2 [+242] | listOrigin.origin_y+0x1e [+30] | 0,0 | 0xc [12] | 0xc [12] | g_browserScrollIndicatorSprite | 0xff0000 | up arrow (if scrollOff!=0) |
| 12 | 3 | QueueFrontendOverlayRect @0x425660 | listOrigin.origin_x+0xf2 [+242] | listOrigin.origin_y+0x6c [+108] | 0,0xc | 0xc [12] | 0xc [12] | g_browserScrollIndicatorSprite | 0xff0000 | down arrow; src_y=0xc |
| 13 | 3 | QueueFrontendOverlayRect @0x425660 | listOrigin.origin_x+0x10 [+16] | (cursorIdx-scrollOff)*0x10+0x2f+origin_y [+47] | 0,0x1b | 0xc [12] | 9 | g_browserSelectionBarSprite | 0xff0000 | selection bar; src_y=0x1b |
| 14 | 4 | QueueFrontendOverlayRect @0x425660 | cW2-200 [-200] | (by-g_menuHeaderLabelYOffset)+-0x40 [-64] | 0,0 | hdrW, hdrH | g_currentScreenIndex | 0 | header |
| 15 | 4 | QueueFrontendOverlayRect @0x425660 | listOrigin.origin_x+0xf2 [+242] | listOrigin.origin_y+0x1e [+30] | 0,0 | 0xc [12] | 0xc [12] | g_browserScrollIndicatorSprite | 0xff0000 | up arrow |
| 16 | 4 | QueueFrontendOverlayRect @0x425660 | listOrigin.origin_x+0xf2 [+242] | listOrigin.origin_y+0x6c [+108] | 0,0xc | 0xc [12] | 0xc [12] | g_browserScrollIndicatorSprite | 0xff0000 | down arrow; src_y=0xc |
| 17 | 4 | QueueFrontendOverlayRect @0x425660 | listOrigin.origin_x+0x10 [+16] | (cursorIdx-scrollOff)*0x10+0x2f+origin_y [+47] | 0,0x1b | 0xc [12] | 9 | g_browserSelectionBarSprite | 0xff0000 | selection bar (blink anim&0x10); src_y=0x1b |
| 18 | 6 | QueueFrontendOverlayRect @0x425660 | cW2-200 [-200] | (by-g_menuHeaderLabelYOffset)+-0x40 [-64] | 0,0 | hdrW, hdrH | g_currentScreenIndex | 0 | header |
| 19 | 7 (outro) | QueueFrontendOverlayRect @0x425660 | cW2-200 [-200] | (by-g_menuHeaderLabelYOffset)+-0x40 [-64] | 0,0 | hdrW, hdrH | g_currentScreenIndex | 0 | header |
| 20 | 7 | MoveFrontendSpriteRect @0x4259d0 | bx+anim*-0x20+10 [+10] | cH2-0x2f [-47] | – | – | slot 0 | – | |
| 21 | 7 | MoveFrontendSpriteRect @0x4259d0 | anim*0x20+10+bx [+10] | cH2+0x89 [+137] | – | – | slot 1 | – | |
| 22 | 7 | MoveFrontendSpriteRect @0x4259d0 | anim*0x30+0x7a+bx [+122] | cH2+0x89 [+137] | – | – | slot 2 | – | |

Delegated sub-draw fns followed: RenderFrontendSessionBrowser() called (case 1/2/3/4) to populate the panel surface; not entered (separate fn, no inline placement args in this caller). List drawn there.
Confidence: [CONFIRMED @ 0x00419cf0; callees by address]. [UNCERTAIN: scroll/selection sprite absolute origin from runtime `g_connBrowserListOriginX_PROVISIONAL[0]`, not immediate; +offsets literal.]
# Frontend Layout Harvest — Part 10 (screen-table indices 10–14)

Source: `TD5_d3d.exe` via Ghidra (read-only). Screen-fn table `g_frontendScreenFnTable` @ `0x004655C4`
(30 LE uint32). Decoded slice:

| idx | fn ptr | name |
|-----|--------|------|
| 10 | 0x0041a7b0 | RunFrontendCreateSessionFlow |
| 11 | 0x0041c330 | RunFrontendNetworkLobby |
| 12 | 0x0041d890 | ScreenOptionsHub |
| 13 | 0x0041f990 | ScreenGameOptions |
| 14 | 0x0041df20 | ScreenControlOptions |

Canvas refs throughout: `cx = g_frontendCanvasW>>1`, `cy = g_frontendCanvasH>>1`.
Common per-state derived bases: `iVar=cx-0xd2` (=cx-210), `iVarY=cy-0x9f` (=cy-159).
`g_frontendAnimFrameCounter` is the per-state slide-in/out animation frame var (referred to as `F` below).

Helper address legend:
- CFDMB = CreateFrontendDisplayModeButton @ 0x00425de0 (label,x,y,w,h,ud); neg x = auto-layout flag.
- QFOR = QueueFrontendOverlayRect @ 0x00425660 (dst_x,dst_y,src_x,src_y,w,h,color,surface).
- MFSR = MoveFrontendSpriteRect @ 0x004259d0 (slot,x,y).
- DFLS = DrawFrontendLocalizedStringToSurface @ 0x00424560 (no x/y operands — position set by preceding MeasureOrCenterFrontendLocalizedString).
- Copy16BitSurfaceRect @ 0x004251a0 and DrawFrontendButtonBackground @ 0x00425b60: **NOT CALLED** by any of these 5 screens.

NOTE on DFLS: in screens 11/13/14 every DFLS is preceded by MeasureOrCenterFrontendLocalizedString
(@ measure/center, args = (str, x, width)); DFLS itself carries no coords, so rows below list the
MeasureOrCenter (x,width) where it governs placement.

---

### Screen 10 @ 0x0041a7b0 — RunFrontendCreateSessionFlow
Mechanism: `cx=W>>1; iVar8=cx-0xd2 (cx-210); iVar6=cy-0x9f (cy-159)`. Header QFOR uses `cx-200` (=cx-0xc8).
States/structure: switch(g_frontendInnerState) cases 0,1,2,3,4,5,6,7,8,0x10,0x11,0x12,0x13,0x14,0x15. Animated name-entry / DXPlay session-join flow.

| # | State/case | Helper @addr | x (verbatim) [dec] | y (verbatim) [dec] | w | h | tex/label/surface | blit-flag | notes |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 0 | CFDMB 0x425de0 | -0x1c0 [-448] | 0 | 0x1c0 [448] | 0x40 [64] | SNK_EnterNewSessionNameButTxt | — | neg x=auto-layout; stored to name button surface |
| 2 | 0 | CFDMB 0x425de0 | -0x70 [-112] | 0 | 0x70 [112] | 0x20 [32] | SNK_BackButTxt | — | auto-layout |
| 3 | 0,2,3,4,5,6,8(none),0x10,0x11,0x12 (header, std form) | QFOR 0x425660 | cx-200 [cx-200] | (iVar6 - g_menuHeaderLabelYOffset) - 0x40 [-64 + base] | g_menuHeaderLabelSurfaceWidth | g_menuHeaderLabelSurfaceHeight | g_currentScreenIndex (header surf) | 0 | src 0,0; recurring menu-header blit |
| 4 | 1 | MFSR 0x4259d0 | iVar8 + F*-0x18 + 0x30a [iVar8 -24F +778] | iVar6 + F*-4 + 0xf0 [iVar6 -4F +240] | — | — | sprite slot 0 | — | slide anim |
| 5 | 1 | MFSR 0x4259d0 | (cx-0x32a) + F*0x18 [cx-810 +24F] | iVar6 + F*-4 + 0x150 [iVar6 -4F +336] | — | — | sprite slot 1 | — | slide anim |
| 6 | 3 | MFSR 0x4259d0 | iVar8 + F*-0x20 + 10 [iVar8 -32F +10] | iVar6 + F*-6 + 0x70 [iVar6 -6F +112] | — | — | sprite slot 0 | — | slide-out anim |
| 7 | 3 | MFSR 0x4259d0 | F*0x20 + 0xa8 + iVar8 [iVar8 +32F +168] | iVar6 + F*-2 + 0xd0 [iVar6 -2F +208] | — | — | sprite slot 1 | — | slide-out anim |
| 8 | 4 | CFDMB 0x425de0 | -0x1c0 [-448] | 0 | 0x1c0 [448] | 0x40 [64] | SNK_EnterPlayerNameButTxt | — | auto-layout |
| 9 | 4 | CFDMB 0x425de0 | -0x70 [-112] | 0 | 0x70 [112] | 0x20 [32] | SNK_BackButTxt | — | auto-layout |
| 10 | 5 | MFSR 0x4259d0 | iVar8 + F*-0x18 + 0x30a [iVar8 -24F +778] | iVar6 + F*-4 + 0xf0 [iVar6 -4F +240] | — | — | sprite slot 0 | — | same as state1 |
| 11 | 5 | MFSR 0x4259d0 | (cx-0x32a) + F*0x18 [cx-810 +24F] | iVar6 + F*-4 + 0x150 [iVar6 -4F +336] | — | — | sprite slot 1 | — | same as state1 |
| 12 | 7 (header, slide variant) | QFOR 0x425660 | iVar8 + F*-0x10 + 10 [iVar8 -16F +10] | (iVar6 - g_menuHeaderLabelYOffset) - 0x40 [-64+base] | g_menuHeaderLabelSurfaceWidth | g_menuHeaderLabelSurfaceHeight | g_currentScreenIndex | 0 | header slides out |
| 13 | 7 | MFSR 0x4259d0 | iVar8 + F*-0x20 + 10 [iVar8 -32F +10] | iVar6 + F*-6 + 0x70 [iVar6 -6F +112] | — | — | sprite slot 0 | — | slide-out |
| 14 | 7 | MFSR 0x4259d0 | F*0x20 + 0xa8 + iVar8 [iVar8 +32F +168] | iVar6 + F*-2 + 0xd0 [iVar6 -2F +208] | — | — | sprite slot 1 | — | slide-out |
| 15 | 0x10 | CFDMB 0x425de0 | -0x1c0 [-448] | 0 | 0x1c0 [448] | 0x40 [64] | SNK_EnterPlayerNameButTxt | — | auto-layout (same as state 4) |
| 16 | 0x10 | CFDMB 0x425de0 | -0x70 [-112] | 0 | 0x70 [112] | 0x20 [32] | SNK_BackButTxt | — | auto-layout |
| 17 | 0x11 | MFSR 0x4259d0 | iVar8 + F*-0x18 + 0x30a [iVar8 -24F +778] | iVar6 + F*-4 + 0xf0 [iVar6 -4F +240] | — | — | sprite slot 0 | — | slide anim (same as state1) |
| 18 | 0x11 | MFSR 0x4259d0 | (cx-0x32a) + F*0x18 [cx-810 +24F] | iVar6 + F*-4 + 0x150 [iVar6 -4F +336] | — | — | sprite slot 1 | — | slide anim |
| 19 | 0x13 (header slide) | QFOR 0x425660 | iVar8 + F*-0x10 + 10 [iVar8 -16F +10] | (iVar6 - g_menuHeaderLabelYOffset) - 0x40 [-64+base] | g_menuHeaderLabelSurfaceWidth | g_menuHeaderLabelSurfaceHeight | g_currentScreenIndex | 0 | header slides out |
| 20 | 0x13 | MFSR 0x4259d0 | iVar8 + F*-0x20 + 10 [iVar8 -32F +10] | iVar6 + F*-6 + 0x70 [iVar6 -6F +112] | — | — | sprite slot 0 | — | slide-out (same as state7) |
| 21 | 0x13 | MFSR 0x4259d0 | F*0x20 + 0xa8 + iVar8 [iVar8 +32F +168] | iVar6 + F*-2 + 0xd0 [iVar6 -2F +208] | — | — | sprite slot 1 | — | slide-out |

Sub-helpers called for text input: RenderFrontendCreateSessionNameInput (states 2,6,0x12) — not followed (separate name-input renderer, no inline coords in this fn). RenderTgaToFrontendSurface/SetSurfaceColorKeyFromRGB carry no x/y.
Delegated sub-draw fns followed: none (RenderFrontendCreateSessionNameInput noted, not entered — out of scope, no operands here).
Confidence: [CONFIRMED @ 0x0041a7b0 decomp; header form recurs verbatim across cases 0/1/2/3/4/5/6/0x10/0x11/0x12; slide-header form across 7/0x13]

---

### Screen 11 @ 0x0041c330 — RunFrontendNetworkLobby
Mechanism: `cx=W>>1 (uVar12); cy=H>>1 (uVar8); iVar9=cy-0x9f (cy-159)`. Header QFOR uses `cx-200`.
States/structure: switch cases 0,1,2,3,4,5,6,7,8,9,10(0xa),0xc,0xd,0xe,0xf,0x10,0x11. Animated multiplayer lobby (roster, chat, error dialog, host/seal flow).

| # | State/case | Helper @addr | x (verbatim) [dec] | y (verbatim) [dec] | w | h | tex/label/surface | blit-flag | notes |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 0 | CFDMB 0x425de0 | -0x1d0 [-464] | 0 | 0x1d0 [464] | 0x18 [24] | label=NULL (decoration strip) | — | -> g_lobbyDecorationStripSurface |
| 2 | 0 | CFDMB 0x425de0 | -0x200 [-512] | 0 | 0x200 [512] | 0x80 [128] | SNK_MessageWindowButTxt | — | -> name button surface (chat/msg window) |
| 3 | 0 | CFDMB 0x425de0 | -0xe0 [-224] | 0 | 0xe0 [224] | 0x86 [134] | SNK_StatusButTxt | — | -> g_lobbyPlayerRosterSurface |
| 4 | 0 | CFDMB 0x425de0 | -200 [-200] (=-0xc8) | 0 | 200 [200] (=0xc8) | 0x20 [32] | SNK_ChangeCarButTxt | — | auto-layout |
| 5 | 0 | CFDMB 0x425de0 | -200 [-200] | 0 | 0x78 [120] | 0x20 [32] | SNK_StartButTxt | — | auto-layout |
| 6 | 0 | CFDMB 0x425de0 | -200 [-200] | 0 | 0x78 [120] | 0x20 [32] | SNK_ExitButTxt | — | auto-layout |
| 7 | 1 | MFSR 0x4259d0 | (cx-0x1a0) + F*0xc [cx-416 +12F] | cy+0x15 [cy+21] | — | — | sprite slot 0 | — | slide-in |
| 8 | 1 | MFSR 0x4259d0 | cx-200 [cx-200] | (F+0xfffffef)*0x10 + iVar9 [(F-17)*16 + iVar9] | — | — | sprite slot 1 | — | F-17 offset |
| 9 | 1 | MFSR 0x4259d0 | cx-200 [cx-200] | iVar9 + F*-8 + 0x172 [iVar9 -8F +370] | — | — | sprite slot 2 | — | |
| 10 | 1 | MFSR 0x4259d0 | cx+0x28 [cx+40] | iVar9 + F*-8 + 0x172 [iVar9 -8F +370] | — | — | sprite slot 3 | — | |
| 11 | 1 | MFSR 0x4259d0 | cx+0x28 [cx+40] | iVar9 + F*-8 + 0x1a2 [iVar9 -8F +418] | — | — | sprite slot 4 | — | |
| 12 | 1 | MFSR 0x4259d0 | cx+0x28 [cx+40] | iVar9 + F*-8 + 0x1d2 [iVar9 -8F +466] | — | — | sprite slot 5 | — | |
| 13 | 1 (header slide-in) | QFOR 0x425660 | cx-200 [cx-200] | ((F*4 -0x90) - hdrYOff) + iVar9 [(4F-144 -hdrYOff)+iVar9] | hdrW | hdrH | g_currentScreenIndex | 0 | animated header |
| 14 | 1 | QFOR 0x425660 | cx-0xb0 [cx-176] | cy-0x97 [cy-151] | 0x1e0 [480] | 0x20 [32] | g_lobbySessionHeaderSurface | 0 | src 0,0; session header strip |
| 15 | 2,3 (header std) | QFOR 0x425660 | cx-200 [cx-200] | (iVar9 - hdrYOff) - 0x40 [-64+base] | hdrW | hdrH | g_currentScreenIndex | 0 | |
| 16 | 2,3 | QFOR 0x425660 | cx-0xb0 [cx-176] | cy-0x97 [cy-151] | 0x1e0 [480] | 0x20 [32] | g_lobbySessionHeaderSurface | 0 | session header strip |
| 17 | 4,5 (header std) | QFOR 0x425660 | cx-200 [cx-200] | (iVar9 - hdrYOff) - 0x40 [-64+base] | hdrW | hdrH | g_currentScreenIndex | 0 | states 4 & 5 |
| 18 | 6 | MFSR 0x4259d0 | cx-0xb0 [cx-176] | iVar9 + F*-0x10 + 0xb4 [iVar9 -16F +180] | — | — | sprite slot 0 | — | error-dialog slide |
| 19 | 6 | MFSR 0x4259d0 | cx-200 [cx-200] | iVar9 + F*-0x10 + 0x30 [iVar9 -16F +48] | — | — | sprite slot 1 | — | |
| 20 | 6 (header std) | QFOR 0x425660 | cx-200 [cx-200] | (iVar9 - hdrYOff) - 0x40 [-64+base] | hdrW | hdrH | g_currentScreenIndex | 0 | |
| 21 | 6 (when F>1) | QFOR 0x425660 | (cx-0xd2) + F*-0x20 + 0x318 [(cx-210) -32F +792] | cy-0x5f [cy-95] | 0x1e2 [482] | 0x40 [64] | g_lobbyErrorDialogSurface | 0 | error dialog slide-in |
| 22 | 6 (state-4/6 error text) MeasureOrCenter→DFLS | 0x424560 | x=0 | (n/a; centered) | width=0x1e2 [482] | — | SNK_NetErrString1/2/3/4 (+idx*0x28 / *0x20) | — | err lines, 4 DFLS calls; x=0 width=482 |
| 23 | 7 (header std) | QFOR 0x425660 | cx-200 [cx-200] | (iVar9 - hdrYOff) - 0x40 [-64] | hdrW | hdrH | g_currentScreenIndex | 0 | |
| 24 | 7 | QFOR 0x425660 | cx-0xba [cx-186] | cy-0x5f [cy-95] | 0x1e2 [482] | 0x40 [64] | g_lobbyErrorDialogSurface | 0 | error dialog static |
| 25 | 7 | CFDMB 0x425de0 | cx-2 [cx-2] | cy-0xf [cy-15] | 0x60 [96] | 0x20 [32] | SNK_OkButTxt | — | EXPLICIT x/y (not auto-layout); OK-only branch |
| 26 | 7 | CFDMB 0x425de0 | cx-0x42 [cx-66] | cy-0xf [cy-15] | 0x60 [96] | 0x20 [32] | SNK_YesButTxt | — | EXPLICIT; yes/no branch |
| 27 | 7 | CFDMB 0x425de0 | g_frontendCanvasW-0xf0 [W-240] | cy-0xf [cy-15] | 0x60 [96] | 0x20 [32] | SNK_NoxButTxt | — | EXPLICIT; x uses full canvasW |
| 28 | 8 (header std) | QFOR 0x425660 | cx-200 [cx-200] | (iVar9 - hdrYOff) - 0x40 [-64] | hdrW | hdrH | g_currentScreenIndex | 0 | |
| 29 | 8 | QFOR 0x425660 | cx-0xba [cx-186] | cy-0x5f [cy-95] | 0x1e2 [482] | 0x40 [64] | g_lobbyErrorDialogSurface | 0 | error dialog static |
| 30 | 9 (header std) | QFOR 0x425660 | cx-200 [cx-200] | (iVar9 - hdrYOff) - 0x40 [-64] | hdrW | hdrH | g_currentScreenIndex | 0 | |
| 31 | 9 | QFOR 0x425660 | cx-0xba [cx-186] | cy-0x5f [cy-95] | 0x1e2 [482] | 0x40 [64] | g_lobbyErrorDialogSurface | 0 | |
| 32 | 0xa (10) | MFSR 0x4259d0 | cx-0xb0 [cx-176] | F*0x10 + -0xcc + iVar9 [16F -204 +iVar9] | — | — | sprite slot 0 | — | error-dialog slide-out |
| 33 | 0xa (10) | MFSR 0x4259d0 | cx-200 [cx-200] | (F+0xfffffeb)*0x10 + iVar9 [(F-21)*16 +iVar9] | — | — | sprite slot 1 | — | F-21 offset |
| 34 | 0xa (10) (header std) | QFOR 0x425660 | cx-200 [cx-200] | (iVar9 - hdrYOff) - 0x40 [-64] | hdrW | hdrH | g_currentScreenIndex | 0 | |
| 35 | 0xa (10) | QFOR 0x425660 | F*0x20 + 0x18 + (cx-0xd2) [(cx-210) +32F +24] | cy-0x5f [cy-95] | 0x1e2 [482] | 0x40 [64] | g_lobbyErrorDialogSurface | 0 | slide-out |
| 36 | 0xc,0xd,0xe,0xf,0x10 (header std) | QFOR 0x425660 | cx-200 [cx-200] | (iVar9 - hdrYOff) - 0x40 [-64] | hdrW | hdrH | g_currentScreenIndex | 0 | each of these states emits one header QFOR only (no positioned sprites) |

Sub-helpers (no inline coords here, not followed): RenderFrontendLobbyChatPanel, RenderFrontendLobbyStatusPanel, RenderFrontendLobbyChatInput (state 3/8), CopyPrimaryFrontendBufferToSecondary/BlitFrontendCachedRect/BlitSecondaryFrontendRectToPrimary (full-canvas 0,0,W,H copies).
Delegated sub-draw fns followed: none (lobby panel renderers are separate fns; out of scope).
Confidence: [CONFIRMED @ 0x0041c330 decomp]. [UNCERTAIN: state 6 err-text DFLS x exactly 0 — MeasureOrCenter(str,0,0x1e2) so x=0 width=482; centering done inside MeasureOrCenter, not a literal placement operand.]

---

### Screen 12 @ 0x0041d890 — ScreenOptionsHub
Mechanism: `cx=W>>1 (uVar4); cy=H>>1 (uVar2); iVar5=cx-0xd2 (cx-210); iVar3=cy-0x9f (cy-159)`. Header QFOR uses `cx-200`.
States/structure: switch cases 0, (1,2), 3, (4,5), 6, 7, 8. 6 menu buttons (auto-layout) + OK; slide anim in state 3 (in) and 8 (out).

| # | State/case | Helper @addr | x (verbatim) [dec] | y (verbatim) [dec] | w | h | tex/label/surface | blit-flag | notes |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 0 | CFDMB 0x425de0 | -0x130 [-304] | 0 | 0x130 [304] | 0x20 [32] | SNK_GameOptionsButTxt | — | auto-layout |
| 2 | 0 | CFDMB 0x425de0 | -0x130 [-304] | 0 | 0x130 [304] | 0x20 [32] | SNK_ControlOptionsButTxt | — | auto-layout |
| 3 | 0 | CFDMB 0x425de0 | -0x130 [-304] | 0 | 0x130 [304] | 0x20 [32] | SNK_SoundOptionsButTxt | — | auto-layout |
| 4 | 0 | CFDMB 0x425de0 | -0x130 [-304] | 0 | 0x130 [304] | 0x20 [32] | SNK_GraphicsOptionsButTxt | — | auto-layout |
| 5 | 0 | CFDMB 0x425de0 | -0x130 [-304] | 0 | 0x130 [304] | 0x20 [32] | SNK_TwoPlayerOptionsButTxt | — | auto-layout |
| 6 | 0 | CFDMB 0x425de0 | -0x130 [-304] | 0 | 0x60 [96] | 0x20 [32] | SNK_OkButTxt | — | auto-layout |
| 7 | 3 | MFSR 0x4259d0 | F*0x10 + -0x266 + iVar5 [16F -614 +iVar5] | cy-0x8f [cy-143] | — | — | sprite slot 0 | — | slide-in |
| 8 | 3 | MFSR 0x4259d0 | iVar5 + F*-0x10 + 0x27a [iVar5 -16F +634] | cy-0x67 [cy-103] | — | — | sprite slot 1 | — | |
| 9 | 3 | MFSR 0x4259d0 | F*0x10 + -0x266 + iVar5 [16F -614 +iVar5] | cy-0x3f [cy-63] | — | — | sprite slot 2 | — | |
| 10 | 3 | MFSR 0x4259d0 | iVar5 + F*-0x10 + 0x27a [iVar5 -16F +634] | cy-0x17 [cy-23] | — | — | sprite slot 3 | — | |
| 11 | 3 | MFSR 0x4259d0 | F*0x10 + -0x266 + iVar5 [16F -614 +iVar5] | cy+0x11 [cy+17] | — | — | sprite slot 4 | — | |
| 12 | 3 | MFSR 0x4259d0 | cx-0x68 [cx-104] | iVar3 + F*-0x10 + 0x398 [iVar3 -16F +920] | — | — | sprite slot 5 | — | OK button slide |
| 13 | 3 (header slide-in) | QFOR 0x425660 | cx-200 [cx-200] | ((F*4 -0xdc) - hdrYOff) + iVar3 [(4F-220 -hdrYOff)+iVar3] | hdrW | hdrH | g_currentScreenIndex | 0 | |
| 14 | 4,5 (header std) | QFOR 0x425660 | cx-200 [cx-200] | (iVar3 - hdrYOff) - 0x40 [-64] | hdrW | hdrH | g_currentScreenIndex | 0 | |
| 15 | 6 (header std) | QFOR 0x425660 | cx-200 [cx-200] | (iVar3 - hdrYOff) - 0x40 [-64] | hdrW | hdrH | g_currentScreenIndex | 0 | |
| 16 | 7 (header std) | QFOR 0x425660 | cx-200 [cx-200] | (iVar3 - hdrYOff) - 0x40 [-64] | hdrW | hdrH | g_currentScreenIndex | 0 | |
| 17 | 8 (header slide-out) | QFOR 0x425660 | iVar5 + F*-0x18 + 10 [iVar5 -24F +10] | (iVar3 - hdrYOff) - 0x40 [-64] | hdrW | hdrH | g_currentScreenIndex | 0 | |
| 18 | 8 | MFSR 0x4259d0 | iVar5 + F*-8 + 10 [iVar5 -8F +10] | iVar3 + F*-0x30 + 0x10 [iVar3 -48F +16] | — | — | sprite slot 0 | — | slide-out |
| 19 | 8 | MFSR 0x4259d0 | iVar5 + F*-8 + 10 [iVar5 -8F +10] | iVar3 + F*-0x20 + 0x38 [iVar3 -32F +56] | — | — | sprite slot 1 | — | |
| 20 | 8 | MFSR 0x4259d0 | iVar5 + F*-6 + 10 [iVar5 -6F +10] | iVar3 + F*-0x18 + 0x60 [iVar3 -24F +96] | — | — | sprite slot 2 | — | |
| 21 | 8 | MFSR 0x4259d0 | (cx-200) + F*6 [cx-200 +6F] | iVar3 + F*-0x18 + 0x88 [iVar3 -24F +136] | — | — | sprite slot 3 | — | |
| 22 | 8 | MFSR 0x4259d0 | iVar5 + F*-6 + 10 [iVar5 -6F +10] | iVar3 + F*-0x18 + 0xb0 [iVar3 -24F +176] | — | — | sprite slot 4 | — | |
| 23 | 8 | MFSR 0x4259d0 | (cx-0x68) + F*6 [cx-104 +6F] | F*0x30 + 0x128 + iVar3 [48F +296 +iVar3] | — | — | sprite slot 5 | — | OK slide-out |

Delegated sub-draw fns followed: none.
Confidence: [CONFIRMED @ 0x0041d890 decomp]

---

### Screen 13 @ 0x0041f990 — ScreenGameOptions
Mechanism: `cx=W>>1 (uVar2); cy=H>>1 (uVar4); iVar3=cx-0xd2 (cx-210); iVar5=cy-0x9f (cy-159)`. Header QFOR uses `cx-200`. Value-panel surface g_lobbyErrorDialogSurface sized 0xe0 x 0x118 (224x280).
States/structure: switch cases 0, (1,2), 3, (4,5), 6, 7, 8. 7 toggle rows + OK (auto-layout); value text drawn into a side panel.

| # | State/case | Helper @addr | x (verbatim) [dec] | y (verbatim) [dec] | w | h | tex/label/surface | blit-flag | notes |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 0 | CFDMB 0x425de0 | -0x128 [-296] | 0 | 0x128 [296] | 0x20 [32] | SNK_CircuitLapsTxt | — | auto-layout |
| 2 | 0 | CFDMB 0x425de0 | -0x128 [-296] | 0 | 0x128 [296] | 0x20 [32] | SNK_CheckpointTimersButTxt | — | auto-layout |
| 3 | 0 | CFDMB 0x425de0 | -0x128 [-296] | 0 | 0x128 [296] | 0x20 [32] | SNK_TrafficButTxt | — | auto-layout |
| 4 | 0 | CFDMB 0x425de0 | -0x128 [-296] | 0 | 0x128 [296] | 0x20 [32] | SNK_CopsButTxt | — | auto-layout |
| 5 | 0 | CFDMB 0x425de0 | -0x128 [-296] | 0 | 0x128 [296] | 0x20 [32] | SNK_DifficultyButTxt | — | auto-layout |
| 6 | 0 | CFDMB 0x425de0 | -0x128 [-296] | 0 | 0x128 [296] | 0x20 [32] | SNK_DynamicsButTxt | — | auto-layout |
| 7 | 0 | CFDMB 0x425de0 | -0x128 [-296] | 0 | 0x128 [296] | 0x20 [32] | SNK_3dCollisionsButTxt | — | auto-layout |
| 8 | 0 | CFDMB 0x425de0 | -0x128 [-296] | 0 | 0x60 [96] | 0x20 [32] | SNK_OkButTxt | — | auto-layout |
| 9 | 3 | MFSR 0x4259d0 | F*0x10 + -0x266 + iVar3 [16F -614 +iVar3] | cy-0x8f [cy-143] | — | — | sprite slot 0 | — | slide-in |
| 10 | 3 | MFSR 0x4259d0 | iVar3 + F*-0x10 + 0x27a [iVar3 -16F +634] | cy-0x67 [cy-103] | — | — | sprite slot 1 | — | |
| 11 | 3 | MFSR 0x4259d0 | F*0x10 + -0x266 + iVar3 [16F -614 +iVar3] | cy-0x3f [cy-63] | — | — | sprite slot 2 | — | |
| 12 | 3 | MFSR 0x4259d0 | iVar3 + F*-0x10 + 0x27a [iVar3 -16F +634] | cy-0x17 [cy-23] | — | — | sprite slot 3 | — | |
| 13 | 3 | MFSR 0x4259d0 | F*0x10 + -0x266 + iVar3 [16F -614 +iVar3] | cy+0x11 [cy+17] | — | — | sprite slot 4 | — | |
| 14 | 3 | MFSR 0x4259d0 | iVar3 + F*-0x10 + 0x27a [iVar3 -16F +634] | cy+0x39 [cy+57] | — | — | sprite slot 5 | — | |
| 15 | 3 | MFSR 0x4259d0 | F*0x10 + -0x266 + iVar3 [16F -614 +iVar3] | cy+0x61 [cy+97] | — | — | sprite slot 6 | — | |
| 16 | 3 | MFSR 0x4259d0 | cx-0x68 [cx-104] | iVar5 + F*-0x10 + 0x398 [iVar5 -16F +920] | — | — | sprite slot 7 (OK) | — | |
| 17 | 3 (header slide-in) | QFOR 0x425660 | cx-200 [cx-200] | ((F*4 -0xdc) - hdrYOff) + iVar5 [(4F-220 -hdrYOff)+iVar5] | hdrW | hdrH | g_currentScreenIndex | 0 | |
| 18 | 4,5 (header std) | QFOR 0x425660 | cx-200 [cx-200] | (iVar5 - hdrYOff) - 0x40 [-64] | hdrW | hdrH | g_currentScreenIndex | 0 | |
| 19 | 4 (value text into panel) MeasureOrCenter→DFLS | 0x424560 | x=10 [10] (=0xa) | (centered in width) | width=0xe0 [224] | — | sprintf row0 + SNK_OnOff/Difficulty/Dynamics tables (7 DFLS calls) | — | each row: MeasureOrCenter(str,10,0xe0) then DFLS; drawn into g_lobbyErrorDialogSurface |
| 20 | 4,5 | QFOR 0x425660 | cx+0x4a [cx+74] | cy-0x8f [cy-143] | 0xe0 [224] | 0x118 [280] | g_lobbyErrorDialogSurface (value panel) | 0 | panel blit |
| 21 | 6 (header std) | QFOR 0x425660 | cx-200 [cx-200] | (iVar5 - hdrYOff) - 0x40 [-64] | hdrW | hdrH | g_currentScreenIndex | 0 | |
| 22 | 6 | QFOR 0x425660 | cx+0x4a [cx+74] | cy-0x8f [cy-143] | 0xe0 [224] | 0x118 [280] | g_lobbyErrorDialogSurface | 0 | value panel |
| 23 | 7 (header std) | QFOR 0x425660 | cx-200 [cx-200] | (iVar5 - hdrYOff) - 0x40 [-64] | hdrW | hdrH | g_currentScreenIndex | 0 | |
| 24 | 7 | QFOR 0x425660 | cx+0x4a [cx+74] | cy-0x8f [cy-143] | 0xe0 [224] | 0x118 [280] | g_lobbyErrorDialogSurface | 0 | value panel |
| 25 | 8 (header slide-out) | QFOR 0x425660 | iVar3 + F*-0x18 + 10 [iVar3 -24F +10] | (iVar5 - hdrYOff) - 0x40 [-64] | hdrW | hdrH | g_currentScreenIndex | 0 | |
| 26 | 8 (panel slide-out) | QFOR 0x425660 | iVar3 + F*-8 + 0x11c [iVar3 -8F +284] | F*0x20 + 0x10 + iVar5 [32F +16 +iVar5] | 0xe0 [224] | 0x118 [280] | g_lobbyErrorDialogSurface | 0 | |
| 27 | 8 | MFSR 0x4259d0 | (cx-200) + F*8 [cx-200 +8F] | iVar5 + F*-0x20 + 0x10 [iVar5 -32F +16] | — | — | sprite slot 0 | — | slide-out |
| 28 | 8 | MFSR 0x4259d0 | (cx-200) + F*8 [cx-200 +8F] | iVar5 + F*-0x20 + 0x38 [iVar5 -32F +56] | — | — | sprite slot 1 | — | |
| 29 | 8 | MFSR 0x4259d0 | (cx-200) + F*6 [cx-200 +6F] | iVar5 + F*-0x20 + 0x60 [iVar5 -32F +96] | — | — | sprite slot 2 | — | |
| 30 | 8 | MFSR 0x4259d0 | (cx-200) + F*6 [cx-200 +6F] | iVar5 + F*-0x20 + 0x88 [iVar5 -32F +136] | — | — | sprite slot 3 | — | |
| 31 | 8 | MFSR 0x4259d0 | (cx-200) + F*6 [cx-200 +6F] | iVar5 + F*-0x20 + 0xb0 [iVar5 -32F +176] | — | — | sprite slot 4 | — | |
| 32 | 8 | MFSR 0x4259d0 | (cx-200) + F*6 [cx-200 +6F] | iVar5 + F*-0x20 + 0xd8 [iVar5 -32F +216] | — | — | sprite slot 5 | — | |
| 33 | 8 | MFSR 0x4259d0 | (cx-200) + F*6 [cx-200 +6F] | iVar5 + F*-0x20 + 0x100 [iVar5 -32F +256] | — | — | sprite slot 6 | — | |
| 34 | 8 | MFSR 0x4259d0 | (cx-0x68) + F*6 [cx-104 +6F] | F*0x30 + 0x128 + iVar5 [48F +296 +iVar5] | — | — | sprite slot 7 (OK) | — | |

Delegated sub-draw fns followed: none.
Confidence: [CONFIRMED @ 0x0041f990 decomp]. [UNCERTAIN: state-4 DFLS row values x=10 width=0xe0 — these are MeasureOrCenter(str,10,0xe0) args into the value panel; first row uses sprintf row0 with g_uiFormatStringScratchTemplate; final on-panel x derives from centering inside MeasureOrCenter.]

---

### Screen 14 @ 0x0041df20 — ScreenControlOptions
Mechanism: `cx=W>>1 (uVar3); cy=H>>1 (uVar5); iVar4=cx-0xd2 (cx-210); iVar6=cy-0x9f (cy-159)`. Header QFOR uses `cx-200`. Device icon surface = g_soundOptionsMenuVolume (Controllers.tga). Value panel g_lobbyErrorDialogSurface 0xe0 x 0xa0 (224x160). Device icon src_y = (deviceIndex<<5), w=0x40 h=0x20.
States/structure: switch cases 0, (1,2), 3, (4,5), 6, 7, 8. P1/Configure/P2/Configure/OK buttons (auto-layout) + per-player device icon overlays + value panel.

| # | State/case | Helper @addr | x (verbatim) [dec] | y (verbatim) [dec] | w | h | tex/label/surface | blit-flag | notes |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 0 | CFDMB 0x425de0 | -0x100 [-256] | 0 | 0x100 [256] | 0x20 [32] | SNK_Player1ButTxt | — | auto-layout |
| 2 | 0 | CFDMB 0x425de0 | -0x100 [-256] | 0 | 0x100 [256] | 0x20 [32] | SNK_ConfigureButTxt | — | auto-layout (P1 configure) |
| 3 | 0 | CFDMB 0x425de0 | -0x100 [-256] | 0 | 0x100 [256] | 0x20 [32] | SNK_Player2ButTxt | — | auto-layout |
| 4 | 0 | CFDMB 0x425de0 | -0x100 [-256] | 0 | 0x100 [256] | 0x20 [32] | SNK_ConfigureButTxt | — | auto-layout (P2 configure) |
| 5 | 0 | CFDMB 0x425de0 | -0x100 [-256] | 0 | 0x60 [96] | 0x20 [32] | SNK_OkButTxt | — | auto-layout |
| 6 | 3 | MFSR 0x4259d0 | F*0x10 + -0x266 + iVar4 [16F -614 +iVar4] | cy-0x8f [cy-143] | — | — | sprite slot 0 | — | slide-in |
| 7 | 3 | MFSR 0x4259d0 | iVar4 + F*-0x10 + 0x27a [iVar4 -16F +634] | cy-0x67 [cy-103] | — | — | sprite slot 1 | — | |
| 8 | 3 | MFSR 0x4259d0 | F*0x10 + -0x266 + iVar4 [16F -614 +iVar4] | cy-0x17 [cy-23] | — | — | sprite slot 2 | — | |
| 9 | 3 | MFSR 0x4259d0 | iVar4 + F*-0x10 + 0x27a [iVar4 -16F +634] | cy+0x11 [cy+17] | — | — | sprite slot 3 | — | |
| 10 | 3 | MFSR 0x4259d0 | cx-0x78 [cx-120] | iVar6 + F*-0x10 + 0x398 [iVar6 -16F +920] | — | — | sprite slot 4 (OK) | — | |
| 11 | 3 (header slide-in) | QFOR 0x425660 | cx-200 [cx-200] | ((F*4 -0xdc) - hdrYOff) + iVar6 [(4F-220 -hdrYOff)+iVar6] | hdrW | hdrH | g_currentScreenIndex | 0 | |
| 12 | 3 (P1 device icon, if local_84!=4) | QFOR 0x425660 | iVar4 + F*-0x10 + 0x38c [iVar4 -16F +908] | cy-0x8f [cy-143] | 0x40 [64] | 0x20 [32] | g_soundOptionsMenuVolume; src_x=0 src_y=local_84<<5 | 0 | device icon, slide-in |
| 13 | 3 (P2 device icon, if local_88!=4) | QFOR 0x425660 | iVar4 + F*-0x10 + 0x38c [iVar4 -16F +908] | cy-0x17 [cy-23] | 0x40 [64] | 0x20 [32] | g_soundOptionsMenuVolume; src_y=local_88<<5 | 0 | device icon |
| 14 | 4,5 (header std) | QFOR 0x425660 | cx-200 [cx-200] | (iVar6 - hdrYOff) - 0x40 [-64] | hdrW | hdrH | g_currentScreenIndex | 0 | |
| 15 | 4,5 (P1 icon) | QFOR 0x425660 | cx+0x4a [cx+74] | cy-0x8f [cy-143] | 0x40 [64] | 0x20 [32] | g_soundOptionsMenuVolume; src_y=local_84<<5 | 0 | |
| 16 | 4,5 (P2 icon) | QFOR 0x425660 | cx+0x4a [cx+74] | cy-0x17 [cy-23] | 0x40 [64] | 0x20 [32] | g_soundOptionsMenuVolume; src_y=local_88<<5 | 0 | |
| 17 | 4 (panel text) MeasureOrCenter→DFLS | 0x424560 | x=0x4a [74] | (centered) | width=0xe0 [224] | — | sprintf DAT_004658e4 or s__s__d_0046603c (P1 then P2; 2 DFLS) | — | drawn into g_lobbyErrorDialogSurface |
| 18 | 4,5 | QFOR 0x425660 | cx+0x4a [cx+74] | cy-0x8f [cy-143] | 0xe0 [224] | 0xa0 [160] | g_lobbyErrorDialogSurface (value panel) | 0 | panel blit |
| 19 | 6 (header std) | QFOR 0x425660 | cx-200 [cx-200] | (iVar6 - hdrYOff) - 0x40 [-64] | hdrW | hdrH | g_currentScreenIndex | 0 | |
| 20 | 6 | QFOR 0x425660 | cx+0x4a [cx+74] | cy-0x8f [cy-143] | 0xe0 [224] | 0xa0 [160] | g_lobbyErrorDialogSurface | 0 | value panel |
| 21 | 6 (P1 icon) | QFOR 0x425660 | cx+0x4a [cx+74] | cy-0x8f [cy-143] | 0x40 [64] | 0x20 [32] | g_soundOptionsMenuVolume; src_y=local_84<<5 | 0 | |
| 22 | 6 (P2 icon) | QFOR 0x425660 | cx+0x4a [cx+74] | cy-0x17 [cy-23] | 0x40 [64] | 0x20 [32] | g_soundOptionsMenuVolume; src_y=local_88<<5 | 0 | |
| 23 | 7 (header std) | QFOR 0x425660 | cx-200 [cx-200] | (iVar6 - hdrYOff) - 0x40 [-64] | hdrW | hdrH | g_currentScreenIndex | 0 | |
| 24 | 7 | QFOR 0x425660 | cx+0x4a [cx+74] | cy-0x8f [cy-143] | 0xe0 [224] | 0xa0 [160] | g_lobbyErrorDialogSurface | 0 | value panel |
| 25 | 7 (P1 icon) | QFOR 0x425660 | cx+0x4a [cx+74] | cy-0x8f [cy-143] | 0x40 [64] | 0x20 [32] | g_soundOptionsMenuVolume; src_y=local_84<<5 | 0 | |
| 26 | 7 (P2 icon) | QFOR 0x425660 | cx+0x4a [cx+74] | cy-0x17 [cy-23] | 0x40 [64] | 0x20 [32] | g_soundOptionsMenuVolume; src_y=local_88<<5 | 0 | |
| 27 | 8 (header slide-out) | QFOR 0x425660 | iVar4 + F*-0x18 + 10 [iVar4 -24F +10] | (iVar6 - hdrYOff) - 0x40 [-64] | hdrW | hdrH | g_currentScreenIndex | 0 | NOTE: iVar4 reused from case 6/7 (uVar3+0x4a) only if fall-through; here iVar4=cx-0xd2 |
| 28 | 8 (panel slide-out) | QFOR 0x425660 | cx+0x4a + F*0xc [cx+74 +12F] | cy-0x8f [cy-143] | 0xe0 [224] | 0xa0 [160] | g_lobbyErrorDialogSurface | 0 | |
| 29 | 8 | MFSR 0x4259d0 | iVar4 + F*-8 + 10 [iVar4 -8F +10] | iVar6 + F*-0x30 + 0x10 [iVar6 -48F +16] | — | — | sprite slot 0 | — | slide-out |
| 30 | 8 | MFSR 0x4259d0 | iVar4 + F*-8 + 10 [iVar4 -8F +10] | iVar6 + F*-0x20 + 0x38 [iVar6 -32F +56] | — | — | sprite slot 1 | — | |
| 31 | 8 | MFSR 0x4259d0 | iVar4 + F*-6 + 10 [iVar4 -6F +10] | iVar6 + F*-0x18 + 0x88 [iVar6 -24F +136] | — | — | sprite slot 2 | — | |
| 32 | 8 | MFSR 0x4259d0 | iVar4 + F*-6 + 10 [iVar4 -6F +10] | iVar6 + F*-0x18 + 0xb0 [iVar6 -24F +176] | — | — | sprite slot 3 | — | |
| 33 | 8 | MFSR 0x4259d0 | iVar4 + (F*3 + 0x2d)*2 [iVar4 + (3F+45)*2] | F*0x30 + 0x128 + iVar6 [48F +296 +iVar6] | — | — | sprite slot 4 (OK) | — | |
| 34 | 8 (P1 icon) | QFOR 0x425660 | cx+0x4a [cx+74] | iVar6 + F*-0x20 + 0x10 [iVar6 -32F +16] | 0x40 [64] | 0x20 [32] | g_soundOptionsMenuVolume; src_y=local_84<<5 | 0 | |
| 35 | 8 (P2 icon) | QFOR 0x425660 | cx+0x4a [cx+74] | iVar6 + F*-0x20 + 0x88 [iVar6 -32F +136] | 0x40 [64] | 0x20 [32] | g_soundOptionsMenuVolume; src_y=local_88<<5 | 0 | |

Note: in cases 6 and 7 a local `iVar4 = uVar3 + 0x4a (cx+74)` is reassigned and used for the panel/icon QFOR dst_x (rows 20/24 use that path). The slide bases iVar4=cx-0xd2 apply to states 3 and 8.
Delegated sub-draw fns followed: none.
Confidence: [CONFIRMED @ 0x0041df20 decomp]. [UNCERTAIN: state-4 DFLS x exactly 0x4a width 0xe0 from MeasureOrCenter(local_80,0x4a,0xe0); final on-panel position derived inside MeasureOrCenter centering. device-icon src_y = (local_84/local_88 << 5) is a runtime device-index, not a literal.]

---

## Summary of helper coverage
- CreateFrontendDisplayModeButton (0x425de0): used on all 5 screens.
- QueueFrontendOverlayRect (0x425660): used on all 5 screens (headers, panels, device icons, error dialogs).
- MoveFrontendSpriteRect (0x4259d0): used on all 5 screens (animated button sprites).
- DrawFrontendLocalizedStringToSurface (0x424560): screens 11, 13, 14 — no own x/y; placement via preceding MeasureOrCenterFrontendLocalizedString.
- Copy16BitSurfaceRect (0x004251a0): NOT CALLED in any of the 5.
- DrawFrontendButtonBackground (0x00425b60): NOT CALLED in any of the 5.
# Frontend Layout RE Harvest — Part 15 (screen-table indices 15–19)

Source: `TD5_d3d.exe`, table `g_frontendScreenFnTable @ 0x004655C4` (30 LE uint32).
Decoded slice indices 15–19:

| idx | fn ptr (LE bytes) | entry addr | function |
|-----|-------------------|-----------|----------|
| 15 | `90 ea 41 00` | 0x0041ea90 | ScreenSoundOptions |
| 16 | `00 04 42 00` | 0x00420400 | ScreenDisplayOptions |
| 17 | `70 0c 42 00` | 0x00420c70 | ScreenTwoPlayerOptions |
| 18 | `00 fe 40 00` | 0x0040fe00 | ScreenControllerBindingPage |
| 19 | `60 84 41 00` | 0x00418460 | ScreenMusicTestExtras |

Helper callee addresses (confirmed by name lookup):
- `CreateFrontendDisplayModeButton @ 0x00425de0` (label, x, y, w, h, ud) — negative x = auto-layout flag.
- `QueueFrontendOverlayRect @ 0x00425660` (dst_x, dst_y, src_x, src_y, w, h, color, surface).
- `MoveFrontendSpriteRect @ 0x004259d0` (slot, x, y).
- (Copy16BitSurfaceRect @ 0x004251a0 / DrawFrontendButtonBackground @ 0x00425b60: NO direct calls with x/y operands found in any of the 5 functions.)
- `DrawFrontendLocalizedStringToSurface @ 0x00424560`: called with NO x/y args (void); the x/y for the string is set by the preceding `MeasureOrCenterFrontendLocalizedString(text, x_or_yfield, width)` call (arg2 = y-position field, arg3 = wrap width). Those measure calls are recorded where present.

Canvas bases used in every function (verbatim names per fn vary; semantics identical):
- `HW = g_frontendCanvasW >> 1` (half canvas width).
- `HH = g_frontendCanvasH >> 1` (half canvas height).
- `BX = HW - 0xd2` (= HW - 210)  [auto-layout sprite/button base X].
- `BY = HH - 0x9f` (= HH - 159)  [auto-layout sprite/button base Y].
- `F = g_frontendAnimFrameCounter` (per-frame slide animation counter).

NOTE on `CreateFrontendDisplayModeButton`: every call in these screens passes x as a NEGATIVE literal (-0x100, -0x120, -0x128) with y=0. Negative x = auto-layout flag (button position computed later by the layout/MoveFrontendSpriteRect logic, not the literal). The negative literal magnitude equals the button width (0x100/0x120) except for OK button (still -0x100/-0x120 but w=0x60).

---

### Screen 15 @ 0x0041ea90 — ScreenSoundOptions
Mechanism: `HW=canvasW>>1`, `HH=canvasH>>1`, `BX=HW-0xd2`, `BY=HH-0x9f`. F=g_frontendAnimFrameCounter.
States/structure: switch(g_frontendInnerState) cases 0,1,2,3,4,5,6,7,8.

| # | State/case | Helper @addr | x (verbatim) [dec] | y (verbatim) [dec] | w | h | tex/label/surface | blit-flag | notes |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 0 init | CreateFrontendDisplayModeButton @0x425de0 | -0x100 [-256] | 0 | 0x100 [256] | 0x20 [32] | SNK_SfxModeButTxt | — | neg x = auto-layout |
| 2 | 0 init | CreateFrontendDisplayModeButton @0x425de0 | -0x100 [-256] | 0 | 0x100 [256] | 0x20 [32] | SNK_SfxVolumeButTxt | — | auto-layout |
| 3 | 0 init | CreateFrontendDisplayModeButton @0x425de0 | -0x100 [-256] | 0 | 0x100 [256] | 0x20 [32] | SNK_MusicVolumeButTxt | — | auto-layout |
| 4 | 0 init | CreateFrontendDisplayModeButton @0x425de0 | -0x100 [-256] | 0 | 0x100 [256] | 0x20 [32] | SNK_MusicTestButTxt | — | auto-layout |
| 5 | 0 init | CreateFrontendDisplayModeButton @0x425de0 | -0x100 [-256] | 0 | 0x60 [96] | 0x20 [32] | SNK_OkButTxt | — | OK btn; EscKeyButtonIndex=4 |
| 6 | 3 slide-in | MoveFrontendSpriteRect @0x4259d0 | F*0x10 + -0x266 + BX  [F*16 -614 +BX] | HH - 0x8f [HH-143] | — | — | sprite slot 0 | — | |
| 7 | 3 slide-in | MoveFrontendSpriteRect @0x4259d0 | BX + F*-0x10 + 0x27a  [BX -F*16 +634] | HH - 0x3f [HH-63] | — | — | sprite slot 1 | — | |
| 8 | 3 slide-in | MoveFrontendSpriteRect @0x4259d0 | F*0x10 + -0x266 + BX  [F*16 -614 +BX] | HH - 0x17 [HH-23] | — | — | sprite slot 2 | — | |
| 9 | 3 slide-in | MoveFrontendSpriteRect @0x4259d0 | BX + F*-0x10 + 0x27a  [BX -F*16 +634] | HH + 0x39 [HH+57] | — | — | sprite slot 3 | — | |
| 10 | 3 slide-in | MoveFrontendSpriteRect @0x4259d0 | HW - 0x78 [HW-120] | BY + F*-0x10 + 0x398  [BY -F*16 +920] | — | — | sprite slot 4 (OK) | — | |
| 11 | 3 slide-in | QueueFrontendOverlayRect @0x425660 | HW - 200 [HW-200] | (F*4 + -0xdc - g_menuHeaderLabelYOffset) + BY  [F*4 -220 -hdrYOff +BY] | g_menuHeaderLabelSurfaceWidth | g_menuHeaderLabelSurfaceHeight | g_currentScreenIndex (header label) | 0 | src 0,0 |
| 12 | 3 slide-in | QueueFrontendOverlayRect @0x425660 | (HW - 0x226) + F*0x10  [HW -550 +F*16] | HH - 0x37 [HH-55] | 0xe0 [224] | 0xc [12] | g_soundOptionsMusicVolume (VolumeBox bg) | 0 | music vol box |
| 13 | 3 slide-in | QueueFrontendOverlayRect @0x425660 | F*0x10 + -0x153 + BX  [F*16 -339 +BX] | HH - 0x36 [HH-54] | uVar2 (clamped vol fill, ≤0xde) | 10 | g_soundOptionsSfxVolume (VolumeFill) | 0 | only if vol!=0; uVar2=master%*F scaled |
| 14 | 3 slide-in | QueueFrontendOverlayRect @0x425660 | BX + F*-0x10 + 0x38c  [BX -F*16 +908] | HH - 0xf [HH-15] | 0xe0 [224] | 0xc [12] | g_soundOptionsMusicVolume | 0 | CD vol box |
| 15 | 3 slide-in | QueueFrontendOverlayRect @0x425660 | BX + F*-0x10 + 0x38d  [BX -F*16 +909] | HH - 0xe [HH-14] | uVar6 (clamped CD vol, ≤0xde) | 10 | g_soundOptionsSfxVolume | 0 | only if vol!=0 |
| 16 | 3 slide-in | QueueFrontendOverlayRect @0x425660 | BX + F*-0x10 + 0x38c  [BX -F*16 +908] | HH - 0x8f [HH-143] | 0x40 [64] | 0x20 [32] | g_soundOptionsMenuVolume (Controllers tga) | 0 | src_x=0, src_y=(g_sfxPlaybackMode+4)*0x20 |
| 17 | 4/5 static | QueueFrontendOverlayRect @0x425660 | HW - 200 [HW-200] | (BY - g_menuHeaderLabelYOffset) + -0x40  [BY -hdrYOff -64] | hdrW | hdrH | g_currentScreenIndex (header) | 0 | |
| 18 | 4/5 static | QueueFrontendOverlayRect @0x425660 | HW + 0x4a [HW+74] | HH - 0x37 [HH-55] | 0xe0 [224] | 0xc [12] | g_soundOptionsMusicVolume | 0 | iVar7=HW+0x4a |
| 19 | 4/5 static | QueueFrontendOverlayRect @0x425660 | HW + 0x4b [HW+75] | HH - 0x36 [HH-54] | iVar5 (master vol fill, ≤0xde) | 10 | g_soundOptionsSfxVolume | 0 | only if >0 |
| 20 | 4/5 static | QueueFrontendOverlayRect @0x425660 | HW + 0x4a [HW+74] | HH - 0xf [HH-15] | 0xe0 [224] | 0xc [12] | g_soundOptionsMusicVolume | 0 | CD box |
| 21 | 4/5 static | QueueFrontendOverlayRect @0x425660 | HW + 0x4b [HW+75] | HH - 0xe [HH-14] | iVar5 (CD vol fill, ≤0xde) | 10 | g_soundOptionsSfxVolume | 0 | only if >0 |
| 22 | 4/5 static | QueueFrontendOverlayRect @0x425660 | HW + 0x4a [HW+74] | HH - 0x8f [HH-143] | 0x40 [64] | 0x20 [32] | g_soundOptionsMenuVolume | 0 | src_y=(g_sfxPlaybackMode+4)*0x20 |
| 23 | 4 only | MeasureOrCenterFrontendLocalizedString | y=0x4a [74] | (width=0xe0) | — | — | SNK_SFX_Modes[g_sfxPlaybackMode] | — | sets text pos for next DrawFrontendLocalizedStringToSurface |
| 24 | 4/5 static | QueueFrontendOverlayRect @0x425660 | HW + 0x4a [HW+74] | HH - 0x8f [HH-143] | 0xe0 [224] | 0xa0 [160] | g_lobbyErrorDialogSurface (mode-label box) | 0 | iVar7=HW+0x4a |
| 25 | 6 input | QueueFrontendOverlayRect @0x425660 | HW - 200 [HW-200] | (BY - hdrYOff) + -0x40 | hdrW | hdrH | g_currentScreenIndex (header) | 0 | |
| 26 | 6 input | QueueFrontendOverlayRect @0x425660 | HW + 0x4a [HW+74] | HH - 0x8f [HH-143] | 0xe0 [224] | 0xa0 [160] | g_lobbyErrorDialogSurface | 0 | iVar5=HW+0x4a |
| 27 | 6 input | QueueFrontendOverlayRect @0x425660 | HW + 0x4a [HW+74] | HH - 0x37 [HH-55] | 0xe0 [224] | 0xc [12] | g_soundOptionsMusicVolume | 0 | |
| 28 | 6 input | QueueFrontendOverlayRect @0x425660 | HW + 0x4b [HW+75] | HH - 0x36 [HH-54] | iVar7 (master vol ≤0xde) | 10 | g_soundOptionsSfxVolume | 0 | only if >0 |
| 29 | 6 input | QueueFrontendOverlayRect @0x425660 | HW + 0x4a [HW+74] | HH - 0xf [HH-15] | 0xe0 [224] | 0xc [12] | g_soundOptionsMusicVolume | 0 | CD box |
| 30 | 6 input | QueueFrontendOverlayRect @0x425660 | HW + 0x4b [HW+75] | HH - 0xe [HH-14] | iVar7 (CD vol ≤0xde) | 10 | g_soundOptionsSfxVolume | 0 | only if >0 |
| 31 | 6 input | QueueFrontendOverlayRect @0x425660 | HW + 0x4a [HW+74] | HH - 0x8f [HH-143] | 0x40 [64] | 0x20 [32] | g_soundOptionsMenuVolume | 0 | src_y=(g_sfxPlaybackMode+4)*0x20 |
| 32 | 7 settle | QueueFrontendOverlayRect @0x425660 | HW - 200 [HW-200] | (BY - hdrYOff) + -0x40 | hdrW | hdrH | g_currentScreenIndex (header) | 0 | |
| 33 | 7 settle | QueueFrontendOverlayRect @0x425660 | HW + 0x4a [HW+74] | HH - 0x8f [HH-143] | 0xe0 [224] | 0xa0 [160] | g_lobbyErrorDialogSurface | 0 | iVar5=HW+0x4a |
| 34 | 7 settle | QueueFrontendOverlayRect @0x425660 | HW + 0x4a [HW+74] | HH - 0x37 [HH-55] | 0xe0 [224] | 0xc [12] | g_soundOptionsMusicVolume | 0 | |
| 35 | 7 settle | QueueFrontendOverlayRect @0x425660 | HW + 0x4b [HW+75] | HH - 0x36 [HH-54] | iVar7 (master vol ≤0xde) | 10 | g_soundOptionsSfxVolume | 0 | only if >0 |
| 36 | 7 settle | QueueFrontendOverlayRect @0x425660 | HW + 0x4a [HW+74] | HH - 0xf [HH-15] | 0xe0 [224] | 0xc [12] | g_soundOptionsMusicVolume | 0 | CD box |
| 37 | 7 settle | QueueFrontendOverlayRect @0x425660 | HW + 0x4b [HW+75] | HH - 0xe [HH-14] | iVar7 (CD vol ≤0xde) | 10 | g_soundOptionsSfxVolume | 0 | only if >0 |
| 38 | 7 settle | QueueFrontendOverlayRect @0x425660 | HW + 0x4a [HW+74] | HH - 0x8f [HH-143] | 0x40 [64] | 0x20 [32] | g_soundOptionsMenuVolume | 0 | src_y=(g_sfxPlaybackMode+4)*0x20 |
| 39 | 8 slide-out | QueueFrontendOverlayRect @0x425660 | iVar7 + F*-0x18 + 10  [HW+0x4a -F*24 +10] | (BY - hdrYOff) + -0x40 | hdrW | hdrH | g_currentScreenIndex (header) | 0 | iVar7 here = HW+0x4a |
| 40 | 8 slide-out | QueueFrontendOverlayRect @0x425660 | HW + 0x4a + F*0xc  [HW+74 +F*12] | HH - 0x8f [HH-143] | 0xe0 [224] | 0xa0 [160] | g_lobbyErrorDialogSurface | 0 | |
| 41 | 8 slide-out | MoveFrontendSpriteRect @0x4259d0 | iVar7 + F*-8 + 10  [HW+0x4a -F*8 +10] | BY + F*-0x30 + 0x10  [BY -F*48 +16] | — | — | sprite slot 0 | — | |
| 42 | 8 slide-out | MoveFrontendSpriteRect @0x4259d0 | iVar7 + F*-8 + 10 | BY + F*-0x20 + 0x60  [BY -F*32 +96] | — | — | sprite slot 1 | — | |
| 43 | 8 slide-out | MoveFrontendSpriteRect @0x4259d0 | iVar7 + F*-6 + 10  [HW+0x4a -F*6 +10] | BY + F*-0x18 + 0x88  [BY -F*24 +136] | — | — | sprite slot 2 | — | |
| 44 | 8 slide-out | MoveFrontendSpriteRect @0x4259d0 | (HW - 200) + F*6  [HW-200 +F*6] | BY + F*-0x18 + 0xd8  [BY -F*24 +216] | — | — | sprite slot 3 | — | |
| 45 | 8 slide-out | MoveFrontendSpriteRect @0x4259d0 | iVar7 + (F*3 + 0x2d)*2  [HW+0x4a + (F*3+45)*2] | F*0x30 + 0x128 + BY  [F*48 +296 +BY] | — | — | sprite slot 4 | — | |
| 46 | 8 slide-out | QueueFrontendOverlayRect @0x425660 | F*0x10 + 0x11c + iVar7  [F*16 +284 +HW+0x4a] | BY + F*-0x20 + 0x68  [BY -F*32 +104] | 0xe0 [224] | 0xc [12] | g_soundOptionsMusicVolume | 0 | iVar7=HW+0x4a |
| 47 | 8 slide-out | QueueFrontendOverlayRect @0x425660 | F*0x10 + 0x11d + iVar7  [F*16 +285 +HW+0x4a] | BY + F*-0x20 + 0x69  [BY -F*32 +105] | iVar3 (master vol ≤0xde) | 10 | g_soundOptionsSfxVolume | 0 | only if >0 |
| 48 | 8 slide-out | QueueFrontendOverlayRect @0x425660 | HW + 0x4a + F*10  [HW+74 +F*10] | BY + (6 - F)*0x18  [BY + (6-F)*24] | 0xe0 [224] | 0xc [12] | g_soundOptionsMusicVolume | 0 | CD box |
| 49 | 8 slide-out | QueueFrontendOverlayRect @0x425660 | HW + 0x4b + F*10  [HW+75 +F*10] | BY + F*-0x18 + 0x91  [BY -F*24 +145] | iVar7 (CD vol ≤0xde) | 10 | g_soundOptionsSfxVolume | 0 | only if >0 |
| 50 | 8 slide-out | QueueFrontendOverlayRect @0x425660 | HW + 0x4a [HW+74] | BY + F*-0x20 + 0x10  [BY -F*32 +16] | 0x40 [64] | 0x20 [32] | g_soundOptionsMenuVolume | 0 | src_y=(g_sfxPlaybackMode+4)*0x20 |

Delegated sub-draw fns followed: none (all draw calls inline).
Confidence: [CONFIRMED @ 0x0041ea90; helpers @ 0x425de0/0x425660/0x4259d0]. Volume-fill widths (uVar2/uVar6/iVar5/iVar7/iVar3) are computed (vol%* scale, clamped ≤0xde) not literals — recorded as such.

---

### Screen 16 @ 0x00420400 — ScreenDisplayOptions
Mechanism: `HW=canvasW>>1`, `HH=canvasH>>1`, `BX=HW-0xd2`, `BY=HH-0x9f`. F=g_frontendAnimFrameCounter.
States/structure: switch(g_frontendInnerState) cases 0,1,2,3,4,5,6,7,8.

| # | State/case | Helper @addr | x (verbatim) [dec] | y (verbatim) [dec] | w | h | tex/label/surface | blit-flag | notes |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 0 init | CreateFrontendDisplayModeButton @0x425de0 | -0x120 [-288] | 0 | 0x120 [288] | 0x20 [32] | SNK_ResolutionButTxt | — | auto-layout |
| 2 | 0 init (CanFog==1) | CreateFrontendDisplayModeButton @0x425de0 | -0x120 [-288] | 0 | 0x120 [288] | 0x20 [32] | SNK_FoggingButTxt | — | only if DXD3D::CanFog()==1 |
| 2b | 0 init (CanFog!=1) | CreateFrontendDisplayModePreviewButton | -0x120 [-288] | 0 | 0x120 [288] | 0x20 [32] | SNK_FoggingButTxt | — | preview-button variant (disabled fog) |
| 3 | 0 init | CreateFrontendDisplayModeButton @0x425de0 | -0x120 [-288] | 0 | 0x120 [288] | 0x20 [32] | SNK_SpeedReadoutButTxt | — | auto-layout |
| 4 | 0 init | CreateFrontendDisplayModeButton @0x425de0 | -0x120 [-288] | 0 | 0x120 [288] | 0x20 [32] | SNK_CameraDampingButTxt | — | auto-layout |
| 5 | 0 init | CreateFrontendDisplayModeButton @0x425de0 | -0x120 [-288] | 0 | 0x60 [96] | 0x20 [32] | SNK_OkButTxt | — | OK btn; EscKeyButtonIndex=4 |
| 6 | 3 slide-in | MoveFrontendSpriteRect @0x4259d0 | F*0x10 + -0x266 + BX  [F*16 -614 +BX] | HH - 0x8f [HH-143] | — | — | sprite slot 0 | — | |
| 7 | 3 slide-in | MoveFrontendSpriteRect @0x4259d0 | BX + F*-0x10 + 0x27a  [BX -F*16 +634] | HH - 0x67 [HH-103] | — | — | sprite slot 1 | — | |
| 8 | 3 slide-in | MoveFrontendSpriteRect @0x4259d0 | F*0x10 + -0x266 + BX  [F*16 -614 +BX] | HH - 0x17 [HH-23] | — | — | sprite slot 2 | — | |
| 9 | 3 slide-in | MoveFrontendSpriteRect @0x4259d0 | BX + F*-0x10 + 0x27a  [BX -F*16 +634] | HH + 0x11 [HH+17] | — | — | sprite slot 3 | — | |
| 10 | 3 slide-in | MoveFrontendSpriteRect @0x4259d0 | F*0x10 + -0x216 + BX  [F*16 -534 +BX] | HH + 0x89 [HH+137] | — | — | sprite slot 4 (OK) | — | |
| 11 | 3 slide-in | QueueFrontendOverlayRect @0x425660 | HW - 200 [HW-200] | (F*4 + -0xdc - hdrYOff) + BY  [F*4 -220 -hdrYOff +BY] | hdrW | hdrH | g_currentScreenIndex (header) | 0 | |
| 12 | 4/5 static | QueueFrontendOverlayRect @0x425660 | HW - 200 [HW-200] | (BY - hdrYOff) + -0x40 | hdrW | hdrH | g_currentScreenIndex (header) | 0 | |
| 13 | 4 only | MeasureOrCenterFrontendLocalizedString | y=10 [10] | (width=0xe0) | — | — | g_displayModeStringTable[ordinal*0x20] | — | resolution label |
| 14 | 4 only (CanFog==1) | MeasureOrCenterFrontendLocalizedString | y=10 [10] | (width=0xe0) | — | — | SNK_OnOffTxt[gFoggingConfigShadow] | — | fog label |
| 15 | 4 only | MeasureOrCenterFrontendLocalizedString | y=10 [10] | (width=0xe0) | — | — | SNK_SpeedReadTxt[unitsShadow] | — | speed-readout label |
| 16 | 4 only | MeasureOrCenterFrontendLocalizedString | y=10 [10] | (width=0xe0) | — | — | local_4 (sprintf camera-damping #) | — | camera-damping value |
| 17 | 4/5 static | QueueFrontendOverlayRect @0x425660 | HW + 0x4a [HW+74] | HH - 0x8f [HH-143] | 0xe0 [224] | 0x118 [280] | g_lobbyErrorDialogSurface (option-values box) | 0 | |
| 18 | 6 input | QueueFrontendOverlayRect @0x425660 | HW - 200 [HW-200] | (BY - hdrYOff) + -0x40 | hdrW | hdrH | g_currentScreenIndex (header) | 0 | |
| 19 | 6 input | QueueFrontendOverlayRect @0x425660 | HW + 0x4a [HW+74] | HH - 0x8f [HH-143] | 0xe0 [224] | 0x118 [280] | g_lobbyErrorDialogSurface | 0 | |
| 20 | 7 settle | QueueFrontendOverlayRect @0x425660 | HW - 200 [HW-200] | (BY - hdrYOff) + -0x40 | hdrW | hdrH | g_currentScreenIndex (header) | 0 | |
| 21 | 7 settle | QueueFrontendOverlayRect @0x425660 | HW + 0x4a [HW+74] | HH - 0x8f [HH-143] | 0xe0 [224] | 0x118 [280] | g_lobbyErrorDialogSurface | 0 | |
| 22 | 8 slide-out | QueueFrontendOverlayRect @0x425660 | BX + F*-0x18 + 10  [BX -F*24 +10] | (BY - hdrYOff) + -0x40 | hdrW | hdrH | g_currentScreenIndex (header) | 0 | iVar4=BX here |
| 23 | 8 slide-out | QueueFrontendOverlayRect @0x425660 | HW + 0x4a + F*0xc  [HW+74 +F*12] | HH - 0x8f [HH-143] | 0xe0 [224] | 0x118 [280] | g_lobbyErrorDialogSurface | 0 | |
| 24 | 8 slide-out | MoveFrontendSpriteRect @0x4259d0 | (HW - 200) + F*8  [HW-200 +F*8] | BY + F*-0x20 + 0x10  [BY -F*32 +16] | — | — | sprite slot 0 | — | |
| 25 | 8 slide-out | MoveFrontendSpriteRect @0x4259d0 | BX + F*-8 + 10  [BX -F*8 +10] | BY + F*-0x20 + 0x38  [BY -F*32 +56] | — | — | sprite slot 1 | — | |
| 26 | 8 slide-out | MoveFrontendSpriteRect @0x4259d0 | BX + F*-8 + 10  [BX -F*8 +10] | BY + F*-0x20 + 0x88  [BY -F*32 +136] | — | — | sprite slot 2 | — | |
| 27 | 8 slide-out | MoveFrontendSpriteRect @0x4259d0 | (HW - 200) + F*8  [HW-200 +F*8] | BY + F*-0x20 + 0xb0  [BY -F*32 +176] | — | — | sprite slot 3 | — | |
| 28 | 8 slide-out | MoveFrontendSpriteRect @0x4259d0 | BX + (F*3 + 0x2d)*2  [BX + (F*3+45)*2] | F*0x20 + 0x128 + BY  [F*32 +296 +BY] | — | — | sprite slot 4 | — | |

Delegated sub-draw fns followed: CreateFrontendDisplayModePreviewButton (case 0, fog-disabled path) — preview/disabled variant of button create; same arg shape. Not decompiled further (outside slice scope; row 2b records its literal args).
Confidence: [CONFIRMED @ 0x00420400; helpers @ 0x425de0/0x425660/0x4259d0]. [UNCERTAIN: CreateFrontendDisplayModePreviewButton entry address not resolved — name only from decomp; literal args confirmed].

---

### Screen 17 @ 0x00420c70 — ScreenTwoPlayerOptions
Mechanism: `HW=canvasW>>1`, `HH=canvasH>>1`, `BX=HW-0xd2`, `BY=HH-0x9f`. F=g_frontendAnimFrameCounter.
States/structure: switch(g_frontendInnerState) cases 0,1,2,3,4,5,6,7,8. (3 buttons: SplitScreen, Catchup, OK.)

| # | State/case | Helper @addr | x (verbatim) [dec] | y (verbatim) [dec] | w | h | tex/label/surface | blit-flag | notes |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 0 init | CreateFrontendDisplayModeButton @0x425de0 | -0x100 [-256] | 0 | 0x100 [256] | 0x20 [32] | SNK_SplitScreenButTxt | — | auto-layout |
| 2 | 0 init | CreateFrontendDisplayModeButton @0x425de0 | -0x100 [-256] | 0 | 0x100 [256] | 0x20 [32] | SNK_CatchupTxt | — | auto-layout |
| 3 | 0 init | CreateFrontendDisplayModeButton @0x425de0 | -0x100 [-256] | 0 | 0x60 [96] | 0x20 [32] | SNK_OkButTxt | — | OK; EscKeyButtonIndex=2 |
| 4 | 3 slide-in | MoveFrontendSpriteRect @0x4259d0 | F*0x10 + -0x266 + BX  [F*16 -614 +BX] | HH - 0x8f [HH-143] | — | — | sprite slot 0 | — | |
| 5 | 3 slide-in | MoveFrontendSpriteRect @0x4259d0 | BX + F*-0x10 + 0x27a  [BX -F*16 +634] | HH - 0x3f [HH-63] | — | — | sprite slot 1 | — | |
| 6 | 3 slide-in | MoveFrontendSpriteRect @0x4259d0 | HW - 0x78 [HW-120] | BY + F*-0x10 + 0x398  [BY -F*16 +920] | — | — | sprite slot 2 (OK) | — | |
| 7 | 3 slide-in | QueueFrontendOverlayRect @0x425660 | HW - 200 [HW-200] | (F*4 + -0xdc - hdrYOff) + BY | hdrW | hdrH | g_currentScreenIndex (header) | 0 | |
| 8 | 3 slide-in | QueueFrontendOverlayRect @0x425660 | BX + F*-0x10 + 0x38c  [BX -F*16 +908] | HH - 0x8f [HH-143] | 0x40 [64] | 0x20 [32] | g_twoPlayerOptionsSelection (SplitScreen tga) | 0 | src_x=0, src_y=g_twoPlayerSplitMode<<5 |
| 9 | 4/5 static | QueueFrontendOverlayRect @0x425660 | HW - 200 [HW-200] | (BY - hdrYOff) + -0x40 | hdrW | hdrH | g_currentScreenIndex (header) | 0 | |
| 10 | 4/5 static | QueueFrontendOverlayRect @0x425660 | HW + 0x4a [HW+74] | HH - 0x8f [HH-143] | 0x40 [64] | 0x20 [32] | g_twoPlayerOptionsSelection | 0 | src_y=g_twoPlayerSplitMode<<5 |
| 11 | 4 only | MeasureOrCenterFrontendLocalizedString | y=0x4a [74] | (width=0xe0) | — | — | SNK_Split_Modes[g_twoPlayerSplitMode] | — | split-mode label |
| 12 | 4 only | MeasureOrCenterFrontendLocalizedString | y=0x4a [74] | (width=0xe0) | — | — | local_4 (sprintf catchup #) | — | catchup-assist value |
| 13 | 4/5 static | QueueFrontendOverlayRect @0x425660 | HW + 0x4a [HW+74] | HH - 0x8f [HH-143] | 0xe0 [224] | 0x78 [120] | g_lobbyErrorDialogSurface (values box) | 0 | |
| 14 | 6 input | QueueFrontendOverlayRect @0x425660 | HW - 200 [HW-200] | (BY - hdrYOff) + -0x40 | hdrW | hdrH | g_currentScreenIndex (header) | 0 | |
| 15 | 6 input | QueueFrontendOverlayRect @0x425660 | HW + 0x4a [HW+74] | HH - 0x8f [HH-143] | 0x40 [64] | 0x20 [32] | g_twoPlayerOptionsSelection | 0 | src_y=g_twoPlayerSplitMode<<5 |
| 16 | 6 input | QueueFrontendOverlayRect @0x425660 | HW + 0x4a [HW+74] | HH - 0x8f [HH-143] | 0xe0 [224] | 0x78 [120] | g_lobbyErrorDialogSurface | 0 | |
| 17 | 7 settle | QueueFrontendOverlayRect @0x425660 | HW - 200 [HW-200] | (BY - hdrYOff) + -0x40 | hdrW | hdrH | g_currentScreenIndex (header) | 0 | |
| 18 | 7 settle | QueueFrontendOverlayRect @0x425660 | HW + 0x4a [HW+74] | HH - 0x8f [HH-143] | 0x40 [64] | 0x20 [32] | g_twoPlayerOptionsSelection | 0 | src_y=g_twoPlayerSplitMode<<5 |
| 19 | 7 settle | QueueFrontendOverlayRect @0x425660 | HW + 0x4a [HW+74] | HH - 0x8f [HH-143] | 0xe0 [224] | 0x78 [120] | g_lobbyErrorDialogSurface | 0 | |
| 20 | 8 slide-out | QueueFrontendOverlayRect @0x425660 | BX + F*-0x18 + 10  [BX -F*24 +10] | (BY - hdrYOff) + -0x40 | hdrW | hdrH | g_currentScreenIndex (header) | 0 | iVar3=BX |
| 21 | 8 slide-out | QueueFrontendOverlayRect @0x425660 | HW + 0x4a [HW+74] | F*0x20 + 0x10 + BY  [F*32 +16 +BY] | 0x40 [64] | 0x20 [32] | g_twoPlayerOptionsSelection | 0 | src_y=g_twoPlayerSplitMode<<5 |
| 22 | 8 slide-out | QueueFrontendOverlayRect @0x425660 | HW + 0x4a + F*0xc  [HW+74 +F*12] | HH - 0x8f [HH-143] | 0xe0 [224] | 0x78 [120] | g_lobbyErrorDialogSurface | 0 | |
| 23 | 8 slide-out | MoveFrontendSpriteRect @0x4259d0 | (HW - 200) + F*8  [HW-200 +F*8] | BY + F*-0x20 + 0x10  [BY -F*32 +16] | — | — | sprite slot 0 | — | |
| 24 | 8 slide-out | MoveFrontendSpriteRect @0x4259d0 | BX + F*-8 + 10  [BX -F*8 +10] | BY + F*-0x20 + 0x10  [BY -F*32 +16] | — | — | sprite slot 1 | — | |
| 25 | 8 slide-out | MoveFrontendSpriteRect @0x4259d0 | BX + (F*3 + 0x2d)*2  [BX + (F*3+45)*2] | F*0x20 + 0x128 + BY  [F*32 +296 +BY] | — | — | sprite slot 2 | — | |

Delegated sub-draw fns followed: none.
Confidence: [CONFIRMED @ 0x00420c70; helpers @ 0x425de0/0x425660/0x4259d0].

---

### Screen 18 @ 0x0040fe00 — ScreenControllerBindingPage
Mechanism: `HW=canvasW>>1`, `HH=canvasH>>1`, `BX=HW-0xd2`, `BY=HH-0x9f`. F=g_frontendAnimFrameCounter. Branches on device type (joystick/wheel/keyboard) and button count.
States/structure: switch(g_frontendInnerState) cases 0, 9, 10(0xa), 11(0xb), 0x13, 0x14, 0x19, 0x1a, 0x1b. (Two parallel flows: joystick-binding 9/10/11; keyboard-binding 0x13/0x14/0x19/0x1a/0x1b.)

| # | State/case | Helper @addr | x (verbatim) [dec] | y (verbatim) [dec] | w | h | tex/label/surface | blit-flag | notes |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 0 init (header text) | MeasureOrCenterFrontendLocalizedString | y=0 [0] | (width=0x1c0) | — | — | SNK_PressingTxt | — | sets pos for DrawFrontendLocalizedStringToSurface |
| 2 | 0 init | MeasureOrCenterFrontendLocalizedString | y=0 [0] | (width=0x1c0) | — | — | SNK_ConfigurationTxt | — | |
| 3 | 0 init (wheel/none/etc) | DrawControlBindingTextWithOkButton | — | — | — | — | (delegate) | — | wheel path: device&0xff00==0x600 |
| 4 | 0 init (no-axis) | OpenControllerBindingPageNoneHeader | — | — | — | — | (delegate) | — | uVar12 in {0,1,2}: none/partial axis |
| 5 | 0 init | CreateFrontendDisplayModeButton @0x425de0 | -0x128 [-296] | 0 | 0x60 [96] | 0x20 [32] | SNK_OkButTxt | — | OK; auto-layout |
| 6 | 9 slide-in | QueueFrontendOverlayRect @0x425660 | (HW - 0x368) + F*0x18  [HW-872 +F*24] | (BY - hdrYOff) + -0x40 | hdrW | hdrH | g_currentScreenIndex (header) | 0 | |
| 7 | 9 slide-in | MoveFrontendSpriteRect @0x4259d0 | BX + F*-0x10 + 0x22a  [BX -F*16 +554] | HH + 0x89  [HH+137]  (uVar12+0x89; uVar12=HH) | — | — | sprite slot 0 (OK) | — | |
| 8 | 10 (0xa) live | QueueFrontendOverlayRect @0x425660 | HW - 200 [HW-200] | (BY - hdrYOff) + -0x40 | hdrW | hdrH | g_currentScreenIndex (header) | 0 | iVar11=HW-200 |
| 9 | 10 (0xa) per-button row | QueueFrontendOverlayRect @0x425660 | iVar11 [HW-200] | local_20 (=local_14+0x20, then +0x18 per row); local_14 = BY + count*-0xc + 0x9a | 0x10 [16] | 0x10 [16] | g_controllerBindingPage_inputCursor (ButtonLights tga) | 0 | src_x=0, src_y= 0 or 0x10 (pressed); loop over buttons |
| 10 | 10 (0xa) | QueueFrontendOverlayRect @0x425660 | HW - 0xb0 [HW-176] | local_14 (= BY + count*-0xc + 0x9a) | 0x1c0 [448] | 0xd8 [216] | g_lobbyErrorDialogSurface (binding text box) | 0 | |
| 11 | 10 (0xa) | QueueFrontendOverlayRect @0x425660 | iVar11 [HW-200] | iVar13 [BY] | 0x1c0 [448] | 0x40 [64] | g_controllerBindingPage_state (status box) | 0 | |
| 12 | 11 (0xb) slide-out | QueueFrontendOverlayRect @0x425660 | iVar11 + F*-0x18 + 10  [HW-200 -F*24 +10] | (BY - hdrYOff) + -0x40 | hdrW | hdrH | g_currentScreenIndex (header) | 0 | |
| 13 | 11 (0xb) | MoveFrontendSpriteRect @0x4259d0 | F*0x10 + 0x6a + iVar11  [F*16 +106 +HW-200] | HH + 0x89  [HH+137]  (uVar12+0x89) | — | — | sprite slot 0 | — | |
| 14 | 11 (0xb) | QueueFrontendOverlayRect @0x425660 | HW - 0xb0 [HW-176] | (HH-5) + (F*2 - count)*0xc  [(HH-5)+(F*2-count)*12] | 0x1c0 [448] | 0xd8 [216] | g_lobbyErrorDialogSurface | 0 | uVar12=HH |
| 15 | 11 (0xb) | QueueFrontendOverlayRect @0x425660 | HW - 200 [HW-200] | iVar13 + F*-8  [BY -F*8] | 0x1c0 [448] | 0x40 [64] | g_controllerBindingPage_state | 0 | |
| 16 | 0x13 kbd slide-in | QueueFrontendOverlayRect @0x425660 | (HW - 0x368) + F*0x18  [HW-872 +F*24] | (BY - hdrYOff) + -0x40 | hdrW | hdrH | g_currentScreenIndex (header) | 0 | keyboard path |
| 17 | 0x14 kbd press-key | QueueFrontendOverlayRect @0x425660 | HW - 200 [HW-200] | (BY - hdrYOff) + -0x40 | hdrW | hdrH | g_currentScreenIndex (header) | 0 | |
| 18 | 0x14 kbd | MeasureOrCenterFrontendLocalizedString | y=0 [0] | (width=0x1c0) | — | — | SNK_PressKeyTxt | — | |
| 19 | 0x19 kbd label | QueueFrontendOverlayRect @0x425660 | HW - 200 [HW-200] | (BY - hdrYOff) + -0x40 | hdrW | hdrH | g_currentScreenIndex (header) | 0 | |
| 20 | 0x19 kbd | MeasureOrCenterFrontendLocalizedString | y=0 [0] | (width=0x1c0) | — | — | SNK_ControlText[progressIndex*0x10] | — | per-action label |
| 21 | 0x1a kbd capture | QueueFrontendOverlayRect @0x425660 | HW - 200 [HW-200] | (BY - hdrYOff) + -0x40 | hdrW | hdrH | g_currentScreenIndex (header) | 0 | |
| 22 | 0x1a kbd capture | QueueFrontendOverlayRect @0x425660 | HW - 200 [HW-200] | iVar13 [BY] | 0x1c0 [448] | 0x40 [64] | g_controllerBindingPage_state | 0 | |
| 23 | 0x1b kbd slide-out | QueueFrontendOverlayRect @0x425660 | iVar11 + F*-0x18 + 10  [HW-200 -F*24 +10] (iVar11=HW-200 reused) | (BY - hdrYOff) + -0x40 | hdrW | hdrH | g_currentScreenIndex (header) | 0 | iVar11 in this case = HW-0xd2? see note |
| 24 | 0x1b kbd slide-out | QueueFrontendOverlayRect @0x425660 | F*0x10 + 10 + iVar11  [F*16 +10 +iVar11] | F*0x20 + iVar13  [F*32 +BY] | 0x1c0 [448] | 0x40 [64] | g_controllerBindingPage_state | 0 | iVar11=BX (HW-0xd2) in cases 0xb/0x1b per local var; see note |

Notes on iVar11 in slide-out cases (0xb, 0x1b): in case 0xa, `iVar11 = uVar10 - 200` (HW-200). In cases 0xb and 0x1b the local `iVar11` retains the FUNCTION-PROLOGUE value `uVar10 - 0xd2` (= BX = HW-210), since those cases do not re-assign it. Rows 12/13/23/24 therefore use BX (HW-0xd2), NOT HW-200. [UNCERTAIN on exact iVar11 binding in 0xb/0x1b: decompiler reuses the same SSA name `iVar11` for two distinct values (prologue BX vs case-0xa HW-200); from control flow, cases 0xb/0x1b are reached without passing through case 0xa's reassignment in the same invocation, so iVar11=BX. Confirm via listing if byte-exact needed.]

Delegated sub-draw fns followed (ONE level): 
- DrawControlBindingTextWithOkButton (case 0, wheel device &0xff00==0x600) — draws binding text + OK button. Not decompiled (entry addr not resolved in this pass).
- OpenControllerBindingPageNoneHeader (case 0, uVar12 in {0,1,2}) — opens header for no/partial-axis devices. Not decompiled.
Confidence: [CONFIRMED @ 0x0040fe00; helpers @ 0x425de0/0x425660/0x4259d0]. [UNCERTAIN: iVar11 value in cases 0xb/0x1b (BX vs HW-200) — see note; per-button row Y is computed from local_14/local_20 chain, recorded with formula].

---

### Screen 19 @ 0x00418460 — ScreenMusicTestExtras
Mechanism: `HW=canvasW>>1`, `HH=canvasH>>1`, `BX=HW-0xd2`, `BY=HH-0x9f`. F=g_frontendAnimFrameCounter. Gated by g_extrasGalleryCrossFadePhase in case 0.
States/structure: switch(g_frontendInnerState) cases 0,1,2,3,4,5,6,7,8. (2 buttons: SelectTrack, OK.)

| # | State/case | Helper @addr | x (verbatim) [dec] | y (verbatim) [dec] | w | h | tex/label/surface | blit-flag | notes |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 0 init | MeasureOrCenterFrontendLocalizedString | y=0 [0] | (width=0x170) | — | — | local_40 (sprintf "%d - %s" track #) | — | track-number box (g_lobbyErrorDialogSurface) |
| 2 | 0 init | MeasureOrCenterFrontendLocalizedString | y=0 [0] | (width=0x170) | — | — | SNK_NowPlayingTxt | — | now-playing box (g_musicTestSelectedTrackId) |
| 3 | 0 init | MeasureOrCenterFrontendLocalizedString | y=0 [0] | (width=0x170) | — | — | PTR_s_GRAVITY_KILLS[trackIdx] (band name) | — | |
| 4 | 0 init | MeasureOrCenterFrontendLocalizedString | y=0 [0] | (width=0x170) | — | — | PTR_s_FALLING[trackIdx] (song name) | — | |
| 5 | 0 init | CreateFrontendDisplayModeButton @0x425de0 | -0x120 [-288] | 0 | 0xa0 [160] | 0x20 [32] | SNK_SelectTrackButTxt | — | auto-layout |
| 6 | 0 init | CreateFrontendDisplayModeButton @0x425de0 | -0x120 [-288] | 0 | 0x60 [96] | 0x20 [32] | SNK_OkButTxt | — | OK; EscKeyButtonIndex=1 |
| 7 | 3 slide-in | MoveFrontendSpriteRect @0x4259d0 | F*0x10 + -0x266 + BX  [F*16 -614 +BX] | HH - 0x8f [HH-143] | — | — | sprite slot 0 | — | |
| 8 | 3 slide-in | MoveFrontendSpriteRect @0x4259d0 | HW - 0x68 [HW-104] | BY + F*-0x10 + 0x398  [BY -F*16 +920] | — | — | sprite slot 1 (OK) | — | |
| 9 | 3 slide-in | QueueFrontendOverlayRect @0x425660 | HW - 200 [HW-200] | (F*4 + -0xdc - hdrYOff) + BY | hdrW | hdrH | g_currentScreenIndex (header) | 0 | |
| 10 | 4/5 static | QueueFrontendOverlayRect @0x425660 | HW - 200 [HW-200] | (BY - hdrYOff) + -0x40 | hdrW | hdrH | g_currentScreenIndex (header) | 0 | |
| 11 | 4/5 static | QueueFrontendOverlayRect @0x425660 | HW - 0x32 [HW-50] | HH - 0x8f [HH-143] | 0x170 [368] | 0x28 [40] | g_lobbyErrorDialogSurface (track-# box) | 0 | |
| 12 | 4/5 static | QueueFrontendOverlayRect @0x425660 | HW - 0xc [HW-12] | HH - 0x3f [HH-63] | 0x170 [368] | 0x78 [120] | g_musicTestSelectedTrackId (now-playing box) | 0 | |
| 13 | 6 input | QueueFrontendOverlayRect @0x425660 | HW - 200 [HW-200] | (BY - hdrYOff) + -0x40 | hdrW | hdrH | g_currentScreenIndex (header) | 0 | |
| 14 | 6 input | QueueFrontendOverlayRect @0x425660 | HW - 0x32 [HW-50] | HH - 0x8f [HH-143] | 0x170 [368] | 0x28 [40] | g_lobbyErrorDialogSurface | 0 | |
| 15 | 6 input | QueueFrontendOverlayRect @0x425660 | HW - 0xc [HW-12] | HH - 0x3f [HH-63] | 0x170 [368] | 0x78 [120] | g_musicTestSelectedTrackId | 0 | |
| 16 | 6 input (SelectTrack pressed) | MeasureOrCenterFrontendLocalizedString | y=0 [0] | (width=0x170) | — | — | SNK_NowPlayingTxt + band + song | — | re-renders now-playing box on play |
| 17 | 6 input (nav≠0) | MeasureOrCenterFrontendLocalizedString | y=0 [0] | (width=0x170) | — | — | local_40 (sprintf track #) | — | re-renders track-# box on cycle |
| 18 | 7 settle | QueueFrontendOverlayRect @0x425660 | HW - 200 [HW-200] | (BY - hdrYOff) + -0x40 | hdrW | hdrH | g_currentScreenIndex (header) | 0 | |
| 19 | 8 slide-out | QueueFrontendOverlayRect @0x425660 | BX + F*-0x18 + 10  [BX -F*24 +10] | (BY - hdrYOff) + -0x40 | hdrW | hdrH | g_currentScreenIndex (header) | 0 | iVar5=BX |
| 20 | 8 slide-out | MoveFrontendSpriteRect @0x4259d0 | BX + F*-8 + 10  [BX -F*8 +10] | BY + F*-0x30 + 0x10  [BY -F*48 +16] | — | — | sprite slot 0 | — | |
| 21 | 8 slide-out | MoveFrontendSpriteRect @0x4259d0 | (HW - 0x68) + F*6  [HW-104 +F*6] | F*0x30 + 0x128 + BY  [F*48 +296 +BY] | — | — | sprite slot 1 | — | |

Delegated sub-draw fns followed: none (all inline). Case 0 is gated by g_extrasGalleryCrossFadePhase: init only fires when phase < -0xf; phase>0xc0 mirrored, >0x40 clamped to 0x40.
Confidence: [CONFIRMED @ 0x00418460; helpers @ 0x425de0/0x425660/0x4259d0].

---

## Cross-screen notes
- Header label (g_currentScreenIndex surface): w=g_menuHeaderLabelSurfaceWidth, h=g_menuHeaderLabelSurfaceHeight (runtime, set by CreateMenuStringLabelSurface(6)); X almost always HW-200, Y = (BY - g_menuHeaderLabelYOffset) - 0x40 in static states or animated in slide states. Recorded verbatim per row.
- Volume/value-fill widths in ScreenSoundOptions (uVar2, uVar6, iVar5, iVar7, iVar3) are COMPUTED: `(percent * scale) / div`, clamped ≤0xde [222]. Not literal.
- No calls to Copy16BitSurfaceRect @0x004251a0 or DrawFrontendButtonBackground @0x00425b60 appear in any of the 5 functions. DrawFrontendLocalizedStringToSurface @0x00424560 is always called with no x/y operands; its position comes from the preceding MeasureOrCenterFrontendLocalizedString(text, y, width) call (recorded).
- All button creates use negative x (auto-layout flag) with y=0; their actual on-screen X comes from the per-state MoveFrontendSpriteRect rows.
# Frontend Screen Layout Harvest — Part 20 (table indices 20–24)

Source: `TD5_d3d.exe` (image base 0x00400000), Ghidra pool slot TD5_pool10, read_only.
Table `g_frontendScreenFnTable @ 0x004655C4`, 30 LE uint32 fn-ptrs. This slice = indices 20,21,22,23,24.

Helper-callee addresses VERIFIED against `function_callees` (e.g. for 0x00413580):
`QueueFrontendOverlayRect @ 0x00425660` (dst_x,dst_y,src_x,src_y,w,h,color,surface),
`MoveFrontendSpriteRect @ 0x004259d0` (slot,x,y),
`CreateFrontendDisplayModeButton @ 0x00425de0` (label,x,y,w,h,ud) — neg x = auto-layout flag,
`DrawFrontendLocalizedStringToSurface @ 0x00424560` — signature is `void __cdecl ...(void)`: it reads globals, takes NO x/y args, so call sites carry NO recoverable coordinate operands.

Canvas convention shorthand used below:
- `cW = g_frontendCanvasW`, `cH = g_frontendCanvasH`
- `hw = cW>>1` (half-width), `hh = cH>>1` (half-height)

---

### Screen 20 @ 0x0040dfc0 — CarSelectionScreenStateMachine
Mechanism: base anchors `sx = (float)(hw - 0x11a) [hw-282]`, `sy = (float)(hh - 0x9f) [hh-159]`; preview-panel anchors `uVar1 = cW - 0x198 [cW-408]`, `iVar8 = cH - 0x164 [cH-356]`; conditional column `iVar12 = (hw - 0x112) [hw-274]` normally, else `(hw - 0xde) [hw-222]` when `g_postRaceRestartSelectedRace!=0 || g_mainMenuFlowPhase!=0`.
States/structure: switch(g_frontendInnerState) cases 0,1,2,3,4,5,6,7,8,10,0xb,0xc,0xd,0xe,0xf,0x10,0x11,0x12,0x14,0x15,0x16,0x17,0x18,0x19,0x1a. Anim var = `g_frontendAnimFrameCounter`.

| # | State/case | Helper @addr | x (verbatim) [dec] | y (verbatim) [dec] | w | h | tex/label/surface | blit-flag | notes |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 4 | CreateFrontendDisplayModeButton @0x425de0 | -0xa8 [-168] | 0 | 0xa8 [168] | 0x20 [32] | SNK_CarButTxt | - | neg x = auto-layout |
| 2 | 4 | CreateFrontendDisplayModeButton | -0xa8 [-168] | 0 | 0xa8 [168] | 0x20 [32] | SNK_PaintButTxt | - | auto-layout |
| 3 | 4 | CreateFrontendDisplayModeButton | -0xa8 [-168] | 0 | 0xa8 [168] | 0x20 [32] | SNK_ConfigButTxt | - | auto-layout |
| 4 | 4 (gameType!=7) | CreateFrontendDisplayModeButton | -0xa8 [-168] | 0 | 0xa8 [168] | 0x20 [32] | SNK_AutoButTxt | - | auto-layout |
| 5 | 4 (gameType==7) | CreateFrontendDisplayModePreviewButton | -0xa8 [-168] | 0 | 0xa8 [168] | 0x20 [32] | SNK_ManualButTxt | - | auto-layout |
| 6 | 4 | CreateFrontendDisplayModeButton | -0x40 [-64] | 0 | 0x40 [64] | 0x20 [32] | SNK_OkButTxt | - | auto-layout |
| 7 | 4 (no restart & flow==0) | CreateFrontendDisplayModeButton | -0x60 [-96] | 0 | 0x60 [96] | 0x20 [32] | SNK_BackButTxt | - | auto-layout |
| 8 | 2 (anim==(cW-0x20)>>3) | QueueFrontendOverlayRect | cW + anim*-8 + 4 | 0 | 0x18 [24] | 0x198 [408] | g_carSelectionConfirmStateMachine (CarSelBar1.tga) | 0 | slide-in bar (final frame) |
| 9 | 2 (anim==(cW-0x20)>>3) | QueueFrontendOverlayRect | cW + anim*-8 + 4 | 0x198 [408] | 0x50 [80] | 0x38 [56] | g_carSelectionPreviewActive (CarSelCurve.tga) | 0 | final frame |
| 10 | 2 (else) | QueueFrontendOverlayRect | cW + anim*-8 + -4 | 0 | 0x18 [24] | 0x198 [408] | g_carSelectionConfirmStateMachine | 0 | sliding bar |
| 11 | 2 (else) | QueueFrontendOverlayRect | cW + anim*-8 + -4 | 0x198 [408] | 0x50 [80] | 0x38 [56] | g_carSelectionPreviewActive | 0 | sliding |
| 12 | 2 | QueueFrontendOverlayRect | iVar8 (=anim*8-0x214 if anim*8<0x215 else 0) | 0x2d [45] | 0x214 [532] | 0x24 [36] | g_carSelectionPaintIndex (CarSelTopBar.tga) | 0 | top bar slide |
| 13 | 5 | MoveFrontendSpriteRect slot 0 | sx + anim*-0x20 + 0x308 [+776] | anim*0x10 + -0x128 + sy [-296] | - | - | sprite 0 | - | button intro slide |
| 14 | 5 | MoveFrontendSpriteRect slot 1 | sx + anim*-0x20 + 0x308 | (hh - 0xdf) [hh-223] + anim*8 | - | - | sprite 1 | - | |
| 15 | 5 | MoveFrontendSpriteRect slot 2 | sx + anim*-0x20 + 0x308 | hh + 9 | - | - | sprite 2 | - | |
| 16 | 5 | MoveFrontendSpriteRect slot 3 | sx + anim*-0x20 + 0x308 | sy + anim*-8 + 400 [0x190] | - | - | sprite 3 | - | |
| 17 | 5 | MoveFrontendSpriteRect slot 4 | iVar12 + anim*-0x20 + 0x300 [+768] | sy + anim*-0x10 + 0x278 [+632] | - | - | sprite 4 | - | uses iVar12 column |
| 18 | 5 (no restart & flow==0) | MoveFrontendSpriteRect slot 5 | sx + anim*-0x20 + 0x350 [+848] | sy + anim*-0x10 + 0x278 | - | - | sprite 5 | - | back button |
| 19 | 5 | QueueFrontendOverlayRect | hw - 0x110 [hw-272] | anim*5 - g_menuHeaderLabelYOffset + -0xb8 [-184] + sy | g_menuHeaderLabelSurfaceWidth | g_menuHeaderLabelSurfaceHeight | g_currentScreenIndex (title label) | 0 | header slide |
| 20 | 6 | QueueFrontendOverlayRect | hw - 0x110 [hw-272] | (sy - g_menuHeaderLabelYOffset) + -0x40 [-64] | hdrW | hdrH | g_currentScreenIndex | 0 | header settled |
| 21 | 8 | BlitFrontendCachedRect | (int)sx | hh - 0x97 [hh-151] | cW - (int)sx | 0x20 [32] | (cached primary) | - | name strip |
| 22 | 10 | FillPrimaryFrontendRect | 0x5c [92] (fill color) | uVar1 [cW-408] (=x) | iVar8 [cH-356] (=y) | 0x198 [408] | 300 [0x12c] | primary | - | preview panel clear |
| 23 | 0xc | BlitSecondaryFrontendRectToPrimary | (int)sx | hh-0x97 [hh-151] (iVar8 local reused) | cW-(int)sx | 0x40 [64] | secondary | - | |
| 24 | 0xc | DrawFrontendLocalizedStringPrimary | uVar1 (centered, from MeasureOrCenter sx..cW) | hh-0x97 [hh-151] | - | - | local_80 ("%s %s") | - | car name string |
| 25 | 0xc (gameType==2 or locked) | DrawFrontendLocalizedStringPrimary | hw - 0xea [hw-234] | hh - 0x77 [hh-119] | - | - | Beast/Beauty/Locked txt | - | |
| 26 | 0xc | BlitFrontendCachedRect | (int)sx | hh-0x97 [hh-151] | cW-(int)sx | 0x40 [64] | cached | - | |
| 27 | 0xf | BlitSecondaryFrontendRectToPrimary | (int)sx | hh-0x97 [hh-151] | cW-(int)sx | 0x20 [32] | secondary | - | |
| 28 | 0xf | DrawFrontendLocalizedStringPrimary | uVar2 (centered sx..cW) | hh-0x97 [hh-151] | - | - | local_80 ("%s %s") | - | |
| 29 | 0xf | BlitFrontendCachedRect | (int)sx | hh-0x97 [hh-151] | cW-(int)sx | 0x20 [32] | cached | - | |
| 30 | 0xf / 0x11 | PresentSecondaryFrontendRect / PresentPrimaryFrontendRect | uVar1 [cW-408] | iVar8 [cH-356] | 0x198 [408] | 300 [0x12c] | preview panel | - | spec-stat panel |
| 31 | 0xf config-header loop | DrawFrontendSmallFontStringToSurface | uVar1 [cW-408] | local_94 (=iVar8=cH-356, +=0xc per row, rows iVar9 0..0x10 step4) | - | - | SNK_Config_Hdrs[] | - | header column |
| 32 | 0xf 2nd hdr loop | DrawFrontendSmallFontStringToSurface | uVar1 | local_94 (=cH-0x11c [cH-284], +=0xc, iVar3 0x10..0x20 step4) | - | - | SNK_Config_Hdrs[0x10..] | - | |
| 33 | 0xf | DrawFrontendSmallFontStringToSurface | uVar1 | cH - 0xe0 [cH-224] | - | - | SNK_Config_Hdrs[0x20] | - | single row |
| 34 | 0xf 3rd hdr loop | DrawFrontendSmallFontStringToSurface | uVar1 | local_94 (=cH-200 [cH-0xc8], +=0xc, 0x24..0x30 step4) | - | - | SNK_Config_Hdrs[0x24..] | - | |
| 35 | 0xf 4th hdr loop | DrawFrontendSmallFontStringToSurface | uVar1 | local_94 (=cH-0x98 [cH-152], +=0xc, 0x30..0x38 step4) | - | - | SNK_Config_Hdrs[0x30..] | - | |
| 36 | 0xf value col loop1 | DrawFrontendSmallFontStringToSurface | iVar12+0x10+uVar1 (=maxhdrW+0x10+cW-408) | local_94 (=cH-356, +=0xc, 0..4) | - | - | car-config scratch strings | - | values aligned after headers |
| 37 | 0xf | DrawFrontendSmallFontStringToSurface | uVar1 (=iVar12+0x10+uVar1) | cH - 0x134 [cH-308] | - | - | &DAT_0049b7bc scratch | - | |
| 38 | 0xf value col loop2 | DrawFrontendSmallFontStringToSurface | uVar1 | iVar9 (=cH-0x11c [cH-284], +=0xc, 4..8) | - | - | track-name scratch | - | |
| 39 | 0xf | DrawFrontendSmallFontStringToSurface | uVar1 | cH - 0xe0 [cH-224] | - | - | option-label scratch | - | |
| 40 | 0xf value col loop3 | DrawFrontendSmallFontStringToSurface | uVar1 | iVar3 (=cH-200, +=0xc, 9..0xc) | - | - | track-name scratch | - | |
| 41 | 0xf value col loop4 | DrawFrontendSmallFontStringToSurface | uVar1 | iVar4 (=cH-0x98 [cH-152], +=0xc, 0xc..0xe) | - | - | track-name scratch | - | |
| 42 | 0x11 value loop | DrawFrontendSmallFontStringToSurface | uVar7 (centered uVar1..uVar2) | iVar8 (=cH-356, +=0xc, 0..0x28) | - | - | SNK_Info_Values[] | - | |
| 43 | 0x18 | MoveFrontendSpriteRect slot 0 | anim*0x20 + 8 + (int)sx | sy + anim*-0x10 + 0x58 [+88] | - | - | sprite 0 | - | exit slide |
| 44 | 0x18 | MoveFrontendSpriteRect slot 1 | anim*0x20 + 8 + (int)sx | sy + anim*-8 + 0x80 [+128] | - | - | sprite 1 | - | |
| 45 | 0x18 | MoveFrontendSpriteRect slot 2 | anim*0x20 + 8 + (int)sx | hh + 9 | - | - | sprite 2 | - | |
| 46 | 0x18 | MoveFrontendSpriteRect slot 3 | anim*0x20 + 8 + (int)sx | hh + 0x31 [+49] + anim*8 | - | - | sprite 3 | - | |
| 47 | 0x18 | MoveFrontendSpriteRect slot 4 | anim*0x20 + iVar12 | anim*0x10 + 0xf8 [+248] + sy | - | - | sprite 4 | - | |
| 48 | 0x18 (no restart & flow==0) | MoveFrontendSpriteRect slot 5 | anim*0x20 + 0x50 [+80] + (int)sx | anim*0x10 + 0xf8 + sy | - | - | sprite 5 | - | |
| 49 | 0x14 / 0x10 | FillPrimaryFrontendRect (case0x14) | 0x5c [92] (color) | uVar1 [cW-408] | iVar8 [cH-356] | 0x198 [408] | 300 | primary | - | panel clear |
| 50 | 0x19 | BlitFrontendCachedRect | 0 | cH + anim*-0x18 [-24] | cW | 0x18/0x30 [24/48] | cached | - | row wipe-down |

Delegated sub-draw fns followed: `DrawCarSelectionPreviewOverlay @ 0x0040ddc0` (called as `(sx,sy)` in states 7,8,0xb,0xc,0xd,0xe,0xf,0x10,0x11,0x12,0x14,0x15,0x16,0x17,0x18) — summary below.
Other delegates NOT followed: none beyond PreviewOverlay; `MeasureOrCenterFrontendString`/`DrawFrontendLocalizedStringPrimary`/`DrawFrontendSmallFontStringToSurface` are leaf text helpers, x/y are passed verbatim from the call sites above.

#### Sub-draw: DrawCarSelectionPreviewOverlay @ 0x0040ddc0  (params sx,sy; reads stack vars in_stack_0xc=x, _0x10=y, _0x14=state-tag)
| # | branch | Helper @addr | x (verbatim) [dec] | y (verbatim) [dec] | w | h | tex/surface | color | notes |
|---|---|---|---|---|---|---|---|---|---|
| S1 | tag==0x18 | QueueFrontendOverlayRect @0x425660 | (int)sx + 10 + anim*0x28 | (sy - g_menuHeaderLabelYOffset) + -0x40 [-64] | hdrW | hdrH | g_currentScreenIndex | 0 | header (slide) |
| S2 | else | QueueFrontendOverlayRect | (int)sx + 10 | (sy - g_menuHeaderLabelYOffset) + -0x40 | hdrW | hdrH | g_currentScreenIndex | 0 | header (settled) |
| S3 | frameIdx==0, accum!=0, tag==0 | QueueFrontendOverlayRect | cW + -0x198 [cW-408] | cH + -0x164 [cH-356] | 0x198 [408] | 0x118 [280] | g_carSelectionFrameAccumulator (CarPic) | 0x5a [90] | car preview panel |
| S4 | frameIdx==0, accum!=0, tag==0xb | QueueFrontendOverlayRect | anim*0x20 + -0x198 + cW | cH + -0x164 | 0x198 [408] | 0x118 [280] | g_carSelectionFrameAccumulator | 0x5a [90] | slide-in |
| S5 | frameIdx==0, accum!=0, tag==0xe | QueueFrontendOverlayRect | cW + anim*-0x40 + 0x4a8 [+1192] | cH + -0x164 | 0x198 [408] | 0x118 [280] | g_carSelectionFrameAccumulator | 0x5a [90] | slide-out |
| S6 | frameIdx!=0, tag==0 | QueueFrontendOverlayRect | in_stack_0xc (=x arg) | in_stack_0x10 (=y arg) | 0x198 [408] | 300 [0x12c] | g_secondaryWorkSurface | 0xffffff | stat panel |
| S7 | frameIdx!=0, tag==0xb | QueueFrontendOverlayRect | anim*0x20 + in_stack_0xc | in_stack_0x10 | 0x198 [408] | 300 [0x12c] | g_secondaryWorkSurface | 0xffffff | stat panel slide |

Confidence: [CONFIRMED @ 0x0040dfc0 main + 0x0040ddc0 sub; callee 0x00425de0/0x004259d0/0x00425660 verified]. [UNCERTAIN: DrawFrontendLocalizedStringPrimary/SmallFont x/y are runtime-computed (MeasureOrCenter return, scratch-table row indices) — literal anchor constants captured; the exact pixel depends on string-measure return at runtime.]

---

### Screen 21 @ 0x00427630 — TrackSelectionScreenStateMachine
Mechanism: `iVar7 = hw - 0xd2 [hw-210]`, `iVar5 = hh - 0x9f [hh-159]`; conditional `iVar8 = hw - 0x88 [hw-136]` when `g_mainMenuButtonHint_PROVISIONAL==2`, else `iVar8 = hw - 200 [hw-200]` (0xc8).
States/structure: switch cases 0,1,2,3,4,5,6,7,8. Anim var = `g_frontendAnimFrameCounter`.

| # | State/case | Helper @addr | x (verbatim) [dec] | y (verbatim) [dec] | w | h | tex/label/surface | blit-flag | notes |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 0 | CreateFrontendDisplayModeButton @0x425de0 | -0xe0 [-224] | 0 | 0xe0 [224] | 0x20 [32] | SNK_TrackButTxt | - | auto-layout |
| 2 | 0 | CreateFrontendDisplayModeButton | -0xe0 [-224] | 0 | 0xe0 [224] | 0x20 [32] | SNK_ForwardsButTxt | - | auto-layout |
| 3 | 0 | CreateFrontendDisplayModeButton | -0xe0 [-224] | 0 | 0x60 [96] | 0x20 [32] | SNK_OkButTxt | - | auto-layout |
| 4 | 0 (hint!=2) | CreateFrontendDisplayModeButton | -0xe0 [-224] | 0 | 0x70 [112] | 0x20 [32] | SNK_BackButTxt | - | auto-layout |
| 5 | 3 | MoveFrontendSpriteRect slot 2 | (anim + 0xfffffd9 [-39]) *0x10 + iVar8 | hh + 0x89 [+137] | - | - | sprite 2 | - | intro slide |
| 6 | 3 (hint!=2) | MoveFrontendSpriteRect slot 3 | iVar7 + anim*-0x10 + 0x2ea [+746] | hh + 0x89 | - | - | sprite 3 | - | |
| 7 | 3 | MoveFrontendSpriteRect slot 0 | anim*0x10 + -0x266 [-614] + iVar7 | hh - 0x8f [hh-143] | - | - | sprite 0 | - | |
| 8 | 3 | MoveFrontendSpriteRect slot 1 | iVar8b (=iVar7+anim*-0x10+0x27a [+634] if track unlocked, else -0xe0 [-224]) | hh - 0x5f [hh-95] | - | - | sprite 1 | - | direction sprite |
| 9 | 3 | QueueFrontendOverlayRect @0x425660 | hw - 200 [hw-200] | (anim*4 + -0xdc [-220]) - g_menuHeaderLabelYOffset + iVar5 | hdrW | hdrH | g_currentScreenIndex | 0 | header slide |
| 10 | 4 | QueueFrontendOverlayRect | iVar8 (=hw-200) | (iVar5 - g_menuHeaderLabelYOffset) + -0x40 [-64] | hdrW | hdrH | g_currentScreenIndex | 0 | header |
| 11 | 4 | QueueFrontendOverlayRect | hw + 0x5c [+92] | hh - 0x69 [hh-105] | 0x98 [152] | 0xe0 [224] | DAT_004a2c94 (track preview tga) | 0 | track thumbnail |
| 12 | 4 | QueueFrontendOverlayRect | hw + 0x18 [+24] | iVar5 [hh-159] | 0x128 [296] | 0x40 [64] | g_lobbyErrorDialogSurface | 0 | name plate (top) |
| 13 | 4 | QueueFrontendOverlayRect | iVar8 (=hw-200) | hh - 0x2f [hh-47] | 0xe0 [224] | 0x78 [120] | g_lobbyErrorDialogSurface | 0 | src_y=0x40 [64] desc box |
| 14 | 4 (nav, recompute) | MoveFrontendSpriteRect slot 1 | iVar8b (unlocked: kept, else -0xe0) | hh - 0x5f [hh-95] | - | - | sprite 1 | - | direction toggle reposition |
| 15 | 5 | QueueFrontendOverlayRect | hw - 200 [hw-200] | (iVar5 - g_menuHeaderLabelYOffset) + -0x40 [-64] | hdrW | hdrH | g_currentScreenIndex | 0 | header |
| 16 | 5 | QueueFrontendOverlayRect | hw - 200 [hw-200] | hh - 0x2f [hh-47] | 0xe0 [224] | 0x78 [120] | g_lobbyErrorDialogSurface | 0 | src_y=0x40 desc box |
| 17 | 7 | MoveFrontendSpriteRect slot 2 | iVar8 (=hw-200) | anim*0x10 + 0x128 [+296] + iVar5 | - | - | sprite 2 | - | exit slide |
| 18 | 7 | QueueFrontendOverlayRect | hw + 0x18 [+24] | anim*0x20 + iVar5 | 0x128 [296] | 0x40 [64] | g_lobbyErrorDialogSurface | 0 | name plate exit |
| 19 | 7 | QueueFrontendOverlayRect | iVar7 + anim*-0x20 + 10 | hh - 0x2f [hh-47] | 0xe0 [224] | 0x78 [120] | g_lobbyErrorDialogSurface | 0 | src_y=0x40 desc box exit |
| 20 | 7 (hint!=2) | MoveFrontendSpriteRect slot 3 | hw - 0x58 [hw-88] | anim*0x10 + 0x128 [+296] + iVar5 | - | - | sprite 3 | - | |
| 21 | 7 | MoveFrontendSpriteRect slot 0 | iVar8 (=hw-200) | iVar5 + anim*-0x10 + 0x10 [+16] | - | - | sprite 0 | - | |
| 22 | 7 | MoveFrontendSpriteRect slot 1 | iVar8b (unlocked: kept w/ y=iVar5+anim*-0x10+0x40; else x=-0xe0, y=hh-0x5f) | (see x cell) | - | - | sprite 1 | - | conditional |
| 23 | 7 | QueueFrontendOverlayRect | (hw - 200) + anim*0x18 | (iVar5 - g_menuHeaderLabelYOffset) + -0x40 [-64] | hdrW | hdrH | g_currentScreenIndex | 0 | header exit slide |
| 24 | 7 | QueueFrontendOverlayRect | hw + 0x5c [+92] + anim*8 | iVar5 + anim*-0x18 + 0x36 [+54] | 0x98 [152] | 0xe0 [224] | DAT_004a2c94 (thumbnail) | 0 | thumbnail exit |
| 25 | 8 | QueueFrontendOverlayRect | hw - 200 [hw-200] | (iVar5 - g_menuHeaderLabelYOffset) + -0x40 [-64] | hdrW | hdrH | g_currentScreenIndex | 0 | header |
| 26 | 8 | QueueFrontendOverlayRect | iVar7 + anim*-0x10 + 0x22e [+558] | hh - 0x69 [hh-105] | 0x98 [152] | 0xe0 [224] | DAT_004a2c94 (thumbnail) | 0 | thumbnail slide-in |
| 27 | 8 | QueueFrontendOverlayRect | hw + 0x18 [+24] | (anim + -0x10 [-16]) *0x10 + iVar5 | 0x128 [296] | 0x40 [64] | g_lobbyErrorDialogSurface | 0 | name plate |
| 28 | 8 | QueueFrontendOverlayRect | hw - 200 [hw-200] | hh - 0x2f [hh-47] | 0xe0 [224] | 0x78 [120] | g_lobbyErrorDialogSurface | 0 | src_y=0x40 desc box |

Delegated sub-draw fns followed: none (track name text drawn via `MeasureOrCenterFrontendLocalizedString` + arg-less `DrawFrontendLocalizedStringToSurface @ 0x00424560` in state 5 — no x/y operands at call site).
Confidence: [CONFIRMED @ 0x00427630]. [UNCERTAIN: in row 8/14/22 the slot-1 sprite x/y depend on a runtime branch (track unlocked vs locked) — both branches captured.]

---

### Screen 22 @ 0x00417d50 — ScreenExtrasGallery (credits scroll)
Mechanism: NO canvas-center math. Uses literal absolute coordinates. Scroll position driven by `g_frontendAnimFrameCounter` (0..0x27f) modulo 0x280; the two 0x140-tall halves wrap. Per-line/per-mugshot positions are at fixed x=0xcc / x=0/0x140.
States/structure: switch cases 0,1,2,3,4,5,6,7. Drawing happens only in case 7.

| # | State/case | Helper @addr | x (verbatim) [dec] | y (verbatim) [dec] | w | h | tex/surface | blit-flag | notes |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 7 (anim<0x140) | QueueFrontendOverlayRect @0x425660 | 0xcc [204] | 0x60 [96] | 0x140 [320] | 0x140 - anim | g_secondaryWorkSurface | 0 | src_x=0, src_y=anim; top half |
| 2 | 7 (anim<0x140, anim!=0) | QueueFrontendOverlayRect | 0xcc [204] | 0x1a0 - anim [416-anim] | 0x140 [320] | anim | g_secondaryWorkSurface | 0 | src_x=0x140, src_y=0; wrap bottom |
| 3 | 7 (anim>=0x140) | QueueFrontendOverlayRect | 0xcc [204] | 0x60 [96] | 0x140 [320] | 0x280 - anim | g_secondaryWorkSurface | 0 | src_x=0x140, src_y=anim-0x140 |
| 4 | 7 (anim>=0x140, anim!=0x140) | QueueFrontendOverlayRect | 0xcc [204] | 0x2e0 - anim [736-anim] | 0x140 [320] | anim - 0x140 | g_secondaryWorkSurface | 0 | src_x=0, src_y=0; wrap |
| 5 | 7 mugshot row (type==0x23 '#', anim<0x140) | FillSurfaceRectWithColor | 0 (color) | 0x140 [320] | anim (y) | 0x140 [320] (w) | 0x20 [32] (h) | secondary | - | clears row before mugshot |
| 6 | 7 mugshot (anim<0x140) | surface vtbl +0x1c (Copy16BitSurfaceRect-equiv) | 0x140 [320] (dst_x) | anim (dst_y) | local_10 rect {x=0,y=assetHandle*0x20,x2=0x140,y2=+0x20} | - | mugshot tga (DAT_004961dc[type1*4]) | 0x11 [keyed] | mugshot blit |
| 7 | 7 mugshot (anim>=0x140) | FillSurfaceRectWithColor | 0 (color) | 0 (y) | anim-0x140 (x?) | 0x140 [320] | 0x20 [32] | secondary | - | wrap-half clear |
| 8 | 7 mugshot (anim>=0x140) | surface vtbl +0x1c | 0 (dst_x) | anim-0x140 (dst_y) | local_10 rect {0,assetHandle*0x20,0x140,+0x20} | - | mugshot tga | 0x11 [keyed] | wrap mugshot blit |
| 9 | 7 text row (type!=0x23, anim<0x140) | FillSurfaceRectWithColor | 0 (color) | 0x140 [320] | anim (y) | 0x140 [320] | 0x20 [32] | secondary | - | clears row |
| 10 | 7 text row (anim<0x140) | MeasureOrCenterFrontendLocalizedString + DrawFrontendLocalizedStringToSurface @0x424560 | (centered in span 0x140..0x280) | (runtime) | - | - | SNK_CreditsText[page*0x18] | - | credit line; x/y not literal at call site |
| 11 | 7 text row (anim>=0x140) | FillSurfaceRectWithColor | 0 (color) | 0 (y) | anim-0x140 | 0x140 [320] | 0x20 [32] | secondary | - | wrap clear |
| 12 | 7 text row (anim>=0x140) | MeasureOrCenterFrontendLocalizedString + DrawFrontendLocalizedStringToSurface | (centered span 0..0x140) | (runtime) | - | - | SNK_CreditsText[page*0x18] | - | wrap line |

Delegated sub-draw fns followed: none — `DrawFrontendLocalizedStringToSurface @ 0x00424560` (arg-less, reads globals set by MeasureOrCenter) is a leaf; its dst position is the MeasureOrCenter span argument (0x140..0x280 or 0..0x140), not a literal call-site operand.
Confidence: [CONFIRMED @ 0x00417d50]. [UNCERTAIN: FillSurfaceRectWithColor / vtbl-call (0x1c) param ordering inferred from decompiler arg list (color,x,y,w,h pattern); the credit-text final pixel y is anim-driven, not a fixed constant.]

---

### Screen 23 @ 0x00413580 — ScreenPostRaceHighScoreTable
Mechanism: `iVar3 = hw - 0xd2 [hw-210]`, `iVar5 = hh - 0x9f [hh-159]`; sprite Y references use `uVar4 = hh` directly.
States/structure: switch cases 0,1,2,3,4,5,6,7,8. Anim var = `g_frontendAnimFrameCounter`.

| # | State/case | Helper @addr | x (verbatim) [dec] | y (verbatim) [dec] | w | h | tex/label/surface | blit-flag | notes |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 0 | CreateFrontendDisplayModeButton @0x425de0 | -0x208 [-520] | 0 | 0x208 [520] | 0x20 [32] | (NULL label) | - | auto-layout |
| 2 | 0 | CreateFrontendDisplayModeButton | -0x130 [-304] | 0 | 0x60 [96] | 0x20 [32] | SNK_OkButTxt | - | auto-layout |
| 3 | 0 | CreateTrackedFrontendSurface / BltColorFillToSurface | - | - | 0x208 [520] | 0x90 [144] | g_lobbyErrorDialogSurface | - | dialog scratch surface |
| 4 | 3 | MoveFrontendSpriteRect slot 0 | anim*0x10 + -0x26b [-619] + iVar3 | hh - 0x8f [hh-143] | - | - | sprite 0 | - | intro slide |
| 5 | 3 | MoveFrontendSpriteRect slot 1 | anim*0x10 + -0x26b [-619] + iVar3 | hh + 0x89 [+137] | - | - | sprite 1 | - | |
| 6 | 3 | QueueFrontendOverlayRect @0x425660 | hw - 200 [hw-200] | (anim*4 + -0xdc [-220]) - g_menuHeaderLabelYOffset + iVar5 | hdrW | hdrH | g_currentScreenIndex | 0 | header slide |
| 7 | 3 | QueueFrontendOverlayRect | iVar3 + anim*-0x30 + 0x755 [+1877] | hh - 0x3f [hh-63] | 0x208 [520] | 0x90 [144] | g_lobbyErrorDialogSurface | 0 | table panel slide |
| 8 | 4,5 | QueueFrontendOverlayRect | hw - 200 [hw-200] | (iVar5 - g_menuHeaderLabelYOffset) + -0x40 [-64] | hdrW | hdrH | g_currentScreenIndex | 0 | header settled |
| 9 | 4,5 | QueueFrontendOverlayRect | hw - 0xcd [hw-205] | hh - 0x3f [hh-63] | 0x208 [520] | 0x90 [144] | g_lobbyErrorDialogSurface | 0 | table panel |
| 10 | 6 | QueueFrontendOverlayRect | hw - 200 [hw-200] | (iVar5 - g_menuHeaderLabelYOffset) + -0x40 [-64] | hdrW | hdrH | g_currentScreenIndex | 0 | header |
| 11 | 6 | QueueFrontendOverlayRect | hw - 0xcd [hw-205] | hh - 0x3f [hh-63] | 0x208 [520] | 0x90 [144] | g_lobbyErrorDialogSurface | 0 | table panel |
| 12 | 7 | QueueFrontendOverlayRect | hw - 200 [hw-200] | (iVar5 - g_menuHeaderLabelYOffset) + -0x40 [-64] | hdrW | hdrH | g_currentScreenIndex | 0 | header |
| 13 | 7 | QueueFrontendOverlayRect | hw - 0xcd [hw-205] | hh - 0x3f [hh-63] | 0x208 [520] | 0x90 [144] | g_lobbyErrorDialogSurface | 0 | table panel |
| 14 | 8 | QueueFrontendOverlayRect | iVar3 + anim*-0x18 + 10 [+10] | (iVar5 - g_menuHeaderLabelYOffset) + -0x40 [-64] | hdrW | hdrH | g_currentScreenIndex | 0 | header exit slide |
| 15 | 8 | MoveFrontendSpriteRect slot 0 | anim*0x30 + 0x6a [+106] + iVar3 | hh - 0x8f [hh-143] | - | - | sprite 0 | - | exit |
| 16 | 8 | MoveFrontendSpriteRect slot 1 | (hw - 0x68 [hw-104]) + anim*6 | anim*0x30 + 0x128 [+296] + iVar5 | - | - | sprite 1 | - | exit |
| 17 | 8 | QueueFrontendOverlayRect | iVar3 + anim*-0x38 + 5 [+5] | hh - 0x3f [hh-63] | 0x208 [520] | 0x90 [144] | g_lobbyErrorDialogSurface | 0 | table panel exit |

Delegated sub-draw fns followed: `DrawPostRaceHighScoreEntry @ 0x00413010` (called as `(0)` in cases 1/2, and `(g_postRaceRacerCardIndex)` in case 6/LAB_00413921) — fills the g_lobbyErrorDialogSurface table content (0x208x0x90). NOT decompiled per one-level budget already spent on CarSelection sub-draw; it writes into the dialog surface blitted by rows 7/9/11/13/17. [UNCERTAIN: its internal row positions not harvested.]
Confidence: [CONFIRMED @ 0x00413580].

---

### Screen 24 @ 0x00422480 — RunRaceResultsScreen
Mechanism: `iVar4 = hw - 0xd2 [hw-210]`, `iVar6 = hh - 0x9f [hh-159]`; sprite Y refs use `uVar5 = hh` directly.
States/structure: switch cases 0,1,2,3,4,5,6,7,8,9,0xa(10),0xb,0xc,0xd,0xe,0xf,0x10,0x11,0x12,0x13,0x14,0x15. Anim var = `g_frontendAnimFrameCounter`. Result-panel surface = g_lobbyErrorDialogSurface (0x198 x 0x188).

| # | State/case | Helper @addr | x (verbatim) [dec] | y (verbatim) [dec] | w | h | tex/label/surface | blit-flag | notes |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 0 | CreateTrackedFrontendSurface / BltColorFillToSurface | - | - | 0x198 [408] | 0x188 [392] | g_lobbyErrorDialogSurface | - | results panel scratch |
| 2 | 0 | CreateFrontendDisplayModeButton @0x425de0 | -0x208 [-520] | 0 | 0x208 [520] | 0x20 [32] | (NULL label) | - | auto-layout |
| 3 | 0 | CreateFrontendDisplayModeButton | -0x208 [-520] | 0 | 0x60 [96] | 0x20 [32] | SNK_OkButTxt | - | auto-layout |
| 4 | 3 | MoveFrontendSpriteRect slot 0 | (hw - 0x6e5 [hw-1765]) + anim*0x28 | hh - 0x8f [hh-143] | - | - | sprite 0 | - | intro slide |
| 5 | 3 | MoveFrontendSpriteRect slot 1 | iVar4 + anim*-0x28 + 0x61d [+1565] | hh + 0x89 [+137] | - | - | sprite 1 | - | |
| 6 | 3 | QueueFrontendOverlayRect @0x425660 | hw - 200 [hw-200] | (anim*4 + -0xdc [-220]) - g_menuHeaderLabelYOffset + iVar6 | hdrW | hdrH | g_currentScreenIndex | 0 | header slide |
| 7 | 4,5 | QueueFrontendOverlayRect | hw - 200 [hw-200] | (iVar6 - g_menuHeaderLabelYOffset) + -0x40 [-64] | hdrW | hdrH | g_currentScreenIndex | 0 | header |
| 8 | 4,5 | QueueFrontendOverlayRect | hw - 0xa8 [hw-168] | iVar6 [hh-159] | 0x198 [408] | 0x188 [392] | g_lobbyErrorDialogSurface | 0 | results panel |
| 9 | 6 | QueueFrontendOverlayRect | hw - 200 [hw-200] | (iVar6 - g_menuHeaderLabelYOffset) + -0x40 [-64] | hdrW | hdrH | g_currentScreenIndex | 0 | header |
| 10 | 6 | QueueFrontendOverlayRect | hw - 0xa8 [hw-168] | iVar6 [hh-159] | 0x198 [408] | 0x188 [392] | g_lobbyErrorDialogSurface | 0 | results panel |
| 11 | 7 | QueueFrontendOverlayRect | hw - 200 [hw-200] | (iVar6 - g_menuHeaderLabelYOffset) + -0x40 [-64] | hdrW | hdrH | g_currentScreenIndex | 0 | header |
| 12 | 7 | QueueFrontendOverlayRect | anim*0x20 + 0x2a [+42] + iVar4 | iVar6 [hh-159] | 0x198 [408] | 0x188 [392] | g_lobbyErrorDialogSurface | 0 | panel slide (prev racer) |
| 13 | 8 | QueueFrontendOverlayRect | hw - 200 [hw-200] | (iVar6 - g_menuHeaderLabelYOffset) + -0x40 [-64] | hdrW | hdrH | g_currentScreenIndex | 0 | header |
| 14 | 8 | QueueFrontendOverlayRect | anim*0x20 + -0x1f6 [-502] + iVar4 | iVar6 [hh-159] | 0x198 [408] | 0x188 [392] | g_lobbyErrorDialogSurface | 0 | panel slide |
| 15 | 9 | QueueFrontendOverlayRect | hw - 200 [hw-200] | (iVar6 - g_menuHeaderLabelYOffset) + -0x40 [-64] | hdrW | hdrH | g_currentScreenIndex | 0 | header |
| 16 | 9 | QueueFrontendOverlayRect | iVar4 + anim*-0x20 + 0x2a [+42] | iVar6 [hh-159] | 0x198 [408] | 0x188 [392] | g_lobbyErrorDialogSurface | 0 | panel slide (next racer) |
| 17 | 10 (0xa) | QueueFrontendOverlayRect | hw - 200 [hw-200] | (iVar6 - g_menuHeaderLabelYOffset) + -0x40 [-64] | hdrW | hdrH | g_currentScreenIndex | 0 | header |
| 18 | 10 (0xa) | QueueFrontendOverlayRect | iVar4 + anim*-0x20 + 0x24a [+586] | iVar6 [hh-159] | 0x198 [408] | 0x188 [392] | g_lobbyErrorDialogSurface | 0 | panel slide |
| 19 | 0xb | QueueFrontendOverlayRect | iVar4 + anim*-0x20 + 10 [+10] | (iVar6 - g_menuHeaderLabelYOffset) + -0x40 [-64] | hdrW | hdrH | g_currentScreenIndex | 0 | header exit |
| 20 | 0xb | QueueFrontendOverlayRect | anim*0x30 + 0x2a [+42] + iVar4 | iVar6 [hh-159] | 0x198 [408] | 0x188 [392] | g_lobbyErrorDialogSurface | 0 | panel exit |
| 21 | 0xb | MoveFrontendSpriteRect slot 0 | hw - 0xcd [hw-205] | iVar6 + anim*-0x20 + 0x10 [+16] | - | - | sprite 0 | - | button exit |
| 22 | 0xb | MoveFrontendSpriteRect slot 1 | hw - 0xcd [hw-205] | hh + 0x89 [+137] + anim*8 | - | - | sprite 1 | - | |
| 23 | 0xe | MoveFrontendSpriteRect slot 0 | (hw - 0x3c8 [hw-968]) + anim*0x18 | hh - 0x8f [hh-143] | - | - | sprite 0 | - | post-race menu slide |
| 24 | 0xe | MoveFrontendSpriteRect slot 1 | iVar4 + anim*-0x18 + 0x30a [+778] | hh - 0x5f [hh-95] | - | - | sprite 1 | - | |
| 25 | 0xe | MoveFrontendSpriteRect slot 2 | (hw - 0x3c8 [hw-968]) + anim*0x18 | hh - 0x2f [hh-47] | - | - | sprite 2 | - | |
| 26 | 0xe | MoveFrontendSpriteRect slot 3 | iVar4 + anim*-0x18 + 0x30a [+778] | hh + 1 [+1] | - | - | sprite 3 | - | |
| 27 | 0xe | MoveFrontendSpriteRect slot 4 | (hw - 0x3c8 [hw-968]) + anim*0x18 | hh + 0x31 [+49] | - | - | sprite 4 | - | |
| 28 | 0x10 | MoveFrontendSpriteRect slot 0 | iVar4 + anim*-0x18 + 10 [+10] | hh - 0x8f [hh-143] | - | - | sprite 0 | - | menu exit slide |
| 29 | 0x10 | MoveFrontendSpriteRect slot 1 | (hw - 200) + anim*0x18 | hh - 0x5f [hh-95] | - | - | sprite 1 | - | |
| 30 | 0x10 | MoveFrontendSpriteRect slot 2 | iVar4 + anim*-0x18 + 10 [+10] | hh - 0x2f [hh-47] | - | - | sprite 2 | - | |
| 31 | 0x10 | MoveFrontendSpriteRect slot 3 | (hw - 200) + anim*0x18 | hh + 1 [+1] | - | - | sprite 3 | - | |
| 32 | 0x10 | MoveFrontendSpriteRect slot 4 | iVar4 + anim*-0x18 + 10 [+10] | hh + 0x31 [+49] | - | - | sprite 4 | - | |
| 33 | 0x11 | CreateFrontendDisplayModeButton | -0x120 [-288] | 0 | 0x120 [288] | 0x20 [32] | SNK_BlockSavedOK / SNK_FailedToSave | - | auto-layout |
| 34 | 0x11 | CreateFrontendDisplayModeButton | -0x120 [-288] | 0 | 0x60 [96] | 0x20 [32] | SNK_OkButTxt | - | auto-layout |
| 35 | 0x12 | MoveFrontendSpriteRect slot 0 | (hw - 0x3c8 [hw-968]) + anim*0x18 | hh - 0x5f [hh-95] | - | - | sprite 0 | - | save-dialog slide |
| 36 | 0x12 | MoveFrontendSpriteRect slot 1 | iVar4 + anim*-0x18 + 0x36a [+874] | hh + 0x31 [+49] | - | - | sprite 1 | - | |
| 37 | 0x14 | MoveFrontendSpriteRect slot 0 | (hw - 200) + anim*0x18 | hh - 0x5f [hh-95] | - | - | sprite 0 | - | save-dialog exit |
| 38 | 0x14 | MoveFrontendSpriteRect slot 1 | iVar4 + anim*-0x18 + 0x6a [+106] | hh + 0x31 [+49] | - | - | sprite 1 | - | |
| 39 | 0xd | CreateFrontendDisplayModeButton (×multiple) | -0x120 [-288] | 0 | 0x120 [288] | 0x20 [32] | SNK_RaceAgain / ViewReplay / ViewRaceData / SelectNewCar / Quit / NextCupRace / SaveRaceStatus / OkButTxt | - | post-race menu buttons, all auto-layout |

DrawFrontendLocalizedStringToSurface @0x424560 calls in case 0 (loops at y-vars uVar3 stepping 0x18 over ranges 0x48/0x60/0x30 → 0xa8/0xf0 depending on gameType) write into the results panel surface — arg-less call site, NO x/y operands recoverable (helper reads globals).

Delegated sub-draw fns followed: `DrawRaceDataSummaryPanel @ 0x00421e90` (called `(g_postRaceRacerCardIndex)` in case 0 and at LAB_00422c47 / state-9 join) — fills g_lobbyErrorDialogSurface (0x198x0x188) results content. NOT decompiled (one-level budget spent on CarSelection PreviewOverlay). [UNCERTAIN: internal row positions not harvested.]
Confidence: [CONFIRMED @ 0x00422480]. [UNCERTAIN: case-0 DrawFrontendLocalizedStringToSurface y-loop fill positions are the uVar3 loop counters (0x48..0xf0 step 0x18) used as the helper's global y — captured as loop ranges, not as literal call-site x/y.]
# Frontend Layout Harvest — Slice 25 (g_frontendScreenFnTable indices 25–29)

Source: deterministic decompilation of `TD5_d3d.exe` via Ghidra (read_only). All
coordinates are literal immediate operands. Conventions:
- `cw = g_frontendCanvasW >> 1`  (= 320 on 640×480), `ch = g_frontendCanvasH >> 1` (= 240).
- `fc = g_frontendAnimFrameCounter` (per-state slide animation counter; terminal value 0x20=32 unless noted).
- Helper signatures (callee addresses CONFIRMED):
  - `QueueFrontendOverlayRect @ 0x00425660 (dst_x, dst_y, src_x, src_y, w, h, color, surface)`
  - `MoveFrontendSpriteRect @ 0x004259d0 (slot, x, y)`
  - `CreateFrontendDisplayModeButton @ 0x00425de0 (label, x, y, w, h, ud)` — negative x = auto-layout flag.
  - `DrawFrontendLocalizedStringToSurface @ 0x00424560 ()` — takes NO x/y operands here; it consumes the centering set up by `MeasureOrCenterFrontendLocalizedString @ 0x004245xx (label, left_x, right_x)` (the left/right bound pair IS the on-surface horizontal extent).
- Dialog content (text) is drawn into an off-screen tracked surface (`g_lobbyErrorDialogSurface`) at SURFACE-LOCAL coordinates, then that whole surface is blitted to canvas via `QueueFrontendOverlayRect`. So text x/y are local-to-dialog, the rect blit x/y are canvas-relative.
- No `Copy16BitSurfaceRect @ 0x004251a0` or `DrawFrontendButtonBackground @ 0x00425b60` calls appear in any of the 5 functions.

g_frontendScreenFnTable @ 0x004655C4 decoded (30 LE u32). Slice indices 25–29:
25 → 0x00413bc0  ScreenPostRaceNameEntry
26 → 0x004237f0  ScreenCupFailedDialog
27 → 0x00423a80  ScreenCupWonDialog
28 → 0x00415370  ScreenStartupInit
29 → 0x0041d630  ScreenSessionLockedDialog

---

### Screen 25 @ 0x00413bc0 — ScreenPostRaceNameEntry
Mechanism: anchored to canvas center. `iVar14 = cw - 0xd2 [cw-210]`, `iVar9 = ch - 0x9f [ch-159]` are the base sprite-slide anchors; the header overlay uses `cw - 200 [cw-200]` and `(iVar9 - g_menuHeaderLabelYOffset) - 0x40`. `fc` drives slide-in/out.
States/structure: switch on g_frontendInnerState, cases 0–0xC (12). Anim var = g_frontendAnimFrameCounter (fc).

| # | State/case | Helper @addr | x (verbatim) [dec] | y (verbatim) [dec] | w | h | tex/label/surface | blit-flag | notes |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 0 | CreateFrontendDisplayModeButton 0x425de0 | -0x1c0 [-448] | 0 [0] | 0x1c0 [448] | 0x40 [64] | SNK_EnterPlayerNameButTxt | n/a | neg x = auto-layout; name-entry button |
| 2 | 1 | QueueFrontendOverlayRect 0x425660 | fc*0x10 + -0x1f6 + iVar14 [fc*16 -502 +(cw-210)] | (iVar9 - g_menuHeaderLabelYOffset) + -0x40 [(ch-159)-hdrYoff-64] | g_menuHeaderLabelSurfaceWidth | g_menuHeaderLabelSurfaceHeight | g_currentScreenIndex (header label surf) | color=0 | slide-in header |
| 3 | 1 | MoveFrontendSpriteRect 0x4259d0 | iVar14 + fc*-0x18 + 0x30a [(cw-210) -fc*24 +778] | iVar9 + fc*-4 + 0xf0 [(ch-159) -fc*4 +240] | – | – | sprite slot 0 | n/a | fc terminal 0x20 |
| 4 | 2 | QueueFrontendOverlayRect 0x425660 | uVar13 - 200 [cw-200] | (iVar9 - g_menuHeaderLabelYOffset) + -0x40 | g_menuHeaderLabelSurfaceWidth | g_menuHeaderLabelSurfaceHeight | g_currentScreenIndex | color=0 | header static |
| 5 | 2 | (RenderFrontendCreateSessionNameInput — name-input sub-render, no literal x/y here) | – | – | – | – | – | – | text-entry field; not a positional helper call |
| 6 | 3 | QueueFrontendOverlayRect 0x425660 | uVar13 - 200 [cw-200] | (iVar9 - g_menuHeaderLabelYOffset) + -0x40 | g_menuHeaderLabelSurfaceWidth | g_menuHeaderLabelSurfaceHeight | g_currentScreenIndex | color=0 | header static |
| 7 | 3 | MoveFrontendSpriteRect 0x4259d0 | iVar14 + fc*-0x20 + 10 [(cw-210) -fc*32 +10] | iVar9 + fc*-6 + 0x70 [(ch-159) -fc*6 +112] | – | – | sprite slot 0 | n/a | slide-out; fc terminal 0x20 |
| 8 | 4 | QueueFrontendOverlayRect 0x425660 | uVar13 - 200 [cw-200] | (iVar9 - g_menuHeaderLabelYOffset) + -0x40 | g_menuHeaderLabelSurfaceWidth | g_menuHeaderLabelSurfaceHeight | g_currentScreenIndex | color=0 | header static |
| 9 | 4 | CreateFrontendDisplayModeButton 0x425de0 | -0x208 [-520] | 0 [0] | 0x208 [520] | 0x20 [32] | (label NULL) | n/a | neg x = auto-layout; backing strip |
| 10 | 4 | CreateFrontendDisplayModeButton 0x425de0 | -0x130 [-304] | 0 [0] | 0x60 [96] | 0x20 [32] | SNK_OkButTxt | n/a | neg x = auto-layout; OK button |
| 11 | 4 | CreateTrackedFrontendSurface | – | – | 0x208 [520] | 0x90 [144] | g_lobbyErrorDialogSurface (alloc) | n/a | high-score dialog surface |
| 12 | 4 | BltColorFillToSurface | 0,0 | 0,0 | 0x208 [520] | 0x90 [144] | g_lobbyErrorDialogSurface | n/a | black fill RGB(0,0,0) |
| 13 | 5,6 | QueueFrontendOverlayRect 0x425660 | uVar13 - 200 [cw-200] | (iVar9 - g_menuHeaderLabelYOffset) + -0x40 | g_menuHeaderLabelSurfaceWidth | g_menuHeaderLabelSurfaceHeight | g_currentScreenIndex | color=0 | header static; then calls DrawPostRaceHighScoreEntry(g_selectedScheduleIndex) |
| 14 | 7 | MoveFrontendSpriteRect 0x4259d0 | fc*0x10 + -0x26b + iVar14 [fc*16 -619 +(cw-210)] | uVar8 - 0x8f [ch-143] | – | – | sprite slot 0 | n/a | – |
| 15 | 7 | MoveFrontendSpriteRect 0x4259d0 | fc*0x10 + -0x26b + iVar14 [fc*16 -619 +(cw-210)] | uVar8 + 0x89 [ch+137] | – | – | sprite slot 1 | n/a | – |
| 16 | 7 | QueueFrontendOverlayRect 0x425660 | iVar14 + fc*-0x30 + 0x755 [(cw-210) -fc*48 +1877] | uVar8 - 0x3f [ch-63] | 0x208 [520] | 0x90 [144] | g_lobbyErrorDialogSurface | color=0 | high-score dialog slide-in |
| 17 | 7 | QueueFrontendOverlayRect 0x425660 | uVar13 - 200 [cw-200] | (iVar9 - g_menuHeaderLabelYOffset) + -0x40 | g_menuHeaderLabelSurfaceWidth | g_menuHeaderLabelSurfaceHeight | g_currentScreenIndex | color=0 | header static; fc terminal 0x27 |
| 18 | 8,9 | QueueFrontendOverlayRect 0x425660 | uVar13 - 200 [cw-200] | (iVar9 - g_menuHeaderLabelYOffset) + -0x40 | g_menuHeaderLabelSurfaceWidth | g_menuHeaderLabelSurfaceHeight | g_currentScreenIndex | color=0 | header static |
| 19 | 8,9 | QueueFrontendOverlayRect 0x425660 | uVar13 - 0xcd [cw-205] | uVar8 - 0x3f [ch-63] | 0x208 [520] | 0x90 [144] | g_lobbyErrorDialogSurface | color=0 | dialog settled position |
| 20 | 10 | QueueFrontendOverlayRect 0x425660 | uVar13 - 200 [cw-200] | (iVar9 - g_menuHeaderLabelYOffset) + -0x40 | g_menuHeaderLabelSurfaceWidth | g_menuHeaderLabelSurfaceHeight | g_currentScreenIndex | color=0 | header static |
| 21 | 10 | QueueFrontendOverlayRect 0x425660 | uVar13 - 0xcd [cw-205] | uVar8 - 0x3f [ch-63] | 0x208 [520] | 0x90 [144] | g_lobbyErrorDialogSurface | color=0 | dialog settled; waits button |
| 22 | 0xb | QueueFrontendOverlayRect 0x425660 | uVar13 - 200 [cw-200] | (iVar9 - g_menuHeaderLabelYOffset) + -0x40 | g_menuHeaderLabelSurfaceWidth | g_menuHeaderLabelSurfaceHeight | g_currentScreenIndex | color=0 | header static |
| 23 | 0xc | QueueFrontendOverlayRect 0x425660 | iVar14 + fc*-0x18 + 10 [(cw-210) -fc*24 +10] | (iVar9 - g_menuHeaderLabelYOffset) + -0x40 | g_menuHeaderLabelSurfaceWidth | g_menuHeaderLabelSurfaceHeight | g_currentScreenIndex | color=0 | header slide-out |
| 24 | 0xc | MoveFrontendSpriteRect 0x4259d0 | fc*0x30 + 0x6a + iVar14 [fc*48 +106 +(cw-210)] | uVar8 - 0x8f [ch-143] | – | – | sprite slot 0 | n/a | – |
| 25 | 0xc | MoveFrontendSpriteRect 0x4259d0 | (uVar13 - 0x68) + fc*6 [(cw-104) +fc*6] | fc*0x30 + 0x128 + iVar9 [fc*48 +296 +(ch-159)] | – | – | sprite slot 1 | n/a | – |
| 26 | 0xc | QueueFrontendOverlayRect 0x425660 | iVar14 + fc*-0x38 + 5 [(cw-210) -fc*56 +5] | uVar8 - 0x3f [ch-63] | 0x208 [520] | 0x90 [144] | g_lobbyErrorDialogSurface | color=0 | dialog slide-out; fc terminal 0x10 |

Delegated sub-draw fns followed: 0x00413010 DrawPostRaceHighScoreEntry (called in case 5/6).
  Draws localized header strings + 5 high-score rows into g_lobbyErrorDialogSurface (520×144) at SURFACE-LOCAL coords. Helpers: MeasureOrCenterFrontendString(label,left,right) sets centering between [left,right]; DrawFrontendSmallFontStringToSurface(label,x,y,surf) draws at returned x; DrawFrontendClippedStringToSurface(name,x,y,surf,maxw). Column layout (left,right pairs) and row y's:
  - row fill: BltColorFillToSurface(0,0,0, 0x208[520],0x90[144], surf)
  - "NAME" header: center[0x10..0x70][16..112], y=7
  - "BEST"/"TIME"/"LAP"/"PTS" header: center[0x80..0xd4][128..212], y=0 (BEST) / y=7 (TIME/LAP/PTS) [BEST drawn at y=0; TIME/LAP/PTS at y=7]
  - "CAR" header: center[0xe4..0x150][228..336], y=7
  - "AVG" header: center[0x160..0x1ac][352..428], y=0; "SPD" sub: same column y=0xe[14]
  - "TOP" header: center[0x1bc..0x208][444..520], y=0; "SPD" sub: same column y=0xe[14]
  - data rows: y starts at 0x30[48], increments +0x10[16] per row, 5 rows (indices 0..4).
    rank# at x=0 ; name via DrawFrontendClippedStringToSurface(x=0x10[16], maxw=0x60[96]) ; time/lap/pts center[0x80..0xd4]; car-manuf center[0xe4..0x150]; avg-speed center[0x160..0x1ac]; top-speed center[0x1bc..0x208]. Highlighted row (uVar3==g_postRaceQualifyingScore) uses g_smallTextbSurface font else g_smallTextSurface.
Confidence: [CONFIRMED @ 0x00413bc0, sub @ 0x00413010]. [UNCERTAIN: g_menuHeaderLabelYOffset / g_menuHeaderLabelSurfaceWidth/Height are runtime globals, not literals — header rect w/h/y depend on them.]

---

### Screen 26 @ 0x004237f0 — ScreenCupFailedDialog
Mechanism: anchored to canvas center. `uVar1 = cw`, `uVar2 = ch`. Dialog rect blitted at `cw-0xa8 [cw-168]`, `ch-0x8f [ch-143]` when settled (state 5); on 640×480 = (152, 97). `fc` drives the slide-in.
States/structure: switch g_frontendInnerState cases 0–5. Cases 1/2/3 share a present-only block. Dialog visible only in states 4–5.

| # | State/case | Helper @addr | x (verbatim) [dec] | y (verbatim) [dec] | w | h | tex/label/surface | blit-flag | notes |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 0 | CreateTrackedFrontendSurface | – | – | 0x198 [408] | 0x70 [112] | g_lobbyErrorDialogSurface (alloc) | n/a | only if 0<gameType<7 |
| 2 | 0 | BltColorFillToSurface | 0,0 | 0,0 | 0x198 [408] | 0x70 [112] | g_lobbyErrorDialogSurface | n/a | black fill |
| 3 | 0 | DrawFrontendLocalizedStringToSurface 0x424560 (after MeasureOrCenter left=0,right=0x198) | center [0..408] | (no y operand) | – | – | SNK_SorryTxt | n/a | text into dialog surf |
| 4 | 0 | DrawFrontendLocalizedStringToSurface 0x424560 | center [0..408] | (no y operand) | – | – | SNK_YouFailedTxt | n/a | – |
| 5 | 0 | DrawFrontendLocalizedStringToSurface 0x424560 | center [0..408] | (no y operand) | – | – | SNK_ToWinTxt | n/a | – |
| 6 | 0 | DrawFrontendLocalizedStringToSurface 0x424560 | center [0..408] | (no y operand) | – | – | *(SNK_RaceTypeText + gameType*4) | n/a | race-type name |
| 7 | 0 | CreateFrontendDisplayModeButton 0x425de0 | -0x120 [-288] | 0 [0] | 0x60 [96] | 0x20 [32] | SNK_OkButTxt | n/a | neg x = auto-layout |
| 8 | 4 | QueueFrontendOverlayRect 0x425660 | uVar1 + fc*-0x18 + 600 [cw -fc*24 +600] | uVar2 - 0x8f [ch-143] | 0x198 [408] | 0x70 [112] | g_lobbyErrorDialogSurface | color=0 | slide-in |
| 9 | 4 | MoveFrontendSpriteRect 0x4259d0 | (uVar1 - 0x318) + fc*0x18 [(cw-792) +fc*24] | uVar2 + 0x31 [ch+49] | – | – | sprite slot 0 | n/a | fc terminal 0x20 |
| 10 | 5 | QueueFrontendOverlayRect 0x425660 | uVar1 - 0xa8 [cw-168] | uVar2 - 0x8f [ch-143] | 0x198 [408] | 0x70 [112] | g_lobbyErrorDialogSurface | color=0 | settled; waits OK button |

Delegated sub-draw fns followed: none (all draws inline).
Confidence: [CONFIRMED @ 0x004237f0].

---

### Screen 27 @ 0x00423a80 — ScreenCupWonDialog
Mechanism: anchored to canvas center. `uVar1 = cw`, `uVar2 = ch`. Dialog 408×196. Settled blit at `cw-0xa8 [cw-168]`, `ch-0x8f [ch-143]`. `fc` drives slide-in.
States/structure: switch cases 0–5; 1/2/3 share present block; visible only 4–5.

| # | State/case | Helper @addr | x (verbatim) [dec] | y (verbatim) [dec] | w | h | tex/label/surface | blit-flag | notes |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 0 | CreateTrackedFrontendSurface | – | – | 0x198 [408] | 0xc4 [196] | g_lobbyErrorDialogSurface (alloc) | n/a | only if 0<gameType<7 |
| 2 | 0 | BltColorFillToSurface | 0,0 | 0,0 | 0x198 [408] | 0xc4 [196] | g_lobbyErrorDialogSurface | n/a | black fill |
| 3 | 0 | DrawFrontendLocalizedStringToSurface 0x424560 (MeasureOrCenter left=0,right=0x198) | center [0..408] | (no y operand) | – | – | SNK_CongratsTxt | n/a | – |
| 4 | 0 | DrawFrontendLocalizedStringToSurface 0x424560 | center [0..408] | (no y operand) | – | – | SNK_YouHaveWonTxt | n/a | – |
| 5 | 0 | DrawFrontendLocalizedStringToSurface 0x424560 | center [0..408] | (no y operand) | – | – | *(SNK_RaceTypeText + gameType*4) | n/a | race-type name |
| 6 | 0 | DrawFrontendLocalizedStringToSurface 0x424560 | center [0..408] | (no y operand) | – | – | local_40 ("%d %s" via s__d__s_004660e0) | n/a | only if g_cupSchedule_currentCup!=0 |
| 7 | 0 | DrawFrontendLocalizedStringToSurface 0x424560 | center [0..408] | (no y operand) | – | – | local_40 ("%d %s") | n/a | only if g_cupSchedule_currentRound!=0 |
| 8 | 0 | CreateFrontendDisplayModeButton 0x425de0 | -0x120 [-288] | 0 [0] | 0x60 [96] | 0x20 [32] | SNK_OkButTxt | n/a | neg x = auto-layout |
| 9 | 4 | QueueFrontendOverlayRect 0x425660 | uVar1 + fc*-0x18 + 600 [cw -fc*24 +600] | uVar2 - 0x8f [ch-143] | 0x198 [408] | 0xc4 [196] | g_lobbyErrorDialogSurface | color=0 | slide-in |
| 10 | 4 | MoveFrontendSpriteRect 0x4259d0 | (uVar1 - 0x318) + fc*0x18 [(cw-792) +fc*24] | uVar2 + 0x61 [ch+97] | – | – | sprite slot 0 | n/a | fc terminal 0x20 |
| 11 | 5 | QueueFrontendOverlayRect 0x425660 | uVar1 - 0xa8 [cw-168] | uVar2 - 0x8f [ch-143] | 0x198 [408] | 0xc4 [196] | g_lobbyErrorDialogSurface | color=0 | settled; waits OK |

Delegated sub-draw fns followed: none (all draws inline).
Confidence: [CONFIRMED @ 0x00423a80]. Note: dialog height 0xc4[196] > screens 26/29 (0x70[112]) — taller because of the extra cup/round lines; sprite-0 settle y is ch+0x61[+97] vs ch+0x31[+49] on screen 26.

---

### Screen 28 @ 0x00415370 — ScreenStartupInit
Mechanism: anchored to canvas center, no anim slide (static placeholder dialog). Uses `(g_frontendCanvasW>>1)-0xa8 [cw-168]`, `(g_frontendCanvasH>>1)-0x8f [ch-143]` directly inline. No `fc`-driven motion. This is an init/transition screen that hands control to table[0] after 4 ticks.
States/structure: switch cases 0–4; 1/2/3 share the blit block.

| # | State/case | Helper @addr | x (verbatim) [dec] | y (verbatim) [dec] | w | h | tex/label/surface | blit-flag | notes |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 0 | CreateTrackedFrontendSurface | – | – | 0x198 [408] | 0x70 [112] | g_lobbyErrorDialogSurface (alloc) | n/a | – |
| 2 | 0 | BltColorFillToSurface | 0,0 | 0,0 | 0x198 [408] | 0x70 [112] | g_lobbyErrorDialogSurface | n/a | black fill |
| 3 | 0 | CreateFrontendDisplayModeButton 0x425de0 | -0x120 [-288] | 0 [0] | 0x60 [96] | 0x20 [32] | SNK_OkButTxt | n/a | neg x = auto-layout |
| 4 | 1,2,3 | QueueFrontendOverlayRect 0x425660 | (g_frontendCanvasW>>1) - 0xa8 [cw-168] | (g_frontendCanvasH>>1) - 0x8f [ch-143] | 0x198 [408] | 0x70 [112] | g_lobbyErrorDialogSurface | color=0 | static; no animation |

Delegated sub-draw fns followed: none.
Confidence: [CONFIRMED @ 0x00415370]. Note: no localized text drawn into the surface (blank black box); state 4 releases surface and resets to screen table[0]. No sprite/MoveFrontendSpriteRect calls.

---

### Screen 29 @ 0x0041d630 — ScreenSessionLockedDialog
Mechanism: anchored to canvas center. `uVar1 = cw`, `uVar2 = ch`. Dialog 408×112. Settled at `cw-0xa8 [cw-168]`, `ch-0x8f [ch-143]`. `fc` slide-in. Structurally identical to screen 26 minus a text line, always returns to MAIN_MENU.
States/structure: switch cases 0–5; 1/2/3 present block; visible 4–5.

| # | State/case | Helper @addr | x (verbatim) [dec] | y (verbatim) [dec] | w | h | tex/label/surface | blit-flag | notes |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 0 | BlitFrontendCachedRect | 0,0 | – | g_frontendCanvasW | g_frontendCanvasH | (cached bg) | n/a | full-canvas cached bg blit |
| 2 | 0 | CreateTrackedFrontendSurface | – | – | 0x198 [408] | 0x70 [112] | g_lobbyErrorDialogSurface (alloc) | n/a | – |
| 3 | 0 | BltColorFillToSurface | 0,0 | 0,0 | 0x198 [408] | 0x70 [112] | g_lobbyErrorDialogSurface | n/a | black fill |
| 4 | 0 | DrawFrontendLocalizedStringToSurface 0x424560 (MeasureOrCenter left=0,right=0x198) | center [0..408] | (no y operand) | – | – | SNK_SorryTxt | n/a | – |
| 5 | 0 | DrawFrontendLocalizedStringToSurface 0x424560 | center [0..408] | (no y operand) | – | – | SNK_SeshLockedTxt | n/a | – |
| 6 | 0 | CreateFrontendDisplayModeButton 0x425de0 | -0x120 [-288] | 0 [0] | 0x60 [96] | 0x20 [32] | SNK_OkButTxt | n/a | neg x = auto-layout |
| 7 | 4 | QueueFrontendOverlayRect 0x425660 | uVar1 + fc*-0x18 + 600 [cw -fc*24 +600] | uVar2 - 0x8f [ch-143] | 0x198 [408] | 0x70 [112] | g_lobbyErrorDialogSurface | color=0 | slide-in |
| 8 | 4 | MoveFrontendSpriteRect 0x4259d0 | (uVar1 - 0x318) + fc*0x18 [(cw-792) +fc*24] | uVar2 + 0x31 [ch+49] | – | – | sprite slot 0 | n/a | fc terminal 0x20 |
| 9 | 5 | QueueFrontendOverlayRect 0x425660 | uVar1 - 0xa8 [cw-168] | uVar2 - 0x8f [ch-143] | 0x198 [408] | 0x70 [112] | g_lobbyErrorDialogSurface | color=0 | settled; waits OK |

Delegated sub-draw fns followed: none.
Confidence: [CONFIRMED @ 0x0041d630].

---

# Consolidated follow-ups / [UNCERTAIN] items

These are the only spots the byte-literal harvest could not fully resolve. Each needs a small targeted Ghidra pass (listing/FP-stack disasm) before the affected rows are byte-exact. Everything NOT listed here is [CONFIRMED] from the decompilation.

1. **Screen 4 (ScreenLegalCopyright)** — the per-row text `y` arg decompiled as a denormal float literal `6.465804e-39`; Ghidra lost the loop-scaled value. `x = (float)(canvasW/10)` is confirmed. → needs FP-stack disassembly to recover the true y stride.
2. **Screen 6 (RaceTypeCategoryMenu) & Screen 16 (ScreenDisplayOptions)** — use `CreateFrontendDisplayModePreviewButton` (greyed/preview variant); its own entry address was not separately confirmed and the greying is cheat/state-gated. Button x/y/w/h literals ARE captured.
3. **Screen 18 (ScreenControllerBindingPage)** — (a) slide-out cases 0xb/0x1b reuse one SSA name `iVar11` for two values (`BX` vs `canvasW-200`); control flow indicates `BX` but needs byte-exact listing confirmation. (b) Delegates `DrawControlBindingTextWithOkButton` and `OpenControllerBindingPageNoneHeader` were noted but not decompiled (entry addrs unresolved).
4. **Screens 23/24 (HighScore / RaceResults)** — internal row layout of delegates `DrawPostRaceHighScoreEntry @0x413010` and `DrawRaceDataSummaryPanel @0x421e90` not harvested (one-level follow budget spent elsewhere). Outer anchors captured.
5. **Screens 8/9 (ConnectionBrowser / SessionPicker)** — scroll-arrow & selection-bar sprites position from the runtime panel origin `g_connBrowserListOriginX_PROVISIONAL[0].origin_x/origin_y`, not immediates. Only the literal `+offsets` (0xf2, 0x1e, 0x6c, 0x10, 0x2f) are captured; the base is runtime.
6. **Screen 14 (ScreenControlOptions)** — device-icon `src_y` values are runtime device-index shifts (`local_84/88 << 5`), not literals.
7. **Text helpers generally** — `DrawFrontendLocalizedStringToSurface @0x424560` is `void(void)`; text pixel x comes from the preceding `MeasureOrCenterFrontendLocalizedString(str, left, right)` centering math. The `(left,right)` extents are captured; exact glyph x requires running the centering formula against the localized string width at runtime (or the LANGUAGE.DLL string lengths).

## Recommended port workflow from here

1. Define the helper signatures + the `BX`/`BY`/`cW2`/`cH2` idioms once (most are already in `td5_frontend.c`).
2. For each screen, replace eyeballed constants with the verbatim formulas in that screen's table — paying attention to the negative-x auto-layout flag and the per-state/animation-frame expressions.
3. Resolve the 7 follow-ups above with targeted Ghidra passes only if/when those specific screens look off.
