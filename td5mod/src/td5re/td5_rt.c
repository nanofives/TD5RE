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
#include "td5re.h"          /* g_td5 (INI: lighting_quality, lighting_enabled) */
#include "td5_platform.h"
#include "td5_config.h"
#include "td5_types.h"
#include "td5_track.h"
#include "td5_render.h"
#include "td5_asset.h"      /* [P2] td5_asset_get_page_transparency (cutout classing) */
#include "td5_material.h"   /* [P2] td5_material_id_for_page                          */
#include "td5_camera.h"
#include "td5_race_state.h"
#include "td5_ai.h"
#include "td5_input.h"      /* td5_input_is_playback_active (chassis-lift parity) */
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

/* -1 = unread (seed on first query). 0 = LOW, 1 = HIGH. Sourced from the
 * [Lighting] Quality INI key (g_td5.ini.lighting_quality); the TD5RE_RT env knob
 * is a dev A/B override that wins when set. td5_rt_set_quality (menu toggle)
 * overwrites it at runtime for an instant LOW<->HIGH switch. */
static int s_quality_high = -1;

static int rt_quality_seed(void)
{
    if (s_quality_high < 0) {
        const char *e = getenv("TD5RE_RT");
        if (e && e[0])
            s_quality_high = atoi(e) ? 1 : 0;           /* dev override */
        else
            s_quality_high = g_td5.ini.lighting_quality ? 1 : 0;
    }
    return s_quality_high;
}

int td5_rt_available(void)
{
    return td5_plat_rt_available();
}

/* [CAR SHADOW 2026-08-06] see td5_rt.h. Default OFF: cars are grounded by the
 * soft blob (td5_render_effects.c), not the laggy/wheel-less RT car cast. */
int td5_rt_car_cast_shadow(void)
{
    static int s_on = -1;
    if (s_on < 0) s_on = td5_env_flag_on("TD5RE_RT_CAR_CAST");
    return s_on;
}

int td5_rt_quality_high(void)
{
    return rt_quality_seed();
}

void td5_rt_set_quality(int high)
{
    s_quality_high = high ? 1 : 0;
}

/* [RT2 P8] Translate the LIGHTING OPTIONS INI tiers (g_td5.ini.rt_*) onto the
 * TD5RE_RT_* env knobs the RT passes already read. Called once at startup AFTER
 * the INI loads (main.c). An env var the user set explicitly is preserved (the
 * dev A/B override wins) — we only fill a knob the env doesn't already define.
 * Defaults are all HIGHEST, so a fresh INI yields the full-quality RT look.
 * HIGH-only effect: LOW never reads these. */
static void rt_setenv_if_unset(const char *key, const char *val)
{
    const char *cur = getenv(key);
    if (cur && cur[0]) return;                 /* explicit env override wins */
    _putenv_s(key, val);
}

