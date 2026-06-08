# TD5 Frontend Flow / Dispatch / State / Persistence — Behavior Model

RE target: `TD5_d3d.exe` (image base 0x00400000). Source: literal Ghidra decompilation of the
functions listed in `frontend_call_graph_closure.md` sections 2, 7, 8, 9. Every claim carries the
Ghidra virtual address that produced it. Values are hex AND decimal.

Scope decompiled here: 30 functions (5 main-loop/FSM + 7 string/state utils + 6 persistence + 12
controller-binding handlers/wrapper-fragments). Jump tables read from memory: 3
(0x00410c84, 0x00417cd4, 0x00418390) + the cup-schedule table 0x00464108.

---

## PART 1 — MAIN LOOP (`RunFrontendDisplayLoop` @0x00414b50)

Decompiled per-frame order, top to bottom (literal):

1. **Surface-lost check.** Reads DDraw surface via `dd_exref+4`, calls vtable+0x60 (TestCooperativeLevel-equivalent). If it returns `-0x7789fe3e` (= 0x87760CC2, DDERR_SURFACELOST), calls vtable+0x6c (Restore), sets `g_frontendRedrawCount = 3`, and falls into the redraw path. If `g_frontendRedrawCount != 0`, runs `BlitFrontendCachedRect(0,0,0x280,0x1e0)` (= 640×480) and decrements the counter. This restores the cached background after a lost surface.
2. **Early-out #1:** `if (g_startRaceRequestFlag != 0) return;` — exit the loop to start a race (no input/draw this frame).
3. **Input poll (keyboard/stick):**
   - `g_frontendInputEdgeBits = ~(g_frontendInputPreviousBits & g_frontendInputCurrentBits);`
   - `g_frontendInputPreviousBits = g_frontendInputCurrentBits;`
   - `g_frontendInputCurrentBits = DXInputGetKBStick(0);`
   - `g_frontendInputEdgeBits = g_frontendInputEdgeBits & g_frontendInputPreviousBits & g_frontendInputCurrentBits;`
     This produces a **rising-edge** mask (bits that were 0 last frame AND are 1 this frame).
4. **Mouse poll:** `g_frontendMouseEdgeBits = ~*(g_appExref+0x108);` then `DXInput::GetMouse();` then `g_frontendMouseEdgeBits &= *(g_appExref+0x108);` (button rising edges). Sets `g_frontendMouseMovedFlag = 1` if the Manhattan distance `|prevX-curX| + |prevY-curY| > 8` OR any mouse-button edge fired. If `g_frontendMouseCursorEnabled == 1`, latches `g_frontendMousePrevX/Y` to the current cursor (g_appExref+0x104 = X, +0x100 = Y).
5. `UpdateFrontendClientOrigin();` (@0x00425170) — zeroes an 8-byte POINT pair in `g_cpuInfo.os_version_string[4..11]`; if `*(g_appExref+0x150)==0` (windowed) calls `ClientToScreen(*(HWND*)g_appExref, &point)` to track the client-area origin for cursor mapping.
6. **Screen tick (PRIME):** `(*g_currentScreenFnPtr)();` — the ONLY screen-function call per frame. (A source-port patch redirects this CALL to `LogicGate_ScreenDispatch` for widescreen; original = direct indirect call.) The screen fn *primes* state (queues overlay rects, moves sprites, advances its inner state); it does not itself blit the composited frame.
7. **Early-out #2:** `if (g_startRaceConfirmFlag != 0) return;` — screen fn requested race start mid-tick.
8. `NoOpHookStub();` (@0x00418450, empty).
9. **Cursor overlay queue:** `if (g_frontendCursorOverlayHidden == 0 && g_frontendMouseCursorEnabled == 1)` → `QueueFrontendOverlayRect(mouseX, mouseY, 0,0, 0x16, 0x1e, 0xff0000, g_frontendCursorTextureId)` (cursor sprite 22×30 px). NOTE the flag-sense inversion documented in Part 5.
10. `RenderFrontendUiRects();` (@0x00425a30) — walks the 3 button/rect slot arrays and emits a `QueueFrontendOverlayRect` per visible entry, offsetting the selected one (`g_frontendButtonIndex`).
11. `FlushFrontendSpriteBlits();` (@0x00425540) — **THE DRAW PASS.** Consumes the overlay double-buffer, blits queued rects into `g_primaryWorkSurface`, then unconditionally runs `UpdateExtrasGalleryDisplay` + `RenderFrontendDisplayModeHighlight`, then the cursor-tracked sprite list. **This is why screen fns only PRIME and the flush DRAWS** — the screen fn at step 6 ran *before* this and merely populated the queues that this step consumes.
12. **Present:**
    - `if (g_frontendHardwareFlipEnabled == 0)`: call DDraw vtable+0x58 (Blt) `(*dd,1,0)` then `PresentFrontendBufferSoftware();`
    - `else`: `DXDraw::Flip(1);` (source-port-patched to a widescreen scale cave), then lock/unlock `g_frontendBackSurfacePtr` (vtable+100 Lock, vtable+0x80 Unlock) for the SNK_ScreenDump path; on Lock failure logs `Msg("Lock failed in SNK_ScreenDump...")` and skips the toggle.
    - `g_frontendFrameToggle ^= 1;` (selects which 0x410-byte overlay bank the next flush reads).
