/**
 * td5_tg_realfork.c -- GEO TRACK: DRIVEABLE FORKS FROM THE REAL MAP (PORT-ONLY).
 *
 * [ROUND 1013 F2] Mariano, after testing master bc143d22: "when there's avenues
 * by default those should be driveable as forks, with the plazas as well, by
 * default every road that is too close to the main road should be treated a
 * driveable fork".
 *
 * WHAT THIS REPLACES, AND WHY ROUND 1009 WAS WRONG. Round 1009 turned a divided
 * avenue into an ISLAND fork: the race road was widened to lanes(A)+lanes(B) and
 * split into two carriageways bowing apart by a fixed `sep`. Round 1010 removed
 * it, correctly: the bow was a constant (TD5_TG_BRANCH_SEP_MIN, not a width), the
 * far carriageway was a bowed COPY of the race road rather than the OSM way sitting
 * beside it, and the detector paired a road with its own split segments. Since
 * 1010 the real opposite carriageway is built from AVENUES.JSON, but as scenery.
 *
 * WHAT THIS DOES. It makes that same real carriageway driveable, by reusing the
 * engine's own fork machinery (span records, jump table, type 8/9/10/11 spans,
 * AI + traffic fork choice, carriageway authority) with ONE difference: the
 * corridor's lateral profile is the MEASURED gap between the two real
 * carriageways, never a half-sine bow.
 *
 *   FORK GEOMETRY  F and R sit at real median openings (or at the ends of the
 *                  real run), the corridor carries the REAL lane count of the
 *                  way it follows, and the median between the two carriageway
 *                  edges is the map's own: median(k) = |offset(k)| - half of both
 *                  carriageways, pinned to zero at the two mouths so the corridor
 *                  lines up with the road halves there and opened at no more than
 *                  TD5RE_GEO_FORK_SLOPE (6.8 degrees), because the engine's own
 *                  0.35 span/span rate is a ceiling, not a shape a car at 180 km/h
 *                  can follow.
 *
 *   NODE WINDOW    The engine's fork has one cross-section at F: main half +
 *                  corridor half side by side. So over each fork's window the
 *                  route's ring nodes carry lanes(A)+lanes(B), RAMPED in over a
 *                  16 node taper in front of the window and out behind it (the
 *                  walk's one-lane-per-seam rule made that a 45 degree edge) and
 *                  added on the corridor's side ONLY: the node moves half the
 *                  added width toward the corridor,
 *                  which keeps the route carriageway's own edge exactly where the
 *                  map has it. That is the line between this and round 1009, whose
 *                  widening was symmetric and displaced the race road by half the
 *                  other carriageway.
 *
 * THREE SOURCES (each has a knob, all default ON, see tg_realfork_build):
 *   avenue    TD5RE_GEO_FORK_AVENUE    AVENUES.JSON's real opposite carriageway.
 *   plaza     TD5RE_GEO_FORK_PLAZA     (reserved -- see the "NOT BUILT" note in
 *                                      tg_realfork_build; needs free corridors)
 *   parallel  TD5RE_GEO_FORK_PARALLEL  a real road close to and roughly parallel
 *                                      with the route, connected at both ends.
 * TD5RE_GEO_REAL_FORKS=0 turns all of it off (the round-1012 behaviour).
 *
 * BYTE-IDENTICAL SYNTHETIC BUILDS. Every entry point returns "nothing" with no
 * geo place loaded, before any file is opened or any table written, and no
 * tg_rand / tg_frand / tg_range call is made here: the single RNG stream is
 * untouched -- the standing rule at td5_trackgen_internal.h:1290-1296.
 */
#include "td5_trackgen_internal.h"
#include "td5_geo.h"              /* is a place loaded, the conditioned route */
#include "td5_geo_avenues.h"      /* AVENUES.JSON: the real divided avenues   */
#include "td5_geo_roads.h"        /* the real OSM road graph (parallel roads) */

#define TG_RF_SRC_AVENUE    0
#define TG_RF_SRC_PLAZA     1
#define TG_RF_SRC_PARALLEL  2

#define TG_RF_STEPS         512   /* TD5_TG_BYPASS_MAXK: corridor steps/lateral */
#define TG_RF_MAXCAND       192
#define TG_RF_MINLEN        24    /* spans: 84 m, TD5_TG_BRANCH_MIN_LEN         */
#define TG_RF_MAXLEN        480
#define TG_RF_LANES_TOTAL   8     /* the lane range the rail LUTs are exercised in */
#define TG_RF_WIDEN         (TD5_TG_BRANCH_WIDEN + 2)   /* approach spans kept uniform */
/* The lane TAPER, in nodes, in front of F and behind R. Measured on Mariano's
 * route (round 1013 F2b): with the lane count stepping one lane per node the road
 * pinched from 4 to 2 lanes over 2 spans, a 45 degree edge taper (one 3.5 m lane
 * per 3.5 m span). Cars leaving the rejoin hit that edge at 130-190 km/h: 5 contact
 * events and two pile-ups at spans 119..124 behind fork 0, none on the same stretch
 * with real forks off. The width now ramps linearly over this many nodes (16 =
 * 56 m, 7 m of width, about 7 degrees) and the lane COUNT follows it by rounding.
 * (TD5RE_GEO_FORK_TAPER sets the length, TD5RE_GEO_FORK_TAPER_SMOOTH=1 a smoothstep
 * shape: measured no better than the linear 16 over 5 race seeds, see the doc.) */
#define TG_RF_TAPER_DEF     16
/* [ROUND 1015 A] The shortest ramp a fork may be built with at either end. A ramp is
 * normally TG_RF_TAPER_DEF nodes; where the start grid, a lane-count change or the
 * finish takes some of that room it is SHORTENED to what is left, down to this. 6
 * nodes is 21 m for the 3.5 m of one lane (9.5 degrees): steeper than the 7 degrees
 * of the full ramp, far gentler than the 45 degrees of the pre-F2b step. Below it the
 * candidate is refused as before. TD5RE_GEO_FORK_TAPER_FIT=0 restores the old rule
 * (full ramp or nothing, route lanes constant over the whole window). */
#define TG_RF_TAPER_MIN     6
/* How far down the avenue the FIRST fork of a run may slide its start to find a clean window. */
#define TG_RF_START_SLIDE   24
/* Two forks may share a taper (the node takes the larger of the two widths, so
 * the road narrows after one and widens again before the next without having to
 * close), but their FULL-width windows F-8 .. R+2 stay at least this far apart. */
#define TG_RF_MIN_GAP       6
/* A fork's window (taper tail included) ends at least this many spans before the
 * finish line, so the gantry never stands in a taper. */
#define TG_RF_FINISH_GAP    8
#define TG_RF_NODES         (TD5_TG_MAX_SPANS + 8)

typedef struct {
    int    src;
    int    F, R;                       /* split span, rejoin span              */
    int    len;                        /* R - F - 1                            */
    int    lanes_a, lanes_b;
    int    i0, w_end;                  /* node window: lane ramp .. last shifted node */
    int    taper_in, taper_out;        /* [1015 A] the ramp lengths actually used     */
    double weight;
    char   name[64];
    double off[TG_RF_STEPS + 2];       /* signed lateral from A's centre, node F+j */
} RfCand;

typedef struct {
    int    src;
    int    F, len, R;
    int    lanes_a, lanes_b;
    int    i0, w_end;
    int    taper_in, taper_out;
    char   name[64];
    double med[TG_RF_STEPS + 2];       /* effective median width per step 0..len  */
    double med_real_lo, med_real_hi;   /* what the map says, before the taper     */
    double med_eff_hi;
    int    contra;                     /* the real way flows AGAINST the race (log) */
} RfFork;

static RfCand  s_cand[TG_RF_MAXCAND];
static int     s_ncand;
static RfFork  s_rf[TD5_TG_BRANCH_MAX];
static int     s_rf_n;

/* The route as td5_geo loaded it (raw, before the walk shifts anything). */
static double  s_rx[TG_RF_NODES], s_rz[TG_RF_NODES];
static int     s_rl[TG_RF_NODES];
static int     s_rn;

/* Per node: which fork's window it is in (-1 none), the lanes the walk must see
 * there, the lanes of the route's own carriageway, and the lateral delta the walk
 * applied (signed, +t, world units). */
