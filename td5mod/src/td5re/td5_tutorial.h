/**
 * td5_tutorial.h -- First-race controller-tutorial overlay (PORT ENHANCEMENT 2026-06).
 *
 * A configurable, dismissable overlay shown at the start of a race: a dimmed
 * (30%-opacity) panel containing a procedurally-drawn Xbox-style controller
 * diagram with a labelled arrow from each control to what it does. The action
 * labels are resolved from the LIVE per-player input binding, so a remap in the
 * Control-Options screen moves the label to the key it was bound to.
 *
 * Behaviour (see /fix design 2026-06-28):
 *   - Pauses the pre-race countdown until the player presses any button.
 *   - "First race only": once dismissed, a persistent flag in
 *     td5re_progress.ini ([Tutorial] Seen) stops it ever showing again.
 *   - Config: [GameOptions] TutorialOverlay (also --TutorialOverlay=N):
 *        0 = off
 *        1 = first-race-only (default)
 *        2 = force every race (dev/testing; does NOT set the seen flag)
 *   - Never armed for network (would desync lockstep), cinematic/replay/attract
 *     demo races, or when slot 0 is AI-driven.
 *
 * This is a source-port feature: the 1999 binary has no equivalent.
 */
#ifndef TD5_TUTORIAL_H
#define TD5_TUTORIAL_H

/** Decide whether to arm the overlay for the race being initialised. Called
 *  once from td5_game_init_race_session() after the per-race reset. */
void td5_tutorial_begin_race(void);

/** 1 while the overlay is up and holding the pre-race countdown; 0 otherwise.
 *  td5_game gates tick_race_countdown() on this so the grid stays frozen. */
int  td5_tutorial_is_active(void);

/** Per-frame: poll for a dismiss press. On the rising edge of any button it
 *  clears the overlay, persists the seen flag (unless force mode), and lets the
 *  countdown resume. No-op when inactive. */
void td5_tutorial_update(void);

/** Per-frame: draw the overlay (dim + panel + controller diagram + labels +
 *  dismiss hint). No-op when inactive. */
void td5_tutorial_draw(void);

#endif /* TD5_TUTORIAL_H */
