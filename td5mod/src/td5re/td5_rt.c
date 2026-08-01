/**
 * td5_rt.c -- game-side ray-traced lighting layer (LIGHTING QUALITY: HIGH).
 * PORT-ONLY. See docs/plans/RT_LIGHTING_PLAN.md and td5_rt.h.
 *
 * Phase 0: capability + activation predicates.
 * Phase 1: world-space geometry feed (track span table + active actor meshes)
 *   into the wrapper's acceleration structures, per-frame TLAS assembly, and a
 *   primary-ray debug view (TD5RE_RT_DEBUGVIEW) used to prove the AS matches the
 *   raster world before the shadow/reflection phases build on it.
 *
 * Coordinate space: FLOAT world units (24.8 / 256.0, +Y down). Track strip
 * vertices are integer world units natively; camera position (24.8) is scaled
 * by 1/256; actor render_pos is already world_pos/256.
 */
#include "td5_rt.h"
#include "td5_platform.h"
#include "td5_config.h"
#include "td5_types.h"
#include "td5_track.h"
#include "td5_render.h"
#include "td5_camera.h"
#include "td5_race_state.h"
#include "td5_ai.h"
#include "../../../re/include/td5_actor_struct.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

/* One-shot diagnostic (TD5RE_RT_DIAG=1): dump feed stats to log/rt_diag.log. */
static int rt_diag_on(void)
{
    static int v = -1;
    if (v < 0) v = td5_env_int("TD5RE_RT_DIAG", 0, 0, 1);
    return v;
}
static void rt_diag(const char *fmt, ...)
{
    FILE *f;
    if (!rt_diag_on()) return;
    f = fopen("log/rt_diag.log", "a");
    if (f) { va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap); fputc('\n', f); fflush(f); fclose(f); }
}

/* -1 = unread (seed from env on first query). 0 = LOW, 1 = HIGH. */
static int s_quality_high = -1;

static int rt_quality_seed(void)
{
    if (s_quality_high < 0)
        s_quality_high = td5_env_int("TD5RE_RT", 0, 0, 1);
    return s_quality_high;
}

int td5_rt_available(void)
{
    return td5_plat_rt_available();
}

int td5_rt_quality_high(void)
{
    return rt_quality_seed();
}

void td5_rt_set_quality(int high)
{
    s_quality_high = high ? 1 : 0;
}

int td5_rt_active(void)
{
    if (!td5_rt_available()) return 0;
    if (!rt_quality_seed())  return 0;
    /* "in a race, not frontend/FMV, lighting enabled" is refined in Phase 2b
     * where td5_rt_active() first gates an actual dispatch. Phase 0/1 have no
     * caller of active(), so availability + quality is a safe predicate. */
    return 1;
}

/* ======================================================================== *
 *  Phase 1 -- geometry feed + per-frame TLAS + debug view
 * ======================================================================== */

#define RT_CHUNK_VERT_BUDGET   60000   /* flush a track chunk near the u16 limit */
#define RT_MAX_TRACK_CHUNKS    256
#define RT_ACTOR_CACHE         128
#define RT_INV256              (1.0f / 256.0f)

static int      s_track_handles[RT_MAX_TRACK_CHUNKS];
static int      s_track_chunk_count;
static struct { const TD5_MeshHeader *mesh; int handle; } s_actor_cache[RT_ACTOR_CACHE];
static int      s_actor_cache_count;
static unsigned s_rt_generation;         /* last observed Backend_RTGeneration   */
static int      s_debugview = -1;        /* TD5RE_RT_DEBUGVIEW latched           */

static int rt_debugview_enabled(void)
{
    if (s_debugview < 0) s_debugview = td5_env_int("TD5RE_RT_DEBUGVIEW", 0, 0, 2);
    return s_debugview;
}

static void rt_destroy_meshes(void)
{
    int i;
    for (i = 0; i < s_track_chunk_count; i++)
        if (s_track_handles[i]) td5_plat_rt_mesh_destroy(s_track_handles[i]);
    s_track_chunk_count = 0;
    for (i = 0; i < s_actor_cache_count; i++)
        if (s_actor_cache[i].handle) td5_plat_rt_mesh_destroy(s_actor_cache[i].handle);
    s_actor_cache_count = 0;
}

