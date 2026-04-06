/**
 * td5_types.h -- Shared types for TD5RE source port
 *
 * Consolidates all common data types, fixed-point math, actor struct,
 * enums, and constants from the reverse-engineered headers in re/include/.
 *
 * This header has ZERO platform dependencies -- only C11 standard library.
 */

#ifndef TD5_TYPES_H
#define TD5_TYPES_H

#include <stdint.h>
#include <stddef.h>

/* ========================================================================
 * Fixed-Point Math
 * ======================================================================== */

/** 24.8 fixed-point shift */
#define TD5_FP_SHIFT            8
#define TD5_FP_ONE              (1 << TD5_FP_SHIFT)       /* 256 */
#define TD5_FP_HALF             (1 << (TD5_FP_SHIFT - 1)) /* 128 */

/** Convert int to 24.8 fixed-point */
#define TD5_INT_TO_FP(x)       ((int32_t)(x) << TD5_FP_SHIFT)

/** Convert 24.8 fixed-point to int (truncating) */
#define TD5_FP_TO_INT(x)       ((int32_t)(x) >> TD5_FP_SHIFT)

/** Convert 24.8 fixed-point to float */
#define TD5_FP_TO_FLOAT(x)     ((float)(x) * (1.0f / 256.0f))

/** Convert float to 24.8 fixed-point */
#define TD5_FLOAT_TO_FP(x)     ((int32_t)((x) * 256.0f))

/** Fixed-point multiply: (a * b) >> 8 */
#define TD5_FP_MUL(a, b)       (((int32_t)(a) * (int32_t)(b)) >> TD5_FP_SHIFT)

/** Fixed-point multiply with rounding: (a * b + 128) >> 8 */
#define TD5_FP_MUL_R(a, b)     (((int32_t)(a) * (int32_t)(b) + TD5_FP_HALF) >> TD5_FP_SHIFT)

/** Fixed-point divide: (a << 8) / b */
#define TD5_FP_DIV(a, b)       (((int32_t)(a) << TD5_FP_SHIFT) / (int32_t)(b))

/* ========================================================================
 * Angle Constants (12-bit, 0-4095 = 0-360 degrees)
 * ======================================================================== */

#define TD5_ANGLE_SHIFT         8           /* accumulator >> 8 = 12-bit display */
#define TD5_ANGLE_FULL          0x1000      /* 4096 = full circle */
#define TD5_ANGLE_HALF          0x0800      /* 180 degrees */
#define TD5_ANGLE_QUARTER       0x0400      /* 90 degrees */
#define TD5_ANGLE_MASK          0x0FFF      /* 12-bit mask */

/* ========================================================================
 * Simulation Constants
 * ======================================================================== */

#define TD5_TICK_ACCUMULATOR_ONE  0x10000   /* one full simulation tick */
#define TD5_MAX_SIM_BUDGET        4.0f     /* max ticks per frame (spiral-of-death cap) */

/** Gravity constants per difficulty */
#define TD5_GRAVITY_EASY        0x05DC      /* 1500 */
#define TD5_GRAVITY_NORMAL      0x076C      /* 1900 */
#define TD5_GRAVITY_HARD        0x0800      /* 2048 */

/** Engine constants */
#define TD5_ENGINE_IDLE_RPM     400
#define TD5_ENGINE_MAX_SLEW_UP  400
#define TD5_ENGINE_MAX_SLEW_DN  200

/* ========================================================================
 * Actor / Race Constants
 * ======================================================================== */

#define TD5_ACTOR_STRIDE            0x388   /* 904 bytes per actor */
#define TD5_MAX_RACER_SLOTS         6
#define TD5_MAX_TRAFFIC_SLOTS       6
#define TD5_MAX_TOTAL_ACTORS        12      /* 6 racers + 6 traffic */

#define TD5_GEAR_REVERSE            0
#define TD5_GEAR_NEUTRAL            1
#define TD5_GEAR_FIRST              2

/** Grip clamp ranges */
#define TD5_PLAYER_GRIP_MIN         0x38    /* 56 */
#define TD5_PLAYER_GRIP_MAX         0x50    /* 80 */
#define TD5_AI_GRIP_MIN             0x70    /* 112 */
#define TD5_AI_GRIP_MAX             0xA0    /* 160 */

