/**
 * td5_roam_physics.c -- GEO FREE ROAM: the contact provider + the resident set
 * (PORT-ONLY, F2 task B). Contract and rules: td5_roam_physics.h.
 * Design: docs/plans/GEO_FREE_ROAM_DESIGN.md 2.1 - 2.6, 0 item 7.
 *
 * Every answer comes from td5_roam_locate, whose heights are the strip's own integer
 * code (td5_track_quad_height_v / td5_track_tri_height_v) on the ROAMTILE vertices,
 * so on an identical road the provider returns the strip's numbers. The per-slot
 * side table keeps the previous hit of each probe as the locator hint (continuity
 * across overlapping streets and bridge decks) and the answers td5_track.c's
 * surface query reads back.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "td5_platform.h"
#include "td5_config.h"
#include "td5_fp.h"
#include "td5_geo.h"
#include "td5re.h"
#include "td5_types.h"
#include "td5_track.h"
#include "td5_physics.h"
#include "td5_roam_tile.h"
#include "td5_roam_locate.h"
#include "td5_roam_mode.h"
#include "td5_roam_physics.h"
#include "td5_geo_roads.h"      /* TD5_GEO_RC_* (dev corner matrix) */
#include "../../../re/include/td5_actor_struct.h"

#define LOG_TAG "roam"

#define RP_SLOTS        32          /* == TD5_MAX_TOTAL_ACTORS                       */
#define RP_RACERS       6           /* walls + backstop: racer slots only (as the strip) */
#define RP_PUSH_CAP     600         /* units per tick, the strip's GEO_PUSH_CAP      */
#define RP_RESCUE_IN    130         /* units (0.3 m) inside the boundary after a rescue */
#define RP_FALL_DEPTH   (860 * 256) /* 24.8 (2 m): chassis this far under its ground fell through */
#define RP_NOHIT_DROP   0x10000     /* 24.8: a wheel over no tile reads ground this far below it */
#define RP_WALL_LOG_ALL 200         /* first N wall hits logged one per line, then every 50th */
#define RP_MAX_TILES    1024
#define RP_MIRROR_CELL  (ROAM_GRID_CELL * 2)

typedef struct {
    uint32_t    hint[8];        /* 0..3 body corners, 4..7 wheels: previous prim  */
    TD5_RoamHit probe[8];       /* the last answer per probe (surface query)      */
    uint8_t     probe_ok[8];
    uint32_t    chassis_hint;
    TD5_RoamHit chassis;
    int         chassis_ok;
    int32_t     good_x, good_y, good_z;   /* last tick on the road with a wheel down */
    int         good_valid;
    int         settled;        /* ticks on the road with a wheel down since the last spawn / teleport */
} RoamSlotState;

typedef struct {
    int                 loaded, load_failed;
    char                place[64];
    TD5_RoamResident   *rs;
    RoamTile           *tiles;
    int                 ntile;
    int32_t             org_x, org_z;
    int                 mirror_knob;        /* -1 = not read yet                      */
    int                 mirror_ready;
    const void         *mirror_src;         /* strip span table the mirror was built from */
    int                 mirror_nspan;
    RoamTile            mtile;
    TD5_RoamResident   *mrs;
    uint32_t           *mgrid;              /* start[] + item[] of the mirror tile    */
    RoamSlotState       slot[RP_SLOTS];
    int                 last_capped;
    TD5_RoamPhysStats   st;
    long                summary_tick, last_sim_tick;
#ifndef TD5RE_RELEASE
    struct RoamDrive *drv;                  /* DEV drive harness state (heap)         */
    int               drv_mode;             /* -1 unread, 0 off, 1 street, 2 corners  */
#endif
} RoamPhys;

static RoamPhys s_rp = { .mirror_knob = -1
#ifndef TD5RE_RELEASE
    , .drv_mode = -1
#endif
};

/* ============================================================================
 * SECTION: small helpers
 * ========================================================================== */

static RoamSlotState *rp_slot(const TD5_Actor *a)
{
    int s = a ? (int)a->slot_index : 0;
    if (s < 0 || s >= RP_SLOTS) s = 0;
    return &s_rp.slot[s];
}

static const TD5_RoamResident *rp_rs(void)
{
    if (td5_roam_active()) return s_rp.loaded ? s_rp.rs : NULL;
    return s_rp.mirror_ready ? s_rp.mrs : NULL;
}

/* TD5 surface code of a hit: the strip's surface_type_for_span_lane on a ribbon quad
 * (the locator's own `surface` field drops the 0x10 lane-mask bit), the patch nibble
 * on a junction triangle. */
static int rp_surface(const TD5_RoamResident *rs, const TD5_RoamHit *h)
{
    const RoamTile *t;
    uint32_t slot, idx;
    if (!rs || !h || h->prim == ROAM_PRIM_NONE) return TD5_SURFACE_DRY_ASPHALT;
    slot = ROAM_PRIM_SLOT(h->prim); idx = ROAM_PRIM_INDEX(h->prim);
    if (slot >= (uint32_t)rs->nslot || !(t = rs->tile[slot])) return TD5_SURFACE_DRY_ASPHALT;
    if (ROAM_PRIM_IS_TRI(h->prim)) {
        if (idx >= t->ntri) return TD5_SURFACE_DRY_ASPHALT;
        return t->tri[idx].surface & 0x0F;
    }
    if (idx >= t->nspan) return TD5_SURFACE_DRY_ASPHALT;
    {
        const TD5_StripSpan *sp = &t->span[idx];
        const int lane = h->lane < 0 ? 0 : h->lane;
        if (sp->pad_02[0] & (1u << (lane & 7))) return (sp->surface_attribute >> 4) | 0x10;
        return sp->surface_attribute & 0x0F;
    }
}

static int32_t rp_iround(double v) { return (int32_t)floor(v + 0.5); }

/* The physics sign of a locator normal. MEASURED (mirror, Moscow span 111, flat): the
 * strip's own contact normal is (0,-4096,0), i.e. triangle_height's raw output on strip
 * winding has ny < 0, and every physics consumer (suspension gravity projection, the
 * heading-normal slope force) is written for that sign. td5_roam_locate returns the
 * flipped ("up", ny > 0) normal (normal_up, on the T6b premise that the strip gives
 * ny > 0), so the provider flips it back. ROAMTILE ribbons and patches come out of
 * triangle_height with ny < 0 (T6b), the strip's sign, so this restores the raw value. */
static void rp_phys_normal(const int16_t in[3], int16_t out[3])
{
    if (in[1] > 0) { out[0] = (int16_t)-in[0]; out[1] = (int16_t)-in[1]; out[2] = (int16_t)-in[2]; }
    else           { out[0] = in[0]; out[1] = in[1]; out[2] = in[2]; }
}

/* ============================================================================
 * SECTION: resident set (the session)
 * ========================================================================== */

static int rp_read_grid(const char *place, RoamGrid *g)
{
    char path[512], txt[4096];
    size_t nr;
    FILE *f;
    td5_geo_source_path(path, sizeof path, place, "roam/ROAMGRID.JSON");
    f = fopen(path, "rb");
    if (!f) return 0;
    nr = fread(txt, 1, sizeof txt - 1, f);
    fclose(f);
    txt[nr] = 0;
    return roam_grid_parse(txt, g);
}

int td5_roam_physics_session_begin(void)
{
    const TD5_RoamSpawn *sp = td5_roam_spawn();
    RoamGrid g;
    int (*list)[2] = NULL;
    int nlist, i, missing = 0;
    char path[512], err[160];
    uint32_t t0 = td5_plat_time_ms();
    double ext = 0.0;

    if (s_rp.loaded) return 1;
    snprintf(s_rp.place, sizeof s_rp.place, "%s", (sp && sp->place[0]) ? sp->place : "la_plata_partido");
    if (!rp_read_grid(s_rp.place, &g)) {
        TD5_LOG_E(LOG_TAG, "roam physics: no roam/ROAMGRID.JSON for '%s' (run --RoamBuildTiles=casco)", s_rp.place);
        s_rp.load_failed = 1;
        return 0;
    }
    list = (int (*)[2])malloc(2048 * sizeof(int[2]));
    s_rp.tiles = (RoamTile *)calloc(RP_MAX_TILES, sizeof(RoamTile));
    s_rp.rs = (TD5_RoamResident *)calloc(1, sizeof(TD5_RoamResident));
    if (!list || !s_rp.tiles || !s_rp.rs) {
        TD5_LOG_E(LOG_TAG, "roam physics: out of memory for the resident set");
        free(list);
        td5_roam_physics_session_end();
        s_rp.load_failed = 1;
        return 0;
    }
    td5_roam_world_origin(&s_rp.org_x, &s_rp.org_z);
    td5_roam_resident_init(s_rp.rs, &g, s_rp.org_x, s_rp.org_z, 0);
    nlist = td5_roam_casco_tiles(&g, list, 2048);
    for (i = 0; i < nlist && s_rp.ntile < RP_MAX_TILES; i++) {
        RoamTile *t = &s_rp.tiles[s_rp.ntile];
        td5_roam_tile_path(path, sizeof path, NULL, s_rp.place, list[i][0], list[i][1]);
        if (!td5_roam_tile_read(path, t, 0, err, sizeof err)) {
            if (++missing <= 3) TD5_LOG_W(LOG_TAG, "roam physics: tile %d,%d: %s", list[i][0], list[i][1], err);
            continue;
        }
        if (td5_roam_resident_add(s_rp.rs, t) < 0) { td5_roam_tile_free(t); continue; }
        /* design 0 item 7: every casco point within +-3.2 M units of the origin */
        {
            double c[4][2];
            int k;
            roam_grid_corners(&g, t->tx, t->tz, c);
            for (k = 0; k < 4; k++) {
                const double dx = fabs(c[k][0] - s_rp.org_x), dz = fabs(c[k][1] - s_rp.org_z);
                if (dx > ext) ext = dx;
                if (dz > ext) ext = dz;
            }
        }
        s_rp.ntile++;
    }
    free(list);
    if (s_rp.ntile == 0) {
        TD5_LOG_E(LOG_TAG, "roam physics: no casco ROAMTILE loaded for '%s' (%d listed)", s_rp.place, nlist);
        td5_roam_physics_session_end();
        s_rp.load_failed = 1;
        return 0;
    }
    memset(s_rp.slot, 0, sizeof s_rp.slot);
    for (i = 0; i < RP_SLOTS; i++) {
        int k;
        for (k = 0; k < 8; k++) s_rp.slot[i].hint[k] = ROAM_PRIM_NONE;
        s_rp.slot[i].chassis_hint = ROAM_PRIM_NONE;
    }
    memset(&s_rp.st, 0, sizeof s_rp.st);
    s_rp.summary_tick = 0;
    s_rp.loaded = 1;
    td5_roam_set_resident(s_rp.rs);
    TD5_LOG_I(LOG_TAG, "roam physics: resident set place=%s tiles=%d/%d missing=%d origin=(%d,%d) "
              "max |tile corner - origin| = %.0f units (24.8 limit 8388607) in %u ms",
              s_rp.place, s_rp.ntile, nlist, missing, (int)s_rp.org_x, (int)s_rp.org_z, ext,
              (unsigned)(td5_plat_time_ms() - t0));
    if (ext > 3.2e6)
        TD5_LOG_W(LOG_TAG, "roam physics: a resident tile reaches %.0f units from the origin (> 3.2 M)", ext);
    return 1;
}

static void rp_write_summary(const char *why);

