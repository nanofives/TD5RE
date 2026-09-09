/**
 * td5_tg_world.c -- auto-track WORLD: seeded heightfield (sea, coast, rivers, mountains, flats), sparse conform/occupancy overlay, terrain classes
 *
 * [TOPOLOGY-FIRST 2026-09-08] See td5_tg_world.h for the model. This file is
 * the ONLY place that knows how terrain is synthesised; everything else asks
 * tg_world_h / tg_world_class / tg_world_is_water.
 */
#include "td5_trackgen_internal.h"
#include "td5_tg_world.h"

/* ------------------------------------------------------------------ noise -- */

/* Integer lattice hash -> [0,1). Seeded per build through s_w.salt so two
 * seeds never share a lattice, and salted per layer so the layers are not
 * shifted copies of one another. */
static unsigned int tg_wn_hash(int ix, int iz, unsigned int salt)
{
    unsigned int h = salt;
    h ^= (unsigned)ix * 0x8DA6B343u;
    h ^= (unsigned)iz * 0xD8163841u;
    h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12; h *= 0x297A2D39u; h ^= h >> 15;
    return h;
}

static double tg_wn_lattice(int ix, int iz, unsigned int salt)
{
    return (double)(tg_wn_hash(ix, iz, salt) >> 8) / 16777216.0;   /* [0,1) */
}

static double tg_wn_smooth(double t) { return t * t * (3.0 - 2.0 * t); }

/* Value noise in [0,1). x,z in LATTICE units. */
static double tg_wn_value(double x, double z, unsigned int salt)
{
    const double fx = floor(x), fz = floor(z);
    const int ix = (int)fx, iz = (int)fz;
    const double tx = tg_wn_smooth(x - fx), tz = tg_wn_smooth(z - fz);
    const double a = tg_wn_lattice(ix,     iz,     salt);
    const double b = tg_wn_lattice(ix + 1, iz,     salt);
    const double c = tg_wn_lattice(ix,     iz + 1, salt);
    const double d = tg_wn_lattice(ix + 1, iz + 1, salt);
    const double top = a + (b - a) * tx, bot = c + (d - c) * tx;
    return top + (bot - top) * tz;
}

/* fBm in [-1,1]: `oct` octaves, wavelength `wl` world units for the first. */
static double tg_wn_fbm(double x, double z, double wl, int oct, unsigned int salt)
{
    double sum = 0.0, amp = 1.0, norm = 0.0, f = 1.0 / wl;
    int o;
    for (o = 0; o < oct; o++) {
        sum  += amp * (tg_wn_value(x * f + 17.31 * o, z * f - 9.77 * o,
                                   salt + 0x9E37u * (unsigned)o) * 2.0 - 1.0);
        norm += amp;
        amp  *= 0.5;
        f    *= 2.0;
    }
    return sum / norm;
}

/* Ridged multifractal in [0,1]: sharp crests, the mountain term. */
static double tg_wn_ridged(double x, double z, double wl, int oct, unsigned int salt)
{
    double sum = 0.0, amp = 1.0, norm = 0.0, f = 1.0 / wl;
    int o;
    for (o = 0; o < oct; o++) {
        double n = tg_wn_value(x * f - 5.13 * o, z * f + 11.9 * o,
                               salt + 0x51EDu * (unsigned)o) * 2.0 - 1.0;
        n = 1.0 - fabs(n);
        sum  += amp * n * n;
        norm += amp;
        amp  *= 0.55;
        f    *= 2.05;
    }
    return sum / norm;
}

static double tg_w_smoothstep(double e0, double e1, double v)
{
    double t;
    if (e1 <= e0) return v >= e1 ? 1.0 : 0.0;
    t = (v - e0) / (e1 - e0);
    if (t < 0.0) t = 0.0;
    if (t > 1.0) t = 1.0;
    return t * t * (3.0 - 2.0 * t);
}

/* ------------------------------------------------------------ parameters -- */

