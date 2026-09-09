/**
 * td5_trackgen.c -- procedural (AUTO-GENERATED) track builder: spec, RNG, centerline, elevation, strip + routes, scenery orchestration, build/regenerate entry points (PORT-ONLY)
 *
 * Split from the td5_trackgen.c monolith (2026-09-06); shared declarations
 * live in td5_trackgen_internal.h. Element map: docs/plans/AUTOTRACK_ELEMENT_CATALOG.md.
 */
#include "td5_trackgen_internal.h"

/* [PICK] Human name for an auto-track texture page id, for the dev geometry
 * picker's HUD/clipboard. Defined ENTIRELY in terms of the TD5_TG_PAGE_*
 * constants above (and their variant counts), so it can never drift out of sync
 * with the numbering the way a hardcoded table would. Covers the named leaf
 * pages and the main variant ranges -- the pages you actually hover; returns
 * NULL for reserved/spare slots so the picker falls back to the bare number.
 * Valid ONLY for the auto slot (page ids are per-track). */
const char *td5_trackgen_page_name(int page)
{
    switch (page) {
    case TD5_TG_PAGE_ROAD:          return "ROAD";
    case TD5_TG_PAGE_WALL:          return "WALL";
    case TD5_TG_PAGE_GREEN:         return "GREEN";
    case TD5_TG_PAGE_TREE:          return "TREE";
    case TD5_TG_PAGE_RAIL:          return "RAIL";
    case TD5_TG_PAGE_GROUND:        return "GROUND";
    case TD5_TG_PAGE_WATER:         return "WATER";
    case TD5_TG_PAGE_SIDEWALK:      return "SIDEWALK";
    case TD5_TG_PAGE_CROSSING:      return "CROSSING";
    case TD5_TG_PAGE_FENCE:         return "FENCE";
    case TD5_TG_PAGE_TREELINE:      return "TREELINE";
    case TD5_TG_PAGE_TUNNEL:        return "TUNNEL";
    case TD5_TG_PAGE_SNOW:          return "SNOW";
    case TD5_TG_PAGE_HILL:          return "HILL";
    case TD5_TG_PAGE_BANNER:        return "BANNER_LEG";
    case TD5_TG_PAGE_LAMPPOST:      return "LAMPPOST";
    case TD5_TG_PAGE_START_L:       return "START_L";
    case TD5_TG_PAGE_START_R:       return "START_R";
    case TD5_TG_PAGE_FINISH_L:      return "FINISH_L";
    case TD5_TG_PAGE_FINISH_R:      return "FINISH_R";
    case TD5_TG_PAGE_BRIDGE_DECK:   return "BRIDGE_DECK";
    case TD5_TG_PAGE_BRANCH_KERB:   return "BRANCH_KERB";
    case TD5_TG_PAGE_R4_GUARDRAIL:  return "GUARDRAIL";
    case TD5_TG_PAGE_R4_PIER:       return "PIER";
    case TD5_TG_PAGE_R4_COAST:      return "COAST";
    case TD5_TG_PAGE_R4_SKYLINE:    return "SKYLINE";
    case TD5_TG_PAGE_R5_LEG:        return "GANTRY_LEG";
    case TD5_TG_PAGE_R9_BORE_CEIL:  return "TUNNEL_CEIL";
    case TD5_TG_PAGE_R9_PORTAL_SURR:return "PORTAL_SURROUND";
    case TD5_TG_PAGE_R9_PORTAL_FACE:return "PORTAL_FACE";
    case TD5_TG_PAGE_R9_UP_ABUT:    return "UNDERPASS_ABUT";
    case TD5_TG_PAGE_R9_UP_SOFFIT:  return "UNDERPASS_SOFFIT";
    case TD5_TG_PAGE_R9_UP_DECK:    return "UNDERPASS_DECK";
    case TD5_TG_PAGE_R9_UP_PARAPET: return "UNDERPASS_PARAPET";
    case TD5_TG_PAGE_R11_SIGN_LEFT: return "SIGN_LEFT";
    case TD5_TG_PAGE_R11_SIGN_RIGHT:return "SIGN_RIGHT";
    case TD5_TG_PAGE_R11_SIGN_STRAIGHT:return "SIGN_STRAIGHT";
    case TD5_TG_PAGE_R11_SIGN_POST: return "SIGN_POST";
    default: break;
    }
    /* Variant ranges (each a group of consecutive pages). */
    if (page >= TD5_TG_PAGE_WALL_EXTRA &&
        page <  TD5_TG_PAGE_WALL_EXTRA + TD5_TG_WALL_VARIANTS - 1) return "WALL_VARIANT";
    if (page >= TD5_TG_PAGE_STORE &&
        page <  TD5_TG_PAGE_STORE + TD5_TG_STORE_VARIANTS)        return "STORE";
    if (page >= TD5_TG_PAGE_TREE_EXTRA &&
        page <  TD5_TG_PAGE_TREE_EXTRA + TD5_TG_TREE_VARIANTS)    return "TREE_VARIANT";
    if (page >= TD5_TG_PAGE_PROP &&
        page <  TD5_TG_PAGE_PROP + TD5_TG_PROP_COUNT)             return "PROP";
    if (page >= TD5_TG_PAGE_ROAD_EXTRA &&
        page <  TD5_TG_PAGE_ROAD_EXTRA + TD5_TG_ROAD_VARIANTS - 1) return "ROAD_VARIANT";
    if (page >= TD5_TG_PAGE_TUNNEL_VAR &&
        page <  TD5_TG_PAGE_TUNNEL_VAR + TD5_TG_TUNNEL_VARIANTS - 1) return "TUNNEL_VARIANT";
    if (page >= TD5_TG_PAGE_R5_FLORA &&
        page <  TD5_TG_PAGE_R5_FLORA + TD5_TG_R5_FLORA_N)         return "FLORA";
    if (page >= TD5_TG_PAGE_R7_WALL_LOW &&
        page <  TD5_TG_PAGE_R7_WALL_LOW + TD5_TG_R7_WALL_LOW_N)   return "WALL_LOW";
    if (page >= TD5_TG_PAGE_R7_WALL_TOWER &&
        page <  TD5_TG_PAGE_R7_WALL_TOWER + TD5_TG_R7_WALL_TOWER_N) return "WALL_TOWER";
    if (page >= TD5_TG_PAGE_R8V_RAIL &&
        page <  TD5_TG_PAGE_R8V_RAIL + TD5_TG_R8V_RAIL_N)         return "GUARDRAIL_VAR";
    return NULL;
}

/* ---------------------------------------------------------------- RNG ----- */
/* Private xorshift32 -- deliberately NOT rand(). The game's rand() is the
 * MSVC-compatible one used for sim determinism and netplay lockstep
 * (td5_msvc_rand.c); drawing track geometry from it would perturb every
 * downstream random draw and break trace goldens. */
static unsigned int s_rng;

/* Seed to run the S2 regen gate against, set during a build and consumed after
 * it finishes (the gate itself rebuilds a centerline, so it cannot run from
 * inside one). 0 = nothing to check. */
unsigned int s_selfcheck_regen_seed = 0;

TG_Fork s_forks[TD5_TG_BRANCH_MAX];

int s_fork_count;

int s_ring_len;

/* ==========================================================================
 * [PARALLEL S0/S1, from autotrack-studio ba8423da] Groundwork for running the
 * per-entry emit loop on more than one thread. All three are INERT
 * single-threaded, which is why they can land ahead of the threading itself.
 * ========================================================================== */

/* [PERF LEVER 1] Does THIS build want scenery?
 *
 * Boot needs the AUTO-GENERATED entry to EXIST in the selector, not to have
 * buildings: every race launch regenerates with a fresh seed anyway
 * (td5_asset_load_level), so the boot build's MODELS.DAT is thrown away
 * unread. Measured, that discarded work is 99.4 percent of a 30-130 s boot.
 *
 * Separate from TD5RE_AUTOTRACK_SCENERY, which is the player's choice of
 * ribbon-vs-scenery and must keep working independently. Both must be true to
 * emit. Reset to 1 after each use so only the boot path is ever affected. */
int s_want_scenery = 1;

/* [SCENERY STREAMING] Does THIS build hand its scenery to a worker instead of
 * emitting it inline?
 *
 * A streamed build does the geometry (about 300 ms) plus TEXTURES.DAT, writes
 * NO MODELS.DAT, and parks everything the scenery phases need -- the node list
 * above all -- for td5_trackgen_stream_scenery to pick up on the streaming
 * worker after the level has loaded. Same three-way relationship as
 * s_want_scenery: independent of the player's TD5RE_AUTOTRACK_SCENERY choice,
 * and reset after each use so only the path that asked for it is affected.
 *
 * THE NODE LIST IS OWNED HERE, not by build_level. build_level frees nl.v at
 * its `done:` label; a streamed build transfers the pointer out first and this
 * module frees it when the worker finishes or is cancelled. */
int         s_want_stream = 0;

/* [R14 GENPERF] crossing-predicate memo (see tg_city_crossing_here). */
signed char s_xhere_memo[TD5_TG_MAX_SPANS];

signed char s_xbase_memo[TD5_TG_MAX_SPANS];

int         s_xmemo_armed;

void tg_xmemo_reset(int armed)
{
    memset(s_xhere_memo, -1, sizeof(s_xhere_memo));
    memset(s_xbase_memo, -1, sizeof(s_xbase_memo));
    /* TD5RE_TG_XMEMO=0: A/B the cache against the recomputing path. */
    s_xmemo_armed = armed && td5_env_flag_on("TD5RE_TG_XMEMO");
}

/* Seed of the last successful build, for reproducing a good random track. */
static unsigned int s_last_seed = 0;

/* [R8 G1] Seed of the build in progress, latched at the top of
 * td5_trackgen_build_level. s_last_seed above is only written AFTER the build
 * finishes, so an emitter reading it during the build would get the PREVIOUS
 * track's seed -- which is why this is a second static rather than a reuse. */
unsigned int s_gen_seed = 0;

unsigned int tg_gen_seed(void) { return s_gen_seed; }

static const char *const k_tg_zone_name[TG_ZONE_COUNT] = {
    "centerline", "strip", "routes", "models prepass",
    "models emit", "r9 city scan", "guard on-road", "guard over-water",
    "models assemble", "textures", "reports",
    "biome layout", "elevation", "levelinf emit",
    "write strip+trk", "write models", "sky install",
    "terrain pre-pass",
    "[in emit] loop 1", "[in emit] loop 2", "[in emit] guard validate",
    "world heightfield", "street network", "world trim"
};

uint64_t s_tg_zone_us[TG_ZONE_COUNT];

long     s_tg_zone_n[TG_ZONE_COUNT];

uint64_t s_tg_build_t0 = 0;   /* whole-build wall clock, for the % column */

uint64_t s_tg_reports_t0 = 0; /* diagnostic block, spans many call sites  */

static uint64_t s_tg_emit_t0 = 0;    /* per-span emit loop, one entry at a time  */

uint64_t s_tg_guard_t0 = 0;   /* on-road guard, per mesh                  */

uint64_t s_tg_wet_t0 = 0;     /* over-water audit, per mesh               */

static const char *const k_tg_sub_name[TG_SUB_COUNT] = {
    "ground skirt", "road quad", "guardrail", "fb city", "fb block",
    "fb cross", "fb flora", "fb forest-cross", "fb park trees",
    "fb slope flora", "fb terrain", "fb infra", "fb track",
    "tunnel", "bridge", "props", "signs", "other"
};

uint64_t s_tg_sub_us[TG_SUB_COUNT];

long     s_tg_sub_n[TG_SUB_COUNT];

uint64_t s_tg_sub_t0 = 0;

int      s_tg_sub_r  = 0;

static const char *const k_tg_sub2_name[TG_SUB2_COUNT] = {
    "cross/sidewalls", "cross/join-zebra", "cross/street-flank",
    "cross/gap-infill", "terrain/far-band", "terrain/far-shore",
    "cross/span-paved"
};

uint64_t s_tg_sub2_us[TG_SUB2_COUNT];

long     s_tg_sub2_n[TG_SUB2_COUNT];

uint64_t s_tg_sub2_t0 = 0;

int      s_tg_sub2_r  = 0;

static const char *const k_tg_sub3_name[TG_SUB3_COUNT] = {
    "band/ground-side", "band/dry-reach", "band/road-cap", "band/road-edge"
};

uint64_t s_tg_sub3_us[TG_SUB3_COUNT];

long     s_tg_sub3_n[TG_SUB3_COUNT];

uint64_t s_tg_sub3_t0 = 0;

double   s_tg_sub3_d  = 0.0;

static const char *const k_tg_t_name[TG_T_COUNT] = {
    "rail/clear-gap", "rail/on-carriageway", "rail/edge-would", "rail/deck-here",
    "rail/kerbfence-here", "rail/street-crosses",
    "city/span-paved", "city/sidewalk", "city/fence", "city/verge-band",
    "city/crossing", "city/cross-street", "city/lamp", "city/backrows",
    "city/forkback", "city/r8-diag",
    "pre/turn-map", "pre/r8-cross-report", "pre/r10-cross-report", "pre/r11-city-report",
    "pre/r13-junc-report", "pre/r13-fill-report",
    "rpt/acct", "rpt/rail-edge", "rpt/r11-guard", "rpt/r13-rail-mouth",
    "rpt/r12-flora", "rpt/r9-bridge", "rpt/r8-bridge-diag", "rpt/r12-spanq",
    "rpt/r8-terrain-extent", "rpt/r9-topo", "rpt/r11-xcurve", "rpt/r12-fcross",
    "rpt/r13-band", "rpt/r11-water", "rpt/r12-tex",
    "rpt/r14-up", "rpt/r14-band", "rpt/r14-coast"
};

uint64_t s_tg_t_us[TG_T_COUNT];

long     s_tg_t_n[TG_T_COUNT];

uint64_t s_tg_t_t0[8];

int      s_tg_t_depth;

int      s_tg_t_ri;

double   s_tg_t_rd;

/* [R14 GENPERF 2026-09-03] Master gate for the per-area diagnostic REPORTS.
 * Several of them were "opt-in" on td5_env_flag_on, which returns 1 when the
 * variable is UNSET -- so they ran on every build, for every player, and cost
 * seconds (the r11-city and r13-junction sweeps alone were most of the 10 s
 * "models prepass"). Rule: a report runs when its OWN variable is explicitly
 * "1", is skipped when it is explicitly "0", and otherwise follows the master
 * TD5RE_TG_REPORTS (default OFF). The verify scripts that need a report set
 * either the master or the specific variable. */
int tg_report_wanted(const char *own)
{
    const char *o = getenv(own);
    const char *m;
    if (o && o[0] == '1') return 1;
    if (o && o[0] == '0') return 0;
    m = getenv("TD5RE_TG_REPORTS");
    return (m && m[0] == '1') ? 1 : 0;
}

/* [R14 GENPERF 2026-09-03] Build progress 0..100 for the loading-screen bar,
 * written by the generator (worker thread) and read by the pump. */
volatile int s_tg_progress;

int td5_trackgen_progress(void) { return s_tg_progress; }

void tg_zone_reset(void)
{
    memset(s_tg_t_us, 0, sizeof s_tg_t_us);
    memset(s_tg_t_n,  0, sizeof s_tg_t_n);
    s_tg_t_depth = 0;
    memset(s_tg_sub3_us, 0, sizeof s_tg_sub3_us);
    memset(s_tg_sub3_n,  0, sizeof s_tg_sub3_n);
    memset(s_tg_sub2_us, 0, sizeof s_tg_sub2_us);
    memset(s_tg_sub2_n,  0, sizeof s_tg_sub2_n);
    memset(s_tg_zone_us, 0, sizeof s_tg_zone_us);
    memset(s_tg_zone_n,  0, sizeof s_tg_zone_n);
    memset(s_tg_sub_us,  0, sizeof s_tg_sub_us);
    memset(s_tg_sub_n,   0, sizeof s_tg_sub_n);
}

/* [MEASURE 2026-09-04] How much of this memo does a build actually use?
 *
 * The question decides the threading design and two sources disagree on it.
 * 41c01090's note says precomputing every slot "would do far more work than
 * the build actually asks for", so the memo must stay lazy and be made safe by
 * publish order. The later session's finding is that threading the terrain
 * pre-pass is slow precisely BECAUSE the memo is lazy and shared, so every
 * worker cold-misses it. Both cannot be acted on at once, and neither states
 * the fill DENSITY, which is the number that settles it:
 *
 *   density high  -> a serial pre-warm costs about what the build pays anyway,
 *                    the memo becomes read-only for workers, blocker gone.
 *   density low   -> pre-warming is real extra work; the memo needs per-thread
 *                    copies (or the terrain path needs its own narrow cache).
 *
 * s_xs_phase marks the region about to be threaded, so the terrain pre-pass's
 * own share is separated from the rest of the build. Counters only. */
long s_xs_fill, s_xs_hit;            /* whole build   */

long s_xs_fill_terr, s_xs_hit_terr;  /* terrain phase */

int  s_xs_phase;                     /* 1 = inside the terrain pre-pass */

/* WARN, not INFO: the shipped td5re.ini runs MinLevel=1, so an INFO report is
 * dropped exactly on the configuration a user would be timing. */
void tg_zone_report(uint64_t total_us)
{
    int z;
    if (!td5_env_flag_on("TD5RE_R14_GENPROF")) return;
    TD5_LOG_W(LOG_TAG, "[R14 GENPERF] generation took %.1f s", total_us / 1e6);
    /* [MEASURE 2026-09-04] side-street memo density -- see the counters. */
    {
        /* Slots the build could possibly fill: two sides per main-ring span.
         * (The memo's own array bound, TD5_TG_R10_XS_MAX, is declared much
         * later in this file; the ring is always the smaller of the two.) */
        long slots = 2L * (long)(s_ring_len > 0 ? s_ring_len : 0);
        TD5_LOG_W(LOG_TAG, "[R14 GENPERF] xs-memo: filled %ld of %ld slots "
                  "(%.1f%%), hits %ld (%.1fx reuse); terrain phase: filled %ld "
                  "hits %ld",
                  s_xs_fill, slots, slots ? 100.0 * (double)s_xs_fill / (double)slots : 0.0,
                  s_xs_hit, s_xs_fill ? (double)s_xs_hit / (double)s_xs_fill : 0.0,
                  s_xs_fill_terr, s_xs_hit_terr);
    }
    for (z = 0; z < TG_ZONE_COUNT; z++) {
        if (!s_tg_zone_n[z]) continue;
        TD5_LOG_W(LOG_TAG, "[R14 GENPERF]   %-17s %8.1f ms  %5.1f%%  (n=%ld)",
                  k_tg_zone_name[z], s_tg_zone_us[z] / 1e3,
                  total_us ? 100.0 * (double)s_tg_zone_us[z] / (double)total_us
                           : 0.0,
                  s_tg_zone_n[z]);
    }
    for (z = 0; z < TG_SUB_COUNT; z++) {
        if (!s_tg_sub_n[z]) continue;
        TD5_LOG_W(LOG_TAG, "[R14 GENPERF]     emit/%-15s %8.1f ms  %5.1f%%  (n=%ld)",
                  k_tg_sub_name[z], s_tg_sub_us[z] / 1e3,
                  total_us ? 100.0 * (double)s_tg_sub_us[z] / (double)total_us
                           : 0.0,
                  s_tg_sub_n[z]);
    }
    for (z = 0; z < TG_SUB2_COUNT; z++) {
        if (!s_tg_sub2_n[z]) continue;
        TD5_LOG_W(LOG_TAG, "[R14 GENPERF]       %-19s %8.1f ms  %5.1f%%  (n=%ld)",
                  k_tg_sub2_name[z], s_tg_sub2_us[z] / 1e3,
                  total_us ? 100.0 * (double)s_tg_sub2_us[z] / (double)total_us
                           : 0.0,
                  s_tg_sub2_n[z]);
    }
    for (z = 0; z < TG_SUB3_COUNT; z++) {
        if (!s_tg_sub3_n[z]) continue;
        TD5_LOG_W(LOG_TAG, "[R14 GENPERF]         %-21s %8.1f ms  %5.1f%%  (n=%ld)",
                  k_tg_sub3_name[z], s_tg_sub3_us[z] / 1e3,
                  total_us ? 100.0 * (double)s_tg_sub3_us[z] / (double)total_us
                           : 0.0,
                  s_tg_sub3_n[z]);
    }
    for (z = 0; z < TG_T_COUNT; z++) {
        if (!s_tg_t_n[z]) continue;
        TD5_LOG_W(LOG_TAG, "[R14 GENPERF]   callee %-22s %8.1f ms  %5.1f%%  (n=%ld)",
                  k_tg_t_name[z], s_tg_t_us[z] / 1e3,
                  total_us ? 100.0 * (double)s_tg_t_us[z] / (double)total_us
                           : 0.0,
                  s_tg_t_n[z]);
    }
}

void tg_srand(unsigned int seed)
{
    s_fork_plan_seed = seed;   /* [FORK KINDS] plan rotation, known before the walk */
    s_rng = seed ? seed : 0x9E3779B9u;
}

const char *const k_acct_names[TG_ACCT_KIND_COUNT] = {
    "buildings", "sidewalks", "shopfronts", "fences",   "lamps",
    "crossings", "trees",     "props",      "water",    "bridge-pieces",
    "tunnel-pieces", "terrain", "far-bands", "banners", "guardrails",
    "road-quads", "checkpoints", "branch-nodes",
    /* [R3] appended in enum order: BLOCK's park/house, CITY's step wall. */
    "parks", "houses", "step-walls",
    /* [R4] one reserved name per area, in enum order, each on its own line so
     * two areas renaming their own slot touch non-adjacent lines. Rename the
     * string to match your renamed enum constant. Keep the trailing comma on
     * every line but the last -- the R3 union merge lost exactly one comma and
     * two names silently concatenated into a single string literal. */
    "r4-flow",              /* FLOW   */
    "cross-furn",           /* CROSS  */
    "fork-back",            /* CITY   */
    "r4-branch",            /* BRANCH */
    "coastline",            /* BRIDGE item 20 */
    /* [R5] one reserved name per area, in enum order, each on its own line and
     * spaced by a blank line. Rename the string to match your renamed enum
     * constant. Keep the trailing comma on every line but the last. */
    "r5-cross",             /* CROSS  */

    "r5-city",              /* CITY   */

    "bridge-kerb",          /* BRIDGE (item 17 deck kerb) */

    "branch-verge",         /* STRUCT item 10 */

    "r5-flora",             /* FLORA  */
    /* [R6] one reserved name per area, in enum order, blank-line spaced.
     * Rename to match your renamed enum constant; keep every comma. */
    "r6-cross",             /* CROSS  */

    "r6-city",              /* CITY   */

    "r6-branch",            /* BRANCH */

    "r6-tunnel",            /* TUNNEL */

    "r6-bridge",            /* BRIDGE */

    "r6-flora",             /* FLORA  */
    /* [R7] one reserved name per area, in enum order, blank-line spaced.
     * Rename to match your renamed enum constant; keep every comma. */
    "guard-rejects",        /* GUARD  */

    "r7-cross",             /* CROSS  */

    "r7-city",              /* CITY   */

    "r7-branch",            /* BRANCH */

    "r7-bridge",            /* BRIDGE */

    "r7-flora",             /* FLORA  */
    /* [R8] one reserved name per area, in enum order, blank-line spaced.
     * Rename to match your renamed enum constant; keep every comma. Only the
     * LAST entry omits its trailing comma -- the R3 union merge lost exactly
     * one comma here and two names concatenated into a single literal. */
    "r8-guard",             /* GUARD   */

    "r8-cross",             /* CROSS   */

    "r8-city",              /* CITY    */

    "r8-bridge",            /* BRIDGE  */

    "r8-terrain",           /* far-band ground restored + far shore + treeline/snow pages */

    "r8-variety",           /* VARIETY */

    "r8-longbranch",        /* SHAPE   */

    "snow-runs",            /* BIOME   */
    /* [R9] one reserved name per area, in enum order, blank-line spaced.
     * Rename to match your renamed enum constant; keep every comma. Only the
     * LAST entry omits its trailing comma. */
    "rail-yields",          /* RAILFIX */

    "r9-underpass",         /* TUNNEL  */

    "r9-topo",              /* TOPO    */

    "r9-bridge",            /* BRIDGE  */

    "r9-city",              /* CITY    */

    "r9-infra",             /* INFRA   */
    /* [R10] one reserved name per area, in enum order. Rename to match your
     * renamed enum constant. Only the LAST entry omits its trailing comma. */
    "r10-cross",            /* CROSS   */
    /* [R11] one reserved name per area, in enum order. Rename to match your
     * renamed enum constant. Only the LAST entry omits its trailing comma. */
    "r11-city",             /* CITY    */

    "r11-signs",            /* SIGNS   */
    /* [R12] one reserved name per area, in enum order. Rename to match your
     * renamed enum constant. Only the LAST entry omits its trailing comma. */
    "r12-cross",            /* CROSS   */
    /* [R13] one reserved name per area, in enum order. Rename to match your
     * renamed enum constant. Only the LAST entry omits its trailing comma. */
    "r13-fill",             /* FILL    */
    /* [R14] one reserved name per area, in enum order. Rename to match your
     * renamed enum constant. Only the LAST entry omits its trailing comma. */
    "r14-overpass",         /* OVERPASS */
    "r14-coast"             /* COAST   */
};

static TG_AcctRow s_acct_rows[TG_ACCT_SLOTS];

/* [S2h] THREAD SLOT, without __thread.
 *
 * Every per-thread datum in this file is indexed by this. It used to be a
 * __thread int, which on this toolchain is emutls: an accessor call behind a
 * global lock, taken several times per span by every worker. That is what made
 * the threaded terrain pre-pass burn 21x the CPU of the serial one for
 * identical output (69565 ms vs 3329 ms), and it is immune to every other fix
 * because the serialisation is inside the accessor, not the data.
 *
 * Instead: an open-addressed table keyed on the OS thread id, claimed once per
 * thread with a compare-exchange and never released (threads outlive builds).
 * A reader pays a thread-id read -- a thread-control-block load, no lock -- and
 * a probe that hits on the first slot in the steady state. Slot 0 is the
 * fallback if the table is ever full: contended, never wrong. */
static unsigned s_tslot_tid[TG_ACCT_SLOTS];   /* 0 = free */

int tg_tslot(void)
{
    const unsigned tid = td5_plat_thread_id();
    unsigned h = (tid * 2654435761u) % (unsigned)TG_ACCT_SLOTS;
    int i;
    for (i = 0; i < TG_ACCT_SLOTS; i++) {
        const int k = (int)((h + (unsigned)i) % (unsigned)TG_ACCT_SLOTS);
        unsigned cur = s_tslot_tid[k];
        if (cur == tid) return k;
        if (cur == 0) {
#if defined(__GNUC__)
            unsigned expect = 0;
            if (__atomic_compare_exchange_n(&s_tslot_tid[k], &expect, tid, 0,
                                            __ATOMIC_RELAXED, __ATOMIC_RELAXED))
                return k;
            if (expect == tid) return k;   /* lost the race to ourselves */
#else
            s_tslot_tid[k] = tid;
            return k;
#endif
        }
    }
    return 0;
}

/* Kept as the name the counter rows are indexed by. */
static int tg_acct_row(void) { return tg_tslot(); }

/* Sum of every thread's row for one kind. Call only after a join. */
long tg_acct_total(int kind)
{
    long t = 0;
    int i;
    for (i = 0; i < TG_ACCT_SLOTS; i++) t += s_acct_rows[i].c[kind];
    return t;
}

unsigned long long s_acct_mask[TD5_TG_MAX_SPANS][TG_ACCT_MASK_WORDS];

static const char *const k_var_axis[TG_VAR_AXIS_COUNT] = {
    "facade-pages", "banner-pages", "rail-pages", "depth-cells", "store-pages"
};

static int  s_var_id[TG_VAR_AXIS_COUNT][TD5_TG_VAR_SLOTS];

static long s_var_hits[TG_VAR_AXIS_COUNT][TD5_TG_VAR_SLOTS];

static int  s_var_n[TG_VAR_AXIS_COUNT];

static long s_var_spill[TG_VAR_AXIS_COUNT];

void tg_var_note(TG_VarAxis axis, int id)
{
    int i;
    if ((unsigned)axis >= TG_VAR_AXIS_COUNT) return;
    for (i = 0; i < s_var_n[axis]; i++) {
        if (s_var_id[axis][i] == id) { s_var_hits[axis][i]++; return; }
    }
    if (s_var_n[axis] >= TD5_TG_VAR_SLOTS) { s_var_spill[axis]++; return; }
    s_var_id[axis][s_var_n[axis]]   = id;
    s_var_hits[axis][s_var_n[axis]] = 1;
    s_var_n[axis]++;
}

static void tg_var_report(void)
{
    int a, i;
    for (a = 0; a < TG_VAR_AXIS_COUNT; a++) {
        char line[400];
        int pos = 0;
        for (i = 0; i < s_var_n[a] && pos < (int)sizeof(line) - 24; i++)
            pos += snprintf(line + pos, sizeof(line) - (size_t)pos,
                            "%s%d:%ld", pos ? " " : "",
                            s_var_id[a][i], s_var_hits[a][i]);
        TD5_LOG_I(LOG_TAG,
                  "trackgen:   %-13s distinct=%-3d spill=%-4ld  %s",
                  k_var_axis[a], s_var_n[a], s_var_spill[a],
                  pos ? line : "-");
    }
}

const char *const k_rail_class[TG_RAIL_CLASS_COUNT] = {
    "roadside", "deck", "kerb-fence"
};

/* [si][0] = LEFT edge, [si][1] = RIGHT edge. "Left" is lateral +1, the sign
 * convention tg_append_row uses and every one of the three emitters already
 * speaks (see the side loops). */
unsigned char s_rail_edge[TD5_TG_MAX_SPANS][2];

