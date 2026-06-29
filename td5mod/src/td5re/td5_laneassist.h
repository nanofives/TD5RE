/* ========================================================================
 * td5_laneassist.h — optional steering aid (PORT-ONLY, default OFF)
 *
 * Lane assist + lane alignment: a capped steering aid that eases a struggling
 * player toward the centre of the DRIVABLE road. The correction climbs
 * CONTINUOUSLY the further off-centre you are (gentle near the middle, firmer
 * near the edges), never steers toward a slow/grass lane, and stays continuous
 * across forks (lane divergence) and merges (lane convergence) so it never
 * yanks. Accessibility aid — NOT faithful to the original binary; there is no
 * original equivalent.
 *
 * Enable (per human player): the Game Options "LANE ASSIST" row (single player),
 * the multiplayer Profile-Selection screen toggle next to AUTOMATIC/MANUAL, or
 * [Input] LaneAssist=1 (g_td5.ini.lane_assist) as the session default; the
 * keyboard 'L' key flips player 0 at runtime. Tuning knobs (all env, read once):
 *   TD5RE_LANEASSIST_STRENGTH       ramp slope % (100=1x)        (default 900)
 *   TD5RE_LANEASSIST_LOOKAHEAD_SPANS look-ahead spans blended    (default 6)
 *   TD5RE_LANEASSIST_MAX_YAW        near-centre yaw cap/tick      (default 400)
 *   TD5RE_LANEASSIST_FORK_COMMIT    follow forks (1) / stay main (0) (default 1)
 * The aid steers (yaw) toward a FAR look-ahead point with a linear cross-track
 * term + derivative damping on the heading error, so it tracks the road without
 * weaving; it never injects lateral velocity (that spun the car) and pauses
 * while airborne/crashed/too slow or while you are steering hard.
 * ======================================================================== */
#ifndef TD5_LANEASSIST_H
#define TD5_LANEASSIST_H

#include <stdint.h>
#include "td5_types.h"   /* TD5_Actor forward typedef */

/* Per-tick lateral nudge for the human player driving `actor`. Called from
 * td5_physics_update_player AFTER the world-frame velocity writeback, with the
 * heading sin/cos (12-bit fixed) already resolved by the caller. No-op unless
 * lane assist is enabled for this player's slot. Never overrides input: the
 * nudge is capped and fades out while the player is actively steering. */
void td5_laneassist_apply(TD5_Actor *actor, int32_t sin_h, int32_t cos_h);

/* Flip the per-player runtime enable (edge-triggered from the input layer). */
void td5_laneassist_toggle(int player);

/* 1 if lane assist is currently enabled for `player`. */
int  td5_laneassist_enabled(int player);

/* Set `player`'s enable from a menu choice (call at race start per human player).
 * Resets the per-player runtime state so a re-enable starts clean. */
void td5_laneassist_set_player(int player, int on);

/* Direction the aid is currently steering for `player` (HUD arrow): -1 = left,
 * 0 = none/centred, +1 = right. Reflects the last applied yaw nudge. */
int  td5_laneassist_hud_dir(int player);

/* 1 if the aid actually steered on the last tick, 0 if it was paused (car
 * airborne / crashed / too slow / you were steering hard). Drives the HUD's
 * active-vs-paused badge so a temporary pause never looks like a failure. */
int  td5_laneassist_hud_active(int player);

/* Mark `player` as not-currently-assisting. Called by the physics dispatch when
 * a car is routed into the stunned (state0f) path, where the apply tick never
 * runs — keeps the HUD badge honest during a crash/launch. */
void td5_laneassist_note_inactive(int player);

/* Clear the per-player smoothed-target state (optional; the apply path also
 * self-snaps on a large position jump, e.g. car reset / new race). */
void td5_laneassist_reset(void);

#endif /* TD5_LANEASSIST_H */