void td5_roam_physics_session_end(void)
{
    int i;
    if (s_rp.loaded) rp_write_summary("session end");
    if (s_rp.rs && td5_roam_resident() == s_rp.rs) td5_roam_set_resident(NULL);
    if (s_rp.tiles) {
        for (i = 0; i < s_rp.ntile; i++) td5_roam_tile_free(&s_rp.tiles[i]);
        free(s_rp.tiles);
    }
    free(s_rp.rs);
    s_rp.tiles = NULL;
    s_rp.rs = NULL;
    s_rp.ntile = 0;
    s_rp.loaded = 0;
    s_rp.load_failed = 0;
}

/* ============================================================================
 * SECTION: the strip mirror (design 2.6 stage A, DEV)
 * ========================================================================== */

static void rp_mirror_free(void)
{
    free(s_rp.mgrid);
    free(s_rp.mrs);
    s_rp.mgrid = NULL;
    s_rp.mrs = NULL;
    memset(&s_rp.mtile, 0, sizeof s_rp.mtile);
    s_rp.mirror_ready = 0;
    s_rp.mirror_src = NULL;
    s_rp.mirror_nspan = 0;
}

static void rp_bb_add(int64_t *x0, int64_t *z0, int64_t *x1, int64_t *z1, int64_t x, int64_t z)
{
    if (x < *x0) *x0 = x;
    if (x > *x1) *x1 = x;
    if (z < *z0) *z0 = z;
    if (z > *z1) *z1 = z;
}

/* One tile = the strip's own span + vertex tables (not copied: same origins, same
 * vertex rows), world frame = strip frame (origin 0), an axis-aligned lattice whose
 * single tile covers every type-1 span, grid cells listing the type-1 spans only.
 * A probe over any other span type finds nothing and keeps the strip's answer. */
static int rp_mirror_build(void)
{
    const int nspan = td5_track_get_span_count();
    TD5_StripSpan *spans = nspan > 0 ? td5_track_get_span(0) : NULL;
    TD5_StripVertex *vt = td5_track_get_vertex(0);
    int64_t minx = INT64_MAX, minz = INT64_MAX, maxx = INT64_MIN, maxz = INT64_MIN;
    int i, n1 = 0, nx, nz, cell = RP_MIRROR_CELL;
    uint32_t *start, *item, total = 0;
    int32_t S;
    RoamGrid g;

    rp_mirror_free();
    if (!spans || !vt || nspan <= 0) return 0;
    for (i = 0; i < nspan; i++) {
        const TD5_StripSpan *sp = &spans[i];
        int vl0, vl1, vr0, vr1, L, k;
        if (sp->span_type != 1) continue;
        L = sp->pad_02[1] & 0x0F; if (L < 1) L = 1;
        td5_track_quad_vertices(sp, 0, &vl0, &vl1, &vr0, &vr1);
        for (k = 0; k <= L; k++) {
            const TD5_StripVertex *a = &vt[vl0 + k], *b = &vt[vr0 + k];
            const int64_t ax = (int64_t)sp->origin_x + a->x, az = (int64_t)sp->origin_z + a->z;
            const int64_t bx = (int64_t)sp->origin_x + b->x, bz = (int64_t)sp->origin_z + b->z;
            rp_bb_add(&minx, &minz, &maxx, &maxz, ax, az);
            rp_bb_add(&minx, &minz, &maxx, &maxz, bx, bz);
        }
        n1++;
    }
    if (n1 == 0) return 0;
    S = (int32_t)((maxx - minx > maxz - minz ? maxx - minx : maxz - minz) + 2 * cell);
    nx = nz = S / cell + 1;
    memset(&g, 0, sizeof g);
    snprintf(g.place, sizeof g.place, "strip-mirror");
    g.ox = (double)(minx - cell); g.oz = (double)(minz - cell);
    g.c = 1.0; g.s = 0.0; g.tile = S;

    /* two passes: count per cell, then fill */
    start = (uint32_t *)calloc((size_t)nx * nz + 1, sizeof(uint32_t));
    if (!start) return 0;
    for (int pass = 0; pass < 2; pass++) {
        uint32_t *fill = NULL;
        if (pass == 1) {
            uint32_t acc = 0;
            for (i = 0; i < nx * nz; i++) { const uint32_t c = start[i]; start[i] = acc; acc += c; }
            start[nx * nz] = acc;
            total = acc;
            s_rp.mgrid = (uint32_t *)malloc(((size_t)nx * nz + 1 + total + 1) * sizeof(uint32_t));
            fill = (uint32_t *)calloc((size_t)nx * nz, sizeof(uint32_t));
            if (!s_rp.mgrid || !fill) { free(start); free(fill); rp_mirror_free(); return 0; }
            memcpy(s_rp.mgrid, start, ((size_t)nx * nz + 1) * sizeof(uint32_t));
        }
        item = s_rp.mgrid ? s_rp.mgrid + nx * nz + 1 : NULL;
        for (i = 0; i < nspan; i++) {
            const TD5_StripSpan *sp = &spans[i];
            int vl0, vl1, vr0, vr1, L, k, cu0, cu1, cv0, cv1, cu, cv;
            int64_t x0 = INT64_MAX, z0 = INT64_MAX, x1 = INT64_MIN, z1 = INT64_MIN;
            if (sp->span_type != 1) continue;
            L = sp->pad_02[1] & 0x0F; if (L < 1) L = 1;
            td5_track_quad_vertices(sp, 0, &vl0, &vl1, &vr0, &vr1);
            for (k = 0; k <= L; k++) {
                const TD5_StripVertex *a = &vt[vl0 + k], *b = &vt[vr0 + k];
                const int64_t ax = (int64_t)sp->origin_x + a->x, az = (int64_t)sp->origin_z + a->z;
                const int64_t bx = (int64_t)sp->origin_x + b->x, bz = (int64_t)sp->origin_z + b->z;
                rp_bb_add(&x0, &z0, &x1, &z1, ax, az);
                rp_bb_add(&x0, &z0, &x1, &z1, bx, bz);
            }
            cu0 = (int)((x0 - 8 - (minx - cell)) / cell); cu1 = (int)((x1 + 8 - (minx - cell)) / cell);
            cv0 = (int)((z0 - 8 - (minz - cell)) / cell); cv1 = (int)((z1 + 8 - (minz - cell)) / cell);
            if (cu0 < 0) cu0 = 0;
            if (cv0 < 0) cv0 = 0;
            if (cu1 > nx - 1) cu1 = nx - 1;
            if (cv1 > nz - 1) cv1 = nz - 1;
            for (cv = cv0; cv <= cv1; cv++)
                for (cu = cu0; cu <= cu1; cu++) {
                    const int cc = cv * nx + cu;
                    if (pass == 0) start[cc]++;
                    else item[s_rp.mgrid[cc] + fill[cc]++] = ((uint32_t)ROAM_ITEM_SPAN << 30) | (uint32_t)i;
                }
        }
        if (pass == 1) free(fill);
    }
    free(start);

    memset(&s_rp.mtile, 0, sizeof s_rp.mtile);
    s_rp.mtile.tx = 0; s_rp.mtile.tz = 0;
    s_rp.mtile.org_x = 0; s_rp.mtile.org_z = 0;
    s_rp.mtile.span = spans; s_rp.mtile.nspan = (uint32_t)nspan;
    s_rp.mtile.vert = vt;
    s_rp.mtile.grid_nx = (uint32_t)nx; s_rp.mtile.grid_nz = (uint32_t)nz;
    s_rp.mtile.grid_cell = (uint32_t)cell; s_rp.mtile.grid_margin = 0;
    s_rp.mtile.grid_start = s_rp.mgrid;
    s_rp.mtile.grid_item = s_rp.mgrid + nx * nz + 1;
    s_rp.mtile.nitem = total;
    s_rp.mrs = (TD5_RoamResident *)calloc(1, sizeof(TD5_RoamResident));
    if (!s_rp.mrs) { rp_mirror_free(); return 0; }
    td5_roam_resident_init(s_rp.mrs, &g, 0, 0, 0);
    td5_roam_resident_add(s_rp.mrs, &s_rp.mtile);
    s_rp.mirror_src = spans;
    s_rp.mirror_nspan = nspan;
    s_rp.mirror_ready = 1;
    memset(&s_rp.st, 0, sizeof s_rp.st);
    s_rp.summary_tick = 0;
    for (i = 0; i < RP_SLOTS; i++) {
        int k;
        memset(&s_rp.slot[i], 0, sizeof s_rp.slot[i]);
        for (k = 0; k < 8; k++) s_rp.slot[i].hint[k] = ROAM_PRIM_NONE;
        s_rp.slot[i].chassis_hint = ROAM_PRIM_NONE;
    }
    TD5_LOG_W(LOG_TAG, "roam physics: STRIP MIRROR built (TD5RE_ROAM_MIRROR=1, dev): %d spans, %d type-1, "
              "grid %dx%d cells of %d, %u items; racer contacts now come from the roam provider",
              nspan, n1, nx, nz, cell, (unsigned)total);
    return 1;
}

static int rp_mirror_knob(void)
{
    if (s_rp.mirror_knob < 0) {
#ifndef TD5RE_RELEASE
        s_rp.mirror_knob = td5_env_flag_off("TD5RE_ROAM_MIRROR");
#else
        s_rp.mirror_knob = 0;
#endif
    }
    return s_rp.mirror_knob;
}

int td5_roam_physics_mirror(void)
{
    return s_rp.mirror_ready && !td5_roam_active();
}

int td5_roam_physics_live(void)
{
    if (td5_roam_active()) {
        if (!s_rp.loaded && !s_rp.load_failed) td5_roam_physics_session_begin();
        return s_rp.loaded;
    }
    if (s_rp.loaded || s_rp.load_failed) td5_roam_physics_session_end();
    if (!rp_mirror_knob()) return 0;
    {
        const int n = td5_track_get_span_count();
        const void *src = n > 0 ? (const void *)td5_track_get_span(0) : NULL;
        if (!src) { if (s_rp.mirror_ready) rp_mirror_free(); return 0; }
        if (!s_rp.mirror_ready || src != s_rp.mirror_src || n != s_rp.mirror_nspan) {
            if (s_rp.mirror_ready) rp_write_summary("strip changed");
            rp_mirror_build();
        }
    }
    return s_rp.mirror_ready;
}

/* ============================================================================
 * SECTION: summary
 * ========================================================================== */