/* ---- track feed: walk the strip span table -> chunked static meshes --------
 * Each span carries `lane_count` lane quads (span-type-aware vertex layout);
 * emit every lane so the whole drivable width is present. Chunk by a vertex
 * budget (u16 indices) rather than a fixed span count. */

/* Flush the accumulated verts/idx as one RT mesh handle. */
static void rt_track_flush(TD5_RTVertex *verts, int nv, unsigned short *idx, int ni)
{
    TD5_RTRange range;
    int handle;
    if (nv <= 0 || ni < 3 || s_track_chunk_count >= RT_MAX_TRACK_CHUNKS) return;
    range.first_index = 0; range.index_count = (unsigned)ni;
    range.texture_id = 0; range.matid_flags = 0;
    handle = td5_plat_rt_mesh_create(verts, (unsigned)nv, idx, (unsigned)ni, &range, 1);
    if (handle) s_track_handles[s_track_chunk_count++] = handle;
}

void td5_rt_level_build(void)
{
    int span_count, s, lane, nv, ni, cap_v, cap_i;
    TD5_RTVertex  *verts;
    unsigned short *idx;

    if (!td5_rt_available()) return;
    rt_destroy_meshes();
    s_rt_generation = td5_plat_rt_generation();

    span_count = td5_track_get_span_count();
    if (span_count <= 0 || !g_strip_vertex_base) return;

    /* One chunk buffer, reused; flush + restart when near the u16 vertex cap. */
    cap_v = RT_CHUNK_VERT_BUDGET + 64;
    cap_i = (cap_v / 4) * 6 + 64;
    verts = (TD5_RTVertex *)malloc((size_t)cap_v * sizeof(TD5_RTVertex));
    idx   = (unsigned short *)malloc((size_t)cap_i * sizeof(unsigned short));
    if (!verts || !idx) { free(verts); free(idx); return; }

    nv = 0; ni = 0;
    for (s = 0; s < span_count; s++) {
        int lc = td5_track_get_span_lane_count(s);
        if (lc < 1) lc = 1;
        for (lane = 0; lane < lc; lane++) {
            float q[4][3]; int c, k;
            float mn[3], mx[3];
            if (!td5_track_get_lane_quad_world(s, lane, q)) continue;
            /* Reject implausibly large quads: some span types (junctions) yield a
             * +1 vertex that belongs to a different run, producing a stray tri
             * shooting off into the sky. Legit lane quads are a few thousand
             * units at most; drop anything an order of magnitude larger. */
            for (k = 0; k < 3; k++) { mn[k] = mx[k] = q[0][k]; }
            for (c = 1; c < 4; c++) for (k = 0; k < 3; k++) { if (q[c][k] < mn[k]) mn[k] = q[c][k]; if (q[c][k] > mx[k]) mx[k] = q[c][k]; }
            if (mx[0]-mn[0] > 12000.0f || mx[1]-mn[1] > 12000.0f || mx[2]-mn[2] > 12000.0f) continue;
            if (nv + 4 > RT_CHUNK_VERT_BUDGET) { rt_track_flush(verts, nv, idx, ni); nv = 0; ni = 0; }
            for (c = 0; c < 4; c++) {
                verts[nv + c].pos[0] = q[c][0]; verts[nv + c].pos[1] = q[c][1]; verts[nv + c].pos[2] = q[c][2];
                verts[nv + c].uv[0] = 0.0f; verts[nv + c].uv[1] = 0.0f;
                verts[nv + c].color = 0xFFFFFFFFu;
            }
            /* ring nearL,farL,farR,nearR -> (0,1,2)+(0,2,3). */
            idx[ni+0]=(unsigned short)(nv+0); idx[ni+1]=(unsigned short)(nv+1); idx[ni+2]=(unsigned short)(nv+2);
            idx[ni+3]=(unsigned short)(nv+0); idx[ni+4]=(unsigned short)(nv+2); idx[ni+5]=(unsigned short)(nv+3);
            nv += 4; ni += 6;
            if (s_track_chunk_count >= RT_MAX_TRACK_CHUNKS) { s = span_count; break; }
        }
    }
    rt_track_flush(verts, nv, idx, ni);
    free(verts); free(idx);

    if (rt_diag_on()) {
        float q[4][3];
        int lc0 = td5_track_get_span_lane_count(0);
        rt_diag("LEVEL_BUILD spans=%d chunks=%d", span_count, s_track_chunk_count);
        if (td5_track_get_lane_quad_world(110, 0, q))
            rt_diag("  span110 lane0 quad: nL(%.0f,%.0f,%.0f) fL(%.0f,%.0f,%.0f) fR(%.0f,%.0f,%.0f) nR(%.0f,%.0f,%.0f) lanes(sp0)=%d",
                    q[0][0],q[0][1],q[0][2], q[1][0],q[1][1],q[1][2], q[2][0],q[2][1],q[2][2], q[3][0],q[3][1],q[3][2], lc0);
        {
            int lc = td5_track_get_span_lane_count(110), L;
            for (L = 0; L < lc; L++)
                if (td5_track_get_lane_quad_world(110, L, q))
                    rt_diag("  span110 lane%d: nL.x=%.0f nR.x=%.0f  nL.z=%.0f nR.z=%.0f", L, q[0][0], q[3][0], q[0][2], q[3][2]);
        }
    }
}

