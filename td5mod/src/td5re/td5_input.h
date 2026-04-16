/**
 * td5_input.h -- Input polling, controller config, force feedback,
 *                and input recording/playback.
 *
 * Original functions:
 *   0x402E30  ResetPlayerVehicleControlAccumulators
 *   0x402E40  ResetPlayerVehicleControlBuffers
 *   0x402E60  UpdatePlayerVehicleControlState
 *   0x42C470  PollRaceSessionInput
 *   0x428880  ConfigureForceFeedbackControllers
 *   0x4288E0  UpdateControllerForceFeedback
 *   0x428A10  PlayAssignedControllerEffect
 *   0x428A60  PlayVehicleCollisionForceFeedback
 *   0x4285B0  CreateRaceForceFeedbackEffects
 *   0x40FE00  ScreenControllerBindingPage
 *
 * The game logic operates on a 32-bit per-player bitmask (TD5_InputBits)
 * plus optional packed analog axis values.  Platform-specific DirectInput
 * calls are abstracted through td5_platform.h.
 */

#ifndef TD5_INPUT_H
#define TD5_INPUT_H

#include "td5_types.h"

/* ========================================================================
 * Constants
 * ======================================================================== */

/** Maximum number of joystick buttons tracked per device. */
#define TD5_INPUT_MAX_JS_BUTTONS        10

/** Joystick digital threshold (32% of +/-250 range). */
#define TD5_INPUT_JS_DIGITAL_THRESHOLD  0x50

/** Joystick axis center/calibration default. */
#define TD5_INPUT_JS_AXIS_CENTER        0xFA

/** Analog Y-axis deadzone threshold (+/-10 raw). */
#define TD5_INPUT_ANALOG_Y_DEADZONE     10

/** Maximum steering command magnitude. */
#define TD5_INPUT_STEER_MAX             0x18000

/** Steering rate numerator. */
#define TD5_INPUT_STEER_RATE_NUM        0xC0000

/** Steering limit numerator. */
#define TD5_INPUT_STEER_LIMIT_NUM       0xC00000

/** Steering ramp-up step per tick. */
#define TD5_INPUT_STEER_RAMP_STEP       0x40

/** Maximum steering ramp-up accumulator. */
#define TD5_INPUT_STEER_RAMP_MAX        0x100

/** Auto-centering rate multiplier vs turn rate. */
#define TD5_INPUT_AUTOCENTER_MULT       4

/** Camera change cooldown (frames). */
#define TD5_INPUT_CAMERA_COOLDOWN       10

/** Gear change debounce (frames). */
#define TD5_INPUT_GEAR_DEBOUNCE         5

/** NOS velocity cap. */
#define TD5_INPUT_NOS_VEL_CAP           400000

/** NOS cooldown (frames). */
#define TD5_INPUT_NOS_COOLDOWN          15

/** NOS vertical impulse. */
#define TD5_INPUT_NOS_VERT_IMPULSE      0x6400

/** FF steering force clamp. */
#define TD5_INPUT_FF_STEER_CLAMP        300000

/** FF collision dampen override magnitude. */
#define TD5_INPUT_FF_COLLISION_DAMPEN   400

/** FF collision magnitude divisor. */
#define TD5_INPUT_FF_COLLISION_DIV      10

/** FF collision max magnitude. */
#define TD5_INPUT_FF_COLLISION_MAX      10000

/** Recording strip mask (removes camera/escape bits). */
#define TD5_INPUT_RECORD_STRIP_MASK     0xBBFFFFFFu

/** Max recording timeline entries. */
#define TD5_INPUT_REC_MAX_ENTRIES       19996

/** Max recording frame index. */
#define TD5_INPUT_REC_MAX_FRAMES        0x7FEE

/** Recording channel 1 flag (bit 15). */
#define TD5_INPUT_REC_CHANNEL1_FLAG     0x8000u

/** Number of keyboard binding slots per player. */
#define TD5_INPUT_KB_BIND_SLOTS         10

/** Number of joystick binding slots per player. */
#define TD5_INPUT_JS_BIND_SLOTS         9

/** FF effect slot indices. */
#define TD5_FF_SLOT_STEER               0
#define TD5_FF_SLOT_FRONTAL             1
#define TD5_FF_SLOT_SIDE                2
#define TD5_FF_SLOT_TERRAIN             3

/* ========================================================================
 * Recording Entry
 * ======================================================================== */

/** Delta-compressed input recording timeline entry (8 bytes). */
typedef struct TD5_InputRecordEntry {
    uint32_t frame_and_channel; /**< bits[14:0]=frame, bit15=channel flag */
    uint32_t value;             /**< 32-bit control bitmask snapshot */
} TD5_InputRecordEntry;

/* ========================================================================
 * Recording State
 * ======================================================================== */

/** In-memory recording/playback buffer. */
typedef struct TD5_InputRecordBuffer {
    int32_t  track_index;
    int32_t  entry_count;
    int32_t  last_frame_index;
    TD5_InputRecordEntry entries[TD5_INPUT_REC_MAX_ENTRIES];

    /* Write state */
    uint32_t frame_cursor;
    uint32_t last_word0;
    uint32_t last_word1;

    /* Read state */
    uint32_t read_index;
    uint32_t replay_word0;
    uint32_t replay_word1;
} TD5_InputRecordBuffer;

/* ========================================================================
 * Per-Player Control State
 *
 * Mirrors the actor struct fields written by UpdatePlayerVehicleControlState.
 * ======================================================================== */

