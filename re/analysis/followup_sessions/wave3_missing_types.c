/*
 * wave3_missing_types.c -- Port-side type definitions for Ghidra type_define_c
 *
 * Purpose: Provide the C-source for 16 port-only types that the Wave 2D
 *          (signature push) agent could not apply because Ghidra's type
 *          archive does not yet know them.
 *
 * Output target: feed each typedef separately through ghidra.type_define_c
 *                (or batch via type_parse_c). Order below is dependency-safe.
 *
 * Sources (kept in comments above each definition):
 *   td5mod/src/td5re/td5_types.h
 *   td5mod/src/td5re/td5_asset.h
 *   td5mod/src/td5re/td5_hud.h
 *   td5mod/src/td5re/td5_render.h
 *   td5mod/src/td5re/td5_platform.h
 *   td5mod/src/td5re/td5_platform_win32.c   (opaque struct body)
 *   td5mod/src/td5re/td5_physics.c          (OBB_CornerData)
 *   td5mod/src/td5re/td5_light_zones_table.inc
 *   re/include/td5_actor_struct.h           (TD5_TrackProbeState, TD5_Vec3_Fixed)
 *
 * IMPORTANT:
 *   - Definitions stripped of designated initializers / constexpr.
 *   - Plain C89 style for Ghidra parser.
 *   - <stdint.h> primitives written as the explicit signed/unsigned width
 *     identifiers Ghidra accepts (int8_t, int16_t, int32_t, uint8_t, ...).
 *   - TD5_CarDef: NOT FOUND in port source (Wave 2D agent error suspected
 *     -- name surfaced in spec but never referenced in apply-plan or
 *     worklist). Reported in summary; no definition emitted below.
 */

#include <stdint.h>

/* =========================================================================
 * Section A -- Foundational vector / matrix types (no dependencies)
 * ========================================================================= */

/* -- TD5_Vec3f -----------------------------------------------------------
 * Source: td5mod/src/td5re/td5_types.h:355
 * 3-component float vector
 */
typedef struct TD5_Vec3f {
    float x;
    float y;
    float z;
} TD5_Vec3f;

/* -- TD5_Mat3x3 ----------------------------------------------------------
 * Source: td5mod/src/td5re/td5_types.h:365
 * 3x3 rotation matrix, row-major float
 */
typedef struct TD5_Mat3x3 {
    float m[9];
} TD5_Mat3x3;

/* =========================================================================
 * Section B -- Enums (no struct dependencies)
 * ========================================================================= */

/* -- TD5_ScreenIndex ----------------------------------------------------
 * Source: td5mod/src/td5re/td5_types.h:311-343
 * Frontend screen indices (30 entries).
 */
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

/* -- TD5_RaceRenderPass ------------------------------------------------
 * Source: td5mod/src/td5re/td5_render.h:171-176
 * Per-race render pass identifier.
 */
typedef enum TD5_RaceRenderPass {
    TD5_RACE_PASS_SKY     = 0,
    TD5_RACE_PASS_OPAQUE  = 1,
    TD5_RACE_PASS_UNUSED  = 2,
    TD5_RACE_PASS_ALPHA   = 3
} TD5_RaceRenderPass;

/* =========================================================================
 * Section C -- Asset / archive types (no inter-dependencies)
 * ========================================================================= */

/* -- TD5_ZipEntry --------------------------------------------------------
 * Source: td5mod/src/td5re/td5_asset.h:71-78
 * Parsed central-directory entry from a ZIP archive.
 * Uses TD5_ZIP_MAX_FILENAME = 260.
 */
typedef struct TD5_ZipEntry {
    char     name[260];                 /* TD5_ZIP_MAX_FILENAME */
    uint32_t compressed_size;
    uint32_t uncompressed_size;
    uint32_t local_header_offset;
    uint16_t compression_method;        /* 0 = stored, 8 = deflate */
    uint32_t crc32;
} TD5_ZipEntry;

/* -- TD5_StaticHedEntry --------------------------------------------------
 * Source: td5mod/src/td5re/td5_asset.h:146-153
 * Named texture entry from static.hed (0x40 = 64 bytes).
 */
