# TD5 Frontend Screens 5/6/7/8/9 — Complete Element + Behavior Spec

RE target: `TD5_d3d.exe` (image base 0x00400000). All addresses are Ghidra VAs, hex AND decimal.
Source: literal Ghidra decompilation of each screen-fn body + the verified flush/render model in
`frontend_rendering_model.md` / `frontend_flow_model.md` / `frontend_call_graph_closure.md`.

## Cross-screen mechanics (apply to all 5 — grounded, not re-derived per screen)

- **INLINE** = the screen-fn writes pixels directly into a work/secondary surface or bakes them
  into a tracked button/label surface this frame (fills, TGA decode, label-surface text bakes,
  `BltColorFillToSurface` into a panel surface, `DrawFrontend*StringToSurface`).
- **FLUSH** = the screen-fn only PRIMES (queues an overlay rect via `QueueFrontendOverlayRect`
  0x425660, builds a button slot via `CreateFrontendDisplayModeButton` 0x425de0, or moves a slot
  via `MoveFrontendSpriteRect` 0x4259d0); the actual on-screen pixels come from the per-frame
  `FlushFrontendSpriteBlits` 0x425540 + `RenderFrontendUiRects` 0x425a30 (button-slot walk) +
  the two unconditional decoupled draws `UpdateExtrasGalleryDisplay` 0x40d830 and
  `RenderFrontendDisplayModeHighlight` 0x4263e0.
- **DECOUPLED draws that run over EVERY one of these 5 screens** (no screen-fn calls them):
  1. **Selection/hover highlight** `RenderFrontendDisplayModeHighlight` 0x4263e0 — 4 edge bars
     color 0xc000 (49152) around the slot indexed by `g_frontendOverlayRectArrayTail` in
     `g_connBrowserListOriginX_PROVISIONAL[]` (0x00499c78, FrontendButtonSlot stride 0x34=52),
     drawn into `g_frontendBackSurfacePtr`. Gate: tail!=-1 AND `g_frontendCursorOverlayHidden`==0.
  2. **Extras gallery cover-art** `UpdateExtrasGalleryDisplay` 0x40d830 — no-op on these 5 unless
     `g_extrasGallerySlideSurfaces`!=0 and `g_frontendScreenTransitionFlag`==2 (band gallery) /
     extras idle; all 5 screens set transitionFlag=0 in state 0, so the gallery is inert here.
  3. **Mouse cursor sprite** — LOOP queues `QueueFrontendOverlayRect(mouseX,mouseY,0,0,0x16,0x1e,
     0xff0000,g_frontendCursorTextureId)` when `g_frontendCursorOverlayHidden`==0 &&
     `g_frontendMouseCursorEnabled`==1 (RunFrontendDisplayLoop 0x414b50 step 9).
- **Selected-button vertical offset**: `RenderFrontendUiRects` 0x425a30 offsets the src-rect of the
  slot == `g_frontendButtonIndex` by its bake height (highlight = top half of the h*2 baked surface).
- **Button-slot table** `g_connBrowserListOriginX_PROVISIONAL` @0x00499c78. Each button created by
  `CreateFrontendDisplayModeButton(label,x,y,w,h,user_data)` allocates a tracked surface of
  (w, h*2) baking BOTH a highlighted (top, state 1) and idle (bottom, state 0) frame+label via
  DrawFrontendButtonBackground 0x425b60 (9-slice from ButtonBits page DAT_00496268) +
  DrawFrontendLocalizedStringToSurface. x<0 means "auto-stack" (off-screen seed; the slide states
  move them in). **The on-screen button pixels are FLUSH-drawn** by RenderFrontendUiRects.
- **Cup-tier gating global** `g_cheatFlagBitfieldGameModes` @0x004962a8 (cheat bitfield &7).
- **Selection nav globals** (LOOP-written, screen-read): `g_frontendButtonIndex`,
  `g_frontendButtonPressedFlag`, `g_postRaceRacerCardNavDirection` (◄/► delta),
  `g_frontendEscKeyButtonIndex` (ESC→button), `g_frontendInputEdgeBits` (kb/stick rising edges;
  bit 0x200=up, 0x400=down for list browsers).
- **g_frontendAnimFrameCounter** (LOOP `++` each frame): drives every slide; transitions fire on
  exact equality. Animation speed = frame rate (no real-time interp).

---

### Screen 5 @0x00415490 — ScreenMainMenuAnd1PRaceFlow  [interactive Y]