static void rp_write_summary(const char *why)
{
    const TD5_RoamPhysStats *s = &s_rp.st;
    const char *dir = td5_plat_log_dir();
    char path[512];
    FILE *f;
    TD5_LOG_I(LOG_TAG, "roam physics summary (%s): ticks=%ld wall_hits=%ld pinches=%ld rescues=%ld rescues_y=%ld "
              "teleports=%ld nohit_wheels=%ld outside_wheels=%ld | mirror cmp=%ld dh=%ld dn=%ld dcap=%ld "
              "dsurf=%ld/%ld fallback=%ld corner %ld/%ld (fb %ld) head %ld/%ld (fb %ld) max|dh|=%ld",
              why, s->ticks, s->wall_hits, s->pinches, s->rescues, s->rescues_y, s->teleports,
              s->nohit_wheels, s->outside_wheels, s->mirror_cmp, s->mirror_diff_h, s->mirror_diff_n,
              s->mirror_diff_cap, s->mirror_diff_surf, s->mirror_scmp, s->mirror_fallback,
              s->mirror_corner_diff, s->mirror_corner_cmp, s->mirror_corner_fallback,
              s->mirror_head_diff, s->mirror_head_cmp, s->mirror_head_fallback, s->mirror_dh_max);
    snprintf(path, sizeof path, "%sroam_phys.txt", dir ? dir : "");
    f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "roam physics summary (%s)\nmode=%s\n", why, td5_roam_active() ? "roam" : "mirror");
    fprintf(f, "ticks=%ld\nwall_hits=%ld\npinches=%ld\nrescues=%ld\nrescues_y=%ld\nteleports=%ld\n"
               "nohit_wheels=%ld\noutside_wheels=%ld\nwall_corner_pen_max=%ld\n",
            s->ticks, s->wall_hits, s->pinches, s->rescues, s->rescues_y, s->teleports,
            s->nohit_wheels, s->outside_wheels, s->wall_corner_pen_max);
    fprintf(f, "mirror_wheel_cmp=%ld\nmirror_wheel_diff_h=%ld\nmirror_wheel_diff_n=%ld\nmirror_wheel_diff_cap=%ld\n"
               "mirror_wheel_surf_cmp=%ld\nmirror_wheel_diff_surf=%ld\nmirror_wheel_fallback=%ld\nmirror_dh_max=%ld\n"
               "mirror_corner_cmp=%ld\nmirror_corner_diff=%ld\nmirror_corner_fallback=%ld\n"
               "mirror_head_cmp=%ld\nmirror_head_diff=%ld\nmirror_head_fallback=%ld\n",
            s->mirror_cmp, s->mirror_diff_h, s->mirror_diff_n, s->mirror_diff_cap, s->mirror_scmp,
            s->mirror_diff_surf, s->mirror_fallback, s->mirror_dh_max, s->mirror_corner_cmp,
            s->mirror_corner_diff, s->mirror_corner_fallback, s->mirror_head_cmp, s->mirror_head_diff,
            s->mirror_head_fallback);
    fprintf(f, "mirror_wheel_diff_walker_lag=%ld\nmirror_wheel_diff_same_span=%ld\n"
               "mirror_corner_diff_walker_lag=%ld\nmirror_corner_diff_same_span=%ld\n",
            s->mirror_diff_walker, s->mirror_diff_same, s->mirror_corner_walker, s->mirror_corner_same);
    fprintf(f, "wheel_gap_samples=%ld\nwheel_gap_min_cm=%.2f\nrest_samples=%ld\nrest_gap_min_cm=%.2f\n"
               "rest_gap_max_cm=%.2f\nrest_gap_mean_cm=%.2f\nchassis_min_above_ground_cm=%.2f\n",
            s->gap_n, s->gap_min / 256.0 / 4.30, s->rest_n, s->rest_min / 256.0 / 4.30,
            s->rest_max / 256.0 / 4.30, s->rest_n ? (double)s->rest_sum / s->rest_n / 256.0 / 4.30 : 0.0,
            s->chassis_min_above / 256.0 / 4.30);
    fclose(f);
}

void td5_roam_physics_stats(TD5_RoamPhysStats *out)
{
    if (out) *out = s_rp.st;
}

/* ============================================================================
 * SECTION: wheels + body corners (td5_physics_refresh_wheel_contacts)
 * ========================================================================== */

int td5_roam_wheel_contact(TD5_Actor *a, int i, int32_t x, int32_t z, int32_t y,
                           int32_t *ground_y, int16_t normal[3], int *surface, int *capped)
{
    const TD5_RoamResident *rs = rp_rs();
    RoamSlotState *ss;
    TD5_RoamHit h;
    int ok, surf;
    if (!rs || !a || i < 0 || i > 3) return 0;
    ss = rp_slot(a);
    if (i == 0 && a->slot_index == 0 && (long)g_td5.simulation_tick_counter != s_rp.last_sim_tick) {
        s_rp.last_sim_tick = (long)g_td5.simulation_tick_counter;   /* sim ticks, not refresh calls */
        s_rp.st.ticks++;
        if (++s_rp.summary_tick % 300 == 0) rp_write_summary("periodic");
    }
    ok = td5_roam_locate(rs, x, z, y, 1, ss->hint[4 + i], &h);
    rp_phys_normal(h.normal, h.normal);

    if (!td5_roam_active()) {
        /* MIRROR: only a probe on a type-1 span the strip would also be on is compared */
        int dh;
        if (!ok || h.prim == ROAM_PRIM_NONE || !h.inside) { s_rp.st.mirror_fallback++; return 0; }
        surf = rp_surface(rs, &h);
        s_rp.st.mirror_cmp++;
        dh = abs(h.ground_y - *ground_y);
        if (dh) { s_rp.st.mirror_diff_h++; if (dh > s_rp.st.mirror_dh_max) s_rp.st.mirror_dh_max = dh; }
        /* attribution: the mirror tile IS the strip's span table, so a differing answer
         * on a different span is the strip walker's lag (design 2.2 a); on the same span
         * it would be a provider bug */
        if (dh || h.normal[0] != normal[0] || h.normal[1] != normal[1] || h.normal[2] != normal[2]) {
            if ((int)ROAM_PRIM_INDEX(h.prim) != (int)a->wheel_probes[i].span_index) s_rp.st.mirror_diff_walker++;
            else s_rp.st.mirror_diff_same++;
        }
        /* the call site normalises the strip's normal after this call; the raw
         * triangle normal is what both sides hand it */
        if (h.normal[0] != normal[0] || h.normal[1] != normal[1] || h.normal[2] != normal[2])
            s_rp.st.mirror_diff_n++;
        if ((dh || h.normal[1] != normal[1]) && s_rp.st.mirror_diff_n + s_rp.st.mirror_diff_h <= 12)
            TD5_LOG_I(LOG_TAG, "MIRROR_DIFF wheel slot=%d w=%d tick=%ld span=%d(strip probe %d lane %d) lane=%d "
                      "h %d/%d n strip(%d,%d,%d) roam(%d,%d,%d)",
                      (int)a->slot_index, i, s_rp.st.ticks, (int)ROAM_PRIM_INDEX(h.prim),
                      (int)a->wheel_probes[i].span_index, (int)a->wheel_probes[i].sub_lane_index, (int)h.lane,
                      (int)*ground_y, (int)h.ground_y, normal[0], normal[1], normal[2],
                      h.normal[0], h.normal[1], h.normal[2]);
        if ((int)h.capped != (*capped != 0)) s_rp.st.mirror_diff_cap++;
        if (*surface >= 0) { s_rp.st.mirror_scmp++; if (surf != *surface) s_rp.st.mirror_diff_surf++; }
        *ground_y = h.ground_y;
        normal[0] = h.normal[0]; normal[1] = h.normal[1]; normal[2] = h.normal[2];
        *capped = h.capped;
        if (*surface >= 0) *surface = surf;
    } else if (!ok || h.prim == ROAM_PRIM_NONE) {
        /* over no tile: no ground (the backstop brings the car back) */
        s_rp.st.nohit_wheels++;
        *ground_y = y - RP_NOHIT_DROP;
        normal[0] = 0; normal[1] = -4096; normal[2] = 0;
        *capped = 1;
        *surface = TD5_SURFACE_DRY_ASPHALT;
        ss->probe_ok[4 + i] = 0;
        s_rp.last_capped = 1;
        return 1;
    } else {
        if (!h.inside) s_rp.st.outside_wheels++;
        surf = rp_surface(rs, &h);
        *ground_y = h.ground_y;
        normal[0] = h.normal[0]; normal[1] = h.normal[1]; normal[2] = h.normal[2];
        *capped = h.capped;
        *surface = surf;
    }
    if (a->slot_index == 0 && h.inside && ss->settled > 30) {   /* not within 1 s of a spawn / teleport */
        /* wheel gap: the transformed wheel bottom (pre-snap) above its ground, 24.8 */
        const int32_t gap = y - h.ground_y;
        const double sp = sqrt((double)a->linear_velocity_x * a->linear_velocity_x +
                               (double)a->linear_velocity_z * a->linear_velocity_z) / 256.0;
        if (s_rp.st.gap_n == 0 || gap < s_rp.st.gap_min) s_rp.st.gap_min = gap;
        s_rp.st.gap_n++;
        if (sp < 0.5 * 430.0 / 30.0 / 3.6) {      /* < 0.5 km/h: at rest */
            if (s_rp.st.rest_n == 0 || gap > s_rp.st.rest_max) s_rp.st.rest_max = gap;
            if (s_rp.st.rest_n == 0 || gap < s_rp.st.rest_min) s_rp.st.rest_min = gap;
            s_rp.st.rest_sum += gap;
            s_rp.st.rest_n++;
        }
    }
    ss->hint[4 + i] = h.prim;
    ss->probe[4 + i] = h;
    ss->probe_ok[4 + i] = 1;
    s_rp.last_capped = h.capped;
    return 1;
}

int td5_roam_corner_height(TD5_Actor *a, int i, int32_t x, int32_t z, int32_t y,
                           int32_t *y_io, int strip_valid)
{
    const TD5_RoamResident *rs = rp_rs();
    RoamSlotState *ss;
    TD5_RoamHit h;
    int ok;
    if (!rs || !a || i < 0 || i > 3 || !y_io) return 0;
    ss = rp_slot(a);
    ok = td5_roam_locate(rs, x, z, y, 1, ss->hint[i], &h);
    if (!td5_roam_active()) {
        if (!ok || h.prim == ROAM_PRIM_NONE || !h.inside || !strip_valid) {
            s_rp.st.mirror_corner_fallback++;
            return 0;
        }
        s_rp.st.mirror_corner_cmp++;
        if (h.ground_y != *y_io) {
            s_rp.st.mirror_corner_diff++;
            if ((int)ROAM_PRIM_INDEX(h.prim) != (int)a->body_probes[i].span_index) s_rp.st.mirror_corner_walker++;
            else s_rp.st.mirror_corner_same++;
        }
    } else if (!ok || h.prim == ROAM_PRIM_NONE) {
        ss->probe_ok[i] = 0;
        return 0;
    }
    *y_io = h.ground_y;
    ss->hint[i] = h.prim;
    ss->probe[i] = h;
    ss->probe_ok[i] = 1;
    return 1;
}

/* ============================================================================
 * SECTION: chassis, heading normal, surface, ground (td5_track.c early-outs)
 * ========================================================================== */

void td5_roam_actor_locate(TD5_Actor *a)
{
    const TD5_RoamResident *rs = rp_rs();
    RoamSlotState *ss;
    TD5_RoamHit h;
    if (!a) return;
    /* F0b rule: in roam every span field stays -1, the street piece is in the side table */
    a->track_span_raw = -1;
    a->track_span_normalized = -1;
    a->track_span_accumulated = -1;
    a->track_span_high_water = -1;
    if (!rs) return;
    ss = rp_slot(a);
    if (td5_roam_locate(rs, a->world_pos.x, a->world_pos.z, a->world_pos.y, 1, ss->chassis_hint, &h) &&
        h.prim != ROAM_PRIM_NONE) {
        rp_phys_normal(h.normal, h.normal);
        ss->chassis = h;
        ss->chassis_ok = 1;
        ss->chassis_hint = h.prim;
        td5_roam_set_slot_edge((int)a->slot_index, h.edge_key);
    } else {
        ss->chassis_ok = 0;
    }
}

/* The heading normal of a hit's triangle with the strip's own heading routine
 * (td5_track_segment_normal_v = ComputeActorHeadingFromTrackSegment's inner, whose
 * FPU normalisation differs from triangle_height's by +-1 unit): a ribbon quad picks
 * its triangle by the same diagonal test as the strip, a patch triangle uses its
 * stored (strip-sign) winding. 0 for a bad hit. */
