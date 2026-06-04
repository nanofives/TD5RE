# Frontend transition fade SFX — universal coverage (S03, 2026-06-04)

**Goal:** every navigable menu screen fades **OUT** (Whoosh = SFX 5) when left and
**IN** (Crash1 chime = SFX 4) when entered — *including* the newer multiplayer /
track screens and any future ones — with no doubling, and the deliberately-silent
end dialogs kept silent.

All changes are in `td5mod/src/td5re/td5_frontend.c` (+ one `td5re.ini` value).

## Step 1 — SFXVolume trap (verify FIRST)

Dev `td5re.ini [Audio] SFXVolume` was **0** (silent). The in-game pause/options
**SOUND** slider persists its value back to this INI, so a previous run dragged it
to 0 → **all** frontend SFX inaudible, which would make the fades look broken no
matter how they're wired. Bumped to **68** (matches `td5re_release.ini`). Always
check `SFXVolume` before chasing any "missing sound".

## Model (from the earlier frontend-sound RE)

- **SFX 5 = Whoosh = slide-OUT** (plays as a screen's buttons slide).
- **SFX 4 = Crash1 = slide-IN settle chime.**
- The original `SetFrontendScreen` (0x00414610) is **silent**; each screen's state
  machine plays its own `Play(N)`. An *unconditional* `Play()` in `set_screen`
  doubles the wired screens' Whoosh (a prior attempt was reverted for exactly this).

## Step 3 — systematic mechanism

`td5_frontend_set_screen()` is the **only** universal transition hook, but the
per-screen whoosh fires *before* it (exit "slide-out prep") and the per-screen
chime fires *after* it (entry settle). So a one-line `Play()` there can't both
cover gaps and avoid doubling. Instead:

- `frontend_play_sfx()` wrapper records `s_fade_whoosh_emitted` / `s_fade_chime_emitted`
  for the **current screen's lifetime** (reset in `set_screen`).
- `set_screen()`:
  - plays the **default slide-OUT (5)** for the screen being **left** *iff* it
    never whooshed this lifetime (every wired screen does → fires only for
    unwired/new screens), and
  - **arms** `s_fade_in_pending` for the screen being **entered**.
- `td5_frontend_display_loop()` (after the screen `fn()` has already run this frame,
  so `s_fade_chime_emitted` is current): if the new screen chimed itself → drop the
  armed default; else fire the **default slide-IN (4)** once the screen settles
  (`s_anim_complete`) or after `TD5_FE_FADE_IN_DEADLINE_MS` (1500 ms) backstop.
- `frontend_screen_wants_fade()` — **default = 1** so any screen added to
  `s_screen_table`, including future ones, inherits the fades with no wiring.
  Excluded (return 0): boot/init/attract/debug `[0,1,2,3,4,28]` and the silent end
  dialogs `CupFailed[26] / CupWon[27] / SessionLocked[29]`.

The dedup is **exact** for the original 30 screens: each plays ≥1 whoosh in its
lifetime, and each chime is emitted co-located with or *before* `s_anim_complete`
(e.g. CarSelection chimes in case 5, sets `s_anim_complete` in case 6; RaceResults
does both in the same case-3 block). The change is **purely additive** — existing
per-screen audio is untouched; only gaps are filled.

## Step 2 — audit

| Area | Before | After |
|------|--------|-------|
| **[30] MP Lobby** (the screen the feedback targeted) | ZERO fades — START/BACK call `set_screen` directly; entry sets `s_anim_complete` with no chime | default whoosh on leave + default chime on enter |
| Screens that set `s_anim_complete` with no co-located chime (TrackSelection[21] entry, RaceResults post-race menu sub-flow, NameEntry[25], several Options sub-screens) | enter chime missing | enter chime via the armed default |
| ESC-back (global handler calls `set_screen`, skipping the slide-out anim) | often silent on leave | default slide-out whoosh |
| Wired screens (MainMenu[5], CarSelection[20], HighScore[23], RaceResults[24] slide-in, RaceTypeCategory[6]…) | own whoosh + chime | unchanged — their calls set the flags → defaults suppressed (no double) |
| Silent dialogs [26][27][29] | zero SFX (intentional) | excluded → stay silent (defaults never added; existing calls never removed) |

## Verification

- `td5_frontend.c` compiles clean with the project flags
  (`-m32 -O2 -Wall -std=c11 -I… -DTD5_INFLATE_USE_ZLIB`), `EXIT=0`; all warnings are
  pre-existing and in unrelated functions — none in the new code, and no "unused"
  warning for the new statics/helper.
- **NOTE:** `td5_frontend.c` was being concurrently edited by a sibling workstream
  (true-2D spatial nav for the Controller-Binding screen) during this work. The
  changes are in disjoint regions, but the full build/link + a drive-test to hear
  the fades should happen at the merge/coordination point (`/end` builds dev+release).

## Acceptance

- Every navigable screen plays slide-out on leave and slide-in on enter — yes,
  via per-screen calls where present and the centralized default elsewhere.
- A brand-new screen added to `s_screen_table` gets fades with no extra wiring —
  yes, `frontend_screen_wants_fade()` defaults to 1.
- No regression of the previously-fixed per-screen audio — yes, all existing
  `frontend_play_sfx` calls are untouched; the default only *adds* where a screen
  was silent, and the dedup prevents doubling.
