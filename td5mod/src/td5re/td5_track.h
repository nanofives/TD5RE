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
 *   0x435930  InitializeTrafficActorsFromQueue
 *   0x435310  RecycleTrafficActorFromQueue
 */

#ifndef TD5_TRACK_H
#define TD5_TRACK_H

#include "td5_types.h"

/* Forward declaration for per-probe track update */
struct TD5_TrackProbeState;

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
int              td5_track_is_valid_mesh_ptr(const void *ptr);
int              td5_track_is_ptr_in_blob(const void *ptr, size_t size);
void            *td5_track_get_display_list(int span_index);

/* --- MODELS.DAT --- */
int  td5_track_parse_models_dat(const void *data, size_t size);
void td5_track_prepare_mesh_resource(TD5_MeshHeader *mesh);

/* Halve per-vertex diffuse of billboard meshes that draw through a
 * type-3 (additive) texture page. Call AFTER td5_asset_load_track_textures
 * so the transparency table is populated. */
void td5_track_dim_additive_billboard_meshes(void);
int  td5_track_get_models_display_list_count(void);
int  td5_track_get_span_display_list_index(int span_index);
const void *td5_track_get_models_display_list_raw(int index, size_t *size_out);

/* --- Actor track position --- */
void td5_track_update_actor_position(TD5_Actor *actor);
void td5_track_update_probe_position(struct TD5_TrackProbeState *probe,
                                     int32_t world_x, int32_t world_z);
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
void td5_track_get_span_edges(int span_index,
                               int *left_x, int *left_z,
                               int *right_x, int *right_z);
int  td5_track_get_span_center_world(int span_index,
                                      int *out_x, int *out_y, int *out_z);
int  td5_track_get_span_lane_world(int span_index, int sub_lane,
                                    int *out_x, int *out_y, int *out_z);

/* --- Lighting --- */
void td5_track_apply_segment_lighting(TD5_Actor *actor, int view_index);
void td5_track_update_light_directions(void);

/* --- Track wall contact resolution (FUN_00406CC0) ---
 * Checks all 4 wheel probes against span edge boundaries.
 * Calls td5_physics_wall_response for each penetration. */
void td5_track_resolve_wall_contacts(TD5_Actor *actor);

/* --- Forward/Reverse boundary contact resolution ---
 * FUN_00406F50 / FUN_004070E0 — check probes against the track's
 * forward-/reverse-side boundary sentinel strips. The two sentinels
 * are per-level constants from a 40-entry table at original VA
 * 0x00473820, NOT `1` / `s_span_count-2`. Bind via
 * td5_track_bind_boundary_sentinels() before racing on a level. */
void td5_track_resolve_forward_contacts(TD5_Actor *actor);
void td5_track_resolve_reverse_contacts(TD5_Actor *actor);

/* Bind the forward/reverse boundary sentinel pair for the level about
 * to race. level_number is the 1-based level ZIP number (same value
 * td5_asset_level_number() returns). Values outside the populated
 * range disable the boundary handlers for that level. */
void td5_track_bind_boundary_sentinels(int level_number);

/* --- Traffic --- */
void td5_track_init_traffic_from_queue(void);
void td5_track_recycle_traffic_actor(void);

/* --- Route data (LEFT/RIGHT.TRK) --- */
int  td5_track_load_routes(const void *left_data, size_t left_size,
                            const void *right_data, size_t right_size);

/* --- Wrap normalization --- */
int  td5_track_normalize_actor_wrap(TD5_Actor *actor);

/* --- Checkpoint detection --- */
int  td5_track_check_checkpoint(TD5_Actor *actor);

/* --- Route heading helpers --- */
int  td5_track_get_primary_route_heading(int span_index);

/* --- Signed spline distance (0x434670) --- */
int32_t td5_track_compute_spline_position(int span_index, int segment_distance,
                                           int route_lane);

/* --- AI target point sampling (0x434800) ---
 * Computes world-space XZ target for AI look-ahead by interpolating between
 * strip vertices using route_byte, then applying perpendicular lateral_bias.
 * Returns 24.8 fixed-point coordinates. Returns 1 on success, 0 on failure. */
int  td5_track_sample_target_point(int span_index, int route_byte,
                                    int *out_x, int *out_z, int lateral_bias);

#endif /* TD5_TRACK_H */
