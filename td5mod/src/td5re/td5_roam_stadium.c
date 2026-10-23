/**
 * td5_roam_stadium.c -- GEO FREE ROAM: STADIUMS and SPORTS PITCHES of one tile (PORT-ONLY, roam PB).
 * Contract, kinds and what is left out: td5_roam_stadium.h.
 *
 * SECTIONS
 *   codes     sport= / surface= -> TD5_RPS_* / TD5_RPF_*, the default surface of a sport
 *   writers   per (sub-block, page) quad batches -> one MODELS.DAT prefab record each, + the guard audit
 *   geometry  ring helpers: closing point, minimum-area box, ray to a ring, chain resampling
 *   stands    the bowl (a ring stand round the pitch, its roof) and the open stand (front / back chains)
 *   pitches   surface, lines, goals / nets / hoops per sport
 *   emit      the stadiums and pitches the tile owns
 *
 * Every decision keys off the record (its id, outline, tags), never off the order tiles are built in.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "td5_trackgen_internal.h"   /* TG_Buf, tg_write_prefab_mesh, TD5_TG_PAGE_*  */
#include "td5_geo_buildings.h"       /* td5_geob_point_in_ring, td5_geob_ring_area    */
#include "td5_config.h"
#include "td5_platform.h"
#include "td5_roam_scenery.h"
#include "td5_roam_stadium.h"

#ifdef LOG_TAG
#undef LOG_TAG
#endif
#define LOG_TAG "roam"

#define SP_UPM        430.0
#define SP_M(m)       ((m) * SP_UPM)
#define SP_CHUNK      128            /* quads per record                                  */
#define SP_SINK       SP_M(0.5)      /* walls start this far under the ground             */
#define SP_GUARD_LO   SP_M(0.5)
#define SP_GUARD_HI   SP_M(2.5)
#define SP_CLEAR      SP_M(4.5)      /* a wall over a drivable way opens this high over it */
#define SP_SMALL_M2   150.0          /* pitches under this are left out                   */
#define SP_RING_MAX   64
#define SP_PI         3.14159265358979323846

/* lifts over the lawn / ground under a pitch: the surface, an inner court patch, the paint */
/* the pitch's own lawn is not drawn (td5_roam_pitch_paints), but a park lawn round it can lie under it: 10 cm
 * clears it (3 cm let the park's grass through at 25 m); the paint 12 cm over the surface */
#define SP_LIFT_SURF  SP_M(0.10)
#define SP_LIFT_COURT SP_M(0.14)
#define SP_LIFT_LINE  SP_M(0.22)

enum { SP_PAINT = 0, SP_GRASS, SP_STONE, SP_METAL, SP_N };

/* ARGB, baked per vertex (then shaded by the face's facing, sp_shade) */
#define SC_LINE      0xFFF2F2EEu
#define SC_CONCRETE  0xFFB4B0A8u
#define SC_CONC_D    0xFF8C8882u
#define SC_CONC_M    0xFFA6A29Au
#define SC_GLASS     0xFF2E3A46u
#define SC_PORTAL    0xFF20242Au
#define SC_MEMBRANE  0xFFF4F4F0u
#define SC_STEEL     0xFFC8CCD0u
#define SC_STEEL_D   0xFF50565Cu
#define SC_FACADE    0xFFE2E0DAu
#define SC_FACADE_D  0xFFCCCAC4u
#define SC_NET       0xFF2A2C2Eu
#define SC_GRASS_L   0xFFE8F4DCu
#define SC_GRASS_D   0xFFB4CAA4u
#define SC_CLAY      0xFFC0643Eu
#define SC_HARD      0xFF8E9290u
#define SC_ACRYL_S   0xFF3F8050u
#define SC_ACRYL_C   0xFF3A6EA8u
#define SC_TURF_L    0xFF44A052u
#define SC_TURF_D    0xFF388C46u
#define SC_SAND      0xFFD8C08Cu
#define SC_ORANGE    0xFFE06A20u

int td5_roam_stadium_enabled(void) { return td5_env_int("TD5RE_ROAM_STADIUM", 1, 0, 1); }
int td5_roam_pitch_enabled(void)   { return td5_env_int("TD5RE_ROAM_PITCH_LINES", 1, 0, 1); }

/* ============================================================================
 * SECTION: codes
 * ========================================================================== */

/* the first token of a ;-list, compared up to its end */
static int sp_tok_is(const char *s, const char *v)
{
    const size_t n = strlen(v);
    return s && !strncmp(s, v, n) && (s[n] == '\0' || s[n] == ';');
}

int td5_roam_sport_of(const char *s)
{
    if (!s || !s[0]) return TD5_RPS_NONE;
    if (sp_tok_is(s, "soccer") || sp_tok_is(s, "football") || sp_tok_is(s, "futsal")) return TD5_RPS_SOCCER;
    if (sp_tok_is(s, "tennis")) return TD5_RPS_TENNIS;
    if (sp_tok_is(s, "padel") || sp_tok_is(s, "paddle_tennis")) return TD5_RPS_PADEL;
    if (sp_tok_is(s, "basketball") || sp_tok_is(s, "multi")) return TD5_RPS_BASKET;
    if (sp_tok_is(s, "volleyball")) return TD5_RPS_VOLLEY;
    if (sp_tok_is(s, "beachvolleyball")) return TD5_RPS_BEACH;
    if (sp_tok_is(s, "field_hockey") || sp_tok_is(s, "hockey")) return TD5_RPS_HOCKEY;
    if (sp_tok_is(s, "rugby_union") || sp_tok_is(s, "rugby_league") || sp_tok_is(s, "rugby")) return TD5_RPS_RUGBY;
    return TD5_RPS_OTHER;     /* athletics, equestrian, skateboard, bowls, pelota, baseball: left out */
}

int td5_roam_surface_of(const char *s)
{
    if (!s || !s[0]) return TD5_RPF_NONE;
    if (sp_tok_is(s, "grass") || sp_tok_is(s, "grass_paver")) return TD5_RPF_GRASS;
    if (sp_tok_is(s, "clay")) return TD5_RPF_CLAY;
    if (sp_tok_is(s, "acrylic") || sp_tok_is(s, "decoturf")) return TD5_RPF_ACRYLIC;
    if (sp_tok_is(s, "artificial_turf")) return TD5_RPF_TURF;
    if (sp_tok_is(s, "sand")) return TD5_RPF_SAND;
    if (sp_tok_is(s, "concrete") || sp_tok_is(s, "cement") || sp_tok_is(s, "asphalt") || sp_tok_is(s, "paved") ||
        sp_tok_is(s, "paving_stones") || sp_tok_is(s, "compacted") || sp_tok_is(s, "tartan") || sp_tok_is(s, "dirt"))
        return TD5_RPF_HARD;
    return TD5_RPF_OTHER;
}

/* the surface a sport is laid on when the pitch carries no surface= (La Plata: 81 of the 94 tennis courts with a
 * surface are clay, every surfaced soccer pitch but 7 is grass) */
static int sp_default_surface(int sport)
{
    switch (sport) {
    case TD5_RPS_SOCCER: case TD5_RPS_RUGBY: return TD5_RPF_GRASS;
    case TD5_RPS_TENNIS: return TD5_RPF_CLAY;
    case TD5_RPS_PADEL:  return TD5_RPF_ACRYLIC;
    case TD5_RPS_HOCKEY: return TD5_RPF_TURF;
    case TD5_RPS_BEACH:  return TD5_RPF_SAND;
    case TD5_RPS_BASKET: case TD5_RPS_VOLLEY: return TD5_RPF_HARD;
    default: return TD5_RPF_NONE;
    }
}

/* ============================================================================
 * SECTION: writers
 * ========================================================================== */

typedef struct SpBatch { int nq; float v[SP_CHUNK * 4 * 5]; unsigned int light[SP_CHUNK * 4]; } SpBatch;

typedef struct {
    TG_TileCtx *c;
    TD5_RoamSportStats *st;
    SpBatch *bat;
    int oom;
    int nofix_sb;          /* sub-block forced for the current piece (-1 = by each quad's centre) */
    int line_quads;        /* counting the paint                                                 */
    int foot;              /* [roam QB] TD5_RSB_* of the wall being laid on the ground (-1 = not a footing) */
} SpEm;

static int sp_page(int pg)
{
    switch (pg) {
    case SP_PAINT: return TD5_TG_PAGE_SNOW;            /* near-white: every colour tints it (td5_roam_shops.c) */
    case SP_GRASS: return TD5_TG_PAGE_R3_BLOCK + 0;    /* the lawn's own page                                  */
    case SP_STONE: return TD5_TG_PAGE_R4_PIER;         /* smooth cast stone                                    */
    default:       return TD5_TG_PAGE_R11_SIGN_POST;   /* brushed metal                                        */
    }
}

static int sp_flush(SpEm *e, int sb, int pg)
{
    SpBatch *q = &e->bat[sb * SP_N + pg];
    unsigned short cmd[3];
    TG_Buf *b;
    if (q->nq <= 0) return 1;
    b = (TG_Buf *)roam_ctx_mesh_begin(e->c, sb);
    if (!b) { e->oom = 1; return 0; }
    cmd[0] = (unsigned short)sp_page(pg); cmd[1] = 0; cmd[2] = (unsigned short)q->nq;
    if (!tg_write_prefab_mesh(b, q->v, q->light, q->nq * 4, cmd, 1, 0, 0.0, 0.0, 0.0, 1.0, 0.0) || b->oom) { e->oom = 1; return 0; }
    e->st->meshes++;
    q->nq = 0;
    return 1;
}