static int rp_heading_of_hit(const TD5_RoamResident *rs, const TD5_RoamHit *h, int32_t x, int32_t z,
                             int16_t out[3])
{
    const uint32_t slot = ROAM_PRIM_SLOT(h->prim), idx = ROAM_PRIM_INDEX(h->prim);
    const RoamTile *t;
    if (h->prim == ROAM_PRIM_NONE || slot >= (uint32_t)rs->nslot || !(t = rs->tile[slot])) return 0;
    if (ROAM_PRIM_IS_TRI(h->prim)) {
        const RoamTri *tr;
        if (idx >= t->ntri) return 0;
        tr = &t->tri[idx];
        td5_track_segment_normal_v(t->vert, tr->v[0], tr->v[1], tr->v[2], out);
    } else {
        const TD5_StripSpan *sp;
        int vl0, vl1, vr0, vr1;
        int32_t lx, lz;
        if (idx >= t->nspan) return 0;
        sp = &t->span[idx];
        td5_track_quad_vertices(sp, h->lane < 0 ? 0 : h->lane, &vl0, &vl1, &vr0, &vr1);
        lx = (int32_t)((int64_t)FP_TRUNC(x) + rs->org_x - t->org_x - sp->origin_x);
        lz = (int32_t)((int64_t)FP_TRUNC(z) + rs->org_z - t->org_z - sp->origin_z);
        if (td5_track_diagonal_cross_v(t->vert, vl0, vr1, lx, lz) > 0)
            td5_track_segment_normal_v(t->vert, vl0, vr1, vl1, out);
        else
            td5_track_segment_normal_v(t->vert, vl0, vr0, vr1, out);
    }
    if (out[1] == 0) out[1] = 1;
    return 1;
}

int td5_roam_heading_normal(TD5_Actor *a, int compare)
{
    const TD5_RoamResident *rs = rp_rs();
    RoamSlotState *ss;
    TD5_RoamHit h;
    if (!a || !rs) return 0;
    ss = rp_slot(a);
    if (compare) {
        if (!td5_roam_locate(rs, a->world_pos.x, a->world_pos.z, a->world_pos.y, 1, ss->chassis_hint, &h) ||
            h.prim == ROAM_PRIM_NONE || !h.inside) {
            s_rp.st.mirror_head_fallback++;
            return 0;
        }
        if (!rp_heading_of_hit(rs, &h, a->world_pos.x, a->world_pos.z, h.normal)) {
            s_rp.st.mirror_head_fallback++;
            return 0;
        }
        ss->chassis_hint = h.prim;
        s_rp.st.mirror_head_cmp++;
        if (a->heading_normal.x != h.normal[0] || a->heading_normal.y != h.normal[1] ||
            a->heading_normal.z != h.normal[2]) {
            s_rp.st.mirror_head_diff++;
            if (s_rp.st.mirror_head_diff <= 8)
                TD5_LOG_I(LOG_TAG, "MIRROR_DIFF heading slot=%d tick=%ld chassis span %d lane %d / roam span %d lane %d "
                          "strip(%d,%d,%d) roam(%d,%d,%d)",
                          (int)a->slot_index, (long)g_td5.simulation_tick_counter, (int)a->track_span_raw, (int)a->track_sub_lane_index,
                          (int)ROAM_PRIM_INDEX(h.prim), (int)h.lane, a->heading_normal.x, a->heading_normal.y,
                          a->heading_normal.z, h.normal[0], h.normal[1], h.normal[2]);
        }
    } else {
        if (!ss->chassis_ok) {
            if (a->heading_normal.y == 0) { a->heading_normal.x = 0; a->heading_normal.y = -4096; a->heading_normal.z = 0; }
            return 0;
        }
        h = ss->chassis;
        if (!rp_heading_of_hit(rs, &h, a->world_pos.x, a->world_pos.z, h.normal))
            rp_phys_normal(ss->chassis.normal, h.normal);
    }
    a->heading_normal.x = h.normal[0];
    a->heading_normal.y = h.normal[1] ? h.normal[1] : 1;
    a->heading_normal.z = h.normal[2];
    return 1;
}

int td5_roam_surface_type(TD5_Actor *a, int probe_index)
{
    const TD5_RoamResident *rs = rp_rs();
    RoamSlotState *ss;
    if (!a || !rs || probe_index < 0 || probe_index > 7) return TD5_SURFACE_DRY_ASPHALT;
    ss = rp_slot(a);
    if (!ss->probe_ok[probe_index]) return TD5_SURFACE_DRY_ASPHALT;
    return rp_surface(rs, &ss->probe[probe_index]);
}

int td5_roam_last_capped(void) { return s_rp.last_capped; }

int32_t td5_roam_ground(int32_t x, int32_t z, int32_t y, int has_y, int16_t *normal)
{
    const TD5_RoamResident *rs = rp_rs();
    TD5_RoamHit h;
    if (normal) { normal[0] = 0; normal[1] = -4096; normal[2] = 0; }
    s_rp.last_capped = 0;
    if (!rs || !td5_roam_locate(rs, x, z, y, has_y, ROAM_PRIM_NONE, &h) || h.prim == ROAM_PRIM_NONE)
        return 0;
    if (normal) rp_phys_normal(h.normal, normal);
    s_rp.last_capped = h.capped;
    return h.ground_y;
}

/* ============================================================================
 * SECTION: walls + the off-surface backstop (design 2.5)
 * ========================================================================== */

static int32_t rp_wall_angle(double dx, double dz)
{
    /* inward normal = push direction (toward the road); the legacy rail-wall angle
     * convention atan2(nnx, -nnz), as geo_resolve_walls */
    return (int32_t)(atan2(dx, -dz) * (4096.0 / (2.0 * 3.14159265358979323846))) & 0xFFF;
}

void td5_roam_resolve_walls(TD5_Actor *a)
{
    const TD5_RoamResident *rs = rp_rs();
    RoamSlotState *ss;
    TD5_Vec3_Fixed *corner;
    TD5_RoamHit hk[4];
    int out[4], k, deep = -1, pinch = 0, pa = -1, pb = -1;
    double nx[4], nz[4];
    if (!a || !rs || !td5_roam_active()) return;
    if (a->slot_index >= RP_RACERS) return;
    ss = rp_slot(a);
    corner = &a->probe_FL;

    for (k = 0; k < 4; k++) {
        out[k] = 0;
        if (!td5_roam_locate(rs, corner[k].x, corner[k].z, corner[k].y, 1, ss->hint[k], &hk[k]) ||
            hk[k].prim == ROAM_PRIM_NONE)
            continue;
        if (hk[k].inside || hk[k].push_dist <= ROAM_KERB_OVERHANG) continue;
        {
            const double m = sqrt((double)hk[k].push_x * hk[k].push_x + (double)hk[k].push_z * hk[k].push_z);
            if (m < 0.5) continue;
            nx[k] = hk[k].push_x / m; nz[k] = hk[k].push_z / m;
        }
        out[k] = 1;
        if (deep < 0 || hk[k].push_dist > hk[deep].push_dist) deep = k;
    }
    for (k = 0; k < 4 && !pinch; k++) {
        int j;
        if (!out[k]) continue;
        for (j = k + 1; j < 4; j++)
            if (out[j] && nx[k] * nx[j] + nz[k] * nz[j] < 0.0) { pinch = 1; pa = k; pb = j; break; }
    }

    if (deep >= 0) {
        const TD5_RoamHit *h = &hk[deep];
        int32_t pen = h->push_dist - ROAM_KERB_OVERHANG;
        const int32_t angle = rp_wall_angle(nx[deep], nz[deep]);
        if (pen > s_rp.st.wall_corner_pen_max) s_rp.st.wall_corner_pen_max = pen;
        if (pen > RP_PUSH_CAP) pen = RP_PUSH_CAP;
        if (pinch) {
            /* no translation this tick: the impulse of the deeper corner only */
            const int32_t sx = a->world_pos.x, sz = a->world_pos.z;
            td5_physics_wall_response(a, angle, -pen, 1, corner[deep].x, corner[deep].z);
            a->world_pos.x = sx; a->world_pos.z = sz;
            td5_physics_rebuild_pose(a);
            s_rp.st.pinches++;
            if (s_rp.st.pinches <= 40) {
                int q;
                for (q = 0; q < 4; q++)
                    TD5_LOG_W(LOG_TAG, "ROAM_WALL_PINCH   corner %d at (%d,%d,%d): prim %08x %s edge %016llx inside=%d "
                              "push (%d,%d) dist %d wall %u lateral %d lane %d",
                              q, (int)FP_TRUNC(corner[q].x), (int)FP_TRUNC(corner[q].y), (int)FP_TRUNC(corner[q].z),
                              (unsigned)hk[q].prim, hk[q].is_patch ? "patch" : "ribbon",
                              (unsigned long long)hk[q].edge_key, (int)hk[q].inside, (int)hk[q].push_x,
                              (int)hk[q].push_z, (int)hk[q].push_dist, (unsigned)hk[q].wall, (int)hk[q].lateral,
                              (int)hk[q].lane);
            }
            if (s_rp.st.pinches <= 500)
                TD5_LOG_W(LOG_TAG, "ROAM_WALL_PINCH slot=%d tick=%ld corners %d/%d edges %016llx/%016llx walls %u/%u "
                          "dist %d/%d pos=(%d,%d) speed=%.0f",
                          (int)a->slot_index, (long)g_td5.simulation_tick_counter, pa, pb,
                          (unsigned long long)hk[pa].edge_key, (unsigned long long)hk[pb].edge_key,
                          (unsigned)hk[pa].wall, (unsigned)hk[pb].wall, (int)hk[pa].push_dist, (int)hk[pb].push_dist,
                          (int)FP_TRUNC(a->world_pos.x), (int)FP_TRUNC(a->world_pos.z),
                          sqrt((double)a->linear_velocity_x * a->linear_velocity_x +
                               (double)a->linear_velocity_z * a->linear_velocity_z) / 256.0);
        } else {
            td5_physics_wall_response(a, angle, -pen, 1, corner[deep].x, corner[deep].z);
            td5_physics_rebuild_pose(a);
            s_rp.st.wall_hits++;
            if (s_rp.st.wall_hits <= RP_WALL_LOG_ALL || (s_rp.st.wall_hits % 50) == 0)
                TD5_LOG_I(LOG_TAG, "ROAM_WALL slot=%d tick=%ld corner=%d edge=%016llx wall=%u dist=%d pen=%d angle=%d "
                          "pos=(%d,%d) (hit #%ld)",
                          (int)a->slot_index, (long)g_td5.simulation_tick_counter, deep, (unsigned long long)h->edge_key,
                          (unsigned)h->wall, (int)h->push_dist, (int)pen, (int)angle,
                          (int)FP_TRUNC(a->world_pos.x), (int)FP_TRUNC(a->world_pos.z), s_rp.st.wall_hits);
        }
    }

    /* ---- off-surface backstop (chassis) ---- */
    {
        TD5_RoamHit c;
        const int ok = td5_roam_locate(rs, a->world_pos.x, a->world_pos.z, a->world_pos.y, 1, ss->chassis_hint, &c);
        if (!ok || c.prim == ROAM_PRIM_NONE) {
            if (ss->good_valid) {
                s_rp.st.teleports++;
                TD5_LOG_W(LOG_TAG, "ROAM_TELEPORT slot=%d tick=%ld: chassis (%d,%d) over no resident tile -> last good (%d,%d,%d)",
                          (int)a->slot_index, (long)g_td5.simulation_tick_counter, (int)FP_TRUNC(a->world_pos.x), (int)FP_TRUNC(a->world_pos.z),
                          (int)FP_TRUNC(ss->good_x), (int)FP_TRUNC(ss->good_y), (int)FP_TRUNC(ss->good_z));
                a->world_pos.x = ss->good_x; a->world_pos.y = ss->good_y; a->world_pos.z = ss->good_z;
                a->linear_velocity_x = a->linear_velocity_y = a->linear_velocity_z = 0;
                ss->settled = 0;
                td5_physics_rebuild_pose(a);
            }
            return;
        }
        if (c.inside && a->slot_index == 0) {
            const int32_t above = a->world_pos.y - c.ground_y;
            if (!s_rp.st.chassis_seen || above < s_rp.st.chassis_min_above) s_rp.st.chassis_min_above = above;
            s_rp.st.chassis_seen = 1;
        }
        if (!c.inside && c.push_dist > ROAM_RESCUE_DIST) {
            const double m = sqrt((double)c.push_x * c.push_x + (double)c.push_z * c.push_z);
            if (m > 0.5) {
                const double ux = c.push_x / m, uz = c.push_z / m;
                const double vout = -((double)a->linear_velocity_x * ux + (double)a->linear_velocity_z * uz);
                const int32_t mx = rp_iround(c.push_x + ux * RP_RESCUE_IN), mz = rp_iround(c.push_z + uz * RP_RESCUE_IN);
                s_rp.st.rescues++;
                TD5_LOG_W(LOG_TAG, "ROAM_RESCUE slot=%d tick=%ld: chassis %d units outside edge %016llx wall %u, "
                          "snapped by (%d,%d) from (%d,%d)",
                          (int)a->slot_index, (long)g_td5.simulation_tick_counter, (int)c.push_dist, (unsigned long long)c.edge_key,
                          (unsigned)c.wall, (int)mx, (int)mz, (int)FP_TRUNC(a->world_pos.x), (int)FP_TRUNC(a->world_pos.z));
                a->world_pos.x += FP_SCALE(mx);
                a->world_pos.z += FP_SCALE(mz);
                if (vout > 0.0) {   /* drop the outward velocity component */
                    a->linear_velocity_x += (int32_t)(vout * ux);
                    a->linear_velocity_z += (int32_t)(vout * uz);
                }
                td5_physics_rebuild_pose(a);
            }
        } else if (c.inside && a->world_pos.y < c.ground_y - RP_FALL_DEPTH) {
            s_rp.st.rescues_y++;
            TD5_LOG_W(LOG_TAG, "ROAM_RESCUE_Y slot=%d tick=%ld: chassis y %d is %d units under its ground %d -> lifted",
                      (int)a->slot_index, (long)g_td5.simulation_tick_counter, (int)FP_TRUNC(a->world_pos.y),
                      (int)FP_TRUNC(c.ground_y - a->world_pos.y), (int)FP_TRUNC(c.ground_y));
            a->world_pos.y = c.ground_y + 215 * 256;
            if (a->linear_velocity_y < 0) a->linear_velocity_y = 0;
            td5_physics_rebuild_pose(a);
        } else if (c.inside && (a->wheel_contact_bitmask & 0x0F) != 0x0F) {
            ss->good_x = a->world_pos.x; ss->good_y = a->world_pos.y; ss->good_z = a->world_pos.z;
            ss->good_valid = 1;
            ss->settled++;
        }
    }
}

