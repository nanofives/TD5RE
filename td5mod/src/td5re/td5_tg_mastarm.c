/* td5_tg_mastarm.c -- GEO TRACK: mast-arm traffic lights + street-name plates
 * (PORT-ONLY, no original counterpart), with the TEXT -> TRACK PAGE emitter
 * they need.
 *
 * [ROUND 1021 D] La Plata signals are not a lone head on a kerb post. The pole
 * stands at the corner, a horizontal arm reaches out over the carriageway, the
 * heads hang from it over the lanes, and a blue plate with the cross street's
 * name in white capitals ("AVENIDA 51") is hung from the arm. This module is
 * that, built on the same placement td5_tg_furniture.c's geo signals already
 * decide (which OSM node, which span, which side, how far out): the arm is a
 * different way of dressing the SAME pole, so nothing about WHERE a signal
 * stands changes.
 *
 * THREE JOBS.
 *   1. PLATES, decided before anything is emitted. For every SIGNALS.JSON node
 *      near the route: the route street (ROUTE.JSON's own name, else the
 *      nearest way) and the cross street (nearest OTHER-named way at the
 *      junction, the same "skip the race street's interned id" rule the HUD
 *      uses). The cross name faces the approaching driver, the route name the
 *      driver who has just passed. See pl_build.
 *   2. TEXT -> TRACK PAGE. A TEXTURES.DAT page is 64x64 at <=256 colours and
 *      the container carries no other size, so a plate is a CELL: 64x16 for one
 *      line, 64x32 for two, packed several to a page (4 rows of 16). Each cell
 *      is rasterised from the TTF at generation time (td5_font_plate_raster),
 *      white on blue with a white border, antialiased through a 16-step ramp.
 *      The cells live on EXTRA pages appended after TD5_TG_PAGE_COUNT, and only
 *      when this module has plates: a synthetic build, and a geo build with the
 *      knob off, write exactly the page file they always did.
 *   3. THE MAST ARM mesh: pole, arm, heads with backplates and the three lamp
 *      billboards each (the existing TD5_MESH_TAG_SIGNAL_LAMP cycle, so the
 *      lamp phase logic in the renderer is untouched), and one two-faced plate
 *      quad.
 *
 * WHY THE TABLE IS BUILT FROM DATA, NOT FROM THE NODE LIST. A streamed build
 * writes TEXTURES.DAT BEFORE the prepass that sees the node list (the pages
 * must exist before the level load); a non-streamed build writes it AFTER the
 * models. Either order has to find the same plates, so pl_build reads only the
 * route, the signals and the roads cache -- all loaded before generation
 * starts -- and is a pure function of them. It is simply run again wherever it
 * is needed.
 *
 * NO COLLISION. The pole keeps the footprint the old head had (same placement,
 * same on-road guard class). Everything that reaches over the carriageway is
 * above TD5_TG_GUARD_OVERHEAD (4.4 m); the lowest piece, the head, is 4.7 m
 * over the road surface at its own span. The guard still judges every mesh.
 *
 * KNOB. TD5RE_GEO_MASTARM=0 restores the post light bit for bit. A signal with
 * no known street name, or on a carriageway too narrow for an arm, keeps the
 * post light as well. Inert on a synthetic build (td5_geo_loaded() gate first,
 * no tg_rand draws anywhere).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "td5re.h"
#include "td5_platform.h"
#include "td5_config.h"
#include "td5_font.h"
#include "td5_geo.h"
#include "td5_geo_signals.h"
#include "td5_geo_roads.h"
#include "td5_trackgen.h"
#include "td5_trackgen_internal.h"

#define LOG_TAG "track"

/* ----------------------------------------------------------------- cells -- */

#define PL_W            64          /* a cell is the full page width            */
#define PL_ROW_H        16          /* one text row                              */
#define PL_ROWS         (TD5_TG_TEX_DIM / PL_ROW_H)
#define PL_CELLS_MAX    128
#define PL_NAME_MAX     96
#define PL_PAD          3           /* border ring + breathing room, in texels   */

/* Smallest cap height (texels) a plate may carry and still be called legible.
 * One text row is 16 texels tall and 0.35 m of plate, so a texel is 2.2 cm and
 * a 7.5 texel cap is a 16 cm letter -- readable from the next junction. Two rows
 * share a cell twice as tall, with the same texel size. */
#define PL_MIN_CAP_1    7.5f
#define PL_MIN_CAP_2    6.8f

typedef struct {
    char name[PL_NAME_MAX];   /* the OSM name, exactly as keyed                 */
    int  page;                /* 0-based among the EXTRA pages                  */
    int  y;                   /* first texel row on the page                    */
    int  h;                   /* PL_ROW_H or 2 * PL_ROW_H                       */
    int  lines;
    int  forced;              /* 1 = made two rows tall to match its partner face */
    float cap;                /* cap height actually drawn, for the census      */
} PlCell;

typedef struct { int front, back; } PlSig;

