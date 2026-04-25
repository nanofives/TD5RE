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
#include "td5_camera.h"

/* Defined in td5_game.c */
extern int td5_game_is_wanted_mode(void);
extern int    g_actorSlotForView[2];
extern uint8_t *g_actor_table_base;

#include <string.h>
#include <stdlib.h>

#define LOG_TAG "input"

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

/** NOS cooldown per actor (actor+0x37C). */
static uint8_t s_nos_cooldown[TD5_MAX_RACER_SLOTS];

/** Camera change cooldown per-player (mirrors 0x4AAF98). */
static int32_t s_camera_cooldown[TD5_MAX_RACER_SLOTS];

/** Rear view flag per-player (mirrors 0x466F88). */
static int32_t s_rear_view[TD5_MAX_RACER_SLOTS];

/* ========================================================================
 * Internal State -- Input Source Config
 *
 * 0 = keyboard, 1+ = joystick index + 1.
 * ======================================================================== */

static int s_input_source[2] = { 0, 0 };

/** Number of active human players (1 or 2). */
static int s_active_players = 1;

/** NOS feature enabled globally. */
static int s_nos_enabled = 0;

/** Cop/encounter mode active. */
static int s_cop_mode = 0;

/** Nitro pending flag per-player (set by game logic, read here). */
static int s_nitro_pending[2] = { 0, 0 };

/* ========================================================================
 * Internal State -- Playback/Replay
 * ======================================================================== */

static int s_playback_active = 0;
static int s_replay_mode_flag = 0;

/* ========================================================================
 * Internal State -- Recording Buffer
 * ======================================================================== */

static TD5_InputRecordBuffer s_rec;

/* ========================================================================
 * Internal State -- Force Feedback
 * ======================================================================== */

static TD5_FFGameState s_ff;

/* ========================================================================
 * Internal State -- Escape / Fade
 * ======================================================================== */

static int s_escape_muted = 0;
static int s_escape_fade_active = 0;

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
    memset(s_nos_cooldown, 0, sizeof(s_nos_cooldown));
    memset(s_camera_cooldown, 0, sizeof(s_camera_cooldown));
    memset(s_rear_view, 0, sizeof(s_rear_view));
    memset(&s_rec, 0, sizeof(s_rec));
    memset(&s_ff, 0, sizeof(s_ff));
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