static int     s_node_fork[TG_RF_NODES];
static int     s_node_ovr[TG_RF_NODES];
static double  s_node_dt[TG_RF_NODES];
/* Lanes of the corridor's width added to node i, in lane units, 0..lanes_b. The
 * node's width is (route lanes + extra) lanes and the node moves half of it toward
 * the corridor: continuous, where the lane COUNT can only step. */
static double  s_node_extra[TG_RF_NODES];
static int     s_rf_taper = TG_RF_TAPER_DEF;
static int     s_rf_taper_smooth = 0;

static int s_rf_diag;
/* The two env knobs, read ONCE per build. tg_realfork_enabled() sits under every
 * carriageway-reach query the scenery makes, which run on the stream worker's
 * threads for every span, so it must not call getenv each time. -1 = not read. */
static int s_rf_knob = -1;

int tg_realfork_enabled(void)
{
    if (!td5_geo_loaded()) return 0;
    if (td5_geo_route_count() < 2) return 0;
    if (s_rf_knob < 0)      /* read before the first build (tg_geo_forks_n asks) */
        return td5_env_flag_on("TD5RE_GEO_FORKS") && td5_env_flag_on("TD5RE_GEO_REAL_FORKS");
    return s_rf_knob;
}

int tg_realfork_n(void) { return tg_realfork_enabled() ? s_rf_n : 0; }

int tg_realfork_get(int i, int *F, int *len, int *lanes_a, int *lanes_b,
                    double *sep)
{
    if (!tg_realfork_enabled() || i < 0 || i >= s_rf_n) return 0;
    if (F)       *F       = s_rf[i].F;
    if (len)     *len     = s_rf[i].len;
    if (lanes_a) *lanes_a = s_rf[i].lanes_a;
    if (lanes_b) *lanes_b = s_rf[i].lanes_b;
    /* A divided avenue reads as an AVENUE (divider + island); a parallel road
     * is a wide split, so it keeps the ground between the two roads. */
    if (sep)     *sep     = (s_rf[i].src == TG_RF_SRC_AVENUE)
                                ? TD5_TG_BRANCH_SEP_MIN : 1.0;
    return 1;
}

const char *tg_realfork_name(int i)
{
    return (i >= 0 && i < s_rf_n) ? s_rf[i].name : "";
}

/* Median width (world units, >= 0) at corridor step k, the value the corridor's
 * shift is built from. Clamped so a stray k can never read past the table. */
double tg_realfork_med(int i, int k)
{
    if (i < 0 || i >= s_rf_n) return 0.0;
    if (k < 0) k = 0;
    if (k > s_rf[i].len) k = s_rf[i].len;
    return s_rf[i].med[k];
}

/* ---- the node window, read by tg_geo_walk -------------------------------- */

int tg_realfork_lanes_override(int node, int raw_lanes)
{
    if (!tg_realfork_enabled() || node < 0 || node >= s_rn) return raw_lanes;
    return s_node_ovr[node] > 0 ? s_node_ovr[node] : raw_lanes;
}

/* Move node `node` toward its corridor by half the width the walk added there, so
 * the route carriageway's own edge stays where the map has it. Returns the delta
 * applied (signed, +t = (tz,-tx), so negative: the corridor is on the -t side). */
double tg_realfork_node_adjust(int node, double *x, double *z)
{
    double tx, tz, len, delta;
    int fi;
    if (!tg_realfork_enabled() || node < 0 || node >= s_rn) return 0.0;
    fi = s_node_fork[node];
    s_node_dt[node] = 0.0;
    if (fi < 0 || s_node_extra[node] <= 0.0) return 0.0;
    delta = s_node_extra[node] * (double)TD5_TG_LANE_WIDTH * 0.5;
    {
        const int p = (node > 0) ? node - 1 : node;
        const int q = (node + 1 < s_rn) ? node + 1 : node;
        tx = s_rx[q] - s_rx[p];
        tz = s_rz[q] - s_rz[p];
    }
    len = sqrt(tx * tx + tz * tz);
    if (len < 1.0) return 0.0;
    tx /= len; tz /= len;
    /* The corridor is on the -t side (the only side the shipped fork has), and
     * +t is (tz, -tx): the node moves toward -t. */
    *x += -delta * tz;
    *z +=  delta * tx;
    s_node_dt[node] = -delta;
    return -delta;
}

/* The width the walk pushes for node `node`, world units: the route's own lanes
 * plus the (fractional) lanes of the corridor's width ramped in over the taper.
 * `lanes` is what the walk settled on after its one-lane-per-seam rule. */
double tg_realfork_node_width(int node, int lanes, double lane_w)
{
    if (!tg_realfork_enabled() || node < 0 || node >= s_rn || s_node_fork[node] < 0
        || s_node_extra[node] <= 0.0)
        return (double)lanes * lane_w;
    /* [ROUND 1015 A] the route's own lanes AT THIS NODE plus what the ramp adds: the
     * same lanes_a on every node of a round-1013 window, more on the 3-lane approach. */
    return ((double)s_rl[node] + s_node_extra[node]) * lane_w;
}

double tg_realfork_node_delta(int node)
{
    if (!tg_realfork_enabled() || node < 0 || node >= s_rn) return 0.0;
    return s_node_dt[node];
}

/* ---- candidate generation ------------------------------------------------ */

static int s_rf_quiet;      /* [1015 A] silent while a start slides along the road */
static void rf_note(const char *what, int src, const char *name, int a, int b,
                    const char *why)
{
    if (!s_rf_diag || s_rf_quiet) return;
    TD5_LOG_I(LOG_TAG, "trackgen: [REAL FORK] %s %s \"%s\" spans %d..%d: %s",
              what, src == TG_RF_SRC_AVENUE ? "avenue"
                    : src == TG_RF_SRC_PARALLEL ? "parallel" : "plaza",
              name ? name : "", a, b, why);
}

static double rf_lane_w(void) { return (double)TD5_TG_LANE_WIDTH; }

/* THE ONE PLACE that says where real forks must be over: the FINISH line. Every
 * fork window ends before it, so nothing is built in the run-off past it (round
 * 1013 F3 puts ~100 spans of real road there, itself a divided avenue).
 *
 * Pure on purpose: tg_finish_span() walks back out of the fork and tunnel tables,
 * and those do not exist yet when the forks are chosen (this runs before the
 * walk), so asking it from here would be stale on a second generation. This is the
 * same `route spans - run-off` arithmetic without the walk-back. A finish that is
 * fixed by the route itself (F3: ROUTE.JSON finish_span, td5_geo_route_finish_span)
 * replaces the body of THIS function and nothing else. -1 = no finish known. */
int tg_realfork_finish_span(void)
{
    const int nsp = s_rn - 1;
    int runoff = td5_env_int("TD5RE_AUTOTRACK_RUNOFF", TD5_TG_RUNOFF_SPANS, 0, 600);
    const int lo = TD5_TG_GRID_SPAN + 60;      /* shortest race worth having */
    /* [ROUND 1013 integ] The route's own finish (F3), same acceptance test as
     * tg_finish_span(): inside the race and short of the ring-3 backstop. */
    const int geo_fs = td5_geo_route_finish_span();
    if (nsp <= lo) return -1;
    if (geo_fs > lo && geo_fs < nsp - 3) return geo_fs;
    if (nsp - runoff <= lo) runoff = nsp - lo;
    return nsp - runoff;
}

/* The last span a fork WINDOW (taper tail included) may touch: before the finish,
 * and inside the 24-span ring tail tg_fork_place keeps. */
static int rf_window_limit(void)
{
    const int nsp = s_rn - 1;
    int lim = nsp - 25 + 2 + s_rf_taper;
    const int fin = tg_realfork_finish_span();
    if (fin > 0 && fin - TG_RF_FINISH_GAP < lim) lim = fin - TG_RF_FINISH_GAP;
    return lim;
}