static PlCell        s_cell[PL_CELLS_MAX];
static int           s_ncell;
static unsigned char s_pg[TD5_TG_MASTARM_PAGES_MAX][TD5_TG_TEX_TEXELS];
static int           s_pg_rows[TD5_TG_MASTARM_PAGES_MAX];
static int           s_npg;
/* The countdown display page: grey ramp, one 64x64 dot-matrix canvas. */
static unsigned char s_dsp[TD5_TG_TEX_TEXELS];
static int           s_have_dsp;
static PlSig        *s_sp;
static int           s_nsig;

/* Census. Written by the single-threaded build and plan calls only. */
static long s_ma_sigs, s_ma_named, s_ma_cross, s_ma_cells, s_ma_two_line,
            s_ma_abbrev, s_ma_nopage, s_ma_noface;
static long s_ma_planned, s_ma_old_noname, s_ma_old_narrow, s_ma_old_gantry;

int tg_mastarm_enabled(void)
{
    if (!td5_geo_loaded()) return 0;                       /* byte-identical */
    if (!td5_env_flag_on("TD5RE_GEO_SIGNALS")) return 0;
    return td5_env_flag_on("TD5RE_GEO_MASTARM");
}

/* ----------------------------------------------------------------- text --- */

/* Upper-case a UTF-8 street name. ASCII and the Latin-1 letters Spanish uses
 * (a-acute .. u-diaeresis, n-tilde) -- the same range the HUD font carries real
 * outlines for. Anything else is copied through untouched. */
static void pl_upper(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    while (*in && o + 4 < cap) {
        const unsigned char c = (unsigned char)*in;
        if (c < 0x80) {
            out[o++] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : (char)c;
            in++;
        } else if ((c & 0xE0) == 0xC0 && ((unsigned char)in[1] & 0xC0) == 0x80) {
            int cp = ((c & 0x1F) << 6) | ((unsigned char)in[1] & 0x3F);
            if (cp >= 0xE0 && cp <= 0xFE && cp != 0xF7) cp -= 0x20;
            out[o++] = (char)(0xC0 | (cp >> 6));
            out[o++] = (char)(0x80 | (cp & 0x3F));
            in += 2;
        } else {
            out[o++] = *in++;
        }
    }
    out[o] = '\0';
}

typedef struct { const char *full, *abbr; } PlAbbr;
/* The words OSM spells out that a real plate abbreviates. Applied one word at a
 * time, longest word first, and ONLY until the name fits: "DIAGONAL 73" stays
 * "DIAGONAL 73" when it can. */
static const PlAbbr k_abbr[] = {
    { "PRESIDENTE", "PTE." },   { "INTENDENTE", "INT." },
    { "ALMIRANTE",  "ALTE." },  { "TENIENTE",   "TTE." },
    { "INGENIERO",  "ING." },   { "SARGENTO",   "SGTO." },
    { "PROFESOR",   "PROF." },  { "CORONEL",    "CNEL." },
    { "GENERAL",    "GRAL." },  { "DIAGONAL",   "DIAG." },
    { "AVENIDA",    "AV." },    { "PASAJE",     "PJE." },
    { "PLAZA",      "PZA." },
    { "DOCTOR",     "DR." },    { "CAPITAN",    "CAP." },
};
#define PL_NABBR ((int)(sizeof k_abbr / sizeof k_abbr[0]))

/* Replace the longest not-yet-abbreviated known word in `s`. 1 if one changed. */
static int pl_abbreviate_one(char *s, size_t cap)
{
    int best = -1, i;
    size_t bl = 0;
    char *bp = NULL;
    for (i = 0; i < PL_NABBR; i++) {
        const size_t fl = strlen(k_abbr[i].full);
        char *p = strstr(s, k_abbr[i].full);
        while (p) {
            const int lead = (p == s) || (p[-1] == ' ');
            const int tail = (p[fl] == '\0') || (p[fl] == ' ');
            if (lead && tail) {
                if (fl > bl) { bl = fl; best = i; bp = p; }
                break;
            }
            p = strstr(p + 1, k_abbr[i].full);
        }
    }
    if (best < 0) return 0;
    {
        const size_t al = strlen(k_abbr[best].abbr);
        const size_t rest = strlen(bp + bl);
        if ((size_t)(bp - s) + al + rest + 1 > cap) return 0;
        memmove(bp + al, bp + bl, rest + 1);
        memcpy(bp, k_abbr[best].abbr, al);
    }
    return 1;
}

/* Split `s` at the space that best balances the two halves. Returns 0 when it
 * has no space to split at. */
