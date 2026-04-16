/**
 * td5_level_formats.h -- Track and level data format structures for Test Drive 5
 *
 * Covers all binary formats loaded from level%03d.zip and companion archives:
 *   STRIP.DAT   -- Track geometry (span records + vertex pool)
 *   LEVELINF.DAT -- Environment configuration (weather, fog, checkpoints)
 *   CHECKPT.NUM  -- Checkpoint remapping matrix (forward/reverse swap pairs)
 *   TRAFFIC.BUS  -- Traffic vehicle spawn table
 *   static.hed   -- Texture page directory (named entries + per-page metadata)
 *   TEXTURES.DAT -- Palettized 64x64 track texture definitions
 *
 * Lighting entries and checkpoint timing metadata are hardcoded in the EXE,
 * not loaded from ZIP files, but are included here for completeness.
 *
 * Derived from reverse engineering of TD5_d3d.exe (864 named functions).
 * Verified against Ghidra decompilations of:
 *   BindTrackStripRuntimePointers (0x444070)
 *   LoadTrackRuntimeData          (0x42FB90)
 *   UpdateActorTrackPosition      (0x4440F0)
 *   InitializeTrackStripMetadata  (0x42FAD0)
 *   ApplyTrackStripAttributeOverrides (0x42FB40)
 *
 * See re/analysis/ai-routing-and-track-geometry.md for full system docs.
 * See re/analysis/level-zip-file-formats.md for file inventory.
 * See re/analysis/levelinf-dat-format.md for LEVELINF field details.
 */

#ifndef TD5_LEVEL_FORMATS_H
#define TD5_LEVEL_FORMATS_H

#include <stddef.h>
#include <stdint.h>

#pragma pack(push, 1)  /* exact binary layout required -- no compiler padding */

/* ========================================================================
 * Constants
 * ======================================================================== */

/* --- STRIP.DAT --- */
#define TD5_STRIP_SPAN_STRIDE       0x18    /* 24 bytes per span record */
#define TD5_STRIP_VERTEX_STRIDE     6       /* 3 x int16 (X, Y, Z) per vertex */
#define TD5_STRIP_HEADER_DWORDS     5       /* 5-dword file header */

/* --- Span Type Constants (byte at span +0x00) ---
 *
 * The type controls triangle subdivision topology and neighbor traversal.
 * Dispatch tables at DAT_00474e40/41 (vertex offsets) and DAT_00474e28/29
 * (edge masks) are indexed by these values.
 */
#define TD5_SPAN_TYPE_INVALID       0   /* not used in valid data */
#define TD5_SPAN_TYPE_QUAD_A        1   /* standard quad: left+right, diagonal split */
#define TD5_SPAN_TYPE_QUAD_B        2   /* standard quad variant */
#define TD5_SPAN_TYPE_TRANSITION_A  3   /* alternate diagonal: shifted vertex pairs */
#define TD5_SPAN_TYPE_TRANSITION_B  4   /* alternate diagonal variant */
#define TD5_SPAN_TYPE_QUAD_C        5   /* standard quad third variant */
#define TD5_SPAN_TYPE_REVERSED_A    6   /* reversed winding: swapped left/right refs */
#define TD5_SPAN_TYPE_REVERSED_B    7   /* reversed winding variant */
#define TD5_SPAN_TYPE_JUNCTION_FWD  8   /* forward junction: T-junction via +0x08 link */
#define TD5_SPAN_TYPE_SENTINEL_START 9  /* backward sentinel: patched at load (first span) */
#define TD5_SPAN_TYPE_SENTINEL_END  10  /* forward sentinel: patched at load (last span) */
#define TD5_SPAN_TYPE_JUNCTION_BWD  11  /* backward junction: link via +0x0A */
#define TD5_SPAN_TYPE_COUNT         12  /* total span types */

/* --- Track Surface Types (strip attribute byte at span +0x01) ---
 *
 * Set by ApplyTrackStripAttributeOverrides (0x42FB40) from per-track
 * override tables at PTR_DAT_0046cb10. Consumed by suspension, tire
 * grip, and audio systems.
 */
typedef enum TD5_SurfaceType {
    TD5_SURFACE_DEFAULT     = 0,    /* default surface */
    TD5_SURFACE_DRY_ASPHALT = 1,    /* dry asphalt -- full grip */
    TD5_SURFACE_WET_ASPHALT = 2,    /* wet asphalt -- reduced grip */
    TD5_SURFACE_DIRT        = 3,    /* dirt/offroad -- low grip */
    TD5_SURFACE_GRAVEL      = 4,    /* gravel -- very low grip */
} TD5_SurfaceType;

/* --- Weather Types (LEVELINF.DAT +0x28) --- */
typedef enum TD5_WeatherType {
    TD5_WEATHER_RAIN  = 0,  /* rain: full particle system + audio */
    TD5_WEATHER_SNOW  = 1,  /* snow: CUT FEATURE -- init only, no render */
    TD5_WEATHER_CLEAR = 2,  /* clear: no particles, no audio */
} TD5_WeatherType;

/* --- Track Layout Types (LEVELINF.DAT +0x00) --- */
typedef enum TD5_TrackType {
    TD5_TRACK_CIRCUIT       = 0,    /* circuit: lapped, checkpoint as finish span */
    TD5_TRACK_POINT_TO_POINT = 1,   /* point-to-point: linear, final checkpoint = finish */
} TD5_TrackType;