typedef struct TD5_StaticHedEntry {
    char     name[44];                  /* +0x00: null-terminated ASCII */
    int32_t  pos_x;                     /* +0x2C */
    int32_t  pos_y;                     /* +0x30 */
    int32_t  width;                     /* +0x34 */
    int32_t  height;                    /* +0x38 */
    int32_t  texture_slot;              /* +0x3C: biased by +0x400 at load time */
} TD5_StaticHedEntry;

/* -- TD5_AtlasEntry ------------------------------------------------------
 * Source: td5mod/src/td5re/td5_hud.h:92-98
 * Atlas entry returned by archive lookup.
 */
typedef struct TD5_AtlasEntry {
    int32_t atlas_x;                    /* +0x2C: atlas X origin */
    int32_t atlas_y;                    /* +0x30: atlas Y origin */
    int32_t width;                      /* +0x34: atlas width */
    int32_t height;                     /* +0x38: atlas height */
    int32_t texture_page;               /* +0x3C: texture page index */
} TD5_AtlasEntry;

/* -- TD5_File ------------------------------------------------------------
 * Source: td5mod/src/td5re/td5_platform.h:74 (forward decl)
 *         td5mod/src/td5re/td5_platform_win32.c:697-699 (concrete body)
 * Opaque file handle (POSIX FILE* wrapper).
 * Ghidra-side this is effectively an opaque pointer-target struct.
 */
typedef struct TD5_File {
    void *fp;                           /* FILE* in the port */
} TD5_File;

/* =========================================================================
 * Section D -- Mesh / render command types
 * ========================================================================= */

/* -- TD5_MeshHeader ------------------------------------------------------
 * Source: td5mod/src/td5re/td5_types.h:419-435
 * Mesh resource header (>= 0x38 bytes).
 */
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
    uint32_t commands_offset;           /* relocated to pointer */
    uint32_t vertices_offset;           /* relocated to pointer */
    uint32_t normals_offset;            /* relocated to pointer */
} TD5_MeshHeader;

/* -- TD5_PrimitiveCmd ----------------------------------------------------
 * Source: td5mod/src/td5re/td5_types.h:438-445
 * Primitive command (16 bytes).
 */
typedef struct TD5_PrimitiveCmd {
    int16_t  dispatch_type;
    int16_t  texture_page_id;
    int32_t  reserved_04;
    uint16_t triangle_count;
    uint16_t quad_count;
    uint32_t vertex_data_ptr;
} TD5_PrimitiveCmd;

/* -- TD5_MeshVertex ------------------------------------------------------
 * Source: td5mod/src/td5re/td5_types.h:448-454
 * Mesh vertex (44 bytes, 0x2C stride).
 */
typedef struct TD5_MeshVertex {
    float    pos_x;
    float    pos_y;
    float    pos_z;                     /* model-space */
    float    view_x;
    float    view_y;
    float    view_z;                    /* view-space (runtime) */
    uint32_t lighting;                  /* intensity byte in low 8 bits */
    float    tex_u;
    float    tex_v;                     /* primary UV */
    float    proj_u;
    float    proj_v;                    /* secondary/projection UV */
} TD5_MeshVertex;

/* =========================================================================
 * Section E -- Track / lighting / collision types
 * ========================================================================= */

/* -- TD5_StripSpan -------------------------------------------------------
 * Source: td5mod/src/td5re/td5_types.h:380-391
 * STRIP.DAT span record (24 bytes, packed).
 * Use Ghidra struct alignment 1 (#pragma pack(1) equivalent) when defining.
 */
#pragma pack(push, 1)
typedef struct TD5_StripSpan {
    uint8_t  span_type;                 /* +0x00 */
    uint8_t  surface_attribute;         /* +0x01 */
    uint8_t  pad_02[2];                 /* +0x02 */
    uint16_t left_vertex_index;         /* +0x04 */
    uint16_t right_vertex_index;        /* +0x06 */
    int16_t  link_next;                 /* +0x08: forward link / junction */
    int16_t  link_prev;                 /* +0x0A: backward link */
    int32_t  origin_x;                  /* +0x0C */
    int32_t  origin_y;                  /* +0x10 */
    int32_t  origin_z;                  /* +0x14 */
} TD5_StripSpan;
#pragma pack(pop)

/* -- TD5_TrackProbeState -------------------------------------------------
 * Source: re/include/td5_actor_struct.h:186-196
 * Per-probe track-position state (16 bytes). Identical layout to the
 * truncated TD5_TrackProbe in td5_types.h:402-412 but with explicit names
 * matching call sites in td5_track.c.
 */