/** Steering limits */
#define TD5_STEER_MAX               0x18000
#define TD5_YAW_TORQUE_MAX          0x0578

/* ========================================================================
 * Rendering Constants
 * ======================================================================== */

#define TD5_MAX_TRANSLUCENT_BATCHES 510
#define TD5_DEPTH_SORT_BUCKETS      4096
#define TD5_MESH_VERTEX_STRIDE      0x2C    /* 44 bytes */
#define TD5_LIGHTING_MIN            0x40
#define TD5_LIGHTING_MAX            0xFF
#define TD5_UV_SCALE                0.984375f
#define TD5_UV_OFFSET               0.0078125f

/* ========================================================================
 * Track Constants
 * ======================================================================== */

#define TD5_STRIP_SPAN_STRIDE       24      /* bytes per span record */
#define TD5_STRIP_VERTEX_STRIDE     6       /* 3 x int16 per vertex */
#define TD5_STRIP_HEADER_DWORDS     5
#define TD5_MAX_TEXTURE_CACHE_SLOTS 600
#define TD5_TRACK_TEXTURE_DIM       64

/* ========================================================================
 * Save System Constants
 * ======================================================================== */

#define TD5_CONFIG_FILE_SIZE        5351
#define TD5_CUPDATA_FILE_SIZE       12966
#define TD5_CONFIG_XOR_KEY          "Outta Mah Face !! "
#define TD5_CUPDATA_XOR_KEY         "Steve Snake says : No Cheating! "

/* ========================================================================
 * Network Constants
 * ======================================================================== */

#define TD5_NET_FRAME_MSG_SIZE      0x80    /* 128 bytes per DXPFRAME */
#define TD5_NET_MAX_PLAYERS         6
#define TD5_NET_RING_BUFFER_SIZE    16

/* ========================================================================
 * Enumerations
 * ======================================================================== */

/** Main game state machine */
typedef enum TD5_GameState {
    TD5_GAMESTATE_INTRO     = 0,
    TD5_GAMESTATE_MENU      = 1,
    TD5_GAMESTATE_RACE      = 2,
    TD5_GAMESTATE_BENCHMARK = 3
} TD5_GameState;

/** Selected game mode */
typedef enum TD5_GameType {
    TD5_GAMETYPE_SINGLE_RACE  = 0,
    TD5_GAMETYPE_CHAMPIONSHIP = 1,
    TD5_GAMETYPE_ERA          = 2,
    TD5_GAMETYPE_CHALLENGE    = 3,
    TD5_GAMETYPE_PITBULL      = 4,
    TD5_GAMETYPE_MASTERS      = 5,
    TD5_GAMETYPE_ULTIMATE     = 6,
    TD5_GAMETYPE_TIME_TRIAL   = 7,
    TD5_GAMETYPE_COP_CHASE    = 8,
    TD5_GAMETYPE_DRAG_RACE    = 9
} TD5_GameType;

/** Weather type */
typedef enum TD5_WeatherType {
    TD5_WEATHER_RAIN  = 0,
    TD5_WEATHER_SNOW  = 1,
    TD5_WEATHER_CLEAR = 2
} TD5_WeatherType;

/** Track layout type */
typedef enum TD5_TrackType {
    TD5_TRACK_CIRCUIT        = 0,
    TD5_TRACK_POINT_TO_POINT = 1
} TD5_TrackType;

/** Difficulty tier */
typedef enum TD5_Difficulty {
    TD5_DIFFICULTY_EASY   = 0,
    TD5_DIFFICULTY_NORMAL = 1,
    TD5_DIFFICULTY_HARD   = 2
} TD5_Difficulty;

/** Vehicle physics mode */
typedef enum TD5_VehicleMode {
    TD5_VMODE_NORMAL   = 0,
    TD5_VMODE_SCRIPTED = 1
} TD5_VehicleMode;

/** Drivetrain type */
typedef enum TD5_DrivetrainType {
    TD5_DRIVE_FWD = 1,
    TD5_DRIVE_RWD = 2,
    TD5_DRIVE_AWD = 3
} TD5_DrivetrainType;

