# Frontend Frame-Rate-Dependent Timers and Counters

## Executive Summary

The TD5 frontend system has **one primary frame counter** (`DAT_0049522c`) that drives ALL
menu animations, and **three secondary frame-driven counters** for fade effects and gallery
slideshow. The attract-mode timeout is the **only time-based** mechanism in the entire frontend.

Running at 165Hz instead of 60Hz would make all animations run ~2.75x faster.

---

## Complete Inventory of Frame-Rate-Dependent Variables

### 1. DAT_0049522c -- Primary Frontend Frame Counter
- **Address:** `0x0049522c`
- **Increment site:** `RunFrontendDisplayLoop` (0x414B50), line: `DAT_0049522c = DAT_0049522c + 1`
- **Rate:** +1 per frame, unconditionally, every call to `RunFrontendDisplayLoop`
- **Usage:** Drives ALL screen transition animations across every screen function
- **Impact at 165Hz:** All animations run 2.75x faster

**Detailed usage by screen function:**

| Screen Function | States Using Counter | What It Controls |
|---|---|---|
| `ScreenMainMenuAnd1PRaceFlow` (0x415490) | 1-2: reset to 0; 3: slide-in (buttons slide from offscreen, caps at 0x27=39); 8: wait for ==2; 9: exit slide (caps at 0x10=16); 10: exit slide (caps at 0x10); 12: exit slide (caps at 0x10); 15: title bar slide (caps at 0x10); 17: exit slide (caps at 0x18=24) | Button slide-in/out animation positions; `MoveFrontendSpriteRect` offsets are `DAT_0049522c * multiplier` |
| `ScreenOptionsHub` (0x41D890) | 1-2: reset to 0; 3: slide-in (caps at 0x27); 7: reset to 0; 8: exit slide (caps at 0x10) | Same slide-in/out pattern |
| `ScreenGameOptions` (0x41F990) | 1-2: reset to 0; 3: slide-in (caps at 0x27); 7: reset to 0; 8: exit slide (caps at 0x10) | Same |
| `ScreenDisplayOptions` (0x420400) | 1-2: reset to 0; 3: slide-in (caps at 0x27); 7: reset to 0; 8: exit slide (caps at 0x10) | Same |
| `ScreenControlOptions` (0x41DF20) | 1-2: reset to 0; 3: slide-in (caps at 0x27); 7: reset to 0; 8: exit slide (caps at 0x10) | Same |
| `ScreenSoundOptions` (0x41EA90) | 1-2: reset to 0; 3: slide-in (caps at 0x27); 7: reset to 0; 8: exit slide (caps at 0x10) | Same + volume bar grow animation |
| `ScreenExtrasGallery` (0x417D50) | 6: set to 0x27F; 7: scrolling credits (decrements by wrapping; text rendered when `(counter & 0x1F) == 0`) | Credits scroll speed: 1px/frame; new text line every 32 frames |
| `CarSelectionScreenStateMachine` (0x40DFC0) | 1: reset to 0; 2: sidebar slide (caps at `(width-0x20)>>3`); 4: reset to 0; 5: button slide (caps at 0x18); 0xB/0x15: transition (caps at 0x15); 0xF-0x12: car info display; 0x14: reset to 0; 0x18: exit slide (caps at 0x18); 0x19: wipe slide | Car selection panel animations |
| `TrackSelectionScreenStateMachine` (0x427630) | 1-2: reset to 0; 3: slide-in (caps at 0x27); 5: track preview load (caps at 2); 6: transition (caps at 2); 7: exit slide (caps at 0x27); 8: preview slide (caps at 0x10) | Track selection animations |
| `RunRaceResultsScreen` (0x422480) | 1-2: reset to 0; 3: slide-in (caps at 0x27); 7-10: results panel slide (caps at 0x11); 0xB: exit slide (caps at 0x11); 0xE: button slide (caps at 0x20); 0x10: exit slide (caps at 0x20); 0x12: button slide (caps at 0x20); 0x14: exit slide (caps at 0x20) | Race results animations |

**Key pattern:** Every screen function follows the same template:
1. State N: reset `DAT_0049522c = 0`
2. State N+1: use `DAT_0049522c` as animation progress, positions = `base + counter * step`
3. When counter reaches target value, advance to next state