void td5_rt_level_unload(void)
{
    rt_destroy_meshes();
}

/* ---- actor mesh feed: TD5_MeshHeader (object space) -> RT mesh ------------- */

static int rt_build_actor_mesh(const TD5_MeshHeader *mesh)
{
    int nv, c, cursor, nidx, cap_idx, handle;
    const TD5_MeshVertex  *mv;
    const TD5_PrimitiveCmd *cmds;
    TD5_RTVertex  *verts;
    unsigned short *idx;
    TD5_RTRange range;

    if (!mesh || mesh->command_count <= 0 || mesh->total_vertex_count <= 0) return 0;
    nv = mesh->total_vertex_count;
    if (nv > 65535) return 0;                 /* u16 index limit (car meshes are small) */
    mv = mesh->vertices; cmds = mesh->commands;
    if (!mv || !cmds) return 0;

    verts = (TD5_RTVertex *)malloc((size_t)nv * sizeof(TD5_RTVertex));
    if (!verts) return 0;
    for (c = 0; c < nv; c++) {
        verts[c].pos[0] = mv[c].pos_x; verts[c].pos[1] = mv[c].pos_y; verts[c].pos[2] = mv[c].pos_z;
        verts[c].uv[0] = mv[c].tex_u; verts[c].uv[1] = mv[c].tex_v;
        verts[c].color = mv[c].lighting;
    }
    cap_idx = 0;
    for (c = 0; c < mesh->command_count; c++)
        cap_idx += (int)cmds[c].triangle_count * 3 + (int)cmds[c].quad_count * 6;
    idx = (unsigned short *)malloc((size_t)(cap_idx > 0 ? cap_idx : 1) * sizeof(unsigned short));
    if (!idx) { free(verts); return 0; }

    nidx = 0; cursor = 0;
    for (c = 0; c < mesh->command_count; c++) {
        const TD5_PrimitiveCmd *cmd = &cmds[c];
        int op = cmd->dispatch_type;
        int tris = cmd->triangle_count, quads = cmd->quad_count;
        int t, q, base, qbase;
        /* External-vertex or non-solid (billboard/translucent) commands break the
         * sequential-vertex stride assumption -> stop at the first one, keeping a
         * correct solid-geometry prefix (Phase 3 refines UV/texture per range). */
        if (cmd->vertex_data_ptr != 0) break;
        if (op < 0 || op > 3) break;
        base = cursor;
        for (t = 0; t < tris; t++) {
            if (base + t * 3 + 2 >= nv) break;
            idx[nidx++] = (unsigned short)(base + t * 3 + 0);
            idx[nidx++] = (unsigned short)(base + t * 3 + 1);
            idx[nidx++] = (unsigned short)(base + t * 3 + 2);
        }
        qbase = base + tris * 3;
        for (q = 0; q < quads; q++) {
            int a = qbase + q * 4;
            if (a + 3 >= nv) break;
            idx[nidx++] = (unsigned short)(a + 0); idx[nidx++] = (unsigned short)(a + 1); idx[nidx++] = (unsigned short)(a + 2);
            idx[nidx++] = (unsigned short)(a + 0); idx[nidx++] = (unsigned short)(a + 2); idx[nidx++] = (unsigned short)(a + 3);
        }
        cursor += tris * 3 + quads * 4;
    }

    handle = 0;
    if (nidx >= 3) {
        range.first_index = 0; range.index_count = (unsigned)nidx;
        range.texture_id = (unsigned)mesh->texture_page_id; range.matid_flags = 0;
        handle = td5_plat_rt_mesh_create(verts, (unsigned)nv, idx, (unsigned)nidx, &range, 1);
    }
    if (rt_diag_on()) {
        float mn[3]={1e30f,1e30f,1e30f}, mx[3]={-1e30f,-1e30f,-1e30f}; int vi,k;
        for (vi=0; vi<nv; vi++) for (k=0;k<3;k++){ if(verts[vi].pos[k]<mn[k])mn[k]=verts[vi].pos[k]; if(verts[vi].pos[k]>mx[k])mx[k]=verts[vi].pos[k]; }
        rt_diag("  ACTOR_MESH cmds=%d nv=%d nidx=%d handle=%d bbox x[%.0f,%.0f] y[%.0f,%.0f] z[%.0f,%.0f]",
                mesh->command_count, nv, nidx, handle, mn[0],mx[0],mn[1],mx[1],mn[2],mx[2]);
    }
    free(verts); free(idx);
    return handle;
}

