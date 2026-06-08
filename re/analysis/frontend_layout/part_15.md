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