/* --- Texture Format Types (TEXTURES.DAT per-texture +0x03) --- */
typedef enum TD5_TextureFormat {
    TD5_TEXFMT_OPAQUE       = 0,    /* opaque: A=0x00, standard RGB */
    TD5_TEXFMT_COLORKEY     = 1,    /* color-keyed: palette index 0 = transparent */
    TD5_TEXFMT_SEMITRANS    = 2,    /* semi-transparent: A=0x80 (50% alpha) */
    TD5_TEXFMT_OPAQUE_ALT   = 3,    /* opaque alternate: treated as type 0 */
} TD5_TextureFormat;

/* --- Limits --- */
#define TD5_MAX_WEATHER_ZONES       6   /* max density zone pairs in LEVELINF.DAT */
#define TD5_MAX_CHECKPOINTS         7   /* max checkpoint stages in LEVELINF.DAT */
#define TD5_CHECKPOINT_MATRIX_ROWS  4   /* CHECKPT.NUM: 4 rows */
#define TD5_CHECKPOINT_MATRIX_COLS  6   /* CHECKPT.NUM: 6 columns */
#define TD5_MAX_TEXTURE_CACHE_SLOTS 600 /* texture streaming cache capacity */
#define TD5_TRACK_TEXTURE_DIM       64  /* all TEXTURES.DAT textures are 64x64 */
#define TD5_MAX_WEATHER_PARTICLES   128 /* max active particles per view (0x80) */
#define TD5_TRAFFIC_BUS_SENTINEL    (-1) /* end-of-list marker in TRAFFIC.BUS */
#define TD5_LIGHTING_ENTRY_STRIDE   0x24 /* 36 bytes per lighting zone entry */
#define TD5_STATIC_HED_ENTRY_SIZE   0x40 /* 64 bytes per named texture entry */
#define TD5_TPAGE_META_SIZE         0x10 /* 16 bytes per page metadata entry */

/* --- NPC Racer Group Table ---                                     NEW wave3 */
#define TD5_NPC_GROUP_COUNT         26  /* 26 groups at 0x4643B8 */
#define TD5_NPC_GROUP_STRIDE        0xA4 /* 164 bytes per group */
#define TD5_ADDR_NPC_GROUP_TABLE    0x004643B8  /* NpcRacerGroup[26] */
#define TD5_NPC_HIGHSCORE_ENTRIES   5   /* 5 high-score slots per group */

/* --- Camera Preset Table ---                                       NEW wave3 */
#define TD5_CAMERA_PRESET_STRIDE    0x10 /* 16 bytes per preset */

/* --- Runtime Global Addresses --- */
#define TD5_ADDR_STRIP_BLOB         0x004AED90  /* int*: raw STRIP.DAT allocation */
#define TD5_ADDR_STRIP_HEADER       0x004C3DA0  /* int*: parsed header pointer */
#define TD5_ADDR_SPAN_TABLE         0x004C3D9C  /* byte*: span record table */
#define TD5_ADDR_VERTEX_TABLE       0x004C3D98  /* short*: vertex coordinate table */
#define TD5_ADDR_SPAN_COUNT         0x004C3D90  /* int: total span count (ring length) */
#define TD5_ADDR_AUX_COUNT          0x004C3D94  /* int: auxiliary span count */
#define TD5_ADDR_SECONDARY_COUNT    0x004C3D8C  /* int: secondary count */
#define TD5_ADDR_LEVELINF           0x004AEE20  /* char*: gTrackEnvironmentConfig */
#define TD5_ADDR_CHECKPOINT_BUF     0x004AEDB0  /* int[24]: CHECKPT.NUM static buffer */
#define TD5_ADDR_CHECKPOINT_META    0x004AED88  /* ptr: checkpoint metadata pointer */
#define TD5_ADDR_TRAFFIC_BUS        0x004AED8C  /* char*: TRAFFIC.BUS heap pointer */
#define TD5_ADDR_TRAFFIC_CURSOR     0x004B08B8  /* short*: TRAFFIC.BUS read cursor */
#define TD5_ADDR_LIGHTING_TABLE     0x004AEE14  /* void*: per-track lighting entries */
#define TD5_ADDR_STRIP_METADATA     0x004AEE10  /* int*: strip metadata block pointer */

/* ========================================================================
 * Forward declaration: TD5_WeatherDensityZone (used by TD5_LevelInf)
 * ======================================================================== */

typedef struct TD5_WeatherDensityZone {
    int16_t segment_id;     /* +0x00: track span index where density changes */
    int16_t density;        /* +0x02: target active particle count (clamped to 128 max) */
} TD5_WeatherDensityZone;

/* ========================================================================
 * 1. TD5_StripVertex -- Track vertex (6 bytes)
 *
 * Relative X, Y, Z offsets from the parent span's world origin.
 * To get world coordinates:
 *   world.x = (origin.x + vertex.x) * 256   (24.8 fixed-point)
 * ======================================================================== */

typedef struct TD5_StripVertex {
    int16_t x;      /* +0x00: relative X offset from span origin */
    int16_t y;      /* +0x02: relative Y offset (vertical) */
    int16_t z;      /* +0x04: relative Z offset from span origin */
} TD5_StripVertex;