/** Surface type */
typedef enum TD5_SurfaceType {
    TD5_SURFACE_DEFAULT     = 0,
    TD5_SURFACE_DRY_ASPHALT = 1,
    TD5_SURFACE_WET_ASPHALT = 2,
    TD5_SURFACE_DIRT        = 3,
    TD5_SURFACE_GRAVEL      = 4
} TD5_SurfaceType;

/** Span type in STRIP.DAT */
typedef enum TD5_SpanType {
    TD5_SPAN_INVALID        = 0,
    TD5_SPAN_QUAD_A         = 1,
    TD5_SPAN_QUAD_B         = 2,
    TD5_SPAN_TRANSITION_A   = 3,
    TD5_SPAN_TRANSITION_B   = 4,
    TD5_SPAN_QUAD_C         = 5,
    TD5_SPAN_REVERSED_A     = 6,
    TD5_SPAN_REVERSED_B     = 7,
    TD5_SPAN_JUNCTION_FWD   = 8,
    TD5_SPAN_SENTINEL_START = 9,
    TD5_SPAN_SENTINEL_END   = 10,
    TD5_SPAN_JUNCTION_BWD   = 11
} TD5_SpanType;

/** Input bitmask flags */
typedef enum TD5_InputBits {
    TD5_INPUT_STEER_LEFT    = 0x00000001,
    TD5_INPUT_STEER_RIGHT   = 0x00000002,
    TD5_INPUT_THROTTLE      = 0x00000200,
    TD5_INPUT_BRAKE         = 0x00000400,
    TD5_INPUT_GEAR_DOWN     = 0x00080000,
    TD5_INPUT_GEAR_UP       = 0x00100000,
    TD5_INPUT_HORN          = 0x00200000,
    TD5_INPUT_STUNNED       = 0x00200000, /* same bit triggers state 0x0F */
    TD5_INPUT_ANALOG_Y_FLAG = 0x08000000,
    TD5_INPUT_ANALOG_X_FLAG = 0x80000000u
} TD5_InputBits;

/** Network message types (DXPTYPE) */
typedef enum TD5_NetMsgType {
    TD5_DXPFRAME            = 0,
    TD5_DXPDATA             = 1,
    TD5_DXPCHAT             = 2,
    TD5_DXPDATA_TARGETED    = 3,
    TD5_DXPSTART            = 4,
    TD5_DXPACK_REQUEST      = 5,
    TD5_DXPACK_REPLY        = 6,
    TD5_DXPSTART_CONFIRM    = 7,
    TD5_DXPROSTER           = 8,
    TD5_DXPDISCONNECT       = 9,
    TD5_DXPRESYNCREQ        = 10,
    TD5_DXPRESYNC           = 11,
    TD5_DXPRESYNCACK        = 12
} TD5_NetMsgType;

/** Render state presets */
typedef enum TD5_RenderPreset {
    TD5_PRESET_OPAQUE_ANISO       = 0,  /* Opaque track geometry */
    TD5_PRESET_TRANSLUCENT_LINEAR = 1,  /* Translucent, linear filter */
    TD5_PRESET_TRANSLUCENT_ANISO  = 2,  /* Translucent, aniso filter */
    TD5_PRESET_OPAQUE_LINEAR      = 3,  /* Opaque, linear filter */
    TD5_PRESET_TRANSLUCENT_POINT  = 4   /* Translucent, point (nearest-neighbour) filter */
} TD5_RenderPreset;

/** Mesh primitive dispatch opcodes */
typedef enum TD5_MeshDispatch {
    TD5_DISPATCH_TRISTRIP        = 0,
    TD5_DISPATCH_TRISTRIP_ALT    = 1,
    TD5_DISPATCH_PROJECTED_TRI   = 2,
    TD5_DISPATCH_PROJECTED_QUAD  = 3,
    TD5_DISPATCH_BILLBOARD       = 4,
    TD5_DISPATCH_TRISTRIP_DIRECT = 5,
    TD5_DISPATCH_QUAD_DIRECT     = 6
} TD5_MeshDispatch;

