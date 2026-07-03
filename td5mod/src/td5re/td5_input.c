/**
 * td5_input.c -- Input polling, controller config, force feedback,
 *                and input recording/playback.
 *
 * Translated from the original TD5_d3d.exe + M2DX.DLL decompilation.
 * All DirectInput calls are routed through td5_platform.h abstractions.
 *
 * Key original addresses:
 *   0x482FFC  gPlayerControlBits[2]
 *   0x482FE4  gPlayerControlBuffer[6]  (gear debounce)
 *   0x483014  gNosLatch[2]
 *   0x463FC4  gBindingTable[2][9]
 *   0x464054  gKbScanCodes[16]
 *   0x466AFC  gTerrainFFCoefficients[13]
 *   0x4A2CC4  gFFControllerAssignment[6]
 *   0x4A2CDC  gFFCollisionActive[6]
 *   0x466E9C  g_inputPlaybackActive
 *   0x4AAF64  g_replayModeFlag
 */

#include "td5_input.h"
#include "td5_platform.h"
#include "td5re.h"
#include "td5_game.h"
#include "td5_track.h"   /* td5_track_get_span_lane_world() for drag lane-change steer */
#include "td5_camera.h"
#include "td5_ai.h"
#include "td5_save.h"
#include "td5_pending.h"  /* F11 toggles the pending-test overlay */
#include "td5_sound.h"   /* td5_sound_toggle_siren — cop-chase siren toggle */
#include "td5_physics.h" /* FF SIGNALS #1: drift/gear/redline/crash getters for rumble */
#include "td5_laneassist.h" /* keyboard 'L' toggles the optional lane-assist aid */
#include "td5_config.h"  /* shared TD5RE_* env-knob accessors */

/* Defined in td5_game.c */
extern int    g_actorSlotForView[TD5_MAX_VIEWPORTS];
extern uint8_t *g_actor_table_base;

/* Defined in td5_render.c (AngleFromVector12 LUT at 0x0040A720) */
extern int AngleFromVector12(int x, int z);

/* [#2 2026-06-15] Defined in td5_frontend.c. Per-local-player menu transmission:
 * 1 = MANUAL, 0 = AUTO. Lets a MANUAL menu pick actually put the car into manual.
 * (td5_input.c does not include td5_frontend.h, so declare it locally.) */
extern int td5_frontend_get_player_manual(int player);

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define LOG_TAG "input"

/* On-disk replay file format (port-only divergence — original M2DX never
 * persisted; see reference_replay_td5_is_memory_only.md). Little-endian.
 *
 *   offset  size  field
 *   0       8     magic       = "TD5RPLY\0"
 *   8       4     version     = 2
 *   12      4     track_index
 *   16      4     entry_count
 *   20      4     last_frame_index
 *   24      4     start_span_offset   [v2+; BUG 5b — absent in v1, treated as 0]
 *   28      N*8   entries[entry_count]   (TD5_InputRecordEntry)
 *
 * [BUG 5b 2026-06-15] Version bumped 1 -> 2 to carry start_span_offset so a
 * persisted replay recorded with a non-zero --StartSpanOffset replays faithfully.
 * v1 files (24-byte header, no offset) still load: the loader reads the smaller
 * header and defaults the offset to 0. */
#define TD5_REPLAY_MAGIC        "TD5RPLY"      /* 8 bytes incl. NUL */
#define TD5_REPLAY_MAGIC_LEN    8
#define TD5_REPLAY_VERSION      2u
#define TD5_REPLAY_VERSION_V1   1u
#define TD5_REPLAY_HDR_BYTES    28             /* v2 header size (writer) */
#define TD5_REPLAY_HDR_BYTES_V1 24             /* v1 header size (legacy reader) */

typedef struct TD5_ReplayFileHeader {
    char     magic[TD5_REPLAY_MAGIC_LEN];
    uint32_t version;
    int32_t  track_index;
    int32_t  entry_count;
    int32_t  last_frame_index;
    int32_t  start_span_offset;   /* [BUG 5b] v2+; 0 for legacy v1 files */
} TD5_ReplayFileHeader;

static uint32_t s_poll_log_counter = 0;
static uint32_t s_control_log_counter = 0;
static char s_replay_log_path[260];

/* ========================================================================
 * Terrain FF Coefficient Table (from 0x466AFC)
 * ======================================================================== */

const int32_t g_terrain_ff_coefficients[13] = {
    1, 200, 260, 180, 100, 600, 50, 240, 400, 400, 200, 360, 190
};

/* ========================================================================
 * Internal State -- Per-player Control Bitmask
 * ======================================================================== */

/** Live per-player packed input bitmask (mirrors gPlayerControlBits). */
static uint32_t s_control_bits[TD5_MAX_RACER_SLOTS];

/** Packed analog X per-player (decoded from bitmask when analog flag set). */
static int16_t s_analog_x[TD5_MAX_RACER_SLOTS];

/** Packed analog Y per-player. */
static int16_t s_analog_y[TD5_MAX_RACER_SLOTS];

/* ========================================================================
 * Internal State -- Vehicle Control Interpreter
 *
 * These mirror the actor struct fields that UpdatePlayerVehicleControlState
 * writes.  In the original binary they live inside the 0x388-byte actor
 * struct; here we keep them in a clean side array until the physics module
 * copies them into its own actor representation.
 * ======================================================================== */

/** Per-player steering command (maps to actor+0x30C). */
static int32_t s_steering_cmd[TD5_MAX_RACER_SLOTS];

/* [DRAG LANE-CHANGE STEER — Phase 2 2026-06-27] Per-human-slot target lane and
 * previous L/R state for edge detection. target_lane = -1 means "uninitialised"
 * (lazily set to the car's spawn lane on the first drag tick). Reset between
 * races by td5_input_drag_reset(). */
static int     s_drag_target_lane[TD5_MAX_RACER_SLOTS];
static uint8_t s_drag_prev_left[TD5_MAX_RACER_SLOTS];
static uint8_t s_drag_prev_right[TD5_MAX_RACER_SLOTS];

/* [DRAG + LANE ASSIST] The lane the player has chosen via L/R taps (the lane the
 * aggressive lane-assist should steer toward). -1 = not yet set. */
int td5_input_drag_target_lane(int slot)
{
    if (slot < 0 || slot >= TD5_MAX_RACER_SLOTS) return -1;
    return s_drag_target_lane[slot];
}

void td5_input_drag_reset(void)
{
    for (int i = 0; i < TD5_MAX_RACER_SLOTS; i++) {
        s_drag_target_lane[i] = -1;
        s_drag_prev_left[i]   = 0;
        s_drag_prev_right[i]  = 0;
    }
}

/** Per-player steering ramp accumulator (actor+0x33A). */
static int16_t s_steer_ramp[TD5_MAX_RACER_SLOTS];

/** Per-player throttle output (actor+0x33E). */
static int16_t s_throttle[TD5_MAX_RACER_SLOTS];

/** Per-player brake flag (actor+0x36D). */
static uint8_t s_brake[TD5_MAX_RACER_SLOTS];

/** Per-player handbrake flag (actor+0x36E). */
static uint8_t s_handbrake[TD5_MAX_RACER_SLOTS];

/** Per-player reverse requested flag (actor+0x370 bit). */
static uint8_t s_reverse_req[TD5_MAX_RACER_SLOTS];

/** Per-player gear index (actor+0x36B). */
static uint8_t s_gear[TD5_MAX_RACER_SLOTS];

/** Gear-change debounce counters (mirrors gPlayerControlBuffer at 0x482FE4). */
static int32_t s_gear_debounce[TD5_MAX_RACER_SLOTS];

/** NOS latch accumulator per-player (mirrors 0x483014). */
static uint8_t s_nos_latch[TD5_MAX_RACER_SLOTS];

/** Cop-chase siren-toggle horn-edge latch per-player (PORT ENHANCEMENT).
 * Tracks the horn bit so we fire the siren on/off toggle once per press
 * rather than every frame the horn is held. */
static uint8_t s_siren_horn_latch[TD5_MAX_RACER_SLOTS];

/** Regular-car horn-honk edge latch per-player (PORT ENHANCEMENT).
 * One honk per press of the horn key — the horn one-shot would otherwise
 * re-trigger every frame the key is held. Separate from the siren latch so a
 * cop in wanted mode toggles the siren while everyone else honks. */
static uint8_t s_horn_honk_latch[TD5_MAX_RACER_SLOTS];

/** NOS cooldown per actor (actor+0x37C). */
static uint8_t s_nos_cooldown[TD5_MAX_RACER_SLOTS];

/** Camera change cooldown per-player (mirrors 0x4AAF98). */
static int32_t s_camera_cooldown[TD5_MAX_RACER_SLOTS];

/** Rear view flag per-player (mirrors 0x466F88). */
static int32_t s_rear_view[TD5_MAX_RACER_SLOTS];

/* [STUCK RECOVERY 2026-06-15] Per-local-player manual car-recovery edge.
 * Indexed by local human player index (0..s_active_players-1), keyed the same
 * way as s_rear_view / s_camera_cooldown above. The poll arms s_recovery_request
 * on the RISING edge of the recovery button (keyboard R / joystick SELECT) and
 * s_recovery_held debounces so a held button fires once per press. The physics
 * sim drains the request via td5_input_recovery_requested(). PORT ENHANCEMENT —
 * no original analog (the 1999 game had no player car-reset). */
static uint8_t s_recovery_request[TD5_MAX_HUMAN_PLAYERS];
static uint8_t s_recovery_held[TD5_MAX_HUMAN_PLAYERS];

/* [#6 support 2026-06-15] Per-local-player held-latches for the frontend
 * camera-button edge getters (td5_input_frontend_change_camera_pressed /
 * _front_view_pressed). Declared here so td5_input_init can zero them; defined
 * once, used only by those getters near the bottom of this file. */
static uint8_t s_fe_cam_held[TD5_MAX_HUMAN_PLAYERS];
static uint8_t s_fe_front_held[TD5_MAX_HUMAN_PLAYERS];

/* ========================================================================
 * Internal State -- Input Source Config
 *
 * 0 = keyboard, 1+ = joystick index + 1.
 * ======================================================================== */

static int s_input_source[TD5_MAX_HUMAN_PLAYERS] = { 0 };

/** Number of active human players (1 or 2). */
static int s_active_players = 1;

/** NOS feature enabled globally. */
static int s_nos_enabled = 0;

/** Cop/encounter mode active. */
static int s_cop_mode = 0;

/** Nitro pending flag per-player (set by game logic, read here).
 *  [PORT ENHANCEMENT 2026-06] grown from 2 to TD5_MAX_HUMAN_PLAYERS. */
static int s_nitro_pending[TD5_MAX_HUMAN_PLAYERS] = { 0 };

/* ========================================================================
 * Internal State -- Playback/Replay
 * ======================================================================== */

static int s_playback_active = 0;
static int s_replay_mode_flag = 0;

/* [PORT ENHANCEMENT 2026-06-14 task#18] Controller exit-replay detection.
 * The faithful replay-abort path (td5_game.c) only watches the raw ESC key, so
 * there was no way to LEAVE a replay with a controller. While replay is playing
 * we live-poll the pads for a "leave/cancel" button (Start/Menu -> PAUSE bit,
 * Back/View -> CHANGE VIEW bit) and latch a one-shot edge here. The game layer
 * reads td5_input_replay_exit_requested() and OR's it into its ESC abort edge.
 * s_replay_exit_btn_held debounces so a held button fires the exit only once.
 * Gate: TD5RE_REPLAY_EXIT (cached) — "0" disables the controller exit (old
 * behaviour: ESC-keyboard only). Default = enabled. */
static int s_replay_exit_requested = 0;   /* one-shot: set on edge, cleared by reader */
static int s_replay_exit_btn_held  = 0;   /* debounce latch across frames */

/* Set by write_open, cleared by write_close. When the [Replay] PersistToDisk
 * flag is on, td5_input_shutdown uses this to flush a partial recording
 * if the race didn't end through the normal teardown (e.g. RaceTraceMaxSimTicks
 * quit, user closed the window mid-race). Port-only — no effect when the
 * flag is off. */
static int s_replay_recording_open = 0;

/* ========================================================================
 * Internal State -- Recording Buffer
 * ======================================================================== */

static TD5_InputRecordBuffer s_rec;

/* ========================================================================
 * Internal State -- Force Feedback
 * ======================================================================== */

static TD5_FFGameState s_ff;

/* [FF SIGNALS 2026-06-15, #1] Physics-driven rumble/vibration state.
 *
 * The FF path here is DirectInput EFFECTS, not XInput motors (see
 * td5_plat_ff_constant in td5_platform_win32.c). There is no low/high motor
 * pair; instead the device owns four DI effect slots created in
 * td5_plat_ff_init: slot 0/1/2 = DICONSTANTFORCE (steering / frontal / side),
 * slot 3 = DIPERIODIC sine (terrain rumble). We map "rumble" types onto the
 * PERIODIC slot (slot 3 — the natural buzz, the low-motor analogue) and "jolt"
 * types onto a CONSTANT-FORCE slot (slot 1 = TD5_FF_SLOT_FRONTAL — the
 * high-motor analogue). The continuous drift/redline buzz is COMPOSED with the
 * existing terrain magnitude inside td5_input_ff_update_player so the two
 * writers of slot 3 don't stomp each other; the crash/gear jolts are fired as
 * short edge-driven pulses on slot 1 from td5_input_ff_update.
 *
 * Per-player edge tracking (indexed by player slot, == device slot):
 *   s_ff_gear_seen  — last-seen td5_physics_gear_change_seq() value
 *   s_ff_crash_seen — last-seen td5_physics_get_crash_fx() sequence id
 *   s_ff_pulse_mag / s_ff_pulse_ticks — current jolt magnitude + remaining
 *     render-frames it stays asserted on slot 1 (a one-shot decays to 0). */
static uint32_t s_ff_gear_seen [TD5_MAX_RACER_SLOTS];
static uint32_t s_ff_crash_seen[TD5_MAX_RACER_SLOTS];
static int      s_ff_pulse_mag  [TD5_MAX_RACER_SLOTS];
static int      s_ff_pulse_ticks[TD5_MAX_RACER_SLOTS];
/* [item #5(4)] Last-seen landing sequence id per player, so an airborne->grounded
 * landing fires its jolt exactly once (the physics seq is monotonic). The landing
 * jolt reuses the slot-1 (FRONTAL/high-motor) decaying pulse above. */
static uint32_t s_ff_land_seen [TD5_MAX_RACER_SLOTS];
/* [FF stuck-motor fix 2026-06-15] SIDE-collision (slot 2 / low motor) decaying
 * pulse, the low-motor analogue of the frontal jolt above. V2V/wall collisions
 * now feed these instead of a persistent motor write that was never cleared. */
static int      s_ff_side_mag   [TD5_MAX_RACER_SLOTS];
static int      s_ff_side_ticks [TD5_MAX_RACER_SLOTS];
/* [MP FF FIX 2026-06-23] One-shot diagnostic guard: log once per local
 * player/device when its FF actor-slot map is non-identity (netplay client or
 * MP position-select), so a log read can confirm which car the wheel follows. */
static int      s_ff_slot_map_logged[TD5_MAX_HUMAN_PLAYERS];

/* TD5RE_FORCE_FEEDBACK (cached): master gate for the physics-driven vibration.
 * Default ON; "0" reverts to the prior behaviour (steering + terrain FF only,
 * no drift/crash/gear/redline rumble). Resolved once on first use. */
static int td5_ff_vibration_enabled(void)
{
    static int s_init = 0;
    static int s_on = 1;
    if (!s_init) {
        s_on = td5_env_flag_on("TD5RE_FORCE_FEEDBACK");   /* default ON */
        s_init = 1;
        TD5_LOG_I(LOG_TAG, "Force feedback vibration: %s",
                  s_on ? "enabled (drift/crash/gear/redline/landing)" : "disabled");
    }
    return s_on;
}

/* [item #5(3); default-OFF 2026-06-16] TD5RE_FF_DRIFT (cached): sub-gate for the
 * continuous drift rumble. DEFAULT OFF, opt in with "1". Diagnosis (FF DIAG log)
 * showed the slip metric (|lateral +0x318| / |longitudinal +0x314|) sits at
 * ~0.75..1.1 during ordinary arcade driving — there is no clean threshold between
 * baseline cornering slip and an intentional drift, so the continuous buzz fired
 * during normal driving at moderate RPM. Off until a geometric slip-onset trigger
 * replaces the raw ratio. Subordinate to TD5RE_FORCE_FEEDBACK (the master gate). */
static int td5_ff_drift_enabled(void)
{
    static int s_init = 0;
    static int s_on = 0;
    if (!s_init) {
        const char *e = getenv("TD5RE_FF_DRIFT");
        s_on = (e && (e[0] == '1' || e[0] == 'y' || e[0] == 'Y')) ? 1 : 0;   /* default OFF (opt-in) */
        s_init = 1;
        TD5_LOG_I(LOG_TAG, "FF drift rumble: %s", s_on ? "enabled" : "disabled");
    }
    return s_on;
}

/* [stronger collisions 2026-06-16] Live-tunable collision pulse gain (Q8). Default
 * TD5_FF_COLLISION_GAIN_Q8; override with TD5RE_FF_COLLISION_GAIN (decimal or 0x..,
 * e.g. 0x600 = 6.0x) to dial collision strength without a rebuild. Clamped 1x..16x. */
static int ff_collision_gain_q8(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("TD5RE_FF_COLLISION_GAIN");
        long g = (e && e[0]) ? strtol(e, NULL, 0) : 0x400;   /* default == TD5_FF_COLLISION_GAIN_Q8 (defined below) */
        if (g < 0x100) g = 0x100;     /* never weaker than 1.0x */
        if (g > 0x1000) g = 0x1000;   /* cap at 16x */
        v = (int)g;
        TD5_LOG_I(LOG_TAG, "FF collision gain Q8 = 0x%X (TD5RE_FF_COLLISION_GAIN=%s)", v, e ? e : "default");
    }
    return v;
}

/* [item #5(2)] TD5RE_FF_COLLISION_DIR (cached): sub-gate for DIRECTIONAL
 * collisions. Default ON: a left-side impact biases the LOW (left) motor, a
 * right-side impact the HIGH (right) motor (XInput), and the DI wheel gets the
 * matching constant-force direction. "0" reverts to the faithful symmetric
 * frontal/side mapping (left and right impacts feel the same). */
static int td5_ff_collision_dir_enabled(void)
{
    static int s_init = 0;
    static int s_on = 1;
    if (!s_init) {
        s_on = td5_env_flag_on("TD5RE_FF_COLLISION_DIR");   /* default ON */
        s_init = 1;
        TD5_LOG_I(LOG_TAG, "FF directional collisions: %s",
                  s_on ? "enabled (left=low motor, right=high motor)" : "disabled (symmetric)");
    }
    return s_on;
}

/* DirectInput periodic/constant magnitudes run 0..DI_FFNOMINALMAX (10000); the
 * platform layer clamps, but we size our contributions to that domain. The
 * only continuous rumble is a LIGHT redline buzz (drift rumble retired — see
 * td5_input_ff_update_player); the gear/crash JOLT uses a stronger short
 * constant pulse. */
#define TD5_FF_MAG_MAX            10000   /* DI_FFNOMINALMAX */
#define TD5_FF_REDLINE_MAG         2200   /* light continuous redline buzz */
#define TD5_FF_GEAR_PULSE_MAG      3000   /* brief low-force gear-shift bump */
#define TD5_FF_GEAR_PULSE_TICKS       2   /* render-frames the gear bump holds */
#define TD5_FF_CRASH_PULSE_MAG_MAX 10000  /* full-scale strong crash jolt */
#define TD5_FF_CRASH_PULSE_TICKS      8   /* render-frames the crash jolt holds [stronger collisions 2026-06-16, was 4] */
#define TD5_FF_COLLISION_PULSE_TICKS 12   /* render-frames a V2V/wall collision jolt holds before auto-release [stronger 2026-06-16, was 6] */
#define TD5_FF_CRASH_NEW_AGE_TICKS    3   /* a crash is "new" if age <= this (sim ticks) */
/* Crash impact magnitude is large (acute threshold 250000); scale it to the FF
 * domain. >>5 maps ~250000 -> ~7800 and saturates to 10000 by ~320000. */
#define TD5_FF_CRASH_MAG_SHIFT        5

/* [item #5(1)] Collision pulse STRENGTH. The faithful td5_input_ff_collision
 * divides the raw impact by TD5_INPUT_FF_COLLISION_DIV (10) and caps at 10000;
 * ordinary wall scrapes then land far below DI_FFNOMINALMAX and feel weak. Scale
 * the divided magnitude up (Q8: 0x100 = 1.0x) and floor it so even a glancing hit
 * is clearly felt, then clamp to the DI nominal max so we never exceed the device
 * range. [stronger collisions 2026-06-16] 0x400 = 4.0x (was 2.0x) so moderate
 * wall/car hits saturate toward full motor; floor raised so even light contacts
 * are clearly felt. Tune live with TD5RE_FF_COLLISION_GAIN (Q8, e.g. 0x600=6x). */
