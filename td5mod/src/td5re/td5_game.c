/**
 * td5_game.c -- Main game loop, state machine, game flow
 *
 * Reimplements the 4-state game FSM: INTRO -> MENU -> RACE -> BENCHMARK
 * and the per-frame simulation/render pipeline.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "td5_game.h"
#include "td5_track.h"
#include "td5_track_registry.h"
#include "td5_fmv.h"
#include "td5_sound.h"
#include "td5_input.h"
#include "td5_ai.h"
#include "td5_asset.h"
#include "td5_physics.h"
#include "td5_render.h"
#include "td5_jobs.h"     /* Phase B Stage 2b: threaded pane recording */
#include "td5_rcmd.h"     /* Phase B render-transform: per-pane CPU command lists */
/* [Phase B render-transform] TD5_MT_PARALLEL_BUILD: build the per-pane CPU
 * command lists on the worker pool. ENABLED 2026-06-11 — the blocker (the mesh
 * transform / lighting / proj-UV writers stomping the SHARED mesh blob from
 * concurrent panes) is fixed: the transform now copies each mesh into a
 * per-pane g_rs vertex workspace, dispatch rebases pointers into it, and
 * depth-bucket prims are copied at queue time (see the RenderScratch workspace
 * note in td5_render.c). Replay stays serial-in-pane-order on the main thread.
 * Whole path remains gated behind [Render] ThreadedPanes (default 0). */
#define TD5_MT_PARALLEL_BUILD 1
#include "../../../re/include/td5_actor_struct.h"
#include "td5_camera.h"
#include "td5_frontend.h"
#include "td5_laneassist.h"   /* seed each human's lane-assist enable at race start */
#include "td5_hud.h"
#include "td5re.h"
#include "td5_platform.h"
#include "td5_net.h"
#include "td5_save.h"
#include "td5_config.h" /* shared TD5RE_* env-knob accessors */

#include "td5_vfx.h"
#include "td5_light.h"    /* [DYNAMIC LIGHTS] per-frame headlight registry */
#include "td5_arcade.h"   /* ARCADE mode: pickup pads + power-ups */
#include "td5_damage.h"   /* [CAR DAMAGE] health reset + knockout completion gate */
#include "td5_tutorial.h" /* first-race controller-tutorial overlay */
#include "td5_trace.h"
#include "td5_profile.h"
#include "td5_benchmark.h"
#include "td5_selftest.h"  /* in-session automated test suite (dev builds) */

int td5_trace_current_sim_tick(void) {
    return g_td5.simulation_tick_counter;
}

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

/* ========================================================================
 * Game State Globals (migrated from td5re_stubs.c — owned by this module)
 * ======================================================================== */

int     g_actorSlotForView[TD5_MAX_VIEWPORTS] = {0};
int     g_actorBaseAddr         = 0;
void   *g_actor_pool            = NULL;
void   *g_actor_base            = NULL;
uint8_t *g_actor_table_base     = NULL;
int     g_actor_slot_map[TD5_MAX_VIEWPORTS] = {0};
int     g_racer_count           = 0;
int     g_game_type             = 0;
int     g_split_screen_mode     = 0;
int     g_traffic_slot_base     = TD5_LEGACY_RACE_SLOTS;  /* racer/traffic boundary; see td5_types.h */
int     g_replay_mode           = 0;
/* Attract-demo flag (orig g_replayModeFlag @ 0x4AAF64). Distinct from
 * g_replay_mode (orig g_inputPlaybackActive @ 0x466E9C). See td5_game.h. */
int     g_demo_mode             = 0;
/* [FIX 2026-05-24 OVERSIGHT: wanted-mode-init; orig 0x004aaf68]
 * Removed orphan g_wanted_mode_enabled (was never written; shadowed
 * the live flag g_td5.wanted_mode_enabled). Consumers re-routed to the
 * real flag. Header extern in td5_game.h also dropped. */
int     g_special_encounter     = 0;
int     g_race_rule_variant     = 0;
uint32_t g_tick_counter         = 0;
int     g_special_render_mode   = 0;
int     g_pending_finish_timer  = 0;
int     g_race_end_state        = 0;
int32_t g_actor_best_lap        = 0;
int32_t g_actor_best_race       = 0;
void   *g_route_data            = NULL;

extern int   g_cameraTransitionActive;  /* td5_camera.c */
extern float g_subTickFraction;        /* td5_camera.c -- [0..1) sub-tick interp */
extern int   g_camWorldPos[TD5_MAX_VIEWPORTS][3];   /* td5_camera.c -- per-viewport camera pos (24.8 fixed) */
extern float g_cameraPos[3];            /* td5_camera.c -- float camera pos for render */
extern float g_render_width_f;          /* td5_render.c */
extern float g_render_height_f;         /* td5_render.c */
extern int   g_track_is_circuit;        /* td5_track.c */
extern int   g_track_type_mode;         /* td5_track.c */
extern uint8_t *g_track_environment_config; /* td5_asset.c -- LEVELINF.DAT buffer (0x4AEE20) */

/* Checkpoint logging now routes through the centralized logger (race.log) */

/* ========================================================================
 * Original function addresses and implementation status
 *
 * 0x4493E0  entry                              -- N/A (CRT, not reimplemented)
 * 0x430A90  GameWinMain                        -- DONE (td5_game_init/tick/shutdown)
 * 0x442170  RunMainGameLoop                    -- DONE (td5_game_tick)
 * 0x42AA10  InitializeRaceSession              -- DONE (td5_game_init_race_session)
 * 0x42B580  RunRaceFrame                       -- DONE (td5_game_run_race_frame)
 * 0x42C8E0  ShowLegalScreens                   -- DONE (td5_game_show_legal_screens)
 * 0x442160  InitializeRaceVideoConfiguration   -- ARCH-DIVERGENCE (thunk JMP 0x0042a950;
 *                                                 body collapsed into td5_render_init +
 *                                                 platform bootstrap; see audit note below)
 * 0x414740  InitializeFrontendResourcesAndState-- DONE (delegates to td5_frontend)
 * 0x414B50  RunFrontendDisplayLoop             -- DONE (delegates to td5_frontend)
 * 0x42C2B0  InitializeRaceViewportLayout       -- DONE (td5_game_init_viewport_layout)
 * 0x430CB0  ResetGameHeap                      -- DONE (delegates to td5_plat_heap_reset)
 * 0x40A2B0  AdvancePendingFinishState          -- DONE (advance_pending_finish_state)
 * 0x40A3D0  AccumulateVehicleSpeedBonusScore   -- DONE (accumulate_speed_bonus)
 * 0x40A440  DecayUltimateVariantTimer          -- DONE (decay_ultimate_timer)
 * 0x40A530  AdjustCheckpointTimersByDifficulty -- DONE (adjust_checkpoint_timers)
 * 0x42CC20  BeginRaceFadeOutTransition         -- DONE (td5_game_begin_fade_out)
 * 0x42CBE0  IsLocalRaceParticipantSlot         -- DONE (td5_game_is_local_participant)
 * 0x42CCD0  StoreRoundedVector3Ints            -- DONE (td5_game_store_rounded_vec3)
 * ======================================================================== */

/* ========================================================================
 * L5 promotion sweep audit -- 2026-05-18 (worktree wave3-l5-promo-9-small-game-tier)
 *
 * InitializeRaceVideoConfiguration thunk @ 0x00442160 [ARCH-DIVERGENCE]
 *   Orig body: 5 bytes `JMP 0x0042a950` (single tail-call to the impl). The
 *   thunk itself contains no logic — it is the public symbol that GameWinMain
 *   calls. Port-side there is no thunk because the body has been collapsed.
 *
 * InitializeRaceVideoConfiguration body @ 0x0042a950 [ARCH-DIVERGENCE]
 *   Orig responsibilities (183 bytes):
 *     1) Resolve g_renderDetailLevelPtr = gRenderDetailLevelTable[lang_id]
 *        clamped to 5, where lang_id = SNK_LangDLL_exref[8] - 0x30.
 *     2) Call InitializeRaceRenderGlobals @ 0x0040AE10.
 *     3) Call QueryRaceSharedPointer(4, &g_vehicleProjectionEffectMode, 4).
 *     4) Set _g_raceRenderInitialized = 1.
 *     5) Cache _g_raceVideoConfigCached = *g_appExref.
 *     6) Hard-set g_renderWidthF/g_renderHeightF = 640/480.
 *     7) Call ConfigureProjectionForViewport(640, 480).
 *         [ARCH-DIVERGENCE @ 0x0043E7E0 ConfigureProjectionForViewport]
 *         See td5_render.c:2087 td5_render_configure_projection() for the
 *         port equivalent: orig writes globals (g_projectionDepth,
 *         g_frustumLeftPlaneNormalX/Z, g_frustumTopPlaneNormalY/Z,
 *         g_cachedViewportWidth/Height) consumed by the D3D3 fixed-pipeline
 *         clipper; port writes static module-locals consumed by the D3D11
 *         software-transform path. Same focal/half-plane math
 *         (focal = w*0.5625, h_cos = focal/h_len, etc.). Caller also
 *         resets g_projectionDepthBias = 0x1000 — port mirrors this at
 *         td5_render.c:2537 [CONFIRMED @ 0x0043E7E0].
 *     8) Center pos: gRenderCenter{X,Y} = render{W,H}F * DAT_0045d5d0 (= 0.5).
 *     9) Call InitializeRaceViewportLayout @ 0x0042C2B0.
 *     10) Call LoadStaticTrackTextureHeader.
 *         [ARCH-DIVERGENCE @ 0x00442560 LoadStaticTrackTextureHeader]
 *         Orig: GetArchiveEntrySize/ReadArchiveEntry from "static.hed"
 *         inside "static.zip" allocated via _malloc, parses gTrackTextureCount
 *         (-= 4 unless projection mode == 2), gStaticHedEntryCount, and a
 *         per-page transparency table at Texture_exref+0x24/+0x28 with a
 *         min/max swap gated on DAT_004c3d04. Port equivalent
 *         td5_asset_init_static_atlas() (td5_asset.c:528) reads the same
 *         static.hed directly from "re/assets/static/static.hed" (extracted
 *         offline, no zip archive at runtime) and stores into s_atlas_table
 *         + s_page_metadata. Same end-state — per-page (atlas_x, atlas_y,
 *         w, h, tex_slot) plus per-page metadata. Port skips the orig
 *         DAT_004c3d04 transparency-swap (port writes width/height directly,
 *         not min/max-of-pair, because the static.hed shipped in assets/ is
 *         the already-canonicalised form). [CONFIRMED @ 0x00442560 entry
 *         table parser + 0x004425D0 page-metadata parser; see also
 *         reference_smoke_atlas_static_hed_parse_trap.md for the entry-stride
 *         trap (correct stride = 64 bytes, ints at offset 0x2C).]
 *     11) Set SetRaceTexturePageLoader(LoadRaceTexturePages).
 *         [ARCH-DIVERGENCE @ 0x0040B580 SetRaceTexturePageLoader]
 *         Orig is a 13-byte one-liner that writes a function pointer at
 *         (Set_exref + 0xC), turning LoadRaceTexturePages into the indirect
 *         callback used by the page-build path. Port has no indirect
 *         dispatch — td5_asset.c:1516 LoadRaceTexturePages @ 0x00442770 is
 *         invoked directly by name from the per-race init. Functionally
 *         equivalent at the call site; the function-pointer indirection is
 *         a D3D3-era pattern that doesn't survive the D3D11 rewrite.
 *         [CONFIRMED @ 0x0040B580 single-store body — `MOV [Set_exref+0xC],
 *         param_1; RET`. No port equivalent needed.]
 *     12) Wire CreateEffects_exref = CreateRaceForceFeedbackEffects (FFB
 *         virtual dispatch slot on the DX::FF singleton).
 *         [ARCH-DIVERGENCE @ 0x004285B0 CreateRaceForceFeedbackEffects]
 *         Orig is 719 bytes of DirectInput 5/7 effect creation:
 *         DXInput::EnumerateEffects(joystick, 3) for constant-force candidates,
 *         creates 4 effects at js_exref+0x1c/+0x20/+0x24/+0x28 via the
 *         IDirectInputDevice2/3 vtable (slot 0=steering resistance,
 *         slot 1=frontal impact, slot 2=side impact, slot 3=spring/condition).
 *         Joystick-type bit 0x600 (wheel) selects condition-type 0x22; other
 *         joysticks get 0x12. Port equivalent td5_plat_ff_init() in
 *         td5_platform_win32.c:1274 + td5_input_ff_init() in td5_input.c:1314
 *         enumerates effects via DirectInput8 (IDirectInputDevice8_EnumEffects
 *         filtered by GUID_ConstantForce), then calls
 *         td5_ff_create_constant_effect for slots 0/1/2 and
 *         td5_ff_create_periodic_effect for slot 3. Same 4-effect slot
 *         layout, same per-slot semantics (slot 0/2 X-axis, slot 1 Y-axis,
 *         slot 3 periodic), different DI version (5/7 -> 8) and different
 *         struct sizes (DIEFFECT_DX5 vs DIEFFECT/DI8). The orig's virtual
 *         dispatch through CreateEffects_exref is replaced by a direct
 *         function call. [CONFIRMED @ 0x004285B0 4-effect creation loop.]
 *
 *   Port handles (1)-(8) implicitly via td5_render_init() / DDraw wrapper.
 *   Steps (10)-(12) are spread across the per-race init in
 *   td5_game_init_race_session() and the platform abstraction
 *   (td5_input_ff_*). Steps (3) (4) (5) (12) carry orig-specific machinery
 *   (QueryRaceSharedPointer dpu accessor, virtual FFB dispatch, app cached
 *   handle) that the port replaced with direct calls. Functionally
 *   equivalent at the user-visible level. The header comment above used to
 *   claim "DONE (in td5_game_init)" but td5_game_init handles only FSM
 *   state init — not the video bootstrap. Re-labelled to ARCH-DIVERGENCE
 *   to reflect that the orig's monolithic bootstrap was split.
 *
 * ResetRaceResultsTable @ 0x0040a880 [ARCH-DIVERGENCE]
 *   Orig writes a 6-entry, 20-byte-stride table at 0x0048d988. Per entry:
 *     byte[0] = 0, byte[1] = entry_idx (0..5), word[2] = 0, dword[4..14]=0;
 *     after the loop, entry[0].byte[0] = 1.
 *   Port helper reset_results_table() (td5_game.c:3729) memsets s_results
 *   to zero, then sets s_results[0].slot_flags = 1. Behaviour-equivalent
 *   end-state but port does NOT prime byte[1] = entry_idx. That priming is
 *   redundant in practice because build_results_table() (td5_game.c:3674)
 *   always overwrites slot_index with `i` before any sort/read. No port
 *   reader observes the post-reset/pre-build window where byte[1] would
 *   matter. Documented divergence; not L4.
 *
 * ResetGameHeap @ 0x00430cb0 [ARCH-DIVERGENCE]
 *   Orig: HeapDestroy gGameHeapHandle (gated on first-call sentinel
 *   DAT_004aee4c), HeapCreate(0, 24_000_000, 0), zero gGameHeapAllocTotal,
 *   set sentinel. Port td5_plat_heap_reset() (td5_platform_win32.c:808)
 *   destroys s_game_heap then HeapCreate(0, 1_048_576, 0). Two divergences:
 *     - Initial size 1 MB vs 24 MB (both growable on Windows; no
 *       functional difference at allocation runtime).
 *     - First-call sentinel replaced with `if (s_game_heap)` null-check
 *       (semantically equivalent — same one-shot destroy).
 *
 * IsLocalRaceParticipantSlot @ 0x0042cbe0 [L5 — byte-faithful]
 *   Three-branch dispatch matched exactly: network -> dpu[0xBCC+slot*4],
 *   split-screen -> slot < 2, else -> slot == 0. Port wrapper
 *   td5_game_is_local_participant() (td5_game.c:3912) calls
 *   td5_net_is_slot_active(slot) for the network path, which reads the
 *   same dpu field. Byte-equivalent.
 *
 * DecayTrackedActorMarkerIntensity @ 0x0043d7e0 [L5 — byte-faithful]
 *   Orig: gate on audio-options-overlay; decrement 0x200/tick; clamp
 *   [0, 0x1000] with early-return at zero. Port tick_wanted_target_tracker()
 *   (td5_game.c:322) mirrors all three operations using the pause-menu
 *   gate (orig's audio-options-overlay is the same overlay surface).
 *   Byte-equivalent decrement + bounds.
 *
 * UpdateRaceCameraTransitionTimer @ 0x0040a490 [ARCH-DIVERGENCE]
 *   See detailed audit comment at tick_race_countdown() — same-shape
 *   timer/level/indicator state machine; intentional port-side semantic
 *   choices for paused-flip timing and indicator gating on blank atlas
 *   cells. The orig's ResetRaceCameraSelectionState call at the timer-zero
 *   crossing IS wired (Phase 2 follow-up 2026-05-18 — see delta A note at
 *   tick_race_countdown).
 *
 * BeginRaceFadeOutTransition @ 0x0042cc20 [ARCH-DIVERGENCE]
 *   See detailed audit comment at td5_game_begin_fade_out(). Port
 *   collapses orig's dual-axis dispatch (param drives radial-pulse gate;
 *   g_raceViewportLayoutMode drives fade direction) into a single `param`
 *   axis that already encodes split_screen_mode at all call sites.
 *   Radial-pulse RESET now wired (Tier 4 port 2026-05-24): port calls
 *   td5_hud_reset_radial_pulse() when the orig gate triggers, mirroring
 *   ResetHudRadialPulseOverlay @ 0x0043a210. The pulse render itself
 *   already lives in td5_render_radial_pulse + td5_hud.c:2514 gate.
 *   Fade-direction alternator collapses orig's complex `& 0x80000001`
 *   repair into a single `^= 1` toggle; both converge to alternating
 *   0/1, byte-equivalent in observable state.
 *
 * ResetRaceCameraSelectionState @ 0x00402000 [L5 — byte-faithful]
 *   Port td5_camera.c:1246 mirrors orig's two-branch dispatch (param==0
 *   restore from packed save bytes, param==1 zero everything) and the
 *   two LoadCameraPresetForView calls with identical view indices.
 *   Byte-equivalent.
 * ======================================================================== */

/* ========================================================================
 * Key globals (from original binary)
 *
 * 0x4C3CE8  g_gameState (dword, enum)
 * 0x45D53C  g_appExref (pointer to DX::app object)
 * 0x474C00  g_introMoviePendingFlag
 * 0x474C04  g_frontendResourceInitPending
 * 0x495248  g_startRaceRequestFlag
 * 0x49524C  g_startRaceConfirmFlag
 * 0x4C3D80  g_raceEndFadeState
 * 0x45D5F4  tick decrement constant
 * 0x45D650  max sim tick budget (4.0f)
 * 0x4AAD60  g_gamePaused
 * ======================================================================== */

#define LOG_TAG "td5_game"
/* Original: g_cameraTransitionActive init=0xA000, -=0x100/tick, 160 ticks total
 * Level = timer / 0x2800; levels 4..0 → digits 5..1, then GO at level<0 */
#define TD5_COUNTDOWN_INIT    0xA000
#define TD5_COUNTDOWN_DECR    0x100
#define TD5_COUNTDOWN_LEVEL_DIV 0x2800

/* ========================================================================
 * Module-private state
 * ======================================================================== */

/* Fade system */
static float s_fade_accumulator;            /* 0.0 -> 255.0 */
static int   s_fade_direction_alternator;   /* toggles 0/1 per race */
/* g_raceEndRadialPulseEnabled (orig dword @ 0x004aaefc). Latched to 1 in
 * td5_game_begin_fade_out when the 1st-place victory star pulse fires. When
 * set, the directional race-end fade is SUPPRESSED so the win shows the star
 * wipe ONLY (mutually exclusive in orig RunRaceFrame @ 0x0042b791/0x0042b797).
 * Reset to 0 per race. */
static int   s_race_end_radial_pulse_enabled;

/* Race completion */
static uint32_t s_post_finish_cooldown;     /* 0x483980: 0 = phase1, >0 = phase2 accumulator */

/* Per-actor race state (6 racer slots) */
typedef struct RaceSlotState {
    uint8_t  state;              /* 0=AI-inactive, 1=active, 2=completed, 3=disabled */
    uint8_t  companion_1;       /* 0=racing, 1=finished */
    uint8_t  companion_2;       /* 0=ok, 1=completed-ok, 2=DNF */
    uint8_t  reserved;
} RaceSlotState;

static RaceSlotState s_slot_state[TD5_MAX_RACER_SLOTS];

/* Per-actor metrics */
typedef struct ActorRaceMetric {
    int32_t  post_finish_metric_base;   /* cumulative timer at finish; 0 = not finished */
    int32_t  cumulative_timer;          /* running race timer (ticks) */
    int16_t  checkpoint_index;          /* current checkpoint or lap */
    uint8_t  checkpoint_bitmask;        /* circuit sector bitmask (4-bit) */
    int16_t  normalized_span;           /* position on track (forward progress) */
    int16_t  timer_ticks;               /* countdown timer for checkpoint mode */
    int32_t  accumulated_score;         /* points / speed bonus */
    int32_t  speed_bonus;               /* running speed bonus accumulator */
    int32_t  top_speed;                 /* max speed seen */
    int32_t  average_speed_raw;         /* for finish calculation */
    int16_t  display_position;          /* 0=1st .. 5=6th */
    int16_t  wanted_kills;              /* cop chase busts */
    int16_t  forward_speed;             /* current speed */
    int16_t  skid_factor;               /* current skid intensity */
    int16_t  contact_count;             /* collision count */
    int16_t  lap_split_times[9];        /* per-lap split deltas, 9 entries mirroring actor+0x34E
                                         * [CONFIRMED: checkpoint_split_times[9] in td5_actor_struct.h]
                                         * Circuit: delta loop @ 0x00409E98; P2P: raw at crossing. */
} ActorRaceMetric;

static ActorRaceMetric s_metrics[TD5_MAX_RACER_SLOTS];

/* Race order array (indices into slot table, sorted by position) */
static uint8_t s_race_order[TD5_MAX_RACER_SLOTS];

/* Results table (0x48d988 in original, 6 entries x 20 bytes) */
typedef struct RaceResultEntry {
    uint8_t  slot_flags;
    uint8_t  slot_index;
    int16_t  final_position;
    uint16_t pad1;
    uint16_t pad2;
    int32_t  primary_metric;     /* finish time */
    int32_t  secondary_metric;   /* accumulated points */
    uint8_t  wanted_kills;
    int16_t  speed_bonus;
    int16_t  top_speed;
} RaceResultEntry;

static RaceResultEntry s_results[TD5_MAX_RACER_SLOTS];

/* [MP COP CHASE results 2026-06-25] Per-slot race-timer value (ticks) at the
 * moment a suspect was arrested (busted) in a cop chase. 0 = never arrested.
 * Stamped once at the bust transition (td5_ai_wanted_cop_hit); the cop-chase
 * results screen shows it as the suspect's TIME OF ARREST ('-' when 0). The
 * original binary stores no arrest time — AwardWantedDamageScore @ 0x0043d690
 * writes only the bust count + ram score — so this is a port-added field. */
static int32_t s_arrest_time[TD5_MAX_RACER_SLOTS];

/* Checkpoint timing record (24 bytes = 12 x uint16, from binary at 0x46CBB0)
 * Loaded per-track via pointer table at 0x46CF6C (1-based index). */
typedef struct CheckpointRecord {
    uint16_t checkpoint_count;   /* always 5 in shipped data */
    uint16_t initial_time;       /* countdown start (8.8 fixed-point seconds) */
    struct {
        uint16_t span_threshold; /* span index where checkpoint triggers */
        uint16_t time_bonus;     /* time added on crossing (8.8 FP seconds) */
    } checkpoints[5];
} CheckpointRecord;

static CheckpointRecord s_active_checkpoint;

/* LEVELINF.DAT checkpoint span storage (+0x08..+0x24) */
static int32_t s_levelinf_checkpoint_spans[7]; /* +0x0C..+0x24 from LEVELINF.DAT */
static int32_t s_levelinf_checkpoint_config;   /* +0x08 from LEVELINF.DAT */
/* Finish span for a migrated point-to-point TD6 track (0 = circuit / non-TD6).
 * Resolved once at race init from the TD6 registry; consumed by the P2P finish
 * path in advance_pending_finish_state (TD6 P2P tracks have no checkpoint data,
 * so reaching this span is the only finish trigger). */
static int s_td6_finish_span = 0;

/* [TD6 SYNTHESIZED CHECKPOINTS] Drive-through markers derived from the in-track
 * ring/banner mesh positions (TD6.exe ships NO checkpoint-trigger data — RE'd
 * 2026-06-04: CHECKPT.NUM is loaded but never read, the banner-texture tables
 * and RINGO have zero code refs; the only per-track span table drives fog). So
 * the visible ring/banner meshes are decoration; these spans make them
 * FUNCTIONAL. s_td6_cp_count is 0 for the 8 migrated tracks without banner art
 * and for ALL faithful TD5 tracks (gated on g_active_td6_level at race init, so
 * a TD5 level reusing a TD6 output-level number is never affected). */
static int     s_td6_cp_count = 0;
static int     s_td6_cp_spans[5];
static uint8_t s_td6_cp_index[TD5_MAX_RACER_SLOTS];

/* [#R3-7 2026-06-19] TD6 P2P checkpoint-timer knobs. TD6.exe shipped NO checkpoint
 * timer for these tracks (RE-confirmed: the system was removed), so there are no
 * original values — these are tunable invented defaults (packed hi-byte = seconds). */
static int td6_cp_timer_enabled(void) {
    static int s = -1;
    if (s < 0) { s = td5_env_flag_on("TD5RE_TD6_CP_TIMER"); }
    return s;
}
static int td6_cp_time_secs(void) {
    static int s = -1;
    if (s < 0) { const char *e = getenv("TD5RE_TD6_CP_TIME"); int v = (e && e[0]) ? atoi(e) : 0;
                 s = (v >= 10 && v <= 250) ? v : 90; }
    return s;
}
static int td6_cp_bonus_secs(void) {
    static int s = -1;
    if (s < 0) { const char *e = getenv("TD5RE_TD6_CP_BONUS"); int v = (e && e[0]) ? atoi(e) : -1;
                 s = (v >= 0 && v <= 250) ? v : 30; }
    return s;
}

/* LEVELINF.DAT additional fields */
static int32_t s_levelinf_track_subvariant;  /* +0x54: 36 for race, -1 for cup */
static int32_t s_levelinf_span_count;        /* +0x58: track ring length (redundant with STRIP.DAT) */

/* Hardcoded checkpoint timing table extracted from DAT_0046CBB0.
 * 40 records, each 12 uint16s = 24 bytes (record_count × 0x18 = 0x3C0 bytes,
 * table ends at 0x0046CF70 which is the pointer-array base DAT_0046CF6C+4).
 *
 * Indexing: the original binary routes schedule_index → pool_id (via
 * gScheduleToPoolIndex @ 0x00466894) → g_trackPoolIndex (via
 * gTrackPoolSpanCountTable @ 0x00466D50) → record_idx = g_trackPoolIndex - 1.
 *
 * k_schedule_to_checkpoint_index below collapses that two-stage chain for
 * the 19 UI schedule slots (schedule 19 = drag race, hardcoded). Comments
 * label each record with the schedule slot that actually reaches it, not
 * with the sequential record order. */
static const uint16_t k_checkpoint_table[40][12] = {
    {5,25659,  869,15360, 1511,11520, 2061,15360, 2618,10240, 3074,    0}, /* 0  ← sched 10 Keswick */
    {5,25659,  826,11520, 1429, 5120, 1652, 7680, 1926,15360, 2516,    0}, /* 1  ← sched 11 SanFrancisco */
    {5,20539,  768,17920, 1379,16640, 2090,16640, 2776,11520, 3221,    0}, /* 2  ← sched 12 Bern */
    {5,17979,  623,12800, 1175,15360, 1751, 8960, 2181, 8960, 2552,    0}, /* 3  ← sched 13 Kyoto */
    {5,17979,  747, 7680, 1006,12800, 1533,14080, 1978,17920, 2754,    0}, /* 4  ← sched 14 Washington */
    {5,16699,  609,10240, 1029,10240, 1560,12800, 2140,16640, 2567,    0}, /* 5  ← sched 15 Munich */
    {5,17979,  556,17920, 1113,14080, 1663,14080, 2305,23040, 3060,    0}, /* 6 */
    {5,20539,  715, 8960,  989, 8960, 1212,14080, 1815,12800, 2508,    0}, /* 7 */
    {5,15419,  585,19200, 1271,20480, 1982,17920, 2593,17920, 3282,    0}, /* 8 */
    {5,15419,  466, 8960,  896,12800, 1472,14080, 2024,12800, 2528,    0}, /* 9 */
    {5,21819,  901,10240, 1346,11520, 1873, 7680, 2132,14080, 2755,    0}, /*10 */
    {5,15419,  519,15360, 1099,11520, 1630, 7680, 2050, 8960, 2523,    0}, /*11 */
    {5,17979,  651,14080, 1128,12800, 1599,14080, 2115,11520, 2574,    0}, /*12 ← sched  8 Honolulu */
    {5,10299,  486,10240, 1057,11520, 1655, 8960, 2071,11520, 2658,    0}, /*13 ← sched  2 Sydney */
    {5,17979,  660,15360, 1297,14080, 1840,10240, 2193,12800, 2656,    0}, /*14 ← sched  9 Tokyo */
    {5,23099,  629,10240, 1182,11520, 1608,12800, 2211,14080, 2644,    0}, /*15 ← sched  1 Scotland */
    {5,17979,  685,16640, 1446,11520, 1842,12800, 2281,17920, 2988,    0}, /*16 ← sched  3 BlueRidge */
    {5,16699,  606,15360, 1122,12800, 1593,16640, 2070,15360, 2610,    0}, /*17 */
    {5,15419,  665,10240, 1081,12800, 1679,11520, 2250,11520, 2635,    0}, /*18 */
    {5,17979,  583,11520,  936,14080, 1479,17920, 2116,14080, 2657,    0}, /*19 */
    {5,16699,  544,19200, 1147,11520, 1573,14080, 2126,14080, 2684,    0}, /*20 */
    {5,23099,  827,12800, 1266,12800, 1662,19200, 2423,15360, 2989,    0}, /*21 */
    {5,17979,  738, 7680, 1116,14080, 1707,10240, 2094,11520, 2649,    0}, /*22 ← sched  0 Moscow */
    {5,17979,  694, 8960, 1081,16640, 1672, 8960, 2050,17920, 2668,    0}, /*23 */
    {5,30779,  106,10240, 1511,11520, 2061,12800, 2618,14080, 3120,    0}, /*24 ← sched 16 Cheddar */
    {5,30779,   25,10240, 1511,11520, 2061,12800, 2618,14080, 3120,    0}, /*25 ← sched  4 Jordan */
    {5,30779,  119,10240, 1511,11520, 2061,12800, 2618,14080, 3120,    0}, /*26 ← sched  7 Italy */
    {5,30779,   56,10240, 1511,11520, 2061,12800, 2618,14080, 3120,    0}, /*27 ← sched  6 Hawaii */
    {5,30779,  116,10240, 1511,11520, 2061,12800, 2618,14080, 3120,    0}, /*28 ← sched  5 Newcastle */
    {1,30779,  204,    0,    0,    0,    0,    0,    0,    0,    0,    0}, /*29 */
    {1,30779,  204,    0,    0,    0,    0,    0,    0,    0,    0,    0}, /*30 */
    {5,30779,  119,10240, 1511,11520, 2061,12800, 2618,14080, 3120,    0}, /*31 */
    {5,30779,   56,10240, 1511,11520, 2061,12800, 2618,14080, 3120,    0}, /*32 */
    {5,30779,  119,10240, 1511,11520, 2061,12800, 2618,14080, 3120,    0}, /*33 */
    {5,30779,   56,10240, 1511,11520, 2061,12800, 2618,14080, 3120,    0}, /*34 */
    {5,30779,  116,10240, 1511,11520, 2061,12800, 2618,14080, 3120,    0}, /*35 */
    {5,30779,   47,10240, 1511,11520, 2061,12800, 2618,14080, 3120,    0}, /*36 ← sched 18 HouseOfBez */
    {5,30779,   47,10240, 1511,11520, 2061,12800, 2618,14080, 3120,    0}, /*37 */
    {5,30779,   35,10240, 1511,11520, 2061,12800, 2618,14080, 3120,    0}, /*38 ← sched 17 Jamaica */
    {5,30779,   35,10240, 1511,11520, 2061,12800, 2618,14080, 3120,    0}, /*39 */
};

/* schedule_index → checkpoint record index.
 * Derived from the original's two-stage lookup:
 *   pool_id = gScheduleToPoolIndex[schedule_index]
 *   record_idx = gTrackPoolSpanCountTable[pool_id] - 1
 * 19 UI schedule slots (slot 19 is drag race; hardcoded elsewhere). */
static const uint8_t k_schedule_to_checkpoint_index[19] = {
    22, /* 0  Moscow */
    15, /* 1  Scotland */
    13, /* 2  Sydney */
    16, /* 3  BlueRidge */
    25, /* 4  Jordan */
    28, /* 5  Newcastle */
    27, /* 6  Hawaii */
    26, /* 7  Italy */
    12, /* 8  Honolulu */
    14, /* 9  Tokyo */
     0, /*10  Keswick */
     1, /*11  SanFrancisco */
     2, /*12  Bern */
     3, /*13  Kyoto */
     4, /*14  Washington */
     5, /*15  Munich */
    24, /*16  Cheddar */
    38, /*17  Jamaica */
    36, /*18  HouseOfBez */
};

/* Benchmark state */
static int   s_benchmark_image_load_pending;
static void *s_benchmark_image_data;

/* Position points tables */
static const int s_championship_points[TD5_MAX_RACER_SLOTS] = {15, 12, 10, 5, 4, 3};
static const int s_ultimate_points[TD5_MAX_RACER_SLOTS]     = {1000, 500, 250, 0, 0, 0};

/* Viewport layout */
typedef struct ViewportRect {
    int x, y, w, h;
} ViewportRect;

static ViewportRect s_viewports[TD5_MAX_VIEWPORTS];

/* [#9 SPLIT-LAYOUT FIX 2026-06-15] Per-race viewport -> grid-cell map.
 * s_view_cell[vp] = the split-grid cell (0..cols*rows-1, row-major) that
 * viewport vp is laid out in. Built once per race in
 * td5_game_init_viewport_layout() from the position-screen permutation
 * (td5_frontend_mp_view_actor_slot): each viewport is placed in the cell its
 * driver actually CHOSE, and the cell(s) no player chose are left for the HUD
 * map/standings filler. Without the fix (or with the identity permutation that
 * AutoRace / the harness / non-positioned MP use) this is simply vp==cell, so
 * the layout is byte-identical to the old "panes fill cells 0..N-1" behaviour.
 * s_view_cell_count mirrors the live viewport_count for query-time clamping. */
static int s_view_cell[TD5_MAX_VIEWPORTS];
static int s_view_cell_count = 0;

/* TD5RE_SPLIT_LAYOUT_FIX: default ON; exactly "0" reverts to the legacy
 * identity cell map (viewport vp -> cell vp, empty cells = the tail). Cached. */
static int td5_split_layout_fix_on(void) {
    static int s_cached = -1;
    if (s_cached < 0) {
        const char *e = getenv("TD5RE_SPLIT_LAYOUT_FIX");
        s_cached = (e && e[0] == '0' && e[1] == '\0') ? 0 : 1;
        TD5_LOG_I(LOG_TAG, "split-layout cell map (#9) %s (TD5RE_SPLIT_LAYOUT_FIX=%s)",
                  s_cached ? "ENABLED" : "disabled", e ? e : "default");
    }
    return s_cached;
}

/* Replay / benchmark timing */
static uint32_t s_race_end_timer_start;

/* Victory-overlay window. When the 1st-place star fires, hold the race (and
 * thus the fade-to-results exit) for this long so the white star animation and
 * the centered finishing number are readable instead of a ~0.5s flash.
 * [user 2026-05-30: "animation too fast" / "number sometimes not present".] */
#define TD5_VICTORY_HOLD_MS 2500u
static uint32_t s_victory_hold_start;   /* ms timestamp the star armed (0 = none) */
static int      s_finish_position_display; /* 1-based place captured at finish (0 = none) */
/* [MP per-viewport finish 2026-06-13] 1-based finishing place captured PER SLOT
 * the moment that slot crosses the line (0 = still racing). Drives each
 * split-screen viewport's own end-of-race indicator + coast-to-stop, so one
 * player finishing no longer ends the race for the others. */
static int      s_slot_finish_place[TD5_MAX_RACER_SLOTS];

/* [TRAFFIC BATTLE 2026-06-28] CHECKPOINTS win-condition "chaser": a deadline that
 * creeps up the track. s_battle_chase_span is its track position in 24.8 fixed
 * spans; any racer whose progress falls behind it is caught (eliminated). Inert
 * unless the battle win condition is CHECKPOINTS. */
static int      s_battle_chase_active;
static int32_t  s_battle_chase_span;          /* 24.8 fixed track span of the deadline */
static int      s_battle_chase_caught[TD5_MAX_RACER_SLOTS];

/* [REPLAY RESULTS PRESERVE 2026-06-27] A "View Replay" re-enters the race via
 * g_td5.race_requested -> init_race_session, which WIPES the live race-results
 * display state (s_results / s_metrics / s_slot_state / ...) so the recorded run
 * can play back from the start. The port's playback is NOT a faithful re-run (the
 * recorded input buffer runs out, cars coast and only partly re-cross the line),
 * so letting the replay's completion rebuild the results would show the player the
 * replay's PARTIAL outcome instead of the real race — most visibly in MP cup,
 * where the polluted table also fed the cup standings ("replay counts as a new
 * race"). The original avoided this with a per-race table reset + faithful
 * playback (RE: completion runs during replay @0x42B5ED-0x42B626; table reset
 * @0x40A880, rebuild @0x40A8C0); the port instead snapshots the real results when
 * a replay starts and restores them when it ends. Knob TD5RE_REPLAY_KEEP_RESULTS=0
 * reverts to the old (polluting) behaviour. */
static int             s_replay_results_saved = 0;
static RaceResultEntry s_saved_results[TD5_MAX_RACER_SLOTS];
static ActorRaceMetric s_saved_metrics[TD5_MAX_RACER_SLOTS];
static RaceSlotState   s_saved_slot_state[TD5_MAX_RACER_SLOTS];
static uint8_t         s_saved_race_order[TD5_MAX_RACER_SLOTS];
static int             s_saved_slot_finish_place[TD5_MAX_RACER_SLOTS];
static int32_t         s_saved_arrest_time[TD5_MAX_RACER_SLOTS];
static int             s_saved_finish_position_display;

/* TD5RE_REPLAY_KEEP_RESULTS gate (default ON). Cached + logged once. "0" reverts
 * to the legacy behaviour where a View Replay rebuilds (and pollutes) the
 * race-results table / MP cup standings. */
static int replay_keep_results_on(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("TD5RE_REPLAY_KEEP_RESULTS");
        v = (e && e[0] == '0' && e[1] == '\0') ? 0 : 1;
        TD5_LOG_I(LOG_TAG, "replay keep-results %s (TD5RE_REPLAY_KEEP_RESULTS=%s)",
                  v ? "ENABLED" : "disabled", e ? e : "default");
    }
    return v;
}

static int      s_replay_mode;       /* 1 = input-playback "View Replay" race */
static int      s_demo_mode;         /* 1 = attract-mode demo race */
/* Per-race RNG seed captured at the start of a recorded (non-replay) race and
 * restored when that race is replayed, so the AI/traffic RNG reproduces the
 * recorded run. Mirrors the original's g_savedRngSeed (@0x4AAD64) save/restore
 * in InitializeRaceSession @0x42AA22-0x42AA4B. */
static uint32_t s_saved_race_seed;
static int      s_replay_abort_pending; /* 1 = ESC pressed during replay → exit to results */
static int      s_race_countdown_ticks;
static int      s_race_countdown_state;
static int      s_pause_menu_active;
static int      s_pause_menu_cursor;   /* [S31][END RACE NOW 2026-06-30][GATED 2026-07-02]
                                        * 0=VIEW 1=SOUND 2=RADIO 3=CONTINUE 4=RESTART.
                                        * Rows past 4 are conditional and computed
                                        * dynamically (row_endrace/row_lobby/row_quit/
                                        * row_exit locals in the input-handling block):
                                        * END RACE NOW only if num_human_players>1
                                        * (local split-screen MP); BACK TO LOBBY only if
                                        * network_active or num_human_players>1 (any MP);
                                        * QUIT TO MENU / EXIT GAME always last. */
static int      s_pause_lobby_pending; /* [S31] BACK TO LOBBY fade in progress;
                                        * run-race-frame returns 4 when done */

/* [S27 2026-06-05] Controller-disconnect pause + per-viewport reconnect modal.
 * SOURCE-PORT FEATURE — the 1999 binary had no hot-plug handling, so this is
 * documented new code, not an RE reconstruction. While ANY required (joystick)
 * player's controller is missing the race is frozen and that player's split-
 * screen pane shows a "reconnect" modal. */
static uint32_t s_player_disconnected_mask;   /* bit p = player p's controller is gone */
static int      s_disconnect_pause_active;    /* sim frozen because a pad is missing */
static int      s_disconnect_prev_paused;     /* g_td5.paused to restore on reconnect */
#ifndef TD5RE_RELEASE
static uint32_t s_sim_loss_mask;              /* DEV: simulate loss for these players */
static uint32_t s_sim_loss_race_start_ms;     /* DEV: race-start wall clock for timed sim */
#endif

/* Wanted-mode tracker marker intensity (DecayTrackedActorMarkerIntensity @ 0x43D7E0).
 * Original global g_wantedTargetTrackerActive. Decays 0x200/sub-tick, clamped to
 * [0, 0x1000]. Gate: audio-options overlay (= pause menu) pauses decay.
 *
 * [CONFIRMED @ 0x0043d7e0 DecayTrackedActorMarkerIntensity; L5 promotion
 *  sweep audit 2026-05-18] -- Byte-faithful port. Operations matched:
 *    1) Gate on overlay/pause-menu (orig: g_audioOptionsOverlayActive).
 *    2) Decrement 0x200 only when value > 0.
 *    3) Early-return after clamp-to-zero on underflow.
 *    4) Upper-clamp 0x1000.
 *  Identical control flow, identical constants. */
static int32_t  s_wanted_target_tracker;

static void tick_wanted_target_tracker(void) {
    if (s_pause_menu_active) return;
    if (s_wanted_target_tracker > 0) {
        s_wanted_target_tracker -= 0x200;
    }
    if (s_wanted_target_tracker < 0) {
        s_wanted_target_tracker = 0;
        return;
    }
    if (s_wanted_target_tracker > 0x1000) {
        s_wanted_target_tracker = 0x1000;
    }
}
static int      s_pause_input_done;    /* reset per-frame, set after first tick processes input */
static int      s_prev_esc_state;      /* edge detector for ESC key */
static int      s_prev_pause_act;      /* edge detector for the rebindable PAUSE action */
/* [BUG 5a 2026-06-15] Per-FRAME ESC edge for the cinematic (View Replay / attract
 * demo) abort. The faithful ESC handling lives INSIDE the fixed-step sim while-loop
 * (s_prev_esc_state, esc_edge), which only runs on frames that drain a 30 Hz tick.
 * At render rates above 30 Hz most frames drain zero ticks, so a brief ESC tap that
 * lands entirely on no-tick frames was never edge-detected and the replay would not
 * exit (the controller exit worked because it latches a one-shot across frames).
 * s_prev_esc_frame mirrors the live ESC key once per frame, OUTSIDE the loop, and
 * arms s_replay_esc_exit on the rising edge during a cinematic race so the abort is
 * caught regardless of tick cadence. Gated by TD5RE_REPLAY_EXIT (shared with the
 * controller exit): "0" reverts to the in-loop-ESC-only behaviour. */
static int      s_prev_esc_frame;      /* per-frame edge detector for cinematic ESC exit */
static int      s_replay_esc_exit;     /* one-shot: set on a per-frame ESC edge in a cinematic race */
/* [BUGFIX #15 2026-06-15] Pause-menu nav edge-detection state. PROMOTED from
 * function-static block locals to file scope so the pause-menu OPEN block can
 * SEED them with the currently-held direction keys (UP is usually accelerate,
 * DOWN is brake). Without seeding, opening the menu while holding UP fired the
 * up-nav edge on the first menu frame and moved the cursor off CONTINUE (row 2)
 * onto SOUND (row 1). Latching the held state on open makes navigation act only
 * on a fresh release-then-press edge after the menu is up. Keyboard + pad. */
static int      s_prev_down;           /* edge detector: pause-menu DOWN nav */
static int      s_prev_up;             /* edge detector: pause-menu UP nav */
static int      s_prev_left;           /* edge detector: pause-menu LEFT (slider) */
static int      s_prev_right;          /* edge detector: pause-menu RIGHT (slider) */
static int      s_prev_enter;          /* edge detector: pause-menu CONFIRM (Enter / pad A) */
static int      s_prev_jb;             /* edge detector: pause-menu pad B = back/continue */
static int      s_pause_exit_pending;  /* 1 = ESC exit fade in progress, return 2 when fade done */
static int      s_pause_restart_pending; /* [REWORK 2026-06-05/S15] 1 = pause-menu RESTART RACE fade in progress, return 3 when fade done */
static int      s_pause_options_dirty; /* 1 = a pause volume slider changed; flush to td5re.ini on close [PART B] */
static int      s_pause_endrace_confirm; /* [END RACE NOW 2026-06-30] 1 = force-finish confirm prompt up in the pause menu */
static int      s_pause_action_confirm; /* [PAUSE CONFIRM 2026-07-02] 0=none, else which pause-menu action
                                          * is awaiting Y/N confirm: 1=RESTART RACE 2=QUIT TO MENU 3=EXIT GAME
                                          * 4=BACK TO LOBBY. Armed only in local split-screen MP
                                          * (num_human_players>1) -- a shared pause menu means one pad's stray
                                          * press shouldn't silently end the race for everyone else on the
                                          * screen. Single-player and network-only (1 local human) keep the
                                          * original immediate behavior (BACK TO LOBBY is MP-only regardless). */

/* ========================================================================
 * Forward declarations (internal helpers)
 * ======================================================================== */

static int  check_race_completion(uint32_t sim_delta);
static void battle_chase_tick(void);                  /* [TRAFFIC BATTLE checkpoints] */
static void build_results_table(void);
static void reset_results_table(void);
static void capture_replay_results_snapshot(void);   /* [REPLAY RESULTS PRESERVE] */
static void restore_replay_results_snapshot(void);   /* [REPLAY RESULTS PRESERVE] */
static void sort_results_by_time_asc(void);
static void sort_results_by_score_desc(void);
static void update_race_order(void);
static void td5_game_force_finish_race(void); /* [END RACE NOW 2026-06-30] pause-menu force-finish */
static void td5_game_pause_execute_action(int which); /* [PAUSE CONFIRM 2026-07-02] RESTART/QUIT/EXIT body */
static int  active_racer_count(void);   /* # of participating racers (state != 3) */
static void advance_pending_finish_state(int slot, uint32_t sim_delta);
static void tick_pending_finish_timer(int slot);
static void sync_actor_race_metrics(int slot);
static void accumulate_speed_bonus(int slot);
static void decay_ultimate_timer(int slot);
static void adjust_checkpoint_timers(int slot);
static void display_loading_screen_tga(void);
static void reset_race_countdown(void);
static void tick_race_countdown(void);
static const char *td5_game_state_name(TD5_GameState state);
static uint32_t td5_game_normalized_dt_to_accum(float dt_normalized);
static float td5_game_normalized_dt_to_seconds(float dt_normalized);
void td5_game_update_split_screen_balance(void);

TD5_Actor *td5_game_get_actor(int slot)
{
    int total = td5_game_get_total_actor_count();

    if (!g_actor_table_base || slot < 0 || slot >= total) {
        return NULL;
    }

    return (TD5_Actor *)(g_actor_table_base + (size_t)slot * TD5_ACTOR_STRIDE);
}

int td5_game_get_total_actor_count(void)
{
    int total = g_td5.total_actor_count;

    if (total <= 0 && g_actor_table_base) {
        if (g_td5.time_trial_enabled) {
            total = (g_td5.split_screen_mode > 0) ? 2 : 1;
        } else if (g_td5.traffic_enabled) {
            total = TD5_MAX_TOTAL_ACTORS;
        } else {
            total = TD5_MAX_RACER_SLOTS;
        }
    }

    if (total < 0) {
        return 0;
    }
    if (total > TD5_MAX_TOTAL_ACTORS) {
        return TD5_MAX_TOTAL_ACTORS;
    }

    return total;
}

/* Minimap checkpoint-connector accessors. RenderTrackMinimapOverlay @ 0x0043A220
 * reads checkpoint spans from g_raceCheckpointTablePtr (0x4aed88) to draw the
 * connector road quads (Quad3/Quad4). s_active_checkpoint is the byte-faithful
 * mirror of that table: count at +0, then 4-byte entries with the span at +0
 * (CheckpointRecord layout). Expose the spans so td5_hud.c can replicate the
 * connector draws. The original reads the count as a single byte. */
int td5_game_get_minimap_checkpoint_count(void)
{
    /* [2026-06-12 task#11] TD6 tracks have no native checkpoint records
     * (s_active_checkpoint is empty), but we synthesize functional checkpoints
     * from the in-track banner meshes into s_td6_cp_spans. Surface THOSE to the
     * minimap so the dashed checkpoint markers draw on migrated TD6 tracks like
     * the original game. Native TD5 keeps s_active_checkpoint. */
    if (g_active_td6_level > 0 && s_td6_cp_count > 0) {
        int c = s_td6_cp_count;
        return (c > 5) ? 5 : c;
    }
    int c = (int)(uint8_t)s_active_checkpoint.checkpoint_count;
    if (c > 5) c = 5; /* checkpoints[] is sized 5 */
    return c;
}

int td5_game_get_minimap_checkpoint_span(int idx)
{
    if (idx < 0 || idx >= td5_game_get_minimap_checkpoint_count())
        return -1;
    if (g_active_td6_level > 0 && s_td6_cp_count > 0)
        return s_td6_cp_spans[idx];
    return (int)s_active_checkpoint.checkpoints[idx].span_threshold;
}

static void set_countdown_indicator_state(int value);

/* ========================================================================
 * Module Init / Shutdown
 * ======================================================================== */

/* [L5 promotion sweep audit 2026-05-18 — ARCH-DIVERGENCE]
 *   GameWinMain @ 0x00430A90 (orig WinMain bootstrap) is decomposed in
 *   the source port across three locations:
 *     - main.c:WinMain      -- Win32 entry; FPU rounding (_RC_DOWN per
 *                              pool4 pilot), INI load, crash handler,
 *                              D3D11/wrapper backend bring-up, message
 *                              pump, td5re_frame loop, shutdown.
 *     - td5re.c:td5re_init  -- module bring-up dispatcher equivalent to
 *                              orig DXWin::Initialize + initial state
 *                              transition prep.
 *     - td5_game_init       -- THIS function. Mirrors the post-DXWin
 *                              GameWinMain block that seeds the 4-state
 *                              FSM into INTRO and clears all per-race
 *                              slot/metric/result state.
 *
 *   Orig 0x00430A90 layout (line-by-line audit):
 *     1. DXWin::Environment(lpCmdLine)                       → main.c (INI parse + log init).
 *     2. Width/Height/BPP defaults (0x280/0x1e0/0x10) +
 *        g_appExref[0x20]=g_titleStr +
 *        g_appExref[0x188]=1 (run flag).                     → main.c WinMain locals + Backend_Init.
 *     3. DXWin::Initialize()                                 → Backend_CreateDevice + td5_platform_win32_init.
 *     4. while (!quit) {
 *          PeekMessage drain;
 *          if (DAT_00473b6c == 0)
 *              RunMainGameLoop();    ← per-frame game tick
 *          else
 *              DXDraw::ConfirmDX6 +
 *              DXWin::DXInitialize +
 *              InitializeRaceVideoConfiguration;
 *        }                                                   → main.c:while(running) PeekMessage + td5re_frame.
 *     5. DXWin::Uninitialize + DestroyWindow                 → td5re_shutdown + Backend_Shutdown.
 *
 *   ARCH-DIVERGENCEs:
 *     a. DDraw → D3D11: original uses DXWin/DXDraw/DXInput COM stack.
 *        Port uses Backend_* + WrapperDirectDraw shim (ddraw_wrapper/).
 *     b. DXWin::ConfirmDX6 / DXDraw recreation path (DAT_00473b6c branch)
 *        has no port equivalent — D3D11 device-lost handling is via the
 *        wrapper backend instead. Functionally equivalent: rebuild
 *        graphics resources without quitting.
 *     c. g_appExref hot-fields (+0x180 quit latch, +0x184/+0x168 gates,
 *        +0x188 run flag, +0x138 shutdown callback, +0x4 hInstance,
 *        +0xc8 nCmdShow) collapsed into g_td5 + main.c's `running`.
 *     d. nCmdShow / hPrevInstance / hInstance ignored (no SDI app shell).
 *     e. FPU rounding mode is set BEFORE everything else (_RC_DOWN, pool4
 *        pilot finding) — port-only INI/log init must run under the same
 *        rounding regime as the sim core.
 *   Game-loop semantics (PeekMessage drain → game tick → loop until quit)
 *   are wire-equivalent. The 4-state FSM and per-frame logic live in
 *   td5_game_tick (= RunMainGameLoop @ 0x00442170).
 */
int td5_game_init(void) {
    /* Initialize game state machine (0x442170 entry, 0x430A90 GameWinMain setup) */
    g_td5.game_state = TD5_GAMESTATE_INTRO;
    g_td5.intro_movie_pending = g_td5.ini.skip_intro ? 0 : 1;
    g_td5.frontend_init_pending = 1;

    s_fade_accumulator = 0.0f;
    s_fade_direction_alternator = 0;
    s_post_finish_cooldown = 0;
    s_benchmark_image_load_pending = 1;
    s_benchmark_image_data = NULL;
    s_replay_mode = 0;
    s_demo_mode = 0;
    s_replay_abort_pending = 0;
    s_race_end_timer_start = 0;
    s_race_countdown_ticks = 0;
    s_race_countdown_state = 0;

    memset(s_slot_state, 0, sizeof(s_slot_state));
    memset(s_metrics, 0, sizeof(s_metrics));
    memset(s_results, 0, sizeof(s_results));
    memset(s_viewports, 0, sizeof(s_viewports));
    g_td5.total_actor_count = 0;

    for (int i = 0; i < TD5_MAX_RACER_SLOTS; i++)
        s_race_order[i] = (uint8_t)i;

    TD5_LOG_I(LOG_TAG, "Game module initialized, state=INTRO");
    return 1;
}

void td5_game_shutdown(void) {
    /* Release any lingering benchmark data */
    if (s_benchmark_image_data) {
        td5_plat_heap_free(s_benchmark_image_data);
        s_benchmark_image_data = NULL;
    }
    /* Release the benchmark sample buffer (mirrors orig's process-exit
     * cleanup of g_benchmarkSampleBuffer; safe to call without prior
     * init). */
    td5_benchmark_shutdown();
    TD5_LOG_I(LOG_TAG, "Game module shut down");
}

/* ========================================================================
 * Main tick -- one frame of RunMainGameLoop (0x442170)
 *
 * 4-state FSM: INTRO -> MENU -> RACE -> BENCHMARK
 * Called once per frame from td5re_frame().
 *
 * L5 promotion sweep audit (2026-05-18, TD5_pool0 read-only) -- the
 * 4-state FSM dispatcher is structurally faithful to orig 991 bytes
 * (0x00442170..0x0044254F). Three ARCH-DIVERGENCEs documented.
 *
 *   Per-state mapping (orig switch(g_gameState) -> port td5_game_tick):
 *
 *   case GAMESTATE_INTRO:
 *     orig: if (g_introMoviePendingFlag) {
 *             if (g_appExref+0x14C && !g_appExref+0x13C) {
 *               LogReport("Playing Movie\n");
 *               PlayIntroMovie();
 *             }
 *             g_introMoviePendingFlag = 0;
 *           }
 *           DXD3D::InitializeMemoryManagement();
 *           DXD3D::SetRenderState();
 *           ShowLegalScreens();
 *           g_gameState = GAMESTATE_MENU; (fallthrough)
 *     port: identical sequence -- intro_movie_pending guard +
 *           td5_game_play_intro_movie() (delegates to td5_fmv) +
 *           td5_render_init() (replaces D3D3 memory-manager init +
 *           render-state setup) + td5_game_show_legal_screens()
 *           (delegates to td5_fmv_show_legal_screens) + fallthrough to
 *           MENU. Behaviour-equivalent.
 *
 *   case GAMESTATE_MENU:
 *     orig: g_frontendResourceInitPending guard ->
 *           InitializeFrontendResourcesAndState();
 *           RunFrontendDisplayLoop();
 *           if (g_startRaceRequestFlag || g_startRaceConfirmFlag) {
 *             g_frontendResourceInitPending = 1; flags = 0;
 *             InitializeRaceSession(); g_gameState = RACE;
 *           }
 *           g_appExref+0x16C = 1; (frontend active mirror)
 *     port: 1:1 mapping -- frontend_init_pending +
 *           td5_frontend_init_resources, td5_frontend_display_loop,
 *           race_requested/race_confirmed flags + td5_game_init_race_session.
 *           Adds AutoRace + StartScreen handling for the INI-driven test
 *           harness (cosmetic, gated to ini.auto_race / ini.start_screen).
 *           [ARCH-DIVERGENCE: g_appExref+0x16C mirror (frontend-active
 *           flag fed back to the DX::app exref) does not exist in the
 *           port -- that exref carried DDraw/DSound capability flags and
 *           there is no consumer in the source-port equivalent. The
 *           orig +0x180 quit latch maps to g_td5.quit_requested.]
 *
 *   case GAMESTATE_RACE:
 *     orig: iVar2 = RunRaceFrame();
 *           g_appExref+0x16C = 0;
 *           if (iVar2) {
 *             dd_exref+0x1730 = 0;
 *             if (g_appExref+0x150) DXD3D::FullScreen(dd_exref+0x1690);
 *             g_gameState = (benchmark ? BENCHMARK : MENU);
 *             DXPlay::UnSync();
 *           }
 *     port: result = td5_game_run_race_frame();
 *           result=1 -> normal completion -> RaceResults screen,
 *           result=2 -> ESC quit -> MainMenu,
 *           benchmark flag routes to GAMESTATE_BENCHMARK.
 *           Behaviour-equivalent. DXPlay::UnSync analog is in
 *           td5_net_tick() teardown (the port's net layer owns its own
 *           synchronisation lifecycle).
 *           [ARCH-DIVERGENCE: orig's DXD3D::FullScreen restore-on-race-
 *           exit (gated on g_appExref+0x150 fullscreen-mode flag) is
 *           absent -- D3D11 swap-chain mode-switch is owned by the
 *           wrapper, not by the game's render-state.]
 *
 *   case GAMESTATE_BENCHMARK:
 *     orig: g_benchmarkModeActive = 0;
 *           if (g_benchmarkImageLoadPending) {
 *             DX::FOpen(FPSName)
 *             + DX::Allocate(0x50000) + DX::Allocate(0xA0000)
 *             + DX::FRead + DX::ImageProTGA;
 *             g_benchmarkImageLoadPending = 0;
 *           }
 *           IDirectDrawSurface::Lock(..., DDLOCK_NOSYSLOCK, ...) ->
 *             manual blit 640x480 16bpp pixels into back surface ->
 *             IDirectDrawSurface::Unlock + DXDraw::Flip;
 *           if (DXInputGetKBStick(0)) {
 *             DX::DeAllocate(decoded);
 *             DX::DeAllocate(raw);
 *             g_benchmarkImageLoadPending = 1;
 *             g_gameState = GAMESTATE_MENU;
 *           }
 *     port: td5_asset_load_png_to_buffer("re/assets/benchmark.png", ...)
 *           + td5_plat_render_upload_texture(0, pixels, w, h, 0)
 *           + td5_plat_present(0); polls td5_plat_input_get_keyboard()
 *           for any keypress -> back to MENU.
 *           [ARCH-DIVERGENCE: the orig's DDraw Lock + manual 16bpp
 *           per-row copy with bounds clipping (iVar4 < 0x280, iVar5 <
 *           0x4B000, /2 ptr math) does not exist in D3D11. The port
 *           uploads as a 32bpp texture and lets the swap-chain present
 *           it. Pixel format flags 0xF800/0x7E0/0x1F (5/6/5 RGB) in the
 *           orig Image_exref are obsolete; the port's PNG loader emits
 *           BGRA and the upload accepts that natively.]
 *
 *   No code edit needed. Effective level after audit: L5 +
 *   [ARCH-DIVERGENCE] (3 items above) for the DX::app+exref mirrors and
 *   the DDraw Lock/blit/Flip benchmark presentation path.
 * ======================================================================== */

int td5_game_tick(void) {
    /* Self-test director (dev builds; inert unless [SelfTest] Enabled=1).
     * Runs before the state switch so it can observe MENU/RACE transitions
     * and re-arm auto_race between scripted scenarios. */
    td5_selftest_tick();

    /* Poll network subsystem (discovery, connection management) */
    td5_net_tick();

    switch (g_td5.game_state) {

    /* ------------------------------------------------------------------
     * GAMESTATE_INTRO (0): Play intro movie, init render, show legals,
     * then fall through to MENU.
     * ------------------------------------------------------------------ */
    case TD5_GAMESTATE_INTRO:
        /* Step 1: Play intro movie if pending and capable */
        if (g_td5.intro_movie_pending) {
            td5_game_play_intro_movie();
            if (g_td5.quit_requested) return 1;
            g_td5.intro_movie_pending = 0;
        }

        /* Step 2: Initialize render memory management and state */
        td5_render_init();

        /* Step 3: Show legal / splash screens (skip if SkipIntro is set) */
        if (!g_td5.ini.skip_intro)
            td5_game_show_legal_screens();

        /* Step 4: Transition to MENU (fallthrough) */
        TD5_LOG_I(LOG_TAG, "State transition: %s -> %s",
                  td5_game_state_name(TD5_GAMESTATE_INTRO),
                  td5_game_state_name(TD5_GAMESTATE_MENU));
        g_td5.game_state = TD5_GAMESTATE_MENU;
        /* FALLTHROUGH */

    /* ------------------------------------------------------------------
     * GAMESTATE_MENU (1): Frontend resource init, display loop, race
     * start detection.
     * ------------------------------------------------------------------ */
    case TD5_GAMESTATE_MENU:
        /* Initialize frontend resources if pending */
        if (g_td5.frontend_init_pending) {
            td5_frontend_init_resources();
            g_td5.frontend_init_pending = 0;
        }

        /* AutoRace: skip frontend, set up race from INI and go straight to loading */
        if (g_td5.ini.auto_race && !g_td5.race_requested) {
            td5_frontend_auto_race_setup();
            g_td5.ini.auto_race = 0;  /* one-shot: don't re-trigger after race ends */
        }

        /* StartScreen: jump to a specific screen on first frontend entry.
         * Fires once after resources are ready; ignored when AutoRace consumed
         * the boot slot above.  -1 = normal flow. */
        if (g_td5.ini.start_screen >= 0 && g_td5.ini.start_screen < TD5_SCREEN_COUNT) {
            TD5_LOG_I(LOG_TAG, "StartScreen=%d: jumping to screen %d",
                      g_td5.ini.start_screen, g_td5.ini.start_screen);
            /* Natural flow runs init_resources via Screen_LocalizationInit.
             * StartScreen bypasses that; load resources directly so fonts,
             * the white fallback page, and the bg gallery are present on
             * the first tick of the target screen. Idempotent: each loader
             * has an internal guard. */
            td5_frontend_init_resources();
#ifndef TD5RE_RELEASE
            /* Post-race preview harness: when TD5RE_INJECT_POSTRACE is set and
             * the jump targets a post-race screen (23..27), fabricate a finished
             * race so the screen renders real data instead of all-zero columns.
             * Must run BEFORE the screen's case-0 (next display-loop tick) reads
             * the result statics. See td5_game_inject_demo_results. */
            if (g_td5.ini.start_screen >= TD5_SCREEN_HIGH_SCORE &&
                g_td5.ini.start_screen <= TD5_SCREEN_CUP_WON) {
                const char *inj = getenv("TD5RE_INJECT_POSTRACE");
                if (inj && inj[0] && inj[0] != '0') {
                    td5_game_inject_demo_results();
                    TD5_LOG_I(LOG_TAG, "StartScreen=%d: injected demo post-race "
                              "results (TD5RE_INJECT_POSTRACE)", g_td5.ini.start_screen);
                }
            }
            /* [layout preview harness 2026-06-29] TD5RE_MP_SIMUL_PREVIEW=N with
             * StartScreen=20 (CAR_SELECTION) jumps straight to the N-player
             * simultaneous car-select grid so the split-screen pane layout can be
             * screenshotted without N controllers. See frontend_mp_simul_preview_setup.
             * [CUP TEAM ROSTER 2026-07-04] Also honoured for StartScreen=39
             * (MP_TEAM_SELECT): slot 0 becomes the host, slots 1..N-1 become dev
             * "ADD AI PLAYER" bots (team_select_slot_is_ai's existing dev-bot
             * branch already treats them as AI opponents), so the roster box +
             * AI skill-slider layout can be screenshotted without N controllers
             * or a real cup AI-opponent count. */
            if (g_td5.ini.start_screen == TD5_SCREEN_CAR_SELECTION ||
                g_td5.ini.start_screen == TD5_SCREEN_MP_TEAM_SELECT) {
                const char *mpv = getenv("TD5RE_MP_SIMUL_PREVIEW");
                if (mpv && mpv[0]) {
                    extern void frontend_mp_simul_preview_setup(int);
                    frontend_mp_simul_preview_setup(atoi(mpv));
                }
            }
#endif
            td5_frontend_set_screen((TD5_ScreenIndex)g_td5.ini.start_screen);
            g_td5.ini.start_screen = -1;  /* one-shot */
        }

        /* Run one frame of the frontend display loop */
        td5_frontend_display_loop();

        /* Check if a race was requested */
        if (g_td5.race_requested || g_td5.race_confirmed) {
            g_td5.frontend_init_pending = 1;   /* re-init frontend on return */
            g_td5.race_confirmed = 0;
            g_td5.race_requested = 0;

            /* Heavy synchronous race session init (loading screen shown inside) */
            td5_game_init_race_session();

            TD5_LOG_I(LOG_TAG, "State transition: %s -> %s",
                      td5_game_state_name(TD5_GAMESTATE_MENU),
                      td5_game_state_name(TD5_GAMESTATE_RACE));
            g_td5.game_state = TD5_GAMESTATE_RACE;
            return 0;
        }
        break;

    /* ------------------------------------------------------------------
     * GAMESTATE_RACE (2): Run one race frame. On completion, transition
     * to MENU or BENCHMARK.
     * ------------------------------------------------------------------ */
    case TD5_GAMESTATE_RACE: {
        int result = td5_game_run_race_frame();
        if (result == 3) {
            /* [REWORK 2026-06-05/S15] Pause-menu RESTART RACE: the fade-out in
             * run_race_frame already released race resources; re-init the SAME
             * race session (reads track/car/opponents/laps/direction straight
             * from g_td5, all unchanged) and stay in RACE. Mirrors the
             * menu->race entry path's init call without rebuilding the AI
             * schedule, so opponents/track/laps are identical. */
            TD5_LOG_I(LOG_TAG, "Pause RESTART RACE: re-initializing race session (same params)");
            td5_game_init_race_session();
            g_td5.game_state = TD5_GAMESTATE_RACE;
            return 0;
        }
        if (result != 0) {
            /* Race is over. Determine next state.
             * result=1: normal race completion (fade finished) -> results screen
             * result=2: ESC/pause menu exit -> main menu */

            /* [REPLAY RESULTS PRESERVE 2026-06-27] If the race that just ended was
             * a View Replay, restore the real race results captured at replay-init
             * so the results / cup-standings screen shows the genuine outcome (not
             * the replay's partial result), and clear the replay flags so the next
             * race is treated as a normal race. */
            if (s_replay_results_saved) {
                restore_replay_results_snapshot();
                td5_game_set_replay_mode(0);
                td5_input_set_replay_mode(0);
                td5_input_set_playback_active(0);
                TD5_LOG_I(LOG_TAG, "Replay ended (result=%d) -> real results restored, replay flags cleared", result);
            }

            if (g_td5.benchmark_active) {
                /* [CONFIRMED @ 0x00428D80 WriteBenchmarkResultsTgaReport]:
                 * orig RunRaceFrame calls this on race-end when benchmark
                 * mode is active.  Port emits a portable plain-text
                 * report (ARCH-DIVERGENCE: no DDraw TGA glyph blit). */
                td5_benchmark_write_report(NULL);
                TD5_LOG_I(LOG_TAG, "State transition: %s -> %s",
                          td5_game_state_name(TD5_GAMESTATE_RACE),
                          td5_game_state_name(TD5_GAMESTATE_BENCHMARK));
                g_td5.game_state = TD5_GAMESTATE_BENCHMARK;
            } else {
                g_td5.game_state = TD5_GAMESTATE_MENU;
                if (result == 4) {
                    /* [S31] BACK TO LOBBY -- the frontend picks the lobby this
                     * race was launched from (net / local MP / main menu). */
                    TD5_LOG_I(LOG_TAG, "Race exited -> back to lobby");
                    td5_frontend_return_to_lobby();
                } else if (result == 2) {
                    /* ESC quit OR a net lockstep loss -> leave the session. A
                     * genuine connection loss (host quit / timeout / dropped link)
                     * shows the CONNECTION LOST notice; a deliberate ESC goes
                     * straight to the main menu. [2026-06-19] */
                    int net_lost = g_td5.network_active && td5_net_is_connection_lost();
                    TD5_LOG_I(LOG_TAG, "Race aborted -> %s",
                              net_lost ? "connection-lost notice" : "main menu");
                    td5_frontend_leave_net_session();
                    if (net_lost)
                        td5_frontend_show_net_disconnect("Lost connection to the host");
                    else
                        td5_frontend_set_screen(TD5_SCREEN_MAIN_MENU);
                } else if (s_demo_mode) {
                    /* [ATTRACT DEMO 2026-06-25] A demo race that runs to a natural
                     * finish goes straight back to the main menu — NO results screen
                     * (an attract demo never shows standings). ESC during the demo
                     * already returns via the result==2 path above; this handles the
                     * AI-driven race actually reaching the finish. Clear the demo flag
                     * so the menu is in a normal state (the next race launch re-clears
                     * it via frontend_init_race_schedule anyway). */
                    TD5_LOG_I(LOG_TAG, "Demo race finished -> main menu (no results)");
                    td5_game_set_demo_mode(0);
                    td5_frontend_set_screen(TD5_SCREEN_MAIN_MENU);
                } else {
                    /* Normal race finish — go to race results screen */
                    TD5_LOG_I(LOG_TAG, "Race finished -> results screen");
                    td5_frontend_set_screen(TD5_SCREEN_RACE_RESULTS);
                }
            }
            return 0;
        }
        break;
    }

    /* ------------------------------------------------------------------
     * GAMESTATE_BENCHMARK (3): Display benchmark TGA, wait for keypress,
     * return to MENU.
     * ------------------------------------------------------------------ */
    case TD5_GAMESTATE_BENCHMARK: {
        g_td5.benchmark_active = 0;

        /* Load and display benchmark results TGA if pending */
        if (s_benchmark_image_load_pending) {
            /*
             * Original loads FPSName_exref TGA file:
             *   alloc 0x50000 for raw data + 0xA0000 for decoded pixels,
             *   decode via DX::ImageProTGA, blit 640x480 to primary surface.
             *
             * Source port: load the TGA through the asset pipeline and
             * present it via the platform render clear + present path.
             */
            void *bm_pixels = NULL;
            int bm_w = 0, bm_h = 0;
            if (td5_asset_load_png_to_buffer("re/assets/benchmark.png",
                                              TD5_COLORKEY_NONE, &bm_pixels, &bm_w, &bm_h)) {
                td5_plat_render_upload_texture(0, bm_pixels, bm_w, bm_h, 0);
                td5_plat_present(0);
                free(bm_pixels);
            } else {
                TD5_LOG_W(LOG_TAG, "benchmark.png not found");
            }
            s_benchmark_image_load_pending = 0;
        }

        /* Poll for any keypress to dismiss */
        const uint8_t *kb = td5_plat_input_get_keyboard();
        if (kb) {
            for (int k = 0; k < 256; k++) {
                if (kb[k]) {
                    /* Keypress detected: return to MENU */
                    s_benchmark_image_load_pending = 1;
                    TD5_LOG_I(LOG_TAG, "State transition: %s -> %s",
                              td5_game_state_name(TD5_GAMESTATE_BENCHMARK),
                              td5_game_state_name(TD5_GAMESTATE_MENU));
                    g_td5.game_state = TD5_GAMESTATE_MENU;
                    break;
                }
            }
        }
        break;
    }

    default:
        TD5_LOG_E(LOG_TAG, "Unknown game state %d", g_td5.game_state);
        g_td5.game_state = TD5_GAMESTATE_MENU;
        break;
    }

    return g_td5.quit_requested;
}

/* ========================================================================
 * State Machine Accessors
 * ======================================================================== */

void td5_game_set_state(TD5_GameState state) {
    g_td5.game_state = state;
}

TD5_GameState td5_game_get_state(void) {
    return g_td5.game_state;
}

#ifndef TD5RE_RELEASE
static TD5_Actor *td5_game_local_player_actor(void);  /* defined below */

/* [SELFTEST 2026-07-02] Programmatic race exit for the self-test director:
 * the pause-menu QUIT TO MENU sequence without the menu (same recipe the
 * net-lockstep-failure path uses) — fade out, release resources when the
 * fade completes, run_race_frame returns 2, FSM goes back to MENU. */
void td5_game_selftest_end_race(void)
{
    if (g_td5.game_state != TD5_GAMESTATE_RACE) return;
    if (s_pause_exit_pending) return;   /* exit already in flight */
    TD5_Actor *pl = td5_game_local_player_actor();
    s_pause_menu_active = 0;
    s_pause_exit_pending = 1;
    s_finish_position_display = pl ? (int)pl->race_position + 1 : 1;
    td5_game_begin_fade_out(0);
    TD5_LOG_I(LOG_TAG, "selftest: forced race exit (fade-out started)");
}
#endif

int td5_game_get_player_lap(int slot)
{
    if (slot < 0 || slot >= TD5_MAX_RACER_SLOTS) return 0;
    return (int)s_metrics[slot].checkpoint_index;
}

/* Live forward-progress span index (actor +0x82) for a slot, or 0 when out of
 * range / no actor. Used by the race-results table to estimate a still-racing
 * car's finish time from its pace (remaining spans / average pace). */
int td5_game_get_slot_span(int slot)
{
    if (slot < 0 || slot >= TD5_MAX_RACER_SLOTS) return 0;
    TD5_Actor *a = td5_game_get_actor(slot);
    return a ? (int)a->track_span_normalized : 0;
}

/* Returns cumulative race timer ticks (30/sec) for lap_index 0,
 * or the split time for lap_index 1-9. Used by HUD. */
int32_t td5_game_get_race_timer(int slot, int lap_index)
{
    if (slot < 0 || slot >= TD5_MAX_RACER_SLOTS) return 0;
    if (lap_index == 0) return s_metrics[slot].cumulative_timer;
    if (lap_index >= 1 && lap_index <= 9)
        return (int32_t)s_metrics[slot].lap_split_times[lap_index - 1];
    return 0;
}

/* ========================================================================
 * Race results / metrics accessors (for td5_frontend.c post-race screen)
 * [CONFIRMED @ 0x0048d988] s_results mirrors original ResultsTable at that addr
 * [CONFIRMED @ 0x40AAD0 / 0x40AB80] sort functions populate final_position
 * ======================================================================== */

/* Race-results accessors.
 *
 * Normally s_results is populated by build_results_table() when the race
 * completes naturally (all racers finished). If the player quits / DNFs,
 * build_results_table never runs and s_results stays zero — but the
 * underlying s_metrics has been ticked the whole race. Fall back to the
 * raw metric so Screen_RaceResults' View Race Data path shows the
 * actual run instead of all "-" / 0. */

/* Returns the primary_metric (finish time ticks) for a given slot.
 * 0 = not finished or out-of-range. [CONFIRMED: DAT_0048d990 = primary base] */
int32_t td5_game_get_result_primary(int slot)
{
    if (slot < 0 || slot >= TD5_MAX_RACER_SLOTS) return 0;
    int32_t v = s_results[slot].primary_metric;
    if (v == 0) v = s_metrics[slot].post_finish_metric_base;
    return v;
}

/* Returns the secondary_metric (accumulated score/points) for a given slot.
 * [CONFIRMED: DAT_0048d98c = secondary base] */
int32_t td5_game_get_result_secondary(int slot)
{
    if (slot < 0 || slot >= TD5_MAX_RACER_SLOTS) return 0;
    int32_t v = s_results[slot].secondary_metric;
    if (v == 0) v = s_metrics[slot].accumulated_score;
    return v;
}

/* Returns the top_speed for a given slot (in internal units).
 * [CONFIRMED: results entry +0x18 = top_speed]
 *
 * [FIX 2026-05-25 hud-metrics; orig 0x004066E2 / 0x00422061]
 *   ActorRaceMetric.top_speed has ZERO writers in the port (dead field).
 *   Orig BuildResultsTable @ 0x0040A8C0 reads peak from actor +0x330
 *   (psVar7[0x155]) and orig results-screen @ 0x00422061 reads from
 *   accumulated_distance / timing_frame_counter table separately.
 *   The actual peak-speed accumulator IS populated on actor->peak_speed
 *   (+0x330) by td5_physics_update_vehicle_actor:1015. Fall back to the
 *   live actor field so the HUD shows the correct value even when the
 *   stale s_metrics/s_results pair is zero. */
int32_t td5_game_get_result_top_speed(int slot)
{
    if (slot < 0 || slot >= TD5_MAX_RACER_SLOTS) return 0;
    int32_t v = (int32_t)s_results[slot].top_speed;
    if (v == 0) v = (int32_t)s_metrics[slot].top_speed;
    if (v == 0) {
        TD5_Actor *actor = td5_game_get_actor(slot);
        if (actor) v = (int32_t)actor->peak_speed;
    }
    return v;
}

/* [#10] Per-racer extended telemetry accessor for the post-race summary screen.
 * Thin forwarder to the physics-owned g_race_metrics[] array (collisions, air
 * time, drifts, plus a running top/avg from planar velocity). NULL out of range. */
const TD5_RaceMetrics *td5_game_get_metrics(int slot)
{
    return td5_physics_get_metrics(slot);
}

/* Returns the average speed accumulator (raw) for a given slot.
 * Port uses s_metrics.average_speed_raw for the actor avg speed.
 * [CONFIRMED: DAT_0048d994 in ScreenPostRaceNameEntry case 4]
 *
 * [FIX 2026-05-25 hud-metrics; orig 0x0040672B/0x00422001]
 *   ActorRaceMetric.average_speed_raw has ZERO writers in the port (dead
 *   field). Orig actor +0x332 (average_speed_metric) IS populated by
 *   td5_physics_update_vehicle_actor:1032 as
 *     accumulated_distance / timing_frame_counter.
 *   Fall back to the live actor field so the HUD avg-speed reading is
 *   non-zero even when build_results_table never ran (DNF/quit). */
int32_t td5_game_get_result_avg_speed(int slot)
{
    if (slot < 0 || slot >= TD5_MAX_RACER_SLOTS) return 0;
    int32_t v = s_metrics[slot].average_speed_raw;
    if (v == 0) {
        TD5_Actor *actor = td5_game_get_actor(slot);
        if (actor) v = (int32_t)actor->average_speed_metric;
    }
    return v;
}

/* Returns the highest (best) position achieved during the race for slot.
 * [CONFIRMED @ 0x00422216 DrawRaceDataSummaryPanel reads actor+0x380 grip_reduction]
 *
 * The orig overloads actor +0x380 as the running MIN of race_position
 * observed during the race; UpdateVehicleActor @ 0x00406823 clamps
 *   grip_reduction = min(grip_reduction, race_position)
 * so the field naturally tracks "best position ever seen".
 *
 * [FIX 2026-05-25 hud-metrics; orig 0x00422216]
 *   Port's frontend previously printed "-" placeholder for HIGHEST_POSITION;
 *   wire it to actor->grip_reduction. Returns 0-based (0 = 1st place);
 *   caller adds +1 for display. */
int td5_game_get_highest_position(int slot)
{
    if (slot < 0 || slot >= TD5_MAX_RACER_SLOTS) return -1;
    TD5_Actor *actor = td5_game_get_actor(slot);
    if (!actor) return -1;
    return (int)actor->grip_reduction;
}

/* Returns wanted_kills (cop-chase arrests) for slot.
 * [CONFIRMED @ 0x0040AA0F BuildResultsTable: psVar7[0x17f] = actor+0x384]
 *
 * Orig stores arrest count in actor->special_encounter_state (+0x384),
 * incremented by td5_ai_wanted_cop_hit on first-hit branch (port
 * td5_ai.c:447 after 2026-05-24 OVERSIGHT fix).
 *
 * [FIX 2026-05-25 hud-metrics; orig 0x0040AA0F]
 *   Port's frontend previously printed "0" placeholder for ARRESTS; wire
 *   it to the actor field directly (matches orig BuildResultsTable read).
 *   ActorRaceMetric.wanted_kills has ZERO writers (dead field). */
int td5_game_get_wanted_kills(int slot)
{
    if (slot < 0 || slot >= TD5_MAX_RACER_SLOTS) return 0;
    int32_t v = (int32_t)s_results[slot].wanted_kills;
    if (v == 0) v = (int32_t)s_metrics[slot].wanted_kills;
    if (v == 0) {
        TD5_Actor *actor = td5_game_get_actor(slot);
        if (actor) v = actor->special_encounter_state & 0xFF;
    }
    return v;
}

/* Cop-chase scoring (port wiring for fields the HUD/results read).
 *
 * [FIX 2026-05-30 cop-chase] td5_ai_wanted_cop_hit previously accumulated
 * private write-only counters that nothing displayed, so the cop-chase score
 * and bust count never appeared. The original AwardWantedDamageScore @
 * 0x0043D690 awards ram points to the player/cop at gap_01f8+0xD0 (=actor
 * +0x2C8, the same field the speed bonus uses, mirrored here by
 * accumulated_score) and counts busts in the field BuildResultsTable reads
 * back (actor+0x384, special_encounter_state). Wire both. Awarded to the
 * player/cop (slot 0 = the wanted-scoring actor). */
void td5_game_add_wanted_score(int slot, int points)
{
    if (slot < 0 || slot >= TD5_MAX_RACER_SLOTS) return;
    s_metrics[slot].accumulated_score += points;
    TD5_Actor *a = td5_game_get_actor(slot);
    if (a) a->clean_driving_score += points;   /* mirror orig +0x2C8 */
}

void td5_game_add_wanted_kill(int slot)
{
    if (slot < 0 || slot >= TD5_MAX_RACER_SLOTS) return;
    s_metrics[slot].wanted_kills++;
    /* Mirror into actor+0x384 (the low byte is the bust count read by
     * BuildResultsTable @ 0x0040AA0F). Safe in cop chase: the special-
     * encounter system is disabled (gSpecialEncounterEnabled=0). */
    TD5_Actor *a = td5_game_get_actor(slot);
    if (a) a->special_encounter_state++;
}

/* [MP COP CHASE results 2026-06-25] Stamp the time (race-timer ticks) at which a
 * suspect was arrested. Called once at the bust transition. Idempotent: only the
 * FIRST arrest stamps (a re-call keeps the original time). Stores a non-zero
 * value so the results screen distinguishes "arrested" (>0, formatted MM:SS.cc)
 * from "finished un-arrested" (0, shown as '-'). */
void td5_game_set_arrest_time(int slot)
{
    if (slot < 0 || slot >= TD5_MAX_RACER_SLOTS) return;
    if (s_arrest_time[slot] != 0) return;                 /* already stamped */
    int32_t t = s_metrics[slot].cumulative_timer;         /* elapsed race time */
    s_arrest_time[slot] = (t > 0) ? t : 1;                /* keep it non-zero  */
    TD5_LOG_I(LOG_TAG, "arrest_time: suspect slot=%d stamped t=%d", slot, s_arrest_time[slot]);
}

/* Returns the arrest time (ticks) for a slot, or 0 if it was never arrested.
 * The cop-chase results screen formats >0 as MM:SS.cc and 0 as '-'. */
int32_t td5_game_get_arrest_time(int slot)
{
    if (slot < 0 || slot >= TD5_MAX_RACER_SLOTS) return 0;
    return s_arrest_time[slot];
}

/* Returns the race order slot index at position 'pos' (0=1st, ...).
 * [CONFIRMED: g_raceOrderTable = s_race_order at 0x004ae279] */
int td5_game_get_race_order(int pos)
{
    if (pos < 0 || pos >= TD5_MAX_RACER_SLOTS) return pos;
    return (int)s_race_order[pos];
}

/* Returns whether a slot is in a finished state (post_finish_metric_base != 0).
 * [CONFIRMED: actor+0x328 / companion_1 in slot state] */
int td5_game_slot_is_finished(int slot)
{
    if (slot < 0 || slot >= TD5_MAX_RACER_SLOTS) return 0;
    return (s_metrics[slot].post_finish_metric_base != 0) ? 1 : 0;
}

/* [MP per-viewport finish 2026-06-13] 1-based finishing place captured when
 * this slot crossed the line, or 0 if it is still racing. The HUD draws this as
 * a per-viewport end-of-race indicator so each split-screen player gets their
 * own finish transition independent of the others. */
int td5_game_slot_finish_place(int slot)
{
    if (slot < 0 || slot >= TD5_MAX_RACER_SLOTS) return 0;
    return s_slot_finish_place[slot];
}

/* 0-based finish position for a slot (0=1st, 1=2nd, ...).
 * [CONFIRMED @ 0x004233E0 dispatch] mirrors DAT_0048d988._2_2_ — the int16 at
 * offset +2 of s_results entry (stride 0x14 = 20 bytes), written by the sort
 * functions at 0x40AAD0/0x40AB80. Returns -1 if unsorted (still 0 from BSS). */
int td5_game_get_finish_position(int slot)
{
    if (slot < 0 || slot >= TD5_MAX_RACER_SLOTS) return -1;
    return (int)s_results[slot].final_position;
}

/* Returns the companion_2 byte for a slot. Mirrors
 * gRaceSlotStateTable.slot[i].companion_state_2 (offset +2 in the 4-byte slot
 * record at original VA 0x004a8898). Used by Screen_RaceResults case 0 to
 * detect the "eliminated mid-cup" condition (companion_2 == 2).
 * [CONFIRMED @ 0x004224B6 RunRaceResultsScreen — reads gRaceSlotStateTable.slot[0].companion_state_2] */
int td5_game_get_slot_companion_2(int slot)
{
    if (slot < 0 || slot >= TD5_MAX_RACER_SLOTS) return 0;
    return (int)s_slot_state[slot].companion_2;
}

/* Returns best lap time (ticks) for slot 0 — used by name entry qualification.
 * Scans lap_split_times[0..8] for smallest nonzero value.
 * [CONFIRMED: ScreenPostRaceNameEntry bVar4==1 scans actor+0x34e..0x35f words (9 entries)] */
int32_t td5_game_get_best_lap_time(int slot)
{
    if (slot < 0 || slot >= TD5_MAX_RACER_SLOTS) return 0x2B818; /* sentinel */
    int32_t best = 0x2B818;
    for (int i = 0; i < 9; i++) {
        int32_t t = (int32_t)s_metrics[slot].lap_split_times[i];
        if (t > 0 && t < best) best = t;
    }
    return best;
}

/* Re-sorts s_results in place (called from Screen [24] case 0).
 * Mirrors the switch in RunRaceResultsScreen case 0.
 * [CONFIRMED @ 0x00422480 case 0]: calls SortRaceResults{By,Desc} by game type */
void td5_game_sort_results(void)
{
    if (td5_game_battle_mode_active()) {   /* [TRAFFIC BATTLE 2026-06-28] by WRECKS */
        sort_results_by_score_desc();
        return;
    }
    switch (g_td5.game_type) {
    case TD5_GAMETYPE_CHAMPIONSHIP:
    case TD5_GAMETYPE_ULTIMATE:
        sort_results_by_score_desc();
        break;
    default:
        sort_results_by_time_asc();
        break;
    }
}

#ifndef TD5RE_RELEASE
/* ----------------------------------------------------------------------------
 * Dev/test harness: fabricate a plausible *finished* race result so the
 * post-race frontend screens (RACE_RESULTS [24], NAME_ENTRY [25],
 * HIGH_SCORE [23], CUP_FAILED/WON) can be VIEWED via a StartScreen jump
 * without driving a full race. These screens normally read s_results /
 * s_metrics / s_slot_state, which are all zero on a fresh boot — so the
 * table/columns render blank or as "00:00.00 / 0KPH" and the screen "looks
 * weird" (user-reported 2026-06-07). This populates those statics for the 6
 * racing slots with realistic data (slot 0 = the player, a strong ~#1 time).
 *
 * Invoked only from the StartScreen path when TD5RE_INJECT_POSTRACE is set;
 * compiled out of the release build (mirrors the TD5RE_FORCE_REPLAY harness).
 * Calibration:
 *   - time in 30 Hz ticks: 1800 = 1:00.00 (frontend_format_score_time).
 *   - speed raw -> frontend_convert_speed: 547 ~= 180 KPH, 395 ~= 130 KPH.
 * -------------------------------------------------------------------------- */
void td5_game_inject_demo_results(void)
{
    const int     demo_racers = 6;        /* player + 5 opponents */
    const int32_t base_finish = 1800;     /* slot 0: 1:00.00 */
    const int32_t place_step  = 195;      /* +6.5s per finishing place */
    const int32_t top_raw     = 547;      /* ~180 KPH / ~112 MPH */
    const int32_t avg_raw     = 395;      /* ~130 KPH /  ~81 MPH */

    for (int i = 0; i < demo_racers && i < TD5_MAX_RACER_SLOTS; i++) {
        int32_t finish = base_finish + place_step * i;

        memset(&s_metrics[i], 0, sizeof(s_metrics[i]));
        s_metrics[i].post_finish_metric_base = finish;          /* != 0 => slot_is_finished */
        s_metrics[i].cumulative_timer        = finish;
        s_metrics[i].accumulated_score       = 5000 - 400 * i;  /* points (higher = better) */
        s_metrics[i].top_speed               = top_raw - 9 * i;
        s_metrics[i].average_speed_raw       = avg_raw - 7 * i;
        s_metrics[i].display_position        = (int16_t)i;
        for (int lap = 0; lap < 3; lap++)
            s_metrics[i].lap_split_times[lap] = (int16_t)(finish / 3);   /* ~3 even laps */

        memset(&s_results[i], 0, sizeof(s_results[i]));
        s_results[i].slot_flags       = 1;
        s_results[i].slot_index       = (uint8_t)i;
        s_results[i].final_position   = (int16_t)i;
        s_results[i].primary_metric   = finish;
        s_results[i].secondary_metric = s_metrics[i].accumulated_score;
        s_results[i].top_speed        = (int16_t)(top_raw - 9 * i);

        s_slot_state[i].state       = 2;   /* completed */
        s_slot_state[i].companion_1 = 1;   /* finished */
        s_slot_state[i].companion_2 = 1;   /* completed-ok (NOT 2 = DNF; keeps Screen_RaceResults
                                            * off the cup-fail early-route) */
        s_slot_state[i].reserved    = 0;

        s_race_order[i] = (uint8_t)i;
    }

    TD5_LOG_I(LOG_TAG,
              "inject_demo_results: fabricated finished race for %d slots "
              "(slot0 finish=%d ticks ~1:00, top~180kph) for post-race frontend preview",
              demo_racers, (int)base_finish);
}
#endif /* TD5RE_RELEASE */

/* ========================================================================
 * InitializeRaceSession (0x42AA10)
 *
 * 33-step synchronous race bootstrap. The loading screen is displayed
 * at step 1, then all heavy asset loading follows. No progress bar.
 * ======================================================================== */

/* [WHEEL OVERHAUL 2026-06-12] Roll a random procedural wheel style for every
 * slot (racers + traffic) from a SEPARATE xorshift stream seeded off the race
 * seed. Using a private stream (not the CRT rand() gameplay/AI consume) keeps
 * the faithful rand() sequence — and /diff-race parity — untouched, while
 * staying identical on every netplay machine (same race_seed) and reproducible
 * on replay. */
static void td5_game_assign_wheel_styles(uint32_t race_seed) {
    uint32_t s = race_seed ^ 0x9E3779B9u;   /* decorrelate from the gameplay seed */
    if (s == 0) s = 0xA5A5A5A5u;            /* xorshift fixed-point guard */
    for (int i = 0; i < TD5_MAX_TOTAL_ACTORS; i++) {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;   /* xorshift32 */
        td5_render_set_wheel_style(i, (int)(s & 0x7FFFFFFFu));  /* setter wraps to style count */
    }
}

/* [MP COP CHASE: RANDOM COP 2026-06-25] When the cop-chase config has RANDOM COP on,
 * the manual cop/suspect role screen was skipped: players chose any car, and the
 * cop(s) are drawn HERE, at race start, so nobody knows their role until the race
 * begins (the HUD "YOU ARE THE COP / SUSPECT" banner reveals it on the countdown).
 *
 * Determinism: seed a PRIVATE xorshift stream off the replicated session_seed (the
 * same trick as td5_game_assign_wheel_styles). This (a) does NOT consume the shared
 * CRT rand() sequence, so the faithful AI/traffic RNG is untouched, and (b) yields
 * the identical pick on every netplay peer and across a replay (which restores the
 * recorded seed). In local split-screen the seed is GetTickCount(), so the cop
 * genuinely varies race to race. Picks TD5RE_COPCHASE_RANDOM_COPS distinct human
 * slots (default 1, clamped to [1, humans-1] so there is always >=1 suspect) and
 * writes cop_slot_mask + cop_slot; cop behaviour, the car the cop drives (whatever
 * they picked), the per-pane reveal, and the minimap all read these. No-op unless
 * the human-cop random path is active. */
static void td5_game_assign_random_cop(uint32_t race_seed) {
    TD5_MpModeConfig *c = &g_td5.mp_mode_config;
    if (c->mode != TD5_MP_MODE_COP_CHASE || g_td5.network_active ||
        c->cop_is_ai || !c->cop_random)
        return;

    int nh = g_td5.num_human_players;
    if (nh < 2) nh = 2;
    if (nh > TD5_MAX_HUMAN_PLAYERS) nh = TD5_MAX_HUMAN_PLAYERS;

    int want = 1;                                  /* one random cop by default */
    {
        const char *e = getenv("TD5RE_COPCHASE_RANDOM_COPS");
        if (e && e[0]) want = atoi(e);
    }
    if (want < 1)        want = 1;
    if (want > nh - 1)   want = nh - 1;            /* always leave >=1 suspect */

    uint32_t s = race_seed ^ 0xC0FFEE5Du;          /* decorrelate from gameplay + wheel streams */
    if (s == 0) s = 0xB7E15163u;                   /* xorshift fixed-point guard */

    /* Partial Fisher-Yates over the human slot list: shuffle the first `want`
     * positions and take them as the cops — distinct, ~uniform, deterministic. */
    int order[TD5_MAX_HUMAN_PLAYERS];
    for (int i = 0; i < nh; i++) order[i] = i;
    for (int i = 0; i < want; i++) {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;   /* xorshift32 */
        int j = i + (int)((s >> 1) % (uint32_t)(nh - i));
        int t = order[i]; order[i] = order[j]; order[j] = t;
    }
    int mask = 0, primary = -1;
    for (int i = 0; i < want; i++) {
        mask |= (1 << order[i]);
        if (primary < 0 || order[i] < primary) primary = order[i];  /* lowest set bit */
    }
    c->cop_slot_mask = mask;
    c->cop_slot      = (primary >= 0) ? primary : 0;
    TD5_LOG_I(LOG_TAG,
              "InitRace: RANDOM COP assigned mask=0x%X primary=%d (%d cop(s) of %d humans, seed=0x%08X)",
              mask, c->cop_slot, want, nh, race_seed);
}

/* Public getter for the active per-race RNG seed (s_saved_race_seed). This is the
 * REPLICATED seed: set from the host-broadcast session_seed at race start (and
 * restored from the recorded value for replays), so it is bit-identical on every
 * netplay peer and across a replay. Sim-deterministic features that must agree
 * across machines (e.g. the AI random branch choice in td5_ai.c) seed a private,
 * sim-tick-only RNG stream from this value rather than the shared CRT rand() —
 * whose sequence is consumed at render rate and therefore diverges between peers
 * running at different frame rates. */
uint32_t td5_game_get_race_seed(void) { return s_saved_race_seed; }

int td5_game_init_race_session(void) {
    #define CK(n) TD5_LOG_I(LOG_TAG, "CK: %s", n)
    CK("ck0_start");
    TD5_LOG_I(LOG_TAG, "InitializeRaceSession: begin");

    /* [TRAFFIC BATTLE checkpoints] Clear the deadline-chaser before any mode
     * setup; the battle block below re-arms it when the CHECKPOINTS win
     * condition is selected. (Done here, before the per-race resets, so the
     * battle block's arming isn't wiped.) */
    s_battle_chase_active = 0;
    s_battle_chase_span   = INT32_MIN;
    memset(s_battle_chase_caught, 0, sizeof(s_battle_chase_caught));

    /* [LANE ASSIST 2026-06-28] Seed each local human's lane-assist enable from
     * their menu choice (SP: Game Options [Input] LaneAssist; MP: the per-player
     * Profile-screen toggle). Keyboard 'L' can still flip it per player in-race. */
    {
        int nh_la = g_td5.num_human_players;
        int pi;
        if (nh_la < 1) nh_la = 1;
        if (nh_la > TD5_MAX_HUMAN_PLAYERS) nh_la = TD5_MAX_HUMAN_PLAYERS;
        for (pi = 0; pi < nh_la; pi++)
            td5_laneassist_set_player(pi, td5_frontend_get_player_laneassist(pi));
    }

#ifndef TD5RE_RELEASE
    /* Dev smoke-test: TD5RE_FORCE_REPLAY=1 boots straight into View-Replay mode
     * (the in-memory record buffer is empty on a fresh boot, so the played-back
     * car stays idle) — lets the REPLAY banner, cinematic trackside camera, and
     * ESC-to-exit be observed without first driving and finishing a real race.
     * Compiled out of the release build. */
    if (!s_replay_mode && getenv("TD5RE_FORCE_REPLAY") != NULL) {
        td5_game_set_replay_mode(1);
        td5_input_set_replay_mode(1);
        td5_input_set_playback_active(1);
        TD5_LOG_W(LOG_TAG, "TD5RE_FORCE_REPLAY: forcing replay mode for this race");
    }
#endif

    /* [REPLAY RESULTS PRESERVE 2026-06-27] If this race-init is a View Replay
     * (s_replay_mode was set before race_requested), the real race results are
     * still live in s_results / s_metrics / s_slot_state right now — snapshot
     * them BEFORE the resets below wipe them, so the post-race / cup results
     * show the genuine outcome after the replay ends instead of the replay
     * playback's partial result. Skipped for normal race starts (s_replay_mode
     * is 0) and pause-RESTART (also a real race), so those stay byte-identical. */
    if (s_replay_mode && replay_keep_results_on()) {
        capture_replay_results_snapshot();
    }

    /* ---- Mode overrides from InitializeRaceSession @ 0x42AA10 ----
     *
     * Network session @ 0x42ABD5 [RE basis: research agent pass]:
     *   unconditionally forces drag-race mode with 4-lap circuit,
     *   clears special encounters, rebuilds slot states from dpu+0xBCC.
     * Split-screen 2-player:
     *   clears gSpecialEncounterEnabled + gTrafficActorsEnabled. */
    if (g_td5.network_active) {
        /* [S31] The original forced 4-lap DRAG mode for net sessions
         * (0x42ABD5) -- but drag mode also forces the MANUAL gearbox in the
         * input layer (bit 28), which silently disabled the brake-at-
         * standstill auto-REVERSE latch ("can't reverse in net races"). The
         * port runs REAL races over lockstep. Laps ride the host's DXPSTART
         * config.
         *
         * [POLICE rewrite 2026-06-19] Traffic + the cop chase USED to be forced
         * off here because they were the "rand()-consuming determinism
         * envelope". The rewrite makes both lockstep-deterministic: the spawner
         * uses a private race-seeded LCG (trf_dyn_rand, never CRT rand()), the
         * spawn anchor is the replicated front-most human, and every cop chase
         * decision is a pure function of replicated actor state (no RNG). So we
         * now RUN traffic + cops in net races, adopting the HOST's replicated
         * traffic volume + POLICE setting so spawn caps + cop cadence are
         * identical on every peer. Wanted mode (the Cop Chase game type) stays
         * off in net. */
        TD5_NetRaceConfig ncfg_l;
        g_td5.drag_race_enabled = 0;
        g_td5.wanted_mode_enabled = 0;
        if (td5_net_get_race_config(&ncfg_l)) {
            if (ncfg_l.lap_count > 0)
                g_td5.circuit_lap_count = ncfg_l.lap_count;
            g_td5.traffic_volume = ncfg_l.traffic_volume;
            if (g_td5.traffic_volume < 0) g_td5.traffic_volume = 0;
            if (g_td5.traffic_volume > TD5_TRAFFIC_VOLUME_COUNT - 1)
                g_td5.traffic_volume = TD5_TRAFFIC_VOLUME_COUNT - 1;
            g_td5.traffic_enabled = (g_td5.traffic_volume > 0) ? 1 : 0;
            g_td5.special_encounter_enabled = ncfg_l.cops ? 1 : 0;
            /* [NET GAME MODES 2026-07-04] Adopt the host's DYNAMICS (ARCADE/SIM)
             * so arcade 3x-collisions + power-up boxes (and Traffic Battle boxes)
             * are identical on every peer. Committed to physics below at
             * td5_physics_set_dynamics() before td5_arcade_init_race() reads it. */
            g_td5.ini.dynamics = ncfg_l.dynamics ? 1 : 0;
            /* [MP GAME MODES 2026-06-22] Adopt the host's replicated game mode +
             * per-mode options so every peer runs the identical mode. Behaviour
             * gating off this lands per work-package (TT/cup/cop-chase); here we
             * only propagate the replicated choice. */
            g_td5.mp_mode_config = ncfg_l.mode_config;
            TD5_LOG_I(LOG_TAG,
                      "InitRace: network session — real race, traffic_vol=%d cops=%d "
                      "mp_mode=%d (replicated from host)",
                      g_td5.traffic_volume, g_td5.special_encounter_enabled,
                      g_td5.mp_mode_config.mode);
        } else {
            /* No replicated config yet — keep traffic/cops off (safe default). */
            g_td5.traffic_enabled = 0;
            g_td5.special_encounter_enabled = 0;
            TD5_LOG_W(LOG_TAG,
                      "InitRace: network session — no race config, traffic/cops off");
        }
    }
    if (g_td5.split_screen_mode > 0) {
        /* [PORT: N-way] The original disables traffic + special encounters in
         * ALL split-screen. The port KEEPS traffic in every split-screen race so
         * it's visible to every player (spawns at g_traffic_slot_base..+6). The
         * special-encounter COP rides traffic slot 9: that's a real traffic slot
         * in a <=6 field (kept), but a RACER slot in a >6 field (so disabled). */
        int field = g_td5.num_human_players + g_td5.num_ai_opponents;
        if (field > TD5_LEGACY_RACE_SLOTS) {
            g_td5.special_encounter_enabled = 0;
            TD5_LOG_I(LOG_TAG,
                      "InitRace: >6-racer split — traffic kept, special-encounter off (slot-9 layout)");
        } else {
            TD5_LOG_I(LOG_TAG,
                      "InitRace: <=6 split — traffic + special-encounter kept (PORT deviation)");
        }
    }

#ifndef TD5RE_RELEASE
    /* [MP GAME MODES test hook 2026-06-22] Force the MP game mode for solo
     * drive-testing without the local split-screen vote screen:
     *   TD5RE_MP_MODE_FORCE=1 cup, 2 traffic battle, 3 cop chase, 4 drag race
     *   (0 = race). Dev build only. Note: human-vs-human effects need >=2
     * human slots; in a solo race only slot 0 is human, so the force lets you
     * exercise the code path + logs even when the visible effect needs MP. */
    {
        const char *mf = getenv("TD5RE_MP_MODE_FORCE");
        if (mf && mf[0]) {
            int m = atoi(mf);
            if (m >= 0 && m < TD5_MP_MODE_COUNT) {
                g_td5.mp_mode_config.mode = m;
                TD5_LOG_I(LOG_TAG, "InitRace: MP mode FORCED to %d (TD5RE_MP_MODE_FORCE)", m);
            }
        }
    }
#endif

    /* [FIX 2026-06-24] Clear stale wanted mode when a LOCAL split-screen MP
     * session runs any non-cop-chase mode (Race / Time Trial / Cup). wanted_mode_
     * enabled is owned by the chosen MP game mode for local split-screen, but the
     * ONLY local writer below just turns it ON for cop chase — nothing turned it
     * back OFF. So Cop Chase -> lobby -> Cup kept the flag at 1, and the Cup race
     * re-ran the cop/suspect layout: player 1 stuck as the cop and the field
     * collapsed to 2 cars (td5_game_mp_cop_chase_field() returns 0 for a non-cop-
     * chase mode -> keep=2 -> slots 2..5 disabled at the wanted-mode block below),
     * leaving the other viewports with no car. Mirror the network reset at line
     * ~1611. Single-player Cop Chase (game_type 8) sets the flag via
     * ConfigureGameTypeFlags and runs with split_screen_mode==0, so it is
     * deliberately excluded here and unaffected. */
    if (g_td5.split_screen_mode > 0 && !g_td5.network_active &&
        g_td5.mp_mode_config.mode != TD5_MP_MODE_COP_CHASE) {
        if (g_td5.wanted_mode_enabled) {
            TD5_LOG_I(LOG_TAG,
                      "InitRace: local MP mode=%d (not cop chase) — clearing stale "
                      "wanted_mode_enabled (was 1)", g_td5.mp_mode_config.mode);
        }
        g_td5.wanted_mode_enabled = 0;
    }

    /* [MP GAME MODES: COP CHASE 2026-06-22] The MP cop-chase mode reuses the
     * wanted-mode machinery (a player-cop rams the other racers — the suspects —
     * to deplete their damage bar). Turn wanted mode on; the effective cop slot
     * comes from mp_mode_config.cop_slot (see td5_game_cop_chase_cop_slot). Local
     * split-screen only for now — the net path above leaves wanted mode off. */
    if (g_td5.mp_mode_config.mode == TD5_MP_MODE_COP_CHASE && !g_td5.network_active) {
        g_td5.wanted_mode_enabled = 1;
        td5_game_reset_infect_state();   /* [INFECT] clear converted/pending sets per race */
        /* NOTE: the AI cop is armed LATER (after td5_ai_init_traffic_actors, whose
         * cop_state_reset() in the dynamic-traffic seeding would otherwise WIPE the
         * arming — that bug made the AI cop revert to a regular racer and the cop
         * role fall back onto player 1). See the td5_ai_cop_chase_setup call after
         * step 14 below. */
        TD5_LOG_I(LOG_TAG, "InitRace: MP COP CHASE active — cop slot=%d ai=%d win=%d head_start=%d",
                  td5_game_cop_chase_cop_slot(), g_td5.mp_mode_config.cop_is_ai,
                  g_td5.mp_mode_config.cop_win_condition,
                  g_td5.mp_mode_config.suspect_head_start);
    }

    /* [TRAFFIC BATTLE 2026-06-28] Single-player entry: a release-safe knob lets a
     * solo race (humans + AI, no split-screen / net) launch the Traffic
     * Destruction mode without the MP lobby — the SP analogue of how the MP vote
     * screen selects it. (The dev-only TD5RE_MP_MODE_FORCE=4 above also reaches
     * this; this knob works in release too.) The AI opponents stay enabled (set
     * by the frontend's num_ai_opponents) so it is genuinely player-vs-AI. */
    if (!g_td5.network_active && g_td5.split_screen_mode == 0) {
        const char *be = getenv("TD5RE_BATTLE");
        if (be && (be[0] == '1' || be[0] == 'y' || be[0] == 'Y' ||
                   be[0] == 't' || be[0] == 'T')) {
            g_td5.mp_mode_config.mode = TD5_MP_MODE_TRAFFIC_BATTLE;
            /* No MP config screen on the SP knob path, so seed the power-up
             * density here (the MP vote path seeds it in
             * mp_mode_config_apply_defaults). Default DENSE; TD5RE_ARCADE_DENSITY
             * overrides at the spacing site anyway. */
            int d = td5_env_int("TD5RE_ARCADE_DENSITY", 2, 0, 3);
            g_td5.mp_mode_config.battle_powerup_density = d;
            /* SP win-condition override (no MP config screen on this path):
             * TD5RE_BATTLE_WIN=1 -> CHECKPOINTS deadline chaser, 0 -> MOST WRECKS. */
            const char *we = getenv("TD5RE_BATTLE_WIN");
            g_td5.mp_mode_config.battle_win_condition =
                (we && (we[0] == '1' || we[0] == 'c' || we[0] == 'C'))
                ? TD5_BATTLE_WIN_CHECKPOINTS : TD5_BATTLE_WIN_MOST_WRECKS;
        }
    }

    /* [TRAFFIC BATTLE 2026-06-28] Battle setup (MP-selected OR synthesised SP).
     * Force a fixed-pace dynamic-traffic stream ON, cops/wanted OFF, collisions
     * ON, and a fixed traffic volume so the field is a steady supply of ram
     * targets. The match runs to the track END (laps / finish line) — no timer
     * (locked design decision). Works for both ARCADE and SIMULATION dynamics;
     * the power-up boxes only appear in arcade dynamics. */
    if (td5_game_battle_mode_active()) {
        if (g_td5.mp_mode_config.battle_spawn_period <= 0) {
            int p = td5_env_int("TD5RE_BATTLE_SPAWN_PERIOD", 30, 5, 240);   /* ticks @30Hz between spawns */
            g_td5.mp_mode_config.battle_spawn_period = p;
        }
        g_td5.wanted_mode_enabled       = 0;     /* not cop chase            */
        g_td5.special_encounter_enabled = 0;     /* cops OFF                 */
        g_td5.drag_race_enabled         = 0;
        g_td5.time_trial_enabled        = 0;
        g_td5.traffic_enabled           = 1;     /* traffic ON              */
        /* [TRAFFIC BATTLE 2026-06-28] NO rival racers — it is you (and any other
         * humans in split-screen) vs the traffic stream only (user: "there should
         * be no opponents in this mode"). Drop the AI opponents and, for a solo
         * race, synthesise the same single-racer field as Time Trial so slots
         * 1..5 are disabled. Network keeps the host's replicated field. */
        if (!g_td5.network_active) {
            g_td5.num_ai_opponents = 0;
            if (g_td5.split_screen_mode == 0)
                g_td5.solo_mode_synth = 1;
        }
        g_td5.ini.traffic_dynamic       = 1;     /* dynamic spawner ON      */
        g_td5.ini.traffic_dyn_period    = g_td5.mp_mode_config.battle_spawn_period;
        {
            int v = td5_env_int("TD5RE_BATTLE_TRAFFIC_VOLUME", 4, 1, 4);   /* 4 = VERY HIGH (full stream) */
            g_td5.traffic_volume = v;
        }
        td5_physics_set_collisions(1);           /* ramming needs collisions ON */
        /* [TRAFFIC BATTLE 2026-06-28] Spawn the racers part-way down the track
         * (default span 150) so the action starts right in the traffic instead of
         * on an empty start line. Respects an explicit dev StartSpanOffset (only
         * applied when it is 0). Consumed per-slot later in this function. */
        if (g_td5.ini.start_span_offset == 0) {
            int s0 = td5_env_int("TD5RE_BATTLE_START_SPAN", 150, 0, 30000);
            g_td5.ini.start_span_offset = s0;
        }
        /* [TRAFFIC BATTLE checkpoints] CHECKPOINTS win condition arms the
         * deadline chaser (anchored on the first race tick once spawn spans are
         * known). MOST WRECKS leaves it off and the race runs to the track end. */
        if (g_td5.mp_mode_config.battle_win_condition == TD5_BATTLE_WIN_CHECKPOINTS) {
            s_battle_chase_active = 1;
            s_battle_chase_span   = INT32_MIN;   /* anchored on first battle_chase_tick */
        }
        TD5_LOG_I(LOG_TAG, "InitRace: TRAFFIC BATTLE active — dynamic traffic ON "
                  "(period=%d vol=%d), cops OFF, oncoming traffic, spawn span=%d, win=%s; "
                  "winner = most WRECKS",
                  g_td5.ini.traffic_dyn_period, g_td5.traffic_volume,
                  g_td5.ini.start_span_offset,
                  s_battle_chase_active ? "CHECKPOINTS" : "MOST-WRECKS");
    }

    /* [DRAG RACE 2026-06-30] MP drag-race mode setup. Mirrors the battle block but
     * keeps standard race placement (race to the finish): drag strip on, NO AI
     * opponents, NO cops, and OPTIONAL oncoming traffic (drag_traffic). Unlike a
     * solo drag, split-screen keeps every human slot (no solo_mode_synth). The
     * distance preset (drag_length) + lane count (num_human_players +
     * drag_extra_lanes) are read straight from mp_mode_config by drag_length_level()
     * and td5_game_drag_field_size(); nothing extra to wire here. */
    if (td5_game_drag_mp_active()) {
        g_td5.drag_race_enabled         = 1;
        g_td5.wanted_mode_enabled       = 0;     /* not cop chase            */
        g_td5.special_encounter_enabled = 0;     /* cops OFF                 */
        g_td5.time_trial_enabled        = 0;
        /* [NET GAME MODES 2026-07-04] NO rival AI (user rule). Over the net the
         * OTHER players live in num_ai_opponents (np-1; num_human_players is
         * forced to 1 at td5_fe_mp_setup.c:447) and the host broadcasts
         * num_opponents=0 for drag, so only zero it for local/solo drag —
         * zeroing it over the net would delete every other player's car. */
        if (!g_td5.network_active)
            g_td5.num_ai_opponents      = 0;     /* NO rival AI (user rule)  */
        if (g_td5.mp_mode_config.drag_traffic) {
            /* Oncoming-only traffic stream (no lane changes; spawns ~50 spans
             * ahead — see trf_force_oncoming + the drag spawn-ahead in td5_ai.c). */
            g_td5.traffic_enabled        = 1;
            g_td5.ini.traffic_dynamic    = 1;
            /* [DRAG TRAFFIC 2026-06-30] Denser stream (~30% faster cadence: a fresh
             * car every ~0.67s @30Hz vs the old 1s) so the 16-car field refills
             * quickly as oncoming cars blow past — "increase the volume of traffic". */
            g_td5.ini.traffic_dyn_period = 20;
            g_td5.traffic_volume         = 4;     /* full oncoming stream (engine max) */
            /* [DRAG TRAFFIC 2026-06-30] Spawn CLOSER — the window is scaled by
             * TD5RE_TRAFFIC_SPAWN_DIST (default 2x), so 8..18 -> ~16..36 spans ahead
             * of the lead (was 25..50 -> 50..100, which popped in much too far out). */
            g_td5.ini.traffic_dyn_spawn_min = 8;
            g_td5.ini.traffic_dyn_spawn_max = 18;
            td5_physics_set_collisions(1);
        } else {
            g_td5.traffic_enabled        = 0;
            g_td5.ini.traffic_dynamic    = 0;
        }
        TD5_LOG_I(LOG_TAG, "InitRace: MP DRAG RACE active — no AI, traffic=%s, distance=%d, "
                  "extra_lanes=%d, field=%d lanes",
                  g_td5.mp_mode_config.drag_traffic ? "ONCOMING" : "off",
                  g_td5.mp_mode_config.drag_length, g_td5.mp_mode_config.drag_extra_lanes,
                  td5_game_drag_field_size());
    }

    /* Resolve g_special_encounter (port mirror of g_specialEncounterType
     * @ 0x004B0FA8). This is the runtime gate read by both the HUD timer
     * widget (RenderRaceHudOverlays @ 0x004391CC) and the per-actor timer
     * decrement (AdvancePendingFinishState @ 0x0040A2DC). Distinct from
     * g_td5.special_encounter_enabled, which is the encoded
     * gSpecialEncounterEnabled/Cops mirror used by AI/physics for spawn
     * gating.
     *
     * Original writers per InitializeRaceSession @ 0x0042AA10:
     *   0x0042ABE0  network active            -> 0
     *   0x0042AD06  wanted mode               -> 0
     *   0x0042AD7F  no schedule               -> 0
     *   0x0042ADAC  selectedGameType != 0     -> 0
     *   0x0042ADD3  attract demo              -> 0
     *   0x0042AE75  circuit track (LEVELINF)  -> 0
     *   else: keep DAT_00466004 (checkpoint-timers user toggle, default 1).
     *
     * Port equivalent: start from g_td5.checkpoint_timers_enabled and apply
     * the same zero conditions. */
    g_special_encounter = g_td5.checkpoint_timers_enabled ? 1 : 0;
    if (g_td5.network_active)        g_special_encounter = 0;
    if (g_td5.split_screen_mode > 0) g_special_encounter = 0;
    if (g_td5.wanted_mode_enabled)   g_special_encounter = 0;
    if (g_td5.game_type != 0)        g_special_encounter = 0;
    if (g_track_is_circuit)          g_special_encounter = 0;
    TD5_LOG_I(LOG_TAG,
              "InitRace: g_special_encounter=%d (cp_timers=%d gt=%d circuit=%d "
              "net=%d split=%d wanted=%d cops/se_enabled=%d)",
              g_special_encounter,
              g_td5.checkpoint_timers_enabled, (int)g_td5.game_type,
              g_track_is_circuit, g_td5.network_active,
              g_td5.split_screen_mode, g_td5.wanted_mode_enabled,
              g_td5.special_encounter_enabled);

    /* ---- Step 0: Reseed CRT + fill race random seed table ---- */
    /* Original InitializeRaceSession @ 0x0042aa51-0x0042aa80:
     *   PUSH g_raceSessionRandomSeed; CALL srand          -- 0x42aa51
     *   ESI = 0x4aadbc; loop: rand() → [ESI]; ESI+=4      -- 0x42aa5f-0x42aa6f
     *   while ESI < 0x4aadec (12 entries = 0x30 bytes)
     *   rand()                                             -- 0x42aa73 (extra)
     *   EDX = EDX % 20; push EDX → sprintf "load%02d.tga" -- 0x42aa78-0x42aa80
     *
     * g_raceRandomSeedTable: 0x4aadbc..0x4aadec = 12 int32 entries.
     * The 13th rand() (the extra one after the loop) provides the loading
     * screen index via IDIV 20 — that rand() IS the display_loading_screen_tga
     * call's rand() in the port (which calls rand()%20 internally).
     *
     * Port's g_raceSessionRandomSeed equivalent: use timeGetTime() outside
     * trace mode (matches original's non-deterministic path); 0x1A2B3C4D
     * under race_trace_enabled (matches the Frida hook's seed_crt path).
     * [CONFIRMED @ 0x0042aa10 disassembly; InitializeRaceSession body]
     *
     * The 12 seed-table rand() calls advance CRT _holdrand state before the
     * loading screen rand() and any subsequent race-runtime rand() consumers
     * (BuildRaceResultsTable @ 0x40A8C0 uses rand() & 0x1F, etc.). */
    {
        /* Determinism: a recorded race captures its seed; replaying that race
         * RESTORES the captured seed so AI/traffic RNG reproduce the recorded
         * run. Mirrors InitializeRaceSession @0x42AA22-0x42AA4B (non-replay =
         * save g_savedRngSeed; replay = restore it). Without this the port
         * reseeded from GetTickCount() each race → replay = "not my race".
         * Trace mode keeps its fixed seed (crt_seed=0x1A2B3C4D, matching the
         * td5_quickrace.py Frida default) and is never a replay. */
        uint32_t session_seed;
        if (s_replay_mode) {
            session_seed = s_saved_race_seed;
            TD5_LOG_I(LOG_TAG, "InitRace step 0/19: REPLAY restoring saved seed=0x%08X",
                      session_seed);
        } else {
            TD5_NetRaceConfig ncfg;
            if (g_td5.network_active && td5_net_get_race_config(&ncfg)) {
                /* [S31] lockstep: every machine seeds from the host-broadcast
                 * value (takes precedence over the trace fixed seed). */
                session_seed = ncfg.rng_seed;
                TD5_LOG_I(LOG_TAG, "InitRace step 0/19: NET seed=0x%08X", session_seed);
            } else if (g_td5.ini.race_trace_enabled) {
                session_seed = (uint32_t)0x1A2B3C4D;
            } else {
                session_seed = (uint32_t)GetTickCount();
            }
            s_saved_race_seed = session_seed;   /* capture for a later View Replay */
        }
        srand(session_seed);
        /* Drain 12 entries to advance CRT state (port doesn't have the
         * DAT_004aadbc global table but must step _holdrand the same way) */
        for (int i = 0; i < 12; i++) {
            (void)rand();
        }
        TD5_LOG_I(LOG_TAG,
                  "InitRace step 0/19: CRT reseeded (seed=0x%08X) + 12 seed-table rands drained",
                  session_seed);

        /* [WHEEL OVERHAUL] Per-race random wheel style per slot, from a private
         * stream off the same seed (does NOT touch the CRT rand() drained
         * above, so faithful AI/traffic RNG is unchanged). */
        td5_game_assign_wheel_styles(session_seed);

        /* [RANDOM COP 2026-06-25] Draw the cop-chase cop(s) from the SAME replicated
         * seed (private stream — does NOT touch the CRT rand() drained above), so the
         * pick is identical on every netplay peer / across a replay yet varies race to
         * race in local split-screen. No-op unless RANDOM COP is on. */
        td5_game_assign_random_cop(session_seed);
    }

    /* ---- Step 1: Display random loading screen TGA (rand()%20) ---- */
    /* The 13th rand() (after the 12 seed-table fills above) is the one
     * the original uses for the loading screen index via IDIV 20. This call
     * internally does rand()%20, which is that 13th rand() in the sequence.
     * [CONFIRMED @ 0x0042aa73-0x0042aa80: extra rand(); IDIV ECX(=0x14)] */
    display_loading_screen_tga();
    TD5_LOG_I(LOG_TAG, "InitRace step 1/19: loading screen displayed for track=%d",
              g_td5.track_index);
    CK("ck1_after_loading_screen");

    /* ---- Step 2: Reset game heap (0x430CB0, 24 MB pool) ---- */
    td5_plat_heap_reset();
    TD5_LOG_I(LOG_TAG, "InitRace step 2/19: heap reset complete");
    CK("ck2_after_heap_reset");

    /* [#10 race telemetry] Zero all per-racer summary metrics at the start of
     * every race so the post-race summary reflects only this race. Accumulated
     * each live sim tick by td5_physics_accumulate_metrics(). */
    td5_physics_reset_metrics();

    /* ---- Step 3: Configure race slot states (player/AI/disabled) ---- */
    for (int i = 0; i < TD5_MAX_RACER_SLOTS; i++) {
        s_slot_state[i].state       = (i == 0) ? 1 : 0;  /* slot 0 = player */
        s_slot_state[i].companion_1 = 0;
        s_slot_state[i].companion_2 = 0;
        s_slot_state[i].reserved    = 0;
        s_slot_finish_place[i]      = 0;  /* per-viewport finish indicator reset */
    }
    /* If split-screen, slots 1..num_human_players-1 are also human players
     * [PORT ENHANCEMENT: N-way split, was just slot 1]. */
    if (g_td5.split_screen_mode > 0) {
        int humans = g_td5.num_human_players;
        if (humans > TD5_MAX_VIEWPORTS) humans = TD5_MAX_VIEWPORTS;
        for (int i = 1; i < humans && i < TD5_MAX_RACER_SLOTS; i++)
            s_slot_state[i].state = 1;
    }
    /* Time trial single-player: disable slot 1 (no ghost car yet) */
    if (g_td5.time_trial_enabled && g_td5.split_screen_mode == 0) {
        s_slot_state[1].state = 3;  /* disabled */
        TD5_LOG_I(LOG_TAG, "Time trial single-player: slot 1 disabled");
    }
    /* Solo synth (user-facing TT, see ConfigureGameTypeFlags case 7): force
     * slots 1..5 to state=3 (INACTIVE) so the player runs alone without
     * AI rear-ends. Mirrors the Frida hook's onLeave InitializeRaceSession
     * suppressor on the original side. The runtime AI/physics paths see
     * single race (game_type=0) so both sides take the same code path.
     *
     * SoloAISlot debug knob: slot 0 stays active (camera tracks slot 0),
     * but its initial route-selector parity is overridden to simulate
     * slot N's behavior (see td5_ai.c InitializeRaceActorRuntime). */
    if (g_td5.solo_mode_synth && g_td5.split_screen_mode == 0) {
        for (int i = 1; i < TD5_MAX_RACER_SLOTS; i++) {
            s_slot_state[i].state = 3;  /* inactive */
        }
        TD5_LOG_I(LOG_TAG,
                  "Solo synth: slots 1..5 INACTIVE, slot 0 simulates SoloAISlot=%d",
                  g_td5.ini.solo_ai_slot);
    }
    /* SoloRace debug (2026-05-30): force slots 1..5 inactive so the player
     * races alone and always finishes 1st — lets the victory star/position
     * overlay be tested without out-driving the AI. Unlike solo_mode_synth this
     * applies NO AI emulation to slot 0 (the human still drives normally). */
    if (g_td5.ini.solo_race && g_td5.split_screen_mode == 0) {
        for (int i = 1; i < TD5_MAX_RACER_SLOTS; i++) {
            s_slot_state[i].state = 3;  /* inactive */
        }
        TD5_LOG_I(LOG_TAG, "SoloRace: slots 1..5 INACTIVE (player races alone)");
    }

    /* Mark unused racer slots as disabled based on the current mode */
    {
        int racer_slot_count = g_td5.time_trial_enabled ? 2 : TD5_MAX_RACER_SLOTS;
        /* Single race / Quick Race (no special mode): honor the configured
         * humans+opponents count [PORT ENHANCEMENT — Quick Race player setup].
         * Slots 0..effective-humans-1 are already human (slot 0 always, slot 1
         * when split>0); the remaining active slots up to total are AI; slots
         * beyond total are disabled here. Guarded on num_human_players>=1 so an
         * un-configured launch (counts still 0) keeps the full 6-car grid.
         * Default (1 human + 5 AI = 6) disables nothing — legacy behavior. */
        if (!g_td5.time_trial_enabled && !g_td5.drag_race_enabled &&
            !g_td5.wanted_mode_enabled && g_td5.num_human_players >= 1) {
            int total = g_td5.num_human_players + g_td5.num_ai_opponents;
            if (total < 1) total = 1;
            if (total > TD5_MAX_RACER_SLOTS) total = TD5_MAX_RACER_SLOTS;
            racer_slot_count = total;
            TD5_LOG_I(LOG_TAG,
                      "InitRace: single-race racer count=%d (humans=%d opponents=%d)",
                      total, g_td5.num_human_players, g_td5.num_ai_opponents);
        }
        for (int i = racer_slot_count; i < TD5_MAX_RACER_SLOTS; i++) {
            s_slot_state[i].state = 3;  /* disabled */
        }
    }
    /* Drag race: slots uVar18..5 forced to state=3 (decoration).
     * [CONFIRMED @ InitializeRaceSession 0x0042ac8e + uVar18 init 0x0042ac66]
     * Original: uVar18 = (g_twoPlayerModeEnabled != 0) + 1 → SP drag spawns
     * only slot 0 active, slots 1..5 all decoration; 2P drag keeps slot 1
     * active (P2) and slots 2..5 decoration.
     *
     * [PORT ENHANCEMENT — diverges from original]
     * The port's CarSelection screen runs a 2-pass loop for game_type==9
     * (drag race) so the user picks TWO cars. InitializeRaceSeriesSchedule
     * @ td5_frontend.c:1628 then loads s_p2_car into slot[1].ext_id and
     * line ~966 loads slot 1's mesh + sound bank. Keeping slot 1 at
     * decoration_start=1 (faithful) leaves that fully-prepared slot inert
     * at span=1 (parked at the strip start), which is the bug the user
     * reports as "no car beside me, two stationary cars at the back."
     * Force decoration_start=2 in drag mode regardless of split-screen so
     * slot 1 stays state=0 (AI in SP) or state=1 (P2 in 2P split) — making
     * the 2-pass CarSelect actually produce a 2-car race. */
    if (g_td5.drag_race_enabled) {
        /* [DRAG DYNAMIC FIELD 2026-06-27] One active car per lane: the field
         * scales with humans+AI (td5_game_drag_field_size), and the track is
         * widened to the same count in td5_track_drag_apply_geometry. Slots
         * 0..field-1 keep their base-init state (slot 0 human, the rest AI, and
         * split-screen humans 1..N), slots field..MAX become decoration. The
         * per-slot asset loop below preps a car for every active slot. */
        int decoration_start = td5_game_drag_active_racers();
        if (decoration_start < 1) decoration_start = 1;
        if (decoration_start > TD5_MAX_RACER_SLOTS) decoration_start = TD5_MAX_RACER_SLOTS;
        for (int i = decoration_start; i < TD5_MAX_RACER_SLOTS; i++) {
            s_slot_state[i].state = 3;
        }
        /* [DRAG Phase 2] Fresh lane-change state so each car aims at its spawn
         * lane (lazy-init picks up the derived sub_lane on the first drag tick). */
        td5_input_drag_reset();
        TD5_LOG_I(LOG_TAG,
                  "Drag race: %d active racers (one per lane), slots %d..%d decoration (split=%d)",
                  decoration_start, decoration_start, TD5_MAX_RACER_SLOTS - 1,
                  g_td5.split_screen_mode);
    }
    /* Cop Chase (game_type 8): original sets g_racerCount=2 for all non-zero
     * game types [CONFIRMED @ InitializeRaceActorRuntime 0x00432E60:
     *   if (g_selectedGameType != 0) g_racerCount = 2].
     * Slots 2..5 are disabled [CONFIRMED @ InitializeRaceSession 0x0042AB91:
     *   if (TVar16 != 0) slots 2..5 set to state=3].
     * Only slot 0 (player) and slot 1 (cop AI) are active.
     * Slot 1 uses standard UpdateActorTrackBehavior (route following) to
     * pursue the player along the track — the "cop pursuit" is implicit in
     * the rubber-band / route system keeping slot 1 behind slot 0.
     *
     * The wanted-mode AI gate in UpdateRaceActors reads gWantedDamageStateTable
     * per slot [CONFIRMED @ 0x00436E1D]: blocks track-behavior only when
     * mode=ON AND damage_table[slot]==0. Original initialises the table to
     * 0x1000 per slot in InitializeWantedHudOverlays [CONFIRMED @ 0x0043D2FC:
     *   _gWantedDamageStateTable = 0x10001000].
     * Port uses ACTOR_ENCOUNTER_STATE (offset 0x384) in place of the separate
     * damage table — initialize it to 0x1000 so cops run from tick 1. */
    if (g_td5.wanted_mode_enabled) {
        /* MP cop chase keeps the human suspects + the AI-cop slot active; SP
         * wanted mode keeps the faithful slots 0-1. Slots 0..humans-1 are already
         * human (1) and the AI-cop slot (= num_human_players) is AI (0); only the
         * slots past the field get disabled. */
        int keep = td5_game_mp_cop_chase_field();
        if (keep <= 0) keep = 2;   /* SP wanted: slot 0 player + slot 1 AI suspect */
        for (int i = keep; i < TD5_MAX_RACER_SLOTS; i++) {
            s_slot_state[i].state = 3;  /* disabled */
        }
        TD5_LOG_I(LOG_TAG, "Cop Chase: active field=%d, slots %d..5 disabled", keep, keep);
    }
    /* Player-as-AI autopilot: mirrors original attract-mode at
     * InitializeRaceSession 0x0042ACCF, which writes
     *   slot[0].state = 1 - (g_attractModeDemoActive | g_benchmarkModeActive)
     * Dropping slot 0 to state=0 (AI) routes it through
     * td5_physics_update_ai at td5_physics.c:463 and makes td5_game skip
     * td5_input_update_player_control(0) at td5_game.c:1538/1559. */
    /* [PORT: N-way] player_is_ai puts EVERY local human slot on AI autopilot
     * (not just slot 0), so all split-screen cars drive themselves — handy for
     * testing the panes without N controllers. */
    if (g_td5.ini.player_is_ai) {
        int humans = g_td5.num_human_players;
        if (humans < 1) humans = 1;
        if (humans > TD5_MAX_RACER_SLOTS) humans = TD5_MAX_RACER_SLOTS;
        for (int i = 0; i < humans; i++) {
            if (s_slot_state[i].state == 1) s_slot_state[i].state = 0;
        }
        TD5_LOG_I(LOG_TAG,
                  "InitRace: player_is_ai=1 -> %d human slot(s) switched to AI autopilot",
                  humans);
    } else if (g_td5.ini.others_ai) {
        /* [PORT: N-way] AI-drive every local human slot EXCEPT slot 0, so the
         * user drives player 1 while the other split-screen panes self-drive.
         * Slot 0 keeps state==1 (human) and the slot-0 AI-dispatch paths key
         * off player_is_ai (not others_ai), so its input path stays live. */
        int humans = g_td5.num_human_players;
        if (humans > TD5_MAX_RACER_SLOTS) humans = TD5_MAX_RACER_SLOTS;
        for (int i = 1; i < humans; i++) {
            if (s_slot_state[i].state == 1) s_slot_state[i].state = 0;
        }
        TD5_LOG_I(LOG_TAG,
                  "InitRace: others_ai=1 -> human slots 1..%d on AI autopilot "
                  "(slot 0 = human)",
                  humans - 1);
    }
    /* [MP AI TEST PLAYERS 2026-06-25] The dev "add AI player" lobby tool fills
     * human-pool slots with simulated players. Each such slot occupies a real
     * split-screen viewport but is AI-driven, so flip exactly the masked slots to
     * state=0 (AI autopilot) — same mechanism as others_ai, but per-slot from the
     * frontend mask instead of a global INI knob. Slot 0 (the human at the wheel)
     * is never in the mask. Runs after player_is_ai/others_ai so it composes with
     * them; before the network override (which re-marks net slots human). */
    if (g_td5.mp_ai_player_mask) {
        int humans = g_td5.num_human_players, did = 0;
        if (humans > TD5_MAX_RACER_SLOTS) humans = TD5_MAX_RACER_SLOTS;
        for (int i = 0; i < humans; i++) {
            if (((g_td5.mp_ai_player_mask >> i) & 1u) && s_slot_state[i].state == 1) {
                s_slot_state[i].state = 0;  /* AI test-player */
                did++;
            }
        }
        TD5_LOG_I(LOG_TAG,
                  "InitRace: mp_ai_player_mask=0x%X -> %d simulated AI player slot(s)",
                  g_td5.mp_ai_player_mask, did);
    }
    /* [DEMO FIX #3 2026-06-15] Attract demo: the PLAYER car (slot 0) must be
     * AI-driven, exactly like the opponents — it's an attract demo, nobody is
     * holding the wheel. Mirrors the original InitializeRaceSession @0x0042ACCF
     *   slot[0].state = 1 - (g_attractModeDemoActive | g_benchmarkModeActive)
     * which drops slot 0 to state=0 (AI) whenever the attract demo is active.
     * Dropping slot 0 to state=0 routes it through td5_physics_update_ai +
     * td5_ai_tick (the opponent path) AND makes the in-race input dispatch skip
     * td5_input_update_player_control(0) — so the demo car drives itself.
     *
     * This is the SAME mechanism g_td5.ini.player_is_ai uses above; demo just
     * applies it to slot 0 unconditionally while the attract demo runs (no INI
     * knob needed — the menu sets s_demo_mode via td5_game_set_demo_mode). Demo
     * never plays back recorded input (that is View Replay / s_replay_mode), so
     * unlike the player_is_ai parity hack we do NOT also feed slot 0's input —
     * the AI fully owns the car.
     *
     * Gated by TD5RE_DEMO_FIX (cached, default ON): "0" reverts to the old
     * behaviour (slot 0 stays state=1 in demo → car sits as if a human with no
     * input were driving). Non-demo races never enter this block (s_demo_mode==0
     * unless the attract demo launched it), so normal play is byte-unchanged. */
    if (s_demo_mode) {
        static int s_demo_fix_init = 0;
        static int s_demo_fix_enabled = 1;
        if (!s_demo_fix_init) {
            s_demo_fix_enabled = td5_env_flag_on("TD5RE_DEMO_FIX");  /* default ON */
            s_demo_fix_init = 1;
            TD5_LOG_I(LOG_TAG, "Demo AI-drive (slot 0): %s",
                      s_demo_fix_enabled ? "enabled" : "disabled (slot 0 stays human)");
        }
        if (s_demo_fix_enabled && s_slot_state[0].state == 1) {
            s_slot_state[0].state = 0;   /* AI-drive the demo player car */
            TD5_LOG_I(LOG_TAG,
                      "InitRace: demo mode -> slot 0 switched to AI autopilot");
        }
    }
    /* [ITEM 2 2026-06-19] Network race: every roster-occupied slot is a HUMAN
     * player driven by the lockstep-exchanged input bits, not AI. The roster
     * (td5_net_is_slot_active) is replicated identically on every peer, so this
     * marks the SAME slots on all machines -> deterministic. (Marking only the
     * LOCAL slot would DESYNC: the host would mark slot 0 while a client marks
     * slot 1, handing the AI module a different slot set per machine.) Without
     * this, a joining client's own car (net slot >= 1) stayed state=0 and the
     * AI overwrote its controls every tick -- the "player is AI" the user saw.
     * Runs last so it overrides the grid-fill / disable passes above. */
    if (g_td5.network_active) {
        for (int i = 0; i < TD5_MAX_RACER_SLOTS && i < TD5_NET_MAX_PLAYERS; i++)
            if (td5_net_is_slot_active(i))
                s_slot_state[i].state = 1;
    }
    /* [FIX 2026-06-30 throttle/camera desync] Restore the original's SINGLE
     * human-control invariant for LOCAL split-screen. The original
     * (UpdateVehicleActor @0x00406650) gates both the throttle path
     * (player input -> actor+0x33E) and the human-vs-AI dynamics dispatch on ONE
     * per-actor control-source flag, re-established every race. The port split
     * that into TWO gates that can DESYNC across a cup/series: the per-slot input
     * update keys on s_slot_state[i].state==1 (td5_game.c:5717 countdown / 5985
     * racing), while the camera-change/poll path keys on the local human count
     * s_active_players (td5_input.c:747) — set from this same num_human_players at
     * step 15. The reported bug — a split-screen player on a SUBSEQUENT cup race
     * who can change the camera but cannot accelerate — is exactly this desync:
     * the slot is still a live local human (camera works) yet its state was left
     * != 1 by some stale input across the series, so td5_input_update_player_control
     * is skipped and throttle never reaches actor+0x33E.
     *
     * The network pass above already guarantees robustness by force-marking every
     * active roster slot human LAST; local split-screen had no equivalent. Mirror
     * it: re-assert state=1 for every live LOCAL HUMAN POSITION (i <
     * num_human_players — the SAME membership that drives the camera the player
     * just used), UNLESS the user explicitly put that slot on AI autopilot
     * (player_is_ai / others_ai co-op / the AI-test-player mask / attract demo).
     * Running LAST overrides any earlier mis-derivation; at race start no slot has
     * finished yet, so clearing a stale state==2 (finished in the previous series
     * race) is correct. By construction camera-works => slot in num_human_players
     * => state forced to 1 => throttle works, so the two gates can no longer
     * disagree for a genuine human. A WARN fires only when a slot is actually
     * repaired, capturing the exact stale trigger in race.log. */
    if (g_td5.split_screen_mode > 0 && !g_td5.network_active) {
        int rep_humans = g_td5.num_human_players;
        if (rep_humans > TD5_MAX_VIEWPORTS)   rep_humans = TD5_MAX_VIEWPORTS;
        if (rep_humans > TD5_MAX_RACER_SLOTS) rep_humans = TD5_MAX_RACER_SLOTS;
        for (int i = 0; i < rep_humans; i++) {
            if (g_td5.ini.player_is_ai)               continue; /* every human on AI autopilot */
            if (g_td5.ini.others_ai && i > 0)         continue; /* co-op AI (slot 0 stays human) */
            if ((g_td5.mp_ai_player_mask >> i) & 1u)  continue; /* simulated AI test player */
            if (s_demo_mode && i == 0)                continue; /* attract demo drives slot 0 */
            if (s_slot_state[i].state == 3)           continue; /* legitimately disabled (TT/drag decoration) */
            if (s_slot_state[i].state != 1) {
                TD5_LOG_W(LOG_TAG,
                          "InitRace: REPAIR live human slot %d state %d->1 "
                          "(split=%d humans=%d mask=0x%X others_ai=%d demo=%d) "
                          "-- throttle/camera gate desync guard",
                          i, s_slot_state[i].state, g_td5.split_screen_mode,
                          g_td5.num_human_players, g_td5.mp_ai_player_mask,
                          g_td5.ini.others_ai, s_demo_mode);
                s_slot_state[i].state = 1;
            }
        }
    }
    /* Propagate player/AI state to physics module for dynamics dispatch */
    for (int i = 0; i < TD5_MAX_RACER_SLOTS; i++) {
        td5_physics_set_race_slot_state(i, s_slot_state[i].state == 1 ? 1 : 0);
    }
    TD5_LOG_I(LOG_TAG,
              "InitRace step 3/19: race slots configured split=%d time_trial=%d traffic=%d",
              g_td5.split_screen_mode, g_td5.time_trial_enabled, g_td5.traffic_enabled);
    g_game_type = g_td5.game_type;
    g_split_screen_mode = g_td5.split_screen_mode;
    g_replay_mode = s_replay_mode;
    g_demo_mode   = s_demo_mode;
    s_replay_abort_pending = 0;   /* fresh per race */
    /* Racer count:
     *   Time trial: 1 (or 2 in split-screen)
     *   Cop Chase (wanted) / any non-zero game_type: 2
     *     [CONFIRMED @ InitializeRaceActorRuntime 0x00432E60:
     *      if (g_selectedGameType != 0) g_racerCount = 2]
     *   Normal race: 6 */
    if (g_td5.time_trial_enabled) {
        g_racer_count = (g_td5.split_screen_mode > 0) ? 2 : 1;
    } else if (g_td5.wanted_mode_enabled) {
        /* MP cop chase: human suspects + AI-cop slot. SP wanted: faithful 2. */
        int f = td5_game_mp_cop_chase_field();
        g_racer_count = (f > 0) ? f : 2;  /* SP: slot 0 = player, slot 1 = cop AI */
    } else if (g_td5.num_human_players >= 1) {
        /* Single race / Quick Race: humans + opponents (<=6) [PORT ENHANCEMENT].
         * Default (1+5) yields 6 — legacy behavior. */
        int total = g_td5.num_human_players + g_td5.num_ai_opponents;
        if (total < 1) total = 1;
        if (total > TD5_MAX_RACER_SLOTS) total = TD5_MAX_RACER_SLOTS;
        g_racer_count = total;
    } else {
        g_racer_count = TD5_MAX_RACER_SLOTS;
    }
    /* SoloRace debug override: 1-racer race (player always 1st). */
    if (g_td5.ini.solo_race && g_td5.split_screen_mode == 0) {
        g_racer_count = 1;
        TD5_LOG_I(LOG_TAG, "SoloRace: g_racer_count=1 (opponents disabled)");
    }

    /* [PORT ENHANCEMENT] Racer/traffic slot boundary. Legacy <=6-racer races
     * keep the faithful slot-6 traffic base (so the slot-9 cop encounter and
     * traffic stay byte-faithful); a >6-racer split-screen field pushes traffic
     * to slot TD5_TRAFFIC_SLOT_BASE and runs with traffic/cops OFF (the cop
     * subsystem is hardwired to the 6+6 layout — out of scope for big fields). */
    g_traffic_slot_base = (g_racer_count > TD5_LEGACY_RACE_SLOTS)
                          ? TD5_TRAFFIC_SLOT_BASE : TD5_LEGACY_RACE_SLOTS;
    if (g_racer_count > TD5_LEGACY_RACE_SLOTS && g_td5.special_encounter_enabled) {
        /* [PORT: N-way] The slot-9 special-encounter COP is hardwired to the
         * legacy 6+6 layout; in a >6-racer field slot 9 is a normal racer, so
         * disable just the special encounter. REGULAR traffic is kept and stays
         * faithful — it spawns at g_traffic_slot_base (16) and behaves exactly
         * as the original (position-relative). */
        TD5_LOG_I(LOG_TAG,
                  "InitRace: >6-racer field (%d) -> special-encounter cop disabled "
                  "(slot-9 layout); regular traffic kept faithful", g_racer_count);
        g_td5.special_encounter_enabled = 0;
    }

    /* ---- Step 4: Load track runtime data ---- */
    /* NOTE: td5_asset_load_level sets g_td5.track_type from LEVELINF.DAT,
     * so g_track_is_circuit / g_track_type_mode must be derived after this call. */
    td5_asset_load_level(g_td5.track_index);
    g_track_is_circuit = (g_td5.track_type == TD5_TRACK_CIRCUIT);
    g_track_type_mode = g_track_is_circuit ? 1 : 0;
    /* [task#14] Un-break all TD6 breakable props for the new race (lampposts/
     * bins re-stand). Load already zeroes these; this also covers a same-track
     * restart that reuses the already-loaded level. No-op on non-prop tracks. */
    td5_track_td6_props_reset_broken();
    TD5_LOG_I(LOG_TAG, "InitRace step 4/19: level runtime loaded track=%d is_circuit=%d", g_td5.track_index, g_track_is_circuit);
    CK("ck4_after_load_level");

    /* [FIX 2026-05-25 circuit-checkpoint-timer-decrement]: the circuit clear
     * at line 1157 reads g_track_is_circuit BEFORE td5_asset_load_level (just
     * above) sets it from LEVELINF.DAT. On the first race of a session
     * g_track_is_circuit is 0 (default); on subsequent races it carries the
     * previous race's value. Either way it can leak g_special_encounter=1 into
     * a circuit race, which makes (a) tick_pending_finish_timer decrement
     * s_metrics[].timer_ticks every sub-tick (it early-returns on
     * g_special_encounter==0) and (b) the bit-0x40 metric-digit HUD widget
     * draw the P2P countdown (gated on g_special_encounter!=0 at
     * td5_hud.c:2314). Result: a 3-digit countdown that "runs out of time"
     * on circuit races, despite the step-21 init seeding timer_ticks=0x7FFF.
     * Re-apply the circuit clear now that g_track_is_circuit reflects the
     * loaded track. */
    if (g_track_is_circuit && g_special_encounter != 0) {
        g_special_encounter = 0;
        TD5_LOG_I(LOG_TAG,
                  "InitRace: g_special_encounter cleared post-load (circuit track)");
    }

    /* ---- Step 4a: Per-level traffic gate from LEVELINF+0x30 ----
     * Faithful port of InitializeRaceSession @ 0x0042AE7A-0x0042AE80:
     *     if (g_trackEnvironmentConfig[0xc] == 0)   // int32 index 0xc = byte +0x30
     *         gTrafficActorsEnabled = 0;
     * LEVELINF+0x30 (int32): 1 = traffic allowed, 0 = no traffic.
     * Byte-verified across all 20 level archives (Moscow/001=0x01,
     * Newcastle/029=0x00, circuits 028/037=0x00, drag 030=0x00).
     * MUST run before step 5b below, which loads traffic vehicle meshes
     * predicated on traffic_enabled. */
    if (g_track_environment_config) {
        int32_t levelinf_traffic_flag = 0;
        memcpy(&levelinf_traffic_flag, g_track_environment_config + 0x30, sizeof(int32_t));
        TD5_LOG_I(LOG_TAG, "InitRace step 4a: LEVELINF traffic flag (+0x30) = %d", levelinf_traffic_flag);
        if (levelinf_traffic_flag == 0 && g_td5.traffic_enabled) {
            /* [dynamic-traffic OnCircuits] The dynamic spawner needs no
             * TRAFFIC.BUS and handles ring wrap, so circuits (and TD6
             * conversions, whose synthesized LEVELINF zeroes this flag) can
             * carry ambient traffic — keep the toggle's value and let step 5b
             * fall back to a default model row. Faithful behavior (clear)
             * with Dynamic=0 or OnCircuits=0. */
            if (g_td5.ini.traffic_dynamic && g_td5.ini.traffic_dyn_circuits) {
                TD5_LOG_I(LOG_TAG,
                          "InitRace step 4a: LEVELINF+0x30=0 on track=%d (is_circuit=%d) "
                          "— OVERRIDDEN by [Traffic] Dynamic+OnCircuits (traffic stays on)",
                          g_td5.track_index, g_track_is_circuit);
            } else {
                TD5_LOG_I(LOG_TAG,
                          "InitRace step 4a: LEVELINF+0x30=0 on track=%d (is_circuit=%d) — clearing traffic_enabled",
                          g_td5.track_index, g_track_is_circuit);
                g_td5.traffic_enabled = 0;
            }
        }
    }

    /* ---- Step 4b: Initialize per-track fog from LEVELINF.DAT ----
     * Original: 0x42AE56 reads fog_enable at +0x5C, fog RGB at +0x60..+0x62,
     * constructs color = (R<<16)|(G<<8)|B and passes to FUN_0040af10. */
    if (g_track_environment_config) {
        int32_t fog_enable;
        memcpy(&fog_enable, g_track_environment_config + 0x5C, sizeof(int32_t));
        if (fog_enable && g_td5.ini.fog_enabled) {
            uint8_t fog_r = g_track_environment_config[0x60];
            uint8_t fog_g = g_track_environment_config[0x61];
            uint8_t fog_b = g_track_environment_config[0x62];
            uint32_t fog_color = ((uint32_t)fog_r << 16) | ((uint32_t)fog_g << 8) | (uint32_t)fog_b;
            td5_render_configure_fog(fog_color, 1);
            TD5_LOG_I(LOG_TAG, "Fog enabled from LEVELINF: color=0x%06X (R=%02X G=%02X B=%02X)",
                      fog_color, fog_r, fog_g, fog_b);
        } else {
            td5_render_configure_fog(0x808080, 0);
            TD5_LOG_I(LOG_TAG, "Fog disabled (levelinf_flag=%d user_pref=%d)",
                      fog_enable, g_td5.ini.fog_enabled);
        }
    } else {
        td5_render_configure_fog(0x808080, 0);
        TD5_LOG_I(LOG_TAG, "Fog disabled (no LEVELINF data)");
    }

    /* ---- Step 4b: Initialize race sound resources ---- */
    td5_sound_init_race_resources();

    /* ---- Cop Chase: force the player into a POLICE car ----
     * [FIX 2026-05-30 cop-chase] Faithful to the original car-select clamp for
     * game_type 8: CarSelectionScreenStateMachine @ 0x0040E0B3 restricts the
     * player's EXT car id to 0x21..0x24 = the 4 police cars (cop/sp5/sp6/sp7.zip
     * = port s_car_zip_paths indices 33..36 = Police Cerbera/Mustang/Charger/
     * Camaro). The port's frontend never clamped, so the player kept their menu
     * car (a non-police model — the user saw a regular Cerbera). Clamp here, the
     * single point before the model/cardef/sound load below picks up
     * g_td5.car_index. Respect an already-police choice (33..36); otherwise
     * default to the Police Cerbera (33 = cars/cop.zip). Suspects (slots 1..5)
     * stay varied via the normal opponent roster — only the player is police
     * (CONFIRMED: orig opponent loop @ 0x0040DD5B uses the difficulty roster,
     * not the police set). */
    if (g_td5.wanted_mode_enabled && td5_game_cop_chase_is_cop(0)) {
        /* Force the police car ONLY when SLOT 0 is A cop — SP wanted mode, or MP
         * cop chase where the host (slot 0) chose the cop role. When the cop is an AI
         * or slot 0 is a suspect, slot 0 KEEPS its chosen car (other human cops get
         * their police car from the per-pane cop roster via the schedule + the cop
         * render path). [multi-cop 2026-06-24] is_cop is mask-aware so slot 0 being a
         * cop is detected even when it isn't the primary. This fixes
         * "player 1 is a cop despite choosing another car" in AI-cop chase.
         * Valid cop cars = TD5 police 33..36 OR ported TD6 cops cp1..cp4 (46..49). */
        int is_cop = (g_td5.car_index >= 33 && g_td5.car_index <= 36) ||
                     (g_td5.car_index >= 46 && g_td5.car_index <= 49);
        if (!is_cop) {
            TD5_LOG_I(LOG_TAG,
                      "Cop Chase: forcing player car %d -> 33 (Police Cerbera, cop.zip)",
                      g_td5.car_index);
            g_td5.car_index = 33;
        } else {
            TD5_LOG_I(LOG_TAG, "Cop Chase: player car %d already a police car",
                      g_td5.car_index);
        }
    } else if (g_td5.wanted_mode_enabled) {
        TD5_LOG_I(LOG_TAG, "Cop Chase: slot 0 is a suspect (cop slot=%d) — keeping chosen car %d",
                  td5_game_cop_chase_cop_slot(), g_td5.car_index);
    }

    /* ---- Step 5: Load vehicle assets and sound banks for all active slots ---- */
    for (int i = 0; i < TD5_MAX_RACER_SLOTS; i++) {
        if (s_slot_state[i].state != 3) {
            /* [S31] Net race: slot 0 must come from the replicated schedule
             * too. g_td5.car_index is the LOCAL player's pick, but on a
             * client the local player is NOT slot 0 -- the host is. Using it
             * here loaded the client's own model AND carparam.dat for the
             * host's car ("client sees the same car twice"), and a carparam
             * mismatch is a physics desync, not just a visual one. */
            int car_for_slot = (i == 0 && !g_td5.network_active)
                                   ? g_td5.car_index
                                   : g_td5.ai_car_indices[i];
            /* Per-slot paint scheme: slot 0 = player's chosen colour, slots 1-5 =
             * AI variants. Committed in InitializeRaceSeriesSchedule. Without this
             * the loader always used carskin0 (the default colour). */
            int paint_for_slot = g_td5.ai_car_variants[i];
            td5_asset_load_vehicle(car_for_slot, i, paint_for_slot);

            /* The player's TD6 paint colour is baked into the BODY texels of the
             * skin at load time (td5_asset_load_vehicle_skin_painted, masked by
             * carmask.png) so glass/lights/chrome/tyres keep their own colours and
             * the wheels (separate hub page) stay untinted. No per-vertex tint is
             * applied here — that would re-tint the whole mesh, fixed parts and
             * wheels included. The photo booth loads the skin UNbaked (grey body)
             * so the menu can preview the paint live. */

            /* Load per-vehicle sound bank (Drive.wav, Rev.wav/Reverb.wav, Horn.wav).
             * [PORT: N-way] Every LOCAL human player's car uses the reverb engine
             * audio (is_reverb=1) so it gets the proper drive/rumble sound rather
             * than the AI fixed-idle whine (gated on !s_reverb_flag in
             * td5_sound). AI opponents (i >= num_human_players) stay non-reverb.
             * When a TD6 player-car override is active, source the bank from the
             * TD6 archive (donor wavs in re/assets/cars/<code>/) instead of the
             * menu car's TD5 zip. */
            const char *car_zip = (i == 0 && td5_asset_player_override_active())
                                      ? td5_asset_get_player_override_zip()
                                      : td5_asset_get_car_zip_path(car_for_slot);
            if (car_zip) {
                int is_human = (i < g_td5.num_human_players);
                td5_sound_load_vehicle_bank(car_zip, i, is_human ? 1 : 0);
            }

            TD5_LOG_I(LOG_TAG, "InitRace step 5/19: vehicle asset loaded slot=%d car_index=%d",
                      i, car_for_slot);
        }
    }

    /* ---- Step 5b: Load traffic vehicle models (Phase 4 of LoadRaceVehicleAssets @ 0x00443280) ----
     * Populates actor slots 6..11 with traffic.zip/model%d.prr meshes.
     * Without this, td5_render_get_vehicle_mesh(slot>=6) returns NULL and
     * the render loop at td5_render.c:1647 silently skips every traffic
     * actor even though td5_ai + td5_physics are ticking them.
     *
     * Model selection now uses the original's two-level lookup chain via
     * td5_asset_resolve_traffic_model_index: per-track row in DAT_00474ce8
     * selected via DAT_00474d74[g_trackPoolIndex]. Resolver returns -1
     * when the track's pool index >= 25, matching the original guard
     * @ 0x004435ad (`iVar15 < 0x19`) which gates a zero-sized traffic
     * allocation.
     *
     * Gate: traffic is the user-facing Traffic toggle (g_td5.traffic_enabled,
     * committed from the Quick Race / Multiplayer track-select row); network
     * races already force it off above; time-trial and drag-race don't spawn
     * traffic actors.
     *
     * [FIX 2026-06-04 S05] Two prior bugs are corrected here so the Traffic
     * toggle actually gates spawning in every mode the row is exposed:
     *   (1) The mesh slot was TD5_MAX_RACER_SLOTS+ti. That constant was 6 when
     *       this loop was written (commit 9a4c1c2) but the N-way work bumped it
     *       to 16, so meshes loaded into slots 16..21 — rejected by
     *       td5_asset_load_traffic_model's "slot >= 12" guard — leaving traffic
     *       INVISIBLE even in single-player. Traffic actors live at
     *       g_traffic_slot_base..+5 (6..11 for the legacy/<=6 field), so load
     *       the mesh into the SAME slot the actor (and renderer) use.
     *   (2) The gate required split_screen_mode == 0, so split-screen never
     *       loaded traffic meshes (yet td5_ai still spawned the actors — they
     *       were meshless/invisible). The port KEEPS traffic in <=6 splits (see
     *       the N-way deviation in step 3), so load it there too.
     *
     * [FIX 2026-06-23 big-field-traffic-invisible] Big (>6-racer) fields keep
     *       traffic at slot base 16 (g_traffic_slot_base..+15). The AI side
     *       already spawns + simulates those actors (td5_ai_init_traffic_actors,
     *       gated on g_active_actor_count > g_traffic_slot_base) so the player
     *       COLLIDES with them — but this loop used to gate on
     *       g_traffic_slot_base == TD5_LEGACY_RACE_SLOTS and skip the mesh load
     *       for big fields, so the traffic was collidable-but-INVISIBLE (the
     *       "8-opponent invisible cars" bug). That gate's premise — a 12-slot
     *       vehicle-mesh array — is stale: TD5_ACTOR_MAX_TOTAL_SLOTS is now 32
     *       and td5_asset_load_traffic_model accepts any slot < 32, deriving the
     *       skin page from (slot - g_traffic_slot_base). So load meshes for ANY
     *       field whose base spawns traffic — the gate is removed below.
     *
     * Reverse-direction flag (TRAFFIC.BUS entry flags bit 0) is applied in
     * td5_ai_init_traffic_actors / td5_ai_recycle_traffic_actor via +0x80000
     * heading offset [CONFIRMED @ 0x00435786, 0x00435C00]. */
    /* [DRAG RACE TRAFFIC 2026-06-30] SP drag never has traffic, but the MP DRAG mode
     * with its TRAFFIC option DOES (the AI dynamic spawner already seeds + collides
     * the actors — they were just meshless/invisible because this loader excluded all
     * drag). Allow the mesh load for MP drag; traffic_enabled is the master gate (only
     * set when the drag TRAFFIC option is on), so SP drag still skips it. */
    if (g_td5.traffic_enabled
        && !g_td5.time_trial_enabled
        && (!g_td5.drag_race_enabled || td5_game_drag_mp_active())) {
        /* [POLICE rewrite 2026-06-19] Traffic now runs in net races, so load its
         * meshes here too (was gated !network_active when net had no traffic) —
         * otherwise net traffic/cops would be invisible.
         * [FIX 2026-06-23 big-field-traffic-invisible] No longer gated on
         * g_traffic_slot_base == TD5_LEGACY_RACE_SLOTS — see the step-5b header
         * comment above. The loop below uses g_traffic_slot_base as its base, so
         * it loads the right slots (6..21 legacy / 16..31 big field) either way. */
        int traffic_loaded = 0;
        /* [POLICE rewrite 2026-06-19] The per-track POLICE car lives at a fixed
         * traffic-pool slot (TD5 = slot 3, the original cop-slot-9 model; TD6
         * city = slot 5, the city police car). Keep CIVILIAN traffic OFF that
         * slot so the only police-liveried cars on the road are actual cops
         * (which draw the police mesh via the cop override) — otherwise the
         * road fills with police-looking civilians and the player can't tell
         * which car will give chase. */
        int police_pool_slot =
            (td5_asset_td6_city_traffic_base(g_active_td6_level) >= 0) ? 5 : 3;
        for (int ti = 0; ti < TD5_MAX_TRAFFIC_SLOTS; ti++) {
            int traffic_slot  = g_traffic_slot_base + ti;  /* 6..21 legacy base / 16..31 big field */
            /* Only 6 distinct traffic car models exist per track; slots past the
             * 6th reuse them (model_slot wraps, matching the skin-page wrap in
             * td5_asset_load_traffic_model). */
            int model_slot    = ti % 6;
            if (model_slot == police_pool_slot)
                model_slot = 0;   /* civilians never use the police model */
            int traffic_model = td5_asset_resolve_traffic_model_index(
                g_td5.track_index, /*reverse=*/0, model_slot);
            if (traffic_model < 0 &&
                g_td5.ini.traffic_dynamic && g_td5.ini.traffic_dyn_circuits) {
                /* [dynamic-traffic OnCircuits] circuits / TD6 conversions have
                 * no row in the per-track traffic-model tables (pool_idx>=25
                 * in the original). Borrow track 0's (Moscow's) model set so
                 * the dynamic spawner has meshes to dress its actors with. */
                traffic_model = td5_asset_resolve_traffic_model_index(0, 0, model_slot);
                if (traffic_model >= 0)
                    TD5_LOG_I(LOG_TAG,
                              "InitRace step 5b: track_index=%d slot_in_pool=%d "
                              "using fallback traffic model %d (track-0 row)",
                              g_td5.track_index, ti, traffic_model);
            }
            /* [2026-06-12] TD6 CITY traffic: replace the borrowed track-0 cars
             * with the REAL imported per-city TD6 models (London/Paris/Rome/NY/
             * Hong Kong). Overrides whatever was resolved above for the 5 city
             * tracks; non-city TD6 (circuits) and native TD5 are unaffected. */
            {
                int td6_city_base = td5_asset_td6_city_traffic_base(g_active_td6_level);
                if (td6_city_base >= 0) {
                    traffic_model = td6_city_base + model_slot;
                    TD5_LOG_I(LOG_TAG,
                              "InitRace step 5b: TD6 city level=%d slot_in_pool=%d "
                              "-> real city traffic model %d",
                              g_active_td6_level, ti, traffic_model);
                }
            }
            if (traffic_model < 0) {
                TD5_LOG_I(LOG_TAG,
                          "InitRace step 5b: track_index=%d slot_in_pool=%d has no traffic model (pool_idx>=25 or unavailable)",
                          g_td5.track_index, ti);
                continue;
            }
            if (td5_asset_load_traffic_model(traffic_model, traffic_slot)) {
                /* Mirror orig LoadRaceVehicleAssets @ 0x00443280 traffic loop:
                 * copy slot-0 cardef into the traffic slot's cardef row. Without
                 * this, bind_default_vehicle_tuning falls through to the all-zero
                 * fallback for traffic, leaving CDEF_S(actor, 0x86) = 0 (no ground
                 * lift → traffic clips through the road) and cardef[0x0C] = 0
                 * (car_half_w wrong in process_traffic_route_advance). */
                td5_physics_seed_traffic_cardef_from_player(traffic_slot);
                traffic_loaded++;
            } else {
                TD5_LOG_W(LOG_TAG,
                          "InitRace step 5b: traffic slot %d (model %d) load failed",
                          traffic_slot, traffic_model);
            }
        }
        TD5_LOG_I(LOG_TAG,
                  "InitRace step 5b/19: traffic vehicle assets loaded (%d/%d slots, "
                  "track_index=%d traffic_base=%d %s)",
                  traffic_loaded, TD5_MAX_TRAFFIC_SLOTS, g_td5.track_index,
                  g_traffic_slot_base,
                  g_traffic_slot_base == TD5_LEGACY_RACE_SLOTS ? "legacy-field"
                                                               : "big-field");

        /* [POLICE rewrite 2026-06-19] Resolve + load each track's DEDICATED
         * police mesh so cops read as the right police car (not an ordinary
         * car). The original speeding cop was traffic slot 9 = traffic-pool
         * slot 3, so pool slot 3 is the game's per-track police model (e.g.
         * Moscow = model 12, the ГАИ van). A TD6 city uses that city's police
         * model (pool slot 5 = city_base+5, e.g. London lopol=36). Only when a
         * track has no dedicated police model (e.g. a circuit with no traffic
         * row) do we fall back to the London police car (36). Cosmetic, loaded
         * once on its own texture page; NULL -> cops draw their normal mesh. */
        {
            int td6_base  = td5_asset_td6_city_traffic_base(g_active_td6_level);
            int cop_model;
            if (td6_base >= 0) {
                cop_model = td6_base + 5;                /* TD6 city police */
            } else {
                cop_model = td5_asset_resolve_traffic_model_index(
                                g_td5.track_index, /*reverse=*/0, /*pool slot*/3);
                if (cop_model < 0) cop_model = 36;       /* fallback: London police */
            }
            td5_render_set_cop_mesh(td5_asset_load_cop_mesh(cop_model));
            TD5_LOG_I(LOG_TAG, "InitRace step 5b: cop mesh model=%d (td6_base=%d track=%d)",
                      cop_model, td6_base, g_td5.track_index);
        }
    } else {
        td5_render_set_cop_mesh(NULL);   /* no traffic -> no cops -> no cop mesh */
        TD5_LOG_I(LOG_TAG,
                  "InitRace step 5b/19: traffic disabled (traffic_enabled=%d time_trial=%d drag=%d net=%d split=%d traffic_base=%d)",
                  g_td5.traffic_enabled, g_td5.time_trial_enabled,
                  g_td5.drag_race_enabled, g_td5.network_active,
                  g_td5.split_screen_mode, g_traffic_slot_base);
    }

    /* ---- Step 6: Bind track strip runtime pointers ---- */
    /* (Internal to td5_asset_load_level -- strip data is patched in place) */
    TD5_LOG_I(LOG_TAG, "InitRace step 6/19: track strip runtime pointers bound");

    /* ---- Step 6b: Trackside (cinematic) replay-camera profiles ----
     * A View-Replay race runs the original's cinematic trackside cameras. Bind
     * this track's authored profile table (selected by world_x = the same pool
     * record the traffic/track loaders use, direction-aware) and count the valid
     * profiles. Mirrors orig InitializeRaceSession @0x42aa10: after
     * LoadTrackRuntimeData binds gTracksideCameraProfiles, it calls
     * InitializeTracksideCameraProfiles when g_inputPlaybackActive. Runs after
     * step 6 so the strip-span/vertex tables the profiles index are already
     * bound. No-op for a normal race. If the track/direction has no profile
     * table (e.g. reverse unavailable, or an unused pool slot), the count stays
     * 0 and the per-frame replay camera falls back to chase so the played-back
     * car is still visible (see td5_camera_update_transition_state). Benchmark
     * trackside cameras (orig also inits them) are left on chase — deferred. */
    if (s_replay_mode) {
        extern int g_trackType;   /* camera circuit-orbit-override flag */
        int world_x = td5_asset_track_pool_index(g_td5.track_index,
                                                  g_td5.reverse_direction);
        g_trackType = g_track_is_circuit;   /* faithful: circuit forces orbit mode */
        td5_camera_bind_trackside_profiles(world_x);
        InitializeTracksideCameraProfiles();   /* self-guards NULL -> count 0 */
        TD5_LOG_I(LOG_TAG,
                  "InitRace step 6b: replay trackside cameras track=%d reverse=%d "
                  "world_x=%d circuit=%d ready=%d",
                  g_td5.track_index, g_td5.reverse_direction, world_x,
                  g_track_is_circuit, td5_camera_replay_trackside_ready());
    }

    /* ---- Step 7: Parse MODELS.DAT from level ZIP ---- */
    /* (Loaded as part of td5_asset_load_level) */
    TD5_LOG_I(LOG_TAG, "InitRace step 7/19: MODELS.DAT parsed from level assets");

    /* ---- Step 8: Load track textures ---- */
    td5_asset_load_track_textures(g_td5.track_index);
    /* Must run AFTER textures load so the per-page transparency table is
     * populated. Dims billboard meshes that use a type-3 (additive) page. */
    td5_track_dim_additive_billboard_meshes();
    /* [LIGHT2 P1] Give normal-less track meshes derived face normals so the
     * G-buffer covers roads/walls (sun-shadow backface skip, SSR). No-op in
     * Mode 0; baked vertex lighting is never touched. */
    td5_track_derive_missing_normals();
    /* [NATIVE BANNERS] Detect native-track double-sided sign panels (coincident
     * reverse-wound primitive pairs) so the banner one-sided cull applies to
     * their pages — stops Tokyo/Kyoto/etc. banners z-fighting/reading mirrored. */
    td5_track_scan_banner_pages();
    /* [LIGHT2] Register the track's street-lamp glow fixtures as light
     * sources (nearest few become real point lights per frame when dark). */
    td5_track_register_lamp_lights();
    TD5_LOG_I(LOG_TAG, "InitRace step 8/19: track textures loaded for track=%d",
              g_td5.track_index);

    /* ---- Step 9: Load sky mesh (SKY.PRR from STATIC.ZIP) ---- */
    /* (Loaded as part of td5_asset_load_track_textures static resource pass) */
    TD5_LOG_I(LOG_TAG, "InitRace step 9/19: sky mesh/static resources prepared");

    /* ---- Step 10: Initialize race vehicle runtime ---- */
    /* Initialize per-actor race metrics.
     * s_metrics[i].display_position stays 0 (from memset) — original's
     * equivalent field is only written by UpdateRaceOrder @ 0x0042F5B0
     * at sim_tick>=1, not at init. Only s_race_order[] seeds identity
     * (that's the original's g_raceOrderTable, which IS initialized to
     * 0..5 before first UpdateRaceOrder). */
    memset(s_metrics, 0, sizeof(s_metrics));
    memset(s_arrest_time, 0, sizeof(s_arrest_time));   /* [MP COP CHASE results] clear arrest times */
    for (int i = 0; i < TD5_MAX_RACER_SLOTS; i++) {
        s_race_order[i] = (uint8_t)i;
    }
    TD5_LOG_I(LOG_TAG, "InitRace step 10/19: race metrics/runtime arrays reset");

    /* ---- Step 11: Allocate actors and init vehicle/AI runtime ---- */
    {
        static uint8_t s_actor_memory[TD5_ACTOR_STRIDE * TD5_MAX_TOTAL_ACTORS];
        /* Racer/traffic slot budget [PORT ENHANCEMENT — original capped at 6
         * racers + 6 traffic = 12 actors]. Legacy modes keep that faithful
         * layout (traffic at the fixed slot-6 base, cop encounter at slot 9);
         * a >6-racer split-screen field uses up to TD5_MAX_RACER_SLOTS racers
         * with traffic/cops disabled (g_traffic_slot_base set in step 3).
         *   racer_count = cars positioned on the grid (slots 0..racer_count-1)
         *   spawn_count = total actor slots incl. traffic / faithful decoration */
        int racer_count;
        if (g_td5.time_trial_enabled) {
            racer_count = (g_td5.split_screen_mode > 0 ? 2 : 1);
        } else if (g_racer_count > TD5_LEGACY_RACE_SLOTS) {
            racer_count = g_racer_count;            /* big multiplayer field */
            if (racer_count > TD5_MAX_RACER_SLOTS) racer_count = TD5_MAX_RACER_SLOTS;
        } else {
            racer_count = TD5_LEGACY_RACE_SLOTS;    /* faithful up-to-6 grid */
            /* Quick Race dropped-opponents: position only the configured
             * humans+opponents; remaining legacy slots stay decoration. The
             * AI side (g_slot_state) and standings (s_slot_state) disable them. */
            if (!g_td5.drag_race_enabled && !g_td5.wanted_mode_enabled &&
                g_td5.num_human_players >= 1) {
                int total = g_td5.num_human_players + g_td5.num_ai_opponents;
                if (total < 1) total = 1;
                if (total > TD5_LEGACY_RACE_SLOTS) total = TD5_LEGACY_RACE_SLOTS;
                racer_count = total;
            }
        }
        int spawn_count;
        if (g_td5.time_trial_enabled) {
            spawn_count = racer_count;                  /* time trial: no traffic */
        } else if (g_td5.traffic_enabled) {
            /* Traffic spawns at g_traffic_slot_base..+6 (6 legacy / 16 big field).
             * In a big field the racer slots between racer_count and the base are
             * inert decoration (disabled like dropped opponents). */
            spawn_count = g_traffic_slot_base + TD5_MAX_TRAFFIC_SLOTS;
        } else if (g_racer_count > TD5_LEGACY_RACE_SLOTS) {
            spawn_count = racer_count;                  /* big field, no traffic */
        } else {
            spawn_count = TD5_LEGACY_RACE_SLOTS;        /* legacy 6-slot grid */
        }
        TD5_LOG_I(LOG_TAG,
                  "InitRace: %d racers / %d actors (traffic_base=%d humans=%d opp=%d)",
                  racer_count, spawn_count, g_traffic_slot_base,
                  g_td5.num_human_players, g_td5.num_ai_opponents);

        memset(s_actor_memory, 0, sizeof(s_actor_memory));
        g_actorBaseAddr = (int)(uintptr_t)s_actor_memory;
        g_actor_pool = s_actor_memory;
        g_actor_base = s_actor_memory;
        g_actor_table_base = s_actor_memory;
        g_td5.total_actor_count = spawn_count;
        td5_ai_bind_actor_table(s_actor_memory);

        /* [DYNAMICS COMMIT FIX 2026-06-28] Re-commit the ARCADE/SIMULATION choice
         * from the authoritative persisted value (g_td5.ini.dynamics — written by
         * EVERY dynamics selector: Quick Race, Track Select, Game Options) right
         * before vehicle + arcade init. ConfigureGameTypeFlags also commits it, but
         * it runs at screen-INIT with the PRE-toggle value; only the Quick Race OK
         * handler re-committed late. So launching from Track Select (or any path)
         * after flipping the toggle left the dynamics flag STALE — keying the arcade
         * power-up gate (and gravity / stat scaling) to the WRONG mode. The visible
         * symptom: item-box power-ups still appearing in SIMULATION. Committing here
         * makes the live mode match the user's pick on every launch path, so the
         * arcade power-ups are truly ARCADE-exclusive (td5_arcade_init_race sets
         * s_active = td5_physics_get_dynamics()==0). */
        td5_physics_set_dynamics(g_td5.ini.dynamics);
        TD5_LOG_I(LOG_TAG, "InitRace: dynamics=%s -> arcade power-ups %s",
                  g_td5.ini.dynamics ? "SIMULATION" : "ARCADE",
                  g_td5.ini.dynamics ? "OFF (sim-exclusive disabled)" : "available");

        /* Original order (0x42AFE2-0x42AFE7): vehicle + AI runtime init
         * BEFORE actor placement (step 22). */
        td5_physics_init_vehicle_runtime();

        /* Compute mesh-derived collision envelopes per slot (0x42F6D0).
         * Must run AFTER init_vehicle_runtime (which binds cardef) and
         * AFTER asset_load_vehicle (which registers mesh). */
        for (int s = 0; s < spawn_count; s++) {
            TD5_Actor *a = (TD5_Actor *)(s_actor_memory + (size_t)s * TD5_ACTOR_STRIDE);
            td5_physics_compute_suspension_envelope(a, s);
        }

        /* [ARCADE 2026-06-26] Place power-up pads along the track ring + clear
         * per-slot/hazard state for this race. No-op (and clears) in SIMULATION
         * mode; the track ring is already loaded by this point. */
        td5_arcade_init_race();

        /* [DEMO FIX #3 2026-06-15] The AI module keeps its OWN slot-state table
         * (td5_ai.c g_slot_state[], separate from this file's s_slot_state[]) and
         * td5_ai_init_race_actor_runtime() decides slot 0's AI/human state by
         * reading g_td5.ini.player_is_ai DIRECTLY (td5_ai.c ~L1776) — it does NOT
         * know about s_demo_mode. So the demo slot-0->AI flip above (s_slot_state)
         * would route physics through td5_physics_update_ai but the AI BRAIN would
         * still treat slot 0 as "player" (g_slot_state[0]==1 -> case 0x01 skip) and
         * never write a steering/throttle command -> the demo car would just idle.
         *
         * The AI module reads player_is_ai ONLY here at init (per-tick AI reads the
         * cached g_slot_state[]; and the AI's track-behavior write runs AFTER the
         * input dispatch each tick, so it always wins for slot 0). So scope a
         * player_is_ai=1 override across JUST this init call, then restore it — the
         * AI sets g_slot_state[0]=0 and drives the demo car, while every other
         * per-tick reader of player_is_ai (input dispatch / finish-brake / carparam)
         * sees the real value. This keeps the AI side in sync WITHOUT editing
         * td5_ai.c (the proper one-line fix is to OR s_demo_mode into the L1776
         * test there — see report). Gated by the same TD5RE_DEMO_FIX (resolved into
         * s_slot_state[0]==0 above): if the demo fix is off, slot 0 stays state==1
         * here and this override no-ops. Non-demo races never enter it. */
        int demo_pia_saved = g_td5.ini.player_is_ai;
        int demo_pia_override = (s_demo_mode && s_slot_state[0].state == 0 &&
                                 !g_td5.ini.player_is_ai);
        if (demo_pia_override) {
            g_td5.ini.player_is_ai = 1;
            TD5_LOG_I(LOG_TAG,
                      "InitRace: demo -> player_is_ai=1 scoped to AI runtime init "
                      "(AI g_slot_state[0]=0 so the demo car self-drives)");
        }
        td5_ai_init_race_actor_runtime();
        if (demo_pia_override) {
            g_td5.ini.player_is_ai = demo_pia_saved;  /* restore real value */
        }

        /* ---- Step 11b: Position racer actors on grid ---- */
        /* Grid patterns from InitializeRaceSession (0x42B07B-0x42B225):
         *   Circuit (0x42B110): paired rows 6 spans apart
         *   Non-circuit (0x42B174): staggered 3 spans apart */
        /* Slots 0-5 are the faithful original grid; 6-15 continue the stagger
         * (PORT ENHANCEMENT for >6-car split-screen fields). int8_t range OK (>=-48). */
        static const int8_t s_circuit_span_offsets[TD5_MAX_RACER_SLOTS] = {
            -6, -6, -12, -12, -18, -18,
            -24, -24, -30, -30, -36, -36, -42, -42, -48, -48
        };
        /* Original (0x42B174): slot 2 placed first (closest to line),
         * slot 0 (player) placed third. Per-slot offsets (6-15 extend 3 apart): */
        static const int8_t s_staggered_span_offsets[TD5_MAX_RACER_SLOTS] = {
            -9, -6, -3, -12, -15, -18,
            -21, -24, -27, -30, -33, -36, -39, -42, -45, -48
        };
        /* Wanted-mode spawn (InitializeRaceSession @ 0x42B1C6..0x42B21E):
         *   slot 0: startSpan - 3,   lane 2
         *   slot 1: startSpan +0x19, lane 2
         *   slot 2: startSpan +0x32, lane 3
         *   slot 3: startSpan +0x4B, lane 3
         *   slot 4: startSpan +0x64, lane 2
         *   slot 5: startSpan +0x7D, lane 3
         * Port lane system supports only lanes 1-2; lane 3 is clamped to 2.
         * [RE basis: deep Ghidra pass at 0x42B1C6] */
        static const int8_t s_wanted_span_offsets[TD5_MAX_RACER_SLOTS] = {
            -3, 0x19, 0x32, 0x4B, 0x64, 0x7D
        };
        static const uint8_t s_wanted_lanes[TD5_MAX_RACER_SLOTS] = {
            2, 2, 2, 2, 2, 2  /* original: 2,2,3,3,2,3 — 3 clamped to 2 */
        };
        /* Drag-race spawn is handled as a special case below in the spawn
         * loop [CONFIRMED @ InitializeRaceSession]:
         *   Slot 0: InitializeActorTrackPose(0, 0x73=115, 1, 0) — hardcoded
         *   immediate at 0x0042b0f8.
         *   Slots 1..5: LAB_0042B228 calls InitializeActorTrackPose(i, 1,
         *   i-1, 0) — span=1 absolute, lanes=0..4 (decoration).
         * The circuit 2x3 grid tables below are NOT used for drag race. */
        static const uint8_t s_racer_lanes[TD5_MAX_RACER_SLOTS] = {
            1, 2, 1, 2, 1, 2,
            1, 2, 1, 2, 1, 2, 1, 2, 1, 2
        };

        /* Per-track start span: indexed by LEVEL NUMBER (1-based, from
         * td5_asset_level_number), NOT schedule slot.
         * Circuit: 16-bit at 0x46CBB0[level*24+4]; non-circuit: byte at 0x466F6F[level]. */
        static const uint16_t s_circuit_start_span[40] = {
            /*  0 */ 0,
            /*  1 */ 869,  826,  768,  623,  747,  609,  556,  715,
            /*  9 */ 585,  466,  901,  519,  651,  486,  660,  629,
            /* 17 */ 685,  606,  665,  583,
            /* 21 */ 544,  827,  738,  694,  106,   25,  119,
            /* 28 */  56,  116,  204,  204,  106,   25,  119,
            /* 35 */  56,  116,   47,   47,   35
        };
        static const uint8_t s_noncircuit_start_span[40] = {
            /*  0 */ 0,
            /*  1 */ 114, 134,  79, 119, 124, 136, 100, 125,
            /*  9 */ 140,  95, 125,  92, 111, 101, 119,  71,
            /* 17 */ 119, 147,  78, 120,
            /* 21 */ 111, 120, 120, 139,   0,   0,   0,
            /* 28 */   0,   0,   0,   0,   0,   0,   0,
            /* 35 */   0,   0,   0,   0,   0
        };

        const int8_t  *span_offsets;
        const uint8_t *active_lanes;
        int drag_mode_spawn = g_td5.drag_race_enabled ? 1 : 0;
        if (g_td5.wanted_mode_enabled) {
            span_offsets = s_wanted_span_offsets;
            active_lanes = s_wanted_lanes;
        } else if (drag_mode_spawn) {
            /* Drag race uses the circuit 2x3 grid (see comment above). */
            span_offsets = s_circuit_span_offsets;
            active_lanes = s_racer_lanes;
        } else if (g_track_is_circuit) {
            span_offsets = s_circuit_span_offsets;
            active_lanes = s_racer_lanes;
        } else {
            span_offsets = s_staggered_span_offsets;
            active_lanes = s_racer_lanes;
        }

        int track_span_count = td5_track_get_span_count();
        int level_num = td5_asset_level_number(g_td5.track_index);
        int start_span;
        if (level_num >= 1 && level_num <= 39) {
            start_span = g_track_is_circuit
                         ? (int)s_circuit_start_span[level_num]
                         : (int)s_noncircuit_start_span[level_num];
        } else {
            start_span = (track_span_count > 0) ? track_span_count : 1;
        }
        if (start_span <= 0)
            start_span = (track_span_count > 0) ? track_span_count : 1;

        /* [TD6 PER-TRACK START SPAN] Each migrated TD6 menu track has its own
         * grid / start-finish span (k_td6_menu_slots); the TD5 per-level tables
         * above don't apply. Use the registry value directly so every TD6 track
         * grids on its real start/finish straight and the circuit lap test
         * anchors there too. Faithful TD5 tracks return 0 and keep the table
         * value byte-identically. (The single-track --OverrideStartSpan ini knob
         * still serves the AutoRace dev path via the clamp fallback below.) */
        {
            int td6_ss = td5_asset_td6_start_span_for_level(level_num);
            if (td6_ss > 0)
                start_span = td6_ss;

            /* [REVERSE CIRCUIT START] In reverse the loaded geometry is STRIPB.DAT,
             * which is reverse-numbered (STRIPB span i ~= forward span
             * span_count - i; confirmed against both native TD5 and TD6 stripb).
             * At this point start_span holds a FORWARD-strip span: the TD6
             * registry value (td6_ss) for migrated TD6 circuits, or
             * s_circuit_start_span[level_num] for native TD5 circuits. So:
             *  - POINT-TO-POINT: using it directly already lands the grid at
             *    STRIPB[fwd_start] ~= the forward FINISH = the reverse start (the
             *    same proven mapping native TD5 P2P reverse relies on), and the
             *    P2P finish (s_td6_finish_span = fwd_finish) lands at the forward
             *    START = the reverse finish. Leave it unchanged (not a circuit).
             *  - CIRCUIT (TD6 *or* native TD5): map the forward start span into
             *    STRIPB numbering (ring - fwd_start) so the lap/start line and the
             *    grid sit on the start BANNER. Without this the reverse circuit
             *    still laps correctly but its start/finish line sits a different
             *    physical span away from the visible banner — and for native
             *    circuits (whose s_circuit_start_span is large, e.g. 106) the grid
             *    would spawn well into the track instead of on the start straight.
             *  Native TD5 circuits only reach here once gen_reverse_track.py has
             *  emitted their stripb.json reverse data: load_level clears
             *  g_td5.reverse_direction for any circuit still lacking reverse data,
             *  so this mirror never fires on a forward-only track. [2026-06-28]
             *  Use the MAIN-ROAD ring length (g_td5.track_span_ring_length =
             *  STRIP hdr[1]), NOT td5_track_get_span_count() which includes
             *  appended branch spans — the reverse-numbering relation
             *  (STRIPB span i ~= forward span ring-i) is defined over the main
             *  ring only, so mixing in branch spans skews the mapped span. */
            int rev_ring = g_td5.track_span_ring_length;
            if (g_td5.reverse_direction && g_track_is_circuit && rev_ring > 1) {
                int fwd_ss = start_span;
                int rev_ss = rev_ring - fwd_ss;
                if (rev_ss < 1) rev_ss = 1;
                if (rev_ss >= rev_ring) rev_ss = rev_ring - 1;
                TD5_LOG_I(LOG_TAG,
                          "reverse circuit start span: fwd=%d -> rev=%d (ring=%d total=%d td6=%d)",
                          fwd_ss, rev_ss, rev_ring, track_span_count, td6_ss);
                start_span = rev_ss;
            }
        }

        /* [CUSTOM TRACK] Honor the registry start span directly, INCLUDING 0
         * (the span-0 start/finish line of a custom circuit, which the td6_ss>0
         * gate above cannot express). Custom levels otherwise fall through to
         * the out-of-range clamp below (start_span defaulted to span_count). */
        if (td5_track_registry_has_level(level_num)) {
            int css = td5_track_registry_start_span_for_level(level_num);
            if (css >= 0 && css < track_span_count)
                start_span = css;
        }

        /* TD6 track migration / out-of-range robustness: the per-level
         * start-span tables are indexed by level number and only meaningful for
         * the 39 shipped TD5 tracks. An OverrideTrackZip'd TD6 track (or any
         * out-of-range level) can yield a start_span PAST the end of the actual
         * strip -> every racer clamps to the wrap point and spawns cramped on
         * top of each other (observed on the TD6 level007 proof). Clamp into the
         * strip mid-ring when that happens. This NEVER fires for faithful tracks
         * (their table value is always < span_count), so faithful spawn stays
         * byte-identical. */
        if (track_span_count > 0 && start_span >= track_span_count) {
            int safe;
            if (g_td5.ini.override_start_span > 0 &&
                g_td5.ini.override_start_span < track_span_count) {
                /* Explicit grid span for this TD6 track — point it at the real
                 * start/finish straight (widest, straightest section). */
                safe = g_td5.ini.override_start_span;
            } else {
                /* Fallback: opening straight. Mid-ring lands on a corner/runoff
                 * and the racers spawn on grass. */
                safe = (track_span_count > 48) ? 24
                       : (track_span_count > 1 ? track_span_count / 2 : 1);
            }
            TD5_LOG_W(LOG_TAG, "start_span %d >= span_count %d (out-of-range level) "
                      "-> grid span %d", start_span, track_span_count, safe);
            start_span = safe;
        }

        /* StartSpanOffset (td5re.ini [Game] or --StartSpanOffset=N): mirrors
         * the Frida hook on InitializeActorTrackPose (0x00434350) which does
         *   esp.add(8).writeS16((span + offset) & 0xFFFF)
         * That's 16-bit signed wraparound, matching the original's own
         * signed-short arithmetic in InitializeRaceSession @ 0x0042aa10:
         *   sVar12 = (short)g_trackStartSpanIndex + -0xF;  // signed short wrap
         *   InitializeActorTrackPose(..., sVar12, ...);
         * The previous port wrapped mod track_span_count which sanitized
         * behavior the original never had — causing ~270u z divergence on
         * negative offsets and ~5.7k y divergence on ring-overshoot offsets.
         *
         * Do NOT apply the offset to the base start_span here. The base stays
         * at its circuit/non-circuit table value and gets published to
         * g_trackStartSpanIndex unchanged (matches 0x0042b076 WRITE — the
         * original never mutates g_trackStartSpanIndex after the table load).
         * Per-slot application happens in the spawn loop below, sign-extended
         * to 16 bits exactly like the Frida writeS16 path. */
        /* [BUG 5b 2026-06-15] Replay StartSpanOffset restore. A replay records
         * only player INPUT; the spawn positions come from the dev StartSpanOffset
         * (g_td5.ini.start_span_offset), read fresh HERE at both record and
         * playback. If the live INI/CLI offset at playback differs from the one
         * the run was recorded under, the cars spawn at a different span and the
         * recorded input — which assumes the recorded spawn — plays back from the
         * wrong start, so the whole run diverges. The recorded value is captured
         * into the replay buffer at WriteOpen (and persisted in the v2 file
         * header); on playback use THAT value (not the live one) for the per-slot
         * spawn below, so playback reproduces the recorded grid. We resolve it into
         * a local (effective_start_span_offset) instead of mutating the global ini,
         * so a later non-replay race in this session still spawns from its own
         * configured offset. Gated by TD5RE_REPLAY_OFFSET_FIX (cached): "0" reverts
         * to using the live offset (old, diverging behaviour). */
        int effective_start_span_offset = g_td5.ini.start_span_offset;
        if (s_replay_mode) {
            static int s_replay_off_init = 0;
            static int s_replay_off_enabled = 1;
            if (!s_replay_off_init) {
                s_replay_off_enabled = td5_env_flag_on("TD5RE_REPLAY_OFFSET_FIX");  /* default ON */
                s_replay_off_init = 1;
                TD5_LOG_I(LOG_TAG, "Replay StartSpanOffset restore: %s",
                          s_replay_off_enabled ? "enabled" : "disabled (live offset)");
            }
            if (s_replay_off_enabled) {
                /* For the default in-memory View Replay the recorded buffer (and
                 * its captured offset) survives from the recording's WriteOpen, so
                 * it is already available. A disk-persisted replay does not load
                 * until InitRace step 12 (below, after the spawn loop), so pull it
                 * in now; the step-12 read_open reloads + resets the read cursor,
                 * so this early load is idempotent. */
                if (g_td5.ini.replay_persist_to_disk)
                    td5_input_read_open("replay.td5");
                int captured = (int)td5_input_replay_start_span_offset();
                if (captured != effective_start_span_offset) {
                    TD5_LOG_I(LOG_TAG,
                              "InitRace: replay start_span_offset live=%d -> recorded=%d",
                              effective_start_span_offset, captured);
                }
                effective_start_span_offset = captured;
            }
        }

        if (effective_start_span_offset != 0) {
            TD5_LOG_I(LOG_TAG,
                      "start_span_offset=%d will be applied per-slot (16-bit sign-extended)",
                      effective_start_span_offset);
        }

        /* [MP COP CHASE 2026-06-23] Cop chase spawns everyone TOGETHER on
         * the same span (the start line) on different lanes — the AI cop right
         * beside the suspects, no down-track stagger and no positional head start —
         * so the chase begins fair + simultaneous (per user request). */
        int mp_cop_mode = (td5_game_mp_cop_chase_field() > 0);

        /* Publish start_span as g_trackStartSpanIndex — consumed by the
         * circuit 4-case sector dispatch in advance_pending_finish_state
         * (verbatim port of CheckRaceCompletionState @ 0x00409E80). This
         * holds the unshifted base — matches original 0x0042b076. */
        g_td5.track_start_span_index = start_span;

        /* [TD6 P2P FINISH] Resolve this track's finish span once (0 for circuits
         * and faithful TD5 tracks). Consumed by advance_pending_finish_state's
         * P2P branch — the only finish trigger for checkpoint-less TD6 P2P
         * tracks. */
        s_td6_finish_span = td5_asset_td6_finish_span_for_level(level_num);

        /* [TD6 SYNTHESIZED CHECKPOINTS] Resolve this track's checkpoint spans
         * once. Gated on g_active_td6_level (NOT level_num) so faithful TD5
         * tracks that reuse a TD6 output-level number get none. Registered as
         * drive-throughs in advance_pending_finish_state (split + player HUD
         * ack), with no fail-timer and without gating the finish. */
        /* Synthesized checkpoints are FORWARD-strip spans (extracted from the
         * forward checkpoint-banner meshes). In reverse the player drives the
         * reverse-numbered STRIPB and the normalized span counts up 0..ring-1, so
         * MIRROR each forward span F to its STRIPB span (ring-1-F) and sort ascending
         * — then the same ascending-basis drive-through detection fires at the right
         * physical points, in reverse order. Non-gating (split times + minimap
         * markers); P2P reverse still finishes on s_td6_finish_span. */
        s_td6_cp_count = (g_active_td6_level > 0)
            ? td5_asset_td6_checkpoint_spans(g_active_td6_level, s_td6_cp_spans)
            : 0;
        if (s_td6_cp_count > 0 && g_td5.reverse_direction) {
            int ring = td5_track_get_ring_length();
            if (ring > 1) {
                int i, j;
                for (i = 0; i < s_td6_cp_count; i++) {
                    int rs = ring - 1 - s_td6_cp_spans[i];
                    if (rs < 0) rs = 0; else if (rs >= ring) rs = ring - 1;
                    s_td6_cp_spans[i] = rs;
                }
                for (i = 1; i < s_td6_cp_count; i++) {   /* insertion sort ascending */
                    int v = s_td6_cp_spans[i];
                    for (j = i - 1; j >= 0 && s_td6_cp_spans[j] > v; j--)
                        s_td6_cp_spans[j + 1] = s_td6_cp_spans[j];
                    s_td6_cp_spans[j + 1] = v;
                }
            }
        }
        /* [#reverse-finish 2026-06-18] In a REVERSE P2P race the player drives
         * STRIPB forward — the raw accumulator (+0x84) climbs from the grid up to
         * the far end — and the race FINISHES at the physical FORWARD START (the
         * reverse course ends where the forward course began). That is the mirror
         * of start_span (ring-1-start_span), NOT the forward finish span: the
         * forward finish mirrors to the reverse START and would end the race a few
         * dozen spans in. Mirror it so the finish (and its banner) line up with the
         * actual reverse end. */
        if (s_td6_finish_span > 0 && g_td5.reverse_direction) {
            int ring = td5_track_get_ring_length();
            if (ring > 1) {
                int fs = ring - 1 - start_span;
                if (fs < 1) fs = 1; else if (fs >= ring) fs = ring - 1;
                TD5_LOG_I(LOG_TAG,
                          "TD6 reverse finish span: start=%d fwd_finish=%d -> rev_finish=%d (ring=%d)",
                          start_span, s_td6_finish_span, fs, ring);
                s_td6_finish_span = fs;
            }
        }
        memset(s_td6_cp_index, 0, sizeof(s_td6_cp_index));
        if (s_td6_cp_count > 0) {
            TD5_LOG_I(LOG_TAG,
                      "TD6 checkpoints: level=%d count=%d first=%d last=%d",
                      g_active_td6_level, s_td6_cp_count, s_td6_cp_spans[0],
                      s_td6_cp_spans[s_td6_cp_count - 1]);
        }

        /* Drag race does NOT derive from start_span — it uses hardcoded
         * absolute span values (slot 0 = 115, slots 1..5 = 1) inside the
         * spawn loop below. Start_span stays at its per-mode default so
         * logs remain readable. */
        TD5_LOG_I(LOG_TAG, "Grid start: slot=%d level=%d circuit=%d start_span=%d span_count=%d",
                  g_td5.track_index, level_num, g_track_is_circuit, start_span, track_span_count);

        for (int slot = 0; slot < racer_count; ++slot) {
            uint8_t *actor = s_actor_memory + slot * TD5_ACTOR_STRIDE;
            int span_index;
            int world_x = 0;
            int world_y = 0;
            int world_z = 0;

            /* SoloAISlot data-swap: when set, make slot 0 spawn at slot N's
             * grid position + lane so we can test each slot's initial
             * conditions while keeping slot 0 as the camera target. Other
             * slots are state=3 so they don't drive anyway. */
            int effective_slot = slot;
            if (g_td5.solo_mode_synth && slot == 0 &&
                g_td5.split_screen_mode == 0)
            {
                int N = g_td5.ini.solo_ai_slot;
                if (N >= 0 && N < TD5_MAX_RACER_SLOTS) effective_slot = N;
            }
            int sub_lane = active_lanes[effective_slot];
            TD5_StripSpan *sp;

            if (track_span_count > 0) {
                /* Match original @ 0x0042aa10: plain signed-short arithmetic.
                 * sVar12 = (short)g_trackStartSpanIndex + <slot_offset>.
                 * Adds start_span_offset here (not to base) so the Frida hook
                 * semantics apply per-slot, exactly like writeS16() does.
                 * After summing in int32, sign-extend via (int16_t) cast —
                 * that IS the 16-bit wraparound the original/hook produce.
                 * NO remap: original FUN_00434350 stores CX directly at
                 * 0x00434377 with no bounds check; actor fields get the raw
                 * int16 value even if negative.
                 * [BUG 5b] effective_start_span_offset = the live offset normally,
                 * or the value captured when this replay was recorded (so playback
                 * spawns on the recorded grid). */
                /* MP cop chase spawns every car on the SAME span (lanes are
                 * spread instead, below); the AI cop starts right beside the
                 * suspects — no positional head start — so the chase is fair +
                 * simultaneous. Other modes keep the per-slot down-track
                 * stagger off start_span. */
                int raw;
                if (mp_cop_mode) {
                    raw = start_span + effective_start_span_offset;
                    /* The cop(s) start a few spans BEHIND the suspects (a gap to
                     * close); suspects line up together at start_span. [multi-cop
                     * 2026-06-24] is_cop is mask-aware so EVERY cop gets the head-start
                     * setback, not just the primary. */
                    if (td5_game_cop_chase_is_cop(slot))
                        raw -= g_td5.mp_mode_config.suspect_head_start;
                } else {
                    raw = start_span + span_offsets[effective_slot] + effective_start_span_offset;
                }
                span_index = (int)(int16_t)(raw & 0xFFFF);
            } else {
                span_index = 1;
            }
            /* Preserve raw int16 value for actor field writes — matches
             * original's MOV word ptr [EAX], CX @ 0x00434377.
             * span_index may be updated to 1 below (fallback for world
             * position) but actor_span stays as the signed 16-bit result. */
            int actor_span = span_index;

            /* Drag race spawn override [CONFIRMED @ 0x0042b0fb, 0x0042b228]:
             *   slot 0 → span=115, lane=1 (hardcoded immediate 0x73 at 0x42b0f8)
             *   slots 1..5 → span=1, lane=(slot-1) via LAB_0042B228 override.
             * Flip flag is 0 for all — no secondary 180° rotation [CONFIRMED
             * @ 0x0043450e: param_4 == 0 skips the flip branch].
             *
             * [PORT ENHANCEMENT — diverges from original]
             * Slot 1 spawns alongside slot 0 at the start line (span=115)
             * instead of the original's span=1 (decoration-park). Pairs with
             * the decoration_start=2 change above so the 2nd CarSelect car
             * (or P2 in split-screen) lines up next to the player on the
             * grid, matching how a drag race actually works. lane=2 keeps it
             * to the right of slot 0's lane=1; level030.zip provides 3+
             * lanes, so lane=2 is a valid drag-strip lane. */
            if (drag_mode_spawn) {
                /* [DRAG DYNAMIC FIELD 2026-06-27] Line every active car up at the
                 * start line (span 115), one per lane, spread left-to-right across
                 * the widened road. The track was widened to `field` lanes, so
                 * lane = slot maps each racer to its own lane (NFS-Underground
                 * grid). Decoration slots park back at the strip head. */
                int field  = td5_game_drag_field_size();   /* widened road width  */
                int active = td5_game_drag_active_racers(); /* cars that actually race */
                if (field < 2) field = 2;
                if (active < 1) active = 1;
                if (active > field) active = field;
                if (slot < active) {
                    span_index = 115;
                    /* Centre the racing cars in the widened road so the EXTRA LANES
                     * (field-active of them) split evenly to either side. */
                    sub_lane = slot + (field - active) / 2;
                } else {
                    span_index = 1;
                    sub_lane = (slot - active) % field;   /* parked decoration, spread */
                }
                actor_span = span_index;  /* drag hardcodes span; actor gets that value */
                TD5_LOG_I(LOG_TAG,
                          "Drag spawn override: slot=%d span=%d lane=%d (field=%d)",
                          slot, span_index, sub_lane, field);
            }

            /* [CUSTOM CIRCUIT GRID WRAP] A circuit with a low start_span (e.g. a
             * custom track starting at span 0) yields NEGATIVE spawn spans for the
             * backward grid offsets (start_span - 6/-12/-18...). Without wrapping,
             * each fails td5_track_get_span() and collapses to span 1 below,
             * STACKING the whole grid on one span (cars clip; the jammed AI can't
             * navigate). Wrap onto the ring instead. Faithful circuits use a high
             * start_span so span_index is already in range -> this is a no-op for
             * them (and never touches point-to-point or drag spawns). */
            if (g_track_is_circuit && !drag_mode_spawn && track_span_count > 0) {
                int ringw = (g_td5.track_span_ring_length > 0)
                            ? g_td5.track_span_ring_length : track_span_count;
                int wrapped = span_index % ringw;
                if (wrapped < 0) wrapped += ringw;
                if (wrapped != span_index) {
                    span_index = wrapped;
                    actor_span = wrapped;
                }
            }

            sp = td5_track_get_span(span_index);
            if (!sp) {
                span_index = 1;
                sp = td5_track_get_span(span_index);
            }
            if (!sp)
                continue;

            /* [TD6 GRID CENTERING — track-scoped] On the wide TD6 city strips the
             * paved road is only PART of the strip (sidewalks/verge are the outer
             * lanes), but the grid lanes (1,2) sit near the left rail, so the player
             * spawns off the road. Re-anchor the grid to the centre of the actual
             * ROAD BAND (the contiguous run of lanes sharing the centre lane's surface
             * class), keeping the relative lane spread. [#20] The earlier version used
             * the GEOMETRIC strip centre ((lc-1)/2), which only lands on the road when
             * the road is centred — false on e.g. London STRIPB span 20 (road at the
             * right cells) -> cars spawned on grass. The road band is road-aware and
             * works in both directions. No-op on narrow strips / faithful TD5. */
            if (g_active_td6_level > 0) {
                int lc = td5_track_span_lane_count_at(span_index);
                if (lc > 4) {
                    int band_lo = 0, band_hi = lc - 1;
                    td5_track_td6_road_band(span_index, lc, &band_lo, &band_hi);
                    sub_lane = (band_lo + band_hi) / 2 + (active_lanes[effective_slot] - 1);
                    if (sub_lane < band_lo) sub_lane = band_lo;
                    if (sub_lane > band_hi) sub_lane = band_hi;
                }
            }

            /* [MP COP CHASE — grid lanes] Cop chase cars all share the start
             * span, so spread the players ACROSS the lanes instead of
             * down-track. Each player gets a DISTINCT lane (the block is
             * centred on the drivable road); when there are more players than
             * lanes, reuse a lane modulo the lane count. Overrides the default
             * grid lane above (incl. the TD6 road-band case, which only sets
             * the band here). */
            if (mp_cop_mode) {
                int lc = td5_track_span_lane_count_at(span_index);
                int lane_lo = 0, lane_hi = (lc > 0 ? lc - 1 : 0);
                if (g_active_td6_level > 0 && lc > 4)
                    td5_track_td6_road_band(span_index, lc, &lane_lo, &lane_hi);
                int lanes = lane_hi - lane_lo + 1;
                if (lanes < 1) lanes = 1;
                /* Grid size: the suspects + the AI-cop slot (so the cop gets
                 * its own lane next to them). */
                int grid_n = td5_game_mp_cop_chase_field();
                if (grid_n < 1) grid_n = 1;
                if (grid_n <= lanes)
                    sub_lane = lane_lo + (lanes - grid_n) / 2 + effective_slot;  /* centred, distinct */
                else
                    sub_lane = lane_lo + (effective_slot % lanes);              /* reuse when out */
                if (sub_lane < lane_lo) sub_lane = lane_lo;
                if (sub_lane > lane_hi) sub_lane = lane_hi;
            }

            /* InitActorTrackSegmentPlacement @ 0x00445F10 seeds:
             *   param_1[0] = spawn_span (+0x80)
             *   param_1[2] = spawn_span (+0x84 — accumulated span counter)
             *   param_1[3] = spawn_span (+0x86 — high-water mark)
             * NormalizeActorTrackWrapState @ 0x00443FB0 then derives +0x82
             * from +0x84. Without seeding +0x84/+0x86 to spawn_span, the very
             * first backward boundary crossing in update_position_recursive
             * decrements +0x84 from 0 to -1, td5_track_normalize_actor_wrap
             * wraps -1 to ring_length-1 (~3999), and every P2P checkpoint
             * threshold compares true at once.
             *
             * Do NOT pre-seed +0x82 (span_normalized): the original
             * InitActorTrackSegmentPlacement writes +0x80/+0x84/+0x86 only
             * and leaves +0x82 at the zero-memset value until
             * NormalizeActorTrackWrapState derives it on the first sub-tick.
             * Setting it to spawn_span here made the AI's first cascade call
             * (during countdown sub-tick=0, before any physics-integrate or
             * wrap-normalize runs) compute target_span = span_norm+4 = 66
             * instead of the original's 0+4 = 4. That shifted the AI target
             * point ~14M world units east, producing target_angle=0xB38
             * (south-southwest) vs original 0x1D0 (north-northeast). The
             * resulting tiny delta (~10 vs ~1697) put the cascade in the
             * fine-sin band (~+1984) instead of the mid-band emergency snap
             * (+0x4000 = +16384), and the steering accumulator never
             * ratcheted up to the original's ~±20000 during countdown.
             * [Confirmed via tools/frida_pool10_004340C0.js + pool15_spline:
             *  original sim_tick=0 paused=1 first call has L=1697, span_idx=4.] */
            *(int16_t *)(actor + 0x080) = (int16_t)actor_span;
            *(int16_t *)(actor + 0x084) = (int16_t)actor_span;
            *(int16_t *)(actor + 0x086) = (int16_t)actor_span;
            /* Sub-lane clamp matching InitActorTrackSegmentPlacement @ 0x445F2A-32:
             *   if ((pbVar1[3] & 0xf) <= iVar7) { iVar7 = lane_nibble - 1;
             *     *(char *)(param_1 + 6) = (char)iVar7; }
             * Original writes the clamped value back into actor+0x8c so downstream
             * consumers (route-state lookups, lane-dependent rendering) see the
             * legal lane index. Port previously stored the raw value, leaving an
             * over-large lane id in the actor record on tracks where the spawn
             * table's lane (1, 2, 3) exceeds the actual span lane_count nibble.
             * [CONFIRMED @ 0x445F2A-32 disassembly pool14 session, pilot 0x00434350.] */
            {
                int lane_count = td5_track_span_lane_count_at(span_index);
                int clamped_sub_lane = sub_lane;
                if (lane_count > 0 && clamped_sub_lane >= lane_count)
                    clamped_sub_lane = lane_count - 1;
                if (clamped_sub_lane < 0)
                    clamped_sub_lane = 0;
                sub_lane = clamped_sub_lane;
            }
            actor[0x08C] = (uint8_t)sub_lane;
            TD5_LOG_I(LOG_TAG,
                      "Actor spawn span: slot=%d actor_span=%d world_span=%d sub_lane=%d",
                      slot, actor_span, span_index, sub_lane);

            if (!td5_track_get_span_lane_world(span_index, sub_lane, &world_x, &world_y, &world_z)) {
                /* Fallback: use span origin, shift to 24.8 FP to match the
                 * format returned by td5_track_get_span_lane_world. */
                world_x = sp->origin_x * 0x100;
                world_y = sp->origin_y * 0x100;
                world_z = sp->origin_z * 0x100;
            }

            /* Actor X/Z in 24.8 FP from lane vertex average.
             * td5_track_get_span_lane_world now returns 24.8 FP directly
             * (matching original FUN_00445f10 which returns origin*0x100 + sum*64).
             * Y: set to 0xC0000000 matching the original binary (0x405D70).
             * The physics reset below runs IntegratePose which should snap
             * Y to the ground surface via wheel contact averaging. */
            *(int32_t *)(actor + 0x1FC) = world_x;
            *(int32_t *)(actor + 0x200) = (int32_t)0xC0000000;
            *(int32_t *)(actor + 0x204) = world_z;

            actor[0x375] = (uint8_t)slot;
            /* Wheel emitter IDs (+0x371..+0x374): 0xFF = "no emitter acquired yet".
             * VFX acquisition check fires only when value == 0xFF; actors start
             * zeroed (memset above), so must be explicitly primed here. */
            memset(actor + 0x371, 0xFF, 4);
            /* track_contact_flag (+0x37B) intentionally NOT set here.
             * Original (0x434350) never touches it during init — it is a
             * per-frame wall-contact flag set by wall_response() and cleared
             * at the start of each physics tick. Starting at 0 is correct.
             * [CONFIRMED @ 0x405D70 / 0x434350 decompilation] */
            td5_track_compute_heading((TD5_Actor *)actor);

            /* [TD6 SPAWN HEADING FIX — track-scoped] TD6's in-span vertex layout
             * differs from TD5's, so the geometry yaw above lands ~90° off the
             * real travel direction: the human car faces sideways at the start
             * (e.g. London by the sidewalk) and AI cars trip the recovery gate
             * (spawn-yaw vs route-heading too large) and stall instead of
             * driving. Re-seed euler_accum.yaw from the synthesized LEFT.TRK
             * route heading (== travel direction) HERE — before
             * reset_actor_state below builds the rotation matrix — so the matrix
             * (hence render + initial motion) reflects the corrected heading.
             * Faithful TD5 tracks keep the geometry yaw byte-identically (the
             * "DO NOT post-process" note below still governs them).
             *
             * [NATIVE REVERSE CIRCUIT 2026-07-01] Reverse native circuits get the
             * SAME correction: the generated reverse strip's per-span vertex
             * layout yields a sheared/diagonal geometry yaw (verified: Newcastle
             * rev slot 0 span 512 geom 855 vs true travel ~772), so re-seed from
             * the reverse route-byte heading (LEFTB/RIGHTB.TRK), with the
             * centreline-tangent fallback. Forward faithful TD5 tracks are still
             * excluded and keep the byte-faithful geometry yaw. */
            if (g_active_td6_level > 0 || g_td5.reverse_direction)
                td5_ai_correct_spawn_heading(slot);

            /* DO NOT post-process the geometry-derived yaw.
             *
             * Original InitializeActorTrackPose @ 0x00434350 writes
             * actor[+0x1F4] = ((angle_from_geometry + 0x800) << 8) ONCE at
             * 0x0043450a and never re-writes it; InitializeRaceSession
             * @ 0x0042AA10 has no further +0x1F4 writes after the pose call.
             * [CONFIRMED via Ghidra disassembly + decompilation, /fix
             *  research session 2026-05-10.]
             *
             * A previous port version called td5_ai_correct_spawn_heading()
             * here to overwrite the geometry yaw with route_bytes[span*3+1]'s
             * encoded heading. The motivation cited was "geometry heading is
             * ~1050 angle units off from route heading → recovery script
             * fires every tick → throttle=0". A Frida calls-trace pass on
             * branch fix-1778389787-4116 (PlayerIsAI=1, Moscow) showed the
             * ORIGINAL also writes geometry-derived yaw at sim_tick=1 entry
             * of UpdateAIVehicleDynamics and drives normally, so the gap
             * between geometry yaw and route_byte yaw is real but tolerated
             * by the original's recovery threshold. The override was masking
             * a different bug (recovery-gate over-triggering on the port);
             * with the gate now matching the original (D1/D2/D3 in commit
             * 47bd157), the override is no longer needed and was the
             * dominant cause of the tick-1 yaw_accum divergence
             * (3 of 6 slots had STEERING SIGN FLIP — see
             *  tools/frida_csv/fix-1778389787-4116/calls_trace_diff_summary.md).
             */

            td5_physics_reset_actor_state((TD5_Actor *)actor);

            /* Restore span_raw to the spawn span after reset.
             * reset_actor_state calls integrate_pose which calls
             * td5_track_update_actor_position — this may overwrite the
             * chassis track_state[0] via update_position_recursive if the
             * actor crossed a boundary.
             *
             * Previously we also zeroed [1]/[2]/[3] here (claiming the
             * original post-init state was 0). Wide /diff-race 2026-04-11
             * showed the original emits span_norm=span_accum=span_high=111
             * at sim_tick=1 for a Viper on Moscow — i.e. these fields are
             * seeded to spawn_span by InitActorTrackSegmentPlacement
             * (0x00445F10) and NEVER zeroed. Restore only span_raw and
             * leave the other three at the values placed above. */
            *(int16_t *)(actor + 0x080) = (int16_t)span_index; /* span_raw   */

            /* Seed RS_TRACK_PROGRESS + RS_TRACK_OFFSET_BIAS from actual spawn
             * position [CONFIRMED @ RefreshActorTrackProgressOffset 0x004342E0].
             * Called after span_raw and position are finalised so each racer
             * gets a different lateral offset seed, enabling diverging lateral
             * positions during peer avoidance. */
            td5_ai_seed_actor_track_progress_offset(slot);
            TD5_LOG_I(LOG_TAG, "Actor bias seed: slot=%d bias=%d progress=%d",
                      slot, td5_ai_get_route_state(slot)[9],
                      td5_ai_get_route_state(slot)[0x19]);


            TD5_LOG_I(LOG_TAG,
                      "Actor spawn: slot=%d span=%d pos=(%d,%d,%d) state=%d lane=%d",
                      slot, span_index,
                      *(int32_t *)(actor + 0x1FC),
                      *(int32_t *)(actor + 0x200),
                      *(int32_t *)(actor + 0x204),
                      s_slot_state[slot].state,
                      sub_lane);
        }

        /* race_position (+0x383) stays 0 at init — original leaves it zero until
         * UpdateRaceOrder @ 0x0042F5B0 writes it at sim_tick>=1 [CONFIRMED via
         * research: only store to +0x383 is the indexed write in UpdateRaceOrder,
         * preceded by the 0xE2-dword memset in InitializeRaceVehicleRuntime]. */

        TD5_LOG_I(LOG_TAG, "InitRace step 11/19: actors spawned and runtime bound count=%d",
                  spawn_count);

        TD5_LOG_I(LOG_TAG, "Actors spawned: base=%p count=%d racers=%d",
                  (void *)s_actor_memory, g_td5.total_actor_count, racer_count);
    }

    /* ---- Step 12: Open input recording/playback ---- */
    if (s_replay_mode) {
        td5_input_read_open("replay.td5");
        td5_input_set_playback_active(1);
    } else {
        td5_input_write_open("replay.td5");
        td5_input_set_playback_active(0);
    }
    TD5_LOG_I(LOG_TAG, "InitRace step 12/19: input %s initialized",
              s_replay_mode ? "playback" : "recording");

    /* [REPLAY FIX #18 2026-06-15] Replay correctness guard + diagnostic.
     * A View Replay must reproduce the just-driven race bit-for-bit. The two
     * deterministic anchors are restored above — the per-race RNG seed (step 0,
     * s_saved_race_seed) and the recorded StartSpanOffset (the spawn grid,
     * TD5RE_REPLAY_OFFSET_FIX). Re-assert here that playback is actually armed
     * and surface the restored anchors in one line so a divergent replay can be
     * triaged from the log ("seed/offset round-tripped?" vs "sim/camera RNG").
     *
     * KNOWN RESIDUAL DIVERGENCE (NOT fixable from td5_game.c — see report):
     *   1) td5_input.c: td5_input_reset_accumulators() does NOT clear
     *      s_steering_cmd[]/s_steer_ramp[], so a replay inherits the recorded
     *      race's END-of-run steering accumulator instead of starting clean —
     *      the first fraction of a second of steering diverges. Fix: also zero
     *      those two arrays per race in td5_input_reset_accumulators().
     *   2) The cinematic trackside camera (td5_camera.c g_tracksideTimer =
     *      rand()%10000) + audio/particle spawns (td5_sound.c/td5_vfx.c) draw the
     *      SHARED CRT rand() at RENDER rate, whose draw count differs between the
     *      recorded run and the replay (different fps) — so the camera cuts to
     *      different shots and the replay "looks different" even though the CAR
     *      path is faithful. The SIM path itself is rand()-clean (integer
     *      physics; AI uses a private per-slot stream off the replicated seed),
     *      so the car trajectory IS reproduced. Fix: seed the trackside camera
     *      timer from td5_game_get_race_seed() instead of the shared rand().
     * Gated by TD5RE_REPLAY_FIX (cached, default ON): "0" silences the guard
     * (no behavioural change either way — this block only logs + re-arms). */
    if (s_replay_mode) {
        static int s_replay_fix_init = 0;
        static int s_replay_fix_enabled = 1;
        if (!s_replay_fix_init) {
            s_replay_fix_enabled = td5_env_flag_on("TD5RE_REPLAY_FIX");  /* default ON */
            s_replay_fix_init = 1;
            TD5_LOG_I(LOG_TAG, "Replay correctness guard: %s",
                      s_replay_fix_enabled ? "enabled" : "disabled");
        }
        if (s_replay_fix_enabled) {
            /* Belt-and-suspenders: make sure the playback path is live even if a
             * caller flipped only the game-side flag. Without this the sim would
             * run the recording path and the played-back car would sit idle. */
            if (!td5_input_is_playback_active()) {
                td5_input_set_playback_active(1);
                TD5_LOG_W(LOG_TAG,
                          "Replay: playback was NOT armed at init -> forced ON");
            }
            TD5_LOG_I(LOG_TAG,
                      "Replay init: seed=0x%08X recorded_span_offset=%d "
                      "track=%d car=%d reverse=%d (deterministic anchors restored; "
                      "any remaining drift = input-accumulator/camera-RNG, see code note)",
                      s_saved_race_seed,
                      (int)td5_input_replay_start_span_offset(),
                      g_td5.track_index, g_td5.car_index,
                      g_td5.reverse_direction);
        }
    }

    CK("ck13_before_ambient");
    /* ---- Step 13: Load ambient sounds ---- */
    td5_sound_load_ambient();
    TD5_LOG_I(LOG_TAG, "InitRace step 13/19: ambient sounds loaded");
    CK("ck13_after_ambient");

    /* ---- Step 14: Initialize particles, smoke, tire tracks, weather ---- */
    td5_vfx_init();
    /* Tier 1 port — cache PoliceLt_red/blue + Police_red/blue atlas UVs
     * and reset marker phase counters. Mirrors orig
     * InitializeTrackedActorMarkerBillboards @ 0x0043c9e0 callsite inside
     * InitializeRaceSession (between LoadRaceAmbientSoundBuffers and
     * InitializeRaceParticleSystem). */
    td5_vfx_init_tracked_actor_marker_billboards();
    TD5_LOG_I(LOG_TAG, "InitRace step 14/19: VFX systems initialized");

    /* ---- Step 14a: Initialize weather overlay particles ----
     * Orig InitializeWeatherOverlayParticles @ 0x00446240, called from
     * InitializeRaceSession @ 0x0042b2ec — after the particle/tire/marker init
     * and immediately before the 2nd traffic fill @ 0x42b2f4 (this position).
     * The weather type is the int32 at LEVELINF.DAT +0x28: 0=rain, 1=snow,
     * 2(or >=2)=none [CONFIRMED @ 0x00446245 MOV EAX,[EAX+0x28] + branch logic
     * @ 0x00446240]. The raw on-disk encoding matches TD5_WeatherType exactly,
     * so it passes straight through. Snow is a cut feature (init seeds positions
     * but the render path is gated off — faithful). NULL config (e.g. TD6 ported
     * tracks with no LEVELINF) -> clear, no weather. */
    {
        TD5_WeatherType weather = TD5_WEATHER_CLEAR;
        if (g_track_environment_config) {
            int32_t wt;
            memcpy(&wt, g_track_environment_config + 0x28, sizeof(int32_t));
            weather = (TD5_WeatherType)wt;
        }
        g_td5.weather = weather;
        td5_vfx_init_weather(weather);
        TD5_LOG_I(LOG_TAG, "InitRace step 14a: weather init type=%d (0=rain,1=snow,2=none)",
                  (int)weather);
    }

    /* ---- Step 14b: SECOND traffic fill ----
     * The orig calls InitializeTrafficActorsFromQueue @ 0x00435940 TWICE
     * during race setup [CONFIRMED @ 0x0042aa10 InitializeRaceSession +
     * 0x00432e60 InitializeRaceActorRuntime — Ghidra function_callers]:
     *   1st: inside InitializeRaceActorRuntime (port: td5_ai_init_race_actor_
     *        runtime @ line 1637) — fills slots 6-11 from queue records 0-5
     *        and advances the queue cursor 6 records.
     *   2nd: directly in InitializeRaceSession, right after
     *        InitializeWeatherOverlayParticles and before
     *        ConfigureForceFeedbackControllers (this position) — re-fills the
     *        SAME slots from queue records 6-11, so the in-race starting
     *        traffic sits ~125 spans further down-track.
     * The port previously only did the 1st fill, leaving traffic at records
     * 0-5 (~125 spans too close to the player → "traffic spawns/encounters
     * happen earlier than the original"). The function self-gates on
     * g_racerCount <= 6, so a bare call is inert when traffic is disabled. */
    td5_ai_init_traffic_actors();

    /* [MP COP CHASE AI COP 2026-06-23] Arm the AI cop HERE — AFTER
     * td5_ai_init_traffic_actors(), whose dynamic-traffic seeding calls
     * cop_state_reset() and would wipe an earlier arming. Without this the AI cop
     * slot (= num_human_players) reverted to a regular AI racer, so the cop role
     * visibly fell back onto a human and the AI just drove as another player. */
    if (g_td5.mp_mode_config.mode == TD5_MP_MODE_COP_CHASE && !g_td5.network_active) {
        if (g_td5.mp_mode_config.cop_is_ai)
            td5_ai_cop_chase_setup(td5_game_cop_chase_cop_slot(), 1);
        /* [MP AI TEST PLAYERS 2026-06-25] Simulated AI players assigned the COP role
         * drive as cops too. Arm EVERY AI-driven slot whose cop_slot_mask bit is
         * set (not just the primary), so a multi-cop AI field all runs cop_drive. */
        if (g_td5.mp_ai_player_mask) {
            int humans = g_td5.num_human_players;
            if (humans > TD5_MAX_RACER_SLOTS) humans = TD5_MAX_RACER_SLOTS;
            for (int i = 0; i < humans; i++)
                if (((g_td5.mp_ai_player_mask >> i) & 1u) && td5_game_cop_chase_is_cop(i))
                    td5_ai_cop_chase_setup(i, 1);
        }
    }

    /* ---- Step 15: Configure force feedback + input mapping ---- */
    /* [PORT ENHANCEMENT] N-way split: one input slot per local human. Players
     * 2..N-1 default to joystick index = player (the per-player device picker
     * is a deferred frontend step). Players 0-1 keep their configured devices. */
    {
        int humans = (g_td5.split_screen_mode > 0) ? g_td5.num_human_players : 1;
        if (humans < 1) humans = 1;
        if (humans > TD5_MAX_HUMAN_PLAYERS) humans = TD5_MAX_HUMAN_PLAYERS;
        td5_input_set_active_players(humans);
        for (int p = 2; p < humans; p++) {
            if (td5_input_get_input_source(p) == 0)
                td5_input_set_input_source(p, p);  /* default: joystick #p */
        }
    }
    /* Resolve each player's input device (keyboard / joystick 1 / joystick 2)
     * from the INI override or Config.td5 and create the DirectInput devices +
     * push joystick bindings, BEFORE FF init (which binds slot 0's device). */
    td5_input_apply_device_selection();
    td5_input_ff_init();
    td5_input_reset_accumulators();
    td5_input_reset_buffers();
    TD5_LOG_I(LOG_TAG, "InitRace step 15/19: force feedback and input buffers initialized (players=%d)",
              g_td5.split_screen_mode > 0 ? 2 : 1);

    /* ---- Step 16: Start CD audio track ---- */
    td5_sound_cd_play(g_td5.track_index % 10 + 1);
    TD5_LOG_I(LOG_TAG, "InitRace step 16/19: CD audio started track=%d",
              g_td5.track_index % 10 + 1);

    CK("ck17_before_viewport");
    /* ---- Step 17: Initialize 3D render state + viewport layout ---- */
    td5_render_reset_texture_cache();
    td5_game_init_viewport_layout();
    /* [PORT ENHANCEMENT] Each viewport follows its own local player slot
     * (viewport vp -> racer slot vp). Humans occupy slots 0..viewport_count-1
     * so every pane tracks a distinct human. Legacy 1/2-view behaviour kept
     * (vp0->slot0; vp1->slot1 when a 2nd actor exists). */
    for (int vp = 0; vp < TD5_MAX_VIEWPORTS; vp++) {
        int slot = (vp < g_td5.viewport_count && vp < g_td5.total_actor_count) ? vp : 0;
        /* [#6 MP POSITION SELECT 2026-06-15] If the local-MP position picker
         * assigned cells, viewport vp (= grid cell vp, row-major) shows the actor
         * of the player who chose that cell. The accessor returns -1 unless the
         * positions feature is active (knob on + committed + the local MP flow),
         * so AutoRace / the harness / non-positioned MP keep the identity map and
         * stay byte-unchanged. Frontend/render only — the deterministic sim and
         * net path are untouched (this just repoints which camera each pane uses). */
        if (vp < g_td5.viewport_count) {
            /* [#9 2026-06-15] Look up the actor by the pane's grid CELL, not the
             * raw viewport index: the position screen permutes panes across cells,
             * so viewport vp renders cell td5_game_get_pane_cell(vp), and that is
             * the cell whose chosen player must drive this pane's camera. With the
             * layout fix off / no permutation, get_pane_cell returns vp, so this is
             * byte-identical to the old identity map. */
            int mapped = td5_frontend_mp_view_actor_slot(
                             td5_game_get_pane_cell(g_td5.viewport_count, vp));
            if (mapped >= 0 && mapped < g_td5.total_actor_count)
                slot = mapped;
        }
        g_actorSlotForView[vp] = slot;
        g_actor_slot_map[vp]   = slot;
    }
    /* [S31 net] One local viewport that follows THIS machine's car: the slot
     * model keeps every net player as a racer slot, but each machine renders
     * a single full-screen view pinned to its own net slot (the client's car
     * is slot 1+, which the identity map above would never show). */
    if (g_td5.network_active) {
        int local = td5_net_local_slot();
        if (local < 0 || local >= g_td5.total_actor_count) local = 0;
        g_actorSlotForView[0] = local;
        g_actor_slot_map[0]   = local;
        TD5_LOG_I(LOG_TAG, "InitRace step 17: net view follows local slot %d", local);
    }
    TD5_LOG_I(LOG_TAG, "InitRace step 17/19: render state and viewport layout initialized views=%d",
              g_td5.viewport_count);
    /* [SPLIT-SCREEN DEVICE BLEED 2026-06-30] Now that devices (step 15) AND the
     * viewport->actor map (above) are set, dump the per-slot local input map and
     * warn if two human slots read the same physical controller — the signature
     * of one pad driving several panes' cameras. */
    td5_input_log_race_input_map();
    CK("ck17_after_viewport");

    /* ---- Step 18: Upload race texture pages to GPU ---- */
    td5_asset_load_race_texture_pages();
    td5_render_load_environs_textures(td5_asset_level_number(g_td5.track_index));
    TD5_LOG_I(LOG_TAG, "InitRace step 18/19: race texture pages + environs uploaded");

    /* ---- Step 19: Initialize HUD, pause menu overlay ---- */
    #define DBG_WRITE(msg) TD5_LOG_I(LOG_TAG, "Step19: %s", msg)
    DBG_WRITE("19a_before_overlay");
    /* race_mode arg mirrors orig InitializeRaceOverlayResources param_1:
     * 0 = replay/benchmark (suppressed HUD; the replay sub-path lights the
     * 0x80000000 REPLAY banner), 1 = normal race AND attract demo (full HUD).
     * [CONFIRMED orig caller @0x42B4xx: (g_inputPlaybackActive==0 && !benchmark)?1:0]
     * Demo is param_1=1 because the original clears g_inputPlaybackActive for
     * demo; the "DEMO MODE" text comes from DrawRaceStatusText, not this bitmask. */
    td5_hud_init_overlay_resources(
        (s_replay_mode || g_td5.benchmark_active) ? 0 : 1, 0);
    DBG_WRITE("19b_before_layout");
    td5_hud_init_layout();
    DBG_WRITE("19c_before_minimap");
    td5_hud_init_minimap_layout();
    DBG_WRITE("19d_before_font");
    td5_hud_init_font_atlas();
    DBG_WRITE("19e_before_pause");
    td5_hud_init_pause_menu(0);
    DBG_WRITE("19f_complete");
    #undef DBG_WRITE
    td5_camera_set_preset(0);
    TD5_LOG_I(LOG_TAG, "InitRace step 19/19: HUD and pause menu initialized");

    /* ---- Load sky texture ---- */
    {
        char sky_path[256];
        int level_num = td5_asset_level_number(g_td5.track_index);
        /* [reverse-sky 2026-06-16] Backwards races load the level's reverse-
         * direction sky (BACKSKY) instead of FORWSKY. Forward-only tracks ship
         * no BACKSKY, so probe for it and fall back to FORWSKY when absent —
         * td5_render_load_sky no-ops on a missing file (leaving no sky), which we
         * must avoid for a reverse race. */
        const char *sky_name = "FORWSKY";
        if (g_td5.reverse_direction) {
            char probe[256];
            snprintf(probe, sizeof(probe),
                     "re/assets/levels/level%03d/BACKSKY.png", level_num);
            FILE *bf = fopen(probe, "rb");
            if (bf) { fclose(bf); sky_name = "BACKSKY"; }
        }
        snprintf(sky_path, sizeof(sky_path),
                 "re/assets/levels/level%03d/%s.png", level_num, sky_name);
        td5_render_load_sky(sky_path);
    }

    /* ---- Step 20: Load checkpoint timing from hardcoded table (0x46CBB0) ---- */
    {
        int tidx = g_td5.track_index;
        int record_idx = -1;
        memset(&s_active_checkpoint, 0, sizeof(s_active_checkpoint));
        if (g_td5.game_type == TD5_GAMETYPE_DRAG_RACE) {
            /* Drag race: original LoadTrackRuntimeData(pool=30) reads the
             * checkpoint record pointer at 0x0046cfe4, which points to
             * 0x0046ce68 = {count=1, initial_time=30779, threshold=204,
             * bonus=0, ...}. The schedule→record map only covers UI slots
             * 0..18; drag's track_index=19 falls past the table, so it
             * needs an explicit branch. k_checkpoint_table[30] (and [29])
             * already mirror that exact row.
             * [CONFIRMED @ LoadTrackRuntimeData 0x0042FB90 +
             *  memory_read(0x0046ce68) {01 00 3B 78 CC 00 00 00 ...}] */
            record_idx = 30;
            TD5_LOG_I(LOG_TAG, "Drag race: forcing checkpoint record_idx=30 (track_index=%d)", tidx);
        } else if (tidx >= 0 && tidx < (int)(sizeof(k_schedule_to_checkpoint_index) /
                                      sizeof(k_schedule_to_checkpoint_index[0]))) {
            record_idx = k_schedule_to_checkpoint_index[tidx];

            /* Reverse-direction races use a DIFFERENT checkpoint record. The
             * original derives the record via the reverse pool-count table
             * (gTrackPoolReverseSpanCountTable @0x466E3C) so reverse gets its
             * own span thresholds / initial_time / time bonuses — without this,
             * reverse compared the player's reverse-walker span against FORWARD
             * checkpoint spans, firing each checkpoint ~10-25 spans before the
             * painted banner and applying the wrong (too-short) timer.
             * [CONFIRMED via runtime dump of the original: reverse Sydney =
             *  record 18 {665,1081,1679,2250,2635} init=15419, vs forward
             *  record 13 {486,1057,1655,2071,2658} init=10299. 1081 == the
             *  stage-2 banner span.] Falls back to the forward record when the
             * track ships no reverse data (resolver returns -1). */
            if (g_td5.reverse_direction) {
                int rev_rec = td5_asset_resolve_checkpoint_record_index(tidx, 1);
                if (rev_rec >= 0) {
                    TD5_LOG_I(LOG_TAG,
                              "Reverse checkpoint record: track=%d fwd_record=%d -> rev_record=%d",
                              tidx, record_idx, rev_rec);
                    record_idx = rev_rec;
                } else {
                    TD5_LOG_W(LOG_TAG,
                              "Reverse requested but no reverse checkpoint record for track=%d; using forward record=%d",
                              tidx, record_idx);
                }
            }
        }
        if (record_idx >= 0 && record_idx < 40) {
            memcpy(&s_active_checkpoint, k_checkpoint_table[record_idx], 24);
            TD5_LOG_I(LOG_TAG, "Checkpoint record loaded: track=%d record=%d count=%d initial_time=%u",
                      tidx, record_idx, (int)s_active_checkpoint.checkpoint_count,
                      (unsigned)s_active_checkpoint.initial_time);
            for (int ci = 0; ci < (int)s_active_checkpoint.checkpoint_count && ci < 5; ci++) {
                TD5_LOG_I(LOG_TAG, "  checkpoint[%d]: span_threshold=%u time_bonus=%u",
                          ci,
                          (unsigned)s_active_checkpoint.checkpoints[ci].span_threshold,
                          (unsigned)s_active_checkpoint.checkpoints[ci].time_bonus);
            }

            /* [DRAG DYNAMIC LENGTH 2026-06-29] Place the finish for the chosen
             * LENGTH option. The drag strip is uniform; SHORT/MEDIUM/LONG move the
             * finish within the base strip, while EPIC has already PHYSICALLY
             * repeated the strip (td5_track_drag_apply_length) so the ring is now
             * ~2x — the finish lands deep in the second copy. base_ring backs out
             * the original (pre-repeat) length so the fractions stay stable. */
            if (g_td5.drag_race_enabled && s_active_checkpoint.checkpoint_count > 0) {
                int last        = (int)s_active_checkpoint.checkpoint_count - 1;
                int base_finish = (int)s_active_checkpoint.checkpoints[last].span_threshold;
                int start       = 115;   /* drag spawn span (see drag spawn override) */
                int ring        = g_td5.track_span_ring_length;
                int inserted    = td5_track_drag_finish_span();   /* >=0 if clean spans inserted */
                int new_finish;
                int cap         = (ring > 4) ? (ring - 2) : base_finish;
                if (inserted >= 0) {
                    /* EPIC: finish on the inserted clean road (the lengthener
                     * already placed it before the shifted wall). */
                    new_finish = inserted;
                } else {
                    /* SHORT/MEDIUM/LONG: move the finish within the existing strip. */
                    new_finish = td5_game_drag_length_finish_span(start, ring);
                }
                if (new_finish > cap) new_finish = cap;
                if (new_finish < start + 8) new_finish = start + 8;
                s_active_checkpoint.checkpoints[last].span_threshold = (uint16_t)new_finish;
                TD5_LOG_I(LOG_TAG,
                          "Drag length: finish span %d -> %d (start=%d ring=%d inserted=%d cap=%d)",
                          base_finish, new_finish, start, ring, inserted, cap);
            }
        } else {
            TD5_LOG_W(LOG_TAG, "Track index %d out of range, no checkpoint data", tidx);
        }
    }

    /* ---- Step 20b: Read remaining LEVELINF.DAT fields ----
     *
     * LEVELINF.DAT layout (100 bytes, loaded into g_track_environment_config):
     *
     *   +0x00 int32  circuit_flag         (0=P2P, 1=circuit)           [READ by td5_asset.c]
     *   +0x04 int32  smoke_enable         (0/1)                        [READ by td5_vfx.c]
     *   +0x08 int32  checkpoint_config    (checkpoint count, 0=disable) [READ below]
     *   +0x0C int32  checkpoint_span_0    (start/traffic span)          [READ below]
     *   +0x10 int32  checkpoint_span_1                                  [READ below]
     *   +0x14 int32  checkpoint_span_2                                  [READ below]
     *   +0x18 int32  checkpoint_span_3                                  [READ below]
     *   +0x1C int32  checkpoint_span_4                                  [READ below]
     *   +0x20 int32  checkpoint_span_5                                  [READ below]
     *   +0x24 int32  checkpoint_span_6    (usually zero)                [READ below]
     *   +0x28 int32  weather_type         (0=rain,1=snow,2=none)        [READ by td5_vfx.c]
     *   +0x2C int32  density_pair_count                                 [READ by td5_vfx.c]
     *   +0x30 int32  special_encounters   (1=enable, 0=disable)         [READ below]
     *   +0x34 ...    density_pairs        (int16 seg, int16 density)×N  [READ by td5_vfx.c]
     *   +0x54 int32  track_subvariant     (36=race, -1=cup)             [READ below]
     *   +0x58 int32  span_count           (ring length, redundant)      [READ below]
     *   +0x5C int32  fog_enable           (0/1)                         [READ in step 4a]
     *   +0x60 byte   fog_r                                              [READ in step 4a]
     *   +0x61 byte   fog_g                                              [READ in step 4a]
     *   +0x62 byte   fog_b                                              [READ in step 4a]
     *   +0x63 byte   padding              (always 0)
     */
    {
        s_levelinf_checkpoint_config = 0;
        memset(s_levelinf_checkpoint_spans, 0, sizeof(s_levelinf_checkpoint_spans));
        s_levelinf_track_subvariant = 0;
        s_levelinf_span_count = 0;

        if (g_track_environment_config) {
            /* +0x08: checkpoint system config (0 = disabled) — assembly at 0x40a04d */
            memcpy(&s_levelinf_checkpoint_config, g_track_environment_config + 0x08, sizeof(int32_t));
            TD5_LOG_I(LOG_TAG, "LEVELINF checkpoint config (+0x08) = %d", s_levelinf_checkpoint_config);

            if (s_levelinf_checkpoint_config == 0) {
                TD5_LOG_I(LOG_TAG, "LEVELINF checkpoint config is 0 — disabling checkpoint system");
                s_active_checkpoint.checkpoint_count = 0;
            }

            /* +0x0C..+0x24: checkpoint span indices (7 x int32) */
            for (int i = 0; i < 7; i++) {
                memcpy(&s_levelinf_checkpoint_spans[i],
                       g_track_environment_config + 0x0C + i * 4, sizeof(int32_t));
            }
            TD5_LOG_I(LOG_TAG, "LEVELINF checkpoint spans: %d %d %d %d %d %d %d",
                      s_levelinf_checkpoint_spans[0], s_levelinf_checkpoint_spans[1],
                      s_levelinf_checkpoint_spans[2], s_levelinf_checkpoint_spans[3],
                      s_levelinf_checkpoint_spans[4], s_levelinf_checkpoint_spans[5],
                      s_levelinf_checkpoint_spans[6]);

            /* +0x30 is the per-level TRAFFIC enable flag (not special
             * encounters, as the port previously assumed). It is consumed
             * in Step 4a above, which ports 0x0042AE7A–0x0042AE80. No
             * decompiled writer of gSpecialEncounterEnabled is gated on
             * +0x30 — the original uses g_specialEncounterType (set to 0
             * when LEVELINF+0x00==1 at 0x0042AE6C) for the circuit case,
             * not the enable flag. */

            /* +0x54: track subvariant (36=race, -1=cup) */
            memcpy(&s_levelinf_track_subvariant, g_track_environment_config + 0x54, sizeof(int32_t));
            /* +0x58: span count (ring length, redundant with STRIP.DAT) */
            memcpy(&s_levelinf_span_count, g_track_environment_config + 0x58, sizeof(int32_t));

            TD5_LOG_I(LOG_TAG, "LEVELINF +0x54 track_subvariant=%d +0x58 span_count=%d",
                      (int)s_levelinf_track_subvariant, (int)s_levelinf_span_count);

            /* Cross-check span count against STRIP.DAT */
            if (g_td5.track_span_ring_length != 0 &&
                g_td5.track_span_ring_length != s_levelinf_span_count) {
                TD5_LOG_I(LOG_TAG, "NOTE: LEVELINF span_count=%d vs STRIP.DAT ring_length=%d",
                          (int)s_levelinf_span_count, (int)g_td5.track_span_ring_length);
            }
        }
    }

    /* [#R3-7 2026-06-19] TD6 P2P checkpoint timer. TD6.exe shipped NONE for these
     * city tracks (RE-confirmed), so synthesize the TD5-format record from the
     * already-extracted TD6 checkpoint spans (s_td6_cp_spans) and force the gates
     * on. Placed AFTER the LEVELINF read (which zeroes the record when +0x08==0)
     * and BEFORE Step 21 (which seeds the timer). Values are tunable invented
     * defaults (packed hi-byte = whole seconds; lo 0x3B matches the originals). */
    int td6_cp_active = 0;
    if (g_active_td6_level > 0 && s_td6_cp_count > 0 && td6_cp_timer_enabled()) {
        int cp_secs    = td6_cp_time_secs();
        int bonus_secs = td6_cp_bonus_secs();
        int n = s_td6_cp_count; if (n > 5) n = 5;
        memset(&s_active_checkpoint, 0, sizeof(s_active_checkpoint));
        s_active_checkpoint.checkpoint_count = (uint16_t)n;
        s_active_checkpoint.initial_time     = (uint16_t)(((cp_secs & 0xFF) << 8) | 0x3B);
        /* [FIX checkpoint-timer 2026-06-23] On a TD6 P2P track the FINISH is a
         * SEPARATE span (s_td6_finish_span), so every synthesized banner is an
         * intermediate checkpoint that must grant time (the user reported "passing
         * a checkpoint on London doesn't add more time"). Only on a TD6 circuit
         * (no separate finish span) is the last cp the lap/finish line and so
         * bonus-less. Previously the last cp was always treated as the bonus-less
         * finish, which on London's separate-finish P2P silently dropped the 4th
         * banner's bonus even before the crossing-side bug below. */
        int last_cp_is_finish = (s_td6_finish_span <= 0);
        for (int ci = 0; ci < n; ci++) {
            s_active_checkpoint.checkpoints[ci].span_threshold = (uint16_t)s_td6_cp_spans[ci];
            s_active_checkpoint.checkpoints[ci].time_bonus =
                (last_cp_is_finish && ci == n - 1)
                    ? 0
                    : (uint16_t)((bonus_secs & 0xFF) << 8);
        }
        s_levelinf_checkpoint_config = 1;   /* enable the per-tick checkpoint watch (gate @ 6746) */
        g_special_encounter          = 1;   /* enable the timer decrement + the HUD digit */
        td6_cp_active = 1;
        TD5_LOG_I(LOG_TAG,
            "TD6 checkpoint timer synthesized: level=%d count=%d initial=%ds bonus=%ds/cp",
            g_active_td6_level, n, cp_secs, bonus_secs);
    }

    /* ---- Step 21: Adjust checkpoint timers by difficulty ---- */
    /* Original AdjustCheckpointTimersByDifficulty @ 0x0040A530 is called
     * per-slot from InitializeRaceVehicleRuntime @ 0x0042F140. The scaling
     * branch fires only for slot 0 (+0x375==0) so the table is mutated
     * exactly once; then actor+0x344 = *(ptr+2) executes for ALL slots
     * (scaling is not gated on player/AI). Port matches by calling
     * adjust_checkpoint_timers for every slot — the scaling itself is
     * idempotent because it reads from s_active_checkpoint.initial_time
     * which stays at the raw value. */
    for (int i = 0; i < TD5_MAX_RACER_SLOTS; i++) {
        if (s_slot_state[i].state == 3) continue;   /* disabled slot */
        if ((g_td5.checkpoint_timers_enabled || td6_cp_active) &&
            g_td5.track_type != TD5_TRACK_CIRCUIT) {
            adjust_checkpoint_timers(i);
        } else {
            s_metrics[i].timer_ticks = 0x7FFF;  /* disable: max timer */
        }
        /* Mirror m->timer_ticks to actor+0x344 immediately — original
         * AdjustCheckpointTimersByDifficulty @ 0x0040A530 writes directly
         * to actor+0x344 during init, not at end of first sub-tick. */
        {
            TD5_Actor *ai = td5_game_get_actor(i);
            if (ai) {
                ai->pending_finish_timer =
                    (s_metrics[i].timer_ticks > 0)
                        ? (uint16_t)s_metrics[i].timer_ticks
                        : 0;
            }
        }
    }

    /* ---- Reset race state ---- */
    g_td5.race_end_fade_state = 0;
    s_race_end_radial_pulse_enabled = 0;  /* orig writes [0x4aaefc] @ 0x0042aaff */
    s_pause_exit_pending = 0;
    s_pause_restart_pending = 0;   /* [REWORK 2026-06-05/S15] clear stale RESTART latch */
    s_pause_endrace_confirm = 0;   /* [END RACE NOW 2026-06-30] clear stale force-finish confirm */
    s_pause_action_confirm = 0;    /* [PAUSE CONFIRM 2026-07-02] clear stale RESTART/QUIT/EXIT confirm */
    g_td5.paused = 1;              /* start paused for countdown */
    /* DAT_00483030 (xz_freeze) has 1 read / 0 writes in the original binary.
     * The port previously set a software flag here to gate XZ integration,
     * but the original leaves position integration active during pause and
     * relies on vel_x/vel_z staying 0 because dynamics is skipped. Dead
     * g_xz_freeze / td5_physics_set_xz_freeze kept around but no longer
     * driven — slated for cleanup. */
    s_pause_menu_active = 0;       /* clear stale pause menu from previous race */
    /* [S27] Clear controller-disconnect state for the new race. */
    s_disconnect_pause_active  = 0;
    s_player_disconnected_mask = 0;
#ifndef TD5RE_RELEASE
    s_sim_loss_mask          = 0;
    s_sim_loss_race_start_ms = 0;
#endif
    s_prev_esc_state = 1;          /* suppress false ESC edge on first frame */
    s_prev_pause_act = 1;          /* suppress false PAUSE-action edge on first frame */
    s_prev_esc_frame = 1;          /* [BUG 5a] suppress false per-frame cinematic-ESC edge on first frame */
    s_replay_esc_exit = 0;         /* [BUG 5a] no pending cinematic-ESC exit at race start */
    g_td5.sim_tick_budget = 0.0f;
    g_td5.sim_time_accumulator = 0;
    g_td5.simulation_tick_counter = 0;
    g_td5.frame_prev_timestamp = td5_plat_time_ms();
    s_fade_accumulator = 0.0f;
    s_post_finish_cooldown = 0;
    s_race_end_timer_start = 0;
    s_victory_hold_start = 0;
    s_finish_position_display = 0;

    /* Reset results table */
    reset_results_table();

    /* [CAR DAMAGE 2026-06-28] Initialize per-actor health + clear deformation for
     * this race (ResetVehicleActorState does NOT touch the damage padding, so this
     * is the only initializer). Inert when [Game] CarDamage=0. Runs after actors
     * are spawned so td5_game_get_actor() returns valid pointers. */
    td5_damage_reset_race();

    /* Notify sound that race is starting */
    td5_sound_set_race_end(0);

    reset_race_countdown();

    /* Seed camera position from the spawned player actor.
     * sim_time_accumulator starts at 0 so no sim ticks fire for the first
     * few frames — without this, g_cameraPos stays at (0,0,0) and the
     * renderer draws from the world origin (geometry invisible).
     * [FIX: camera at origin for first ~4 frames of race] */
    td5_camera_tick();
    TD5_LOG_I(LOG_TAG, "Camera seeded: pos=(%.1f, %.1f, %.1f)",
              g_cameraPos[0], g_cameraPos[1], g_cameraPos[2]);

    /* Seed prev_world_pos so the first sub-tick render interpolation pass
     * (before any sim tick has fired) lerps current->current = no jump. */
    td5_physics_seed_prev_world_pos();

    /* [CONFIRMED @ 0x00428D20 InitializeBenchmarkFrameRateCapture]:
     * orig InitializeRaceSession calls this once at race start to
     * reset the per-frame sample buffer.  Port mirrors the cadence;
     * the capture is a no-op when benchmark_active is 0 so non-
     * benchmark races pay zero cost. */
    if (g_td5.benchmark_active) {
        td5_benchmark_init_capture();
    }

    TD5_LOG_I(LOG_TAG, "InitializeRaceSession: complete (%d actors)",
              g_td5.total_actor_count);

    /* [PORT 2026-06] Arm the first-race controller-tutorial overlay. Self-gated:
     * no-op unless enabled by config and the race is a normal local human race
     * (never network — would desync lockstep — nor cinematic/AI). While active it
     * holds the countdown (see the tick_race_countdown gates below). */
    td5_tutorial_begin_race();

    return 1;
}

/* ========================================================================
 * Race Trace Helper
 *
 * Snapshots frame / actor / view state and writes CSV rows via td5_trace.
 * Called at stage boundaries inside the race frame loop.
 * ======================================================================== */

static void td5_game_trace_stage_impl(const char *stage, unsigned int stage_bit,
                                      int ticks_this_frame)
{
    uint32_t frame = g_tick_counter;
    uint32_t sim_tick = (uint32_t)g_td5.simulation_tick_counter;

    /* Sim-tick cap — shuts the trace down AND requests a clean quit once the
     * target simulated window has been captured. Without the quit_requested
     * kick, the game keeps fast-forwarding after the trace file closes — the
     * sim loop and renderer keep going until the diff-race orchestrator's
     * CSV-stability poll fires, and with fast_forward=4 that's many extra
     * in-game seconds past the cap (the user sees the in-game timer overshoot
     * 15s and keep climbing to 1:21 etc.). Setting quit_requested makes
     * td5_game_run_race_frame exit on the next tick. */
    if (g_td5.ini.race_trace_max_sim_ticks > 0 &&
        (int)sim_tick > g_td5.ini.race_trace_max_sim_ticks) {
        static int s_trace_cap_logged = 0;
        if (!s_trace_cap_logged) {
            TD5_LOG_I(LOG_TAG,
                      "Race trace sim_tick cap reached (%d), requesting quit",
                      g_td5.ini.race_trace_max_sim_ticks);
            s_trace_cap_logged = 1;
        }
        td5_trace_shutdown();
        g_td5.quit_requested = 1;
        return;
    }

    /* MaxSpan cap: exit when slot 0's span_normalized reaches the threshold.
     * Used for per-slot AI benchmarking — see --SoloAISlot=N + --MaxSpan=N. */
    if (g_td5.ini.max_span > 0) {
        TD5_Actor *a0 = td5_game_get_actor(0);
        if (a0) {
            int16_t span_norm = *(int16_t *)((uint8_t *)a0 + 0x82);
            if (span_norm >= g_td5.ini.max_span) {
                static int s_max_span_logged = 0;
                if (!s_max_span_logged) {
                    TD5_LOG_I(LOG_TAG,
                              "MaxSpan cap reached (span_norm=%d >= %d), requesting quit",
                              (int)span_norm, g_td5.ini.max_span);
                    s_max_span_logged = 1;
                }
                td5_trace_shutdown();
                g_td5.quit_requested = 1;
                return;
            }
        }
    }

    if (!td5_trace_begin_frame(frame))
        return;

    /* Frame row -- one per stage call, no actor loop. */
    if (td5_trace_active(TD5_TRACE_MOD_FRAME, stage_bit)) {
        TD5_TraceFrameRow fr;
        fr.game_state           = (int)g_td5.game_state;
        fr.paused               = g_td5.paused;
        fr.pause_menu_active    = s_pause_menu_active;
        fr.fade_state           = g_td5.race_end_fade_state;
        fr.countdown_timer      = s_race_countdown_state;
        fr.sim_time_accumulator = g_td5.sim_time_accumulator;
        fr.sim_tick_budget      = g_td5.sim_tick_budget;
        fr.frame_dt             = g_td5.normalized_frame_dt;
        fr.instant_fps          = g_td5.instant_fps;
        fr.viewport_count       = g_td5.viewport_count;
        fr.split_screen_mode    = g_td5.split_screen_mode;
        fr.ticks_this_frame     = ticks_this_frame;
        td5_trace_emit_frame(frame, sim_tick, stage, &fr);
    }

    /* Per-actor rows. The actor loop only runs if at least one slot-bound
     * module is active for this stage — otherwise we skip the actor->bytes
     * casts entirely. */
    int any_slot_module =
        td5_trace_active(TD5_TRACE_MOD_POSE,     stage_bit) ||
        td5_trace_active(TD5_TRACE_MOD_MOTION,   stage_bit) ||
        td5_trace_active(TD5_TRACE_MOD_TRACK,    stage_bit) ||
        td5_trace_active(TD5_TRACE_MOD_CONTROLS, stage_bit) ||
        td5_trace_active(TD5_TRACE_MOD_PROGRESS, stage_bit) ||
        td5_trace_active(TD5_TRACE_MOD_ROTATION, stage_bit);

    if (any_slot_module) {
        for (int i = 0; i < TD5_MAX_RACER_SLOTS; i++) {
            if (!td5_trace_selected_slot(i))
                continue;
            if (s_slot_state[i].state == 3)
                continue;  /* disabled */

            TD5_Actor *actor = td5_game_get_actor(i);
            if (!actor) continue;
            uint8_t *a = (uint8_t *)actor;

            if (td5_trace_active(TD5_TRACE_MOD_POSE, stage_bit)) {
                TD5_TracePoseRow r;
                r.slot       = i;
                r.world_x    = *(int32_t *)(a + 0x1FC);
                r.world_y    = *(int32_t *)(a + 0x200);
                r.world_z    = *(int32_t *)(a + 0x204);
                r.ang_roll   = *(int32_t *)(a + 0x1C0);
                r.ang_yaw    = *(int32_t *)(a + 0x1C4);
                r.ang_pitch  = *(int32_t *)(a + 0x1C8);
                r.disp_roll  = *(int16_t *)(a + 0x208);
                r.disp_yaw   = *(int16_t *)(a + 0x20A);
                r.disp_pitch = *(int16_t *)(a + 0x20C);
                td5_trace_emit_pose(frame, sim_tick, stage, &r);
            }

            if (td5_trace_active(TD5_TRACE_MOD_MOTION, stage_bit)) {
                TD5_TraceMotionRow r;
                r.slot       = i;
                r.vel_x      = *(int32_t *)(a + 0x1CC);
                r.vel_y      = *(int32_t *)(a + 0x1D0);
                r.vel_z      = *(int32_t *)(a + 0x1D4);
                r.long_speed = *(int32_t *)(a + 0x314);
                r.lat_speed  = *(int32_t *)(a + 0x318);
                r.front_slip = *(int32_t *)(a + 0x31C);
                r.rear_slip  = *(int32_t *)(a + 0x320);
                td5_trace_emit_motion(frame, sim_tick, stage, &r);
            }

            if (td5_trace_active(TD5_TRACE_MOD_TRACK, stage_bit)) {
                TD5_TraceTrackRow r;
                r.slot               = i;
                r.span_raw           = *(int16_t *)(a + 0x080);
                r.span_norm          = *(int16_t *)(a + 0x082);
                r.span_accum         = *(int16_t *)(a + 0x084);
                r.span_high          = *(int16_t *)(a + 0x086);
                r.track_contact_flag = *(uint8_t *)(a + 0x37B);
                r.wheel_contact_mask = *(uint8_t *)(a + 0x37D);
                td5_trace_emit_track(frame, sim_tick, stage, &r);
            }

            if (td5_trace_active(TD5_TRACE_MOD_CONTROLS, stage_bit)) {
                TD5_TraceControlsRow r;
                r.slot         = i;
                r.slot_state   = s_slot_state[i].state;
                r.steering_cmd = *(int32_t *)(a + 0x30C);
                r.engine_speed = *(int32_t *)(a + 0x310);
                r.current_gear = *(uint8_t *)(a + 0x36B);
                r.vehicle_mode = *(uint8_t *)(a + 0x379);
                td5_trace_emit_controls(frame, sim_tick, stage, &r);
            }

            if (td5_trace_active(TD5_TRACE_MOD_ROTATION, stage_bit)) {
                TD5_TraceRotationRow r;
                r.slot          = i;
                r.ang_vel_roll  = *(int32_t *)(a + 0x1C0);
                r.ang_vel_yaw   = *(int32_t *)(a + 0x1C4);
                r.ang_vel_pitch = *(int32_t *)(a + 0x1C8);
                r.euler_roll    = *(int32_t *)(a + 0x1F0);
                r.euler_yaw     = *(int32_t *)(a + 0x1F4);
                r.euler_pitch   = *(int32_t *)(a + 0x1F8);
                r.disp_roll     = *(int16_t *)(a + 0x208);
                r.disp_yaw      = *(int16_t *)(a + 0x20A);
                r.disp_pitch    = *(int16_t *)(a + 0x20C);
                /* +0x37C is NEW airborne mask post-D2 fix; +0x37D is OLD/prev. */
                r.wcb           = *(uint8_t *)(a + 0x37C);
                r.scf           = *(uint8_t *)(a + 0x376);
                r.vmode         = *(uint8_t *)(a + 0x379);
                r.afc           = *(uint16_t *)(a + 0x360);
                r.world_y       = *(int32_t *)(a + 0x200);
                r.vel_y         = *(int32_t *)(a + 0x1D0);
                td5_trace_emit_rotation(frame, sim_tick, stage, &r);
            }

            if (td5_trace_active(TD5_TRACE_MOD_PROGRESS, stage_bit)) {
                TD5_TraceProgressRow r;
                r.slot                     = i;
                r.race_position            = *(uint8_t *)(a + 0x383);
                r.finish_time              = *(int32_t *)(a + 0x328);
                r.accum_distance           = *(int32_t *)(a + 0x32C);
                r.pending_finish_timer     = *(uint16_t *)(a + 0x344);
                r.metric_checkpoint_index  = s_metrics[i].checkpoint_index;
                r.metric_checkpoint_mask   = s_metrics[i].checkpoint_bitmask;
                r.metric_normalized_span   = s_metrics[i].normalized_span;
                r.metric_timer_ticks       = s_metrics[i].timer_ticks;
                r.metric_display_position  = s_metrics[i].display_position;
                r.metric_speed_bonus       = s_metrics[i].speed_bonus;
                r.metric_top_speed         = s_metrics[i].top_speed;
                td5_trace_emit_progress(frame, sim_tick, stage, &r);
            }
        }
    }

    /* View rows */
    if (td5_trace_active(TD5_TRACE_MOD_VIEW, stage_bit)) {
        for (int vp = 0; vp < g_td5.viewport_count && vp < TD5_MAX_VIEWPORTS; vp++) {
            TD5_TraceViewRow r;
            r.view_index   = vp;
            r.actor_slot   = g_actorSlotForView[vp];
            r.cam_world_x  = g_camWorldPos[vp][0];
            r.cam_world_y  = g_camWorldPos[vp][1];
            r.cam_world_z  = g_camWorldPos[vp][2];
            td5_trace_emit_view(frame, sim_tick, stage, &r);
        }
    }
}

/* Map stage strings to bitmask + dispatch. Each call site below the FSM
 * uses the string + bit pair so the gate is computed once per stage. */
static void td5_game_trace_stage(const char *stage, int ticks_this_frame)
{
    /* Profiler hook: attribute the time since the previous stage to this stage
     * name (zone "post_physics" = the physics phase, etc.). ~zero cost when the
     * profiler is disabled. */
    td5_profile_mark(stage);

    unsigned int stage_bit = 0;
    if      (!strcmp(stage, "frame_begin"))   stage_bit = TD5_TRACE_STG_FRAME_BEGIN;
    else if (!strcmp(stage, "pre_physics"))   stage_bit = TD5_TRACE_STG_PRE_PHYSICS;
    else if (!strcmp(stage, "post_physics"))  stage_bit = TD5_TRACE_STG_POST_PHYSICS;
    else if (!strcmp(stage, "post_ai"))       stage_bit = TD5_TRACE_STG_POST_AI;
    else if (!strcmp(stage, "post_track"))    stage_bit = TD5_TRACE_STG_POST_TRACK;
    else if (!strcmp(stage, "post_camera"))   stage_bit = TD5_TRACE_STG_POST_CAMERA;
    else if (!strcmp(stage, "post_progress")) stage_bit = TD5_TRACE_STG_POST_PROGRESS;
    else if (!strcmp(stage, "frame_end"))     stage_bit = TD5_TRACE_STG_FRAME_END;
    else if (!strcmp(stage, "pause_menu"))    stage_bit = TD5_TRACE_STG_PAUSE_MENU;
    else if (!strcmp(stage, "countdown"))     stage_bit = TD5_TRACE_STG_COUNTDOWN;
    else                                      stage_bit = TD5_TRACE_STG_ALL;
    td5_game_trace_stage_impl(stage, stage_bit, ticks_this_frame);
}

/* ========================================================================
 * RunRaceFrame (0x42B580)
 *
 * Fixed-timestep simulation loop + render pipeline + audio tick.
 * Returns 0 = racing continues, 1 = race over (fade complete).
 * ======================================================================== */

/* ---- Photo-booth carpic capture (offline preview generation, dev only) ----
 * When --PhotoBoothCar is active, frame the car at two angles (a 3/4 view and a
 * pure side), grab each composited frame via the backend readback, write them as
 * raw PNGs into the car's asset dir, then quit so the generator does the next
 * car. re/tools/td6_photobooth.py crops by chroma + stacks the two into
 * carpic0..3.png. Angles/distance are env-tunable (TD5RE_PB_*) for dialing-in. */
extern void Backend_RequestCapture(void);
extern int  Backend_GetCapture(unsigned char **px, int *w, int *h);
extern int  stbi_write_png(const char *f, int w, int h, int comp, const void *data, int stride);

static float pb_env_f(const char *name, float def) {
    const char *v = getenv(name);
    return (v && v[0]) ? (float)atof(v) : def;
}
static void pb_write_frame(const char *code, char tag) {
    unsigned char *px; int w, h;
    if (!Backend_GetCapture(&px, &w, &h)) return;
    unsigned char *rgb = (unsigned char *)malloc((size_t)w * h * 3);
    if (!rgb) return;
    for (int i = 0; i < w * h; i++) {       /* BGRA -> RGB */
        rgb[i*3+0] = px[i*4+2];
        rgb[i*3+1] = px[i*4+1];
        rgb[i*3+2] = px[i*4+0];
    }
    char path[256];
    snprintf(path, sizeof(path), "re/assets/cars/%s/_pb_%c.png", code, tag);
    stbi_write_png(path, w, h, 3, rgb, w * 3);
    free(rgb);
    TD5_LOG_I(LOG_TAG, "photobooth: wrote %s (%dx%d)", path, w, h);
}
static void td5_photobooth_tick(void) {
    if (!td5_render_photobooth_active()) return;
    static char code[16] = {0};
    if (!code[0]) {
        const char *zip = td5_asset_get_player_override_zip();   /* "cars/<code>.zip" */
        if (!zip) return;
        const char *p = strrchr(zip, '/'); p = p ? p + 1 : zip;
        int n = 0; while (p[n] && p[n] != '.' && n < 15) { code[n] = p[n]; n++; }
        code[n] = '\0';
    }
    float dist = pb_env_f("TD5RE_PB_DIST", 1900.0f);

    /* Derive the car's world heading from its orientation matrix so the booth
     * angles are RELATIVE to where the nose actually points — the car spawns
     * with a few degrees of yaw, so a fixed absolute azimuth leaves the "side"
     * shot slightly off-perpendicular (you can see a sliver of the back). The
     * car's local forward (+Z) maps to world (m[2], m[8]); the azimuth that
     * places the camera directly in front of the nose is atan2(m[2], m[8])
     * (matches apply_inspection_camera's offset = (sin az, *, cos az)). */
    float front_az = 0.0f;
    {
        TD5_Actor *pa = td5_game_get_actor(0);
        if (pa) {
            const float *m = pa->rotation_matrix.m;
            front_az = atan2f(m[2], m[8]) * (180.0f / 3.14159265358979f);
        }
    }
    /* Offsets are measured from front_az (0 = dead-on nose):
     *   A = front-3/4 hero  (+45 -> front + one flank), EL 5 = ~mid-car height
     *       so the roof is nearly edge-on (user-approved framing).
     *   B = pure side       (+90 -> camera square to the door, level). */
    float azA = front_az + pb_env_f("TD5RE_PB_AOFS", 45.0f), elA = pb_env_f("TD5RE_PB_EL_A", 5.0f);
    float azB = front_az + pb_env_f("TD5RE_PB_BOFS", 90.0f), elB = pb_env_f("TD5RE_PB_EL_B", 0.0f);

    static int logged = 0;
    if (!logged) { logged = 1;
        TD5_LOG_I(LOG_TAG, "photobooth: front_az=%.1f -> azA=%.1f azB=%.1f", front_az, azA, azB);
    }

    static int st = 0, fc = 0;
    fc++;
    switch (st) {
    case 0:  /* hold angle A until the scene settles, then request a grab */
        td5_render_set_inspect_cam(1, azA, elA, dist, 0);
        if (fc > 50) { Backend_RequestCapture(); st = 1; }
        break;
    case 1:  /* angle A captured last present -> save it, switch to angle B */
        pb_write_frame(code, 'a');
        td5_render_set_inspect_cam(1, azB, elB, dist, 0);
        fc = 0; st = 2;
        break;
    case 2:  /* hold angle B, then grab */
        if (fc > 20) { Backend_RequestCapture(); st = 3; }
        break;
    case 3:  /* save B and quit */
        pb_write_frame(code, 'b');
        TD5_LOG_I(LOG_TAG, "photobooth: done for '%s', quitting", code);
        g_td5.quit_requested = 1;
        st = 4;
        break;
    default: break;
    }
}

/* ========================================================================
 * Network lockstep input sync (S10)
 *
 * Mirrors the original PollRaceSessionInput network branch (0x0042C470):
 * once per rendered frame, sample the local player's input, exchange it through
 * the deterministic lockstep barrier (host merges all slots + broadcasts; client
 * submits its slot + receives the merged frame), then write the host-merged
 * authoritative input for all racer slots back into the input store so every
 * substep this frame uses identical bits. The client also adopts the host's
 * authoritative frame dt and corrects this frame's sim-time accumulator to it,
 * so all machines advance the simulation by the same number of ticks. Only input
 * bitmasks + dt cross the wire, so the simulation stays bit-identical.
 *
 * Returns 1 if the barrier failed (timeout/disconnect): the local slot's ESCAPE
 * bit is set so the race falls through to its normal quit/fade path.
 * ======================================================================== */
static int s_net_pause_round;  /* [S31] merged NET_PAUSE present this round  */
static int s_net_pause_slot;   /* first REMOTE slot pausing (-1 = none/local) */

/* HUD query: a REMOTE player is holding the race paused and no local menu is
 * covering the screen -> draw the "PAUSED BY <name>" overlay. */
int td5_game_net_remote_pause_slot(void)
{
    if (!g_td5.network_active || !s_net_pause_round || s_pause_menu_active)
        return -1;
    return s_net_pause_slot;
}

/* [S31] The actor whose finishing position the LOCAL player cares about:
 * slot 0 in local play, but the machine's own net slot in a network race
 * (on a client, actor 0 is the HOST -- capturing it showed the host's
 * position in the fade digit / "last position" after BACK TO LOBBY). */
static TD5_Actor *td5_game_local_player_actor(void)
{
    int slot = 0;
    if (g_td5.network_active) {
        slot = td5_net_local_slot();
        if (slot < 0 || slot >= TD5_MAX_RACER_SLOTS)
            slot = 0;
    }
    return td5_game_get_actor(slot);
}

/* [#5 2026-06-19] Decouple render from the lockstep barrier. When ON (default),
 * the network input exchange runs once per 30 Hz SIM TICK (inside the fixed-step
 * loop) instead of once per RENDER FRAME, so render frames that drain no tick
 * skip the blocking peer-wait and free-run at the monitor refresh rate. The sim
 * stays a deterministic 30 Hz lockstep: each tick is one mutual round, so both
 * peers keep an identical round/tick count (the handshake itself matches them,
 * which is why the per-frame dt-correction is unneeded in this path). Set
 * TD5RE_NET_RENDER_DECOUPLE=0 to revert to the legacy once-per-frame exchange
 * (race capped at the lockstep round-trip rate). */
static int net_render_decouple_enabled(void) {
    static int init = 0, on = 1;
    if (!init) {
        /* [2026-06-19] Render-decouple via NON-BLOCKING per-tick lockstep
         * (td5_game_net_try_sync + td5_net_*_frame_nb): render free-runs at the
         * monitor rate and interpolates while the 30 Hz sim advances one tick per
         * completed round, so a 180 Hz client is no longer paced by a 60 Hz host.
         * DEFAULT ON — this is the standard netplay path now. It POLLS the peer
         * instead of stalling the render thread (the earlier blocking per-tick
         * attempt did, -> 15 fps). Set =0 ON BOTH PEERS to fall back to the legacy
         * blocking per-frame exchange (caps the client at the host's refresh) if a
         * desync ever turns up -- the two paths use different round cadences, so
         * both machines must run the same setting. */
        on = td5_env_flag_on("TD5RE_NET_RENDER_DECOUPLE");   /* default ON */
        init = 1;
        TD5_LOG_I(LOG_TAG, "Netplay render decouple: %s",
                  on ? "ON (exchange per sim tick, render free-runs)"
                     : "OFF (legacy once-per-frame exchange)");
    }
    return on;
}

/* apply_dt_correction: 1 = legacy once-per-frame call (the client adopts the
 * host's dt so its per-frame tick COUNT matches the host's). 0 = per-tick
 * decoupled call (one round == one tick, so the handshake already matches tick
 * counts and the accumulator fixup must be skipped). */
static int td5_game_net_sync_frame(int apply_dt_correction) {
    uint32_t bits[TD5_NET_MAX_PLAYERS];
    int local = td5_net_local_slot();
    int i, ok;
    float local_dt = g_td5.normalized_frame_dt;
    float dt = local_dt;   /* host: authoritative; client: receives host's dt */

    if (local < 0 || local >= TD5_NET_MAX_PLAYERS)
        local = 0;

    /* Sample local controller(s); the port writes the primary local player into
     * slot 0. Place that input at our assigned network slot for the exchange. */
    td5_input_poll_race_session();

    for (i = 0; i < TD5_NET_MAX_PLAYERS; i++)
        bits[i] = 0;
    bits[local] = td5_input_get_control_bits(0);

    /* [S31 PAUSE SYNC] Hold the NET_PAUSE bit while the local pause menu is
     * open. The sim freeze keys on the MERGED bit (any slot) so every machine
     * pauses and resumes on the same lockstep round -- a one-sided freeze
     * would diverge the tick counts permanently. */
    if (s_pause_menu_active)
        bits[local] |= (uint32_t)TD5_INPUT_NET_PAUSE;

    /* [S31] Drain in-race ring messages: RACE_LEFT (0x17) = a peer quit the
     * race from their pause menu. Lockstep cannot continue without them, so
     * end ours through the same quit-to-menu fade. */
    {
        TD5_NetMsgType mtype;
        void *mdata;
        int msize;
        while (td5_net_receive(&mtype, &mdata, &msize)) {
            if (mtype == TD5_DXPDATA && mdata && msize >= 1 &&
                ((const uint8_t *)mdata)[0] == 0x19 && !s_pause_restart_pending) {
                /* [S31] Host restarted the race: same fade + re-init here. */
                TD5_LOG_I(LOG_TAG, "net: host restarted the race -> re-init");
                s_pause_menu_active = 0;
                s_pause_restart_pending = 1;
                td5_game_begin_fade_out(0);
            } else if (mtype == TD5_DXPDATA && mdata && msize >= 1 &&
                ((const uint8_t *)mdata)[0] == 0x17 &&
                !s_pause_exit_pending && !s_pause_lobby_pending) {
                TD5_Actor *pl = td5_game_local_player_actor();
                TD5_LOG_I(LOG_TAG, "net: peer left the race -> back to lobby");
                s_pause_menu_active = 0;
                s_pause_lobby_pending = 1;   /* session stays alive */
                s_finish_position_display = pl ? (int)pl->race_position + 1 : 1;
                td5_game_begin_fade_out(0);
            } else if (mtype == TD5_DXPDATA && mdata && msize >= 1 &&
                ((const uint8_t *)mdata)[0] == 0x1A) {
                /* [END RACE NOW 2026-06-30] A peer force-finished the race. End
                 * ours the same way so all machines reach results together;
                 * lockstep keeps running until check_race_completion() builds the
                 * results (no leave-pending flag — this is a finish, not a quit). */
                TD5_LOG_I(LOG_TAG, "net: peer force-finished the race -> ending here too");
                s_pause_menu_active = 0;
                s_pause_endrace_confirm = 0;
                td5_game_force_finish_race();
            }
        }
    }

    /* [2026-06-19] A peer is leaving the race (back-to-lobby / quit / restart
     * pending -- set locally by our own pause menu, or just drained from a
     * peer's 0x17/0x19 above). Lockstep cannot continue, so SKIP the blocking
     * exchange and let the fade-out run. Without this the next barrier waits on
     * FRAMEs the departed peer will never send -> the client froze ~20s then
     * fell through to the MENU instead of the lobby. (The net worker also
     * SetEvents s_evt_frame_ack on 0x17/0x19 so a peer already blocked in the
     * wait wakes promptly enough to reach this drain.) */
    if (s_pause_lobby_pending || s_pause_exit_pending || s_pause_restart_pending)
        return 0;

    ok = td5_net_is_host() ? td5_net_handle_host_frame(bits, &dt)
                           : td5_net_handle_client_frame(bits, &dt);

    if (!ok) {
        bits[local] |= (uint32_t)TD5_INPUT_ESCAPE;
        td5_input_set_control_bits(local, bits[local]);
        TD5_LOG_W(LOG_TAG, "net lockstep failed (%s) -> quit to menu",
                  td5_net_is_connection_lost() ? "connection lost" : "timeout");
        /* [S31] Don't leave the player in a dead lockstep race (the old ESC
         * bit only opened the pause menu over a frozen sim): run the pause
         * QUIT TO MENU sequence directly -- fade out, release, back to the
         * frontend, where the lobby/browser screens already handle the
         * connection-lost cleanup. */
        if (!s_pause_exit_pending) {
            TD5_Actor *pl = td5_game_local_player_actor();
            s_pause_menu_active = 0;
            s_pause_exit_pending = 1;
            s_finish_position_display = pl ? (int)pl->race_position + 1 : 1;
            td5_game_begin_fade_out(0);
        }
        return 1;
    }

    /* Client adopts the host's dt; correct this frame's accumulator, which
     * update_frame_timing() already advanced by the LOCAL dt. uint32 wrap is
     * safe because the accumulator was just incremented by old_delta. */
    if (apply_dt_correction && !td5_net_is_host() && dt != local_dt) {
        uint32_t old_delta = td5_game_normalized_dt_to_accum(local_dt);
        uint32_t new_delta = td5_game_normalized_dt_to_accum(dt);
        g_td5.sim_time_accumulator += new_delta - old_delta;
        g_td5.normalized_frame_dt = dt;
    }

    /* Push host-merged authoritative input into every racer slot. */
    for (i = 0; i < TD5_NET_MAX_PLAYERS; i++)
        td5_input_set_control_bits(i, bits[i]);

    /* [S31 PAUSE SYNC] Derive this round's synced pause state from the merged
     * bits -- identical on every machine, so every machine freezes/resumes on
     * the same round. */
    s_net_pause_round = 0;
    s_net_pause_slot  = -1;
    for (i = 0; i < TD5_NET_MAX_PLAYERS; i++) {
        if (bits[i] & (uint32_t)TD5_INPUT_NET_PAUSE) {
            s_net_pause_round = 1;
            if (i != local && s_net_pause_slot < 0)
                s_net_pause_slot = i;
        }
    }

    /* Mute engine/skid SFX on the machine that did NOT pause while a peer
     * holds the race frozen (the pauser's own menu handles its own mute). */
    {
        static int s_prev_net_pause;
        if (s_net_pause_round != s_prev_net_pause) {
            s_prev_net_pause = s_net_pause_round;
            if (!s_pause_menu_active)
                td5_sound_set_sfx_muted(s_net_pause_round);
        }
    }

    return 0;
}

/* [#5 2026-06-19] Non-blocking per-tick lockstep for the DECOUPLED render path.
 * Unlike td5_game_net_sync_frame (which blocks the render thread on the peer),
 * this samples local input, exchanges ONE round non-blocking, and returns:
 *   1 = round complete (merged bits pushed to all slots; run the sim tick),
 *   0 = pending (no peer input yet -> render an interpolated frame, retry),
 *  -1 = failed (connection lost; quit-to-menu fade armed).
 * Render frames that get 0 free-run at monitor rate (g_subTickFraction holds at
 * 1.0, already clamped) while the 30 Hz sim waits for the next lockstep round. */
static int td5_game_net_try_sync(void) {
    extern int td5_net_host_frame_nb(uint32_t *control_bits, float *frame_dt);
    extern int td5_net_client_frame_nb(uint32_t *control_bits, float *frame_dt);
    uint32_t bits[TD5_NET_MAX_PLAYERS];
    int local = td5_net_local_slot();
    int i, r;
    float dt = g_td5.normalized_frame_dt;
    if (local < 0 || local >= TD5_NET_MAX_PLAYERS) local = 0;

    td5_input_poll_race_session();
    for (i = 0; i < TD5_NET_MAX_PLAYERS; i++) bits[i] = 0;
    bits[local] = td5_input_get_control_bits(0);
    if (s_pause_menu_active)
        bits[local] |= (uint32_t)TD5_INPUT_NET_PAUSE;

    /* Drain in-race ring messages (peer back-to-lobby 0x17 / host restart 0x19),
     * same as td5_game_net_sync_frame. */
    {
        TD5_NetMsgType mtype;
        void *mdata;
        int msize;
        while (td5_net_receive(&mtype, &mdata, &msize)) {
            if (mtype == TD5_DXPDATA && mdata && msize >= 1 &&
                ((const uint8_t *)mdata)[0] == 0x19 && !s_pause_restart_pending) {
                s_pause_menu_active = 0;
                s_pause_restart_pending = 1;
                td5_game_begin_fade_out(0);
            } else if (mtype == TD5_DXPDATA && mdata && msize >= 1 &&
                ((const uint8_t *)mdata)[0] == 0x17 &&
                !s_pause_exit_pending && !s_pause_lobby_pending) {
                TD5_Actor *pl = td5_game_local_player_actor();
                s_pause_menu_active = 0;
                s_pause_lobby_pending = 1;
                s_finish_position_display = pl ? (int)pl->race_position + 1 : 1;
                td5_game_begin_fade_out(0);
            } else if (mtype == TD5_DXPDATA && mdata && msize >= 1 &&
                ((const uint8_t *)mdata)[0] == 0x1A) {
                /* [END RACE NOW 2026-06-30] Peer force-finished -> end ours too
                 * (results, not leave); lockstep continues to build results. */
                s_pause_menu_active = 0;
                s_pause_endrace_confirm = 0;
                td5_game_force_finish_race();
            }
        }
    }
    /* Race is exiting -> stop lockstepping, let the fade run (freeze fix). */
    if (s_pause_lobby_pending || s_pause_exit_pending || s_pause_restart_pending)
        return 1;

    r = td5_net_is_host() ? td5_net_host_frame_nb(bits, &dt)
                          : td5_net_client_frame_nb(bits, &dt);
    if (r == 0)
        return 0;   /* round not ready -> render interpolated, retry next frame */
    if (r < 0) {
        /* Lockstep failed -> run the quit-to-menu fade (clone of the blocking
         * path's failure handling). */
        if (!s_pause_exit_pending) {
            TD5_Actor *pl = td5_game_local_player_actor();
            s_pause_menu_active = 0;
            s_pause_exit_pending = 1;
            s_finish_position_display = pl ? (int)pl->race_position + 1 : 1;
            td5_game_begin_fade_out(0);
        }
        return -1;
    }

    /* Round complete: push host-merged authoritative input into every slot. */
    for (i = 0; i < TD5_NET_MAX_PLAYERS; i++)
        td5_input_set_control_bits(i, bits[i]);
    /* Derive this round's synced pause state from the merged bits. */
    s_net_pause_round = 0;
    s_net_pause_slot  = -1;
    for (i = 0; i < TD5_NET_MAX_PLAYERS; i++) {
        if (bits[i] & (uint32_t)TD5_INPUT_NET_PAUSE) {
            s_net_pause_round = 1;
            if (i != local && s_net_pause_slot < 0)
                s_net_pause_slot = i;
        }
    }
    return 1;
}

/* ========================================================================
 * [S27 2026-06-05] Controller-disconnect pause + per-viewport reconnect modal
 *
 * SOURCE-PORT FEATURE. The original TD5 binary delegated all device handling to
 * M2DX/DXInput and never queried device presence, so there is no original
 * behaviour to reverse-engineer here — this is documented new code.
 *
 * Detection lives in the platform layer (td5_plat_input_joystick_is_lost): a
 * physically removed pad makes GetDeviceState fail persistently and that latches
 * a per-slot "lost" flag. Here, once per render frame, we map each active human
 * player to its device and, if a JOYSTICK player's device is gone, freeze the
 * race (g_td5.paused = 1) and remember the player so the HUD can modal that
 * player's pane. Keyboard players have no device to lose, so they are never
 * flagged (no modal). The race stays paused while ANY required player is missing
 * and resumes when the last one returns.
 * ======================================================================== */
int td5_game_device_disconnect_active(void) { return s_disconnect_pause_active; }
int td5_game_player_disconnected(int player)
{
    if (player < 0 || player >= 32) return 0;
    return (int)((s_player_disconnected_mask >> player) & 1u);
}

#ifndef TD5RE_RELEASE
/* DEV test hook (F9): toggle a simulated controller loss for one player so the
 * pause/modal/resume path can be exercised without a physical unplug. */
void td5_game_debug_toggle_sim_device_loss(int player)
{
    if (player < 0 || player >= TD5_MAX_HUMAN_PLAYERS) return;
    s_sim_loss_mask ^= (1u << player);
    TD5_LOG_W(LOG_TAG, "DEBUG F9: simulate device loss player %d -> %s",
              player, ((s_sim_loss_mask >> player) & 1u) ? "LOST" : "back");
}
#endif

/* Re-evaluate controller-disconnect state for the active human players and drive
 * the instant pause / resume. Called once per render frame at the top of
 * td5_game_run_race_frame (before the fixed-step sim loop). */
static void td5_game_update_device_disconnect(void)
{
    uint32_t mask = 0;
    int hp = g_td5.num_human_players;
    if (hp < 1) hp = 1;
    if (hp > TD5_MAX_HUMAN_PLAYERS) hp = TD5_MAX_HUMAN_PLAYERS;

#ifndef TD5RE_RELEASE
    /* Timed simulation knob ([Debug] SimulateJoyLoss). Drives s_sim_loss_mask
     * from WALL-CLOCK time (the sim clock freezes while paused, so a sim-tick
     * timer would never reach the auto-reconnect): lose the pad delay_ms after
     * race start, give it back hold_ms later. Lets a headless run capture the
     * whole pause->modal->resume cycle. */
    if (g_td5.ini.sim_joy_loss_player >= 0 &&
        g_td5.ini.sim_joy_loss_player < TD5_MAX_HUMAN_PLAYERS) {
        int sp = g_td5.ini.sim_joy_loss_player;
        if (s_sim_loss_race_start_ms == 0) s_sim_loss_race_start_ms = td5_plat_time_ms();
        uint32_t elapsed = td5_plat_time_ms() - s_sim_loss_race_start_ms;
        uint32_t on  = (uint32_t)g_td5.ini.sim_joy_loss_delay_ms;
        uint32_t off = on + (uint32_t)g_td5.ini.sim_joy_loss_hold_ms;
        if (elapsed >= on && elapsed < off) s_sim_loss_mask |=  (1u << sp);
        else                                s_sim_loss_mask &= ~(1u << sp);
    }
#endif

    for (int p = 0; p < hp; p++) {
        int lost = 0;
        /* Real disconnect: only a joystick player (input source > 0) can lose a
         * device. Keyboard players (source 0) are deliberately never flagged. */
        if (td5_input_get_input_source(p) > 0 && td5_plat_input_joystick_is_lost(p))
            lost = 1;
#ifndef TD5RE_RELEASE
        if (s_sim_loss_mask & (1u << p)) lost = 1;   /* DEV simulated loss */
#endif
        if (lost) mask |= (1u << p);
    }
    s_player_disconnected_mask = mask;

    int any = (mask != 0);
    if (any && !s_disconnect_pause_active) {
        s_disconnect_prev_paused  = g_td5.paused;   /* usually 0 mid-race, 1 in countdown */
        s_disconnect_pause_active = 1;
        g_td5.paused = 1;
        td5_sound_set_sfx_muted(1);                 /* silence engine/skid like a pause */
        TD5_LOG_W(LOG_TAG, "Controller disconnect: race paused (player mask=0x%X)", mask);
    } else if (any && s_disconnect_pause_active) {
        g_td5.paused = 1;                           /* re-assert so the sim stays frozen */
    } else if (!any && s_disconnect_pause_active) {
        s_disconnect_pause_active = 0;
        if (!s_pause_menu_active) {                 /* don't fight an open pause menu */
            g_td5.paused = s_disconnect_prev_paused;
            td5_sound_set_sfx_muted(0);
        }
        TD5_LOG_I(LOG_TAG, "Controller reconnect: race resumed");
    }
}

/* [Phase B render-transform] Per-pane CPU command lists. Each pane's expensive
 * world build (track walk / mesh transform / cull) records into its own list
 * (no shared GPU state — the texture-cache lookup is deferred to replay); the
 * main thread replays the lists in order onto the immediate context. */
static RCmdList *s_pane_cmd[TD5_MAX_VIEWPORTS];

static int rcmd_pool_ensure(int count)
{
    int i;
    if (count > TD5_MAX_VIEWPORTS) count = TD5_MAX_VIEWPORTS;
    for (i = 0; i < count; i++) {
        if (!s_pane_cmd[i]) {
            s_pane_cmd[i] = td5_rcmd_create();
            if (!s_pane_cmd[i]) return 0;
        }
    }
    return 1;
}

/* Build ONE pane's world into its command list. Camera+projection were baked
 * into g_rs[vp] by the serial Phase-1 pass (camera_apply touches the global
 * g_depthFovFactor). Records draws + state into s_pane_cmd[vp]; issues NO GPU
 * work and touches no shared GPU state, so this is safe to run on a worker. */
static void mt_build_pane_rcmd(int vp, void *unused)
{
    (void)unused;
    td5_render_scratch_bind(vp);              /* this thread's g_rs -> pool[vp] */
    td5_rcmd_reset(s_pane_cmd[vp]);
    td5_rcmd_begin(s_pane_cmd[vp]);

    td5_plat_render_set_viewport(s_viewports[vp].x, s_viewports[vp].y,
                                 s_viewports[vp].w, s_viewports[vp].h);
    td5_plat_render_set_clip_rect(s_viewports[vp].x, s_viewports[vp].y,
                                  s_viewports[vp].x + s_viewports[vp].w,
                                  s_viewports[vp].y + s_viewports[vp].h);

    td5_render_set_race_pass(TD5_RACE_PASS_SKY);
    td5_render_set_fog(0);
    td5_render_set_race_pass(TD5_RACE_PASS_OPAQUE);
    td5_render_set_fog(1);

    td5_render_begin_world_pass();
    td5_render_actors_for_view(vp);
    td5_render_flush_translucent();
    td5_render_flush_projected_buckets();
    td5_render_flush_deferred_additive();

    td5_rcmd_end();
    td5_render_scratch_unbind();
}

int td5_game_run_race_frame(void) {
    int i;
    td5_photobooth_tick();   /* sets the booth camera before this frame renders */
    /* ---- Update frame timing ---- */
    td5_game_update_frame_timing();

    /* ---- Network lockstep input sync (S10) ----
     * With a network session active, sample + exchange input ONCE per frame
     * here and hold the host-merged authoritative bits constant across every
     * substep below (mirrors the original's once-per-frame network poll).
     * Non-network play keeps polling once per substep, inside the loop. */
    int net_lockstep = (g_td5.network_active && td5_net_is_active());
    /* [#5 2026-06-19] Decoupled (default): exchange per SIM TICK inside the
     * fixed-step loop below so render free-runs at monitor rate. Legacy (knob
     * off): exchange ONCE here and hold the merged bits constant across this
     * frame's substeps (mirrors the original's once-per-frame network poll). */
    int net_decoupled = net_lockstep && net_render_decouple_enabled();
    if (net_lockstep && !net_decoupled)
        td5_game_net_sync_frame(1);

    td5_profile_begin_frame();   /* race-frame profiler timeline (zones via trace_stage + render/present below) */
    td5_game_trace_stage("frame_begin", 0);

    /* [S27] Controller-disconnect check (once per frame, before the sim loop).
     * Pauses instantly the frame a required player's pad drops; resumes when it
     * returns. Sets g_td5.paused so the fixed-step loop freezes below. */
    td5_game_update_device_disconnect();

    /* ---- Race completion check (before sim loop) ----
     *
     * Original CheckRaceCompletionState @ 0x00409E80 is invoked once per frame
     * from the head of RunRaceFrame @ 0x0042B580 — it inlines the per-actor
     * checkpoint/circuit scan and then runs the aggregate finish check.
     * The port mirrors that cadence: advance_pending_finish_state runs once
     * per frame per active slot here, feeding the results of any finish
     * promotion into check_race_completion's aggregate scan below. */
    if (g_td5.race_end_fade_state == 0) {
        int pf;
        uint32_t frame_accum = td5_game_normalized_dt_to_accum(g_td5.normalized_frame_dt);
        /* [INFECT 2026-06-25] Convert suspects arrested during last frame's sim
         * loop into cops (random police car). Deferred here so the heavy per-slot
         * car-swap runs OUTSIDE the sim loop, and BEFORE check_race_completion so
         * the "all suspects infected -> cops win" end condition sees the new state
         * (and a final arrest converts rather than triggering a premature end). */
        td5_game_process_pending_infections();
        for (pf = 0; pf < TD5_MAX_RACER_SLOTS; pf++) {
            if (s_slot_state[pf].state == 3) continue;  /* disabled */
            advance_pending_finish_state(pf, frame_accum);
            /* [MP per-viewport finish] Capture this slot's place the moment it
             * finishes (companion_1 0->1), for its viewport's own indicator. */
            /* [FIX timeout-DNF 2026-06-23] Don't capture a finishing place for a
             * slot that DNF'd on the P2P checkpoint timer (companion_2==2) — a
             * timeout is a loss, so its viewport shows no finishing-position digit
             * (matches the original routing a timeout to the FAILED screen rather
             * than a placed result, RE 0x00422480). A genuine finish sets
             * companion_2==1 and is unaffected. */
            if (s_slot_state[pf].companion_1 && s_slot_finish_place[pf] == 0 &&
                s_slot_state[pf].companion_2 != 2) {
                TD5_Actor *fa = td5_game_get_actor(pf);
                s_slot_finish_place[pf] = fa ? (int)fa->race_position + 1 : 1;
                TD5_LOG_I(LOG_TAG, "Viewport finish: slot=%d place=%d",
                          pf, s_slot_finish_place[pf]);
            }
        }

        int completion = check_race_completion(frame_accum);
        if (completion) {
            g_td5.race_end_fade_state = 1;

            /* Capture the finishing place ONCE for the centered victory digit.
             * Reading actor->race_position every frame in the draw path was racy
             * (update_race_order can re-sort mid-window), which blanked/changed
             * the number — [user 2026-05-30: "number sometimes is not present"]. */
            {
                TD5_Actor *pl = td5_game_local_player_actor();
                s_finish_position_display = pl ? (int)pl->race_position + 1 : 1;
                TD5_LOG_I(LOG_TAG,
                          "Finish capture: player race_position=%d -> display=%d "
                          "(active_racers=%d, g_racer_count=%d, traffic_base=%d)",
                          pl ? (int)pl->race_position : -1,
                          s_finish_position_display, active_racer_count(),
                          g_racer_count, g_traffic_slot_base);
            }

            /* Select fade direction based on viewport layout */
            td5_game_begin_fade_out(g_td5.split_screen_mode);
        }
    }

    /* ---- Fade-out accumulation ---- */
    if (g_td5.race_end_fade_state > 0) {
        /* Replay timeout: force fade after 45 seconds */
        if (s_replay_mode && s_race_end_timer_start > 0) {
            uint32_t elapsed = td5_plat_time_ms() - s_race_end_timer_start;
            if (elapsed > 45000) {
                s_fade_accumulator = 255.0f;
            }
        }

        /* Victory hold: while the 1st-place star is playing, freeze the fade so
         * the race view (with the white star + centered finishing number) stays
         * up for TD5_VICTORY_HOLD_MS, THEN fade to results. Without this the
         * race exited in ~0.5s and the (now faithfully-slow 1/640) star never
         * got room to expand — it read as a fast dark flash and the number
         * blinked by. The directional wipe is already suppressed for the star
         * case (mutual-exclusion gate below), so the hold just delays the cut to
         * results. [user 2026-05-30: animation too fast / number missing.] */
        int victory_holding =
            (s_race_end_radial_pulse_enabled && s_victory_hold_start != 0 &&
             (td5_plat_time_ms() - s_victory_hold_start) < TD5_VICTORY_HOLD_MS);

        if (!victory_holding) {
            /* Accumulate fade — ~1s wipe at 60fps.
             * Clamp dt to 1/30 to prevent instant fade after pause frames
             * (pause menu exit produces a huge dt spike on the next frame). */
            float fade_dt_seconds = td5_game_normalized_dt_to_seconds(g_td5.normalized_frame_dt);
            /* Per-frame cap guards against the huge dt spike on pause-menu exit. Scale
             * the cap with the game-speed (fast-forward) multiplier so the black wipe
             * stays in sync with the sim at higher speeds instead of crawling at
             * real-time while everything else races (user 2026-05-30: black transition
             * didn't scale with game speed). At 1x the cap is unchanged (~1s wipe). */
            float fade_cap = 0.034f;
            if (g_td5.ini.trace_fast_forward > 1.0f) fade_cap *= g_td5.ini.trace_fast_forward;
            if (fade_dt_seconds > fade_cap) fade_dt_seconds = fade_cap;
            s_fade_accumulator += fade_dt_seconds * 255.0f;
            if (s_fade_accumulator >= 255.0f) {
                s_fade_accumulator = 255.0f;

                /* Fade complete: release all race resources and exit */
                td5_game_release_race_resources();

                if (s_pause_restart_pending) {
                    /* [REWORK 2026-06-05/S15] Pause-menu RESTART RACE: resources
                     * released above; signal the FSM to re-init the SAME race
                     * (same track/car/opponents/laps/direction from g_td5). */
                    TD5_LOG_I(LOG_TAG, "Fade complete (RESTART RACE) -> re-init same race");
                    s_pause_restart_pending = 0;
                    return 3;  /* 3 = restart same race (-> re-init, stay in RACE) */
                }
                if (s_pause_exit_pending) {
                    TD5_LOG_I(LOG_TAG, "Fade complete (ESC exit) -> main menu");
                    s_pause_exit_pending = 0;
                    s_pause_lobby_pending = 0;
                    return 2;  /* 2 = ESC quit (-> main menu) */
                }
                if (s_pause_lobby_pending) {
                    TD5_LOG_I(LOG_TAG, "Fade complete (BACK TO LOBBY) -> lobby");
                    s_pause_lobby_pending = 0;
                    return 4;  /* 4 = back to the net / local-MP lobby */
                }
                TD5_LOG_I(LOG_TAG, "Fade complete (race finish) -> results");
                return 1;  /* 1 = normal race finish (-> results screen) */
            }
        }
    }

    /* ---- Fixed-timestep simulation loop ---- */
    /* Drain sim_time_accumulator in 0x10000 steps.
     * Normal play caps at 4 ticks/frame (spiral-of-death protection).
     * Trace fast-forward bypasses this cap so the diff-race skill can
     * actually run at multiple sim-ticks per render frame without the
     * injected accumulator piling up unused. */
    int ticks_this_frame = 0;
    s_pause_input_done = 0;  /* allow pause input once this frame */

    /* [BUG 5a 2026-06-15] Per-frame ESC edge for the cinematic abort.
     * Detected here, OUTSIDE the fixed-step loop, so a brief ESC tap exits a
     * View Replay / attract demo even on frames that drain zero sim ticks (the
     * in-loop esc_edge below misses those entirely at >30 Hz render rates).
     * Arms the one-shot s_replay_esc_exit, which the cinematic-abort block ORs
     * into its trigger; the latch persists across no-tick frames until consumed.
     * Gated by TD5RE_REPLAY_EXIT (shared with the controller exit): "0" reverts
     * to in-loop-ESC-only. */
    {
        static int s_replay_esc_init = 0;
        static int s_replay_esc_enabled = 1;
        if (!s_replay_esc_init) {
            s_replay_esc_enabled = td5_env_flag_on("TD5RE_REPLAY_EXIT");  /* default ON */
            s_replay_esc_init = 1;
            TD5_LOG_I(LOG_TAG, "Replay per-frame ESC-exit: %s",
                      s_replay_esc_enabled ? "enabled" : "disabled (in-loop ESC only)");
        }
        /* [BUG 5a ROOT CAUSE 2026-06-15] Refresh the live keyboard buffer here.
         * td5_plat_input_key_pressed() reads s_keyboard[], which is ONLY filled
         * by td5_plat_input_poll() (its GetDeviceState call). During a cinematic
         * race the in-loop poll (td5_input_poll_race_session) takes the playback
         * branch and `goto`s past the keyboard read, polling ONLY joystick slots;
         * and at >30 Hz most frames drain zero sim ticks, so the in-loop poll
         * never runs at all. Result: s_keyboard[] stays frozen at whatever it held
         * when replay started, so BOTH this per-frame ESC edge AND the in-playback
         * ESC check (td5_input.c) read a stale key and the abort never fires —
         * THE reason "ESC still doesn't exit replay". Pump the platform input once
         * per frame (slot 0; the poll refreshes s_keyboard regardless of slot),
         * exactly like td5_frontend_display_loop does, so the live ESC is seen.
         * Only while cinematic so normal races are byte-unchanged. */
        if (s_replay_esc_enabled && td5_game_is_cinematic_race()) {
            TD5_InputState kb_refresh;
            td5_plat_input_poll(0, &kb_refresh);
        }
        int esc_frame_now = td5_plat_input_key_pressed(0x01);
        if (s_replay_esc_enabled && esc_frame_now && !s_prev_esc_frame &&
            td5_game_is_cinematic_race() && td5_plat_window_is_focused()) {
            /* [ATTRACT DEMO 2026-06-25] Only arm the cinematic ESC abort when the
             * window is focused: an attract demo must keep playing in the background
             * (and a stale/global ESC must not cut a self-running demo short). The
             * user can only intend to abort while looking at the window. */
            s_replay_esc_exit = 1;
            TD5_LOG_I(LOG_TAG, "Replay: per-frame ESC edge -> exit requested");
        }
        s_prev_esc_frame = esc_frame_now;
    }

    const int max_ticks_per_frame =
        (g_td5.ini.trace_fast_forward > 1.0f)
            ? ((int)g_td5.ini.trace_fast_forward + 8)
            : 4;

    {
        static uint32_t s_frame_diag_ctr = 0;
        if ((s_frame_diag_ctr++ % 60u) == 0u) {
            TD5_LOG_I(LOG_TAG, "race_frame: accum=0x%X paused=%d pause_menu=%d trans=%d",
                      g_td5.sim_time_accumulator, g_td5.paused,
                      s_pause_menu_active, g_cameraTransitionActive);
        }
    }

    while (g_td5.sim_time_accumulator > 0xFFFF &&
           ticks_this_frame < max_ticks_per_frame) {
        /* --- Input polling / lockstep exchange ---
         * Local-only play polls the controller every substep. Networked play:
         *  - decoupled (knob on): exchange ONE lockstep round HERE per sim tick,
         *    NON-BLOCKING. If the peer's input for this round hasn't arrived,
         *    td5_game_net_try_sync() returns 0 and we break -> this render frame
         *    free-runs and interpolates (g_subTickFraction holds at 1.0) instead
         *    of stalling, so a 180 Hz client is no longer paced by a 60 Hz host.
         *    The 30 Hz sim still advances exactly one tick per completed round
         *    (deterministic). Keeps flowing during a net pause (the accumulator
         *    advances regardless, so this loop still iterates).
         *  - legacy (knob off): input was sampled + exchanged once per frame above
         *    and the merged bits are held constant; re-polling would clobber them. */
        if (net_lockstep) {
            if (net_decoupled) {
                int nb = td5_game_net_try_sync();
                if (nb <= 0) break;   /* 0 = round pending (interpolate); -1 = failed (fade armed) */
            }
        } else {
            td5_input_poll_race_session();
        }

        /* --- Camera angle caching (per viewport) --- */
        /* Original (0x0042b84e) gates camera inside the pause-flag check.
         * Camera must tick during countdown but NOT during pause menu.
         *
         * Split camera into two phases to match RunRaceFrame (0x0042B580):
         *   - cache_angles runs HERE, before physics, snapshotting the
         *     previous-frame orientation so UpdateVehicleRelativeCamera /
         *     chase modes 1-2 can compute a shortest-path delta later.
         *   - update_chase_all runs AFTER physics (see post-physics calls
         *     below) so the orbit smoothing reads fresh post-integration
         *     actor angles/position instead of stale ones. Running it
         *     before physics caused visible chase-cam shake whenever the
         *     vehicle was accelerating (pre-physics yaw fed into the
         *     orbit angle integrator one tick behind the actual heading). */
        if (!s_pause_menu_active && !(net_lockstep && s_net_pause_round)) {
            td5_camera_cache_angles();
        }

        /* --- Pause menu (ESC toggles) --- */
        int esc_now = td5_plat_input_key_pressed(0x01);
        int esc_edge = (esc_now && !s_prev_esc_state);
        int pause_menu_was_active = s_pause_menu_active;
        s_prev_esc_state = esc_now;

        /* [PORT ENHANCEMENT 2026-06] The rebindable PAUSE action (keyboard key or
         * joystick button mapped in the control-config screen) opens/toggles the
         * pause menu, in addition to ESC. The original only had ESC; the PAUSE
         * action bit was being SET by the input layer but never consumed here, so
         * a mapped pause key did nothing. Read it from any human player's control
         * word. It only opens/continues — it never triggers the ESC exit-to-results
         * or the cinematic abort (those stay ESC-only). */
        int pause_act_now = 0;
        {
            int hp = g_td5.num_human_players;
            if (hp < 1) hp = 1;
            if (hp > TD5_MAX_HUMAN_PLAYERS) hp = TD5_MAX_HUMAN_PLAYERS;
            for (int pi = 0; pi < hp; pi++)
                if (td5_input_get_control_bits(pi) & TD5_INPUT_PAUSE) { pause_act_now = 1; break; }
        }
        int pause_act_edge = (pause_act_now && !s_prev_pause_act);
        s_prev_pause_act = pause_act_now;

        /* Replay/demo abort: ESC during a cinematic (View Replay / attract demo)
         * race does NOT open the pause menu — it ends the race immediately and
         * returns to where it was launched (results for replay, menu for demo).
         * [CONFIRMED orig PollRaceSessionInput @0x42C470: when g_inputPlaybackActive
         *  the pause/MuteAll block is skipped and the abort block ORs bit 30 →
         *  fade-out. The original used a simple ESC-to-exit, NOT a pause menu.] */
        /* [BUG 5a] s_replay_esc_exit is the per-frame ESC latch armed above the
         * loop; consume it here so a no-tick-frame ESC tap still aborts. */
        int replay_esc_edge = s_replay_esc_exit;
        s_replay_esc_exit = 0;
        if ((esc_edge || replay_esc_edge || td5_input_replay_exit_requested()) &&
            td5_game_is_cinematic_race() &&
            td5_plat_window_is_focused() &&   /* [ATTRACT DEMO] only abort when focused */
            !s_replay_abort_pending &&
            g_td5.race_end_fade_state == 0) {
            s_replay_abort_pending = 1;
            /* View Replay was launched from the results screen → return there
             * (s_pause_exit_pending left 0 → run-race-frame returns 1 = results).
             * Attract demo was launched from the main menu → return there
             * (s_pause_exit_pending = 1 → returns 2 = main menu). */
            if (s_demo_mode) s_pause_exit_pending = 1;
            TD5_LOG_I(LOG_TAG, "%s: ESC abort -> fade out (-> %s)",
                      s_demo_mode ? "Demo" : "Replay",
                      s_demo_mode ? "menu" : "results");
            td5_game_begin_fade_out(0);
        }
        if ((esc_edge || pause_act_edge) && !s_pause_menu_active && !td5_game_is_cinematic_race()) {
            s_pause_menu_active = 1;
            s_pause_menu_cursor = 3;  /* default to CONTINUE. [RADIO + END RACE
                                       * NOW 2026-06-30] A RADIO slider sits at
                                       * row 2 and END RACE NOW is an action row:
                                       * 0=VIEW 1=SOUND 2=RADIO 3=CONTINUE
                                       * 4=RESTART RACE 5=END RACE NOW
                                       * [6=BACK TO LOBBY MP-only] QUIT / EXIT.
                                       * ENTER then dismisses in one keypress. */
            /* [BUGFIX #15 2026-06-15] Consume the currently-held direction keys so
             * the menu's nav edge-detection treats them as ALREADY pressed. UP is
             * usually accelerate and DOWN is brake; without this seed, opening the
             * menu mid-throttle fired the up/down nav edge on the first menu frame
             * and bumped the cursor off CONTINUE. Latch s_prev_* = held state now
             * (same key/pad reads as the nav block below) so only a fresh
             * release-then-press moves the cursor. Keyboard + controller. */
            {
                uint32_t jnav_open = 0;
                int hp = g_td5.num_human_players;
                if (hp < 1) hp = 1;
                if (hp > TD5_MAX_HUMAN_PLAYERS) hp = TD5_MAX_HUMAN_PLAYERS;
                for (int pi = 0; pi < hp; pi++) jnav_open |= td5_plat_input_joystick_nav(pi);
                s_prev_up    = td5_plat_input_key_pressed(0xC8) || (jnav_open & 0x04);
                s_prev_down  = td5_plat_input_key_pressed(0xD0) || (jnav_open & 0x08);
                s_prev_left  = td5_plat_input_key_pressed(0xCB) || (jnav_open & 0x01);
                s_prev_right = td5_plat_input_key_pressed(0xCD) || (jnav_open & 0x02);
                s_prev_enter = td5_plat_input_key_pressed(0x1C) || (jnav_open & 0x10);
                s_prev_jb    = (jnav_open & 0x20) ? 1 : 0;
            }
            td5_sound_set_sfx_muted(1);  /* silence engine/skid SFX on pause
                                          * (mirrors DXSound::MuteAll @0x0042C470). */
            /* [REWORK 2026-06-05/S15] Silence the in-race music (CD audio) while
             * paused — user does not want pause music. Restored (replayed) on
             * resume to gameplay; left stopped on RESTART/QUIT/EXIT (RESTART's
             * init re-plays it, the others tear the race down). */
            td5_sound_cd_stop();
            td5_sound_set_paused(1);  /* [item 24] suspend ALL audio at once on pause (gated TD5RE_PAUSE_MUTE) */
            /* [FF stuck-motor fix 2026-06-15] The sim (and its FF update inside
             * PollRaceSessionInput) freezes while paused, so any motor asserted at
             * the moment of pause would stay latched — the reported "force feedback
             * doesn't stop on the pause menu". Zero all motors now; the per-frame FF
             * update naturally resumes them on CONTINUE. */
            td5_input_ff_stop();
            TD5_LOG_I(LOG_TAG, "Pause menu opened (cursor=CONTINUE row 3, SFX muted, music stopped, FF stopped)");
        }
        if (s_pause_menu_active) {
            /* Process pause menu input ONCE per frame (not per tick) to avoid
             * multiple edge triggers from the sim tick loop. */
            if (!s_pause_input_done) {
                s_pause_input_done = 1;

                /* Nav edge-detection state (s_prev_down/up/left/right/enter/jb)
                 * is file-static and SEEDED on menu-open with the held keys
                 * [BUGFIX #15], so held UP/DOWN can't auto-navigate on frame 1. */
                /* [PORT ENHANCEMENT 2026-06] Aggregate joystick nav from every human
                 * player's in-race device (dpad/left stick = move, A = confirm) so the
                 * pause menu is navigable with the pad, not just the keyboard. The
                 * frontend scan handles are released while the race owns the device,
                 * so this reads each player's exclusive device directly. */
                uint32_t jnav = 0;
                {
                    int hp = g_td5.num_human_players;
                    if (hp < 1) hp = 1;
                    if (hp > TD5_MAX_HUMAN_PLAYERS) hp = TD5_MAX_HUMAN_PLAYERS;
                    for (int pi = 0; pi < hp; pi++) jnav |= td5_plat_input_joystick_nav(pi);
                }
                int key_down  = td5_plat_input_key_pressed(0xD0) || (jnav & 0x08);
                int key_up    = td5_plat_input_key_pressed(0xC8) || (jnav & 0x04);
                int key_left  = td5_plat_input_key_pressed(0xCB) || (jnav & 0x01);
                int key_right = td5_plat_input_key_pressed(0xCD) || (jnav & 0x02);
                int key_enter = td5_plat_input_key_pressed(0x1C) || (jnav & 0x10); /* A = confirm */

                /* Navigation: [REWORK 2026-06-05/S15] 6 selectable rows
                 * (VIEW / SOUND / CONTINUE / RESTART RACE / QUIT TO MENU / EXIT GAME).
                 * Original had 5 rows (RunAudioOptionsOverlay @ 0x0043BF70). */
                /* [MP 2026-06-13] BACK TO LOBBY only exists in a
                 * multiplayer/network session — it's removed from the single-
                 * player pause menu (see td5_hud_init_pause_menu). Wrap navigation
                 * over the live row count so QUIT TO MENU / EXIT GAME stay
                 * reachable and there's no gap. */
                /* [RADIO 2026-06-30] +1 row over the original (RADIO slider @
                 * row 2). */
                /* [END RACE NOW GATING 2026-07-02] END RACE NOW is now local-MP
                 * only (num_human_players>1) instead of always-present — see
                 * td5_hud_init_pause_menu for the matching row-skip. Row indices
                 * below are computed dynamically so cursor comparisons stay
                 * correct across all 3 combinations (SP / network-only MP /
                 * local split-screen MP). */
                int local_mp = (g_td5.num_human_players > 1);        /* local split-screen MP */
                int mp_any   = (g_td5.network_active || local_mp);   /* any MP (net or local) */
                int row_continue = 3;
                int row_restart  = 4;
                int row_endrace  = local_mp ? 5 : -1;                 /* -1 = row not present */
                int row_lobby    = mp_any ? (5 + (local_mp ? 1 : 0)) : -1;
                int row_quit     = 5 + (local_mp ? 1 : 0) + (mp_any ? 1 : 0);
                int row_exit     = row_quit + 1;
                int pause_rows   = row_exit + 1;
                /* Freeze cursor movement while a confirmation prompt (END RACE NOW
                 * or the local-MP RESTART/QUIT/EXIT confirm) is up so the highlight
                 * can't drift off the row mid-confirm. */
                if (!s_pause_endrace_confirm && !s_pause_action_confirm) {
                    if (key_down  && !s_prev_down)  s_pause_menu_cursor = (s_pause_menu_cursor + 1) % pause_rows;
                    if (key_up    && !s_prev_up)    s_pause_menu_cursor = (s_pause_menu_cursor + pause_rows - 1) % pause_rows;
                }

                /* Left/right adjusts sliders for rows 0-2.
                 * [CONFIRMED @ 0x0043C211] CONTINUOUS while held — (&DAT_004B135C)[cursor] ± 0.02f, clamp [0,1].
                 * Row 0 "VIEW"  (DAT_004B135C) → [0x00466EA8] view distance, no audio call [0x0043C379]
                 * Row 1 "MUSIC" (DAT_004B1360) → DXSound::CDSetVolume(frac*0xFFFF)        [0x0043C390]
                 * Row 2 "SOUND" (DAT_004B1364) → DXSound::SetVolume  (frac*0xFFFF) master [0x0043C3A8] */
                /* Diagnostic: log every frame when L/R is held during pause,
                 * so we can confirm input reaches this code path. */
                if (key_left || key_right) {
                    static int s_pause_input_seq = 0;
                    if ((s_pause_input_seq++ & 0x07) == 0) {  /* throttle 1-in-8 */
                        TD5_LOG_I(LOG_TAG,
                                  "PAUSE input: cursor=%d L=%d R=%d (slider gate cursor<2: %s)",
                                  s_pause_menu_cursor, key_left, key_right,
                                  s_pause_menu_cursor < 2 ? "PASS" : "BLOCKED");
                    }
                }
                /* [RADIO 2026-06-30] Three slider rows now: 0=VIEW, 1=SOUND,
                 * 2=RADIO (internet-radio volume). */
                if (s_pause_menu_cursor < 3) {
                    int delta = key_right ? +1 : (key_left ? -1 : 0);
                    if (delta) {
                        if (s_pause_menu_cursor == 0) {
                            float v = td5_save_get_view_distance() + (float)delta * 0.02f;
                            td5_save_set_view_distance(v);
                            TD5_LOG_I(LOG_TAG, "Pause slider VIEW: %.2f", td5_save_get_view_distance());
                        } else if (s_pause_menu_cursor == 1) {  /* SOUND (SFX master volume) */
                            int v = td5_save_get_sfx_volume() + delta * 2;
                            td5_save_set_sfx_volume(v);
                            td5_sound_set_sfx_volume(td5_save_get_sfx_volume());
                            s_pause_options_dirty = 1;  /* persist to td5re.ini on close [PART B] */
                            TD5_LOG_I(LOG_TAG, "Pause slider SOUND: %d", td5_save_get_sfx_volume());
                        } else {  /* cursor 2 = RADIO (internet-radio volume) */
                            int v = g_td5.ini.radio_volume + delta * 2;
                            if (v < 0)   v = 0;
                            if (v > 100) v = 100;
                            g_td5.ini.radio_volume = v;
                            td5_sound_set_radio_volume(v);  /* live-apply to the radio */
                            s_pause_options_dirty = 1;      /* persist to td5re.ini on close */
                            TD5_LOG_I(LOG_TAG, "Pause slider RADIO: %d", v);
                        }
                    }
                }

                /* Confirm (Enter). Rows: 3=CONTINUE 4=RESTART RACE
                 *   [5=END RACE NOW, local-MP only] [BACK TO LOBBY, MP only]
                 *   QUIT TO MENU / EXIT GAME. Row indices are the row_* locals
                 *   computed above (dynamic — see [END RACE NOW GATING
                 *   2026-07-02]). */
                if (key_enter && !s_prev_enter) {
                    if (s_pause_endrace_confirm) {
                        /* [END RACE NOW 2026-06-30] Second ENTER on the
                         * confirmation = YES. Force-finish the race for everyone,
                         * ranked by current track progress (no DNFs). In a net
                         * race, broadcast RACE_FORCE_FINISH (0x1A) so every machine
                         * ends + builds results together. Any player may trigger
                         * this (unlike host-only RESTART) — it just ends the race
                         * like QUIT/LOBBY do, so there is no one-sided re-init to
                         * reconcile. */
                        s_pause_endrace_confirm = 0;
                        s_pause_menu_active = 0;
                        if (g_td5.network_active && td5_net_is_active()) {
                            uint8_t ff_msg[8] = {0x1A, 0, 0, 0, 0, 0, 0, 0};
                            td5_net_send(TD5_DXPDATA, ff_msg, 8);
                        }
                        td5_game_force_finish_race();
                        TD5_LOG_I(LOG_TAG, "Pause menu: END RACE NOW confirmed -> force-finishing race");
                    } else if (s_pause_action_confirm) {
                        /* [PAUSE CONFIRM 2026-07-02] Second ENTER on the
                         * RESTART/QUIT/EXIT confirmation = YES. Run the actual
                         * action body (shared with the no-confirm-needed path). */
                        int confirmed = s_pause_action_confirm;
                        s_pause_action_confirm = 0;
                        TD5_LOG_I(LOG_TAG, "Pause menu: action %d confirmed (split-screen MP)", confirmed);
                        td5_game_pause_execute_action(confirmed);
                    } else if (s_pause_menu_cursor == row_continue) {
                        /* CONTINUE — close the menu, resume the race. */
                        s_pause_menu_active = 0;
                    } else if (row_endrace >= 0 && s_pause_menu_cursor == row_endrace) {
                        /* [END RACE NOW 2026-06-30][GATED 2026-07-02] Arm the
                         * confirmation prompt (don't end yet). A second ENTER
                         * confirms; B/ESC cancels. Only reachable when
                         * row_endrace>=0, i.e. local split-screen MP
                         * (num_human_players>1) -- see the row-index locals above. */
                        s_pause_endrace_confirm = 1;
                        TD5_LOG_I(LOG_TAG, "Pause menu: END RACE NOW selected -> confirm prompt");
                    } else if (s_pause_menu_cursor == row_restart) {
                        /* RESTART RACE. [PAUSE CONFIRM 2026-07-02] In local
                         * split-screen MP, arm a Y/N confirm first (restarting is
                         * shared — a stray press shouldn't silently reset everyone
                         * else's race). Single-player and network-only (1 local
                         * human) keep the original immediate behavior. */
                        if (local_mp) {
                            s_pause_action_confirm = 1;
                            TD5_LOG_I(LOG_TAG, "Pause menu: RESTART RACE selected -> confirm prompt (split-screen MP)");
                        } else {
                            td5_game_pause_execute_action(1);
                        }
                    } else if (row_lobby >= 0 && s_pause_menu_cursor == row_lobby) {
                        /* BACK TO LOBBY [S31] -- only reachable when row_lobby>=0
                         * (any MP session). [PAUSE CONFIRM 2026-07-02] Same
                         * local-split-screen-MP confirm gating as RESTART RACE /
                         * QUIT TO MENU / EXIT GAME above -- it ends the shared
                         * race for every pad on the screen just like those do.
                         * Network-only (1 local human) keeps the original
                         * immediate behavior. */
                        if (local_mp) {
                            s_pause_action_confirm = 4;
                            TD5_LOG_I(LOG_TAG, "Pause menu: BACK TO LOBBY selected -> confirm prompt (split-screen MP)");
                        } else {
                            td5_game_pause_execute_action(4);
                        }
                    } else if (s_pause_menu_cursor == row_quit) {
                        /* QUIT TO MENU. [PAUSE CONFIRM 2026-07-02] Same
                         * local-split-screen-MP confirm gating as RESTART RACE
                         * above. */
                        if (local_mp) {
                            s_pause_action_confirm = 2;
                            TD5_LOG_I(LOG_TAG, "Pause menu: QUIT TO MENU selected -> confirm prompt (split-screen MP)");
                        } else {
                            td5_game_pause_execute_action(2);
                        }
                    } else if (s_pause_menu_cursor == row_exit) {
                        /* EXIT GAME. [PAUSE CONFIRM 2026-07-02] Same
                         * local-split-screen-MP confirm gating as RESTART RACE
                         * above. */
                        if (local_mp) {
                            s_pause_action_confirm = 3;
                            TD5_LOG_I(LOG_TAG, "Pause menu: EXIT GAME selected -> confirm prompt (split-screen MP)");
                        } else {
                            td5_game_pause_execute_action(3);
                        }
                    }
                }

                /* Joystick B = back = continue (close the menu), mirroring the
                 * frontend B=back convention. [PORT ENHANCEMENT 2026-06]
                 * [END RACE NOW 2026-06-30][PAUSE CONFIRM 2026-07-02] When
                 * either confirmation prompt is up, B cancels it (returns to the
                 * menu) instead of closing the menu — matching the "B = NO"
                 * footer. */
                {
                    int jb = (jnav & 0x20) ? 1 : 0;
                    if (jb && !s_prev_jb) {
                        if (s_pause_endrace_confirm)     s_pause_endrace_confirm = 0;
                        else if (s_pause_action_confirm) s_pause_action_confirm = 0;
                        else                              s_pause_menu_active = 0;
                    }
                    s_prev_jb = jb;
                }

                s_prev_down = key_down; s_prev_up = key_up;
                s_prev_left = key_left; s_prev_right = key_right;
                s_prev_enter = key_enter;
            }

            /* ESC again = continue; the PAUSE action also toggles the menu shut.
             * [END RACE NOW 2026-06-30][PAUSE CONFIRM 2026-07-02] When either
             * confirmation prompt is up, ESC cancels it (keeps the menu open)
             * instead of closing the menu — matching the "ESC = NO" footer. */
            if ((esc_edge || pause_act_edge) && pause_menu_was_active) {
                if (s_pause_endrace_confirm)     s_pause_endrace_confirm = 0;
                else if (s_pause_action_confirm) s_pause_action_confirm = 0;
                else                              s_pause_menu_active = 0;
            }

            /* Update graphical overlay (SELBOX + sliders). Rows: 0=VIEW,
             * 1=SOUND, 2=RADIO. The 3rd overlay arg carries the radio fraction
             * (it was the legacy MUSIC slot, repurposed). */
            float view_frac  = td5_save_get_view_distance();
            float radio_frac = (float)g_td5.ini.radio_volume / 100.0f;
            float sfx_frac   = (float)td5_save_get_sfx_volume()   / 100.0f;
            td5_hud_update_pause_overlay(s_pause_menu_cursor, view_frac, radio_frac, sfx_frac);

            /* SFX audio gating while paused (mirrors the original's MuteAll on
             * entry + per-row preview + UnMuteAll on exit):
             *   - menu open  → engine/skid SFX stay muted EXCEPT on the SOUND row
             *     ([REWORK 2026-06-05/S15] now cursor 1, was cursor 2 before the
             *     MUSIC row was removed), where un-muting lets the volume slider
             *     preview audibly. [CONFIRMED @ 0x0043BF70: ModifyOveride(1,...)]
             *   - menu just closed this frame → restore all SFX, and if we are
             *     resuming gameplay (CONTINUE / ESC / pad-B — NOT restart, quit-to-
             *     menu, or exit-game) replay the in-race music that pause-enter
             *     stopped. RESTART's race re-init re-plays it; the two exit paths
             *     leave it stopped so it doesn't bleed past the race. */
            if (s_pause_menu_active) {
                td5_sound_set_sfx_muted(s_pause_menu_cursor == 1 ? 0 : 1);
                /* [RADIO 2026-06-30] Preview the radio while the cursor sits on
                 * the RADIO row (2) so the volume slider is audible; otherwise the
                 * radio stays muted on the pause menu (mirrors the SFX preview on
                 * the SOUND row). */
                td5_plat_radio_set_playing(s_pause_menu_cursor == 2 ? 1 : 0);
            } else if (pause_menu_was_active) {
                td5_sound_set_sfx_muted(0);
                td5_sound_set_paused(0);  /* [item 24] resume audio + restore music volume */
                if (!s_pause_exit_pending && !s_pause_restart_pending && !g_td5.quit_requested) {
                    td5_sound_cd_play(g_td5.track_index % 10 + 1);  /* same call as InitRace step 16 */
                    TD5_LOG_I(LOG_TAG, "Pause resumed -> music restarted (track=%d)",
                              g_td5.track_index % 10 + 1);
                }
                /* Pause menu just closed: re-anchor the chase-camera smoothing
                 * so the first resumed frame doesn't glide across the gap (esp.
                 * pausing right at the start, when the countdown spring is mid-
                 * settle). Render-only; cannot affect the sim. */
                td5_camera_snap_smoothing();
            }

            /* If the pause menu just closed (Continue / ESC) and a volume slider
             * changed, flush it to td5re.ini. The Exit path persists inline
             * above before its fade. [PART B 2026-06-02] */
            if (pause_menu_was_active && !s_pause_menu_active && s_pause_options_dirty) {
                g_td5.ini.sfx_volume   = td5_save_get_sfx_volume();
                g_td5.ini.music_volume = td5_save_get_music_volume();
                td5_ini_persist_options();
                s_pause_options_dirty = 0;
            }

            /* [S31 PAUSE SYNC] Net race: the sim freeze keys on the MERGED
             * pause bit so both machines stop on the same lockstep round.
             * Until the locally-opened menu's bit round-trips (~1 frame) the
             * sim keeps ticking under the menu -- identically everywhere. */
            if (!net_lockstep || s_net_pause_round) {
                g_td5.sim_time_accumulator -= TD5_TICK_ACCUMULATOR_ONE;
                ticks_this_frame++;
                td5_game_trace_stage("pause_menu", ticks_this_frame);
                continue;
            }
        } else if (net_lockstep && s_net_pause_round) {
            /* A REMOTE player paused: freeze the same rounds without a local
             * menu; the HUD draws the PAUSED BY overlay. */
            g_td5.sim_time_accumulator -= TD5_TICK_ACCUMULATOR_ONE;
            ticks_this_frame++;
            td5_game_trace_stage("pause_menu", ticks_this_frame);
            continue;
        }

        /* [S27] Controller-disconnect freeze: a required player's pad is gone.
         * Drain a tick exactly like the pause menu (skip physics + AI entirely)
         * so the whole field stops dead until the pad returns — distinct from the
         * start-countdown freeze below, which still ticks AI/engine-RPM. The
         * per-viewport reconnect modal is drawn in the HUD render path. The input
         * poll at the top of the loop keeps running so reconnect is detected. */
        if (s_disconnect_pause_active) {
            g_td5.sim_time_accumulator -= TD5_TICK_ACCUMULATOR_ONE;
            ticks_this_frame++;
            td5_game_trace_stage("pause_menu", ticks_this_frame);
            continue;
        }

        if (g_td5.paused) {
            /* Sub-tick body ordering matches RunRaceFrame @ 0x0042B580:
             *   UpdateRaceActors (reads OLD g_gamePaused)  @ 0x42B7C7
             *   ResolveVehicleContacts (unconditional)     @ 0x42BA0C
             *   UpdateRaceCameraTransitionTimer (LATE)     @ 0x42BA3A
             *   g_simTimeAccumulator -= 0x10000            @ 0x42BA94
             *   if (gate == 0) g_simulationTickCounter++   @ 0x42BAA1
             *
             * The countdown decrement runs AFTER physics so physics on the
             * gate-clearing sub-tick uses the OLD paused=1, while the counter
             * increment below sees the NEW paused=0 and ticks up. This makes
             * the port's sim_tick=1 row match the original's sim_tick=1 row:
             * a paused-physics snapshot (vel=0).
             *
             * Poll input during countdown so player can rev the engine.
             * Physics runs with g_game_paused=1, which only updates
             * engine RPM (no vehicle movement). */
            td5_physics_set_paused(1);
            for (i = 0; i < TD5_MAX_RACER_SLOTS; i++) {
                /* state==1 = player. Also run for slot 0 under PlayerIsAI=1
                 * to mirror the orig's Frida-hook scoping at
                 * td5_quickrace_hook.js:381 — slot[0].state=0 is scoped to
                 * UpdateRaceActors body ONLY; UpdatePlayerVehicleControlState
                 * fires BEFORE that with state[0]=1. Without this hybrid step,
                 * slot 0's throttle_state / encounter_steering_cmd are never
                 * written by the "no input" path (ts=1, enc_steer=0) before
                 * the AI tick. Then when AI early-returns (script blocking,
                 * e.g. Blueridge heading-misalignment recovery), the snapshot
                 * still carries the injected sub_tick=0 values instead of the
                 * orig's UpdatePlayerVehicleControlState outputs. */
                if (s_slot_state[i].state == 1 ||
                    (i == 0 && g_td5.ini.player_is_ai))
                    td5_input_update_player_control(i);
            }
            /* Zero steering_command at the START of each paused sub-tick.
             * Frida-traced 2026-05-15 on 0x004340C0: every paused-tick AI
             * call has actor->steering_command = 0 on entry, despite the
             * function unconditionally writing to +0x30C. The original must
             * zero steering_command between paused sub-ticks (likely inside
             * UpdateRaceActors @ 0x00436A70 or via a paused-branch helper).
             *
             * Resetting BEFORE AI (rather than after) preserves the AI-
             * written value at the end-of-sub-tick snapshot, matching the
             * orig snapshot which captures the AI-write each paused sub-tick
             * (typically saturated cascade value 0x4000=16384). Pre-AI zero
             * still prevents 121-tick cascade accumulation.
             *
             * Round 1 commit e6622f2 placed this clear post-AI; Agent F + E
             * Round 2 independently identified the placement was wrong — the
             * snapshot was capturing the cleared value (port=0) instead of
             * the AI's computed value, producing universal "port=0 vs orig=
             * non-zero" divergence on all 6 slots across all scenarios. */
            for (i = 0; i < TD5_MAX_RACER_SLOTS; i++) {
                if (s_slot_state[i].state == 3) continue;
                TD5_Actor *cd_actor = td5_game_get_actor(i);
                if (cd_actor)
                    cd_actor->steering_command = 0;
            }

            /* AI runs during countdown — matches RunRaceFrame @ 0x42B580
             * which calls UpdateRaceActors → UpdateActorTrackBehavior
             * (0x00434FE0) unconditionally per sub-tick. UpdateActorTrackBehavior
             * does NOT check g_gamePaused — the AI cascade writes
             * steering_command (+0x30C) every paused sub-tick. */
            td5_ai_tick();

            /* Physics during countdown: run on first + last 3 paused
             * sub-ticks; skip the middle ~117 sub-ticks.
             *
             * RE basis: UpdateVehicleActor @ 0x00406650 + IntegrateVehiclePoseAndContacts
             * @ 0x00405E80 are CALLED unconditionally during pause in the
             * original — the paused branch (0x00406881) only skips the
             * dynamics dispatch (404030 / 404EC0 / 403D90). The
             * IntegrateVehiclePoseAndContacts chain (LAB_0040690B) runs
             * regardless and applies gravity + ground-snap + suspension
             * response every paused sub-tick.
             *
             * State-replay snapshot (2026-05-15) shows orig actor[1]
             * sub_tick=0..120 keeps wheel_contact_bitmask=0,
             * surface_type_chassis=0, wheel_load_accum_*=0,
             * linear_velocity_y=0, world_pos_y unchanged.
             * This is the integrator converging to steady-state for a
             * stationary car welded to the ground: ground-snap
             * recomputes lvy to (avg_ground - prev_y) - g which equals
             * -g while stationary, then the suspension response zeroes
             * out the per-wheel state.
             *
             * Port's td5_physics_tick chain does NOT converge to this
             * zero state in one tick — it produces wcb=6,
             * surface_type_chassis=1, wheel_load_accum_FL=1900, lvy=64
             * after the first paused sub-tick. Running it for all 121
             * paused sub-ticks accumulates those deltas into the
             * sub_tick=1 snapshot (244 labeled divergences).
             *
             * Mitigation strategy: run physics on the FIRST paused
             * sub-tick to perform the initial ground-snap from the
             * spawn pose, then SKIP physics on the middle ~117 paused
             * sub-ticks (the car isn't moving so the integrator has no
             * useful work — except producing port-vs-orig drift), then
             * run physics on the LAST 3 paused sub-ticks so the
             * transition into racing tick 1 sees a freshly-integrated
             * pose.
             *
             * Result on Honolulu PlayerIsAI=1 baseline:
             *   sub_tick=0: 390 → 383 (-7)
             *   sub_tick=1: 244 → 179 (-27%)
             *   racing tick 1: 636 → 636 (=0)
             *   total racing 1..15 aggregate: 9873 → 9859 (-14)
             *
             * The residual ~179 divergences at sub_tick=1 are downstream
             * of port-physics-vs-orig-physics (e.g. actor[2]
             * wheel_load_accum=-1172 = orig sub_tick=0 stale value,
             * vs orig sub_tick=1 = 876 from orig's converging
             * suspension chain). Fixing those requires editing
             * td5_physics.c, which is out of scope for this single-file
             * gate fix. */
            {
                static int s_countdown_physics_ticks = 0;
                static uint32_t s_countdown_last_sim_tick = (uint32_t)-1;
                /* Reset the counter at the start of every new race. */
                if (s_countdown_last_sim_tick != g_td5.simulation_tick_counter &&
                    g_td5.simulation_tick_counter == 0u)
                {
                    s_countdown_physics_ticks = 0;
                }
                s_countdown_last_sim_tick = g_td5.simulation_tick_counter;

                int is_last_paused =
                    (g_cameraTransitionActive > 0 &&
                     g_cameraTransitionActive <= 3 * TD5_COUNTDOWN_DECR);

                if (s_countdown_physics_ticks < 1 || is_last_paused) {
                    td5_physics_tick();
                    if (s_countdown_physics_ticks < 1)
                        s_countdown_physics_ticks++;
                } else {
                    /* Mirror the engine-RPM portion of orig's paused branch
                     * (UpdateVehicleActor @ 0x00406881-0x00406908). The full
                     * td5_physics_tick() is skipped on these middle ~117
                     * paused sub-ticks (see comment block above), but the
                     * engine smoother + paused housekeeping (prev_race_pos
                     * copy, scf clear, AI engine pin, scf-from-cardef-on-
                     * high-RPM gate) MUST run every paused sub-tick to match
                     * orig's RPM convergence from 400 → ~1200 by sub_tick=1.
                     *
                     * Skipping these closed the engine_speed_accum cluster:
                     * 42 fields across 7 scenarios at sub_tick=1 (port=400 or
                     * 800 vs orig=1193-1200 per AI slot, smaller delta on
                     * player slots).
                     *
                     * IntegrateVehiclePoseAndContacts is INTENTIONALLY still
                     * skipped — the port's integrator does not converge to
                     * orig's zero state during stationary countdown, which
                     * is the reason this gate exists. */
                    td5_physics_run_paused_engine_step();
                    /* [FIX 2026-05-26 traffic-countdown-stall] Traffic (6..11)
                     * MUST integrate every countdown sub-tick — orig runs
                     * UpdateTrafficActorMotion unconditionally so traffic
                     * accelerates during the countdown and enters the race at
                     * speed. The engine step above is racer-only; this runs the
                     * traffic friction+pose on the skipped middle sub-ticks.
                     * Without it traffic starts the race from rest and the
                     * steering controller deadlocks (Moscow stuck/spin). */
                    td5_physics_run_paused_traffic_step();
                }
            }

            /* (steering_command reset moved to BEFORE td5_ai_tick() above,
             * so the end-of-sub-tick snapshot captures the AI write rather
             * than the cleared zero. Pre-AI zero still prevents 121-tick
             * cascade accumulation.) */

            /* Run update_race_order during countdown sub-ticks too.
             * Original UpdateRaceActors @ 0x00436A70 calls UpdateRaceOrder
             * unconditionally each sub-tick. Without this, port's
             * g_raceOrderTable stays at identity [0..5] until tick=1,
             * while the original's countdown walker may have established
             * a staggered span_high_water permutation that's preserved
             * across ticks. Per UpdateVehicleActor's paused branch
             * (0x00406888), prev_race_position = race_position is copied
             * each paused sub-tick, so by tick=1 entry, prev_race_position
             * reflects the post-sort race_position from the last
             * countdown sub-tick. */
            update_race_order();
            for (i = 0; i < TD5_MAX_RACER_SLOTS; i++) {
                if (s_slot_state[i].state == 3) continue;
                TD5_Actor *pa = td5_game_get_actor(i);
                if (pa)
                    pa->prev_race_position = pa->race_position;
            }
            /* [2026-06-08 procedural FX] Advance VFX (particle position/size/
             * lifetime + tire-track aging) during the COUNTDOWN too. The normal
             * call lives only in the unpaused branch below, so smoke spawned by
             * revving on the line otherwise sat frozen at its spawn pose/size —
             * the "frozen disc behind the car at the start". This is the COSMETIC
             * particle update only (no rand(), no actor-sim writes — rand() is
             * confined to the spawners), so it cannot desync the lockstep sim. */
            td5_vfx_tick();
            /* Chase camera runs AFTER physics — matches RunRaceFrame
             * (0x0042B580). Countdown still updates the camera so the
             * fly-in/idle-orbit animates while the grid counts down. */
            td5_camera_solve_tick_all();
            /* Countdown decrement runs LATE in the sub-tick, matching
             * UpdateRaceCameraTransitionTimer at 0x42BA3A. On the sub-tick
             * where the timer reaches 0, this flips g_td5.paused 1→0; the
             * counter gate below then sees paused=0 and increments.
             *
             * [PORT 2026-06] While the first-race tutorial overlay is up, DO NOT
             * tick the countdown — freezing g_td5.paused at 1 holds the grid
             * until the player dismisses the overlay (td5_tutorial.c). */
            if (!td5_tutorial_is_active())
                tick_race_countdown();
            g_td5.sim_time_accumulator -= TD5_TICK_ACCUMULATOR_ONE;
            ticks_this_frame++;
            /* Trace BEFORE counter++: Frida emits post_progress on
             * UpdateRaceCameraTransitionTimer.onEnter @ 0x42BA3A, which
             * fires BEFORE the counter bump at 0x42BAA1. Matching that
             * order means the post_progress row for the clearing sub-tick
             * carries counter=0 (paused physics snapshot, sim_tick=0),
             * and counter=1 first appears on the NEXT sub-tick — which
             * has already run non-paused physics once. That's how the
             * original binary's sim_tick=1 post_progress row has
             * non-zero AI velocities. Previously the port bumped the
             * counter on the clearing sub-tick BEFORE emitting, so its
             * sim_tick=1 post_progress captured the paused-physics state
             * instead. [RE basis: 0x42B7C7 UpdateRaceActors →
             * 0x42BA0C ResolveVehicleContacts → 0x42BA3A
             * UpdateRaceCameraTransitionTimer onEnter (trace emit) →
             * 0x42BAA1 counter++] */
            td5_game_trace_stage(g_td5.paused ? "countdown" : "post_progress",
                                 ticks_this_frame);
            if (!g_td5.paused) {
                g_td5.simulation_tick_counter++;
                TD5_LOG_I(LOG_TAG, "Race countdown cleared on sub-tick — counter now 1, next sub-tick runs unpaused physics");
            }
            /* --- Per-actor wrap normalization (countdown sub-tick) ---
             * Original RunRaceFrame @ 0x0042B580 runs the
             * NormalizeActorTrackWrapState loop at the END of EVERY
             * sub-tick (the gRaceCameraTransitionGate gate only suppresses
             * the simulation_tick_counter increment, not the wrap pass).
             * On the very first sub-tick (countdown sub-tick=0), this
             * derives actor+0x82 (span_normalized) from actor+0x84
             * (span_accum) for the first time — seeded by
             * td5_track_init_actor_segment_placement during spawn.
             *
             * Without this call in the paused branch, the next sub-tick
             * (sim_tick=1, unpaused) reads +0x82 = 0 in its post_physics
             * emit, causing the AI cascade to use target span 4 for every
             * slot, saturating steering output at +16384 for all six
             * (vs original full -23904..+40672 range).
             * [Confirmed via diff_race Edinburgh PlayerIsAI=1: port
             *  emitted span_norm=0 for all 6 slots at sim_tick=1; original
             *  emits 62, 65, 68, 59, 56, 53.]
             *
             * [BUGFIX 2026-05-26 traffic-stale-span-norm] Loop bound was
             * TD5_MAX_RACER_SLOTS (6) — only racers got their +0x82
             * (span_normalized) synced per sub-tick. Orig RunRaceFrame
             * @ 0x0042B580 bounds the loop at g_racerCount (= 12 with
             * traffic) so traffic slots 6-11 also normalize. Without it,
             * traffic span_normalized stayed frozen at its spawn value
             * while span_raw advanced → render span-cull (td5_render.c
             * uses +0x82) dropped the car bodies (only shadows rendered)
             * and minimap plotted stale/off-road positions. */
            {
                int norm_count = td5_game_get_total_actor_count();
                for (i = 0; i < norm_count; i++) {
                    TD5_Actor *lap_actor;
                    /* s_slot_state only covers racer slots 0..5; traffic
                     * slots 6-11 are never state==3 during normal play. */
                    if (i < TD5_MAX_RACER_SLOTS && s_slot_state[i].state == 3)
                        continue; /* disabled */
                    lap_actor = td5_game_get_actor(i);
                    if (lap_actor) {
                        td5_track_normalize_actor_wrap(lap_actor);
                    }
                }
            }
            continue;
        }
        td5_physics_set_paused(0);

        /* Continue ticking the countdown timer through the post-paused level==0
         * window so the "1" indicator stays visible for the full ~40 sub-ticks
         * then hides cleanly at timer==0. Mirrors orig's per-frame call to
         * UpdateRaceCameraTransitionTimer @ 0x0040A490 which is invoked
         * regardless of paused state. */
        if (g_cameraTransitionActive > 0 && !td5_tutorial_is_active()) {
            tick_race_countdown();
        }

        /* Input record/playback is handled inside td5_input_poll_race_session(). */

        /* --- Per-slot player control update ---
         * state==1 = active player. Also run for slot 0 under PlayerIsAI=1
         * to mirror the orig's Frida-hook scoping at td5_quickrace_hook.js:381
         * (slot[0].state=0 is scoped to UpdateRaceActors body only).
         * See paused-branch comment above for full rationale. */
        for (i = 0; i < TD5_MAX_RACER_SLOTS; i++) {
            if (s_slot_state[i].state == 1 ||
                (i == 0 && g_td5.ini.player_is_ai)) {
                td5_input_update_player_control(i);
            }
        }

        /* --- Core simulation: AI, physics, contacts ---
         * AI must run BEFORE physics: original UpdateRaceActors @ 0x00436A70
         * interleaves ComputeAIRubberBandThrottle → UpdateActorTrackBehavior
         * → UpdateVehicleActor per slot. The port approximates by ordering
         * AI then physics each tick, so encounter_steering_cmd (+0x33E) is
         * written before td5_physics_update_ai @ 0x00404EC0 reads it
         * (throttle gate > 0x20 at 0x404EF4). */
        /* Trace-stage labels match the Frida reference:
         *   pre_physics = UpdateRaceActors.onEnter      @ 0x42B7C7
         *   post_physics = ResolveVehicleContacts.onEnter @ 0x42BA0C
         *     (after per-actor dynamics, before inter-actor contact resolve)
         *   post_ai = ResolveVehicleContacts.onLeave    @ 0x42BA0C
         *     (after dynamics + contact resolve, the first point the
         *      original binary's per-tick post-physics state is stable)
         *
         * Port's td5_physics_tick already runs per-actor dynamics followed
         * by inter-actor contact resolution in one call — there is no
         * split point to emit a distinct post_physics. For now post_physics
         * and post_ai emit at the same point (after td5_physics_tick),
         * both carrying the same actor state; the comparator keys by
         * (sim_tick, stage, kind, id) so having both labels means /diff-race
         * with either --stage post_ai or --stage post_physics lines up
         * with the reference.
         *
         * Previously port emitted "post_ai" BEFORE physics, which made
         * /diff-race --stage post_ai (or the default post_progress via
         * upstream propagation) show all AI slots with vel=0 at sim_tick=1
         * while the reference had the full post-tick state. */
        td5_game_trace_stage("pre_physics", ticks_this_frame);
        td5_ai_tick();

        /* [S26 2026-06-05] Race-end auto-brake for the HUMAN player(s). Once the
         * finish line triggers the end transition (race_end_fade_state > 0), the
         * human loses control and the car brakes itself to a stop. The normal
         * per-slot input update (td5_input_update_player_control, gated on
         * s_slot_state.state==1) STOPS for a slot the moment it finishes, so the
         * brake must be forced here instead — AFTER td5_ai_tick (so a finished
         * slot that gets AI-coasted is overridden) and BEFORE td5_physics_tick
         * (so the brake is integrated THIS tick). Human slots only: an AI-driven
         * slot 0 (attract mode) and AI co-op slots keep driving. */
        /* [MP per-viewport finish 2026-06-13] Brake a human slot to a stop
         * EITHER when the whole race is ending (global fade) OR the moment THAT
         * player personally finishes — so a finished split-screen player coasts
         * to a halt in their own pane while the others keep racing. A finished
         * slot's state flips to 2, so td5_input_update_player_control already
         * stopped feeding it input (it coasts); forcing the brake here brings it
         * to rest instead of drifting on momentum. */
        {
            int hp = g_td5.num_human_players;
            if (hp < 1) hp = 1;
            if (hp > TD5_MAX_RACER_SLOTS) hp = TD5_MAX_RACER_SLOTS;
            for (int fb = 0; fb < hp; fb++) {
                /* [DEMO FIX #3] An AI-driven slot 0 keeps driving and is never
                 * force-braked: player_is_ai (debug) OR the attract demo, which
                 * drops slot 0 to state==0 above. Mirrors the original, where
                 * slot[0].state = 1-(demo|benchmark) makes the demo car AI. */
                if (fb == 0 && (g_td5.ini.player_is_ai || s_demo_mode)) continue;  /* attract slot keeps driving */
                if (fb > 0 && g_td5.ini.others_ai)     continue;  /* AI-driven co-op slots */
                if (g_td5.race_end_fade_state == 0 &&
                    s_slot_state[fb].companion_1 == 0) continue;  /* still racing */
                TD5_Actor *fa = td5_game_get_actor(fb);
                if (!fa) continue;
                uint8_t *fp = (uint8_t *)fa;
                *(int32_t *)(fp + 0x30C) = 0;   /* steering_command centred */
                *(int16_t *)(fp + 0x33E) = 0;   /* throttle off */
                fp[0x36D] = 1;                  /* brake_flag on (byte boolean) */
                fp[0x36E] = 0;                  /* handbrake off */
            }
        }

        td5_physics_tick();

        /* [#10 race telemetry] Accumulate per-racer summary metrics for this
         * LIVE race sim tick (top/avg speed, collisions, air time, drifts).
         * Placed here — after the live td5_physics_tick(), inside the
         * deterministic sub-tick loop and PAST the countdown/pause `continue`
         * guards above — so it runs exactly once per genuine race tick from
         * replicated state only (lockstep- and replay-deterministic). The
         * countdown-boundary td5_physics_tick() call does NOT accumulate (cars
         * are stationary during the count-in). Cheap; runs regardless of the
         * summary-screen knob so the data is always available for A/B. */
        td5_physics_accumulate_metrics();

        /* [ARCADE 2026-06-26] Power-up pickups + effect timers + hazard spinouts.
         * Runs once per genuine race tick AFTER physics (so positions are settled),
         * inside the deterministic sub-tick loop. No-op in SIMULATION mode. */
        td5_arcade_tick();

        td5_game_trace_stage("post_physics", ticks_this_frame);
        td5_game_trace_stage("post_ai", ticks_this_frame);

        /* Wanted mode: decay target-tracker marker per sub-tick
         * (DecayTrackedActorMarkerIntensity @ 0x43D7E0) */
        if (g_td5.wanted_mode_enabled) {
            tick_wanted_target_tracker();
        }

        td5_game_trace_stage("post_track", ticks_this_frame);

        /* --- VFX tick (tire tracks, particle lifetimes) --- */
        td5_vfx_tick();

        /* --- Weather density zoning (per active view) ---
         * Orig UpdateAmbientParticleDensityForSegment @ 0x004464B0, called in
         * RunRaceFrame's sim phase right after the general particle update
         * (UpdateRaceParticleEffects), once per view, unrolled @ 0x0042ba3c /
         * 0x0042ba5e. Walks the LEVELINF density-pair table keyed on the actor's
         * current span (+0x80) and ramps the active raindrop count +/-1 per
         * sub-tick toward the per-span target, so rain fades in/out as the
         * player drives through rain zones. Inside the per-sub-tick loop to
         * match the original's cadence. */
        {
            int wview = g_td5.viewport_count > 0 ? g_td5.viewport_count : 1;
            if (wview > 2) wview = 2;
            for (int wv = 0; wv < wview; wv++) {
                TD5_Actor *wa = td5_game_get_actor(g_actorSlotForView[wv]);
                if (wa) td5_vfx_update_ambient_density(wa, wv);
            }
        }

        /* --- Per-actor tire-track emitter dispatch moved to render path ---
         * Original: UpdateTireTrackEmitterDispatch @ 0x43FAE0 has a SINGLE
         * caller — RenderRaceActorForView @ 0x0040C120 (LAB_0040c7ba). Sim
         * tick only runs UpdateTireTrackPool() (per-slot intensity decay),
         * NOT the per-actor emit step. Dispatch now happens in
         * td5_render_actors_for_view, gated per view, so smoke spawns into
         * the correct per-view particle bank.
         * [CONFIRMED @ 0x40C120 + 0x43FAE0 function_callers]. */

        /* --- Update race order (bubble sort by span position) --- */
        update_race_order();

        /* --- Per-viewport camera update (post-physics) ---
         * Must run AFTER physics + AI + track + update_race_order,
         * matching UpdateChaseCamera near the tail of RunRaceFrame's
         * sim-tick loop (0x0042B580). See cache_angles call earlier
         * in this loop for the full rationale. */
        td5_camera_solve_tick_all();
        td5_game_trace_stage("post_camera", ticks_this_frame);

        /* --- Per-actor wrap normalization ---
         * [BUGFIX 2026-05-26 traffic-stale-span-norm] Bound at active actor
         * count (12 with traffic), not TD5_MAX_RACER_SLOTS (6). Orig
         * RunRaceFrame @ 0x0042B580 normalizes slots 0..g_racerCount-1 every
         * sub-tick. Capping at 6 left traffic span_normalized frozen → render
         * span-cull dropped car bodies (shadow-only) + off-road minimap dots. */
        {
            int norm_count = td5_game_get_total_actor_count();
            for (i = 0; i < norm_count; i++) {
                TD5_Actor *lap_actor;
                /* s_slot_state only covers racer slots 0..5; traffic
                 * slots 6-11 are never state==3 during normal play. */
                if (i < TD5_MAX_RACER_SLOTS && s_slot_state[i].state == 3)
                    continue; /* disabled */
                lap_actor = td5_game_get_actor(i);
                if (lap_actor) {
                    /* Track owns span normalization only. Race progression stays here. */
                    td5_track_normalize_actor_wrap(lap_actor);
                }
            }
        }

        /* --- Per-actor race progression (sub-tick cadence) ---
         *
         * Original (0x00406650 UpdateVehicleActor) calls
         *   AccumulateVehicleSpeedBonusScore (0x0040A3D0) — per-tick, gated on Ultimate
         *   AdvancePendingFinishState        (0x0040A2B0) — per-tick, decrements +0x344
         *   DecayUltimateVariantTimer        (0x0040A440) — per-tick from contact paths
         * from inside UpdateRaceActors (which itself runs from the sub-tick loop of
         * RunRaceFrame). Only the checkpoint-scan & finish-state promotion
         * (CheckRaceCompletionState @ 0x00409E80) runs once per frame — see the
         * pre-sub-tick call site in td5_game_run_race_frame for that.
         *
         * accumulate_speed_bonus contains a (simulation_tick_counter & 3) == 0
         * gate that models the original's per-sub-tick throttle at 0x00409E24;
         * this gate stays valid because the counter still advances per sub-tick. */
        for (i = 0; i < TD5_MAX_RACER_SLOTS; i++) {
            if (s_slot_state[i].state == 3) continue; /* disabled */
            /* Race timer: matches UpdateVehicleActor @ 0x00406650 +0x34c
             * increment — once per sub-tick, gated on actor+0x328 (finish).
             * Sub-tick loop only runs with paused=0, so the countdown gate
             * is implicit (the countdown path has its own `continue`). */
            if (s_metrics[i].post_finish_metric_base == 0) {
                s_metrics[i].cumulative_timer++;
            }
            tick_pending_finish_timer(i);
            accumulate_speed_bonus(i);
            decay_ultimate_timer(i);
            sync_actor_race_metrics(i);
        }

        /* --- Per-non-paused-tick world animation advances ---
         * [FPS-DECOUPLE 2026-06-07] Sky rotation, billboard strobe, tracked-
         * marker phase and fog fade run once per non-paused sim tick — matching
         * the original's "per non-paused tick" cadence (AdvanceGlobalSkyRotation
         * @0x43D7C0). They were previously called from the per-viewport render
         * loop, which advanced them per rendered frame *and* per viewport, so the
         * sky spun ~2x too fast at 60 fps single-screen and worse at higher
         * refresh / in split-screen. On the fixed 30 Hz tick they are frame-rate-
         * and viewport-count-independent. */
        if (!g_td5.paused) {
            td5_render_advance_sky_rotation();
            td5_render_advance_billboard_anims();
            td5_vfx_advance_tracked_marker_phases();
            td5_render_per_tick_fog_fade();
            /* [TRAFFIC BATTLE checkpoints] Advance the deadline chaser once per
             * fixed sim tick (frame-rate-independent + lockstep-safe). No-op
             * unless the CHECKPOINTS win condition armed it. */
            battle_chase_tick();
        }

        /* --- Consume one tick --- */
        g_td5.sim_time_accumulator -= TD5_TICK_ACCUMULATOR_ONE;
        ticks_this_frame++;
        /* post_progress emits with pre-increment counter — matches Frida's
         * UpdateRaceCameraTransitionTimer.onEnter @ 0x42BA3A, which fires
         * BEFORE the counter bump at 0x42BAA1. */
        td5_game_trace_stage("post_progress", ticks_this_frame);
        g_td5.simulation_tick_counter++;
    }

    /* Compute sub-tick interpolation fraction for camera/VFX rendering.
     * Original (0x0042b709): fraction is NOT recomputed when paused.
     * [S31] A net-synced REMOTE pause freezes it too -- otherwise the
     * non-pausing machine's body extrapolation + camera kept gliding on a
     * frozen sim ("pause screen on the other computer isn't fully frozen"). */
    if (!s_pause_menu_active && !(net_lockstep && s_net_pause_round)) {
        g_subTickFraction = (float)g_td5.sim_time_accumulator / (float)TD5_TICK_ACCUMULATOR_ONE;
        if (g_subTickFraction < 0.0f) g_subTickFraction = 0.0f;
        if (g_subTickFraction > 1.0f) g_subTickFraction = 1.0f;
        TD5_LOG_D(LOG_TAG, "subTickFraction=%.4f accum=0x%X ticks=%d",
                  g_subTickFraction, g_td5.sim_time_accumulator, ticks_this_frame);

        /* Sub-tick render-position interpolation: lerp every actor's
         * prev_world_pos -> world_pos by g_subTickFraction and write into
         * actor->render_pos. The body-mesh draw at td5_render.c:1786 uses
         * its own velocity-extrapolation (faithful to original 0x40C164),
         * but the camera, HUD, shadows, smoke, and audio all read
         * actor->render_pos -- without this pass they stay snapped at the
         * last sim-tick position while the body mesh slides forward,
         * producing the rubber-band wobble at render rates above 30 Hz. */
        td5_physics_apply_render_interpolation(g_subTickFraction);

        /* Re-finalize chase camera position with the freshly computed
         * subtick fraction. Without this call, at render rates above the
         * 30 Hz sim tick rate (i.e. any frame where the fixed-tick loop
         * ran 0 times), g_camWorldPos is stale from the previous tick
         * while the car-mesh render extrapolates world_pos by vel*subtick
         * each frame — producing sawtooth shake that scales with speed.
         * td5_camera_finalize_all() re-writes g_camWorldPos using the
         * latest orbit state + actor pose + current subtick, matching
         * td5_render.c:1530-1537. */
        td5_camera_finalize_all();
    }

    if ((g_td5.simulation_tick_counter % 60u) == 0u) {
        TD5_LOG_D(LOG_TAG,
                  "Race frame timing: norm_dt=%.3f fps=%.2f ticks_this_frame=%d paused=%d pause_menu=%d countdown_indicator=%d countdown_timer=0x%X fade=%d",
                  g_td5.normalized_frame_dt,
                  g_td5.instant_fps,
                  ticks_this_frame,
                  g_td5.paused,
                  s_pause_menu_active,
                  s_race_countdown_state,
                  g_cameraTransitionActive,
                  g_td5.race_end_fade_state);
        {
            TD5_Actor *actor0 = td5_game_get_actor(0);
            if (actor0) {
                uint8_t *a0 = (uint8_t *)actor0;
                TD5_LOG_D(LOG_TAG,
                          "Race actor0: pos=(%d,%d,%d) speed=%d gear=%d",
                          *(int32_t *)(a0 + 0x1CC),
                          *(int32_t *)(a0 + 0x1D0),
                          *(int32_t *)(a0 + 0x1D4),
                          *(int32_t *)(a0 + 0x208),
                          *(int32_t *)(a0 + 0x224));
            }
        }
    }

    /* ---- Per-tick fog fade now runs inside the fixed-step sim loop (see the
     *      "Per-non-paused-tick world animation advances" block) so it advances
     *      at 30 Hz instead of per rendered frame. ---- */

    /* ---- Split-screen steering balance ---- */
    td5_game_update_split_screen_balance();

    /* ---- Rendering pipeline ---- */

    /* Begin scene */
    td5_render_begin_scene();

    /* Clear backbuffer once before any viewport renders.
     * Moved out of td5_render_actors_for_view so the split-screen P2 pass
     * does not wipe P1's already-rendered half.
     * [RE: RunRaceFrame 0x42B580 — single BeginScene/EndScene pair, one clear] */
    /* Photo booth clears to chroma BLUE88 (0,0,88) so the menu can color-key the
     * car out of the captured preview; normal race uses the sky-blue clear. */
    td5_plat_render_clear(td5_render_photobooth_active() ? 0xFF000058u : 0xFF4080C0u);

    /* [DYNAMIC LIGHTS] Rebuild the world-space dynamic-light registry once per
     * frame (before any viewport's lighting pass; lights are shared across all
     * split-screen panes). Headlights move with each car. No-op when the
     * lighting system or the headlight emitter is disabled. */
    td5_light_begin_frame();
    /* [LIGHT2 P0] Enable + clear the normal/material G-buffer for this frame
     * (Mode>=1); z-writing world draws populate it and the per-pane light pass
     * applies proper N.L. No-op (off) in Mode 0. */
    td5_render_lighting2_frame_begin();
    /* [AUTO LIGHTS] Decide whether the environment is poorly lit. In AUTO mode
     * (default), non-clear weather / dark tracks get the full headlight treatment:
     * the beams turn on AND the scene is dimmed so they read strongly — the look
     * approved manually, applied automatically. Bright daylight stays untouched.
     * The manual [Lighting] DarkMode always forces it on. */
    {
        int manual_dark = g_td5.ini.light_dark_mode;
        int env_dark;
        if (td5_light_auto()) {
            env_dark = td5_render_env_is_dark() || manual_dark;
            td5_render_set_dark_mode(env_dark);
        } else {
            env_dark = manual_dark;   /* dimming stays as configured at startup */
        }
        td5_light_set_env_dark(env_dark);
    }
    if (!td5_render_photobooth_active())
        td5_light_emit_vehicle_headlights();
    /* [LIGHT2] Street lamps: promote the nearest registered lamp fixtures to
     * real point lights (env-dark gated, same verdict as auto headlights). */
    if (!td5_render_photobooth_active())
        td5_light_emit_street_lamps();

    /* [S01 2026-06-04] Live window resize: if the render dimensions changed
     * (drag-resize / maximize, applied in the platform layer's WM_SIZE handler),
     * recompute the per-viewport rectangles so the camera projection, scissor
     * and HUD layout follow the new client size. Cheap per-frame compare; the
     * relayout only runs on an actual change. */
    {
        static int s_last_vp_w = 0, s_last_vp_h = 0;
        if (g_td5.render_width != s_last_vp_w || g_td5.render_height != s_last_vp_h) {
            td5_game_init_viewport_layout();              /* 3D viewport rects + projection input */
            td5_hud_init_layout();              /* HUD/minimap layout (reads viewport_count) */
            s_last_vp_w = g_td5.render_width;
            s_last_vp_h = g_td5.render_height;
        }
    }

    /* [Phase B Stage 2b] Decide whether to record panes on worker threads.
     * Gated: dev-only [Render] ThreadedPanes, >2 panes, a live worker pool, the
     * per-pane g_rs + deferred-context pools available, AND past a 2-frame warmup
     * (so the first race frames populate the lazy set-once atlas/mesh caches on
     * the main thread before any worker reads them). Otherwise: serial path. */
    int mt_threaded = 0;
    {
        static int s_mt_warm = 0;
        static int s_mt_diag = 0;
        int n = g_td5.viewport_count;
        if (g_td5.ini.threaded_panes && n > 2 && td5_jobs_worker_count() > 0) {
            if (s_mt_warm >= 2) {
                int rs_ok = td5_render_scratch_pool_ensure(n);
                int cl_ok = rcmd_pool_ensure(n);
                if (rs_ok && cl_ok) mt_threaded = 1;
                if (!s_mt_diag) {
                    TD5_LOG_I("render", "ThreadedPanes gate: tp=%d n=%d workers=%d warm=%d rs_pool=%d cl_pool=%d -> %s",
                              g_td5.ini.threaded_panes, n, td5_jobs_worker_count(), s_mt_warm,
                              rs_ok, cl_ok, mt_threaded ? "THREADED" : "SERIAL");
                    s_mt_diag = 1;
                }
            } else {
                s_mt_warm++;
            }
        } else {
            s_mt_warm = 0;
            if (!s_mt_diag && g_td5.viewport_count > 0) {
                TD5_LOG_W("render", "ThreadedPanes gate(off): tp=%d n=%d workers=%d",
                          g_td5.ini.threaded_panes, n, td5_jobs_worker_count());
                s_mt_diag = 1;
            }
        }
    }

    if (mt_threaded) {
        int n = g_td5.viewport_count;
        static int s_mt_logged = 0;
        if (!s_mt_logged) {
            TD5_LOG_I("render", "ThreadedPanes ENGAGED: %d panes recorded on %d workers",
                      n, td5_jobs_worker_count());
            s_mt_logged = 1;
        }

        /* Phase 1 (serial, main): per-pane camera + projection, baked into each
         * pane's g_rs instance. Serial because camera_apply_view writes the
         * camera-global g_depthFovFactor (read while baking the frustum). */
        for (int vp = 0; vp < n; vp++) {
            td5_render_scratch_bind(vp);
            td5_camera_apply_view(vp);
            td5_render_bake_camera();   /* store view vp's camera into g_rs[vp] this frame */
            td5_render_configure_projection(s_viewports[vp].w, s_viewports[vp].h);
        }
        td5_render_scratch_unbind();
        /* From here, render uses each pane's baked camera (don't re-read the shared
         * camera-module "current", which now holds only the last pane's view). */
        td5_render_set_camera_prebaked(1);

        /* Phase 2 — BUILD each pane's world into its CPU command list.
         * [validation step] SERIAL on the main thread for now: if record+replay
         * renders identically to the plain serial path, the command list is
         * correct, and only this loop swaps to td5_jobs_parallel_for to thread it. */
#ifdef TD5_MT_PARALLEL_BUILD
        td5_jobs_parallel_for(n, mt_build_pane_rcmd, NULL);
#else
        for (int vp = 0; vp < n; vp++)
            mt_build_pane_rcmd(vp, NULL);
#endif

        /* Phase 3 — REPLAY each pane's command list in order on the immediate
         * context (live GPU). g_rs back to the default instance for the live
         * texture-cache/bind path; geometry is self-contained in each list. */
        td5_render_scratch_unbind();
        for (int vp = 0; vp < n; vp++)
            td5_rcmd_replay(s_pane_cmd[vp]);

        /* Phase 4 (serial, main): VFX + HUD per pane on the immediate context,
         * with g_rs bound to the pane instance so VFX reads its camera/projection
         * and refills/flushes its (now-drained) translucent buckets. */
        for (int vp = 0; vp < n; vp++) {
            td5_render_scratch_bind(vp);
            td5_plat_render_set_viewport(s_viewports[vp].x, s_viewports[vp].y,
                                         s_viewports[vp].w, s_viewports[vp].h);
            td5_plat_render_set_clip_rect(s_viewports[vp].x, s_viewports[vp].y,
                                          s_viewports[vp].x + s_viewports[vp].w,
                                          s_viewports[vp].y + s_viewports[vp].h);
            td5_render_set_race_pass(TD5_RACE_PASS_OPAQUE);
            td5_render_set_fog(1);

            if (g_td5.ini.debug_collisions) {
                TD5_Actor *wire_actor = td5_game_get_actor(g_actorSlotForView[vp]);
                if (wire_actor) {
                    int wire_span = *(int16_t *)((uint8_t *)wire_actor + 0x80);
                    td5_render_debug_lines_reset();
                    td5_track_debug_emit_collision_lines(wire_span, 40);
                    td5_render_debug_lines_flush();
                }
            }

            /* [task#14] Visible TD6 breakable-prop boxes — solid scene geometry,
             * drawn after track + actors, before the translucent VFX. */
            if (!td5_render_photobooth_active())
                render_td6_props(td5_game_get_actor(g_actorSlotForView[vp]));

            if (!td5_render_photobooth_active()) {
                td5_vfx_render_tire_tracks();
                td5_vfx_render_tire_marks();
                {
                    TD5_Actor *wa = td5_game_get_actor(g_actorSlotForView[vp]);
                    if (wa) td5_vfx_render_ambient_streaks(wa, g_td5.sim_tick_budget, vp);
                }
                td5_vfx_draw_particles(vp);
                td5_render_arcade_pads();   /* [ARCADE] glowing power-up pads + hazards */
            }
            td5_render_flush_translucent();
            td5_render_flush_projected_buckets();

            td5_render_set_race_pass(TD5_RACE_PASS_OPAQUE);
            td5_render_set_fog(0);
            if (!td5_render_photobooth_active())
                td5_hud_draw_status_text(vp, vp);
        }
        td5_render_scratch_unbind();
        td5_render_set_camera_prebaked(0);   /* restore normal per-frame camera refresh */
    } else
    /* For each viewport: camera setup, sky, track, actors, vfx, hud */
    for (int vp = 0; vp < g_td5.viewport_count; vp++) {
        /* Set viewport rectangle + scissor.
         * Viewport maps NDC to screen-space; scissor prevents geometry from
         * one half bleeding into the other. ScissorEnable=TRUE is set globally
         * in the D3D11 rasterizer state so RSSetScissorRects is always active.
         * [RE: SetProjectedClipRect per view @ InitializeRaceViewportLayout 0x42C2B0] */
        td5_plat_render_set_viewport(
            s_viewports[vp].x, s_viewports[vp].y,
            s_viewports[vp].w, s_viewports[vp].h);
        td5_plat_render_set_clip_rect(
            s_viewports[vp].x, s_viewports[vp].y,
            s_viewports[vp].x + s_viewports[vp].w,
            s_viewports[vp].y + s_viewports[vp].h);

        /* Camera transition state */
        {
            static int s_cam_debug_logged = 0;
            TD5_Actor *actor = td5_game_get_actor(g_actorSlotForView[vp]);

            if (!actor && g_actor_table_base) {
                int slot = g_actorSlotForView[vp];
                int total = td5_game_get_total_actor_count();

                if (slot >= 0 && slot < total) {
                    actor = (TD5_Actor *)(g_actor_table_base + (size_t)slot * TD5_ACTOR_STRIDE);
                }
            }

            if (!s_cam_debug_logged) {
                TD5_LOG_I(LOG_TAG,
                          "Camera first frame: actor=%p base=%p count=%d slot=%d",
                          (void *)actor,
                          (void *)g_actor_table_base,
                          td5_game_get_total_actor_count(),
                          g_actorSlotForView[vp]);
                s_cam_debug_logged = 1;
            }
        }
        td5_camera_apply_view(vp);

        /* Configure projection for this viewport */
        td5_render_configure_projection(s_viewports[vp].w, s_viewports[vp].h);

        /* ---- Pass 0: SKY ---- */
        td5_render_set_race_pass(TD5_RACE_PASS_SKY);
        td5_render_set_fog(0);  /* fog off for sky */
        /* Sky rotation / billboard strobe / tracked-marker phase advance moved
         * into the fixed-step sim loop (30 Hz, once per non-paused tick, once
         * per frame regardless of viewport count) — see the
         * "Per-non-paused-tick world animation advances" block above. */

        /* ---- Pass 1: OPAQUE (world + track + actors) ---- */
        td5_render_set_race_pass(TD5_RACE_PASS_OPAQUE);
        td5_render_set_fog(1);  /* fog on for world geometry */

        /* Enable deferred additive capture. Type-3 (streetlight / glow)
         * batches emitted during this pass are copied into a side buffer
         * instead of being drawn immediately, so they can be composited
         * on top of all opaque geometry (including alpha-keyed trees)
         * after the world pass finishes. */
        td5_render_begin_world_pass();
        td5_profile_mark("v_setup");   /* [perf probe] camera+projection+pass setup */

        /* Render race actors for this view */
        td5_render_actors_for_view(vp);  /* emits v_track (sky+terrain) internally */
        td5_profile_mark("v_actors");  /* [perf probe] per-view actor (car) draws */

        /* Debug: collision-wireframe overlay (F12 / [Debug] Collisions /
         * --DebugCollisions). Drawn after opaque terrain + actors so the depth
         * buffer occludes hidden rails, and before translucent VFX. Centered on
         * the raw current span (+0x80) of the actor this viewport follows. */
        if (g_td5.ini.debug_collisions) {
            TD5_Actor *wire_actor = td5_game_get_actor(g_actorSlotForView[vp]);
            if (wire_actor) {
                int wire_span = *(int16_t *)((uint8_t *)wire_actor + 0x80);
                td5_render_debug_lines_reset();
                td5_track_debug_emit_collision_lines(wire_span, 40);
                /* [task#14] mark nearby TD6 breakable props (magenta poles) so
                 * their invisible collision volumes are visible for testing. */
                td5_track_debug_emit_prop_markers(
                    (float)wire_actor->world_pos.x * (1.0f / 256.0f),
                    (float)wire_actor->world_pos.y * (1.0f / 256.0f),
                    (float)wire_actor->world_pos.z * (1.0f / 256.0f),
                    40000.0f);
                td5_render_debug_lines_flush();
            }
        }

        /* [task#14] Visible TD6 breakable-prop boxes — solid scene geometry,
         * after track + actors, before translucent VFX (single-view path). */
        if (!td5_render_photobooth_active())
            render_td6_props(td5_game_get_actor(g_actorSlotForView[vp]));

        /* [LIGHT2 P2] Sun-shadow pass FIRST (multiplicative darkening of the
         * opaque world), then the additive light pass — this order keeps
         * headlight pools from being darkened by the shadow term. */
        if (!td5_render_photobooth_active())
            td5_render_apply_shadow_pass(s_viewports[vp].x, s_viewports[vp].y);

        /* [DEFERRED LIGHTS] Screen-space dynamic-light pass: now that the opaque
         * world (track + actors + props) has filled the depth buffer for this
         * pane, add the headlight/light contribution per pixel before the
         * translucent VFX + HUD. */
        if (!td5_render_photobooth_active())
            td5_render_apply_light_pass(s_viewports[vp].x, s_viewports[vp].y);

        /* [LIGHT2 P3] Screen-space reflections LAST of the deferred passes so
         * they mirror the lit + shadowed scene (car paint, glass, wet roads). */
        if (!td5_render_photobooth_active())
            td5_render_apply_ssr_pass(s_viewports[vp].x, s_viewports[vp].y);

        /* VFX: tire tracks, particles */
        if (!td5_render_photobooth_active()) {
            td5_vfx_render_tire_tracks();
            td5_vfx_render_tire_marks();
            /* Weather rain streaks — orig RenderAmbientParticleStreaks @ 0x00446560,
             * called per view in RunRaceFrame's draw phase AFTER the actors + tire
             * tracks and BEFORE the general particle draw (order @ 0x0042be9a:
             * RenderRaceActorsForView -> RenderTireTrackPool -> RenderAmbient-
             * ParticleStreaks -> DrawRaceParticleEffects). Only rain renders (snow
             * gated off, faithful). sim_budget = port's normalized per-frame sim
             * budget (frame_dt*30), the analog of orig (int)g_simTickBudget. */
            {
                TD5_Actor *wa = td5_game_get_actor(g_actorSlotForView[vp]);
                if (wa) td5_vfx_render_ambient_streaks(wa, g_td5.sim_tick_budget, vp);
            }
            td5_vfx_draw_particles(vp);
            td5_render_arcade_pads();   /* [ARCADE] glowing power-up pads + hazards */
        }
        td5_profile_mark("v_vfx");     /* [perf probe] per-view tire/streak/particle draws */
        td5_render_flush_translucent();
        td5_render_flush_projected_buckets();

        /* Now that all opaque world geometry has depth-written, composite
         * the deferred additive lights on top. Fog stays on — lights
         * follow the same fog the world does. */
        td5_render_flush_deferred_additive();
        td5_profile_mark("v_flush");   /* [perf probe] translucent/projected/additive flushes */

        /* ---- Pass 3: ALPHA (overlay effects) ---- */
        td5_render_set_race_pass(TD5_RACE_PASS_ALPHA);

        /* ---- Pass 1 again: HUD overlays ---- */
        td5_render_set_race_pass(TD5_RACE_PASS_OPAQUE);
        td5_render_set_fog(0);  /* fog off for HUD */

        /* HUD overlay for this viewport */
        if (!td5_render_photobooth_active())
            td5_hud_draw_status_text(vp, vp);
        td5_profile_mark("v_hud");     /* [perf probe] per-view HUD status text */
        /* NOTE: minimap is drawn from td5_hud_render_overlays (below).
         * A duplicated call here used to be a no-op (set_clip_rect was a stub),
         * but once 65a4fea wired hardware scissor, it left the minimap rect
         * active across the speedo/tach/digit/countdown draws that follow in
         * render_overlays — clipping every HUD element on non-circuit tracks. */
    }

    /* Reset viewport and scissor to full-screen before HUD overlays.
     * After the per-viewport loop the last viewport's clip rect is still active;
     * HUD elements (speedo, divider, minimap) must render over the full screen.
     * [RE: post-render SetProjectedClipRect(fullscreen) @ RunRaceFrame 0x42B580] */
    {
        int fw = g_td5.render_width;
        int fh = g_td5.render_height;
        td5_plat_render_set_viewport(0, 0, fw, fh);
        td5_plat_render_set_clip_rect(0, 0, fw, fh);
    }

    /* Full-screen HUD overlay (speedometer, lap counter, etc.) */
    if (!td5_render_photobooth_active())
        td5_hud_render_overlays(g_td5.normalized_frame_dt);

    /* [PORT 2026-06-08] Per-viewport player identity: coloured frame in each
     * player's accent colour + a name plate under the car. Self-gated (only when
     * the MP frontend set identities and the race is split). */
    td5_hud_draw_player_id_overlays();

    /* [PORT 2026-06-08] Per-viewport player identity: coloured frame in each
     * player's accent colour + a name plate under the car. Self-gated (only when
     * the MP frontend set identities and the race is split). */
    td5_hud_draw_player_id_overlays();

    /* [ARCADE 2026-06-26] Per-viewport active power-up chip (label + timer bar).
     * Self-gated: no-op unless the race is in ARCADE mode with an active effect. */
    td5_hud_draw_arcade_chips();

    /* [TRAFFIC BATTLE 2026-06-28] Per-viewport "WRECKS N" tally. Self-gated:
     * no-op unless the Traffic Destruction battle mode is active. */
    td5_hud_draw_battle_wrecks();
    /* [CAR DAMAGE 2026-06-29 split-screen fix] Per-viewport car-health bar
     * (top-left of each pane). Drawn HERE in the full-screen overlay pass -- NOT
     * in the per-pane status-text loop above -- so the screen-space bar quads map
     * into every split-screen pane instead of being clipped to one offset
     * viewport. Self-gated (no-op when the DAMAGE BAR toggle is off). */
    td5_hud_draw_damage_bars();
    /* [LANE ASSIST 2026-06-28] Per-viewport "LANE ASSIST" indicator (on/off +
     * steering-direction arrow). Self-gated: no-op unless the optional aid is
     * enabled for the pane's player. */
    td5_hud_draw_laneassist_indicator();

    /* [S27] Controller-disconnect modal: a semi-transparent "reconnect" panel
     * over each disconnected player's split-screen viewport. Self-gated (no-op
     * unless a controller is currently missing). Drawn on top of the HUD. */
    td5_hud_draw_disconnect_overlays();
    td5_hud_draw_net_pause_overlay();

    /* [PORT 2026-06] First-race controller-tutorial overlay. update() polls for
     * a dismiss press (releasing the countdown next tick); draw() renders the
     * dim + controller diagram. Both self-gate to no-ops unless the overlay was
     * armed at race init. Drawn over the HUD but under the pause menu. */
    td5_tutorial_update();
    td5_tutorial_draw();

    /* Pause overlay drawn LAST among the in-race overlays so the menu (BLACKBOX
     * panel + selbox + sliders + text) sits on top of everything else — the HUD,
     * the per-viewport coloured player-identity borders/name plates, and the
     * disconnect modal — instead of those bleeding over the menu. (The race-end
     * fade below is the leaving-transition wipe and intentionally stays on top.) */
    if (s_pause_menu_active) {
        td5_hud_draw_pause_overlay();
        /* [END RACE NOW 2026-06-30] Force-finish YES/NO confirmation, drawn on
         * top of the pause panel. Self-gated: no-op unless the prompt is armed. */
        td5_hud_draw_endrace_confirm();
        /* [PAUSE CONFIRM 2026-07-02] RESTART/QUIT/EXIT YES/NO confirmation
         * (local split-screen MP only). Self-gated: no-op unless armed. */
        td5_hud_draw_pause_action_confirm();
    }

    /* Race end fade: directional wipe overlay (black bars closing in).
     * [CONFIRMED @ 0x0042b791/0x0042b797 RunRaceFrame] The directional fade and
     * the 1st-place victory star pulse are MUTUALLY EXCLUSIVE: orig CMPs
     * g_raceEndRadialPulseEnabled (0x4aaefc) and the JNZ jumps over the entire
     * directional-fade (SetClipBounds) block when the pulse is enabled. So a
     * 1st-place finish shows the star wipe ONLY; any other finish shows the
     * directional fade ONLY. The port previously drew BOTH -> "star + fade at
     * the same time" on a win. */
    if (g_td5.race_end_fade_state > 0 && !s_race_end_radial_pulse_enabled) {
        td5_hud_draw_race_fade(s_fade_accumulator, g_td5.fade_direction);
    }

    /* Finishing-position digit is now drawn INSIDE td5_hud_render_overlays (in the
     * centered render state, alongside the star pulse). Drawing it here picked up
     * a leftover viewport/clip offset and landed it off-centre. The captured place
     * is exposed via td5_game_get_finish_position() below. */

    td5_hud_flush_text();

    /* ---- Audio tick ---- */

    /* Feed camera position into the sound system as listener position.
     * g_camWorldPos is in 24.8 fixed-point, which is the same coordinate
     * space td5_sound expects (matching actor world_pos). NOTE: the camera
     * solve only fills viewports 0-1; the mixer's per-car volume uses the human
     * players' CAR positions directly (td5_sound_update_audio_mix) so panes 2+
     * being unset here no longer silences their cars. */
    for (int vp = 0; vp < g_td5.viewport_count && vp < TD5_MAX_VIEWPORTS; vp++) {
        td5_sound_set_listener_pos(vp,
            g_camWorldPos[vp][0],
            g_camWorldPos[vp][1],
            g_camWorldPos[vp][2]);
    }

    /* Feed per-vehicle gear state into the sound system (engine/horn vol LUT).
     * The tyre screech is NOT plumbed from here: the original 0x440B00 D1 block
     * reads the slip-excess (+0x31C/+0x320) directly off the actor, so the port's
     * mixer reads it directly too -- no skid-intensity feed. */
    for (i = 0; i < TD5_MAX_RACER_SLOTS; i++) {
        if (s_slot_state[i].state == 3) continue;
        TD5_Actor *actor_snd = td5_game_get_actor(i);
        if (!actor_snd) continue;
        uint8_t *a = (uint8_t *)actor_snd;

        /* Gear state (offset 0x224) -- used for engine/horn volume table lookup */
        int gear = *(int32_t *)(a + 0x224);
        td5_sound_set_gear_state(i, gear);
    }

    for (i = 0; i < TD5_MAX_RACER_SLOTS; i++) {
        if (s_slot_state[i].state != 3) {
            td5_sound_update_vehicle_looping_state(i);
        }
    }
    /* [POLICE rewrite] Drive the cop-chase siren from the nearest actively-
     * chasing traffic cop (gated by the POLICE option), distance-attenuated to
     * the local camera. Cosmetic / post-sim — no netplay effect. Silenced while
     * the pause menu is up (the sim is frozen — no siren on pause). */
    if (s_pause_menu_active)
        td5_sound_stop_tracked_vehicle_audio();
    else
        td5_sound_update_police_siren();
    td5_sound_update_audio_mix();
    /* [CONFIRMED @ 0x00440B00]: ambient weather (rain) sound runs each
     * frame after the vehicle audio mix, gated by weather particle density. */
    td5_sound_update_ambient();
    td5_sound_tick();

    /* End scene and present */
    td5_render_end_scene();
    td5_profile_mark("render");      /* all draw submission since post_progress */
    td5_plat_present(1);
    td5_profile_mark("present");     /* present + GPU drain + vsync wait */

    td5_game_trace_stage("frame_end", ticks_this_frame);
    td5_profile_end_frame();

    return 0;  /* race continues */
}

static void set_countdown_indicator_state(int value)
{
    /* [PORT: N-way split 2026-06-08] Drive the countdown (3-2-1) indicator on
     * EVERY active viewport, not just the legacy 2. With >2 split-screen panes
     * the extra panes otherwise kept the stale per-actor digit the fly-in set
     * once (UpdateCameraTransitionHudIndicator = race_position+2); for a car in
     * 8th+ place that value (>=10) indexes past the 5x2 NUMBERS atlas and drew a
     * blank/garbage "blue square" on the 9th pane. Setting all panes here makes
     * the synchronized countdown overwrite it every level. */
    int view_count = g_td5.viewport_count;

    if (view_count < 1) {
        view_count = 1;
    }
    if (view_count > TD5_MAX_VIEWPORTS) {
        view_count = TD5_MAX_VIEWPORTS;
    }

    for (int i = 0; i < view_count; i++) {
        td5_hud_set_indicator_state(i, value);
    }
}

static void reset_race_countdown(void)
{
    /* Original timer init: 0xA000 (160 ticks total, 40 per level).
     * Levels 4,3 → indicator 5,4 → blank atlas cells (not visible in original).
     * Only levels 2,1,0 → digits 3,2,1 are actually shown. */
    g_cameraTransitionActive = TD5_COUNTDOWN_INIT;
    s_race_countdown_ticks   = 0;
    s_race_countdown_state   = 0;   /* hide indicator until level 2 is reached */
    set_countdown_indicator_state(0);
    TD5_LOG_I(LOG_TAG, "Race countdown reset: timer=0x%X", g_cameraTransitionActive);
}

/* [CONFIRMED @ 0x0040a490 UpdateRaceCameraTransitionTimer; L5 promotion
 *  sweep audit 2026-05-18] -- ARCH-DIVERGENCE, same-shape state machine
 *  with three intentional port-side deltas.
 *
 *  Orig structure (37 lines decompiled at 0x0040a490):
 *    1) if timer == 0: SetRaceHudIndicatorState(0,0); SetRaceHudIndicatorState(1,0); return
 *    2) elif timer < 0x101: timer = 0;
 *                           ResetRaceCameraSelectionState(replay_or_playback ? 1 : 0)
 *    3) else: timer -= 0x100
 *    4) level = timer / 0x2800; indicator = level + 1
 *    5) SetRaceHudIndicatorState(0, indicator); SetRaceHudIndicatorState(1, indicator)
 *    6) if level == 0: g_gamePaused = 0; gRaceCameraTransitionGate = 0
 *
 *  Port deltas (this function):
 *    A) [RESOLVED 2026-05-18 Phase 2] Step (2)'s
 *       ResetRaceCameraSelectionState call IS wired at the timer-zero
 *       crossing below (clear when playback/replay, else restore) --
 *       [CONFIRMED @ 0x0040A4B4].
 *    B) Step (6)'s paused-flip happens at timer==0 in the port instead of
 *       level==0 in orig. User-observed in-game behaviour (2026-05-17)
 *       confirmed the orig's car-hold extends across the level==0 window
 *       (~40 sub-ticks) and the actual race-start moment is timer==0.
 *       Either the Ghidra disasm shows a g_gamePaused write at level==0
 *       that is gated by an unmodeled inner check, or the visible
 *       race-start is gated elsewhere. Port chooses to match the
 *       user-visible timer==0 moment. Recent commit 630d797 had matched
 *       orig literally at level==0; the v3 fix in this file restored
 *       timer==0 to match observation.
 *    C) Port maps level >= 3 (indicator >= 4) to indicator=0. Orig calls
 *       SetRaceHudIndicatorState with raw indicator value 4 or 5; those
 *       atlas cells are blank in the original asset and render garbage
 *       in the port if not suppressed. Port-only workaround for an asset
 *       difference, not a logic divergence.
 *    D) Port has indicator-change gate (only update when next != cur);
 *       orig calls SetRaceHudIndicatorState every tick. Set is
 *       idempotent so this is a no-op behaviourally; saves redundant
 *       HUD work.
 *    E) gRaceCameraTransitionGate at 0x0048306C is collapsed into the
 *       port's single g_td5.paused / g_game_paused. Both flags are set
 *       to 0 at the same race-start moment; readers in the port
 *       reference g_game_paused. Behaviour-equivalent. */
static void tick_race_countdown(void)
{
    int level, next_indicator;
    static int s_level0_logged = 0;  /* log-once gate for level==0 transition */

    /* Only the timer gate matters here. Orig UpdateRaceCameraTransitionTimer
     * @ 0x0040A490 is called every frame regardless of paused — its
     * entry-guard returns when timer == 0 and the function continues
     * decrementing during the level==0 window (paused already 0). The port
     * must mirror this so the "1" digit stays visible for the full level==0
     * window (~40 sub-ticks) and only hides when timer fully reaches 0.
     * Caller must arrange to invoke this from both paused and racing
     * branches until timer expires. */
    if (g_cameraTransitionActive <= 0) {
        return;
    }

    /* Mirror UpdateRaceCameraTransitionTimer @ 0x0040A490:
     *   if (timer < 0x101) { timer = 0; ResetRaceCameraSelectionState(...); }
     *   else                 timer -= 0x100;
     *   level = timer / 0x2800;  indicator = level + 1;
     *   if (level == 0) g_gamePaused = 0;
     *
     * Orig flips pause when LEVEL transitions to 0 (timer < 0x2800), i.e. the
     * first sub-tick where the visible indicator shows "1" — NOT when the timer
     * fully reaches 0. The previous port logic flipped pause at timer==0,
     * delaying the race start by ~40 sub-ticks (one full "level"). */
    if (g_cameraTransitionActive <= TD5_COUNTDOWN_DECR) {
        g_cameraTransitionActive = 0;
        /* [CONFIRMED @ 0x0040A4B4 UpdateRaceCameraTransitionTimer; Phase 2
         *  follow-up 2026-05-18] Orig reloads camera preset state at the
         *  timer < 0x101 crossing, taking the "clear" branch when input
         *  playback OR replay mode is active, else the "restore" branch.
         *  Today the camera preset is not re-saved during countdown so
         *  the restore is a no-op, but the structural call is added for
         *  parity. Fires once at the timer-zero crossing inside this
         *  branch (caller's countdown_active>0 guard prevents re-entry). */
        {
            int clear_or_restore = (td5_input_is_playback_active() ||
                                    s_replay_mode) ? 1 : 0;
            ResetRaceCameraSelectionState(clear_or_restore);
        }
    } else {
        g_cameraTransitionActive -= TD5_COUNTDOWN_DECR;
    }

    level = g_cameraTransitionActive / TD5_COUNTDOWN_LEVEL_DIV;
    /* Only levels 2,1,0 map to visible digits 3,2,1.
     * Levels 4,3 (indicator 5,4) correspond to blank atlas cells in the original
     * — don't show them to avoid rendering garbage sprites. */
    next_indicator = (level <= 2) ? (level + 1) : 0;
    if (next_indicator != s_race_countdown_state) {
        s_race_countdown_state = next_indicator;
        set_countdown_indicator_state(next_indicator);
        TD5_LOG_I(LOG_TAG, "Race countdown: level=%d indicator=%d timer=0x%X",
                  level, next_indicator, g_cameraTransitionActive);
    }

    /* level==0 marks the "1" digit appearing. User-observed behavior in
     * the original (2026-05-17): race does NOT start here — the car
     * stays held until "1" disappears (timer==0). Earlier audit (630d797)
     * read orig as flipping g_gamePaused at level==0 but the in-game
     * race-start moment perceived by the user is timer==0, so the paused
     * flip must happen there. Either the Ghidra disasm was misread or
     * orig has a separate inner gate that holds the car during the
     * level==0 window even though g_gamePaused == 0; either way, matching
     * the user-visible behavior means flipping here at timer==0. */
    if (level == 0 && s_level0_logged == 0) {
        s_level0_logged = 1;
        TD5_LOG_I(LOG_TAG,
                  "Race countdown: level==0 (\"1\" indicator visible) timer=0x%X",
                  g_cameraTransitionActive);
    }

    /* Hide indicator + flip paused at timer==0. Mirrors orig's entry-
     * guard at 0x0040A490 plus the perceived race-start moment. */
    if (g_cameraTransitionActive == 0 && s_race_countdown_state != 0) {
        set_countdown_indicator_state(0);
        s_race_countdown_state = 0;
        g_td5.paused = 0;
        s_level0_logged = 0;
        TD5_LOG_I(LOG_TAG, "Race countdown: GO at timer==0 (paused→0, indicator hidden)");
    }
}

/* ========================================================================
 * Release race resources
 *
 * Called when fade-out completes. Releases sound, input, render resources.
 * ======================================================================== */

void td5_game_release_race_resources(void) {
    TD5_LOG_I(LOG_TAG, "Releasing race resources");

    /* [S31] Net race over (finish, quit, or abort): drop the lockstep latch
     * so the next lobby visit doesn't read a stale rendezvous and relaunch.
     * A synced RESTART keeps it -- the machines resume the same lockstep
     * stream in the re-initialized race. */
    if (g_td5.network_active && !s_pause_restart_pending)
        td5_net_race_done();

    /* Stop and release all race sound channels */
    td5_sound_release_race_channels();
    td5_sound_set_race_end(1);

    /* Stop force feedback and reset input config */
    td5_input_ff_stop();
    td5_input_ff_shutdown();

    /* Close input recording/playback */
    if (s_replay_mode) {
        td5_input_read_close();
    } else {
        td5_input_write_close();
    }
    td5_input_set_playback_active(0);

    /* Release render resources */
    td5_render_reset_texture_cache();

    /* Post-process: fix up any actors whose display position was unset */
    for (int i = 0; i < TD5_MAX_RACER_SLOTS; i++) {
        if (s_metrics[i].display_position < 0 ||
            s_metrics[i].display_position >= TD5_MAX_RACER_SLOTS) {
            s_metrics[i].display_position = (int16_t)i;
        }
    }
}

/* Post-finish dwell before the victory star/fade/results fire.
 * The ORIGINAL uses 0x3FFFFF: g_raceEndControl accrues a fixed 30 ticks/sec
 * (16.16 fp, verified @ 0x0040a230 + frame increment @ 0x0042b5d1) and the
 * gate is `0x3FFFFF < g_raceEndControl` = 64 ticks / 30 Hz = 2.13s.
 * Per user request (2026-05-30) the port triggers the victory animation AT the
 * finish line instead of 2s later — a deliberate snappier-than-original choice.
 * 0 = expire on the first post-latch frame (~1 frame, effectively instant).
 * Raise toward 0x3FFFFF (4194303) to restore the original 2.13s dwell. */
#define TD5_RACE_END_DWELL  0u

/* ========================================================================
 * Race Completion Detection
 *
 * Two-phase architecture matching CheckRaceCompletionState (0x409e80):
 *   Phase 1: Per-actor finish detection (when s_post_finish_cooldown == 0)
 *   Phase 2: Cooldown accumulator; when > TD5_RACE_END_DWELL, build results
 * ======================================================================== */

/* [MP COP CHASE win conditions 2026-06-22] Count active (non-disabled) suspects
 * and how many are busted (damage bar depleted). */
static int mp_cop_chase_count_suspects(int *busted_out) {
    int cop = td5_game_cop_chase_cop_slot();
    int active = 0, busted = 0, s;
    if (cop < 0) { if (busted_out) *busted_out = 0; return 0; }
    for (s = 0; s < TD5_MAX_RACER_SLOTS; s++) {
        if (!td5_game_cop_chase_is_suspect(s)) continue;
        if (s_slot_state[s].state == 3) continue;        /* disabled slot */
        active++;
        if (g_wanted_damage_state[s] <= 0) busted++;      /* arrested */
    }
    if (busted_out) *busted_out = busted;
    return active;
}

/* ========================================================================
 * [MP COP CHASE INFECT 2026-06-25] Arrested-suspect -> cop conversion.
 *
 * When the INFECT toggle is on, a suspect that gets arrested is converted into
 * a COP instead of being permanently parked: its car is swapped to a randomly
 * chosen police car (mesh + carparam reloaded for that slot via the per-slot
 * loader td5_asset_load_vehicle, RE-grounded by LoadRaceVehicleAssets @
 * 0x00443280) and the slot is flagged a cop. Eliminated players thus keep
 * playing as cops; the round ends once every suspect has been infected.
 *
 * s_infected_mask  - racer slots already converted; consulted by
 *                    td5_game_cop_chase_is_cop so a converted suspect counts as
 *                    a cop in BOTH the human-cop (mask) and AI-cop (single-slot)
 *                    paths, regardless of cop_is_ai.
 * s_infect_pending - work queue: the arrest path sets a bit; the heavy car-swap
 *                    runs once per frame in td5_game_process_pending_infections
 *                    at a point where asset I/O is safe (never in the collision
 *                    handler). Both reset per race in the cop-chase init block.
 * ======================================================================== */
static uint32_t s_infected_mask  = 0;
static uint32_t s_infect_pending = 0;

/* Random police-car roster indices into s_car_zip_paths: 33=cop.zip (Police
 * Cerbera), 34=sp5 (Mustang), 35=sp6 (Charger), 36=sp7 (Camaro) — the 4
 * canonical TD5 police cars, all present as extracted assets. */
static const int k_infect_cop_cars[4] = { 33, 34, 35, 36 };

void td5_game_reset_infect_state(void) { s_infected_mask = 0; s_infect_pending = 0; }

/* 1 when INFECT mode is active for this (local) cop chase. */
int td5_game_cop_chase_infect_enabled(void) {
    return g_td5.wanted_mode_enabled
        && g_td5.mp_mode_config.mode == TD5_MP_MODE_COP_CHASE
        && !g_td5.network_active
        && g_td5.mp_mode_config.cop_infect_enabled != 0;
}

/* Queue an arrested suspect for conversion into a cop. Called from the arrest
 * path (td5_ai_wanted_cop_hit); a no-op when INFECT is off or the slot is
 * already a cop. The heavy car-swap is deferred to the per-frame processor. */
void td5_game_infect_request(int suspect_slot) {
    if (suspect_slot < 0 || suspect_slot >= TD5_MAX_RACER_SLOTS) return;
    if (!td5_game_cop_chase_infect_enabled()) return;
    if ((s_infected_mask >> suspect_slot) & 1u) return;   /* already a cop */
    s_infect_pending |= (1u << suspect_slot);
    TD5_LOG_I(LOG_TAG, "INFECT: slot=%d queued for cop conversion", suspect_slot);
}

/* Process queued infections once per frame at a safe point (outside the sim
 * loop). For each pending slot: swap its car to a random police car (mesh +
 * carparam reloaded), flag it a cop (s_infected_mask -> is_cop true), restore
 * full health, and — for AI-driven slots — arm the cop AI driver. Human-driven
 * infected cops keep manual control; they're cops via the mask + their freshly
 * loaded police car (the render path draws the loaded vehicle, not the generic
 * cop overlay, for any cop_chase_is_cop slot). */
void td5_game_process_pending_infections(void) {
    if (!s_infect_pending) return;
    if (!td5_game_cop_chase_infect_enabled()) { s_infect_pending = 0; return; }
    int humans = g_td5.num_human_players;
    if (humans < 0) humans = 0;
    if (humans > TD5_MAX_RACER_SLOTS) humans = TD5_MAX_RACER_SLOTS;
    for (int s = 0; s < TD5_MAX_RACER_SLOTS; s++) {
        if (!((s_infect_pending >> s) & 1u)) continue;
        s_infect_pending &= ~(1u << s);
        if ((s_infected_mask >> s) & 1u) continue;        /* already converted */

        /* Swap this slot's vehicle to a random police car (mesh + physics). */
        int pick = k_infect_cop_cars[(unsigned)rand() % 4u];
        td5_asset_load_vehicle(pick, s, 0);

        /* Flag as a cop and give it a fresh, healthy car. */
        s_infected_mask |= (1u << s);
        g_wanted_damage_state[s] = 0x1000;

        /* AI-driven slot -> arm the cop driver; a human slot keeps manual control
         * (its cop identity comes from s_infected_mask, not the AI cop driver). */
        int is_human = (s < humans) && !(g_td5.mp_ai_player_mask & (1u << s));
        if (!is_human)
            td5_ai_cop_chase_setup(s, 1);

        TD5_LOG_I(LOG_TAG, "INFECT: slot=%d converted to cop (car=%d, driver=%s)",
                  s, pick, is_human ? "human" : "AI");
    }
}

/* Returns 1 when the cop-chase win condition ends the race early. BUST_ALL: cop
 * wins once every suspect is arrested. SUDDEN_DEATH: cop wins on the first
 * arrest. MOST_BUSTS: no early end — the race runs to its natural finish and the
 * arrest tally decides. */
static int mp_cop_chase_resolved(void) {
    int active, busted = 0, wc;
    if (g_td5.mp_mode_config.mode != TD5_MP_MODE_COP_CHASE) return 0;
    if (!g_td5.wanted_mode_enabled) return 0;
    active = mp_cop_chase_count_suspects(&busted);
    /* [INFECT 2026-06-25] When INFECT mode has converted every suspect into a cop,
     * no suspects remain — the cops win. Checked BEFORE the active<=0 guard below
     * (which otherwise treats "no suspects" as "not a cop chase" and never ends
     * the round, since cops don't cross the finish line). Gated on at least one
     * conversion so a mis-configured 0-suspect setup can't false-trigger. */
    if (td5_game_cop_chase_infect_enabled() && active == 0 && s_infected_mask != 0) {
        TD5_LOG_I(LOG_TAG, "Cop chase INFECT: all suspects converted to cops -> race end");
        return 1;
    }
    if (active <= 0) return 0;
    /* [MP COP CHASE results 2026-06-25] Every suspect arrested -> nobody left to
     * chase, so end the race NOW regardless of win condition (user request: "when
     * police arrest all suspects it should go right to the results"). This also
     * subsumes the old BUST_ALL `busted >= active` check below. */
    if (busted >= active) {
        TD5_LOG_I(LOG_TAG, "Cop chase: all %d suspects arrested -> race end", active);
        return 1;
    }
    wc = g_td5.mp_mode_config.cop_win_condition;
    if (wc == TD5_COP_WIN_SUDDEN_DEATH && busted >= 1) {
        TD5_LOG_I(LOG_TAG, "Cop chase: SUDDEN DEATH — first arrest, cop wins");
        return 1;
    }
    return 0;
}

static int check_race_completion(uint32_t sim_delta) {
    int i;

    /* Phase 2: Post-finish cooldown (near-instant per TD5_RACE_END_DWELL).
     * [CAR DAMAGE 2026-06-28] When the finish-orbit is enabled, hold here for a
     * few seconds instead of ending instantly, so the chase camera can orbit the
     * player's (stationary, finished) car to show off the accumulated damage
     * before results. NOT during a replay (which reuses this path). Faithful
     * near-instant dwell when CarDamage is off. */
    if (s_post_finish_cooldown != 0) {
        uint32_t dwell = TD5_RACE_END_DWELL;
        if (td5_damage_finish_orbit_enabled() && (!s_replay_mode || !replay_keep_results_on()))
            dwell = (uint32_t)td5_damage_finish_orbit_hold_ms();
        s_post_finish_cooldown += sim_delta;
        if (s_post_finish_cooldown > dwell) {
            /* Dwell expired: build results and signal completion */
            s_post_finish_cooldown = 0;
            /* [REPLAY RESULTS PRESERVE 2026-06-27] A View Replay reaches this
             * completion path too (just like the original @0x42B5ED-0x42B626),
             * but it is NOT a real race — it must not rebuild the results table
             * or award cup points. Doing so overwrote the real standings with the
             * replay's partial outcome and made the replay "count as a new race".
             * The genuine results were snapshotted at replay-init and are restored
             * when the replay ends; here we only drive the fade/finish. */
            if (!s_replay_mode || !replay_keep_results_on()) {
                TD5_LOG_I(LOG_TAG, "Race completion: building results (dwell=%u)", dwell);
                build_results_table();
                td5_game_mp_cup_award();   /* [MP CUP] tally this race's points */
            } else {
                TD5_LOG_I(LOG_TAG, "Replay completion: skip results rebuild + cup award (real results preserved)");
            }
            return 1;
        }
        return 0;
    }

    /* [MP COP CHASE] Win condition can end the race early (bust-all / sudden
     * death). Mirrors the all-finished latch: start the post-finish dwell, then
     * phase 2 builds the results next tick. */
    if (mp_cop_chase_resolved()) {
        s_post_finish_cooldown = 1;
        s_race_end_timer_start = td5_plat_time_ms();
        TD5_LOG_I(LOG_TAG, "Cop chase: win condition met -> race end");
        return 0;
    }

    /* Phase 1: Check if all required actors have finished */
    int all_finished = 1;

    if (g_td5.time_trial_enabled) {
        /* Time trial: only require slots 0 and 1 (human players) */
        for (i = 0; i < 2; i++) {
            if (s_slot_state[i].state != 3 &&
                s_metrics[i].post_finish_metric_base == 0) {
                all_finished = 0;
                break;
            }
        }
    } else {
        /* Normal race: the race ends when every HUMAN player has finished;
         * unfinished AI no longer block once all humans are done.
         *
         * [MP per-viewport finish 2026-06-13] Previously this ended the race as
         * soon as SLOT 0 finished (the `i != 0 && slot-0 done -> continue`
         * exception treated every other slot — including other human players —
         * as non-blocking). In split-screen MP that ended the race for everyone
         * the instant player 1 crossed the line. Now each human must finish; in
         * single-player (humans==1) this reduces to "slot 0 done", unchanged. */
        int humans = g_td5.num_human_players;
        if (humans < 1) humans = 1;
        if (humans > TD5_MAX_RACER_SLOTS) humans = TD5_MAX_RACER_SLOTS;

        /* [CUP TRACK SELECT 2026-06-25] AI-driven "players" (the ADD AI PLAYER dev
         * tool fills human-pool slots with bots; bit set in mp_ai_player_mask)
         * are NOT real humans, so they must not hold up the finish — the race ends
         * the moment every REAL human has crossed the line. mp_ai_player_mask is 0
         * in real human sessions, so this leaves normal split-screen/net MP and
         * single-player unchanged. */
        /* [MP COP CHASE results 2026-06-25] An ARRESTED suspect (damage bar
         * drained to 0) can never cross the finish line, so it must not hold up
         * the "all alive players reached the end" completion. Treat it as done in
         * both scans below. The race then ends once every cop + every un-arrested
         * suspect has finished (the all-suspects-arrested case ends even sooner
         * via mp_cop_chase_resolved above). */
        const int cop_chase = (g_td5.mp_mode_config.mode == TD5_MP_MODE_COP_CHASE)
                              && g_td5.wanted_mode_enabled;

        int all_humans_done = 1;
        for (i = 0; i < humans; i++) {
            if (s_slot_state[i].state == 3) continue;             /* disabled */
            if (g_td5.mp_ai_player_mask & (1u << i)) continue;    /* AI-driven, not a real human */
            if (cop_chase && td5_game_cop_chase_is_suspect(i) &&
                g_wanted_damage_state[i] <= 0) continue;          /* arrested -> done */
            if (td5_damage_slot_knocked_out(i)) continue;         /* [CAR DAMAGE] wrecked -> done */
            if (s_slot_state[i].companion_1 == 0) { all_humans_done = 0; break; }
        }

        for (i = 0; i < TD5_MAX_RACER_SLOTS; i++) {
            if (s_slot_state[i].state == 3) continue;  /* disabled */
            if (cop_chase && td5_game_cop_chase_is_suspect(i) &&
                g_wanted_damage_state[i] <= 0) continue;          /* arrested -> done */
            if (td5_damage_slot_knocked_out(i)) continue;         /* [CAR DAMAGE] wrecked -> done */
            if (s_slot_state[i].companion_1 == 0) {     /* not finished */
                int is_human = (i < humans) &&
                               !(g_td5.mp_ai_player_mask & (1u << i));
                /* Unfinished AI (incl. AI-driven players) don't hold up the
                 * results once all real humans are done. */
                if (!is_human && all_humans_done) continue;
                all_finished = 0;
                break;
            }
        }
    }

    if (all_finished) {
        /* Latch the cooldown accumulator to begin phase 2 */
        s_post_finish_cooldown = 1;
        s_race_end_timer_start = td5_plat_time_ms();
        TD5_LOG_I(LOG_TAG,
                  "Race completion triggered: slot0=%d slot1=%d slot2=%d slot3=%d slot4=%d slot5=%d",
                  s_slot_state[0].companion_1,
                  s_slot_state[1].companion_1,
                  s_slot_state[2].companion_1,
                  s_slot_state[3].companion_1,
                  s_slot_state[4].companion_1,
                  s_slot_state[5].companion_1);
    }

    return 0;
}

/* ========================================================================
 * Per-sub-tick P2P timer countdown + timeout failure (0x40A2B0)
 *
 * Mirrors AdvancePendingFinishState @ 0x0040A2B0. The original packs a 16-bit
 * time bank at actor+0x344 as CONCAT11(hi, lo):
 *
 *   lo -= 2                              (sub-fractional counter, 0..56)
 *   if (lo < 0)  lo += 0x39, hi -= 1     (borrow 1 unit from the upper byte)
 *   if (hi < 0)  seed post-finish metrics and promote slot state to 2
 *
 * The decrement is gated on g_specialEncounterType != 0 (P2P modes only) and
 * on actor+0x328 (finish_time) still being zero. When hi underflows the
 * original sets:
 *   actor+0x328 = actor+0x34c        (post_finish_metric_base from stored field)
 *   actor+0x334 = clamp(actor+0x314 >> 8, min 1)
 *   actor+0x336 = (track_sub_progress * 0x5dc) / actor+0x334
 * and promotes state '\x01' -> '\x02' with companion_1 = '\x01', companion_2 = '\x02'.
 *
 * In the port, m->timer_ticks is a flat int16 representing the combined
 * HI*0x39 + LO bank, updated here with the same wraparound semantics so that
 * HUD/result code can keep treating it as a scalar. Timeout promotes slot
 * state to 2 (completed, but marked as a failure via post_finish_metric_base
 * fallback to cumulative_timer for sort fallback). This is the missing
 * race-fail-on-timeout path for P2P.
 * ======================================================================== */

static void tick_pending_finish_timer(int slot) {
    ActorRaceMetric *m = &s_metrics[slot];

    /* [CONFIRMED @ 0x0040A2B0 AdvancePendingFinishState; Phase 2 follow-up
     *  2026-05-18] All five gates from orig now mirrored verbatim:
     *   gRaceSlotStateTable.slot[uVar1].state == '\x01'   (racing)
     *   && gRaceCameraTransitionGate == 0                 (countdown done)
     *   && g_replayModeFlag == 0                          (not replaying)
     *   && *(int *)(param_1 + 0x328) == 0                 (finish_time unset)
     *   && g_specialEncounterType != 0                    (P2P MODE ONLY)
     *
     * Port mirror of g_specialEncounterType is g_special_encounter
     * (resolved per-race in td5_game_init_race_session). Previously this
     * gate read g_td5.special_encounter_enabled, which is wired to the
     * COPS toggle in the frontend (td5_frontend.c:2681) and is therefore
     * the wrong global — when cops were OFF the timer never decremented
     * and the race never finished via the timer path.
     *
     * The gRaceCameraTransitionGate gate is mirrored via g_cameraTransitionActive
     * (collapsed-state convention, see tick_race_countdown delta E). The
     * sub-tick loop's outer paused=0 already prevents this function from
     * firing during the countdown window, but the explicit gate matches
     * orig structure for parity. The timer_ticks <= 0 early return was
     * removed — orig writes back the CONCAT11 value unconditionally inside
     * the finish_time==0 branch; the underflow lo=0x39 / hi-stays-0 path
     * is idempotent on an already-promoted slot (companion_1 gate above
     * catches it first).
     * [CONFIRMED @ 0x0040A2DC: if (g_specialEncounterType != 0 && ...).] */
    if (g_special_encounter == 0) return;  /* non-P2P -> no decrement */
    if (s_active_checkpoint.checkpoint_count == 0) return;
    /* state==1 gate per orig 0x0040A2DC. Port debug knob PlayerIsAI=1 forces
     * slot 0 to state==0 (mirrors orig attract-mode AI write at 0x0042ACCF)
     * but we still want the P2P checkpoint timer to tick down — otherwise
     * the HUD shows a frozen value forever and the race never finishes via
     * the timer-expiry path. Accept state==0 only for slot 0 under PlayerIsAI;
     * all other slots must be state==1 (racing). */
    if (s_slot_state[slot].state != 1) {
        if (!(slot == 0 && g_td5.ini.player_is_ai && s_slot_state[slot].state == 0)) {
            return;
        }
    }
    if (g_cameraTransitionActive != 0) return;          /* gRaceCameraTransitionGate */
    if (g_replay_mode != 0) return;                     /* g_replayModeFlag */
    if (s_slot_state[slot].companion_1 != 0) return;  /* already finished */
    if (m->post_finish_metric_base != 0) return;       /* already scored */

    /* Diagnostic: log slot 0's timer once a second so we can confirm decrement
     * cadence + observe the run-up to underflow. Removeable once timer behavior
     * is verified across tracks. */
    if (slot == 0) {
        static int s_log_div = 0;
        if ((++s_log_div % 30) == 0) {
            TD5_LOG_I(LOG_TAG,
                      "tick_pending_finish: slot=0 timer_ticks=%d cp_count=%d cp_idx=%d",
                      (int)m->timer_ticks,
                      (int)s_active_checkpoint.checkpoint_count,
                      (int)m->checkpoint_index);
        }
    }

    /* Faithful port of original AdvancePendingFinishState @ 0x0040A2DC:
     *   uVar6 = uVar5 - 2; if (uVar6 < 0) { uVar6 = uVar5 + 0x39; uVar2--; }
     *   *(ushort*)(actor+0x344) = CONCAT11(uVar2, uVar6);
     *
     * uVar5 = lo byte, uVar2 = hi byte. The wrap uVar5 + 0x39 (57) means
     * lo cycles 0..58 even / 1..59 odd → 30 sub-ticks per hi-decrement →
     * hi-byte = SECONDS at 30 Hz. The previous flat `-= 2` decremented
     * timer_ticks correctly in raw count but made the displayed hi-byte
     * tick at ~0.23/sec instead of 1/sec. */
    {
        uint16_t t = (uint16_t)m->timer_ticks;
        int hi = (int)((t >> 8) & 0xFF);
        int lo = (int)(t & 0xFF);
        int new_lo = lo - 2;
        if (new_lo < 0) {
            new_lo = lo + 0x39;
            if (hi == 0) {
                /* Hi-byte underflow → race-finish path below. */
                m->timer_ticks = 0;
            } else {
                hi -= 1;
                m->timer_ticks = (int16_t)(((uint16_t)hi << 8) | (uint8_t)new_lo);
            }
        } else {
            m->timer_ticks = (int16_t)(((uint16_t)hi << 8) | (uint8_t)new_lo);
        }
    }

    if (m->timer_ticks <= 0) {
        /* Timer expired - race-fail-on-timeout. Equivalent to the original's
         * state promotion at 0x0040A34F: post-finish metrics seeded and slot
         * flipped into state 2. Fall back to cumulative_timer when
         * post_finish_metric_base is unset so results sort consistently. */
        m->timer_ticks = 0;
        if (m->post_finish_metric_base == 0) {
            m->post_finish_metric_base = (m->cumulative_timer != 0)
                ? m->cumulative_timer
                : 1;  /* nonzero sentinel = "finished" for aggregator */
        }
        s_slot_state[slot].companion_1 = 1;
        s_slot_state[slot].companion_2 = 2;
        s_slot_state[slot].state = 2;
        TD5_LOG_I(LOG_TAG,
                  "Actor finish: slot=%d mode=p2p-timeout timer=0 cumulative=%d cp=%d",
                  slot, m->cumulative_timer, (int)m->checkpoint_index);
    }
}

/* ========================================================================
 * Advance pending finish state per actor (0x40A2B0 / 0x409E80 per-actor body)
 *
 * Per-frame update for each actor: check circuit sectors or P2P checkpoints.
 * Called once per frame (not per sub-tick) — matches the original's
 * CheckRaceCompletionState cadence at the head of RunRaceFrame.
 * ======================================================================== */

static void advance_pending_finish_state(int slot, uint32_t sim_delta) {
    ActorRaceMetric *m = &s_metrics[slot];
    TD5_Actor *actor = td5_game_get_actor(slot);

    /* Original reads actor+0x82 directly per-tick but does NOT sync it into
     * the metric struct — metric normalized_span stays 0 until checkpoint. */
    int16_t actor_span = actor ? actor->track_span_normalized : 0;

    /* Already finished */
    if (s_slot_state[slot].companion_1 != 0) return;

    /* [TD6 SYNTHESIZED CHECKPOINTS] Register a drive-through when this actor
     * reaches the next synthesized checkpoint span (derived from the in-track
     * ring/banner meshes — TD6.exe has no checkpoint-trigger data). Records a
     * split and, for the player, a brief HUD acknowledgement. Deliberately
     * runs BEFORE the circuit/P2P branches and does NOT end the race: P2P
     * finish stays s_td6_finish_span, circuit stays lap-based, and there is no
     * beat-the-clock fail-timer. s_td6_cp_count is 0 on every faithful TD5
     * track, so this is a no-op there. */
    /* [S18] For P2P TD6 tracks (s_td6_finish_span > 0) the checkpoint spans
     * ascend monotonically toward the finish, so gate them on the RAW
     * accumulated span (+0x84) for the same reason as the finish gate below:
     * a reverse at the start wraps the normalized span (+0x82) to ~ring_length-1
     * and would otherwise FALSELY register every checkpoint at once (observed:
     * "cp=1/5 span=2353" the moment the car reversed past span 0). The raw
     * accumulator equals the normalized span for forward driving (so forward
     * behaviour is unchanged) and stays negative on reverse. Circuit TD6 tracks
     * (s_td6_finish_span == 0) keep the normalized span — laps wrap there and
     * the normalized value is the correct lap-relative position. */
    int td6_cp_basis = (int)actor_span;
    /* Forward P2P gates on the RAW accumulator (+0x84) to dodge the reverse-at-start
     * normalized-span wrap. In REVERSE the player drives STRIPB forward so the
     * normalized span (+0x82) counts up monotonically and is the right basis (the raw
     * accumulator stays negative when driving the reverse direction). */
    if (s_td6_finish_span > 0 && actor && !g_td5.reverse_direction)
        td6_cp_basis = (int)*(int16_t *)((uint8_t *)actor + 0x84);
    if (s_td6_cp_count > 0 &&
        s_td6_cp_index[slot] < s_td6_cp_count &&
        td6_cp_basis >= s_td6_cp_spans[s_td6_cp_index[slot]]) {
        int idx = s_td6_cp_index[slot];
        if (idx < 9)
            m->lap_split_times[idx] = (int16_t)m->cumulative_timer;
        /* [FIX checkpoint-timer 2026-06-23] Add the per-checkpoint time bonus to
         * the P2P countdown bank. The byte-faithful P2P bonus path (~line 7110,
         * RE basis 0x00409E80: per-actor timer += signed16 record bonus) is
         * SKIPPED for TD6 P2P tracks because the s_td6_finish_span>0 branch in the
         * P2P section returns early before reaching it — so London's checkpoint
         * timer only ever counted DOWN and never gained time at a banner. Apply
         * the bonus seeded into s_active_checkpoint at race init here, where the
         * synthesized crossing is registered. timer_ticks is packed hi-byte=whole
         * seconds and time_bonus was seeded as (secs<<8), so a plain add mirrors
         * the original's `timer += bonus`. Gated on g_special_encounter so it only
         * fires when the TD6 checkpoint timer is actually enabled+seeded. */
        if (g_special_encounter != 0 &&
            idx < (int)s_active_checkpoint.checkpoint_count) {
            int16_t bonus = (int16_t)s_active_checkpoint.checkpoints[idx].time_bonus;
            if (bonus != 0) {
                m->timer_ticks += bonus;
                TD5_LOG_I(LOG_TAG,
                          "TD6 checkpoint bonus: slot=%d cp=%d +%ds (packed=%d) -> timer_ticks=%d",
                          slot, idx, (int)(bonus >> 8), (int)bonus, (int)m->timer_ticks);
            }
        }
        s_td6_cp_index[slot] = (uint8_t)(idx + 1);
        TD5_LOG_I(LOG_TAG,
                  "TD6 checkpoint: slot=%d cp=%d/%d span=%d timer=%d",
                  slot, idx + 1, s_td6_cp_count, td6_cp_basis,
                  m->cumulative_timer);
        /* [REMOVED 2026-06-05] previously poked the HUD's on-screen
         * "CHECKPOINT n/N" flash here (td5_hud_set_td6_checkpoint_flash) for
         * slot 0 — the user did not want that indicator. Split-time recording
         * above is retained. */
    }

    /* Race timer increment is driven per sub-tick in td5_game_run_race_frame
     * (see the per-slot block after sync_actor_race_metrics). Originally the
     * write at UpdateVehicleActor +0x34c runs once per sub-tick gated on the
     * countdown-cleared gate and actor+0x328==0, so it must not fire here
     * (this path runs once per render frame, which made the HUD timer accrue
     * countdown ticks and run at render-rate instead of the 30 Hz sim rate). */

    /* Circuit branch — faithful port of CheckRaceCompletionState circuit
     * body @ 0x00409E80 (condition gTrackIsCircuit!=0 && g_selectedGameType==0),
     * sector dispatch at LAB_0040A014, finish-state seeding at 0x00409FC0.
     *
     * Actor offsets (read via psVar5 = (short*)&actor[0x336]):
     *   psVar5[-0x15a] (+0x082) int16  track_span_normalized   — progress index (iVar13)
     *   psVar5[-0x11]  (+0x314) int32  longitudinal_speed       — finish denom source
     *   psVar5[-0x07]  (+0x328) int32  finish_time              — zero while racing
     *   psVar5[-0x01]  (+0x334) int16  finish_time_aux          — finish denominator
     *   psVar5[ 0x00]  (+0x336) int16  finish_time_subtick      — sector bitmask 0/1/3/7/0xF,
     *                                                              overwritten on finish
     *   psVar5[ 0x0b]  (+0x34C) int16  timing_frame_counter     — current race tick counter
     *   psVar5[ 0x0c]  (+0x34E) int16[9] checkpoint_split_times — per-lap delta array
     *   psVar5[ 0x24]  (+0x37E) uint8  checkpoint_count         — lap counter (byte)
     *
     * Globals:
     *   g_trackStartSpanIndex (g_td5.track_start_span_index)  — start-line anchor
     *   g_trackTotalSpanCount (g_td5.track_span_ring_length)  — ring length
     *   gCircuitLapCount      (g_td5.circuit_lap_count)       — laps to finish
     */
    if (g_td5.track_type == TD5_TRACK_CIRCUIT) {
        int32_t track_start = g_td5.track_start_span_index;
        int32_t total_spans = g_td5.track_span_ring_length;

        if (!actor || total_spans <= 0) {
            return;
        }

        /* ---------------------------------------------------------------
         * Split-time delta loop @ 0x00409E98 — runs unconditionally
         * before any lap/sector logic, every tick, every unfinished actor.
         *
         *   splits[lap]  = timing_frame_counter;        // seed current
         *   for (i = 0; i < lap; i++) {
         *       splits[lap] -= splits[i];               // subtract priors
         *   }
         *
         * At steady state: splits[lap] holds the DELTA ticks of the
         * currently-running lap (cumulative - sum of completed-lap deltas).
         * When the lap increments below, the slot freezes with its final
         * delta and splits[lap+1] takes over. Port's ActorRaceMetric
         * mirrors actor+0x34E as lap_split_times[9] (9 shorts = 18 bytes).
         * --------------------------------------------------------------- */
        {
            int lap = (int)m->checkpoint_index;
            if (lap >= 0 && lap < 9) {
                m->lap_split_times[lap] = (int16_t)m->cumulative_timer;
                for (int i = 0; i < lap; i++) {
                    m->lap_split_times[lap] -= m->lap_split_times[i];
                }
            }
        }

        /* ---------------------------------------------------------------
         * Start-line / lap-complete test @ 0x00409F78.
         * The original reads *psVar5 as a full int16 from actor+0x336 and
         * compares to 0xF literally. Port's uint8_t checkpoint_bitmask is
         * value-equivalent (never exceeds 0xF). Note: lap increment and
         * bitmask reset BOTH happen before the finish gate — that is, the
         * lap counter is already advanced when we check for finish.
         * --------------------------------------------------------------- */
        /* [TD6 LAP/FINISH FIX — track-scoped, OverrideTrackZip-gated]
         * The native gate requires the 4-sector checkpoint bitmask (==0x0F) to
         * latch before a start-line crossing counts a lap. But checkpoints are
         * a point-to-point feature (not meaningful on a circuit), AND the sector
         * boundary formula assumes track_start < ring/2 — on a substituted TD6
         * ring the start/finish straight (g_trackStartSpanIndex = OverrideStart
         * Span, e.g. 312 of 450) sits past the midpoint, so the sector bits
         * never latch and no lap/finish ever fires. For override tracks, count a
         * lap the simple circuit way: arm at the ring point opposite the start/
         * finish, then complete when the car returns to the start/finish span.
         * checkpoint_bitmask is reused as a 1-bit armed latch (the sector
         * dispatch is skipped for override below). The lap ticks exactly at the
         * visual start/finish straight where the grid spawns; faithful tracks
         * keep the byte-faithful sector-gated path unchanged. */
        /* [NATIVE REVERSE CIRCUIT 2026-06-28] A native TD5 circuit driven in
         * reverse loads STRIPB and mirrors its start span to (ring - fwd_start)
         * (see "reverse circuit start span" above). For tracks whose forward
         * start sits in the first half (e.g. Cheddar fwd=106), the mirrored
         * reverse start lands PAST the ring midpoint (577 of 683) — the exact
         * condition under which the 4-sector bitmask gate below never latches
         * (the sector boundary formula assumes track_start < ring/2), so no lap
         * ever counts and the race never finishes. Use the same position-
         * independent circuit lap logic as the TD6 path (arm opposite, complete
         * at start). Forward faithful circuits (start < ring/2) keep the
         * byte-faithful sector-gated path unchanged. */
        int use_simple_circuit_lap =
            (g_active_td6_level > 0) ||
            (g_td5.reverse_direction && g_track_is_circuit);
        int lap_crossed;
        if (use_simple_circuit_lap) {
            int32_t opp = (track_start + total_spans / 2) % total_spans;
            int32_t d_start = (int32_t)actor_span - track_start;
            if (d_start < 0) d_start = -d_start;
            if (d_start > total_spans - d_start) d_start = total_spans - d_start;
            int32_t d_opp = (int32_t)actor_span - opp;
            if (d_opp < 0) d_opp = -d_opp;
            if (d_opp > total_spans - d_opp) d_opp = total_spans - d_opp;
            if (d_opp <= 10) m->checkpoint_bitmask = 1;   /* arm at far side */
            /* [NATIVE REVERSE CIRCUIT 2026-06-29] Align the lap/finish with the
             * actual start/finish line (the banner sits at track_start). The
             * symmetric ±10 window latches when the car merely ENTERS the window
             * — up to 10 spans BEFORE the line — so the lap ticked early and out
             * of step with the banner. For reverse native circuits, complete only
             * once the car has REACHED/crossed track_start going forward (small
             * "spans past start"), keeping a 10-span catch window for fast cars
             * that skip the exact span. TD6 keeps the symmetric window. */
            if (g_td5.reverse_direction && g_active_td6_level == 0) {
                int32_t past = ((int32_t)actor_span - track_start) % total_spans;
                if (past < 0) past += total_spans;
                lap_crossed = (m->checkpoint_bitmask == 1 && past <= 10);
            } else {
                lap_crossed = (m->checkpoint_bitmask == 1 && d_start <= 10);
            }
        } else {
            lap_crossed = ((int32_t)actor_span >= (track_start - 1) &&
                           (int32_t)actor_span <= (track_start + 1) &&
                           m->checkpoint_bitmask == 0x0F);
        }
        if (lap_crossed) {
            /* lap++; bitmask = 0. DO NOT write split here — the delta loop
             * above has already captured it into lap_split_times[lap]. */
            m->checkpoint_index++;
            m->checkpoint_bitmask = 0;
            m->normalized_span = actor_span;

            TD5_LOG_I(LOG_TAG,
                      "Circuit lap complete: slot=%d lap=%d span=%d",
                      slot, m->checkpoint_index, (int)actor_span);

            if ((uint8_t)m->checkpoint_index == (uint8_t)g_td5.circuit_lap_count) {
                /* Finish state seeding @ 0x00409FC0.
                 * Gated on actor+0x328 == 0 (finish_time still zero).
                 *
                 *   actor+0x328  = (uint)(ushort) actor+0x34C     // finish_time
                 *   iVar        = (longitudinal_speed + sign) >> 8
                 *   actor+0x334 = max(iVar, 1)                     // finish denom
                 *   actor+0x336 = (track_sub_progress * 0x5DC) / finish_denom
                 *   slot.companion_state_1 = 1
                 *   slot.state             = 2
                 *   slot.companion_state_2 = 2
                 *
                 * [UNCERTAIN] track_sub_progress is not mirrored in the
                 * port. gActorTrackSubProgress @ 0x004afc3c is a per-slot
                 * int32 (stride 0x11C) written by route/traffic code and
                 * consumed here for sub-tick tie-breaking. Writing 0 for
                 * the numerator matches the port's current behaviour and
                 * leaves lap-count and timer correct. */
                if (m->post_finish_metric_base == 0) {
                    m->post_finish_metric_base = m->cumulative_timer;

                    /* finish_denom: clamp(longitudinal_speed>>8, min 1).
                     * Original signed-right-shift with sign correction
                     * matches arithmetic divide-by-256. */
                    int32_t ls = (actor->longitudinal_speed + ((actor->longitudinal_speed >> 31) & 0xFFu));
                    int32_t denom = ls >> 8;
                    if (denom < 1) denom = 1;

                    /* gActorTrackSubProgress @ 0x004afc3c: per-slot int32, stride 0x11C.
                     * Sub-span fractional progress 0..255, used for sub-tick tie-breaking.
                     * [CONFIRMED @ 0x00409F76] formula: actor+0x336 = (sub_prog * 0x5DC) / denom
                     *
                     * The port approximates this by projecting the actor's world XZ position
                     * onto the current span's longitudinal axis (from the span's left vertex
                     * to the left vertex of the next span, clamped to 0..255).
                     * This faithfully captures the spatial meaning of sub-span progress
                     * at finish-line crossing. [INFERRED from UpdateTrafficRoutePlan read:
                     * span*0x100 + gActorTrackSubProgress = composite track progress.] */
                    int32_t sub_prog = 0;
                    {
                        int span_i = (int)actor->track_span_raw;
                        TD5_StripSpan *sp_cur  = td5_track_get_span(span_i);
                        TD5_StripSpan *sp_next = td5_track_get_span(span_i + 1);
                        if (sp_cur && sp_next) {
                            TD5_StripVertex *vl0 = td5_track_get_vertex((int)sp_cur->left_vertex_index);
                            TD5_StripVertex *vl1 = td5_track_get_vertex((int)sp_next->left_vertex_index);
                            if (vl0 && vl1) {
                                /* Span longitudinal axis vector (in strip-local 16-bit units) */
                                int32_t ax = (int32_t)vl1->x - (int32_t)vl0->x;
                                int32_t az = (int32_t)vl1->z - (int32_t)vl0->z;
                                int64_t len2 = (int64_t)ax * ax + (int64_t)az * az;
                                if (len2 > 0) {
                                    /* Actor position relative to span origin (strip-local units) */
                                    int32_t rx = (actor->world_pos.x >> 8) - (int32_t)sp_cur->origin_x - (int32_t)vl0->x;
                                    int32_t rz = (actor->world_pos.z >> 8) - (int32_t)sp_cur->origin_z - (int32_t)vl0->z;
                                    /* Dot product / length² gives fractional progress 0..1,
                                     * scaled to 0..255 to match gActorTrackSubProgress range. */
                                    int64_t dot = (int64_t)rx * ax + (int64_t)rz * az;
                                    int32_t prog256 = (int32_t)((dot * 256) / len2);
                                    if (prog256 < 0)   prog256 = 0;
                                    if (prog256 > 255) prog256 = 255;
                                    sub_prog = prog256;
                                }
                            }
                        }
                    }

                    /* Mirror into actor so UpdateRaceOrder / results-screen
                     * consumers see the same values the original wrote. */
                    actor->finish_time         = m->cumulative_timer;
                    actor->finish_time_aux     = (int16_t)denom;
                    actor->finish_time_subtick = (int16_t)((sub_prog * 0x5DC) / denom);
                }
                s_slot_state[slot].companion_1 = 1;
                /* [CONFIRMED @ 0x00409FCC] Original: param_1[1]='\x01' (companion_2=1),
                 * NOT 2. P2P branch at 0x0040A180 also writes '\x01'. Circuit was
                 * incorrectly setting companion_2=2. */
                s_slot_state[slot].companion_2 = 1;
                s_slot_state[slot].state = 2;

                TD5_LOG_I(LOG_TAG,
                          "Actor finish: slot=%d mode=circuit lap=%d timer=%d span=%d",
                          slot, m->checkpoint_index, m->cumulative_timer,
                          (int)actor_span);
            }
        }

        /* ---------------------------------------------------------------
         * 4-case sector bitmask dispatch @ LAB_0040A014.
         *   remaining = total - 2*start
         *   boundary  = 2*start + 1         (sector 0)
         *   step      = remaining / 5       (integer, signed)
         *   boundary_n = boundary + n*step  (n = 0..3)
         * Each sector promotes the mask only if the previous sector
         * latched (0→1, 1→3, 3→7, 7→0xF). This enforces one-direction
         * travel — a spun-out car moving backward cannot skip sectors to
         * reach 0xF and illegitimately trip the start-line lap increment.
         * --------------------------------------------------------------- */
        if (g_active_td6_level == 0) {  /* native sector anti-cut gate;
            * ALL TD6 tracks (g_active_td6_level>0) — whether OverrideTrackZip OR a
            * MENU-SELECTED slot (e.g. Egypt DefaultTrack=31) — use the simple armed
            * start-line crossing above and must NOT run this, because it reuses
            * checkpoint_bitmask as the 1-bit armed latch. [#20 EGYPT LAP FIX] The old
            * gate (override_track_zip==0) was TRUE for menu-selected TD6 circuits, so
            * this sector dispatch ran and set bitmask=0x01 at the first sector near the
            * start, which the TD6 lap logic misread as "armed at far side" -> false lap
            * at span ~19 -> race ended after a few spans. Keying on g_active_td6_level
            * matches the lap-logic condition at the top of this block. */
            int32_t remaining = total_spans - track_start * 2;
            int32_t boundary  = track_start * 2 + 1;
            int32_t step      = remaining / 5;   /* matches orig; signed div */

            for (int sector = 0; sector < 4; sector++) {
                if ((int32_t)actor_span >= (boundary - 2) &&
                    (int32_t)actor_span <= boundary) {
                    switch (sector) {
                    case 0:
                        if (m->checkpoint_bitmask == 0x00)
                            m->checkpoint_bitmask = 0x01;
                        break;
                    case 1:
                        if (m->checkpoint_bitmask == 0x01)
                            m->checkpoint_bitmask = 0x03;
                        break;
                    case 2:
                        if (m->checkpoint_bitmask == 0x03)
                            m->checkpoint_bitmask = 0x07;
                        break;
                    case 3:
                        if (m->checkpoint_bitmask == 0x07)
                            m->checkpoint_bitmask = 0x0F;
                        break;
                    }
                }
                boundary += step;
            }
        }
    } else {
        /* [TD6 P2P FINISH — track-scoped] Migrated TD6 point-to-point tracks
         * ship no checkpoint data (synth LEVELINF +0x08 == 0), so the native
         * checkpoint-finish below never fires and the race could not end. End
         * the race for these tracks when the player reaches the registered
         * finish span (near the end of the strip). Faithful TD5 P2P tracks have
         * s_td6_finish_span == 0 and fall through to the byte-faithful
         * checkpoint path unchanged. */
        if (s_td6_finish_span > 0) {
            /* [S18 FIX — reverse-at-start retirement] Gate the finish on the RAW
             * accumulated span (+0x84, span_accumulated), NOT the wrapped
             * normalized span (+0x82 = actor_span).
             *
             * On a TD6 P2P track the grid sits near span 0 (Rome/London
             * start_span=20, slot-0 spawns ~span 11). A small reverse off the
             * line decrements the raw accumulator below 0; NormalizeActorTrackWrap
             * State (@0x00443FB0) then wraps a negative raw to ring_length-1
             * (Rome: -1 -> 2353), and that high value spuriously satisfied
             * `actor_span >= finish_span` (2348) and ENDED the race after ~2 s of
             * reverse (the user-reported "reversing at the start kicks me off the
             * race entirely"; confirmed in log: "span=2353 finish=2348 timer=107").
             *
             * The raw accumulator is monotonic forward progress: it only reaches
             * finish_span by actually driving there and goes NEGATIVE on reverse,
             * so it never trips on a backward wrap. For a legitimate forward
             * finish raw == normalized (both < ring_length on a no-branch synth
             * P2P ring), so this is byte-identical for the normal finish and only
             * differs on the backward-wrap case. */
            int32_t raw_acc = actor
                ? (int32_t)*(int16_t *)((uint8_t *)actor + 0x84)
                : (int32_t)actor_span;
            if (s_slot_state[slot].companion_1 == 0 &&
                raw_acc >= s_td6_finish_span) {
                m->post_finish_metric_base = m->cumulative_timer;
                s_slot_state[slot].companion_1 = 1;
                s_slot_state[slot].companion_2 = 1;
                s_slot_state[slot].state = 2;
                TD5_LOG_I(LOG_TAG,
                          "Actor finish: slot=%d mode=td6-p2p raw=%d norm=%d finish=%d timer=%d",
                          slot, (int)raw_acc, (int)actor_span, s_td6_finish_span,
                          m->cumulative_timer);
            }
            return;   /* TD6 P2P: this is the only finish path (no checkpoints) */
        }

        /* Point-to-point / time trial: checkpoint crossing (0x409E80 P2P branch)
         * Original comparison: (int)(uint)(uint16_t)threshold <= (int)(int16_t)span
         *
         * LEVELINF +0x08 gate (asm 0x0040A047):
         *   MOV EAX,[g_trackEnvironmentConfig + 8]; TEST EAX,EAX; JZ 0x0040A1F7
         * When LEVELINF +0x08 is zero the original short-circuits the entire
         * P2P per-actor loop. Port mirrors this two ways:
         *   1. Asset load zeroes s_active_checkpoint.checkpoint_count when
         *      +0x08 == 0 (td5_game.c:1204), so the block below is a no-op.
         *   2. Explicit early-out here as a belt-and-braces guard against any
         *      path that leaves checkpoint_count stale while +0x08 is 0. */
        if (s_levelinf_checkpoint_config == 0) {
            if (slot == 0) {
                static int s_log_div = 0;
                if ((++s_log_div % 120) == 0) {
                    TD5_LOG_I(LOG_TAG,
                              "P2P checkpoint skipped: LEVELINF +0x08 == 0 (no checkpoints on track)");
                }
            }
            return;
        }
        if (m->checkpoint_index < s_active_checkpoint.checkpoint_count) {
            int cp = m->checkpoint_index;
            int threshold = (int)(unsigned int)s_active_checkpoint.checkpoints[cp].span_threshold;
            int span_val  = (int)actor_span;
            if (slot == 0) {
                static int s_log_div = 0;
                if ((++s_log_div % 60) == 0) {
                    TD5_LOG_I(LOG_TAG,
                              "P2P checkpoint watch: slot=0 cp=%d span=%d threshold=%d bonus=%d remaining_cps=%d",
                              cp, span_val, threshold,
                              (int)(int16_t)s_active_checkpoint.checkpoints[cp].time_bonus,
                              (int)s_active_checkpoint.checkpoint_count - cp);
                }
            }
            if (span_val >= threshold) {
                /* Crossed checkpoint: add time bonus */
                m->timer_ticks +=
                    (int16_t)s_active_checkpoint.checkpoints[cp].time_bonus;
                m->checkpoint_index++;
                m->normalized_span = actor_span;
                TD5_LOG_I(LOG_TAG,
                          "Checkpoint crossed: slot=%d cp=%d span=%d threshold=%d bonus=%d timer=%d",
                          slot, cp, span_val, threshold,
                          (int)(int16_t)s_active_checkpoint.checkpoints[cp].time_bonus,
                          m->cumulative_timer);

                /* Store split time — raw timing_frame_counter at crossing [CONFIRMED @ 0x00409E98] */
                if (cp < 9) {
                    m->lap_split_times[cp] = (int16_t)m->cumulative_timer;
                }
            }
        }

        /* Check if all checkpoints passed (skip if checkpoint data not loaded) */
        if (s_active_checkpoint.checkpoint_count > 0 &&
            m->checkpoint_index >= s_active_checkpoint.checkpoint_count) {
            m->post_finish_metric_base = m->cumulative_timer;
            if (m->average_speed_raw > 0) {
                int avg = (actor_span * 1500) / m->average_speed_raw;
                m->speed_bonus += (avg * 1000 - m->average_speed_raw * 1000 / 256);
            }
            s_slot_state[slot].companion_1 = 1;
            s_slot_state[slot].companion_2 = 1;
            s_slot_state[slot].state = 2;
            TD5_LOG_I(LOG_TAG,
                      "Actor finish: slot=%d mode=checkpoint checkpoints=%d timer=%d span=%d",
                      slot, m->checkpoint_index, m->cumulative_timer, (int)actor_span);
        }
    }
}

static void sync_actor_race_metrics(int slot)
{
    TD5_Actor *actor = td5_game_get_actor(slot);
    ActorRaceMetric *m = &s_metrics[slot];

    if (!actor) {
        return;
    }

    actor->finish_time = m->post_finish_metric_base;
    actor->pending_finish_timer = (m->timer_ticks > 0) ? (uint16_t)m->timer_ticks : 0;
    actor->timing_frame_counter = (int16_t)m->cumulative_timer;
    for (int i = 0; i < 9; i++)
        actor->checkpoint_split_times[i] = m->lap_split_times[i];
}

/* ========================================================================
 * Accumulate speed bonus (0x40A3D0)
 * ======================================================================== */

static void accumulate_speed_bonus(int slot) {
    /* [FIX 2026-05-24 OVERSIGHT: dead-fields; orig 0x0040A3D0 AccumulateVehicleSpeedBonusScore]
     *
     * Previous implementation read m->forward_speed / m->skid_factor /
     * m->contact_count which are NEVER WRITTEN anywhere in the codebase
     * (grep-confirmed: only declared in struct + read here). Effective no-op.
     *
     * Rewrite per orig 0x0040A3D0 to read directly from actor fields:
     *   gate: finish_time==0, surface_contact_flags==0, lateral_speed>0,
     *         (sim_tick_counter & 3) == 0
     *   bonus = (lateral_speed >> 15) - (race_position >> 1)
     *   clamp 0 if surface_type_chassis > 15 OR bonus < 0
     *   slot-0 only: bonus = 0 if track_span_normalized < track_span_high_water
     *   accumulate into actor->clean_driving_score (+0x2C8).
     */
    TD5_Actor *actor = td5_game_get_actor(slot);
    ActorRaceMetric *m = &s_metrics[slot];

    if (!actor) return;
    /* [FIX 2026-05-31 cop-chase] No passive speed/clean-driving bonus in cop
     * chase. In the original the clean-driving score is gated on the actor-9
     * special encounter, which is DISABLED in wanted mode (gSpecialEncounter
     * Enabled=0), so cop-chase POINTS = the RAM/BUST score ONLY — you do NOT
     * earn points just for driving. User-confirmed against the original; the
     * port previously accumulated this every tick into accumulated_score (the
     * field the cop-chase POINTS HUD reads), making POINTS tick up while
     * merely driving. Ram points (td5_game_add_wanted_score) still accrue. */
    if (g_td5.wanted_mode_enabled) return;
    if (s_slot_state[slot].companion_1 != 0) return;       /* finished */
    if (actor->finish_time != 0) return;                    /* finished (mirrors orig actor+0x328 gate) */
    if (actor->surface_contact_flags != 0) return;          /* in contact / airborne flag set */
    if (actor->lateral_speed <= 0) return;
    if ((g_td5.simulation_tick_counter & 3) != 0) return;

    int bonus = (actor->lateral_speed >> 15) - ((int)actor->race_position >> 1);
    if (actor->surface_type_chassis > 15 || bonus < 0) bonus = 0;

    /* Slot-0 only: no bonus if behind high-water mark (orig per-slot gate). */
    if (slot == 0 && actor->track_span_normalized < actor->track_span_high_water) {
        bonus = 0;
    }

    actor->clean_driving_score += bonus;
    /* Keep per-slot mirror so HUD/serialization paths that already read
     * m->accumulated_score continue to function. */
    m->accumulated_score += bonus;
}

/* ========================================================================
 * Decay ultimate variant timer (0x40A440)
 * ======================================================================== */

static void decay_ultimate_timer(int slot) {
    ActorRaceMetric *m = &s_metrics[slot];

    if (s_slot_state[slot].companion_1 != 0) return;
    if (g_td5.race_rule_variant != 4) return;   /* Ultimate mode only */

    m->accumulated_score -= 1;
    if (m->accumulated_score < 0) m->accumulated_score = 0;
}

/* ========================================================================
 * Adjust checkpoint timers by difficulty (0x40A530)
 *
 * Applied once per human player during InitializeRaceSession.
 *   Easy   (tier 0): +20% time (multiply by 12/10)
 *   Normal (tier 1): +10% time (multiply by 11/10)
 *   Hard   (tier 2): no adjustment (baseline)
 *
 * [L5 promotion sweep audit 2026-05-18 — byte-equivalent]
 *   Decompile of AdjustCheckpointTimersByDifficulty @ 0x0040A530 verified
 *   line-by-line against this routine. The two scaling branches (tier=0
 *   ×12/10, tier=1 ×11/10) and the unconditional actor field clears
 *   (+0x344=table[+2], +0x37E=0, +0x328=0, +0x34C=0) all reproduce here
 *   with semantic-equivalent writes into the port's ActorRaceMetric.
 *
 * [ARCH-DIVERGENCE — per-slot ActorRaceMetric vs in-place table mutation]
 *   Original mutates g_raceCheckpointTablePtr[+2] AND the per-checkpoint
 *   time_bonus entries IN PLACE, then re-reads on subsequent slot calls.
 *   The port stores the scaled values into s_active_checkpoint (per-race)
 *   plus per-slot s_metrics[]. To preserve idempotency the port gates the
 *   scaling on slot==0 (mirroring orig's +0x375==0 / "is human player"
 *   discriminator). Value identity across all six racer slots is
 *   preserved — only the storage layout differs.
 * ======================================================================== */

static void adjust_checkpoint_timers(int slot) {
    ActorRaceMetric *m = &s_metrics[slot];
    int numerator = 10, denominator = 10;

    /* Original AdjustCheckpointTimersByDifficulty @ 0x0040A530:
     *   1. If +0x375 == 0 (slot 0): scale *(ptr+2) and each checkpoint
     *      bonus in place by difficulty tier.
     *   2. Then unconditionally: actor+0x344 = *(ptr+2), and clear +0x37E,
     *      +0x328, +0x34C on the actor.
     * The in-place table mutation is why scaling happens at most once:
     * subsequent slot-0 (re-entry) or non-slot-0 calls re-read the already
     * scaled table and produce identical values. */

    int is_slot_zero = (slot == 0);

    /* Step 1 — scale table in place, gated on slot 0.
     * Original AdjustCheckpointTimersByDifficulty @ 0x0040A530 reads
     * gRaceDifficultyTier (@ 0x00463210, .data-initialized to 2 = Hard),
     * NOT the user-selected difficulty. The port mirrors this via
     * g_td5.difficulty_tier (initialized to 2 in td5re.c). Previously
     * the port read g_td5.difficulty (user-selected, default Normal=1),
     * which caused an unintended 1.1× scale on the Hard-tier default
     * binary state. [CONFIRMED @ 0x0040A55A reads gRaceDifficultyTier,
     * 0x0040A57F scales *12/10, 0x0040A5AF scales *11/10.] */
    if (is_slot_zero) {
        switch (g_td5.difficulty_tier) {
        case 0: numerator = 12; break;  /* tier 0 (Easy AI):   +20% */
        case 1: numerator = 11; break;  /* tier 1 (Normal AI): +10% */
        case 2:
        default:
            numerator = 10; break;       /* tier 2 (Hard AI):  no change */
        }
        TD5_LOG_I(LOG_TAG,
                  "adjust_checkpoint_timers: tier=%d scale=%d/10 initial_time=%d -> scaled=%d",
                  g_td5.difficulty_tier, numerator,
                  (int)s_active_checkpoint.initial_time,
                  (int)((int)s_active_checkpoint.initial_time * numerator / denominator));
        if (numerator != denominator) {
            s_active_checkpoint.initial_time =
                (uint16_t)((int)s_active_checkpoint.initial_time *
                           numerator / denominator);
            for (int i = 0; i < (int)s_active_checkpoint.checkpoint_count && i < 5; i++) {
                s_active_checkpoint.checkpoints[i].time_bonus =
                    (uint16_t)((int)s_active_checkpoint.checkpoints[i].time_bonus *
                               numerator / denominator);
            }
        }
    }

    /* Step 2 — seed actor+0x344 (= m->timer_ticks) + clear timers.
     * s_active_checkpoint.initial_time is now the already-scaled value,
     * so every slot reads the same final value as the original. */
    m->timer_ticks = (int16_t)s_active_checkpoint.initial_time;
    m->checkpoint_index = 0;
    m->post_finish_metric_base = 0;
    m->cumulative_timer = 0;
}

/* ========================================================================
 * Build race results table (0x40A8C0)
 *
 * Populates s_results from s_metrics, then sorts by the appropriate
 * metric for the current game type.
 * ======================================================================== */

static void build_results_table(void) {
    int i;
    int battle = td5_game_battle_mode_active();   /* [TRAFFIC BATTLE 2026-06-28] */

    for (i = 0; i < TD5_MAX_RACER_SLOTS; i++) {
        ActorRaceMetric *m = &s_metrics[i];
        RaceResultEntry *r = &s_results[i];
        TD5_Actor *actor_i = td5_game_get_actor(i);

        r->slot_flags = (s_slot_state[i].state != 3) ? 1 : 0;
        r->slot_index = (uint8_t)i;

        /* If AI didn't actually finish, synthesize a finish time */
        if (m->post_finish_metric_base == 0 && s_slot_state[i].state != 3) {
            int estimated = (rand() & 0x1F) + m->cumulative_timer + 100;
            m->post_finish_metric_base = estimated;
        }

        r->primary_metric   += m->post_finish_metric_base;
        r->secondary_metric += m->accumulated_score;
        /* [FIX 2026-05-25 hud-metrics; orig 0x0040AA0F BuildResultsTable]
         *   Orig reads peak_speed/avg_speed/wanted_kills from the ACTOR
         *   (psVar7[0x155]=actor+0x330, psVar7[0x156]=actor+0x332,
         *   psVar7[0x17f]=actor+0x384). Port's s_metrics mirrors are dead
         *   fields (zero writers). Read directly from actor to match. */
        if (actor_i) {
            r->wanted_kills = (uint8_t)(actor_i->special_encounter_state & 0xFF);
            r->speed_bonus  += actor_i->average_speed_metric;
            if (actor_i->peak_speed > r->top_speed)
                r->top_speed = actor_i->peak_speed;
        } else {
            r->wanted_kills = (uint8_t)m->wanted_kills;
            r->speed_bonus += m->speed_bonus;
            if (m->top_speed > r->top_speed)
                r->top_speed = (int16_t)m->top_speed;
        }

        /* Award position points based on game type */
        int pos = s_race_order[i];
        if (battle) {
            /* [TRAFFIC BATTLE 2026-06-28] Placement is ignored — the ONLY metric
             * that matters is WRECKS (wanted_kills). Drive the descending sort
             * off secondary_metric (which sort_results_by_score_desc ranks) so we
             * can reuse that routine: override it to the wreck count and skip the
             * championship / ultimate position points entirely. */
            r->secondary_metric = (int32_t)r->wanted_kills;
        } else if (g_td5.race_rule_variant == 0 && pos < TD5_MAX_RACER_SLOTS) {
            /* Championship: {15, 12, 10, 5, 4, 3} */
            r->secondary_metric += s_championship_points[pos];
        } else if (g_td5.race_rule_variant == 4 && pos < TD5_MAX_RACER_SLOTS) {
            /* Ultimate: {1000, 500, 250, 0, 0, 0} */
            r->secondary_metric += s_ultimate_points[pos];
        }
    }

    /* Sort results by the appropriate metric for the game type */
    if (battle) {
        /* [TRAFFIC BATTLE 2026-06-28] Winner = most WRECKS (secondary_metric was
         * set to the wreck count above). Reuse the descending-score sort. */
        sort_results_by_score_desc();
        return;
    }
    switch (g_td5.game_type) {
    case TD5_GAMETYPE_CHAMPIONSHIP:
    case TD5_GAMETYPE_ULTIMATE:
        sort_results_by_score_desc();
        break;
    case TD5_GAMETYPE_ERA:
    case TD5_GAMETYPE_CHALLENGE:
    case TD5_GAMETYPE_PITBULL:
    case TD5_GAMETYPE_MASTERS:
    default:
        sort_results_by_time_asc();
        break;
    }
}

/* ========================================================================
 * Reset results table (0x40A880)
 *
 * [CONFIRMED @ 0x0040a880 ResetRaceResultsTable; L5 promotion sweep audit
 *  2026-05-18] -- ARCH-DIVERGENCE, behaviour-equivalent.
 *
 * Orig per-entry zero with byte[1]=entry_idx priming, then entry[0].byte[0]=1.
 * Port memsets to zero then sets entry[0].slot_flags=1. The orig's per-entry
 * slot_index priming is overwritten by build_results_table() (line ~3674)
 * before any reader observes the table, so the zero-instead-of-index init
 * is byte-equivalent for all observable port state. See header L5 audit
 * block (line ~107) for full rationale.
 * ======================================================================== */

static void reset_results_table(void) {
    memset(s_results, 0, sizeof(s_results));
    s_results[0].slot_flags = 1;  /* mark entry 0 as active */
}

/* ========================================================================
 * [REPLAY RESULTS PRESERVE 2026-06-27] Snapshot / restore the full
 * race-results display state across a View Replay.
 *
 * capture: called at the top of init_race_session when a replay is starting,
 *   while the real race's results are still live (before the per-race resets
 *   wipe them).
 * restore: called from the race FSM when a replay ends, so whichever screen
 *   shows the results (SP race results, MP post-race / cup standings) reads the
 *   genuine race outcome rather than the replay playback's partial result.
 *
 * The captured set covers everything the results/standings accessors read:
 *   s_results[]            -> get_result_primary/secondary/top/finish_position
 *   s_metrics[]            -> slot_is_finished (post_finish_metric_base) +
 *                             primary/secondary/avg/top fallbacks
 *   s_slot_state[]         -> get_slot_companion_2 (eliminated-mid-cup)
 *   s_race_order[]         -> get_race_order (finishing order)
 *   s_slot_finish_place[]  -> slot_finish_place (per-viewport indicator)
 *   s_arrest_time[]        -> cop-chase arrest times
 *   s_finish_position_display -> centered victory digit
 * ======================================================================== */
static void capture_replay_results_snapshot(void) {
    memcpy(s_saved_results,           s_results,           sizeof(s_results));
    memcpy(s_saved_metrics,           s_metrics,           sizeof(s_metrics));
    memcpy(s_saved_slot_state,        s_slot_state,        sizeof(s_slot_state));
    memcpy(s_saved_race_order,        s_race_order,        sizeof(s_race_order));
    memcpy(s_saved_slot_finish_place, s_slot_finish_place, sizeof(s_slot_finish_place));
    memcpy(s_saved_arrest_time,       s_arrest_time,       sizeof(s_arrest_time));
    s_saved_finish_position_display = s_finish_position_display;
    s_replay_results_saved = 1;
    TD5_LOG_I(LOG_TAG, "Replay start: captured real race results "
              "(slot0 finished=%d primary=%d, order0=%d)",
              td5_game_slot_is_finished(0), (int)td5_game_get_result_primary(0),
              (int)s_race_order[0]);
}

static void restore_replay_results_snapshot(void) {
    if (!s_replay_results_saved) return;
    memcpy(s_results,           s_saved_results,           sizeof(s_results));
    memcpy(s_metrics,           s_saved_metrics,           sizeof(s_metrics));
    memcpy(s_slot_state,        s_saved_slot_state,        sizeof(s_slot_state));
    memcpy(s_race_order,        s_saved_race_order,        sizeof(s_race_order));
    memcpy(s_slot_finish_place, s_saved_slot_finish_place, sizeof(s_slot_finish_place));
    memcpy(s_arrest_time,       s_saved_arrest_time,       sizeof(s_arrest_time));
    s_finish_position_display = s_saved_finish_position_display;
    s_replay_results_saved = 0;
    TD5_LOG_I(LOG_TAG, "Replay end: restored real race results "
              "(slot0 finished=%d primary=%d, order0=%d)",
              td5_game_slot_is_finished(0), (int)td5_game_get_result_primary(0),
              (int)s_race_order[0]);
}

/* Number of actual race participants = the contiguous prefix of racer slots
 * that are NOT disabled (state != 3).
 *
 * [FIX 2026-06-05 finish-position-15-16] The original ranks only its fixed
 * racer field — UpdateRaceOrder @0x0042F5B0 writes the position byte (+0x831b)
 * for the first 6 entries only [CONFIRMED @0x0042F6C0 CMP EAX,0x6] and gates
 * the sort on the participation flag +0x82c0==0 [CONFIRMED @0x0042F5D9]. The
 * port grew to 16 racer slots (N-way split) but kept the sort/rank loops at
 * TD5_MAX_RACER_SLOTS, so inactive decoration slots (a 2-car Quick Race leaves
 * slots 2..5 spawned-but-disabled, and 6..15 never spawn) got display_position
 * 0..15 too. Worse, build_results_table synthesizes a finish time ONLY for
 * active slots (td5_game.c:5380), so the disabled slots keep primary_metric==0
 * and sort_results_by_time_asc floats them to the FRONT of the SHARED
 * s_race_order; update_race_order's post-finish `continue` skips then freeze
 * that order into display_position, pushing the player to index 14 -> "15".
 *
 * The port's analogue of the original participation gate is s_slot_state.state
 * != 3 (every mode parks its dropped/decoration slots there: Quick Race
 * dropped opponents 1500-1502, cop-chase 1547-1552, drag 1521-1525, time-trial
 * 1448-1451). Those disabled slots are always the TAIL, so the active racers
 * occupy s_race_order[0 .. count). Bounding all sorts + position writes to this
 * count ranks only real participants (rank 0..N-1) and never lets an inactive
 * slot receive a standings position. */
static int active_racer_count(void) {
    int base = g_traffic_slot_base;            /* racer/traffic boundary */
    if (base < 1) base = 1;
    if (base > TD5_MAX_RACER_SLOTS) base = TD5_MAX_RACER_SLOTS;
    int n = 0;
    for (int i = 0; i < base; i++) {
        if (s_slot_state[i].state != 3) n++;   /* 3 == disabled/decoration */
    }
    if (n < 1) n = 1;                           /* slot 0 always participates */
    if (n > TD5_MAX_RACER_SLOTS) n = TD5_MAX_RACER_SLOTS;
    return n;
}

/* [DRAG DYNAMIC FIELD 2026-06-27] How many cars — and therefore how many track
 * lanes — a dynamic drag race uses. Scales with the configured field
 * (humans + AI opponents), clamped to [2, max]. This is the SINGLE source of
 * truth: the track lane-widener (td5_track_drag_apply_geometry) and the drag
 * slot/decoration + spawn logic all read THIS, so the road width and the number
 * of racers can never disagree. NFS-Underground-style: a lane per car.
 * Knobs:
 *   TD5RE_DRAG_DYNAMIC=0   disable widening -> faithful 2-car drag
 *   TD5RE_DRAG_LANES=N     force the field to exactly N (clamped [2,8])
 *   TD5RE_DRAG_LANE_MAX=N  cap the auto field (default 8) */
int td5_game_drag_field_size(void)
{
    const char *dyn = getenv("TD5RE_DRAG_DYNAMIC");
    if (dyn && dyn[0] == '0')
        return 2;                          /* faithful 2-car field */

    int maxlanes = td5_env_int("TD5RE_DRAG_LANE_MAX", 8, 2, 8);  /* ceiling: int8 sub_lane + int16 vtx headroom */

    int n;
    const char *force = getenv("TD5RE_DRAG_LANES");
    if (force && force[0])
        n = atoi(force);                   /* dev override first */
    else if (td5_game_drag_mp_active())
        /* MP DRAG: one lane per human player PLUS the EXTRA LANES option. Over
         * the net every player is a separate machine folded into
         * num_ai_opponents (num_human_players is forced to 1), so the human
         * count is num_human_players + num_ai_opponents on every peer. */
        n = (g_td5.network_active
                 ? g_td5.num_human_players + g_td5.num_ai_opponents
                 : g_td5.num_human_players)
            + g_td5.mp_mode_config.drag_extra_lanes;
    else
        n = g_td5.num_human_players + g_td5.num_ai_opponents;

    if (n < 2) n = 2;                      /* always at least you + 1 rival */
    if (n > maxlanes) n = maxlanes;
    return n;
}

/* [DRAG RACE MP 2026-06-30] How many drag cars actually RACE — decoupled from how
 * many LANES the road is widened to (td5_game_drag_field_size). SP drag = one car
 * per lane (player + AI). MP drag has NO AI opponents — only the human players race
 * and the EXTRA LANES are empty extra road — so the active car count is the human
 * count, NOT the (wider) lane count. Using the lane count here is exactly what put
 * CPU cars in the extra lanes. */
int td5_game_drag_active_racers(void)
{
    int n;
    if (td5_game_drag_mp_active())
        /* humans only; extra lanes stay empty. Over the net the other players
         * are folded into num_ai_opponents (num_human_players==1), so count
         * both there to get the true racer count on every peer. */
        n = g_td5.network_active
                ? g_td5.num_human_players + g_td5.num_ai_opponents
                : g_td5.num_human_players;
    else
        n = td5_game_drag_field_size();    /* SP: a car (player/AI) per lane */
    if (n < 1) n = 1;
    if (n > TD5_MAX_RACER_SLOTS) n = TD5_MAX_RACER_SLOTS;
    return n;
}

/* [DRAG LENGTH 2026-06-29] The chosen drag LENGTH level: 0=SHORT 1=MEDIUM
 * 2=LONG 3=EPIC. From the menu option (g_td5.ini.drag_length); env
 * TD5RE_DRAG_LENGTH_LEVEL overrides for testing. */
static int drag_length_level(void)
{
    const char *e = getenv("TD5RE_DRAG_LENGTH_LEVEL");
    int v;
    if (e && e[0])
        v = atoi(e);                                     /* dev override first */
    else if (td5_game_drag_mp_active())
        v = g_td5.mp_mode_config.drag_length;            /* MP DISTANCE option */
    else
        v = g_td5.ini.drag_length;                       /* SP menu / INI      */
    if (v < 0) v = 0;
    if (v > 3) v = 3;
    return v;
}

/* [DRAG DISTANCE 2x 2026-06-30] Clean straight spans to INSERT mid-strip so the
 * drivable strip physically extends beyond its ~300-span data. SHORT is unchanged;
 * MEDIUM/LONG/EPIC are 100% LONGER than their original race distance, which overruns
 * the strip, so we insert clean straight spans at ins (~150, mid racing-straight)
 * to reach the doubled finish. The inserted road is painted by the procedural strip
 * ribbon and enclosed by the stadium tiling (td5_render.c); the route/boundary shift
 * to follow (td5_track.c). The finish lands at ins+extra-4 == start + doubled_dist. */
int td5_game_drag_length_repeats(void)
{
    int ring  = g_td5.track_span_ring_length;
    int start = 115;                         /* drag spawn span (see spawn override) */
    int avail, ins, base_dist, want_dist, extra;
    if (ring < 16) ring = 300;
    avail = ring - start;
    if (avail < 8) avail = 8;
    ins = 150;                               /* MUST match td5_track_drag_apply_length */
    if (ins >= ring - 8) ins = ring / 3;
    if (ins < 8)         ins = 8;
    switch (drag_length_level()) {
        case 1: base_dist = (avail * 48) / 100;  break;    /* MEDIUM */
        case 2: base_dist = (avail * 85) / 100;  break;    /* LONG */
        case 3: base_dist = (ring - 22) - start; break;    /* EPIC */
        default: return 0;                                  /* SHORT — unchanged */
    }
    want_dist = base_dist * 2;                              /* 100% longer */
    extra = want_dist - (ins - start - 4);                 /* finish = ins+extra-4 = start+want_dist */
    if (extra < 4) return 0;
    extra = (extra / 4) * 4;                                /* whole 4-span blocks */
    return extra;
}

/* The finish-line span for the chosen LENGTH option. With NO insertion (SHORT, or if
 * the insertion bailed) it returns the ORIGINAL preset finish; MEDIUM/LONG/EPIC
 * normally finish on the inserted road (td5_track_drag_finish_span, doubled). These
 * are the un-doubled fallbacks. base_ring = the strip ring length; caller clamps to
 * ring-2. */
int td5_game_drag_length_finish_span(int start, int base_ring)
{
    int avail = base_ring - start;                  /* drivable straight ahead of spawn */
    if (avail < 8) avail = 8;
    switch (drag_length_level()) {
        case 0:  return start + (avail * 30) / 100;          /* SHORT  ~170 */
        case 1:  return start + (avail * 48) / 100;          /* MEDIUM (fallback) */
        case 2:  return start + (avail * 85) / 100;          /* LONG   (fallback) */
        case 3:  return base_ring - 22;                      /* EPIC   (fallback) */
        default: return start + (avail * 48) / 100;
    }
}

/* ========================================================================
 * Sort results by primary metric ascending (fastest wins)
 * Bubble sort on s_race_order, matching SortRaceResultsByPrimaryMetricAsc
 * (0x40AAD0). Used for game types 2-5 (Era, Challenge, Pitbull, Masters).
 * ======================================================================== */

static void sort_results_by_time_asc(void) {
    /* Rank only the active racers (s_race_order[0..n) — see active_racer_count).
     * Disabled slots have primary_metric==0 and would otherwise float to the
     * front of the shared order table. */
    int n = active_racer_count();
    int swapped;
    do {
        swapped = 0;
        for (int i = 0; i < n - 1; i++) {
            int a = s_race_order[i];
            int b = s_race_order[i + 1];
            /* Compare: primary_metric * 100 / 30 ascending (lower = better) */
            int32_t val_a = s_results[a].primary_metric * 100 / 30;
            int32_t val_b = s_results[b].primary_metric * 100 / 30;
            if (val_a > val_b) {
                s_race_order[i]     = (uint8_t)b;
                s_race_order[i + 1] = (uint8_t)a;
                swapped = 1;
            }
        }
    } while (swapped);

    /* Write final positions (active racers only) */
    for (int i = 0; i < n; i++) {
        s_results[s_race_order[i]].final_position = (int16_t)i;
    }
}

/* ========================================================================
 * Sort results by secondary metric descending (most points wins)
 * Bubble sort matching SortRaceResultsBySecondaryMetricDesc (0x40AB80).
 * Used for game types 1, 6 (Championship, Ultimate).
 * ======================================================================== */

static void sort_results_by_score_desc(void) {
    /* Rank only the active racers (see active_racer_count). */
    int n = active_racer_count();
    int swapped;
    do {
        swapped = 0;
        for (int i = 0; i < n - 1; i++) {
            int a = s_race_order[i];
            int b = s_race_order[i + 1];
            if (s_results[a].secondary_metric < s_results[b].secondary_metric) {
                s_race_order[i]     = (uint8_t)b;
                s_race_order[i + 1] = (uint8_t)a;
                swapped = 1;
            }
        }
    } while (swapped);

    /* Write final positions (active racers only) */
    for (int i = 0; i < n; i++) {
        s_results[s_race_order[i]].final_position = (int16_t)i;
    }
}

/* ========================================================================
 * Update race order (0x42F5B0)
 *
 * Bubble sort s_race_order by normalized span position (forward progress).
 * Called every sim tick inside RunRaceFrame.
 * ======================================================================== */

static void update_race_order(void) {
    /* UpdateRaceOrder @ 0x0042F5B0 — original sorts g_raceOrderTable[6]
     * by actor+0x86 (track_span_high_water) DESCENDING, not by +0x82.
     * Rank ONLY active racers: the original's rank loop bound was the fixed
     * 6-racer field [CONFIRMED @0x0042F6C0]; the port's faithful analogue is
     * active_racer_count() (disabled slots are state==3 in the tail). Bounding
     * here keeps inactive decoration slots out of the standings — the cause of
     * the "15/16" finish display in a 2-car race. */
    int n = active_racer_count();
    int swapped;
    do {
        swapped = 0;
        for (int i = 0; i < n - 1; i++) {
            int a = s_race_order[i];
            int b = s_race_order[i + 1];

            /* Skip already-finished actors in span comparison */
            if (s_metrics[a].post_finish_metric_base != 0) continue;
            if (s_metrics[b].post_finish_metric_base != 0) continue;

            TD5_Actor *actor_a = td5_game_get_actor(a);
            TD5_Actor *actor_b = td5_game_get_actor(b);
            /* Disabled slots (state 3 — SoloRace / solo-synth opponents) sort to
             * the BACK so they never outrank an active racer in the standings. */
            int32_t span_a = (s_slot_state[a].state == 3) ? INT32_MIN
                             : (actor_a ? (int32_t)actor_a->track_span_high_water : 0);
            int32_t span_b = (s_slot_state[b].state == 3) ? INT32_MIN
                             : (actor_b ? (int32_t)actor_b->track_span_high_water : 0);

            /* Decide whether b ranks ahead of a (swap so the further-along car
             * gets the lower index / better position). */
            int swap_ab;
            if (g_td5.track_type == TD5_TRACK_CIRCUIT) {
                /* [BUG fix 2026-06-24 — "finished 2nd despite winning" on circuits]
                 * track_span_high_water (actor+0x86) is an int16_t cumulative span
                 * counter. The original UpdateRaceOrder @0x0042F5B0 sorts DESC by a
                 * lap-aware cumulative value [CONFIRMED @0x0042F5E3/0x00443FB0], but
                 * on a multi-lap circuit the port's int16 high-water can lose its
                 * cross-lap monotonicity (sign-wrap past 32767, or under-counted
                 * span crosses), letting an AI on the SAME or a LOWER lap outrank the
                 * leader — the player crossed the line first yet was written
                 * race_position 1 ("2nd"). Rank by the INDEPENDENT lap counter
                 * (checkpoint_index, advanced by the finish-line detector, NOT the
                 * walker accumulator) FIRST, then by high-water within a lap (where a
                 * single lap can never overflow int16). For non-overflowing fields
                 * this is IDENTICAL to the old high-water-only order — high-water is
                 * monotonic with lap — so working races are unchanged. */
                int32_t lap_a = (s_slot_state[a].state == 3) ? INT32_MIN
                                : (int32_t)s_metrics[a].checkpoint_index;
                int32_t lap_b = (s_slot_state[b].state == 3) ? INT32_MIN
                                : (int32_t)s_metrics[b].checkpoint_index;
                if (lap_a != lap_b) {
                    swap_ab = (lap_a < lap_b);
                    static int s_lap_rank_logged = 0;
                    if (!s_lap_rank_logged) {
                        s_lap_rank_logged = 1;
                        TD5_LOG_I(LOG_TAG,
                                  "update_race_order: circuit lap-aware rank active "
                                  "(slot %d lap=%d hw=%d vs slot %d lap=%d hw=%d)",
                                  a, (int)lap_a, (int)span_a,
                                  b, (int)lap_b, (int)span_b);
                    }
                } else {
                    swap_ab = (span_a < span_b);   /* same lap: within-lap progress */
                }
            } else {
                /* P2P: higher span_high = further ahead = better (lower index). */
                swap_ab = (span_a < span_b);
            }

            if (swap_ab) {
                s_race_order[i]     = (uint8_t)b;
                s_race_order[i + 1] = (uint8_t)a;
                swapped = 1;
            }
        }
    } while (swapped);

    /* Write display positions (active racers only — rank 0..n-1) */
    for (int i = 0; i < n; i++) {
        s_metrics[s_race_order[i]].display_position = (int16_t)i;
    }

    /* Time trial tiebreaker: if both slots 0 and 1 finished, compare by
     * finish_time * 256 - post_finish_metric for ms-level precision */
    if (g_td5.time_trial_enabled &&
        s_metrics[0].post_finish_metric_base != 0 &&
        s_metrics[1].post_finish_metric_base != 0) {
        int32_t t0 = s_metrics[0].post_finish_metric_base;
        int32_t t1 = s_metrics[1].post_finish_metric_base;
        if (t0 <= t1) {
            s_metrics[0].display_position = 0;
            s_metrics[1].display_position = 1;
        } else {
            s_metrics[0].display_position = 1;
            s_metrics[1].display_position = 0;
        }
    }

    for (int i = 0; i < n; i++) {
        TD5_Actor *actor = td5_game_get_actor(i);
        if (!actor) {
            continue;
        }
        actor->prev_race_position = actor->race_position;
        actor->race_position = (uint8_t)s_metrics[i].display_position;
    }
}

/* ========================================================================
 * [END RACE NOW 2026-06-30] Pause-menu force-finish
 *
 * Ends the race immediately, classifying every still-racing car by its CURRENT
 * track-progress rank (no DNFs). For split-screen / MP situations where one
 * player is stuck and can never reach the finish, blocking the
 * all-humans-finished completion latch in check_race_completion().
 *
 * It reuses the normal finish path rather than inventing a new results route:
 *   1. Pre-seed each unfinished active car's synthesized finish metric
 *      (post_finish_metric_base) from its live display_position — the HUD
 *      position rank update_race_order() writes by track progress every tick.
 *      build_results_table() only synthesizes a time when this is 0 and the
 *      results sort (sort_results_by_time_asc) ranks by primary_metric, which
 *      derives from this field; so seeding it by progress makes the results
 *      reproduce the current on-track order regardless of game type. The seed is
 *      offset past the largest REAL finish metric so cars that already crossed
 *      the line keep their true times and stay ahead. Setting it non-zero also
 *      makes update_race_order() freeze their order from here on (it skips
 *      already-"finished" actors), so a resumed tick can't reshuffle them.
 *   2. Latch the post-finish cooldown exactly as the all-finished /
 *      cop-chase-resolved paths do, so check_race_completion() builds results +
 *      awards cup points and the FSM fades to the results screen next tick.
 *
 * Port-only feature — the original had no force-finish. The net-synced trigger
 * (RACE_FORCE_FINISH 0x1A) is sent by the pause dispatch so every machine ends
 * together; each machine seeds from its own last-synced lockstep state. If the
 * 0x1A arrives a round or two apart the standings can differ by a hair on close
 * calls — acceptable for an "abort the race" action, and the same fire-and-handle
 * shape the shipped RACE_LEFT (0x17) / RACE_RESTART (0x19) paths already use.
 * ======================================================================== */
static void td5_game_force_finish_race(void)
{
    int i;

    if (s_post_finish_cooldown != 0) return;   /* race already ending — ignore re-trigger */

    int32_t max_real = 0;
    for (i = 0; i < TD5_MAX_RACER_SLOTS; i++) {
        if (s_slot_state[i].state == 3) continue;                 /* disabled/decoration */
        if (s_metrics[i].post_finish_metric_base > max_real)
            max_real = s_metrics[i].post_finish_metric_base;
    }
    for (i = 0; i < TD5_MAX_RACER_SLOTS; i++) {
        if (s_slot_state[i].state == 3) continue;
        if (s_metrics[i].post_finish_metric_base == 0) {          /* hasn't crossed the line */
            /* Worse (larger) than any real finish time, ordered by live progress
             * rank so the still-racing cars rank by current track position. */
            s_metrics[i].post_finish_metric_base =
                max_real + 1 + (int32_t)s_metrics[i].display_position;
        }
    }

    s_post_finish_cooldown = 1;
    s_race_end_timer_start = td5_plat_time_ms();
    TD5_LOG_I(LOG_TAG,
              "Force-finish (END RACE NOW): ending race; %d active car(s) ranked by current progress",
              active_racer_count());
}

/* [END RACE NOW 2026-06-30] Whether the force-finish confirmation prompt is up
 * (read by the HUD overlay to draw the YES/NO modal). */
int td5_game_pause_endrace_confirm_active(void)
{
    return s_pause_endrace_confirm;
}

/* [PAUSE CONFIRM 2026-07-02] Which pause-menu RESTART/QUIT/EXIT/LOBBY
 * confirmation (if any) is up: 0=none 1=RESTART RACE 2=QUIT TO MENU
 * 3=EXIT GAME 4=BACK TO LOBBY. Read by the HUD overlay to draw the matching
 * "ACTION?" YES/NO modal. */
int td5_game_pause_action_confirm(void)
{
    return s_pause_action_confirm;
}

/* [PAUSE CONFIRM 2026-07-02] Executes the pause-menu RESTART RACE / QUIT TO
 * MENU / EXIT GAME / BACK TO LOBBY action body. Called either immediately
 * (single-player, or MP without local split-screen — the original
 * unconfirmed behavior) or after the player answers YES to the "ACTION?"
 * prompt armed for local split-screen MP (num_human_players>1), where these
 * actions end the shared race for every pad on the screen.
 * which: 1=RESTART RACE 2=QUIT TO MENU 3=EXIT GAME 4=BACK TO LOBBY. */
static void td5_game_pause_execute_action(int which)
{
    if (which == 1) {
        /* RESTART RACE — re-run the SAME race (track/car/opponents/laps/
         * direction unchanged). Fade out, then the fade-complete handler
         * returns 3 and the FSM re-inits the race session. The dirty-volume
         * flush on menu-close (close-path) persists any SOUND slider change;
         * the race re-init re-plays the music.
         * [S31] Ignored in a net race unless we're the host: a one-sided
         * re-init can't be reconciled with the other machines' lockstep
         * state. */
        if (g_td5.network_active && td5_net_is_active()) {
            /* [S31] Net race: host-only and SYNCED. Broadcast RACE_RESTART
             * (0x19) so every machine runs the same fade + re-init; the
             * lockstep latch survives the restart (release_race_resources
             * keeps it while restart_pending) and InitRace re-seeds from the
             * archived DXPSTART config, so all machines rebuild the identical
             * race and resume the same exchange. */
            if (td5_net_is_host()) {
                uint8_t rs_msg[8] = {0x19, 0, 0, 0, 0, 0, 0, 0};
                td5_net_send(TD5_DXPDATA, rs_msg, 8);
                s_pause_menu_active = 0;
                s_pause_restart_pending = 1;
                TD5_LOG_I(LOG_TAG, "Pause menu: net RESTART (host), fading out");
                td5_game_begin_fade_out(0);
            } else {
                TD5_LOG_I(LOG_TAG, "Pause menu: RESTART is host-only in net races");
            }
        } else {
            s_pause_menu_active = 0;
            s_pause_restart_pending = 1;
            TD5_LOG_I(LOG_TAG, "Pause menu: RESTART RACE selected, starting fade-out");
            td5_game_begin_fade_out(0);
        }
    } else if (which == 2) {
        /* QUIT TO MENU — leave the race, return to the frontend. */
        s_pause_menu_active = 0;
        s_pause_exit_pending = 1;
        /* PART A (user 2026-06-02): capture the player's CURRENT race position
         * so the centered finishing-position digit (td5_hud_draw_finish_position,
         * drawn during the fade whenever td5_game_get_victory_position() > 0)
         * shows "where I was before exiting". The finish path captures this at
         * completion (see check_race_completion), but the pause-Exit path began
         * the fade without it, so the digit was 0 and nothing drew. Mirror the
         * finish capture here. */
        {
            TD5_Actor *pl = td5_game_local_player_actor();
            s_finish_position_display = pl ? (int)pl->race_position + 1 : 1;
        }
        /* Flush any pending pause-slider volume change before the fade tears
         * down the race (close-path persist only runs on the continue path).
         * [PART B 2026-06-02] */
        if (s_pause_options_dirty) {
            g_td5.ini.sfx_volume   = td5_save_get_sfx_volume();
            g_td5.ini.music_volume = td5_save_get_music_volume();
            td5_ini_persist_options();
            s_pause_options_dirty = 0;
        }
        /* [S31] Tell the peers we are leaving the race so they end theirs too
         * (lockstep cannot continue without us) instead of stalling out their
         * input barrier. */
        if (g_td5.network_active && td5_net_is_active()) {
            uint8_t left_msg[8] = {0x17, 0, 0, 0, 0, 0, 0, 0};
            td5_net_send(TD5_DXPDATA, left_msg, 8);
        }
        TD5_LOG_I(LOG_TAG, "Pause menu: Quit-to-menu selected, starting fade-out (pos=%d)",
                  s_finish_position_display);
        /* Trigger fade-out; resources released when fade completes.
         * Original (0x43C317): calls BeginRaceFadeOutTransition(0). */
        td5_game_begin_fade_out(0);
    } else if (which == 3) {
        /* EXIT GAME — clean application shutdown (distinct from QUIT TO
         * MENU). Sets the same quit latch the frontend Quit button uses
         * (g_td5.quit_requested); the main loop tears the app down on its
         * next iteration. Persist any pending volume change first so it
         * survives the exit. */
        s_pause_menu_active = 0;
        if (s_pause_options_dirty) {
            g_td5.ini.sfx_volume   = td5_save_get_sfx_volume();
            g_td5.ini.music_volume = td5_save_get_music_volume();
            td5_ini_persist_options();
            s_pause_options_dirty = 0;
        }
        TD5_LOG_I(LOG_TAG, "Pause menu: EXIT GAME selected -> requesting clean shutdown");
        g_td5.quit_requested = 1;
    } else if (which == 4) {
        /* BACK TO LOBBY [S31] -- end the race and return to the lobby it
         * came from: network lobby (LAN/direct-IP) or local-MP lobby. Only
         * reachable in an MP session (row_lobby>=0 guard at the call site).
         * Net: tell the peers; lockstep cannot continue without us, so their
         * race ends through the same fade and they land back in the lobby
         * too (session stays alive). */
        s_pause_menu_active = 0;
        s_pause_lobby_pending = 1;
        {
            TD5_Actor *pl = td5_game_local_player_actor();
            s_finish_position_display = pl ? (int)pl->race_position + 1 : 1;
        }
        if (s_pause_options_dirty) {
            g_td5.ini.sfx_volume   = td5_save_get_sfx_volume();
            g_td5.ini.music_volume = td5_save_get_music_volume();
            td5_ini_persist_options();
            s_pause_options_dirty = 0;
        }
        if (g_td5.network_active && td5_net_is_active()) {
            uint8_t left_msg[8] = {0x17, 0, 0, 0, 0, 0, 0, 0};
            td5_net_send(TD5_DXPDATA, left_msg, 8);
        }
        TD5_LOG_I(LOG_TAG, "Pause menu: BACK TO LOBBY, starting fade-out");
        td5_game_begin_fade_out(0);
    }
}

/* ========================================================================
 * Race Flow
 * ======================================================================== */

int td5_game_check_race_completion(void) {
    return check_race_completion(td5_game_normalized_dt_to_accum(g_td5.normalized_frame_dt));
}

/* ========================================================================
 * BeginRaceFadeOutTransition (0x42CC20)
 *
 * Sets fade state and selects direction based on viewport layout.
 *   Single player: alternates horizontal/vertical (s_fade_direction_alternator)
 *   Horizontal split: always direction 0 (horizontal)
 *   Vertical split:   always direction 1 (vertical)
 *
 * [CONFIRMED @ 0x0042cc20 BeginRaceFadeOutTransition; L5 promotion sweep
 *  audit 2026-05-18] -- ARCH-DIVERGENCE, behaviour-equivalent.
 *
 * Orig structure (165 bytes):
 *   - Radial-pulse gate (param_1 == 1 && g_humanPlayerCount == 1 && actor
 *     field +0x383 == 0 && !network && game_type == 0 && !drag):
 *     calls ResetHudRadialPulseOverlay; sets g_raceEndRadialPulseEnabled=1.
 *   - Always sets g_raceEndFadeState = 1.
 *   - Direction dispatch on g_raceViewportLayoutMode:
 *       0 (single):   alternator ^= 1 with `& 0x80000001` sign repair
 *       1 (horiz):    direction = 0
 *       2 (vert):     direction = 1
 *
 * Port collapses the dual-axis dispatch into a single `param` axis. All
 * call sites in td5_game.c pass g_td5.split_screen_mode. The radial-pulse
 * gate is now WIRED (Tier 4 port 2026-05-24): td5_hud_reset_radial_pulse()
 * is invoked when single-player non-network non-drag matches orig's gate.
 * The render path was already live (td5_render.c:6202 + td5_hud.c:2514).
 * The alternator complex bit-twiddle is replaced with `^= 1` -- both
 * converge to alternating 0/1 across calls, byte-equivalent in
 * observable state. Port additionally zeroes s_fade_accumulator which
 * the orig does not -- orig relies on the accumulator already being at
 * its baseline; the port adds the explicit reset defensively. No
 * behaviour change in steady-state replay.
 * ======================================================================== */

void td5_game_begin_fade_out(int param) {
    g_td5.race_end_fade_state = 1;
    s_fade_accumulator = 0.0f;

    /* [#3 2026-06-20] Kill the race-start countdown the instant ANY exit fade
     * begins (quit / back-to-lobby / finish / restart all route through here).
     * g_cameraTransitionActive is only ever re-armed to 0xA000 by the next
     * race's reset_race_countdown(); it is NEVER cleared on exit, so if the
     * player leaves a race WHILE the 3/2/1 fly-in is still counting down (timer
     * still > 0), the digit kept rendering over the fade-out and bled into the
     * results / menu — the "old countdown when exiting a race" residual. Forcing
     * it to 0 here (HIDDEN, not armed) makes the countdown vanish immediately;
     * the RESTART path re-arms it cleanly when the same race re-inits. */
    g_cameraTransitionActive = 0;
    s_race_countdown_ticks   = 0;
    s_race_countdown_state   = 0;
    set_countdown_indicator_state(0);

    /* [CONFIRMED @ 0x0042cc30..0x0042cc73 BeginRaceFadeOutTransition; Tier 4
     * port 2026-05-24, REGR-FIX 2026-05-25] Radial-pulse gate. Orig checks:
     *   param_1 == 1 && g_humanPlayerCount == 1 && actor+0x383 == 0 &&
     *   !network && selectedGameType == 0 && !drag
     *
     * actor+0x383 is race_position (0 = 1st place). The orig pulse is the
     * VICTORY burst, fired only when the player finishes 1st in a normal
     * single-player single-race (no split-screen, no network, no drag, no
     * cop chase). Without this check the pulse fired on every single-player
     * race exit (including losing and pause-menu Exit), producing a
     * "star fade" that played simultaneously with the normal black wipe.
     *
     * Port mapping: split_screen_mode==0 covers the (param_1==1 &&
     * g_humanPlayerCount==1) orig gate -- both call sites in
     * td5_game_run_race_frame pass either 0 (pause-exit) or
     * g_td5.split_screen_mode. We additionally require slot-0 race_position
     * == 0 so the pulse only triggers on a 1st-place finish, matching
     * orig's actor+0x383 check. */
    if (param == 0 &&
        !g_td5.network_active &&
        g_td5.game_type == TD5_GAMETYPE_SINGLE_RACE &&
        !g_td5.drag_race_enabled) {
        TD5_Actor *player = td5_game_get_actor(0);
        int rp = player ? (int)player->race_position : -1;
        /* Never fire the victory star during a cinematic (View Replay / attract
         * demo) race — the original suppresses all normal HUD elements there
         * (replay bitmask = 0x80000000 only). This also keeps the ESC-abort path
         * from triggering a star when the played-back car happens to lead. */
        /* BACK TO LOBBY is a quit, not a win — never fire the victory star on
         * it (the star sets s_race_end_radial_pulse_enabled, which SUPPRESSES
         * the normal full-screen fade). Excluding it here makes BACK TO LOBBY
         * play the same plain black wipe across ALL viewports as QUIT TO MENU.
         * [MP back-to-lobby transition 2026-06-13] */
        /* [FIX timeout-DNF 2026-06-23] A P2P checkpoint-timer expiry is a LOSS, not
         * a win, even if the player was still leading on track when time ran out
         * (race_position==0). The original suppresses the victory star here because
         * a timeout leaves race_position != 0 (the 0x0042CC20 gate has no dedicated
         * DNF flag — RE-confirmed). The port keeps the leader at race_position 0 on
         * timeout, so gate explicitly on the DNF marker the timeout path sets
         * (tick_pending_finish_timer: companion_2 = 2; a real finish sets it to 1). */
        int local_dnf = (s_slot_state[0].companion_2 == 2);
        int star_fired = (player && player->race_position == 0 && !local_dnf &&
                          !s_pause_exit_pending && !s_pause_lobby_pending &&
                          !td5_game_is_cinematic_race());
        /* DIAG (race-finish-transition /fix): make the star-gate decision observable. */
        TD5_LOG_I(LOG_TAG,
                  "STAR-GATE: param=%d net=%d gt=%d drag=%d race_position=%d pause_exit=%d dnf=%d -> star=%s",
                  param, (int)g_td5.network_active, (int)g_td5.game_type,
                  (int)g_td5.drag_race_enabled, rp, (int)s_pause_exit_pending,
                  local_dnf, star_fired ? "FIRED" : "skipped");
        if (star_fired) {
            td5_hud_reset_radial_pulse();
            /* [CONFIRMED @ 0x0042cc74] orig sets g_raceEndRadialPulseEnabled=1
             * right after ResetHudRadialPulseOverlay -> suppresses the
             * directional fade for this race (star wipe ONLY). */
            s_race_end_radial_pulse_enabled = 1;
            /* Arm the victory hold so the star animation + finishing number get
             * TD5_VICTORY_HOLD_MS of screen time before the cut to results. */
            s_victory_hold_start = td5_plat_time_ms();
            TD5_LOG_I(LOG_TAG, "Victory hold armed: pos=%d hold=%ums",
                      s_finish_position_display, TD5_VICTORY_HOLD_MS);
        }
    } else {
        TD5_LOG_I(LOG_TAG,
                  "STAR-GATE: outer gate failed (param=%d net=%d gt=%d drag=%d) -> star skipped",
                  param, (int)g_td5.network_active, (int)g_td5.game_type,
                  (int)g_td5.drag_race_enabled);
    }

    switch (param) {
    case 0:  /* single player */
        g_td5.fade_direction = s_fade_direction_alternator;
        s_fade_direction_alternator ^= 1;  /* toggle for next race */
        break;
    case 1:  /* horizontal split */
        g_td5.fade_direction = 0;
        break;
    case 2:  /* vertical split */
        g_td5.fade_direction = 1;
        break;
    default:
        g_td5.fade_direction = 0;
        break;
    }

    TD5_LOG_I(LOG_TAG, "Fade out begin: param=%d direction=%d", param, g_td5.fade_direction);
}

static const char *td5_game_state_name(TD5_GameState state)
{
    switch (state) {
    case TD5_GAMESTATE_INTRO: return "INTRO";
    case TD5_GAMESTATE_MENU: return "MENU";
    case TD5_GAMESTATE_RACE: return "RACE";
    case TD5_GAMESTATE_BENCHMARK: return "BENCHMARK";
    default: return "UNKNOWN";
    }
}

/* ========================================================================
 * IsLocalRaceParticipantSlot (0x42CBE0)
 *
 * [CONFIRMED @ 0x0042cbe0 IsLocalRaceParticipantSlot; L5 promotion sweep
 *  audit 2026-05-18] -- Byte-faithful port. Three-branch dispatch matches
 *  orig exactly:
 *    if dpu[0xc08] != 0      -> dpu[0xbcc + slot*4]   (network participant flag)
 *    elif viewport_mode != 0 -> slot < 2              (split-screen pair)
 *    else                    -> slot == 0             (single-player owns slot 0)
 *  td5_net_is_slot_active() reads the same dpu+0xBCC slot vector as orig.
 * ======================================================================== */

int td5_game_is_local_participant(int slot) {
    if (g_td5.network_active) return td5_net_is_slot_active(slot); /* dpu_exref[0xBCC + slot*4] */
    if (g_td5.split_screen_mode > 0) return (slot < 2);
    return (slot == 0);
}

/* ========================================================================
 * Frame Timing (from RunRaceFrame 0x42B580, timing block)
 *
 * g_frameEndTimestamp = td5_plat_time_ms()
 * frameDeltaMs = end - prev
 * g_instantFPS = 1000.0 / frameDeltaMs
 * g_normalizedFrameDt = frameDeltaSeconds * 30.0f
 * g_simTickBudget = g_normalizedFrameDt (clamped to 4.0 max)
 * g_simTimeAccumulator += g_normalizedFrameDt * 0x10000
 * ======================================================================== */

static uint32_t td5_game_normalized_dt_to_accum(float dt_normalized)
{
    if (dt_normalized <= 0.0f) {
        return 0;
    }
    return (uint32_t)(dt_normalized * (float)TD5_TICK_ACCUMULATOR_ONE);
}

static float td5_game_normalized_dt_to_seconds(float dt_normalized)
{
    return dt_normalized * (1.0f / 30.0f);
}

/* Always-on FPS overlay readouts. Updated by td5_game_update_fps_overlay(),
 * which is called from the main loop EVERY frame (all states) — unlike
 * td5_game_update_frame_timing(), which only runs in the race frame. */
float g_td5_display_fps = 0.0f;
int   g_td5_peak_frame_ms = 0;

void td5_game_update_fps_overlay(void) {
    /* Sampling (EMA fps + worst-frame) runs EVERY frame for accuracy, but the
     * on-screen values are PUBLISHED at 4 Hz (every 250 ms). [S12 follow-up
     * 2026-06-05] Previously g_td5_display_fps was written every frame (digits
     * flickered) and g_td5_peak_frame_ms refreshed at 1 Hz — now both refresh at
     * a uniform, readable 4 Hz. The 1 s diagnostic log keeps its own cadence. */
    static uint32_t s_prev = 0;          /* prev frame timestamp                  */
    static uint32_t s_pub_start = 0;     /* start of the current 250 ms publish window */
    static uint32_t s_pub_max = 0;       /* worst frame-ms within the publish window   */
    static uint32_t s_log_start = 0;     /* start of the 1 s diagnostic-log window      */
    static uint32_t s_log_max = 0;       /* worst frame-ms within the log window         */
    static float    s_disp = 0.0f;       /* EMA-smoothed fps                       */
    uint32_t now = td5_plat_time_ms();
    if (s_prev == 0) { s_prev = now; s_pub_start = now; s_log_start = now; return; }
    uint32_t delta = now - s_prev;
    s_prev = now;
    if (delta < 1)    delta = 1;
    if (delta > 1000) delta = 1000;

    float fps = 1000.0f / (float)delta;
    if (s_disp <= 0.0f) s_disp = fps;
    s_disp += (fps - s_disp) * 0.1f;                 /* EMA for a stable readout */
    if (delta > s_pub_max) s_pub_max = delta;        /* worst frame this 250ms window */
    if (delta > s_log_max) s_log_max = delta;        /* worst frame this 1s window    */

    /* Publish the on-screen FPS/MS 4x per second (every 250 ms). */
    if (now - s_pub_start >= 250) {
        g_td5_display_fps   = s_disp;
        g_td5_peak_frame_ms = (int)s_pub_max;        /* worst frame over the last ~250ms */
        s_pub_max = 0;
        s_pub_start = now;
    }

    /* 1 s diagnostic log to engine.log — independent of the 4 Hz display cadence. */
    if (now - s_log_start >= 1000) {
        TD5_LOG_W("plat", "FPS: %.0f  peak_frame=%ums", (double)s_disp, s_log_max);
        s_log_max = 0;
        s_log_start = now;
    }
}

void td5_game_update_frame_timing(void) {
    uint32_t now = td5_plat_time_ms();
    uint32_t delta_ms = now - g_td5.frame_prev_timestamp;
    float frame_dt_seconds;
    float frame_dt_normalized;

    /* Clamp minimum to avoid division by zero (and max to 100ms = 10fps) */
    if (delta_ms < 1) delta_ms = 1;
    if (delta_ms > 100) delta_ms = 100;

    /* Instant FPS */
    g_td5.instant_fps = 1000.0f / (float)delta_ms;

    /* Normalized frame delta time: 1.0 = one 30 Hz simulation tick. */
    frame_dt_seconds = (float)delta_ms / 1000.0f;
    frame_dt_normalized = frame_dt_seconds * 30.0f;

    /* TraceFastForward is a speed multiplier — 1.0 = real-time (default),
     * 2.0 = 2x, 0.5 = half-speed, N = Nx — implemented by scaling the
     * normalized frame dt that feeds the sim accumulator. <=0 is treated as
     * 1.0 (legacy "off" sentinel). Applied unconditionally so it works for
     * casual play, not just race-trace capture. */
    if (g_td5.ini.trace_fast_forward > 0.0f) {
        frame_dt_normalized *= g_td5.ini.trace_fast_forward;
    }
    g_td5.normalized_frame_dt = frame_dt_normalized;

    /* Per-frame simulation budget in normalized tick units. The spiral cap
     * scales with the multiplier so 4x doesn't get crushed back to 4
     * ticks/frame. */
    g_td5.sim_tick_budget = frame_dt_normalized;
    {
        float budget_cap = TD5_MAX_SIM_BUDGET;
        if (g_td5.ini.trace_fast_forward > 1.0f) {
            budget_cap *= g_td5.ini.trace_fast_forward;
        }
        if (g_td5.sim_tick_budget > budget_cap) {
            g_td5.sim_tick_budget = budget_cap;
        }
    }

    /* Convert normalized frame time to the 16.16 tick accumulator. */
    g_td5.sim_time_accumulator += td5_game_normalized_dt_to_accum(frame_dt_normalized);

    /* Benchmark mode: force constant sim budget for deterministic timing */
    if (g_td5.benchmark_active) {
        g_td5.sim_tick_budget = 3.0f;
    }

    g_td5.frame_end_timestamp = now;
    g_td5.frame_prev_timestamp = now;

    /* [CONFIRMED @ 0x00428D40 RecordBenchmarkFrameRateSample]: orig
     * pushes one delta sample per frame into g_benchmarkSampleBuffer
     * while benchmark mode is active.  Port records the microsecond
     * delta into td5_benchmark.c's ring; td5_benchmark_write_report
     * consumes it when the FSM transitions to GAMESTATE_BENCHMARK. */
    if (g_td5.benchmark_active) {
        td5_benchmark_record_sample(delta_ms * 1000u);
    }
}

float td5_game_get_fps(void) {
    return g_td5.instant_fps;
}

float td5_game_get_frame_dt(void) {
    return g_td5.normalized_frame_dt;
}

/* 1-based finishing place captured at the finish line, for the centered victory
 * digit drawn by td5_hud_render_overlays. 0 = no finish this race yet.
 * (Distinct from td5_game_get_finish_position(slot), which reads the results
 * table's 0-based final_position and is only valid after results are built.) */
int td5_game_get_victory_position(void) {
    /* [Bug A fix 2026-06-21] The centered finishing-place digit
     * (td5_hud_draw_finish_position) is a VICTORY flourish rendered from the PNG
     * NUMBERS atlas. It must appear only on a genuine race finish — NOT during a
     * pause-menu-initiated exit fade (Quit to menu / Back to lobby / Restart),
     * all of which also stamp s_finish_position_display and raise
     * race_end_fade_state. Those paths made the big bitmap digit pop up during
     * the quit transition ("old number of position with the PNG atlas"); the
     * player's place still shows via the top-left TTF position label during the
     * fade. Returning 0 here suppresses ONLY the centered digit's race-end-fade
     * fallback in td5_hud_render_overlays (the per-slot genuine-finish path keys
     * off s_slot_finish_place, which a quit never sets). */
    if (s_pause_exit_pending || s_pause_lobby_pending || s_pause_restart_pending)
        return 0;
    /* [FIX timeout-DNF 2026-06-23] A P2P checkpoint-timer expiry (companion_2==2)
     * is a LOSS, not a finish — suppress the centered finishing-place digit. The
     * original routes a timeout to the FAILED results screen with no position
     * (RE 0x00422480); the port keeps the leader at race_position 0, so key off
     * the DNF marker instead. The per-viewport genuine-finish path
     * (s_slot_finish_place) is gated on the same flag at its capture site, so MP
     * timed-out viewports are covered too. */
    {
        int ls = 0;
        if (g_td5.network_active) {
            ls = td5_net_local_slot();
            if (ls < 0 || ls >= TD5_MAX_RACER_SLOTS) ls = 0;
        }
        if (s_slot_state[ls].companion_2 == 2)
            return 0;
    }
    return s_finish_position_display;
}

/* ========================================================================
 * Viewport Layout (0x42C2B0)
 *
 * 3 modes derived from render_width / render_height:
 *   Mode 0 (single):     1 viewport, full screen
 *   Mode 1 (horiz split): 2 viewports, top/bottom halves
 *   Mode 2 (vert split):  2 viewports, left/right halves
 * ======================================================================== */

/* Resolve the split-screen pane grid for `views` panes. Honours the committed
 * Multiplayer Options layout pick (g_td5.split_grid_cols/rows, resolved in
 * frontend_init_race_schedule from the player count + chosen layout); falls
 * back to the automatic ladder when no grid was committed (AutoRace harness /
 * legacy launch paths leave cols/rows 0). This is the SINGLE source of truth
 * for the grid — the 3D viewport rects (td5_game_init_viewport_layout), the HUD
 * per-pane layout (td5_hud_init_layout), and the divider lines
 * (hud_draw_split_dividers) all call it so they can never disagree on
 * orientation (the 3-player LEFT/RIGHT-vs-UP/DOWN mismatch fixed 2026-06-07). */
void td5_game_resolve_split_grid(int views, int *cols, int *rows) {
    int c = g_td5.split_grid_cols;
    int r = g_td5.split_grid_rows;
    if (views < 1) views = 1;
    if (c < 1 || r < 1 || c * r < views) {
        if (views == 2) {
            if (g_td5.split_screen_mode == 2) { c = 2; r = 1; }  /* left | right */
            else                              { c = 1; r = 2; }  /* top / bottom */
        } else if (views == 3) {
            c = 1; r = 3;                       /* 3 horizontal strips (legacy) */
        } else {
            c = (views <= 4) ? 2 : 3;           /* 4=2x2, 5-6=3x2, 7-9=3x3 */
            r = (views + c - 1) / c;
        }
    }
    if (cols) *cols = c;
    if (rows) *rows = r;
}

/* Pane rect (pixels) for view v of a views-pane split across a w x h target.
 * Row-major over the shared split grid; views<=1 = the full target. Single
 * source for the 3D viewport rects, the HUD pane layout and the divider
 * lines so they cannot disagree (integer pane sizes: the right/bottom
 * remainder pixels are outside every pane, matching the 3D viewports). */
void td5_game_get_pane_rect(int views, int v, int w, int h,
                            int *x, int *y, int *pw, int *ph)
{
    if (views <= 1) { *x = 0; *y = 0; *pw = w; *ph = h; return; }
    int cols, rows;
    td5_game_resolve_split_grid(views, &cols, &rows);
    int cw = w / cols;
    int ch = h / rows;
    *x  = (v % cols) * cw;
    *y  = (v / cols) * ch;
    *pw = cw;
    *ph = ch;
}

void td5_game_init_viewport_layout(void) {
    int w = g_td5.render_width;
    int h = g_td5.render_height;

    /* [PORT ENHANCEMENT] N-way split. Viewport count = number of local human
     * players (each human gets its own pane). split_screen_mode==0 -> single
     * fullscreen view. The pane grid (cols x rows) comes from the committed
     * Multiplayer Options layout pick via td5_game_resolve_split_grid (e.g. 3
     * players default to LEFT/RIGHT = 3x1), falling back to an automatic ladder.
     * The original was hard-capped at 2 viewports (RunRaceFrame 0x42B580) — this
     * deliberately deviates. */
    /* Pane count = local humans + AI spectator panes (dev/profiling). The N
     * spectator panes follow AI-driven slots 1..N (set up in InitRace's
     * actor_slot_map); only the humans read input. num_spectate_screens is 0 in
     * every faithful flow, so this is identical to num_human_players there. */
    int views = g_td5.num_human_players + g_td5.num_spectate_screens;
    int cols, rows;
    if (views < 1) views = 1;
    if (views > TD5_MAX_VIEWPORTS) views = TD5_MAX_VIEWPORTS;
    if (g_td5.split_screen_mode == 0) views = 1;

    g_td5.viewport_count = views;

    /* Reset the cell map to identity (vp -> cell vp) up front; the positioned
     * branch below overwrites it. This keeps the queries well-defined even on
     * the single-view / fallback paths. */
    s_view_cell_count = views;
    for (int vp = 0; vp < TD5_MAX_VIEWPORTS; vp++) s_view_cell[vp] = vp;

    if (views <= 1) {
        s_viewports[0].x = 0; s_viewports[0].y = 0;
        s_viewports[0].w = w; s_viewports[0].h = h;
        TD5_LOG_I(LOG_TAG, "Viewport layout: single %dx%d", w, h);
        return;
    }

    /* Grid resolved by the shared helper so the HUD agrees on cols x rows. */
    td5_game_resolve_split_grid(views, &cols, &rows);

    /* [#9 SPLIT-LAYOUT FIX] Build the viewport -> cell map from the position
     * screen's permutation. For each grid cell in row-major order ask the
     * frontend which actor (if any) the player parked there; every OCCUPIED
     * cell becomes the next viewport's home cell, so panes land in the cells the
     * players chose and the cell(s) nobody chose are left free for the HUD
     * map/standings filler. mp_view_actor_slot() returns -1 for every cell when
     * the feature is inactive (knob off, nothing committed, or not the local MP
     * flow) — in that case the loop below contributes nothing and we keep the
     * identity map seeded above, so AutoRace / the harness / non-positioned MP
     * stay byte-identical (panes fill cells 0..N-1, empty cells are the tail). */
    if (td5_split_layout_fix_on()) {
        int total = cols * rows;
        if (total > TD5_MAX_VIEWPORTS) total = TD5_MAX_VIEWPORTS;
        int assigned = 0;
        for (int cell = 0; cell < total && assigned < views; cell++) {
            int actor = td5_frontend_mp_view_actor_slot(cell);
            if (actor >= 0) s_view_cell[assigned++] = cell;
        }
        /* Only adopt the permutation if it accounts for every viewport; a
         * partial map (e.g. fewer committed cells than panes, or spectator panes
         * with no picker entry) would otherwise leave some panes without a cell.
         * In that case fall back to the identity map (already seeded). */
        if (assigned == views) {
            TD5_LOG_I(LOG_TAG, "Viewport layout: positioned cell map [%d %d %d %d %d %d %d %d %d]",
                      s_view_cell[0], s_view_cell[1], s_view_cell[2], s_view_cell[3],
                      s_view_cell[4], s_view_cell[5], s_view_cell[6], s_view_cell[7],
                      s_view_cell[8]);
        } else {
            for (int vp = 0; vp < TD5_MAX_VIEWPORTS; vp++) s_view_cell[vp] = vp;
        }
    }

    /* Lay out each pane at ITS cell (row-major over cols x rows), not at vp.
     * With the identity map this is the legacy "panes fill cells 0..N-1". */
    for (int vp = 0; vp < views; vp++) {
        td5_game_get_pane_rect(views, s_view_cell[vp], w, h,
                               &s_viewports[vp].x, &s_viewports[vp].y,
                               &s_viewports[vp].w, &s_viewports[vp].h);
    }

    TD5_LOG_I(LOG_TAG, "Viewport layout: mode=%d humans=%d spectate=%d count=%d grid=%dx%d %dx%d",
              g_td5.split_screen_mode, g_td5.num_human_players, g_td5.num_spectate_screens,
              g_td5.viewport_count, cols, rows, w, h);
}

/* [#9] Grid cell (0..cols*rows-1, row-major) that viewport v is laid out in.
 * Single source for the HUD: a cell IS a viewport iff some v maps to it, so the
 * HUD draws the map/standings filler only in cells this never returns. Returns v
 * unchanged for out-of-range v or when the layout fix is off (identity map). */
int td5_game_get_pane_cell(int views, int v) {
    (void)views;
    if (v < 0 || v >= TD5_MAX_VIEWPORTS) return v;
    if (v >= s_view_cell_count) return v;
    return s_view_cell[v];
}

/* [MP audio 2026-06-24] Stereo pan (DirectSound DSBPAN domain: -10000 = full
 * left .. 0 = centre .. +10000 = full right) for viewport `vp`, derived from
 * the screen COLUMN of its pane in the resolved split grid. This lets the audio
 * mixer place each local player's car sound in the speaker matching their
 * on-screen quadrant: in a 4-pane 2x2 game the left-column players (top-left,
 * bottom-left) get their engine on the LEFT and the right-column players on the
 * RIGHT, so every player — not just player 1 — can pick out their own car.
 *
 * col 0 -> -TD5_VIEW_PAN_MAX, col (cols-1) -> +TD5_VIEW_PAN_MAX, interior
 * columns proportional (a 3-wide grid's middle column stays centred). Returns 0
 * for the single view and for purely horizontal splits (cols == 1: top/middle/
 * bottom strips cannot be separated by stereo pan). Honours the position-screen
 * cell permutation via td5_game_get_pane_cell, so it agrees with where the pane
 * actually renders. Same single-source-of-truth contract as get_pane_rect — call
 * only in-race (after td5_game_init_viewport_layout). */
#define TD5_VIEW_PAN_MAX 9000   /* outer columns: strong L/R, not fully hard-panned */
int td5_game_get_view_pan(int vp) {
    int views = g_td5.viewport_count;
    if (views <= 1 || vp < 0) return 0;
    int cols, rows;
    td5_game_resolve_split_grid(views, &cols, &rows);
    if (cols <= 1) return 0;                 /* vertical-only split: no L/R cue */
    int col = td5_game_get_pane_cell(views, vp) % cols;
    /* Integer map: (2*col - (cols-1)) / (cols-1) spans exactly [-1, +1]. */
    return (2 * col - (cols - 1)) * TD5_VIEW_PAN_MAX / (cols - 1);
}

/* [#9] 1 iff grid cell `cell` is occupied by a player viewport (so the HUD must
 * NOT draw an empty-cell map/standings there); 0 iff the cell is free. The HUD
 * should iterate all cols*rows cells and fill only the cells where this is 0,
 * instead of assuming the empties are the contiguous tail (views..cols*rows-1) —
 * which is wrong once the position screen permutes which cell each pane uses. */
int td5_game_split_cell_is_viewport(int views, int cell) {
    int n = (views < s_view_cell_count) ? views : s_view_cell_count;
    if (n > TD5_MAX_VIEWPORTS) n = TD5_MAX_VIEWPORTS;
    for (int v = 0; v < n; v++)
        if (s_view_cell[v] == cell) return 1;
    return 0;
}

/* ========================================================================
 * Intro / Legal Screens
 * ======================================================================== */

void td5_game_play_intro_movie(void) {
    /* Original: PlayIntroMovie (0x43C440)
     * Plays Movie/intro.tgq via EA TGQ engine.
     * Source port: try MP4 first (transcoded from TGQ), then AVI, then WMV.
     * TGQ is not supported by Media Foundation -- it will be rejected early
     * with a log message telling the user to transcode. */
    if (td5_fmv_is_supported()) {
        if (!td5_fmv_play("Movie/intro.mp4") &&
            !td5_fmv_play("Movie/intro.avi") &&
            !td5_fmv_play("Movie/intro.wmv")) {
            /* None of the transcoded formats found. Try the original TGQ
             * path -- td5_fmv_play will log the "transcode to MP4" hint. */
            td5_fmv_play("Movie/intro.tgq");
        }
    }
}

void td5_game_show_legal_screens(void) {
    /* Original: ShowLegalScreens (0x42C8E0)
     * Loads legal1.tga, legal2.tga from LEGALS.ZIP and displays each for ~5s.
     * Source port: delegates to td5_fmv module which loads pre-extracted TGAs. */
    td5_fmv_show_legal_screens();
}

/* ========================================================================
 * Display Loading Screen TGA
 *
 * Selects a random loading image from LOADING.ZIP (load00.tga..load19.tga)
 * and displays it as a static screen while race session init completes.
 *
 * Image index = rand() % 20 (seeded from session seed chain).
 * ======================================================================== */

static void display_loading_screen_tga(void) {
    char png_path[128];
    int index = rand() % 20;
    void *pixels = NULL;
    int img_w = 0, img_h = 0;

    snprintf(png_path, sizeof(png_path), "re/assets/loading/load%02d.png", index);
    TD5_LOG_I(LOG_TAG, "Loading screen: %s", png_path);

    if (!td5_asset_load_png_to_buffer(png_path, TD5_COLORKEY_NONE, &pixels, &img_w, &img_h)) {
        TD5_LOG_W(LOG_TAG, "Loading screen %s not found", png_path);
        return;
    }

    /* Draw fullscreen quad and present */
    {
        int screen_w = 0, screen_h = 0;
        td5_plat_get_window_size(&screen_w, &screen_h);
        float sw = (float)screen_w;
        float sh = (float)screen_h;
        TD5_D3DVertex verts[4];
        uint16_t indices[6] = {0,1,2, 0,2,3};

        verts[0].screen_x = 0.0f; verts[0].screen_y = 0.0f;
        verts[0].depth_z = 0.0f;  verts[0].rhw = 1.0f;
        verts[0].diffuse = 0xFFFFFFFF; verts[0].specular = 0;
        verts[0].tex_u = 0.0f;    verts[0].tex_v = 0.0f;

        verts[1].screen_x = sw;   verts[1].screen_y = 0.0f;
        verts[1].depth_z = 0.0f;  verts[1].rhw = 1.0f;
        verts[1].diffuse = 0xFFFFFFFF; verts[1].specular = 0;
        verts[1].tex_u = 1.0f;    verts[1].tex_v = 0.0f;

        verts[2].screen_x = sw;   verts[2].screen_y = sh;
        verts[2].depth_z = 0.0f;  verts[2].rhw = 1.0f;
        verts[2].diffuse = 0xFFFFFFFF; verts[2].specular = 0;
        verts[2].tex_u = 1.0f;    verts[2].tex_v = 1.0f;

        verts[3].screen_x = 0.0f; verts[3].screen_y = sh;
        verts[3].depth_z = 0.0f;  verts[3].rhw = 1.0f;
        verts[3].diffuse = 0xFFFFFFFF; verts[3].specular = 0;
        verts[3].tex_u = 0.0f;    verts[3].tex_v = 1.0f;

        td5_plat_render_clear(0x00000000);
        td5_plat_render_upload_texture(0, pixels, img_w, img_h, 2);
        td5_plat_render_begin_scene();
        td5_plat_render_set_viewport(0, 0, screen_w, screen_h);
        td5_plat_render_set_preset(TD5_PRESET_OPAQUE_LINEAR);
        td5_plat_render_bind_texture(0);
        td5_plat_render_draw_tris(verts, 4, indices, 6);
        td5_plat_render_end_scene();
        td5_plat_present(0);
        td5_plat_present_texture_page(0, 0);
        free(pixels);
    }
}

/* ========================================================================
 * Utilities
 * ======================================================================== */

/** StoreRoundedVector3Ints (0x42CCD0)
 *  Converts 3 floats to rounded integers via the original __ftol behavior. */
void td5_game_store_rounded_vec3(const float *in, int32_t *out) {
    for (int i = 0; i < 3; i++) {
        out[i] = (int32_t)(in[i] + (in[i] >= 0.0f ? 0.5f : -0.5f));
    }
}

/* ========================================================================
 * Game Logic Helpers (migrated from td5re_stubs.c)
 * ======================================================================== */

int td5_game_get_player_slot(int viewport) {
    /* [PORT: N-way] was capped at viewports 0/1, so views 2..N used slot 0's
     * span as their actor-cull-window centre + camera-target — their own car
     * was culled (invisible) whenever it drifted >±64 spans from slot 0 (e.g.
     * after a recovery). Honour every viewport now. */
    if (viewport < 0 || viewport >= TD5_MAX_VIEWPORTS) return 0;
    return g_actorSlotForView[viewport];
}
int td5_game_is_split_screen(void) {
    return g_split_screen_mode != 0;
}

int td5_game_get_slot_state(int slot) {
    if (slot < 0 || slot >= TD5_MAX_RACER_SLOTS) return 3;  /* disabled */
    return (int)s_slot_state[slot].state;
}
/* [CONFIRMED @ 0x40A4C9, 0x0042B27E, 0x0042BF1A]: original ORs g_inputPlaybackActive
 * (0x466E9C) and g_replayModeFlag (0x4AAF64) at every call site. */
int td5_game_is_replay_active(void) {
    return td5_input_is_playback_active() || s_replay_mode;
}

/* DA-M1 audit 2026-05-22: frontend View-Replay button only flipped
 * td5_input_set_replay_mode + td5_input_set_playback_active (both inside
 * td5_input.c's state). The game-side s_replay_mode static was never set,
 * so td5_game_init_race_session @ line 1902 hit the WriteOpen branch and
 * memset the recording buffer to zero — playback then returned 0 input
 * forever, making the race appear to restart blank. Closes
 * todo-view-replay-restarts-race-2026-05-19. */
void td5_game_set_replay_mode(int v) {
    s_replay_mode = v ? 1 : 0;
}

/* Attract-demo flag (orig g_replayModeFlag). Independent of replay; drives the
 * "DEMO MODE" status text. Sets both the static (synced at race init) and the
 * live global the HUD reads. */
void td5_game_set_demo_mode(int v) {
    s_demo_mode = v ? 1 : 0;
    g_demo_mode = s_demo_mode;
}

/* True when EITHER View Replay (input playback) OR attract demo is active.
 * Benchmark is handled separately by its own flag. */
int td5_game_is_cinematic_race(void) {
    return td5_input_is_playback_active() || s_replay_mode || s_demo_mode;
}
/* [CONFIRMED @ 0x00443240 GetTrafficVehicleVariantType]: two-table lookup;
 * returns 2 if model==0xe(14), 1 otherwise, 0 if gate fails or index==4. */
int td5_game_get_traffic_variant(int traffic_index) {
    if (!g_td5.traffic_enabled) return 0;
    if (traffic_index < 0 || traffic_index >= 6 || traffic_index == 4) return 0;
    int model = td5_asset_resolve_traffic_model_index(g_td5.track_index, 0, traffic_index);
    if (model < 0) return 0;
    return (model == 0xe) ? 2 : 1;
}
/* Slot 1 is the active pursuing cop in wanted mode (slots 2-5 are inactive).
 * [INFERRED from slot-state init @ 0x42ABF8 + spawn layout @ 0x42B1C6] */
/* Mirrors orig g_wantedTargetSlotIndex @ 0x004bf51c — .data-init = 0,
 * zero writers across the binary (6 readers verified Ghidra 2026-05-20).
 * Orig's tracked-vehicle audio fires when slot_iter == 0, so the siren
 * source is the player's actor (slot 0). The port previously returned 1,
 * routing the siren onto an opponent AI. */
/* ========================================================================
 * MP CUP (2026-06-22) — self-contained best-of-X series for the "Cup" game
 * mode. Independent of the SP championship (s_cup_schedules / s_race_within_
 * series) to avoid entangling that state machine. Drives a track list, per-
 * position points, optional teams, and routes to the winners/podium screen.
 * ======================================================================== */
static int s_mpcup_active     = 0;
static int s_mpcup_race_count = 0;
static int s_mpcup_current    = 0;   /* 0-based index of the race in progress */
static int s_mpcup_awarded    = 0;   /* points already tallied for this race  */
static int s_mpcup_team_mode  = 0;
static int s_mpcup_tracks[TD5_CUP_MAX_RACES];
static int s_mpcup_points[TD5_MAX_RACER_SLOTS];
static int s_mpcup_team[TD5_MAX_RACER_SLOTS];
/* [CUP TEAM SELECT 2026-06-25] Per-AI-opponent difficulty (0..2) the host set on
 * CHOOSE YOUR TEAM, snapshot at cup_begin so it stays constant across the series.
 * -1 => inherit the global difficulty (human slots, or AI never overridden). */
static int s_mpcup_ai_difficulty[TD5_MAX_RACER_SLOTS];

/* Points by finishing position (0-based). Matches the original cup ladder
 * {15,12,10,5,4,3} @ 0x00463a18. */
static const int k_mpcup_points[6] = { 15, 12, 10, 5, 4, 3 };

static void mpcup_build_track_list(void) {
    int i, n = g_td5.mp_mode_config.cup_race_count, have_cfg = 0;
    if (n < 1) n = 3;
    if (n > TD5_CUP_MAX_RACES) n = TD5_CUP_MAX_RACES;
    s_mpcup_race_count = n;
    for (i = 0; i < n; i++)
        if (g_td5.mp_mode_config.cup_track_indices[i] > 0) { have_cfg = 1; break; }
    for (i = 0; i < n; i++) {
        /* Host-configured list when present, else a rotation from the picked
         * track. The configured list (now set by the cup track-picker) is
         * honored as-is across the full unlocked range incl. migrated TD6 tracks
         * (0..36); the rotation fallback stays on the native racing range
         * (0..18, skipping the drag strip). */
        int t = have_cfg ? g_td5.mp_mode_config.cup_track_indices[i]
                         : (g_td5.track_index + i);
        if (t < 0) t = 0;
        if (have_cfg) { if (t > 36) t = 0; }   /* explicit pick: keep as chosen */
        else          { t %= 19; }             /* rotation: native range only   */
        s_mpcup_tracks[i] = t;
    }
}

void td5_game_mp_cup_begin(void) {
    int i;
    if (g_td5.mp_mode_config.mode != TD5_MP_MODE_CUP) { s_mpcup_active = 0; return; }
    mpcup_build_track_list();
    s_mpcup_current   = 0;
    s_mpcup_awarded   = 0;
    s_mpcup_team_mode = g_td5.mp_mode_config.cup_team_mode;
    for (i = 0; i < TD5_MAX_RACER_SLOTS; i++) {
        s_mpcup_points[i] = 0;
        /* [CUP TEAM SELECT 2026-06-25] Teams + per-opponent difficulty come from
         * the CHOOSE YOUR TEAM screen via the authoritative per-racer-slot arrays.
         * (The legacy team_of_slot[6] only covered net-player slots, and cup_begin
         * runs before the human/AI split is committed, so it cannot resolve AI
         * slots itself.) */
        s_mpcup_team[i]          = g_td5.mp_mode_config.cup_team_of_slot[i];
        s_mpcup_ai_difficulty[i] = g_td5.mp_mode_config.cup_ai_difficulty[i];
    }
    s_mpcup_active = 1;
    TD5_LOG_I(LOG_TAG, "MP CUP begin: races=%d team_mode=%d track[0]=%d",
              s_mpcup_race_count, s_mpcup_team_mode, s_mpcup_tracks[0]);
}

int  td5_game_mp_cup_active(void)     { return s_mpcup_active; }
int  td5_game_mp_cup_current(void)    { return s_mpcup_current; }
int  td5_game_mp_cup_race_count(void) { return s_mpcup_race_count; }
int  td5_game_mp_cup_team_mode(void)  { return s_mpcup_team_mode; }
int  td5_game_mp_cup_has_next(void) {
    return s_mpcup_active && (s_mpcup_current + 1 < s_mpcup_race_count);
}
int  td5_game_mp_cup_points(int slot) {
    return (slot >= 0 && slot < TD5_MAX_RACER_SLOTS) ? s_mpcup_points[slot] : 0;
}
/* [MP CUP RESULTS 2026-06-25] Points EARNED for the race that just finished (this
 * race only), computed the SAME way td5_game_mp_cup_award() tallies them (finish
 * position into k_mpcup_points, DNF -> last). The cumulative cup total is
 * td5_game_mp_cup_points(); the cup results table's POINTS column shows
 * "<this race> (<running total>)". */
int  td5_game_mp_cup_race_points(int slot) {
    int pos;
    if (!s_mpcup_active) return 0;
    if (slot < 0 || slot >= TD5_MAX_RACER_SLOTS) return 0;
    if (s_slot_state[slot].state == 3) return 0;   /* inactive slot */
    pos = td5_game_get_finish_position(slot);       /* 0-based; -1 if DNF */
    if (pos < 0 || pos >= 6) pos = 5;               /* DNF -> last place */
    return k_mpcup_points[pos];
}
int  td5_game_mp_cup_team(int slot) {
    return (slot >= 0 && slot < TD5_MAX_RACER_SLOTS) ? s_mpcup_team[slot] : 0;
}
/* [CUP TEAM SELECT 2026-06-25] Per-AI-opponent difficulty tier (0..2) the host
 * set on CHOOSE YOUR TEAM, or -1 to inherit the global difficulty. Consumed by
 * the in-race SmartAI per-car skill derivation (td5_ai_smart_race_init). */
int  td5_game_mp_cup_ai_difficulty(int slot) {
    if (!s_mpcup_active) return -1;
    if (slot < 0 || slot >= TD5_MAX_RACER_SLOTS) return -1;
    return s_mpcup_ai_difficulty[slot];
}
int  td5_game_mp_cup_track(void) {
    if (!s_mpcup_active) return -1;
    if (s_mpcup_current < 0 || s_mpcup_current >= s_mpcup_race_count) return -1;
    return s_mpcup_tracks[s_mpcup_current];
}

/* Tally points by finishing position for the race that just completed. */
void td5_game_mp_cup_award(void) {
    int slot;
    if (!s_mpcup_active || s_mpcup_awarded) return;
    for (slot = 0; slot < TD5_MAX_RACER_SLOTS; slot++) {
        int pos;
        if (s_slot_state[slot].state == 3) continue;   /* inactive slot */
        pos = td5_game_get_finish_position(slot);       /* 0-based; -1 if DNF */
        if (pos < 0 || pos >= 6) pos = 5;               /* DNF -> last place */
        s_mpcup_points[slot] += k_mpcup_points[pos];
        /* [MP CUP RESULTS 2026-06-25] Log per-slot award so the cup results
         * POINTS column ("<this race> (<total>)") can be verified from race.log
         * without a screenshot. */
        TD5_LOG_I(LOG_TAG, "MP CUP slot %d pos=%d +%d pts -> total %d",
                  slot, pos, k_mpcup_points[pos], s_mpcup_points[slot]);
    }
    s_mpcup_awarded = 1;
    TD5_LOG_I(LOG_TAG, "MP CUP race %d/%d points tallied",
              s_mpcup_current + 1, s_mpcup_race_count);
}

void td5_game_mp_cup_advance(void) {
    if (!s_mpcup_active) return;
    if (s_mpcup_current + 1 < s_mpcup_race_count) {
        s_mpcup_current++;
        s_mpcup_awarded = 0;
        TD5_LOG_I(LOG_TAG, "MP CUP advance -> race %d/%d track=%d",
                  s_mpcup_current + 1, s_mpcup_race_count, td5_game_mp_cup_track());
    }
}

void td5_game_mp_cup_end(void) { s_mpcup_active = 0; }

/* [MP GAME MODES: TRAFFIC FAIRNESS 2026-06-22] In local split-screen MP the
 * traffic stream is shared across viewports. To keep it fair ("everyone sees the
 * same cars at the same time"), a player's collision must NOT permanently wreck
 * background traffic — a totaled car would become a static obstacle for every
 * viewport even though only one player caused it. With this on, plain traffic
 * takes the hit (free-slide) and then recovers to its deterministic lane; cops
 * and pursued racers are unaffected. Knob TD5RE_MP_TRAFFIC_FAIR=0 disables.
 * Off in single-player (byte-faithful). */
int td5_game_mp_traffic_fair(void) {
    static int knob = -1;
    if (knob < 0) {
        knob = td5_env_flag_on("TD5RE_MP_TRAFFIC_FAIR");   /* default ON */
    }
    return knob && g_td5.split_screen_mode > 0 && g_td5.num_human_players >= 2;
}

/* [TRAFFIC BATTLE 2026-06-28] 1 when the active race is the Traffic Destruction
 * battle mode. The mode is selected on the MP mode-vote screen (split-screen /
 * net) or synthesised for single-player vs AI in td5_game_init_race_session. */
int td5_game_battle_mode_active(void) {
    return g_td5.mp_mode_config.mode == TD5_MP_MODE_TRAFFIC_BATTLE;
}

/* [DRAG RACE 2026-06-30] Active when the MP lobby selected the DRAG RACE mode.
 * Mutually exclusive with battle (distinct mode value). Gates the drag-mode
 * gameplay setup (drag_race_enabled on, no AI, optional oncoming traffic) and the
 * option reads in drag_length_level / td5_game_drag_field_size / trf_force_oncoming. */
int td5_game_drag_mp_active(void) {
    return g_td5.mp_mode_config.mode == TD5_MP_MODE_DRAG_RACE;
}

/* ========================================================================
 * [TRAFFIC BATTLE 2026-06-28] CHECKPOINTS win-condition "chaser"
 *
 * A deadline that creeps forward up the track at a fixed pace. Any racer whose
 * forward progress falls behind it is "caught" — eliminated (marked finished
 * where it stands so it stops scoring and the race-completion latch counts it).
 * This makes the mode a balance of crashing for points vs. keeping moving: stop
 * to farm wrecks too long and the deadline runs you down. Self-contained (does
 * NOT use the SP-only P2P checkpoint timer, which split-screen forces off), so
 * it works identically in solo and split-screen. The winner is still whoever
 * wrecked the most (results sort by WRECKS).
 * ======================================================================== */
static int chase_lead_spans(void) {
    int v = td5_env_int("TD5RE_BATTLE_CHASE_LEAD", 120, 0, 100000);   /* spans the deadline starts behind */
    return v;
}
static int32_t chase_pace_q8(void) {
    int sps = td5_env_int("TD5RE_BATTLE_CHASE_SPS", 12, 1, 200);  /* deadline speed, spans per second */
    return (int32_t)(((int64_t)sps * 256) / 30);   /* 24.8 spans per 30Hz tick */
}
/* Cumulative forward progress (spans) of a racer slot — laps folded in so it is
 * monotonic on both point-to-point and circuit tracks. */
static int32_t battle_racer_progress(int slot, int ring) {
    TD5_Actor *a = td5_game_get_actor(slot);
    if (!a) return 0;
    int lap = td5_game_get_player_lap(slot);
    if (lap < 0) lap = 0;
    int span = a->track_span_normalized;
    if (span < 0) span = 0;
    return (int32_t)((int64_t)lap * ring + span);
}

static void battle_chase_tick(void) {
    if (!s_battle_chase_active) return;
    int ring = td5_track_get_span_count();
    if (ring < 1) ring = 1;
    int base = g_traffic_slot_base;
    if (base < 1) base = 1;
    if (base > TD5_MAX_RACER_SLOTS) base = TD5_MAX_RACER_SLOTS;

    /* Anchor the deadline behind the field on the first tick (spawn spans are
     * now committed). */
    if (s_battle_chase_span == INT32_MIN) {
        int32_t minprog = INT32_MAX;
        for (int i = 0; i < base; i++) {
            if (s_slot_state[i].state == 3) continue;
            int32_t p = battle_racer_progress(i, ring);
            if (p < minprog) minprog = p;
        }
        if (minprog == INT32_MAX) minprog = 0;
        s_battle_chase_span = (int32_t)(((int64_t)(minprog - chase_lead_spans())) << 8);
    }

    s_battle_chase_span += chase_pace_q8();
    int32_t deadline = s_battle_chase_span >> 8;

    for (int i = 0; i < base; i++) {
        if (s_slot_state[i].state == 3) continue;            /* inactive slot   */
        if (s_battle_chase_caught[i]) continue;              /* already caught  */
        if (s_metrics[i].post_finish_metric_base != 0) continue; /* already done */
        if (battle_racer_progress(i, ring) >= deadline) continue;
        /* Caught: eliminate where it stands so the completion latch counts it
         * and it stops being scored (the wreck sweep skips finished racers). */
        s_battle_chase_caught[i] = 1;
        int32_t t = s_metrics[i].cumulative_timer;
        s_metrics[i].post_finish_metric_base = (t > 0) ? t : 1;
        if (s_slot_finish_place[i] == 0) {
            int placed = 0;
            for (int j = 0; j < base; j++)
                if (s_metrics[j].post_finish_metric_base != 0) placed++;
            s_slot_finish_place[i] = placed;   /* includes self -> 1-based */
        }
        TD5_Actor *a = td5_game_get_actor(i);
        if (a) a->finish_time = s_metrics[i].post_finish_metric_base;
        TD5_LOG_I(LOG_TAG, "battle_chase: slot=%d caught by deadline (deadline_span=%d)", i, deadline);
    }
}

/* 1 while the CHECKPOINTS deadline chaser is active (HUD reads this). */
int td5_game_battle_chase_active(void) { return s_battle_chase_active; }

/* Spans a racer is currently AHEAD of the deadline (negative once caught). Large
 * sentinel when the chaser is inactive / not yet anchored. The HUD shows this so
 * each player can see the deadline creeping up. */
int td5_game_battle_chase_gap(int slot) {
    if (!s_battle_chase_active || s_battle_chase_span == INT32_MIN) return 99999;
    if (slot < 0 || slot >= TD5_MAX_RACER_SLOTS) return 99999;
    int ring = td5_track_get_span_count();
    if (ring < 1) ring = 1;
    return battle_racer_progress(slot, ring) - (s_battle_chase_span >> 8);
}

/* [MP GAME MODES: COP CHASE 2026-06-22] Effective cop slot for cop-chase /
 * wanted mode. Single-player wanted (game_type 8) keeps the player (slot 0) as
 * the cop ramming AI suspects; MP cop chase uses the host-configured cop_slot
 * and makes every OTHER racer a suspect. Returns -1 when not a cop chase.
 * (Returning 0 for SP keeps every existing call byte-identical.) */
int td5_game_cop_chase_cop_slot(void) {
    if (!g_td5.wanted_mode_enabled) return -1;
    if (g_td5.mp_mode_config.mode == TD5_MP_MODE_COP_CHASE) {
        /* [AI COP 2026-06-23] When the cop is an AI, it occupies the first
         * NON-human slot so every human stays a suspect driving their own car.
         * (Previously the cop_slot default 0 made player 1 the AI cop — the
         * "selected a viper but plays as a cop" report.) frontend_init_race_
         * schedule forces >=1 AI opponent in this mode so the slot exists. The
         * human-cop path keeps the role-screen-picked slot. */
        if (g_td5.mp_mode_config.cop_is_ai) {
            int s = g_td5.num_human_players;
            if (s < 0) s = 0;
            if (s >= TD5_MAX_RACER_SLOTS) s = TD5_MAX_RACER_SLOTS - 1;
            return s;
        }
        int s = g_td5.mp_mode_config.cop_slot;
        if (s < 0 || s >= TD5_MAX_RACER_SLOTS) s = 0;   /* must be a racer slot */
        return s;
    }
    return 0;
}

/* [MP COP CHASE 2026-06-23] Active racer count for an MP cop chase: the human
 * suspects + one AI-cop slot when the cop is an AI. Returns 0 for everything
 * else (SP wanted mode, non-cop-chase) so callers fall back to the faithful
 * 2-slot wanted field. The faithful "slots 2..5 inactive" wanted-mode rule (one
 * player-cop + one AI suspect) does not fit MP cop chase, which needs N human
 * suspects plus an AI cop — without this the AI cop slot was disabled and the
 * cop role fell back onto player 1. */
int td5_game_mp_cop_chase_field(void) {
    if (!g_td5.wanted_mode_enabled) return 0;
    if (g_td5.mp_mode_config.mode != TD5_MP_MODE_COP_CHASE || g_td5.network_active)
        return 0;
    int f = g_td5.num_human_players + (g_td5.mp_mode_config.cop_is_ai ? 1 : 0);
    if (f < 2) f = 2;
    if (f > TD5_MAX_RACER_SLOTS) f = TD5_MAX_RACER_SLOTS;
    return f;
}

/* [MP COP CHASE multi-cop 2026-06-24] True when `slot` is a COP in the active cop
 * chase. SP wanted + AI cop keep the single-cop slot; MP human cop chase reads
 * cop_slot_mask so 2+ split-screen players can all be cops. Returns 0 outside an
 * active cop chase (wanted mode off) — matches cop_chase_cop_slot()'s -1 gate. */
int td5_game_cop_chase_is_cop(int slot) {
    int primary = td5_game_cop_chase_cop_slot();
    if (primary < 0) return 0;                       /* no active cop chase */
    if (slot < 0 || slot >= TD5_MAX_RACER_SLOTS) return 0;
    /* [INFECT 2026-06-25] A converted (arrested-then-infected) suspect is a cop in
     * EVERY cop-chase variant, regardless of the AI-cop / human-cop branch below. */
    if ((s_infected_mask >> slot) & 1u) return 1;
    if (g_td5.mp_mode_config.mode == TD5_MP_MODE_COP_CHASE &&
        !g_td5.network_active && !g_td5.mp_mode_config.cop_is_ai) {
        /* Human cop chase: any slot whose mask bit is set is a cop. A 0 mask
         * (e.g. role screen skipped) falls back to the single primary slot. */
        int mask = g_td5.mp_mode_config.cop_slot_mask;
        if (mask != 0) return (mask >> slot) & 1;
    }
    return slot == primary;                          /* SP / AI cop / no mask */
}

/* True when `slot` is a racer SUSPECT (a non-cop racer) in a cop chase. */
int td5_game_cop_chase_is_suspect(int slot) {
    if (td5_game_cop_chase_cop_slot() < 0) return 0;  /* no active cop chase */
    if (slot < 0 || slot >= TD5_MAX_RACER_SLOTS) return 0;
    return !td5_game_cop_chase_is_cop(slot);          /* every non-cop racer */
}

/* [MP COP CHASE results 2026-06-25] True when a LOCAL split-screen cop chase is
 * running — the gate for the dedicated COPS/SUSPECTS results layout and the
 * "CHASE RESULTS" title. Net play and SP wanted mode keep the normal table. */
int td5_game_mp_cop_chase_active(void) {
    return g_td5.wanted_mode_enabled
        && g_td5.mp_mode_config.mode == TD5_MP_MODE_COP_CHASE
        && !g_td5.network_active;
}

int td5_game_get_cop_actor_index(void) {
    return g_td5.wanted_mode_enabled ? td5_game_cop_chase_cop_slot() : -1;
}
/* [CONFIRMED]: g_wantedModeEnabled @ 0x4AAF68 set at InitializeRaceSession */
int td5_game_is_wanted_mode(void) { return g_td5.wanted_mode_enabled; }
/* [COP-CHASE SIREN 2026-06-21] Expose pause-menu state to the audio layer so the
 * cop-chase horn-siren refresh can be suspended while paused, letting it re-arm
 * cleanly on resume (see td5_sound_update_vehicle_looping_state). */
int td5_game_is_pause_menu_active(void) { return s_pause_menu_active; }
/* [COP-CHASE 2026-06-21] 1 during the pre-race countdown (camera-transition timer
 * still running, cars held); 0 once the race is running. Lets the wanted-objective
 * banner hold solid through the countdown instead of burning its display timer. */
int td5_game_is_countdown_active(void) { return g_cameraTransitionActive != 0; }
/* [CONFIRMED @ 0x0043D7C0 AdvanceGlobalSkyRotation]: increments
 * g_wantedTargetTrackerActive (0x004BF500) by 0x400 when not paused. */
void td5_game_advance_sky_rotation(void) {
    if (!s_pause_menu_active) {
        s_wanted_target_tracker += 0x400;
    }
}

/* [CONFIRMED @ 0x004BF500 g_wantedTargetTrackerActive read site in
 * RenderTrackedActorMarker 0x0043cde0] — exposes the marker intensity to
 * the render path. Decays at 0x200/sub-tick and clamps to [0, 0x1000]
 * inside tick_wanted_target_tracker (see td5_game.c:484). */
int32_t td5_game_get_wanted_target_tracker(void) {
    return s_wanted_target_tracker;
}

/* [CONFIRMED @ 0x004bf51c g_wantedTargetSlotIndex] — slot index of the
 * tracked actor used by the cop-chase marker render gate. Orig .data-
 * init = 0 (player slot); no binary writers. */
int td5_game_get_wanted_target_slot(void) {
    /* [MP COP CHASE] The "target slot" is the COP — the racer that must NOT be
     * damaged (td5_ai_wanted_cop_hit gate) and who earns the ram/bust points.
     * SP returns 0 (player-cop), identical to before. */
    int cop = td5_game_cop_chase_cop_slot();
    return cop < 0 ? 0 : cop;
}

void *td5_game_heap_alloc(size_t size) {
    return calloc(1, size);
}

/* ========================================================================
 * Split-Screen Steering Balance (0x4036B0)
 *
 * Simple rubber-banding for split-screen mode: the player who is behind
 * gets a steering sensitivity boost, while the leader gets nerfed.
 * Scale is centered at 0x100 (1.0 in fixed-point).
 * ======================================================================== */

/** Per-player steering scale factors (0x100 = neutral) */
int g_steer_scale_p1 = 0x100;
int g_steer_scale_p2 = 0x100;

#define SPLIT_STEER_MAX_ADJUSTMENT  0x40  /* max boost/nerf (25%) */

void td5_game_update_split_screen_balance(void)
{
    int pos1, pos2;
    int delta;
    TD5_Actor *a1, *a2;

    if (!g_split_screen_mode) {
        g_steer_scale_p1 = 0x100;
        g_steer_scale_p2 = 0x100;
        return;
    }

    a1 = td5_game_get_actor(g_actorSlotForView[0]);
    a2 = td5_game_get_actor(g_actorSlotForView[1]);
    if (!a1 || !a2) {
        g_steer_scale_p1 = 0x100;
        g_steer_scale_p2 = 0x100;
        return;
    }

    /* Read normalized span (track position) from actor struct at +0x1E4 */
    pos1 = *(int32_t *)((uint8_t *)a1 + 0x1E4);
    pos2 = *(int32_t *)((uint8_t *)a2 + 0x1E4);

    delta = abs(pos2 - pos1) * 2;
    if (delta > SPLIT_STEER_MAX_ADJUSTMENT)
        delta = SPLIT_STEER_MAX_ADJUSTMENT;

    if (pos1 < pos2) {
        /* Player 1 behind: boost P1, nerf P2 */
        g_steer_scale_p1 = 0x100 + delta;
        g_steer_scale_p2 = 0x100 - delta;
    } else {
        g_steer_scale_p1 = 0x100 - delta;
        g_steer_scale_p2 = 0x100 + delta;
    }
}

/* ============================================================
 * [CITATION-SWEEP 2026-05-21] Phase 1 audit-header refresh
 *
 * The following L3 Ghidra functions are ported (or folded) into
 * this file but were missed by build_confidence_map.py's
 * 2026-05-18 citation scan due to snake_case rename or
 * multi-line comment wraps. Listed here so the next confidence-
 * map run promotes them L3 -> L4 (cited without precision
 * keywords). Per-function audits remain a separate Phase 4 task.
 *
 * Source: re/analysis/l3_triage_2026-05-21.csv +
 *         re/analysis/phase1_manifest_assignment.csv
 *
 *   0x00428D20  InitializeBenchmarkFrameRateCapture  [PORTED 2026-05-24 Tier 5]
 *     Orig (Ghidra-verified 0x00428D20): one-shot HeapAllocTracked(1000000) into
 *     g_benchmarkSampleBuffer + zeroes g_benchmarkSampleCount. Port mirror:
 *     td5_benchmark_init_capture() in td5_benchmark.c; wired from
 *     td5_game_init_race_session under the benchmark_active gate.
 *   0x00428D60  FormatBenchmarkReportText  (density-match, verify in Phase 4)
 *   0x0042D950  ApplyMeshRenderBasisFromWorldPosition  (density-match, verify in Phase 4)
 */

/* ============================================================
 * [PORTED 2026-05-24 Tier 5 — benchmark report parity] (was [ARCH-DIVERGENCE])
 *
 * Three orig functions newly ported into td5_benchmark.c.  Sample
 * capture is now byte-faithful (u32 ring of frame deltas).  Report
 * output remains ARCH-DIVERGENT in *format* (plain text instead of
 * 640x480 24bpp TGA composed by DDraw glyph blits + DXDraw::PrintTGA
 * + DXDecimal -- the source-port surface stack has no equivalent),
 * but the report content (sample count, min/max/avg FPS, per-sample
 * dt) carries the same data orig wrote into the TGA.
 *
 *   0x00428D40  RecordBenchmarkFrameRateSample   -> td5_benchmark_record_sample
 *                                                   (wired from td5_game_update_frame_timing)
 *   0x00428D60  FormatBenchmarkReportText        -> folded into td5_benchmark_write_report
 *                                                   (orig's only caller was the TGA writer)
 *   0x00428D80  WriteBenchmarkResultsTgaReport   -> td5_benchmark_write_report
 *                                                   (wired from RACE->BENCHMARK transition)
 */