static int pl_split2(const char *s, char *l1, char *l2, size_t cap)
{
    const size_t n = strlen(s);
    size_t i, best = 0;
    float bestd = 1e9f;
    for (i = 1; i + 1 < n; i++) {
        float a, b, d;
        char t1[PL_NAME_MAX];
        if (s[i] != ' ') continue;
        memcpy(t1, s, i); t1[i] = '\0';
        a = td5_font_plate_fit_cap(t1, PL_W, PL_PAD);
        b = td5_font_plate_fit_cap(s + i + 1, PL_W, PL_PAD);
        /* The widest half sets the shared cap; the split that leaves the
         * LARGER cap wins, i.e. the larger of the two fit caps is irrelevant
         * and the smaller must be maximised. */
        d = -((a < b) ? a : b);
        if (d < bestd) { bestd = d; best = i; }
    }
    if (!best) return 0;
    if (best + 1 > cap || n - best > cap) return 0;
    memcpy(l1, s, best); l1[best] = '\0';
    snprintf(l2, cap, "%s", s + best + 1);
    return 1;
}

/* Decide the layout of an (upper-case) name: one row, or two. Fills l1/l2 and
 * returns the number of rows (1 or 2). Abbreviates only as far as needed. */
static int pl_plan_text(const char *up, char *l1, char *l2, size_t cap,
                        int *abbreviated)
{
    char w[PL_NAME_MAX];
    int rows = 1;
    float c1, c2 = 0.0f;
    *abbreviated = 0;
    snprintf(w, sizeof w, "%s", up);
    for (;;) {
        c2 = 0.0f;
        c1 = td5_font_plate_fit_cap(w, PL_W, PL_PAD);
        if (c1 >= PL_MIN_CAP_1) { snprintf(l1, cap, "%s", w); l2[0] = 0; return 1; }
        if (pl_split2(w, l1, l2, cap)) {
            const float a = td5_font_plate_fit_cap(l1, PL_W, PL_PAD);
            const float b = td5_font_plate_fit_cap(l2, PL_W, PL_PAD);
            c2 = (a < b) ? a : b;
            if (c2 >= PL_MIN_CAP_2) return 2;
        }
        /* Neither fits legibly: shorten one word and look again. */
        if (!pl_abbreviate_one(w, sizeof w)) break;
        *abbreviated = 1;
    }
    /* Nothing left to abbreviate: take whichever layout is the larger. */
    if (c2 > c1 && pl_split2(w, l1, l2, cap)) rows = 2;
    else { snprintf(l1, cap, "%s", w); l2[0] = 0; rows = 1; }
    return rows;
}

/* ---------------------------------------------------------------- pages --- */

/* Palette: entry 0 is the plate blue, entry 15 white, the rest a straight ramp.
 * Entries are stored BGR, like every other page emitter. */
static void pl_pal(int i, unsigned char *b, unsigned char *g, unsigned char *r)
{
    static const int k_bg[3] = { 150, 56, 22 };       /* B, G, R: plate blue */
    const int t = i;                                   /* 0..15               */
    *b = (unsigned char)(k_bg[0] + (255 - k_bg[0]) * t / 15);
    *g = (unsigned char)(k_bg[1] + (255 - k_bg[1]) * t / 15);
    *r = (unsigned char)(k_bg[2] + (255 - k_bg[2]) * t / 15);
}

/* Find (or open) room for a cell `rows` text rows tall. Returns the page index
 * and writes the first row, or -1 when every extra page is spoken for. */
static int pl_alloc(int rows, int *row0)
{
    int p;
    for (p = 0; p < s_npg; p++) {
        if (s_pg_rows[p] + rows <= PL_ROWS) {
            *row0 = s_pg_rows[p];
            s_pg_rows[p] += rows;
            return p;
        }
    }
    if (s_npg >= TD5_TG_MASTARM_PAGES_MAX - 1) return -1;   /* one slot is the display */
    p = s_npg++;
    memset(s_pg[p], 0, sizeof s_pg[p]);
    s_pg_rows[p] = rows;
    *row0 = 0;
    return p;
}

/* How many text rows the name would take, without touching a page. */
static int pl_rows_of(const char *name)
{
    char up[PL_NAME_MAX], l1[PL_NAME_MAX], l2[PL_NAME_MAX];
    int abbr = 0;
    if (!name || !name[0]) return 0;
    pl_upper(name, up, sizeof up);
    l2[0] = 0;
    return pl_plan_text(up, l1, l2, sizeof l1, &abbr);
}