typedef struct {
    unsigned int seed, salt;
    double relief;        /* master amplitude, world units                  */
    double sea_abs;       /* sea level in RAW (pre-offset) height units     */
    double mtn_bias;      /* -1..1: how much of the map is mountainous      */
    double river_w;       /* channel half-width in noise units (0 = none)   */
    double ox, oz;        /* sampling offset that puts the start on land    */
    double h0;            /* raw height at the origin, subtracted           */
    int    built, frozen;
    int    target_spans;
} TG_WorldParams;

static TG_WorldParams s_w;

/* ------------------------------------------------------------ base height -- */

/* Large-scale part only (no ridges, no hills, no detail): what a river's
 * surface follows, so the water inside a channel is level ACROSS it. */
static double tg_w_large(double rx, double rz)
{
    return tg_wn_fbm(rx, rz, 420000.0, 3, s_w.salt + 0x100u) * s_w.relief * 0.8;
}

/* River channel proximity: 0 on the channel centre line, 1 at the bank and
 * beyond. Rivers follow the zero set of a low-frequency field. */
static double tg_w_river_t(double rx, double rz)
{
    double n;
    if (s_w.river_w <= 0.0) return 1.0;
    n = tg_wn_fbm(rx, rz, 380000.0, 2, s_w.salt + 0x400u);
    n = fabs(n) / s_w.river_w;
    return n > 1.0 ? 1.0 : n;
}

#define TG_W_RIVER_DEPTH   1400.0   /* bed below the river surface          */
#define TG_W_RIVER_SURF     500.0   /* surface below the large-scale ground */

/* RAW height at raw coordinates (before the start offset / h0 shift). */
static double tg_w_raw(double rx, double rz)
{
    const double R = s_w.relief;
    const double cont  = tg_w_large(rx, rz);
    const double mm    = tg_w_smoothstep(0.05, 0.55,
                             tg_wn_fbm(rx, rz, 260000.0, 2, s_w.salt + 0x200u)
                             + s_w.mtn_bias);
    /* Slope budget (value noise: slope ~ amp / (wavelength/4) per octave,
     * roughly doubled by the octaves above it):
     *   ridges  1.5R over 50000  -> ~1.4 at the crests, 0.5..0.8 typical:
     *           the >= tan 30 deg terrain that forces tunnels and bridges;
     *   hills   0.12R over 60000 -> ~0.1..0.2: cut/fill country;
     *   detail  0.01R over 9000  -> ~0.07: texture, never a structure. */
    const double ridge = tg_wn_ridged(rx, rz, 50000.0, 4, s_w.salt + 0x300u);
    const double hills = tg_wn_fbm(rx, rz, 60000.0, 4, s_w.salt + 0x500u);
    const double det   = tg_wn_fbm(rx, rz, 9000.0, 2, s_w.salt + 0x600u);
    double h = cont
             + mm * ridge * R * 1.5
             + hills * R * 0.12 * (0.5 + 0.5 * mm)
             + det * R * 0.01;
    /* River: carve below the large-scale ground where the land is high
     * enough above the sea for a river to exist at all. */
    {
        const double t = tg_w_river_t(rx, rz);
        if (t < 1.0 && cont > s_w.sea_abs + 800.0) {
            const double bed  = cont - TG_W_RIVER_SURF - TG_W_RIVER_DEPTH;
            const double u    = 1.0 - t * t;          /* 1 centre .. 0 bank */
            const double want = bed + (h - bed) * (1.0 - u);
            if (want < h) h = want;
        }
    }
    return h;
}

static double tg_w_river_surface_raw(double rx, double rz)
{
    return tg_w_large(rx, rz) - TG_W_RIVER_SURF;
}

double tg_world_h_base(double x, double z)
{
    if (!s_w.built) return 0.0;
    return tg_w_raw(x + s_w.ox, z + s_w.oz) - s_w.h0;
}

/* --------------------------------------------------------------- overlay -- */

#define TG_W_CELLS (TG_WORLD_CHUNK * TG_WORLD_CHUNK)

typedef struct {
    int   cx, cz;                       /* chunk coordinates                */
    float tgt[TG_W_CELLS];              /* conform target height            */
    unsigned char w  [TG_W_CELLS];      /* conform weight 0..255            */
    unsigned char occ[TG_W_CELLS];      /* TG_WO_* bits                      */
    signed char   bio[TG_W_CELLS];      /* painted biome, -1 = none         */
} TG_WChunk;

