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
