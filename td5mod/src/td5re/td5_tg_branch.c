/**
 * td5_tg_branch.c -- auto-track BRANCHES: fork corridors, long diverging branch, carriageway query, branch pavement + avenue divider
 *
 * Split from the td5_trackgen.c monolith (2026-09-06); shared declarations
 * live in td5_trackgen_internal.h. Element map: docs/plans/AUTOTRACK_ELEMENT_CATALOG.md.
 */
#include "td5_trackgen_internal.h"

int tg_branches_enabled(void)
{
    /* Default ON (2026-08-26); set TD5RE_AUTOTRACK_BRANCHES=0 to disable. */
    return td5_env_flag_on("TD5RE_AUTOTRACK_BRANCHES");
}

/* True if span si lies in ANY fork's cleared region (approach through rejoin):
 * the corridor bows into the side<0 lateral, so verge scenery there would sit in
 * the branch. Used by the facade/prop/water suppression. */
int tg_span_in_fork_clear(int si)
{
    int i;
    for (i = 0; i < s_fork_count; i++)
        if (si >= s_forks[i].F - TD5_TG_BRANCH_WIDEN - 2 &&
            si <= s_forks[i].R + 2)
            return 1;
    return 0;
}

/* [R6 item 10] Largest per-span heading change (radians) over a fork's span
 * range, read from the already-built centreline tangents. A fork narrows the
 * main road to its LEFT half and shifts it a quarter-road toward the inside of
 * the bend (TD5_TG_MAIN_SHIFT) while the branch bows a full road-width the
 * other way; on a tight/acute curve the shifted carriageway folds over its own
 * inner edge, which lifts a car driving it -- the user's "span 570 collision is
 * LIFTING the car" (fork 2 on seed 99991 lands on an acute S-bend) and the
 * paired request to "avoid this kind of curve". The placement loop slides a
 * fork forward until this stays under TD5_TG_FORK_MAX_TURN, i.e. onto a
 * sweeping stretch where the shift cannot fold. Covers the widened approach
 * (F-WIDEN) through the rejoin span (R). */
double tg_fork_region_max_curve(const TG_NodeList *nl, int F, int L, int ring)
{
    /* Scan exactly the span range the walk-time clamp (tg_span_in_fork_run)
     * gentles, so the logged number reflects what was actually straightened --
     * a span one past either end is ordinary single road (no fork geometry) and
     * its curvature is not a fold risk. */
    int lo = F - TD5_TG_BRANCH_WIDEN;
    int hi = F + 1 + L;
    double worst = 0.0;
    int i;
    if (lo < 1) lo = 1;
    if (hi > ring - 1) hi = ring - 1;
    for (i = lo; i <= hi && i + 1 < nl->count; i++) {
        double d = nl->v[i].tx * nl->v[i + 1].tx + nl->v[i].tz * nl->v[i + 1].tz;
        double a;
        if (d > 1.0) d = 1.0; else if (d < -1.0) d = -1.0;
        a = acos(d);
        if (a > worst) worst = a;
    }
    return worst;
}

/* Append one vertex row of (lanes+1) points for node n, relative to (ox,oy,oz),
 * laterally shifted by `shift` (world units, +ve = left of travel) and using
 * `width`. Returns the row's first vertex index. */
int tg_append_row(TG_Buf *verts, int *vtx_count, const TG_Node *n,
                         int lanes, double width, double shift,
                         int ox, int oy, int oz)
{
    const double lx = n->tz, lz = -n->tx;
    const int first = *vtx_count;
    int j;
    for (j = 0; j <= lanes; j++) {
        double t = shift + (width * 0.5) - (width * (double)j / (double)lanes);
        int dx = tg_round(n->x + lx * t) - ox;
        int dy = tg_round(n->y) - oy;
        int dz = tg_round(n->z + lz * t) - oz;
        tg_put_u16(verts, (unsigned)(dx & 0xFFFF));
        tg_put_u16(verts, (unsigned)(dy & 0xFFFF));
        tg_put_u16(verts, (unsigned)(dz & 0xFFFF));
    }
    *vtx_count += lanes + 1;
    return first;
}

/* Overwrite fields of an already-emitted span record in place. */
void tg_patch_span(TG_Buf *spans, int si, int type, int lanes,
                          int lvi, int rvi, int link_next, int link_prev,
                          int ox, int oy, int oz)
{
    unsigned char *r = spans->b + (size_t)si * 24;
    r[0] = (unsigned char)type;
    r[3] = (unsigned char)((TD5_TG_HEIGHT_NIBBLE << 4) | (lanes & 0x0F));
    r[4] = (unsigned char)(lvi & 0xFF);       r[5] = (unsigned char)(lvi >> 8);
    r[6] = (unsigned char)(rvi & 0xFF);       r[7] = (unsigned char)(rvi >> 8);
    r[8] = (unsigned char)(link_next & 0xFF); r[9] = (unsigned char)((link_next >> 8) & 0xFF);
    r[10]= (unsigned char)(link_prev & 0xFF); r[11]= (unsigned char)((link_prev >> 8) & 0xFF);
    {   /* origin: these spans own their rows, so they own their origin too */
        int i, v[3];
        v[0] = ox; v[1] = oy; v[2] = oz;
        for (i = 0; i < 3; i++) {
            unsigned int u = (unsigned int)v[i];
            r[12 + i*4 + 0] = (unsigned char)(u & 0xFF);
            r[12 + i*4 + 1] = (unsigned char)((u >> 8) & 0xFF);
            r[12 + i*4 + 2] = (unsigned char)((u >> 16) & 0xFF);
            r[12 + i*4 + 3] = (unsigned char)((u >> 24) & 0xFF);
        }
    }
}

/* Append a whole span record (used for the pad span and the corridor).
 *
 * `attr` is the surface byte, passed in rather than hardcoded so an appended
 * corridor span carries the SAME surface class as the main-ring span it runs
 * beside (biome grip included) -- the branch is the same road, not a different
 * one.
 *
 * MASK 0, deliberately. The old body wrote `1 | (1 << (lanes - 1))`, i.e. "the
 * two outer lanes use the ALTERNATE surface", copied from shipped level001. On
 * a HALF carriageway br_lanes is 2, so bit0 | bit1 = 0x03 marks BOTH lanes --
 * the whole branch became the 0x10 alternate class, and
 * td5_track_surface_is_slow returns 1 for anything with 0x10 set. That is the
 * reported "some branches spawn with slow lane attribute, like driving on a
 * sidewalk": every lane of the corridor was the slow class while textured like
 * tarmac. tg_emit_span_range was already fixed to write 0 for the main ring
 * (see the note there); this is the same fix for the appended spans, which were
 * missed because they go through a different writer. */
void tg_append_span(TG_Buf *spans, int type, int attr, int lanes,
                           int lvi, int rvi, int link_next, int link_prev,
                           int ox, int oy, int oz)
{
    tg_put_u8 (spans, (unsigned)type);
    tg_put_u8 (spans, (unsigned)attr);
    tg_put_u8 (spans, 0);
    tg_put_u8 (spans, (unsigned)((TD5_TG_HEIGHT_NIBBLE << 4) | (lanes & 0x0F)));
    tg_put_u16(spans, (unsigned)lvi);
    tg_put_u16(spans, (unsigned)rvi);
    tg_put_u16(spans, (unsigned)(link_next & 0xFFFF));
    tg_put_u16(spans, (unsigned)(link_prev & 0xFFFF));
    tg_put_i32(spans, ox);
    tg_put_i32(spans, oy);
    tg_put_i32(spans, oz);
}

int tg_branch_min_len(void)
{
    return td5_env_int("TD5RE_AUTOTRACK_BRANCH_MINLEN",
                       TD5_TG_BRANCH_MIN_LEN, 8, 400);
}

int tg_r8_longbranch_enabled(void)
{
    /* DEFAULT OFF -- td5_env_flag_OFF, not flag_on. This is deliberate and it is
     * not a confidence problem: the feature works, races finish on both seeds,
     * and the geometry checks are clean (see the R8 SHAPE notes in
     * td5_changelog.h). It is parked because it REPARTITIONS THE MAIN RING.
     *
     * MEASURED on seed 99991, this knob alone against a purged environment:
     * fork 2's rejoin moves from ring span 631 to 771, so ring spans 631-770
     * stop being full road and become the fork's main half; the fork-back
     * backdrop grows from 122 to 262 spans (last span 633 -> 773); and the
     * main-ring scenery counts move with it -- buildings 2245->2228, fences
     * 1829->1698, crossings 377->341, props 669->622, cross-furn 505->452,
     * step-walls 75->69. Round 8's user-reported items are span-referenced
     * ("tiles spilling over the road around 627"), and item 7's span sits
     * directly inside the range this moves. Shipping it default ON would mean
     * the user re-drives a different track from the one they reported, and
     * could not verify the other seven areas' fixes where they reported them.
     *
     * TD5RE_R8_SHAPE_LONGBRANCH=1 turns it on. Flip the default to
     * td5_env_flag_on once the round's span-referenced items are closed. */
    return td5_env_flag_off("TD5RE_R8_SHAPE_LONGBRANCH");
}

/* THE fork length table. Both the stateless run gate (tg_span_in_fork_run,
 * consulted by the centreline walk BEFORE s_forks exists) and the placement
 * loop in tg_emit_strip read it, and they MUST agree -- a mismatch straightens
 * the wrong span range. One function so they cannot drift. */
/* [FORK KINDS] The plan ladder. Six shapes in census proportions (the
 * symmetric ones are the majority), rotated by the seed so two tracks do not
 * open with the same fork. The seed is captured by tg_srand, which every build
 * path calls before the centreline walk, so the rotation is known before
 * s_forks exists. TD5RE_AUTOTRACK_BRANCH_COUNT (default 6, 0..8) sets how
 * many of the ladder are placed; TD5RE_AUTOTRACK_BRANCH_KINDS=0 pins the
 * pre-kinds ladder (3 symmetric forks of 24/40/120 spans) for an A/B. */
unsigned int s_fork_plan_seed;