#define TG_W_MAP_CAP 4096               /* open-addressed chunk table       */
static TG_WChunk *s_w_map[TG_W_MAP_CAP];
static int        s_w_nchunks;

static unsigned int tg_w_key_hash(int cx, int cz)
{
    unsigned int h = (unsigned)cx * 0x8DA6B343u ^ (unsigned)cz * 0xD8163841u;
    h ^= h >> 13; h *= 0x2C1B3C6Du; h ^= h >> 16;
    return h;
}

static void tg_w_cell_of(double x, double z, int *cx, int *cz, int *ci)
{
    const int gx = (int)floor(x / TG_WORLD_CELL);
    const int gz = (int)floor(z / TG_WORLD_CELL);
    /* floor division into chunks, for negative coordinates too */
    const int ccx = (gx >= 0) ? gx / TG_WORLD_CHUNK : -((-gx - 1) / TG_WORLD_CHUNK) - 1;
    const int ccz = (gz >= 0) ? gz / TG_WORLD_CHUNK : -((-gz - 1) / TG_WORLD_CHUNK) - 1;
    *cx = ccx; *cz = ccz;
    *ci = (gz - ccz * TG_WORLD_CHUNK) * TG_WORLD_CHUNK + (gx - ccx * TG_WORLD_CHUNK);
}

static TG_WChunk *tg_w_chunk(int cx, int cz, int create)
{
    unsigned int i = tg_w_key_hash(cx, cz) & (TG_W_MAP_CAP - 1);
    int probes = 0;
    while (probes++ < TG_W_MAP_CAP) {
        TG_WChunk *c = s_w_map[i];
        if (!c) {
            if (!create || s_w.frozen) return NULL;
            if (s_w_nchunks >= TG_W_MAP_CAP - 64) return NULL;   /* keep probes short */
            c = (TG_WChunk *)malloc(sizeof(TG_WChunk));
            if (!c) return NULL;
            memset(c, 0, sizeof(*c));
            memset(c->bio, -1, sizeof(c->bio));
            c->cx = cx; c->cz = cz;
            s_w_map[i] = c;
            s_w_nchunks++;
            return c;
        }
        if (c->cx == cx && c->cz == cz) return c;
        i = (i + 1) & (TG_W_MAP_CAP - 1);
    }
    return NULL;
}

static const TG_WChunk *tg_w_chunk_at(double x, double z, int *ci)
{
    int cx, cz;
    tg_w_cell_of(x, z, &cx, &cz, ci);
    return tg_w_chunk(cx, cz, 0);
}

static TG_WChunk *tg_w_chunk_at_w(double x, double z, int *ci)
{
    int cx, cz;
    tg_w_cell_of(x, z, &cx, &cz, ci);
    return tg_w_chunk(cx, cz, 1);
}

/* Conform weight + target at a cell CENTRE, bilinearly blended across the
 * four cells around (x,z). Cells without a stamp contribute weight 0. */
static void tg_w_overlay_at(double x, double z, double *pw, double *ptgt)
{
    const double u = x / TG_WORLD_CELL - 0.5, v = z / TG_WORLD_CELL - 0.5;
    const double fu = floor(u), fv = floor(v);
    const double tu = u - fu, tv = v - fv;
    double wsum = 0.0, tsum = 0.0;
    int k;
    for (k = 0; k < 4; k++) {
        const int dx = k & 1, dz = k >> 1;
        const double bw = (dx ? tu : 1.0 - tu) * (dz ? tv : 1.0 - tv);
        const double cx = (fu + dx + 0.5) * TG_WORLD_CELL;
        const double cz = (fv + dz + 0.5) * TG_WORLD_CELL;
        int ci;
        const TG_WChunk *c = tg_w_chunk_at(cx, cz, &ci);
        if (c && c->w[ci]) {
            const double w = (double)c->w[ci] / 255.0;
            wsum += bw * w;
            tsum += bw * w * (double)c->tgt[ci];
        }
    }
    *pw   = wsum;
    *ptgt = (wsum > 1e-9) ? tsum / wsum : 0.0;
}

