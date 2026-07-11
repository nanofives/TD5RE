/* td5_track_internal.h -- PRIVATE seam between td5_track.c and
 * td5_track_parser.c (S6 module split, see REFACTOR_PLAN.md).
 *
 * MODELS.DAT parsing state/types and the handful of helper functions
 * shared across the new file boundary. Not a public API -- only
 * td5_track.c and td5_track_parser.c include this. Declarations here
 * must match td5_track.c's definitions verbatim (same rule as
 * td5_physics_internal.h / td5_ai_internal.h).
 */

#ifndef TD5_TRACK_INTERNAL_H
#define TD5_TRACK_INTERNAL_H

#include <stdint.h>
#include <stddef.h>
#include "td5_track.h"

#define MODELS_DAT_MAX_ENTRIES  2048

#pragma pack(push, 1)
typedef struct TD5_TrackRawMeshHeader {
    uint8_t  render_type;
    uint8_t  texture_page_id;
    uint16_t flags;
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
    uint32_t commands_offset;
    uint32_t vertices_offset;
    uint32_t normals_offset;
} TD5_TrackRawMeshHeader;
#pragma pack(pop)

/* ========================================================================
 * Shared cross-file state (defined non-static in td5_track.c; the parser
 * in td5_track_parser.c reads/writes these).
 * ======================================================================== */

extern int              s_span_count;
extern TD5_ModelsDatEntry s_models[MODELS_DAT_MAX_ENTRIES];
extern int              s_model_count;
extern uint8_t         *s_models_blob;
extern size_t           s_models_blob_size;
extern uint32_t        *s_models_entry_offsets;
extern uint32_t        *s_models_entry_sizes;
extern int              s_models_display_list_count;

/* ========================================================================
 * Functions defined in the remaining td5_track.c, called from
 * td5_track_parser.c.
 * ======================================================================== */

int  get_display_list_first_mesh_header(int index,
                                        const TD5_TrackRawMeshHeader **out_mesh);
void rebuild_span_display_list_mapping(void);
void free_models_dat_runtime(void);

#endif /* TD5_TRACK_INTERNAL_H */
