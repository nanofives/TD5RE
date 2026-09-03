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
#define RT_MATID_CUTOUT        0x100u   /* mirror of DXR_MATID_CUTOUT (d3d12_dxr.c) */

/* [RT WINDOW 2026-09-03] Windowed, per-entry MERGED world feed.
 *
 * The [RT2-P2] feed walked the WHOLE MODELS.DAT display list once at level load
 * and built one BLAS per scenery mesh, capped at 1900. That cap was sized for
 * Moscow (~1600 meshes). An auto-generated track has ~14000 (seed 20260901:
 * 1493 buildings, 2607 sidewalks, 1859 trees, 1800 terrain skirts, ...), so the
 * feed filled its 1900 slots with the first ~60 entries (~240 spans) and
 * DROPPED 12052 meshes: past span ~240 the ray-traced scene was empty -- no
 * building / tree shadows, nothing to reflect -- and (measured) the town at
 * span 500 ran FASTER with RT than the track start, because there was nothing
 * left to trace against.
 *
 * Now: the scene is a WINDOW of display-list entries around each pane's player
 * (fwd/back span reach below), re-evaluated every frame. Entries entering the
 * window are fed, entries leaving it (plus a hysteresis margin) are freed, at
 * most RT_FEED_PER_FRAME entries per frame (the loading-screen warmup feeds the
 * whole initial window). Each entry becomes ONE merged BLAS -- every scenery
 * mesh of the entry is a separate geometry RANGE (own page + material, own
 * OPAQUE/cutout flag) of that BLAS, split only at the u16 vertex limit -- so the
 * TLAS holds ~100-200 well-built BVHs instead of thousands of tiny overlapping
 * per-mesh instances. The wrapper resolves a hit's range through
 * InstanceID() + GeometryIndex() (SM 6.5) into the per-range GeoRecord.
 *
 * Bounds: 4096 entries covers MODELS_DAT_MAX_ENTRIES (2048); 8 chunk handles
 * per entry covers a 480k-vertex entry (none observed; the densest auto-track
 * town entry is ~2k vertices). */
#define RT_ENTRY_MAX           4096
#define RT_ENTRY_CHUNKS        8
#define RT_WIN_FWD_SPANS_DEF   400     /* == the render walk's max forward reach   */
#define RT_WIN_BACK_SPANS_DEF  120
#define RT_WIN_MARGIN_ENTRIES  8       /* evict only this far OUTSIDE the window   */
#define RT_FEED_PER_FRAME_DEF  2
#define RT_ACC_VERT_CAP        65535   /* u16 indices                              */
#define RT_ACC_IDX_CAP         196608  /* quads: 6 idx per 4 verts -> 1.5x + slack */
#define RT_ACC_RANGE_CAP       2048    /* meshes per chunk                         */

typedef struct {
    const TD5_SpanDisplayList *dl;     /* block fed (NULL = empty entry)           */
    int   owner;                       /* entry holding the chunks (== self, or an
                                        * alias when two entries share one block)  */
    int   handle[RT_ENTRY_CHUNKS];
    int   nchunks;
    int   fed;                         /* 1 = processed (even when 0 chunks)       */
    int   meshes;                      /* diag: meshes merged                      */
} RTEntry;

static int      s_track_handles[RT_MAX_TRACK_CHUNKS];
static int      s_track_chunk_count;
static RTEntry  s_entries[RT_ENTRY_MAX];
static int      s_entry_count;         /* display-list entries of the loaded level */
static int      s_fed_entries, s_fed_chunks, s_fed_meshes;   /* live counts (diag) */
static unsigned s_feed_frames;         /* frames since the level's first feed      */

/* Per-chunk accumulator (one entry's meshes merged; flushed at the u16 limit). */
static TD5_RTVertex   s_acc_verts[RT_ACC_VERT_CAP + 8];
static unsigned short s_acc_idx[RT_ACC_IDX_CAP + 16];
static TD5_RTRange    s_acc_ranges[RT_ACC_RANGE_CAP];
static int            s_acc_nv, s_acc_ni, s_acc_nr;