13. `UpdateFrontendDisplayModeSelection();` (@0x00426580) — post-present selection/highlight animation advance.
14. **ESC → button:** `if (DXInput::CheckKey(1) != 0 && g_frontendEscKeyButtonIndex != -1)` → `g_frontendButtonIndex = g_frontendEscKeyButtonIndex; g_frontendButtonPressedFlag = 1;` (scan code 1 = DIK_ESCAPE). This maps ESC onto whichever button the current screen registered as its escape target.
15. **Cheat-code FSM (OptionsHub only):** `if (g_currentScreenFnPtr == PTR_ScreenOptionsHub_004655f4)`: loops 6 cheat slots; each slot has a 0x28-stride scan-code sequence (`g_cheatCodeKeySequences`) and a per-slot progress byte (`g_cheatCodeKeyProgress`). On the matching `DXInput::CheckKey`, advances progress; when the next sequence byte == -1 (terminator) the code completes: XORs `*g_cheatCodeTargetPointers[slot]` with `g_cheatCodeXorMaskTable[slot]`, plays sound 5 (now off) or 4 (now on), resets progress, and walks `g_npcRacerGroupTable` (stride 0xa4, up to 0x464fe4) setting/clearing bit in `gNpcRacerCheatFlags` (clear→ `&1`, set→ `|2`). Else (not on OptionsHub): resets cheat progress to 0.
16. `g_frontendAnimFrameCounter += 1;` (the universal per-screen animation clock; source port NOPs the original INC and re-adds it in the logic gate).
17. **Idle → attract mode:** `if (g_currentScreenFnPtr == PTR_ScreenMainMenuAnd1PRaceFlow_004655d8 && (g_frontendFrameTimestamp_ms = timeGetTime(), now - g_frontendInnerState > 50000) && g_extrasGalleryCrossFadePhase < -0xf)`: pick a random attract track `g_attractModeTrackIndex = rand() % 0x13` (mod 19) skipping disabled entries in `g_packedConfig_displayPrefsBlock`, set `g_frontendScreenTransitionFlag=1`, reset inner state + anim counter, set `g_currentScreenFnPtr = PTR_RunAttractModeDemoScreen_004655cc`, re-stamp `g_frontendInnerState = now`, `g_frontendBootDispatchMode = 1`. Idle threshold = **50000 ms (50 s)** on the main menu.

[UNCERTAIN] `g_frontendInnerState` is reused both as a screen-enter `timeGetTime()` timestamp (set by SetFrontendScreen / InitializeFrontendResourcesAndState) AND, within screen fns, as the per-screen integer inner-state index. The loop's attract-idle math at step 17 treats it as a ms timestamp; the screen fns treat it as a small enum. Same global symbol, two consumers; the timestamp role only survives on the main-menu screen which never overwrites it with a small state value. Missing evidence: a single decompile that disambiguates the two roles in one frame.

---

## PART 2 — SCREEN FSM, TRANSITIONS, INNER-STATE + ANIMATION MODEL

### Screen pointer + transition
- `SetFrontendScreen(index)` @0x00414610 (literal, full body): `g_frontendInnerState = 0; g_frontendAnimFrameCounter = 0; g_currentScreenFnPtr = (&g_frontendScreenFnTable)[index]; g_frontendInnerState = timeGetTime();`
  - i.e. it resets the inner-state enum to 0, zeroes the animation clock, points the loop at the new screen-table entry (table @0x004655C4, 30 LE u32 ptrs), then re-stamps inner-state with the current ms time. **It does NOT release surfaces or reset selection here** — the closure's prose to that effect is not in this function body; surface release happens inside each screen's own exit state (e.g. ScreenControllerBindingPage state 0xb/0x1b call `ReleaseTrackedFrontendSurface` then `SetFrontendScreen`). The final `g_frontendInnerState = timeGetTime()` overwrites the 0 it just wrote (so the new screen sees inner-state = a large ms value, not 0, on its first frame — relevant to the idle-timestamp role above).