/* ========================================================================
 * 2. TD5_StripSpan -- Span record from STRIP.DAT (24 bytes = 0x18)
 *
 * Each span defines a trapezoidal road section via 4 corner vertices
 * (left pair + right pair). The span type controls triangle subdivision
 * topology and neighbor traversal.
 *
 * Verified against BindTrackStripRuntimePointers (0x444070):
 *   - Sentinel patching writes type byte at +0x00
 *   - Link field at +0x0A written as short (span_count - 1)
 *   - Last span at offset (count * 0x18 - 0x18), type at +0x00 = 10
 *   - Last span link at offset (count * 0x18 - 0x10) cleared to 0
 *
 * Verified against UpdateActorTrackPosition (0x4440F0):
 *   - Type byte at pbVar1[0] (span_base + 0x00)
 *   - Packed sub-span at pbVar1[3] (span_base + 0x03): low nibble = count
 *   - Left vertex index at *(uint16*)(pbVar1 + 4)
 *   - Right vertex index at *(uint16*)(pbVar1 + 6)
 *   - World origin X at *(int32*)(pbVar1 + 0x0C)
 *   - World origin Z at *(int32*)(pbVar1 + 0x14)
 *
 * Verified against ApplyTrackStripAttributeOverrides (0x42FB40):
 *   - Attribute byte written at (iVar4 + 1 + DAT_004c3d9c), stride 0x18
 * ======================================================================== */

typedef struct TD5_StripSpan {
    uint8_t  span_type;         /* +0x00: span type (0-11), see TD5_SPAN_TYPE_* constants.
                                 *        Patched at load: first span = 9, last span = 10. */
    uint8_t  surface_attr;      /* +0x01: strip surface attribute byte.
                                 *        Set by ApplyTrackStripAttributeOverrides.
                                 *        See TD5_SurfaceType enum. */
    uint8_t  reserved_02;       /* +0x02: padding / unused */
    uint8_t  packed_sub_span;   /* +0x03: low nibble = sub-span lane count (1-based),
                                 *        high nibble = height offset for neighbor transitions.
                                 *        Access: count = (packed & 0x0F),
                                 *                h_off = (packed >> 4). */
    uint16_t left_vertex_idx;   /* +0x04: index into vertex table for left edge base vertex.
                                 *        Consecutive indices (base + sub_span_index) give
                                 *        the left edge vertices for each sub-lane. */
    uint16_t right_vertex_idx;  /* +0x06: index into vertex table for right edge base vertex. */
    uint16_t forward_link;      /* +0x08: forward link span index (used by types 8, 10, 11).
                                 *        For junctions/sentinels, the next span in the chain. */
    uint16_t backward_link;     /* +0x0A: backward link span index (used by types 9, 11).
                                 *        Patched at load: first span gets (span_count - 1). */
    int32_t  origin_x;          /* +0x0C: world origin X (absolute integer coordinate).
                                 *        Vertex world X = (origin_x + vertex.x) << 8. */
    int32_t  origin_y;          /* +0x10: world origin Y (vertical). */
    int32_t  origin_z;          /* +0x14: world origin Z (absolute integer coordinate). */
} TD5_StripSpan;

_Static_assert(sizeof(TD5_StripVertex) == 0x06, "TD5_StripVertex must stay 6 bytes");
_Static_assert(sizeof(TD5_StripSpan) == TD5_STRIP_SPAN_STRIDE, "TD5_StripSpan size drifted");
_Static_assert(offsetof(TD5_StripSpan, span_type) == 0x00, "TD5_StripSpan.span_type offset drifted");
_Static_assert(offsetof(TD5_StripSpan, left_vertex_idx) == 0x04, "TD5_StripSpan.left_vertex_idx offset drifted");
_Static_assert(offsetof(TD5_StripSpan, right_vertex_idx) == 0x06, "TD5_StripSpan.right_vertex_idx offset drifted");
_Static_assert(offsetof(TD5_StripSpan, forward_link) == 0x08, "TD5_StripSpan.forward_link offset drifted");
_Static_assert(offsetof(TD5_StripSpan, backward_link) == 0x0A, "TD5_StripSpan.backward_link offset drifted");
_Static_assert(offsetof(TD5_StripSpan, origin_x) == 0x0C, "TD5_StripSpan.origin_x offset drifted");
_Static_assert(offsetof(TD5_StripSpan, origin_z) == 0x14, "TD5_StripSpan.origin_z offset drifted");

/* ========================================================================
 * 3. TD5_StripHeader -- STRIP.DAT file header (5 dwords = 20 bytes)
 *
 * The file begins with 5 uint32 values. All offsets are relative to the
 * start of the file (self-relative). After loading, BindTrackStripRuntimePointers
 * (0x444070) resolves them to absolute pointers:
 *
 *   DAT_004c3d9c = blob + blob[0]   (span record table)
 *   DAT_004c3d98 = blob + blob[2]   (vertex coordinate table)
 *   DAT_004c3d90 = blob[1]          (total span count)
 *   DAT_004c3d94 = blob[4]          (auxiliary count)
 *   DAT_004c3d8c = blob[3]          (secondary count)
 *
 * Verified against Ghidra decompilation of BindTrackStripRuntimePointers.
 * ======================================================================== */

typedef struct TD5_StripHeader {
    uint32_t span_table_offset;     /* [0]: byte offset from file start to span record array */
    uint32_t span_count;            /* [1]: total number of span records (ring length for circuits) */
    uint32_t vertex_table_offset;   /* [2]: byte offset from file start to vertex coordinate array */
    uint32_t secondary_count;       /* [3]: secondary span/segment count */
    uint32_t auxiliary_count;        /* [4]: auxiliary span/segment count */
    /* --- Jump table (optional, follows header) ---
     *
     * At header + 0x14: jump entry count (uint32)
     * At header + 0x18: array of 6-byte jump entries for segment boundary remapping.
     * Each entry: { uint16 start, uint16 end, int16 remap_offset }
     * Used by ResolveActorSegmentBoundary (0x443FF0) for branching paths.
     */
} TD5_StripHeader;