double tg_world_h(double x, double z)
{
    double base, w, tgt;
    if (!s_w.built) return 0.0;
    base = tg_world_h_base(x, z);
    if (s_w_nchunks == 0) return base;
    tg_w_overlay_at(x, z, &w, &tgt);
    if (w <= 1e-9) return base;
    if (w > 1.0) w = 1.0;
    return base * (1.0 - w) + tgt * w;
}

double tg_world_sea_y(void) { return s_w.built ? s_w.sea_abs - s_w.h0 : -1e9; }

double tg_world_water_y(double x, double z)
{
    const double sea = tg_world_sea_y();
    double rs;
    if (!s_w.built) return sea;
    if (tg_w_river_t(x + s_w.ox, z + s_w.oz) < 1.0) {
        rs = tg_w_river_surface_raw(x + s_w.ox, z + s_w.oz) - s_w.h0;
        return rs > sea ? rs : sea;
    }
    return sea;
}

int tg_world_is_water(double x, double z)
{
    if (!s_w.built) return 0;
    /* A conformed cell is a ROAD BED (main road, corridor, gore, street): it
     * is never water, even where it lies below a neighbouring river's
     * surface (a cutting beside a river). Bridges were decided from the
     * natural ground before any conform, so they are unaffected. */
    if (s_w_nchunks > 0) {
        double w, tgt;
        tg_w_overlay_at(x, z, &w, &tgt);
        if (w >= 0.5) return 0;
    }
    return tg_world_h(x, z) < tg_world_water_y(x, z);
}

double tg_world_slope(double x, double z)
{
    const double d = TG_WORLD_CELL * 0.5;
    double gx, gz;
    if (!s_w.built) return 0.0;
    gx = (tg_world_h(x + d, z) - tg_world_h(x - d, z)) / (2.0 * d);
    gz = (tg_world_h(x, z + d) - tg_world_h(x, z - d)) / (2.0 * d);
    return sqrt(gx * gx + gz * gz);
}

double tg_world_slope_along(double x, double z, double dx, double dz)
{
    const double d = TG_WORLD_CELL * 0.5;
    if (!s_w.built) return 0.0;
    return (tg_world_h(x + dx * d, z + dz * d) - tg_world_h(x - dx * d, z - dz * d))
         / (2.0 * d);
}

TG_WorldClass tg_world_class(double x, double z)
{
    double h, wy, s;
    if (!s_w.built) return TG_WC_FLAT;
    h  = tg_world_h(x, z);
    wy = tg_world_water_y(x, z);
    if (h < wy)
        return (wy > tg_world_sea_y() + 0.5) ? TG_WC_RIVER : TG_WC_SEA;
    if (h < tg_world_sea_y() + TG_WORLD_SHORE_BAND) return TG_WC_SHORE;
    s = tg_world_slope(x, z);
    if (s >= TG_WORLD_STEEP_SLOPE) return TG_WC_MOUNTAIN;
    if (s >= TG_WORLD_HILL_SLOPE)  return TG_WC_HILL;
    return TG_WC_FLAT;
}

/* --------------------------------------------------------------- conform -- */