/** Frontend screen indices */
typedef enum TD5_ScreenIndex {
    TD5_SCREEN_LOCALIZATION_INIT   = 0,
    TD5_SCREEN_POSITIONER_DEBUG    = 1,
    TD5_SCREEN_ATTRACT_MODE        = 2,
    TD5_SCREEN_LANGUAGE_SELECT     = 3,
    TD5_SCREEN_LEGAL_COPYRIGHT     = 4,
    TD5_SCREEN_MAIN_MENU           = 5,
    TD5_SCREEN_RACE_TYPE_MENU      = 6,
    TD5_SCREEN_QUICK_RACE          = 7,
    TD5_SCREEN_CONNECTION_BROWSER  = 8,
    TD5_SCREEN_SESSION_PICKER      = 9,
    TD5_SCREEN_CREATE_SESSION      = 10,
    TD5_SCREEN_NETWORK_LOBBY       = 11,
    TD5_SCREEN_OPTIONS_HUB         = 12,
    TD5_SCREEN_GAME_OPTIONS        = 13,
    TD5_SCREEN_CONTROL_OPTIONS     = 14,
    TD5_SCREEN_SOUND_OPTIONS       = 15,
    TD5_SCREEN_DISPLAY_OPTIONS     = 16,
    TD5_SCREEN_TWO_PLAYER_OPTIONS  = 17,
    TD5_SCREEN_CONTROLLER_BINDING  = 18,
    TD5_SCREEN_MUSIC_TEST          = 19,
    TD5_SCREEN_CAR_SELECTION       = 20,
    TD5_SCREEN_TRACK_SELECTION     = 21,
    TD5_SCREEN_EXTRAS_GALLERY      = 22,
    TD5_SCREEN_HIGH_SCORE          = 23,
    TD5_SCREEN_RACE_RESULTS        = 24,
    TD5_SCREEN_NAME_ENTRY          = 25,
    TD5_SCREEN_CUP_FAILED          = 26,
    TD5_SCREEN_CUP_WON             = 27,
    TD5_SCREEN_STARTUP_INIT        = 28,
    TD5_SCREEN_SESSION_LOCKED      = 29,
    TD5_SCREEN_COUNT               = 30
} TD5_ScreenIndex;

/* ========================================================================
 * Vector / Matrix Types
 * ======================================================================== */

/** 3-component vector, 24.8 fixed-point */
typedef struct TD5_Vec3i {
    int32_t x, y, z;
} TD5_Vec3i;

/** 3-component float vector */
typedef struct TD5_Vec3f {
    float x, y, z;
} TD5_Vec3f;

/** 3-component int16 vector (compact directional data) */
typedef struct TD5_Vec3s {
    int16_t x, y, z;
} TD5_Vec3s;

/** 3x3 rotation matrix, row-major float */
typedef struct TD5_Mat3x3 {
    float m[9];
} TD5_Mat3x3;

/** 3x4 render transform (3x3 rotation + translation) */
typedef struct TD5_Mat3x4 {
    float m[12]; /* [0..8] = 3x3 rotation, [9..11] = translation */
} TD5_Mat3x4;

/* ========================================================================
 * Track Data Structures
 * ======================================================================== */

/** STRIP.DAT span record (24 bytes) */
#pragma pack(push, 1)
typedef struct TD5_StripSpan {
    uint8_t  span_type;             /* +0x00 */
    uint8_t  surface_attribute;     /* +0x01 */
    uint8_t  pad_02[2];            /* +0x02 */
    uint16_t left_vertex_index;     /* +0x04 */
    uint16_t right_vertex_index;    /* +0x06 */
    int16_t  link_next;             /* +0x08: forward link / junction */
    int16_t  link_prev;             /* +0x0A: backward link */
    int32_t  origin_x;              /* +0x0C */
    int32_t  origin_y;              /* +0x10 */
    int32_t  origin_z;              /* +0x14 */
} TD5_StripSpan;
#pragma pack(pop)

/** STRIP.DAT vertex (6 bytes: 3 x int16) */
#pragma pack(push, 1)
typedef struct TD5_StripVertex {
    int16_t x, y, z;
} TD5_StripVertex;
#pragma pack(pop)