/* the guard audit of one face (td5_roam_plaza.c pz_face_on_road) */
static int sp_face_on_road(SpEm *e, const double *x, const double *y, const double *z)
{
    double fx[4], fz[4], ymin = y[0], ymax = y[0];
    int m = 0, k, a;
    for (k = 0; k < 4; k++) {
        if (y[k] < ymin) ymin = y[k];
        if (y[k] > ymax) ymax = y[k];
        for (a = 0; a < m; a++) if (fabs(fx[a] - x[k]) < 1.0 && fabs(fz[a] - z[k]) < 1.0) break;
        if (a == m) { fx[m] = x[k]; fz[m] = z[k]; m++; }
    }
    if (m >= 3) {
        double ar = 0.0;
        int q;
        for (q = 0; q < m; q++) { const int q2 = (q + 1) % m; ar += fx[q] * fz[q2] - fx[q2] * fz[q]; }
        if (fabs(0.5 * ar) < 4.0) {
            int i0 = 0, i1 = 1, i, j;
            double bd = -1.0;
            for (i = 0; i < m; i++) for (j = i + 1; j < m; j++) {
                const double d = hypot(fx[i] - fx[j], fz[i] - fz[j]);
                if (d > bd) { bd = d; i0 = i; i1 = j; }
            }
            fx[0] = fx[i0]; fz[0] = fz[i0]; fx[1] = fx[i1]; fz[1] = fz[i1];
            m = 2;
        }
    }
    if (m < 2) return 0;
    return roam_ctx_on_road_band(e->c, fx, fz, m, ymin - SP_GUARD_LO, ymax + SP_GUARD_HI);
}

/* a colour darkened by how the face turns from a fixed sun: tiers and walls read as solids */
static unsigned int sp_shade(unsigned int col, const double *x, const double *y, const double *z)
{
    const double ax = x[1] - x[0], ay = y[1] - y[0], az = z[1] - z[0];
    const double bx = x[3] - x[0], by = y[3] - y[0], bz = z[3] - z[0];
    double nx = ay * bz - az * by, ny = az * bx - ax * bz, nz = ax * by - ay * bx, L = sqrt(nx * nx + ny * ny + nz * nz);
    double f;
    unsigned int r, g, b;
    if (L < 1e-9) return col;
    nx /= L; ny /= L; nz /= L;
    f = 0.60 + 0.40 * fabs(nx * 0.42 + ny * 0.80 + nz * 0.43);
    if (f > 1.0) f = 1.0;
    r = (unsigned int)(((col >> 16) & 0xFFu) * f + 0.5);
    g = (unsigned int)(((col >> 8) & 0xFFu) * f + 0.5);
    b = (unsigned int)((col & 0xFFu) * f + 0.5);
    return (col & 0xFF000000u) | (r << 16) | (g << 8) | b;
}

/* one quad (tile-local x/z, absolute y); UVs from the position, one repeat per `tm` metres */
static int sp_quad(SpEm *e, int pg, const double *x, const double *y, const double *z, unsigned int col, double tm, int shade)
{
    SpBatch *q;
    double u[4], v[4];
    int k, sb, flat;
    const double t = SP_M(tm);
    if (e->oom) return 0;
    if (sp_face_on_road(e, x, y, z)) {
        e->st->guard_hits++;
        if (e->st->guard_hits <= 3)
            TD5_LOG_W(LOG_TAG, "ROAM_SPORT_GUARD page %d face at local (%.0f,%.0f) y %.0f on the drivable surface",
                      sp_page(pg), x[0], z[0], y[0]);
    }
    sb = e->nofix_sb >= 0 ? e->nofix_sb : roam_ctx_sb_of(e->c, 0.25 * (x[0] + x[1] + x[2] + x[3]), 0.25 * (z[0] + z[1] + z[2] + z[3]));
    q = &e->bat[sb * SP_N + pg];
    if (q->nq >= SP_CHUNK && !sp_flush(e, sb, pg)) return 0;
    flat = fabs(y[0] - y[2]) + fabs(y[1] - y[3]) < 0.25 * (hypot(x[2] - x[0], z[2] - z[0]) + hypot(x[3] - x[1], z[3] - z[1]));
    for (k = 0; k < 4; k++) {
        if (flat) { u[k] = x[k] / t; v[k] = z[k] / t; }
        else      { u[k] = (x[k] + z[k]) / t; v[k] = -y[k] / t; }
    }
    if (shade) col = sp_shade(col, x, y, z);
    for (k = 0; k < 4; k++) {
        const int o = (q->nq * 4 + k) * 5;
        q->v[o + 0] = (float)x[k]; q->v[o + 1] = (float)y[k]; q->v[o + 2] = (float)z[k];
        q->v[o + 3] = (float)u[k]; q->v[o + 4] = (float)v[k];
        q->light[q->nq * 4 + k] = col;
    }
    q->nq++;
    return 1;
}

/* a vertical wall quad from (ax, az) to (bx, bz), bottoms ya0 / yb0, tops ya1 / yb1 */
/* [roam QB] the base census of one footing: its bottom edge against the drawn ground at both ends and the middle */
static void sp_foot_census(SpEm *e, double ax, double az, double bx, double bz, double ya0, double yb0)
{
    int k;
    const int f = e->foot;
    if (f < 0 || f >= TD5_RSB__N) return;
    for (k = 0; k < 3; k++) {
        const double t = 0.5 * (double)k;
        const double x = ax + (bx - ax) * t, z = az + (bz - az) * t, y = ya0 + (yb0 - ya0) * t;
        double g = (y - roam_ctx_drawn_ground(e->c, x, z)) / SP_UPM;
        if (g < 0.0) g = 0.0;
        e->st->foot_n[f]++;
        e->st->foot_sum[f] += g;
        if (g > e->st->foot_max[f]) e->st->foot_max[f] = g;
        if (g > 0.25) e->st->foot_over[f]++;
    }
}

static int sp_wall(SpEm *e, int pg, double ax, double az, double bx, double bz, double ya0, double yb0, double ya1, double yb1,
                   unsigned int col)
{
    const double fx[2] = { ax, bx }, fz[2] = { az, bz };
    const double lo = ya0 < yb0 ? ya0 : yb0, hi = ya1 < yb1 ? ya1 : yb1;
    if (e->foot == TD5_RSB_OUTER && getenv("TD5RE_QB_DBG")) {
        double ry = 0.0;
        const int anyr = roam_ctx_on_road_band(e->c, fx, fz, 2, -1e30, 1e30);
        const int hr = roam_ctx_road_y(e->c, 0.5 * (ax + bx), 0.5 * (az + bz), lo, &ry);
        fprintf(stderr, "QBDBG outer (%.0f,%.0f) on_road_any %d road_y_hit %d road-lo %.2f m dg-lo %.2f m\n", 0.5 * (ax + bx), 0.5 * (az + bz),
                anyr, hr, hr ? (ry - lo) / SP_UPM : 0.0, (roam_ctx_drawn_ground(e->c, 0.5 * (ax + bx), 0.5 * (az + bz)) - lo) / SP_UPM);
    }
    if (roam_ctx_on_road_band(e->c, fx, fz, 2, lo - SP_GUARD_LO, lo + SP_GUARD_HI)) {
        /* a wall standing on a drivable way (the Unico's service drives run under its end stands): an opening, the
         * wall's bottom SP_CLEAR over the road */
        double ry;
        if (!roam_ctx_road_y(e->c, 0.5 * (ax + bx), 0.5 * (az + bz), lo, &ry)) ry = lo;
        e->st->openings++;
        if (e->foot >= 0) { e->st->open_n++; e->st->open_m += hypot(bx - ax, bz - az) / SP_UPM; }
        if (ry + SP_CLEAR >= hi) return 1;
        ya0 = yb0 = ry + SP_CLEAR;
    }
    sp_foot_census(e, ax, az, bx, bz, ya0, yb0);
    {
        const double x[4] = { ax, bx, bx, ax }, y[4] = { ya0, yb0, yb1, ya1 }, z[4] = { az, bz, bz, az };
        return sp_quad(e, pg, x, y, z, col, 2.0, 1);
    }
}

/* a box: centre (cx, cz), y0..y1, half sizes hx along (ux, uz) and hz across; sides + top */
static int sp_box(SpEm *e, int pg, double cx, double cz, double y0, double y1, double hx, double hz, double ux, double uz,
                  unsigned int col)
{
    const double wx = -uz, wz = ux;
    double X[4], Z[4];
    int k;
    X[0] = cx - ux * hx - wx * hz; Z[0] = cz - uz * hx - wz * hz;
    X[1] = cx + ux * hx - wx * hz; Z[1] = cz + uz * hx - wz * hz;
    X[2] = cx + ux * hx + wx * hz; Z[2] = cz + uz * hx + wz * hz;
    X[3] = cx - ux * hx + wx * hz; Z[3] = cz - uz * hx + wz * hz;
    for (k = 0; k < 4; k++) {
        const int j = (k + 1) & 3;
        if (!sp_wall(e, pg, X[k], Z[k], X[j], Z[j], y0, y0, y1, y1, col)) return 0;
    }
    {
        const double y[4] = { y1, y1, y1, y1 };
        return sp_quad(e, pg, X, y, Z, col, 1.0, 1);
    }
}

/* a bar between two 3D points, square section 2h, as 4 long faces */
static int sp_bar(SpEm *e, int pg, double ax, double ay, double az, double bx, double by, double bz, double h, unsigned int col)
{
    double dx = bx - ax, dy = by - ay, dz = bz - az, L = sqrt(dx * dx + dy * dy + dz * dz);
    double p[3], q[3], ox[4], oy[4], oz[4];
    int k;
    if (L < 1.0) return 1;
    dx /= L; dy /= L; dz /= L;
    /* p: horizontal perpendicular (or x when vertical), q = d x p */
    if (fabs(dy) > 0.95) { p[0] = 1.0; p[1] = 0.0; p[2] = 0.0; }
    else { const double hl = hypot(dx, dz); p[0] = -dz / hl; p[1] = 0.0; p[2] = dx / hl; }
    q[0] = dy * p[2] - dz * p[1]; q[1] = dz * p[0] - dx * p[2]; q[2] = dx * p[1] - dy * p[0];
    for (k = 0; k < 4; k++) {
        const double sa = (k == 0 || k == 3) ? -h : h, sb = (k < 2) ? -h : h;
        ox[k] = sa * p[0] + sb * q[0]; oy[k] = sa * p[1] + sb * q[1]; oz[k] = sa * p[2] + sb * q[2];
    }
    for (k = 0; k < 4; k++) {
        const int j = (k + 1) & 3;
        const double x[4] = { ax + ox[k], ax + ox[j], bx + ox[j], bx + ox[k] };
        const double y[4] = { ay + oy[k], ay + oy[j], by + oy[j], by + oy[k] };
        const double z[4] = { az + oz[k], az + oz[j], bz + oz[j], bz + oz[k] };
        if (!sp_quad(e, pg, x, y, z, col, 1.0, 1)) return 0;
    }
    return 1;
}

