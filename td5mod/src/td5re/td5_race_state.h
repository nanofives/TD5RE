/* td5_race_state.h -- narrow READ-ONLY race-state query API for leaf modules.
 *
 * [LAYERING 2026-07-06] td5_game.h is the repo's dependency magnet (32
 * includers at baseline): the full race-FSM header couples leaf modules
 * (track/asset/physics/sound/light) to the game core, so any change there
 * ripples everywhere. Most leaf modules only ever needed a handful of
 * read-only queries — this header is exactly that surface and nothing else.
 *
 * RULES:
 *   - Declarations here are a strict SUBSET of td5_game.h (implementations
 *     stay in td5_game.c; signatures must match verbatim — C tolerates the
 *     duplicate declaration when both headers meet in one TU).
 *   - READ-ONLY queries only. If a leaf module needs to MUTATE game state
 *     (see td5_ai.c's cop-chase scoring), it keeps td5_game.h until that
 *     mutation is redesigned — never add a mutator here.
 *   - New leaf modules include THIS, not td5_game.h. The structure lint
 *     (scripts/lint_structure.ps1) fails CI on any new td5_game.h includer.
 *
 * [S7 2026-07-10] td5_sound.c's one mutating call (the sky-rotation-tracker
 * advance on the cop siren) is inverted via td5_sound_take_sky_rotation_advance_request()
 * (see td5_sound.h) instead of calling into td5_game.c directly, so it could
 * drop td5_game.h for this file's read-only subset.
 */

#ifndef TD5_RACE_STATE_H
#define TD5_RACE_STATE_H

#include "td5_types.h"

/* --- Actor roster (read-only) ------------------------------------------ */
TD5_Actor *td5_game_get_actor(int slot);
int  td5_game_get_total_actor_count(void);
int  td5_game_get_racer_count(void);
int  td5_game_get_player_slot(int viewport);
int  td5_game_get_view_pan(int vp);

/* --- Mode queries ------------------------------------------------------- */
int  td5_game_is_replay_active(void);
int  td5_game_is_wanted_mode(void);
int  td5_game_is_pause_menu_active(void);
int  td5_game_cop_chase_is_cop(int slot);
int  td5_game_cop_chase_is_suspect(int slot);
int  td5_game_get_cop_actor_index(void);
int  td5_game_mp_traffic_fair(void);
int  td5_game_battle_mode_active(void);
int  td5_game_drag_mp_active(void);

/* --- Race progress / results (read-only) -------------------------------- */
int     td5_game_get_player_lap(int slot);
int     td5_game_get_race_order(int pos);         /* slot index at finish position pos */
int     td5_game_slot_is_finished(int slot);      /* 1 if post_finish_metric_base != 0 */
int     td5_game_slot_finish_place(int slot);     /* 1-based place captured when slot finished (0 = still racing) */
int     td5_game_get_finish_position(int slot);   /* 0-based finish position (0 = 1st), -1 if not set */
int     td5_game_get_highest_position(int slot);  /* best (lowest) race_position seen this race; 0=1st, -1 if invalid */
int32_t td5_game_get_result_top_speed(int slot); /* top speed raw units */
int32_t td5_game_get_result_avg_speed(int slot); /* average speed raw units */
int     td5_game_get_victory_position(void);
int     td5_game_is_countdown_active(void);

/* --- Drag-strip configuration (read-only) ------------------------------- */
int  td5_game_drag_field_size(void);
int  td5_game_drag_length_repeats(void);

/* --- Traffic queries (read-only) ----------------------------------------- */
int  td5_game_get_traffic_variant(int traffic_index);

#endif /* TD5_RACE_STATE_H */