void td5_rt_apply_lighting_options(void)
{
    char buf[32];

    /* SHADOW QUALITY -> sun-shadow sample count (P2b denoise): 1 / 4 / 8. */
    snprintf(buf, sizeof(buf), "%d", g_td5.ini.rt_shadow_rays);
    rt_setenv_if_unset("TD5RE_RT_RAYS", buf);

    /* REFLECTION RANGE -> RT reflection ray TMax (P6): NEAR=SSR horizon,
     * FAR=50000, UNLIMITED=1e7. */
    rt_setenv_if_unset("TD5RE_RT_REFL_DIST",
        (g_td5.ini.rt_reflection_rng <= 0) ? "4000" :
        (g_td5.ini.rt_reflection_rng == 1) ? "50000" : "10000000");

    /* GLOBAL ILLUMINATION -> GI enable + ray count (P4). OFF disables the GI
     * pass, which re-arms the analytic zone dark-mode fallback in HIGH (see the
     * darkener gate in td5_render_mesh.c). LOW=2 rays, HIGH=4. */
    rt_setenv_if_unset("TD5RE_RT_GI", g_td5.ini.rt_gi_quality > 0 ? "1" : "0");
    if (g_td5.ini.rt_gi_quality > 0) {
        snprintf(buf, sizeof(buf), "%d", g_td5.ini.rt_gi_quality >= 2 ? 4 : 2);
        rt_setenv_if_unset("TD5RE_RT_GI_RAYS", buf);
    }

    /* LIGHTS -> P7 soft headlight penumbra + smooth projector cone. BASIC =
     * legacy (1 ray, hard cone); REALISTIC = soft (2 rays, feathered). */
    if (g_td5.ini.rt_light_quality > 0) {
        rt_setenv_if_unset("TD5RE_RT_LIGHT_RAYS", "2");
        rt_setenv_if_unset("TD5RE_RT_LIGHT_CONE_SOFT", "0.15");
    } else {
        rt_setenv_if_unset("TD5RE_RT_LIGHT_RAYS", "1");
        rt_setenv_if_unset("TD5RE_RT_LIGHT_CONE_SOFT", "0");
    }

    /* SUN & SKY -> AUTO = image-probe sun + disc (P1); CLASSIC = no disc.
     * (Full zone-sun-instead-of-image-sun override is a documented residual.) */
    rt_setenv_if_unset("TD5RE_SUN_DISC", g_td5.ini.rt_sun_probe > 0 ? "1" : "0");

    /* REFLECTIONS OFF -> disable the reflection pass entirely (mirrors the
     * existing [Lighting]Reflections toggle). HALF/FULL both dispatch full-res
     * today (the half-res path is the P6.3 residual). */
    if (g_td5.ini.rt_reflection_q <= 0) g_td5.ini.reflections = 0;

    /* rt_shadow_res (HALF/FULL) is a documented residual — the half-res dispatch
     * (P6.3) isn't wired yet; FULL is always used. The INI key persists so the
     * row goes live for free once the mechanism lands. */

    rt_diag("P8 lighting options applied: rays=%d refl_rng=%d gi=%d light_q=%d "
            "sun_probe=%d refl_q=%d shadow_res=%d",
            g_td5.ini.rt_shadow_rays, g_td5.ini.rt_reflection_rng,
            g_td5.ini.rt_gi_quality, g_td5.ini.rt_light_quality,
            g_td5.ini.rt_sun_probe, g_td5.ini.rt_reflection_q,
            g_td5.ini.rt_shadow_res);
}

int td5_rt_active(void)
{
    if (!td5_rt_available()) return 0;    /* no DXR device -> auto-fallback to LOW */
    if (!rt_quality_seed())  return 0;    /* LIGHTING QUALITY = LOW               */
    if (!g_td5.ini.lighting_enabled) return 0;  /* [Lighting] Enabled master gate */
    /* "in a race, not frontend/FMV" is guaranteed by the sole caller: td5_rt_frame
     * runs only from the in-race per-pane deferred site, and the RT passes it
     * arms (shadow/light/SSR) are dispatched only during race rendering -- so the
     * feed + dispatch are naturally dormant in the frontend and during FMV. */
    return 1;
}

/* ======================================================================== *
 *  Phase 1 -- geometry feed + per-frame TLAS + debug view
 * ======================================================================== */

#define RT_CHUNK_VERT_BUDGET   60000   /* flush a track chunk near the u16 limit */
#define RT_MAX_TRACK_CHUNKS    256
#define RT_ACTOR_CACHE         128
#define RT_INV256              (1.0f / 256.0f)
/* [RT2-P2] full-scene world feed. Static "world" BLAS set built once at level
 * load: one BLAS per display-list scenery mesh + per billboard-chunk. Capped so
 * track chunks + scenery + actors stay under the wrapper's 1024 instance cap. */
#define RT_MAX_SCENERY         1900
#define RT_MATID_CUTOUT        0x100u   /* mirror of DXR_MATID_CUTOUT (d3d12_dxr.c) */

static int      s_track_handles[RT_MAX_TRACK_CHUNKS];
static int      s_track_chunk_count;
static int      s_scenery_handles[RT_MAX_SCENERY];
static unsigned char s_scenery_mask[RT_MAX_SCENERY];  /* [road-cast] per-mesh TLAS InstanceMask */
static int      s_scenery_count;
static int      s_scenery_fed;             /* one-shot: MODELS ready by frame 1 */

