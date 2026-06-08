# Frontend DIFF blind-spot post-mortem (2026-06-01)

## Symptom
The Music Test (screen 19) faithful fix shipped WITHOUT the ◄► track-selector arrows.
User caught it. The arrows are the obvious selector affordance on that screen.

## What actually happened (it was NOT an RE miss)
Three of four analysis layers correctly captured the arrows:
- behavior_15.md: `InitializeFrontendDisplayModeArrows(0,1)` on the SELECT TRACK row; plus a
  cross-screen note "Cycler rows are flagged by InitializeFrontendDisplayModeArrows(rowIdx,1)".
- screens_15.md (complete spec): "the SHORT ◄► track selector … ◄► arrow-capable".
- part_15.md (position harvest): MISSED (only "re-renders on cycle").

The DIFF layer (frontend_diff/diff_15.md) is where it broke. Its screen-19 row:
  `Row0 "SELECT TRACK" ... +arrows(0,1) | 0x418460 | :8462 "Select Track" -0x120/0xA0 | MATCH (width 0xA0 ✓)`
→ The diff agent compared only the BUTTON-CREATION call (label/x/w) between orig and port,
  saw they matched, and stamped the row **MATCH** — even though its OWN "original" column
  literally said `+arrows(0,1)` and the port has NO Music Test arrow draw. The `+arrows`
  affordance was present in the evidence and excluded from the verdict.

Then I (main loop) drove the fix from the diff's MATCH verdict and never cross-checked the
richer behavior/spec docs, which were correct.

## Root cause (generalizable — applies to all 30 screens)
**CREATION ≠ RENDERING.** In this engine many elements are produced by a per-screen
*render/dispatch* path that is SEPARATE from the element's creation call:
- ◄► arrows: created implicitly (button is selector-capable) but DRAWN by the port's
  per-screen `case` in the overlay switch `fe_draw_option_arrows(...)` (~td5_frontend.c:5784).
  Screen 19 was never added to that switch → no arrows, despite correct button creation.
- album cover art: primed by the screen, DRAWN by the shared flush (UpdateExtrasGalleryDisplay).
  Same shape — the earlier album-art miss and this arrow miss are the SAME bug class.

A diff that keys on the creation call is structurally blind to "is this screen wired into the
RENDER dispatch for element X." MATCH on creation was reported as MATCH on the element.

## Corrective rule for the remaining screens
When diffing each screen, for EVERY element verify BOTH:
  (1) creation/prime call matches (label, pos, size, gating), AND
  (2) the port actually RENDERS it — i.e. the screen is wired into the relevant render
      dispatch (the overlay `switch(s_current_screen)` for arrows/values, the flush for
      decoupled draws). A `MATCH` requires (1) AND (2); creation-only is at most PARTIAL.
Specifically: re-audit all 30 screens' `+arrows(r,1)` rows against the
`fe_draw_option_arrows` dispatch switch (~:5784) — list which screens/rows are wired and
which are missing. Do the same for every decoupled/flush element.

## Immediate consequence to fix
Screen 19 needs `case TD5_SCREEN_MUSIC_TEST: fe_draw_option_arrows(0, sx, sy);` added to the
overlay arrow dispatch (button 0 = TRACK selector). Original = `InitializeFrontendDisplayModeArrows(0,1)`.