static const struct { int kind; int len; double sep; } k_fork_plan[6] = {
    { TG_FORK_AVENUE, 40,  0.20 },
    { TG_FORK_ISLAND,  6,  0.16 },
    { TG_FORK_WIDE,  120,  1.00 },
    { TG_FORK_SLIP,   32,  0.55 },
    { TG_FORK_MAJOR,  60,  0.45 },
    { TG_FORK_ISLAND,  5,  0.16 },
};

const char *tg_fork_kind_name(int kind)
{
    static const char *const k_names[TG_FORK_KIND_COUNT] =
        { "AVENUE", "ISLAND", "WIDE", "SLIP", "MAJOR" };
    return (kind >= 0 && kind < TG_FORK_KIND_COUNT) ? k_names[kind] : "?";
}

static int tg_fork_kinds_enabled(void)
{
    return td5_env_flag_on("TD5RE_AUTOTRACK_BRANCH_KINDS");
}

int tg_fork_count_planned(void)
{
    if (!tg_fork_kinds_enabled()) return 3;
    return td5_env_int("TD5RE_AUTOTRACK_BRANCH_COUNT", 6, 0, TD5_TG_BRANCH_MAX);
}

void tg_fork_plan(int index, int *kind, int *len, double *sep)
{
    if (!tg_fork_kinds_enabled()) {
        /* pre-kinds ladder: three symmetric forks, the old sep ladder */
        static const int k_lens[3] = { 8, 40, 120 };
        int L = (index >= 0 && index < 3) ? k_lens[index] : 0;
        if (index == 2 && tg_r8_longbranch_enabled())
            L = td5_env_int("TD5RE_R8_SHAPE_LONGBRANCH_LEN",
                            TD5_TG_R8_LONG_LEN, 24, 400);
        if (kind) *kind = TG_FORK_WIDE;
        if (len)  *len  = L;
        if (sep)  *sep  = tg_fork_sep_for(index);
        return;
    }
    {
        const int n = (int)(sizeof(k_fork_plan) / sizeof(k_fork_plan[0]));
        const unsigned int rng = s_fork_plan_seed * 2654435761u;
        /* [R20 FORK VARIETY] The old rotation `rng >> 29` yields 0..7 but the
         * ladder has only n==6 entries, so rot 6 folds onto 0 and rot 7 onto 1
         * -- rotations 0 and 1 land twice as often, and most seeds picked the
         * same shape (measured: 4 of 5 test seeds -> effective rotation 1).
         * Take high bits and take them modulo n (derived from the table) so all
         * n rotations are equally likely. Knob OFF restores the skewed shift for
         * A/B; ON changes the chosen shape on most seeds. Deterministic in the
         * plan seed. */
        const int rot = td5_env_flag_on("TD5RE_R20_FORK_ROT")
                            ? (int)((rng >> 16) % (unsigned int)n)   /* 0..n-1 */
                            : (int)(rng >> 29);                      /* old 0..7 */
        const int e = ((index < 0 ? 0 : index) + rot) % n;
        int L = k_fork_plan[e].len;
        if (k_fork_plan[e].kind == TG_FORK_WIDE && tg_r8_longbranch_enabled())
            L = td5_env_int("TD5RE_R8_SHAPE_LONGBRANCH_LEN",
                            TD5_TG_R8_LONG_LEN, 24, 400);
        if (kind) *kind = k_fork_plan[e].kind;
        if (len)  *len  = L;
        if (sep)  *sep  = k_fork_plan[e].sep;
    }
}

/* Corridor length after the taper floor. Only the shapes that BOW need the
 * TD5_TG_BRANCH_MIN_LEN floor (it is what lets the bow taper within the
 * lateral-rate limit); an island barely bows (sep 0.16 over a handful of
 * spans keeps the derived bow to a few hundred units), so it keeps its length. */
static int tg_fork_len_floored(int kind, int L)
{
    const int min_len = tg_branch_min_len();
    if (kind == TG_FORK_ISLAND) return (L < 3) ? 3 : L;
    return L < min_len ? min_len : L;
}

/* How the lanes split at a fork of `kind` on a road of `lanes` lanes. Every
 * shipped fork obeys lanes(F) = lanes(F+1) + lanes(B0); these do too. */
void tg_fork_split_lanes(int kind, int lanes, int *main_lanes, int *br_lanes)
{
    int m, bl;
    switch (kind) {
    case TG_FORK_SLIP:   bl = (lanes >= 5) ? 2 : 1; m = lanes - bl; break;
    case TG_FORK_MAJOR:  m  = (lanes >= 4) ? 2 : 1; bl = lanes - m; break;
    default:             m  = lanes / 2;            bl = lanes - m; break;
    }
    if (m < 1)  { m = 1;  bl = lanes - 1; }
    if (bl < 1) { bl = 1; m  = lanes - 1; }
    if (main_lanes) *main_lanes = m;
    if (br_lanes)   *br_lanes   = bl;
}

int tg_branch_len_for(int index)
{
    int kind, L;
    tg_fork_plan(index, &kind, &L, NULL);
    return L;
}
int tg_branch_count_max(void) { return tg_fork_count_planned(); }

/* Is `len` a LONG corridor (i.e. one entitled to the raised bow ceiling)? */
int tg_branch_is_long(int len)
{
    return tg_r8_longbranch_enabled() && len >= TD5_TG_R8_LONG_MIN;
}

/* [R20 FORK VARIETY] Fork PLACEMENT positions -- the SINGLE SOURCE OF TRUTH.
 * Three call sites decide where forks sit and they MUST agree, or the walk
 * protects one set of spans while the placement loop commits to another. That
 * disagreement was the measured regression: seed-derived positions in the loop
 * only, while the walk's stateless gates (tg_span_in_fork_run, keeping lane
 * changes out of fork windows; tg_fork_window_ahead, widening the road before a
 * fork) still predicted the OLD constants -- so every moved fork landed exactly
 * where the walk had left a lane change and the uniformity guard rightly
 * rejected it. Deriving all three from these two helpers makes the walk protect
 * and widen the SAME spans the loop uses, exactly as the old hardcoded 120/150
 * did by construction. Knob OFF pins the old constants (byte-identical to
 * today). Deterministic in the plan seed (set by tg_srand before the walk); the
 * bounds keep the first fork past the grid + the F-WIDEN-2 approach window and
 * the gap above that window, so no guard is weakened. */
/* [R20 PLACE] DEFAULT ON as of the A/B below. Fork positions are consumed in
 * THREE places that must agree -- the placement loop in td5_trackgen.c,
 * tg_span_in_fork_run (the walk keeps lane changes OUT of fork windows) and
 * tg_fork_window_ahead (the walk widens the road ahead of a fork). All three
 * used to hardcode grid+120 / R+150, so they agreed by construction. A first
 * attempt moved ONLY the placement loop and measured a clear regression --
 * 4 forks / 3 skipped / 1907 spans versus 6 / 1 / 2209 -- with every reject
 * logging "lane count changes inside its window", because the walk was still
 * protecting and widening the OLD spans. No guard was weakened to fix it: the
 * guard was right, the inputs disagreed. All three now call these helpers.
 * MEASURED with real assets, all else equal:
 *   seed 771144 OFF: 6 forks, 1 skipped, 2209 spans, first F=144
 *   seed 771144 ON : 6 forks, 1 skipped, 2209 spans, first F=149
 *   seed 5150   ON : 6 forks, 1 skipped, 2209 spans, first F=224
 * Count, skips and span total match the OFF path; positions now vary per seed
 * INCLUDING the first fork, which was pinned at 144 on every track before.
 * TD5RE_R20_FORK_PLACE=0 restores the old constants. */
int tg_fork_first_off(void)
{
    if (!td5_env_flag_on("TD5RE_R20_FORK_PLACE")) return 120;   /* =0 -> old */
    return 120 + (int)(((s_fork_plan_seed * 2654435761u) >> 13) % 96u);   /* 120..215 */
}
int tg_fork_gap(void)
{
    if (!td5_env_flag_on("TD5RE_R20_FORK_PLACE")) return 150;   /* =0 -> old */
    return 130 + (int)(((s_fork_plan_seed * 2246822519u + 3266489917u) >> 13) % 61u); /* 130..190 */
}

/* [R6 item 10] Is span si inside a fork's span range (widened approach through
 * rejoin)? Stateless, derived only from si and the deterministic fork placement
 * constants -- the SAME positions the placement loop in tg_emit_strip commits to
 * (pos = grid+120, then R+150; L = max(k_len, min_len)). Used by the centreline
 * walk (via the forward declaration up top) to gentle the curvature there,
 * BEFORE s_forks exists. If the placement loop later drops the last fork (ring-
 * fit / lane check), the extra straightened span range simply carries no fork --
 * gentle road, no harm. Mirrors the bridge-run gate exactly. */
int tg_span_in_fork_run(int si)
{
    int pos, gap, i;
    if (!tg_branches_enabled()) return 0;
    pos = TD5_TG_GRID_SPAN + tg_fork_first_off();   /* [R20] shared with the loop */
    gap = tg_fork_gap();
    for (i = 0; i < tg_branch_count_max(); i++) {
        int kind, kl;
        tg_fork_plan(i, &kind, &kl, NULL);
        {
        int L = tg_fork_len_floored(kind, kl);
        int F = pos;
        int R = F + 1 + L;
        /* +/-2 spans of margin past the widened approach and the rejoin so the
         * road eases INTO and OUT of the fork gently rather than meeting a sharp
         * bend right where the carriageways start to split / merge. */
        if (si >= F - TD5_TG_BRANCH_WIDEN - 2 && si <= R + 2) return 1;
        pos = R + gap;
        }
    }
    return 0;
}

static double tg_branch_gain_f(int k, int len, int base_lanes);
static int tg_branch_lane_gain(int k, int len, int base_lanes);

/* [FORK KINDS] Lanes a fork of `kind` needs on the road at F to read as its
 * kind: a slip road or a major branch needs 5 (so the narrow side is 2 and the
 * wide side 3+), everything else 4 (2+2). */
int tg_fork_kind_min_lanes(int kind)
{
    return (kind == TG_FORK_SLIP || kind == TG_FORK_MAJOR) ? 5 : 4;
}

/* Does a fork window (widened approach through rejoin) START within `within`
 * spans after span si? Returns the planned kind, or -1. Stateless like
 * tg_span_in_fork_run, so the centreline walk can widen the road ahead of a
 * fork instead of arriving at it with too few lanes to split. */