/* [si][side] = 1 where the ROUND-8 roadside emitter would have railed, i.e.
 * every edge that passed all of tg_emit_guardrail's own gates before ownership
 * was considered. This is the baseline for the SAFETY half of the invariant.
 *
 * Why it has to be recorded rather than derived: the first version of this fix
 * reported only the doubling, and "no rail was removed" was then argued from
 * class totals (roadside fell by 726, doubled was 696). That arithmetic hid 30
 * edges which had a roadside rail ALONE and ended up with none, because the
 * roadside yielded to a kerb railing that never ran on a corridor span. A total
 * going DOWN is no more evidence of correct removal than a total going up is of
 * correct placement, so the safety property is now measured directly. */
unsigned char s_rail_would[TD5_TG_MAX_SPANS][2];

void tg_rail_edge_note(TG_RailClass c, int si, double lateral_sign)
{
    if ((unsigned)c >= TG_RAIL_CLASS_COUNT) return;
    if (si < 0 || si >= TD5_TG_MAX_SPANS) return;
    s_rail_edge[si][lateral_sign >= 0.0 ? 0 : 1] |= (unsigned char)(1u << c);
}

void tg_rail_edge_would(int si, double lateral_sign)
{
    if (si < 0 || si >= TD5_TG_MAX_SPANS) return;
    s_rail_would[si][lateral_sign >= 0.0 ? 0 : 1] = 1;
}

void tg_acct_reset(void)
{
    /* Rows only -- slot ASSIGNMENTS persist for the process, so a worker that
     * already owns a row keeps it across builds. */
    memset(s_acct_rows, 0, sizeof(s_acct_rows));
    memset(s_acct_mask,  0, sizeof(s_acct_mask));
    memset(s_rail_edge,  0, sizeof(s_rail_edge));
    memset(s_rail_would, 0, sizeof(s_rail_would));
    memset(s_var_id,    0, sizeof(s_var_id));
    memset(s_var_hits,  0, sizeof(s_var_hits));
    memset(s_var_n,     0, sizeof(s_var_n));
    memset(s_var_spill, 0, sizeof(s_var_spill));
}

/* n elements of `kind` standing at span si. si out of range still COUNTS (the
 * total must stay truthful) but contributes no run -- an emitter placing
 * something off the ring is itself a finding, and silently dropping it would
 * hide it. */
void tg_acct_n(TG_AcctKind kind, int si, int n)
{
    if ((unsigned)kind >= TG_ACCT_KIND_COUNT || n <= 0) return;
    s_acct_rows[tg_acct_row()].c[kind] += n;
    if (si >= 0 && si < TD5_TG_MAX_SPANS)
        TG_ACCT_MASK_SET(si, kind);
}

void tg_acct(TG_AcctKind kind, int si) { tg_acct_n(kind, si, 1); }

/* One element that OCCUPIES spans si0..si1 (a bridge deck, a tunnel bore, a
 * water plane). Counted once -- it is one object -- but marked present across
 * its whole extent so the run list brackets every span you can see it from. */
void tg_acct_range(TG_AcctKind kind, int si0, int si1)
{
    int s;
    if ((unsigned)kind >= TG_ACCT_KIND_COUNT) return;
    if (si1 < si0) { s = si0; si0 = si1; si1 = s; }
    s_acct_rows[tg_acct_row()].c[kind] += 1;
    if (si0 < 0) si0 = 0;
    if (si1 >= TD5_TG_MAX_SPANS) si1 = TD5_TG_MAX_SPANS - 1;
    for (s = si0; s <= si1; s++)
        TG_ACCT_MASK_SET(s, kind);
}

void tg_acct_report(int nspans)
{
    int k;
    if (nspans > TD5_TG_MAX_SPANS) nspans = TD5_TG_MAX_SPANS;
    TD5_LOG_I(LOG_TAG, "trackgen: ---- element inventory (%d spans) ----", nspans);
    for (k = 0; k < TG_ACCT_KIND_COUNT; k++) {
        char runs[240];
        int  pos = 0, nruns = 0, touched = 0, first = -1, last = -1;
        int  s = 0;

        if (tg_acct_total(k) == 0) continue;
        runs[0] = '\0';
        while (s < nspans) {
            int a;
            if (!TG_ACCT_MASK_TEST(s, k)) { s++; continue; }
            a = s;
            while (s < nspans && TG_ACCT_MASK_TEST(s, k)) s++;
            touched += s - a;
            if (first < 0) first = a;
            last = s - 1;
            nruns++;
            if (nruns <= TD5_TG_ACCT_MAX_RUNS && pos < (int)sizeof(runs) - 24) {
                pos += snprintf(runs + pos, sizeof(runs) - (size_t)pos,
                                "%s%d-%d", pos ? "," : "", a, s - 1);
            }
        }
        if (nruns > TD5_TG_ACCT_MAX_RUNS)
            snprintf(runs + pos, sizeof(runs) - (size_t)pos,
                     ",+%d more", nruns - TD5_TG_ACCT_MAX_RUNS);
        TD5_LOG_I(LOG_TAG,
                  "trackgen:   %-14s n=%-6ld spans=%-5d first=%-5d last=%-5d runs[%d]: %s",
                  k_acct_names[k], tg_acct_total(k), touched, first, last,
                  nruns, runs[0] ? runs : "-");
    }
    /* Empty kinds are reported as a single line rather than skipped silently:
     * "trees n=0" is a finding, and a reader who does not see the kind at all
     * cannot tell "none emitted" from "not accounted yet". */
    {
        char none[320];
        int pos = 0;
        for (k = 0; k < TG_ACCT_KIND_COUNT; k++) {
            if (tg_acct_total(k) != 0) continue;
            if (pos < (int)sizeof(none) - 20)
                pos += snprintf(none + pos, sizeof(none) - (size_t)pos,
                                "%s%s", pos ? " " : "", k_acct_names[k]);
        }
        if (pos)
            TD5_LOG_I(LOG_TAG, "trackgen:   NONE emitted: %s", none);
    }
    tg_var_report();
    TD5_LOG_I(LOG_TAG, "trackgen: ---- end inventory ----");
}

/* ==========================================================================
 * TIME OF DAY  (feedback R2 item 22)
 *
 * Night is a property of the RACE, not of a span, a biome or a texture page, so
 * it is decided ONCE when the race is entered (td5_trackgen_regenerate, which is
 * what a race launch calls) and latched. Emitters that need it -- street lamps
 * only lighting up at night, headlights, sky choice -- read the latch through
 * td5_trackgen_is_night() instead of each rolling its own predicate, which is
 * how a track ends up with lit lamps under a noon sky.
 *
 * Latching also makes it stable across the build: tg_emit_models runs thousands
 * of times per track and a predicate that re-rolled per call would light every
 * other lamp.
 *
 * TD5RE_AUTOTRACK_NIGHT: 0 = always day, 1 = always night, 2 = decide from the
 * seed (default). Seed-derived keeps a given seed reproducible -- the same seed
 * is the same track at the same time of day, which the whole generator relies on.
 * ========================================================================== */
int s_is_night = 0;

/* The decision itself, so tg_decide_night (which latches it for the build) and
 * the [R21 ROLLS] registry (which only REPORTS it, and must agree) cannot
 * drift apart. mode 0/1 = pinned day/night, 2 = derive from the seed. */
static int tg_night_for(unsigned int seed, int mode)
{
    if (mode < 2) return mode;
    /* Knuth multiplicative hash of the seed, high bit. ~1 in 4 night, which
     * is roughly the shipped TD5 ratio (5 of the 19 schedule tracks run at
     * night or dusk) rather than a coin flip. */
    return ((seed * 2654435761u) >> 29) == 0 ? 1 : 0;
}

static void tg_decide_night(unsigned int seed)
{
    int mode = td5_env_int("TD5RE_AUTOTRACK_NIGHT", 2, 0, 2);
    s_is_night = tg_night_for(seed, mode);
    TD5_LOG_I(LOG_TAG, "trackgen: time of day = %s (seed=%u knob=%d)",
              s_is_night ? "NIGHT" : "DAY", seed, mode);
}

int td5_trackgen_is_night(void) { return s_is_night; }

/* ================== [R21 ROLLS] SEED-DERIVED PARAMETER REGISTRY ============
 *
 * Placed ABOVE tg_rand() ON PURPOSE. A roll must never consume the geometry
 * RNG stream, and the only mechanical guarantee of that is being unable to
 * call it. Rolls HASH instead (tg_roll_hash), exactly as the biome layout does
 * and for the same reason: one extra tg_rand() draw shifts every later draw
 * and so moves the road for every existing seed.
 *
 * SALT CONVENTION. Each entry owns a hand-picked salt 0x2101xxxx for R21
 * (0x2201xxxx for R22, ...), low 16 bits incrementing. A roll is keyed by its
 * SALT ALONE -- there is deliberately no table-index term -- so entries may be
 * appended, reordered or retired without moving any other entry's roll. Two
 * rules follow, and both matter:
 *   1. A salt is NEVER reused and NEVER renumbered. Retire an entry by nulling
 *      its name, not by compacting the table.
 *   2. Two entries sharing a salt correlate silently and forever, which is the
 *      one mistake here that destroys variety without failing anything. The
 *      duplicate-salt scan in tg_rolls_resolve turns it into a log line.
 *
 * STYLE vs PRESENCE. Every choice of a STYLE entry is a legitimate look, so it
 * carries real weights. A PRESENCE entry has a choice that REMOVES content
 * (guardrails, sidewalks, scenery): a seed that rolled several of those off at
 * once reads as broken rather than varied, so those ship weighted to today's
 * value. RANDOM stays their default and the mechanism stays live and reported,
 * so a later round tunes one byte instead of re-plumbing. All five entries
 * below are STYLE.
 *
 * PINNED VALUES ARE NOT SNAPPED TO THE TABLE. tg_rolls_apply_spec writes only
 * the entries the knobs did NOT pin, so a pinned knob keeps exactly whatever
 * td5_trackgen_apply_config already computed for it -- including out-of-table
 * dev values like TD5RE_AUTOTRACK_CURVESAFE=250. The table index is then only
 * used to LABEL it. That is also why the master knob off is byte-identical:
 * apply_spec writes nothing at all. */

typedef struct {
    const char          *name;    /* log/UI facing                            */
    const char          *knob;    /* NULL = composite, resolved specially     */
    unsigned int         salt;
    const int           *vals;    /* choice literals                          */
    const char *const   *cnames;
    const unsigned char *w;       /* relative weights; NULL = uniform         */
    short                n;
    short                legacy;  /* choice index matching PRE-R21 unset      */
    int                  lo, hi;  /* clamp for a pinned value (display only)  */
    /* [R22] PRESENCE rank, 1 = restore first, 0 = not a presence entry. Rows
     * that omit it are zero-initialised by C, so only the presence rows carry
     * one. See the presence budget in td5_trackgen_resolve_rolls. */
    unsigned char        pres_rank;
} TG_RollEntry;

/* Choice sets. These MUST match the studio's tables in td5_fe_race.c -- the
 * arity check in Screen_AutoTrackOptions asserts it, because a silent drift
 * here shows the player one thing and builds another. */
static const int         k_tgr_twist_v[] = { 0, 1, 2, 3 };
static const char *const k_tgr_twist_n[] = { "GENTLE", "BALANCED", "TWISTY",
                                             "EXTREME" };
static const unsigned char k_tgr_twist_w[] = { 20, 35, 30, 15 };
/* straight, curve, acute -- index 1 is the generator's shipped 35/40/15. */
static const int k_tgr_twist_mix[4][3] = {
    { 60, 35,  5 }, { 35, 40, 15 }, { 20, 45, 35 }, { 10, 40, 50 }
};

static const int         k_tgr_corner_v[] = { 120, 150, 180, 240, 320 };
static const char *const k_tgr_corner_n[] = { "TIGHT", "NARROW", "STANDARD",
                                              "WIDE", "SWEEPING" };
static const unsigned char k_tgr_corner_w[] = { 15, 20, 30, 20, 15 };

/* GRADIENT is weighted UP deliberately. Measured on seed 5150: the row is a
 * CAP, and STANDARD -> SEVERE left the height RANGE identical at 34007 because
 * nothing drives slope toward the cap. [R21 GRADE] adds the drive; weighting
 * the low choices down is what makes "steep climbs" the common case. FLAT
 * keeps a small share because a genuinely flat track is a valid look. */
static const int         k_tgr_grade_v[] = { 0, 60, 120, 160, 200 };
static const char *const k_tgr_grade_n[] = { "FLAT", "GENTLE", "STANDARD",
                                             "STEEP", "SEVERE" };
static const unsigned char k_tgr_grade_w[] = { 5, 15, 30, 30, 20 };

static const int         k_tgr_dual_v[] = { 0, 5, 10, 20, 35 };
static const char *const k_tgr_dual_n[] = { "NONE", "RARE", "SOME", "OFTEN",
                                            "CONSTANT" };
static const unsigned char k_tgr_dual_w[] = { 10, 25, 30, 25, 10 };

static const int         k_tgr_hills_v[] = { 0, 3000, 6000, 12000, 20000 };
static const char *const k_tgr_hills_n[] = { "FLAT", "LOW", "MEDIUM", "HIGH",
                                             "EXTREME" };
static const unsigned char k_tgr_hills_w[] = { 5, 15, 30, 30, 20 };

static const int         k_tgr_night_v[] = { 0, 1 };
static const char *const k_tgr_night_n[] = { "DAY", "NIGHT" };

/* LENGTH is weighted away from both ends: a rolled 3000-span MARATHON triples
 * build time on a row the player never touched, and 600 is barely a lap. */
static const int         k_tgr_len_v[] = { 600, 1200, 1800, 2400, 3000 };
static const char *const k_tgr_len_n[] = { "SHORT", "MEDIUM", "LONG",
                                           "VERY LONG", "MARATHON" };
static const unsigned char k_tgr_len_w[] = { 0, 15, 70, 15, 0 };

/* How often a section changes its lane count -- the existing per-section
 * narrower/wider-lane machinery ([LANES] in tg_build_centerline). This is the
 * "sections with narrower lanes and wider lanes" dial. */
static const int         k_tgr_lanepct_v[] = { 0, 15, 35, 60, 85 };
static const char *const k_tgr_lanepct_n[] = { "UNIFORM", "RARE", "SOME",
                                               "OFTEN", "CONSTANT" };
static const unsigned char k_tgr_lanepct_w[] = { 5, 20, 35, 30, 10 };

static const int         k_tgr_runoff_v[] = { 0, 50, 100, 200, 400 };
static const char *const k_tgr_runoff_n[] = { "NONE", "SHORT", "STANDARD",
                                              "LONG", "VERY LONG" };
static const unsigned char k_tgr_runoff_w[] = { 10, 20, 35, 25, 10 };

static const int         k_tgr_reach_v[] = { 30000, 80000, 160000, 300000 };
static const char *const k_tgr_reach_n[] = { "NEAR", "MEDIUM", "FAR",
                                             "VERY FAR" };
static const unsigned char k_tgr_reach_w[] = { 40, 30, 20, 10 };

static const int         k_tgr_blend_v[] = { 0, 10, 20, 40 };
static const char *const k_tgr_blend_n[] = { "SHARP", "SHORT", "NORMAL",
                                             "LONG" };
static const unsigned char k_tgr_blend_w[] = { 10, 25, 40, 25 };

static const int         k_tgr_rail_v[] = { 20, 35, 50, 90, 160 };
static const char *const k_tgr_rail_n[] = { "EVERYWHERE", "FREQUENT",
                                            "STANDARD", "SPARSE",
                                            "BENDS ONLY" };
static const unsigned char k_tgr_rail_w[] = { 15, 20, 30, 20, 15 };

static const int         k_tgr_sky_v[] = { -1, 12, 24, 36, 48, 60 };
static const char *const k_tgr_sky_n[] = { "NONE", "12", "24", "36", "48",
                                           "60" };
static const unsigned char k_tgr_sky_w[] = { 5, 15, 20, 25, 20, 15 };

/* Shared boolean choice set. */
static const int         k_tgr_bool_v[] = { 0, 1 };
static const char *const k_tgr_bool_n[] = { "OFF", "ON" };
/* PRESENCE: the OFF choice removes content.
 *
 * [R22] R21 shipped every presence entry on k_tgr_w_keep = { 0, 100 }, i.e.
 * weighted to today's value, and the round doc called that "a later round tunes
 * one byte". This is that round: the rows were reported as only PRETENDING to
 * be random, which is exactly what a 0/100 weight is. They now carry real
 * weights.
 *
 * The reason R21 hedged is still true -- a seed that rolled guardrails,
 * sidewalks and scenery off at once reads as broken rather than varied -- so
 * the hedge moved from the weights to a BUDGET (the presence budget in
 * td5_trackgen_resolve_rolls). Two mechanisms, each doing one job: the weight
 * says how often this row is absent, the budget says how much can be absent at
 * once. Weighting alone cannot express the second: 23 independent 15% rolls
 * still puts several seeds a year at eight things missing simultaneously, and
 * those are the seeds a player would report as a broken generator.
 *
 * OFF rates are graded by how much a row removes, not by taste: k_tgr_w_scarce
 * for the rows that take away a whole layer of the world, k_tgr_w_mostly_on for
 * a feature, k_tgr_w_even for a detail nobody would call missing. */
static const unsigned char k_tgr_w_scarce[]    = { 12,  88 };
static const unsigned char k_tgr_w_mostly_on[] = { 25,  75 };
static const unsigned char k_tgr_w_even[]      = { 50,  50 };
static const unsigned char k_tgr_w_rare[]      = { 70,  30 };

#define TGR_BOOL(nm, kb, slt, wts, leg) \
    { nm, kb, slt, k_tgr_bool_v, k_tgr_bool_n, wts, 2, leg, 0, 1, 0 }
/* A presence row: same shape plus its restore rank for the budget. */
#define TGR_PRES(nm, kb, slt, wts, leg, rank) \
    { nm, kb, slt, k_tgr_bool_v, k_tgr_bool_n, wts, 2, leg, 0, 1, rank }

/* MUST be in TD5_TgRollId order -- the array is indexed by the id. Salts are
 * independent of position, so the six original entries keep theirs and their
 * rolls are unchanged by everything appended after them. */
static const TG_RollEntry k_tg_rolls[TD5_TG_ROLL_COUNT] = {
 /* name          knob                          salt         vals/names/weights                              n  leg  lo   hi   */
 { "TWISTINESS",  NULL,                         0x21010001u, k_tgr_twist_v,   k_tgr_twist_n,   k_tgr_twist_w,   4, 1, 0, 3 , 0 },
 { "CORNERS",     "TD5RE_AUTOTRACK_CURVESAFE",  0x21010002u, k_tgr_corner_v,  k_tgr_corner_n,  k_tgr_corner_w,  5, 2, 100, 800 , 0 },
 { "GRADIENT",    "TD5RE_AUTOTRACK_GRADE",      0x21010003u, k_tgr_grade_v,   k_tgr_grade_n,   k_tgr_grade_w,   5, 2, 0, 200 , 0 },
 { "DUAL LANES",  "TD5RE_AUTOTRACK_PCT_DUAL",   0x21010004u, k_tgr_dual_v,    k_tgr_dual_n,    k_tgr_dual_w,    5, 2, 0, 100 , 0 },
 { "HILLS",       "TD5RE_AUTOTRACK_ELEVATION",  0x21010005u, k_tgr_hills_v,   k_tgr_hills_n,   k_tgr_hills_w,   5, 2, 0, 40000 , 0 },
 { "LENGTH",      "TD5RE_AUTOTRACK_SPANS",      0x21010007u, k_tgr_len_v,     k_tgr_len_n,     k_tgr_len_w,     5, 2, 200, 3000 , 0 },
 { "TIME OF DAY", "TD5RE_AUTOTRACK_NIGHT",      0x21010006u, k_tgr_night_v,   k_tgr_night_n,   NULL,            2, 0, 0, 1 , 0 },
 { "LANE VARIETY","TD5RE_AUTOTRACK_LANE_PCT",   0x21010008u, k_tgr_lanepct_v, k_tgr_lanepct_n, k_tgr_lanepct_w, 5, 2, 0, 100 , 0 },
 { "RUN-OFF",     "TD5RE_AUTOTRACK_RUNOFF",     0x21010009u, k_tgr_runoff_v,  k_tgr_runoff_n,  k_tgr_runoff_w,  5, 2, 0, 4000 , 0 },
 { "VIEW REACH",  "TD5RE_AUTOTRACK_TERRAIN_REACH",0x2101000Au,k_tgr_reach_v,  k_tgr_reach_n,   k_tgr_reach_w,   4, 0, 1000, 400000 , 0 },
 { "TRANSITIONS", "TD5RE_AUTOTRACK_BIOME_BLEND",0x2101000Bu, k_tgr_blend_v,   k_tgr_blend_n,   k_tgr_blend_w,   4, 2, 0, 75 , 0 },
 { "RAIL DENSITY","TD5RE_AUTOTRACK_RAIL_DEG10", 0x2101000Cu, k_tgr_rail_v,    k_tgr_rail_n,    k_tgr_rail_w,    5, 2, 0, 3600 , 0 },
 { "SKY",         "TD5RE_AUTOTRACK_SKY_ANIM",   0x2101000Du, k_tgr_sky_v,     k_tgr_sky_n,     k_tgr_sky_w,     6, 3, -1, 240 , 0 },
 TGR_BOOL("SNOW",          "TD5RE_AUTOTRACK_SNOW",           0x2101000Eu, k_tgr_w_rare,      1),
 TGR_BOOL("PARKS",         "TD5RE_AUTOTRACK_PARKS",          0x2101000Fu, k_tgr_w_even,      0),
 TGR_BOOL("PARK HOUSES",   "TD5RE_AUTOTRACK_PARK_HOUSES",    0x21010010u, k_tgr_w_mostly_on, 1),
 TGR_BOOL("PARK HEDGES",   "TD5RE_AUTOTRACK_PARK_HEDGE",     0x21010011u, k_tgr_w_mostly_on, 1),
 TGR_BOOL("AVENUES",       "TD5RE_AUTOTRACK_AVENUE_DIVIDER", 0x21010012u, k_tgr_w_mostly_on, 1),
 TGR_BOOL("START IN TOWN", "TD5RE_AUTOTRACK_START_CITY",     0x21010013u, k_tgr_w_even,      1),
 TGR_BOOL("MOUNTAINS",     "TD5RE_AUTOTRACK_TUNNEL_MOUNTAIN",0x21010014u, k_tgr_w_mostly_on, 1),
 TGR_BOOL("TUNNEL LAMPS",  "TD5RE_AUTOTRACK_TUNNEL_LAMPS",   0x21010015u, k_tgr_w_mostly_on, 1),
 TGR_BOOL("BRIDGE STYLE",  "TD5RE_AUTOTRACK_BRIDGE_VARIETY", 0x21010016u, k_tgr_w_mostly_on, 1),
 TGR_BOOL("CLEAR VERGES",  "TD5RE_AUTOTRACK_FLORA_CLEAR",    0x21010017u, k_tgr_w_mostly_on, 1),
 TGR_BOOL("MIRROR TREES",  "TD5RE_AUTOTRACK_TREE_MIRROR",    0x21010018u, k_tgr_w_even,      1),
 /* ---- PRESENCE from here down ------------------------------------------
  * [R22] Real weights + a restore RANK. Rank 1 is restored first when a seed
  * busts the presence budget, so the ordering is "what would a player miss
  * most", not the table order. Ranks are unique so the restore is a total
  * order and therefore deterministic. Salts are untouched: an entry's roll is
  * a pure function of seed and salt, so re-weighting changes WHICH seeds turn
  * a row off without renumbering anything. */
 TGR_PRES("BRANCHES",      "TD5RE_AUTOTRACK_BRANCHES",       0x21010019u, k_tgr_w_mostly_on, 1, 23),
 TGR_PRES("TERRAIN",       "TD5RE_AUTOTRACK_TERRAIN_HILLS",  0x2101001Au, k_tgr_w_scarce,    1,  2),
 TGR_PRES("BACKDROP",      "TD5RE_AUTOTRACK_TERRAIN_FAR",    0x2101001Bu, k_tgr_w_scarce,    1,  3),
 TGR_PRES("COASTLINE",     "TD5RE_AUTOTRACK_COASTLINE",      0x2101001Cu, k_tgr_w_mostly_on, 1, 22),
 /* BRIDGES/TUNNELS are scarce for a reason beyond looks: with the topology
  * generator the structures are DETECTED from terrain, so a water-heavy seed
  * with bridges off falls back to causeways and forced conforms. */
 TGR_PRES("BRIDGES",       "TD5RE_AUTOTRACK_BRIDGES",        0x2101001Du, k_tgr_w_scarce,    1, 10),
 TGR_PRES("OVERHEADS",     "TD5RE_AUTOTRACK_BRIDGE_OVERHEAD",0x2101001Eu, k_tgr_w_even,      1, 19),
 TGR_PRES("TUNNELS",       "TD5RE_AUTOTRACK_TUNNELS",        0x2101001Fu, k_tgr_w_scarce,    1, 11),
 TGR_PRES("GUARDRAILS",    "TD5RE_AUTOTRACK_GUARDRAILS",     0x21010020u, k_tgr_w_scarce,    1,  6),
 TGR_PRES("ARMCO",         "TD5RE_AUTOTRACK_ARMCO",          0x21010021u, k_tgr_w_even,      1, 18),
 TGR_PRES("DISTRICTS",     "TD5RE_AUTOTRACK_DISTRICTS",      0x21010022u, k_tgr_w_scarce,    1,  7),
 TGR_PRES("BUILDING MASS", "TD5RE_AUTOTRACK_FACADE_MASS",    0x21010023u, k_tgr_w_scarce,    1,  8),
 TGR_PRES("BACK ROWS",     "TD5RE_AUTOTRACK_BACKROWS",       0x21010024u, k_tgr_w_mostly_on, 1, 15),
 TGR_PRES("CROSSINGS",     "TD5RE_AUTOTRACK_CROSSINGS",      0x21010025u, k_tgr_w_mostly_on, 1, 12),
 TGR_PRES("SIDE STREETS",  "TD5RE_AUTOTRACK_CROSS_STREETS",  0x21010026u, k_tgr_w_mostly_on, 1, 13),
 TGR_PRES("ROAD MARKS",    "TD5RE_AUTOTRACK_CROSS_MARKINGS", 0x21010027u, k_tgr_w_mostly_on, 1, 16),
 TGR_PRES("INTERSECTIONS", "TD5RE_AUTOTRACK_INTERSECTIONS",  0x21010028u, k_tgr_w_mostly_on, 1, 14),
 TGR_PRES("SIDEWALKS",     "TD5RE_AUTOTRACK_SIDEWALKS",      0x21010029u, k_tgr_w_scarce,    1,  5),
 TGR_PRES("SCENERY",       "TD5RE_AUTOTRACK_SCENERY",        0x2101002Au, k_tgr_w_scarce,    1,  1),
 TGR_PRES("TREE LINE",     "TD5RE_AUTOTRACK_TREELINE",       0x2101002Bu, k_tgr_w_mostly_on, 1,  9),
 TGR_PRES("LAMP POSTS",    "TD5RE_AUTOTRACK_LAMP_POSTS",     0x2101002Cu, k_tgr_w_even,      1, 17),
 TGR_PRES("BANNERS",       "TD5RE_AUTOTRACK_BANNERS",        0x2101002Du, k_tgr_w_even,      1, 20),
 TGR_PRES("REAL TEXTURES", "TD5RE_AUTOTRACK_REAL_TEX",       0x2101002Eu, k_tgr_w_scarce,    1,  4),
 TGR_PRES("REAL FURNITURE","TD5RE_AUTOTRACK_REAL_FURNITURE", 0x2101002Fu, k_tgr_w_mostly_on, 1, 21)
};

/* The table is indexed by TD5_TgRollId, so a missing or extra row would
 * silently shift every entry past it onto the wrong knob. */
typedef char tg_assert_rolls_len[
    (sizeof(k_tg_rolls) / sizeof(k_tg_rolls[0]) == TD5_TG_ROLL_COUNT) ? 1 : -1];

static TD5_TgRolls s_rolls;      /* latched for the build, like s_is_night */
static int         s_rolls_valid = 0;

int tg_rolls_enabled(void) { return td5_env_flag_on("TD5RE_R21_ROLL"); }

/* Sibling of tg_biome_hash with its OWN salt namespace and, deliberately, no
 * index term -- see the salt convention above. */
unsigned int tg_roll_hash(unsigned int seed, unsigned int salt)
{
    unsigned int h = seed ^ (salt * 0x9E3779B9u);
    h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12; h ^= h >> 16;
    return h;
}

/* For decisions made DURING the walk, where the POSITION is the key. */
unsigned int tg_roll_hash_at(unsigned int salt, int index)
{
    unsigned int h = tg_roll_hash(s_gen_seed, salt);
    h += (unsigned int)index * 2654435761u;
    h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12;
    return h;
}

/* Weighted choice from a hash. Uses the HIGH bits: the low bit of a
 * multiply-xor mix is its weakest. */
int tg_roll_pick_w(unsigned int h, const unsigned char *w, int n)
{
    unsigned int sum = 0, r;
    int i;

    if (n <= 1) return 0;
    if (!w) return (int)((h >> 8) % (unsigned int)n);
    for (i = 0; i < n; i++) sum += w[i];
    if (!sum) return 0;                  /* degenerate table -> first choice */
    r = (h >> 8) % sum;
    for (i = 0; i < n; i++) {
        if (r < (unsigned int)w[i]) return i;
        r -= (unsigned int)w[i];
    }
    return n - 1;
}

/* Nearest choice index to `v`, for LABELLING a pinned value that need not be
 * a table member (CURVESAFE=250 is legal and pins 250; it just displays as
 * the closest named choice). */
static int tg_roll_nearest(const TG_RollEntry *e, int v)
{
    int i, best = 0, bd = -1;
    for (i = 0; i < e->n; i++) {
        int d = e->vals[i] > v ? e->vals[i] - v : v - e->vals[i];
        if (bd < 0 || d < bd) { bd = d; best = i; }
    }
    return best;
}