/* the base height under (x, z): the lawn, else the pavement slab, else the drawn ground (td5_roam_plaza.c pz_base) */
static double sp_base(TG_TileCtx *c, double x, double z)
{
    double y;
    if (roam_ctx_lawn_y(c, x, z, &y)) return y;
    if (roam_ctx_slab_y(c, x, z, &y)) return y;
    return roam_ctx_ground(c, x, z);
}

/* ============================================================================
 * SECTION: geometry
 * ========================================================================== */

/* copy a ring without its closing point; returns the count (0 when < 3) */
static int sp_ring(const double *px, const double *pz, int n, double *x, double *z)
{
    int k;
    if (n > SP_RING_MAX) n = SP_RING_MAX;
    for (k = 0; k < n; k++) { x[k] = px[k]; z[k] = pz[k]; }
    if (n > 3 && fabs(x[0] - x[n - 1]) < 1.0 && fabs(z[0] - z[n - 1]) < 1.0) n--;
    return n >= 3 ? n : 0;
}

/* the minimum-area box of a ring over its edge directions: centre, unit long axis, half length >= half width */
typedef struct { double cx, cz, ux, uz, hl, hw; } SpBox;

static int sp_obb(const double *x, const double *z, int n, SpBox *b)
{
    double best = -1.0;
    int i, k;
    for (i = 0; i < n; i++) {
        const int j = (i + 1) % n;
        double ux = x[j] - x[i], uz = z[j] - z[i], L = hypot(ux, uz);
        double s0 = 1e300, s1 = -1e300, t0 = 1e300, t1 = -1e300, ar;
        if (L < SP_M(0.5)) continue;
        ux /= L; uz /= L;
        for (k = 0; k < n; k++) {
            const double s = x[k] * ux + z[k] * uz, t = -x[k] * uz + z[k] * ux;
            if (s < s0) s0 = s;
            if (s > s1) s1 = s;
            if (t < t0) t0 = t;
            if (t > t1) t1 = t;
        }
        ar = (s1 - s0) * (t1 - t0);
        if (best < 0.0 || ar < best - 1e-6) {
            const double sm = 0.5 * (s0 + s1), tm = 0.5 * (t0 + t1);
            best = ar;
            b->cx = sm * ux - tm * uz; b->cz = sm * uz + tm * ux;
            if (s1 - s0 >= t1 - t0) { b->ux = ux; b->uz = uz; b->hl = 0.5 * (s1 - s0); b->hw = 0.5 * (t1 - t0); }
            else { b->ux = -uz; b->uz = ux; b->hl = 0.5 * (t1 - t0); b->hw = 0.5 * (s1 - s0); }
        }
    }
    return best > 0.0;
}

/* the farthest crossing of the ray (cx, cz) + t (dx, dz), t > 0, with a ring; -1 when none */
static double sp_ray(const double *x, const double *z, int n, double cx, double cz, double dx, double dz)
{
    double best = -1.0;
    int i;
    for (i = 0; i < n; i++) {
        const int j = (i + 1) % n;
        const double ex = x[j] - x[i], ez = z[j] - z[i];
        const double den = dx * ez - dz * ex;
        double t, s;
        if (fabs(den) < 1e-12) continue;
        t = ((x[i] - cx) * ez - (z[i] - cz) * ex) / den;
        s = ((x[i] - cx) * dz - (z[i] - cz) * dx) / den;
        if (t > 0.0 && s >= 0.0 && s <= 1.0 && t > best) best = t;
    }
    return best;
}

/* distance from a point to the box (0 inside) */
static double sp_box_dist(const SpBox *b, double x, double z)
{
    const double s = (x - b->cx) * b->ux + (z - b->cz) * b->uz, t = -(x - b->cx) * b->uz + (z - b->cz) * b->ux;
    const double ds = fabs(s) - b->hl, dt = fabs(t) - b->hw;
    return hypot(ds > 0.0 ? ds : 0.0, dt > 0.0 ? dt : 0.0);
}

/* a chain (cnt points from ring index i0, step +1 or -1, cyclic) resampled to m + 1 points by arc length */
static void sp_chain(const double *x, const double *z, int n, int i0, int cnt, int step, int m, double *ox, double *oz)
{
    double cum[SP_RING_MAX], tot;
    int k, j = 0;
    cum[0] = 0.0;
    for (k = 1; k < cnt; k++) {
        const int a = ((i0 + (k - 1) * step) % n + n) % n, b = ((i0 + k * step) % n + n) % n;
        cum[k] = cum[k - 1] + hypot(x[b] - x[a], z[b] - z[a]);
    }
    tot = cum[cnt - 1];
    for (k = 0; k <= m; k++) {
        const double want = tot * (double)k / (double)m;
        int a, b;
        double f;
        while (j < cnt - 2 && cum[j + 1] < want) j++;
        a = ((i0 + j * step) % n + n) % n; b = ((i0 + (j + 1) * step) % n + n) % n;
        f = (cum[j + 1] - cum[j]) > 1e-9 ? (want - cum[j]) / (cum[j + 1] - cum[j]) : 0.0;
        if (f < 0.0) f = 0.0;
        if (f > 1.0) f = 1.0;
        ox[k] = x[a] + (x[b] - x[a]) * f; oz[k] = z[a] + (z[b] - z[a]) * f;
    }
}

/* ============================================================================
 * SECTION: stands
 * ========================================================================== */

#define SP_BOWL_M      96         /* sectors round a bowl                 */
#define SP_STAND_MAXM  48
#define SP_TIER_MAX    64
#define SP_SEAT_M      0.5        /* seat width along a row (m): the seat census and the capacity rule */

typedef struct { unsigned int seat, seat2; int every; int capacity; } SpLook;

/* the seat colours: by stadium (the club colours of the two La Plata clubs), else a neutral grey-blue */
static SpLook sp_look(double osm_id)
{
    SpLook l;
    l.seat = 0xFF8C9AACu; l.seat2 = 0xFF8C9AACu; l.every = 0; l.capacity = 0;
    /* the Estadio Unico seats about 53,000 (a known fact, not OSM): the bowl gets as many rows as that takes */
    if (osm_id == 119937010.0) l.capacity = 53000;
    if (osm_id == 1392060296.0) { l.seat = 0xFF2A4C9Au; l.seat2 = 0xFFE8E8E8u; l.every = 4; }   /* Zerillo: Gimnasia   */
    if (osm_id == 3882033.0)    { l.seat = 0xFFB8282Cu; l.seat2 = 0xFFE8E8E8u; l.every = 4; }   /* Hirschi: Estudiantes */
    return l;
}

static unsigned int sp_row_col(const SpLook *l, int k)
{
    return (l->every > 0 && (k % l->every) == l->every - 1) ? l->seat2 : l->seat;
}

/* the rows of a stand between a front polyline F and a back polyline Bk (m + 1 points each, `closed` = a ring):
 * K tiers, row k tread at T[k] between fractions k/K and (k+1)/K, its riser at k/K from the previous tread;
 * kb = the row whose riser carries the band of boxes (glass) on top, -1 = none */
static int sp_tiers(SpEm *e, const double *fx, const double *fz, const double *bx, const double *bz, int m, int closed,
                    int K, const double *T, double y_front, int kb, double box_h, const SpLook *lk)
{
    int k, j;
    const int nj = closed ? m : m;   /* segments j .. j + 1, j < m (closed: point m == point 0) */
    for (k = 0; k < K; k++) {
        const double f0 = (double)k / (double)K, f1 = (double)(k + 1) / (double)K;
        const double ylo = k == 0 ? y_front : T[k - 1];
        for (j = 0; j < nj; j++) {
            const int j1 = j + 1;
            const double ax = fx[j] + (bx[j] - fx[j]) * f0, az = fz[j] + (bz[j] - fz[j]) * f0;
            const double cx = fx[j1] + (bx[j1] - fx[j1]) * f0, cz = fz[j1] + (bz[j1] - fz[j1]) * f0;
            const double ax1 = fx[j] + (bx[j] - fx[j]) * f1, az1 = fz[j] + (bz[j] - fz[j]) * f1;
            const double cx1 = fx[j1] + (bx[j1] - fx[j1]) * f1, cz1 = fz[j1] + (bz[j1] - fz[j1]) * f1;
            {   /* the riser */
                const double top = (k == kb) ? T[k] - box_h : T[k];
                if (!sp_wall(e, SP_PAINT, ax, az, cx, cz, ylo, ylo, top, top, SC_CONCRETE)) return 0;
                if (k == kb && !sp_wall(e, SP_PAINT, ax, az, cx, cz, top, top, T[k], T[k], SC_GLASS)) return 0;
            }
            {   /* the tread */
                const double x[4] = { ax, cx, cx1, ax1 }, y[4] = { T[k], T[k], T[k], T[k] }, z[4] = { az, cz, cz1, az1 };
                if (!sp_quad(e, SP_PAINT, x, y, z, sp_row_col(lk, k), 1.0, 1)) return 0;
            }
            e->st->tier_quads += 2;
        }
    }
    return 1;
}

/* portals: `np` dark doorways on the wall polyline wx/wz (m + 1 points), pushed `out` along the outward normals
 * (ox, oz per point), w x h metres, bottoms at the ground */