void tg_world_conform(double x, double z, double y, double r_flat, double r_blend)
{
    const double R = r_flat + r_blend;
    int gx0, gx1, gz0, gz1, gx, gz;
    if (!s_w.built || s_w.frozen || R <= 0.0) return;
    gx0 = (int)floor((x - R) / TG_WORLD_CELL); gx1 = (int)floor((x + R) / TG_WORLD_CELL);
    gz0 = (int)floor((z - R) / TG_WORLD_CELL); gz1 = (int)floor((z + R) / TG_WORLD_CELL);
    for (gz = gz0; gz <= gz1; gz++) {
        for (gx = gx0; gx <= gx1; gx++) {
            const double cx = (gx + 0.5) * TG_WORLD_CELL;
            const double cz = (gz + 0.5) * TG_WORLD_CELL;
            const double d  = sqrt((cx - x) * (cx - x) + (cz - z) * (cz - z));
            double w;
            int ci;
            TG_WChunk *c;
            if (d > R) continue;
            w = (d <= r_flat) ? 1.0
              : (r_blend > 0.0 ? 1.0 - tg_w_smoothstep(0.0, 1.0, (d - r_flat) / r_blend) : 0.0);
            if (w <= 0.0) continue;
            c = tg_w_chunk_at_w(cx, cz, &ci);
            if (!c) continue;
            {
                const unsigned char wb = (unsigned char)(w * 255.0 + 0.5);
                if (wb > c->w[ci]) {            /* max-composite */
                    c->w[ci]   = wb;
                    c->tgt[ci] = (float)y;
                } else if (wb == c->w[ci] && wb) {
                    /* equal weight: the lower target wins, so a cut through
                     * a ridge is never undone by a neighbouring fill */
                    if ((float)y < c->tgt[ci]) c->tgt[ci] = (float)y;
                }
            }
        }
    }
}

void tg_world_conform_seg(double x0, double z0, double y0,
                          double x1, double z1, double y1,
                          double r_flat, double r_blend)
{
    const double dx = x1 - x0, dz = z1 - z0;
    const double len = sqrt(dx * dx + dz * dz);
    const int n = (int)ceil(len / (TG_WORLD_CELL * 0.5));
    int i;
    for (i = 0; i <= n; i++) {
        const double t = n ? (double)i / (double)n : 0.0;
        tg_world_conform(x0 + dx * t, z0 + dz * t, y0 + (y1 - y0) * t, r_flat, r_blend);
    }
}

/* ------------------------------------------------------------- occupancy -- */

unsigned tg_world_occ(double x, double z)
{
    int ci;
    const TG_WChunk *c;
    if (!s_w.built) return 0;
    c = tg_w_chunk_at(x, z, &ci);
    return c ? c->occ[ci] : 0u;
}

void tg_world_occ_set(double x, double z, unsigned bits)
{
    int ci;
    TG_WChunk *c;
    if (!s_w.built || s_w.frozen) return;
    c = tg_w_chunk_at_w(x, z, &ci);
    if (c) c->occ[ci] |= (unsigned char)bits;
}

void tg_world_occ_disc(double x, double z, double radius, unsigned bits)
{
    int gx0, gx1, gz0, gz1, gx, gz;
    if (!s_w.built || s_w.frozen) return;
    gx0 = (int)floor((x - radius) / TG_WORLD_CELL); gx1 = (int)floor((x + radius) / TG_WORLD_CELL);
    gz0 = (int)floor((z - radius) / TG_WORLD_CELL); gz1 = (int)floor((z + radius) / TG_WORLD_CELL);
    for (gz = gz0; gz <= gz1; gz++)
        for (gx = gx0; gx <= gx1; gx++) {
            const double cx = (gx + 0.5) * TG_WORLD_CELL, cz = (gz + 0.5) * TG_WORLD_CELL;
            if ((cx - x) * (cx - x) + (cz - z) * (cz - z) <= radius * radius)
                tg_world_occ_set(cx, cz, bits);
        }
}

void tg_world_occ_seg(double x0, double z0, double x1, double z1,
                      double half_w, unsigned bits)
{
    const double dx = x1 - x0, dz = z1 - z0;
    const double len = sqrt(dx * dx + dz * dz);
    const int n = (int)ceil(len / (TG_WORLD_CELL * 0.5));
    int i;
    for (i = 0; i <= n; i++) {
        const double t = n ? (double)i / (double)n : 0.0;
        tg_world_occ_disc(x0 + dx * t, z0 + dz * t, half_w, bits);
    }
}

int tg_world_occ_near(double x, double z, double radius, unsigned bits)
{
    int gx0, gx1, gz0, gz1, gx, gz;
    if (!s_w.built || s_w_nchunks == 0) return 0;
    gx0 = (int)floor((x - radius) / TG_WORLD_CELL); gx1 = (int)floor((x + radius) / TG_WORLD_CELL);
    gz0 = (int)floor((z - radius) / TG_WORLD_CELL); gz1 = (int)floor((z + radius) / TG_WORLD_CELL);
    for (gz = gz0; gz <= gz1; gz++)
        for (gx = gx0; gx <= gx1; gx++) {
            const double cx = (gx + 0.5) * TG_WORLD_CELL, cz = (gz + 0.5) * TG_WORLD_CELL;
            if ((cx - x) * (cx - x) + (cz - z) * (cz - z) <= radius * radius &&
                (tg_world_occ(cx, cz) & bits))
                return 1;
        }
    return 0;
}

