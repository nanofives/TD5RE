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
    long                summary_tick;
#ifndef TD5RE_RELEASE
    struct {
        int      mode;                      /* -1 unread, 0 off, 1 street, 2 corners  */
        double   kmh;
        int      secs, quit;
        long     t;
        double   dir_x, dir_z;              /* the street direction being followed    */
        int      have_dir;
        double   lat_target;
        long     stuck_ticks, stuck_events;
        double   max_above_rest, min_below;  /* m: chassis wheel ground gaps          */
        long     below_ticks, rest_ticks;
        double   dist_m;
        int32_t  last_x, last_z;
        /* corner matrix */
        int      ci, cn, phase;
        long     ct;
        long     c_pinch0, c_hits0, c_stuck;
        int      c_done;
        int      c_kind[64]; int32_t c_cx[64], c_cz[64]; int16_t c_in[64], c_out[64];
        int      c_speed_idx;
        double   c_loss;
        FILE    *rep;
    } drv;
#endif
} RoamPhys;

static RoamPhys s_rp = { .mirror_knob = -1
#ifndef TD5RE_RELEASE
    , .drv = { .mode = -1 }
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
    if (i == 0 && a->slot_index == 0) {
        s_rp.st.ticks++;
        if (++s_rp.summary_tick % 300 == 0) rp_write_summary("periodic");
    }
    ok = td5_roam_locate(rs, x, z, y, 1, ss->hint[4 + i], &h);

    if (!td5_roam_active()) {
        /* MIRROR: only a probe on a type-1 span the strip would also be on is compared */
        int dh;
        if (!ok || h.prim == ROAM_PRIM_NONE || !h.inside) { s_rp.st.mirror_fallback++; return 0; }
        surf = rp_surface(rs, &h);
        s_rp.st.mirror_cmp++;
        dh = abs(h.ground_y - *ground_y);
        if (dh) { s_rp.st.mirror_diff_h++; if (dh > s_rp.st.mirror_dh_max) s_rp.st.mirror_dh_max = dh; }
        /* the call site normalises the strip's normal after this call; the raw
         * triangle normal is what both sides hand it */
        if (h.normal[0] != normal[0] || h.normal[1] != normal[1] || h.normal[2] != normal[2])
            s_rp.st.mirror_diff_n++;
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
        normal[0] = 0; normal[1] = 4096; normal[2] = 0;
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
        if (h.ground_y != *y_io) s_rp.st.mirror_corner_diff++;
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
        ss->chassis = h;
        ss->chassis_ok = 1;
        ss->chassis_hint = h.prim;
        td5_roam_set_slot_edge((int)a->slot_index, h.edge_key);
    } else {
        ss->chassis_ok = 0;
    }
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
        ss->chassis_hint = h.prim;
        s_rp.st.mirror_head_cmp++;
        if (a->heading_normal.x != h.normal[0] || a->heading_normal.y != h.normal[1] ||
            a->heading_normal.z != h.normal[2])
            s_rp.st.mirror_head_diff++;
    } else {
        if (!ss->chassis_ok) {
            if (a->heading_normal.y == 0) { a->heading_normal.x = 0; a->heading_normal.y = 4096; a->heading_normal.z = 0; }
            return 0;
        }
        h = ss->chassis;
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
    if (normal) { normal[0] = 0; normal[1] = 4096; normal[2] = 0; }
    s_rp.last_capped = 0;
    if (!rs || !td5_roam_locate(rs, x, z, y, has_y, ROAM_PRIM_NONE, &h) || h.prim == ROAM_PRIM_NONE)
        return 0;
    if (normal) { normal[0] = h.normal[0]; normal[1] = h.normal[1]; normal[2] = h.normal[2]; }
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
            if (s_rp.st.pinches <= 500)
                TD5_LOG_W(LOG_TAG, "ROAM_WALL_PINCH slot=%d tick=%ld corners %d/%d edges %016llx/%016llx walls %u/%u "
                          "dist %d/%d pos=(%d,%d) speed=%.0f",
                          (int)a->slot_index, s_rp.st.ticks, pa, pb,
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
                          (int)a->slot_index, s_rp.st.ticks, deep, (unsigned long long)h->edge_key,
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
                          (int)a->slot_index, s_rp.st.ticks, (int)FP_TRUNC(a->world_pos.x), (int)FP_TRUNC(a->world_pos.z),
                          (int)FP_TRUNC(ss->good_x), (int)FP_TRUNC(ss->good_y), (int)FP_TRUNC(ss->good_z));
                a->world_pos.x = ss->good_x; a->world_pos.y = ss->good_y; a->world_pos.z = ss->good_z;
                a->linear_velocity_x = a->linear_velocity_y = a->linear_velocity_z = 0;
                td5_physics_rebuild_pose(a);
            }
            return;
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
                          (int)a->slot_index, s_rp.st.ticks, (int)c.push_dist, (unsigned long long)c.edge_key,
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
                      (int)a->slot_index, s_rp.st.ticks, (int)FP_TRUNC(a->world_pos.y),
                      (int)FP_TRUNC(c.ground_y - a->world_pos.y), (int)FP_TRUNC(c.ground_y));
            a->world_pos.y = c.ground_y + 215 * 256;
            if (a->linear_velocity_y < 0) a->linear_velocity_y = 0;
            td5_physics_rebuild_pose(a);
        } else if (c.inside && (a->wheel_contact_bitmask & 0x0F) != 0x0F) {
            ss->good_x = a->world_pos.x; ss->good_y = a->world_pos.y; ss->good_z = a->world_pos.z;
            ss->good_valid = 1;
        }
    }
}

/* ============================================================================
 * SECTION: dev drive harness (DEV only)
 * ========================================================================== */
#ifndef TD5RE_RELEASE
uint32_t td5_roam_dev_drive_bits(TD5_Actor *a)
{
    (void)a;
    return 0;
}
#endif
