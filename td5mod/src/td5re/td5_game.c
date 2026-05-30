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
#include "td5_fmv.h"
#include "td5_sound.h"
#include "td5_input.h"
#include "td5_ai.h"
#include "td5_asset.h"
#include "td5_physics.h"
#include "td5_render.h"
#include "../../../re/include/td5_actor_struct.h"
#include "td5_camera.h"
#include "td5_frontend.h"
#include "td5_hud.h"
#include "td5re.h"
#include "td5_platform.h"
#include "td5_net.h"
#include "td5_save.h"

#ifdef TD5_PILOT_TRACE_00434350
#include "td5_pilot_trace_00434350.h"
#endif
#include "td5_vfx.h"
#include "td5_trace.h"
#include "td5_trace_whole_state.h"
#include "td5_trace_replay.h"
#include "td5_benchmark.h"

int td5_trace_current_sim_tick(void) {
    return g_td5.simulation_tick_counter;
}

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* ========================================================================
 * Game State Globals (migrated from td5re_stubs.c — owned by this module)
 * ======================================================================== */

int     g_actorSlotForView[2]   = {0};
int     g_actorBaseAddr         = 0;
void   *g_actor_pool            = NULL;
void   *g_actor_base            = NULL;
uint8_t *g_actor_table_base     = NULL;
int     g_actor_slot_map[2]     = {0};
int     g_racer_count           = 0;
int     g_game_type             = 0;
int     g_split_screen_mode     = 0;
int     g_replay_mode           = 0;
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
extern int   g_camWorldPos[2][3];       /* td5_camera.c -- per-viewport camera pos (24.8 fixed) */
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
 *   cells. ONE TODO opened: orig's ResetRaceCameraSelectionState call at
 *   the timer-zero crossing is not yet wired in the port (no current
 *   functional impact because the camera preset is never re-saved during
 *   the countdown, but the missing call is documented for completeness).
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

static ViewportRect s_viewports[2];

/* Replay / benchmark timing */
static uint32_t s_race_end_timer_start;
static int      s_replay_mode;
static int      s_race_countdown_ticks;
static int      s_race_countdown_state;
static int      s_pause_menu_active;
static int      s_pause_menu_cursor;   /* 0=VIEW, 1=MUSIC, 2=SOUND, 3=CONTINUE, 4=EXIT */

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
static int      s_pause_exit_pending;  /* 1 = ESC exit fade in progress, return 2 when fade done */

/* ========================================================================
 * Forward declarations (internal helpers)
 * ======================================================================== */

static int  check_race_completion(uint32_t sim_delta);
static void build_results_table(void);
static void reset_results_table(void);
static void sort_results_by_time_asc(void);
static void sort_results_by_score_desc(void);
static void update_race_order(void);
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
    int c = (int)(uint8_t)s_active_checkpoint.checkpoint_count;
    if (c > 5) c = 5; /* checkpoints[] is sized 5 */
    return c;
}