int tg_fork_window_ahead(int si, int within)
{
    int pos, gap, i;
    if (!tg_branches_enabled()) return -1;
    pos = TD5_TG_GRID_SPAN + tg_fork_first_off();   /* [R20] shared with the loop */
    gap = tg_fork_gap();
    for (i = 0; i < tg_branch_count_max(); i++) {
        int kind, kl;
        tg_fork_plan(i, &kind, &kl, NULL);
        {
        const int L = tg_fork_len_floored(kind, kl);
        const int F = pos, R = F + 1 + L;
        const int w0 = F - TD5_TG_BRANCH_WIDEN - 2;
        if (si <= w0 && w0 - si <= within) return kind;
        pos = R + gap;
        }
    }
    return -1;
}

/* [FORK KINDS] Fork-aware carriageway geometry. The symmetric case (fm = fb =
 * 0.5) reproduces the pre-kinds numbers exactly: main at +w/4 with half the
 * width, branch at -w/4 minus the bow. An asymmetric split keeps both
 * carriageways INSIDE the road's own footprint at the mouth (main centre at
 * +w(1-fm)/2, branch centre at -w(1-fb)/2, so main's left edge and branch's
 * right edge are the road's edges) and the bow carries the branch out from
 * there. Lane GAIN is a WIDE-only trait: an avenue or island must keep its
 * fixed half (the median is slim), and a slip road or a major branch is
 * defined by its lane split, so widening it would undo the shape. */
static double tg_fork_fm(int fi) { return (fi >= 0 && fi < s_fork_count) ? s_forks[fi].fm : 0.5; }
static double tg_fork_fb(int fi) { return (fi >= 0 && fi < s_fork_count) ? s_forks[fi].fb : 0.5; }
static int    tg_fork_gains(int fi)
{
    return fi >= 0 && fi < s_fork_count && s_forks[fi].kind == TG_FORK_WIDE
        && s_forks[fi].sep > TD5_TG_AVENUE_SEP_MAX;
}
double tg_fork_main_shift(int fi, double w)  { return w * (1.0 - tg_fork_fm(fi)) * 0.5; }
double tg_fork_main_wscale(int fi)           { return tg_fork_fm(fi); }
double tg_fork_br_shift(int fi, int k, double w)
{
    const int    len = (fi >= 0 && fi < s_fork_count) ? s_forks[fi].len : 1;
    const double sep = (fi >= 0 && fi < s_fork_count) ? s_forks[fi].sep : 1.0;
    const double f   = (len > 0) ? (double)k / (double)len : 0.0;
    const double bow = sin(f * TD5_TG_PI);
    return -w * (1.0 - tg_fork_fb(fi)) * 0.5 - w * tg_branch_bow(len, w) * sep * bow;
}
double tg_fork_br_wscale(int fi, int k)
{
    const double base = tg_fork_fb(fi);
    if (!tg_fork_gains(fi)) return base;
    return base + 0.25 * tg_branch_gain_f(k, s_forks[fi].len, s_forks[fi].br_lanes);
}
int tg_fork_br_lanes_at(int fi, int k)
{
    const int base = (fi >= 0 && fi < s_fork_count) ? s_forks[fi].br_lanes : 0;
    if (!tg_fork_gains(fi)) return base;
    return base + tg_branch_lane_gain(k, s_forks[fi].len, base);
}

/* Bow amplitude (x width) usable over a corridor of `len` spans without the
 * branch centre moving sideways faster than TD5_TG_BRANCH_RATE * span_length
 * per span. d/dk of the half-sine peaks at amp*width*PI/len, so
 * amp <= RATE*span_len*len / (width*PI). Never above TD5_TG_BRANCH_BOW: a long
 * fork should look like the authored one, not sail off across the map. */
double tg_branch_bow(int len, double width)
{
    /* [R8 SHAPE G5] A LONG corridor gets a raised ceiling so it can actually
     * go somewhere; everything else keeps the shipped 1.20. The RATE-derived
     * bound below is unchanged and still applies first, so a long fork that is
     * somehow not long enough to taper still gets only what it can taper to. */
    double cap = tg_branch_is_long(len) ? TD5_TG_R8_BOW_LONG
                                        : TD5_TG_BRANCH_BOW;
    double amp;
    if (len <= 0 || width < 1.0) return 0.0;
    /* [R8 SHAPE G5] HARD int16 clamp, and it is not theoretical. A corridor span
     * carries a PER-SPAN origin, and tg_append_row writes each point's offset
     * from it as an int16 with a silent `& 0xFFFF`. The outermost point of the
     * branch sits at width*(0.25 + bow) + half the branch's own (up to full)
     * width from the centreline. At the 6000-unit road of seed 99991 a bow of
     * 3.0 lands at 24000, well inside; at seed 777's 9000-unit road the same
     * 3.0 lands at 33750 -- PAST 32767, where the offset wraps sign and the
     * corridor's geometry inverts. So express the ceiling in WORLD UNITS and
     * derive the multiple from the actual road width, keeping headroom for the
     * far row's origin delta (one span step) and rounding. */
    {
        const double unit_cap = TD5_TG_R8_LAT_MAX / width - 0.75;
        if (unit_cap < cap) cap = unit_cap;
        if (cap < 0.0) cap = 0.0;
    }
    amp = TD5_TG_BRANCH_RATE * (double)TD5_TG_SPAN_LENGTH * (double)len
        / (width * TD5_TG_PI);
    return (amp > cap) ? cap : amp;
}

double tg_fork_sep_for(int fork_index)
{
    static const double k_sep[] = { 0.20, 0.60, 1.00, 0.30 };
    const int n = (int)(sizeof(k_sep) / sizeof(k_sep[0]));
    double s;
    /* Default ON: the user asked for various separation sizes. =0 pins the old
     * uniform widest (sep 1.0) for an A/B against the pre-item-10 behaviour. */
    if (!td5_env_flag_on("TD5RE_AUTOTRACK_BRANCH_VARY_SEP")) return 1.0;
    if (fork_index < 0) fork_index = 0;
    s = k_sep[(unsigned)fork_index % (unsigned)n];
    if (s < TD5_TG_BRANCH_SEP_MIN) s = TD5_TG_BRANCH_SEP_MIN;
    return s;
}

/* True where fork `fork_index` is tight enough to read as a divided avenue
 * (item 9: "a distinction of different dividers ... if it is a branch
 * representing an avenue"). Above the threshold the carriageways diverge into
 * two separate roads and a central island would just float in open ground. */
int tg_fork_is_avenue(int fork_index)
{
    if (fork_index >= 0 && fork_index < s_fork_count)
        return s_forks[fork_index].sep <= TD5_TG_AVENUE_SEP_MAX;
    return tg_fork_sep_for(fork_index) <= TD5_TG_AVENUE_SEP_MAX;
}

/* [R16 MEDIAN] "should be longer by default ... a handful of spans should
 * either extend or not exist." A raised median only reads as a divided avenue
 * over a decent length; on a short fork it is a stub -- exactly the "really
 * small median" the report picked (seed 20260907 fork 0 was ISLAND len 6). So
 * a fork shorter than TD5_TG_MEDIAN_MIN_FORK_LEN carries no median island (and
 * no end caps) at all. Length is a per-fork constant, so the placement mirror
 * (tg_median_at_raw) and the emitter get the same answer on every span of the
 * fork and cannot drift. TD5RE_MEDIAN_MIN_RUN=0 restores the old short-fork
 * medians for a single-variable A/B. */
int tg_median_fork_long_enough(int fork_index)
{
    if (!td5_env_flag_on("TD5RE_MEDIAN_MIN_RUN")) return 1;   /* =0 for A/B */
    if (fork_index >= 0 && fork_index < s_fork_count)
        return s_forks[fork_index].len >= TD5_TG_MEDIAN_MIN_FORK_LEN;
    return 1;
}

/* Lateral centre of the BRANCH (right) half carriageway at corridor step k with
 * separation scale `sep`: the right-half centre (-width/4) plus an outward bow
 * that is 0 at both ends (so it lines back up with the road halves at the fork
 * and the rejoin) and peaks in the middle, scaled by `sep`.
 *
 * SEMANTIC CHANGE 2026-08-26: the bow amplitude is length-derived
 * (tg_branch_bow) rather than the flat TD5_TG_BRANCH_BOW. 2026-08-28 (item 10):
 * split out the sep-scaled form; tg_branch_shift is now the sep=1.0 (widest)
 * case, which is what the tunnel-bore enclosure wants (it must cover the WIDEST
 * a corridor could be). Sign convention is unchanged (negative = right of
 * travel), so every reader keeps working. */
double tg_branch_shift_s(int k, int len, double width, double sep)
{
    double f   = (len > 0) ? (double)k / (double)len : 0.0;   /* 0 .. 1 */
    double bow = sin(f * TD5_TG_PI);                          /* 0 -> 1 -> 0 */
    return -width * 0.25 - width * tg_branch_bow(len, width) * sep * bow;
}

double tg_branch_shift(int k, int len, double width)
{
    return tg_branch_shift_s(k, len, width, 1.0);
}

/* ---- branch lane GAIN (feedback: "branches should be able to spawn and
 * widen their lanes") ----
 * The old corridor was a fixed half carriageway for its whole length: br_lanes
 * lanes at wscale 0.5, start to finish. A real alternate route opens out once
 * it is clear of the split.
 *
 * So the corridor now RAMPS: it must be exactly the half carriageway at k=0
 * and at k=len-1 (those two spans are where it lines up with the fork span and
 * the type-11 rejoin, both of which are full width and unchanged), and in
 * between it gains up to TD5RE_AUTOTRACK_BRANCH_GAIN extra lanes on a half sine
 * -- same shape as the bow, so the widening and the divergence peak together.
 *
 * Per-span lane counts are legal on these spans because appended corridor spans
 * own their vertex rows (see the BRANCHES design note); no shared row changes
 * point count. Default ON, TD5RE_AUTOTRACK_BRANCH_WIDEN=0 to pin the old fixed
 * half width. */