/* ========================================================================
 * 4. TD5_LevelInf -- LEVELINF.DAT environment configuration (100 bytes)
 *
 * Describes track environment: layout type, weather, fog, checkpoints,
 * and per-segment weather density zones.
 *
 * Loaded by LoadTrackRuntimeData into gTrackEnvironmentConfig (0x4AEE20).
 *
 * Verified field offsets against levelinf-dat-format.md analysis and
 * cross-referenced with InitializeRaceSession, InitializeWeatherOverlayParticles,
 * UpdateAmbientParticleDensityForSegment, and ConfigureRaceFogColorAndMode.
 * ======================================================================== */

typedef struct TD5_LevelInf {
    uint32_t track_type;            /* +0x00: 0=circuit, 1=point-to-point.
                                     *        See TD5_TrackType enum. */
    uint32_t smoke_enabled;         /* +0x04: 1=allocate tire smoke sprite pool, 0=disabled */
    uint32_t checkpoint_count;      /* +0x08: number of checkpoint stages (1-7) */
    uint32_t checkpoint_spans[TD5_MAX_CHECKPOINTS];
                                    /* +0x0C: span index for each checkpoint (zero-padded).
                                     *        7 dwords = 28 bytes, ends at +0x28. */
    uint32_t weather_type;          /* +0x28: see TD5_WeatherType enum.
                                     *        0=rain, 1=snow(CUT), 2=clear. */
    uint32_t density_pair_count;    /* +0x2C: number of weather density zone pairs (0-6) */
    uint32_t is_circuit;            /* +0x30: 1=circuit (lapped), 0=point-to-point.
                                     *        Separate from track_type; controls race features. */
    TD5_WeatherDensityZone density_pairs[TD5_MAX_WEATHER_ZONES];
                                    /* +0x34: weather density zone pairs.
                                     *        6 entries x 4 bytes = 24 bytes, ends at +0x4C. */
    uint8_t  padding_4C[8];         /* +0x4C: zero padding */
    uint32_t sky_animation_index;   /* +0x54: 36 for circuits, 0xFFFFFFFF for P2P.
                                     *        VESTIGIAL: no PC code reads this field. */
    uint32_t total_span_count;      /* +0x58: total spans in track (9999 sentinel for P2P) */
    uint32_t fog_enabled;           /* +0x5C: 0=no fog, nonzero=fog active */
    uint8_t  fog_color_r;           /* +0x60: fog red component (0-255) */
    uint8_t  fog_color_g;           /* +0x61: fog green component (0-255) */
    uint8_t  fog_color_b;           /* +0x62: fog blue component (0-255) */
    uint8_t  padding_63;            /* +0x63: zero padding to 0x64 boundary */
} TD5_LevelInf;

_Static_assert(sizeof(TD5_LevelInf) == 0x64, "TD5_LevelInf size drifted from 0x64");
_Static_assert(offsetof(TD5_LevelInf, track_type) == 0x00, "TD5_LevelInf.track_type offset drifted");
_Static_assert(offsetof(TD5_LevelInf, checkpoint_count) == 0x08, "TD5_LevelInf.checkpoint_count offset drifted");
_Static_assert(offsetof(TD5_LevelInf, checkpoint_spans) == 0x0C, "TD5_LevelInf.checkpoint_spans offset drifted");
_Static_assert(offsetof(TD5_LevelInf, weather_type) == 0x28, "TD5_LevelInf.weather_type offset drifted");
_Static_assert(offsetof(TD5_LevelInf, density_pairs) == 0x34, "TD5_LevelInf.density_pairs offset drifted");
_Static_assert(offsetof(TD5_LevelInf, total_span_count) == 0x58, "TD5_LevelInf.total_span_count offset drifted");
_Static_assert(offsetof(TD5_LevelInf, fog_enabled) == 0x5C, "TD5_LevelInf.fog_enabled offset drifted");

/* ========================================================================
 * 5. TD5_WeatherDensityZone -- defined above (forward declaration)
 *    See struct definition near the top of this file.
 * ======================================================================== */

/* ========================================================================
 * 6. TD5_CheckpointEntry -- Single checkpoint record (4 bytes)
 *
 * Part of the per-track checkpoint metadata stored in the EXE (NOT in ZIP).
 * Pointer table at VA 0x46CF6C, indexed by level ZIP number.
 * LoadTrackRuntimeData copies 24 bytes into DAT_004aed98.
 *
 * Difficulty scaling (AdjustCheckpointTimersByDifficulty, 0x40A530):
 *   Hard = 1.0x (raw values), Normal = 1.1x, Easy = 1.2x.
 * ======================================================================== */

typedef struct TD5_CheckpointEntry {
    uint16_t span_threshold;    /* +0x00: strip span index that triggers this checkpoint.
                                 *        For circuit tracks, first entry doubles as
                                 *        the finish-line span. */
    uint16_t time_bonus;        /* +0x02: timer ticks added when checkpoint reached.
                                 *        0 for the final checkpoint (finish line).
                                 *        Ticks are at 30fps; divide by 30 for seconds. */
} TD5_CheckpointEntry;

/* ========================================================================
 * 7. TD5_CheckpointMetadata -- Per-track checkpoint header + entries (24 bytes)
 *
 * Stored in EXE at addresses pointed to by table at 0x46CF6C.
 * Copied into static buffer DAT_004aed98 by LoadTrackRuntimeData.
 * ======================================================================== */