/* Did a knob pin this entry, and to what? Raw getenv rather than
 * td5_env_int_opt because that helper's contract needs the sentinel to sit
 * below `lo`, and TD5_TG_ROLL_RANDOM must survive the parse so a dev can write
 * -2 on a command line to mean "roll it". */
static int tg_roll_pin_of(const TG_RollEntry *e, int *out)
{
    const char *s;
    int v;

    if (!e->knob) {                      /* TWISTINESS: three PCT_* knobs */
        const char *st = getenv("TD5RE_AUTOTRACK_PCT_STRAIGHT");
        const char *cu = getenv("TD5RE_AUTOTRACK_PCT_CURVE");
        const char *ac = getenv("TD5RE_AUTOTRACK_PCT_ACUTE");
        int i;
        if ((!st || !st[0]) && (!cu || !cu[0]) && (!ac || !ac[0])) return 0;
        /* Reverse-map the mix so the report names what was pinned. */
        for (i = 0; i < 4; i++) {
            if (st && atoi(st) == k_tgr_twist_mix[i][0] &&
                ac && atoi(ac) == k_tgr_twist_mix[i][2]) { *out = i; return 1; }
        }
        *out = e->legacy;                /* pinned to a mix we do not name */
        return 1;
    }
    s = getenv(e->knob);
    if (!s || !s[0]) return 0;                       /* unset  == RANDOM */
    v = atoi(s);
    if (v == TD5_TG_ROLL_RANDOM) return 0;           /* explicit RANDOM  */
    /* TIME OF DAY keeps its long-standing 2 = RANDOM spelling. */
    if (e->knob && !strcmp(e->knob, "TD5RE_AUTOTRACK_NIGHT")) {
        if (v >= 2) return 0;
        *out = v; return 1;
    }
    if (v < e->lo) v = e->lo;
    if (v > e->hi) v = e->hi;
    *out = v;
    return 1;
}

void td5_trackgen_resolve_rolls(unsigned int seed, TD5_TgRolls *out)
{
    const int on = tg_rolls_enabled();
    int i;

    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->seed = seed;

    for (i = 0; i < TD5_TG_ROLL_COUNT; i++) {
        const TG_RollEntry *e = &k_tg_rolls[i];
        int pinned = 0, v = 0;

        if (!e->name) continue;                      /* retired slot */
        pinned = tg_roll_pin_of(e, &v);
        if (pinned) {
            out->pinned[i] = 1;
            /* TWISTINESS and TIME OF DAY pin a CHOICE; the rest pin a value. */
            if (!e->knob || i == TD5_TG_ROLL_NIGHT) {
                out->choice[i] = (unsigned char)v;
                out->value[i]  = e->vals[v < e->n ? v : 0];
            } else {
                out->value[i]  = v;
                out->choice[i] = (unsigned char)tg_roll_nearest(e, v);
            }
        } else if (!on) {
            out->choice[i] = (unsigned char)e->legacy;
            out->value[i]  = e->vals[e->legacy];
        } else {
            int c = tg_roll_pick_w(tg_roll_hash(seed, e->salt), e->w, e->n);
            out->choice[i] = (unsigned char)c;
            out->value[i]  = e->vals[c];
        }
    }
    /* [R22] PRESENCE BUDGET -- the hedge that lets the presence weights above
     * be honest. Each presence row now really rolls, so a seed can legitimately
     * turn several layers of the world off at once; past a few simultaneous
     * absences a track stops reading as "varied" and starts reading as "the
     * generator is broken", which is the complaint R21 avoided by not rolling
     * these rows at all.
     *
     * Restores by RANK, not by seed: given the same set of OFF rows the same
     * ones come back, so the budget cannot itself become a source of variance.
     * Runs INSIDE the resolver, which is the only place it can run -- the
     * resolver is what both the studio display and the build consume, so a
     * budget applied anywhere else would let the screen promise a row the
     * track does not have.
     *
     * PINNED rows are counted but never restored: if a player switched six
     * things off by hand that is the track they asked for. They are counted so
     * their absences still consume the budget, and therefore still hold rolled
     * rows on -- otherwise pinning three things off and rolling four more would
     * produce the very seven-holes-at-once track this exists to prevent.
     *
     * THE CAP IS MEASURED, NOT PICKED. Over 20000 seeds the raw absence count
     * averages 5.45 (sd ~1.9), so the cap decides whether this is a tail guard
     * or a second pinning mechanism:
     *
     *   cap   budget binds   a rank<=8 row actually stays OFF
     *    4        67.8%                18.2%
     *    6        28.3%                46.6%
     *    8         6.2%                60.9%
     *   10         0.7%                63.5%
     *
     * At the 4 this first shipped with, the budget fired on two thirds of all
     * seeds and -- restoring lowest rank first -- held the structural rows ON
     * almost always, which is exactly the "only pretending to be random"
     * behaviour R22 exists to remove. 8 fires on 6% of seeds and captures
     * nearly all the available variety; past 8 the curve is flat (60.9 -> 63.5)
     * while the number of simultaneous holes keeps growing, so 8 is the knee.
     * Re-derive with the offline model if the weights above ever move. */
    if (on) {
        const int cap = td5_env_int("TD5RE_R22_PRESENCE_OFF_MAX", 8, 0, 64);
        int off_n = 0;

        for (i = 0; i < TD5_TG_ROLL_COUNT; i++) {
            const TG_RollEntry *e = &k_tg_rolls[i];
            if (!e->name || !e->pres_rank) continue;
            if (out->value[i] == 0) off_n++;
        }
        while (off_n > cap) {
            int best = -1, best_rank = 0;
            for (i = 0; i < TD5_TG_ROLL_COUNT; i++) {
                const TG_RollEntry *e = &k_tg_rolls[i];
                if (!e->name || !e->pres_rank) continue;
                if (out->pinned[i] || out->value[i] != 0) continue;
                if (best < 0 || e->pres_rank < best_rank) {
                    best = i; best_rank = e->pres_rank;
                }
            }
            if (best < 0) break;   /* every remaining absence is a human's pin */
            out->choice  [best] = 1;
            out->value   [best] = k_tg_rolls[best].vals[1];
            out->restored[best] = 1;
            off_n--;
        }
    }

    /* TIME OF DAY is owned by tg_decide_night; mirror its answer so the report
     * cannot contradict the build. */
    {
        int mode = td5_env_int("TD5RE_AUTOTRACK_NIGHT", 2, 0, 2);
        out->value [TD5_TG_ROLL_NIGHT] = tg_night_for(seed, mode);
        out->choice[TD5_TG_ROLL_NIGHT] =
            (unsigned char)out->value[TD5_TG_ROLL_NIGHT];
        out->pinned[TD5_TG_ROLL_NIGHT] = (unsigned char)(mode < 2);
    }
}

/* ---- environment publishing ------------------------------------------------
 * ~40 of these knobs are read by td5_env_* calls scattered across the eight
 * generator modules. Rather than edit 40 call sites (and get one of them
 * wrong), the registry PUBLISHES each resolved value into the environment, so
 * every existing read picks it up unchanged. The studio already writes its
 * rows the same way (_putenv_s in at_setenv), so this is the established
 * spelling in this codebase, not a new mechanism.
 *
 * OWNERSHIP IS THE WHOLE TRICK. A published value is indistinguishable from a
 * human's pin by the time getenv sees it, so the registry records what it
 * wrote and GIVES IT BACK before the next resolve. Without that, the first
 * build's roll would look pinned forever after and every later build would
 * reproduce it -- and the studio row would stop showing RANDOM after one race.
 * td5_trackgen_roll_is_owned is how the studio tells the two apart. */
static unsigned char s_roll_owned[TD5_TG_ROLL_COUNT];

void tg_rolls_unpublish(void)
{
    int i;
    for (i = 0; i < TD5_TG_ROLL_COUNT; i++) {
        if (!s_roll_owned[i]) continue;
        s_roll_owned[i] = 0;
        if (k_tg_rolls[i].knob) {
            _putenv_s(k_tg_rolls[i].knob, "");
        } else if (i == TD5_TG_ROLL_TWIST) {
            _putenv_s("TD5RE_AUTOTRACK_PCT_STRAIGHT", "");
            _putenv_s("TD5RE_AUTOTRACK_PCT_CURVE", "");
            _putenv_s("TD5RE_AUTOTRACK_PCT_ACUTE", "");
        }
    }
}

static void tg_rolls_publish(void)
{
    char buf[24];
    int i;

    if (!tg_rolls_enabled()) return;
    for (i = 0; i < TD5_TG_ROLL_COUNT; i++) {
        const TG_RollEntry *e = &k_tg_rolls[i];
        if (!e->name || s_rolls.pinned[i]) continue;
        /* TIME OF DAY is the one exception: tg_decide_night owns it and reads
         * its knob as a tri-state where 2 means "roll it". Publishing 0 or 1
         * would read back as a PIN on the next build and freeze the time of
         * day for good. */
        if (i == TD5_TG_ROLL_NIGHT) continue;
        if (e->knob) {
            snprintf(buf, sizeof(buf), "%d", s_rolls.value[i]);
            _putenv_s(e->knob, buf);
            s_roll_owned[i] = 1;
        } else if (i == TD5_TG_ROLL_TWIST) {
            int mix[3];
            td5_trackgen_twist_mix(s_rolls.choice[i], mix);
            snprintf(buf, sizeof(buf), "%d", mix[0]);
            _putenv_s("TD5RE_AUTOTRACK_PCT_STRAIGHT", buf);
            snprintf(buf, sizeof(buf), "%d", mix[1]);
            _putenv_s("TD5RE_AUTOTRACK_PCT_CURVE", buf);
            snprintf(buf, sizeof(buf), "%d", mix[2]);
            _putenv_s("TD5RE_AUTOTRACK_PCT_ACUTE", buf);
            s_roll_owned[i] = 1;
        }
    }
}

const char *td5_trackgen_roll_knob(int id)
{
    if (id < 0 || id >= TD5_TG_ROLL_COUNT) return NULL;
    return k_tg_rolls[id].knob;
}

int td5_trackgen_roll_is_owned(int id)
{
    if (id < 0 || id >= TD5_TG_ROLL_COUNT) return 0;
    return s_roll_owned[id] ? 1 : 0;
}

void td5_trackgen_roll_disown(int id)
{
    if (id < 0 || id >= TD5_TG_ROLL_COUNT) return;
    s_roll_owned[id] = 0;
}

int td5_trackgen_roll_is_pinned_now(int id)
{
    int v = 0;
    if (id < 0 || id >= TD5_TG_ROLL_COUNT || !k_tg_rolls[id].name) return 0;
    return tg_roll_pin_of(&k_tg_rolls[id], &v);
}

/* Defined below, next to the mood table; latched here so the rolls and the
 * mood always share a lifetime. */
static void tg_mood_resolve(unsigned int seed);

void tg_rolls_resolve(unsigned int seed)
{
    int i, j;

    /* Give back last build's published values FIRST, so the only thing left in
     * the environment is what a human or a script actually pinned. */
    tg_rolls_unpublish();

    td5_trackgen_resolve_rolls(seed, &s_rolls);
    s_rolls_valid = 1;
    tg_rolls_publish();
    tg_mood_resolve(seed);   /* latched alongside the rolls, same lifetime */

    /* One-shot duplicate-salt scan: two entries on one salt correlate forever
     * and nothing else would ever notice. */
    for (i = 0; i < TD5_TG_ROLL_COUNT; i++) {
        if (!k_tg_rolls[i].name) continue;
        for (j = i + 1; j < TD5_TG_ROLL_COUNT; j++) {
            if (!k_tg_rolls[j].name) continue;
            if (k_tg_rolls[i].salt == k_tg_rolls[j].salt) {
                TD5_LOG_E(LOG_TAG, "trackgen: [R21 ROLL] DUPLICATE SALT %#x on "
                          "'%s' and '%s' -- these two parameters will always "
                          "roll together", k_tg_rolls[i].salt,
                          k_tg_rolls[i].name, k_tg_rolls[j].name);
            }
        }
    }
}

int tg_roll_value(int id)
{
    if (!s_rolls_valid || id < 0 || id >= TD5_TG_ROLL_COUNT) return 0;
    return s_rolls.value[id];
}

int tg_roll_choice(int id)
{
    if (!s_rolls_valid || id < 0 || id >= TD5_TG_ROLL_COUNT) return 0;
    return s_rolls.choice[id];
}

int td5_trackgen_roll_choice_count(int id)
{
    if (id < 0 || id >= TD5_TG_ROLL_COUNT || !k_tg_rolls[id].name) return 0;
    return k_tg_rolls[id].n;
}

const char *td5_trackgen_roll_choice_name(int id, int choice)
{
    const TG_RollEntry *e;
    if (id < 0 || id >= TD5_TG_ROLL_COUNT || !k_tg_rolls[id].name) return "?";
    e = &k_tg_rolls[id];
    if (choice < 0 || choice >= e->n) return "?";
    return e->cnames[choice];
}

const char *td5_trackgen_roll_name(int id)
{
    if (id < 0 || id >= TD5_TG_ROLL_COUNT || !k_tg_rolls[id].name) return "?";
    return k_tg_rolls[id].name;
}

int td5_trackgen_twist_mix(int choice, int out3[3])
{
    if (!out3) return 0;
    if (choice < 0 || choice > 3) choice = 1;
    out3[0] = k_tgr_twist_mix[choice][0];
    out3[1] = k_tgr_twist_mix[choice][1];
    out3[2] = k_tgr_twist_mix[choice][2];
    return 1;
}

/* Fold the resolved rolls into a spec whose seed is already final. Writes ONLY
 * the entries no knob pinned, so td5_trackgen_apply_config keeps ownership of
 * every pinned value (and its clamping). With the master knob off this writes
 * nothing, which is what makes OFF byte-identical to pre-R21. */
void tg_rolls_apply_spec(TD5_TrackGenSpec *spec)
{
    if (!spec || !s_rolls_valid || !tg_rolls_enabled()) return;

    if (!s_rolls.pinned[TD5_TG_ROLL_TWIST]) {
        int mix[3];
        td5_trackgen_twist_mix(s_rolls.choice[TD5_TG_ROLL_TWIST], mix);
        spec->weight[TD5_TG_STRAIGHT] = mix[0];
        spec->weight[TD5_TG_CURVE]    = mix[1];
        spec->weight[TD5_TG_ACUTE]    = mix[2];
    }
    if (!s_rolls.pinned[TD5_TG_ROLL_CORNERS])
        spec->curve_safety_x100 = s_rolls.value[TD5_TG_ROLL_CORNERS];
    if (!s_rolls.pinned[TD5_TG_ROLL_GRADE])
        spec->max_grade_x1000 = s_rolls.value[TD5_TG_ROLL_GRADE];
    if (!s_rolls.pinned[TD5_TG_ROLL_DUAL])
        spec->weight[TD5_TG_DUAL_LANE] = s_rolls.value[TD5_TG_ROLL_DUAL];
    if (!s_rolls.pinned[TD5_TG_ROLL_HILLS])
        spec->elevation_amplitude = s_rolls.value[TD5_TG_ROLL_HILLS];
    if (!s_rolls.pinned[TD5_TG_ROLL_LENGTH])
        spec->target_spans = s_rolls.value[TD5_TG_ROLL_LENGTH];
}

/* Build identity, not a diagnostic -- logged unconditionally and BEFORE the
 * GENSTAMP check, so a REUSED build (which prints no inventory at all) still
 * says what it is. */
void tg_rolls_report(void)
{
    const char *unpinned = tg_rolls_enabled() ? "rolled" : "legacy";
    int i, rolled = 0, pinned = 0, restored = 0, pres_off = 0;

    if (!s_rolls_valid) return;
    TD5_LOG_I(LOG_TAG, "trackgen: [R21 ROLL] ---- randomized parameters "
              "(seed %u, master=%s) ----", s_rolls.seed,
              tg_rolls_enabled() ? "on" : "OFF (legacy defaults)");
    for (i = 0; i < TD5_TG_ROLL_COUNT; i++) {
        const TG_RollEntry *e = &k_tg_rolls[i];
        if (!e->name) continue;
        if (s_rolls.pinned[i]) pinned++; else rolled++;
        if (s_rolls.restored[i]) restored++;
        if (e->pres_rank && s_rolls.value[i] == 0) pres_off++;
        if (i == TD5_TG_ROLL_TWIST) {
            int mix[3];
            td5_trackgen_twist_mix(s_rolls.choice[i], mix);
            TD5_LOG_I(LOG_TAG, "trackgen: [R21 ROLL]   %-12s = %-10s %-6s "
                      "(%d/%d/%d)", e->name,
                      td5_trackgen_roll_choice_name(i, s_rolls.choice[i]),
                      s_rolls.pinned[i] ? "PINNED" : unpinned,
                      mix[0], mix[1], mix[2]);
        } else {
            TD5_LOG_I(LOG_TAG, "trackgen: [R21 ROLL]   %-12s = %-10s %-6s "
                      "(%d)%s%s", e->name,
                      td5_trackgen_roll_choice_name(i, s_rolls.choice[i]),
                      s_rolls.pinned[i] ? "PINNED" : unpinned,
                      s_rolls.value[i],
                      s_rolls.restored[i] ? " RESTORED by the presence budget"
                      : s_rolls.pinned[i] && e->knob ? " via " : "",
                      s_rolls.restored[i] ? ""
                      : s_rolls.pinned[i] && e->knob ? e->knob : "");
        }
    }
    TD5_LOG_I(LOG_TAG, "trackgen: [R21 ROLL] ---- %d rolled, %d pinned ----",
              rolled, pinned);
    /* [R22] The presence line is what makes the budget tunable: it says how
     * much of the world this seed left out and how much the budget put back. */
    TD5_LOG_I(LOG_TAG, "trackgen: [R22 PRESENCE] %d of %d presence row(s) OFF "
              "(budget %d), %d restored by rank (knob "
              "TD5RE_R22_PRESENCE_OFF_MAX)", pres_off,
              tg_rolls_presence_count(),
              td5_env_int("TD5RE_R22_PRESENCE_OFF_MAX", 8, 0, 64), restored);
}

/* How many entries are presence rows -- reported rather than hardcoded as 23,
 * so adding a presence row cannot make the line lie. */
int tg_rolls_presence_count(void)
{
    int i, n = 0;
    for (i = 0; i < TD5_TG_ROLL_COUNT; i++)
        if (k_tg_rolls[i].name && k_tg_rolls[i].pres_rank) n++;
    return n;
}

/* ---- [R21 MOOD] -----------------------------------------------------------
 * Scaffolding: resolved, latched and logged, consumed by nothing yet. See the
 * TG_Mood block in the internal header for why it is latched and why grip
 * carries a floor. Salts continue the R21 range. */
static TG_Mood s_mood;

static const char *const k_mood_season[]  = { "SPRING", "SUMMER", "AUTUMN",
                                              "WINTER" };
static const char *const k_mood_weather[] = { "CLEAR", "OVERCAST", "RAIN",
                                              "FOG" };
/* Weather leans dry: rain and fog change how a track reads more than any other
 * mood axis, so they are the exception rather than half of all tracks. */
static const unsigned char k_mood_weather_w[] = { 50, 30, 13, 7 };

static void tg_mood_resolve(unsigned int seed)
{
    memset(&s_mood, 0, sizeof(s_mood));
    s_mood.season  = tg_roll_pick_w(tg_roll_hash(seed, 0x21012001u), NULL, 4);
    s_mood.weather = tg_roll_pick_w(tg_roll_hash(seed, 0x21012002u),
                                    k_mood_weather_w, 4);
    /* Derived, not independently rolled: a dry CLEAR track with 80% wetness
     * would be incoherent, and coherence is the whole point of latching. */
    s_mood.wetness = (s_mood.weather == 2) ? 60 + (int)(tg_roll_hash(seed, 0x21012003u) % 40u)
                   : (s_mood.weather == 1) ? (int)(tg_roll_hash(seed, 0x21012003u) % 25u)
                   : 0;
    s_mood.fog_pct = (s_mood.weather == 3) ? 50 + (int)(tg_roll_hash(seed, 0x21012004u) % 50u)
                   : 0;
    s_mood.wear    = (int)(tg_roll_hash(seed, 0x21012005u) % 101u);
    /* FLOOR, not a free roll -- grip reaches the simulation. 100 dry, and at
     * most a 25% reduction fully wet, which is the same posture ALPTOWN's
     * tarmac decision takes for ice. */
    s_mood.grip_pct = 100 - (s_mood.wetness * 25) / 100;
    if (s_mood.grip_pct < 75) s_mood.grip_pct = 75;

    TD5_LOG_I(LOG_TAG, "trackgen: [R21 MOOD] season=%s weather=%s wetness=%d "
              "fog=%d wear=%d grip=%d%% (not yet consumed by any emitter)",
              k_mood_season[s_mood.season], k_mood_weather[s_mood.weather],
              s_mood.wetness, s_mood.fog_pct, s_mood.wear, s_mood.grip_pct);
}

const TG_Mood *tg_mood(void) { return &s_mood; }

/* ---- [R21 LANDMARKS] ------------------------------------------------------
 * Scaffolding: the pass runs over the merged biome runs and reports, but the
 * table is empty, so it places nothing and cannot change a build. Adding a
 * landmark is a row here plus an emitter; see the TG_Landmark block in the
 * internal header for the placement contract. */
static const TG_Landmark k_landmarks[] = {
    /* name, biome_mask, min_run, once, weight, flat, water, night, salt */
    { NULL, 0u, 0, 0, 0, 0, 0, 0, 0u }   /* deliberately empty */
};
#define TG_LANDMARK_N ((int)(sizeof(k_landmarks) / sizeof(k_landmarks[0])))

void tg_landmarks_place(const TG_NodeList *nl, int nspans)
{
    int placed = 0, runs = 0, si;

    if (!nl || nspans <= 0) return;

    /* Walk MERGED runs, not cells: a repeated 300-span city is one run, and a
     * once_per_track piece must not be offered it twice. */
    for (si = 0; si < nspans; ) {
        int a = si, b = si, li;
        tg_biome_run_bounds(si, &a, &b);
        if (b < a) break;
        runs++;
        for (li = 0; li < TG_LANDMARK_N; li++) {
            const TG_Landmark *L = &k_landmarks[li];
            if (!L->name) continue;                 /* empty slot */
            if (b - a + 1 < L->min_run_spans) continue;
            if (L->biome_mask &&
                !(L->biome_mask & (1u << tg_biome_cell_index(a)))) continue;
            if (L->needs_night && !s_is_night) continue;
            /* Hash-gated so this consumes no RNG: adding a landmark must not
             * be able to move the road. */
            if ((tg_roll_hash(s_gen_seed, L->salt) % 100u)
                >= (unsigned)L->weight) continue;
            placed++;
        }
        si = b + 1;
    }
    TD5_LOG_I(LOG_TAG, "trackgen: [R21 LANDMARK] %d run(s), %d table row(s), "
              "%d placed", runs, TG_LANDMARK_N - 1, placed);
}

unsigned int tg_rand(void)
{
    unsigned int x = s_rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s_rng = x;
    return x;
}

/* Uniform in [lo, hi] inclusive. */
int tg_range(int lo, int hi)
{
    if (hi <= lo) return lo;
    return lo + (int)(tg_rand() % (unsigned int)(hi - lo + 1));
}

/* Uniform in [0, 1). */
double tg_frand(void)
{
    return (double)(tg_rand() >> 8) / 16777216.0;
}

/* [SCENERY STREAMING] The stash a streamed build leaves for the worker (see
 * s_want_stream above). THE NODE LIST IS OWNED HERE, not by build_level, which
 * frees nl.v at its `done:` label -- a streamed build transfers the pointer out
 * first and td5_trackgen_stream_discard frees it. */
TG_NodeList s_stream_nl;         /* .v owned here while pending */

int         s_stream_nspans, s_stream_lanes;

int         s_stream_pending;    /* geometry done, scenery not started */

char        s_stream_dir[256];   /* level dir, for the deferred write */

static int tg_nodes_reserve(TG_NodeList *nl, int need)
{
    if (need <= nl->cap) return 1;
    {
        int cap = nl->cap ? nl->cap : 256;
        TG_Node *nv;
        while (cap < need) cap *= 2;
        nv = (TG_Node *)realloc(nl->v, (size_t)cap * sizeof(TG_Node));
        if (!nv) return 0;
        nl->v = nv;
        nl->cap = cap;
    }
    return 1;
}

/* [PREVIEW] Progress/cancel hook for td5_trackgen_preview_route. NULL for every
 * normal build, so the walk stays bit-identical when no preview is running --
 * which is what lets the preview share this code path with the real generator
 * instead of drifting from it. Set and cleared by preview_route alone. */
static const TD5_TrackGenPreviewSink *s_preview_sink;

static int s_preview_cancelled;

int tg_nodes_push(TG_NodeList *nl, double x, double z,
                  double width, int lanes)
{
    if (!tg_nodes_reserve(nl, nl->count + 1)) return 0;
    {
        TG_Node *n = &nl->v[nl->count++];
        n->x = x; n->y = 0.0; n->z = z;
        n->width = width;
        n->lanes = lanes;
        n->lane_base = TD5_TG_HEIGHT_NIBBLE;   /* [LANES] shipped baseline 8 */
        n->lane_side = 0;                      /* [LANES] no change at this seam */
        n->jx = 0.0; n->jz = 0.0;
        n->tx = 0.0; n->tz = 1.0;
    }
    /* Every centerline node passes through here, so one hook covers the whole
     * walk. Cancelling returns 0, which the three push sites already propagate
     * as a build failure -- no new control flow in the walk itself. */
    if (s_preview_sink) {
        const TG_Node *n = &nl->v[nl->count - 1];
        TD5_TrackGenPoint p;
        p.x = (float)n->x;
        p.z = (float)n->z;
        p.lanes = n->lanes;
        p.branch = 0;
        /* [R22 item 1 fix] Which node this is. The walk rolls back rejected
         * candidate directions, so the same index is published more than once
         * and the LAST publish is the surviving route. See the field comment. */
        p.node = nl->count - 1;
        if (s_preview_sink->on_points)
            s_preview_sink->on_points(&p, 1, s_preview_sink->ctx);
        if (s_preview_sink->should_cancel &&
            s_preview_sink->should_cancel(s_preview_sink->ctx)) {
            s_preview_cancelled = 1;
            return 0;
        }
    }
    return 1;
}

int tg_too_close(const TG_NodeList *nl, double x, double z,
                 double width, double lane_width, int skip)
{
    const int limit = nl->count - skip;
    int i;
    for (i = 0; i < limit; i++) {
        double dx = nl->v[i].x - x;
        double dz = nl->v[i].z - z;
        double need = (nl->v[i].width + width) * 0.5 + lane_width * 0.25;
        if (dx * dx + dz * dz < need * need) return 1;
    }
    return 0;
}

/* Acute heading budget in radians, from the env knob, clamped so cos(limit)>0
 * (the non-trapping proof requires strictly-positive forward progress). */
double tg_acute_heading_limit(void)
{
    int deg = td5_env_int("TD5RE_AUTOTRACK_ACUTE_DEG", TD5_TG_ACUTE_HEADING_DEG,
                          TD5_TG_ACUTE_HEADING_MIN, TD5_TG_ACUTE_HEADING_MAX);
    return (double)deg * TD5_TG_PI / 180.0;
}

/* Derived adjacent-skip window (see the self-intersection guard note): the
 * smallest N such that two nodes N spans apart along the road provably cannot
 * be within tg_too_close's "need" distance of each other.
 *
 * ROOT CAUSE of "geometry can overlap on acute curves" (2026-08-27). This used
 * to derive N from the AXIS advance -- dmin = span_len * cos(limit_max), the
 * guaranteed progress along TD5_TG_AXIS_HEADING. That is the right quantity for
 * the NON-TRAPPING proof and the wrong one for SEPARATION, because cos(88 deg)
 * is 0.035: at the default acute budget it produced N = 351. Every pair of
 * nodes less than 351 spans apart was therefore EXEMPT from the overlap test,
 * and an acute section is only 4..12 spans long -- a run of them can double the
 * road back alongside itself well inside that window with nothing checking it.
 * The road then interpenetrates, which is exactly the reported symptom (and the
 * span walker snaps to the wrong span there, the same failure the guard exists
 * to prevent).
 *
 * Separation does not come from the heading budget at all. It comes from the
 * CURVATURE SAFETY floor: every turning span is clamped to
 * radius >= (width/2) * curve_safety, so the WORST case for two nodes n spans
 * apart is a single arc at exactly that radius, where their separation is the
 * chord c(n) = 2r * sin(n * span_len / (2r)). Anything less curved -- a
 * straight, a sweeping curve, an S, a hairpin made of two arcs -- puts them
 * further apart, not closer. So N is the smallest n with c(n) >= need_max:
 *
 *     n >= (2r / span_len) * asin(need_max / (2r)),   2r = curve_safety * w_max
 *
 * At 12 lanes and curve_safety 1.8 that is ~13 spans rather than 351, so the
 * 13..351 band -- where the real overlaps live -- is now CHECKED instead of
 * assumed away. 2r > need_max holds for any road at least one lane wide, so the
 * asin argument stays in range; the degenerate branch below covers the rest.
 *
 * Knobs: TD5RE_AUTOTRACK_ADJ_SKIP (0 = derive, else force a window) and
 * TD5RE_AUTOTRACK_SKIP_AXIS=1 (restore the old axis-derived window), both for
 * bisecting a track that comes out shorter than wanted -- a tighter window
 * rejects more sections, so the walk falls back to straights more often. */