static int sp_portals(SpEm *e, const double *wx, const double *wz, const double *nx, const double *nz, int m, int np,
                      double w, double h)
{
    double cum[SP_BOWL_M + 1], tot;
    int k, p, j;
    cum[0] = 0.0;
    for (k = 1; k <= m; k++) cum[k] = cum[k - 1] + hypot(wx[k] - wx[k - 1], wz[k] - wz[k - 1]);
    tot = cum[m];
    if (np <= 0 || tot < SP_M(w * 2.0)) return 1;
    for (p = 0; p < np; p++) {
        const double at = tot * ((double)p + 0.5) / (double)np;
        double px, pz, tx, tz, L, ox, oz, gy;
        for (j = 0; j < m - 1 && cum[j + 1] < at; j++) { }
        {
            const double f = (cum[j + 1] - cum[j]) > 1e-9 ? (at - cum[j]) / (cum[j + 1] - cum[j]) : 0.0;
            px = wx[j] + (wx[j + 1] - wx[j]) * f; pz = wz[j] + (wz[j + 1] - wz[j]) * f;
            ox = nx[j] + (nx[j + 1] - nx[j]) * f; oz = nz[j] + (nz[j + 1] - nz[j]) * f;
        }
        tx = wx[j + 1] - wx[j]; tz = wz[j + 1] - wz[j]; L = hypot(tx, tz);
        if (L < 1e-6) continue;
        tx /= L; tz /= L;
        L = hypot(ox, oz);
        if (L < 1e-6) continue;
        ox = ox / L * SP_M(0.15); oz = oz / L * SP_M(0.15);
        gy = sp_base(e->c, px, pz);
        e->foot = TD5_RSB_PORTAL;
        if (!sp_wall(e, SP_PAINT, px + ox - tx * SP_M(w * 0.5), pz + oz - tz * SP_M(w * 0.5),
                     px + ox + tx * SP_M(w * 0.5), pz + oz + tz * SP_M(w * 0.5), gy, gy, gy + SP_M(h), gy + SP_M(h), SC_PORTAL))
            return 0;
        e->foot = -1;
        e->st->portals++;
    }
    return 1;
}

/* THE BOWL: a ring stand (outline with a courtyard) round a pitch. Sectors by angle from the pitch centre: the
 * front line (the pitch box + 7 m, corners rounded 14 m), the outline (the back of the stands, the outer wall)
 * and the courtyard ring (the roof's inner edge). */
static int sp_bowl(SpEm *e, const TD5_RoamStadium *sd, const TD5_RoamStand *s, const SpBox *pb, const double *ox, const double *oz,
                   int on, const double *hx, const double *hz, int hn)
{
    double fx[SP_BOWL_M + 1], fz[SP_BOWL_M + 1], bx[SP_BOWL_M + 1], bz[SP_BOWL_M + 1], ix[SP_BOWL_M + 1], iz[SP_BOWL_M + 1];
    double nx[SP_BOWL_M + 1], nz[SP_BOWL_M + 1], gy[SP_BOWL_M + 1], T[SP_TIER_MAX];
    double rx[40], rz[40], depth = 0.0, B, Ht, top, y0, rise, box_h = SP_M(2.4), yr_o, yr_i, row_m = 0.0;
    const double a0 = atan2(pb->uz, pb->ux);
    const SpLook lk = sp_look(sd->osm_id);
    int j, k, K, kb, nr = 0;
    {   /* the front line: the pitch box grown 7 m, corners rounded on a 14 m radius (10 points each) */
        const double hl = pb->hl + SP_M(7.0), hw = pb->hw + SP_M(7.0), R = SP_M(14.0);
        int q, p;
        for (q = 0; q < 4; q++) {
            const double sgs = (q == 0 || q == 3) ? 1.0 : -1.0, sgt = (q < 2) ? 1.0 : -1.0;
            const double ccs = sgs * (hl - R), cct = sgt * (hw - R), aq = q * 0.5 * SP_PI;
            for (p = 0; p < 10; p++) {
                const double a = aq + 0.5 * SP_PI * (double)p / 9.0;
                const double ls = ccs + cos(a) * R, lt = cct + sin(a) * R;
                rx[nr] = pb->cx + pb->ux * ls - pb->uz * lt; rz[nr] = pb->cz + pb->uz * ls + pb->ux * lt; nr++;
            }
        }
    }
    for (j = 0; j <= SP_BOWL_M; j++) {
        const double a = a0 + 2.0 * SP_PI * (double)(j % SP_BOWL_M) / (double)SP_BOWL_M, dx = cos(a), dz = sin(a);
        double ro = sp_ray(ox, oz, on, pb->cx, pb->cz, dx, dz), ri = sp_ray(hx, hz, hn, pb->cx, pb->cz, dx, dz);
        double rf = sp_ray(rx, rz, nr, pb->cx, pb->cz, dx, dz);
        if (ro <= 0.0 || rf <= 0.0) return -1;
        if (rf > ro - SP_M(12.0)) rf = ro - SP_M(12.0);
        if (rf < SP_M(5.0)) return -1;
        if (ri <= 0.0 || ri > ro - SP_M(4.0)) ri = ro - SP_M(4.0);
        if (ri < rf) ri = rf;
        fx[j] = pb->cx + dx * rf; fz[j] = pb->cz + dz * rf;
        bx[j] = pb->cx + dx * ro; bz[j] = pb->cz + dz * ro;
        ix[j] = pb->cx + dx * ri; iz[j] = pb->cz + dz * ri;
        nx[j] = dx; nz[j] = dz;
        depth += ro - rf;
    }
    depth /= (double)(SP_BOWL_M + 1);
    {   /* the mean row length: half way between the front line and the outline */
        double pf = 0.0, pbk = 0.0;
        for (j = 0; j < SP_BOWL_M; j++) {
            pf += hypot(fx[j + 1] - fx[j], fz[j + 1] - fz[j]);
            pbk += hypot(bx[j + 1] - bx[j], bz[j + 1] - bz[j]);
        }
        row_m = 0.5 * (pf + pbk) / SP_UPM;
    }
    B = sp_base(e->c, pb->cx, pb->cz);
    for (j = 0; j <= SP_BOWL_M; j++) gy[j] = sp_base(e->c, bx[j], bz[j]);
    Ht = s->height > SP_M(12.0) ? s->height : SP_M(12.0);
    top = B + 0.78 * Ht;
    y0 = B + SP_M(1.2);
    K = (int)(depth / SP_M(1.6) + 0.5);
    if (lk.capacity > 0 && row_m > 1.0) K = (int)((double)lk.capacity * SP_SEAT_M / row_m + 0.5);   /* rows for the capacity */
    if (K < 6) K = 6;
    if (K > SP_TIER_MAX) K = SP_TIER_MAX;
    e->st->seats += (int)((double)K * row_m / SP_SEAT_M);
    /* drawn: one step per two seat rows past 24 (the tiers were 9,216 of the bowl's ~10k quads, CPU-transformed every
     * frame: +1.8 ms from the street; a 1.6 m step is not seen from there) */
    if (K > 24) K = (K + 1) / 2;
    kb = K / 2;
    rise = (top - y0 - box_h) / (double)K;
    for (k = 0; k < K; k++) T[k] = y0 + (double)(k + 1) * rise + (k >= kb ? box_h : 0.0);
    /* the wall round the pitch (front of row 0), then the tiers */
    e->foot = TD5_RSB_FRONT;
    for (j = 0; j < SP_BOWL_M; j++)
        if (!sp_wall(e, SP_PAINT, fx[j], fz[j], fx[j + 1], fz[j + 1], B - SP_SINK, B - SP_SINK, y0, y0, SC_CONC_D)) return 0;
    e->foot = -1;
    if (!sp_tiers(e, fx, fz, bx, bz, SP_BOWL_M, 1, K, T, y0, kb, box_h, &lk)) return 0;
    /* the outer wall to the mapped height: facade + a dark ribbon half way up */
    yr_o = B + Ht + SP_M(0.6);
    yr_i = B + Ht - SP_M(1.0);
    for (j = 0; j < SP_BOWL_M; j++) {
        /* plain panels in two tones per sector (a textured page made moire arcs across 100 m of wall) */
        e->foot = TD5_RSB_OUTER;
        if (!sp_wall(e, SP_PAINT, bx[j], bz[j], bx[j + 1], bz[j + 1], gy[j] - SP_SINK, gy[j + 1] - SP_SINK, yr_o, yr_o,
                     (j & 1) ? SC_FACADE : SC_FACADE_D))
            return 0;
        e->foot = -1;
        {
            const double px0 = bx[j] + nx[j] * SP_M(0.12), pz0 = bz[j] + nz[j] * SP_M(0.12);
            const double px1 = bx[j + 1] + nx[j + 1] * SP_M(0.12), pz1 = bz[j + 1] + nz[j + 1] * SP_M(0.12);
            const double yb = B + 0.45 * Ht;
            if (!sp_wall(e, SP_PAINT, px0, pz0, px1, pz1, yb, yb, yb + SP_M(1.4), yb + SP_M(1.4), SC_GLASS)) return 0;
        }
    }
    {   /* entrances: 12 portals round the outer wall */
        double px[SP_BOWL_M + 1], pz[SP_BOWL_M + 1];
        for (j = 0; j <= SP_BOWL_M; j++) { px[j] = bx[j]; pz[j] = bz[j]; }
        if (!sp_portals(e, px, pz, nx, nz, SP_BOWL_M, 12, 6.0, 4.5)) return 0;
    }
    /* the membrane roof: outline -> courtyard ring, 4 radial pieces, folded: a ridge on every even sector line */
    for (j = 0; j < SP_BOWL_M; j++) {
        const int RS = 4;
        int r;
        for (r = 0; r < RS; r++) {
            const double t0 = (double)r / RS, t1 = (double)(r + 1) / RS;
            double x[4], y[4], z[4];
            int q;
            for (q = 0; q < 4; q++) {
                const int jj = (q == 0 || q == 3) ? j : j + 1;
                const double t = (q < 2) ? t0 : t1;
                const double amp = ((jj & 1) == 0) ? SP_M(2.2) : -SP_M(0.6);
                x[q] = bx[jj] + (ix[jj] - bx[jj]) * t; z[q] = bz[jj] + (iz[jj] - bz[jj]) * t;
                y[q] = yr_o + (yr_i - yr_o) * t + amp * sin(SP_PI * t);
            }
            if (!sp_quad(e, SP_PAINT, x, y, z, SC_MEMBRANE, 4.0, 1)) return 0;
        }
        /* the steel edge rings: compression ring on the wall top, tension ring on the courtyard edge */
        if (!sp_wall(e, SP_METAL, bx[j], bz[j], bx[j + 1], bz[j + 1], yr_o - SP_M(0.9), yr_o - SP_M(0.9), yr_o + SP_M(0.5),
                     yr_o + SP_M(0.5), SC_STEEL)) return 0;
        if (!sp_wall(e, SP_METAL, ix[j], iz[j], ix[j + 1], iz[j + 1], yr_i - SP_M(0.8), yr_i - SP_M(0.8), yr_i + SP_M(0.2),
                     yr_i + SP_M(0.2), SC_STEEL_D)) return 0;
    }
    e->st->roofs++;
    e->st->bowls++;
    return 1;
}