int tg_world_biome(double x, double z)
{
    int ci;
    const TG_WChunk *c;
    if (!s_w.built) return -1;
    c = tg_w_chunk_at(x, z, &ci);
    return c ? (int)c->bio[ci] : -1;
}

void tg_world_biome_paint(double x, double z, double radius, int biome)
{
    int gx0, gx1, gz0, gz1, gx, gz;
    if (!s_w.built || s_w.frozen) return;
    gx0 = (int)floor((x - radius) / TG_WORLD_CELL); gx1 = (int)floor((x + radius) / TG_WORLD_CELL);
    gz0 = (int)floor((z - radius) / TG_WORLD_CELL); gz1 = (int)floor((z + radius) / TG_WORLD_CELL);
    for (gz = gz0; gz <= gz1; gz++)
        for (gx = gx0; gx <= gx1; gx++) {
            const double cx = (gx + 0.5) * TG_WORLD_CELL, cz = (gz + 0.5) * TG_WORLD_CELL;
            int ci;
            TG_WChunk *c;
            if ((cx - x) * (cx - x) + (cz - z) * (cz - z) > radius * radius) continue;
            c = tg_w_chunk_at_w(cx, cz, &ci);
            if (c && c->bio[ci] < 0) c->bio[ci] = (signed char)biome;   /* first paint wins */
        }
}

/* ------------------------------------------------------------- lifecycle -- */

void tg_world_free(void)
{
    int i;
    for (i = 0; i < TG_W_MAP_CAP; i++) {
        free(s_w_map[i]);
        s_w_map[i] = NULL;
    }
    s_w_nchunks = 0;
    memset(&s_w, 0, sizeof(s_w));
}

int  tg_world_ready(void)  { return s_w.built; }
void tg_world_freeze(void) { s_w.frozen = 1; }
int  tg_world_chunk_count(void) { return s_w_nchunks; }

/* Is the origin a legal start under this sampling offset? Dry with margin,
 * flat, and the lead-in straight (+Z, GRID_SPAN + 16 spans) walkable without
 * any structure. Returns a badness score (0 = perfect). */
static double tg_w_start_badness(double ox, double oz)
{
    const double margin = 2500.0;
    double bad = 0.0;
    int i;
    /* The lead-in runs along +X: TD5_TG_AXIS_HEADING is PI/2 and the walk
     * does x += sin(heading), so the grid straight is the +X axis and the
     * lateral is Z. */
    for (i = 0; i <= TD5_TG_GRID_SPAN + 24; i += 2) {
        const double rx = ox + (double)i * TG_WORLD_CELL;
        const double h  = tg_w_raw(rx, oz);
        const double hl = tg_w_raw(rx, oz - 6000.0), hr = tg_w_raw(rx, oz + 6000.0);
        double s;
        if (h < s_w.sea_abs + margin) bad += (s_w.sea_abs + margin - h) * 4.0;
        if (tg_w_river_t(rx, oz) < 1.0) bad += 5000.0;
        s = fabs(tg_w_raw(rx + 750.0, oz) - tg_w_raw(rx - 750.0, oz)) / 1500.0;
        if (s > 0.06) bad += (s - 0.06) * 40000.0;
        s = fabs(hr - hl) / 12000.0;
        if (s > 0.10) bad += (s - 0.10) * 20000.0;
    }
    return bad;
}