int td5_game_get_minimap_checkpoint_span(int idx)
{
    if (idx < 0 || idx >= td5_game_get_minimap_checkpoint_count())
        return -1;
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
        if (result != 0) {
            /* Race is over. Determine next state.
             * result=1: normal race completion (fade finished) -> results screen
             * result=2: ESC/pause menu exit -> main menu */
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
                if (result == 2) {
                    /* ESC quit — go to main menu */
                    TD5_LOG_I(LOG_TAG, "Race aborted (ESC) -> main menu");
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

int td5_game_get_player_lap(int slot)
{
    if (slot < 0 || slot >= TD5_MAX_RACER_SLOTS) return 0;
    return (int)s_metrics[slot].checkpoint_index;
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

/* ========================================================================
 * InitializeRaceSession (0x42AA10)
 *
 * 33-step synchronous race bootstrap. The loading screen is displayed
 * at step 1, then all heavy asset loading follows. No progress bar.
 * ======================================================================== */

int td5_game_init_race_session(void) {
    #define CK(n) TD5_LOG_I(LOG_TAG, "CK: %s", n)
    CK("ck0_start");
    TD5_LOG_I(LOG_TAG, "InitializeRaceSession: begin");

    /* ---- Mode overrides from InitializeRaceSession @ 0x42AA10 ----
     *
     * Network session @ 0x42ABD5 [RE basis: research agent pass]:
     *   unconditionally forces drag-race mode with 4-lap circuit,
     *   clears special encounters, rebuilds slot states from dpu+0xBCC.
     * Split-screen 2-player:
     *   clears gSpecialEncounterEnabled + gTrafficActorsEnabled. */
    if (g_td5.network_active) {
        TD5_LOG_I(LOG_TAG, "InitRace: network session — forcing drag race, 4 laps, no encounters");
        g_td5.drag_race_enabled = 1;
        g_td5.special_encounter_enabled = 0;
        g_td5.wanted_mode_enabled = 0;
        g_td5.traffic_enabled = 0;
        g_td5.circuit_lap_count = 4;
    }
    if (g_td5.split_screen_mode > 0) {
        TD5_LOG_I(LOG_TAG, "InitRace: 2-player split-screen — disabling traffic/encounters");
        g_td5.traffic_enabled = 0;
        g_td5.special_encounter_enabled = 0;
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
        /* StateReplay harness needs the same deterministic seed the orig
         * snapshot was captured with (td5_quickrace.py default
         * crt_seed=0x1A2B3C4D). Without this, port's CRT diverges each run
         * and any rand()-fed init field (route_table_selector etc.)
         * randomises into sub_tick=0. */
        uint32_t session_seed =
            (g_td5.ini.race_trace_enabled || td5_trace_replay_active())
                ? (uint32_t)0x1A2B3C4D
                : (uint32_t)GetTickCount();
        srand(session_seed);
        /* Drain 12 entries to advance CRT state (port doesn't have the
         * DAT_004aadbc global table but must step _holdrand the same way) */
        for (int i = 0; i < 12; i++) {
            (void)rand();
        }
        TD5_LOG_I(LOG_TAG,
                  "InitRace step 0/19: CRT reseeded (seed=0x%08X) + 12 seed-table rands drained",
                  session_seed);
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

    /* ---- Step 3: Configure race slot states (player/AI/disabled) ---- */
    for (int i = 0; i < TD5_MAX_RACER_SLOTS; i++) {
        s_slot_state[i].state       = (i == 0) ? 1 : 0;  /* slot 0 = player */
        s_slot_state[i].companion_1 = 0;
        s_slot_state[i].companion_2 = 0;
        s_slot_state[i].reserved    = 0;
    }
    /* If split-screen, slot 1 is also a player */
    if (g_td5.split_screen_mode > 0) {
        s_slot_state[1].state = 1;
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
        int decoration_start = 2;
        for (int i = decoration_start; i < TD5_MAX_RACER_SLOTS; i++) {
            s_slot_state[i].state = 3;
        }
        TD5_LOG_I(LOG_TAG,
                  "Drag race: slot 1 active (slot1.state=%d), slots %d..5 decoration (split=%d)",
                  s_slot_state[1].state, decoration_start, g_td5.split_screen_mode);
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
        for (int i = 2; i < TD5_MAX_RACER_SLOTS; i++) {
            s_slot_state[i].state = 3;  /* disabled — matches original */
        }
        TD5_LOG_I(LOG_TAG,
                  "Cop Chase: slots 2..5 disabled (original racerCount=2 for game_type!=0)");
    }
    /* Player-as-AI autopilot: mirrors original attract-mode at
     * InitializeRaceSession 0x0042ACCF, which writes
     *   slot[0].state = 1 - (g_attractModeDemoActive | g_benchmarkModeActive)
     * Dropping slot 0 to state=0 (AI) routes it through
     * td5_physics_update_ai at td5_physics.c:463 and makes td5_game skip
     * td5_input_update_player_control(0) at td5_game.c:1538/1559. */
    if (g_td5.ini.player_is_ai && s_slot_state[0].state == 1) {
        s_slot_state[0].state = 0;
        TD5_LOG_I(LOG_TAG,
                  "InitRace: player_is_ai=1 -> slot 0 switched to AI "
                  "(mirrors 0x0042ACCF attract-mode write)");
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
    /* Racer count:
     *   Time trial: 1 (or 2 in split-screen)
     *   Cop Chase (wanted) / any non-zero game_type: 2
     *     [CONFIRMED @ InitializeRaceActorRuntime 0x00432E60:
     *      if (g_selectedGameType != 0) g_racerCount = 2]
     *   Normal race: 6 */
    if (g_td5.time_trial_enabled) {
        g_racer_count = (g_td5.split_screen_mode > 0) ? 2 : 1;
    } else if (g_td5.wanted_mode_enabled) {
        g_racer_count = 2;  /* slot 0 = player, slot 1 = cop AI */
    } else {
        g_racer_count = TD5_MAX_RACER_SLOTS;
    }
    /* SoloRace debug override: 1-racer race (player always 1st). */
    if (g_td5.ini.solo_race && g_td5.split_screen_mode == 0) {
        g_racer_count = 1;
        TD5_LOG_I(LOG_TAG, "SoloRace: g_racer_count=1 (opponents disabled)");
    }

    /* ---- Step 4: Load track runtime data ---- */
    /* NOTE: td5_asset_load_level sets g_td5.track_type from LEVELINF.DAT,
     * so g_track_is_circuit / g_track_type_mode must be derived after this call. */
    td5_asset_load_level(g_td5.track_index);
    g_track_is_circuit = (g_td5.track_type == TD5_TRACK_CIRCUIT);
    g_track_type_mode = g_track_is_circuit ? 1 : 0;
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
            TD5_LOG_I(LOG_TAG,
                      "InitRace step 4a: LEVELINF+0x30=0 on track=%d (is_circuit=%d) — clearing traffic_enabled",
                      g_td5.track_index, g_track_is_circuit);
            g_td5.traffic_enabled = 0;
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

    /* ---- Step 5: Load vehicle assets and sound banks for all active slots ---- */
    for (int i = 0; i < TD5_MAX_RACER_SLOTS; i++) {
        if (s_slot_state[i].state != 3) {
            int car_for_slot = (i == 0) ? g_td5.car_index : g_td5.ai_car_indices[i];
            td5_asset_load_vehicle(car_for_slot, i);

            /* Load per-vehicle sound bank (Drive.wav, Rev.wav/Reverb.wav, Horn.wav).
             * Slot 0 is the local player and uses Reverb.wav (is_reverb=1). */
            const char *car_zip = td5_asset_get_car_zip_path(car_for_slot);
            if (car_zip) {
                td5_sound_load_vehicle_bank(car_zip, i, (i == 0) ? 1 : 0);
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
     * Gate mirrors the original 0x0042AA10 forced-off conditions:
     * network / split-screen already cleared g_td5.traffic_enabled above;
     * time-trial and drag-race don't spawn traffic actors.
     * Reverse-direction flag (TRAFFIC.BUS entry flags bit 0) is applied in
     * td5_ai_init_traffic_actors / td5_ai_recycle_traffic_actor via +0x80000
     * heading offset [CONFIRMED @ 0x00435786, 0x00435C00]. */
    if (g_td5.traffic_enabled
        && !g_td5.time_trial_enabled
        && !g_td5.drag_race_enabled
        && !g_td5.network_active
        && g_td5.split_screen_mode == 0) {
        int traffic_loaded = 0;
        for (int ti = 0; ti < 6; ti++) {
            int traffic_slot  = TD5_MAX_RACER_SLOTS + ti;  /* slots 6..11 */
            int traffic_model = td5_asset_resolve_traffic_model_index(
                g_td5.track_index, /*reverse=*/0, ti);
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
                  "InitRace step 5b/19: traffic vehicle assets loaded (%d/6 slots, track_index=%d)",
                  traffic_loaded, g_td5.track_index);
    } else {
        TD5_LOG_I(LOG_TAG,
                  "InitRace step 5b/19: traffic disabled (traffic_enabled=%d time_trial=%d drag=%d net=%d split=%d)",
                  g_td5.traffic_enabled, g_td5.time_trial_enabled,
                  g_td5.drag_race_enabled, g_td5.network_active,
                  g_td5.split_screen_mode);
    }

    /* ---- Step 6: Bind track strip runtime pointers ---- */
    /* (Internal to td5_asset_load_level -- strip data is patched in place) */
    TD5_LOG_I(LOG_TAG, "InitRace step 6/19: track strip runtime pointers bound");

    /* ---- Step 7: Parse MODELS.DAT from level ZIP ---- */
    /* (Loaded as part of td5_asset_load_level) */
    TD5_LOG_I(LOG_TAG, "InitRace step 7/19: MODELS.DAT parsed from level assets");

    /* ---- Step 8: Load track textures ---- */
    td5_asset_load_track_textures(g_td5.track_index);
    /* Must run AFTER textures load so the per-page transparency table is
     * populated. Dims billboard meshes that use a type-3 (additive) page. */
    td5_track_dim_additive_billboard_meshes();
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
    for (int i = 0; i < TD5_MAX_RACER_SLOTS; i++) {
        s_race_order[i] = (uint8_t)i;
    }
    TD5_LOG_I(LOG_TAG, "InitRace step 10/19: race metrics/runtime arrays reset");

    /* ---- Step 11: Allocate actors and init vehicle/AI runtime ---- */
    {
        static uint8_t s_actor_memory[TD5_ACTOR_STRIDE * TD5_MAX_TOTAL_ACTORS];
        int spawn_count = g_td5.time_trial_enabled
                          ? (g_td5.split_screen_mode > 0 ? 2 : 1)
                          : (g_td5.traffic_enabled ? TD5_MAX_TOTAL_ACTORS : TD5_MAX_RACER_SLOTS);
        int racer_count = (spawn_count > TD5_MAX_RACER_SLOTS)
                          ? TD5_MAX_RACER_SLOTS : spawn_count;

        memset(s_actor_memory, 0, sizeof(s_actor_memory));
        g_actorBaseAddr = (int)(uintptr_t)s_actor_memory;
        g_actor_pool = s_actor_memory;
        g_actor_base = s_actor_memory;
        g_actor_table_base = s_actor_memory;
        g_td5.total_actor_count = spawn_count;
        td5_ai_bind_actor_table(s_actor_memory);

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

        td5_ai_init_race_actor_runtime();

        /* ---- Step 11b: Position racer actors on grid ---- */
        /* Grid patterns from InitializeRaceSession (0x42B07B-0x42B225):
         *   Circuit (0x42B110): paired rows 6 spans apart
         *   Non-circuit (0x42B174): staggered 3 spans apart */
        static const int8_t s_circuit_span_offsets[TD5_MAX_RACER_SLOTS] = {
            -6, -6, -12, -12, -18, -18
        };
        /* Original (0x42B174): slot 2 placed first (closest to line),
         * slot 0 (player) placed third. Per-slot offsets: */
        static const int8_t s_staggered_span_offsets[TD5_MAX_RACER_SLOTS] = {
            -9, -6, -3, -12, -15, -18
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
            1, 2, 1, 2, 1, 2
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
        if (g_td5.ini.start_span_offset != 0) {
            TD5_LOG_I(LOG_TAG,
                      "start_span_offset=%d will be applied per-slot (16-bit sign-extended)",
                      g_td5.ini.start_span_offset);
        }

        /* Publish start_span as g_trackStartSpanIndex — consumed by the
         * circuit 4-case sector dispatch in advance_pending_finish_state
         * (verbatim port of CheckRaceCompletionState @ 0x00409E80). This
         * holds the unshifted base — matches original 0x0042b076. */
        g_td5.track_start_span_index = start_span;

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
                 * int16 value even if negative. */
                int raw = start_span + span_offsets[effective_slot] + g_td5.ini.start_span_offset;
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
                if (slot == 0) {
                    span_index = 115;
                    sub_lane = 1;
                } else if (slot == 1) {
                    span_index = 115;
                    sub_lane = 2;
                } else {
                    span_index = 1;
                    sub_lane = slot - 1;
                }
                actor_span = span_index;  /* drag hardcodes span; actor gets that value */
                TD5_LOG_I(LOG_TAG,
                          "Drag spawn override: slot=%d span=%d lane=%d",
                          slot, span_index, sub_lane);
            }

            sp = td5_track_get_span(span_index);
            if (!sp) {
                span_index = 1;
                sp = td5_track_get_span(span_index);
            }
            if (!sp)
                continue;

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

#ifdef TD5_PILOT_TRACE_00434350
            {
                static int s_pilot_00434350_call_idx = 0;
                /* param_flip = 0 mirrors original — every observed call site
                 * passes 0 (see audit pilot_00434350_audit.md). */
                td5_pilot_emit_00434350_row(s_pilot_00434350_call_idx++,
                                            slot,
                                            (int)(int16_t)actor_span,
                                            (int)(int8_t)sub_lane,
                                            0,
                                            actor);
            }
#endif

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

    /* ---- Step 15: Configure force feedback + input mapping ---- */
    td5_input_set_active_players(g_td5.split_screen_mode > 0 ? 2 : 1);
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
    g_actorSlotForView[0] = 0;
    g_actorSlotForView[1] = (g_td5.split_screen_mode > 0 && g_td5.total_actor_count > 1) ? 1 : 0;
    g_actor_slot_map[0] = g_actorSlotForView[0];
    g_actor_slot_map[1] = g_actorSlotForView[1];
    TD5_LOG_I(LOG_TAG, "InitRace step 17/19: render state and viewport layout initialized views=%d",
              g_td5.viewport_count);
    CK("ck17_after_viewport");

    /* ---- Step 18: Upload race texture pages to GPU ---- */
    td5_asset_load_race_texture_pages();
    td5_render_load_environs_textures(td5_asset_level_number(g_td5.track_index));
    TD5_LOG_I(LOG_TAG, "InitRace step 18/19: race texture pages + environs uploaded");

    /* ---- Step 19: Initialize HUD, pause menu overlay ---- */
    #define DBG_WRITE(msg) TD5_LOG_I(LOG_TAG, "Step19: %s", msg)
    DBG_WRITE("19a_before_overlay");
    td5_hud_init_overlay_resources(1, 0);
    DBG_WRITE("19b_before_layout");
    td5_hud_init_layout(g_td5.split_screen_mode);
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
        snprintf(sky_path, sizeof(sky_path),
                 "re/assets/levels/level%03d/FORWSKY.png", level_num);
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
        if (g_td5.checkpoint_timers_enabled &&
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
    g_td5.paused = 1;              /* start paused for countdown */
    /* DAT_00483030 (xz_freeze) has 1 read / 0 writes in the original binary.
     * The port previously set a software flag here to gate XZ integration,
     * but the original leaves position integration active during pause and
     * relies on vel_x/vel_z staying 0 because dynamics is skipped. Dead
     * g_xz_freeze / td5_physics_set_xz_freeze kept around but no longer
     * driven — slated for cleanup. */
    s_pause_menu_active = 0;       /* clear stale pause menu from previous race */
    s_prev_esc_state = 1;          /* suppress false ESC edge on first frame */
    g_td5.sim_tick_budget = 0.0f;
    g_td5.sim_time_accumulator = 0;
    g_td5.simulation_tick_counter = 0;
    g_td5.frame_prev_timestamp = td5_plat_time_ms();
    s_fade_accumulator = 0.0f;
    s_post_finish_cooldown = 0;
    s_race_end_timer_start = 0;

    /* Reset results table */
    reset_results_table();

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

    /* Whole-state snapshot -- INDEPENDENT of the per-module CSV trace.
     * Fires before td5_trace_begin_frame so it works with [Trace] RaceTrace=0.
     * Captures the full 6 x 0x388 actor array + a 128-byte globals blob each
     * POST_PROGRESS tick. Counterpart: tools/frida_whole_state_snapshot.js
     * hooks the same logical instant on the original binary.
     *
     * POST_PROGRESS only (skip COUNTDOWN): paused-physics countdown sub-ticks
     * all carry sim_tick=0 so capturing them just wastes the MaxTicks budget
     * before any actual race tick advances the counter. */
    if ((stage_bit & TD5_TRACE_STG_POST_PROGRESS) &&
        td5_trace_whole_state_is_open() && g_actor_table_base)
    {
        TD5_WholeStateGlobals ws;
        memset(&ws, 0, sizeof(ws));
        ws.game_state              = (int32_t)g_td5.game_state;
        ws.game_paused             = (int32_t)g_td5.paused;
        ws.race_end_fade_state     = (int32_t)g_td5.race_end_fade_state;
        ws.sim_time_accumulator    = g_td5.sim_time_accumulator;
        ws.sim_tick_budget         = g_td5.sim_tick_budget;
        ws.simulation_tick_counter = (int32_t)g_td5.simulation_tick_counter;
        ws.normalized_frame_dt     = g_td5.normalized_frame_dt;
        ws.instant_fps             = g_td5.instant_fps;
        ws.view_count              = (int32_t)g_td5.viewport_count;
        ws.splitscreen_count       = (int32_t)g_td5.split_screen_mode;
        for (int i = 0; i < TD5_MAX_RACER_SLOTS; i++) {
            ws.race_slot_state_table[i] =
                ((uint32_t)s_slot_state[i].state       <<  0) |
                ((uint32_t)s_slot_state[i].companion_1 <<  8) |
                ((uint32_t)s_slot_state[i].companion_2 << 16) |
                ((uint32_t)s_slot_state[i].reserved    << 24);
            ws.race_slot_player_flags[i] =
                (s_slot_state[i].state == 1) ? 1 : 0;
            ws.race_order_array[i] = s_race_order[i];
        }
        td5_trace_whole_state_emit(frame, sim_tick,
                                   (const void *)g_actor_table_base, &ws);
    }

    /* Snapshot-replay harness: dump/inject per-sub-tick state at the SAME
     * logical instant as the Frida hook on UpdateRaceCameraTransitionTimer
     * (which fires for countdown sub-ticks too). Fire on BOTH post_progress
     * (race ticks) and countdown (paused sub-ticks). */
    if ((stage_bit & (TD5_TRACE_STG_POST_PROGRESS | TD5_TRACE_STG_COUNTDOWN)) &&
        td5_trace_replay_active() && g_actor_table_base)
    {
        td5_trace_replay_step();
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
        for (int vp = 0; vp < g_td5.viewport_count && vp < 2; vp++) {
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

int td5_game_run_race_frame(void) {
    int i;
    /* ---- Update frame timing ---- */
    td5_game_update_frame_timing();

    td5_game_trace_stage("frame_begin", 0);

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
        for (pf = 0; pf < TD5_MAX_RACER_SLOTS; pf++) {
            if (s_slot_state[pf].state == 3) continue;  /* disabled */
            advance_pending_finish_state(pf, frame_accum);
        }

        int completion = check_race_completion(frame_accum);
        if (completion) {
            g_td5.race_end_fade_state = 1;

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

            if (s_pause_exit_pending) {
                TD5_LOG_I(LOG_TAG, "Fade complete (ESC exit) -> main menu");
                s_pause_exit_pending = 0;
                return 2;  /* 2 = ESC quit (-> main menu) */
            }
            TD5_LOG_I(LOG_TAG, "Fade complete (race finish) -> results");
            return 1;  /* 1 = normal race finish (-> results screen) */
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
        /* --- Input polling --- */
        td5_input_poll_race_session();

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
        if (!s_pause_menu_active) {
            td5_camera_cache_angles();
        }

        /* --- Pause menu (ESC toggles) --- */
        int esc_now = td5_plat_input_key_pressed(0x01);
        int esc_edge = (esc_now && !s_prev_esc_state);
        int pause_menu_was_active = s_pause_menu_active;
        s_prev_esc_state = esc_now;
        if (esc_edge && !s_pause_menu_active) {
            s_pause_menu_active = 1;
            s_pause_menu_cursor = 0;  /* default to VIEW so LEFT/RIGHT
                                       * immediately moves the view-distance
                                       * slider without forcing the user to
                                       * UP-arrow first. The original defaulted
                                       * to CONTINUE (row 3); that made
                                       * dismiss-with-ENTER one keypress, but
                                       * hid the slider behind 3 UP presses.
                                       * Slider responsiveness wins. */
            TD5_LOG_I(LOG_TAG, "Pause menu opened (cursor=VIEW row 0)");
        }
        if (s_pause_menu_active) {
            /* Process pause menu input ONCE per frame (not per tick) to avoid
             * multiple edge triggers from the sim tick loop. */
            if (!s_pause_input_done) {
                s_pause_input_done = 1;

                static int s_prev_down = 0, s_prev_up = 0;
                static int s_prev_left = 0, s_prev_right = 0;
                static int s_prev_enter = 0;
                int key_down  = td5_plat_input_key_pressed(0xD0);
                int key_up    = td5_plat_input_key_pressed(0xC8);
                int key_left  = td5_plat_input_key_pressed(0xCB);
                int key_right = td5_plat_input_key_pressed(0xCD);
                int key_enter = td5_plat_input_key_pressed(0x1C);

                /* Navigation: 5 selectable items (CD Music / SFX / Audio3 / Continue / Exit).
                 * [CONFIRMED @ 0x0043BF70] RunAudioOptionsOverlay: 5 rows total. */
                if (key_down  && !s_prev_down)  s_pause_menu_cursor = (s_pause_menu_cursor + 1) % 5;
                if (key_up    && !s_prev_up)    s_pause_menu_cursor = (s_pause_menu_cursor + 4) % 5;

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
                                  "PAUSE input: cursor=%d L=%d R=%d (slider gate cursor<3: %s)",
                                  s_pause_menu_cursor, key_left, key_right,
                                  s_pause_menu_cursor < 3 ? "PASS" : "BLOCKED");
                    }
                }
                if (s_pause_menu_cursor < 3) {
                    int delta = key_right ? +1 : (key_left ? -1 : 0);
                    if (delta) {
                        if (s_pause_menu_cursor == 0) {
                            float v = td5_save_get_view_distance() + (float)delta * 0.02f;
                            td5_save_set_view_distance(v);
                            TD5_LOG_I(LOG_TAG, "Pause slider VIEW: %.2f", td5_save_get_view_distance());
                        } else if (s_pause_menu_cursor == 1) {
                            int v = td5_save_get_music_volume() + delta * 2;
                            td5_save_set_music_volume(v);
                            td5_sound_set_music_volume(td5_save_get_music_volume());
                            TD5_LOG_I(LOG_TAG, "Pause slider MUSIC: %d", td5_save_get_music_volume());
                        } else {
                            int v = td5_save_get_sfx_volume() + delta * 2;
                            td5_save_set_sfx_volume(v);
                            td5_sound_set_sfx_volume(td5_save_get_sfx_volume());
                            TD5_LOG_I(LOG_TAG, "Pause slider SOUND: %d", td5_save_get_sfx_volume());
                        }
                    }
                }

                /* Confirm (Enter) */
                if (key_enter && !s_prev_enter) {
                    if (s_pause_menu_cursor == 3) {
                        s_pause_menu_active = 0;
                    } else if (s_pause_menu_cursor == 4) {
                        s_pause_menu_active = 0;
                        s_pause_exit_pending = 1;
                        TD5_LOG_I(LOG_TAG, "Pause menu: Exit selected, starting fade-out");
                        /* Trigger fade-out; resources released when fade completes.
                         * Original (0x43C317): calls BeginRaceFadeOutTransition(0). */
                        td5_game_begin_fade_out(0);
                    }
                }

                s_prev_down = key_down; s_prev_up = key_up;
                s_prev_left = key_left; s_prev_right = key_right;
                s_prev_enter = key_enter;
            }

            /* ESC again = continue */
            if (esc_edge && pause_menu_was_active) {
                s_pause_menu_active = 0;
            }

            /* Update graphical overlay (SELBOX + sliders).
             * [CONFIRMED @ 0x0043C0E5] Row 0=VIEW (DAT_004B135C → 0x00466EA8),
             * Row 1=MUSIC (DAT_004B1360), Row 2=SOUND (DAT_004B1364). */
            float view_frac  = td5_save_get_view_distance();
            float music_frac = (float)td5_save_get_music_volume() / 100.0f;
            float sfx_frac   = (float)td5_save_get_sfx_volume()   / 100.0f;
            td5_hud_update_pause_overlay(s_pause_menu_cursor, view_frac, music_frac, sfx_frac);

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
            td5_track_tick();
            /* Chase camera runs AFTER physics — matches RunRaceFrame
             * (0x0042B580). Countdown still updates the camera so the
             * fly-in/idle-orbit animates while the grid counts down. */
            td5_camera_update_chase_all();
            /* Countdown decrement runs LATE in the sub-tick, matching
             * UpdateRaceCameraTransitionTimer at 0x42BA3A. On the sub-tick
             * where the timer reaches 0, this flips g_td5.paused 1→0; the
             * counter gate below then sees paused=0 and increments. */
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
        if (g_cameraTransitionActive > 0) {
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
        td5_physics_tick();
        td5_game_trace_stage("post_physics", ticks_this_frame);
        td5_game_trace_stage("post_ai", ticks_this_frame);

        /* Wanted mode: decay target-tracker marker per sub-tick
         * (DecayTrackedActorMarkerIntensity @ 0x43D7E0) */
        if (g_td5.wanted_mode_enabled) {
            tick_wanted_target_tracker();
        }

        /* --- Track update (tire marks, wrap normalization) --- */
        td5_track_tick();
        td5_game_trace_stage("post_track", ticks_this_frame);

        /* --- VFX tick (tire tracks, particle lifetimes) --- */
        td5_vfx_tick();

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
        td5_camera_update_chase_all();
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
     * Original (0x0042b709): fraction is NOT recomputed when paused. */
    if (!s_pause_menu_active) {
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

    /* ---- Per-tick fog fade ---- */
    td5_render_per_tick_fog_fade();

    /* ---- Split-screen steering balance ---- */
    td5_game_update_split_screen_balance();

    /* ---- Rendering pipeline ---- */

    /* Begin scene */
    td5_render_begin_scene();

    /* Clear backbuffer once before any viewport renders.
     * Moved out of td5_render_actors_for_view so the split-screen P2 pass
     * does not wipe P1's already-rendered half.
     * [RE: RunRaceFrame 0x42B580 — single BeginScene/EndScene pair, one clear] */
    td5_plat_render_clear(0xFF4080C0u);

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
        td5_camera_update_transition_state(vp, vp);

        /* Configure projection for this viewport */
        td5_render_configure_projection(s_viewports[vp].w, s_viewports[vp].h);

        /* ---- Pass 0: SKY ---- */
        td5_render_set_race_pass(TD5_RACE_PASS_SKY);
        td5_render_set_fog(0);  /* fog off for sky */
        td5_render_advance_sky_rotation();
        td5_render_advance_billboard_anims();
        /* Tier 1 port — advance tracked-actor marker strobe phases
         * (orig AdvanceWorldBillboardAnimations @ 0x0043cdc0 stride
         * 0x22c × 2 entries = first 2 sub-blocks of marker pool). */
        td5_vfx_advance_tracked_marker_phases();

        /* ---- Pass 1: OPAQUE (world + track + actors) ---- */
        td5_render_set_race_pass(TD5_RACE_PASS_OPAQUE);
        td5_render_set_fog(1);  /* fog on for world geometry */

        /* Enable deferred additive capture. Type-3 (streetlight / glow)
         * batches emitted during this pass are copied into a side buffer
         * instead of being drawn immediately, so they can be composited
         * on top of all opaque geometry (including alpha-keyed trees)
         * after the world pass finishes. */
        td5_render_begin_world_pass();

        /* Render race actors for this view */
        td5_render_actors_for_view(vp);

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
                td5_render_debug_lines_flush();
            }
        }

        /* VFX: tire tracks, particles */
        td5_vfx_render_tire_tracks();
        td5_vfx_render_tire_marks();
        td5_vfx_draw_particles(vp);
        td5_render_flush_translucent();
        td5_render_flush_projected_buckets();

        /* Now that all opaque world geometry has depth-written, composite
         * the deferred additive lights on top. Fog stays on — lights
         * follow the same fog the world does. */
        td5_render_flush_deferred_additive();

        /* ---- Pass 3: ALPHA (overlay effects) ---- */
        td5_render_set_race_pass(TD5_RACE_PASS_ALPHA);

        /* ---- Pass 1 again: HUD overlays ---- */
        td5_render_set_race_pass(TD5_RACE_PASS_OPAQUE);
        td5_render_set_fog(0);  /* fog off for HUD */

        /* HUD overlay for this viewport */
        td5_hud_draw_status_text(vp, vp);
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
    td5_hud_render_overlays(g_td5.normalized_frame_dt);

    /* Pause overlay: panel + PAUSETXT atlas glyphs are all pre-built quads */
    if (s_pause_menu_active) {
        td5_hud_draw_pause_overlay();
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

    /* Finishing position: big centered digit during the race-end victory
     * window (port enhancement, user 2026-05-30). Drawn last so it stays
     * readable over the star glow / directional fade. race_position is 0-based
     * (0 = 1st); display 1-based. */
    if (g_td5.race_end_fade_state > 0) {
        TD5_Actor *fp_player = td5_game_get_actor(0);
        if (fp_player) {
            td5_hud_draw_finish_position((int)fp_player->race_position + 1);
        }
    }

    td5_hud_flush_text();

    /* ---- Audio tick ---- */

    /* Feed camera position into the sound system as listener position.
     * g_camWorldPos is in 24.8 fixed-point, which is the same coordinate
     * space td5_sound expects (matching actor world_pos). */
    for (int vp = 0; vp < (g_td5.split_screen_mode ? 2 : 1); vp++) {
        td5_sound_set_listener_pos(vp,
            g_camWorldPos[vp][0],
            g_camWorldPos[vp][1],
            g_camWorldPos[vp][2]);
    }

    /* Feed per-vehicle skid intensity and gear state into the sound system */
    for (i = 0; i < TD5_MAX_RACER_SLOTS; i++) {
        if (s_slot_state[i].state == 3) continue;
        TD5_Actor *actor_snd = td5_game_get_actor(i);
        if (!actor_snd) continue;
        uint8_t *a = (uint8_t *)actor_snd;

        /* Skid intensity: max of front/rear axle slip excess (offset 0x31C, 0x320) */
        int slip_front = *(int32_t *)(a + 0x31C);
        int slip_rear  = *(int32_t *)(a + 0x320);
        int slip_max   = (slip_front > slip_rear) ? slip_front : slip_rear;
        if (slip_max < 0) slip_max = 0;

        /* Feed skid intensity for the viewport that is watching this vehicle */
        for (int vp = 0; vp < (g_td5.split_screen_mode ? 2 : 1); vp++) {
            if (g_actorSlotForView[vp] == i) {
                td5_sound_set_skid_intensity(vp, slip_max);
            }
        }

        /* Gear state (offset 0x224) -- used for horn volume table lookup */
        int gear = *(int32_t *)(a + 0x224);
        td5_sound_set_gear_state(i, gear);
    }

    for (i = 0; i < TD5_MAX_RACER_SLOTS; i++) {
        if (s_slot_state[i].state != 3) {
            td5_sound_update_vehicle_looping_state(i);
        }
    }
    td5_sound_update_audio_mix();
    /* [CONFIRMED @ 0x00440B00]: ambient weather (rain) sound runs each
     * frame after the vehicle audio mix, gated by weather particle density. */
    td5_sound_update_ambient();
    td5_sound_tick();

    /* End scene and present */
    td5_render_end_scene();
    td5_plat_present(1);

    td5_game_trace_stage("frame_end", ticks_this_frame);

    return 0;  /* race continues */
}

static void set_countdown_indicator_state(int value)
{
    int view_count = g_td5.viewport_count;

    if (view_count < 1) {
        view_count = 1;
    }
    if (view_count > 2) {
        view_count = 2;
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
 *    A) [TODO] Step (2)'s ResetRaceCameraSelectionState call is NOT wired
 *       in the port. Orig calls it once at the timer-zero crossing to
 *       reload camera presets for both views. Today the camera preset is
 *       never re-saved during the countdown window, so the reload is a
 *       no-op in practice -- but the call is structurally missing.
 *       See todo_countdown_reset_camera_preset_call_2026-05-18.md.
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

static int check_race_completion(uint32_t sim_delta) {
    int i;

    /* Phase 2: Post-finish cooldown (near-instant per TD5_RACE_END_DWELL) */
    if (s_post_finish_cooldown != 0) {
        s_post_finish_cooldown += sim_delta;
        if (s_post_finish_cooldown > TD5_RACE_END_DWELL) {
            /* Dwell expired: build results and signal completion */
            TD5_LOG_I(LOG_TAG, "Race completion: building results (dwell=%u)", TD5_RACE_END_DWELL);
            s_post_finish_cooldown = 0;
            build_results_table();
            return 1;
        }
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
        /* Normal race: require all active slots to finish */
        for (i = 0; i < TD5_MAX_RACER_SLOTS; i++) {
            if (s_slot_state[i].state == 3) continue;  /* disabled */
            if (s_slot_state[i].companion_1 == 0) {     /* not finished */
                /* Exception: if slot 0 is done, allow spectator mode exit */
                if (i != 0 && s_metrics[0].post_finish_metric_base != 0) {
                    continue;  /* allow unfinished AI if player done */
                }
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
        if ((int32_t)actor_span >= (track_start - 1) &&
            (int32_t)actor_span <= (track_start + 1) &&
            m->checkpoint_bitmask == 0x0F) {
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
        {
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
        if (g_td5.race_rule_variant == 0 && pos < TD5_MAX_RACER_SLOTS) {
            /* Championship: {15, 12, 10, 5, 4, 3} */
            r->secondary_metric += s_championship_points[pos];
        } else if (g_td5.race_rule_variant == 4 && pos < TD5_MAX_RACER_SLOTS) {
            /* Ultimate: {1000, 500, 250, 0, 0, 0} */
            r->secondary_metric += s_ultimate_points[pos];
        }
    }

    /* Sort results by the appropriate metric for the game type */
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
 * Sort results by primary metric ascending (fastest wins)
 * Bubble sort on s_race_order, matching SortRaceResultsByPrimaryMetricAsc
 * (0x40AAD0). Used for game types 2-5 (Era, Challenge, Pitbull, Masters).
 * ======================================================================== */

static void sort_results_by_time_asc(void) {
    int swapped;
    do {
        swapped = 0;
        for (int i = 0; i < TD5_MAX_RACER_SLOTS - 1; i++) {
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

    /* Write final positions */
    for (int i = 0; i < TD5_MAX_RACER_SLOTS; i++) {
        s_results[s_race_order[i]].final_position = (int16_t)i;
    }
}

/* ========================================================================
 * Sort results by secondary metric descending (most points wins)
 * Bubble sort matching SortRaceResultsBySecondaryMetricDesc (0x40AB80).
 * Used for game types 1, 6 (Championship, Ultimate).
 * ======================================================================== */

static void sort_results_by_score_desc(void) {
    int swapped;
    do {
        swapped = 0;
        for (int i = 0; i < TD5_MAX_RACER_SLOTS - 1; i++) {
            int a = s_race_order[i];
            int b = s_race_order[i + 1];
            if (s_results[a].secondary_metric < s_results[b].secondary_metric) {
                s_race_order[i]     = (uint8_t)b;
                s_race_order[i + 1] = (uint8_t)a;
                swapped = 1;
            }
        }
    } while (swapped);

    /* Write final positions */
    for (int i = 0; i < TD5_MAX_RACER_SLOTS; i++) {
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
     * by actor+0x86 (track_span_high_water) DESCENDING, not by +0x82. */
    int swapped;
    do {
        swapped = 0;
        for (int i = 0; i < TD5_MAX_RACER_SLOTS - 1; i++) {
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

            /* Higher span_high = further ahead = better position (lower index) */
            if (span_a < span_b) {
                s_race_order[i]     = (uint8_t)b;
                s_race_order[i + 1] = (uint8_t)a;
                swapped = 1;
            }
        }
    } while (swapped);

    /* Write display positions */
    for (int i = 0; i < TD5_MAX_RACER_SLOTS; i++) {
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

    for (int i = 0; i < TD5_MAX_RACER_SLOTS; i++) {
        TD5_Actor *actor = td5_game_get_actor(i);
        if (!actor) {
            continue;
        }
        actor->prev_race_position = actor->race_position;
        actor->race_position = (uint8_t)s_metrics[i].display_position;
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
        int star_fired = (player && player->race_position == 0 && !s_pause_exit_pending);
        /* DIAG (race-finish-transition /fix): make the star-gate decision observable. */
        TD5_LOG_I(LOG_TAG,
                  "STAR-GATE: param=%d net=%d gt=%d drag=%d race_position=%d pause_exit=%d -> star=%s",
                  param, (int)g_td5.network_active, (int)g_td5.game_type,
                  (int)g_td5.drag_race_enabled, rp, (int)s_pause_exit_pending,
                  star_fired ? "FIRED" : "skipped");
        if (star_fired) {
            td5_hud_reset_radial_pulse();
            /* [CONFIRMED @ 0x0042cc74] orig sets g_raceEndRadialPulseEnabled=1
             * right after ResetHudRadialPulseOverlay -> suppresses the
             * directional fade for this race (star wipe ONLY). */
            s_race_end_radial_pulse_enabled = 1;
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

/* ========================================================================
 * Viewport Layout (0x42C2B0)
 *
 * 3 modes derived from render_width / render_height:
 *   Mode 0 (single):     1 viewport, full screen
 *   Mode 1 (horiz split): 2 viewports, top/bottom halves
 *   Mode 2 (vert split):  2 viewports, left/right halves
 * ======================================================================== */

void td5_game_init_viewport_layout(void) {
    int w = g_td5.render_width;
    int h = g_td5.render_height;

    switch (g_td5.split_screen_mode) {
    case 0: /* Single player -- fullscreen */
        g_td5.viewport_count = 1;
        s_viewports[0].x = 0;
        s_viewports[0].y = 0;
        s_viewports[0].w = w;
        s_viewports[0].h = h;
        break;

    case 1: /* Horizontal split -- top/bottom */
        g_td5.viewport_count = 2;
        s_viewports[0].x = 0;
        s_viewports[0].y = 0;
        s_viewports[0].w = w;
        s_viewports[0].h = h / 2;

        s_viewports[1].x = 0;
        s_viewports[1].y = h / 2;
        s_viewports[1].w = w;
        s_viewports[1].h = h / 2;
        break;

    case 2: /* Vertical split -- left/right */
        g_td5.viewport_count = 2;
        s_viewports[0].x = 0;
        s_viewports[0].y = 0;
        s_viewports[0].w = w / 2;
        s_viewports[0].h = h;

        s_viewports[1].x = w / 2;
        s_viewports[1].y = 0;
        s_viewports[1].w = w / 2;
        s_viewports[1].h = h;
        break;

    default:
        g_td5.viewport_count = 1;
        s_viewports[0].x = 0;
        s_viewports[0].y = 0;
        s_viewports[0].w = w;
        s_viewports[0].h = h;
        break;
    }

    TD5_LOG_I(LOG_TAG, "Viewport layout: mode=%d, count=%d, %dx%d",
              g_td5.split_screen_mode, g_td5.viewport_count, w, h);
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
    if (viewport < 0 || viewport > 1) return 0;
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
int td5_game_get_cop_actor_index(void) { return g_td5.wanted_mode_enabled ? 0 : -1; }
/* [CONFIRMED]: g_wantedModeEnabled @ 0x4AAF68 set at InitializeRaceSession */
int td5_game_is_wanted_mode(void) { return g_td5.wanted_mode_enabled; }
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
    return 0;
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