/* ============================================================================
 * SECTION: dev drive harness (DEV only)
 * ========================================================================== */
#ifndef TD5RE_RELEASE
/* TD5RE_ROAM_DRIVE=street  follow the street the car is on at TD5RE_ROAM_DRIVE_KMH (50) for
 *                          TD5RE_ROAM_DRIVE_SECS (60), then brake, rest 3 s (the rest-height
 *                          sample) and quit (TD5RE_ROAM_DRIVE_QUIT=0 keeps running).
 * TD5RE_ROAM_DRIVE=corners the design 2.5 corner matrix: up to TD5RE_ROAM_CORNERS_PER_KIND (10)
 *                          junctions of each kind (0 one-lane / service, 1 calle x calle,
 *                          2 avenida x calle, 3 diagonal x avenida), each driven at 40 then
 *                          80 km/h: teleport 55 m up the in-arm, pure pursuit S -> centroid ->
 *                          exit arm, speed held. Per run: wall hits, ROAM_WALL_PINCH (must be 0),
 *                          stuck (< 1 km/h for > 2 s with throttle on, must be 0), rescues.
 *                          Report: <log dir>/roam_corner_test.txt.
 * Same steer / throttle / brake bits as a keyboard (td5_input.c overlays them), so the drive
 * goes through the real input ramp, physics, provider and wall code. Compiled out of RELEASE. */
#define RD_PI           3.14159265358979323846
#define RD_UPT_KMH      (430.0 / 30.0 / 3.6)   /* units per tick at 1 km/h (30 Hz sim) */
#define RD_MAXC         64
#define RD_M            430.0

/* path: [0] start 45 m up the in-ribbon, [1] the in-ribbon end row, [1..RD_NP-2] a quadratic Bezier
 * through the junction (control point = where the two street centre lines meet) ending on the
 * out-ribbon end row, [RD_NP-1] 40 m out along it */
#define RD_NP 9
typedef struct { int kind; double cx, cz, inx, inz, outx, outz, defl; uint64_t cl, ein, eout; double px[RD_NP], pz[RD_NP]; } RdCorner;

static const char *const k_rd_kind[4] = { "one-lane/service", "calle x calle", "avenida x calle", "diagonal x avenida" };

struct RoamDrive {
    double  kmh;
    int     secs, quit, per_kind;
    long    t0;                   /* sim tick the harness started at */
    int     phase;
    /* street */
    double  dir_x, dir_z, off0, dist_m;
    int     have_dir, have_off, have_last, placed;
    uint64_t cl_key;                      /* the junction the street follower is crossing */
    double  cl_x, cl_z, arm_x, arm_z, tgt_x, tgt_z;
    int32_t lx, lz;
    long    rest_t, log_t;
    /* corners */
    RdCorner c[RD_MAXC];
    int     nc, ci, si;
    long    ct, stuck_t, slow_t, rev_t, stalls;
    int     retries;
    long    tot_stalls, tot_retries, k_stalls[4][2], k_retries[4][2];
    long    hits0, pinch0, resc0, tele0;
    double  vmin, vmax;
    long    runs, ok, stuck, pinch, hits, resc;
    long    k_runs[4][2], k_ok[4][2], k_stuck[4][2], k_pinch[4][2], k_hits[4][2], k_resc[4][2];
    FILE   *rep;
};

static double rd_speed_upt(const TD5_Actor *a)
{
    return sqrt((double)a->linear_velocity_x * a->linear_velocity_x +
                (double)a->linear_velocity_z * a->linear_velocity_z) / 256.0;
}

static void rd_heading(const TD5_Actor *a, double *hx, double *hz)
{
    const double sp = rd_speed_upt(a);
    if (sp > 20.0) {
        *hx = a->linear_velocity_x / 256.0 / sp;
        *hz = a->linear_velocity_z / 256.0 / sp;
    } else {
        const double yaw = (double)(a->display_angles.yaw & 0xFFF) * (2.0 * RD_PI / 4096.0);
        *hx = sin(yaw);
        *hz = cos(yaw);
    }
}

/* MEASURED by the strip harness (td5_track_dev_gap_drive_bits): STEER_LEFT turns the heading
 * toward +x, i.e. a positive atan2(x, z) error is a LEFT turn. */
static uint32_t rd_steer_to(const TD5_Actor *a, double tx, double tz, double kmh_target)
{
    double hx, hz, err;
    const double px = (double)FP_TRUNC(a->world_pos.x), pz = (double)FP_TRUNC(a->world_pos.z);
    const double kmh = rd_speed_upt(a) / RD_UPT_KMH;
    uint32_t bits = 0;
    rd_heading(a, &hx, &hz);
    err = atan2(tx - px, tz - pz) - atan2(hx, hz);
    while (err > RD_PI) err -= 2.0 * RD_PI;
    while (err < -RD_PI) err += 2.0 * RD_PI;
    if (err > 0.03) bits |= (uint32_t)TD5_INPUT_STEER_LEFT;
    else if (err < -0.03) bits |= (uint32_t)TD5_INPUT_STEER_RIGHT;
    if (kmh < kmh_target) bits |= (uint32_t)TD5_INPUT_THROTTLE;
    else if (kmh > kmh_target * 1.10) bits |= (uint32_t)TD5_INPUT_BRAKE;
    return bits;
}

/* centre line of the chassis hit's ribbon quad, world units: point nearest the car + unit axis */
static int rd_street_axis(const TD5_RoamResident *rs, const TD5_RoamHit *h, double px, double pz,
                          double *cx, double *cz, double *ax, double *az)
{
    const uint32_t slot = ROAM_PRIM_SLOT(h->prim), idx = ROAM_PRIM_INDEX(h->prim);
    const RoamTile *t;
    const TD5_StripSpan *sp;
    int vl0, vl1, vr0, vr1, L;
    double bx, bz, c0x, c0z, c1x, c1z, dx, dz, len, tt;
    if (h->prim == ROAM_PRIM_NONE || ROAM_PRIM_IS_TRI(h->prim) || slot >= (uint32_t)rs->nslot) return 0;
    if (!(t = rs->tile[slot]) || idx >= t->nspan) return 0;
    sp = &t->span[idx];
    L = sp->pad_02[1] & 0x0F;
    if (L < 1) L = 1;
    td5_track_quad_vertices(sp, 0, &vl0, &vl1, &vr0, &vr1);
    bx = (double)t->org_x - rs->org_x + sp->origin_x;
    bz = (double)t->org_z - rs->org_z + sp->origin_z;
    c0x = bx + 0.5 * (t->vert[vl0].x + t->vert[vl0 + L].x);
    c0z = bz + 0.5 * (t->vert[vl0].z + t->vert[vl0 + L].z);
    c1x = bx + 0.5 * (t->vert[vr0].x + t->vert[vr0 + L].x);
    c1z = bz + 0.5 * (t->vert[vr0].z + t->vert[vr0 + L].z);
    dx = c1x - c0x; dz = c1z - c0z;
    len = sqrt(dx * dx + dz * dz);
    if (len < 1.0) return 0;
    *ax = dx / len; *az = dz / len;
    tt = (px - c0x) * *ax + (pz - c0z) * *az;
    *cx = c0x + *ax * tt;
    *cz = c0z + *az * tt;
    return 1;
}

static const char *rd_name(const RoamTile *t, unsigned id);

/* TD5RE_ROAM_DRIVE_ON="<street name>": move the car onto the ribbon quad of that street
 * nearest to where it spawned, pointing along the quad order (TD5RE_ROAM_DRIVE_DIR=-1
 * reverses). 1 when it moved the car. */