int tg_world_build(unsigned int seed, int target_spans)
{
    int relief_pct, sea_pct, mtn_pct, river_pct;
    tg_world_free();

    s_w.seed = seed;
    s_w.salt = tg_roll_hash_at(0x22010001u, 0);
    s_w.target_spans = target_spans;

    /* Knobs. Each is a seed ROLL unless pinned (unset = roll; the
     * TD5RE_ prefix puts them in the GENSTAMP env hash automatically). */
    relief_pct = td5_env_int("TD5RE_TG_WORLD_RELIEF", -1, -1, 400);
    sea_pct    = td5_env_int("TD5RE_TG_WORLD_SEA",    -999, -999, 60);
    mtn_pct    = td5_env_int("TD5RE_TG_WORLD_MOUNTAINS", -1, -1, 100);
    river_pct  = td5_env_int("TD5RE_TG_WORLD_RIVERS",    -1, -1, 100);
    if (relief_pct < 0) relief_pct = 70 + (int)(tg_roll_hash_at(0x22010002u, 0) % 90u);   /* 70..159 */
    if (sea_pct == -999) sea_pct = -35 + (int)(tg_roll_hash_at(0x22010003u, 0) % 70u);   /* -35..34 */
    if (mtn_pct < 0) mtn_pct = (int)(tg_roll_hash_at(0x22010004u, 0) % 101u);
    if (river_pct < 0) river_pct = (int)(tg_roll_hash_at(0x22010005u, 0) % 101u);

    s_w.relief   = 15000.0 * (double)relief_pct / 100.0;
    /* Sea level against the continental term (+-0.8 relief): the roll maps
     * -35% -> sea at -0.81 (almost no water), 0 -> -0.25 (~30% water),
     * +34% -> +0.29 (archipelago). */
    s_w.sea_abs  = s_w.relief * 0.8 * (((double)sea_pct / 100.0) * 1.6 - 0.25);
    s_w.mtn_bias = -0.20 + 0.7 * (double)mtn_pct / 100.0;      /* -0.20..0.50 */
    s_w.river_w  = (river_pct < 15) ? 0.0 : 0.006 + 0.014 * (double)river_pct / 100.0;
    s_w.built    = 1;                       /* tg_w_raw needs the params */

    /* Start search: a deterministic spiral of offsets, 30000 units apart. */
    {
        double best = 1e30, bx = 0.0, bz = 0.0;
        int k;
        for (k = 0; k < 900; k++) {
            /* square spiral index -> (i,j) */
            int r = (int)floor((sqrt((double)k) + 1.0) / 2.0), i, j, side;
            int base = (2 * r - 1) * (2 * r - 1), d = k - base;
            if (r == 0) { i = 0; j = 0; }
            else {
                side = d / (2 * r); d %= (2 * r);
                switch (side) {
                    case 0:  i = r;      j = -r + d + 1; break;
                    case 1:  i = r - d - 1; j = r;       break;
                    case 2:  i = -r;     j = r - d - 1;  break;
                    default: i = -r + d + 1; j = -r;     break;
                }
            }
            {
                const double ox = 100000.0 + i * 30000.0;
                const double oz = 100000.0 + j * 30000.0;
                const double b  = tg_w_start_badness(ox, oz);
                if (b < best) { best = b; bx = ox; bz = oz; }
                if (b <= 0.0) break;
            }
        }
        s_w.ox = bx; s_w.oz = bz;
        s_w.h0 = tg_w_raw(bx, bz);
        TD5_LOG_I(LOG_TAG, "trackgen: [WORLD] seed=%u relief=%.0f sea=%.0f "
                  "(sea%+d%%) mountains=%d%% rivers=%d%% start-offset=(%.0f,%.0f) "
                  "badness=%.0f after %d candidate(s)",
                  seed, s_w.relief, s_w.sea_abs - s_w.h0, sea_pct, mtn_pct,
                  river_pct, bx, bz, best, k + 1);
    }
    return 1;
}

/* ----------------------------------------------------------- diagnostics -- */

