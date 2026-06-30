/**
 * td5_ai.c -- AI routing, rubber-banding, traffic, script VM
 *
 * Reimplementation of:
 *   0x434FE0  UpdateActorTrackBehavior
 *   0x4340C0  UpdateActorSteeringBias
 *   0x4370A0  AdvanceActorTrackScript (12-opcode bytecode VM)
 *   0x434900  UpdateActorTrackOffsetBias
 *   0x4337E0  FindActorTrackOffsetPeer
 *   0x434AA0  UpdateActorRouteThresholdState
 *   0x432D60  ComputeAIRubberBandThrottle
 *   0x432E60  InitializeRaceActorRuntime
 *   0x434DA0  UpdateSpecialTrafficEncounter
 *   0x434BA0  UpdateSpecialEncounterControl
 *   0x4353B0  RecycleTrafficActorFromQueue
 *   0x435940  InitializeTrafficActorsFromQueue
 *   0x435E80  UpdateTrafficRoutePlan
 *   0x433680  FindNearestRoutePeer
 *   0x436A70  UpdateRaceActors (master dispatcher)
 */

#include "td5_ai.h"
#include "td5_track.h"
#include "td5_physics.h"
#include "td5_platform.h"
#include "td5_sound.h"
#include "td5_trace.h"
#include "td5_game.h"   /* td5_game_get_wanted_target_slot, td5_game_is_wanted_mode */
#include "td5_save.h"   /* td5_save_get_catchup_assist — CATCHUP level (S06) */
#include "td5_input.h"  /* g_td5_steering_bias_max_swing — CATCHUP steering swing (S06) */
#include "td5re.h"
#include <string.h>
#include <math.h>
#include <limits.h>
#include <stdlib.h>   /* getenv/atoi — TD6 AI steering-damping knob */

#define LOG_TAG "ai"

/* ========================================================================
 * Route State access macros
 *
 * Route state array: base 0x4AFB60, stride 0x47 dwords (0x11C bytes).
 * Each field index is a dword offset within the entry.
 * ======================================================================== */

#define RS_STRIDE_DWORDS  0x47
#define RS_STRIDE_BYTES   0x11C

#define RS_ROUTE_TABLE_PTR        0x00
#define RS_ROUTE_TABLE_SELECTOR   0x03
#define RS_DEFAULT_THROTTLE       0x04
/* RS dword 0x09 (byte 0x24) per Ghidra: DAT_004afb84 (0x004afb84) is the
 * per-slot lateral_bias array. With gActorRouteStateTable base = 0x004afb60,
 * `0x4afb84 - 0x4afb60 = 0x24` bytes = dword index 9.
 * [CONFIRMED via Ghidra symbol audit pass 2026-05-11 cross-checking stride
 * against DAT_004afbb0 (+0x50 = index 20 = RS_ACTIVE_LOWER_BOUND) and
 * gActorTrackSpanProgress (+0x64 = index 25 = RS_TRACK_PROGRESS).]
 *
 * A prior port revision used 0x0B with a comment that computed the offset
 * as 0x2C — that arithmetic is wrong (0x84 - 0x60 = 0x24, not 0x2C). */
#define RS_TRACK_OFFSET_BIAS      0x09
#define RS_LEFT_BOUNDARY_A        0x0E
#define RS_LEFT_BOUNDARY_B        0x0F
#define RS_RIGHT_BOUNDARY_A       0x10
#define RS_RIGHT_BOUNDARY_B       0x11
#define RS_RIGHT_EXTENT_A         0x12
#define RS_RIGHT_EXTENT_B         0x13
#define RS_ACTIVE_LOWER_BOUND     0x14
#define RS_ACTIVE_UPPER_BOUND     0x15
#define RS_LEFT_DEVIATION         0x16
#define RS_RIGHT_DEVIATION        0x17
#define RS_FORWARD_TRACK_COMP     0x18
#define RS_TRACK_PROGRESS         0x19
#define RS_SCRIPT_OFFSET_PARAM    0x1B
#define RS_ENCOUNTER_HANDLE       0x1F
#define RS_RECOVERY_STAGE         0x22
/* RS_DIRECTION_POLARITY_LEGACY = 0x25 (byte 0x94): this offset is NOT a real
 * field — the original game listing has zero references to base+0x94. The
 * macro was port-only and reads/writes here had no effect. Kept as a
 * compile-time alias so any leftover code paths can be audited if found; do
 * not introduce new uses. The canonical field is RS_ROUTE_DIRECTION_POLARITY
 * at dword index 0x3F (byte 0xFC), which corresponds to the original symbol
 * `gActorRouteDirectionPolarity` at 0x004afc5c.
 */
#define RS_DIRECTION_POLARITY_LEGACY 0x25  /* DEPRECATED — do not use in new code */
#define RS_ROUTE_DIRECTION_POLARITY  0x3F  /* gActorRouteDirectionPolarity @ 0x4afc5c */
#define RS_SLOT_INDEX             0x35
#define RS_SCRIPT_BASE_PTR        0x3A
#define RS_SCRIPT_IP              0x3B
#define RS_SCRIPT_SPEED_PARAM     0x3C
#define RS_SCRIPT_FLAGS           0x3D
#define RS_SCRIPT_FIELD_3E        0x3E
#define RS_SCRIPT_FIELD_43        0x43
#define RS_SCRIPT_COUNTDOWN       0x45

/* ========================================================================
 * Actor runtime access macros (stride 0xE2 dwords = 0x388 bytes)
 *
 * Byte offsets from actor base.
 * ======================================================================== */

#define ACTOR_STRIDE              0x388

#define ACTOR_SPAN_RAW            0x080   /* int16: raw span index — JUMPS at junction remap */
#define ACTOR_SPAN_NORMALIZED     0x082   /* int16: wrapped-but-not-remapped span for route-table indexing */
#define ACTOR_SPAN_ACCUM          0x084   /* int16: monotonically-accumulated span; immune to remap */
#define ACTOR_SUB_LANE_INDEX      0x08C   /* byte  */
#define ACTOR_CAR_DEF_PTR         0x1B8   /* void*: car definition pointer (bounding box, mass) */
#define ACTOR_YAW_ACCUM           0x1F4   /* int32 */
#define ACTOR_STEERING_CMD        0x30C   /* int32 */
#define ACTOR_LONGITUDINAL_SPEED  0x314   /* int32 */
#define ACTOR_REAR_AXLE_SLIP      0x320   /* int32: rear_axle_slip_excess */
#define ACTOR_STEERING_RAMP_ACCUM 0x33A   /* int16: steering_ramp_accumulator */
#define ACTOR_LIN_VEL_X           0x1CC   /* int32: world-space velocity X */
#define ACTOR_LIN_VEL_Z           0x1D4   /* int32: world-space velocity Z */
#define ACTOR_WORLD_POS_X         0x1FC   /* int32: 24.8 fixed */
#define ACTOR_WORLD_POS_Z         0x204   /* int32: 24.8 fixed */
#define ACTOR_ENCOUNTER_STEER     0x33E   /* int16 */
#define ACTOR_BRAKE_FLAG          0x36D   /* byte  */
#define ACTOR_THROTTLE_STATE      0x36F   /* byte  */
#define ACTOR_SLOT_INDEX          0x375   /* byte  */
#define ACTOR_VEHICLE_MODE        0x379   /* byte  */
#define ACTOR_TRACK_CONTACT_FLAG  0x37B   /* byte: V2W contact flag (1=wall, 2=boundary) */
#define ACTOR_ENCOUNTER_STATE     0x384   /* int32 */

/* Wheel contact probe positions (4 × Vec3_Fixed @ +0x90, stride 0x0C).
 * Used by UpdateActorTrackBounds [CONFIRMED @ 0x004366E0-0x00436891]. */
#define ACTOR_PROBE_FL_BASE       0x090   /* int32 x,y,z = probe_FL */
#define ACTOR_PROBE_FR_BASE       0x09C   /* int32 x,y,z = probe_FR */
#define ACTOR_PROBE_RL_BASE       0x0A8   /* int32 x,y,z = probe_RL */
#define ACTOR_PROBE_RR_BASE       0x0B4   /* int32 x,y,z = probe_RR */

/* ========================================================================
 * Helper accessors (cast through char* arithmetic)
 * ======================================================================== */

static int32_t  g_route_state_storage[TD5_MAX_TOTAL_ACTORS * RS_STRIDE_DWORDS];
static int32_t *g_route_state_base;     /* 0x4AFB60 */
static char    *g_actor_base;           /* 0x4AB108 */
static const uint8_t *g_route_tables[2];
static size_t         g_route_table_sizes[2];

static inline int32_t *route_state(int slot) {
    return g_route_state_base + slot * RS_STRIDE_DWORDS;
}

/* Public accessor used by td5_physics.c */
int32_t *td5_ai_get_route_state(int slot) {
    if (slot < 0 || slot >= TD5_MAX_TOTAL_ACTORS || !g_route_state_base)
        return NULL;
    return route_state(slot);
}

static inline char *actor_ptr(int slot) {
    return g_actor_base + slot * ACTOR_STRIDE;
}

#define ACTOR_I16(base, off)  (*(int16_t *)((base) + (off)))
#define ACTOR_I32(base, off)  (*(int32_t *)((base) + (off)))
#define ACTOR_U8(base, off)   (*(uint8_t *)((base) + (off)))
#define ACTOR_I8(base, off)   (*(int8_t  *)((base) + (off)))

/* ========================================================================
 * Globals
 * ======================================================================== */

/* Rubber-band parameters (4 globals at 0x473D9C-0x473DA8) */
static int32_t g_rb_behind_scale;       /* 0x473D9C */
static int32_t g_rb_ahead_scale;        /* 0x473DA0 */
static int32_t g_rb_behind_range;       /* 0x473DA4 */
static int32_t g_rb_ahead_range;        /* 0x473DA8 */

/* Default throttle table (0x473D64, 14 int32 entries).
 * [CONFIRMED @ memory_read 0x473D64]: raw bytes are
 *   { 0x100, 0x100, 0x140, 0x118, 0x122, 0x140, 0, 0, 0, 0x2bc, 0, 0, 0, 0 }
 * Index 9 = 0x2bc (700) is NON-ZERO: it is the special-encounter cop slot
 * (slot 9). The cop is hijacked from traffic to racer-AI, and its forward
 * throttle comes ONLY from this seed (the rubber-band recompute @0x00432D60
 * covers slots 0-5 only). The prior port table zeroed index 9, so the cop
 * steered toward the player but never accelerated. [FIX 2026-06-01 cops-traffic] */
int32_t g_default_throttle[TD5_MAX_TOTAL_ACTORS] = {
    /* 0-5: faithful original racer seeds [CONFIRMED @ 0x473D64] */
    0x0100, 0x0100, 0x0140, 0x0118, 0x0122, 0x0140,
    /* 6-8: traffic in the legacy layout (0); racer seeds for big fields */
    0x0140, 0x0140, 0x0140,
    /* 9: special-encounter cop seed (0x2bc) in the legacy 6+6 layout — MUST
     * stay for the faithful cop chase; harmless as a big-field racer seed
     * (rubber-band recompute @0x432D60 overwrites AI throttle each tick) */
    0x2bc,
    /* 10-15: racer seeds for >6-racer split-screen fields */
    0x0140, 0x0140, 0x0140, 0x0140, 0x0140, 0x0140,
    /* 16-21: traffic slots (no AI race throttle) */
    0, 0, 0, 0, 0, 0
};

/* Live throttle table (0x473D2C) -- copied from default each tick,
 * then rubber-band-modified per slot */
int32_t g_live_throttle[TD5_MAX_TOTAL_ACTORS];

/* Per-actor throttle bias output consumed by route threshold */
static int32_t g_actor_route_steer_bias[TD5_MAX_TOTAL_ACTORS];

/* Per-actor forward track component */
static int32_t g_actor_forward_track_component[TD5_MAX_TOTAL_ACTORS];

/* Slot state: 0x00=AI, 0x01=player, 0x02=finished/dead */
static int32_t g_slot_state[TD5_MAX_TOTAL_ACTORS];

/* Total active actor count (6=no traffic, 12=with traffic, 2=time trial) */
static int32_t g_active_actor_count;

/* Traffic recovery stage per slot (0=normal, 1-7=recovering) */
static int32_t g_traffic_recovery_stage[TD5_MAX_TOTAL_ACTORS];

/* Lateral avoidance direction toggle (0x4B08B0): 0=push negative, 1=push positive.
 * Persistent; flips only when peer reaches a lane boundary (sub_lane==0 or ==lane_count). */
static int32_t g_lateral_avoidance_direction;

/* Track script program banks (hardcoded .rdata at 0x473CD4-0x473D18) */

/* Program A @ 0x473CD4: stop, set offset -32, steer-left, stop, terminate */
static const int32_t g_script_program_a[] = {
    8, 2, (int32_t)0xFFFFFFE0, 5, 8, 0
};

/* Program B @ 0x473CEC: stop, set offset +64, steer-right, terminate */
static const int32_t g_script_program_b[] = {
    8, 2, 0x40, 6, 0
};

/* Program C @ 0x473D00: stop, set offset -32, steer-right, stop, terminate */
static const int32_t g_script_program_c[] = {
    8, 2, (int32_t)0xFFFFFFE0, 6, 8, 0
};

/* Program D @ 0x473D18: stop, set offset +64, steer-left, terminate */
static const int32_t g_script_program_d[] = {
    8, 2, 0x40, 5, 0
};

/* Array of 4 program bank pointers for round-robin cycling */
static const int32_t *g_script_banks[4] = {
    g_script_program_a,
    g_script_program_b,
    g_script_program_c,
    g_script_program_d
};

/* Initial recovery script (0x473CC8): stop, auto-select, terminate */
static const int32_t g_script_init_recovery[] = { 8, 9, 0 };

/* Current script bank index per-actor for round-robin */
static int g_script_bank_index[TD5_MAX_TOTAL_ACTORS];
static int32_t g_last_logged_opcode[TD5_MAX_TOTAL_ACTORS];

/* Special encounter globals */
static int32_t g_encounter_tracked_handle = -1;    /* -1 = none */
static int32_t g_encounter_enabled;                 /* master gate */
static int32_t g_encounter_cooldown;                /* counts down from 300 */
static int32_t g_encounter_active[TD5_MAX_TOTAL_ACTORS];
static int32_t g_encounter_control_active_latch;
static int32_t g_encounter_steer_bias_latch;
static int32_t g_encounter_route_table_selector;
static int32_t g_encounter_phase_flag;              /* 0=acquisition, !0=tracking */
static int32_t g_encounter_ref_slot;                /* 0x4B0630 */
/* gSpecialEncounterMinForwardTrackComponentThreshold @ 0x4B05BC — gates the
 * brake band in UpdateSpecialEncounterControl. Defaults to 0 in static memory. */
static int32_t g_special_encounter_min_fwd_track_threshold;

/* ========================================================================
 * Police chase rewrite (2026-06-19) — multi-cop, deterministic, MP-safe.
 *
 * A cop is a TRAFFIC vehicle (any traffic slot) flagged at spawn: every
 * CopRatio-th regular spawn becomes a cop. It cruises as normal traffic
 * (COP_IDLE) until a racer passes it, then chases that ONE racer (no two
 * cops on the same car) at up to CopCatchup% of the chased car's speed with
 * no top-speed cap, following it through branches, until it overtakes it
 * (COP_PULLOVER -> forces the target to a full stop), loses it (traffic
 * despawn distance), or the target breaks down.
 *
 * Every array below is written ONLY inside the sim tick from replicated
 * actor state (positions, spans, speeds, g_slot_state) -> identical on every
 * lockstep peer. The chase/branch decisions consume NO RNG; any cop spawn
 * randomness uses the private seeded LCG trf_dyn_rand(). The cosmetics
 * (glow / siren / smoke) read this state through the public API and run in
 * the per-frame render path, after the sim loop. ====================== */
enum {
    COP_IDLE     = 0,  /* not a cop, or a cop cruising as normal traffic */
    COP_CHASING  = 1,  /* actively pursuing g_cop_target[slot] */
    COP_PULLOVER = 2   /* caught the target; forcing it to a full stop */
};
static uint8_t g_cop_is_cop[TD5_MAX_TOTAL_ACTORS];        /* 1 = this traffic slot is a cop car */
static uint8_t g_cop_phase[TD5_MAX_TOTAL_ACTORS];         /* COP_IDLE / COP_CHASING / COP_PULLOVER */
static int8_t  g_cop_target[TD5_MAX_TOTAL_ACTORS];        /* racer slot being chased, or -1 */
/* [MULTI-COP 2026-06-29] g_actor_pursued_by[] (a single pursuer slot per target)
 * was REMOVED. A car may now be chased by several cops at once, so a one-cop
 * field can't represent it. td5_ai_actor_is_pursued() scans the cops instead. */
static uint8_t g_actor_broken_down[TD5_MAX_TOTAL_ACTORS]; /* 1 = crashed/disabled: parked + smoking */
static int16_t g_actor_broken_ticks[TD5_MAX_TOTAL_ACTORS];/* remaining broken-down ticks */
static int16_t g_cop_glow_phase[TD5_MAX_TOTAL_ACTORS];    /* steady strobe intensity ramp per cop */
static int16_t g_cop_cooldown[TD5_MAX_TOTAL_ACTORS];      /* ticks before a cop may re-acquire after a chase */
static int32_t s_cop_spawn_counter;                       /* deterministic 1:CopRatio cop cadence */
/* [R3-12 COP NO-PROGRESS WATCHDOG 2026-06-19] Per-cop chase progress tracker:
 * the best (smallest) main-ring |gap| this cop has achieved toward its current
 * target, and how many consecutive ticks it has gone WITHOUT improving on that
 * best. A cop that loops a branch corridor keeps moving (own span advances) but
 * never closes on the target, so the overtake / too-far / target-down end
 * conditions never fire and it circles forever. When no-progress exceeds
 * COP_STALL_TICKS the chase is abandoned (-> IDLE), and the cop resumes ordinary
 * traffic driving, whose route plan walks it back onto the main ring. Reset on
 * every (re)acquire. Pure function of replicated span state -> lockstep-safe. */
static int32_t s_cop_best_gap[TD5_MAX_TOTAL_ACTORS];      /* smallest |gap| to target seen this chase */
static int16_t s_cop_stall_ticks[TD5_MAX_TOTAL_ACTORS];   /* ticks since |gap| last improved */
/* [#4 SUSTAINED OVERTAKE 2026-06-20] Consecutive ticks this cop has held a lead
 * (gap <= -COP_OVERTAKE_MARGIN) over its target. The pull-over brake only fires
 * once the cop has STAYED ahead for COP_OVERTAKE_HOLD_TICKS (~1s) — a momentary
 * draw-level on a swerve no longer instantly stops the target, and the cop keeps
 * chasing through the hold instead of giving up the moment it noses ahead. Any
 * tick the target is level/ahead again resets it to 0. */
static int16_t s_cop_overtake_ticks[TD5_MAX_TOTAL_ACTORS];
/* [#6 CHASE SPIN-UP 2026-06-20] Ticks since this cop's chase began. For the
 * first COP_CHASE_SPINUP_TICKS the cop drives as an ordinary AI racer (corner
 * speed-easing + route-line settling intact) BEFORE the 1.5x catch-up throttle
 * is force-pinned. Without it a cop floored from a slow cruise (or a
 * velocity-zeroed oncoming flip) the instant a chase started and overshot the
 * first corner straight into a wall. Reset on acquire / chase-end. */
static int16_t s_cop_chase_ticks[TD5_MAX_TOTAL_ACTORS];
/* [MP COP CHASE RAM 2026-06-23] Per-cop ramming oscillator: once the cop reaches a
 * suspect it can't just sit alongside, so it REVERSES to open a gap then LUNGES
 * forward to crash in — repeating until the suspect is arrested. */
static uint8_t s_cop_ram_phase[TD5_MAX_TOTAL_ACTORS];   /* COP_RAM_APPROACH/BACKUP/LUNGE */
static int16_t s_cop_ram_timer[TD5_MAX_TOTAL_ACTORS];   /* ticks left in the current phase */
/* Stuck detection (e.g. wedged against a wall): count ticks with ~no world-position
 * movement; once stuck, reverse out with a hard wiggling steer to escape. */
static int16_t s_cop_ram_stuck[TD5_MAX_TOTAL_ACTORS];
static int32_t s_cop_ram_lastx[TD5_MAX_TOTAL_ACTORS];
static int32_t s_cop_ram_lastz[TD5_MAX_TOTAL_ACTORS];
/* [#3 NO-INSTANT-RE-CHASE 2026-06-21] After a cop SUCCESSFULLY pulls a car over
 * (a chase that ran to COMPLETE) it leaves THAT car alone for a while: it won't
 * re-acquire the same target until the cooldown lapses, so the same cop doesn't
 * instantly turn around and re-grab you. A DIFFERENT cop can still chase you
 * immediately, and the same cop becomes eligible again after the cooldown — so
 * chases never stop for the whole race (the over-aggressive permanent block did,
 * because slot 9 is the preferred recurring cop). Per-cop: the most-recent
 * busted target + ticks remaining. Replicated (deterministic) -> netplay-safe. */
static int8_t  s_cop_rebust_target[TD5_MAX_TOTAL_ACTORS];
static int16_t s_cop_rebust_ticks [TD5_MAX_TOTAL_ACTORS];

/* Chase tuning (spans / fixed-point speed). All in the same units the
 * surrounding AI uses; gaps are computed on the folded main ring so they are
 * branch-safe. */
/* "Passed by" window: a cop always spawns AHEAD of every racer, so the first
 * time a racer's along-track gap goes positive it has genuinely overtaken the
 * cop. The window is wide so a fast racer that jumps several spans in one 30Hz
 * tick is still caught (the original ==2 single-span test missed fast passes). */
#define COP_TRIGGER_LO       0   /* alongside counts as a pass */
#define COP_TRIGGER_HI      15   /* up to 15 spans ahead still counts as "just passed" */
#define COP_OVERTAKE_MARGIN  1   /* cop ahead of target by >=this many spans = overtaken */
#define COP_STOP_SPEED  0x2000   /* |longitudinal speed| at/under which a target counts as stopped */
#define COP_FULL_THROTTLE 0x0200 /* forward drive command while catching up (gentler accel) */
/* [MP COP CHASE RAM 2026-06-23] Aggressive ram-the-suspect steering for the AI
 * cop. Within COP_RAM_SPAN_RANGE spans of the target the cop steers straight at
 * it (over the racing line); within COP_RAM_LUNGE_SPAN it punches the throttle
 * through any brake to land the hit. COP_RAM_GAIN is the steering gain on the
 * heading error (high = sharp turns; the route brain caps |cmd| at 0x18000). */
#define COP_RAM_SPAN_RANGE 30     /* within this many spans -> ram oscillation zone (fwd/rev);
                                   * beyond it -> navigate-and-catch-up. Wide so a momentum
                                   * overshoot during a fwd->rev flip doesn't escape to "far". */
#define COP_RAM_LUNGE_SPAN 5      /* force forward throttle (close the final gap) */
#define COP_RAM_GAIN       0x08   /* GENTLE proportional bias (a full-lock slam spun the cop) */
#define COP_RAM_MAX        0x6000 /* max ram bias added to the route steer (1/4 of full lock 0x18000) */
#define COP_RAM_DEADBAND   0x40   /* ignore tiny heading errors -> no jitter, leave the route steer */
#define COP_ALIGNED_DEV    0x200  /* |heading error| within this = suspect dead ahead */
#define COP_BEHIND_DEV     0x500  /* |heading error| beyond this (~112deg) = suspect is BEHIND
                                   * (cop overshot / suspect passed) -> brake + turn back */
/* [MP COP CHASE RAM] Ramming oscillator: reach the suspect, REVERSE to open a gap,
 * then LUNGE forward to crash in — repeat until it's arrested. */
/* Ramming cycle (per user spec 2026-06-23): LUNGE at FULL speed into the suspect,
 * let the COLLISION (or a wall) stop the cop, then REVERSE for a run-up and lunge
 * again — repeating constantly until the suspect is arrested. */
enum { COP_RAM_LUNGE = 0, COP_RAM_BACKUP = 1 };
#define COP_RAM_BACKUP_TICKS 26     /* reverse ~0.85s for a run-up */
#define COP_RAM_REVERSE      0xFC00 /* firm reverse throttle (~ -1024) */
#define COP_LUNGE_MIN        16     /* min lunge ticks before a collision triggers the next backup */
#define COP_STOPPED_TICKS    8      /* low-movement ticks during a lunge = collided/wedged -> reverse */
#define COP_STUCK_MOVE       0x1000 /* world-units/tick (24.8) below which the cop counts as "not moving" */
#define COP_SLOW_SPEED       0x4000 /* below this speed the steer may reach full lock (tight turns) */
#define COP_RAM_CLOSE        4      /* gap at/under which the cop BULLDOZES (constant full throttle into
                                     * the suspect) instead of coast/brake -> kills the accel/brake jitter */
#define COP_RETARGET_MARGIN  5      /* switch to a different suspect only if it's this many spans CLOSER
                                     * (dynamic proximity targeting, with hysteresis to avoid flip-flop) */
#define COP_RECHASE_COOLDOWN 150 /* ~5s a cop cruises normally before it may re-acquire after a chase */
/* [R3-12] No-progress watchdog: a chasing cop that fails to shrink its |gap| to
 * the target by >= COP_STALL_IMPROVE spans for COP_STALL_TICKS consecutive ticks
 * (~7s at 30Hz) is looping / stuck (typically a branch corridor) -> end the
 * chase. Generous so a cop merely pacing a same-speed target (gap steady, not
 * growing) is NOT dropped: a real pace holds gap ~constant but the overtake path
 * still closes it over time; only a genuine no-closure loop trips this. */
#define COP_STALL_TICKS    210   /* ~7s of zero net closure -> abandon the chase */
#define COP_STALL_IMPROVE    2   /* spans of |gap| reduction that counts as progress */
/* [#4] Hold time the cop must stay overtaken (gap <= -margin) before the target
 * is forced to brake. 30 ticks = ~1s at 30Hz. Tunable via TD5RE_COP_OVERTAKE_HOLD
 * (ticks); 0 restores the old instant-on-overtake behaviour. */
#define COP_OVERTAKE_HOLD_TICKS 30

/* Reset all police-chase per-slot state (global init + per-race). */
static void cop_state_reset(void) {
    memset(g_cop_is_cop, 0, sizeof(g_cop_is_cop));
    memset(g_cop_phase, 0, sizeof(g_cop_phase));
    memset(g_actor_broken_down, 0, sizeof(g_actor_broken_down));
    memset(g_actor_broken_ticks, 0, sizeof(g_actor_broken_ticks));
    memset(g_cop_glow_phase, 0, sizeof(g_cop_glow_phase));
    memset(g_cop_cooldown, 0, sizeof(g_cop_cooldown));
    memset(s_cop_ram_phase, 0, sizeof(s_cop_ram_phase));
    memset(s_cop_ram_timer, 0, sizeof(s_cop_ram_timer));
    memset(s_cop_ram_stuck, 0, sizeof(s_cop_ram_stuck));
    memset(s_cop_ram_lastx, 0, sizeof(s_cop_ram_lastx));
    memset(s_cop_ram_lastz, 0, sizeof(s_cop_ram_lastz));
    memset(s_cop_stall_ticks, 0, sizeof(s_cop_stall_ticks));
    memset(s_cop_overtake_ticks, 0, sizeof(s_cop_overtake_ticks));
    memset(s_cop_chase_ticks, 0, sizeof(s_cop_chase_ticks));
    memset(s_cop_rebust_ticks, 0, sizeof(s_cop_rebust_ticks));
    memset(s_cop_rebust_target, -1, sizeof(s_cop_rebust_target));
    for (int i = 0; i < TD5_MAX_TOTAL_ACTORS; i++) {
        g_cop_target[i]       = -1;
        s_cop_best_gap[i]     = 0x7FFFFFFF;   /* [R3-12] no progress baseline yet */
    }
    s_cop_spawn_counter = 0;
}

/* End the chase a cop is running: release its target's "pursued-by" lock and
 * return the cop to COP_IDLE — but it STAYS a cop (g_cop_is_cop kept) so it can
 * acquire the next car that passes it. */
static void cop_end_chase(int slot) {
    if (slot < 0 || slot >= TD5_MAX_TOTAL_ACTORS) return;
    /* [MULTI-COP 2026-06-29] No per-target pursuer lock to clear anymore — a car
     * can be chased by several cops at once; td5_ai_actor_is_pursued() scans cops. */
    g_cop_target[slot] = -1;
    g_cop_phase[slot]  = COP_IDLE;
    g_cop_glow_phase[slot] = 0;
    /* [R3-12] Clear the no-progress watchdog so a re-acquire starts fresh. */
    s_cop_best_gap[slot]   = 0x7FFFFFFF;
    s_cop_stall_ticks[slot] = 0;
    /* [#4] Clear the sustained-overtake hold so the next chase starts fresh. */
    s_cop_overtake_ticks[slot] = 0;
    /* [#6] Clear the chase spin-up timer for the next acquire. */
    s_cop_chase_ticks[slot] = 0;
    /* Cruise normally for a while before this cop may re-acquire — stops the
     * "same cop chases me again the instant the last chase ends" loop. */
    g_cop_cooldown[slot] = COP_RECHASE_COOLDOWN;
}

/* [COP CATCH-UP 2026-06-29] How far (spans) a cop trails its target before giving
 * up. Was tied to traffic_dyn_despawn (65) — the cop abandoned the chase the
 * instant the player opened a straight-line lead ("the police can't catch up").
 * A longer leash keeps the cop on the player so it can close the gap when they
 * slow for the next corner / crash / traffic. The no-progress stall watchdog
 * (COP_STALL_TICKS) still ends a chase a cop genuinely can't close, so this never
 * strands a hopeless cop forever. TD5RE_COP_CHASE_GAP overrides (spans). */
static int cop_chase_max_gap(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("TD5RE_COP_CHASE_GAP");
        v = (e && e[0]) ? atoi(e) : 110;
        if (v < 20)  v = 20;
        if (v > 400) v = 400;
    }
    return v;
}

/* [R3-12] A/B knob: end a chase when the cop makes no net progress toward the
 * target for COP_STALL_TICKS (the looping-cop recovery). TD5RE_COP_STALL_END=0
 * restores the prior unbounded chase. Default on. */
static int cop_stall_end_enabled(void) {
    static int s = -1;
    if (s < 0) {
        const char *e = getenv("TD5RE_COP_STALL_END");
        s = (!e || e[0] != '0') ? 1 : 0;
    }
    return s;
}

/* [#4] Ticks the cop must hold its overtake before the target is forced to brake.
 * TD5RE_COP_OVERTAKE_HOLD overrides COP_OVERTAKE_HOLD_TICKS (ticks @30Hz); 0 =
 * instant-on-overtake (the old behaviour). Clamped to a sane range. */
static int cop_overtake_hold_ticks(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("TD5RE_COP_OVERTAKE_HOLD");
        long t = (e && e[0]) ? strtol(e, NULL, 10) : COP_OVERTAKE_HOLD_TICKS;
        if (t < 0)   t = 0;
        if (t > 300) t = 300;
        v = (int)t;
    }
    return v;
}

/* [#6] Chase spin-up window: for this many ticks after a chase begins, a cop
 * drives as a plain AI racer (which eases for corners and settles onto the
 * racing line) BEFORE the 1.5x catch-up throttle is force-pinned. 20 ticks
 * (~0.67s @30Hz) is enough to align + get rolling without letting the player
 * escape. TD5RE_COP_CHASE_SPINUP overrides it; 0 = instant sprint (old, crashy)
 * behaviour. Clamped. */
#define COP_CHASE_SPINUP_TICKS 20
static int cop_chase_spinup_ticks(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("TD5RE_COP_CHASE_SPINUP");
        long t = (e && e[0]) ? strtol(e, NULL, 10) : COP_CHASE_SPINUP_TICKS;
        if (t < 0)   t = 0;
        if (t > 300) t = 300;
        v = (int)t;
    }
    return v;
}

/* [MP COP CHASE RAM] Knob: TD5RE_COP_RAM=0 disables the aggressive ram-steering
 * (cop reverts to the road-safe pursuit). Default ON. */
static int cop_ram_enabled(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("TD5RE_COP_RAM"); v = (e && e[0] == '0') ? 0 : 1; }
    return v;
}

/* [#3] A/B knob: after a successful pull-over a cop leaves THAT car alone for a
 * cooldown instead of instantly re-chasing it. Default ON; TD5RE_COP_NO_RECHASE=0
 * restores the old behaviour (re-acquire any passer once the short generic
 * cooldown lapses). */
static int cop_no_rechase_enabled(void) {
    static int s = -1;
    if (s < 0) { const char *e = getenv("TD5RE_COP_NO_RECHASE");
                 s = (e && e[0] == '0') ? 0 : 1; }
    return s;
}

/* [#3] How long (ticks @30Hz) a cop ignores a car it just busted. Default 1800
 * (~60s): long enough that the cop visibly "moved on", short enough that chases
 * resume. TD5RE_COP_REBUST_COOLDOWN overrides; clamped. Other cops are unaffected
 * (they can chase the same car immediately). */
#define COP_REBUST_COOLDOWN 1800
static int cop_rebust_cooldown(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("TD5RE_COP_REBUST_COOLDOWN");
        long t = (e && e[0]) ? strtol(e, NULL, 10) : COP_REBUST_COOLDOWN;
        if (t < 0)     t = 0;
        if (t > 32000) t = 32000;
        v = (int)t;
    }
    return v;
}

/* Fully strip a slot's cop identity (called when the traffic slot is recycled
 * / despawned so a future ordinary spawn doesn't inherit cop-ness). */
static void cop_release(int slot) {
    if (slot < 0 || slot >= TD5_MAX_TOTAL_ACTORS) return;
    cop_end_chase(slot);
    g_cop_is_cop[slot] = 0;
    s_cop_rebust_target[slot] = -1;   /* [#3] a recycled cop is a fresh car */
    s_cop_rebust_ticks[slot]  = 0;
}

/* Age the broken-down state once per sim tick (deterministic): count each
 * timer down and clear the flag at zero. The actual PARKING of a broken
 * traffic/cop slot happens in ai_update_single_traffic (so the per-slot drive
 * doesn't overwrite it); racer slots keep control (the flag there only ends a
 * chase + emits smoke). Called every tick from td5_ai_update_race_actors,
 * independent of the POLICE gate so ordinary broken traffic still smokes. */
static void cop_tick_broken_down(void) {
    for (int s = 0; s < TD5_MAX_TOTAL_ACTORS; s++) {
        if (!g_actor_broken_down[s]) continue;
        /* [#5 2026-06-20] ticks < 0 = a PERMANENT wreck: it never self-recovers;
         * it stays parked + smoking until td5_ai_recycle_traffic_actor reclaims
         * the slot by distance. Only the (legacy A/B) timed mode counts down. */
        if (g_actor_broken_ticks[s] < 0) continue;
        if (g_actor_broken_ticks[s] > 0) g_actor_broken_ticks[s]--;
        if (g_actor_broken_ticks[s] <= 0)
            g_actor_broken_down[s] = 0;
    }
}

/* Per-car torque triplets table (9 cars x 3 dwords at 0x466F90) */
static int32_t g_car_torque_triplets[9 * 3];

/* AI physics template (128 bytes at 0x473DB0 in TD5_d3d.exe) — verbatim
 * copy of the original binary's global AI tuning data. Used by the
 * UpdateAIVehicleDynamics bicycle solve for all AI slots; the port
 * previously pointed AI slots' tuning_data_ptr at per-slot carparam.dat,
 * which gave DIFFERENT Wf/Wr/I values and flipped the sign of
 * D = (Wf*Wr - I) >> 10 in the bicycle solve. That flipped the sign of
 * front_lat at v≈0 with saturated steer, which is what drove the
 * positive-feedback yaw spin symptom. */
static uint8_t g_ai_physics_template[128] = {
    0xA0, 0x00, 0xC0, 0x00, 0xD8, 0x00, 0xE0, 0x00,  /* +0x00 torque curve */
    0xE4, 0x00, 0xE8, 0x00, 0xEC, 0x00, 0xF0, 0x00,  /* +0x08 */
    0xF4, 0x00, 0xF8, 0x00, 0xFC, 0x00, 0x00, 0x01,  /* +0x10 */
    0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01,  /* +0x18 */
    0x20, 0xBF, 0x02, 0x00, 0xC0, 0x5D, 0x00, 0x00,  /* +0x20 I=180000 half_wb=24000 */
    0x90, 0x01, 0x90, 0x01, 0xAC, 0x0D, 0xF0, 0x0A,  /* +0x28 Wf=400 Wr=400 grip=3500 */
    0x00, 0x00, 0xC4, 0x09, 0x3A, 0x07, 0x5A, 0x05,  /* +0x30 gear ratios */
    0xF2, 0x03, 0xEE, 0x02, 0x26, 0x02, 0x0F, 0x27,  /* +0x38 */
    0x00, 0x00, 0x64, 0x19, 0x64, 0x19, 0x64, 0x19,  /* +0x40 */
    0x64, 0x19, 0x64, 0x19, 0x0F, 0x27, 0x0F, 0x27,  /* +0x48 */
    0x00, 0x00, 0x00, 0x00, 0xCC, 0x10, 0xCC, 0x10,  /* +0x50 */
    0xCC, 0x10, 0xCC, 0x10, 0xCC, 0x10, 0x32, 0x00,  /* +0x58 */
    0x28, 0x00, 0x1E, 0x00, 0x00, 0x20, 0x18, 0x00,  /* +0x60 */
    0x64, 0x00, 0x2C, 0x01, 0xB8, 0x0B, 0x58, 0x02,  /* +0x68 steer=0x100 brake=0x2C1? speedlim=0x0BB8 */
    0x58, 0x02, 0xE8, 0x1C, 0x2A, 0x04, 0x03, 0x00,  /* +0x70 */
    0x50, 0x00, 0xA0, 0x00, 0xA0, 0x00, 0x00, 0x00,  /* +0x78 */
};

/* Difficulty tier (0/1/2) mirrors gRaceDifficultyTier @ 0x00463210.
 * Written by ConfigureGameTypeFlags @ 0x00410CA0 (port: td5_frontend.c:2675)
 * from game_type: 1/2→0, 3/4/5→1, 6/7→2.
 * Pre-2026-05-14: this file-static was *never written* in the port — every
 * race used tier=0 regardless of game type, which produced systematic AI
 * under-performance on tier-1/2 races (pool11 0x00432D60 audit blocker #1).
 * The fix routes through g_td5.difficulty_tier instead — keep this static
 * removed and rely on the global TD5 state.
 *
 * static int32_t g_race_difficulty_tier;  -- REMOVED 2026-05-14 (precise-00432E60)
 */

/* Traffic queue pointer and base */
static const uint8_t *g_traffic_queue_base;
static const uint8_t *g_traffic_queue_ptr;

/* Frame counter for misc timing */
static uint32_t g_ai_frame_counter;

/* Wanted-mode damage state table (mirrors gWantedDamageStateTable @ 0x4BEAD4).
 * int16[6], one per racer slot. Initialized to 0x1000 per slot.
 * Decremented on player<->cop collision by 0x200 (impact<=20000) or 0x400 (impact>20000)
 * [CONFIRMED @ AwardWantedDamageScore 0x43D690]. When slot reaches 0, cop is arrested
 * and its AI is frozen [CONFIRMED @ UpdateRaceActors 0x436E1D gate]. */
int16_t g_wanted_damage_state[TD5_MAX_RACER_SLOTS];

/* [COP CHASE HUD 2026-06-27] Per-suspect "last material collision" sim-tick.
 * Written by td5_ai_wanted_cop_hit on every qualifying cop<->suspect crash;
 * read by td5_hud_update_wanted_damage_indicator so the bust ARROW only shows
 * over a suspect for a short window right after you crash into it (user: the
 * arrow used to show over EVERY suspect at once). Sentinel TD5_AI_NO_HIT_TICK
 * means "never hit this race". */
#define TD5_AI_NO_HIT_TICK (-1000000)
int32_t g_wanted_hit_tick[TD5_MAX_RACER_SLOTS] = {
    TD5_AI_NO_HIT_TICK, TD5_AI_NO_HIT_TICK, TD5_AI_NO_HIT_TICK,
    TD5_AI_NO_HIT_TICK, TD5_AI_NO_HIT_TICK, TD5_AI_NO_HIT_TICK,
};
/* [COP CHASE SIREN GATE 2026-06-27] Per-cop "rammed a suspect with the siren
 * OFF" sim-tick. Set by td5_ai_wanted_cop_hit when a cop crashes a suspect
 * without its siren on (no arrest credited); read by td5_hud_draw_status_text
 * to flash a "TURN ON YOUR SIREN" reminder on that cop's pane. */
int32_t g_cop_siren_warn_tick[TD5_MAX_RACER_SLOTS] = {
    TD5_AI_NO_HIT_TICK, TD5_AI_NO_HIT_TICK, TD5_AI_NO_HIT_TICK,
    TD5_AI_NO_HIT_TICK, TD5_AI_NO_HIT_TICK, TD5_AI_NO_HIT_TICK,
};

/* [R3-10] Anti-pileup persistent escape-lane state. Declared here (not in the
 * smart-traffic block below) because td5_ai_smart_traffic_lane — defined earlier
 * in the file — reads it to bias the jam-escape car toward its cleared lane.
 * Written by traffic_collision_escape; reset in td5_traffic_smart_reset. */
static int8_t   s_traffic_escape_lane[TD5_MAX_TOTAL_ACTORS];    /* cleared-lane side (-1/0/+1)     */
static int16_t  s_traffic_escape_lane_ttl[TD5_MAX_TOTAL_ACTORS];/* ticks the lane bias persists    */

/* ========================================================================
 * Forward declarations (internal helpers)
 * ======================================================================== */

static void ai_update_single_racer(int slot);
static void ai_update_single_traffic(int slot);
/* [R3-10] used by traffic_escape_pick_side, which is defined before it. */
static int traffic_lane_is_clear(int self_slot, int self_span,
                                 int target_lane, int polarity);

uint8_t *td5_ai_get_physics_template(void) {
    return g_ai_physics_template;
}

static int32_t ai_cos_fixed12(int32_t angle);
static int32_t ai_sin_fixed12(int32_t angle);
static void td5_ai_refresh_route_state_slot(int slot);
static int32_t ai_angle_from_vector(int32_t dx, int32_t dz);

/* Pre-loop helpers for the per-slot bias-clamp/boundary chain ported from
 * UpdateRaceActors @ 0x00436A70. Gated behind g_td5.ini.experimental_bias_clamp. */
static void td5_ai_update_actor_track_bounds(int slot);
static int  td5_ai_classify_track_offset_clamp_v2(int slot, int track_offset_bias);
static int  td5_ai_remap_for_classify(int span_normalized);

/* ---- Smart opponent AI overhaul (non-faithful; gated by [GameOptions]SmartAI).
 * Defined in the "SMART OPPONENT AI" section below; forward-declared here
 * because td5_ai_compute_rubber_band / td5_ai_init_race_actor_runtime sit
 * earlier in the file than the section. ------------------------------------ */
static int   td5_ai_smart_active(void);          /* race-level master gate */
static void  td5_ai_smart_race_init(void);       /* per-race skill + seeds */
static float td5_ai_smart_skill(int slot);       /* continuous 0..1 competence */
static void  td5_ai_smart_lane_bias(int slot);   /* writes RS_TRACK_OFFSET_BIAS */
static void  td5_ai_smart_speed(int slot);       /* post-modulates throttle/brake */
static void  td5_ai_smart_branch(int slot);      /* strategic junction selector */
static int   td5_ai_smart_traffic_lane(int slot, int target_span,
                                        int lane_count, int base_sub_lane,
                                        int polarity);
static int   td5_ai_smart_leash_modifier(int slot, int32_t delta,
                                          int32_t *out_modifier);

static int32_t ai_cos_fixed12(int32_t angle) {
    /* Use standard math — the quadratic approximation was catastrophically
     * wrong at quadrant boundaries (sin(0) returned -4096 instead of 0). */
    double rad = (double)(angle & 0xFFF) * (2.0 * 3.14159265358979323846 / 4096.0);
    return (int32_t)(cos(rad) * 4096.0);
}

static int32_t ai_sin_fixed12(int32_t angle) {
    double rad = (double)(angle & 0xFFF) * (2.0 * 3.14159265358979323846 / 4096.0);
    return (int32_t)(sin(rad) * 4096.0);
}

/* ========================================================================
 * AngleFromVector12 -- atan2-based angle computation (0x40A720 equivalent)
 *
 * Returns angle in 0..4095 (12-bit), matching original's table-based LUT.
 * Original uses 1024-entry atan LUT at 0x463214; we use atan2f for precision.
 *
 * Coordinate system: 0=north(+Z), 0x400=east(+X), 0x800=south(-Z), 0xC00=west(-X)
 * ======================================================================== */

/* AngleFromVector12Full (0x00433FC0). L5 promotion sweep audit (2026-05-18).
 * [ARCH-DIVERGENCE @ 0x00433FC0] Orig calls AngleFromVector12 (0x0040A720)
 *   which indexes a 12-bit atan2 LUT at DAT_00463214. Port uses libm
 *   atan2(double) scaled to 12-bit modular units. Cosmetically equivalent:
 *   same +Z-axis-clockwise-positive convention, same 0..0xFFF output range,
 *   matches orig within +/-1 LSB.
 */
static int32_t ai_angle_from_vector(int32_t dx, int32_t dz) {
    if (dx == 0 && dz == 0) return 0;
    /* atan2(dx, dz) gives angle from +Z axis, clockwise = positive */
    double angle_rad = atan2((double)dx, (double)dz);
    /* Convert from [-pi, pi] to [0, 4096) */
    int32_t angle = (int32_t)(angle_rad * (4096.0 / (2.0 * 3.14159265358979323846)));
    return angle & 0xFFF;
}

/* Compute route heading from route table for a given actor span.
 * Matches inline computation at 0x43503A in the original, which reads
 * span_normalized (+0x82), NOT span_raw (+0x80). Post-junction-remap the
 * two diverge by thousands (e.g. 499→2790 on Moscow); indexing the route
 * table with raw returns heading bytes for the wrong ring position and
 * makes the recovery-misalignment check at 0x00434FE0 fire at every
 * branch entry — the visible symptom is the AI braking into the turn. */
/* [CRASH FIX 2026-06-15 Scotland/level016] Bounds-checked route-table byte read
 * (lane at k=0, heading at k=1). Returns 0 (safe default) when the span lies past
 * the route table: some TD6-converted tracks ship LEFT/RIGHT.TRK SHORTER than the
 * strip's span count, so an in-ring span (Scotland span 3092 vs a shorter table)
 * exceeded the table and OOB-crashed (0xC0000005 — #20 Newcastle class). `table`
 * must be g_route_tables[0] or [1]. Pure safety bound: identical result for any
 * in-range span, only the crashing out-of-range read is replaced with 0. */
static int ai_route_byte(const uint8_t *table, int span, int k) {
    if (!table || span < 0) return 0;
    size_t sz  = (table == g_route_tables[1]) ? g_route_table_sizes[1]
                                              : g_route_table_sizes[0];
    size_t off = (size_t)(unsigned)span * 3u + (unsigned)k;
    if (off >= sz) {
        /* Out-of-range span: return the safe default (0) instead of reading
         * past the route table. One-shot WARN so a field occurrence is visible
         * without per-tick spam. This is the central guard for the
         * Courmayeur/Italy (level027) route-table OOB crash: that track's
         * LEFT/RIGHT.TRK are 1800 bytes (600 rows) while a branch-remapped
         * span_normalized can transiently exceed the table, and the original
         * tolerated the OOB read only because its route tables sit inside one
         * big game heap (no guard page). */
        static int s_route_oob_warned = 0;
        if (!s_route_oob_warned) {
            s_route_oob_warned = 1;
            TD5_LOG_W(LOG_TAG,
                "ai_route_byte: span=%d k=%d off=%lu past route table sz=%lu "
                "-> returning 0 (route-table OOB guard)",
                span, k, (unsigned long)off, (unsigned long)sz);
        }
        return 0;
    }
    return (int)table[off];
}

static int32_t ai_route_heading_for_actor(const int32_t *rs, const char *actor) {
    const uint8_t *rb = (const uint8_t *)(intptr_t)rs[RS_ROUTE_TABLE_PTR];
    int16_t sp = ACTOR_I16(actor, ACTOR_SPAN_NORMALIZED);
    if (rb && sp >= 0) {
        return (((int)ai_route_byte(rb, sp, 1) * 0x102C) >> 8) & 0xFFF;
    }
    return 0;
}

/* ========================================================================
 * Module lifecycle
 * ======================================================================== */

int td5_ai_init(void) {
    memset(g_live_throttle, 0, sizeof(g_live_throttle));
    memset(g_route_state_storage, 0, sizeof(g_route_state_storage));
    memset(g_actor_route_steer_bias, 0, sizeof(g_actor_route_steer_bias));
    memset(g_actor_forward_track_component, 0, sizeof(g_actor_forward_track_component));
    memset(g_slot_state, 0, sizeof(g_slot_state));
    memset(g_traffic_recovery_stage, 0, sizeof(g_traffic_recovery_stage));
    memset(g_encounter_active, 0, sizeof(g_encounter_active));
    memset(g_script_bank_index, 0, sizeof(g_script_bank_index));
    memset(g_last_logged_opcode, 0xFF, sizeof(g_last_logged_opcode));
    g_encounter_tracked_handle = -1;
    g_encounter_cooldown = 0;
    g_encounter_enabled = 0;
    g_encounter_control_active_latch = 0;
    g_encounter_steer_bias_latch = 0;
    g_encounter_phase_flag = 0;
    cop_state_reset();
    g_active_actor_count = 6;
    g_lateral_avoidance_direction = 0;
    g_ai_frame_counter = 0;
    g_rb_behind_scale = 0x80;
    g_rb_ahead_scale = 0x80;
    g_rb_behind_range = 0x80;
    g_rb_ahead_range = 0x80;
    g_route_state_base = g_route_state_storage;
    g_actor_base = NULL;
    g_route_tables[0] = NULL;
    g_route_tables[1] = NULL;
    g_route_table_sizes[0] = 0;
    g_route_table_sizes[1] = 0;
    return 1;
}

void td5_ai_shutdown(void) {
    /* nothing to free -- all static */
}

/* Per-slot encounter latch accessor [CONFIRMED @ 0x00403180].
 * Original UpdatePlayerVehicleControlState gates the encounter-control branch
 * on a per-slot field at gActorSpecialEncounterActive[slot * 0x11c], NOT on
 * the global g_wantedModeEnabled. Without this distinction, the port routed
 * every Cop Chase frame into td5_ai_update_encounter_control, blocking the
 * player's throttle write to actor+0x33E. */
int td5_ai_is_encounter_active(int slot) {
    if (slot < 0 || slot >= TD5_MAX_TOTAL_ACTORS) return 0;
    return g_encounter_active[slot] != 0;
}

/* ========================================================================
 * Police chase — public API for the cosmetic layer (render/sound).
 *
 * These read the deterministic sim state computed in the AI tick. They are
 * pure queries (no sim writes) and are safe to call from the per-frame
 * render/audio path; the values are identical on every peer for any given
 * sim tick, but the consumers may further weight them by the LOCAL camera
 * (e.g. siren distance), which is purely cosmetic. ====================== */

/* 1 when `slot` is a cop currently engaged in a chase (pursuing or pulling a
 * caught target over). Gated by the POLICE option at the call sites. */
int td5_ai_cop_is_chasing(int slot) {
    if (slot < 0 || slot >= TD5_MAX_TOTAL_ACTORS) return 0;
    return g_cop_is_cop[slot] &&
           (g_cop_phase[slot] == COP_CHASING || g_cop_phase[slot] == COP_PULLOVER);
}

/* Steady strobe intensity for the cop-light glow over a chasing cop. The
 * marker's quad radii scale linearly with this, so 0x1000 is the wanted-mode
 * full size; cops use a larger value so the lights read clearly from a
 * distance (env TD5RE_COP_GLOW overrides). Stays steady (no decay) for the
 * whole chase. */
int td5_ai_cop_glow_intensity(int slot) {
    static int s_glow = -1;
    if (s_glow < 0) {
        const char *e = getenv("TD5RE_COP_GLOW");
        s_glow = e ? atoi(e) : 0x3000;     /* 3x the wanted-mode size */
        if (s_glow < 0x400)  s_glow = 0x400;
        if (s_glow > 0x8000) s_glow = 0x8000;
        TD5_LOG_I(LOG_TAG, "cop_glow intensity = 0x%X (env TD5RE_COP_GLOW)", s_glow);
    }
    if (!td5_ai_cop_is_chasing(slot)) return 0;
    return s_glow;
}

/* 1 when `slot` (racer OR traffic/cop) is in the broken-down state set by a
 * hard collision in td5_physics.c — used to emit smoke + keep it parked. */
int td5_ai_actor_is_broken_down(int slot) {
    if (slot < 0 || slot >= TD5_MAX_TOTAL_ACTORS) return 0;
    return g_actor_broken_down[slot] != 0;
}

/* 1 when `slot` is currently the target of some cop's chase. td5_physics.c uses
 * this so a hard crash by a chased racer flags it broken-down (ending the
 * pursuit) without flagging ordinary racers who merely bumped something. */
int td5_ai_actor_is_pursued(int slot) {
    if (slot < 0 || slot >= TD5_MAX_TOTAL_ACTORS) return 0;
    /* [MULTI-COP 2026-06-29] A target may be pursued by MORE THAN ONE cop at once
     * now (the single g_actor_pursued_by[] slot was removed). Scan the cops so this
     * stays correct regardless of how many are chasing: pursued == ANY cop is
     * actively chasing / pulling over this slot.
     *
     * Scoped to TRAFFIC-slot cops (>= g_traffic_slot_base) — exactly the set the
     * removed g_actor_pursued_by[] tracked. The MP cop-chase mode uses player-slot
     * cops that never set that field, so they were never reported pursued here;
     * keeping the lower bound preserves that (no change to the MP cop path). */
    for (int k = g_traffic_slot_base; k < TD5_MAX_TOTAL_ACTORS; k++) {
        if (g_cop_is_cop[k] &&
            (g_cop_phase[k] == COP_CHASING || g_cop_phase[k] == COP_PULLOVER) &&
            g_cop_target[k] == slot)
            return 1;
    }
    return 0;
}

/* 1 when `slot` is a cop car (any phase, incl. idle-cruising). td5_render.c
 * uses this to draw the dedicated police mesh over the slot so cops look like
 * cops in traffic, not ordinary cars. */
int td5_ai_actor_is_cop(int slot) {
    if (slot < 0 || slot >= TD5_MAX_TOTAL_ACTORS) return 0;
    return g_cop_is_cop[slot] != 0;
}

/* [#5 2026-06-20] Wrecks are PERMANENT by default: a totalled traffic/cop car
 * never drives itself again — it sits and smokes until the slot is recycled out
 * by distance (td5_ai_recycle_traffic_actor clears the flag). Set
 * TD5RE_WRECK_PERMANENT=0 to restore the old timed recovery (cop_smoke_ticks
 * then drives again). */
static int wreck_permanent_enabled(void) {
    static int s = -1;
    if (s < 0) {
        const char *e = getenv("TD5RE_WRECK_PERMANENT");
        s = (!e || e[0] != '0') ? 1 : 0;
    }
    return s;
}

/* Flag an actor as broken down (a wreck). Called from the physics collision
 * response on a hard V2V / wall impact for traffic and cop slots. Idempotent
 * while already broken. Player/AI racer slots are left to the existing crash-fx
 * path (this is for traffic/cops; for a pursued racer it only ends the chase).
 * Permanent by default (ticks = -1); see wreck_permanent_enabled(). */
void td5_ai_mark_actor_broken_down(int slot) {
    if (slot < 0 || slot >= TD5_MAX_TOTAL_ACTORS) return;
    g_actor_broken_down[slot] = 1;
    /* [COP RE-CHASE FIX 2026-06-27] A RACER/PLAYER slot must NEVER be flagged a
     * PERMANENT wreck. For a racer this flag's only job is to END an in-progress
     * cop chase (the scheduler ends the pursuit when g_actor_broken_down[tgt]
     * is set); racers are never parked/smoked/immobilised by it (see
     * cop_tick_broken_down). But racer slots are never recycled by distance
     * (only traffic >= g_traffic_slot_base is), and cop_tick_broken_down never
     * decrements a negative (permanent) timer — so a permanent flag here would
     * stay set for the WHOLE race, and the acquire loop's
     * `if (g_actor_broken_down[c]) continue;` would then make EVERY future cop
     * skip that car. Result: a player who crashes hard while being chased gets
     * pursued exactly once, then no cop ever chases them again (the reported
     * MP-splitscreen "first cop chase then no more" bug; the cop's own ramming
     * makes a crash-during-chase the common case). Give racers a short
     * self-clearing timer (cop_smoke_ticks) so the chase ends, then the car is
     * chaseable again. Traffic/cops keep the permanent-until-recycled wreck. */
    if (slot < g_traffic_slot_base) {
        g_actor_broken_ticks[slot] = (int16_t)g_td5.ini.cop_smoke_ticks;
        TD5_LOG_I(LOG_TAG,
                  "mark_broken_down: RACER slot=%d -> transient %d ticks "
                  "(chase ends, re-chaseable after timer; never a permanent wreck)",
                  slot, g_td5.ini.cop_smoke_ticks);
        return;
    }
    g_actor_broken_ticks[slot] = wreck_permanent_enabled()
        ? (int16_t)-1
        : (int16_t)g_td5.ini.cop_smoke_ticks;
}

/* Nearest chasing cop to the given listener world position (24.8 fixed), or
 * -1 if none. td5_sound.c uses this to drive the (single) siren channel from
 * the closest active chase, then attenuates by that cop's distance. */
int td5_ai_nearest_chasing_cop(int32_t listener_x, int32_t listener_z) {
    int best = -1;
    int64_t best_d2 = 0;
    if (!g_actor_base) return -1;
    for (int s = 0; s < TD5_MAX_TOTAL_ACTORS; s++) {
        if (!td5_ai_cop_is_chasing(s)) continue;
        /* [#R12 2026-06-19] Only sound the siren when a cop is chasing a HUMAN
         * player's car, not an AI opponent — otherwise the player heard a siren
         * "despite not being on a chase" (a cop was pursuing a nearby AI).
         * g_slot_state[tgt]==1 marks a human racer slot; cop targets are always
         * racer slots (< TD5_MAX_RACER_SLOTS). */
        int tgt = g_cop_target[s];
        if (tgt < 0 || tgt >= TD5_MAX_RACER_SLOTS || g_slot_state[tgt] != 1)
            continue;
        char *a = actor_ptr(s);
        int64_t dx = (int64_t)ACTOR_I32(a, ACTOR_WORLD_POS_X) - listener_x;
        int64_t dz = (int64_t)ACTOR_I32(a, ACTOR_WORLD_POS_Z) - listener_z;
        int64_t d2 = dx * dx + dz * dz;
        if (best < 0 || d2 < best_d2) { best = s; best_d2 = d2; }
    }
    return best;
}

/* [FIX 2026-05-24 OVERSIGHT: missing-gates-and-arrest-counter; orig 0x0043D690
 * AwardWantedDamageScore]
 * [FIX 2026-05-24 wanted-scoring-writer; orig 0x0043D329 / 0x0043D6A0]
 *
 * Mirror port of DAT_004bf518 — orig "wanted-damage scoring suppressed" flag.
 * Ghidra writer audit (TD5_d3d.exe, pool1, 2026-05-24): the variable has
 * exactly ONE writer (InitializeWantedHudOverlays @ 0x0043D329 writing 0)
 * and ONE reader (this gate @ 0x0043D6A0 testing `== 0`). No code path in
 * the orig binary ever sets it to 1, so the gate is permanently open and
 * the variable is effectively a vestigial debug/cheat-disable hook.
 *
 * Port semantic: 1 = scoring ENABLED (gate open, default), 0 = suppressed.
 * Default 1 mirrors orig's "always-on" runtime behaviour byte-for-byte.
 * Kept as a named static so future debug toggles can flip it; no writer to
 * wire from orig because none exists. */
static int s_wanted_scoring_enabled = 1;

/* Slot whose damage bar the HUD currently shows — mirrors
 * g_wantedDamageHudOverlayCount @ 0x004bf504 (orig .data init -1, reset to -1
 * by InitializeWantedHudOverlays). Set to the last-rammed suspect on first
 * contact and used as the first-hit-vs-rehit discriminator. The HUD reads it
 * via td5_ai_get_wanted_overlay_slot() to float the DAMAGE bar over that car.
 *
 * [FIX 2026-05-30 cop-chase] Previously a function-local static here that the
 * HUD never saw (hud_wanted_active_slot() hardcoded 0), so the damage bar only
 * tried to render over slot 0 (the player/cop) — which never takes ram damage
 * — and therefore never appeared. Promoted to file scope + exposed. */
static int s_wanted_hud_overlay_slot = -1;

/* Accessor for the HUD damage-bar gate. Returns the last-rammed suspect slot
 * (1..5) or -1 when none has been hit yet. */
int td5_ai_get_wanted_overlay_slot(void) { return s_wanted_hud_overlay_slot; }

/* Reset per-race cop-chase transient state (mirrors the -1 reset in
 * InitializeWantedHudOverlays @ 0x0043D2D0). */
void td5_ai_reset_wanted_state(void) {
    s_wanted_hud_overlay_slot = -1;
    /* [COP CHASE HUD 2026-06-27] Clear the per-suspect collision-arrow and the
     * per-cop siren-warning timestamps so a stale tick from a previous race
     * can't flash an arrow / reminder at the next race's start. */
    for (int i = 0; i < TD5_MAX_RACER_SLOTS; i++) {
        g_wanted_hit_tick[i]       = TD5_AI_NO_HIT_TICK;
        g_cop_siren_warn_tick[i]   = TD5_AI_NO_HIT_TICK;
    }
}

/* Award damage to a cop slot on player<->cop collision.
 * Mirrors AwardWantedDamageScore @ 0x43D690 [CONFIRMED]:
 *   - Multiple early-out gates (impact_mag, scoring-enabled flag, target tracker)
 *   - First-hit vs re-hit branching:
 *       first hit → reset message timer, choose random message, no decrement
 *       re-hit   → decrement state, increment arrest counter, bump timer
 *   - Decrement: 0x400 if impact_mag > 20000, else 0x200.
 *   - Clamp to [0, 0x1000].
 *   - When state reaches 0, cop AI is frozen (arrested).
 * Called from td5_physics.c when player (slot 0) collides with a cop. */
/* [COP CHASE ARREST 2026-06-25] Returns 1 when THIS hit completed the arrest (the
 * suspect's wanted damage just reached 0), else 0. The physics caller uses it to
 * fire the strong short arrest jolt on both the cop and the busted suspect.
 * `suspect_slot` is the rammed SUSPECT; `cop_slot` the COP that rammed it — the
 * arrest is credited to the actual ramming cop (per-cop tally in multi-cop) and
 * the suspect's arrest time stamped. [MP COP CHASE results 2026-06-25] */
int td5_ai_wanted_cop_hit(int cop_slot, int suspect_slot, int32_t impact_mag) {
    int16_t dec, cur;
    char *actor;
    extern int g_wanted_msg_timer;   /* td5_hud.c */
    extern int g_wanted_msg_index;

    /* `suspect_slot` is the rammed SUSPECT; `cop_slot` is the COP that rammed it
     * (the caller in td5_physics.c resolves which is which). Crediting the actual
     * ramming cop — rather than the single representative cop — is what makes a
     * MULTI-cop chase award each cop their OWN arrest tally (the cop-chase
     * results screen lists per-cop arrests). [MP COP CHASE results 2026-06-25] */
    if (suspect_slot < 0 || suspect_slot >= TD5_MAX_RACER_SLOTS) return 0;
    if (cop_slot < 0 || cop_slot >= TD5_MAX_RACER_SLOTS) return 0;

    /* Gate 1 [orig 0x0043D690 entry]: impact must be material (> 9999). */
    if (impact_mag <= 9999) return 0;

    /* Gate 2 [orig 0x0043D6A0 test on DAT_004bf518]: scoring-enabled flag. */
    if (s_wanted_scoring_enabled == 0) return 0;

    /* Gate 3 [orig 0x0043D6B4]: never damage a cop. The caller already passes a
     * non-cop suspect, but guard against a cop-vs-cop accidental collision. */
    if (td5_game_cop_chase_is_cop(suspect_slot)) return 0;

    /* [COP CHASE HUD 2026-06-27] Record this material crash so the bust ARROW
     * shows over THIS suspect for a short window. The indicator is otherwise
     * hidden (user: the arrow should only appear when you crash into a car, not
     * persistently over every suspect). Set on first-hit AND re-hit, and BEFORE
     * the siren gate below so the arrow appears on a crash even when the siren
     * is off (it just won't make arrest progress). */
    g_wanted_hit_tick[suspect_slot] = g_td5.simulation_tick_counter;

    /* [COP CHASE SIREN GATE 2026-06-27, PORT-ONLY] In LOCAL MP cop chase a cop
     * can only ARREST a suspect while its SIREN is ON. If the ramming cop's
     * siren is off, credit no damage/arrest and flash a "TURN ON YOUR SIREN"
     * reminder on that cop's pane instead. The original arrest path has NO siren
     * check [CONFIRMED @ 0x0043D690]; this is a requested gameplay rule scoped to
     * MP cop chase so SP wanted mode + netplay keep faithful behaviour. */
    {
        int mp_cop_chase = (g_td5.mp_mode_config.mode == TD5_MP_MODE_COP_CHASE &&
                            !g_td5.network_active);
        extern int td5_sound_cop_siren_is_on(int slot);
        if (mp_cop_chase && !td5_sound_cop_siren_is_on(cop_slot)) {
            g_cop_siren_warn_tick[cop_slot] = g_td5.simulation_tick_counter;
            TD5_LOG_I(LOG_TAG,
                      "wanted_no_siren: cop=%d rammed suspect=%d siren OFF — no "
                      "arrest, siren reminder shown", cop_slot, suspect_slot);
            return 0;
        }
    }

    /* First-hit-vs-rehit branch [orig 0x0043D6D8]: if the HUD overlay was
     * NOT pointing at this suspect last frame, this is a "first hit" — reset
     * the wanted-message banner, choose a random message, and return
     * WITHOUT applying any damage decrement. */
    if (s_wanted_hud_overlay_slot != suspect_slot) {
        s_wanted_hud_overlay_slot = suspect_slot;
        g_wanted_msg_timer        = 0;
        /* Orig picks rand() & 7 over the 8 wanted-message pairs (table
         * @ 0x474038) [CONFIRMED @ 0x0043D6F? — DAT_004bf508 = rand() & 7]. */
        g_wanted_msg_index = rand() & 7;
        TD5_LOG_I(LOG_TAG,
                  "wanted_first_hit: suspect=%d cop=%d msg_idx=%d impact=%d",
                  suspect_slot, cop_slot, g_wanted_msg_index, (int)impact_mag);
        return 0;
    }

    /* Re-hit path [orig 0x0043D6F8 onward]: only a suspect with health left
     * can take damage. Light hit (force 10k..20k) = -0x200 + 10 pts; heavy
     * hit (>20k) = -0x400 + 20 pts; the hit that depletes health adds the
     * +50 kill bonus and bumps the bust counter. Score accrues to the
     * ramming cop. */
    if (g_wanted_damage_state[suspect_slot] <= 0) {
        /* Already busted — no further damage/score (orig gate `health > 0`). */
        return 0;
    }
    dec = (impact_mag > 20000) ? (int16_t)0x400 : (int16_t)0x200;
    int points = (dec == (int16_t)0x200) ? 10 : 20;
    cur = g_wanted_damage_state[suspect_slot] - dec;
    int killed = 0;
    if (cur <= 0) {
        cur = 0;
        points += 50;          /* kill bonus [orig 0x0043D720] */
        killed = 1;
    }
    if (cur > 0x1000) cur = 0x1000;   /* upper clamp [orig] */
    g_wanted_damage_state[suspect_slot] = cur;

    /* Award ram points + (on bust) the kill to the COP that did the ramming so
     * the HUD score and the per-cop arrests column populate. */
    td5_game_add_wanted_score(cop_slot, points);
    if (killed) {
        td5_game_add_wanted_kill(cop_slot);
        /* Stamp this suspect's time of arrest for the cop-chase results screen
         * (shown there as '-' if a suspect never gets stamped). */
        td5_game_set_arrest_time(suspect_slot);
    }

    TD5_LOG_I(LOG_TAG,
              "wanted_damage: suspect=%d cop=%d new_state=%d dec=%d impact=%d "
              "pts=%d killed=%d",
              suspect_slot, cop_slot, (int)cur, (int)dec, (int)impact_mag, points, killed);

    if (killed) {
        /* Suspect busted: freeze AI — matches original gate at 0x436E1D */
        actor = actor_ptr(suspect_slot);
        if (actor) {
            ACTOR_U8(actor, ACTOR_BRAKE_FLAG) = 1;
            ACTOR_I32(actor, ACTOR_STEERING_CMD) = 0;
            ACTOR_I16(actor, ACTOR_ENCOUNTER_STEER) = 0;
        }
        TD5_LOG_I(LOG_TAG, "wanted_arrest: suspect slot=%d arrested by cop=%d", suspect_slot, cop_slot);
        /* [MP COP CHASE INFECT 2026-06-25] In INFECT mode the bust still scores
         * above, but the suspect is then CONVERTED into a cop (random police car)
         * rather than staying parked. Queue it; the heavy car-swap runs at a safe
         * point next frame (td5_game_process_pending_infections). No-op when the
         * INFECT toggle is off, so the classic park-on-arrest path is unchanged. */
        td5_game_infect_request(suspect_slot);
    }
    return killed;
}

void td5_ai_bind_actor_table(void *actor_base) {
    extern void *g_route_data;

    g_actor_base = (char *)actor_base;
    g_route_state_base = g_route_state_storage;
    g_route_data = g_route_state_base;
}

void td5_ai_set_route_tables(const uint8_t *left_route, size_t left_size,
                             const uint8_t *right_route, size_t right_size) {
    g_route_tables[0] = left_route;
    g_route_table_sizes[0] = left_size;
    g_route_tables[1] = right_route;
    g_route_table_sizes[1] = right_size;
    td5_ai_refresh_route_state();
}

/**
 * Correct an AI actor's spawn heading to match the LEFT.TRK route byte.
 *
 * Problem: td5_track_compute_heading writes a geometry-derived heading that
 * is ~1050 angle units off from what the LEFT.TRK route byte expects.  This
 * causes td5_ai_update_track_behavior to enter the recovery-script loop on
 * every tick from the very first frame because hdelta > 0x320 (800 decimal)
 * is immediately true.  With v=0 the bicycle model cannot generate yaw
 * torque, so the script never successfully turns the car, and the recovery
 * fires forever → throttle stays 0 → cop never moves.
 *
 * Fix: after compute_heading seeds the yaw accumulator from geometry, re-seed
 * it from the route-byte heading instead.  route_bytes[span*3+1] encodes the
 * expected 12-bit heading as route_heading = (rb * 0x102C) >> 8.  Writing
 * that value to ACTOR_YAW_ACCUM (shift left 8 to match the 20-bit accum
 * convention) places the car at exactly the heading the route cascade expects,
 * suppressing the recovery check.
 *
 * Called from td5_game.c immediately after td5_track_compute_heading() for
 * AI racer slots (state == 0).  Only acts when the route table is available.
 *
 * [CONFIRMED need @ race.log "recovery: slot=1 hdelta=0x41A heading=0xB90
 *  route=0xFAA" — same delta for every tick of the race, car never moves]
 */
void td5_ai_correct_spawn_heading(int slot) {
    int32_t *rs;
    const uint8_t *route_bytes;
    int16_t span;
    int32_t rb, route_heading;
    char *actor;

    if (slot < 0 || slot >= g_traffic_slot_base || !g_route_state_base)
        return;

    rs = route_state(slot);
    if (!rs) return;

    actor = actor_ptr(slot);
    span = ACTOR_I16(actor, ACTOR_SPAN_RAW);
    if (span < 0) return;

    route_bytes = (const uint8_t *)(intptr_t)rs[RS_ROUTE_TABLE_PTR];
    rb = ai_route_byte(route_bytes, span, 1);

    if (route_bytes && rb >= 4) {
        /* Primary path: derive the spawn heading from the route byte.
         * Same formula as ai_route_heading_for_actor [CONFIRMED @ td5_ai.c:305]:
         * route_heading = (rb * 0x102C) >> 8.  (rb < 4 is a junction-zone
         * sentinel that can't encode a heading; route_bytes==NULL is a track
         * that shipped without LEFT/RIGHT.TRK — both fall through below.) */
        route_heading = ((rb * 0x102C) >> 8) & 0xFFF;

        TD5_LOG_I(LOG_TAG,
                  "correct_spawn_heading: slot=%d span=%d rb=%d geom_yaw=%d route_yaw=%d",
                  slot, (int)span, (int)rb,
                  ACTOR_I32(actor, ACTOR_YAW_ACCUM) >> 8,
                  route_heading);

        /* Write route heading to the 20-bit yaw accumulator (<<8 = 12→20 bit) */
        ACTOR_I32(actor, ACTOR_YAW_ACCUM) = route_heading << 8;
        return;
    }

    /* [S21 DEFENSIVE FALLBACK 2026-06-05] No usable route byte (route-less
     * track, or a junction-sentinel span where rb < 4).  Previously this
     * returned early and left the geometry yaw from td5_track_compute_heading,
     * which on TD6 tracks lands ~90° off the real travel direction (TD6's
     * in-span vertex layout differs from TD5's) — the AI then accelerates
     * sideways into a wall and stalls "in the middle of the road".  Instead,
     * derive the heading from the track CENTERLINE TANGENT: sample the
     * centerline world XZ at the spawn span and 4 spans ahead and take atan2 of
     * the delta.  The tangent is layout-independent, so the car faces down-track
     * regardless of vertex layout or missing route data.
     *
     * Yaw convention: ACTOR_YAW_ACCUM stores (angle + 0x800) << 8 — the same
     * +0x800 baseline the route_heading path and the geometry spawn pose use
     * (td5_game.c spawn writes (geom_angle + 0x800) << 8; deviation baseline is
     * ~0x800, see td5_ai.c:4010).  This function is only called from the
     * TD6-gated site (td5_game.c, g_active_td6_level > 0), so faithful TD5
     * tracks never reach here and keep byte-identical behaviour; for tracks
     * that have a valid route byte (all currently-migrated TD6 levels at their
     * spawn spans, e.g. Rome rb=188) the primary path above is taken. */
    {
        int span_count = td5_track_get_span_count();
        int s0 = (int)span;
        int s1, x0 = 0, z0 = 0, x1 = 0, z1 = 0;
        if (span_count <= 1) return;
        s1 = (s0 + 4) % span_count;     /* 4 spans ahead, matching AI lookahead */
        if (td5_track_sample_target_point(s0, 128, &x0, &z0, 0) &&
            td5_track_sample_target_point(s1, 128, &x1, &z1, 0)) {
            int32_t dx = (int32_t)(x1 - x0) >> 8;   /* 24.8 FP -> integer */
            int32_t dz = (int32_t)(z1 - z0) >> 8;
            int32_t tangent = ai_angle_from_vector(dx, dz) & 0xFFF;
            int32_t yaw12   = (tangent + 0x800) & 0xFFF;
            TD5_LOG_I(LOG_TAG,
                      "correct_spawn_heading: slot=%d span=%d TANGENT fallback "
                      "(rb=%d no usable route byte) geom_yaw=%d tangent_yaw=%d dx=%d dz=%d",
                      slot, (int)span, (int)rb,
                      ACTOR_I32(actor, ACTOR_YAW_ACCUM) >> 8, yaw12, dx, dz);
            ACTOR_I32(actor, ACTOR_YAW_ACCUM) = yaw12 << 8;
        }
    }
}


void td5_ai_refresh_route_state(void) {
    int slot_count = g_active_actor_count;

    if (!g_route_state_base || !g_actor_base) {
        return;
    }

    if (slot_count < 0) {
        slot_count = 0;
    }
    if (slot_count > TD5_MAX_TOTAL_ACTORS) {
        slot_count = TD5_MAX_TOTAL_ACTORS;
    }

    for (int slot = 0; slot < slot_count; ++slot) {
        td5_ai_refresh_route_state_slot(slot);
    }
}

/* ========================================================================
 * UpdateActorTrackBounds @ 0x004366E0 (port byte-faithful)
 *
 * Samples the actor's four wheel-contact probe positions (+0x90 .. +0xB7,
 * stride 0x0C) against the actor's current span_raw, stores the four progress
 * dwords into route_state scratch slots 10..13, computes min/max into RS[5]
 * and RS[6], copies RS[7]=RS[11] / RS[8]=RS[10], and caches signed offsets
 * (computed against both LEFT and RIGHT route tables at the min/max progress)
 * into RS_RIGHT_BOUNDARY_A/B (0x10/0x11) and RS_RIGHT_EXTENT_A/B (0x12/0x13).
 *
 * The min/max + four signed-offset writes are consumed by the per-slot
 * bias-clamp chain at 0x00436BFE-0x00436E50 and ultimately drive the four
 * boundary writebacks (RS_LEFT/RIGHT_BOUNDARY + RS_ACTIVE_LOWER/UPPER_BOUND).
 * ======================================================================== */
static void td5_ai_update_actor_track_bounds(int slot) {
    int32_t *rs;
    char *actor;
    int span_raw;
    int span_norm;
    int32_t progress[4];
    int32_t min_p, max_p;
    const uint8_t *left_table  = g_route_tables[0];
    const uint8_t *right_table = g_route_tables[1];

    if (!g_route_state_base || !g_actor_base ||
        slot < 0 || slot >= TD5_MAX_TOTAL_ACTORS) {
        return;
    }

    rs    = route_state(slot);
    actor = actor_ptr(slot);
    span_raw  = (int)(int16_t)ACTOR_I16(actor, ACTOR_SPAN_RAW);
    span_norm = (int)(int16_t)ACTOR_I16(actor, ACTOR_SPAN_NORMALIZED);

    /* Four wheel-contact probe ComputeTrackSpanProgress calls.
     * [CONFIRMED @ 0x004366E0-0x00436749] — actor+0x90, +0x9C, +0xA8, +0xB4. */
    {
        const int probe_offsets[4] = {
            ACTOR_PROBE_FL_BASE, ACTOR_PROBE_FR_BASE,
            ACTOR_PROBE_RL_BASE, ACTOR_PROBE_RR_BASE
        };
        for (int i = 0; i < 4; ++i) {
            int32_t *probe = (int32_t *)(actor + probe_offsets[i]);
            int64_t prog64 = td5_track_compute_span_progress(span_raw, probe);
            progress[i] = (int32_t)(uint32_t)(prog64 & 0xFFFFFFFFu);
            rs[10 + i] = progress[i];  /* RS scratch 10..13 = DAT_004afb88..94 */
        }
    }

    /* min/max scan + copy [CONFIRMED @ 0x00436798-0x004367DD]:
     *   min  = clamp init 0x10000, max = init -0x10000
     *   RS[5] = min, RS[6] = max
     *   RS[7] = RS[11] (probe_FR progress)
     *   RS[8] = RS[10] (probe_FL progress) */
    min_p =  0x10000;
    max_p = -0x10000;
    for (int i = 0; i < 4; ++i) {
        if (progress[i] < min_p) min_p = progress[i];
        if (progress[i] > max_p) max_p = progress[i];
    }
    rs[5] = min_p;
    rs[6] = max_p;
    rs[7] = progress[1];
    rs[8] = progress[0];

    /* Four signed-offset writes [CONFIRMED @ 0x004367E0-0x00436878]:
     *   RS[0x10] = signed_offset(span_raw, min, LEFT[span_norm*3])
     *   RS[0x11] = signed_offset(span_raw, max, LEFT[span_norm*3])
     *   RS[0x12] = signed_offset(span_raw, min, RIGHT[span_norm*3])
     *   RS[0x13] = signed_offset(span_raw, max, RIGHT[span_norm*3])
     *
     * NB: the route_byte read is keyed on span_normalized (not span_raw)
     * — the original reads actor->field_0x82, never +0x80, in this block. */
    {
        int route_byte_left  = 0;
        int route_byte_right = 0;
        /* [CRASH FIX 2026-06-14 #20] Bound the route-table index. The original
         * relied on span_norm always being normalized into the ring (< route rows);
         * a malformed / mis-offset branch jump table could leak an out-of-ring span
         * here and read past the ring-sized LEFT/RIGHT.TRK tables. Clamp like the
         * other route-table reads (see ~lines 4122/6659). */
        if (left_table && span_norm >= 0 &&
            (size_t)(unsigned)span_norm * 3u < g_route_table_sizes[0]) {
            route_byte_left  = (int)left_table[(size_t)(unsigned)span_norm * 3u];
        }
        if (right_table && span_norm >= 0 &&
            (size_t)(unsigned)span_norm * 3u < g_route_table_sizes[1]) {
            route_byte_right = (int)right_table[(size_t)(unsigned)span_norm * 3u];
        }
        rs[RS_RIGHT_BOUNDARY_A] =
            td5_track_compute_signed_offset(span_raw, min_p, route_byte_left);
        rs[RS_RIGHT_BOUNDARY_B] =
            td5_track_compute_signed_offset(span_raw, max_p, route_byte_left);
        rs[RS_RIGHT_EXTENT_A] =
            td5_track_compute_signed_offset(span_raw, min_p, route_byte_right);
        rs[RS_RIGHT_EXTENT_B] =
            td5_track_compute_signed_offset(span_raw, max_p, route_byte_right);
    }
}

/* ========================================================================
 * ClassifyTrackOffsetClamp @ 0x004368A0 (port byte-faithful)
 *
 * Samples the planned look-ahead position 4 spans forward (with
 * junction-table remapping when self is on the RIGHT route) and projects it
 * onto the next span. Returns:
 *   0 = both samples in-range (no clamp)
 *   1 = first sample (cardef+8 offset) projects past the span end → clamp
 *   2 = second sample (cardef+0 offset) projects backwards → clamp
 * Used to pick which side of the bounding box should re-seed the bias on
 * the next sim_tick.
 *
 * NB: this is a probe — it does NOT mutate route state. The caller
 * (td5_ai_refresh_route_state_slot) then rewrites RS_TRACK_OFFSET_BIAS
 * accordingly.
 * ======================================================================== */
static int td5_ai_remap_for_classify(int span_normalized) {
    /* Replicates the inline junction walk at 0x004368C8-0x00436954.
     *   iVar5 = span_normalized + 4
     *   if (g_trackTotalSpanCount <= iVar5):
     *     iVar5 = (span_normalized - g_trackTotalSpanCount) + 4
     *   walk DAT_004c3da0 jump table for a matching entry; remap inline.
     * The port's td5_track_branch_to_junction implements the same lookup
     * (post-wrap) for span indices >= ring_length. */
    int ring = td5_track_get_ring_length();
    int remapped = span_normalized + 4;
    if (ring > 0 && ring <= remapped) {
        remapped = (span_normalized - ring) + 4;
    }
    /* If post-wrap span is inside the branch range, fold back into main road
     * (same as the original's loop body at 0x004368F0-0x00436950). */
    if (ring > 0 && remapped >= ring) {
        int folded = td5_track_branch_to_junction(remapped);
        if (folded >= 0) {
            remapped = folded;
        }
    }
    return remapped;
}

static int td5_ai_classify_track_offset_clamp(int slot, int track_offset_bias) {
    int32_t *rs;
    char *actor;
    int span_norm;
    int sample_span;      /* iVar5: junction-remapped OR simple-wrap */
    int route_byte_idx;   /* iVar4: ALWAYS simple-wrap span_norm+4 */
    int route_byte;
    int target_x = 0;
    int target_z = 0;
    int sample_progress;
    int64_t prog64;
    int16_t *cardef;
    int cardef0, cardef8;

    if (!g_route_state_base || !g_actor_base ||
        slot < 0 || slot >= TD5_MAX_TOTAL_ACTORS) {
        return 0;
    }

    rs    = route_state(slot);
    actor = actor_ptr(slot);
    span_norm = (int)(int16_t)ACTOR_I16(actor, ACTOR_SPAN_NORMALIZED);
    cardef = (int16_t *)(intptr_t)ACTOR_I32(actor, ACTOR_CAR_DEF_PTR);
    if (!cardef) {
        return 0;
    }
    cardef0 = (int)cardef[0];   /* bounds_lo (LEFT-side extent) */
    cardef8 = (int)cardef[4];   /* bounds_hi (RIGHT-side extent), at byte +0x08 */

    /* Two parallel span computations. Match the original's iVar5/iVar4 split:
     *   iVar4 = ALWAYS simple wrap of span_norm + 4
     *   iVar5 = junction-remapped iff RIGHT-selector AND inside branch range,
     *           else simple wrap (same as iVar4)
     * [CONFIRMED @ 0x004368B6-0x004368FB + LAB_0043699B] */
    {
        int ring = td5_track_get_ring_length();
        route_byte_idx = span_norm + 4;
        if (ring > 0 && ring <= route_byte_idx) {
            route_byte_idx = (span_norm - ring) + 4;
        }
        if (route_byte_idx < 0) route_byte_idx = 0;
    }
    {
        const uint8_t *self_table = (const uint8_t *)(intptr_t)rs[RS_ROUTE_TABLE_PTR];
        if (self_table == g_route_tables[0]) {
            sample_span = route_byte_idx;  /* LEFT route — use simple-wrap */
        } else {
            sample_span = td5_ai_remap_for_classify(span_norm);
            if (sample_span < 0) sample_span = route_byte_idx;
        }
    }
    if (sample_span < 0) sample_span = 0;

    /* First sample: cardef[4] (bounds_hi) + track_offset_bias offset
     * [CONFIRMED @ 0x004369C4-0x004369F2]
     *   route_byte = *piVar1[iVar4 * 3]    ← simple-wrap idx, NOT sample_span
     *   SampleTrackTargetPoint(sample_span=iVar5, route_byte, ...) */
    {
        const uint8_t *table = (const uint8_t *)(intptr_t)rs[RS_ROUTE_TABLE_PTR];
        if (!table) return 0;
        route_byte = ai_route_byte(table, route_byte_idx, 0);
        if (!td5_track_sample_target_point(sample_span, route_byte,
                                           &target_x, &target_z,
                                           cardef8 + track_offset_bias)) {
            return 0;
        }
    }
    {
        int32_t pos_vec[3];
        pos_vec[0] = target_x;
        pos_vec[1] = 0;
        pos_vec[2] = target_z;
        prog64 = td5_track_compute_span_progress(sample_span, pos_vec);
        sample_progress = (int)(int32_t)(uint32_t)(prog64 & 0xFFFFFFFFu);
    }
    /* (int)lVar7 < 0xff → return 1 [CONFIRMED @ 0x00436A0C] */
    if (sample_progress < 0xFF) {
        return 1;
    }

    /* Second sample: cardef[0] (bounds_lo) + track_offset_bias offset
     * [CONFIRMED @ 0x00436A12-0x00436A38]
     * Note original recomputes iVar3 (span_norm) here and rebuilds iVar4 from
     * scratch — equivalent to our cached route_byte_idx since span_norm didn't
     * change in between. */
    {
        const uint8_t *table = (const uint8_t *)(intptr_t)rs[RS_ROUTE_TABLE_PTR];
        route_byte = ai_route_byte(table, route_byte_idx, 0);
        if (!td5_track_sample_target_point(sample_span, route_byte,
                                           &target_x, &target_z,
                                           cardef0 + track_offset_bias)) {
            return 0;
        }
    }
    {
        int32_t pos_vec[3];
        pos_vec[0] = target_x;
        pos_vec[1] = 0;
        pos_vec[2] = target_z;
        prog64 = td5_track_compute_span_progress(sample_span, pos_vec);
        sample_progress = (int)(int32_t)(uint32_t)(prog64 & 0xFFFFFFFFu);
    }
    /* `(0 < (int)lVar7) - 1) & 0x200000002` → 2 if positive, 0 otherwise
     * [CONFIRMED @ 0x00436A4D] */
    if (sample_progress > 0) {
        return 2;
    }
    return 0;
}

static void td5_ai_refresh_route_state_slot(int slot) {
    int32_t *rs;
    char *actor;
    int selector;
    const uint8_t *route_table;
    size_t route_count;
    int16_t span;
    int32_t route_heading;
    int32_t forward_heading;

    if (!g_route_state_base || !g_actor_base ||
        slot < 0 || slot >= TD5_MAX_TOTAL_ACTORS) {
        return;
    }

    rs = route_state(slot);
    actor = actor_ptr(slot);

    /* Per-tick branch-vs-main selector update [CONFIRMED @ 0x00436AAD..0x00436C9B].
     *
     * Orig UpdateRaceActors has THREE paths for the selector/ptr write:
     *
     *   PATH 1  (span_raw > total_span_count):
     *     selector := 1, ptr := RIGHT.TRK
     *
     *   PATH 2a (junction match — span_norm in entry main-road range AND
     *            branch_lo - main_target + span_norm != -1):
     *     selector := 0, ptr := LEFT.TRK
     *
     *   PATH 2b (default — no junction match):
     *     selector := 0, ptr UNCHANGED   ← KEY DIVERGENCE FROM PRIOR PORT
     *
     * Prior port wrote rs[RS_ROUTE_TABLE_PTR] = g_route_tables[selector]
     * UNCONDITIONALLY. That meant slot 0 (initialized to RIGHT.TRK by
     * td5_ai_initialize_runtime via `selector = (slot & 1) ? 0 : 1`) had
     * its ptr force-rewritten to LEFT.TRK on every tick where span_raw <=
     * total. In orig, slot 0 keeps RIGHT.TRK in PATH 2b → route_byte read
     * via that table → hdelta computed against RIGHT-relative heading →
     * recovery-script trigger fires when hdelta in (0x320, 0xCE0).
     *
     * On Moscow at sub_tick=1, this is what drove RS[0x3D] (script_flags)
     * and RS[0x45] (script_countdown) to stay at 0 in port: with LEFT.TRK
     * substituted, hdelta misses the trigger window, script_base_ptr stays
     * 0, AdvanceActorTrackScript never runs, and the two RS fields never
     * pick up their script-driven values (0x10 / 0x96).
     *
     * Fix (this commit): only write rs[RS_ROUTE_TABLE_PTR] when the orig
     * writes it (PATH 1 or PATH 2a). PATH 2b leaves the ptr alone — it
     * keeps whatever value was set during td5_ai_initialize_runtime (LEFT
     * for odd slots, RIGHT for even slots). */
    {
        int ring_len = td5_track_get_ring_length();
        int span_raw_val = (int)(int16_t)ACTOR_I16(actor, ACTOR_SPAN_RAW);
        int span_norm_val = (int)(int16_t)ACTOR_I16(actor, ACTOR_SPAN_NORMALIZED);

        if (ring_len > 0 && span_raw_val > ring_len) {
            /* PATH 1: span_raw > total → selector=1, ptr=RIGHT
             * [orig 0x436ab4 CMP EAX,ECX / JLE 0x436adb — fall-through on >]. */
            rs[RS_ROUTE_TABLE_SELECTOR] = 1;
            rs[RS_ROUTE_TABLE_PTR] = (int32_t)(intptr_t)g_route_tables[1];
        } else if (td5_track_route_junction_path2a_match(span_norm_val)) {
            /* PATH 2a: junction match → selector=0, ptr=LEFT */
            rs[RS_ROUTE_TABLE_SELECTOR] = 0;
            rs[RS_ROUTE_TABLE_PTR] = (int32_t)(intptr_t)g_route_tables[0];
        } else {
            /* PATH 2b: default → selector=0, ptr UNCHANGED */
            rs[RS_ROUTE_TABLE_SELECTOR] = 0;
        }
    }

    selector = rs[RS_ROUTE_TABLE_SELECTOR] & 1;
    /* route_table now derived from rs[RS_ROUTE_TABLE_PTR] which may have been
     * left UNCHANGED in PATH 2b, OR forcibly set in PATH 1/2a. The selector
     * variable continues to drive downstream branches; the lookup against
     * g_route_tables[selector] is only used to derive route_count for the
     * empty-table fallback and route_byte indexing. To keep downstream code
     * byte-equivalent we still cache route_table = the resolved ptr, but
     * we read from rs (not g_route_tables) so PATH 2b's preserved ptr wins. */
    route_table = (const uint8_t *)(intptr_t)rs[RS_ROUTE_TABLE_PTR];
    /* For route_count, prefer the size that matches the actual ptr. If the
     * stored ptr equals g_route_tables[1] use selector=1's size; otherwise
     * use selector=0's size (covers PATH 2b where ptr keeps initial value). */
    route_count = (route_table == g_route_tables[1])
                  ? (g_route_table_sizes[1] / 3u)
                  : (g_route_table_sizes[0] / 3u);

    /* rs[RS_ROUTE_TABLE_PTR] write is now path-gated above; no fall-through
     * unconditional write here. */
    if (!route_table || route_count == 0u) {
        rs[RS_FORWARD_TRACK_COMP] = 0;
        g_actor_forward_track_component[slot] = 0;
        /* Even without route data, run the bias-clamp Step-12 boundary
         * writebacks (lines 1017-1018 / 1044-1045) using whatever values
         * RS_RIGHT_BOUNDARY_A/B + RS_RIGHT_EXTENT_A/B currently hold (e.g.
         * from replay-injected orig state). Orig's per-tick AI runs this
         * writeback unconditionally; absent it, port's RS_ACTIVE_*_BOUND
         * fields are stale relative to the injected RS[0x10..0x13] values
         * that the replay harness puts in place. This is a port-only fallback
         * for the missing-route-tables scenario (Honolulu level zip is
         * sometimes built without LEFT/RIGHT.TRK in the assets bundle); when
         * route data IS loaded, the gated block below handles the writeback
         * via the same lines, so behaviour is preserved. */
        {
            int slot_is_racer = (slot < g_traffic_slot_base);
            int slot_is_encounter_9 =
                (slot == 9 && g_encounter_tracked_handle != -1);
            int racer_or_encounter = slot_is_racer || slot_is_encounter_9;
            /* Selector-driven branch: SELECTOR==0 → LEFT/main route (matches
             * line 1017-1018 in the gated block); SELECTOR==1 → RIGHT route
             * (matches line 1044-1045). */
            if ((rs[RS_ROUTE_TABLE_SELECTOR] & 1) == 0) {
                /* LEFT route: ACTIVE_UPPER=BOUND_A, ACTIVE_LOWER=BOUND_B.
                 * Racer LEFT-side writeback: RS_LEFT_BOUNDARY_A <- bias. */
                if (racer_or_encounter) {
                    rs[RS_LEFT_BOUNDARY_A] = rs[RS_TRACK_OFFSET_BIAS];
                }
                rs[RS_ACTIVE_UPPER_BOUND] = rs[RS_RIGHT_BOUNDARY_A];
                rs[RS_ACTIVE_LOWER_BOUND] = rs[RS_RIGHT_BOUNDARY_B];
            } else {
                /* RIGHT route: ACTIVE_UPPER=EXTENT_A, ACTIVE_LOWER=EXTENT_B.
                 * Racer RIGHT-side writeback: RS_LEFT_BOUNDARY_B <- bias. */
                if (racer_or_encounter) {
                    rs[RS_LEFT_BOUNDARY_B] = rs[RS_TRACK_OFFSET_BIAS];
                }
                rs[RS_ACTIVE_UPPER_BOUND] = rs[RS_RIGHT_EXTENT_A];
                rs[RS_ACTIVE_LOWER_BOUND] = rs[RS_RIGHT_EXTENT_B];
            }
        }
        return;
    }

    /* Original UpdateActorTrackBehavior @ 0x00435022/0x00435039 reads
     * actor->span_normalized (+0x82), NOT span_raw (+0x80). span_norm is the
     * wrapped-but-not-branch-remapped index consistent with the route-table
     * layout; using span_raw picks up the post-remap jump (e.g. 499→2790 at
     * Moscow's first branch), which clamps to the tail of the route table and
     * reads a heading ~180° off the car's motion — flipping fwd_comp sign and
     * triggering the emergency-brake path post-branch. */
    span = ACTOR_I16(actor, ACTOR_SPAN_NORMALIZED);
    if (span < 0) {
        span = 0;
    } else if ((size_t)span >= route_count) {
        span = (int16_t)(route_count - 1u);
    }

    /* Route-byte → 12-bit angle: (byte * 0x102C) >> 8 [CONFIRMED @ 0x434FFB] */
    route_heading = (((int32_t)route_table[(size_t)span * 3u + 1u] * 0x102C) >> 8) & 0xFFF;

    forward_heading = route_heading;
    /* Polarity at dword 0x3F = gActorRouteDirectionPolarity (0x004afc5c) per
     * UpdateTrafficRoutePlan @ 0x00435ef4 / UpdateSpecialEncounterControl
     * teardown @ 0x00434d40. Prior port read dword 0x25 (byte 0x94) which has
     * NO references in the original — the mirror branch was effectively dead. */
    if (rs[RS_ROUTE_DIRECTION_POLARITY] != 0) {
        forward_heading = (forward_heading + 0x800) & 0xFFF;
    }

    /* Project world-frame velocity onto route heading direction.
     * [CONFIRMED @ 0x436B43-0x436BC4: RS[0x18] = (vx*sin + vz*cos) / sqrt(sin² + cos²)]
     *
     * Original uses an FPU-computed sqrt-magnitude divisor instead of a
     * fixed 4096 shift, because ai_sin/cos_fixed12 use float-to-int
     * truncation which gives `sin² + cos²` slightly below 4096² for
     * non-axis angles. The actual divisor is ~4094..4096 depending on
     * which 12-bit angle bucket the heading falls in.
     *
     * Prior port used `(num) >> 12` which introduces a +1 LSB drift on
     * any non-axis heading. Slot 2 sustained this on 142/186 captured
     * countdown rows (orig=3 vs port=4) — quantified via pool13 paired
     * diff and identified via Ghidra disasm of UpdateRaceActors per-slot
     * body. See memory/reference_fwd_track_comp_sqrt_divisor_2026-05-21.md. */
    {
        int32_t vx = ACTOR_I32(actor, ACTOR_LIN_VEL_X) >> 8;
        int32_t vz = ACTOR_I32(actor, ACTOR_LIN_VEL_Z) >> 8;
        int32_t cos_r = ai_cos_fixed12(forward_heading);
        int32_t sin_r = ai_sin_fixed12(forward_heading);
        int32_t mag2  = sin_r * sin_r + cos_r * cos_r;
        /* lrintf matches orig's FILD/FSQRT/__ftol (CRT _ftol on the FPU
         * with PC=64 + RC=RDN). For positive sqrt results the difference
         * between lrintf (round-nearest) and floor (RC=RDN) is at most
         * 1 LSB; in practice the squared sum sits just below 4096² so
         * sqrt rounds to 4095 in both. If a future probe shows a
         * remaining LSB drift, swap to (int32_t)floorf(sqrtf((float)mag2))
         * for byte-faithfulness with the shipped FPU RC=RDN. */
        int32_t divisor = (int32_t)lrintf(sqrtf((float)mag2));
        int32_t num = vx * sin_r + vz * cos_r;
        int32_t fwd_comp = (divisor != 0) ? (num / divisor) : 0;
        rs[RS_FORWARD_TRACK_COMP] = fwd_comp;
        g_actor_forward_track_component[slot] = fwd_comp;
    }

    /* ===================================================================
     * Pre-loop ClassifyTrackOffsetClamp chain (feature-gated)
     *
     * Port of the per-slot body at 0x00436B43-0x00436E50 in
     * UpdateRaceActors. Without this block, RS_TRACK_OFFSET_BIAS follows a
     * different trajectory across the 160 countdown sub-ticks, leaving the
     * port with ~-1280 more bias at sim_tick=1. That delta cascades into
     * the steering-2x family (steering_command, angular_velocity_yaw,
     * linear_velocity_x/z, euler_accum_yaw, world_pos.x/z) plus the
     * wheel/center suspension state.
     *
     * Gated behind [Trace] ExperimentalBiasClamp=1 so the change is opt-in
     * per session (it can be reverted instantly by flipping the flag).
     *
     * 2026-05-15: unconditional enable for the replay-diff measurement —
     * snapshot-replay isolates per-tick error so the prior trajectory-diff
     * regression (21 → 30 labeled) doesn't reproduce. Per-tick the
     * boundary/bias-clamp writes track orig within ~30-200 units instead of
     * compounding for 160 sub-ticks.
     *
     * 2026-05-16 (Round 3 Wave 1, F1 follow-up): gated to slots 0..5 only.
     * Traffic actors (slots 6..11) use a DIFFERENT init flow
     * (RecycleTrafficActorFromQueue @ 0x004353B0 — see memory
     * reference_arch_recycle_heading_collapse.md) and don't expect these
     * RS_ACTIVE_LOWER/UPPER_BOUND / RS_TRACK_OFFSET_BIAS writes. Running
     * the clamp for traffic slots was corrupting their spawn pose, route-
     * table indexing, and V2V eligibility -- causing the regression in
     * memory todo_traffic_through_ground_no_motion_no_collision_2026-05-16.md
     * (traffic vehicles spawning underground, idle, no collision). */
    if (slot < g_traffic_slot_base) {
        int span_raw_i = (int)(int16_t)ACTOR_I16(actor, ACTOR_SPAN_RAW);
        int span_norm_i = (int)(int16_t)ACTOR_I16(actor, ACTOR_SPAN_NORMALIZED);

        /* Step 3-4: span_progress + cache in RS_TRACK_PROGRESS
         * [CONFIRMED @ 0x00436B80-0x00436BCC] */
        {
            int32_t pos_vec[3];
            pos_vec[0] = ACTOR_I32(actor, ACTOR_WORLD_POS_X);
            pos_vec[1] = 0;
            pos_vec[2] = ACTOR_I32(actor, ACTOR_WORLD_POS_Z);
            int64_t prog64 = td5_track_compute_span_progress(span_raw_i, pos_vec);
            rs[RS_TRACK_PROGRESS] = (int32_t)(uint32_t)(prog64 & 0xFFFFFFFFu);
        }

        /* Step 5: RenderTrackSegmentNearActor @ 0x00433CE0 writes RS[0x37]
         * and RS[0x38], neither of which is consumed elsewhere in the port.
         * Skipping it (no state effect on lateral_bias cascade).
         *
         * [L5 audit 2026-05-18, TD5_pool0 read-only]
         *
         * Despite the name, this is SIM math, NOT render. It is called from
         * UpdateRaceActors @ 0x00436A70 (callers={UpdateRaceActors}, the
         * SIM-side per-actor update loop, NOT the render pipeline). It takes
         * the per-actor RS-table pointer (gActorRouteStateTable + slot*0x47)
         * and writes ONLY two int32s at +0xdc/+0xe0 of that table — i.e.
         * RS[0x37] (dword index 0xdc/4 = 55) and RS[0x38] (224/4 = 56).
         *
         * The body (decompiled @ 0x00433CE0):
         *   switch on strip-type (1/2/5 vs 3/4 vs 6/7) computes
         *     a 2x2 linear system [iVar9*iVar6 - iVar7*iVar8] = det,
         *     RS[0x37] = (cross_num * 0x100) / det,
         *     RS[0x38] = (cross_num2 * 0x100) / det.
         *   The math is span-local barycentric coords ("actor's x/z relative
         *   to the span's vertex parallelogram", scaled <<8). Per Wave 5
         *   confidence-map notes: "computes actor's field_0xdc/0xe0 — local-
         *   space x/z relative to span". The actor->field_0xdc and 0xe0 are
         *   NOT actor-struct fields — they are RS_TABLE[+0xdc/+0xe0].
         *
         * Original-program consumer audit: zero. A program-wide instruction
         * scan for `MOV ESI,[EBX+0xdc]` style reads returns hits only inside
         * UpdateTrafficRoutePlan @ 0x00435E80, where EBX holds an ACTOR base
         * (stride 0x388), NOT the RS-table — reading actor field +0xdc which
         * is a separate, unrelated location. No instruction in the binary
         * reads gActorRouteStateTable[+0xdc] or +0xe0.
         *
         * Conclusion: byte-faithful skipping is the correct port — these RS
         * slots are dead writes in the original. Promoting to L5. If a future
         * Frida sweep ever turns up a hidden reader (e.g. unanalyzed DLL or
         * fastcall thunk), the math is trivial to bring online; the helper
         * `td5_track_compute_span_progress()` already gives us the parallel
         * primitive. Confidence map (re/analysis/ghidra_confidence_map_*.csv)
         * should be updated to L5 + UNUSED-IN-PORT tag. */

        /* Step 6: UpdateActorTrackBounds — caches min/max progress + signed
         * offsets that feed the boundary writebacks below. */
        td5_ai_update_actor_track_bounds(slot);

        /* Step 7-9: ClassifyTrackOffsetClamp + bias rewrite
         * [CONFIRMED @ 0x00436BFE-0x00436CFE] */
        {
            int classify = td5_ai_classify_track_offset_clamp_v2(
                slot, rs[RS_TRACK_OFFSET_BIAS]);
            int16_t *cardef = (int16_t *)(intptr_t)ACTOR_I32(actor, ACTOR_CAR_DEF_PTR);
            const uint8_t *table = (const uint8_t *)(intptr_t)rs[RS_ROUTE_TABLE_PTR];
            int route_byte_now = 0;
            if (table && span_norm_i >= 0) {
                route_byte_now = ai_route_byte(table, span_norm_i, 0);
            }
            if (classify == 1 && cardef) {
                int32_t off = td5_track_compute_signed_offset(
                    span_raw_i, 0x100, route_byte_now);
                rs[RS_TRACK_OFFSET_BIAS] = (int)cardef[0] + off - 0x20;
            } else if (classify == 2 && cardef) {
                int32_t off = td5_track_compute_signed_offset(
                    span_raw_i, 0, route_byte_now);
                rs[RS_TRACK_OFFSET_BIAS] = (int)cardef[4] + off + 0x20;
            }
        }

        /* Step 10: surface_type > 0x0F → zero bias
         * [CONFIRMED @ 0x00436D29-0x00436D67]
         *
         * Inline GetTrackSegmentSurfaceType @ 0x0042F100 against the actor's
         * own span_raw + sub_lane_index (NOT body_probes[0] which port's
         * td5_track_get_surface_type uses — those are bounding-box corners
         * which can lag span_raw by a frame). High nibble + 0x10 sentinel
         * → original returns >= 0x10, so the >0x0F check fires whenever the
         * sub_lane bit is set in the strip's lane_bitmask. */
        {
            TD5_StripSpan *sp = td5_track_get_span(span_raw_i);
            if (sp) {
                uint8_t sub_lane = ACTOR_U8(actor, ACTOR_SUB_LANE_INDEX);
                uint8_t lane_mask = sp->pad_02[0]; /* lane_bitmask at +0x02 */
                int surface;
                if ((lane_mask & (1u << (sub_lane & 0x1F))) != 0u) {
                    surface = (sp->surface_attribute >> 4) | 0x10;
                } else {
                    surface = sp->surface_attribute & 0x0F;
                }
                if (surface > 0x0F) {
                    rs[RS_TRACK_OFFSET_BIAS] = 0;
                }
            }
        }

        /* Step 11: track_contact_flag in {1, 2} → zero bias
         * [CONFIRMED @ 0x00436D77-0x00436D9C] */
        {
            uint8_t tcf = ACTOR_U8(actor, ACTOR_TRACK_CONTACT_FLAG);
            if (tcf == 1 || tcf == 2) {
                rs[RS_TRACK_OFFSET_BIAS] = 0;
            }
        }

        /* Step 12: boundary writebacks — selector and slot-class dependent.
         * [CONFIRMED @ 0x00436DA9-0x00436E50] */
        {
            const uint8_t *self_table =
                (const uint8_t *)(intptr_t)rs[RS_ROUTE_TABLE_PTR];
            int slot_is_racer = (slot < g_traffic_slot_base);
            int slot_is_encounter_9 =
                (slot == 9 && g_encounter_tracked_handle != -1);
            int racer_or_encounter = slot_is_racer || slot_is_encounter_9;

            if (self_table == g_route_tables[0]) {
                /* Selector == LEFT (DAT_004afb58):
                 *   racer    : RS_LEFT_BOUNDARY_A   = RS_TRACK_OFFSET_BIAS
                 *   traffic  : RS_TRACK_OFFSET_BIAS = signed_offset(LEFT)
                 *              RS_LEFT_BOUNDARY_A   = same
                 *   both     : RS_LEFT_BOUNDARY_B   = signed_offset(RIGHT)
                 *              RS_ACTIVE_UPPER      = RS_RIGHT_BOUNDARY_A
                 *              RS_ACTIVE_LOWER      = RS_RIGHT_BOUNDARY_B
                 * NB: original's `&DAT_004afbb4` / `&DAT_004afbb0` writes are
                 *     RS[0x15] / RS[0x14] in port-index land:
                 *     0x4afbb4 - 0x4afb60 = 0x54 → dword 0x15 = ACTIVE_UPPER_BOUND
                 *     0x4afbb0 - 0x4afb60 = 0x50 → dword 0x14 = ACTIVE_LOWER_BOUND */
                if (racer_or_encounter) {
                    rs[RS_LEFT_BOUNDARY_A] = rs[RS_TRACK_OFFSET_BIAS];
                } else if (g_route_tables[0] && span_norm_i >= 0) {
                    int route_byte_left =
                        ai_route_byte(g_route_tables[0], span_norm_i, 0);
                    int32_t offL = td5_track_compute_signed_offset(
                        span_raw_i, rs[RS_TRACK_PROGRESS], route_byte_left);
                    rs[RS_TRACK_OFFSET_BIAS] = offL;
                    rs[RS_LEFT_BOUNDARY_A]   = offL;
                }
                if (g_route_tables[1] && span_norm_i >= 0) {
                    int route_byte_right =
                        ai_route_byte(g_route_tables[1], span_norm_i, 0);
                    rs[RS_LEFT_BOUNDARY_B] = td5_track_compute_signed_offset(
                        span_raw_i, rs[RS_TRACK_PROGRESS], route_byte_right);
                }
                rs[RS_ACTIVE_UPPER_BOUND] = rs[RS_RIGHT_BOUNDARY_A];
                rs[RS_ACTIVE_LOWER_BOUND] = rs[RS_RIGHT_BOUNDARY_B];
            } else {
                /* Selector == RIGHT (DAT_004b08b4):
                 *   racer    : RS_LEFT_BOUNDARY_B   = RS_TRACK_OFFSET_BIAS
                 *              [CONFIRMED @ 0x00436DC4: DAT_004afb9c write]
                 *   traffic  : RS_TRACK_OFFSET_BIAS = signed_offset(RIGHT)
                 *              RS_LEFT_BOUNDARY_B   = same
                 *   both     : RS_LEFT_BOUNDARY_A   = signed_offset(LEFT)
                 *              RS_ACTIVE_UPPER      = RS_RIGHT_EXTENT_A
                 *              RS_ACTIVE_LOWER      = RS_RIGHT_EXTENT_B */
                if (racer_or_encounter) {
                    rs[RS_LEFT_BOUNDARY_B] = rs[RS_TRACK_OFFSET_BIAS];
                } else if (g_route_tables[1] && span_norm_i >= 0) {
                    int route_byte_right =
                        ai_route_byte(g_route_tables[1], span_norm_i, 0);
                    int32_t offR = td5_track_compute_signed_offset(
                        span_raw_i, rs[RS_TRACK_PROGRESS], route_byte_right);
                    rs[RS_TRACK_OFFSET_BIAS] = offR;
                    rs[RS_LEFT_BOUNDARY_B]   = offR;
                }
                if (g_route_tables[0] && span_norm_i >= 0) {
                    int route_byte_left =
                        ai_route_byte(g_route_tables[0], span_norm_i, 0);
                    rs[RS_LEFT_BOUNDARY_A] = td5_track_compute_signed_offset(
                        span_raw_i, rs[RS_TRACK_PROGRESS], route_byte_left);
                }
                rs[RS_ACTIVE_UPPER_BOUND] = rs[RS_RIGHT_EXTENT_A];
                rs[RS_ACTIVE_LOWER_BOUND] = rs[RS_RIGHT_EXTENT_B];
            }
        }
    }
}

void td5_ai_tick(void) {
    td5_ai_compute_rubber_band();
    /* [dynamic-traffic] fades, despawn checks and spawn cadence — once per
     * sim tick, before the per-actor loop (no-op when Dynamic=0). NOTE: this
     * lives here, NOT in td5_ai_pre_tick — pre_tick has no callers in the
     * port's game loop; td5_ai_tick is the live per-tick AI entry. */
    td5_ai_traffic_dynamic_tick();
    if ((g_ai_frame_counter % 60u) == 0u) {
        int racer_count = g_traffic_slot_base;
        if (g_active_actor_count < racer_count)
            racer_count = g_active_actor_count;
        for (int i = 0; i < racer_count; i++) {
            if (g_slot_state[i] == 0) {
                TD5_LOG_D(LOG_TAG,
                          "Rubber band: slot=%d throttle=%d steer_bias=%d",
                          i, g_live_throttle[i], g_actor_route_steer_bias[i]);
            }
        }
    }
    td5_ai_update_race_actors();
    g_ai_frame_counter++;
}

/* ========================================================================
 * Rubber-Band System  (0x432D60  ComputeAIRubberBandThrottle)
 *
 * Per-tick: copies default throttle -> live throttle, then for each AI
 * slot computes: modifier = (scale * delta) / range
 *                live_bias[slot] = 0x100 - modifier
 *
 * When AI is behind player: delta < 0, range > 0 ->
 *   modifier = (scale * negative) / positive = negative
 *   bias = 0x100 - negative > 0x100 (catch-up boost)
 * When AI is ahead: delta > 0, range > 0 ->
 *   modifier = (scale * positive) / positive = positive
 *   bias = 0x100 - positive < 0x100 (slow down)
 *
 * NOTE: the denominator is always the POSITIVE range value for both branches.
 * An earlier port error used (-g_rb_behind_range) in the behind branch, which
 * made the modifier positive and throttled down AI that should be boosted.
 * ======================================================================== */

/* ========================================================================
 * Pool11 pilot trace integration (0x00432D60).
 *
 * The pilot snapshot mirrors the function inputs read by Frida from the
 * original. Collector lives here so it can read the static slot-state
 * array + the public actor pointer. */


/* ========================================================================
 * CATCHUP / rubber-band assist controls (S06 2026-06-04)
 *
 * Restores the CATCHUP setting removed in the 2026-06-04 input revamp and
 * exposes it as a tunable gate over the AI rubber-band:
 *   - Effective level resolved from the td5re.ini override (>=0 wins) else the
 *     persisted value (S05 Multiplayer Options toggle, default 1 = on).
 *   - level 0  -> catchup OFF: rubber-band modifier forced to 0, so each AI runs
 *                 on its plain difficulty-tier throttle (no player-distance
 *                 boost/cut). bias = 0x100 (neutral), same as the network branch.
 *   - level >0 -> catchup ON, but the modifier is softened by
 *                 K_CATCHUP_STRENGTH_PCT/256 so it assists without yo-yoing the
 *                 player (the user-reported "too aggressive" symptom).
 * ======================================================================== */

/* Softening applied to the rubber-band modifier when catchup is ON. 256 = full
 * original strength; 176 (~0.69) tames the over-correction / yo-yo. */
#define K_CATCHUP_STRENGTH_PCT 176

int td5_ai_get_catchup_level(void) {
    int ini = g_td5.ini.catchup_assist;     /* -1 = use persisted; 0..9 override */
    if (ini >= 0) return ini;
    return td5_save_get_catchup_assist();    /* persisted; default 1 */
}

void td5_ai_compute_rubber_band(void) {
    int i, racer_count;
    int32_t player0_span, ai_span, delta, modifier;
    int catchup = td5_ai_get_catchup_level();   /* 0 = off; >0 = on (softened) */


    /* [0x00432D6B-7A] MOVSD.REP ECX=14: unconditional default→live copy */
    memcpy(g_live_throttle, g_default_throttle, sizeof(g_live_throttle));

    /* [0x00432D60-7C] TEST g_networkRaceActive; JZ → rubber-band loop.
     * Fall-through here = network ACTIVE → write 0x100 bias for AI slots. */
    if (g_td5.network_active) {
        /* [0x00432D7E-DC2] network-active branch.
         * Original re-reads g_racerCount each iter (CMP ESI,0x6 with ESI cached);
         * effective cap = min(g_racerCount, 6). */
        racer_count = g_traffic_slot_base;
        if (g_active_actor_count < racer_count)
            racer_count = g_active_actor_count;
        for (i = 0; i < racer_count; i++) {
            /* [0x00432D9F-A8] state byte at gRaceSlotStateTable.slot[i].state */
            if (g_slot_state[i] == 0) { /* AI slot */
                /* [0x00432DAA] DAT_00473d2c[i] = 0x100 (live throttle override) */
                g_live_throttle[i] = 0x100;
                /* [0x00432DB5] gActorDefaultRouteSteerBias[i*0x47] = 0x100 */
                g_actor_route_steer_bias[i] = 0x100;
            }
            /* [0x00432DBC] ADD EDX,0x11C unconditional (handled by [i] indexing) */
        }
        return;
    }

    /* [0x00432DC4-E4B] rubber-band branch.
     *
     * Per pilot_00432D60_audit.md: original reads actor+0x84 (span_accum) as
     * signed 16-bit, computes delta = ai - player0. Behind branch has a DEAD
     * upper-clamp at +behind_range (never fires for negative delta); there is
     * NO lower-clamp at -behind_range — earlier port over-approximated this.
     * Ahead branch has an ACTIVE upper-clamp at +ahead_range.
     *
     * IDIV truncates toward zero (signed); C's `/` operator since C99 matches. */

    /* [0x00432DF1] MOVSX EAX, word ptr [0x4ab18c] — actor[0].+0x84 read once
     * (constant address in original; player0 span_accum). */
    player0_span = (int16_t)ACTOR_I16(actor_ptr(0), ACTOR_SPAN_ACCUM);

    /* [0x00432DD5-E4B] g_racerCount re-read at loop top each iteration in
     * original. We cache here — the function is single-threaded with no
     * callees, so g_racerCount cannot change within this call. Equivalent. */
    racer_count = g_traffic_slot_base;
    if (g_active_actor_count < racer_count)
        racer_count = g_active_actor_count;

    for (i = 0; i < racer_count; i++) {
        /* [0x00432DE8-EC] CMP byte [EBP],0x0; JNZ skip — non-AI slots leave
         * gActorDefaultRouteSteerBias[i] untouched. */
        if (g_slot_state[i] != 0)
            continue;

        /* [0x00432DEE] MOVSX ECX, word ptr [ESI] — actor[i].+0x84 */
        ai_span = (int16_t)ACTOR_I16(actor_ptr(i), ACTOR_SPAN_ACCUM);
        /* [0x00432DF8] SUB ECX,EAX — signed 32-bit delta */
        delta = ai_span - player0_span;
        int32_t delta0 = delta;   /* SmartAI leash needs the unclamped delta */

        /* [0x00432DFA] JNS — sign of delta selects branch */
        if (delta < 0) {
            /* [0x00432DFC-16] AI BEHIND player: catch-up boost.
             *   MOV  EAX, [0x473da4]    ; behind_range (positive)
             *   CMP  ECX, EAX           ; delta vs +range
             *   JLE  skip_clamp         ; always true (delta<0 < range>0)
             *   MOV  ECX, EAX           ; DEAD — never executes
             * skip_clamp:
             *   MOV  EAX, [0x473d9c]    ; behind_scale
             *   IMUL EAX, ECX
             *   CDQ
             *   IDIV [0x473da4]         ; / behind_range, trunc-to-zero
             * Faithful port: keep the dead upper-clamp for byte parity with
             * the listing's branch shape; do NOT add any lower-clamp. */
            if (delta > g_rb_behind_range)
                delta = g_rb_behind_range;  /* dead path; mirrors 0x00432E01-05 */
            modifier = (g_rb_behind_scale * delta) / g_rb_behind_range;
        } else {
            /* [0x00432E18-32] AI AHEAD or tied: throttle reduction.
             *   MOV  EAX, [0x473da8]    ; ahead_range
             *   CMP  ECX, EAX
             *   JLE  skip_clamp
             *   MOV  ECX, EAX           ; ACTIVE — clamp delta to +ahead_range
             *   MOV  EAX, [0x473da0]    ; ahead_scale
             *   IMUL EAX, ECX
             *   CDQ
             *   IDIV [0x473da8] */
            if (delta > g_rb_ahead_range)
                delta = g_rb_ahead_range;
            modifier = (g_rb_ahead_scale * delta) / g_rb_ahead_range;
        }

        /* CATCHUP gate/soften (S06 2026-06-04). OFF -> modifier 0 so the AI runs
         * on its plain difficulty-tier throttle (bias = 0x100, neutral). ON ->
         * scale the swing down so catchup assists without yo-yoing the player.
         * `/256` truncates toward zero, keeping the behind(neg)/ahead(pos)
         * branches symmetric. */
        if (catchup <= 0) {
            modifier = 0;
        } else {
            modifier = (modifier * K_CATCHUP_STRENGTH_PCT) / 256;
        }

        /* SmartAI: replace the faithful (asymmetric, uncapped-behind) rubber
         * band with a gentle SYMMETRIC leash — behind→small boost, ahead→small
         * trim, capped to a narrow band so the catch-up never feels rigged.
         * Independent of CatchupAssist; off when SmartAILeash=0 (pure skill). */
        if (td5_ai_smart_active()) {
            int32_t smod;
            if (td5_ai_smart_leash_modifier(i, delta0, &smod))
                modifier = smod;
        }

        /* [0x00432E32-39] MOV ECX,0x100; SUB ECX,EAX; MOV [EDI],ECX */
        g_actor_route_steer_bias[i] = 0x100 - modifier;
    }
}

/* ========================================================================
 * Race Actor Runtime Initialization  (0x432E60)
 *
 * Configures rubber-band parameters via a decision tree over:
 *   difficulty (easy/normal/hard)
 *   circuit vs point-to-point
 *   traffic enabled
 *   difficulty tier (0/1/2)
 *   game mode (pitbull, time-trial, etc.)
 *
 * Also applies difficulty scaling to the AI physics template and sets
 * the active actor count (6/12/2).
 * ======================================================================== */

/* Forward decl — defined with the S20 smart-traffic helpers below. */
static void td5_traffic_smart_reset(void);

void td5_ai_init_race_actor_runtime(void) {
    /* [S20 smart-traffic] reset per-actor smart-traffic state once per race
     * (route assignments, PRNG seeds, lane biases). Cheap; harmless when the
     * feature is disabled. */
    td5_traffic_smart_reset();
    /* [SmartAI overhaul] derive per-car skill + per-car branch tie-breaks once
     * per race (cheap; only consumed when SmartAI is on). Must run after
     * g_td5.difficulty_tier is set for this race. */
    if (g_td5.ini.smart_ai) td5_ai_smart_race_init();
    /* [CONFIRMED @ InitializeRaceActorRuntime 0x00432E60 reads gRaceDifficultyTier
     * @ 0x00463210 throughout the Layer-2 decision tree.] The port previously
     * read a file-static `g_race_difficulty_tier` that was never written —
     * route through g_td5.difficulty_tier (which ConfigureGameTypeFlags writes
     * at td5_frontend.c:2675 mirroring the original's switch at 0x00410CA0). */
    int tier = g_td5.difficulty_tier;
    int is_circuit = (g_td5.track_type == TD5_TRACK_CIRCUIT);
    int has_traffic = g_td5.traffic_enabled;
    /* [@ 0x00432FF0] Cop Chase ("wanted mode") branch — original gates on
     *   [0x00466e94] gTrackIsCircuit==0 && [0x004aaf74] g_raceOverlayPresetMode==4.
     * g_raceOverlayPresetMode=4 is set ONLY by ConfigureGameTypeFlags case 8
     * (Cop Chase). The port's previous `is_pitbull = race_rule_variant==4`
     * was misnamed AND wrong: race_rule_variant=4 is set by case 6 (Ultimate),
     * not Cop Chase. */
    int is_cop_chase = g_td5.wanted_mode_enabled;
    int is_time_trial = g_td5.time_trial_enabled;
    int racer_count;

    if (!g_route_state_base) {
        g_route_state_base = g_route_state_storage;
    }

    /* --- Active actor count --- */
    if (is_time_trial) {
        g_active_actor_count = (g_td5.split_screen_mode > 0) ? 2 : 1;
    } else if (g_traffic_slot_base > TD5_LEGACY_RACE_SLOTS) {
        /* [PORT ENHANCEMENT] big split-screen field. Traffic (if enabled) spawns
         * at g_traffic_slot_base..+TD5_MAX_TRAFFIC_SLOTS, so the active-actor
         * count must reach those slots; otherwise it's just the racers. */
        if (has_traffic) {
            g_active_actor_count = g_traffic_slot_base + TD5_MAX_TRAFFIC_SLOTS;
        } else {
            int total = g_td5.num_human_players + g_td5.num_ai_opponents;
            if (total < 1) total = 1;
            if (total > TD5_MAX_RACER_SLOTS) total = TD5_MAX_RACER_SLOTS;
            g_active_actor_count = total;
        }
    } else if (has_traffic) {
        g_active_actor_count = TD5_LEGACY_RACE_SLOTS + TD5_MAX_TRAFFIC_SLOTS;  /* 12 */
    } else {
        g_active_actor_count = TD5_LEGACY_RACE_SLOTS;  /* 6 */
    }
    g_td5.total_actor_count = g_active_actor_count;
    racer_count = is_time_trial
                  ? ((g_td5.split_screen_mode > 0) ? 2 : 1)
                  : TD5_MAX_RACER_SLOTS;

    memset(g_route_state_storage, 0, sizeof(g_route_state_storage));
    memset(g_actor_forward_track_component, 0, sizeof(g_actor_forward_track_component));
    memset(g_actor_route_steer_bias, 0, sizeof(g_actor_route_steer_bias));
    memset(g_slot_state, 0, sizeof(g_slot_state));

    for (int i = 0; i < TD5_MAX_TOTAL_ACTORS; ++i) {
        int32_t *rs = route_state(i);
        int selector = 0;

        rs[RS_SLOT_INDEX] = i;
        rs[RS_ENCOUNTER_HANDLE] = -1;
        rs[RS_DEFAULT_THROTTLE] = g_default_throttle[i];
        /* [CONFIRMED @ InitializeRaceActorRuntime 0x00433526]: the original
         * seeds gActorDefaultRouteSteerBias[i] = live_throttle[i] for ALL 12
         * actors (MOV [EAX+0x10], live[i]; loop covers the full actor pool),
         * NOT just racers 0-5. UpdateActorRouteThresholdState reads this bias
         * (port mirror g_actor_route_steer_bias[]) and writes it to the actor's
         * throttle word (+0x33E) in its fallback branch. The per-tick rubber-band
         * recompute (@0x00432D60) overwrites only slots 0-5, so the seed for the
         * encounter-cop slot 9 (= g_default_throttle[9] = 0x2bc) must be planted
         * here or the hijacked cop has zero throttle. The port previously left
         * g_actor_route_steer_bias[] memset to 0 and only the rubber-band (0-5)
         * populated it, starving slot 9. [FIX 2026-06-01 cops-traffic] */
        g_actor_route_steer_bias[i] = g_default_throttle[i];
        if (g_default_throttle[i] != 0 && i >= g_traffic_slot_base) {
            TD5_LOG_I(LOG_TAG,
                      "init_actor_runtime: encounter-cop slot=%d seeded "
                      "route_steer_bias=live_throttle=%d (0x%X)",
                      i, g_default_throttle[i], g_default_throttle[i]);
        }
        rs[RS_RECOVERY_STAGE] = 0;
        rs[RS_ROUTE_DIRECTION_POLARITY] = 0;

        /* [CONFIRMED via InitializeRaceActorRuntime @ 0x00432e60 decomp]
         * Route-table selection is by slot parity, not sub_lane:
         *   (slot & 1) == 1 (odd)  → LEFT.TRK  (selector 0, canonical)
         *   (slot & 1) == 0 (even) → RIGHT.TRK (selector 1, junction remap active)
         *
         * 2026-05-18 BlueRidge regression follow-up — track-aware override:
         *
         * Orig's UpdateRaceActors @ 0x00436A70 runs PATH 2b (selector=0, ptr
         * UNCHANGED) when span_norm < junction main_target. The Moscow agent's
         * 2412baf port correctly mirrors this — even slots keep init RIGHT in
         * PATH 2b. This matches orig on Moscow / Edinburgh / Sydney / Jarash /
         * Honolulu where even slots remain RIGHT at sub_tick=0 capture.
         *
         * BUT snapshot evidence on BlueRidge shows orig has ALL slots reading
         * the SAME route_table_ptr (LEFT) at sub_tick=0 — meaning orig writes
         * LEFT to even slots somewhere between init and sub_tick=0 capture via
         * a path not visible in the static disassembly audited (UpdateRaceActors
         * + InitializeRaceActorRuntime + InitializeTrafficActorsFromQueue all
         * checked — only PATH 1 and PATH 2a write the ptr field). Likely an
         * earlier UpdateRaceActors call with different span_norm, or another
         * init function we haven't traced.
         *
         * Empirical discriminator: smallest junction main_target falls in the
         * window [200, 400):
         *   - BlueRidge:  smallest mt=301 → IN window → force LEFT
         *   - Honolulu:   smallest mt=156 → BELOW → keep parity (orig=RIGHT)
         *   - Moscow:     smallest mt=500 → ABOVE → keep parity (orig=RIGHT)
         *   - Edinburgh:  smallest mt=741 → ABOVE → keep parity
         *   - Sydney:     smallest mt=510 → ABOVE → keep parity
         *   - Jarash:     no junctions   → keep parity
         *
         * Honolulu's low mt=156 means PATH 2a *could* fire there for spans near
         * 156, but slot 0 spawns at ~102 — so orig keeps even=RIGHT (PATH 2b).
         * Moscow's mt=500 means PATH 2a never fires for typical spawn spans.
         * BlueRidge's mt=301 is the sweet spot where orig somehow ends up at
         * LEFT for all slots — mechanism unclear, empirically validated.
         *
         * Sweep results pre-fix → post-fix (sub_tick=1 filtered):
         *   blueridge_ai_viper:   33 → 18 (-15)  ✓ matches pre-2412baf baseline
         *   blueridge_ai_viper sub3: 17 → 5 (-12) bonus
         *   honolulu_ai_viper:    49 → 49 (no change)
         *   honolulu_hum_viper:   37 → 37 (no change)
         *   actual_moscow_ai:     20 → 20 (no change — Moscow agent's -14 held)
         *   edinburgh_ai_viper:   61 → 61 (no change)
         *   sydney_ai_viper:      28 → 28 (no change)
         *   jarash_ai_viper:      40 → 40 (no change) */
        /* Data-swap: for SoloAISlot=N, give slot 0 the parity-based
         * selector that slot N would have. Lets us A/B test the AI on
         * either route-table (LEFT vs RIGHT) within solo mode without
         * touching the camera/render pipeline. Only applies to slot 0
         * itself (the camera target); other slots keep natural parity. */
        int effective_parity = i;
        if (g_td5.solo_mode_synth && i == 0 && g_td5.split_screen_mode == 0) {
            int N = g_td5.ini.solo_ai_slot;
            if (N >= 0 && N < TD5_MAX_RACER_SLOTS) effective_parity = N;
        }
        selector = (effective_parity & 1) ? 0 : 1;
        if (selector == 1) {
            int junction_count = td5_track_get_junction_count();
            const uint8_t *jt   = td5_track_get_junction_entries();
            if (jt && junction_count > 0) {
                int smallest_mt = INT_MAX;
                for (int j = 0; j < junction_count; j++) {
                    int mt = (int)(*(const uint16_t *)(jt + j * 6 + 4));
                    if (mt > 0 && mt < smallest_mt) smallest_mt = mt;
                }
                if (smallest_mt >= 200 && smallest_mt < 400) {
                    selector = 0;
                }
            }
        }
        rs[RS_ROUTE_TABLE_SELECTOR] = selector;
        rs[RS_ROUTE_TABLE_PTR] = (int32_t)(intptr_t)g_route_tables[selector];
        g_last_logged_opcode[i] = -1;
    }

    /* Player-as-AI: leave slot 0 at 0 so td5_ai_update_race_actors drives
     * it and td5_ai_compute_rubber_band treats it as an AI racer. Mirrors
     * the original attract-mode slot-state write at 0x0042ACCF. */
    if (!g_td5.ini.player_is_ai) {
        g_slot_state[0] = 1;
    } else {
        TD5_LOG_I(LOG_TAG,
                  "player_is_ai=1 -> AI slot_state[0]=0 (autopilot active)");
    }
    if (g_td5.split_screen_mode > 0 && racer_count > 1 && !g_td5.ini.player_is_ai) {
        /* [PORT ENHANCEMENT] mark slots 1..num_human_players-1 as human (N-way).
         * Skipped when player_is_ai so every local slot stays AI autopilot.
         * Under others_ai, slots 1..N-1 are AI (0) instead so only slot 0 is
         * human — the AI's g_slot_state MUST agree with td5_game.c's
         * s_slot_state or the AI skips those slots (player state) AND no input
         * drives them, leaving them parked. */
        int humans = g_td5.num_human_players;
        int human_state = g_td5.ini.others_ai ? 0 : 1;
        if (humans > TD5_MAX_VIEWPORTS) humans = TD5_MAX_VIEWPORTS;
        for (int k = 1; k < humans && k < g_traffic_slot_base; k++)
            g_slot_state[k] = human_state;
        if (g_td5.ini.others_ai)
            TD5_LOG_I(LOG_TAG,
                      "others_ai=1 -> AI g_slot_state[1..%d]=0 (slot 0 = human)",
                      (humans < g_traffic_slot_base ? humans : g_traffic_slot_base) - 1);
    }
    /* [MP AI TEST PLAYERS 2026-06-25] Simulated AI players occupy human-pool slots
     * but are AI-driven. This g_slot_state[] table is SEPARATE from td5_game.c's
     * s_slot_state[] (see the split-screen + network notes), so the InitRace
     * mp_ai_player_mask flip does NOT reach here — without this the masked slots
     * stay g_slot_state==1 (human) and ai_update_single_racer skips them, leaving
     * the cars parked. Mirror the InitRace per-slot flip so the AI drives them. */
    if (g_td5.mp_ai_player_mask) {
        int humans = g_td5.num_human_players;
        if (humans > g_traffic_slot_base) humans = g_traffic_slot_base;
        for (int k = 1; k < humans; k++)
            if (((g_td5.mp_ai_player_mask >> k) & 1u) && g_slot_state[k] == 1)
                g_slot_state[k] = 0;   /* AI test-player drives itself */
        TD5_LOG_I(LOG_TAG,
                  "mp_ai_player_mask=0x%X -> AI g_slot_state simulated slots = 0",
                  g_td5.mp_ai_player_mask);
    }
    /* [ITEM 2 2026-06-19] Network race: the AI's g_slot_state MUST agree with
     * td5_game.c's s_slot_state (see the split-screen note above) or the AI
     * drives a human's car. Mark every roster-occupied slot human -- the roster
     * is replicated identically on every peer, so this is deterministic. Without
     * it, a joining client's own slot stayed g_slot_state=0 and the AI overwrote
     * its controls every tick ("player is AI" on the client). s_slot_state alone
     * (td5_game.c) was NOT enough -- the AI reads THIS array. */
    if (g_td5.network_active) {
        extern int td5_net_is_slot_active(int slot);
        int k;
        for (k = 0; k < g_traffic_slot_base; k++)
            if (td5_net_is_slot_active(k))
                g_slot_state[k] = 1;
    }
    /* Wanted mode (cop chase): slots 2-5 are inactive (no AI, no physics dispatch).
     * Mirrors gRaceSlotStateTable init at 0x42ABF8 for non-zero game types. */
    if (g_td5.wanted_mode_enabled) {
        /* MP cop chase keeps the human suspects + the AI-cop slot (the cop sits at
         * slot num_human_players, AI-driven via g_slot_state==0); SP wanted keeps
         * the faithful slots 0-1. MUST match td5_game.c's s_slot_state field. */
        int keep = td5_game_mp_cop_chase_field();
        if (keep <= 0) keep = 2;
        for (int k = keep; k < g_traffic_slot_base; k++)
            g_slot_state[k] = 3;
        TD5_LOG_I(LOG_TAG, "wanted_mode: g_slot_state[%d..] = 3 (inactive, field=%d)", keep, keep);
    }
    /* Drag race: slots 2..5 are decoration (no AI dispatch). Mirrors the
     * same override in td5_game.c:849-857 so the two parallel slot-state
     * tables stay consistent. Without this, the synthetic drag driver in
     * ai_update_single_racer case 0x00 fires for slots 2..5 too — and
     * those slots have no spawned actor, so the drive command lands on
     * uninitialized memory. */
    if (g_td5.drag_race_enabled) {
        for (int k = 2; k < g_traffic_slot_base; k++)
            g_slot_state[k] = 3;
        TD5_LOG_I(LOG_TAG, "drag_race: g_slot_state[2..5] = 3 (inactive)");
    }

    /* Solo mode synth (Time Trial mapped to gt=0 — see ConfigureGameTypeFlags
     * case 7): slots 1..5 inactive. Mirrors td5_game.c:1215-1222. */
    if (g_td5.solo_mode_synth && g_td5.split_screen_mode == 0) {
        for (int k = 1; k < g_traffic_slot_base; k++)
            g_slot_state[k] = 3;
        TD5_LOG_I(LOG_TAG, "solo_mode_synth: g_slot_state[1..5] = 3 (inactive)");
    }

    /* Single race / Quick Race reduced field [PORT ENHANCEMENT]: disable racer
     * slots beyond the configured total (humans + AI opponents) so the AI does
     * NOT drive the dropped opponents. MUST mirror the same override in
     * td5_game.c InitRace — the AI's g_slot_state[] is a SEPARATE table from
     * the game's s_slot_state[], and without this the dropped slots stay
     * g_slot_state==0 (AI) and get driven (they were seen spawning + circling).
     * Guarded on num_human_players>=1 so an un-configured launch keeps the grid. */
    if (!is_time_trial && !g_td5.drag_race_enabled && !g_td5.wanted_mode_enabled &&
        !(g_td5.solo_mode_synth && g_td5.split_screen_mode == 0) &&
        g_td5.num_human_players >= 1) {
        int total = g_td5.num_human_players + g_td5.num_ai_opponents;
        if (total < 1) total = 1;
        if (total > TD5_MAX_RACER_SLOTS) total = TD5_MAX_RACER_SLOTS;
        for (int k = total; k < g_traffic_slot_base; k++)
            g_slot_state[k] = 3;
        TD5_LOG_I(LOG_TAG, "single-race: g_slot_state[%d..5] = 3 (humans=%d opponents=%d)",
                  total, g_td5.num_human_players, g_td5.num_ai_opponents);
    }

    /* --- First layer: DYNAMICS (arcade/sim) scaling on AI physics template ---
     * Mirrors InitializeRaceActorRuntime [@ 0x00432F2F..0x00432FEC]. The original
     * gates this block on the dynamics globals gDifficultyHard @0x004AAF80 /
     * gDifficultyEasy @0x004AAF84 — NOT on the user difficulty selector
     * (gRaceDifficultyTier @0x00463210 is only read AFTER it, @0x00432FFD).
     * gDifficultyEasy = gDynamicsConfigShadow (the ARCADE/SIMULATION toggle);
     * gDifficultyHard has NO writers, so its HARD 4-field branch never executes
     * and is omitted here (matching runtime). Live three-way collapses to:
     *   ARCADE     (Easy==0): steer *= 0x168/256, grip *= 0x12C/256  [@0x432FBC..0x432FEC]
     *   SIMULATION (Easy!=0): no scaling                             [@0x432FBA skip]
     *
     * Previously keyed on g_td5.difficulty — a documented deviation (see the
     * note at ConfigureGameTypeFlags, td5_frontend.c). Now keyed on the dynamics
     * flag via td5_physics_get_dynamics() (0=ARCADE, 1=SIMULATION == gDifficultyEasy),
     * matching the player path (td5_physics_init_vehicle_runtime, 0x42F140).
     *
     * Template field offsets:  +0x2C grip (short), +0x68 steer (short).
     */
    {
        int16_t *steer = (int16_t *)(g_ai_physics_template + 0x68);
        int16_t *grip  = (int16_t *)(g_ai_physics_template + 0x2C);

        /* [ARCADE/SIM CONSOLIDATION 2026-06-26] Apply the arcade template
         * scaling (steer*0x168/256, grip*300/256) in BOTH dynamics modes so AI
         * cars get the same grip/steer values the player cars now get in both
         * modes (player path: td5_physics_init_vehicle_runtime). Previously this
         * was gated `get_dynamics()==0` (arcade only); SIMULATION skipped it,
         * leaving AI on raw template values. The user's consolidation makes
         * SIMULATION use arcade stats too, so the AI must match. */
        {
            /* [@ 0x00432FBC..0x00432FEC]: steer*0x168/256, grip*300/256. */
            *steer = (int16_t)(((int32_t)*steer * 0x168) >> 8);
            *grip  = (int16_t)(((int32_t)*grip  * 0x12C) >> 8); /* 0x12C = 300 */
        }
    }

    /* --- Second layer: mode/circuit/traffic/tier decision tree ---
     *
     * Mirrors [@ 0x00432FF0..0x004334E1] verbatim.
     *
     * Every leaf writes: steer (+0x68), grip (+0x2C), brake (+0x6E)=1000,
     *                    lspd_brake (+0x70)=1000, top_speed (+0x74)=branch_const
     * plus the rubber-band tuples: rb_behind_scale, rb_behind_range,
     *                              rb_ahead_scale, rb_ahead_range.
     */
    {
        int16_t *steer = (int16_t *)(g_ai_physics_template + 0x68);
        int16_t *grp   = (int16_t *)(g_ai_physics_template + 0x2C);
        int16_t *brake = (int16_t *)(g_ai_physics_template + 0x6E);
        int16_t *lspdb = (int16_t *)(g_ai_physics_template + 0x70);
        int16_t *spd   = (int16_t *)(g_ai_physics_template + 0x74);

        if (is_time_trial) {
            /* [@ 0x00432F01-1F] Time-Trial path — skip all template scaling,
             * write rb_*=0/0x40/0/0x40 only. No brake/spd writes. */
            g_rb_behind_scale = 0;
            g_rb_behind_range = 0x40;
            g_rb_ahead_scale  = 0;
            g_rb_ahead_range  = 0x40;
        } else if (!is_circuit) {
            /* [@ 0x00432FF7 JZ 0x00433178] !circuit branch */
            if (is_cop_chase) {
                /* [@ 0x00433181..0x004331F0] g_raceOverlayPresetMode == 4 (Cop Chase) */
                *steer = (int16_t)(((int32_t)*steer * 0x91) >> 8);   /* [@ 0x00433185-99] */
                *brake = 1000;                                       /* [@ 0x004331B3] */
                *lspdb = 1000;                                       /* [@ 0x004331B7] */
                *spd   = 0x3A1;                                      /* [@ 0x004331BB] */
                *grp   = (int16_t)(((int32_t)*grp   * 0xB9) >> 8);   /* [@ 0x004331A1-C4] */
                g_rb_behind_scale = 0x8C;                            /* [@ 0x004331C8] */
                g_rb_behind_range = 0x64;                            /* [@ 0x004331D2] */
                g_rb_ahead_scale  = 0xC0;                            /* [@ 0x004331DC] */
                g_rb_ahead_range  = 0x40;                            /* [@ 0x004331E6] */
            } else if (!has_traffic) {
                /* [@ 0x004331F5-FA reads gTrafficActorsEnabled; 0x004331FC
                 * loads gRaceDifficultyTier; 0x00433201 JNZ → traffic-on path.
                 * Fall through: P2P no-traffic tier dispatch. */
                switch (tier) {
                case 0:
                    /* [@ 0x00433300..0x00433370 tier-0] */
                    *steer = (int16_t)(((int32_t)*steer * 0xAA) >> 8);
                    *brake = 1000;
                    *lspdb = 1000;
                    *spd   = 0x3A1;
                    *grp   = (int16_t)(((int32_t)*grp * 0x100) >> 8); /* [@ 0x00433324] */
                    g_rb_behind_scale = 0xA0;                          /* [@ 0x00433345] */
                    g_rb_behind_range = 0x64;                          /* [@ 0x0043334F] */
                    g_rb_ahead_scale  = 0x96;                          /* [@ 0x00433359] */
                    g_rb_ahead_range  = 0x50;                          /* [@ 0x00433363] */
                    break;
                case 1:
                    /* [@ 0x00433299..0x004332FB] */
                    *steer = (int16_t)(((int32_t)*steer * 0xB4) >> 8);
                    *brake = 1000;
                    *lspdb = 1000;
                    *spd   = 0x3A1;                                   /* [@ 0x004332D2] */
                    *grp   = (int16_t)(((int32_t)*grp * 0x100) >> 8); /* [@ 0x004332B7] */
                    g_rb_behind_scale = 0xC8;                          /* [@ 0x004332DD] */
                    g_rb_behind_range = 0x4B;                          /* [@ 0x004332E7] */
                    g_rb_ahead_scale  = 0xC0;                          /* [@ 0x004332EC] */
                    g_rb_ahead_range  = 0x4B;                          /* [@ 0x004332F6] */
                    break;
                default: /* tier 2 */
                    /* [@ 0x0043321E..0x00433294] */
                    *steer = (int16_t)(((int32_t)*steer * 0xDC) >> 8);
                    *brake = 1000;
                    *lspdb = 1000;
                    *spd   = 0x433;                                   /* [@ 0x0043325F] */
                    *grp   = (int16_t)(((int32_t)*grp * 0x10E) >> 8);
                    g_rb_behind_scale = 0x10E;                         /* [@ 0x0043326C] */
                    g_rb_behind_range = 0x41;                          /* [@ 0x00433276] */
                    g_rb_ahead_scale  = 0x96;                          /* [@ 0x00433280] */
                    g_rb_ahead_range  = 0x50;                          /* [@ 0x0043328A] */
                    break;
                }
            } else {
                /* [@ 0x00433372..0x004334DF] P2P WITH traffic */
                switch (tier) {
                case 0:
                    /* [@ 0x00433478..0x004334DF] */
                    *steer = (int16_t)(((int32_t)*steer * 0xB4) >> 8);
                    *brake = 1000;
                    *lspdb = 1000;
                    *spd   = 0x3A1;
                    *grp   = (int16_t)(((int32_t)*grp * 0x100) >> 8);
                    g_rb_behind_scale = 0xB4;
                    g_rb_behind_range = 0x4B;
                    g_rb_ahead_scale  = 0xBE;
                    g_rb_ahead_range  = 0x64;
                    break;
                case 1:
                    /* [@ 0x00433402..0x00433476] note unique top_spd=0x3B9 */
                    *steer = (int16_t)(((int32_t)*steer * 0xBE) >> 8);
                    *brake = 1000;
                    *lspdb = 1000;
                    *spd   = 0x3B9;                                   /* [@ 0x00433441] */
                    *grp   = (int16_t)(((int32_t)*grp * 0x10E) >> 8);
                    g_rb_behind_scale = 0xC8;
                    g_rb_behind_range = 0x3C;
                    g_rb_ahead_scale  = 0xBE;
                    g_rb_ahead_range  = 0x64;
                    break;
                default: /* tier 2 */
                    /* [@ 0x00433389..0x004333FD] */
                    *steer = (int16_t)(((int32_t)*steer * 0xDC) >> 8);
                    *brake = 1000;
                    *lspdb = 1000;
                    *spd   = 0x433;
                    *grp   = (int16_t)(((int32_t)*grp * 0x122) >> 8);
                    g_rb_behind_scale = 0xDC;
                    g_rb_behind_range = 0x3C;
                    g_rb_ahead_scale  = 0x64;
                    g_rb_ahead_range  = 0x40;
                    break;
                }
            }
        } else {
            /* [@ 0x00432FFD..0x00433173] CIRCUIT branch — tier dispatch */
            switch (tier) {
            case 0:
                /* [@ 0x00433104..0x00433173] */
                *steer = (int16_t)(((int32_t)*steer * 0x91) >> 8);
                *brake = 1000;
                *lspdb = 1000;
                *spd   = 0x3A1;                                       /* [@ 0x0043313E] */
                *grp   = (int16_t)(((int32_t)*grp * 0xC8) >> 8);
                g_rb_behind_scale = 0x8C;                              /* [@ 0x0043314B] */
                g_rb_behind_range = 0x64;                              /* [@ 0x00433155] */
                g_rb_ahead_scale  = 0xC8;                              /* [@ 0x0043315F] */
                g_rb_ahead_range  = 0x37;                              /* [@ 0x00433169] */
                break;
            case 1:
                /* [@ 0x0043308C..0x004330FF] */
                *steer = (int16_t)(((int32_t)*steer * 0xA0) >> 8);
                *brake = 1000;
                *lspdb = 1000;
                *spd   = 0x3A1;                                       /* [@ 0x004330CA] */
                *grp   = (int16_t)(((int32_t)*grp * 0xEC) >> 8);
                g_rb_behind_scale = 0x96;                              /* [@ 0x004330D7] */
                g_rb_behind_range = 0x64;                              /* [@ 0x004330E1] */
                g_rb_ahead_scale  = 0xC0;                              /* [@ 0x004330EB] */
                g_rb_ahead_range  = 0x40;                              /* [@ 0x004330F5] */
                break;
            default: /* tier 2 */
                /* [@ 0x00433015..0x00433087] circuit tier-2 leaf */
                *steer = (int16_t)(((int32_t)*steer * 0xC3) >> 8);
                *brake = 1000;
                *lspdb = 1000;
                *spd   = 0x433;                                       /* [@ 0x00433052] */
                *grp   = (int16_t)(((int32_t)*grp * 0x104) >> 8);
                g_rb_behind_scale = 0xC8;                              /* [@ 0x0043305F] */
                g_rb_behind_range = 0x64;                              /* [@ 0x00433069] */
                g_rb_ahead_scale  = 0x78;                              /* [@ 0x00433073] */
                g_rb_ahead_range  = 0x40;                              /* [@ 0x0043307D] */
                break;
            }
        }
    }

    /* Cop Chase (wanted_mode): initialize gWantedDamageStateTable proxy.
     * Original: InitializeWantedHudOverlays @ 0x43D2FC sets all 6 int16 entries
     * to 0x1000, but ONLY when g_wantedModeEnabled != 0 [CONFIRMED @ orig
     * decomp 0x0043D2D0]. Port was missing this gate, leaving wanted_damage
     * = 0x1000 in non-cop-chase races. Inert in wanted_mode=0 (no consumers
     * read it), but visible as a diff vs orig in pool13 captures. */
    if (g_td5.wanted_mode_enabled) {
        for (int i = 0; i < TD5_MAX_RACER_SLOTS; i++)
            g_wanted_damage_state[i] = 0x1000;
        td5_ai_reset_wanted_state();   /* overlay slot -> -1 (orig @ 0x0043D2FC) */
        TD5_LOG_I(LOG_TAG,
                  "Cop Chase: g_wanted_damage_state[0..5]=0x1000 "
                  "(mirrors gWantedDamageStateTable init @ 0x0043D2FC)");
    }

    /* Restore the original CATCHUP -> 2-player steering-bias-swing propagation
     * (orig `DAT_0048301c = DAT_00465ff8` at the MainMenu->1PRace transition,
     * 0x004155EA). The revamp dropped the frontend CATCHUP row that fed this;
     * re-derive it from the resolved catchup level so the assist is restored and
     * the S05 toggle drives it. 0 = off (swing 0, the BSS default). The swing
     * only bites once a 2-player split feeds td5_input_set_player_steering_bias_input;
     * inert otherwise, but kept faithful. [S06 2026-06-04 catchup restore] */
    {
        int catchup = td5_ai_get_catchup_level();
        g_td5_steering_bias_max_swing = (catchup > 0) ? catchup : 0;
    }

    /* Initialize encounter globals */
    g_encounter_tracked_handle = -1;
    g_encounter_cooldown = 0;
    g_encounter_enabled = g_td5.special_encounter_enabled;

    /* Initialize traffic actors from queue if traffic is enabled */
    if (g_active_actor_count > g_traffic_slot_base) {
        td5_ai_init_traffic_actors();
    }

    td5_ai_refresh_route_state();

    for (int i = 0; i < g_active_actor_count; ++i) {
        TD5_LOG_I(LOG_TAG,
                  "Init AI runtime: slot=%d tier=%d mode=%d rb_behind=(scale=%d range=%d) rb_ahead=(scale=%d range=%d)",
                  i,
                  tier,
                  g_slot_state[i],
                  g_rb_behind_scale,
                  g_rb_behind_range,
                  g_rb_ahead_scale,
                  g_rb_ahead_range);
    }
}

/* ========================================================================
 * AI Steering Bias  (0x4340C0  UpdateActorSteeringBias)
 *
 * 4-layer steering pipeline:
 *   Layer 1: Route heading (computed externally, passed as heading delta)
 *   Layer 2: Offset bias (lateral avoidance from UpdateActorTrackOffsetBias)
 *   Layer 3: Threshold state (from UpdateActorRouteThresholdState)
 *   Layer 4: Steering bias (this function)
 *
 * Speed-dependent with 3 deviation bands:
 *   < 0x400: direct steering delta (sin-scaled if < 0x100)
 *   0x400-0x7FF: ramp-up with accumulator (0x40/tick, max 0x100)
 *   >= 0x800: emergency snap (0x4000/tick)
 *
 * param route_state: pointer to this actor's route state dword array
 * param steer_weight: 0x10000 (braking), 0x20000 (coasting), 0x4000 (script)
 *
 * [CONFIRMED @ 0x004340C0] Byte-faithful with orig UpdateActorSteeringBias.
 * L5 audit 2026-05-18 (TD5_pool0 read-only):
 *   - Field offsets match: longitudinal_speed +0x314, rear_axle_slip +0x320,
 *     steering_cmd +0x30C, steering_ramp_accum +0x33A, slot_index +0xD4 (RS[0x35]).
 *   - RS[0x16]/[0x17] = LEFT/RIGHT_DEVIATION (param_1+0x58/+0x5C).
 *   - abs(longitudinal_speed >> 8) via XOR-sign idiom matches.
 *   - rate cap iVar2 = 0xC0000 / ((|v|*0x400)/(slip²+0x400) + 0x40) match.
 *   - cap iVar3 = 0x1800000 / ((|v|*0x10000)/(slip²+0x10000) + 0x100) match.
 *   - NESTED cascade structure (LEFT<0x800 → LEFT<0x401 → param_2==0 OR
 *     LEFT<0x100 OR 0x100<=LEFT<0x401; ELSE 0x401<=LEFT<0x800 +0x4000;
 *     ELSE RIGHT<0x401 mirror; ELSE -0x4000) all branches walked.
 *   - Script-ramp branch: ramp += 0x40 if <0x100; bias = curr + sar_rz(iVar2*ramp, 8);
 *     write bias FIRST, then conditional overwrite to iVar3 (LEFT) or -iVar3 (RIGHT).
 *     The RIGHT mirror uses `iVar4 <= iVar7` (skip overwrite) vs port
 *     `param_2 <= bias` — semantically identical.
 *   - Sin-fine branch uses sar_rz with 0xFFF mask (n=12). Match.
 *   - Final clamp ±0x18000 match (orig 0xfffe8000 == -0x18000).
 *   - ai_sin_fixed12 uses libm sin*4096 truncate vs original's int LUT; both
 *     yield int32 (same integer values within FPU precision). Pre-existing
 *     known class — see float-LUT trig commit 4e71a88.
 * ======================================================================== */

/* Pilot tracing — per-function entry/exit emitter for pool10 / 0x004340C0. */

/* Thread-local-style call-site hint. Set by each port caller immediately
 * before invoking td5_ai_update_steering_bias() so the trace probe can
 * attribute rows to "track_behavior", "traffic_plan", or "script_advance"
 * (mirroring the Frida probe's return-address classification). */


/* [task#19 2026-06-12] TD6 AI steering damping.
 * The faithful cascade (td5_ai_update_steering_bias) adds a FLAT ±steer_weight
 * per tick whenever the heading deviation is in the mid band (0x100..0x401).
 * For RACERS the weight is 0x20000 — LARGER than the ±0x18000 saturation clamp —
 * so a single mid-band tick snaps the steering to full lock; when the heading
 * error flips sign (which it does every few ticks on TD6's curvy converted
 * routes + pulsing-lane_count geometry that scrapes the rails) the car slams
 * full-lock the other way => the reported violent zig-zag, wall scrapes and lost
 * speed. The original never hits the mid band on its hand-authored TD5 routes,
 * so this is a TD6-data problem, fixed by ramping the steering over several
 * ticks instead of snapping. Scale the weight down on TD6 only (native TD5 is
 * byte-faithful and untouched). Knob TD5RE_TD6_AI_STEER = percent of the
 * faithful weight (default 30; 100 = faithful, clamped 5..100). */
/* [task#19] A/B knob for the TD6 drivable-band clamp in smart_lane_bias
 * (TD5RE_TD6_AI_BAND=0 disables; default on). */
static int td6_ai_band_enabled(void) {
    static int s = -1;
    if (s < 0) { const char *e = getenv("TD5RE_TD6_AI_BAND"); s = (e && e[0] == '0') ? 0 : 1; }
    return s;
}

/* [task#19] TD6 proportional+self-centering steering law (port of TD6.exe's
 * FUN_0043a290). TD6 (a later engine build) REWROTE the 1999 TD5 steering
 * cascade we ported: TD5 uses BANDED flat slams (±0x8000/±0x20000 per tick in
 * the mid deviation band) and NO centering (holds the last value), which limit-
 * cycles on TD6's curvy converted routes. TD6 instead ramps the steering toward
 * a PROPORTIONAL target (|deviation|*gain) at the speed-scaled rate cap, and
 * SELF-CENTERS (decays toward 0) when aligned. TD5RE_TD6_AI_PROP=0 disables
 * (back to the faithful banded cascade). */
static int td6_prop_steer_enabled(void) {
    static int s = -1;
    if (s < 0) { const char *e = getenv("TD5RE_TD6_AI_PROP"); s = (e && e[0] == '0') ? 0 : 1; }
    return s;
}

/* Proportional gain: steer cap = |signed_deviation| * gain (deviation is a
 * 12-bit angle 0..0x800). TD6's per-car gain is iVar3/8 in [0..0x100]; 0x80 is a
 * stable midpoint. Knob TD5RE_TD6_AI_GAIN (clamped 0x10..0x100). */
static int32_t td6_steer_gain(void) {
    static int g = -1;
    if (g < 0) {
        const char *e = getenv("TD5RE_TD6_AI_GAIN");
        g = (e && e[0]) ? atoi(e) : 0x80;
        if (g < 0x10)  g = 0x10;
        if (g > 0x100) g = 0x100;
    }
    return g;
}

static int32_t td5_ai_td6_steer_weight(int32_t w) {
    static int s_pct = -1;
    if (s_pct < 0) {
        const char *e = getenv("TD5RE_TD6_AI_STEER");
        s_pct = (e && e[0]) ? atoi(e) : 100;   /* default OFF: weight-damping alone didn't help */
        if (s_pct < 5)   s_pct = 5;
        if (s_pct > 100) s_pct = 100;
    }
    if (g_active_td6_level <= 0 || s_pct >= 100) return w;
    return (int32_t)(((int64_t)w * (int64_t)s_pct) / 100);
}

void td5_ai_update_steering_bias(int *route_state, int32_t steer_weight) {
    /* Literal translation of UpdateActorSteeringBias @ 0x4340C0.
     *
     * NESTED cascade (NOT parallel left/right):
     *   if (LEFT < 0x800) left-side branches
     *   else if (RIGHT < 0x401) right-side small branches
     *   else -0x4000
     *
     * This prevents the aligned case (LEFT=0xFFF, RIGHT=0) from
     * incorrectly firing the LEFT-emergency path and saturating steer_cmd.
     *
     * param_2 == 0 path is SCRIPT mode (uses actor ramp accumulator +0x33A
     * and iVar2-rate-limited update with clamp to iVar3).
     * param_2 != 0 path is NORMAL mode (uses SinFixed12bit for fine bands
     * and direct param_2 addition for mid bands).
     * Clamp at end is always ±0x18000.
     */
    int   slot       = route_state[RS_SLOT_INDEX];
    char *actor      = actor_ptr(slot);
    int32_t iVar6    = ACTOR_I32(actor, ACTOR_LONGITUDINAL_SPEED);
    int32_t sign_mask = iVar6 >> 31;
    int32_t iVar4    = ACTOR_I32(actor, ACTOR_REAR_AXLE_SLIP) >> 8;
    int32_t iVar2, iVar3;
    int32_t left_dev, right_dev;
    int32_t bias;
    int32_t param_2 = steer_weight;

    iVar4 = iVar4 * iVar4;                           /* rear_slip_excess_shifted² */
    iVar6 = ((iVar6 >> 8) ^ sign_mask) - sign_mask;  /* abs(longitudinal_speed >> 8) */

    /* iVar2 = per-tick rate cap (small-band script path) */
    iVar2 = 0xC0000 / (((iVar6 * 0x400) / (iVar4 + 0x400)) + 0x40);

    /* iVar3 = absolute steering cap (small-band script path) */
    iVar3 = 0x1800000 / (((iVar6 * 0x10000) / (iVar4 + 0x10000)) + 0x100);

    left_dev  = route_state[RS_LEFT_DEVIATION];
    right_dev = route_state[RS_RIGHT_DEVIATION];

    /* [task#19 2026-06-12] TD6 proportional + self-centering steering law
     * (port of TD6.exe FUN_0043a290 @0x0043a290). Replaces the banded TD5 slam
     * on TD6 tracks for normal driving (param_2 != 0). Derive the SIGNED angular
     * deviation from our RIGHT deviation (RIGHT in [0,0x800] = steer right by
     * that much; (0x800,0xFFF] wraps to a left error): sdev = +right..-left,
     * matching TD6's `((target-heading-0x800)&0xfff)-0x800`. Then: ramp the
     * steering command toward a proportional cap (|sdev|*gain) at the speed-
     * scaled rate cap, decelerating through zero on a reversal, and DECAY toward
     * 0 (self-centre, 4x rate) when aligned — exactly TD6's three branches
     * (flags&1 / flags&2 / flags&3==0). The steering-ramp accumulator (+0x33A)
     * eases the correction in, as in TD6. */
    if (param_2 != 0 && g_active_td6_level > 0 && td6_prop_steer_enabled()) {
        int32_t cur   = ACTOR_I32(actor, ACTOR_STEERING_CMD);
        int16_t ramp  = ACTOR_I16(actor, ACTOR_STEERING_RAMP_ACCUM);
        int32_t sdev  = (right_dev < left_dev) ? -right_dev : left_dev;  /* + = steer positive (cascade sign) */
        int32_t inner = (iVar6 * 0x40) / (iVar4 + 0x40);          /* speed/slip term */
        int32_t rate  = (int32_t)(0xC0000 / (inner + 0x40));      /* TD6 per-tick rate cap */
        int32_t mag   = (sdev < 0) ? -sdev : sdev;
        int32_t cap   = mag * td6_steer_gain();                   /* proportional target */
        if (cap > 0x18000) cap = 0x18000;

        if (sdev > 0x40) {                       /* steer RIGHT toward +cap */
            if (ramp < 0x100) ramp += 0x40;
            if (cur < 0) cur += rate * 2;        /* decel through zero on reversal */
            else         cur += ((int32_t)ramp * rate) >> 8;
            if (cur > cap) cur = cap;
        } else if (sdev < -0x40) {               /* steer LEFT toward -cap */
            if (ramp < 0x100) ramp += 0x40;
            if (cur > 0) cur -= rate * 2;
            else         cur -= ((int32_t)ramp * rate) >> 8;
            if (cur < -cap) cur = -cap;
        } else {                                 /* aligned: SELF-CENTRE (decay to 0) */
            if (ramp > 0) ramp -= 0x40;
            int32_t dec = rate * 4;
            if (cur > dec)       cur -= dec;
            else if (cur < -dec) cur += dec;
            else                 cur = 0;
        }
        if (cur > 0x18000)       cur = 0x18000;
        else if (cur < -0x18000) cur = -0x18000;

        ACTOR_I16(actor, ACTOR_STEERING_RAMP_ACCUM) = ramp;
        ACTOR_I32(actor, ACTOR_STEERING_CMD)        = cur;
        if ((g_ai_frame_counter % 60u) == 0u)
            TD5_LOG_I(LOG_TAG, "td6_prop_steer: slot=%d sdev=%d cap=%d rate=%d out=%d",
                      slot, sdev, cap, rate, cur);
        return;
    }

    if (left_dev < 0x800) {
        if (left_dev < 0x401) {
            if (param_2 == 0) {
                /* Script mode: ramp-accumulated delta, clamp to +iVar3 */
                int16_t ramp = ACTOR_I16(actor, ACTOR_STEERING_RAMP_ACCUM);
                if (ramp < 0x100) {
                    ramp += 0x40;
                    ACTOR_I16(actor, ACTOR_STEERING_RAMP_ACCUM) = ramp;
                }
                iVar2 = (int32_t)ramp * iVar2;
                bias = ACTOR_I32(actor, ACTOR_STEERING_CMD)
                     + ((iVar2 + ((iVar2 >> 31) & 0xFF)) >> 8);
                ACTOR_I32(actor, ACTOR_STEERING_CMD) = bias;
                if (iVar3 < bias) {
                    ACTOR_I32(actor, ACTOR_STEERING_CMD) = iVar3;
                }
                goto clamp_end;
            }
            if (left_dev < 0x100) {
                /* Normal mode, fine deviation: proportional correction.
                 * [CONFIRMED @ 0x4340C0 via 0x0040a700, and LUT init
                 * BuildSinCosLookupTables @ 0x0040a650]
                 * Original calls FUN_0040a700(raw_deviation) = SinFixed12bit.
                 * DAT_00483984 is a COSINE table (init stores cos(i*step)*4096),
                 * so LUT[(arg - 0x400) & 0xFFF] = cos(arg - π/2) = +sin(arg).
                 * For small deviation the return is ≈ arg, producing a
                 * proportional correction. sin(0) = 0 → aligned car (left=0xFFF
                 * right=0 fires this branch via the right mirror) stays put. */
                int32_t t   = ai_sin_fixed12(left_dev);
                int32_t mul = t * param_2;
                ACTOR_I32(actor, ACTOR_STEERING_CMD) =
                    ACTOR_I32(actor, ACTOR_STEERING_CMD)
                    + ((mul + ((mul >> 31) & 0xFFF)) >> 12);
                TD5_LOG_I(LOG_TAG, "cascade_left_fine: slot=%d L=%d w=%d t=%d",
                          slot, left_dev, param_2, t);
                goto clamp_end;
            }
            /* 0x100 <= left_dev < 0x401: direct add param_2 */
            param_2 = ACTOR_I32(actor, ACTOR_STEERING_CMD) + param_2;
        } else {
            /* 0x401 <= left_dev < 0x800: fixed +0x4000 */
            param_2 = ACTOR_I32(actor, ACTOR_STEERING_CMD) + 0x4000;
        }
    } else if (right_dev < 0x401) {
        if (param_2 == 0) {
            /* Script mode mirror: subtract, clamp to -iVar3 */
            int16_t ramp = ACTOR_I16(actor, ACTOR_STEERING_RAMP_ACCUM);
            if (ramp < 0x100) {
                ramp += 0x40;
                ACTOR_I16(actor, ACTOR_STEERING_RAMP_ACCUM) = ramp;
            }
            iVar2 = (int32_t)ramp * iVar2;
            bias = ACTOR_I32(actor, ACTOR_STEERING_CMD)
                 - ((iVar2 + ((iVar2 >> 31) & 0xFF)) >> 8);
            param_2 = -iVar3;
            ACTOR_I32(actor, ACTOR_STEERING_CMD) = bias;
            if (param_2 <= bias) {
                goto clamp_end;
            }
            /* fall through: write -iVar3 as new bias */
        } else {
            if (right_dev < 0x100) {
                /* +sin via FUN_0040a700 mirror — see left fine-band comment.
                 * Aligned car (delta=0) -> LEFT=0xFFF, RIGHT=0: this path
                 * fires with sin(0)=0 → no correction → car stays aligned. */
                int32_t t   = ai_sin_fixed12(right_dev);
                int32_t mul = t * param_2;
                ACTOR_I32(actor, ACTOR_STEERING_CMD) =
                    ACTOR_I32(actor, ACTOR_STEERING_CMD)
                    - ((mul + ((mul >> 31) & 0xFFF)) >> 12);
                TD5_LOG_I(LOG_TAG, "cascade_right_fine: slot=%d R=%d w=%d t=%d",
                          slot, right_dev, param_2, t);
                goto clamp_end;
            }
            /* 0x100 <= right_dev < 0x401: direct sub param_2 */
            param_2 = ACTOR_I32(actor, ACTOR_STEERING_CMD) - param_2;
        }
    } else {
        /* LEFT >= 0x800 AND RIGHT >= 0x401: emergency snap -0x4000 */
        param_2 = ACTOR_I32(actor, ACTOR_STEERING_CMD) + -0x4000;
    }
    ACTOR_I32(actor, ACTOR_STEERING_CMD) = param_2;

clamp_end:
    if (ACTOR_I32(actor, ACTOR_STEERING_CMD) > 0x18000) {
        ACTOR_I32(actor, ACTOR_STEERING_CMD) = 0x18000;
    } else if (ACTOR_I32(actor, ACTOR_STEERING_CMD) < -0x18000) {
        ACTOR_I32(actor, ACTOR_STEERING_CMD) = -0x18000;
    }

    TD5_LOG_I(LOG_TAG, "steer_bias: slot=%d L=%d R=%d w=%d out=%d",
              slot, left_dev, right_dev, steer_weight,
              ACTOR_I32(actor, ACTOR_STEERING_CMD));

}

/* ========================================================================
 * Route Threshold State  (0x434AA0  UpdateActorRouteThresholdState)
 *
 * Byte-faithful port of TD5_d3d.exe @ 0x00434AA0..0x00434B95
 * (TD5_pool6, Ghidra read_only=true, 2026-05-14).
 *
 * Throttle/brake controller operating on the route speed-threshold byte.
 * Consumes the rubber-band-modified throttle bias.
 *
 * Returns: 1 only on the emergency-brake branch (threshold==0 AND
 *          fwd_comp>0x80 AND speed<0x10000); 0 on all other paths
 *          including the slot-9 special-encounter early-exit.
 *
 * Listing structure (post-prologue at 0x00434ACF):
 *
 *   [SPECIAL-ENCOUNTER EARLY-EXIT @ 0x00434AAD-0x00434ACE]
 *     if (slot == 9
 *         && gSpecialEncounterTrackedActorHandle != -1
 *         && gActorSpecialEncounterActive[tracked * 0x47] != 0)
 *       goto early_out;            ; JNZ 0x00434B8F → ret 0, NO writes
 *
 *   [MAIN BODY @ 0x00434ACF-0x00434B7B]
 *     span      = (int16_t)actor[+0x82]    ; SPAN_NORMALIZED, MOVSX
 *     rt        = RS[0]                    ; ROUTE_TABLE_PTR
 *     bias      = RS[4]                    ; DEFAULT_THROTTLE / steer bias
 *     threshold = (uint8_t)rt[span*3 + 2]
 *     if (threshold == 0) goto T0;
 *     ; --- scaled-threshold branch ---
 *     scaled = ((threshold << 10) * 0x80808081_signed) added/SAR(7)
 *            = (threshold * 0x400) / 255          ; signed magic div
 *     if (fwd_comp >= scaled) {            ; JL fallthrough
 *       actor[+0x33E] = 0;
 *       actor[+0x36D] = 0;
 *       actor[+0x36F] = 0;
 *       return 0;
 *     }
 *     goto fallback;
 *   T0:
 *     if (fwd_comp <= 0x80) goto fallback; ; JLE
 *     if (speed   >= 0x10000) goto fallback; ; JGE
 *     actor[+0x33E] = 0xFF00;
 *     actor[+0x36D] = 1;
 *     actor[+0x36F] = 1;
 *     return 1;
 *   fallback:                              ; 0x00434B7C
 *     actor[+0x33E] = (int16_t)bias;
 *     actor[+0x36D] = 0;
 *     actor[+0x36F] = 0;
 *   early_out:                             ; 0x00434B8F
 *     return 0;
 *
 * Span source: MOVSX EAX, word [ECX + 0x4ab18a] at 0x00434AE8 with
 * ECX = slot * 0x388. Base 0x4ab18a = g_actorRuntimeState + 0x82, so the
 * reader uses actor[+0x82] = ACTOR_SPAN_NORMALIZED, NOT the raw +0x80.
 * On junction-remap spans (Moscow ~498, Newcastle wrap) the wrong field
 * triggered spurious emergency-brake / scaled-throttle gates — the
 * normalized field is the only one safe for route-table lookups.
 *
 * Special-encounter early-exit: the original gates the whole writer
 * behind the slot-9 race/traffic hijack flag so a hijacked slot 9 does
 * not stomp the racer-AI encounter_steer / brake outputs while
 * UpdateSpecialEncounterControl is also writing them. Port mirrors this
 * gate exactly using g_encounter_tracked_handle / g_encounter_active[]
 * which are this module's analogs of gSpecialEncounterTrackedActorHandle
 * / gActorSpecialEncounterActive[slot * 0x11c]. */

int td5_ai_update_route_threshold(int slot) {
    /* --- Special-encounter early-exit (slot 9 only) --------------------
     * Listing 0x00434AAD-0x00434ACE: if (slot==9 && tracked!=-1 &&
     * gActorSpecialEncounterActive[tracked*0x47]!=0) ret 0 without writing.
     * FIXME: port indexes g_encounter_active[] per-slot; original indexes
     * gActorSpecialEncounterActive at stride 0x11c. Both are "is slot N
     * currently in a special encounter?" with the same non-zero semantics. */
    if (slot == 9
        && g_encounter_tracked_handle != -1
        && g_encounter_active[g_encounter_tracked_handle] != 0) {
        return 0;
    }

    int32_t *rs    = route_state(slot);
    char    *actor = actor_ptr(slot);

    int32_t fwd_comp = g_actor_forward_track_component[slot]; /* RS[24] mirror */
    int32_t speed    = ACTOR_I32(actor, ACTOR_LONGITUDINAL_SPEED); /* +0x31C */
    int32_t bias     = g_actor_route_steer_bias[slot];        /* RS[4] mirror */

    /* MOVSX EAX, [actor+0x82] @ 0x00434AE8 — sign-extended int16. */
    int32_t span = (int32_t)ACTOR_I16(actor, ACTOR_SPAN_NORMALIZED);

    /* MOV AL, [EDI + EDX + 2] with EDI = span*3, EDX = rt; threshold is
     * a zero-extended byte (XOR EAX,EAX before MOV AL).
     *
     * PORT-ONLY NULL GUARD (KEEP): original 0x00434AFB dereferences
     * RS[0]=route_table unconditionally. Upstream invariant in original is
     * "RS[0] is bound to a LEFT.TRK / RIGHT.TRK throttle table during
     * BindActorRouteState at race-start" (race-init path 0x00435F60). Port
     * runs this AI tick from td5_game.c's menu-state benchmark path with
     * unbound RS slots, so a NULL would fault. Behaving as "no limit"
     * (threshold=0xFF) maps to the bias-fallback exit in original. */
    int32_t threshold;
    {
        const uint8_t *route_table = (const uint8_t *)(intptr_t)rs[RS_ROUTE_TABLE_PTR];
        size_t rt_sz = (route_table == g_route_tables[1]) ? g_route_table_sizes[1]
                                                          : g_route_table_sizes[0];
        if (route_table && span >= 0 &&
            ((size_t)(unsigned)span * 3u + 2u) < rt_sz) {
            threshold = (int32_t)route_table[(size_t)(unsigned)span * 3u + 2u];
        } else {
            /* NULL table OR span past the route table -> no-limit fallback
             * (0xFF), matching the original's bias-fallback exit. The added
             * bound prevents the Courmayeur-class OOB read at
             * route_table[span*3+2] when a branch-remapped span exceeds the
             * 600-row LEFT/RIGHT.TRK table. */
            threshold = 0xFF;
        }
    }

    TD5_LOG_D(LOG_TAG, "route_threshold: slot=%d span_norm=%d thr=0x%02X",
              slot, (int)span, (unsigned)threshold);

    if (threshold == 0) {
        /* T0 branch @ 0x00434B45 */
        if (fwd_comp > 0x80 && speed < 0x10000) {
            ACTOR_I16(actor, ACTOR_ENCOUNTER_STEER) = (int16_t)0xFF00;
            ACTOR_U8(actor, ACTOR_BRAKE_FLAG)       = 1;
            ACTOR_U8(actor, ACTOR_THROTTLE_STATE)   = 1;
            return 1;
        }
        /* fall through to fallback */
    } else {
        /* Scaled-threshold branch @ 0x00434B0B-0x00434B29.
         * Magic-divide of (threshold << 10) by 0xFF:
         *   IMUL EDI by 0x80808081 (signed) → high 32 bits → ADD EDX,EDI → SAR 7
         *   → ADD signed-bit correction. Equivalent to signed (a/255).
         * Since threshold ∈ [1,255] and (threshold<<10) ∈ [0x400,0x3FC00],
         * (threshold * 0x400) / 0xFF reproduces the result on any modern compiler.
         * At threshold==0xFF this yields 0x400 (not skipped). */
        int32_t scaled = (threshold * 0x400) / 0xFF;

        if (fwd_comp >= scaled) {
            ACTOR_I16(actor, ACTOR_ENCOUNTER_STEER) = 0;
            ACTOR_U8(actor, ACTOR_BRAKE_FLAG)       = 0;
            ACTOR_U8(actor, ACTOR_THROTTLE_STATE)   = 0;
            return 0;
        }
        /* fall through to fallback */
    }

    /* fallback @ 0x00434B7C: MOV word [...], BP — write low 16 bits of bias. */
    ACTOR_I16(actor, ACTOR_ENCOUNTER_STEER) = (int16_t)bias;
    ACTOR_U8(actor, ACTOR_BRAKE_FLAG)       = 0;
    ACTOR_U8(actor, ACTOR_THROTTLE_STATE)   = 0;
    return 0;
}

/* ========================================================================
 * Track Offset Bias / Peer Avoidance
 *   0x4337E0  FindActorTrackOffsetPeer
 *   0x434900  UpdateActorTrackOffsetBias
 * ======================================================================== */

/**
 * FindActorTrackOffsetPeer (precise-004337E0): byte-faithful port of the
 * original at 0x004337E0. Replaces the prior simplified single-pass scan.
 *
 * TWO-PASS structure [CONFIRMED @ disassembly 0x004337E0-0x00433CDE]:
 *
 * Pass 1 (TRAFFIC slots): gated on `g_racerCount > 6`. Iterates slots 6..N-1.
 *   - Compares self vs each traffic slot. Uses peer.field_0x82 (span_normalized)
 *     for the proximity test (self.field_0x82 <= peer.field_0x82).
 *   - Cross-route is NOT a separate branch in this pass — same body executes
 *     for both. The route-table swap happens inline (rs[0]==DAT_004afb58 chain).
 *   - Range gate: target_offset_at_clamp must lie within peer's RS[0x14],RS[0x15].
 *   - dist gate (final): `0 < local_c && local_c < 0x28` and best != self.
 *
 * Pass 2 (RACER slots): always runs. Iterates slots 0..min(g_racerCount,6)-1,
 *   skipping i==self.
 *   - Extra gate: peer.RS[0x18] - 0x10 <= self.RS[0x18]
 *     (RS_FORWARD_TRACK_COMP forward-track-component proximity).
 *   - Cross-route inline swap (same code as pass 1).
 *   - dist gate (final): `-1 < local_c && local_c <= 0x28` and best != self.
 *
 * Both passes use ClassifyTrackOffsetClamp + ComputeSignedTrackOffset (port:
 * td5_ai_classify_track_offset_clamp, module-level helper) to produce the
 * lateral target offset used in the range-gate against peer's RS[0x14]/RS[0x15].
 *
 * Direction formula [CONFIRMED @ 0x00433A11-0x00433A4D and 0x00433C84-0x00433CBE]:
 *   mid = (DAT_004afbb4[best*0x47] + DAT_004afbb0[best*0x47]) / 2
 *     -- these are RS[0x14]/RS[0x15] of the BEST peer (signed CDQ-divide /2,
 *        which equals C99 signed div for nonneg arg). Pass uses SAR 1 after
 *        CDQ — the original adds the sign bit before SAR, so >>1 with signed
 *        is equivalent to /2 with truncation toward -infinity for negatives
 *        (different from C /2 which truncates toward zero). We match SAR 1.
 *   abs_r = abs((peer_cardef_hi - mid) + 0x20 + offset_at_clamp)
 *   abs_l = abs((peer_cardef_lo - mid) - 0x20 + offset_at_clamp)
 *   DAT_004b08b0 = (abs_r >= abs_l)  -- note: SETGE means right >= left,
 *     which differs from prior port's "abs_r <= abs_l" by sign convention.
 *   ** Bounds source [CONFIRMED @ 0x00433C92]: cardef pointer comes from
 *     g_actorRuntimeState.slot[self_slot].field_0x1b8 (SELF's cardef), NOT
 *     the peer's. The EAX*0x8 arithmetic resolves to self_slot*0x388+0x1b8.
 *
 * Returns: best peer slot (int), or self_slot when nothing in range. Also
 * writes DAT_004b08b0 (g_lateral_avoidance_direction) when a peer is found
 * AND the direction formula executed.
 *
 * [CONFIRMED @ disassembly 0x004337E0-0x00433CDE — full pool14 audit].
 */

/* td5_ai_classify_track_offset_clamp_v2: byte-faithful port of
 * ClassifyTrackOffsetClamp @ 0x004368A0. Returns 0,1,2 to select which side's
 * cardef offset to apply in the target-offset formula.
 *   0 = in range (both samples produce span_progress >= 0xFF on first try
 *       OR second sample's progress > 0).
 *   1 = clamp against the "hi-bound" path (first sample produced progress
 *       >= 0xFF — early-exit with low32 = 1).
 *   2 = clamp against the "lo-bound" path (second sample's progress <= 0).
 *
 * Original control flow [CONFIRMED @ 0x004368A0-0x00436A65]:
 *   if (rs[0] == DAT_004afb58)             -- primary route
 *       iVar3 = span_norm                  -- NO walker; fall through
 *   else                                   -- secondary route
 *       iVar3 = span_norm
 *       iVar5 = (iVar3 + 4) wrapped
 *       walk DAT_004c3da0 jump table:
 *           if a row matches iVar5: iVar5 = remap; goto LAB_0043699b
 *   fall-through (primary OR secondary that did NOT match walker):
 *   iVar5 = (iVar3 + 4) wrapped            -- unconditional default
 *   LAB_0043699b:
 *     sample using iVar5 and iVar4 (= iVar3+4 wrapped)
 *
 * Prior port double-applied the walker remap (called
 * td5_track_apply_target_span_remap twice in succession on the secondary
 * route), which could produce the wrong iVar5 if the helper is not
 * idempotent on already-remapped spans. The original applies it ONCE.
 */
static int td5_ai_classify_track_offset_clamp_v2(int param_1, int param_2) {
    int32_t *rs = route_state(param_1);
    char *self = actor_ptr(param_1);
    int iVar3 = (int)ACTOR_I16(self, ACTOR_SPAN_NORMALIZED);
    int iVar5;
    int iVar4;
    int local_c[3] = {0, 0, 0};
    int64_t lVar7;
    const uint8_t *route_bytes = (const uint8_t *)(intptr_t)rs[RS_ROUTE_TABLE_PTR];
    int16_t *self_cd;
    int walker_matched = 0;

    /* Secondary-route walker remap: ONLY on secondary, ONLY once.
     * On primary (rs[0] == g_route_tables[0]) skip the walker entirely. */
    if (rs[RS_ROUTE_TABLE_PTR] != (int32_t)(intptr_t)g_route_tables[0]) {
        int iVar5_pre = iVar3 + 4;
        if (g_strip_span_count > 0 && g_strip_span_count <= iVar5_pre) {
            iVar5_pre = (iVar3 - g_strip_span_count) + 4;
        }
        {
            int remapped = td5_track_apply_target_span_remap(iVar5_pre, /*is_canonical=*/0);
            if (remapped != iVar5_pre && remapped != -1) {
                iVar5 = remapped;
                walker_matched = 1;
            } else {
                iVar5 = iVar5_pre; /* fall through to LAB_0043699b default */
            }
        }
    } else {
        iVar5 = 0; /* placeholder; set below in fall-through */
    }

    /* LAB_0043699b fall-through: when walker did not match (or on primary
     * route), iVar5 = (iVar3 + 4) wrapped. */
    if (!walker_matched) {
        iVar5 = iVar3 + 4;
        if (g_strip_span_count > 0 && g_strip_span_count <= iVar5) {
            iVar5 = (iVar3 - g_strip_span_count) + 4;
        }
    }

    iVar4 = iVar3 + 4;
    if (g_strip_span_count > 0 && g_strip_span_count <= iVar4) {
        iVar4 = (iVar3 - g_strip_span_count) + 4;
    }

    self_cd = (int16_t *)(intptr_t)ACTOR_I32(self, ACTOR_CAR_DEF_PTR);
    if (!route_bytes || !self_cd) return 0;

    /* SampleTrackTargetPoint(iVar5, route_bytes[iVar4*3], local_c,
     *                         cardef[+0x08] + param_2) */
    {
        int route_byte_a = (int)route_bytes[(size_t)iVar4 * 3u];
        int bias_arg = (int)self_cd[4] + param_2; /* cardef+0x08 = hi bound */
        if (!td5_track_sample_target_point(iVar5, route_byte_a,
                                            &local_c[0], &local_c[2], bias_arg)) {
            return 0;
        }
        lVar7 = td5_track_compute_span_progress(iVar5, local_c);
    }
    if ((int)lVar7 < 0xff) {
        /* Second sample uses cardef[+0x00] (lo bound) + param_2. */
        int span_norm = (int)ACTOR_I16(self, ACTOR_SPAN_NORMALIZED);
        int iVar3c = span_norm + 4;
        int route_byte_b;
        int bias_arg2;
        if (g_strip_span_count > 0 && g_strip_span_count <= iVar3c) {
            iVar3c = (span_norm - g_strip_span_count) + 4;
        }
        route_byte_b = (int)route_bytes[(size_t)iVar3c * 3u];
        bias_arg2 = (int)self_cd[0] + param_2;
        if (!td5_track_sample_target_point(iVar5, route_byte_b,
                                            &local_c[0], &local_c[2], bias_arg2)) {
            return 0;
        }
        lVar7 = td5_track_compute_span_progress(iVar5, local_c);
        /* (0 < lVar7) - 1 → 0 when lVar7>0, -1 (=0xFFFFFFFF) when lVar7<=0.
         * Mask & 0x200000002 keeps only bit 1: net low32 = (lVar7 > 0) ? 0 : 2. */
        return (((int)lVar7 > 0) ? 0 : 2);
    }
    /* CONCAT44((int)((ulonglong)lVar7 >> 0x20), 1) -> low32 = 1 */
    return 1;
}

int td5_ai_find_offset_peer(int *route_state_ptr) {
    int slot = route_state_ptr[RS_SLOT_INDEX];
    char *self = actor_ptr(slot);
    int self_slot = slot;
    int32_t self_field82 = (int32_t)ACTOR_I16(self, ACTOR_SPAN_NORMALIZED);
    int32_t self_field80 = (int32_t)ACTOR_I16(self, ACTOR_SPAN_RAW);
    int32_t self_fwd_track = route_state_ptr[RS_FORWARD_TRACK_COMP];
    int racer_count = g_active_actor_count;
    int32_t best_slot;
    int32_t best_dist;
    int i;
    int32_t offset_at_clamp = route_state_ptr[RS_TRACK_OFFSET_BIAS];

    (void)self_field80; /* read below per-peer */

    /* ----------------------------------------------------------------
     * Pass 1: TRAFFIC slots [g_traffic_slot_base .. racer_count-1].
     * Gated on racer_count > g_traffic_slot_base (i.e., traffic mode active).
     * [CONFIRMED @ 0x004337E8/0x00433807/0x00433815: dual cmp,jl/jle].
     * [PORT: N-way] "6" is the racer/traffic boundary; legacy => 6. */
    best_slot = self_slot;
    best_dist = 0x2ee00;
    if (racer_count > g_traffic_slot_base) {
        for (i = g_traffic_slot_base; i < racer_count; i++) {
            int32_t *peer_rs = route_state(i);
            char *peer_actor = actor_ptr(i);
            int32_t peer_field82 = (int32_t)ACTOR_I16(peer_actor, ACTOR_SPAN_NORMALIZED);
            int32_t peer_field80 = (int32_t)ACTOR_I16(peer_actor, ACTOR_SPAN_RAW);
            int32_t classify, offset_a, offset_b;
            int classify_result;
            int16_t *peer_cd;
            int32_t bound_lo_q, bound_hi_q;
            int32_t dist;

            /* [PER-VIEWPORT TRAFFIC] skip other-partition peers so a car's lateral
             * avoidance ignores its cross-partition twin (gated; no-op when off). */
            if (td5_ai_traffic_pair_blocked(self_slot, i)) continue;
            /* Route-id match: rs[3] [CONFIRMED @ 0x00433832] */
            if (route_state_ptr[3] != peer_rs[3]) continue;
            /* self.field_0x82 <= peer.field_0x82 [CONFIRMED @ 0x00433851] */
            if (self_field82 > peer_field82) continue;

            /* Cross-route swap [CONFIRMED @ 0x0043385C-0x0043388A] */
            if (route_state_ptr[RS_ROUTE_TABLE_PTR] != peer_rs[RS_ROUTE_TABLE_PTR]) {
                if (route_state_ptr[RS_ROUTE_TABLE_PTR] == (int32_t)(intptr_t)g_route_tables[0]) {
                    /* On primary: rs[0x0F]→rs[9], ptr ← DAT_004b08b4
                     *   [DAT_004b08b4 = g_route_tables[1] / RIGHT route] */
                    route_state_ptr[RS_TRACK_OFFSET_BIAS] = route_state_ptr[RS_LEFT_BOUNDARY_B];
                    route_state_ptr[RS_ROUTE_TABLE_PTR]   = (int32_t)(intptr_t)g_route_tables[1];
                } else {
                    /* On secondary (or other): rs[0x0E]→rs[9], ptr ← DAT_004afb58 */
                    route_state_ptr[RS_TRACK_OFFSET_BIAS] = route_state_ptr[RS_LEFT_BOUNDARY_A];
                    route_state_ptr[RS_ROUTE_TABLE_PTR]   = (int32_t)(intptr_t)g_route_tables[0];
                }
            }
            /* Bounds writeback [original 0x00433887-0x004338A8].
             *
             * Direction inverted vs the prior port comment: empirical replay
             * data (Honolulu sub_tick=0 → sub_tick=1) shows orig writes
             *   RS_ACTIVE_LOWER (RS[0x14]) = RS_RIGHT_BOUNDARY_B (RS[0x11])
             *   RS_ACTIVE_UPPER (RS[0x15]) = RS_RIGHT_BOUNDARY_A (RS[0x10])
             * across all 6 racer slots — matching the bias-clamp Step-12
             * writeback at lines 1017-1018 / 1044-1045. The previous port
             * comment ("rs[0x14]←rs[0x10]") was wrong; Ghidra mislabeled the
             * dst indices. Aligning find_offset_peer with the bias-clamp
             * direction keeps both writers consistent. */
            if (route_state_ptr[RS_ROUTE_TABLE_PTR] == (int32_t)(intptr_t)g_route_tables[1]) {
                route_state_ptr[RS_ACTIVE_UPPER_BOUND] = route_state_ptr[RS_RIGHT_EXTENT_A];
                route_state_ptr[RS_ACTIVE_LOWER_BOUND] = route_state_ptr[RS_RIGHT_EXTENT_B];
            } else {
                route_state_ptr[RS_ACTIVE_UPPER_BOUND] = route_state_ptr[RS_RIGHT_BOUNDARY_A];
                route_state_ptr[RS_ACTIVE_LOWER_BOUND] = route_state_ptr[RS_RIGHT_BOUNDARY_B];
            }

            classify_result = td5_ai_classify_track_offset_clamp(i, route_state_ptr[RS_TRACK_OFFSET_BIAS]);

            peer_cd = (int16_t *)(intptr_t)ACTOR_I32(self, ACTOR_CAR_DEF_PTR);
            if (!peer_cd) continue;
            offset_a = (int32_t)peer_cd[0];      /* cardef+0x00 */
            offset_b = (int32_t)peer_cd[4];      /* cardef+0x08 */

            if (classify_result == 1) {
                int32_t signed_off = td5_track_compute_signed_offset(
                    (int)peer_field80, 0x100, (int)(uint8_t)((const uint8_t *)(intptr_t)peer_rs[RS_ROUTE_TABLE_PTR])[(size_t)peer_field82 * 3u]);
                classify = (int32_t)peer_cd[0] + signed_off - 0x20;
            } else if (classify_result == 2) {
                int32_t signed_off = td5_track_compute_signed_offset(
                    (int)peer_field80, 0, (int)(uint8_t)((const uint8_t *)(intptr_t)peer_rs[RS_ROUTE_TABLE_PTR])[(size_t)peer_field82 * 3u]);
                classify = (int32_t)peer_cd[4] + signed_off - 0x20;
            } else {
                classify = route_state_ptr[RS_TRACK_OFFSET_BIAS];
            }

            /* Self cardef for the range gate at 0x00433968-0x00433988:
             *   bound_lo_q = cardef[0] + classify - 0x20
             *   bound_hi_q = cardef[8] + classify + 0x20
             *   guards: bound_lo_q <= peer.RS[0x14] AND peer.RS[0x15] <= bound_hi_q */
            bound_lo_q = offset_a + classify - 0x20;
            bound_hi_q = offset_b + classify + 0x20;
            if (bound_lo_q > peer_rs[RS_ACTIVE_LOWER_BOUND]) continue;
            if (peer_rs[RS_ACTIVE_UPPER_BOUND] > bound_hi_q) continue;

            dist = peer_field82 - self_field82;
            if (dist < best_dist) {
                best_dist = dist;
                best_slot = i;
                offset_at_clamp = classify; /* piVar5 carried forward */
            }
            (void)peer_field80; /* used inside classify branches */
        }

        /* Pass 1 finalisation [CONFIRMED @ 0x004339D8-0x00433A5D]:
         *   0 < local_c && local_c < 0x28 && piVar11 != piVar3[0x35] */
        if (best_dist > 0 && best_dist < 0x28 && best_slot != self_slot) {
            int32_t *bp_rs = route_state(best_slot);
            int32_t mid;
            int32_t mid_lo = bp_rs[RS_ACTIVE_LOWER_BOUND]; /* DAT_004afbb0[best*0x47] = RS[0x14] */
            int32_t mid_hi = bp_rs[RS_ACTIVE_UPPER_BOUND]; /* DAT_004afbb4[best*0x47] = RS[0x15] */
            int32_t mid_sum = mid_hi + mid_lo;
            int16_t *cd_for_dir;
            int32_t val_r, val_l, abs_r, abs_l;

            /* CDQ + SAR ESI,1 → arithmetic shift right (toward -infinity).
             * For non-negative sums this equals /2; for negatives, differs from
             * C99 signed /2 by one when sum is odd. Use shift to match. */
            mid = mid_sum >> 1;

            /* Self cardef at [0x00433957-0x00433968]: arithmetic resolves to
             *   ECX = self.field_0xd4 = RS_SLOT_INDEX
             *   EAX = (ECX*8 - ECX) << 4 + ECX = ECX*0x71 → then SHL 3 → ECX*0x388
             *   EBX = *(DWORD*)(EAX*8 + 0x4ab2c0) = actor[ECX].field_0x1b8 = self_cd
             * I.e. uses SELF's cardef. */
            cd_for_dir = (int16_t *)(intptr_t)ACTOR_I32(self, ACTOR_CAR_DEF_PTR);
            if (cd_for_dir) {
                int32_t cd_lo = (int32_t)cd_for_dir[0]; /* cardef+0x00 */
                int32_t cd_hi = (int32_t)cd_for_dir[4]; /* cardef+0x08 */

                val_r = (cd_hi - mid) + 0x20 + offset_at_clamp;
                val_l = (cd_lo - mid) - 0x20 + offset_at_clamp;
                abs_r = (val_r < 0) ? -val_r : val_r;
                abs_l = (val_l < 0) ? -val_l : val_l;
                /* FIX 2026-05-21: prior port comparison was `(abs_r >= abs_l)`,
                 * but orig at 0x00433A4B is `CMP EAX,EBP` where EAX=abs(val_l),
                 * EBP=abs(val_r), then `SETGE CL` ⇒ dir = (abs_l >= abs_r).
                 * The previous comment misread the operand order. With the
                 * inverted comparison the dir flipped each call, making
                 * UpdateActorTrackOffsetBias oscillate bias between two values
                 * instead of accumulating in one direction. */
                g_lateral_avoidance_direction = (abs_l >= abs_r) ? 1 : 0;
            }
            return best_slot;
        }
        /* fall through: pass 1 found nothing; reset for pass 2 */
    }

    /* ----------------------------------------------------------------
     * Pass 2: RACER slots [0 .. min(racer_count, g_traffic_slot_base)-1],
     * skipping self. [CONFIRMED @ 0x00433A5E-0x00433CD0].
     * [PORT: N-way] racer region is [0, g_traffic_slot_base); legacy => 6.
     * Inactive racer slots are filtered by the span/route checks below, the
     * same way the original handles a <6-car race scanning all 6 slots. */
    {
        int cap = (racer_count >= g_traffic_slot_base) ? g_traffic_slot_base : racer_count;
        best_slot = self_slot;
        best_dist = 0x2ee00;
        offset_at_clamp = route_state_ptr[RS_TRACK_OFFSET_BIAS];
        for (i = 0; i < cap; i++) {
            int32_t *peer_rs;
            char *peer_actor;
            int32_t peer_field82, peer_field80;
            int classify_result;
            int32_t classify;
            int16_t *peer_cd;
            int32_t bound_lo_q, bound_hi_q;
            int32_t dist;

            if (i == self_slot) continue;
            /* [PER-VIEWPORT TRAFFIC] a partition's car avoids only its OWN
             * viewport's player, not the other pane's (gated; no-op when off). */
            if (td5_ai_traffic_pair_blocked(self_slot, i)) continue;
            peer_rs = route_state(i);
            peer_actor = actor_ptr(i);

            /* rs[3] route-id match [CONFIRMED @ 0x00433AA5] */
            if (route_state_ptr[3] != peer_rs[3]) continue;
            peer_field82 = (int32_t)ACTOR_I16(peer_actor, ACTOR_SPAN_NORMALIZED);
            peer_field80 = (int32_t)ACTOR_I16(peer_actor, ACTOR_SPAN_RAW);
            /* self.field_0x82 <= peer.field_0x82 [CONFIRMED @ 0x00433AC7] */
            if (self_field82 > peer_field82) continue;
            /* peer.RS[0x18] - 0x10 <= self.RS[0x18] [CONFIRMED @ 0x00433AD0-0x00433AD9] */
            if (peer_rs[RS_FORWARD_TRACK_COMP] - 0x10 > self_fwd_track) continue;

            /* Cross-route inline swap [CONFIRMED @ 0x00433ADF-0x00433B0B] */
            if (route_state_ptr[RS_ROUTE_TABLE_PTR] != peer_rs[RS_ROUTE_TABLE_PTR]) {
                if (route_state_ptr[RS_ROUTE_TABLE_PTR] == (int32_t)(intptr_t)g_route_tables[0]) {
                    route_state_ptr[RS_TRACK_OFFSET_BIAS] = route_state_ptr[RS_LEFT_BOUNDARY_B];
                    route_state_ptr[RS_ROUTE_TABLE_PTR]   = (int32_t)(intptr_t)g_route_tables[1];
                } else {
                    route_state_ptr[RS_TRACK_OFFSET_BIAS] = route_state_ptr[RS_LEFT_BOUNDARY_A];
                    route_state_ptr[RS_ROUTE_TABLE_PTR]   = (int32_t)(intptr_t)g_route_tables[0];
                }
            }
            /* Pass-2 same direction as Pass-1 (see comment above). */
            if (route_state_ptr[RS_ROUTE_TABLE_PTR] == (int32_t)(intptr_t)g_route_tables[1]) {
                route_state_ptr[RS_ACTIVE_UPPER_BOUND] = route_state_ptr[RS_RIGHT_EXTENT_A];
                route_state_ptr[RS_ACTIVE_LOWER_BOUND] = route_state_ptr[RS_RIGHT_EXTENT_B];
            } else {
                route_state_ptr[RS_ACTIVE_UPPER_BOUND] = route_state_ptr[RS_RIGHT_BOUNDARY_A];
                route_state_ptr[RS_ACTIVE_LOWER_BOUND] = route_state_ptr[RS_RIGHT_BOUNDARY_B];
            }

            /* FIX 2026-05-21: was td5_ai_classify_track_offset_clamp (v1),
             * which produces stale clamp result during countdown that makes
             * the AABB gate reject every peer. v2 is the byte-faithful rewrite
             * already used by td5_ai_update_race_actors.
             * See memory/reference_v1_v2_classify_clamp_fix_2026-05-21.md. */
            classify_result = td5_ai_classify_track_offset_clamp_v2(i, route_state_ptr[RS_TRACK_OFFSET_BIAS]);

            peer_cd = (int16_t *)(intptr_t)ACTOR_I32(self, ACTOR_CAR_DEF_PTR);
            if (!peer_cd) continue;

            if (classify_result == 1) {
                int peer_route_byte = (int)(uint8_t)((const uint8_t *)(intptr_t)peer_rs[RS_ROUTE_TABLE_PTR])[(size_t)peer_field82 * 3u];
                int32_t signed_off = td5_track_compute_signed_offset(
                    (int)peer_field80, 0x100, peer_route_byte);
                classify = (int32_t)peer_cd[0] + signed_off - 0x20;
            } else if (classify_result == 2) {
                int peer_route_byte = (int)(uint8_t)((const uint8_t *)(intptr_t)peer_rs[RS_ROUTE_TABLE_PTR])[(size_t)peer_field82 * 3u];
                int32_t signed_off = td5_track_compute_signed_offset(
                    (int)peer_field80, 0, peer_route_byte);
                classify = (int32_t)peer_cd[4] + signed_off - 0x20;
            } else {
                classify = route_state_ptr[RS_TRACK_OFFSET_BIAS];
            }

            /* Range gate [CONFIRMED @ 0x00433BE6-0x00433C0A]:
             *   bound_lo_q = self_cardef[0] + classify - 0x20
             *   bound_hi_q = self_cardef[8] + classify + 0x20
             *   bound_lo_q <= peer.RS[0x14], peer.RS[0x15] <= bound_hi_q */
            bound_lo_q = (int32_t)peer_cd[0] + classify - 0x20;
            bound_hi_q = (int32_t)peer_cd[4] + classify + 0x20;
            if (bound_lo_q > peer_rs[RS_ACTIVE_LOWER_BOUND]) continue;
            if (peer_rs[RS_ACTIVE_UPPER_BOUND] > bound_hi_q) continue;

            dist = peer_field82 - self_field82;
            if (dist < best_dist) {
                best_dist = dist;
                best_slot = i;
                offset_at_clamp = classify;
            }
        }

        /* Pass 2 finalisation [CONFIRMED @ 0x00433C4B-0x00433CD0]:
         *   -1 < local_c && local_c <= 0x28 && local_8 != piVar3[0x35] */
        if (best_dist > -1 && best_dist <= 0x28 && best_slot != self_slot) {
            int32_t *bp_rs = route_state(best_slot);
            int32_t mid_lo = bp_rs[RS_ACTIVE_LOWER_BOUND];
            int32_t mid_hi = bp_rs[RS_ACTIVE_UPPER_BOUND];
            int32_t mid_sum = mid_hi + mid_lo;
            int32_t mid = mid_sum >> 1;
            int16_t *cd_for_dir = (int16_t *)(intptr_t)ACTOR_I32(self, ACTOR_CAR_DEF_PTR);
            int32_t val_r, val_l, abs_r, abs_l;

            if (cd_for_dir) {
                int32_t cd_lo = (int32_t)cd_for_dir[0];
                int32_t cd_hi = (int32_t)cd_for_dir[4];

                val_r = (cd_hi - mid) + 0x20 + offset_at_clamp;
                val_l = (cd_lo - mid) - 0x20 + offset_at_clamp;
                abs_r = (val_r < 0) ? -val_r : val_r;
                abs_l = (val_l < 0) ? -val_l : val_l;
                /* Same inversion fix as pass-1 finalisation above. */
                g_lateral_avoidance_direction = (abs_l >= abs_r) ? 1 : 0;
            }
            TD5_LOG_I(LOG_TAG,
                      "find_offset_peer: slot=%d peer=%d dist=%d off=%d dir=%d",
                      self_slot, best_slot, best_dist, offset_at_clamp,
                      g_lateral_avoidance_direction);
            return best_slot;
        }
    }

    /* No peer found in either pass: original returns piVar3[0x35] (self_slot).
     * Adapter — UpdateActorTrackOffsetBias treats `peer < 0 || peer == self`
     * as "no peer"; both work. Match listing: return self_slot.
     * The caller's existing branch `if (peer >= 0)` still triggers; the
     * helper distinguishes via `peer != self` semantics elsewhere. To
     * preserve pre-existing port adapter behaviour (peer<0 ⇒ decay path),
     * convert self-return to -1 here.
     *
     * [PORT-DIVERGENCE — caller adapter; intentional, behaviour-equivalent.
     *  See memory/reference_arch_find_offset_peer_return_minus_one.md.
     *  Do not "fix" without auditing every `peer >= 0` check in callers.] */
    return -1;
}

/**
 * UpdateActorTrackOffsetBias — byte-faithful port of 0x00434900.
 *
 * Listing audit (precise-00434900, pool5_00434900):
 *
 *   0x00434900-0x00434923  CALL FindActorTrackOffsetPeer(&rs[slot]).
 *                          Test EAX vs slotIndex: peer == self → no-peer path.
 *
 *   No-peer path (0x00434925-0x0043496c):
 *     bias = rs[RS_TRACK_OFFSET_BIAS]
 *     if (bias < 0) { bias += 8; store; if (bias > 0) bias = 0; store; }
 *     reload bias
 *     if (bias <= 0) return                           // RET via 0x00434a94
 *     bias -= 8; store; if (bias >= 0) return         // RET via 0x00434a94
 *     bias = 0; store; return                         // RET via 0x00434969
 *
 *   Peer path (0x0043496d-0x00434a97):
 *     load direction = DAT_004b08b0
 *     if (direction == 1):                            // 0x00434982/0x0043498d
 *       BL = peer_actor.field_0x8C  (ACTOR_SUB_LANE_INDEX)   [@ 0x004349A0]
 *       if (BL == 0):
 *         direction = 0                                       [@ 0x004349AC]
 *         goto LAB_004349b2 (positive push)
 *       else:
 *         goto common_neg (0x00434a24)
 *     else if (direction == 0):                       // 0x004349e4 TEST ECX,ECX
 *       EDX = peer_actor.field_0x80 (SPAN_RAW) * 3            [@ 0x004349FF]
 *       DL  = g_strip_span_base[EDX*8 + 3]                    [@ 0x00434A09]
 *       EDX &= 0xF
 *       BL = peer_actor.field_0x8C  (SUB_LANE_INDEX)          [@ 0x00434A0D]
 *       if (BL != DL):
 *         goto LAB_004349b2 (positive push)
 *       else:
 *         direction = 1                                       [@ 0x00434A1A]
 *         fall through to common_neg
 *     else:                                          // direction not in {0,1}
 *       goto common_neg (no flip)
 *
 *   LAB_004349b2 (positive push):                     [@ 0x004349B2-0x00434A78]
 *     dist = (int)peer_actor.field_0x82 - (int)self_actor.field_0x82
 *     if (dist > 0x28) dist = 0x28
 *     else if (dist < 1) dist = 1
 *     bias += (0x29 - dist); return
 *
 *   common_neg (negative push):                       [@ 0x00434A24-0x00434A97]
 *     dist = (int)peer_actor.field_0x82 - (int)self_actor.field_0x82
 *     if (dist > 0x28) dist = 0x28
 *     else if (dist < 1) dist = 1
 *     bias += (dist - 0x29); return
 *
 *   Field-offset notes (verified against listing):
 *     0x4ab108 = g_actorRuntimeState slot base
 *     0x4ab188 = base + 0x80  → ACTOR_SPAN_RAW (strip-table index, peer-only)
 *     0x4ab18a = base + 0x82  → ACTOR_SPAN_NORMALIZED (distance compare, both)
 *     0x4ab194 = base + 0x8C  → ACTOR_SUB_LANE_INDEX
 *     0x4afb84 = rs[slot] + 0x84 = RS_TRACK_OFFSET_BIAS
 *     0x004c3d9c = g_strip_span_base (g_trackStripRecords in Ghidra)
 *     0x004b08b0 = g_lateral_avoidance_direction
 *
 *   Strip-record stride is 24 bytes (EDX*8 with EDX = span*3). byte+3 is the
 *   geometry_metadata byte; low nibble is the span lane count (0..15).
 *
 * Divergences from the previous (non-faithful) port:
 *   1. Distance used SPAN_RAW (0x80); listing uses SPAN_NORMALIZED (0x82).
 *      The two fields differ at junction-remap spans; using RAW measured the
 *      wrong distance at boundaries.
 *   2. Strip lane count went through td5_track_get_span_lane_count() which
 *      min-clamps to 1; the listing reads the raw nibble (can be 0). For
 *      byte-faithful equality we read it raw here, with a bounds guard so
 *      the port doesn't fault on an out-of-range peer span.
 *   3. Removed port-only ±0x600 saturation clamp; not present in original.
 *      Prior crashes attributed to clamp removal predate the SPAN_RAW fix
 *      and should now be revisited downstream.
 *   4. Negative-push branch is now reached for direction values outside
 *      {0,1} (no flip), matching the JNZ at 0x004349E6 falling through to
 *      common_neg without touching DAT_004b08b0.
 */
/* Phantom-peer setup (port-only, 2026-05-22 v2).
 *
 * In TD5_d3d.exe 6-racer mode, find_offset_peer reliably returns a real
 * peer every tick, and the peer-found branch oscillates RS_TRACK_OFFSET_BIAS
 * in a narrow band (orig slot 1 Edinburgh spans 95-130: 462-1176, avg 856).
 * The non-zero bias laterally shifts td5_track_sample_target_point's output,
 * keeping target_angle smooth tick-to-tick, keeping the AI cascade in the
 * fine band, and preventing the ±0x18000 saturation spiral that walks the
 * car into walls.
 *
 * In the port's TT+PlayerIsAI solo mode, slots 1..5 are state==3 and their
 * RS_ACTIVE_LOWER/UPPER are never updated (they stay at init = 0). The AABB
 * gate at line ~2567 always rejects them → find_offset_peer returns -1 → the
 * no-peer decay drives bias to 0 → cascade compounds → walls.
 *
 * This helper sets up an inactive slot (slot 1, or slot 2 if SoloAISlot=1)
 * as a phantom peer just before find_offset_peer is called, with all the
 * RS/actor fields the gates+classify+peer-found branch read. The phantom
 * is configured to mirror self's lane, route_table_ptr, and bounds, and
 * sits a small fixed distance ahead. Then the unmodified orig logic
 * produces emergent per-slot bias dynamics — different self route tables
 * (LEFT vs RIGHT) and boundaries yield different val_r/val_l/abs comparisons
 * in find_offset_peer's finalization, which drive the peer-found branch
 * via g_lateral_avoidance_direction.
 *
 * Gated behind --PhantomPeer (default 1). When disabled, the no-peer path
 * falls back to the v1 solo-emulation push cycle (still slot-0 only).
 *
 * Returns the phantom slot index, or -1 if disabled / unable to set up. */
static int td5_ai_setup_phantom_peer_for_solo0(int32_t *self_rs) {
    if (!g_td5.solo_mode_synth) return -1;
    if (!g_td5.ini.phantom_peer) return -1;
    if (self_rs[RS_SLOT_INDEX] != 0) return -1;

    char *self_actor = actor_ptr(0);
    int32_t self_cd = ACTOR_I32(self_actor, ACTOR_CAR_DEF_PTR);
    if (!self_cd) return -1;  /* not yet initialized; no peer */

    /* Phantom slot: prefer slot 1; if SoloAISlot=1, use slot 2 so the user's
     * --SoloAISlot=1 data-swap doesn't end up making the phantom equal self
     * in any other observable. (slot 0 IS self; we can't use it.) */
    int phantom_slot = (g_td5.ini.solo_ai_slot == 1) ? 2 : 1;
    int32_t *phantom_rs = route_state(phantom_slot);
    char *phantom_actor = actor_ptr(phantom_slot);

    /* Disable the other inactive slots as candidates by setting their
     * RS_ROUTE_TABLE_SELECTOR to an impossible value. find_offset_peer
     * rejects on rs[3] mismatch before any other read. We do this each
     * tick because nothing else touches these slots in solo mode. */
    for (int k = 1; k < TD5_MAX_RACER_SLOTS; k++) {
        if (k == phantom_slot) continue;
        route_state(k)[RS_ROUTE_TABLE_SELECTOR] = -1;
    }

    /* Phantom span = self.span + 4 (so dist clamps to 4 → push magnitude
     * 0x29-4 = 37 / 4-0x29 = -37, close to orig's measured ±38). Wrap to
     * span_count. */
    int span_count = g_strip_span_count > 0 ? g_strip_span_count : 1;
    int self_span_norm = (int)(int16_t)ACTOR_I16(self_actor, ACTOR_SPAN_NORMALIZED);
    int self_span_raw  = (int)(int16_t)ACTOR_I16(self_actor, ACTOR_SPAN_RAW);
    int phantom_span_norm = (self_span_norm + 4) % span_count;
    int phantom_span_raw  = (self_span_raw  + 4) % span_count;

    /* RS state — mirror self's lane / route / bounds. find_offset_peer +
     * classify_v2 + peer-found branch read these (route_table_ptr, selector,
     * forward_track_comp, active bounds, route_table boundaries). */
    phantom_rs[RS_ROUTE_TABLE_PTR]      = self_rs[RS_ROUTE_TABLE_PTR];
    phantom_rs[RS_ROUTE_TABLE_SELECTOR] = self_rs[RS_ROUTE_TABLE_SELECTOR];
    phantom_rs[RS_FORWARD_TRACK_COMP]   = self_rs[RS_FORWARD_TRACK_COMP];
    phantom_rs[RS_ACTIVE_LOWER_BOUND]   = self_rs[RS_ACTIVE_LOWER_BOUND];
    phantom_rs[RS_ACTIVE_UPPER_BOUND]   = self_rs[RS_ACTIVE_UPPER_BOUND];
    phantom_rs[RS_LEFT_BOUNDARY_A]      = self_rs[RS_LEFT_BOUNDARY_A];
    phantom_rs[RS_LEFT_BOUNDARY_B]      = self_rs[RS_LEFT_BOUNDARY_B];
    phantom_rs[RS_RIGHT_BOUNDARY_A]     = self_rs[RS_RIGHT_BOUNDARY_A];
    phantom_rs[RS_RIGHT_BOUNDARY_B]     = self_rs[RS_RIGHT_BOUNDARY_B];
    phantom_rs[RS_RIGHT_EXTENT_A]       = self_rs[RS_RIGHT_EXTENT_A];
    phantom_rs[RS_RIGHT_EXTENT_B]       = self_rs[RS_RIGHT_EXTENT_B];
    phantom_rs[RS_TRACK_PROGRESS]       = self_rs[RS_TRACK_PROGRESS];
    phantom_rs[RS_TRACK_OFFSET_BIAS]    = 0;  /* not read in peer-found path */
    phantom_rs[RS_SLOT_INDEX]           = phantom_slot;

    /* Actor state — span fields, sub_lane, cardef. The peer-found branch
     * reads SPAN_NORMALIZED (dist), SPAN_RAW (strip nibble lookup when
     * direction==0), and SUB_LANE_INDEX (direction-flip / push-sign tests).
     * classify_v2 reads peer's CAR_DEF_PTR — use self's. */
    ACTOR_I16(phantom_actor, ACTOR_SPAN_NORMALIZED) = (int16_t)phantom_span_norm;
    ACTOR_I16(phantom_actor, ACTOR_SPAN_RAW)        = (int16_t)phantom_span_raw;
    ACTOR_I32(phantom_actor, ACTOR_CAR_DEF_PTR)     = self_cd;

    /* SUB_LANE_INDEX — set to self's sub_lane so the phantom appears
     * "co-lane" with self. The peer-found branch's direction logic:
     *   - direction==1 && peer.sub_lane==0 → flip dir=0 + positive push
     *   - direction==1 && peer.sub_lane!=0 → negative push (no flip)
     *   - direction==0 && peer.sub_lane!=strip_nibble → positive push
     *   - direction==0 && peer.sub_lane==strip_nibble → flip dir=1 + neg push
     * direction is overwritten each tick by find_offset_peer's geometry-
     * derived finalization (abs_l vs abs_r). So the bias dynamics emerge
     * from self's route/bounds geometry through the unmodified orig path. */
    uint8_t self_sub_lane = ACTOR_U8(self_actor, ACTOR_SUB_LANE_INDEX);
    ACTOR_U8(phantom_actor, ACTOR_SUB_LANE_INDEX) = self_sub_lane;

    return phantom_slot;
}

void td5_ai_update_track_offset_bias(int slot) {
    int32_t *rs = route_state(slot);

    /* Solo-mode phantom-peer setup (port-only, 2026-05-22 v2). Runs ONCE
     * before find_offset_peer so the unmodified orig peer-found logic
     * produces emergent per-slot bias dynamics. See helper comment. */
    (void)td5_ai_setup_phantom_peer_for_solo0(rs);

    int peer = td5_ai_find_offset_peer(rs);

    if (peer < 0) {
        /* Phantom-peer fallback: when --PhantomPeer=0 (or setup couldn't
         * find a valid self car_def_ptr), use the v1 push cycle so solo
         * mode still escapes the no-peer decay-to-zero trap. Slot-0-only,
         * solo_mode_synth-only — does NOT affect 6-racer mode. */
        if (g_td5.solo_mode_synth && slot == 0) {
            /* Solo-mode peer-bias emulation v1 (kept as fallback when
             * phantom_peer is disabled or fails). Frida orig 6-racer slot 1
             * spans 95-130 measured bias hovering at ~1000 via cyclic peer
             * pushes (+38 every ~5 ticks + ∓8 decay). v1 emulates that cycle
             * in the bias's prevailing sign. */
            static uint32_t s_solo_bias_tick = 0;
            int32_t bias = rs[RS_TRACK_OFFSET_BIAS];
            s_solo_bias_tick++;
            int do_push = ((s_solo_bias_tick % 5u) == 0u);
            int simulated_parity = (g_td5.ini.solo_ai_slot >= 0 &&
                                    g_td5.ini.solo_ai_slot < TD5_MAX_RACER_SLOTS)
                                   ? g_td5.ini.solo_ai_slot : 0;
            int default_sign = (simulated_parity & 1) ? 1 : -1;
            int sign = (bias > 16) ? 1 : (bias < -16 ? -1 : default_sign);

            if (do_push) {
                bias += sign * 38;
            } else {
                /* Standard ±8 zero-crossing decay (orig listing path) */
                if (bias < 0) {
                    bias += 8;
                    if (bias > 0) bias = 0;
                } else if (bias > 0) {
                    bias -= 8;
                    if (bias < 0) bias = 0;
                }
            }
            rs[RS_TRACK_OFFSET_BIAS] = bias;
            return;
        }

        /* No-peer path [0x00434925-0x0043496c]. Two-stage zero-crossing
         * decay: if bias <0, +=8 then clamp-at-zero; if (still) >0, -=8
         * then clamp-at-zero. Each store/reload mirrors the listing. */
        int32_t bias = rs[RS_TRACK_OFFSET_BIAS];
        if (bias < 0) {
            bias += 8;
            rs[RS_TRACK_OFFSET_BIAS] = bias;
            if (bias > 0) {
                rs[RS_TRACK_OFFSET_BIAS] = 0;
            }
        }
        bias = rs[RS_TRACK_OFFSET_BIAS];
        if (bias <= 0) {
            return; /* RET @ 0x00434a94 via JLE at 0x0043494c */
        }
        bias -= 8;
        rs[RS_TRACK_OFFSET_BIAS] = bias;
        if (bias >= 0) {
            return; /* RET @ 0x00434a94 via JGE at 0x0043495d */
        }
        rs[RS_TRACK_OFFSET_BIAS] = 0;
        return;
    }

    /* Peer-found path [0x0043496d-0x00434a97]. */
    {
        char *self_actor = actor_ptr(slot);
        char *peer_actor = actor_ptr(peer);
        int  do_positive;
        int32_t dist;
        int32_t direction = g_lateral_avoidance_direction;
        uint8_t peer_sub_lane = ACTOR_U8(peer_actor, ACTOR_SUB_LANE_INDEX);

        if (direction == 1) {
            /* [0x00434982-0x004349AA]: if peer.SUB_LANE_INDEX == 0 then flip
             * direction to 0 and take positive push; else take negative push
             * (no flip). */
            if (peer_sub_lane == 0) {
                g_lateral_avoidance_direction = 0;
                do_positive = 1;
            } else {
                do_positive = 0;
            }
        } else if (direction == 0) {
            /* [0x004349E8-0x00434A1A]: read peer's strip nibble using
             * peer.SPAN_RAW (field_0x80); compare to peer.SUB_LANE_INDEX.
             * Mismatch → positive push (no flip). Match → set direction=1
             * and negative push. */
            uint8_t strip_nibble = 0;
            int16_t peer_span_raw = ACTOR_I16(peer_actor, ACTOR_SPAN_RAW);
            const uint8_t *strips = (const uint8_t *)g_strip_span_base;
            /* PORT-ONLY NULL GUARD (KEEP): original 0x00434A09 loads
             * g_trackStripRecords + peer.SPAN_RAW*0x18 + 3 unconditionally.
             * Upstream invariant in original: STRIP.DAT is parsed before
             * any actor is alive (LoadStripFile @ 0x004444A0 from race
             * init). Port may run AI from menu/benchmark probe paths
             * before strips are bound. Once `strips != NULL` the
             * peer.SPAN_RAW index is in-range by the FindActorTrackOffsetPeer
             * contract (peer is an active actor whose SPAN_RAW was set
             * by InitActorTrackSegmentPlacement). Bounds check REMOVED
             * (was port-only insurance) for byte-faithfulness — original
             * has no clamp and trusts the peer-actor invariant. */
            if (strips) {
                int strip_idx = (int)peer_span_raw;
                strip_nibble = strips[strip_idx * 0x18 + 3] & 0x0F;
            }
            if (peer_sub_lane != strip_nibble) {
                do_positive = 1;
            } else {
                g_lateral_avoidance_direction = 1;
                do_positive = 0;
            }
        } else {
            /* [0x004349E4-0x004349E6]: direction not in {0,1} falls through
             * to common_neg without touching DAT_004b08b0. */
            do_positive = 0;
        }

        /* Distance from SPAN_NORMALIZED [0x004349C0-0x004349CF, 0x00434A3B-0x00434A50].
         * Both branches use field_0x82 (signed 16-bit). */
        {
            int32_t self_sn = (int32_t)ACTOR_I16(self_actor, ACTOR_SPAN_NORMALIZED);
            int32_t peer_sn = (int32_t)ACTOR_I16(peer_actor, ACTOR_SPAN_NORMALIZED);
            dist = peer_sn - self_sn;
        }

        /* Clamp dist to [1, 0x28]; listing orders the test as
         *   if (dist > 0x28) dist = 0x28; else if (dist < 1) dist = 1;
         * The else-if order matters only for unreachable dist values, but
         * is preserved here for clarity. */
        if (dist > 0x28) {
            dist = 0x28;
        } else if (dist < 1) {
            dist = 1;
        }

        if (do_positive) {
            /* Branch A [0x00434A68-0x00434A75]: bias += (0x29 - dist). */
            rs[RS_TRACK_OFFSET_BIAS] += (0x29 - dist);
            TD5_LOG_I(LOG_TAG,
                      "offset_bias: slot=%d pos push=%d dir=%d sub_lane=%u bias=%d",
                      slot, (0x29 - dist), direction, peer_sub_lane,
                      rs[RS_TRACK_OFFSET_BIAS]);
        } else {
            /* Branch B [0x00434A83-0x00434A8E]: bias += (dist - 0x29). */
            rs[RS_TRACK_OFFSET_BIAS] += (dist - 0x29);
            TD5_LOG_I(LOG_TAG,
                      "offset_bias: slot=%d neg push=%d dir=%d sub_lane=%u bias=%d",
                      slot, (dist - 0x29), direction, peer_sub_lane,
                      rs[RS_TRACK_OFFSET_BIAS]);
        }
    }
}

/* ========================================================================
 * RefreshActorTrackProgressOffset (0x004342E0)
 *
 * Seeds RS_TRACK_PROGRESS and RS_TRACK_OFFSET_BIAS for a racer slot at spawn.
 * Reads actor's current span (field_0x80 = ACTOR_SPAN_RAW) and world position
 * (field_0x1FC/0x204 = ACTOR_WORLD_POS_X/Z), computes 8-bit normalised progress
 * along the span via ComputeTrackSpanProgress, then computes the signed lateral
 * offset from the route byte via ComputeSignedTrackOffset.
 * [CONFIRMED @ 0x004342E0]
 * ======================================================================== */
void td5_ai_seed_actor_track_progress_offset(int slot)
{
    int32_t *rs = route_state(slot);
    char *actor = actor_ptr(slot);
    int span_raw  = (int)(int16_t)ACTOR_I16(actor, ACTOR_SPAN_RAW);
    int span_norm = (int)(int16_t)ACTOR_I16(actor, ACTOR_SPAN_NORMALIZED);

    /* Actor position as int32[3] with [0]=world_x (24.8), [2]=world_z (24.8)
     * [CONFIRMED @ 0x004342F0: param_2 = &field_0x1fc of actor slot] */
    int32_t actor_pos[3];
    actor_pos[0] = ACTOR_I32(actor, ACTOR_WORLD_POS_X);
    actor_pos[2] = ACTOR_I32(actor, ACTOR_WORLD_POS_Z);

    int64_t progress64 = td5_track_compute_span_progress(span_raw, actor_pos);
    int progress = (int)(int32_t)(uint32_t)(progress64 & 0xFFFFFFFF);

    /* Store progress [CONFIRMED @ 0x00434313: gActorTrackSpanProgress[slot*0x47]] */
    rs[RS_TRACK_PROGRESS] = progress;

    /* Route byte from route table at span_norm * 3 [CONFIRMED @ 0x00434327].
     *
     * BUT — at seed time during InitializeRaceSession, the ORIGINAL reads
     * actor->field_0x82 (span_norm) BEFORE it has been populated from the
     * spawn world position. The field still holds its zero/uninitialized
     * value. The port had this field set before this seed runs, causing
     * port to read route_table[286*3]=76 while original reads
     * route_table[0*3]=106 (Frida-confirmed: all 6 slots get
     * route_byte=106 during init).
     *
     * Result of port bug: port's seed for slot 0 produces bias=+443
     * (progress=95 > route_byte=76 → positive); original produces
     * bias=-297 (progress=95 < route_byte=106 → negative). Same sign-flip
     * for slots 2 and 4. The wrong-signed bias shifts the AI's look-ahead
     * target to the wrong side, producing the visible "AI steers slightly
     * left into the wall" symptom on Moscow.
     *
     * Faithful port: read route_table[0] (matches original's
     * uninitialized-span_norm read at seed time).
     * [Frida-confirmed 2026-05-11: 36 init-phase ComputeSignedTrackOffset
     * calls all pass route_byte=106 regardless of span_raw {277..292};
     * port's per-span route_byte 75-79 was the bug.] */
    const uint8_t *route_bytes = (const uint8_t *)(intptr_t)rs[RS_ROUTE_TABLE_PTR];
    int route_byte = 0;
    if (route_bytes) {
        route_byte = (int)route_bytes[0];
    }
    (void)span_norm;

    /* Store signed lateral offset [CONFIRMED @ 0x00434332: DAT_004afb84[slot*0x47]] */
    {
        int32_t bias = td5_track_compute_signed_offset(span_raw, progress, route_byte);
        /* Route byte coordinate space is 0=right, 255=left (right_vertex_index base).
         * Route bytes at Moscow spawn spans are 97-145, all less than the actors'
         * lateral progress ~160 — yielding ALL-positive computed biases regardless of
         * route assignment.  The zero-bounds abs-compare in FindActorTrackOffsetPeer
         * maps positive-bias → direction=0 → positive push, negative → direction=1 →
         * negative push.  To get divergence, selector=0 (LEFT.TRK) cars need a
         * NEGATIVE starting bias so the peer-avoidance drives them leftward. */
        /* Bias stored AS-IS, matching original `ComputeSignedTrackOffset @ 0x00434670`
         * which returns the value without negation. The previous port-only
         * `if (selector == 0) bias = -bias;` was a lane-spread enhancement that
         * diverged from the original and produced a sign-flipped lateral target on
         * Moscow TT slot 0 (port dx=-2097 vs original dx=+4722 confirmed via Frida
         * target_probe). Combined with the corrected tangent direction in
         * td5_track_sample_target_point, this restores parity. */
        rs[RS_TRACK_OFFSET_BIAS] = bias;
    }

    TD5_LOG_I("ai", "seed: slot=%d sel=%d span_raw=%d span_norm=%d progress=%d route_byte=%d bias=%d",
              slot, (int)rs[RS_ROUTE_TABLE_SELECTOR], span_raw, span_norm,
              progress, route_byte, (int)rs[RS_TRACK_OFFSET_BIAS]);
}

/* ========================================================================
 * Script VM  (0x4370A0  AdvanceActorTrackScript)
 *
 * 12-opcode bytecode interpreter. Per-actor script state is stored in the
 * route state array. Scripts are executed one opcode per tick, with some
 * opcodes blocking (returning 0) until a condition is met.
 *
 * Returns: 0 = script still running (blocking), 1 = script complete/reset
 * ======================================================================== */


int td5_ai_advance_track_script(int *rs) {
    /* Verbatim port of AdvanceActorTrackScript @ 0x004370A0.
     * Order of operations matches the original byte-for-byte:
     *   1. Countdown decrement + program rotation (A<->B, C<->D)
     *   2. Flag 0x04 XOR Flag 0x08 (mutually exclusive)
     *      ALIGNED branch: recompute LEFT/RIGHT_DEVIATION from combined
     *      (steer_cmd + yaw_accum) and call UpdateActorSteeringBias(0x4000)
     *   3. Flag 0x10 release-or-maintain — returns 0 unconditionally
     *   4. Flag 0x02 brake/accel until speed threshold
     *   5. Opcode switch (cases 0..11)
     *
     * The previous port handled flags in inverse order (0x10 → 0x02 →
     * 0x04 → 0x08 → countdown → switch), skipped the LAB_004372cf
     * deviation recompute, treated flag 4/8 independently, used bilateral
     * aligned-tests, mishandled flag-2 threshold (4× off), did unconditional
     * case-0 terminate, and never zeroed angular_velocity_yaw on terminate.
     * All of those drove yaw rotation away from original. */
    int slot = rs[RS_SLOT_INDEX];
    char *actor = actor_ptr(slot);


    /* ==== 1. Countdown decrement + program rotation [orig prologue] ==== */
    rs[RS_SCRIPT_COUNTDOWN]--;
    if (rs[RS_SCRIPT_COUNTDOWN] < 0) {
        intptr_t cur = (intptr_t)rs[RS_SCRIPT_BASE_PTR];
        intptr_t next = cur;
        if (cur == (intptr_t)g_script_program_a) {
            next = (intptr_t)g_script_program_b;
        } else if (cur == (intptr_t)g_script_program_b) {
            next = (intptr_t)g_script_program_a;
        } else if (cur == (intptr_t)g_script_program_c) {
            next = (intptr_t)g_script_program_d;
        } else if (cur == (intptr_t)g_script_program_d) {
            next = (intptr_t)g_script_program_c;
        }
        /* Other base_ptr values (uninitialized / DAT_00473cc8 initial
         * recovery) leave base/ip alone — only countdown resets. */
        if (next != cur) {
            rs[RS_SCRIPT_BASE_PTR] = (int32_t)next;
            rs[RS_SCRIPT_IP]       = 0;
        }
        rs[RS_SCRIPT_COUNTDOWN] = 0x96;
    }

    /* ==== 2. Flag 0x04 / Flag 0x08 — MUTUALLY EXCLUSIVE in orig ==== */
    int32_t flags        = rs[RS_SCRIPT_FLAGS];
    int32_t heading      = ACTOR_I32(actor, ACTOR_YAW_ACCUM) >> 8;
    int32_t route_heading = ai_route_heading_for_actor(rs, actor);
    int32_t hdelta_raw   = (heading - route_heading) & 0xFFF;
    int32_t hdelta_mirror = (int32_t)((-(int32_t)hdelta_raw) & 0xFFF);
    int aligned_finalize = 0;

    if ((flags & 0x04) == 0) {
        if (flags & 0x08) {
            /* Flag-8 aligned test: orig `0xDFF < mirror`. */
            if (hdelta_mirror > 0xDFF) {
                if ((int32_t)rs[RS_SCRIPT_OFFSET_PARAM] < 0) {
                    ACTOR_I32(actor, ACTOR_STEERING_CMD) = 0;
                    rs[RS_SCRIPT_FLAGS] ^= 0x08;
                }
                aligned_finalize = 1;
            } else {
                int32_t sc = ACTOR_I32(actor, ACTOR_STEERING_CMD) - 0x4000;
                if (sc < -0x19000) sc = -0x19000;
                ACTOR_I32(actor, ACTOR_STEERING_CMD) = sc;
            }
        }
    } else {
        /* Flag-4 aligned test: listing 0x004371a0-a5 tests `EAX = hd_mir`
         * (CMP EAX,0x200 / JLE → aligned). Sibling flag-8 test above uses
         * `hdelta_mirror > 0xDFF` (listing 0x0043726b-70). Port previously
         * used `hdelta_raw < 0x201`; that fires on +small bias instead of
         * the +/-small bias the mirror semantics require. pool11 D1 fix. */
        if (hdelta_mirror < 0x201) {
            if ((int32_t)rs[RS_SCRIPT_OFFSET_PARAM] < 0) {
                ACTOR_I32(actor, ACTOR_STEERING_CMD) = 0;
                rs[RS_SCRIPT_FLAGS] ^= 0x04;
            }
            aligned_finalize = 1;
        } else {
            int32_t sc = ACTOR_I32(actor, ACTOR_STEERING_CMD) + 0x4000;
            if (sc > 0x19000) sc = 0x19000;
            ACTOR_I32(actor, ACTOR_STEERING_CMD) = sc;
        }
    }

    if (aligned_finalize) {
        /* LAB_004372cf — recompute RS_LEFT/RIGHT_DEVIATION from combined
         * heading (steer_cmd + yaw_accum) and call steering-bias with
         * weight 0x4000 (script-release path). Original formula:
         *   dev = -((((combined>>8 - target) - 0x800) & 0xFFF) - 0x800) & 0xFFF) & 0xFFF
         * Flow continues to flag-0x10 / flag-0x02 / opcode switch. */
        int32_t combined = ACTOR_I32(actor, ACTOR_STEERING_CMD)
                         + ACTOR_I32(actor, ACTOR_YAW_ACCUM);
        int32_t combined_h = combined >> 8;
        uint32_t d0 = ((uint32_t)(combined_h - route_heading) - 0x800U) & 0xFFF;
        uint32_t d1 = (d0 - 0x800U) & 0xFFF;
        uint32_t dev = ((uint32_t)(-(int32_t)d1)) & 0xFFF;
        rs[RS_LEFT_DEVIATION]  = (int32_t)dev;
        rs[RS_RIGHT_DEVIATION] = (int32_t)(0xFFF - dev);
        td5_ai_update_steering_bias(rs, 0x4000);
    }

    /* Re-fetch flags (flag 4/8 path may have toggled bits). */
    flags = rs[RS_SCRIPT_FLAGS];

    /* ==== 3. Flag 0x10 — stop-and-wait; orig returns 0 on BOTH branches.
     *
     * Reverse-aware extension (port-only, 2026-05-22): when the car is moving
     * BACKWARD (lspd < -0x100), the orig path applies encounter_steer=0xFF00
     * (reverse throttle) + brake_flag=1. That combination stops a forward-
     * moving car, but for a car already reversing it keeps the reverse
     * throttle on while braking — net effect is the car stays in reverse.
     *
     * Edinburgh TT solo PlayerIsAI=1 hit this: AI hits wall → wall response
     * inverts lspd → script fires recovery → flag-0x10 brake never reaches
     * the (-0x100, 0x100) release band → cascade saturates after script
     * times out → loop. Visible as "AI very unstable" with car spending
     * 80%+ of ticks at the ±0x18000 steering clamp.
     *
     * Fix: when lspd is meaningfully negative, apply FORWARD throttle
     * (0x00FF) so brake_flag+throttle work together to decelerate the
     * reverse motion. Once lspd crosses into the near-zero band, the
     * standard release path takes over.
     *
     * The orig was never tested with PlayerIsAI=1 (slot 0 is always
     * human-driven), and the orig's 6-racer AI rarely got into deep reverse
     * because peer-mediated bias damping kept cars off walls. */
    if (flags & 0x10) {
        int32_t lspd = ACTOR_I32(actor, ACTOR_LONGITUDINAL_SPEED);
        if (lspd < 0x100 && lspd > -0x100) {
            rs[RS_SCRIPT_FLAGS] = flags ^ 0x10;
            ACTOR_U8(actor, ACTOR_BRAKE_FLAG) = 0;
            ACTOR_I16(actor, ACTOR_ENCOUNTER_STEER) = 0;
            return 0;
        }
        if (lspd <= -0x100) {
            /* Reversing — forward throttle to overcome the reverse motion. */
            ACTOR_I16(actor, ACTOR_ENCOUNTER_STEER) = (int16_t)0x00FF;
        } else {
            /* Forward (lspd >= 0x100) — orig behavior. */
            ACTOR_I16(actor, ACTOR_ENCOUNTER_STEER) = (int16_t)0xFF00;
        }
        ACTOR_U8(actor, ACTOR_BRAKE_FLAG) = 1;
        return 0;
    }

    /* ==== 4. Flag 0x02 — brake/accel until speed threshold.
     * Orig always writes brake_flag=0 at entry; release zeroes encounter_steer
     * but KEEPS flag set; threshold scale is `(op * 0x40308) >> 8` with
     * sign-rounding (4× the previous port's `<< 8` shorthand). */
    if (flags & 0x02) {
        ACTOR_U8(actor, ACTOR_BRAKE_FLAG) = 0;
        int32_t lspd = ACTOR_I32(actor, ACTOR_LONGITUDINAL_SPEED);
        int32_t op   = rs[RS_SCRIPT_OFFSET_PARAM];
        int32_t prod = op * 0x40308;
        int32_t thr  = (prod + ((prod >> 31) & 0xFF)) >> 8;
        if (op < 1) {
            ACTOR_I16(actor, ACTOR_ENCOUNTER_STEER) = (int16_t)0xFF00;
            if (lspd < thr) {
                ACTOR_I16(actor, ACTOR_ENCOUNTER_STEER) = 0;
            }
        } else {
            ACTOR_I16(actor, ACTOR_ENCOUNTER_STEER) = (int16_t)0x00FF;
            if (thr < lspd) {
                ACTOR_I16(actor, ACTOR_ENCOUNTER_STEER) = 0;
            }
        }
    }

    /* ==== 5. Opcode switch ==== */
    const int32_t *base = (const int32_t *)(intptr_t)rs[RS_SCRIPT_BASE_PTR];
    if (!base) return 1;
    int ip = rs[RS_SCRIPT_IP];
    int32_t opcode = base[ip];

    if (g_last_logged_opcode[slot] != opcode) {
        TD5_LOG_I(LOG_TAG, "Script opcode change: slot=%d ip=%d opcode=%d flags=0x%X",
                  slot, ip, opcode, rs[RS_SCRIPT_FLAGS]);
        g_last_logged_opcode[slot] = opcode;
    }

    switch (opcode) {
    case 0: {
        /* Mid-band reject: stay in script if heading not aligned within ±0x40
         * of either forward (0) or reverse (0x800). On actual terminate,
         * ZERO angular_velocity_yaw (+0x1C4). */
        uint32_t a0 = ((uint32_t)hdelta_raw - 0x800U) & 0xFFF;
        uint32_t b0 = (a0 - 0x800U) & 0xFFF;
        uint32_t hd_mir = ((uint32_t)(-(int32_t)b0)) & 0xFFF;
        if (hd_mir > 0x3F && hd_mir < 0xFC1) {
            return 0;
        }
        if ((int32_t)rs[RS_SCRIPT_OFFSET_PARAM] < 0) {
            ACTOR_I32(actor, ACTOR_STEERING_CMD) = 0;
        }
        rs[RS_SCRIPT_BASE_PTR] = 0;
        rs[RS_SCRIPT_FLAGS]    = 0;
        rs[RS_SCRIPT_FIELD_3E] = 0;
        rs[RS_SCRIPT_FIELD_43] = 0;
        ACTOR_I32(actor, ACTOR_STEERING_CMD) = 0;
        ACTOR_I32(actor, 0x1C4) = 0; /* angular_velocity_yaw */
        return 1;
    }

    case 1:
        rs[RS_SCRIPT_SPEED_PARAM] = base[ip + 1];
        rs[RS_SCRIPT_FLAGS] |= 0x01;
        rs[RS_SCRIPT_IP] = ip + 2;
        return 0;

    case 2:
        rs[RS_SCRIPT_FLAGS] |= 0x02;
        rs[RS_SCRIPT_OFFSET_PARAM] = base[ip + 1];
        rs[RS_SCRIPT_IP] = ip + 2;
        return 0;

    case 3:
        rs[RS_SCRIPT_FLAGS] |= base[ip + 1];
        rs[RS_SCRIPT_IP] = ip + 2;
        return 0;

    case 4:
        rs[RS_SCRIPT_FLAGS] &= ~base[ip + 1];
        rs[RS_SCRIPT_IP] = ip + 2;
        return 0;

    case 5:
        rs[RS_SCRIPT_FLAGS] |= 0x04;
        rs[RS_SCRIPT_IP] = ip + 1;
        return 0;

    case 6:
        rs[RS_SCRIPT_FLAGS] |= 0x08;
        rs[RS_SCRIPT_IP] = ip + 1;
        return 0;

    case 7:
        ACTOR_I16(actor, ACTOR_ENCOUNTER_STEER) = (int16_t)0xFF00;
        ACTOR_U8(actor, ACTOR_BRAKE_FLAG) = 1;
        rs[RS_SCRIPT_IP] = ip + 1;
        return 0;

    case 8:
        /* Orig only sets flag 0x10; brake is applied by the flag-0x10
         * block next tick. */
        rs[RS_SCRIPT_FLAGS] |= 0x10;
        rs[RS_SCRIPT_IP] = ip + 1;
        return 0;

    case 9: {
        /* Auto-select program from MIRRORED hdelta + strip-half-lane.
         * Strip byte at [strip_base + span_raw*0x18 + 3] >> 1 & 7. */
        rs[RS_SCRIPT_COUNTDOWN] = 0xFA;
        uint32_t hd9 = (uint32_t)hdelta_mirror;
        int16_t span_raw = ACTOR_I16(actor, ACTOR_SPAN_RAW);
        int8_t strip_half = 0;
        if (g_strip_span_base && span_raw >= 0 && (int32_t)span_raw < g_strip_span_count) {
            const uint8_t *rec = (const uint8_t *)g_strip_span_base
                               + (uint32_t)span_raw * 0x18;
            strip_half = (int8_t)((rec[3] >> 1) & 7);
        }
        int8_t sub_lane = (int8_t)ACTOR_U8(actor, ACTOR_SUB_LANE_INDEX);

        const int32_t *sel = g_script_program_d;
        int chosen = 0;
        if (hd9 > 0x900 && hd9 < 0xF00 && sub_lane <= strip_half) {
            sel = g_script_program_a;
            chosen = 1;
        }
        if (!chosen && hd9 > 0x6FF) {
            if (strip_half < sub_lane) {
                sel = g_script_program_b;
                chosen = 1;
            } else if (hd9 > 0x700) {
                sel = g_script_program_d;
                chosen = 1;
            }
        }
        if (!chosen) {
            if (hd9 > 0xFF && strip_half <= sub_lane) {
                sel = g_script_program_c;
            } else {
                sel = g_script_program_d;
            }
        }
        rs[RS_SCRIPT_BASE_PTR] = (int32_t)(intptr_t)sel;
        rs[RS_SCRIPT_IP] = 0;
        return 0;
    }

    case 10:
        rs[RS_SCRIPT_FLAGS] |= 0x40;
        rs[RS_SCRIPT_IP] = ip + 1;
        return 0;

    case 11:
        rs[RS_SCRIPT_FLAGS] |= 0x80;
        rs[RS_SCRIPT_IP] = ip + 1;
        return 0;

    default:
        return 0;
    }
}

/* ========================================================================
 * Actor Track Behavior  (0x434FE0  UpdateActorTrackBehavior)
 *
 * Main AI path-following tick for non-player racers and the encounter actor.
 * ======================================================================== */

/* Forward decl for the pool9_00434FE0 pilot trace (precise-port workflow). */

/* ========================================================================
 * SMART OPPONENT AI OVERHAUL  (non-faithful; [GameOptions] SmartAI, default 1)
 *
 * A from-scratch decision brain for the racing opponents AND background
 * traffic. It does NOT replace the tuned execution layer (waypoint sampling →
 * LEFT/RIGHT deviation → UpdateActorSteeringBias cascade → bicycle physics) —
 * it only computes smarter DECISIONS and feeds them through the SAME levers
 * the faithful code uses:
 *
 *   - lane choice         -> RS_TRACK_OFFSET_BIAS (perpendicular target shift)
 *   - branch choice       -> folded into the lane target near a fork
 *   - speed / following   -> post-modulates ENCOUNTER_STEER / BRAKE_FLAG
 *   - catch-up leash       -> gentle symmetric override in ComputeAIRubberBand
 *   - difficulty          -> a continuous per-car `skill` scales competence
 *
 * Everything here is gated by td5_ai_smart_active(); with SmartAI=0 the AI is
 * byte-faithful to the original. Cop-chase (wanted) and drag modes keep their
 * faithful behaviour. Geometry is read through the existing public track
 * helpers + the raw strip records (g_strip_span_base), so td5_track.c is
 * untouched.
 *
 * Units: lateral position is expressed as `u` in [0,1] across the road
 * (0 = left rail, 1 = right rail). A RS_TRACK_OFFSET_BIAS value is a
 * perpendicular shift in raw track-distance units along the left->right rail
 * axis — exactly what td5_track_sample_target_point consumes — so a target u
 * is reached with bias = (u - u_routeline) * road_width_track.
 * ======================================================================== */

#define SMART_MAX_LANES            32
#define SMART_LANE_FALLBACK_WIDTH  256.0  /* track units if rail geometry missing */

static float   g_smart_skill[TD5_MAX_TOTAL_ACTORS];       /* 0..1 competence */
static int8_t  g_smart_branch_pref[TD5_MAX_TOTAL_ACTORS]; /* -1/+1 per-car tie-break */
static double  g_smart_branch_pull[TD5_MAX_TOTAL_ACTORS]; /* cached fork pull -1..+1 */
static double  g_smart_lane_u[TD5_MAX_TOTAL_ACTORS];      /* smoothed target lateral u (road fraction); -1 = uninit */
static int8_t  g_smart_target_lane[TD5_MAX_TOTAL_ACTORS]; /* COMMITTED lane index; -1 = uninit */
static int16_t g_smart_lane_dwell[TD5_MAX_TOTAL_ACTORS];  /* ticks since last committed lane switch */
static int     g_smart_inited = 0;

/* [task#16 2026-06-15] AI branch pre-commitment. When a fork (type-8 fwd / type-11
 * bwd junction span) comes into range the car ROLLS ONCE which way to go and locks
 * the decision until it passes the fork, so it confidently commits to a branch
 * instead of re-deciding every tick and defaulting to the main road (the
 * "AI avoids branches" symptom). g_smart_branch_commit_span[slot] is the fork span
 * the decision is locked to (-1 = no live commitment); _take is 1 to take the
 * branch (high sub-lanes), 0 to hold the main road (low sub-lanes). */
static int16_t g_smart_branch_commit_span[TD5_MAX_TOTAL_ACTORS];
static int8_t  g_smart_branch_commit_take[TD5_MAX_TOTAL_ACTORS];

/* [task#16 netplay determinism 2026-06-15] Private per-car RNG for the branch
 * coin-flip. Seeded at race init from the REPLICATED race seed (decorrelated +
 * mixed with the car's slot index) and advanced ONLY on a sim-tick branch
 * commit — NEVER from the shared CRT rand(), whose stream is also drawn at
 * render rate (td5_sound pitch, td5_vfx particles, td5_camera timers) and so is
 * NOT bit-identical across lockstep peers running at different frame rates. The
 * dynamic-traffic spawner uses the same private-LCG technique (s_trf_dyn_rng /
 * trf_dyn_rand below). Each peer + replay derives the SAME branch choices. */
static uint32_t g_smart_branch_rng[TD5_MAX_TOTAL_ACTORS];

/* Numerical Recipes LCG step; the top bits are the usable (well-mixed) ones. */
static inline uint32_t smart_branch_rand(int slot) {
    g_smart_branch_rng[slot] = g_smart_branch_rng[slot] * 1664525u + 1013904223u;
    return g_smart_branch_rng[slot] >> 16;
}

static inline int smart_iabs(int x) { return x < 0 ? -x : x; }

/* Deterministic integer hash (no runtime RNG — reproducible across replays). */
static inline uint32_t smart_hash_u32(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352dU;
    x ^= x >> 15; x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

/* Race-level master gate: on, and not in a faithful-only mode. */
static int td5_ai_smart_active(void) {
    return g_td5.ini.smart_ai
        && !g_td5.drag_race_enabled
        && !g_td5.wanted_mode_enabled;
}

static float td5_ai_smart_skill(int slot) {
    if (!g_smart_inited || slot < 0 || slot >= TD5_MAX_TOTAL_ACTORS)
        return 0.6f;
    return g_smart_skill[slot];
}

/* Per-race: derive a continuous skill from the difficulty tier plus a small,
 * deterministic per-car spread so the field isn't uniform (some opponents are
 * genuinely faster/cleaner than others). */
static void td5_ai_smart_race_init(void) {
    int tier = g_td5.difficulty_tier;
    float base = (tier <= 0) ? 0.42f : (tier == 1 ? 0.63f : 0.86f);
    /* [task#16] Decorrelate the replicated race seed (cf. td5_game_assign_wheel_
     * styles) for the private per-car branch RNG. Identical on every peer/replay
     * because td5_game_get_race_seed() returns the host-broadcast session seed. */
    uint32_t branch_seed_base = td5_game_get_race_seed() ^ 0x9E3779B9u;
    for (int s = 0; s < TD5_MAX_TOTAL_ACTORS; s++) {
        /* [CUP TEAM SELECT 2026-06-25] Per-opponent difficulty: when an MP cup
         * host set this AI slot's skill on CHOOSE YOUR TEAM, derive THIS car's
         * base competence from its own tier instead of the global one, so a
         * mixed-skill field actually drives at the assigned levels. -1 (the
         * default / human slots) keeps the global tier. */
        int car_tier = tier;
        int per_slot = td5_game_mp_cup_ai_difficulty(s);
        if (per_slot >= 0 && per_slot <= 2) car_tier = per_slot;
        float car_base = (car_tier <= 0) ? 0.42f : (car_tier == 1 ? 0.63f : 0.86f);
        uint32_t h = smart_hash_u32((uint32_t)s * 2654435761u
                                    + (uint32_t)(car_tier + 1) * 40503u);
        float spread = ((float)(h & 0xFFFF) / 65535.0f - 0.5f) * 0.24f; /* +-0.12 */
        float sk = car_base + spread;
        if (sk < 0.12f) sk = 0.12f;
        if (sk > 0.97f) sk = 0.97f;
        g_smart_skill[s] = sk;
        g_smart_branch_pref[s] = (int8_t)(((h >> 17) & 1u) ? 1 : -1);
        g_smart_branch_pull[s] = 0.0;
        g_smart_lane_u[s] = -1.0;   /* snap to the car's actual u on first sight */
        g_smart_target_lane[s] = -1;
        g_smart_lane_dwell[s] = 0;
        g_smart_branch_commit_span[s] = -1;   /* [task#16] no live fork commitment */
        g_smart_branch_commit_take[s] = 0;
        /* [task#16] Per-car private branch RNG seed: race seed mixed with the
         * slot index (deterministic, distinct per car). Guard against a 0 state
         * (a degenerate LCG fixed point would never advance). */
        {
            uint32_t bs = branch_seed_base + (uint32_t)s * 2654435761u;
            g_smart_branch_rng[s] = bs ? bs : 0xA5A5A5A5u;
        }
    }
    g_smart_inited = 1;
    TD5_LOG_I(LOG_TAG, "smart_ai_init: tier=%d base_skill=%.2f aggression=%d leash=%d",
              tier, (double)base, g_td5.ini.smart_ai_aggression,
              g_td5.ini.smart_ai_leash);
}

/* Left-rail origin + unit edge vector + road width (all in track units) for a
 * span, from the existing rail-edge helper. Returns 0 if geometry unusable. */
static int smart_span_frame(int span, double *lx, double *lz,
                            double *ex, double *ez, double *width) {
    int rlx = 0, rlz = 0, rrx = 0, rrz = 0;
    int sc = td5_track_get_span_count();
    if (span < 0 || sc <= 0 || span >= sc) return 0;
    /* Use the SAME rail frame the bias consumer (sample_target_point) uses, so
     * a positive bias always shifts toward higher u (sign-consistent). */
    if (!td5_track_get_span_route_frame(span, &rlx, &rlz, &rrx, &rrz))
        return 0;
    double dx = (double)(rrx - rlx), dz = (double)(rrz - rlz);
    double w = sqrt(dx * dx + dz * dz);
    if (w < 1.0) return 0;
    *lx = (double)rlx; *lz = (double)rlz;
    *ex = dx / w; *ez = dz / w; *width = w;
    return 1;
}

/* Lateral fraction u in [0,1] of an actor across the road at `span`. */
static double smart_actor_u(char *actor, int span) {
    double lx, lz, ex, ez, w;
    if (!smart_span_frame(span, &lx, &lz, &ex, &ez, &w)) return 0.5;
    double ax = (double)(ACTOR_I32(actor, ACTOR_WORLD_POS_X) >> 8);
    double az = (double)(ACTOR_I32(actor, ACTOR_WORLD_POS_Z) >> 8);
    double u = ((ax - lx) * ex + (az - lz) * ez) / w;
    if (u < 0.0) u = 0.0;
    if (u > 1.0) u = 1.0;
    return u;
}

/* Forward span gap (wrap-aware): >0 means `to` is ahead of `from`. */
static int smart_span_gap(int from_span, int to_span, int span_count) {
    int g = to_span - from_span;
    if (span_count > 0) {
        while (g >  span_count / 2) g -= span_count;
        while (g < -span_count / 2) g += span_count;
    }
    return g;
}

/* [R3-8 BIG-FIELD PHANTOM-PEER FILTER 2026-06-19] 1 when slot `i` is a real,
 * on-road car that the AI peer-scans should react to; 0 for slots that hold no
 * live car. In a >6-racer field (g_traffic_slot_base==16) the racer region is
 * 0..15 but only `humans+opponents` of those are actually spawned — the rest are
 * marked decoration (g_slot_state==3) and NEVER placed on the track, so their
 * pose fields (span / world-pos / lane) are stale/zero. The peer scans iterate
 * g_active_actor_count (32 with traffic) and previously filtered only FINISHED
 * racers (state 2), so those un-spawned decoration slots showed up as phantom
 * cars clustered at span 0 / the world origin: real racers + traffic braked,
 * swerved and lane-changed to dodge ghosts -> the "cars stutter/glitch with >5
 * opponents" report. Excludes: out-of-range, decoration (state 3), finished
 * racers (state 2), an unbound car_definition_ptr (the same "absent slot" test
 * the dynamic-traffic lead-span scan already uses), and the dynamic-traffic
 * parked (despawned) slots. Pure function of replicated state (g_slot_state +
 * the actor's own bound-ness) -> identical on every lockstep peer. */
static int ai_peer_is_present(int i) {
    char *a;
    if (i < 0 || i >= TD5_MAX_TOTAL_ACTORS) return 0;
    if (i < g_traffic_slot_base) {
        /* racer region: skip finished (2) and decoration/absent (3) slots */
        if (g_slot_state[i] == 2 || g_slot_state[i] == 3) return 0;
    } else if (td5_ai_traffic_dynamic_parked(i)) {
        return 0;   /* despawned traffic: no live pose */
    }
    a = actor_ptr(i);
    if (!a) return 0;
    if (ACTOR_I32(a, ACTOR_CAR_DEF_PTR) == 0) return 0;   /* never spawned */
    return 1;
}

typedef struct { double u; int gap; int32_t speed; int slot; } SmartBlocker;

/* Collect actors within [-1 .. lookahead] spans of self (incl. the human in
 * slot 0). Lateral position is the peer's own-span u. */
static int smart_gather_blockers(int self_slot, int self_span, int span_count,
                                 int lookahead, SmartBlocker *out, int max_out) {
    int n = g_active_actor_count;
    if (n > TD5_MAX_TOTAL_ACTORS) n = TD5_MAX_TOTAL_ACTORS;
    int cnt = 0;
    for (int i = 0; i < n && cnt < max_out; i++) {
        if (i == self_slot) continue;
        /* [PER-VIEWPORT TRAFFIC] same-partition (+ own player) blockers only. */
        if (td5_ai_traffic_pair_blocked(self_slot, i)) continue;
        if (!ai_peer_is_present(i)) continue;   /* [R3-8] skip phantom/absent slots */
        char *a = actor_ptr(i);
        if (!a) continue;
        int peer_span = (int)ACTOR_I16(a, ACTOR_SPAN_RAW);
        int gap = smart_span_gap(self_span, peer_span, span_count);
        if (gap < -1 || gap > lookahead) continue;
        out[cnt].u     = smart_actor_u(a, peer_span);
        out[cnt].gap   = gap;
        out[cnt].speed = ACTOR_I32(a, ACTOR_LONGITUDINAL_SPEED);
        out[cnt].slot  = i;
        cnt++;
    }
    return cnt;
}


/* ========================================================================
 * RAY-SENSING DECISION LAYER  ([GameOptions] SmartAIRays, default 1)
 *
 * A forward RAY FAN cast from each AI car's nose senses upcoming WALLS (span
 * rails) and CARS, and a curvature scan of the route ahead drives a
 * slow-in/fast-out racing line. The outputs feed the SAME levers the rest of
 * SmartAI uses, so this stays an augmentation, not a rewrite:
 *   - a target lateral fraction u in [0,1]  -> RS_TRACK_OFFSET_BIAS (lane brain)
 *   - a corner speed cap (0..1 of throttle) -> ENCOUNTER_STEER / BRAKE (speed brain)
 *
 * All geometry is read through the existing public track helpers, in the SAME
 * integer track-unit space smart_span_frame()/smart_actor_u() already use
 * (actor world pos >> 8). Math is double precision + deterministic (no rng),
 * matching the existing SmartAI discipline, so it is reusable by both racers
 * and traffic and stays replay-stable. With SmartAIRays=0 none of this runs.
 *
 * Orientation is derived from geometry, never from a guessed angle sign: the
 * forward direction is mid(span)->mid(span+1) and "+u" (toward the right rail)
 * is the left->right rail axis, so left/right decisions are made by dotting a
 * ray against that axis — no coordinate-handedness assumptions.
 * ======================================================================== */

#define SMART_RAY_COUNT     7        /* fan size: centre + 3 each side          */
#define SMART_RAY_SPREAD    0.60     /* +-radians of the outermost rays (~34 deg)*/
#define SMART_RAY_SPANS     8        /* spans of rail tested per ray            */
#define SMART_RAY_FWD_LEAN  0.45     /* |lateral-dot| below this == "forward-ish" */
#define SMART_CORNER_WIN    3        /* span window for the curvature measure    */
#define SMART_CORNER_SHARP  0x2A0    /* heading delta over the window that == 1.0 */
#define SMART_AVOID_GAIN    0.34     /* max road-fraction a wall/car push applies */
#define SMART_RAY_MARGIN    0.06     /* hard backstop: never aim within 6% of rail*/

typedef struct {
    int    turn_dir;    /* dominant bend ahead: -1 = +u side, +1 = -u side, 0 straight */
    double sharpness;   /* 0..1 curvature of that bend                           */
    int    apex_d;      /* spans ahead to the bend apex                          */
    double u_line;      /* racing-line lateral fraction (around u_base)          */
    double speed_cap;   /* 0..1 throttle fraction for slow-in                    */
} SmartCorner;

typedef struct {
    double avoid_u;      /* signed lateral push toward the clearer side (road frac) */
    int    danger;       /* a wall/car sits close in the forward corridor        */
    int    car_ahead;    /* slot of a car blocking ~dead ahead, else -1          */
    double front_clear;  /* clearance straight ahead (track units, capped)       */
    int    wall_imminent;/* a WALL is very close ahead -> force a brake          */
    double span_len;     /* longitudinal length of the current span (track units)*/
} SmartSense;

static inline int smart_ang_signed(int a) { a &= 0xFFF; if (a > 0x800) a -= 0x1000; return a; }

/* Mid-point (track units) of a span's left/right rail. */
static int smart_span_mid(int span, int span_count, double *mx, double *mz) {
    int lx, lz, rx, rz;
    int s = span % span_count; if (s < 0) s += span_count;
    if (!td5_track_get_span_route_frame(s, &lx, &lz, &rx, &rz)) return 0;
    *mx = 0.5 * ((double)lx + (double)rx);
    *mz = 0.5 * ((double)lz + (double)rz);
    return 1;
}

/* Unit forward direction + span length from mid(span)->mid(span+1). */
static int smart_forward_dir(int span, int span_count,
                             double *fx, double *fz, double *len) {
    double ax, az, bx, bz;
    if (!smart_span_mid(span,     span_count, &ax, &az)) return 0;
    if (!smart_span_mid(span + 1, span_count, &bx, &bz)) return 0;
    double dx = bx - ax, dz = bz - az;
    double d = sqrt(dx * dx + dz * dz);
    if (d < 1.0) return 0;
    *fx = dx / d; *fz = dz / d; *len = d;
    return 1;
}

/* Ray O + t*Dir (Dir unit, t in [0,maxd]) vs segment A->B. Returns t, or -1. */
static double smart_ray_seg(double ox, double oz, double dx, double dz, double maxd,
                            double ax, double az, double bx, double bz) {
    double ex = bx - ax, ez = bz - az;
    double det = ex * dz - dx * ez;           /* [[Dx,-Ex],[Dz,-Ez]] determinant */
    if (det > -1e-9 && det < 1e-9) return -1.0; /* parallel */
    double wx = ax - ox, wz = az - oz;
    double t = (-wx * ez + ex * wz) / det;    /* distance along the ray   */
    double s = (dx * wz - dz * wx) / det;     /* parameter along segment  */
    if (t < 0.0 || t > maxd) return -1.0;
    if (s < 0.0 || s > 1.0)  return -1.0;
    return t;
}

/* Ray vs circle (car obstacle). Returns nearest forward t, or -1. */
static double smart_ray_circle(double ox, double oz, double dx, double dz, double maxd,
                               double cx, double cz, double r) {
    double fx = ox - cx, fz = oz - cz;
    double b = fx * dx + fz * dz;             /* D is unit => a == 1       */
    double c = fx * fx + fz * fz - r * r;
    double disc = b * b - c;
    if (disc < 0.0) return -1.0;
    double sq = sqrt(disc);
    double t = -b - sq;
    if (t < 0.0) t = -b + sq;                 /* origin inside the circle  */
    if (t < 0.0 || t > maxd) return -1.0;
    return t;
}

/* Curvature scan + outside-in-outside racing line + slow-in speed cap. */
static void smart_corner_eval(int slot, int span_raw, int span_count,
                              double u_base, float skill, SmartCorner *out) {
    out->turn_dir = 0; out->sharpness = 0.0; out->apex_d = 0;
    out->u_line = u_base; out->speed_cap = 1.0;
    (void)slot;
    if (span_count <= 0) return;

    int D = 4 + (int)(skill * 8.0f);          /* look 4..12 spans ahead */
    int best_turn = 0, apex_d = 0;
    for (int d = 0; d <= D; d++) {
        int s0 = ((span_raw + d) % span_count + span_count) % span_count;
        int s1 = ((span_raw + d + SMART_CORNER_WIN) % span_count + span_count) % span_count;
        int turn = smart_ang_signed(td5_track_get_primary_route_heading(s1)
                                    - td5_track_get_primary_route_heading(s0));
        int at = turn < 0 ? -turn : turn;
        int ab = best_turn < 0 ? -best_turn : best_turn;
        if (at > ab) { best_turn = turn; apex_d = d; }
    }
    int mag = best_turn < 0 ? -best_turn : best_turn;
    double sharp = (double)mag / (double)SMART_CORNER_SHARP;
    if (sharp > 1.0) sharp = 1.0;
    out->sharpness = sharp;
    out->apex_d    = apex_d;

    /* Slow-in cap: tighter bend -> lower cap; higher skill carries more speed
     * (brakes later, loses less). Floor keeps cars from crawling. Computed even
     * when the racing-line geometry is unusable, so braking is independent of it. */
    double max_loss = 0.50 - (double)skill * 0.22;   /* ~0.47 (rookie) .. 0.29 (ace) */
    double cap = 1.0 - max_loss * sharp;
    if (cap < 0.45) cap = 0.45;
    out->speed_cap = cap;

    if (sharp <= 0.02) return;                /* effectively straight -> no line shift */

    /* Find the INSIDE of the bend GEOMETRICALLY (no heading-sign convention
     * guess): the forward direction rotates toward the inside as the car rounds
     * the corner, so (F_apex - F_here) points into the turn. Dotting that against
     * the +u (left->right) rail axis tells whether the inside is the +u or -u
     * side. This is self-correcting regardless of coordinate handedness; the wall
     * rays are a further backstop. */
    double f0x, f0z, l0, f1x, f1z, l1, ex, ez, w, slx, slz;
    int inside_sign = 0;
    if (smart_forward_dir(span_raw, span_count, &f0x, &f0z, &l0) &&
        smart_forward_dir(span_raw + apex_d, span_count, &f1x, &f1z, &l1) &&
        smart_span_frame(((span_raw % span_count) + span_count) % span_count,
                         &slx, &slz, &ex, &ez, &w)) {
        double cx = f1x - f0x, cz = f1z - f0z;       /* centripetal-ish vector */
        double dot = cx * ex + cz * ez;              /* >0 -> inside is +u side */
        inside_sign = (dot > 0.0) ? 1 : -1;
    }
    out->turn_dir = inside_sign;                     /* +1 = inside on the +u side */
    if (inside_sign == 0) return;

    double appro = (D > 0) ? (1.0 - (double)apex_d / (double)D) : 0.0; /* 0 far..1 near apex */
    if (appro < 0.0) appro = 0.0;
    if (appro > 1.0) appro = 1.0;
    double inside  = (double)inside_sign;        /* apex side  */
    double outside = (double)(-inside_sign);     /* entry/exit side */
    const double ENTRY = 0.30, APEX = 0.34;
    double off = (outside * ENTRY * (1.0 - appro) + inside * APEX * appro) * sharp;
    double u = u_base + off;
    if (u < SMART_RAY_MARGIN)       u = SMART_RAY_MARGIN;
    if (u > 1.0 - SMART_RAY_MARGIN) u = 1.0 - SMART_RAY_MARGIN;
    out->u_line = u;
}

/* Cast the ray fan and reduce it to steering + speed signals for one actor. */
static void smart_sense(int slot, int span_raw, int span_count,
                        float skill, SmartSense *out) {
    out->avoid_u = 0.0; out->danger = 0; out->car_ahead = -1;
    out->front_clear = 1e9; out->wall_imminent = 0; out->span_len = 0.0;
    if (span_count <= 0) return;

    char *self = actor_ptr(slot);
    if (!self) return;
    double ox = (double)(ACTOR_I32(self, ACTOR_WORLD_POS_X) >> 8);
    double oz = (double)(ACTOR_I32(self, ACTOR_WORLD_POS_Z) >> 8);

    double fx, fz, span_len;
    if (!smart_forward_dir(span_raw, span_count, &fx, &fz, &span_len)) return;
    out->span_len = span_len;

    /* +u (right-rail) lateral axis at the current span. */
    double slx, slz, sex, sez, swidth;
    if (!smart_span_frame(((span_raw % span_count) + span_count) % span_count,
                          &slx, &slz, &sex, &sez, &swidth)) return;

    /* Build the rail polylines once. */
    double Lx[SMART_RAY_SPANS + 2], Lz[SMART_RAY_SPANS + 2];
    double Rx[SMART_RAY_SPANS + 2], Rz[SMART_RAY_SPANS + 2];
    int npts = 0;
    for (int d = 0; d <= SMART_RAY_SPANS; d++) {
        int s = ((span_raw + d) % span_count + span_count) % span_count;
        int lx, lz, rx, rz;
        if (!td5_track_get_span_route_frame(s, &lx, &lz, &rx, &rz)) break;
        Lx[npts] = lx; Lz[npts] = lz; Rx[npts] = rx; Rz[npts] = rz; npts++;
    }
    if (npts < 2) return;

    /* Gather nearby cars (world pos) within the ray span window. */
    double Cx[TD5_MAX_TOTAL_ACTORS], Cz[TD5_MAX_TOTAL_ACTORS];
    int    Cslot[TD5_MAX_TOTAL_ACTORS], ncar = 0;
    int lanes = td5_track_get_span_lane_count(((span_raw % span_count) + span_count) % span_count);
    if (lanes < 1) lanes = 1;
    double car_r = 0.35 * swidth / (double)lanes;
    if (car_r < 40.0)  car_r = 40.0;
    if (car_r > 220.0) car_r = 220.0;
    int n = g_active_actor_count;
    if (n > TD5_MAX_TOTAL_ACTORS) n = TD5_MAX_TOTAL_ACTORS;
    for (int i = 0; i < n && ncar < TD5_MAX_TOTAL_ACTORS; i++) {
        if (i == slot) continue;
        /* [PER-VIEWPORT TRAFFIC] ignore cars from OTHER viewports' partitions: a
         * car must not see/dodge its cross-partition twin (same position) nor a
         * player who can't see it — that both desynced the sets and showed traffic
         * "dodging nothing". Within a partition the cars are identical, so the
         * avoidance is identical and the partitions stay in lockstep. */
        if (td5_ai_traffic_pair_blocked(slot, i)) continue;
        if (!ai_peer_is_present(i)) continue;   /* [R3-8] skip phantom/absent slots */
        char *a = actor_ptr(i);
        if (!a) continue;
        int gap = smart_span_gap(span_raw, (int)ACTOR_I16(a, ACTOR_SPAN_RAW), span_count);
        if (gap < -1 || gap > SMART_RAY_SPANS) continue;
        Cx[ncar]    = (double)(ACTOR_I32(a, ACTOR_WORLD_POS_X) >> 8);
        Cz[ncar]    = (double)(ACTOR_I32(a, ACTOR_WORLD_POS_Z) >> 8);
        Cslot[ncar] = i;
        ncar++;
    }

    double maxd = span_len * (double)SMART_RAY_SPANS;
    if (maxd < 400.0) maxd = 400.0;

    double open_hi = 0.0, open_lo = 0.0;   /* clearance weighted toward +u / -u */
    double fwd_min = maxd; int fwd_is_car = 0, fwd_slot = -1;
    double step = (SMART_RAY_COUNT > 1)
                  ? (2.0 * SMART_RAY_SPREAD / (double)(SMART_RAY_COUNT - 1)) : 0.0;

    for (int r = 0; r < SMART_RAY_COUNT; r++) {
        double ang = -SMART_RAY_SPREAD + step * (double)r;
        double ca = cos(ang), sa = sin(ang);
        double dx = fx * ca - fz * sa;
        double dz = fx * sa + fz * ca;

        double best = maxd; int is_car = 0; int hit_slot = -1;
        for (int i = 0; i + 1 < npts; i++) {
            double t = smart_ray_seg(ox, oz, dx, dz, best, Lx[i], Lz[i], Lx[i+1], Lz[i+1]);
            if (t >= 0.0 && t < best) { best = t; is_car = 0; hit_slot = -1; }
            t = smart_ray_seg(ox, oz, dx, dz, best, Rx[i], Rz[i], Rx[i+1], Rz[i+1]);
            if (t >= 0.0 && t < best) { best = t; is_car = 0; hit_slot = -1; }
        }
        for (int k = 0; k < ncar; k++) {
            double t = smart_ray_circle(ox, oz, dx, dz, best, Cx[k], Cz[k], car_r);
            if (t >= 0.0 && t < best) { best = t; is_car = 1; hit_slot = Cslot[k]; }
        }

        double lean = dx * sex + dz * sez;        /* >0 leans toward +u (right rail) */
        if (lean > 0.0) open_hi += best * lean;
        else            open_lo += best * (-lean);

        if (lean < SMART_RAY_FWD_LEAN && lean > -SMART_RAY_FWD_LEAN && best < fwd_min) {
            fwd_min = best; fwd_is_car = is_car; fwd_slot = hit_slot;
        }
    }

    out->front_clear = fwd_min;
    /* Steer away from danger toward the more open side. A clear road (large
     * fwd_min) yields ~0 push, so the car holds its racing line. */
    double danger_dist = span_len * 1.8;
    if (danger_dist < 200.0) danger_dist = 200.0;
    if (fwd_min < danger_dist) {
        double strength = (danger_dist - fwd_min) / danger_dist;   /* 0..1 */
        if (strength > 1.0) strength = 1.0;
        double sign = (open_hi >= open_lo) ? 1.0 : -1.0;           /* +1 -> toward +u */
        out->avoid_u = sign * SMART_AVOID_GAIN * strength;
        out->danger  = 1;
    }
    if (fwd_is_car && fwd_min < span_len * 2.5) out->car_ahead = fwd_slot;
    if (!fwd_is_car && fwd_min < span_len * 1.1) out->wall_imminent = 1;
    (void)skill;
}

/* [task#16 2026-06-15] A/B knob for RANDOM AI branch pre-commitment. Default on.
 * TD5RE_AI_BRANCH_RANDOM=0 restores the prior congestion-only "prefer main unless
 * jammed" policy (the AI then rarely takes a fork). Logged once on first read. */
static int td5_ai_branch_random_enabled(void) {
    static int s = -1;
    if (s < 0) {
        const char *e = getenv("TD5RE_AI_BRANCH_RANDOM");
        s = (e && e[0] == '0') ? 0 : 1;
        TD5_LOG_I(LOG_TAG, "ai_branch_random knob: TD5RE_AI_BRANCH_RANDOM=%d "
                  "(AI pre-picks forks at random %s)", s, s ? "ON" : "OFF");
    }
    return s;
}

/* A branch corridor reachable from this fork is genuinely DRIVABLE: always so on
 * `branch_span` must already be range-checked. (TD6 branches are connected
 * drivable roads, so td5_track_td6_branches_drivable() is always true on a TD6
 * track; the gate is kept for symmetry with native handling.) */
static int td5_ai_branch_takeable(int branch_span, int span_count) {
    if (branch_span < 0 || branch_span >= span_count) return 0;
    if (g_active_td6_level > 0 && !td5_track_td6_branches_drivable()) return 0;
    return 1;
}

/* Strategic branch: scan ahead on the raw strip records for a junction span
 * (type 8 fwd / 11 bwd). At a fork the "branch" route is taken from the HIGH
 * sub-lanes and "main" from the LOW sub-lanes (see td5_track.c junction logic),
 * so we express the preference as a lateral pull (-1 = hug low/main lanes,
 * +1 = hug high/branch lanes) that the lane brain folds into its scoring.
 *
 * [task#16] With TD5RE_AI_BRANCH_RANDOM (default on) the car DECIDES BEFOREHAND:
 * the first time a fork comes into look-ahead range it rolls ONCE and LOCKS the
 * choice (take-branch vs stay-main) for that fork, then commits to it until it
 * is passed. The roll uses the car's PRIVATE per-slot RNG (g_smart_branch_rng,
 * seeded at race init from the replicated race seed) and is advanced ONLY on a
 * sim-tick branch commit — NOT the shared CRT rand(), which is also drawn at
 * render rate (sound/vfx/camera) and so diverges across lockstep peers at
 * different frame rates. Every netplay client and every replay therefore picks
 * the SAME branch (no Date/time/render-rate/local-only state). The result is
 * confident branch-taking instead of the old congestion-only policy that almost
 * always fell back to the main road ("AI avoids branches"). Congestion still
 * tilts the odds — a jammed main road makes the branch more likely — but a clear
 * fork is now a genuine coin-flip, weighted to actually use the branch. */
static void td5_ai_smart_branch(int slot) {
    g_smart_branch_pull[slot] = 0.0;
    const uint8_t *strips = (const uint8_t *)g_strip_span_base;
    int span_count = td5_track_get_span_count();
    if (!strips || span_count <= 0) {
        if (slot >= 0 && slot < TD5_MAX_TOTAL_ACTORS)
            g_smart_branch_commit_span[slot] = -1;
        return;
    }
    char *actor = actor_ptr(slot);
    int span = (int)ACTOR_I16(actor, ACTOR_SPAN_RAW);
    if (span < 0 || span >= span_count) {
        if (slot >= 0 && slot < TD5_MAX_TOTAL_ACTORS)
            g_smart_branch_commit_span[slot] = -1;
        return;
    }

    float skill = td5_ai_smart_skill(slot);
    /* [branch-lookahead 2026-06-23] Decide a few more spans in advance. The fork
     * is COMMITTED the first tick it enters this range, and the lateral pull
     * (front-loaded below) starts sliding the car toward the chosen arm from that
     * tick — so a wider window = earlier, surer branch entry. Raised the floor
     * 5->7 so even low-skill / fast cars have runway to reach the outer sub-lanes
     * before the at-crossing route test (td5_track.c cases 8/11). */
    int look = 7 + (int)(skill * 8.0f); /* 7..15 spans — react early enough to lane-change before the fork */

    for (int d = 1; d <= look; d++) {
        int s = span + d;
        if (s >= span_count) s -= span_count;
        if (s < 0) continue;
        uint8_t stype = strips[(size_t)s * 0x18];
        if (stype != 8 && stype != 11) continue;

        int main_span = (stype == 8) ? (s + 1) : (s - 1);
        if (main_span >= span_count) main_span -= span_count;
        if (main_span < 0) main_span += span_count;
        int branch_span = (stype == 8)
            ? (int)*(const int16_t *)(const void *)(strips + (size_t)s * 0x18 + 8)
            : (int)*(const int16_t *)(const void *)(strips + (size_t)s * 0x18 + 10);
        int takeable = td5_ai_branch_takeable(branch_span, span_count);

        int cong_main = 0, cong_branch = 0;
        int n = g_active_actor_count;
        if (n > TD5_MAX_TOTAL_ACTORS) n = TD5_MAX_TOTAL_ACTORS;
        for (int i = 0; i < n; i++) {
            if (i == slot) continue;
            if (!ai_peer_is_present(i)) continue;   /* [R3-8] skip phantom/absent slots */
            int psp = (int)ACTOR_I16(actor_ptr(i), ACTOR_SPAN_RAW);
            if (smart_iabs(psp - main_span) <= 2) cong_main++;
            if (branch_span >= 0 && branch_span < span_count &&
                smart_iabs(psp - branch_span) <= 2) cong_branch++;
        }

        double pref;
        if (g_cop_is_cop[slot] && g_cop_phase[slot] == COP_CHASING &&
            g_cop_target[slot] >= 0 && g_cop_target[slot] < TD5_MAX_TOTAL_ACTORS) {
            /* [POLICE rewrite 2026-06-19] Chase branch policy: take whichever
             * side of the fork (main continuation vs branch) is closer IN WORLD
             * UNITS to the chased car. This makes the cop follow the player onto
             * a branch, and when the player is already past the fork it picks
             * the side that minimizes the unit gap — exactly the requested
             * "choose the branch the player is in / the closest-in-units branch".
             * Deterministic: replicated world positions + track geometry, no RNG. */
            int take = 0;
            if (takeable && branch_span >= 0 && branch_span < span_count) {
                char *cop_tgt = actor_ptr(g_cop_target[slot]);
                int64_t tx = (int64_t)(ACTOR_I32(cop_tgt, ACTOR_WORLD_POS_X) >> 8);
                int64_t tz = (int64_t)(ACTOR_I32(cop_tgt, ACTOR_WORLD_POS_Z) >> 8);
                int blx, blz, brx, brz, mlx, mlz, mrx, mrz;
                if (td5_track_get_span_route_frame(branch_span, &blx, &blz, &brx, &brz) &&
                    td5_track_get_span_route_frame(main_span,   &mlx, &mlz, &mrx, &mrz)) {
                    int64_t bcx = ((int64_t)blx + brx) / 2, bcz = ((int64_t)blz + brz) / 2;
                    int64_t mcx = ((int64_t)mlx + mrx) / 2, mcz = ((int64_t)mlz + mrz) / 2;
                    int64_t db = (bcx - tx) * (bcx - tx) + (bcz - tz) * (bcz - tz);
                    int64_t dm = (mcx - tx) * (mcx - tx) + (mcz - tz) * (mcz - tz);
                    take = (db < dm) ? 1 : 0;
                }
            }
            if (slot >= 0 && slot < TD5_MAX_TOTAL_ACTORS) {
                g_smart_branch_commit_span[slot] = (int16_t)s;
                g_smart_branch_commit_take[slot] = (int8_t)take;
            }
            pref = take ? +1.0 : -1.0;
        } else if (td5_ai_branch_random_enabled()) {
            /* [task#16] RANDOM PRE-COMMITMENT. Decide once per fork and lock it.
             * The walker routes onto the branch from the HIGH sub-lanes, so a
             * "take" commitment must be made early (while still spans away) for
             * the lane brain to slide the car across in time — hence the decision
             * is taken on the FIRST tick the fork enters range and held. */
            int take;
            if (slot >= 0 && slot < TD5_MAX_TOTAL_ACTORS &&
                g_smart_branch_commit_span[slot] == (int16_t)s) {
                take = g_smart_branch_commit_take[slot];   /* already locked */
            } else if (!takeable) {
                take = 0;                                  /* branch unusable → main */
                if (slot >= 0 && slot < TD5_MAX_TOTAL_ACTORS) {
                    g_smart_branch_commit_span[slot] = (int16_t)s;
                    g_smart_branch_commit_take[slot] = 0;
                }
            } else {
                /* Roll the dice with the car's PRIVATE per-slot RNG (seeded at
                 * race init from the replicated race seed; see g_smart_branch_rng).
                 * Base ~55% chance to TAKE a clear, valid branch so forks are
                 * genuinely used; shift the odds by congestion (jammed main → up to
                 * ~85% take; jammed branch → down to ~30%). EXACTLY ONE LCG step
                 * per fork commitment (not per tick — the `== s` branch above short-
                 * circuits re-rolls once committed), so the stream advances only on
                 * sim-tick branch-commit EVENTS.
                 *
                 * Netplay determinism: this stream is derived ONLY from replicated
                 * sim state (race seed + slot index + the sequence of commit events)
                 * and is NEVER the shared CRT rand(), whose sequence is also drawn at
                 * render rate (td5_sound engine pitch, td5_vfx particles, td5_camera
                 * trackside timers) and therefore diverges between peers/replays at
                 * different frame rates. Same seed → same branch choices on every
                 * peer and on replay. (Bounds-guarded: slot must index the per-car
                 * RNG array; an out-of-range slot — which also cannot store a commit
                 * — falls back to ~coin-flip without touching the array.) */
                uint32_t roll = (slot >= 0 && slot < TD5_MAX_TOTAL_ACTORS)
                                    ? smart_branch_rand(slot) : 0x8000u;
                int r = (int)(roll % 100u);         /* 0..99 */
                int take_threshold = 45;            /* >= threshold → TAKE branch */
                int cong_delta = cong_main - cong_branch;
                take_threshold -= cong_delta * 15;  /* main busier → easier to take */
                if (take_threshold < 15)  take_threshold = 15;
                if (take_threshold > 85)  take_threshold = 85;
                take = (r >= take_threshold) ? 1 : 0;
                if (slot >= 0 && slot < TD5_MAX_TOTAL_ACTORS) {
                    g_smart_branch_commit_span[slot] = (int16_t)s;
                    g_smart_branch_commit_take[slot] = (int8_t)take;
                }
                TD5_LOG_I(LOG_TAG,
                          "ai_branch_commit: slot=%d fork=%d type=%d take=%d "
                          "r=%d thr=%d main_c=%d branch_c=%d branch=%d",
                          slot, s, stype, take, r, take_threshold,
                          cong_main, cong_branch, branch_span);
            }
            pref = take ? +1.0 : -1.0;
        } else {
            /* Legacy congestion-only policy: hold the main through-route unless it
             * is notably more congested than the branch. */
            if (!takeable)
                pref = -1.0;
            else if (cong_main - cong_branch >= 2)
                pref = +1.0;
            else
                pref = -1.0;
        }
        (void)g_smart_branch_pref;

        /* [branch-lookahead 2026-06-23] FRONT-LOADED commitment. The car only
         * routes onto the branch if it is already in the outer sub-lanes when it
         * CROSSES the fork (the at-crossing sub_lane vs lane_count test in
         * td5_track.c cases 8/11, faithful to orig UpdateActorTrackPosition
         * @ 0x004440F0). Sliding there takes several ticks, so the lateral pull
         * must be substantial for the WHOLE approach, not just at the fork.
         *
         * The OLD ramp `1 - (d-1)/look` was ~0 the instant the fork came into
         * range and only peaked AT the fork: the car committed its decision early
         * but didn't START turning until it was on top of the fork, couldn't reach
         * the outer lanes in time, and fell back to the default (right) arm — the
         * reported "couldn't turn on time" symptom, worst on branch-dense TD6
         * tracks. Now: ~0.6 strength the moment the fork is detected, ramping to
         * full at the fork, so the lane brain begins the slide several spans out.
         * The u-space rate limit downstream still keeps the physical move gentle. */
        double proximity = (double)(look - d + 1) / (double)look; /* ~1/look far -> 1 at fork */
        double strength = 0.6 + 0.4 * proximity;                  /* 0.6 far -> 1.0 at fork */
        if (strength > 1.0) strength = 1.0;
        g_smart_branch_pull[slot] = pref * strength;
        if ((g_ai_frame_counter % 60u) == 0u) {
            TD5_LOG_I(LOG_TAG, "smart_branch: slot=%d fork=%d d=%d type=%d "
                      "main=%d(c%d) branch=%d(c%d) takeable=%d pull=%.2f",
                      slot, s, d, stype, main_span, cong_main,
                      branch_span, cong_branch, takeable, g_smart_branch_pull[slot]);
        }
        return;
    }

    /* No fork in range — drop any stale commitment so the next fork rolls afresh. */
    if (slot >= 0 && slot < TD5_MAX_TOTAL_ACTORS)
        g_smart_branch_commit_span[slot] = -1;
}

/* Lane brain: score candidate lanes (surface, occupancy, wall, racing-line,
 * change-cost, fork pull), pick the best, and steer RS_TRACK_OFFSET_BIAS toward
 * it with a skill-scaled rate limit and an occasional low-skill lapse. */
static void td5_ai_smart_lane_bias(int slot) {
    int32_t *rs = route_state(slot);
    char *actor = actor_ptr(slot);
    int span_count = td5_track_get_span_count();
    int span      = (int)ACTOR_I16(actor, ACTOR_SPAN_NORMALIZED);
    int span_raw  = (int)ACTOR_I16(actor, ACTOR_SPAN_RAW);

    /* A branch-road span is an APPENDED detour (span_raw beyond the main ring,
     * e.g. Moscow-reverse span 2790). These are tight, authored, often
     * wall-lined roads where imposing lane-center targeting drives the car into
     * the boundary — so on them we follow the authored route line instead. */
    int ring_len = td5_track_get_ring_length();
    int on_branch = (ring_len > 0 && span_raw >= ring_len);

    td5_ai_smart_branch(slot);

    if (span_count <= 0 || span < 0) {
        int32_t b = rs[RS_TRACK_OFFSET_BIAS];
        if (b > 8) b -= 8; else if (b < -8) b += 8; else b = 0;
        rs[RS_TRACK_OFFSET_BIAS] = b;
        return;
    }

    float skill = td5_ai_smart_skill(slot);
    int lookahead = 4 + (int)(skill * 8.0f);

    double clx, clz, cex, cez, cwidth;
    if (!smart_span_frame(span, &clx, &clz, &cex, &cez, &cwidth))
        cwidth = SMART_LANE_FALLBACK_WIDTH;

    int look_span = span + 2;
    if (look_span >= span_count) look_span -= span_count;
    int L = td5_track_get_span_lane_count(look_span);
    if (L < 1) L = 1;
    if (L > SMART_MAX_LANES) L = SMART_MAX_LANES;

    double u_base = 0.5;
    {
        const uint8_t *rt = (const uint8_t *)(intptr_t)rs[RS_ROUTE_TABLE_PTR];
        if (rt) {
            int rb = (int)rt[(size_t)(unsigned)look_span * 3u];
            u_base = (double)rb / 256.0;
            if (u_base < 0.0) u_base = 0.0;
            if (u_base > 1.0) u_base = 1.0;
        }
    }

    int proj_span = (span_raw >= 0 && span_raw < span_count) ? span_raw : span;
    double u_self = smart_actor_u(actor, proj_span);

    SmartBlocker bl[TD5_MAX_TOTAL_ACTORS];
    int nb = smart_gather_blockers(slot, span_raw, span_count, lookahead,
                                   bl, TD5_MAX_TOTAL_ACTORS);

    int aggression = g_td5.ini.smart_ai_aggression;
    double lane_w = 1.0 / (double)L;

    /* ====================================================================
     * DYNAMIC racing-line correction (no discrete lanes). The target starts
     * ON the authored racing line — so a clear road runs at faithful pace with
     * no lane-to-lane darting — and we add smooth, continuous nudges ONLY for
     * the four things that actually matter: SLOW surface, a BRANCH ahead, a
     * WALL, and other CARS. Positions are road fractions u in [0,1].
     * ==================================================================== */
    const double WALL_MARGIN = 0.11;        /* never aim within 11% of a rail */
    double target_u = u_base;

    /* RAY BRAIN (SmartAIRays): corner racing line + ray wall/car avoidance,
     * feeding the same target_u the rate-limit tail consumes. Replaces the
     * discrete-lane scorer + hard wall clamp; with rays off the old path runs. */
    SmartCorner co; SmartSense se;
    int rays_on = g_td5.ini.smart_ai_rays;
    if (rays_on) {
        smart_corner_eval(slot, span_raw, span_count, u_base, skill, &co);
        smart_sense(slot, span_raw, span_count, skill, &se);
        double bpull = g_smart_branch_pull[slot];
        if (on_branch) {
            target_u = u_base + se.avoid_u;                /* hold authored branch line + avoid */
        } else if (bpull != 0.0) {
            /* [branch-lookahead 2026-06-23] Use the SAME proven anchors as the
             * rays-OFF path below (0.86 take / 0.18 stay). The old rays-ON anchors
             * (0.72/0.28) were too gentle: near a fork the route line's own u_base
             * already reaches ~0.78, so a 0.72 take-anchor pulled the car DOWN,
             * BELOW the line and out of the outer sub-lanes the at-crossing route
             * test needs (td5_track.c cases 8/11). A committed "take" then silently
             * fell back to the main arm — observed directly: take=1 cars never left
             * span 499->500 onto branch 2790. 0.86 sits the car decisively in the
             * outer lanes so the fork actually routes onto the branch; 0.18 keeps a
             * "stay" equally decisive below the split threshold. SMART_RAY_MARGIN
             * (0.06) leaves headroom to 0.94, so neither anchor is clamped. */
            double anchor = (bpull < 0.0) ? 0.18 : 0.86;
            double w = (bpull < 0.0) ? -bpull : bpull; if (w > 1.0) w = 1.0;
            target_u = u_base * (1.0 - w) + anchor * w + se.avoid_u;
        } else {
            target_u = co.u_line + se.avoid_u;             /* racing line + ray avoidance */
        }
        if ((g_ai_frame_counter % 60u) == 0u)
            TD5_LOG_I(LOG_TAG, "smart_ray: slot=%d skill=%.2f span=%d u_base=%.2f "
                      "u_line=%.2f avoid=%.2f tgt=%.2f turn=%d sharp=%.2f apex=%d "
                      "cap=%.2f front=%.0f dgr=%d car=%d%s",
                      slot, (double)skill, span, u_base, co.u_line, se.avoid_u,
                      target_u, co.turn_dir, co.sharpness, co.apex_d, co.speed_cap,
                      se.front_clear, se.danger, se.car_ahead,
                      on_branch ? " [branch]" : "");
    } else if (!on_branch) {
        double pull = g_smart_branch_pull[slot];
        if (pull != 0.0) {
            /* BRANCH dominates near a fork: firmly blend the target toward the
             * chosen side (main = low u for pull<0, branch = high u for pull>0),
             * ramping with fork proximity, so the car arrives correctly placed
             * and enters cleanly instead of clipping the branch wall. Surface /
             * overtake nudges are skipped here — getting onto the right road
             * matters more than lane choice for these few spans.
             *
             * [task#16 2026-06-15] The walker routes onto the branch only when the
             * running sub-lane index is >= the post-fork main lane count (see
             * resolve_neighbor in td5_track.c), i.e. the car must be in the OUTER
             * (high-u) lanes. The fork span is wider than the main road, so 0.72
             * sometimes wasn't outer enough to clear next_lanes — a committed
             * "take" then silently fell back to main. Push the take anchor hard to
             * the high rail (0.86, just inside WALL_MARGIN) so the sub-lane reliably
             * lands in the branch range; the "stay" anchor hugs the low lanes (0.18)
             * so a committed main choice is equally decisive. */
            double anchor = (pull < 0.0) ? 0.18 : 0.86;
            double w = (pull < 0.0) ? -pull : pull;   /* 0..1 */
            if (w > 1.0) w = 1.0;
            target_u = u_base * (1.0 - w) + anchor * w;
        } else {
            /* SPREAD across the width. Score every lane and CLAIM the one that
             * is clearest of nearby cars, so the field fans out instead of
             * stacking single-file on the (central) racing line and fighting for
             * it. Surface, walls and the racing line are only tiebreaks. The
             * choice is COMMITTED with hysteresis so it never jitters; faster
             * cars (skill-pace) then drive past in their own clear lane, so
             * overtaking emerges naturally without an explicit "pass" state. */
            double lane_score[SMART_MAX_LANES];
            for (int l = 0; l < L; l++) {
                double u_c = (l + 0.5) * lane_w;
                double s = 0.0;
                /* occupancy (PRIMARY): a car in/near this lane (ahead or right
                 * beside) makes it a bad place to be — weighted by how laterally
                 * and longitudinally close it is. */
                for (int k = 0; k < nb; k++) {
                    double du = bl[k].u - u_c; if (du < 0) du = -du;
                    if (du < lane_w * 1.3) {
                        double prox = (lane_w * 1.3 - du) / (lane_w * 1.3);   /* 0..1 lateral */
                        int g = bl[k].gap < 0 ? 0 : bl[k].gap;
                        double lng = (double)(lookahead - g + 1) / (double)(lookahead + 1);
                        if (lng < 0.0) lng = 0.0;
                        s += 5.0 * prox * lng;
                    }
                }
                if (L > 1 &&
                    td5_track_surface_is_slow(td5_track_get_span_lane_surface(look_span, l)))
                    s += 6.0;                                  /* slow surface */
                if (L > 2 && (l == 0 || l == L - 1)) s += 0.8; /* wall-adjacent lane */
                { double dl = u_c - u_base; if (dl < 0) dl = -dl; s += dl * 0.8; } /* racing-line tiebreak */
                lane_score[l] = s;
            }

            int line_lane = (int)(u_base * L);
            if (line_lane < 0) line_lane = 0;
            if (line_lane >= L) line_lane = L - 1;

            int cur_lane = (int)g_smart_target_lane[slot];
            if (cur_lane < 0 || cur_lane >= L) {
                cur_lane = (int)(u_self * L);
                if (cur_lane < 0) cur_lane = 0;
                if (cur_lane >= L) cur_lane = L - 1;
                g_smart_lane_dwell[slot] = 0;
            }
            int best_lane = cur_lane;
            for (int l = 0; l < L; l++)
                if (lane_score[l] < lane_score[best_lane]) best_lane = l;

            /* Hysteresis + dwell: hold the committed lane unless another is
             * clearly better AND we've held this one long enough — OR a car has
             * moved in close (occupied), which we react to at once. */
            int min_dwell = 16 + (int)((1.0f - skill) * 20.0f);   /* ~16..36 ticks */
            int cur_occupied = (lane_score[cur_lane] >= 4.0);
            if (best_lane != cur_lane &&
                lane_score[best_lane] + 1.2 < lane_score[cur_lane] &&
                ((int)g_smart_lane_dwell[slot] >= min_dwell || cur_occupied)) {
                cur_lane = best_lane;
                g_smart_lane_dwell[slot] = 0;
                if ((g_ai_frame_counter % 30u) == 0u)
                    TD5_LOG_I(LOG_TAG, "smart_spread: slot=%d -> lane %d/%d (score %.1f)",
                              slot, cur_lane, L, lane_score[cur_lane]);
            } else if ((int)g_smart_lane_dwell[slot] < 30000) {
                g_smart_lane_dwell[slot]++;
            }
            g_smart_target_lane[slot] = (int8_t)cur_lane;

            /* On the racing-line lane → take the EXACT line (fast); otherwise
             * the lane centre. */
            target_u = (cur_lane == line_lane) ? u_base : (cur_lane + 0.5) * lane_w;
        }
    }
    (void)aggression;

    /* [task#19 2026-06-12] TD6 DRIVABLE-BAND clamp. On wide TD6 city spans only
     * the central route band is paved road; the outer width is plaza/sidewalk,
     * and the rail width PULSES ~6x (a 16000u plaza span next to a 2620u "gate"
     * span). The spread logic above fans cars across the FULL width into the
     * plaza; when the road then narrows to the gate they SLAM the wall -> the
     * reported zig-zag/scrape/slow. Confine the target to the band spanned by the
     * LEFT and RIGHT route lines (the actual drivable road) so the car threads
     * the gates. WALL_MARGIN (0.11 of the full width) is far too loose on a
     * 16000u span (1760u from the rail is still deep in the plaza). Native TD5 is
     * untouched (the whole width is road there). Skipped on branch corridors
     * (handled by the on_branch path). Knob TD5RE_TD6_AI_BAND=0 disables. */
    if (g_active_td6_level > 0 && !on_branch && td6_ai_band_enabled()) {
        const uint8_t *lt  = g_route_tables[0];
        const uint8_t *rtb = g_route_tables[1];
        size_t idx = (size_t)(unsigned)look_span * 3u;
        if (lt && rtb && idx < g_route_table_sizes[0] && idx < g_route_table_sizes[1]) {
            double ul = (double)lt[idx]  / 256.0;
            double ur = (double)rtb[idx] / 256.0;
            if (ul > ur) { double t = ul; ul = ur; ur = t; }
            /* SHRINK the band slightly toward its centre (not widen it): the road
             * width pulses ~6x, so on a wide/plaza span a car sitting at the band
             * EDGE is fine, but when the road narrows to the next "gate" span that
             * same edge IS the wall. Keeping cars inside the route lines (with a
             * little inward bias) lets them thread the gates without scraping. */
            double m = (ur - ul) * 0.10;          /* pull each side inward 10% */
            ul += m; ur -= m;
            if (ur < ul) { double c = (ul + ur) * 0.5; ul = ur = c; }
            if (target_u < ul) target_u = ul;
            if (target_u > ur) target_u = ur;
        }
    }

    /* (4) WALL: never aim within WALL_MARGIN of a rail — the single guard that
     * keeps the car off the boundary whether the authored line hugs a rail, a
     * surface nudge went wide, or we're threading a tight branch. */
    double clamp_margin = rays_on ? SMART_RAY_MARGIN : WALL_MARGIN;
    if (target_u < clamp_margin)       target_u = clamp_margin;
    if (target_u > 1.0 - clamp_margin) target_u = 1.0 - clamp_margin;

    double best_u = target_u;

    /* --- Smooth the physical move (rate-limit in u-space) so the car holds a
     * clean line and any correction is one gentle slide, not a jerk. Because
     * target_u is a CONTINUOUS function of the situation (not a discrete lane
     * pick), it doesn't flip tick-to-tick, so no hysteresis/commitment is
     * needed — the rate limit alone keeps it smooth. -------------------------*/
    double cu = g_smart_lane_u[slot];
    if (cu < 0.0 || cu > 1.0) cu = u_self;     /* first sight / post-reset snap */
    double max_du = 0.022 + skill * 0.028;     /* ~2.2%..5% of road width per tick */
    if (on_branch) {
        if (cu > best_u + 0.12 || cu < best_u - 0.12)
            cu = u_self;                       /* shed a stale wide-road offset */
        max_du = 0.12;                         /* converge onto the branch line fast */
    }
    if (best_u > cu + max_du)      cu += max_du;
    else if (best_u < cu - max_du) cu -= max_du;
    else                           cu = best_u;
    g_smart_lane_u[slot] = cu;

    /* Bias = perpendicular shift from the route line to fraction `cu`. */
    int32_t cur = (int32_t)((cu - u_base) * cwidth);
    rs[RS_TRACK_OFFSET_BIAS] = cur;

    if ((g_ai_frame_counter % 60u) == 0u) {
        TD5_LOG_I(LOG_TAG, "smart_lane: slot=%d skill=%.2f span=%d L=%d "
                  "u_self=%.2f u_base=%.2f u_tgt=%.2f u_cur=%.2f bias=%d nb=%d pull=%.2f%s",
                  slot, (double)skill, span, L, u_self, u_base, best_u, cu,
                  (int)cur, nb, g_smart_branch_pull[slot], on_branch ? " [on_branch]" : "");
    }
}

/* Speed brain (car-following): after route_threshold sets throttle, if a car
 * we can't yet clear sits ahead in our corridor and we're closing, ease the
 * throttle (or brake when very close). Never raises throttle. */
static void td5_ai_smart_speed(int slot) {
    char *actor = actor_ptr(slot);
    int span_count = td5_track_get_span_count();
    if (span_count <= 0) return;
    int span_raw = (int)ACTOR_I16(actor, ACTOR_SPAN_RAW);

    float skill = td5_ai_smart_skill(slot);
    int aggression = g_td5.ini.smart_ai_aggression;
    int lookahead = 3 + (int)(skill * 5.0f);

    /* Skill -> PACE. Give higher-skill cars genuinely more speed and lower-skill
     * a touch less (~±8%), so the field has real pace differences. Without this
     * the cars equalize and drive in a fixed formation with nothing to overtake;
     * with it, faster cars catch slower ones and the overtake logic resolves the
     * pass. The gentle leash still keeps the pack from spreading too far. */
    {
        int16_t thr = ACTOR_I16(actor, ACTOR_ENCOUNTER_STEER);
        if (thr > 0) {
            double f = 0.96 + (double)skill * 0.08;     /* ~±4%: variation, not field-spread */
            /* Do NOT add corner-entry speed: on a sharp bend cap the boost so a
             * fast car can't overcook into the wall (Newcastle span ~137). The
             * low-skill REDUCTION still applies (slower = safer into a corner). */
            if (f > 1.0) {
                int hs = (span_raw + 7) % span_count;
                int hd = (td5_track_get_primary_route_heading(hs)
                          - td5_track_get_primary_route_heading(span_raw)) & 0xFFF;
                if (hd > 0x800) hd -= 0x1000;
                if (hd < 0) hd = -hd;
                if (hd > 0x140) f = 1.0;            /* ~28° bend ahead → no boost */
            }
            int v = (int)((double)thr * f);
            if (v > 0x7FFF) v = 0x7FFF;
            ACTOR_I16(actor, ACTOR_ENCOUNTER_STEER) = (int16_t)v;
        }
    }

    /* RAY BRAIN corner governor + wall safety. Only ever LOWERS the command
     * (fast-out is automatic: no bend ahead -> cap 1.0 -> no change), so the
     * faithful throttle ceiling is preserved. */
    if (g_td5.ini.smart_ai_rays) {
        SmartCorner co; SmartSense se;
        smart_corner_eval(slot, span_raw, span_count, 0.5, skill, &co);
        smart_sense(slot, span_raw, span_count, skill, &se);
        int16_t thr0 = ACTOR_I16(actor, ACTOR_ENCOUNTER_STEER);
        int32_t self_speed = ACTOR_I32(actor, ACTOR_LONGITUDINAL_SPEED);

        if (se.wall_imminent && self_speed > 0x4000) {
            /* aimed at a wall while moving -> brake to stay off the boundary */
            ACTOR_U8(actor, ACTOR_BRAKE_FLAG)       = 1;
            ACTOR_I16(actor, ACTOR_ENCOUNTER_STEER) = 0;
            ACTOR_U8(actor, ACTOR_THROTTLE_STATE)   = 1;
            if ((g_ai_frame_counter % 60u) == 0u)
                TD5_LOG_I(LOG_TAG, "smart_ray_wall: slot=%d brake front=%.0f v=%d",
                          slot, se.front_clear, (int)self_speed);
        } else if (co.speed_cap < 0.999 && thr0 > 0 &&
                   co.apex_d <= (4 + (int)(skill * 6.0f))) {
            /* slow-in: ease toward the corner cap before the apex */
            int v = (int)((double)thr0 * co.speed_cap);
            if (v < 0) v = 0;
            ACTOR_I16(actor, ACTOR_ENCOUNTER_STEER) = (int16_t)v;
            if (co.speed_cap < 0.55 && self_speed > 0xA000 && co.apex_d <= 2) {
                ACTOR_U8(actor, ACTOR_BRAKE_FLAG)     = 1;
                ACTOR_U8(actor, ACTOR_THROTTLE_STATE) = 1;
            }
            if ((g_ai_frame_counter % 60u) == 0u)
                TD5_LOG_I(LOG_TAG, "smart_ray_corner: slot=%d cap=%.2f apex=%d sharp=%.2f thr %d->%d",
                          slot, co.speed_cap, co.apex_d, co.sharpness, (int)thr0, v);
        }
    }

    int L = td5_track_get_span_lane_count(span_raw);
    if (L < 1) L = 1;
    double lane_w = 1.0 / (double)L;
    double u_self = smart_actor_u(actor, span_raw);

    SmartBlocker bl[TD5_MAX_TOTAL_ACTORS];
    int nb = smart_gather_blockers(slot, span_raw, span_count, lookahead,
                                   bl, TD5_MAX_TOTAL_ACTORS);
    int nearest_gap = 99; int32_t peer_speed = 0; int have = 0;
    for (int k = 0; k < nb; k++) {
        if (bl[k].gap < 0) continue;
        double du = bl[k].u - u_self; if (du < 0) du = -du;
        if (du < lane_w * 0.8 && bl[k].gap < nearest_gap) {
            nearest_gap = bl[k].gap; peer_speed = bl[k].speed; have = 1;
        }
    }

    /* Branch-road ADAPT: a car that wandered onto a branch must keep driving it
     * out, not sit there. If the road ahead is clear, override any brake/coast
     * the route-threshold left and commit a forward throttle. (Car-following
     * below still slows it if a peer is genuinely close in the same lane.) */
    {
        int rl = td5_track_get_ring_length();
        if (rl > 0 && span_raw >= rl && (!have || nearest_gap > 2)) {
            ACTOR_U8(actor, ACTOR_BRAKE_FLAG) = 0;
            ACTOR_U8(actor, ACTOR_THROTTLE_STATE) = 0;
            int16_t thr_b = ACTOR_I16(actor, ACTOR_ENCOUNTER_STEER);
            if (thr_b <= 0x40) {               /* coasting/braking → drive */
                ACTOR_I16(actor, ACTOR_ENCOUNTER_STEER) = (int16_t)0xA0;
                if ((g_ai_frame_counter % 60u) == 0u)
                    TD5_LOG_I(LOG_TAG, "smart_branch_drive: slot=%d span=%d thr %d->0xA0 (adapt)",
                              slot, span_raw, (int)thr_b);
            }
        }
    }

    if (!have) return;

    int32_t self_speed = ACTOR_I32(actor, ACTOR_LONGITUDINAL_SPEED);
    int desired = 2 - aggression;            /* clean=2 realistic=1 aggressive=1 (follow close) */
    if (desired < 1) desired = 1;
    int16_t thr = ACTOR_I16(actor, ACTOR_ENCOUNTER_STEER);

    if (nearest_gap <= desired && self_speed > peer_speed) {
        if (nearest_gap <= 1 && (self_speed - peer_speed) > 0x6000) {
            ACTOR_U8(actor, ACTOR_BRAKE_FLAG)     = 1;
            ACTOR_I16(actor, ACTOR_ENCOUNTER_STEER) = 0;
            ACTOR_U8(actor, ACTOR_THROTTLE_STATE) = 1;
        } else if (thr > 0) {
            double f = (double)nearest_gap / (double)(desired + 1);
            if (f < 0.66) f = 0.66;   /* follow close, keep pace; light contact OK */
            ACTOR_I16(actor, ACTOR_ENCOUNTER_STEER) = (int16_t)((double)thr * f);
        }
        if ((g_ai_frame_counter % 60u) == 0u) {
            TD5_LOG_I(LOG_TAG, "smart_follow: slot=%d gap=%d desired=%d "
                      "self_v=%d peer_v=%d thr0=%d", slot, nearest_gap, desired,
                      (int)self_speed, (int)peer_speed, (int)thr);
        }
    }
}

/* Gentle symmetric catch-up leash. Returns 1 and writes *out_modifier to be
 * used in place of the faithful rubber-band modifier; 0x100-modifier becomes
 * the throttle bias, so behind→boost / ahead→trim, capped to a small band. */
static int td5_ai_smart_leash_modifier(int slot, int32_t delta, int32_t *out_modifier) {
    (void)slot;
    int s = g_td5.ini.smart_ai_leash;
    if (s <= 0) { *out_modifier = 0; return 1; }   /* leash off → pure skill */
    int32_t range = 0x40;
    int32_t d = delta;
    if (d >  range) d =  range;
    if (d < -range) d = -range;
    int32_t maxmod = (0x30 * s) / 9;               /* up to ~0x30 (~19%) */
    *out_modifier = (d * maxmod) / range;          /* symmetric, signed */
    return 1;
}

/* Traffic lane brain: score lanes by surface, occupancy ahead, wall, and
 * change-cost; return the best, clamped to a ±1 lane step so the change is
 * gradual (a multi-lane jump would slam the steering cascade). */
static int td5_ai_smart_traffic_lane(int slot, int target_span, int lane_count,
                                     int base_sub_lane, int polarity) {
    (void)polarity;
    if (lane_count <= 1) return base_sub_lane;
    if (lane_count > SMART_MAX_LANES) lane_count = SMART_MAX_LANES;

    char *actor = actor_ptr(slot);
    int self_span  = (int)ACTOR_I16(actor, ACTOR_SPAN_RAW);
    int span_count = td5_track_get_span_count();
    float skill = td5_ai_smart_skill(slot);
    int lookahead = 3 + (int)(skill * 4.0f);

    int base = base_sub_lane;
    if (base < 0) base = 0;
    if (base >= lane_count) base = lane_count - 1;

    /* [R3-10] Anti-pileup lane commit: while a recent jam-escape's lane bias is
     * live (set by traffic_collision_escape), shift the reference lane toward the
     * cleared side so the scoring + ±1 step below actually move this car OUT of
     * the lane it kept re-jamming in, instead of the occupancy score settling it
     * right back. Deterministic (replicated escape state). */
    if (s_traffic_escape_lane_ttl[slot] > 0 && s_traffic_escape_lane[slot] != 0) {
        base += (int)s_traffic_escape_lane[slot];
        if (base < 0) base = 0;
        if (base >= lane_count) base = lane_count - 1;
    }

    int occ[SMART_MAX_LANES];
    for (int l = 0; l < lane_count; l++) occ[l] = 0;
    int n = g_active_actor_count;
    if (n > TD5_MAX_TOTAL_ACTORS) n = TD5_MAX_TOTAL_ACTORS;
    for (int i = 0; i < n; i++) {
        if (i == slot) continue;
        if (!ai_peer_is_present(i)) continue;   /* [R3-8] skip phantom/absent slots */
        char *a = actor_ptr(i);
        int psp = (int)ACTOR_I16(a, ACTOR_SPAN_RAW);
        int gap = smart_span_gap(self_span, psp, span_count);
        if (gap < -1 || gap > lookahead) continue;
        int pl = (int)ACTOR_U8(a, ACTOR_SUB_LANE_INDEX);
        if (pl >= 0 && pl < lane_count)
            occ[pl] += (lookahead - (gap < 0 ? 0 : gap) + 1);
    }

    double best_score = 1e18; int best = base;
    for (int l = 0; l < lane_count; l++) {
        double score = 0.0;
        if (td5_track_surface_is_slow(td5_track_get_span_lane_surface(target_span, l)))
            score += 6.0;
        score += (double)occ[l] * 1.5;
        if (l == 0 || l == lane_count - 1) score += 0.6;
        score += (double)smart_iabs(l - base) * (1.4 - skill * 0.7);
        if (score < best_score) { best_score = score; best = l; }
    }
    if (best > base + 1) best = base + 1;
    else if (best < base - 1) best = base - 1;

    if (best != base && (g_ai_frame_counter % 90u) == 0u)
        TD5_LOG_I(LOG_TAG, "smart_traffic_lane: slot=%d base=%d -> %d "
                  "(lanes=%d tspan=%d)", slot, base, best, lane_count, target_span);
    return best;
}

/* Traffic car-following: scale the cruise throttle down when a car sits close
 * ahead in the same lane. Returns a 0..256 fixed-point scale. */
static int td5_ai_smart_traffic_cruise_scale(int slot) {
    char *actor = actor_ptr(slot);
    int span_count = td5_track_get_span_count();
    int self_span = (int)ACTOR_I16(actor, ACTOR_SPAN_RAW);
    int self_lane = (int)ACTOR_U8(actor, ACTOR_SUB_LANE_INDEX);
    int n = g_active_actor_count;
    if (n > TD5_MAX_TOTAL_ACTORS) n = TD5_MAX_TOTAL_ACTORS;
    int nearest = 99;
    for (int i = 0; i < n; i++) {
        if (i == slot) continue;
        if (!ai_peer_is_present(i)) continue;   /* [R3-8] skip phantom/absent slots */
        char *a = actor_ptr(i);
        if ((int)ACTOR_U8(a, ACTOR_SUB_LANE_INDEX) != self_lane) continue;
        int gap = smart_span_gap(self_span, (int)ACTOR_I16(a, ACTOR_SPAN_RAW), span_count);
        if (gap >= 0 && gap < nearest) nearest = gap;
    }
    if (nearest <= 1) return 96;    /* ~0.375x */
    if (nearest == 2) return 176;   /* ~0.69x  */
    if (nearest <= 4) return 224;   /* ~0.875x */
    return 256;                     /* clear   */
}

void td5_ai_update_track_behavior(int slot) {
    int32_t *rs = route_state(slot);
    char *actor  = actor_ptr(slot);
    int32_t heading, route_heading, hdelta;
    int threshold_result;
    int32_t steer_weight;

    /* Pool9 pilot capture: entry snapshot before any state mutation. */

    /* Calls-trace probe: capture entry state per slot per tick.
     * Hooks YAML: re/trace-hooks/tick0_ai_chain.yaml
     * Original RVA: 0x00434FE0 UpdateActorTrackBehavior
     * Args layout: slot, steer_cmd, yaw_accum, span_norm, vlong, encounter_steer, throttle_byte, brake_flag */
    TD5_TRACE_CALL_ENTER("ai_track_behavior",
        slot,
        ACTOR_I32(actor, ACTOR_STEERING_CMD),
        ACTOR_I32(actor, ACTOR_YAW_ACCUM),
        (int32_t)ACTOR_I16(actor, ACTOR_SPAN_NORMALIZED),
        ACTOR_I32(actor, ACTOR_LONGITUDINAL_SPEED),
        (int32_t)ACTOR_I16(actor, ACTOR_ENCOUNTER_STEER),
        (int32_t)ACTOR_U8(actor, ACTOR_THROTTLE_STATE),
        (int32_t)ACTOR_U8(actor, ACTOR_BRAKE_FLAG));

    /* Time trial used to skip AI track behavior entirely (slot 0 was assumed
     * human). With PlayerIsAI=1 we deliberately route slot 0 through the AI,
     * so the gate is removed — racer_count is already 1 in TT, so only slot 0
     * reaches this function anyway. */

    /* AI probe (debug-level): state matching frida_ai_probe.js for side-by-side
     * diff. Keep at TD5_LOG_D to avoid flooding INFO logs. Match fields with
     * tools/frida_ai_probe.js UAB onLeave sample. */
    TD5_LOG_D(LOG_TAG, "ai_probe: slot=%d yaw=%d steer=%d wx=%d wz=%d lspd=%d ld=%d rd=%d",
              slot,
              ACTOR_I32(actor, ACTOR_YAW_ACCUM),
              ACTOR_I32(actor, ACTOR_STEERING_CMD),
              ACTOR_I32(actor, 0x1FC),
              ACTOR_I32(actor, 0x204),
              ACTOR_I32(actor, 0x314),
              rs[RS_LEFT_DEVIATION], rs[RS_RIGHT_DEVIATION]);

    /* No countdown pre-seed: the original at 0x00434FE0 has NO countdown
     * gate and NO paused-branch write of encounter_steering_cmd (+0x33E) /
     * brake_flag (+0x36D) — the cascade below reaches
     * td5_ai_update_route_threshold which writes the same
     * g_actor_route_steer_bias[slot] → encounter_steering_cmd on the
     * coasting/normal branch (see td5_ai.c:968-969). Keeping an explicit
     * pre-seed here was redundant with the original and raced with the
     * cascade output during countdown. */

    /* --- Game-type gate: scripts + misalignment-recovery only fire in SINGLE_RACE ---
     *
     * Original FUN_00434FE0 @ 0x00434FE0 has an outer
     *   if (g_selectedGameType == 0) { script-or-trigger }
     * gate at the function entry. Disassembly:
     *   0x00434FE0  MOV EAX, [0x004aaf6c]    ; g_selectedGameType
     *   0x00434FE8  TEST EAX, EAX
     *   0x00434FF2  JNZ  0x004350a5          ; jump past both inner branches
     *
     * SINGLE_RACE = 0 in TD5_GameType. For modes != SINGLE_RACE
     * (CHAMPIONSHIP, MASTERS, TT, COP_CHASE, DRAG_RACE, ...) the original
     * skips both the script-execution path and the heading-misalignment
     * recovery-script-set path, falling straight through to normal track
     * following. Without this gate, AI in non-SINGLE_RACE modes was
     * receiving recovery scripts the original never sets — the +0x4000
     * per-tick steering ramp produced wall-leaning behaviour for ~150
     * ticks regardless of contact state.
     * [CONFIRMED via Ghidra disassembly + decompilation @ 0x00434FE0]. */
    /* SmartAI: detect a branch-road span (appended detour, span_raw beyond the
     * main ring). On a branch we make the car ADAPT and drive it rather than
     * run the faithful heading-misalignment recovery, which otherwise brakes a
     * car that wandered onto a branch into a standstill — the "refuses to drive
     * the branch it entered by mistake" report (Hong Kong). */
    int smart_on_branch = 0;
    if (td5_ai_smart_active()) {
        int sr_b = (int)ACTOR_I16(actor, ACTOR_SPAN_RAW);
        int rl_b = td5_track_get_ring_length();
        smart_on_branch = (rl_b > 0 && sr_b >= rl_b);
    }

    if (g_td5.game_type == TD5_GAMETYPE_SINGLE_RACE) {
        /* SmartAI on a branch: cancel any active recovery/brake script and skip
         * the misalignment trigger below, so the normal path drives the branch
         * route forward instead of freezing. */
        if (smart_on_branch && rs[RS_SCRIPT_BASE_PTR] != 0) {
            rs[RS_SCRIPT_BASE_PTR] = 0;
            rs[RS_SCRIPT_IP] = 0;
        }
        /* --- Script check: if a script is active, run it --- */
        if (rs[RS_SCRIPT_BASE_PTR] != 0) {
            int result = td5_ai_advance_track_script(rs);
            if (result == 0) {
                /* Script still running (blocking). Original 0x00435095-0x004350A3
                 * recomputes RS_TRACK_PROGRESS + RS_TRACK_OFFSET_BIAS from the
                 * actor's CURRENT span/world-pos before returning, so they don't
                 * go stale during long brake-wait or recovery scripts. */
                int span_raw  = (int)(int16_t)ACTOR_I16(actor, ACTOR_SPAN_RAW);
                int span_norm = (int)(int16_t)ACTOR_I16(actor, ACTOR_SPAN_NORMALIZED);
                int32_t pos_vec[3];
                pos_vec[0] = ACTOR_I32(actor, ACTOR_WORLD_POS_X);
                pos_vec[1] = 0;
                pos_vec[2] = ACTOR_I32(actor, ACTOR_WORLD_POS_Z);
                int64_t prog64 = td5_track_compute_span_progress(span_raw, pos_vec);
                int progress = (int)(int32_t)(uint32_t)(prog64 & 0xFFFFFFFFu);
                rs[RS_TRACK_PROGRESS] = progress;
                const uint8_t *route_bytes = (const uint8_t *)(intptr_t)rs[RS_ROUTE_TABLE_PTR];
                int route_byte = 0;
                if (route_bytes && span_norm >= 0) {
                    route_byte = ai_route_byte(route_bytes, span_norm, 0);
                }
                rs[RS_TRACK_OFFSET_BIAS] =
                    td5_track_compute_signed_offset(span_raw, progress, route_byte);
                return;
            }
            /* result == 1: script complete, fall through to normal AI */
        }

        /* --- Heading misalignment trigger: start recovery script --- */
        heading = ACTOR_I32(actor, ACTOR_YAW_ACCUM) >> 8;

        /* Compute route heading inline from route byte, matching original @ 0x43503A. */
        route_heading = ai_route_heading_for_actor(rs, actor);

        /* Formula: uVar3 = -(((heading - route_heading - 0x800U & 0xFFF) - 0x800 & 0xFFF) & 0xFFF) */
        {
            uint32_t adjusted = (((uint32_t)(heading - route_heading) - 0x800U) & 0xFFF);
            adjusted = (adjusted - 0x800U) & 0xFFF;
            hdelta = (int32_t)(-(int32_t)adjusted) & 0xFFF;

            /* [CONFIRMED @ 0x00434FE0] decomp: if ((800 < uVar3) && (uVar3 < 0xce0))
             * — 800 is DECIMAL (= 0x320), upper is strict <.
             *
             * Suppress the recovery-script set when route data is absent.
             * Without LEFT.TRK/RIGHT.TRK, ai_route_heading_for_actor returns
             * a hard-coded 0, which makes hdelta a function purely of actor
             * yaw + 0x800. A car spawned roughly aligned with the world axes
             * trivially falls inside (0x320, 0xCE0), firing a spurious
             * recovery-script set on the first sub-tick. orig with route data
             * loaded computes a meaningful route_heading and does NOT enter
             * recovery. */
            if (rs[RS_ROUTE_TABLE_PTR] != 0 && hdelta > 0x320 && hdelta < 0xCE0
                && !smart_on_branch) {
                TD5_LOG_I(LOG_TAG, "recovery: slot=%d hdelta=0x%X heading=0x%X route=0x%X gt=%d",
                          slot, hdelta, heading & 0xFFF, route_heading & 0xFFF,
                          (int)g_td5.game_type);
                /* [PRECISE-PORT D1 narrowed 2026-05-14, pool9_00434FE0]:
                 * the original at 0x00435094-0x0043509F writes ONLY the two
                 * pointer fields, both with the same recovery-script pointer
                 * (DAT_00473cc8). The four extra writes (FLAGS, FIELD_3E,
                 * FIELD_43, COUNTDOWN) had no counterpart in the listing and
                 * may have over-aggressive interactions with the script VM
                 * (AdvanceActorTrackScript) which is responsible for stepping
                 * those fields itself.
                 *
                 *   MOV [EDI + 0x4afc48], 0x473cc8   ; RS_SCRIPT_BASE_PTR
                 *   MOV [EDI + 0x4afc4c], 0x473cc8   ; RS_SCRIPT_IP
                 *
                 * IMPORTANT semantic divergence vs original RS_SCRIPT_IP:
                 * the original at 0x00437405-0x0043740B dispatches opcodes via
                 *   MOV EAX, [ESI + 0xec]   ; ip = raw pointer to opcode dword
                 *   MOV ECX, [EAX]          ; *ip  (pointer-deref)
                 * so writing the program-base pointer into rs[+0xEC] means
                 * "ip points at opcode[0]". The PORT implements ip as an
                 * INTEGER INDEX and dispatches via `base[ip]` (see line ~2435).
                 * The index-equivalent of "start of program" is 0. Writing the
                 * pointer value (0x473cc8 / ~5M) here was treated as an index
                 * and crashed AdvanceActorTrackScript's `mov (%edx,%ebp,4),%eax`
                 * with EBP=ip=0x5194D8, walking off into the .rdata texture
                 * table at 0x49D1A4. Reverted to ip=0 to match port semantics. */
                rs[RS_SCRIPT_BASE_PTR] = (int32_t)(intptr_t)g_script_init_recovery;
                rs[RS_SCRIPT_IP]       = 0;
                return;
            }
        }
    } else {
        /* Suppress compiler warnings for unused locals when the gate is closed. */
        (void)heading; (void)route_heading; (void)hdelta;
    }

    /* --- Normal AI path following --- */

    /* 1. Lateral offset targeting.
     *    SmartAI: replace the faithful nearest-peer nudge with the lane brain
     *    (surface/occupancy/wall/racing-line/branch scoring). Off → faithful. */
    if (td5_ai_smart_active()) {
        td5_ai_smart_lane_bias(slot);
    } else {
        td5_ai_update_track_offset_bias(slot);
    }

    /* 2. Look-ahead waypoint sampling + deviation computation
     *    [CONFIRMED @ 0x43523D-0x435372]
     *
     *    Original pipeline:
     *      a) Compute target span = current + 4 (wrapped by span count)
     *      b) SampleTrackTargetPoint -> world XZ of target point
     *      c) dx = (target_x - actor_x) >> 8, dz = (target_z - actor_z) >> 8
     *      d) target_angle = AngleFromVector12(dx, dz) with 4-quadrant expansion
     *      e) actor_heading = ((yaw_accum + steering_cmd) >> 8) & 0xFFF
     *      f) delta = actor_heading - target_angle
     *      g) Decompose into LEFT_DEVIATION / RIGHT_DEVIATION
     */
    {
        /* Original at 0x00435243 reads MOVSX EAX, word ptr [actor+0x82] —
         * track_span_NORMALIZED, NOT track_span_raw [CONFIRMED via Frida +
         * Ghidra round-15 audit 2026-05-03]. Port had been reading
         * +0x80 (RAW) which on Honolulu sim_tick=1 produces target_span=106
         * vs orig's 97 — a 9-span gap that drives port's 3.4× over-steer
         * and yaw_torque overshoot, and is the root cause of the
         * Honolulu rollover residual after the engine-pin fix. */
        int16_t span = ACTOR_I16(actor, ACTOR_SPAN_NORMALIZED);
        int span_count = td5_track_get_span_count();
        if (span_count > 0 && span >= 0) {
            /* (a) Target span: 4 spans ahead, then remap through junction
             * table when the actor is NOT on the LEFT.TRK (canonical) route.
             * [CONFIRMED @ 0x00435180-0x00435260] Original walks
             * DAT_004c3da0 (= STRIP.DAT +0x14/+0x18) to jump across track
             * junctions to a physically-farther span; without this, the AI
             * target_angle is computed against a too-close point, producing
             * tiny first-tick deviations that land outside the cascade's
             * direct-add band and prevent the port from reaching the
             * saturated→wrap→stable oscillation regime the original uses.
             *
             * `is_canonical_route` = rs[RS_ROUTE_TABLE_SELECTOR] == 0
             * which the init path at td5_ai.c:573 maps to g_route_tables[0]
             * (LEFT.TRK). Original gates on pointer equality to DAT_004afb58
             * (LEFT.TRK blob ptr) — same intent. */
            int is_canonical = (rs[RS_ROUTE_TABLE_SELECTOR] == 0);
            int lin_span = ((int)span + 4) % span_count;
            int target_span = td5_track_apply_target_span_remap(lin_span, is_canonical);

            /* Adaptive lookahead (solo_mode_synth slot 0 only, 2026-05-22 v2).
             *
             * Root cause from [[target-angle-wobble-finding-2026-05-22]]: in
             * dense-span curve sections (Edinburgh spans 95-99) the +4
             * lookahead lands so close to the actor that atan2 wobbles 150°+
             * tick-to-tick. Cascade hits mid-band, slams ±0x18000, car spins.
             *
             * Two-threshold design — KEY DIFFERENCE from earlier reverted
             * "Solo-mode short-span guard" which extended until dist >= 1500
             * (overshot curves, slot 0 regressed to stuck-at-span-98):
             *   - TRIGGER 200: only extend when first sample is *very* close
             *   - RELEASE 600: stop extending as soon as distance is workable
             * Narrow extension window prevents overshooting the curve.
             *
             * Probe uses lateral_bias=0 so the trigger is independent of the
             * peer-bias emulation cycle; sample for steering downstream still
             * applies the real bias. */
            if (g_td5.solo_mode_synth && slot == 0) {
                enum { TD5_ADAPTIVE_LOOKAHEAD_TRIGGER = 200,
                       TD5_ADAPTIVE_LOOKAHEAD_RELEASE = 600,
                       TD5_ADAPTIVE_LOOKAHEAD_MAX_EXTEND = 6 };
                const uint8_t *rt_probe = (const uint8_t *)(intptr_t)rs[RS_ROUTE_TABLE_PTR];
                int probe_rb = 128;
                if (rt_probe) probe_rb = (int)rt_probe[(size_t)(unsigned)lin_span * 3u];
                int32_t actor_x_chk = ACTOR_I32(actor, ACTOR_WORLD_POS_X);
                int32_t actor_z_chk = ACTOR_I32(actor, ACTOR_WORLD_POS_Z);
                int target_x_probe = 0, target_z_probe = 0;
                int triggered = 0;
                if (td5_track_sample_target_point(target_span, probe_rb,
                                                   &target_x_probe, &target_z_probe, 0)) {
                    int32_t ddx = (target_x_probe - actor_x_chk) >> 8;
                    int32_t ddz = (target_z_probe - actor_z_chk) >> 8;
                    int32_t adx = ddx < 0 ? -ddx : ddx;
                    int32_t adz = ddz < 0 ? -ddz : ddz;
                    if (adx + adz < TD5_ADAPTIVE_LOOKAHEAD_TRIGGER) triggered = 1;
                }
                if (triggered) {
                    int extended = 0;
                    for (int e = 0; e < TD5_ADAPTIVE_LOOKAHEAD_MAX_EXTEND; e++) {
                        lin_span = (lin_span + 1) % span_count;
                        target_span = td5_track_apply_target_span_remap(lin_span, is_canonical);
                        if (rt_probe) probe_rb = (int)rt_probe[(size_t)(unsigned)lin_span * 3u];
                        extended = e + 1;
                        if (!td5_track_sample_target_point(target_span, probe_rb,
                                                            &target_x_probe, &target_z_probe, 0))
                            break;
                        int32_t ddx = (target_x_probe - actor_x_chk) >> 8;
                        int32_t ddz = (target_z_probe - actor_z_chk) >> 8;
                        int32_t adx = ddx < 0 ? -ddx : ddx;
                        int32_t adz = ddz < 0 ? -ddz : ddz;
                        if (adx + adz >= TD5_ADAPTIVE_LOOKAHEAD_RELEASE) break;
                    }
                    TD5_LOG_I(LOG_TAG, "adaptive_lookahead: slot=%d span=%d lin=%d tspan=%d ext=%d",
                              slot, (int)span, lin_span, target_span, extended);
                }
            }

            /* [PRECISE-PORT D4 removed 2026-05-14, pool9_00434FE0]: original
             * 0x00434FE0 does NOT write rs[RS_TRACK_PROGRESS] in the normal-AI
             * branch — that field is only written in the script-completed
             * sub-branch via ComputeTrackSpanProgress + ComputeSignedTrackOffset.
             * The port's extra td5_track_compute_spline_position(...) call here
             * corrupted RS_TRACK_PROGRESS every tick (e.g. -2081 vs orig 95 at
             * sim_tick=0), and cascaded into downstream callers that read this
             * field. The verbatim listing path simply does NOT touch it in this
             * branch. */

            /* (b) Get target world position via SampleTrackTargetPoint
             * [CONFIRMED @ 0x434800: 2-vertex interpolation with route_byte + lateral bias]
             * route_byte from route table controls left-right interpolation (0-255).
             * lateral_bias from RS_TRACK_OFFSET_BIAS applies perpendicular peer avoidance. */
            {
                int target_x = 0, target_z = 0;
                int route_byte = 128; /* default: center of span */
                int lateral_bias = rs[RS_TRACK_OFFSET_BIAS];

                /* Route byte uses PRE-junction-remap span (lin_span =
                 * current_span + 4 wrapped), NOT the post-remap target_span.
                 * [CONFIRMED @ 0x00435260-0x00435280 in FUN_00434FE0]:
                 *
                 *   iVar4 = *local_14 + 4;            // PRE-remap
                 *   if (DAT_004c3d90 <= iVar4)
                 *     iVar4 = (*local_14 - DAT_004c3d90) + 4;
                 *   FUN_00434800(iVar10,              // GEOMETRY = post-remap
                 *                (uint)*(byte *)(iVar4*3 + *piVar1),
                 *                ...);                // ROUTE_BYTE = pre-remap
                 *
                 * Geometry is sampled from the junction-aware span; lateral
                 * lane interpolation is read from the unremapped table. Using
                 * target_span here aimed the AI at the wrong lane offset on
                 * junction spans. */
                const uint8_t *route_bytes = (const uint8_t *)(intptr_t)rs[RS_ROUTE_TABLE_PTR];
                if (route_bytes) {
                    route_byte = (int)route_bytes[(size_t)(unsigned)lin_span * 3u];
                }
                TD5_LOG_I(LOG_TAG, "route_byte_pick: slot=%d lin=%d tspan=%d rb=%d",
                          slot, lin_span, target_span, route_byte);

                if (td5_track_sample_target_point(target_span, route_byte,
                                                   &target_x, &target_z, lateral_bias)) {
                    /* [POLICE rewrite 2026-06-19] A chasing cop drives the ROUTE
                     * like a fast AI racer (NOT homing on the target's exact
                     * position — that snapped its heading into the target's rear
                     * and DISCARDED the SmartAI lane/ray avoidance, so it rammed
                     * instead of overtaking). Route-following + SmartAI
                     * avoidance + the branch-follow override + the 1.5x catch-up
                     * throttle make it close in and OVERTAKE naturally, the way
                     * an AI opponent passes a slower car. */
                    /* (c) Delta vector: target - actor, shift from 24.8 FP to integer
                     * [CONFIRMED @ 0x4352A1-0x4352C8] */
                    int32_t actor_x = ACTOR_I32(actor, ACTOR_WORLD_POS_X);
                    int32_t actor_z = ACTOR_I32(actor, ACTOR_WORLD_POS_Z);
                    int32_t dx = (target_x - actor_x) >> 8;
                    int32_t dz = (target_z - actor_z) >> 8;

                    /* target_probe: AI look-ahead target per tick.
                     * Upgraded to INFO for /fix session 1776736681 — diagnoses
                     * Moscow spin-out (dx ~5000 vs orig ~80000). Revert to
                     * TD5_LOG_D once the 15× symptom is understood. */
                    TD5_LOG_I(LOG_TAG, "target_probe: slot=%d span=%d lin=%d tspan=%d rb=%d tx=%d tz=%d dx=%d dz=%d sel=%d actor=(%d,%d)",
                              slot, (int)span, lin_span, target_span, route_byte,
                              target_x, target_z, dx, dz, (int)rs[RS_ROUTE_TABLE_SELECTOR],
                              actor_x, actor_z);

                    /* (d) Compute target angle using atan2 equivalent
                     * [CONFIRMED @ 0x4352CB-0x435336: no +0x800 on target]
                     * The yaw_accum convention (angle+0x800)<<8 creates a delta
                     * baseline of ~0x800 for aligned cars. The steering bands at
                     * 0x4340C0 are designed around this offset — L~0x7FF enters
                     * the moderate-left band which turns the car slowly, matching
                     * the original's behavior. */
                    int32_t target_angle = ai_angle_from_vector(dx, dz) & 0xFFF;

                    /* Solo-mode target_angle smoothing (port-only, 2026-05-22).
                     *
                     * On dense-span curves the actor frequently sits within one
                     * tick's motion of the +4 lookahead target. atan2(dx,dz) is
                     * hyper-sensitive to small position changes when dx²+dz² is
                     * small → target_angle wobbles 150°+ between consecutive
                     * ticks at the same tspan (Frida-traced Edinburgh slot 1
                     * spans 95-97: ta 0x966→0x28F→0x98E→0x2B0). The cascade
                     * can't track those swings and saturates at ±clamp.
                     *
                     * In orig 6-racer mode peer interactions shift the lateral
                     * target each tick, masking the wobble. Solo PlayerIsAI has
                     * no peers (even with the phantom-peer bias emulation
                     * above the geometry-side wobble remains), so we IIR-filter
                     * target_angle here. Wrap-aware diff handles the 0xFFF↔0
                     * crossing; alpha=1/4 is heavily smoothed (response
                     * timescale ~4 ticks @ 30Hz = ~130 ms).
                     *
                     * Strictly gated to solo_mode_synth slot 0 — 6-racer mode
                     * keeps byte-fidelity with orig 0x4352CB-0x435336.
                     */
                    if (g_td5.solo_mode_synth && slot == 0) {
                        static int32_t s_smoothed_ta = -1;
                        /* Conditional near-target smoothing. Only smooth when
                         * |dx|+|dz| < 600 in post->>8 fixed-point units. Far-
                         * target atan2 is already stable; smoothing there
                         * adds lag at sharp turns. Near-target is where the
                         * wobble cascade fires (Edinburgh slot 1 spans 95-97
                         * Frida trace). Re-entering the near band: reset
                         * smoother to current raw so we don't snap-back. */
                        int32_t abs_dx = (dx < 0) ? -dx : dx;
                        int32_t abs_dz = (dz < 0) ? -dz : dz;
                        int near_target = (abs_dx + abs_dz) < 600;

                        if (s_smoothed_ta < 0 || !near_target) {
                            s_smoothed_ta = target_angle;
                        } else {
                            int32_t diff = target_angle - s_smoothed_ta;
                            if (diff >  0x800) diff -= 0x1000;
                            if (diff < -0x800) diff += 0x1000;
                            s_smoothed_ta = (s_smoothed_ta + (diff >> 2)) & 0xFFF;
                            TD5_LOG_D(LOG_TAG, "ta_smooth: slot=%d raw=0x%X smooth=0x%X dxz=%d",
                                      slot, target_angle, s_smoothed_ta, abs_dx + abs_dz);
                            target_angle = s_smoothed_ta;
                        }
                    }

                    /* (e) Actor heading: (yaw_accum + steering_cmd) >> 8
                     * [CONFIRMED @ 0x435338-0x43534C] */
                    int32_t yaw = ACTOR_I32(actor, ACTOR_YAW_ACCUM);
                    int32_t steer = ACTOR_I32(actor, ACTOR_STEERING_CMD);
                    int32_t actor_heading = ((yaw + steer) >> 8) & 0xFFF;

                    /* (f) Delta = actor_heading - target_angle
                     * [CONFIRMED @ 0x435352] */
                    int32_t delta = actor_heading - target_angle;

                    /* (g) Decompose into left/right deviation
                     * [CONFIRMED @ 0x435354-0x435372 — formula verified
                     * faithful via raw disasm 2026-05-12]
                     *
                     * [PRECISE-PORT D2 removed 2026-05-14, pool9_00434FE0]:
                     * the prior `abs_delta < 64` clamp was an explicit
                     * workaround comment to mask a seed-progress 1-unit
                     * divergence in td5_track_compute_span_progress. The
                     * original at 0x00435354-0x00435372 has NO such clamp —
                     * the listing decomposes into the two branches below
                     * directly. Per re/analysis/precise_port_workflow.md and
                     * feedback_precise_port_over_approximation: remove the
                     * over-approximation, let the upstream divergence
                     * surface so it can be fixed at source. */
                    int32_t abs_delta = delta < 0 ? -delta : delta;
                    if (delta >= 0) {
                        rs[RS_LEFT_DEVIATION]  = 0xFFF - abs_delta;
                        rs[RS_RIGHT_DEVIATION] = delta;
                    } else {
                        rs[RS_LEFT_DEVIATION]  = abs_delta;
                        rs[RS_RIGHT_DEVIATION] = delta + 0xFFF;
                    }

                    TD5_LOG_D(LOG_TAG, "deviation: slot=%d tspan=%d ta=0x%X "
                              "hd=0x%X delta=%d L=%d R=%d",
                              slot, target_span, target_angle,
                              actor_heading, delta,
                              rs[RS_LEFT_DEVIATION], rs[RS_RIGHT_DEVIATION]);
                }
            }
        }
    }

    /* 3. Throttle/brake decision */
    threshold_result = td5_ai_update_route_threshold(slot);

    /* 3b. SmartAI: car-following speed control. Runs AFTER route_threshold has
     *     written the throttle, and only eases / brakes (never raises it), so
     *     the corner-speed governor stays the upper bound. */
    if (td5_ai_smart_active()) {
        td5_ai_smart_speed(slot);
    }

    /* 5. Steering: band-based steering-command controller.
     *
     * Faithful port of UpdateActorSteeringBias @ 0x004340C0. Writes to
     * actor+0x30C (STEERING_CMD) only, never to +0x1C4 (angular_velocity_yaw).
     * Original dispatches on threshold result:
     *   threshold != 0 → weight = 0x10000  [CONFIRMED @ 0x00435392]
     *   threshold == 0 → weight = 0x20000  [CONFIRMED @ 0x0043539F]
     *
     * Omega is NOT touched here. Angular velocity evolves from the bicycle
     * model in UpdateAIVehicleDynamics @ 0x00404EC0, which at v=0 produces
     * ~0 torque and therefore ~0 omega, so the countdown grid-hold period
     * naturally leaves actors still. */
    steer_weight = (threshold_result != 0) ? 0x10000 : 0x20000;
    steer_weight = td5_ai_td6_steer_weight(steer_weight);  /* [task#19] TD6: damp mid-band slam */
    td5_ai_update_steering_bias(rs, steer_weight);
    TD5_LOG_I(LOG_TAG, "track_behavior: slot=%d thr=%d weight=0x%X",
              slot, threshold_result, steer_weight);
}

/* ========================================================================
 * Traffic System
 *
 *   0x4353B0  RecycleTrafficActorFromQueue
 *   0x435940  InitializeTrafficActorsFromQueue
 *   0x435E80  UpdateTrafficRoutePlan
 *   0x433680  FindNearestRoutePeer
 *
 * Traffic actors occupy slots 6-11. They use:
 *   - Constant speed: encounter_steering_override = 0x3C (60)
 *   - 7-stage FSM: recycle -> heading -> edge -> normal -> target -> steer -> yield
 * ======================================================================== */

/**
 * FindNearestRoutePeer (precise-00433680): byte-faithful port of the original
 * at 0x00433680. Replaces the prior simplified-by-active-count scan.
 *
 * [CONFIRMED @ disassembly 0x00433680-0x004337D1 — full pool6 audit.]
 *
 * Two near-identical loops gated on `route_state[+0xfc]` (the dword that
 * Ghidra symbolises as `gActorRouteDirectionPolarity`, i.e. RS dword index
 * 0x3F — NOT the field already known in this port as `RS_DIRECTION_POLARITY`
 * which is index 0x25 and indexes a separate byte at RS+0x94 used elsewhere
 * for the traffic heading-mirror branch). The +0xfc field controls direction
 * sense for the peer scan: 0 → look ahead (peer ahead of self); nonzero →
 * look behind (peer behind self).
 *
 * Both loops iterate the FULL 12-slot actor pool (slots 0..11), gated by the
 * EBX/EBP < 0x4b08bc terminator at 0x0043371a/0x004337b3 — i.e. the
 * route-state stride times 12 from base 0x004afb6c. The prior port gated on
 * `g_active_actor_count`; the original does not.
 *
 * Same-actor skip: `i == self_slot` skips the slot whose RS_SLOT_INDEX
 * (dword 0x35 = `gActorTrackReferenceSlot`) matches the caller's iVar2.
 *
 * Lane match: byte at actor+0x8c == self.field_0x8c.
 *
 * Route-table identity: dword at RS[0x00] (`gActorRouteTableSelector` =
 * the route-table base POINTER, NOT the +0x0c selector at index 0x03)
 * compared between self.RS[0] and peer.RS[0]. The prior port compared
 * peer.RS[0x03] to self_selector — that was the wrong field.
 *
 * Distance gate: `0 < dist <= 0x20` for the final accept (TEST/JZ at
 * 0x00433726 + CMP 0x20/JG at 0x0043372e). Prior port used `< 0x22`.
 *
 * Direction-relative distance:
 *   path 1 (`*+0xfc == 0`): keep peers with peer_span >= self_span;
 *                            dist = peer_span - self_span
 *   path 2 (`*+0xfc != 0`): keep peers with peer_span <= self_span;
 *                            dist = self_span - peer_span
 *
 * Return: `local_4` = best peer slot if (0 < best_dist <= 0x20), else iVar2
 * (=self_slot). The original returns either iVar2 directly or via the
 * load-from-stack at 0x00433737 / 0x004337c8; both paths share the trailing
 * "if (best_dist == 0 || best_dist > 0x20) preserve iVar2" semantic.
 */

/* Route-state direction-polarity dword [CONFIRMED via Ghidra symbol
 * `gActorRouteDirectionPolarity` @ 0x004afc5c; route-state base 0x004afb60;
 * delta 0xfc bytes = dword index 0x3F]. Same field as RS_ROUTE_DIRECTION_POLARITY
 * declared at the top of this file. FindNearestRoutePeer uses this to switch
 * ahead/behind peer-scan direction; UpdateTrafficRoutePlan uses the same dword
 * for the route-heading mirror branch. Both names alias the same field. */
#define RS_PEER_SCAN_REVERSE      RS_ROUTE_DIRECTION_POLARITY

int td5_ai_find_nearest_route_peer(int *route_state_ptr) {
    int self_slot = route_state_ptr[RS_SLOT_INDEX];
    int32_t scan_reverse = route_state_ptr[RS_PEER_SCAN_REVERSE];
    int32_t self_route_ptr;
    int16_t self_span;
    uint8_t self_lane;
    int32_t best_dist = 0x2ee00;   /* CONFIRMED @ 0x0043369b */
    int best_slot = self_slot;     /* CONFIRMED: local_4 = iVar2 @ 0x00433697 */
    int i;

    /* Bounds: required because route_state(slot) returns the storage array;
     * disassembly bypasses bounds because EAX is read from a struct field. */
    if (self_slot < 0 || self_slot >= TD5_MAX_TOTAL_ACTORS) {
        return self_slot;
    }

    {
        char *self = actor_ptr(self_slot);
        self_span = ACTOR_I16(self, ACTOR_SPAN_RAW);
        self_lane = ACTOR_U8(self, ACTOR_SUB_LANE_INDEX);
        self_route_ptr = route_state_ptr[RS_ROUTE_TABLE_PTR];
    }

    if (scan_reverse == 0) {
        /* ----------------------------------------------------------------
         * Path 1 [CONFIRMED @ 0x004336a9-0x00433722]: forward scan.
         * Keep peers ahead: dist = peer_span - self_span, < best_dist.
         * ---------------------------------------------------------------- */
        for (i = 0; i < TD5_MAX_TOTAL_ACTORS; i++) {
            char *peer_actor;
            int32_t *peer_rs;
            int16_t peer_span;
            uint8_t peer_lane;
            int32_t dist;

            /* CMP EBP,EAX / JZ skip @ 0x004336b5 */
            if (i == self_slot) continue;
            /* [PER-VIEWPORT TRAFFIC] ignore cars from other partitions (the twin
             * sits at the same span/lane and would read as a 0-distance peer ->
             * phantom brake + desync). No-op when per-viewport is off. */
            if (td5_ai_traffic_pair_blocked(self_slot, i)) continue;

            peer_actor = actor_ptr(i);
            peer_rs = route_state(i);

            /* MOV SI,[EDI] @ 0x004336b9 → peer.field_0x80 = peer_span.
             * MOV DX,[ECX + 0x4ab188] @ 0x004336cd → self.field_0x80 = self_span.
             * CMP DX,SI / JG skip @ 0x004336d4 → skip if self_span > peer_span. */
            peer_span = ACTOR_I16(peer_actor, ACTOR_SPAN_RAW);
            if (self_span > peer_span) continue;

            /* MOV CL,[ECX + 0x4ab194] @ 0x004336d9 → self.field_0x8c = self_lane.
             * CMP CL,[EDI + 0xc] @ 0x004336df → compare against peer.field_0x8c.
             * JNZ skip @ 0x004336e2 → lane must match. */
            peer_lane = ACTOR_U8(peer_actor, ACTOR_SUB_LANE_INDEX);
            if (peer_lane != self_lane) continue;

            /* MOV ECX,[ECX*4 + 0x4afb6c] @ 0x004336ec → self.RS[0] (route ptr).
             * CMP ECX,[EBX] @ 0x004336f3 → compare against peer.RS[0].
             * JNZ skip @ 0x004336f5 → route-table pointers must match. */
            if (self_route_ptr != peer_rs[RS_ROUTE_TABLE_PTR]) continue;

            /* MOVSX EDX,DX / MOVSX ECX,SI / SUB ECX,EDX @ 0x004336f7-0x004336fd
             * → dist = (int32)peer_span - (int32)self_span.
             * CMP ECX,best_dist / JGE skip @ 0x004336ff-0x00433703
             * → strict less-than: skip if dist >= best_dist. */
            dist = (int32_t)peer_span - (int32_t)self_span;
            if (dist >= best_dist) continue;

            best_dist = dist;
            best_slot = i;
        }

        /* TEST ECX,ECX / JZ @ 0x00433726-0x00433728: best_dist == 0 → return self
         * CMP ECX,0x20 / JG @ 0x0043372e-0x00433731: best_dist > 0x20 → return self
         * MOV EAX,best_slot / RET @ 0x00433737-0x00433740: otherwise return best_slot. */
        if (best_dist == 0)   return self_slot;
        if (best_dist > 0x20) return self_slot;
        return best_slot;
    } else {
        /* ----------------------------------------------------------------
         * Path 2 [CONFIRMED @ 0x00433741-0x004337c1]: reverse scan.
         * Keep peers behind: dist = self_span - peer_span, < best_dist.
         * ---------------------------------------------------------------- */
        for (i = 0; i < TD5_MAX_TOTAL_ACTORS; i++) {
            char *peer_actor;
            int32_t *peer_rs;
            int16_t peer_span;
            uint8_t peer_lane;
            int32_t dist;

            /* CMP EDI,EAX / JZ skip @ 0x0043374d */
            if (i == self_slot) continue;
            /* [PER-VIEWPORT TRAFFIC] skip other partitions' cars (gated; no-op off). */
            if (td5_ai_traffic_pair_blocked(self_slot, i)) continue;

            peer_actor = actor_ptr(i);
            peer_rs = route_state(i);

            /* MOV DL,[ESI + 0xc] @ 0x00433751 → peer.field_0x8c = peer_lane.
             * CMP DL,[ECX + 0x4ab194] @ 0x00433765 → compare against self.field_0x8c.
             * JNZ skip @ 0x0043376b → lane must match.
             * (Note path 2 tests lane BEFORE span, opposite of path 1; semantics same.) */
            peer_lane = ACTOR_U8(peer_actor, ACTOR_SUB_LANE_INDEX);
            if (peer_lane != self_lane) continue;

            /* MOV DX,[ESI] @ 0x0043376d → peer.field_0x80 = peer_span.
             * MOV CX,[ECX + 0x4ab188] @ 0x00433770 → self.field_0x80 = self_span.
             * CMP DX,CX / JG skip @ 0x00433777-0x0043377a → skip if peer_span > self_span. */
            peer_span = ACTOR_I16(peer_actor, ACTOR_SPAN_RAW);
            if (peer_span > self_span) continue;

            /* MOV EBX,[EBX*4 + 0x4afb6c] @ 0x00433784 → self.RS[0].
             * CMP EBX,[EBP] @ 0x0043378b → compare against peer.RS[0].
             * JNZ skip @ 0x0043378e. */
            if (self_route_ptr != peer_rs[RS_ROUTE_TABLE_PTR]) continue;

            /* MOVSX EDX,DX / MOVSX ECX,CX / SUB ECX,EDX @ 0x00433790-0x00433796
             * → dist = (int32)self_span - (int32)peer_span. */
            dist = (int32_t)self_span - (int32_t)peer_span;
            if (dist >= best_dist) continue;

            best_dist = dist;
            best_slot = i;
        }

        /* TEST ECX,ECX / JZ @ 0x004337bf-0x004337c1: best_dist == 0 → preserve self.
         * CMP ECX,0x20 / JG @ 0x004337c3-0x004337c6: best_dist > 0x20 → preserve self.
         * MOV EAX,best_slot @ 0x004337c8: otherwise return best_slot. */
        if (best_dist == 0)   return self_slot;
        if (best_dist > 0x20) return self_slot;
        return best_slot;
    }
}

/**
 * RecycleTrafficActorFromQueue — byte-faithful port of 0x004353B0.
 *
 * [CONFIRMED @ 0x004353B0] L5 promotion sweep audit (2026-05-18).
 *
 * Verbatim 10-step translation per disassembly listing (1352 bytes /
 * ~340 instructions). Each step in the comment block below is anchored
 * to its orig listing range:
 *   1.  Bail g_racerCount <= 6           [0x004353B0-C1].
 *   2.  Cursor pre-scan over queue       [0x004353C9-FD].
 *   3.  Linear scan slots 6..min(N,12)   [0x004353FE-465].
 *   4.  best_dist <= 0x28 early-return.
 *   5.  *cursor == -1 early-return.
 *   6.  Slot-9 + special-encounter gate.
 *   7.  RS direction polarity + table_ptr_index write.
 *   8.  LEFT/RIGHT branch dispatch (queue_byte vs strip_byte):
 *       - LEFT: InitActorTrackSegmentPlacement + geometry + angle +
 *         polarity + ResetVehicleActorState + RefreshActorTrack
 *         ProgressOffset + NormalizeActorTrackWrapState.
 *       - RIGHT: inline jump-table scan + remap + Init + geometry +
 *         RefreshActorTrackProgressOffset + ResolveActorSegmentBoundary.
 *   9.  Advance DAT_004b08b8 +4.
 *  10.  LAB_0043588d post-call zero block.
 *
 * ARCHITECTURAL DIVERGENCES (documented in code below):
 *   - RS_DIRECTION_POLARITY: the port writes ONLY 0x3F (dword index 0x3F =
 *     gActorRouteDirectionPolarity), matching the original. The earlier
 *     defensive dual-write to 0x25 was removed (0x25 has no readers in orig).
 *   - Recycle heading dispatch collapsed into td5_track_compute_heading
 *     (see memory/reference_arch_recycle_heading_collapse.md).
 *
 * KNOWN DIVERGENCES: none beyond the two documented architectural ones.
 *
 * Effective level: L5 (byte-faithful per static audit; arch-divergences
 * are equivalence-preserving by design).
 *
 * Verbatim translation of TD5_d3d.exe @ 0x004353B0 disassembly (pool3
 * 2026-05-14). Replaces the prior semantically-named port (which used
 * wrong route-state offsets, wrote +0x82/+0x84/+0x86 directly instead
 * of letting InitActorTrackSegmentPlacement / RSB / NormalizeWrap do
 * it, and called helpers in the wrong direction).
 *
 * Listing flow:
 *   1. Bail if g_racerCount <= 6 (no traffic).
 *   2. Pre-scan g_traffic_queue cursor forward over entries that are
 *      either == -1 sentinel, behind player.span_norm (+0x82), or within
 *      0x28 spans ahead. Advance DAT_004b08b8 over rejected entries.
 *   3. Linear scan slots 6..min(g_racerCount,12)-1: find the slot with
 *      max (player.span_norm - traffic[i].span_norm) — store as
 *      (best_slot, best_dist). NOTE: gate (>=0x29) is applied AFTER the
 *      scan, not inside it. Slot 9 is NOT excluded from the scan.
 *   4. If best_dist <= 0x28: write cursor and return.
 *   5. If *cursor == -1: write cursor and return (queue exhausted at the
 *      currently-pointed entry — no recyclable spawn).
 *   6. If best_slot == 9 AND gSpecialEncounterTrackedActorHandle != -1:
 *      write cursor and return. (This is the slot-9 protection — but
 *      only fires when slot 9 was actually selected as best.)
 *   7. Set route_state[best_slot] direction polarity (dword +0xFC),
 *      route_table_ptr_index (dword +0x68) = best_slot*4+0x10.
 *   8. Compare queue.byte_3 (sub_lane) vs g_trackStripRecords[queue_span * 0x18 + 3] & 0xf:
 *        - queue_byte > strip_byte: LEFT branch (selector=0)
 *            * actor+0x80 = queue_span; actor+0x8c = queue_byte (raw)
 *            * Call InitActorTrackSegmentPlacement(actor+0x80, actor+0x1FC)
 *            * Geometry-derive heading from strip vertex deltas (case 1-7)
 *            * Call AngleFromVector12Full; store (angle+0x800)*0x100 to +0x1f4
 *            * If polarity flag, add 0x80000 to yaw
 *            * Call ResetVehicleActorState(actor)
 *            * Call RefreshActorTrackProgressOffset(best_slot)
 *            * Call NormalizeActorTrackWrapState(actor)
 *        - queue_byte <= strip_byte: RIGHT branch (selector=1)
 *            * Inline jump-table scan: queue_span in [third, third+(high-low)-1]
 *              → remapped = queue_span + (low - third); else -1 sentinel.
 *            * actor+0x80 = remapped; actor+0x8c = queue_byte - (strip_byte & 0xf)
 *            * Call InitActorTrackSegmentPlacement; geometry; angle; polarity; reset.
 *            * Call RefreshActorTrackProgressOffset(best_slot)
 *            * Call ResolveActorSegmentBoundary(actor) (0x00443FF0)
 *   9. Advance DAT_004b08b8 by 4 bytes (past consumed entry).
 *  10. Zero post-call state fields (LAB_0043588d) including velocities,
 *      steering, encounter_handle = -1, etc.
 *
 * NOTE on RS_DIRECTION_POLARITY: the disassembly writes to
 * gActorRouteDirectionPolarity (0x004afc5c), which is at route_state
 * base+0xFC = dword index 0x3F. The port writes ONLY this field, via
 * rs[RS_ROUTE_DIRECTION_POLARITY] (see the live write below), matching the
 * original. An earlier port also wrote dword 0x25, but that field has no
 * readers in the original listing, so the defensive dual-write was removed —
 * no discrepancy remains.
 *
 * NOTE on ResolveActorSegmentBoundary: 0x00443FF0 is ported in unmerged
 * branch precise-00443FF0 (SHA b99e36a) as td5_track_resolve_actor_segment_boundary.
 * Declared __attribute__((weak)) so the port links cleanly in master
 * (with NormalizeActorTrackWrapState as fallback) and uses the real RSB
 * once b99e36a merges.
 */

/* Forward decl, weakly linked: real symbol comes from b99e36a's
 * td5_track.c port of ResolveActorSegmentBoundary @ 0x00443FF0.
 * If unresolved at link time, the address is NULL and the fallback runs. */
extern void td5_track_resolve_actor_segment_boundary(TD5_Actor *actor)
    __attribute__((weak));

/* ARCHITECTURAL DIVERGENCE — see
 *   memory/reference_arch_recycle_heading_collapse.md
 *
 * The original 0x004353B0 inlines a span_type → vertex-offset case dispatch
 * (cases 1/2/5, 3/4, 6/7) twice — once per LEFT/RIGHT branch — feeding
 * AngleFromVector12Full to derive the spawn heading. In the port we instead
 * route through td5_track_compute_heading, which implements the same
 * geometry-from-strip atan2 contract for the actor's current span and
 * writes the result into actor+0x1F4. The case dispatch is therefore
 * absorbed into that helper; do not re-implement it here.
 *
 * Static byte-equivalence audit (cases 1/2/5, 3/4, 6/7, default):
 *  - span source: actor+0x80 (SPAN_RAW) is written with q_span (LEFT) or
 *    remapped (RIGHT) before each helper call, matching what the original's
 *    inline dispatch keys off.
 *  - vertex offsets, signed div-by-4 (>>2 with sign bias), and the
 *    (angle+0x800)<<8 writeback to +0x1F4 all match td5_track.c:3139.
 *  - helper additionally writes actor+0x290 (heading_normal) which the
 *    inline original may not — benign because nothing in the post-call
 *    zero block (LAB_0043588d) reads +0x290.
 *
 * If you ever need to byte-diff this against the original, prototype was
 * `recycle_compute_heading_angle_12` (mentioned in ca6e5bb commit body)
 * but was removed pending byte-diff validation against the existing helper.
 */

/* [PORT: N-way split] Span extremes across the LOCAL human players (slots
 * 0..num_human_players-1, clamped to the racer range). span = span_normalized
 * (+0x82). For a single player both return slot 0's span, so legacy traffic
 * recycling stays byte-identical to the original. Used so traffic is recycled
 * only once the TRAILING player has passed it and respawns ahead of the LEAD
 * player. */
/* [PER-VIEWPORT TRAFFIC 2026-06-22] When >= 0, the dynamic-traffic spawner /
 * manager is SCOPED to a single viewport's player: the anchor + proximity helpers
 * below consider ONLY this racer slot, so each split-screen viewport's traffic set
 * is placed relative to its own player. -1 (the default) restores the original
 * all-players behaviour, so every non-scoped caller is byte-identical. */
static int s_trf_scope_slot = -1;

static int ai_player_span_lead(void)
{
    int humans = g_td5.num_human_players;
    if (s_trf_scope_slot >= 0)
        return (int)(int16_t)ACTOR_I16(actor_ptr(s_trf_scope_slot), ACTOR_SPAN_NORMALIZED);
    if (humans < 1) humans = 1;
    if (humans > g_traffic_slot_base) humans = g_traffic_slot_base;
    int ext = (int)(int16_t)ACTOR_I16(actor_ptr(0), ACTOR_SPAN_NORMALIZED);
    for (int s = 1; s < humans; s++) {
        int sp = (int)(int16_t)ACTOR_I16(actor_ptr(s), ACTOR_SPAN_NORMALIZED);
        if (sp > ext) ext = sp;
    }
    return ext;
}
static int ai_player_span_trailing(void)
{
    int humans = g_td5.num_human_players;
    if (s_trf_scope_slot >= 0)
        return (int)(int16_t)ACTOR_I16(actor_ptr(s_trf_scope_slot), ACTOR_SPAN_NORMALIZED);
    if (humans < 1) humans = 1;
    if (humans > g_traffic_slot_base) humans = g_traffic_slot_base;
    int ext = (int)(int16_t)ACTOR_I16(actor_ptr(0), ACTOR_SPAN_NORMALIZED);
    for (int s = 1; s < humans; s++) {
        int sp = (int)(int16_t)ACTOR_I16(actor_ptr(s), ACTOR_SPAN_NORMALIZED);
        if (sp < ext) ext = sp;
    }
    return ext;
}

/* [traffic one-way] Forward decl — defined next to the other trf_* knobs below.
 * When on, every traffic lane is forced to head down-track (polarity 0) so all
 * traffic flows the SAME direction (user: "the orientation should make the
 * traffic all head the same way"). Used by the legacy queue spawn paths here
 * AND the dynamic spawner's lane-direction / cross-traffic helpers. */
static int trf_oneway_traffic(void);

void td5_ai_recycle_traffic_actor(void) {
    int       racer_count;
    int       best_slot = 0;
    int32_t   best_dist = 0;
    int       i;
    int       loop_max;
    char     *p0;
    int16_t   player_span_norm;
    const uint8_t *qp;
    int16_t   q_span;
    uint8_t   q_flags;
    uint8_t   q_byte_3;
    char     *a;
    int32_t  *rs;
    uint8_t  *rs_bytes;

    /* [0x004353b0-c1] EAX = g_racerCount; bail if <= 6. The original reads
     * `dword ptr [0x004aaf00]` which is g_racerCount (also exposed in port
     * as g_active_actor_count for the runtime active-slot count).
     * [PORT: N-way] "6" is really the racer/traffic boundary — only recycle
     * once traffic actors exist beyond the racer slots. */
    racer_count = g_active_actor_count;
    if (racer_count <= g_traffic_slot_base) return;

    if (!g_actor_base || !g_route_state_base) return;
    if (!g_traffic_queue_ptr || !g_traffic_queue_base) return;

    /* [0x004353c9-fd] Pre-scan: advance queue cursor over entries that are
     * not-yet-overtaken by the player. The match condition for STOPPING the
     * scan is:
     *   q_span == -1  (sentinel; loop exits)
     *   OR q_span < player.span_norm   (entry is behind player → stop)
     *   OR q_span - player.span_norm >= 0x28   (≥40 spans ahead → stop)
     *
     * Otherwise (q_span >= player AND ahead < 0x28): advance.
     *
     * NOTE: player slot's span_norm at byte offset 0x82 — read as 16-bit
     * sign-extended value (MOV BP, word ptr [0x004ab18a]). Equivalent to
     * actor[0]+0x82, i.e. the span_norm field. The pre-2026-05-14 port
     * used ACTOR_SPAN_RAW (+0x80) which is the wrong field for tracks
     * with junction remaps. */
    p0 = actor_ptr(0);
    (void)p0;
    /* [PORT: N-way] Pre-scan/respawn reference = the LEAD local player, so a
     * recycled traffic car respawns ahead of the front-runner. (1 player =>
     * slot 0 span, byte-identical to the original.) */
    player_span_norm = (int16_t)ai_player_span_lead();

    qp = g_traffic_queue_ptr;
    q_span = (int16_t)(qp[0] | (qp[1] << 8));
    if (q_span != -1) {
        for (;;) {
            int sx = (int)q_span;
            int px = (int)player_span_norm;
            if (sx < px) break;           /* behind player → stop */
            if ((sx - px) >= 0x28) break; /* ≥40 ahead → stop */
            /* fall through: too close — advance */
            qp += 4;
            q_span = (int16_t)(qp[0] | (qp[1] << 8));
            if (q_span == -1) break;
        }
    }

    /* [0x00435401-46] Scan slots 6..min(g_racerCount,12)-1; find slot with
     * MAX (player.span_norm - traffic[i].span_norm). The gate (>=0x29) is
     * applied AFTER the scan, NOT inside it. The disassembly explicitly
     * does NOT exclude slot 9 here — slot 9 is only protected later, IF
     * it was actually selected as the winner.
     *
     * The original's EBX walks the actor table at slot[6]+0x82 (raw byte
     * address 0x004ac6ba); the inner read is signed 16-bit (MOVSX). */
    loop_max = racer_count;
    if (loop_max > g_traffic_slot_base + TD5_MAX_TRAFFIC_SLOTS)
        loop_max = g_traffic_slot_base + TD5_MAX_TRAFFIC_SLOTS;
    {
        int psn_loop;
        /* [PORT: N-way] scan the TRAFFIC region [g_traffic_slot_base, loop_max),
         * not the legacy 6..12, so big fields recycle their own traffic slots
         * (16..21) instead of mistakenly teleporting racer slots 6..15. */
        for (i = g_traffic_slot_base; i < loop_max; i++) {
            int16_t ts;
            int32_t dist;
            /* [PORT: N-way] Recycle reference = the TRAILING local player, so a
             * traffic car is only retired once EVERY player has passed it by
             * >40 spans (1 player => slot 0 span, byte-identical to original).
             * Orig reloaded slot 0's span_norm here each iteration (0x00435438). */
            psn_loop = ai_player_span_trailing();
            ts = ACTOR_I16(actor_ptr(i), ACTOR_SPAN_NORMALIZED);
            dist = psn_loop - (int)ts;
            if (dist > best_dist) {
                best_slot = i;
                best_dist = dist;
            }
        }
    }

    /* [0x00435448-4b] If best_dist <= 0x28: commit cursor and return.
     * Original writes DAT_004b08b8 unconditionally just before bail. */
    if (best_dist <= 0x28) {
        g_traffic_queue_ptr = qp;
        return;
    }

    /* [0x00435451-55] If queue cursor entry is -1 sentinel: commit cursor
     * and return. The pre-scan may have left q_span==-1 OR may have left a
     * valid entry; this check fires only when entry IS sentinel. */
    if ((int16_t)(qp[0] | (qp[1] << 8)) == -1) {
        g_traffic_queue_ptr = qp;
        return;
    }

    /* [0x0043545b-67] Slot 9 protection: only blocks if slot 9 was chosen
     * AND a special-encounter handle is active. */
    if (best_slot == 9 && g_encounter_tracked_handle != -1) {
        g_traffic_queue_ptr = qp;
        return;
    }

    /* [0x0043546d-9c] uVar11 = best_slot * 0x11c. Write polarity and a
     * derived field at uVar11 + 0x4afbc8. The +0x68 dword (route_state
     * index 0x1A) holds best_slot*4+0x10 — purpose unclear; copied verbatim.
     * The polarity field is at route_state base+0xFC (dword index 0x3F)
     * per `gActorRouteDirectionPolarity = 0x004afc5c`. */
    rs = route_state(best_slot);
    rs_bytes = (uint8_t *)rs;
    /* Refresh q_span and qp[2..3] from cursor (may have been advanced by
     * pre-scan, then validated above). */
    q_span   = (int16_t)(qp[0] | (qp[1] << 8));
    q_flags  = qp[2];
    q_byte_3 = qp[3];

    /* dword at rs+0xFC = polarity (q_flags & 1) [0x0043548a]. The original
     * writes ONLY this dword (= gActorRouteDirectionPolarity, dword index 0x3F).
     * Prior port also wrote dword 0x25 (RS_DIRECTION_POLARITY_LEGACY) but that
     * field has no references in the original listing — the defensive
     * dual-write was unnecessary. */
    /* [traffic one-way] Force polarity 0 so this car follows the route in the
     * down-track direction; otherwise honour the authored TRAFFIC.BUS bit. */
    rs[RS_ROUTE_DIRECTION_POLARITY] =
        trf_oneway_traffic() ? 0 : (int32_t)(q_flags & 1u);
    (void)rs_bytes;

    /* dword at rs+0x68 = best_slot*4 + 0x10 [0x00435497]. Purpose unclear
     * (the original uses it as some kind of slot-derived index/offset).
     * Port verbatim. */
    *(int32_t *)(rs_bytes + 0x68) = best_slot * 4 + 0x10;

    /* [0x0043549d-af] Compare queue.byte_3 (raw sub_lane) vs strip_records
     * [q_span * 0x18 + 3] & 0xf (strip's lane_count nibble). */
    a = actor_ptr(best_slot);
    {
        const uint8_t *strip = (const uint8_t *)g_strip_span_base;
        uint8_t strip_lane_count;
        int     left_branch; /* 1 = LEFT (queue_byte > strip_lane), 0 = RIGHT */

        if (!strip) {
            /* Without strip records we cannot proceed; commit cursor and bail. */
            g_traffic_queue_ptr = qp + 4;
            return;
        }
        strip_lane_count = strip[(size_t)q_span * 0x18 + 3] & 0x0Fu;
        left_branch = ((unsigned)strip_lane_count < (unsigned)q_byte_3) ? 1 : 0;

        if (left_branch) {
            /* ====== LEFT branch (0x004354b5-0x004356cd, selector=0) ====== */
            int32_t yaw_pack;

            /* [0x004354bb] selector = 0 */
            rs[RS_ROUTE_TABLE_SELECTOR] = 0;

            /* [0x00435541-55] actor+0x80 = queue_span; actor+0x8c = queue_byte (raw) */
            ACTOR_I16(a, ACTOR_SPAN_RAW)       = q_span;
            ACTOR_U8(a, ACTOR_SUB_LANE_INDEX)  = q_byte_3;

            /* [0x0043555b-67] InitActorTrackSegmentPlacement(actor+0x80, actor+0x1FC).
             * Byte-faithful port @ 0x00445F10 — writes world_pos[0..2] in 24.8 FP
             * AND seeds actor +0x84 (span_accum) + +0x86 (span_high) from span_raw,
             * AND clamps +0x8C (sub_lane) to (lane_count - 1) if out of range. */
            td5_track_init_actor_segment_placement(
                (int16_t *)(a + ACTOR_SPAN_RAW),
                (int32_t *)(a + ACTOR_WORLD_POS_X));

            /* [0x00435568-0x00435690] geometry-derive heading angle from strip,
             * then AngleFromVector12Full, then (angle+0x800)<<8 → actor+0x1f4.
             * For tighter fidelity, recycle_compute_heading_angle_12 walks the
             * span_type case dispatch; td5_track_compute_heading is the
             * port's existing equivalent path. We prefer the existing helper
             * here because it shares the same atan2 sign/wrap convention with
             * the rest of the port; the angle field +0x1f4 is then written
             * with the +0x800 bias as the original does. */
            td5_track_compute_heading((TD5_Actor *)a);

            /* [0x004356a6-bb] If polarity flag set, add 0x80000 to yaw (180°
             * flip = oncoming heading). Suppressed in one-way mode so the car
             * faces down-track like all other traffic. */
            yaw_pack = ACTOR_I32(a, ACTOR_YAW_ACCUM);
            if (!trf_oneway_traffic() && (q_flags & 1u) != 0u) {
                ACTOR_I32(a, ACTOR_YAW_ACCUM) = yaw_pack + 0x80000;
            }

            /* [0x004356bb-c2] ResetVehicleActorState(actor) at 0x00405d70 —
             * port equivalent zeroes velocities & integrates pose. */
            td5_physics_reset_actor_state((TD5_Actor *)a);

            /* [0x004356c7-c8] RefreshActorTrackProgressOffset(best_slot) at 0x004342e0 */
            td5_ai_seed_actor_track_progress_offset(best_slot);

            /* [0x004356cd-ce] NormalizeActorTrackWrapState(actor) at 0x00443fb0 */
            td5_track_normalize_actor_wrap((TD5_Actor *)a);

            /* [0x004356d3-de] Commit DAT_004b08b8 += 4 (consume entry) */
            g_traffic_queue_ptr = qp + 4;

        } else {
            /* ====== RIGHT branch (0x004356ea-0x004358a6, selector=1) ====== */
            int32_t yaw_pack;
            int     remapped;

            /* [0x004356ea-f3] selector = 1 (RIGHT route / alt-segment) */
            rs[RS_ROUTE_TABLE_SELECTOR] = 1;

            /* [0x004354c5-0x0043550f + 0x004355e2-00] Inline jump-table scan:
             *   Find entry i where queue_span ∈ [entry.third, entry.third + (entry.high - entry.low) - 1].
             *   Result = queue_span + (entry.low - entry.third) (queue→main decode).
             *   No match: remapped = -1 (sentinel).
             *
             * The port's td5_track_apply_target_span_remap(lin_span,
             * is_canonical=0) implements the IDENTICAL math: entry[+0] read
             * as "remap_dst" (=low), entry[+2] as "remap_end_exc" (=high),
             * entry[+4] as "range_lo" (=third). Match condition:
             *   range_lo <= lin <= range_lo + (remap_end_exc - remap_dst) - 1
             * Result: lin + (remap_dst - range_lo)
             * — which is exactly queue_span + (low - third).
             *
             * Behavior contract: returns lin_span unchanged when no entry
             * matches; the original writes -1 in that case. We restore the
             * -1 sentinel by detecting "unchanged" outcome here. */
            remapped = td5_track_apply_target_span_remap((int)q_span, 0);
            if (remapped == (int)q_span) {
                /* No remap happened — either no jump entries or no match.
                 * Original stores -1 as the sentinel (OR EBP, 0xffffffff at
                 * 0x0043550f). Treat as -1. */
                remapped = -1;
            }

            /* [0x00435541-55] actor+0x80 = remapped; actor+0x8c = q_byte_3 - (strip_lane & 0xf). */
            ACTOR_I16(a, ACTOR_SPAN_RAW) = (int16_t)remapped;
            ACTOR_I8(a, ACTOR_SUB_LANE_INDEX) =
                (int8_t)((int)(int8_t)q_byte_3 - (int)(strip_lane_count & 0xFu));

            /* [0x0043555b-67] InitActorTrackSegmentPlacement byte-faithful port.
             * Seeds actor +0x84/+0x86 (span_accum/high), clamps +0x8C if sub_lane
             * exceeds lane_nibble, writes world_pos[0..2] in 24.8 FP. */
            if (remapped >= 0) {
                td5_track_init_actor_segment_placement(
                    (int16_t *)(a + ACTOR_SPAN_RAW),
                    (int32_t *)(a + ACTOR_WORLD_POS_X));
            }

            /* [0x00435568-0x00435690 mirror] heading + yaw + polarity.
             * One-way mode suppresses the 180° oncoming flip. */
            td5_track_compute_heading((TD5_Actor *)a);
            yaw_pack = ACTOR_I32(a, ACTOR_YAW_ACCUM);
            if (!trf_oneway_traffic() && (q_flags & 1u) != 0u) {
                ACTOR_I32(a, ACTOR_YAW_ACCUM) = yaw_pack + 0x80000;
            }

            /* [0x00435865-72] ResetVehicleActorState; RefreshActorTrackProgressOffset. */
            td5_physics_reset_actor_state((TD5_Actor *)a);
            td5_ai_seed_actor_track_progress_offset(best_slot);

            /* [0x00435877-78] ResolveActorSegmentBoundary(actor) at 0x00443ff0.
             * Real symbol from b99e36a (precise-00443FF0). Weakly-linked
             * forward decl; fall back to NormalizeWrap if unavailable. */
            if (&td5_track_resolve_actor_segment_boundary != NULL) {
                td5_track_resolve_actor_segment_boundary((TD5_Actor *)a);
            } else {
                /* FIXME[precise-00443FF0]: NormalizeWrap is the closest in-tree
                 * helper but does NOT handle the jump-table-decode case for
                 * out-of-ring raw spans (>ring_length). Once b99e36a is
                 * merged, the weak-linked RSB above takes over and the
                 * fallback dies. */
                td5_track_normalize_actor_wrap((TD5_Actor *)a);
            }

            /* [0x00435882-88] Commit DAT_004b08b8 += 4. */
            g_traffic_queue_ptr = qp + 4;
        }
    }

    /* ====== LAB_0043588d: shared post-call state zero (both branches) ======
     * Original writes (all dwords unless marked):
     *   slot+0x314 = 0   (LONGITUDINAL_SPEED)
     *   slot+0x30c = 0   (STEERING_CMD)
     *   slot+0x379 = 0   (byte: VEHICLE_MODE)
     *   slot+0x1f0 = 0
     *   slot+0x1f8 = 0
     *   slot+0x1c0 = 0
     *   slot+0x1c4 = 0
     *   slot+0x1c8 = 0
     *   slot+0x1cc = 0   (LIN_VEL_X)
     *   route_state[+0x88] = 0   (RS_RECOVERY_STAGE at dword 0x22)
     *   slot+0x1d0 = 0
     *   route_state[+0xF0] = 0   (dword index 0x3C; port macro RS_SCRIPT_SPEED_PARAM
     *                              — but here it's used as a generic clear)
     *   slot+0x1d4 = 0   (LIN_VEL_Z)
     *   route_state[+0x7C] = -1  (RS_ENCOUNTER_HANDLE at dword 0x1F)
     *   word slot+0x338 = 0      (16-bit) — UNNAMED slot field
     */
    {
        char *aa = actor_ptr(best_slot);
        if (aa) {
            *(int32_t *)(aa + 0x314) = 0;
            *(int32_t *)(aa + 0x30C) = 0;
            *(int8_t  *)(aa + 0x379) = 0;
            *(int32_t *)(aa + 0x1F0) = 0;
            *(int32_t *)(aa + 0x1F8) = 0;
            *(int32_t *)(aa + 0x1C0) = 0;
            *(int32_t *)(aa + 0x1C4) = 0;
            *(int32_t *)(aa + 0x1C8) = 0;
            *(int32_t *)(aa + 0x1CC) = 0;
            *(int32_t *)(aa + 0x1D0) = 0;
            *(int32_t *)(aa + 0x1D4) = 0;
            *(int16_t *)(aa + 0x338) = 0;
        }
        /* route_state byte-direct writes match the original's
         * `[uVar11 + literal]` style. */
        *(int32_t *)((uint8_t *)rs + 0x88) = 0;
        *(int32_t *)((uint8_t *)rs + 0xF0) = 0;
        *(int32_t *)((uint8_t *)rs + 0x7C) = -1;
        /* Mirror into port's semantic macros for consistency. */
        rs[RS_RECOVERY_STAGE]   = 0;
        rs[RS_ENCOUNTER_HANDLE] = -1;
        /* Track recovery stage shadow used by other port code: clear too. */
        if (best_slot >= 0 && best_slot < TD5_MAX_TOTAL_ACTORS) {
            g_traffic_recovery_stage[best_slot] = 0;
        }
    }

    /* [#5 2026-06-20] A recycled slot is a fresh car: clear any (permanent) wreck
     * state so the reused slot drives again and the traffic pool is never starved
     * by totalled cars left behind the player. */
    if (best_slot >= 0 && best_slot < TD5_MAX_TOTAL_ACTORS) {
        g_actor_broken_down[best_slot]  = 0;
        g_actor_broken_ticks[best_slot] = 0;
    }

    TD5_LOG_I(LOG_TAG,
              "recycle_traffic: slot=%d player_span=%d q_span=%d q_lane=%u q_flags=0x%02X dist=%d",
              best_slot, (int)player_span_norm, (int)q_span, q_byte_3, q_flags, best_dist);
}

/**
 * InitializeTrafficActorsFromQueue: fill slots 6-11 from the head
 * of the TRAFFIC.BUS queue at race start.
 */
void td5_ai_set_traffic_queue(const uint8_t *data, int size) {
    /* Store the raw pointer — caller owns the backing buffer for the race
     * lifetime. `size` is informational; the queue is actually terminated
     * by a span == -1 sentinel record. */
    g_traffic_queue_base = data;
    g_traffic_queue_ptr  = data;
    TD5_LOG_I(LOG_TAG, "traffic queue bound: data=%p size=%d records=%d",
              (const void *)data, size, size / 4);
}

/* Byte-faithful port of InitializeTrafficActorsFromQueue @ 0x00435940.
 *
 * Original signature: void __stdcall InitializeTrafficActorsFromQueue(void)
 * Spawns ambient traffic actors into slots [6, min(g_racerCount, 12)).
 * Source: Ghidra disassembly listing 0x00435940-0x00435CB7 (271 instructions).
 *
 * [CONFIRMED @ 0x00435940] L5 audit 2026-05-18 (TD5_pool0 read-only) — byte-
 * faithful with original for all in-loop logic:
 *   - Outer gate `if (6 < g_racerCount)` matches 0x0043595D.
 *   - racer_cap = min(racer_count, 12) matches 0x0043597E.
 *   - Per-slot initial local_18=6, local_c=0x28 match 0x0043595B-70.
 *   - Branch condition (lane_count > queue.sub_lane) matches JA at 0x004359C1.
 *   - NORMAL path span_type switch (cases 1/2/5, 3/4, 6/7) byte-faithful.
 *   - REMAP path inlined ComputeActorTrackHeading switch byte-faithful.
 *   - Yaw computation `(angle + 0x800) << 8` matches 0x435C44 / 0x00435A77.
 *   - Polarity flip +0x80000 matches 0x435C58 / 0x00435A88.
 *   - Per-slot advance: qp+=4, slot++, local_c+=4 matches 0x00435C8B-CA5.
 *
 * [ARCH-DIVERGENCE] Three intentional port-side divergences kept post-audit.
 * See `reference_arch_init_traffic_actors_2026-05-18.md` for full rationale.
 *   D1. REMAP-miss returns input span vs orig -1 sentinel; port re-checks
 *       equality and substitutes -1 to match orig externally.
 *   D2. Trailing common ops (ACTOR_SLOT_INDEX mirror + RS_SLOT_INDEX +
 *       RS_ENCOUNTER_HANDLE = -1) are port-only bookkeeping; orig achieves
 *       the same via EBX address-mode addressing + ResetVehicleActorState
 *       side-effects.
 *   D3. RS_DIRECTION_POLARITY (legacy alias 0x25) dual-write removed
 *       2026-05-14 after confirming no original readers. Orig writes
 *       dword 0x3F = gActorRouteDirectionPolarity only.
 *
 * Per-slot algorithm:
 *   1. Read 4-byte queue record (span, polarity_byte, sub_lane).
 *   2. Compute strip-record lane_count = strip[queue.span].byte3 & 0xF.
 *   3. If queue.sub_lane < lane_count: NORMAL path (route selector = 0).
 *      Else: REMAP path (route selector = 1) — search junction-remap table
 *      and adjust span/sub_lane.
 *   4. Common: place actor, build yaw, ResetVehicleActorState,
 *      RefreshActorTrackProgressOffset (NORMAL) or ComputeTrackSpanProgress
 *      + ComputeSignedTrackOffset (REMAP), NormalizeActorTrackWrapState.
 *   5. Advance queue pointer 4 bytes.
 *
 * Divergences from existing port v0 (replaced 2026-05-14):
 *   - Adds ResetVehicleActorState + NormalizeActorTrackWrapState calls per
 *     original behavior (v0 skipped these).
 *   - Inline switch geometry matches ComputeActorTrackHeading @ 0x00435CE0
 *     (REMAP path) and an inline AngleFromVector12Full (NORMAL path).
 *   - REMAP path now seeds RS_TRACK_PROGRESS + RS_TRACK_OFFSET_BIAS via
 *     ComputeTrackSpanProgress + ComputeSignedTrackOffset (per original
 *     LAB_00435a96-bd).
 *   - REMAP path sets actor.span_normalized to ORIGINAL queue.span
 *     (post-call, per 0x00435ad1), not the remapped span.
 *   - REMAP path subtracts lane_count from sub_lane (per 0x00435a59 SUB).
 *
 * [ARCH-DIVERGENCE D3] direction-polarity-macro: the original writes polarity
 * at byte 0xFC of route_state slot (= dword index 0x3F =
 * gActorRouteDirectionPolarity). The port previously had
 * RS_DIRECTION_POLARITY = 0x25 (byte 0x94) — an established port-wide macro
 * mismatch. The dual-write defence was removed 2026-05-14 after confirming
 * no original readers of the 0x25 alias. See
 * reference_arch_init_traffic_actors_2026-05-18.md for catalogue.
 */
void td5_ai_init_traffic_actors(void) {
    int local_18;       /* slot counter (original local_18, starts at 6) */
    int local_c;        /* per-slot small constant (slot*4 + 0x10), starts at 0x28 */
    const uint8_t *qp;  /* DAT_004b08b8 queue cursor */
    int racer_count;
    int racer_cap;

    /* 0x00435940-50: gate on g_racerCount > 6 */
    racer_count = g_active_actor_count;
    if (racer_count <= g_traffic_slot_base)
        return;

    /* [PORT ENHANCEMENT dynamic-traffic] GTA-style spawner replaces the queue
     * fill entirely (and works on tracks WITHOUT a TRAFFIC.BUS, e.g. TD6
     * conversions — hence this gate sits before the queue NULL check).
     * [Traffic] Dynamic=0 falls through to the byte-faithful queue path. */
    if (g_td5.ini.traffic_dynamic && g_td5.traffic_enabled) {
        td5_ai_traffic_dynamic_race_init();
        return;
    }

    qp = g_traffic_queue_ptr ? g_traffic_queue_ptr : g_traffic_queue_base;
    if (!qp) {
        TD5_LOG_W(LOG_TAG, "init_traffic_actors: g_traffic_queue_ptr is NULL");
        return;
    }

    /* [DIAG fix-1780404735 upstream-remap] track how often this race-start fill
     * runs and how far the queue cursor has advanced. The original calls
     * InitializeTrafficActorsFromQueue ONCE per race; if the port calls it
     * repeatedly the cursor walks deep into the queue into branch-entries that
     * miss the junction remap (-> span=-1 -> origin placement -> stuck). */
    {
        static int s_init_traffic_calls = 0;
        s_init_traffic_calls++;
        TD5_LOG_I(LOG_TAG,
                  "init_traffic_ENTER: call#=%d cursor_off=%ld span_count=%d",
                  s_init_traffic_calls,
                  (long)(g_traffic_queue_base ? (qp - g_traffic_queue_base) : -1),
                  td5_track_get_span_count());
    }

    /* 0x00435975-7e: cap iteration at min(racer_count, traffic_base+6 traffic).
     * [PORT] traffic base is g_traffic_slot_base (6 legacy / 16 big fields). */
    racer_cap = (racer_count > g_traffic_slot_base + TD5_MAX_TRAFFIC_SLOTS)
                ? (g_traffic_slot_base + TD5_MAX_TRAFFIC_SLOTS) : racer_count;

    /* Initial loop state (0x4359 5b/68/63/70) — traffic actors start right
     * after the racer slots (g_traffic_slot_base: 6 legacy, 16 big fields). */
    local_18 = g_traffic_slot_base;             /* 6 legacy */
    local_c  = g_traffic_slot_base * 4 + 0x10;  /* 0x28 legacy */

    while (local_18 < racer_cap) {
        char *a = actor_ptr(local_18);
        int32_t *rs = route_state(local_18);
        int16_t queue_span;
        uint8_t queue_byte2;          /* polarity in bit 0 */
        uint8_t queue_byte3;          /* sub_lane */
        int polarity_bit;
        int lane_count;
        int orig_queue_span;          /* preserved for REMAP path post-call */

        queue_span  = (int16_t)(qp[0] | ((uint16_t)qp[1] << 8));
        queue_byte2 = qp[2];
        queue_byte3 = qp[3];
        orig_queue_span = (int)queue_span;
        /* [traffic one-way] Force forward heading; otherwise honour TRAFFIC.BUS
         * bit 0. Zeroing here propagates to the polarity store, both yaw-flip
         * sites, and the diagnostic log below. */
        polarity_bit = trf_oneway_traffic() ? 0 : ((int)queue_byte2 & 1);

        /* 0x435989-9d: common writes — polarity, local_c, RECOVERY_STAGE=0,
         * DAT_004afc50=0. Polarity goes to dword 0x3F (= gActorRouteDirectionPolarity
         * @ 0x004afc5c). Prior port also wrote dword 0x25 (legacy macro) but
         * that field has no references in the original — the dual-write was
         * unnecessary and is now removed.
         * `local_c` (slot*4+0x10) writes the field at byte 0x68 within
         * route_state (dword 0x1A) — original semantics unknown; mirror raw. */
        rs[RS_ROUTE_DIRECTION_POLARITY] = polarity_bit;
        rs[0x1A] = local_c;                         /* DAT_004afbc8[slot] */
        rs[RS_RECOVERY_STAGE] = 0;                  /* dword 0x22 */
        rs[RS_SCRIPT_SPEED_PARAM] = 0;              /* dword 0x3C = DAT_004afc50[slot] */
        g_traffic_recovery_stage[local_18] = 0;     /* port-side mirror */

        /* 0x004359a0-bf: compute lane_count for branching */
        {
            const TD5_StripSpan *strip_base =
                (const TD5_StripSpan *)g_strip_span_base;
            int span_idx = (int)queue_span;
            uint8_t strip_byte3 = 0;

            if (strip_base && span_idx >= 0 && span_idx < g_strip_span_count) {
                strip_byte3 = ((const uint8_t *)&strip_base[span_idx])[3];
            }
            lane_count = (int)(strip_byte3 & 0x0F);
        }

        /* Branch on (lane_count > queue.sub_lane) — JA at 0x004359c1.
         * JA-taken (NORMAL path) means lane_count strictly greater. */
        if (lane_count > (int)queue_byte3) {
            /* ---------- NORMAL path (selector = 0, 0x00435b0d) ---------- */
            const TD5_StripSpan *strip_base =
                (const TD5_StripSpan *)g_strip_span_base;
            const TD5_StripVertex *vpool =
                (const TD5_StripVertex *)g_strip_vertex_base;
            const TD5_StripSpan *sp;
            const int16_t *psVar2;   /* left vertex base shorts */
            const int16_t *psVar3;   /* right vertex base shorts */
            int32_t local_10 = 0;    /* dx component */
            int32_t local_14 = 0;    /* dz component */
            int has_geom = 0;
            int angle_full;
            int32_t yaw_stored;

            rs[RS_ROUTE_TABLE_SELECTOR] = 0;

            /* 0x435b10-2b: actor.span_raw = queue.span; actor.sub_lane = queue.byte3 */
            ACTOR_I16(a, ACTOR_SPAN_RAW) = queue_span;
            ACTOR_U8(a, ACTOR_SUB_LANE_INDEX) = queue_byte3;

            /* 0x435b2e: InitActorTrackSegmentPlacement(&actor.span_raw, &actor.world_pos).
             * Byte-faithful port @ 0x00445F10. Seeds:
             *   actor+0x84 (span_accum) = span_raw
             *   actor+0x86 (span_high)  = span_raw
             *   actor+0x8C (sub_lane)   = clamp if >= lane_nibble
             *   actor+0x1FC/+0x200/+0x204 = 4-vertex barycenter (24.8 FP). */
            td5_track_init_actor_segment_placement(
                (int16_t *)(a + ACTOR_SPAN_RAW),
                (int32_t *)(a + ACTOR_WORLD_POS_X));

            /* 0x435b33-72: load strip[span] + first/second vertex pointers */
            sp = NULL;
            psVar2 = psVar3 = NULL;
            if (strip_base && vpool &&
                queue_span >= 0 && queue_span < g_strip_span_count) {
                sp = &strip_base[queue_span];
                psVar2 = (const int16_t *)&vpool[sp->left_vertex_index];
                psVar3 = (const int16_t *)&vpool[sp->right_vertex_index];
                has_geom = 1;
            }

            /* Switch on strip type byte 0 (sp->span_type).
             * Original disassembles to a jump table at 0x00435cb8 covering
             * type-1 in [0,6]; type 0 or type > 7 falls through to the
             * default which leaves local_10/local_14 as their stack values.
             * For first iteration these are uninitialized. To produce
             * deterministic-port behavior on default-type spans, leave both
             * components at 0 (since stack residue is non-deterministic). */
            if (has_geom) {
                switch (sp->span_type) {
                case 1: case 2: case 5: {
                    int32_t dx, dz_part;
                    dx = ((int32_t)psVar2[3] - (int32_t)psVar3[3])
                       - (int32_t)psVar3[0] + (int32_t)psVar2[0];
                    local_10 = (dx + ((dx >> 31) & 3)) >> 2;
                    dz_part = ((int32_t)psVar2[5] - (int32_t)psVar3[5])
                            - (int32_t)psVar3[2];
                    {
                        int32_t dz = dz_part + (int32_t)psVar2[2];
                        local_14 = (dz + ((dz >> 31) & 3)) >> 2;
                    }
                    break;
                }
                case 3: case 4: {
                    int32_t dx, dz_part;
                    dx = ((int32_t)psVar2[3] - (int32_t)psVar3[6])
                       - (int32_t)psVar3[3] + (int32_t)psVar2[0];
                    local_10 = (dx + ((dx >> 31) & 3)) >> 2;
                    dz_part = ((int32_t)psVar2[5] - (int32_t)psVar3[8])
                            - (int32_t)psVar3[5];
                    {
                        int32_t dz = dz_part + (int32_t)psVar2[2];
                        local_14 = (dz + ((dz >> 31) & 3)) >> 2;
                    }
                    break;
                }
                case 6: case 7: {
                    int32_t dx, dz;
                    dx = ((int32_t)psVar2[6] - (int32_t)psVar3[3])
                       + (int32_t)psVar2[3] - (int32_t)psVar3[0];
                    local_10 = (dx + ((dx >> 31) & 3)) >> 2;
                    dz = ((int32_t)psVar2[8] - (int32_t)psVar3[5])
                       - (int32_t)psVar3[2] + (int32_t)psVar2[5];
                    local_14 = (dz + ((dz >> 31) & 3)) >> 2;
                    break;
                }
                default:
                    /* Original default: skip recompute, retain stack values.
                     * Port treats as 0/0 for determinism. */
                    break;
                }
            }

            /* 0x435c37-44: yaw = (AngleFromVector12Full(dx, dz) + 0x800) << 8 */
            angle_full = ai_angle_from_vector(local_10, local_14);
            yaw_stored = (angle_full + 0x800) << 8;
            ACTOR_I32(a, ACTOR_YAW_ACCUM) = yaw_stored;

            /* 0x435c47-58: polarity flip — add 0x80000 */
            if (polarity_bit) {
                yaw_stored += 0x80000;
                ACTOR_I32(a, ACTOR_YAW_ACCUM) = yaw_stored;
            }

            /* 0x435c5c-60: ResetVehicleActorState(actor) */
            td5_physics_reset_actor_state((TD5_Actor *)a);

            /* 0x435c69-6a: RefreshActorTrackProgressOffset(slot) */
            td5_ai_seed_actor_track_progress_offset(local_18);

            /* 0x435c6f-70: NormalizeActorTrackWrapState(actor) */
            td5_track_normalize_actor_wrap((TD5_Actor *)a);
        }
        else {
            /* ---------- REMAP path (selector = 1, 0x004359c7) ----------
             *
             * Original walks the junction-remap table at DAT_004c3da0+0x18,
             * matching queue.span into [range_lo, range_lo + (B - A) - 1].
             * On match: remapped = (A - C) + queue.span. On miss: -1.
             *
             * Port reuses td5_track_apply_target_span_remap which performs
             * the same walker but returns the input span (not -1) on miss.
             * [ARCH-DIVERGENCE D1] queue records whose span is outside every
             * junction range will have remapped_span = queue.span here, vs
             * -1 in original. Local re-check below substitutes -1 to match
             * orig externally. See
             * reference_arch_init_traffic_actors_2026-05-18.md. */
            int remapped_int;
            int16_t remapped_span;

            rs[RS_ROUTE_TABLE_SELECTOR] = 1;

            remapped_int = td5_track_apply_target_span_remap((int)queue_span, 0);

            if (remapped_int == (int)queue_span) {
                /* Junction-remap MISS. This fires on the 2nd (down-track) traffic
                 * fill when a queue record asks for a BRANCH lane
                 * (sub_lane >= lane_count) off a span that has no matching
                 * junction-remap entry on this track. The original sets a -1
                 * sentinel and then indexes strip[-1] (garbage); the port's
                 * range-guarded placement zeroed that to the world ORIGIN,
                 * stranding a visible, immovable traffic car off-track
                 * (user-reported "traffic standing still" / dead car at spawn).
                 *
                 * [SOURCE-PORT FIX S20 2026-06-05] Instead of stranding it, fall
                 * back to a NORMAL main-road placement at the queued span on the
                 * canonical (LEFT) route, so the car spawns on-track and cruises
                 * like the rest. Selector + route ptr are set to canonical so the
                 * Stage-2 heading check stays aligned (a branch-route car on the
                 * main road would otherwise trip the recovery brake). Strictly
                 * better than the origin-strand; only changes the miss case. */
                remapped_span = (int16_t)queue_span;
                rs[RS_ROUTE_TABLE_SELECTOR] = 0;
                if (g_route_tables[0])
                    rs[RS_ROUTE_TABLE_PTR] = (int32_t)(intptr_t)g_route_tables[0];
                TD5_LOG_I(LOG_TAG,
                          "init_remap_MISS: slot=%d queue_span=%d sub_lane=%d "
                          "lane_count=%d -> main-road fallback (was origin-strand)",
                          local_18, (int)queue_span, (int)queue_byte3, lane_count);
            } else {
                remapped_span = (int16_t)remapped_int;
            }

            /* 0x00435a2b-5b: actor.span_raw = remapped; actor.sub_lane =
             * queue.byte3 - lane_count. */
            ACTOR_I16(a, ACTOR_SPAN_RAW) = remapped_span;
            ACTOR_U8(a, ACTOR_SUB_LANE_INDEX) = (uint8_t)((int)queue_byte3 - lane_count);

            /* 0x00435a5e: InitActorTrackSegmentPlacement(&span_raw, &world_pos).
             * Byte-faithful port @ 0x00445F10 — defensive guard: original has none
             * but our table-driven path can index into NULL span pool. The helper
             * itself early-returns with zeroed out_pos when track isn't bound. */
            td5_track_init_actor_segment_placement(
                (int16_t *)(a + ACTOR_SPAN_RAW),
                (int32_t *)(a + ACTOR_WORLD_POS_X));

            /* 0x00435a67: ComputeActorTrackHeading(actor) → 12-bit angle.
             * Inline the original switch + AngleFromVector12 to avoid the
             * extra +0x290 heading_normal write that td5_track_compute_heading
             * (port of 0x00434350) performs. */
            {
                const TD5_StripSpan *strip_base =
                    (const TD5_StripSpan *)g_strip_span_base;
                const TD5_StripVertex *vpool =
                    (const TD5_StripVertex *)g_strip_vertex_base;
                const TD5_StripSpan *sp = NULL;
                const int16_t *psVar2 = NULL;
                const int16_t *psVar3 = NULL;
                int32_t uVar9 = 0, param_1 = 0;
                int angle12;
                int32_t yaw_stored;
                int rs_span = (int)(int16_t)ACTOR_I16(a, ACTOR_SPAN_RAW);

                if (strip_base && vpool &&
                    rs_span >= 0 && rs_span < g_strip_span_count) {
                    sp = &strip_base[rs_span];
                    psVar2 = (const int16_t *)&vpool[sp->left_vertex_index];
                    psVar3 = (const int16_t *)&vpool[sp->right_vertex_index];

                    switch (sp->span_type) {
                    case 1: case 2: case 5: {
                        int32_t iVar6 = ((int32_t)psVar2[3] - (int32_t)psVar3[3])
                                      - (int32_t)psVar3[0] + (int32_t)psVar2[0];
                        int32_t iVar5 = (int32_t)psVar2[5] - (int32_t)psVar3[5]
                                      - (int32_t)psVar3[2] + (int32_t)psVar2[2];
                        iVar6 = iVar6 + ((iVar6 >> 31) & 3);
                        param_1 = (iVar5 + ((iVar5 >> 31) & 3)) >> 2;
                        uVar9 = iVar6 >> 2;
                        break;
                    }
                    case 3: case 4: {
                        int32_t iVar6 = ((int32_t)psVar2[3] - (int32_t)psVar3[6])
                                      - (int32_t)psVar3[3] + (int32_t)psVar2[0];
                        int32_t iVar5 = (int32_t)psVar2[5] - (int32_t)psVar3[8]
                                      - (int32_t)psVar3[5] + (int32_t)psVar2[2];
                        iVar6 = iVar6 + ((iVar6 >> 31) & 3);
                        param_1 = (iVar5 + ((iVar5 >> 31) & 3)) >> 2;
                        uVar9 = iVar6 >> 2;
                        break;
                    }
                    case 6: case 7: {
                        int32_t iVar6 = ((int32_t)psVar2[6] - (int32_t)psVar3[3])
                                      + (int32_t)psVar2[3] - (int32_t)psVar3[0];
                        int32_t iVar5 = ((int32_t)psVar2[8] - (int32_t)psVar3[5])
                                      - (int32_t)psVar3[2] + (int32_t)psVar2[5];
                        param_1 = (iVar5 + ((iVar5 >> 31) & 3)) >> 2;
                        uVar9 = (iVar6 + ((iVar6 >> 31) & 3)) >> 2;
                        break;
                    }
                    default:
                        /* default leaves uVar9 = param_1 = 0 (initial). */
                        break;
                    }
                }

                /* 0x00435dd1-44: quadrant-fold dispatch onto AngleFromVector12.
                 * Use the existing port helper ai_angle_from_vector for the
                 * full-circle equivalent. */
                angle12 = ai_angle_from_vector(uVar9, param_1);
                yaw_stored = (angle12 + 0x800) << 8;
                ACTOR_I32(a, ACTOR_YAW_ACCUM) = yaw_stored;

                if (polarity_bit) {
                    yaw_stored += 0x80000;
                    ACTOR_I32(a, ACTOR_YAW_ACCUM) = yaw_stored;
                }
            }

            /* 0x00435a91: ResetVehicleActorState(actor) */
            td5_physics_reset_actor_state((TD5_Actor *)a);

            /* 0x00435a96-b8: ComputeTrackSpanProgress + ComputeSignedTrackOffset
             * — seed RS_TRACK_PROGRESS and RS_TRACK_OFFSET_BIAS. The original
             * indexes the route table by actor.span_normalized (which at this
             * point is still zero/uninit); the port helper does likewise. */
            {
                int span_raw = (int)(int16_t)ACTOR_I16(a, ACTOR_SPAN_RAW);
                int span_norm = (int)(int16_t)ACTOR_I16(a, ACTOR_SPAN_NORMALIZED);
                int32_t pos[3];
                int64_t prog64;
                int32_t progress;
                int route_byte = 0;
                pos[0] = ACTOR_I32(a, ACTOR_WORLD_POS_X);
                pos[1] = *(int32_t *)(a + 0x200);
                pos[2] = ACTOR_I32(a, ACTOR_WORLD_POS_Z);
                prog64 = td5_track_compute_span_progress(span_raw, pos);
                progress = (int32_t)(uint32_t)(prog64 & 0xFFFFFFFF);
                rs[RS_TRACK_PROGRESS] = progress;

                {
                    const uint8_t *rb =
                        (const uint8_t *)(intptr_t)rs[RS_ROUTE_TABLE_PTR];
                    if (rb && span_norm >= 0) {
                        route_byte = (int)rb[(size_t)(unsigned)span_norm * 3u];
                    }
                }
                rs[RS_TRACK_OFFSET_BIAS] =
                    td5_track_compute_signed_offset(span_raw, progress, route_byte);
            }

            /* 0x00435abe-c1: NormalizeActorTrackWrapState(actor) */
            td5_track_normalize_actor_wrap((TD5_Actor *)a);

            /* 0x00435ac6-d1: actor.span_normalized = ORIGINAL queue.span
             * (the un-remapped one), as final write. */
            *(int16_t *)(a + 0x082) = (int16_t)orig_queue_span;
        }

        /* ---------- common trailing ops (port-side bookkeeping) ----------
         * [ARCH-DIVERGENCE D2] The original 0x00435940 does NOT zero
         * steering_cmd / vehicle_mode / etc — those resets happen in
         * ResetVehicleActorState. We also mirror the slot index here for
         * AI dispatcher consumption (port-only). Orig achieves equivalent
         * state via EBX address-mode addressing. See
         * reference_arch_init_traffic_actors_2026-05-18.md. */
        ACTOR_U8(a, ACTOR_SLOT_INDEX) = (uint8_t)local_18;
        rs[RS_SLOT_INDEX] = local_18;
        rs[RS_ENCOUNTER_HANDLE] = -1;

        TD5_LOG_I(LOG_TAG,
                  "init_traffic: slot=%d span=%d remapped=%d lane=%d "
                  "polarity=%d sel=%d pos=(%d,%d,%d)",
                  local_18, orig_queue_span,
                  (int)(int16_t)ACTOR_I16(a, ACTOR_SPAN_RAW),
                  (int)ACTOR_U8(a, ACTOR_SUB_LANE_INDEX),
                  polarity_bit, (int)rs[RS_ROUTE_TABLE_SELECTOR],
                  ACTOR_I32(a, ACTOR_WORLD_POS_X),
                  *(int32_t *)(a + 0x200),
                  ACTOR_I32(a, ACTOR_WORLD_POS_Z));

        /* 0x00435c8b-ca5: advance queue pointer, slot, local_c, racer_cap reload */
        qp += 4;
        local_18 += 1;
        local_c  += 4;

        /* 0x00435c85-78: reload g_racerCount each iteration (in case it
         * changed; mirror original even if our port doesn't mutate it
         * mid-loop). [PORT: N-way] re-cap to the traffic region end
         * (g_traffic_slot_base + TD5_MAX_TRAFFIC_SLOTS) so big fields fill all
         * traffic slots, not just one — must match the top cap at 0x435975.
         * Legacy (base 6) => 12, byte-identical. */
        racer_count = g_active_actor_count;
        racer_cap = (racer_count > g_traffic_slot_base + TD5_MAX_TRAFFIC_SLOTS)
                    ? (g_traffic_slot_base + TD5_MAX_TRAFFIC_SLOTS) : racer_count;
    }

    g_traffic_queue_ptr = qp;
}

/**
 * UpdateTrafficRoutePlan: 7-stage FSM per traffic slot.
 *
 * Stage 1 (recycle):   Call RecycleTrafficActorFromQueue
 * Stage 2 (heading):   Heading misalignment check -> enter recovery if > 90 deg
 * Stage 3 (edge):      Edge-of-track / recovery bail-out -> brake and return
 * Stage 4 (normal):    Set constant speed (encounter_steering_override = 0x3C)
 * Stage 5 (target):    Compute next target span (accounting for direction polarity)
 * Stage 6 (steer):     Call UpdateActorSteeringBias with weight 0x8000
 * Stage 7 (yield):     FindNearestRoutePeer -> brake if closing on peer
 *
 * [precise-00435E80] Byte-faithful port from 0x00435E80 disassembly listing.
 *
 * [CONFIRMED @ 0x00435E80] L5 promotion sweep audit (2026-05-18).
 *   - Stages 1-7 enumerated above match orig FSM block layout
 *     (0x00435E80..0x00436A6F, 2142 bytes / 521 instructions).
 *   - Stage 2 ref_slot via rs[RS_SLOT_INDEX], not param_1 — CONFIRMED at
 *     0x00435EA6-0x00435FA8 EBX+0xD4 dispatch.
 *   - Stage 2 polarity reads RS_ROUTE_DIRECTION_POLARITY (dword 0x3F),
 *     not the prior incorrectly-aliased 0x25 — CONFIRMED at 0x00435EF4.
 *   - Stage 2 strict hdelta band (0x400, 0xC00) — CONFIRMED at
 *     0x00435F2B JLE / 0x00435F32 JGE pair.
 *   - Stage 2 special-encounter audio cleanup asymmetric — orig only
 *     clears g_encounter_tracked_handle on polarity==0 path.
 *   - Stage 5 target span calculation accounts for polarity sign flip.
 *   - Stage 7 yield brake — FindNearestRoutePeer dispatch verified.
 *
 * KNOWN DIVERGENCES: none documented (port commits document the four
 * shipped fixes vs prior approximate port).
 *
 * Effective level: L5 (byte-faithful end-to-end).
 *
 * Notable fixes vs prior approximate port:
 *   - Stage 2 reads heading/yaw_accum from the REFERENCE actor
 *     (actor_ptr(rs[RS_SLOT_INDEX])), not from the param_1 actor. Original
 *     dereferences EBX+0xD4 = rs[RS_SLOT_INDEX] before every per-actor read.
 *   - Stage 2 random-recovery seed reads actor[0].world_pos_z (DAT_004ab30c),
 *     not the local g_ai_frame_counter. Same source the original uses.
 *   - Stage 2 special-encounter cleanup: slot==9 + !wanted_mode in
 *     polarity==0 path stops audio AND clears handle. polarity!=0 path
 *     stops audio only — DOES NOT clear the handle. Original asymmetric.
 *   - Stage 3 bail-out checks rs[RS_SCRIPT_SPEED_PARAM] (DAT_004afc50) and
 *     g_traffic_recovery_stage[slot] (DAT_004afbe8). Prior port duplicated
 *     the recovery check and never read the script-speed-param sentinel.
 *   - Stage 2 hdelta band check uses strict bounds (0x400, 0xC00) per the
 *     JLE/JGE pair at 0x435F2B/0x435F32, not >= and <=.
 */

/* ========================================================================
 * SOURCE-PORT ENHANCEMENT — Smart Traffic (S20, 2026-06-05)
 *
 * The original background traffic is deliberately simple: a flat 0x3c cruise
 * command [CONFIRMED @ 0x00435E80], a fully deterministic junction route
 * (a linear remap-table walk, NO RNG — confirmed _rand @ 0x00448157 is never
 * called from the route path), and no active lateral wall avoidance (traffic
 * slots >= 6 are even skipped by the lateral wall-contact synthesis, see
 * td5_track.c:1035). This block layers three OPTIONAL, individually-gateable
 * behaviours on top, applied ONLY to traffic slots (>= g_traffic_slot_base);
 * racing AI (slots 0..5, a different code path entirely) is untouched. With
 * [Traffic] TrafficSmart=0 the traffic is byte-faithful again.
 *
 *   1. WallAvoid     — bias an edge-lane car's lateral target toward the lane
 *                      interior so it stops scraping the rail.
 *   2. AvoidSlowLane — prefer the asphalt lane over an off-road shoulder lane
 *                      (the only per-lane attribute the track exposes is the
 *                      lane bitmask + surface_attribute; there is NO native
 *                      lane-speed field, so this is a clearly-marked heuristic).
 *   3. Lookahead     — when a car is close ahead in our lane, change to a clear
 *                      adjacent lane (and ease the hard brake) instead of just
 *                      ramming/braking. Falls back to the faithful TTC brake.
 *
 * All three operate purely on the traffic car's lateral target / chosen sub-lane
 * — they do NOT touch route_state, the route table, or the actor's yaw, so they
 * neither trip the heading-recovery brake nor perturb racer routing.
 *
 * REMOVED (was behaviour "RandomBranch"): assigning each traffic car a random
 * route table to vary branches at forks. TD5 traffic STRICTLY follows one
 * prescribed route, enforced by the Stage-2 heading-misalignment recovery brake
 * (UpdateTrafficRoutePlan @ 0x00435E80): re-pointing a live car at a different
 * route table desynced its yaw from the new route's heading bytes, tripping the
 * recovery brake -> the car froze in place until recycled (visible as "traffic
 * standing still"). It also fed the faithful racer peer-scan
 * td5_ai_find_offset_peer (which reads traffic route_state) and shifted racer
 * race-lines. A faithful, stable random-branch lever does not exist on top of
 * the route-following + recovery design, so the behaviour was dropped. */

/* Per-actor smart-traffic state (indexed by actor slot). */
static int8_t   s_traffic_lane_bias[TD5_MAX_TOTAL_ACTORS];     /* situational lane offset (-1/0/+1), 1-tick latency */
static int      s_traffic_stuck_frames[TD5_MAX_TOTAL_ACTORS];  /* AntiFreeze: consecutive recovery-frozen ticks */
/* [#3 COLLISION-DEADLOCK ESCAPE 2026-06-19] */
static int      s_traffic_stall_frames[TD5_MAX_TOTAL_ACTORS];  /* consecutive near-stopped ticks */
static int16_t  s_traffic_escape_ticks[TD5_MAX_TOTAL_ACTORS];  /* remaining escape-burst ticks */
static int8_t   s_traffic_escape_side[TD5_MAX_TOTAL_ACTORS];   /* current escape steer side (-1/+1) */
/* [R3-10 ANTI-PILEUP STRENGTHEN 2026-06-19] Forward-PROGRESS jam detection +
 * persistent escape lane. The old escape only fired on |speed|<thresh for 45
 * ticks, so a car that keeps BUMPING (grinding forward a few units, never fully
 * stopping) never qualified and ground against its neighbour forever. These
 * track real along-track progress via ACTOR_SPAN_ACCUM (monotonic, remap-immune)
 * over a sliding window: too few spans of progress over the window == jammed,
 * regardless of instantaneous speed. After a burst, an escape LANE bias + a
 * cooldown make the car commit to a clear lane instead of re-braking into the
 * same peer and re-jamming. All deterministic (replicated span accum + tick
 * counters + slot parity, no rand) -> MP-lockstep safe. */
static int16_t  s_traffic_prog_accum[TD5_MAX_TOTAL_ACTORS];    /* span-accum at window start */
static int16_t  s_traffic_prog_window[TD5_MAX_TOTAL_ACTORS];   /* ticks elapsed in this window */
static int16_t  s_traffic_lowprog[TD5_MAX_TOTAL_ACTORS];       /* consecutive low-progress ticks */
static uint8_t  s_traffic_lowprog_latch[TD5_MAX_TOTAL_ACTORS]; /* 1 = last window showed no progress */
static int16_t  s_traffic_escape_cooldown[TD5_MAX_TOTAL_ACTORS];/* post-escape re-arm suppression */
/* s_traffic_escape_lane / s_traffic_escape_lane_ttl are declared near the top
 * globals (td5_ai_smart_traffic_lane, defined earlier, reads them). */

/* [AI UNSTICK 2026-06-26] Per-RACER collision-escape state. Racer slots
 * (0..g_traffic_slot_base) never had the jam-escape that traffic and cops do,
 * so two wedged racers — or a racer vs the player — pushed at full throttle
 * forever until an external force broke them apart. These mirror the traffic
 * escape exactly (no-progress detection -> short steer-away burst) and are
 * fully deterministic (span-accum + tick counters + slot parity, no rand) so
 * MP lockstep is preserved. */
static int16_t  s_racer_escape_ticks[TD5_MAX_TOTAL_ACTORS];    /* remaining brake-yield burst ticks */
static int8_t   s_racer_escape_side[TD5_MAX_TOTAL_ACTORS];     /* committed steer-around side (-1/+1) */
static int16_t  s_racer_escape_cooldown[TD5_MAX_TOTAL_ACTORS]; /* post-burst re-arm suppression */
static uint8_t  s_racer_has_moved[TD5_MAX_TOTAL_ACTORS];       /* gate: don't act at the start line */
static uint8_t  s_racer_stall_active[TD5_MAX_TOTAL_ACTORS];    /* 1 = sustained low-speed steer-around running */
static int16_t  s_racer_recover_ticks[TD5_MAX_TOTAL_ACTORS];   /* ticks back up to speed (release the steer) */
/* V2V grind detection: physics sets the flag each tick this racer touches a car;
 * the leaky load climbs while grinding and decays when clear (catches an
 * intermittent side-by-side rub that a speed/progress test misses). */
static uint8_t  s_racer_contact_flag[TD5_MAX_TOTAL_ACTORS];    /* set by physics, consumed by AI */
static int8_t   s_racer_contact_peer[TD5_MAX_TOTAL_ACTORS];    /* slot most recently collided with */
static int16_t  s_racer_contact_load[TD5_MAX_TOTAL_ACTORS];    /* leaky accumulator of recent contacts */

/* Per-race diagnostic counters — observe which behaviours actually trigger
 * (the rate-limited per-event logs can miss firings). Dumped periodically. */
static struct {
    int react_calls;          /* react_to_peer entered with a real peer       */
    int ease_diff_lane;       /* eased: nearest peer in a different lane       */
    int ease_lane_change;     /* eased: same-lane, changed to a clear lane     */
    int blocked_single_lane;  /* couldn't act: span had <=1 lane (brake stays) */
    int no_clear_lane;        /* same-lane peer but no clear adjacent (brake)   */
    int slow_lane_change;     /* choose_lane stepped off a slow/off-road lane  */
    int wall_nudges;          /* edge-lane targets nudged toward lane centre   */
} s_smart_stat;

/* Reset all smart-traffic per-actor state. Called once per race from
 * td5_ai_init_race_actor_runtime(). */
static void td5_traffic_smart_reset(void) {
    for (int i = 0; i < TD5_MAX_TOTAL_ACTORS; i++) {
        s_traffic_lane_bias[i] = 0;
        s_traffic_stuck_frames[i] = 0;
        s_traffic_stall_frames[i] = 0;
        s_traffic_escape_ticks[i] = 0;
        s_traffic_escape_side[i]  = 0;
        s_traffic_prog_accum[i]   = 0;
        s_traffic_prog_window[i]  = 0;
        s_traffic_lowprog[i]      = 0;
        s_traffic_lowprog_latch[i] = 0;
        s_traffic_escape_cooldown[i] = 0;
        s_traffic_escape_lane[i]  = 0;
        s_traffic_escape_lane_ttl[i] = 0;
        /* [AI UNSTICK] racer collision-escape state (per race). */
        s_racer_escape_ticks[i]    = 0;
        s_racer_escape_side[i]     = 0;
        s_racer_escape_cooldown[i] = 0;
        s_racer_has_moved[i]       = 0;
        s_racer_stall_active[i]    = 0;
        s_racer_recover_ticks[i]   = 0;
        s_racer_contact_flag[i]    = 0;
        s_racer_contact_peer[i]    = -1;
        s_racer_contact_load[i]    = 0;
    }
    memset(&s_smart_stat, 0, sizeof(s_smart_stat));
}

/* AntiFreeze (source-port enhancement): the faithful traffic recovery-brake only
 * clears via RecycleTrafficActorFromQueue, which requires the player to advance
 * (~41 spans) — so a heading-misaligned traffic car freezes PERMANENTLY when the
 * player is parked/slow. This un-sticks a car that has been recovery-frozen for
 * `traffic_antifreeze_frames` consecutive ticks: clear the recovery flag and
 * re-align its heading to the road geometry (so Stage-2 below does not just
 * re-arm it), reset its velocity, and re-seed track progress. Independent of the
 * "smart" gate; default ON. Skips the active special-encounter cop (slot 9). */
static void traffic_smart_antifreeze(int slot, char *actor, int32_t *rs) {
    if (!g_td5.ini.traffic_antifreeze)
        return;
    if (slot < g_traffic_slot_base || slot >= TD5_MAX_TOTAL_ACTORS)
        return;
    if (slot == 9 && g_encounter_tracked_handle != -1) {
        s_traffic_stuck_frames[slot] = 0;
        return;
    }
    if (g_traffic_recovery_stage[slot] == 0) {
        s_traffic_stuck_frames[slot] = 0;   /* not frozen — reset the counter */
        return;
    }
    if (++s_traffic_stuck_frames[slot] < g_td5.ini.traffic_antifreeze_frames)
        return;                              /* not stuck long enough yet */

    /* Stuck too long — un-stick in place. */
    s_traffic_stuck_frames[slot] = 0;
    g_traffic_recovery_stage[slot] = 0;
    /* Re-align heading to the road at the current span (same helper the faithful
     * recycle uses), then flip 180 deg for reverse-polarity (oncoming) traffic,
     * matching the spawn/recycle convention. This keeps Stage-2's heading delta
     * small so it does not immediately re-arm the recovery brake. */
    td5_track_compute_heading((TD5_Actor *)actor);
    if ((rs[RS_ROUTE_DIRECTION_POLARITY] & 1) != 0)
        ACTOR_I32(actor, ACTOR_YAW_ACCUM) += 0x80000;
    td5_physics_reset_actor_state((TD5_Actor *)actor);
    td5_ai_seed_actor_track_progress_offset(slot);
    td5_track_normalize_actor_wrap((TD5_Actor *)actor);
    if ((g_ai_frame_counter % 30u) == 0u)
        TD5_LOG_I(LOG_TAG,
                  "traffic_antifreeze: slot=%d unstuck at span=%d (cleared recovery + realigned)",
                  slot, (int)ACTOR_I16(actor, ACTOR_SPAN_RAW));
}

/* [#3 COLLISION-DEADLOCK ESCAPE 2026-06-19] Safety net for traffic cars that pile
 * into each other (or geometry) and cannot free themselves. When a traffic car
 * has been effectively stopped for ~1.5 s — and it is NOT a deliberate stop
 * (broken-down / parked-despawn / cop) — force a brief "steer-away" burst: brake
 * off, throttle on, and a hard steering command to one side, so it peels off
 * whatever it is jammed against. The steer side alternates on each engagement
 * (seeded by slot parity so adjacent jammed cars peel opposite ways), so a car
 * that jams a wall on one try escapes the other way next time. It drives the same
 * control fields normal traffic uses, applied as a POST-override at the end of the
 * per-slot update. Deterministic (speed + tick counters + slot parity, no rand)
 * -> MP-lockstep safe. The existing 45-tick stuck despawn stays as the last-resort
 * backstop. Knob TD5RE_TRAFFIC_ESCAPE (default on). */
static int traffic_escape_enabled(void) {
    static int s = -1;
    if (s < 0) {
        const char *e = getenv("TD5RE_TRAFFIC_ESCAPE");
        s = (!e || e[0] != '0') ? 1 : 0;
    }
    return s;
}

/* [R3-10] Anti-pileup tuning. JAM = low FORWARD progress (not just low speed):
 * fewer than JAM_MIN_SPANS of span-accum gained over a JAM_WINDOW-tick window
 * counts the window as "no progress"; LOWPROG_TICKS such ticks in a row arm the
 * escape. This catches the grind-bumpers the speed-only test missed. After the
 * burst the car commits to a clear lane for ESCAPE_LANE_TTL ticks and suppresses
 * re-arming for ESCAPE_COOLDOWN ticks so it does not instantly re-jam. */
#define TRAFFIC_JAM_WINDOW       30   /* ~1s progress-sampling window            */
#define TRAFFIC_JAM_MIN_SPANS     1   /* < this many spans/window == stalled     */
#define TRAFFIC_LOWPROG_TICKS    45   /* ~1.5s of no progress -> escape          */
#define TRAFFIC_ESCAPE_BURST     36   /* ~1.2s steer-away burst (was 24)         */
#define TRAFFIC_ESCAPE_COOLDOWN  30   /* ~1s after a burst before re-arming      */
#define TRAFFIC_ESCAPE_LANE_TTL  90   /* ~3s commit to the cleared lane          */

/* [R3-10] Pick the side (-1/+1) with more lateral room for `slot` to escape
 * toward: compare lane occupancy just ahead on each side via the existing
 * lane-clear scan. Falls back to slot-parity-alternation (deterministic) when
 * both sides are equally (un)clear, so adjacent jammed cars peel opposite ways.
 * Pure function of replicated lane/span state -> lockstep-safe. */
static int8_t traffic_escape_pick_side(int slot, char *actor) {
    int span  = (int)ACTOR_I16(actor, ACTOR_SPAN_RAW);
    int lane  = (int)ACTOR_U8(actor, ACTOR_SUB_LANE_INDEX);
    int pol   = (int)(route_state(slot)[RS_ROUTE_DIRECTION_POLARITY] & 1);
    int left_clear  = traffic_lane_is_clear(slot, span, lane - 1, pol);
    int right_clear = traffic_lane_is_clear(slot, span, lane + 1, pol);
    if (left_clear && !right_clear) return (int8_t)-1;
    if (right_clear && !left_clear) return (int8_t)+1;
    /* Tie: alternate off the previous side (seeded by parity). */
    {
        int8_t prev = s_traffic_escape_side[slot];
        if (prev == 0) prev = (slot & 1) ? (int8_t)1 : (int8_t)-1;
        return (int8_t)-prev;
    }
}

static void traffic_collision_escape(int slot) {
    if (!traffic_escape_enabled()) return;
    if (slot < g_traffic_slot_base || slot >= TD5_MAX_TOTAL_ACTORS) return;
    if (g_cop_is_cop[slot]) return;                         /* cops run their own driver */

    char *actor = actor_ptr(slot);

    if (g_actor_broken_down[slot] || td5_ai_traffic_dynamic_parked(slot)) {
        s_traffic_stall_frames[slot]    = 0;
        s_traffic_escape_ticks[slot]    = 0;
        s_traffic_lowprog[slot]         = 0;
        s_traffic_lowprog_latch[slot]   = 0;
        s_traffic_escape_cooldown[slot] = 0;
        s_traffic_escape_lane[slot]     = 0;
        s_traffic_escape_lane_ttl[slot] = 0;
        s_traffic_prog_window[slot]     = 0;
        return;                                             /* deliberate stop, not a jam */
    }

    /* Persistent escape-lane commit: count down the TTL. Both lane choosers
     * (td5_ai_smart_traffic_lane and traffic_smart_choose_lane) read
     * s_traffic_escape_lane/_ttl DIRECTLY to bias toward the cleared side while
     * this is live, so the car heads into a different lane instead of re-braking
     * into the same peer. (Not routed through s_traffic_lane_bias — the route
     * plan zeroes that every tick before the chooser runs.) */
    if (s_traffic_escape_lane_ttl[slot] > 0)
        s_traffic_escape_lane_ttl[slot]--;
    if (s_traffic_escape_cooldown[slot] > 0)
        s_traffic_escape_cooldown[slot]--;

    /* Active escape burst: override the route plan's brake with a forward
     * steer-away so the car physically peels off the obstacle. */
    if (s_traffic_escape_ticks[slot] > 0) {
        s_traffic_escape_ticks[slot]--;
        ACTOR_U8(actor,  ACTOR_BRAKE_FLAG)     = 0;
        ACTOR_U8(actor,  ACTOR_THROTTLE_STATE) = 1;
        ACTOR_I32(actor, ACTOR_STEERING_CMD)   =
            (int32_t)s_traffic_escape_side[slot] * 0x12000;  /* near full-lock to one side */
        return;
    }

    /* Jam detection. TWO signals, either one arms the escape:
     *  (1) near-zero SPEED for LOWPROG_TICKS straight (the classic full-stop jam);
     *  (2) low forward PROGRESS — < JAM_MIN_SPANS of span-accum gained over a
     *      JAM_WINDOW-tick window — which catches a car that keeps BUMPING and
     *      grinding forward a hair at a time but never actually advances.
     * Both feed the same s_traffic_lowprog counter. */
    int32_t spd  = ACTOR_I32(actor, ACTOR_LONGITUDINAL_SPEED);
    int32_t aspd = (spd < 0) ? -spd : spd;

    /* Sliding progress window on the monotonic span accumulator. The result is
     * LATCHED until the next window completes, so a grind-bumper (speed never
     * fully zero) is flagged jammed on EVERY tick of the window, not just on the
     * single tick the window closes — otherwise the per-tick lowprog counter
     * would barely advance and the escape would take ~45s to arm. */
    if (s_traffic_prog_window[slot] == 0)
        s_traffic_prog_accum[slot] = ACTOR_I16(actor, ACTOR_SPAN_ACCUM);
    if (++s_traffic_prog_window[slot] >= TRAFFIC_JAM_WINDOW) {
        int prog = (int)(int16_t)(ACTOR_I16(actor, ACTOR_SPAN_ACCUM) -
                                  s_traffic_prog_accum[slot]);
        if (prog < 0) prog = -prog;                          /* oncoming counts down */
        s_traffic_lowprog_latch[slot] = (prog < TRAFFIC_JAM_MIN_SPANS) ? 1 : 0;
        s_traffic_prog_window[slot] = 0;                     /* restart the window */
    }

    if (aspd < 0x1800 || s_traffic_lowprog_latch[slot]) {
        /* Don't re-arm immediately after a burst (let the steer-away land). */
        if (s_traffic_escape_cooldown[slot] == 0 &&
            ++s_traffic_lowprog[slot] >= TRAFFIC_LOWPROG_TICKS) {
            s_traffic_lowprog[slot]      = 0;
            s_traffic_escape_side[slot]  = traffic_escape_pick_side(slot, actor);
            s_traffic_escape_ticks[slot] = TRAFFIC_ESCAPE_BURST;
            /* Commit to the cleared lane after the burst so the route plan heads
             * there instead of re-braking into the same neighbour. */
            s_traffic_escape_lane[slot]     = s_traffic_escape_side[slot];
            s_traffic_escape_lane_ttl[slot] = TRAFFIC_ESCAPE_LANE_TTL;
            s_traffic_escape_cooldown[slot] = TRAFFIC_ESCAPE_BURST + TRAFFIC_ESCAPE_COOLDOWN;
            if ((g_ai_frame_counter % 30u) == 0u)
                TD5_LOG_I(LOG_TAG,
                    "traffic_escape: slot=%d JAM (spd=%d prog_latch=%d) -> burst side=%d "
                    "lane_commit=%d", slot, aspd, (int)s_traffic_lowprog_latch[slot],
                    s_traffic_escape_side[slot], s_traffic_escape_lane[slot]);
        }
        /* legacy near-stopped counter (diagnostic; the actual stuck-despawn
         * backstop keys off s_traffic_stuck_frames in the AntiFreeze layer). */
        s_traffic_stall_frames[slot]++;
    } else {
        s_traffic_stall_frames[slot] = 0;                     /* moving fine */
        if (s_traffic_lowprog[slot] > 0) s_traffic_lowprog[slot]--;
    }
}

/* [dynamic-traffic] forward decls — defined in the dynamic-traffic module
 * below; used here so the SmartAI lane-changer never moves a car into a lane
 * of the opposite drive direction (lanes carry a direction when Dynamic=1). */
static int trf_dyn_lane_direction(int lane_count, int lane);
static int trf_dyn_lane_change_blocked(int slot, int lane_count, int cand_lane);

/* Scan active actors for one occupying `target_lane` near `self_span` (a car
 * beside or just ahead). Returns 1 if the lane is clear. */
static int traffic_lane_is_clear(int self_slot, int self_span,
                                 int target_lane, int polarity) {
    int n = g_active_actor_count;
    if (n > TD5_MAX_TOTAL_ACTORS) n = TD5_MAX_TOTAL_ACTORS;
    for (int i = 0; i < n; i++) {
        char *a;
        int lane, span, diff;
        if (i == self_slot) continue;
        /* [PER-VIEWPORT TRAFFIC] only consider same-partition cars (+ own player):
         * a partition must not see another partition's twin when picking a clear
         * lane, or the placements desync. */
        if (td5_ai_traffic_pair_blocked(self_slot, i)) continue;
        if (!ai_peer_is_present(i)) continue;   /* [R3-8] skip phantom/absent slots */
        a = actor_ptr(i);
        if (!a) continue;
        lane = (int)ACTOR_U8(a, ACTOR_SUB_LANE_INDEX);
        if (lane != target_lane) continue;
        span = (int)ACTOR_I16(a, ACTOR_SPAN_RAW);
        diff = (polarity == 0) ? (span - self_span) : (self_span - span);
        if (diff >= -2 && diff <= 6) return 0;   /* occupied */
    }
    return 1;
}

/* ===================================================================
 * [AI UNSTICK 2026-06-26] Racer collision-escape
 *
 * Racers have the faithful LATERAL dodge (find_offset_peer -> steering offset)
 * but NO throttle/brake reaction to a blocking car — confirmed in the original
 * @0x00432D60, where AI throttle is field-position rubber-band only, with no
 * other-actor proximity read. So when the lateral dodge can't separate two
 * cars they wedge and grind at full throttle indefinitely until physics shoves
 * them apart. This gives racers the same jam-escape that traffic
 * (traffic_collision_escape) and cops (cop_ram_stuck) already have: detect no
 * forward progress, then a short steer-away burst that physically peels the car
 * off the obstacle (the codebase deliberately avoids AI reverse — see the cop
 * driver note at the ram path — so we steer-away like traffic instead).
 * Port-only, AI-racer-only, default ON (TD5RE_AI_UNSTICK=0 to disable).
 * =================================================================== */
#define RACER_ESCAPE_BURST     14    /* ~0.5s brake-yield burst (fast grind)      */
#define RACER_ESCAPE_COOLDOWN  30    /* ~1s after a brake burst before re-arming  */
#define RACER_STUCK_SPEED   0x1800   /* |speed| below this == slow / blocked       */
#define RACER_RECOVER_SPEED 0x3000   /* |speed| above this (held) == moving again  */
#define RACER_RECOVER_HOLD     8     /* ticks at speed before releasing the steer  */
#define RACER_STALL_STEER   0x12000  /* full-lock steer-around for a LOW-SPEED block
                                      * (safe at low speed; no high-speed yank)    */
#define RACER_ESCAPE_THROTTLE 0x50   /* throttle once a gap opens (not touching)    */
#define RACER_NUDGE_THROTTLE  0x18   /* barely-feathered throttle WHILE touching the
                                      * obstacle, so we redirect around it instead of
                                      * powering in and burying into the other car  */
#define RACER_CONTACT_GAIN     6     /* load added per tick a V2V contact happened */
#define RACER_CONTACT_DECAY    1     /* load shed per clear tick                   */
#define RACER_CONTACT_CAP     60     /* load ceiling (hysteresis)                  */
#define RACER_GRIND_LOAD      30     /* fast same-speed grind -> brake-yield        */
#define RACER_BLOCK_LOAD      12     /* slow + this much contact -> blocked, steer around */

/* Default ON; TD5RE_AI_UNSTICK=0 disables (restores the faithful no-escape AI). */
static int racer_unstick_enabled(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("TD5RE_AI_UNSTICK"); v = (e && e[0] == '0') ? 0 : 1; }
    return v;
}

/* Pick the side (-1 left / +1 right) with a clear adjacent lane to peel into.
 * Reuses the traffic lane-occupancy scan. When only one side is clear, take it.
 * On a tie (both clear / both blocked) COMMIT to the side already chosen for
 * this episode so the car decisively goes one way around instead of wiggling
 * back and forth; first time, pick a stable side by slot parity. */
static int8_t racer_escape_pick_side(int slot, char *actor) {
    int span_raw = (int)ACTOR_I16(actor, ACTOR_SPAN_RAW);
    int lane     = (int)ACTOR_U8(actor, ACTOR_SUB_LANE_INDEX);
    int lc       = td5_track_get_span_lane_count((int)ACTOR_I16(actor, ACTOR_SPAN_NORMALIZED));
    int can_lo, can_hi, lo_clear, hi_clear, center, interior;
    if (lc < 1) lc = 1;
    /* Only consider sides that stay ON the track: -1 steers toward the lower
     * lane index, +1 toward the higher (matches the traffic escape's sign). */
    can_lo = (lane - 1) >= 0;
    can_hi = (lane + 1) <= (lc - 1);
    if (!can_lo && !can_hi) return 0;                 /* single lane -> nowhere safe to go */
    lo_clear = can_lo && traffic_lane_is_clear(slot, span_raw, lane - 1, 0);
    hi_clear = can_hi && traffic_lane_is_clear(slot, span_raw, lane + 1, 0);
    if (lo_clear && !hi_clear) return (int8_t)-1;
    if (hi_clear && !lo_clear) return (int8_t)+1;
    /* Tie (both clear, or both blocked but on-track): steer toward the track
     * INTERIOR (toward the centre lane), never toward an off-track edge. This is
     * what keeps a blocked car from steering itself out of bounds. */
    center   = (lc - 1) / 2;
    interior = (lane > center) ? -1 : +1;
    if (interior < 0 && !can_lo) interior = +1;
    if (interior > 0 && !can_hi) interior = -1;
    return (int8_t)interior;
}

/* [AI UNSTICK] Physics V2V hook — records that `slot` touched `peer` this tick.
 * Cheap (two writes); range-guarded so physics can call it unconditionally. */
void td5_ai_note_v2v_contact(int slot, int peer) {
    if (slot < 0 || slot >= TD5_MAX_TOTAL_ACTORS) return;
    s_racer_contact_flag[slot] = 1;
    s_racer_contact_peer[slot] = (int8_t)peer;
}

/* Per-tick racer unstick. Runs AFTER td5_ai_update_track_behavior so it can
 * override the throttle/steer the normal racing-line driver just set. Split by
 * SPEED: SLOW + touching a car -> sustained steer-around until moving again
 * (handles being blocked behind a stopped/slow car, or a dead wedge); FAST +
 * grinding a same-speed car -> the car behind brakes briefly to drop back. */
static void racer_collision_escape(int slot) {
    char   *actor;
    int32_t spd, aspd;
    int     contacted, peer;

    if (!racer_unstick_enabled()) return;
    if (slot < 0 || slot >= g_traffic_slot_base) return;   /* AI RACER slots only */
    if (td5_ai_actor_is_cop(slot)) return;                 /* cops run their own driver */
    if (g_actor_broken_down[slot]) return;

    actor = actor_ptr(slot);
    if (!actor) return;

    /* Consume this tick's contact flag (set by physics last tick) into a leaky
     * load accumulator: climbs while repeatedly colliding, bleeds off when clear. */
    contacted = s_racer_contact_flag[slot];
    peer      = s_racer_contact_peer[slot];
    s_racer_contact_flag[slot] = 0;
    if (contacted) {
        s_racer_contact_load[slot] += RACER_CONTACT_GAIN;
        if (s_racer_contact_load[slot] > RACER_CONTACT_CAP)
            s_racer_contact_load[slot] = RACER_CONTACT_CAP;
    } else if (s_racer_contact_load[slot] > 0) {
        s_racer_contact_load[slot] -= RACER_CONTACT_DECAY;
        if (s_racer_contact_load[slot] < 0) s_racer_contact_load[slot] = 0;
    }

    spd  = ACTOR_I32(actor, ACTOR_LONGITUDINAL_SPEED);
    aspd = (spd < 0) ? -spd : spd;
    if (aspd > RACER_STUCK_SPEED) s_racer_has_moved[slot] = 1;

    if (s_racer_escape_cooldown[slot] > 0) s_racer_escape_cooldown[slot]--;

    /* ---- 1. SUSTAINED low-speed steer-around (blocked behind a car / wedge). ----
     * Blocked = has moved this race, is now slow, and is touching a car. At low
     * speed a firm steer is safe (no high-speed yank), and SUSTAINING it (rather
     * than a burst+cooldown that lets the car drift back into the obstacle) is
     * what actually gets it past a stopped/slow car instead of sitting there. */
    if (s_racer_has_moved[slot] && aspd < RACER_STUCK_SPEED &&
        s_racer_contact_load[slot] >= RACER_BLOCK_LOAD) {
        if (!s_racer_stall_active[slot]) {
            int8_t side = racer_escape_pick_side(slot, actor);
            if (side != 0) {              /* only escape if there's a safe on-track way around */
                s_racer_stall_active[slot] = 1;
                s_racer_escape_side[slot]  = side;
                TD5_LOG_I(LOG_TAG,
                    "racer_unstick: slot=%d BLOCKED peer=%d spd=%d -> steer-around side=%d",
                    slot, peer, (int)aspd, (int)side);
            }
        }
        s_racer_recover_ticks[slot] = 0;
    }
    if (s_racer_stall_active[slot]) {
        /* Re-pick the side EVERY tick so it tracks the (edge-aware, interior-
         * biased) safe direction as the car moves — never letting a committed
         * side walk it off the track. side==0 means we've reached an edge / run
         * out of safe room, so abandon the maneuver. */
        int8_t side = racer_escape_pick_side(slot, actor);
        if (side != 0) s_racer_escape_side[slot] = side;
        /* Release when back up to speed for a few ticks (got around), OR as soon
         * as we're no longer touching anything (pulled clear) — the latter stops
         * the car steering in a circle out in open space. */
        if (aspd > RACER_RECOVER_SPEED) s_racer_recover_ticks[slot]++;
        else                            s_racer_recover_ticks[slot] = 0;
        if (side == 0 ||
            s_racer_recover_ticks[slot] >= RACER_RECOVER_HOLD ||
            s_racer_contact_load[slot] == 0) {
            s_racer_stall_active[slot]  = 0;
            s_racer_recover_ticks[slot] = 0;
            s_racer_escape_side[slot]   = 0;
        } else {
            /* Steer toward the open side. While still TOUCHING the obstacle,
             * barely feather the throttle so we redirect AROUND it instead of
             * powering INTO it — driving hard into a stopped car built up deep
             * penetration that snapped the other car away ("teleport"). Once a
             * gap opens (not touching this tick) give it a bit more to slot in. */
            ACTOR_I32(actor, ACTOR_STEERING_CMD)    =
                (int32_t)s_racer_escape_side[slot] * RACER_STALL_STEER;
            ACTOR_U8(actor,  ACTOR_BRAKE_FLAG)      = 0;
            ACTOR_U8(actor,  ACTOR_THROTTLE_STATE)  = 1;
            ACTOR_I16(actor, ACTOR_ENCOUNTER_STEER) =
                (int16_t)(contacted ? RACER_NUDGE_THROTTLE : RACER_ESCAPE_THROTTLE);
            return;
        }
    }

    /* ---- 2. FAST same-speed grind: the car behind brakes briefly to drop back. ----
     * Longitudinal only (no steering at racing speed -> no weird turns). */
    if (s_racer_escape_ticks[slot] > 0) {
        s_racer_escape_ticks[slot]--;
        ACTOR_U8(actor,  ACTOR_BRAKE_FLAG)      = 1;
        ACTOR_U8(actor,  ACTOR_THROTTLE_STATE)  = 1;
        ACTOR_I16(actor, ACTOR_ENCOUNTER_STEER) = (int16_t)0xFF00;   /* brake — drop back */
        return;
    }
    if (s_racer_escape_cooldown[slot] > 0) return;   /* settle before re-arming */

    /* Arm a brake-yield only for a fast, sustained grind against a car keeping
     * pace, and only for the car that's behind (lower span-accum; higher slot on
     * a tie). A slower/stopped peer is handled by section 1 (steer-around), never
     * by braking — so a car never sits and waits behind a stopped car. */
    if (aspd >= RACER_STUCK_SPEED &&
        s_racer_contact_load[slot] >= RACER_GRIND_LOAD &&
        peer >= 0 && peer < TD5_MAX_TOTAL_ACTORS) {
        char *pa = actor_ptr(peer);
        int32_t peer_spd = pa ? ACTOR_I32(pa, ACTOR_LONGITUDINAL_SPEED) : 0;
        int my_acc   = (int)(int16_t)ACTOR_I16(actor, ACTOR_SPAN_ACCUM);
        int peer_acc = pa ? (int)(int16_t)ACTOR_I16(pa, ACTOR_SPAN_ACCUM) : my_acc;
        int gap      = my_acc - peer_acc;
        int behind   = (gap < -1) ? 1 : (gap > 1) ? 0 : (slot > peer);
        if (peer_spd < 0) peer_spd = -peer_spd;
        if (peer_spd >= (aspd >> 1) && behind) {
            s_racer_escape_ticks[slot]    = RACER_ESCAPE_BURST;
            s_racer_escape_cooldown[slot] = RACER_ESCAPE_BURST + RACER_ESCAPE_COOLDOWN;
            TD5_LOG_I(LOG_TAG,
                "racer_unstick: slot=%d GRIND peer=%d load=%d spd=%d -> brake-yield",
                slot, peer, (int)s_racer_contact_load[slot], (int)aspd);
        }
        s_racer_contact_load[slot] = 0;
    }
}

/* (4) Lookahead: when a peer sits in our lane just ahead, pick a clear adjacent
 * lane to move into (preferring the interior side and never a slow lane), set
 * the 1-tick-latency lane bias consumed by traffic_smart_choose_lane next tick,
 * and return 1 to tell the caller to EASE the hard brake. Returns 0 (and clears
 * the bias) when disabled, single-lane, no same-lane peer, or no clear lane —
 * in which case the faithful TTC brake stands. */
static int traffic_smart_react_to_peer(int slot, int self_span, int self_sub_lane,
                                        int lane_count, int peer_slot, int polarity) {
    char *peer;
    int peer_lane, dir_first, dirs[2], k;
    if (!g_td5.ini.traffic_smart || !g_td5.ini.traffic_lookahead ||
        peer_slot < 0 || peer_slot >= TD5_MAX_TOTAL_ACTORS || peer_slot == slot) {
        s_traffic_lane_bias[slot] = 0;
        return 0;
    }
    peer = actor_ptr(peer_slot);
    if (!peer) { s_traffic_lane_bias[slot] = 0; return 0; }
    s_smart_stat.react_calls++;
    if (lane_count <= 1) {
        /* Single-lane span: nowhere to go around — the faithful brake is the
         * correct response. */
        s_smart_stat.blocked_single_lane++;
        s_traffic_lane_bias[slot] = 0;
        return 0;
    }
    peer_lane = (int)ACTOR_U8(peer, ACTOR_SUB_LANE_INDEX);
    if (peer_lane != self_sub_lane) {
        s_smart_stat.ease_diff_lane++;
        /* The nearest peer is in a DIFFERENT lane on a multi-lane span — not an
         * actual collision course. The faithful TTC brakes purely on span
         * distance (lane-agnostic) and would needlessly stop us behind a car in
         * the next lane. Ease (keep rolling, no lane change) so parallel-lane
         * traffic flows past instead of stacking up. */
        s_traffic_lane_bias[slot] = 0;
        if ((g_ai_frame_counter % 30u) == 0u)
            TD5_LOG_I(LOG_TAG,
                      "traffic_smart_avoid: slot=%d peer=%d in lane %d (self %d) "
                      "-> ease (different lane, no brake)",
                      slot, peer_slot, peer_lane, self_sub_lane);
        return 1;
    }

    dir_first = (self_sub_lane < lane_count / 2) ? +1 : -1;  /* prefer interior */
    dirs[0] = dir_first;
    dirs[1] = -dir_first;
    for (k = 0; k < 2; k++) {
        int cand = self_sub_lane + dirs[k];
        if (cand < 0 || cand >= lane_count) continue;
        if (trf_dyn_lane_change_blocked(slot, lane_count, cand))
            continue;   /* [dynamic-traffic] never swerve into an opposite-direction lane */
        if (g_td5.ini.traffic_avoid_slow_lane &&
            td5_track_surface_is_slow(td5_track_get_span_lane_surface(self_span, cand)))
            continue;
        if (traffic_lane_is_clear(slot, self_span, cand, polarity)) {
            s_traffic_lane_bias[slot] = (int8_t)dirs[k];
            s_smart_stat.ease_lane_change++;
            if ((g_ai_frame_counter % 30u) == 0u)
                TD5_LOG_I(LOG_TAG,
                          "traffic_smart_avoid: slot=%d peer=%d lane %d->%d (ease brake)",
                          slot, peer_slot, self_sub_lane, cand);
            return 1;
        }
    }
    s_smart_stat.no_clear_lane++;
    s_traffic_lane_bias[slot] = 0;
    return 0;
}

/* (2)+(3) Choose the target sub-lane: apply the situational lane bias (set last
 * tick by react_to_peer) then avoid-slow-lane. Returns base_sub_lane unchanged
 * when disabled or single-lane. Result is clamped to [0, lane_count-1]. */
static int traffic_smart_choose_lane(int slot, int target_span,
                                     int lane_count, int base_sub_lane) {
    int lane;
    if (!g_td5.ini.traffic_smart || lane_count <= 1)
        return base_sub_lane;
    lane = base_sub_lane;
    if (lane < 0) lane = 0;
    if (lane >= lane_count) lane = lane_count - 1;

    /* situational lane change (1-tick latency) */
    if (g_td5.ini.traffic_lookahead && s_traffic_lane_bias[slot] != 0) {
        int cand = lane + s_traffic_lane_bias[slot];
        if (cand >= 0 && cand < lane_count &&
            !trf_dyn_lane_change_blocked(slot, lane_count, cand))
            lane = cand;
    }

    /* [R3-10] Anti-pileup lane commit: while a recent jam-escape's lane bias is
     * live, step toward the cleared side so the car leaves the lane it kept
     * re-jamming in. Read directly (not via s_traffic_lane_bias, which the route
     * plan clears every tick). Deterministic (replicated escape state). */
    if (s_traffic_escape_lane_ttl[slot] > 0 && s_traffic_escape_lane[slot] != 0) {
        int cand = lane + (int)s_traffic_escape_lane[slot];
        if (cand >= 0 && cand < lane_count &&
            !trf_dyn_lane_change_blocked(slot, lane_count, cand))
            lane = cand;
    }

    /* avoid a slow / off-road lane: step to the nearest faster lane (that
     * also matches the car's drive direction when Dynamic=1) */
    if (g_td5.ini.traffic_avoid_slow_lane &&
        td5_track_surface_is_slow(td5_track_get_span_lane_surface(target_span, lane))) {
        int toward_centre = (lane < lane_count / 2) ? +1 : -1;
        int picked = lane, d;
        for (d = 1; d < lane_count; d++) {
            int c1 = lane + toward_centre * d;
            int c2 = lane - toward_centre * d;
            if (c1 >= 0 && c1 < lane_count &&
                !trf_dyn_lane_change_blocked(slot, lane_count, c1) &&
                !td5_track_surface_is_slow(td5_track_get_span_lane_surface(target_span, c1))) {
                picked = c1; break;
            }
            if (c2 >= 0 && c2 < lane_count &&
                !trf_dyn_lane_change_blocked(slot, lane_count, c2) &&
                !td5_track_surface_is_slow(td5_track_get_span_lane_surface(target_span, c2))) {
                picked = c2; break;
            }
        }
        if (picked != lane) s_smart_stat.slow_lane_change++;
        lane = picked;
    }

    if (lane != base_sub_lane && (g_ai_frame_counter % 60u) == 0u)
        TD5_LOG_I(LOG_TAG, "traffic_smart_lane: slot=%d base=%d -> %d (lanes=%d span=%d)",
                  slot, base_sub_lane, lane, lane_count, target_span);
    return lane;
}

/* (2) WallAvoid: blend an edge-lane car's lateral target (24.8 FP) toward the
 * interior neighbour lane so it stops scraping the rail. Interior lanes are
 * left exactly centred (preserves lane separation). No-op when disabled or
 * single-lane. */
static void traffic_smart_wall_nudge(int target_span, int target_sub_lane,
                                     int lane_count, int32_t *tx,
                                     int32_t *ty, int32_t *tz) {
    int bias, inward, ix = 0, iy = 0, iz = 0;
    if (!g_td5.ini.traffic_smart || !g_td5.ini.traffic_wall_avoid || lane_count <= 1)
        return;
    bias = g_td5.ini.traffic_wall_avoid_bias;
    if (bias <= 0) return;
    if (target_sub_lane <= 0)
        inward = 1;
    else if (target_sub_lane >= lane_count - 1)
        inward = lane_count - 2;
    else
        return;   /* interior lane already away from both rails */
    if (!td5_track_get_span_lane_world(target_span, inward, &ix, &iy, &iz))
        return;
    {
        int32_t dx_move = (int32_t)(((int64_t)(ix - *tx) * bias) >> 8);
        int32_t dz_move = (int32_t)(((int64_t)(iz - *tz) * bias) >> 8);
        *tx += dx_move;
        *ty += (int32_t)(((int64_t)(iy - *ty) * bias) >> 8);
        *tz += dz_move;
        s_smart_stat.wall_nudges++;
        /* Rate-limited so the before/after trace can count edge-lane nudges. */
        if ((g_ai_frame_counter % 60u) == 0u)
            TD5_LOG_I(LOG_TAG,
                      "traffic_smart_wallnudge: span=%d edge_lane=%d->interior=%d "
                      "moved(dx=%d dz=%d) [24.8 FP]",
                      target_span, target_sub_lane, inward, dx_move, dz_move);
    }
}

/* ========================================================================
 * [PORT ENHANCEMENT 2026-06-11] Dynamic (GTA-style) ambient traffic
 *
 * Replaces the TRAFFIC.BUS fixed-spawn queue with a distance-driven
 * spawn/despawn state machine, gated by [Traffic] Dynamic (default 1).
 * Dynamic=0 leaves the byte-faithful queue init (InitializeTrafficActors-
 * FromQueue @ 0x00435940) + recycle (RecycleTrafficActorFromQueue @
 * 0x004353B0) paths completely untouched.
 *
 * Per traffic slot state machine:
 *   INACTIVE  — parked at its last pose; skipped by AI route plan, traffic
 *               physics, V2V broadphase, render (body/shadow/wheels/brakes),
 *               minimap dot and engine audio.
 *   FADE_IN   — just (re)placed on the road; render alpha ramps 0→255 over
 *               [Traffic] FadeTicks. Fully simulated from tick 0.
 *   ACTIVE    — normal traffic.
 *   FADE_OUT  — every local player is > DespawnDistance spans away (or the
 *               car is recovery-latched out of sight); alpha ramps 255→0,
 *               then the slot parks (INACTIVE).
 *
 * Spawning: every SpawnPeriod-ish ticks (volume-scaled + jittered) one
 * INACTIVE slot is placed at a random span [SpawnAheadMin..SpawnAheadMax]
 * ahead of the LEAD local player, on a random CLEAR, NON-SLOW lane
 * (td5_track_surface_is_slow — dirt/gravel/alternate-surface shoulders are
 * never picked). Direction polarity (oncoming bit, +0x80000 heading like
 * the queue's flags bit0 [CONFIRMED @ 0x00435786]) is rolled against the
 * track's own TRAFFIC.BUS oncoming ratio so per-track direction character
 * is preserved. Placement reuses the exact recycle placement chain
 * (selector 0 / main ring only).
 *
 * Multiplayer: despawn requires EVERY local player to be out of range and
 * spawn validates the window against ALL players — unlike the original
 * recycle which only ever reads slot 0's span (word @ 0x004AB18A
 * [CONFIRMED]). Netplay is unaffected (traffic is forced off there).
 *
 * Police: traffic slot 9 is the speeding-pursuit cop donor
 * (UpdateSpecialTrafficEncounter @ 0x00434DA0). The spawner PREFERS slot 9
 * when picking a slot to (re)spawn so the cop-capable car is usually on the
 * road, and a slot-9 car is never despawned while the encounter handle is
 * live (mirrors the recycle's slot-9 guard @ 0x0043545B).
 *
 * Determinism: a private LCG seeded from the track index — deliberately NOT
 * the CRT rand(), whose sequence is consumed at render rate by the audio
 * mix (td5_sound.c traffic pitch) and must stay untouched for the fixed-
 * seed A/B trace harness.
 * ======================================================================== */

enum {
    TRF_DYN_INACTIVE = 0,
    TRF_DYN_FADE_IN  = 1,
    TRF_DYN_ACTIVE   = 2,
    TRF_DYN_FADE_OUT = 3
};

static uint8_t  s_trf_dyn_state[TD5_MAX_TOTAL_ACTORS];
static int16_t  s_trf_dyn_alpha[TD5_MAX_TOTAL_ACTORS];   /* 0..255 draw alpha */
static int      s_trf_dyn_cooldown;                       /* ticks to next spawn attempt */
static uint32_t s_trf_dyn_rng;                            /* private LCG state */
static int      s_trf_dyn_oncoming_pct;                   /* 0..100 from TRAFFIC.BUS mix (diagnostic) */
static int      s_trf_dyn_seeded;                         /* race_init ran for this race */

/* ---- [PER-VIEWPORT TRAFFIC 2026-06-22] ----------------------------------------
 * Split-screen TIME TRIAL: each viewport gets its OWN traffic partition (a disjoint
 * sub-range of the traffic slots), spawned with an IDENTICAL RNG seed but scoped to
 * its own player (s_trf_scope_slot), so the sets land in matching places yet are
 * simulated / collided / rendered independently — one player perturbing a traffic
 * car never shows in another player's pane. INERT unless s_trf_per_vp is set in
 * race_init (knob on + mp mode == TIME_TRIAL + viewport_count > 1). When inert,
 * every path below is bypassed and the original shared spawner runs unchanged. */
int             g_traffic_slot_owner_vp[TD5_MAX_TOTAL_ACTORS]; /* viewport owner; -1 = shared */
static int      s_trf_per_vp     = 0;     /* per-viewport traffic active this race      */
static int      s_trf_vp_count   = 1;     /* number of viewports partitioned across     */
static int      s_trf_vp_k       = TD5_MAX_TRAFFIC_SLOTS;  /* traffic slots per viewport */
static uint32_t s_trf_dyn_rng_vp[TD5_MAX_VIEWPORTS];      /* per-viewport spawn RNG      */
static int      s_trf_dyn_cooldown_vp[TD5_MAX_VIEWPORTS]; /* per-viewport spawn cooldown */
/* The partition currently being spawned (>=0) — the existing-traffic density gate
 * counts ONLY this partition's cars so each partition's placement is identical
 * (a partition must not "see" another partition's twins when deciding clumping).
 * -1 = count all traffic (default / non-per-vp). */
static int      s_trf_spawn_partition = -1;

static int trf_per_viewport_knob(void)
{
    static int s = -1;
    if (s < 0) {
        const char *e = getenv("TD5RE_TT_PER_VIEWPORT_TRAFFIC");
        s = (e && e[0] == '0') ? 0 : 1;   /* default ON */
    }
    return s;
}

/* Viewport index whose player occupies racer `slot` (-1 if none). Split-screen is
 * an identity map (vp == slot) but resolve it via the game table to be safe. */
static int trf_viewport_of_slot(int slot)
{
    int vc = s_trf_vp_count;   /* the partition count (== num_human_players) */
    if (vc > TD5_MAX_VIEWPORTS) vc = TD5_MAX_VIEWPORTS;
    for (int v = 0; v < vc; v++)
        if (td5_game_get_player_slot(v) == slot) return v;
    return -1;
}

int td5_ai_traffic_per_viewport_active(void) { return s_trf_per_vp; }

int td5_ai_traffic_slot_owner_vp(int slot)
{
    if (!s_trf_per_vp || slot < 0 || slot >= TD5_MAX_TOTAL_ACTORS) return -1;
    return g_traffic_slot_owner_vp[slot];
}

/* 1 if a collision between these two slots must be SUPPRESSED for per-viewport
 * isolation: a traffic car only interacts with its owning viewport's player, and
 * two traffic cars from DIFFERENT partitions (which sit at matching positions)
 * must not collide with each other. Returns 0 (allow) whenever per-vp is off. */
int td5_ai_traffic_pair_blocked(int slot_a, int slot_b)
{
    int base, a_traf, b_traf, owner_a, owner_b, vp;
    if (!s_trf_per_vp) return 0;
    base    = g_traffic_slot_base;
    a_traf  = (slot_a >= base);
    b_traf  = (slot_b >= base);
    owner_a = (a_traf && slot_a >= 0 && slot_a < TD5_MAX_TOTAL_ACTORS) ? g_traffic_slot_owner_vp[slot_a] : -1;
    owner_b = (b_traf && slot_b >= 0 && slot_b < TD5_MAX_TOTAL_ACTORS) ? g_traffic_slot_owner_vp[slot_b] : -1;
    if (a_traf && b_traf)
        return (owner_a >= 0 && owner_b >= 0 && owner_a != owner_b);
    if (a_traf) { vp = trf_viewport_of_slot(slot_b); return (owner_a >= 0 && vp >= 0 && vp != owner_a); }
    if (b_traf) { vp = trf_viewport_of_slot(slot_a); return (owner_b >= 0 && vp >= 0 && vp != owner_b); }
    return 0;
}

/* Decide per-viewport activation + fill the slot->owner partition map. Called from
 * race_init after g_traffic_slot_base / viewport_count are known. */
static void trf_per_viewport_setup(void)
{
    int base = g_traffic_slot_base, v, slot, vc;
    for (slot = 0; slot < TD5_MAX_TOTAL_ACTORS; slot++)
        g_traffic_slot_owner_vp[slot] = -1;
    s_trf_per_vp   = 0;
    s_trf_vp_count = 1;
    s_trf_vp_k     = TD5_MAX_TRAFFIC_SLOTS;
    /* Partition by num_human_players, NOT viewport_count: traffic init runs at
     * InitRace step ~4 but the viewport layout (viewport_count) isn't set until
     * step ~17, so viewport_count is still 1 here. In split-screen each human is
     * one viewport with an identity slot map, so num_human_players is the right
     * (and already-set) partition count. */
    vc = g_td5.num_human_players;
    if (!trf_per_viewport_knob()) return;
    if (g_td5.mp_mode_config.mode != TD5_MP_MODE_TIME_TRIAL) return;
    if (vc <= 1) return;
    if (vc > TD5_MAX_VIEWPORTS) vc = TD5_MAX_VIEWPORTS;
    s_trf_per_vp   = 1;
    s_trf_vp_count = vc;
    s_trf_vp_k     = TD5_MAX_TRAFFIC_SLOTS / vc;
    if (s_trf_vp_k < 1) s_trf_vp_k = 1;
    for (v = 0; v < vc; v++) {
        int pv_lo = base + v * s_trf_vp_k;
        int pv_hi = pv_lo + s_trf_vp_k;
        if (v == vc - 1) pv_hi = base + vc * s_trf_vp_k;   /* last gets the slack slots */
        for (slot = pv_lo; slot < pv_hi && slot < TD5_MAX_TOTAL_ACTORS; slot++)
            g_traffic_slot_owner_vp[slot] = v;
        s_trf_dyn_rng_vp[v]      = 0x54443552u ^ ((uint32_t)g_td5.track_index * 2654435761u);
        s_trf_dyn_cooldown_vp[v] = 0;
    }
    TD5_LOG_I(LOG_TAG, "traffic_per_viewport: ON (time trial split-screen) viewports=%d "
              "slots/vp=%d base=%d", s_trf_vp_count, s_trf_vp_k, base);
}

/* Per-track lane→direction map learned from the authored TRAFFIC.BUS records:
 * each 4-byte record couples (span, lane, polarity), and the span's strip
 * lane-count nibble keys which road layout the lane index refers to. Indexed
 * [lane_count][lane]: -1 = no authored data, 0 = forward, 1 = oncoming
 * (majority vote). s_trf_dyn_oncoming_left summarizes which HALF of the road
 * carries oncoming traffic on this track (fallback for unseen combos and for
 * tracks without a TRAFFIC.BUS, e.g. TD6 conversions). */
#define TRF_DYN_MAX_LANES 15
static int8_t   s_trf_dyn_lane_dir[TRF_DYN_MAX_LANES + 1][TRF_DYN_MAX_LANES];
static int      s_trf_dyn_oncoming_left;                  /* 1 = oncoming on left half */

/* Direction a freshly spawned car should drive in `lane` of a
 * `lane_count`-lane span: authored data first, side-half heuristic second.
 * Single-lane spans always drive forward. */
/* [TRAFFIC BATTLE 2026-06-28] Force ALL traffic to drive ONCOMING (toward the
 * players) — head-on demolition fodder. Default ON in battle mode; the
 * TD5RE_TRAFFIC_ONCOMING knob overrides either way (1 = on, 0 = off) so it can be
 * forced on/off in any mode for testing. Reads process config + replicated mode,
 * so it is lockstep-deterministic. */
static int trf_force_oncoming(void)
{
    const char *e = getenv("TD5RE_TRAFFIC_ONCOMING");
    if (e && e[0]) return atoi(e) != 0;
    return td5_game_battle_mode_active();
}

static int trf_dyn_lane_direction(int lane_count, int lane)
{
    /* [TRAFFIC BATTLE] Every lane heads oncoming — takes precedence over the
     * one-way (all-forward) default so battle traffic comes at the players. */
    if (trf_force_oncoming())
        return 1;
    /* [traffic one-way] Every lane heads down-track — no oncoming lanes. This is
     * the single chokepoint for the dynamic spawner: it sets both the spawn
     * polarity and trf_dyn_lane_change_blocked()'s direction test. */
    if (trf_oneway_traffic())
        return 0;
    if (lane_count >= 1 && lane_count <= TRF_DYN_MAX_LANES &&
        lane >= 0 && lane < lane_count &&
        s_trf_dyn_lane_dir[lane_count][lane] >= 0)
        return (int)s_trf_dyn_lane_dir[lane_count][lane];
    if (lane_count <= 1)
        return 0;
    {
        int left_half = (lane * 2 < lane_count);
        return s_trf_dyn_oncoming_left ? left_half : !left_half;
    }
}

/* Span of the 1ST-PLACE RACE CAR — humans AND AI racers (user rule: traffic
 * appears ahead of whoever leads the race; a last-place human otherwise sees
 * traffic materializing around themselves on the minimap). Skips decoration/
 * absent slots (state 3) and slots with no car bound. Falls back to the lead
 * human when no racer qualifies (e.g. degenerate solo states). Note: on
 * circuits the comparison is ring-relative like every other span comparison
 * in this module, so right at the lap seam the anchor may briefly trail. */
static int trf_dyn_race_span_lead(void)
{
    int n = g_traffic_slot_base;
    int best = -0x7FFFFFFF;
    if (s_trf_scope_slot >= 0)   /* per-viewport: anchor on this viewport's player */
        return (int)(int16_t)ACTOR_I16(actor_ptr(s_trf_scope_slot), ACTOR_SPAN_NORMALIZED);
    if (n > g_active_actor_count) n = g_active_actor_count;
    if (n > TD5_MAX_TOTAL_ACTORS) n = TD5_MAX_TOTAL_ACTORS;
    for (int s = 0; s < n; s++) {
        char *a = actor_ptr(s);
        void *cdef = NULL;
        int sp;
        if (!a) continue;
        if (g_slot_state[s] == 3) continue;          /* decoration / absent */
        memcpy(&cdef, a + 0x1B8, sizeof(cdef));      /* car_definition_ptr */
        if (!cdef) continue;
        sp = (int)(int16_t)ACTOR_I16(a, ACTOR_SPAN_NORMALIZED);
        if (sp > best) best = sp;
    }
    return (best == -0x7FFFFFFF) ? ai_player_span_lead() : best;
}

/* 1 when moving `slot` into `cand_lane` would put it in a lane of the
 * OPPOSITE drive direction. Inert (0) in faithful mode, where lanes carry
 * no direction and the SmartAI lane-changer keeps its original freedom. */
static int trf_dyn_lane_change_blocked(int slot, int lane_count, int cand_lane)
{
    int32_t *rs;
    if (!td5_ai_traffic_dynamic_active()) return 0;
    if (slot < 0 || slot >= TD5_MAX_TOTAL_ACTORS) return 0;
    rs = route_state(slot);
    if (!rs) return 0;
    return trf_dyn_lane_direction(lane_count, cand_lane) !=
           (rs[RS_ROUTE_DIRECTION_POLARITY] & 1);
}

static uint32_t trf_dyn_rand(void)
{
    /* Numerical Recipes LCG; top bits are the usable ones. */
    s_trf_dyn_rng = s_trf_dyn_rng * 1664525u + 1013904223u;
    return s_trf_dyn_rng >> 8;
}

int td5_ai_traffic_dynamic_active(void)
{
    return g_td5.ini.traffic_dynamic && g_td5.traffic_enabled && s_trf_dyn_seeded;
}

int td5_ai_traffic_dynamic_parked(int slot)
{
    if (!td5_ai_traffic_dynamic_active()) return 0;
    if (slot < g_traffic_slot_base ||
        slot >= g_traffic_slot_base + TD5_MAX_TRAFFIC_SLOTS ||
        slot >= TD5_MAX_TOTAL_ACTORS) return 0;
    return s_trf_dyn_state[slot] == TRF_DYN_INACTIVE;
}

int td5_ai_traffic_get_draw_alpha(int slot)
{
    if (!td5_ai_traffic_dynamic_active()) return 255;
    if (slot < g_traffic_slot_base ||
        slot >= g_traffic_slot_base + TD5_MAX_TRAFFIC_SLOTS ||
        slot >= TD5_MAX_TOTAL_ACTORS) return 255;
    return (int)s_trf_dyn_alpha[slot];
}

/* [task#12 2026-06-15] A/B knob for the consistent-density fixes (proximity
 * recycle + per-volume retune + VERY HIGH tier). TD5RE_TRAFFIC_DENSITY=0 restores
 * the prior behaviour (corridor-only despawn, old caps/periods); default on.
 * Logged once on first read. Independent of TD5RE_TRAFFIC_BRANCHES / _SPAWN_DIST
 * (those keep their own knobs and stay in effect). */
static int trf_dyn_density_enabled(void)
{
    static int s = -1;
    if (s < 0) {
        const char *e = getenv("TD5RE_TRAFFIC_DENSITY");
        s = (e && e[0] == '0') ? 0 : 1;
        TD5_LOG_I(LOG_TAG, "traffic_density knob: TD5RE_TRAFFIC_DENSITY=%d "
                  "(proximity recycle + retune + VERY HIGH %s)",
                  s, s ? "ON" : "OFF");
    }
    return s;
}

/* Resolved traffic volume 0..4 (Off/Low/Medium/High/Very-High). Code paths that
 * only set the legacy boolean (cup save restore, forced-on game types) leave
 * traffic_volume at 0 — treat enabled+no-volume as High (the classic 6-car
 * density). The frontend selector currently masks the option to 0..3; value 4
 * (VERY HIGH) is wired here and in main.c's clamp so the frontend can be extended
 * to emit it without further engine changes. */
static int trf_dyn_volume(void)
{
    int v = g_td5.traffic_volume;
    if (v <= 0) v = g_td5.traffic_enabled ? 3 : 0;
    if (v > 4) v = 4;
    return v;
}

/* Concurrency cap from the track-select Traffic volume row.
 *   Off=0  Low=2  Medium=4  High=6  Very-High=16
 * The hard ceiling is TD5_MAX_TRAFFIC_SLOTS (16 — see td5_types.h). HIGH keeps the
 * faithful 6; VERY HIGH fills all 16 traffic slots (the extra cars reuse each
 * track's 6 car models + 6 skin pages — normal for traffic) and packs them near
 * the player via the tighter spawn window / faster respawn below. */
static int trf_dyn_cap(void)
{
    static const int k_cap[5] = { 0, 2, 4, 6, 16 };
    int v = trf_dyn_volume();
    int cap;
    if (!trf_dyn_density_enabled()) {
        /* legacy 0..3 caps */
        static const int k_old[4] = { 0, 2, 4, 6 };
        int ov = v > 3 ? 3 : v;
        cap = k_old[ov];
    } else {
        cap = k_cap[v];
    }
    return (cap > TD5_MAX_TRAFFIC_SLOTS) ? TD5_MAX_TRAFFIC_SLOTS : cap;
}

/* Volume also paces the spawner: Low is sparse, High busy, Very-High relentless.
 * The period is the gap (ticks @30Hz) between spawn attempts when below the cap;
 * a shorter period refills emptied slots faster, which (with the proximity
 * recycle below) is the main lever that keeps the road consistently full. */
static int trf_dyn_spawn_period(void)
{
    int p = g_td5.ini.traffic_dyn_period;
    int v = trf_dyn_volume();
    if (!trf_dyn_density_enabled()) {
        /* legacy pacing */
        if (v == 1) p *= 2;
        else if (v >= 3) p = (p * 2) / 3;
    } else {
        /* Retuned so the tiers are clearly distinct:
         *   Low      ×2.0   (sparse — a car every few seconds)
         *   Medium   ×1.0   (stock cadence)
         *   High     ×0.5   (busy — refills fast)
         *   VeryHigh ×0.3   (relentless — slots refill almost immediately) */
        if (v == 1)      p = (p * 2);
        else if (v == 2) p = p;
        else if (v == 3) p = (p * 1) / 2;
        else if (v >= 4) p = (p * 3) / 10;
    }
    if (p < 4) p = 4;
    /* +/-50% jitter so spawns don't metronome. */
    return p / 2 + (int)(trf_dyn_rand() % (uint32_t)p);
}

/* [PER-VIEWPORT TRAFFIC] Per-viewport on-road cap: a normal per-player density,
 * bounded by the partition size so it never overflows a viewport's slot range. */
static int trf_per_viewport_cap(void)
{
    int c = trf_dyn_cap();
    if (c > s_trf_vp_k) c = s_trf_vp_k;
    if (c < 1) c = 1;
    return c;
}

/* Wrap-aware |span distance| from `span_norm` to the NEAREST local player.
 * Wrapping only applies on circuits (the ring); point-to-point distances are
 * plain differences on the normalized span axis. */
static int trf_dyn_min_player_dist(int span_norm)
{
    int humans = g_td5.num_human_players;
    int ring = td5_track_get_ring_length();
    int is_circuit = (g_td5.track_type == TD5_TRACK_CIRCUIT);
    int best = 0x7FFFFFFF;
    int s_lo = 0;
    if (humans < 1) humans = 1;
    if (humans > g_traffic_slot_base) humans = g_traffic_slot_base;
    if (s_trf_scope_slot >= 0) {     /* per-viewport: only this viewport's player */
        s_lo = s_trf_scope_slot;
        humans = s_trf_scope_slot + 1;
    }
    for (int s = s_lo; s < humans; s++) {
        int ps = (int)(int16_t)ACTOR_I16(actor_ptr(s), ACTOR_SPAN_NORMALIZED);
        int d = span_norm - ps;
        if (is_circuit && ring > 0) {
            int half = ring / 2;
            while (d > half)  d -= ring;
            while (d < -half) d += ring;
        }
        if (d < 0) d = -d;
        if (d < best) best = d;
    }
    return best;
}

/* Wrap-aware |span distance| between two normalized spans (circuit ring-aware). */
static int trf_dyn_span_dist(int a, int b)
{
    int ring = td5_track_get_ring_length();
    int is_circuit = (g_td5.track_type == TD5_TRACK_CIRCUIT);
    int d = a - b;
    if (is_circuit && ring > 0) {
        int half = ring / 2;
        while (d > half)  d -= ring;
        while (d < -half) d += ring;
    }
    return d < 0 ? -d : d;
}

/* [item#10 2026-06-15] Count ACTIVE/FADING traffic cars within `radius` spans of
 * `player_span`. Branch-corridor cars (span >= ring) are normalized to their
 * parallel main span first so they count toward whichever player is near that
 * stretch of road. Used to find the human who currently has the least traffic. */
static int trf_dyn_count_traffic_near(int player_span, int radius)
{
    int t_base = g_traffic_slot_base;
    int t_end  = g_active_actor_count;
    int ring   = td5_track_get_ring_length();
    int n = 0;
    if (t_end > t_base + TD5_MAX_TRAFFIC_SLOTS) t_end = t_base + TD5_MAX_TRAFFIC_SLOTS;
    if (t_end > TD5_MAX_TOTAL_ACTORS) t_end = TD5_MAX_TOTAL_ACTORS;
    for (int slot = t_base; slot < t_end; slot++) {
        int sp;
        /* [PER-VIEWPORT TRAFFIC] count only the partition being spawned, so each
         * partition's clump gate behaves identically (ignores other partitions'
         * twins sitting at the same spans). */
        if (s_trf_spawn_partition >= 0 &&
            g_traffic_slot_owner_vp[slot] != s_trf_spawn_partition)
            continue;
        if (s_trf_dyn_state[slot] != TRF_DYN_ACTIVE &&
            s_trf_dyn_state[slot] != TRF_DYN_FADE_IN)
            continue;
        sp = (int)(int16_t)ACTOR_I16(actor_ptr(slot), ACTOR_SPAN_NORMALIZED);
        if (ring > 0 && sp >= ring) {
            int m = td5_track_branch_to_main_span(sp);   /* fold branch -> main */
            if (m >= 0) sp = m;
        }
        if (trf_dyn_span_dist(sp, player_span) <= radius) n++;
    }
    return n;
}

/* [proximity gate 2026-06-18] Stop traffic from spawning in a knot. A fresh
 * spawn is rejected when the stretch of road around the candidate span already
 * holds >= cluster_max ACTIVE/FADING cars within +/- cluster_radius spans (all
 * lanes; branch-corridor cars fold to their parallel main span via
 * trf_dyn_count_traffic_near). This complements the per-lane clearance
 * (traffic_lane_is_clear), which only blocks the SAME lane within a few spans
 * and so let cars pile up across adjacent lanes / nearby spans (user: London
 * reverse ~span 223 spawned a clump). Both knobs are tunable; MAX=0 disables.
 *   TD5RE_TRAFFIC_CLUSTER_SPANS  radius in spans (default 8 = the clearance reach)
 *   TD5RE_TRAFFIC_CLUSTER_MAX    cars allowed in that window before rejecting
 *                                (default auto: 2, or 4 on VERY HIGH so dense
 *                                 modes still fill; 0 = gate off) */
static int trf_dyn_cluster_radius(void)
{
    static int r = -1;
    if (r < 0) {
        const char *e = getenv("TD5RE_TRAFFIC_CLUSTER_SPANS");
        r = e ? atoi(e) : 8;
        if (r < 1) r = 1;
    }
    return r;
}

static int trf_dyn_cluster_max(void)
{
    static int cached = -2;   /* -2 = unread; -1 = auto; >=0 = explicit (0 = off) */
    if (cached == -2) {
        const char *e = getenv("TD5RE_TRAFFIC_CLUSTER_MAX");
        cached = (e && e[0]) ? atoi(e) : -1;
        TD5_LOG_I(LOG_TAG, "traffic_cluster gate: max=%s radius=%d "
                  "(TD5RE_TRAFFIC_CLUSTER_MAX/_SPANS; 0 = off)",
                  (e && e[0]) ? e : "auto", trf_dyn_cluster_radius());
    }
    if (cached >= 0) return cached;
    return (trf_dyn_volume() >= 4) ? 4 : 2;   /* VERY HIGH packs tighter */
}

/* [item#10 2026-06-15] Live-spawn anchor for the consistent-density goal. In a
 * multi-human (split-screen) race the players can spread far apart; anchoring all
 * spawns on the FRONT-MOST human (ai_player_span_lead) leaves the trailing
 * player(s) with little/no nearby traffic. This returns the span of the human who
 * currently has the FEWEST traffic cars within `radius`, tie-breaking toward the
 * TRAILING (lowest-span) player, so fresh spawns are steered to whoever is
 * starved and every human ends up with a comparable amount of traffic. With a
 * single human this is just that human's span (== ai_player_span_lead), so
 * single-player behaviour is unchanged. Gated by the caller on
 * TD5RE_TRAFFIC_DENSITY + num_human_players > 1. */
static int trf_dyn_starved_player_span(int radius)
{
    int humans = g_td5.num_human_players;
    int best_span;
    int best_cnt;
    if (s_trf_scope_slot >= 0)   /* per-viewport: only this viewport's player */
        return (int)(int16_t)ACTOR_I16(actor_ptr(s_trf_scope_slot), ACTOR_SPAN_NORMALIZED);
    if (humans < 1) humans = 1;
    if (humans > g_traffic_slot_base) humans = g_traffic_slot_base;
    best_span = (int)(int16_t)ACTOR_I16(actor_ptr(0), ACTOR_SPAN_NORMALIZED);
    best_cnt  = trf_dyn_count_traffic_near(best_span, radius);
    for (int s = 1; s < humans; s++) {
        int sp  = (int)(int16_t)ACTOR_I16(actor_ptr(s), ACTOR_SPAN_NORMALIZED);
        int cnt = trf_dyn_count_traffic_near(sp, radius);
        /* Strictly fewer wins; on a tie prefer the TRAILING (lower-span) player so
         * the player who is behind is favoured for fresh traffic. */
        if (cnt < best_cnt || (cnt == best_cnt && sp < best_span)) {
            best_cnt  = cnt;
            best_span = sp;
        }
    }
    return best_span;
}

/* [task#8 2026-06-14] A/B knob for spawning ambient traffic on branch corridors
 * (both sides of a fork), not just the main/right route. TD5RE_TRAFFIC_BRANCHES=0
 * restores the prior main-ring-only spawn (TD6 branches stay empty). Default on.
 * Logged once on first read. */
static int trf_dyn_branches_enabled(void)
{
    static int s = -1;
    if (s < 0) {
        const char *e = getenv("TD5RE_TRAFFIC_BRANCHES");
        /* Default ON: branches are connected drivable roads with route tables, so
         * traffic spawns on and drives them like the main ring. =0 forces all
         * traffic onto the main ring only. */
        s = (e && e[0] == '0') ? 0 : 1;
        TD5_LOG_I(LOG_TAG, "traffic_branches knob: TD5RE_TRAFFIC_BRANCHES=%d "
                  "(spawn on branch corridors %s)", s, s ? "ON" : "OFF");
    }
    return s;
}

/* Branch traffic spawning is allowed when the knob is on AND the track has
 * branch corridors (jump table populated). Works for BOTH TD6 city tracks and
 * TD5 tracks (e.g. Moscow) — now that td5_track_normalize_actor_wrap folds a
 * branch car's SPAN_NORMALIZED to its parallel main span on ALL tracks (not
 * just TD6), TD5 branch cars get valid route guidance + despawn distances and
 * drive the corridor like any other car. Previously gated TD6-only, so TD5
 * forks never got branch traffic (user: "traffic not spawning on the right
 * track of a branch"). */
static int trf_dyn_branch_spawn_ok(void)
{
    return trf_dyn_branches_enabled() && td5_track_corridor_count() > 0;
}

/* [#16 cross-traffic 2026-06-17] A/B knob for CROSS-TRAFFIC AT INTERSECTIONS.
 * When a dynamic-traffic car spawns on a TD6 BRANCH corridor (the drivable
 * alternate route taken at a junction), a fraction of those spawns are forced to
 * the OPPOSITE polarity (oncoming) so they drive AGAINST the player's flow
 * through the junction — giving the read of cross-traffic at intersections.
 * DEFAULT ON at a moderate fraction (#16 implemented 2026-06-19). Disable with
 * TD5RE_CROSS_TRAFFIC=0; set an explicit fraction (e.g. 0.5) to tune it.
 *
 * Returns the cross-traffic FRACTION as N/16 (0 = off). When unset it defaults
 * to a believable minority; "1" maps to the default fraction; an explicit
 * decimal sets the fraction directly. Clamped so a minority (never all) of
 * branch spawns are oncoming, keeping a two-way mix rather than a wall of
 * head-on cars. Logged once. */
static int trf_cross_traffic_x16(void)
{
    static int x16 = -2;
    /* [traffic one-way] One-way mode means no oncoming cars at all, including
     * the junction-branch cross-traffic — behaves exactly like
     * TD5RE_CROSS_TRAFFIC=0 (same lockstep-RNG consumption). */
    if (trf_oneway_traffic()) return 0;
    if (x16 == -2) {
        const char *e = getenv("TD5RE_CROSS_TRAFFIC");
        double f;
        if (e && e[0]) {
            f = strtod(e, NULL);                  /* explicit value; "0" disables */
            /* "1" (the common on-switch) means "enabled at the default fraction",
             * NOT 100% oncoming. Any other value is read as the literal fraction. */
            if (f == 1.0) f = 0.5;
        } else {
            f = 0.35;                             /* [#16] default ON: an occasional
                                                   * oncoming car on junction branches */
        }
        if (f < 0.0) f = 0.0;
        if (f > 0.75) f = 0.75;                   /* never more than 3/4 oncoming */
        x16 = (int)(f * 16.0 + 0.5);
        TD5_LOG_I(LOG_TAG, "cross_traffic knob: TD5RE_CROSS_TRAFFIC -> %d/16 "
                  "(oncoming fraction on branch corridors; 0 = off)", x16);
    }
    return x16;
}

/* True when cross-traffic is enabled at all (any non-zero fraction). */
static int trf_cross_traffic_enabled(void)
{
    return trf_cross_traffic_x16() > 0;
}

/* [#18] Width (in spans) of the traffic emergency-brake "proximity gate": a peer
 * this close AHEAD in-lane brakes the car immediately; farther peers are handled by
 * the closing-rate TTC formula. Default 4 (the faithful comment's "within 4 spans").
 * Env TD5RE_TRAFFIC_BRAKE_NEAR. */
static int trf_brake_near_spans(void)
{
    static int s = -1;
    if (s < 0) {
        const char *e = getenv("TD5RE_TRAFFIC_BRAKE_NEAR");
        /* 8 spans (was 4): 4 left too little stopping distance, so cars closed up and
         * OVERLAPPED before the brake took hold; 8 keeps a visible following gap while
         * still far below the old unbounded behaviour (which braked for cars ~30 spans
         * ahead and chain-stopped). The TTC closing-rate formula handles anything
         * beyond this. Env TD5RE_TRAFFIC_BRAKE_NEAR. */
        s = e ? atoi(e) : 8;
        if (s < 1) s = 1;
    }
    return s;
}

/* [traffic one-way] Make ALL traffic head the SAME direction (down-track). When
 * on, every lane's drive direction is forced forward (polarity 0): no oncoming
 * lanes, no junction cross-traffic, and no 180° heading flip on queue spawns.
 * DEFAULT ON (user: "the orientation should make the traffic all head the same
 * way"). TD5RE_TRAFFIC_ONEWAY=0 restores the authored two-way / cross-traffic. */
static int trf_oneway_traffic(void)
{
    static int s = -1;
    if (s < 0) {
        const char *e = getenv("TD5RE_TRAFFIC_ONEWAY");
        s = (e && e[0]) ? (atoi(e) != 0) : 1;   /* default ON */
        TD5_LOG_I(LOG_TAG, "traffic_oneway knob: TD5RE_TRAFFIC_ONEWAY -> %d "
                  "(1 = all traffic heads down-track; 0 = two-way/cross-traffic)", s);
    }
    return s;
}

/* [traffic pile-up] Hard minimum following gap (in spans). A same-lane peer
 * directly AHEAD within this many spans forces an unconditional brake — even
 * when the SmartAI lane-changer found a clear lane to ease into — because the
 * lane change has a 1-tick latency and easing here would roll the car onto the
 * peer this tick. Guarantees traffic "keeps some distance" instead of stacking
 * nose-to-tail. Default 3; TD5RE_TRAFFIC_MIN_GAP tunes it (>=1). */
static int trf_min_gap_spans(void)
{
    static int s = -1;
    if (s < 0) {
        const char *e = getenv("TD5RE_TRAFFIC_MIN_GAP");
        s = e ? atoi(e) : 3;
        if (s < 1) s = 1;
    }
    return s;
}

/* [task#13 2026-06-14] Spawn-distance multiplier (×16 fixed-point). The per-tick
 * spawner places traffic [SpawnAheadMin..SpawnAheadMax] spans ahead of the race
 * leader; the stock 25..50 window pops cars in close to the player. This knob
 * scales that window so traffic appears further away. TD5RE_TRAFFIC_SPAWN_DIST is
 * a decimal multiplier (e.g. "2.0"); "0" restores the stock 1.0x window. Default
 * 2.0x. Clamped 1.0x..6.0x. Logged once. */
static int trf_dyn_spawn_dist_x16(void)
{
    static int x16 = -1;
    if (x16 < 0) {
        const char *e = getenv("TD5RE_TRAFFIC_SPAWN_DIST");
        double m;
        if (e && e[0] == '0' && e[1] == '\0') {
            m = 1.0;                         /* explicit "0" = stock window */
        } else if (e && e[0]) {
            m = strtod(e, NULL);
            if (m < 1.0) m = 1.0;            /* never shrink below stock */
        } else {
            m = 2.0;                         /* default: twice as far away */
        }
        if (m > 6.0) m = 6.0;
        x16 = (int)(m * 16.0 + 0.5);
        TD5_LOG_I(LOG_TAG, "traffic_spawn_dist knob: TD5RE_TRAFFIC_SPAWN_DIST x%d/16 "
                  "(spawn window scaled %d%%)", x16, (x16 * 100) / 16);
    }
    return x16;
}

/* [traffic-front-despawn 2026-06-24] Minimum forward-keep distance, in spans. A
 * traffic car must NEVER fade out while it could still be on-screen AHEAD of a
 * human — that is the "traffic disappears right in front of me instead of far
 * away after I passed them" report. The forward despawn bounds (ahead-of-leader
 * and far-from-all) are normally despawn + eff_hi (default 65 + 100 = 165 spans,
 * comfortably past the ~88-128-span actor render cull). But on SHORT tracks
 * eff_hi is capped at ring/3 (see trf_dyn_effective_spawn_window), which can pull
 * that bound BELOW the render distance so a car fades while still visible ahead —
 * worst in split-screen, where the players spread apart and the far-from-all test
 * (min distance to ANY human) becomes the active despawn condition. This floor
 * keeps the forward-keep distance past the render cull on every track. The
 * rear/"behind" despawn is left untouched (a car retiring far behind after you've
 * passed it is the intended, wanted behaviour). Default 150 spans (render cull +
 * margin); TD5RE_TRAFFIC_FRONT_KEEP overrides (>=0; 0 disables the floor). */
static int trf_dyn_front_keep_floor(void)
{
    static int s = -1;
    if (s < 0) {
        const char *e = getenv("TD5RE_TRAFFIC_FRONT_KEEP");
        if (e && e[0]) {
            s = atoi(e);            /* explicit override always wins */
        } else {
            /* [traffic-view-dist 2026-06-29] Default tracks the extended actor
             * render cull (td5_render reads TD5RE_TRAFFIC_VIEW_DIST, default 1.6x)
             * so a car always fades out BEYOND the visible window, never on-screen.
             * Faithful single-screen max render = 128 spans * view-dist mult; keep
             * +24 spans of margin above that. Was a fixed 150. Reading the same env
             * var here (not a cross-module call) keeps the two in lockstep. */
            const char *ev = getenv("TD5RE_TRAFFIC_VIEW_DIST");
            float vm = (ev && ev[0]) ? (float)atof(ev) : 1.6f;
            if (vm < 1.0f) vm = 1.0f;
            if (vm > 2.5f) vm = 2.5f;
            s = (int)(128.0f * vm + 0.5f) + 24;
            if (s < 150) s = 150;   /* never below the prior faithful floor */
        }
        if (s < 0) s = 0;
        TD5_LOG_I(LOG_TAG, "traffic_front_keep floor: %d spans "
                  "(cars stay until this far ahead of every human)", s);
    }
    return s;
}

/* Effective per-tick spawn window after the distance scale, clamped so it never
 * grows past a safe fraction of the ring (a window wider than ~1/3 of the loop
 * would wrap onto the player from behind and could starve placement). Returns the
 * scaled [*lo, *hi]; the despawn cleanup bound below uses the same *hi so a car
 * spawned far ahead is not immediately faded out. */
static void trf_dyn_effective_spawn_window(int *lo, int *hi)
{
    int base_lo = g_td5.ini.traffic_dyn_spawn_min;
    int base_hi = g_td5.ini.traffic_dyn_spawn_max;
    int s16 = trf_dyn_spawn_dist_x16();
    int elo = (int)(((int64_t)base_lo * s16) >> 4);
    int ehi = (int)(((int64_t)base_hi * s16) >> 4);
    int ring = td5_track_get_ring_length();
    if (elo < base_lo) elo = base_lo;       /* never closer than stock min */
    /* [task#12 2026-06-15] Busy tiers also fill the NEAR road. _SPAWN_DIST scales
     * the FAR edge so cars appear in the distance; on HIGH/VERY HIGH we pull the
     * NEAR edge back down to (near) the stock min so the window spans near→far and
     * the six slots cover the whole stretch instead of clustering at one distance.
     * Low/Medium keep the _SPAWN_DIST window unchanged. */
    if (trf_dyn_density_enabled()) {
        int v = trf_dyn_volume();
        if (v >= 4)      elo = base_lo;              /* Very-High: from the stock min */
        else if (v == 3) elo = (elo + base_lo) / 2; /* High: halfway back toward min */
        if (elo < base_lo) elo = base_lo;
    }
    if (ehi < elo + 1) ehi = elo + 1;
    if (ring > 0) {
        /* Keep the FAR edge within ~1/3 of the ring so the spawn point stays
         * genuinely ahead and there is always placeable road. The NEAR edge
         * follows it down (but never below the stock min) so the window stays a
         * valid [lo<hi] range on small loops. */
        int cap = ring / 3;
        if (cap < base_hi) cap = base_hi;   /* tiny rings: never below stock */
        if (ehi > cap) ehi = cap;
        if (elo > ehi - 1) elo = (ehi - 1 > base_lo) ? ehi - 1 : base_lo;
        if (ehi < elo + 1) ehi = elo + 1;
    }
    if (lo) *lo = elo;
    if (hi) *hi = ehi;
}

/* Place `slot` at (span, lane, polarity) on the canonical main ring.
 * Same placement chain as the faithful recycle LEFT branch
 * [CONFIRMED @ 0x004354B5-0x004356CE + shared zero block LAB_0043588D]. */
static void trf_dyn_place(int slot, int span, int lane, int polarity)
{
    char    *a  = actor_ptr(slot);
    int32_t *rs = route_state(slot);
    uint8_t *rsb = (uint8_t *)rs;

    rs[RS_ROUTE_DIRECTION_POLARITY] = polarity ? 1 : 0;
    *(int32_t *)(rsb + 0x68) = slot * 4 + 0x10;   /* DAT_004afbc8 mirror */
    rs[RS_ROUTE_TABLE_SELECTOR] = 0;

    ACTOR_I16(a, ACTOR_SPAN_RAW)      = (int16_t)span;
    ACTOR_U8(a, ACTOR_SUB_LANE_INDEX) = (uint8_t)lane;

    td5_track_init_actor_segment_placement(
        (int16_t *)(a + ACTOR_SPAN_RAW),
        (int32_t *)(a + ACTOR_WORLD_POS_X));
    td5_track_compute_heading((TD5_Actor *)a);
    if (polarity)
        ACTOR_I32(a, ACTOR_YAW_ACCUM) += 0x80000;  /* oncoming: 180 deg flip */
    td5_physics_reset_actor_state((TD5_Actor *)a);
    td5_ai_seed_actor_track_progress_offset(slot);
    td5_track_normalize_actor_wrap((TD5_Actor *)a);

    /* Shared post-placement state zero (mirrors recycle LAB_0043588D). */
    *(int32_t *)(a + 0x314) = 0;
    *(int32_t *)(a + 0x30C) = 0;
    *(int8_t  *)(a + 0x379) = 0;
    *(int32_t *)(a + 0x1F0) = 0;
    *(int32_t *)(a + 0x1F8) = 0;
    *(int32_t *)(a + 0x1C0) = 0;
    *(int32_t *)(a + 0x1C4) = 0;
    *(int32_t *)(a + 0x1C8) = 0;
    *(int32_t *)(a + 0x1CC) = 0;
    *(int32_t *)(a + 0x1D0) = 0;
    *(int32_t *)(a + 0x1D4) = 0;
    *(int16_t *)(a + 0x338) = 0;
    *(int32_t *)(rsb + 0x88) = 0;
    *(int32_t *)(rsb + 0xF0) = 0;
    *(int32_t *)(rsb + 0x7C) = -1;
    rs[RS_RECOVERY_STAGE]   = 0;
    rs[RS_ENCOUNTER_HANDLE] = -1;
    if (slot >= 0 && slot < TD5_MAX_TOTAL_ACTORS) {
        g_traffic_recovery_stage[slot] = 0;
        s_traffic_stuck_frames[slot]   = 0;
        s_traffic_lane_bias[slot]      = 0;
        s_traffic_stall_frames[slot]   = 0;
        s_traffic_escape_ticks[slot]   = 0;
        s_traffic_escape_side[slot]    = 0;
        /* [R3-10] clear anti-pileup progress/lane-commit state on (re)placement */
        s_traffic_lowprog[slot]         = 0;
        s_traffic_lowprog_latch[slot]   = 0;
        s_traffic_prog_window[slot]     = 0;
        s_traffic_escape_cooldown[slot] = 0;
        s_traffic_escape_lane[slot]     = 0;
        s_traffic_escape_lane_ttl[slot] = 0;
        /* [#1 WRECK 2026-06-21] A (re)placed slot is a FRESH car — clear any stale
         * broken-down/wreck state so a recycled wreck slot does NOT spawn already
         * wrecked (anchored + smoking). The legacy recycle path clears this
         * (td5_ai_recycle_traffic_actor, ~6479); the dynamic spawner missed it,
         * which is the "lots of cars spawning wrecked" the user saw. (My
         * s_wreck_push_ticks in td5_physics.c is only read while broken_down is
         * set, so clearing the flag here makes any stale slide-window inert too.) */
        g_actor_broken_down[slot]  = 0;
        g_actor_broken_ticks[slot] = 0;
    }
}

/* [RIGHT-BRANCH TRAFFIC FIX 2026-06-21] Keep traffic on a fork's branch road.
 * Two failure modes (verified via the traffic_dev trace on Moscow: ring=2789,
 * 11 forks) peg traffic steering to ±0x18000 and fishtail it into the walls:
 *   A) span_norm lags span_raw on a branch, so the norm-based remap target
 *      stays at the car's OWN span and flips behind it -> ~180deg error.
 *   B) at a branch end the target lands on an ORPHAN boundary span (the dead
 *      spans between the main road and the branch dst ranges, e.g. ring_length)
 *      whose lane geometry is garbage -> the target is ~285000 units away.
 * Default ON; TD5RE_BRANCH_TRAFFIC_FIX=0 restores the old behaviour for A/B. */
static int branch_traffic_fix_enabled(void) {
    static int s = -1;
    if (s < 0) { const char *e = getenv("TD5RE_BRANCH_TRAFFIC_FIX");
                 s = (e && e[0] == '0') ? 0 : 1; }
    return s;
}

/* [task#18 2026-06-12] TD6 drivable lane band from the route tables.
 * On wide TD6 city strips only the CENTRAL lanes are paved road; the outer
 * lanes are sidewalk. The strip lane bitmask does NOT mark them (London's is
 * all zero), so td5_track_surface_is_slow can't gate them and traffic happily
 * drives on the pavement. The route tables DO encode the road: LEFT.TRK's lane
 * byte is the left racing line and RIGHT.TRK's the right line, each a lateral
 * position 0..255 across the full rail-to-rail width. Convert both to lane
 * indices -> the drivable band [lo,hi]; clamp traffic into it so cars keep off
 * the kerb. Native TD5 (g_active_td6_level==0) is untouched — the whole width
 * is road there. Branch-region spans (>= ring) have no route coverage, so the
 * helper returns 0 and the caller leaves the lane unclamped. Returns 1 when a
 * band narrower than the full lane range was derived.
 * `route_span` is a normalized (main-ring) span index == the route-table row. */
static int td5_ai_td6_drivable_band(int route_span, int lane_count,
                                    int *out_lo, int *out_hi)
{
    int lo = 0, hi = (lane_count > 0) ? lane_count - 1 : 0;
    if (out_lo) *out_lo = lo;
    if (out_hi) *out_hi = hi;
    if (g_active_td6_level <= 0 || lane_count <= 2 || route_span < 0)
        return 0;
    {
        /* [#18 2026-06-16] BRANCH corridors have no route-table coverage. Derive the
         * band straight from the SURFACE GRID: the contiguous road-class lanes about
         * the centre (excludes the sidewalk/verge edge lanes that surface_is_slow
         * misses). This keeps branch traffic on the actual two-way road — both
         * directions, off the kerb — instead of the median-only or full-width
         * heuristics. Falls through to the central-half fallback if the grid is flat. */
        int ringL = td5_track_get_ring_length();
        if (ringL > 0 && route_span >= ringL) {
            int rl, rh;
            if (td5_track_td6_road_band(route_span, lane_count, &rl, &rh)) {
                if (out_lo) *out_lo = rl;
                if (out_hi) *out_hi = rh;
                return 1;
            }
            /* grid is flat (no distinct kerb) -> central half keeps cars centred */
            {
                int m = lane_count / 4;
                lo = m; hi = lane_count - 1 - m;
                if (hi < lo) hi = lo;
                if (out_lo) *out_lo = lo;
                if (out_hi) *out_hi = hi;
                return (lo > 0 || hi < lane_count - 1);
            }
        }
    }
    {
        const uint8_t *lt = g_route_tables[0];
        const uint8_t *rt = g_route_tables[1];
        size_t idx = (size_t)(unsigned)route_span * 3u;
        int lb, rb;
        if (!lt || !rt ||
            idx >= g_route_table_sizes[0] || idx >= g_route_table_sizes[1]) {
            /* [task#20 2026-06-13] BRANCH / no-route-coverage span: the route table
             * only covers the main ring, so branch corridors (and the wide fork
             * spans) had NO band -> traffic wandered into the plaza/gate walls
             * (user: traffic "crash into walls and zig-zag" on branches). Fall back
             * to the CENTRAL HALF of the road, which is the drivable band the TD6
             * surface grid encodes (London row = [7,7,1,1,1,1,7,7] = central 4/8 =
             * road, outer = sidewalk). Keeps branch traffic on the corridor road. */
            int m = lane_count / 4;
            lo = m;
            hi = lane_count - 1 - m;
            if (hi < lo) hi = lo;
            if (out_lo) *out_lo = lo;
            if (out_hi) *out_hi = hi;
            return (lo > 0 || hi < lane_count - 1);
        }
        lb = (int)lt[idx];
        rb = (int)rt[idx];
        if (lb > rb) { int t = lb; lb = rb; rb = t; }
        lo = (lb * lane_count) / 256;
        hi = (rb * lane_count) / 256;
        if (lo < 0) lo = 0;
        if (hi > lane_count - 1) hi = lane_count - 1;
        if (hi < lo) hi = lo;
        /* [#18 2026-06-16] Intersect with the surface road-band so the route band
         * never includes a sidewalk-class edge lane (kerb is full-grip, so the
         * slow-lane filter misses it). Only tightens, never widens. */
        {
            int rl, rh;
            if (td5_track_td6_road_band(route_span, lane_count, &rl, &rh)) {
                if (lo < rl) lo = rl;
                if (hi > rh) hi = rh;
                if (hi < lo) hi = lo;
            }
        }
    }
    if (out_lo) *out_lo = lo;
    if (out_hi) *out_hi = hi;
    return (lo > 0 || hi < lane_count - 1);
}

/* Pick a random clear, non-slow lane on `span`; the drive direction is the
 * LANE's direction (learned from TRAFFIC.BUS / side heuristic), written to
 * *out_polarity. -1 when nothing qualifies.
 *
 * "Non-slow" is a HARD filter only when the span actually has a fast/slow
 * MIX (a shoulder next to asphalt — the user rule targets shoulders). When
 * EVERY lane is slow-flagged the whole road surface is alternate (e.g.
 * Moscow's cobblestone district sets the 0x10 alternate-surface bit on all
 * lanes) and refusing it starves the spawner for the entire stretch — there,
 * any clear lane qualifies.
 *
 * [#16 cross-traffic 2026-06-17] `desired_pol` is a direction HINT: -1 = use
 * each lane's natural direction (the original behaviour); 0/1 = prefer to spawn
 * a car driving in that polarity (1 = oncoming, against the player's forward
 * flow). When a hint is given we try the matching-direction lanes first AND
 * validate clearance in that requested direction (the clearance test is
 * direction-dependent — see traffic_lane_is_clear), then fall back to the
 * natural-direction pass so a hinted spawn is never starved. */
static int trf_dyn_pick_lane_dir(int slot, int span, int lane_count,
                                 int desired_pol, int *out_polarity)
{
    int start = (lane_count > 1) ? (int)(trf_dyn_rand() % (uint32_t)lane_count) : 0;
    int any_fast = 0;
    int band_lo = 0, band_hi = lane_count - 1;
    int pass;
    /* [task#18] TD6: confine spawns to the paved central band (route tables). */
    td5_ai_td6_drivable_band(span, lane_count, &band_lo, &band_hi);
    for (int lane = 0; lane < lane_count; lane++) {
        if (!td5_track_surface_is_slow(td5_track_get_span_lane_surface(span, lane))) {
            any_fast = 1;
            break;
        }
    }
    /* Pass 0 honours `desired_pol` (skipped when no hint); pass 1 is the
     * original natural-direction fallback. */
    for (pass = (desired_pol >= 0) ? 0 : 1; pass <= 1; pass++) {
        for (int k = 0; k < lane_count; k++) {
            int lane = (start + k) % lane_count;
            int nat  = trf_dyn_lane_direction(lane_count, lane);
            int pol  = (pass == 0) ? (desired_pol & 1) : nat;
            if (lane < band_lo || lane > band_hi)
                continue;   /* [task#18] outside the drivable band = sidewalk */
            if (any_fast &&
                td5_track_surface_is_slow(td5_track_get_span_lane_surface(span, lane)))
                continue;   /* user rule: never spawn on a slow (shoulder) lane */
            if (!traffic_lane_is_clear(slot, span, lane, pol))
                continue;
            *out_polarity = pol;
            return lane;
        }
    }
    return -1;
}
/* NOTE: the previous wrapper `trf_dyn_pick_lane(slot,span,lane_count,out)` was a
 * thin alias for `trf_dyn_pick_lane_dir(..., -1, ...)`; it had a single call site
 * which now passes the cross-traffic hint directly, so the alias was removed to
 * avoid an unused-static warning. Pass desired_pol = -1 for the old behaviour. */

/* [task#8] Branch-corridor enumeration helpers (defined in td5_track.c). Declared
 * in-file (not the shared header) per the same convention as the externs above. */
extern int td5_track_count_branch_corridors(int main_span);
extern int td5_track_branch_corridor_span(int main_span, int which);

/* [item#9 2026-06-15] Deliberately pick a MAIN-ring span (within [win_lo..win_hi]
 * ahead of the anchor `ps`) that is PARALLELED by an active branch corridor, so a
 * subsequent branch retarget reliably lands a car on a fork. The earlier branch
 * code only retargeted when the randomly-rolled main span happened to sit inside
 * a fork window (statistically rare with only a few short fork windows on the
 * ring), so branch corridors stayed nearly empty (user: "traffic is still not
 * spawning on the right track of branches"). Returning a main span that is KNOWN
 * to have a branch lets the spawner run its normal edge / start-clearance /
 * per-player-proximity checks against that main span (exactly as for any main-road
 * spawn) and then map it to the branch span — every existing safety check still
 * applies, and a fork is hit on demand instead of by luck. Returns a ring-relative
 * main span, or -1 if no corridor overlaps the forward window. */
static int trf_dyn_pick_branch_main_span(int ps, int win_lo, int win_hi)
{
    int ring = td5_track_get_ring_length();
    int is_circuit = (g_td5.track_type == TD5_TRACK_CIRCUIT);
    int ncorr = td5_track_corridor_count();
    int q_lo[64], q_hi[64];
    int nq = 0;
    int wlo = ps + win_lo;
    int whi = ps + win_hi;
    if (ncorr <= 0) return -1;
    if (ncorr > 64) ncorr = 64;

    for (int i = 0; i < ncorr; i++) {
        int b_lo, b_hi, m_lo, m_hi;
        int lo, hi;
        if (!td5_track_corridor_info(i, &b_lo, &b_hi, &m_lo, &m_hi)) continue;
        if (b_lo < ring) continue;              /* not a displaced corridor */
        lo = m_lo; hi = m_hi;
        if (is_circuit && ring > 0) {
            /* shift the corridor's parallel-main range onto the window's arc so a
             * corridor that straddles the lap seam still qualifies */
            while (hi < wlo) lo += ring, hi += ring;
            while (lo > whi) lo -= ring, hi -= ring;
        }
        if (hi < wlo || lo > whi) continue;     /* no overlap with the window */
        if (lo < wlo) lo = wlo;                  /* clamp to the window */
        if (hi > whi) hi = whi;
        if (hi < lo) continue;
        q_lo[nq] = lo; q_hi[nq] = hi; nq++;
    }
    if (nq <= 0) return -1;
    {
        int pick = (int)(trf_dyn_rand() % (uint32_t)nq);
        int m = q_lo[pick] + (int)(trf_dyn_rand() % (uint32_t)(q_hi[pick] - q_lo[pick] + 1));
        if (is_circuit && ring > 0) {
            m %= ring;
            if (m < 0) m += ring;
        }
        return m;
    }
}

/* Try to place `slot` at a random span [win_lo..win_hi] ahead of `anchor`
 * (-1 = the live RACE LEADER, any racer slot, human or AI — user rule:
 * traffic appears in front of 1st place, and only retires once the TRAILING
 * human has passed it; AI racers never retire traffic). Race-init seeding
 * passes the START-LINE span explicitly because at init time the grid is not
 * placed yet and every actor's span still reads 0 — anchoring on the live
 * "leader" there scattered seeds around the lap line, ignoring the
 * start-clearance zone on circuits (the Scotland race-start bug).
 * Returns 1 on success. */

static int trf_dyn_spawn_in_window(int slot, int anchor, int win_lo, int win_hi)
{
    int ring   = td5_track_get_ring_length();
    int limit  = (ring > 0) ? ring : td5_track_get_span_count();
    int is_circuit = (g_td5.track_type == TD5_TRACK_CIRCUIT);

    if (win_hi < win_lo) win_hi = win_lo;
    if (limit <= 16) return 0;   /* degenerate strip */

    for (int attempt = 0; attempt < 8; attempt++) {
        /* [task#12 2026-06-15] Live-spawn anchor. The legacy anchor is the overall
         * RACE LEADER (any racer, incl. an AI). In a single-player race the AI
         * leader routinely pulls hundreds of spans ahead of the human, so
         * leader-anchored spawns appear off-screen ahead of the AI and the
         * human's surroundings steadily empty out — the user's "traffic gets
         * sparse / spawns less often as the race goes on." With
         * TD5RE_TRAFFIC_DENSITY on, anchor live spawns on the FRONT-MOST HUMAN
         * instead, so traffic always materialises in a human's forward view
         * regardless of where the AI pack is, and the proximity recycle keeps the
         * six slots cycling through that view. (Race-init seeding still passes an
         * explicit start-line anchor.) Knob off → legacy race-leader anchor. */
        int ps;
        if (anchor >= 0)
            ps = anchor;
        else if (trf_dyn_density_enabled()) {
            /* [item#10 2026-06-15] In a multi-human race, anchor on whichever human
             * currently has the LEAST nearby traffic (tie -> trailing player), so
             * the trailing player(s) get refilled instead of every spawn piling up
             * around the leader — each human ends up with a comparable amount of
             * traffic. Single human: this is just that human's span, identical to
             * the front-most-human anchor. The radius matches the far_from_all
             * recycle corridor below (despawn + scaled spawn-max) so "starved" here
             * and "too far to keep" there use the same yardstick. */
            if (g_td5.num_human_players > 1) {
                int rhi = 0;
                trf_dyn_effective_spawn_window(NULL, &rhi);
                ps = trf_dyn_starved_player_span(g_td5.ini.traffic_dyn_despawn + rhi);
            } else {
                ps = ai_player_span_lead();    /* front-most (== only) human */
            }
        } else
            ps = trf_dyn_race_span_lead();     /* legacy: live race leader */
        /* [traffic-density-patch 2026-06-24] On later attempts the nominal
         * [win_lo, win_hi] window is failing to place a car — typically the
         * stretch directly ahead is a junction, a branch / no-road region, or an
         * all-slow surface where every candidate span is rejected, leaving a
         * visible empty PATCH for the whole time the player approaches it (the
         * "patches of track where the frequency of traffic decreased
         * significantly" report). Progressively push the FAR edge of the search
         * band outward so the spawner can reach PAST the bad stretch to placeable
         * road and fill the gap beyond it, instead of hammering the same dead
         * window 8 times and giving up. The NEAR edge stays at win_lo so a car is
         * never popped in close to the player, and the min-player-dist gate below
         * still uses win_lo. The first two attempts use the nominal window so the
         * common (already-placeable) case is byte-unchanged; the span<3 /
         * span>=limit-8 and circuit-wrap clamps below bound the widened far edge.
         * TD5RE_TRAFFIC_SPAWN_DIST / window math are untouched — this only affects
         * the RETRY band when the nominal window has no room. */
        int a_win_hi = win_hi;
        if (attempt >= 2 && win_hi > win_lo)
            a_win_hi = win_hi + (attempt - 1) * (win_hi - win_lo);
        int dist = win_lo + (int)(trf_dyn_rand() % (uint32_t)(a_win_hi - win_lo + 1));
        int span = ps + dist;
        int main_span;
        int lane_count, lane, polarity;
        int want_branch = 0;
        int on_branch = 0;       /* [#16] set when the spawn retargets onto a branch corridor */
        int cross_pol = -1;      /* [#16] desired-direction hint for the lane picker (-1 = none) */

        if (is_circuit && ring > 0) {
            span %= ring;
            if (span < 0) span += ring;
        }

        /* [item#9 2026-06-15] Deliberate branch attempt: roughly every third
         * placement (when branch corridors are drivable + enabled), aim the spawn
         * at a main span that is KNOWN to be paralleled by a fork, so the retarget
         * below reliably populates a branch corridor instead of waiting for a
         * random main span to land in a (rare) fork window. The chosen main span
         * then runs through all the normal edge/clearance/proximity checks. Gated
         * on TD5RE_TRAFFIC_BRANCHES (default on). */
        if (trf_dyn_branch_spawn_ok() && (trf_dyn_rand() % 3u) == 0u) {
            int bm = trf_dyn_pick_branch_main_span(ps, win_lo, win_hi);
            if (bm >= 0) { span = bm; want_branch = 1; }
        }
        /* Keep off the track-edge spans where the faithful route plan brakes
         * (span < 3 || span >= ring-8, cf. Stage 3 @ 0x00435FAA). */
        if (span < 3 || span >= limit - 8) continue;

        /* Start-line clearance ([Traffic] SpawnStartOffset, user rule): no
         * traffic placements within N spans AFTER the start line, so the
         * grid/launch stretch stays clear. Wrap-aware on circuits (the zone
         * repeats each lap). */
        if (g_td5.ini.traffic_dyn_start_offset > 0) {
            int rel = span - g_td5.track_start_span_index;
            if (is_circuit && ring > 0) {
                rel %= ring;
                if (rel < 0) rel += ring;
            }
            if (rel >= 0 && rel < g_td5.ini.traffic_dyn_start_offset)
                continue;
        }

        /* The window must hold against EVERY local player, not just the one
         * we rolled (multiplayer: no spawning on top of another pane). */
        if (trf_dyn_min_player_dist(span) < win_lo) continue;

        /* [proximity gate] Don't clump: skip this span if its stretch of road
         * already holds enough traffic. `span` here is the main-ring candidate
         * (branch retarget below folds back to it for counting), matching how
         * trf_dyn_count_traffic_near normalizes branch cars to their main span. */
        {
            int cmax = trf_dyn_cluster_max();
            if (cmax > 0 &&
                trf_dyn_count_traffic_near(span, trf_dyn_cluster_radius()) >= cmax)
                continue;
        }

        /* [task#8 2026-06-14] Populate ALL branch corridors of a fork, not just
         * the main/right route. The chosen main-ring span has passed every
         * proximity/clearance check; if one or more branch corridors PARALLEL this
         * main span and branches are drivable, roll uniformly over {main road} +
         * {each branch} and retarget the spawn onto whichever was picked, so both
         * sides of a fork get traffic (user: "traffic only populates the RIGHT
         * track of a branch"; with 2 corridors a branch is picked 2/3 of the time).
         * Earlier this called td5_track_main_to_branch_span(), which returns only
         * the FIRST matching corridor -> a single branch was ever populated. The
         * branch span is contiguous, so the walker traverses it and rejoins the
         * main ring at the corridor end like any other car. Gated on
         * TD5RE_TRAFFIC_BRANCHES (default on). */
        main_span = span;
        if (trf_dyn_branch_spawn_ok()) {
            int ncorr = td5_track_count_branch_corridors(main_span);
            if (ncorr > 0) {
                /* When this main span was chosen DELIBERATELY for a branch
                 * (want_branch), always land on a branch corridor — pick one of
                 * the `ncorr` corridors uniformly. Otherwise roll over {main road}
                 * + {each branch} with equal weight so forks fill evenly while the
                 * main road is never starved (the original task#8 behaviour). */
                int corr = want_branch
                    ? (int)(trf_dyn_rand() % (uint32_t)ncorr)
                    : (int)(trf_dyn_rand() % (uint32_t)(ncorr + 1)) - 1;
                if (corr >= 0) {
                    int bspan = td5_track_branch_corridor_span(main_span, corr);
                    /* [#20 2026-06-18] Never place traffic on a blacklisted fork
                     * (user-flagged dead-end sidewalk corridor). If this attempt
                     * was deliberately aiming at a branch, retry a fresh span;
                     * otherwise just stay on the main road. */
                    if (bspan >= 0 && td5_track_branch_blacklisted(bspan)) {
                        if (want_branch) continue;
                        bspan = -1;
                    }
                    if (bspan >= 0) {
                        span = bspan;
                        on_branch = 1;   /* [#16] this car is on a crossing branch corridor */
                    }
                }
            }
        }

        lane_count = td5_track_span_lane_count_at(span);
        if (lane_count <= 0) continue;

        /* [#16 cross-traffic 2026-06-17] If this car landed on a BRANCH corridor
         * (a crossing route taken at a junction) and cross-traffic is enabled,
         * force a fraction of these spawns to ONCOMING (polarity 1) so they drive
         * AGAINST the player's forward flow through the junction. The lane picker
         * still validates clearance in the requested direction and falls back to
         * the lane's natural direction if no oncoming lane is free, so the car is
         * never starved. Netplay-safe: trf_dyn_rand() is the lockstep RNG, so
         * every peer/replay rolls the same cross decision. NOTE/LIMITATION: TD6
         * branches are PARALLEL-alternate corridors, not true perpendicular roads —
         * the data has no crossing geometry — so "cross-traffic" here is realised as
         * OPPOSITE-DIRECTION traffic on the junction's branch route. It reads as
         * cross-traffic where a branch meets the main ring at the junction, which is
         * the best the parallel-branch data supports. */
        cross_pol = -1;
        if (on_branch && trf_cross_traffic_enabled() &&
            (int)(trf_dyn_rand() % 16u) < trf_cross_traffic_x16())
            cross_pol = 1;   /* oncoming = against the player's forward flow */

        /* Direction follows the LANE (authored TRAFFIC.BUS map / side heuristic)
         * unless the cross-traffic hint above asked for oncoming — a car never
         * drives against its lane's direction in normal (non-cross) flow. */
        polarity = 0;
        lane = trf_dyn_pick_lane_dir(slot, span, lane_count, cross_pol, &polarity);
        if (lane < 0) continue;

        trf_dyn_place(slot, span, lane, polarity);
        TD5_LOG_I(LOG_TAG,
                  "traffic_dyn_spawn: slot=%d span=%d (main=%d) lane=%d/%d oncoming=%d "
                  "cross=%d player_dist=%d attempt=%d",
                  slot, span, main_span, lane, lane_count, polarity,
                  (cross_pol == 1), trf_dyn_min_player_dist(span), attempt);
        return 1;
    }
    return 0;
}

/* Race-start seeding: scatter up to `cap` cars around the players (no fade —
 * they were "always there"), park the rest. Replaces the queue fill of
 * InitializeTrafficActorsFromQueue when dynamic mode is on. */
void td5_ai_traffic_dynamic_race_init(void)
{
    int t_base = g_traffic_slot_base;
    int t_end  = g_active_actor_count;
    int cap, placed = 0;

    if (t_end > t_base + TD5_MAX_TRAFFIC_SLOTS)
        t_end = t_base + TD5_MAX_TRAFFIC_SLOTS;
    if (t_end > TD5_MAX_TOTAL_ACTORS) t_end = TD5_MAX_TOTAL_ACTORS;

    memset(s_trf_dyn_state, TRF_DYN_INACTIVE, sizeof(s_trf_dyn_state));
    memset(s_trf_dyn_alpha, 0, sizeof(s_trf_dyn_alpha));
    s_trf_dyn_rng      = 0x54443552u ^ ((uint32_t)g_td5.track_index * 2654435761u);
    s_trf_dyn_cooldown = 0;
    s_trf_dyn_seeded   = 1;
    cop_state_reset();   /* police-chase per-race state (deterministic) */
    trf_per_viewport_setup();   /* [per-viewport traffic] partition + activation */

    /* Learn the per-track lane→direction map from the authored TRAFFIC.BUS
     * records (4-byte {span, flags bit0 = oncoming, lane}, -1 span sentinel
     * [CONFIRMED @ 0x004353CE]). Junction-table entries (lane >= the span's
     * lane-count nibble — the original's REMAP branch @ 0x004359C1) are
     * skipped: their lane index refers to the alternate table. Also derives
     * which HALF of the road carries oncoming traffic, as the fallback for
     * unseen (lane_count, lane) combos and for tracks without a queue
     * (TD6 conversions default to oncoming-on-the-left-half). */
    memset(s_trf_dyn_lane_dir, -1, sizeof(s_trf_dyn_lane_dir));
    s_trf_dyn_oncoming_left = 1;
    s_trf_dyn_oncoming_pct  = 0;
    if (g_traffic_queue_base) {
        static int16_t votes[TRF_DYN_MAX_LANES + 1][TRF_DYN_MAX_LANES][2];
        const uint8_t *qp = g_traffic_queue_base;
        int total = 0, oncoming = 0;
        /* side accumulator: normalized lane position (0..256 across the road)
         * summed separately for forward and oncoming entries. */
        int32_t side_sum[2] = { 0, 0 };
        int32_t side_n[2]   = { 0, 0 };
        memset(votes, 0, sizeof(votes));
        for (int i = 0; i < 4096; i++, qp += 4) {
            int16_t q_span = (int16_t)(qp[0] | (qp[1] << 8));
            int pol, lane, lc;
            if (q_span == -1) break;
            pol  = (int)(qp[2] & 1u);
            lane = (int)qp[3];
            lc   = td5_track_span_lane_count_at((int)q_span);
            total++;
            if (pol) oncoming++;
            if (lc < 1 || lc > TRF_DYN_MAX_LANES || lane >= lc)
                continue;   /* junction-table entry or unusable span */
            votes[lc][lane][pol]++;
            side_sum[pol] += ((lane * 2 + 1) * 128) / lc;   /* 0..256 centre pos */
            side_n[pol]++;
        }
        for (int lc = 1; lc <= TRF_DYN_MAX_LANES; lc++)
            for (int lane = 0; lane < lc; lane++)
                if (votes[lc][lane][0] | votes[lc][lane][1])
                    s_trf_dyn_lane_dir[lc][lane] =
                        (votes[lc][lane][1] > votes[lc][lane][0]) ? 1 : 0;
        if (side_n[0] > 0 && side_n[1] > 0) {
            s_trf_dyn_oncoming_left =
                (side_sum[1] / side_n[1]) < (side_sum[0] / side_n[0]);
        }
        if (total > 0)
            s_trf_dyn_oncoming_pct = (oncoming * 100) / total;
    }

    cap = trf_dyn_cap();
    if (s_trf_per_vp) {
        /* [PER-VIEWPORT TRAFFIC] Seed each partition identically: same start-line
         * anchor + identical-seed RNG, scoped to its own viewport's player, so every
         * viewport starts with matching grid traffic in matching places. */
        for (int vp = 0; vp < s_trf_vp_count; vp++) {
            int p_lo = t_base + vp * s_trf_vp_k;
            int p_hi = (vp == s_trf_vp_count - 1) ? (t_base + s_trf_vp_count * s_trf_vp_k)
                                                  : (p_lo + s_trf_vp_k);
            int vp_placed = 0, vp_cap = trf_per_viewport_cap();
            if (p_hi > t_end) p_hi = t_end;
            /* Shared start-line anchor + identical-seed RNG => identical grid. */
            s_trf_dyn_rng         = s_trf_dyn_rng_vp[vp];
            s_trf_spawn_partition = vp;   /* density gate counts only this partition */
            for (int slot = p_lo; slot < p_hi; slot++) {
                int seed_lo = g_td5.ini.traffic_dyn_start_offset + 8;
                if (vp_placed < vp_cap &&
                    trf_dyn_spawn_in_window(slot, g_td5.track_start_span_index,
                                            seed_lo, seed_lo + 100)) {
                    s_trf_dyn_state[slot] = TRF_DYN_ACTIVE;
                    s_trf_dyn_alpha[slot] = 255;
                    vp_placed++;
                } else {
                    s_trf_dyn_state[slot] = TRF_DYN_INACTIVE;
                    s_trf_dyn_alpha[slot] = 0;
                }
            }
            s_trf_spawn_partition = -1;
            s_trf_dyn_rng_vp[vp]  = s_trf_dyn_rng;
            placed += vp_placed;
        }
        s_trf_scope_slot = -1;
    } else
    for (int slot = t_base; slot < t_end; slot++) {
        /* Seed anchored on the START-LINE span (the grid isn't placed yet at
         * init time — live actor spans all read 0 here), scattered across a
         * 100-span stretch just past the start-clearance zone, so the road is
         * already populated where the field will first encounter traffic. */
        int seed_lo = g_td5.ini.traffic_dyn_start_offset + 8;
        if (placed < cap &&
            trf_dyn_spawn_in_window(slot, g_td5.track_start_span_index,
                                    seed_lo, seed_lo + 100)) {
            s_trf_dyn_state[slot] = TRF_DYN_ACTIVE;
            s_trf_dyn_alpha[slot] = 255;
            placed++;
        } else {
            /* Parked: every consumer (AI/physics/V2V/render/minimap/audio)
             * skips INACTIVE slots, so the zeroed pose is never observed. */
            s_trf_dyn_state[slot] = TRF_DYN_INACTIVE;
            s_trf_dyn_alpha[slot] = 0;
        }
    }
    TD5_LOG_I(LOG_TAG,
              "traffic_dyn_init: seeded %d/%d cars (volume=%d cap=%d oncoming=%d%% "
              "oncoming_left=%d window=[%d..%d] despawn=%d fade=%d period=%d speed=%d%% "
              "start_clear=%d@%d)",
              placed, t_end - t_base, g_td5.traffic_volume, cap,
              s_trf_dyn_oncoming_pct, s_trf_dyn_oncoming_left,
              g_td5.ini.traffic_dyn_spawn_min, g_td5.ini.traffic_dyn_spawn_max,
              g_td5.ini.traffic_dyn_despawn, g_td5.ini.traffic_dyn_fade_ticks,
              g_td5.ini.traffic_dyn_period, g_td5.ini.traffic_dyn_speed_pct,
              g_td5.ini.traffic_dyn_start_offset, g_td5.track_start_span_index);
    {
        /* [task#8/#13] Report the resolved run-time spawn behaviour once per race:
         * the SCALED per-tick spawn window (the close-pop-in fix) and whether
         * branch corridors are populated (the branch-traffic fix). The init seed
         * above intentionally uses the stock close window; these are the values the
         * in-race spawner uses. */
        int eff_lo = 0, eff_hi = 0;
        trf_dyn_effective_spawn_window(&eff_lo, &eff_hi);
        TD5_LOG_I(LOG_TAG,
                  "traffic_dyn_runtime: volume=%d cap=%d period~=%d density=%s "
                  "spawn_window=[%d..%d] (scaled x%d/16) branches=%s td6=%d "
                  "branches_drivable=%d",
                  trf_dyn_volume(), trf_dyn_cap(), g_td5.ini.traffic_dyn_period,
                  trf_dyn_density_enabled() ? "ON" : "OFF",
                  eff_lo, eff_hi, trf_dyn_spawn_dist_x16(),
                  trf_dyn_branches_enabled() ? "ON" : "OFF",
                  g_active_td6_level, td5_track_td6_branches_drivable());
    }
}

/* Per-sim-tick driver — fades, despawn checks, spawn cadence.
 * Called once per tick from td5_ai_pre_tick. */
void td5_ai_traffic_dynamic_tick(void)
{
    int t_base, t_end, fade_step, on_road = 0;

    if (!td5_ai_traffic_dynamic_active()) return;
    if (!g_actor_base || !g_route_state_base) return;
    t_base = g_traffic_slot_base;
    t_end  = g_active_actor_count;
    if (t_end <= t_base) return;
    if (t_end > t_base + TD5_MAX_TRAFFIC_SLOTS) t_end = t_base + TD5_MAX_TRAFFIC_SLOTS;
    if (t_end > TD5_MAX_TOTAL_ACTORS) t_end = TD5_MAX_TOTAL_ACTORS;

    fade_step = 255 / g_td5.ini.traffic_dyn_fade_ticks;
    if (fade_step < 1) fade_step = 1;

    for (int slot = t_base; slot < t_end; slot++) {
        char *a = actor_ptr(slot);
        /* [PER-VIEWPORT TRAFFIC] Despawn uses the SHARED (all-players) corridor so
         * every partition retires the same cars at the same time — keeping the
         * partitions identical-in-place. Independence is collision/render only. */
        switch (s_trf_dyn_state[slot]) {
        case TRF_DYN_FADE_IN:
            s_trf_dyn_alpha[slot] = (int16_t)(s_trf_dyn_alpha[slot] + fade_step);
            if (s_trf_dyn_alpha[slot] >= 255) {
                s_trf_dyn_alpha[slot] = 255;
                s_trf_dyn_state[slot] = TRF_DYN_ACTIVE;
            }
            on_road++;
            break;

        case TRF_DYN_ACTIVE: {
            int sp, behind, ahead, ring, is_circuit;
            /* Never retire the live cop (mirrors recycle's slot-9 guard
             * @ 0x0043545B). */
            if (slot == 9 && g_encounter_tracked_handle != -1) { on_road++; break; }
            /* Live corridor = [trailing HUMAN - despawn .. race LEADER +
             * despawn]. A car only retires once the LAST human has passed it
             * by DespawnDistance (signed — cars still AHEAD of a last-place
             * human stay alive for them to encounter), or if it somehow ends
             * up far past the race leader. AI racers never retire traffic. */
            sp = (int)(int16_t)ACTOR_I16(a, ACTOR_SPAN_NORMALIZED);
            behind = sp - ai_player_span_trailing();
            ahead  = sp - trf_dyn_race_span_lead();
            ring = td5_track_get_ring_length();
            is_circuit = (g_td5.track_type == TD5_TRACK_CIRCUIT);
            if (is_circuit && ring > 0) {
                int half = ring / 2;
                while (behind > half)  behind -= ring;
                while (behind < -half) behind += ring;
                while (ahead > half)   ahead -= ring;
                while (ahead < -half)  ahead += ring;
            }
            /* Ahead-of-leader bound is a generous CLEANUP (despawn+spawn_max,
             * ~115 spans — past the ~88-128 actor render cull): a SpeedScale'd
             * traffic car can genuinely outrun the race leader, and fading it
             * at the plain despawn distance was visible from the leader's
             * seat ("traffic disappears in front of me").
             *
             * [task#13] The cleanup distance uses the EFFECTIVE (scaled) spawn
             * max, not the raw INI value: TD5RE_TRAFFIC_SPAWN_DIST pushes spawns
             * further ahead, so a car freshly placed near the scaled max must NOT
             * be inside the despawn corridor or it would fade out the instant it
             * spawned. Tying the bound to the same scaled max keeps the spawn /
             * cleanup distances consistent.
             *
             * Stuck replacement is DEBOUNCED on s_traffic_stuck_frames (45
             * ticks ≈ 1.5s continuously recovery-frozen, counted by the
             * AntiFreeze layer): the Stage-2 recovery latch also arms
             * TRANSIENTLY for a tick or two on curves (esp. oncoming cars),
             * and replacing on a transient latch vanished healthy cars in
             * plain sight. (With [Traffic] AntiFreeze=0 the counter stays 0 —
             * stuck cars then just brake until the corridor passes them.) */
            {
            int eff_lo, eff_hi;
            int far_from_all;
            int front_keep;
            trf_dyn_effective_spawn_window(&eff_lo, &eff_hi);
            /* [traffic-front-despawn 2026-06-24] Forward-keep distance: cars
             * stay alive until this far ahead of every human, floored at the
             * render-visible distance so none ever fades while on-screen ahead.
             * Only ever WIDENS the keep-zone (never below despawn+eff_hi), so a
             * freshly spawned car at eff_hi ahead is never inside it -> no
             * spawn/despawn thrash. The rear "behind" bound is unchanged. */
            front_keep = g_td5.ini.traffic_dyn_despawn + eff_hi;
            {
                int fk_floor = trf_dyn_front_keep_floor();
                if (front_keep < fk_floor) front_keep = fk_floor;
            }
            /* [task#12 2026-06-15] PROXIMITY RECYCLE — the dwindle fix.
             * The corridor test above is anchored on the TRAILING HUMAN (behind)
             * and the RACE LEADER (ahead, who may be an AI). When the field
             * spreads — a fast AI leader pulls away from a slow human, or two
             * humans drift apart — that corridor grows to hundreds of spans, so
             * a healthy car that nobody is near NEVER satisfies behind<-despawn
             * or ahead>despawn+eff_hi. It stays ACTIVE forever, on_road pins at
             * the cap, the spawner is gated off, and the six slots smear thinly
             * across the whole gap → "traffic gets sparse / spawns less often as
             * the race goes on." Recycling any car that is farther than
             * (despawn + scaled spawn-max) from EVERY human returns its slot to
             * the pool so a fresh car is placed back near the action, holding the
             * density steady for the whole race regardless of field spread.
             * (The fresh car re-enters via FADE_IN ahead of the leader, so this
             * never strands the leader without traffic.) Gated on
             * TD5RE_TRAFFIC_DENSITY. */
            far_from_all = trf_dyn_density_enabled() &&
                           !td5_ai_cop_is_chasing(slot) &&
                           trf_dyn_min_player_dist(sp) > front_keep;
            /* [#20] (Removed) The branch idle-fade + blacklist self-heal that used to
             * sit here coped with traffic stranded on "displaced/off-road" forks.
             * Branches are connected drivable roads, so branch traffic is ordinary
             * traffic handled by the general despawn (behind/ahead/far) below. */
            if (!td5_ai_cop_is_chasing(slot) &&
                (behind < -g_td5.ini.traffic_dyn_despawn ||
                ahead  >  front_keep ||
                far_from_all ||
                (g_traffic_recovery_stage[slot] != 0 &&
                 s_traffic_stuck_frames[slot] >= 45 &&
                 /* [traffic-front-despawn 2026-06-29] far enough that the fade is
                  * unobtrusive — gate on the SAME forward-keep distance as the
                  * ahead/far paths (front_keep, which is >= the ~128-span actor
                  * render cull at ViewDistance=100), NOT the bare 65-span despawn.
                  * A stuck car 66..128 spans away is still ON-SCREEN, so fading it
                  * at the old 65-span gate vanished it in plain sight ("traffic
                  * disappears in front of me"). A nearer stuck car is realigned by
                  * AntiFreeze / just brakes until the player passes it (then the
                  * rear despawn retires it out of view). */
                 trf_dyn_min_player_dist(sp) > front_keep))) {
                s_trf_dyn_state[slot] = TRF_DYN_FADE_OUT;
                TD5_LOG_I(LOG_TAG,
                          "traffic_dyn_despawn: slot=%d behind_trail=%d ahead_lead=%d "
                          "min_player=%d front_keep=%d far_from_all=%d recovery=%d stuck=%d (fading out)",
                          slot, behind, ahead, trf_dyn_min_player_dist(sp),
                          front_keep, far_from_all, g_traffic_recovery_stage[slot],
                          s_traffic_stuck_frames[slot]);
            }
            }   /* eff spawn-window scope */
            on_road++;
            break;
        }

        case TRF_DYN_FADE_OUT:
            s_trf_dyn_alpha[slot] = (int16_t)(s_trf_dyn_alpha[slot] - fade_step);
            if (s_trf_dyn_alpha[slot] <= 0) {
                s_trf_dyn_alpha[slot] = 0;
                s_trf_dyn_state[slot] = TRF_DYN_INACTIVE;
                /* Police: a recycled slot loses its cop identity + any chase
                 * lock, so a future ordinary spawn here isn't born a cop. */
                if (g_cop_is_cop[slot]) cop_release(slot);
                /* Park: freeze all motion so the hidden actor stays put. */
                *(int32_t *)(a + 0x314) = 0;   /* longitudinal speed */
                *(int32_t *)(a + 0x1C0) = 0;
                *(int32_t *)(a + 0x1C4) = 0;
                *(int32_t *)(a + 0x1C8) = 0;
                *(int32_t *)(a + 0x1CC) = 0;   /* lin vel x */
                *(int32_t *)(a + 0x1D0) = 0;
                *(int32_t *)(a + 0x1D4) = 0;   /* lin vel z */
                ACTOR_U8(a, ACTOR_BRAKE_FLAG) = 1;
            } else {
                on_road++;
            }
            break;

        default: /* TRF_DYN_INACTIVE */
            break;
        }
    }

    s_trf_scope_slot = -1;   /* clear per-slot despawn scope before the spawn pass */

    /* [PER-VIEWPORT TRAFFIC] Time-trial split-screen: each partition runs its OWN
     * spawn cadence — scoped to its viewport's player + private identical-seed RNG —
     * so the sets land in matching places yet stay fully independent (one player
     * perturbing a car never shows in another pane). No cop logic (TT has no cops). */
    if (s_trf_per_vp) {
        for (int vp = 0; vp < s_trf_vp_count; vp++) {
            int p_lo = t_base + vp * s_trf_vp_k;
            int p_hi = (vp == s_trf_vp_count - 1) ? (t_base + s_trf_vp_count * s_trf_vp_k)
                                                  : (p_lo + s_trf_vp_k);
            int vp_on_road = 0, pick = -1, slot, spawn_lo, spawn_hi;
            if (p_hi > t_end) p_hi = t_end;
            if (s_trf_dyn_cooldown_vp[vp] > 0) s_trf_dyn_cooldown_vp[vp]--;
            for (slot = p_lo; slot < p_hi; slot++)
                if (s_trf_dyn_state[slot] != TRF_DYN_INACTIVE) vp_on_road++;
            if (vp_on_road >= trf_per_viewport_cap() || s_trf_dyn_cooldown_vp[vp] > 0)
                continue;
            for (slot = p_lo; slot < p_hi; slot++)
                if (s_trf_dyn_state[slot] == TRF_DYN_INACTIVE) { pick = slot; break; }
            if (pick < 0) { s_trf_dyn_cooldown_vp[vp] = 10; continue; }
            /* Shared anchor/proximity (no per-player scope) + this partition's
             * identical-seed RNG => every partition spawns the SAME car at the
             * SAME place; only the target slot differs. */
            s_trf_dyn_rng         = s_trf_dyn_rng_vp[vp];
            s_trf_spawn_partition = vp;   /* density gate counts only this partition */
            trf_dyn_effective_spawn_window(&spawn_lo, &spawn_hi);
            if (trf_dyn_spawn_in_window(pick, -1, spawn_lo, spawn_hi)) {
                s_trf_dyn_state[pick] = TRF_DYN_FADE_IN;
                s_trf_dyn_alpha[pick] = 0;
                s_trf_dyn_cooldown_vp[vp] = trf_dyn_spawn_period();
                g_cop_is_cop[pick] = 0; g_cop_phase[pick] = COP_IDLE; g_cop_target[pick] = -1;
            } else {
                s_trf_dyn_cooldown_vp[vp] = 10;   /* nothing placeable now — retry soon */
            }
            s_trf_spawn_partition = -1;
            s_trf_dyn_rng_vp[vp]  = s_trf_dyn_rng;   /* persist this partition's RNG */
            s_trf_scope_slot      = -1;
        }
        return;
    }

    /* Spawn cadence. Prefer slot 9 (the cop-capable slot) so speeding
     * pursuits can still trigger; round-robin the rest. */
    if (s_trf_dyn_cooldown > 0) s_trf_dyn_cooldown--;
    if (on_road < trf_dyn_cap() && s_trf_dyn_cooldown <= 0) {
        int pick = -1;
        if (9 >= t_base && 9 < t_end && s_trf_dyn_state[9] == TRF_DYN_INACTIVE)
            pick = 9;
        if (pick < 0) {
            for (int slot = t_base; slot < t_end; slot++) {
                if (s_trf_dyn_state[slot] == TRF_DYN_INACTIVE) { pick = slot; break; }
            }
        }
        static int s_trf_dyn_starved = 0;
        /* [task#13] Spawn FURTHER from the player: the per-tick window is scaled by
         * TD5RE_TRAFFIC_SPAWN_DIST (default 2x) so traffic appears in the distance
         * instead of popping in nearby. Race-init seeding (above) keeps the stock
         * close window — those cars were "always there" near the start. */
        int spawn_lo, spawn_hi;
        trf_dyn_effective_spawn_window(&spawn_lo, &spawn_hi);
        if (pick >= 0 &&
            trf_dyn_spawn_in_window(pick, -1, spawn_lo, spawn_hi)) {
            s_trf_dyn_state[pick] = TRF_DYN_FADE_IN;
            s_trf_dyn_alpha[pick] = 0;
            s_trf_dyn_cooldown = trf_dyn_spawn_period();
            s_trf_dyn_starved = 0;
            /* Police 1-in-CopRatio cadence (deterministic: counter advances on
             * each successful spawn, identical on every lockstep peer). A fresh
             * spawn is an ordinary traffic car; after CopRatio regular cars the
             * next spawn is flagged a cop (only when the POLICE option is on).
             * An IDLE cop cruises as normal traffic until a racer passes it. */
            g_cop_is_cop[pick] = 0;
            g_cop_phase[pick]  = COP_IDLE;
            g_cop_target[pick] = -1;
            if (g_encounter_enabled) {
                if (s_cop_spawn_counter >= g_td5.ini.cop_ratio) {
                    g_cop_is_cop[pick]  = 1;
                    s_cop_spawn_counter = 0;
                    TD5_LOG_I(LOG_TAG, "cop_spawn: slot=%d flagged COP (1 per %d cars)",
                              pick, g_td5.ini.cop_ratio);
                } else {
                    s_cop_spawn_counter++;
                }
            }
        } else {
            s_trf_dyn_cooldown = 10;   /* nothing placeable right now — retry soon */
            /* Persistent failure is a data/window problem worth surfacing
             * (e.g. all-slow surface stretch, track-end window, no clear
             * lanes). One line per ~10s of continuous starvation. */
            if ((++s_trf_dyn_starved % 30) == 0) {
                TD5_LOG_W(LOG_TAG,
                          "traffic_dyn_starved: %d consecutive spawn failures "
                          "(lead_span=%d ring=%d span_count=%d on_road=%d cap=%d)",
                          s_trf_dyn_starved, ai_player_span_lead(),
                          td5_track_get_ring_length(), td5_track_get_span_count(),
                          on_road, trf_dyn_cap());
            }
        }
    }
}

void td5_ai_update_traffic_route_plan(int slot) {
    int32_t *rs = route_state(slot);
    char *actor  = actor_ptr(slot);
    int  ref_slot;
    char *ref_actor;
    int32_t heading_shifted, route_byte_val, route_heading_shifted;
    int32_t hdelta, hdelta_neg;
    int polarity, peer;

    /* [FIX 2026-06-02 traffic-recovery-faithful — fix-1780404735]
     * REMOVED a non-original per-tick decrement of g_traffic_recovery_stage
     * that used to sit here. Exhaustive Ghidra search of TD5_d3d.exe confirms
     * the original (gActorTrafficRecoveryStage @ 0x4AFBE8) is NEVER decremented
     * anywhere — it is set 1..7 by Stage 2 (heading-misalignment, unconditional),
     * escalated on heavy collision (0x408215, saturating at 7), and cleared in
     * EXACTLY ONE place: RecycleTrafficActorFromQueue (0x4353B0) when the actor
     * falls >0x28 spans behind the player and is respawned ahead. The prior port
     * author's premise ("the orig has a per-tick countdown somewhere") was wrong
     * — the original relies on queue-recycle, not a countdown, to retire a
     * misaligned/stuck traffic actor. The invented decrement (plus the `== 0`
     * arming guard removed below) produced a non-faithful brake↔recover
     * oscillation at lane-change/junction headings instead of the original's
     * clean brake-until-recycle. Restoring the faithful behavior here. */

    /* --- Stage 1: Recycle --- */
    if (td5_ai_traffic_dynamic_active()) {
        /* [dynamic-traffic] The queue recycle is replaced by the distance
         * spawner (td5_ai_traffic_dynamic_tick, driven from td5_ai_pre_tick).
         * A parked (despawned) car has no route plan at all — hold the brake
         * and bail before any heading/recovery logic can run on its frozen
         * pose. */
        if (td5_ai_traffic_dynamic_parked(slot)) {
            ACTOR_U8(actor, ACTOR_BRAKE_FLAG) = 1;
            ACTOR_I16(actor, ACTOR_ENCOUNTER_STEER) = 0;
            return;
        }
    } else {
        td5_ai_recycle_traffic_actor();
    }

    /* [S20 AntiFreeze] un-stick a traffic car that the faithful recovery brake
     * has frozen and the player-relative recycle can't reach (parked player). */
    traffic_smart_antifreeze(slot, actor, rs);

    /* [S20 smart-traffic] periodic diagnostic dump (one slot, every ~300 frames)
     * so the before/after trace can confirm which behaviours actually fired. */
    if (g_td5.ini.traffic_smart && slot == g_traffic_slot_base &&
        (g_ai_frame_counter % 300u) == 0u) {
        TD5_LOG_I(LOG_TAG,
                  "traffic_smart_stat: react=%d ease_difflane=%d ease_lanechg=%d "
                  "single_lane=%d no_clear=%d slow_lane=%d wallnudge=%d",
                  s_smart_stat.react_calls, s_smart_stat.ease_diff_lane,
                  s_smart_stat.ease_lane_change, s_smart_stat.blocked_single_lane,
                  s_smart_stat.no_clear_lane, s_smart_stat.slow_lane_change,
                  s_smart_stat.wall_nudges);
    }

    /* --- Stage 2: Heading misalignment check ---
     * [CONFIRMED @ 0x00435EA6-0x00435FA8]
     * The original reads ref_slot = rs[RS_SLOT_INDEX] (EBX+0xD4), then
     * loads span_normalized + yaw_accum from actor_ptr(ref_slot). For traffic
     * ref_slot == param_1 so behaviour is the same, but the port follows the
     * original dispatch path. */
    ref_slot = rs[RS_SLOT_INDEX];
    if (ref_slot < 0 || ref_slot >= TD5_MAX_TOTAL_ACTORS) ref_slot = slot;
    ref_actor = actor_ptr(ref_slot);

    {
        const uint8_t *rb = (const uint8_t *)(intptr_t)rs[RS_ROUTE_TABLE_PTR];
        int16_t ref_span_norm = ACTOR_I16(ref_actor, ACTOR_SPAN_NORMALIZED);
        if (rb && ref_span_norm >= 0) {
            route_byte_val = (int32_t)rb[(size_t)(unsigned)ref_span_norm * 3u + 1u];
        } else {
            route_byte_val = 0;
        }
    }
    heading_shifted       = ACTOR_I32(ref_actor, ACTOR_YAW_ACCUM) >> 8;
    /* Original sequence at 0x435ED8-0x435EF2 computes
     *   tmp = route_byte * 0x102C
     *   tmp = (tmp + (tmp >> 31 & 0xFF)) >> 8
     * which is signed div-by-256 with arithmetic rounding toward zero. */
    {
        int32_t tmp = route_byte_val * 0x102C;
        route_heading_shifted = (tmp + ((tmp >> 31) & 0xFF)) >> 8;
    }
    /* Polarity = gActorRouteDirectionPolarity (dword 0x3F) per
     * UpdateTrafficRoutePlan @ 0x00435ef4. Prior port read dword 0x25 which
     * has no references in the original — the polarity branch was reading
     * a zero-init field that was never written. */
    polarity = rs[RS_ROUTE_DIRECTION_POLARITY];

    /* See 0x435EFD-0x435F18:
     *   ECX = (heading - route_heading) - 0x800           ; (signed)
     *   ECX = ECX & 0xFFF
     *   EAX = (ECX + 0xFFFFF800) & 0xFFF  ; = (ECX - 0x800) & 0xFFF
     *   EAX = (-EAX) & 0xFFF
     * which is the "negated heading delta" used in the polarity==0 test. */
    {
        uint32_t ecx = ((uint32_t)(heading_shifted - route_heading_shifted)) - 0x800u;
        uint32_t eax;
        ecx &= 0xFFFu;
        eax = (ecx - 0x800u) & 0xFFFu;
        eax = ((uint32_t)(-(int32_t)eax)) & 0xFFFu;
        hdelta_neg = (int32_t)eax;
    }

    if (polarity == 0) {
        hdelta = hdelta_neg;
    } else {
        /* 0x435F69: polarity != 0 path subtracts another 0x800 (= flip 180 deg). */
        hdelta = (int32_t)(((uint32_t)hdelta_neg - 0x800u) & 0xFFFu);
    }

    /* Original 0x435F2B JLE 0x400 + 0x435F32 JGE 0xC00 implement strict bounds:
     * body fires for hdelta in (0x400, 0xC00) exclusive. */
    if (hdelta > 0x400 && hdelta < 0xC00) {
        /* [FAITHFUL 0x435F2B] The original arms UNCONDITIONALLY here — no
         * `recovery_stage == 0` guard (the port-only guard was removed with the
         * decrement above). Re-arming each tick to a fresh (world_z & 7)|1 value
         * matches the original; the latch stays non-zero until queue-recycle. */
        /* Original at 0x435F34/0x435F81: reads DAT_004ab30c = actor[0].world_pos_z
         * (g_actor_base + 0x204). Used as a pseudo-random source for the
         * recovery stage. */
        int32_t seed_src = 0;
        if (g_actor_base) {
            seed_src = ACTOR_I32(actor_ptr(0), ACTOR_WORLD_POS_Z);
        }
        g_traffic_recovery_stage[slot] = (int)(seed_src & 7);
        if (g_traffic_recovery_stage[slot] == 0)
            g_traffic_recovery_stage[slot] = 1;
        /* Observability (fix-1780404735): rate-limited. The original re-arms
         * UNCONDITIONALLY every tick a car is in the (0x400,0xC00) heading band,
         * so a stuck (e.g. remap-miss / origin-placed) actor arms every frame —
         * log at most every 30 frames per the bail log to avoid flooding while
         * keeping the arm→brake→recycle cycle visible for diagnosis. */
        if ((g_ai_frame_counter % 30u) == 0u) {
            TD5_LOG_I(LOG_TAG,
                      "traffic_recovery_arm: slot=%d hdelta=0x%X polarity=%d stage=%d "
                      "span_raw=%d (brakes until recycle >0x28 spans behind player)",
                      slot, hdelta, polarity, g_traffic_recovery_stage[slot],
                      (int)ACTOR_I16(actor, ACTOR_SPAN_RAW));
        }
        /* Slot 9 special-encounter audio cleanup. */
        if (slot == 9) {
            if (polarity == 0) {
                if (!g_td5.wanted_mode_enabled) {
                    /* StopTrackedVehicleAudio @ 0x00440AE0 */
                    td5_sound_stop_tracked_vehicle_audio();
                }
                /* 0x435F5D: only the polarity==0 branch clears the handle. */
                g_encounter_tracked_handle = -1;
            } else {
                if (!g_td5.wanted_mode_enabled) {
                    /* StopTrackedVehicleAudio — handle NOT cleared in this branch. */
                    td5_sound_stop_tracked_vehicle_audio();
                }
            }
        }
    }

    /* --- Stage 3: Edge-of-track / recovery bail-out ---
     * [CONFIRMED @ 0x00435FAA-0x00436019]
     * Original condition (Ghidra):
     *   sVar5 = field_0x80 (ACTOR_SPAN_RAW)   (param_1 actor)
     *   bail if: ((sVar5 < 3 || g_trackTotalSpanCount - 8 <= sVar5) &&
     *             rs[RS_ROUTE_TABLE_SELECTOR] == 0)
     *         || rs[RS_SCRIPT_SPEED_PARAM] != 0   (DAT_004afc50)
     *         || g_traffic_recovery_stage[slot] != 0   (DAT_004afbe8) */
    {
        int16_t span_raw = ACTOR_I16(actor, ACTOR_SPAN_RAW);
        int ring_length = td5_track_get_ring_length();
        /* [dynamic-traffic OnCircuits] the near-edge brake is P2P track-END
         * logic (orig g_trackTotalSpanCount-8 guard); on a CIRCUIT spans
         * [ring-8..ring)+[0..3) are just the lap line and braking there would
         * stall every traffic car once per lap. Inert for faithful mode —
         * the original never has circuit traffic to begin with. */
        int near_edge = (g_td5.track_type != TD5_TRACK_CIRCUIT) &&
                        (span_raw < 3 || (ring_length > 0 && span_raw >= ring_length - 8));
        int on_canonical = (rs[RS_ROUTE_TABLE_SELECTOR] == 0);

        if ((near_edge && on_canonical)
            || rs[RS_SCRIPT_SPEED_PARAM] != 0
            || g_traffic_recovery_stage[slot] != 0) {
            /* LAB_004366c7: brake = 1, encounter_steer = 0, return */
            ACTOR_U8(actor, ACTOR_BRAKE_FLAG) = 1;
            ACTOR_I16(actor, ACTOR_ENCOUNTER_STEER) = 0;
            /* Observability (fix-1780404735): rate-limited so a latched actor
             * braking-until-recycle is visible without per-tick spam. Only logs
             * the recovery-latch cause (not the faithful near-edge/script bails). */
            if (g_traffic_recovery_stage[slot] != 0 &&
                (g_ai_frame_counter % 30u) == 0u) {
                TD5_LOG_I(LOG_TAG,
                          "traffic_recovery_bail: slot=%d stage=%d span_raw=%d "
                          "(braking, awaiting recycle)",
                          slot, g_traffic_recovery_stage[slot], (int)span_raw);
            }
            return;
        }
    }

    /* --- Stage 4: Normal driving -- cruise throttle (orig constant 0x3C=60) ---
     * S06 2026-06-04: derive the cruise throttle from the traffic vehicle's OWN
     * carparam top speed so different traffic cars move at car-appropriate speeds
     * instead of one difficulty-independent constant. The traffic dynamics are a
     * simplified force balance (no torque curve / +0x74 cap), so we scale the
     * emergent cruise command by the car's top-speed ratio vs a baseline, clamped
     * to a sane band. Gated by [GameOptions] AIAccelFromCar; falls back to the
     * faithful 0x3C when off or the carparam is unavailable. */
    int cruise = 0x3C;
    if (g_td5.ini.ai_accel_from_car) {
        int top = td5_physics_get_carparam_top_speed(slot);
        if (top > 0) {
            const int baseline_top = 950;   /* ~mid of the carparam top-speed range */
            int scaled = (0x3C * top) / baseline_top;
            if (scaled < 0x2A) scaled = 0x2A;   /* ~0.70x floor */
            if (scaled > 0x4E) scaled = 0x4E;   /* ~1.30x ceiling */
            cruise = scaled;
        }
    }
    /* [dynamic-traffic] cruise-speed scale ([Traffic] SpeedScale %, default
     * 150). The traffic dynamics (IntegrateVehicleFrictionForces port) feed
     * throttle = cruise*4 into a linear force balance, so the emergent top
     * speed tracks this scale. Applied BEFORE the SmartAI car-following ease
     * so queueing behind a slower car still slows the follower. */
    if (td5_ai_traffic_dynamic_active()) {
        cruise = (cruise * g_td5.ini.traffic_dyn_speed_pct) / 100;
        if (cruise > 0xB4) cruise = 0xB4;   /* 3x faithful — int16 cmd, sane ceiling */
    }
    /* [SmartAI] car-following: ease the cruise throttle when a car sits close
     * ahead in the same lane so traffic queues instead of nose-to-tail tunnelling.
     * Scope choice = opponents + traffic, so traffic shares the smart brain. */
    if (td5_ai_smart_active()) {
        int cscale = td5_ai_smart_traffic_cruise_scale(slot);
        cruise = (cruise * cscale) >> 8;
        if (cruise < 0) cruise = 0;
    }
    ACTOR_I16(actor, ACTOR_ENCOUNTER_STEER) = (int16_t)cruise;
    ACTOR_U8(actor, ACTOR_BRAKE_FLAG) = 0;

    /* RAY BRAIN for traffic: slow into bends and brake if aimed at a wall, using
     * the same forward ray sensor as the racers. Floors keep traffic rolling;
     * lane selection still comes from the S20 / smart_traffic_lane chooser. */
    if (td5_ai_smart_active() && g_td5.ini.smart_ai_rays) {
        int tsc = td5_track_get_span_count();
        if (tsc > 0) {
            int   tspan = (int)ACTOR_I16(actor, ACTOR_SPAN_RAW);
            float tsk   = td5_ai_smart_skill(slot);
            SmartCorner tco; smart_corner_eval(slot, tspan, tsc, 0.5, tsk, &tco);
            if (tco.speed_cap < 0.999) {
                int cc = (int)((double)cruise * tco.speed_cap);
                if (cc < 0x14) cc = 0x14;     /* keep traffic moving */
                cruise = cc;
                ACTOR_I16(actor, ACTOR_ENCOUNTER_STEER) = (int16_t)cruise;
            }
            SmartSense tse; smart_sense(slot, tspan, tsc, tsk, &tse);
            if (tse.wall_imminent) {
                ACTOR_U8(actor, ACTOR_BRAKE_FLAG)       = 1;
                ACTOR_I16(actor, ACTOR_ENCOUNTER_STEER) = 0;
                if ((g_ai_frame_counter % 90u) == 0u)
                    TD5_LOG_I(LOG_TAG, "smart_ray_traffic_wall: slot=%d span=%d front=%.0f",
                              slot, tspan, tse.front_clear);
            }
        }
    }

    /* --- Stage 5: Compute target span and deviation
     * [CONFIRMED @ 0x00435F0A-0x004364CA]
     *
     * Original UpdateTrafficRoutePlan computes LEFT/RIGHT deviation to the
     * next-span waypoint and writes them to route_state +0x58/+0x5c (=
     * RS_LEFT_DEVIATION/RS_RIGHT_DEVIATION). UpdateActorSteeringBias @
     * 0x004340C0 then reads these and writes steering_command (+0x30C).
     *
     * CRITICAL: the original calls InitActorTrackSegmentPlacement @ 0x00445F10
     * for the target point, NOT SampleTrackTargetPoint @ 0x00434800. These are
     * two DIFFERENT geometries:
     *   - InitActorTrackSegmentPlacement: 4-vertex barycenter of the sub_lane
     *     cell at (target_span, sub_lane_index). Uses DAT_00474E40 (spawn
     *     table). Traffic uses this. Port equivalent: td5_track_get_span_lane_world.
     *   - SampleTrackTargetPoint: route_byte interpolation between left rail
     *     and left+lane_count vertex of the whole span. Uses DAT_00473C68 (AI
     *     target table). AI racers use this. Port equivalent: td5_track_sample_target_point.
     *
     * The prior port routed traffic through sample_target_point, so traffic
     * aimed at the road center instead of its assigned sub-lane. On multi-lane
     * Moscow segments this pulled the chasing target into the opposite lane,
     * which drives the bicycle-model steering_command toward the neighbouring
     * rail and clips walls. The original also does NOT apply RS_TRACK_OFFSET_BIAS
     * (peer-avoidance perpendicular offset) to the traffic target — that is a
     * racer-only behaviour inside 0x00434800.
     *
     * Traffic peek-ahead is span+1 forward / span-1 reverse (racer uses span+4).
     *
     * BUG FIXED (2026-04-25): The original uses field_0x82 (ACTOR_SPAN_NORMALIZED)
     * for the jump-table lookup, NOT field_0x80 (ACTOR_SPAN_RAW). On branch roads
     * the raw span holds the branch index (>= ring_length) while the normalized
     * span is the main-road equivalent — using raw caused the remap walker to miss
     * every entry (branch indices are always outside the main-road range_lo values).
     * [CONFIRMED @ 0x00435F6A: Ghidra "iVar14 = field_0x82; iVar8 = iVar14 + 1"]
     *
     * BUG FIXED (2026-04-25): When the remap walker finds a branch match AND the
     * actor's raw span (field_0x80) is still on main road (< ring_length) AND the
     * branch target span is also on main road (<= ring_length), the original adjusts
     * sub_lane by (current_sub_lane - cur_span_lane_count). This accounts for the
     * lane-count difference between the junction span and the branch entry span so
     * that the traffic actor aims at the correct sub_lane cell.
     * [CONFIRMED @ 0x004361C0-0x004361D8: Ghidra "local_4 -= uVar6" path]
     *
     * Sub_lane ±1 adjustments at 0x004361B8/0x0043627C only fire on lane-count
     * mismatches between adjacent strips, which are rare. */
    {
        int16_t span_norm = ACTOR_I16(actor, ACTOR_SPAN_NORMALIZED); /* field_0x82 */
        int16_t span_raw  = ACTOR_I16(actor, ACTOR_SPAN_RAW);        /* field_0x80 */
        int span_count = td5_track_get_span_count();
        int ring_length = td5_track_get_ring_length();
        if (span_count > 0 && span_norm >= 0) {
            int lookup_span;    /* junction lookup span (norm-based, matches orig 0x00435F6A) */
            int fallback_span;  /* no-junction-match target (raw-based, matches orig 0x0043614c /
                                 * 0x004362C6 — `iVar8 = iVar10 + 1` / `iVar8 = iVar7 - 1` where
                                 * iVar10/iVar7 = span_raw). */
            if (polarity == 0) {
                /* [CONFIRMED @ 0x00435F6A] iVar14=field_0x82; iVar8 = iVar14 + 1 */
                lookup_span = ((int)span_norm + 1) % span_count;
                /* [CONFIRMED @ 0x0043614c] iVar10 = field_0x80 (span_raw); iVar8 = iVar10 + 1 */
                fallback_span = ((int)span_raw + 1) % span_count;
                if (fallback_span < 0) fallback_span += span_count;
            } else {
                lookup_span = (int)span_norm - 1;
                if (lookup_span < 0) lookup_span += span_count;
                /* [CONFIRMED @ 0x004362C6] iVar7 = field_0x80; iVar8 = iVar7 - 1 */
                fallback_span = (int)span_raw - 1;
                if (fallback_span < 0) fallback_span += span_count;
                if (fallback_span >= span_count) fallback_span -= span_count;
            }

            int is_canonical = (rs[RS_ROUTE_TABLE_SELECTOR] == 0);
            int remapped = td5_track_apply_target_span_remap(lookup_span,
                                                              is_canonical);
            /* [BUGFIX 2026-05-26 traffic-steer-saturation] When junction remap
             * does NOT fire (remapped == lookup_span), orig uses span_raw±1 as
             * target, NOT span_norm±1. Previously port used norm-based target
             * unconditionally; when actor's span_norm lagged span_raw (e.g. after
             * lap wrap or because normalize_wrap isn't called per-tick for traffic),
             * the target stayed anchored behind the actor's actual position →
             * deviation grew → steering cascade hit ±0x18000 emergency-snap and
             * saturated at -0x18000 within ~13s. Verified via Frida
             * tools/frida_traffic_compare.js: pre-fix slot 6 had 89.5% of ticks
             * at |steer|>50000 vs orig ~131 avg. */
            int remap_fired = (remapped != lookup_span);
            int target_span = remap_fired ? remapped : fallback_span;

            /* [RIGHT-BRANCH TRAFFIC FIX 2026-06-21 Part A] A car physically ON a
             * branch road (span_raw >= ring) overshoots the norm-based remap
             * target: span_norm lags span_raw there, so the target stays at the
             * car's own span and flips behind it -> ±0x18000 snap -> wall. Target
             * the ADJACENT branch span by raw so it advances WITH the car. Only
             * branch traffic; main-road cars keep the faithful norm-based remap. */
            int traffic_on_branch =
                branch_traffic_fix_enabled() && slot >= g_traffic_slot_base &&
                ring_length > 0 && (int)span_raw >= ring_length && span_count > 0;
            if (traffic_on_branch) {
                int adj = (polarity == 0) ? ((int)span_raw + 1)
                                          : ((int)span_raw - 1);
                if (adj < 0)           adj += span_count;
                if (adj >= span_count) adj -= span_count;
                target_span = adj;
            }

            /* Target sub_lane: start with current sub_lane (default path). */
            int target_sub_lane = (int)ACTOR_U8(actor, ACTOR_SUB_LANE_INDEX);

            /* [CONFIRMED @ 0x004361B8-0x004361D8]
             * When the remap found a branch target AND the actor is still on the
             * main road (raw < ring_length) AND the target is also within main road
             * bounds (target <= ring_length), adjust sub_lane by subtracting the
             * current span's lane count. This shifts the target cell to match
             * the branch entry sub_lane index.
             * Condition "iVar7 <= g_trackTotalSpanCount" in Ghidra uses <=, meaning
             * the branch target must be <= ring_length (inclusive, since ring_length
             * is the first branch index). */
            if (remap_fired &&                      /* junction remap fired */
                (int)span_raw < ring_length &&       /* actor on main road */
                target_span <= ring_length) {        /* target on/at branch edge */
                int cur_lane_count = td5_track_get_span_lane_count((int)span_norm);
                if (cur_lane_count > 0 && target_sub_lane >= cur_lane_count) {
                    target_sub_lane -= cur_lane_count;
                }
            }

            /* [S20 smart-traffic] AvoidSlowLane + situational lane change:
             * adjust the faithful target sub-lane toward a faster / clearer
             * lane. No-op when disabled / single-lane.
             *
             * [RIGHT-BRANCH TRAFFIC FIX 2026-06-21 Part C] Skip the chooser on a
             * branch road: it flip-flops the target sub-lane 0<->1 tick to tick,
             * and on a narrow (2-3 lane) branch that lateral jitter jerks the
             * 1-span-lookahead steering and the car fishtails into the rail. Hold
             * the car's current lane there; the wall-nudge below still keeps an
             * edge lane off the rail, and branches are short so we lose nothing. */
            if (!traffic_on_branch) {
                if (td5_ai_smart_active()) {
                    /* [SmartAI] unified lane brain for traffic: score lanes by
                     * surface/occupancy/wall/change-cost (±1 step). Replaces the
                     * S20 react-to-nearest-peer chooser. */
                    target_sub_lane = td5_ai_smart_traffic_lane(
                        slot, target_span,
                        td5_track_get_span_lane_count(target_span),
                        target_sub_lane, polarity);
                } else {
                    target_sub_lane = traffic_smart_choose_lane(
                        slot, target_span,
                        td5_track_get_span_lane_count(target_span), target_sub_lane);
                }
            }

            /* [RIGHT-BRANCH TRAFFIC FIX 2026-06-21] The held branch lane must be
             * valid for the raw-based target span (raw±1 may have a different
             * lane count than the car's current span). */
            if (traffic_on_branch) {
                int blc = td5_track_get_span_lane_count(target_span);
                if (blc > 0) {
                    if (target_sub_lane < 0)    target_sub_lane = 0;
                    if (target_sub_lane >= blc) target_sub_lane = blc - 1;
                }
            }

            /* [task#18 2026-06-12] TD6: keep traffic off the sidewalk. The lane
             * choosers above can pick any lane 0..lane_count-1, but on wide TD6
             * city strips only the central route band is paved. Clamp the target
             * lane into the drivable band derived from the route tables (no-op on
             * native TD5 and on branch spans without route coverage). */
            {
                int blo, bhi;
                if (td5_ai_td6_drivable_band(
                        target_span,
                        td5_track_get_span_lane_count(target_span),
                        &blo, &bhi)) {
                    if (target_sub_lane < blo) target_sub_lane = blo;
                    else if (target_sub_lane > bhi) target_sub_lane = bhi;
                }
            }

            int target_x = 0, target_y = 0, target_z = 0;

            if (td5_track_get_span_lane_world(target_span, target_sub_lane,
                                              &target_x, &target_y, &target_z)) {
                /* [S20 smart-traffic] WallAvoid: nudge an edge-lane target
                 * toward the lane interior so the car stops scraping the rail.
                 * Interior lanes are untouched. No-op when disabled. */
                traffic_smart_wall_nudge(
                    target_span, target_sub_lane,
                    td5_track_get_span_lane_count(target_span),
                    &target_x, &target_y, &target_z);
                /* [CONFIRMED @ 0x00436344-0x004363EF] Original re-reads
                 *   EAX = rs[RS_SLOT_INDEX] (= ref_slot)
                 *   ESI = ref_actor.world_pos_x   (offset 0x1FC, DAT_004ab304)
                 *   EAX = ref_actor.world_pos_z   (offset 0x204, DAT_004ab30c)
                 *   EBP = &ref_actor              (DAT_004ab108 + ref_slot*0x388)
                 * before computing target_angle and combined heading.
                 * For traffic ref_slot == slot, but follow the original
                 * dispatch verbatim. */
                int32_t actor_x = ACTOR_I32(ref_actor, ACTOR_WORLD_POS_X);
                int32_t actor_z = ACTOR_I32(ref_actor, ACTOR_WORLD_POS_Z);
                int32_t dx = (target_x - actor_x) >> 8;
                int32_t dz = (target_z - actor_z) >> 8;

                int32_t target_angle = ai_angle_from_vector(dx, dz) & 0xFFF;

                int32_t yaw   = ACTOR_I32(ref_actor, ACTOR_YAW_ACCUM);
                int32_t steer = ACTOR_I32(ref_actor, ACTOR_STEERING_CMD);
                int32_t actor_heading = ((yaw + steer) >> 8) & 0xFFF;

                /* [RIGHT-BRANCH TRAFFIC FIX 2026-06-21 Part B] Reject an
                 * ORPHAN/garbage target span whose lane geometry returns a point
                 * hundreds of thousands of units away (the dead spans between the
                 * main road and the branch dst ranges). A real 1-span lookahead is
                 * < ~6000u; past 20000u is bogus -> hold heading (delta 0 = steer
                 * straight) this tick so the car coasts across the transition
                 * instead of snapping the wheel to ±0x18000 into the wall. */
                if (branch_traffic_fix_enabled()) {
                    long md = (long)(dx < 0 ? -dx : dx) +
                              (long)(dz < 0 ? -dz : dz);
                    if (md > 20000) target_angle = actor_heading;
                }

                int32_t delta = actor_heading - target_angle;
                int32_t abs_delta = delta < 0 ? -delta : delta;
                if (delta >= 0) {
                    rs[RS_LEFT_DEVIATION]  = 0xFFF - abs_delta;
                    rs[RS_RIGHT_DEVIATION] = delta;
                } else {
                    rs[RS_LEFT_DEVIATION]  = abs_delta;
                    rs[RS_RIGHT_DEVIATION] = delta + 0xFFF;
                }

                if ((g_ai_frame_counter % 60u) == 0u) {
                    TD5_LOG_I(LOG_TAG,
                              "traffic_dev: slot=%d raw=%d norm=%d tspan=%d sublane=%d "
                              "ta=0x%X hd=0x%X delta=%d L=%d R=%d",
                              slot, (int)span_raw, (int)span_norm, target_span,
                              target_sub_lane, target_angle, actor_heading, delta,
                              rs[RS_LEFT_DEVIATION], rs[RS_RIGHT_DEVIATION]);
                }
            }
        }
    }

    /* --- Stage 6: Steering --- */
    td5_ai_update_steering_bias(rs, td5_ai_td6_steer_weight(0x8000));  /* [task#19] TD6 traffic too */

    /* --- Stage 7: Peer avoidance / yield --- */
    peer = td5_ai_find_nearest_route_peer(rs);

    /* [S20 smart-traffic] default: no lane change this tick (cleared unless a
     * close same-lane peer triggers react_to_peer below). */
    if (slot >= 0 && slot < TD5_MAX_TOTAL_ACTORS)
        s_traffic_lane_bias[slot] = 0;

    if (peer != slot) {
        /* Peer found — faithful TTC formula [CONFIRMED @ 0x4364E0–0x43656E] */
        char *peer_actor = actor_ptr(peer);
        int16_t self_span = ACTOR_I16(actor, ACTOR_SPAN_RAW);
        int16_t peer_span = ACTOR_I16(peer_actor, ACTOR_SPAN_RAW);
        int32_t self_speed, peer_speed, speed_delta;
        int32_t span_diff_dir; /* direction-adjusted span difference (raw spans) */
        int32_t iVar14;        /* combined progress delta: (spans * 0x100) */
        int32_t iVar7, iVar13; /* intermediate TTC and final TTC */
        int32_t speed_shifted; /* self_speed >> 10 with arithmetic rounding */
        int     smart_ease = 0; /* 1 = a clear lane exists; ease instead of hard brake */

        self_speed  = g_actor_forward_track_component[slot];
        peer_speed  = g_actor_forward_track_component[peer];
        speed_delta = self_speed - peer_speed; /* iVar8 in original */

        /* Direction-adjusted span difference */
        if (polarity == 0) {
            span_diff_dir = (int32_t)peer_span - (int32_t)self_span;
        } else {
            span_diff_dir = (int32_t)self_span - (int32_t)peer_span;
        }

        /* [S20 smart-traffic] Lookahead: when the nearest peer is close AHEAD in
         * our lane and a clear adjacent lane exists, set up a lane change (next
         * tick) and ease the hard brake instead of ramming/stopping.
         * span_diff_dir > 0 means the peer is in FRONT of us in our travel
         * direction — we only dodge what we can see. A peer BEHIND us
         * (span_diff_dir <= 0) is left to the faithful path: a driver can't see
         * behind, so traffic must not swerve for a car overtaking from the rear. */
        if (span_diff_dir > 0 && span_diff_dir < 8 && self_speed > 0) {
            smart_ease = traffic_smart_react_to_peer(
                slot, (int)self_span,
                (int)ACTOR_U8(actor, ACTOR_SUB_LANE_INDEX),
                td5_track_get_span_lane_count((int)self_span),
                peer, polarity);
        }

        /* Proximity gate [CONFIRMED @ 0x43646A]: if peer within 4 spans and moving, brake now.
         * [#18 2026-06-16 FIX] The faithful intent is "within 4 spans", but the port
         * checked only the LOWER bound (> -4) — so a peer up to ~30 spans AHEAD in the
         * same lane tripped this emergency brake, and traffic chain-stopped behind any
         * car ahead however far. Restore the near-zone upper bound; farther peers fall
         * through to the TTC formula below, which brakes only when actually closing.
         * Env TD5RE_TRAFFIC_BRAKE_NEAR (default 4) tunes the gate width. */
        if (span_diff_dir > -4 && span_diff_dir < trf_brake_near_spans() && self_speed > 0) {
            /* [traffic pile-up] A peer directly AHEAD within the minimum gap
             * overrides the smart-ease lane change: brake now and keep distance.
             * Easing here (brake off, gentle throttle) would roll us onto the peer
             * this tick because the lane change has a 1-tick latency — that is the
             * "traffic driving onto each other" the user reported. Once there is
             * room (gap > min), the ease/lane-change path resumes. */
            int too_close = (span_diff_dir > 0 && span_diff_dir <= trf_min_gap_spans());
            if (smart_ease && !too_close) {
                /* Clear lane found AND enough room — keep rolling (gentle throttle)
                 * while the lane bias above steers around the obstacle next tick. */
                ACTOR_U8(actor, ACTOR_BRAKE_FLAG) = 0;
                ACTOR_I16(actor, ACTOR_ENCOUNTER_STEER) = (int16_t)0x28; /* eased cruise */
            } else {
                ACTOR_U8(actor, ACTOR_BRAKE_FLAG) = 1;
                ACTOR_I16(actor, ACTOR_ENCOUNTER_STEER) = (int16_t)0xFF00; /* -256 */
                TD5_LOG_I(LOG_TAG, "ttc_brake: slot=%d proximity gate span_diff=%d too_close=%d",
                          slot, span_diff_dir, too_close);
            }
            goto ttc_done;
        }

        /* Combined progress delta (sub_progress approximated as 0) */
        iVar14 = span_diff_dir * 0x100;

        /* TTC formula [CONFIRMED @ 0x4364EE]:
         * iVar7 = ((iVar14 * peer_speed * 0x5DC) / speed_delta + iVar14 * 0x5DC) / self_speed
         * Right-shift by 8 with arithmetic rounding → iVar13 */
        if (speed_delta != 0 && self_speed != 0) {
            int64_t tmp = ((int64_t)iVar14 * peer_speed * 0x5DC) / speed_delta
                        + (int64_t)iVar14 * 0x5DC;
            iVar7  = (int32_t)(tmp / self_speed);
            iVar13 = (iVar7 + ((int32_t)iVar7 >> 31 & 0xFF)) >> 8;
        } else {
            iVar13 = 0x2EE00; /* no closing rate — far away */
        }

        /* self_speed arithmetic right-shift by 10 [CONFIRMED @ 0x436561] */
        speed_shifted = (self_speed + (self_speed >> 31 & 0x3FF)) >> 10;

        /* Brake condition [CONFIRMED @ 0x436561–0x43656E]:
         * ttc - 8 <= speed_shifted  &&  ttc >= 0  &&  self_speed > 0 */
        if (iVar13 - 8 <= speed_shifted && iVar13 > -1 && self_speed > 0) {
            /* [traffic pile-up] Same minimum-gap floor as the proximity gate: a
             * peer within the min gap directly ahead brakes regardless of a
             * found lane, so closing traffic keeps distance instead of overlapping. */
            int too_close = (span_diff_dir > 0 && span_diff_dir <= trf_min_gap_spans());
            if (smart_ease && !too_close) {
                /* [S20 smart-traffic] ease instead of hard brake — a clear lane
                 * exists and there is room, so slow gently and steer around next tick. */
                ACTOR_U8(actor, ACTOR_BRAKE_FLAG) = 0;
                ACTOR_I16(actor, ACTOR_ENCOUNTER_STEER) = (int16_t)0x28;
            } else {
                ACTOR_U8(actor, ACTOR_BRAKE_FLAG) = 1;
                ACTOR_I16(actor, ACTOR_ENCOUNTER_STEER) = (int16_t)0xFF00; /* -256 */
                TD5_LOG_I(LOG_TAG, "ttc_brake: slot=%d ttc=%d spd_shifted=%d", slot, iVar13, speed_shifted);
            }
        }
    }
ttc_done:;
}

/* ========================================================================
 * Special Encounter System
 *
 *   0x434DA0  UpdateSpecialTrafficEncounter
 *   0x434BA0  UpdateSpecialEncounterControl
 *
 * Slot 9 promotion: traffic slot 9 is "hijacked" when encounter conditions
 * are met. It switches from traffic AI to full racer AI.
 *
 * Lifecycle: spawn -> approach -> active -> teardown, 300-frame cooldown.
 * ======================================================================== */

/* ------------------------------------------------------------------
 * Byte-faithful port of 0x00434DA0 UpdateSpecialTrafficEncounter.
 *
 * Tracks acquisition and release of the special actor-9 traffic
 * encounter (cop / police chase NPC) in modes gated by DAT_0046320c.
 * This path is distinct from the wanted-mode flag DAT_004aaf68
 * (ENC_WANTED_MODE), which only suppresses the engine-audio
 * Start/Stop calls inside this function.
 *
 * Original-symbol mapping (TD5_d3d.exe @ 0x004XXXXX):
 *   DAT_004b05d8  gSpecialEncounterTrackedActorHandle  → g_encounter_tracked_handle
 *   DAT_004b055c  gSpecialEncounterRouteTable          → s_enc_route_table_ptr
 *   DAT_004b05c0  gSpecialEncounterTrackProgress       → s_enc_track_progress
 *   DAT_004b0580  gSpecialEncounterSignedTrackOffset   → s_enc_signed_track_offset
 *   DAT_004b05e4  encounter phase flag (0=ACQUIRE)     → g_encounter_phase_flag
 *   DAT_004b064c  g_specialEncounterCooldown           → g_encounter_cooldown
 *   DAT_004b0658  teardown secondary flag              → s_enc_teardown_flag
 *   DAT_004b0568  encounter route-table-selector ref   → g_encounter_route_table_selector
 *   DAT_004afbe0  gActorSpecialEncounterActive (RS+0x80, stride 0x11c)
 *                                                       → g_encounter_active[slot]
 *   DAT_004ab18a  actor[0].span_normalized  (+0x82)
 *   DAT_004ab41c  actor[0].longitudinal_speed (+0x314)
 *   DAT_004ad150  actor[9].span_raw         (+0x80)
 *   DAT_004ad152  actor[9].span_normalized  (+0x82)
 *   DAT_004ad2cc  actor[9].world_pos_x      (+0x1fc)  — ComputeTrackSpanProgress reads [x,_,z]
 *   DAT_004ad449  actor[9].vehicle_mode     (+0x379)
 *   DAT_004ad0d0  actor[9] base
 *   DAT_004afbc0  route_state[0][RS_FORWARD_TRACK_COMP] (slot 0)
 *   DAT_004afb6c  route_state[0][RS_ROUTE_TABLE_SELECTOR]
 *   DAT_004afb60  route_state[0][RS_ROUTE_TABLE_PTR]
 *   DAT_004aaf68  ENC_WANTED_MODE
 *
 * The port keeps g_encounter_active[] as a parallel int32 array (this
 * is what the rest of the port reads via td5_ai_is_encounter_active).
 * The original stores it inside the route_state row at RS+0x80; the
 * write effect is preserved logically.
 *
 * g_encounter_enabled is a port-only master gate fed from
 * g_td5.special_encounter_enabled. The original has no equivalent
 * gate at the function entry; the function is unconditionally called
 * by 0x00436A70 UpdateRaceActors, and the encounter system is
 * effectively idle when DAT_0046320c / DAT_004aaf68 are zero (no
 * cop slot configured). Keeping the gate here is a no-op when the
 * port mirrors the original's idle state but lets us hard-disable
 * encounters from frontend config.
 * ------------------------------------------------------------------ */

/* gSpecialEncounterRouteTable: pointer to the route-table byte stream
 * (LEFT.TRK / RIGHT.TRK) of the tracked actor. Refreshed every tick
 * when the tracked actor's RS_ROUTE_TABLE_PTR differs from the cached
 * value. Used as the source of the per-span lateral lane byte read
 * at offset (span_norm * 3). */
static const uint8_t *s_enc_route_table_ptr;

/* gSpecialEncounterTrackProgress: low dword of the ComputeTrackSpanProgress
 * return (longlong). The high dword is discarded by the original. */
static int32_t s_enc_track_progress;

/* gSpecialEncounterSignedTrackOffset: low dword of ComputeSignedTrackOffset
 * (ulonglong) return. */
static int32_t s_enc_signed_track_offset;

/* _DAT_004b0658: secondary teardown flag. Zeroed on the >0x40 despawn
 * path; not read elsewhere in this function. */
static int32_t s_enc_teardown_flag;

/* External symbols mapped from absolute addresses in the disassembly. */
/* DAT_004aaf68 ENC_WANTED_MODE mirror in the port lives at
 * g_td5.wanted_mode_enabled (int). Wrap it for byte-faithful naming. */
#define ENC_WANTED_MODE  (g_td5.wanted_mode_enabled)

/* Audio Start/Stop hooks — wire to ported StartTrackedVehicleAudio
 * (0x00440AB0) / StopTrackedVehicleAudio (0x00440AE0) in td5_sound.c. */
static inline void td5_enc_start_tracked_audio(int arg) {
    TD5_LOG_I(LOG_TAG, "enc_audio_start: arg=%d", arg);
    td5_sound_start_tracked_vehicle_audio(arg);
}

static inline void td5_enc_stop_tracked_audio(void) {
    TD5_LOG_I(LOG_TAG, "enc_audio_stop");
    td5_sound_stop_tracked_vehicle_audio();
}

/* Forward declaration: the deterministic-sim crash-recovery state reset
 * for actor[9]. Original is ResetVehicleActorState @ 0x00405d70.
 * Port: td5_physics.c:td5_physics_reset_actor_state(TD5_Actor *). */
extern void td5_physics_reset_actor_state(TD5_Actor *actor);

/**
 * UpdateSpecialTrafficEncounter — byte-faithful port of 0x00434DA0.
 * Called from UpdateRaceActors (0x00436A70) at end of traffic loop.
 */
/* ========================================================================
 * Multi-cop chase scheduler (rewrite 2026-06-19) — replaces the original
 * single-cop, slot-9 `UpdateSpecialTrafficEncounter`. Runs once per sim tick
 * after the racer + traffic loops; deterministic (no RNG, only replicated
 * actor state) so it is lockstep-safe in netplay.
 *
 * Per cop (any traffic slot flagged at spawn):
 *   IDLE     -> acquire the first un-pursued racer that just passed it.
 *   CHASING  -> drive at the target (cop_drive); end on overtake / too-far /
 *               target-broke-down. (Driving is in ai_update_single_traffic.)
 *   PULLOVER -> force the overtaken target to brake to a full stop, then end.
 * The "force the target to a stop" is signalled via g_encounter_active[target]
 * (consumed by the human input path + the AI racer path).
 * ======================================================================== */
/* [DEBUG 2026-06-24] Cops normally chase HUMAN players only (state 1). This
 * default-OFF knob also lets a chase trigger on AI-driven racers (state 0) so
 * the chase/dodge/durability behaviour can be exercised headlessly (PlayerIsAI
 * runs, where slot 0 is AI). Never enabled in normal play. */
static int cop_chase_ai_debug(void) {
    static int s = -1;
    if (s < 0) { const char *e = getenv("TD5RE_COP_CHASE_AI"); s = (e && e[0] == '1') ? 1 : 0; }
    return s;
}
void td5_ai_update_special_encounter(void) {
    int span_count, ring_len, t_base, t_end, racer_n;

    /* POLICE option off -> no chases (and the cosmetic siren/glow gate on the
     * same g_td5.special_encounter_enabled, so cops are silent + invisible). */
    if (!g_encounter_enabled) return;
    if (!g_actor_base) return;

    span_count = td5_track_get_span_count();
    ring_len   = td5_track_get_ring_length();
    if (ring_len <= 0) ring_len = span_count;

    t_base = g_traffic_slot_base;
    t_end  = g_active_actor_count;
    if (t_end > t_base + TD5_MAX_TRAFFIC_SLOTS) t_end = t_base + TD5_MAX_TRAFFIC_SLOTS;
    if (t_end > TD5_MAX_TOTAL_ACTORS) t_end = TD5_MAX_TOTAL_ACTORS;

    /* Racer candidate slots = players + AI opponents (0 .. traffic base). */
    racer_n = g_traffic_slot_base;
    if (racer_n > g_active_actor_count) racer_n = g_active_actor_count;
    if (racer_n > TD5_MAX_TOTAL_ACTORS) racer_n = TD5_MAX_TOTAL_ACTORS;

    for (int k = t_base; k < t_end; k++) {
        char *cop;
        int   cop_main, phase, tgt, gap;

        if (!g_cop_is_cop[k]) continue;
        /* [#3] Age the per-cop re-chase cooldown (deterministic; one tick/cop). */
        if (s_cop_rebust_ticks[k] > 0) s_cop_rebust_ticks[k]--;
        if (td5_ai_traffic_dynamic_parked(k)) continue;   /* not on the road */

        /* A cop that itself broke down (crashed) abandons its chase + releases
         * any pull-over hold on its target. */
        if (g_actor_broken_down[k]) {
            if (g_cop_phase[k] != COP_IDLE) {
                int bt = g_cop_target[k];
                if (g_cop_phase[k] == COP_PULLOVER &&
                    bt >= 0 && bt < TD5_MAX_TOTAL_ACTORS)
                    g_encounter_active[bt] = 0;
                cop_end_chase(k);
            }
            continue;
        }

        cop = actor_ptr(k);
        /* Fold branch-corridor spans onto the main ring so cop/target gaps are
         * comparable even when one of them is on a branch. */
        cop_main = td5_track_branch_to_main_span((int)ACTOR_I16(cop, ACTOR_SPAN_RAW));
        phase    = g_cop_phase[k];

        if (phase == COP_IDLE) {
            /* Cruise normally during the post-chase cooldown (don't immediately
             * re-acquire the car it just chased). */
            if (g_cop_cooldown[k] > 0) { g_cop_cooldown[k]--; continue; }
            /* Acquire the FIRST (lowest-slot = deterministic) racer that just
             * passed this cop and isn't already pursued by another cop. */
            int chosen = -1;
            for (int c = 0; c < racer_n; c++) {
                char *cand;
                int   st, cgap;
                /* [MULTI-COP 2026-06-29] (removed) The old
                 *   `if (g_actor_pursued_by[c] != -1) continue;  // no double-chase`
                 * guard let only ONE cop chase a given car. It is gone now: pass
                 * a cluster of cops and they ALL acquire you. A target carries as
                 * many pursuers as there are cops in range. */
                /* [#3] This cop busted this car recently -> leave it alone until
                 * its re-chase cooldown lapses (a DIFFERENT cop can still acquire
                 * it immediately, and this cop resumes after the cooldown). */
                if (cop_no_rechase_enabled() && s_cop_rebust_ticks[k] > 0 &&
                    s_cop_rebust_target[k] == (int8_t)c)
                    continue;
                st = g_slot_state[c];
                /* Cops chase HUMAN players only (state 1), never AI opponents.
                 * TD5RE_COP_CHASE_AI (debug, default off) also allows AI racers
                 * (state 0) so the chase is observable headlessly. */
                if (st != 1 && !(cop_chase_ai_debug() && st == 0)) continue;
                if (g_actor_broken_down[c]) continue;
                cand = actor_ptr(c);
                /* Line of sight: a cop can't START a chase across a fork. If the
                 * player is on a branch corridor and the cop is on the main road
                 * (or vice-versa) the cop can't see them, so don't acquire. (An
                 * already-running chase still follows the player onto branches.)
                 * Branch corridor spans are >= the main-ring length. */
                if (((int)ACTOR_I16(cop,  ACTOR_SPAN_RAW) >= ring_len) !=
                    ((int)ACTOR_I16(cand, ACTOR_SPAN_RAW) >= ring_len))
                    continue;
                cgap = smart_span_gap(cop_main,
                          td5_track_branch_to_main_span((int)ACTOR_I16(cand, ACTOR_SPAN_RAW)),
                          ring_len);
                /* "passed by" = now just ahead of the cop (1..COP_TRIGGER_HI
                 * spans), moving forward, above the speeding threshold. */
                if (cgap < COP_TRIGGER_LO || cgap > COP_TRIGGER_HI) continue;
                if (ACTOR_I32(cand, ACTOR_LONGITUDINAL_SPEED) <= g_td5.ini.cop_min_speed) continue;
                if (g_actor_forward_track_component[c] <= 0) continue;
                chosen = c;
                break;
            }
            if (chosen >= 0) {
                g_cop_target[k]            = (int8_t)chosen;
                /* [MULTI-COP 2026-06-29] No pursuer lock written — multiple cops
                 * may target the same car; td5_ai_actor_is_pursued() scans cops. */
                g_cop_phase[k]             = COP_CHASING;
                /* [R3-12] Arm the no-progress watchdog for this fresh chase. */
                s_cop_best_gap[k]          = 0x7FFFFFFF;
                s_cop_stall_ticks[k]       = 0;
                /* [#6] Start the chase spin-up: drive as a normal AI racer first
                 * (corner-safe) before the catch-up sprint, so the cop doesn't
                 * floor itself into a wall the instant the chase begins. */
                s_cop_chase_ticks[k]       = 0;
                /* If the cop was ONCOMING traffic (facing the wrong way), swing
                 * it around to the race direction so it gives chase cleanly
                 * instead of doing a reverse-arc: clear the oncoming polarity and
                 * re-seed its heading forward from the track. Forward-facing cops
                 * are left untouched. */
                if (route_state(k)[RS_ROUTE_DIRECTION_POLARITY] != 0) {
                    route_state(k)[RS_ROUTE_DIRECTION_POLARITY] = 0;
                    td5_track_compute_heading((TD5_Actor *)cop);
                    /* Drop the backward (oncoming) momentum so it accelerates
                     * forward cleanly instead of sliding backward while facing
                     * forward. */
                    ACTOR_I32(cop, ACTOR_LIN_VEL_X) = 0;
                    ACTOR_I32(cop, ACTOR_LIN_VEL_Z) = 0;
                }
                TD5_LOG_I(LOG_TAG, "cop_chase START: cop=%d target=%d", k, chosen);
            }
            continue;
        }

        /* CHASING or PULLOVER — validate the target, then maintain. */
        tgt = g_cop_target[k];
        if (tgt < 0 || tgt >= racer_n) { cop_end_chase(k); continue; }

        /* End: the chased car broke down (crashed into traffic/car/wall). */
        if (g_actor_broken_down[tgt]) {
            if (phase == COP_PULLOVER) g_encounter_active[tgt] = 0;
            TD5_LOG_I(LOG_TAG, "cop_chase END (target broke down): cop=%d target=%d", k, tgt);
            cop_end_chase(k);
            continue;
        }

        {
            char *tactor   = actor_ptr(tgt);
            int   tgt_main = td5_track_branch_to_main_span((int)ACTOR_I16(tactor, ACTOR_SPAN_RAW));
            gap = smart_span_gap(cop_main, tgt_main, ring_len);  /* >0 = target ahead of cop */

            /* End: target escaped. [COP CATCH-UP 2026-06-29] Was the 65-span
             * traffic-despawn metric, which made a cop quit the moment the player
             * opened a straight-line lead. cop_chase_max_gap() (default 110 spans)
             * gives the cop room to stay on the player and close on the next
             * corner. The no-progress stall watchdog below still ends a chase the
             * cop truly can't close. */
            if (smart_iabs(gap) > cop_chase_max_gap()) {
                if (phase == COP_PULLOVER) g_encounter_active[tgt] = 0;
                TD5_LOG_I(LOG_TAG, "cop_chase END (too far): cop=%d target=%d gap=%d", k, tgt, gap);
                cop_end_chase(k);
                continue;
            }

            if (phase == COP_CHASING) {
                /* [R3-12] No-progress watchdog: track the smallest |gap| achieved
                 * toward the target. Each tick that betters the best by
                 * COP_STALL_IMPROVE spans resets the stall timer; otherwise it
                 * counts up. A cop looping a branch keeps driving but never closes
                 * -> the timer runs out and the chase is abandoned (the cop's
                 * ordinary route plan then walks it back onto the main ring). This
                 * is in ADDITION to the too-far end above, which only catches a
                 * cop falling behind, not one orbiting at a fixed distance. */
                if (cop_stall_end_enabled()) {
                    int agap = smart_iabs(gap);
                    if (agap <= s_cop_best_gap[k] - COP_STALL_IMPROVE) {
                        s_cop_best_gap[k]    = agap;
                        s_cop_stall_ticks[k] = 0;
                    } else if (++s_cop_stall_ticks[k] >= COP_STALL_TICKS) {
                        TD5_LOG_I(LOG_TAG,
                            "cop_chase END (no progress %d ticks, looping/stuck): "
                            "cop=%d target=%d gap=%d best=%d",
                            (int)s_cop_stall_ticks[k], k, tgt, gap, s_cop_best_gap[k]);
                        cop_end_chase(k);
                        continue;
                    }
                }
                /* Overtake: the cop must STAY ahead (gap <= -margin) for a sustained
                 * window before it forces the target to pull over. A brief draw-level
                 * on a swerve no longer instantly brakes the target, and the cop keeps
                 * pursuing through the hold rather than giving up the moment it noses
                 * ahead. The counter resets whenever the target is level/ahead again,
                 * so only a held lead counts. [#4 2026-06-20] */
                if (gap <= -COP_OVERTAKE_MARGIN) {
                    if (++s_cop_overtake_ticks[k] >= cop_overtake_hold_ticks()) {
                        g_cop_phase[k]          = COP_PULLOVER;
                        g_encounter_active[tgt] = 1;
                        s_cop_overtake_ticks[k] = 0;
                        TD5_LOG_I(LOG_TAG,
                            "cop_chase OVERTAKE (held %d ticks): cop=%d target=%d -> PULLOVER",
                            cop_overtake_hold_ticks(), k, tgt);
                    }
                } else {
                    s_cop_overtake_ticks[k] = 0;   /* target level/ahead again -> reset hold */
                }
                continue;
            }

            /* COP_PULLOVER: hold the target braked until it is fully stopped. */
            {
                int32_t tspeed = ACTOR_I32(tactor, ACTOR_LONGITUDINAL_SPEED);
                int32_t tabs   = tspeed < 0 ? -tspeed : tspeed;
                g_encounter_active[tgt] = 1;     /* keep forcing the stop */
                if (tabs <= COP_STOP_SPEED) {
                    g_encounter_active[tgt] = 0;
                    /* [#3] Successful bust ("execution done correctly"): this cop
                     * leaves THIS car alone for the re-chase cooldown. */
                    if (cop_no_rechase_enabled() && tgt >= 0) {
                        s_cop_rebust_target[k] = (int8_t)tgt;
                        s_cop_rebust_ticks[k]  = (int16_t)cop_rebust_cooldown();
                    }
                    TD5_LOG_I(LOG_TAG, "cop_chase COMPLETE (target stopped): cop=%d target=%d rebust_cd=%d", k, tgt, (int)s_cop_rebust_ticks[k]);
                    cop_end_chase(k);
                } else if (gap > COP_OVERTAKE_MARGIN) {
                    /* Target slipped back ahead of the cop -> resume the pursuit. */
                    g_encounter_active[tgt] = 0;
                    g_cop_phase[k] = COP_CHASING;
                }
            }
        }
    }
}

#if 0  /* Legacy single-cop encounter (slot 9) — superseded by the multi-cop
        * scheduler above. Kept disabled for reference during bring-up. */
void td5_ai_update_special_encounter_legacy(void) {
    int32_t handle;
    char *cop;        /* actor[9] */
    int32_t cop_span_norm;     /* actor[9] +0x82 (cop is always traffic slot 9) */

    /* Port-only master gate (see header comment). */
    if (!g_encounter_enabled) return;

    /* Bind frequently-used addresses once. */
    if (!g_actor_base) return;
    cop = actor_ptr(9);

    cop_span_norm = (int32_t)(int16_t)ACTOR_I16(cop, ACTOR_SPAN_NORMALIZED);

    handle = g_encounter_tracked_handle;

    /* ---- Prologue (0x00434da0–0x00434e11) ----------------------------
     * if (handle != -1 && cached_route_table_ptr !=
     *     route_state[handle][RS_ROUTE_TABLE_PTR]) {
     *     // route changed — refresh cached pointer + recompute
     *     // span progress + signed track offset.
     * }
     * ----------------------------------------------------------------- */
    if (handle != -1) {
        const uint8_t *cur_rb;
        const int32_t *handle_rs = route_state(handle);
        cur_rb = (const uint8_t *)(intptr_t)handle_rs[RS_ROUTE_TABLE_PTR];
        if (s_enc_route_table_ptr != cur_rb) {
            int32_t cop_span_raw = (int32_t)(int16_t)ACTOR_I16(cop, ACTOR_SPAN_RAW);
            int32_t *cop_xyz     = (int32_t *)(cop + ACTOR_WORLD_POS_X);
            uint8_t lane_byte;

            s_enc_route_table_ptr = cur_rb;

            /* longlong ComputeTrackSpanProgress(int span, int *xyz_at_+0x1fc) */
            s_enc_track_progress = (int32_t)td5_track_compute_span_progress(
                cop_span_raw, cop_xyz);

            /* The original passes the per-span lane byte at
             *   route_table[ span_norm * 3 ]
             * i.e. the BL = byte ptr [ECX + EDX*1] where ECX=span_norm,
             * EDX = base + span_norm*2 ⇒ effective addr = base + span_norm*3. */
            lane_byte = (cur_rb && cop_span_norm >= 0)
                ? cur_rb[(size_t)cop_span_norm * 3u]
                : 0;

            /* ulonglong ComputeSignedTrackOffset(int span_raw, int progress, uint lane) */
            s_enc_signed_track_offset = (int32_t)td5_track_compute_signed_offset(
                cop_span_raw, s_enc_track_progress, (int)lane_byte);
        }
    }

    /* ---- Phase-flag dispatch (0x00434e13–0x00434e26) -----------------
     * if (phase_flag != 0) {
     *     if (handle == -1) return;
     *     goto active_monitor;            // 0x00434f1b
     * }
     * if (handle != -1) goto active_monitor;
     * // else: try to spawn the encounter
     * ----------------------------------------------------------------- */
    if (g_encounter_phase_flag != 0) {
        if (handle == -1) return;
        /* fall through to active_monitor */
    } else if (handle == -1) {
        /* ---- Spawn attempt (0x00434e2c–0x00434f11) ------------------
         * PORT DEVIATION (multiplayer): the original latches the encounter
         * onto slot 0 (the only human). Here we scan EVERY human slot
         * [0 .. num_human_players) and latch the FIRST one that passes the
         * full gate, so the cop chases whichever player drives past it
         * first — and only that car (the active monitor + per-slot
         * g_encounter_active gating below confine the effect to that slot).
         * With a single player this loops once over slot 0, reproducing the
         * original byte-for-byte. The cop is always traffic slot 9 (special-
         * encounter is only enabled for fields of <=6 racers, where slot 9
         * is free). */
        int n_players;
        int spawn_slot = -1;
        int16_t spawn_span_norm = 0;
        int32_t spawn_speed = 0, spawn_fwd = 0, spawn_selector = 0;

        /* Cooldown is a single shared timer — check once. */
        if (g_encounter_cooldown != 0) {
            if ((g_ai_frame_counter % 90u) == 0u) {
                TD5_LOG_I(LOG_TAG, "cop_spawn_gate: BLOCK=cooldown!=0 (%d)",
                          g_encounter_cooldown);
            }
            return;
        }

        n_players = g_td5.num_human_players;
        if (n_players < 1) n_players = 1;
        if (n_players > g_traffic_slot_base) n_players = g_traffic_slot_base;

        for (int ps = 0; ps < n_players; ps++) {
            char   *pl       = actor_ptr(ps);
            int16_t ps_span  = ACTOR_I16(pl, ACTOR_SPAN_NORMALIZED);
            int32_t ps_speed = ACTOR_I32(pl, ACTOR_LONGITUDINAL_SPEED);
            int32_t ps_fwd   = g_actor_forward_track_component[ps];
            int32_t ps_sel   = route_state(ps)[RS_ROUTE_TABLE_SELECTOR];
            /* (int16)player.span_norm - (int16)cop.span_norm == 2 */
            int32_t sd       = (int32_t)ps_span - cop_span_norm;

            /* [DIAG] near-miss log — cop (slot 9) within 1..3 spans of this
             * player (the window in which span_delta crosses the required ==2),
             * plus a periodic slot-0 heartbeat. Zero behavioral effect. */
            if ((sd >= 1 && sd <= 3) || (ps == 0 && (g_ai_frame_counter % 90u) == 0u)) {
                const char *blocker =
                    (sd != 2)                                     ? "span_delta!=2" :
                    (ps_speed <= 0x15638)                         ? "speed<=0x15638" :
                    (ps_fwd <= 0)                                 ? "fwd<=0" :
                    (ps_sel != g_encounter_route_table_selector)  ? "selector!=0" :
                                                                    "PASSES(will spawn)";
                TD5_LOG_I(LOG_TAG,
                          "cop_spawn_gate[p%d]: BLOCK=%s span_delta=%d "
                          "(player_norm=%d cop9_norm=%d) speed=%d(>0x15638?%d) "
                          "fwd=%d(>0?%d) selector=%d(==%d?%d)",
                          ps, blocker, sd, (int)ps_span, cop_span_norm,
                          ps_speed, ps_speed > 0x15638, ps_fwd, ps_fwd > 0,
                          ps_sel, g_encounter_route_table_selector,
                          ps_sel == g_encounter_route_table_selector);
            }

            /* actor[ps].span_norm - actor[9].span_norm == 2 */
            if (sd != 2) continue;
            /* actor[ps].long_speed > 0x15638 (JLE → fail at 0x15638) */
            if (ps_speed <= 0x15638) continue;
            /* RS[ps].forward_track_component > 0 (JLE → fail at 0) */
            if (ps_fwd <= 0) continue;
            /* RS[ps].route_table_selector == DAT_004b0568 (JNZ → fail) */
            if (ps_sel != g_encounter_route_table_selector) continue;

            /* The original's final abs(player_span - player_span) > 0x10 check
             * is vacuously true (always 0); dropped — no behavioral effect. */

            /* First qualifying player wins the chase. */
            spawn_slot      = ps;
            spawn_span_norm = ps_span;
            spawn_speed     = ps_speed;
            spawn_fwd       = ps_fwd;
            spawn_selector  = ps_sel;
            break;
        }

        if (spawn_slot < 0) return;

        /* ===== SPAWN =====
         * Original (0x00434e97–0x00434f11) latches handle=0 (slot 0 = player)
         * and caches route_state[0][RS_ROUTE_TABLE_PTR]. We use spawn_slot so
         * the chase tracks the winning player; the active monitor below is
         * already handle-generic, so it follows the right car automatically. */
        {
            int32_t cop_span_raw = (int32_t)(int16_t)ACTOR_I16(cop, ACTOR_SPAN_RAW);
            int32_t *cop_xyz     = (int32_t *)(cop + ACTOR_WORLD_POS_X);
            const uint8_t *rb0;
            uint8_t lane_byte;

            g_encounter_tracked_handle = spawn_slot;

            s_enc_track_progress = (int32_t)td5_track_compute_span_progress(
                cop_span_raw, cop_xyz);

            /* IMPORTANT: the original reads the lane byte from the *previous*
             * gSpecialEncounterRouteTable, then immediately overwrites it.
             * (0x434eb8 reads MOV EDX,[0x4b055c] BEFORE 0x434eea writes.) */
            rb0 = s_enc_route_table_ptr;
            lane_byte = (rb0 && cop_span_norm >= 0)
                ? rb0[(size_t)cop_span_norm * 3u]
                : 0;

            s_enc_signed_track_offset = (int32_t)td5_track_compute_signed_offset(
                cop_span_raw, s_enc_track_progress, (int)lane_byte);

            /* Cache the tracked player's route_table_ptr. Since the tracked
             * actor IS spawn_slot after this, the prologue "ptr changed" check
             * will be false on the very next tick. */
            s_enc_route_table_ptr = (const uint8_t *)(intptr_t)
                                    route_state(spawn_slot)[RS_ROUTE_TABLE_PTR];

            td5_physics_reset_actor_state((TD5_Actor *)cop);
        }

        TD5_LOG_I(LOG_TAG,
                  "Encounter spawn: handle=%d player_span_norm=%d cop_span_norm=%d "
                  "speed=%d fwd=%d sel=%d",
                  spawn_slot, (int)spawn_span_norm, cop_span_norm, spawn_speed,
                  spawn_fwd, spawn_selector);

        if (ENC_WANTED_MODE != 0) return;
        td5_enc_start_tracked_audio(9);
        return;
    }

    /* ---- Active monitor (0x00434f1b–0x00434fd3) -----------------------
     * Reached when (handle != -1). Either phase == 0 fall-through or
     * phase != 0 explicit JNZ.
     *
     * if (cooldown != 0) return;
     * cop_span = (int16)actor[handle].span_norm  // +0x82 with stride 0x388
     * delta = cop_span - DAT_004ad152            // DAT_004ad152 = actor[9].span_norm
     *
     * Note: handle is set to 0 at spawn (slot 0 = player), so
     * "actor[handle].span_norm" reads the player's span_norm and
     * delta = player_span_norm - cop_span_norm.
     *
     * if (delta > 0x40 && actor[9].vehicle_mode == 0) {
     *     teardown:
     *       teardown_flag = 0
     *       tracked_handle = -1
     *       phase_flag = 0
     *       cooldown = 300
     *       if (ENC_WANTED_MODE == 0) StopTrackedVehicleAudio();
     *       ResetVehicleActorState(&actor[9]);
     *       return;
     * }
     *
     * // delta <= 0x40 path:
     * ecx = DAT_004ad152 - cop_span (= cop_span_norm - player_span_norm)
     * if (ecx < 2) return;
     * if (route_state[handle][RS_ROUTE_TABLE_SELECTOR] != [0x4b0568]) return;
     * if (actor[9].vehicle_mode != 0) return;
     * route_state[handle][0x20] = 1;             // per-slot encounter active
     * if (ENC_WANTED_MODE == 0) StopTrackedVehicleAudio();
     * ----------------------------------------------------------------- */
    {
        char *tracked_actor;
        int32_t tracked_span_norm;
        int32_t delta;
        uint8_t cop_mode;

        if (g_encounter_cooldown != 0) return;

        tracked_actor     = actor_ptr(handle);
        tracked_span_norm = (int32_t)(int16_t)ACTOR_I16(tracked_actor, ACTOR_SPAN_NORMALIZED);
        delta             = tracked_span_norm - cop_span_norm;
        cop_mode          = ACTOR_U8(cop, ACTOR_VEHICLE_MODE);

        if (delta > 0x40 && cop_mode == 0) {
            /* Teardown — cop is far ahead of player AND cop mode == 0. */
            s_enc_teardown_flag        = 0;
            g_encounter_tracked_handle = -1;
            g_encounter_phase_flag     = 0;
            g_encounter_cooldown       = 0x12c;   /* 300 */

            if (ENC_WANTED_MODE == 0) {
                td5_enc_stop_tracked_audio();
            }

            td5_physics_reset_actor_state((TD5_Actor *)cop);

            TD5_LOG_I(LOG_TAG,
                      "Encounter despawn (delta>0x40): tracked_span=%d cop_span=%d "
                      "delta=%d cooldown=300",
                      tracked_span_norm, cop_span_norm, delta);

            /* The original does NOT clear per-slot g_encounter_active
             * flags or zero the control latches here — UpdateSpecial
             * EncounterControl (0x00434BA0) handles deactivation when
             * it next runs and finds the conditions invalid. */
            return;
        }

        /* delta <= 0x40 path — encounter activation check. */
        {
            int32_t ecx;
            const int32_t *tracked_rs;

            ecx = cop_span_norm - tracked_span_norm;
            if (ecx < 2) return;

            tracked_rs = route_state(handle);
            if (tracked_rs[RS_ROUTE_TABLE_SELECTOR] !=
                g_encounter_route_table_selector) return;

            if (cop_mode != 0) return;

            /* route_state[handle][0x20] = 1.
             * Mirror to the parallel g_encounter_active[] used by the
             * rest of the port. handle is guaranteed in [0, TD5_MAX_TOTAL_ACTORS). */
            g_encounter_active[handle] = 1;

            if (ENC_WANTED_MODE == 0) {
                td5_enc_stop_tracked_audio();
            }

            TD5_LOG_I(LOG_TAG,
                      "Encounter activate: handle=%d ecx=%d tracked_span=%d cop_span=%d",
                      (int)handle, ecx, tracked_span_norm, cop_span_norm);
        }
    }
}
#endif  /* legacy single-cop encounter */

/**
 * UpdateSpecialEncounterControl  (0x00434BA0)
 *
 * Byte-faithful port of the original __cdecl function at 0x00434BA0.
 *
 * Two yaw-delta computations:
 *   Block 1: uses gActorTrackReferenceSlot[slot*0x47] which == slot (set
 *            by InitializeRaceActorRuntime via rs[RS_SLOT_INDEX] = i), so
 *            it reads YAW/SPAN_NORMALIZED from actor_ptr(slot) and the
 *            route table from rs[RS_ROUTE_TABLE_PTR] of the slot.
 *   Block 2: uses g_specialEncounterTrackedActorHandle (typically the
 *            player target, slot 0) and the cached gSpecialEncounterRouteTable
 *            pointer (updated each tick by UpdateSpecialTrafficEncounter).
 *
 * Each angle delta is folded to a 12-bit modular value via the orig idiom:
 *     d = (((diff - 0x800) & 0xFFF) - 0x800) & 0xFFF
 *     uVarN = (-d) & 0xFFF
 * The 0x400 < uVarN <= 0xC00 alignment band is symmetric around the route
 * heading: any angle inside the band counts as "facing the wrong way" and
 * forces teardown.
 *
 * Fast-path writes target actor_ptr(slot), NOT actor_ptr(9). DAT_004ad152
 * is the player's SPAN_NORMALIZED snapshot — it gates the brake band.
 */
/* Pull-over control (rewrite 2026-06-19): when the multi-cop scheduler has
 * flagged `slot` as being pulled over (g_encounter_active[slot] != 0, set when
 * a cop overtakes it), force that car to brake to a full stop. Called for
 * HUMAN players from the input path (td5_input.c); AI-racer targets get the
 * same stop applied in ai_update_single_racer. No-op otherwise. Replaces the
 * faithful single-cop heading/brake control (kept below under #if 0). */
void td5_ai_update_encounter_control(int slot) {
    char *actor_self;
    if (slot < 0 || slot >= TD5_MAX_TOTAL_ACTORS) return;
    if (!g_encounter_active[slot]) return;
    actor_self = actor_ptr(slot);
    /* Stop command: brake on, drive command = idle/brake (0xFF00), throttle
     * off, wheel centred. Physics decays the speed to rest (mirrors
     * ai_apply_stop_command / the finished-slot brake set). */
    ACTOR_U8(actor_self,  ACTOR_BRAKE_FLAG)      = 1;
    ACTOR_I16(actor_self, ACTOR_ENCOUNTER_STEER) = (int16_t)0xFF00;
    ACTOR_U8(actor_self,  ACTOR_THROTTLE_STATE)  = 0;
    ACTOR_I32(actor_self, ACTOR_STEERING_CMD)    = 0;
}

#if 0  /* Legacy single-cop encounter control — superseded by the pull-over
        * control above. Kept disabled for reference during bring-up. */
void td5_ai_update_encounter_control_legacy(int slot) {
    char    *actor_self   = actor_ptr(slot);
    int32_t *rs_self      = route_state(slot);
    int      tracked      = g_encounter_tracked_handle;
    /* DAT_004ad152 in orig = player's SPAN_NORMALIZED snapshot; UpdateSpecialTrafficEncounter
     * keeps it equal to the current player's field_0x82 each tick. Port reads it live.
     * PORT DEVIATION (multiplayer): read the span of the TRACKED (chased) player, not a
     * hardcoded slot 0, so the brake gate matches single-player when the chased car is
     * not slot 0. For a single player tracked==0, so this is byte-identical. */
    int      span_ref_slot = (tracked >= 0 && tracked < TD5_MAX_TOTAL_ACTORS) ? tracked : 0;
    int32_t  player_span_ref = (int32_t)(int16_t)ACTOR_I16(actor_ptr(span_ref_slot), ACTOR_SPAN_NORMALIZED);
    /* gSpecialEncounterRouteTable in orig = cached route-table pointer of the tracked actor.
     * Port reads the live equivalent from the tracked actor's rs[RS_ROUTE_TABLE_PTR]. */
    const uint8_t *tracked_route_tbl = (tracked >= 0 && tracked < TD5_MAX_TOTAL_ACTORS)
        ? (const uint8_t *)(intptr_t)route_state(tracked)[RS_ROUTE_TABLE_PTR]
        : NULL;
    const uint8_t *self_route_tbl = (const uint8_t *)(intptr_t)rs_self[RS_ROUTE_TABLE_PTR];

    int32_t fwd_comp;
    uint32_t uVar3, uVar1;

    /* --- Block 1: yaw delta of slot's own actor vs slot's route-table heading --- */
    {
        int32_t yaw_self = ACTOR_I32(actor_self, ACTOR_YAW_ACCUM) >> 8;  /* SAR EAX,8 (signed) */
        int16_t span_self = ACTOR_I16(actor_self, ACTOR_SPAN_NORMALIZED); /* MOVSX [+0x82] */
        uint32_t byte_val = 0;
        int32_t heading_off, diff;

        if (self_route_tbl && span_self >= 0) {
            /* byte at route_tbl + span*3 + 1 (XOR EBX,EBX; MOV BL,[...] = unsigned byte). */
            byte_val = (uint32_t)self_route_tbl[(size_t)(uint16_t)span_self * 3u + 1u];
        }
        /* heading_offset = (byte_val * 0x102C) >>s 8.  byte_val is unsigned [0,255], so
         * the CDQ/AND/ADD/SAR idiom in the orig reduces to a plain >>8 (product is
         * always non-negative). */
        heading_off = (int32_t)(byte_val * 0x102Cu) >> 8;
        diff = yaw_self - heading_off;
        diff = (diff - 0x800) & 0xFFF;
        diff = (diff - 0x800) & 0xFFF;
        uVar3 = ((uint32_t)(-diff)) & 0xFFF;
    }

    /* --- Block 2: yaw delta of tracked actor vs cached gSpecialEncounterRouteTable --- */
    {
        int32_t yaw_tr = 0;
        int16_t span_tr = 0;
        uint32_t byte_val = 0;
        int32_t heading_off, diff;

        if (tracked >= 0 && tracked < TD5_MAX_TOTAL_ACTORS) {
            char *actor_tr = actor_ptr(tracked);
            yaw_tr  = ACTOR_I32(actor_tr, ACTOR_YAW_ACCUM) >> 8;
            span_tr = ACTOR_I16(actor_tr, ACTOR_SPAN_NORMALIZED);
        }
        if (tracked_route_tbl && span_tr >= 0) {
            byte_val = (uint32_t)tracked_route_tbl[(size_t)(uint16_t)span_tr * 3u + 1u];
        }
        heading_off = (int32_t)(byte_val * 0x102Cu) >> 8;
        diff = yaw_tr - heading_off;
        diff = (diff - 0x800) & 0xFFF;
        diff = (diff - 0x800) & 0xFFF;
        uVar1 = ((uint32_t)(-diff)) & 0xFFF;
    }

    fwd_comp = g_actor_forward_track_component[slot];

    /* Set latches (orig writes these unconditionally before the conditional teardown). */
    g_encounter_control_active_latch = 1;
    g_encounter_steer_bias_latch     = 0xFF00; /* word write (MOV [...], DX where DX=0xFF00) */

    /* Teardown if any of:
     *   - fwd_comp <= 8
     *   - 0x400 <= uVar3 <= 0xC00   (range [JL→0x434ccd] ; [JLE→teardown])
     *   - 0x400 <= uVar1 <= 0xC00
     */
    if (fwd_comp <= 8) goto teardown;
    if (!((int32_t)uVar3 < 0x400 || (int32_t)uVar3 > 0xC00)) goto teardown;
    if (!((int32_t)uVar1 < 0x400 || (int32_t)uVar1 > 0xC00)) goto teardown;

    /* Active branch:
     *   if fwd_comp >= gSpecialEncounterMinForwardTrackComponentThreshold (orig DAT_004b05bc)
     *      and  (DAT_004ad152 - actor[slot].SPAN_NORMALIZED) < 3
     *      → BRAKE_FLAG = 1; ENCOUNTER_STEER = 0xFF00
     *   else → ENCOUNTER_STEER = 0  (BRAKE_FLAG untouched)
     */
    {
        int32_t span_self = (int32_t)(int16_t)ACTOR_I16(actor_self, ACTOR_SPAN_NORMALIZED);
        int32_t span_delta = player_span_ref - span_self;
        if (fwd_comp >= g_special_encounter_min_fwd_track_threshold && span_delta < 3) {
            ACTOR_U8(actor_self,  ACTOR_BRAKE_FLAG)      = 1;
            ACTOR_I16(actor_self, ACTOR_ENCOUNTER_STEER) = (int16_t)0xFF00;
            return;
        }
        ACTOR_I16(actor_self, ACTOR_ENCOUNTER_STEER) = 0;
    }
    return;

teardown:
    /* Orig: XOR EAX,EAX
     *       MOV [ESI+0x4afbe0],EAX   (gActorSpecialEncounterActive[slot] = 0)
     *       MOV [ESI+0x4afc5c],EAX   (gActorRouteDirectionPolarity[slot] = 0)
     *       MOV [0x4ad40e],AX        (steer_bias_latch = 0, word)
     *       MOV [0x4ad43d],AL        (control_active_latch = 0, byte)
     *       MOV [0x4b05c4],EAX       (DAT_004b05c4 = 0)
     *       MOV [0x4b05e4],EAX       (DAT_004b05e4 = 0)
     *       MOV [0x4b05d8],-1        (tracked-handle alias; port writes g_encounter_tracked_handle)
     *       MOV [0x4b064c],300       (g_specialEncounterCooldown)
     *       MOV CL,[actor[slot].+0x384]; INC CL; MOV [actor[slot].+0x384],CL  (byte increment)
     *
     * AUDIT(audit-encounter-dats, 2026-05-14): the three unlabeled DATs are
     * fully accounted for by the existing port state:
     *
     *   DAT_004b05d8 = gSpecialEncounterTrackedActorHandle
     *     → port's g_encounter_tracked_handle. Cleared to -1 on the next line
     *       below; verified label exists in Ghidra (symbol_by_name match).
     *
     *   DAT_004b05e4 = g_encounter_phase_flag (port mirror exists already).
     *     The original has only two writers, both to 0 (this teardown +
     *     UpdateSpecialTrafficEncounter teardown at 0x00434f6d), and a single
     *     reader at 0x00434e13 testing == 0. The variable is always 0 in
     *     steady-state, so this redundant zero-write here is observationally
     *     identical to omitting it. Port matches.
     *
     *   DAT_004b05c4 — write-only DEAD variable in the original. reference_to
     *     finds exactly 1 reference, this MOV [0x4b05c4],EAX (always 0). No
     *     reader anywhere in TD5_d3d.exe. Likely a vestigial latch whose
     *     consumer was optimized out at build time. Omitting from the port
     *     has zero behavioral effect.
     */
    g_encounter_active[slot] = 0;
    /* gActorRouteDirectionPolarity = rs[0xFC/4 = 0x3F] per UpdateSpecialEncounter-
     * Control teardown @ 0x00434d40 `MOV [ESI + 0x4afc5c], EAX`. Only dword 0x3F
     * is written by the original; the prior defensive write to dword 0x25
     * (RS_DIRECTION_POLARITY_LEGACY) targeted a field with no references in the
     * original listing and is removed. */
    rs_self[RS_ROUTE_DIRECTION_POLARITY] = 0;
    g_encounter_steer_bias_latch = 0;
    g_encounter_control_active_latch = 0;
    g_encounter_tracked_handle = -1;
    g_encounter_cooldown = 300;
    /* [DELIBERATE DIVERGENCE fix-1780404735 — close stuck-siren gap]
     * The original UpdateSpecialEncounterControl teardown (0x434BA0) does NOT
     * stop the tracked-vehicle (siren) audio — RE-confirmed. The siren is
     * normally killed earlier by the ACTIVATE transition in
     * UpdateSpecialTrafficEncounter (0x434DA0). But if an encounter ends via
     * THIS teardown (player slowed -> fwd_comp<=8, or the heading-misalignment
     * band) WITHOUT having activated first, the siren is never stopped and keeps
     * playing while the cop drives away — matching the user report "siren stuck
     * despite the cop being far away". The gap exists in the original too;
     * stopping here closes it. SAFE: td5_sound_stop_tracked_vehicle_audio is a
     * no-op when the siren is already stopped (s_tracked_veh_active==0), so the
     * common already-activated path is unaffected (just begins a fade that's
     * already at zero). Gated to the special encounter (ENC_WANTED_MODE==0); the
     * Cop-Chase-mode siren is the separate user horn-toggle path. */
    if (ENC_WANTED_MODE == 0) {
        td5_enc_stop_tracked_audio();
        TD5_LOG_I(LOG_TAG,
                  "encounter_control teardown: siren stop (close stuck-siren gap, slot=%d)",
                  slot);
    }
    /* Byte-wide increment (orig uses CL via INC). Field at +0x384 is the encounter
     * completion counter on the slot's own actor (NOT on the player). */
    ACTOR_U8(actor_self, ACTOR_ENCOUNTER_STATE) =
        (uint8_t)(ACTOR_U8(actor_self, ACTOR_ENCOUNTER_STATE) + 1);
}
#endif  /* legacy single-cop encounter control */

/* ========================================================================
 * Master Dispatcher  (0x436A70  UpdateRaceActors)
 *
 * Called every simulation tick. Iterates all active actor slots and
 * dispatches to the appropriate update function based on slot type:
 *
 *   Slots 0-5: Racers
 *     - state 0x00 (AI):       UpdateActorTrackBehavior + UpdateVehicleActor
 *     - state 0x01 (player):   skip AI (handled by input/physics)
 *     - state 0x02 (finished): brake behavior
 *
 *   Slots 6-11: Traffic
 *     - slot 9 with encounter: UpdateActorTrackBehavior (hijacked to racer AI)
 *     - all others:            UpdateTrafficRoutePlan + UpdateTrafficActorMotion
 *
 *   After traffic loop: UpdateSpecialTrafficEncounter
 * ======================================================================== */

/* [CONFIRMED @ 0x00436A70] L5 promotion sweep audit (2026-05-18).
 *
 * UpdateRaceActors — 1569 bytes / 450 instructions / 270 decompiled lines.
 * Master per-sub-tick dispatcher with FOUR contiguous regions:
 *   A: rubber-band call → ComputeAIRubberBandThrottle (0x00432D60).
 *   B: per-racer track-state loop (branch detection + forward component +
 *      span progress + offset-clamp + deviation write).
 *   C/C': non-drag vs drag-mode racer dispatch (state==0/2/3 branches).
 *   D/D': non-drag vs drag-mode traffic dispatch (slots 6-11 + slot-9
 *      special-encounter handling + UpdateSpecialTrafficEncounter call).
 *
 * SHIPPED FIXES (in master):
 *   - 3-path route_table_ptr selector (commit 2412baf, 1a95b4c):
 *     PATH 1 (selector=1, ptr=RIGHT), PATH 2a (selector=0, ptr=LEFT),
 *     PATH 2b (default — ptr UNCHANGED). Closes Moscow 34→20 (sub_tick=1).
 *   - F1 traffic bias-clamp slot gate (commit 79023df).
 *   - Steering pre-AI clear placement (commit 79023df / e6622f2 cluster).
 *   - State-2 finished encounter_steer = (int16_t)0xFF00 — bit-equivalent
 *     to orig's WORD 0xFF00 (D5 audited MATCH).
 *
 * KNOWN DIVERGENCES (re/analysis/pilot_00436A70_audit.md):
 *   D1     Branch detection table walker collapsed in port's
 *          `td5_ai_refresh_route_state_slot` — partial port via 3-path
 *          selector (2412baf), but underlying DAT_004C3DA0 walker still
 *          simplified. PARTIAL CLOSE via commit 2412baf.
 *   D2     Per-actor track-state work (gActorTrackSpanProgress / Render
 *          TrackSegmentNearActor / UpdateActorTrackBounds /
 *          ClassifyTrackOffsetClamp / GetTrackSegmentSurfaceType /
 *          recovery-stage override / branch-aware deviation block) lives
 *          in `td5_track_recompute_actor_offsets` rather than inline in
 *          this dispatcher. PER-TICK TIMING may lag by one sub-tick.
 *   D3     Forward-track-component formula uses (>>12) instead of orig's
 *          FILD+FSQRT+IDIV by sqrt(sin²+cos²) — sub-LSB magnitude.
 *   D4     State-2 (finished) 5-byte cluster zero-fill at +0x371..+0x376
 *          not yet written by port.
 *   D6-D8  Loop / slot iteration LOW-impact differences (audited).
 *
 * KNOWN TODO CHAIN OWNERS:
 *   - todo_cascade_unwind_2026-05-17.md (RS_TRACK_PROGRESS quotient/
 *     remainder cluster).
 *
 * Audit reference: re/analysis/pilot_00436A70_audit.md (pool13, 2026-05-14).
 * Effective level: L4 (multiple shipped fixes; D1/D2 are partial-port
 * residuals tracked as cascade-unwind work).
 */
void td5_ai_update_race_actors(void) {
    int i;

    /* Age broken-down traffic/cops (parks them + drives the smoke) every tick,
     * independent of the POLICE gate. */
    cop_tick_broken_down();

    /* --- Step 1: Rubber-band already computed in td5_ai_tick --- */
    td5_ai_refresh_route_state();

    if ((g_ai_frame_counter % 60u) == 0u) {
        for (i = 0; i < g_active_actor_count; i++) {
            char *actor = actor_ptr(i);
            TD5_LOG_D(LOG_TAG,
                      "AI state: slot=%d steering=%d throttle=%d route_span=%d mode=%d",
                      i,
                      ACTOR_I32(actor, ACTOR_STEERING_CMD),
                      g_live_throttle[i],
                      ACTOR_I16(actor, ACTOR_SPAN_RAW),
                      g_slot_state[i]);
        }
    }

    /* --- Step 2: Update racers (slots 0 through min(count, 6)) --- */
    {
        int racer_count = g_traffic_slot_base;
        if (g_active_actor_count < racer_count)
            racer_count = g_active_actor_count;

        for (i = 0; i < racer_count; i++) {
            ai_update_single_racer(i);
        }
    }

    /* --- Step 3: Update traffic (slots 6-11) if active --- */
    if (g_active_actor_count > g_traffic_slot_base) {
        int traffic_max = g_active_actor_count;
        if (traffic_max > TD5_MAX_TOTAL_ACTORS)
            traffic_max = TD5_MAX_TOTAL_ACTORS;

        for (i = g_traffic_slot_base; i < traffic_max; i++) {
            ai_update_single_traffic(i);
        }

        /* --- Step 4: Special encounter check --- */
        td5_ai_update_special_encounter();
    }

}

/* [item#2 2026-06-15] A/B knob: stop AI cars once a CIRCUIT race is over.
 * On a circuit the race ends when every human has finished; unfinished AI no
 * longer block (td5_game.c check_race_completion) and were left in slot-state 0,
 * so td5_ai_update_track_behavior kept driving them around the loop during the
 * post-finish fade / results hold (user: "on circuit track AI should stop when
 * completing the lap"). With TD5RE_AI_FINISH_STOP on, an AI racer still in
 * state 0 cuts throttle and brakes the moment the race is complete on a circuit,
 * so it coasts to rest instead of continuing to lap. (AI that finished its own
 * laps already flips to slot-state 2, which brakes via the case below — this
 * covers the cars that DIDN'T finish before the race ended.) Off → byte-identical
 * (AI keeps lapping). Only affects AI actors, only on circuit tracks, only after
 * the race-end latch. Logged once on first read. */
static int ai_finish_stop_enabled(void)
{
    static int s = -1;
    if (s < 0) {
        const char *e = getenv("TD5RE_AI_FINISH_STOP");
        s = (e && e[0] == '0') ? 0 : 1;
        TD5_LOG_I(LOG_TAG, "ai_finish_stop knob: TD5RE_AI_FINISH_STOP=%d "
                  "(stop AI on circuit race finish %s)", s, s ? "ON" : "OFF");
    }
    return s;
}

/* True when the current race is a finished CIRCUIT race — the post-finish latch
 * (race_end_fade_state > 0) is set and the track is a closed loop. AI racers
 * that are still driving (slot-state 0) should brake to a stop here. */
static int ai_circuit_race_finished(void)
{
    return ai_finish_stop_enabled() &&
           g_track_is_circuit &&
           g_td5.race_end_fade_state > 0;
}

/* Coast an AI racer to a stop: cut throttle, brake on, centre the wheel. Mirrors
 * the finished-slot (state 2) brake command set so the physics integrator decays
 * the speed to rest. Physics reads encounter_steering_cmd (+0x33E) as the drive
 * command (0xFF00 = -0x100 = brake/idle, cf. case 0x02 / route-threshold T0). */
static void ai_apply_stop_command(char *actor)
{
    ACTOR_I16(actor, ACTOR_ENCOUNTER_STEER) = (int16_t)0xFF00;
    ACTOR_U8(actor,  ACTOR_BRAKE_FLAG)      = 1;
    ACTOR_U8(actor,  ACTOR_THROTTLE_STATE)  = 0;
    ACTOR_I32(actor, ACTOR_STEERING_CMD)    = 0;
}

static void cop_drive(int slot);            /* defined below */
static void mp_cop_pick_target(int cop_slot); /* defined below */

/**
 * Dispatch a single racer slot based on its state.
 */
static void ai_update_single_racer(int slot) {
    char *actor = actor_ptr(slot);
    int state = g_slot_state[slot];

    /* [MP COP CHASE 2026-06-23] An AI-driven cop racer (g_cop_is_cop, COP_CHASING)
     * hunts the suspects via the dedicated cop driver (aggressive ram steering)
     * instead of the normal racer AI. The live per-tick loop (td5_ai_update_race_
     * actors) calls THIS function for racer slots, so the dispatch MUST live here —
     * td5_ai_update_actor (where it used to be) is never called. This is why the
     * cop drove like a regular racer. */
    if (g_cop_is_cop[slot] && g_cop_phase[slot] == COP_CHASING &&
        g_td5.mp_mode_config.mode == TD5_MP_MODE_COP_CHASE && !g_td5.network_active) {
        mp_cop_pick_target(slot);
        cop_drive(slot);
        return;
    }

    switch (state) {
    case 0x00: /* AI racer */
        /* [POLICE rewrite] An AI racer being pulled over by a cop (the scheduler
         * set g_encounter_active[slot]) is forced to brake to a stop, the same
         * way the human input path forces a human target via
         * td5_ai_update_encounter_control. */
        if (g_encounter_active[slot]) {
            ai_apply_stop_command(actor);
            return;
        }
        /* Wanted mode gate [CONFIRMED @ UpdateRaceActors 0x436E1D]:
         * Original: run UpdateActorTrackBehavior only when
         *   (g_wantedModeEnabled == 0) || (gWantedDamageStateTable[slot] != 0)
         * Slot 0 is the player and uses a separate path in the original, so
         * skip only cop slots (slot != 0) to keep PlayerIsAI working. */
        if (g_td5.wanted_mode_enabled && slot != 0 &&
            g_wanted_damage_state[slot] == 0) {
            return; /* cop arrested — AI frozen */
        }

        /* Drag race: synthetic full-throttle straight-line driver.
         *
         * Original [CONFIRMED @ 0x0042AC85-AC97 + 0x00436FA0..0x0043703F]:
         * spawns slots 1..5 as state=3 (decoration) and the drag-mode
         * dispatcher in UpdateRaceActors skips UpdateVehicleActor for
         * state==3. So the original has no AI driver in drag — slots
         * 1..5 are static cosmetic cars at the strip start.
         *
         * Port diverges at td5_game.c:849-857 (decoration_start=2) to give
         * SP a real 2-car race. That leaves slot 1 at state=0 (AI), so
         * physics ticks but no command writer runs (UpdateActorTrackBehavior
         * is gated off here to mirror original drag mode). Physics reads
         * encounter_steering_cmd (+0x33E) as throttle [CONFIRMED @
         * td5_physics.c:830] — left at 0, slot idles.
         *
         * Drag strip is a single straight span: pin full throttle, zero
         * steer, no brake. Once slot 1 crosses the finish, state flips
         * to 0x02 (finished) and the brake branch below takes over. */
        if (g_td5.drag_race_enabled) {
            ACTOR_I16(actor, ACTOR_ENCOUNTER_STEER) = (int16_t)0xFF;
            ACTOR_I32(actor, ACTOR_STEERING_CMD) = 0;
            ACTOR_U8(actor, ACTOR_BRAKE_FLAG) = 0;
            if ((g_td5.simulation_tick_counter % 60u) == 0u) {
                TD5_LOG_I(LOG_TAG,
                    "drag_ai_drive: slot=%d throttle=0xFF steer=0 brake=0",
                    slot);
            }
            return;
        }

        /* [item#2] Circuit race over: stop this still-driving AI instead of
         * lapping the finished circuit. Gated TD5RE_AI_FINISH_STOP (default on),
         * circuit-only, post-finish only. Slot 0 (PlayerIsAI / a human's empty
         * slot) is included — it's an AI driver here too. */
        if (ai_circuit_race_finished()) {
            ai_apply_stop_command(actor);
            if ((g_td5.simulation_tick_counter % 60u) == 0u) {
                TD5_LOG_I(LOG_TAG,
                    "ai_finish_stop: slot=%d circuit race over -> brake to stop",
                    slot);
            }
            return;
        }

        td5_ai_update_track_behavior(slot);
        /* [AI UNSTICK] If this AI racer is wedged against another car, override
         * the racing-line throttle/steer with a short steer-away burst so it
         * peels off instead of grinding forever. Port-only; no-op when disabled
         * or when the car is making normal forward progress. */
        racer_collision_escape(slot);
        /* UpdateVehicleActor would follow here in the physics module */
        break;

    case 0x01: /* Player: skip AI update */
        break;

    case 0x02: /* Finished/dead: brake behavior
                * [precise-port pool13 0x00436A70: state-2 dispatch] */
        {
            int32_t fwd = g_actor_forward_track_component[slot];
            if (fwd < 0) {
                /* [0x00436EE0-E4] Moving backward: word ptr [ESI] = 0
                 * ESI = &actor.+0x33E. Clear encounter_steer = 0. */
                ACTOR_I16(actor, ACTOR_ENCOUNTER_STEER) = 0;
            } else {
                /* [0x00436ED5-DD] Moving forward: brake_flag = 1,
                 * encounter_steer = 0xFF00 (== (int16_t)-0x100). */
                ACTOR_U8(actor, ACTOR_BRAKE_FLAG) = 1;
                ACTOR_I16(actor, ACTOR_ENCOUNTER_STEER) = (int16_t)(-0x100);
            }
            /* [precise-port D4 fix from pilot_00436A70_audit]
             * Original [0x00436EE5-F5] unconditionally writes a 5-byte
             * cluster after the fwd-comp branch:
             *   MOV byte ptr [ESI+0x36], 0xff  ; actor+0x374
             *   MOV byte ptr [ESI+0x35], 0xff  ; actor+0x373
             *   MOV byte ptr [ESI+0x34], 0xff  ; actor+0x372
             *   MOV byte ptr [ESI+0x33], 0xff  ; actor+0x371
             *   MOV byte ptr [ESI+0x38], 0x00  ; actor+0x376
             * ESI = &actor.+0x33E so +0x33..0x38 = actor offsets 0x371..0x376.
             * Port previously skipped these; the cluster is faithful to the
             * listing's unconditional path (both branches above fall through
             * to it via JMP 0x00436EE5). */
            ACTOR_U8(actor, 0x371) = 0xff;
            ACTOR_U8(actor, 0x372) = 0xff;
            ACTOR_U8(actor, 0x373) = 0xff;
            ACTOR_U8(actor, 0x374) = 0xff;
            ACTOR_U8(actor, 0x376) = 0x00;
        }
        break;
    }
}

/**
 * Dispatch a single traffic slot.
 * Slot 9 is hijacked to racer AI when encounter is active.
 */
/* [#18] A/B knob: TD5RE_TD6_TRAFFIC_ROAD=0 disables the road-lane clamp. */
static int td6_traffic_road_clamp_enabled(void) {
    static int s = -1;
    /* Default OFF: the per-tick lane nudge oscillated traffic near forks and
     * tripped the recovery brake ("most don't move"). Master's lane chooser
     * already avoids slow lanes; =1 re-enables the nudge. */
    if (s < 0) { const char *e = getenv("TD5RE_TD6_TRAFFIC_ROAD"); s = (e && e[0] == '1') ? 1 : 0; }
    return s;
}

/* Drive a traffic slot as ordinary traffic: route plan + (TD6) road-lane clamp
 * + jam-escape. Factored out of ai_update_single_traffic so a CHASING cop can
 * reuse the SAME road-following driver (see cop_drive). */
static void traffic_drive_normal(int slot) {
    td5_ai_update_traffic_route_plan(slot);

    if (g_active_td6_level > 0 && td6_traffic_road_clamp_enabled()) {
        char *a = actor_ptr(slot);
        int span = (int)ACTOR_I16(a, ACTOR_SPAN_RAW);
        int cur  = (int)ACTOR_U8(a, ACTOR_SUB_LANE_INDEX);
        int road = td5_track_nearest_road_lane(span, cur);
        if (road != cur)
            ACTOR_U8(a, ACTOR_SUB_LANE_INDEX) = (uint8_t)(cur + (road > cur ? 1 : -1));
    }

    traffic_collision_escape(slot);
}

/* [#6 2026-06-20] A/B knob: TD5RE_COP_RACER_DRIVE=1 restores the old chase
 * driver (the racer AI, td5_ai_update_track_behavior). Default 0 = the
 * road-safe traffic driver. The racer driver assumes the car is already on the
 * racing line; a cop acquired from a traffic lane (often hard against a wall)
 * oversteered off the line into the wall the instant a chase started. The
 * traffic driver KEEPS the cop on the road (idle cops use it and never crash),
 * so a chasing cop now follows the road toward the player and just drives it
 * fast. */
static int cop_racer_drive_enabled(void) {
    static int s = -1;
    if (s < 0) { const char *e = getenv("TD5RE_COP_RACER_DRIVE"); s = (e && e[0] == '1') ? 1 : 0; }
    return s;
}

/* Drive a CHASING cop. By default it drives as ordinary (road-following)
 * traffic — which provably keeps it on the road — and is simply made FAST: once
 * spun up, the catch-up throttle is forced (full forward while below CopCatchup%
 * of the target's speed, coast at/above it; the physics top-speed cap is removed
 * in td5_physics.c so it can reach 1.5x). The chase scheduler (span-based
 * overtake/pullover) is unaffected. TD5RE_COP_RACER_DRIVE=1 reverts to the old
 * racer-AI driver. Pure function of replicated state -> lockstep-safe. */
/* Steer the cop DIRECTLY at its target suspect (world-space heading), overriding
 * the racing line so it crashes INTO the suspect. High gain => sharp turns. The
 * yaw store is (actual_angle + 0x800) << 8 (see correct_spawn_heading), so the
 * cop's facing angle is (yaw>>8) - 0x800; ai_angle_from_vector gives the heading
 * to the target in the same +Z=0/+X=0x400 clockwise convention. */
static int32_t cop_heading_error(int slot, int tgt) {
    char *cop = actor_ptr(slot), *t = actor_ptr(tgt);
    if (!cop || !t) return 0;
    int32_t dx   = ACTOR_I32(t, ACTOR_WORLD_POS_X) - ACTOR_I32(cop, ACTOR_WORLD_POS_X);
    int32_t dz   = ACTOR_I32(t, ACTOR_WORLD_POS_Z) - ACTOR_I32(cop, ACTOR_WORLD_POS_Z);
    /* ACTOR_YAW_ACCUM>>8 IS the travel direction in the ai_angle_from_vector
     * convention (verified vs the diagnostic: subtracting 0x800 put a suspect that
     * was clearly AHEAD at ~157°, which braked the cop forever). */
    int32_t face = (ACTOR_I32(cop, ACTOR_YAW_ACCUM) >> 8) & 0xFFF;
    int32_t dev  = (ai_angle_from_vector(dx, dz) - face) & 0xFFF;
    if (dev > 0x800) dev -= 0x1000;                  /* signed heading error +right/-left */
    return dev;
}

/* GENTLE proportional steering bias toward the target, ADDED on top of the route
 * steer (the route brain keeps the cop ON the road; the bias leans it into the
 * suspect) and clamped well below full lock — a full-lock slam spun the cop out. */
static void cop_ram_steer(int slot, int32_t dev) {
    char *cop = actor_ptr(slot);
    int32_t spd, maxb, bias, out;
    if (!cop) return;
    if (dev > -COP_RAM_DEADBAND && dev < COP_RAM_DEADBAND) return;  /* aligned -> leave route steer */
    /* Allow a HARD turn when slow (tight U-turn after a hit / repositioning) but keep
     * it gentle at speed so a fast cop doesn't spin out. */
    spd = ACTOR_I32(cop, ACTOR_LONGITUDINAL_SPEED);
    if (spd < 0) spd = -spd;
    /* Full lock when slow (so it can swerve INTO / U-turn onto the suspect); capped
     * at speed so a fast chasing cop doesn't spin out. */
    maxb = (spd < COP_SLOW_SPEED) ? 0x18000 : 0x8000;
    bias = dev * 0x10;
    if (bias >  maxb) bias =  maxb;
    if (bias < -maxb) bias = -maxb;
    out = ACTOR_I32(cop, ACTOR_STEERING_CMD) + bias;
    if (out >  0x18000) out =  0x18000;
    if (out < -0x18000) out = -0x18000;
    ACTOR_I32(cop, ACTOR_STEERING_CMD) = out;
}

/* ====================================================================
 * [COP OVERHAUL 2026-06-29] Catch-up acceleration rubber-band + predictive
 * corner easing.
 *
 * The original binary has NO pursuit AI at all (RE-confirmed: 0 hits for
 * cop/police/pursuit; the racer rubber-band @0x00432D60 touches only racer
 * slots 0..5, never traffic; no wall look-ahead exists @0x00434FE0). So the
 * whole cop-in-races system — and these tweaks — are PORT heuristics, not a
 * faithful port of original behaviour.
 *
 * ROOT CAUSE of "police can't catch up on straights": a chasing cop is a
 * TRAFFIC slot, so it integrates through td5_physics_update_traffic(), whose
 * propulsion is a FIXED throttle force (encounter_steering_cmd*4) with NO
 * engine-power / top-speed term. phys_top_speed_rating()->0x7FFF uncaps only
 * the RACER physics path (MP cop chase), not the traffic cop — so a fast player
 * car out-tops the cop's fixed traffic speed and pulls away. The fix below is a
 * true deterministic acceleration rubber-band: scale the cop's traffic
 * propulsion by a Q8 boost that grows with its speed deficit vs the catch-up
 * cap, then tapers to 1.0 as it reaches the cap (self-limiting, no runaway).
 * Pure function of replicated speeds/spans -> lockstep-safe. ============== */

/* Max propulsion boost (% , 100 = no boost) applied when a chasing cop is far
 * below its catch-up cap. TD5RE_COP_CHASE_ACCEL overrides; clamped [100,500]. */
static int cop_chase_accel_max_pct(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("TD5RE_COP_CHASE_ACCEL");
        v = (e && e[0]) ? atoi(e) : 280;
        if (v < 100) v = 100;
        if (v > 500) v = 500;
        TD5_LOG_I(LOG_TAG, "cop_chase_accel: max boost = %d%% (TD5RE_COP_CHASE_ACCEL)", v);
    }
    return v;
}

/* A/B knob: predictive corner easing for a chasing cop. Default ON;
 * TD5RE_COP_CORNER_EASE=0 disables (cop floors the catch-up throttle even into
 * bends — the old crash-prone behaviour). */
static int cop_corner_ease_enabled(void) {
    static int s = -1;
    if (s < 0) { const char *e = getenv("TD5RE_COP_CORNER_EASE");
                 s = (e && e[0] == '0') ? 0 : 1; }
    return s;
}

/* Corner-scan tuning (spans / heading units; same scale as the SmartAI scan). */
#define COP_CORNER_WIN          3       /* heading-delta window (spans) */
#define COP_CORNER_LOOK_MIN     3       /* min spans of look-ahead */
#define COP_CORNER_LOOK_MAX     12      /* max spans of look-ahead */
#define COP_CORNER_LOOK_SPD  0x8000     /* speed / this = extra look-ahead spans */
#define COP_CORNER_SHARP_THRESH 0x180   /* |heading delta| over the window that
                                         * counts as a bend worth easing for
                                         * (~0.59 of SmartAI's ==1.0 value 0x2A0) */

/* 1 when a corner sharp enough to ease for sits within the cop's speed-scaled
 * braking horizon ahead. Used to (a) suppress the catch-up throttle floor in
 * cop_drive and (b) zero the accel-boost, so the cop slows for the bend and
 * lets its route plan steer through instead of flooring into the outside wall.
 * Pure function of replicated span/heading state -> lockstep-safe. */
static int cop_corner_imminent(int slot) {
    char *cop;
    int ring, span, main_span, look, worst, d;
    int32_t spd;
    if (!cop_corner_ease_enabled()) return 0;
    cop = actor_ptr(slot);
    if (!cop) return 0;
    ring = td5_track_get_ring_length();
    if (ring <= 0) return 0;
    span = (int)ACTOR_I16(cop, ACTOR_SPAN_RAW);
    main_span = td5_track_branch_to_main_span(span);
    spd = ACTOR_I32(cop, ACTOR_LONGITUDINAL_SPEED);
    if (spd < 0) spd = -spd;
    look = COP_CORNER_LOOK_MIN + (int)(spd / COP_CORNER_LOOK_SPD);
    if (look > COP_CORNER_LOOK_MAX) look = COP_CORNER_LOOK_MAX;
    worst = 0;
    for (d = 0; d <= look; d++) {
        int s0 = (((main_span + d) % ring) + ring) % ring;
        int s1 = (((main_span + d + COP_CORNER_WIN) % ring) + ring) % ring;
        int turn = smart_ang_signed(td5_track_get_primary_route_heading(s1)
                                    - td5_track_get_primary_route_heading(s0));
        int at = turn < 0 ? -turn : turn;
        if (at > worst) worst = at;
    }
    return worst >= COP_CORNER_SHARP_THRESH;
}

/* [COP OVERHAUL 2026-06-29] Q8 (0x100 = 1.0) propulsion multiplier for a chasing
 * cop, applied to the traffic-integrator throttle in td5_physics_update_traffic.
 * 1.0 (no boost) for non-cops, idle cops, an imminent corner (so the cop eases
 * for the bend), or once the cop has reached its catch-up cap. Below the cap it
 * scales up with the speed deficit so the cop out-accelerates a faster suspect
 * on straights, then tapers back to 1.0 near the cap (self-limiting). Same cap
 * (target_speed * cop_catchup_pct/100, floored at cop_min_speed) the cop_drive
 * catch-up gate uses. Deterministic: replicated speeds/spans + env knob only. */
int td5_ai_cop_chase_throttle_boost_q8(int slot) {
    char *cop, *t;
    int tgt, maxb;
    int32_t cabs, tabs, cap, deficit, boost;
    if (slot < 0 || slot >= TD5_MAX_TOTAL_ACTORS) return 0x100;
    if (!td5_ai_cop_is_chasing(slot)) return 0x100;
    tgt = g_cop_target[slot];
    if (tgt < 0 || tgt >= TD5_MAX_TOTAL_ACTORS) return 0x100;
    if (cop_corner_imminent(slot)) return 0x100;        /* ease for the bend */
    cop = actor_ptr(slot); t = actor_ptr(tgt);
    if (!cop || !t) return 0x100;
    cabs = ACTOR_I32(cop, ACTOR_LONGITUDINAL_SPEED); if (cabs < 0) cabs = -cabs;
    tabs = ACTOR_I32(t,   ACTOR_LONGITUDINAL_SPEED); if (tabs < 0) tabs = -tabs;
    cap = (int32_t)((int64_t)tabs * g_td5.ini.cop_catchup_pct / 100);
    if (cap < g_td5.ini.cop_min_speed) cap = g_td5.ini.cop_min_speed;
    if (cap <= 0 || cabs >= cap) return 0x100;          /* at/above cap -> coast */
    /* deficit fraction (Q8): (cap - cabs)/cap, in [0,0x100]. */
    deficit = (int32_t)((((int64_t)(cap - cabs)) << 8) / cap);
    if (deficit < 0)     deficit = 0;
    if (deficit > 0x100) deficit = 0x100;
    maxb = cop_chase_accel_max_pct();                   /* e.g. 280 */
    /* boost = 1.0 + (maxb/100 - 1.0) * deficit  (all Q8). */
    boost = 0x100 + (int32_t)((((int64_t)((maxb - 100) * 0x100 / 100)) * deficit) >> 8);
    if (boost < 0x100) boost = 0x100;
    return boost;
}

static void cop_drive(int slot) {
    char *cop = actor_ptr(slot);
    int   tgt = g_cop_target[slot];

    /* Run the base driver. A RACER-slot cop (MP cop chase, slot < traffic base)
     * MUST use the RACER AI (route-follow + throttle) — the traffic driver braked
     * a racer slot to a dead stop (the cop never moved). A TRAFFIC-slot cop keeps
     * the traffic driver (keeps it on the paved band). Either sets steering, the
     * BRAKE_FLAG, and a base throttle command; the catch-up + ram layers below
     * then override. */
    if (slot < g_traffic_slot_base || cop_racer_drive_enabled())
        td5_ai_update_track_behavior(slot);
    else
        traffic_drive_normal(slot);

    /* [#6 2026-06-20] Chase spin-up: for the first cop_chase_spinup_ticks() of a
     * fresh chase, DON'T force the catch-up throttle — let the base driver above
     * bring the cop up to speed gently and settle it onto the road before the
     * sprint, so it never floors itself into a wall the instant a chase starts. */
    if (slot >= 0 && slot < TD5_MAX_TOTAL_ACTORS) {
        if (s_cop_chase_ticks[slot] < (int16_t)0x7FFF) s_cop_chase_ticks[slot]++;
        if (s_cop_chase_ticks[slot] <= cop_chase_spinup_ticks())
            return;
    }

    /* [MP COP CHASE — ram oscillator 2026-06-23] Chase the suspect, then crash into
     * it REPEATEDLY: once adjacent the cop REVERSES to open a run-up gap, then
     * LUNGES forward to ram — looping until the suspect is arrested. (Just sitting
     * alongside did no damage; the reverse gives it momentum to hit hard.) */
    if (cop_ram_enabled() && slot < g_traffic_slot_base &&
        g_td5.mp_mode_config.mode == TD5_MP_MODE_COP_CHASE && !g_td5.network_active &&
        tgt >= 0 && tgt < TD5_MAX_TOTAL_ACTORS) {
        int cop_span = (int)ACTOR_I16(cop, ACTOR_SPAN_RAW);
        int tgt_span = (int)ACTOR_I16(actor_ptr(tgt), ACTOR_SPAN_RAW);
        /* Branch-aware gap: fold branch-corridor spans onto the main ring. Branch
         * (fork) spans are numbered far PAST the main ring, so a raw difference made
         * the cop think the suspect was miles away the instant it took a fork -> the
         * cop went "far" and gave up the chase. smart_span_gap is wrap-aware. */
        int ring_len = td5_track_get_ring_length();
        int cop_main = (ring_len > 0) ? td5_track_branch_to_main_span(cop_span) : cop_span;
        int tgt_main = (ring_len > 0) ? td5_track_branch_to_main_span(tgt_span) : tgt_span;
        int gap = (ring_len > 0) ? smart_iabs(smart_span_gap(cop_main, tgt_main, ring_len))
                                 : smart_iabs(cop_span - tgt_span);
        int32_t dev  = cop_heading_error(slot, tgt);
        int32_t adev = (dev < 0) ? -dev : dev;
        int phase = s_cop_ram_phase[slot];

        /* Stuck detection: if the cop has barely moved for a while it's wedged
         * (typically against a wall) -> reverse out with a hard wiggling steer. */
        {
            int32_t cx = ACTOR_I32(cop, ACTOR_WORLD_POS_X);
            int32_t cz = ACTOR_I32(cop, ACTOR_WORLD_POS_Z);
            int32_t moved = smart_iabs(cx - s_cop_ram_lastx[slot]) +
                            smart_iabs(cz - s_cop_ram_lastz[slot]);
            s_cop_ram_lastx[slot] = cx;
            s_cop_ram_lastz[slot] = cz;
            if (moved < COP_STUCK_MOVE) {
                if (s_cop_ram_stuck[slot] < 0x3FFF) s_cop_ram_stuck[slot]++;
            } else {
                s_cop_ram_stuck[slot] = 0;
            }
        }

        if ((g_ai_frame_counter % 30u) == 0u)
            TD5_LOG_I(LOG_TAG,
                "cop_ram DIAG: slot=%d ramphase=%d tgt=%d cop_span=%d tgt_span=%d "
                "cop_main=%d tgt_main=%d copbr=%d tgtbr=%d gap=%d "
                "adev=%d timer=%d stuck=%d steer_cmd=%d throttle=%d face=%d",
                slot, phase, tgt, cop_span, tgt_span, cop_main, tgt_main,
                (ring_len > 0 && cop_span >= ring_len), (ring_len > 0 && tgt_span >= ring_len),
                gap, (int)adev, (int)s_cop_ram_timer[slot],
                (int)s_cop_ram_stuck[slot],
                (int)ACTOR_I32(cop, ACTOR_STEERING_CMD),
                (int)ACTOR_I16(cop, ACTOR_ENCOUNTER_STEER),
                (int)((ACTOR_I32(cop, ACTOR_YAW_ACCUM) >> 8) & 0xFFF));

        cop_ram_steer(slot, dev);   /* always aim at the suspect (speed-aware) */
        /* FAR -> navigate toward the suspect. */
        if (gap > COP_RAM_SPAN_RANGE) {
            s_cop_ram_phase[slot] = COP_RAM_LUNGE;
            /* If the suspect is BEHIND the cop (it overshot far past), DON'T keep
             * driving forward away from it — brake hard; the low-speed (=hard) steer
             * then U-turns the cop back toward the suspect. This is the "drove off and
             * never came back" fix. */
            if (adev >= COP_BEHIND_DEV) {
                ACTOR_I16(cop, ACTOR_ENCOUNTER_STEER) = (int16_t)0xFF00;
                ACTOR_U8(cop,  ACTOR_BRAKE_FLAG)      = 1;
                return;
            }
            /* Suspect ahead -> catch-up toward it, RESPECTING the base brake so it
             * doesn't fly off a corner. */
            if (ACTOR_U8(cop, ACTOR_BRAKE_FLAG))
                return;
            {
                int32_t tabs = ACTOR_I32(actor_ptr(tgt), ACTOR_LONGITUDINAL_SPEED);
                int32_t cabs = ACTOR_I32(cop, ACTOR_LONGITUDINAL_SPEED);
                int16_t ai_cmd = ACTOR_I16(cop, ACTOR_ENCOUNTER_STEER);
                int32_t cap;
                tabs = tabs < 0 ? -tabs : tabs; cabs = cabs < 0 ? -cabs : cabs;
                cap = (int32_t)((int64_t)tabs * g_td5.ini.cop_catchup_pct / 100);
                if (cap < g_td5.ini.cop_min_speed) cap = g_td5.ini.cop_min_speed;
                if (cabs < cap) {
                    if (ai_cmd >= 0 && (int16_t)COP_FULL_THROTTLE > ai_cmd)
                        ACTOR_I16(cop, ACTOR_ENCOUNTER_STEER) = (int16_t)COP_FULL_THROTTLE;
                } else if (ai_cmd > 0) {
                    ACTOR_I16(cop, ACTOR_ENCOUNTER_STEER) = 0;
                }
            }
            return;
        }

        /* CLOSE -> alignment-proportional ram (NO reverse — it was too slow/awkward).
         * The cop only floors while FACING the suspect; the instant the suspect slides
         * off-centre it COASTS (killing the overshoot fast), and if the suspect ends up
         * BEHIND it BRAKES hard so the (hard at low speed) steer whips it through a
         * quick forward U-turn back onto the suspect. A stall kicks it forward. */
        (void)phase;
        if (adev >= COP_BEHIND_DEV) {
            /* Suspect behind (overshot) -> BRAKE hard for a quick steer-driven forward
             * U-turn back onto it (no reverse). */
            ACTOR_U8(cop,  ACTOR_BRAKE_FLAG)      = 1;
            ACTOR_I16(cop, ACTOR_ENCOUNTER_STEER) = (int16_t)0xFF00;
        } else if (gap <= COP_RAM_CLOSE || adev < COP_ALIGNED_DEV) {
            /* Right on top of the suspect (BULLDOZE — constant full throttle, the hard
             * steer keeps grinding into it) OR facing it (RAM). No coast/brake here, so
             * the cop doesn't fall into the short accel/brake jitter once it reaches you;
             * it keeps pushing/ramming continuously. */
            ACTOR_U8(cop,  ACTOR_BRAKE_FLAG)      = 0;
            ACTOR_I16(cop, ACTOR_ENCOUNTER_STEER) = (int16_t)COP_FULL_THROTTLE;
        } else {
            /* Mid-range, suspect off to the side -> COAST so it bleeds speed and the
             * hard steer curves it back on instead of blasting straight past. */
            ACTOR_U8(cop,  ACTOR_BRAKE_FLAG)      = 0;
            ACTOR_I16(cop, ACTOR_ENCOUNTER_STEER) = 0;
        }
        return;
    }

    /* Not an MP cop chase cop (or ram disabled) -> legacy catch-up driver. */
    if (ACTOR_U8(cop, ACTOR_BRAKE_FLAG))
        return;
    /* [DODGE 2026-06-24] If the road-following brain (traffic_drive_normal above)
     * is actively steering around a car ahead this tick — i.e. it set a lane
     * bias to change lanes for a close peer — DON'T slam the catch-up throttle
     * into the obstacle. Leave the route plan's eased throttle so the cop bleeds
     * speed and completes the lane change instead of rear-ending the peer at
     * 1.5x catch-up speed. This is the core of "the police crashes 80% of the
     * time and never makes it to overtake": the catch-up sprint used to override
     * the dodge and ram the traffic car ahead. Catch-up resumes the instant the
     * lane is clear (bias back to 0). The brake-flag return just above already
     * covers the no-clear-lane case (route plan braked). */
    if (slot >= 0 && slot < TD5_MAX_TOTAL_ACTORS && s_traffic_lane_bias[slot] != 0) {
        if ((g_ai_frame_counter % 30u) == 0u)
            TD5_LOG_I(LOG_TAG, "cop_dodge: slot=%d easing around peer (lane_bias=%d) — no catch-up floor",
                      slot, (int)s_traffic_lane_bias[slot]);
        return;
    }
    /* [COP OVERHAUL 2026-06-29] Predictive corner easing: if a sharp bend sits
     * within the cop's speed-scaled braking horizon, DON'T floor the catch-up
     * throttle — leave the route plan's eased throttle so the cop slows and
     * steers through the corner instead of flooring into the outside wall. The
     * accel rubber-band in td5_physics_update_traffic is likewise zeroed for an
     * imminent corner (cop_corner_imminent), so the eased throttle is not
     * re-amplified. The accel-boost then resumes the instant the road straightens
     * out (this is the wall-avoidance half of the overhaul). */
    if (cop_corner_imminent(slot)) {
        if ((g_ai_frame_counter % 30u) == 0u)
            TD5_LOG_I(LOG_TAG, "cop_corner_ease: slot=%d bend ahead — no catch-up floor", slot);
        return;
    }
    if (tgt >= 0 && tgt < TD5_MAX_TOTAL_ACTORS) {
        int32_t tspd = ACTOR_I32(actor_ptr(tgt), ACTOR_LONGITUDINAL_SPEED);
        int32_t cspd = ACTOR_I32(cop, ACTOR_LONGITUDINAL_SPEED);
        int32_t tabs = tspd < 0 ? -tspd : tspd;
        int32_t cabs = cspd < 0 ? -cspd : cspd;
        int32_t cap  = (int32_t)((int64_t)tabs * g_td5.ini.cop_catchup_pct / 100);
        int16_t ai_cmd = ACTOR_I16(cop, ACTOR_ENCOUNTER_STEER);
        if (cap < g_td5.ini.cop_min_speed) cap = g_td5.ini.cop_min_speed;
        if (cabs < cap) {
            if (ai_cmd >= 0 && (int16_t)COP_FULL_THROTTLE > ai_cmd)
                ACTOR_I16(cop, ACTOR_ENCOUNTER_STEER) = (int16_t)COP_FULL_THROTTLE;
        } else if (ai_cmd > 0) {
            ACTOR_I16(cop, ACTOR_ENCOUNTER_STEER) = 0;
        }
    }
}

static void ai_update_single_traffic(int slot) {
    /* Broken-down traffic/cop: park it (brake, no throttle) so it sits and
     * smokes until the timer clears + it is recycled. Skip all driving. */
    if (g_actor_broken_down[slot]) {
        ai_apply_stop_command(actor_ptr(slot));
        return;
    }

    /* Police: a CHASING cop drives as a racer aimed at its target (cop_drive
     * runs the full racer AI with the aim-point overridden onto the chased car,
     * then forces the catch-up throttle). An IDLE or PULLOVER cop drives as
     * ordinary traffic (so it cruises before a chase and coasts to a halt in
     * front of the target while pulling it over). */
    if (g_cop_is_cop[slot] && g_cop_phase[slot] == COP_CHASING) {
        cop_drive(slot);
        return;
    }

    /* Normal traffic update: route plan + (TD6) road-lane clamp + jam-escape.
     * [#18] The road clamp keeps TD6 traffic off sidewalk/shoulder lanes; the
     * collision-escape frees a car jammed too long. (Both now live in
     * traffic_drive_normal, shared with the chasing-cop driver.) */
    traffic_drive_normal(slot);
    /* UpdateTrafficActorMotion(slot) would follow in the physics module */
}

/* ========================================================================
 * Per-frame AI setup — rubber-band + route state refresh.
 * Called once before the interleaved per-actor loop.
 * Matches the top of UpdateRaceActors (0x436A70) before the actor loop.
 * ======================================================================== */
void td5_ai_pre_tick(void) {
    td5_ai_compute_rubber_band();
    td5_ai_refresh_route_state();
    g_ai_frame_counter++;
}

/* ========================================================================
 * Per-actor AI dispatch — call for each slot before physics.
 * Matches the per-actor body of UpdateRaceActors (0x436A70):
 *   racer slots: ai_update_single_racer(slot) [CONFIRMED @ 0x436E1D]
 *   traffic slots: ai_update_single_traffic(slot)
 * ======================================================================== */
/* [MP COP CHASE 2026-06-22] Arm a RACER slot as an AI-driven cop so the chase
 * driver (cop_drive) takes it over in td5_ai_update_actor and phys_top_speed
 * uncaps it (td5_ai_cop_is_chasing). is_ai==0 leaves the slot human-driven. */
void td5_ai_cop_chase_setup(int cop_slot, int is_ai) {
    if (cop_slot < 0 || cop_slot >= TD5_MAX_TOTAL_ACTORS) return;
    if (!is_ai) return;
    g_cop_is_cop[cop_slot] = 1;
    g_cop_phase[cop_slot]  = COP_CHASING;
    g_cop_target[cop_slot] = -1;
    TD5_LOG_I(LOG_TAG, "cop_chase_setup: slot=%d AI cop armed", cop_slot);
}

/* Pick the nearest still-active suspect (by track span) as the AI cop's chase
 * target — gives cop_drive its catch-up speed reference. */
static void mp_cop_pick_target(int cop_slot) {
    char *cop = actor_ptr(cop_slot);
    int cop_span, best = -1, best_d = 0x7FFFFFFF, s;
    /* Suspects are ONLY the active HUMAN players (slots 0..num_human_players-1).
     * Slots beyond that are disabled decoration cars (their g_wanted_damage_state
     * is still 0x1000), so the old loop locked the cop onto a PARKED car and drove
     * into a wall trying to reach it. */
    int nsusp = g_td5.num_human_players;
    int cur, cur_d = 0x7FFFFFFF;
    if (!cop) return;
    if (nsusp > TD5_MAX_RACER_SLOTS) nsusp = TD5_MAX_RACER_SLOTS;
    cop_span = (int)ACTOR_I16(cop, ACTOR_SPAN_RAW);

    /* Dynamic proximity targeting: chase the NEAREST active human suspect, switching
     * to another only when it is clearly closer (COP_RETARGET_MARGIN) so the cop
     * doesn't flip-flop tick to tick. Drops a target that gets arrested. */
    for (s = 0; s < nsusp; s++) {
        char *a; int d;
        if (s == cop_slot) continue;                   /* the (human) cop is not a suspect */
        if (td5_game_cop_chase_is_cop(s)) continue;    /* [MP AI TEST PLAYERS] don't chase fellow cops */
        if (g_wanted_damage_state[s] <= 0) continue;   /* already arrested */
        a = actor_ptr(s);
        if (!a) continue;
        d = (int)ACTOR_I16(a, ACTOR_SPAN_RAW) - cop_span;
        if (d < 0) d = -d;
        if (d < best_d) { best_d = d; best = s; }
    }

    cur = g_cop_target[cop_slot];
    if (cur >= 0 && cur < nsusp && cur != cop_slot && g_wanted_damage_state[cur] > 0) {
        int cd = (int)ACTOR_I16(actor_ptr(cur), ACTOR_SPAN_RAW) - cop_span;
        cur_d = (cd < 0) ? -cd : cd;
        /* Keep the current suspect unless another is meaningfully closer. */
        if (best < 0 || best == cur || best_d + COP_RETARGET_MARGIN >= cur_d)
            return;
    }
    g_cop_target[cop_slot] = best;
}

void td5_ai_update_actor(int slot) {
    if (slot < 0 || slot >= g_active_actor_count)
        return;

    if (slot < g_traffic_slot_base) {
        /* [MP COP CHASE] An AI-driven cop racer slot chases the suspects via the
         * road-safe cop driver (cop_drive) instead of the normal racer AI. */
        if (g_cop_is_cop[slot] && g_cop_phase[slot] == COP_CHASING &&
            g_td5.mp_mode_config.mode == TD5_MP_MODE_COP_CHASE) {
            mp_cop_pick_target(slot);
            cop_drive(slot);
            return;
        }
        ai_update_single_racer(slot);
    } else {
        ai_update_single_traffic(slot);
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
 *   0x0043D4E0  UpdateWantedDamageIndicator  [ARCH-DIVERGENCE: HUD overlay - not in td5_ai.c; L5 sweep 2026-05-21]
 *     Orig (Ghidra-verified 0x0043D4E0): queues two translucent HUD quads into
 *     QueueTranslucentPrimitiveBatch using gWantedDamageStateTable[slot] and
 *     fixed BGRA constants (0x43000000 fill). The port has no equivalent
 *     because the wanted-mode overlay is rendered through td5_hud.c's generic
 *     HUD primitive path keyed off g_wantedModeEnabled + the table at
 *     td5_ai.c:308. Listed under AI because it reads ai-owned state.
 *   0x0043FB90  ReadCompressedTrackStreamChunk  [ARCH-DIVERGENCE: inflate stream callback removed; L5 sweep 2026-05-21]
 *     Orig (Ghidra-verified 0x0043FB90): tiny fwrite-to-output callback for
 *     the orig binary's streaming zlib inflate (writes g_zipDecompressOutputPtr
 *     to g_zipDecompressOutputFile). Port uses td5_inflate_mem_to_mem in
 *     td5_inflate.c (no streaming callback needed). Not implemented anywhere.
 */