static void rt_feed_world_scenery(void);   /* defined below td5_rt_level_build */
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
    for (i = 0; i < s_scenery_count; i++)
        if (s_scenery_handles[i]) td5_plat_rt_mesh_destroy(s_scenery_handles[i]);
    s_scenery_count = 0;
    s_scenery_fed = 0;                      /* re-feed after unload / device-lost */
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

    /* [P3 memory] Only feed when RT is ACTIVE (Quality=HIGH). The level-load hook
     * calls this unconditionally; in LOW it must not build the ASes (td5_rt_frame
     * lazily rebuilds on a later LOW->HIGH switch). See td5_rt_frame. */
    if (!td5_rt_active()) return;
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
                /* [P3] Road lane quads carry no texture/UV in the feed, so the
                 * reflection hit shading (chit_refl) uses vertex color directly.
                 * A representative dark-asphalt gray, NOT white: white here made
                 * every reflective surface (which mostly reflects the road) wash
                 * near-white -- the Phase 3 blowout. Kept opaque (A=0xFF). */
                verts[nv + c].color = 0xFF4A4A4Au;
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

    /* NB: the full-scene scenery feed is NOT done here. This level_build hook
     * runs from the track loader (td5_track.c) BEFORE MODELS.DAT is parsed, so
     * the display-list table is empty here. td5_rt_frame feeds scenery on the
     * first HIGH frame instead (s_scenery_fed), by which point MODELS is ready. */

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

static int rt_build_actor_mesh(const TD5_MeshHeader *mesh, unsigned matid_flags)
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
        range.texture_id = (unsigned)mesh->texture_page_id; range.matid_flags = matid_flags;
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
    h = rt_build_actor_mesh(mesh, 0u);   /* actors: opaque, matid 0 */
    s_actor_cache[s_actor_cache_count].mesh = mesh;
    s_actor_cache[s_actor_cache_count].handle = h;   /* cache 0 too (don't retry a failed mesh) */
    s_actor_cache_count++;
    return h;
}

/* ======================================================================== *
 *  [RT2-P2] Full-scene world feed: MODELS.DAT display-list scenery + billboards
 * ======================================================================== */

/* Dedup set: a display-list mesh appears in many span entries (the renderer
 * dedups the same way). Bounded; overflow just re-emits (harmless, rare). */
static const void *s_seen[8192];
static int         s_seen_count;
static int rt_seen(const void *p)
{
    int i;
    for (i = 0; i < s_seen_count; i++) if (s_seen[i] == p) return 1;
    if (s_seen_count < (int)(sizeof(s_seen)/sizeof(s_seen[0]))) s_seen[s_seen_count++] = p;
    return 0;
}

/* Material id + CUTOUT flag from a scenery mesh's texture page. */
static unsigned rt_scenery_matid(const TD5_MeshHeader *mesh)
{
    int page = mesh->texture_page_id;
    unsigned mat = td5_material_id_for_page(page);
    if (td5_asset_get_page_transparency(page) == 1) mat |= RT_MATID_CUTOUT;   /* alpha-test */
    return mat;
}

/* Build a static world BLAS from a MODELS.DAT display-list mesh. Unlike the car
 * path (rt_build_actor_mesh, sequential base vertices), scenery commands carry a
 * per-command vertex base in vertex_data_ptr -- resolved here exactly as the
 * renderer does (td5_render_span_display_list) -- so buildings/walls/bridges/
 * terrain extract correctly. Vertices are already WORLD space -> identity
 * instance. Topology is tri_count tris + quad_count quads per command for every
 * opcode (opcode selects render state, not topology). Returns a mesh handle. */
