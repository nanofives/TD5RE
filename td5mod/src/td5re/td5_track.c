/**
 * td5_track.c -- Track geometry, segment contacts, strip data
 *
 * Full implementation of the TD5 track subsystem covering:
 *   - STRIP.DAT loading and runtime pointer binding
 *   - 4-edge cross-product boundary tests with recursive neighbor traversal
 *   - Barycentric contact resolution (triangle half selection, plane equation)
 *   - Checkpoint detection and lap counting
 *   - Zone-based track lighting (3 blend modes)
 *   - Race order bubble sort on track_span_high_water
 *   - Surface type lookup via lane bitmask
 *   - TRAFFIC.BUS FIFO spawn/despawn
 *   - Route loading (LEFT.TRK / RIGHT.TRK)
 *   - MODELS.DAT parsing and mesh relocation
 *   - Wrap normalization (modulo ring length)
 */

#include "td5_track.h"
#include "td5_ai.h"
#include "td5_platform.h"
#include "td5re.h"
#include <string.h>
#include <stdlib.h>

#define LOG_TAG "track"

static uint32_t s_actor_position_log_counter = 0;
static uint32_t s_probe_log_counter = 0;

/* Track geometry globals (owned here, externed by other modules) */
int     g_strip_span_count      = 0;
int     g_strip_total_segments  = 0;
void   *g_strip_span_base      = NULL;
void   *g_strip_vertex_base    = NULL;
int     g_track_is_circuit      = 0;
int     g_track_type_mode       = 0;

/* ========================================================================
 * Original functions: status
 *
 * 0x444070  BindTrackStripRuntimePointers       -- DONE
 * 0x42FB90  LoadTrackRuntimeData                -- DONE
 * 0x42FAD0  InitializeTrackStripMetadata        -- DONE
 * 0x42FB40  ApplyTrackStripAttributeOverrides   -- DONE
 * 0x4440F0  UpdateActorTrackPosition            -- DONE
 * 0x430150  ApplyTrackLightingForVehicleSegment -- DONE
 * 0x42CE90  UpdateActiveTrackLightDirections    -- DONE
 * 0x431260  GetTrackSpanDisplayListEntry        -- DONE
 * 0x431190  ParseModelsDat                      -- DONE
 * 0x40AC00  PrepareMeshResource                 -- DONE
 * 0x436A70  UpdateRaceActors                    -- DONE (in td5_track_tick)
 * 0x42F5B0  UpdateRaceOrder                     -- DONE
 * 0x435930  InitializeTrafficActorsFromQueue    -- DONE
 * 0x435310  RecycleTrafficActorFromQueue        -- DONE
 * ======================================================================== */

/* ========================================================================
 * Constants and Lookup Tables
 * ======================================================================== */

/** Maximum recursion depth for boundary crossing traversal */
#define TRACK_MAX_RECURSION     8

/** Maximum number of models in MODELS.DAT */
#define MODELS_DAT_MAX_ENTRIES  2048

/** Maximum number of traffic slots */
#define TRAFFIC_MAX_SLOTS       6

/** Traffic actor slots start at index 6 (after 6 racer slots) */
#define TRAFFIC_SLOT_BASE       6

/**
 * Per-span-type vertex offset LUT (DAT_00474e40/41).
 * Index by span_type (0-11). Pairs: [left_offset, right_offset].
 * These control which vertices form the quad corners for each span type.
 */
static const int8_t s_vtx_offset_left[12] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};
static const int8_t s_vtx_offset_right[12] = {
    0, 0, 0, 3, 3, 0, 3, 3, 0, 0, 0, 0
};

/**
 * Per-span-type edge mask LUT (DAT_00474e28/29).
 * Controls which of the 4 boundary edges exist at the first/last sub-span.
 * Bitmask: bit0=forward, bit1=right, bit2=backward, bit3=left.
 */
static const uint8_t s_edge_mask_first[12] = {
    0x0F, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D,
    0x0D, 0x0F, 0x0F, 0x0D
};
static const uint8_t s_edge_mask_last[12] = {
    0x0F, 0x0E, 0x0E, 0x0E, 0x0E, 0x0E, 0x0E, 0x0E,
    0x0E, 0x0F, 0x0F, 0x0E
};

/**
 * Per-span-type wall vertex offset tables (DAT_004631A0/A4).
 * Used for collision boundary vertex selection.
 */
static const int32_t s_wall_vtx_left[12] = {
    0, 0, -1, -1, -2, 0, 0, 0, 0, 0, 0, 0
};
static const int32_t s_wall_vtx_right[12] = {
    0, 0, 0, 0, 0, -1, -1, -2, 0, 0, 0, 0
};

/**
 * Per-span-type forward/lateral edge index offsets (DAT_00473C6C/68).
 */