typedef struct TD5_CheckpointMetadata {
    uint8_t  checkpoint_count;      /* +0x00: number of checkpoint stages (1-5 typical) */
    uint8_t  padding_01;            /* +0x01: alignment padding */
    uint16_t initial_time;          /* +0x02: starting timer value (ticks at 30fps, Hard) */
    TD5_CheckpointEntry checkpoints[5];
                                    /* +0x04: up to 5 checkpoint entries (20 bytes).
                                     *        Last entry always has time_bonus = 0. */
} TD5_CheckpointMetadata;

_Static_assert(sizeof(TD5_CheckpointMetadata) == 24, "TD5_CheckpointMetadata size drifted");
_Static_assert(offsetof(TD5_CheckpointMetadata, initial_time) == 0x02, "TD5_CheckpointMetadata.initial_time offset drifted");
_Static_assert(offsetof(TD5_CheckpointMetadata, checkpoints) == 0x04, "TD5_CheckpointMetadata.checkpoints offset drifted");

/* ========================================================================
 * 8. TD5_CheckpointTable -- CHECKPT.NUM format (96 bytes = 4x6 int32 matrix)
 *
 * Contains span-index swap pairs for remapping track strip data when
 * running in reverse direction. Loaded directly into static buffer
 * at DAT_004aedb0 (0x4AEDB0), no heap allocation.
 *
 * Row-major layout, 4 rows x 6 columns.
 * Values: span indices (>= 0) or -1 (unused/skip).
 *
 * RemapCheckpointOrderForTrackDirection (0x42FD70) performs swaps:
 *   Mode A (gReverseTrackDirection == -1): swap Col0<->Col4, Col1<->Col3
 *   Mode B (otherwise): swap Col0<->Col5, Col1<->Col4, Col2<->Col3
 * ======================================================================== */

typedef struct TD5_CheckpointTable {
    int32_t entries[TD5_CHECKPOINT_MATRIX_ROWS][TD5_CHECKPOINT_MATRIX_COLS];
                                    /* 4 rows x 6 columns = 24 int32 = 96 bytes.
                                     *
                                     * Column stride = 16 bytes (4 dwords per column).
                                     * Row stride = 4 bytes (1 dword per row within a column).
                                     *
                                     * NOTE: This is stored as row-major in memory but the
                                     * remapping algorithm iterates rows (4 iterations) and
                                     * accesses columns at fixed offsets:
                                     *   Col0=[row+0x00], Col1=[row+0x10], ..., Col5=[row+0x50]
                                     */
} TD5_CheckpointTable;

_Static_assert(sizeof(TD5_CheckpointTable) == 0x60, "TD5_CheckpointTable size drifted from 0x60");

/* ========================================================================
 * 9. TD5_TrafficBusEntry -- TRAFFIC.BUS spawn record (4 bytes)
 *
 * Each record specifies a traffic vehicle spawn point along the track.
 * Records are consumed sequentially as a FIFO queue.
 *
 * File: 4-byte records, terminated by span_index == -1 (0xFFFF).
 * Runtime pointer: DAT_004aed8c (heap), cursor: DAT_004b08b8.
 *
 * Consumers:
 *   InitializeTrafficActorsFromQueue (0x435930) -- initial spawn
 *   RecycleTrafficActorFromQueue (0x435310)     -- per-frame respawn
 * ======================================================================== */

typedef struct TD5_TrafficBusEntry {
    int16_t  span_index;    /* +0x00: track span where this vehicle spawns.
                             *        -1 (0xFFFF) = end-of-list sentinel. */
    uint8_t  flags;         /* +0x02: bit 0 = direction polarity:
                             *          0 = same direction as player (forward),
                             *          1 = oncoming traffic (heading += 0x80000 = 180 deg). */
    uint8_t  lane;          /* +0x03: lane index (0-based) within the span.
                             *        If lane >= span lane count (from packed_sub_span & 0x0F),
                             *        vehicle is placed on the alternate route table. */
} TD5_TrafficBusEntry;

/* Compile-time size check: 4 bytes */
typedef char _TD5_TrafficBusEntry_size_check[sizeof(TD5_TrafficBusEntry) == 4 ? 1 : -1];

/* Traffic direction flag bits */
#define TD5_TRAFFIC_DIR_FORWARD     0x00    /* same direction as player */
#define TD5_TRAFFIC_DIR_ONCOMING    0x01    /* heading flipped 180 degrees */

/* ========================================================================
 * 10. TD5_TrackLightEntry -- Per-segment lighting zone (36 bytes = 0x24)
 *
 * Hardcoded in EXE, pointed to by DAT_004aee14 (set from table at 0x469C78).
 * One array per track, variable length.
 *
 * Consumed by ApplyTrackLightingForVehicleSegment (0x430150):
 *   walks entries to find the zone covering the current span, then
 *   dispatches on mode byte to select blend function.
 * ======================================================================== */