static int rt_build_scenery_mesh(const TD5_MeshHeader *mesh, unsigned matid_flags,
                                 unsigned char *out_mask)
{
    int c, nvo, nidx, cap_v, cap_i, cursor, handle;
    const TD5_MeshVertex   *base = mesh->vertices;
    const TD5_PrimitiveCmd *cmds = mesh->commands;
    TD5_RTVertex   *verts;
    unsigned short *idx;
    TD5_RTRange range;
    float bbmin[3] = { 1e30f, 1e30f, 1e30f }, bbmax[3] = { -1e30f, -1e30f, -1e30f };

    if (out_mask) *out_mask = 0xFFu;
    if (!cmds || mesh->command_count <= 0) return 0;

    /* Output verts = sum of each command's consumed verts (they live in the
     * blob at per-command bases, NOT in mesh->vertices), collected sequentially
     * so u16 indices reference our own buffer. Cap at the u16 limit. */
    cap_v = cap_i = 0;
    for (c = 0; c < mesh->command_count; c++) {
        cap_v += (int)cmds[c].triangle_count * 3 + (int)cmds[c].quad_count * 4;
        cap_i += (int)cmds[c].triangle_count * 3 + (int)cmds[c].quad_count * 6;
    }
    if (cap_v < 3 || cap_v > 65535 || cap_i < 3) return 0;
    verts = (TD5_RTVertex *)malloc((size_t)cap_v * sizeof(TD5_RTVertex));
    idx   = (unsigned short *)malloc((size_t)cap_i * sizeof(unsigned short));
    if (!verts || !idx) { free(verts); free(idx); return 0; }

    nvo = 0; nidx = 0; cursor = 0;
    for (c = 0; c < mesh->command_count; c++) {
        const TD5_PrimitiveCmd *cmd = &cmds[c];
        int tris = cmd->triangle_count, quads = cmd->quad_count;
        int need = tris * 3 + quads * 4, vbase, qb, t, q, j;
        const TD5_MeshVertex *cv;
        if (need <= 0) continue;
        /* Resolve this command's vertex base EXACTLY as the renderer does
         * (td5_render_span_display_list): a small value is a byte offset from the
         * mesh header; a large value is an absolute pointer ONLY IF it lands in
         * the models blob. NOTE: vertex_data_ptr is a uint32_t (on-disk record),
         * so on x64 a stray non-zero value is a TRUNCATED pointer -- must be
         * blob-bounds-checked before any deref or it faults (0xC0000005). */
        if (cmd->vertex_data_ptr != 0) {
            uintptr_t vp = (uintptr_t)cmd->vertex_data_ptr;
            size_t vneed = (size_t)need * sizeof(TD5_MeshVertex);
            if (vp < 0x10000u) {
                cv = (const TD5_MeshVertex *)((const uint8_t *)mesh + vp);
                if (!td5_track_is_ptr_in_blob(cv, vneed)) continue;
            } else if (td5_track_is_ptr_in_blob((const void *)vp, vneed)) {
                cv = (const TD5_MeshVertex *)vp;
            } else {
                continue;                                  /* truncated/bad ptr */
            }
        } else {
            if (!base || cursor + need > mesh->total_vertex_count) { if (!base) continue; else break; }
            cv = base + cursor; cursor += need;
        }
        if (nvo + need > cap_v) break;                     /* safety */
        vbase = nvo;
        for (j = 0; j < need; j++) {
            float px = cv[j].pos_x, py = cv[j].pos_y, pz = cv[j].pos_z;
            verts[nvo].pos[0] = px; verts[nvo].pos[1] = py; verts[nvo].pos[2] = pz;
            verts[nvo].uv[0] = cv[j].tex_u;  verts[nvo].uv[1] = cv[j].tex_v;
            verts[nvo].color = cv[j].lighting;
            if (px < bbmin[0]) bbmin[0] = px; if (px > bbmax[0]) bbmax[0] = px;
            if (py < bbmin[1]) bbmin[1] = py; if (py > bbmax[1]) bbmax[1] = py;
            if (pz < bbmin[2]) bbmin[2] = pz; if (pz > bbmax[2]) bbmax[2] = pz;
            nvo++;
        }
        for (t = 0; t < tris; t++) {
            int a = vbase + t * 3;
            idx[nidx++] = (unsigned short)(a + 0);
            idx[nidx++] = (unsigned short)(a + 1);
            idx[nidx++] = (unsigned short)(a + 2);
        }
        qb = vbase + tris * 3;
        for (q = 0; q < quads; q++) {
            int a = qb + q * 4;
            idx[nidx++] = (unsigned short)(a + 0); idx[nidx++] = (unsigned short)(a + 1); idx[nidx++] = (unsigned short)(a + 2);
            idx[nidx++] = (unsigned short)(a + 0); idx[nidx++] = (unsigned short)(a + 2); idx[nidx++] = (unsigned short)(a + 3);
        }
    }

    handle = 0;
    if (nidx >= 3 && nvo >= 3) {
        range.first_index = 0; range.index_count = (unsigned)nidx;
        range.texture_id = (unsigned)mesh->texture_page_id; range.matid_flags = matid_flags;
        handle = td5_plat_rt_mesh_create(verts, (unsigned)nvo, idx, (unsigned)nidx, &range, 1);
    }
    /* [ROAD-CAST FIX 2026-08-03] Classify a horizontal SLAB (road / ground / kerb):
     * vertical (Y) extent small vs BOTH horizontal extents. A flat surface is a
     * shadow RECEIVER, not a meaningful sun-shadow caster, and per-span road/ground
     * slabs were the source of the alternating near-camera self/cross-shadow
     * stripes (proven: TD5RE_RT_ONLYCARS, which drops all world casters, clears
     * them). Feed such meshes with InstanceMask bit 0 cleared (0xFE) so shadow rays
     * (mask 0x01) skip them; walls/buildings/posts (tall) keep 0xFF and cast.
     * TD5RE_RT_FLATSHADOW=1 restores flat meshes as casters (A/B). */
    /* [2026-08-03] Default: flat scenery meshes DO cast now. The per-span
     * self-shadow stripe this exclusion originally suppressed was really the
     * screen-linear (non-perspective) depth reconstruction bug — fixed by the
     * perspective-depth work + shadow back-face cull. With that fixed, excluding
     * "flat" meshes only mis-fired on real wall/kerb chunks whose bbox happens to
     * be long+wide+low (a wide-based barrier along the road), so some road spans
     * lost their wall shadow. Set TD5RE_RT_FLATSHADOW=0 to restore the old
     * flat-slab exclusion (heuristic below) for A/B. */
    if (out_mask && handle) {
        static int s_flatshadow = -1;
        if (s_flatshadow < 0) s_flatshadow = td5_env_int("TD5RE_RT_FLATSHADOW", 1, 0, 1);
        if (!s_flatshadow) {
            float hx = bbmax[0]-bbmin[0], hy = bbmax[1]-bbmin[1], hz = bbmax[2]-bbmin[2];
            int flat = (hx > 200.0f && hz > 200.0f && hy < 0.25f*hx && hy < 0.25f*hz);
            if (flat) *out_mask = 0xFEu;   /* not a sun-shadow caster */
        }
    }
    free(verts); free(idx);
    return handle;
}