int tg_adjacent_skip(const TD5_TrackGenSpec *spec, double limit_max)
{
    const double span_len   = (double)spec->span_length;
    const double lane_width = (double)spec->lane_width;
    const double w_max      = (double)TD5_TG_MAX_LANES * lane_width;
    /* Worst-case need in tg_too_close: both roads at the max width. */
    const double need_max   = w_max + lane_width * 0.25;
    /* [R21 SHAPE] This window must stay a track-wide WORST CASE, so it is
     * sized with the TIGHTEST corner any biome can ask for, not with a local
     * value. Direction of the effect, since it is not obvious: with
     * f(r) = (N/L)*asin(u)/u and u = N/2r, asin(u)/u increases in u, so a
     * SMALLER two_r yields a LARGER skip -- tightening corners GROWS the
     * exemption window, which is the conservative direction and the correct
     * one (a tighter arc can double back on itself in fewer spans). Expect the
     * adjacent_skip= number logged below to RISE when road character is on; if
     * it does not, this is not wired up. */
    const int    worst_x100 = tg_shape_worst_safety_x100(spec);
    const double safety     = (worst_x100 > 0)
                            ? (double)worst_x100 / 100.0
                            : TD5_TG_CURVE_SAFETY;
    const double two_r      = safety * w_max;    /* 2x the tightest legal radius */
    const int    forced     = td5_env_int("TD5RE_AUTOTRACK_ADJ_SKIP", 0, 0, 4000);
    const double step       = (span_len > 1.0) ? span_len : 1.0;
    int skip;

    if (forced > 0) return forced;

    if (td5_env_flag_off("TD5RE_AUTOTRACK_SKIP_AXIS")) {
        double dmin = span_len * cos(limit_max);
        if (dmin < 1.0) dmin = 1.0;              /* guard against cos -> 0 */
        skip = (int)ceil(need_max / dmin);
    } else if (two_r > need_max) {
        /* +2 spans of margin: the width RAMPS across a dual-lane taper, so the
         * two nodes need not carry the same width and the closed form above is
         * a bound rather than an identity. */
        skip = (int)ceil((two_r / step) * asin(need_max / two_r)) + 2;
    } else {
        /* Degenerate spec (a road as wide as its own tightest turn): fall back
         * to the straight-line bound, which is always safe. */
        skip = (int)ceil(need_max / step) + 2;
    }
    if (skip < 1) skip = 1;
    return skip;
}

/* Pick a section type from the normalised weights. */
/* [R21 SHAPE] `si` is the span the section will START on, so the mix can be
 * the biome's own -- "each biome should have its own twistiness".
 *
 * DRAW BUDGET IS UNCHANGED: still exactly one tg_range per pick. What changes
 * is the MAPPING from roll to section, not the number of draws, so this cannot
 * shift the RNG stream by itself. (Existing seeds still move, because the four
 * section kinds consume different numbers of draws further down and the mix
 * changes which kind comes up -- that is expected and already true of any
 * generator change. What must hold, and is tested, is that OFF is
 * byte-identical.) */
TD5_TrackGenSection tg_pick_section(const TD5_TrackGenSpec *spec, int si)
{
    int w[TD5_TG_SECTION_COUNT];
    int total = 0, i, roll;

    for (i = 0; i < TD5_TG_SECTION_COUNT; i++) w[i] = spec->weight[i];

    if (tg_r21_road_char()) {
        static const int k_field[TD5_TG_SECTION_COUNT] = {
            TG_SF_W_STRAIGHT, TG_SF_W_CURVE, TG_SF_W_ACUTE, TG_SF_W_DUAL
        };
        for (i = 0; i < TD5_TG_SECTION_COUNT; i++)
            w[i] = w[i] * tg_shape_lerp_pct(si, k_field[i],
                                            TD5_TG_BIOME_BLEND) / 100;
    }

    for (i = 0; i < TD5_TG_SECTION_COUNT; i++) if (w[i] > 0) total += w[i];
    /* A biome could in principle scale every weight to zero; fall back to the
     * spec's own mix rather than silently returning nothing but straights. */
    if (total <= 0) {
        for (i = 0; i < TD5_TG_SECTION_COUNT; i++) {
            w[i] = spec->weight[i];
            if (w[i] > 0) total += w[i];
        }
    }
    if (total <= 0) return TD5_TG_STRAIGHT;
    roll = tg_range(0, total - 1);
    for (i = 0; i < TD5_TG_SECTION_COUNT; i++) {
        if (w[i] <= 0) continue;
        if (roll < w[i]) return (TD5_TrackGenSection)i;
        roll -= w[i];
    }
    return TD5_TG_STRAIGHT;
}

const char *tg_section_name(TD5_TrackGenSection s)
{
    switch (s) {
        case TD5_TG_STRAIGHT:  return "straight";
        case TD5_TG_CURVE:     return "curve";
        case TD5_TG_ACUTE:     return "acute";
        case TD5_TG_DUAL_LANE: return "dual-lane";
        default:               return "?";
    }
}

/* Lowest road node on the WHOLE track, cached.
 *
 * A global fact with one consumer that genuinely needs a global one: the
 * background band (tg_emit_far_band) reaches tens of thousands of units sideways
 * from its own span, so a height taken from that span is meaningless once the
 * band has swept out over a different stretch of track. See the ceiling note on
 * TD5_TG_FAR_REACH for what the local version looked like in frame.
 *
 * Computed lazily rather than at generate time because the node list is still
 * being appended to (branch corridors) after the elevation pass; by the time the
 * first band is emitted the list is final. tg_apply_elevation invalidates it, so
 * a second generate in the same process cannot inherit the first one's floor. */
static double s_track_min_y;

static int    s_track_min_valid;

/* [TOPOLOGY-FIRST] The elevation lives in td5_tg_road.c now; it calls this
 * so a second build in one process cannot inherit the first one's floor. */
void tg_track_min_y_invalidate(void) { s_track_min_valid = 0; }

double tg_track_min_y(const TG_NodeList *nl)
{
    int i;

    if (s_track_min_valid) return s_track_min_y;
    s_track_min_y = (nl->count > 0) ? nl->v[0].y : 0.0;
    for (i = 1; i < nl->count; i++)
        if (nl->v[i].y < s_track_min_y) s_track_min_y = nl->v[i].y;
    s_track_min_valid = 1;
    return s_track_min_y;
}

void tg_buf_free(TG_Buf *buf)
{
    free(buf->b);
    buf->b = NULL;
    buf->len = buf->cap = 0;
}

int tg_buf_need(TG_Buf *buf, size_t extra)
{
    if (buf->oom) return 0;
    if (buf->len + extra <= buf->cap) return 1;
    {
        size_t cap = buf->cap ? buf->cap : 4096;
        unsigned char *nb;
        while (cap < buf->len + extra) cap *= 2;
        nb = (unsigned char *)realloc(buf->b, cap);
        if (!nb) { buf->oom = 1; return 0; }
        buf->b = nb;
        buf->cap = cap;
    }
    return 1;
}

void tg_put_u8(TG_Buf *buf, unsigned int v)
{
    if (!tg_buf_need(buf, 1)) return;
    buf->b[buf->len++] = (unsigned char)(v & 0xFF);
}

void tg_put_u16(TG_Buf *buf, unsigned int v)
{
    if (!tg_buf_need(buf, 2)) return;
    buf->b[buf->len++] = (unsigned char)(v & 0xFF);
    buf->b[buf->len++] = (unsigned char)((v >> 8) & 0xFF);
}

void tg_put_u32(TG_Buf *buf, unsigned int v)
{
    if (!tg_buf_need(buf, 4)) return;
    buf->b[buf->len++] = (unsigned char)(v & 0xFF);
    buf->b[buf->len++] = (unsigned char)((v >> 8) & 0xFF);
    buf->b[buf->len++] = (unsigned char)((v >> 16) & 0xFF);
    buf->b[buf->len++] = (unsigned char)((v >> 24) & 0xFF);
}

void tg_put_i32(TG_Buf *buf, int v)
{
    tg_put_u32(buf, (unsigned int)v);
}

static void tg_put_zeros(TG_Buf *buf, size_t n)
{
    if (!tg_buf_need(buf, n)) return;
    memset(buf->b + buf->len, 0, n);
    buf->len += n;
}

/* Copy a file verbatim. Used for the sky: the renderer wants a 256x256 RGBA
 * FORWSKY.png in the level directory, and reusing a shipped panorama is both
 * guaranteed-valid and better looking than anything this generator could
 * synthesise. Forward-only tracks need no BACKSKY -- the loader falls back to
 * FORWSKY when it is absent (td5_game.c:4644-4648). */
static int tg_copy_file(const char *src, const char *dst)
{
    FILE *fi, *fo;
    char buf[16384];
    size_t n;
    int ok = 1;

    fi = fopen(src, "rb");
    if (!fi) return 0;
    fo = fopen(dst, "wb");
    if (!fo) { fclose(fi); return 0; }
    while ((n = fread(buf, 1, sizeof(buf), fi)) > 0) {
        if (fwrite(buf, 1, n, fo) != n) { ok = 0; break; }
    }
    fclose(fi);
    fclose(fo);
    return ok;
}

/* Install a sky by borrowing one from a shipped level, chosen from the seed so
 * different generated tracks get different skies. Candidates are probed in
 * order because not every level ships one. Non-fatal: without it the race just
 * renders against the flat clear colour, which is what happened before. */
/* [R8 G1 "use different skyboxes for day and night"] Sky pools, split by a
 * MEASURED property rather than a hand-picked list -- the R7 flora lesson
 * (listing pages fixes the instance, measuring the property fixes the class).
 *
 * Every shipped FORWSKY.png is a 256x256 backdrop panorama; the 31 that exist
 * were scored by mean luminance (0.299R + 0.587G + 0.114B over all 65536
 * texels) and the distribution has a clear gap:
 *
 *   level037   0.0   degenerate, all black -- EXCLUDED from both pools
 *   level030  27.3   night, heavy cloud over a dark horizon
 *   level005  36.7   night, storm cloud
 *   level023  62.7   dusk city, lit windows
 *   level018  76.5   overcast dusk over water
 *   ---------------- gap ----------------
 *   level039  96.5   level015 102.2   level002 106.8   level017 115.7
 *   level013 118.4   level010 118.6   level003 119.4   level004 119.6 ...
 *   ... up to level008 164.7
 *
 * The old pool was { 1, 2, 3, 5, 8, 13, 21, 29 } chosen blind, so a DAY track
 * had a one-in-eight chance of drawing level005's night storm and a NIGHT track
 * had a seven-in-eight chance of racing under a bright blue sky. That is the
 * complaint. td5_trackgen_is_night() is the same latched predicate the lamp
 * posts and the headlights already read, so the sky now agrees with them.
 *
 * TD5RE_R8_VARIETY_SKY=0 restores the single blind pool for an A/B. */
static const int k_sky_day[]   = { 39, 15, 2, 17, 13, 10, 3, 4, 25, 28, 1, 20,
                                   27, 19, 16, 29, 11, 9, 6, 26, 21, 22, 12,
                                   7, 14, 8 };

static const int k_sky_night[] = { 30, 5, 23, 18 };

static const int k_sky_blind[] = { 1, 2, 3, 5, 8, 13, 21, 29 };

void tg_install_sky(const char *dir, unsigned int seed)
{
    const int night = td5_trackgen_is_night();
    const int variety = td5_env_flag_on("TD5RE_R8_VARIETY_SKY");
    const int *pool = !variety ? k_sky_blind
                    : (night ? k_sky_night : k_sky_day);
    const int n = !variety
        ? (int)(sizeof(k_sky_blind) / sizeof(k_sky_blind[0]))
        : (night ? (int)(sizeof(k_sky_night) / sizeof(k_sky_night[0]))
                 : (int)(sizeof(k_sky_day)   / sizeof(k_sky_day[0])));
    char src[256], dst[320];
    int i;

    for (i = 0; i < n; i++) {
        int lvl = pool[(seed / 7u + (unsigned)i) % (unsigned)n];
        snprintf(src, sizeof(src),
                 "re/assets/levels/level%03d/FORWSKY.png", lvl);
        snprintf(dst, sizeof(dst), "%s/FORWSKY.png", dir);
        if (tg_copy_file(src, dst)) {
            TD5_LOG_I(LOG_TAG,
                      "trackgen: sky from level%03d -> %s (%s pool, %d cands)",
                      lvl, dst, !variety ? "blind" : (night ? "NIGHT" : "DAY"),
                      n);
            return;
        }
    }
    TD5_LOG_W(LOG_TAG, "trackgen: no shipped FORWSKY.png found to borrow; "
              "race will render against the flat clear colour");
}

int tg_write_file(const char *dir, const char *name,
                         const unsigned char *data, size_t len)
{
    char path[320];
    FILE *f;
    size_t wrote;

    snprintf(path, sizeof(path), "%s/%s", dir, name);
    f = fopen(path, "wb");
    if (!f) {
        TD5_LOG_E(LOG_TAG, "trackgen: cannot open %s for writing", path);
        return 0;
    }
    wrote = fwrite(data, 1, len, f);
    fclose(f);
    if (wrote != len) {
        TD5_LOG_E(LOG_TAG, "trackgen: short write on %s (%zu/%zu)",
                  path, wrote, len);
        return 0;
    }
    TD5_LOG_I(LOG_TAG, "trackgen: wrote %s (%zu bytes)", path, len);
    return 1;
}

/* Round-to-nearest for signed doubles (lrint is not uniformly available). */
int tg_round(double v)
{
    return (int)(v >= 0.0 ? (v + 0.5) : (v - 0.5));
}

/* ===================== [LANES] LANE MANAGEMENT ============================
 * Per-span lane count, the way shipped tracks do it (docs/plans/
 * AUTOTRACK_ELEMENT_CATALOG.md section 4, from a census of all 36 levels):
 *   - a span's lane count is its own (low nibble of packed byte 3);
 *   - the row two spans SHARE has min(lanes) + 1 points;
 *   - the WIDER span at a change is the transition span: type 2 (+1 lane on
 *     the right), 3 (+1 left), 4 (+1 each side) when it is wider than the span
 *     before it; type 5 (-1 right), 6 (-1 left), 7 (-1 each side) when it is
 *     wider than the span after it;
 *   - the high nibble is a lane BASE: a lane gained on the LEFT shifts every
 *     lane index by one, so the base drops by one (and rises by one when the
 *     left lane goes), which is what keeps resolve_neighbor's
 *     `sub_lane += h_off - dest_h` continuous across the seam.
 * The walk decides WHERE lanes change (tg_build_centerline); these helpers
 * only read the nodes, so the strip, the road mesh and every scenery emitter
 * see one answer. */
int tg_row_points(const TG_NodeList *nl, int node)
{
    int la, lb;
    if (node <= 0) return nl->v[0].lanes + 1;
    if (node >= nl->count - 1) return nl->v[nl->count - 2].lanes + 1;
    la = nl->v[node - 1].lanes;             /* span ending on this row  */
    lb = nl->v[node].lanes;                 /* span starting on this row */
    return (la < lb ? la : lb) + 1;
}

int tg_span_type_for(const TG_NodeList *nl, int si)
{
    const int L = nl->v[si].lanes;
    const int nspans = nl->count - 1;
    if (si > 0 && L > nl->v[si - 1].lanes) {
        const int d = L - nl->v[si - 1].lanes;
        if (d >= 2) return 4;                       /* ADD2: one each side */
        return nl->v[si].lane_side > 0 ? 3 : 2;     /* ADD1_L : ADD1_R */
    }
    if (si + 1 < nspans && L > nl->v[si + 1].lanes) {
        const int d = L - nl->v[si + 1].lanes;
        if (d >= 2) return 7;                       /* DROP2 */
        return nl->v[si + 1].lane_side > 0 ? 6 : 5; /* DROP1_L : DROP1_R */
    }
    return 1;
}

/* ===================== [S1] RANGE EMITTER =====================
 * Emit spans [first_span, first_span+span_count) and their vertex rows into
 * CALLER-OWNED buffers, with vertex indices continuing from *vtx_count.
 *
 * This exists for Phase 2 streaming (docs/plans/AUTOTRACK_STREAMING.md): a
 * rolling ring buffer has to rewrite a REGION of the track, not rebuild the
 * whole thing, so the emitter has to be addressable by range.
 *
 * CONTRACT: first_span MUST be block-aligned (first_span % block == 0), and
 * span_count SHOULD be a multiple of block except for the final partial one.
 * Origin blocks share an origin and share rows across the block, so a range
 * starting mid-block would produce a different origin for those spans and
 * different vertex indices -- silently different geometry, not an error. The
 * streaming design already advances the write cursor in block units for this
 * reason. Asserted below rather than assumed.
 */
static int tg_emit_span_range(const TG_NodeList *nl, int first_span,
                              int span_count, int block,
                              TG_Buf *spans, TG_Buf *verts, int *vtx_count)
{
    const int range_end = first_span + span_count;
    const int nspans_all = nl->count - 1;
    int s0, ok = 1;

    if (block <= 0 || span_count <= 0) return 0;
    if (first_span % block != 0) {
        TD5_LOG_E(LOG_TAG, "trackgen: emit range first_span=%d is not aligned "
                  "to the %d-span origin block; refusing (would silently "
                  "change geometry)", first_span, block);
        return 0;
    }
    if (range_end > nspans_all) {
        TD5_LOG_E(LOG_TAG, "trackgen: emit range [%d,%d) exceeds %d spans",
                  first_span, range_end, nspans_all);
        return 0;
    }

    for (s0 = first_span; s0 < range_end; s0 += block) {
        const int ns  = (s0 + block <= range_end) ? block : (range_end - s0);
        const int ox  = tg_round(nl->v[s0].x);
        const int oy  = tg_round(nl->v[s0].y);
        const int oz  = tg_round(nl->v[s0].z);
        const int base = *vtx_count;
        /* [LANES] Row k of this block (node s0+k) is SHARED by span s0+k-1
         * (its far row) and span s0+k (its near row). Shipped tracks size a
         * shared row to the NARROWER of the two spans it joins (Keswick span
         * 1536: type 7, lanes 4, near row 5 points, far row 3 points, then
         * span 1537 lanes 2 starts on that 3-point row), so a lane change
         * costs no row break: the wider span is the TRANSITION span and its
         * type (2..7) tells the walker which edge gained or lost the lane.
         * Row start indices are therefore cumulative, not k * row_pts. */
        int row_start[TD5_TG_ORIGIN_BLOCK_MAX + 1];
        int row_pts[TD5_TG_ORIGIN_BLOCK_MAX + 1];
        int need = 0, k;

        if (ns > TD5_TG_ORIGIN_BLOCK_MAX) {
            TD5_LOG_E(LOG_TAG, "trackgen: origin block %d exceeds the %d-span "
                      "row table", ns, TD5_TG_ORIGIN_BLOCK_MAX);
            return 0;
        }
        for (k = 0; k <= ns; k++) {
            row_pts[k]   = tg_row_points(nl, s0 + k);
            row_start[k] = base + need;
            need += row_pts[k];
        }
        if (*vtx_count + need > TD5_TG_MAX_VERTICES) {
            TD5_LOG_W(LOG_TAG, "trackgen: vertex ceiling hit at span %d "
                      "(%d verts); truncating", s0, *vtx_count);
            break;
        }

        /* ns+1 rows, one per node s0..s0+ns inclusive, all relative to this
         * block's origin. The shared row at a block seam is re-emitted under
         * the new origin, exactly as shipped tracks do. */
        for (k = 0; k <= ns; k++) {
            const TG_Node *n = &nl->v[s0 + k];
            const double lx = n->tz, lz = -n->tx;
            const int pts = row_pts[k];
            const int rl  = pts - 1;               /* lanes across THIS row */
            int j;
            for (j = 0; j < pts; j++) {
                double t  = (n->width * 0.5)
                          - (n->width * (double)j / (double)rl);
                int dx = tg_round(n->x + lx * t) - ox;
                int dy = tg_round(n->y) - oy;
                int dz = tg_round(n->z + lz * t) - oz;
                if (dx < -32768 || dx > 32767 || dy < -32768 || dy > 32767 ||
                    dz < -32768 || dz > 32767) {
                    TD5_LOG_E(LOG_TAG, "trackgen: vertex offset out of int16 "
                              "range in block at span %d (%d,%d,%d) -- reduce "
                              "the origin block size", s0, dx, dy, dz);
                    ok = 0;
                }
                tg_put_u16(verts, (unsigned)(dx & 0xFFFF));
                tg_put_u16(verts, (unsigned)(dy & 0xFFFF));
                tg_put_u16(verts, (unsigned)(dz & 0xFFFF));
            }
            *vtx_count += pts;
        }

        for (k = 0; k < ns; k++) {
            const int si = s0 + k;
            const TG_Node *n = &nl->v[si];
            tg_put_u8 (spans, (unsigned)tg_span_type_for(nl, si));
            tg_put_u8 (spans, (unsigned)tg_surface_attr(si));
            /* Lane bitmask 0 = every lane is the low-nibble surface (dry asphalt,
             * full grip). The old `1 | (1<<(lanes-1))` marked the OUTER lanes,
             * which surface_type_for_span_lane turns into the 0x10
             * "alternate/off-road" surface -- td5_track_surface_is_slow returns
             * 1 for anything with 0x10 set, so those lanes silently SLOWED the
             * car while textured identically to the fast lanes (reported as
             * "lanes that make the car slower look the same as the road"). A
             * generated arcade road is uniformly drivable. */
            tg_put_u8 (spans, 0);
            /* [LANES] high nibble = lane BASE (the walker's cross-span lane
             * index shift, td5_track.c resolve_neighbor: sub_lane += h_off -
             * dest_h), low nibble = this span's own lane count. */
            tg_put_u8 (spans, (unsigned)(((n->lane_base & 0x0F) << 4)
                                         | (n->lanes & 0x0F)));
            tg_put_u16(spans, (unsigned)row_start[k]);
            tg_put_u16(spans, (unsigned)row_start[k + 1]);
            tg_put_u16(spans, 0xFFFF);                   /* link_next = -1 */
            tg_put_u16(spans, 0xFFFF);                   /* link_prev = -1 */
            tg_put_i32(spans, ox);
            tg_put_i32(spans, oy);
            tg_put_i32(spans, oz);
        }
    }
    return ok && !spans->oom && !verts->oom;
}

/* [S1 GATE] Prove the range emitter is composable: emitting the track in
 * block-aligned CHUNKS must be byte-identical to emitting it in one call. If
 * that does not hold, streaming would silently produce different geometry from
 * the same seed. Runs only when TD5RE_AUTOTRACK_SELFCHECK=1. */
void tg_selfcheck_ranges(const TG_NodeList *nl, int block)
{
    const int nspans = nl->count - 1;
    TG_Buf one_s, one_v, many_s, many_v;
    int vc_one = 0, vc_many = 0, s0, ok = 1;
    int chunk = block * 5;      /* several blocks per chunk, still aligned */

    memset(&one_s, 0, sizeof(one_s));   memset(&one_v, 0, sizeof(one_v));
    memset(&many_s, 0, sizeof(many_s)); memset(&many_v, 0, sizeof(many_v));

    if (!tg_emit_span_range(nl, 0, nspans, block, &one_s, &one_v, &vc_one))
        ok = 0;

    for (s0 = 0; ok && s0 < nspans; s0 += chunk) {
        int n = (s0 + chunk <= nspans) ? chunk : (nspans - s0);
        if (!tg_emit_span_range(nl, s0, n, block, &many_s, &many_v, &vc_many))
            ok = 0;
    }

    if (!ok) {
        TD5_LOG_E(LOG_TAG, "trackgen selfcheck: range emit FAILED");
    } else if (one_s.len != many_s.len || one_v.len != many_v.len ||
               vc_one != vc_many) {
        TD5_LOG_E(LOG_TAG, "trackgen selfcheck: FAIL size (spans %zu vs %zu, "
                  "verts %zu vs %zu, vtx %d vs %d)", one_s.len, many_s.len,
                  one_v.len, many_v.len, vc_one, vc_many);
    } else if (memcmp(one_s.b, many_s.b, one_s.len) != 0) {
        TD5_LOG_E(LOG_TAG, "trackgen selfcheck: FAIL span bytes differ");
    } else if (memcmp(one_v.b, many_v.b, one_v.len) != 0) {
        TD5_LOG_E(LOG_TAG, "trackgen selfcheck: FAIL vertex bytes differ");
    } else {
        TD5_LOG_I(LOG_TAG, "trackgen selfcheck: PASS -- %d spans emitted in "
                  "%d-span chunks is byte-identical to one shot (%zu span B, "
                  "%zu vtx B, %d verts)", nspans, chunk, one_s.len, one_v.len,
                  vc_one);
    }

    tg_buf_free(&one_s);  tg_buf_free(&one_v);
    tg_buf_free(&many_s); tg_buf_free(&many_v);
}