static void rt_scene_update(int budget);   /* defined below td5_rt_level_build */
static struct { const TD5_MeshHeader *mesh; int handle; } s_actor_cache[RT_ACTOR_CACHE];
static int      s_actor_cache_count;
static unsigned s_rt_generation;         /* last observed Backend_RTGeneration   */
static int      s_debugview = -1;        /* TD5RE_RT_DEBUGVIEW latched           */

static int rt_debugview_enabled(void)
{
    if (s_debugview < 0) s_debugview = td5_env_int("TD5RE_RT_DEBUGVIEW", 0, 0, 2);
    return s_debugview;
}

/* [RT WINDOW] Free every fed entry's chunks and forget the level's entry table.
 * The next rt_scene_update re-feeds the window from scratch (level change,
 * unload, device-lost generation bump). */
static void rt_entries_destroy_all(void)
{
    int e, c;
    for (e = 0; e < s_entry_count && e < RT_ENTRY_MAX; e++) {
        RTEntry *E = &s_entries[e];
        if (E->fed && E->owner == e)
            for (c = 0; c < E->nchunks; c++)
                if (E->handle[c]) td5_plat_rt_mesh_destroy(E->handle[c]);
        memset(E, 0, sizeof(*E));
    }
    s_entry_count = 0;
    s_fed_entries = s_fed_chunks = s_fed_meshes = 0;
    s_feed_frames = 0;
}