void td5_input_set_active_players(int n)       { s_active_players = clamp_i(n, 1, 2); }
void td5_input_set_input_source(int p, int s)   { if (p >= 0 && p < 2) s_input_source[p] = s; }
void td5_input_set_playback_active(int v)       { s_playback_active = v; }
void td5_input_set_replay_mode(int v)           { s_replay_mode_flag = v; }
void td5_input_set_nos_enabled(int v)           { s_nos_enabled = v; }
void td5_input_set_cop_mode(int v)              { s_cop_mode = v; }
void td5_input_set_nitro_pending(int p, int v)  { if (p >= 0 && p < 2) s_nitro_pending[p] = v; }

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

        /* OR in nitro pending flag from game logic */
        if (s_nitro_pending[i]) {
            s_control_bits[i] |= 0x10000000u;
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
            /* Original calls CycleRaceCameraPreset(i, 1) then
             * LoadCameraPresetForView(actor+0x208, 1, i, 1). (0x42C470) */
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
                    LoadCameraPresetForView(
                        (int)((uint8_t *)actor + 0x208), 1, i, 1);
                }
            }
            s_camera_cooldown[i] = TD5_INPUT_CAMERA_COOLDOWN;
            TD5_LOG_I(LOG_TAG, "camera change: player=%d", i);
        }

        /* Rear view flag */
        if (((s_control_bits[i] & 0x2000000u) == 0) ||
            s_replay_mode_flag || s_escape_fade_active)
        {
            if (s_rear_view[i]) {
                td5_camera_set_rear_view(i, 0);
                TD5_LOG_I(LOG_TAG, "rear view OFF: player=%d", i);
            }
            s_rear_view[i] = 0;
        } else {
            if (!s_rear_view[i]) {
                td5_camera_set_rear_view(i, 1);
                TD5_LOG_I(LOG_TAG, "rear view ON: player=%d", i);
            }
            s_rear_view[i] = 1;
        }
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

    /* Check F3 key for keyboard camera cycling (scan code 0x3D) */
    if (td5_plat_input_key_pressed(0x3D)) {
        /* Camera cycle logic placeholder -- original toggles a flag */
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
                    LoadCameraPresetForView(
                        (int)((uint8_t *)actor + 0x208), 1, i, 1);
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
     * The original code reads:
     *   speed = abs(actor.velocity >> 8)  (actor+0x314)
     *   speed_sq = actor.velocity_sq >> 8 (actor+0x31C)
     *
     * Without live actor data we use placeholder values.
     * The real integration happens when the physics module calls this
     * function and provides the actor state.  For the stub we compute
     * with speed=0 which gives maximum steering authority.
     */

    /* Read live speed from actor physics state */
    int speed = 0;
    int speed_sq = 0;
    int vehicle_stopped = 1;
    int encounter_active = 0;
    {
        TD5_Actor *actor = td5_game_get_actor(slot);
        if (actor) {
            uint8_t *abytes = (uint8_t *)actor;
            /* +0x314 = lateral_speed (int32), same field the original reads */
            int32_t raw_speed = *(int32_t *)(abytes + 0x314);
            speed = (raw_speed < 0 ? -raw_speed : raw_speed) >> 8;
            /* +0x31C = velocity_sq (int32), pre-computed by physics */
            speed_sq = *(int32_t *)(abytes + 0x31C) >> 8;
            /* Vehicle is stopped if speed is below threshold */
            vehicle_stopped = (speed < 100) ? 1 : 0;
        }
        encounter_active = td5_game_is_wanted_mode();
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

    /* Path correction bias (simplified -- full version reads track heading).
     * In the stub this is 0; the physics module will supply the real value.
     * int path_bias = 0;  -- reserved for physics integration */

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

        /*
         * Original uses a fixed-point multiply:
         *   steering_command = (raw_x * steer_limit) / divisor
         * The magic constant 0x10624DD3 is approx 1/250 in fixed-point.
         * Result: steering_command = raw_x * steer_limit / 250
         */
        int64_t product = (int64_t)raw_x * (int64_t)(raw_x < 0 ? neg_limit : pos_limit);
        /* Divide by 250 (the axis range) using the same magic constant approach */
        int64_t magic = (int64_t)0x10624DD3;
        if (raw_x >= 0) magic = -(int64_t)0x10624DD3;
        int64_t hi = (magic * (int64_t)(raw_x * (raw_x < 0 ? neg_limit : pos_limit))) >> 32;
        s_steering_cmd[slot] = (int)((hi >> 4) - (hi >> 31));
    }

    /* ---- Throttle / Brake ---- */
    if (bits & TD5_INPUT_ANALOG_Y_FLAG) {
        /* Analog Y-axis */
        int raw_y = (int)((bits >> 9) & 0x1FF) - TD5_INPUT_JS_AXIS_CENTER;

        if (raw_y > -TD5_INPUT_ANALOG_Y_DEADZONE &&
            raw_y < TD5_INPUT_ANALOG_Y_DEADZONE)
        {
            /* Deadzone -- no throttle or brake */
            s_throttle[slot] = 0;
            s_brake[slot] = 0;
            s_reverse_req[slot] = 0;
        } else if (raw_y >= TD5_INPUT_ANALOG_Y_DEADZONE) {
            /* Positive Y = brake */
            s_throttle[slot] = (int16_t)0xFF00; /* full brake */
            s_brake[slot] = 1;
            s_reverse_req[slot] = 0;
        } else {
            /* Negative Y = accelerate */
            s_throttle[slot] = 0xFF;  /* full digital throttle (0-255 range, >>8 in RPM formula) */
            s_brake[slot] = 0;
            s_reverse_req[slot] = 0;
        }
    } else {
        /* Digital throttle/brake */
        s_handbrake[slot] = 0;

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
        } else if (encounter_active) {
            /* Encounter steering override -- placeholder */
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

        /* Handbrake (bit 0x100000) — level-triggered via K_DOWN, so the flag
         * stays set for as long as Space is held. No throttle / reverse
         * override: a drift handbrake cuts rear grip via tuning+0x7A, it does
         * NOT brake the car or shift to reverse. The physics path already
         * halves grip[2]/grip[3] while actor->handbrake_flag is set. */
        if (bits & 0x100000u) {
            s_handbrake[slot] = 1;
        }
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

    /* Cop mode: horn zeroes other actors' velocities.
     * Deferred to physics module integration. */

    /* ---- Gear Changes (5-frame debounce) ---- */
    if (((~(bits >> 28)) & 1) == 0) {
        /* NOS flag set means stunned, skip gear changes */
    } else if (s_gear_debounce[slot] == 0) {
        /* Gear up (bit 0x400000) */
        if (bits & 0x400000u) {
            if (s_gear[slot] < 7) {  /* placeholder max gear */
                s_gear[slot]++;
            }
            s_gear_debounce[slot] = TD5_INPUT_GEAR_DEBOUNCE;
        }

        /* Gear down (bit 0x800000) */
        if (bits & 0x800000u) {
            if (s_gear[slot] > 0) {
                /* Downshift RPM protection would go here --
                 * checks if RPM would exceed redline.
                 * Deferred to physics module. */
                s_gear[slot]--;
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
                s_throttle[slot] = 0x100;
                s_steering_cmd[slot] = 0;
                s_brake[slot] = 0;
            } else if (slot == 0) {
                static uint32_t s_at_log2 = 0;
                if ((s_at_log2++ % 60u) == 0u) {
                    TD5_LOG_I(LOG_TAG,
                              "AutoThrottle INACTIVE: auto_throttle=%d steer=%d",
                              g_td5.ini.auto_throttle, s_steering_cmd[slot]);
                }
            }

            /* 0x30C: steering_command (int32) */
            *(int32_t *)(a + 0x30C) = s_steering_cmd[slot];
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

/* ========================================================================
 * ResetPlayerVehicleControlAccumulators  (0x402E30)
 *
 * Zeroes the NOS latch accumulators for all players.
 * Original: _DAT_00483014 = 0; _DAT_00483018 = 0;
 * ======================================================================== */

void td5_input_reset_accumulators(void)
{
    for (int i = 0; i < TD5_MAX_RACER_SLOTS; i++) {
        s_nos_latch[i] = 0;
    }
}

/* ========================================================================
 * ResetPlayerVehicleControlBuffers  (0x402E40)
 *
 * Zeroes the gear-change debounce counters for all 6 slots.
 * Original: memset(DAT_00482FE4, 0, 6 * 4)
 * ======================================================================== */

void td5_input_reset_buffers(void)
{
    for (int i = 0; i < TD5_MAX_RACER_SLOTS; i++) {
        s_gear_debounce[i] = 0;
    }
}

/* ========================================================================
 * Control State Queries
 * ======================================================================== */

uint32_t td5_input_get_control_bits(int slot)
{
    if (slot < 0 || slot >= TD5_MAX_RACER_SLOTS) return 0;
    return s_control_bits[slot];
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

    TD5_LOG_I(LOG_TAG, "Replay write open: path=%s track=%d",
              s_replay_log_path, s_rec.track_index);
    return 1;
}

void td5_input_write_close(void)
{
    if (s_rec.frame_cursor > 0) {
        s_rec.last_frame_index = (int32_t)(s_rec.frame_cursor - 1);
    }
    TD5_LOG_I(LOG_TAG, "Replay write close: path=%s entries=%d frames=%u",
              s_replay_log_path, s_rec.entry_count, s_rec.frame_cursor);
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

int td5_input_read_open(const char *path)
{
    /* Reset read cursor; the buffer already contains recorded data */
    strncpy(s_replay_log_path, path ? path : "<memory>", sizeof(s_replay_log_path) - 1);
    s_replay_log_path[sizeof(s_replay_log_path) - 1] = '\0';
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

    /* Attempt to init FF on device 0 (primary joystick).
     * CreateRaceForceFeedbackEffects (0x4285B0) creates 4 effect slots:
     *   Slot 0: Constant force -- steering resistance
     *   Slot 1: Constant force -- frontal collision impact
     *   Slot 2: Constant force -- side collision impact
     *   Slot 3: Spring/condition -- terrain vibration + centering
     *
     * In the source port, the platform layer handles the actual
     * DirectInput effect creation.  We just request initialization. */
    int ok = td5_plat_ff_init(0);
    if (ok) {
        TD5_LOG_I(LOG_TAG, "Force feedback initialized on device 0");
    } else {
        TD5_LOG_W(LOG_TAG, "Force feedback not available");
    }
    return ok;
}

void td5_input_ff_shutdown(void)
{
    for (int slot = 0; slot < 4; slot++) {
        td5_plat_ff_stop(slot);
    }
    td5_plat_ff_shutdown();
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
void td5_input_ff_update(void)
{
    /* Dispatch per-player FF update for all active local players.
     * [CONFIRMED @ 0x0042C470]: UpdateControllerForceFeedback dispatched
     * inside PollRaceSessionInput for slot 0 (single-player) or the
     * local participant slot (network). */
    td5_input_ff_update_player(0);
    if (s_active_players > 1) {
        td5_input_ff_update_player(1);
    }
    TD5_LOG_D(LOG_TAG, "FF dispatcher: players=%d", s_active_players);
}

void td5_input_ff_stop(void)
{
    for (int slot = 0; slot < 4; slot++) {
        TD5_LOG_I(LOG_TAG, "FF stop: slot=%d", slot);
        td5_plat_ff_stop(slot);
    }
    for (int i = 0; i < 2; i++) {
        s_ff.steer_effect_started[i] = 0;
        s_ff.terrain_effect_started[i] = 0;
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
 * ======================================================================== */

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

void td5_input_ff_update_player(int slot)
{
    if (slot < 0 || slot >= TD5_MAX_RACER_SLOTS) return;
    if (s_ff.controller_assignment[slot] == 0) return;

    int js_idx = s_ff.controller_assignment[slot] - 1;
    if (js_idx < 0 || js_idx > 1) return;

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

    if (!s_ff.steer_effect_started[js_idx]) {
        /* First-time play: start the effect */
        td5_plat_ff_constant(0, steer_mag);
        s_ff.steer_effect_started[js_idx] = 1;
    } else {
        /* Update running effect parameters */
        td5_plat_ff_constant(0, steer_mag);
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

    if (!s_ff.terrain_effect_started[js_idx]) {
        /* First play of terrain effect */
        td5_plat_ff_constant(3, terrain_mag);
        s_ff.terrain_effect_started[js_idx] = 1;
        s_ff.collision_active[slot] = 0;
    } else {
        if (s_ff.collision_active[slot] != 0) {
            /* During collision recovery, dampen terrain vibration */
            terrain_mag = TD5_INPUT_FF_COLLISION_DAMPEN;
        }
        td5_plat_ff_constant(3, terrain_mag);
    }
}

/* ========================================================================
 * PlayAssignedControllerEffect  (0x428A10)
 *
 * A thin helper to fire arbitrary effects on a player's assigned
 * controller.  Validates slot and FF capability before dispatching.
 * ======================================================================== */

void td5_input_ff_play_effect(int slot, int effect_slot, int magnitude,
                              int repeat_count)
{
    (void)effect_slot;
    (void)repeat_count;

    if (slot < 0 || slot >= TD5_MAX_RACER_SLOTS) return;
    if (s_ff.controller_assignment[slot] == 0) return;

    if (effect_slot < 0) effect_slot = 0;
    if (effect_slot > 3) effect_slot = 3;
    TD5_LOG_I(LOG_TAG, "FF start: player=%d slot=%d magnitude=%d repeat=%d",
              slot, effect_slot, magnitude, repeat_count);
    td5_plat_ff_constant(effect_slot, magnitude);
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
 * ======================================================================== */

void td5_input_ff_collision(int contact_side, int actor_a_slot,
                            int actor_b_slot, int raw_magnitude)
{
    int mag = raw_magnitude / TD5_INPUT_FF_COLLISION_DIV;
    if (mag > TD5_INPUT_FF_COLLISION_MAX) mag = TD5_INPUT_FF_COLLISION_MAX;

    int mag_a = mag;
    int mag_b = mag;

    int slot_a_effect, slot_b_effect;

    switch (contact_side) {
    case 0x01: case 0x02: case 0x04: case 0x08:
        /* Front/rear contact: A gets frontal impact, B gets side impact */
        slot_a_effect = TD5_FF_SLOT_FRONTAL;
        slot_b_effect = TD5_FF_SLOT_SIDE;
        break;

    case 0x10: case 0x20: case 0x40: case 0x80:
        /* Left/right contact: B gets frontal impact, A gets side impact */
        slot_a_effect = TD5_FF_SLOT_SIDE;
        slot_b_effect = TD5_FF_SLOT_FRONTAL;
        break;

    default:
        /* General collision: both get side impact */
        slot_a_effect = TD5_FF_SLOT_SIDE;
        slot_b_effect = TD5_FF_SLOT_SIDE;
        break;
    }

    /* Fire effect on vehicle A's player */
    if (actor_a_slot >= 0 && actor_a_slot < TD5_MAX_RACER_SLOTS) {
        td5_input_ff_play_effect(actor_a_slot, slot_a_effect, mag_a, 0);
        s_ff.collision_active[actor_a_slot] = 1;
    }

    /* Fire effect on vehicle B's player */
    if (actor_b_slot >= 0 && actor_b_slot < TD5_MAX_RACER_SLOTS) {
        td5_input_ff_play_effect(actor_b_slot, slot_b_effect, mag_b, 0);
        s_ff.collision_active[actor_b_slot] = 1;
    }
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
    if (player < 0 || player >= 2) return 0;
    int source = s_input_source[player];
    if (source == 0) return 0;  /* keyboard */
    /* source is 1-based device index for joysticks */
    return td5_plat_input_device_type(source);
}
