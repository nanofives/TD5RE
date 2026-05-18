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
#include "td5_render.h"
#include "td5_trace.h"
#include "td5_pilot_trace_004440F0.h"
#include "td5_pilot_trace_pool15_spline.h"
#include "../../../re/include/td5_actor_struct.h"
#include "td5re.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>

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
 * 0x4353B0  RecycleTrafficActorFromQueue        -- STUB ONLY (real port in td5_ai.c)
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
 * Per-span-type vertex offset LUT — REMOVED 2026-04-20.
 * The single source of truth is `k_quad_vertex_offsets[12][2]` below
 * (DAT_00474e40/41 verified). The prior s_vtx_offset_left/right pair
 * held wrong values ({0,0,0,3,3,0,3,3,0,0,0,0} on the right) that
 * didn't match the binary and were never read anyway.
 */

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

/* td5_track_resolve_wall_contacts — port-specific lateral wall synthesis.
 *
 * The original TD5_d3d.exe has NO generic mid-strip lateral wall impulse —
 * 0x00406CC0 is a sub-lane-extremity transverse-edge boundary check (rumble
 * strips at strip start/end), and 0x00406F50/0x004070E0 only test the LEFT
 * rail of a single per-level fence strip. Lateral containment in the original
 * is supposed to be topological via 0x004440F0 + state-0x0F off-track damping.
 *
 * The port keeps an explicit lateral-rail check (this function) because the
 * topological path alone produces no visible feedback when the car drifts off
 * the road, and users expect walls to behave like walls. The function tests
 * each wheel probe geometrically against the left and right rails of the
 * actor's chassis span and applies a wall_response impulse when a probe is
 * outside the rail (d < 0 after auto-flip toward the inside reference).
 *
 * History:
 *   - 2026-04-21: added per-probe sub_lane extremity gate intended to mirror
 *     0x00406CC0 dispatch. The gate was wrong because 0x00406CC0 uses
 *     transverse edges (perp = along-road) while this function uses
 *     longitudinal rails (perp = lateral). The gate created the
 *     "invisible right-lane wall trap" — every probe in the rightmost
 *     sub-lane fired the right wall, regardless of whether it had actually
 *     crossed the rail.
 *   - 2026-04-28: gate removed. Wall fires purely on geometric d < 0,
 *     matching the function's actual semantics (lateral rail containment),
 *     not the original's 0x00406CC0 (which it never resembled). The
 *     strip-type filter (1/2/5) still rejects irregular vertex layouts
 *     (transition/reversed/junction/sentinel) where the rail edge produces
 *     inverted normals.
 */
#if 1
void td5_track_resolve_wall_contacts(TD5_Actor *actor)
{
    static uint32_t s_wall_tick = 0;
    int diag_slot0 = 0;

    if (!actor || !s_span_array || !s_vertex_table) return;

    /* Per-probe sub_lane gate — added 2026-04-21 in session
     * fix-1776777642-12165 to match original 0x00406CC0 semantics.
     *
     * Ghidra confirmed 0x00406CC0 only writes a wall impulse when a
     * probe's sub_lane is at the strip extremity:
     *   sub_lane == 0              → l_wall test (strip entry edge)
     *   sub_lane >= lane_count - 1 → r_wall test (strip exit  edge)
     * Middle sub_lanes fall through with no writes. Without this gate
     * the port's transverse-edge geometry falsely fires at mid-strip
     * probe positions — Newcastle (level 29) reproduced this at chassis
     * span ~122, producing a pen=-30 clamp bounce ~1.8s after countdown.
     *
     * See the l_hit_count / r_hit_count guards and the Pass-2 per-probe
     * guards below. */

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

    /* Fork-adjacent guard (Wave 1 Chain A fix, 2026-05-17).
     *
     * The current span's type (1/2/5) is "regular quad" but its rail-vertex
     * pair can STILL diverge into the fork branch when the link_next or
     * link_prev neighbour is a transition/reversed/junction/sentinel type
     * (3, 4, 6, 7, 8, 11). On Sydney sp526 and BlueRidge sp257 the rail
     * vertices diverge enough that the auto-flip's `opp_dot` test reads
     * the OPPOSITE-rail corner on the WRONG side of the wall edge, flipping
     * the perp normal AWAY from the road instead of toward it.
     *
     * `td5_physics_wall_response` then receives a wrong-sign `wall_angle`
     * and faithfully reflects the velocity into the lateral direction —
     * that is exactly the observed "AI shoved sideways by an invisible
     * wall". The same flipped normal also fails to detect penetration
     * during AI recovery (the dot test passes with the wrong sign and
     * `d` stays > -10 dead zone), letting the car clip through walls.
     *
     * Suppressing this port-only synthesizer on fork-adjacent geometry is
     * safe: the original binary has no equivalent mid-strip lateral wall
     * impulse here, and the topological off-track damping (state 0x0F) is
     * what's supposed to contain the car near forks anyway. See
     * `todo_wall_response_clamp_fix_2026-05-16.md` for the full audit.
     *
     * Closes: todo_wall_response_clamp_fix, todo_ai_recovery_wall_collision_missing,
     *         todo_sydney_ai_first_divergence_span526_wall (over-shove half),
     *         partial: todo_ai_early_turn_into_wall (wall no longer over-fires
     *         near forks).
     */
    {
        int prev_idx = (int)sp->link_prev;
        int next_idx = (int)sp->link_next;
        if (prev_idx >= 0 && prev_idx < s_span_count) {
            int t = s_span_array[prev_idx].span_type;
            if (t == 3 || t == 4 || t == 6 || t == 7 || t == 8 || t == 11)
                return;
        }
        if (next_idx >= 0 && next_idx < s_span_count) {
            int t = s_span_array[next_idx].span_type;
            if (t == 3 || t == 4 || t == 6 || t == 7 || t == 8 || t == 11)
                return;
        }
    }

    /* Branch spans (index >= ring_length) previously skipped this function
     * because the stale (no-gate) logic produced simultaneous LEFT+RIGHT
     * false-positives on the narrow 2-lane branches. With the sub_lane
     * extremity gate now in place (below), only probes at sub_lane==0 or
     * sub_lane>=lane_count-1 contribute, and the 2+ probe vote filters
     * single-wheel hits. That eliminates the original false-positive
     * pattern, so we run walls on branches too — the user needs branch
     * containment. */

    int lane_count = span_lane_count(sp);
    if (lane_count < 1) lane_count = 1;

    /* Vertex-row layout — CORRECTED 2026-04-13 from UpdateActorTrackPosition
     * @ 0x004440F0 cross-product walker analysis:
     *
     *   left_vertex_index   → base of the LEFT rail (runs near→far)
     *   right_vertex_index  → base of the RIGHT rail (runs near→far)
     *   +k walks LONGITUDINALLY along each rail (near→far, k = 0..lane_count)
     *
     * Evidence: 0x004440F0 case-1 triggers strip DECREMENT when a probe
     * crosses the (vr[k], vl[k]) edge — that only makes sense if vl[k]
     * and vr[k] are two points on the SAME transverse slice at position k,
     * i.e. each rail has k=0 at near end and k=lane_count at far end.
     *
     * Therefore the vector `(vl[0]+vl[1]) − (vr[0]+vr[1])` computed by
     * compute_heading @ 0x00434350 is 2·(left_near_avg − right_near_avg)
     * = LATERAL R→L (right rail → left rail), NOT longitudinal. That
     * matches the +0x400 compass rotation used in td5_track_compute_heading.
     *
     * Prior interpretation (+k lateral, vl=NEAR row, vr=FAR row) is WRONG.
     * It was empirically self-consistent for the Moscow span-111 dump but
     * contradicts the walker's strip-transition logic.
     *
     * CAVEAT — the wall-line construction below predates this correction:
     *   base=vl[0] → end=vr[0] is the NEAR TRANSVERSE edge (left-near to
     *   right-near), NOT a longitudinal rail line. perp points along-road.
     * That means this function is currently testing span entry/exit
     * transverse lines and calling them "walls" — equivalent to the
     * original's `0x00406CC0` sub-lane-extremity boundary check, not a
     * lateral wall impulse. The multi-probe + lane-count filter papers
     * over the resulting false positives but the geometry is wrong.
     *
     * Geometry corrected to longitudinal rails per original 0x406F50/0x4070E0:
     *   0x406F50 uses *(ushort*)(span+4) = left_vertex_index = li_base as near,
     *   li_base + lane_count_nibble = li_base + lane_count = left-far as far.
     *   0x4070E0 mirrors with right_vertex_index = ri_base → ri_base+lane_count.
     *   Edge runs along each rail near→far; perp is lateral across the road.
     *   [CONFIRMED @ 0x406F96–0x406FA5, 0x407120–0x40712F] */

    /* For standard types (1, 2, 5) vertex offsets are always 0. */
    int li_base = (int)sp->left_vertex_index;
    int ri_base = (int)sp->right_vertex_index;

    struct wall_line {
        TD5_StripVertex *base;    /* near vertex */
        TD5_StripVertex *end_v;   /* far vertex */
        TD5_StripVertex *inside;  /* reference vertex definitely on the inside */
    };

    /* Strip vertex layout (corrected 2026-04-28 fix-1777408127-59867):
     *   left_vertex_index   = NEAR transverse row (vertices walk laterally
     *                         left→right as +i, NOT along rail)
     *   right_vertex_index  = FAR  transverse row
     *   lane_count          = LATERAL lane count (surface_attribute & 0xF)
     *
     * So:
     *   li_base + 0           = NW corner (near, leftmost)
     *   li_base + lane_count  = NE corner (near, rightmost)
     *   ri_base + 0           = SW corner (far,  leftmost)
     *   ri_base + lane_count  = SE corner (far,  rightmost)
     *
     * Prior interpretation walked +i along a "rail" longitudinally; that
     * produced TRANSVERSE edges (NW→NE) and an along-road perpendicular,
     * which manifested as walls perpendicular to the driving direction
     * firing on every spawn. Corrected geometry uses the road's actual
     * left/right rails — edges between NEAR and FAR rows at the leftmost
     * and rightmost lateral positions. */

    /* LEFT rail: edge from NW (li_base+0) to SW (ri_base+0). Runs along
     * driving direction. Inside ref = NE corner (li_base+lane_count) — on
     * the road side of the left rail. */
    struct wall_line l_wall = {
        vertex_at(li_base + 0),
        vertex_at(ri_base + 0),
        vertex_at(li_base + lane_count)
    };
    /* RIGHT rail: edge from NE (li_base+lane_count) to SE (ri_base+lane_count).
     * Inside ref = NW corner (li_base+0) — on the road side of the right rail. */
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

    /* Per-probe pass: collect distances, gated by sub_lane extremity.
     * Pass 2 below requires 2+ probes to vote before firing wall_response.
     * This 601f95f-style aggregate filter prevents single-wheel transitions
     * (a front wheel briefly entering sub_lane=lane_count-1 during normal
     * span advancement) from firing the FAR transverse edge wall and
     * trapping the actor at the boundary. Combined with the per-probe
     * sub_lane extremity gate (0x00406CC0 dispatch mirror) this restores
     * the 2026-04-21 verified Newcastle behavior — no wall_contact on
     * span 122 during normal driving.
     *
     * 83acc2a (2026-04-23) removed dead_zone/clamp/vote claiming they
     * caused "car clips half its hood through wall" delay. That symptom
     * was on LATERAL rail walls (which the port no longer synthesises);
     * for transverse-edge boundary checks the dead_zone+vote are
     * essential to keep boundary rumble from accumulating into a trap. */
    static const int32_t WALL_DEAD_ZONE = -10;
    int32_t probe_l_d[4] = {0}, probe_r_d[4] = {0};
    int probe_l_ok[4] = {0}, probe_r_ok[4] = {0};
    int l_hit_count = 0, r_hit_count = 0;

    for (int pi = 0; pi < 4; pi++) {
        int32_t px = probe_block[pi].x >> 8;
        int32_t pz = probe_block[pi].z >> 8;

        /* Per-probe sub_lane extremity gate — mirrors original 0x00406CC0
         * dispatch [CONFIRMED @ 0x00406CC0].
         *
         * Despite the misleading l_wall/r_wall names, the geometry above is
         * NEAR/FAR TRANSVERSE-edge construction under the rails vertex
         * layout (li_base..li_base+lane_count walks LEFT rail NEAR->FAR;
         * same for right):
         *   l_wall edge = li[0]          -> ri[0]          = NEAR transverse
         *   r_wall edge = li[lane_count] -> ri[lane_count] = FAR  transverse
         *
         * 0x00406CC0 dispatches exactly this gate per probe:
         *   sub_lane <  1            -> test NEAR edge (writes l_wall)
         *   sub_lane >= lane_count-1 -> test FAR  edge (writes r_wall)
         *   else                     -> no edge check  (interior segments)
         *
         * sub_lane here is a LONGITUDINAL sub-position WITHIN the span (not
         * a lateral lane). */
        int probe_sub_lane = (int)actor->wheel_probes[pi].sub_lane_index;
        int l_extremity = (probe_sub_lane < 1);
        int r_extremity = (probe_sub_lane >= lane_count - 1);

        if (l_par.ok) {
            int32_t rel_x = px - l_par.base_x - sp->origin_x;
            int32_t rel_z = pz - l_par.base_z - sp->origin_z;
            int64_t dot = (int64_t)rel_x * l_par.nnx + (int64_t)rel_z * l_par.nnz;
            int32_t d = (int32_t)((dot + ((dot >> 63) & 0xFFF)) >> 12);
            if (d < -30) d = -30;
            probe_l_d[pi] = d;
            probe_l_ok[pi] = l_extremity;
            if (l_extremity && d < WALL_DEAD_ZONE) l_hit_count++;
        }
        if (r_par.ok) {
            int32_t rel_x = px - r_par.base_x - sp->origin_x;
            int32_t rel_z = pz - r_par.base_z - sp->origin_z;
            int64_t dot = (int64_t)rel_x * r_par.nnx + (int64_t)rel_z * r_par.nnz;
            int32_t d = (int32_t)((dot + ((dot >> 63) & 0xFFF)) >> 12);
            if (d < -30) d = -30;
            probe_r_d[pi] = d;
            probe_r_ok[pi] = r_extremity;
            if (r_extremity && d < WALL_DEAD_ZONE) r_hit_count++;
        }

        if (diag_slot0) {
            TD5_LOG_I(LOG_TAG, "wall_diag slot0 p%d span=%d sub=%d L:d=%d R:d=%d probe=(%d,%d)",
                      pi, span_idx, probe_sub_lane,
                      probe_l_d[pi], probe_r_d[pi], px, pz);
        }
    }

    /* Pass 2: fire wall_response only if 2+ probes hit the same edge AND
     * each contributing probe is past WALL_DEAD_ZONE. This matches the
     * 601f95f verified setup that prevents single-wheel boundary
     * transitions from firing transverse-edge walls. */
    for (int pi = 0; pi < 4; pi++) {
        /* Skip degenerate span where both walls flag the same probe. */
        if (probe_l_ok[pi] && probe_r_ok[pi] &&
            probe_l_d[pi] < 0 && probe_r_d[pi] < 0)
            continue;

        if (probe_l_ok[pi] && probe_l_d[pi] < WALL_DEAD_ZONE && l_hit_count >= 2) {
            double rad = atan2((double)l_par.nnx, (double)(-l_par.nnz));
            int32_t wall_angle = (int32_t)(rad * (4096.0 / (2.0 * 3.14159265358979323846))) & 0xFFF;
            td5_physics_wall_response(actor, wall_angle, probe_l_d[pi], 1,
                                      probe_block[pi].x, probe_block[pi].z);
            td5_physics_rebuild_pose(actor);
            TD5_LOG_I(LOG_TAG, "wall_contact: slot=%d probe=%d LEFT span=%d d=%d angle=%d n=%d",
                      actor->slot_index, pi, span_idx, probe_l_d[pi], wall_angle, l_hit_count);
        }

        if (probe_r_ok[pi] && probe_r_d[pi] < WALL_DEAD_ZONE && r_hit_count >= 2) {
            double rad = atan2((double)r_par.nnx, (double)(-r_par.nnz));
            int32_t wall_angle = (int32_t)(rad * (4096.0 / (2.0 * 3.14159265358979323846))) & 0xFFF;
            td5_physics_wall_response(actor, wall_angle, probe_r_d[pi], 2,
                                      probe_block[pi].x, probe_block[pi].z);
            td5_physics_rebuild_pose(actor);
            TD5_LOG_I(LOG_TAG, "wall_contact: slot=%d probe=%d RIGHT span=%d d=%d angle=%d n=%d",
                      actor->slot_index, pi, span_idx, probe_r_d[pi], wall_angle, r_hit_count);
        }
    }
}
#endif /* legacy wall_contacts disabled */

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
 *
 * L5 promotion sweep audit (2026-05-18):
 *   - UpdateActorTrackSegmentContactsForward @ 0x00406F50: byte-faithful.
 *     [CONFIRMED @ 0x406F87 JG gate, 0x406FB6 per-probe JG, 0x40702A SAR;
 *     0x40705F JNS sign test, 0x407094 PUSH 0 flag, 0x4070A1 CALL force-fn,
 *     0x4070C0 CMP 0x8 probe-loop bound.]
 *   - UpdateActorTrackSegmentContactsReverse @ 0x004070E0: byte-faithful,
 *     symmetric with Forward — outer gate inverted (`track_span_raw <
 *     sentinel-1` instead of `> sentinel+1`), per-probe gate uses equality
 *     instead of `<=`, base/end vertex pair swapped (psVar1 = end, psVar2
 *     = base) so outward normal flips, and the atan2 edge tangent goes
 *     base->end inverted. [CONFIRMED @ 0x407124 JG inverted; 0x40717C
 *     CMP equality; 0x40717E JNZ probe-skip.] The port's `reverse_mode`
 *     branch in `fwd_rev_resolve_contact` swaps both nx/nz signs and the
 *     ref_v base/end vertex, mirroring orig's swap exactly.
 *   - Normalization uses StoreRoundedVector3Ints @ 0x0042CCD0 in orig
 *     (FILD/FSQRT/FDIVR 4096.0f / FMUL / __ftol-truncate). Port uses
 *     `sqrtf` + float division + `(int32_t)` cast (also round-toward-zero
 *     after 32-bit float precision). Differs only at the 80-bit->32-bit
 *     FPU residue boundary (±1 LSB in normalized normal vector), which
 *     does not change the sign of the penetration dot at any value
 *     observable in the field.
 *   - tangent-angle computation: orig calls AngleFromVector12 @ 0x0040A720,
 *     port uses `atan2` + scale to 4096-units with `& 0xFFF`. Same value
 *     range, ±0/±1 LSB difference at quadrant edges.
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
    td5_physics_wall_response(actor, wall_angle, pen, -1,
                              probe_block[pi].x, probe_block[pi].z);
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