static int tg_branch_widen_enabled(void)
{
    return td5_env_flag_on("TD5RE_AUTOTRACK_BRANCH_WIDEN");
}

/* Extra lanes at corridor step k as a CONTINUOUS quantity, 0 at both ends.
 *
 * Split out from tg_branch_lane_gain (2026-08-27) because the two consumers
 * want different things and conflating them is what produced the reported
 * "sudden changes of lane widths on right track branches". The strip row needs
 * an INTEGER point count -- a row has a whole number of points -- but the road
 * WIDTH has no such constraint, and taking the width from the rounded lane
 * count quantised it to 0.5 / 0.75 / 1.0 of the road: three discrete widths
 * with a hard step at whichever span the rounding tipped over. */
static double tg_branch_gain_f(int k, int len, int base_lanes)
{
    int gain_max;
    double f;
    if (!tg_branch_widen_enabled() || len < 4) return 0.0;
    if (k <= 0 || k >= len - 1) return 0.0;
    gain_max = td5_env_int("TD5RE_AUTOTRACK_BRANCH_GAIN", 2, 0, 4);
    /* Keep the widened corridor inside the lane range the rail LUTs, edge masks
     * and suspension paths are exercised in (see td5_trackgen_apply_config's
     * 2..4 clamp): never take the branch past 4 lanes. */
    if (base_lanes + gain_max > 8) gain_max = 8 - base_lanes;   /* [LANES] was 4 */
    if (gain_max <= 0) return 0.0;
    /* Half sine over the INTERIOR of the corridor, so the widening grows in and
     * out over several spans instead of stepping at k=1. */
    f = (double)(k - 1) / (double)(len - 3 > 0 ? len - 3 : 1);
    return sin(f * TD5_TG_PI) * (double)gain_max;
}

/* Extra lanes at corridor step k (0 at both ends), rounded to a whole lane.
 * SUBDIVISION ONLY: this is the row's point count, not its width. */
static int tg_branch_lane_gain(int k, int len, int base_lanes)
{
    const double gf = tg_branch_gain_f(k, len, base_lanes);
    int g = (int)(gf + 0.5);
    if (g < 0) g = 0;
    return g;
}

/* Width scale of the branch carriageway at step k: 0.5 (the half road) plus a
 * quarter road per gained lane -- CONTINUOUS, so the widening is a taper the
 * eye reads as a smooth opening-out rather than a step at a span boundary.
 *
 * SEMANTIC CHANGE 2026-08-27: this used to quantise through tg_branch_lane_gain
 * and return one of {0.50, 0.75, 1.00}; it now returns any value in [0.5, 1.0].
 * Signature and meaning ("multiply the road's full width by this to get the
 * branch carriageway's width at step k") are unchanged, and the new value is
 * never larger than the old one by more than half a lane, so a reader using it
 * for CLEARANCE is still conservative. A reader that assumed the three discrete
 * values would now disagree. */
static double tg_branch_wscale(int k, int len, int base_lanes)
{
    return 0.5 + 0.25 * tg_branch_gain_f(k, len, base_lanes);
}

/* Separation-aware forms (item 10). The branch row is centred on tg_branch_shift
 * and extends +/- half its width, so WIDENING grows the carriageway INWARD as
 * well as outward. On a WIDE split there is room -- the bow has carried the
 * branch far enough right that its inner edge still clears the main road. On a
 * tight AVENUE there is not: gaining a lane would push the inner edge back
 * across the slim median into the oncoming main carriageway. So an avenue keeps
 * the FIXED half carriageway (a constant 2+2 divided road, which is what an
 * avenue actually is); only diverging forks open out. Both the width and the
 * point count are gated the SAME way, so the strip rows and the road mesh stay
 * point-for-point identical however the fork is classified. */
double tg_branch_wscale_s(int k, int len, int base_lanes, double sep)
{
    if (sep <= TD5_TG_AVENUE_SEP_MAX) return 0.5;
    return tg_branch_wscale(k, len, base_lanes);
}

int tg_branch_lane_gain_s(int k, int len, int base_lanes, double sep)
{
    if (sep <= TD5_TG_AVENUE_SEP_MAX) return 0;
    return tg_branch_lane_gain(k, len, base_lanes);
}

/* Half the MAIN road's width at span si, taking the wider of the span's two
 * ends so a width ramp never reports less road than the span actually has. */
double tg_road_half_width(const TG_NodeList *nl, int si)
{
    double w;
    if (!nl || si < 0 || si >= nl->count) return 0.0;
    w = nl->v[si].width;
    if (si + 1 < nl->count && nl->v[si + 1].width > w) w = nl->v[si + 1].width;
    return w * 0.5;
}

/* Outermost drivable lateral at span si on `side`, as a POSITIVE distance from
 * the main centerline. Never less than the main road's own half width. */
double tg_carriageway_reach(const TG_NodeList *nl, int si, double side)
{
    double reach = tg_road_half_width(nl, si);
    int i;

    if (side >= 0.0) return reach;            /* no corridor bows LEFT */
    if (!tg_branches_enabled()) return reach;
    if (!nl || si < 0 || si + 1 >= nl->count) return reach;

    for (i = 0; i < s_fork_count; i++) {
        const int F = s_forks[i].F, L = s_forks[i].len;
        int k, e;
        /* One span of slack past each mouth: at the fork and the rejoin the
         * corridor is still lined up with the road, but a caller asking about
         * the mouth span itself must not see a narrower answer than its
         * neighbour or scenery pops in for one span. */
        if (si < F - 1 || si > F + L + 1) continue;
        k = si - F - 1;
        if (k < 0) k = 0;
        if (k > L) k = L;
        /* BOTH ends of the span: the bow and the widening grow across it, so
         * the near end alone under-reports. */
        for (e = 0; e <= 1; e++) {
            const int    kk = (k + e > L) ? L : k + e;
            const int    ni = (si + e < nl->count) ? si + e : si;
            const double w  = nl->v[ni].width;
            /* Branch centre is NEGATIVE (right of travel); its outer edge is a
             * further half carriageway-width right of that. Uses the fork's OWN
             * separation (item 10) so a tight avenue reports a nearer reach than
             * a wide split and scenery clears the branch at its ACTUAL width. */
            const double out = -tg_fork_br_shift(i, kk, w)
                             + w * tg_fork_br_wscale(i, kk) * 0.5;
            if (out > reach) reach = out;
        }
    }
    return reach;
}

/* Is `lateral` on (or within `margin` of) any carriageway at span si? */
int tg_on_carriageway(const TG_NodeList *nl, int si, double lateral,
                             double margin)
{
    const double side = (lateral >= 0.0) ? 1.0 : -1.0;
    const double lat  = (lateral >= 0.0) ? lateral : -lateral;
    return lat <= tg_carriageway_reach(nl, si, side) + margin;
}

/* Push a setback measured from the MAIN ROAD EDGE out far enough that nothing
 * standing on it overlaps a carriageway. Returns `gap` unchanged where the road
 * edge is already the outermost tarmac (which is every span on the +ve lateral,
 * and every span off a fork). */
double tg_carriageway_clear_gap(const TG_NodeList *nl, int si,
                                       double side, double gap, double margin)
{
    const double need = tg_carriageway_reach(nl, si, side)
                      - tg_road_half_width(nl, si) + margin;
    return (gap < need) ? need : gap;
}

/* [R3 item 17] Geometry-safety validation pass. "Add additional checks to avoid
 * geometry of floor and walls getting in the way of the road."
 *
 * This is a read-only diagnostic run once the strip is built. It cannot MOVE
 * geometry (the meshes are already authored to clear the carriageway through
 * tg_carriageway_clear_gap / tg_side_blocked), so its job is to make the two
 * failure modes that DO put a floor on the road VISIBLE as a number in race.log
 * instead of a silent hole you can only find by driving into it:
 *
 *   1. ROAD SELF-OVERLAP. The centreline is an OPEN wandering walk
 *      (tg_build_centerline); when the section placer runs out of retries
 *      escaping a cul-de-sac it forces a straight and can leave the road passing
 *      back over an EARLIER stretch -- two road floors sharing the same ground.
 *      Flagged when two NON-neighbour spans' road surfaces actually overlap
 *      (centre distance < 0.8*(half_i+half_j)); a clean hairpin keeps its two
 *      sides a full road-width apart (the curvature-safety floor guarantees it)
 *      so it does NOT trip this.
 *   2. SUSPECT FORK REACH. The shared carriageway authority
 *      (tg_carriageway_reach) must never report a reach narrower than the plain
 *      road nor an implausibly wide one; either means a fork put drivable
 *      surface where scenery (or another carriageway) is placed. Uses the
 *      authority read-only -- no fork arithmetic is re-derived here.
 *
 * Scoped to the MAIN RING (s_ring_len): appended branch corridors legitimately
 * run beside the main road and would false-positive the overlap test. */