/* [ROUND 1015 A] FIT THE NODE WINDOW to the room the route has.
 *
 * Round 1013 wanted the full taper (16 nodes) at both ends and a route lane count
 * that never changes across the whole window, and refused the candidate otherwise.
 * On Mariano's La Plata route that refused the fork he asked for FIRST: "as soon as I
 * get into the avenue I should be able to drive on both parts". Diagonal 73's
 * opposite carriageway exists from span 47, 23 spans after the start grid, and the
 * route carries 3 lanes up to span 38 (Calle 40) and 2 after it, so the window of a
 * fork at F=47 (nodes 23..) runs into the grid and across the lane change.
 *
 * The rule is now what the road actually needs, per end:
 *   FULL WIDTH F-8 .. R+2   the route's lane count must equal lanes(A) (as before).
 *   RAMP in front / behind  as long as the room allows, at most the full taper, at
 *                           least TG_RF_TAPER_MIN. It may not start inside the start
 *                           grid, may not run past the window limit (the finish), and
 *                           may cross route nodes carrying MORE lanes than lanes(A)
 *                           (up to lanes(A)+lanes(B)): the ramp then grows the road
 *                           from the lanes it already has to the fork's width, see the
 *                           node width in tg_realfork_build. A node with FEWER lanes than
 *                           lanes(A) ends the ramp there.
 * Returns 0 (candidate refused, reason logged) when the full-width window itself does
 * not fit or a ramp is under TG_RF_TAPER_MIN. TD5RE_GEO_FORK_TAPER_FIT=0 restores the
 * round-1013 rule exactly. */
static int rf_fit_window(RfCand *c)
{
    const int fit = td5_env_flag_on("TD5RE_GEO_FORK_TAPER_FIT");
    const int f0 = c->F - TG_RF_WIDEN, f1 = c->R + 2;     /* the full-width window */
    int i, lo, hi, tin, tout;

    if (!fit) {
        c->taper_in = c->taper_out = s_rf_taper;
        c->i0    = c->F - TG_RF_WIDEN - s_rf_taper;
        c->w_end = c->R + 2 + s_rf_taper;
        if (c->i0 < TD5_TG_GRID_SPAN + 2) {
            rf_note("REJECT", c->src, c->name, c->F, c->R, "inside the start grid");
            return 0;
        }
        if (c->w_end > rf_window_limit()) {
            rf_note("REJECT", c->src, c->name, c->F, c->R,
                    "past the finish line (or inside the ring tail)");
            return 0;
        }
        lo = c->i0 - 1; if (lo < 0) lo = 0;
        hi = c->w_end + 1; if (hi > s_rn - 1) hi = s_rn - 1;
        for (i = lo; i <= hi; i++)
            if (s_rl[i] != c->lanes_a) {
                rf_note("REJECT", c->src, c->name, c->F, c->R,
                        "the route's own lane count changes inside the window");
                return 0;
            }
        return 1;
    }

    /* the full-width window itself: clear of the grid, short of the finish, constant lanes */
    if (f0 < TD5_TG_GRID_SPAN + 2 + TG_RF_TAPER_MIN) {
        rf_note("REJECT", c->src, c->name, c->F, c->R, "inside the start grid");
        return 0;
    }
    if (f1 + TG_RF_TAPER_MIN > rf_window_limit() || c->R + 25 > s_rn - 1) {
        rf_note("REJECT", c->src, c->name, c->F, c->R,
                "past the finish line (or inside the ring tail)");
        return 0;
    }
    /* the fork BODY F .. R+? keeps the route's own count exactly; the approach in front
     * of F (the 8 uniform nodes) and the 2 behind R may carry more lanes than lanes(A), up
     * to the fork's total, exactly as a ramp node may (a 3-lane junction flare in front of
     * a 2+2 avenue): the width there is made up to lanes(A)+lanes(B) in tg_realfork_build. */
    lo = c->F; if (lo < 0) lo = 0;
    hi = c->R + 1; if (hi > s_rn - 1) hi = s_rn - 1;
    for (i = lo; i <= hi; i++)
        if (s_rl[i] != c->lanes_a) {
            rf_note("REJECT", c->src, c->name, c->F, c->R,
                    "the route's own lane count changes inside the window");
            return 0;
        }
    for (i = f0 < 0 ? 0 : f0; i <= f1 && i < s_rn; i++) {
        if (i >= c->F && i <= c->R + 1) continue;
        if (s_rl[i] < c->lanes_a || s_rl[i] > c->lanes_a + c->lanes_b) {
            rf_note("REJECT", c->src, c->name, c->F, c->R,
                    "the route's own lane count changes inside the window");
            return 0;
        }
    }
    /* the ramps: walk outward while the room and the route allow */
    for (tin = 0; tin < s_rf_taper; tin++) {
        const int n = f0 - 1 - tin;
        if (n < TD5_TG_GRID_SPAN + 2) break;
        if (s_rl[n] < c->lanes_a || s_rl[n] > c->lanes_a + c->lanes_b) break;
    }
    for (tout = 0; tout < s_rf_taper; tout++) {
        const int n = f1 + 1 + tout;
        if (n > rf_window_limit() || n >= s_rn - 1) break;
        if (s_rl[n] < c->lanes_a || s_rl[n] > c->lanes_a + c->lanes_b) break;
    }
    if (tin < TG_RF_TAPER_MIN || tout < TG_RF_TAPER_MIN) {
        rf_note("REJECT", c->src, c->name, c->F, c->R,
                tin < TG_RF_TAPER_MIN ? "no room for the lane ramp in front of it "
                                        "(start grid or a narrower road)"
                                      : "no room for the lane ramp behind it "
                                        "(finish or a narrower road)");
        return 0;
    }
    c->taper_in  = tin;  c->taper_out = tout;
    c->i0    = f0 - tin;
    c->w_end = f1 + tout;
    return 1;
}

/* Fill the derived window of a candidate and reject it when the route cannot
 * carry it. Every reason is logged under TD5RE_GEO_FORK_DIAG so "why is there no
 * fork there" is answerable from race.log, not from a guess. */
static int rf_validate(RfCand *c)
{
    int i;
    double worst = 0.0;

    c->len = c->R - c->F - 1;
    if (c->lanes_b < 1) c->lanes_b = 1;
    if (c->lanes_a < 1) return 0;
    if (c->lanes_a + c->lanes_b > TG_RF_LANES_TOTAL) {
        rf_note("REJECT", c->src, c->name, c->F, c->R,
                "lanes(A)+lanes(B) over the 8-lane rail range");
        return 0;
    }
    if (c->len < TG_RF_MINLEN || c->len > TG_RF_MAXLEN) return 0;
    /* The full width holds F-8 .. R+2 (the engine's own uniform window); the taper
     * runs s_rf_taper nodes in front of it and behind it. */
    if (!rf_fit_window(c)) return 0;
    /* A fork on a sharp bend folds its shifted carriageways (the R6 "span 570"
     * report): the same per-span heading cap the placement loop logs. */
    for (i = c->F - TG_RF_WIDEN; i <= c->R + 2 && i + 2 < s_rn; i++) {
        double ax, az, bx, bz, la, lb, d;
        if (i < 0) continue;
        ax = s_rx[i + 1] - s_rx[i];     az = s_rz[i + 1] - s_rz[i];
        bx = s_rx[i + 2] - s_rx[i + 1]; bz = s_rz[i + 2] - s_rz[i + 1];
        la = sqrt(ax * ax + az * az); lb = sqrt(bx * bx + bz * bz);
        if (la < 1.0 || lb < 1.0) continue;
        d = (ax * bx + az * bz) / (la * lb);
        if (d > 1.0) d = 1.0; else if (d < -1.0) d = -1.0;
        d = acos(d);
        if (d > worst) worst = d;
    }
    if (worst > TD5_TG_FORK_MAX_TURN) {
        char why[96];
        snprintf(why, sizeof why, "bend of %.3f rad/span in the window "
                 "(cap %.3f)", worst, TD5_TG_FORK_MAX_TURN);
        rf_note("REJECT", c->src, c->name, c->F, c->R, why);
        return 0;
    }
    return 1;
}

static RfCand *rf_new_cand(int src, const char *name, int F, int R,
                           int lanes_a, int lanes_b)
{
    RfCand *c;
    if (s_ncand >= TG_RF_MAXCAND) return NULL;
    c = &s_cand[s_ncand];
    memset(c->off, 0, sizeof c->off);
    c->src = src; c->F = F; c->R = R;
    c->lanes_a = lanes_a; c->lanes_b = lanes_b;
    snprintf(c->name, sizeof c->name, "%s", name ? name : "");
    c->weight = 0.0;
    return c;
}

/* [ROUND 1015 A] One avenue candidate for the span range F..R: reads the real offsets from
 * the sidecar, makes the candidate and validates it. Returns 1 when it was kept. */