static int rt_actor_mesh_handle(const TD5_MeshHeader *mesh)
{
    int i, h;
    if (!mesh) return 0;
    for (i = 0; i < s_actor_cache_count; i++)
        if (s_actor_cache[i].mesh == mesh) return s_actor_cache[i].handle;
    if (s_actor_cache_count >= RT_ACTOR_CACHE) return 0;
    h = rt_build_actor_mesh(mesh);
    s_actor_cache[s_actor_cache_count].mesh = mesh;
    s_actor_cache[s_actor_cache_count].handle = h;   /* cache 0 too (don't retry a failed mesh) */
    s_actor_cache_count++;
    return h;
}

/* ---- per-frame driver ----------------------------------------------------- */

static void rt_build_tlas(void)
{
    static const float identity[12] = { 1,0,0,0, 0,1,0,0, 0,0,1,0 };
    static int s_diag_frames = 0;
    int diag_this = rt_diag_on() && (s_diag_frames++ < 1);
    int i, slot, total;

    td5_plat_rt_scene_begin();
    if (!td5_env_int("TD5RE_RT_ONLYCARS", 0, 0, 1))
        for (i = 0; i < s_track_chunk_count; i++)
            if (s_track_handles[i]) td5_plat_rt_scene_instance(s_track_handles[i], identity, 0);

    total = td5_game_get_total_actor_count();
    for (slot = 0; slot < total; slot++) {
        TD5_Actor *actor = td5_game_get_actor(slot);
        TD5_MeshHeader *mesh;
        float m[12];
        int h;
        if (!actor) continue;
        mesh = td5_render_get_vehicle_mesh(slot);
        if (!mesh) continue;
        if (slot < g_traffic_slot_base && td5_game_get_slot_state(slot) == 3) continue;
        if (td5_ai_traffic_get_draw_alpha(slot) == 0) continue;

        h = rt_actor_mesh_handle(mesh);
        if (!h) continue;
        /* model->world 3x4 (row-major): [ rotation_matrix | world_pos/256 ].
         * world_pos (24.8 fixed) is the authoritative position for all three
         * axes; render_pos.y is not populated at this point in the frame. */
        {
            float px = (float)actor->world_pos.x * RT_INV256;
            float py = (float)actor->world_pos.y * RT_INV256;
            float pz = (float)actor->world_pos.z * RT_INV256;
            m[0]=actor->rotation_matrix.m[0]; m[1]=actor->rotation_matrix.m[1]; m[2]=actor->rotation_matrix.m[2];  m[3]=px;
            m[4]=actor->rotation_matrix.m[3]; m[5]=actor->rotation_matrix.m[4]; m[6]=actor->rotation_matrix.m[5];  m[7]=py;
            m[8]=actor->rotation_matrix.m[6]; m[9]=actor->rotation_matrix.m[7]; m[10]=actor->rotation_matrix.m[8]; m[11]=pz;
        }
        td5_plat_rt_scene_instance(h, m, 0);
        if (diag_this)
            rt_diag("  INSTANCE slot=%d h=%d world/256=(%.0f,%.0f,%.0f) rot0=%.3f rot4=%.3f rot8=%.3f",
                    slot, h, (float)actor->world_pos.x*RT_INV256, (float)actor->world_pos.y*RT_INV256, (float)actor->world_pos.z*RT_INV256,
                    actor->rotation_matrix.m[0], actor->rotation_matrix.m[4], actor->rotation_matrix.m[8]);
    }
    td5_plat_rt_scene_end();
    if (diag_this) rt_diag("TLAS built: track_chunks=%d total_actors=%d", s_track_chunk_count, td5_game_get_total_actor_count());
}