/* AN OPEN STAND: the outline cut at two edges into the chain facing the field (front) and the one away from it */
static int sp_open_stand(SpEm *e, const TD5_RoamStadium *sd, const TD5_RoamStand *s, const SpBox *pb, int has_pb,
                         double fpx, double fpz)
{
    double x[SP_RING_MAX], z[SP_RING_MAX], d[SP_RING_MAX];
    double fx[SP_STAND_MAXM + 1], fz[SP_STAND_MAXM + 1], bx[SP_STAND_MAXM + 1], bz[SP_STAND_MAXM + 1];
    double nx[SP_STAND_MAXM + 1], nz[SP_STAND_MAXM + 1], T[SP_TIER_MAX];
    double best = -1.0, depth = 0.0, len = 0.0, B, Ht, top, y0, rise, ytop;
    const SpLook lk = sp_look(sd->osm_id);
    int n = sp_ring(s->px, s->pz, s->n, x, z), a, b, ba = -1, bb = -1, front1 = 1, m, j, k, K;
    if (!n) return 0;
    for (k = 0; k < n; k++) d[k] = has_pb ? sp_box_dist(pb, x[k], z[k]) : hypot(x[k] - fpx, z[k] - fpz);
    for (a = 0; a < n; a++)
        for (b = a + 1; b < n; b++) {
            /* chain 1 = vertices a+1 .. b, chain 2 = b+1 .. a (cyclic) */
            const int c1 = b - a, c2 = n - c1;
            double m1 = 0.0, m2 = 0.0, sc;
            if (c1 < 2 || c2 < 2) continue;
            for (k = a + 1; k <= b; k++) m1 += d[k];
            for (k = 1; k <= c2; k++) m2 += d[(b + k) % n];
            m1 /= c1; m2 /= c2;
            sc = fabs(m1 - m2);
            if (sc > best + 1e-6) { best = sc; ba = a; bb = b; front1 = m1 <= m2; }
        }
    if (ba < 0) return 0;
    for (k = 0; k < n; k++) len += hypot(x[(k + 1) % n] - x[k], z[(k + 1) % n] - z[k]);
    m = (int)(0.5 * len / SP_M(5.0) + 0.5);
    if (m < 2) m = 2;
    if (m > SP_STAND_MAXM) m = SP_STAND_MAXM;
    if (front1) {   /* front a+1 .. b, back a, a-1 .. b+1 */
        sp_chain(x, z, n, ba + 1, bb - ba, 1, m, fx, fz);
        sp_chain(x, z, n, ba, n - (bb - ba), -1, m, bx, bz);
    } else {        /* front b+1 .. a, back b, b-1 .. a+1 */
        sp_chain(x, z, n, bb + 1, n - (bb - ba), 1, m, fx, fz);
        sp_chain(x, z, n, bb, bb - ba, -1, m, bx, bz);
    }
    for (j = 0; j <= m; j++) {
        const double L = hypot(bx[j] - fx[j], bz[j] - fz[j]);
        depth += L;
        nx[j] = L > 1e-6 ? (bx[j] - fx[j]) / L : 0.0; nz[j] = L > 1e-6 ? (bz[j] - fz[j]) / L : 0.0;
    }
    depth /= (double)(m + 1);
    B = sp_base(e->c, 0.5 * (fx[m / 2] + bx[m / 2]), 0.5 * (fz[m / 2] + bz[m / 2]));
    Ht = s->height > SP_M(4.0) ? s->height : SP_M(4.0);
    top = B + (0.85 * Ht > SP_M(3.0) ? 0.85 * Ht : SP_M(3.0));
    y0 = B + SP_M(0.8);
    K = (int)(depth / SP_M(2.0) + 0.5);   /* drawn steps of ~2 m (2.5 seat rows): CPU-transformed, see sp_bowl */
    if (K < 2) K = 2;
    if (K > SP_TIER_MAX) K = SP_TIER_MAX;
    rise = (top - y0) / (double)K;
    for (k = 0; k < K; k++) T[k] = y0 + (double)(k + 1) * rise;
    {   /* the seat census: a seat row every 0.8 m of depth, the mean of the front and back chain lengths */
        double lf = 0.0, lb = 0.0;
        for (j = 0; j < m; j++) { lf += hypot(fx[j + 1] - fx[j], fz[j + 1] - fz[j]); lb += hypot(bx[j + 1] - bx[j], bz[j + 1] - bz[j]); }
        e->st->seats += (int)(floor(depth / SP_M(0.8)) * 0.5 * (lf + lb) / SP_UPM / SP_SEAT_M);
    }
    e->foot = TD5_RSB_SFRONT;
    for (j = 0; j < m; j++)
        if (!sp_wall(e, SP_PAINT, fx[j], fz[j], fx[j + 1], fz[j + 1], B - SP_SINK, B - SP_SINK, y0, y0, SC_CONC_D)) return 0;
    e->foot = -1;
    if (!sp_tiers(e, fx, fz, bx, bz, m, 0, K, T, y0, -1, 0.0, &lk)) return 0;
    /* the back wall (a parapet over the top row; up to the roof when there is one), the stepped end walls */
    ytop = Ht >= SP_M(15.0) ? top + SP_M(3.5) : top + SP_M(1.1);
    e->foot = TD5_RSB_SBACK;
    for (j = 0; j < m; j++)
        if (!sp_wall(e, SP_PAINT, bx[j], bz[j], bx[j + 1], bz[j + 1], B - SP_SINK, B - SP_SINK, ytop, ytop,
                     (j & 1) ? SC_CONCRETE : SC_CONC_M)) return 0;
    e->foot = TD5_RSB_SEND;
    for (j = 0; j <= m; j += m)
        for (k = 0; k < K; k++) {
            const double f0 = (double)k / K, f1 = (double)(k + 1) / K;
            if (!sp_wall(e, SP_PAINT, fx[j] + (bx[j] - fx[j]) * f0, fz[j] + (bz[j] - fz[j]) * f0,
                         fx[j] + (bx[j] - fx[j]) * f1, fz[j] + (bz[j] - fz[j]) * f1, B - SP_SINK, B - SP_SINK, T[k], T[k], SC_CONCRETE))
                return 0;
        }
    e->foot = -1;
    {
        double back = 0.0;
        for (j = 0; j < m; j++) back += hypot(bx[j + 1] - bx[j], bz[j + 1] - bz[j]);
        if (!sp_portals(e, bx, bz, nx, nz, m, (int)(back / SP_M(25.0)), 3.0, 3.0)) return 0;
    }
    if (Ht >= SP_M(15.0)) {   /* a cantilever roof over 75 % of the depth (a rule: OSM maps none here) */
        for (j = 0; j < m; j++) {
            const double qx0 = fx[j] + (bx[j] - fx[j]) * 0.25, qz0 = fz[j] + (bz[j] - fz[j]) * 0.25;
            const double qx1 = fx[j + 1] + (bx[j + 1] - fx[j + 1]) * 0.25, qz1 = fz[j + 1] + (bz[j + 1] - fz[j + 1]) * 0.25;
            const double xx[4] = { bx[j], bx[j + 1], qx1, qx0 }, zz[4] = { bz[j], bz[j + 1], qz1, qz0 };
            const double yy[4] = { ytop, ytop, top + SP_M(2.2), top + SP_M(2.2) };
            if (!sp_quad(e, SP_METAL, xx, yy, zz, SC_STEEL, 2.0, 1)) return 0;
            if (!sp_wall(e, SP_METAL, qx0, qz0, qx1, qz1, top + SP_M(1.6), top + SP_M(1.6), top + SP_M(2.2), top + SP_M(2.2), SC_STEEL_D))
                return 0;
        }
        e->st->stand_roofs++;
    }
    return 1;
}

/* ============================================================================
 * SECTION: pitches
 * ========================================================================== */

typedef struct {
    SpEm *e;
    SpBox b;         /* the pitch box (pitch frame: s along u, t across)              */
    double lift;     /* SP_LIFT_LINE                                                    */
    double w;        /* line width (units)                                              */
} SpPf;

static void sp_pf_xz(const SpPf *p, double s, double t, double *x, double *z)
{
    *x = p->b.cx + p->b.ux * s - p->b.uz * t;
    *z = p->b.cz + p->b.uz * s + p->b.ux * t;
}

/* a flat patch over the pitch frame rectangle [s0,s1] x [t0,t1], cells <= 6 m, following the ground; `in` = keep only
 * cells whose centre is inside the outline (the surface), else every cell */