static int rd_place_on_street(TD5_Actor *a)
{
    const char *want = getenv("TD5RE_ROAM_DRIVE_ON");
    const int dirsign = td5_env_int("TD5RE_ROAM_DRIVE_DIR", 1, -1, 1) < 0 ? -1 : 1;
    const double px = (double)FP_TRUNC(a->world_pos.x), pz = (double)FP_TRUNC(a->world_pos.z);
    double best = 1e30, bx = 0, bz = 0, bax = 0, baz = 0;
    int ti, found = 0;
    if (!want || !want[0]) return 0;
    for (ti = 0; ti < s_rp.ntile; ti++) {
        const RoamTile *t = &s_rp.tiles[ti];
        uint32_t ei, si;
        for (ei = 0; ei < t->nedge; ei++) {
            const RoamEdgeRec *e = &t->edge[ei];
            if (e->layer != 0 || strcmp(rd_name(t, e->name_id), want) != 0) continue;
            for (si = e->span_first; si < (uint32_t)e->span_first + e->span_count && si < t->nspan; si++) {
                TD5_RoamHit h;
                double cx, cz, ax, az, d2;
                memset(&h, 0, sizeof h);
                h.prim = ROAM_PRIM_MAKE(ti, 0, si);   /* slot == load index (resident_add in order) */
                if (td5_roam_resident_find(s_rp.rs, t->tx, t->tz) != ti) continue;
                if (!rd_street_axis(s_rp.rs, &h, px, pz, &cx, &cz, &ax, &az)) continue;
                d2 = (cx - px) * (cx - px) + (cz - pz) * (cz - pz);
                if (d2 < best) { best = d2; bx = cx; bz = cz; bax = ax * dirsign; baz = az * dirsign; found = 1; }
            }
        }
    }
    if (!found) {
        TD5_LOG_W(LOG_TAG, "[ROAMDRIVE] no ribbon named '%s' in the resident set", want);
        return 0;
    }
    a->world_pos.x = (int32_t)(bx * 256.0);
    a->world_pos.z = (int32_t)(bz * 256.0);
    a->euler_accum.yaw = FP_SCALE((int32_t)(atan2(bax, baz) * 4096.0 / (2.0 * RD_PI)) & 0xFFF);
    td5_physics_reset_actor_state(a);
    {
        RoamSlotState *ss = rp_slot(a);
        int k;
        for (k = 0; k < 8; k++) ss->hint[k] = ROAM_PRIM_NONE;
        ss->chassis_hint = ROAM_PRIM_NONE;
        ss->settled = 0;
    }
    TD5_LOG_I(LOG_TAG, "[ROAMDRIVE] placed on '%s' %.0f m from the spawn at world (%.0f,%.0f) heading (%.2f,%.2f)",
              want, sqrt(best) / RD_M, bx, bz, bax, baz);
    return 1;
}

static void rd_finish(struct RoamDrive *d, const char *why)
{
    rp_write_summary(why);
    if (d->quit) g_td5.quit_requested = 1;
    d->phase = 99;
}

/* The end row of street piece `key` nearest to (cx, cz), world units: its centre and the unit
 * direction along the ribbon pointing AWAY from that end into the street. 0 when not resident. */
static int rd_edge_end_near(uint64_t key, double cx, double cz, double *ex, double *ez, double *ox, double *oz)
{
    double best = 1e30;
    int ti, found = 0;
    for (ti = 0; ti < s_rp.ntile; ti++) {
        const RoamTile *t = &s_rp.tiles[ti];
        uint32_t k;
        for (k = 0; k < t->nedge; k++) {
            const RoamEdgeRec *e = &t->edge[k];
            int end;
            if (e->key != key || e->span_count == 0) continue;
            for (end = 0; end < 2; end++) {
                const uint32_t si = end ? (uint32_t)e->span_first + e->span_count - 1 : e->span_first;
                const TD5_StripSpan *sp;
                int vl0, vl1, vr0, vr1, L;
                double bx, bz, nx0, nz0, fx0, fz0, qx, qz, rx, rz, d2, len;
                if (si >= t->nspan) continue;
                sp = &t->span[si];
                L = sp->pad_02[1] & 0x0F;
                if (L < 1) L = 1;
                td5_track_quad_vertices(sp, 0, &vl0, &vl1, &vr0, &vr1);
                bx = (double)t->org_x - s_rp.rs->org_x + sp->origin_x;
                bz = (double)t->org_z - s_rp.rs->org_z + sp->origin_z;
                nx0 = bx + 0.5 * (t->vert[vl0].x + t->vert[vl0 + L].x);
                nz0 = bz + 0.5 * (t->vert[vl0].z + t->vert[vl0 + L].z);
                fx0 = bx + 0.5 * (t->vert[vr0].x + t->vert[vr0 + L].x);
                fz0 = bz + 0.5 * (t->vert[vr0].z + t->vert[vr0 + L].z);
                if (end) { qx = fx0; qz = fz0; rx = nx0 - fx0; rz = nz0 - fz0; }   /* far row, inward = back */
                else     { qx = nx0; qz = nz0; rx = fx0 - nx0; rz = fz0 - nz0; }
                d2 = (qx - cx) * (qx - cx) + (qz - cz) * (qz - cz);
                len = sqrt(rx * rx + rz * rz);
                if (len < 1.0 || d2 >= best) continue;
                best = d2; *ex = qx; *ez = qz; *ox = rx / len; *oz = rz / len; found = 1;
            }
        }
    }
    return found;
}

/* ---- street ---- */
static uint32_t rd_street(struct RoamDrive *d, TD5_Actor *a, long tick)
{
    const TD5_RoamResident *rs = rp_rs();
    const RoamSlotState *ss = rp_slot(a);
    const double px = (double)FP_TRUNC(a->world_pos.x), pz = (double)FP_TRUNC(a->world_pos.z);
    const double kmh = rd_speed_upt(a) / RD_UPT_KMH;
    double cx, cz, ax, az, hx, hz, look, tx, tz;
    uint32_t bits = 0;
    if (!d->placed) {
        d->placed = 1;
        if (rd_place_on_street(a)) { d->t0 = tick; return 0; }
    }
    if (tick - d->t0 < 15) return 0;
    if (d->have_last) d->dist_m += sqrt((px - d->lx) * (px - d->lx) + (pz - d->lz) * (pz - d->lz)) / RD_M;
    d->lx = (int32_t)px; d->lz = (int32_t)pz; d->have_last = 1;
    rd_heading(a, &hx, &hz);
    if (!d->have_dir) { d->dir_x = hx; d->dir_z = hz; d->have_dir = 1; }
    look = 14.0 * RD_M + rd_speed_upt(a) * 20.0;
    if (ss->chassis_ok && rd_street_axis(rs, &ss->chassis, px, pz, &cx, &cz, &ax, &az)) {
        double qx, qz, off;
        if (ax * d->dir_x + az * d->dir_z < 0.0) { ax = -ax; az = -az; }
        if (ax * d->dir_x + az * d->dir_z < 0.82) {     /* > 35 deg off: a crossing street, keep going straight */
            ax = d->dir_x; az = d->dir_z;
            cx = px; cz = pz;
        }
        d->dir_x = ax; d->dir_z = az;
        qx = d->dir_z; qz = -d->dir_x;                  /* a perpendicular, kept by continuity */
        off = (px - cx) * qx + (pz - cz) * qz;
        if (!d->have_off) { d->off0 = d->placed > 1 ? 0.0 : off; d->have_off = 1; d->placed = 2; }
        tx = cx + qx * d->off0 + d->dir_x * look;
        tz = cz + qz * d->off0 + d->dir_z * look;
    } else if (ss->chassis_ok && ss->chassis.is_patch && rs) {
        /* junction patch: once per cluster take the arm that best continues the heading
         * (never the one we came in on) and aim 25 m out along it, like a driver would */
        const uint32_t slot = ROAM_PRIM_SLOT(ss->chassis.prim), idx = ROAM_PRIM_INDEX(ss->chassis.prim);
        const RoamTile *t = slot < (uint32_t)rs->nslot ? rs->tile[slot] : NULL;
        if (t && idx < t->ntri && t->tri[idx].patch < t->nnode) {
            const RoamNodeRec *nd = &t->node[t->tri[idx].patch];
            if ((uint64_t)nd->cluster_key != d->cl_key) {
                int k, bk = -1;
                double bd = -0.3;
                for (k = 0; k < nd->narm; k++) {
                    const double b = (double)t->arm[nd->arm_first + k].bearing12 * (2.0 * RD_PI / 4096.0);
                    const double dd = cos(b) * d->dir_x + sin(b) * d->dir_z;
                    if (dd > bd) { bd = dd; bk = k; }
                }
                d->cl_key = (uint64_t)nd->cluster_key;
                d->cl_x = (double)t->org_x - rs->org_x + nd->x;
                d->cl_z = (double)t->org_z - rs->org_z + nd->z;
                d->tgt_x = d->tgt_z = 0.0;
                if (bk >= 0) {
                    const RoamArmRec *ar = &t->arm[nd->arm_first + bk];
                    const double b = (double)ar->bearing12 * (2.0 * RD_PI / 4096.0);
                    double ex = 0.0, ez = 0.0, ox = 0.0, oz = 0.0;
                    d->arm_x = cos(b); d->arm_z = sin(b);
                    /* aim at where the arm's ribbon starts (wide plaza patches: the bearing line from
                     * the centroid can cross the island), then 15 m along the ribbon */
                    if (rd_edge_end_near(ar->edge_key, d->cl_x, d->cl_z, &ex, &ez, &ox, &oz)) {
                        d->arm_x = ox; d->arm_z = oz;
                        d->tgt_x = ex + ox * 15.0 * RD_M; d->tgt_z = ez + oz * 15.0 * RD_M;
                    }
                } else {
                    d->arm_x = d->dir_x; d->arm_z = d->dir_z;
                }
                if (d->tgt_x == 0.0 && d->tgt_z == 0.0) {
                    d->tgt_x = d->cl_x + d->arm_x * 25.0 * RD_M; d->tgt_z = d->cl_z + d->arm_z * 25.0 * RD_M;
                }
                d->have_off = 0;                        /* re-centre on the exit street */
                TD5_LOG_I(LOG_TAG, "[ROAMDRIVE] junction %016llx (%d arms) at (%.0f,%.0f): heading (%.2f,%.2f) -> arm %d "
                          "(%.2f,%.2f), car at (%.0f,%.0f)", (unsigned long long)nd->cluster_key, (int)nd->narm,
                          d->cl_x, d->cl_z, d->dir_x, d->dir_z, bk, d->arm_x, d->arm_z, px, pz);
            }
            tx = d->tgt_x;
            tz = d->tgt_z;
            d->dir_x = d->arm_x; d->dir_z = d->arm_z;
        } else {
            tx = px + d->dir_x * look;
            tz = pz + d->dir_z * look;
        }
    } else {                                            /* nothing under the chassis: hold the direction */
        tx = px + d->dir_x * look;
        tz = pz + d->dir_z * look;
    }
    if (d->phase == 0) {
        /* slow down for a turn: the target speed scales with how far the aim is off the heading */
        double e = atan2(tx - px, tz - pz) - atan2(hx, hz), vt = d->kmh;
        while (e > RD_PI) e -= 2.0 * RD_PI;
        while (e < -RD_PI) e += 2.0 * RD_PI;
        if (fabs(e) > 0.25) vt = d->kmh * 0.4 > 15.0 ? d->kmh * 0.4 : 15.0;
        bits = rd_steer_to(a, tx, tz, vt);
        if (tick - d->t0 >= (long)d->secs * 30) {
            d->phase = 1;
            TD5_LOG_I(LOG_TAG, "[ROAMDRIVE] %d s done: %.0f m driven, braking to rest", d->secs, d->dist_m);
        }
    } else if (d->phase == 1) {
        /* brake down to 3 km/h, then coast: brake held at a standstill is REVERSE in TD5 */
        bits = rd_steer_to(a, tx, tz, 0.0) & ~(uint32_t)(TD5_INPUT_THROTTLE | TD5_INPUT_BRAKE);
        if (kmh > 3.0) bits |= (uint32_t)TD5_INPUT_BRAKE;
        if (kmh < 0.3) { d->phase = 2; d->rest_t = tick; s_rp.st.rest_n = 0; s_rp.st.rest_sum = 0; }
    } else if (d->phase == 2) {
        bits = 0;
        if (tick - d->rest_t >= 90) {
            TD5_LOG_I(LOG_TAG, "[ROAMDRIVE] RESULT %.0f m in %d s, wall_hits=%ld pinches=%ld rescues=%ld+%ld teleports=%ld "
                      "nohit_wheels=%ld wheel_gap_min=%.2f cm rest gap %.2f..%.2f cm (mean %.2f, %ld samples) "
                      "chassis min above ground %.2f cm",
                      d->dist_m, d->secs, s_rp.st.wall_hits, s_rp.st.pinches, s_rp.st.rescues, s_rp.st.rescues_y,
                      s_rp.st.teleports, s_rp.st.nohit_wheels, s_rp.st.gap_min / 256.0 / 4.30,
                      s_rp.st.rest_min / 256.0 / 4.30, s_rp.st.rest_max / 256.0 / 4.30,
                      s_rp.st.rest_n ? (double)s_rp.st.rest_sum / s_rp.st.rest_n / 256.0 / 4.30 : 0.0, s_rp.st.rest_n,
                      s_rp.st.chassis_min_above / 256.0 / 4.30);
            rd_finish(d, "street drive done");
        }
    }
    if (tick - d->log_t >= 15 && d->phase < 99) {
        d->log_t = tick;
        TD5_LOG_I(LOG_TAG, "[ROAMDRIVE] t=%ld pos=(%d,%d,%d) kmh=%.1f edge=%016llx patch=%d inside=%d mask=0x%x "
                  "walls=%ld pinch=%ld dist=%.0f m",
                  tick - d->t0, (int)px, (int)FP_TRUNC(a->world_pos.y), (int)pz, kmh,
                  (unsigned long long)ss->chassis.edge_key, (int)ss->chassis.is_patch, (int)ss->chassis.inside,
                  (unsigned)a->wheel_contact_bitmask, s_rp.st.wall_hits, s_rp.st.pinches, d->dist_m);
    }
    return bits;
}