static int rf_avenue_try(const char *name, int F, int R, int merge, int *oom)
{
    int la = 0, lb_n[10], lb_best = 2, nb = 0, i, ok = 1;
    RfCand *c;
    memset(lb_n, 0, sizeof lb_n);
    for (i = F; i <= R && ok; i++) {
        double off = 0.0; int lb = 2, op = 0;
        if (!td5_geo_avenue_at(i, &off, &lb, &op)) {
            /* A hole in the sidecar inside the run: the real
             * carriageway is not known there, so no fork over it. */
            ok = 0; break;
        }
        if (off > 0.0) { ok = 0; break; }   /* +t side: see rf_validate */
        if (lb >= 1 && lb < 10) lb_n[lb]++;
    }
    if (!ok) {
        rf_note("REJECT", TG_RF_SRC_AVENUE, name, F, R,
                "the opposite carriageway is missing or on the +t "
                "side (left corridors are parked)");
        return 0;
    }
    for (i = 1; i < 10; i++)
        if (lb_n[i] > nb) { nb = lb_n[i]; lb_best = i; }
    la = (F >= 0 && F < s_rn) ? s_rl[F] : 2;
    c = rf_new_cand(TG_RF_SRC_AVENUE, name, F, R, la, lb_best);
    if (!c) { *oom = 1; return 0; }
    for (i = F; i <= R; i++) {
        double off = 0.0;
        td5_geo_avenue_at(i, &off, NULL, NULL);
        c->off[i - F] = off;
    }
    if (!rf_validate(c)) return 0;
    /* [ROUND 1014 A] Weighted by its FULL length unless the merge
     * knob is off. The old cap of 90 made two 55-span blocks worth
     * more than the 194-span run they are the halves of, so the
     * selector kept alternate blocks (full-width windows of
     * neighbouring blocks overlap across the 2-span median
     * opening) and the other blocks were left as a scenery
     * carriageway that looks drivable and is not. */
    c->weight = (double)(merge ? c->len : (c->len > 90 ? 90 : c->len)) * 1.05;
    s_ncand++;
    return 1;
}

/* SOURCE 1: the divided avenues of AVENUES.JSON.
 *
 * Gates are the places a fork can start or end: the two ends of the real run,
 * and each cross-street OPENING in the median. A fork runs from one gate to a
 * later one, so its mouths are real junctions and its length is whatever the
 * map's own blocks make it. Every pair of gates far enough apart is a candidate;
 * the selector below keeps the combination that drives the most corridor. */
static void rf_gen_avenue(void)
{
    int a;
    /* [ROUND 1014 A] TD5RE_GEO_FORK_MERGE=0 restores the round-1013 rule: one
     * block per candidate, weight capped at 90. */
    const int merge = td5_env_flag_on("TD5RE_GEO_FORK_MERGE");
    if (!td5_env_flag_on("TD5RE_GEO_FORK_AVENUE")) return;
    if (tg_geo_avenue_n() < 1) return;

    for (a = 0; a < td5_geo_avenues_count(); a++) {
        int s0 = 0, s1 = 0, s;
        const char *name = "";
        int gate_a[64], gate_b[64], ng = 0, g, h;

        if (!td5_geo_avenue_range(a, &s0, &s1, &name)) continue;
        if (s1 - s0 < 1) continue;

        /* END gate at the start of the run, one gate per opening run, END gate
         * at the end. An opening run is a maximal stretch of `open` spans. */
        gate_a[ng] = s0; gate_b[ng] = s0; ng++;
        for (s = s0; s <= s1 && ng < 62; ) {
            int op = 0;
            if (td5_geo_avenue_at(s, NULL, NULL, &op) && op) {
                int e = s, op2 = 0;
                while (e + 1 <= s1 && td5_geo_avenue_at(e + 1, NULL, NULL, &op2) && op2) e++;
                /* The run's own ends are gates already. */
                if (s > s0 && e < s1) { gate_a[ng] = s; gate_b[ng] = e; ng++; }
                s = e + 1;
            } else s++;
        }
        gate_a[ng] = s1; gate_b[ng] = s1; ng++;

        for (g = 0; g < ng; g++)
            for (h = g + 1; h < ng; h++) {
                /* [ROUND 1014 A] The earliest F the start grid allows is the one
                 * whose WINDOW (F - widen - taper) clears the grid, not the old
                 * GRID_SPAN + 12 -- a clamp that always failed rf_validate's
                 * "inside the start grid" test, so a run that begins in the grid
                 * (Diagonal 73 at span 24) lost its first 34 spans of corridor. */
                /* [ROUND 1015 A] ... and with the ramp fitted to the room the grid
                 * leaves (rf_fit_window), the earliest F is the one whose SHORTEST
                 * ramp clears the grid: an avenue that opens at span 47 gets its
                 * fork at 47, not at 50 -- or at 94 when the 3-lane approach made the
                 * round-1014 window refuse every earlier start. */
                const int fit = td5_env_flag_on("TD5RE_GEO_FORK_TAPER_FIT");
                const int Fmin = merge ? TD5_TG_GRID_SPAN + 2 + TG_RF_WIDEN
                                         + (fit ? TG_RF_TAPER_MIN : s_rf_taper)
                                       : TD5_TG_GRID_SPAN + 12;
                const int F = (gate_a[g] > Fmin) ? gate_a[g] : Fmin;
                const int R = gate_b[h];
                int oom = 0;
                /* BETWEEN consecutive gates: a fork is one block of the avenue,
                 * from one real opening (or run end) to the next. A block too
                 * short to be worth a fork is merged with the next one; the
                 * first span of gates that is long enough is the candidate, and
                 * nothing longer is offered -- a corridor that skips an opening
                 * is a worse picture of the map than two that each end at one. */
                if (R - F - 1 < TG_RF_MINLEN) continue;
                if (R - F - 1 > TG_RF_MAXLEN) break;
                if (!rf_avenue_try(name, F, R, merge, &oom) && !oom && g == 0 && fit) {
                    /* [ROUND 1015 A] THE START SLIDES. The first gate is where the avenue's
                     * opposite carriageway begins, and the road at that point is often
                     * still turning onto the avenue (Diagonal 73 leaves Calle 40 at spans
                     * 35..38; Avenida 60 and Avenida 7 leave a plaza ring), so the window of
                     * a fork there holds a bend or a lane change and is refused. The fork
                     * starts at the first span, at or after the gate, whose window is clean:
                     * the avenue's opposite carriageway is a scenery road up to it and a
                     * driveable one from it. */
                    int slide;
                    s_rf_quiet = 1;
                    for (slide = 1; slide <= TG_RF_START_SLIDE && !oom; slide++) {
                        if (R - (F + slide) - 1 < TG_RF_MINLEN) break;
                        if (rf_avenue_try(name, F + slide, R, merge, &oom)) {
                            s_rf_quiet = 0;
                            rf_note("SLID", TG_RF_SRC_AVENUE, name, F, R,
                                    "the start of the fork moved down the avenue past "
                                    "a bend or a lane change");
                            break;
                        }
                    }
                    s_rf_quiet = 0;
                }
                if (oom) return;
                if (!merge) break;      /* the old rule: the nearest long-enough gate only */
            }
    }
}

/* ---- SOURCE 3: a real road close to and roughly parallel with the route ------
 *
 * "by default every road that is too close to the main road should be treated a
 * driveable fork". Per route node the nearest way within TD5RE_GEO_FORK_PAR_MAX_M
 * that runs parallel (or anti-parallel) to the route is found; a run of such nodes
 * is a parallel road, and its lateral profile is MEASURED, node by node, exactly as
 * AVENUES.JSON's is for an avenue.
 *
 * The two ends of a fork must be REAL JUNCTIONS. OSM splits a street into one way
 * per junction, so a place where the matched way changes along a run is a junction
 * by construction, and so are the run's two ends; each is kept as a gate only if a
 * real way reaches the route there (a shared OSM vertex with a way that touches
 * the route, or the parallel road merging into the route itself). Forks are then
 * the blocks between consecutive gates, as for an avenue.
 *
 * MEASURED on Mariano's route (scratchpad par_proto.py, 952 nodes x 2291 ways):
 * with 25 m / 20 deg every run is the opposite carriageway of Diagonal 73 or
 * Avenida 13, and nothing else -- the route crosses a city grid whose parallel
 * streets are ~100 m apart. Widening to 40 m / 20 deg adds one 12-span piece of
 * Calle 14 (under the 24-span floor), and 25 m / 35 deg one 13-span piece. So the
 * thresholds are not what limits it; this route has no other parallel road. The
 * source still matters: it is what makes the second Diagonal 73 run (nodes
 * 340..589, absent from AVENUES.JSON) driveable before the avenue detector learns
 * it, and what covers a place with a service road beside the route. */