/* Rasterise one name into a fresh cell. Returns the cell index or -1. */
static int pl_cell_for(const char *name, int force2, int verbose)
{
    char up[PL_NAME_MAX], l1[PL_NAME_MAX], l2[PL_NAME_MAX];
    unsigned char cov[PL_W * PL_ROW_H * 2];
    PlCell *c;
    int i, rows, lines, abbr = 0, row0 = 0, pg, x, y, h;
    float cap = 0.0f;

    if (!name || !name[0]) return -1;
    for (i = 0; i < s_ncell; i++)
        if (s_cell[i].forced == force2 && strcmp(s_cell[i].name, name) == 0) return i;
    if (s_ncell >= PL_CELLS_MAX) { s_ma_nopage++; return -1; }

    pl_upper(name, up, sizeof up);
    l2[0] = '\0';
    lines = pl_plan_text(up, l1, l2, sizeof l1, &abbr);
    /* A plate's two faces hang in one frame, so they must be the same height:
     * the shorter one is remade two rows tall -- split over two lines where the
     * name has a space, else one line centred -- rather than shown with the
     * other face's text seen through the gap, mirrored. */
    if (force2 && lines == 1) {
        char one[PL_NAME_MAX];
        snprintf(one, sizeof one, "%s", l1);
        if (pl_split2(one, l1, l2, sizeof l1)) lines = 2;
    }
    rows = force2 ? 2 : lines;
    h = rows * PL_ROW_H;
    if (!td5_font_plate_raster(l1, lines == 2 ? l2 : NULL, PL_W, h, PL_PAD, cov,
                               &cap)) {
        s_ma_noface++;
        return -1;
    }
    pg = pl_alloc(rows, &row0);
    if (pg < 0) { s_ma_nopage++; return -1; }

    /* The 1-texel outer ring stays plate blue on purpose: scenery samples
     * LINEAR + WRAP, so the quad's UVs are inset to texel centres and anything
     * a bilinear tap picks up beyond the edge is blue, not the next cell. */
    for (y = 0; y < h; y++) {
        for (x = 0; x < PL_W; x++) {
            int v = (cov[y * PL_W + x] * 15 + 127) / 255;
            const int ring = (x == 1 || x == PL_W - 2 || y == 1 || y == h - 2)
                          && x >= 1 && x <= PL_W - 2 && y >= 1 && y <= h - 2;
            if (ring) v = 15;
            s_pg[pg][(row0 * PL_ROW_H + y) * TD5_TG_TEX_DIM + x] = (unsigned char)v;
        }
    }
    c = &s_cell[s_ncell];
    snprintf(c->name, sizeof c->name, "%s", name);
    c->page = pg; c->y = row0 * PL_ROW_H; c->h = h; c->lines = lines;
    c->forced = force2; c->cap = cap;
    s_ma_cells++;
    if (rows == 2) s_ma_two_line++;
    if (abbr) s_ma_abbrev++;
    if (verbose)
        TD5_LOG_I(LOG_TAG, "[GEO MASTARM] plate '%s' -> %s%s%s%s cap=%.1f page+%d row %d%s",
              name, l1, lines == 2 ? " / " : "", lines == 2 ? l2 : "",
              abbr ? " (abbreviated)" : "", cap, pg, row0,
              force2 ? " (matched to the other face)" : "");
    return s_ncell++;
}

/* ---------------------------------------------------------------- build --- */

#define PL_NEAR_M   30.0   /* a signal belongs to the route within this        */
#define PL_SEEK_M   14.0   /* route node -> the way it drives                  */
#define PL_CROSS_M  26.0   /* signal node -> the cross street                  */
#define PL_UPM      430.0

static void pl_reset(int plan_too)
{
    free(s_sp);
    s_sp = NULL;
    s_nsig = 0;
    s_ncell = 0;
    s_npg = 0;
    s_have_dsp = 0;
    memset(s_pg_rows, 0, sizeof s_pg_rows);
    s_ma_sigs = s_ma_named = s_ma_cross = s_ma_cells = s_ma_two_line = 0;
    s_ma_abbrev = s_ma_nopage = s_ma_noface = 0;
    if (plan_too) s_ma_planned = s_ma_old_noname = s_ma_old_narrow = s_ma_old_gantry = 0;
}