/* ---- corners ---- */
static const char *rd_name(const RoamTile *t, unsigned id)
{
    const char *p = t->names, *e;
    unsigned k = 0;
    if (!p || id == 0xFFFFu) return "";
    e = p + t->names_bytes;
    while (p < e && k < id) { p += strlen(p) + 1; k++; }
    return p < e ? p : "";
}

static const RoamEdgeRec *rd_edge(uint64_t key, const RoamTile **tout)
{
    int i;
    uint32_t k;
    for (i = 0; i < s_rp.ntile; i++) {
        const RoamTile *t = &s_rp.tiles[i];
        for (k = 0; k < t->nedge; k++)
            if (t->edge[k].key == key) { *tout = t; return &t->edge[k]; }
    }
    return NULL;
}

static int rd_is(const char *n, const char *pfx) { return strncmp(n, pfx, strlen(pfx)) == 0; }

/* way of a world point: the locator's edge key >> 16 (way id), 0 = none / patch */
static uint64_t rd_way_at(double x, double z)
{
    TD5_RoamHit h;
    if (!td5_roam_locate(s_rp.rs, (int32_t)(x * 256.0), (int32_t)(z * 256.0), 0, 0, ROAM_PRIM_NONE, &h) ||
        h.prim == ROAM_PRIM_NONE || !h.inside || h.is_patch)
        return 0;
    return h.edge_key >> 16;
}

static int rd_kind_of(const RoamEdgeRec *ea, const RoamEdgeRec *eb, const char *na, const char *nb, double defl)
{
    const int av_a = rd_is(na, "Avenida"), av_b = rd_is(nb, "Avenida");
    const int dg_a = rd_is(na, "Diagonal"), dg_b = rd_is(nb, "Diagonal");
    if ((dg_a && av_b) || (av_a && dg_b)) return (defl >= 45.0 && defl <= 135.0) ? 3 : -1;
    if (dg_a || dg_b) return -1;
    if (av_a && av_b) return -1;
    if (av_a != av_b) return fabs(defl - 90.0) <= 15.0 ? 2 : -1;
    if ((ea->klass == TD5_GEO_RC_SERVICE || ea->lanes == 1) && (eb->klass == TD5_GEO_RC_SERVICE || eb->lanes == 1))
        return fabs(defl - 90.0) <= 20.0 ? 0 : -1;
    if (ea->klass >= TD5_GEO_RC_LIVING && ea->klass <= TD5_GEO_RC_TERTIARY &&
        eb->klass >= TD5_GEO_RC_LIVING && eb->klass <= TD5_GEO_RC_TERTIARY)
        return fabs(defl - 90.0) <= 15.0 ? 1 : -1;
    return -1;
}

static void rd_pick_corners(struct RoamDrive *d)
{
    int ti, kind_n[4] = { 0, 0, 0, 0 };
    uint32_t ni;
    d->nc = 0;
    for (ti = 0; ti < s_rp.ntile; ti++) {
        const RoamTile *t = &s_rp.tiles[ti];
        for (ni = 0; ni < t->nnode; ni++) {
            const RoamNodeRec *nd = &t->node[ni];
            int ai, ao, kind;
            double best[4] = { 1e9, 1e9, 1e9, 1e9 };
            RdCorner cand[4];
            memset(cand, 0, sizeof cand);
            if (nd->narm < 3 || nd->narm > 4 || (nd->flags & 0x04)) continue;
            for (ai = 0; ai < nd->narm; ai++) {
                for (ao = 0; ao < nd->narm; ao++) {
                    const RoamArmRec *A = &t->arm[nd->arm_first + ai], *B = &t->arm[nd->arm_first + ao];
                    const RoamTile *ta = NULL, *tb = NULL;
                    const RoamEdgeRec *ea, *eb;
                    double ia, oa, inx, inz, outx, outz, defl, cx, cz, eix, eiz, eox, eoz;
                    if (ai == ao) continue;
                    if (!(ea = rd_edge(A->edge_key, &ta)) || !(eb = rd_edge(B->edge_key, &tb))) continue;
                    if (ea->layer != 0 || eb->layer != 0) continue;
                    ia = (double)A->bearing12 * (2.0 * RD_PI / 4096.0);
                    oa = (double)B->bearing12 * (2.0 * RD_PI / 4096.0);
                    inx = cos(ia); inz = sin(ia); outx = cos(oa); outz = sin(oa);
                    defl = acos(fmax(-1.0, fmin(1.0, -(inx * outx + inz * outz)))) * 180.0 / RD_PI;
                    kind = rd_kind_of(ea, eb, rd_name(ta, ea->name_id), rd_name(tb, eb->name_id), defl);
                    if (kind < 0 || fabs(defl - 90.0) >= best[kind]) continue;
                    cx = (double)t->org_x + nd->x - s_rp.rs->org_x;
                    cz = (double)t->org_z + nd->z - s_rp.rs->org_z;
                    /* the path runs on the ribbons' own centre lines (the arm bearing through the
                     * centroid can run along a kerb): in-ribbon end row and axis, out-ribbon likewise */
                    if (!rd_edge_end_near(A->edge_key, cx, cz, &eix, &eiz, &inx, &inz)) continue;
                    if (!rd_edge_end_near(B->edge_key, cx, cz, &eox, &eoz, &outx, &outz)) continue;
                    defl = acos(fmax(-1.0, fmin(1.0, -(inx * outx + inz * outz)))) * 180.0 / RD_PI;
                    if (rd_kind_of(ea, eb, rd_name(ta, ea->name_id), rd_name(tb, eb->name_id), defl) != kind) continue;
                    if (fabs(defl - 90.0) >= best[kind]) continue;
                    /* the approach and the exit must be on the arms' own ways */
                    if (rd_way_at(eix + inx * 45.0 * RD_M, eiz + inz * 45.0 * RD_M) != (A->edge_key >> 16)) continue;
                    if (rd_way_at(eox + outx * 40.0 * RD_M, eoz + outz * 40.0 * RD_M) != (B->edge_key >> 16)) continue;
                    {
                        /* control point: E_in + t*(-in) meets E_out + u*(-out) */
                        const double ax_ = -inx, az_ = -inz, bx_ = -outx, bz_ = -outz;
                        const double det = ax_ * (-bz_) - az_ * (-bx_);
                        double qx = 0.5 * (eix + eox), qz = 0.5 * (eiz + eoz);
                        int q;
                        if (fabs(det) > 1e-6) {
                            const double tt = ((eox - eix) * (-bz_) - (eoz - eiz) * (-bx_)) / det;
                            if (tt > 0.0 && tt < 60.0 * RD_M) { qx = eix + ax_ * tt; qz = eiz + az_ * tt; }
                        }
                        cand[kind].px[0] = eix + inx * 45.0 * RD_M; cand[kind].pz[0] = eiz + inz * 45.0 * RD_M;
                        for (q = 0; q <= RD_NP - 3; q++) {
                            const double u = (double)q / (double)(RD_NP - 3);
                            const double w0 = (1 - u) * (1 - u), w1 = 2 * u * (1 - u), w2 = u * u;
                            cand[kind].px[1 + q] = w0 * eix + w1 * qx + w2 * eox;
                            cand[kind].pz[1 + q] = w0 * eiz + w1 * qz + w2 * eoz;
                        }
                        cand[kind].px[RD_NP - 1] = eox + outx * 40.0 * RD_M;
                        cand[kind].pz[RD_NP - 1] = eoz + outz * 40.0 * RD_M;
                    }
                    best[kind] = fabs(defl - 90.0);
                    cand[kind].kind = kind; cand[kind].cx = cx; cand[kind].cz = cz;
                    cand[kind].inx = inx; cand[kind].inz = inz; cand[kind].outx = outx; cand[kind].outz = outz;
                    cand[kind].defl = defl; cand[kind].cl = (uint64_t)nd->cluster_key;
                    cand[kind].ein = A->edge_key; cand[kind].eout = B->edge_key;
                }
            }
            for (kind = 0; kind < 4; kind++) {
                int j, far_ok = 1;
                if (best[kind] > 1e8 || kind_n[kind] >= d->per_kind || d->nc >= RD_MAXC) continue;
                for (j = 0; j < d->nc; j++)
                    if (d->c[j].kind == kind &&
                        hypot(d->c[j].cx - cand[kind].cx, d->c[j].cz - cand[kind].cz) < 250.0 * RD_M) far_ok = 0;
                if (!far_ok) continue;
                d->c[d->nc++] = cand[kind];
                kind_n[kind]++;
            }
        }
    }
    TD5_LOG_I(LOG_TAG, "[ROAMCORNER] picked %d corners: one-lane/service %d, calle x calle %d, avenida x calle %d, "
              "diagonal x avenida %d", d->nc, kind_n[0], kind_n[1], kind_n[2], kind_n[3]);
}

static void rd_corner_start(struct RoamDrive *d, TD5_Actor *a, long tick)
{
    const RdCorner *c = &d->c[d->ci];
    const double v = (d->si ? 80.0 : 40.0) * RD_UPT_KMH;
    const double sx = c->px[0], sz = c->pz[0];
    const double tx = -c->inx, tz = -c->inz;
    RoamSlotState *ss = rp_slot(a);
    int k;
    a->world_pos.x = (int32_t)(sx * 256.0);
    a->world_pos.z = (int32_t)(sz * 256.0);
    a->euler_accum.yaw = FP_SCALE((int32_t)(atan2(tx, tz) * 4096.0 / (2.0 * RD_PI)) & 0xFFF);
    td5_physics_reset_actor_state(a);
    a->linear_velocity_x = (int32_t)(tx * v * 256.0);
    a->linear_velocity_z = (int32_t)(tz * v * 256.0);
    for (k = 0; k < 8; k++) ss->hint[k] = ROAM_PRIM_NONE;
    ss->chassis_hint = ROAM_PRIM_NONE;
    ss->good_valid = 0;
    ss->settled = 0;
    d->ct = tick; d->stuck_t = 0; d->vmin = 1e9; d->vmax = 0.0;
    d->slow_t = 0; d->rev_t = 0; d->stalls = 0; d->retries = 0;
    d->hits0 = s_rp.st.wall_hits; d->pinch0 = s_rp.st.pinches;
    d->resc0 = s_rp.st.rescues + s_rp.st.rescues_y; d->tele0 = s_rp.st.teleports;
    d->phase = 1;
}