static void rt_destroy_meshes(void)
{
    int i;
    for (i = 0; i < s_track_chunk_count; i++)
        if (s_track_handles[i]) td5_plat_rt_mesh_destroy(s_track_handles[i]);
    s_track_chunk_count = 0;
    rt_entries_destroy_all();               /* re-fed by the next scene update */
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
     * first HIGH frame instead (rt_scene_update), by which point MODELS is ready. */

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

/* Material id + CUTOUT flag from a scenery mesh's texture page.
 * [RT WINDOW 2026-09-03] TD5RE_RT_CUTOUT_OPAQUE=1 (dev A/B) feeds alpha-tested
 * pages as OPAQUE geometry: no any-hit shader, canopies/fences occlude as solid
 * quads. Measures what the cutout any-hit costs on a given track. */
static int rt_cutout_opaque(void)
{
    static int v = -1;
    if (v < 0) v = td5_env_int("TD5RE_RT_CUTOUT_OPAQUE", 0, 0, 1);
    return v;
}
static unsigned rt_scenery_matid(const TD5_MeshHeader *mesh)
{
    int page = mesh->texture_page_id;
    unsigned mat = td5_material_id_for_page(page);
    if (td5_asset_get_page_transparency(page) == 1 && !rt_cutout_opaque())
        mat |= RT_MATID_CUTOUT;   /* alpha-test */
    return mat;
}

/* ---- [RT WINDOW] per-entry chunk accumulator ------------------------------ */

static int  s_acc_entry = -1;   /* entry being accumulated (diag)             */

static void rt_acc_reset(void) { s_acc_nv = s_acc_ni = s_acc_nr = 0; }

/* Create the accumulated chunk as one RT mesh handle on the entry. */
static void rt_acc_flush(RTEntry *E)
{
    int h;
    if (s_acc_nv < 3 || s_acc_ni < 3 || s_acc_nr < 1) { rt_acc_reset(); return; }
    if (E->nchunks >= RT_ENTRY_CHUNKS) {
        static int s_warned = 0;
        if (!s_warned) {
            TD5_LOG_W("rt", "RT WINDOW: entry %d exceeds %d chunks -- extra geometry dropped",
                      s_acc_entry, RT_ENTRY_CHUNKS);
            s_warned = 1;
        }
        rt_acc_reset(); return;
    }
    h = td5_plat_rt_mesh_create(s_acc_verts, (unsigned)s_acc_nv, s_acc_idx, (unsigned)s_acc_ni,
                                s_acc_ranges, (unsigned)s_acc_nr);
    if (h) { E->handle[E->nchunks++] = h; s_fed_chunks++; }
    rt_acc_reset();
}

/* Append one solid MODELS.DAT scenery mesh as a geometry range. Commands carry a
 * per-command vertex base in vertex_data_ptr -- resolved here exactly as the
 * renderer does (td5_render_span_display_list) -- so buildings/walls/bridges/
 * terrain extract correctly. Vertices are already WORLD space -> identity
 * instance. Topology is tri_count tris + quad_count quads per command for every
 * opcode (opcode selects render state, not topology). Returns 1 if appended. */
static int rt_acc_add_solid(RTEntry *E, const TD5_MeshHeader *mesh)
{
    int c, need_v = 0, need_i = 0, cursor = 0, first_idx, vbase0;
    const TD5_MeshVertex   *base = mesh->vertices;
    const TD5_PrimitiveCmd *cmds = mesh->commands;
    unsigned matid;

    if (!cmds || mesh->command_count <= 0) return 0;
    for (c = 0; c < mesh->command_count; c++) {
        need_v += (int)cmds[c].triangle_count * 3 + (int)cmds[c].quad_count * 4;
        need_i += (int)cmds[c].triangle_count * 3 + (int)cmds[c].quad_count * 6;
    }
    if (need_v < 3 || need_i < 3) return 0;
    if (need_v > RT_ACC_VERT_CAP || need_i > RT_ACC_IDX_CAP) return 0;   /* u16 limit: skip (rare) */
    /* Does not fit in the open chunk -> flush and start a new one. */
    if (s_acc_nv + need_v > RT_ACC_VERT_CAP || s_acc_ni + need_i > RT_ACC_IDX_CAP ||
        s_acc_nr >= RT_ACC_RANGE_CAP)
        rt_acc_flush(E);

    matid = rt_scenery_matid(mesh);
    first_idx = s_acc_ni;
    vbase0 = s_acc_nv;
    for (c = 0; c < mesh->command_count; c++) {
        const TD5_PrimitiveCmd *cmd = &cmds[c];
        int tris = cmd->triangle_count, quads = cmd->quad_count;
        int need = tris * 3 + quads * 4, vbase, qb, t, q, j;
        const TD5_MeshVertex *cv;
        if (need <= 0) continue;
        /* Resolve this command's vertex base EXACTLY as the renderer does: a
         * small value is a byte offset from the mesh header; a large value is an
         * absolute pointer ONLY IF it lands in the models blob. vertex_data_ptr
         * is a uint32_t (on-disk record), so on x64 a stray non-zero value is a
         * TRUNCATED pointer -- blob-bounds-check before any deref. */
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
            if (!base) continue;
            if (cursor + need > mesh->total_vertex_count) break;
            cv = base + cursor; cursor += need;
        }
        if (s_acc_nv + need > RT_ACC_VERT_CAP || s_acc_ni + tris * 3 + quads * 6 > RT_ACC_IDX_CAP) break;
        vbase = s_acc_nv;
        for (j = 0; j < need; j++) {
            TD5_RTVertex *o = &s_acc_verts[s_acc_nv++];
            o->pos[0] = cv[j].pos_x; o->pos[1] = cv[j].pos_y; o->pos[2] = cv[j].pos_z;
            o->uv[0] = cv[j].tex_u;  o->uv[1] = cv[j].tex_v;
            o->color = cv[j].lighting;
        }
        for (t = 0; t < tris; t++) {
            int a = vbase + t * 3;
            s_acc_idx[s_acc_ni++] = (unsigned short)(a + 0);
            s_acc_idx[s_acc_ni++] = (unsigned short)(a + 1);
            s_acc_idx[s_acc_ni++] = (unsigned short)(a + 2);
        }
        qb = vbase + tris * 3;
        for (q = 0; q < quads; q++) {
            int a = qb + q * 4;
            s_acc_idx[s_acc_ni++] = (unsigned short)(a + 0); s_acc_idx[s_acc_ni++] = (unsigned short)(a + 1); s_acc_idx[s_acc_ni++] = (unsigned short)(a + 2);
            s_acc_idx[s_acc_ni++] = (unsigned short)(a + 0); s_acc_idx[s_acc_ni++] = (unsigned short)(a + 2); s_acc_idx[s_acc_ni++] = (unsigned short)(a + 3);
        }
    }
    if (s_acc_ni - first_idx < 3) { s_acc_nv = vbase0; s_acc_ni = first_idx; return 0; }
    {
        TD5_RTRange *r = &s_acc_ranges[s_acc_nr++];
        r->first_index = (unsigned)first_idx;
        r->index_count = (unsigned)(s_acc_ni - first_idx);
        r->texture_id  = (unsigned)mesh->texture_page_id;
        r->matid_flags = matid;
    }
    return 1;
}

