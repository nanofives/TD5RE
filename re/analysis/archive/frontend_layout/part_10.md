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