void tg_validate_geometry_safety(const TG_NodeList *nl, int nspans)
{
    int i, j, ring, overlaps = 0, bad_reach = 0, checked = 0;
    double span_len;

    if (!nl || nl->count < 3 || nspans < 3) return;
    ring = (s_ring_len > 0 && s_ring_len < nspans) ? s_ring_len : nspans;
    if (ring + 1 > nl->count) ring = nl->count - 1;
    if (ring < 3) return;

    /* Approx span spacing from the first step, to size the "legitimately near"
     * exclusion window: spans within a few of each other are ALWAYS close and
     * are not an overlap. */
    {
        const double dx = nl->v[1].x - nl->v[0].x;
        const double dz = nl->v[1].z - nl->v[0].z;
        span_len = sqrt(dx * dx + dz * dz);
        if (span_len < 1.0) span_len = 1.0;
    }

    for (i = 0; i < ring; i++) {
        const double hi = tg_road_half_width(nl, i);
        const double rr = tg_carriageway_reach(nl, i, -1.0);
        /* [R8 SHAPE G5] Ceiling the fork geometry can LEGITIMATELY produce,
         * derived rather than guessed. A branch centre sits a quarter road right
         * of the centreline plus at most the bow ceiling, and its own carriageway
         * extends up to half a road further. The old bound was a flat 6x the road
         * half width -- a number that sat above the then-current 1.20 bow ceiling
         * and below nothing in particular, so raising that ceiling for long forks
         * tripped it 116 times on seed 99991 with nothing actually wrong. Note
         * this makes the check STRICTER, not looser, when the long branch is off:
         * 14700 against the old 18001 at the shipped 6000-unit road width. */
        const double w = nl->v[i].width;
        const double bowmax = tg_r8_longbranch_enabled() ? TD5_TG_R8_BOW_LONG
                                                         : TD5_TG_BRANCH_BOW;
        const double lim = hi + w * (0.25 + bowmax + 0.5) + 1.0;
        if (rr < hi - 1.0 || rr > lim) {
            if (bad_reach < 8)
                TD5_LOG_W(LOG_TAG, "geometry-safety: span %d carriageway reach "
                          "%.0f vs road half %.0f (suspect fork geometry)",
                          i, rr, hi);
            bad_reach++;
        }
    }

    for (i = 0; i < ring; i++) {
        const double xi = nl->v[i].x, zi = nl->v[i].z;
        const double hi = tg_road_half_width(nl, i);
        /* Exclusion window: pairs this close along the path are neighbours and
         * are supposed to be near each other. Sized off the wider of the two
         * roads so a wide road's neighbours are never mistaken for an overlap. */
        for (j = i + 1; j < ring; j++) {
            const double hj = tg_road_half_width(nl, j);
            const int window = (int)((hi + hj) / span_len) + 6;
            double dx, dz, d2, lim;
            if (j - i <= window) continue;
            dx = nl->v[j].x - xi; dz = nl->v[j].z - zi;
            d2 = dx * dx + dz * dz;
            lim = (hi + hj) * 0.8;
            checked++;
            if (d2 < lim * lim) {
                if (overlaps < 8)
                    TD5_LOG_W(LOG_TAG, "geometry-safety: road self-overlap spans "
                              "%d<->%d (dist %.0f < %.0f)", i, j, sqrt(d2), lim);
                overlaps++;
            }
        }
    }

    if (overlaps || bad_reach)
        TD5_LOG_W(LOG_TAG, "geometry-safety: %d road-overlap pair(s), %d suspect "
                  "fork span(s) over %d main-ring spans (%d pairs tested)",
                  overlaps, bad_reach, ring, checked);
    else
        TD5_LOG_I(LOG_TAG, "geometry-safety: clean -- 0 road overlaps, 0 suspect "
                  "fork spans over %d main-ring spans (%d pairs tested)",
                  ring, checked);
}

/* ============ [R14 BRANCH item 2b] PAVEMENT OUTER FACE (the back edge) ======
 * "The sidewalk face OPPOSITE the road needs height -- it is visible from some
 * angles and currently reads as zero-thickness."
 *
 * Same defect class as R12 GEOM item 7 (the avenue median had no end cap), one
 * element over: a slab given a top and ONE side. Both pavement emitters
 * (tg_emit_branch_sidewalk below and tg_city_emit_sidewalk) lay the top and the
 * ROAD-facing kerb face, and nothing at all along the outer edge, so a 130-high
 * slab presents a zero-thickness line to any camera that sees its back -- which
 * is every camera at a fork mouth, a frontage gap, a park or a junction arm,
 * because there is no facade standing there to hide it. On a branch corridor
 * nothing EVER stands behind the pavement, so its back edge is open for the
 * whole fork.
 *
 * The face runs from the slab top down PAST road level to TD5_TG_GROUND_DROP,
 * the depth the ground skirt already sits at, so it buries into the terrain
 * instead of leaving a 70-unit slit between the kerb and the ground -- the same
 * mistake the R6 item-9 branch verge fix documents.
 *
 * WINDING, derived from the faces these emitters already write rather than
 * guessed (the R12 GEOM method): the convention is normal = (v1-v0) x (v2-v1).
 * Take a road-facing kerb face with the tangent along +X, where a point at
 * lateral t maps to z = -t: its order (inner-bottom, inner-top, far-top,
 * far-bottom) gives (0,K,0) x (L,0,0) = (0,0,-KL), i.e. -Z = +lateral, which is
 * toward the road on the RIGHT side. The outer face must point the other way,
 * so it takes the MIRRORED order -- outer-top, outer-bottom, far-outer-bottom,
 * far-outer-top -- giving (0,-K,0) x (L,0,0) = (0,0,+KL) = -lateral, outward.
 * Correct by construction, and by the SAME construction as the kerb face it
 * backs onto, so the two stay consistent if either is ever re-wound.
 *
 * Emitted on EVERY pavement span-side, not only the visibly-open ones: the face
 * occupies road level..kerb height at the slab's back edge and a facade wall
 * STARTS at kerb height standing on that edge, so there is no coplanar pair to
 * z-fight and a built frontage simply gains the plinth it was missing. One
 * extra quad per span-side, and no extra MESH -- TG_MAX_MESHES_PER_ENTRY is
 * untouched. TD5RE_R14_PAVE_FACE=0 restores the open back edge for an A/B. */
long s_r14_outer_faces;      /* outer faces emitted, both emitters */

int tg_r14_pave_face(void)
{
    return td5_env_flag_on("TD5RE_R14_PAVE_FACE");
}

/* ===================== BRANCH-CORRIDOR PAVEMENT (item 9a) =====================
 * The branch carriageway of a fork is APPENDED after the ring and never sees the
 * city hooks (tg_emit_fb_city runs on main-ring spans only), so a corridor was
 * bare road meeting open ground -- the "missing gaps between road and sidewalk
 * on branches" report. This lays a pavement along the branch's OUTER edge, on
 * the shipped sidewalk page, derived from the SAME shift/width the corridor road
 * mesh used (tg_branch_shift_s / tg_branch_wscale with the fork's own sep) so it
 * follows the bow and the taper instead of a fixed quarter-road assumption.
 *
 * Coordinate frame is identical to tg_append_row and tg_emit_gore: a point at
 * lateral t (POSITIVE = left of travel) off node n is
 * (n->x + n->tz*t, n->y, n->z - n->tx*t). The branch sits at NEGATIVE lateral;
 * its outer edge is the most-negative point, so the pavement runs further
 * negative still (t decreasing).
 *
 * `mb` is the base MAIN node the corridor step rides on; `acct_si` is the
 * APPENDED corridor span, passed only so the element inventory brackets the
 * pavement at the corridor spans (>= ring) -- that is what proves, without a
 * frame, that scenery now reaches the branch. */
int tg_emit_branch_sidewalk(const TG_NodeList *nl, int mb, int k, int L,
                                    int fi, const TG_Biome *b,
                                    TG_Buf *blk, size_t *moff, int *nmesh,
                                    int acct_si)
{
    const TG_Node *a = &nl->v[mb];
    const TG_Node *c = &nl->v[mb + 1];
    const double sw = tg_city_sidewalk_w(b);
    const double kh = tg_city_kerb_h(b);
    /* Branch centre and half width at each end, from the shared helpers. */
    const double sh0 = tg_fork_br_shift(fi, k,     a->width);
    const double sh1 = tg_fork_br_shift(fi, k + 1, c->width);
    const double h0  = a->width * tg_fork_br_wscale(fi, k)     * 0.5;
    const double h1  = c->width * tg_fork_br_wscale(fi, k + 1) * 0.5;
    (void)L;
    const double e0  = sh0 - h0;                 /* outer (right) edge, near */
    const double e1  = sh1 - h1;                 /* outer (right) edge, far  */
    const double u_w = sw / (double)TD5_TG_SPAN_LENGTH;
    const double u_k = kh / (double)TD5_TG_SPAN_LENGTH;
    double px[12], py[12], pz[12], uu[12], vv[12];
    int seg_page = TD5_TG_PAGE_SIDEWALK, seg_nq;
    int n = 0;

    if (sw <= 0.0) return 1;

    /* Top slab: near-edge, near-outer, far-outer, far-edge -- the up-facing
     * winding tg_emit_gore uses (near-high-t, near-low-t, far-low-t, far-high-t;
     * the outer point is at LOWER t because the branch is at negative lateral). */
    px[n]=a->x+a->tz*e0;        py[n]=a->y+kh; pz[n]=a->z-a->tx*e0;        uu[n]=0.0;  vv[n]=0.0; n++;
    px[n]=a->x+a->tz*(e0-sw);   py[n]=a->y+kh; pz[n]=a->z-a->tx*(e0-sw);   uu[n]=u_w;  vv[n]=0.0; n++;
    px[n]=c->x+c->tz*(e1-sw);   py[n]=c->y+kh; pz[n]=c->z-c->tx*(e1-sw);   uu[n]=u_w;  vv[n]=1.0; n++;
    px[n]=c->x+c->tz*e1;        py[n]=c->y+kh; pz[n]=c->z-c->tx*e1;        uu[n]=0.0;  vv[n]=1.0; n++;

    /* Kerb face, facing the branch carriageway (toward +lateral): base on the
     * asphalt, top at the slab. */
    px[n]=a->x+a->tz*e0; py[n]=a->y;    pz[n]=a->z-a->tx*e0; uu[n]=0.0; vv[n]=0.0; n++;
    px[n]=a->x+a->tz*e0; py[n]=a->y+kh; pz[n]=a->z-a->tx*e0; uu[n]=u_k; vv[n]=0.0; n++;
    px[n]=c->x+c->tz*e1; py[n]=c->y+kh; pz[n]=c->z-c->tx*e1; uu[n]=u_k; vv[n]=1.0; n++;
    px[n]=c->x+c->tz*e1; py[n]=c->y;    pz[n]=c->z-c->tx*e1; uu[n]=0.0; vv[n]=1.0; n++;

    /* [R14 BRANCH item 2b] OUTER FACE, the same missing back edge the main-road
     * slab had (see tg_r14_pave_face's block comment). The branch corridor is
     * where it shows worst: nothing stands behind a branch pavement at all, so
     * its back edge is open to the verge for the whole corridor.
     *
     * Winding: this emitter's kerb face runs base -> top -> far-top -> far-base
     * at the SAME lateral, which is the main slab's kerb order, so the outer
     * face takes the same mirrored order -- top, base, far-base, far-top -- and
     * points away from the carriageway by the identical derivation. The branch
     * lies at negative lateral, so its outer edge is at (e - sw). */
    if (tg_r14_pave_face()) {
        const double o0 = e0 - sw, o1 = e1 - sw;
        px[n]=a->x+a->tz*o0; py[n]=a->y+kh;                 pz[n]=a->z-a->tx*o0; uu[n]=0.0; vv[n]=0.0; n++;
        px[n]=a->x+a->tz*o0; py[n]=a->y-TD5_TG_GROUND_DROP; pz[n]=a->z-a->tx*o0; uu[n]=u_k; vv[n]=0.0; n++;
        px[n]=c->x+c->tz*o1; py[n]=c->y-TD5_TG_GROUND_DROP; pz[n]=c->z-c->tx*o1; uu[n]=u_k; vv[n]=1.0; n++;
        px[n]=c->x+c->tz*o1; py[n]=c->y+kh;                 pz[n]=c->z-c->tx*o1; uu[n]=0.0; vv[n]=1.0; n++;
        s_r14_outer_faces++;
    }

    seg_nq = n / 4;
    tg_acct_n(TG_ACCT_SIDEWALK, acct_si, 1);
    moff[(*nmesh)++] = blk->len;
    {   /* [R9 CITY item 2] provenance for the pavement-uniqueness sweep */
        const size_t p0 = blk->len;
        const int r = tg_write_quad_mesh(blk, px, py, pz, uu, vv, n,
                                         &seg_page, &seg_nq, 1);
        tg_pave_mark(p0, blk->len, TG_PVS_BRANCH, acct_si);
        return r;
    }
}