typedef struct TD5_TrackProbeState {
    int16_t  span_index;                /* +0x00: current STRIP.DAT span index */
    int16_t  span_normalized;           /* +0x02: span modulo ring length */
    int16_t  span_accumulated;          /* +0x04: monotonic forward span counter */
    int16_t  span_high_water;           /* +0x06: high-water mark for ordering */
    int16_t  contact_vertex_A;          /* +0x08: left-edge track vertex index */
    int16_t  contact_vertex_B;          /* +0x0A: right-edge track vertex index */
    int8_t   sub_lane_index;            /* +0x0C: sub-lane within span */
    uint8_t  _pad_0D;                   /* +0x0D: padding */
    int16_t  _pad_0E;                   /* +0x0E: padding/unused */
} TD5_TrackProbeState;

/* -- TD5_LightZone -------------------------------------------------------
 * Source: td5mod/src/td5re/td5_light_zones_table.inc:11-25
 * Per-track lighting zone (extracted from orig 0x00469c78).
 * Original record was anonymous-typedef; we name it TD5_LightZone.
 */
typedef struct TD5_LightZone {
    int16_t  span_lo;
    int16_t  span_hi;
    int16_t  dir_x;
    int16_t  dir_y;
    int16_t  dir_z;                     /* directional light vector (world frame) */
    uint8_t  weight_r;
    uint8_t  weight_g;
    uint8_t  weight_b;                  /* per-channel directional weight */
    uint8_t  amb_r;
    uint8_t  amb_g;
    uint8_t  amb_b;                     /* per-channel ambient */
    int16_t  pos_off_x;
    int16_t  pos_off_y;                 /* track-vertex sample offset (cases 1/2) */
    uint8_t  blend_mode;                /* 0=static, 1=transition, 2=multi-sample, 3=half-blend */
    uint8_t  spacing;                   /* track-vertex stride for sampling */
    uint8_t  sub_mode;                  /* case-2 vertex pick: 0=normal,1=alt edge,2=midpoint */
    uint8_t  multiplier;                /* angle dampening multiplier (cases 1/2) */
    uint32_t state_key;                 /* state-machine trigger; & 0xFF = environs light_index */
    uint32_t slot_color;                /* DAT_004c38a0[slot] entry (currently unused in port) */
    int32_t  light_index;               /* convenience alias = state_key & 0xFF (0..3) */
} TD5_LightZone;

/* -- OBB_CornerData ------------------------------------------------------
 * Source: td5mod/src/td5re/td5_physics.c:105-112
 * OBB corner test output: per-corner penetration data (12 bytes).
 * Used by obb_corner_test() and apply_collision_response().
 */
typedef struct OBB_CornerData {
    int16_t  proj_x;                    /* corner position in TARGET's local frame (X) */
    int16_t  proj_z;                    /* corner position in TARGET's local frame (Z) */
    int16_t  pen_x;                     /* penetration depth along X axis */
    int16_t  pen_z;                     /* penetration depth along Z axis */
    int16_t  own_x;                     /* rotated corner offset from penetrator's center, in TARGET's frame (X) */
    int16_t  own_z;                     /* rotated corner offset from penetrator's center, in TARGET's frame (Z) */
} OBB_CornerData;

/* =========================================================================
 * Section F -- Types listed in spec but NOT FOUND in port source
 * =========================================================================
 *
 * TD5_CarDef
 *   No `typedef struct TD5_CarDef` or equivalent exists anywhere in the
 *   project tree (td5mod/, re/, tools/). The port refers to vehicle
 *   tuning via TD5_VehicleTuning (td5_types.h:467) and to raw cardef
 *   bytes via uint8_t* / int16_t* casts (see td5_physics.c). The 16-name
 *   spec list likely conflated "CarDef" (a generic term) with the actual
 *   typed struct TD5_VehicleTuning.
 *
 *   ACTION FOR WAVE 2D RESCUE: none of the 24 skipped signatures
 *   referenced TD5_CarDef in their port-side type list, so dropping it
 *   does not lose any signature. If a later wave produces a TD5_CarDef-
 *   typed signature, fall back to TD5_VehicleTuning or extract it then.
 */