#define TG_RF_PAR_MIN_GAP_M   0.5      /* median between the two carriageway edges */
#define TG_RF_PAR_TOUCH_U     2150.0   /* 5 m: a way "reaches the route"           */
#define TG_RF_UPM             430.0

typedef struct { int x, z, way; } RfVtx;
static RfVtx *s_vt;
static int    s_vtn;

static int rf_vtx_cmp(const void *pa, const void *pb)
{
    const RfVtx *a = (const RfVtx *)pa, *b = (const RfVtx *)pb;
    if (a->x != b->x) return (a->x < b->x) ? -1 : 1;
    if (a->z != b->z) return (a->z < b->z) ? -1 : 1;
    return a->way - b->way;
}

static void rf_vtx_build(void)
{
    int w, k, n = 0, cap = td5_geo_roads_points();
    free(s_vt); s_vt = NULL; s_vtn = 0;
    if (cap < 1) return;
    s_vt = (RfVtx *)malloc((size_t)cap * sizeof *s_vt);
    if (!s_vt) return;
    for (w = 0; w < td5_geo_roads_count() && n < cap; w++) {
        const TD5_GeoRoad *r = td5_geo_roads_get(w);
        if (!r) continue;
        for (k = 0; k < r->count && n < cap; k++) {
            double x = 0.0, z = 0.0;
            if (!td5_geo_roads_point(r, k, &x, &z)) continue;
            s_vt[n].x = (int)floor(x + 0.5);
            s_vt[n].z = (int)floor(z + 0.5);
            s_vt[n].way = w;
            n++;
        }
    }
    s_vtn = n;
    qsort(s_vt, (size_t)s_vtn, sizeof *s_vt, rf_vtx_cmp);
}

