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

/* Always-on FPS overlay: smoothed FPS + worst frame time (ms) over the last ~1s.
 * Drawn by the frontend and the in-race HUD so the counter shows everywhere.
 * td5_game_update_fps_overlay() must be called once per frame from the main loop
 * (all states), since td5_game_update_frame_timing() only runs in the race. */
extern float g_td5_display_fps;
extern int   g_td5_peak_frame_ms;
void td5_game_update_fps_overlay(void);

/* --- Viewport --- */
void td5_game_init_viewport_layout(void);

/* Resolve the split-screen pane grid (cols x rows) for `views` local panes.
 * Honours the committed Multiplayer Options layout pick (g_td5.split_grid_*)
 * when valid, else falls back to the automatic ladder. Single source of truth
 * so the 3D viewport rects, HUD per-pane layout, and divider lines all agree. */
void td5_game_resolve_split_grid(int views, int *cols, int *rows);
void td5_game_get_pane_rect(int views, int v, int w, int h,
                            int *x, int *y, int *pw, int *ph);

/* [#9 SPLIT-LAYOUT FIX 2026-06-15] Position-screen cell mapping.
 *
 * The "choose your screen" picker lets each player park their pane in ANY grid
 * cell, so the panes are a permutation of cells (not always the contiguous head
 * 0..N-1) and the empty/map cell is not always the tail. These two queries make
 * the viewport rects (td5_game) and the empty-cell filler (HUD) agree on which
 * cells are panes and which are free:
 *
 *   td5_game_get_pane_cell(views, v)
 *     -> the grid cell (row-major over the resolved cols x rows) that viewport v
 *        is laid out in. Identity (returns v) when the layout fix is off or no
 *        permutation was committed (AutoRace / harness / non-positioned MP).
 *
 *   td5_game_split_cell_is_viewport(views, cell)
 *     -> 1 if a player viewport occupies `cell`, else 0. The HUD must iterate all
 *        cols*rows cells and draw the map/standings filler ONLY where this is 0,
 *        rather than assuming the empties are the tail (views..cols*rows-1) —
 *        that assumption is what put a pane on top of the empty/map cell (#9b).
 *
 * Both reflect the map built in td5_game_init_viewport_layout(); call them only
 * after it has run (i.e. in-race). */
int  td5_game_get_pane_cell(int views, int v);
int  td5_game_split_cell_is_viewport(int views, int cell);

/* [MP audio 2026-06-24] Stereo pan (-10000..+10000) for viewport `vp` from its
 * pane column in the split grid — outer columns map to L/R so each local
 * player's car audio can be placed in the speaker matching their on-screen
 * quadrant. 0 for single-view / horizontal-only splits. In-race only. */
int  td5_game_get_view_pan(int vp);

/* --- Intro / Legal --- */
void td5_game_play_intro_movie(void);
void td5_game_show_legal_screens(void);

/* --- Utilities --- */
void td5_game_store_rounded_vec3(const float *in, int32_t *out);
TD5_Actor *td5_game_get_actor(int slot);
int td5_game_get_total_actor_count(void);
int td5_game_get_racer_count(void);   /* racer slots only (no traffic/scenery) */
int td5_game_get_minimap_checkpoint_count(void);
int td5_game_get_minimap_checkpoint_span(int idx);
int td5_game_get_player_lap(int slot);
int td5_game_get_slot_span(int slot);   /* live track_span_normalized (+0x82) for a slot, 0 if none */
int td5_game_get_slot_heaviness_q8(int slot); /* car heaviness Q8 (0x100=median, higher=heavier), 0 if none */
int td5_game_get_slot_accel(int slot);        /* power-to-weight ACCEL score (torque*inv_mass), 0 if none */
int td5_game_get_slot_progress(int slot);     /* monotonic lap*ring+span (warp detect); folded span on P2P */
int td5_game_get_slot_climb(int slot);        /* signed per-tick climb rate (>0 uphill), 0 if none */
int32_t td5_game_get_race_timer(int slot, int lap_index);

/* 0x430CF0: Allocate from game heap */
void *td5_game_heap_alloc(size_t size);

/* --- Game state globals (defined in td5_game.c) --- */
extern int      g_replay_mode;
/* Attract-mode demo flag (port analog of g_replayModeFlag @ 0x4AAF64). Set
 * ONLY by the attract-demo launch path — distinct from g_replay_mode which is
 * the input-playback "View Replay" flag (orig g_inputPlaybackActive @ 0x466E9C).
 * The original keeps these as two separate globals: demo shows the "DEMO MODE"
 * status text and is AI-driven; View Replay shows the flashing "REPLAY" banner
 * and plays back recorded input. Both run cinematic trackside cameras. */