static int sp_patch(SpPf *p, int pg, double s0, double t0, double s1, double t1, double lift, unsigned int col, unsigned int col2,
                    int stripes, const double *rx, const double *rz, int rn)
{
    int ns = stripes > 0 ? stripes : (int)ceil((s1 - s0) / SP_M(6.0)), nt = (int)ceil((t1 - t0) / SP_M(6.0)), i, j, q;
    if (ns < 1) ns = 1;
    if (nt < 1) nt = 1;
    if (ns > 64) ns = 64;
    if (nt > 64) nt = 64;
    for (i = 0; i < ns; i++)
        for (j = 0; j < nt; j++) {
            const double a0 = s0 + (s1 - s0) * i / ns, a1 = s0 + (s1 - s0) * (i + 1) / ns;
            const double b0 = t0 + (t1 - t0) * j / nt, b1 = t0 + (t1 - t0) * (j + 1) / nt;
            const double S[4] = { a0, a1, a1, a0 }, Tt[4] = { b0, b0, b1, b1 };
            double x[4], y[4], z[4], mx, mz;
            for (q = 0; q < 4; q++) { sp_pf_xz(p, S[q], Tt[q], &x[q], &z[q]); y[q] = sp_base(p->e->c, x[q], z[q]) + lift; }
            if (rx) {
                sp_pf_xz(p, 0.5 * (a0 + a1), 0.5 * (b0 + b1), &mx, &mz);
                if (!td5_geob_point_in_ring(rx, rz, rn, mx, mz)) continue;
            }
            if (!sp_quad(p->e, pg, x, y, z, (stripes > 0 && (i & 1)) ? col2 : col, pg == SP_GRASS ? 7.9 : 2.0, 0)) return 0;
        }
    return 1;
}

/* a painted line from (s0, t0) to (s1, t1) in the pitch frame, cut every 4 m to follow the ground */
static int sp_line(SpPf *p, double s0, double t0, double s1, double t1)
{
    const double L = hypot(s1 - s0, t1 - t0);
    int n = (int)ceil(L / SP_M(4.0)), i, q;
    double ds, dt;
    if (L < 1.0) return 1;
    if (n < 1) n = 1;
    ds = -(t1 - t0) / L * p->w * 0.5; dt = (s1 - s0) / L * p->w * 0.5;
    for (i = 0; i < n; i++) {
        const double fa = (double)i / n, fb = (double)(i + 1) / n;
        const double sa = s0 + (s1 - s0) * fa, ta = t0 + (t1 - t0) * fa, sb = s0 + (s1 - s0) * fb, tb = t0 + (t1 - t0) * fb;
        const double S[4] = { sa - ds, sb - ds, sb + ds, sa + ds }, Tt[4] = { ta - dt, tb - dt, tb + dt, ta + dt };
        double x[4], y[4], z[4];
        for (q = 0; q < 4; q++) { sp_pf_xz(p, S[q], Tt[q], &x[q], &z[q]); y[q] = sp_base(p->e->c, x[q], z[q]) + p->lift; }
        if (!sp_quad(p->e, SP_PAINT, x, y, z, SC_LINE, 1.0, 0)) return 0;
        p->e->st->line_quads++;
    }
    return 1;
}

static int sp_rect(SpPf *p, double s0, double t0, double s1, double t1)
{
    return sp_line(p, s0, t0, s1, t0) && sp_line(p, s1, t0, s1, t1) && sp_line(p, s1, t1, s0, t1) && sp_line(p, s0, t1, s0, t0);
}

/* an arc round (cs, ct), radius r, from angle a0 to a1 (rad, 0 = +s) */
static int sp_arc(SpPf *p, double cs, double ct, double r, double a0, double a1)
{
    int n = (int)ceil(fabs(a1 - a0) * r / SP_M(1.5)), i;
    if (n < 6) n = 6;
    if (n > 48) n = 48;
    for (i = 0; i < n; i++) {
        const double u0 = a0 + (a1 - a0) * i / n, u1 = a0 + (a1 - a0) * (i + 1) / n;
        if (!sp_line(p, cs + cos(u0) * r, ct + sin(u0) * r, cs + cos(u1) * r, ct + sin(u1) * r)) return 0;
    }
    return 1;
}

static int sp_spot(SpPf *p, double s, double t, double r)
{
    const double w = p->w;
    int ok;
    p->w = 2.0 * r;
    ok = sp_line(p, s - r, t, s + r, t);
    p->w = w;
    return ok;
}

/* a goal on the line s = gs (facing -sgn s): posts + crossbar + two rear struts down to the ground */
static int sp_goal(SpPf *p, double gs, double sgn, double half_w, double h, double depth)
{
    const double pr = SP_M(0.06);
    double ax, az, bx, bz, cx, cz, dx, dz;
    double ya, yb;
    sp_pf_xz(p, gs, -half_w, &ax, &az);
    sp_pf_xz(p, gs, half_w, &bx, &bz);
    sp_pf_xz(p, gs + sgn * depth, -half_w, &cx, &cz);
    sp_pf_xz(p, gs + sgn * depth, half_w, &dx, &dz);
    ya = sp_base(p->e->c, ax, az); yb = sp_base(p->e->c, bx, bz);
    if (!sp_bar(p->e, SP_PAINT, ax, ya, az, ax, ya + h, az, pr, SC_LINE)) return 0;
    if (!sp_bar(p->e, SP_PAINT, bx, yb, bz, bx, yb + h, bz, pr, SC_LINE)) return 0;
    if (!sp_bar(p->e, SP_PAINT, ax, ya + h, az, bx, yb + h, bz, pr, SC_LINE)) return 0;
    if (!sp_bar(p->e, SP_METAL, ax, ya + h, az, cx, ya, cz, pr * 0.6, SC_STEEL)) return 0;
    if (!sp_bar(p->e, SP_METAL, bx, yb + h, bz, dx, yb, dz, pr * 0.6, SC_STEEL)) return 0;
    if (!sp_bar(p->e, SP_METAL, cx, ya + SP_M(0.05), cz, dx, yb + SP_M(0.05), dz, pr * 0.6, SC_STEEL)) return 0;
    p->e->st->goals++;
    return 1;
}

/* a net across the pitch at s = ns from t = -half to +half: two posts and the net panel from y0 to y1 */
static int sp_net(SpPf *p, double ns, double half, double y0, double y1)
{
    double ax, az, bx, bz;
    double ya, yb;
    sp_pf_xz(p, ns, -half, &ax, &az);
    sp_pf_xz(p, ns, half, &bx, &bz);
    ya = sp_base(p->e->c, ax, az); yb = sp_base(p->e->c, bx, bz);
    if (!sp_bar(p->e, SP_METAL, ax, ya, az, ax, ya + y1 + SP_M(0.05), az, SP_M(0.05), SC_STEEL_D)) return 0;
    if (!sp_bar(p->e, SP_METAL, bx, yb, bz, bx, yb + y1 + SP_M(0.05), bz, SP_M(0.05), SC_STEEL_D)) return 0;
    if (!sp_wall(p->e, SP_PAINT, ax, az, bx, bz, ya + y0, yb + y0, ya + y1 - SP_M(0.06), yb + y1 - SP_M(0.06), SC_NET)) return 0;
    if (!sp_wall(p->e, SP_PAINT, ax, az, bx, bz, ya + y1 - SP_M(0.06), yb + y1 - SP_M(0.06), ya + y1, yb + y1, SC_LINE)) return 0;
    p->e->st->nets++;
    return 1;
}

/* a basketball hoop behind the baseline s = bs (court towards -sgn): pole, arm, backboard, rim */
static int sp_hoop(SpPf *p, double bs, double sgn, double sc)
{
    double px, pz, qx, qz, rx, rz, wx0, wz0, wx1, wz1, y;
    sp_pf_xz(p, bs + sgn * SP_M(1.0), 0.0, &px, &pz);                /* pole, 1 m behind the line */
    sp_pf_xz(p, bs - sgn * SP_M(1.2 * sc), 0.0, &qx, &qz);           /* board plane               */
    sp_pf_xz(p, bs - sgn * SP_M(1.2 * sc + 0.38), 0.0, &rx, &rz);    /* rim centre                */
    y = sp_base(p->e->c, px, pz);
    if (!sp_bar(p->e, SP_METAL, px, y, pz, px, y + SP_M(3.6), pz, SP_M(0.08), SC_STEEL_D)) return 0;
    if (!sp_bar(p->e, SP_METAL, px, y + SP_M(3.5), pz, qx, y + SP_M(3.5), qz, SP_M(0.05), SC_STEEL_D)) return 0;
    sp_pf_xz(p, bs - sgn * SP_M(1.2 * sc), -SP_M(0.9), &wx0, &wz0);
    sp_pf_xz(p, bs - sgn * SP_M(1.2 * sc), SP_M(0.9), &wx1, &wz1);
    if (!sp_wall(p->e, SP_PAINT, wx0, wz0, wx1, wz1, y + SP_M(2.9), y + SP_M(2.9), y + SP_M(3.95), y + SP_M(3.95), SC_LINE)) return 0;
    if (!sp_box(p->e, SP_METAL, rx, rz, y + SP_M(3.03), y + SP_M(3.07), SP_M(0.23), SP_M(0.23), p->b.ux, p->b.uz, SC_ORANGE)) return 0;
    p->e->st->hoops++;
    return 1;
}

