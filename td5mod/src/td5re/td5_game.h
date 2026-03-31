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
int  td5_game_run_race_frame(void);   /* Returns 0=racing, 1=race over */
void td5_game_release_race_resources(void);

/* --- Race flow --- */
int  td5_game_check_race_completion(void);
void td5_game_begin_fade_out(int param);
int  td5_game_is_local_participant(int slot);

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

#endif /* TD5_GAME_H */