/* Pure function of the route + signals + roads caches (see the header note). */
static void pl_build(int verbose)
{
    int n, nr, i;

    pl_reset(verbose);
    if (!tg_mastarm_enabled()) return;
    n  = td5_geo_signals_sync();
    nr = td5_geo_route_count();
    if (n <= 0 || nr < 2) return;
    td5_geo_roads_sync(td5_geo_place_slug());

    s_sp = (PlSig *)malloc((size_t)n * sizeof(PlSig));
    if (!s_sp) return;
    s_nsig = n;
    s_ma_sigs = n;

    for (i = 0; i < n; i++) {
        double x, z, bd = 1e300, rx = 0.0, rz = 0.0;
        int j, bj = -1;
        const TD5_GeoRoad *rd, *cd;
        const char *rn, *cn = NULL;
        char rname[PL_NAME_MAX], cname[PL_NAME_MAX];
        int skip = -1;

        s_sp[i].front = s_sp[i].back = -1;
        if (!td5_geo_signals_get(i, &x, &z)) continue;
        for (j = 0; j < nr; j++) {
            double nx = 0.0, nz = 0.0, d;
            td5_geo_route_node(j, &nx, &nz, NULL);
            d = (nx - x) * (nx - x) + (nz - z) * (nz - z);
            if (d < bd) { bd = d; bj = j; rx = nx; rz = nz; }
        }
        if (bj < 0 || bd > (PL_NEAR_M * PL_UPM) * (PL_NEAR_M * PL_UPM)) continue;

        /* The street the race drives: the route's own name, else the way. */
        rd = td5_geo_roads_nearest(rx, rz, PL_SEEK_M * PL_UPM, -1, NULL, NULL, NULL);
        rn = td5_geo_route_name(bj);
        if (!rn || !rn[0]) rn = rd ? td5_geo_roads_name(rd) : NULL;
        if (rd && rd->name_id >= 0) skip = rd->name_id;

        /* The cross street: the nearest other-named way at the junction. */
        cd = td5_geo_roads_nearest(x, z, PL_CROSS_M * PL_UPM, skip, NULL, NULL, NULL);
        if (cd) cn = td5_geo_roads_name(cd);

        snprintf(rname, sizeof rname, "%s", (rn && rn[0]) ? rn : "");
        snprintf(cname, sizeof cname, "%s", (cn && cn[0]) ? cn : "");
        /* A cross street that IS the race street (a second way of it) is no
         * cross street at all. */
        if (rname[0] && strcmp(rname, cname) == 0) cname[0] = '\0';
        if (!rname[0] && !cname[0]) continue;

        s_ma_named++;
        if (cname[0]) s_ma_cross++;
        {
            const char *fn = cname[0] ? cname : rname;
            const char *bn = rname[0] ? rname : cname;
            /* Both faces must end up the same height: the single-row one is
             * remade two rows tall (see pl_cell_for). */
            const int rf = pl_rows_of(fn), rb = pl_rows_of(bn);
            s_sp[i].front = pl_cell_for(fn, rf < rb, verbose);
            s_sp[i].back  = pl_cell_for(bn, rb < rf, verbose);
        }
    }
}

/* The countdown display: a dot-matrix "25" on a 64x64 canvas, grey ramp (the
 * billboard that carries it is additive and takes its colour from the lamp
 * phase, so white here becomes green there). The quad is 0.40 x 0.26 m, so the
 * glyphs are rasterised into a 64x42 box (the quad's aspect) and stretched to
 * the canvas rows; a 4-texel dot pitch with a 3x3 core gives the matrix look.
 * STATIC digits: the renderer can swap a lamp's colour per frame from the wall
 * clock but not a texture, so the number does not count down. */
static void pl_build_display(void)
{
    unsigned char cov[64 * 42];
    int x, y;
    memset(s_dsp, 0, sizeof s_dsp);
    s_have_dsp = 0;
    if (!td5_font_plate_raster("25", NULL, 64, 42, 2, cov, NULL)) return;
    for (y = 0; y < 64; y++) {
        for (x = 0; x < 64; x++) {
            const int cx = (x / 4) * 4 + 2, cy = (y / 4) * 4 + 2;
            const int sy = cy * 42 / 64;
            const int core = (x % 4) < 3 && (y % 4) < 3;
            if (core && cov[sy * 64 + cx] > 110)
                s_dsp[y * TD5_TG_TEX_DIM + x] = 15;
        }
    }
    s_have_dsp = 1;
}

void tg_mastarm_prepare(void)
{
    pl_build(1);
    if (s_ncell > 0) pl_build_display();
}

int tg_mastarm_plate_pages(void)
{
    pl_build(0);
    if (s_ncell > 0) pl_build_display();
    return s_npg + (s_have_dsp ? 1 : 0);
}

void tg_mastarm_emit_plate_page(TG_Buf *out, int p)
{
    int i;
    const int is_dsp = (p == s_npg);                    /* the page after the plates */
    tg_put_u8(out, 0); tg_put_u8(out, 0); tg_put_u8(out, 0);
    tg_put_u8(out, 0);                                  /* opaque */
    tg_put_u32(out, TD5_TG_PAL_COUNT);
    for (i = 0; i < TD5_TG_PAL_COUNT; i++) {
        unsigned char b, g, r;
        if (is_dsp) b = g = r = (unsigned char)(i * 17);
        else        pl_pal(i, &b, &g, &r);
        tg_put_u8(out, b); tg_put_u8(out, g); tg_put_u8(out, r);
    }
    for (i = 0; i < TD5_TG_TEX_TEXELS; i++)
        tg_put_u8(out, is_dsp ? s_dsp[i]
                              : ((p >= 0 && p < s_npg) ? s_pg[p][i] : 0u));
}

/* ------------------------------------------------------------ the arm ---- */

