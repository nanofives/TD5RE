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

/* [ROUND 1022 S] Live signals: a counting-down display (digit atlas page, the
 * renderer picks the cell per frame) and a cross-street head that runs the
 * opposite phase. TD5RE_GEO_SIGNAL_LIVE=0 restores the round 1021 geometry and
 * page byte for byte: static "25", cross head tagged like the main heads. */
static int ma_live(void)
{
    return td5_env_flag_on("TD5RE_GEO_SIGNAL_LIVE");
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
    /* [roam SA] The plates were planned along the strip route (each signal's nearest route node
     * named the race street); the strip geo BUILD is deleted, so no plate is planned. */
    pl_reset(verbose);
}

/* 5x7 dot-matrix digits, one row per byte, bit 4 = leftmost dot. */
static const unsigned char k_dot_digit[10][7] = {
    { 0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E },   /* 0 */
    { 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E },   /* 1 */
    { 0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F },   /* 2 */
    { 0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E },   /* 3 */
    { 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 },   /* 4 */
    { 0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E },   /* 5 */
    { 0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E },   /* 6 */
    { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 },   /* 7 */
    { 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E },   /* 8 */
    { 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C }    /* 9 */
};

/* The countdown display page.
 *
 * LIVE (default): a digit ATLAS -- cell k (TD5_GEO_DIGIT_* in td5_geo_signals.h)
 * holds digit k as a 5x7 dot matrix on a 3-texel pitch with a 2x2 lit core, grey
 * ramp. The two display quads carry TD5_MESH_TAG_SIGNAL_DIGIT + 0/1 and the
 * renderer rewrites their UVs and diffuse every frame from the lamp cycle's
 * remaining time (td5_geo_signal_countdown), which is how a page that cannot be
 * swapped still shows a changing number. The page is opaque, so the diffuse
 * (green / red) tints the grey dots and black is the box.
 *
 * LIVE=0: the round 1021 static dot-matrix "25" on a 64x64 canvas (the quad was
 * 0.40 x 0.26 m, so the glyphs are rasterised into a 64x42 box). */