#define TD5_FF_COLLISION_GAIN_Q8   0x400  /* 4.0x boost on the divided collision mag */
#define TD5_FF_COLLISION_FLOOR     4500   /* min felt collision pulse (clearly perceptible) */

/* [item #5(3)] Continuous DRIFT rumble. Proportional to td5_physics_get_drift_level
 * (0 = not drifting, ~1..255). EXACTLY 0 below the threshold so a car tracking
 * straight never buzzes. Scaled so a hard slide approaches a light/medium buzz —
 * intentionally below the crash/redline ceiling so it reads as "tyres scrubbing",
 * not an alarm. Like the redline buzz it rides the low motor (gamepad) / slot-3
 * periodic (wheel) and is recomputed every frame (0 when not drifting => the
 * continuous-buzz contributor returns to 0, so it can never stick on). */
#define TD5_FF_DRIFT_LEVEL_MIN       24   /* drift_level below this => 0 rumble (~14deg+ slide already deadzoned in physics) */
#define TD5_FF_DRIFT_MAG_MAX       3200   /* rumble at full (255) drift level */
#define TD5_FF_DRIFT_MAG_FLOOR     1100   /* min buzz once over the threshold so onset is felt */

/* [item #5(4)] Air-time LANDING jolt. A decaying pulse fired when the physics
 * landing signal reports a touchdown (td5_physics_get_landing_fx). Scaled by the
 * downward vertical impact speed (raw 24.8). >>3 maps a stiff ~50000-unit drop
 * to ~6250 and a big drop saturates to the cap; floored so even a light hop is
 * felt. Held a few frames then released by the same decay path as the crash jolt. */
#define TD5_FF_LANDING_PULSE_TICKS    4   /* render-frames the landing jolt holds */
#define TD5_FF_LANDING_MAG_SHIFT      3   /* impact >> this maps vertical speed -> FF domain */
#define TD5_FF_LANDING_MAG_MAX     9000   /* cap (just under full-scale) */
#define TD5_FF_LANDING_MAG_FLOOR   2800   /* min felt landing jolt */

/* ========================================================================
 * Internal State -- Escape / Fade
 * ======================================================================== */

static int s_escape_muted = 0;
static int s_escape_fade_active = 0;

/* F3 key edge-detection latch [CONFIRMED @ 0x42C6FC: DAT_004aaf93] */
static uint8_t s_f3_held = 0;

/* ========================================================================
 * Helper: clamp int to range
 * ======================================================================== */