/* Append one billboard-tag mesh (header page 1/2) as a static CUTOUT crossed-
 * quad pair at its world bounding centre, sized by its bounding radius. Static
 * (NOT camera-facing) so the shadow is stable under camera motion. Real texture
 * page comes from the mesh's first command (the header id is just the billboard
 * tag). +Y is DOWN in world, so the canopy top is at cy - h (smaller Y). */
static int rt_acc_add_billboard(RTEntry *E, const TD5_MeshHeader *mesh)
{
    static const unsigned short k_idx[12] = { 0,1,2, 0,2,3, 4,5,6, 4,6,7 };
    float cx = mesh->bounding_center_x, cy = mesh->bounding_center_y, cz = mesh->bounding_center_z;
    float r  = mesh->bounding_radius;
    int page, i, vb;
    TD5_RTVertex *v;
    TD5_RTRange *rg;

    if (r != r || r <= 1.0f) return 0;
    if (cx != cx || cy != cy || cz != cz) return 0;
    page = (mesh->command_count > 0 && mesh->commands) ? mesh->commands[0].texture_page_id : 0;
    if (page <= 2) return 0;                 /* no real page id to alpha-test against */
    if (s_acc_nv + 8 > RT_ACC_VERT_CAP || s_acc_ni + 12 > RT_ACC_IDX_CAP || s_acc_nr >= RT_ACC_RANGE_CAP)
        rt_acc_flush(E);

    vb = s_acc_nv;
    v = &s_acc_verts[vb];
    {
        float e = r * 0.7071f;               /* half-extent: quad diagonal ~ radius   */
        /* Quad A: XY plane at z=cz (faces +/-Z). u across X, v down Y (top=cy-e). */
        v[0].pos[0]=cx-e; v[0].pos[1]=cy-e; v[0].pos[2]=cz;  v[0].uv[0]=0; v[0].uv[1]=0;
        v[1].pos[0]=cx-e; v[1].pos[1]=cy+e; v[1].pos[2]=cz;  v[1].uv[0]=0; v[1].uv[1]=1;
        v[2].pos[0]=cx+e; v[2].pos[1]=cy+e; v[2].pos[2]=cz;  v[2].uv[0]=1; v[2].uv[1]=1;
        v[3].pos[0]=cx+e; v[3].pos[1]=cy-e; v[3].pos[2]=cz;  v[3].uv[0]=1; v[3].uv[1]=0;
        /* Quad B: YZ plane at x=cx (faces +/-X). u across Z, v down Y. */
        v[4].pos[0]=cx; v[4].pos[1]=cy-e; v[4].pos[2]=cz-e;  v[4].uv[0]=0; v[4].uv[1]=0;
        v[5].pos[0]=cx; v[5].pos[1]=cy+e; v[5].pos[2]=cz-e;  v[5].uv[0]=0; v[5].uv[1]=1;
        v[6].pos[0]=cx; v[6].pos[1]=cy+e; v[6].pos[2]=cz+e;  v[6].uv[0]=1; v[6].uv[1]=1;
        v[7].pos[0]=cx; v[7].pos[1]=cy-e; v[7].pos[2]=cz+e;  v[7].uv[0]=1; v[7].uv[1]=0;
        for (i = 0; i < 8; i++) v[i].color = 0xFF808080u;   /* mid-grey; tex modulates */
    }
    rg = &s_acc_ranges[s_acc_nr++];
    rg->first_index = (unsigned)s_acc_ni;
    rg->index_count = 12;
    rg->texture_id  = (unsigned)page;
    rg->matid_flags = td5_material_id_for_page(page) | (rt_cutout_opaque() ? 0u : RT_MATID_CUTOUT);
    for (i = 0; i < 12; i++) s_acc_idx[s_acc_ni++] = (unsigned short)(vb + k_idx[i]);
    s_acc_nv += 8;
    return 1;
}