typedef struct TD5_PlayerControl {
    int32_t  steering_command;       /**< current steering, +/- TD5_INPUT_STEER_MAX */
    int16_t  steering_ramp;          /**< progressive ramp accumulator (0..0x100) */
    int16_t  throttle_raw;           /**< decoded throttle value */
    uint8_t  brake_flag;             /**< 1 = brake active */
    uint8_t  handbrake_flag;         /**< 1 = handbrake active */
    uint8_t  reverse_flag;           /**< 1 = reverse requested */
    uint8_t  gear_index;             /**< current gear (0=rev, 1=neutral, 2+=forward) */
    uint8_t  gear_max;               /**< max gear from car params */
    uint8_t  _pad0;
    int32_t  gear_debounce;          /**< frames remaining before next shift allowed */
    uint8_t  nos_latch;              /**< NOS already triggered this press */
    uint8_t  nos_cooldown;           /**< frames until NOS available again */
    uint8_t  camera_rear_view;       /**< 1 = rear view held */
    uint8_t  stunned;                /**< 1 = vehicle stunned / NOS flag clear */
} TD5_PlayerControl;

/* ========================================================================
 * Force Feedback State
 * ======================================================================== */

/** Per-player FF assignment and state. */
typedef struct TD5_FFGameState {
    int32_t  controller_assignment[TD5_MAX_RACER_SLOTS]; /**< 1-based JS idx, 0=none */
    int32_t  collision_active[TD5_MAX_RACER_SLOTS];      /**< dampens terrain FF */
    int      steer_effect_started[2];   /**< per-JS: slot 0 started? */
    int      terrain_effect_started[2]; /**< per-JS: slot 3 started? */
} TD5_FFGameState;

/* ========================================================================
 * Terrain FF Coefficient Table (13 entries)
 * ======================================================================== */

extern const int32_t g_terrain_ff_coefficients[13];

/* ========================================================================
 * Public API -- Lifecycle
 * ======================================================================== */

int  td5_input_init(void);
void td5_input_shutdown(void);
void td5_input_tick(void);

/* ========================================================================
 * Public API -- Race Input Polling (PollRaceSessionInput)
 * ======================================================================== */

void td5_input_poll_race_session(void);

/* ========================================================================
 * Public API -- Vehicle Control Interpreter (UpdatePlayerVehicleControlState)
 * ======================================================================== */

void td5_input_update_player_control(int slot);

/* ========================================================================
 * Public API -- Reset Functions
 * ======================================================================== */

/** Zero the NOS latch accumulators for all players. (0x402E30) */
void td5_input_reset_accumulators(void);

/** Zero the gear-change debounce buffers for all players. (0x402E40) */
void td5_input_reset_buffers(void);

/* ========================================================================
 * Public API -- Control State Queries
 * ======================================================================== */

uint32_t td5_input_get_control_bits(int slot);
int16_t  td5_input_get_analog_x(int slot);
int16_t  td5_input_get_analog_y(int slot);
int32_t  td5_input_get_steering_cmd(int slot);
int16_t  td5_input_get_throttle(int slot);
uint8_t  td5_input_get_brake(int slot);
uint8_t  td5_input_get_handbrake(int slot);
uint8_t  td5_input_get_gear(int slot);
int      td5_input_get_rear_view(int slot);

/* ========================================================================
 * Public API -- Configuration Setters
 *
 * Called by the game/frontend modules to configure the input system.
 * ======================================================================== */

void td5_input_set_active_players(int n);
void td5_input_set_input_source(int player, int source);
void td5_input_set_playback_active(int v);
void td5_input_set_replay_mode(int v);
void td5_input_set_nos_enabled(int v);
void td5_input_set_cop_mode(int v);
void td5_input_set_nitro_pending(int player, int v);

/* ========================================================================
 * Public API -- Recording / Playback
 * ======================================================================== */

int  td5_input_write_open(const char *path);
void td5_input_write_close(void);
void td5_input_write_frame(uint32_t word0, uint32_t word1, int strip_mode);

int  td5_input_read_open(const char *path);
void td5_input_read_close(void);
int  td5_input_read_frame(uint32_t *word0, uint32_t *word1);

/* ========================================================================
 * Public API -- Force Feedback
 * ======================================================================== */

int  td5_input_ff_init(void);
void td5_input_ff_shutdown(void);
void td5_input_ff_update(int magnitude);
void td5_input_ff_stop(void);

/** Configure per-slot FF controller assignments. (0x428880) */
void td5_input_ff_configure(const int *assignments, int count);

/** Per-frame steering + terrain FF update for one player. (0x4288E0) */
void td5_input_ff_update_player(int slot);

/** Fire a one-shot FF effect on a player's controller. (0x428A10) */
void td5_input_ff_play_effect(int slot, int effect_slot, int magnitude,
                              int repeat_count);

/** Dispatch collision FF to both vehicles. (0x428A60) */
void td5_input_ff_collision(int contact_side, int actor_a_slot,
                            int actor_b_slot, int raw_magnitude);

/* ========================================================================
 * Public API -- Device Enumeration & Configuration
 * ======================================================================== */

int  td5_input_enumerate_devices(void);
void td5_input_set_device(int player, int device_index);
const char *td5_input_get_device_name(int index);

/** Get the controller type for a player slot.
 *  Returns 0=keyboard, 1=gamepad, 2=joystick/wheel. */
int td5_input_get_device_type(int player);

#endif /* TD5_INPUT_H */