typedef struct TD5_TrackLightEntry {
    int16_t  span_start;            /* +0x00: first span where this lighting zone applies */
    int16_t  span_end;              /* +0x02: last span of this lighting zone */
    int16_t  light_dir_x;          /* +0x04: directional light vector X component */
    int16_t  light_dir_y;          /* +0x06: directional light vector Y component */
    int16_t  light_dir_z;          /* +0x08: directional light vector Z component */
    uint8_t  light_color_r;        /* +0x0A: light color red (0-255) */
    uint8_t  light_color_g;        /* +0x0B: light color green (0-255) */
    uint8_t  light_color_b;        /* +0x0C: light color blue (0-255) */
    uint8_t  ambient_r;            /* +0x0D: ambient color red */
    uint8_t  ambient_g;            /* +0x0E: ambient color green */
    uint8_t  ambient_b;            /* +0x0F: ambient color blue */
    uint32_t fog_color_override;   /* +0x10: fog color override (0xFFrrggbb or 0) */
    uint8_t  reserved_14[4];       /* +0x14: reserved / unused */
    uint8_t  light_mode;           /* +0x18: blend mode (0=instant, 1=interpolate, 2=multi-dir).
                                    *        Switch cases in ApplyTrackLightingForVehicleSegment. */
    uint8_t  blend_distance;       /* +0x19: interpolation distance for mode 1 blending */
    uint8_t  reserved_1A[2];       /* +0x1A: padding */
    uint32_t projection_effect;    /* +0x1C: parameter passed to ConfigureActorProjectionEffect.
                                    *        Controls environment map / projection effect. */
    uint32_t material_flag;        /* +0x20: stored in DAT_004c38a0[actor_index].
                                    *        Material/surface rendering flag. */
} TD5_TrackLightEntry;

_Static_assert(sizeof(TD5_TrackLightEntry) == 0x24, "TD5_TrackLightEntry size drifted from 0x24");

/* ========================================================================
 * 11. TD5_TextureCacheEntry -- Texture streaming cache entry (8 bytes)
 *
 * The track texture streaming system manages up to 600 texture slots.
 * Each slot tracks whether a texture is loaded and its D3D handle.
 *
 * Base: DAT_0048dc40, managed by:
 *   AdvanceTextureStreamingScheduler (0x40B830) -- LRU eviction
 *   BindRaceTexturePage (0x40B660) -- on-demand load + bind
 *   PreloadLevelTexturePages (0x40BAE0) -- init preload
 * ======================================================================== */

typedef struct TD5_TextureCacheEntry {
    uint32_t state;         /* +0x00: 0=empty, nonzero=loaded (D3D texture handle or flags) */
    uint32_t lru_stamp;     /* +0x04: last-used frame counter for LRU eviction */
} TD5_TextureCacheEntry;

/* ========================================================================
 * 12. TD5_StaticHedEntry -- static.hed named texture entry (64 bytes = 0x40)
 *
 * The texture page directory in static.hed. Each entry maps a name
 * (e.g., "CAR_0", "wheels", "SKY", "TRAF%d") to a texture page slot.
 *
 * Loaded by LoadStaticTrackTextureHeader (0x442560).
 * Searched by FindArchiveEntryByName.
 *
 * File layout:
 *   static.hed header: { int32 texture_page_count, int32 named_entry_count }
 *   followed by named_entry_count x TD5_StaticHedEntry
 *   followed by texture_page_count x TD5_TexturePageMeta
 * ======================================================================== */

typedef struct TD5_StaticHedEntry {
    char     name[60];          /* +0x00: null-terminated ASCII name.
                                 *        Known names: "CAR_0".."CAR_2", "wheels", "SKY",
                                 *        "TRAF%d", "ENV%d", "chassis", "RAINDROP",
                                 *        "SNOWDROP", "RAINSPL", "SMOKE". */
    int32_t  texture_slot;      /* +0x3C: texture page slot index.
                                 *        For non-streamed pages: physical index = slot - 0x400.
                                 *        Used by UploadRaceTexturePage and BindRaceTexturePage. */
} TD5_StaticHedEntry;

_Static_assert(sizeof(TD5_StaticHedEntry) == 0x40, "TD5_StaticHedEntry size drifted from 0x40");

/* ========================================================================
 * 13. TD5_TexturePageMeta -- Per-page metadata from static.hed (16 bytes = 0x10)
 *
 * Follows the named entry array in static.hed. One entry per texture page.
 * Controls pixel format, upload mode, and source/target dimensions.
 *
 * Consumed by LoadRaceTexturePages (0x442770) and
 * LoadStaticTrackTextureHeader (0x442560) for dimension selection.
 * ======================================================================== */

typedef struct TD5_TexturePageMeta {
    int32_t  transparency_flag;     /* +0x00: 0=opaque (3 bytes/pixel RGB24),
                                     *        nonzero=transparent (4 bytes/pixel RGBA32). */
    int32_t  image_type;            /* +0x04: upload mode:
                                     *          0 = DXD3DTexture::LoadRGBS24 (RGB24)
                                     *          1 = DXD3DTexture::LoadRGBS32 type 7 (RGBA32)
                                     *          2 = DXD3DTexture::LoadRGBS32 type 8 (RGBA32) */
    int32_t  source_dimension;      /* +0x08: source texture width=height (square, typically 256) */
    int32_t  target_dimension;      /* +0x0C: target texture width=height after downscale.
                                     *        Dimension selection depends on quality mode
                                     *        (DAT_004c3d04): mode 0 uses min(src,tgt). */
} TD5_TexturePageMeta;

_Static_assert(sizeof(TD5_TexturePageMeta) == 0x10, "TD5_TexturePageMeta size drifted from 0x10");

/* ========================================================================
 * 14. TD5_StaticHedHeader -- static.hed file header (8 bytes)
 *
 * Precedes the named entry array and page metadata array.
 * ======================================================================== */

typedef struct TD5_StaticHedHeader {
    int32_t  texture_page_count;    /* +0x00: total tpage slots (includes 4 reserved for car skins).
                                     *        Level textures = texture_page_count - 4. */
    int32_t  named_entry_count;     /* +0x04: number of TD5_StaticHedEntry records following. */
    /* TD5_StaticHedEntry named_entries[named_entry_count]; */
    /* TD5_TexturePageMeta page_metadata[texture_page_count]; */
} TD5_StaticHedHeader;

