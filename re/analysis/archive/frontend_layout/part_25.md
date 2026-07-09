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