- `InitializeFrontendDisplayModeState` @0x00414a90: rebuilds the filtered display-mode list (`BuildEnumeratedDisplayModeList`), writes `gConfiguredDisplayModeOrdinal` into the list head+4 and into `gSelectedDisplayModeOrdinal`, calls `WritePackedConfigTd5()` (persists Config.td5), seeds the 2-player particle slot if `g_twoPlayerModeEnabled`, and — when `g_startRaceRequestFlag==0 && g_frontendInitInProgressLatch==1` — releases all tracked surfaces, sets `g_startRaceConfirmFlag=1` (loop will early-return to race), and picks a random CD attract track (`g_selectedCdTrackIndex = (rand()&0x80000007 normalized)+4`) avoiding repeats and certain track/level pairings (candidate 2→avoid 9, candidate 3→avoid 0xb). Returns 1. This is the screen-leave → start-race bookkeeping bridge.

### One-time init
- `InitializeFrontendResourcesAndState` @0x00414740 (literal order):
  1. `timeGetTime()` → `__set_new_handler` (seeds a timing global); `g_frontendRedrawCount=0`.
  2. If `g_inputPlaybackActive` clear it; else `SerializeRaceStatusSnapshot()` (snapshots race status before entering the menu). `g_postRaceSkipResultsBanner = playbackActive`.
  3. Clears `g_carSelectChampionshipReturnFlag`, `g_postRaceRestartSelectedRace`; `NoOpHookStub()`.
  4. Sets latches: `g_trackedFrontendSurfaceListHead=1`, `g_startRaceRequestFlag=0`, `g_frontendFrameToggle=0`, `g_frontendInitInProgressLatch=1`, `g_frontendScreenTransitionFlag=1`, `g_frontendHardwareFlipEnabled=1` (+Alt), `g_extrasGalleryEnabledFlag=0`, `g_frontendBackSurfacePtr = *(dd_exref+8)`, `g_attractCdTrackPickDone=0`.
  5. `DXSound::CDSetVolume((g_persistedCdVolumePercent*0xfc00)/100 & 0xfc00);`
  6. Installs `*(g_appExref+300) = InitializeFrontendDisplayModeState` (the resize/mode-change callback).
  7. `ClearFrontendSurfaceRegistry();` Reads back-buffer bit depth `g_frontendSurfaceBitDepth = *(dd_exref+0x16a0)`; selects TGA decode masks for 15/16/other bpp.
  8. `g_frontendCanvasW=0x280 (640); g_frontendCanvasH=0x1e0 (480);` `BuildFrontendDitherOffsetTable();`
  9. `ResetFrontendOverlayState(); ResetFrontendSelectionState(); ResetFrontendInlineStringTable();`
  10. `g_frontendInnerState=0; g_frontendAnimFrameCounter=0; g_currentScreenFnPtr = g_initialScreenFnPtr; g_frontendInnerState = timeGetTime();`
  11. `g_primaryWorkSurface = CreateTrackedFrontendSurface(640,480);` `g_secondaryWorkSurface = same;` `InitializeFrontendPresentationState(); DXDraw::ClearBuffers(); ActivateFrontendCursorOverlay(); g_frontendMouseCursorEnabled=1;`
  12. Loads the shared frontend sprite sheets via `LoadFrontendTgaSurfaceFromArchive(... , "Front End\\FrontEnd.zip")`: ButtonBits.tga (DAT_00496268), ArrowButtonz.tga, ArrowExtras.tga, snkmouse.tga (→ g_frontendCursorTextureId), body text font (BodyText.tga if LANGUAGE.DLL byte[8]==0x30 else SmallText2.tga), SmallText.tga, SmallTextb.tga, MenuFont.tga; `g_smallFontSurface = g_smallTextSurface`.
  13. `LoadFrontendSoundEffects(); LoadExtrasGalleryImageSurfaces();` resets cheat progress.
  14. If `g_attractCdTrackPickDone==0`: set it 1, pick random CD track `rand()%7` (avoiding repeats/certain pairs), `DXSound::CDPlay(track+2, 1)`. This boots the menu music.

### Tick/ready gate
- `AdvanceFrontendTickAndCheckReady` @0x004259b0 (literal): `g_frontendDisplayModePreviewCount_PROVISIONAL += 1; return (g_frontendDisplayModePreviewCount_PROVISIONAL > 2);` — a 3-frame settle gate. Used e.g. by ScreenMusicTestExtras state 3 to hold the slide-in one extra beat before committing the static state.

