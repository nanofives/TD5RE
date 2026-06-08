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