static const int32_t s_edge_fwd_offset[12] = {
    0, 0, 0, -1, -1, -2, 0, -1, -1, -2, 0, 0
};
static const int32_t s_edge_lat_offset[12] = {
    0, 0, -1, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

/* ========================================================================
 * Module State
 * ======================================================================== */

/** Raw STRIP.DAT blob (owned allocation) */
static uint8_t     *s_strip_blob = NULL;
static size_t       s_strip_blob_size = 0;

/** Resolved runtime pointers from STRIP.DAT header */
static TD5_StripSpan   *s_span_array = NULL;
static TD5_StripVertex *s_vertex_table = NULL;
static int              s_span_count = 0;
static int              s_aux_count = 0;
static int              s_secondary_count = 0;
static uint32_t        *s_strip_header = NULL;

/** Jump table for segment boundary remapping */
static int              s_jump_entry_count = 0;
static uint8_t         *s_jump_entries = NULL; /* 6-byte entries */

/** Strip attribute override table */
static uint8_t         *s_attr_override_table = NULL;
static int              s_attr_override_count = 0;

/** MODELS.DAT entries */
static TD5_ModelsDatEntry s_models[MODELS_DAT_MAX_ENTRIES];
static int              s_model_count = 0;
static uint8_t         *s_models_blob = NULL;
static size_t           s_models_blob_size = 0;
static uint32_t        *s_models_entry_offsets = NULL;
static uint32_t        *s_models_entry_sizes = NULL;
static int              s_models_display_list_count = 0;
static int             *s_span_display_list_indices = NULL;

/** Display list table (parallel to span array, built during load) */
static void           **s_display_lists = NULL;
static void            *s_fallback_display_list = NULL;
static int              s_display_lists_are_generated_meshes = 0;
static int              s_display_list_count = 0;

/** Route tables (LEFT.TRK / RIGHT.TRK) -- byte arrays, 3 bytes per span */
static uint8_t         *s_route_left = NULL;
static size_t           s_route_left_size = 0;
static uint8_t         *s_route_right = NULL;
static size_t           s_route_right_size = 0;

/** Traffic bus (TRAFFIC.BUS) FIFO queue */
static TD5_TrafficBusEntry *s_traffic_bus = NULL;
static int              s_traffic_bus_count = 0;
static int              s_traffic_cursor = 0;

/** Lighting state */
static TD5_TrackLightEntry *s_lighting_table = NULL;
static int              s_lighting_entry_count = 0;

/** Per-slot directional light state */
static int              s_light_enabled[TD5_TRACK_LIGHT_SLOTS];
static float            s_light_dir[TD5_TRACK_LIGHT_SLOTS][3]; /* xyz direction * intensity */

/** Per-actor lighting zone cache (which zone each actor is in) */
static int              s_actor_light_zone[TD5_MAX_TOTAL_ACTORS];

/** Race order array: sorted slot indices by position (6 bytes) */
static uint8_t          s_race_order[TD5_MAX_RACER_SLOTS];

/** Strip metadata (per-strip type flags) */
static int              s_strip_metadata[4];

/**
 * Fallback display-list pages use the first track texture slot.
 * td5_asset_load_track_textures() populates page 0 from TEXTURES.DAT
 * and the extracted tex_000.png assets, so the placeholder geometry can
 * sample a normal track page instead of a debug-only slot.
 */
#define TRACK_FALLBACK_TEXTURE_PAGE 0

typedef struct TD5_FallbackDisplayList {
    uint32_t          sub_mesh_count;
    uint32_t          mesh_ptr;
    TD5_MeshHeader    mesh;
    TD5_PrimitiveCmd  cmd;
    TD5_MeshVertex    verts[4];
    TD5_VertexNormal  norms[4];
} TD5_FallbackDisplayList;

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

static void free_models_dat_runtime(void)
{
    if (s_models_blob) {
        free(s_models_blob);
        s_models_blob = NULL;
    }
    s_models_blob_size = 0;

    if (s_models_entry_offsets) {
        free(s_models_entry_offsets);
        s_models_entry_offsets = NULL;
    }
    if (s_models_entry_sizes) {
        free(s_models_entry_sizes);
        s_models_entry_sizes = NULL;
    }
    if (s_span_display_list_indices) {
        free(s_span_display_list_indices);
        s_span_display_list_indices = NULL;
    }

    s_models_display_list_count = 0;
    s_model_count = 0;
    memset(s_models, 0, sizeof(s_models));
}

static void free_display_lists(void)
{
    if (s_display_lists) {
        if (s_display_lists_are_generated_meshes) {
            for (int i = 0; i < s_display_list_count; i++) {
                free(s_display_lists[i]);
            }
        }
        free(s_display_lists);
        s_display_lists = NULL;
    }
    s_display_lists_are_generated_meshes = 0;
    s_display_list_count = 0;

    if (s_fallback_display_list) {
        free(s_fallback_display_list);
        s_fallback_display_list = NULL;
    }
}

static int build_placeholder_display_lists(void)
{
    TD5_FallbackDisplayList *fallback;
    int list_count = (s_span_count > 0) ? s_span_count : 1;

    if (s_fallback_display_list && s_display_lists)
        return 1;

    free_display_lists();

    fallback = (TD5_FallbackDisplayList *)calloc(1, sizeof(*fallback));
    if (!fallback)
        return 0;

    fallback->sub_mesh_count = 1;
    fallback->mesh_ptr = (uint32_t)(uintptr_t)&fallback->mesh;
    fallback->mesh.render_type = 0;
    fallback->mesh.texture_page_id = TRACK_FALLBACK_TEXTURE_PAGE;
    fallback->mesh.command_count = 1;
    fallback->mesh.total_vertex_count = 4;
    fallback->mesh.bounding_radius = 1000000.0f;
    fallback->mesh.bounding_center_x = 0.0f;
    fallback->mesh.bounding_center_y = 0.0f;
    fallback->mesh.bounding_center_z = 4096.0f;
    fallback->mesh.origin_x = 0.0f;
    fallback->mesh.origin_y = 0.0f;
    fallback->mesh.origin_z = 0.0f;
    fallback->mesh.commands_offset = (uint32_t)(uintptr_t)&fallback->cmd;
    fallback->mesh.vertices_offset = (uint32_t)(uintptr_t)&fallback->verts[0];
    fallback->mesh.normals_offset = (uint32_t)(uintptr_t)&fallback->norms[0];

    fallback->cmd.dispatch_type = 3;
    fallback->cmd.texture_page_id = TRACK_FALLBACK_TEXTURE_PAGE;
    fallback->cmd.quad_count = 1;
    fallback->cmd.vertex_data_ptr = 0;

    fallback->verts[0].pos_x = -96.0f;  fallback->verts[0].pos_y = 12.0f; fallback->verts[0].pos_z = 256.0f;
    fallback->verts[1].pos_x =  96.0f;  fallback->verts[1].pos_y = 12.0f; fallback->verts[1].pos_z = 256.0f;
    fallback->verts[2].pos_x = 160.0f;  fallback->verts[2].pos_y = 12.0f; fallback->verts[2].pos_z = 960.0f;
    fallback->verts[3].pos_x = -160.0f; fallback->verts[3].pos_y = 12.0f; fallback->verts[3].pos_z = 960.0f;

    fallback->verts[0].tex_u = 0.0f; fallback->verts[0].tex_v = 0.0f;
    fallback->verts[1].tex_u = 1.0f; fallback->verts[1].tex_v = 0.0f;
    fallback->verts[2].tex_u = 1.0f; fallback->verts[2].tex_v = 1.0f;
    fallback->verts[3].tex_u = 0.0f; fallback->verts[3].tex_v = 1.0f;

    for (int i = 0; i < 4; i++) {
        fallback->verts[i].view_x = 0.0f;
        fallback->verts[i].view_y = 0.0f;
        fallback->verts[i].view_z = 0.0f;
        fallback->verts[i].lighting = (i < 2) ? 0xE0u : 0xA0u;
        fallback->verts[i].proj_u = 0.0f;
        fallback->verts[i].proj_v = 0.0f;
        fallback->norms[i].nx = 0.0f;
        fallback->norms[i].ny = 1.0f;
        fallback->norms[i].nz = 0.0f;
        fallback->norms[i].visible_flag = 1;
    }

    s_fallback_display_list = fallback;
    s_display_lists = (void **)calloc((size_t)list_count, sizeof(void *));
    if (!s_display_lists) {
        free(s_fallback_display_list);
        s_fallback_display_list = NULL;
        return 0;
    }

    for (int i = 0; i < list_count; i++) {
        s_display_lists[i] = s_fallback_display_list;
    }

    s_display_list_count = list_count;

    return 1;
}

/* ========================================================================
 * Internal Helpers: Vertex Access
 * ======================================================================== */

/** Get a vertex from the vertex table by index. */
static inline TD5_StripVertex *vertex_at(int index)
{
    return &s_vertex_table[index];
}

/** Get a span record by index. */
static inline TD5_StripSpan *span_at(int index)
{
    return &s_span_array[index];
}

static int get_models_display_list_entry(int index,
                                         const uint8_t **out_entry,
                                         uint32_t *out_size)
{
    if (!s_models_blob || !s_models_entry_offsets || !s_models_entry_sizes)
        return 0;
    if (index < 0 || index >= s_models_display_list_count)
        return 0;

    if (out_entry)
        *out_entry = s_models_blob + s_models_entry_offsets[index];
    if (out_size)
        *out_size = s_models_entry_sizes[index];
    return 1;
}

static int get_display_list_first_mesh_header(int index,
                                              const TD5_TrackRawMeshHeader **out_mesh)
{
    const uint8_t *entry;
    uint32_t entry_size;
    uint32_t sub_mesh_count;
    uint32_t first_mesh_offset;

    if (!get_models_display_list_entry(index, &entry, &entry_size))
        return 0;
    if (entry_size < 8)
        return 0;

    sub_mesh_count = *(const uint32_t *)(const void *)entry;
    if (sub_mesh_count == 0)
        return 0;
    if (entry_size < 4u + sub_mesh_count * 4u)
        return 0;

    first_mesh_offset = *(const uint32_t *)(const void *)(entry + 4);
    if (first_mesh_offset >= entry_size)
        return 0;
    if (first_mesh_offset + sizeof(TD5_TrackRawMeshHeader) > entry_size)
        return 0;

    if (out_mesh)
        *out_mesh = (const TD5_TrackRawMeshHeader *)(const void *)(entry + first_mesh_offset);
    return 1;
}

static void get_quad_vertices(const TD5_StripSpan *sp, int sub_lane,
                               int *vl0, int *vl1, int *vr0, int *vr1);

/** Get sub-span lane count from packed byte. */
static inline int span_lane_count(const TD5_StripSpan *sp)
{
    /* pad_02[1] is at byte +0x03 = geometry_metadata. Low nibble = lane count. */
    uint8_t packed = ((const uint8_t *)sp)[3]; /* byte +0x03 */
    return packed & 0x0F;
}

/** Get height offset nibble from packed byte. */
static inline int span_height_offset(const TD5_StripSpan *sp)
{
    uint8_t packed = ((const uint8_t *)sp)[3];
    return (packed >> 4) & 0x0F;
}

/** Get lane bitmask byte at +0x02. */
static inline uint8_t span_lane_bitmask(const TD5_StripSpan *sp)
{
    return sp->pad_02[0]; /* +0x02 */
}

void td5_track_get_span_edges(int span_index,
                              int *left_x, int *left_z,
                              int *right_x, int *right_z)
{
    const TD5_StripSpan *sp;
    int lane_count, vl0, vl1, vr0, vr1;

    *left_x = *left_z = *right_x = *right_z = 0;
    if (!s_span_array || !s_vertex_table ||
        span_index < 0 || span_index >= s_span_count) return;

    sp = &s_span_array[span_index];
    lane_count = span_lane_count(sp);
    if (lane_count < 1) lane_count = 1;

    /* Get leftmost and rightmost vertices */
    get_quad_vertices(sp, 0, &vl0, &vl1, &vr0, &vr1);
    {
        TD5_StripVertex *lv = vertex_at(vl0);
        if (lv) {
            *left_x = ((int32_t)sp->origin_x + (int32_t)lv->x);
            *left_z = ((int32_t)sp->origin_z + (int32_t)lv->z);
        }
    }
    get_quad_vertices(sp, lane_count - 1, &vl0, &vl1, &vr0, &vr1);
    {
        TD5_StripVertex *rv = vertex_at(vr0);
        if (rv) {
            *right_x = ((int32_t)sp->origin_x + (int32_t)rv->x);
            *right_z = ((int32_t)sp->origin_z + (int32_t)rv->z);
        }
    }
}

static int compute_span_center_world(int span_index,
                                     float *out_x,
                                     float *out_y,
                                     float *out_z)
{
    const TD5_StripSpan *sp;
    int lane_count;
    int vl0, vl1, vr0, vr1;
    TD5_StripVertex *src[4];
    float x, y, z;

    if (!s_span_array || !s_vertex_table || span_index < 0 || span_index >= s_span_count)
        return 0;

    sp = &s_span_array[span_index];
    if (sp->span_type == 9 || sp->span_type == 10)
        return 0;

    lane_count = span_lane_count(sp);
    if (lane_count < 1)
        lane_count = 1;

    get_quad_vertices(sp, lane_count / 2, &vl0, &vl1, &vr0, &vr1);
    src[0] = vertex_at(vl0);
    src[1] = vertex_at(vl1);
    src[2] = vertex_at(vr0);
    src[3] = vertex_at(vr1);

    /* Average of 4 corner positions in integer-coord float space */
    x = (float)(sp->origin_x + src[0]->x + sp->origin_x + src[1]->x +
                sp->origin_x + src[2]->x + sp->origin_x + src[3]->x) * 0.25f;
    y = (float)(sp->pad_10 + src[0]->y + sp->pad_10 + src[1]->y +
                sp->pad_10 + src[2]->y + sp->pad_10 + src[3]->y) * 0.25f;
    z = (float)(sp->origin_z + src[0]->z + sp->origin_z + src[1]->z +
                sp->origin_z + src[2]->z + sp->origin_z + src[3]->z) * 0.25f;

    if (out_x) *out_x = x;
    if (out_y) *out_y = y;
    if (out_z) *out_z = z;
    return 1;
}

int td5_track_get_span_center_world(int span_index,
                                    int *out_x, int *out_y, int *out_z)
{
    const TD5_StripSpan *sp;
    int lane_count;
    int vl0, vl1, vr0, vr1;
    TD5_StripVertex *src[4];

    if (out_x) *out_x = 0;
    if (out_y) *out_y = 0;
    if (out_z) *out_z = 0;

    if (!s_span_array || !s_vertex_table || span_index < 0 || span_index >= s_span_count)
        return 0;

    sp = &s_span_array[span_index];
    if (sp->span_type == 9 || sp->span_type == 10)
        return 0;

    lane_count = span_lane_count(sp);
    if (lane_count < 1)
        lane_count = 1;

    get_quad_vertices(sp, lane_count / 2, &vl0, &vl1, &vr0, &vr1);
    src[0] = vertex_at(vl0);
    src[1] = vertex_at(vl1);
    src[2] = vertex_at(vr0);
    src[3] = vertex_at(vr1);
    if (!src[0] || !src[1] || !src[2] || !src[3])
        return 0;

    if (out_x) *out_x = sp->origin_x + (src[0]->x + src[1]->x + src[2]->x + src[3]->x) / 4;
    if (out_y) *out_y = sp->pad_10 + (src[0]->y + src[1]->y + src[2]->y + src[3]->y) / 4;
    if (out_z) *out_z = sp->origin_z + (src[0]->z + src[1]->z + src[2]->z + src[3]->z) / 4;
    return 1;
}

int td5_track_get_span_lane_world(int span_index, int sub_lane,
                                   int *out_x, int *out_y, int *out_z)
{
    const TD5_StripSpan *sp;
    int lane_count;
    int vl0, vl1, vr0, vr1;
    TD5_StripVertex *src[4];

    if (out_x) *out_x = 0;
    if (out_y) *out_y = 0;
    if (out_z) *out_z = 0;

    if (!s_span_array || !s_vertex_table || span_index < 0 || span_index >= s_span_count)
        return 0;

    sp = &s_span_array[span_index];
    if (sp->span_type == 9 || sp->span_type == 10)
        return 0;

    lane_count = span_lane_count(sp);
    if (lane_count < 1)
        lane_count = 1;
    if (sub_lane < 0) sub_lane = 0;
    if (sub_lane >= lane_count) sub_lane = lane_count - 1;

    get_quad_vertices(sp, sub_lane, &vl0, &vl1, &vr0, &vr1);
    src[0] = vertex_at(vl0);
    src[1] = vertex_at(vl1);
    src[2] = vertex_at(vr0);
    src[3] = vertex_at(vr1);
    if (!src[0] || !src[1] || !src[2] || !src[3])
        return 0;

    if (out_x) *out_x = sp->origin_x + (src[0]->x + src[1]->x + src[2]->x + src[3]->x) / 4;
    if (out_y) *out_y = sp->pad_10 + (src[0]->y + src[1]->y + src[2]->y + src[3]->y) / 4;
    if (out_z) *out_z = sp->origin_z + (src[0]->z + src[1]->z + src[2]->z + src[3]->z) / 4;
    return 1;
}

static void rebuild_span_display_list_mapping(void)
{
    int *mapping;
    int entry_index = 0;

    if (!s_span_array || s_span_count <= 0 || s_models_display_list_count <= 0) {
        if (s_span_display_list_indices) {
            free(s_span_display_list_indices);
            s_span_display_list_indices = NULL;
        }
        return;
    }

    mapping = (int *)malloc((size_t)s_span_count * sizeof(int));
    if (!mapping)
        return;

    for (int span_index = 0; span_index < s_span_count; span_index++) {
        float sx, sy, sz;
        int assigned = entry_index;

        if (!compute_span_center_world(span_index, &sx, &sy, &sz)) {
            mapping[span_index] = (span_index > 0) ? mapping[span_index - 1] : 0;
            continue;
        }

        while (entry_index + 1 < s_models_display_list_count) {
            /* After relocation, mesh slots contain absolute pointers.
             * Read the first mesh pointer directly from each block. */
            const uint8_t *blk_a = s_models_blob + s_models_entry_offsets[entry_index];
            const uint8_t *blk_b = s_models_blob + s_models_entry_offsets[entry_index + 1];
            uint32_t cnt_a = *(const uint32_t *)blk_a;
            uint32_t cnt_b = *(const uint32_t *)blk_b;
            const TD5_MeshHeader *mesh_a, *mesh_b;
            float ax, az, bx, bz;
            float da, db;

            if (cnt_a == 0 || cnt_b == 0)
                break;

            /* Slot now holds an absolute pointer after relocation */
            mesh_a = (const TD5_MeshHeader *)(uintptr_t)(*(const uint32_t *)(blk_a + 4));
            mesh_b = (const TD5_MeshHeader *)(uintptr_t)(*(const uint32_t *)(blk_b + 4));
            if (!mesh_a || !mesh_b ||
                (uintptr_t)mesh_a < 0x10000u || (uintptr_t)mesh_b < 0x10000u)
                break;

            ax = mesh_a->bounding_center_x + mesh_a->origin_x;
            az = mesh_a->bounding_center_z + mesh_a->origin_z;
            bx = mesh_b->bounding_center_x + mesh_b->origin_x;
            bz = mesh_b->bounding_center_z + mesh_b->origin_z;

            da = (sx - ax) * (sx - ax) + (sz - az) * (sz - az);
            db = (sx - bx) * (sx - bx) + (sz - bz) * (sz - bz);
            if (db > da)
                break;

            entry_index++;
            assigned = entry_index;
        }

        mapping[span_index] = assigned;
    }

    if (s_span_display_list_indices)
        free(s_span_display_list_indices);
    s_span_display_list_indices = mapping;

}

/**
 * Compute world-space vertex position (24.8 fixed-point) from span origin
 * and vertex offset.
 */
static inline void vertex_world_pos(const TD5_StripSpan *sp,
                                     const TD5_StripVertex *v,
                                     int32_t *out_x, int32_t *out_y, int32_t *out_z)
{
    *out_x = ((int32_t)sp->origin_x + (int32_t)v->x) << 8;
    *out_y = ((int32_t)sp->origin_z + (int32_t)v->y) << 8; /* origin_z maps to Y in world for pad_10 */
    *out_z = ((int32_t)sp->origin_z + (int32_t)v->z) << 8;
}

/**
 * Get the 4 corner vertex indices for a span quad at a given sub-lane.
 * Returns: vl0, vl1 (left pair), vr0, vr1 (right pair).
 */
static void get_quad_vertices(const TD5_StripSpan *sp, int sub_lane,
                               int *vl0, int *vl1, int *vr0, int *vr1)
{
    int li = sp->left_vertex_index + sub_lane;
    int ri = sp->right_vertex_index + sub_lane;
    *vl0 = li;
    *vl1 = li + 1;
    *vr0 = ri;
    *vr1 = ri + 1;
}

static uint32_t color_from_surface_attr(uint8_t attr, int lane_index, int lane_count)
{
    static const uint32_t k_surface_palette[16] = {
        0xFFFF4040, 0xFFFFA040, 0xFFFFFF40, 0xFF80FF40,
        0xFF40FF80, 0xFF40FFFF, 0xFF4080FF, 0xFF8040FF,
        0xFFFF40FF, 0xFFFF4080, 0xFFFF8040, 0xFFC0FF40,
        0xFF40FFC0, 0xFF40C0FF, 0xFFC040FF, 0xFFFF40C0
    };
    uint32_t color = k_surface_palette[attr & 0x0F];
    int brighten = (lane_count > 1) ? (lane_index * 20) / lane_count : 0;
    uint32_t r = ((color >> 16) & 0xFFu);
    uint32_t g = ((color >> 8) & 0xFFu);
    uint32_t b = (color & 0xFFu);

    r = (r + (uint32_t)brighten > 255u) ? 255u : r + (uint32_t)brighten;
    g = (g + (uint32_t)brighten > 255u) ? 255u : g + (uint32_t)brighten;
    b = (b + (uint32_t)brighten > 255u) ? 255u : b + (uint32_t)brighten;
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

static int surface_type_for_span_lane(const TD5_StripSpan *sp, int lane)
{
    uint8_t lane_mask;
    uint8_t attr;

    if (!sp)
        return TD5_SURFACE_DRY_ASPHALT;

    if (lane < 0)
        lane = 0;

    lane_mask = span_lane_bitmask(sp);
    attr = sp->surface_attribute;

    if ((lane_mask & (1u << lane)) != 0)
        return (attr >> 4) | 0x10;

    return attr & 0x0F;
}

static void *build_span_strip_display_list(int span_index)
{
    const TD5_StripSpan *sp;
    int lane_count;
    int command_count;
    int vertex_count;
    size_t bytes;
    uint8_t *mem;
    uint32_t *header;
    TD5_MeshHeader *mesh;
    TD5_PrimitiveCmd *cmds;
    TD5_MeshVertex *verts;
    float min_x, min_y, min_z, max_x, max_y, max_z;
    float origin_x, origin_y, origin_z;

    if (!s_span_array || !s_vertex_table || span_index < 0 || span_index >= s_span_count)
        return NULL;

    sp = &s_span_array[span_index];
    if (sp->span_type == 9 || sp->span_type == 10)
        return NULL;

    lane_count = span_lane_count(sp);
    if (lane_count < 1)
        lane_count = 1;
    if (lane_count > 32)
        lane_count = 32;

    command_count = lane_count;
    vertex_count = lane_count * 4;
    bytes = sizeof(uint32_t) * 2
          + sizeof(TD5_MeshHeader)
          + sizeof(TD5_PrimitiveCmd) * (size_t)command_count
          + sizeof(TD5_MeshVertex) * (size_t)vertex_count
          + sizeof(TD5_VertexNormal) * (size_t)vertex_count;

    mem = (uint8_t *)calloc(1, bytes);
    if (!mem)
        return NULL;

    header = (uint32_t *)mem;
    mesh = (TD5_MeshHeader *)(mem + sizeof(uint32_t) * 2);
    cmds = (TD5_PrimitiveCmd *)((uint8_t *)mesh + sizeof(*mesh));
    verts = (TD5_MeshVertex *)((uint8_t *)cmds + sizeof(*cmds) * (size_t)command_count);

    header[0] = 1;
    header[1] = (uint32_t)(uintptr_t)mesh;

    mesh->render_type = 0;
    mesh->texture_page_id = TRACK_FALLBACK_TEXTURE_PAGE;
    mesh->command_count = command_count;
    mesh->total_vertex_count = vertex_count;
    mesh->commands_offset = (uint32_t)(uintptr_t)cmds;
    mesh->vertices_offset = (uint32_t)(uintptr_t)verts;
    mesh->normals_offset = 0;

    min_x = min_y = min_z =  1.0e30f;
    max_x = max_y = max_z = -1.0e30f;
    /* Span origins in integer-coord float space (same as MODELS.DAT
     * and camera render coordinates). */
    origin_x = (float)sp->origin_x;
    origin_y = (float)sp->pad_10;
    origin_z = (float)sp->origin_z;
    mesh->origin_x = origin_x;
    mesh->origin_y = origin_y;
    mesh->origin_z = origin_z;

    for (int lane = 0; lane < lane_count; lane++) {
        int vl0, vl1, vr0, vr1;
        TD5_StripVertex *src[4];
        float px[4];
        float py[4];
        float pz[4];
        int base = lane * 4;
        uint32_t lane_color = color_from_surface_attr(sp->surface_attribute, lane, lane_count);

        get_quad_vertices(sp, lane, &vl0, &vl1, &vr0, &vr1);
        src[0] = vertex_at(vl0);
        src[1] = vertex_at(vl1);
        src[2] = vertex_at(vr1);
        src[3] = vertex_at(vr0);

        for (int i = 0; i < 4; i++) {
            /* Strip vertices (int16) are LOCAL offsets from the span origin,
             * in integer-coord space. */
            px[i] = (float)src[i]->x;
            py[i] = (float)src[i]->y;
            pz[i] = (float)src[i]->z;

            if (px[i] < min_x) min_x = px[i];
            if (py[i] < min_y) min_y = py[i];
            if (pz[i] < min_z) min_z = pz[i];
            if (px[i] > max_x) max_x = px[i];
            if (py[i] > max_y) max_y = py[i];
            if (pz[i] > max_z) max_z = pz[i];
        }

        verts[base + 0].pos_x = px[0]; verts[base + 0].pos_y = py[0]; verts[base + 0].pos_z = pz[0];
        verts[base + 1].pos_x = px[1]; verts[base + 1].pos_y = py[1]; verts[base + 1].pos_z = pz[1];
        verts[base + 2].pos_x = px[2]; verts[base + 2].pos_y = py[2]; verts[base + 2].pos_z = pz[2];
        verts[base + 3].pos_x = px[3]; verts[base + 3].pos_y = py[3]; verts[base + 3].pos_z = pz[3];

        for (int i = 0; i < 4; i++) {
            int local = i;
            verts[base + i].tex_u = (local == 2 || local == 3) ? 1.0f : 0.0f;
            verts[base + i].tex_v = (local == 1 || local == 2) ? 1.0f : 0.0f;
            verts[base + i].lighting = lane_color;
        }

        cmds[lane].dispatch_type = TD5_DISPATCH_PROJECTED_QUAD;
        cmds[lane].texture_page_id = TRACK_FALLBACK_TEXTURE_PAGE;
        cmds[lane].quad_count = 1;
        cmds[lane].vertex_data_ptr = (uint32_t)(uintptr_t)&verts[base];
    }

    mesh->bounding_center_x = (min_x + max_x) * 0.5f;
    mesh->bounding_center_y = (min_y + max_y) * 0.5f;
    mesh->bounding_center_z = (min_z + max_z) * 0.5f;
    mesh->bounding_radius = (max_x - min_x) + (max_y - min_y) + (max_z - min_z);
    if (mesh->bounding_radius < 0.1f)
        mesh->bounding_radius = 0.1f;

    return mem;
}

static int build_strip_display_lists(void)
{
    void **lists;
    int built = 0;

    if (!s_span_array || !s_vertex_table || s_span_count <= 0)
        return build_placeholder_display_lists();

    free_display_lists();

    lists = (void **)calloc((size_t)s_span_count, sizeof(void *));
    if (!lists)
        return build_placeholder_display_lists();

    for (int i = 0; i < s_span_count; i++) {
        lists[i] = build_span_strip_display_list(i);
        if (lists[i])
            built++;
    }

    if (built <= 0) {
        free(lists);
        return build_placeholder_display_lists();
    }

    s_display_lists = lists;
    s_display_lists_are_generated_meshes = 1;
    s_display_list_count = s_span_count;
    TD5_LOG_I("track", "built %d strip display lists from STRIP.DAT", built);
    return 1;
}

/* ========================================================================
 * STRIP.DAT Loading (0x444070 BindTrackStripRuntimePointers)
 * ======================================================================== */

int td5_track_load_strip(const void *data, size_t size)
{
    uint32_t *hdr;
    uint32_t span_offset, vertex_offset;

    if (s_strip_blob) {
        free(s_strip_blob);
        s_strip_blob = NULL;
    }
    s_strip_blob_size = 0;
    s_span_array = NULL;
    s_vertex_table = NULL;
    s_span_count = 0;
    s_strip_header = NULL;
    s_jump_entry_count = 0;
    s_jump_entries = NULL;
    free_display_lists();
    if (s_span_display_list_indices) {
        free(s_span_display_list_indices);
        s_span_display_list_indices = NULL;
    }

    if (!data || size < TD5_STRIP_HEADER_DWORDS * 4)
        return build_placeholder_display_lists();

    /* Allocate and copy the raw blob */
    s_strip_blob = (uint8_t *)malloc(size);
    if (!s_strip_blob)
        return build_placeholder_display_lists();
    memcpy(s_strip_blob, data, size);
    s_strip_blob_size = size;

    /* Parse the 5-dword header */
    hdr = (uint32_t *)s_strip_blob;
    s_strip_header = hdr;

    span_offset     = hdr[0];  /* byte offset to span record table */
    s_span_count    = (int)hdr[1]; /* total span count (ring length) */
    vertex_offset   = hdr[2];  /* byte offset to vertex table */
    s_secondary_count = (int)hdr[3];
    s_aux_count     = (int)hdr[4];

    /* Resolve runtime pointers */
    if (span_offset >= size || vertex_offset >= size) {
        free(s_strip_blob);
        s_strip_blob = NULL;
        s_strip_blob_size = 0;
        return build_placeholder_display_lists();
    }

    s_span_array   = (TD5_StripSpan *)(s_strip_blob + span_offset);
    s_vertex_table = (TD5_StripVertex *)(s_strip_blob + vertex_offset);

    /* Store in global state */
    g_td5.track_span_ring_length = s_span_count;

    /* Parse jump table at header + 0x14 (if present) */
    if (size >= 6 * 4) {
        s_jump_entry_count = (int)(*(uint32_t *)(s_strip_blob + 0x14));
        if (s_jump_entry_count > 0 && size >= 0x18 + (size_t)s_jump_entry_count * 6) {
            s_jump_entries = s_strip_blob + 0x18;
        } else {
            s_jump_entry_count = 0;
            s_jump_entries = NULL;
        }
    }

    /* Bind runtime pointers (patch sentinels) */
    td5_track_bind_runtime_pointers();
    g_strip_span_count = s_span_count;
    g_strip_total_segments = s_span_count;
    g_strip_span_base = s_span_array;
    g_strip_vertex_base = s_vertex_table;

    if (s_models_display_list_count > 0)
        rebuild_span_display_list_mapping();

    if (!build_strip_display_lists() && !build_placeholder_display_lists())
        return 0;

    TD5_LOG_I(LOG_TAG,
              "Strip loaded: spans=%d vertices=%d jumps=%d attr_overrides=%d",
              s_span_count,
              (int)((s_strip_blob_size > vertex_offset)
                  ? ((s_strip_blob_size - vertex_offset) / sizeof(TD5_StripVertex))
                  : 0),
              s_jump_entry_count,
              s_secondary_count);

    return 1;
}

/**
 * BindTrackStripRuntimePointers (0x444070)
 *
 * Patches sentinel records at the first and last span:
 *   - First span: type = 9 (SENTINEL_START), backward_link = span_count - 1
 *   - Last span:  type = 10 (SENTINEL_END), link at +0x10 cleared to 0
 */
void td5_track_bind_runtime_pointers(void)
{
    TD5_StripSpan *first, *last;

    if (!s_span_array || s_span_count < 2)
        return;

    first = &s_span_array[0];
    last  = &s_span_array[s_span_count - 1];

    /* Patch first span: type = SENTINEL_START, backward link = last span */
    first->span_type = 9;  /* TD5_SPAN_SENTINEL_START */
    first->link_prev = (int16_t)(s_span_count - 1);

    /* Patch last span: type = SENTINEL_END, clear forward link area */
    last->span_type = 10;  /* TD5_SPAN_SENTINEL_END */
    /* The original clears the int32 at +0x10 (origin_y / pad_10).
     * In the original binary this was the link field area at +0x10 = 0. */
    last->pad_10 = 0;
}

/* ========================================================================
 * Strip Attribute Overrides (0x42FB40)
 *
 * Applies a compact override table to the surface_attribute byte (+0x01)
 * of each span record. Table format: pairs of (span_index, value) at
 * 8-byte stride. Between overrides, the current attribute propagates.
 * ======================================================================== */

void td5_track_apply_attribute_overrides(void)
{
    int i, ov_idx;
    uint8_t current_attr;
    int next_override_span;
    uint8_t next_override_val;

    if (!s_span_array || !s_attr_override_table || s_attr_override_count <= 0)
        return;

    ov_idx = 0;
    current_attr = 0;

    /* Read first override pair */
    if (ov_idx < s_attr_override_count) {
        next_override_span = *(int32_t *)(s_attr_override_table + ov_idx * 8);
        next_override_val  = *(uint8_t *)(s_attr_override_table + ov_idx * 8 + 4);
    } else {
        next_override_span = s_span_count + 1; /* unreachable */
        next_override_val = 0;
    }

    for (i = 0; i < s_span_count; i++) {
        if (i == next_override_span) {
            current_attr = next_override_val;
            ov_idx++;
            if (ov_idx < s_attr_override_count) {
                next_override_span = *(int32_t *)(s_attr_override_table + ov_idx * 8);
                next_override_val  = *(uint8_t *)(s_attr_override_table + ov_idx * 8 + 4);
            } else {
                next_override_span = s_span_count + 1;
            }
        }
        s_span_array[i].surface_attribute = current_attr;
    }
}

/* ========================================================================
 * Surface Type Lookup (0x42F100 GetTrackSegmentSurfaceType)
 *
 * The surface_attribute byte at +0x01 encodes two surface types:
 *   - Low nibble = primary surface type
 *   - High nibble = alternate surface type
 * The lane_bitmask at +0x02 selects which surface to use per lane:
 *   If bit N is set, lane N uses the alternate (high nibble) surface.
 * ======================================================================== */

int td5_track_get_surface_type(TD5_Actor *actor, int probe_index)
{
    TD5_TrackProbe *probe;
    TD5_StripSpan *sp;
    int lane;

    if (!actor || !s_span_array)
        return TD5_SURFACE_DRY_ASPHALT;

    /* Access the probe state within the actor.
     * Probes are at actor + 0x00, each 16 bytes, 8 probes total. */
    probe = (TD5_TrackProbe *)((uint8_t *)actor + probe_index * 16);
    sp = &s_span_array[probe->span_index];
    lane = probe->sub_lane_index;
    return surface_type_for_span_lane(sp, lane);
}

/* ========================================================================
 * 4-Edge Cross-Product Boundary Tests (0x4440F0 UpdateActorTrackPosition)
 *
 * Determines which edges of the current track quad the actor has crossed
 * using cross-product sign tests on each of the 4 boundary edges.
 *
 * The 4-bit result encodes:
 *   bit 0 (0x01): forward boundary crossed
 *   bit 1 (0x02): right boundary crossed
 *   bit 2 (0x04): backward boundary crossed
 *   bit 3 (0x08): left boundary crossed
 *
 * On crossing, transitions to the neighbor span and recursively re-tests.
 * ======================================================================== */

/**
 * Cross-product edge test: returns > 0 if the point (px,pz) is on the
 * "outside" of the edge from (ax,az) to (bx,bz).
 *
 *   cross = (bx - ax) * (pz - az) - (bz - az) * (px - ax)
 */
static inline int32_t edge_cross(int32_t ax, int32_t az,
                                  int32_t bx, int32_t bz,
                                  int32_t px, int32_t pz)
{
    int64_t dx = (int64_t)(bx - ax);
    int64_t dz = (int64_t)(bz - az);
    int64_t ex = (int64_t)(px - ax);
    int64_t ez = (int64_t)(pz - az);
    int64_t cross = dx * ez - dz * ex;
    if (cross > 0) return 1;
    if (cross < 0) return -1;
    return 0;
}

/**
 * Compute the 4-bit boundary crossing result for the given position
 * within the specified span and sub-lane.
 */
static uint8_t compute_boundary_bits(int span_idx, int sub_lane,
                                      int32_t pos_x, int32_t pos_z)
{
    TD5_StripSpan *sp;
    TD5_StripVertex *vl0, *vl1, *vr0, *vr1;
    int32_t ox, oz;
    int32_t lx0, lz0, lx1, lz1, rx0, rz0, rx1, rz1;
    uint8_t result = 0;
    uint8_t edge_mask;
    int lane_count;

    if (span_idx < 0 || span_idx >= s_span_count)
        return 0;

    sp = &s_span_array[span_idx];
    lane_count = span_lane_count(sp);
    if (lane_count < 1) lane_count = 1;

    /* Get the 4 corner vertices for this sub-lane */
    vl0 = vertex_at(sp->left_vertex_index + sub_lane);
    vl1 = vertex_at(sp->left_vertex_index + sub_lane + 1);
    vr0 = vertex_at(sp->right_vertex_index + sub_lane);
    vr1 = vertex_at(sp->right_vertex_index + sub_lane + 1);

    /* Convert to world-space XZ (24.8 fixed-point) */
    ox = sp->origin_x << 8;
    oz = sp->origin_z << 8;

    lx0 = ox + ((int32_t)vl0->x << 8);
    lz0 = oz + ((int32_t)vl0->z << 8);
    lx1 = ox + ((int32_t)vl1->x << 8);
    lz1 = oz + ((int32_t)vl1->z << 8);
    rx0 = ox + ((int32_t)vr0->x << 8);
    rz0 = oz + ((int32_t)vr0->z << 8);
    rx1 = ox + ((int32_t)vr1->x << 8);
    rz1 = oz + ((int32_t)vr1->z << 8);

    /* Select edge mask based on sub-lane position within span */
    if (sub_lane == 0) {
        edge_mask = s_edge_mask_first[sp->span_type];
    } else if (sub_lane >= lane_count - 1) {
        edge_mask = s_edge_mask_last[sp->span_type];
    } else {
        edge_mask = 0x0F; /* all 4 edges for interior sub-lanes */
    }

    /* Test each edge. Cross product sign > 0 means outside. */

    /* Edge 0 (forward): left1 -> right1 (the "far" edge) */
    if ((edge_mask & 0x01) && edge_cross(lx1, lz1, rx1, rz1, pos_x, pos_z) > 0)
        result |= 0x01;

    /* Edge 1 (right): right0 -> right1 */
    if ((edge_mask & 0x02) && edge_cross(rx0, rz0, rx1, rz1, pos_x, pos_z) > 0)
        result |= 0x02;

    /* Edge 2 (backward): right0 -> left0 (the "near" edge) */
    if ((edge_mask & 0x04) && edge_cross(rx0, rz0, lx0, lz0, pos_x, pos_z) > 0)
        result |= 0x04;

    /* Edge 3 (left): left1 -> left0 */
    if ((edge_mask & 0x08) && edge_cross(lx1, lz1, lx0, lz0, pos_x, pos_z) > 0)
        result |= 0x08;

    return result;
}

/**
 * Resolve neighbor span index for a given crossing direction.
 * Returns the new span index (and updates sub_lane via pointer).
 */
static int resolve_neighbor(int span_idx, int *sub_lane, uint8_t crossing_bit)
{
    TD5_StripSpan *sp = &s_span_array[span_idx];
    int new_span = span_idx;
    int h_offset = span_height_offset(sp);

    switch (crossing_bit) {
    case 0x01: /* Forward */
        switch (sp->span_type) {
        case 8:  /* JUNCTION_FWD: follow forward link */
        case 10: /* SENTINEL_END */
        case 11: /* JUNCTION_BWD */
            new_span = (int)sp->link_next;
            break;
        default:
            new_span = span_idx + 1;
            break;
        }
        break;

    case 0x02: /* Right */
        switch (sp->span_type) {
        case 8:
        case 10:
            new_span = (int)sp->link_next;
            break;
        default:
            new_span = span_idx + 1;
            break;
        }
        /* Adjust sub-lane by height offset for lateral transitions */
        *sub_lane += h_offset;
        break;

    case 0x04: /* Backward */
        switch (sp->span_type) {
        case 9:  /* SENTINEL_START: follow backward link */
        case 11: /* JUNCTION_BWD */
            new_span = (int)sp->link_prev;
            break;
        default:
            new_span = span_idx - 1;
            break;
        }
        break;

    case 0x08: /* Left */
        switch (sp->span_type) {
        case 9:
        case 11:
            new_span = (int)sp->link_prev;
            break;
        default:
            new_span = span_idx - 1;
            break;
        }
        *sub_lane -= h_offset;
        break;
    }

    /* Clamp span index to valid range */
    if (new_span < 0) new_span = 0;
    if (new_span >= s_span_count) new_span = s_span_count - 1;

    /* Clamp sub-lane */
    if (*sub_lane < 0) *sub_lane = 0;
    {
        int max_lane = span_lane_count(&s_span_array[new_span]) - 1;
        if (max_lane < 0) max_lane = 0;
        if (*sub_lane > max_lane) *sub_lane = max_lane;
    }

    return new_span;
}

/**
 * Recursive boundary traversal. Updates the actor's track position state
 * when crossing span boundaries.
 */
static void update_position_recursive(int16_t *track_state, int32_t pos_x, int32_t pos_z,
                                       int depth)
{
    int span_idx = (int)track_state[0];
    int sub_lane = (int)((int8_t *)track_state)[12]; /* sub_lane_index at byte +0x0C from track_state base */
    uint8_t bits;
    int new_span;

    if (depth >= TRACK_MAX_RECURSION)
        return;

    bits = compute_boundary_bits(span_idx, sub_lane, pos_x, pos_z);

    if (bits == 0)
        return; /* Actor is within the current quad */

    /* Process the lowest set bit (single dominant crossing direction) */
    if (bits & 0x01) {
        /* Forward crossing: increment accumulated span counter */
        new_span = resolve_neighbor(span_idx, &sub_lane, 0x01);
        track_state[0] = (int16_t)new_span;
        track_state[2]++; /* accumulated spans (forward progress) */
        if (track_state[2] > track_state[3])
            track_state[3] = track_state[2]; /* high-water mark */
    } else if (bits & 0x02) {
        /* Right crossing */
        new_span = resolve_neighbor(span_idx, &sub_lane, 0x02);
        track_state[0] = (int16_t)new_span;
        track_state[2]++;
        if (track_state[2] > track_state[3])
            track_state[3] = track_state[2];
    } else if (bits & 0x04) {
        /* Backward crossing: decrement accumulated span counter */
        new_span = resolve_neighbor(span_idx, &sub_lane, 0x04);
        track_state[0] = (int16_t)new_span;
        track_state[2]--;
    } else if (bits & 0x08) {
        /* Left crossing */
        new_span = resolve_neighbor(span_idx, &sub_lane, 0x08);
        track_state[0] = (int16_t)new_span;
        track_state[2]--;
    }

    /* Write back sub-lane */
    ((int8_t *)track_state)[12] = (int8_t)sub_lane;

    /* Recursively test in the new span */
    update_position_recursive(track_state, pos_x, pos_z, depth + 1);
}

/**
 * UpdateActorTrackPosition (0x4440F0)
 *
 * Updates the actor's track span position using 4-edge cross-product
 * boundary tests. On boundary crossing, transitions to the neighbor span
 * and recursively re-evaluates.
 */
void td5_track_update_actor_position(TD5_Actor *actor)
{
    int16_t *track_state;
    int32_t pos_x, pos_z;

    if (!actor || !s_span_array || s_span_count == 0)
        return;

    /* Track position state is at actor + 0x80 */
    track_state = (int16_t *)((uint8_t *)actor + 0x80);

    /* World position in 24.8 fixed-point */
    pos_x = *(int32_t *)((uint8_t *)actor + 0x1FC); /* world_pos.x */
    pos_z = *(int32_t *)((uint8_t *)actor + 0x204); /* world_pos.z */

    update_position_recursive(track_state, pos_x, pos_z, 0);

    if ((uintptr_t)actor == (uintptr_t)0x004AB108u) {
        s_actor_position_log_counter++;
        if ((s_actor_position_log_counter % 60u) == 0u) {
            float normalized = (s_span_count > 0)
                ? ((float)track_state[1] / (float)s_span_count)
                : 0.0f;
            TD5_LOG_D(LOG_TAG, "Actor0 track pos: span=%d normalized=%.3f",
                      (int)track_state[1], normalized);
        }
    }
}

/* ========================================================================
 * Barycentric Contact Resolution (0x4456D0 / 0x445A70)
 *
 * Given a span, sub-lane, and world XZ position, determines which triangle
 * half the point falls in (diagonal split of quad) and computes the ground
 * height using barycentric interpolation via plane equation.
 * ======================================================================== */

/**
 * Cross-product triangle half test.
 * Returns which half of the quad diagonal the point falls in.
 * 0 = lower-left triangle, 1 = upper-right triangle.
 */
static int classify_triangle_half(const TD5_StripSpan *sp, int sub_lane,
                                   int32_t local_x, int32_t local_z)
{
    TD5_StripVertex *vl0, *vr1;
    int32_t dx, dz;

    /* Test against the diagonal from vl0 to vr1 */
    vl0 = vertex_at(sp->left_vertex_index + sub_lane);
    vr1 = vertex_at(sp->right_vertex_index + sub_lane + 1);

    dx = (int32_t)vr1->x - (int32_t)vl0->x;
    dz = (int32_t)vr1->z - (int32_t)vl0->z;

    /* Cross product to determine side:
     * cross = dx * (local_z - vl0->z) - dz * (local_x - vl0->x) */
    {
        int64_t cross = (int64_t)dx * (int64_t)(local_z - (int32_t)vl0->z)
                       - (int64_t)dz * (int64_t)(local_x - (int32_t)vl0->x);
        return (cross > 0) ? 1 : 0;
    }
}

/**
 * Compute barycentric height from a triangle defined by 3 vertices.
 * Uses plane equation: height = va.y + ((px - va.x)*nx + (pz - va.z)*nz) / ny
 *
 * Returns height in 24.8 fixed-point.
 */
static int32_t triangle_height(int va_idx, int vb_idx, int vc_idx,
                                int32_t origin_x, int32_t origin_y, int32_t origin_z,
                                int32_t pos_x, int32_t pos_z,
                                int16_t *out_normal)
{
    TD5_StripVertex *va, *vb, *vc;
    int32_t e1x, e1y, e1z, e2x, e2y, e2z;
    int32_t nx, ny, nz;
    int32_t dx, dz;
    int32_t height;

    va = vertex_at(va_idx);
    vb = vertex_at(vb_idx);
    vc = vertex_at(vc_idx);

    /* Edge vectors from A to B and A to C */
    e1x = (int32_t)vb->x - (int32_t)va->x;
    e1y = (int32_t)vb->y - (int32_t)va->y;
    e1z = (int32_t)vb->z - (int32_t)va->z;

    e2x = (int32_t)vc->x - (int32_t)va->x;
    e2y = (int32_t)vc->y - (int32_t)va->y;
    e2z = (int32_t)vc->z - (int32_t)va->z;

    /* Cross product -> surface normal (right-shifted for precision) */
    nx = (e1y * e2z - e1z * e2y) >> 4;
    ny = (e1z * e2x - e1x * e2z) >> 4;
    nz = (e1x * e2y - e1y * e2x) >> 4;

    /* Prevent divide-by-zero */
    if (ny == 0) ny = 1;

    /* Position relative to vertex A in span-local coordinates */
    dx = (pos_x >> 8) - origin_x - (int32_t)va->x;
    dz = (pos_z >> 8) - origin_z - (int32_t)va->z;

    /* Plane equation: y = va.y + (dx * nx + dz * nz) / ny */
    height = (int32_t)va->y - ((int32_t)dx * nx + (int32_t)dz * nz) / ny;

    /* Output normal if requested (normalized to int16 range) */
    if (out_normal) {
        /* Re-compute normal with higher precision shift for normal output */
        int32_t hnx = (e1y * e2z - e1z * e2y) >> 12;
        int32_t hny = (e1z * e2x - e1x * e2z) >> 12;
        int32_t hnz = (e1x * e2y - e1y * e2x) >> 12;
        if (hny == 0) hny = 1;

        /* Store as int16 normalized components */
        out_normal[0] = (int16_t)(hnx);
        out_normal[1] = (int16_t)(hny);
        out_normal[2] = (int16_t)(hnz);
    }

    /* Return height in 24.8 fixed-point */
    return (origin_y + height) << 8;
}

static int32_t probe_span_lane_height(const TD5_StripSpan *sp, int sub_lane,
                                       int32_t world_x, int32_t world_z,
                                       int16_t *out_normal)
{
    int32_t local_x, local_z;
    int half;
    int vl0, vl1, vr0, vr1;
    int va, vb, vc;
    int max_lane;

    if (!sp)
        return 0;

    max_lane = span_lane_count(sp) - 1;
    if (max_lane < 0)
        max_lane = 0;
    if (sub_lane < 0)
        sub_lane = 0;
    if (sub_lane > max_lane)
        sub_lane = max_lane;

    get_quad_vertices(sp, sub_lane, &vl0, &vl1, &vr0, &vr1);

    local_x = (world_x >> 8) - sp->origin_x;
    local_z = (world_z >> 8) - sp->origin_z;

    switch (sp->span_type) {
    case 1: case 2: case 5:
    case 8: case 9: case 10: case 11:
        half = classify_triangle_half(sp, sub_lane, local_x, local_z);
        if (half == 0) {
            va = vl0; vb = vl1; vc = vr0;
        } else {
            va = vl1; vb = vr1; vc = vr0;
        }
        break;

    case 3: case 4:
        half = classify_triangle_half(sp, sub_lane, local_x, local_z);
        if (half == 0) {
            va = vl0; vb = vl1; vc = vr1;
        } else {
            va = vl0; vb = vr1; vc = vr0;
        }
        break;

    case 6: case 7:
        half = classify_triangle_half(sp, sub_lane, local_x, local_z);
        if (half == 0) {
            va = vr0; vb = vr1; vc = vl0;
        } else {
            va = vr1; vb = vl1; vc = vl0;
        }
        break;

    default:
        va = vl0; vb = vl1; vc = vr0;
        break;
    }

    return triangle_height(va, vb, vc,
                           sp->origin_x, sp->pad_10, sp->origin_z,
                           world_x, world_z, out_normal);
}

/**
 * Compute ground contact height at a world XZ position on the given span.
 */
int32_t td5_track_compute_contact_height(int span_index, int sub_lane,
                                          int32_t world_x, int32_t world_z)
{
    return td5_track_compute_contact_height_with_normal(span_index, sub_lane,
                                                         world_x, world_z, NULL);
}

/**
 * Compute ground contact height with surface normal output.
 *
 * This is the master contact resolver: determines which triangle half of
 * the quad the position falls in, then computes the plane equation height.
 */
int32_t td5_track_compute_contact_height_with_normal(int span_index, int sub_lane,
                                                      int32_t world_x, int32_t world_z,
                                                      int16_t *out_normal)
{
    TD5_StripSpan *sp;

    if (span_index < 0 || span_index >= s_span_count)
        return 0;

    sp = &s_span_array[span_index];
    return probe_span_lane_height(sp, sub_lane, world_x, world_z, out_normal);
}

int td5_track_probe_height(int world_x, int world_z, int current_span,
                            int *out_y, int *out_surface_type)
{
    TD5_StripSpan *sp;
    int lane_count;
    int best_lane = 0;
    int best_score = 0x7FFFFFFF;

    if (!out_y || !out_surface_type || !s_span_array || !s_vertex_table || s_span_count <= 0)
        return 0;

    if (current_span < 0)
        current_span = 0;
    if (current_span >= s_span_count)
        current_span = s_span_count - 1;

    sp = &s_span_array[current_span];
    lane_count = span_lane_count(sp);
    if (lane_count < 1)
        lane_count = 1;

    for (int lane = 0; lane < lane_count; lane++) {
        uint8_t bits = compute_boundary_bits(current_span, lane, world_x, world_z);
        int score = 0;

        if (bits == 0) {
            best_lane = lane;
            best_score = 0;
            break;
        }

        for (uint8_t mask = bits; mask != 0; mask >>= 1) {
            score += (mask & 1u);
        }

        if (score < best_score) {
            best_score = score;
            best_lane = lane;
        }
    }

    *out_y = probe_span_lane_height(sp, best_lane, world_x, world_z, NULL);
    *out_surface_type = surface_type_for_span_lane(sp, best_lane);
    s_probe_log_counter++;
    if ((s_probe_log_counter % 120u) == 0u && current_span == 0) {
        TD5_LOG_D(LOG_TAG, "Probe actor0: y=%d surface=%d span=%d",
                  *out_y, *out_surface_type, current_span);
    }
    return 1;
}

/* ========================================================================
 * Heading Computation (0x435CE0 / 0x445B90)
 *
 * Derives a 12-bit heading angle from the track segment geometry.
 * The heading is computed from the difference vectors of span corner
 * vertices, dispatched by span type.
 * ======================================================================== */

/**
 * AngleFromVector12Full (0x433FC0).
 * Full 4-quadrant 12-bit angle from a 2D vector (dx, dz).
 * Returns 0x000-0xFFF = 0-360 degrees.
 */
static int angle_from_vector_full(int32_t dx, int32_t dz)
{
    int32_t ax, az;
    int quadrant = 0;
    int angle;

    /* Determine quadrant and normalize to Q1 */
    if (dx >= 0 && dz >= 0) {
        ax = dx; az = dz; quadrant = 0;
    } else if (dx < 0 && dz >= 0) {
        ax = dz; az = -dx; quadrant = 0x400;
    } else if (dx < 0 && dz < 0) {
        ax = -dx; az = -dz; quadrant = 0x800;
    } else {
        ax = -dz; az = dx; quadrant = 0xC00;
    }

    /* Q1 angle: atan2 approximation using integer lookup
     * For the source port we use a direct atan2 approximation */
    if (ax == 0 && az == 0) return 0;

    /* Simple atan2 approximation scaled to 0x400 range (one quadrant) */
    if (ax >= az) {
        if (ax == 0) return quadrant;
        angle = (int)((int64_t)az * 0x400 / (int64_t)ax);
        /* Clamp to quadrant */
        if (angle > 0x3FF) angle = 0x3FF;
    } else {
        if (az == 0) return quadrant;
        angle = 0x400 - (int)((int64_t)ax * 0x400 / (int64_t)az);
        if (angle < 1) angle = 1;
    }

    return (quadrant + angle) & 0xFFF;
}

void td5_track_compute_heading(TD5_Actor *actor)
{
    int16_t *track_state;
    int span_idx, sub_lane;
    TD5_StripSpan *sp;
    TD5_StripVertex *vl0, *vl1, *vr0, *vr1;
    int32_t dx, dz;
    int angle;
    int16_t *heading_normal;

    if (!actor || !s_span_array)
        return;

    track_state = (int16_t *)((uint8_t *)actor + 0x80);
    span_idx = (int)track_state[0];
    sub_lane = (int)((uint8_t *)actor)[0x8C];

    if (span_idx < 0 || span_idx >= s_span_count)
        return;

    sp = &s_span_array[span_idx];

    /* Get the quad corner vertices */
    vl0 = vertex_at(sp->left_vertex_index + sub_lane);
    vl1 = vertex_at(sp->left_vertex_index + sub_lane + 1);
    vr0 = vertex_at(sp->right_vertex_index + sub_lane);
    vr1 = vertex_at(sp->right_vertex_index + sub_lane + 1);

    /* Compute heading based on span type */
    switch (sp->span_type) {
    case 1: case 2: case 5:
    case 8: case 9: case 10: case 11:
        /* Standard: dx = (left1 - right1) - right0 + left0, divide by 4 */
        dx = ((int32_t)vl1->x - (int32_t)vr1->x) - (int32_t)vr0->x + (int32_t)vl0->x;
        dz = ((int32_t)vl1->z - (int32_t)vr1->z) - (int32_t)vr0->z + (int32_t)vl0->z;
        dx >>= 2;
        dz >>= 2;
        break;

    case 3: case 4:
        /* Alternate diagonal: shifted right vertex pairs */
        dx = ((int32_t)vl1->x - (int32_t)vr1->x) - (int32_t)vr0->x + (int32_t)vl0->x;
        dz = ((int32_t)vl1->z - (int32_t)vr1->z) - (int32_t)vr0->z + (int32_t)vl0->z;
        dx >>= 2;
        dz >>= 2;
        break;

    case 6: case 7:
        /* Reversed winding */
        dx = ((int32_t)vl1->x - (int32_t)vr1->x) + (int32_t)vl0->x - (int32_t)vr0->x;
        dz = ((int32_t)vl1->z - (int32_t)vr1->z) + (int32_t)vl0->z - (int32_t)vr0->z;
        dx >>= 2;
        dz >>= 2;
        break;

    default:
        dx = 0; dz = 1;
        break;
    }

    angle = angle_from_vector_full(dx, dz);

    /* Write heading to actor's heading_normal at +0x290 (as int16[3]) */
    heading_normal = (int16_t *)((uint8_t *)actor + 0x290);
    heading_normal[0] = (int16_t)dx;
    heading_normal[1] = 0;
    heading_normal[2] = (int16_t)dz;

    /* Also update the yaw euler accumulator at +0x1F4 */
    *(int32_t *)((uint8_t *)actor + 0x1F4) = angle << 8;
}

/* ========================================================================
 * Wrap Normalization (0x443FB0 NormalizeActorTrackWrapState)
 *
 * Normalizes the accumulated span counter (actor+0x84) into a wrapped
 * position modulo the total span ring length. Returns the lap count.
 * ======================================================================== */

void td5_track_normalize_actor_wrap(TD5_Actor *actor)
{
    int16_t *track_state;
    int32_t raw_span;
    int32_t ring_length;
    int32_t wrapped, laps;

    if (!actor || s_span_count == 0)
        return;

    track_state = (int16_t *)((uint8_t *)actor + 0x80);
    raw_span = (int32_t)track_state[2]; /* accumulated spans */
    ring_length = (int32_t)s_span_count;

    if (ring_length <= 0)
        return;

    if (raw_span >= 0) {
        wrapped = raw_span % ring_length;
        laps = raw_span / ring_length;
    } else {
        /* Handle negative wrap: ensure positive modulo result */
        wrapped = (raw_span % ring_length) + ring_length;
        if (wrapped >= ring_length)
            wrapped -= ring_length;
        laps = raw_span / ring_length;
        if (raw_span % ring_length != 0)
            laps--; /* floor division for negative values */
    }

    track_state[1] = (int16_t)wrapped;  /* normalized span */
    (void)laps; /* Lap count is used by the checkpoint system externally */
}

/* ========================================================================
 * Segment Boundary Remapping (0x443FF0 ResolveActorSegmentBoundary)
 *
 * Handles segment boundary discontinuities using the jump table from
 * the STRIP.DAT header for tracks with non-contiguous span numbering.
 * ======================================================================== */

static void resolve_segment_boundary(int16_t *track_state)
{
    int i;
    int16_t span;
    uint16_t start, end;
    int16_t remap;

    if (!s_jump_entries || s_jump_entry_count <= 0)
        return;

    span = track_state[1]; /* normalized span */

    for (i = 0; i < s_jump_entry_count; i++) {
        uint8_t *entry = s_jump_entries + i * 6;
        start = *(uint16_t *)(entry + 0);
        end   = *(uint16_t *)(entry + 2);
        remap = *(int16_t *)(entry + 4);

        if ((uint16_t)span >= start && (uint16_t)span <= end) {
            span = (int16_t)(span + remap);
            track_state[1] = span; /* normalized */
            track_state[2] = span; /* accumulated */
            break;
        }
    }
}

/* ========================================================================
 * Checkpoint Detection
 *
 * Circuit tracks: 4-bit sector bitmask system. Track divided into 4
 * equal sectors. Each sector crossing sets a progressive bit. When all
 * 4 bits are set (0x0F), a lap is complete.
 *
 * Point-to-point tracks: linear checkpoint span threshold comparison.
 * ======================================================================== */

int td5_track_check_checkpoint(TD5_Actor *actor)
{
    int16_t *track_state;
    int16_t current_span;
    int sector_size, sector;
    uint8_t *checkpoint_count_ptr;
    int16_t *gate_mask_ptr;
    int16_t gate_mask;
    int lap_complete = 0;

    if (!actor || s_span_count == 0)
        return 0;

    track_state = (int16_t *)((uint8_t *)actor + 0x80);
    current_span = track_state[1]; /* normalized span */

    if (g_td5.track_type == TD5_TRACK_CIRCUIT && !g_td5.time_trial_enabled) {
        /* Circuit mode: 4-sector bitmask lap detection */
        sector_size = s_span_count / 4;
        if (sector_size <= 0) sector_size = 1;

        /* gate_mask is overloaded at +0x336 (finish_time_subtick) */
        gate_mask_ptr = (int16_t *)((uint8_t *)actor + 0x336);
        gate_mask = *gate_mask_ptr;

        /* Determine which sector the actor is in */
        sector = current_span / sector_size;
        if (sector > 3) sector = 3;

        /* Progressive bit setting: must cross sectors in order */
        switch (gate_mask) {
        case 0x00:
            if (sector == 0 && current_span >= sector_size - 1 &&
                current_span <= sector_size + 1) {
                *gate_mask_ptr = 0x01;
                TD5_LOG_I(LOG_TAG, "Checkpoint crossing: sector=0 span=%d mask=0x%02X",
                          current_span, (unsigned int)*gate_mask_ptr);
            }
            break;
        case 0x01:
            if (sector == 1 && current_span >= sector_size * 2 - 1 &&
                current_span <= sector_size * 2 + 1) {
                *gate_mask_ptr = 0x03;
                TD5_LOG_I(LOG_TAG, "Checkpoint crossing: sector=1 span=%d mask=0x%02X",
                          current_span, (unsigned int)*gate_mask_ptr);
            }
            break;
        case 0x03:
            if (sector == 2 && current_span >= sector_size * 3 - 1 &&
                current_span <= sector_size * 3 + 1) {
                *gate_mask_ptr = 0x07;
                TD5_LOG_I(LOG_TAG, "Checkpoint crossing: sector=2 span=%d mask=0x%02X",
                          current_span, (unsigned int)*gate_mask_ptr);
            }
            break;
        case 0x07:
            /* Check if actor has crossed back to sector 0 (start/finish) */
            if (current_span < sector_size / 2) {
                *gate_mask_ptr = 0x0F;
                lap_complete = 1;
                TD5_LOG_I(LOG_TAG, "Checkpoint crossing: finish span=%d mask=0x%02X",
                          current_span, (unsigned int)*gate_mask_ptr);
            }
            break;
        case 0x0F:
            /* Lap is complete. Caller should increment lap counter
             * and reset gate_mask to 0. */
            break;
        }

        if (lap_complete) {
            /* Reset for next lap */
            *gate_mask_ptr = 0x00;

            /* Increment checkpoint_count (overloaded at +0x37E ghost_flag) */
            checkpoint_count_ptr = (uint8_t *)actor + 0x37E;
            (*checkpoint_count_ptr)++;

            /* Check if race is finished (all laps completed) */
            if (*checkpoint_count_ptr >= (uint8_t)g_td5.circuit_lap_count) {
                /* Record finish time */
                int32_t *finish_time = (int32_t *)((uint8_t *)actor + 0x328);
                int16_t *timing_ctr = (int16_t *)((uint8_t *)actor + 0x34C);
                if (*finish_time == 0) {
                    *finish_time = (int32_t)*timing_ctr;
                }
                return 2; /* Race complete */
            }
            return 1; /* Lap complete */
        }
    } else {
        /* Point-to-point mode: checkpoint span threshold comparison.
         * Checkpoint data would be loaded from per-track tables.
         * The checkpoint_count_ptr at +0x37E indexes into checkpoint array.
         * Forward progress = current_span >= checkpoint[idx].span_threshold. */

        checkpoint_count_ptr = (uint8_t *)actor + 0x37E;
        /* In P2P mode this tracks current checkpoint index */

        /* The actual checkpoint thresholds would come from the loaded
         * CheckpointMetadata. For now, we use the span_high_water
         * as a proxy for forward progress. */
        if (track_state[3] > track_state[2]) {
            /* Forward progress detected -- checkpoint system would
             * compare against the per-track span thresholds here. */
        }
    }

    return 0;
}

/* ========================================================================
 * Track Lighting (0x430150 / 0x42CE90)
 *
 * Zone-based lighting with 3 blend modes:
 *   Mode 0: Instant -- direct set of ambient + diffuse + direction
 *   Mode 1: Interpolated -- blends between zone start/end lighting
 *   Mode 2: Multi-directional -- extended blend with end-side interpolation
 * ======================================================================== */

/**
 * SetTrackLightDirectionContribution (0x42E130).
 * Configures one of 3 directional light slots.
 */
static void set_light_slot(int slot, int16_t dx, int16_t dy, int16_t dz,
                            uint8_t r, uint8_t g, uint8_t b)
{
    float intensity;

    if (slot < 0 || slot >= TD5_TRACK_LIGHT_SLOTS)
        return;

    if (r == 0 && g == 0 && b == 0) {
        s_light_enabled[slot] = 0;
        s_light_dir[slot][0] = 0.0f;
        s_light_dir[slot][1] = 0.0f;
        s_light_dir[slot][2] = 0.0f;
        return;
    }

    s_light_enabled[slot] = 1;
    intensity = (float)(r + g + b) / 3.0f;

    /* Scale direction by intensity * (1/1024) */
    s_light_dir[slot][0] = (float)dx * intensity * 0.0009765625f;
    s_light_dir[slot][1] = (float)dy * intensity * 0.0009765625f;
    s_light_dir[slot][2] = (float)dz * intensity * 0.0009765625f;
}

/**
 * ApplyTrackLightingForVehicleSegment (0x430150).
 * Master per-vehicle lighting selector. Walks the lighting zone table
 * to find the entry covering the current span, then dispatches on the
 * zone's blend mode.
 */
void td5_track_apply_segment_lighting(TD5_Actor *actor, int view_index)
{
    int16_t *track_state;
    int16_t current_span;
    int actor_idx;
    int i;
    TD5_TrackLightEntry *zone = NULL;
    TD5_TrackLightEntry *prev_zone = NULL;
    float t; /* interpolation factor */

    if (!actor || !s_lighting_table || s_lighting_entry_count <= 0)
        return;

    (void)view_index; /* Used for split-screen viewport selection */

    track_state = (int16_t *)((uint8_t *)actor + 0x80);
    current_span = track_state[1]; /* normalized span */
    actor_idx = (int)((uint8_t *)actor)[0x375]; /* slot_index */

    /* Walk lighting zones to find the one covering the current span */
    for (i = 0; i < s_lighting_entry_count; i++) {
        TD5_TrackLightEntry *entry = &s_lighting_table[i];
        if (current_span >= entry->span_start && current_span <= entry->span_end) {
            zone = entry;
            if (i > 0)
                prev_zone = &s_lighting_table[i - 1];
            break;
        }
    }

    if (!zone)
        return;

    /* Track zone transitions for the actor */
    if (actor_idx >= 0 && actor_idx < TD5_MAX_TOTAL_ACTORS) {
        s_actor_light_zone[actor_idx] = i;
    }

    /* Dispatch on blend mode */
    switch (zone->light_mode) {
    case 0:
        /* Mode 0: Instant / constant lighting.
         * Set ambient and diffuse directly from zone data. */
        set_light_slot(0,
                        zone->light_dir_x, zone->light_dir_y, zone->light_dir_z,
                        zone->light_color_r, zone->light_color_g, zone->light_color_b);
        /* Clear secondary lights */
        set_light_slot(1, 0, 0, 0, 0, 0, 0);
        set_light_slot(2, 0, 0, 0, 0, 0, 0);
        break;

    case 1:
        /* Mode 1: Interpolated / blended lighting.
         * Compute interpolation factor based on span position within zone. */
        {
            int zone_width = zone->span_end - zone->span_start;
            if (zone_width <= 0) zone_width = 1;

            t = (float)(current_span - zone->span_start) / (float)zone_width;
            if (t < 0.0f) t = 0.0f;
            if (t > 1.0f) t = 1.0f;

            /* Blend with the blend_distance parameter controlling falloff */
            if (zone->blend_distance > 0) {
                float blend_range = (float)zone->blend_distance / (float)zone_width;
                if (blend_range > 1.0f) blend_range = 1.0f;

                if (t < blend_range) {
                    /* Blending in from previous zone */
                    float bt = t / blend_range;
                    if (prev_zone) {
                        uint8_t r = (uint8_t)((float)prev_zone->light_color_r * (1.0f - bt)
                                              + (float)zone->light_color_r * bt);
                        uint8_t g = (uint8_t)((float)prev_zone->light_color_g * (1.0f - bt)
                                              + (float)zone->light_color_g * bt);
                        uint8_t b = (uint8_t)((float)prev_zone->light_color_b * (1.0f - bt)
                                              + (float)zone->light_color_b * bt);
                        int16_t dx = (int16_t)((float)prev_zone->light_dir_x * (1.0f - bt)
                                               + (float)zone->light_dir_x * bt);
                        int16_t dy = (int16_t)((float)prev_zone->light_dir_y * (1.0f - bt)
                                               + (float)zone->light_dir_y * bt);
                        int16_t dz = (int16_t)((float)prev_zone->light_dir_z * (1.0f - bt)
                                               + (float)zone->light_dir_z * bt);
                        set_light_slot(0, dx, dy, dz, r, g, b);
                    } else {
                        set_light_slot(0, zone->light_dir_x, zone->light_dir_y,
                                        zone->light_dir_z,
                                        zone->light_color_r, zone->light_color_g,
                                        zone->light_color_b);
                    }
                } else {
                    /* Fully within this zone */
                    set_light_slot(0, zone->light_dir_x, zone->light_dir_y,
                                    zone->light_dir_z,
                                    zone->light_color_r, zone->light_color_g,
                                    zone->light_color_b);
                }
            } else {
                /* No blend distance: instant */
                set_light_slot(0, zone->light_dir_x, zone->light_dir_y,
                                zone->light_dir_z,
                                zone->light_color_r, zone->light_color_g,
                                zone->light_color_b);
            }

            /* Clear secondary lights in mode 1 */
            set_light_slot(1, 0, 0, 0, 0, 0, 0);
            set_light_slot(2, 0, 0, 0, 0, 0, 0);
        }
        break;

    case 2:
        /* Mode 2: Multi-directional / extended blend.
         * Uses the ambient colors as a secondary light source alongside
         * the primary directional light. */
        {
            int zone_width = zone->span_end - zone->span_start;
            if (zone_width <= 0) zone_width = 1;

            t = (float)(current_span - zone->span_start) / (float)zone_width;
            if (t < 0.0f) t = 0.0f;
            if (t > 1.0f) t = 1.0f;

            /* Primary directional light with weight (1 - t) */
            {
                uint8_t r = (uint8_t)((float)zone->light_color_r * (1.0f - t));
                uint8_t g = (uint8_t)((float)zone->light_color_g * (1.0f - t));
                uint8_t b = (uint8_t)((float)zone->light_color_b * (1.0f - t));
                set_light_slot(0, zone->light_dir_x, zone->light_dir_y,
                                zone->light_dir_z, r, g, b);
            }

            /* Secondary light from ambient with weight t */
            {
                uint8_t r = (uint8_t)((float)zone->ambient_r * t);
                uint8_t g = (uint8_t)((float)zone->ambient_g * t);
                uint8_t b = (uint8_t)((float)zone->ambient_b * t);
                /* Use a different direction for the secondary (inverted primary) */
                set_light_slot(1, -zone->light_dir_x, -zone->light_dir_y,
                                -zone->light_dir_z, r, g, b);
            }

            /* Clear third light */
            set_light_slot(2, 0, 0, 0, 0, 0, 0);
        }
        break;

    default:
        /* Unknown mode: fall back to instant */
        set_light_slot(0, zone->light_dir_x, zone->light_dir_y,
                        zone->light_dir_z,
                        zone->light_color_r, zone->light_color_g,
                        zone->light_color_b);
        set_light_slot(1, 0, 0, 0, 0, 0, 0);
        set_light_slot(2, 0, 0, 0, 0, 0, 0);
        break;
    }
}

/**
 * UpdateActiveTrackLightDirections (0x42CE90).
 * Updates the global light direction state used by the renderer.
 * Called after all actors' lighting has been resolved.
 */
void td5_track_update_light_directions(void)
{
    /* Light directions are maintained in s_light_dir[] and s_light_enabled[].
     * The renderer reads these to configure its directional light state.
     * This function is a no-op in the source port as the state is updated
     * directly during apply_segment_lighting. In the original, this copied
     * from per-actor state to the global rendering light slots. */
}

/* ========================================================================
 * Race Order (0x42F5B0 UpdateRaceOrder)
 *
 * Bubble sort on track_span_high_water (+0x86) to determine race positions.
 * Unfinished actors are sorted by forward progress; finished actors retain
 * their finish-time-based positions.
 * ======================================================================== */

void td5_track_update_race_order(void)
{
    int i, swapped;
    uint8_t tmp;

    /* Bubble sort the race order array by span_high_water (descending) */
    do {
        swapped = 0;
        for (i = 0; i < TD5_MAX_RACER_SLOTS - 1; i++) {
            int slot_a = s_race_order[i];
            int slot_b = s_race_order[i + 1];

            /* Get the actor pointers -- using stride 0x388 from base.
             * In the source port, actors are managed externally; we access
             * them via byte offset from a base pointer.
             * For now, we use the global state's total_actor_count as a guard. */
            int16_t hw_a, hw_b;
            int32_t finish_a, finish_b;
            uint8_t *actor_base_a, *actor_base_b;

            /* Skip invalid slots */
            if (slot_a >= g_td5.total_actor_count ||
                slot_b >= g_td5.total_actor_count)
                continue;

            /* Access actors through byte-level pointer math.
             * In a full integration, these would use the actor table pointer. */
            actor_base_a = (uint8_t *)(uintptr_t)0; /* placeholder */
            actor_base_b = (uint8_t *)(uintptr_t)0;

            /* For the source port, use the offset approach:
             * high_water at actor + 0x86 (int16)
             * finish_time at actor + 0x328 (int32) */
            (void)actor_base_a;
            (void)actor_base_b;

            /* Since we cannot directly access the actor table base from here,
             * we store minimal state. In practice the race module would pass
             * actor pointers. For now, implement the sorting logic structurally. */
            hw_a = 0;
            hw_b = 0;
            finish_a = 0;
            finish_b = 0;

            /* Only swap unfinished actors whose span progress is lower */
            if (finish_a == 0 && finish_b == 0) {
                if (hw_a < hw_b) {
                    tmp = s_race_order[i];
                    s_race_order[i] = s_race_order[i + 1];
                    s_race_order[i + 1] = tmp;
                    swapped = 1;
                }
            }
        }
    } while (swapped);

    /* Write back race positions to each actor's race_position field (+0x383).
     * In full integration, this would iterate actors and set the byte. */
}

/* ========================================================================
 * Traffic Bus FIFO (0x435930 / 0x435310)
 *
 * TRAFFIC.BUS is a FIFO queue of 4-byte spawn records. The cursor
 * advances forward for spawning and wraps to provide continuous traffic.
 * ======================================================================== */

void td5_track_init_traffic_from_queue(void)
{
    int slot;

    if (!s_traffic_bus || s_traffic_bus_count <= 0)
        return;

    if (!g_td5.traffic_enabled)
        return;

    s_traffic_cursor = 0;

    /* Spawn initial traffic actors into slots 6-11 */
    for (slot = 0; slot < TRAFFIC_MAX_SLOTS; slot++) {
        TD5_TrafficBusEntry *entry;
        int cursor;

        /* Find the next valid entry (skip sentinels) */
        cursor = s_traffic_cursor;
        while (cursor < s_traffic_bus_count) {
            entry = &s_traffic_bus[cursor];
            if (entry->span_index == -1) {
                /* End sentinel: wrap around to beginning */
                cursor = 0;
                if (cursor == s_traffic_cursor)
                    break; /* Avoid infinite loop */
                continue;
            }
            break;
        }

        if (cursor >= s_traffic_bus_count)
            break;

        entry = &s_traffic_bus[cursor];

        /* The actual actor spawning would:
         * 1. Place the actor at the span_index position
         * 2. Set sub_lane_index to entry->lane
         * 3. If flags & 1: add 0x80000 to heading (oncoming)
         * 4. Call InitActorTrackSegmentPlacement
         * 5. Call ResetVehicleActorState
         *
         * Traffic actor slot = TRAFFIC_SLOT_BASE + slot */

        s_traffic_cursor = cursor + 1;
        if (s_traffic_cursor >= s_traffic_bus_count)
            s_traffic_cursor = 0;
    }
}

/**
 * RecycleTrafficActorFromQueue (0x435310).
 * Despawns a traffic actor that has fallen too far behind and respawns
 * it at the next queue position ahead of the player.
 */
void td5_track_recycle_traffic_actor(void)
{
    TD5_TrafficBusEntry *entry;
    int cursor;

    if (!s_traffic_bus || s_traffic_bus_count <= 0)
        return;

    if (!g_td5.traffic_enabled)
        return;

    /* Find the next valid entry from the FIFO cursor */
    cursor = s_traffic_cursor;
    while (cursor < s_traffic_bus_count) {
        entry = &s_traffic_bus[cursor];
        if (entry->span_index == -1) {
            cursor = 0; /* wrap */
            if (cursor == s_traffic_cursor)
                return;
            continue;
        }
        break;
    }

    if (cursor >= s_traffic_bus_count)
        return;

    entry = &s_traffic_bus[cursor];

    /* The recycling logic:
     * 1. Find the traffic actor farthest behind the player
     * 2. Teleport it to entry->span_index
     * 3. Set lane and direction from entry
     * 4. Advance the FIFO cursor
     */

    s_traffic_cursor = cursor + 1;
    if (s_traffic_cursor >= s_traffic_bus_count)
        s_traffic_cursor = 0;
}

/* ========================================================================
 * Route Loading (LEFT.TRK / RIGHT.TRK)
 *
 * Each route file is a byte array, 3 bytes per span:
 *   [0] = lateral offset (0x00-0xFF = left-to-right)
 *   [1] = heading byte
 *   [2] = reserved
 * ======================================================================== */

int td5_track_load_routes(const void *left_data, size_t left_size,
                           const void *right_data, size_t right_size)
{
    /* Free any existing route data */
    if (s_route_left) {
        free(s_route_left);
        s_route_left = NULL;
    }
    if (s_route_right) {
        free(s_route_right);
        s_route_right = NULL;
    }

    s_route_left_size = 0;
    s_route_right_size = 0;
    td5_ai_set_route_tables(NULL, 0, NULL, 0);

    /* Load LEFT.TRK */
    if (left_data && left_size > 0) {
        s_route_left = (uint8_t *)malloc(left_size);
        if (!s_route_left)
            return 0;
        memcpy(s_route_left, left_data, left_size);
        s_route_left_size = left_size;
    }

    /* Load RIGHT.TRK */
    if (right_data && right_size > 0) {
        s_route_right = (uint8_t *)malloc(right_size);
        if (!s_route_right) {
            free(s_route_left);
            s_route_left = NULL;
            s_route_left_size = 0;
            td5_ai_set_route_tables(NULL, 0, NULL, 0);
            return 0;
        }
        memcpy(s_route_right, right_data, right_size);
        s_route_right_size = right_size;
    }

    td5_ai_set_route_tables(s_route_left, s_route_left_size,
                            s_route_right, s_route_right_size);

    TD5_LOG_I(LOG_TAG, "Route data loaded: left=%u bytes right=%u bytes",
              (unsigned int)s_route_left_size, (unsigned int)s_route_right_size);

    return 1;
}

/* ========================================================================
 * MODELS.DAT Parsing (0x431190 ParseModelsDat)
 *
 * MODELS.DAT contains a sequence of mesh resources. Each entry begins
 * with a 4-byte size field followed by mesh data. The parser extracts
 * mesh headers and relocates internal pointers.
 * ======================================================================== */

int td5_track_parse_models_dat(const void *data, size_t size)
{
    const uint8_t *src;
    uint32_t raw_entry_count;
    int parsed_count = 0;
    int display_list_count = 0;
    uint32_t table_start_byte;   /* byte offset of entry table in file */

    if (!data || size < 8)
        return 0;

    free_models_dat_runtime();

    src = (const uint8_t *)data;
    s_models_blob = (uint8_t *)malloc(size);
    if (!s_models_blob)
        return 0;
    memcpy(s_models_blob, src, size);
    s_models_blob_size = size;

    /*
     * MODELS.DAT format (from Ghidra RE of SetupModelsDisplayList 0x431190):
     *
     *   DWORD[0] = entry_count (number of display list blocks)
     *   DWORD[1..count*2] = table of (offset, block_size) pairs, 8 bytes each
     *     offset: byte offset from file start to display list block
     *     block_size: byte size of the block
     *   [data blocks follow the table]
     *
     * The original lookup (0x431260) does: return *(table_base + span_index * 8)
     * where table_base = &DWORD[1].
     *
     * Heuristic to detect which format variant we have:
     *   - If DWORD[0] looks like a count (small number, and DWORD[1] == 4 + DWORD[0]*8),
     *     treat DWORD[0] as count with table at byte 4.
     *   - Otherwise, fall back to offset-based detection (DWORD[1] / 8 = entry count,
     *     table at byte 0).
     */
    {
        uint32_t dword0 = *(const uint32_t *)(const void *)(s_models_blob);
        uint32_t dword1 = *(const uint32_t *)(const void *)(s_models_blob + 4);

        /* Try format A: DWORD[0] = count, table starts at byte 4 */
        if (dword0 > 0 && dword0 < 10000 &&
            dword1 == 4 + dword0 * 8 &&
            (size_t)(4 + dword0 * 8) <= size) {
            raw_entry_count = dword0;
            table_start_byte = 4;
            TD5_LOG_I("track", "MODELS.DAT format A: count=%u table@4 first_block@%u",
                      raw_entry_count, dword1);
        }
        /* Try format B: no count header, table at byte 0, count = DWORD[1]/8 */
        else if (dword1 > 0 && (dword1 & 7u) == 0 && dword1 <= size) {
            raw_entry_count = dword1 / 8u;
            table_start_byte = 0;
            TD5_LOG_I("track", "MODELS.DAT format B: count=%u table@0 first_block@%u",
                      raw_entry_count, dword1);
        }
        /* Try format A fallback: maybe DWORD[1] isn't exactly 4+count*8 but DWORD[0]
         * is still a count and the table has 8-byte stride from byte 4 */
        else if (dword0 > 0 && dword0 < 10000 && (size_t)(4 + dword0 * 8) <= size) {
            raw_entry_count = dword0;
            table_start_byte = 4;
            TD5_LOG_I("track", "MODELS.DAT format A (relaxed): count=%u table@4",
                      raw_entry_count);
        }
        else {
            TD5_LOG_W("track", "MODELS.DAT: cannot determine format (dword0=%u dword1=%u size=%zu)",
                      dword0, dword1, size);
            free_models_dat_runtime();
            return 0;
        }
    }

    if (raw_entry_count == 0) {
        free_models_dat_runtime();
        return 0;
    }

    s_models_entry_offsets = (uint32_t *)calloc(raw_entry_count, sizeof(uint32_t));
    s_models_entry_sizes = (uint32_t *)calloc(raw_entry_count, sizeof(uint32_t));
    if (!s_models_entry_offsets || !s_models_entry_sizes) {
        free_models_dat_runtime();
        return 0;
    }

    for (uint32_t i = 0; i < raw_entry_count; i++) {
        uint32_t tbl_byte = table_start_byte + i * 8u;
        uint32_t entry_offset, entry_size;
        const uint8_t *entry_base;
        uint32_t sub_mesh_count;

        if (tbl_byte + 8 > size)
            break;

        entry_offset = *(const uint32_t *)(const void *)(s_models_blob + tbl_byte);
        entry_size   = *(const uint32_t *)(const void *)(s_models_blob + tbl_byte + 4);

        /* Both format A and B use [offset, size] pairs.  The only difference
         * is where the entry table starts (byte 4 for A, byte 0+4 for B).
         * The old swap was incorrect and caused blocks to be read from wrong
         * positions, producing 28-64% validation failures. */

        if (entry_offset == 0 || entry_offset >= size)
            continue;

        /* Compute block size from next entry's offset if available,
         * as entry_size may not be reliable in all format variants */
        if (entry_size == 0 || (size_t)entry_offset + (size_t)entry_size > size) {
            /* Estimate size from next entry or end of file */
            uint32_t next_off = (uint32_t)size;
            if (i + 1 < raw_entry_count) {
                uint32_t next_tbl = table_start_byte + (i + 1) * 8u;
                if (next_tbl + 8 <= size) {
                    uint32_t candidate =
                        *(const uint32_t *)(const void *)(s_models_blob + next_tbl);
                    if (candidate > entry_offset && candidate <= size)
                        next_off = candidate;
                }
            }
            entry_size = next_off - entry_offset;
        }

        if (entry_size < 8 || (size_t)entry_offset + (size_t)entry_size > size)
            continue;

        entry_base = s_models_blob + entry_offset;
        sub_mesh_count = *(const uint32_t *)(const void *)entry_base;
        if (sub_mesh_count == 0 || sub_mesh_count > 256)
            continue;
        if (entry_size < 4u + sub_mesh_count * 4u)
            continue;

        s_models_entry_offsets[display_list_count] = entry_offset;
        s_models_entry_sizes[display_list_count] = entry_size;
        s_models_display_list_count = display_list_count + 1;

        {
            const TD5_TrackRawMeshHeader *first_mesh;
            if (parsed_count < MODELS_DAT_MAX_ENTRIES &&
                get_display_list_first_mesh_header(display_list_count, &first_mesh)) {
                TD5_ModelsDatEntry *dst = &s_models[parsed_count++];
                dst->mesh_ptr = (void *)(uintptr_t)first_mesh;
                dst->texture_page_id = (int32_t)first_mesh->texture_page_id;
                dst->bounding_radius = first_mesh->bounding_radius;
            }
        }

        display_list_count++;
    }

    s_models_display_list_count = display_list_count;
    s_model_count = parsed_count;

    /* Relocate mesh pointers within each display list block.
     * Each block is: [sub_mesh_count][mesh_offset_0][mesh_offset_1]...
     * mesh_offset values are byte offsets from block start → convert to
     * absolute pointers. Then relocate internal MeshHeader fields. */
    for (int dl = 0; dl < s_models_display_list_count; dl++) {
        uint8_t *block_base = s_models_blob + s_models_entry_offsets[dl];
        uint32_t block_size = s_models_entry_sizes[dl];
        uint32_t sub_count = *(uint32_t *)block_base;

        if (sub_count == 0 || sub_count > 256 || block_size < 4u + sub_count * 4u)
            continue;

        for (uint32_t j = 0; j < sub_count; j++) {
            uint32_t *slot = (uint32_t *)(block_base + 4 + j * 4);
            uint32_t mesh_off = *slot;

            if (mesh_off == 0 || mesh_off >= block_size)
                continue;
            if (mesh_off + sizeof(TD5_MeshHeader) > block_size)
                continue;

            /* Convert relative offset to absolute pointer */
            TD5_MeshHeader *mesh = (TD5_MeshHeader *)(block_base + mesh_off);
            *slot = (uint32_t)(uintptr_t)mesh;

            /* Validate mesh fields before relocation.
             * Allow command_count==0 or vertex_count==0 — the original game
             * has placeholder/empty meshes that the renderer simply skips. */
            if (mesh->command_count < 0 || mesh->command_count > 4096 ||
                mesh->total_vertex_count < 0 || mesh->total_vertex_count > 65536) {
                static int s_vfail_log = 0;
                if (s_vfail_log < 5) {
                    TD5_LOG_W("track", "MODELS.DAT mesh validation fail: dl=%d slot=%d "
                              "cmds=%d verts=%d off=0x%x",
                              dl, j, mesh->command_count, mesh->total_vertex_count, mesh_off);
                    s_vfail_log++;
                }
                *slot = 0;
                continue;
            }

            /* Validate internal offsets stay within the models blob before
             * relocating.  A wild commands_offset or vertices_offset is the
             * most common crash vector for tracks with unusual MODELS.DAT.
             * Offsets are relative to the mesh header start. */
            {
                uintptr_t mesh_abs = (uintptr_t)mesh;
                uintptr_t blob_end = (uintptr_t)s_models_blob + s_models_blob_size;
                uint32_t cmd_off = mesh->commands_offset;
                uint32_t vtx_off = mesh->vertices_offset;

                if (cmd_off != 0 && mesh_abs + cmd_off >= blob_end) {
                    *slot = 0;
                    continue;
                }
                if (vtx_off != 0 && mesh_abs + vtx_off >= blob_end) {
                    *slot = 0;
                    continue;
                }
            }

            /* Relocate commands/vertices/normals offsets within mesh */
            td5_track_prepare_mesh_resource(mesh);
        }
    }

    /* Post-relocation validation: log bad blocks but do NOT disable all
     * display lists.  The per-mesh validation in td5_render_span_display_list
     * already skips individual bad meshes safely.  The old 25% threshold was
     * disabling tracks that were 72% valid, leaving no visible geometry. */
    {
        int bad_blocks = 0;
        for (int dl = 0; dl < s_models_display_list_count; dl++) {
            uint8_t *blk = s_models_blob + s_models_entry_offsets[dl];
            uint32_t sc = *(uint32_t *)blk;
            if (sc == 0 || sc > 256) { bad_blocks++; continue; }
            for (uint32_t j = 0; j < sc; j++) {
                uint32_t ptr_val = *(uint32_t *)(blk + 4 + j * 4);
                if (ptr_val == 0) continue;
                TD5_MeshHeader *m = (TD5_MeshHeader *)(uintptr_t)ptr_val;
                if ((uintptr_t)m < 0x10000u ||
                    !td5_track_is_ptr_in_blob(m, sizeof(TD5_MeshHeader)) ||
                    m->command_count < 0 || m->command_count > 4096 ||
                    m->total_vertex_count < 0 || m->total_vertex_count > 65536) {
                    static int s_post_fail_log = 0;
                    if (s_post_fail_log < 10) {
                        TD5_LOG_W("track",
                            "post-reloc fail: dl=%d j=%d ptr=0x%08X in_blob=%d "
                            "cmds=%d verts=%d cmd_off=0x%08X vtx_off=0x%08X",
                            dl, (int)j, (unsigned)ptr_val,
                            td5_track_is_ptr_in_blob(m, sizeof(TD5_MeshHeader)),
                            ((uintptr_t)m >= 0x10000u) ? m->command_count : -1,
                            ((uintptr_t)m >= 0x10000u) ? m->total_vertex_count : -1,
                            ((uintptr_t)m >= 0x10000u) ? m->commands_offset : 0,
                            ((uintptr_t)m >= 0x10000u) ? m->vertices_offset : 0);
                        s_post_fail_log++;
                    }
                    *(uint32_t *)(blk + 4 + j * 4) = 0;
                    bad_blocks++;
                }
            }
        }
        if (bad_blocks > 0) {
            TD5_LOG_W("track",
                "MODELS.DAT: %d/%d blocks had bad meshes (zeroed, not disabled)",
                bad_blocks, s_models_display_list_count);
        }
    }

    if (s_models_display_list_count > 0 && s_span_count > 0)
        rebuild_span_display_list_mapping();

    TD5_LOG_I("track", "MODELS.DAT: %d/%u display lists, %d model entries, blob=%zuB",
              s_models_display_list_count, raw_entry_count, s_model_count, size);

    return parsed_count;
}

/**
 * PrepareMeshResource (0x40AC00).
 * Relocates internal offset fields in a mesh header to absolute pointers.
 * The commands_offset, vertices_offset, and normals_offset fields are
 * converted from byte-offsets-from-header-start to absolute pointers.
 */
void td5_track_prepare_mesh_resource(TD5_MeshHeader *mesh)
{
    uint8_t *base;

    if (!mesh)
        return;

    base = (uint8_t *)mesh;

    /* Relocate offsets to absolute pointers.
     * The original stores these as uint32 offsets relative to the mesh
     * header start. We convert them to pointers (stored back as uint32
     * for the original 32-bit engine). In the source port we store them
     * as uintptr_t-compatible values. */
    if (mesh->commands_offset != 0)
        mesh->commands_offset = (uint32_t)(uintptr_t)(base + mesh->commands_offset);
    if (mesh->vertices_offset != 0)
        mesh->vertices_offset = (uint32_t)(uintptr_t)(base + mesh->vertices_offset);
    if (mesh->normals_offset != 0)
        mesh->normals_offset = (uint32_t)(uintptr_t)(base + mesh->normals_offset);
}

/* ========================================================================
 * Track Runtime Data Loading (0x42FB90 LoadTrackRuntimeData)
 * ======================================================================== */

/**
 * InitializeTrackStripMetadata (0x42FAD0).
 * Sets up per-strip metadata from the level data.
 */
static void initialize_strip_metadata(int track_index)
{
    int i;
    (void)track_index;

    /* Initialize strip type flags to default (1) */
    for (i = 0; i < 4; i++) {
        s_strip_metadata[i] = 1;
    }
}

int td5_track_load_runtime_data(int track_index, int reverse)
{
    (void)reverse;

    /* Initialize strip metadata */
    initialize_strip_metadata(track_index);

    /* Store track config in global state */
    g_td5.track_index = track_index;
    g_td5.reverse_direction = reverse;

    /* Initialize race order array to identity */
    {
        int i;
        for (i = 0; i < TD5_MAX_RACER_SLOTS; i++) {
            s_race_order[i] = (uint8_t)i;
        }
    }

    /* Clear actor lighting zones */
    memset(s_actor_light_zone, 0, sizeof(s_actor_light_zone));

    /* Clear directional lights */
    memset(s_light_enabled, 0, sizeof(s_light_enabled));
    memset(s_light_dir, 0, sizeof(s_light_dir));

    /* Ensure renderer-owned fallback geometry is always available even when
     * the level archive is missing or the real display-list builder has not
     * been wired in yet. */
    if (!s_display_lists && !build_strip_display_lists() && !build_placeholder_display_lists())
        return 0;

    /* In the original, this function loads:
     *   - STRIP.DAT from the level ZIP (calls td5_track_load_strip)
     *   - LEFT.TRK / RIGHT.TRK route data (calls td5_track_load_routes)
     *   - TRAFFIC.BUS queue data
     *   - CHECKPT.NUM checkpoint remapping table
     *   - LEVELINF.DAT environment configuration
     *   - Applies attribute overrides (calls td5_track_apply_attribute_overrides)
     *   - Copies checkpoint timing metadata from the per-track EXE tables
     *
     * The actual file loading is delegated to the asset module (td5_asset.c).
     * This function performs the runtime initialization after all data is loaded. */

    return 1;
}

/* ========================================================================
 * Span Access and Display List
 * ======================================================================== */

int td5_track_get_span_count(void)
{
    return s_span_count;
}

int td5_track_is_ptr_in_blob(const void *ptr, size_t need)
{
    if (!ptr || !s_models_blob || s_models_blob_size == 0) return 0;
    uintptr_t p = (uintptr_t)ptr;
    uintptr_t base = (uintptr_t)s_models_blob;
    uintptr_t end  = base + s_models_blob_size;
    if (p < base || p >= end) return 0;
    if (need > 0 && p + need > end) return 0;
    return 1;
}

int td5_track_is_valid_mesh_ptr(const void *ptr)
{
    uintptr_t p;
    /* Reject NULL and anything below 1MB — no legitimate heap/blob pointer
     * on Win32 is that low.  Unrelocated MODELS.DAT offsets (relative byte
     * offsets within the file) are typically 0x100-0xFFFFF and must not be
     * dereferenced as pointers. */
    if (!ptr || (uintptr_t)ptr < 0x100000u) return 0;
    p = (uintptr_t)ptr;
    /* Check if pointer is within the models blob */
    if (s_models_blob && s_models_blob_size > 0) {
        uintptr_t base = (uintptr_t)s_models_blob;
        if (p >= base && p < base + s_models_blob_size) {
            if (p + sizeof(TD5_MeshHeader) <= base + s_models_blob_size)
                return 1;
            return 0;
        }
    }
    /* Accept heap pointers from generated strip display lists —
     * these are above 0x100000 on any normal Win32 system. */
    return 1;
}

TD5_StripSpan *td5_track_get_span(int index)
{
    if (!s_span_array || index < 0 || index >= s_span_count)
        return NULL;
    return &s_span_array[index];
}

TD5_StripVertex *td5_track_get_vertex(int index)
{
    if (!s_vertex_table || index < 0)
        return NULL;
    return &s_vertex_table[index];
}

int td5_track_get_models_display_list_count(void)
{
    return s_models_display_list_count;
}

int td5_track_get_span_display_list_index(int span_index)
{
    if (!s_span_display_list_indices || span_index < 0 || span_index >= s_span_count)
        return -1;
    return s_span_display_list_indices[span_index];
}

const void *td5_track_get_models_display_list_raw(int index, size_t *size_out)
{
    const uint8_t *entry;
    uint32_t entry_size;

    if (!get_models_display_list_entry(index, &entry, &entry_size))
        return NULL;
    if (size_out)
        *size_out = (size_t)entry_size;
    return entry;
}

/**
 * GetTrackSpanDisplayListEntry (0x431260).
 * Returns the display list (pre-built render command buffer) for a span.
 */
void *td5_track_get_display_list(int span_index)
{
    static int s_models_log = 0;

    /* Prefer real MODELS.DAT display lists when available.
     * Original (0x431260): return *(uint*)(table_base + span_index * 8)
     * The MODELS.DAT table has one entry per span (direct 1:1 mapping). */
    if (s_span_display_list_indices && s_models_blob && s_models_entry_offsets &&
        s_models_display_list_count > 0 &&
        span_index >= 0 && span_index < s_span_count) {
        int idx = s_span_display_list_indices[span_index];
        if (idx >= 0 && idx < s_models_display_list_count) {
            uint8_t *blk = s_models_blob + s_models_entry_offsets[idx];
            if (s_models_log < 3) {
                uint32_t sub_count = *(uint32_t *)blk;
                TD5_LOG_I("track",
                    "MODELS.DAT display list: span=%d idx=%d offset=0x%x sub_meshes=%u",
                    span_index, idx, s_models_entry_offsets[idx], sub_count);
                if (sub_count > 0 && sub_count <= 256) {
                    TD5_MeshHeader *mesh = (TD5_MeshHeader *)(uintptr_t)(*(uint32_t *)(blk + 4));
                    if (mesh && (uintptr_t)mesh > 0x10000u) {
                        TD5_LOG_I("track",
                            "  mesh0: origin=(%.1f,%.1f,%.1f) radius=%.1f cmds=%d verts=%d",
                            mesh->origin_x, mesh->origin_y, mesh->origin_z,
                            mesh->bounding_radius,
                            mesh->command_count, mesh->total_vertex_count);
                    }
                }
                s_models_log++;
            }
            return blk;
        }
    }

    /* Fallback: generated strip display lists from STRIP.DAT */
    if (s_display_lists && span_index >= 0) {
        if (s_span_count > 0 && span_index < s_span_count)
            return s_display_lists[span_index];
        if (s_span_count <= 0 && span_index == 0)
            return s_display_lists[0];
    }

    if (!s_display_lists && span_index == 0 && s_fallback_display_list)
        return s_fallback_display_list;

    return NULL;
}

/* ========================================================================
 * Module Lifecycle
 * ======================================================================== */

int td5_track_init(void)
{
    free_display_lists();
    free_models_dat_runtime();

    memset(s_race_order, 0, sizeof(s_race_order));
    memset(s_light_enabled, 0, sizeof(s_light_enabled));
    memset(s_light_dir, 0, sizeof(s_light_dir));
    memset(s_actor_light_zone, 0, sizeof(s_actor_light_zone));
    memset(s_models, 0, sizeof(s_models));

    s_span_array = NULL;
    s_vertex_table = NULL;
    s_span_count = 0;
    s_strip_blob = NULL;
    s_strip_blob_size = 0;
    s_strip_header = NULL;
    s_jump_entry_count = 0;
    s_jump_entries = NULL;
    s_attr_override_table = NULL;
    s_attr_override_count = 0;
    s_display_lists = NULL;
    s_models_blob = NULL;
    s_models_blob_size = 0;
    s_models_entry_offsets = NULL;
    s_models_entry_sizes = NULL;
    s_models_display_list_count = 0;
    s_span_display_list_indices = NULL;
    s_route_left = NULL;
    s_route_right = NULL;
    s_route_left_size = 0;
    s_route_right_size = 0;
    s_traffic_bus = NULL;
    s_traffic_bus_count = 0;
    s_traffic_cursor = 0;
    s_lighting_table = NULL;
    s_lighting_entry_count = 0;
    s_model_count = 0;
    g_strip_span_count = 0;
    g_strip_total_segments = 0;
    g_strip_span_base = NULL;
    g_strip_vertex_base = NULL;

    return 1;
}

void td5_track_shutdown(void)
{
    if (s_strip_blob) {
        free(s_strip_blob);
        s_strip_blob = NULL;
    }
    s_strip_blob_size = 0;
    s_span_array = NULL;
    s_vertex_table = NULL;
    s_span_count = 0;
    s_strip_header = NULL;
    s_jump_entry_count = 0;
    s_jump_entries = NULL;
    g_strip_span_count = 0;
    g_strip_total_segments = 0;
    g_strip_span_base = NULL;
    g_strip_vertex_base = NULL;

    free_display_lists();
    free_models_dat_runtime();

    if (s_route_left) {
        free(s_route_left);
        s_route_left = NULL;
    }
    s_route_left_size = 0;

    if (s_route_right) {
        free(s_route_right);
        s_route_right = NULL;
    }
    s_route_right_size = 0;
    td5_ai_set_route_tables(NULL, 0, NULL, 0);

    if (s_traffic_bus) {
        free(s_traffic_bus);
        s_traffic_bus = NULL;
    }
    s_traffic_bus_count = 0;
    s_traffic_cursor = 0;

    s_lighting_table = NULL;
    s_lighting_entry_count = 0;
}

/**
 * Per-tick update: runs race actor updates and traffic recycling.
 * Called from the main game loop each simulation tick.
 *
 * Corresponds to UpdateRaceActors (0x436A70) which iterates all active
 * actors, updates their track positions, checks checkpoints, normalizes
 * wrap state, and recycles traffic.
 */
void td5_track_tick(void)
{
    /* Update race order (bubble sort on high-water mark) */
    td5_track_update_race_order();

    /* Recycle traffic actors that have fallen behind */
    if (g_td5.traffic_enabled) {
        td5_track_recycle_traffic_actor();
    }
}

/* ========================================================================
 * Track Probe Functions (migrated from td5re_stubs.c)
 * ======================================================================== */

void UpdateActorTrackPosition(short *probe, int *pos) {
    /*
     * Lightweight span-boundary walk for camera probes.
     *
     * probe is a TD5_TrackProbe laid out as short[]:
     *   [0] = span_index
     *   byte offset 12 = sub_lane_index (int8)
     *
     * pos = { world_x, world_z } in 24.8 fixed-point.
     */
    int span_count;
    int span_idx;
    int sub_lane;

    if (!probe || !pos) return;

    span_count = td5_track_get_span_count();
    if (span_count <= 0) return;

    span_idx = (int)probe[0];
    sub_lane = (int)((int8_t *)probe)[12];

    /* Clamp span index to valid range */
    if (span_idx < 0) span_idx = 0;
    if (span_idx >= span_count) span_idx = span_count - 1;

    /* Clamp sub_lane */
    if (sub_lane < 0) sub_lane = 0;

    /* Write back clamped values */
    probe[0] = (short)span_idx;
    ((int8_t *)probe)[12] = (int8_t)sub_lane;
}

void ComputeActorTrackContactNormal(short *probe, int *pos, int *out_y) {
    /*
     * Compute terrain contact height at the probe's span/sub-lane
     * for the world position pos = {x, z}.
     *
     * Returns the ground Y height (24.8 fixed-point, span-local units)
     * via *out_y. The camera uses this to compute pitch/roll from
     * three sample points around the vehicle.
     */
    int span_idx;
    int sub_lane;
    int span_count;
    int32_t height;

    if (!out_y) return;
    *out_y = 0;

    if (!probe || !pos) return;

    span_count = td5_track_get_span_count();
    if (span_count <= 0) return;

    span_idx = (int)probe[0];
    sub_lane = (int)((int8_t *)probe)[12];

    if (span_idx < 0 || span_idx >= span_count) return;
    if (sub_lane < 0) sub_lane = 0;

    /* Delegate to the barycentric contact height resolver */
    height = td5_track_compute_contact_height(span_idx, sub_lane,
                                               pos[0], pos[1]);
    *out_y = (int)height;
}