### Universal inner-state pattern (literal, from idx18 / idx19 / idx6 / idx22 bodies)
Every screen fn is `switch(g_frontendInnerState){…}` with this canonical shape (concrete states differ per screen but the roles recur):
- **state 0 = init/load.** Loads MainMenu.tga background, `CopyPrimaryFrontendBufferToSecondary()`, bakes label surfaces (`CreateMenuStringLabelSurface`), creates tracked panel surfaces (`CreateTrackedFrontendSurface`), fills them (`BltColorFillToSurface`), builds buttons (`CreateFrontendDisplayModeButton` / `InitializeFrontendDisplayModeArrows`), sets `g_frontendEscKeyButtonIndex`, then sets `g_frontendInnerState` to the slide-in state and returns. (idx18 state 0 ends by setting inner-state 9; idx19 state 0 sets 1.)
- **states 1/2 = present/settle.** `PresentPrimaryFrontendBufferViaCopy()`, reset `g_frontendAnimFrameCounter=0`, `++inner-state`.
- **state(s) slide-in (idx18 = 9; idx19 = 3; idx6 = caseD_7; idx22).** Each frame re-queues the header label overlay rect at an X offset that is a linear function of `g_frontendAnimFrameCounter` (e.g. idx18 state 9: `x = (cx-0x368) + anim*0x18`), and calls `MoveFrontendSpriteRect(slot, x(anim), y(anim))` to slide panels in. When `g_frontendAnimFrameCounter == 0x1c (28)` (idx18) / `0x20 (32)` / `0x27 (39)` it commits: `DeactivateFrontendCursorOverlay()`, `++inner-state`. The slide is therefore a fixed-length **timed animation driven solely by g_frontendAnimFrameCounter** (incremented once per frame by the loop), NOT by elapsed ms.
- **state(s) static/interactive (idx18 = 10/0x1a; idx19 = 4/5/6).** 4/5 re-queue the static panels and `++inner-state` (a 2-frame double-present); 6 is the **interactive** state: reads `g_frontendButtonPressedFlag`, `g_frontendButtonIndex`, `g_postRaceRacerCardNavDirection`, and `DXInput::GetJS`/`CheckKey` to act on input, redraw, or transition.
- **state(s) slide-out (idx18 = 0xb/0x1b; idx19 = 8; idx6 = caseD_a; idx22).** Mirror of slide-in: header label X = `… + anim*-0x18`, panels slide off-screen; when `g_frontendAnimFrameCounter` hits the terminal value (`0x1c`/`0x20`/`0x23`) it `ActivateFrontendCursorOverlay()`, releases all tracked surfaces (`ReleaseTrackedFrontendSurface` ×N, `ReleaseFrontendDisplayModeButtons`), and calls `SetFrontendScreen(returnIndex)` — the **exit** action.
- The closure's "0=init,1/2=present,3=slide-in,4/5=static,6=interactive,7/8=slide-out,9=exit" template matches idx19 (ScreenMusicTestExtras) almost exactly (it has states 0..8 + the exit folded into state 8). Other screens renumber but keep init→present→slide-in→static→interactive→slide-out→exit.

### Animation model summary
- `g_frontendAnimFrameCounter`: free-running per-frame counter, zeroed by SetFrontendScreen and at each state transition by the screen fn. All slide positions are `base ± counter*step`; transitions fire on exact counter equality (28/32/39/35 px-frames). No real-time interpolation — purely frame-count driven, so animation speed = frame rate.
- `MoveFrontendSpriteRect(slot, x, y)` (@0x004259d0) repositions a sprite slot; called repeatedly with counter-derived coordinates to produce the slide. `QueueFrontendOverlayRect` re-emits the header/panel each frame at the animated offset.

---

## PART 3 — INPUT ROUTING

How the loop's polled state reaches the current screen (all consumed inside the screen fns):