Inner states (in-body `switch(g_frontendInnerState)`, NOT a jump table): 0 init/build,
1+2 present/settle, 3 slide-in (anim 0..0x27=39), 4 MAIN-MENU interactive (7-button dispatch),
5 EXIT-confirm-dialog settle, 6 EXIT-confirm interactive (Yes/No), 7 cancel-dialog teardown→4,
8 generic slide-out-cover→9, 9 slide-out + post-select device-check → SetFrontendScreen, 10 EXIT
"yes" slide-out commit→0xb, 0xb release/restore→0xc, 0xc final slide-out → SetFrontendScreen(EXTRAS
GALLERY idx22), 0x14 controller-required dialog build, 0x15 dialog slide-in (bakes the "MUST
SELECT" + per-mode message text), 0x16 dialog interactive (any press dismisses → 0x14 rebuild),
0x17 dialog slide-out → SetFrontendScreen(CONTROL OPTIONS idx14). (Confirmed addrs: state bodies
inline at 0x415490..0x416822.)

ELEMENTS:
| element | produced by (fn/global/asset) | INLINE/FLUSH | dest pos | gating | notes |
|---|---|---|---|---|---|
| Full-screen background | LoadTgaToFrontendSurface16bpp(MainMenu.tga, FrontEnd.zip) @state0; CopyPrimaryFrontendBufferToSecondary | INLINE | (0,0) 640×480 g_primaryWorkSurface | state 0 | s_Front_End_MainMenu_tga_00463ecc |
| Header label "MAIN MENU" | CreateMenuStringLabelSurface(0)→g_currentScreenIndex; QueueFrontendOverlayRect each frame at anim-driven X | FLUSH | (cx-200, …) header band | states 3-0xc | menu-font label surface |
| 7 menu buttons: Race, QuickRace, TimeDemo/2Player, NetPlay, Options, HiScore, Exit | CreateFrontendDisplayModeButton(label,-0xe0,0,0xe0,0x20,0) ×7 @state0 → slots 0..6 | FLUSH | x=-0xe0 seed; slid in by MoveFrontendSpriteRect(0..6) | state 0 build | label SNK_RaceMenuButTxt/QuickRaceButTxt/(TimeDemoButTxt if g_appExref+0x170==0 else TwoPlayerButTxt)/NetPlayButTxt/OptionsButTxt/HiScoreButTxt/ExitButTxt |
| Selection highlight (4 edge bars) | RenderFrontendDisplayModeHighlight 0x4263e0 | FLUSH (decoupled) | around selected slot | tail!=-1 && cursorOverlayHidden==0 | color 0xc000 |
| Mouse cursor | LOOP QueueFrontendOverlayRect(cursor) | FLUSH (decoupled) | mouse XY | cursorOverlayHidden==0 && mouseEnabled | snkmouse, 22×30 |
| EXIT confirm dialog label | CreateMenuStringLabelSurface(8)→g_oneRaceFlowSelectedCarId; QueueFrontendOverlayRect | FLUSH | (cx-200, …+0x124) | states 5/6/10 | "Are you sure" prompt |
| EXIT Yes / No buttons | CreateFrontendDisplayModeButton(SNK_YesButTxt,cx-0xc2,cy+0xa5,0x60,0x20) + (SNK_NoxButTxt,cx-0x42,cy+0xa5,0x60,0x20) @state6-from-4 | FLUSH | cy+0xa5 row | btn6 pressed on state 4 | preview layout snapshot via BeginFrontendDisplayModePreviewLayout |
| Controller-required dialog panel | CreateTrackedFrontendSurface(0x1e2,0x40)→g_lobbyErrorDialogSurface; BltColorFillToSurface clears | INLINE (panel) then FLUSH (queued) | (cx-0xc2, (H-0x6c)/2) 0x1e2×0x40 | state 0x14/0x15/0x16 | dialog backing |
| Controller-required dialog text | MeasureOrCenterFrontendLocalizedString(SNK_MustSelectTxt)+ (g_frontendMustSelectMessage @0x004962d8) → DrawFrontendLocalizedStringToSurface | INLINE bake | into dialog panel | state 0x15 anim==0x10 | message = MustPlayer1Txt or MustPlayer2Txt by mode |
| Controller dialog OK button | CreateFrontendDisplayModeButton(SNK_OkButTxt,0,-0x60,0x60,0x20) @state0x14 | FLUSH | -0x60 seed, slid in | state 0x14 | esc btn implicit |

Primed contract globals: `g_currentScreenIndex` (header label surf), `g_oneRaceFlowSelectedCarId`
(confirm/exit dialog label surf), `g_lobbyErrorDialogSurface` (0x1e2×0x40 controller dialog panel),
`g_menuHeaderLabelSurfaceWidth/Height`, `g_menuHeaderLabelYOffset`, `g_frontendEscKeyButtonIndex`
(=6 main menu / =5 after cancel), `g_returnToScreenIndex` + `g_mainMenuButtonHint_PROVISIONAL`
(1..6 deferred target for the post-slide device check), `g_frontendMustSelectMessage_PROVISIONAL`
(@0x004962d8), button slots 0..6 in g_connBrowserListOriginX[]. Also writes race-config shadows
on state-0 second pass (gFog/units/camera/difficulty/laps → live globals) and DXSound volumes.

Animation: states 3/9/0xc/0x15/0x17 slide buttons via MoveFrontendSpriteRect(slot,x(anim),y(anim))
+ re-queue header at anim*±0x18/0x10. State 3 commits at anim==0x27(39) (held to 0x26 + 3-frame
AdvanceFrontendTickAndCheckReady gate). State 9/0xc commit at anim==0x10(16). State 0x15 dialog
slide commits at anim==0x10. State 0x17 dialog slide-out commits at anim==0x18(24). Slide-in uses
SoundEffect 4 (settle), exits use SoundEffect 5.

Conditional elements:
- **Button 2 label**: `*(g_appExref+0x170)==0` → "Two Player" (TwoPlayerButTxt); else "Time Demo"
  (TimeDemoButTxt = benchmark). (state 0, @0x415d3a.)
- **Button 2 action**: if g_appExref+0x170!=0 → benchmark path (g_benchmarkRequestPending=1,
  InitializeRaceSeriesSchedule + InitializeFrontendDisplayModeState, early return to race); else
  2-player car-select (g_twoPlayerModeEnabled=1, gameType=0). (state 4 case 2.)
- **Controller-required dialog** (states 0x14-0x17): entered from state 9 only when the deferred
  hint requires a device that is unset — hint 1/2/4 check g_player1InputSource==7 (=none) →
  state 0x14; hint 3 (2-player) checks player1 then player2 ==7. If a device is missing the flow
  diverts to the "MUST SELECT PLAYER 1/2 CONTROLLER" dialog then to CONTROL OPTIONS (idx14) instead
  of the requested screen.
- **NetPlay (button 3)**: state 4 case 3 jumps straight to a wipe (state 8) without the device
  check; target = CONNECTION_BROWSER (idx8).

Input dispatch (state 4, `g_frontendButtonPressedFlag` + `g_frontendButtonIndex`):
btn0→returnIndex=RACE_TYPE_MENU(6), hint=1 → wipe state8.
btn1→QUICK_RACE(7), hint=2 → wipe state8.
btn2→(2P) CAR_SELECTION(20) hint3 / (benchmark) early race-start.
btn3→CONNECTION_BROWSER(8), hint4 → wipe state8.
btn4→OPTIONS_HUB(12), hint5 → wipe state8.
btn5→HIGH_SCORE(23), hint6 → wipe state8.
btn6→Exit-confirm dialog (build Yes/No, state→5). ESC (esc-idx 6) = Exit.
State 6 confirm: btn0(Yes)→state10 exit-slide→0xb→0xc→SetFrontendScreen(EXTRAS_GALLERY 22);
btn1(No)→restore layout→state7→state4. State 9 then resolves hint→SetFrontendScreen(returnIndex)
or controller dialog. State 0x16: any press dismisses the dialog → rebuild state 0x14.

Confidence: [CONFIRMED @0x415490 full body; flush/decoupled model @0x425540/0x4263e0/0x425a30;
g_cheatFlagBitfieldGameModes @0x004962a8; g_frontendMustSelectMessage @0x004962d8].
[UNCERTAIN: `g_appExref+0x170` is the benchmark/2-player capability flag by usage; exact field name
not resolved. The "3D background" mentioned in the harvest brief does NOT exist — the background is
the static MainMenu.tga (no live 3D scene in this screen-fn).]

---

### Screen 6 @0x004168b0 — RaceTypeCategoryMenuStateMachine  [interactive Y]

Inner states: pointer-dispatched via JMP [EAX*4 + 0x00417cd4] @0x004168e2 (12 entries, raw bytes
confirmed: 0x4168e9,0x416a81,0x416beb,0x416c4b,0x416d80,0x416f16,0x41707a,0x4173b1,0x4174ce,
0x4175bc,0x417700,0x417918 = states 0..11). PLUS an in-body re-entry state 0x14 (20) tail
(g_frontendInnerState=1, anim=-1) reached when the flow loops back. State roles:
0 build TOP menu, 1 slide-in (anim→0x20=32), 2 settle (3-frame gate), 3 TOP interactive +
hover-change detect, 4 hover-description rebake → back to 3, 5 slide-out → SetFrontendScreen(return),
6 TOP→CUP slide-out + build CUP-TIER menu (gated), 7 CUP slide-in (anim→0x20), 8 CUP interactive +
hover detect, 9 CUP hover-description rebake → back to 8, 10 CUP→exit slide-out →
SetFrontendScreen(return), 0xb CUP→TOP slide-out + rebuild TOP menu (Back), 0xc TOP re-slide-in→3.

ELEMENTS:
| element | produced by (fn/global/asset) | INLINE/FLUSH | dest pos | gating | notes |
|---|---|---|---|---|---|
| Full-screen background | LoadTgaToFrontendSurface16bpp(MainMenu.tga); CopyPrimaryFrontendBufferToSecondary | INLINE | (0,0) 640×480 | state 0 | |
| Header label "RACE MENU" | CreateMenuStringLabelSurface(1)→g_currentScreenIndex; QueueFrontendOverlayRect anim X | FLUSH | header band | all states | |
| Hover-description panel | CreateTrackedFrontendSurface(0x110,0xb4)→g_lobbyErrorDialogSurface; BltColorFillToSurface clears; DrawFrontendLocalizedStringToSurface (title) + DrawFrontendSmallFontStringToSurface (wrapped body 0x20..0xaf step 0xc) | INLINE bake | (cx+0x26, cy-0x5f) 0x110×0xb4 | states 1/3/4/5/6/7/8/9/0xb/0xc | text = SNK_RaceTypeText[gameType] |
| TOP menu 7 buttons: SingleRace, CupRace, ContCup, TimeTrials, DragRace, CopChase, Back | CreateFrontendDisplayModeButton(label,-0xe0,0,0xe0,0x20) ×6 + Back(-0xe0,0,0x70,0x20) @state0; ContCup = PreviewButton if ValidateCupDataChecksum==0 | FLUSH | slid in | state 0 | slots 0..6; Back narrower (0x70) |
| CUP-TIER 7 buttons: Championship, Era, Challenge, Pitbull, Masters, Ultimate, Back | CreateFrontendDisplayModeButton / PreviewButton ×6 + Back @state6 | FLUSH | slid in | state 6 (built at anim==0x23) | preview-greyed by cheat tier (see conditional) |
| Selection highlight | RenderFrontendDisplayModeHighlight 0x4263e0 | FLUSH (decoupled) | selected slot | tail!=-1 && overlay active | |
| Mouse cursor | LOOP queue | FLUSH (decoupled) | mouse XY | | |

Primed contract globals: `g_currentScreenIndex` (header), `g_lobbyErrorDialogSurface` (0x110×0xb4
hover-desc panel), `g_previousButtonIndex` (hover-change latch, -1 init), `g_frontendSelectedGameType`
(written per button — see dispatch), `g_returnToScreenIndex`, `g_frontendEscKeyButtonIndex`=6,
`g_raceWithinSeriesIndex`, `g_selectedScheduleIndex`=g_attractModeTrackIndex (cup path),
`g_cheatFlagBitfieldGameModes` (cup-tier preview gating). ConfigureGameTypeFlags 0x410ca0 invoked
on commit (drives the cup-schedule configurator data-table @0x00464108).

Animation: state 1/7 slide-in 6 (TOP) or 7 (with Back) sprite slots by anim-derived Y, commit
anim==0x20(32)+SoundEffect 4. State 5/6/0xb/0xc cross-slide commit anim==0x23(35). Hover-change
states 4/9 are a single-frame rebake (BltColorFillToSurface the panel, re-draw the new gameType
description) then return to the interactive state. SoundEffect 5 on exits, 4 on settle.

Conditional elements:
- **"Continue Cup" (button 2)** is a *preview* (greyed/disabled) button when
  `ValidateCupDataChecksum()`==0 (no valid CupData.td5 save); a normal selectable button otherwise.
  (state 0 @0x416a1e and state 0xb rebuild.)
- **Cup-tier buttons gated by `g_cheatFlagBitfieldGameModes` @0x004962a8** (state 6 @0x41736b):
  - ==0: Championship+Era selectable; **Challenge/Pitbull/Masters/Ultimate are PreviewButtons**
    (locked/greyed).
  - ==1: Championship+Era+Challenge+Pitbull selectable; Masters/Ultimate Preview (locked).
  - else (>=2): ALL six tiers selectable (full unlock).
- Hover-description text is per-selected-gameType from SNK_RaceTypeText[gameType*4] (state 4/9).

Input dispatch:
- State 3 (TOP): hover change (btnIndex != previousIndex) → state 4 rebake. On press:
  btn0→gameType=0 (Single); btn3→9 (Drag); btn4→7? — actually state-3 mapping: btn0→0,
  btn{3,4,5}→btnIndex+3 (btn3 special→9), then ConfigureGameTypeFlags, return=CAR_SELECTION(20),
  →state5. btn1(CupRace)→state6 (open cup tiers). btn2(ContCup)→LoadContinueCupData,
  g_replayFileAvailable=0, return=RACE_RESULTS(24), →state5. btn6(Back)→return computed
  (CREATE_SESSION vs offset by g_networkLobbyEntryPhase) →state5. ESC=btn6.
- State 4 maps the *display* gameType for the description panel: btn0→0, btn4→7, btn5→8, btn3→9,
  btn1→10, btn2→0xb (these are the SNK_RaceTypeText label indices), then →state3.
- State 8 (CUP): hover change→state9. On press btn0..5 → gameType=btnIndex+1,
  return=CAR_SELECTION(20), raceWithinSeries=0, ConfigureGameTypeFlags, schedule=attract track,
  →state10. btn6(Back)→state0xb (back to TOP). ESC=btn6.
- State 9 rebakes the cup-tier description (gameType=btnIndex+1) →state8.

Confidence: [CONFIRMED @0x004168b0 full body; jump table raw bytes @0x00417cd4;
g_cheatFlagBitfieldGameModes @0x004962a8; ConfigureGameTypeFlags 0x410ca0 + cup table 0x00464108
per flow_model §4B]. [UNCERTAIN: state-3 non-Back gameType arithmetic (`btnIndex+3`, btn3→9) yields
{0,?,?,9,7,8} via the state-4 normalizer; the exact SNK_RaceTypeText index per physical button is
the state-4 remap, decompiled literally above.]

---

### Screen 7 @0x004213d0 — ScreenQuickRaceMenu  [interactive Y]

Inner states (in-body switch): 0 init/build, 1+2 present/settle, 3 slide-in (anim→0x27=39),
4 INTERACTIVE (◄► car/track cycling + OK/Back), 5 exit settle, 6 slide-out → SetFrontendScreen /
InitializeRaceSeriesSchedule. (Confirmed @0x4213d0..0x421d76.)

ELEMENTS:
| element | produced by (fn/global/asset) | INLINE/FLUSH | dest pos | gating | notes |
|---|---|---|---|---|---|
| Full-screen background | LoadTgaToFrontendSurface16bpp(MainMenu.tga); CopyPrimaryFrontendBufferToSecondary | INLINE | (0,0) 640×480 | state 0 | |
| Header label "QUICK RACE" | CreateMenuStringLabelSurface(3)→g_currentScreenIndex; QueueFrontendOverlayRect anim X | FLUSH | header band | all | |
| Car+Track info panel | CreateTrackedFrontendSurface(0x208,200)→g_lobbyErrorDialogSurface; BltColorFillToSurface clears; sprintf_game(&g_quickRaceMenuFormatPrefix)+DrawFrontendLocalizedStringToSurface (car name top half, track name bottom half) | INLINE bake | (cx-200, cy-0x8f) 0x208×200 | states 3/4/5/6 | re-baked on each ◄► cycle (state 4) |
| "locked" overlay text | DrawFrontendLocalizedStringToSurface when (&g_savedCarLockTable)[carId]!=0 / (&g_packedConfig_displayPrefsBlock)[track]!=0 AND cheat off | INLINE bake | into panel | car/track locked & !cheat | shows lock message |
| Button 0 "CHANGE CAR" + ◄► arrows | CreateFrontendDisplayModeButton(SNK_ChangeCarButTxt,cx-200,cy-0x67,0x100,0x20); InitializeFrontendDisplayModeArrows(0,1) | FLUSH | cy-0x67 | state 0 | arrows set slot flag|=2 (two-axis) |
| Button 1 "CHANGE TRACK" + ◄► arrows | CreateFrontendDisplayModeButton(SNK_ChangeTrackButTxt,cx-200,cy+0x11,0x100,0x20); InitializeFrontendDisplayModeArrows(1,1) | FLUSH | cy+0x11 | state 0 | arrows |
| Button 2 "OK" | CreateFrontendDisplayModeButton(SNK_OkButTxt,cx-200,cy+0x89,0x60,0x20) | FLUSH | cy+0x89 | state 0 | |
| Button 3 "BACK" | CreateFrontendDisplayModeButton(SNK_BackButTxt,cx-0x58,cy+0x89,0x70,0x20) | FLUSH | cy+0x89 | state 0 | ESC=3 |
| Selection highlight | RenderFrontendDisplayModeHighlight 0x4263e0 | FLUSH (decoupled) | selected slot | active | |
| Mouse cursor | LOOP queue | FLUSH (decoupled) | mouse XY | | |

Primed contract globals: `g_currentScreenIndex` (header), `g_lobbyErrorDialogSurface` (0x208×200
car/track panel), `g_frontendEscKeyButtonIndex`=3, `g_quickRaceSelectedTrackId` (the car id, ◄►
cycled), `g_selectedScheduleIndex` (the track id, ◄► cycled), `g_selectedCarIndex` /
`g_player1SelectedPaintScheme=0` / `g_player1SelectedWheelScheme=0` (committed on OK/Back),
`g_returnToScreenIndex`, `g_frontendSelectedGameType=-1`. Arrows set slot flags|=2 via
InitializeFrontendDisplayModeArrows 0x426260 (◄► sprite from g_browserSelectionBarSprite).

Animation: state 3 slide-in 4 button slots + header + panel, commit anim==0x27 (held 0x26 +
3-frame gate). State 6 slide-out, commit anim==0x10(16) → release surfaces → SetFrontendScreen.

Conditional elements:
- **Unlock clamping (state 0)**: `iVar4` = unlock ceiling = `g_savedMaxUnlockedCar` normally; =0x20
  (32) when `DAT_00463e6d`!=0 (all-cars cheat partial); car/track ids clamped to it. With
  `g_cheatPostRaceHighScoreUnlock` set the ceiling becomes 0x24 (36) / 0x21 (33) wrap bounds.
- **Locked-item gating**: a locked car (`g_savedCarLockTable[carId]`) or locked track
  (`g_packedConfig_displayPrefsBlock[track]`) draws a lock message AND blocks OK (state 4 btn2:
  plays SoundEffect 10 = error buzzer and returns without committing) — unless
  `g_cheatPostRaceHighScoreUnlock`!=0.
- ◄► wrap bounds differ by cheat: no-cheat wraps at g_savedMaxUnlockedCar / g_savedMusicTrackIndex;
  cheat wraps at 0x20/0x24 (cars) and 0x12/0x13 (tracks).

Input dispatch (state 4):
- `g_postRaceRacerCardNavDirection`!=0 (◄/►): if btn0 → car id += dir (wrap), rebake top half of
  panel; if btn1 → track id += dir (wrap), rebake bottom half.
- `g_frontendButtonPressedFlag`: btn2(OK) → if locked&!cheat play SoundEffect 10 + return; else
  commit car/paint/wheel, return=~LOCALIZATION_INIT (sentinel → InitializeRaceSeriesSchedule +
  InitializeFrontendDisplayModeState = start race), →state5. btn3(BACK) → commit car, return=
  MAIN_MENU(5), →state5. ESC=btn3.
- State 6: if return==~LOCALIZATION_INIT → InitializeRaceSeriesSchedule + InitializeFrontendDisplay
  ModeState (race); else SetFrontendScreen(return).

Confidence: [CONFIRMED @0x004213d0 full body]. [UNCERTAIN: `DAT_00463e6d`/`g_cheatAttractMode
Override_PROVISIONAL` exact cheat semantics (all-cars vs all-tracks) not separately resolved;
the format string `g_quickRaceMenuFormatPrefix` feeds sprintf for both car and track names — the
selecting field (car vs track) is implied by which BltColorFillToSurface y-band is cleared (0 vs
0x78), decompiled literally above.]

---

### Screen 8 @0x00418d50 — RunFrontendConnectionBrowser  [interactive Y] — LIST BROWSER

Inner states (in-body switch): 0 init (computer name, ConnectionEnumerate), 1 build buttons +
list-panel surface, 2 slide-in (anim→0x14=20), 3 first list paint + ready-gate, 4 second list
paint + force btnIndex=1, 5 INTERACTIVE (button press: OK/Back), 6 LIST NAVIGATION (up/down/mouse
cursor row-hit), 8 exit settle, 9 slide-out → release / branch, 10 commit → SetFrontendScreen
(SESSION_PICKER 9), 0xb cross-fade → SetFrontendScreen(MAIN_MENU 5).

ELEMENTS:
| element | produced by (fn/global/asset) | INLINE/FLUSH | dest pos | gating | notes |
|---|---|---|---|---|---|
| Full-screen background | LoadTgaToFrontendSurface16bpp(MainMenu.tga); CopyPrimaryFrontendBufferToSecondary; BlitFrontendCachedRect | INLINE | (0,0) 640×480 | state 0 | |
| Header label "CHOOSE CONNECTION" | CreateMenuStringLabelSurface(5)→g_currentScreenIndex; QueueFrontendOverlayRect anim X | FLUSH | header band | all | |
| List-panel button (big) | CreateFrontendDisplayModeButton(SNK_ChooseConnectionButTxt,-0x1f0,cy-0x2f,0x1f0,0x80)→g_postRaceNameButtonSurfacePtr | FLUSH | cy-0x2f, 0x1f0×0x80 | state 1 | slot 0 = the list container |
| **List rows (connection names)** | BltColorFillToSurface(panel rect 0x20,iVar,0x1b0,0x5e) then DrawFrontendClippedStringToSurface(dpu_exref + (scrollOffset+row)*0x3c, x=0x20, y, panel, clip 0x1b0) for up to 4 rows | INLINE bake (into the slot-0 panel surface → then FLUSH-drawn) | rows at y=base+0x10*(row+1) inside panel | states 3/4/5/6 | row count = min(4, *(dpu_exref+0x1e0)); stride 0x3c=60 per connection record |
| **Up-scroll indicator ▲** | QueueFrontendOverlayRect(slot0.x+0xf2, slot0.y+0x1e, …,0xc,0xc,0xff0000, g_browserScrollIndicatorSprite) | FLUSH | (x+0xf2, y+0x1e) 12×12 | scrollOffset!=0 | states 5/6 |
| **Down-scroll indicator ▼** | QueueFrontendOverlayRect(slot0.x+0xf2, slot0.y+0x6c, src y 0xc,0xc,0xc,0xff0000, scrollIndicatorSprite) | FLUSH | (x+0xf2, y+0x6c) 12×12 | scrollOffset+4 < count | states 5/6 |
| **List selection bar** (row cursor) | QueueFrontendOverlayRect(slot0.x+0x10, (cursor-scroll)*0x10+0x2f+slot0.y, src y 0x1b,0xc,9,0xff0000, g_browserSelectionBarSprite) | FLUSH | per-row | state 5 always; state 6 blinks (anim&0x10) | the row highlight (distinct from the button edge-bar highlight) |
| OK button | CreateFrontendDisplayModeButton(SNK_OkButTxt,-0x1f0,cy+0x89,0x60,0x20) | FLUSH | cy+0x89 | state 1 | slot 1 |
| BACK button | CreateFrontendDisplayModeButton(SNK_BackButTxt,-0x1f0,cy+0x89,0x70,0x20) | FLUSH | cy+0x89 | state 1 | slot 2, ESC=2 |
| Button selection highlight | RenderFrontendDisplayModeHighlight 0x4263e0 | FLUSH (decoupled) | selected slot | active | |
| Mouse cursor | LOOP queue | FLUSH (decoupled) | mouse XY | | |

Primed contract globals: `g_currentScreenIndex` (header), `g_postRaceNameButtonSurfacePtr_PROVISIONAL`
(the list-panel surface = slot 0), `g_connBrowserCursorIndex` @0x004970a8 (selected row),
`g_connBrowserScrollOffset` @0x00497038 (top visible row), `g_connBrowserRedrawDirty` @0x0049730c
(alternating colour/redraw latch — even→bg 0x392152 light, odd→0 dark), `g_frontendEscKeyButtonIndex`=2,
`g_returnToScreenIndex`, `g_connBrowserListOriginX_PROVISIONAL[0]` (read for the row/scroll/cursor
sprite positions — origin_x/origin_y/select_progress fields). Connection list source =
`dpu_exref` array: count at +0x1e0, records stride 0x3c (60), picked index written to +0x1e4.

Animation: state 2 slide-in 3 button slots, commit anim==0x14(20)+SoundEffect 4. State 6 row-cursor
blinks via (anim & 0x10). State 9 slide-out commit anim==0x18(24). State 0xb cross-fade via
AdvanceCrossFadeTransition (LoadTgaToFrontendSurface16bppVariant at anim==1).

Conditional elements:
- **Computer-name seed (state 0)**: GetComputerNameA → uppercased; if empty falls back to copying
  "Clint Eastwood" (s_Clint_Eastwood_00465f84) into g_localComputerName.
- Scroll indicators only when off-top / off-bottom (scrollOffset!=0 / scrollOffset+4<count).
- State 6 list-cursor only blinks while the cursor row is within the 4 visible rows AND
  (anim&0x10)!=0.
- Row count capped at 4 (`if (3<iVar9) iVar9=4`).

Input dispatch:
- State 5 (button mode): btn0(the list panel) press → state6 (enter list-nav). btn1(OK) →
  return=9 (SESSION_PICKER), state8. btn2(BACK) → return=5 (MAIN_MENU), state8. ESC=2.
- State 6 (list-nav): edge bit 0x200 (up) → cursor--, adjust scroll, SoundEffect 2; edge bit 0x400
  (down) → cursor++, adjust scroll; mouse press → row-hit math from (mouseY - slot0.origin_y) maps
  to row 0..4 (row0 = scroll up, row5 = scroll down, mid = set cursor); leaving list (btnIndex!=0
  with mouse) → back to state5. Each nav re-bakes the rows (BltColorFillToSurface + ClippedString
  loop) and bumps redrawDirty. On press in mid: cursor committed.
- State 9 branch: if return==5 → release header surf, state 0xb (cross-fade to main menu); else
  state10 → write picked index to dpu+0x1e4 → SetFrontendScreen(SESSION_PICKER).

Confidence: [CONFIRMED @0x00418d50 full body; g_connBrowserCursorIndex @0x004970a8,
g_connBrowserScrollOffset @0x00497038, g_connBrowserRedrawDirty @0x0049730c,
g_connBrowserListOriginX_PROVISIONAL @0x00499c78 stride 0x34]. [UNCERTAIN: `dpu_exref` is the DXPlay
connection-list block; +0x1e0=count, +0x1e4=picked-index, record stride 0x3c — field meaning beyond
the displayed name string not resolved (ARCH-DIVERGENCE: DXPTYPE net layer).]

---

### Screen 9 @0x00419cf0 — RunFrontendSessionPicker  [interactive Y] — LIST BROWSER

Inner states (in-body switch): 0 ConnectionPick + reset cursor/scroll, 1 build buttons + first
RenderFrontendSessionBrowser, 2 slide-in (anim→0x20=32) + force btnIndex=1, 3 INTERACTIVE (button
press: OK=join / New / Back), 4 LIST NAVIGATION (up/down/mouse), 6 exit settle, 7 slide-out →
SetFrontendScreen. (Note: states 5 absent; the body falls through to the common
`g_connBrowserScrollOffset = uVar1` tail.)

ELEMENTS:
| element | produced by (fn/global/asset) | INLINE/FLUSH | dest pos | gating | notes |
|---|---|---|---|---|---|
| Header label "CHOOSE SESSION" | g_currentScreenIndex (inherited from screen 8's CreateMenuStringLabelSurface(5)); QueueFrontendOverlayRect anim X | FLUSH | header band | all | screen 9 does NOT rebuild it — reuses g_currentScreenIndex |
| List-panel button (big) | CreateFrontendDisplayModeButton(SNK_ChooseSessionButTxt,-0x1f0,0,0x1f0,0x80)→g_postRaceNameButtonSurfacePtr | FLUSH | slot 0 | state 1 | the session-list container |
| **List rows (sessions)** | RenderFrontendSessionBrowser 0x00419b30: per row (≤4) DrawFrontendClippedStringToSurface ×3 — session name (dpu+rec*0x84+0x164, x=0x20 clip 0xa0), host name (+0x1a0, x=0xc6 clip 0xa0), game-type label (SNK_GameTypeTxt + typeIdx*0x10, x=0x16c clip 0x3c), and player count "d/d" via sprintf (s__d__d_00465f94, x=0x1b2 clip 0x24). Row 0 special = "NEW SESSION" (SNK_NewSessionTxt) | INLINE bake (into slot-0 panel) → FLUSH-drawn | rows inside panel | states 1/2/3/4 | record stride 0x84 (132); count = *(dpu_exref+0xbfc)+1 |
| **Up/Down scroll indicators** | QueueFrontendOverlayRect(slot0.x+0xf2, …, g_browserScrollIndicatorSprite) | FLUSH | (x+0xf2, y+0x1e / y+0x6c) 12×12 | scrollOffset!=0 / scrollOffset+4<count+1 | states 3/4 |
| **List selection bar** | QueueFrontendOverlayRect(slot0.x+0x10, (cursor-scroll)*0x10+0x2f+slot0.y, src y 0x1b,0xc,9,0xff0000, g_browserSelectionBarSprite) | FLUSH | per-row | state 3 always; state 4 blinks (anim&0x10) | row highlight |
| OK / New button | CreateFrontendDisplayModeButton(SNK_OkButTxt,-0x60,0,0x60,0x20) | FLUSH | slot 1 | state 1 | |
| BACK button | CreateFrontendDisplayModeButton(SNK_BackButTxt,-0x70,0,0x70,0x20) | FLUSH | slot 2 | state 1 | ESC=2 |
| Button selection highlight | RenderFrontendDisplayModeHighlight 0x4263e0 | FLUSH (decoupled) | selected slot | active | |
| Mouse cursor | LOOP queue | FLUSH (decoupled) | mouse XY | | |

Primed contract globals: `g_postRaceNameButtonSurfacePtr_PROVISIONAL` (session-list panel = slot 0),
`g_connBrowserCursorIndex` (selected session row), `g_connBrowserScrollOffset` (top row),
`g_connBrowserRedrawDirty` (colour/redraw latch), `g_frontendEscKeyButtonIndex`=2,
`g_returnToScreenIndex`, `g_currentScreenIndex` (header, inherited). Session list source =
`dpu_exref`: count-1 at +0xbfc, dirty/refresh flag at +0xc04 (RenderFrontendSessionBrowser clears
it), record stride 0x84 (132) from base +0x164, chosen session index written to +0xc00.
SNK_GameTypeTxt table + SNK_NewSessionTxt are the per-row labels.

Animation: state 2 slide-in 3 button slots by anim-derived X/Y, commit anim==0x20(32) +
SoundEffect 4 + DeactivateFrontendCursorOverlay. State 4 row-cursor blinks (anim&0x10). State 7
slide-out, commit anim==0x18(24) → release surfaces → SetFrontendScreen(return). RenderFrontendSession
Browser re-runs whenever redrawDirty&1 OR the net-refresh flag *(dpu+0xc04)!=0 (sessions arriving).

Conditional elements:
- **Row 0 = "NEW SESSION"** (SNK_NewSessionTxt) always; data rows are real enumerated sessions.
- Live session refresh: the list re-renders when DXPlay reports new sessions (dpu+0xc04) — the only
  one of the 5 screens whose list mutates from an external (network) source mid-frame.
- Scroll/selection-bar gating identical to screen 8 but count uses +0xbfc (count-1, so +1 in tests).

Input dispatch:
- State 3 (button mode): btn0(list panel) → state4 (list-nav). btn1(OK/Join) → write chosen index
  (cursor-1) to dpu+0xc00, return=CREATE_SESSION(10), DXPlay::EnumSessionTimer(0), state6.
  btn2(BACK) → return=CONNECTION_BROWSER(8), EnumSessionTimer(0), state6. ESC=2.
- State 4 (list-nav): edge 0x200 up → cursor--, scroll adjust; edge 0x400 down → cursor++, scroll
  adjust, SoundEffect 2; mouse press → row-hit math (mouseY - slot0.origin_y) → row 0..4 (row0
  scroll-up, row5 scroll-down, mid set cursor); btnIndex!=0 w/ mouse → back to state3.
- State 7: SetFrontendScreen(return); if return==CONNECTION_BROWSER release the header surf first.

Confidence: [CONFIRMED @0x00419cf0 + RenderFrontendSessionBrowser @0x00419b30 full bodies; same
browser globals as screen 8]. [UNCERTAIN: `dpu_exref` field map (+0xbfc count-1, +0xc00 chosen,
+0xc04 refresh, record stride 0x84 with name@+0x164 / host@+0x1a0 / gametype@+0x1e4) is the DXPlay
session block — beyond display strings, semantics are the net layer (ARCH-DIVERGENCE: DXPTYPE).]