/* Feed one billboard-tag mesh (header page 1/2) as a static CUTOUT crossed-quad
 * pair at its world bounding centre, sized by its bounding radius. Static (NOT
 * camera-facing) so the shadow is stable under camera motion. Real texture page
 * comes from the mesh's first command (the header id is just the billboard tag).
 * +Y is DOWN in world, so the canopy top is at cy - h (smaller Y). */
static int rt_feed_billboard(const TD5_MeshHeader *mesh)
{
    TD5_RTVertex v[8];
    unsigned short idx[12] = { 0,1,2, 0,2,3, 4,5,6, 4,6,7 };
    TD5_RTRange range;
    float cx = mesh->bounding_center_x, cy = mesh->bounding_center_y, cz = mesh->bounding_center_z;
    float r  = mesh->bounding_radius;
    int page, h, i;

    if (r != r || r <= 1.0f) return 0;
    if (cx != cx || cy != cy || cz != cz) return 0;
    if (s_scenery_count >= RT_MAX_SCENERY) return 0;
    page = (mesh->command_count > 0 && mesh->commands) ? mesh->commands[0].texture_page_id : 0;
    if (page <= 2) return 0;                 /* no real page id to alpha-test against */

    {
        float e = r * 0.7071f;               /* half-extent: quad diagonal ~ radius   */
        /* Quad A: XY plane at z=cz (faces ±Z). u across X, v down Y (top=cy-e). */
        v[0].pos[0]=cx-e; v[0].pos[1]=cy-e; v[0].pos[2]=cz;  v[0].uv[0]=0; v[0].uv[1]=0;
        v[1].pos[0]=cx-e; v[1].pos[1]=cy+e; v[1].pos[2]=cz;  v[1].uv[0]=0; v[1].uv[1]=1;
        v[2].pos[0]=cx+e; v[2].pos[1]=cy+e; v[2].pos[2]=cz;  v[2].uv[0]=1; v[2].uv[1]=1;
        v[3].pos[0]=cx+e; v[3].pos[1]=cy-e; v[3].pos[2]=cz;  v[3].uv[0]=1; v[3].uv[1]=0;
        /* Quad B: YZ plane at x=cx (faces ±X). u across Z, v down Y. */
        v[4].pos[0]=cx; v[4].pos[1]=cy-e; v[4].pos[2]=cz-e;  v[4].uv[0]=0; v[4].uv[1]=0;
        v[5].pos[0]=cx; v[5].pos[1]=cy+e; v[5].pos[2]=cz-e;  v[5].uv[0]=0; v[5].uv[1]=1;
        v[6].pos[0]=cx; v[6].pos[1]=cy+e; v[6].pos[2]=cz+e;  v[6].uv[0]=1; v[6].uv[1]=1;
        v[7].pos[0]=cx; v[7].pos[1]=cy-e; v[7].pos[2]=cz+e;  v[7].uv[0]=1; v[7].uv[1]=0;
        for (i = 0; i < 8; i++) v[i].color = 0xFF808080u;   /* mid-grey; tex modulates */
    }
    range.first_index = 0; range.index_count = 12;
    range.texture_id  = (unsigned)page;
    range.matid_flags = td5_material_id_for_page(page) | RT_MATID_CUTOUT;
    h = td5_plat_rt_mesh_create(v, 8, idx, 12, &range, 1);
    if (h) { s_scenery_mask[s_scenery_count] = 0xFFu;   /* billboards cast (cutout leaf shadows) */
             s_scenery_handles[s_scenery_count++] = h; }
    return h != 0;
}