static int rf_vtx_lower(int x, int z)
{
    int lo = 0, hi = s_vtn;
    while (lo < hi) {
        const int mid = lo + (hi - lo) / 2;
        if (s_vt[mid].x < x || (s_vt[mid].x == x && s_vt[mid].z < z)) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

/* Does way `c` reach the route near node `ni` (any vertex within TOUCH of a
 * route node in [ni-20, ni+20])? */
static int rf_way_reaches_route(const TD5_GeoRoad *c, int ni)
{
    int k, q;
    const int lo = (ni - 20 < 0) ? 0 : ni - 20;
    const int hi = (ni + 20 > s_rn - 1) ? s_rn - 1 : ni + 20;
    for (k = 0; k < c->count; k++) {
        double x = 0.0, z = 0.0;
        if (!td5_geo_roads_point(c, k, &x, &z)) continue;
        for (q = lo; q <= hi; q++) {
            const double dx = x - s_rx[q], dz = z - s_rz[q];
            if (dx * dx + dz * dz <= TG_RF_PAR_TOUCH_U * TG_RF_PAR_TOUCH_U) return 1;
        }
    }
    return 0;
}

static int rf_cross_cut_at(int g, const int *chain, int nchain);

/* Is there a REAL junction between way `j` and the route within +/-6 nodes of
 * node g? Either the parallel road merges into the route, it shares an OSM vertex
 * with another way that reaches the route (a slip road), or a cross street cuts
 * between the two (rf_cross_cut_at). */
static int rf_junction_at(int j, int g, const int *chain, int nchain)
{
    const TD5_GeoRoad *B = td5_geo_roads_get(j);
    int k, q;
    const int lo = (g - 6 < 0) ? 0 : g - 6;
    const int hi = (g + 6 > s_rn - 1) ? s_rn - 1 : g + 6;
    if (!B) return 0;
    for (k = 0; k < B->count; k++) {
        double x = 0.0, z = 0.0;
        int near_g = 0, idx;
        if (!td5_geo_roads_point(B, k, &x, &z)) continue;
        /* near this node along the route: within a block's lateral reach of a
         * node in the window */
        for (q = lo; q <= hi; q++) {
            const double dx = x - s_rx[q], dz = z - s_rz[q];
            const double d = sqrt(dx * dx + dz * dz);
            if (d <= 16.0 * TG_RF_UPM) near_g = 1;
            if (d <= TG_RF_PAR_TOUCH_U) return 1;         /* B merges into the route */
        }
        if (!near_g) continue;
        idx = rf_vtx_lower((int)floor(x + 0.5), (int)floor(z + 0.5));
        for (; idx < s_vtn &&
               s_vt[idx].x == (int)floor(x + 0.5) &&
               s_vt[idx].z == (int)floor(z + 0.5); idx++) {
            const int c = s_vt[idx].way;
            int skip = (c == j), m;
            for (m = 0; m < nchain && !skip; m++) if (chain[m] == c) skip = 1;
            if (skip) continue;
            {
                const TD5_GeoRoad *C = td5_geo_roads_get(c);
                if (C && rf_way_reaches_route(C, g)) return 1;
            }
        }
    }
    return rf_cross_cut_at(g, chain, nchain);
}

static int    s_pw[TG_RF_NODES];       /* best parallel way at node i, -1 none */
static double s_plat[TG_RF_NODES];     /* its signed lateral (+t), world units  */
static int    s_plb[TG_RF_NODES];      /* its lane count                        */

static double rf_cross(double ax, double az, double bx, double bz, double cx, double cz)
{
    return (bx - ax) * (cz - az) - (bz - az) * (cx - ax);
}

/* Do segments AB and CD intersect (proper or touching)? */
static int rf_seg_hit(double ax, double az, double bx, double bz,
                      double cx, double cz, double dx, double dz)
{
    const double d1 = rf_cross(ax, az, bx, bz, cx, cz);
    const double d2 = rf_cross(ax, az, bx, bz, dx, dz);
    const double d3 = rf_cross(cx, cz, dx, dz, ax, az);
    const double d4 = rf_cross(cx, cz, dx, dz, bx, bz);
    return ((d1 > 0.0) != (d2 > 0.0)) && ((d3 > 0.0) != (d4 > 0.0));
}

/* A CROSS STREET cuts between the route and the parallel road within +/-4 nodes
 * of node g: a way (not one of the parallel road's own pieces) with a segment that
 * crosses the line from a route node to the parallel road's matched point there,
 * and that runs at 45 deg or more to the route. This is the geometric junction:
 * La Plata's divided avenues are NOT joined to their cross streets by a shared OSM
 * vertex (measured: the vertices at the way boundaries of Diagonal 73's carriageway
 * are shared only with other pieces of the same carriageway), so a vertex test
 * alone finds no junction on an avenue that has twelve of them. */
static int rf_cross_cut_at(int g, const int *chain, int nchain)
{
    const int nways = td5_geo_roads_count();
    int gg, j, k, m;
    for (gg = g - 4; gg <= g + 4; gg++) {
        double tx, tz, tl, px, pz, qx, qz, lox, hix, loz, hiz;
        const int p = (gg > 0) ? gg - 1 : gg, q = (gg + 1 < s_rn) ? gg + 1 : gg;
        if (gg < 0 || gg >= s_rn || s_pw[gg] < 0) continue;
        tx = s_rx[q] - s_rx[p]; tz = s_rz[q] - s_rz[p];
        tl = sqrt(tx * tx + tz * tz);
        if (tl < 1.0) continue;
        tx /= tl; tz /= tl;
        px = s_rx[gg]; pz = s_rz[gg];
        qx = px + s_plat[gg] * tz; qz = pz - s_plat[gg] * tx;
        lox = (px < qx ? px : qx) - 1500.0; hix = (px > qx ? px : qx) + 1500.0;
        loz = (pz < qz ? pz : qz) - 1500.0; hiz = (pz > qz ? pz : qz) + 1500.0;
        for (j = 0; j < nways; j++) {
            const TD5_GeoRoad *C = td5_geo_roads_get(j);
            int skip = 0;
            if (!C || C->count < 2) continue;
            if (C->maxx < lox || C->minx > hix || C->maxz < loz || C->minz > hiz) continue;
            for (m = 0; m < nchain && !skip; m++) if (chain[m] == j) skip = 1;
            if (skip) continue;
            for (k = 0; k + 1 < C->count; k++) {
                double ax = 0.0, az = 0.0, bx = 0.0, bz = 0.0, dl, ux, uz;
                if (!td5_geo_roads_point(C, k, &ax, &az) ||
                    !td5_geo_roads_point(C, k + 1, &bx, &bz)) continue;
                dl = sqrt((bx - ax) * (bx - ax) + (bz - az) * (bz - az));
                if (dl < 1.0) continue;
                ux = (bx - ax) / dl; uz = (bz - az) / dl;
                if (fabs(ux * tx + uz * tz) > 0.7071) continue;   /* not across */
                if (rf_seg_hit(px, pz, qx, qz, ax, az, bx, bz)) return 1;
            }
        }
    }
    return 0;
}

static void rf_par_scan(double dmax, double cos_ang)
{
    const double lw = rf_lane_w();
    const int nways = td5_geo_roads_count();
    int i, j, k;

    for (i = 0; i < s_rn; i++) {
        double tx, tz, tl;
        double best_abs = 1e30;
        const int p = (i > 0) ? i - 1 : i, q = (i + 1 < s_rn) ? i + 1 : i;
        s_pw[i] = -1; s_plat[i] = 0.0; s_plb[i] = 0;
        tx = s_rx[q] - s_rx[p]; tz = s_rz[q] - s_rz[p];
        tl = sqrt(tx * tx + tz * tz);
        if (tl < 1.0) continue;
        tx /= tl; tz /= tl;
        for (j = 0; j < nways; j++) {
            const TD5_GeoRoad *r = td5_geo_roads_get(j);
            double bd = 1e30, bqx = 0.0, bqz = 0.0, bux = 0.0, buz = 0.0;
            double lat, gap, dotp, alat;
            int lb;
            if (!r || r->count < 2) continue;
            if (r->roundabout || r->bridge || r->tunnel) continue;
            if (s_rx[i] < r->minx - dmax || s_rx[i] > r->maxx + dmax ||
                s_rz[i] < r->minz - dmax || s_rz[i] > r->maxz + dmax) continue;
            for (k = 0; k + 1 < r->count; k++) {
                double ax = 0.0, az = 0.0, bx = 0.0, bz = 0.0, dx, dz, l2, t, qx, qz, d;
                if (!td5_geo_roads_point(r, k, &ax, &az) ||
                    !td5_geo_roads_point(r, k + 1, &bx, &bz)) continue;
                dx = bx - ax; dz = bz - az; l2 = dx * dx + dz * dz;
                t = (l2 > 0.0) ? ((s_rx[i] - ax) * dx + (s_rz[i] - az) * dz) / l2 : 0.0;
                if (t < 0.0) t = 0.0; else if (t > 1.0) t = 1.0;
                qx = ax + t * dx; qz = az + t * dz;
                d = sqrt((s_rx[i] - qx) * (s_rx[i] - qx) + (s_rz[i] - qz) * (s_rz[i] - qz));
                if (d < bd) {
                    const double sl = sqrt(l2);
                    bd = d; bqx = qx; bqz = qz;
                    bux = (sl > 0.0) ? dx / sl : 0.0; buz = (sl > 0.0) ? dz / sl : 0.0;
                }
            }
            if (bd > dmax) continue;
            lat  = (bqx - s_rx[i]) * tz + (bqz - s_rz[i]) * (-tx);   /* +t = (tz,-tx) */
            alat = (lat < 0.0) ? -lat : lat;
            dotp = bux * tx + buz * tz;
            if ((dotp < 0.0 ? -dotp : dotp) < cos_ang) continue;
            lb = r->lanes < 1 ? 1 : r->lanes;
            gap = alat - (double)(s_rl[i] + lb) * lw * 0.5;
            if (gap < TG_RF_PAR_MIN_GAP_M * TG_RF_UPM) continue;    /* the same road */
            if (alat < best_abs) {
                best_abs = alat;
                s_pw[i] = j; s_plat[i] = lat; s_plb[i] = lb;
            }
        }
    }
}

static int rf_overlaps_avenue(int F, int R)
{
    int c, over = 0;
    for (c = 0; c < s_ncand; c++) {
        int lo, hi;
        if (s_cand[c].src != TG_RF_SRC_AVENUE) continue;
        lo = (s_cand[c].F > F) ? s_cand[c].F : F;
        hi = (s_cand[c].R < R) ? s_cand[c].R : R;
        if (hi > lo) over += hi - lo;
    }
    return over * 2 > (R - F);
}

static const char *rf_way_name(int w)
{
    const TD5_GeoRoad *r = (w >= 0) ? td5_geo_roads_get(w) : NULL;
    return r ? td5_geo_roads_name(r) : "";
}

static void rf_gen_parallel(void)
{
    const double dmax = (double)td5_env_float("TD5RE_GEO_FORK_PAR_MAX_M", 25.0f, 8.0f, 80.0f) * TG_RF_UPM;
    const double ang  = (double)td5_env_float("TD5RE_GEO_FORK_PAR_ANG_DEG", 20.0f, 3.0f, 60.0f);
    const double cosang = cos(ang * TD5_TG_PI / 180.0);
    int i, a;

    if (!td5_env_flag_on("TD5RE_GEO_FORK_PARALLEL")) return;
    if (!td5_geo_roads_sync(td5_geo_place_slug())) return;
    rf_vtx_build();
    rf_par_scan(dmax, cosang);

    /* bridge gaps of <= 2 nodes (a parallel road's matched vertex can jump) */
    for (i = 1; i + 3 < s_rn; i++) {
        int g;
        if (s_pw[i] >= 0 || s_pw[i - 1] < 0) continue;
        for (g = 1; g <= 2; g++)
            if (s_pw[i + g] >= 0 && s_pw[i - 1] == s_pw[i + g]) {
                int m;
                for (m = 0; m < g; m++) {
                    const double f = (double)(m + 1) / (double)(g + 1);
                    s_pw[i + m] = s_pw[i - 1];
                    s_plat[i + m] = s_plat[i - 1] + f * (s_plat[i + g] - s_plat[i - 1]);
                    s_plb[i + m] = s_plb[i - 1];
                }
                break;
            }
    }

    for (a = 0; a < s_rn; ) {
        int b, gates[64], ng = 0, g, h, chain[32], nchain = 0, q;
        if (s_pw[a] < 0) { a++; continue; }
        b = a;
        while (b + 1 < s_rn && s_pw[b + 1] >= 0) b++;
        if (b - a + 1 < TG_RF_MINLEN) { a = b + 1; continue; }
        /* the ways of this run */
        for (q = a; q <= b; q++) {
            int m, have = 0;
            for (m = 0; m < nchain; m++) if (chain[m] == s_pw[q]) { have = 1; break; }
            if (!have && nchain < 32) chain[nchain++] = s_pw[q];
        }
        for (q = a; q <= b && ng < 62; q++) {
            const int edge = (q == a || q == b || s_pw[q] != s_pw[q - 1]);
            if (!edge) continue;
            if (rf_junction_at(s_pw[q], q, chain, nchain)) gates[ng++] = q;
            else if (s_rf_diag)
                TD5_LOG_I(LOG_TAG, "trackgen: [REAL FORK] parallel \"%s\" node %d: "
                          "no real junction reaches the route here -- not a gate",
                          rf_way_name(s_pw[q]), q);
        }
        for (g = 0; g < ng; g++)
            for (h = g + 1; h < ng; h++) {
                const int F = (gates[g] > TD5_TG_GRID_SPAN + 12) ? gates[g]
                                                                 : TD5_TG_GRID_SPAN + 12;
                const int R = gates[h];
                int lb_n[13], lb_best = 2, nb = 0, k;
                RfCand *c;
                if (R - F - 1 < TG_RF_MINLEN) continue;
                if (R - F - 1 > TG_RF_MAXLEN) break;
                if (rf_overlaps_avenue(F, R)) {
                    rf_note("SKIP", TG_RF_SRC_PARALLEL, rf_way_name(s_pw[(F + R) / 2]),
                            F, R, "already an avenue fork (the avenue source wins)");
                    break;
                }
                memset(lb_n, 0, sizeof lb_n);
                for (k = F; k <= R; k++) {
                    if (s_pw[k] < 0) { nb = -1; break; }
                    if (s_plat[k] > 0.0) { nb = -2; break; }     /* +t side */
                    if (s_plb[k] >= 1 && s_plb[k] < 13) lb_n[s_plb[k]]++;
                }
                if (nb < 0) {
                    rf_note("REJECT", TG_RF_SRC_PARALLEL, rf_way_name(s_pw[(F + R) / 2]),
                            F, R, nb == -1 ? "the parallel road is not matched along "
                                             "the whole block"
                                           : "the parallel road is on the +t side "
                                             "(left corridors are parked)");
                    break;
                }
                for (k = 1; k < 13; k++) if (lb_n[k] > nb) { nb = lb_n[k]; lb_best = k; }
                c = rf_new_cand(TG_RF_SRC_PARALLEL, rf_way_name(s_pw[(F + R) / 2]), F, R,
                                (F >= 0 && F < s_rn) ? s_rl[F] : 2, lb_best);
                if (!c) { free(s_vt); s_vt = NULL; s_vtn = 0; return; }
                for (k = F; k <= R; k++) c->off[k - F] = s_plat[k];
                if (rf_validate(c)) {
                    c->weight = (double)(c->len > 90 ? 90 : c->len) * 0.9;
                    s_ncand++;
                }
                break;     /* the nearest long-enough gate only */
            }
        a = b + 1;
    }
    free(s_vt); s_vt = NULL; s_vtn = 0;
}


/* ---- SOURCE 2: plaza rings -- MEASURED, NOT BUILT ----------------------------
 *
 * Logs every named junction=circular|roundabout group the route touches, with the
 * numbers that say why its far side cannot be a corridor in this engine. The fork
 * corridor is tied 1:1 to the MAIN node it rides: corridor step k sits on node
 * F+1+k at a lateral shift, and the road mesh, the strip rows, the ground, the AI
 * heading table, the carriageway authority and the scenery all key off that node.
 * A plaza's far side is not a function of the route's nodes: on Mariano's route
 * Plaza Miguel de Azcuenaga's ring is ~90 m in radius, the route skirts one side of
 * it, and the other side is a ~290 m arc reaching ~180 m from the route -- against
 * a 65 m (TD5_TG_R8_LAT_MAX) lateral cap, a lateral rate limit of 0.35 span per
 * span, and ground that stops 70 m from the route. The corridor needs geometry of
 * its own. TD5RE_GEO_FORK_PLAZA=1 (the default) only turns this report on. */
static void rf_plaza_report(void)
{
    const int nways = td5_geo_roads_count();
    int ids[16], nid = 0, g, w, k, i;

    if (!td5_env_flag_on("TD5RE_GEO_FORK_PLAZA")) return;
    if (!td5_geo_roads_sync(td5_geo_place_slug())) return;
    for (w = 0; w < nways && nid < 16; w++) {
        const TD5_GeoRoad *r = td5_geo_roads_get(w);
        int have = 0;
        if (!r || !r->roundabout || r->name_id < 0) continue;
        for (g = 0; g < nid; g++) if (ids[g] == r->name_id) { have = 1; break; }
        if (!have) ids[nid++] = r->name_id;
    }
    for (g = 0; g < nid; g++) {
        double sx = 0.0, sz = 0.0, rad = 0.0, a0 = 0.0, a1 = 0.0;
        int n = 0, ia = -1, ib = -1;
        for (w = 0; w < nways; w++) {
            const TD5_GeoRoad *r = td5_geo_roads_get(w);
            if (!r || !r->roundabout || r->name_id != ids[g]) continue;
            for (k = 0; k < r->count; k++) {
                double x = 0.0, z = 0.0;
                if (td5_geo_roads_point(r, k, &x, &z)) { sx += x; sz += z; n++; }
            }
        }
        if (n < 3) continue;
        sx /= (double)n; sz /= (double)n;
        for (w = 0; w < nways; w++) {
            const TD5_GeoRoad *r = td5_geo_roads_get(w);
            if (!r || !r->roundabout || r->name_id != ids[g]) continue;
            for (k = 0; k < r->count; k++) {
                double x = 0.0, z = 0.0;
                if (td5_geo_roads_point(r, k, &x, &z))
                    rad += sqrt((x - sx) * (x - sx) + (z - sz) * (z - sz));
            }
        }
        rad /= (double)n;
        /* the route nodes that run ALONG the ring (within 12 m of its circle) */
        for (i = 0; i < s_rn; i++) {
            const double d = sqrt((s_rx[i] - sx) * (s_rx[i] - sx) + (s_rz[i] - sz) * (s_rz[i] - sz));
            if (d < rad - 12.0 * TG_RF_UPM || d > rad + 12.0 * TG_RF_UPM) continue;
            if (ia < 0) { ia = i; a0 = atan2(s_rz[i] - sz, s_rx[i] - sx); }
            ib = i; a1 = atan2(s_rz[i] - sz, s_rx[i] - sx);
        }
        if (ia < 0 || ib - ia < 4) continue;
        {
            double dth = a1 - a0;
            double arc, far_arc;
            while (dth >  TD5_TG_PI) dth -= 2.0 * TD5_TG_PI;
            while (dth < -TD5_TG_PI) dth += 2.0 * TD5_TG_PI;
            arc = (dth < 0.0 ? -dth : dth) * rad;
            far_arc = 2.0 * TD5_TG_PI * rad - arc;
            TD5_LOG_I(LOG_TAG, "trackgen: [REAL FORK] plaza \"%s\": ring r=%.0f m, the "
                      "route runs along it over nodes %d..%d (%.0f m of arc); the other "
                      "side is %.0f m of arc up to %.0f m from the route -- not a "
                      "function of the route's nodes (lateral cap %.0f m), NOT BUILT: "
                      "needs a free-geometry corridor",
                      td5_geo_roads_name_by_id(ids[g]), rad / TG_RF_UPM, ia, ib,
                      arc / TG_RF_UPM, far_arc / TG_RF_UPM, 2.0 * rad / TG_RF_UPM,
                      TD5_TG_R8_LAT_MAX / TG_RF_UPM);
        }
    }
}

/* ---- selection ----------------------------------------------------------- */

static int rf_cmp_cand(const void *pa, const void *pb)
{
    const RfCand *a = (const RfCand *)pa, *b = (const RfCand *)pb;
    if (a->F != b->F) return a->F - b->F;
    return a->R - b->R;
}

/* Weighted interval scheduling: windows may not overlap, and a window is the
 * lane ramp in front of F through the lane ramp behind R. Keeps the combination
 * that drives the most corridor. */
static int rf_select(int *pick)
{
    static double dp[TG_RF_MAXCAND];
    static int    prv[TG_RF_MAXCAND];
    int i, j, best = -1, n = 0, chain[TG_RF_MAXCAND];
    double bestw = -1.0;

    if (s_ncand < 1) return 0;
    qsort(s_cand, (size_t)s_ncand, sizeof s_cand[0], rf_cmp_cand);
    for (i = 0; i < s_ncand; i++) {
        dp[i] = s_cand[i].weight; prv[i] = -1;
        for (j = 0; j < i; j++) {
            /* The full-width windows (F-8 .. R+2) stay apart; the tapers on either
             * side of them may overlap, see TG_RF_MIN_GAP. */
            if (s_cand[i].F - TG_RF_WIDEN > s_cand[j].R + 2 + TG_RF_MIN_GAP &&
                dp[j] + s_cand[i].weight > dp[i]) {
                dp[i] = dp[j] + s_cand[i].weight; prv[i] = j;
            }
        }
        if (dp[i] > bestw) { bestw = dp[i]; best = i; }
    }
    for (i = best; i >= 0; i = prv[i]) chain[n++] = i;
    /* chain is last..first; the table has to ascend in F. */
    for (i = 0; i < n; i++) pick[i] = chain[n - 1 - i];
    /* The engine holds TD5_TG_BRANCH_MAX forks: keep the longest. */
    while (n > TD5_TG_BRANCH_MAX) {
        int k, shortest = 0;
        for (k = 1; k < n; k++)
            if (s_cand[pick[k]].len < s_cand[pick[shortest]].len) shortest = k;
        for (k = shortest; k + 1 < n; k++) pick[k] = pick[k + 1];
        n--;
    }
    return n;
}

/* ---- finalise a chosen candidate ------------------------------------------ */

static void rf_finalise(RfFork *f, const RfCand *c)
{
    const double lw   = rf_lane_w();
    const double half = (double)(c->lanes_a + c->lanes_b) * lw * 0.5;
    /* How fast the median may open, world units per span. TD5_TG_BRANCH_RATE (0.35
     * span/span, 19 degrees) is the CEILING every fork stays under, not a shape:
     * the synthetic avenue's bow leaves at 2.6 degrees. A real median opened at the
     * ceiling put an S (3.7 m across in 3 spans) in front of cars doing 200 km/h
     * and measured pile-ups at the corridor entries (round 1013 F2b: 3 cars jammed
     * at fork 2 step 6, 670 ticks at full lock). 0.12 span/span is 6.8 degrees. */
    const double rate = (double)td5_env_float("TD5RE_GEO_FORK_SLOPE", 0.12f, 0.02f,
                                              (float)TD5_TG_BRANCH_RATE)
                      * (double)TD5_TG_SPAN_LENGTH;
    int k;

    memset(f, 0, sizeof *f);
    f->src = c->src; f->F = c->F; f->R = c->R; f->len = c->len;
    f->lanes_a = c->lanes_a; f->lanes_b = c->lanes_b;
    f->i0 = c->i0; f->w_end = c->w_end;
    f->taper_in = c->taper_in; f->taper_out = c->taper_out;
    snprintf(f->name, sizeof f->name, "%s", c->name);

    f->med_real_lo = 1e30; f->med_real_hi = 0.0;
    for (k = 0; k <= c->len; k++) {
        /* corridor step k sits on main node F+1+k; the sidecar's offset is the
         * distance between the two carriageway CENTRELINES, so the median is
         * that minus half of each carriageway. */
        const int j = k + 1;
        const double a = (c->off[j] < 0.0) ? -c->off[j] : c->off[j];
        double med = a - half;
        if (med < 0.0) med = 0.0;
        f->med[k] = med;
        if (med < f->med_real_lo) f->med_real_lo = med;
        if (med > f->med_real_hi) f->med_real_hi = med;
    }
    /* Both mouths pinned to zero (the corridor lines up with the road halves
     * there), then rate-limited from each end so the corridor never bends
     * faster than every other fork's does. Only ever LOWERS a value, so the
     * corridor is never farther from the race road than the map puts it. */
    f->med[0] = 0.0; f->med[c->len] = 0.0;
    for (k = 1; k <= c->len; k++)
        if (f->med[k] > f->med[k - 1] + rate) f->med[k] = f->med[k - 1] + rate;
    for (k = c->len - 1; k >= 0; k--)
        if (f->med[k] > f->med[k + 1] + rate) f->med[k] = f->med[k + 1] + rate;
    for (k = 0; k <= c->len; k++)
        if (f->med[k] > f->med_eff_hi) f->med_eff_hi = f->med[k];
}

/* ---- the build ----------------------------------------------------------- */

void tg_realfork_reset(void)
{
    s_rf_n = 0;
    s_ncand = 0;
    s_rn = 0;
    s_rf_knob = -1;
}

int tg_realfork_build(void)
{
    int n, i, npick, pick[TG_RF_MAXCAND], f;
    long cov = 0;

    tg_realfork_reset();
    s_rf_knob = tg_realfork_enabled() ? 1 : 0;   /* answered from the env just now */
    if (!s_rf_knob) return 0;

    s_rf_diag = td5_env_flag_off("TD5RE_GEO_FORK_DIAG");
    s_rf_taper = td5_env_int("TD5RE_GEO_FORK_TAPER", TG_RF_TAPER_DEF, 2, 48);
    s_rf_taper_smooth = td5_env_flag_off("TD5RE_GEO_FORK_TAPER_SMOOTH");   /* default linear */
    n = td5_geo_route_count();
    if (n > TG_RF_NODES) n = TG_RF_NODES;
    for (i = 0; i < n; i++) {
        double x = 0.0, z = 0.0; int l = 0;
        td5_geo_route_node(i, &x, &z, &l);
        l = tg_geo_plaza_floor(i, l);      /* [1014 B] the same lanes the walk will push */
        s_rx[i] = x; s_rz[i] = z; s_rl[i] = l;
        s_node_fork[i] = -1; s_node_ovr[i] = 0; s_node_dt[i] = 0.0;
        s_node_extra[i] = 0.0;
    }
    s_rn = n;

    rf_gen_avenue();
    rf_gen_parallel();
    rf_plaza_report();
    /* PLAZA (TD5RE_GEO_FORK_PLAZA) -- NOT BUILT, and the reason is a measured
     * one, not a deferral: the fork corridor is tied 1:1 to the MAIN node it
     * rides (corridor step k sits on node F+1+k at a lateral shift), and every
     * emitter, the AI heading table, the carriageway authority and the scenery
     * keys off that node. A plaza's far side is the wrong shape for it: on
     * Mariano's route Plaza Miguel de Azcuenaga is a ~230 m octagon, the route
     * skirts its south side (raw 27..44) and the other side is a ~350 m arc up
     * to ~230 m from the route, against a 65 m lateral cap. That needs a corridor
     * with geometry of its own, which is a refactor of the emitters, not a
     * profile. See docs/plans/GEO_REAL_FORKS.md. */

    npick = rf_select(pick);
    for (f = 0; f < npick; f++) {
        RfFork *rf = &s_rf[s_rf_n];
        const RfCand *c = &s_cand[pick[f]];
        rf_finalise(rf, c);
        /* Fill the node window the walk reads. */
        for (i = c->i0; i <= c->w_end && i < s_rn; i++) {
            double fr = 1.0, extra, want;
            if (i < 0) continue;
            if (i < c->F - TG_RF_WIDEN)
                fr = (double)(i - c->i0) / (double)c->taper_in;            /* widening */
            else if (i > c->R + 2)
                fr = 1.0 - (double)(i - (c->R + 2)) / (double)c->taper_out;  /* narrowing */
            if (fr < 0.0) fr = 0.0; else if (fr > 1.0) fr = 1.0;
            /* Smoothstep: zero slope at both ends of the taper, so the road edge
             * does not kink where the taper starts and stops (a linear ramp
             * changes the edge's heading by its whole slope in one span, which
             * a car at 190 km/h answers with a rear slide: measured at spans
             * 131..134 behind fork 0). Same peak edge angle as the linear 16
             * at 24 nodes (1.5 x 7 m / 84 m). */
            if (s_rf_taper_smooth) fr = fr * fr * (3.0 - 2.0 * fr);
            /* [ROUND 1015 A] The ramp grows the road from the lanes the route HAS at
             * this node to the fork's width, so a node that already carries more than
             * lanes(A) (the 3-lane approach to Diagonal 73) is only widened by what is
             * still missing. With the route at lanes(A) -- every node of the round-1013
             * windows -- this is lanes_b * fr exactly. */
            want  = (double)c->lanes_a + (double)c->lanes_b * fr;
            extra = want - (double)s_rl[i];
            if (extra < 0.0) extra = 0.0;
            if (extra <= s_node_extra[i] && s_node_fork[i] >= 0) continue;  /* shared taper */
            s_node_fork[i]  = s_rf_n;
            s_node_extra[i] = extra;
            /* the lane COUNT is the rounded width; 0 = leave the route's own */
            s_node_ovr[i] = (extra >= 0.5) ? s_rl[i] + (int)(extra + 0.5) : 0;
        }
        cov += rf->len;
        TD5_LOG_I(LOG_TAG, "trackgen: [REAL FORK] %d: %s \"%s\" F=%d len=%d R=%d "
                  "lanes %d+%d, median %.2f..%.2f m real (%.2f m built), "
                  "ring window nodes %d..%d", s_rf_n,
                  rf->src == TG_RF_SRC_AVENUE ? "avenue"
                      : rf->src == TG_RF_SRC_PARALLEL ? "parallel" : "plaza",
                  rf->name, rf->F, rf->len, rf->R, rf->lanes_a, rf->lanes_b,
                  rf->med_real_lo / 430.0, rf->med_real_hi / 430.0,
                  rf->med_eff_hi / 430.0, rf->i0, rf->w_end);
        s_rf_n++;
    }
    TD5_LOG_I(LOG_TAG, "trackgen: [REAL FORK] %d driveable fork(s) from the real "
              "map, %ld corridor span(s) (%d candidate(s) considered; knobs "
              "AVENUE=%d PLAZA=%d PARALLEL=%d)", s_rf_n, cov, s_ncand,
              td5_env_flag_on("TD5RE_GEO_FORK_AVENUE"),
              td5_env_flag_on("TD5RE_GEO_FORK_PLAZA"),
              td5_env_flag_on("TD5RE_GEO_FORK_PARALLEL"));
    return s_rf_n;
}