- **Button index / selection — `g_frontendButtonIndex`**: written by `RenderFrontendUiRects` selection logic, by the ESC handler (step 14: set to `g_frontendEscKeyButtonIndex`), and reset by `ResetFrontendSelectionState` (→ 0). Read by screen fns to branch on which button is active (idx18 state 0xa / idx19 state 6 check `g_frontendButtonIndex == 0` for OK/Select).
- **Press latch — `g_frontendButtonPressedFlag`**: set to 1 by the ESC handler (step 14) and by the selection/cursor update; read by screen fns as the "confirm this frame" edge; reset to 0 by `ResetFrontendSelectionState`. Pattern in every interactive state: `if (g_frontendButtonPressedFlag != 0 && g_frontendButtonIndex == N) { … }`.
- **Arrow / cycle direction — `g_postRaceRacerCardNavDirection`**: reset to 0 by `ResetFrontendSelectionState`. Read by screen fns as the ◄/► cycle delta (idx19 state 6: `g_selectedCdTrackIndex += g_postRaceRacerCardNavDirection`, wrapping 0..0xb). When nonzero the screen treats the frame as an arrow-cycle rather than a confirm.
- **Escape mapping — `g_frontendEscKeyButtonIndex`**: each screen writes the button index that ESC should activate (idx19 state 0 sets it to 1 = the OK/back button); reset to -1 (0xffffffff) by `ResetFrontendSelectionState`. The loop (step 14) reads it after present: if ESC (DIK scancode 1) is down and it's not -1, it forces `g_frontendButtonIndex` to it and sets the press latch — so ESC is funneled through the same button path the screen already handles.
- **Cursor / mouse**: `g_frontendMouseCursorEnabled` gates whether the cursor sprite is queued (step 9) and whether prevX/Y latch (step 4). `g_frontendMouseMovedFlag` distinguishes mouse navigation from keyboard. `g_frontendCursorOverlayHidden` gates both the cursor sprite (loop) and the selection-highlight bars (`RenderFrontendDisplayModeHighlight`). Mouse position lives at `g_appExref+0x104` (X) / `+0x100` (Y); buttons at `+0x108`.
- **Controller-binding capture** (idx18, see Part 4): polls `DXInput::GetJS(deviceIndex-1)` into `g_controllerBindingCurrentButtons` and computes its own prev/edge masks; for the keyboard re-bind sub-flow (states 0x14/0x19/0x1a) it scans all 256 scancodes via `DXInput::CheckKey(code)` and writes the first un-used pressed code into `g_keyboardScanCodeTable` at offset `g_keyboardBindingProgressIndex` (10 actions, indices 0..9).

---

## PART 4 — POINTER-DISPATCHED HANDLERS (the zero-direct-caller fragments)

These have no direct call edge because they are **switch-case bodies reached through `JMP DWORD PTR [reg*4 + table]` computed jumps**; Ghidra split them into standalone "functions" living in the address gap below the owning screen fn. There are THREE such jump tables plus one true data-pointer dispatch table.

### 4A. Controller-binding page jump table @0x00410c84 (owner: ScreenControllerBindingPage @0x0040fe00, idx18)
- Mechanism: inside the idx18 state-0xa render loop, the computed jump at **0x00410435** indexes table @0x00410c84 by the per-row up/down-binding bitmask (`local_28` / `in_stack_00000018`, values 0..3). Table read from memory (5 LE u32):
  | idx | target | handler | behavior |
  |---|---|---|---|
  | 0 | 0x00410129 | OpenControllerBindingPageNoneHeader | if device hi-byte==0x600 draws localized "none" string; creates OK button (SNK_OkButTxt, -0x128,0,0x60,0x20); sets inner-state 9 |
  | 1 | 0x0041043c | RenderControllerBindingPageUpDownHeader | draws up/down localized header + fills, queues 0x1c0×0xd8 + 0x1c0×0x40 panels, runs the binding-edit tail |
  | 2 | 0x004104b2 | RenderControllerBindingPageDownHeader | same tail, "down" header |
  | 3 | 0x00410527 | RenderControllerBindingPageUpHeader | same tail, "up" header |
  | 4 | 0x00410599 | RenderControllerBindingPageBlankOrRearViewHeader | hi-byte==0x600 → localized string, else blank fill; same tail |
  - The shared **binding-edit tail** (present in 0x41043c/4b2/527/599 and inlined in the idx18 body): `g_controllerBindingEdgeMask &= g_controllerBindingCurrentButtons;` then either (count==2) swap `g_controllerBindings_current[slot*9]` ↔ `g_controllerBindingsCache[slot*9]` on button 0x40000/0x80000 edges, or (count!=2) for each of `g_controllerBindingButtonCount` rows: on the row's button rising edge increment `g_controllerBindingsCache[row + slot*9]`, wrapping `>10 → 2` (cycles the action assigned to that physical button). On `g_frontendButtonPressedFlag && g_frontendButtonIndex==0` → `g_frontendAnimFrameCounter=0; ++g_frontendInnerState` (commit, advance to slide-out).