/* Enumerate every display-list scenery mesh once at level load. Solid meshes ->
 * one static world BLAS each (rt_build_actor_mesh, identity instance); billboard
 * meshes -> cutout crossed quads. Deduped by pointer; capped at RT_MAX_SCENERY.
 * Gated: TD5RE_RT_SCENERY (default 1), TD5RE_RT_BILLBOARDS (default 1). */
static void rt_feed_world_scenery(void)
{
    static int s_scenery_on = -1, s_bb_on = -1;
    int span_count, sp, solid = 0, bb = 0, dropped = 0, big = 0;
    if (s_scenery_on < 0) s_scenery_on = td5_env_int("TD5RE_RT_SCENERY",    1, 0, 1);
    if (s_bb_on      < 0) s_bb_on      = td5_env_int("TD5RE_RT_BILLBOARDS", 1, 0, 1);
    if (!s_scenery_on) return;

    /* Enumerate the MODELS.DAT display-list ENTRIES the renderer's main walk uses
     * (td5_track_get_display_list_entry over [0, ring_entries)) -- these carry
     * the real building/wall/bridge/terrain meshes with resolved vertex pointers.
     * (td5_track_get_display_list(span) instead returns STRIP road blocks whose
     * per-command vertex_data_ptr is a TRUNCATED x64 pointer the renderer itself
     * skips -- not the geometry we want.) ring_entries = (ring+3)>>2 (TD5 span>>2
     * map); iterate a generous bound and let the getter NULL past the table. */
    s_seen_count = 0;
    {
        int ring = td5_track_get_ring_length();
        span_count = (ring > 0) ? ((ring + 3) >> 2) : 0;
        if (span_count <= 0 || span_count > 65536) span_count = 65536;
    }
    for (sp = 0; sp < span_count; sp++) {
        const TD5_SpanDisplayList *dl = td5_track_get_display_list_entry(sp);
        int i;
        if (!dl || !dl->meshes) continue;
        if (rt_seen(dl)) continue;                 /* block already walked */
        for (i = 0; i < (int)dl->count; i++) {
            TD5_MeshHeader *mesh = dl->meshes[i];
            int is_bb;
            if (!mesh || (uintptr_t)mesh < 0x100000u) continue;
            if (mesh->command_count <= 0 || mesh->command_count > 4096) continue;
            if (mesh->total_vertex_count <= 0 || mesh->total_vertex_count > 131072) continue;
            if (!mesh->commands || !mesh->vertices) continue;
            if ((uintptr_t)mesh->commands < 0x10000u || (uintptr_t)mesh->vertices < 0x10000u) continue;
            if (rt_seen(mesh)) continue;
            if (s_scenery_count >= RT_MAX_SCENERY) { dropped++; continue; }
            is_bb = (mesh->texture_page_id == 1 || mesh->texture_page_id == 2);
            if (rt_diag_on() && s_seen_count < 40) {
                const TD5_PrimitiveCmd *c0 = mesh->commands;
                rt_diag("  MESH page=%d cmds=%d nv=%d cmd0(op=%d vp=0x%X tri=%d quad=%d) is_bb=%d",
                        mesh->texture_page_id, mesh->command_count, mesh->total_vertex_count,
                        c0[0].dispatch_type, (unsigned)c0[0].vertex_data_ptr,
                        c0[0].triangle_count, c0[0].quad_count, is_bb);
            }
            if (is_bb) {
                if (s_bb_on && rt_feed_billboard(mesh)) bb++;
            } else if (mesh->total_vertex_count > 65535) {
                big++;                                 /* u16 index limit; skip (rare) */
            } else {
                unsigned char smask = 0xFFu;
                int h = rt_build_scenery_mesh(mesh, rt_scenery_matid(mesh), &smask);
                if (h) { s_scenery_mask[s_scenery_count] = smask;
                         s_scenery_handles[s_scenery_count++] = h; solid++; }
            }
        }
    }
    rt_diag("SCENERY_FEED spans=%d seen=%d solid=%d billboards=%d handles=%d big_skipped=%d dropped=%d",
            span_count, s_seen_count, solid, bb, s_scenery_count, big, dropped);
}

