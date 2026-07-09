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