/* ---- [RT WINDOW] entry feed / evict --------------------------------------- */

static int rt_scenery_on(void)   { static int v = -1; if (v < 0) v = td5_env_int("TD5RE_RT_SCENERY",    1, 0, 1); return v; }
static int rt_billboards_on(void){ static int v = -1; if (v < 0) v = td5_env_int("TD5RE_RT_BILLBOARDS", 1, 0, 1); return v; }

/* Feed entry e: merge every valid scenery mesh of its block into chunk BLASes.
 * A block shared with an already-fed entry (TD6 tracks reuse blocks) is not
 * duplicated: the new entry aliases the owner. */
static void rt_entry_feed(int e)
{
    RTEntry *E = &s_entries[e];
    const TD5_SpanDisplayList *dl;
    int i, x;

    memset(E, 0, sizeof(*E));
    E->fed = 1; E->owner = e;
    s_fed_entries++;
    if (!rt_scenery_on()) return;
    dl = td5_track_get_display_list_entry(e);
    E->dl = dl;
    if (!dl || !dl->meshes) return;
    for (x = 0; x < s_entry_count; x++) {
        if (x == e || !s_entries[x].fed || s_entries[x].dl != dl) continue;
        E->owner = s_entries[x].owner;          /* alias the block's owner */
        return;
    }

    rt_acc_reset();
    s_acc_entry = e;
    for (i = 0; i < (int)dl->count; i++) {
        TD5_MeshHeader *mesh = dl->meshes[i];
        int is_bb;
        if (!mesh || (uintptr_t)mesh < 0x100000u) continue;
        if (mesh->command_count <= 0 || mesh->command_count > 4096) continue;
        if (mesh->total_vertex_count <= 0 || mesh->total_vertex_count > 131072) continue;
        if (!mesh->commands || !mesh->vertices) continue;
        if ((uintptr_t)mesh->commands < 0x10000u || (uintptr_t)mesh->vertices < 0x10000u) continue;
        is_bb = (mesh->texture_page_id == 1 || mesh->texture_page_id == 2);
        if (is_bb) {
            if (rt_billboards_on() && rt_acc_add_billboard(E, mesh)) { E->meshes++; s_fed_meshes++; }
        } else if (rt_acc_add_solid(E, mesh)) {
            E->meshes++; s_fed_meshes++;
        }
    }
    rt_acc_flush(E);
    s_acc_entry = -1;
}

static void rt_entry_evict(int e)
{
    RTEntry *E = &s_entries[e];
    int c, x, heir = -1;
    if (!E->fed) return;
    s_fed_entries--;
    if (E->owner == e && E->nchunks > 0) {
        /* Another fed entry aliasing this block inherits the chunks. */
        for (x = 0; x < s_entry_count; x++)
            if (x != e && s_entries[x].fed && s_entries[x].owner == e) { heir = x; break; }
        if (heir >= 0) {
            RTEntry *H = &s_entries[heir];
            memcpy(H->handle, E->handle, sizeof(H->handle));
            H->nchunks = E->nchunks; H->meshes = E->meshes; H->owner = heir;
            for (x = 0; x < s_entry_count; x++)
                if (s_entries[x].fed && s_entries[x].owner == e) s_entries[x].owner = heir;
        } else {
            for (c = 0; c < E->nchunks; c++)
                if (E->handle[c]) { td5_plat_rt_mesh_destroy(E->handle[c]); s_fed_chunks--; }
            s_fed_meshes -= E->meshes;
        }
    }
    memset(E, 0, sizeof(*E));
}