- **Additional ptr-reached idx18 fragments** (reached by fall-through/secondary computed jumps, not in the 5-entry table but listed in §9):
  - 0x004100c0 OpenControllerBindingPageWrapper — `DrawFrontendLocalizedStringToSurface(); OpenControllerBindingPageNoneHeader();`
  - 0x004100ce DrawControlBindingTextWithOkButton — small switch(EDI 0..3) selecting whether to draw a localized line, then creates OK button + sets inner-state 9.
  - 0x004100de OpenControllerBindingPageRearViewHeader / 0x004100fa DrawControlBindingText1WithOkButton / 0x00410111 DrawControlBindingText2WithOkButton — each: draw one localized string, create OK button, set inner-state 9. (Three near-identical header variants for rear-view / wheel / pedal device classes.)
  - 0x00410380 RenderControllerBindingMenuPage — the full per-row render loop (header switch 0..3 + the binding-edit tail); this is the body the table-0..4 fragments are the heads of.
  - 0x00410613 RenderControllerBindingPageRows — the binding-edit tail as a standalone block (queues the two panels, runs the count==2 swap / count!=2 increment, checks confirm).
  - 0x00410940 DrawControlOptionsBindingHeader — `BltColorFillToSurface(0,0,0x18,0x1c0,0x18, g_controllerBindingPage_state)`; centers + draws `SNK_ControlText[g_keyboardBindingProgressIndex*0x10]` (the 10 action labels: LEFT/RIGHT/ACCELERATE/BRAKE/HANDBRAKE/HORN-SIREN/GEAR UP/GEAR DOWN/CHANGE VIEW/REAR VIEW); `++g_frontendInnerState`. Used by the keyboard re-bind header (idx18 state 0x19).
- idx18 device gating (state 0 body): if active device byte (`g_player1DeviceDesc[deviceIndex]`) == 3 → jump straight to keyboard-rebind state 0x13; else compute `g_controllerBindingButtonCount` (device<4→2, device 4..8→device value, ≥9→8) and route to the appropriate header handler by the present-bindings bitmask `uVar12` (bit0 if any cache slot==2, bit1 if any==3). Hi-byte 0x600 = wheel/pedal class (different header text path).

### 4B. Cup-schedule configurator table @0x00464108 (count byte 0x63=99 at 0x00464104)
- Mechanism: data table of 6 LE u32 function pointers, indexed by `g_frontendSelectedGameType` (the cup tier), called from `ConfigureGameTypeFlags` @0x00410ca0 (idx6/idx24). Table read from memory @0x00464108:
  | idx (tier) | target | handler | behavior |
  |---|---|---|---|
  | 1 Championship | 0x00410f60 | ConfigureCupChampionshipSchedule | if first-play flag set → currentRound=2; clear flag; if `g_savedMusicTrackIndex>0x11 && (cheatFlags&7)==0` set cheat bit 1 |
  | 2 Era | 0x00410fa0 | ConfigureCupEraSchedule | if tier flag → currentRound=2; clamp `g_savedMusicTrackIndex` to min 0x12 (18); clear flags |
  | 3 Challenge | 0x00410ff0 | ConfigureCupChallengeSchedule | if flag → currentRound=3; clear DAT_004668bc..be; if DAT_00463e5f==0 set cheat bit 2 |
  | 4 Pitbull | 0x00411030 | ConfigureCupPitbullSchedule | if flag → currentCup=5; clear era bytes; if DAT_004668be==0 set cheat bit 2 |
  | 5 Masters | 0x00411070 | ConfigureCupMastersSchedule | if flag → currentRound=2; clear DAT_004668b8/b9 |
  | 6 Ultimate | 0x004110a0 | ConfigureCupUltimateSchedule | if quickRaceByte → round=2; if DAT_00463e67 → currentCup=9; clear many mask bytes; `g_savedMusicTrackIndex=0x13 (19)`; unlock cars (`g_savedMaxUnlockedCar` to min 0x25=37) |
  (The 0x00464104 value `0x00000063` is the dword preceding the table; the 6 pointers begin at 0x00464108, matching the closure.)

### 4C. RaceTypeCategoryMenuStateMachine inner-state jump table @0x00417cd4 (owner: idx6 @0x004168b0)
- Mechanism: computed jump at **0x004168e2** = `JMP DWORD PTR [EAX*4 + 0x00417cd4]` indexed by inner-state (0..11). Table read from memory (12 LE u32): 0x004168e9, 0x00416a81, 0x00416beb, 0x00416c4b, 0x00416d80, 0x00416f16, 0x0041707a, **0x004173b1**, 0x004174ce, 0x004175bc, **0x00417700**, 0x00417918.
  - idx **7** = `caseD_7` @0x004173b1: slide-in animation — queues header, `QueueFrontendOverlayRect(panel 0x110×0xb4)`, slides 6 sprite slots by counter-derived Y; at `g_frontendAnimFrameCounter==0x20 (32)` plays sound 4 and `++inner-state`.
  - idx **10** = `caseD_a` @0x00417700: slide-out — header X = `… + anim*-0x18`; for anim<3 double-presents secondary↔primary, plays sound 5 at anim==1; for anim≥3 slides 7 sprite slots out; at `anim==0x23 (35)` resets inline strings, re-bakes small-text TGA, releases surfaces, `SetFrontendScreen(g_returnToScreenIndex)` (exit).