/* ---- per-frame driver ----------------------------------------------------- */

static void rt_build_tlas(void)
{
    static const float identity[12] = { 1,0,0,0, 0,1,0,0, 0,0,1,0 };
    static int s_diag_frames = 0;
    int diag_this = rt_diag_on() && (s_diag_frames++ < 1);
    int i, slot, total;

    td5_plat_rt_scene_begin();
    if (!td5_env_int("TD5RE_RT_ONLYCARS", 0, 0, 1)) {
        /* [ROAD-CAST FIX 2026-08-03] The synthetic per-span road lane quads are
         * fed with InstanceMask 0xFE (bit 0 = "sun-shadow caster" CLEARED): a flat
         * road is a shadow RECEIVER, not an occluder, and these per-span quads
         * sit fractionally off the reconstructed road surface, so as sun-shadow
         * occluders they only produced alternating per-span self-shadow stripes
         * right in front of the camera. They stay visible to reflection/primary
         * rays (0xFF) so the road still reflects. Walls/buildings/props/cars keep
         * the default 0xFF and cast onto the road normally. */
        for (i = 0; i < s_track_chunk_count; i++)
            if (s_track_handles[i]) td5_plat_rt_scene_instance(s_track_handles[i], identity, 0xFEu);
        /* [RT2-P2] Static world scenery + billboards (world-space verts -> identity). */
        for (i = 0; i < s_scenery_count; i++)
            if (s_scenery_handles[i]) td5_plat_rt_scene_instance(s_scenery_handles[i], identity, s_scenery_mask[i]);
    }

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
        /* model->world 3x4 (row-major): [ rotation_matrix | translation/256 ].
         *
         * [SHADOW SYNC 2026-08-03] Match the SUB-TICK-EXTRAPOLATED pose the body
         * mesh is RASTERISED at (td5_render_mesh.c ~2902):
         *   render = world_pos + linear_velocity * g_subTickFraction   (per axis)
         * plus the racer chassis height lift. Feeding the raw world_pos here made
         * the shadow OCCLUDER trail the visible car by velocity*subtick — the car
         * shadow "falling behind then syncing after a few frames" at speed. The
         * same |vy| clamp as the render path guards a cold-spawn garbage vertical
         * speed. Rotation stays the physics attitude (rotation_matrix), unchanged. */
        {
            float frac = g_subTickFraction;
            int   vy_raw = actor->linear_velocity_y;
            float vy_extrap = (vy_raw > 0x40000 || vy_raw < -0x40000)
                                  ? 0.0f : (float)vy_raw * frac;
            float ix = (float)actor->world_pos.x + (float)actor->linear_velocity_x * frac;
            float iy = (float)actor->world_pos.y + vy_extrap;
            float iz = (float)actor->world_pos.z + (float)actor->linear_velocity_z * frac;
            /* Racer chassis lift (g_trackHeightBaseOffset << 8): -36 normally,
             * -18 under replay playback. Traffic (slot >= base) skips it, matching
             * the render path's racer-only gate. */
            if (slot < g_traffic_slot_base) {
                int hbo = td5_input_is_playback_active() ? -18 : -36;
                iy -= (float)(hbo << 8);
            }
            float px = ix * RT_INV256;
            float py = iy * RT_INV256;
            float pz = iz * RT_INV256;
            m[0]=actor->rotation_matrix.m[0]; m[1]=actor->rotation_matrix.m[1]; m[2]=actor->rotation_matrix.m[2];  m[3]=px;
            m[4]=actor->rotation_matrix.m[3]; m[5]=actor->rotation_matrix.m[4]; m[6]=actor->rotation_matrix.m[5];  m[7]=py;
            m[8]=actor->rotation_matrix.m[6]; m[9]=actor->rotation_matrix.m[7]; m[10]=actor->rotation_matrix.m[8]; m[11]=pz;
        }
        /* [CAR SHADOW 2026-08-06] Default: feed cars with the sun-shadow caster
         * bit (0x01) CLEARED (0xFE) so shadow rays (InstanceInclusionMask 0x01)
         * skip them — the soft blob grounds the car instead (no lag/glitch, has
         * wheels, works in the dark). Reflection/GI/primary rays trace 0xFF, so
         * 0xFE still lets cars reflect + occlude GI. TD5RE_RT_CAR_CAST=1 -> 0
         * (wrapper remaps 0 -> 0xFF) restores the old body-BLAS sun-shadow cast. */
        td5_plat_rt_scene_instance(h, m, td5_rt_car_cast_shadow() ? 0u : 0xFEu);
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

    if (!td5_rt_available()) return;

    /* [P3 memory] Gate the whole AS feed on ACTIVE (Quality=HIGH), not merely
     * availability. Plan §1.2 originally fed whenever DXR was present so a
     * LOW<->HIGH switch was instant, but that made every LOW frame pay the full
     * RT cost -- ~270 MB of VB/IB/BLAS pools plus per-track growth (confirmed the
     * sole cause of the selftest degrade-private-bytes growth: DXR-disabled runs
     * are flat). Trade-off: a LOW->HIGH switch now lazily rebuilds the ASes over a
     * frame or two (a brief warm-up during which the deferred passes fall back to
     * the screen-space stack) instead of being instant -- worth 270 MB. */
    if (!td5_rt_active()) {
        if (vp == 0) {
            td5_plat_rt_set_mode(0);
            if (s_track_chunk_count > 0) td5_rt_level_unload();  /* HIGH->LOW: reclaim BLAS/meshes */
        }
        return;
    }

    /* Lazy (re)build: on the first HIGH frame (LOW->HIGH switch or HIGH-at-level-
     * load), or after a device-lost generation bump, (re)feed the track from the
     * still-loaded span table. */
    gen = td5_plat_rt_generation();
    if (gen != s_rt_generation || s_track_chunk_count == 0) {
        td5_rt_level_build();
    }

    /* [RT2-P2] Full-scene scenery feed, once, on the first HIGH frame -- by now
     * InitRace has parsed MODELS.DAT (the level_build hook ran too early). The
     * wrapper chunks the BLAS builds across frames so a big track warms up over
     * a few frames without a TDR. */
    if (vp == 0 && !s_scenery_fed) {
        rt_feed_world_scenery();
        s_scenery_fed = 1;
    }

    /* TLAS is world-space and shared across panes: build once per frame (vp 0). */
    if (vp == 0) {
        rt_build_tlas();
        td5_plat_rt_set_mode(1);   /* HIGH: deferred passes run the RT composite */
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