static inline int clamp_i(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/* static inline int abs_i(int v) { return v < 0 ? -v : v; } -- reserved */

/* ========================================================================
 * Lifecycle
 * ======================================================================== */

int td5_input_init(void)
{
    int ok;
    int device_count;

    memset(s_control_bits, 0, sizeof(s_control_bits));
    memset(s_analog_x, 0, sizeof(s_analog_x));
    memset(s_analog_y, 0, sizeof(s_analog_y));
    memset(s_steering_cmd, 0, sizeof(s_steering_cmd));
    memset(s_steer_ramp, 0, sizeof(s_steer_ramp));
    memset(s_throttle, 0, sizeof(s_throttle));
    memset(s_brake, 0, sizeof(s_brake));
    memset(s_handbrake, 0, sizeof(s_handbrake));
    memset(s_reverse_req, 0, sizeof(s_reverse_req));
    memset(s_gear, 0, sizeof(s_gear));
    memset(s_gear_debounce, 0, sizeof(s_gear_debounce));
    memset(s_nos_latch, 0, sizeof(s_nos_latch));
    memset(s_siren_horn_latch, 0, sizeof(s_siren_horn_latch));
    memset(s_horn_honk_latch, 0, sizeof(s_horn_honk_latch));
    memset(s_nos_cooldown, 0, sizeof(s_nos_cooldown));
    memset(s_camera_cooldown, 0, sizeof(s_camera_cooldown));
    memset(s_rear_view, 0, sizeof(s_rear_view));
    memset(s_recovery_request, 0, sizeof(s_recovery_request));
    memset(s_recovery_held, 0, sizeof(s_recovery_held));
    memset(&s_rec, 0, sizeof(s_rec));
    memset(&s_ff, 0, sizeof(s_ff));
    memset(s_ff_gear_seen, 0, sizeof(s_ff_gear_seen));
    memset(s_ff_crash_seen, 0, sizeof(s_ff_crash_seen));
    memset(s_ff_land_seen, 0, sizeof(s_ff_land_seen));
    memset(s_ff_pulse_mag, 0, sizeof(s_ff_pulse_mag));
    memset(s_ff_pulse_ticks, 0, sizeof(s_ff_pulse_ticks));
    memset(s_ff_side_mag, 0, sizeof(s_ff_side_mag));
    memset(s_ff_side_ticks, 0, sizeof(s_ff_side_ticks));
    memset(s_ff_slot_map_logged, 0, sizeof(s_ff_slot_map_logged));
    memset(s_fe_cam_held, 0, sizeof(s_fe_cam_held));
    memset(s_fe_front_held, 0, sizeof(s_fe_front_held));
    memset(s_replay_log_path, 0, sizeof(s_replay_log_path));

    s_escape_muted = 0;
    s_escape_fade_active = 0;

    ok = td5_plat_input_init();
    device_count = td5_plat_input_enumerate_devices();
    TD5_LOG_I(LOG_TAG, "Input init: result=%d devices=%d active_players=%d",
              ok, device_count, s_active_players);
    return ok;
}

void td5_input_shutdown(void)
{
    /* Flush any open recording when persistence is enabled. Covers exit
     * paths that bypass the normal race-end teardown (RaceTraceMaxSimTicks
     * quit, user closed window mid-race). No-op in faithful default mode. */
    if (s_replay_recording_open && g_td5.ini.replay_persist_to_disk) {
        TD5_LOG_I(LOG_TAG, "Replay shutdown: flushing partial recording");
        td5_input_write_close();
    }

    td5_input_ff_shutdown();
    td5_plat_input_shutdown();
}

void td5_input_tick(void)
{
    td5_input_poll_race_session();
}

/* ========================================================================
 * Setters for state driven by other modules
 * ======================================================================== */

void td5_input_set_active_players(int n)       { s_active_players = clamp_i(n, 1, TD5_MAX_HUMAN_PLAYERS); }
void td5_input_set_input_source(int p, int s)
{
    if (p < 0 || p >= TD5_MAX_HUMAN_PLAYERS) return;
    s_input_source[p] = s;
    /* Activate the device so the poll path actually reads it: source 0 releases
     * this slot's joystick (keyboard), source >=1 creates joystick (1-based
     * device index). Without this the chosen source was a dead flag. */
    td5_plat_input_set_device(p, s);
}
int  td5_input_get_input_source(int p)          { return (p >= 0 && p < TD5_MAX_HUMAN_PLAYERS) ? s_input_source[p] : 0; }
void td5_input_set_joystick_bindings(int player, const int32_t *bindings, int count)
{
    if (player < 0 || player >= TD5_MAX_HUMAN_PLAYERS) return;
    td5_plat_input_set_joystick_bindings(player, bindings, count);
}

void td5_input_set_action_bindings(int player, const uint32_t *codes, int count)
{
    if (player < 0 || player >= TD5_MAX_HUMAN_PLAYERS) return;
    td5_plat_input_set_action_bindings(player, codes, count);
}

/* Resolve and apply each player's input device + bindings at race start.
 * Source precedence: INI override (Player1Joystick/Player2Joystick, >0 = a
 * 1-based enumerated joystick index) wins for players 1/2; otherwise the
 * per-player device index persisted via the save module (Config.td5 p1/p2,
 * td5re_input.ini [Devices] PlayerN for 3-9). 0 = keyboard. For a joystick
 * player the saved 9-slot binding row is pushed to the live poll. Called from
 * InitializeRaceSession (Step 15) BEFORE FF init so each device exists.
 * [PORT ENHANCEMENT 2026-06] generalized from 2 to s_active_players (up to 9). */
void td5_input_apply_device_selection(void)
{
    /* Device count (idempotent re-enumeration) so a stale/placeholder persisted
     * index can't try to open a device that doesn't exist. */
    int dev_count = td5_plat_input_enumerate_devices();
    int players = s_active_players;
    if (players < 1) players = 1;
    if (players > TD5_MAX_HUMAN_PLAYERS) players = TD5_MAX_HUMAN_PLAYERS;

    const uint32_t *bind = td5_save_get_controller_bindings_mutable();
    for (int p = 0; p < players; p++) {
        int src;
        if (p == 0 && g_td5.ini.player1_joystick > 0)
            src = g_td5.ini.player1_joystick;
        else if (p == 1 && g_td5.ini.player2_joystick > 0)
            src = g_td5.ini.player2_joystick;
        else
            src = (int)td5_save_get_player_device_index(p);

        /* Clamp out-of-range indices to keyboard (the shipped default Config.td5
         * carries a non-zero placeholder at +0x20/+0x21). */
        if (src < 0 || src >= dev_count) src = 0;

        td5_input_set_input_source(p, src);   /* creates/releases the device */
        if (src > 0 && bind) {
            int32_t row[9];
            for (int i = 0; i < 9; i++) row[i] = (int32_t)bind[p * 9 + i];
            td5_input_set_joystick_bindings(p, row, 9);
        }
        /* Push the per-action bindings (button/axis/trigger). These FOLLOW THE
         * DEVICE, not the player slot: a controller configured under ANY
         * Control-Options player applies whenever that physical device drives a
         * race slot. (Otherwise configuring "PLAYER 2"'s joystick and then
         * driving slot 0 with it would fall back to the default mapping — the
         * bug being fixed.) Find the configured owner of this slot's device:
         * prefer a player whose persisted device == src AND has bindings, else
         * any player on that device, else this slot's own row.
         * [PORT ENHANCEMENT 2026-06] */
        if (src > 0) {
            const uint32_t *ab = td5_save_get_action_bindings_mutable();
            if (ab) {
                int owner = -1, q, i;
                for (q = 0; q < TD5_MAX_HUMAN_PLAYERS; q++) {
                    if ((int)td5_save_get_player_device_index(q) != src) continue;
                    const uint32_t *row = ab + (size_t)q * TD5_JSBIND_ACTIONS;
                    int any = 0;
                    for (i = 0; i < TD5_JSBIND_ACTIONS; i++) if (row[i]) { any = 1; break; }
                    if (any) { owner = q; break; }   /* configured owner wins */
                    if (owner < 0) owner = q;        /* else first claimant of the device */
                }
                if (owner < 0) owner = p;            /* fallback: this slot's own row */
                {
                    const uint32_t *row = ab + (size_t)owner * TD5_JSBIND_ACTIONS;
                    int any = 0;
                    for (i = 0; i < TD5_JSBIND_ACTIONS; i++) if (row[i]) { any = 1; break; }
                    if (any) {
                        td5_input_set_action_bindings(p, row, TD5_JSBIND_ACTIONS);
                        TD5_LOG_I(LOG_TAG, "Device selection: slot=%d device=%d uses bindings from player %d",
                                  p, src, owner + 1);
                    }
                }
            }
        }
        TD5_LOG_I(LOG_TAG, "Device selection: player=%d source=%d (%s)",
                  p, src, (src == 0) ? "keyboard" : "joystick");
    }
}
void td5_input_set_playback_active(int v)       { s_playback_active = v; }
int  td5_input_is_playback_active(void)         { return s_playback_active; }
void td5_input_set_replay_mode(int v)           { s_replay_mode_flag = v; }

/* [PORT ENHANCEMENT 2026-06-14 task#18] One-shot: returns 1 once when a
 * controller Back/Start was pressed during replay playback (see the playback
 * branch of td5_input_poll_race_session). Reading CONSUMES the request so the
 * exit fires exactly once per press. The game layer should OR this into its
 * replay/cinematic ESC abort edge. Always 0 when not in playback (the latch is
 * only ever set inside the playback branch). */
int  td5_input_replay_exit_requested(void)
{
    int r = s_replay_exit_requested;
    s_replay_exit_requested = 0;
    return r;
}

/* [STUCK RECOVERY 2026-06-15] One-shot per-local-player manual-recovery edge.
 * Drained by the deterministic physics tick. `player` is the LOCAL human player
 * index (0..s_active_players-1), which EQUALS the actor slot that player drives
 * (identity binding, see td5_input_split_btn_route_on). The physics drain resets
 * actor slot == player, NOT g_actorSlotForView[player] (the pane permutation).
 * Reading consumes the edge. Returns 0 for any out-of-range index or when the latch
 * was never armed (knob off / no press). */
int  td5_input_recovery_requested(int player)
{
    if (player < 0 || player >= TD5_MAX_HUMAN_PLAYERS) return 0;
    int r = s_recovery_request[player];
    s_recovery_request[player] = 0;
    return r;
}

void td5_input_set_nos_enabled(int v)           { s_nos_enabled = v; }
void td5_input_set_cop_mode(int v)              { s_cop_mode = v; }
void td5_input_set_nitro_pending(int p, int v)  { if (p >= 0 && p < TD5_MAX_HUMAN_PLAYERS) s_nitro_pending[p] = v; }

/* [SPLIT-SCREEN BUTTON ROUTING 2026-06-27] In local split-screen MP each player p
 * DRIVES actor slot p (identity — the same binding the per-slot steering loop uses,
 * and the original's gPrimarySelectedSlot/{0,1} map, RE-confirmed @0x0042AB33). The
 * MP "CHOOSE YOUR SCREEN" position picker only permutes which PANE shows which car
 * (g_actorSlotForView), NOT who drives what. So a per-player VIEW action
 * (change-camera, rear-view) and the manual car-reset must act on the player's OWN
 * car (slot p) / the pane that SHOWS it (view v where g_actorSlotForView[v]==p) —
 * not on pane p. Before this fix, when players picked non-default panes the camera /
 * reset buttons hit the OTHER player's car. When the pane map is identity (AutoRace,
 * the harness, non-positioned MP) view_for_player(p)==p, so those flows stay
 * byte-identical. Gated by TD5RE_SPLIT_BTN_ROUTE (default ON; exactly "0" reverts to
 * the legacy pane==player routing). */
int td5_input_split_btn_route_on(void)
{
    static int s_cached = -1;
    if (s_cached < 0) {
        const char *e = getenv("TD5RE_SPLIT_BTN_ROUTE");
        s_cached = (e && e[0] == '0' && e[1] == '\0') ? 0 : 1;
        TD5_LOG_I(LOG_TAG, "split-screen button routing %s (TD5RE_SPLIT_BTN_ROUTE=%s)",
                  s_cached ? "ENABLED" : "disabled", e ? e : "default");
    }
    return s_cached;
}

/* Return the viewport/pane index that DISPLAYS the car driven by local player p
 * (= actor slot p): the pane v where g_actorSlotForView[v]==p. Falls back to identity
 * (p) when the fix is off, when p's car isn't shown in any pane, or when p is out of
 * range — so the non-positioned / single-view paths are unchanged. */
static int view_for_player(int p)
{
    if (td5_input_split_btn_route_on()) {
        int views = g_td5.viewport_count;
        if (views < 1) views = 1;
        if (views > TD5_MAX_VIEWPORTS) views = TD5_MAX_VIEWPORTS;
        for (int v = 0; v < views; v++)
            if (g_actorSlotForView[v] == p) return v;
    }
    return (p >= 0 && p < TD5_MAX_VIEWPORTS) ? p : 0;
}

/* [SPLIT-SCREEN DEVICE BLEED 2026-06-30] One-shot race-start dump of the local
 * input map: per active local player slot — the bound device + its physical-
 * device key, the actor slot it drives (identity), and the pane that shows that
 * car (view_for_player). If two HUMAN slots resolve to the SAME physical pad it
 * logs a loud WARNING: that is the signature of one controller bleeding onto
 * multiple split-screen panes (e.g. the change-camera button flipping several
 * players' views — the bug this diagnostic was added for). Pure logging; called
 * from InitRace after device + viewport setup. Routed to frontend.log (input). */
void td5_input_log_race_input_map(void)
{
    int n = s_active_players;
    char keys[TD5_MAX_HUMAN_PLAYERS][300];
    if (n < 1) n = 1;
    if (n > TD5_MAX_HUMAN_PLAYERS) n = TD5_MAX_HUMAN_PLAYERS;
    for (int p = 0; p < n; p++) {
        int src   = s_input_source[p];
        int is_ai = (g_td5.mp_ai_player_mask & (1u << p)) ? 1 : 0;
        int pane  = view_for_player(p);
        int shows = (pane >= 0 && pane < TD5_MAX_VIEWPORTS) ? g_actorSlotForView[pane] : -1;
        const char *name = (src == 0) ? "keyboard" : td5_input_get_device_name(src);
        keys[p][0] = '\0';
        if (src > 0) td5_plat_input_slot_phys_key(p, keys[p], sizeof keys[p]);
        TD5_LOG_I(LOG_TAG,
            "race input-map: player=%d source=%d ai=%d device=\"%s\" drives-actor=%d "
            "pane=%d pane-shows-actor=%d phys=\"%s\"",
            p, src, is_ai, name ? name : "?", p, pane, shows,
            keys[p][0] ? keys[p] : "-");
    }
    for (int a = 0; a < n; a++) {
        if (!keys[a][0] || (g_td5.mp_ai_player_mask & (1u << a))) continue;
        for (int b = a + 1; b < n; b++) {
            if (!keys[b][0] || (g_td5.mp_ai_player_mask & (1u << b))) continue;
            if (strcmp(keys[a], keys[b]) == 0)
                TD5_LOG_W(LOG_TAG,
                    "race input-map: players %d and %d read the SAME physical controller "
                    "(key=%s) — one pad will drive BOTH panes' inputs/camera (device bleed)",
                    a, b, keys[a]);
        }
    }
}

/* ========================================================================
 * PollRaceSessionInput  (0x42C470)
 *
 * Called each frame from RunRaceFrame.  Bridges hardware input to the
 * game's per-player control DWORD array.
 *
 * In playback mode, reads from the recording buffer instead of polling
 * hardware, but still checks Escape via the keyboard for manual abort.
 * ======================================================================== */

void td5_input_poll_race_session(void)
{
    /* ---- Playback mode ---- */
    if (s_playback_active) {
        uint32_t w0 = 0, w1 = 0;
        td5_input_read_frame(&w0, &w1);
        s_control_bits[0] = w0;
        s_control_bits[1] = w1;

        /* Still check Escape from live keyboard so player can abort replay. */
        if (td5_plat_input_key_pressed(0x01)) {
            s_control_bits[0] |= (uint32_t)TD5_INPUT_STUNNED;  /* bit 30 = 0x40000000 */
            /* Actually escape is bit 30 */
            s_control_bits[0] |= 0x40000000u;
        }

        /* [PORT ENHANCEMENT 2026-06-14 task#18] Controller exit-replay.
         * Recorded input drives the sim during playback, so the live pad is
         * free — repurpose its Back/Start button as "leave replay". Live-poll
         * each active player's device (the platform poll reads hardware
         * regardless of playback state) and edge-detect Start/Menu (decoded as
         * TD5_INPUT_PAUSE) or Back/View (decoded as TD5_INPUT_CAMERA_CHANGE).
         * The result is latched in s_replay_exit_requested for the game layer
         * (see td5_input_replay_exit_requested). We also OR the ESCAPE control
         * bit so any control-bit-based consumer sees the abort too. */
        {
            static int s_replay_exit_init = 0;
            static int s_replay_exit_enabled = 1;
            if (!s_replay_exit_init) {
                s_replay_exit_enabled = td5_env_flag_on("TD5RE_REPLAY_EXIT");  /* default ON */
                s_replay_exit_init = 1;
                TD5_LOG_I(LOG_TAG, "Replay controller-exit: %s",
                          s_replay_exit_enabled ? "enabled" : "disabled (ESC only)");
            }

            int exit_btn_now = 0;
            if (s_replay_exit_enabled) {
                int players = s_active_players;
                if (players < 1) players = 1;
                if (players > TD5_MAX_HUMAN_PLAYERS) players = TD5_MAX_HUMAN_PLAYERS;
                /* The exit button is a controller action; ignore keyboard players
                 * (their ESC is handled above). Back/Start map through the decoded
                 * PAUSE / CHANGE VIEW bits regardless of per-pad rebinding. */
                const uint32_t k_exit_bits = (uint32_t)TD5_INPUT_PAUSE |
                                             (uint32_t)TD5_INPUT_CAMERA_CHANGE;
                for (int pi = 0; pi < players; pi++) {
                    if (s_input_source[pi] == 0) continue;   /* keyboard slot */
                    TD5_InputState pst;
                    memset(&pst, 0, sizeof(pst));
                    td5_plat_input_poll(pi, &pst);
                    if (pst.buttons & k_exit_bits) { exit_btn_now = 1; break; }
                }
            }

            if (exit_btn_now && !s_replay_exit_btn_held) {
                s_replay_exit_requested = 1;            /* one-shot edge */
                s_control_bits[0] |= 0x40000000u;       /* ESCAPE bit, belt-and-suspenders */
                TD5_LOG_I(LOG_TAG, "Replay: controller Back/Start -> exit requested");
            }
            s_replay_exit_btn_held = exit_btn_now;
        }
        goto post_poll;
    }

    /* ---- Normal polling ---- */
    for (int i = 0; i < s_active_players; i++) {
        TD5_InputState state;
        memset(&state, 0, sizeof(state));

        s_control_bits[i] = 0;

        if (s_input_source[i] == 0) {
            /* Keyboard */
            td5_plat_input_poll(i, &state);
        } else {
            /* Joystick (source is 1-based index) */
            td5_plat_input_poll(i, &state);
        }

        s_control_bits[i] = state.buttons;
        s_analog_x[i] = state.analog_x;
        s_analog_y[i] = state.analog_y;

        /* Bit 28 = auto/manual gearbox toggle.
         * Orig 0x00402E60 derives actor+0x378 = ~(bits >> 28) & 1, then gates
         * the entire gear-up/gear-down block on `field_0x378 == 0` (i.e.
         * bit 28 must be SET for gear keys to do anything). The original game
         * defaults to auto and exposes a menu toggle; the port doesn't wire
         * that toggle, so the [GameOptions] AutoGearbox INI key is the only
         * way to switch. AutoGearbox=1 (default) → bit 28 clear → auto mode.
         * AutoGearbox=0 → bit 28 set → manual (player gear keys honored).
         *
         * (The previous mapping of bit 28 to `s_nitro_pending` was dead code —
         * the setter was never called — and misnamed a real gearbox flag.) */
        /* Drag race FORCES MANUAL for the human player [CONFIRMED chain
         * 0x0042c51e OR bit28 -> 0x00402e97 actor+0x378=0 -> 0x00404529 auto-gear
         * call skipped]. This loop only iterates active human players
         * (i < s_active_players), so the AI opponent in drag is unaffected.
         * Otherwise honor the [GameOptions] AutoGearbox INI key.
         *
         * [#2 2026-06-15] The car-select MANUAL/AUTOMATIC toggle now also drives
         * this: if THIS player picked MANUAL in the menu, set the manual bit even
         * when AutoGearbox=1 (per-player in split-screen via
         * td5_frontend_get_player_manual). Gated by TD5RE_MANUAL_GEARBOX (default
         * ON; "0" reverts to the legacy auto_gearbox/drag-only behaviour). */
        {
            static int s_manual_gearbox = -1;
            if (s_manual_gearbox < 0) {
                const char *e = getenv("TD5RE_MANUAL_GEARBOX");
                s_manual_gearbox = (e && e[0] == '0' && e[1] == '\0') ? 0 : 1;
                TD5_LOG_I(LOG_TAG, "menu manual gearbox (#2) %s (TD5RE_MANUAL_GEARBOX=%s)",
                          s_manual_gearbox ? "ENABLED" : "disabled", e ? e : "default");
            }
            int want_manual = !g_td5.ini.auto_gearbox || g_td5.drag_race_enabled ||
                              (s_manual_gearbox && td5_frontend_get_player_manual(i));
            if (want_manual)
                s_control_bits[i] |=  0x10000000u;   /* manual (gear keys honored) */
            else
                s_control_bits[i] &= ~0x10000000u;   /* auto */
        }

        /* Camera change with cooldown */
        if (s_camera_cooldown[i] > 0) {
            s_camera_cooldown[i]--;
        }
        if ((s_control_bits[i] & TD5_INPUT_STEER_LEFT /*reuse check*/) != 0) {
            /* actual camera check below */
        }
        if (((s_control_bits[i] & 0x1000000u) != 0) &&
            (s_camera_cooldown[i] == 0) &&
            (!s_replay_mode_flag) &&
            (!s_escape_fade_active))
        {
            /* Original (0x0042C470, per-player do..while over iVar2):
             *   CycleRaceCameraPreset(iVar2, 1);
             *   LoadCameraPresetForView(slot*0x388+0x4ab310, 0, 0, 1);
             *
             * The 2nd arg (force_reload) is literally 0 — this is the
             * view-switch TRANSITION. LoadCameraPresetForView only SNAPS the
             * chase-spring accumulators (radius/height/orbit-angle/pitch) when
             * force_reload != 0 OR the preset MODE changes (0x00401450:543).
             * Chase presets 0-5 all share mode 0, so with force_reload=0 the
             * accumulators are left intact and UpdateChaseCamera's per-frame
             * spring (0x00401590) GLIDES from the old camera pose to the new
             * one. That smooth convergence IS the original's camera-change
             * animation — there is no separate blend timer. Passing 1 (as the
             * port previously did, from a mis-transcribed comment) hard-snaps
             * every switch and destroys the glide.
             *
             * view-index: original hard-codes 0; port keeps `i` so the per-view
             * preset arrays stay paired with CycleRaceCameraPreset(i). Identical
             * to the original in single-player (loop var is always 0 there);
             * only relevant to split-screen, which is out of scope here. */
            /* [SPLIT-SCREEN BUTTON ROUTING 2026-06-27] Cycle the camera of the pane
             * that SHOWS this player's car (view_for_player(i)), not pane i. When the
             * MP position picker permutes panes these differ; identity otherwise. */
            int cam_view = view_for_player(i);
            CycleRaceCameraPreset(cam_view, 1);
            {
                int slot = g_actorSlotForView[cam_view];
                TD5_Actor *actor = td5_game_get_actor(slot);
                if (!actor && g_actor_table_base && slot >= 0 &&
                    slot < td5_game_get_total_actor_count()) {
                    actor = (TD5_Actor *)(g_actor_table_base +
                                          (size_t)slot * TD5_ACTOR_STRIDE);
                }
                if (actor) {
                    LoadCameraPresetForView(
                        (int)((uint8_t *)actor + 0x208), 0, cam_view, 1);
                }
            }
            s_camera_cooldown[i] = TD5_INPUT_CAMERA_COOLDOWN;
            TD5_LOG_I(LOG_TAG, "camera change: player=%d view=%d slot=%d",
                      i, cam_view, g_actorSlotForView[cam_view]);
        }

        /* Rear view flag. [SPLIT-SCREEN BUTTON ROUTING 2026-06-27] Apply to the pane
         * that SHOWS this player's car (view_for_player(i)), not pane i — same reason
         * as the camera-change block above. The latch stays keyed by player i. */
        {
            int rv_view = view_for_player(i);
            if (((s_control_bits[i] & 0x2000000u) == 0) ||
                s_replay_mode_flag || s_escape_fade_active)
            {
                if (s_rear_view[i]) {
                    td5_camera_set_rear_view(rv_view, 0);
                    TD5_LOG_I(LOG_TAG, "rear view OFF: player=%d view=%d", i, rv_view);
                }
                s_rear_view[i] = 0;
            } else {
                if (!s_rear_view[i]) {
                    td5_camera_set_rear_view(rv_view, 1);
                    TD5_LOG_I(LOG_TAG, "rear view ON: player=%d view=%d", i, rv_view);
                }
                s_rear_view[i] = 1;
            }
        }

        /* [STUCK RECOVERY 2026-06-15] Manual car-recovery edge for this local
         * human player. Triggers: keyboard R (DIK_R = 0x13) and joystick SELECT
         * (Back / View = physical button 6 in the standard Xbox-via-DirectInput
         * layout; A0 B1 X2 Y3 LB4 RB5 Back6 Start7 L3=8 R3=9).
         *
         * This sits in the NORMAL (non-playback) poll branch, so it is inert
         * during replay watching — no collision with the replay-exit-on-R path
         * (td5_input_replay_exit_requested), which only runs in the playback
         * branch above.
         *
         * Keyboard R is honoured only for player 0: the slot-0 keyboard default
         * binds CHANGE VIEW to DIK_T (0x14), so R is free; the split-screen P2
         * keyboard default binds CHANGE VIEW to DIK_R (0x13), so claiming R for
         * recovery there would double-purpose that player's view key.
         *
         * [RECOVERY KEY -> SELECT 2026-06-24] SELECT (button 6) was the default
         * CHANGE VIEW joystick bind. To dedicate it to recovery without also
         * flipping the camera on every reset, CHANGE VIEW was moved off SELECT.
         * [CAMERA -> Y 2026-06-27] CHANGE VIEW now defaults to Y (button 3) — the
         * natural, discoverable camera button — and HORN/SIREN takes the L3 /
         * left-stick-click slot (button 8). See k_default_js_action_bind in
         * td5_platform_win32.c. Net per-pad layout: SELECT recovers, Y changes
         * view, L3 honks, R3 is REAR VIEW, Start is PAUSE.
         *
         * Edge-triggered via s_recovery_held so a held button recovers once per
         * press. Gated by TD5RE_STUCK_RECOVERY (cached): "0" never arms the
         * latch, so the whole feature (manual + the physics auto path) is off. */
        {
            static int s_recovery_init = 0;
            static int s_recovery_enabled = 1;
            if (!s_recovery_init) {
                s_recovery_enabled = td5_env_flag_on("TD5RE_STUCK_RECOVERY");  /* default ON */
                s_recovery_init = 1;
                TD5_LOG_I(LOG_TAG, "Stuck recovery (manual R / SELECT): %s",
                          s_recovery_enabled ? "enabled" : "disabled");
            }

            int recover_now = 0;
            if (s_recovery_enabled && !s_replay_mode_flag && !s_escape_fade_active) {
                if (s_input_source[i] == 0) {
                    /* Keyboard player — R only for player 0 (see note above). */
                    if (i == 0 && td5_plat_input_key_pressed(0x13))  /* DIK_R */
                        recover_now = 1;
                } else {
                    /* Joystick player — SELECT / Back (physical button 6). The
                     * device slot index equals the player index for the in-race
                     * poll. CHANGE VIEW's default joystick bind was moved off
                     * button 6 (now on Y / button 3) so SELECT is dedicated to
                     * recovery and never also flips the camera — see
                     * k_default_js_action_bind in td5_platform_win32.c. */
                    if (td5_plat_input_joystick_buttons(i) & (1u << 6))
                        recover_now = 1;
                }
            }

            if (i >= 0 && i < TD5_MAX_HUMAN_PLAYERS) {
                if (recover_now && !s_recovery_held[i]) {
                    s_recovery_request[i] = 1;       /* one-shot edge */
                    TD5_LOG_I(LOG_TAG, "recovery requested: player=%d", i);
                }
                s_recovery_held[i] = (uint8_t)recover_now;
            }
        }
    }

    /* [LANE ASSIST 2026-06-28] Keyboard 'L' (DIK_L = 0x26) toggles the optional
     * lane-assist steering aid for player 0 at runtime (rising edge so a held key
     * doesn't strobe). The aid is a DRIVING aid keyed per-driver (player 0 ==
     * actor slot 0); [Input] LaneAssist sets the per-session default for every
     * human player. Kept out of the dev-only #ifdef block so it works in release
     * too (accessibility). L is unbound for player 0 (P0 keyboard drives with
     * arrows + R recover / T change-view). */
    {
        static int s_la_held = 0;
        int la_now = td5_plat_input_key_pressed(0x26);  /* DIK_L */
        if (la_now && !s_la_held) {
            TD5_LOG_I(LOG_TAG, "L key edge -> lane-assist toggle(0)");
            td5_laneassist_toggle(0);
        }
        s_la_held = la_now;
    }

    /* Force feedback update for local players.
     * [CONFIRMED @ 0x0042C470]: UpdateControllerForceFeedback called here
     * inside PollRaceSessionInput in original RunRaceFrame. */
    td5_input_ff_update();

    /* Record input (delta-compressed, strip camera/escape bits) */
    td5_input_write_frame(s_control_bits[0], s_control_bits[1], 1);

post_poll:
    s_poll_log_counter++;
    if ((s_poll_log_counter % 30u) == 0u) {
        TD5_LOG_D(LOG_TAG, "Race session poll p0: raw_bits=0x%08X",
                  (unsigned int)s_control_bits[0]);
    }

    /* F3 key edge detection [CONFIRMED @ 0x42C6FC-0x42C71F in FUN_0042C470].
     * Original: rising edge sets DAT_004aaf93 (latch), falling edge clears
     * latch and pulses DAT_004aadf0=1. Reader (FUN_0042b580 main loop) clears
     * DAT_004aadf0 immediately with no other action — effectively dead code. */
    if (td5_plat_input_key_pressed(0x3D)) {
        s_f3_held = 1;
    } else if (s_f3_held) {
        s_f3_held = 0;
        TD5_LOG_D(LOG_TAG, "F3 release: camera cycle pulse (no-op in original)");
    }

    /* F12 — toggle the collision-wireframe overlay (DIK_F12 = 0x58). Rising
     * edge only so a held key doesn't strobe the flag. Mirrors the
     * --DebugCollisions CLI / [Debug] Collisions INI setting.
     * Dev-only affordance — compiled out of the release build. */
#ifndef TD5RE_RELEASE
    {
        static int s_prev_f12 = 0;
        int f12_now = td5_plat_input_key_pressed(0x58);
        if (f12_now && !s_prev_f12) {
            g_td5.ini.debug_collisions = !g_td5.ini.debug_collisions;
            TD5_LOG_I(LOG_TAG, "F12: collision wireframe %s",
                      g_td5.ini.debug_collisions ? "ON" : "OFF");
        }
        s_prev_f12 = f12_now;
    }

    /* [S27] F9 (DIK_F9 = 0x43) — simulate a controller disconnect for player 0,
     * to exercise the disconnect pause + per-viewport reconnect modal without a
     * physical unplug. Rising edge toggles it (press once to "drop" the pad,
     * again to "reconnect"). Dev-only affordance — compiled out of release. */
    {
        static int s_prev_f9 = 0;
        int f9_now = td5_plat_input_key_pressed(0x43);
        if (f9_now && !s_prev_f9)
            td5_game_debug_toggle_sim_device_loss(0);
        s_prev_f9 = f9_now;
    }
#endif

    /* F11 — toggle the PENDING TO TEST in-game overlay (DIK_F11 = 0x57). Rising
     * edge only. Mirrors the IN-GAME OVERLAY button on the PENDING TO TEST menu.
     * Present in dev AND release (the feature ships in both builds). */
    {
        static int s_prev_f11 = 0;
        int f11_now = td5_plat_input_key_pressed(0x57);
        if (f11_now && !s_prev_f11) {
            td5_pending_toggle_overlay();
            TD5_LOG_I(LOG_TAG, "F11: pending-test overlay %s",
                      td5_pending_overlay_on() ? "ON" : "OFF");
        }
        s_prev_f11 = f11_now;
    }

    /* Escape handling: trigger race exit fade */
    if ((s_control_bits[0] & 0x40000000u) != 0 &&
        !s_escape_muted &&
        !s_playback_active &&
        !s_replay_mode_flag &&
        !s_escape_fade_active)
    {
        s_escape_muted = 1;
        /* Original calls DXSound::MuteAll() and sets fade state */
    }

    /* F4/F5 camera cycling for both player slots */
    for (int i = 0; i < 2; i++) {
        if (td5_plat_input_key_pressed(0x3E + i) &&
            (s_camera_cooldown[i] == 0) &&
            !s_replay_mode_flag &&
            !s_escape_fade_active)
        {
            CycleRaceCameraPreset(i, 1);
            {
                int slot = g_actorSlotForView[i];
                TD5_Actor *actor = td5_game_get_actor(slot);
                if (!actor && g_actor_table_base && slot >= 0 &&
                    slot < td5_game_get_total_actor_count()) {
                    actor = (TD5_Actor *)(g_actor_table_base +
                                          (size_t)slot * TD5_ACTOR_STRIDE);
                }
                if (actor) {
                    /* force_reload = 0 → smooth spring glide, matching orig
                     * F4/F5 path: LoadCameraPresetForView(...,0,0,1) @0x0042C470.
                     * (Same transition rationale as the camera-button branch
                     * above.) */
                    LoadCameraPresetForView(
                        (int)((uint8_t *)actor + 0x208), 0, i, 1);
                }
            }
            s_camera_cooldown[i] = TD5_INPUT_CAMERA_COOLDOWN;
            TD5_LOG_I(LOG_TAG, "camera cycle F%d: player=%d", 4 + i, i);
        }
    }
}

/* ========================================================================
 * UpdatePlayerVehicleControlState  (0x402E60)
 *
 * Decodes the per-player bitmask into steering_command, throttle, brake,
 * gear changes, handbrake, and NOS.
 *
 * Two completely different code paths:
 *   Path A: Digital input (keyboard / digital joystick) -- bit 31 clear
 *   Path B: Analog input (joystick with packed X-axis) -- bit 31 set
 * ======================================================================== */

void td5_input_update_player_control(int slot)
{
    if (slot < 0 || slot >= TD5_MAX_RACER_SLOTS) return;

    uint32_t bits = s_control_bits[slot];

    /*
     * Read external actor state that the original code accesses.
     * In the full port these come from the physics/actor module.
     * For now we use the local control state arrays.
     */

    /* Stunned flag: bit 28 clear -> stunned=1, set -> stunned=0.
     * Original: actor+0x378 = ~(bits >> 28) & 1 */
    s_nos_cooldown[slot] = (uint8_t)((s_nos_cooldown[slot] > 0) ?
                                      s_nos_cooldown[slot] - 1 : 0);

    /* ---- Steering ---- */

    /*
     * Speed-dependent steering rate and limit.
     *
     * Original reads (0x402EBF-0x402ECD):
     *   speed     = abs(actor+0x314) >> 8  (longitudinal_speed, signed 24.8 FP)
     *   speed_sq  = actor+0x31C >> 8       (front_axle_slip_excess, written each tick)
     *
     * steer_rate_denom = (speed * 0x40) / (speed_sq^2 + 0x40)
     *   No slip:   denom = speed   → rate = NUM/(speed+0x40), slower at high speed.
     *   High slip: denom → 0       → rate = NUM/0x40, faster corrections while sliding.
     * [CONFIRMED @ 0x402EC1: IMUL EBX,EAX — single square of speed_sq]
     */
    int speed = 0;
    int speed_sq = 0;
    int vehicle_stopped = 1;
    int encounter_active = 0;
    {
        TD5_Actor *actor = td5_game_get_actor(slot);
        if (actor) {
            uint8_t *abytes = (uint8_t *)actor;
            /* +0x314 = longitudinal_speed (int32, signed 24.8 FP) */
            int32_t raw_speed = *(int32_t *)(abytes + 0x314);
            speed = (raw_speed < 0 ? -raw_speed : raw_speed) >> 8;
            /* +0x31C = front_axle_slip_excess (int32), written by player physics each tick */
            speed_sq = *(int32_t *)(abytes + 0x31C) >> 8;
            vehicle_stopped = (speed < 100) ? 1 : 0;
        }
        /* Per-slot encounter latch [CONFIRMED @ 0x00403180].
         * Original gate: gActorSpecialEncounterActive[slot * 0x11c] != 0.
         * Bug pre-fix: used td5_game_is_wanted_mode() (a global mode flag),
         * which is true for the entire Cop Chase session. That routed every
         * frame into the encounter-control branch below and prevented the
         * normal throttle write — Cop Chase player car never accelerated. */
        encounter_active = td5_ai_is_encounter_active(slot);
    }

    int steer_rate_denom;
    if (vehicle_stopped) {
        steer_rate_denom = 0;
    } else {
        /* Original (0x402EBF-0x402ECD):
         *   iVar8 = actor[0x31C] >> 8  (already in speed_sq)
         *   EBX = iVar8 * iVar8        (squared once = quadratic)
         *   EAX = abs_speed * 0x40
         *   result = EAX / (EBX + 0x40)
         * [CONFIRMED @ 0x402EC1: IMUL EBX,EAX — single square, not 4th power] */
        steer_rate_denom = (speed * 0x40) / (speed_sq * speed_sq + 0x40);
    }
    int steer_rate = (int)(TD5_INPUT_STEER_RATE_NUM /
                           (int64_t)(steer_rate_denom + 0x40));

    /* Speed-dependent steering limit */
    int effective_speed = vehicle_stopped ? 0 : speed;
    int steer_limit = (int)(TD5_INPUT_STEER_LIMIT_NUM /
                            (int64_t)(effective_speed + 0x80));

    /* Clamp limit to absolute max */
    int pos_limit = steer_limit;
    if (pos_limit > TD5_INPUT_STEER_MAX) pos_limit = TD5_INPUT_STEER_MAX;
    int neg_limit = -steer_limit;
    if (neg_limit < -TD5_INPUT_STEER_MAX) neg_limit = -TD5_INPUT_STEER_MAX;

    /* Path correction bias [CONFIRMED @ 0x00402F12-0x00402FB1].
     * Orig computes:
     *   motion_angle = AngleFromVector12(lin_vel_x>>8, lin_vel_z>>8)
     *   angle_diff   = ((motion_angle - (yaw_accum>>8)) - 0x800) & 0xFFF) - 0x800
     *                  (wrap to signed 12-bit, range -0x800..+0x7FF)
     *   if (rear_axle_slip < 0x800):
     *       path_bias_fp0 = (speed_long * angle_diff) >> 11  (signed div by 0x800)
     *   else:
     *       path_bias_fp0 = 0
     *   path_bias_fp8 = path_bias_fp0 << 8
     *   pos_limit_effective = clamp(max(path_bias_fp8,  steer_limit), ..., +STEER_MAX)
     *   neg_limit_effective = clamp(min(path_bias_fp8, -steer_limit), -STEER_MAX, ...)
     * Then digital/analog steering uses pos/neg_limit_effective.
     * Without this, port had only speed-dependent ±steer_limit and the player
     * lost the drift-driven steering-range expansion (= "stiff handling"). */
    {
        int32_t path_bias_fp8 = 0;
        TD5_Actor *actor = td5_game_get_actor(slot);
        if (actor) {
            uint8_t *abytes = (uint8_t *)actor;
            int32_t vx = (*(int32_t *)(abytes + 0x1cc)) >> 8;
            int32_t vz = (*(int32_t *)(abytes + 0x1d4)) >> 8;
            int32_t motion_angle = AngleFromVector12((int)vx, (int)vz);
            int32_t yaw_h = (*(int32_t *)(abytes + 0x1f4)) >> 8;
            int32_t angle_diff = ((motion_angle - yaw_h) - 0x800) & 0xFFF;
            angle_diff -= 0x800;  /* signed 12-bit, -0x800..+0x7FF */
            int32_t rear_slip = (*(int32_t *)(abytes + 0x31c)) >> 8;
            (void)rear_slip;  /* gate is on +0x320 in orig, not +0x31C */
            int32_t rear_slip_x = (*(int32_t *)(abytes + 0x320));
            int32_t speed_long  = (*(int32_t *)(abytes + 0x314));
            int32_t abs_long = speed_long < 0 ? -speed_long : speed_long;
            if (rear_slip_x < 0x800) {
                int64_t prod = (int64_t)abs_long * (int64_t)angle_diff;
                int32_t path_bias_fp0 = (int32_t)(prod >> 11);
                path_bias_fp8 = path_bias_fp0 << 8;
            }
        }
        /* Expand pos_limit if path_bias is more positive than speed-cap */
        if (path_bias_fp8 > pos_limit) {
            pos_limit = path_bias_fp8;
            if (pos_limit > TD5_INPUT_STEER_MAX) pos_limit = TD5_INPUT_STEER_MAX;
        }
        /* Expand neg_limit if path_bias is more negative than -speed-cap */
        if (path_bias_fp8 < neg_limit) {
            neg_limit = path_bias_fp8;
            if (neg_limit < -TD5_INPUT_STEER_MAX) neg_limit = -TD5_INPUT_STEER_MAX;
        }
    }

    if ((bits & TD5_INPUT_ANALOG_X_FLAG) == 0) {
        /* ---- Path A: Digital steering ---- */

        /* Steer left (bit 1 = 0x02 in original; our enum has LEFT=0x01, RIGHT=0x02) */
        if (bits & TD5_INPUT_STEER_RIGHT) {
            /* Ramp up */
            if (s_steer_ramp[slot] < TD5_INPUT_STEER_RAMP_MAX) {
                s_steer_ramp[slot] += TD5_INPUT_STEER_RAMP_STEP;
            }

            int cmd = s_steering_cmd[slot];
            if (cmd < 0) {
                /* Steering was left, snap-correct right */
                cmd += steer_rate * 2;
            } else {
                /* Continue right */
                cmd += (int)(((int32_t)s_steer_ramp[slot] * steer_rate +
                              ((int32_t)s_steer_ramp[slot] * steer_rate >> 31 & 0xFF))
                             >> 8);
            }
            if (cmd > pos_limit) cmd = pos_limit;
            s_steering_cmd[slot] = cmd;
        }

        if (bits & TD5_INPUT_STEER_LEFT) {
            /* Ramp up */
            if (s_steer_ramp[slot] < TD5_INPUT_STEER_RAMP_MAX) {
                s_steer_ramp[slot] += TD5_INPUT_STEER_RAMP_STEP;
            }

            int cmd = s_steering_cmd[slot];
            if (cmd > 0) {
                /* Steering was right, snap-correct left */
                cmd += steer_rate * -2;
            } else {
                /* Continue left */
                cmd -= (int)(((int32_t)s_steer_ramp[slot] * steer_rate +
                              ((int32_t)s_steer_ramp[slot] * steer_rate >> 31 & 0xFF))
                             >> 8);
            }
            if (cmd < neg_limit) cmd = neg_limit;
            s_steering_cmd[slot] = cmd;
        }

        /* No steering input -- auto-center */
        if ((bits & (TD5_INPUT_STEER_LEFT | TD5_INPUT_STEER_RIGHT)) == 0) {
            if (s_steer_ramp[slot] > 0) {
                s_steer_ramp[slot] -= TD5_INPUT_STEER_RAMP_STEP;
            }

            int cmd = s_steering_cmd[slot];
            int center_rate = steer_rate * TD5_INPUT_AUTOCENTER_MULT;

            if (cmd > -center_rate && cmd < center_rate) {
                s_steering_cmd[slot] = 0;
            } else if (cmd > 0) {
                s_steering_cmd[slot] = cmd - center_rate;
            } else {
                s_steering_cmd[slot] = cmd + center_rate;
            }
        }
    } else {
        /* ---- Path B: Analog steering ---- */
        int raw_x = (int)(bits & 0x1FF) - TD5_INPUT_JS_AXIS_CENTER;

        /* [PORT ENHANCEMENT 2026-06-14 task#15] Non-linear (expo) steering curve.
         * The raw analog stick axis is desensitised in the small/mid range so a
         * gentle stick deflection steers gently, while FULL deflection still
         * reaches full lock. Applies ONLY to analog stick steering (this Path B);
         * digital/keyboard steering (Path A above) is untouched and stays
         * full/instant. Knob TD5RE_STEER_EXPO (cached once):
         *   0   = linear  -> byte-identical to the faithful old behaviour
         *   0.6 = default -> out = (1-e)*x + e*x^3, sign- and endpoint-preserving
         * The curve is applied on the normalised deflection x = raw_x/250 in
         * [-1..+1] (x=0 -> 0, x=+/-1 -> +/-1 exactly), then mapped back into the
         * same +/-250 axis domain so the existing speed-dependent limit scale and
         * the fixed-point divide-by-250 below are reused unchanged. Any deadzone
         * the platform layer already applied to the packed axis survives: x near 0
         * maps to an even smaller output, never larger. */
        static int   s_steer_expo_init = 0;
        static float s_steer_expo = 0.6f;          /* default expo strength */
        if (!s_steer_expo_init) {
            s_steer_expo = td5_env_float("TD5RE_STEER_EXPO", 0.6f, 0.0f, 1.0f);
            s_steer_expo_init = 1;
            TD5_LOG_I(LOG_TAG, "Analog steer expo: strength=%.3f (%s)",
                      s_steer_expo,
                      (s_steer_expo > 0.0f) ? "expo" : "linear");
        }

        int eff_x = raw_x;
        if (s_steer_expo > 0.0f && raw_x != 0) {
            /* Normalise by the +250 half-range; the axis can read slightly past
             * +/-1 (raw 0..0x1FF vs centre 250) so clamp x to [-1..+1] to keep
             * the cubic monotonic and the endpoint exact. */
            float x = (float)raw_x / (float)TD5_INPUT_JS_AXIS_CENTER;
            if (x > 1.0f) x = 1.0f;
            if (x < -1.0f) x = -1.0f;
            float e = s_steer_expo;
            float curved = (1.0f - e) * x + e * (x * x * x);  /* sign preserved */
            int mapped = (int)(curved * (float)TD5_INPUT_JS_AXIS_CENTER +
                               (curved >= 0.0f ? 0.5f : -0.5f));   /* round to nearest */
            /* Preserve the saturated endpoint magnitude (full lock) and the sign
             * of the raw input regardless of float rounding. */
            if (raw_x > 0 && mapped < 0) mapped = 0;
            if (raw_x < 0 && mapped > 0) mapped = 0;
            eff_x = mapped;
        }

        /*
         * Original uses a fixed-point multiply:
         *   steering_command = (eff_x * steer_limit) / divisor
         * The magic constant 0x10624DD3 is approx 1/250 in fixed-point.
         * Result: steering_command = eff_x * steer_limit / 250
         * (eff_x == raw_x when expo is linear, so the linear path is unchanged.)
         */
        int64_t magic = (int64_t)0x10624DD3;
        if (eff_x >= 0) magic = -(int64_t)0x10624DD3;
        int64_t hi = (magic * (int64_t)(eff_x * (eff_x < 0 ? neg_limit : pos_limit))) >> 32;
        s_steering_cmd[slot] = (int)((hi >> 4) - (hi >> 31));
    }

    /* ---- Throttle / Brake ---- */
    if (bits & TD5_INPUT_ANALOG_Y_FLAG) {
        /* Analog Y-axis */
        int raw_y = (int)((bits >> 9) & 0x1FF) - TD5_INPUT_JS_AXIS_CENTER;

        /* [PORT ENHANCEMENT 2026-06] PROPORTIONAL analog throttle/brake. The axis
         * (a trigger or stick Y) past the deadzone scales 0..0x100 instead of
         * snapping to full. Previously ANY deflection latched full throttle/brake
         * ("accelerate all the way"); now a trigger/stick gives partial drive.
         * Span = [deadzone .. axis_center] mapped to [0 .. 0x100]. */
        {
            int span = TD5_INPUT_JS_AXIS_CENTER - TD5_INPUT_ANALOG_Y_DEADZONE; /* 250-10 */
            if (span < 1) span = 1;
            TD5_Actor *a_actor = td5_game_get_actor(slot);
            if (raw_y > -TD5_INPUT_ANALOG_Y_DEADZONE &&
                raw_y < TD5_INPUT_ANALOG_Y_DEADZONE)
            {
                /* Deadzone -- no throttle or brake. Reset to forward, mirroring the
                 * digital no-input branch (so releasing the brake exits reverse). */
                s_throttle[slot] = 0;
                s_brake[slot] = 0;
                s_reverse_req[slot] = 0;
                if (a_actor) ((uint8_t *)a_actor)[0x36F] = 1;
            } else if (raw_y >= TD5_INPUT_ANALOG_Y_DEADZONE) {
                /* Positive Y = brake/reverse, proportional to how far past the
                 * deadzone. The reverse latch mirrors the digital BRAKE path
                 * [CONFIRMED @ 0x4032A0-0x403300]: with an auto gearbox, once the
                 * car is nearly stopped (and field_0x376==0) holding the brake
                 * flips throttle_state (+0x36F) to 0 = reverse, so a negative
                 * throttle drives backward instead of just braking. Without this
                 * the joystick could brake but never reverse. */
                int mag = raw_y - TD5_INPUT_ANALOG_Y_DEADZONE;     /* 0..span */
                int t   = (mag * 0x100) / span;                    /* 0..0x100 */
                uint8_t throttle_st = 1, auto_gearbox = 0, sflags = 0;
                if (t > 0x100) t = 0x100;
                if (a_actor) {
                    uint8_t *ab2 = (uint8_t *)a_actor;
                    throttle_st  = ab2[0x36F];
                    auto_gearbox = ab2[0x378];
                    sflags       = ab2[0x376];
                }
                if (auto_gearbox != 0 && throttle_st == 1 && speed < 10 && sflags == 0)
                    throttle_st = 0;                 /* -> reverse */
                s_brake[slot]    = throttle_st;      /* 1=brake, 0=reverse (no brake light) */
                s_throttle[slot] = (int16_t)(-t);    /* proportional negative = brake/reverse force */
                s_reverse_req[slot] = 0;
                if (a_actor) ((uint8_t *)a_actor)[0x36F] = throttle_st;
            } else {
                /* Negative Y = accelerate, proportional. Reset throttle_state to
                 * forward (mirror the digital no-brake branch). */
                int mag = (-raw_y) - TD5_INPUT_ANALOG_Y_DEADZONE;  /* 0..span */
                int t   = (mag * 0x100) / span;                    /* 0..0x100 */
                if (t > 0x100) t = 0x100;
                s_throttle[slot] = (int16_t)t;     /* 0..0x100 forward (0x100 = full) */
                s_brake[slot] = 0;
                s_reverse_req[slot] = 0;
                if (a_actor) ((uint8_t *)a_actor)[0x36F] = 1;
            }
        }
    } else {
        /* Digital throttle/brake */
        if (bits & TD5_INPUT_BRAKE) {
            /* Brake / reverse latch — matches original at 0x4032A0-0x403300.
             *
             * The original uses throttle_state (+0x36F) as a two-state latch:
             *   1 = forward/braking mode (brake_flag=1)
             *   0 = reverse mode (brake_flag=0, physics takes drive path)
             *
             * Transition to reverse: auto gearbox (+0x378!=0) AND currently
             * forward (throttle_state==1) AND nearly stopped (speed<10) AND
             * not airborne (surface_contact_flags==0 means no ground contact,
             * which blocks transition).
             *
             * Once in reverse: brake_flag=0, throttle=0xFF00 (-256), so
             * auto_gear_select sees throttle<0 → gear=REVERSE, and the
             * negative throttle through compute_drive_torque produces
             * backward force. [CONFIRMED @ 0x4032A0-0x403300] */
            {
                TD5_Actor *a_actor = td5_game_get_actor(slot);
                uint8_t throttle_st = 1;  /* default forward */
                uint8_t auto_gearbox = 0;
                uint8_t sflags = 0;
                if (a_actor) {
                    uint8_t *ab = (uint8_t *)a_actor;
                    throttle_st = ab[0x36F];
                    auto_gearbox = ab[0x378];
                    sflags = ab[0x376];
                }

                /* Reverse transition [CONFIRMED @ 0x4032A0-0x4032BC]:
                 * original checks field_0x376 == 0 (no surface contact). */
                if (auto_gearbox != 0 && throttle_st == 1 &&
                    speed < 10 && sflags == 0)
                {
                    TD5_LOG_I(LOG_TAG,
                              "brake->reverse latch: speed=%d slot=%d sflags=0x%X",
                              speed, slot, sflags);
                    throttle_st = 0;
                }

                /* brake_flag mirrors throttle_state [CONFIRMED @ 0x4032E8] */
                s_brake[slot] = throttle_st;
                s_throttle[slot] = (int16_t)0xFF00;
                s_reverse_req[slot] = 0;

                /* Write throttle_state back immediately */
                if (a_actor) {
                    ((uint8_t *)a_actor)[0x36F] = throttle_st;
                }
            }
        } else {
            /* No brake: reset throttle_state to forward [CONFIRMED @ 0x403180] */
            s_brake[slot] = 0;
            s_reverse_req[slot] = 0;
            {
                TD5_Actor *a_actor = td5_game_get_actor(slot);
                if (a_actor) {
                    ((uint8_t *)a_actor)[0x36F] = 1;  /* reset to forward */
                }
            }

            if (bits & TD5_INPUT_THROTTLE) {
                s_throttle[slot] = 0x100;  /* original DAT_0046317C = 0x0100 [CONFIRMED @ 0x403182] */
            } else {
                s_throttle[slot] = 0;
            }
        }

    }

    /* ---- Handbrake (bit 0x100000) ----
     * Faithful to UpdatePlayerVehicleControlState's shared tail block
     * [CONFIRMED @ 0x004033fb-0x00403422]: when the handbrake bit is set —
     * on BOTH the analog and digital throttle paths, which is why this is
     * unified here AFTER the if/else rather than nested in the digital arm —
     * the original writes three actor fields and OVERRIDES whatever
     * throttle/brake the decode above produced:
     *   actor+0x33E (encounter_steering_cmd / throttle) = 0xFF00 (-256)
     *   actor+0x36D (brake_flag)                        = 1
     *   actor+0x36E (handbrake_flag)                    = 1
     * Setting brake_flag turns the brake lights on (the taillight gate at
     * RenderVehicleTaillightQuads 0x004011C0 reads +0x36D) and routes the
     * physics through the on-ground brake-deceleration path; the -256 throttle
     * gives that path a non-zero retarding force (bf = tuning[0x6E] * throttle
     * >> 8 @ 0x004043f0). The rear-grip cut (tuning+0x7A) and slip-z gate in
     * td5_physics still key off handbrake_flag. When NOT pressed,
     * handbrake_flag is cleared to 0 [CONFIRMED @ the (bits&0x100000)==0
     * branch].
     *
     * Earlier port comment claimed handbrake "does NOT brake the car or shift
     * to reverse" and set only handbrake_flag — that diverged from the
     * original (no deceleration, no brake lights). This restores the faithful
     * behavior. */
    if (bits & 0x100000u) {
        s_handbrake[slot] = 1;                 /* rear-grip cut + slip-z gate — always */
        /* [FIX 2026-06-15 item #8] TD5RE_HANDBRAKE_BRAKE (default ON): the handbrake
         * must actually slow the car even while accelerating. When ON, always engage
         * the faithful original brake path (throttle=-256, brake=1) regardless of the
         * throttle bit — restoring the literal original (@0x004033fb always forces
         * throttle=-256+brake). The rear-grip cut via s_handbrake still gives slidey
         * handbrake turns. When set to "0", revert to the donut/power-slide deviation
         * below (throttle+handbrake keeps forward drive, no brake). */
        static int s_hb_brake_init = 0;
        static int s_hb_brake = 1;                 /* default ON = always brakes */
        if (!s_hb_brake_init) {
            s_hb_brake = td5_env_flag_on("TD5RE_HANDBRAKE_BRAKE");
            s_hb_brake_init = 1;
            TD5_LOG_I(LOG_TAG, "Handbrake-while-accel: %s",
                      s_hb_brake ? "always brakes" : "donut (throttle held, no brake)");
        }
        if (s_hb_brake || !(bits & TD5_INPUT_THROTTLE)) {
            /* Handbrake brakes the car to a stop (faithful). Default path; also the
             * handbrake-only (off throttle) path when the donut deviation is enabled. */
            s_throttle[slot]  = (int16_t)0xFF00;  /* -256: full brake throttle */
            s_brake[slot]     = 1;                 /* brake lights + brake path */
        } else {
            /* [donut deviation, TD5RE_HANDBRAKE_BRAKE=0] On throttle + handbrake: keep
             * the forward drive (do NOT override to the -256 brake throttle, do NOT set
             * brake_flag) so the rear breaks loose UNDER POWER -> the car power-
             * slides / donuts instead of braking to a stop. The literal original
             * always forces throttle=-256+brake (@0x004033fb), which precludes a
             * handbrake donut; this is a deliberate port deviation for that gameplay.
             * s_throttle/s_brake keep their decoded forward values (throttle=0x100,
             * brake=0 from the accelerate branch above). */
        }
        TD5_LOG_I(LOG_TAG, "handbrake ON slot=%d: throttle=%d brake=%d hb=1",
                  slot, (int)s_throttle[slot], (int)s_brake[slot]);
    } else {
        s_handbrake[slot] = 0;
    }

    /* [POLICE pullover 2026-06-19] When a cop has overtaken this player
     * (g_encounter_active[slot], set by the chase scheduler), force the car to
     * brake to a STOP immediately — regardless of throttle/brake input — and
     * hold at rest. Runs AFTER the normal throttle/brake + handbrake decode (the
     * same post-override pattern), so it overrides whatever the player pressed:
     * the brake starts the instant the cop passes, not when the player lets off.
     * throttle_state (+0x36F) is forced to 1 (forward) so the brake->reverse
     * latch never arms — the car brakes, it does NOT reverse. Once stopped it
     * holds with no brake (so auto-reverse can't engage) and no throttle, until
     * the cop releases (target reaches 0 speed) and encounter_active clears.
     *
     * [FIX 2026-07-02 police-brake-deadzone] This branch used to compare
     * `speed` (raw longitudinal speed >> 8) against a hardcoded 100 — copied
     * from the unrelated `vehicle_stopped` steering-formula guard above, not
     * from this feature's own "stopped" definition. The chase scheduler
     * (td5_ai.c td5_ai_update_special_encounter, COP_PULLOVER phase) only
     * actually releases the hold once |longitudinal_speed| <= COP_STOP_SPEED
     * (0x2000 raw == 32 in this shifted scale, ~10.5 km/h) — three times
     * lower than the old 100 (~33 km/h). Below ~33 km/h but above ~10.5 km/h
     * this branch cancelled the brake (coast, no throttle) while the AI side
     * was still holding the pullover active, so the car just coasted on drag
     * with zero control for that ~20-23 km/h band — "brakes stop working for
     * the last 20-30 km/h" — until it finally decayed past the real release
     * point. Comparing against COP_STOP_SPEED directly keeps both sides in
     * lockstep: the player keeps braking exactly until the scheduler is about
     * to release, then coast-holds for the remaining sliver down to rest. */
    if (encounter_active) {
        TD5_Actor *a_actor = td5_game_get_actor(slot);
        if (speed >= (COP_STOP_SPEED >> 8)) {
            s_throttle[slot] = (int16_t)0xFF00;  /* full brake (forward gear) */
            s_brake[slot]    = 1;
        } else {
            s_throttle[slot] = 0;                /* stopped: coast-hold, no brake */
            s_brake[slot]    = 0;
        }
        s_reverse_req[slot] = 0;
        s_handbrake[slot]   = 0;
        if (a_actor) ((uint8_t *)a_actor)[0x36F] = 1;  /* forward gear — never reverse */
        TD5_LOG_I(LOG_TAG, "police_pullover: slot=%d speed=%d stop_thresh=%d brake=%d",
                  slot, speed, (int)(COP_STOP_SPEED >> 8), (int)s_brake[slot]);
    }

    /* ---- NOS / Horn ---- */
    if (s_nos_enabled) {
        if ((bits & 0x200000u) == 0) {
            s_nos_latch[slot] = 0;
        } else if (s_nos_latch[slot] == 0 && s_nos_cooldown[slot] == 0) {
            /* Fire NOS boost.
             * Original doubles velocity X/Z, adds vertical impulse,
             * sets 15-frame cooldown.  The actual velocity modification
             * is done by the physics module; we just set the signal. */
            s_nos_cooldown[slot] = TD5_INPUT_NOS_COOLDOWN;
            s_nos_latch[slot] = 1;
        }
    }

    /* ---- Cop-chase siren toggle (PORT ENHANCEMENT, user-requested 2026-05-30) ----
     * In wanted mode the player drives the cop car (slot == cop actor index, =0).
     * The original keeps the siren on while the horn key (0x200000) is held; the
     * user prefers a press-to-toggle. Edge-detect the horn key here and flip the
     * siren on/off via td5_sound_toggle_siren(). The toggle couples siren audio
     * + the flashing-light marker (both live in the same horn-gated branch in
     * the original). Only the cop slot toggles; AI suspects never press horn.
     * [COP CHASE SIREN PER-COP 2026-06-27] Gate on td5_game_cop_chase_is_cop(slot)
     * (mask-aware) instead of the single representative cop slot so EVERY human
     * cop in a local MP cop chase toggles ITS OWN siren (td5_sound_toggle_cop_
     * siren) — the per-cop arrest rule reads the same per-cop state. */
    if (td5_game_is_wanted_mode() && td5_game_cop_chase_is_cop(slot)) {
        if ((bits & 0x200000u) == 0) {
            s_siren_horn_latch[slot] = 0;
        } else if (s_siren_horn_latch[slot] == 0) {
            s_siren_horn_latch[slot] = 1;
            td5_sound_toggle_cop_siren(slot);
        }
        /* The cop's horn IS the siren toggle — never also honk. */
        s_horn_honk_latch[slot] = 0;
    } else {
        s_siren_horn_latch[slot] = 0;

        /* ---- Regular-car horn honk (PORT ENHANCEMENT) ----
         * The original never played a per-car horn: bit 0x200000 only drove the
         * cop siren + a remote-brake cheat, and Horn.wav (loaded to slot i*3+2)
         * was never played (RE-confirmed @ 0x00440a30 / 0x00441a80). Wire the
         * port's existing (dead) horn-playback block: honk once per horn-key
         * press edge. Skipped for the cop-in-wanted-mode above (siren instead);
         * fires for every other human car, including suspects in a cop chase
         * and any car in a normal race. The honk is local cosmetic audio
         * (s_horn_state), so it carries no sim/netplay desync risk. */
        if ((bits & 0x200000u) == 0) {
            s_horn_honk_latch[slot] = 0;
        } else if (s_horn_honk_latch[slot] == 0) {
            s_horn_honk_latch[slot] = 1;
            td5_sound_play_horn(slot);
        }
    }

    /* Cop mode: horn zeroes other actors' velocities.
     * Deferred to physics module integration. */

    /* ---- Gear Changes (5-frame debounce) ----
     * Manual-mode gate [CONFIRMED @ 0x00402E60: orig wraps the entire gear
     * up/down block in `if (field_0x378 == 0)`. field_0x378 = ~(bits>>28)&1,
     * so field_0x378 == 0 ⟺ bit 28 set ⟺ MANUAL mode.
     * Port previously gated the OPPOSITE way — gear keys worked in auto
     * (default) mode and were ignored in manual. Fixed 2026-05-22 alongside
     * the [GameOptions] AutoGearbox INI key. */
    if ((bits & 0x10000000u) == 0) {
        /* Auto mode (default) — orig ignores gear up/down keys here. */
    } else if (s_gear_debounce[slot] == 0) {
        /* Gear up (bit 0x400000) */
        if (bits & 0x400000u) {
            /* Max gear from actor+0x36C [CONFIRMED @ 0x4035E0, DAT_004ab474].
             * Original condition: (uint8_t)actor[0x36B] < (uint8_t)(actor[0x36C]-1)
             * Original reads actor[0x36B] for current gear; port must sync from
             * the live actor so physics auto-gear updates are visible here. */
            {
                TD5_Actor *a_gear = td5_game_get_actor(slot);
                uint8_t max_gear = a_gear ? ((uint8_t *)a_gear)[0x36C] : 8u;
                if (max_gear == 0) max_gear = 8u;  /* guard: uninitialized actor */
                /* Sync current gear from live actor [CONFIRMED @ 0x402E60] */
                if (a_gear) s_gear[slot] = ((uint8_t *)a_gear)[0x36B];
                if (s_gear[slot] < (uint8_t)(max_gear - 1u)) {
                    s_gear[slot]++;
                    TD5_LOG_I(LOG_TAG, "gear up: slot=%d gear=%d max=%d",
                              slot, (int)s_gear[slot], (int)max_gear);

                    /* FUN_0042EEA0 [CONFIRMED @ 0x42EEA0-0x42EF06]:
                     * Gear-shift weight-transfer impulse — adds to front wheel-load
                     * accumulators and subtracts from rear (nose-dip on upshift).
                     * Formula: impulse = (actor[0x33E] * car_cfg[0x68] * 26 / 256)
                     *                    * gear_lut[gear] / 256
                     * gear_lut[8] = {0,0,256,192,128,64,32,16} @ DAT_00467394 */
                    {
                        static const int32_t k_gear_shift_lut[8] = {
                            0, 0, 256, 192, 128, 64, 32, 16
                        };
                        uint8_t *ab = (uint8_t *)a_gear;
                        void *car_cfg = (void *)(uintptr_t)*(uint32_t *)(ab + 0x1BC);
                        if (car_cfg) {
                            int spd_val = *(int16_t *)(ab + 0x33E);
                            int rpm_s   = *(int16_t *)((uint8_t *)car_cfg + 0x68);
                            int g       = (int)(s_gear[slot] & 7u);
                            int v = spd_val * rpm_s * 26;
                            v = ((v + (v >> 31 & 0xFF)) >> 8) * k_gear_shift_lut[g];
                            v = (v + (v >> 31 & 0xFF)) >> 8;
                            *(int32_t *)(ab + 0x2EC) += v;
                            *(int32_t *)(ab + 0x2F0) += v;
                            *(int32_t *)(ab + 0x2F4) -= v;
                            *(int32_t *)(ab + 0x2F8) -= v;
                        }

                        /* Gear-2 auto-gearbox check [CONFIRMED @ FUN_00402E60]:
                         * If RPM > 75% of redline when shifting into gear 2,
                         * write car_config+0x76 (auto-gearbox mode byte) into
                         * actor+0x376.
                         * car_config+0x72 = redline RPM (int16) [INFERRED from context]
                         * actor+0x310     = current RPM (int32) [CONFIRMED]
                         * actor+0x376     = auto-gearbox active flag (byte) [CONFIRMED] */
                        if (s_gear[slot] == 2 && car_cfg) {
                            int t = *(int16_t *)((uint8_t *)car_cfg + 0x72) * 3;
                            if (((t + (t >> 31 & 3)) >> 2) <
                                *(int32_t *)((uint8_t *)a_gear + 0x310))
                            {
                                ((uint8_t *)a_gear)[0x376] =
                                    *((uint8_t *)car_cfg + 0x76);
                                TD5_LOG_I(LOG_TAG,
                                    "gear2 autogear: slot=%d rpm=%d flag=0x%02X",
                                    slot,
                                    *(int32_t *)((uint8_t *)a_gear + 0x310),
                                    (unsigned)*((uint8_t *)car_cfg + 0x76));
                            }
                        }
                    }
                    /* Write new gear back to actor so physics sees manual shift
                     * [CONFIRMED @ 0x402E60: original writes actor[0x36B]] */
                    if (a_gear) ((uint8_t *)a_gear)[0x36B] = s_gear[slot];
                }
            }
            s_gear_debounce[slot] = TD5_INPUT_GEAR_DEBOUNCE;
        }

        /* Gear down (bit 0x800000) */
        if (bits & 0x800000u) {
            TD5_Actor *a_gdn = td5_game_get_actor(slot);
            /* Sync current gear from live actor before down-shift check */
            if (a_gdn) s_gear[slot] = ((uint8_t *)a_gdn)[0x36B];
            if (s_gear[slot] > 0) {
                /* Downshift RPM protection would go here --
                 * checks if RPM would exceed redline.
                 * Deferred to physics module. */
                s_gear[slot]--;
                if (a_gdn) ((uint8_t *)a_gdn)[0x36B] = s_gear[slot];
            }
            s_gear_debounce[slot] = TD5_INPUT_GEAR_DEBOUNCE;
        }
    } else {
        s_gear_debounce[slot]--;
    }

    /* ---- Write back to actor struct for physics ---- */
    {
        TD5_Actor *actor = td5_game_get_actor(slot);
        if (actor) {
            uint8_t *a = (uint8_t *)actor;

            /* AutoThrottle: force full gas + straight steering for slot 0.
             * Used for deterministic trace comparison (both port and original
             * get identical input). Enable via [Trace] AutoThrottle=1 in INI. */
            if (slot == 0 && g_td5.ini.auto_throttle) {
                static uint32_t s_at_log = 0;
                if (s_steering_cmd[slot] != 0 && (s_at_log++ % 30u) == 0u) {
                    TD5_LOG_I(LOG_TAG,
                              "AutoThrottle: zeroing steer=%d (bits=0x%X analog_x=%d)",
                              s_steering_cmd[slot], bits,
                              (bits & TD5_INPUT_ANALOG_X_FLAG) ? 1 : 0);
                }
                s_steering_cmd[slot] = 0;
                /* AutoThrottleStopSpan: once slot 0 reaches the target span,
                 * brake to a full stop so the car settles STATIONARY on the
                 * slope — for a clean at-rest WHEELGAP capture (the moving
                 * capture is contaminated by accel-bounce). 0 = drive forever. */
                int stop_span = g_td5.ini.auto_throttle_stop_span;
                int cur_span  = (int)*(int16_t *)(a + 0x80);
                if (stop_span > 0 && cur_span >= stop_span) {
                    s_throttle[slot] = 0;
                    s_brake[slot]    = 0x100;
                } else {
                    s_throttle[slot] = g_td5.ini.auto_throttle_value
                                           ? (int16_t)g_td5.ini.auto_throttle_value : 0x100;
                    s_brake[slot]    = 0;
                }
            } else if (slot == 0) {
                static uint32_t s_at_log2 = 0;
                if ((s_at_log2++ % 60u) == 0u) {
                    TD5_LOG_I(LOG_TAG,
                              "AutoThrottle INACTIVE: auto_throttle=%d steer=%d",
                              g_td5.ini.auto_throttle, s_steering_cmd[slot]);
                }
            }

            /* (Race-end auto-brake for the finished human player is applied in
             * td5_game.c RunRaceFrame, between td5_ai_tick and td5_physics_tick —
             * NOT here. The per-slot input update stops the moment a slot
             * finishes, so an input-layer lockout would never run post-finish. */

            /* 0x30C: steering_command (int32).
             *
             * Skip for slot 0 under PlayerIsAI=1. The AI cascade in
             * td5_ai_update_steering_bias (0x004340C0 port) is ADDITIVE:
             * `param_2 = ACTOR_STEERING_CMD + delta`. Writing back the
             * keyboard-derived s_steering_cmd[0] (= 0 with no human input)
             * here resets the cascade's accumulator every tick, pinning the
             * field at exactly the per-tick delta (e.g. -0x4000 from the
             * emergency-snap branch) instead of allowing it to saturate to
             * ±0x18000. Visible symptom (Edinburgh PlayerIsAI=1 2000-tick):
             * slot 0 locked at span 137 from sim_tick 440+, steering pinned
             * at -16384 = -0x4000. Letting the AI value survive ends the
             * lock-up. Other input fields (throttle, brake, gearbox) are
             * unaffected because AI writes them as absolute values after
             * input runs, so the input pre-write is overwritten. */
            /* [DRAG LANE-CHANGE 2026-06-29 — lane-assist] In drag race a human's
             * LEFT/RIGHT taps pick a TARGET LANE; the aggressive lane-assist
             * (td5_laneassist_apply, run from physics) steers the car into it with
             * a capped, damped pure-pursuit yaw (no spin, no shake). The raw wheels
             * carry NO steering input — the aid does all the steering. Tap anytime;
             * each edge moves the target one lane and the aid re-aims smoothly.
             * Knob: TD5RE_DRAG_AUTOSTEER=0 -> free-steer on the wide track instead. */
            if (g_td5.drag_race_enabled && slot < g_td5.num_human_players) {
                if (td5_env_flag_on("TD5RE_DRAG_AUTOSTEER")) {
                    int field    = td5_game_drag_field_size();
                    int cur_lane = (int)*(int8_t *)(a + 0x8C);    /* derived sub_lane */
                    int l_now    = (bits & TD5_INPUT_STEER_LEFT)  ? 1 : 0;
                    int r_now    = (bits & TD5_INPUT_STEER_RIGHT) ? 1 : 0;
                    int l_edge   = (l_now && !s_drag_prev_left[slot])  ? 1 : 0;
                    int r_edge   = (r_now && !s_drag_prev_right[slot]) ? 1 : 0;
                    s_drag_prev_left[slot]  = (uint8_t)l_now;
                    s_drag_prev_right[slot] = (uint8_t)r_now;

                    if (s_drag_target_lane[slot] < 0)
                        s_drag_target_lane[slot] = cur_lane;     /* lazy init = spawn lane */

                    if (l_edge ^ r_edge) {                       /* one lane per tap */
                        int nt = s_drag_target_lane[slot] + (r_edge ? 1 : -1);
                        if (nt < 0) nt = 0;
                        if (nt > field - 1) nt = field - 1;
                        s_drag_target_lane[slot] = nt;
                    }

                    s_steering_cmd[slot] = 0;                    /* lane-assist steers */
                }
            }

            if (!(slot == 0 && g_td5.ini.player_is_ai)) {
                *(int32_t *)(a + 0x30C) = s_steering_cmd[slot];
            }
            /* 0x33E: encounter_steering_cmd (int16) — used as throttle by physics */
            *(int16_t *)(a + 0x33E) = s_throttle[slot];
            /* 0x36D: brake_flag (byte) */
            a[0x36D] = s_brake[slot];
            /* 0x36E: handbrake_flag (byte) — was missing; without it physics
             * never sees the Q-key handbrake input, so rear-wheel grip
             * reduction in td5_physics_update_player (tuning+0x7A) never
             * fires and no drift-induced tire marks appear. */
            a[0x36E] = s_handbrake[slot];
            /* 0x378: auto gearbox flag [CONFIRMED @ original input path]
             * Original: actor+0x378 = ~(bits >> 28) & 1
             * bit 28 clear = normal driving → auto_gearbox = 1
             * bit 28 set   = stunned       → auto_gearbox = 0 */
            a[0x378] = (uint8_t)((~(bits >> 28)) & 1);
        }
    }

    if (slot == 0) {
        s_control_log_counter++;
        if ((s_control_log_counter % 30u) == 0u) {
            uint8_t actor_hb = 0;
            {
                TD5_Actor *a_dbg = td5_game_get_actor(0);
                if (a_dbg) actor_hb = ((uint8_t *)a_dbg)[0x36E];
            }
            TD5_LOG_I(LOG_TAG,
                      "Player control p0: steer=%d thr=%d brake=%u hb=%u actor_hb=%u bits=0x%08X",
                      (int)s_steering_cmd[0], (int)s_throttle[0],
                      (unsigned int)s_brake[0], (unsigned int)s_handbrake[0],
                      (unsigned int)actor_hb, (unsigned int)s_control_bits[0]);
        }
    }
}

/* [ARCH-DIVERGENCE: 2-dword globals collapsed into per-slot array; L5 sweep 2026-05-21]
 *   Orig 0x00402E30 zeroes two specific dwords:
 *     _g_remoteBrakeCheatPerSlotLatch = 0;     (DAT_00483014)
 *     _g_playerControlAccum1 = 0;              (DAT_00483018)
 *   Port stores NOS-latch state per-slot in s_nos_latch[TD5_MAX_RACER_SLOTS]
 *   rather than the orig's 2-dword pair. The port zeroes the whole array here
 *   to match the conceptual "reset all NOS accumulators" semantics. Orig's
 *   globals don't exist in port code (only as DAT_* aliases in
 *   td5_orig_globals.h:153-154 for cross-reference). Per-slot storage is the
 *   port's design choice driven by TD5_MAX_RACER_SLOTS=6 racer abstraction.
 *
 *   ResetPlayerVehicleControlAccumulators  (0x402E30) */
void td5_input_reset_accumulators(void)
{
    for (int i = 0; i < TD5_MAX_RACER_SLOTS; i++) {
        s_nos_latch[i] = 0;
    }
    /* [#18b replay-fix 2026-06-15] Reset per-race steering accumulators here too —
     * they were only zeroed at startup, so a replay (or any second race) inherited
     * the previous run's end-of-race steering state, diverging the first moments of
     * playback. A race must start steering-neutral. */
    memset(s_steering_cmd, 0, sizeof(s_steering_cmd));
    memset(s_steer_ramp,  0, sizeof(s_steer_ramp));
}

/* ========================================================================
 * ResetPlayerVehicleControlBuffers  (0x402E40)
 *
 * Zeroes the gear-change debounce counters for all 6 slots.
 * Original: memset(DAT_00482FE4, 0, 6 * 4)
 *
 * [CONFIRMED @ 0x00402E40] — Ghidra-verified: orig walks &gPlayerControlBuffer
 * with a 6-iteration loop writing 0 per int (6 × 4 bytes). Port loop matches
 * iteration count and per-element zero write; TD5_MAX_RACER_SLOTS == 6.
 * L5 promotion sweep 2026-05-21.
 * ======================================================================== */

void td5_input_reset_buffers(void)
{
    for (int i = 0; i < TD5_MAX_RACER_SLOTS; i++) {
        s_gear_debounce[i] = 0;
    }
}

/* ========================================================================
 * UpdatePlayerSteeringWeightBalance  (0x004036b0, 101 bytes)
 *
 * [CONFIRMED @ 0x004036b0; Tier 4 port 2026-05-24]
 *
 * Two-player split-screen steering-weight balance. Each frame the orig
 * compares an "input source signed value" between player 0 and player 1
 * (the orig reads two 16-bit scratch words inside the particle pool at
 * 0x4ab18c and 0x4ab514 — see g_raceParticlePoolBase). The deltas drive a
 * pair of 16-bit weights at g_player0SteeringBiasShort/g_player1SteeringBiasShort
 * (0x0046317c / 0x0046317e); UpdatePlayerVehicleControlState uses bit
 * 0x200 of the control bits to optionally apply those weights to the
 * encounter/lane control output at DAT_004ab446.
 *
 * Orig logic:
 *   delta = abs(p0_input - p1_input) * 2
 *   delta = min(delta, g_steeringBiasMaxSwing)
 *   if (p0 < p1)  // p1 stronger -> bias toward p0
 *     player0_weight = 0x100 + delta
 *     player1_weight = 0x100 - delta
 *   else
 *     player0_weight = 0x100 - delta
 *     player1_weight = 0x100 + delta
 *
 * Port: 2P split-screen input scratch words are not currently mirrored in
 * the port (no analog of g_raceParticlePoolBase[0x200/0x20e]). The
 * function is provided for completeness so a future 2P-split implementation
 * can call it once the two source words exist as port-side statics. It
 * operates on a port-local pair of weights td5_input_get_player_steering_bias()
 * which falls back to neutral (0x100) when never invoked.
 *
 * Caller: RunRaceFrame @ 0x42b96b — INERT in port until 2P-split is wired.
 * ======================================================================== */

static int16_t s_player_steering_bias_short[2] = { 0x100, 0x100 };   /* orig 0x0046317c/7e */
static int16_t s_player_steering_bias_input[2] = { 0, 0 };           /* orig 0x004ab18c/0x004ab514 */
int32_t        g_td5_steering_bias_max_swing  = 0;                    /* orig 0x0048301c [CONFIRMED via Ghidra read]:
                                                                          BSS-resident, ROM default = 0 (inactive).
                                                                          Live value propagated from g_twoPlayerCatchupAssist
                                                                          (orig 0x00465ff8, range 0..9, ROM default = 4) at the
                                                                          MainMenu→1PRace transition (orig writer near 0x004155EA
                                                                          inside ScreenMainMenuAnd1PRaceFlow:
                                                                          `DAT_0048301c = DAT_00465ff8;`). Port has no 2P-split,
                                                                          so the value is effectively unused; keep BSS default. */

void td5_input_update_player_steering_weight_balance(void)
{
    int i0 = (int)s_player_steering_bias_input[0];
    int i1 = (int)s_player_steering_bias_input[1];
    int delta;
    if (i0 < i1) {
        delta = (i1 - i0) * 2;
        if (delta > g_td5_steering_bias_max_swing) delta = g_td5_steering_bias_max_swing;
        s_player_steering_bias_short[0] = (int16_t)(delta + 0x100);
        s_player_steering_bias_short[1] = (int16_t)(0x100 - delta);
    } else {
        delta = (i0 - i1) * 2;
        if (delta > g_td5_steering_bias_max_swing) delta = g_td5_steering_bias_max_swing;
        s_player_steering_bias_short[0] = (int16_t)(0x100 - delta);
        s_player_steering_bias_short[1] = (int16_t)(delta + 0x100);
    }
}

int16_t td5_input_get_player_steering_bias(int player)
{
    if (player < 0 || player > 1) return 0x100;
    return s_player_steering_bias_short[player];
}

void td5_input_set_player_steering_bias_input(int player, int16_t value)
{
    if (player < 0 || player > 1) return;
    s_player_steering_bias_input[player] = value;
}

/* ========================================================================
 * Control State Queries
 * ======================================================================== */

uint32_t td5_input_get_control_bits(int slot)
{
    if (slot < 0 || slot >= TD5_MAX_RACER_SLOTS) return 0;
    return s_control_bits[slot];
}

void td5_input_set_control_bits(int slot, uint32_t bits)
{
    if (slot < 0 || slot >= TD5_MAX_RACER_SLOTS) return;
    s_control_bits[slot] = bits;
}

int16_t td5_input_get_analog_x(int slot)
{
    if (slot < 0 || slot >= TD5_MAX_RACER_SLOTS) return 0;
    return s_analog_x[slot];
}

int16_t td5_input_get_analog_y(int slot)
{
    if (slot < 0 || slot >= TD5_MAX_RACER_SLOTS) return 0;
    return s_analog_y[slot];
}

int32_t td5_input_get_steering_cmd(int slot)
{
    if (slot < 0 || slot >= TD5_MAX_RACER_SLOTS) return 0;
    return s_steering_cmd[slot];
}

int16_t td5_input_get_throttle(int slot)
{
    if (slot < 0 || slot >= TD5_MAX_RACER_SLOTS) return 0;
    return s_throttle[slot];
}

uint8_t td5_input_get_brake(int slot)
{
    if (slot < 0 || slot >= TD5_MAX_RACER_SLOTS) return 0;
    return s_brake[slot];
}

uint8_t td5_input_get_handbrake(int slot)
{
    if (slot < 0 || slot >= TD5_MAX_RACER_SLOTS) return 0;
    return s_handbrake[slot];
}

uint8_t td5_input_get_gear(int slot)
{
    if (slot < 0 || slot >= TD5_MAX_RACER_SLOTS) return 0;
    return s_gear[slot];
}

int td5_input_get_rear_view(int slot)
{
    if (slot < 0 || slot >= TD5_MAX_RACER_SLOTS) return 0;
    return s_rear_view[slot];
}

/* ========================================================================
 * Frontend camera-button edge getters (#6 support)
 *
 * Edge-triggered, per LOCAL human player. Test the player's CHANGE-CAMERA /
 * FRONT-VIEW controls in the FRONTEND/menu context so a controller can drive
 * the MP position screen even though no race owns the device. Each keeps its own
 * per-player held-latch so a held button fires once per press; the latches are
 * separate from the in-race rear-view / camera-cooldown state, so calling these
 * in the frontend never disturbs an in-race session. Gated by
 * TD5RE_FRONTEND_CAM_BUTTONS (cached; default ON) — "0" leaves the latches
 * disarmed and both getters return 0 (consumers fall back to keyboard nav).
 * (The per-player held-latches s_fe_cam_held / s_fe_front_held are declared near
 * the top of this file so td5_input_init can zero them.)
 *
 * TWO input sources are merged, because the frontend reads them through two
 * DIFFERENT subsystems:
 *
 *   KEYBOARD — td5_plat_input_poll() always decodes the rebindable keyboard
 *   binding table (CHANGE VIEW = action 8 -> TD5_INPUT_CAMERA_CHANGE,
 *   FRONT/REAR VIEW = action 9 -> TD5_INPUT_REAR_VIEW), regardless of which
 *   device a slot is "assigned", so the keyboard path works as-is.
 *
 *   JOYSTICK — [BUG #13 2026-06-15] In the frontend NO per-slot EXCLUSIVE
 *   joystick exists: the MP setup flow deliberately calls
 *   td5_input_set_input_source(p, 0) for every slot (td5_fe_race.c
 *   frontend_mp_setup_init) so the shared NON-exclusive scan handles can be
 *   polled for menu nav. But td5_plat_input_poll()'s joystick block only fires
 *   when s_di_joystick[slot] is non-NULL — which it never is here — so it
 *   returned KEYBOARD-ONLY bits and the pad's CHANGE-CAMERA / FRONT-VIEW did
 *   nothing. The live frontend pad is reachable only through the exported scan
 *   reader td5_plat_input_frontend_nav() (the same path the menus already use).
 *   We OR its signals in so a controller works pre-race like the keyboard.
 *
 *   PLATFORM GAP (reported, not fixable from this module): the scan reader only
 *   decodes nav buttons (dir / A=0x10 / B=0x20 / Start=0x40 i.e. physical
 *   buttons 0/1/7); it does NOT decode the CHANGE-VIEW (default btn 6) or
 *   REAR-VIEW (default btn 9) action buttons, and there is no exported
 *   scan-device action-button reader. Of the scan signals, only Start/Menu is
 *   free on the MP position screen (dir = cell move, A = ready, B = back), so
 *   the pad's Start/Menu button drives CHANGE-CAMERA (the layout cycle — the
 *   reported control). FRONT-VIEW (empty-cell content) has no free scan signal,
 *   so on the pad it still needs an exclusive device (works post-bind / in
 *   race); fully fixing it pre-bind needs a platform td5_plat_input_*
 *   scan action-button accessor (see report).
 * ======================================================================== */

static int td5_fe_cam_buttons_enabled(void)
{
    static int s_init = 0;
    static int s_on = 1;
    if (!s_init) {
        s_on = td5_env_flag_on("TD5RE_FRONTEND_CAM_BUTTONS");   /* default ON */
        s_init = 1;
        TD5_LOG_I(LOG_TAG, "Frontend camera buttons: %s",
                  s_on ? "enabled" : "disabled");
    }
    return s_on;
}

/* Rising edge of an aggregated frontend-pad nav bit, with a SINGLE shared latch
 * (NOT per-player). The pad scan path (td5_plat_input_frontend_nav: bit 1 LEFT
 * 2 RIGHT 4 UP 8 DOWN 0x10 A 0x20 B 0x40 Start/Menu) reads every connected
 * joystick via the shared scan handles and can't attribute a press to a slot, so
 * the edge must fire exactly ONCE per press for the whole screen — important
 * because the consumer loops players and `break`s on the first hit
 * (td5_fe_race.c), which with a per-player latch + a shared signal would re-fire
 * on the next frame via an unlatched player. Called once per frame (from the
 * player-0 path of the change-camera getter). `held` is the per-action shared
 * latch (by reference). */
static int td5_fe_pad_nav_edge(uint32_t pad_nav_bit, uint8_t *held)
{
    int now = (pad_nav_bit && (td5_plat_input_frontend_nav() & pad_nav_bit))
              ? 1 : 0;
    int edge = (now && !*held) ? 1 : 0;
    *held = (uint8_t)now;
    return edge;
}

/* Per-player edge detector for a frontend camera control's KEYBOARD source.
 * `kb_bit` is the control bit td5_plat_input_poll decodes for this player (also
 * carries this slot's EXCLUSIVE joystick if one happens to be bound — e.g. when
 * re-entered mid-session). `held` is the per-player debounce latch (by
 * reference). Returns 1 on the rising edge. */
static int td5_fe_button_edge(int player, uint32_t kb_bit, uint8_t *held)
{
    if (player < 0 || player >= TD5_MAX_HUMAN_PLAYERS) return 0;
    if (!td5_fe_cam_buttons_enabled()) { *held = 0; return 0; }

    TD5_InputState st;
    memset(&st, 0, sizeof(st));
    /* Keyboard (+ this slot's exclusive joystick if one happens to be bound):
     * the platform decodes the configured bindings into the in-race control-bit
     * set. The device slot index equals the local player index, as on the
     * in-race poll path. In the pre-race frontend no exclusive device is bound,
     * so this carries the keyboard bits only — the pad signal is handled
     * separately by td5_fe_pad_nav_edge so its single press isn't multi-fired
     * across players. */
    td5_plat_input_poll(player, &st);

    int now = (st.buttons & kb_bit) ? 1 : 0;
    int edge = (now && !*held) ? 1 : 0;
    *held = (uint8_t)now;
    return edge;
}

int td5_input_frontend_change_camera_pressed(int player)
{
    /* Shared latch for the pad's Start/Menu edge (one per screen, not per
     * player) so a single Start press cycles the layout exactly once. */
    static uint8_t s_pad_cam_held = 0;
    int pad = 0;
    if (player < 0 || player >= TD5_MAX_HUMAN_PLAYERS) return 0;

    /* Keyboard CHANGE VIEW per player (also this slot's exclusive joystick if
     * one happens to be bound). td5_fe_button_edge disarms s_fe_cam_held when
     * the feature is off. */
    int kb = td5_fe_button_edge(player, (uint32_t)TD5_INPUT_CAMERA_CHANGE,
                                &s_fe_cam_held[player]);

    /* OR the pad's Start/Menu (0x40) — the one scan nav signal free on the MP
     * position screen — so a controller cycles the grid LAYOUT like the keyboard
     * ([BUG #13]).
     *
     * [#18c] The aggregated scan reader (td5_plat_input_frontend_nav) can't
     * attribute a press to a slot, so a SINGLE shared latch fires the edge
     * exactly once per press for the whole screen. Previously this was gated to
     * player 0 only; now it is evaluated on whichever player the consumer checks
     * FIRST this frame (it loops players and breaks on the first hit), so the pad
     * START works regardless of which player index owns the iteration — not just
     * player 0. The shared latch + first-evaluation-wins keeps it single-fire:
     * the first player call this frame computes the rising edge and updates the
     * latch; later same-frame calls read the now-set latch and return 0. */
    if (td5_fe_cam_buttons_enabled()) pad = td5_fe_pad_nav_edge(0x40u, &s_pad_cam_held);
    else                              s_pad_cam_held = 0;
    return kb || pad;
}

int td5_input_frontend_front_view_pressed(int player)
{
    if (player < 0 || player >= TD5_MAX_HUMAN_PLAYERS) return 0;
    /* Keyboard FRONT/REAR VIEW, plus this slot's exclusive joystick if bound.
     * No FREE frontend-pad scan signal remains for a second action (see the
     * PLATFORM GAP note above), so there is no pad term here: on a controller
     * this needs a bound device (post-bind / in race) until a platform scan
     * action-button reader exists. */
    return td5_fe_button_edge(player, (uint32_t)TD5_INPUT_REAR_VIEW,
                              &s_fe_front_held[player]);
}

/* ========================================================================
 * Recording -- WriteOpen / Write / WriteClose
 *
 * Delta-compressed input timeline.  Two channels (player 1 & 2).
 * Only emits entries when a value changes from the previous frame.
 *
 * Original M2DX functions:
 *   DXInput::WriteOpen  (0x1000A640)
 *   DXInput::Write      (0x1000A660)
 *   DXInput::WriteClose (0x1000A740)
 * ======================================================================== */

int td5_input_write_open(const char *path)
{
    memset(&s_rec, 0, sizeof(s_rec));
    strncpy(s_replay_log_path, path ? path : "<memory>", sizeof(s_replay_log_path) - 1);
    s_replay_log_path[sizeof(s_replay_log_path) - 1] = '\0';
    s_rec.track_index = g_td5.track_index;
    s_rec.entry_count = 0;
    s_rec.frame_cursor = 0;
    s_rec.last_word0 = 0;
    s_rec.last_word1 = 0;
    /* [BUG 5b 2026-06-15] Capture the dev StartSpanOffset so a later View Replay
     * re-applies the same per-slot grid shift this run is recorded under. Without
     * this the playback re-reads whatever the live INI/CLI offset happens to be,
     * spawning the cars at a different span than recorded -> the played-back input
     * diverges from frame 0. Same mechanism as the captured RNG seed. */
    s_rec.start_span_offset = (int32_t)g_td5.ini.start_span_offset;

    s_replay_recording_open = 1;
    TD5_LOG_I(LOG_TAG, "Replay write open: path=%s track=%d start_span_offset=%d",
              s_replay_log_path, s_rec.track_index, s_rec.start_span_offset);
    return 1;
}

/* [BUG 5b 2026-06-15] StartSpanOffset captured at record time. The game applies
 * it on playback (see td5_game_init_race_session) so a replay recorded with a
 * non-zero offset reproduces the recorded spawn positions. 0 = no offset (also
 * the value for legacy in-memory buffers / v1 disk files). */
int32_t td5_input_replay_start_span_offset(void)
{
    return s_rec.start_span_offset;
}

/* Flush the recorded buffer to disk in the documented file format.
 * Port-only — gated by [Replay] PersistToDisk (default 0). Returns 1 on
 * success, 0 on any failure (open/short-write/etc.) — the in-memory
 * replay buffer is unaffected either way. */
static int td5_input_replay_flush_to_disk(const char *path)
{
    TD5_ReplayFileHeader hdr;
    FILE *f;
    size_t body_bytes;
    size_t written;
    int32_t entries_to_write;

    if (!path || !*path) return 0;

    entries_to_write = s_rec.entry_count;
    if (entries_to_write < 0) entries_to_write = 0;
    if (entries_to_write > (int32_t)TD5_INPUT_REC_MAX_ENTRIES)
        entries_to_write = (int32_t)TD5_INPUT_REC_MAX_ENTRIES;

    memcpy(hdr.magic, TD5_REPLAY_MAGIC, TD5_REPLAY_MAGIC_LEN);
    hdr.version           = TD5_REPLAY_VERSION;
    hdr.track_index       = s_rec.track_index;
    hdr.entry_count       = entries_to_write;
    hdr.last_frame_index  = s_rec.last_frame_index;
    hdr.start_span_offset = s_rec.start_span_offset;  /* [BUG 5b] */

    f = fopen(path, "wb");
    if (!f) {
        TD5_LOG_W(LOG_TAG, "Replay persist: fopen('%s') for write failed", path);
        return 0;
    }

    written = fwrite(&hdr, 1, TD5_REPLAY_HDR_BYTES, f);
    if (written != TD5_REPLAY_HDR_BYTES) {
        TD5_LOG_W(LOG_TAG, "Replay persist: short header write %zu/%d",
                  written, TD5_REPLAY_HDR_BYTES);
        fclose(f);
        return 0;
    }

    body_bytes = (size_t)entries_to_write * sizeof(TD5_InputRecordEntry);
    if (body_bytes > 0) {
        written = fwrite(s_rec.entries, 1, body_bytes, f);
        if (written != body_bytes) {
            TD5_LOG_W(LOG_TAG, "Replay persist: short body write %zu/%zu",
                      written, body_bytes);
            fclose(f);
            return 0;
        }
    }

    fclose(f);
    TD5_LOG_I(LOG_TAG, "Replay persist: wrote %s (v%u, %d entries, %d frames, track=%d, start_span_offset=%d)",
              path, hdr.version, entries_to_write, hdr.last_frame_index + 1,
              hdr.track_index, hdr.start_span_offset);
    return 1;
}

void td5_input_write_close(void)
{
    if (s_rec.frame_cursor > 0) {
        s_rec.last_frame_index = (int32_t)(s_rec.frame_cursor - 1);
    }
    TD5_LOG_I(LOG_TAG, "Replay write close: path=%s entries=%d frames=%u",
              s_replay_log_path, s_rec.entry_count, s_rec.frame_cursor);

    if (g_td5.ini.replay_persist_to_disk && s_replay_log_path[0] &&
        strcmp(s_replay_log_path, "<memory>") != 0)
    {
        td5_input_replay_flush_to_disk(s_replay_log_path);
    }

    s_replay_recording_open = 0;
}

void td5_input_write_frame(uint32_t word0, uint32_t word1, int strip_mode)
{
    if (strip_mode) {
        word0 &= TD5_INPUT_RECORD_STRIP_MASK;
        word1 &= TD5_INPUT_RECORD_STRIP_MASK;
    }

    if (s_rec.frame_cursor == 0) {
        /* First frame: store initial state as two entries */
        s_rec.entries[0].frame_and_channel = 0;
        s_rec.entries[0].value = word0;
        s_rec.entries[1].frame_and_channel = TD5_INPUT_REC_CHANNEL1_FLAG;
        s_rec.entries[1].value = word1;
        s_rec.entry_count = 2;
        s_rec.last_word0 = word0;
        s_rec.last_word1 = word1;
        s_rec.frame_cursor = 1;
        return;
    }

    if (s_rec.entry_count < TD5_INPUT_REC_MAX_ENTRIES &&
        s_rec.frame_cursor < TD5_INPUT_REC_MAX_FRAMES)
    {
        /* Channel 0: emit if changed */
        if (word0 != s_rec.last_word0) {
            s_rec.entries[s_rec.entry_count].frame_and_channel = s_rec.frame_cursor;
            s_rec.entries[s_rec.entry_count].value = word0;
            s_rec.entry_count++;
            s_rec.last_word0 = word0;
        }

        /* Channel 1: emit if changed */
        if (word1 != s_rec.last_word1) {
            s_rec.entries[s_rec.entry_count].frame_and_channel =
                s_rec.frame_cursor | TD5_INPUT_REC_CHANNEL1_FLAG;
            s_rec.entries[s_rec.entry_count].value = word1;
            s_rec.entry_count++;
            s_rec.last_word1 = word1;
        }
    }

    s_rec.frame_cursor++;
}

/* ========================================================================
 * Recording -- ReadOpen / Read / ReadClose
 *
 * Original M2DX functions:
 *   DXInput::ReadOpen  (0x1000A760)
 *   DXInput::Read      (0x1000A780)
 *   DXInput::ReadClose (0x1000D370) -- stub/no-op
 * ======================================================================== */

/* Load a previously persisted replay from disk into s_rec. Port-only —
 * gated by [Replay] PersistToDisk. Returns 1 on success, 0 on any
 * failure (missing file, bad magic, version mismatch, sanity check fail).
 * On failure the in-memory buffer keeps whatever it had. */
static int td5_input_replay_load_from_disk(const char *path)
{
    TD5_ReplayFileHeader hdr;
    FILE *f;
    size_t got;
    size_t body_bytes;

    if (!path || !*path) return 0;

    f = fopen(path, "rb");
    if (!f) {
        TD5_LOG_W(LOG_TAG, "Replay load: fopen('%s') for read failed", path);
        return 0;
    }

    /* [BUG 5b] Read the v1-compatible 24-byte prefix first, then the v2
     * start_span_offset tail only if the file declares v2+. This keeps legacy
     * v1 replays loadable (offset defaults to 0). hdr is zeroed so the offset
     * field is 0 unless explicitly read below. */
    memset(&hdr, 0, sizeof(hdr));
    got = fread(&hdr, 1, TD5_REPLAY_HDR_BYTES_V1, f);
    if (got != TD5_REPLAY_HDR_BYTES_V1) {
        TD5_LOG_W(LOG_TAG, "Replay load: short header read %zu/%d", got, TD5_REPLAY_HDR_BYTES_V1);
        fclose(f);
        return 0;
    }
    if (memcmp(hdr.magic, TD5_REPLAY_MAGIC, TD5_REPLAY_MAGIC_LEN) != 0) {
        TD5_LOG_W(LOG_TAG, "Replay load: bad magic in '%s'", path);
        fclose(f);
        return 0;
    }
    if (hdr.version != TD5_REPLAY_VERSION && hdr.version != TD5_REPLAY_VERSION_V1) {
        TD5_LOG_W(LOG_TAG, "Replay load: version=%u expected=%u or %u",
                  hdr.version, TD5_REPLAY_VERSION, TD5_REPLAY_VERSION_V1);
        fclose(f);
        return 0;
    }
    if (hdr.version >= TD5_REPLAY_VERSION) {
        /* v2+: pull the start_span_offset that follows the v1 prefix. */
        got = fread(&hdr.start_span_offset, 1, sizeof(hdr.start_span_offset), f);
        if (got != sizeof(hdr.start_span_offset)) {
            TD5_LOG_W(LOG_TAG, "Replay load: short v2 header read %zu/%zu",
                      got, sizeof(hdr.start_span_offset));
            fclose(f);
            return 0;
        }
    }
    if (hdr.entry_count < 0 || hdr.entry_count > (int32_t)TD5_INPUT_REC_MAX_ENTRIES) {
        TD5_LOG_W(LOG_TAG, "Replay load: insane entry_count=%d (cap=%u)",
                  hdr.entry_count, TD5_INPUT_REC_MAX_ENTRIES);
        fclose(f);
        return 0;
    }
    if (hdr.last_frame_index < 0 ||
        hdr.last_frame_index > (int32_t)TD5_INPUT_REC_MAX_FRAMES) {
        TD5_LOG_W(LOG_TAG, "Replay load: insane last_frame_index=%d", hdr.last_frame_index);
        fclose(f);
        return 0;
    }

    /* Replace in-memory state. memset clears the entries array so any
     * unread tail beyond entry_count is well-defined. */
    memset(&s_rec, 0, sizeof(s_rec));
    s_rec.track_index       = hdr.track_index;
    s_rec.entry_count       = hdr.entry_count;
    s_rec.last_frame_index  = hdr.last_frame_index;
    s_rec.start_span_offset = hdr.start_span_offset;  /* [BUG 5b] 0 for v1 files */

    body_bytes = (size_t)hdr.entry_count * sizeof(TD5_InputRecordEntry);
    if (body_bytes > 0) {
        got = fread(s_rec.entries, 1, body_bytes, f);
        if (got != body_bytes) {
            TD5_LOG_W(LOG_TAG, "Replay load: short body read %zu/%zu",
                      got, body_bytes);
            fclose(f);
            memset(&s_rec, 0, sizeof(s_rec));
            return 0;
        }
    }

    fclose(f);

    if (hdr.track_index != g_td5.track_index) {
        TD5_LOG_W(LOG_TAG, "Replay load: track mismatch (file=%d current=%d) — "
                  "playback will desync", hdr.track_index, g_td5.track_index);
    }
    TD5_LOG_I(LOG_TAG, "Replay load: read %s (v%u, %d entries, %d frames, track=%d, start_span_offset=%d)",
              path, hdr.version, hdr.entry_count, hdr.last_frame_index + 1,
              hdr.track_index, hdr.start_span_offset);
    return 1;
}

int td5_input_read_open(const char *path)
{
    /* Reset read cursor; the buffer already contains recorded data */
    strncpy(s_replay_log_path, path ? path : "<memory>", sizeof(s_replay_log_path) - 1);
    s_replay_log_path[sizeof(s_replay_log_path) - 1] = '\0';

    /* Port-only: when persistence is enabled and a path was given, load
     * the recorded buffer from disk before the read cursor resets. The
     * loader replaces s_rec wholesale on success; on failure the existing
     * in-memory buffer is kept so callers still get faithful behavior. */
    if (g_td5.ini.replay_persist_to_disk && path && *path &&
        strcmp(path, "<memory>") != 0)
    {
        td5_input_replay_load_from_disk(path);
    }

    s_rec.frame_cursor = 0;
    s_rec.read_index = 0;
    s_rec.replay_word0 = 0;
    s_rec.replay_word1 = 0;

    TD5_LOG_I(LOG_TAG, "Replay open: path=%s entries=%d frames=%d",
              s_replay_log_path, s_rec.entry_count, s_rec.last_frame_index + 1);
    return 1;
}

void td5_input_read_close(void)
{
    /* No-op, matching original DXInput::ReadClose */
    TD5_LOG_I(LOG_TAG, "Replay close: path=%s frames=%d",
              s_replay_log_path, s_rec.last_frame_index + 1);
}

int td5_input_read_frame(uint32_t *word0, uint32_t *word1)
{
    if (s_rec.frame_cursor == 0) {
        /* First frame: load initial state from entries 0 and 1 */
        if (s_rec.entry_count >= 2) {
            s_rec.replay_word0 = s_rec.entries[0].value;
            s_rec.replay_word1 = s_rec.entries[1].value;
        }
        s_rec.read_index = 2;
    } else {
        if ((int32_t)s_rec.read_index >= s_rec.entry_count) {
            /* Recording exhausted */
            *word0 = 0;
            *word1 = 0;
            return 0;
        }

        /* Check channel 0 */
        uint32_t entry_fc = s_rec.entries[s_rec.read_index].frame_and_channel;
        if (s_rec.frame_cursor == (entry_fc & 0x7FFF)) {
            if ((entry_fc & TD5_INPUT_REC_CHANNEL1_FLAG) == 0) {
                s_rec.replay_word0 = s_rec.entries[s_rec.read_index].value;
                s_rec.read_index++;
            }
        }

        /* Check channel 1 */
        if ((int32_t)s_rec.read_index < s_rec.entry_count) {
            entry_fc = s_rec.entries[s_rec.read_index].frame_and_channel;
            if ((entry_fc & TD5_INPUT_REC_CHANNEL1_FLAG) != 0) {
                if (s_rec.frame_cursor == (entry_fc & 0x7FFF)) {
                    s_rec.replay_word1 = s_rec.entries[s_rec.read_index].value;
                    s_rec.read_index++;
                }
            }
        }
    }

    *word0 = s_rec.replay_word0;
    *word1 = s_rec.replay_word1;
    s_rec.frame_cursor++;
    return 1;
}

/* ========================================================================
 * Force Feedback -- Lifecycle
 * ======================================================================== */

int td5_input_ff_init(void)
{
    memset(&s_ff, 0, sizeof(s_ff));

    /* [FF SIGNALS #1] Seed the per-player gear/crash edge baselines from the
     * CURRENT physics sequence ids so a brand-new race doesn't fire a spurious
     * gear/crash jolt on its first frame (the sequences are monotonic across the
     * session; only an INCREASE is an event). Clear any pending jolt too. The FF
     * layer's player slot is the actor slot (== device slot), exactly as
     * td5_input_ff_update_player treats it. */
    {
        int p;
        for (p = 0; p < TD5_MAX_RACER_SLOTS; p++) {
            s_ff_gear_seen[p]   = td5_physics_gear_change_seq(p);
            s_ff_crash_seen[p]  = td5_physics_get_crash_fx(p, NULL, NULL);
            s_ff_land_seen[p]   = td5_physics_get_landing_fx(p, NULL); /* [#5(4)] don't fire a stale landing on race start */
            s_ff_pulse_mag[p]   = 0;
            s_ff_pulse_ticks[p] = 0;
            s_ff_side_mag[p]    = 0;
            s_ff_side_ticks[p]  = 0;
        }
    }

    /* CreateRaceForceFeedbackEffects (0x4285B0) creates 4 effect slots PER
     * device:
     *   Slot 0: Constant force -- steering resistance
     *   Slot 1: Constant force -- frontal collision impact
     *   Slot 2: Constant force -- side collision impact
     *   Slot 3: Periodic       -- terrain vibration / rumble
     *
     * [PORT ENHANCEMENT 2026-06] N-way vibration: each human player whose input
     * source is a joystick gets its own FF effect set on its own device. The
     * per-player FF controller assignment (= the player's 1-based device index)
     * is configured here — the original wired ConfigureForceFeedbackControllers
     * @0x428880 but the port never called it, so FF had been inert. */
    int players = s_active_players;
    if (players < 1) players = 1;
    if (players > TD5_MAX_HUMAN_PLAYERS) players = TD5_MAX_HUMAN_PLAYERS;

    int assignments[TD5_MAX_RACER_SLOTS];
    memset(assignments, 0, sizeof(assignments));
    for (int p = 0; p < players; p++)
        assignments[p] = s_input_source[p];   /* 1-based device idx, 0 = keyboard */
    td5_input_ff_configure(assignments, players);

    int any = 0;
    for (int p = 0; p < players; p++) {
        if (s_input_source[p] <= 0) continue;  /* keyboard player: no FF device */
        if (td5_plat_ff_init(p)) {
            TD5_LOG_I(LOG_TAG, "Force feedback initialized: player=%d device=%d",
                      p, s_input_source[p]);
            any = 1;
        } else {
            TD5_LOG_W(LOG_TAG, "Force feedback not available: player=%d device=%d",
                      p, s_input_source[p]);
        }
    }
    return any;
}

void td5_input_ff_shutdown(void)
{
    td5_plat_ff_shutdown();   /* stops + releases effects on every device */
    memset(&s_ff, 0, sizeof(s_ff));
}

/* ========================================================================
 * td5_input_ff_update  (top-level FF dispatcher)
 *
 * Called once per race frame from PollRaceSessionInput (0x0042C470).
 * [CONFIRMED @ 0x0042C470]: UpdateControllerForceFeedback(0) is called
 * from within PollRaceSessionInput for single-player; in net play the
 * local slot index is passed instead.
 *
 * The magnitude parameter is not used here — per-player force magnitude
 * is computed from actor state inside td5_input_ff_update_player.
 * ======================================================================== */
/* [FF SIGNALS 2026-06-15, #1] Per-player crash/gear JOLT update.
 *
 * Edge-detects the physics gear-change and acute-crash sequences for one player
 * (player slot == actor slot == device slot, as elsewhere in the FF path) and
 * drives a short CONSTANT-FORCE pulse on slot 1 (TD5_FF_SLOT_FRONTAL — the
 * "high motor" analogue, distinct from the continuous periodic rumble on slot
 * 3). Rules:
 *   - CRASH/COLLISION: a STRONG pulse on a NEW acute crash (sequence increased,
 *     and age small), magnitude scaled by the impact mag.
 *   - GEAR SWITCH: a brief LOW-force pulse on each gear_change_seq increment.
 * The pulse magnitude is held for a few render frames then decays to 0, at which
 * point slot 1 is stopped so it doesn't sit asserted. A crash outranks a gear
 * bump if they coincide. No-op when TD5RE_FORCE_FEEDBACK is off or this player
 * has no FF device. */
/* [MP FF FIX 2026-06-23] Map a LOCAL player/device index to the ACTOR slot whose
 * car that device should respond to. In single-player and split-screen the local
 * humans occupy actor slots 0..N-1 (g_actorSlotForView is the identity map); in
 * NETPLAY the single local human drives actor slot td5_net_local_slot() — 1+ on a
 * client — surfaced as g_actorSlotForView[0] (td5_game InitRace step 17). The FF
 * consumers previously assumed actor-slot == device-index, so a client's wheel
 * replayed actor-slot-0's pulses (the HOST's collisions/steering): "I feel the
 * vibrations off my opponents in MP". Reading the right actor slot here restores
 * the original's netplay-local-slot FF [CONFIRMED @ 0x0042C470] and also fixes the
 * MP position-select pane permutation. Device-side indices (controller_assignment,
 * td5_plat_ff_*, steer/terrain_effect_started) stay keyed by the device index. */
static int ff_local_actor_slot(int player)
{
    int aslot = player;
    if (player >= 0 && player < TD5_MAX_VIEWPORTS)
        aslot = g_actorSlotForView[player];
    if (aslot < 0 || aslot >= TD5_MAX_RACER_SLOTS)
        aslot = player;   /* defensive: fall back to identity */
    if (aslot != player && player >= 0 && player < TD5_MAX_HUMAN_PLAYERS &&
        !s_ff_slot_map_logged[player]) {
        s_ff_slot_map_logged[player] = 1;
        TD5_LOG_I(LOG_TAG, "FF: local player/device %d follows actor slot %d (non-identity map)",
                  player, aslot);
    }
    return aslot;
}

static void td5_input_ff_update_jolt(int player)
{
    if (player < 0 || player >= TD5_MAX_HUMAN_PLAYERS) return;
    if (s_ff.controller_assignment[player] == 0) return;
    if (!td5_ff_vibration_enabled()) return;
    int dev = player;                          /* physical FF device index */
    int slot = ff_local_actor_slot(player);    /* [MP FF FIX] actor slot this device follows */
    if (slot < 0 || slot >= TD5_MAX_RACER_SLOTS) return;

    /* ---- GEAR SWITCH edge: brief LOW-force bump ---- */
    {
        uint32_t seq = td5_physics_gear_change_seq(slot);
        if (seq != s_ff_gear_seen[slot]) {
            s_ff_gear_seen[slot] = seq;
            /* Don't override a stronger crash pulse already in flight. */
            if (TD5_FF_GEAR_PULSE_MAG > s_ff_pulse_mag[slot]) {
                s_ff_pulse_mag[slot]   = TD5_FF_GEAR_PULSE_MAG;
                s_ff_pulse_ticks[slot] = TD5_FF_GEAR_PULSE_TICKS;
            }
        }
    }

    /* ---- CRASH/COLLISION edge: STRONG pulse scaled by impact magnitude ---- */
    {
        int32_t  mag = 0;
        int      age = 0;
        uint32_t cseq = td5_physics_get_crash_fx(slot, &mag, &age);
        /* "New" crash: a fresh sequence id AND a young event (so re-entering a
         * race that left a stale non-zero seq, or polling long after the hit,
         * doesn't fire). */
        if (cseq != 0 && cseq != s_ff_crash_seen[slot] &&
            age <= TD5_FF_CRASH_NEW_AGE_TICKS) {
            s_ff_crash_seen[slot] = cseq;
            int cm = (mag > 0 ? (int)(mag >> TD5_FF_CRASH_MAG_SHIFT) : 0);
            if (cm > TD5_FF_CRASH_PULSE_MAG_MAX) cm = TD5_FF_CRASH_PULSE_MAG_MAX;
            /* Floor so even a borderline-acute hit is clearly felt. */
            if (cm < TD5_FF_GEAR_PULSE_MAG) cm = TD5_FF_GEAR_PULSE_MAG;
            if (cm >= s_ff_pulse_mag[slot]) {     /* crash outranks an equal/smaller bump */
                s_ff_pulse_mag[slot]   = cm;
                s_ff_pulse_ticks[slot] = TD5_FF_CRASH_PULSE_TICKS;
            }
            TD5_LOG_I(LOG_TAG, "FF crash jolt: player=%d mag=%d age=%d -> pulse=%d",
                      slot, (int)mag, age, s_ff_pulse_mag[slot]);
        } else if (cseq != s_ff_crash_seen[slot]) {
            /* Acknowledge an old/late sequence so it can't fire belatedly. */
            s_ff_crash_seen[slot] = cseq;
        }
    }

    /* ---- AIR-TIME LANDING edge: decaying jolt scaled by vertical impact ----
     * [item #5(4)] When the physics landing signal reports a fresh touchdown
     * (sequence increased), fire a pulse on the slot-1 (FRONTAL/high-motor) decay
     * path, scaled by the downward vertical impact speed. Reuses the same pulse
     * the crash/gear jolts use, so it auto-releases — no stuck motor. Gated by
     * TD5RE_FF_LANDING inside the physics getter (off => seq never advances). */
    {
        int32_t  impact = 0;
        uint32_t lseq = td5_physics_get_landing_fx(slot, &impact);
        if (lseq != 0 && lseq != s_ff_land_seen[slot]) {
            s_ff_land_seen[slot] = lseq;
            int lm = (impact > 0 ? (int)(impact >> TD5_FF_LANDING_MAG_SHIFT) : 0);
            if (lm > TD5_FF_LANDING_MAG_MAX) lm = TD5_FF_LANDING_MAG_MAX;
            if (lm < TD5_FF_LANDING_MAG_FLOOR) lm = TD5_FF_LANDING_MAG_FLOOR;
            if (lm >= s_ff_pulse_mag[slot]) {     /* don't cut a stronger crash jolt short */
                s_ff_pulse_mag[slot]   = lm;
                s_ff_pulse_ticks[slot] = TD5_FF_LANDING_PULSE_TICKS;
            }
            TD5_LOG_I(LOG_TAG, "FF landing jolt: player=%d impact=%d -> pulse=%d",
                      slot, (int)impact, s_ff_pulse_mag[slot]);
        } else if (lseq != s_ff_land_seen[slot]) {
            s_ff_land_seen[slot] = lseq;          /* acknowledge any stale seq */
        }
    }

    /* ---- Drive / decay the jolt on slot 1 (FRONTAL -> high motor) ---- */
    /* [stuck-motor fix 2026-06-15] Drive while ticks remain; do NOT zero the
     * magnitude on the expiry frame — that pre-zero made the `else if (mag != 0)`
     * release branch DEAD (ticks==0 always coincided with mag==0), so the motor
     * was left asserted at its last pulse value forever ("whenever something
     * triggers FF it gets stuck"). Leaving mag set lets the next frame's else-if
     * actually call td5_plat_ff_stop and release the motor. */
    if (s_ff_pulse_ticks[slot] > 0) {
        td5_plat_ff_constant(dev, TD5_FF_SLOT_FRONTAL, s_ff_pulse_mag[slot]);
        s_ff_pulse_ticks[slot]--;
    } else if (s_ff_pulse_mag[slot] != 0) {
        /* Pulse expired: release the slot so it stops asserting force. Frontal
         * V2V/wall collisions now feed THIS same pulse (td5_input_ff_collision),
         * so there is no separate persistent collision effect to protect — always
         * stop. (The OLD collision_active guard left the motor asserted forever
         * once a start-line contact latched the flag.) */
        s_ff_pulse_mag[slot] = 0;
        td5_plat_ff_stop(dev, TD5_FF_SLOT_FRONTAL);
    }

    /* ---- Drive / decay the SIDE collision pulse on slot 2 (low motor) ---- */
    if (s_ff_side_ticks[slot] > 0) {
        td5_plat_ff_constant(dev, TD5_FF_SLOT_SIDE, s_ff_side_mag[slot]);
        s_ff_side_ticks[slot]--;
    } else if (s_ff_side_mag[slot] != 0) {
        s_ff_side_mag[slot] = 0;
        td5_plat_ff_stop(dev, TD5_FF_SLOT_SIDE);
    }

    /* Terrain-dampen latch counts down so it can't stick on. The old code only
     * ever cleared it on the one-time terrain-effect start, so any collision left
     * it asserted for the rest of the race. */
    if (s_ff.collision_active[slot] > 0)
        s_ff.collision_active[slot]--;
}

void td5_input_ff_update(void)
{
    /* Dispatch per-player FF update for all active local players.
     * [CONFIRMED @ 0x0042C470]: UpdateControllerForceFeedback dispatched inside
     * PollRaceSessionInput for slot 0 (single-player) or the local participant
     * slot (network). [PORT ENHANCEMENT 2026-06] iterate all active humans. */
    int players = s_active_players;
    if (players < 1) players = 1;
    if (players > TD5_MAX_HUMAN_PLAYERS) players = TD5_MAX_HUMAN_PLAYERS;
    for (int p = 0; p < players; p++) {
        td5_input_ff_update_player(p);   /* steering + terrain + redline rumble */
        td5_input_ff_update_jolt(p);     /* [FF SIGNALS #1] crash/gear jolt pulse */
    }
    TD5_LOG_D(LOG_TAG, "FF dispatcher: players=%d", players);
}

void td5_input_ff_stop(void)
{
    for (int dev = 0; dev < TD5_MAX_HUMAN_PLAYERS; dev++) {
        for (int slot = 0; slot < 4; slot++)
            td5_plat_ff_stop(dev, slot);
        s_ff.steer_effect_started[dev] = 0;
        s_ff.terrain_effect_started[dev] = 0;
        /* [FF SIGNALS #1] drop any in-flight crash/gear jolt so a stopped device
         * doesn't resume buzzing when effects restart. [stuck-motor fix 2026-06-15]
         * also drop the side-collision pulse and the terrain-dampen latch so a
         * pause/race-end leaves every motor at zero. */
        s_ff_pulse_mag[dev] = 0;
        s_ff_pulse_ticks[dev] = 0;
        s_ff_side_mag[dev] = 0;
        s_ff_side_ticks[dev] = 0;
        s_ff.collision_active[dev] = 0;
    }
}

/* ========================================================================
 * ConfigureForceFeedbackControllers  (0x428880)
 *
 * Stores per-slot controller assignments for force feedback.
 * In network play, only the local participant slot gets an assignment.
 *
 * params:
 *   assignments -- array of joystick indices (1-based, 0=none)
 *   count       -- number of entries to copy (up to 6)
 *
 * [ARCH-DIVERGENCE: network-mode branch moved to caller; L5 sweep 2026-05-21]
 *   Ghidra-verified 0x00428880: orig clears g_ffControllerAssignment[0..5]
 *   then either copies the param_1[0..count-1] into [0..count-1] when in
 *   non-network mode (dpu_exref+0xc08 == 0), or splices only one entry
 *   (param_1[0] -> g_ffControllerAssignment[dpu_exref+0xbe8]) in network
 *   mode. Port preserves the bulk-clear + bulk-copy half of the original
 *   (same 6-slot stride, same count clamp) but moves the network-mode
 *   single-slot splice to caller code; the port comment explicitly notes
 *   "we handle that at a higher level". Bulk-copy path itself byte-faithful. */

void td5_input_ff_configure(const int *assignments, int count)
{
    /* Zero all 6 slots first */
    for (int i = 0; i < TD5_MAX_RACER_SLOTS; i++) {
        s_ff.controller_assignment[i] = 0;
    }

    if (count <= 0) return;

    /* In the original, network mode only assigns the local player slot.
     * For the source port, we handle that at a higher level.
     * Here we just copy the provided assignments. */
    for (int i = 0; i < count && i < TD5_MAX_RACER_SLOTS; i++) {
        s_ff.controller_assignment[i] = assignments[i];
    }
}

/* ========================================================================
 * UpdateControllerForceFeedback  (0x4288E0)
 *
 * Called every frame for each active player slot.  Updates:
 *   1. Steering resistance (effect slot 0) -- proportional to lateral force
 *   2. Terrain vibration (effect slot 3) -- modulated by surface type
 *
 * Reads vehicle state:
 *   actor+0x318 = lat_speed (lateral body-frame velocity, int32 24.8 FP)
 *                 [CONFIRMED @ 0x4288E0]: g_actorRuntimeState.slot.field_0x318
 *   actor+0x370 = surface type byte (index into g_terrain_ff_coefficients[13])
 *                 [CONFIRMED @ 0x4288E0]: cast to uint then used as LUT index
 * ======================================================================== */

void td5_input_ff_update_player(int player)
{
    /* The player's FF device lives at device slot == player slot (created by
     * td5_plat_input_set_device(player, ...)). Only human players (0..8) own a
     * device slot. [PORT ENHANCEMENT 2026-06] no longer capped at js_idx<=1.
     * [MP FF FIX 2026-06-23] dev = device index (per local human); slot = the
     * ACTOR slot that device follows (== device index except on a netplay client
     * or MP position-select, where the local car is a non-zero actor slot).
     *
     * [SPLIT-SCREEN FF FIX 2026-06-24] dev = player is CORRECT here and must NOT
     * be "faithfully" changed to controller_assignment[player]-1. The original
     * keyed a GLOBAL DI-device array by g_ffControllerAssignment[player]-1 (a
     * 1-based chosen JS index) @0x004288E0 / 0x00428A10 because that array was
     * indexed by enumeration. The port instead binds a PER-SLOT device array
     * (s_di_joystick[player]) to the player's chosen device at
     * td5_plat_input_set_device time, so the indirection is already baked in and
     * dev = player resolves to the player's OWN stick. The only mis-routing was
     * the port-only XInput user correlation, fixed in td5_xinput_user_for_device
     * (td5_platform_win32.c). */
    if (player < 0 || player >= TD5_MAX_HUMAN_PLAYERS) return;
    if (s_ff.controller_assignment[player] == 0) return;
    int dev = player;
    int slot = ff_local_actor_slot(player);
    if (slot < 0 || slot >= TD5_MAX_RACER_SLOTS) return;

    /* ---- Steering resistance (slot 0) ---- */

    /*
     * [CONFIRMED @ 0x4288E0]: iVar2 = *(int*)(actor + slot*0x388 + 0x318)
     * This is lat_speed — body-frame lateral velocity (int32, 24.8 FP).
     * Negative values are reflected: iVar2 = iVar2 * -2
     * Then clamped to 300000.
     * Original passes through __ftol (no-op for integer input).
     */
    int lateral_force = 0;
    if (g_actor_table_base) {
        uint8_t *actor = g_actor_table_base + (size_t)slot * TD5_ACTOR_STRIDE;
        lateral_force = *(int32_t *)(actor + 0x318); /* lat_speed */
    }

    if (lateral_force < 0) {
        lateral_force = lateral_force * -2;  /* asymmetric feel [CONFIRMED @ 0x4288E0] */
    }
    if (lateral_force > TD5_INPUT_FF_STEER_CLAMP) {
        lateral_force = TD5_INPUT_FF_STEER_CLAMP;
    }

    int steer_mag = lateral_force; /* __ftol in original: integer value, no conversion needed */

    if (!s_ff.steer_effect_started[dev]) {
        /* First-time play: start the effect */
        td5_plat_ff_constant(dev, 0, steer_mag);
        s_ff.steer_effect_started[dev] = 1;
    } else {
        /* Update running effect parameters */
        td5_plat_ff_constant(dev, 0, steer_mag);
    }

    /* ---- Terrain vibration (slot 3) ---- */

    /*
     * [CONFIRMED @ 0x4288E0]: (uint)(byte)(actor + slot*0x388 + 0x370)
     * Surface type byte — indexes into DAT_00466afc = g_terrain_ff_coefficients[13].
     * Formula: iVar2 = (0x1e - iVar2/10000) * coeff[surface_type]
     * where iVar2 is the already-clamped lateral_force.
     */
    int surface_type = 0;
    if (g_actor_table_base) {
        uint8_t *actor = g_actor_table_base + (size_t)slot * TD5_ACTOR_STRIDE;
        surface_type = (int)(uint8_t)(actor[0x370]); /* surface type [CONFIRMED @ 0x4288E0] */
    }
    if (surface_type < 0) surface_type = 0;
    if (surface_type > 12) surface_type = 12;

    int terrain_coeff = g_terrain_ff_coefficients[surface_type];
    int terrain_mag = (30 - lateral_force / 10000) * terrain_coeff;

    /* [FF SIGNALS #1; gamepad-rumble fix 2026-06-15; drift re-added #5(3) 2026-06-16]
     * Compose the physics-driven CONTINUOUS rumbles onto the periodic slot (slot 3
     * — the buzz, the low-motor analogue) so they ride on top of the terrain
     * effect for FF WHEELS rather than a second writer fighting it. Two sources,
     * each EXACTLY 0 when its condition is absent (so neither can stick on):
     *   - MAX RPM (manual only): LIGHT continuous rumble while at redline. The
     *     "manual only" rule keys off the actor's auto-gearbox flag (+0x378==0 =>
     *     manual; the same flag td5_input_update_player_control writes). In auto
     *     the box upshifts at redline so a sustained redline buzz would be noise;
     *     in manual the player holds the limiter, which is what we want to feel.
     *   - DRIFT: rumble proportional to td5_physics_get_drift_level (0 = tracking
     *     straight). The physics getter already deadzones small slip, and we
     *     additionally require drift_level >= TD5_FF_DRIFT_LEVEL_MIN, so a car
     *     driving normally produces drift_rumble == 0 and the buzz vanishes.
     *
     * The DRIFT rumble was previously REMOVED to fix an always-on-buzz bug; it is
     * safe to re-add because (a) it is 0 whenever the car is not actually sliding,
     * (b) it is folded into the SAME continuous-buzz value the redline path uses
     * (no extra platform contributor that could be left asserted), and (c) it is
     * pushed every frame, so the moment a slide ends the value returns to 0 and
     * td5_plat_ff_xinput_rumble drives the motor contributor back to silence. On a
     * gamepad the continuous buzz reaches ONLY the dedicated low-motor contributor
     * via td5_plat_ff_xinput_rumble (slot 3 itself is not motor-routed), so steady
     * normal driving leaves every gamepad motor at zero.
     * Master gate: TD5RE_FORCE_FEEDBACK; drift sub-gate: TD5RE_FF_DRIFT (both
     * default ON). When the master gate is off, terrain-only (faithful). */
    int redline_rumble = 0;
    int drift_rumble   = 0;
    if (td5_ff_vibration_enabled()) {
        if (td5_physics_at_redline(slot)) {
            /* [FF redline manual-gate fix 2026-06-16] Gate on the AUTHORITATIVE
             * manual control bit (28), computed this same poll from the gearbox
             * selection (auto_gearbox INI / drag / per-player menu MANUAL pick),
             * NOT the actor +0x378 byte: that byte is dual-used as
             * throttle_input_active, so it can read "auto" while flooring in manual
             * (and the reverse), leaking the rev-limiter buzz into AUTOMATIC. Bit
             * 28 set == MANUAL; clear == AUTOMATIC => no buzz. */
            int manual = (slot < TD5_MAX_RACER_SLOTS) &&
                         (s_control_bits[slot] & 0x10000000u) != 0;
            if (manual)
                redline_rumble = TD5_FF_REDLINE_MAG;     /* rev-limiter buzz, manual only */
        }

        /* DRIFT buzz, proportional to slide magnitude; 0 below the threshold. */
        if (td5_ff_drift_enabled()) {
            int dl = td5_physics_get_drift_level(slot);  /* 0, else ~1..255 */
            if (dl >= TD5_FF_DRIFT_LEVEL_MIN) {
                /* Scale drift_level (1..255) -> [FLOOR..MAG_MAX], floored at onset
                 * so the start of a slide is felt. Integer math (deterministic). */
                int span = TD5_FF_DRIFT_MAG_MAX - TD5_FF_DRIFT_MAG_FLOOR;
                if (span < 0) span = 0;
                drift_rumble = TD5_FF_DRIFT_MAG_FLOOR + (dl * span) / 255;
                if (drift_rumble > TD5_FF_DRIFT_MAG_MAX) drift_rumble = TD5_FF_DRIFT_MAG_MAX;
            }
            /* else: drift_rumble stays 0 -> no buzz when not sliding. */
        }
    }

    /* The single continuous-buzz value: the louder of redline and drift. Both are
     * 0 when their condition is absent, so this is 0 during steady normal driving
     * -> the gamepad motor contributor is set to 0 (silence) and the wheel slot-3
     * gets only its terrain magnitude. */
    int continuous_buzz = (redline_rumble > drift_rumble) ? redline_rumble : drift_rumble;

    if (continuous_buzz > 0) {
        /* FF WHEEL: add the buzz to the terrain magnitude on slot 3; the platform
         * clamps to DI_FFNOMINALMAX. Floored at the buzz so a smooth surface still
         * vibrates while at the limiter / sliding. (No-op on a gamepad — slot 3 is
         * not motor-routed; the buzz reaches the gamepad motor only through the
         * dedicated XInput rumble call below.) */
        int composed = terrain_mag + continuous_buzz;
        if (composed < continuous_buzz) composed = continuous_buzz;
        if (composed > TD5_FF_MAG_MAX) composed = TD5_FF_MAG_MAX;
        terrain_mag = composed;
    }

    /* GAMEPAD (XInput) continuous buzz: drive a dedicated low-motor contributor
     * with ONLY the redline-in-manual/drift magnitude (0 otherwise), so the
     * gamepad motor never picks up terrain/steering. Pushed every frame, so when
     * the car stops drifting / leaves the limiter the value returns to 0 and the
     * motor goes silent — no stuck rumble. No-op for DI wheels (they already got
     * the buzz folded into slot 3 above). */
    td5_plat_ff_xinput_rumble(dev, continuous_buzz);

    if (!s_ff.terrain_effect_started[dev]) {
        /* First play of terrain effect */
        td5_plat_ff_constant(dev, 3, terrain_mag);
        s_ff.terrain_effect_started[dev] = 1;
        s_ff.collision_active[slot] = 0;
    } else {
        if (s_ff.collision_active[slot] != 0) {
            /* During collision recovery, dampen terrain vibration */
            terrain_mag = TD5_INPUT_FF_COLLISION_DAMPEN;
        }
        td5_plat_ff_constant(dev, 3, terrain_mag);
    }
}

/* ========================================================================
 * PlayAssignedControllerEffect  (0x428A10)
 *
 * A thin helper to fire arbitrary effects on a player's assigned
 * controller.  Validates slot and FF capability before dispatching.
 *
 * [ARCH-DIVERGENCE: DXInput::PlayEffect -> td5_plat_ff_constant; L5 sweep 2026-05-21]
 *   Ghidra-verified 0x00428A10: orig validates slot < 6 and
 *   g_ffControllerAssignment[slot] != 0, then calls
 *   DXInput::PlayEffect(jsidx, effect_slot, magnitude, repeat_count) if
 *   the per-joystick FF-capable flag at js_exref[jsidx*0x70+0x44] is set.
 *   Port replaces DXInput with td5_plat_ff_constant (a 2-param API:
 *   effect_slot, magnitude), so repeat_count is discarded (already a noop
 *   for the port's effect cache) and the FF-capable flag check is folded
 *   into the platform layer's no-op behaviour on unsupported joysticks.
 *   Port adds an upper-bound `js_idx > 1` clamp the orig lacks, but
 *   port targets at most 2 force-feedback wheels so the clamp is a noop
 *   in practice. */

void td5_input_ff_play_effect(int slot, int effect_slot, int magnitude,
                              int repeat_count)
{
    (void)repeat_count;

    if (slot < 0 || slot >= TD5_MAX_RACER_SLOTS) return;
    if (s_ff.controller_assignment[slot] == 0) return;
    if (slot >= TD5_MAX_HUMAN_PLAYERS) return;   /* device slots are per human player */

    if (effect_slot < 0) effect_slot = 0;
    if (effect_slot > 3) effect_slot = 3;
    TD5_LOG_I(LOG_TAG, "FF start: player=%d slot=%d magnitude=%d repeat=%d",
              slot, effect_slot, magnitude, repeat_count);
    td5_plat_ff_constant(slot, effect_slot, magnitude);
}

/* ========================================================================
 * PlayVehicleCollisionForceFeedback  (0x428A60)
 *
 * Called from the physics collision resolver.  Dispatches collision
 * effects to both vehicles involved.
 *
 * contact_side bitmask determines which effect slot each vehicle gets:
 *   0x01-0x08 (front/rear):  A gets slot 1 (frontal), B gets slot 2 (side)
 *   0x10-0x80 (left/right):  B gets slot 1 (frontal), A gets slot 2 (side)
 *   default (other):         Both get slot 2 (side)
 *
 * [ARCH-DIVERGENCE: actor+0x375 -> explicit slot params; L5 sweep 2026-05-21]
 *   Ghidra-verified 0x00428A60: orig magnitude = param_4 / 10 clamped to
 *   10000 (TD5_INPUT_FF_COLLISION_DIV / TD5_INPUT_FF_COLLISION_MAX in port
 *   match byte-exact), same switch on contact_side with the same
 *   frontal/side assignments (effect slots 1 and 2 = TD5_FF_SLOT_FRONTAL/
 *   TD5_FF_SLOT_SIDE in port match orig DXInput::PlayEffect constants),
 *   reading the actor's per-actor controller-assignment byte at +0x375.
 *   Port takes (actor_a_slot, actor_b_slot) as explicit int parameters
 *   rather than reading actor+0x375 directly, and replaces DXInput::
 *   PlayEffect with td5_input_ff_play_effect (port wrapper, see
 *   ARCH-DIVERGENCE block on PlayAssignedControllerEffect above). Port
 *   also sets s_ff.collision_active[slot] = 1 per-actor (port-only state
 *   used to dampen terrain vibration during collision recovery; the
 *   orig has no equivalent damping). Switch logic, clamps, and slot
 *   assignment byte-faithful. */

/* [stuck-motor fix 2026-06-15] Arm a collision as a DECAYING pulse that the FF
 * manager (td5_input_ff_update_jolt) drives and auto-releases each frame, rather
 * than the old td5_input_ff_play_effect path which started a CONSTANT force that
 * was never stopped (the motor then buzzed for the rest of the race). slot 1
 * (FRONTAL) -> high-motor pulse, slot 2 (SIDE) -> low-motor pulse. max() so a
 * stronger jolt already in flight isn't cut short by a glancing follow-up. */
static void ff_arm_collision_pulse(int slot, int effect_slot, int mag)
{
    if (slot < 0 || slot >= TD5_MAX_RACER_SLOTS) return;
    if (mag <= 0) return;
    if (effect_slot == TD5_FF_SLOT_FRONTAL) {
        if (mag > s_ff_pulse_mag[slot]) s_ff_pulse_mag[slot] = mag;
        if (TD5_FF_COLLISION_PULSE_TICKS > s_ff_pulse_ticks[slot])
            s_ff_pulse_ticks[slot] = TD5_FF_COLLISION_PULSE_TICKS;
    } else { /* TD5_FF_SLOT_SIDE */
        if (mag > s_ff_side_mag[slot]) s_ff_side_mag[slot] = mag;
        if (TD5_FF_COLLISION_PULSE_TICKS > s_ff_side_ticks[slot])
            s_ff_side_ticks[slot] = TD5_FF_COLLISION_PULSE_TICKS;
    }
    /* Arm the terrain-dampen countdown (auto-expires in the FF manager). */
    s_ff.collision_active[slot] = TD5_FF_COLLISION_PULSE_TICKS;
}

void td5_input_ff_collision(int contact_side, int actor_a_slot,
                            int actor_b_slot, int raw_magnitude)
{
    int mag = raw_magnitude / TD5_INPUT_FF_COLLISION_DIV;

    /* [item #5(1)] STRONGER collisions: boost the divided magnitude and floor it
     * so wall/car hits are clearly felt, then clamp to the device nominal max so
     * we never exceed DI_FFNOMINALMAX. (raw==0 -> mag 0 stays 0: no spurious
     * pulse from a zero-magnitude call.) */
    if (mag > 0) {
        mag = (int)(((long)mag * ff_collision_gain_q8()) >> 8);   /* Q8 gain (live-tunable) */
        if (mag < TD5_FF_COLLISION_FLOOR) mag = TD5_FF_COLLISION_FLOOR;
    }
    if (mag > TD5_INPUT_FF_COLLISION_MAX) mag = TD5_INPUT_FF_COLLISION_MAX;

    int mag_a = mag;
    int mag_b = mag;

    int slot_a_effect, slot_b_effect;

    /* [item #5(2)] DIRECTIONAL collisions. The contact_side bitmask carries the
     * impact side; we use LEFT (0x10/0x20) vs RIGHT (0x40/0x80) to drive the
     * correct side of the device:
     *   - left-side impact  -> SIDE  slot (slot 2 -> XInput LOW/left motor;
     *                          DI wheel X-axis constant force)
     *   - right-side impact -> FRONTAL slot (slot 1 -> XInput HIGH/right motor;
     *                          DI wheel Y-axis constant force, distinct direction)
     * NOTE: XInput's low/high motors are FREQUENCY-not-spatial (a gamepad has no
     * true left/right shaker), but routing left->low and right->high still makes a
     * left impact feel audibly/tactilely DISTINCT from a right one; on a DI wheel
     * the two slots are different constant-force axes/directions, which IS spatial.
     * The faithful a/b frontal-vs-side mapping is preserved for front/rear hits and
     * for the second vehicle (B) of a V2V pair, and as the fallback when the
     * directional sub-knob is off. */
    int dir = td5_ff_collision_dir_enabled();
    switch (contact_side) {
    case 0x01: case 0x02: case 0x04: case 0x08:
        /* Front/rear contact: A gets frontal impact, B gets side impact */
        slot_a_effect = TD5_FF_SLOT_FRONTAL;
        slot_b_effect = TD5_FF_SLOT_SIDE;
        break;

    case 0x10: case 0x20:
        /* LEFT-side contact. Directional: A (the impacted car) -> SIDE (low/left
         * motor). B (the other car, hit on ITS right) -> FRONTAL. Symmetric
         * fallback keeps the original A=SIDE/B=FRONTAL. */
        slot_a_effect = TD5_FF_SLOT_SIDE;
        slot_b_effect = TD5_FF_SLOT_FRONTAL;
        break;

    case 0x40: case 0x80:
        /* RIGHT-side contact. Directional: A -> FRONTAL (high/right motor); the
         * distinct routing vs the LEFT case above is what makes left/right feel
         * different on both gamepad and wheel. When the sub-knob is off, fall back
         * to the faithful A=SIDE/B=FRONTAL so behaviour matches the legacy switch. */
        slot_a_effect = dir ? TD5_FF_SLOT_FRONTAL : TD5_FF_SLOT_SIDE;
        slot_b_effect = dir ? TD5_FF_SLOT_SIDE    : TD5_FF_SLOT_FRONTAL;
        break;

    default:
        /* General collision: both get side impact */
        slot_a_effect = TD5_FF_SLOT_SIDE;
        slot_b_effect = TD5_FF_SLOT_SIDE;
        break;
    }

    /* Arm a decaying pulse on each vehicle's player (auto-releases in the FF
     * manager — no more persistent, never-stopped collision force). */
    ff_arm_collision_pulse(actor_a_slot, slot_a_effect, mag_a);
    ff_arm_collision_pulse(actor_b_slot, slot_b_effect, mag_b);
}

/* [COP CHASE ARREST 2026-06-25] One-shot strong+short jolt on one slot's wheel/pad.
 * Reuses the decaying collision-pulse path (TD5_FF_COLLISION_PULSE_TICKS ~= 12
 * render frames) so it self-releases — never the never-stopped constant-force path.
 * Fired for BOTH the arresting cop and the busted suspect when an arrest lands. */
void td5_input_ff_jolt(int slot, int magnitude)
{
    if (slot < 0 || slot >= TD5_MAX_RACER_SLOTS) return;
    if (magnitude <= 0) return;
    if (magnitude > TD5_INPUT_FF_COLLISION_MAX) magnitude = TD5_INPUT_FF_COLLISION_MAX;
    ff_arm_collision_pulse(slot, TD5_FF_SLOT_FRONTAL, magnitude);
    TD5_LOG_I(LOG_TAG, "FF arrest jolt: slot=%d mag=%d", slot, magnitude);
}

/* ========================================================================
 * Device Enumeration & Configuration
 * ======================================================================== */

int td5_input_enumerate_devices(void)
{
    return td5_plat_input_enumerate_devices();
}

void td5_input_set_device(int player, int device_index)
{
    td5_plat_input_set_device(player, device_index);
}

const char *td5_input_get_device_name(int index)
{
    return td5_plat_input_device_name(index);
}

int td5_input_get_device_type(int player)
{
    if (player < 0 || player >= TD5_MAX_HUMAN_PLAYERS) return 0;
    int source = s_input_source[player];
    if (source == 0) return 0;  /* keyboard */
    /* source is 1-based device index for joysticks */
    return td5_plat_input_device_type(source);
}