/* Byte-faithful port of InitActorTrackSegmentPlacement @ 0x00445F10.
 *
 * Disassembly summary (push EBX/EBP/ESI, ESI = param_1 = &actor+0x80,
 * later EBX = param_2 from [ESP+0x18]):
 *
 *   sVar4 = *param_1;                                              [0F BF 0E]
 *   param_1[2] = sVar4;                                            [66 89 4E 04]
 *   param_1[3] = sVar4;                                            [66 89 4E 06]
 *   pbVar1 = g_trackStripRecords + sVar4 * 0x18;                   [LEA EDX+ECX*8]
 *   iVar7 = (int)(char)param_1[6];                                 [MOVSX EAX, BYTE [ESI+0xC]]
 *   uVar5 = *(ushort *)(pbVar1 + 4);
 *   cVar2 = DAT_00474e40[(uint)*pbVar1 * 2];     // = k_quad_vertex_offsets[type][0]
 *   cVar3 = DAT_00474e41[(uint)*pbVar1 * 2];     // = k_quad_vertex_offsets[type][1]
 *   uVar6 = *(ushort *)(pbVar1 + 6);
 *   if ((int)(pbVar1[3] & 0xf) <= iVar7) {       // sub_lane >= lane_count
 *       iVar7 = (pbVar1[3] & 0xf) - 1;
 *       *(char *)(param_1 + 6) = (char)iVar7;    // writeback clamp
 *   }
 *   iVar9 = (int)cVar3 + (uint)uVar6 + iVar7;    // right_vertex_base
 *   iVar7 = iVar7 + (int)cVar2 + (uint)uVar5;    // left_vertex_base
 *   iVar10 = iVar7 * 6;                          // stride 6 (3 shorts)
 *   iVar8 = iVar9 * 6;
 *   sum_x = vpool[iVar7+1].x + vpool[iVar9+1].x + vpool[iVar7].x + vpool[iVar9].x
 *   x = ((sum_x << 8) + ((sum_x<<8)>>31 & 3)) >> 2 + origin_x * 0x100   [SHL/CDQ/AND/ADD/SAR pattern]
 *   ...same for y/z using +2/+8 and +4/+10 shorts...
 *
 * The "+ (x>>31 & 3)" is the compiler-generated sign bias for SAR-by-2 to
 * implement signed division by 4 that rounds toward zero — equivalent to C
 * `int /4`. We use plain `/4` here for clarity.
 *
 * Side effects on the actor (via param_1 = &actor+0x80):
 *   - +0x84 (span_accum)  <- span_raw
 *   - +0x86 (span_high)   <- span_raw
 *   - +0x8C (sub_lane)    <- clamped to lane_count-1 if >= lane_count
 *
 * Writes 24.8 fixed-point world position to out_pos[0..2]
 * (matches actor +0x1FC/+0x200/+0x204 layout).
 */
/* Forward declaration — table itself is defined later in this file. */
static const int8_t k_quad_vertex_offsets[12][2];

void td5_track_init_actor_segment_placement(int16_t *actor_at_0x80, int32_t *out_pos)
{
    int16_t  span_raw;
    int      span_index;
    const TD5_StripSpan *sp;
    int      type;
    int      lane_nibble;
    int      sub_lane;
    int8_t   left_off, right_off;
    int      left_vert_base, right_vert_base;
    int      left_idx0, left_idx1, right_idx0, right_idx1;
    TD5_StripVertex *vL0, *vL1, *vR0, *vR1;
    int      sum;

    if (!actor_at_0x80 || !out_pos)
        return;

    span_raw = actor_at_0x80[0];

    /* 0x00445F1A-21: param_1[2] = span; param_1[3] = span (init span_accum + span_high). */
    actor_at_0x80[2] = span_raw;
    actor_at_0x80[3] = span_raw;

    span_index = (int)span_raw;

    /* Out-of-range or no track: leave out_pos zero (defensive — original
     * dereferences strip[span] directly with no range check). */
    if (out_pos) { out_pos[0] = 0; out_pos[1] = 0; out_pos[2] = 0; }

    if (!s_span_array || !s_vertex_table ||
        span_index < 0 || span_index >= s_span_count)
        return;

    sp = &s_span_array[span_index];

    /* sub_lane: SIGN-EXTENDED char read from actor +0x8C
     * (= (char *)(actor_at_0x80 + 6) = byte +0xC from actor_at_0x80). */
    sub_lane = (int)*((const int8_t *)(actor_at_0x80) + 12);

    type = (int)sp->span_type;
    lane_nibble = ((const uint8_t *)sp)[3] & 0x0F;  /* (pbVar1[3] & 0xf) */

    /* DAT_00474E40 / DAT_00474E41 per-type pair (spawn vertex offsets). */
    if (type >= 0 && type < 12) {
        left_off  = (int8_t)k_quad_vertex_offsets[type][0];
        right_off = (int8_t)k_quad_vertex_offsets[type][1];
    } else {
        left_off = 0;
        right_off = 0;
    }

    /* 0x00445F61-63: CMP EAX,EBP / JL — if lane_nibble <= sub_lane, clamp. */
    if (lane_nibble <= sub_lane) {
        sub_lane = lane_nibble - 1;
        /* 0x00445F68: MOV byte ptr [ESI+0xc], AL — writeback clamped value. */
        *((int8_t *)(actor_at_0x80) + 12) = (int8_t)sub_lane;
    }

    /* 0x00445F6B-6D:
     *   EDX = right_off + uVar6 + sub_lane    (right_vertex_base)
     *   EAX = sub_lane + left_off + uVar5     (left_vertex_base) */
    left_vert_base  = sub_lane + (int)left_off  + (int)sp->left_vertex_index;
    right_vert_base = sub_lane + (int)right_off + (int)sp->right_vertex_index;

    /* Read 4 vertices: vL[base], vL[base+1], vR[base], vR[base+1]. */
    left_idx0  = left_vert_base;
    left_idx1  = left_vert_base + 1;
    right_idx0 = right_vert_base;
    right_idx1 = right_vert_base + 1;

    /* Defensive bounds — original has none. */
    if (left_idx0 < 0 || left_idx1 < 0 || right_idx0 < 0 || right_idx1 < 0)
        return;

    vL0 = vertex_at(left_idx0);
    vL1 = vertex_at(left_idx1);
    vR0 = vertex_at(right_idx0);
    vR1 = vertex_at(right_idx1);

    /* 0x00445F7C-FB5: out_pos[0] = ((sum_x * 0x100) / 4) + origin_x * 0x100
     * Order matches disasm (vL1, vR1, vL0, vR0). */
    sum = (int)vL1->x + (int)vR1->x + (int)vL0->x + (int)vR0->x;
    out_pos[0] = ((sum << 8) / 4) + sp->origin_x * 0x100;

    /* 0x00445FBD-FEB: out_pos[1] uses +0x02/+0x08 shorts (y component). */
    sum = (int)vL0->y + (int)vL1->y + (int)vR0->y + (int)vR1->y;
    out_pos[1] = ((sum << 8) / 4) + sp->origin_y * 0x100;

    /* 0x00445FF4-025: out_pos[2] uses +0x04/+0x0a shorts (z component). */
    sum = (int)vL1->z + (int)vL0->z + (int)vR1->z + (int)vR0->z;
    out_pos[2] = ((sum << 8) / 4) + sp->origin_z * 0x100;
}

/* Public accessor for the span's lane-count nibble used by InitializeRaceSession
 * callers. Mirrors the raw `(byte_3 & 0x0F)` read in
 * `InitActorTrackSegmentPlacement @ 0x00445F24` for the sub_lane clamp.
 * Returns 0 if the span index is out of range. */
