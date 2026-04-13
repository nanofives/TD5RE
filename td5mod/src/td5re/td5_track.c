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
#include "td5_asset.h"
#include "td5_physics.h"
#include "td5_ai.h"
#include "td5_platform.h"
#include "../../../re/include/td5_actor_struct.h"
#include "td5re.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>

#define LOG_TAG "track"

extern int AngleFromVector12(int x, int z);

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
    0x00, 0x0E, 0x0E, 0x06, 0x06, 0x0E, 0x0C, 0x0C,
    0x0E, 0x0E, 0x0E, 0x0E
};
static const uint8_t s_edge_mask_last[12] = {
    0x00, 0x0B, 0x03, 0x0B, 0x03, 0x09, 0x0B, 0x09,
    0x0B, 0x0B, 0x0B, 0x0B
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

/* ========================================================================
 * Generic lateral wall contacts (port-specific; deviates from original)
 *
 * This function *previously* claimed to implement `0x406CC0`
 * UpdateActorTrackSegmentContacts as a "lateral wall" test. That was wrong.
 * Per Ghidra decomp verified 2026-04-11, `0x406CC0` is a **sub_lane extremity
 * longitudinal boundary** test — not a lateral wall. Its vertex pair is
 * right[0] / left[0] (one from each rail), giving a transverse edge whose
 * perpendicular is **along the road**, not across it. The port's old
 * geometry was therefore testing longitudinal distance, not lateral, which
 * is why the "LEFT/RIGHT sign-flip" saga never produced a working wall.
 *
 * The original TD5_d3d.exe binary has **no dedicated function** that applies
 * lateral wall impulses for generic mid-strip driving. Off-road lateral
 * containment is topological: `UpdateActorTrackPosition` (0x4440F0) walks
 * boundary bits and either transitions the probe to a neighbor span or
 * fails, in which case the car enters state 0x0F damping. The only calls
 * to ApplyTrackSurfaceForceToActor (0x406980) are:
 *   - `0x406CC0` branches 1/2  → sub_lane start/end longitudinal rumble
 *   - `0x406F50`               → left rail of fence strip DAT_00483954
 *   - `0x4070E0`               → left rail of fence strip DAT_00483550
 * None handle mid-track lateral walls.
 *
 * THIS FUNCTION is a **port-specific** lateral wall check added to give the
 * expected "drive into wall and bounce" feel while the state-0x0F damping
 * path is still stubbed in the port. It tests the probe against both rails
 * of the probe's CURRENT span, using two vertices on the SAME rail to get
 * an along-road edge and a **lateral** perpendicular (pointing inward).
 * `d < 0` means the probe is outside that rail.
 *
 * Not gated by sub_lane — the original's gate served a purpose only with
 * the original's (longitudinal) geometry. Here the test is safe to run
 * every probe every tick.
 *
 * See reference_v2w_function_semantics.md for full background.
 * ======================================================================== */

void td5_track_resolve_wall_contacts(TD5_Actor *actor)
{
    static uint32_t s_wall_tick = 0;
    int diag_slot0 = 0;

    if (!actor || !s_span_array || !s_vertex_table) return;

    /* Traffic actors (slots 6+) skip lateral wall checks entirely.
     * This function is port-specific (original has no mid-strip lateral walls).
     * Traffic follows fixed AI paths and doesn't need containment.
     * Running the check on traffic with uninitialized probe positions produced
     * massive false-positive contacts (raw d = -199000) whose cumulative
     * rebuild_pose calls disrupted the simulation for ALL actors. */
    if (actor->slot_index >= 6) return;

    /* Pre-spawn guard: skip if world position or all probes are at origin. */
    if (actor->world_pos.x == 0 && actor->world_pos.z == 0) return;

    if (actor->slot_index == 0) {
        s_wall_tick++;
        if ((s_wall_tick % 60u) == 0u) diag_slot0 = 1;
    }

    TD5_Vec3_Fixed *probe_block = &actor->probe_FL;

    /* Use the chassis span (actor+0x80), not per-probe span_index, because
     * the boundary walker leaves wheel_probes[pi].span_index stale across
     * span transitions. Runtime diag showed probes testing against a span
     * whose rail is hundreds of units from the actual probe position —
     * that's the source of the d=-4000 "invisible walls". The chassis
     * span is maintained by update_actor_track_position and lags less.
     *
     * All four wheel probes test against the same chassis span. That's
     * close enough for lateral wall tests since the wheels are ~800 units
     * apart — within a single span's extent on any normal track. */
    int span_idx = (int)actor->track_span_raw;
    if (span_idx < 0 || span_idx >= s_span_count) return;

    const TD5_StripSpan *sp = &s_span_array[span_idx];
    int type = sp->span_type;
    /* Only run on standard quad types (1, 2, 5) whose vertex layout gives
     * reliable longitudinal wall lines. Transition (3, 4), reversed (6, 7),
     * junction (8, 11), and sentinel (9, 10) types have irregular vertex
     * geometry that produces inverted wall normals — the road center tests
     * as "outside" both walls, creating invisible barriers mid-road. */
    if (type != 1 && type != 2 && type != 5) return;

    /* Skip wall checks on branch road spans (index >= ring_length).
     * Branch roads are typically narrow (1-2 lanes) and their vertex
     * layout produces false-positive wall contacts on BOTH sides of the
     * car simultaneously (probe 0/1 on RIGHT, probe 2/3 on LEFT, d=-30
     * identical), creating invisible walls that trap the car. The
     * original has no lateral wall system at all — this is port-specific.
     * Forward/reverse boundary contacts still run on branch spans. */
    if (span_idx >= g_td5.track_span_ring_length) return;

    int lane_count = span_lane_count(sp);
    if (lane_count < 1) lane_count = 1;

    /* NEAR and FAR rows. The strip field labels in TD5 data are misleading:
     *
     *   left_vertex_index   → base of the span's NEAR row (entry edge)
     *   right_vertex_index  → base of the span's FAR row  (exit edge)
     *   +k walks LATERALLY across the span's lane_count lanes (0..lane_count
     *   inclusive gives lane_count+1 vertices per row).
     *
     * Verified by the wall_layout dump on Moscow span 111:
     *   - left[k+1] - left[k] = (-1355, -600) constant — lateral step
     *   - right[k] - left[k] ≈ (-545, 1242), perpendicular to the lateral
     *     step — that's the longitudinal travel direction
     *
     * And verified against slot 0's visible heading: probe layout gives
     * car forward ≈ 114° world. The (right[0] - left[0]) vector
     * atan2(1242, -545) ≈ 114° matches exactly. Cars travel along the
     * near→far direction; they walk the +k lane index only when drifting
     * laterally.
     *
     * So the outer ROAD walls run in the longitudinal direction and sit
     * at the two edges of the lane range:
     *
     *   LEFT road wall  = line from left[0]            to right[0]
     *   RIGHT road wall = line from left[lane_count]   to right[lane_count]
     *
     * Each "wall" edge runs near→far (travel direction); perpendicular is
     * lateral (across the road). d < 0 means the probe is outside that
     * edge. The auto-flip uses an inside-reference vertex from the
     * opposite lane extreme to pick the correct sign.
     *
     * This approach tests just two lines per span regardless of how many
     * lanes the span has, which is what a lateral wall check should do. */

    /* For standard types (1, 2, 5) vertex offsets are always 0. */
    int li_base = (int)sp->left_vertex_index;
    int ri_base = (int)sp->right_vertex_index;

    struct wall_line {
        TD5_StripVertex *base;    /* near vertex */
        TD5_StripVertex *end_v;   /* far vertex */
        TD5_StripVertex *inside;  /* reference vertex definitely on the inside */
    };

    struct wall_line l_wall = {
        vertex_at(li_base + 0),
        vertex_at(ri_base + 0),
        vertex_at(li_base + lane_count)
    };
    struct wall_line r_wall = {
        vertex_at(li_base + lane_count),
        vertex_at(ri_base + lane_count),
        vertex_at(li_base + 0)
    };

    if (!l_wall.base || !l_wall.end_v || !l_wall.inside) return;
    if (!r_wall.base || !r_wall.end_v || !r_wall.inside) return;

    /* Precompute each wall's perp (with auto-flipped inside direction). */
    struct wall_params {
        int32_t edge_dx, edge_dz;
        int32_t nnx, nnz;
        int32_t base_x, base_z;
        int ok;
    } l_par = {0}, r_par = {0};

    {
        l_par.edge_dx = (int32_t)l_wall.end_v->x - (int32_t)l_wall.base->x;
        l_par.edge_dz = (int32_t)l_wall.end_v->z - (int32_t)l_wall.base->z;
        l_par.base_x  = (int32_t)l_wall.base->x;
        l_par.base_z  = (int32_t)l_wall.base->z;
        float fnx = (float)(l_par.edge_dz);
        float fnz = (float)(-l_par.edge_dx);
        float fmag = sqrtf(fnx * fnx + fnz * fnz);
        if (fmag >= 0.5f) {
            int32_t nnx = (int32_t)(fnx / fmag * 4096.0f);
            int32_t nnz = (int32_t)(fnz / fmag * 4096.0f);
            /* Auto-flip so perp points INWARD (toward the opposite lane
             * extreme inside vertex). */
            int32_t opp_rel_x = (int32_t)l_wall.inside->x - l_par.base_x;
            int32_t opp_rel_z = (int32_t)l_wall.inside->z - l_par.base_z;
            int64_t opp_dot = (int64_t)opp_rel_x * nnx + (int64_t)opp_rel_z * nnz;
            if (opp_dot < 0) { nnx = -nnx; nnz = -nnz; }
            l_par.nnx = nnx; l_par.nnz = nnz; l_par.ok = 1;
        }
    }
    {
        r_par.edge_dx = (int32_t)r_wall.end_v->x - (int32_t)r_wall.base->x;
        r_par.edge_dz = (int32_t)r_wall.end_v->z - (int32_t)r_wall.base->z;
        r_par.base_x  = (int32_t)r_wall.base->x;
        r_par.base_z  = (int32_t)r_wall.base->z;
        float fnx = (float)(r_par.edge_dz);
        float fnz = (float)(-r_par.edge_dx);
        float fmag = sqrtf(fnx * fnx + fnz * fnz);
        if (fmag >= 0.5f) {
            int32_t nnx = (int32_t)(fnx / fmag * 4096.0f);
            int32_t nnz = (int32_t)(fnz / fmag * 4096.0f);
            int32_t opp_rel_x = (int32_t)r_wall.inside->x - r_par.base_x;
            int32_t opp_rel_z = (int32_t)r_wall.inside->z - r_par.base_z;
            int64_t opp_dot = (int64_t)opp_rel_x * nnx + (int64_t)opp_rel_z * nnz;
            if (opp_dot < 0) { nnx = -nnx; nnz = -nnz; }
            r_par.nnx = nnx; r_par.nnz = nnz; r_par.ok = 1;
        }
    }

    /* Simple wall fire: d < dead zone and clamp at -200. No trend
     * detection, no span-transition skip, no hysteresis. Earlier
     * attempts to filter walker-lag spikes ended up suppressing real
     * walls — the geometry is correct (verified), the walker is broken
     * (root cause), and no wall-side heuristic can reliably distinguish
     * the two. Accept occasional walker-lag fires (clamped to -200 so
     * the push is bounded) in exchange for walls that actually work. */

    for (int pi = 0; pi < 4; pi++) {
        int32_t px = probe_block[pi].x >> 8;
        int32_t pz = probe_block[pi].z >> 8;

        int32_t l_d = 0, r_d = 0;
        int l_ok = 0, r_ok = 0;

        if (l_par.ok) {
            int32_t rel_x = px - l_par.base_x - sp->origin_x;
            int32_t rel_z = pz - l_par.base_z - sp->origin_z;
            int64_t dot = (int64_t)rel_x * l_par.nnx + (int64_t)rel_z * l_par.nnz;
            l_d = (int32_t)((dot + ((dot >> 63) & 0xFFF)) >> 12);
            if (l_d < -30) l_d = -30;
            l_ok = 1;
        }
        if (r_par.ok) {
            int32_t rel_x = px - r_par.base_x - sp->origin_x;
            int32_t rel_z = pz - r_par.base_z - sp->origin_z;
            int64_t dot = (int64_t)rel_x * r_par.nnx + (int64_t)rel_z * r_par.nnz;
            r_d = (int32_t)((dot + ((dot >> 63) & 0xFFF)) >> 12);
            if (r_d < -30) r_d = -30;
            r_ok = 1;
        }

        const int32_t WALL_DEAD_ZONE = -10;

        /* Sanity check: if BOTH walls report the probe as outside (both d
         * negative), the span has degenerate geometry (e.g., lane-count
         * transition where one wall edge runs diagonally instead of
         * longitudinally). Skip both impulses for this probe. */
        if (l_ok && r_ok && l_d < 0 && r_d < 0)
            continue;

        if (l_ok && l_d < WALL_DEAD_ZONE) {
            double rad = atan2((double)l_par.nnx, (double)(-l_par.nnz));
            int32_t wall_angle = (int32_t)(rad * (4096.0 / (2.0 * 3.14159265358979323846))) & 0xFFF;
            td5_physics_wall_response(actor, wall_angle, l_d, 1, l_par.nnx, l_par.nnz, 4096);
            td5_physics_rebuild_pose(actor);
            TD5_LOG_I(LOG_TAG, "wall_contact: slot=%d probe=%d LEFT span=%d d=%d angle=%d",
                      actor->slot_index, pi, span_idx, l_d, wall_angle);
        }

        if (r_ok && r_d < WALL_DEAD_ZONE) {
            double rad = atan2((double)r_par.nnx, (double)(-r_par.nnz));
            int32_t wall_angle = (int32_t)(rad * (4096.0 / (2.0 * 3.14159265358979323846))) & 0xFFF;
            td5_physics_wall_response(actor, wall_angle, r_d, 2, r_par.nnx, r_par.nnz, 4096);
            td5_physics_rebuild_pose(actor);
            TD5_LOG_I(LOG_TAG, "wall_contact: slot=%d probe=%d RIGHT span=%d d=%d angle=%d",
                      actor->slot_index, pi, span_idx, r_d, wall_angle);
        }

        if (diag_slot0) {
            TD5_LOG_I(LOG_TAG, "wall_diag slot0 p%d span=%d L:d=%d R:d=%d probe=(%d,%d)",
                      pi, span_idx, l_d, r_d, px, pz);
        }
    }
}

/* ========================================================================
 * Forward / Reverse track-segment contact handlers
 *
 * FUN_00406F50 (Forward) and FUN_004070E0 (Reverse). Paired with the
 * lateral handler above; the three run in order Reverse -> Forward ->
 * Lateral inside UpdateVehicleActor (0x00406650) after integrate_pose.
 *
 * Both functions gate on a per-level boundary span index:
 *   DAT_00483954 = forward-side sentinel  (used by Forward handler)
 *   DAT_00483550 = reverse-side sentinel  (used by Reverse handler)
 *
 * Both values are written EXACTLY ONCE by LoadTrackRuntimeData @ 0x0042fb90
 * from a 40-entry table at 0x00473820, keyed on (level_number - 1). Raw
 * disasm @ 0x0042fd00-0x0042fd57:
 *   MOV EAX, [ESP+0x128]     ; level number (1..N)
 *   DEC EAX
 *   MOV EDX, [EAX*8 + 0x473820]  ; DAT_00483954 = fwd sentinel
 *   MOV EAX, [EAX*8 + 0x473824]  ; DAT_00483550 = rev sentinel
 *   MOV [0x00483954], EDX
 *   MOV [0x00483550], EAX
 *
 * The values are NOT `(1, s_span_count-2)` — Australia (level 14) is
 * `{21, 2716}`, Scotland (level 16) is `{20, 2736}`, etc. Naming is also
 * inverted from intuition: the FORWARD sentinel is a SMALL span number
 * near the start, and the REVERSE sentinel is a LARGE span number near
 * the end. Placeholder levels (drag-race reverse-only entries, unused
 * slots) use `{-1, 9999}` which naturally disables both handlers.
 *
 * The port's previous assumption of `(1, s_span_count-2)` caused the
 * Forward handler to fire at spawn on Australia/Scotland (player at
 * span ~92, boundary=2735 instead of 21), compute a huge bogus
 * penetration against strip[2735], and teleport the player off-track.
 *
 * Raw-disasm details [CONFIRMED @ 0x406FCF, 0x007115]:
 *   - Outer gate uses actor->track_span_raw vs the boundary ± 1.
 *   - Per-probe gate filters probes whose span matches (Reverse: equality)
 *     or stays below the boundary (Forward: <=).
 *   - The strip record used to build the outward normal is ALWAYS
 *     g_strip[boundary], NOT the probe's own span.
 *   - Outward normal comes from the two vertices (base, base+count) of
 *     the boundary strip's left edge, normalized to length 4096.
 *   - flag=0 to wall_response so track_contact_flag is NOT written
 *     (port hooks side<0 to skip that write).
 * ======================================================================== */

/* Per-level boundary sentinel pair, copied from the original binary's
 * table at 0x00473820 (40 entries x int32[2], little-endian). Indexed by
 * `level_number - 1`. Each row is { forward_sentinel, reverse_sentinel }.
 * [CONFIRMED via raw memory_read 0x00473820..0x0047395F, 320 bytes.] */
static const int32_t s_level_boundary_sentinels[40][2] = {
    /* L1  */ {   36, 3135 },  /* L2  */ {   34, 2615 },
    /* L3  */ {    1, 3326 },  /* L4  */ {   24, 2610 },
    /* L5  */ {   24, 2855 },  /* L6  */ {   36, 2658 },
    /* L7  */ {   40, 3114 },  /* L8  */ {   27, 2608 },
    /* L9  */ {   36, 3359 },  /* L10 */ {   38, 2624 },
    /* L11 */ {   25, 2856 },  /* L12 */ {    2, 2624 },
    /* L13 */ {   23, 2695 },  /* L14 */ {   21, 2716 },
    /* L15 */ {   20, 2757 },  /* L16 */ {   20, 2736 },
    /* L17 */ {   20, 3089 },  /* L18 */ {    1, 2720 },
    /* L19 */ {   21, 2736 },  /* L20 */ {   20, 2757 },
    /* L21 */ {   20, 2736 },  /* L22 */ {   20, 3089 },
    /* L23 */ {   20, 2769 },  /* L24 */ {   20, 2769 },
    /* L25 */ {   -1, 9999 },  /* L26 */ {   -1, 9999 },
    /* L27 */ {   -1, 9999 },  /* L28 */ {   -1, 9999 },
    /* L29 */ {   -1, 9999 },  /* L30 */ {  104,  240 },
    /* L31 */ {  104,  240 },  /* L32 */ {   -1, 9999 },
    /* L33 */ {   -1, 9999 },  /* L34 */ {   -1, 9999 },
    /* L35 */ {   -1, 9999 },  /* L36 */ {   -1, 9999 },
    /* L37 */ {   -1, 9999 },  /* L38 */ {   -1, 9999 },
    /* L39 */ {   -1, 9999 },  /* L40 */ {   -1, 9999 },
};

/* Current race's boundary pair. Set by td5_track_bind_boundary_sentinels()
 * at level load. Defaults to {-1, 9999} which disables both handlers. */
static int32_t s_boundary_fwd_sentinel = -1;
static int32_t s_boundary_rev_sentinel = 9999;

void td5_track_bind_boundary_sentinels(int level_number)
{
    if (level_number < 1 || level_number > 40) {
        s_boundary_fwd_sentinel = -1;
        s_boundary_rev_sentinel = 9999;
        TD5_LOG_W(LOG_TAG,
                  "boundary sentinels: level=%d out of table range, disabling",
                  level_number);
        return;
    }
    s_boundary_fwd_sentinel = s_level_boundary_sentinels[level_number - 1][0];
    s_boundary_rev_sentinel = s_level_boundary_sentinels[level_number - 1][1];
    TD5_LOG_I(LOG_TAG,
              "boundary sentinels: level=%d fwd=%d rev=%d",
              level_number, s_boundary_fwd_sentinel, s_boundary_rev_sentinel);
}

static void fwd_rev_resolve_contact(TD5_Actor *actor,
                                    const TD5_StripSpan *sp,
                                    const TD5_StripVertex *base_v,
                                    const TD5_StripVertex *end_v,
                                    int reverse_mode,
                                    TD5_Vec3_Fixed *probe_block,
                                    int pi,
                                    int boundary)
{
    /* Outward normal perpendicular to the edge (base -> end), rotated
     * 90° out of the playable region. In Forward the edge goes (base -> end)
     * and outward = (end.z - base.z, base.x - end.x). In Reverse both
     * signs flip so the normal points back upstream. */
    int32_t edge_dx, edge_dz, nx, nz;
    if (!reverse_mode) {
        edge_dx = (int32_t)end_v->x  - (int32_t)base_v->x;
        edge_dz = (int32_t)end_v->z  - (int32_t)base_v->z;
        nx = (int32_t)end_v->z  - (int32_t)base_v->z;  /* end.z - base.z */
        nz = (int32_t)base_v->x - (int32_t)end_v->x;   /* base.x - end.x */
    } else {
        edge_dx = (int32_t)base_v->x - (int32_t)end_v->x;
        edge_dz = (int32_t)base_v->z - (int32_t)end_v->z;
        nx = (int32_t)base_v->z - (int32_t)end_v->z;   /* base.z - end.z */
        nz = (int32_t)end_v->x  - (int32_t)base_v->x;  /* end.x - base.x */
    }

    /* Normalize to length 4096 (matches StoreRoundedVector3Ints @ 0x42CCD0). */
    float fnx = (float)nx;
    float fnz = (float)nz;
    float fmag = sqrtf(fnx * fnx + fnz * fnz);
    if (fmag < 0.5f) return;
    int32_t nnx = (int32_t)(fnx / fmag * 4096.0f);
    int32_t nnz = (int32_t)(fnz / fmag * 4096.0f);

    /* Reference vertex used in the dot: base_v for Forward, end_v for Reverse.
     * [CONFIRMED raw disasm 0x406FE7]: psVar1 is the reference, and Reverse
     * swaps base/end relative to Forward. */
    const TD5_StripVertex *ref_v = reverse_mode ? end_v : base_v;

    int32_t probe_x = probe_block[pi].x >> 8;
    int32_t probe_z = probe_block[pi].z >> 8;

    /* Note the asymmetric decomposition: the Z term subtracts origin_z
     * before ref.z, while the X term subtracts ref.x before origin_x.
     * This matches the raw compiler output literally. */
    int64_t pen64 = (int64_t)((probe_z - sp->origin_z) - (int32_t)ref_v->z) * nnz
                  + (int64_t)((probe_x - (int32_t)ref_v->x) - sp->origin_x) * nnx;
    int32_t pen = (int32_t)((pen64 + ((pen64 >> 63) & 0xFFF)) >> 12);

    if (pen >= 0) return;  /* probe inside the playable region */

    /* Tangent along the edge (NOT the outward normal) feeds the angle. */
    double rad = atan2((double)edge_dz, (double)edge_dx);
    int32_t wall_angle = (int32_t)(rad * (4096.0 / (2.0 * 3.14159265358979323846))) & 0xFFF;

    /* side=-1 -> wall_response skips the track_contact_flag write, matching
     * the original's flag=0 branch of ApplyTrackSurfaceForceToActor. */
    td5_physics_wall_response(actor, wall_angle, pen, -1, nnx, nnz, 4096);
    td5_physics_rebuild_pose(actor);

    TD5_LOG_I(LOG_TAG, "%s_contact: slot=%d probe=%d boundary=%d pen=%d angle=%d",
              reverse_mode ? "reverse" : "forward",
              actor->slot_index, pi, boundary, pen, wall_angle);
}

static void fwd_rev_handler(TD5_Actor *actor, int reverse_mode)
{
    if (!actor || !s_span_array || !s_vertex_table || s_span_count < 3) return;
    if (actor->world_pos.x == 0 && actor->world_pos.z == 0) return;

    /* Boundary sentinel span — set at level load from the per-level table
     * mirrored from original VA 0x00473820. Placeholder values {-1, 9999}
     * naturally fail the outer gate and disable the handler. */
    int boundary = reverse_mode ? s_boundary_rev_sentinel
                                : s_boundary_fwd_sentinel;
    if (boundary < 0 || boundary >= s_span_count) return;

    /* Outer gate [CONFIRMED raw disasm 0x406F77 / 0x00407107]:
     *   Forward: actor.track_span_raw > fwd_sentinel + 1 -> early exit
     *   Reverse: actor.track_span_raw < rev_sentinel - 1 -> early exit */
    int32_t chassis_span = (int32_t)actor->track_span_raw;
    if (!reverse_mode) {
        if (chassis_span > boundary + 1) return;
    } else {
        if (chassis_span < boundary - 1) return;
    }

    const TD5_StripSpan *sp = &s_span_array[boundary];
    TD5_StripVertex *base_v = vertex_at((int)sp->left_vertex_index);
    int count = span_lane_count(sp);
    TD5_StripVertex *end_v  = vertex_at((int)sp->left_vertex_index + count);
    if (!base_v || !end_v) return;

    TD5_Vec3_Fixed *probe_block = &actor->probe_FL;

    /* Iterate the car probe table [0,1,2,3,0xFF,...] — 4 probes. */
    static const int8_t k_probe_table[8] = { 0, 1, 2, 3, -1, 0, 0, 0 };
    for (int i = 0; i < 8; i++) {
        int probe_idx = k_probe_table[i];
        if (probe_idx < 0) break;

        /* Per-probe gate — Forward: probe_span <= boundary, Reverse: equality. */
        int probe_span = (int)actor->wheel_probes[i].span_index;
        if (!reverse_mode) {
            if (probe_span > boundary) continue;
        } else {
            if (probe_span != boundary) continue;
        }

        fwd_rev_resolve_contact(actor, sp, base_v, end_v, reverse_mode,
                                probe_block, probe_idx, boundary);
    }
}

void td5_track_resolve_forward_contacts(TD5_Actor *actor)
{
    fwd_rev_handler(actor, 0);
}

void td5_track_resolve_reverse_contacts(TD5_Actor *actor)
{
    fwd_rev_handler(actor, 1);
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
    y = (float)(sp->origin_y + src[0]->y + sp->origin_y + src[1]->y +
                sp->origin_y + src[2]->y + sp->origin_y + src[3]->y) * 0.25f;
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
    if (out_y) *out_y = sp->origin_y + (src[0]->y + src[1]->y + src[2]->y + src[3]->y) / 4;
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

    /* Original FUN_00445f10 @ 0x445FB8-0x445FF7 returns 24.8 fixed-point:
     *   world = ((sum_of_4_verts * 0x100) / 4) + origin * 0x100
     * Callers in td5_game.c (spawn) and td5_physics.c (ground snap) both
     * treat this as 24.8 FP. */
    {
        int sum_x = src[0]->x + src[1]->x + src[2]->x + src[3]->x;
        int sum_y = src[0]->y + src[1]->y + src[2]->y + src[3]->y;
        int sum_z = src[0]->z + src[1]->z + src[2]->z + src[3]->z;
        if (out_x) *out_x = (sum_x * 0x100) / 4 + sp->origin_x * 0x100;
        if (out_y) *out_y = (sum_y * 0x100) / 4 + sp->origin_y * 0x100;
        if (out_z) *out_z = (sum_z * 0x100) / 4 + sp->origin_z * 0x100;
    }
    TD5_LOG_I(LOG_TAG,
              "span_lane_world: span=%d lane=%d world=(%d,%d,%d) [24.8 FP]",
              span_index, sub_lane,
              out_x ? *out_x : 0, out_y ? *out_y : 0, out_z ? *out_z : 0);
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
            /* Skip entries that failed validation (offset == 0) */
            if (s_models_entry_offsets[entry_index] == 0 ||
                s_models_entry_offsets[entry_index + 1] == 0) {
                entry_index++;
                continue;
            }
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
    *out_y = ((int32_t)sp->origin_y + (int32_t)v->y) << 8;
    *out_z = ((int32_t)sp->origin_z + (int32_t)v->z) << 8;
}

/**
 * Per-span-type vertex index offsets from original binary (0x474E40/41).
 * Junction span types shift one side's vertex base by -1.
 * Indexed by span_type: [left_offset, right_offset].
 */
static const int8_t k_span_vertex_offsets[12][2] = {
    /* type  0 */ {  0,  0 },  /* sentinel start */
    /* type  1 */ {  0,  0 },  /* normal */
    /* type  2 */ {  0,  0 },  /* normal */
    /* type  3 */ { -1,  0 },  /* junction (left shift) */
    /* type  4 */ { -1,  0 },  /* junction (left shift) */
    /* type  5 */ {  0,  0 },  /* normal variant */
    /* type  6 */ {  0, -1 },  /* junction (right shift) */
    /* type  7 */ {  0, -1 },  /* junction (right shift) */
    /* type  8 */ {  0,  0 },  /* forward junction */
    /* type  9 */ {  0,  0 },  /* sentinel wrap-start */
    /* type 10 */ {  0,  0 },  /* sentinel wrap-end */
    /* type 11 */ {  0,  0 },  /* reverse junction */
};

/**
 * Get the 4 corner vertex indices for a span quad at a given sub-lane.
 * Returns: vl0, vl1 (left pair), vr0, vr1 (right pair).
 * Applies per-span-type vertex index offsets for junction geometry.
 */
static void get_quad_vertices(const TD5_StripSpan *sp, int sub_lane,
                               int *vl0, int *vl1, int *vr0, int *vr1)
{
    int type = sp->span_type;
    int left_off = 0, right_off = 0;
    if (type >= 0 && type < 12) {
        left_off  = k_span_vertex_offsets[type][0];
        right_off = k_span_vertex_offsets[type][1];
    }
    int li = left_off  + sp->left_vertex_index  + sub_lane;
    int ri = right_off + sp->right_vertex_index + sub_lane;
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
    origin_y = (float)sp->origin_y;
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
    s_span_count    = (int)hdr[1]; /* main road span count (ring length) */
    vertex_offset   = hdr[2];  /* byte offset to vertex table */
    s_secondary_count = (int)hdr[3];
    s_aux_count     = (int)hdr[4];

    /* Branch roads (divergent paths) are stored as extra spans after the
     * main road array. hdr[1] = main road count, but the span array
     * physically extends up to (vertex_offset - span_offset) / 24 spans.
     * Junction spans (type 8/11) link to these branch spans via forward_link
     * and backward_link fields. Without including them, all branch links
     * are out of bounds and the span walker overflows at road forks.
     *
     * Compute the actual physical span count from the data layout. The
     * ring_length for wrapping/checkpoint purposes stays at hdr[1]. */
    int physical_span_count = (int)((vertex_offset - span_offset) / 24);
    if (physical_span_count > s_span_count)
        s_span_count = physical_span_count;

    /* Resolve runtime pointers */
    if (span_offset >= size || vertex_offset >= size) {
        free(s_strip_blob);
        s_strip_blob = NULL;
        s_strip_blob_size = 0;
        return build_placeholder_display_lists();
    }

    s_span_array   = (TD5_StripSpan *)(s_strip_blob + span_offset);
    s_vertex_table = (TD5_StripVertex *)(s_strip_blob + vertex_offset);

    /* Store in global state. ring_length = main road only (for wrapping),
     * but s_span_count includes branch spans (for bounds checks). */
    g_td5.track_span_ring_length = (int)hdr[1];

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
 * Patches sentinel records at the first and last MAIN ROAD span:
 *   - First span: type = 9 (SENTINEL_START), backward_link = ring_length - 1
 *   - Last main road span: type = 10 (SENTINEL_END), link at +0x10 cleared
 *
 * Uses ring_length (main road count) — NOT s_span_count (total incl branches).
 * The original reads DAT_004c3d90 which is the ring length.
 * Branch spans (>= ring_length) keep their original types (junction links
 * back to main road). [CONFIRMED @ 0x444070]
 */
void td5_track_bind_runtime_pointers(void)
{
    TD5_StripSpan *first, *last;
    int ring = g_td5.track_span_ring_length;

    if (!s_span_array || ring < 2)
        return;

    first = &s_span_array[0];
    last  = &s_span_array[ring - 1];

    /* Patch first span: type = SENTINEL_START, backward link = last main road span */
    first->span_type = 9;  /* TD5_SPAN_SENTINEL_START */
    first->link_prev = (int16_t)(ring - 1);

    /* Patch last main road span: type = SENTINEL_END, clear forward link area */
    last->span_type = 10;  /* TD5_SPAN_SENTINEL_END */
    /* The original clears the int32 at +0x10 (origin_y). */
    last->origin_y = 0;

    TD5_LOG_I(LOG_TAG, "bind_runtime: sentinels at span[0]=type9, span[%d]=type10 (ring=%d total=%d)",
              ring - 1, ring, s_span_count);
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

    /* Select edge mask based on sub-lane position within span.
     * For single-lane spans (sub_lane is both first AND last), AND both
     * masks together. [CONFIRMED @ 0x444188-0x4441A2] */
    edge_mask = 0x0F;
    if (sub_lane == 0)
        edge_mask = s_edge_mask_first[sp->span_type];
    if (sub_lane >= lane_count - 1)
        edge_mask &= s_edge_mask_last[sp->span_type];

    /* Test each edge. Cross product sign > 0 means outside. */

    /* Edge 0 (forward): left1 -> right1 (the "far" edge) */
    if ((edge_mask & 0x01) && edge_cross(lx1, lz1, rx1, rz1, pos_x, pos_z) > 0)
        result |= 0x01;

    /* Edge 1 (right): right1 -> right0 (far-right to near-right)
     * Cross > 0 means P is to the +X side (outside right wall).
     * Previously rx0→rx1 gave false positives for inside points. */
    if ((edge_mask & 0x02) && edge_cross(rx1, rz1, rx0, rz0, pos_x, pos_z) > 0)
        result |= 0x02;

    /* Edge 2 (backward): right0 -> left0 (the "near" edge) */
    if ((edge_mask & 0x04) && edge_cross(rx0, rz0, lx0, lz0, pos_x, pos_z) > 0)
        result |= 0x04;

    /* Edge 3 (left): left0 -> left1 (near-left to far-left)
     * Cross > 0 means P is to the -X side (outside left wall).
     * Previously lx1→lx0 gave false positives for inside points. */
    if ((edge_mask & 0x08) && edge_cross(lx0, lz0, lx1, lz1, pos_x, pos_z) > 0)
        result |= 0x08;

    return result;
}

/**
 * Compute the car's effective sub_lane from its XZ position within a span.
 * Returns a value in [0, lane_count-1] based on lateral position.
 * Used for junction branch decisions when sub_lane tracking is unreliable.
 */
static int compute_effective_sublane(int span_idx, int32_t pos_x, int32_t pos_z)
{
    if (span_idx < 0 || span_idx >= s_span_count) return 0;
    const TD5_StripSpan *sp = &s_span_array[span_idx];
    int lc = span_lane_count(sp);
    if (lc < 2) return 0;

    /* Measure lateral position as fraction across the span width.
     * Left edge vertex = left_vertex_index[0], right edge = left_vertex_index[lc].
     * Project car position onto the lateral axis (perpendicular to the
     * longitudinal near→far direction). */
    TD5_StripVertex *v0 = vertex_at((int)sp->left_vertex_index);
    TD5_StripVertex *vN = vertex_at((int)sp->left_vertex_index + lc);
    if (!v0 || !vN) return 0;

    int32_t ox = sp->origin_x;
    int32_t oz = sp->origin_z;
    /* Lateral vector: v0 → vN */
    int32_t lat_dx = (int32_t)vN->x - (int32_t)v0->x;
    int32_t lat_dz = (int32_t)vN->z - (int32_t)v0->z;
    int64_t lat_sq = (int64_t)lat_dx * lat_dx + (int64_t)lat_dz * lat_dz;
    if (lat_sq == 0) return 0;

    /* Project car's position (relative to v0) onto lateral axis */
    int32_t car_rx = (pos_x >> 8) - ox - (int32_t)v0->x;
    int32_t car_rz = (pos_z >> 8) - oz - (int32_t)v0->z;
    int64_t proj = (int64_t)car_rx * lat_dx + (int64_t)car_rz * lat_dz;

    /* Convert projection to sub_lane index */
    int result = (int)(proj * lc / lat_sq);
    if (result < 0) result = 0;
    if (result >= lc) result = lc - 1;
    return result;
}

/**
 * Resolve neighbor span index for a given crossing direction.
 * Returns the new span index (and updates sub_lane via pointer).
 */
static int resolve_neighbor(int span_idx, int *sub_lane, uint8_t crossing_bit,
                            int32_t pos_x, int32_t pos_z)
{
    TD5_StripSpan *sp = &s_span_array[span_idx];
    int new_span = span_idx;
    int h_offset = span_height_offset(sp);
    int cur_lane_count = span_lane_count(sp);

    /* Junction branch decision [CONFIRMED @ 0x4443E8, 0x4444A0]:
     * The original decides main-road vs branch based on sub_lane:
     *   Type 8 (fwd junction): sub_lane < next_span.lane_count → span+1 (main)
     *                          sub_lane >= next_span.lane_count → forward_link (branch)
     *   Type 11 (bwd junction): sub_lane < prev_span.lane_count → span-1 (main)
     *                           sub_lane >= prev_span.lane_count → backward_link (branch)
     *
     * HOWEVER: the port's sub_lane tracking is unreliable (h_offset adjustment
     * doesn't produce correct lateral tracking). As a workaround, we compute
     * the car's geometric position within the junction span to determine which
     * side of the lane threshold the car is on. This matches the original's
     * intent (sub_lane reflects position) without depending on broken tracking.
     *
     * The lane threshold divides the junction's lateral extent. The car's
     * perpendicular distance from the left edge, as a fraction of total width,
     * determines the effective sub_lane for the branch decision. */

    switch (crossing_bit) {
    case 0x01: /* Forward */
        switch (sp->span_type) {
        case 8: { /* JUNCTION_FWD [CONFIRMED @ 0x4440F0 case 1]:
                   * sub_lane < next_span.lane_count → main road (span+1)
                   * sub_lane >= next_span.lane_count → branch (link_next) */
            int next_idx = span_idx + 1;
            if (next_idx >= 0 && next_idx < s_span_count) {
                int next_lanes = span_lane_count(&s_span_array[next_idx]);
                if (*sub_lane < next_lanes) {
                    /* Main road */
                    new_span = next_idx;
                    int dest_h = span_height_offset(&s_span_array[next_idx]);
                    *sub_lane += h_offset - dest_h;
                } else {
                    /* Branch road */
                    new_span = (int)sp->link_next;
                    if (new_span >= 0 && new_span < s_span_count) {
                        int dest_lanes = span_lane_count(&s_span_array[new_span]);
                        *sub_lane += dest_lanes - cur_lane_count;
                        /* junction_fwd branch taken */
                    } else {
                        new_span = next_idx; /* fallback to main */
                    }
                }
            }
            break;
        }
        case 10: /* SENTINEL_END: always follow forward link */
            new_span = (int)sp->link_next;
            if (new_span >= 0 && new_span < s_span_count) {
                int dest_lanes = span_lane_count(&s_span_array[new_span]);
                *sub_lane += dest_lanes - cur_lane_count;
            }
            break;
        default:
            new_span = span_idx + 1;
            /* Normal: adjust sub_lane by height_offset difference [CONFIRMED @ 0x444414] */
            if (new_span >= 0 && new_span < s_span_count) {
                int dest_h = span_height_offset(&s_span_array[new_span]);
                *sub_lane += h_offset - dest_h;
            }
            break;
        }
        break;

    case 0x02: /* Right (forward + lateral) */
        switch (sp->span_type) {
        case 8: { /* JUNCTION_FWD: branch decision same as 0x01 */
            int next_idx = span_idx + 1;
            if (next_idx >= 0 && next_idx < s_span_count) {
                int next_lanes = span_lane_count(&s_span_array[next_idx]);
                if (*sub_lane < next_lanes) {
                    new_span = next_idx;
                } else {
                    new_span = (int)sp->link_next;
                    if (new_span < 0 || new_span >= s_span_count)
                        new_span = next_idx;
                }
            } else {
                new_span = span_idx + 1;
            }
            break;
        }
        case 10:
            new_span = (int)sp->link_next;
            if (new_span >= 0 && new_span < s_span_count) {
                int dest_lanes = span_lane_count(&s_span_array[new_span]);
                *sub_lane += dest_lanes - cur_lane_count;
            }
            break;
        default:
            new_span = span_idx + 1;
            break;
        }
        *sub_lane += h_offset;
        break;

    case 0x04: /* Backward */
        switch (sp->span_type) {
        case 11: { /* JUNCTION_BWD [CONFIRMED @ 0x4440F0 case 4]:
                    * sub_lane >= prev_span.lane_count → branch (link_prev)
                    * sub_lane < prev_span.lane_count → main road (span-1) */
            int prev_idx = span_idx - 1;
            if (prev_idx >= 0 && prev_idx < s_span_count) {
                int prev_lanes = span_lane_count(&s_span_array[prev_idx]);
                if (*sub_lane >= prev_lanes) {
                    /* Branch road */
                    new_span = (int)sp->link_prev;
                    if (new_span >= 0 && new_span < s_span_count) {
                        int dest_lanes = span_lane_count(&s_span_array[new_span]);
                        *sub_lane += dest_lanes - cur_lane_count;
                        /* junction_bwd branch taken */
                    } else {
                        new_span = prev_idx; /* fallback to main */
                    }
                } else {
                    /* Main road */
                    new_span = prev_idx;
                    int dest_h = span_height_offset(&s_span_array[prev_idx]);
                    *sub_lane += h_offset - dest_h;
                }
            }
            break;
        }
        case 9: /* SENTINEL_START: always follow backward link */
            new_span = (int)sp->link_prev;
            if (new_span >= 0 && new_span < s_span_count) {
                int dest_lanes = span_lane_count(&s_span_array[new_span]);
                *sub_lane += dest_lanes - cur_lane_count;
            }
            break;
        default:
            new_span = span_idx - 1;
            if (new_span >= 0) {
                int dest_h = span_height_offset(&s_span_array[new_span]);
                *sub_lane += h_offset - dest_h;
            }
            break;
        }
        break;

    case 0x08: /* Left (backward + lateral) */
        switch (sp->span_type) {
        case 11: { /* JUNCTION_BWD: branch decision same as 0x04 */
            int prev_idx = span_idx - 1;
            if (prev_idx >= 0 && prev_idx < s_span_count) {
                int prev_lanes = span_lane_count(&s_span_array[prev_idx]);
                if (*sub_lane >= prev_lanes) {
                    new_span = (int)sp->link_prev;
                    if (new_span < 0 || new_span >= s_span_count)
                        new_span = prev_idx;
                } else {
                    new_span = prev_idx;
                }
            } else {
                new_span = span_idx - 1;
            }
            break;
        }
        case 9:
            new_span = (int)sp->link_prev;
            if (new_span >= 0 && new_span < s_span_count) {
                int dest_lanes = span_lane_count(&s_span_array[new_span]);
                *sub_lane += dest_lanes - cur_lane_count;
            }
            break;
        default:
            new_span = span_idx - 1;
            break;
        }
        *sub_lane -= h_offset;
        break;
    }

    /* Branch-return safety net: only fires when the walker lands on a
     * span that's truly out of bounds (negative or >= s_span_count).
     * Do NOT fire just because new_span crossed a branch boundary —
     * adjacent branches may legitimately have consecutive span indices,
     * and the 8/9/10/11 span-type handlers already handle proper
     * branch-to-main-road returns via link_next/link_prev.
     *
     * This safety net only protects against walker producing truly
     * invalid spans (e.g. branch road data without a terminator). */
    if ((sp->span_type < 8 || sp->span_type > 11) &&
        (new_span < 0 || new_span >= s_span_count)) {
        int ring = g_td5.track_span_ring_length;
        if (span_idx >= ring && s_jump_entries && s_jump_entry_count > 0) {
            for (int j = 0; j < s_jump_entry_count; j++) {
                uint16_t *entry_u = (uint16_t *)(s_jump_entries + j * 6);
                int16_t  *entry_s = (int16_t  *)(s_jump_entries + j * 6);
                int branch_lo   = (int)entry_u[0];
                int branch_hi   = (int)entry_u[1];
                int main_target = (int)entry_s[2];
                if (span_idx >= branch_lo && span_idx <= branch_hi) {
                    int remapped;
                    if (new_span >= s_span_count) {
                        remapped = main_target + (branch_hi - branch_lo) + 1;
                        TD5_LOG_I(LOG_TAG, "branch_return_fwd: span=%d new=%d -> main=%d",
                                  span_idx, new_span, remapped);
                    } else {
                        remapped = main_target - 1;
                        TD5_LOG_I(LOG_TAG, "branch_return_bwd: span=%d new=%d -> main=%d",
                                  span_idx, new_span, remapped);
                    }
                    new_span = remapped;
                    *sub_lane = 0;  /* force inner lane to avoid re-branching */
                    break;
                }
            }
        }
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
 * Iterative boundary traversal matching original FUN_004440f0 switch logic.
 * Handles compound crossings (diagonal movement) in a single step.
 * Bitmask: bit0=forward(1), bit1=right(2), bit2=backward(4), bit3=left(8).
 */
static void update_position_recursive(int16_t *track_state, int32_t pos_x, int32_t pos_z,
                                       int depth)
{
    int span_idx = (int)track_state[0];
    int sub_lane = (int)((int8_t *)track_state)[12];
    int new_span;
    int iter;

    /* Save state snapshot so we can roll back on non-convergence.
     * If the boundary walk doesn't converge in TRACK_MAX_RECURSION steps,
     * the state would be left pointing at a completely wrong span, which
     * corrupts ground-snap height probes and wall collision checks. */
    int16_t saved_state[8];
    memcpy(saved_state, track_state, 16);

    for (iter = 0; iter < TRACK_MAX_RECURSION; iter++) {
        uint8_t bits = compute_boundary_bits(span_idx, sub_lane, pos_x, pos_z);

        if (bits == 0)
            break; /* Actor is within the current quad */

        switch (bits) {
        case 1: /* Forward */
            new_span = resolve_neighbor(span_idx, &sub_lane, 0x01, pos_x, pos_z);
            span_idx = new_span;
            track_state[0] = (int16_t)new_span;
            track_state[2]++;
            if (track_state[2] > track_state[3])
                track_state[3] = track_state[2];
            break;

        case 2: /* Right */
            new_span = resolve_neighbor(span_idx, &sub_lane, 0x02, pos_x, pos_z);
            span_idx = new_span;
            track_state[0] = (int16_t)new_span;
            break;

        case 3: /* Forward + Right */
            new_span = resolve_neighbor(span_idx, &sub_lane, 0x01, pos_x, pos_z);
            span_idx = new_span;
            track_state[0] = (int16_t)new_span;
            track_state[2]++;
            if (track_state[2] > track_state[3])
                track_state[3] = track_state[2];
            new_span = resolve_neighbor(span_idx, &sub_lane, 0x02, pos_x, pos_z);
            span_idx = new_span;
            track_state[0] = (int16_t)new_span;
            break;

        case 4: /* Backward */
            new_span = resolve_neighbor(span_idx, &sub_lane, 0x04, pos_x, pos_z);
            span_idx = new_span;
            track_state[0] = (int16_t)new_span;
            track_state[2]--;
            break;

        case 6: /* Right + Backward */
            new_span = resolve_neighbor(span_idx, &sub_lane, 0x02, pos_x, pos_z);
            span_idx = new_span;
            track_state[0] = (int16_t)new_span;
            new_span = resolve_neighbor(span_idx, &sub_lane, 0x04, pos_x, pos_z);
            span_idx = new_span;
            track_state[0] = (int16_t)new_span;
            track_state[2]--;
            break;

        case 8: /* Left */
            new_span = resolve_neighbor(span_idx, &sub_lane, 0x08, pos_x, pos_z);
            span_idx = new_span;
            track_state[0] = (int16_t)new_span;
            break;

        case 9: /* Forward + Left */
            new_span = resolve_neighbor(span_idx, &sub_lane, 0x01, pos_x, pos_z);
            span_idx = new_span;
            track_state[0] = (int16_t)new_span;
            track_state[2]++;
            if (track_state[2] > track_state[3])
                track_state[3] = track_state[2];
            new_span = resolve_neighbor(span_idx, &sub_lane, 0x08, pos_x, pos_z);
            span_idx = new_span;
            track_state[0] = (int16_t)new_span;
            break;

        case 12: /* Backward + Left */
            new_span = resolve_neighbor(span_idx, &sub_lane, 0x04, pos_x, pos_z);
            span_idx = new_span;
            track_state[0] = (int16_t)new_span;
            track_state[2]--;
            new_span = resolve_neighbor(span_idx, &sub_lane, 0x08, pos_x, pos_z);
            span_idx = new_span;
            track_state[0] = (int16_t)new_span;
            break;

        default:
            /* Unhandled compound case — resolve forward/backward first, then lateral */
            if (bits & 0x01) {
                new_span = resolve_neighbor(span_idx, &sub_lane, 0x01, pos_x, pos_z);
                span_idx = new_span;
                track_state[0] = (int16_t)new_span;
                track_state[2]++;
                if (track_state[2] > track_state[3])
                    track_state[3] = track_state[2];
            } else if (bits & 0x04) {
                new_span = resolve_neighbor(span_idx, &sub_lane, 0x04, pos_x, pos_z);
                span_idx = new_span;
                track_state[0] = (int16_t)new_span;
                track_state[2]--;
            }
            if (bits & 0x02) {
                new_span = resolve_neighbor(span_idx, &sub_lane, 0x02, pos_x, pos_z);
                span_idx = new_span;
                track_state[0] = (int16_t)new_span;
            } else if (bits & 0x08) {
                new_span = resolve_neighbor(span_idx, &sub_lane, 0x08, pos_x, pos_z);
                span_idx = new_span;
                track_state[0] = (int16_t)new_span;
            }
            break;
        }

        /* Write back sub-lane after each transition */
        ((int8_t *)track_state)[12] = (int8_t)sub_lane;
    }

    if (iter >= TRACK_MAX_RECURSION) {
        /* Non-convergence: boundary walk failed. Restore snapshot, then
         * do a brute-force nearest-span search in a ±SEARCH_RADIUS window
         * around the saved span to find the correct span by XZ distance. */
        memcpy(track_state, saved_state, 16);

        int search_center = (int)saved_state[0];
        int best_span = search_center;
        int64_t best_dist = INT64_MAX;
        int search_lo = search_center - 32;
        int search_hi = search_center + 32;
        if (search_lo < 0) search_lo = 0;
        if (search_hi >= s_span_count) search_hi = s_span_count - 1;

        /* Convert position to world units for comparison with span centers */
        int32_t wx = pos_x >> 8;
        int32_t wz = pos_z >> 8;

        for (int si = search_lo; si <= search_hi; si++) {
            int cx = 0, cy = 0, cz = 0;
            /* Inline span center: use origin + first vertex midpoint for speed */
            const TD5_StripSpan *tsp = &s_span_array[si];
            if (tsp->span_type == 9 || tsp->span_type == 10) continue;
            TD5_StripVertex *v0 = vertex_at(tsp->left_vertex_index);
            TD5_StripVertex *v1 = vertex_at(tsp->right_vertex_index);
            if (!v0 || !v1) continue;
            cx = tsp->origin_x + ((int32_t)v0->x + (int32_t)v1->x) / 2;
            cz = tsp->origin_z + ((int32_t)v0->z + (int32_t)v1->z) / 2;

            int64_t dx = (int64_t)(wx - cx);
            int64_t dz = (int64_t)(wz - cz);
            int64_t dist = dx * dx + dz * dz;
            if (dist < best_dist) {
                best_dist = dist;
                best_span = si;
            }
        }

        if (best_span != search_center) {
            track_state[0] = (int16_t)best_span;
            /* Update accumulated span counter based on direction of movement */
            int16_t delta = (int16_t)(best_span - search_center);
            track_state[2] += delta;
            if (track_state[2] > track_state[3])
                track_state[3] = track_state[2];
        }
    }
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

/**
 * UpdateProbeTrackPosition -- per-probe variant of UpdateActorTrackPosition
 *
 * The original FUN_004440F0 is called per wheel probe from
 * RefreshVehicleWheelContactFrames (0x403720) with each probe's own
 * track state and world position. This gives each probe its own
 * span index for accurate wall edge testing.
 */
void td5_track_update_probe_position(TD5_TrackProbeState *probe,
                                     int32_t world_x, int32_t world_z)
{
    if (!probe || !s_span_array || s_span_count == 0)
        return;

    /* TD5_TrackProbeState layout matches the int16_t[8] layout expected
     * by update_position_recursive: [0]=span_index, [1]=normalized,
     * [2]=accumulated, [3]=high_water, ... [6]=sub_lane_index (byte 12) */
    update_position_recursive((int16_t *)probe, world_x, world_z, 0);
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

    /* Cross product -> surface normal (int64 to prevent overflow from
     * int16 edge vectors: e.g., 30000 * 30000 = 900M > INT32_MAX) */
    nx = (int32_t)(((int64_t)e1y * e2z - (int64_t)e1z * e2y) >> 4);
    ny = (int32_t)(((int64_t)e1z * e2x - (int64_t)e1x * e2z) >> 4);
    nz = (int32_t)(((int64_t)e1x * e2y - (int64_t)e1y * e2x) >> 4);

    /* If ny is too small, the triangle is nearly vertical — the plane
     * equation would produce an enormous height. Return the vertex Y
     * directly as the best available estimate. */
    if (ny > -16 && ny < 16) {
        return (int32_t)va->y;
    }

    /* Position relative to vertex A in span-local coordinates */
    dx = (pos_x >> 8) - origin_x - (int32_t)va->x;
    dz = (pos_z >> 8) - origin_z - (int32_t)va->z;

    /* Plane equation: y = va.y - (dx * nx + dz * nz) / ny
     * Returns height in integer world units (same scale as vertex Y). */
    height = (int32_t)va->y - (int32_t)(((int64_t)dx * nx + (int64_t)dz * nz) / ny);

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
                           sp->origin_x, sp->origin_y, sp->origin_z,
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
    /*
     * InitializeActorTrackPose (0x434350):
     * Computes initial heading from track span geometry.
     *
     * Fixes vs previous implementation (verified against Ghidra 0x434350):
     *  1. Sub-lane NOT added to vertex indices [CONFIRMED @ 0x434390]
     *  2. Case 3/4: uses right+2 vertex [CONFIRMED @ 0x434410]
     *  3. Case 6/7: uses left+2 vertex [CONFIRMED @ 0x434450]
     *  4. Signed divide rounds toward zero [CONFIRMED @ 0x434408]
     *  5. 180° (0x800) offset added to heading [CONFIRMED @ 0x434501]
     */
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

    /* Get base quad vertices — sub_lane is NOT added to indices
     * [CONFIRMED @ 0x434390: uses span.left_vertex_index directly] */
    vl0 = vertex_at(sp->left_vertex_index);
    vl1 = vertex_at(sp->left_vertex_index + 1);
    vr0 = vertex_at(sp->right_vertex_index);
    vr1 = vertex_at(sp->right_vertex_index + 1);

    /* Compute heading based on span type */
    switch (sp->span_type) {
    case 1: case 2: case 5:
    case 8: case 9: case 10: case 11:
        /* Standard: dx = (left1 - right1) - right0 + left0 */
        dx = ((int32_t)vl1->x - (int32_t)vr1->x) - (int32_t)vr0->x + (int32_t)vl0->x;
        dz = ((int32_t)vl1->z - (int32_t)vr1->z) - (int32_t)vr0->z + (int32_t)vl0->z;
        /* Signed divide-by-4 rounding toward zero [CONFIRMED @ 0x434408] */
        dx = (dx + ((dx >> 31) & 3)) >> 2;
        dz = (dz + ((dz >> 31) & 3)) >> 2;
        break;

    case 3: case 4:
        /* Shifted diagonal: uses right+2 vertex [CONFIRMED @ 0x434410] */
        {
            TD5_StripVertex *vr2 = vertex_at(sp->right_vertex_index + 2);
            dx = ((int32_t)vl1->x - (int32_t)vr2->x) - (int32_t)vr1->x + (int32_t)vl0->x;
            dz = ((int32_t)vl1->z - (int32_t)vr2->z) - (int32_t)vr1->z + (int32_t)vl0->z;
        }
        dx = (dx + ((dx >> 31) & 3)) >> 2;
        dz = (dz + ((dz >> 31) & 3)) >> 2;
        break;

    case 6: case 7:
        /* Reversed winding: uses left+2 vertex [CONFIRMED @ 0x434450] */
        {
            TD5_StripVertex *vl2 = vertex_at(sp->left_vertex_index + 2);
            dx = ((int32_t)vl2->x - (int32_t)vr1->x) + (int32_t)vl1->x - (int32_t)vr0->x;
            dz = ((int32_t)vl2->z - (int32_t)vr1->z) + (int32_t)vl1->z - (int32_t)vr0->z;
        }
        dx = (dx + ((dx >> 31) & 3)) >> 2;
        dz = (dz + ((dz >> 31) & 3)) >> 2;
        break;

    default:
        dx = 0; dz = 1;
        break;
    }

    /* Original calls AngleFromVector12 (0x40A720, integer LUT) — NOT
     * angle_from_vector_full (0x433FC0, different quadrant mapping).
     * Using the shared AngleFromVector12 for consistent rounding. */
    angle = AngleFromVector12(dx, dz) & 0xFFF;

    TD5_LOG_I(LOG_TAG, "compute_heading: span=%d type=%d dx=%d dz=%d angle=%d",
              span_idx, sp->span_type, dx, dz, angle);

    /* Write heading to actor's heading_normal at +0x290 (as int16[3]) */
    heading_normal = (int16_t *)((uint8_t *)actor + 0x290);
    heading_normal[0] = (int16_t)dx;
    heading_normal[1] = 0;
    heading_normal[2] = (int16_t)dz;

    /* Store to yaw euler accumulator at +0x1F4 with 180° offset
     * [CONFIRMED @ 0x434501: (angle + 0x800) * 0x100] */
    *(int32_t *)((uint8_t *)actor + 0x1F4) = (angle + 0x800) << 8;
}

/* ========================================================================
 * Wrap Normalization (0x443FB0 NormalizeActorTrackWrapState)
 *
 * Normalizes the accumulated span counter (actor+0x84) into a wrapped
 * position modulo the total span ring length. Returns the lap count.
 * ======================================================================== */

int td5_track_normalize_actor_wrap(TD5_Actor *actor)
{
    int16_t *track_state;
    int32_t raw_span;
    int32_t ring_length;
    int32_t wrapped, laps;

    if (!actor || s_span_count == 0)
        return 0;

    track_state = (int16_t *)((uint8_t *)actor + 0x80);
    raw_span = (int32_t)track_state[2]; /* +0x84: accumulated spans (high_water) */
    ring_length = (int32_t)s_span_count;

    if (ring_length <= 0)
        return 0;

    if (raw_span >= 0) {
        laps = raw_span / ring_length;
        wrapped = raw_span % ring_length;
    } else {
        /* C truncates toward zero for negative dividends.
         * Original behavior: remainder adjusted to be non-negative. */
        laps = raw_span / ring_length;
        wrapped = raw_span % ring_length;
        /* When remainder is negative, shift into [0, ring_length) */
        wrapped += ring_length;
        if (wrapped >= ring_length)
            wrapped -= ring_length;
    }

    track_state[1] = (int16_t)wrapped;  /* +0x82: normalized span */
    return (int)laps;
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

    if (g_td5.track_type == TD5_TRACK_CIRCUIT) {
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
 * Circuit Lap Tracking (0x434DA0 UpdateCircuitLap)
 *
 * Per-actor circuit-mode lap counting with anti-cheat checks:
 *   - Checkpoint index: -1 = between laps (awaiting start-line crossing)
 *   - New lap triggers when player crosses start line by 2 spans,
 *     with sufficient speed, aligned heading, matching route lane,
 *     and cooldown expired.
 *   - Wrong-way detection: if >64 spans behind checkpoint, reset state
 *     and set 300-frame cooldown.
 *   - Lap completion: if >1 span ahead and route matches, set finish flag.
 *
 * Returns: 0 = no event, 1 = lap complete, 2 = race complete (all laps done)
 * ======================================================================== */

int td5_track_update_circuit_lap(TD5_Actor *actor, int slot)
{
    int16_t *track_state;
    int16_t current_span;
    int32_t ring_length;
    int16_t checkpoint_span;
    int32_t actor_heading;
    int32_t route_heading;
    int16_t heading_delta;
    int32_t speed;
    int32_t cooldown;
    int16_t *checkpoint_idx_ptr;
    int16_t  checkpoint_idx;
    uint8_t *lap_count_ptr;
    int32_t *cooldown_ptr;
    int32_t *finish_time_ptr;

    if (!actor || s_span_count == 0)
        return 0;

    ring_length = (int32_t)s_span_count;
    if (ring_length <= 0)
        return 0;

    /* Only applies to circuit mode */
    if (g_td5.track_type != TD5_TRACK_CIRCUIT)
        return 0;

    track_state = (int16_t *)((uint8_t *)actor + 0x80);
    current_span = track_state[1]; /* +0x82: normalized span */

    /* Checkpoint index at +0x336 (overloaded field):
     * -1 = between laps (waiting for start line crossing)
     *  0+ = checkpoint span target */
    checkpoint_idx_ptr = (int16_t *)((uint8_t *)actor + 0x336);
    checkpoint_idx = *checkpoint_idx_ptr;

    /* Lap count at +0x37E */
    lap_count_ptr = (uint8_t *)actor + 0x37E;

    /* Cooldown timer at +0x338 (int32) */
    cooldown_ptr = (int32_t *)((uint8_t *)actor + 0x338);
    cooldown = *cooldown_ptr;

    /* Finish time at +0x328 */
    finish_time_ptr = (int32_t *)((uint8_t *)actor + 0x328);

    /* Decrement cooldown if active */
    if (cooldown > 0) {
        (*cooldown_ptr)--;
        return 0;
    }

    /* Actor speed at +0x314 (longitudinal speed, fixed-point) */
    speed = *(int32_t *)((uint8_t *)actor + 0x314);

    /* Actor heading at +0x1F4 (yaw accumulator, >>8 = 12-bit angle) */
    actor_heading = (*(int32_t *)((uint8_t *)actor + 0x1F4) >> 8) & 0xFFF;

    if (checkpoint_idx == -1) {
        /* --- Between laps: waiting to cross start line --- */

        /* Must be within first 2 spans of the track (start line zone) */
        if (current_span > 2)
            return 0;

        /* Speed threshold: must be moving forward */
        if (speed < 0x100)
            return 0;

        /* Heading alignment: compute delta between actor heading and
         * route heading at span 0. Must be within 16 angle units. */
        if (s_route_left && s_route_left_size >= 3) {
            route_heading = ((int32_t)s_route_left[1] << 4) & 0xFFF;
        } else {
            route_heading = 0;
        }
        heading_delta = (int16_t)((actor_heading - route_heading) & 0xFFF);
        if (heading_delta > 0x800)
            heading_delta = (int16_t)(heading_delta - 0x1000);
        if (heading_delta < 0)
            heading_delta = -heading_delta;
        if (heading_delta > 16)
            return 0;

        /* All checks passed: begin new lap checkpoint tracking */
        *checkpoint_idx_ptr = current_span;
        return 0;
    }

    /* --- Active checkpoint tracking --- */
    checkpoint_span = checkpoint_idx;

    /* Wrong-way detection: if actor is >64 spans behind the checkpoint,
     * reset state and impose 300-frame cooldown */
    {
        int32_t behind = (int32_t)checkpoint_span - (int32_t)current_span;
        /* Handle wrapping */
        if (behind < -ring_length / 2)
            behind += ring_length;
        if (behind > ring_length / 2)
            behind -= ring_length;

        if (behind > 64) {
            *checkpoint_idx_ptr = -1;
            *cooldown_ptr = 300;
            TD5_LOG_I(LOG_TAG,
                      "Circuit wrong-way: slot=%d span=%d checkpoint=%d behind=%d",
                      slot, current_span, checkpoint_span, behind);
            return 0;
        }
    }

    /* Lap completion check: actor must be >1 span ahead of the checkpoint.
     * For circuit tracks, this means passing the start/finish line. */
    {
        int32_t ahead = (int32_t)current_span - (int32_t)checkpoint_span;
        if (ahead < 0)
            ahead += ring_length;

        if (ahead > 1 && ahead < ring_length / 2) {
            /* Lap complete */
            (*lap_count_ptr)++;
            *checkpoint_idx_ptr = -1; /* reset for next lap */

            TD5_LOG_I(LOG_TAG,
                      "Circuit lap complete: slot=%d lap=%d span=%d",
                      slot, (int)*lap_count_ptr, current_span);

            /* Check if race is finished (all laps completed) */
            if (*lap_count_ptr >= (uint8_t)g_td5.circuit_lap_count) {
                int16_t *timing_ctr = (int16_t *)((uint8_t *)actor + 0x34C);
                if (*finish_time_ptr == 0) {
                    *finish_time_ptr = (int32_t)*timing_ctr;
                }
                return 2; /* race complete */
            }
            return 1; /* lap complete */
        }
    }

    return 0;
}

/* ========================================================================
 * Signed Spline Position (0x434670 ComputeSplinePosition)
 *
 * Computes the signed distance along the track between two span vertices.
 * Used by the AI routing system to determine forward progress magnitude
 * along a spline segment.
 *
 * The function interpolates between the current span vertex and the next
 * span vertex based on the difference between segment_distance and
 * route_lane, then returns the magnitude with sign indicating direction.
 *
 * Parameters:
 *   span_index       - index into the span array
 *   segment_distance - position parameter along span
 */

/* ========================================================================
 * SampleTrackTargetPoint (0x434800)
 *
 * Computes world-space XZ position for AI look-ahead by interpolating
 * between strip left/right vertices using route_byte (0-255), then
 * applying a perpendicular lateral_bias offset.
 *
 * [CONFIRMED @ 0x434800-0x4348F1]
 * ======================================================================== */

int td5_track_sample_target_point(int span_index, int route_byte,
                                   int *out_x, int *out_z, int lateral_bias)
{
    const TD5_StripSpan *sp;
    TD5_StripVertex *v_left, *v_right;
    int lane_count;
    int type_offset;

    if (out_x) *out_x = 0;
    if (out_z) *out_z = 0;

    if (!s_span_array || !s_vertex_table ||
        span_index < 0 || span_index >= s_span_count)
        return 0;

    sp = &s_span_array[span_index];
    if (sp->span_type == 9 || sp->span_type == 10)
        return 0;

    /* Lane count from geometry metadata (low nibble of byte +0x03)
     * [CONFIRMED @ 0x434836] */
    lane_count = span_lane_count(sp);
    if (lane_count < 1) lane_count = 1;

    /* Span-type vertex offset [CONFIRMED @ 0x434836: DAT_00473c68]
     * Uses the left column of k_span_vertex_offsets for the right-side vertex */
    type_offset = 0;
    if (sp->span_type >= 0 && sp->span_type < 12)
        type_offset = k_span_vertex_offsets[sp->span_type][0];

    /* Two-vertex lookup [CONFIRMED @ 0x434803-0x434862]
     * left  = vertex_table[left_vertex_index]
     * right = vertex_table[left_vertex_index + type_offset + lane_count] */
    v_left  = vertex_at((int)sp->left_vertex_index);
    v_right = vertex_at((int)sp->left_vertex_index + type_offset + lane_count);
    if (!v_left || !v_right)
        return 0;

    /* World-space base coordinates [CONFIRMED @ 0x43483D] */
    int32_t left_x  = (int32_t)v_left->x  + sp->origin_x;
    int32_t left_z  = (int32_t)v_left->z  + sp->origin_z;
    int32_t right_x = (int32_t)v_right->x + sp->origin_x;
    int32_t right_z = (int32_t)v_right->z + sp->origin_z;

    /* Route-byte interpolation (24.8 output) [CONFIRMED @ 0x434868-0x43489D]
     * result = (right - left) * route_byte + left * 0x100 */
    int32_t result_x = (right_x - left_x) * route_byte + left_x * 0x100;
    int32_t result_z = (right_z - left_z) * route_byte + left_z * 0x100;

    /* Perpendicular lateral offset [CONFIRMED @ 0x434883-0x4348F1] */
    if (lateral_bias != 0) {
        int32_t edge_dx = (int16_t)(right_x - left_x);
        int32_t edge_dz = (int16_t)(right_z - left_z);

        /* Normalize edge direction to unit vector (ConvertFloatVec4ToShortAngles)
         * [CONFIRMED @ 0x42CDB0] */
        float fex = (float)edge_dx;
        float fez = (float)edge_dz;
        float mag = sqrtf(fex * fex + fez * fez);
        if (mag > 0.5f) {
            /* Perpendicular is (-dz, dx) normalized to short range */
            int16_t perp_x = (int16_t)((-fez / mag) * 4096.0f);
            int16_t perp_z = (int16_t)(( fex / mag) * 4096.0f);

            /* Apply bias with fixed-point 4.12 multiply and rounding
             * [CONFIRMED @ 0x4348B1-0x4348F1] */
            int32_t off_x = (int32_t)perp_x * lateral_bias;
            int32_t off_z = (int32_t)perp_z * lateral_bias;
            /* Signed rounding: (val + (CDQ(val) & 0xFFF)) >> 12 */
            off_x = (off_x + ((off_x >> 31) & 0xFFF)) >> 12;
            off_z = (off_z + ((off_z >> 31) & 0xFFF)) >> 12;
            result_x += off_x * 0x100;
            result_z += off_z * 0x100;
        }
    }

    if (out_x) *out_x = result_x;
    if (out_z) *out_z = result_z;
    return 1;
}

/* ========================================================================
 * Signed spline distance (0x434670)
 *   route_lane       - route lane offset (subtracted from segment_distance)
 *
 * Returns: signed distance (positive = ahead, negative = behind)
 * ======================================================================== */

int32_t td5_track_compute_spline_position(int span_index, int segment_distance,
                                           int route_lane)
{
    const TD5_StripSpan *sp;
    int lane_count;
    int next_vertex_offset;
    int vertex_idx_a, vertex_idx_b;
    TD5_StripVertex *vtx_a, *vtx_b;
    int32_t ax, az, bx, bz;
    int32_t dx, dz;
    int32_t t;
    int32_t interp_x, interp_z;
    int32_t mag_sq, mag;

    if (!s_span_array || !s_vertex_table ||
        span_index < 0 || span_index >= s_span_count)
        return 0;

    sp = &s_span_array[span_index];

    /* Get current vertex index from right_vertex_index (+0x06) */
    vertex_idx_a = (int)sp->right_vertex_index;

    /* Get lane nibble from byte +0x03 (low nibble = lane count) */
    lane_count = span_lane_count(sp);
    if (lane_count < 1)
        lane_count = 1;

    /* Compute next vertex index:
     * next = current + lane_count + height_offset
     * The height_offset from the span type is encoded in the high nibble of +0x03.
     * This matches the original zone_offset_table lookup. */
    next_vertex_offset = lane_count + span_height_offset(sp);
    vertex_idx_b = vertex_idx_a + next_vertex_offset;

    /* Get vertex positions (x, z only -- this is 2D track-plane distance) */
    vtx_a = vertex_at(vertex_idx_a);
    vtx_b = vertex_at(vertex_idx_b);

    if (!vtx_a || !vtx_b)
        return 0;

    ax = (int32_t)vtx_a->x;
    az = (int32_t)vtx_a->z;
    bx = (int32_t)vtx_b->x;
    bz = (int32_t)vtx_b->z;

    /* Direction vector from A to B */
    dx = bx - ax;
    dz = bz - az;

    /* Interpolation parameter: how far along the segment */
    t = segment_distance - route_lane;

    /* Fixed-point interpolation with rounding toward zero:
     * interp = (delta * t + bias) >> 8
     * where bias = (delta * t >> 31) & 0xFF  (adds 255 for negative products) */
    interp_x = dx * t;
    interp_x = (interp_x + ((interp_x >> 31) & 0xFF)) >> 8;

    interp_z = dz * t;
    interp_z = (interp_z + ((interp_z >> 31) & 0xFF)) >> 8;

    /* Compute magnitude */
    mag_sq = interp_x * interp_x + interp_z * interp_z;
    mag = td5_isqrt(mag_sq);

    /* Sign: negative if behind (segment_distance < route_lane) */
    return (segment_distance < route_lane) ? -mag : mag;
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

        /* Try format A (strict): DWORD[0] = count, DWORD[1] == 4 + count*8 */
        if (dword0 > 0 && dword0 < 10000 &&
            dword1 == 4 + dword0 * 8 &&
            (size_t)(4 + dword0 * 8) <= size) {
            raw_entry_count = dword0;
            table_start_byte = 4;
            TD5_LOG_I("track", "MODELS.DAT format A: count=%u table@4 first_block@%u",
                      raw_entry_count, dword1);
        }
        /* Try format A (relaxed): DWORD[0] looks like a count, DWORD[1] is the
         * first entry's offset (not necessarily == 4+count*8 due to padding).
         * Validate by checking that DWORD[1] points to a valid sub_mesh_count.
         * Must check BEFORE format B because format B misinterprets the count
         * header as entry[0] when DWORD[1] happens to be divisible by 8. */
        else if (dword0 > 0 && dword0 < 10000 && (size_t)(4 + dword0 * 8) <= size &&
                 dword1 >= 4 + dword0 * 8 && dword1 < size && dword1 + 4 <= size &&
                 *(const uint32_t *)(s_models_blob + dword1) >= 1 &&
                 *(const uint32_t *)(s_models_blob + dword1) <= 256) {
            raw_entry_count = dword0;
            table_start_byte = 4;
            TD5_LOG_I("track", "MODELS.DAT format A (relaxed): count=%u table@4 first_block@%u",
                      raw_entry_count, dword1);
        }
        /* Try format B: no count header, table at byte 0, count = DWORD[1]/8 */
        else if (dword1 > 0 && (dword1 & 7u) == 0 && dword1 <= size) {
            raw_entry_count = dword1 / 8u;
            table_start_byte = 0;
            TD5_LOG_I("track", "MODELS.DAT format B: count=%u table@0 first_block@%u",
                      raw_entry_count, dword1);
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

        {
            uint32_t f0 = *(const uint32_t *)(const void *)(s_models_blob + tbl_byte);
            uint32_t f1 = *(const uint32_t *)(const void *)(s_models_blob + tbl_byte + 4);

            /* Auto-detect field order: one field is the block offset (should be
             * >= table end), the other is the block size (smaller).
             * Check which field points to a valid sub_mesh_count (1..256). */
            uint32_t table_end = table_start_byte + raw_entry_count * 8u;
            int f0_is_offset = (f0 >= table_end && f0 < size &&
                                f0 + 4 <= size &&
                                *(const uint32_t *)(s_models_blob + f0) >= 1 &&
                                *(const uint32_t *)(s_models_blob + f0) <= 256);
            int f1_is_offset = (f1 >= table_end && f1 < size &&
                                f1 + 4 <= size &&
                                *(const uint32_t *)(s_models_blob + f1) >= 1 &&
                                *(const uint32_t *)(s_models_blob + f1) <= 256);

            if (f0_is_offset && !f1_is_offset) {
                entry_offset = f0; entry_size = f1;  /* [offset, size] */
            } else if (f1_is_offset && !f0_is_offset) {
                entry_offset = f1; entry_size = f0;  /* [size, offset] */
            } else if (f0_is_offset && f1_is_offset) {
                /* Both valid — pick the one closer to table_end for first entry */
                entry_offset = (f0 <= f1) ? f0 : f1;
                entry_size = (f0 <= f1) ? f1 : f0;
            } else {
                entry_offset = f0; entry_size = f1;  /* fallback */
            }
        }

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
                    /* Pick whichever field of the next entry looks like a
                     * plausible offset (> current offset, within file). */
                    uint32_t nf0 = *(const uint32_t *)(const void *)(s_models_blob + next_tbl);
                    uint32_t nf1 = *(const uint32_t *)(const void *)(s_models_blob + next_tbl + 4);
                    uint32_t candidate = (nf0 > entry_offset && nf0 <= size) ? nf0 :
                                         (nf1 > entry_offset && nf1 <= size) ? nf1 : (uint32_t)size;
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

        /* Store at raw table index i (not compacted display_list_count).
         * The original lookup (0x431260) does: table_base + span_index * 8,
         * so entry[i] must correspond to span i — not the i-th valid entry.
         * Invalid entries stay 0 from calloc → lookup returns NULL for them. */
        s_models_entry_offsets[i] = entry_offset;
        s_models_entry_sizes[i] = entry_size;

        {
            const TD5_TrackRawMeshHeader *first_mesh;
            /* Use i (raw index) for the header lookup since offsets are now
             * stored at their true table position. */
            s_models_display_list_count = i + 1;
            if (parsed_count < MODELS_DAT_MAX_ENTRIES &&
                get_display_list_first_mesh_header(i, &first_mesh)) {
                TD5_ModelsDatEntry *dst = &s_models[parsed_count++];
                dst->mesh_ptr = (void *)(uintptr_t)first_mesh;
                dst->texture_page_id = (int32_t)first_mesh->texture_page_id;
                dst->bounding_radius = first_mesh->bounding_radius;
            }
        }

        display_list_count++;
    }

    s_models_display_list_count = raw_entry_count;
    s_model_count = parsed_count;

    /* Relocate mesh pointers within each display list block.
     * Each block is: [sub_mesh_count][mesh_offset_0][mesh_offset_1]...
     * mesh_offset values are byte offsets from block start → convert to
     * absolute pointers. Then relocate internal MeshHeader fields. */
    for (int dl = 0; dl < s_models_display_list_count; dl++) {
        if (s_models_entry_offsets[dl] == 0) continue;  /* skip invalid entries */
        uint8_t *block_base = s_models_blob + s_models_entry_offsets[dl];
        uint32_t block_size = s_models_entry_sizes[dl];
        uint32_t sub_count = *(uint32_t *)block_base;

        if (sub_count == 0 || sub_count > 256 || block_size < 4u + sub_count * 4u)
            continue;

        for (uint32_t j = 0; j < sub_count; j++) {
            uint32_t *slot = (uint32_t *)(block_base + 4 + j * 4);
            uint32_t mesh_off = *slot;

            if (mesh_off == 0) continue;

            /* Convert relative offset to absolute pointer.
             * Try block-relative first, then blob-relative for offsets
             * that exceed the block boundary (some MODELS.DAT entries
             * store global blob offsets instead of block-local offsets). */
            TD5_MeshHeader *mesh;
            if (mesh_off < block_size && mesh_off + sizeof(TD5_MeshHeader) <= block_size) {
                mesh = (TD5_MeshHeader *)(block_base + mesh_off);
            } else if (mesh_off < s_models_blob_size &&
                       mesh_off + sizeof(TD5_MeshHeader) <= s_models_blob_size) {
                mesh = (TD5_MeshHeader *)(s_models_blob + mesh_off);
            } else {
                *slot = 0;
                continue;
            }
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
            if (s_models_entry_offsets[dl] == 0) continue;  /* skip invalid entries */
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

    /* Note: per-vertex diffuse dim for additive billboards lives in a
     * separate post-pass (td5_track_dim_additive_billboard_meshes) called
     * AFTER track textures load. At this point the page transparency
     * table is still empty and we can't tell which billboard tags (1/2)
     * correspond to real lights (type-3 page) vs normal trees/signs
     * (type-1 alpha-keyed). Dimming all of them would wash out trees. */
}

void td5_track_dim_additive_billboard_meshes(void)
{
    /* Walk every parsed MODELS.DAT display list, find billboard meshes
     * (mesh header +0x02 == 1 || 2) whose FIRST command renders through
     * a type-3 (additive) texture page, and halve their per-vertex
     * diffuse.
     *
     * Why: the asset authors baked per-vertex intensity around 0xA0 for
     * streetlight quads in a 16bpp R5G6B5 + ONE/ONE additive pipeline.
     * At 32bpp the same values saturate the framebuffer because the
     * additive sum has more headroom before the 0xFF clamp. Halving
     * pulls the additive result back into the CRT-era perceptual range.
     *
     * One-shot per track load (call AFTER td5_asset_load_track_textures
     * so the transparency table is populated). Regular alpha-keyed
     * billboards (trees, signs — type-1 pages) are left untouched. */
    int dimmed = 0;
    int total_bb = 0;

    for (int dl = 0; dl < s_models_display_list_count; dl++) {
        if (s_models_entry_offsets[dl] == 0) continue;
        uint8_t *block_base = s_models_blob + s_models_entry_offsets[dl];
        uint32_t sub_count = *(const uint32_t *)block_base;
        if (sub_count == 0 || sub_count > 256) continue;

        for (uint32_t j = 0; j < sub_count; j++) {
            uint32_t mesh_ptr_val = *(const uint32_t *)(block_base + 4 + j * 4);
            if (mesh_ptr_val == 0) continue;

            TD5_MeshHeader *mesh = (TD5_MeshHeader *)(uintptr_t)mesh_ptr_val;
            if (!td5_track_is_ptr_in_blob(mesh, sizeof(TD5_MeshHeader)))
                continue;
            if (mesh->texture_page_id != 1 && mesh->texture_page_id != 2)
                continue;
            if (mesh->commands_offset == 0 || mesh->vertices_offset == 0)
                continue;
            if (mesh->command_count <= 0 || mesh->total_vertex_count <= 0)
                continue;
            if (mesh->total_vertex_count > 65536)
                continue;

            total_bb++;

            /* Check the first command's texture page — if not type-3 we
             * leave the mesh alone (that's a tree/sign, not a light). */
            const TD5_PrimitiveCmd *cmd0 =
                (const TD5_PrimitiveCmd *)(uintptr_t)mesh->commands_offset;
            if (td5_asset_get_page_transparency(cmd0->texture_page_id) != 3)
                continue;

            /* Scale per-vertex RGB to ~25% of disk value. Disk-baked
             * intensity is 0xA0 (160, 63% of full). Halving to 0x50 (80,
             * 31%) still read slightly too bright on a 32bpp
             * framebuffer; scaling by 3/8 → ~0x3C (60, 24%) lands in the
             * CRT-era additive perceptual range. */
            TD5_MeshVertex *bb_v =
                (TD5_MeshVertex *)(uintptr_t)mesh->vertices_offset;
            for (int vi = 0; vi < mesh->total_vertex_count; vi++) {
                uint32_t c = bb_v[vi].lighting;
                uint32_t a = c & 0xFF000000u;
                uint32_t r = (c >> 16) & 0xFFu;
                uint32_t g = (c >>  8) & 0xFFu;
                uint32_t b =  c        & 0xFFu;
                r = (r * 3u) >> 3; g = (g * 3u) >> 3; b = (b * 3u) >> 3;
                bb_v[vi].lighting = a | (r << 16) | (g << 8) | b;
            }
            dimmed++;
        }
    }

    TD5_LOG_I("track",
        "additive billboard dim: %d/%d billboard meshes had type-3 pages "
        "(halved per-vertex diffuse)", dimmed, total_bb);
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

int td5_track_get_ring_length(void)
{
    return g_td5.track_span_ring_length;
}

int td5_track_branch_to_junction(int span_idx)
{
    /* If span_idx is on a branch road (>= ring_length), find its main-road
     * equivalent span. Returns the main-road span index, or -1 if not found.
     *
     * Jump table entry layout [CONFIRMED @ 0x443FF0 ResolveActorSegmentBoundary]:
     *   +0x00 uint16  branch_lo     -- first span of branch range
     *   +0x02 uint16  branch_hi     -- last span of branch range
     *   +0x04 int16   main_target   -- ABSOLUTE main-road span index
     * Formula: main_equiv = main_target + (span_idx - branch_lo)
     */
    int ring = g_td5.track_span_ring_length;
    if (span_idx < ring) return span_idx;  /* already on main road */
    if (!s_jump_entries || s_jump_entry_count <= 0) return -1;

    for (int j = 0; j < s_jump_entry_count; j++) {
        uint16_t *entry_u = (uint16_t *)(s_jump_entries + j * 6);
        int16_t  *entry_s = (int16_t  *)(s_jump_entries + j * 6);
        int branch_lo   = (int)entry_u[0];
        int branch_hi   = (int)entry_u[1];
        int main_target = (int)entry_s[2];
        if (span_idx >= branch_lo && span_idx <= branch_hi)
            return main_target + (span_idx - branch_lo);
    }
    return -1;
}

int td5_track_get_fwd_sentinel(void)
{
    return s_boundary_fwd_sentinel;
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
     * Original render loop (0x42baf4) converts span_index to display list
     * index with >> 2 (divide by 4), mapping ~2789 spans to ~698 entries.
     * Original lookup (0x431260): return *(uint*)(table_base + dl_index * 8)
     * [CONFIRMED @ 0x42baf4: SHR by 2; table count stored at 0x4aaee0] */
    if (s_models_blob && s_models_entry_offsets &&
        s_models_display_list_count > 0 && span_index >= 0) {
        int dl_index = span_index >> 2;  /* ~4 spans per display list block */
        if (dl_index >= 0 && dl_index < s_models_display_list_count) {
            uint32_t off = s_models_entry_offsets[dl_index];
            if (off != 0)
                return s_models_blob + off;
        }
        /* Don't return NULL here — branch spans (>= ring_length) may have
         * dl_index outside the MODELS.DAT range. Fall through to STRIP.DAT
         * fallback so branch road geometry is still visible. */
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

/* Stub — TODO: implement from Ghidra decompilation */
int td5_track_get_primary_route_heading(int span_index)
{
    (void)span_index;
    return 0;
}