/* [R5 STRUCT item 10] Flat VERGE BAND along a branch corridor's OUTER edge.
 *
 * tg_emit_branch_sidewalk above lays a raised kerb+slab, but ONLY where the
 * corridor's base main node is a PAVED (city) biome -- tg_city_sidewalk_w is 0
 * for the billboard/tree biomes. On seed 99991 fork 2 (F=510) runs its corridor
 * mostly through ORIENTAL (main nodes 511-599), so its whole start had NO edge
 * treatment at all: bare road meeting open ground on the right, the reported
 * "no sidewalk on the right side of the branch". Meanwhile the ORIENTAL MAIN
 * road gets the flat verge band (tg_city_emit_verge_band). This gives the branch
 * the SAME out-of-town margin on its outer edge, mirroring the main road, so the
 * corridor is no longer bare where its biome has no pavement. Flat (no kerb),
 * one quad, lifted TD5_TG_VERGE_LIFT off the ground like the main-road band, on
 * the shipped sidewalk page. Frame and outer-edge derivation identical to
 * tg_emit_branch_sidewalk. Default ON; TD5RE_R5_BRANCH_VERGE=0 for an A/B. */
int tg_emit_branch_verge(const TG_NodeList *nl, int mb, int k, int L,
                                int fi, double bw,
                                TG_Buf *blk, size_t *moff, int *nmesh,
                                int acct_si)
{
    const TG_Node *a = &nl->v[mb];
    const TG_Node *c = &nl->v[mb + 1];
    const double sh0 = tg_fork_br_shift(fi, k,     a->width);
    const double sh1 = tg_fork_br_shift(fi, k + 1, c->width);
    const double h0  = a->width * tg_fork_br_wscale(fi, k)     * 0.5;
    const double h1  = c->width * tg_fork_br_wscale(fi, k + 1) * 0.5;
    (void)L;
    const double e0  = sh0 - h0;                 /* outer (right) edge, near */
    const double e1  = sh1 - h1;                 /* outer (right) edge, far  */
    const double lift = TD5_TG_VERGE_LIFT;
    const double u_w = bw / (double)TD5_TG_SPAN_LENGTH;
    double px[4], py[4], pz[4], uu[4], vv[4];
    int seg_page = TD5_TG_PAGE_SIDEWALK, seg_nq;
    int n = 0;

    if (bw <= 0.0) return 1;
    /* [R6 items 9 & 5] The INNER edge sits at ROAD LEVEL (a->y / c->y), the outer
     * edge at +lift. The band used to sit ENTIRELY at +lift, which floated its
     * whole inner edge TD5_TG_VERGE_LIFT above the carriageway it abuts. On the
     * main road the ground skirt fills behind that step, but a branch corridor
     * has NO skirt (pad + corridor carry road only), so the step was open to the
     * void -- the "small see-through gap between the branch and the sidewalk"
     * (item 9), and the floating band parallaxing against the road as the car
     * moved read as "the floor on the right side of the branch MOVES" (item 5).
     * Meeting the road edge exactly and ramping up to the lift outboard closes
     * both. Winding unchanged: near-edge, near-outer, far-outer, far-edge; the
     * branch is at negative lateral so "further out" is t decreasing (e - bw). */
    px[n]=a->x+a->tz*e0;        py[n]=a->y;      pz[n]=a->z-a->tx*e0;        uu[n]=0.0; vv[n]=0.0; n++;
    px[n]=a->x+a->tz*(e0-bw);   py[n]=a->y+lift; pz[n]=a->z-a->tx*(e0-bw);   uu[n]=u_w; vv[n]=0.0; n++;
    px[n]=c->x+c->tz*(e1-bw);   py[n]=c->y+lift; pz[n]=c->z-c->tx*(e1-bw);   uu[n]=u_w; vv[n]=1.0; n++;
    px[n]=c->x+c->tz*e1;        py[n]=c->y;      pz[n]=c->z-c->tx*e1;        uu[n]=0.0; vv[n]=1.0; n++;

    seg_nq = n / 4;
    tg_acct_n(TG_ACCT_BRANCH_VERGE, acct_si, 1);
    moff[(*nmesh)++] = blk->len;
    {   /* [R9 CITY item 2] provenance for the pavement-uniqueness sweep */
        const size_t p0 = blk->len;
        const int r = tg_write_quad_mesh(blk, px, py, pz, uu, vv, n,
                                         &seg_page, &seg_nq, 1);
        tg_pave_mark(p0, blk->len, TG_PVS_BRVERGE, acct_si);
        return r;
    }
}

/* [R7 item 10] SCENERY on the branch corridor's GRASS verge. tg_emit_branch_verge
 * lays a flat grass band along the corridor's outer edge, but nothing DRESSES it,
 * so a fork through a tree biome (seed 99991 fork 2 runs its corridor through
 * ORIENTAL) showed bare grass and nothing else beside the branch -- "barely any
 * grass to the side and nothing else ... add more scenery on the side if it is
 * grass" (item 10, span 569). This plants the biome's OWN roadside tree billboards
 * along that verge, mirroring the main road's roadside trees (tg_building_for_span
 * tree path), so the branch flank reads like the main road's instead of a bald
 * strip.
 *
 * PLACEMENT clears the branch through the shared carriageway authority: the
 * setback is measured from the MAIN road edge and pushed out past the bowed
 * corridor's CURRENT width by tg_carriageway_clear_gap on the RIGHT side, so a
 * trunk can never land on the branch carriageway (the GUARD contract -- the same
 * adoption facades, terrain skirts and the fork backdrop use). Billboard (grass)
 * biomes only; where the biome has no trees nothing is added and the verge is
 * unchanged. `mb` is the base MAIN node the corridor step rides on; `acct_si` is
 * the appended corridor span, so the inventory brackets the tree at the corridor
 * spans (>= ring) -- what proves, without a frame, that scenery now reaches the
 * branch flank. Default ON; TD5RE_R7_BRANCH_FLORA=0 for an A/B. */
int tg_emit_branch_flora(const TG_NodeList *nl, int mb,
                                const TG_Biome *b, TG_Buf *blk,
                                size_t *moff, int *nmesh, int acct_si)
{
    const TG_Node *n = &nl->v[mb];
    unsigned int h = (unsigned)mb * 2654435761u;
    const TG_TreePage *tp;
    double jit, tw, th, gap, set, cx, cz;
    int tv;

    if (!b->billboard || b->tree_n <= 0) return 1;
    if (!td5_env_flag_on("TD5RE_R7_BRANCH_FLORA")) return 1;
    /* One tree in roughly half of the corridor spans: a dressed verge, not a
     * solid wall of trunks hugging the branch. */
    if ((h >> 28) & 1u) return 1;

    tv  = b->tree_set[(h >> 13) % (unsigned)b->tree_n];
    tp  = &k_tree_pages[tv];
    jit = 0.8 + (double)((h >> 9) % 41) * 0.01;        /* 0.80 .. 1.20 */
    tw  = (double)tp->w * jit;
    th  = (double)tp->h * jit;
    /* Base gap past the branch's own grass verge, then clear_gap lifts it past
     * the bowed corridor so the trunk clears the branch at its ACTUAL width. */
    gap = tg_verge_band_w(b) + 500.0 + (double)((h >> 5) % 1800);
    set = n->width * 0.5
        + tg_carriageway_clear_gap(nl, mb, -1.0, gap, TD5_TG_CARRIAGEWAY_MARGIN)
        + tw * 0.5;
    /* Right of travel is NEGATIVE lateral: point at lateral t off node n is
     * (n->x + n->tz*t, n->y, n->z - n->tx*t). */
    cx = n->x + n->tz * (-set);
    cz = n->z - n->tx * (-set);
    /* [R12 item 4] the one tree emitter that does not go through tg_flora_diag;
     * it takes the same shared spacing rule -- see tg_r12_flora_accept. */
    if (!tg_r12_flora_accept(mb, "branch", tg_tree_slot(tv), -1.0,
                             cx, cz, tw, th)) return 1;
    tg_acct_n(TG_ACCT_R7_BRANCH, acct_si, 1);
    moff[(*nmesh)++] = blk->len;
    /* [R18 EDGE item 1] "these trees are floating near the road" on a branch
     * CORRIDOR. This emitter planted the trunk at n->y -- the MAIN node's ROAD
     * height -- exactly the bug tg_flora_plant (R7 item 18 / R9 topo item 6)
     * fixed for the main-road roadside trees, but this branch-corridor emitter
     * never inherited the same drop sampling. The trunk sits out past the bowed
     * branch (set = half road + clear_gap + half canopy), where the outboard
     * skirt/far-band terrain has already fallen away from the road, so a tree
     * pinned at road height hangs in the air above it.
     *
     * Sample the ground drop at the trunk's lateral distance from the SAME
     * right-side (-1) cross-section the branch's own skirt/far-band are built
     * from, on the MAIN node mb (< ring, so tg_topo_road_cap is honoured), and
     * stand the tree on it. Uses the whole topo chain (skirt + far-band descent)
     * where topo is on -- the trunk lands past the branch clearance, i.e. out in
     * the far band -- and the plain skirt profile otherwise. Can only ever LOWER
     * the base onto terrain, never raise it. Default ON, mirroring the main-road
     * flora that already ships this behaviour; TD5RE_R18_BRANCH_FLORA_DROP=0
     * restores the flat road-height plant for an A/B. */
    {
        double base_y = n->y;
        if (td5_env_flag_on("TD5RE_R18_BRANCH_FLORA_DROP")) {
            const double d = set - n->width * 0.5;   /* trunk dist from road edge */
            if (tg_topo_enabled()) {
                TG_TopoChain c;
                tg_topo_chain(nl, mb, 0 /*right of travel*/, &c);
                base_y = n->y - tg_topo_drop_at(&c, d);
            } else {
                const double wsd = b->water ? tg_water_side(mb) : 0.0;
                base_y = n->y - tg_infra_ground_dy(nl, mb, -1.0, d, wsd);
            }
        }
        return tg_emit_billboard_mesh(blk, cx, base_y, cz, tw * 0.5, th,
                                      tg_tree_slot(tv), 1);
    }
}