void tg_world_log_stats(const char *tag, double x0, double z0, double x1, double z1)
{
    const int N = 96;
    int i, j, n = 0, sea = 0, river = 0, shore = 0, flat = 0, hill = 0, mtn = 0;
    double hmin = 1e30, hmax = -1e30;
    if (!s_w.built) return;
    for (j = 0; j < N; j++)
        for (i = 0; i < N; i++) {
            const double x = x0 + (x1 - x0) * ((double)i + 0.5) / N;
            const double z = z0 + (z1 - z0) * ((double)j + 0.5) / N;
            const double h = tg_world_h(x, z);
            switch (tg_world_class(x, z)) {
                case TG_WC_SEA:      sea++;   break;
                case TG_WC_RIVER:    river++; break;
                case TG_WC_SHORE:    shore++; break;
                case TG_WC_FLAT:     flat++;  break;
                case TG_WC_HILL:     hill++;  break;
                default:             mtn++;   break;
            }
            if (h < hmin) hmin = h;
            if (h > hmax) hmax = h;
            n++;
        }
    TD5_LOG_I(LOG_TAG, "trackgen: [WORLD] %s box (%.0f,%.0f)-(%.0f,%.0f): "
              "sea %d%% river %d%% shore %d%% flat %d%% hill %d%% mountain %d%%, "
              "height %.0f..%.0f (sea level %.0f), chunks=%d (%zu KB)",
              tag, x0, z0, x1, z1,
              sea * 100 / n, river * 100 / n, shore * 100 / n, flat * 100 / n,
              hill * 100 / n, mtn * 100 / n, hmin, hmax, tg_world_sea_y(),
              s_w_nchunks, (size_t)s_w_nchunks * sizeof(TG_WChunk) / 1024u);
}

void tg_world_dump(const char *dir, double x0, double z0, double x1, double z1)
{
    int N = 1024, M = 1024;   /* columns (x), rows (z): long side 1024 */
    char path[320];
    FILE *f;
    unsigned char *row;
    int i, j;
    double hmin = 1e30, hmax = -1e30;
    if (!s_w.built || !td5_env_flag_off("TD5RE_TG_WORLD_DUMP")) return;
    if (x1 - x0 > z1 - z0) M = (int)(1024.0 * (z1 - z0) / (x1 - x0));
    else                   N = (int)(1024.0 * (x1 - x0) / (z1 - z0));
    if (N < 16) N = 16;
    if (M < 16) M = 16;
    snprintf(path, sizeof path, "%s/WORLD.PGM", dir);
    f = fopen(path, "wb");
    if (!f) return;
    row = (unsigned char *)malloc((size_t)N);
    if (!row) { fclose(f); return; }
    for (j = 0; j < M; j += 8)
        for (i = 0; i < N; i += 8) {
            const double h = tg_world_h(x0 + (x1 - x0) * i / N, z0 + (z1 - z0) * j / M);
            if (h < hmin) hmin = h;
            if (h > hmax) hmax = h;
        }
    if (hmax - hmin < 1.0) hmax = hmin + 1.0;
    fprintf(f, "P5\n%d %d\n255\n", N, M);
    /* z grows DOWN the image (row 0 = z1), x grows right. Water is drawn at
     * 0..31 so the coastline reads at a glance; land maps to 64..255. */
    for (j = M - 1; j >= 0; j--) {
        const double z = z0 + (z1 - z0) * ((double)j + 0.5) / M;
        for (i = 0; i < N; i++) {
            const double x = x0 + (x1 - x0) * ((double)i + 0.5) / N;
            const double h = tg_world_h(x, z);
            unsigned occ = tg_world_occ(x, z);
            int v;
            if (occ & (TG_WO_ROAD | TG_WO_DRIVABLE)) v = 255;
            else if (occ & TG_WO_STREET) v = 200;
            else if (h < tg_world_water_y(x, z)) v = 8 + (int)(23.0 * (h - hmin) / (hmax - hmin));
            else v = 64 + (int)(160.0 * (h - hmin) / (hmax - hmin));
            if (v < 0) v = 0;
            if (v > 255) v = 255;
            row[i] = (unsigned char)v;
        }
        fwrite(row, 1, (size_t)N, f);
    }
    free(row);
    fclose(f);
    TD5_LOG_I(LOG_TAG, "trackgen: [WORLD] heightmap dumped to %s (%dx%d, "
              "box (%.0f,%.0f)-(%.0f,%.0f), height %.0f..%.0f)",
              path, N, M, x0, z0, x1, z1, hmin, hmax);
}