extern int      g_demo_mode;
/* [FIX 2026-05-24 OVERSIGHT] g_wanted_mode_enabled removed — was orphan
 * shadow of g_td5.wanted_mode_enabled. Use td5_game_is_wanted_mode() or
 * g_td5.wanted_mode_enabled directly. */
extern int      g_special_encounter;
extern int      g_race_rule_variant;
extern int      g_game_type;
extern int      g_split_screen_mode;
extern int      g_racer_count;
extern uint32_t g_tick_counter;
extern int      g_actor_slot_map[TD5_MAX_VIEWPORTS];
extern void    *g_actor_pool;
extern uint8_t *g_actor_table_base;   /* gRuntimeSlotActorTable base (0x4AB108) */
extern int      g_actorSlotForView[TD5_MAX_VIEWPORTS];  /* per-view actor slot */
extern int      g_actorBaseAddr;      /* legacy actor-table base handle */
extern void    *g_route_data;         /* loaded LEFT/RIGHT.TRK route blob */
/* Race-end / render-mode state owned by td5_game.c, read by the HUD. */
extern int      g_special_render_mode;
extern int      g_pending_finish_timer;
extern int      g_race_end_state;
extern int32_t  g_actor_best_lap;
extern int32_t  g_actor_best_race;

/* --- Split-screen --- */
int td5_game_get_player_slot(int viewport);
int td5_game_is_split_screen(void);

/* --- [S27] Controller-disconnect pause + reconnect modal ---
 * device_disconnect_active(): 1 while the race is frozen for a missing pad.
 * player_disconnected(p): 1 if player p's controller is currently gone (drives
 * the per-viewport HUD modal). Source-port feature (no original equivalent). */
int td5_game_device_disconnect_active(void);
int td5_game_player_disconnected(int player);
int td5_game_net_remote_pause_slot(void);
#ifndef TD5RE_RELEASE
void td5_game_debug_toggle_sim_device_loss(int player);  /* F9 dev test hook */
#endif

/* --- Post-race results accessors (for Screen [24]/[25]) ---
 * [CONFIRMED @ 0x0040AAD0/0x0040AB80] sort populates s_results
 * [CONFIRMED @ 0x00413BC0] ScreenPostRaceNameEntry reads these fields */
int32_t td5_game_get_result_primary(int slot);   /* finish time ticks */
int32_t td5_game_get_result_secondary(int slot); /* accumulated points */
int32_t td5_game_get_result_top_speed(int slot); /* top speed raw units */
int32_t td5_game_get_result_avg_speed(int slot); /* average speed raw units */
int     td5_game_get_race_order(int pos);         /* slot index at finish position pos */
int     td5_game_slot_is_finished(int slot);      /* 1 if post_finish_metric_base != 0 */
int     td5_game_slot_finish_place(int slot);     /* 1-based place captured when slot finished (0 = still racing) */
int     td5_game_get_finish_position(int slot);   /* 0-based finish position (0 = 1st), -1 if not set */
int32_t td5_game_get_best_lap_time(int slot);     /* best lap ticks, lap-type tracks */
int     td5_game_get_highest_position(int slot);  /* best (lowest) race_position seen this race; 0=1st, -1 if invalid */
int     td5_game_get_wanted_kills(int slot);       /* cop-chase arrest count (actor->special_encounter_state low byte) */
void    td5_game_add_wanted_score(int slot, int points); /* cop-chase ram points -> accumulated_score (+0x2C8) */
void    td5_game_add_wanted_kill(int slot);              /* cop-chase bust -> wanted_kills + actor+0x384 */

/* Re-sort s_results in place using the current game_type's metric.
 * Called from Screen [24] case 0 to mirror RunRaceResultsScreen behaviour.
 * [CONFIRMED @ 0x00422480 case 0] */
void td5_game_sort_results(void);

#ifndef TD5RE_RELEASE
/* Dev/test harness: fabricate a finished-race result (s_results / s_metrics /
 * s_slot_state for the 6 racing slots) so the post-race frontend screens can be
 * previewed via a StartScreen jump. Gated by TD5RE_INJECT_POSTRACE in the
 * StartScreen path; compiled out of the release build. */
void td5_game_inject_demo_results(void);

/* Self-test director: force the current race to exit cleanly to the menu
 * (pause-menu QUIT TO MENU sequence, no menu). No-op outside GAMESTATE_RACE. */