static int sp_soccer(SpPf *p)
{
    const double in = SP_M(0.5), L = p->b.hl - in, W = p->b.hw - in;
    double s = (2.0 * L) / SP_M(105.0), g;
    int e;
    if ((2.0 * W) / SP_M(68.0) < s) s = (2.0 * W) / SP_M(68.0);
    if (s > 1.0) s = 1.0;
    if (s < 0.3) s = 0.3;
    g = s < 0.5 ? 3.0 / 7.32 : s;                            /* a futbol 5 pitch: 3 x 2 m goals */
    if (!sp_rect(p, -L, -W, L, W)) return 0;
    if (!sp_line(p, 0.0, -W, 0.0, W)) return 0;
    if (!sp_arc(p, 0.0, 0.0, SP_M(9.15 * s), 0.0, 2.0 * SP_PI)) return 0;
    if (!sp_spot(p, 0.0, 0.0, SP_M(0.15))) return 0;
    for (e = -1; e <= 1; e += 2) {
        const double gl = e * L;
        double pw = SP_M(20.16 * s), aw = SP_M(9.16 * s);
        if (pw > W - SP_M(1.0)) pw = W - SP_M(1.0);
        if (aw > pw - SP_M(1.0)) aw = pw - SP_M(1.0);
        if (!sp_rect(p, gl - e * SP_M(16.5 * s), -pw, gl, pw)) return 0;
        if (!sp_rect(p, gl - e * SP_M(5.5 * s), -aw, gl, aw)) return 0;
        if (!sp_spot(p, gl - e * SP_M(11.0 * s), 0.0, SP_M(0.15))) return 0;
        {   /* the penalty arc outside the box: +-acos(5.5 / 9.15) round the pitch's centre direction */
            const double al = acos(5.5 / 9.15), c0 = e > 0 ? SP_PI : 0.0;
            if (!sp_arc(p, gl - e * SP_M(11.0 * s), 0.0, SP_M(9.15 * s), c0 - al, c0 + al)) return 0;
        }
        {   /* corner arcs */
            const double c0 = e > 0 ? SP_PI : 0.0;
            if (!sp_arc(p, gl, -W, SP_M(1.0), c0 + (e > 0 ? -0.5 * SP_PI : 0.0), c0 + (e > 0 ? 0.0 : 0.5 * SP_PI))) return 0;
            if (!sp_arc(p, gl, W, SP_M(1.0), c0 + (e > 0 ? 0.0 : -0.5 * SP_PI), c0 + (e > 0 ? 0.5 * SP_PI : 0.0))) return 0;
        }
        if (!sp_goal(p, gl, (double)e, SP_M(3.66 * g), SP_M(s < 0.5 ? 2.0 : 2.44), SP_M(s < 0.5 ? 1.0 : 1.8))) return 0;
    }
    return 1;
}

static int sp_hockey(SpPf *p)
{
    const double in = SP_M(0.5), L = p->b.hl - in, W = p->b.hw - in;
    double s = (2.0 * L) / SP_M(91.4);
    int e;
    if ((2.0 * W) / SP_M(55.0) < s) s = (2.0 * W) / SP_M(55.0);
    if (s > 1.0) s = 1.0;
    if (s < 0.4) s = 0.4;
    if (!sp_rect(p, -L, -W, L, W)) return 0;
    if (!sp_line(p, 0.0, -W, 0.0, W)) return 0;
    for (e = -1; e <= 1; e += 2) {
        const double gl = e * L, R = SP_M(14.63 * s), gp = SP_M(1.83);
        const double c0 = e > 0 ? SP_PI : 0.0;
        if (!sp_line(p, gl - e * SP_M(22.9 * s), -W, gl - e * SP_M(22.9 * s), W)) return 0;
        if (!sp_line(p, gl - e * R, -gp, gl - e * R, gp)) return 0;
        if (!sp_arc(p, gl, -gp, R, e > 0 ? SP_PI : -0.5 * SP_PI, e > 0 ? 1.5 * SP_PI : 0.0)) return 0;
        if (!sp_arc(p, gl, gp, R, e > 0 ? 0.5 * SP_PI : 0.0, e > 0 ? SP_PI : 0.5 * SP_PI)) return 0;
        (void)c0;
        if (!sp_spot(p, gl - e * SP_M(6.4 * s), 0.0, SP_M(0.15))) return 0;
        if (!sp_goal(p, gl, (double)e, gp, SP_M(2.14), SP_M(1.2))) return 0;
    }
    return 1;
}

static int sp_rugby(SpPf *p)
{
    const double in = SP_M(0.5), L = p->b.hl - in, W = p->b.hw - in, T = 0.9 * L, s = T / SP_M(50.0);
    int e;
    if (!sp_rect(p, -L, -W, L, W)) return 0;
    if (!sp_line(p, 0.0, -W, 0.0, W)) return 0;
    for (e = -1; e <= 1; e += 2) {
        double ax, az, bx, bz, ya, yb;
        if (!sp_line(p, e * T, -W, e * T, W)) return 0;
        if (!sp_line(p, e * (T - SP_M(22.0 * s)), -W, e * (T - SP_M(22.0 * s)), W)) return 0;
        sp_pf_xz(p, e * T, -SP_M(2.8), &ax, &az);
        sp_pf_xz(p, e * T, SP_M(2.8), &bx, &bz);
        ya = sp_base(p->e->c, ax, az); yb = sp_base(p->e->c, bx, bz);
        if (!sp_bar(p->e, SP_PAINT, ax, ya, az, ax, ya + SP_M(11.0), az, SP_M(0.08), SC_LINE)) return 0;
        if (!sp_bar(p->e, SP_PAINT, bx, yb, bz, bx, yb + SP_M(11.0), bz, SP_M(0.08), SC_LINE)) return 0;
        if (!sp_bar(p->e, SP_PAINT, ax, ya + SP_M(3.0), az, bx, yb + SP_M(3.0), bz, SP_M(0.08), SC_LINE)) return 0;
        p->e->st->goals++;
    }
    return 1;
}

/* courts tiled in the box: n along x m across, each `cl` x `cw` (m) with its run-off cell `pl` x `pw`; returns the
 * scale (1 = regulation) and the cell centres through cs / ct */
static int sp_tile_courts(const SpBox *b, double cl, double cw, double pl, double pw, double *sc, double *cs, double *ct, int cap)
{
    int na = (int)floor((2.0 * b->hl / SP_UPM + 1.0) / pl), nc = (int)floor((2.0 * b->hw / SP_UPM + 1.0) / pw), i, j, n = 0;
    double s = 1.0;
    if (na < 1) na = 1;
    if (nc < 1) nc = 1;
    if (na > 4) na = 4;
    if (nc > 6) nc = 6;
    if (2.0 * b->hl / SP_UPM / na < cl + 1.0) s = (2.0 * b->hl / SP_UPM / na - 1.0) / cl;
    if (2.0 * b->hw / SP_UPM / nc < cw + 0.6 && (2.0 * b->hw / SP_UPM / nc - 0.6) / cw < s) s = (2.0 * b->hw / SP_UPM / nc - 0.6) / cw;
    if (s < 0.55) return 0;
    if (s > 1.0) s = 1.0;
    for (i = 0; i < na; i++)
        for (j = 0; j < nc && n < cap; j++) {
            cs[n] = -b->hl + (2.0 * b->hl) * ((double)i + 0.5) / na;
            ct[n] = -b->hw + (2.0 * b->hw) * ((double)j + 0.5) / nc;
            n++;
        }
    *sc = s;
    return n;
}

/* a shifted copy of the pitch frame centred on a court cell */
static SpPf sp_sub(const SpPf *p, double cs, double ct)
{
    SpPf q = *p;
    sp_pf_xz(p, cs, ct, &q.b.cx, &q.b.cz);
    return q;
}

static int sp_tennis(SpPf *p, int padel, int acrylic)
{
    double cs[24], ct[24], s;
    const int n = padel ? sp_tile_courts(&p->b, 20.0, 10.0, 22.0, 12.0, &s, cs, ct, 24)
                        : sp_tile_courts(&p->b, 23.77, 10.97, 36.0, 18.0, &s, cs, ct, 24);
    int i;
    for (i = 0; i < n; i++) {
        SpPf q = sp_sub(p, cs[i], ct[i]);
        if (padel) {
            const double L = SP_M(10.0 * s), W = SP_M(5.0 * s), sv = SP_M(6.95 * s);
            if (acrylic && !sp_patch(&q, SP_PAINT, -L, -W, L, W, SP_LIFT_COURT, SC_ACRYL_C, SC_ACRYL_C, 0, NULL, NULL, 0)) return 0;
            if (!sp_rect(&q, -L, -W, L, W) || !sp_line(&q, -sv, -W, -sv, W) || !sp_line(&q, sv, -W, sv, W) ||
                !sp_line(&q, -sv, 0.0, sv, 0.0)) return 0;
            if (!sp_net(&q, 0.0, W + SP_M(0.2), SP_M(0.0), SP_M(0.88))) return 0;
        } else {
            const double L = SP_M(11.885 * s), W = SP_M(5.485 * s), Ws = SP_M(4.115 * s), sv = SP_M(6.40 * s);
            if (acrylic && !sp_patch(&q, SP_PAINT, -L - SP_M(3.0), -W - SP_M(2.0), L + SP_M(3.0), W + SP_M(2.0), SP_LIFT_COURT,
                                     SC_ACRYL_C, SC_ACRYL_C, 0, NULL, NULL, 0)) return 0;
            if (!sp_rect(&q, -L, -W, L, W) || !sp_line(&q, -L, -Ws, L, -Ws) || !sp_line(&q, -L, Ws, L, Ws) ||
                !sp_line(&q, -sv, -Ws, -sv, Ws) || !sp_line(&q, sv, -Ws, sv, Ws) || !sp_line(&q, -sv, 0.0, sv, 0.0) ||
                !sp_line(&q, -L, 0.0, -L + SP_M(0.2), 0.0) || !sp_line(&q, L - SP_M(0.2), 0.0, L, 0.0)) return 0;
            if (!sp_net(&q, 0.0, W + SP_M(0.914), SP_M(0.0), SP_M(1.0))) return 0;
        }
        p->e->st->courts++;
    }
    return 1;
}