int tg_emit_strip(const TG_NodeList *nl, TG_Buf *out, int *out_spans)
{
    const int nspans = nl->count - 1;
    TG_Buf spans, verts;
    int vtx_count = 0, ok = 1;

    memset(&spans, 0, sizeof(spans));
    memset(&verts, 0, sizeof(verts));

    /* Lane count is uniform (see tg_build_centerline), so every row has the
     * same point count and consecutive spans can SHARE a row: span i spans
     * rows i and i+1. Shipped tracks do exactly this (level001: span0
     * lvi=0 rvi=5, span1 lvi=5 rvi=10). Emitting a private duplicated row
     * pair per span is geometrically identical but breaks span-to-span
     * adjacency, which resolve_neighbor (td5_track.c:4013) detects by vertex
     * INDEX -- contact then fails at every seam and the car sinks through the
     * road (measured: wheel_mask=0 for 343/385 ticks, even dead flat). */
    /* [LANES] The lane count is per span now; each fork reads its own at F
     * (uniform over the fork window by construction, checked below). */
    /* Reset per-call: a failed or branch-less build must not leave stale fork
     * records from a previous generation in the header. */
    if (!s_fork_placed) s_fork_count = 0;
    s_ring_len = 0;
    /* Spans per shared origin. Tunable so the effect of origin granularity on
     * ground contact is measurable: the ground probe appears to read a span's
     * origin_y (a track starting at y=-2040 probed -522240 = -2040*256), and a
     * shared origin_y makes the collision ground a flat step at the block base
     * while the visible road ramps away from it on hills. */
    const int block = td5_env_int("TD5RE_AUTOTRACK_BLOCK",
                                  TD5_TG_ORIGIN_BLOCK, 1, 20);

    /* ORIGIN BLOCKS. The loader resolves BOTH of a span's vertex rows against
     * THAT span's origin, so two spans can only share a row if they share an
     * origin. Hence origins are per-BLOCK, constant across the block, with the
     * shared row duplicated at block seams -- which is exactly what shipped
     * tracks do (level001 span0 lvi=0 rvi=5, span1 lvi=5 rvi=10).
     *
     * Storing one origin per span while sharing rows displaces every span's FAR
     * edge by one span step, skewing the road surface: contact then flickers
     * between all-four-wheels and none, and the car gets flung down-track
     * (measured: wall_clear median -583186 while driving, though a perfect 2250
     * at rest -- the giveaway that only the far row was wrong).
     *
     * Block length is bounded by the int16 vertex offset: TD5_TG_ORIGIN_BLOCK
     * spans of span_length, plus half the widest road, must stay under 32767. */
    /* [S1] Whole track = one block-aligned range. The emitter below can write
     * any block-aligned range into caller buffers, which is what Phase 2
     * streaming needs to rewrite a region in place. */
    if (!tg_emit_span_range(nl, 0, nspans, block, &spans, &verts, &vtx_count))
        ok = 0;

    /* ---- BRANCHES (opt-in): multiple forks, each a split-and-rejoin ---- */
    {
        const int ring = (int)(spans.len / 24);   /* main ring span count */
        if (!s_fork_placed) s_fork_count = 0;

        if (ok && tg_branches_enabled()) {
            /* Varied corridor lengths give the shipped topologies: a short
             * chicane, a canonical split, a long alternate route. The first
             * entry USED to be 8 spans, which is the "very small branch" that
             * glitched -- lengths are now floored at tg_branch_min_len(). */
            /* [TOPOLOGY-FIRST] Placement lives in tg_fork_place (td5_tg_branch.c)
             * so build_level can place the forks BEFORE the street network
             * plans bypass corridors for them; a caller that did not (the
             * preview, the regen self-check) places here. */
            unsigned int i;
            if (!s_fork_placed) tg_fork_place(nl, ring);
            s_fork_placed = 0;

            /* Emit each fork's strip pieces. Corridors are appended in fork
             * order, so their indices match the cbase computed above. */
            for (i = 0; ok && i < (unsigned)s_fork_count; i++) {
                const int F = s_forks[i].F, L = s_forks[i].len;
                const int b0 = s_forks[i].cbase, R = s_forks[i].R;
                const int sentinel_end = b0 + L - 1;
                const int lanes = s_forks[i].lanes;          /* [LANES] */
                const int main_half = s_forks[i].main_lanes;  /* [FORK KINDS] */
                const int br_lanes  = s_forks[i].br_lanes;
                const int fi = (int)i;
                int k;

                /* 1. FORK span F: full width, type 8, link_next -> corridor. */
                {
                    const TG_Node *a = &nl->v[F], *b = &nl->v[F + 1];
                    int ox = tg_round(a->x), oy = tg_round(a->y), oz = tg_round(a->z);
                    int lvi = tg_append_row(&verts, &vtx_count, a, lanes,
                                            a->width, 0.0, ox, oy, oz);
                    int rvi = tg_append_row(&verts, &vtx_count, b, lanes,
                                            b->width, 0.0, ox, oy, oz);
                    tg_patch_span(&spans, F, 8, lanes, lvi, rvi, b0, -1, ox, oy, oz);
                }
                /* 2. MAIN half carriageway [F+1 .. F+L]. */
                for (k = 1; k <= L; k++) {
                    const int si = F + k;
                    const TG_Node *a = &nl->v[si], *b = &nl->v[si + 1];
                    int ox = tg_round(a->x), oy = tg_round(a->y), oz = tg_round(a->z);
                    int lvi = tg_append_row(&verts, &vtx_count, a, main_half,
                                            a->width * tg_fork_main_wscale(fi),
                                            tg_fork_main_shift(fi, a->width),
                                            ox, oy, oz);
                    int rvi = tg_append_row(&verts, &vtx_count, b, main_half,
                                            b->width * tg_fork_main_wscale(fi),
                                            tg_fork_main_shift(fi, b->width),
                                            ox, oy, oz);
                    tg_patch_span(&spans, si, 1, main_half, lvi, rvi, -1, -1,
                                  ox, oy, oz);
                }
                /* 3. PAD span at b0-1 (the current append head). */
                {
                    const TG_Node *a = &nl->v[F];
                    int ox = tg_round(a->x), oy = tg_round(a->y), oz = tg_round(a->z);
                    int lvi = tg_append_row(&verts, &vtx_count, a, br_lanes,
                                            a->width * s_forks[i].fb, 0.0, ox, oy, oz);
                    int rvi = tg_append_row(&verts, &vtx_count, a, br_lanes,
                                            a->width * s_forks[i].fb, 0.0, ox, oy, oz);
                    tg_append_span(&spans, 1, tg_surface_attr(F), br_lanes,
                                   lvi, rvi, -1, -1, ox, oy, oz);
                }
                /* 4. BRANCH corridor b0..b0+L-1 (right half, bowed, and gaining
                 *    lanes over its interior -- see tg_branch_lane_gain). Each
                 *    row's point count comes from the SAME helper the road mesh
                 *    uses, so strip and mesh cannot drift apart. */
                for (k = 0; k < L; k++) {
                    const TG_Node *a = &nl->v[F + 1 + k], *b = &nl->v[F + 2 + k];
                    const int ln = tg_fork_br_lanes_at(fi, k);
                    const double wn = tg_fork_br_wscale(fi, k);
                    const int lnf = tg_fork_br_lanes_at(fi, k + 1);
                    const double wf = tg_fork_br_wscale(fi, k + 1);
                    int ox = tg_round(a->x), oy = tg_round(a->y), oz = tg_round(a->z);
                    /* A span's two rows must have the same POINT COUNT (they are
                     * the two edges of one quad grid), so a span where the gain
                     * rounds up uses the finer subdivision for both rows.
                     *
                     * Their WIDTHS are independent, and giving both rows the
                     * wider end's width -- which is what this did until
                     * 2026-08-27 -- is what produced the reported "sudden
                     * changes of lane widths on right track branches": the
                     * corridor became a staircase of constant-width spans that
                     * stepped wherever the rounding tipped over. Each row now
                     * carries its OWN width, so consecutive spans meet at
                     * identical outer points (the collision rails are row points
                     * 0 and lanes_here) and the widening reads as a taper. */
                    const int lanes_here = (lnf > ln) ? lnf : ln;
                    int lvi = tg_append_row(&verts, &vtx_count, a, lanes_here,
                                            a->width * wn,
                                            tg_fork_br_shift(fi, k, a->width),
                                            ox, oy, oz);
                    int rvi = tg_append_row(&verts, &vtx_count, b, lanes_here,
                                            b->width * wf,
                                            tg_fork_br_shift(fi, k + 1, b->width),
                                            ox, oy, oz);
                    int type = (k == 0) ? 9 : ((k == L - 1) ? 10 : 1);
                    int nxt  = (k == L - 1) ? R : -1;
                    int prv  = (k == 0) ? F : -1;
                    tg_append_span(&spans, type, tg_surface_attr(F + 1 + k),
                                   lanes_here, lvi, rvi, nxt, prv, ox, oy, oz);
                }
                /* 5. REJOIN span R: full width, type 11, link_prev -> sentinel. */
                {
                    const TG_Node *a = &nl->v[R], *b = &nl->v[R + 1];
                    int ox = tg_round(a->x), oy = tg_round(a->y), oz = tg_round(a->z);
                    int lvi = tg_append_row(&verts, &vtx_count, a, lanes,
                                            a->width, 0.0, ox, oy, oz);
                    int rvi = tg_append_row(&verts, &vtx_count, b, lanes,
                                            b->width, 0.0, ox, oy, oz);
                    if (R >= 0 && R < ring)
                        tg_patch_span(&spans, R, 11, lanes, lvi, rvi, -1,
                                      sentinel_end, ox, oy, oz);
                }
                TD5_LOG_I(LOG_TAG, "trackgen: fork %u %s F=%d len=%d corridor=%d..%d "
                          "rejoin=%d sep=%.2f split %d->%d+%d%s (ring=%d)", i,
                          tg_fork_kind_name(s_forks[i].kind), F, L, b0,
                          sentinel_end, R, s_forks[i].sep, lanes, main_half, br_lanes,
                          tg_fork_is_avenue((int)i) ? " divider" : "", ring);
            }
        }
        s_ring_len = ring;
    }

    if (spans.oom || verts.oom) {
        TD5_LOG_E(LOG_TAG, "trackgen: out of memory building strip");
        ok = 0;
    }

    if (ok) {
        const int emitted = (int)(spans.len / 24);
        const unsigned int vtx_off =
            (unsigned int)(TD5_TG_SPAN_OFFSET + 24 * emitted);

        tg_put_u32(out, TD5_TG_SPAN_OFFSET);
        /* Ring length = the MAIN ROAD span count. With a branch, `emitted`
         * also counts the pad + corridor spans, which must sit OUTSIDE the
         * ring -- that is what makes branch spans normalize through the jump
         * table. Without a branch the two are identical.
         *
         * Shipped level001 writes span_count-1 but it is a CIRCUIT, where the
         * last span closes onto the first. Copying that made the walker wrap
         * backward off span 0 to the ring end (span_raw oscillated 0 <-> 1798
         * every tick, 952 times in one run). */
        tg_put_u32(out, (unsigned int)(s_ring_len > 0 ? s_ring_len : emitted));
        tg_put_u32(out, vtx_off);
        tg_put_u32(out, (unsigned int)vtx_count);
        tg_put_u32(out, (unsigned int)emitted);   /* total spans */
        /* Jump-entry count at 0x14, then N 6-byte records [lo,hi,base] from 0x18
         * (native TD5 offset; the loader iterates all N). The block must stay
         * exactly TD5_TG_PRE_SPAN_BYTES long -- the loader derives the span count
         * from (vtx_off - span_off)/24 -- so pad out the remainder. */
        {
            int j, nrec = s_fork_count;
            if (nrec > (TD5_TG_PRE_SPAN_BYTES - 4) / 6)
                nrec = (TD5_TG_PRE_SPAN_BYTES - 4) / 6;
            tg_put_u32(out, (unsigned int)nrec);
            for (j = 0; j < nrec; j++) {
                tg_put_u16(out, (unsigned)s_forks[j].cbase);
                tg_put_u16(out, (unsigned)(s_forks[j].cbase + s_forks[j].len - 1));
                tg_put_u16(out, (unsigned)(s_forks[j].F + 1));
            }
            tg_put_zeros(out, TD5_TG_PRE_SPAN_BYTES - 4 - nrec * 6);
        }
        if (!tg_buf_need(out, spans.len + verts.len)) {
            ok = 0;
        } else {
            memcpy(out->b + out->len, spans.b, spans.len);
            out->len += spans.len;
            memcpy(out->b + out->len, verts.b, verts.len);
            out->len += verts.len;
        }
        if (out->len != (size_t)vtx_off + (size_t)vtx_count * 6) {
            TD5_LOG_E(LOG_TAG, "trackgen: strip size mismatch (%zu vs %u)",
                      out->len, (unsigned)(vtx_off + vtx_count * 6));
            ok = 0;
        }
        *out_spans = emitted;
        TD5_LOG_I(LOG_TAG, "trackgen: strip = %d spans, %d vertices, %zu bytes",
                  emitted, vtx_count, out->len);
    }

    tg_buf_free(&spans);
    tg_buf_free(&verts);
    return ok && !out->oom;
}

/* ---------------------------------------------------- LEFT/RIGHT.TRK ----- */
/* 3 bytes per span, exactly ring-length rows (short tables are what causes the
 * out-of-bounds read documented at td5_track.c:3503).
 *   byte0 = lateral corridor position, 0 = left rail .. 255 = right rail
 *   byte1 = local heading, 128 = straight ahead
 *   byte2 = authored corner-speed cap, 255 = uncapped (curvature governs)
 */
int tg_emit_routes(const TG_NodeList *nl, int nspans,
                          int lateral, TG_Buf *out)
{
    int i;
    for (i = 0; i < nspans; i++) {
        /* `nspans` is the TOTAL strip span count, which with branches includes
         * each fork's pad + corridor tail -- but the centerline only has the
         * main-ring nodes, so nl->v[i] for i >= nl->count READ PAST THE ARRAY
         * (uninitialised realloc slack at best, heap overread at worst) and put
         * garbage headings in the table the AI steers by. Map an appended span
         * to the main node its corridor runs beside instead; the pad span, which
         * has no geometry of its own, takes the last real node. */
        int ni = i;
        if (ni > nl->count - 2) {
            int ck = 0, fi = tg_fork_of_corridor(i, &ck);
            ni = (fi >= 0) ? s_forks[fi].F + 1 + ck : nl->count - 2;
            if (ni > nl->count - 2) ni = nl->count - 2;
            if (ni < 0) ni = 0;
        }
        /* byte1 is the ABSOLUTE 12-bit heading, not a deflection: the engine
         * recovers it as heading = (byte * 0x102C) >> 8 (td5_ai.c:1280, and the
         * same formula in ai_route_heading_for_actor at td5_ai.c:305), so invert
         * exactly that. Yaw convention is forward = (sin h, cos h) --
         * td5_physics.c, cited at td5_ai.c:1306 -- hence atan2(tx, tz).
         *
         * Getting this wrong is not subtle: encoding a deflection here (128 =
         * straight) spawned every car pointing the wrong way, and with
         * auto-throttle they drove backwards off the start line into the void. */
        double h = atan2(nl->v[ni].tx, nl->v[ni].tz) * 4096.0 / (2.0 * TD5_TG_PI);
        int h12 = tg_round(h) & 0xFFF;
        int hb  = tg_round((double)h12 * 256.0 / 4140.0);   /* 4140 = 0x102C */

        /* byte < 4 is a junction-zone sentinel, not a heading. The axis offset
         * (TD5_TG_AXIS_HEADING) keeps us clear of it; clamp as a backstop. */
        if (hb < 4)   hb = 4;
        if (hb > 253) hb = 253;

        tg_put_u8(out, (unsigned)lateral);
        tg_put_u8(out, (unsigned)hb);
        tg_put_u8(out, 255);
    }
    return !out->oom;
}

/* Finish span for a ring of `ring` main-road spans, or -1 if the ring is too
 * short to hold a grid, a race and a run-off. Pure function of the ring length
 * and the fork table, so the LEVELINF writer and the banner emitter agree
 * without either of them storing it. */
int tg_finish_span(int ring)
{
    int runoff = td5_env_int("TD5RE_AUTOTRACK_RUNOFF", TD5_TG_RUNOFF_SPANS,
                             0, 600);
    int lo = TD5_TG_GRID_SPAN + 60;      /* shortest race worth having */
    int fs, guard;

    if (ring <= lo) return -1;
    /* A short track cannot afford the full run-off; give it what is left after
     * the minimum race distance rather than refusing to place a finish. */
    if (ring - runoff <= lo) runoff = ring - lo;
    fs = ring - runoff;

    /* Never put the finish line inside a fork's split region (the line would
     * cross two separated half carriageways, so a banner over it would either
     * miss the road or straddle the gore) nor inside a tunnel run (the gantry
     * would stand through the roof). Walk it back to clear ground. */
    for (guard = 0; guard < ring && fs > lo &&
         (tg_span_in_fork_clear(fs) || tg_span_in_tunnel(fs)); guard++)
        fs--;
    if (fs <= lo) return -1;
    return fs;
}

/* ------------------------------------------------------ LEVELINF.DAT ----- */
/* 100 bytes. The level loader reads DWORD[0] (1 = circuit, else
 * point-to-point) and keeps the rest as opaque environment config. */
int tg_emit_levelinf(const TD5_TrackGenSpec *spec, int nspans,
                            TG_Buf *out)
{
    /* Checkpoints are compared against the NORMALIZED main-ring span, so they
     * are placed on the ring -- not on `nspans`, which also counts the appended
     * corridors. s_ring_len is set by tg_emit_strip, which always runs first. */
    const int ring = (s_ring_len > 0) ? s_ring_len : nspans;
    const int finish = tg_finish_span(ring);
    int cp_count = 4, i;
    int cp_span[7];

    for (i = 0; i < 7; i++) cp_span[i] = 0;
    if (ring < 200) cp_count = 2;
    if (finish > 0) {
        /* Evenly spaced from the grid to the finish, LAST one exactly on the
         * finish span -- that crossing is what ends the race. */
        const int span0 = TD5_TG_GRID_SPAN;
        for (i = 0; i < cp_count; i++) {
            cp_span[i] = span0 + (int)((long)(finish - span0) * (i + 1)
                                       / cp_count);
            tg_acct(TG_ACCT_CHECKPOINT, cp_span[i]);
        }
        TD5_LOG_I(LOG_TAG, "trackgen: finish span %d of ring %d (%d spans of "
                  "run-off past the line), %d checkpoints",
                  finish, ring, ring - finish, cp_count);
    } else {
        /* Ring too short for a grid + race + run-off: fall back to the old
         * proportional placement rather than shipping a track that cannot be
         * finished. */
        for (i = 0; i < cp_count; i++)
            cp_span[i] = (int)((long)ring * (i + 1) / (cp_count + 1));
        TD5_LOG_W(LOG_TAG, "trackgen: ring %d too short to place a finish with "
                  "run-off; using proportional checkpoints", ring);
    }

    tg_put_u32(out, spec->circuit ? 1u : 0u);   /* 0x00 track_type */
    tg_put_u32(out, 1);                         /* 0x04 smoke_enable */
    tg_put_u32(out, (unsigned)cp_count);        /* 0x08 checkpoint_count */
    for (i = 0; i < 7; i++)                     /* 0x0C checkpoint_spans */
        tg_put_u32(out, (unsigned)cp_span[i]);
    tg_put_u32(out, 2);                         /* 0x28 weather_type */
    tg_put_u32(out, 0);                         /* 0x2C density_pair_count */
    tg_put_u32(out, 0);                         /* 0x30 traffic_enable */
    tg_put_zeros(out, 24);                      /* 0x34 density_pairs 12xu16 */
    tg_put_zeros(out, 8);                       /* 0x4C pad */
    /* 0x54 sky_animation_index. Shipped circuits use 36, point-to-point tracks
     * use -1. This forced 36 onto a POINT-TO-POINT track on the theory that -1
     * was why a generated track rendered against a flat clear colour -- but that
     * was written [UNCERTAIN] and untested, and the missing FORWSKY.png it names
     * as the other candidate has since been fixed (tg_install_sky copies one in,
     * and the renderer now logs it loading at 256x256, probe class=SUNNY).
     *
     * Being chased 2026-08-26: the auto track shows a large dark region overhead
     * that is NOT geometry (survives every terrain/tunnel knob) and NOT weather
     * (LEVELINF says none, runtime confirms particles=0). The sky draws with
     * z_func=ALWAYS/z_write=0, so anywhere it fails to cover you see the clear
     * colour -- which is what that dark region looks like. Knob so the value can
     * be swept without a rebuild; -1 is the shipped P2P value.
     *
     * RESULT: swept to -1, frame IDENTICAL -- this field is NOT the cause, so
     * the default stays at the value that has actually been driven. The next
     * suspect is the fog/background COLOUR written at 0x60..0x62 just below,
     * which this generator leaves at 0,0,0: if the renderer clears to the
     * level's background colour, every part of the view the sky band does not
     * cover is filled with BLACK, which is exactly what the dark region is. */
    tg_put_u32(out, (unsigned)td5_env_int("TD5RE_AUTOTRACK_SKY_ANIM",
                                          36, -1, 64));
    tg_put_u32(out, (unsigned)nspans);          /* 0x58 total_span_count */
    tg_put_u32(out, 0);                         /* 0x5C fog_enabled */
    tg_put_u8(out, 0);                          /* 0x60 fog r */
    tg_put_u8(out, 0);                          /* 0x61 fog g */
    tg_put_u8(out, 0);                          /* 0x62 fog b */
    tg_put_u8(out, 0);                          /* 0x63 pad */
    return !out->oom;
}

/* TD5_TG_SPANS_PER_ENTRY moved up near TD5_TG_MAX_SPANS (the guard/meshtag code
 * needs it too); entry = span >> 2. */

void tg_put_f32(TG_Buf *buf, double v)
{
    float f = (float)v;
    unsigned int u;
    memcpy(&u, &f, sizeof(u));
    tg_put_u32(buf, u);
}

/* Left and right road edge at fraction f along span si. */
void tg_road_edge(const TG_NodeList *nl, int si, double f, double shift,
                         double wscale,
                         double *lx, double *ly, double *lz,
                         double *rx, double *ry, double *rz)
{
    const TG_Node *a = &nl->v[si];
    const TG_Node *b = &nl->v[si + 1];
    double x = a->x + (b->x - a->x) * f;
    double y = a->y + (b->y - a->y) * f;
    double z = a->z + (b->z - a->z) * f;
    double w = (a->width + (b->width - a->width) * f) * wscale;
    double tx = a->tx + (b->tx - a->tx) * f;
    double tz = a->tz + (b->tz - a->tz) * f;
    double m = sqrt(tx * tx + tz * tz);
    if (m < 1e-9) { tx = 0.0; tz = 1.0; m = 1.0; }
    tx /= m; tz /= m;
    /* Left of travel is (tz, -tx), matching the strip row order. `shift` slides
     * the whole cross-section along that axis and `wscale` scales its width --
     * 1.0/0 for the plain main road, 0.5 + a half-width offset for the split
     * fork carriageways, so the road mesh follows the strip rows exactly. */
    x += tz * shift; z -= tx * shift;
    *lx = x + tz * (w * 0.5); *ly = y; *lz = z - tx * (w * 0.5);
    *rx = x - tz * (w * 0.5); *ry = y; *rz = z + tx * (w * 0.5);
}

/* ============ SECTION: per-span side buffers (parallel groundwork) =========
 * `terrain` is ~31 percent of a build and every byte of it comes from
 * tg_emit_far_band, which contains no caller of tg_r12_flora_accept -- so it
 * has no cross-entry order dependency and can be computed for all spans up
 * front, eventually in parallel.
 *
 * The point of doing it as a SIDE BUFFER spliced back in at the same position,
 * rather than restructuring the entry loop, is that the mesh SEQUENCE inside
 * each entry is unchanged -- so the result is verifiable byte-for-byte against
 * the serial build, which is the only test strong enough to trust here.
 *
 * Two things travel with the bytes and must be re-based on splice:
 *   - moff[]: the emitter records each mesh's start offset. Offsets are stored
 *     RELATIVE to the side buffer and get +base added on splice, which keeps
 *     moff ascending -- tg_guard_validate_entry compacts the entry buffer with
 *     a single forward write cursor and its own comment records that a
 *     descending offset walks that cursor off the end of the allocation.
 *   - guard marks: tg_emit_far_band marks byte ranges of its own output.
 *     Captured by snapshotting s_guard_ex_n around the emit, stored relative,
 *     and re-marked on splice. Getting these wrong would make the on-road
 *     guard mis-classify a mesh and drop or keep the wrong geometry, so they
 *     are as load-bearing as the bytes.
 */
static int tg_buf_append(TG_Buf *buf, const unsigned char *src, size_t n)
{
    if (n == 0) return 1;
    if (!tg_buf_need(buf, n)) return 0;
    memcpy(buf->b + buf->len, src, n);
    buf->len += n;
    return 1;
}

static TG_SideRec *s_side_terr;

static int         s_side_terr_n;

/* Non-zero would mean the pre-pass and the real run could have disagreed about
 * the mesh budget, i.e. the side buffer is no longer a faithful substitute.
 * Reported as a WARNING because it is a silent-wrong-output condition. */
static long        s_side_budget_risk;

static long        s_side_overflow;

/* Latched once per build from TD5RE_AUTOTRACK_SIDEBUF so the hot dispatch does
 * not do a getenv per span. */
static int         s_side_use;

static void tg_side_terrain_free(void)
{
    int i;
    if (!s_side_terr) return;
    for (i = 0; i < s_side_terr_n; i++) tg_buf_free(&s_side_terr[i].buf);
    free(s_side_terr);
    s_side_terr = NULL;
    s_side_terr_n = 0;
}

/* [S2f] WORK TIME vs WALL TIME for the pre-pass.
 *
 * A sampling profiler needs admin on this box (xperf/wpr/nsys all want kernel
 * ETW), but the question that separates the remaining suspects does not need
 * stacks -- it needs to know whether the parallel region does MORE WORK or
 * merely takes longer:
 *
 *   sum(job) ~= serial sum, wall >> sum/threads  -> synchronisation or load
 *                                                   imbalance; the work itself
 *                                                   is fine.
 *   sum(job) >> serial sum                       -> each unit genuinely got
 *                                                   slower: contention, false
 *                                                   sharing or memory
 *                                                   bandwidth.
 *
 * Accumulated per thread on the same slot rows the counters use, so measuring
 * cannot itself contend. Written once per span, so the timer cost is noise. */
static uint64_t s_side_job_us[TG_ACCT_SLOTS];

static long     s_side_job_n[TG_ACCT_SLOTS];

static int tg_side_terrain_one(TG_SideJob *j, int si)
{
    const uint64_t t0 = td5_plat_time_us();
    const int slot = tg_acct_row();
    int rc = 1;
    TG_SideRec *r = &s_side_terr[si];
    TG_FBHook hook;
    size_t scratch[TG_SIDE_MAX_MESH];
    int nm = 0, snap, i;

    r->ok = 1;
    /* The same two gates the real dispatcher applies before it would ever
     * reach the terrain hook: appended corridor/pad spans carry road only,
     * and a tunnel span takes the tunnel branch instead. */
    if (si >= j->ring || tg_span_in_tunnel(si)) goto done;

    hook.nl = j->nl; hook.si = si; hook.nspans = j->nspans; hook.lanes = j->nl->v[si].lanes;   /* [LANES] */
    hook.b = &k_biomes[tg_biome_for_span(si)];
    hook.blk = &r->buf; hook.moff = scratch; hook.nmesh = &nm;
    hook.maxmesh = TG_SIDE_MAX_MESH;

    snap = s_guard_ex_n(slot);
    r->ok = tg_emit_fb_terrain(&hook);

    if (nm > TG_SIDE_MAX_MESH || (s_guard_ex_n(slot) - snap) > TG_SIDE_MAX_MESH) {
        TG_ATOMIC_ADD_LONG(&j->overflow, 1L);   /* would truncate -- refuse */
        s_guard_ex_n(slot) = snap;
        rc = 0;
        goto done;
    }
    for (i = 0; i < nm; i++) r->moff[i] = scratch[i];
    r->nmoff = nm;
    for (i = snap; i < s_guard_ex_n(slot); i++) {
        const int k = i - snap;
        r->mlo[k]   = s_guard_ex_lo[slot][i];
        r->mhi[k]   = s_guard_ex_hi[slot][i];
        r->mkind[k] = s_guard_ex_kind[slot][i];
        r->msi[k]   = s_guard_ex_si[slot][i];
    }
    r->nmark = s_guard_ex_n(slot) - snap;
    /* The marks belong to the ENTRY that splices these bytes, not to the
     * pre-pass, so unwind them here and re-mark at the splice with the
     * real offsets. */
    s_guard_ex_n(slot) = snap;

    rc = r->ok ? 1 : 0;

done:
    /* [S2f] This thread's share of the pre-pass work, whichever way it exited. */
    s_side_job_us[slot] += td5_plat_time_us() - t0;
    s_side_job_n[slot]++;
    return rc;
}

static void tg_side_terrain_job(int si, void *ctx)
{
    TG_SideJob *j = (TG_SideJob *)ctx;
    /* A refusal cannot stop the other spans mid-flight the way the serial
     * loop's early return did, so it is recorded and the caller refuses the
     * whole pre-pass afterwards. Same outcome (inline terrain), just reached
     * after the remaining spans have run. */
    if (!tg_side_terrain_one(j, si)) j->failed = 1;
}

/* Threading the pre-pass is OPT-IN (TD5RE_AUTOTRACK_SIDEBUF_MT), because the
 * last attempt at it produced three different tracks from three runs. The byte
 * test is the gate, not the reasoning. */
static int tg_side_mt(void)
{
    return td5_env_flag_off("TD5RE_AUTOTRACK_SIDEBUF_MT")
           && td5_jobs_worker_count() > 0;
}

static int tg_side_terrain_build(const TG_NodeList *nl, int nspans, int lanes)
{
    TG_SideJob job;
    int si;

    s_side_budget_risk = 0;
    s_side_overflow = 0;
    s_side_terr = (TG_SideRec *)calloc((size_t)nspans, sizeof(TG_SideRec));
    if (!s_side_terr) return 0;
    s_side_terr_n = nspans;

    memset(s_side_job_us, 0, sizeof(s_side_job_us));
    memset(s_side_job_n,  0, sizeof(s_side_job_n));

    job.nl = nl; job.nspans = nspans; job.lanes = lanes;
    job.ring = (s_ring_len > 0) ? s_ring_len : nspans;
    job.overflow = 0; job.failed = 0;

    if (tg_side_mt()) {
        td5_jobs_parallel_for(nspans, tg_side_terrain_job, &job);
    } else {
        for (si = 0; si < nspans; si++)
            if (!tg_side_terrain_one(&job, si)) { job.failed = 1; break; }
    }

    s_side_overflow = job.overflow;
    /* [S2f] Work vs wall. threads>1 with sum ~= the serial sum means the
     * parallelism is sound and the wall time is synchronisation; sum far ABOVE
     * the serial sum means each unit got slower, which is contention, false
     * sharing or bandwidth -- and no amount of scheduling fixes that. */
    {
        uint64_t sum = 0; long n = 0; int i, threads = 0;
        for (i = 0; i < TG_ACCT_SLOTS; i++) {
            if (!s_side_job_n[i]) continue;
            threads++; sum += s_side_job_us[i]; n += s_side_job_n[i];
        }
        TD5_LOG_W(LOG_TAG, "[R14 GENPERF] side-prepass: %s, %ld spans over %d "
                  "thread(s), work-sum %.0f ms", tg_side_mt() ? "THREADED" : "serial",
                  n, threads, sum / 1000.0);
    }
    return job.failed ? 0 : 1;
}

/* Splice span `si`'s pre-built terrain into the live entry buffer. */
static int tg_side_terrain_splice(int si, TG_Buf *meshes, size_t *moff,
                                  int *nmesh, int maxmesh)
{
    const TG_SideRec *r;
    size_t base;
    int i;

    if (!s_side_terr || si < 0 || si >= s_side_terr_n) return 1;
    r = &s_side_terr[si];
    if (!r->ok) return 0;
    if (r->buf.len == 0) return 1;               /* this span emits nothing */

    /* Faithfulness check, not a safety net: the emitter skipped itself when
     * *nmesh + 2 >= maxmesh, and the pre-pass evaluated that against an empty
     * record. If the live count is high enough that the real run would have
     * skipped, the two disagree and the splice is not equivalent. */
    if (*nmesh + 2 >= maxmesh) { s_side_budget_risk++; return 1; }
    if (*nmesh + r->nmoff > maxmesh) { s_side_budget_risk++; return 1; }

    base = meshes->len;
    if (!tg_buf_append(meshes, r->buf.b, r->buf.len)) return 0;
    for (i = 0; i < r->nmoff; i++) moff[(*nmesh)++] = base + r->moff[i];
    for (i = 0; i < r->nmark; i++)
        tg_guard_mark(base + r->mlo[i], base + r->mhi[i],
                      (int)r->mkind[i], r->msi[i]);
    return 1;
}

static TG_ScnCtx s_scn;

/* ===================== [R22 CORRIDOR item 4] FORK-CORRIDOR SCENERY ==========
 * "there's basically no geometry rendered here, only tiles at the side of the
 * road, no buildings, no scenery or trees" -- driving a fork/bypass corridor.
 *
 * MEASURED (seed 2082186171, TD5RE_R21_ROLL=0, per display-list entry): the
 * main ring runs 29 meshes/entry, the 68 appended corridor entries only 8.6,
 * and every entry on the whole track with <= 8 meshes is a corridor. The L2
 * scenery loop below is gated `if (si >= ring) continue;`, so a corridor span
 * gets its road quad, a branch verge/sidewalk and one sparse roadside tree
 * (tg_emit_branch_flora) and nothing else: no tree line, no roadside density.
 *
 * WHY NOT JUST RUN THE L2 HOOKS FOR CORRIDOR SPANS. Every L2 emitter takes a
 * TG_FBHook keyed by `si` and both indexes nl->v[si] AND runs span-scoped
 * lookups (biome, tunnel, water) on that same si -- and a corridor span has NO
 * node in the centreline (nl->v only holds the ring). Its road is built from
 * the base MAIN node mb = F+1+ck plus a lateral bow (tg_fork_br_shift). Feeding
 * an emitter mb without the bow would place its scenery on the MAIN road and
 * z-fight it; feeding it a synthetic node would still break every span-scoped
 * lookup that reuses si as an index. So corridor scenery needs a corridor FRAME
 * (main node + lateral offset) that the emitter honours, and the emitters in
 * td5_tg_terrain.c cannot be handed one without rewriting their si contract.
 *
 * WHY TREES, NOT GROUND. The outboard side of the fork is a RIDGE GATE, not a
 * ground gate (tg_emit_fb_terrain, R8 TERRAIN): over a fork the main road still
 * lays its far-band GROUND out to tg_far_reach() (30000) across and past the
 * corridor -- tg_topo_road_cap is ring-only and never caps at a corridor -- but
 * SUPPRESSES the tree wall there so a distant skyline does not land on the
 * branch. So the ground under a corridor already exists; a ground apron here
 * would z-fight it (the single biggest risk this round was warned about). What
 * is genuinely missing is the VERTICAL scenery, so this plants a corridor tree
 * line and nothing else -- fail closed, standing on ground that is already there.
 *
 * FRAME. Reuses the exact placement authority the branch flora and the main
 * road's own roadside trees use: tg_carriageway_clear_gap(nl, mb, fs, ...)
 * pushes a setback measured from the MAIN road edge out past the bowed corridor
 * at its ACTUAL width, and tg_flora_plant(nl, mb, b, fs, gap, tw, ...) turns
 * that into a trunk world point + a base_y sampled off the SAME topo chain the
 * far-band ground is built from (so the trunk sits ON the terrain, not at road
 * height) and vetoes a trunk over water. Because mb < ring, both are honoured.
 * An outboard band: `base` clears the corridor once, then each tree steps a
 * further TD5_TG_R22_CORR_STEP out so the flank reads as a receding line, not a
 * single hedge, all within the 30000 the far-band ground already reaches.
 *
 * DETERMINISM. No tg_rand() draw (that would shift every later draw and move
 * every seed's road); tg_roll_hash_at mixes the build seed with mb, exactly the
 * discipline the rest of the generator uses for per-position decisions.
 *
 * Billboard biomes only: a paved corridor wants buildings, which are keyed to
 * the street graph and left for a later round, so emitting nothing there is the
 * fail-closed choice, not a bare grass strip. TD5RE_R22_CORRIDOR_SCENERY=0
 * restores d7b11c5e byte-for-byte. */
