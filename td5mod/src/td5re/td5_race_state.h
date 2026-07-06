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
 *     (see td5_ai.c's cop-chase scoring, td5_sound.c's sky-rotation call),
 *     it keeps td5_game.h until that mutation is redesigned — never add a
 *     mutator here.
 *   - New leaf modules include THIS, not td5_game.h. The structure lint
 *     (scripts/lint_structure.ps1) fails CI on any new td5_game.h includer.
 */

#ifndef TD5_RACE_STATE_H
#define TD5_RACE_STATE_H

#include "td5_types.h"

/* --- Actor roster (read-only) ------------------------------------------ */
TD5_Actor *td5_game_get_actor(int slot);
int  td5_game_get_total_actor_count(void);
int  td5_game_get_player_slot(int viewport);

/* --- Mode queries ------------------------------------------------------- */
int  td5_game_is_replay_active(void);
int  td5_game_is_wanted_mode(void);
int  td5_game_cop_chase_is_cop(int slot);
int  td5_game_cop_chase_is_suspect(int slot);
int  td5_game_mp_traffic_fair(void);
int  td5_game_battle_mode_active(void);
int  td5_game_drag_mp_active(void);

/* --- Drag-strip configuration (read-only) ------------------------------- */
int  td5_game_drag_field_size(void);
int  td5_game_drag_length_repeats(void);

#endif /* TD5_RACE_STATE_H */