#define M_(m)         ((m) * TD5_TG_INFRA_M)
#define MA_POLE_TOP   M_(6.30)    /* above the road surface at the span        */
#define MA_POLE_HW    M_(0.10)
#define MA_ARM_Y      M_(5.80)    /* arm axis above the road surface           */
#define MA_ARM_HT     M_(0.07)
#define MA_PLATE_W    M_(1.40)
#define MA_PLATE_ROW  M_(0.35)    /* 16 texels (2.2 cm each)                   */
#define MA_PLATE_OFF  M_(0.03)    /* the two faces stand this far off the axis */
#define MA_HEAD_H     M_(0.95)
#define MA_HEAD_W     M_(0.34)
#define MA_HEAD_D     M_(0.30)
#define MA_LAMP_R     M_(0.13)
#define MA_LAMP_DY    M_(0.29)
#define MA_BP_HW      M_(0.30)
#define MA_BP_HH      M_(0.62)
#define MA_BP_HD      M_(0.02)
/* Narrowest carriageway (metres, geo frame) that still gets an arm. Below it
 * the arm would be shorter than the plate it carries. */
#define MA_MIN_WIDTH_M 5.5

/* Does this signal get a mast arm, and with which plates? Called from the
 * single-threaded prepass so the census cannot race. */
int tg_mastarm_plan(int sig, double carriageway_width, int near_gantry,
                    int *front, int *back)
{
    if (!tg_mastarm_enabled() || sig < 0 || sig >= s_nsig || !s_sp) return 0;
    /* The start/finish gantry spans the road at its own span; an arm through
     * its banner is clutter, so those few signals keep the plain post. */
    if (near_gantry) { s_ma_old_gantry++; return 0; }
    if (s_sp[sig].front < 0 && s_sp[sig].back < 0) { s_ma_old_noname++; return 0; }
    if (carriageway_width < MA_MIN_WIDTH_M * PL_UPM) { s_ma_old_narrow++; return 0; }
    *front = s_sp[sig].front >= 0 ? s_sp[sig].front : s_sp[sig].back;
    *back  = s_sp[sig].back  >= 0 ? s_sp[sig].back  : s_sp[sig].front;
    s_ma_planned++;
    return 1;
}

/* Meshes one arm needs, so the caller can fall back to the post light when the
 * entry's budget cannot hold it. */
int tg_mastarm_mesh_count(double carriageway_width)
{
    const int nh = (carriageway_width * 0.5 >= 3.8 * PL_UPM) ? 2 : 1;
    /* pole + arm, heads (housing, backplate, lamps), the cross-direction head
     * (housing + lamps), the countdown (box + lit face), two plate brackets,
     * the plate */
    return 2 + nh * (2 + TD5_GEO_SIGNAL_PHASES) + (1 + TD5_GEO_SIGNAL_PHASES)
             + 2 + 2 + 1;
}

/* One plate quad pair. `u0` is the inboard distance of the plate's near edge
 * from the pole axis. Two segments, front then back, each its own cell. */
static int ma_emit_plate(TG_Buf *blk, const double pole[2], const double l[2],
                         const double t[2], double side, double u0, double y_top,
                         int cf, int cb)
{
    double px[8], py[8], pz[8], uu[8], vv[8];
    int seg_page[2], seg_nq[2];
    const PlCell *c[2];
    int face, k;

    c[0] = &s_cell[cf];
    c[1] = &s_cell[cb];
    for (face = 0; face < 2; face++) {
        /* Which end of the plate is on the viewer's LEFT. MEASURED from a tour
         * frame, not derived: the first cut assumed a left-handed frame and
         * drew the text mirrored. For the front face (seen by a driver
         * approaching along the tangent) and side < 0 the far (large u) end is
         * on the left; the back face is seen from the other side, which swaps
         * it. */
        const double h  = (double)c[face]->h / (double)PL_ROW_H * MA_PLATE_ROW;
        const double ua = u0, ub = u0 + MA_PLATE_W;
        const int far_left = (face == 0) ? (side < 0.0) : (side > 0.0);
        const double ul = far_left ? ub : ua;
        const double ur = far_left ? ua : ub;
        const double off = (face == 0) ? -MA_PLATE_OFF : MA_PLATE_OFF;
        const double ty = y_top, by = y_top - h;
        const double U0 = 1.5 / 64.0, U1 = 62.5 / 64.0;
        const double V0 = ((double)c[face]->y + 1.5) / 64.0;
        const double V1 = ((double)(c[face]->y + c[face]->h) - 1.5) / 64.0;
        double qx[4], qz[4];
        const double us[4] = { ul, ur, ur, ul };
        for (k = 0; k < 4; k++) {
            qx[k] = pole[0] - l[0] * us[k] + t[0] * off;
            qz[k] = pole[1] - l[1] * us[k] + t[1] * off;
        }
        for (k = 0; k < 4; k++) {
            const int o = face * 4 + k;
            px[o] = qx[k]; pz[o] = qz[k];
            py[o] = (k < 2) ? by : ty;                 /* BL BR TR TL */
            uu[o] = (k == 0 || k == 3) ? U0 : U1;
            vv[o] = (k < 2) ? V1 : V0;
        }
        seg_page[face] = TD5_TG_PAGE_COUNT + c[face]->page;
        seg_nq[face]   = 1;
    }
    return tg_write_quad_mesh(blk, px, py, pz, uu, vv, 8, seg_page, seg_nq, 2);
}

