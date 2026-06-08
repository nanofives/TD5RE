# Frontend RE Methodology Post-Mortem (2026-05-30)

Why the port frontend keeps diverging from the original despite "CONFIRMED @ address"
RE, and why whole elements (e.g. the Music Test album cover art) were omitted.

Triggered by a user side-by-side: original Music Test shows the playing track's ALBUM
COVER ART (Fear Factory "Remanufacture"), short "TRACK" selector w/ ◄► arrows, and
"NOW PLAYING" text. The port shows "SELECT TRACK" (wrong label/too long), the track
name clipped behind the button, NO cover art, and a stray 3D car in the background.

## The proven case: how the album art is actually drawn (and why it was missed)

The original frontend is DEFERRED/RETAINED-mode; the harvest treated it as immediate-mode.

- `ScreenMusicTestExtras @0x00418460` NEVER draws the cover art. It only primes globals:
  - `LoadExtrasBandGalleryImages @0x0040d6a0` loads 5 band-cover TGAs from
    `Front End\Extras\Extras.zip` into `g_extrasGallerySlideSurfaces @0x0048f2d4`
    (Fear Factory / Gravity Kills / Junkie XL / KMFDM / PitchShifter; count @0x...=5).
  - 12 CD tracks → 5 band slides via LUT `@0x00465e4c` = {1,3,4,4,2,0,0,1,3,4,4,4},
    indexed by `g_attractCdTrackCandidate` (set from `g_selectedCdTrackIndex`).
  - Sets `g_extrasGalleryCrossFadePhase @0x0048f2fc` = 0x100 on track change.
- The DRAW is `UpdateExtrasGalleryDisplay @0x0040d830`, blitting via
  `CrossFade16BitSurfaces @0x0040d190` (custom MMX RGB565 alpha blend) + a raw DDraw
  BltFast vtable `(**(*surf+0x1c))()`, panel at (x=0x76=118, y=0x8c=140).
- CRITICAL: `UpdateExtrasGalleryDisplay` is invoked by the SHARED per-frame
  `FlushFrontendSpriteBlits @0x00425540`, NOT by the screen function. The screen
  function's call graph has no edge to it.

Blind-spot quantified: of ~14 surface-touching callees of 0x00418460, the harvest's
fixed 4-helper filter {Copy16BitSurfaceRect 0x4251a0, QueueFrontendOverlayRect 0x425660,
MoveFrontendSpriteRect 0x4259d0, CreateFrontendDisplayModeButton 0x425de0} matched 3.
The album-art draw was in ZERO callees of the screen function. Assets are already
extracted at re/assets/extras/ — the port HAS the art; it never draws it.

Button label: `SNK_SelectTrackButTxt` (LANGUAGE.DLL ptr @0x0045d298; literal not in
the EXE). Original button HAS ◄► arrows (`InitializeFrontendDisplayModeArrows(0,1)`).
Photo shows the rendered label is the short "TRACK". Port hardcoded "Select Track".

## Root causes (systemic)

1. STATIC DECOMP TREATED AS GROUND TRUTH. "CONFIRMED @ addr" only ever meant "this
   constant/call exists in the decompiled code", never "the port reproduces the
   original's pixels". No screenshot / visual-diff loop was ever run against the
   running original (despite tools/orig_goto_screen.py existing for it). The human
   was the de-facto validation loop.
2. CALL-GRAPH-SCOPED, FIXED-HELPER HARVEST. Each screen function was decompiled and
   its callees scanned for 4 named blitters. Structurally blind to:
   (a) elements drawn by shared/global per-frame flush passes from primed globals
       (the album art via FlushFrontendSpriteBlits),
   (b) primitives outside the fixed list (CrossFade16BitSurfaces, raw surf+0x1c BltFast),
   (c) asset CONTENT / which image maps to which track (decomp shows code, not pixels).
3. DISMISS-AS-COSMETIC REFLEX. Seen-but-unanchored references ("gallery", "crossfade")
   were labeled "ARCH-DIV, not ported" rather than traced to their draw site.
4. PORT-AS-BASELINE BIAS. Diffed orig-decomp against the already-wrong port and adopted
   the port's omissions as intentional divergences.
5. LANGUAGE.DLL NEVER READ. Button/option labels were hardcoded English guesses instead
   of the actual SNK_ strings — wrong text AND wrong length (drives layout: a too-long
   guessed label needs a wider button, which then covers neighbours).
6. --StartScreen JUMP TESTING. Reaches a screen in a non-representative state (stray 3D
   car background = scene not torn down by the jump), so "verification" launches did not
   reflect normal navigation.
7. SUMMARY TRUST. Sub-agents filtered to the helpers they were asked about, faithfully
   propagating the harvester's blind spot back as "CONFIRMED" coverage.

## Corrected methodology (for any future frontend faithfulness pass)

1. VISUAL GROUND TRUTH FIRST. For each screen: capture the ORIGINAL via
   tools/orig_goto_screen.py + a screenshot; capture the PORT via NORMAL navigation
   (never --StartScreen for fidelity checks); diff the images. The screenshot is the
   source of truth; RE explains/reproduces it. Reserve "CONFIRMED" for items validated
   against rendered output.
2. ENUMERATE ALL DRAWS, NOT A HELPER LIST. Per screen, list every surface-touching call,
   AND trace the global deferred-flush compositors (FlushFrontendSpriteBlits and any
   per-frame draw pass) plus every gNNN surface/state global the screen PRIMES → where
   each is later consumed/drawn. Follow primed-global → flush-draw edges explicitly.
3. INVENTORY ASSETS. Map per-screen image sets (e.g. the 5 Extras band covers, the
   track→band LUT) to the visual, so "which picture shows for track N" is answered.
4. READ LANGUAGE.DLL. Resolve SNK_ label strings for real text + measured width before
   choosing button widths/positions.
5. DROP PORT-AS-BASELINE. Establish orig-render vs orig-decomp truth first; THEN list
   what the port lacks. A missing feature is a gap, not a decision.

## Concrete open gaps this exposed on Music Test (NOT yet fixed — investigation only)
- Album cover art panel (5 covers, track→band LUT, crossfade) at (118,140): MISSING.
- Button label should be the SNK_ string ("TRACK"), not hardcoded "Select Track".
- Button width/font (see reference_frontend_button_label_font_2026-05-30): 12px font,
  short label → small arrowed selector, not a wide bar.
- Stray 3D car background: background not the original diamond-plate/static; needs a
  normal-navigation repro to confirm whether it's a jump artifact or a real port bug.