static void pl_build_display(void)
{
    memset(s_dsp, 0, sizeof s_dsp);
    s_have_dsp = 0;
    if (ma_live()) {
        int d, r, c;
        for (d = 0; d < 10; d++) {
            const int ox = (d % TD5_GEO_DIGIT_COLS) * TD5_GEO_DIGIT_CELL_W;
            const int oy = (d / TD5_GEO_DIGIT_COLS) * TD5_GEO_DIGIT_CELL_H;
            for (r = 0; r < 7; r++) {
                for (c = 0; c < 5; c++) {
                    int dx, dy;
                    if (!((k_dot_digit[d][r] >> (4 - c)) & 1)) continue;
                    for (dy = 0; dy < 2; dy++)
                        for (dx = 0; dx < 2; dx++)
                            s_dsp[(oy + r * 3 + dy) * TD5_TG_TEX_DIM +
                                  (ox + 1 + c * 3 + dx)] = 15;
                }
            }
        }
        s_have_dsp = 1;
        return;
    }
    {
        unsigned char cov[64 * 42];
        int x, y;
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

/* [ROUND 1030 D] Does signal node `sig` carry a street name for a plate? Lets the
 * junction layout pick a named node out of a cluster without touching the census. */
int tg_mastarm_has_plate(int sig)
{
    if (!tg_mastarm_enabled() || sig < 0 || sig >= s_nsig || !s_sp) return 0;
    return s_sp[sig].front >= 0 || s_sp[sig].back >= 0;
}

int tg_mastarm_front_cell(int sig)
{
    if (!tg_mastarm_enabled() || sig < 0 || sig >= s_nsig || !s_sp) return -1;
    return s_sp[sig].front;
}

/* [ROUND 1030 D item 12] "the street indicator should be in between the two traffic
 * lights in the middle of the crossing stick" (Mariano, level091 e57 s47 plate, span
 * 229). The plate hung next to the pole with both heads further out on the arm. With
 * the knob on the plate sits at the middle of the pole-side carriageway and the two
 * heads hang one either side of it, so an arm always has two. TD5RE_GEO_MASTARM_MID=0
 * restores the round 1021 layout (plate by the pole, heads over the lane centres). */
static int ma_mid(void)
{
    return td5_env_flag_on("TD5RE_GEO_MASTARM_MID");
}

static int ma_nheads(double hw)
{
    if (ma_mid()) return 2;
    return (hw >= 3.8 * PL_UPM) ? 2 : 1;
}

/* Meshes one arm needs, so the caller can fall back to the post light when the
 * entry's budget cannot hold it. */
int tg_mastarm_mesh_count(double carriageway_width)
{
    const int nh = ma_nheads(carriageway_width * 0.5);
    /* pole + arm, heads (housing, backplate, lamps), the cross-direction head
     * (housing + lamps), the countdown (box + lit face), two plate brackets,
     * the plate */
    return 2 + nh * (2 + TD5_GEO_SIGNAL_PHASES) + (1 + TD5_GEO_SIGNAL_PHASES)
             + (ma_live() ? 3 : 2) + 2 + 1;
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
    const int nh = ma_nheads(hw);
    const double road_y = n->y;
    const double arm_y  = road_y + MA_ARM_Y;
    const double top_y  = road_y + MA_POLE_TOP;
    double reach  = gap + hw + 0.4 * PL_UPM;         /* a little past the centreline */
    double armlen;
    double plate_u0 = MA_POLE_HW + M_(0.15);
    double head_u[2], disp_u;
    int k, lamp;

    if (ma_mid()) {
        /* [1030 D item 12] Plate centred over the middle of the pole-side half, one
         * head either side of it, clear of the plate by a backplate and a hand. The
         * countdown and the cross head stay outboard of the far head; the arm grows
         * when a narrow road would otherwise fold them onto it. */
        const double dsep_min = MA_PLATE_W * 0.5 + MA_BP_HW + M_(0.12);
        const double u_min    = MA_POLE_HW + MA_BP_HW + M_(0.10);
        /* The RACE carriageway, which inside a fork window is not the node's full
         * width about the node: the main half-carriageway is laid shifted and
         * narrowed (tg_fork_main_shift / _wscale_w, the strip's own rows). Measured
         * at level091 span 223 before this: heads 5.3 and 8.75 m right of the race
         * road's centre, i.e. over the median and Diagonal 73's second carriageway. */
        const int    fi    = tg_fork_of_main(si);
        const double c_off = (fi >= 0) ? tg_fork_main_shift(fi, n->width) : 0.0;
        const double mh    = (fi >= 0) ? n->width * tg_fork_main_wscale_w(fi, n->width) * 0.5
                                       : hw;
        /* u of the race road's pole-side half: edge at hw + gap - side*c_off - mh */
        double uc   = hw + gap - side * c_off - mh * 0.5;
        double dsep = mh * 0.25;
        double tip;
        if (dsep < dsep_min) dsep = dsep_min;
        if (uc - dsep < u_min) uc = u_min + dsep;
        head_u[0] = uc - dsep;
        head_u[1] = uc + dsep;
        plate_u0  = uc - MA_PLATE_W * 0.5;
        disp_u    = head_u[1] + M_(0.62);
        tip       = disp_u + M_(0.24) + M_(0.25) + MA_HEAD_W * 0.5 + M_(0.22);
        if (reach < tip) reach = tip;
    } else {
        for (k = 0; k < nh; k++)
            head_u[k] = gap + hw * ((double)k + 0.5) / (double)nh;
        disp_u = head_u[nh - 1] + M_(0.62);
    }
    armlen = reach;
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
        const double u  = head_u[k];
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
     * traffic on the cross street sees it. LIVE: its lamps carry the CROSS tag
     * range, lit from the cycle half a period behind (red while the main heads
     * are green and vice versa). LIVE=0: the round 1021 tags, the SAME state as
     * the main heads. Cosmetic either way. */
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
                                        (ma_live() ? TD5_MESH_TAG_SIGNAL_CROSS
                                                   : TD5_MESH_TAG_SIGNAL_LAMP) + lamp))
                return 0;
        }
    }

    /* COUNTDOWN DISPLAY on the arm just past the last facing head: a black box
     * with a dot-matrix face.
     * LIVE: two digit billboards (tens / ones, TD5_MESH_TAG_SIGNAL_DIGIT + 0/1)
     * whose UVs and colour the renderer sets every frame from the lamp cycle
     * (green digits counting down the green, red digits counting down the red).
     * LIVE=0: the round 1021 single additive face, static "25". */
    if (s_have_dsp) {
        const double u  = disp_u;
        const double dx = pole[0] - l[0] * u;
        const double dz = pole[1] - l[1] * u;
        const double dh = M_(0.26);
        const double dy = arm_y - MA_ARM_HT + M_(0.02) - dh * 0.5 - M_(0.06);
        moff[(*nmesh)++] = blk->len;
        if (!tg_emit_box_mesh(blk, dx, dy, dz, M_(0.24), dh * 0.5 + M_(0.03), M_(0.08),
                              t[0], t[1], TD5_TG_PAGE_R11_SIGN_POST, MA_HEAD_W,
                              0xFF161618u))
            return 0;
        if (ma_live()) {
            /* Tens is on the viewer's LEFT. Which end of the arm that is was
             * MEASURED for the plate (ma_emit_plate): for a driver approaching
             * along the tangent and side < 0 the far (larger u) end is on the
             * left; side > 0 swaps it. */
            const double dig_hw = M_(0.085), dig_h = M_(0.22), dig_dx = M_(0.105);
            const double far_sign = (side < 0.0) ? 1.0 : -1.0;
            int dgt;
            for (dgt = 0; dgt < TD5_GEO_SIGNAL_DIGITS; dgt++) {
                const double du = (dgt == 0 ? far_sign : -far_sign) * dig_dx;
                const double ux = pole[0] - l[0] * (u + du);
                const double uz = pole[1] - l[1] * (u + du);
                moff[(*nmesh)++] = blk->len;
                if (!tg_emit_billboard_mesh(blk, ux - t[0] * M_(0.10), dy - dig_h * 0.5,
                                            uz - t[1] * M_(0.10), dig_hw, dig_h,
                                            TD5_TG_PAGE_COUNT + s_npg,
                                            TD5_MESH_TAG_SIGNAL_DIGIT + dgt))
                    return 0;
            }
        } else {
            moff[(*nmesh)++] = blk->len;
            if (!tg_emit_billboard_mesh(blk, dx - t[0] * M_(0.10), dy - dh * 0.5,
                                        dz - t[1] * M_(0.10), M_(0.20), dh,
                                        TD5_TG_PAGE_COUNT + s_npg,
                                        TD5_MESH_TAG_SIGNAL_LAMP + 2))
                return 0;
        }
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

/* [ROAM F4c] pure wrappers over the file's text planner, for td5_roam_furniture.c's own
 * atlas (no strip behaviour changes: these only expose pl_upper / pl_plan_text /
 * pl_split2 to another module). */
void tg_plate_upper(const char *in, char *out, size_t cap) { pl_upper(in, out, cap); }
int  tg_plate_plan(const char *upper_name, char *l1, char *l2, size_t cap, int *abbreviated)
{
    int abbr = 0;
    const int rows = pl_plan_text(upper_name, l1, l2, cap, &abbr);
    if (abbreviated) *abbreviated = abbr;
    return rows;
}
int  tg_plate_split2(const char *upper_one_line, char *l1, char *l2, size_t cap)
{
    return pl_split2(upper_one_line, l1, l2, cap);
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