/** Track probe state (16 bytes, per-wheel or per-body-corner) */
typedef struct TD5_TrackProbe {
    int16_t  span_index;
    int16_t  span_normalized;
    int16_t  span_accumulated;
    int16_t  span_high_water;
    int16_t  contact_vertex_a;
    int16_t  contact_vertex_b;
    int8_t   sub_lane_index;
    uint8_t  _pad_0d;
    int16_t  _pad_0e;
} TD5_TrackProbe;

/* ========================================================================
 * Mesh Resource Structures (PRR format)
 * ======================================================================== */

/** Mesh resource header (>= 0x38 bytes) */
typedef struct TD5_MeshHeader {
    int16_t  render_type;
    int16_t  texture_page_id;
    int32_t  command_count;
    int32_t  total_vertex_count;
    float    bounding_radius;
    float    bounding_center_x;
    float    bounding_center_y;
    float    bounding_center_z;
    float    origin_x;
    float    origin_y;
    float    origin_z;
    uint32_t reserved_28;
    uint32_t commands_offset;       /* relocated to pointer */
    uint32_t vertices_offset;       /* relocated to pointer */
    uint32_t normals_offset;        /* relocated to pointer */
} TD5_MeshHeader;

/** Primitive command (16 bytes) */
typedef struct TD5_PrimitiveCmd {
    int16_t  dispatch_type;
    int16_t  texture_page_id;
    int32_t  reserved_04;
    uint16_t triangle_count;
    uint16_t quad_count;
    uint32_t vertex_data_ptr;
} TD5_PrimitiveCmd;

/** Mesh vertex (44 bytes, 0x2C stride) */
typedef struct TD5_MeshVertex {
    float    pos_x, pos_y, pos_z;           /* model-space */
    float    view_x, view_y, view_z;        /* view-space (runtime) */
    uint32_t lighting;                       /* intensity byte in low 8 bits */
    float    tex_u, tex_v;                   /* primary UV */
    float    proj_u, proj_v;                /* secondary/projection UV */
} TD5_MeshVertex;

/** Vertex normal (16 bytes) */
typedef struct TD5_VertexNormal {
    float    nx, ny, nz;
    int32_t  visible_flag;
} TD5_VertexNormal;

/* ========================================================================
 * Vehicle Tuning Data
 * ======================================================================== */

/** Vehicle tuning parameter block (pointed to by actor+0x1BC) */
typedef struct TD5_VehicleTuning {
    uint8_t  _pad_00[0x20];
    int32_t  inertia;               /* +0x20 */
    int32_t  suspension_half_travel;/* +0x24 */
    int16_t  front_weight;          /* +0x28 */
    int16_t  rear_weight;           /* +0x2A */
    int16_t  total_weight;          /* +0x2C */
    int16_t  gear_ratios[8];        /* +0x2E: gear 0 (reverse) through gear 7 */
    int16_t  torque_curve[16];      /* +0x3E: sampled every 512 RPM units */
    int16_t  upshift_rpm[8];        /* +0x5E: per-gear upshift threshold */
    int16_t  downshift_rpm[8];      /* +0x6E: per-gear downshift threshold */
    int16_t  spring_rate;           /* +0x7E: suspension stiffness */
    int16_t  damping_rate;          /* +0x80: velocity damping */
    int16_t  damping_rate2;         /* +0x82: position-proportional damping */
    int16_t  travel_limit;          /* +0x84: max deflection */
    int16_t  contact_force_k;       /* +0x86: contact force multiplier */
    int16_t  steering_sensitivity;  /* +0x88 */
    int16_t  high_speed_drag;       /* +0x8A */
    int16_t  low_speed_drag;        /* +0x8C */
    int16_t  brake_front;           /* +0x8E */
    int16_t  brake_rear;            /* +0x90 */
    int16_t  engine_redline;        /* +0x92 */
    int16_t  speed_limiter;         /* +0x94 */
    uint8_t  drivetrain_type;       /* +0x96: 1=FWD, 2=RWD, 3=AWD */
    uint8_t  _pad_97;
    int16_t  brake_balance;         /* +0x98 */
    int16_t  rear_grip_modifier;    /* +0x9A: handbrake grip scale */
    int16_t  tire_slip_threshold;   /* +0x9C */
    uint8_t  _pad_9e[4];
    int16_t  suspension_rest_height;/* +0xA2 */
    uint8_t  _pad_a4[4];
    int16_t  traffic_stiffness;     /* +0xA8: traffic suspension override */
} TD5_VehicleTuning;