### 4D. ScreenExtrasGallery inner-state jump table @0x00418390 (owner: idx22 @0x00417d50)
- Mechanism: computed jump at **0x00417d65** = `JMP DWORD PTR [ECX*4 + 0x00418390]` indexed by inner-state (0..7). Table read from memory (8 LE u32): 0x00417d6c, 0x00417dac, 0x0041807f, 0x0041807f, 0x0041807f, 0x0041807f, **0x0041808d**, 0x004180b3.
  - idx **6** = `caseD_6` @0x0041808d: `LockSecondaryFrontendSurfaceFillColor(0); g_frontendAnimFrameCounter = 0x27f (639); ++g_frontendInnerState;` — primes a full-frame fade by locking+clearing the secondary surface.
  - 0x00423e40 `LockSecondaryFrontendSurfaceFillColor(color)`: converts `color` to the surface bit-depth (15/16 bpp masks) and calls secondary-surface vtable+0x14 (Blt/ColorFill) over (0,0,0,0x400). Leaf called only by caseD_6.

**CORRECTION to closure §9:** the closure labels caseD_6/7/a as "music-test idx19 switch cases." That is wrong. By Ghidra namespace + the computed-jump tables above: caseD_7 (state 7) and caseD_a (state 10) belong to **RaceTypeCategoryMenuStateMachine (idx6, table @0x00417cd4)**; caseD_6 (state 6) belongs to **ScreenExtrasGallery (idx22, table @0x00418390)**. ScreenMusicTestExtras (idx19 @0x00418460) uses a plain in-body `switch(g_frontendInnerState)` (states 0..8) with NO external jump-table fragments — its dynamic CD-track strings come from the `PTR_s_GRAVITY_KILLS_00465e1c[]` / `PTR_s_FALLING_00465e58[]` pointer arrays indexed by `g_selectedCdTrackIndex`, plus `SetFrontendInlineStringTable(SNK_MusicTest_MT, …)`.

---

## PART 5 — STATE / PERSISTENCE

### 5A. Inline-string table (dynamic string injection)
- `SetFrontendInlineStringTable(char *blob, a, b)` @0x004183b0: resets `g_frontendInlineStringFSM_state=0`, then walks `blob` as a sequence of NUL-terminated strings, storing each string pointer into successive slots of `g_frontendInlineStringEntryTable` (incrementing the FSM-state count per entry) until it sees a double-NUL or the table bound 0x4963f4. Sets `_DAT_00465e10 = 0xffffffff` (dirty/invalidate sentinel) and stores `a`→0x004963f8, `b`→0x004963fc. This is how a screen injects a run of dynamic strings (e.g. the music-test labels) in one call; the renderer later expands inline `%`-style references against this table.
- `SetFrontendInlineStringEntry(index, ptr)` @0x00418410: `g_frontendInlineStringEntryTable[index] = ptr; _DAT_00465e10 = 0xffffffff;` — replaces a single entry (used by idx5/14/20/21 to set e.g. the selected **track name** / car name into a fixed slot). This is the mechanism by which dynamic strings like track names are spliced into otherwise-static localized layouts.
- `ResetFrontendInlineStringTable` @0x00418430: `_DAT_00465e10 = 0xffffffff; g_frontendInlineStringFSM_state = 0;` — clears the table (called by INIT and by screens on exit, e.g. idx19 state 7, caseD_a).

### 5B. Config.td5 (`WritePackedConfigTd5` @0x0040f8d0)
- Triggered by: `InitializeFrontendDisplayModeState` (every display-mode commit / screen-mode change) and "many SCR / FSM" per closure §8.
- Assembles a packed blob (base 0x0048f384) from the live config globals, then **CRC32**s it (table `g_crc32LookupTable`, into `g_configTd5IoBuffer`/`_g_configTd5IoBuffer` = ~crc), opens `g_configTd5Filename` in write mode, **XOR-obfuscates** each byte with `g_configTd5XorKey[i] ^ 0x80` (key index wraps at key length), and `_fwrite`s the buffer.
- Fields packed (literal, partial): player1/2 input source (DAT_0048f3a4 b0/b1 ← g_player1/2InputSource), keyboard scancode table (g_keyboardScanCodeTable + DAT_00464058/5c), controller-binding scroll offset, 7-dword circuit-laps block (gCircuitLapsConfigShadow → g_configTd5GameOptionsCopy), CD volume %, SFX playback mode, master volume %, fog/units/camera-speed config shadows, configured display-mode ordinal (ram 0x0048f441), 0x12-dword controller bindings, 8-dword player1/2 device descs, 0x62-dword player1/2 custom bindings, music track index, 2-player split/catchup/camera views, 0x42a-dword NPC racer group table, cheat game-mode bitfield (`&7`), max-unlocked-car, all-cars-unlocked flag, car-lock table, and a 0x1a-byte NPC cheat-flag mirror (`gNpcRacerCheatFlags[i] & 1`).