/* ========================================================================
 * 15. TD5_StripJumpEntry -- Segment boundary jump table entry (6 bytes)
 *
 * Stored after the 5-dword STRIP.DAT header. Used for tracks with
 * non-contiguous span numbering (branching paths, shortcuts).
 *
 * Count at header + 0x14 (DAT_004c3da0 + 0x14).
 * Array at header + 0x18 (DAT_004c3da0 + 0x18).
 *
 * Consumer: ResolveActorSegmentBoundary (0x443FF0).
 * ======================================================================== */

typedef struct TD5_StripJumpEntry {
    uint16_t start_span;    /* +0x00: first span of the remapping range */
    uint16_t end_span;      /* +0x02: last span of the remapping range */
    int16_t  remap_offset;  /* +0x04: span index adjustment (span += target - source) */
} TD5_StripJumpEntry;

/* ========================================================================
 * 16. TD5_TrackTextureRecord -- TEXTURES.DAT per-texture record
 *
 * TEXTURES.DAT is a concatenation of these records. Each contains a
 * palettized 64x64 texture. Expanded to ARGB32 by
 * BuildTrackTextureCacheImpl (0x40B1D0).
 *
 * NOTE: This struct is variable-length due to palette_count.
 * In practice palette_count is always 256, giving a fixed record size
 * of 3 + 1 + 4 + (256*3) + 4096 = 4872 bytes.
 * ======================================================================== */

typedef struct TD5_TrackTextureRecord {
    uint8_t  header_pad[3];         /* +0x00: header padding / alignment bytes */
    uint8_t  format_type;           /* +0x03: see TD5_TextureFormat enum.
                                     *        0=opaque, 1=colorkey, 2=semitrans, 3=opaque-alt. */
    int32_t  palette_count;         /* +0x04: number of palette entries (typically 256) */
    /* uint8_t  palette[palette_count * 3];   RGB palette (3 bytes per entry: R, G, B) */
    /* uint8_t  pixels[4096];                 64x64 palette indices */
} TD5_TrackTextureRecord;

/* ========================================================================
 * Accessor Macros
 * ======================================================================== */

/** Get pointer to span record N in the loaded span table */
#define TD5_SPAN(n) \
    ((TD5_StripSpan*)((uint8_t*)(*(uint32_t*)TD5_ADDR_SPAN_TABLE) + (n) * TD5_STRIP_SPAN_STRIDE))

/** Get pointer to vertex N in the loaded vertex table */
#define TD5_VERTEX(n) \
    ((TD5_StripVertex*)((uint8_t*)(*(uint32_t*)TD5_ADDR_VERTEX_TABLE) + (n) * TD5_STRIP_VERTEX_STRIDE))

/** Get total span count (ring length) */
#define TD5_SPAN_COUNT() \
    (*(int32_t*)TD5_ADDR_SPAN_COUNT)

/** Get sub-span lane count from a span's packed byte */
#define TD5_SPAN_LANE_COUNT(span_ptr) \
    ((span_ptr)->packed_sub_span & 0x0F)

/** Get height offset nibble from a span's packed byte */
#define TD5_SPAN_HEIGHT_OFFSET(span_ptr) \
    ((span_ptr)->packed_sub_span >> 4)

/** Get the gTrackEnvironmentConfig pointer as TD5_LevelInf* */
#define TD5_LEVELINF() \
    ((TD5_LevelInf*)(*(uint32_t*)TD5_ADDR_LEVELINF))

/** Get the checkpoint metadata pointer */
#define TD5_CHECKPOINT_META() \
    ((TD5_CheckpointMetadata*)(*(uint32_t*)TD5_ADDR_CHECKPOINT_META))

/** Get the CHECKPT.NUM matrix buffer */
#define TD5_CHECKPOINT_TABLE() \
    ((TD5_CheckpointTable*)(TD5_ADDR_CHECKPOINT_BUF))

/** Get pointer to TRAFFIC.BUS data */
#define TD5_TRAFFIC_BUS() \
    ((TD5_TrafficBusEntry*)(*(uint32_t*)TD5_ADDR_TRAFFIC_BUS))

/** Get pointer to the lighting entry table for the current track */
#define TD5_LIGHTING_TABLE() \
    ((TD5_TrackLightEntry*)(*(uint32_t*)TD5_ADDR_LIGHTING_TABLE))

/** Compute world coordinate from span origin + vertex offset (24.8 fixed-point) */
#define TD5_WORLD_COORD(origin, vertex_component) \
    (((origin) + (int32_t)(vertex_component)) << 8)

/** Convert world coordinate to float */
#define TD5_WORLD_TO_FLOAT(coord) \
    ((float)(coord) * (1.0f / 256.0f))

/* ========================================================================
 * Span Type Lookup Tables (read-only, in .rdata)
 *
 * These tables are indexed by span type (0-11) and control the
 * triangle topology and edge test behavior.
 * ======================================================================== */

#define TD5_ADDR_EDGE_MASK_LEFT     0x00474E28  /* byte[12*2]: left edge mask, stride 2 */
#define TD5_ADDR_EDGE_MASK_RIGHT    0x00474E29  /* byte[12*2]: right edge mask, stride 2 */
#define TD5_ADDR_VTXOFF_LEFT        0x00474E40  /* byte[12*2]: left vertex offset, stride 2 */
#define TD5_ADDR_VTXOFF_RIGHT       0x00474E41  /* byte[12*2]: right vertex offset, stride 2 */
#define TD5_ADDR_SUSPENSION_FLAG    0x0046318C  /* byte[12]: per-type suspension enable */