void td5_game_selftest_end_race(void);
#endif

/* --- Split-Screen Steering Balance (0x4036B0) --- */
void td5_game_update_split_screen_balance(void);

/* --- Wanted-mode / replay / traffic accessors (0x40A2B0 region) --- */
int  td5_game_is_replay_active(void);
/* Set replay mode (1=playback path / 0=recording path).
 * Gates the WriteOpen-vs-ReadOpen branch in td5_game_init_race_session
 * at td5_game.c:1902. Frontend View-Replay must call this; otherwise the
 * race re-entry path runs WriteOpen which memsets the recording buffer
 * (closes todo-view-replay-restarts-race-2026-05-19 per DA-M1 audit). */
void td5_game_set_replay_mode(int v);
/* Set attract-demo mode (1=attract demo / 0=clear). Drives the "DEMO MODE"
 * status text + cinematic camera. Independent of replay (View Replay). */
void td5_game_set_demo_mode(int v);
/* 1 when EITHER View Replay OR attract demo is active — used to select the
 * cinematic trackside camera + replay/demo HUD bitmask + ESC-to-exit. */
int  td5_game_is_cinematic_race(void);
int  td5_game_get_traffic_variant(int traffic_index);
int  td5_game_get_cop_actor_index(void);
int  td5_game_is_wanted_mode(void);
/* [MP GAME MODES: COP CHASE 2026-06-22] Effective PRIMARY cop slot (-1 if not a
 * cop chase). SP wanted -> cop slot 0; MP cop chase -> the lowest configured cop
 * slot. For "is THIS slot a cop?" use td5_game_cop_chase_is_cop (multi-cop aware);
 * cop_chase_cop_slot is only the single representative cop. */
int  td5_game_cop_chase_cop_slot(void);
/* [MP COP CHASE multi-cop 2026-06-24] 1 when `slot` is a cop in the active cop
 * chase: SP/AI = the single cop slot; MP human = any bit set in cop_slot_mask.
 * Returns 0 outside an active cop chase (wanted mode off). */
int  td5_game_cop_chase_is_cop(int slot);
int  td5_game_cop_chase_is_suspect(int slot);
/* Active racer count for an MP cop chase (human suspects + an AI-cop slot when
 * the cop is an AI). 0 = not an MP cop chase -> callers keep the faithful SP
 * wanted-mode 2-slot field. Keeps the field-size decisions (g_racer_count,
 * s_slot_state, the AI's g_slot_state) consistent so the AI cop slot stays
 * active instead of being disabled with slots 2..5. */
int  td5_game_mp_cop_chase_field(void);
/* [MP COP CHASE results 2026-06-25] True when a LOCAL (split-screen) MP cop chase
 * is active — gates the dedicated COPS/SUSPECTS results layout and the "CHASE
 * RESULTS" title. SP wanted mode and net play keep the normal results table. */
int  td5_game_mp_cop_chase_active(void);
/* [MP COP CHASE INFECT 2026-06-25] INFECT mode: an arrested suspect is converted
 * into a cop (random police car) instead of being parked. infect_enabled() gates
 * the whole feature; infect_request() (called from the arrest path) queues a
 * busted suspect; process_pending_infections() performs the deferred per-slot
 * car-swap once per frame at a safe point; reset_infect_state() clears the
 * converted/pending sets per race. */
int  td5_game_cop_chase_infect_enabled(void);
void td5_game_infect_request(int suspect_slot);
void td5_game_process_pending_infections(void);
void td5_game_reset_infect_state(void);
/* [MP COP CHASE results 2026-06-25] Per-suspect "time of arrest" (race-timer
 * ticks). set() stamps the elapsed time once at the bust transition; get()
 * returns 0 for a suspect that was never arrested (results screen shows '-').
 * Port-added: the original stores no arrest time (AwardWantedDamageScore @
 * 0x0043d690 writes only the bust count + ram score). */
void    td5_game_set_arrest_time(int slot);
int32_t td5_game_get_arrest_time(int slot);

/* [MP GAME MODES: CUP 2026-06-22] Self-contained best-of-X series orchestrator
 * for the "Cup" mode. begin() arms the series (track list, zeroed standings,
 * teams); award() tallies points by finish position after each race; advance()
 * moves to the next race; track() is the current race's track index. */