### 5C. CupData.td5 (`WriteCupData` @0x004114f0 / `LoadContinueCupData` @0x00411590 / `ValidateCupDataChecksum` @0x00411630)
- `WriteCupData`: triggered by RunRaceResultsScreen (idx24). Takes the current race-status snapshot blob (base `g_snapshotGameType`, length `g_snapshotPayloadSize`), XOR-obfuscates each byte with `g_cupDataXorKey[i] ^ 0x80` (key wraps), `_fwrite`s to `g_cupDataTd5Filename`. Returns 1 on full write.
- `LoadContinueCupData`: idx6 (race-type menu, "Continue"). Reads up to 0x4000 bytes from `g_cupDataTd5Filename`, de-XORs into the snapshot blob, sets `g_snapshotPayloadSize`, then calls `RestoreRaceStatusSnapshot()` to apply it. Returns its result (1 if CRC valid).
- `ValidateCupDataChecksum`: idx6. Reads + de-XORs the file, recomputes CRC32 over it, compares to the stored CRC (`uStack_3ff4` field). Returns 1 if match — used to gate the "Continue" option without committing the restore.

### 5D. Race-status snapshot (`SerializeRaceStatusSnapshot` @0x00411120 / `RestoreRaceStatusSnapshot` @0x004112c0)
- `SerializeRaceStatusSnapshot`: called by INIT (on entering the frontend, unless input-playback). Packs into the `g_snapshot*` block: race-within-series index, game type, schedule index, difficulty tier (`gRaceDifficultyTier`), attract track index, special-encounter type/enabled, circuit lap count, a 0x1e-dword DXP race-settings block, the 0x1e-dword `g_raceResults` table, a 0xc5c-dword actor-runtime block, a 6-entry slot-state table, and cup bookkeeping (p1/p2 schedule index, completion bitmask, masters-encounter flags). Then CRC32s 0x32a6 (12966) bytes → `g_snapshotCrc32`, sets `g_snapshotPayloadSize = 0x32a6`.
- `RestoreRaceStatusSnapshot`: called by idx6 + idx24. Recomputes CRC over `g_snapshotPayloadSize` bytes; if it matches `g_snapshotCrc32`, unpacks every field back into the live race globals (inverse of Serialize), including selected car/paint/wheel/transmission; if game-type byte == 0xff sets `g_frontendSelectedGameType = -1`. Returns 1 on valid restore, 0 otherwise.

### Flag-sense note (cursor overlay)
- `ActivateFrontendCursorOverlay` @0x004258c0 sets `g_frontendCursorOverlayHidden = 1`; `DeactivateFrontendCursorOverlay` @0x004258e0 sets it `= 0`. The loop queues the cursor when `g_frontendCursorOverlayHidden == 0`. So despite the symbol name, **1 = interactive/highlight-active state (Activate), 0 = highlight off (Deactivate)**, and the loop draws the cursor only in the Deactivate (==0) state. [UNCERTAIN] the global symbol name `…Hidden` is the prior pass's guess and contradicts the Activate/Deactivate semantics; treat it as "selection-highlight active" rather than "cursor hidden." Missing evidence: a rename grounded in all read-sites simultaneously.

---

## Appendix — dispatch-table raw bytes (verification)
- @0x00410c84 (idx18 header table, 5×u32): `29 01 41 00 | 3c 04 41 00 | b2 04 41 00 | 27 05 41 00 | 99 05 41 00` → 0x410129,0x41043c,0x4104b2,0x410527,0x410599.
- @0x00464104: `63 00 00 00` (count 99) then @0x00464108 (6×u32): `60 0f 41 00 | a0 0f 41 00 | f0 0f 41 00 | 30 10 41 00 | 70 10 41 00 | a0 10 41 00` → the 6 ConfigureCup* fns.
- @0x004168e2: `FF 24 85 D4 7C 41 00` = JMP [EAX*4+0x417cd4]. @0x00417cd4 (12×u32) includes idx7=0x4173b1 (caseD_7), idx10=0x417700 (caseD_a).
- @0x00417d65: `FF 24 8D 90 83 41 00` = JMP [ECX*4+0x418390]. @0x00418390 (8×u32) includes idx6=0x41808d (caseD_6).