#define TD5_TG_SALT_R22_CORRIDOR  0x22040400u   /* round 22, item 4 */
#define TD5_TG_R22_CORR_TREES     3      /* trees attempted per corridor span */
#define TD5_TG_R22_CORR_GAP0    600.0    /* base setback the clear-gap floors  */
#define TD5_TG_R22_CORR_STEP   2600.0    /* outward step per tree in the line  */

static long s_r22_corr_spans;    /* corridor spans that gained >=1 tree */
static long s_r22_corr_trees;    /* corridor trees planted this build   */

static int tg_emit_corridor_flora(const TG_NodeList *nl, int mb, int ck, int L,
                                   int fi, const TG_Biome *b, TG_Buf *blk,
                                   size_t *moff, int *nmesh, int acct_si)
{
    const double fs = (double)tg_fork_side(fi);
    double base;
    int i, planted = 0;

    (void)ck; (void)L;
    if (!td5_env_flag_on("TD5RE_R22_CORRIDOR_SCENERY")) return 1;
    if (!b->billboard || b->tree_n <= 0) return 1;   /* fail closed: no trees here */
    if (mb < 0 || mb + 1 >= nl->count) return 1;

    /* One cleared setback shared by the whole line, then step out from it: the
     * clear-gap would floor every tree to the SAME distance (it returns the max
     * of the requested gap and the reach past the corridor), so the band step
     * has to be added ON TOP of the cleared base, not inside the request. */
    base = tg_carriageway_clear_gap(nl, mb, fs, TD5_TG_R22_CORR_GAP0,
                                    TD5_TG_CARRIAGEWAY_MARGIN);

    for (i = 0; i < TD5_TG_R22_CORR_TREES; i++) {
        const unsigned int h =
            tg_roll_hash_at(TD5_TG_SALT_R22_CORRIDOR + (unsigned)i, mb);
        const int    tv  = b->tree_set[(h >> 13) % (unsigned)b->tree_n];
        const double jit = 0.8 + (double)((h >> 9) % 41) * 0.01;   /* 0.80..1.20 */
        const double tw  = (double)k_tree_pages[tv].w * jit;
        const double th  = (double)k_tree_pages[tv].h * jit;
        const double gap = base + (double)i * TD5_TG_R22_CORR_STEP
                         + (double)((h >> 5) % 700);
        double cx, cz, base_y;

        if (*nmesh + 2 >= TG_MAX_MESHES_PER_ENTRY) break;   /* keep headroom */
        /* Placement + ground drop + water veto, all off the main node mb. */
        if (!tg_flora_plant(nl, mb, b, fs, gap, tw, &cx, &cz, &base_y))
            continue;                                       /* over water etc. */
        if (!tg_r12_flora_accept(mb, "corridor", tg_tree_slot(tv), fs,
                                 cx, cz, tw, th))
            continue;                                       /* spacing dedup   */
        moff[(*nmesh)++] = blk->len;
        if (!tg_emit_billboard_mesh(blk, cx, base_y, cz, tw * 0.5, th,
                                    tg_tree_slot(tv), 1))
            return 0;
        tg_acct_n(TG_ACCT_R7_BRANCH, acct_si, 1);
        planted++;
        /* [R22 CORRIDOR DIAG] Prove PLACEMENT, not just count: a tree's count
         * is equally consistent with it standing out on the corridor and with
         * it stacked on the main carriageway (the fail-open z-fight). This logs,
         * per planted tree, the corridor bow at this span and the trunk's
         * distance to BOTH centrelines. On-corridor means d_main tracks the bow
         * (large, grows with it) while d_corr stays at the verge setback. `tlat`
         * is the trunk's signed lateral off the MAIN node along its normal, so
         * a value near 0 would be a tree ON the main road. Dev-only, gated OFF;
         * no geometry effect. */
        if (td5_env_flag_on("TD5RE_R22_CORRIDOR_DIAG")) {
            const TG_Node *a = &nl->v[mb];
            const double sh   = tg_fork_br_shift(fi, ck, a->width);  /* bow */
            const double corx = a->x + a->tz * sh;
            const double corz = a->z - a->tx * sh;
            const double dmx  = cx - a->x, dmz = cz - a->z;
            const double dcx  = cx - corx, dcz = cz - corz;
            const double tlat = dmx * a->tz - dmz * a->tx;
            TD5_LOG_I(LOG_TAG, "[R22 CORRIDOR DIAG] si=%d mb=%d bow=%.0f "
                      "tlat=%.0f d_main=%.0f d_corr=%.0f",
                      acct_si, mb, sh, tlat,
                      sqrt(dmx * dmx + dmz * dmz),
                      sqrt(dcx * dcx + dcz * dcz));
        }
    }
    if (planted) { s_r22_corr_spans++; s_r22_corr_trees += planted; }
    return 1;
}

static int tg_scenery_begin(const TG_NodeList *nl, int nspans, int lanes)
{
    /* `nspans` is the FULL strip span count. With branches it INCLUDES each
     * fork's pad + corridor tail; the main ring is s_ring_len and the corridors
     * are appended after it (fork descriptors in s_forks). The centerline `nl`
     * only has the main-ring nodes, so a corridor span takes its geometry from
     * the base main node nl->v[F+1+k] plus the branch shift. */
    const int branch_active = tg_branches_enabled() && s_fork_count > 0;
    const int ring = branch_active ? s_ring_len : nspans;

    /* [R22 CORRIDOR item 4] per-build corridor-scenery counters. */
    s_r22_corr_spans = 0;
    s_r22_corr_trees = 0;
    /* [R9 INFRA] per-build counters, reset with the rest of the accounting. */
    s_r9_infra_props = 0;
    s_r9_infra_ponds = 0;
    s_r9_infra_reported = 0;
    /* [R12 PROPS] this round's two share counters, same lifetime. */
    s_r12_bench_form = 0;
    s_r12_sign_kept = 0;
    s_r12_sign_ctx = 0;
    s_r12_sign_rate = 0;
    /* [R13 PROPS] this round's four share counters, same lifetime. */
    s_r13_bench_end_uv = 0;
    s_r13_plank_crop = 0;
    s_r13_awn_form = 0;
    s_r13_awn_kept = 0;
    s_r13_awn_ctx = 0;
    s_r13_animal_kept = 0;
    s_r13_animal_town = 0;
    /* [R14 FCROSS] forest-crossing clearance counters, same lifetime. */
    s_r14_fcross_animal_moved = 0;
    s_r14_fcross_side_hit = 0;
    s_r14_fcross_prop_skip = 0;
    s_r14_fcross_pave_stop = 0;
    s_r14_fcross_worn = 0;
    /* [R10 SPAN66] side-street occupancy counters, same lifetime. */
    s_r10_prop_skipped = 0;
    s_r10_audit_n = 0;
    tg_r10_xs_memo_reset();
    /* [R11 CROSS] crossing/street-mouth barrier counters, same lifetime. */
    s_r11_rail_on_cross   = 0;
    s_r11_rail_on_xstreet = 0;
    /* [R12 GEOM] median end-cap counters, same lifetime. */
    s_r12_median_caps = 0;
    s_r12_median_runs = 0;
    /* Native-faithful fork: the road SPLITS into two half-width carriageways --
     * MAIN (left, main_half lanes, +width/4) and BRANCH (right, br_lanes, bowed)
     * over the appended corridor. Fork/rejoin spans stay full width. */
    const int main_half = lanes / 2;
    const int br_lanes  = lanes - main_half;
    const int nentries = (nspans + TD5_TG_SPANS_PER_ENTRY - 1)
                       / TD5_TG_SPANS_PER_ENTRY;
    /* Per span: ground skirt + road + guardrail + building + up to 3 tunnel
     * pieces + up to 2 bridge pieces + several prop billboards. */
    /* 96 per span, raised from 48 once all five scenery areas of the 2026-08-26
     * batch were emitting at once: a built city span can carry ground + road +
     * gore + rail + facade + storefront + sidewalk + kerb + railing + crossing
     * + 6 lamp pieces + 4 back-row rows + treeline + far band + props, and the
     * budget is per ENTRY of TD5_TG_SPANS_PER_ENTRY spans, so the worst case is
     * four such spans in a row. Overflow is SILENT (the loop just stops adding
     * scenery), which is why it is counted and logged below rather than trusted.
     * Cost is stack only: moff is 96*4*8 = 3 KB. */
    const int rails = tg_guardrails_enabled();
    int nrails = 0;
    int nbudget = 0;                /* entries that ran out of mesh slots */
    TG_Buf *blocks;

    blocks = (TG_Buf *)calloc((size_t)nentries, sizeof(TG_Buf));
    if (!blocks) return 0;

    tg_meshtag_reset(nentries);   /* [PICK] per-(entry,slot) kind sidecar */

    s_guard_rejects = 0;   /* [R7 GUARD] per-build tally, reported below */
    s_guard_residual = 0;
    s_guard_ex_scope_hits = 0;
    s_guard_unsorted = 0;  /* [R10] compaction health, reported below */
    s_guard_unsafe = 0;
    memset(s_guard_rej_kind, 0, sizeof s_guard_rej_kind);
    s_r11_signs = 0;       /* [R11 SIGNS] per-build tally, reported below */
    s_r11_sign_skip_lamp = 0;
    s_r11_sign_skip_street = 0;
    s_r11_sign_skip_side = 0;
    s_r11_sign_left = 0;
    s_r11_sign_right = 0;

    /* [R8 CROSS item 9] Sharp-bend map, built ONCE per generation: the two
     * predicates that carry the continuation (tg_facade_built, tg_block_arm_skew)
     * take a span index and no node list, and a per-call rescan would be
     * quadratic in the span count. Must precede the first facade query. */
    {
        TG_ZONE_BEGIN(TG_ZONE_PREPASS);
        s_tg_progress = 10;
        TG_TV(TG_T_PRE_TURNMAP,  tg_turn_map_build(nl, nspans));
        TG_TV(TG_T_PRE_R8CROSS,  tg_r8_cross_report(nl, nspans));   /* [R8 CROSS] class sweep, opt-in */
        TG_TV(TG_T_PRE_R10CROSS, tg_r10_cross_report(nl, nspans));  /* [R10 CROSS] side-street setback, opt-in */
        TG_TV(TG_T_PRE_R11CITY,  tg_r11_city_report(nl, nspans));   /* [R11 CITY] frontage/junction dump, TD5RE_TG_REPORTS */
        TG_TV(TG_T_PRE_R13JUNC,  tg_r13_junc_report(nl, nspans));   /* [R13 JUNCTION] bend-fold sweep, TD5RE_TG_REPORTS */
        TG_TV(TG_T_PRE_R13FILL,  tg_r13_fill_report(nl, nspans));   /* [R13 FILL] exposed-rear sweep, opt-in */
        tg_r9_city_reset();               /* [R9 CITY] pavement/massing sweep */
        tg_r13_faces_reset();             /* [R13 FACES] run-end return census */
        TG_ZONE_END(TG_ZONE_PREPASS);
        tg_xmemo_reset(1);                /* [R14 GENPERF] tables final -> cache the crossing predicates */
        /* [S0] WARM THE CHEAP BUILD-SCOPE LAZY CACHES HERE, while this is
         * still the only thread. Both are pure functions of state already
         * frozen at this point (the node list, the elevation profile, the
         * biome grid, the fork table), so computing them now cannot change an
         * answer -- it only moves the work out of the region that is about to
         * become parallel, and removes two lazy first-touch writes from it.
         *
         * The water table is warmed UNCONDITIONALLY, which is a deliberate
         * deviation from ba8423da (it warms only when TD5RE_R9_BRIDGE_REPORT
         * is set). Its reader tg_r9_water_audit_mesh DROPS MESHES, so the
         * table is a geometry input, not a report input: with the report off
         * it would still be built, just lazily and inside the parallel
         * region, which is the thing being removed. One pass over the node
         * list, so warming it costs nothing measurable. */
        (void)tg_track_min_y(nl);
        /* [CRASH] Build it OUTRIGHT, not "if stale". The staleness key catches
         * the case that crashed (a second generation with a SHORTER node list,
         * indices out of range), but it cannot catch a second generation whose
         * list happens to land at the same address with the same count: that
         * table is silently WRONG rather than out of range, and it decides
         * which meshes get dropped. Warming here is one pass over the node
         * list, so paying it every generation costs nothing measurable and
         * removes the whole class. */
        tg_r9_water_table_build(nl);
    }

    /* [PARALLEL GROUNDWORK] Build every span's terrain up front. Still serial;
     * the loop inside only ever writes a span's own record, so turning it into
     * a td5_jobs_parallel_for is a later change with no new sharing.
     * TD5RE_AUTOTRACK_SIDEBUF=0 falls back to emitting terrain inline;
     * TD5RE_AUTOTRACK_SIDEBUF_MT=1 runs it across the job pool. */
    s_side_use = td5_env_flag_on("TD5RE_AUTOTRACK_SIDEBUF");
    s_xs_phase = 1;   /* [MEASURE] attribute memo traffic to the terrain phase */
    TG_ZONE_BEGIN(TG_ZONE_SIDEBUILD);
    if (s_side_use && !tg_side_terrain_build(nl, nspans, lanes)) {
        TD5_LOG_W(LOG_TAG, "trackgen: terrain side-buffer build failed "
                  "(overflow=%ld); falling back to inline terrain",
                  s_side_overflow);
        tg_side_terrain_free();
        s_side_use = 0;
    }
    TG_ZONE_END(TG_ZONE_SIDEBUILD);
    s_xs_phase = 0;

    int ok = 1;

    /* publish the derived setup for tg_scenery_entry / _end */
    s_scn.nl = nl; s_scn.nspans = nspans; s_scn.lanes = lanes;
    s_scn.ring = ring; s_scn.main_half = main_half; s_scn.br_lanes = br_lanes;
    s_scn.nentries = nentries; s_scn.rails = rails;
    s_scn.branch_active = branch_active;
    s_scn.nrails = nrails; s_scn.nbudget = nbudget;
    s_scn.blocks = blocks; s_scn.ok = ok; s_scn.active = 1;
    return ok;
}