/** Span type suspension response flags (from gSpanTypeSuspensionFlag at 0x46318C).
 *  1 = full gravity-compensated vertical force, 0 = disabled (bouncy). */
static const uint8_t TD5_SpanTypeSuspensionFlag[TD5_SPAN_TYPE_COUNT] = {
    1, /* 0: straight */
    1, /* 1: gentle curve */
    1, /* 2: sharp curve */
    0, /* 3: chicane/S-curve */
    1, /* 4: crest/hill */
    0, /* 5: dip/valley */
    0, /* 6: crossover */
    0, /* 7: junction entry */
    1, /* 8: branch (fork) */
    0, /* 9: sentinel (start) */
    0, /* 10: sentinel (end) */
    0, /* 11: reserved */
};

/* ========================================================================
 * 17. TD5_HighScoreEntry -- Single high-score slot (32 bytes = 0x20) NEW wave3
 *
 * Five entries per NPC racer group. Sorted best-to-worst.
 * Score interpretation depends on group_type (see TD5_NpcGroupType).
 *
 * Displayed by DrawHighScorePanel (0x413010).
 * Persisted in Config.td5 alongside the parent group.
 * ======================================================================== */

typedef struct TD5_HighScoreEntry {
    char     name[16];      /* +0x00: null-padded ASCII player/NPC name */
    uint32_t score;         /* +0x10: time (centiseconds), lap time, points, or ms
                             *        depending on parent group's group_type. */
    uint32_t car_index;     /* +0x14: car index used (mapped through remap table at 0x463E24) */
    uint32_t avg_speed;     /* +0x18: average speed (display units, MPH or KPH) */
    uint32_t top_speed;     /* +0x1C: top speed (display units) */
} TD5_HighScoreEntry;

_Static_assert(sizeof(TD5_HighScoreEntry) == 0x20, "TD5_HighScoreEntry size drifted from 0x20");

/* ========================================================================
 * 18. TD5_NpcRacerGroup -- NPC racer group record (164 bytes = 0xA4) NEW wave3
 *
 * 26 groups at VA 0x4643B8, stride 0xA4.
 * Persisted in Config.td5 at offset 0x03E1.
 * Each group holds 5 high-score entries for a cup race opponent bracket.
 *
 * group_type controls score display format:
 *   0 = Time (centiseconds, MM:SS.cc) -- point-to-point races
 *   1 = Lap/Era (centiseconds, MM:SS.cc) -- era cup lap-based
 *   2 = Points (raw integer) -- championship/ultimate scoring
 *   4 = TimeTrial (milliseconds, MM:SS.mmm) -- time trial mode
 *
 * Consumers:
 *   DrawHighScorePanel (0x413010) -- display
 *   AwardCupCompletionUnlocks (0x421DA0) -- unlock logic
 *   WritePackedConfigTd5 / LoadPackedConfigTd5 -- persistence
 *
 * Group-to-race assignment: Race-to-NPC-Group table at 0x4640A4 (Section 15
 * in data-tables-decoded.md).
 * ======================================================================== */

typedef struct TD5_NpcRacerGroup {
    uint8_t  group_type;    /* +0x00: scoring type (see TD5_NpcGroupType enum) */
    uint8_t  pad[3];        /* +0x01: alignment padding */
    TD5_HighScoreEntry entries[TD5_NPC_HIGHSCORE_ENTRIES];
                            /* +0x04: 5 high-score slots (5 x 32 = 160 bytes) */
} TD5_NpcRacerGroup;

_Static_assert(sizeof(TD5_NpcRacerGroup) == 0xA4, "TD5_NpcRacerGroup size drifted from 0xA4");
_Static_assert(offsetof(TD5_NpcRacerGroup, group_type) == 0x00, "TD5_NpcRacerGroup.group_type offset drifted");
_Static_assert(offsetof(TD5_NpcRacerGroup, entries) == 0x04, "TD5_NpcRacerGroup.entries offset drifted");

/* ========================================================================
 * 19. TD5_CameraPreset -- Chase camera preset (16 bytes = 0x10) NEW wave3
 *
 * 7 presets at VA 0x463098, stride 0x10.
 * Indexed through DAT_00482FD0[player_slot] (current camera mode).
 * Consumer: SetCameraPreset (0x401450).
 *
 * Presets 0-5 are chase cameras with increasing proximity.
 * Preset 6 is bumper/hood cam (height_offset=1 triggers alternate path,
 * flags=0xFF38 contains packed mode bits, distance/pitch/fov all zero).
 * ======================================================================== */

typedef struct TD5_CameraPreset {
    int16_t  height_offset;     /* +0x00: camera height above car (world units).
                                 *        0 for chase cams, 1 for bumper cam. */
    int16_t  follow_distance;   /* +0x02: distance behind car (world units).
                                 *        Range: 240 (very close) to 600 (far). */
    int16_t  pitch_angle;       /* +0x04: camera look-down angle (engine angle units).
                                 *        Range: 1200 (close) to 2100 (far). */
    int16_t  fov_adjust;        /* +0x06: field of view adjustment.
                                 *        Higher = wider FOV. 0 for bumper cam. */
    uint32_t flags;             /* +0x08: camera behavior flags.
                                 *        0x00 for chase cams, 0xFF38 for bumper cam. */
    uint32_t reserved;          /* +0x0C: unused (always 0) */
} TD5_CameraPreset;

_Static_assert(sizeof(TD5_CameraPreset) == 0x10, "TD5_CameraPreset size drifted from 0x10");

#pragma pack(pop)

#endif /* TD5_LEVEL_FORMATS_H */