/* ---- [RT WINDOW] the window ----------------------------------------------- */

typedef struct { int lo, hi, center; } RTWin;   /* entry indices; may run outside [0,n) */

static int rt_win_fwd(void)  { static int v = -1; if (v < 0) v = td5_env_int("TD5RE_RT_WIN_FWD",  RT_WIN_FWD_SPANS_DEF,  16, 4096); return v; }
static int rt_win_back(void) { static int v = -1; if (v < 0) v = td5_env_int("TD5RE_RT_WIN_BACK", RT_WIN_BACK_SPANS_DEF, 16, 4096); return v; }
static int rt_feed_budget(void){ static int v = -1; if (v < 0) v = td5_env_int("TD5RE_RT_FEED_PER_FRAME", RT_FEED_PER_FRAME_DEF, 1, 64); return v; }

/* Same span->entry mapping as the render walk (td5_render_actors_for_view):
 * faithful TD5 layout = span>>2; TD6 = proportional over the real entry count.
 * Centre = the pane's player on its NORMALISED ring span (branch spans fold
 * onto the main ring), mirrored in reverse like every MODELS.DAT consumer. */
static void rt_view_window(int vp, int n, int ring, RTWin *w)
{
    TD5_Actor *a = td5_game_get_actor(td5_game_get_player_slot(vp));
    int span = a ? (int)a->track_span_normalized : 0;
    int fwd = rt_win_fwd(), back = rt_win_back(), lo_s, hi_s;
    if (ring > 0) { if (span < 0) span = 0; else if (span >= ring) span = ring - 1; }
    if (g_td5.reverse_direction && ring > 0) {
        span = ring - 1 - span;
        lo_s = span - fwd; hi_s = span + back;     /* travel = decreasing entry index */
    } else {
        lo_s = span - back; hi_s = span + fwd;
    }
    if (g_active_td6_level > 0 && ring > 0) {
        long long re = (long long)n;
        w->lo = (int)(((long long)lo_s * re) / ring);
        w->hi = (int)((((long long)hi_s * re) + (ring - 1)) / ring);
        w->center = (int)(((long long)span * re) / ring);
    } else {
        w->lo = lo_s >> 2; w->hi = hi_s >> 2; w->center = span >> 2;
    }
    if (w->hi < w->lo) w->hi = w->lo;
}

/* Is entry e inside window w widened by `margin` entries? Circuits wrap. */
static int rt_win_contains(int e, const RTWin *w, int margin, int n, int circuit)
{
    int lo = w->lo - margin, hi = w->hi + margin, d;
    if (!circuit) return e >= lo && e <= hi;
    if (hi - lo >= n - 1) return 1;
    d = (e - lo) % n; if (d < 0) d += n;
    return d <= hi - lo;
}

/* Re-evaluate the window(s) and stream entries in/out. `budget` = max entries
 * fed this call (the warmup passes RT_ENTRY_MAX to fill the whole window). */