static int tg_scenery_entry(int e)
{
    /* Alias prologue: the body below is the original loop body verbatim, so
     * it must see the same names its enclosing scope used. */
    const TG_NodeList *nl = s_scn.nl;
    const int nspans = s_scn.nspans, lanes = s_scn.lanes, ring = s_scn.ring;
    const int main_half = s_scn.main_half, br_lanes = s_scn.br_lanes;
    const int nentries = s_scn.nentries, rails = s_scn.rails;
    const int branch_active = s_scn.branch_active;
    TG_Buf *blocks = s_scn.blocks;
    /* [LANES] the track-wide trio is kept for the alias prologue's shape; the
     * body now reads per-span (nl->v[si].lanes) and per-fork (s_forks[fi])
     * counts, so these three are only referenced here. */
    (void)lanes; (void)main_half; (void)br_lanes;
    int nrails = s_scn.nrails, nbudget = s_scn.nbudget;
    int ok = 1;
    (void)nentries;

        const int s0 = e * TD5_TG_SPANS_PER_ENTRY;
        int ns = nspans - s0;
        size_t moff[TG_MAX_MESHES_PER_ENTRY];
        TG_Buf meshes;
        int nmesh = 0, i;

        s_tg_progress = 12 + (80 * e) / (nentries > 0 ? nentries : 1);   /* loading bar */

        if (ns > TD5_TG_SPANS_PER_ENTRY) ns = TD5_TG_SPANS_PER_ENTRY;
        memset(&meshes, 0, sizeof(meshes));
        tg_guard_ex_reset();   /* [R7 GUARD] exempt ranges are per-entry */
        tg_pave_mark_reset();  /* [R9 CITY] pavement provenance, per-entry */

        /* Ground skirt then road, per span. Offsets are RECORDED as meshes are
         * appended -- sizes differ once ground, buildings and road quads are
         * mixed, so they cannot come from a uniform stride. */
        s_tg_emit_t0 = td5_plat_time_us();     /* [R14 GENPERF] per-span emit */
        TG_ZONE_BEGIN(TG_ZONE_ENTRY_L1);
        for (i = 0; i < ns && ok; i++) {
            const int si = s0 + i;

            /* Appended corridor span: the BRANCH (right) half carriageway only,
             * at the bowed geometry of whichever fork owns it. The pad span
             * (si == cbase-1) carries nothing. */
            if (si >= ring) {
                int ck = 0, fi = branch_active ? tg_fork_of_corridor(si, &ck) : -1;
                if (fi >= 0) {
                    const int mb = s_forks[fi].F + 1 + ck;  /* base main node */
                    const int L  = s_forks[fi].len;
                    const int main_half = s_forks[fi].main_lanes;   /* [FORK KINDS] */
                    const int br_lanes  = s_forks[fi].br_lanes;
                    (void)main_half;
                    /* Same lane/width helpers the STRIP rows used, so the
                     * surface you see is the surface you collide with even where
                     * the corridor gains a lane. */
                    const double sep = s_forks[fi].sep;
                    const double wn = tg_fork_br_wscale(fi, ck);
                    const double wf = tg_fork_br_wscale(fi, ck + 1);
                    (void)sep;
                    /* Items 7 & 11: u_scale is CONSTANT along the corridor -- the
                     * base half carriageway (wscale 0.5) carries br_lanes lanes,
                     * so u_scale = br_lanes/0.5 = 2*br_lanes gives U = br_lanes at
                     * the base and one fixed-pitch lane stripe per real lane as it
                     * widens. It does NOT depend on this span's lane count, so the
                     * paint neither stretches with the width taper nor jumps when
                     * a lane is gained (both were the old flat max(ln,lnf) U). */
                    /* [FORK KINDS] the base carriageway (wscale fb) carries
                     * br_lanes lanes, so U = br_lanes there: u_scale = br/fb. */
                    const double u_scale = (double)br_lanes / s_forks[fi].fb;
                    moff[nmesh++] = meshes.len;
                    /* Widths near/far, NOT the wider of the two: the strip rows
                     * taper across the span (see the corridor loop in
                     * tg_emit_strip) and the mesh has to taper with them or the
                     * surface you see stops being the surface you collide with. */
                    if (!tg_emit_road_quad_taper(nl, mb, u_scale,
                                           tg_fork_br_shift(fi, ck, nl->v[mb].width),
                                           tg_fork_br_shift(fi, ck + 1, nl->v[mb + 1].width),
                                           wn, wf, tg_road_page(mb), &meshes))
                        ok = 0;
                    /* [R7 GUARD] the branch carriageway is drivable; it sits deep
                     * inside the -side reach envelope, so mark it exempt. */
                    /* [R8 merge] GUARD replaced the blanket tg_guard_ex_mark()
                     * with the per-kind tg_guard_mark(); SHAPE was written
                     * against the old API. Use the new one -- BRANCHROAD is
                     * span-scoped exempt, which is what this quad needs. */
                    tg_guard_mark(moff[nmesh - 1], meshes.len, TG_GK_BRANCHROAD, si);
                    /* [R8 SHAPE G5] Count the LONG corridor's own road quads, so
                     * "did the long branch actually get built, and over which
                     * spans" is answerable from the element inventory instead of
                     * from a frame. Long forks only -- an ordinary fork already
                     * reports under branch-nodes. */
                    if (tg_branch_is_long(L))
                        tg_acct(TG_ACCT_R8_LONGBRANCH, si);
                    /* Item 9a: the branch carriageway had NO kerb of its own, so
                     * driving a corridor there was road meeting bare ground with
                     * no pavement -- the "missing gaps between road and sidewalk
                     * on branches". Lay a pavement along the branch's OUTER edge,
                     * derived from the SAME shift/width the road just used so it
                     * follows the bow and the widening. Paved biomes only, same
                     * rule the main-ring sidewalk uses. */
                    if (ok) {
                        /* [R14 BRANCH item 2a] HARD cell, not the dithered
                         * span. The main-road pavement has asked
                         * tg_scenery_biome_index since R11 GUARD precisely so a
                         * structural edge cannot flicker across a blend band,
                         * and the corridor pavement was left on the soft
                         * lookup -- so over a boundary the two owners of one
                         * edge could disagree about whether there IS a pavement
                         * and hand off to each other into thin air. One
                         * authority for both, which is also what makes
                         * tg_r14_branch_pave_here able to predict this
                         * emitter's answer from the main span. */
                        const TG_Biome *cb =
                            &k_biomes[td5_env_flag_on("TD5RE_R14_FORK_PAVE")
                                      ? tg_scenery_biome_index(mb)
                                      : tg_biome_for_span(mb)];
                        if (tg_city_sidewalk_w(cb) > 0.0 &&
                            td5_env_flag_on("TD5RE_AUTOTRACK_SIDEWALKS")) {
                            if (!tg_emit_branch_sidewalk(nl, mb, ck, L, fi,
                                                         cb, &meshes,
                                                         moff, &nmesh, si)) ok = 0;
                        /* [R5 item 10] Out-of-town corridor: no city pavement, so
                         * give the outer edge the SAME flat verge band the main
                         * road gets in that biome. Without this a fork through a
                         * tree biome (seed 99991 fork 2 through ORIENTAL) had a
                         * bare right edge -- "no sidewalk on the right of the
                         * branch". */
                        } else if (tg_verge_band_w(cb) > 0.0 &&
                                   td5_env_flag_on("TD5RE_R5_BRANCH_VERGE")) {
                            if (!tg_emit_branch_verge(nl, mb, ck, L, fi,
                                                      tg_verge_band_w(cb),
                                                      &meshes, moff, &nmesh, si)) ok = 0;
                        }
                        /* [R7 item 10] Dress the grass verge with the biome's own
                         * roadside trees. No-op on paved biomes (no billboard set)
                         * and inherently clear of the branch (clear_gap). */
                        if (ok && !tg_emit_branch_flora(nl, mb, cb, &meshes,
                                                        moff, &nmesh, si)) ok = 0;
                        /* [R22 CORRIDOR item 4] the corridor's own tree line,
                         * standing on the far-band ground the main road already
                         * lays past the fork. See tg_emit_corridor_flora. */
                        if (ok && !tg_emit_corridor_flora(nl, mb, ck, L, fi, cb,
                                                          &meshes, moff, &nmesh,
                                                          si)) ok = 0;
                    }
                }
                /* The other appended span is the PAD (si == cbase-1). It is
                 * DEGENERATE by construction -- both of its rows are node F --
                 * and the fork span's own full-width quad already covers that
                 * ground, so giving it a mesh would only z-fight (the Keswick
                 * start-banner lesson). It stays geometry-free on purpose. */
                continue;
            }

            moff[nmesh++] = meshes.len;
            {
                double wsd = tg_water_side(si);
                if (!TG_SUB(TG_SUB_GROUND, tg_emit_ground(nl, si, &meshes, wsd))) { ok = 0; break; }
            }
            /* [R7 GUARD] the ground skirt underlaps the road by design. */
            tg_guard_mark(moff[nmesh - 1], meshes.len, TG_GK_SKIRT, si);
            {
            const size_t road_ex0 = meshes.len;   /* [R7 GUARD] road+gore+divider */
            moff[nmesh++] = meshes.len;
            /* MAIN carriageway is narrowed to the LEFT half over each fork's
             * region [F+1 .. F+len]; elsewhere (incl. the full-width fork and
             * rejoin spans) it is the plain full road. */
            {
                int fi = branch_active ? tg_fork_of_main(si) : -1;
                if (fi >= 0) {
                    const int L = s_forks[fi].len;  (void)L;   /* [FORK KINDS] geometry now comes from the fork helpers */
                    const int main_half = s_forks[fi].main_lanes;   /* [FORK KINDS] */
                    const int br_lanes  = s_forks[fi].br_lanes;
                    const int j = si - s_forks[fi].F - 1;   /* corridor step */
                    if (!tg_emit_road_quad(nl, si, main_half,
                                           tg_fork_main_shift(fi, nl->v[si].width),
                                           tg_fork_main_shift(fi, nl->v[si + 1].width),
                                           tg_fork_main_wscale(fi), tg_road_page(si), &meshes))
                        ok = 0;
                    if (ok) {
                        /* Branch half widths from the SAME helper the corridor
                         * strip rows and mesh use, so the gore always meets the
                         * branch's left edge however wide the taper has made it. */
                        const double sh0 = tg_fork_br_shift(fi, j, nl->v[si].width);
                        const double sh1 = tg_fork_br_shift(fi, j + 1, nl->v[si + 1].width);
                        const double gw0 = nl->v[si].width
                            * tg_fork_br_wscale(fi, j) * 0.5;
                        const double gw1 = nl->v[si + 1].width
                            * tg_fork_br_wscale(fi, j + 1) * 0.5;
                        moff[nmesh++] = meshes.len;
                        /* [R7 item 12] Median surface stable per fork, not the
                         * blended per-span biome that dithered grass/tiles at a
                         * boundary crossing the fork. TD5RE_R7_MEDIAN_STABLE=0
                         * restores the per-span page for an A/B. */
                        {
                            int gore_page =
                                td5_env_flag_on("TD5RE_R7_MEDIAN_STABLE")
                                    ? tg_fork_gore_page(fi)
                                    : k_biomes[tg_biome_for_span(si)].ground_page;
                            /* [R8 item 17] "Avoid green medians within tunnels
                             * like on span 562." The gore's surface is the
                             * BIOME's ground page, and the biome does not stop
                             * at a portal -- so where a fork runs through a bore
                             * (seed 777: fork 510-631 crosses tunnel 560-579,
                             * biome ALPINE) the median between the two
                             * carriageways was grass, indoors. The bore decides
                             * what is legal inside it, the same rule that keeps
                             * sidewalks and guardrails out of a tunnel; a bore
                             * floor is made surface. */
                            /* [R17 MEDIAN item 2] "Avoid medians without a
                             * height difference." A median-width gore with no
                             * raised island (a fork too short for one, R16) is a
                             * flat ground strip that reads as a flush median.
                             * Pave it as ROAD so the fork throat looks like the
                             * road widening, not a median. Bore override below
                             * still wins indoors. TD5RE_R17_GORE_ROAD=0 = A/B. */
                            if (td5_env_flag_on("TD5RE_R17_GORE_ROAD") &&
                                tg_gore_reads_as_median(nl, si, br_lanes))
                                gore_page = tg_road_page(si);
                            /* [R19 FORK THROAT] A sliver gore at a fork mouth /
                             * rejoin (the pinched throat, one span past the
                             * split) is a tiny GREEN patch hemmed by tarmac and
                             * reads as a void where the road should be whole.
                             * Pave it road so the pinch looks like the road
                             * briefly splitting -- the R17 case R17_GORE_ROAD's
                             * median-width test misses on a non-avenue fork.
                             * TD5RE_R19_GORE_THROAT_ROAD=0 = A/B. */
                            if (td5_env_flag_on("TD5RE_R19_GORE_THROAT_ROAD") &&
                                tg_gore_throat_sliver(nl, si, br_lanes))
                                gore_page = tg_road_page(si);
                            if (tg_span_in_tunnel(si) &&
                                td5_env_flag_on("TD5RE_R8_BORE_MEDIAN"))
                                gore_page = TD5_TG_PAGE_R8_BRIDGE + 0;
                            if (!tg_emit_gore(nl, si, sh0, sh1, gw0, gw1,
                                              gore_page, &meshes, tg_fork_side(fi)))
                                ok = 0;
                            if (tg_span_in_tunnel(si))
                                tg_acct(TG_ACCT_R8_BRIDGE, si);
                        }
                        /* Items 9c/10: where the fork is TIGHT (an avenue) the
                         * gore is a slim central median, not a wide split -- give
                         * it a raised divider so the two carriageways read as one
                         * divided avenue. Treatment varies per fork. Emitted on
                         * the MAIN fork span alongside the gore it sits on. */
                        /* [R11 CROSS item 8] ... and on EVERY fork, not just
                         * the ones classified as an avenue. The island is the
                         * only thing that ever stands proud of the median
                         * floor, so "avenues only" meant every other fork's
                         * median was flush with the road. The emitter decides
                         * per span whether the gore is median-sized; a wide
                         * split still emits nothing new (its widths fail
                         * tg_emit_avenue_divider's own fill test). */
                        /* [MEDIAN/BRIDGE 2026-09-07] ... and never as a stub on
                         * a bridge. A fork that only overlaps the head of a
                         * bridge run used to start a median on the deck and drop
                         * it mid-crossing; it is now all spans of the run or
                         * none (tg_median_bridge_uniform). Off a bridge this is
                         * always 1, so nothing else changes. */
                        if (ok && (tg_fork_is_avenue(fi)
                                   || tg_r11_median_rise()) &&
                            tg_median_bridge_uniform(nl, si, br_lanes) &&
                            td5_env_flag_on("TD5RE_AUTOTRACK_AVENUE_DIVIDER"))
                            if (!tg_emit_avenue_divider(nl, si, fi, sh0, sh1,
                                                        gw0, gw1, br_lanes,
                                                        &meshes, moff, &nmesh))
                                ok = 0;
                    }
                } else if (!TG_SUB(TG_SUB_ROAD, tg_emit_road_mesh(nl, si, nl->v[si].lanes, &meshes))) {
                    ok = 0;
                }
            }
            /* [R7 GUARD] road quads, the fork gore and the avenue divider all
             * occupy the drivable envelope on purpose. */
            tg_guard_mark(road_ex0, meshes.len, TG_GK_ROAD, si);
            }
            /* Guardrails belong in THIS loop, not the box pass below: that pass
             * recovers each piece's offset by dividing the appended bytes by
             * n_added, which only holds while every piece is a same-sized box.
             * A rail prism has a different vertex count and would silently
             * corrupt those offsets. Here each offset is recorded explicitly. */
            if (ok && rails &&
                tg_span_needs_guardrail(nl, si, nspans)) {
                const size_t r0 = meshes.len;
                int emitted = 0;
                /* [R9 RAILFIX] The offset is only COMMITTED (nmesh++) if a mesh
                 * was actually written: the emitter can now refuse both sides
                 * (deck / kerb-fence ownership), and a moff entry pointing at
                 * zero bytes is read as garbage geometry, not as a missing
                 * rail. */
                moff[nmesh] = meshes.len;
                if (!TG_SUB(TG_SUB_RAIL, tg_emit_guardrail(nl, si, &meshes, &emitted))) ok = 0;
                else if (emitted) { nmesh++; nrails++; }
                tg_guard_mark(r0, meshes.len, TG_GK_RAIL, si);
            }
        }
        TG_ZONE_END(TG_ZONE_ENTRY_L1);
        TG_ZONE_BEGIN(TG_ZONE_ENTRY_L2);
        for (i = 0; i < ns && ok; i++) {
            const int si = s0 + i;
            int k;

            if (si >= ring) continue;   /* pad + corridor carry road only */
            /* Headroom for the widest single span (see the budget note above).
             * Hitting this is not an error, but it drops the rest of the entry's
             * scenery with no other symptom, so COUNT it -- a silent hole in the
             * world is the hardest kind of bug to chase from a screenshot. */
            if (nmesh + 96 > TG_MAX_MESHES_PER_ENTRY) { nbudget++; break; }

            {   /* [FB] scenery hooks: one call per work area. `hook` is rebuilt
                 * per span so *nmesh always tracks the live counter. */
                TG_FBHook hook;
                hook.nl = nl; hook.si = si; hook.nspans = nspans;
                hook.lanes = nl->v[si].lanes;   /* [LANES] per span */
                hook.b = &k_biomes[tg_biome_for_span(si)];
                hook.blk = &meshes; hook.moff = moff; hook.nmesh = &nmesh;
                hook.maxmesh = TG_MAX_MESHES_PER_ENTRY;
                if (tg_span_in_tunnel(si)) {
                    /* [R7 GUARD] the tunnel-portal mountain massing sits above and
                     * beside the bore on purpose. */
                    size_t t0 = meshes.len;
                    if (!TG_SUB(TG_SUB_TUNNEL, tg_emit_fb_tunnel(&hook))) { ok = 0; break; }
                    tg_guard_mark(t0, meshes.len, TG_GK_TUNNEL, si);
                } else {
                    /* [R8 GUARD] Kind marks on the NON-exempt hooks too. These
                     * license nothing (every kind below is SCENERY class); they
                     * exist so a rejection can name the emitter that produced
                     * the mesh, which is what makes "rejections broken down by
                     * kind, and zero of them road/deck/gantry/tunnel" a number
                     * in the log rather than a claim. */
                    size_t g0 = meshes.len;
                    if (!TG_SUB(TG_SUB_CITY, tg_emit_fb_city(&hook)))    { ok = 0; break; }
                    tg_guard_mark(g0, meshes.len, TG_GK_CITY, si);
                    g0 = meshes.len;
                    if (!TG_SUB(TG_SUB_BLOCK, tg_emit_fb_block(&hook)))   { ok = 0; break; }
                    tg_guard_mark(g0, meshes.len, TG_GK_BLOCK, si);
                    g0 = meshes.len;
                    if (!TG_SUB(TG_SUB_CROSS, tg_emit_fb_cross(&hook)))   { ok = 0; break; }
                    if (!TG_SUB(TG_SUB_CROSS, tg_net_emit_entry(&hook)))  { ok = 0; break; }  /* [TOPOLOGY-FIRST] */
                    tg_guard_mark(g0, meshes.len, TG_GK_CROSS, si);
                    g0 = meshes.len;
                    if (!TG_SUB(TG_SUB_FLORA, tg_emit_fb_flora(&hook)))   { ok = 0; break; }
                    tg_guard_mark(g0, meshes.len, TG_GK_FLORA, si);
                    /* [R12 CROSS item 5] The forest side road runs AFTER the
                     * tree-line band, whose cut it fills. Its own dispatcher
                     * marks the carriageway and the fold walls separately (see
                     * tg_emit_fb_forest_cross); the enclosing mark here is only
                     * the fail-safe, and tg_guard_kind_of prefers the narrower. */
                    g0 = meshes.len;
                    if (!TG_SUB(TG_SUB_FCROSS, tg_emit_fb_forest_cross(&hook))) { ok = 0; break; }
                    tg_guard_mark(g0, meshes.len, TG_GK_FLORA, si);
                    g0 = meshes.len;
                    if (!TG_SUB(TG_SUB_PARKTREE, tg_emit_fb_park_trees(&hook))) { ok = 0; break; }
                    tg_guard_mark(g0, meshes.len, TG_GK_PARKTREE, si);
                    g0 = meshes.len;
                    /* [R9 TOPO item 6] trees ON the slope, not on its lip.
                     * Marked FLORA so the on-road guard validates it exactly as
                     * it validates every other billboard -- a new emitter must
                     * inherit R7's authority, not be exempted from it. */
                    if (!TG_SUB(TG_SUB_SLOPEFLORA, tg_emit_fb_slope_flora(&hook))) { ok = 0; break; }
                    tg_guard_mark(g0, meshes.len, TG_GK_FLORA, si);
                    g0 = meshes.len;
                    /* Pre-built by tg_side_terrain_build unless the knob is
                     * off or that build bailed. Either way the bytes, the moff
                     * entries and the marks land exactly where the inline emit
                     * would have put them. Both paths are charged to the SAME
                     * terrain bucket so the profile stays comparable. */
                    if (s_side_use) {
                        if (!TG_SUB(TG_SUB_TERRAIN,
                                    tg_side_terrain_splice(si, &meshes, moff, &nmesh,
                                                           TG_MAX_MESHES_PER_ENTRY))) {
                            ok = 0; break;
                        }
                    } else if (!TG_SUB(TG_SUB_TERRAIN, tg_emit_fb_terrain(&hook))) {
                        ok = 0; break;
                    }
                    tg_guard_mark(g0, meshes.len, TG_GK_TERRAIN, si);
                    /* [R9 INFRA] street furniture. Marked TG_GK_PROP, which is
                     * SCENERY class -- deliberately NOT exempt from the on-road
                     * guard, so a bin standing in the carriageway is dropped
                     * and counted rather than licensed. */
                    g0 = meshes.len;
                    if (!TG_SUB(TG_SUB_INFRA, tg_emit_fb_infra(&hook)))   { ok = 0; break; }
                    tg_guard_mark(g0, meshes.len, TG_GK_PROP, si);
                }
                /* [R7 GUARD] the start/finish gantry legs stand at the road edge
                 * and its beam spans overhead -- authored across the road. */
                {
                    size_t k0 = meshes.len;
                    if (!TG_SUB(TG_SUB_TRACK, tg_emit_fb_track(&hook))) { ok = 0; break; }
                    tg_guard_mark(k0, meshes.len, TG_GK_GANTRY, si);
                }
            }

            /* [R3 item 19] Backstop walls at the two OPEN ends of the strip,
             * aligned with the collision boundary sentinels (fwd=2, rev=ring-3).
             * Emitted here so a tunnel span at either end still gets its cap. */
            if (si == 2 || si == ring - 3) {
                size_t ew0 = meshes.len;
                if (!tg_emit_end_wall(nl, si, si != 2, moff, &nmesh,
                                      TG_MAX_MESHES_PER_ENTRY, &meshes)) {
                    ok = 0; break;
                }
                /* [R7 GUARD] the strip's two end backstop walls span the road on
                 * purpose (the collision sentinels behind the start/finish). */
                tg_guard_mark(ew0, meshes.len, TG_GK_ENDWALL, si);
            }

            if (tg_span_in_tunnel(si)) {
                /* Enclosed: no buildings, they would stand inside the walls.
                 * Tunnel pieces are equal-sized boxes, so recover each from the
                 * appended span. */
                size_t before = meshes.len;
                int n_added = 0;
                if (!TG_SUB(TG_SUB_TUNNEL, tg_emit_tunnel(nl, si, &meshes, &n_added))) { ok = 0; break; }
                for (k = 0; k < n_added; k++)
                    moff[nmesh++] = before + (size_t)k *
                                    ((meshes.len - before) / (size_t)n_added);
                /* [R7 GUARD] the tunnel bore encloses the road. */
                tg_guard_mark(before, meshes.len, TG_GK_TUNNEL, si);
            } else {
                const TG_Biome *b = &k_biomes[tg_biome_for_span(si)];
                size_t b0, b1;
                int nb = 0;
                /* [R9 item 13] CITY UNDERPASS. Emitted in the NON-tunnel arm on
                 * purpose: an underpass does not suppress the town, the town is
                 * what the highway is built through, so the buildings, flora and
                 * terrain hooks all still run for this span and should.
                 *
                 * MUST be guard-marked EXEMPT. A deck over the carriageway is
                 * the precise silhouette TD5RE_R7_GUARD was built to reject;
                 * left unmarked the guard eats it and item 13 ships invisible.
                 * TG_GK_DECK is the existing exempt kind for "an authored road
                 * surface above this one", which is exactly what this is. */
                if (si == tg_underpass_span(si)) {
                    size_t up0 = meshes.len;
                    int nm0 = nmesh;
                    if (!tg_emit_underpass(nl, si, &meshes, moff, &nmesh,
                                           TG_MAX_MESHES_PER_ENTRY)) {
                        ok = 0; break;
                    }
                    /* [R12 item 11a] WHY THE BACKSTOP NEVER CAUGHT THE PILLAR.
                     * One tg_guard_mark covered the WHOLE element -- piers, deck
                     * and parapets -- as TG_GK_DECK, and TG_GK_DECK is
                     * TG_GKC_EXEMPT. That licence is correct and necessary for
                     * the deck (an authored road surface ABOVE this one is the
                     * precise silhouette the guard exists to reject), but the
                     * piers are ordinary ground structures standing beside the
                     * carriageway, and blanket-exempting them made "a pillar on
                     * the road" unreportable by construction: the guard was not
                     * failing to judge it, it was told not to.
                     *
                     * The first TWO meshes this emitter records are the two
                     * piers (see its abutment loop), so the exemption is split
                     * at that boundary: piers are marked TG_GK_BLOCK, which is
                     * TG_GKC_SCENERY -- validated like any other roadside mass
                     * -- and only the deck and parapets keep the deck licence.
                     * Future overpass geometry lands in the deck half by
                     * default, so this is deliberately the conservative split.
                     *
                     * With the geometric fix above the guard has nothing to
                     * reject; verified 0 pier rejections on all three seeds.
                     * TD5RE_R12_UP_PIERGUARD=0 restores the blanket exemption. */
                    if (td5_env_flag_on("TD5RE_R12_UP_PIERGUARD") &&
                        nmesh >= nm0 + 3) {
                        tg_guard_mark(up0, moff[nm0 + 2], TG_GK_BLOCK, si);
                        tg_guard_mark(moff[nm0 + 2], meshes.len, TG_GK_DECK, si);
                    } else {
                        tg_guard_mark(up0, meshes.len, TG_GK_DECK, si);
                    }
                }
                /* Building: 0 or 1 mesh -- record its offset explicitly.
                 *
                 * [R10] b0 IS TAKEN HERE, NOT AT THE TOP OF THE ARM. It used to
                 * be captured before the underpass block above, which made it
                 * stale on every underpass span: the underpass appends its own
                 * meshes AND records their (ascending) offsets, so pushing the
                 * older b0 afterwards left moff[] DESCENDING at that point --
                 * and pointing at the underpass's first mesh rather than at the
                 * building. moff[] must stay ascending: tg_guard_validate_entry
                 * compacts the buffer with a single forward write cursor and a
                 * backwards offset makes that cursor run off the end of the
                 * allocation. See the R10 note on that function. */
                b0 = meshes.len;
                if (!tg_building_for_span(nl, si, &meshes)) { ok = 0; break; }
                /* [S2] The verge tree, split out of the call above. Wall and
                 * tree are mutually exclusive per span, so at most one of the
                 * two emits and the single moff entry below still describes
                 * exactly one mesh -- the offsets stay ascending, which
                 * tg_guard_validate_entry's forward compaction cursor
                 * requires. */
                if (!tg_building_verge_tree(nl, si, &meshes)) { ok = 0; break; }
                if (meshes.len > b0) moff[nmesh++] = b0;
                tg_guard_mark(b0, meshes.len, TG_GK_BUILDING, si);
                /* Bridge: 0..N equal-sized boxes among themselves.
                 * [R7 GUARD] the deck IS the road and the piers descend from it;
                 * the parapets/ribs sit on the deck edge or overhead. */
                b1 = meshes.len;
                if (!TG_SUB(TG_SUB_BRIDGE, tg_emit_bridge(nl, si, &meshes, &nb))) { ok = 0; break; }
                for (k = 0; k < nb; k++)
                    moff[nmesh++] = b1 + (size_t)k *
                                    ((meshes.len - b1) / (size_t)nb);
                tg_guard_mark(b1, meshes.len, TG_GK_DECK, si);
                /* Sloped parapets + optional overhead ribs (items 12, 13):
                 * separate quad meshes, each records its own offset. */
                {
                    size_t br0 = meshes.len;
                    if (!tg_emit_bridge_rails(nl, si, &meshes, moff, &nmesh)) {
                        ok = 0; break;
                    }
                    tg_guard_mark(br0, meshes.len, TG_GK_DECK, si);
                }
                /* River under a bridge run. [R7 GUARD] the water is the surface a
                 * bridge deck flies over -- never scenery on the road. */
                if (tg_span_in_bridge_run(si)) {
                    size_t bw0 = meshes.len;
                    if (!tg_emit_bridge_water(nl, si, &meshes, moff, &nmesh)) {
                        ok = 0; break;
                    }
                    tg_guard_mark(bw0, meshes.len, TG_GK_WATER, si);
                }
                /* [TOPOLOGY-FIRST] no coastline band: the banks are the
                 * world's own ground (tg_ground_side samples it). */
                /* Prop billboards: variable count/size, each records its own. */
                {
                    size_t p0 = meshes.len;
                    if (!TG_SUB(TG_SUB_PROPS, tg_emit_props(nl, si, b, &meshes, moff, &nmesh,
                                       TG_MAX_MESHES_PER_ENTRY))) { ok = 0; break; }
                    tg_guard_mark(p0, meshes.len, TG_GK_PROP, si);
                }
                /* Sea plane on coastal runs. [R6 item 14] NOT on a bridge run:
                 * the full-width bridge water already covers the surface there
                 * (now at sea level), and laying the one-sided sea plane over it
                 * too gave two overlapping surfaces -- the z-fighting seam and
                 * the level step of "the water is not continuous". */
                /* [TOPOLOGY-FIRST] wherever the shore table found water on a
                 * side (sea, lake, river beside the road) -- both sides. */
                if (!tg_span_in_bridge_run(si) && tg_road_wet_any(si)) {
                    size_t sw0 = meshes.len;
                    if (!tg_emit_water(nl, si,  1.0, &meshes, moff, &nmesh)) { ok = 0; break; }
                    if (!tg_emit_water(nl, si, -1.0, &meshes, moff, &nmesh)) { ok = 0; break; }
                    /* [R7 GUARD] the sea plane sits beside/below the road. */
                    tg_guard_mark(sw0, meshes.len, TG_GK_WATER, si);
                }
                /* [R11 SIGNS item 16] Direction signage, LAST in the span's
                 * sequence ON PURPOSE: it consults the element inventory for si
                 * (TG_ACCT_LAMP) to keep a sign off a span whose lamp head
                 * already occupies panel height, and that bit is only set once
                 * every earlier emitter for si has run. Marked TG_GK_PROP --
                 * FURNITURE class, NOT exempt -- so a sign in the carriageway is
                 * dropped and counted like any other piece of street furniture.
                 */
                {
                    size_t sg0 = meshes.len;
                    if (!TG_SUB(TG_SUB_SIGN, tg_emit_r11_sign(nl, si, nspans, &meshes, moff, &nmesh,
                                          TG_MAX_MESHES_PER_ENTRY))) {
                        ok = 0; break;
                    }
                    tg_guard_mark(sg0, meshes.len, TG_GK_PROP, si);
                }
            }
        }

        /* [R7 GUARD] Post-emit backstop: reject any scenery mesh standing in the
         * carriageway, over ALL of this entry's assembled geometry, whatever
         * emitter produced it. Runs before the block header so the rejected
         * meshes never reach the offset table. */
        TG_ZONE_END(TG_ZONE_ENTRY_L2);
        s_tg_zone_us[TG_ZONE_EMIT] += td5_plat_time_us() - s_tg_emit_t0;
        s_tg_zone_n[TG_ZONE_EMIT]++;
        /* [R9 CITY] Measure BEFORE the guard compacts the buffer -- the pavement
         * marks are byte offsets into it. Pavement is never a guard reject
         * (rejects-by-kind reports zero sidewalk/city kinds), so measuring here
         * and shipping after describe the same geometry. */
        if (ok) {
            TG_ZONE_BEGIN(TG_ZONE_CITYSCAN);
            tg_r9_city_scan_entry(nl, ring, s0, ns, &meshes, moff, nmesh);
            TG_ZONE_END(TG_ZONE_CITYSCAN);
        }
        if (ok) {
            TG_ZONE_BEGIN(TG_ZONE_GUARDVAL);
            tg_guard_validate_entry(nl, ring, s0, ns, &meshes, moff, &nmesh);
            TG_ZONE_END(TG_ZONE_GUARDVAL);
        }

        if (ok) {
            TG_ZONE_BEGIN(TG_ZONE_ASSEMBLE);
            const unsigned int hdr = (unsigned)(4 + nmesh * 4);
            tg_put_u32(&blocks[e], (unsigned)nmesh);
            for (i = 0; i < nmesh; i++) {
                tg_put_u32(&blocks[e], hdr + (unsigned)moff[i]);
                /* [PICK] entries the guard never validated (guard off, or a
                 * branch-corridor entry s0>=ring) still need a kind; here moff[]
                 * is the uncompacted offset the guard marks are keyed by. No-op
                 * when validate already tagged the slot. */
                tg_meshtag_fallback(e, i, moff[i]);
            }
            if (!tg_buf_need(&blocks[e], meshes.len)) ok = 0;
            else {
                memcpy(blocks[e].b + blocks[e].len, meshes.b, meshes.len);
                blocks[e].len += meshes.len;
            }
            TG_ZONE_END(TG_ZONE_ASSEMBLE);
        }
        tg_buf_free(&meshes);
    /* carry the cross-entry tallies back */
    s_scn.nrails = nrails; s_scn.nbudget = nbudget;
    if (!ok) s_scn.ok = 0;
    return ok;
}

static int tg_scenery_end(TG_Buf *out)
{
    const TG_NodeList *nl = s_scn.nl;
    const int nspans = s_scn.nspans, ring = s_scn.ring;
    const int nentries = s_scn.nentries;
    TG_Buf *blocks = s_scn.blocks;
    const int rails = s_scn.rails;
    int nrails = s_scn.nrails, nbudget = s_scn.nbudget;
    int ok = s_scn.ok, e;
    unsigned int cursor;
    unsigned long long rt_ = 0;
    (void)nl; (void)nspans; (void)ring; (void)rt_;

    if (ok) {
        /* Header: count, then (offset,size) pairs. Block 0 must begin exactly
         * at 4 + count*8 -- the strict-format-A autodetect requires it. */
        cursor = (unsigned)(4 + nentries * 8);
        tg_put_u32(out, (unsigned)nentries);
        for (e = 0; e < nentries; e++) {
            tg_put_u32(out, cursor);
            tg_put_u32(out, (unsigned)blocks[e].len);
            cursor += (unsigned)blocks[e].len;
        }
        for (e = 0; e < nentries; e++) {
            if (!tg_buf_need(out, blocks[e].len)) { ok = 0; break; }
            memcpy(out->b + out->len, blocks[e].b, blocks[e].len);
            out->len += blocks[e].len;
        }
        if (ok) {
            TD5_LOG_I(LOG_TAG, "trackgen: models = %d entries, %zu bytes "
                      "(%d road meshes)", nentries, out->len, nspans);
            /* Report coverage rather than assuming the gate is sane: a rail
             * count of 0 or of nspans both mean the curvature threshold is
             * wrong, and that is invisible without a number. */
            if (rails)
                TD5_LOG_I(LOG_TAG, "trackgen: guardrails on %d/%d spans (%d%%)",
                          nrails, nspans,
                          nspans ? (nrails * 100 / nspans) : 0);
            /* [R22 CORRIDOR item 4] how much scenery reached the fork corridors,
             * the number the density sweep is checked against. Kinds is `tree`
             * only for now (ground is already carried by the main road's far
             * band; see tg_emit_corridor_flora). Always printed when branches
             * ran, so a zero (biome with no trees, or the knob off) is legible
             * rather than a silent absence. */
            if (s_scn.branch_active)
                TD5_LOG_I(LOG_TAG, "trackgen: [R22 CORRIDOR] %ld tree(s) on %ld "
                          "corridor span(s) (kinds: tree)",
                          s_r22_corr_trees, s_r22_corr_spans);
            /* WARN, not INFO: any non-zero count means some spans are missing
             * scenery they were meant to have, and nothing else would say so. */
            if (nbudget)
                TD5_LOG_W(LOG_TAG, "trackgen: mesh budget exhausted in %d/%d "
                          "entries -- those spans lost scenery (raise "
                          "TG_MAX_MESHES_PER_ENTRY)", nbudget, nentries);
            /* [R7 GUARD] class-level evidence: a non-zero count is a real number
             * of scenery meshes that WOULD have stood in the road, dropped (or,
             * in report-only mode, logged) by the post-emit backstop. "clean"
             * means the whole strip validated against the carriageway. */
            if (!tg_guard_enabled())
                TD5_LOG_I(LOG_TAG, "on-road guard: DISABLED (TD5RE_R7_GUARD=0)");
            else if (s_guard_rejects)
                TD5_LOG_W(LOG_TAG, "on-road guard: %ld scenery mesh(es) %s the "
                          "carriageway across %d spans%s; %ld remaining after "
                          "the pass", s_guard_rejects,
                          tg_guard_report_only() ? "would overlap"
                                                 : "rejected for overlapping",
                          nspans, tg_guard_report_only() ? " (REPORT-ONLY)" : "",
                          s_guard_residual);
            else
                TD5_LOG_I(LOG_TAG, "on-road guard: clean -- 0 scenery meshes in "
                          "the road over %d spans (%ld remaining)", nspans,
                          s_guard_residual);
            if (s_guard_residual && !tg_guard_report_only())
                TD5_LOG_W(LOG_TAG, "on-road guard: RESIDUAL %ld non-exempt "
                          "on-road mesh(es) survived -- parse/compaction bug",
                          s_guard_residual);
            /* [R11 SIGNS item 16] The round's acceptance number. A count of ZERO
             * is the failure mode this exists to make loud: a new roadside
             * emitter that is gated wrong, or judged by the on-road guard,
             * produces NOTHING and every other number in this log looks
             * healthy. The per-arrow split is here because a pool that always
             * resolves to the same page is that same failure one level down
             * (R7 item 4), and the skip counters say WHICH gate refused. */
            if (!tg_r11_signs_enabled())
                TD5_LOG_I(LOG_TAG, "R11 signs: DISABLED (TD5RE_R11_SIGNS=0)");
            else if (!s_r11_signs)
                TD5_LOG_W(LOG_TAG, "R11 signs: NONE emitted over %d spans -- "
                          "the bend map or a placement gate is refusing every "
                          "candidate (skips: lamp=%ld street=%ld side=%ld)",
                          nspans, s_r11_sign_skip_lamp,
                          s_r11_sign_skip_street, s_r11_sign_skip_side);
            else
                TD5_LOG_I(LOG_TAG, "R11 signs: %ld panel(s) over %d spans "
                          "(left=%ld right=%ld) | skips: lamp=%ld street=%ld "
                          "side=%ld", s_r11_signs, nspans, s_r11_sign_left,
                          s_r11_sign_right, s_r11_sign_skip_lamp,
                          s_r11_sign_skip_street, s_r11_sign_skip_side);
            /* [R9 CITY] pavement uniqueness + mouth massing, over the whole
             * assembled strip. These are the round's acceptance numbers. */
            tg_r9_city_report(nl, nspans);
            /* [R14 BRANCH item 2a] pavement OVERLAPS + fork-region HOLES, the
             * complementary measurement to R9's separated-band check. */
            tg_r14_branch_report(nl, nspans);
            /* [R13 FACES item 6] run-end returns, counted at the emit site. */
            tg_r13_faces_report(nspans);
            /* [R8] The acceptance evidence for the precision pass, as numbers.
             * ONE line per kind that was rejected, and an explicit line for the
             * kinds that must NEVER appear: if road / deck / gantry / tunnel /
             * skirt / water / branch-road ever show a non-zero count, the guard
             * has started eating the track and that is a stop-the-line failure,
             * not a tuning question. */
            if (tg_guard_enabled()) {
                /* The kinds that must NEVER be rejected: the drivable surface
                 * and the structures that enclose it. NOT skirt/water/coast --
                 * those are UNDER class, and rejecting one that has climbed ON
                 * TOP of the carriageway is the R8 item-7 fix, not a failure. */
                static const int k_never[] = {
                    TG_GK_ROAD, TG_GK_BRANCHROAD, TG_GK_DECK, TG_GK_GANTRY,
                    TG_GK_TUNNEL, TG_GK_ENDWALL
                };
                long never = 0;
                int ki;
                for (ki = 0; ki < TG_GK_COUNT; ki++)
                    if (s_guard_rej_kind[ki])
                        TD5_LOG_I(LOG_TAG, "on-road guard: rejects by kind: "
                                  "%-12s %ld", k_guard_kind_name[ki],
                                  s_guard_rej_kind[ki]);
                for (ki = 0; ki < (int)(sizeof k_never / sizeof k_never[0]); ki++)
                    never += s_guard_rej_kind[k_never[ki]];
                if (never)
                    TD5_LOG_W(LOG_TAG, "on-road guard: %ld TRACK-SURFACE mesh(es) "
                              "rejected (road/branch-road/deck/gantry/tunnel/"
                              "end-wall) -- the guard is eating the track", never);
                else
                    TD5_LOG_I(LOG_TAG, "on-road guard: 0 road/branch-road/deck/"
                              "gantry/tunnel/end-wall meshes rejected");
                if (s_guard_ex_scope_hits)
                    TD5_LOG_I(LOG_TAG, "on-road guard: %ld exempt mesh(es) fell "
                              "outside their own span run and were validated as "
                              "scenery", s_guard_ex_scope_hits);
                /* [R10] Compaction health. Both zero is the invariant; a
                 * non-zero `unsorted` names an emitter that records its mesh
                 * offsets out of order, which is what made the compaction
                 * cursor overrun the mesh buffer and kill generation inside
                 * free() with no error line. */
                if (s_guard_unsorted || s_guard_unsafe)
                    TD5_LOG_W(LOG_TAG, "on-road guard: compaction UNSORTED=%ld "
                              "UNSAFE=%ld -- an emitter is recording mesh "
                              "offsets out of ascending order", s_guard_unsorted,
                              s_guard_unsafe);
                else
                    TD5_LOG_I(LOG_TAG, "on-road guard: compaction clean -- mesh "
                              "offsets ascending, 0 unsafe moves");
            }
        }
    }

    /* [PARALLEL GROUNDWORK] A non-zero risk count means the pre-pass and the
     * inline emit could have made different budget decisions for some span, so
     * the side buffer is no longer a faithful substitute there. WARN, because
     * the symptom would be missing scenery and nothing else would say so. */
    if (s_side_use && s_side_budget_risk)
        TD5_LOG_W(LOG_TAG, "trackgen: terrain side-buffer skipped %ld span(s) "
                  "on mesh budget -- those spans lost terrain (raise "
                  "TG_MAX_MESHES_PER_ENTRY or set TD5RE_AUTOTRACK_SIDEBUF=0)",
                  s_side_budget_risk);
    else if (s_side_use)
        TD5_LOG_I(LOG_TAG, "trackgen: terrain side-buffer used for %d spans, "
                  "0 budget skips", s_side_terr_n);
    tg_side_terrain_free();

    for (e = 0; e < nentries; e++) tg_buf_free(&blocks[e]);
    free(blocks);
    /* Clearing these is not bookkeeping: blocks[] has just been freed, so
     * leaving s_scn.blocks set hands the next caller a dangling pointer. (The
     * split originally kept an early `return` here, which made both these
     * stores unreachable -- no reader today, but the streaming driver polls
     * s_scn.active.) */
    s_scn.active = 0;
    s_scn.blocks = NULL;
    return ok && !out->oom;
}