/* ========================================================================
 * Actor Struct (0x388 bytes)
 *
 * Full layout defined in re/include/td5_actor_struct.h.
 * This is a forward declaration for module APIs. The complete struct
 * is too large to duplicate here; modules that need field-level access
 * should include td5_actor_struct.h from re/include/.
 * ======================================================================== */

typedef struct TD5_Actor TD5_Actor;

/* ========================================================================
 * Save Data Structures
 * ======================================================================== */

/** Game options block (7 dwords, from Config.td5 offset 0x04) */
typedef struct TD5_GameOptions {
    int32_t circuit_laps;           /* 0-3 -> 2/4/6/8 laps */
    int32_t checkpoint_timers;      /* 0=Off, 1=On */
    int32_t traffic;                /* 0=Off, 1=On */
    int32_t cops;                   /* 0=Off, 1=On */
    int32_t difficulty;             /* 0=Easy, 1=Normal, 2=Hard */
    int32_t dynamics;               /* 0=Arcade, 1=Simulation */
    int32_t collisions_3d;          /* 0=Off, 1=On */
} TD5_GameOptions;

/** NPC high-score entry (32 bytes)
 *  Field layout verified against binary defaults at 0x4643B8. */
#pragma pack(push, 1)
typedef struct TD5_NpcEntry {
    char     name[13];              /* player name (null-terminated) */
    uint8_t  _pad[3];
    int32_t  score;                 /* race time in game ticks (30fps) for types 0/1/4, points for type 2 */
    int32_t  car_id;                /* car identifier (low byte used for name lookup) */
    int32_t  avg_speed;             /* average speed in raw internal units */
    int32_t  top_speed;             /* top speed in raw internal units */
} TD5_NpcEntry;
#pragma pack(pop)

/** NPC high-score group (164 bytes) — one per track, 26 total.
 *  Header byte selects score column type:
 *    0 = "TIME"  (MM:SS.cc)     1 = "LAP" (MM:SS.cc)
 *    2 = "PTS"   (integer)      4 = "TIME" (MM:SS.mmm) */
typedef struct TD5_NpcGroup {
    int32_t      header;
    TD5_NpcEntry entries[5];
} TD5_NpcGroup;

/* ========================================================================
 * Network Protocol Structures
 * ======================================================================== */

/** DXPFRAME message (128 bytes on wire) */
typedef struct TD5_NetFrame {
    uint32_t msg_type;              /* always 0 */
    uint32_t control_bits[6];       /* per-slot input bitmask */
    float    frame_delta_time;      /* host's normalized dt */
    uint32_t sync_sequence;         /* monotonic frame counter */
    uint8_t  _reserved[92];
} TD5_NetFrame;

/** DXPROSTER message (52 bytes) */
typedef struct TD5_NetRoster {
    uint32_t msg_type;              /* always 8 */
    uint32_t player_ids[6];
    uint32_t active_flags[6];
    uint32_t host_id;
} TD5_NetRoster;

/* ========================================================================
 * Utility: Integer square root (for tire slip circle)
 * ======================================================================== */

static inline int32_t td5_isqrt(int32_t x) {
    if (x <= 0) return 0;
    int32_t r = 0, b = 0x40000000;
    while (b > 0) {
        if (x >= r + b) {
            x -= r + b;
            r = (r >> 1) + b;
        } else {
            r >>= 1;
        }
        b >>= 2;
    }
    return r;
}

/* ========================================================================
 * CRC-32 (standard polynomial, used by save system)
 * ======================================================================== */

/** Compute CRC-32 over a buffer. Init=0xFFFFFFFF, final XOR=0xFFFFFFFF. */
uint32_t td5_crc32(const uint8_t *data, size_t len);

#endif /* TD5_TYPES_H */