static void rt_scene_update(int budget)
{
    int n = td5_track_get_models_display_list_count();
    int ring = td5_track_get_ring_length();
    int circuit = (g_td5.track_type == TD5_TRACK_CIRCUIT) ? 1 : 0;
    int views = (g_td5.viewport_count > 0) ? g_td5.viewport_count : 1;
    RTWin w[TD5_MAX_VIEWPORTS];
    int v, e, k, reach = 0, fed_now = 0;

    if (n <= 0 || n > RT_ENTRY_MAX) { if (s_entry_count) rt_entries_destroy_all(); return; }
    if (s_entry_count != n) { rt_entries_destroy_all(); s_entry_count = n; }
    if (views > TD5_MAX_VIEWPORTS) views = TD5_MAX_VIEWPORTS;
    for (v = 0; v < views; v++) {
        rt_view_window(v, n, ring, &w[v]);
        if (w[v].hi - w[v].lo > reach) reach = w[v].hi - w[v].lo;
    }
    s_feed_frames++;

    /* Evict everything outside every window (+ hysteresis margin). */
    for (e = 0; e < n; e++) {
        int keep = 0;
        if (!s_entries[e].fed) continue;
        for (v = 0; v < views && !keep; v++)
            keep = rt_win_contains(e, &w[v], RT_WIN_MARGIN_ENTRIES, n, circuit);
        if (!keep) rt_entry_evict(e);
    }
    /* Feed nearest-first around each pane's player, forward before back. */
    for (k = 0; k <= reach && fed_now < budget; k++) {
        for (v = 0; v < views && fed_now < budget; v++) {
            int s;
            for (s = 0; s < 2 && fed_now < budget; s++) {
                int c = w[v].center + (s == 0 ? k : -k);
                if (k == 0 && s == 1) continue;
                if (circuit) { c %= n; if (c < 0) c += n; }
                else if (c < 0 || c >= n) continue;
                if (!rt_win_contains(c, &w[v], 0, n, circuit)) continue;
                if (s_entries[c].fed) continue;
                rt_entry_feed(c);
                fed_now++;
            }
        }
    }

    if (rt_diag_on() && (fed_now || (s_feed_frames % 300u) == 1u)) {
        unsigned vbu = 0, vbc = 0, ibu = 0, ibc = 0, nm = 0;
        td5_plat_rt_pool_stats(&vbu, &vbc, &ibu, &ibc, &nm);
        rt_diag("SCENE_WINDOW frame=%u entries=%d win0=[%d..%d] c=%d fed=%d chunks=%d meshes=%d fed_now=%d pool vb=%u/%uMB ib=%u/%uMB meshes=%u",
                s_feed_frames, n, w[0].lo, w[0].hi, w[0].center, s_fed_entries, s_fed_chunks, s_fed_meshes,
                fed_now, vbu >> 20, vbc >> 20, ibu >> 20, ibc >> 20, nm);
    }
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
        /* [RT WINDOW] Merged per-entry scenery chunks of the live window
         * (world-space verts -> identity instance, full caster mask). */
        for (i = 0; i < s_entry_count; i++) {
            const RTEntry *E = &s_entries[i];
            int c;
            if (!E->fed || E->owner != i) continue;
            for (c = 0; c < E->nchunks; c++)
                if (E->handle[c]) td5_plat_rt_scene_instance(E->handle[c], identity, 0xFFu);
        }
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

int td5_rt_warmup_prepare(void)
{
    /* [RT WARMUP 2026-08-08] Called at the END of InitRace (MODELS.DAT fully
     * parsed, actors spawned) to move the first-HIGH-frame RT cost onto the
     * loading screen. Normally the ~1600-mesh scenery feed + the resulting BLAS
     * build wave happen on the first race frame (see td5_rt_frame: the level_build
     * hook runs too early, before MODELS is ready), which stacked with the first-
     * DispatchRays driver compile and TDR'd slower GPUs (RTX 3070 froze after one
     * non-RT frame). Do that feed NOW so td5_rt_frame's lazy feed is a no-op, and
     * report that a warmup pump is needed so the caller can drain the BLAS wave in
     * watchdog-safe per-frame chunks while the loading splash is shown.
     * Returns 1 when RT is HIGH-active (caller should run the warmup pump), else 0
     * (LOW / unavailable -> nothing to warm). */
    if (!td5_rt_active()) return 0;
    /* Track lane quads: normally built at the MODELS.DAT-parse hook; ensure they
     * exist here in case that hook ran before RT went active. */
    if (s_track_chunk_count == 0)
        td5_rt_level_build();
    /* [RT WINDOW] Feed the whole initial window now (unbounded budget) so the
     * loading-screen pump drains its BLAS wave before the first race frame. */
    rt_scene_update(RT_ENTRY_MAX);
    return 1;
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
    /* [RT WINDOW] Stream the scenery window: evict entries the players left
     * behind, feed the ones coming into reach (a few per frame; the wrapper
     * chunks their BLAS builds by tri budget). Also the lazy first feed after a
     * LOW->HIGH switch or device-lost rebuild (the entry table is empty then). */
    if (vp == 0)
        rt_scene_update(rt_feed_budget());

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