### 2. DAT_00494fc8 -- Fade Effect Scanline Counter
- **Address:** `0x00494fc8`
- **Initialized:** `InitFrontendFadeColor` (0x411750) sets to 0
- **Increment site:** `RenderFrontendFadeEffect` (0x411810): `DAT_00494fc8 = DAT_00494fc8 + 2`
- **Increment site:** `RenderFrontendFadeOutEffect` (0x411B00): `DAT_00494fc8 = DAT_00494fc8 + 2`
- **Rate:** +2 per frame when fade is active
- **Purpose:** Processes 64 scanlines per frame (from `DAT_00494fc8` to `DAT_00494fc8 + 0x40`), sweeping a color fade or crossfade from top to bottom of the 480-line screen
- **Total duration:** 480 / 2 = 240 frames = 4.0 seconds at 60Hz, 1.45 seconds at 165Hz
- **Completion:** When `DAT_00494fc8` exceeds `DAT_00495200` (screen height), sets `DAT_00494fc0 = 0` (fade complete flag)
- **Impact at 165Hz:** Fade-to-black and crossfade wipes complete 2.75x faster
- **Used by:** `RunAttractModeDemoScreen` state 5, and various screen transitions

### 3. DAT_0048f2fc -- Gallery/Slideshow Crossfade Counter
- **Address:** `0x0048f2fc`
- **Initialized:** `AdvanceExtrasGallerySlideshow` (0x40D750) sets to `0x100`
- **Decrement sites (all in `UpdateExtrasGalleryDisplay` 0x40D9B0):**
  - Halved each frame: `DAT_0048f2fc = DAT_0048f2fc / 2` (exponential decay)
  - Then decremented: `DAT_0048f2fc = DAT_0048f2fc - 1` (linear countdown)
  - When reaches `-0x18`, calls `AdvanceExtrasGallerySlideshow()` to pick next random image
- **Rate:** Combined halving + linear decrement per frame
- **Purpose:** Controls crossfade blend weight between two surfaces. Value of 0x100 starts as fully "new image"; decays toward 0 (50/50 blend); then goes negative (fully old image fading out)
- **Total cycle:** ~0x100 -> 0 (halving: ~8 frames) + 0 -> -0x18 (linear: 24 frames) = ~32 frames at 60Hz = 0.53 seconds; at 165Hz = 0.19 seconds
- **Also used by:** Main menu background car slideshow (when `DAT_004951dc == 0` and `DAT_00495234 != 0`)
- **Impact at 165Hz:** Image transitions 2.75x faster, images cycle 2.75x more frequently

### 4. DAT_004951fc -- Double-Buffer Flip-Flop
- **Address:** `0x004951fc`
- **Toggle site:** `RunFrontendDisplayLoop`: `DAT_004951fc = DAT_004951fc ^ 1`
- **Rate:** Toggles 0/1 every frame
- **Purpose:** Selects which of two sprite restore buffers (at `DAT_00498720 + index * 0x410`) to use for dirty-rect restoration in `FlushFrontendSpriteBlits`
- **Impact at 165Hz:** NONE -- this is a double-buffer selector, works correctly at any rate

### 5. DAT_00495218 -- Surface Restore Counter
- **Address:** `0x00495218`
- **Set to 3:** When DirectDraw surface is lost (restored via `IDirectDrawSurface::Restore`)
- **Decremented:** Each frame in `RunFrontendDisplayLoop` (the `LAB_00414b8c` path)
- **Purpose:** After surface loss, re-blit cached content for 3 frames to ensure both front/back buffers are valid
- **Impact at 165Hz:** Completes in 3 frames instead of... 3 frames. Shorter wall-clock time but functionally identical. No issue.

---

## Time-Based Mechanisms (Frame-Rate Independent)

### g_frontendFrameTimestamp (DAT_004951E0) -- Attract Mode Timeout
- **Address:** `0x004951E0`
- **Set by:** `SetFrontendScreen` (0x414610): `g_frontendFrameTimestamp = timeGetTime()`
- **Read by:** `RunFrontendDisplayLoop` (0x414B50): `DAT_00495224 - g_frontendFrameTimestamp > 50000`
- **Purpose:** If the player sits idle on the main menu for 50 seconds (50000ms), the game enters attract/demo mode
- **Mechanism:** `DAT_00495224 = timeGetTime()` is called once per frame, then compared to the stored timestamp
- **Impact at 165Hz:** NONE -- this is already timeGetTime-based, completely frame-rate independent

### ShowLegalScreens (0x42C8E0) -- Legal Splash Timeout
- **Mechanism:** `timeGetTime()` loop with 5000ms timeout
- **Impact at 165Hz:** NONE -- already frame-rate independent