int tg_emit_mastarm(const TG_NodeList *nl, int si, double side, double gap,
                    double base_y, int cf, int cb,
                    TG_Buf *blk, size_t *moff, int *nmesh)
{
    const TG_Node *n = &nl->v[si];
    const double t[2]  = { n->tx, n->tz };
    /* outward lateral, toward the pole's own kerb */
    const double l[2]  = { n->tz * side, -n->tx * side };
    const double hw    = n->width * 0.5;
    const double pole[2] = { n->x + l[0] * (hw + gap), n->z + l[1] * (hw + gap) };
    const int nh = (hw >= 3.8 * PL_UPM) ? 2 : 1;
    const double road_y = n->y;
    const double arm_y  = road_y + MA_ARM_Y;
    const double top_y  = road_y + MA_POLE_TOP;
    const double reach  = gap + hw + 0.4 * PL_UPM;   /* a little past the centreline */
    double armlen = reach;
    const double plate_u0 = MA_POLE_HW + M_(0.15);
    int k, lamp;

    if (armlen < plate_u0 + MA_PLATE_W + M_(0.3))
        armlen = plate_u0 + MA_PLATE_W + M_(0.3);

    /* POLE. Same footprint as the post it replaces, carried up to the arm. */
    moff[(*nmesh)++] = blk->len;
    if (!tg_emit_box_mesh(blk, pole[0], (base_y + top_y) * 0.5, pole[1],
                          MA_POLE_HW, (top_y - base_y) * 0.5, MA_POLE_HW,
                          t[0], t[1], TD5_TG_PAGE_R11_SIGN_POST, MA_POLE_HW * 2.0,
                          0xFF606064u))
        return 0;

    /* ARM. A long thin box along the lateral axis (the box's "right" axis is
     * (fz,-fx), so fwd = (-lz, lx) puts it on l). The tile is enormous so the
     * post page's cross-tube shading is sampled at one flat mid-grey. */
    moff[(*nmesh)++] = blk->len;
    if (!tg_emit_box_mesh(blk, pole[0] - l[0] * armlen * 0.5, arm_y,
                          pole[1] - l[1] * armlen * 0.5,
                          armlen * 0.5, MA_ARM_HT, MA_ARM_HT,
                          -l[1], l[0], TD5_TG_PAGE_R11_SIGN_POST, 1.0e7,
                          0xFF70747Cu))
        return 0;

    /* HEADS over the lane centres of the pole-side half. */
    for (k = 0; k < nh; k++) {
        const double u  = gap + hw * ((double)k + 0.5) / (double)nh;
        const double hx = pole[0] - l[0] * u;
        const double hz = pole[1] - l[1] * u;
        const double head_top = arm_y - MA_ARM_HT + M_(0.02);
        const double head_y   = head_top - MA_HEAD_H * 0.5;
        const double off      = MA_HEAD_D * 0.5 + MA_LAMP_R * 0.35;

        moff[(*nmesh)++] = blk->len;
        if (!tg_emit_box_mesh(blk, hx, head_y, hz,
                              MA_HEAD_W * 0.5, MA_HEAD_H * 0.5, MA_HEAD_D * 0.5,
                              t[0], t[1], TD5_TG_PAGE_R11_SIGN_POST, MA_HEAD_W,
                              0xFF242428u))
            return 0;
        /* BACKPLATE: the dark board behind the housing that makes a lit lens
         * read as a signal from a long way off. */
        moff[(*nmesh)++] = blk->len;
        if (!tg_emit_box_mesh(blk, hx + t[0] * (MA_HEAD_D * 0.5 + MA_BP_HD),
                              head_y, hz + t[1] * (MA_HEAD_D * 0.5 + MA_BP_HD),
                              MA_BP_HW, MA_BP_HH, MA_BP_HD,
                              t[0], t[1], TD5_TG_PAGE_R11_SIGN_POST, MA_HEAD_W,
                              0xFF161618u))
            return 0;
        for (lamp = 0; lamp < TD5_GEO_SIGNAL_PHASES; lamp++) {
            const double ly = head_y + (1.0 - (double)lamp) * MA_LAMP_DY;
            moff[(*nmesh)++] = blk->len;
            if (!tg_emit_billboard_mesh(blk, hx - t[0] * off,
                                        ly - MA_LAMP_R, hz - t[1] * off,
                                        MA_LAMP_R, MA_LAMP_R * 2.0,
                                        tg_prop_slot(PP_LAMP),
                                        TD5_MESH_TAG_SIGNAL_LAMP + lamp))
                return 0;
        }
    }

    /* CROSS-DIRECTION HEAD, turned a quarter and mounted at the arm's tip so
     * traffic on the cross street sees it. The lamps use the same phase tags as
     * the main heads, so it shows the SAME state, not the opposite one: the
     * renderer colours a lamp from its tag alone and there is no tag for "the
     * other direction". Cosmetic. */
    {
        const double u  = reach - M_(0.22);
        const double hx = pole[0] - l[0] * u;
        const double hz = pole[1] - l[1] * u;
        const double head_y = arm_y - MA_ARM_HT + M_(0.02) - MA_HEAD_H * 0.5;
        const double off    = MA_HEAD_D * 0.5 + MA_LAMP_R * 0.35;
        moff[(*nmesh)++] = blk->len;
        if (!tg_emit_box_mesh(blk, hx, head_y, hz,
                              MA_HEAD_W * 0.5, MA_HEAD_H * 0.5, MA_HEAD_D * 0.5,
                              l[0], l[1], TD5_TG_PAGE_R11_SIGN_POST, MA_HEAD_W,
                              0xFF242428u))
            return 0;
        for (lamp = 0; lamp < TD5_GEO_SIGNAL_PHASES; lamp++) {
            const double ly = head_y + (1.0 - (double)lamp) * MA_LAMP_DY;
            moff[(*nmesh)++] = blk->len;
            if (!tg_emit_billboard_mesh(blk, hx - l[0] * off, ly - MA_LAMP_R,
                                        hz - l[1] * off, MA_LAMP_R,
                                        MA_LAMP_R * 2.0, tg_prop_slot(PP_LAMP),
                                        TD5_MESH_TAG_SIGNAL_LAMP + lamp))
                return 0;
        }
    }

    /* COUNTDOWN DISPLAY on the arm just past the last facing head: a black box
     * with a green dot-matrix face. The face is the additive lamp billboard
     * tagged GREEN, so it is lit while the signal is green and dim otherwise;
     * the digits themselves are static (see pl_build_display). */
    if (s_have_dsp) {
        const double u  = gap + hw * ((double)(nh - 1) + 0.5) / (double)nh + M_(0.62);
        const double dx = pole[0] - l[0] * u;
        const double dz = pole[1] - l[1] * u;
        const double dh = M_(0.26);
        const double dy = arm_y - MA_ARM_HT + M_(0.02) - dh * 0.5 - M_(0.06);
        moff[(*nmesh)++] = blk->len;
        if (!tg_emit_box_mesh(blk, dx, dy, dz, M_(0.24), dh * 0.5 + M_(0.03), M_(0.08),
                              t[0], t[1], TD5_TG_PAGE_R11_SIGN_POST, MA_HEAD_W,
                              0xFF161618u))
            return 0;
        moff[(*nmesh)++] = blk->len;
        if (!tg_emit_billboard_mesh(blk, dx - t[0] * M_(0.10), dy - dh * 0.5,
                                    dz - t[1] * M_(0.10), M_(0.20), dh,
                                    TD5_TG_PAGE_COUNT + s_npg,
                                    TD5_MESH_TAG_SIGNAL_LAMP + 2))
            return 0;
    }

    /* PLATE, hung BELOW the arm from two short brackets (as in the photo),
     * both faces named. Its bottom stays above the 4.4 m overhead line. */
    {
        const double drop  = M_(0.24);
        const double ytop  = arm_y - MA_ARM_HT - drop;
        const double bu[2] = { plate_u0 + M_(0.18), plate_u0 + MA_PLATE_W - M_(0.18) };
        for (k = 0; k < 2; k++) {
            moff[(*nmesh)++] = blk->len;
            if (!tg_emit_box_mesh(blk, pole[0] - l[0] * bu[k], ytop + drop * 0.5 + M_(0.02),
                                  pole[1] - l[1] * bu[k], M_(0.012), drop * 0.5 + M_(0.02),
                                  M_(0.012), t[0], t[1], TD5_TG_PAGE_R11_SIGN_POST, 1.0e7,
                                  0xFF606064u))
                return 0;
        }
        moff[(*nmesh)++] = blk->len;
        if (!ma_emit_plate(blk, pole, l, t, side, plate_u0, ytop, cf, cb))
            return 0;
    }
    return 1;
}

void tg_mastarm_report(void)
{
    if (!td5_geo_loaded()) return;
    if (!td5_env_flag_on("TD5RE_GEO_MASTARM")) {
        TD5_LOG_I(LOG_TAG, "[GEO MASTARM] DISABLED (TD5RE_GEO_MASTARM=0)");
        return;
    }
    TD5_LOG_I(LOG_TAG, "[GEO MASTARM] signals=%ld named=%ld (with a cross street "
              "%ld) | plates: %ld cell(s) on %d extra page(s), %ld two-row, %ld "
              "abbreviated | planned=%ld | post light kept: no-name=%ld "
              "narrow=%ld gantry=%ld | dropped: no-page=%ld no-font=%ld",
              s_ma_sigs, s_ma_named, s_ma_cross, s_ma_cells, s_npg,
              s_ma_two_line, s_ma_abbrev, s_ma_planned, s_ma_old_noname,
              s_ma_old_narrow, s_ma_old_gantry, s_ma_nopage, s_ma_noface);
}
