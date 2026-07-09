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