/* ===================== AVENUE DIVIDER (items 9c / 10) =====================
 * Where a fork is TIGHT -- a divided AVENUE rather than a road splitting in two
 * (tg_fork_is_avenue) -- the gore between the two carriageways is a slim central
 * strip, and a real avenue puts something ON it. This raises a median island
 * down the centre of the gore. THREE treatments, chosen by the fork's ordinal so
 * a track with several avenues shows all of them: a planted strip, a concrete
 * barrier, and a plain kerbed island ("a distinction of different dividers").
 *
 * A 3-quad prism (top + the two road-facing side walls), not a single decal,
 * because the car passes on BOTH sides and each face must be visible from its
 * carriageway even with backface culling on -- the same reasoning the guardrail
 * prism documents. Windings verified against the gore's up-facing top quad.
 *
 * The island pinches to nothing at the fork and the rejoin (the gore does too),
 * so it opens out of and closes back into the full-width road like a real
 * avenue median opening from an intersection. `sh*`/`half*` are the branch shift
 * and half width at the span's two ends, exactly as passed to tg_emit_gore. */
int tg_emit_avenue_divider(const TG_NodeList *nl, int si, int fork_index,
                                  double sh0, double sh1, double half0,
                                  double half1, int br_lanes, TG_Buf *blk,
                                  size_t *moff, int *nmesh)
{
    const TG_Node *a = &nl->v[si];
    const TG_Node *c = &nl->v[si + 1];
    /* The gore runs from the main road's right edge (lateral ~0) to the branch's
     * LEFT edge (sh+half, negative). Median centre = halfway; gore width = how
     * far that edge is from the road centre. */
    const double bl0 = sh0 + half0, bl1 = sh1 + half1;   /* branch left edges */
    const double gw0 = -bl0, gw1 = -bl1;                 /* gore widths (>=0)  */
    /* [R8 item 17] Same rule as the gore surface: a PLANTED median (treat 0,
     * grass top) is not legal inside a bore. Substitute the concrete barrier,
     * which is what a real divided tunnel carries, for the spans that are
     * actually enclosed -- the swap lands on the portal line, where a material
     * change belongs. Only the planted treatment is affected; a kerbed island
     * indoors is fine. */
    const int    treat0 = (tg_span_in_tunnel(si) &&
                           td5_env_flag_on("TD5RE_R8_BORE_MEDIAN") &&
                           (((unsigned)fork_index) % 3) == 0)
                        ? 1 : (int)(((unsigned)fork_index) % 3);
    /* [R11 CROSS item 8] "Avoid medians at the same height as the road."
     *
     * MEASURED first, on seed 20260901: the median SURFACE is tg_emit_gore's
     * floor, and it is laid at road level minus TD5_TG_GORE_DROP -- 4 units --
     * on every fork span of the track (logged rise=-4 for all of forks 0/1/2,
     * gore widths 0..5698). It is not approximately flush, it IS flush; the
     * only thing that ever stood proud of it was this island, and this island
     * ran on AVENUE forks only. Forks 1 and 2 got no island at all, so their
     * whole median was a strip of ground texture lying in the road surface --
     * the report, verbatim.
     *
     * So the island now runs on every fork (see the call site), and where the
     * gore is MEDIAN-SIZED it is widened to fill it instead of leaving flush
     * margins either side. A gore wider than TD5_TG_R11_MEDIAN_MAX is not a
     * median at all -- it is a genuine split with scenery living between the
     * two carriageways -- and is left exactly as it was.
     *
     * The FLOOR stays put, byte for byte: it underlaps both carriageways by
     * TD5_TG_GORE_OVERLAP to close the see-through slit at the fork mouth, so
     * raising it would put a kerb lip 240 units into each live lane. The island
     * is inset from both edges by TD5_TG_R11_MEDIAN_INSET (> that overlap)
     * instead, which keeps every raised face inside the gore. */
    const int    fill   = tg_r12_median_fill(gw0, gw1);
    /* A CONCRETE BARRIER is narrow by nature, so filling a 2000-unit median
     * with one would be a lie about what the material is. Where the fill
     * applies, the treatment rolls between planted and kerbed only -- and a
     * planted top is still illegal in a bore, exactly as above. */
    const int    treat  = !fill ? treat0
                        : ((((unsigned)fork_index) & 1u)
                           || (tg_span_in_tunnel(si)
                               && td5_env_flag_on("TD5RE_R8_BORE_MEDIAN")))
                          ? 2 : 0;
    const double mw_cap = (treat == 1) ? 260.0 : (fill ? 1e9 : 520.0);
    double       H      = (treat == 1) ? 360.0
                        : fill         ? TD5_TG_R11_MEDIAN_H
                        : (treat == 0) ? 220.0 : 150.0;
    /* [R16 MEDIAN] "always median with height": a surviving median must stand
     * proud enough to read. Only the kerbed treatment (150) fell below this;
     * the report's "barely visible" stub. Floor it. Purely vertical -- the top
     * quad and both side walls just get taller, no lateral change, so no lane
     * intrusion and the placement mirror (which tests position, not height) is
     * unaffected. TD5RE_MEDIAN_MIN_H=0 restores the per-treatment heights. */
    if (td5_env_flag_on("TD5RE_MEDIAN_MIN_H") && H < TD5_TG_MEDIAN_MIN_H)
        H = TD5_TG_MEDIAN_MIN_H;
    /* [R8 TERRAIN item 16] A PLANTED median (treat 0, grass top) in a snow biome
     * is a green strip between two icy carriageways -- the verbatim complaint.
     * Snow it over with the ploughed-snow page, which is distinct from the snow
     * GROUND page so the median still reads as its own surface. The barrier and
     * kerb treatments are man-made and keep their concrete. */
    const int    page   = (treat == 0) ? tg_r8_median_page(si, TD5_TG_PAGE_GREEN)
                        : (treat == 1) ? TD5_TG_PAGE_RAIL : TD5_TG_PAGE_SIDEWALK;
    /* Item 8: the two vertical side walls must NOT wear the TOP page. A planted
     * median (treat 0, GREEN top) painted grass up its walls -- verbatim "grass
     * as walls is wrong"; a kerbed island (treat 2) put paving slabs on its
     * walls. Both want a concrete KERB face on the sides. A concrete barrier
     * (treat 1, RAIL) is a wall material through and through, so it keeps RAIL on
     * every face. Per-quad paging via a 2-segment mesh: seg 0 = the single TOP
     * quad on `page`, seg 1 = the two side-wall quads on `side_page`. */
    const int    side_page = (treat == 1) ? TD5_TG_PAGE_RAIL : TD5_TG_PAGE_BRANCH_KERB;
    double mc0, mc1, mw0, mw1, cl0, cr0, cl1, cr1, base0, base1;
    /* [R12 GEOM item 7] 12 -> 20: room for the two END CAP quads. */
    double px[20], py[20], pz[20], uu[20], vv[20];
    int seg_page[2], seg_nq[2];
    int n = 0;
    /* [R12 GEOM item 7] Run ends, from the shared placement predicate. */
    const int cap    = tg_r12_median_cap();
    const int cap_in = cap && !tg_r12_median_at(nl, si - 1, br_lanes);
    const int cap_out= cap && !tg_r12_median_at(nl, si + 1, br_lanes);

    /* [R11 CROSS item 8] INSTRUMENT the height question before touching it:
     * what actually stands proud of the road on this median span, and by how
     * much? `rise` is the island top relative to the carriageway it divides --
     * the number the complaint is about ("flush with the road reads as a
     * texture stripe"). A skipped island leaves only the gore floor, which sits
     * TD5_TG_GORE_DROP *below* road level, i.e. rise = -4. */
    if (tg_r11_cross_diag())
        TD5_LOG_I(LOG_TAG, "trackgen: [R11 MEDIAN] span %4d fork %d treat %d "
                  "gw=%.0f/%.0f mw_cap=%.0f H=%.0f rise=%.0f%s",
                  si, fork_index, treat, gw0, gw1, mw_cap, H,
                  H - TD5_TG_GORE_DROP,
                  (gw0 < 200.0 && gw1 < 200.0) ? " SKIPPED(sliver)"
                                               : (fill ? " FILL" : ""));

    /* No island where the gore is a mere sliver (near the mouths): a 100-unit
     * strip of raised concrete popping in and out reads worse than nothing. */
    if (gw0 < 200.0 && gw1 < 200.0) return 1;
    /* [R16 MEDIAN] "should be longer by default": a fork too short to carry a
     * readable median carries none. Mirrored in tg_median_at_raw so end caps
     * agree. Placed with the sliver reject -- both are "this span emits no
     * island" gates. */
    if (!tg_median_fork_long_enough(fork_index)) return 1;
    /* [R11 CROSS item 8] A fork that is NOT an avenue only gains an island
     * where the fill applies. Without this the widened call site would also
     * drop the shipped 0.32-capped island into the middle of a 5000-unit
     * SPLIT, which is a different thing from a median and was never asked for.
     * The avenue path is unchanged, fill or no fill. */
    if (!fill && !tg_fork_is_avenue(fork_index)) return 1;

    mc0 = bl0 * 0.5; mc1 = bl1 * 0.5;                    /* median centre lateral */
    /* [R11 CROSS item 8] FILL: half the gore less the inset, i.e. the island's
     * faces stand TD5_TG_R11_MEDIAN_INSET clear of each carriageway edge. Never
     * NARROWER than the shipped 0.32 rule, so widening can only ever add
     * raised median; the max() is what makes that a property rather than an
     * arithmetic accident at some particular width. */
    mw0 = gw0 * 0.32;
    mw1 = gw1 * 0.32;
    if (fill) {
        /* [R17 ROADMARK item 3] "this grass is overlapping with road ... the
         * median should be the same width." The gore FLOOR (tg_emit_gore, a
         * single GREEN quad) spans the whole visible gore -- road inner edge
         * (lateral 0) to branch inner edge (lateral bl) -- and underlaps each
         * carriageway a further TD5_TG_GORE_OVERLAP. The filled island stood
         * TD5_TG_R11_MEDIAN_INSET (300) clear of each edge, so a flat 300-unit
         * strip of that GREEN floor was left EXPOSED at road level on each side
         * of the raised island: grass lying beside (and, via the overlap,
         * lapping onto) the tarmac. Reaching the carriageway edge (inset 0)
         * makes the raised island cover the whole visible gore floor, so the
         * only GREEN the driver sees beside the lane is the island's own raised
         * top -- the floor's overlap now hides UNDER the main road quad on the
         * road side and UNDER the branch road quad on the branch side. The
         * raised kerb face then sits AT the lane edge (lateral 0 / bl), not in
         * the lane: those are the carriageways' own inner edges, so nothing new
         * enters a live lane. Height and placement are untouched, so the R16
         * MIN_H floor and the tg_median_at_raw end-cap mirror still agree.
         * TD5RE_R17_MEDIAN_FLUSH=0 restores the 300-unit inset for an A/B. */
        const double inset = td5_env_flag_on("TD5RE_R17_MEDIAN_FLUSH")
                           ? 0.0 : TD5_TG_R11_MEDIAN_INSET;
        const double f0 = gw0 * 0.5 - inset;
        const double f1 = gw1 * 0.5 - inset;
        if (f0 > mw0) mw0 = f0;
        if (f1 > mw1) mw1 = f1;
    }
    if (mw0 > mw_cap) mw0 = mw_cap;
    if (mw0 < 0.0)    mw0 = 0.0;
    if (mw1 > mw_cap) mw1 = mw_cap;
    if (mw1 < 0.0)    mw1 = 0.0;
    cl0 = mc0 + mw0; cr0 = mc0 - mw0;                    /* road side / branch side */
    cl1 = mc1 + mw1; cr1 = mc1 - mw1;
    /* Sit the base at the gore's own level (4 below road) so the island rises
     * out of the median rather than floating a hair above it. */
    base0 = a->y - TD5_TG_GORE_DROP; base1 = c->y - TD5_TG_GORE_DROP;

    /* TOP (up-facing): near-road, near-branch, far-branch, far-road. */
    px[n]=a->x+a->tz*cl0; py[n]=base0+H; pz[n]=a->z-a->tx*cl0; uu[n]=0.0; vv[n]=0.0; n++;
    px[n]=a->x+a->tz*cr0; py[n]=base0+H; pz[n]=a->z-a->tx*cr0; uu[n]=1.0; vv[n]=0.0; n++;
    px[n]=c->x+c->tz*cr1; py[n]=base1+H; pz[n]=c->z-c->tx*cr1; uu[n]=1.0; vv[n]=1.0; n++;
    px[n]=c->x+c->tz*cl1; py[n]=base1+H; pz[n]=c->z-c->tx*cl1; uu[n]=0.0; vv[n]=1.0; n++;

    /* ROAD-side wall (faces +lateral, toward the main carriageway). */
    px[n]=a->x+a->tz*cl0; py[n]=base0;   pz[n]=a->z-a->tx*cl0; uu[n]=0.0; vv[n]=1.0; n++;
    px[n]=a->x+a->tz*cl0; py[n]=base0+H; pz[n]=a->z-a->tx*cl0; uu[n]=0.0; vv[n]=0.0; n++;
    px[n]=c->x+c->tz*cl1; py[n]=base1+H; pz[n]=c->z-c->tx*cl1; uu[n]=1.0; vv[n]=0.0; n++;
    px[n]=c->x+c->tz*cl1; py[n]=base1;   pz[n]=c->z-c->tx*cl1; uu[n]=1.0; vv[n]=1.0; n++;

    /* BRANCH-side wall (faces -lateral, toward the corridor). */
    px[n]=a->x+a->tz*cr0; py[n]=base0;   pz[n]=a->z-a->tx*cr0; uu[n]=0.0; vv[n]=1.0; n++;
    px[n]=c->x+c->tz*cr1; py[n]=base1;   pz[n]=c->z-c->tx*cr1; uu[n]=1.0; vv[n]=1.0; n++;
    px[n]=c->x+c->tz*cr1; py[n]=base1+H; pz[n]=c->z-c->tx*cr1; uu[n]=1.0; vv[n]=0.0; n++;
    px[n]=a->x+a->tz*cr0; py[n]=base0+H; pz[n]=a->z-a->tx*cr0; uu[n]=0.0; vv[n]=0.0; n++;

    /* [R12 GEOM item 7] LEADING END CAP -- the face the report is about. Only
     * where the previous span carries no island, so an interior section stays a
     * single continuous prism with nothing coincident inside it.
     *
     * WINDING, derived rather than tried: the emitted faces above establish the
     * convention as normal = (v1-v0) x (v2-v1). Check it on the road-side wall
     * with the tangent along +X (tx=1, tz=0, so lateral t maps to z = -t): its
     * corners give (0,H,0) x (L,0,0) = (0,0,-HL), i.e. -Z = +lateral, exactly
     * what its own comment claims. A cap facing BACK down the road therefore
     * needs -X, and going base-road -> base-branch -> top-branch -> top-road
     * gives (0,0,2mw) x (0,H,0) = (-2mw*H,0,0). Correct by construction.
     *
     * Concrete `side_page`, not the top's page: the nose of a median is the same
     * cast face as its walls, and a grass or paving-slab end cap is the R8
     * item-8 "grass as walls" mistake seen end-on. */
    if (cap_in) {
        px[n]=a->x+a->tz*cl0; py[n]=base0;   pz[n]=a->z-a->tx*cl0; uu[n]=0.0; vv[n]=1.0; n++;
        px[n]=a->x+a->tz*cr0; py[n]=base0;   pz[n]=a->z-a->tx*cr0; uu[n]=1.0; vv[n]=1.0; n++;
        px[n]=a->x+a->tz*cr0; py[n]=base0+H; pz[n]=a->z-a->tx*cr0; uu[n]=1.0; vv[n]=0.0; n++;
        px[n]=a->x+a->tz*cl0; py[n]=base0+H; pz[n]=a->z-a->tx*cl0; uu[n]=0.0; vv[n]=0.0; n++;
        s_r12_median_caps++;
        s_r12_median_runs++;
    }
    /* [R12 GEOM item 7] TRAILING END CAP, faces +X (the mirror order). The
     * report only names the beginning, but the trailing section is open by the
     * same three-quad construction, and it is what a driver sees in the mirror
     * and on any reverse or circuit lap -- so the CLASS is "cap every island
     * end", not "cap the one the screenshot showed". */
    if (cap_out) {
        px[n]=c->x+c->tz*cl1; py[n]=base1+H; pz[n]=c->z-c->tx*cl1; uu[n]=0.0; vv[n]=0.0; n++;
        px[n]=c->x+c->tz*cr1; py[n]=base1+H; pz[n]=c->z-c->tx*cr1; uu[n]=1.0; vv[n]=0.0; n++;
        px[n]=c->x+c->tz*cr1; py[n]=base1;   pz[n]=c->z-c->tx*cr1; uu[n]=1.0; vv[n]=1.0; n++;
        px[n]=c->x+c->tz*cl1; py[n]=base1;   pz[n]=c->z-c->tx*cl1; uu[n]=0.0; vv[n]=1.0; n++;
        s_r12_median_caps++;
    }

    /* TOP quad (verts 0..3) on `page`; the two side walls (verts 4..11) and the
     * end caps (12..19) on `side_page`. Segments consume quads sequentially in
     * vertex order, so this split matches the emit order above exactly. */
    seg_page[0] = page;      seg_nq[0] = 1;
    seg_page[1] = side_page; seg_nq[1] = 2 + (cap_in ? 1 : 0) + (cap_out ? 1 : 0);
    /* [R12 GEOM item 7] VERTEX-LEVEL EVIDENCE. Dumps the prism this call is
     * about to write: how many quads it has, which ends it closes, and the
     * near-end cross-section corners. Also self-checks the shared predicate --
     * tg_r12_median_at(si) must agree with the fact that this call reached the
     * write, or the neighbour queries above are asking a stale question. */
    if (tg_r12_geom_diag())
        TD5_LOG_I(LOG_TAG, "trackgen: [R12 MEDIAN] span %4d fork %d quads=%d "
                  "(top 1 + walls 2 + caps %d) cap_in=%d cap_out=%d self=%d "
                  "near cl=%.0f cr=%.0f base=%.0f top=%.0f",
                  si, fork_index, n / 4,
                  (cap_in ? 1 : 0) + (cap_out ? 1 : 0), cap_in, cap_out,
                  tg_r12_median_at(nl, si, br_lanes),
                  cl0, cr0, base0, base0 + H);
    /* Accounted as a FENCE (a linear median structure) rather than a new
     * inventory kind, to keep the shared enum untouched for the parallel batch. */
    tg_acct_n(TG_ACCT_FENCE, si, 1);
    moff[(*nmesh)++] = blk->len;
    return tg_write_quad_mesh(blk, px, py, pz, uu, vv, n, seg_page, seg_nq, 2);
}