void td5_game_mp_cup_begin(void);
void td5_game_mp_cup_award(void);
void td5_game_mp_cup_advance(void);
void td5_game_mp_cup_end(void);
int  td5_game_mp_cup_active(void);
int  td5_game_mp_cup_has_next(void);
int  td5_game_mp_cup_current(void);
int  td5_game_mp_cup_race_count(void);
int  td5_game_mp_cup_points(int slot);       /* cumulative cup total for slot     */
int  td5_game_mp_cup_race_points(int slot);  /* points earned in the race just run */
int  td5_game_mp_cup_team(int slot);
int  td5_game_mp_cup_ai_difficulty(int slot);  /* [CUP TEAM SELECT] per-opponent difficulty 0..2, -1=inherit global */
int  td5_game_mp_cup_team_mode(void);
int  td5_game_mp_cup_track(void);
/* [NET GAME MODES 2026-07-04] Net cup sync: the host advances the series and
 * broadcasts the current race index; every client adopts it so the track,
 * "race X of Y" and standings stay in lockstep. race_finished() = the current
 * race's points have been tallied (the host uses it to decide when to advance
 * before broadcasting the next race). */
void td5_game_mp_cup_set_current(int idx);
int  td5_game_mp_cup_race_finished(void);

/* [MP GAME MODES: TRAFFIC FAIRNESS 2026-06-22] 1 when local split-screen MP
 * should keep shared traffic deterministic (no permanent player-caused wrecks). */
int  td5_game_mp_traffic_fair(void);
/* [TRAFFIC BATTLE 2026-06-28] 1 when the current race is the Traffic Destruction
 * battle mode (mp_mode_config.mode == TD5_MP_MODE_TRAFFIC_BATTLE). Consulted by
 * the scoring hook, the dynamic-spawner force, the results sort, and the HUD. */
int  td5_game_battle_mode_active(void);
/* [DRAG RACE 2026-06-30] 1 when the MP lobby selected DRAG RACE mode. */
int  td5_game_drag_mp_active(void);
/* [TRAFFIC BATTLE checkpoints] 1 while the CHECKPOINTS deadline chaser is armed. */
int  td5_game_battle_chase_active(void);
/* Spans a racer slot is ahead of the creeping deadline (negative = caught). */
int  td5_game_battle_chase_gap(int slot);
/* 1 while the in-race pause menu is open (audio layer reads this to suspend the
 * cop-chase siren refresh during pause so it re-arms on resume). */
int  td5_game_is_pause_menu_active(void);
/* Pause-menu overlay queries consumed by the HUD renderer (td5_hud.c). */
int  td5_game_pause_endrace_confirm_active(void);
int  td5_game_pause_action_confirm(void);
/* Final finishing position of the player (post-race victory HUD). */
int  td5_game_get_victory_position(void);
/* 1 during the pre-race countdown, 0 once the race is running. */
int  td5_game_is_countdown_active(void);
void td5_game_advance_sky_rotation(void);
/* Cop-chase tracked-marker intensity (orig g_wantedTargetTrackerActive
 * @ 0x004BF500; decays 0x200/sub-tick, clamped to [0, 0x1000]). Consumed
 * by render-side RenderTrackedActorMarker port for pulse scaling. */
int32_t td5_game_get_wanted_target_tracker(void);
/* Slot index of the wanted-mode tracked actor (orig g_wantedTargetSlotIndex
 * @ 0x004bf51c; .data-init=0, no binary writers). */
int     td5_game_get_wanted_target_slot(void);

/* Active per-race RNG seed (replicated: host-broadcast session_seed at race
 * start, restored from the recording for replays). Sim-deterministic features
 * that must stay in lockstep across netplay peers seed a private, sim-tick-only
 * stream from this instead of the shared CRT rand(). */
uint32_t td5_game_get_race_seed(void);

/* [DRAG DYNAMIC FIELD] Number of cars / track lanes for a dynamic drag race,
 * = clamp(num_human + num_ai, 2, 8). Read by both the drag slot/spawn logic
 * (td5_game.c) and the track lane-widener (td5_track.c). See knobs in the
 * definition. */
int td5_game_drag_field_size(void);
/* [DRAG RACE MP 2026-06-30] Cars that actually race (humans-only in MP drag), vs the
 * wider lane count above — extra lanes carry no car. */
int td5_game_drag_active_racers(void);

/* [DRAG LENGTH] Extra strip copies to append for the drag LENGTH option (0 for
 * SHORT/MEDIUM/LONG, 1 for EPIC), and the finish-line span on the resulting
 * strip (base_ring = the original pre-repeat ring length). */
int td5_game_drag_length_repeats(void);
int td5_game_drag_length_finish_span(int start, int base_ring);

#endif /* TD5_GAME_H */