/* [STREAMED SCENERY] Equivalence check for the streaming ingest path, run on
 * the blocks this build just assembled (dev knob, off by default).
 *
 * The point is to exercise td5_track_scenery_reserve/publish_entry against
 * REAL generated blocks and print totals that must match the parse path's own
 * "runtime display lists: N blocks, M mesh slots, K mesh records" line for the
 * same seed. Publishing here is throwaway: the level load that follows parses
 * MODELS.DAT and calls free_models_dat_runtime, which drops this table.
 *
 * (stream_done rebuilds the span mapping against whatever span array is
 * current, which during a build is the PREVIOUS level's. Harmless -- the real
 * load rebuilds it again -- but it is why this is a knob and not always on.) */
static void tg_scenery_stream_selfcheck(int nspans)
{
    int e, geom = 0;

    if (!td5_track_scenery_reserve(s_scn.nentries,
                                   (size_t)nspans * TG_STREAM_BYTES_PER_SPAN)) {
        TD5_LOG_W(LOG_TAG, "stream selfcheck: reserve failed for %d entries",
                  s_scn.nentries);
        return;
    }
    for (e = 0; e < s_scn.nentries; e++)
        geom += td5_track_scenery_publish_entry(e, s_scn.blocks[e].b,
                                                s_scn.blocks[e].len);
    TD5_LOG_I(LOG_TAG, "stream selfcheck: %d/%d entries carried geometry, "
              "%d settled", geom, s_scn.nentries,
              td5_track_scenery_ready_entries());
    td5_track_scenery_stream_done();
}

/* The original whole-build entry point, now a driver over the three phases. */
int tg_emit_models(const TG_NodeList *nl, int nspans, int lanes,
                          TG_Buf *out)
{
    int e;
    if (!tg_scenery_begin(nl, nspans, lanes)) return 0;
    for (e = 0; e < s_scn.nentries && s_scn.ok; e++)
        tg_scenery_entry(e);
    /* Before tg_scenery_end, which frees blocks[]. */
    if (s_scn.ok && td5_env_flag_off("TD5RE_AUTOTRACK_STREAM_SELFCHECK"))
        tg_scenery_stream_selfcheck(nspans);
    return tg_scenery_end(out);
}

/* ===================== [S2] REGENERATE FOR IN-PLACE REWRITE =====================
 * Rebuild the main-road span records for `seed` and hand them back as a blob of
 * 24-byte records. Phase 2 streaming needs this so the track module can
 * overwrite a REGION of its live span array with bytes that provably match what
 * the same seed produced originally
 * (docs/plans/AUTOTRACK_STREAMING.md, stage S2).
 *
 * Ownership is deliberate: the generator produces bytes, the track module owns
 * its arrays and does the writing. This function never touches live state.
 *
 * Deterministic by construction -- tg_srand(seed) resets the private xorshift,
 * and nothing here draws from the game's rand(), so a later call with the same
 * seed reproduces the same bytes regardless of what happened in between.
 *
 * Caller frees *out_bytes. Returns 1 on success.
 */
/* =============================== SECTION: route preview (PORT-ONLY) =========
 * Mesh-free route walk for the AUTO TRACK STUDIO screen. See the contract on
 * td5_trackgen_preview_route in td5_trackgen.h -- in particular that this is
 * NOT safe to run concurrently with a real build, because both walk s_rng and
 * the biome grid.
 *
 * Deliberately built from the SAME functions td5_trackgen_build_level uses, in
 * the same order, stopping at the point where the build starts emitting bytes.
 * Any other arrangement would let the preview and the real track disagree,
 * which is the one failure this feature cannot have. */
static void tg_preview_emit_forks(const TG_NodeList *nl,
                                  const TD5_TrackGenPreviewSink *sink)
{
    int i;
    if (!sink || !sink->on_points) return;

    /* Corridors are not separate nodes: tg_emit_strip lays each one over the
     * main-ring nodes F..R, pushed sideways by tg_branch_shift_s. Rebuilding
     * them the same way keeps the preview and the strip in agreement. */
    for (i = 0; i < s_fork_count; i++) {
        const int F = s_forks[i].F;
        const int R = s_forks[i].R;
        const int L = s_forks[i].len;  (void)L;   /* [FORK KINDS] geometry now comes from the fork helpers */
        int si;
        if (F < 0 || R >= nl->count) continue;
        for (si = F; si <= R; si++) {
            const TG_Node *n = &nl->v[si];
            const double lx = n->tz, lz = -n->tx;
            const double sh = tg_fork_br_shift(i, si - F, n->width);
            TD5_TrackGenPoint p;
            p.x = (float)(n->x + lx * sh);
            p.z = (float)(n->z + lz * sh);
            p.lanes = n->lanes;
            p.branch = i + 1;
            /* [R22 item 1 fix] A corridor point has no ring node of its own --
             * it is the main node displaced by the fork's lateral bow -- so it
             * is APPENDED after the ring rather than indexed into it. These are
             * published once each, after the walk, so no dedup applies. */
            p.node = -1;
            sink->on_points(&p, 1, sink->ctx);
        }
    }
}

int td5_trackgen_preview_route(const TD5_TrackGenSpec *spec,
                               const TD5_TrackGenPreviewSink *sink,
                               TD5_TrackGenPreviewStats *out_stats)
{
    TD5_TrackGenSpec eff;
    TG_NodeList nl;
    TG_Buf strip;
    int tally[TD5_TG_SECTION_COUNT];
    int nspans = 0, ok = 0;

    if (!spec) return 0;

    memset(&nl, 0, sizeof(nl));
    memset(&strip, 0, sizeof(strip));
    memset(tally, 0, sizeof(tally));
    if (out_stats) memset(out_stats, 0, sizeof(*out_stats));

    /* [R21 ROLLS] The preview has to walk the SAME road the race will build, so
     * it resolves and folds the rolls exactly as td5_trackgen_regenerate does.
     * Miss this and the studio draws the shipped defaults while the race drives
     * the rolls, with nothing but a hand comparison to catch it.
     *
     * `spec` is const (the studio owns that struct), so fold into a local copy
     * and re-aim the pointer -- every spec-> read below then sees the folded
     * values with no further edits. Latching into the shared roll table is safe
     * here for exactly the reason tg_srand and the biome grid already are: a
     * preview and a build must never overlap, which the joins in
     * td5_asset_load_level and td5_tgstream_cancel_join enforce. */
    eff = *spec;
    tg_rolls_resolve(eff.seed);
    tg_rolls_apply_spec(&eff);
    spec = &eff;

    /* Same preamble as build_level, minus the _mkdir. */
    s_gen_seed = spec->seed;
    tg_acct_reset();
    tg_biome_layout(spec->seed, spec->target_spans);
    tg_srand(spec->seed);

    s_preview_cancelled = 0;
    s_preview_sink = sink;

    if (tg_build_centerline(spec, &nl, tally)) {
        tg_apply_elevation(spec, &nl);
        /* Points are already published by the push hook; the strip emit is
         * what makes s_forks[] and s_ring_len real, so it runs even though the
         * buffer is thrown away. */
        if (tg_emit_strip(&nl, &strip, &nspans) && nspans >= 8) {
            tg_preview_emit_forks(&nl, sink);
            ok = 1;
        }
    }

    s_preview_sink = NULL;

    if (out_stats) {
        int s;
        out_stats->seed       = spec->seed;
        out_stats->node_count = nl.count;
        out_stats->span_count = nspans;
        out_stats->ring_len   = s_ring_len;
        /* [R22 item 1] Published, not derived. tg_finish_span is only correct
         * AFTER tg_emit_strip has filled s_forks[] and the structure table,
         * which is exactly here: it consults tg_span_in_fork_clear and
         * tg_span_in_tunnel to walk the line back off a gore or out of a bore,
         * so no caller can reproduce it from ring_len and the RUN-OFF knob. */
        out_stats->grid_span   = TD5_TG_GRID_SPAN;
        out_stats->finish_span = tg_finish_span(s_ring_len);
        out_stats->fork_count = s_fork_count;
        out_stats->cancelled  = s_preview_cancelled;
        for (s = 0; s < TD5_TG_SECTION_COUNT; s++)
            out_stats->tally[s] = tally[s];
        if (nl.count > 0) {
            double lo = nl.v[0].y, hi = nl.v[0].y;
            int i;
            for (i = 1; i < nl.count; i++) {
                if (nl.v[i].y < lo) lo = nl.v[i].y;
                if (nl.v[i].y > hi) hi = nl.v[i].y;
            }
            out_stats->min_y = tg_round(lo);
            out_stats->max_y = tg_round(hi);
        }
    }

    free(nl.v);
    tg_world_free();                 /* [TOPOLOGY-FIRST] built by the walk */
    tg_buf_free(&strip);

    if (!ok && !s_preview_cancelled)
        TD5_LOG_W(LOG_TAG, "trackgen: preview route for seed %u FAILED",
                  spec->seed);
    return ok;
}

int td5_trackgen_regenerate_main_spans(unsigned int seed,
                                      unsigned char **out_bytes,
                                      int *out_span_count)
{
    TD5_TrackGenSpec spec;
    TG_NodeList nl;
    TG_Buf spans, verts;
    int vtx_count = 0, ok = 0, block;

    if (!out_bytes || !out_span_count) return 0;
    *out_bytes = NULL;
    *out_span_count = 0;

    td5_trackgen_default_spec(&spec);
    td5_trackgen_apply_config(&spec);
    spec.seed = seed;
    /* [R21 ROLLS] This path re-derives the main spans for the streaming
     * consumer and its documented contract is that the bytes provably match
     * what the seed produced. Without the same fold the road here would be
     * built from the shipped defaults while the race used the rolls. */
    tg_rolls_resolve(seed);
    tg_rolls_apply_spec(&spec);

    memset(&nl, 0, sizeof(nl));
    memset(&spans, 0, sizeof(spans));
    memset(&verts, 0, sizeof(verts));

    tg_srand(spec.seed);
    block = td5_env_int("TD5RE_AUTOTRACK_BLOCK", TD5_TG_ORIGIN_BLOCK, 1, 20);

    {
        int tally[TD5_TG_SECTION_COUNT];
        memset(tally, 0, sizeof(tally));
        if (tg_build_centerline(&spec, &nl, tally)) {
            tg_apply_elevation(&spec, &nl);
            if (tg_emit_span_range(&nl, 0, nl.count - 1, block,
                                   &spans, &verts, &vtx_count)) {
                *out_bytes = spans.b;          /* hand the buffer over */
                *out_span_count = (int)(spans.len / 24);
                spans.b = NULL;                /* so tg_buf_free does not free it */
                spans.len = spans.cap = 0;
                ok = 1;
            }
        }
    }

    free(nl.v);
    tg_world_free();                 /* [TOPOLOGY-FIRST] built by the walk */
    tg_buf_free(&spans);
    tg_buf_free(&verts);

    if (ok)
        TD5_LOG_I(LOG_TAG, "trackgen: regenerated %d main span records for "
                  "seed %u (%d bytes)", *out_span_count, seed,
                  *out_span_count * 24);
    else
        TD5_LOG_E(LOG_TAG, "trackgen: regenerate for seed %u FAILED", seed);
    return ok;
}

/* [S2 GATE, generator half] Regenerating twice from one seed must give
 * identical bytes -- otherwise an in-place rewrite could not be trusted to
 * reproduce what the track was built from. Cheap, so it runs with the same
 * TD5RE_AUTOTRACK_SELFCHECK knob as the S1 gate. */
static void tg_selfcheck_regen(unsigned int seed)
{
    unsigned char *a = NULL, *b = NULL;
    int na = 0, nb = 0;

    if (!td5_trackgen_regenerate_main_spans(seed, &a, &na) ||
        !td5_trackgen_regenerate_main_spans(seed, &b, &nb)) {
        TD5_LOG_E(LOG_TAG, "trackgen selfcheck: regen FAILED");
    } else if (na != nb || memcmp(a, b, (size_t)na * 24) != 0) {
        TD5_LOG_E(LOG_TAG, "trackgen selfcheck: regen NOT deterministic "
                  "(%d vs %d spans)", na, nb);
    } else {
        TD5_LOG_I(LOG_TAG, "trackgen selfcheck: regen PASS -- seed %u "
                  "reproduces %d identical span records on a second call",
                  seed, na);
    }
    free(a);
    free(b);
}

/* ------------------------------------------------------- lifecycle ------- */
int td5_trackgen_init(void)
{
    /* [R14 GENPERF 2026-09-03] No build at boot. This used to generate a whole
     * track here "so the entry is loadable from the main menu onwards", and
     * every race entry then threw it away and generated AGAIN with a fresh
     * seed (td5_game.c). Measured: 19.8 s of a 76 s launch-to-green-light was
     * this discarded build, paid by every player, including those who never
     * pick the auto track. The selector needs only the REGISTRY entry (name,
     * circuit flag, start span); the finish span is re-registered by the real
     * build before the level is loaded, and the frontend never reads the
     * level files themselves. */
    TD5_TrackGenSpec spec;
    td5_trackgen_default_spec(&spec);
    td5_trackgen_apply_config(&spec);
    td5_track_registry_set_auto(TD5_TG_SLOT, TD5_TG_LEVEL_NUM, TD5_TG_TRACK_NAME,
                               spec.circuit, TD5_TG_GRID_SPAN, 0);
    TD5_LOG_I(LOG_TAG, "trackgen: " TD5_TG_TRACK_NAME " registered (slot %d, level %d); "
              "built on race entry", TD5_TG_SLOT, TD5_TG_LEVEL_NUM);
    return 1;
}

static unsigned int tg_fnv1a(unsigned int h, const void *p, size_t n)
{
    const unsigned char *b = (const unsigned char *)p;
    size_t i;
    for (i = 0; i < n; i++) { h ^= b[i]; h *= 16777619u; }
    return h;
}

static unsigned int tg_env_hash(void)
{
    unsigned int h = 2166136261u;
    char **e;
    for (e = _environ; e && *e; e++)
        if (strncmp(*e, "TD5RE_", 6) == 0 && strncmp(*e, "TD5RE_CONTROL_PORT", 18) != 0
            && strncmp(*e, "TD5RE_WINDOW_TITLE", 18) != 0 && strncmp(*e, "TD5RE_RT_", 9) != 0
            && strncmp(*e, "TD5RE_FRAME_CAP", 15) != 0 && strncmp(*e, "TD5RE_FRAMEDUMP", 15) != 0
            && strncmp(*e, "TD5RE_D3D12_", 12) != 0 && strncmp(*e, "TD5RE_TG_REPORTS", 16) != 0
            && strncmp(*e, "TD5RE_R14_GENPROF", 17) != 0)
            h = tg_fnv1a(h, *e, strlen(*e));
    return h;
}

static unsigned long long tg_exe_id(void)
{
    char *path = NULL;
    struct stat st;
    if (_get_pgmptr(&path) != 0 || !path || stat(path, &st) != 0) return 0ull;
    return ((unsigned long long)st.st_mtime << 20) ^ (unsigned long long)st.st_size;
}

static void tg_stamp_path(char *out, size_t n)
{
    snprintf(out, n, "re/assets/levels/level%03d/GENSTAMP.TXT", TD5_TG_LEVEL_NUM);
}

static int tg_stamp_read(TG_Stamp *st)
{
    char path[256];
    FILE *f;
    int n;
    tg_stamp_path(path, sizeof(path));
    f = fopen(path, "r");
    if (!f) return 0;
    memset(st, 0, sizeof(*st));
    n = fscanf(f, "v%u seed=%u spec=%x env=%x exe=%llx spans=%d ring=%d finish=%d circuit=%d night=%d",
               &st->version, &st->seed, &st->spec_hash, &st->env_hash, &st->exe_id,
               &st->spans, &st->ring, &st->finish, &st->circuit, &st->night);
    fclose(f);
    return n == 10;
}

static void tg_stamp_write(const TG_Stamp *st)
{
    char path[256];
    FILE *f;
    tg_stamp_path(path, sizeof(path));
    f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "v%u seed=%u spec=%x env=%x exe=%llx spans=%d ring=%d finish=%d circuit=%d night=%d\n",
            st->version, st->seed, st->spec_hash, st->env_hash, st->exe_id,
            st->spans, st->ring, st->finish, st->circuit, st->night);
    fclose(f);
}

static int tg_level_files_present(void)
{
    static const char *const k_files[] = { "STRIP.DAT", "MODELS.DAT", "LEVELINF.DAT",
                                           "LEFT.TRK", "RIGHT.TRK", "TEXTURES.DAT" };
    char path[256];
    size_t i;
    for (i = 0; i < sizeof(k_files) / sizeof(k_files[0]); i++) {
        snprintf(path, sizeof(path), "re/assets/levels/level%03d/%s", TD5_TG_LEVEL_NUM, k_files[i]);
        if (!td5_plat_file_exists(path)) return 0;
    }
    return 1;
}

int td5_trackgen_prepare_race(int restart, int streamed)
{
    /* A pause-menu RESTART re-enters the same race: keep the seed (and, via
     * the stamp, skip the rebuild). Any other entry rolls a new one, unless
     * TD5RE_AUTOTRACK_SEED pins it (handled inside regenerate). */
    unsigned int seed = (restart && s_last_seed) ? s_last_seed : 0u;
    /* Dev A/B: TD5RE_TG_DOUBLE_BUILD=1 builds twice (second build wins), the
     * way the old boot+race flow did, to expose generator state that leaks
     * from one build into the next. Deliberately NOT streamed: the point is to
     * compare two complete builds, and a streamed build writes no MODELS.DAT. */
    if (td5_env_flag_off("TD5RE_TG_DOUBLE_BUILD")) {
        unsigned int pinned = (unsigned int)td5_env_int("TD5RE_AUTOTRACK_SEED", 0, 0, 0x7FFFFFFF);
        _putenv_s("TD5RE_AUTOTRACK_REUSE", "0");
        td5_trackgen_regenerate(seed ? seed : pinned);
        return td5_trackgen_regenerate(seed ? seed : pinned);
    }
    /* [SCENERY STREAMING] `streamed` is the CALLER's decision, passed in rather
     * than read here, because the knob (td5_tgstream_enabled) belongs to the
     * consumer module td5_trackgen_stream.c -- the generator must not include
     * its own consumer. A streamed build stops after the geometry and parks
     * what the scenery phases need; td5_tgstream_begin() picks it up once the
     * level has loaded. If the GENSTAMP reuse inside regenerate short-circuits
     * the build, nothing is parked, s_stream_pending stays 0 and _begin is a
     * no-op -- which is correct, because a reused build already has its
     * MODELS.DAT on disk and owes no scenery. */
    return streamed ? td5_trackgen_regenerate_streamed(seed)
                    : td5_trackgen_regenerate(seed);
}

void td5_trackgen_shutdown(void)
{
    s_last_seed = 0;
}

/* -------------------------------------------------------- identity ------- */
int td5_trackgen_level_number(void) { return TD5_TG_LEVEL_NUM; }

int td5_trackgen_slot(void)         { return TD5_TG_SLOT; }

int td5_trackgen_is_auto_slot(int slot) { return slot == TD5_TG_SLOT; }

unsigned int td5_trackgen_last_seed(void) { return s_last_seed; }

int td5_trackgen_regenerate(unsigned int seed)
{
    TD5_TrackGenSpec spec;
    int spans = 0;

    /* [R21 ROLLS] Before the knobs are read: hand back anything the LAST build
     * published, or apply_config would read a previous roll as if a human had
     * pinned it. */
    tg_rolls_unpublish();

    td5_trackgen_default_spec(&spec);
    td5_trackgen_apply_config(&spec);

    /* TD5RE_AUTOTRACK_SEED pins the seed so two runs generate the IDENTICAL
     * track. Without this any A/B measurement compares two different random
     * roads and attributes the difference to whatever knob was changed. */
    if (seed == 0) {
        int pinned = td5_env_int("TD5RE_AUTOTRACK_SEED", 0, 0, 0x7FFFFFFF);
        seed = pinned ? (unsigned int)pinned
                      : (unsigned int)td5_plat_time_ms() * 2654435761u
                        + 0x9E3779B9u;
    }
    spec.seed = seed;

    /* [R2 item 22] Time of day is decided HERE -- regenerate is what a race
     * launch calls, so this is "on entering the race" -- and BEFORE the build,
     * so every emitter that asks td5_trackgen_is_night() during it agrees. */
    tg_decide_night(seed);

    /* [R21 ROLLS] Resolve the randomized parameters and fold them into the spec
     * HERE: after the seed is final, before the stamp is hashed, before the
     * build. Two consequences worth stating.
     *
     * spec_hash BELOW COVERS THEM. tg_rolls_apply_spec writes into `spec`, and
     * want.spec_hash is taken from the finished struct, so a different roll is
     * a different stamp and the REUSE path can never serve a track that does
     * not match the seed. That is why the registry needs no TG_STAMP_VERSION
     * bump -- and why a roll must stay a pure function of seed + environment,
     * both of which the stamp already covers.
     *
     * The report is emitted BEFORE the early return below, so a reused build
     * still says what it is; it otherwise prints no inventory at all. */
    tg_rolls_resolve(seed);
    tg_rolls_apply_spec(&spec);
    tg_rolls_report();

    /* [R14 GENPERF 2026-09-03] Identical build already on disk? Then the only
     * work is the cheap prologue the runtime depends on (biome grid, night,
     * seed latch, ring length) and the registry line. */
    {
        TG_Stamp want, have;
        memset(&want, 0, sizeof(want));
        want.version   = TG_STAMP_VERSION;
        want.seed      = seed;
        want.spec_hash = tg_fnv1a(2166136261u, &spec, sizeof(spec));
        want.env_hash  = tg_env_hash();
        want.exe_id    = tg_exe_id();
        if (td5_env_flag_on("TD5RE_AUTOTRACK_REUSE") && tg_stamp_read(&have) &&
            have.version == want.version && have.seed == want.seed &&
            have.spec_hash == want.spec_hash && have.env_hash == want.env_hash &&
            have.exe_id == want.exe_id && have.exe_id != 0ull &&
            have.spans > 0 && have.ring > 0 && tg_level_files_present()) {
            tg_biome_layout(seed, spec.target_spans);
            s_gen_seed  = seed;
            s_last_seed = seed;
            s_ring_len  = have.ring;
            s_tg_progress = 100;
            td5_track_registry_set_auto(TD5_TG_SLOT, TD5_TG_LEVEL_NUM,
                                       TD5_TG_TRACK_NAME, have.circuit,
                                       TD5_TG_GRID_SPAN, have.finish);
            TD5_LOG_W(LOG_TAG, "trackgen: REUSED the on-disk build for seed %u "
                      "(%d spans, ring %d, finish %d) -- generation skipped",
                      seed, have.spans, have.ring, have.finish);
            return 1;
        }
    }

    if (!td5_trackgen_build_level(&spec, TD5_TG_LEVEL_NUM, &spans)) {
        TD5_LOG_E(LOG_TAG, "trackgen: regenerate failed; auto track unavailable");
        return 0;
    }

    s_last_seed = seed;

    if (s_selfcheck_regen_seed) {
        unsigned int sd = s_selfcheck_regen_seed;
        s_selfcheck_regen_seed = 0;
        tg_selfcheck_regen(sd);
    }

    /* [R3 item 18] Finish span for the registry MUST be the same MAIN-RING span
     * that tg_emit_levelinf placed the last checkpoint on -- tg_finish_span(ring)
     * -- NOT `spans - 4`.
     *
     * `spans` is the FULL emitted strip count, which also counts every fork's pad
     * and appended corridor tail (with branches on seed 99991 the main ring is
     * ~1800 spans but `spans` is ~1987). The registry finish span is consumed by
     * advance_pending_finish_state as s_td6_finish_span, and that P2P branch takes
     * PRECEDENCE over the LEVELINF checkpoints (it returns before reaching the
     * checkpoint-crossing code). So `spans - 4` put the finish line on a CORRIDOR
     * tail (~1983) that a normal main-line drive never reaches: the race could
     * only finish by driving ~180 spans into a second lap, which is why the user
     * saw the race never finish and race.log logged zero checkpoint-pass events
     * (that code was dead). Deriving the finish from s_ring_len makes the registry
     * finish, the LEVELINF last checkpoint and the finish banner all agree on one
     * on-ring span. */
    {
        int ring   = (s_ring_len > 0) ? s_ring_len : spans;
        int finish = tg_finish_span(ring);
        if (finish <= 0)   /* ring too short for a placed finish: last-resort */
            finish = (spans > 8) ? spans - 4 : spans - 1;
        td5_track_registry_set_auto(TD5_TG_SLOT, TD5_TG_LEVEL_NUM,
                                   TD5_TG_TRACK_NAME, spec.circuit,
                                   TD5_TG_GRID_SPAN, finish);
        TD5_LOG_I(LOG_TAG, "trackgen: registry finish span=%d (main ring=%d, full "
                  "strip=%d; old spans-4 would be %d)", finish, ring, spans,
                  spans > 8 ? spans - 4 : spans - 1);
        /* [R14 GENPERF] Stamp what this build was made from (see TG_Stamp). */
        {
            TG_Stamp st;
            memset(&st, 0, sizeof(st));
            st.version   = TG_STAMP_VERSION;
            st.seed      = seed;
            st.spec_hash = tg_fnv1a(2166136261u, &spec, sizeof(spec));
            st.env_hash  = tg_env_hash();
            st.exe_id    = tg_exe_id();
            st.spans = spans; st.ring = ring; st.finish = finish;
            st.circuit = spec.circuit; st.night = s_is_night;
            tg_stamp_write(&st);
        }
        s_tg_progress = 100;
    }

    TD5_LOG_I(LOG_TAG, "trackgen: auto track ready (slot %d, level %d, "
              "seed %u, %d spans)", TD5_TG_SLOT, TD5_TG_LEVEL_NUM, seed, spans);
    return 1;
}

/* [PERF LEVER 1] Geometry only: strip, routes, levelinf, sky -- no MODELS.DAT
 * and no texture pages. See s_want_scenery for why boot does not need them.
 * The registry entry this produces is identical, because the finish span comes
 * from s_ring_len (the strip), not from the scenery. */
int td5_trackgen_regenerate_geometry_only(unsigned int seed)
{
    int r;
    s_want_scenery = 0;
    r = td5_trackgen_regenerate(seed);
    s_want_scenery = 1;     /* restore before any race-launch build */
    return r;
}

/* ===================== SECTION: streamed scenery producer ==================
 * The consumer half of this lives in td5_trackgen_stream.c, which owns the
 * worker thread and the lifecycle. Everything here is the generator's side:
 * a build that stops after the geometry, and a driver that runs the scenery
 * phases later, publishing each entry as it is assembled.
 *
 * SINGLE INSTANCE, STILL. The generator has ~102 mutable file-scope statics
 * and the stashed node list is read by the worker, so between the streamed
 * build and td5_trackgen_stream_discard NOTHING else may run a build or a
 * route preview. td5_asset_load_level joins both workers before regenerating;
 * the studio screen joins them too. */

int td5_trackgen_regenerate_streamed(unsigned int seed)
{
    int r;
    s_want_stream = 1;
    r = td5_trackgen_regenerate(seed);
    s_want_stream = 0;      /* one build only; every other path is unaffected */
    if (!r) td5_trackgen_stream_discard();
    return r;
}

int td5_trackgen_stream_pending(void)    { return s_stream_pending; }

int td5_trackgen_stream_span_count(void) { return s_stream_nspans; }

int td5_trackgen_stream_entry_count(void)
{
    if (!s_stream_pending) return 0;
    return (s_stream_nspans + TD5_TG_SPANS_PER_ENTRY - 1)
           / TD5_TG_SPANS_PER_ENTRY;
}

void td5_trackgen_stream_discard(void)
{
    tg_world_free();                 /* [TOPOLOGY-FIRST] travelled with nl */
    free(s_stream_nl.v);
    memset(&s_stream_nl, 0, sizeof(s_stream_nl));
    s_stream_pending = 0;
    s_stream_nspans = 0;
    s_stream_lanes = 0;
}

/* Run the scenery phases for the stashed build, publishing each entry to the
 * track module as soon as it exists. Called ONLY from the streaming worker.
 *
 * `cancel` is polled between entries, which is the only place it is safe to
 * stop: an entry is the unit the flora rule makes indivisible, and a
 * half-assembled block must never be published.
 *
 * Still writes MODELS.DAT at the end, which is not bookkeeping -- it is what
 * keeps the byte-identity gate available. The same blocks assembled in the
 * same order must produce the same file as the non-streamed path, so a
 * streamed build can be sha256-compared against TD5RE_AUTOTRACK_STREAM=0.
 *
 * Returns 1 only when the whole table was published. */
int td5_trackgen_stream_scenery(volatile int *cancel)
{
    TG_Buf models;
    int e, nentries, ok = 1;

    if (!s_stream_pending) return 0;
    memset(&models, 0, sizeof(models));

    if (!tg_scenery_begin(&s_stream_nl, s_stream_nspans, s_stream_lanes)) {
        TD5_LOG_W(LOG_TAG, "trackgen: streamed scenery begin failed");
        return 0;
    }
    nentries = s_scn.nentries;

    for (e = 0; e < nentries && s_scn.ok; e++) {
        if (cancel && *cancel) { ok = 0; break; }
        tg_scenery_entry(e);
        if (!s_scn.ok) { ok = 0; break; }
        td5_track_scenery_publish_entry(e, s_scn.blocks[e].b,
                                        s_scn.blocks[e].len);
    }

    /* _end runs even after a cancel: it owns the per-entry buffers, and
     * skipping it would leak every one of them plus the side-terrain table. */
    if (!tg_scenery_end(&models))
        ok = 0;
    else if (ok) {
        tg_write_file(s_stream_dir, "MODELS.DAT", models.b, models.len);
        tg_meshtag_write(s_stream_dir);   /* [PICK] sidecar, separate file */
    }
    tg_buf_free(&models);

    /* (No per-build report here. The incoming commit called its TG_PF
     * per-emitter report at this point; master reports through the TG_ZONE
     * hierarchy from build_level, on the MAIN thread, and this function runs on
     * the streaming worker -- printing the shared zone tallies from here would
     * both race the main thread and attribute the worker's time to whatever
     * build reports next.) */
    return ok;
}
