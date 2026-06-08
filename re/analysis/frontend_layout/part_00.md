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
