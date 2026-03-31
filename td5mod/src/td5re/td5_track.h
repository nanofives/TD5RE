/**
 * td5_track.h -- Track geometry, segment contacts, strip data
 *
 * Original functions:
 *   0x444070  BindTrackStripRuntimePointers
 *   0x42FB90  LoadTrackRuntimeData
 *   0x42FAD0  InitializeTrackStripMetadata
 *   0x42FB40  ApplyTrackStripAttributeOverrides
 *   0x4440F0  UpdateActorTrackPosition
 *   0x430150  ApplyTrackLightingForVehicleSegment
 *   0x42CE90  UpdateActiveTrackLightDirections
 *   0x431260  GetTrackSpanDisplayListEntry
 *   0x431190  ParseModelsDat
 *   0x40AC00  PrepareMeshResource
 *   0x434FE0  UpdateActorTrackBehavior (AI path-following)
 *   0x436A70  UpdateRaceActors
 *   0x42F5B0  UpdateRaceOrder
 *   0x435930  InitializeTrafficActorsFromQueue
 *   0x435310  RecycleTrafficActorFromQueue
 */

#ifndef TD5_TRACK_H
#define TD5_TRACK_H

#include "td5_types.h"

/* ========================================================================
 * Track Light Entry (36 bytes, 0x24 stride)
 *
 * Hardcoded per-track in EXE, pointed to by lighting table pointer.
 * Used by ApplyTrackLightingForVehicleSegment.
 * ======================================================================== */

typedef struct TD5_TrackLightEntry {
    int16_t  span_start;        /* +0x00 */
    int16_t  span_end;          /* +0x02 */
    int16_t  light_dir_x;       /* +0x04 */
    int16_t  light_dir_y;       /* +0x06 */
    int16_t  light_dir_z;       /* +0x08 */
    uint8_t  light_color_r;     /* +0x0A */
    uint8_t  light_color_g;     /* +0x0B */
    uint8_t  light_color_b;     /* +0x0C */
    uint8_t  ambient_r;         /* +0x0D */
    uint8_t  ambient_g;         /* +0x0E */
    uint8_t  ambient_b;         /* +0x0F */
    uint32_t fog_color_override; /* +0x10 */
    uint8_t  reserved_14[4];    /* +0x14 */
    uint8_t  light_mode;        /* +0x18: 0=instant, 1=interpolate, 2=multi-dir */
    uint8_t  blend_distance;    /* +0x19 */
    uint8_t  reserved_1A[2];    /* +0x1A */
    uint32_t projection_effect; /* +0x1C */
    uint32_t material_flag;     /* +0x20 */
} TD5_TrackLightEntry;

/* ========================================================================
 * Traffic Bus Entry (4 bytes)
 * ======================================================================== */

typedef struct TD5_TrafficBusEntry {
    int16_t  span_index;    /* +0x00: -1 = end sentinel */
    uint8_t  flags;         /* +0x02: bit 0 = oncoming direction */
    uint8_t  lane;          /* +0x03: sub-lane index */
} TD5_TrafficBusEntry;

/* ========================================================================
 * MODELS.DAT entry (runtime)
 * ======================================================================== */

typedef struct TD5_ModelsDatEntry {
    void    *mesh_ptr;          /* pointer to mesh data */
    int32_t  texture_page_id;   /* texture page reference */
    float    bounding_radius;   /* bounding sphere */
} TD5_ModelsDatEntry;

/* ========================================================================
 * Directional light slot (runtime)
 * ======================================================================== */

#define TD5_TRACK_LIGHT_SLOTS  3

/* ========================================================================
 * Route Data (LEFT.TRK / RIGHT.TRK)
 *
 * Each file is a byte array indexed by span number, giving lateral offset
 * (0x00-0xFF = left-to-right across track width). Three bytes per span:
 *   [0] = lateral offset byte
 *   [1] = heading byte
 *   [2] = reserved
 * ======================================================================== */

/* --- Module lifecycle --- */
int  td5_track_init(void);
void td5_track_shutdown(void);
void td5_track_tick(void);

/* --- STRIP.DAT loading --- */
int  td5_track_load_strip(const void *data, size_t size);
void td5_track_bind_runtime_pointers(void);
void td5_track_apply_attribute_overrides(void);

/* --- Track runtime data --- */
int  td5_track_load_runtime_data(int track_index, int reverse);

/* --- Span access --- */
int              td5_track_get_span_count(void);
TD5_StripSpan   *td5_track_get_span(int index);
TD5_StripVertex *td5_track_get_vertex(int index);
void            *td5_track_get_display_list(int span_index);

/* --- MODELS.DAT --- */
int  td5_track_parse_models_dat(const void *data, size_t size);
void td5_track_prepare_mesh_resource(TD5_MeshHeader *mesh);
int  td5_track_get_models_display_list_count(void);
int  td5_track_get_span_display_list_index(int span_index);
const void *td5_track_get_models_display_list_raw(int index, size_t *size_out);

/* --- Actor track position --- */
void td5_track_update_actor_position(TD5_Actor *actor);
int  td5_track_get_surface_type(TD5_Actor *actor, int probe_index);
void td5_track_compute_heading(TD5_Actor *actor);

/* --- Barycentric contact --- */
int32_t td5_track_compute_contact_height(int span_index, int sub_lane,
                                          int32_t world_x, int32_t world_z);
int32_t td5_track_compute_contact_height_with_normal(int span_index, int sub_lane,
                                                      int32_t world_x, int32_t world_z,
                                                      int16_t *out_normal);
int  td5_track_probe_height(int world_x, int world_z, int current_span,
                             int *out_y, int *out_surface_type);

/* --- Lighting --- */
void td5_track_apply_segment_lighting(TD5_Actor *actor, int view_index);
void td5_track_update_light_directions(void);

/* --- Race order --- */
void td5_track_update_race_order(void);

/* --- Traffic --- */
void td5_track_init_traffic_from_queue(void);
void td5_track_recycle_traffic_actor(void);

/* --- Route data (LEFT/RIGHT.TRK) --- */
int  td5_track_load_routes(const void *left_data, size_t left_size,
                            const void *right_data, size_t right_size);

/* --- Wrap normalization --- */
void td5_track_normalize_actor_wrap(TD5_Actor *actor);

/* --- Checkpoint detection --- */
int  td5_track_check_checkpoint(TD5_Actor *actor);

#endif /* TD5_TRACK_H */
