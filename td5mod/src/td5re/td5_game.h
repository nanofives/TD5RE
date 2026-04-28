/**
 * td5_game.h -- Main game loop, state machine, game flow
 *
 * Covers: GameWinMain, RunMainGameLoop (4-state FSM), RunRaceFrame,
 * InitializeRaceSession, frame timing, fade system, race completion.
 *
 * Original functions:
 *   0x4493E0  entry (CRT)
 *   0x430A90  GameWinMain
 *   0x442170  RunMainGameLoop
 *   0x42AA10  InitializeRaceSession
 *   0x42B580  RunRaceFrame
 *   0x42C8E0  ShowLegalScreens
 *   0x442160  InitializeRaceVideoConfiguration
 *   0x414740  InitializeFrontendResourcesAndState
 *   0x414B50  RunFrontendDisplayLoop
 *   0x42C2B0  InitializeRaceViewportLayout
 *   0x430CB0  ResetGameHeap
 *   0x40A2B0  AdvancePendingFinishState
 *   0x40A3D0  AccumulateVehicleSpeedBonusScore
 *   0x40A440  DecayUltimateVariantTimer
 *   0x40A530  AdjustCheckpointTimersByDifficulty
 *   0x42CC20  BeginRaceFadeOutTransition
 *   0x42CBE0  IsLocalRaceParticipantSlot
 *   0x42CCD0  StoreRoundedVector3Ints
 */

#ifndef TD5_GAME_H
#define TD5_GAME_H

#include "td5_types.h"

/* --- Module lifecycle --- */
int  td5_game_init(void);
void td5_game_shutdown(void);
int  td5_game_tick(void);  /* Returns 0=continue, 1=quit */

/* --- State machine --- */
void td5_game_set_state(TD5_GameState state);
TD5_GameState td5_game_get_state(void);

/* --- Race session --- */
int  td5_game_init_race_session(void);
int  td5_game_run_race_frame(void);   /* Returns 0=racing, 1=race finished, 2=ESC quit */
void td5_game_release_race_resources(void);

/* --- Race flow --- */
int  td5_game_check_race_completion(void);
void td5_game_begin_fade_out(int param);
int  td5_game_is_local_participant(int slot);

/* Returns the race slot state byte:
 *   0 = AI racer, 1 = active player, 2 = completed, 3 = disabled.
 * Used by the renderer to skip disabled slots in drag race. */
int  td5_game_get_slot_state(int slot);

/* Returns the companion_2 byte for a slot (offset +2 in the slot record).
 * Used by Screen_RaceResults to detect cup elimination (value == 2). */
int  td5_game_get_slot_companion_2(int slot);

/* Returns 1 iff the actor at slot has posted a finish score
 * (post_finish_metric_base != 0); 0 otherwise. */
int  td5_game_slot_is_finished(int slot);

/* --- Timing --- */
void td5_game_update_frame_timing(void);
float td5_game_get_fps(void);
float td5_game_get_frame_dt(void);

/* --- Viewport --- */
void td5_game_init_viewport_layout(void);

/* --- Intro / Legal --- */
void td5_game_play_intro_movie(void);
void td5_game_show_legal_screens(void);

/* --- Utilities --- */
void td5_game_store_rounded_vec3(const float *in, int32_t *out);
TD5_Actor *td5_game_get_actor(int slot);
int td5_game_get_total_actor_count(void);
int td5_game_get_player_lap(int slot);
int32_t td5_game_get_race_timer(int slot, int lap_index);

/* 0x430CF0: Allocate from game heap */
void *td5_game_heap_alloc(size_t size);

/* --- Game state globals (defined in td5_game.c) --- */
extern int      g_replay_mode;
extern int      g_wanted_mode_enabled;
extern int      g_special_encounter;
extern int      g_race_rule_variant;
extern int      g_game_type;
extern int      g_split_screen_mode;
extern int      g_racer_count;
extern float    g_instant_fps;
extern uint32_t g_tick_counter;
extern int      g_actor_slot_map[2];
extern void    *g_actor_pool;

/* --- Split-screen --- */
int td5_game_get_player_slot(int viewport);
int td5_game_is_split_screen(void);

/* --- Post-race results accessors (for Screen [24]/[25]) ---
 * [CONFIRMED @ 0x0040AAD0/0x0040AB80] sort populates s_results
 * [CONFIRMED @ 0x00413BC0] ScreenPostRaceNameEntry reads these fields */
int32_t td5_game_get_result_primary(int slot);   /* finish time ticks */
int32_t td5_game_get_result_secondary(int slot); /* accumulated points */
int32_t td5_game_get_result_top_speed(int slot); /* top speed raw units */
int32_t td5_game_get_result_avg_speed(int slot); /* average speed raw units */
int     td5_game_get_race_order(int pos);         /* slot index at finish position pos */
int     td5_game_slot_is_finished(int slot);      /* 1 if post_finish_metric_base != 0 */
int32_t td5_game_get_best_lap_time(int slot);     /* best lap ticks, lap-type tracks */

/* Re-sort s_results in place using the current game_type's metric.
 * Called from Screen [24] case 0 to mirror RunRaceResultsScreen behaviour.
 * [CONFIRMED @ 0x00422480 case 0] */
void td5_game_sort_results(void);

/* --- Split-Screen Steering Balance (0x4036B0) --- */
void td5_game_update_split_screen_balance(void);

#endif /* TD5_GAME_H */