### FUN_004274a0 (Copyright Screen) -- Hold Duration
- **Address:** `0x4274a0` (screen function in `g_frontendScreenFnTable`)
- **Mechanism:** State 2 uses `timeGetTime() - g_frontendFrameTimestamp > 2999` (3 second hold)
- **Impact at 165Hz:** Hold is time-based (fine), but fade-in/fade-out use DAT_00494fc8 (frame-driven, 2.75x faster)
- **Note:** This screen also resets `DAT_0049522c = 0` in state 1, confirming it's a screen function

---

## CrossFade16BitSurfaces (0x40CE40) -- Analysis

This function is a **pure pixel blender** with no internal timing. It takes a blend weight
(`param_1`, 0-32 range) as input and performs a weighted average of two locked 16-bit surfaces.
The blend weight is computed by its **caller** (`UpdateExtrasGalleryDisplay`) from `DAT_0048f2fc`.
So the timing is entirely controlled by the frame-driven counter #3 above.

---

## FlushFrontendSpriteBlits (0x425540) -- Analysis

No frame counters. Calls `UpdateExtrasGalleryDisplay()` (which uses counter #3) and
`RenderFrontendDisplayModeHighlight()`. The function itself is a pure render-flush operation
with no timing dependencies.

---

## Recommended Approach: Frame-Rate Independence

### Option A: Frame-Skip in RunFrontendDisplayLoop (RECOMMENDED)

Insert a time check at the top of `RunFrontendDisplayLoop` that skips the logic portion
(screen function call + counter increment) when less than 16ms have elapsed since the last
logic tick, but ALWAYS runs the render/present portion.

**Implementation:**
```
; At entry to RunFrontendDisplayLoop, after input processing:
; Check elapsed time since last logic tick
MOV EAX, [DAT_00495224]    ; current timeGetTime
SUB EAX, [new_lastLogicTick]
CMP EAX, 16               ; 16ms = ~60Hz
JB  skip_logic             ; if less than 16ms, skip logic

; Update last logic tick
MOV EAX, [DAT_00495224]
MOV [new_lastLogicTick], EAX

; ... original logic (screen function, counter increment) ...

skip_logic:
; ... always run render + present ...
```

**Patch site:** After the input processing block in `RunFrontendDisplayLoop` (around VA 0x414C40,
before `(*g_frontendScreenFn)()` call at VA ~0x414C48), insert a JMP to a code cave.

**What to skip when < 16ms:**
- `(*g_frontendScreenFn)()` -- the screen function dispatcher
- `DAT_0049522c += 1` -- the frame counter increment
- The `NoOpHookStub()` call (harmless but unnecessary)
- The keyboard shortcut processing block

**What to ALWAYS run:**
- Input polling (DXInputGetKBStick, GetMouse) -- needed for responsiveness
- `UpdateFrontendClientOrigin()` -- mouse cursor tracking
- `RenderFrontendUiRects()` + `FlushFrontendSpriteBlits()` -- render
- `Flip` / `PresentFrontendBufferSoftware()` -- present
- `UpdateFrontendDisplayModeSelection()` -- selection highlight
- `DAT_004951fc ^= 1` -- double-buffer toggle (must match present rate)
- The attract-mode timeout check (already time-based)

**Side effects:**
- Input still polled at 165Hz (good -- responsive cursor)
- Present still runs at 165Hz (good -- no tearing)
- Animations run at ~60Hz (good -- correct speed)
- `DAT_004951fc` flip-flop runs at present rate (good -- must match)
- `FlushFrontendSpriteBlits` runs at 165Hz but `UpdateExtrasGalleryDisplay` inside it would
  also need to be gated -- HOWEVER, since it reads `DAT_0048f2fc` which is only modified by
  the screen function (`ScreenExtrasGallery` sets it, `UpdateExtrasGalleryDisplay` decrements
  it), the slideshow counter decrement IS in the always-run path. This needs special handling.

**Critical fix for FlushFrontendSpriteBlits path:**
The `UpdateExtrasGalleryDisplay` function (called from `FlushFrontendSpriteBlits`) ALSO
decrements `DAT_0048f2fc` each frame. This is NOT inside `(*g_frontendScreenFn)()` -- it's in
the render path. Options:
1. Move the gate to wrap `UpdateExtrasGalleryDisplay` as well (complex)
2. Accept that the slideshow will still run 2.75x faster (minor visual issue)
3. Add a separate time-gate inside `UpdateExtrasGalleryDisplay` (clean but more patches)

### Option B: Patch Each Counter Individually

For each of the 3 frame-driven counters, change the increment to be time-proportional:
- `DAT_0049522c`: increment only when 16ms elapsed (add time check)
- `DAT_00494fc8`: increment by `elapsed_ms / 16 * 2` instead of flat +2
- `DAT_0048f2fc`: decrement only when 16ms elapsed

This is more surgical but requires 3 separate patches.

### Option C: Skip Entire RunFrontendDisplayLoop Call (SIMPLEST)

In the caller of `RunFrontendDisplayLoop` (the game state dispatcher at 0x442170), add a
time check that skips the call entirely when < 16ms have passed. This means:
- No input polling at high rate (laggy cursor)
- No present at high rate (could cause issues with VSync)
- Simplest to implement (one check, one skip)

**Not recommended** due to input lag.

---

## Summary Table

| Variable | Address | Type | Rate | Duration at 60Hz | Duration at 165Hz | Fix Needed |
|---|---|---|---|---|---|---|
| `DAT_0049522c` | 0x0049522c | Frame counter | +1/frame | Varies (16-39 frames per anim = 0.27-0.65s) | 0.10-0.24s | YES |
| `DAT_00494fc8` | 0x00494fc8 | Scanline counter | +2/frame | 240 frames = 4.0s | 1.45s | YES |
| `DAT_0048f2fc` | 0x0048f2fc | Crossfade blend | halve+decr/frame | ~32 frames = 0.53s | 0.19s | YES |
| `DAT_004951fc` | 0x004951fc | Flip-flop | toggle/frame | N/A | N/A | NO |
| `DAT_00495218` | 0x00495218 | Surface restore | -1/frame (max 3) | 3 frames | 3 frames | NO |
| `g_frontendFrameTimestamp` | 0x004951E0 | Timestamp (ms) | timeGetTime | 50000ms | 50000ms | NO |

**Total patches needed for Option A:** 1 code cave (time-gate around screen function + counter increment), plus 1 additional gate for `UpdateExtrasGalleryDisplay` in the render path.

**Recommended approach:** Option A with the additional `UpdateExtrasGalleryDisplay` gate.
This preserves 165Hz input polling and present while keeping all animations at their intended
~60Hz speed. The code cave needs ~30 bytes for the time check logic.

---

## Precise Patch Sites (from disassembly)

### RunFrontendDisplayLoop Key Addresses

```
VA 0x414C99: CALL 0x00425170          ; UpdateFrontendClientOrigin -- KEEP (cursor)
VA 0x414C9E: CALL [0x00495238]        ; (*g_frontendScreenFn)() -- GATE THIS
VA 0x414CA4: MOV EAX, [0x0049524C]    ; early-return check
VA 0x414CB1: CALL 0x00418450          ; NoOpHookStub -- can skip
VA 0x414CF6: CALL 0x00425A30          ; RenderFrontendUiRects -- KEEP
VA 0x414CFB: CALL 0x00425540          ; FlushFrontendSpriteBlits -- KEEP (but contains gallery)
VA 0x414E9F: MOV EDX, [0x0049522C]    ; read frame counter
VA 0x414EB0: INC EDX                  ; frame counter increment -- GATE THIS
VA 0x414EB3: MOV [0x0049522C], EDX    ; frame counter store -- GATE THIS
```

### Optimal Gate Location

**Gate start:** VA 0x414C9E (the `CALL [0x00495238]` = screen function dispatch)
**Gate end:** VA 0x414CB1 (before `NoOpHookStub`)
**Counter gate:** VA 0x414E9F through 0x414EB3 (the INC + store)

**Strategy:** At VA 0x414C9E, replace the 6-byte indirect call with a JMP to a code cave.
In the cave:
1. Call `timeGetTime()` (via `[0x0045D5C0]`, already used later in the function)
2. Compare with stored `last_logic_tick`
3. If elapsed < 16ms, skip the screen function call and JMP to 0x414CA4
4. Otherwise, update `last_logic_tick`, call `[0x00495238]`, JMP to 0x414CA4

For the counter increment at 0x414E9F-0x414EB3, add a similar check or use a
shared "did_logic_this_frame" flag set by the first gate.

### UpdateExtrasGalleryDisplay Gate

The `DAT_0048f2fc` decrement happens inside `UpdateExtrasGalleryDisplay` (0x40D9B0),
which is called from `FlushFrontendSpriteBlits` (0x425540) at every frame regardless.
This function needs its own time gate or a shared flag from the main gate.

**Simplest approach:** Use a 1-byte global flag (`did_logic_tick`) set to 1 by the main
gate when logic runs, and checked by `UpdateExtrasGalleryDisplay` to decide whether to
decrement `DAT_0048f2fc`. Clear the flag at the start of each `RunFrontendDisplayLoop` call.