int td5_track_span_lane_count_at(int span_index)
{
    if (!s_span_array || span_index < 0 || span_index >= s_span_count)
        return 0;
    return span_lane_count(&s_span_array[span_index]);
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
 * Per-span-type vertex index offsets — DAT_00473c68 (int32, stride 8).
 * Stride-8 array: each "type row" has 2 columns of int32.
 *
 *   Col 0 (base 0x473c68) read by SampleTrackTargetPoint (0x00434836).
 *   Col 1 (base 0x473c6c) read by ComputeSignedTrackOffset (0x004346A3)
 *                              and ComputeTrackSpanProgress (0x004345E4).
 *
 * Both consumers add the value to (vertex_base + lane_count) to pick the
 * "end" vertex for 2-vertex interpolation.
 *
 * [CONFIRMED @ 0x00473C68 via mcp__ghidra__memory_read pool15 2026-05-14]
 * — Verified independently via 16-byte reads at 0x473c8c..0x473cb0 and
 * 4-byte single reads at each suspect cell. Earlier prior-port comment
 * had it shifted by 1 row AND used col 0 values for the col 1 consumer.
 *
 *   type:  0  1  2  3  4  5  6  7  8  9 10 11
 *   col0:  0  0 -1 -1 -2  0  0  0  0  0  0  0     <- SampleTrackTargetPoint
 *   col1:  0  0  0  0  0 -1 -1 -2  0  0  0  0     <- ComputeSignedTrackOffset
 *                                                    + ComputeTrackSpanProgress
 *
 * Effect of the prior bug:
 *   - SampleTrackTargetPoint picked the wrong right-end vertex on
 *     type-3/4/5/7/8/9 spans (now correctly only types 2/3/4 use col0).
 *   - ComputeSignedTrackOffset / ComputeTrackSpanProgress applied a
 *     non-zero offset on types 3/4/5/7/8/9 where col 1 is 0 — overshoot
 *     of 1 or 2 vertices. Now applies a non-zero offset only on
 *     types 5/6/7.
 *
 * See todo_ai_right_route_branches.md for the prior symptom (Moscow
 * RIGHT.TRK AI chase corrupted target).
 */
static const int8_t k_target_vertex_offsets[12][2] = {
    /* type  0 */ {  0,  0 },
    /* type  1 */ {  0,  0 },
    /* type  2 */ { -1,  0 },
    /* type  3 */ { -1,  0 },
    /* type  4 */ { -2,  0 },
    /* type  5 */ {  0, -1 },
    /* type  6 */ {  0, -1 },
    /* type  7 */ {  0, -2 },
    /* type  8 */ {  0,  0 },
    /* type  9 */ {  0,  0 },
    /* type 10 */ {  0,  0 },
    /* type 11 */ {  0,  0 },
};

/**
 * Per-span-type vertex index offsets — DAT_00474E40 / DAT_00474E41
 * (int8 pairs, stride 2). Read by InitActorTrackSegmentPlacement
 * (0x00445F10) when averaging 4 vertices (near+near+1 + far+far+1) to
 * place the actor in world space at spawn and ground-snap.
 *
 * Col 0 ("near" / left rail offset)  = DAT_00474E40
 * Col 1 ("far"  / right rail offset) = DAT_00474E41
 *
 * [CONFIRMED @ 0x00474E40 via memory_read, xref @ 0x00445F23/0x00445F2E]:
 *   row:   0   1   2   3   4   5   6   7   8   9  10  11
 *   col0:  0   0   0  -1  -1   0   0   0   0   0   0   0
 *   col1:  0   0   0   0   0   0  -1  -1   0   0   0   0
 *
 * This differs from DAT_00473C68 at types 2 and 4 (+0/+1 shift vs +1/+2
 * in the AI-target table). Conflating them caused Moscow spawn at span
 * offsets that land on type-2/4 spans to read vertex indices 1–2 apart
 * from the original, placing the actor hundreds of thousands of world
 * units off the intended edge.
 */
static const int8_t k_quad_vertex_offsets[12][2] = {
    /* type  0 */ {  0,  0 },
    /* type  1 */ {  0,  0 },
    /* type  2 */ {  0,  0 },
    /* type  3 */ { -1,  0 },
    /* type  4 */ { -1,  0 },
    /* type  5 */ {  0,  0 },
    /* type  6 */ {  0, -1 },
    /* type  7 */ {  0, -1 },
    /* type  8 */ {  0,  0 },
    /* type  9 */ {  0,  0 },
    /* type 10 */ {  0,  0 },
    /* type 11 */ {  0,  0 },
};

/**
 * Get the 4 corner vertex indices for a span quad at a given sub-lane.
 * Returns: vl0, vl1 (left pair), vr0, vr1 (right pair).
 * Applies DAT_00474E40/E41 per-span-type offsets — this is the spawn /
 * ground-snap / world-pose semantic, NOT the AI-target-point semantic.
 */
static void get_quad_vertices(const TD5_StripSpan *sp, int sub_lane,
                               int *vl0, int *vl1, int *vr0, int *vr1)
{
    int type = sp->span_type;
    int left_off = 0, right_off = 0;
    if (type >= 0 && type < 12) {
        left_off  = k_quad_vertex_offsets[type][0];
        right_off = k_quad_vertex_offsets[type][1];
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

    /* Span-type histogram for RIGHT.TRK-route debugging — one-shot at load.
     * The k_target_vertex_offsets fix changes behavior only for span_type in
     * {3,4,5,7,8,9}. If Moscow's branch entries sit on other types, the fix
     * won't help the zig-zag symptom and we need to keep investigating. */
    {
        int type_counts[16] = {0};
        for (int i = 0; i < s_span_count; i++) {
            unsigned t = (unsigned)s_span_array[i].span_type;
            if (t < 16) type_counts[t]++;
        }
        TD5_LOG_I(LOG_TAG,
                  "Span-type histogram: t0=%d t1=%d t2=%d t3=%d t4=%d t5=%d "
                  "t6=%d t7=%d t8=%d t9=%d t10=%d t11=%d",
                  type_counts[0], type_counts[1], type_counts[2],
                  type_counts[3], type_counts[4], type_counts[5],
                  type_counts[6], type_counts[7], type_counts[8],
                  type_counts[9], type_counts[10], type_counts[11]);
    }

    return 1;
}

/**
 * BindTrackStripRuntimePointers (0x444070) — byte-faithful port
 *
 * Original listing (29 instructions, 0x00444070-0x004440E0):
 *   MOV EAX,[0x004aed90]                ; blob base ptr
 *   MOV [0x004c3da0],EAX                ; DAT_004c3da0 = blob_ptr
 *   MOV ECX,[EAX]; ADD ECX,EAX          ; ECX = blob + blob[0] = strip records
 *   MOV [0x004c3d9c],ECX                ; g_trackStripRecords
 *   MOV EDX,[EAX+8]; ADD EDX,EAX        ; EDX = blob + blob[2] = vertex pool
 *   MOV [0x004c3d98],EDX                ; g_trackVertexPool
 *   MOV EDX,[EAX+4]; MOV [0x004c3d90],EDX   ; g_trackTotalSpanCount = blob[1]
 *   MOV ESI,[EAX+0x10]; MOV [0x004c3d94],ESI ; g_trackStripAttributeBasePtr (raw blob[4])
 *   MOV EAX,[EAX+0xc]; MOV [0x004c3d8c],EAX  ; _DAT_004c3d8c = blob[3]
 *   DEC EDX                              ; total - 1
 *   MOV [ECX+0xa],DX                     ; span[0].link_prev = total - 1
 *   MOV ECX,[0x004c3d9c]; MOV byte [ECX],0x9 ; span[0].span_type = 9
 *   MOV EAX,[0x004c3d90]; LEA EDX,[EAX+EAX*2]
 *   MOV EAX,[0x004c3d9c]
 *   MOV word [EAX+EDX*8-0x10],0x0        ; *(uint16*)(strip + total*0x18 - 0x10)
 *                                        ; = last-span +0x08 (link_next) cleared
 *   MOV EAX,[0x004c3d90]; MOV EDX,[0x004c3d9c]; LEA ECX,[EAX+EAX*2]
 *   MOV byte [EDX+ECX*8-0x18],0xa        ; *(uint8*)(strip + total*0x18 - 0x18)
 *                                        ; = last-span +0x00 (span_type) = 10
 *
 * Port maps the original's "g_trackTotalSpanCount" (blob[1], main road ring
 * length) to g_td5.track_span_ring_length. s_span_count may be larger when
 * branch spans exist past the main ring, but the sentinel patches are at
 * (ring-1) just like the original. The blob base / span base / vertex base /
 * attribute base globals are wired up in td5_track_load_strip above; this
 * helper performs only the two sentinel patches that the original executes
 * after the global wiring.
 */
void td5_track_bind_runtime_pointers(void)
{
    TD5_StripSpan *first, *last;
    int ring = g_td5.track_span_ring_length;

    if (!s_span_array || ring < 2)
        return;

    first = &s_span_array[0];
    last  = &s_span_array[ring - 1];

    /* Patch first span:
     *   span_type   = 9   (SENTINEL_START)  -- byte at +0x00
     *   link_prev   = ring - 1              -- int16 at +0x0a */
    first->span_type = 9;
    first->link_prev = (int16_t)(ring - 1);

    /* Patch last main road span:
     *   span_type   = 10  (SENTINEL_END)    -- byte at +0x00
     *   link_next   = 0                     -- uint16 at +0x08 (NOT origin_y!)
     *
     * Original disassembly: `MOV word ptr [EAX + EDX*0x8 + -0x10],0x0`
     * with EDX = total*3 and EAX = strip base, so the displacement
     * total*0x18 - 0x10 = (total-1)*0x18 + 0x08, i.e. last-span +0x08
     * which is link_next, not +0x10 (origin_y) as the previous comment
     * claimed. */
    last->span_type = 10;
    last->link_next = 0;

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

    /* Apply per-span-type lane offset (DAT_00474e40 left / DAT_00474e41 right).
     * [CONFIRMED @ 0x00444208-0x00444336, LUT verified at 0x00474e40] */
    int left_off  = 0;
    int right_off = 0;
    if (sp->span_type >= 0 && sp->span_type < 12) {
        left_off  = k_quad_vertex_offsets[sp->span_type][0];
        right_off = k_quad_vertex_offsets[sp->span_type][1];
    }

    /* Get the 4 corner vertices for this sub-lane */
    vl0 = vertex_at(sp->left_vertex_index  + left_off  + sub_lane);
    vl1 = vertex_at(sp->left_vertex_index  + left_off  + sub_lane + 1);
    vr0 = vertex_at(sp->right_vertex_index + right_off + sub_lane);
    vr1 = vertex_at(sp->right_vertex_index + right_off + sub_lane + 1);

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

    /* Test each edge. Cross product sign > 0 means outside.
     * Bit-to-edge mapping verified at disasm level in 0x00444208..0x00444336:
     *   0x01 FORWARD  = R0 -> L0  (near edge, traversed right-to-left)
     *   0x02 RIGHT    = R1 -> R0  (right side, traversed far-to-near)
     *   0x04 BACKWARD = L1 -> R1  (far edge, traversed left-to-right)
     *   0x08 LEFT     = L0 -> L1  (left side, traversed near-to-far)
     * Previously bits 0x01 and 0x04 had their edges swapped (port tested
     * L1->R1 for forward and R0->L0 for backward), producing a false
     * positive on every tick that made the walker drift monotonically
     * forward to span max_sp-1 and clamp. */

    if ((edge_mask & 0x01) && edge_cross(rx0, rz0, lx0, lz0, pos_x, pos_z) > 0)
        result |= 0x01;  /* FORWARD: R0 -> L0 */

    if ((edge_mask & 0x02) && edge_cross(rx1, rz1, rx0, rz0, pos_x, pos_z) > 0)
        result |= 0x02;  /* RIGHT: R1 -> R0 */

    if ((edge_mask & 0x04) && edge_cross(lx1, lz1, rx1, rz1, pos_x, pos_z) > 0)
        result |= 0x04;  /* BACKWARD: L1 -> R1 */

    if ((edge_mask & 0x08) && edge_cross(lx0, lz0, lx1, lz1, pos_x, pos_z) > 0)
        result |= 0x08;  /* LEFT: L0 -> L1 */

    return result;
}

/**
 * Resolve neighbor span index for a given crossing direction.
 * Returns the new span index (and updates sub_lane via pointer).
 */
static int resolve_neighbor(int span_idx, int *sub_lane, uint8_t crossing_bit,
                            int32_t pos_x, int32_t pos_z)
{
    (void)pos_x; (void)pos_z; /* formerly used by compute_effective_sublane workaround */
    TD5_StripSpan *sp = &s_span_array[span_idx];
    int new_span = span_idx;
    int h_offset = span_height_offset(sp);
    int cur_lane_count = span_lane_count(sp);

    /* Junction branch decision [CONFIRMED @ 0x004440F0]:
     * The original uses running sub_lane_index (cVar15) directly for the
     * junction routing decision — no geometric projection.
     *   Type 8 (fwd junction): sub_lane < next_span.lane_count → span+1 (main)
     *                          sub_lane >= next_span.lane_count → forward_link (branch)
     *   Type 11 (bwd junction): sub_lane < prev_span.lane_count → span-1 (main)
     *                           sub_lane >= prev_span.lane_count → backward_link (branch)
     * Branch delta: sub_lane += branch_dest_lanes - cur_lane_count [CONFIRMED]
     * Walker appends ±1 post-step unconditionally [CONFIRMED @ 0x004440F0 cases 0x01/0x04] */

    switch (crossing_bit) {
    case 0x01: /* Forward */
        switch (sp->span_type) {
        case 8: { /* JUNCTION_FWD [CONFIRMED @ 0x004440F0]:
                   * original uses running cVar15 (sub_lane_index) directly:
                   *   cVar15 < next_span.lane_count → main road (span+1), no delta
                   *   cVar15 >= next_span.lane_count → branch (link_next),
                   *     cVar15 += branch_dest_lanes - cur_lanes
                   * threshold = pbVar1[0x1b] & 0xf = span_records[(span_idx+1)*0x18+3] & 0xf
                   *           = span_lane_count(&s_span_array[span_idx+1]) [CONFIRMED]
                   * walker appends -1 post-step unconditionally [CONFIRMED] */
            int next_idx = span_idx + 1;
            int sl = *sub_lane;
            if (next_idx >= 0 && next_idx < s_span_count) {
                int next_lanes = span_lane_count(&s_span_array[next_idx]);
                if (sl < next_lanes) {
                    /* Main road — no sub_lane delta; walker applies -1 */
                    new_span = next_idx;
                    TD5_LOG_I(LOG_TAG, "jfwd_main: span=%d sl=%d next_lanes=%d -> span=%d",
                              span_idx, sl, next_lanes, new_span);
                } else {
                    /* Branch road — apply dest-lane delta */
                    new_span = (int)sp->link_next;
                    if (new_span >= 0 && new_span < s_span_count) {
                        int branch_lanes = span_lane_count(&s_span_array[new_span]);
                        sl += branch_lanes - cur_lane_count;
                        *sub_lane = sl;
                    } else {
                        new_span = next_idx; /* fallback to main */
                    }
                    TD5_LOG_I(LOG_TAG, "jfwd_branch: span=%d sl=%d next_lanes=%d -> span=%d sl_new=%d",
                              span_idx, *sub_lane, next_lanes, new_span, sl);
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
        case 8: { /* JUNCTION_FWD: same branch decision as 0x01 [CONFIRMED @ 0x004440F0] */
            int next_idx = span_idx + 1;
            int sl = *sub_lane;
            if (next_idx >= 0 && next_idx < s_span_count) {
                int next_lanes = span_lane_count(&s_span_array[next_idx]);
                if (sl < next_lanes) {
                    new_span = next_idx;
                } else {
                    new_span = (int)sp->link_next;
                    if (new_span >= 0 && new_span < s_span_count) {
                        int branch_lanes = span_lane_count(&s_span_array[new_span]);
                        sl += branch_lanes - cur_lane_count;
                        *sub_lane = sl;
                    } else {
                        new_span = next_idx;
                    }
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
        case 11: { /* JUNCTION_BWD [CONFIRMED @ 0x004440F0]:
                    * threshold = pbVar1[-0x15] & 0xf
                    *           = span_records[(span_idx-1)*0x18+3] & 0xf
                    *           = span_lane_count(&s_span_array[span_idx-1]) [CONFIRMED]
                    * cVar15 >= prev_lanes → branch (link_prev), delta applied
                    * cVar15 <  prev_lanes → main road (span-1), no delta
                    * walker appends +1 post-step [CONFIRMED] */
            int prev_idx = span_idx - 1;
            int sl = *sub_lane;
            if (prev_idx >= 0 && prev_idx < s_span_count) {
                int prev_lanes = span_lane_count(&s_span_array[prev_idx]);
                if (sl >= prev_lanes) {
                    /* Branch road */
                    new_span = (int)sp->link_prev;
                    if (new_span >= 0 && new_span < s_span_count) {
                        int branch_lanes = span_lane_count(&s_span_array[new_span]);
                        sl += branch_lanes - cur_lane_count;
                        *sub_lane = sl;
                    } else {
                        new_span = prev_idx; /* fallback to main */
                    }
                } else {
                    /* Main road — no sub_lane delta; walker applies +1 */
                    new_span = prev_idx;
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
        case 11: { /* JUNCTION_BWD: same branch decision as 0x04 [CONFIRMED @ 0x004440F0] */
            int prev_idx = span_idx - 1;
            int sl = *sub_lane;
            if (prev_idx >= 0 && prev_idx < s_span_count) {
                int prev_lanes = span_lane_count(&s_span_array[prev_idx]);
                if (sl >= prev_lanes) {
                    new_span = (int)sp->link_prev;
                    if (new_span >= 0 && new_span < s_span_count) {
                        int branch_lanes = span_lane_count(&s_span_array[new_span]);
                        sl += branch_lanes - cur_lane_count;
                        *sub_lane = sl;
                    } else {
                        new_span = prev_idx;
                    }
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
 * Compound-case cross-product helper. Computes
 *   cross = (b.x - a.x)*(pos.z - a.z) - (b.z - a.z)*(pos.x - a.x)
 * using span-origin-relative 24.8 fixed-point coordinates. v_a_idx and
 * v_b_idx are indices into the shared vertex pool; span origin supplies
 * the world-space offset. This mirrors the cross used by the original's
 * compound retest blocks (0x00444920 / 0x00444DAF / 0x0044517F /
 * 0x004453E7), which reconstruct it from psVar2/psVar3/psVar5 word reads.
 */
static int64_t compound_cross(const TD5_StripSpan *sp,
                               int v_a_idx, int v_b_idx,
                               int32_t pos_x, int32_t pos_z)
{
    TD5_StripVertex *va;
    TD5_StripVertex *vb;
    int32_t ox, oz, ax, az, bx, bz;
    int64_t dx, dz, ex, ez;

    va = vertex_at(v_a_idx);
    vb = vertex_at(v_b_idx);
    if (!va || !vb) return 0;
    ox = sp->origin_x << 8;
    oz = sp->origin_z << 8;
    ax = ox + ((int32_t)va->x << 8);
    az = oz + ((int32_t)va->z << 8);
    bx = ox + ((int32_t)vb->x << 8);
    bz = oz + ((int32_t)vb->z << 8);
    dx = (int64_t)(bx - ax);
    dz = (int64_t)(bz - az);
    ex = (int64_t)(pos_x - ax);
    ez = (int64_t)(pos_z - az);
    return dx * ez - dz * ex;
}

/**
 * Iterative boundary traversal matching original FUN_004440f0 switch logic.
 * Handles compound crossings (diagonal movement) in a single step.
 * Bitmask: bit0=forward(1), bit1=right(2), bit2=backward(4), bit3=left(8).
 *
 * `single_step`: when nonzero, exit after the FIRST single-bit case (1/2/4/8)
 * fires — matches original 0x004440F0 "one-step-per-call" semantics. Compound
 * cases (3/6/9/0xC) already exit after one step via compound_done. Used by
 * the per-wheel walker (`td5_track_update_probe_position`) to prevent
 * front wheels from over-advancing 2-3 spans per tick on slope onsets,
 * which produced spurious +1792 FP ground-probe deltas at Moscow span 196
 * front-wheel projections (Frida-localized 2026-05-01).
 */
static void update_position_recursive(int16_t *track_state, int32_t pos_x, int32_t pos_z,
                                       int depth, int single_step)
{
    int span_idx = (int)track_state[0];
    int sub_lane = (int)((int8_t *)track_state)[12];
    if (sub_lane < 0) sub_lane = 0;  /* clamp stored -1 (from junction ±1 step) before boundary test */
    int new_span;
    int iter;
    /* Compound-case atomic flag. The original's UpdateActorTrackPosition
     * returns after one step — each case handler is terminal. Single-bit
     * cases (1/2/4/8) in the port iterate for multi-span moves, but the
     * compound cases (3/6/9/0xC) now resolve their own retest internally,
     * so they must exit after one step to match the original and to avoid
     * re-entering the same REJECT branch on an unchanged (span, sub_lane)
     * → TRACK_MAX_RECURSION → brute-force fallback that mis-places the
     * actor far from its true span (breaks wall_contact dispatch). */
    int compound_done = 0;

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
        case 1: { /* Forward */
            int prev_type = s_span_array[span_idx].span_type;
            new_span = resolve_neighbor(span_idx, &sub_lane, 0x01, pos_x, pos_z);
            span_idx = new_span;
            track_state[0] = (int16_t)new_span;
            track_state[2]++;
            if (track_state[2] > track_state[3])
                track_state[3] = track_state[2];
            /* Original appends -1 to sub_lane post-step unconditionally,
             * including for junction (type 8) transitions.
             * [CONFIRMED @ 0x004440F0 case 0x01: `cVar15 + ... + (-1)`]
             * resolve_neighbor now uses the running sub_lane (not geometric
             * projection) so the -1 applies cleanly.  The early return keeps
             * single-step semantics matching the original — negative stored
             * values are clamped to 0 at the next call's walker entry. */
            sub_lane -= 1;
            if (prev_type == 8) {
                ((int8_t *)track_state)[12] = (int8_t)sub_lane;
                return;
            }
            break;
        }

        case 2: /* Right */
            new_span = resolve_neighbor(span_idx, &sub_lane, 0x02, pos_x, pos_z);
            span_idx = new_span;
            track_state[0] = (int16_t)new_span;
            break;

        case 3: { /* Forward + Right — single FWD step + post-step retest
                   * [CONFIRMED @ 0x00444920-0x004449BD, pass-4 Ghidra]:
                   * Primary cross on NEW span's lateral edge at new_sub
                   * (e40[new_sub] → e41[new_sub]). Secondary cross on
                   * e41 column at (new_sub-1, new_sub). Three outcomes:
                   *   cross1 <= 0              → advance,   sub = new_sub
                   *   mask & 0x08 == 0
                   *     or cross2 <= 0         → advance,   sub = new_sub - 1
                   *   else                     → REJECT,    sub = OLD - 1, keep OLD span */
            compound_done = 1;
            int old_span = span_idx;
            int old_sub  = sub_lane;
            int stepped_sub = sub_lane;
            int new_span_fwd = resolve_neighbor(span_idx, &stepped_sub, 0x01, pos_x, pos_z);
            TD5_StripSpan *nsp = &s_span_array[new_span_fwd];
            int nt = (nsp->span_type >= 0 && nsp->span_type < 12) ? nsp->span_type : 0;
            int e40_base = (int)nsp->left_vertex_index  + k_quad_vertex_offsets[nt][0];
            int e41_base = (int)nsp->right_vertex_index + k_quad_vertex_offsets[nt][1];
            int64_t cross1 = compound_cross(nsp,
                                            e40_base + stepped_sub,
                                            e41_base + stepped_sub,
                                            pos_x, pos_z);
            if (cross1 <= 0) {
                span_idx = new_span_fwd;
                sub_lane = stepped_sub;
                track_state[0] = (int16_t)new_span_fwd;
                track_state[2]++;
                if (track_state[2] > track_state[3]) track_state[3] = track_state[2];
                break;
            }
            uint8_t mask = (stepped_sub > 1) ? 0x0F : s_edge_mask_first[nt];
            int64_t cross2 = 0;
            if (mask & 0x08) {
                cross2 = compound_cross(nsp,
                                        e41_base + stepped_sub - 1,
                                        e41_base + stepped_sub,
                                        pos_x, pos_z);
            }
            if ((mask & 0x08) == 0 || cross2 <= 0) {
                span_idx = new_span_fwd;
                sub_lane = stepped_sub - 1;
                track_state[0] = (int16_t)new_span_fwd;
                track_state[2]++;
                if (track_state[2] > track_state[3]) track_state[3] = track_state[2];
                break;
            }
            /* REJECT */
            span_idx = old_span;
            sub_lane = old_sub - 1;
            track_state[0] = (int16_t)old_span;
            break;
        }

        case 4: { /* Backward */
            int prev_type = s_span_array[span_idx].span_type;
            new_span = resolve_neighbor(span_idx, &sub_lane, 0x04, pos_x, pos_z);
            span_idx = new_span;
            track_state[0] = (int16_t)new_span;
            track_state[2]--;
            /* Original appends +1 to sub_lane post-step unconditionally,
             * including for junction (type 11) transitions.
             * [CONFIRMED @ 0x004440F0 case 0x04: `cVar15 + ... + '\x01'`] */
            sub_lane += 1;
            if (prev_type == 11) {
                ((int8_t *)track_state)[12] = (int8_t)sub_lane;
                return;
            }
            break;
        }

        case 6: { /* Right + Backward compound bits — ORIGINAL DOES FWD PRIMARY
                   * [CONFIRMED @ 0x00444CB5 INC probe[2], pass-4 Ghidra].
                   * Primary cross on NEW span's lateral edge at (new_sub+1)
                   * (e40[new_sub+1] → e41[new_sub+1]). Secondary cross on
                   * e40 column at (new_sub+1, new_sub+2). Outcomes:
                   *   cross1 <= 0 or mask1 & 0x04 == 0  → advance, sub = new_sub
                   *   mask2 & 0x08 == 0 or cross2 <= 0  → advance, sub = new_sub + 1
                   *   else                              → REJECT,  sub = OLD + 1 */
            compound_done = 1;
            int old_span = span_idx;
            int old_sub  = sub_lane;
            int stepped_sub = sub_lane;
            int new_span_fwd = resolve_neighbor(span_idx, &stepped_sub, 0x01, pos_x, pos_z);
            TD5_StripSpan *nsp = &s_span_array[new_span_fwd];
            int nt = (nsp->span_type >= 0 && nsp->span_type < 12) ? nsp->span_type : 0;
            int lc_new = span_lane_count(nsp);
            int e40_base = (int)nsp->left_vertex_index  + k_quad_vertex_offsets[nt][0];
            int e41_base = (int)nsp->right_vertex_index + k_quad_vertex_offsets[nt][1];
            uint8_t mask1 = (stepped_sub < lc_new - 1) ? 0x0F : s_edge_mask_last[nt];
            if ((mask1 & 0x04) == 0) {
                span_idx = new_span_fwd;
                sub_lane = stepped_sub;
                track_state[0] = (int16_t)new_span_fwd;
                track_state[2]++;
                if (track_state[2] > track_state[3]) track_state[3] = track_state[2];
                break;
            }
            int64_t cross1 = compound_cross(nsp,
                                            e40_base + stepped_sub + 1,
                                            e41_base + stepped_sub + 1,
                                            pos_x, pos_z);
            if (cross1 <= 0) {
                span_idx = new_span_fwd;
                sub_lane = stepped_sub;
                track_state[0] = (int16_t)new_span_fwd;
                track_state[2]++;
                if (track_state[2] > track_state[3]) track_state[3] = track_state[2];
                break;
            }
            uint8_t mask2 = ((stepped_sub + 1) < lc_new - 1) ? 0x0F : s_edge_mask_last[nt];
            int64_t cross2 = 0;
            if (mask2 & 0x08) {
                cross2 = compound_cross(nsp,
                                        e40_base + stepped_sub + 1,
                                        e40_base + stepped_sub + 2,
                                        pos_x, pos_z);
            }
            if ((mask2 & 0x08) == 0 || cross2 <= 0) {
                span_idx = new_span_fwd;
                sub_lane = stepped_sub + 1;
                track_state[0] = (int16_t)new_span_fwd;
                track_state[2]++;
                if (track_state[2] > track_state[3]) track_state[3] = track_state[2];
                break;
            }
            /* REJECT */
            span_idx = old_span;
            sub_lane = old_sub + 1;
            track_state[0] = (int16_t)old_span;
            break;
        }

        case 8: /* Left */
            new_span = resolve_neighbor(span_idx, &sub_lane, 0x08, pos_x, pos_z);
            span_idx = new_span;
            track_state[0] = (int16_t)new_span;
            break;

        case 9: { /* Forward + Left — FWD primary + post-step retest
                   * [CONFIRMED @ 0x00445134-0x004451FB, pass-4 Ghidra]:
                   * Primary cross uses SWAPPED roles vs case 3: psVar2=e40,
                   * psVar3=e41. Secondary cross on e41 column with orientation
                   * (v_a=cur, v_b=prev) → sign opposite case 3's secondary.
                   * Outcomes:
                   *   cross1 <= 0              → advance, sub = new_sub
                   *   mask & 0x02 == 0 or
                   *     cross2 <= 0            → advance, sub = new_sub - 1
                   *   else                     → REJECT,  sub = OLD - 1 */
            compound_done = 1;
            int old_span = span_idx;
            int old_sub  = sub_lane;
            int stepped_sub = sub_lane;
            int new_span_fwd = resolve_neighbor(span_idx, &stepped_sub, 0x01, pos_x, pos_z);
            TD5_StripSpan *nsp = &s_span_array[new_span_fwd];
            int nt = (nsp->span_type >= 0 && nsp->span_type < 12) ? nsp->span_type : 0;
            int e40_base = (int)nsp->left_vertex_index  + k_quad_vertex_offsets[nt][0];
            int e41_base = (int)nsp->right_vertex_index + k_quad_vertex_offsets[nt][1];
            /* case-9 primary cross: v_a=e41 (psVar3), v_b=e40 (psVar2) */
            int64_t cross1 = compound_cross(nsp,
                                            e41_base + stepped_sub,
                                            e40_base + stepped_sub,
                                            pos_x, pos_z);
            if (cross1 <= 0) {
                span_idx = new_span_fwd;
                sub_lane = stepped_sub;
                track_state[0] = (int16_t)new_span_fwd;
                track_state[2]++;
                if (track_state[2] > track_state[3]) track_state[3] = track_state[2];
                break;
            }
            uint8_t mask = (stepped_sub > 1) ? 0x0F : s_edge_mask_first[nt];
            int64_t cross2 = 0;
            if (mask & 0x02) {
                /* case-9 secondary cross: v_a=cur, v_b=prev (opposite case 3) */
                cross2 = compound_cross(nsp,
                                        e41_base + stepped_sub,
                                        e41_base + stepped_sub - 1,
                                        pos_x, pos_z);
            }
            if ((mask & 0x02) == 0 || cross2 <= 0) {
                span_idx = new_span_fwd;
                sub_lane = stepped_sub - 1;
                track_state[0] = (int16_t)new_span_fwd;
                track_state[2]++;
                if (track_state[2] > track_state[3]) track_state[3] = track_state[2];
                break;
            }
            /* REJECT */
            span_idx = old_span;
            sub_lane = old_sub - 1;
            track_state[0] = (int16_t)old_span;
            break;
        }

        case 12: { /* Backward + Left — BACK primary + post-step retest
                    * [CONFIRMED @ 0x004452A5 DEC probe[2], pass-4 Ghidra].
                    * Primary cross same vertices as case 6, but mask2 tests
                    * bit 0x02 (not 0x08), secondary cross is e41 column at
                    * (new_sub+2, new_sub+1) with v_a=nextnext, v_b=cur — sign
                    * opposite case 6's secondary. Outcomes:
                    *   cross1 <= 0 or mask1 & 0x04 == 0  → advance, sub = new_sub
                    *   mask2 & 0x02 == 0 or cross2 <= 0  → advance, sub = new_sub + 1
                    *   else                              → REJECT,  sub = OLD + 1 */
            compound_done = 1;
            int old_span = span_idx;
            int old_sub  = sub_lane;
            int stepped_sub = sub_lane;
            int new_span_back = resolve_neighbor(span_idx, &stepped_sub, 0x04, pos_x, pos_z);
            TD5_StripSpan *nsp = &s_span_array[new_span_back];
            int nt = (nsp->span_type >= 0 && nsp->span_type < 12) ? nsp->span_type : 0;
            int lc_new = span_lane_count(nsp);
            int e40_base = (int)nsp->left_vertex_index  + k_quad_vertex_offsets[nt][0];
            int e41_base = (int)nsp->right_vertex_index + k_quad_vertex_offsets[nt][1];
            uint8_t mask1 = (stepped_sub < lc_new - 1) ? 0x0F : s_edge_mask_last[nt];
            if ((mask1 & 0x04) == 0) {
                span_idx = new_span_back;
                sub_lane = stepped_sub;
                track_state[0] = (int16_t)new_span_back;
                track_state[2]--;
                break;
            }
            int64_t cross1 = compound_cross(nsp,
                                            e40_base + stepped_sub + 1,
                                            e41_base + stepped_sub + 1,
                                            pos_x, pos_z);
            if (cross1 <= 0) {
                span_idx = new_span_back;
                sub_lane = stepped_sub;
                track_state[0] = (int16_t)new_span_back;
                track_state[2]--;
                break;
            }
            uint8_t mask2 = ((stepped_sub + 1) < lc_new - 1) ? 0x0F : s_edge_mask_last[nt];
            int64_t cross2 = 0;
            if (mask2 & 0x02) {
                /* case-C secondary cross: v_a=e41[+2], v_b=e41[+1] (opposite case 6) */
                cross2 = compound_cross(nsp,
                                        e41_base + stepped_sub + 2,
                                        e41_base + stepped_sub + 1,
                                        pos_x, pos_z);
            }
            if ((mask2 & 0x02) == 0 || cross2 <= 0) {
                span_idx = new_span_back;
                sub_lane = stepped_sub + 1;
                track_state[0] = (int16_t)new_span_back;
                track_state[2]--;
                break;
            }
            /* REJECT */
            span_idx = old_span;
            sub_lane = old_sub + 1;
            track_state[0] = (int16_t)old_span;
            break;
        }

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

        /* Compound cases 3/6/9/0xC are atomic — matches original's one-
         * step-per-call semantics (function returns after each case path
         * per pass-4 disasm). Exit the loop now. */
        if (compound_done)
            break;

        /* When called from the per-wheel walker, exit after a single step
         * regardless of which case fired. The per-wheel forward projection
         * can carry a wheel across 2-3 span boundaries in one tick when
         * the chassis is at speed; allowing the loop to chase those
         * boundaries lands the wheel on a span 2-3 ahead of where the
         * original would walk to (originals' 0x004440F0 is one-step-per-
         * call). On slope onsets this produced +1792 FP spurious chassis-Y
         * launches at Moscow span 196 front wheels. The chassis walker
         * passes single_step=0 and continues iterating. */
        if (single_step)
            break;
    }

    if (!compound_done && iter >= TRACK_MAX_RECURSION) {
        /* Non-convergence: boundary walk did not settle within
         * TRACK_MAX_RECURSION steps. Restore the saved state and leave the
         * track position unchanged for this tick — matches the original's
         * single-pass-per-call semantics.
         *
         * The previous port-specific brute-force ±32-span nearest-span
         * search (removed 2026-04-28) was warping the chassis_span by up
         * to 32 spans per tick whenever the actor was at a span boundary
         * with multi-bit crossings the recursion couldn't resolve. That
         * warp:
         *   1. Caused the visible "warp forward at the furthest point on
         *      each lane" symptom (Symptom 2 of fix-1777408127-59867).
         *   2. Bumped track_state[3] (high_water) by the warp delta,
         *      which then propagated into UpdateRaceOrder and prevented
         *      the lap-complete gate `track_start-1 <= span <=
         *      track_start+1` from ever latching, killing finish detection
         *      (Symptom 3, cascade).
         *
         * Original 0x004440F0 [CONFIRMED via Ghidra full-body decomp,
         * fix-1777408127-59867] is single-pass with no fallback — if the
         * boundary cross-products don't resolve cleanly, the function
         * simply returns and the next tick retries with the new world_pos.
         *
         * RE basis: original 0x004440F0 has no ±N-span search loop, no
         * span-center distance metric, and no track_state[2]/track_state[3]
         * write-back outside the per-case bodies. */
        TD5_LOG_W(LOG_TAG,
                  "walker_nonconverge: span=%d sub=%d pos=(%d,%d) — keeping prior state",
                  (int)saved_state[0], (int)((int8_t *)saved_state)[12],
                  pos_x >> 8, pos_z >> 8);
        memcpy(track_state, saved_state, 16);
    }
}

/**
 * UpdateActorTrackPosition (0x4440F0)
 *
 * Updates the actor's track span position using 4-edge cross-product
 * boundary tests. On boundary crossing, transitions to the neighbor span
 * and recursively re-evaluates.
 *
 * [CONFIRMED @ 0x004440F0] L5 promotion sweep audit (2026-05-18).
 *
 * Body is the largest single function in scope at 4908 bytes
 * (0x004440F0..0x0044541C). Pure-compute leaf — no callees. Port logic
 * lives in static `update_position_recursive` @ td5_track.c:2457, called
 * from both `td5_track_update_actor_position` (chassis, single_step=1)
 * and `td5_track_update_probe_position` (per-wheel, single_step=1).
 *
 * SHIPPED FIXES (in master):
 *   - single_step=1 on both chassis + probe variants matches orig's
 *     one-step-per-call semantics [Ghidra-verified 2026-05-12].
 *   - Junction branch decisions cases 1/4/8/11 (commit history).
 *   - SAR usage at 0x00444164/0x0044416A — port uses plain SAR matching
 *     orig (D1 in audit: NO ROUND-TO-ZERO IDIOM — confirmed match).
 *
 * KNOWN DIVERGENCES (re/analysis/pilot_004440F0_audit.md):
 *   D2     Case 1 (FORWARD) — orig performs secondary cross-product on
 *          previous-lane forward edge (psVar2[-3] read). Port just calls
 *          resolve_neighbor(0x01) without the secondary retest. May
 *          matter at lane-boundary onsets where actor barely crosses
 *          FWD edge.
 *   D3     Case 4 (BACKWARD) — symmetric to D2.
 *   D4     `s_loaded_tuning` / `s_jump_entries` fallback (port-only
 *          safety net for out-of-bounds new_span). Orig has no such
 *          net — branches via type-8/9/10/11 dispatch.
 *   D5     TRACK_MAX_RECURSION outer loop + saved-state rollback —
 *          effectively a no-op at runtime (single_step=1 exits after
 *          one iteration). Adds ~32 bytes of dead code on every call.
 *   D6     Camera-probe stub at td5_track.c:4911 doesn't actually walk
 *          (relies on chassis result). Not in pilot critical path.
 *   D7     s_jump_entries heuristic branch-return remap (port-only).
 *          Inert if upstream binds correctly.
 *
 * KNOWN TODO CHAIN OWNERS (cascade dependency):
 *   - todo_jarash_spawn_span_19_vs_92_2026-05-17.md (track-walker)
 *   - chassis-snap chain ownership (downstream consumer)
 *
 * Audit reference: re/analysis/pilot_004440F0_audit.md (pool10, 2026-05-14).
 * Effective level: L4 (byte-faithful overall; D2/D3 secondary-retest
 * gaps tracked as cascade-unwind work).
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

    /* Chassis walker: single-pass per call (single_step=1) — matches original
     * 0x004440F0 which has NO outer loop, every case returns immediately.
     * [Ghidra-verified 2026-05-12]. Earlier port used multi-pass iteration
     * (single_step=0) which over-advanced 3 spans/call at Edinburgh span
     * 259-260, producing bimodal contact_y → +51k vy y-snap launch chain
     * that cascaded into the AI tumbling for the rest of the lap. Single-
     * pass restores faithful behavior; under the original spec the chassis
     * walker incurs at most a 1-tick lag after V2V push / spawn jumps,
     * which the next-tick call resolves. */
    {
        int32_t world_pos_xz[3] = { pos_x, 0, pos_z };
        td5_pilot_emit_004440F0_enter(track_state, world_pos_xz,
                                      (uintptr_t)__builtin_return_address(0));
        update_position_recursive(track_state, pos_x, pos_z, 0, /*single_step=*/1);
        td5_pilot_emit_004440F0_leave(track_state, 0);
    }

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
     * [2]=accumulated, [3]=high_water, ... [6]=sub_lane_index (byte 12)
     *
     * single_step=1: match original 0x004440F0's per-wheel one-step-per-call
     * semantics. Without this, the port's wheel walker can advance 2-3 spans
     * per tick when the wheel's forward projection (~474 world units ahead of
     * chassis) crosses multiple span boundaries, causing the wheel to read
     * ground from a span ahead of where the original would. On slope onsets
     * this produces +1792 FP spurious chassis-Y launches. Frida-localized at
     * Moscow span 196 front wheels (2026-05-01). */
    {
        int32_t world_pos_xz[3] = { world_x, 0, world_z };
        td5_pilot_emit_004440F0_enter(probe, world_pos_xz,
                                      (uintptr_t)__builtin_return_address(0));
        update_position_recursive((int16_t *)probe, world_x, world_z, 0, /*single_step=*/1);
        td5_pilot_emit_004440F0_leave(probe, 0);
    }
}

/* Write per-probe contact_vertex_A/B from probe state. Mirrors the prefix
 * of ComputeActorTrackContactNormal[Extended] at 0x00445450 / 0x004457E0:
 *
 *   iVar9  = probe->span_index * 0x18                       ; strip-record stride
 *   uVar6  = *(byte *)(iVar9 + g_trackStripRecords)         ; span_type
 *   iVar5  = strip[span].left_vertex_index
 *          + (char)param_1[6]                                ; sub_lane_index
 *          + (char)DAT_00474e40[type * 2]                    ; left_off LUT
 *   param_1[4] = (short)iVar5
 *   iVar7  = strip[span].right_vertex_index
 *          + (char)param_1[6]
 *          + (char)DAT_00474e41[type * 2]                    ; right_off LUT
 *   param_1[5] = (short)iVar7
 *
 * The walker (UpdateActorTrackPosition) updates span_index + sub_lane;
 * this helper writes contact_vertex_A/B that the downstream pose code
 * (and whole-state diff) reads at probe + 0x08 / + 0x0A. The port's
 * k_quad_vertex_offsets[][0] = DAT_00474e40, [][1] = DAT_00474e41.
 *
 * Whole-state diff 2026-05-15: port had contact_vertex_A/B = 0 (memset)
 * while original had 770/779 on tick 1. */
void td5_track_compute_probe_contact_vertices(TD5_TrackProbeState *probe)
{
    if (!probe || !s_span_array)
        return;

    int span_idx = (int)probe->span_index;
    if (span_idx < 0 || span_idx >= s_span_count)
        return;

    const TD5_StripSpan *sp = &s_span_array[span_idx];
    int type = sp->span_type;
    int left_off = 0, right_off = 0;
    if (type >= 0 && type < 12) {
        left_off  = (int)k_quad_vertex_offsets[type][0];
        right_off = (int)k_quad_vertex_offsets[type][1];
    }
    int sub_lane = (int)probe->sub_lane_index;
    probe->contact_vertex_A = (int16_t)((int)sp->left_vertex_index  + sub_lane + left_off);
    probe->contact_vertex_B = (int16_t)((int)sp->right_vertex_index + sub_lane + right_off);
}

/* ========================================================================
 * Barycentric Contact Resolution (0x4456D0 / 0x445A70)
 *
 * Given a span, sub-lane, and world XZ position, determines which triangle
 * half the point falls in (diagonal split of quad) and computes the ground
 * height using barycentric interpolation via plane equation.
 * ======================================================================== */

/**
 * Diagonal cross-product test. All three branches of
 * ComputeActorTrackContactNormalExtended @ 0x004457E0 (left-edge,
 * right-edge, and interior) share the SAME cross formula — verified at
 * the disasm level: the left-edge path at 0x004458b3..0x004458f4 falls
 * through into the shared tail at 0x00445970 which computes the cross
 * identically to the interior path at 0x004459d2..0x004459e2.
 *
 *   cross = (v[ri+1].x - v[li].x) * (local_z - v[li].z)
 *         - (v[ri+1].z - v[li].z) * (local_x - v[li].x)
 *
 * Branch rule (also shared, verified at both 0x00445983 and 0x004459e6
 * via JLE): `cross > 0` → triangle `(li, ri+1, li+1)`;
 * `cross <= 0` → triangle `(li, ri, ri+1)`.
 * [CONFIRMED @ 0x00445970..0x00445975, 0x004459d2..0x004459e6]
 */
static int64_t diagonal_cross(int vl0_idx, int vr1_idx,
                              int32_t local_x, int32_t local_z)
{
    TD5_StripVertex *vli  = vertex_at(vl0_idx);
    TD5_StripVertex *vri1 = vertex_at(vr1_idx);
    int32_t dx = (int32_t)vri1->x - (int32_t)vli->x;
    int32_t dz = (int32_t)vri1->z - (int32_t)vli->z;
    return (int64_t)dx * (int64_t)(local_z - (int32_t)vli->z)
         - (int64_t)dz * (int64_t)(local_x - (int32_t)vli->x);
}

/**
 * Compute barycentric height from a triangle defined by 3 vertices.
 * Port of ComputeTrackTriangleBarycentricsWithNormal @ 0x00445A70.
 *
 * Faithful sequence (verified 2026-05-01 against live Ghidra decomp):
 *   1. e1 = vb - va, e2 = vc - va — int32 (sign-extended int16 vertices)
 *   2. cross = (e1y*e2z - e1z*e2y, e1z*e2x - e1x*e2z, e1x*e2y - e1y*e2x)
 *      — int32 multiply (matches CrossProduct3i @ 0x0042EA70)
 *   3. n[i] = cross[i] >> 12 — int32, sign-preserving
 *      [CONFIRMED @ 0x00445B16-0x00445B2C: SAR ECX/EDI/EDX, 0xc]
 *   4. ConvertFloatVec3ToShortAnglesB (0x0042CD40) — renormalize via FPU:
 *        len  = sqrt(nx² + ny² + nz²)
 *        un[i] = (int16) trunc(n[i] * 4096.0 / len)
 *      [CONFIRMED @ 0x0042CD40-0x0042CDA8: FILD/FDIVR/FMUL/__ftol-truncate]
 *      [CONFIRMED @ 0x00467374 = 0x45800000 = 4096.0f, dumped 2026-05-01]
 *   5. if (un[1] == 0) un[1] = 1 — only the exact-zero case
 *      [CONFIRMED @ 0x00445B3F-0x00445B4C, on the int16 unit-Y after step 4]
 *   6. height = ((va.z - local_z) * un[2] + (va.x - local_x) * un[0]) / un[1]
 *             + va.y, then << 8
 *      [CONFIRMED @ 0x00445B52-0x00445B89: int32 IMULs, signed CDQ;IDIV]
 *
 * The previous port used `cross >> 4` raw + `±256` saturation as the divisor,
 * which produced a different quantization of the plane slope at slope-onset
 * spans. Frida-localized 2026-05-01 at Moscow span 196 front-wheel +1792 FP
 * spurious chassis-Y rise (see tools/frida_csv/fix-1777656380-129050).
 *
 * Note: K=4096 is the conventional TD5 12-bit-fixed-point grain (matches
 * angle full-circle = 0x1000). Dumped 2026-05-01: bytes at 0x00467374 are
 * 00 00 80 45 = 0x45800000 = IEEE-754 4096.0f. [CONFIRMED]
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
    int16_t unx, uny, unz;
    int32_t dx, dz;
    int32_t height;
    double len;

    va = vertex_at(va_idx);
    vb = vertex_at(vb_idx);
    vc = vertex_at(vc_idx);

    e1x = (int32_t)vb->x - (int32_t)va->x;
    e1y = (int32_t)vb->y - (int32_t)va->y;
    e1z = (int32_t)vb->z - (int32_t)va->z;

    e2x = (int32_t)vc->x - (int32_t)va->x;
    e2y = (int32_t)vc->y - (int32_t)va->y;
    e2z = (int32_t)vc->z - (int32_t)va->z;

    /* int32 cross then >>12 — matches 0x00445B16-0x00445B2C SAR sequence. */
    nx = (e1y * e2z - e1z * e2y) >> 12;
    ny = (e1z * e2x - e1x * e2z) >> 12;
    nz = (e1x * e2y - e1y * e2x) >> 12;

    /* FPU unit-renormalize to length 4096, truncate-toward-zero to int16. */
    len = sqrt((double)nx * (double)nx
             + (double)ny * (double)ny
             + (double)nz * (double)nz);
    if (len > 0.0) {
        unx = (int16_t)(int32_t)((double)nx * 4096.0 / len);
        uny = (int16_t)(int32_t)((double)ny * 4096.0 / len);
        unz = (int16_t)(int32_t)((double)nz * 4096.0 / len);
    } else {
        unx = 0; uny = 0; unz = 0;
    }

    /* Post-conversion exact-zero guard on the int16 unit-Y. */
    if (uny == 0) uny = 1;

    /* dx/dz are local coords relative to (origin + va). Algebra:
     *   port:  va.y - (dx*unx + dz*unz)/uny
     *   orig:  va.y + ((va.x-local_x)*unx + (va.z-local_z)*unz)/uny
     * with local_x = (pos_x>>8) - origin_x → identical via sign distribution. */
    dx = (pos_x >> 8) - origin_x - (int32_t)va->x;
    dz = (pos_z >> 8) - origin_z - (int32_t)va->z;

    height = (int32_t)va->y
           - ((dx * (int32_t)unx + dz * (int32_t)unz) / (int32_t)uny);

    /* Same int16 unit normal as the divisor — original re-reads from param_5. */
    if (out_normal) {
        out_normal[0] = unx;
        out_normal[1] = uny;
        out_normal[2] = unz;
    }

    TD5_TRACE_CALL_RET("triangle_bary", height,
                       (int32_t)(int16_t)va_idx,
                       (int32_t)(int16_t)vb_idx,
                       (int32_t)(int16_t)vc_idx);

    return (origin_y + height) << 8;
}

/**
 * InterpolateTrackSegmentNormal — byte-faithful port of 0x00445E30.
 *
 * The inner helper called by ComputeActorHeadingFromTrackSegment @ 0x00445B90
 * after that dispatcher picks 3 vertex indices (va, vb, vc) for the actor's
 * span/sub_lane/type triple. Computes the unit normal of the triangle and
 * stores it as int16[3] at the actor's heading_normal slot (caller-side
 * pointer to actor+0x290), with a post-conversion `if (uny == 0) uny = 1`
 * sentinel that is the entire reason ApplyMissingWheelVelocityCorrection ever
 * fires in the original (the port previously wrote uny = 0 unconditionally,
 * neutering the correction).
 *
 * Original disassembly (0x00445E30..0x00445F09, 67 instructions):
 *
 *   00445e30  SUB    ESP, 0x24
 *   00445e33  MOVSX  EAX, word ptr [ESP + 0x28]      ; param_2 = vb_idx
 *   00445e38  MOV    EDX, [0x004c3d98]               ; g_trackVertexPool
 *   00445e3e  MOVSX  ECX, word ptr [ESP + 0x2c]      ; param_3 = vc_idx
 *   00445e43  PUSH   ESI
 *   00445e44  LEA    ECX, [ECX + ECX*2]              ; ECX = vc_idx * 3
 *   00445e47  LEA    ECX, [EDX + ECX*2]              ; ECX = pool + vc_idx*6
 *   00445e4a  PUSH   EDI
 *   00445e4b  MOVSX  EDI, word ptr [ECX]             ; EDI = vc.x  (note: this is
 *                                                    ;  later "vb" in source listing,
 *                                                    ;  Ghidra renames psVar1 differently)
 *   ...
 *
 * Decompiler view (variables renamed for clarity):
 *
 *   psVar1 = pool + vb_idx * 6         (= short* to vb's int16-triple)
 *   psVar2 = pool + va_idx * 6         (= short* to va)
 *   local_c = va.x - vb.x              (= -e1.x where e1 = vb - va)
 *   local_8 = va.y - vb.y              (= -e1.y)
 *   local_4 = va.z - vb.z              (= -e1.z)
 *   iVar3   = pool + vc_idx * 6        (= short* to vc)
 *   local_18 = va.x - vc.x             (= -e2.x)
 *   local_14 = va.y - vc.y             (= -e2.y)
 *   local_10 = va.z - vc.z             (= -e2.z)
 *   CrossProduct3i(&local_c, &local_18, &local_24)  ; n = (-e1) x (-e2) = e1 x e2
 *   local_1c = local_1c >> 0xc                       ; SAR each component
 *   local_24 = local_24 >> 0xc                       ; (n.x, n.y, n.z) >>= 12
 *   local_20 = local_20 >> 0xc
 *   ConvertFloatVec3ToShortAnglesB(&local_24, param_4)
 *      ; FILD int32 -> float, len_sq = x*x + y*y + z*z,
 *      ; inv_norm = 4096.0f / sqrt(len_sq),
 *      ; for each: FMUL inv_norm, __ftol (truncate-to-int32), store low int16.
 *   if (param_4[1] == 0) param_4[1] = 1
 *   return
 *
 * Key fidelity points:
 *
 *   - CrossProduct3i sign: orig uses (va - vb) and (va - vc) (negated edges).
 *     `(-e1) x (-e2) = e1 x e2`, so the cross-product RESULT is byte-identical
 *     to `(vb - va) x (vc - va)`. The port may use either pair; for minimum
 *     surprise vs the listing this port mirrors the orig's negated-edge form.
 *
 *   - SAR by 12 on signed int32: arithmetic right-shift preserves sign on x86.
 *     C's `>>` on signed int is implementation-defined but mingw/gcc lowers it
 *     to SAR for signed types, matching the listing exactly.
 *
 *   - 4096.0f constant: dumped from 0x00467374 = 0x45800000 (IEEE-754 4096.0f).
 *     The FPU loads single-precision, but x87 widens to extended (80-bit) for
 *     the FMUL/FDIVR; the final __ftol-truncate to int32 lands the same value
 *     as `(int32_t)(double)((double)n * 4096.0 / sqrt(len_sq))` on any modern
 *     x86 with default rounding. The port uses double-precision math, which
 *     preserves at least as many bits as the original x87 extended path
 *     before truncation; truncation-toward-zero is identical to `(int16_t)
 *     (int32_t)` cast in C.
 *
 *   - `__ftol` semantics: x86 MSVC runtime helper. Truncates toward zero
 *     regardless of FPU CW rounding mode (the wrapper sets CW to RC=11 and
 *     restores it). Matches C's `(int32_t)(double_value)` cast on x86.
 *
 *   - Exact-zero guard on out_normal[1] applies ONLY to the int16 unit-y
 *     AFTER the truncation, NOT the float intermediate. This means slopes
 *     where the y component truncates to 0 (e.g. very steep) get +1 fp12
 *     instead of 0 -- crucial for the divide-by-uny in
 *     ApplyMissingWheelVelocityCorrection's heading_normal[1] divisor.
 *     [CONFIRMED @ 0x00445EF7-0x00445F02: CMP word ptr [ESI+0x2],0x0 / JNZ /
 *      MOV word ptr [ESI+0x2],0x1]
 *
 *   - The orig writes ONLY through param_4 (the LEA [esi+0x290] passed by
 *     the outer dispatcher). No actor pointer is dereferenced in this inner.
 *
 * This function is intentionally NOT wired up here -- the outer dispatcher
 * (ComputeActorHeadingFromTrackSegment @ 0x00445B90) lands in a sibling
 * worktree (precise-00445B90) and is the one that picks (va, vb, vc) and
 * calls this helper. Until that wires, the helper is unreferenced. We keep
 * the prototype in td5_track.h so the sibling worktree can pick it up
 * without a re-port. */
void td5_track_interpolate_segment_normal(int16_t va_idx, int16_t vb_idx,
                                           int16_t vc_idx, int16_t *out_normal)
{
    TD5_StripVertex *va, *vb, *vc;
    int32_t e1_neg_x, e1_neg_y, e1_neg_z;   /* local_c/8/4 = va - vb  */
    int32_t e2_neg_x, e2_neg_y, e2_neg_z;   /* local_18/14/10 = va - vc */
    int32_t nx, ny, nz;                      /* local_24/1c/20 */
    double len_sq, inv_norm;
    int16_t unx, uny, unz;

    if (!out_normal || !s_vertex_table)
        return;

    /* g_trackVertexPool + idx*6 -- vertex_at clamps neither (orig doesn't
     * either; vertex indices come from the dispatcher's vertex-base + offset
     * arithmetic and are pre-validated upstream). */
    va = vertex_at((int)va_idx);
    vb = vertex_at((int)vb_idx);
    vc = vertex_at((int)vc_idx);

    /* MOVSX-then-SUB sequences at 0x00445E51..0x00445EA8.
     * Orig computes negated edges; the cross-product of two negated vectors
     * equals the cross-product of the originals, so this is mathematically
     * identical to (vb-va) x (vc-va). We mirror the listing's signs to keep
     * intermediate values comparable when stepping a Frida hook. */
    e1_neg_x = (int32_t)va->x - (int32_t)vb->x;
    e1_neg_y = (int32_t)va->y - (int32_t)vb->y;
    e1_neg_z = (int32_t)va->z - (int32_t)vb->z;

    e2_neg_x = (int32_t)va->x - (int32_t)vc->x;
    e2_neg_y = (int32_t)va->y - (int32_t)vc->y;
    e2_neg_z = (int32_t)va->z - (int32_t)vc->z;

    /* CALL 0x0042EA70 CrossProduct3i(&e1_neg, &e2_neg, &n)
     *   n.x = e1_neg.y * e2_neg.z - e1_neg.z * e2_neg.y
     *   n.y = e1_neg.z * e2_neg.x - e1_neg.x * e2_neg.z
     *   n.z = e1_neg.x * e2_neg.y - e1_neg.y * e2_neg.x
     * [CONFIRMED @ 0x0042EA70-0x0042EAB4 listing.] */
    nx = e1_neg_y * e2_neg_z - e1_neg_z * e2_neg_y;
    ny = e1_neg_z * e2_neg_x - e1_neg_x * e2_neg_z;
    nz = e1_neg_x * e2_neg_y - e1_neg_y * e2_neg_x;

    /* SAR EAX/EDX/ECX, 0xc at 0x00445ED2/0x00445EDD/0x00445EE0.
     * Signed int32 -> int32 arithmetic shift, preserves sign. */
    nx >>= 12;
    ny >>= 12;
    nz >>= 12;

    /* CALL 0x0042CD40 ConvertFloatVec3ToShortAnglesB(&n, out_normal):
     *   len_sq = n.x^2 + n.y^2 + n.z^2  (all int32 -> float promoted)
     *   inv_norm = 4096.0f / sqrt(len_sq)
     *   out[0] = (int16) __ftol(n.x * inv_norm)
     *   out[1] = (int16) __ftol(n.y * inv_norm)
     *   out[2] = (int16) __ftol(n.z * inv_norm)
     * [CONFIRMED @ 0x0042CD40-0x0042CDA8 listing + 0x00467374 = 4096.0f.]
     *
     * If len_sq == 0 (degenerate triangle: collinear or all-zero), the orig
     * computes 1/sqrt(0) which raises FPE_FLTDIV; on the FPU this yields
     * +Inf for the inv_norm and __ftol of (Inf * 0/n) is implementation-
     * dependent. In practice the LEFT/RIGHT triangle dispatch never lets
     * three collinear vertices reach this helper. The port adds a guard
     * just to keep the divide finite -- the post-zero-guard then bumps uny
     * to 1 and the caller continues. */
    len_sq = (double)nx * (double)nx
           + (double)ny * (double)ny
           + (double)nz * (double)nz;
    if (len_sq > 0.0) {
        inv_norm = 4096.0 / sqrt(len_sq);
        unx = (int16_t)(int32_t)((double)nx * inv_norm);
        uny = (int16_t)(int32_t)((double)ny * inv_norm);
        unz = (int16_t)(int32_t)((double)nz * inv_norm);
    } else {
        /* PORT-ONLY GUARD: original 0x0042CD6E FSQRT on zero yields +0 ->
         * 0x0042CD70 FDIVR 4096.0f / 0.0 -> +Inf -> __ftol(+Inf * 0) which is
         * UB on x86. The dispatcher upstream never legitimately produces a
         * collinear triple for a real span/lane, so this is unreachable in
         * the original; we set zeros and let the uny=1 guard below handle
         * the divisor case. */
        unx = 0;
        uny = 0;
        unz = 0;
    }

    /* CMP word ptr [ESI+0x2],0x0 / JNZ +6 / MOV word ptr [ESI+0x2],0x1
     * Exact-zero sentinel on the int16 unit-Y. This is the root-cause fix
     * for ApplyMissingWheelVelocityCorrection: the divisor (heading_normal.y)
     * must be non-zero so the correction can fire. The orig guarantees this
     * with the +1 fp12 bump; the port previously hard-coded uny=0 elsewhere,
     * making every divide degenerate. [CONFIRMED @ 0x00445EF7-0x00445F02] */
    if (uny == 0)
        uny = 1;

    out_normal[0] = unx;
    out_normal[1] = uny;
    out_normal[2] = unz;
}

/**
 * Literal port of ComputeActorTrackContactNormalExtended @ 0x004457E0.
 *
 * Three-way dispatch on sub_lane position within the span's lane range:
 *   sub_lane == 0               -> LEFT EDGE
 *   sub_lane == lane_count - 1  -> RIGHT EDGE
 *   otherwise                   -> INTERIOR
 *
 * Within each branch, per-type rules select which 3 quad vertices form
 * the triangle passed to ComputeTrackTriangleBarycentricsWithNormal.
 * Triangle sets come from verified Ghidra transcription of 0x004457E0.
 *
 * span_type 0 is a no-op at the edges (the original returns without
 * writing *out_y). For the port we return `origin_y << 8` so the caller
 * still gets a sensible-ish value, and we zero out_normal to preserve
 * the "default flat normal" assumption callers rely on.
 * [CONFIRMED @ 0x004457E0]
 */
static int32_t probe_span_lane_height(const TD5_StripSpan *sp, int sub_lane,
                                       int32_t world_x, int32_t world_z,
                                       int16_t *out_normal)
{
    int32_t local_x, local_z;
    int vl0, vl1, vr0, vr1;
    int va, vb, vc;
    int max_lane;
    int type;
    int64_t cross;

    if (!sp)
        return 0;

    type = sp->span_type;
    max_lane = span_lane_count(sp) - 1;
    if (max_lane < 0) max_lane = 0;
    if (sub_lane < 0) sub_lane = 0;
    if (sub_lane > max_lane) sub_lane = max_lane;

    get_quad_vertices(sp, sub_lane, &vl0, &vl1, &vr0, &vr1);

    local_x = (world_x >> 8) - sp->origin_x;
    local_z = (world_z >> 8) - sp->origin_z;

    if (sub_lane == 0) {
        /* LEFT EDGE — table 3, rows for sub_lane==0 */
        switch (type) {
        case 1: case 2: case 5:
        case 8: case 9: case 10: case 11:
            cross = diagonal_cross(vl0, vr1, local_x, local_z);
            if (cross > 0) { va = vl0; vb = vr1; vc = vl1; }
            else           { va = vl0; vb = vr0; vc = vr1; }
            break;

        case 3: case 4:
            /* Unconditional (li+1, ri, ri+1). [CONFIRMED @ 0x004458f7] */
            va = vl1; vb = vr0; vc = vr1;
            break;

        case 6: case 7:
            /* Unconditional (li, ri+1, li+1). [CONFIRMED @ 0x004459e8] */
            va = vl0; vb = vr1; vc = vl1;
            break;

        case 0:
        default:
            /* Original returns without writing — no triangle pick. */
            if (out_normal) { out_normal[0] = 0; out_normal[1] = 4096; out_normal[2] = 0; }
            return (int32_t)sp->origin_y << 8;
        }
    } else if (sub_lane == max_lane) {
        /* RIGHT EDGE — table 3, rows for sub_lane==lane_count-1 */
        switch (type) {
        case 1: case 3: case 6:
        case 8: case 9: case 10: case 11:
            cross = diagonal_cross(vl0, vr1, local_x, local_z);
            if (cross > 0) { va = vl0; vb = vr1; vc = vl1; }
            else           { va = vl0; vb = vr0; vc = vr1; }
            break;

        case 2: case 4:
            /* Unconditional (li, ri, ri+1). [CONFIRMED @ 0x004459f9] */
            va = vl0; vb = vr0; vc = vr1;
            break;

        case 5: case 7:
            /* Unconditional (li, ri, li+1). [CONFIRMED @ 0x0044598c] */
            va = vl0; vb = vr0; vc = vl1;
            break;

        case 0:
        default:
            if (out_normal) { out_normal[0] = 0; out_normal[1] = 4096; out_normal[2] = 0; }
            return (int32_t)sp->origin_y << 8;
        }
    } else {
        /* INTERIOR — single unconditional path, span_type NOT consulted.
         * Applies to ALL types including 0.
         * [CONFIRMED @ 0x0044599b..0x004459e6] */
        cross = diagonal_cross(vl0, vr1, local_x, local_z);
        if (cross > 0) { va = vl0; vb = vr1; vc = vl1; }
        else           { va = vl0; vb = vr0; vc = vr1; }
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

    /* Compute heading based on span type.
     *
     * Original FUN_00434350 has a 7-entry jump table at 0x00434588 covering
     * types 1..7 only; types 0 and >=8 hit the default branch
     * (JA 0x0043448c at 0x004343C7-0x004343CA). [CONFIRMED via Ghidra
     *  disassembly @ 0x004343C7 + 0x00434588 jump table.]
     *
     * Previously this switch lumped types 8..11 with case 1/2/5; that
     * applied the standard 4-vertex formula to span types the original
     * sends to the slot-index default. On Moscow start spans this could
     * shift the spawn yaw by enough to push the AI's first-tick steering
     * correction in the wrong direction. Removed 2026-05-10 in the
     * spawn-pose /fix. */
    switch (sp->span_type) {
    case 1: case 2: case 5:
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

    default: {
        /* Original LAB_0043448c at 0x0043448C-94 reloads BOTH dx (EBX) and
         * dz (ECX) from [ESP+0x14] = param_1 = slot. The decompiler shows
         * `uVar7 = uVar10 = param_1` pre-init persisting into the default
         * branch. Replicate exactly so that span types outside [1,7] yield
         * `AngleFromVector12(slot, slot)` instead of the previous
         * `dx=0, dz=1` port-only fabrication.
         * [CONFIRMED @ 0x0043448C-94 disassembly in pilot pool14 session.] */
        int slot_id = (int)((const uint8_t *)actor)[0x375];
        dx = (int32_t)slot_id;
        dz = (int32_t)slot_id;
        break;
    }
    }

    /* Original: 4-quadrant dispatch → full-circle angle, then at LAB_00434501:
     *   stored = (full_angle + 0x800) * 0x100  [CONFIRMED @ 0x00434501]
     * Port's atan2-based AngleFromVector12 also returns the full-circle angle
     * for any (dx, dz), so the bias must also be +0x800, not +0x400.
     * Frida datum: Munich original disp_yaw=2054 = atan2(6,large)+0x800 ✓ */
    angle = AngleFromVector12(dx, dz) & 0xFFF;

    TD5_LOG_I(LOG_TAG, "compute_heading: span=%d type=%d dx=%d dz=%d "
              "lateral_angle=%d yaw=%d",
              span_idx, sp->span_type, dx, dz, angle,
              (angle + 0x800) & 0xFFF);

    /* Write heading to actor's heading_normal at +0x290 (as int16[3]) */
    heading_normal = (int16_t *)((uint8_t *)actor + 0x290);
    heading_normal[0] = (int16_t)dx;
    heading_normal[1] = 0;
    heading_normal[2] = (int16_t)dz;

    /* Store yaw to euler accumulator at +0x1F4. [CONFIRMED @ 0x00434501] */
    *(int32_t *)((uint8_t *)actor + 0x1F4) = (angle + 0x800) << 8;
}

/* ========================================================================
 * ComputeActorHeadingFromTrackSegment (0x00445B90) — byte-faithful port.
 *
 * Per-tick heading-normal writer called from THREE pose-integrator sites:
 *   - IntegrateVehiclePoseAndContacts (0x00405E80)  @ 0x00405fb6
 *   - UpdateVehiclePoseFromPhysicsState (0x004063A0) @ 0x00406439
 *   - UpdateTrafficVehiclePose (0x00443CF0)          @ 0x00443d3d
 *
 * Signature in original:
 *   void __cdecl(short *track_state,   ; actor + 0x80
 *                int   *world_pos,     ; actor + 0x1FC
 *                short *out_normal);   ; actor + 0x290
 *
 * The function:
 *   1. Reads span index = track_state[0]      (= actor +0x80 word)
 *      Reads sub_lane   = track_state[0xC]    (= actor +0x8C byte; the
 *      LISTING uses MOVSX EBX, byte [ESI+0xc] — i.e. a sign-extended byte
 *      at param_1 + 0x0C, which is the lane index field. The Ghidra
 *      decompile says `(char)param_1[6]` which is the same byte under the
 *      short* type — verified by listing).
 *   2. Fetches the span's vertex offsets via k_quad_vertex_offsets[type]
 *      (= DAT_00474E40[type*2]) and writes the resolved indices back to
 *      param_1[4] = actor +0x88 (left_index) and param_1[5] = actor +0x8A
 *      (right_index). This *side effect* is preserved here because the
 *      same fields are read elsewhere (e.g. by td5_track AI walkers).
 *   3. Computes local_x = (world_x>>8) - origin_x, local_z analogously.
 *   4. Two-level dispatch on (sub_lane vs lane_count-1 vs interior) then
 *      span_type: chooses 3 vertex indices from {li, li+1, ri, ri+1, ri+3}
 *      and forwards them to InterpolateTrackSegmentNormal.
 *
 * Dispatch tables (LUTs at 0x00445DEC, 0x00445DF8, 0x00445E04):
 *   k_heading_left_edge_class[11]  (= byte LUT 0x00445DF8 for sub_lane==0):
 *       type 1..0xB → class 0/1/2 (0=diag-class-A, 1=fixed-B, 2=fixed-C)
 *   left edge final dispatch by class:
 *       0 → diag test, true: (li, ri+1, li+1) else fall-through (li, ri, ri+1)
 *       1 → unconditional (li+1, ri, ri+1)           [case 3,4]
 *       2 → unconditional (li, ri+1, li+1)           [case 6,7]
 *   right edge final dispatch on type:
 *       1,3,6,8,9,0xA,0xB → diag test, same as left edge class 0
 *       2,4   → fall-through (li, ri, ri+1)
 *       5,7   → unconditional (li, ri, li+1)
 *   interior:
 *       always diag test, true: (li, ri+1, li+1) else (li, ri, ri+1)
 *
 * Type 0 hits switchD_00445c37_default and the function returns WITHOUT
 * writing — the original leaves the stale value at +0x290 untouched.
 * Types >= 0xC are not in the table and also hit default.
 *
 * [CONFIRMED via Ghidra pool0 listing 2026-05-14, body 0x00445b90..0x00445de8,
 *  219 instructions, jump tables at 0x00445DEC/0x00445DF8/0x00445E04]
 * ======================================================================== */

/* Class indices for left-edge sub_lane==0 dispatch (byte LUT 0x00445DF8).
 * Indexed by (span_type - 1) where span_type in [1, 0xB]. */
static const int8_t k_heading_left_edge_class[11] = {
    0, /* type 1 → class 0 (diag) */
    0, /* type 2 → class 0 (diag) */
    1, /* type 3 → class 1 (li+1, ri, ri+1) */
    1, /* type 4 → class 1 */
    0, /* type 5 → class 0 */
    2, /* type 6 → class 2 (li, ri+1, li+1) */
    2, /* type 7 → class 2 */
    0, /* type 8 → class 0 */
    0, /* type 9 → class 0 */
    0, /* type 10 → class 0 */
    0, /* type 11 → class 0 */
};

/* Local diagonal cross-product test used by the dispatch. Matches the
 * disasm sequence at 0x00445C51..0x00445C85 (and the identical block at
 * 0x00445CF8..0x00445D2C for the right edge, 0x00445D7C..0x00445DB0 for
 * the interior). The test points are the vertices:
 *   va = pool + ri*6 + 6        ; = ri + 1   (LEA EDX, [ECX+ECX*2+3] then *2)
 *   vb = pool + li*6            ; = li
 *   sign = (va.x - vb.x) * (local_z - vb.z) - (va.z - vb.z) * (local_x - vb.x)
 *   if sign > 0 (JLE → skip): triangle = (li, ri+1, li+1)
 *   else                     : triangle = (li, ri, ri+1)  [LAB_00445DD5 tail]
 */
static int64_t heading_diag_cross(int li_idx, int ri_plus1_idx,
                                  int32_t local_x, int32_t local_z)
{
    TD5_StripVertex *vli = vertex_at(li_idx);
    TD5_StripVertex *vri = vertex_at(ri_plus1_idx);
    int32_t dx = (int32_t)vri->x - (int32_t)vli->x;
    int32_t dz = (int32_t)vri->z - (int32_t)vli->z;
    return (int64_t)dx * (int64_t)(local_z - (int32_t)vli->z)
         - (int64_t)dz * (int64_t)(local_x - (int32_t)vli->x);
}

/* Public per-tick heading-normal writer. Pose integrators (player + AI
 * + traffic) call this immediately after td5_track_update_actor_position
 * to refresh actor->heading_normal at +0x290 from the live span geometry.
 *
 * NOTE: Distinct from td5_track_compute_heading() above, which ports
 * 0x00434350 (spawn-only InitializeActorTrackPose) and hard-codes
 * heading_normal[1]=0 — that path is preserved for spawn-pose callers.
 */
void td5_track_compute_runtime_heading_normal(TD5_Actor *actor)
{
    int16_t *track_state;
    int16_t *out_normal;
    int     span_idx;
    int     sub_lane;
    int8_t  type;                  /* span_type clamped to int8 (low byte) */
    int     left_lut, right_lut;   /* lane-bias entries */
    int     li, ri;                /* resolved left/right base indices */
    int     lane_count_minus_1;
    int32_t local_x, local_z;
    int32_t world_x, world_z;
    TD5_StripSpan *sp;
    int     va, vb, vc;            /* selected triangle indices */
    int     have_triangle;

    if (!actor || !s_span_array || !s_vertex_table)
        return;

    track_state = (int16_t *)((uint8_t *)actor + 0x80);
    out_normal  = (int16_t *)((uint8_t *)actor + 0x290);

    /* [00445b94..00445b9b]
     *   EDX = MOVSX word[ESI]          ; span_idx (signed 16 → 32)
     *   EBX = MOVSX byte[ESI + 0xc]    ; sub_lane (signed 8 → 32)
     */
    span_idx = (int)track_state[0];
    sub_lane = (int)((int8_t *)actor)[0x8C];

    if (span_idx < 0 || span_idx >= s_span_count)
        return;

    sp = &s_span_array[span_idx];

    /* [00445bae] MOV AL, byte[EDX + EDI + 3]   ; AL = packed_metadata
     * [00445bb7] AND EAX, 0xF                  ; low nibble = lane_count
     * [00445bb4] MOV CL, byte[EDX + EDI]       ; CL = span_type
     * [00445bbe] MOVSX EBP, byte[0x474E40 + CL*2]  ; left lane-bias
     */
    type = (int8_t)sp->span_type;
    lane_count_minus_1 = span_lane_count(sp) - 1;
    if (lane_count_minus_1 < 0) lane_count_minus_1 = 0;

    /* k_quad_vertex_offsets matches DAT_00474E40 (12 entries × 2 int8). */
    if ((unsigned)type < 12) {
        left_lut  = (int)k_quad_vertex_offsets[(unsigned)type][0];
        right_lut = (int)k_quad_vertex_offsets[(unsigned)type][1];
    } else {
        left_lut = 0;
        right_lut = 0;
    }

    /* [00445bcd..00445bd5]
     *   EDI = EBX + EBP                 ; sub_lane + left_lut
     *   EAX = (ushort)[EDX + EDI + 4]   ; left_vertex_index
     *   AX  += EDI
     *   [ESI + 8] = AX                  ; actor +0x88 ← resolved li
     * [00445be9..00445bfa]
     *   EBP = EBX + EDI(=right_lut)
     *   DI  = (ushort)[EDX + EBP + 6]   ; right_vertex_index
     *   CX  = DI + EBP
     *   [ESI + 0xA] = CX                ; actor +0x8A ← resolved ri
     */
    li = (int)(uint16_t)sp->left_vertex_index  + sub_lane + left_lut;
    ri = (int)(uint16_t)sp->right_vertex_index + sub_lane + right_lut;
    track_state[4] = (int16_t)li;
    track_state[5] = (int16_t)ri;

    /* [00445bfe..00445c13]
     *   ESI = world_x; EDI = world_z (24.8 fixed-point)
     *   ESI = (world_x >> 8) - origin_x   ; local_x
     *   EDI = (world_z >> 8) - origin_z   ; local_z
     */
    world_x = *(int32_t *)((uint8_t *)actor + 0x1FC);
    world_z = *(int32_t *)((uint8_t *)actor + 0x204);
    local_x = (world_x >> 8) - sp->origin_x;
    local_z = (world_z >> 8) - sp->origin_z;

    have_triangle = 0;
    va = vb = vc = 0;

    /* [00445c17..00445c19] TEST EBX, EBX ; JNZ → not-sub_lane-0 branch */
    if (sub_lane == 0) {
        /* [00445c1f..00445c37] LEFT-EDGE DISPATCH
         *   EBP = type - 1; CMP EBP, 0xa; JA default → return-no-write
         *   DL = byte[EBP + 0x445DF8]
         *   JMP [EDX*4 + 0x445DEC]
         */
        int ti = (int)(uint8_t)type - 1;
        if (ti < 0 || ti > 10) {
            return; /* default: no write, leaves stale heading_normal */
        }
        int klass = (int)k_heading_left_edge_class[ti];
        switch (klass) {
        case 0: {
            /* [00445c3e..00445ca8] class A:
             * diag test (li, ri+1) — JLE → fall-through to LAB_00445dd5
             *   sign > 0  → (li, ri+1, li+1)
             *   sign <= 0 → (li, ri, ri+1)   [LAB_00445dd5 tail]
             */
            int64_t sign = heading_diag_cross(li, ri + 1, local_x, local_z);
            if (sign > 0) {
                va = li; vb = ri + 1; vc = li + 1;
            } else {
                va = li; vb = ri;     vc = ri + 1;
            }
            have_triangle = 1;
            break;
        }
        case 1:
            /* [00445ca9..00445cc2] class B (types 3,4):
             * Unconditional (li+1, ri, ri+1) — pushes (EAX+1, ECX, ECX+1)
             */
            va = li + 1; vb = ri; vc = ri + 1;
            have_triangle = 1;
            break;
        case 2:
            /* [switchD_00445c37_caseD_6 → LAB_00445dd5 via path at 0x00445DD5]
             * Wait — the decompiler ALSO shows this label calling
             * (sVar4, sVar6+1, sVar4+1) = (li, ri+1, li+1). The bytes at
             * 0x00445DEC[idx=2] would be 0x00445DB6, but reading the
             * dispatch-table dump the third slot is 0x00445DB6 / actually
             * 0x00445C3E for case A, 0x00445CA9 for case B, 0x00445DD5 (tail)
             * for the case-6/7 path. From the decompile output, the
             * "switchD_00445c37_caseD_6" label routes types 6/7 to call
             * InterpolateTrackSegmentNormal(li, ri+1, li+1) unconditionally.
             * That matches LEFT-EDGE case 6/7 in probe_span_lane_height.
             * [CONFIRMED via decompile]
             */
            va = li; vb = ri + 1; vc = li + 1;
            have_triangle = 1;
            break;
        default:
            return;
        }
    } else if (sub_lane == lane_count_minus_1) {
        /* [00445cc3..00445cde] RIGHT-EDGE DISPATCH
         *   EDX = type - 1; CMP EDX, 0xa; JA → default (no write)
         *   JMP [EDX*4 + 0x445E04]
         * The right-edge LUT is dense (no byte indirection) — each type
         * has its own pointer. The actual dispatch resolves to one of
         * three labels:
         *   0x00445CE5  diag test (same as left-edge class 0)
         *   0x00445DD0  fall-through to LAB_00445dd5 (li, ri, ri+1)
         *   0x00445D50  unconditional (li, ri, li+1)
         */
        switch ((int)(uint8_t)type) {
        case 1: case 3: case 6:
        case 8: case 9: case 10: case 11: {
            /* [00445ce5..00445d4f] diag-class for right edge.
             * Identical diag test to left-edge class A. */
            int64_t sign = heading_diag_cross(li, ri + 1, local_x, local_z);
            if (sign > 0) {
                va = li; vb = ri + 1; vc = li + 1;
            } else {
                va = li; vb = ri;     vc = ri + 1;
            }
            have_triangle = 1;
            break;
        }
        case 2: case 4:
            /* [switchD_00445cde_caseD_2 → LAB_00445dd5] fall-through:
             * (li, ri, ri+1). Note: original PUSHes (sVar4, sVar6, sVar6+1)
             * via LEA EDX,[ECX+1] / PUSH EDX / PUSH ECX / PUSH EAX. */
            va = li; vb = ri; vc = ri + 1;
            have_triangle = 1;
            break;
        case 5: case 7:
            /* [00445d50..00445d68] unconditional (li, ri, li+1).
             * Pushes (EAX+1, ECX, EAX) → reordered as (EAX, ECX, EAX+1)
             * by the C calling convention (right-to-left push order
             * already matches argument order li, ri, li+1). */
            va = li; vb = ri; vc = li + 1;
            have_triangle = 1;
            break;
        default:
            return; /* no write */
        }
    } else {
        /* [00445d69..00445dcf] INTERIOR:
         * Always runs diag test, no span_type discrimination.
         *   sign > 0  → (li, ri+1, li+1)  [PUSH branch at 0x00445DB6]
         *   sign <= 0 → (li, ri, ri+1)    [LAB_00445DD0 → LAB_00445DD5 tail]
         */
        int64_t sign = heading_diag_cross(li, ri + 1, local_x, local_z);
        if (sign > 0) {
            va = li; vb = ri + 1; vc = li + 1;
        } else {
            va = li; vb = ri;     vc = ri + 1;
        }
        have_triangle = 1;
    }

    if (!have_triangle)
        return;

    /* InterpolateTrackSegmentNormal — byte-faithful extern from precise-00445E30
     * (defined above in this same file). Cross-products + FPU-normalises +
     * applies the uny=1 sentinel so heading_normal.y is never exactly zero. */
    td5_track_interpolate_segment_normal(va, vb, vc, out_normal);
}

/* ========================================================================
 * Wrap Normalization (0x443FB0 NormalizeActorTrackWrapState)
 *
 * Verbatim port of NormalizeActorTrackWrapState @ 0x00443FB0 in TD5_d3d.exe.
 * Listing (pool0 2026-05-14):
 *   00443fb0  MOV    ECX, [ESP + 0x4]                ; actor
 *   00443fb4  MOV    AX,  word ptr [ECX + 0x84]      ; raw accumulated span (int16)
 *   00443fbb  TEST   AX,  AX                         ; sign test on the 16-bit value
 *   00443fbe  JL     0x00443fd2                      ; negative branch
 *   00443fc0  MOVSX  EAX, AX                         ; sign-extend to 32-bit
 *   00443fc3  CDQ
 *   00443fc4  IDIV   dword ptr [0x004c3d90]          ; g_trackTotalSpanCount
 *   00443fca  MOV    word ptr [ECX + 0x82], DX       ; remainder -> normalized
 *   00443fd1  RET                                    ; EAX = quotient (laps)
 *   00443fd2  MOVSX  EAX, AX                         ; negative path
 *   00443fd5  PUSH   ESI
 *   00443fd6  MOV    ESI, dword ptr [0x004c3d90]
 *   00443fdc  CDQ
 *   00443fdd  IDIV   ESI
 *   00443fdf  ADD    EDX, ESI                        ; remainder + ring_length
 *   00443fe1  MOV    word ptr [ECX + 0x82], DX       ; store (NO bounds-clamp)
 *   00443fe8  POP    ESI
 *   00443fe9  RET                                    ; EAX = quotient (laps, negative or 0)
 *
 * Critical fidelity points vs prior port:
 *   - Divisor is g_trackTotalSpanCount (DAT_004c3d90) = hdr[1] = main-road
 *     ring length. The port's s_span_count is the INFLATED physical span
 *     count (includes branch-road spans) and does NOT match the original
 *     divisor. Use s_strip_header[1] / g_td5.track_span_ring_length
 *     instead. This mirrors the sibling fix in ResolveActorSegmentBoundary
 *     (precise-00443FF0).
 *   - Sign test is on the 16-bit AX value; the post-MOVSX 32-bit raw is
 *     equivalent in C since int16_t -> int32_t preserves sign.
 *   - Negative branch ADDs ring_length without any subsequent bounds check;
 *     if the dividend is an exact negative multiple of ring_length, EDX is
 *     0 and the stored value is exactly ring_length (matches original
 *     register-level behavior; the original truncates to 16-bit on store
 *     via MOV word ptr, DX).
 *   - Return value is the IDIV quotient (EAX), i.e. the lap count. For
 *     negative dividends with non-zero remainder, x86 IDIV truncates
 *     toward zero, so a value of -1 yields quotient 0 (not -1); this
 *     matches C's `/` operator on signed ints since C99.
 *   - Only the +0x82 field is touched. The +0x84 raw span is NOT
 *     modified by this function.
 *
 * FIXME(divergence): original has no null/zero guards. If
 * g_trackTotalSpanCount == 0 (no track loaded), the IDIV traps. We keep
 * a soft guard for robustness since the port is reachable from the
 * frontend init path before STRIP.DAT is bound.
 * ======================================================================== */

int td5_track_normalize_actor_wrap(TD5_Actor *actor)
{
    int16_t *track_state;
    int32_t raw_span;       /* MOVSX EAX, AX -> int32_t from int16_t */
    int32_t ring_length;    /* dword ptr [0x004c3d90] = g_trackTotalSpanCount */
    int32_t quotient;
    int32_t remainder;

    if (!actor)
        return 0;

    /* Source the divisor from hdr[1] (main-road span count, == original
     * g_trackTotalSpanCount), NOT the inflated s_span_count. */
    if (s_strip_header) {
        ring_length = (int32_t)s_strip_header[1];
    } else {
        ring_length = (int32_t)g_td5.track_span_ring_length;
    }
    if (ring_length == 0)
        return 0; /* PORT-ONLY GUARD (KEEP): original 0x00443FC4 issues
                   * IDIV dword ptr [0x004c3d90] unconditionally and traps
                   * if g_trackTotalSpanCount==0. Upstream invariant in
                   * original is "STRIP.DAT was parsed before any actor
                   * crosses this code path" (set in LoadStripFile @
                   * 0x004444A0). Port reaches the wrap normalizer from
                   * the FRONTEND init pass (td5_game.c spawn-actor at
                   * menu enter) which runs BEFORE the strip file is
                   * bound, so the guard cannot be dropped without
                   * regressing reach analysis. */

    track_state = (int16_t *)((uint8_t *)actor + 0x80);

    /* MOV AX, word ptr [ECX + 0x84] ; MOVSX EAX, AX */
    raw_span = (int32_t)track_state[2]; /* +0x84: raw accumulated span */

    /* TEST AX,AX ; JL 0x00443fd2 -- sign-extended raw_span < 0 is
     * equivalent to the 16-bit AX sign-bit test. */
    if (raw_span >= 0) {
        /* Positive branch: IDIV by g_trackTotalSpanCount, store EDX -> +0x82,
         * return EAX. */
        quotient  = raw_span / ring_length;
        remainder = raw_span % ring_length;
        track_state[1] = (int16_t)remainder; /* MOV word ptr [ECX + 0x82], DX */
        return (int)quotient;
    } else {
        /* Negative branch: IDIV by ring_length, then ADD EDX, ring_length,
         * store DX. NO subsequent bounds clamp -- if remainder is 0 (raw is
         * an exact negative multiple), DX ends up == ring_length and is
         * stored truncated to 16-bit. */
        quotient  = raw_span / ring_length;
        remainder = raw_span % ring_length;
        remainder += ring_length;
        track_state[1] = (int16_t)remainder; /* MOV word ptr [ECX + 0x82], DX */
        return (int)quotient;
    }
}

/* ========================================================================
 * Segment Boundary Remapping (0x443FF0 ResolveActorSegmentBoundary)
 *
 * Verbatim port of ResolveActorSegmentBoundary @ 0x00443FF0 in TD5_d3d.exe.
 * Caller in original: RecycleTrafficActorFromQueue @ 0x004353B0 (only xref).
 *
 * Listing layout (from 0x00443FF0 disassembly, pool9 2026-05-14):
 *   - DAT_004c3da0   = pointer to STRIP.DAT blob (== s_strip_blob).
 *   - [strip + 0x04] = main-road span count (hdr[1], == ring_length).
 *   - [strip + 0x14] = jump_entry_count.
 *   - [strip + 0x18] = jump_entry[0] -- 3 shorts per entry, stride 6:
 *                       short[0] = segment_low  (inclusive)
 *                       short[1] = segment_high (inclusive)
 *                       short[2] = segment_anchor (remap basis)
 *
 * Behavior:
 *   1. raw = (int)(int16_t)actor[+0x80].
 *   2. If raw <= ring_length: write raw to actor[+0x82] and actor[+0x84],
 *      return. (Sentinel: no remap needed for in-ring spans.)
 *   3. Else, scan the jump-entry table sequentially. The first entry where
 *      raw is signed-in-range [low, high] determines the remap. If no
 *      entry matches, return without writing.
 *   4. On match for entry i:
 *        new = raw + (anchor[i] - low[i])
 *      then store `new` (int16) to actor[+0x82] and actor[+0x84].
 *
 * Critical fidelity points vs prior port (resolve_segment_boundary, unused):
 *   - Source field is the RAW span at +0x80 (track_state[0]), not the
 *     normalized span at +0x82.
 *   - Compare against ring_length is on signed int values (JG semantics).
 *   - Sentinel branch writes raw -> +0x82 AND +0x84 unconditionally for
 *     the in-ring case (no early-return on missing jump table).
 *   - Match arithmetic SUBTRACTS the low boundary: it's
 *     `raw + (anchor - low)`, not `raw + remap_field`.
 *   - When the loop exits without a match, NEITHER +0x82 nor +0x84 is
 *     touched (the function simply returns).
 * ======================================================================== */

void td5_track_resolve_actor_segment_boundary(TD5_Actor *actor)
{
    int16_t *track_state;
    int     raw_span;
    int     ring_length;
    int     i;
    int     entry_count;
    uint8_t *entry_base;

    if (!actor) return;

    track_state = (int16_t *)((uint8_t *)actor + 0x80);

    /* Line: MOVSX EDX, word ptr [EBX + 0x80] -> sign-extended raw span. */
    raw_span = (int)track_state[0];

    /* Original reads [DAT_004c3da0 + 0x04] = hdr[1] = main-road ring length.
     * In the port, hdr[1] is mirrored at g_td5.track_span_ring_length and
     * also lives at s_strip_header[1] if the header pointer is bound. We
     * prefer the runtime ring_length since the original's [+0x04] is
     * exactly that value (s_span_count is the port's inflated physical
     * count and does NOT match the original [+0x04]). */
    if (s_strip_header) {
        ring_length = (int)s_strip_header[1];
    } else {
        ring_length = g_td5.track_span_ring_length;
    }

    /* JG 0x0044401d == "raw > ring_length -> fall through to scan",
     * the fallthrough taken when "raw <= ring_length" writes raw to
     * +0x82/+0x84 and returns. */
    if (raw_span <= ring_length) {
        track_state[1] = (int16_t)raw_span; /* +0x82 */
        track_state[2] = (int16_t)raw_span; /* +0x84 */
        return;
    }

    /* Out-of-ring: scan jump table. ESI = [EDI + 0x14] = jump_entry_count.
     * If count <= 0 the loop is skipped and the function returns without
     * writing (XOR EAX,EAX; JLE 0x00444069). */
    entry_count = s_jump_entry_count;
    entry_base  = s_jump_entries;
    if (entry_count <= 0 || !entry_base)
        return;

    /* ECX = EDI + 0x1a initially. puVar3[-1] = entry.low (signed short),
     * puVar3[ 0] = entry.high (signed short). Compare semantics: JL on
     * "raw < low" or JG on "high < raw" => keep scanning. Both are signed
     * (the LEA / MOVZX / MOVSX dance produces zero-extended shorts that
     * are then signed-compared as 32-bit). */
    for (i = 0; i < entry_count; i++) {
        const int16_t *entry = (const int16_t *)(entry_base + i * 6);
        int low    = (int)(uint16_t)entry[0];
        int high   = (int)(uint16_t)entry[1];
        int anchor = (int)(int16_t)entry[2];

        /* Equivalent to the disassembly's:
         *   JL (raw < low)  -> next
         *   JLE (raw > high) inverse -> if (high < raw) next
         *   else: MATCH (fallthrough to remap block) */
        if (raw_span < low) continue;
        if (high < raw_span) continue;

        /* MATCH at index i. Compute the remap:
         *   AX = [EDI + i*6 + 0x1c]    ; anchor (third short of entry)
         *   AX-= [EDI + i*6 + 0x18]    ; low    (first short of entry)
         *   AX+= raw_span
         * Store AX (16-bit truncation) to +0x82 and +0x84. */
        int new_span = (anchor - low) + raw_span;
        track_state[1] = (int16_t)new_span; /* +0x82 */
        track_state[2] = (int16_t)new_span; /* +0x84 */
        return;
    }
    /* Loop exhausted with no match: original returns without touching
     * +0x82/+0x84 (POP EDI/ESI/EBP/EBX; RET at 0x00444069). */
}

/* Circuit checkpoint and lap tracking removed from this module.
 * Canonical implementation: advance_pending_finish_state() in td5_game.c
 * (ported from CheckRaceCompletionState @ 0x00409E80). */

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
 * ComputeTrackSpanProgress (0x004345B0)
 *
 * Projects the actor's 24.8 world position onto the span's longitudinal
 * axis (right_vertex_index → right_vertex_index + lane_count + type_offset).
 * Returns packed int64: low32 = 8-bit normalised progress (0-255 per span),
 * high32 = remainder. Uses right_vertex_index (span+0x06), NOT left (span+0x04).
 * [CONFIRMED @ 0x004345B0]
 * ======================================================================== */
int64_t td5_track_compute_span_progress(int span_index, const int32_t *actor_pos)
{
    const TD5_StripSpan *sp;
    int start_vi, end_vi, type_offset, lane_count;
    const TD5_StripVertex *v_start, *v_end;
    int start_x, start_z, end_x, end_z, dX, dZ, span_len;
    int ax, az, rel_x, rel_z, dot, scaled;

    if (!s_span_array || !s_vertex_table ||
        span_index < 0 || span_index >= s_span_count)
        return 0;

    sp = &s_span_array[span_index];

    /* [CONFIRMED @ 0x004345E0] base vertex = *(uint16*)(span + 0x06) = right_vertex_index */
    start_vi    = (int)(uint16_t)sp->right_vertex_index;
    /* [CONFIRMED @ 0x004345E4: `[EDX*8 + 0x473c6c]` reads COL 1 of the table,
     * not col 0 that SampleTrackTargetPoint reads.  Col 1 has non-zero
     * values only at types 5/6/7. */
    type_offset = (int)k_target_vertex_offsets[(int)sp->span_type][1];
    lane_count  = span_lane_count(sp);
    end_vi      = start_vi + lane_count + type_offset;

    v_start = vertex_at(start_vi);
    v_end   = vertex_at(end_vi);
    if (!v_start || !v_end) return 0;

    start_x = (int)(int16_t)v_start->x;
    start_z = (int)(int16_t)v_start->z;  /* psVar2[2] = int16 at offset +4 [CONFIRMED] */
    end_x   = (int)(int16_t)v_end->x;
    end_z   = (int)(int16_t)v_end->z;

    dX = end_x - start_x;
    dZ = end_z - start_z;

    /* Span length via sqrt [CONFIRMED @ 0x00434602: FILD/FSQRT/__ftol].
     * Orig uses x87 FPU 80-bit precision then truncates via __ftol. Port
     * originally used sqrtf (32-bit float) which lost precision on edge
     * cases — at Moscow span 111 this produced port_progress=94 vs orig=95
     * (1-unit divergence cascading to bias delta and target_angle delta and
     * RS_LEFT_DEVIATION=35 per-tick cascade re-fire). Switching to sqrt
     * (64-bit double) closes the precision gap. */
    span_len = (int)sqrt((double)(dX * dX + dZ * dZ));
    if (span_len == 0) return 0;

    ax = actor_pos[0] >> 8;  /* strip 24.8 FP to integer world [CONFIRMED @ 0x00434631] */
    az = actor_pos[2] >> 8;

    /* rel pos = actor - span_origin - start_vertex [CONFIRMED @ 0x00434631-0x0043463a] */
    rel_x = ax - (int)sp->origin_x - start_x;
    rel_z = az - (int)sp->origin_z - start_z;

    dot = rel_z * dZ + rel_x * dX;

    /* Two-stage normalisation: (dot/len)<<8 then /len [CONFIRMED @ 0x0043465d] */
    scaled = (dot / span_len) << 8;
    return ((int64_t)(scaled % span_len) << 32) | (uint32_t)(scaled / span_len);
}

/* ========================================================================
 * ComputeSignedTrackOffset (0x00434670)
 *
 * Returns the signed world-unit lateral distance between the actor's 8-bit
 * progress value and the route byte (target lateral position).
 * Negative when progress < route_byte (actor is inward of target).
 * [CONFIRMED @ 0x00434670]
 * ======================================================================== */
int32_t td5_track_compute_signed_offset(int span_index, int progress, int route_byte)
{
    const TD5_StripSpan *sp;
    int start_vi, end_vi, type_offset, lane_count;
    const TD5_StripVertex *v_start, *v_end;
    int start_x, start_z, end_x, end_z;
    int delta, dX, dZ, dX_sc, dZ_sc, len, v;

    if (!s_span_array || !s_vertex_table ||
        span_index < 0 || span_index >= s_span_count)
        return 0;

    sp = &s_span_array[span_index];

    /* Same vertex pair as ComputeTrackSpanProgress [CONFIRMED @ 0x00434680-0x004346b8] */
    start_vi    = (int)(uint16_t)sp->right_vertex_index;
    /* [CONFIRMED @ 0x004346A3: `[EDX*8 + 0x473c6c]` reads COL 1 of the table.
     * Non-zero values only at types 5/6/7. */
    type_offset = (int)k_target_vertex_offsets[(int)sp->span_type][1];
    lane_count  = span_lane_count(sp);
    end_vi      = start_vi + lane_count + type_offset;

    v_start = vertex_at(start_vi);
    v_end   = vertex_at(end_vi);
    if (!v_start || !v_end) return 0;

    start_x = (int)(int16_t)v_start->x;
    start_z = (int)(int16_t)v_start->z;
    end_x   = (int)(int16_t)v_end->x;
    end_z   = (int)(int16_t)v_end->z;

    delta = progress - route_byte;

    /* Signed >>8 with rounding [CONFIRMED @ 0x004346ca-0x004346ed] */
    v = (end_x - start_x) * delta;
    dX_sc = (v + ((v >> 31) & 0xFF)) >> 8;
    v = (end_z - start_z) * delta;
    dZ_sc = (v + ((v >> 31) & 0xFF)) >> 8;

    /* sqrt: use double precision to match orig FPU 80-bit truncation
     * (same precision issue as in compute_span_progress). */
    len = (int)sqrt((double)(dX_sc * dX_sc + dZ_sc * dZ_sc));

    /* Sign from comparison [CONFIRMED @ 0x004346f5] */
    {
        int32_t ret_val = (progress < route_byte) ? -len : len;
        td5_pilot_trace_pool15_emit_signed_offset(span_index, progress, route_byte, ret_val);
        return ret_val;
    }
}

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

    /* No span_type filter — original SampleTrackTargetPoint @ 0x00434800
     * has no such early-out. The previous port version returned 0 for
     * span_type 9/10 (causing the AI caller to SKIP the entire target-angle
     * update and keep last tick's stale value). When the AI's spawn target
     * span lands on a type-9/10 span (Moscow start spans on RIGHT.TRK can
     * be 9/10 per the agent's k_target_vertex_offsets table audit), the
     * port produced delta=-10 vs original delta=-101 at tick 1, a 10×
     * geometric difference that translates into stuck-in-the-floating-
     * wmask=0 state by span ~430. [CONFIRMED via Ghidra audit of
     * 0x00434800-0x004348FB: no span_type filter; reads
     * k_target_vertex_offsets[span_type] for any type 0..11 without
     * branching out.] */

    /* Lane count from geometry metadata (low nibble of byte +0x03).
     * Original reads the raw nibble with no clamp [CONFIRMED @ 0x00434836:
     * `pbVar1[3] & 0xf`] — do not clamp to >=1 here. */
    lane_count = span_lane_count(sp);

    /* Span-type vertex offset [CONFIRMED @ 0x00434836: DAT_00473c68].
     * This is the AI-target-point table, distinct from the spawn/ground-snap
     * k_quad_vertex_offsets (DAT_00474E40). */
    type_offset = 0;
    if (sp->span_type >= 0 && sp->span_type < 12)
        type_offset = k_target_vertex_offsets[sp->span_type][0];

    /* Two-vertex lookup [CONFIRMED @ 0x00434803-0x00434862]
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

    /* Perpendicular lateral offset [CONFIRMED @ 0x434883-0x4348F1].
     *
     * Builds an int16 tangent vector then applies the bias along it. The
     * tangent is computed via ConvertFloatVec4ToShortAngles @ 0x0042CDB0,
     * which normalises to length 4096 using x87 80-bit precision for
     * sqrt + divide. The first output (tan_x) is computed at 80-bit
     * precision; the scale is then truncated to 32-bit float (FST) and
     * reused for the second output (tan_z) at 32-bit precision.
     *
     * Bias is applied along the (left->right) edge tangent itself — the
     * rail-crossing edge IS the lateral axis of the lane. Original step 6
     * builds `local_8 = {(short)(Rx-Lx), 0, (short)(Rz-Lz)}` (the edge
     * vector); step 7 calls ConvertFloatVec4ToShortAngles which scales
     * by 4096/mag (no rotation); step 8 applies
     *   `out[0] += (local_8[0]*bias)>>12 << 8` (along +dx).
     */
    if (lateral_bias != 0) {
        int32_t edge_dx = (int16_t)(right_x - left_x);
        int32_t edge_dz = (int16_t)(right_z - left_z);

        /* Mirror ConvertFloatVec4ToShortAngles' two-precision scale:
         *   scale_hi = 4096.0 / sqrt(double(fex² + fez²))  [80-bit equiv]
         *   scale_lo = (float)scale_hi                     [32-bit FST]
         * tan_x uses scale_hi (first __ftol from 80-bit ST(0));
         * tan_z uses scale_lo (FLD of 32-bit FST value, second __ftol).
         * [CONFIRMED @ 0x0042CE03 FST + 0x0042CE10 FLD reload]. */
        double  fex_d = (double)edge_dx;
        double  fez_d = (double)edge_dz;
        double  mag_sq_d = fex_d * fex_d + fez_d * fez_d;
        if (mag_sq_d > 0.25) {
            double scale_hi = 4096.0 / sqrt(mag_sq_d);
            float  scale_lo = (float)scale_hi;
            int16_t tan_x = (int16_t)(int32_t)(scale_hi * fex_d);
            int16_t tan_z = (int16_t)(int32_t)((float)(scale_lo * (float)edge_dz));

            /* Apply bias with fixed-point 4.12 multiply and rounding
             * [CONFIRMED @ 0x4348B1-0x4348F1] */
            int32_t off_x = (int32_t)tan_x * lateral_bias;
            int32_t off_z = (int32_t)tan_z * lateral_bias;
            /* Signed rounding: (val + (CDQ(val) & 0xFFF)) >> 12 */
            off_x = (off_x + ((off_x >> 31) & 0xFFF)) >> 12;
            off_z = (off_z + ((off_z >> 31) & 0xFFF)) >> 12;
            result_x += off_x * 0x100;
            result_z += off_z * 0x100;
        }
    }

    if (out_x) *out_x = result_x;
    if (out_z) *out_z = result_z;

    td5_pilot_trace_pool15_emit_target_point(span_index, route_byte, lateral_bias,
                                              result_x, result_z);
    return 1;
}

/* ========================================================================
 * Target-span junction remap
 * Port of the walker inside UpdateActorTrackBehavior @
 * 0x00435180-0x00435260. Only runs when actor's route_ptr != LEFT.TRK
 * (LEFT is the canonical route that uses linear span advance).
 * ======================================================================== */
int td5_track_apply_target_span_remap(int lin_span, int is_canonical_route)
{
    /* Wrap lin into [0, span_count). Mirrors original's JL/SUB at 0x004351F2. */
    int target_span;
    int lin;
    int i;

    if (s_span_count <= 0) return lin_span;

    lin = lin_span;
    if (lin >= s_span_count) lin -= s_span_count;
    if (lin < 0) lin += s_span_count;
    target_span = lin;

    /* Canonical (LEFT) route skips the walker — linear advance only. */
    if (is_canonical_route) return target_span;

    if (!s_jump_entries || s_jump_entry_count <= 0) return target_span;

    for (i = 0; i < s_jump_entry_count; ++i) {
        /* [CONFIRMED @ 0x00435218] walker field layout (stride 6, per entry):
         *   u16 remap_dst           at +0
         *   u16 remap_end_exclusive at +2
         *   u16 range_lo            at +4
         * DO NOT confuse with the port's other walkers at td5_track.c:2013 and
         * td5_track.c:2829 which (currently) treat +0 as start-of-range — those
         * are separate code paths for branch-return handling; this walker uses
         * the authoritative Ghidra-confirmed layout for AI target remapping. */
        const uint8_t *entry = s_jump_entries + i * 6;
        uint32_t remap_dst     = *(const uint16_t *)(entry + 0);
        uint32_t remap_end_exc = *(const uint16_t *)(entry + 2);
        uint32_t range_lo      = *(const uint16_t *)(entry + 4);
        int len = (int)remap_end_exc - (int)remap_dst;
        int range_hi;
        int cand;

        if (len <= 0) continue;
        range_hi = (int)range_lo + len - 1;

        if (lin < (int)range_lo || lin > range_hi) continue;

        cand = ((int)remap_dst - (int)range_lo) + lin;
        if (cand != -1) target_span = cand;
        /* Original breaks on first range match regardless of cand sentinel. */
        break;
    }

    return target_span;
}

/* ========================================================================
 * Signed spline distance — forwarder to td5_track_compute_signed_offset
 *
 * Both `td5_track_compute_spline_position` and `td5_track_compute_signed_offset`
 * port the same listing function 0x00434670 ComputeSignedTrackOffset. The
 * older `compute_spline_position` implementation had three divergences:
 *   1. used `span_height_offset(sp)` (high nibble of byte+3) instead of
 *      the type_offset table at 0x473c6c (col 1 of k_target_vertex_offsets);
 *   2. clamped lane_count to >=1 (listing has no clamp);
 *   3. used td5_isqrt (integer sqrt) instead of sqrt((double)) — listing
 *      uses FILD/FSQRT/__ftol on the 32-bit mag_sq, giving x87 80-bit
 *      precision then truncating.
 *
 * The newer `compute_signed_offset` is byte-faithful to the listing. Both
 * call sites in td5_ai.c pass the same listing arguments (param_2 =
 * "segment_distance"/"progress", param_3 = "route_lane"/"route_byte").
 * Forwarder eliminates the duplicate while preserving the public ABI.
 * ======================================================================== */

int32_t td5_track_compute_spline_position(int span_index, int segment_distance,
                                           int route_lane)
{
    return td5_track_compute_signed_offset(span_index, segment_distance, route_lane);
}

/* ========================================================================
 * Track Lighting (0x430150 / 0x42CE90)
 *
 * Per-vehicle zone-driven 3-light + ambient basis. Implementation moved to
 * td5_render_apply_track_lighting() in td5_render.c (2026-04-23): the prior
 * skeleton in this file used wrong struct offsets, was never wired to a
 * populated zone table, and wrote to a parallel set of globals that the
 * mesh-vertex-lighting pass did not read. The new code reuses the per-track
 * zone array generated in td5_light_zones_table.inc and writes directly to
 * s_light_dirs[]/s_ambient_intensity which the renderer already consumes.
 * ======================================================================== */
#if 0 /* legacy skeleton -- intentionally retained as a #if-0 block until the
       * historical RE notes inside it are migrated into the new code. */
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

#endif /* legacy track-lighting skeleton */

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
 * RecycleTrafficActorFromQueue: vestigial track-side stub.
 *
 * NOTE (precise-00435310 audit, 2026-05-14): the scope-file address
 * 0x00435310 does NOT correspond to a function entry in TD5_d3d.exe.
 * Ghidra reports 0x00435310 as an instruction inside
 * UpdateActorTrackBehavior (0x00434FE0-0x004353AC), not a function
 * start. The actual traffic-recycle entry lives at 0x004353B0
 * (RecycleTrafficActorFromQueue), and the byte-faithful port of that
 * function lives in td5_ai.c (td5_ai_recycle_traffic_actor). The
 * call-site dispatcher (UpdateRaceActors @ 0x00436A70) is in td5_ai.c
 * too, so the canonical home for this logic is td5_ai.c.
 *
 * This stub is retained only to keep external references (route-loader
 * scaffolding) building until those callers are migrated; it performs
 * no recycle work itself. Do not call from new code -- use
 * td5_ai_recycle_traffic_actor() in td5_ai.c.
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

/**
 * Return the lane count (low nibble of byte +0x03) for the span at span_index.
 * Returns 1 if the span is invalid or has a zero nibble.
 * Used by UpdateTrafficRoutePlan sub_lane adjustment when entering a branch.
 */
int td5_track_get_span_lane_count(int span_index)
{
    if (!s_span_array || span_index < 0 || span_index >= s_span_count)
        return 1;
    int lc = span_lane_count(&s_span_array[span_index]);
    return lc < 1 ? 1 : lc;
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

/* Mirrors orig UpdateRaceActors @ 0x00436ADB..0x00436C74 junction-check.
 *
 * Orig disasm summary (verified read-only @ Ghidra session 2026-05-17):
 *   EDX = strip_base + 0x1c       ; iterates ushort* over junction entries
 *   ESI = span_norm               ; actor.field_0x82
 *   For each entry (6-byte stride):
 *     CX  = ushort[+0x1c]         ; "main_target" (entry_s[2] in port naming)
 *     If span_norm < CX: skip (JL 0x436b26)
 *     SI  = ushort[+0x18]         ; "branch_lo" (entry_u[0])
 *     BP  = ushort[+0x1a]         ; "branch_hi" (entry_u[1])
 *     EBP -= ESI = (branch_hi - branch_lo)        ; range width
 *     max = CX + (branch_hi - branch_lo) - 1
 *     If span_norm <= max: jump to PATH 2a candidate (LAB_b51)
 *     Else: keep scanning
 *   PATH 2a candidate (LAB_436c51):
 *     BP  = ushort[entry.+0x18]   ; branch_lo  (=SI from earlier)
 *     EBP = ushort[entry.+0x1c]   ; main_target (=CX from earlier)
 *     If (branch_lo - main_target + span_norm) == -1: PATH 2b (no match)
 *     Else: PATH 2a match → write selector=0, ptr=LEFT.TRK
 *
 * Returns 1 for PATH 2a match (write ptr=LEFT), 0 for PATH 2b (keep ptr).
 */
int td5_track_route_junction_path2a_match(int span_norm)
{
    if (!s_jump_entries || s_jump_entry_count <= 0) return 0;
    if (span_norm < 0) return 0;

    for (int j = 0; j < s_jump_entry_count; j++) {
        uint16_t *entry_u = (uint16_t *)(s_jump_entries + j * 6);
        int branch_lo   = (int)entry_u[0];        /* +0x18 */
        int branch_hi   = (int)entry_u[1];        /* +0x1a */
        int main_target = (int)entry_u[2];        /* +0x1c (signed in port,
                                                     orig reads as unsigned
                                                     via MOV CX,word). */
        /* Range check: main_target <= span_norm <= main_target +
         * (branch_hi - branch_lo) - 1. Skip until in range. */
        if (span_norm < main_target) continue;
        int max_bound = main_target + (branch_hi - branch_lo) - 1;
        if (span_norm > max_bound) continue;

        /* PATH 2a candidate: confirm with the -1 sentinel check.
         * (branch_lo - main_target + span_norm) == -1 → PATH 2b (no match);
         * else PATH 2a. Both orig branches `break` out of the scan loop here. */
        int probe = branch_lo - main_target + span_norm;
        return (probe == -1) ? 0 : 1;
    }
    return 0;
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
     * [CONFIRMED @ 0x42baf4: SHR by 2; gModelsDatEntryTable @ 0x004AEE60]
     *
     * Reverse-direction mirror [CONFIRMED @ 0x0042BB80-0x0042BB97 RunRaceFrame]:
     *   if (gReverseTrackDirection) eff = g_trackTotalSpanCount - span_index;
     *   else                        eff = span_index;
     *   dl_index = eff >> 2;
     * MODELS.DAT is loaded once per level (forward index) and shared across
     * directions; in reverse, the strip walker hands back STRIPB.DAT span
     * indices which must be re-mapped into the forward-frame MODELS.DAT slot
     * before the >>2.  Without the mirror, reverse renders MODELS.DAT
     * geometry from forward span_index>>2, which can be ~1.5M world units
     * away from the player and gets frustum-culled (Sydney-reverse 0/256). */
    if (s_models_blob && s_models_entry_offsets &&
        s_models_display_list_count > 0 && span_index >= 0) {
        int eff_index = span_index;
        /* Reverse-direction mirror operates on the main-road ring length
         * (g_trackTotalSpanCount in the original = STRIP/STRIPB header[1]),
         * NOT on the physical span count (which includes branches).
         * Branch spans (>= ring_length) keep their raw index — they fall
         * through to the STRIP fallback below since branch geometry isn't
         * in MODELS.DAT anyway. */
        int ring = g_td5.track_span_ring_length;
        if (g_td5.reverse_direction && ring > 0 && span_index < ring)
            eff_index = ring - 1 - span_index;
        int dl_index = eff_index >> 2;  /* ~4 spans per display list block */
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
     *
     * [CONFIRMED @ 0x00445450] Byte-faithful with orig ComputeActorTrackContactNormal.
     *
     * Triangle-pick + barycentric math verified end-to-end against the
     * orig 2026-05-18 (L5 promotion audit):
     *   - DAT_00474e40/41 LUT mirrors `k_quad_vertex_offsets[12][2]` byte-for-byte
     *     [CONFIRMED @ 0x00474e40, dumped 2026-05-18: types 3/4 = (-1,0),
     *      6/7 = (0,-1), all others (0,0)].
     *   - Sub_lane==0 / sub_lane==max-1 / interior branches reproduce orig's
     *     per-type triangle selection (probe_span_lane_height at td5_track.c:3367).
     *     Cross-product diagonal test uses identical (vl0, vr1) pivots.
     *   - Bary plane equation matches ComputeTrackTriangleBarycentrics @ 0x004456d0:
     *     port computes `va.y - ((local-va).x*nx + (local-va).z*nz)/ny`, orig
     *     computes `va.y + ((va-local).x*nx + (va-local).z*nz)/ny`. Algebraically
     *     identical via sign distribution (port comment at td5_track.c:3137).
     *   - Final `*out_y = bary_height*256 + origin_y*256` matches orig
     *     `*param_3 = iVar9 + *piVar3 * 0x100` (port returns (origin_y + bary)<<8
     *     after triangle_height factors out the *256).
     *
     * [ARCH-DIVERGENCE — port splits orig 0x00445450 into wrapper + 2 helpers]
     *
     * Original 0x00445450 does THREE jobs in one function:
     *   (1) write probe[4]/[5] = (li/ri + sub_lane + DAT_00474e40/41[type*2]),
     *   (2) per-type triangle pick (sub_lane edge vs interior),
     *   (3) call ComputeTrackTriangleBarycentrics + write origin_y*256.
     *
     * Port splits these into:
     *   - `td5_track_compute_probe_contact_vertices` (td5_track.c:3004) — job (1).
     *   - `td5_track_compute_contact_height` / `probe_span_lane_height`
     *     (td5_track.c:3367) — jobs (2)+(3).
     *   - This thin wrapper — calls (2)+(3) only, does NOT write probe[4]/[5].
     *
     * Rationale for the split: jobs (1) and (2)+(3) have independent lifecycles
     * in the port — the physics body-probe path needs both, but the camera path
     * (td5_camera.c:624,627,630) only needs (2)+(3) on disposable temp probes.
     * Calling job (1) via the camera path would write contact_vertex_A/B to
     * stack-local probes whose memory is reclaimed before any consumer reads
     * them. Keeping (1) separate keeps the camera path clean and avoids 12
     * wasted bytes per camera-sample (4 probes × 3 ticks × ...).
     *
     * Caller adaptation:
     *   - Physics body-probe path (td5_physics.c:6913+6923) calls
     *     `td5_track_compute_probe_contact_vertices` and then this wrapper
     *     in sequence — semantically equivalent to orig.
     *   - Camera callers (td5_camera.c) call only this wrapper; probe[4]/[5]
     *     writes would be dead, so the omission is benign.
     *
     * Risk: any future caller that uses a LONG-LIVED probe AND reads
     * probe[4]/[5] downstream of this call would see stale data. None exist
     * today. Add a TODO to td5_track_compute_probe_contact_vertices' callsite
     * if a new caller pattern emerges. See
     * todo_compute_actor_track_contact_normal_2026-05-18.md for the original
     * split-discovery audit.
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

/*
 * td5_track_get_primary_route_heading — compute track-forward heading for a span.
 *
 * RE basis: InitializeActorTrackPose (0x00434350) + ComputeActorTrackHeading
 *           (0x00435CE0). Both inline the same heading computation:
 *
 *   1. Load strip record at g_trackStripRecords + span_index * 0x18.
 *      [CONFIRMED @ 0x00434380: *0x18 stride used in both functions]
 *   2. Strip byte[0] = span_type; ushort[2] = left_vertex_index (+4);
 *      ushort[3] = right_vertex_index (+6). [CONFIRMED @ 0x00434390]
 *   3. Vertex pool: entry i = short[3]{x,y,z} at +i*6 bytes.
 *      [CONFIRMED @ 0x00434390: (uint)*(ushort*)(puVar1+4)*6]
 *   4. dx/dz computed by span_type switch (cases 1/2/5, 3/4, 6/7).
 *      [CONFIRMED @ 0x00434408/0x00434410/0x00434450]
 *   5. Signed divide-by-4 rounding toward zero.
 *      [CONFIRMED @ 0x00434408: (v + (v>>31 & 3)) >> 2]
 *   6. AngleFromVector12(dx, dz) & 0xFFF returns a 12-bit heading.
 *      [CONFIRMED @ 0x004344D0/0x00434501 in InitializeActorTrackPose]
 *
 * Returns the lateral (right→left) heading of the span in 12-bit angle units
 * (0x000–0xFFF = full circle). Forward heading is this value + 0x400.
 * Returns 0 on invalid span.
 */
int td5_track_get_primary_route_heading(int span_index)
{
    TD5_StripSpan   *sp;
    TD5_StripVertex *vl0, *vl1, *vr0, *vr1;
    int32_t dx, dz;

    if (!s_span_array || !s_vertex_table)
        return 0;
    if (span_index < 0 || span_index >= s_span_count)
        return 0;

    sp = &s_span_array[span_index];

    /* Base vertices — sub_lane NOT added [CONFIRMED @ 0x00434390] */
    vl0 = vertex_at(sp->left_vertex_index);
    vl1 = vertex_at(sp->left_vertex_index + 1);
    vr0 = vertex_at(sp->right_vertex_index);
    vr1 = vertex_at(sp->right_vertex_index + 1);

    switch (sp->span_type) {
    case 1: case 2: case 5:
    case 8: case 9: case 10: case 11:
        /* [CONFIRMED @ 0x00434408] standard quad */
        dx = ((int32_t)vl1->x - (int32_t)vr1->x) - (int32_t)vr0->x + (int32_t)vl0->x;
        dz = ((int32_t)vl1->z - (int32_t)vr1->z) - (int32_t)vr0->z + (int32_t)vl0->z;
        dx = (dx + ((dx >> 31) & 3)) >> 2;
        dz = (dz + ((dz >> 31) & 3)) >> 2;
        break;
    case 3: case 4:
        /* [CONFIRMED @ 0x00434410] diagonal — right+2 vertex */
        {
            TD5_StripVertex *vr2 = vertex_at(sp->right_vertex_index + 2);
            dx = ((int32_t)vl1->x - (int32_t)vr2->x) - (int32_t)vr1->x + (int32_t)vl0->x;
            dz = ((int32_t)vl1->z - (int32_t)vr2->z) - (int32_t)vr1->z + (int32_t)vl0->z;
        }
        dx = (dx + ((dx >> 31) & 3)) >> 2;
        dz = (dz + ((dz >> 31) & 3)) >> 2;
        break;
    case 6: case 7:
        /* [CONFIRMED @ 0x00434450] reversed winding — left+2 vertex */
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

    return AngleFromVector12(dx, dz) & 0xFFF;
}

/* ========================================================================
 * Debug overlay: collision wireframe
 *
 * Emits world-space line segments showing the rail and transverse edges of
 * spans within ±span_radius of center_span. The same geometry is what the
 * wall-contact resolvers test probes against, so this is a live view of the
 * collision world as the physics layer sees it.
 *
 * Color scheme:
 *   white   — left/right rails (the actual wall edges)
 *   cyan    — transverse near/far edges (span boundaries)
 *   yellow  — current player span (highlighted on top of white/cyan)
 * ======================================================================== */

static inline void emit_strip_line(const TD5_StripSpan *sp,
                                   const TD5_StripVertex *a,
                                   const TD5_StripVertex *b,
                                   uint32_t argb)
{
    if (!a || !b) return;
    float ax = (float)((int32_t)sp->origin_x + (int32_t)a->x);
    float ay = (float)((int32_t)sp->origin_y + (int32_t)a->y);
    float az = (float)((int32_t)sp->origin_z + (int32_t)a->z);
    float bx = (float)((int32_t)sp->origin_x + (int32_t)b->x);
    float by = (float)((int32_t)sp->origin_y + (int32_t)b->y);
    float bz = (float)((int32_t)sp->origin_z + (int32_t)b->z);
    td5_render_debug_line_world(ax, ay, az, bx, by, bz, argb);
}

static void emit_span_wireframe(int span_index, uint32_t rail_color,
                                uint32_t cross_color)
{
    if (span_index < 0 || span_index >= s_span_count) return;
    const TD5_StripSpan *sp = &s_span_array[span_index];

    /* Skip sentinels / non-geometry spans. */
    if (sp->span_type == 9 || sp->span_type == 10) return;

    int lane_count = span_lane_count(sp);
    if (lane_count < 1) lane_count = 1;

    int li = (int)sp->left_vertex_index;
    int ri = (int)sp->right_vertex_index;

    /* Layout (corrected 2026-04-28):
     *   li_base + i   = NEAR row, i in [0..lane_count] (left→right)
     *   ri_base + i   = FAR  row */
    TD5_StripVertex *nw = vertex_at(li + 0);
    TD5_StripVertex *ne = vertex_at(li + lane_count);
    TD5_StripVertex *sw = vertex_at(ri + 0);
    TD5_StripVertex *se = vertex_at(ri + lane_count);

    /* Left rail (NW→SW) and right rail (NE→SE) — the actual wall edges. */
    emit_strip_line(sp, nw, sw, rail_color);
    emit_strip_line(sp, ne, se, rail_color);

    /* Transverse edges (NW→NE, SW→SE) — span boundaries. */
    emit_strip_line(sp, nw, ne, cross_color);
    emit_strip_line(sp, sw, se, cross_color);

    /* Inner sub-lane separators (cosmetic, helps visualize lane count). */
    for (int i = 1; i < lane_count; i++) {
        TD5_StripVertex *n = vertex_at(li + i);
        TD5_StripVertex *s = vertex_at(ri + i);
        emit_strip_line(sp, n, s, 0xFF404040u);
    }
}

void td5_track_debug_emit_collision_lines(int center_span, int span_radius)
{
    if (!s_span_array || !s_vertex_table || s_span_count <= 0) return;
    if (span_radius < 0) span_radius = 0;
    if (span_radius > s_span_count) span_radius = s_span_count;

    const uint32_t RAIL_COLOR  = 0xFFFFFFFFu; /* white */
    const uint32_t CROSS_COLOR = 0xFF00FFFFu; /* cyan */
    const uint32_t PLAYER_RAIL = 0xFFFFFF00u; /* yellow */
    const uint32_t PLAYER_CROSS = 0xFFFFA000u;/* amber */

    int lo = center_span - span_radius;
    int hi = center_span + span_radius;
    if (lo < 0) lo = 0;
    if (hi >= s_span_count) hi = s_span_count - 1;

    for (int s = lo; s <= hi; s++) {
        if (s == center_span) continue;
        emit_span_wireframe(s, RAIL_COLOR, CROSS_COLOR);
    }
    /* Player span last so it draws on top of any overlapping neighbors. */
    if (center_span >= 0 && center_span < s_span_count)
        emit_span_wireframe(center_span, PLAYER_RAIL, PLAYER_CROSS);
}