static void rd_corner_end(struct RoamDrive *d, long tick, int ok, int stuck, const char *why)
{
    const RdCorner *c = &d->c[d->ci];
    const long hits = s_rp.st.wall_hits - d->hits0, pin = s_rp.st.pinches - d->pinch0;
    const long resc = s_rp.st.rescues + s_rp.st.rescues_y + s_rp.st.teleports - d->resc0 - d->tele0;
    const int sp = d->si;
    d->runs++; d->ok += ok; d->stuck += stuck; d->pinch += pin; d->hits += hits; d->resc += resc;
    d->k_runs[c->kind][sp]++; d->k_ok[c->kind][sp] += ok; d->k_stuck[c->kind][sp] += stuck;
    d->k_pinch[c->kind][sp] += pin; d->k_hits[c->kind][sp] += hits; d->k_resc[c->kind][sp] += resc;
    d->tot_stalls += d->stalls; d->tot_retries += d->retries;
    d->k_stalls[c->kind][sp] += d->stalls; d->k_retries[c->kind][sp] += d->retries;
    TD5_LOG_I(LOG_TAG, "[ROAMCORNER] RESULT #%d %s cluster=%016llx defl=%.0f v=%d km/h: %s (%s) ticks=%ld wall_hits=%ld "
              "pinches=%ld stuck=%d stalls=%ld retries=%d rescues=%ld speed min/max near C %.0f/%.0f km/h",
              d->ci, k_rd_kind[c->kind], (unsigned long long)c->cl, c->defl, sp ? 80 : 40, ok ? "EXIT" : "NO EXIT", why,
              tick - d->ct, hits, pin, stuck, d->stalls, d->retries, resc, d->vmin > 1e8 ? 0.0 : d->vmin, d->vmax);
    if (d->rep)
        fprintf(d->rep, "%d\t%s\t%016llx\t%.0f\t%d\t%s\t%s\t%ld\t%ld\t%ld\t%d\t%ld\t%d\t%ld\t%.0f\t%.0f\n",
                d->ci, k_rd_kind[c->kind], (unsigned long long)c->cl, c->defl, sp ? 80 : 40, ok ? "EXIT" : "NO_EXIT", why,
                tick - d->ct, hits, pin, stuck, d->stalls, d->retries, resc, d->vmin > 1e8 ? 0.0 : d->vmin, d->vmax);
    d->phase = 0;
    if (++d->ci >= d->nc) { d->ci = 0; d->si++; }
}

static uint32_t rd_corners(struct RoamDrive *d, TD5_Actor *a, long tick)
{
    const RdCorner *c;
    double px, pz, kmh, look, rx, rz, along_out, lat_out, tx, tz;
    uint32_t bits;
    if (d->phase == 0) {
        if (d->si >= 2 || d->nc == 0) {
            int k, sp;
            TD5_LOG_I(LOG_TAG, "[ROAMCORNER] SUMMARY runs=%ld exit=%ld stuck=%ld stalls=%ld retries=%ld pinches=%ld wall_hits=%ld rescues=%ld",
                      d->runs, d->ok, d->stuck, d->tot_stalls, d->tot_retries, d->pinch, d->hits, d->resc);
            if (d->rep) {
                fprintf(d->rep, "\nSUMMARY runs=%ld exit=%ld stuck=%ld stalls=%ld retries=%ld pinches=%ld wall_hits=%ld rescues=%ld\n",
                        d->runs, d->ok, d->stuck, d->tot_stalls, d->tot_retries, d->pinch, d->hits, d->resc);
                for (k = 0; k < 4; k++)
                    for (sp = 0; sp < 2; sp++)
                        fprintf(d->rep, "kind %-20s %2d km/h: runs %ld exit %ld stuck %ld stalls %ld retries %ld pinches %ld wall_hits %ld rescues %ld\n",
                                k_rd_kind[k], sp ? 80 : 40, d->k_runs[k][sp], d->k_ok[k][sp], d->k_stuck[k][sp],
                                d->k_stalls[k][sp], d->k_retries[k][sp], d->k_pinch[k][sp], d->k_hits[k][sp], d->k_resc[k][sp]);
                fclose(d->rep);
                d->rep = NULL;
            }
            rd_finish(d, "corner matrix done");
            return 0;
        }
        rd_corner_start(d, a, tick);
        return (uint32_t)TD5_INPUT_THROTTLE;
    }
    c = &d->c[d->ci];
    px = (double)FP_TRUNC(a->world_pos.x);
    pz = (double)FP_TRUNC(a->world_pos.z);
    kmh = rd_speed_upt(a) / RD_UPT_KMH;
    rx = px - c->cx; rz = pz - c->cz;
    if (hypot(rx, rz) < 20.0 * RD_M) {
        if (kmh < d->vmin) d->vmin = kmh;
        if (kmh > d->vmax) d->vmax = kmh;
    }
    /* pure pursuit on the polyline: arc length of the car's nearest point, aim `look` further */
    look = (d->si ? 16.0 : 10.0) * RD_M;
    {
        double best = 1e30, cum = 0.0, total = 0.0, s_car = 0.0, want;
        int k;
        for (k = 0; k < RD_NP - 1; k++) {
            const double sx = c->px[k + 1] - c->px[k], sz = c->pz[k + 1] - c->pz[k];
            const double len = sqrt(sx * sx + sz * sz);
            double tt = len > 0.0 ? ((px - c->px[k]) * sx + (pz - c->pz[k]) * sz) / (len * len) : 0.0, qx, qz, dd;
            if (tt < 0.0) tt = 0.0;
            if (tt > 1.0) tt = 1.0;
            qx = c->px[k] + sx * tt; qz = c->pz[k] + sz * tt;
            dd = (px - qx) * (px - qx) + (pz - qz) * (pz - qz);
            if (dd < best) { best = dd; s_car = cum + tt * len; }
            cum += len;
        }
        total = cum;
        want = s_car + look;
        if (want > total) want = total + look;           /* past the end: keep going straight out */
        cum = 0.0;
        tx = c->px[RD_NP - 1] + c->outx * (want - total);
        tz = c->pz[RD_NP - 1] + c->outz * (want - total);
        for (k = 0; k < RD_NP - 1; k++) {
            const double sx = c->px[k + 1] - c->px[k], sz = c->pz[k + 1] - c->pz[k];
            const double len = sqrt(sx * sx + sz * sz);
            if (want <= cum + len && len > 0.0) {
                tx = c->px[k] + sx * (want - cum) / len;
                tz = c->pz[k] + sz * (want - cum) / len;
                break;
            }
            cum += len;
        }
        along_out = s_car - (total - 40.0 * RD_M);       /* > 0 once on the exit ribbon past its end row */
        lat_out = sqrt(best);
    }
    bits = rd_steer_to(a, tx, tz, d->si ? 80.0 : 40.0);
    if (d->rev_t > 0) {
        /* reverse-and-retry, as a driver would: back off with the wheel turned away */
        const uint32_t steer = bits & (uint32_t)(TD5_INPUT_STEER_LEFT | TD5_INPUT_STEER_RIGHT);
        d->rev_t--;
        bits = (uint32_t)TD5_INPUT_BRAKE;
        if (steer & TD5_INPUT_STEER_LEFT) bits |= (uint32_t)TD5_INPUT_STEER_RIGHT;
        else if (steer & TD5_INPUT_STEER_RIGHT) bits |= (uint32_t)TD5_INPUT_STEER_LEFT;
        d->stuck_t = 0;
        d->slow_t = 0;
    } else {
        /* the spec's stall metric: < 1 km/h for > 2 s with the throttle on (counted once per stall) */
        if ((bits & TD5_INPUT_THROTTLE) && kmh < 1.0) {
            if (++d->stuck_t == 61) d->stalls++;
        } else d->stuck_t = 0;
        if ((bits & TD5_INPUT_THROTTLE) && kmh < 3.0) d->slow_t++;
        else d->slow_t = 0;
        if (d->slow_t > 45 && d->retries < 2) { d->retries++; d->rev_t = 40; d->slow_t = 0; }
    }
    if (along_out > 30.0 * RD_M && lat_out < 6.0 * RD_M) rd_corner_end(d, tick, 1, 0, "exit reached");
    else if (d->retries >= 2 && d->slow_t > 60) rd_corner_end(d, tick, 0, 1, "stuck after 2 retries");
    else if (tick - d->ct > 20 * 30) rd_corner_end(d, tick, 0, 0, "timeout");
    return bits;
}

uint32_t td5_roam_dev_drive_bits(TD5_Actor *a)
{
    struct RoamDrive *d;
    long tick;
    if (s_rp.drv_mode < 0) {
        const char *m = getenv("TD5RE_ROAM_DRIVE");
        s_rp.drv_mode = !m ? 0 : (strcmp(m, "street") == 0 ? 1 : (strcmp(m, "corners") == 0 ? 2 : 0));
    }
    if (!s_rp.drv_mode || !a || !td5_roam_active() || !s_rp.loaded) return 0;
    if (!s_rp.drv) {
        char path[512];
        const char *dir = td5_plat_log_dir();
        s_rp.drv = (struct RoamDrive *)calloc(1, sizeof(struct RoamDrive));
        if (!(d = s_rp.drv)) return 0;
        d->kmh = (double)td5_env_int("TD5RE_ROAM_DRIVE_KMH", 50, 5, 200);
        d->secs = td5_env_int("TD5RE_ROAM_DRIVE_SECS", 60, 1, 3600);
        d->quit = td5_env_int("TD5RE_ROAM_DRIVE_QUIT", 1, 0, 1);
        d->per_kind = td5_env_int("TD5RE_ROAM_CORNERS_PER_KIND", 10, 1, 16);
        d->t0 = g_td5.simulation_tick_counter;
        d->log_t = d->t0;
        if (s_rp.drv_mode == 2) {
            rd_pick_corners(d);
            snprintf(path, sizeof path, "%sroam_corner_test.txt", dir ? dir : "");
            d->rep = fopen(path, "w");
            if (d->rep)
                fprintf(d->rep, "idx\tkind\tcluster\tdefl\tkmh\tresult\twhy\tticks\twall_hits\tpinches\tstuck\tstalls\tretries\trescues\tvmin\tvmax\n");
        }
        TD5_LOG_W(LOG_TAG, "[ROAMDRIVE] dev drive harness: %s (kmh=%.0f secs=%d quit=%d)",
                  s_rp.drv_mode == 1 ? "street" : "corners", d->kmh, d->secs, d->quit);
    }
    d = s_rp.drv;
    if (d->phase == 99) return 0;
    tick = g_td5.simulation_tick_counter;
    if (tick - d->t0 < 15) return 0;          /* let the spawn settle */
    return s_rp.drv_mode == 1 ? rd_street(d, a, tick) : rd_corners(d, a, tick);
}
#endif