static int sp_basket(SpPf *p)
{
    const double in = SP_M(0.6);
    double s = (2.0 * (p->b.hl - in)) / SP_M(28.0), L, W, r3, ts;
    int e;
    if ((2.0 * (p->b.hw - in)) / SP_M(15.0) < s) s = (2.0 * (p->b.hw - in)) / SP_M(15.0);
    if (s > 1.0) s = 1.0;
    if (s < 0.5) return sp_rect(p, -(p->b.hl - in), -(p->b.hw - in), p->b.hl - in, p->b.hw - in);
    L = SP_M(14.0 * s); W = SP_M(7.5 * s); r3 = SP_M(6.75 * s); ts = W - SP_M(0.9 * s);
    if (!sp_rect(p, -L, -W, L, W) || !sp_line(p, 0.0, -W, 0.0, W) || !sp_arc(p, 0.0, 0.0, SP_M(1.8 * s), 0.0, 2.0 * SP_PI)) return 0;
    for (e = -1; e <= 1; e += 2) {
        const double bl = e * L, hs = bl - e * SP_M(1.575 * s), fl = bl - e * SP_M(5.8 * s), kw = SP_M(2.45 * s);
        const double a = asin(ts / r3), c0 = e > 0 ? SP_PI : 0.0;
        const double ds = r3 * cos(a);
        if (!sp_rect(p, e > 0 ? fl : bl, -kw, e > 0 ? bl : fl, kw)) return 0;
        if (!sp_arc(p, fl, 0.0, SP_M(1.8 * s), 0.0, 2.0 * SP_PI)) return 0;
        if (!sp_arc(p, hs, 0.0, r3, c0 - a, c0 + a)) return 0;
        if (!sp_line(p, bl, -ts, hs - e * ds, -ts) || !sp_line(p, bl, ts, hs - e * ds, ts)) return 0;
        if (!sp_hoop(p, bl, (double)e, s)) return 0;
    }
    return 1;
}

static int sp_volley(SpPf *p)
{
    const double in = SP_M(0.6);
    double s = (2.0 * (p->b.hl - in)) / SP_M(18.0), L, W;
    if ((2.0 * (p->b.hw - in)) / SP_M(9.0) < s) s = (2.0 * (p->b.hw - in)) / SP_M(9.0);
    if (s > 1.0) s = 1.0;
    if (s < 0.5) return 1;
    L = SP_M(9.0 * s); W = SP_M(4.5 * s);
    if (!sp_rect(p, -L, -W, L, W) || !sp_line(p, 0.0, -W, 0.0, W) || !sp_line(p, -SP_M(3.0 * s), -W, -SP_M(3.0 * s), W) ||
        !sp_line(p, SP_M(3.0 * s), -W, SP_M(3.0 * s), W)) return 0;
    return sp_net(p, 0.0, W + SP_M(1.0), SP_M(1.43), SP_M(2.43));
}

/* whether a pitch gets painted, and why not: 0 = painted, else the census slot it is left out under (1 small,
 * 2 sport, 3 untagged / bad ring, 4 on a road). Shared by the emit and td5_roam_pitch_paints. */
static int sp_pitch_verdict(TG_TileCtx *c, const TD5_RoamPitch *pt, double *x, double *z, int *n, int *surf, SpBox *b)
{
    double m2;
    *n = sp_ring(pt->rx, pt->rz, pt->n, x, z);
    *surf = pt->surface;
    if (!*n) return 3;
    m2 = fabs(td5_geob_ring_area(x, z, *n)) / (SP_UPM * SP_UPM);
    if (m2 < SP_SMALL_M2) return 1;
    if (pt->sport == TD5_RPS_OTHER && *surf == TD5_RPF_NONE) return 2;
    if (pt->sport == TD5_RPS_NONE && (*surf == TD5_RPF_NONE || *surf == TD5_RPF_OTHER)) return 3;
    if (*surf == TD5_RPF_NONE || *surf == TD5_RPF_OTHER) *surf = sp_default_surface(pt->sport);
    if (roam_ctx_on_road(c, x, z, *n)) return 4;
    if (!sp_obb(x, z, *n, b)) return 3;
    return 0;
}

int td5_roam_pitch_paints(TG_TileCtx *c, const TD5_RoamPitch *pt)
{
    double x[SP_RING_MAX], z[SP_RING_MAX];
    int n, surf;
    SpBox b;
    return td5_roam_pitch_enabled() && sp_pitch_verdict(c, pt, x, z, &n, &surf, &b) == 0;
}

static int sp_pitch(SpEm *e, const TD5_RoamPitch *pt)
{
    double x[SP_RING_MAX], z[SP_RING_MAX];
    int n, surf, sport = pt->sport, ok = 1, stripes = 0, v;
    unsigned int c1 = 0, c2 = 0;
    SpPf p;
    memset(&p, 0, sizeof p);
    v = sp_pitch_verdict(e->c, pt, x, z, &n, &surf, &p.b);
    if (v == 1) { e->st->skip_small++; return 1; }
    if (v == 2) { e->st->skip_sport++; return 1; }
    if (v == 3) { e->st->skip_none++; return 1; }
    if (v == 4) { e->st->drop_road++; return 1; }
    p.e = e;
    p.lift = SP_LIFT_LINE;
    /* wider than regulation (0.12 / 0.05 m): thinner, a line falls under a pixel from 40 m and breaks into dashes */
    p.w = SP_M(sport == TD5_RPS_SOCCER || sport == TD5_RPS_RUGBY || sport == TD5_RPS_HOCKEY ? 0.24 : 0.12);
    switch (surf) {
    case TD5_RPF_GRASS: c1 = SC_GRASS_L; c2 = SC_GRASS_D; break;
    case TD5_RPF_CLAY:  c1 = c2 = SC_CLAY; break;
    case TD5_RPF_ACRYLIC: c1 = c2 = SC_ACRYL_S; break;
    case TD5_RPF_TURF:  c1 = SC_TURF_L; c2 = SC_TURF_D; break;
    case TD5_RPF_SAND:  c1 = c2 = SC_SAND; break;
    default:            c1 = c2 = SC_HARD; break;
    }
    if (surf == TD5_RPF_GRASS || surf == TD5_RPF_TURF) {   /* mowing stripes across the long axis, about 5.5 m each */
        stripes = 2 * (int)(p.b.hl / SP_M(5.5) + 0.5);
        if (stripes < 2) stripes = 2;
    }
    if (!sp_patch(&p, surf == TD5_RPF_GRASS ? SP_GRASS : SP_PAINT, -p.b.hl, -p.b.hw, p.b.hl, p.b.hw, SP_LIFT_SURF, c1, c2,
                  stripes, x, z, n)) return 0;
    e->st->by_surface[surf]++;
    switch (sport) {
    case TD5_RPS_SOCCER: ok = sp_soccer(&p); break;
    case TD5_RPS_TENNIS: ok = sp_tennis(&p, 0, surf == TD5_RPF_ACRYLIC); break;
    case TD5_RPS_PADEL:  ok = sp_tennis(&p, 1, surf == TD5_RPF_ACRYLIC); break;
    case TD5_RPS_BASKET: ok = sp_basket(&p); break;
    case TD5_RPS_VOLLEY: case TD5_RPS_BEACH: ok = sp_volley(&p); break;
    case TD5_RPS_HOCKEY: ok = sp_hockey(&p); break;
    case TD5_RPS_RUGBY:  ok = sp_rugby(&p); break;
    default:             e->st->surface_only++; break;
    }
    if (!ok) return 0;
    e->st->pitches++;
    e->st->by_sport[sport]++;
    return 1;
}

/* ============================================================================
 * SECTION: emit
 * ========================================================================== */

static int sp_stadium(SpEm *e, const TD5_RoamStadium *sd)
{
    SpBox pb;
    int has_pb = 0, i;
    double fpx = 0.0, fpz = 0.0;
    if (sd->qn >= 3) {
        double qx[SP_RING_MAX], qz[SP_RING_MAX];
        const int qn = sp_ring(sd->qx, sd->qz, sd->qn, qx, qz);
        has_pb = qn && sp_obb(qx, qz, qn, &pb);
    }
    for (i = 0; i < sd->n; i++) { fpx += sd->rx[i]; fpz += sd->rz[i]; }
    if (sd->n > 0) { fpx /= sd->n; fpz /= sd->n; }
    e->st->stadiums++;
    for (i = 0; i < sd->nst && !e->oom; i++) {
        const TD5_RoamStand *s = &sd->st[i];
        int r = 0;
        e->st->stands++;
        if (has_pb && s->nh > 0 && s->hn[0] >= 3) {
            /* a courtyard holding the pitch centre: the bowl */
            double ox[SP_RING_MAX], oz[SP_RING_MAX], hx[SP_RING_MAX], hz[SP_RING_MAX];
            const int on = sp_ring(s->px, s->pz, s->n, ox, oz), hn = sp_ring(s->hx, s->hz, s->hn[0], hx, hz);
            if (on && hn && td5_geob_point_in_ring(hx, hz, hn, pb.cx, pb.cz)) r = sp_bowl(e, sd, s, &pb, ox, oz, on, hx, hz, hn);
            if (r == 0 && e->oom) return 0;
        }
        if (r <= 0 && !sp_open_stand(e, sd, s, &pb, has_pb, fpx, fpz) && e->oom) return 0;
    }
    return !e->oom;
}

int td5_roam_sport_emit(TG_TileCtx *c, const TD5_RoamStadium *sd, int nsd, const TD5_RoamPitch *pt, int npt,
                        TD5_RoamSportStats *st)
{
    SpEm e;
    int i, sb, pg;
    if (!c || !st || (nsd <= 0 && npt <= 0)) return 1;
    memset(&e, 0, sizeof e);
    e.c = c; e.st = st; e.nofix_sb = -1; e.foot = -1;
    e.bat = (SpBatch *)calloc((size_t)TD5_ROAM_SCN_NSB * SP_N, sizeof(SpBatch));
    if (!e.bat) return 0;
    for (i = 0; i < nsd && !e.oom; i++) sp_stadium(&e, &sd[i]);
    for (i = 0; i < npt && !e.oom; i++) if (!sp_pitch(&e, &pt[i])) e.oom = 1;
    for (sb = 0; sb < TD5_ROAM_SCN_NSB && !e.oom; sb++)
        for (pg = 0; pg < SP_N && !e.oom; pg++)
            if (!sp_flush(&e, sb, pg)) e.oom = 1;
    free(e.bat);
    return e.oom ? 0 : 1;
}