void td5_rt_frame(int vp, int pane_x, int pane_y, int pane_w, int pane_h)
{
    float cam[3], right[3], up[3], fwd[3], basis9[9];
    unsigned gen;

    if (!td5_rt_available()) return;   /* build ASes whenever DXR is present (plan 1.2) */

    /* Device-lost: meshes were destroyed with the old device -> re-feed. */
    gen = td5_plat_rt_generation();
    if (gen != s_rt_generation) {
        td5_rt_level_build();   /* rebuilds track from the still-loaded span table */
    }

    /* TLAS is world-space and shared across panes: build once per frame (vp 0). */
    if (vp == 0) {
        rt_build_tlas();
        /* HIGH mode: the deferred shadow/light passes run the RT composite. */
        td5_plat_rt_set_mode(td5_rt_active());
    }

    /* Per-pane camera view. td5_camera_get_position returns FLOAT world units
     * already (same ~136180 scale as track verts and world_pos/256) -- do NOT
     * divide by 256 (verified via diag: /256 placed the camera 256x too close
     * to the origin, ~135k units from the scene). */
    td5_camera_get_position(&cam[0], &cam[1], &cam[2]);
    td5_camera_get_basis(right, up, fwd);
    basis9[0]=right[0]; basis9[1]=right[1]; basis9[2]=right[2];
    basis9[3]=up[0];    basis9[4]=up[1];    basis9[5]=up[2];
    basis9[6]=fwd[0];   basis9[7]=fwd[1];   basis9[8]=fwd[2];
    td5_plat_rt_set_view(cam, basis9,
                         td5_render_get_focal_length(),
                         td5_render_get_center_x(), td5_render_get_center_y(),
                         pane_x, pane_y, pane_w, pane_h, NULL);
    { static int camlog=0; if (rt_diag_on() && camlog<1){camlog=1;
        rt_diag("CAM pos=(%.0f,%.0f,%.0f) right=(%.2f,%.2f,%.2f) fwd=(%.2f,%.2f,%.2f) focal=%.1f center=(%.0f,%.0f)",
            cam[0],cam[1],cam[2], basis9[0],basis9[1],basis9[2], basis9[6],basis9[7],basis9[8],
            td5_render_get_focal_length(), td5_render_get_center_x(), td5_render_get_center_y()); } }

    if (rt_debugview_enabled())
        td5_plat_rt_debug_view();
}
