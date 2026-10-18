/**
 * td5_roam_stream.c -- GEO FREE ROAM: tile streaming (PORT-ONLY, F3-A).
 * Contract, layers and rules: td5_roam_stream.h. Design: GEO_FREE_ROAM_DESIGN.md 3.
 *
 * Threading follows td5_trackgen_stream.c (raw CreateThread, the main FP env applied
 * first in the thread, a one-way cancel flag, join on the main thread) with one
 * difference: the worker is persistent for the session and talks to the main thread
 * through two queues (requests, results) under one critical section. Everything the
 * locator and the renderer read is touched on the main thread only, between frames.
 * The worker owns its tile-builder context (opened lazily, first build or verify)
 * and its own height source; it never reads a tile the main thread owns.
 */
#include <windows.h>
#include <psapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "td5re.h"
#include "td5_platform.h"
#include "td5_config.h"
#include "td5_fp.h"
#include "td5_geo.h"
#include "td5_types.h"
#include "td5_camera.h"         /* g_cameraPos: the SCENERY centre */
#include "td5_race_state.h"     /* td5_game_get_actor (read-only roster) */
#include "td5_roam_tile.h"
#include "td5_roam_cache.h"     /* worker-side census (way count) for the memory log */
#include "td5_roam_locate.h"
#include "td5_roam_render.h"
#include "td5_roam_stream.h"
#include "td5_roam_physics.h"   /* td5_roam_physics_note_frame */
#include "td5_roam_mode.h"      /* td5_roam_active (the full-load memory samples) */
#include "td5_hitchwatch.h"      /* [F3-B DEV] what the worker is doing, for the stall sampler */
#include "td5_roam_scenery.h"   /* [F4a] the worker's scenery source              */
#include "td5_roam_partido.h"   /* [F9b] which lattice tiles are streamable (the place boundary) */
#include "td5_roam_rebase.h"    /* [F9b] frame time of a rebase frame */
#include "../../../re/include/td5_actor_struct.h"

#define LOG_TAG "roam"

#define RS_ROAD          1u
#define RS_SCEN          2u
#define RS_MAX_TILES     2048           /* the casco list (pin-all harness)           */
#define RS_HASH0         4096           /* [F9b] initial hash size, power of two, grows at 50 % load */
#define RS_MAX_JOBS      2048
#define RS_MAX_RESULTS   512
#define RS_SLOTS         32             /* == TD5_MAX_TOTAL_ACTORS                   */
#define RS_RACERS        6
#define RS_UPM           430.0
#define RS_TICKS_PER_S   30.0
#define RS_FRAME_BINS    5000           /* 0.1 ms bins, 0 .. 500 ms                  */
#define RS_PRIO_GATE     (-100)
#define RS_PRIO_URGENT   (-1000)
#define RS_PRIO_VERIFY   100000         /* below every load                          */
#define RS_MISS_LOG      20

enum { RS_JOB_LOAD = 1, RS_JOB_VERIFY = 2 };

typedef struct {
    int        tx, tz;
    RoamTile  *road;              /* ROAD-resident (in the locator set), NULL = not   */
    int        scen;              /* baked tile installed in the render              */
    size_t     road_bytes, scen_bytes;
    uint8_t    want, keep, pend;  /* RS_ROAD | RS_SCEN, this frame / in flight       */
    uint8_t    pinned, gate;      /* pin_all / the start gate want ROAD (+SCEN)      */
    uint8_t    verify;            /* 0 not yet, 1 queued, 2 done                     */
    uint8_t    failed;            /* no file and no build: given up this session     */
    uint8_t    built;             /* the worker built it this session                */
    uint8_t    outside;           /* [F9b] not streamable (beyond the place boundary): never loaded */
    uint32_t   out_ms[2];         /* when the layer left the keep ring (0 = inside)  */
    uint32_t   used_ms[2];        /* last time the layer was kept (LRU)              */
    int        prio;
} RsTile;

typedef struct { int idx, kind, prio; int tx, tz; uint8_t layers; } RsJob;   /* [F9b] tx,tz: the worker never reads the tile table */

typedef struct {
    int            idx, kind;
    int            tx, tz;
    uint8_t        layers;
    RoamTile      *road;          /* heap, owned by the result until installed       */
    TD5_RoamBaked *baked;
    int            built, stale, failed, verified;
    uint32_t       ms_read, ms_build, ms_bake, ms_verify;
    char           err[96];
} RsResult;

typedef struct {
    /* fixed for the session */
    char               place[64];
    RoamGrid           grid;
    TD5_RoamResident  *rs;
    RsTile            *tile;               /* [F9b] grows on demand (main thread only)   */
    int                ntile, captile;
    int32_t           *hash;  int nhash;   /* [F9b] open addressing, power of two        */
    int                partido;            /* [F9b] 1 = every tile of the place boundary, 0 = the casco list */
    int                ncasco;
    /* knobs */
    int   road_r, view_r, lead_ms, linger_ms, lru, verify, build, worker_on;
    int   inst_road, inst_scen, gate_ms, stat_ms, guard_m;
    double road_budget, scen_budget;      /* bytes */
    /* worker */
    HANDLE             thread, wake, done;
    CRITICAL_SECTION   cs;
    int                cs_live;
    volatile LONG      cancel;
    TD5_FpEnv          fpenv;
    TD5_RoamTileCtx   *ctx;               /* worker-owned                             */
    int                ctx_failed;
    TD5_RoamHeight    *hc;                /* worker-owned                             */
    int                hc_ok;
    TD5_RoamScnSource *scn;               /* [F4a] worker-owned scenery source        */
    RsJob              job[RS_MAX_JOBS];  int njob;        /* under cs */
    RsResult           res[RS_MAX_RESULTS]; int nres;      /* under cs */
    /* main-only inbox (results moved out of the shared queue) */
    RsResult          *inbox;  int ninbox, capinbox;
    /* stats */
    TD5_RoamStreamStats st;
    uint32_t           t_begin_ms;
    uint64_t           last_tick_us;
    int                first_tick_done, prev_installed, warm;
    int                gate_done;         /* the spawn gate ran: the guard is live from now */
    uint32_t           warm_ms;
    uint32_t          *fhist;             /* RS_FRAME_BINS                            */
    long               fcount;
    uint32_t           next_stat_ms, next_log_ms;
    FILE              *csv;
    uint32_t           verify_ms_max, build_ms_max, bake_ms_max;
    double             last_priv_mb;      /* main-thread memory jump attribution        */
    int                last_installed, last_evicted;
    long               guard_tick[RS_SLOTS];
    uint8_t            guard_valid[RS_SLOTS], guard_hold[RS_SLOTS];
} RsSession;

typedef struct {
    TD5_RoamStreamLoadscreenFn loadscreen;   /* registered by td5_game.c, survives sessions */
    RsSession *s;
    uint32_t   f2_t0, f2_next;               /* TD5RE_ROAM_STREAM=0: private-bytes samples (A/B) */
    volatile const char *doing;              /* [F3-B DEV] the worker's current step (static strings) */
} RoamStream;

static RoamStream s_stream;

/* ============================================================================
 * SECTION: small helpers
 * ========================================================================== */

/* [F3-B DEV] the worker's current step, read by the stall sampler from its own thread */
#define RS_DOING(str) (s_stream.doing = (str))
static const char *rs_doing(void) { const char *d = (const char *)s_stream.doing; return d ? d : "none"; }

static int rs_hash_of(int tx, int tz, int nhash) { return (int)(((unsigned)tx * 73856093u ^ (unsigned)tz * 19349663u) & (unsigned)(nhash - 1)); }

static int rs_find(const RsSession *s, int tx, int tz)
{
    int h = rs_hash_of(tx, tz, s->nhash), n;
    for (n = 0; n < s->nhash; n++, h = (h + 1) & (s->nhash - 1)) {
        const int i = s->hash[h];
        if (i < 0) return -1;
        if (s->tile[i].tx == tx && s->tile[i].tz == tz) return i;
    }
    return -1;
}

static void rs_hash_put(int32_t *hash, int nhash, int tx, int tz, int idx)
{
    int h = rs_hash_of(tx, tz, nhash);
    while (hash[h] >= 0) h = (h + 1) & (nhash - 1);
    hash[h] = idx;
}

/* [F9b] Append a table entry for (tx,tz) (not present). The tile array and the hash grow; nothing
 * outside the main thread holds an RsTile pointer or index-derived pointer across this call. -1 on
 * out of memory. A tile beyond the place boundary gets an entry too, flagged `outside`, so the
 * polygon test runs once per tile. */
static int rs_add(RsSession *s, int tx, int tz, int outside)
{
    RsTile *t;
    if (s->ntile >= s->captile) {
        const int nc = s->captile ? s->captile * 2 : 1024;
        RsTile *nt = (RsTile *)realloc(s->tile, (size_t)nc * sizeof *nt);
        if (!nt) return -1;
        s->tile = nt; s->captile = nc;
    }
    if ((s->ntile + 1) * 2 > s->nhash) {
        const int nh = s->nhash * 2;
        int32_t *h2 = (int32_t *)malloc((size_t)nh * sizeof(int32_t));
        int i;
        if (!h2) return -1;
        for (i = 0; i < nh; i++) h2[i] = -1;
        for (i = 0; i < s->ntile; i++) rs_hash_put(h2, nh, s->tile[i].tx, s->tile[i].tz, i);
        free(s->hash);
        s->hash = h2; s->nhash = nh;
    }
    t = &s->tile[s->ntile];
    memset(t, 0, sizeof *t);
    t->tx = tx; t->tz = tz;
    t->outside = (uint8_t)(outside ? 1 : 0);
    t->prio = 1 << 20;
    rs_hash_put(s->hash, s->nhash, tx, tz, s->ntile);
    return s->ntile++;
}

/* [F9b] The entry of (tx,tz), created on first use when the session streams the whole place; -1 for
 * a tile that is not streamable (beyond the boundary, or outside the casco list in casco mode). */
static int rs_get(RsSession *s, int tx, int tz)
{
    int i = rs_find(s, tx, tz);
    if (i < 0) {
        if (!s->partido) return -1;
        i = rs_add(s, tx, tz, !td5_roam_partido_tile_ok(&s->grid, tx, tz));
        if (i < 0) return -1;
    }
    return s->tile[i].outside ? -1 : i;
}

static int rs_tile_of_place(RsSession *s, double px, double pz)
{
    int tx, tz;
    roam_grid_tile_of(&s->grid, px, pz, &tx, &tz);
    return rs_get(s, tx, tz);
}

static uint8_t rs_resident(const RsTile *t) { return (uint8_t)((t->road ? RS_ROAD : 0u) | (t->scen ? RS_SCEN : 0u)); }

static double rs_private_mb(void)
{
    PROCESS_MEMORY_COUNTERS_EX pmc;
    memset(&pmc, 0, sizeof pmc);
    pmc.cb = sizeof pmc;
    if (!GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&pmc, sizeof pmc)) return 0.0;
    return (double)pmc.PrivateUsage / 1048576.0;
}

static void rs_tile_path(const RsSession *s, int tx, int tz, char *out, size_t cap)
{
    td5_roam_tile_path(out, cap, NULL, s->place, tx, tz);
}

void td5_roam_stream_set_loadscreen(TD5_RoamStreamLoadscreenFn fn) { s_stream.loadscreen = fn; }

int td5_roam_stream_enabled(void) { return td5_env_int("TD5RE_ROAM_STREAM", 1, 0, 1); }

int td5_roam_stream_active(void) { return s_stream.s != NULL; }

/* ============================================================================
 * SECTION: the worker's unit of work (also run inline when there is no thread)
 * ========================================================================== */

/* Read the tile file into a heap RoamTile. 1 on success. */
static int rs_read(const RsSession *s, int tx, int tz, RoamTile **out, char *err, size_t errn)
{
    char path[512];
    RoamTile *t = (RoamTile *)calloc(1, sizeof *t);
    *out = NULL;
    if (!t) { snprintf(err, errn, "out of memory"); return 0; }
    rs_tile_path(s, tx, tz, path, sizeof path);
    if (!td5_roam_tile_read(path, t, 0, err, errn)) { free(t); return 0; }
    *out = t;
    return 1;
}

/* The worker's tile-builder context, opened on first use (a build or a verify). It holds
 * the process-wide OSM cache session of td5_roam_cache.c for the rest of the session. */
static TD5_RoamTileCtx *rs_ctx(RsSession *s)
{
    if (!s->ctx && !s->ctx_failed) {
        const uint32_t t0 = td5_plat_time_ms();
        const double m0 = rs_private_mb();
        RS_DOING("ctx_open");
        s->ctx = td5_roam_tile_ctx_open(s->place, 0, NULL);
        if (!s->ctx) s->ctx_failed = 1;
        TD5_LOG_I(LOG_TAG, "ROAM_STREAM worker: tile builder context for %s %s in %u ms (private %+.1f MB)", s->place,
                  s->ctx ? "open" : "FAILED", (unsigned)(td5_plat_time_ms() - t0), rs_private_mb() - m0);
    }
    return s->ctx;
}

/* Build (tx,tz) with the worker's ctx and write the file (temp name + rename). */
static int rs_build(RsSession *s, int tx, int tz, RoamTile **out, char *err, size_t errn)
{
    char path[512], tmp[560], dir[512];
    RoamTile *t;
    TD5_RoamTileStats st;
    *out = NULL;
    if (!s->build) { snprintf(err, errn, "build off (TD5RE_ROAM_STREAM_BUILD=0)"); return 0; }
    if (!rs_ctx(s)) { snprintf(err, errn, "no builder context"); return 0; }
    t = (RoamTile *)calloc(1, sizeof *t);
    if (!t) { snprintf(err, errn, "out of memory"); return 0; }
    RS_DOING("build");
    if (!td5_roam_tile_build(s->ctx, tx, tz, t, &st)) { free(t); snprintf(err, errn, "build failed"); return 0; }
    rs_tile_path(s, tx, tz, path, sizeof path);
    td5_geo_source_path(dir, sizeof dir, s->place, "roam");
    td5_roam_mkdirs(dir);
    snprintf(tmp, sizeof tmp, "%s.tmp%lu", path, (unsigned long)GetCurrentThreadId());
    if (!td5_roam_tile_write(tmp, t) || !MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileA(tmp);
        TD5_LOG_W(LOG_TAG, "ROAM_STREAM: built tile %d,%d could not be written to %s (kept in memory)", tx, tz, path);
    }
    TD5_LOG_I(LOG_TAG, "ROAM_STREAM_BUILD tile %d,%d %u ms (graph %u ribbon %u) quads=%d tris=%d bytes=%u input=%08x",
              tx, tz, (unsigned)st.ms, (unsigned)st.ms_graph, (unsigned)st.ms_ribbon, st.quads, st.tris,
              (unsigned)st.bytes, (unsigned)t->stamp);
    *out = t;
    return 1;
}

static void rs_free_tile(RoamTile *t)
{
    if (!t) return;
    td5_roam_tile_free(t);
    free(t);
}

static void rs_process(RsSession *s, const RsJob *j, RsResult *r)
{
    const int tx = j->tx, tz = j->tz;     /* [F9b] never s->tile: main may grow it meanwhile */
    RoamTile *t = NULL;
    uint32_t t0 = td5_plat_time_ms();
    memset(r, 0, sizeof *r);
    r->idx = j->idx; r->kind = j->kind; r->layers = j->layers; r->tx = tx; r->tz = tz;
    RS_DOING("read");
    if (!rs_read(s, tx, tz, &t, r->err, sizeof r->err)) {
        r->ms_read = td5_plat_time_ms() - t0;
        t0 = td5_plat_time_ms();
        if (!rs_build(s, tx, tz, &t, r->err, sizeof r->err)) { r->failed = 1; return; }
        r->built = 1;
        r->ms_build = td5_plat_time_ms() - t0;
    } else {
        r->ms_read = td5_plat_time_ms() - t0;
        if (j->kind == RS_JOB_VERIFY) {
            uint32_t want = 0;
            t0 = td5_plat_time_ms();
            if (!rs_ctx(s) || (RS_DOING("verify_hash"), !td5_roam_tile_input_hash(s->ctx, tx, tz, &want))) {
                r->ms_verify = td5_plat_time_ms() - t0;
                snprintf(r->err, sizeof r->err, "verify: no input hash");
                rs_free_tile(t);
                return;                               /* left as it is: not judged */
            }
            r->ms_verify = td5_plat_time_ms() - t0;
            if (want == t->stamp) { r->verified = 1; rs_free_tile(t); return; }
            TD5_LOG_W(LOG_TAG, "ROAM_STREAM_STALE tile %d,%d: file input %08x, sources now %08x: rebuilding",
                      tx, tz, (unsigned)t->stamp, (unsigned)want);
            rs_free_tile(t);
            t = NULL;
            r->stale = 1;
            t0 = td5_plat_time_ms();
            if (!rs_build(s, tx, tz, &t, r->err, sizeof r->err)) { r->failed = 1; return; }
            r->built = 1;
            r->ms_build = td5_plat_time_ms() - t0;
        }
    }
    if (j->layers & RS_SCEN) {
        t0 = td5_plat_time_ms();
        RS_DOING("bake");
        r->baked = td5_roam_render_bake_ex(t, s->hc_ok ? s->hc : NULL, s->scn);
        r->ms_bake = td5_plat_time_ms() - t0;
    }
    if (j->layers & RS_ROAD) r->road = t;
    else rs_free_tile(t);
}

static void rs_result_free(RsResult *r)
{
    rs_free_tile(r->road);
    if (r->baked) td5_roam_render_free_baked(r->baked);
    r->road = NULL; r->baked = NULL;
}

/* take the best pending job (lowest prio, then lowest tile index); 1 when one was taken */
static int rs_take_job(RsSession *s, RsJob *out)
{
    int i, best = -1;
    EnterCriticalSection(&s->cs);
    for (i = 0; i < s->njob; i++)
        if (best < 0 || s->job[i].prio < s->job[best].prio ||
            (s->job[i].prio == s->job[best].prio && s->job[i].idx < s->job[best].idx))
            best = i;
    if (best >= 0) {
        *out = s->job[best];
        s->job[best] = s->job[--s->njob];
    }
    LeaveCriticalSection(&s->cs);
    return best >= 0;
}

static int rs_push_result(RsSession *s, const RsResult *r)
{
    for (;;) {
        EnterCriticalSection(&s->cs);
        if (s->nres < RS_MAX_RESULTS) {
            s->res[s->nres++] = *r;
            LeaveCriticalSection(&s->cs);
            SetEvent(s->done);
            return 1;
        }
        LeaveCriticalSection(&s->cs);
        if (InterlockedCompareExchange(&s->cancel, 0, 0)) return 0;
        Sleep(2);
    }
}

/* [F4a] a neighbour ROAD tile the scenery's guard needs and no file holds: built here */
static RoamTile *rs_scn_build_road(void *u, int tx, int tz)
{
    RsSession *s = (RsSession *)u;
    RoamTile *t = NULL;
    char err[96];
    return rs_build(s, tx, tz, &t, err, sizeof err) ? t : NULL;
}

static void rs_scn_open(RsSession *s)
{
    if (td5_roam_scn_enabled()) s->scn = td5_roam_scn_open(s->place, &s->grid, rs_scn_build_road, s);
}

static DWORD WINAPI rs_thread_proc(LPVOID param)
{
    RsSession *s = (RsSession *)param;
    RsJob j;
    RsResult r;
    /* FIRST, before any build: adopt the main thread's rounding mode (td5_platform.h) */
    td5_plat_fpenv_apply(&s->fpenv);
    RS_DOING("height_open");
    if (s->hc) s->hc_ok = td5_roam_render_open_height(s->hc);
    rs_scn_open(s);
    while (!InterlockedCompareExchange(&s->cancel, 0, 0)) {
        double m0;
        if (!rs_take_job(s, &j)) { RS_DOING("idle"); WaitForSingleObject(s->wake, 50); continue; }
        m0 = rs_private_mb();
        rs_process(s, &j, &r);
        {
            /* attribution of process memory growth to worker jobs (> 2 MB in one job) */
            const double dm = rs_private_mb() - m0;
            if (dm > 2.0)
                TD5_LOG_I(LOG_TAG, "ROAM_STREAM_MEM worker job %s tile %d,%d: private +%.1f MB (built=%d stale=%d, "
                          "cache ways now %d)", j.kind == RS_JOB_VERIFY ? "verify" : "load", j.tx,
                          j.tz, dm, r.built, r.stale, s->ctx ? td5_roam_cache_way_count() : -1);
        }
        RS_DOING("push");
        if (!rs_push_result(s, &r)) { rs_result_free(&r); break; }
    }
    if (s->ctx) { td5_roam_tile_ctx_close(s->ctx); s->ctx = NULL; }
    if (s->hc && s->hc_ok) td5_roam_height_close(s->hc);
    s->hc_ok = 0;
    td5_roam_scn_close(s->scn);
    s->scn = NULL;
    return 0;
}

/* ============================================================================
 * SECTION: requests (main)
 * ========================================================================== */

static void rs_submit(RsSession *s, int idx, int kind, uint8_t layers, int prio)
{
    RsTile *t = &s->tile[idx];
    if (!layers) return;
    EnterCriticalSection(&s->cs);
    if (s->njob < RS_MAX_JOBS) {
        s->job[s->njob].idx = idx; s->job[s->njob].kind = kind;
        s->job[s->njob].tx = t->tx; s->job[s->njob].tz = t->tz;
        s->job[s->njob].layers = layers; s->job[s->njob].prio = prio;
        s->njob++;
        if (kind == RS_JOB_LOAD) t->pend |= layers;
        else t->verify = 1;
    }
    LeaveCriticalSection(&s->cs);
    if (s->wake) SetEvent(s->wake);
}

/* The layers a tile still needs from a LOAD job. */
static uint8_t rs_needed(const RsTile *t)
{
    uint8_t w = t->want;
    if (t->pinned || t->gate) w |= RS_ROAD;
    if (t->gate) w |= RS_SCEN;
    return (uint8_t)(w & ~rs_resident(t) & ~((t->failed) ? (RS_ROAD | RS_SCEN) : 0u));
}

/* Re-prioritise / narrow / drop pending LOAD jobs, then request what is missing. */
static void rs_update_requests(RsSession *s)
{
    int i;
    EnterCriticalSection(&s->cs);
    for (i = 0; i < s->njob; ) {
        RsJob *j = &s->job[i];
        RsTile *t = &s->tile[j->idx];
        if (j->kind == RS_JOB_LOAD) {
            const uint8_t keep = (uint8_t)(j->layers & rs_needed(t));
            if (!keep) {
                t->pend &= (uint8_t)~j->layers;
                s->job[i] = s->job[--s->njob];
                continue;
            }
            t->pend &= (uint8_t)~(j->layers & ~keep);
            j->layers = keep;
            j->prio = t->prio;
        } else if (!t->road) {                /* verify of a tile that left: re-queued if it comes back */
            t->verify = 0;
            s->job[i] = s->job[--s->njob];
            continue;
        }
        i++;
    }
    LeaveCriticalSection(&s->cs);
    for (i = 0; i < s->ntile; i++) {
        RsTile *t = &s->tile[i];
        const uint8_t need = (uint8_t)(rs_needed(t) & ~t->pend);
        if (need) rs_submit(s, i, RS_JOB_LOAD, need, t->prio);
    }
}

/* ============================================================================
 * SECTION: install / evict (main)
 * ========================================================================== */

static void rs_install_road(RsSession *s, int idx, RoamTile *t)
{
    RsTile *tl = &s->tile[idx];
    if (tl->road) {                      /* replace (stale rebuild) */
        td5_roam_resident_remove(s->rs, tl->tx, tl->tz);
        rs_free_tile(tl->road);
        tl->road = NULL;
    }
    if (td5_roam_resident_add(s->rs, t) < 0) {
        TD5_LOG_W(LOG_TAG, "ROAM_STREAM: tile %d,%d could not join the resident set (full?)", tl->tx, tl->tz);
        rs_free_tile(t);
        return;
    }
    tl->road = t;
    tl->road_bytes = t->arena_bytes + sizeof *t;
    tl->out_ms[0] = 0;
}

static void rs_evict_road(RsSession *s, int idx)
{
    RsTile *tl = &s->tile[idx];
    if (!tl->road) return;
    td5_roam_resident_remove(s->rs, tl->tx, tl->tz);
    rs_free_tile(tl->road);
    tl->road = NULL;
    tl->road_bytes = 0;
    tl->out_ms[0] = 0;
    s->st.evicted_road++;
}

static void rs_evict_scen(RsSession *s, int idx)
{
    RsTile *tl = &s->tile[idx];
    if (!tl->scen) return;
    td5_roam_render_remove_tile(tl->tx, tl->tz);
    tl->scen = 0;
    tl->scen_bytes = 0;
    tl->out_ms[1] = 0;
    s->st.evicted_scen++;
}

/* Apply one result. `force` = ignore what the tile wants now (gate / pin / guard). */
static void rs_apply(RsSession *s, RsResult *r, int force, int *n_road, int *n_scen)
{
    RsTile *tl = &s->tile[r->idx];
    const uint8_t keepm = (uint8_t)(tl->keep | tl->want | (tl->pinned || tl->gate ? RS_ROAD : 0u) | (tl->gate ? RS_SCEN : 0u));
    if (r->kind == RS_JOB_LOAD) tl->pend &= (uint8_t)~r->layers;
    else if (tl->verify == 1) tl->verify = 2;
    if (r->failed) {
        s->st.failed++;
        if (r->kind == RS_JOB_LOAD) {
            tl->failed = 1;
            TD5_LOG_W(LOG_TAG, "ROAM_STREAM: tile %d,%d unavailable (%s): given up this session", tl->tx, tl->tz, r->err);
        } else {
            TD5_LOG_W(LOG_TAG, "ROAM_STREAM: tile %d,%d stale and its rebuild failed (%s): the old one stays",
                      tl->tx, tl->tz, r->err);
        }
    }
    if (r->verified) s->st.verified++;
    if (r->ms_verify > s->verify_ms_max) s->verify_ms_max = r->ms_verify;
    if (r->ms_build > s->build_ms_max) s->build_ms_max = r->ms_build;
    if (r->ms_bake > s->bake_ms_max) s->bake_ms_max = r->ms_bake;
    if (r->built) { s->st.built++; tl->built = 1; }
    if (r->stale) s->st.stale++;
    if (r->road) {
        if (r->kind == RS_JOB_VERIFY ? tl->road != NULL : (!tl->road && (force || (keepm & RS_ROAD)))) {
            rs_install_road(s, r->idx, r->road);
            if (n_road) (*n_road)++;
            if (r->kind == RS_JOB_LOAD) s->st.loaded++;
        } else {
            rs_free_tile(r->road);
        }
        r->road = NULL;
    }
    if (r->baked) {
        if (r->kind == RS_JOB_VERIFY ? tl->scen : (force || (keepm & RS_SCEN))) {
            tl->scen_bytes = td5_roam_render_baked_bytes(r->baked);
            tl->scen = td5_roam_render_install(r->baked);
            if (!tl->scen) tl->scen_bytes = 0;
            tl->out_ms[1] = 0;
            if (n_scen) (*n_scen)++;
        } else {
            td5_roam_render_free_baked(r->baked);
        }
        r->baked = NULL;
    }
    if (r->kind == RS_JOB_LOAD && !r->failed && (r->layers & RS_ROAD) && tl->road)
        TD5_LOG_I(LOG_TAG, "ROAM_TILE %d,%d src=%s road_bytes=%u scen_bytes=%u read_ms=%u build_ms=%u bake_ms=%u",
                  tl->tx, tl->tz, r->built ? "built" : "disk", (unsigned)tl->road_bytes, (unsigned)tl->scen_bytes,
                  (unsigned)r->ms_read, (unsigned)r->ms_build, (unsigned)r->ms_bake);
    /* a tile read from disk is verified later, once, at the lowest priority */
    if (s->verify && tl->road && !tl->built && tl->verify == 0 && !tl->failed)
        rs_submit(s, r->idx, RS_JOB_VERIFY, (uint8_t)(tl->scen ? (RS_ROAD | RS_SCEN) : RS_ROAD), RS_PRIO_VERIFY + tl->prio);
}

/* Move the worker's results into the main inbox. */
static void rs_collect(RsSession *s)
{
    int i;
    EnterCriticalSection(&s->cs);
    if (s->ninbox + s->nres > s->capinbox) {
        const int nc = (s->ninbox + s->nres) * 2 + 64;
        RsResult *n = (RsResult *)realloc(s->inbox, (size_t)nc * sizeof *n);
        if (!n) { LeaveCriticalSection(&s->cs); return; }
        s->inbox = n; s->capinbox = nc;
    }
    for (i = 0; i < s->nres; i++) s->inbox[s->ninbox++] = s->res[i];
    s->nres = 0;
    LeaveCriticalSection(&s->cs);
}

/* Run one pending job on THIS thread (no worker). */
static int rs_inline_one(RsSession *s)
{
    RsJob j;
    RsResult r;
    if (!rs_take_job(s, &j)) return 0;
    rs_process(s, &j, &r);
    if (s->ninbox >= s->capinbox) {
        const int nc = s->capinbox * 2 + 64;
        RsResult *n = (RsResult *)realloc(s->inbox, (size_t)nc * sizeof *n);
        if (!n) { rs_result_free(&r); return 1; }
        s->inbox = n; s->capinbox = nc;
    }
    s->inbox[s->ninbox++] = r;
    return 1;
}

/* Apply inbox results within the per-frame budgets (budget < 0 = all). */
static int rs_drain(RsSession *s, int road_budget, int scen_budget, int force)
{
    int i, n = 0, nr = 0, ns = 0, applied = 0;
    rs_collect(s);
    for (i = 0; i < s->ninbox; i++) {
        RsResult *r = &s->inbox[i];
        const int need_r = r->road != NULL, need_s = r->baked != NULL;
        if ((need_r && road_budget >= 0 && nr >= road_budget) || (need_s && scen_budget >= 0 && ns >= scen_budget)) {
            s->inbox[n++] = *r;               /* next frame */
            continue;
        }
        rs_apply(s, r, force, &nr, &ns);
        applied++;
    }
    s->ninbox = n;
    return applied;
}

/* ============================================================================
 * SECTION: wanted set (main)
 * ========================================================================== */

static void rs_mark(RsSession *s, double px, double pz, int R, uint8_t layer, int prio_bias)
{
    int ctx, ctz, dx, dz;
    roam_grid_tile_of(&s->grid, px, pz, &ctx, &ctz);
    for (dz = -R - 1; dz <= R + 1; dz++)
        for (dx = -R - 1; dx <= R + 1; dx++) {
            const int i = rs_get(s, ctx + dx, ctz + dz);
            const int d = abs(dx) > abs(dz) ? abs(dx) : abs(dz);
            RsTile *t;
            if (i < 0) continue;
            t = &s->tile[i];
            t->keep |= layer;
            if (d <= R) {
                const int p = d * 10 + prio_bias;
                t->want |= layer;
                if (p < t->prio) t->prio = p;
            }
        }
}

/* Racer i: place position + the velocity lead point. 0 when the slot has no car. */
static int rs_racer(RsSession *s, int slot, double *px, double *pz, double *lx, double *lz, double *speed_ms)
{
    const TD5_Actor *a = td5_game_get_actor(slot);
    double vx, vz, lead_s;
    if (!a) return 0;
    *px = (double)FP_TRUNC(a->world_pos.x) + s->rs->org_x;
    *pz = (double)FP_TRUNC(a->world_pos.z) + s->rs->org_z;
    vx = (double)a->linear_velocity_x / 256.0 * RS_TICKS_PER_S;     /* units / s */
    vz = (double)a->linear_velocity_z / 256.0 * RS_TICKS_PER_S;
    lead_s = (double)s->lead_ms / 1000.0;
    *lx = *px + vx * lead_s;
    *lz = *pz + vz * lead_s;
    if (speed_ms) *speed_ms = sqrt(vx * vx + vz * vz) / RS_UPM;
    return 1;
}

/* The racer slots the ROAD layer follows: every racer slot that has an actor (rs_racer
 * returns 0 for the rest). Since F3-B a roam race spawns one actor (td5_game.c: no
 * decoration slots), so this is slot 0 alone; the old parked slots 1..5 at the world
 * origin no longer exist, and neither does the 3x3 they kept resident. */
static int rs_racer_count(void)
{
    return RS_RACERS;
}

static void rs_compute_wanted(RsSession *s)
{
    int i, nr, have0 = 0;
    double p0x = 0, p0z = 0, l0x = 0, l0z = 0, sp0 = 0;
    for (i = 0; i < s->ntile; i++) { s->tile[i].want = 0; s->tile[i].keep = 0; s->tile[i].prio = 1 << 20; }
    nr = rs_racer_count();
    for (i = 0; i < nr; i++) {
        double px, pz, lx, lz, sp;
        if (!rs_racer(s, i, &px, &pz, &lx, &lz, &sp)) continue;
        if (i == 0) { have0 = 1; p0x = px; p0z = pz; l0x = lx; l0z = lz; sp0 = sp; }
        rs_mark(s, px, pz, s->road_r, RS_ROAD, 0);
        rs_mark(s, lx, lz, s->road_r, RS_ROAD, 2);
    }
    {
        /* SCENERY round the camera; before the camera is placed (> 1 km from the
         * player) round the player */
        double cx = (double)g_cameraPos[0] + s->rs->org_x, cz = (double)g_cameraPos[2] + s->rs->org_z;
        if (have0 && hypot(cx - p0x, cz - p0z) > 1000.0 * RS_UPM) { cx = p0x; cz = p0z; }
        rs_mark(s, cx, cz, s->view_r, RS_SCEN, 5);
        if (have0 && sp0 > 5.0) rs_mark(s, cx + (l0x - p0x), cz + (l0z - p0z), 1, RS_SCEN, 7);
    }
    for (i = 0; i < s->ntile; i++) {
        RsTile *t = &s->tile[i];
        if (t->pinned && t->prio > 50) t->prio = 50;
    }
}

static void rs_evict(RsSession *s, uint32_t now)
{
    int L;
    for (L = 0; L < 2; L++) {
        const uint8_t bit = L ? RS_SCEN : RS_ROAD;
        const double budget = L ? s->scen_budget : s->road_budget;
        double total = 0.0;
        int i;
        for (i = 0; i < s->ntile; i++) {
            RsTile *t = &s->tile[i];
            const int res = L ? t->scen : (t->road != NULL);
            if (!res) continue;
            total += (double)(L ? t->scen_bytes : t->road_bytes);
            if ((t->keep & bit) || t->gate || (!L && t->pinned)) { t->out_ms[L] = 0; t->used_ms[L] = now; continue; }
            if (!t->out_ms[L]) t->out_ms[L] = now ? now : 1;
        }
        for (;;) {
            int best = -1;
            for (i = 0; i < s->ntile; i++) {
                const RsTile *t = &s->tile[i];
                const int res = L ? t->scen : (t->road != NULL);
                if (!res || !t->out_ms[L] || (int)(now - t->out_ms[L]) < s->linger_ms) continue;
                if (!s->lru) { best = i; break; }
                if (best < 0 || t->used_ms[L] < s->tile[best].used_ms[L]) best = i;
            }
            if (best < 0) break;
            if (s->lru && total <= budget) break;
            total -= (double)(L ? s->tile[best].scen_bytes : s->tile[best].road_bytes);
            if (L) rs_evict_scen(s, best); else rs_evict_road(s, best);
        }
    }
}

/* ============================================================================
 * SECTION: stats
 * ========================================================================== */

static void rs_counts(RsSession *s)
{
    int i;
    s->st.road_tiles = s->st.scen_tiles = 0;
    s->st.road_mb = s->st.scen_mb = 0.0;
    for (i = 0; i < s->ntile; i++) {
        if (s->tile[i].road) { s->st.road_tiles++; s->st.road_mb += (double)s->tile[i].road_bytes / 1048576.0; }
        if (s->tile[i].scen) { s->st.scen_tiles++; s->st.scen_mb += (double)s->tile[i].scen_bytes / 1048576.0; }
    }
    if (s->warm) {
        if (s->st.road_min < 0 || s->st.road_tiles < s->st.road_min) s->st.road_min = s->st.road_tiles;
        if (s->st.road_tiles > s->st.road_max) s->st.road_max = s->st.road_tiles;
        if (s->st.scen_min < 0 || s->st.scen_tiles < s->st.scen_min) s->st.scen_min = s->st.scen_tiles;
        if (s->st.scen_tiles > s->st.scen_max) s->st.scen_max = s->st.scen_tiles;
    }
}

static double rs_frame_pct(const RsSession *s, double q)
{
    long acc = 0, want;
    int b;
    if (!s->fhist || s->fcount <= 0) return 0.0;
    want = (long)(q * (double)s->fcount);
    for (b = 0; b < RS_FRAME_BINS; b++) {
        acc += (long)s->fhist[b];
        if (acc > want) return (b + 0.5) * 0.1;
    }
    return RS_FRAME_BINS * 0.1;
}

static void rs_stat_line(RsSession *s, uint32_t now)
{
    int pend;
    const double pmb = rs_private_mb();
    const double t = (double)(now - s->t_begin_ms) / 1000.0;
    EnterCriticalSection(&s->cs);
    pend = s->njob;
    LeaveCriticalSection(&s->cs);
    rs_counts(s);
    if (s->csv) {
        fprintf(s->csv, "%.1f,%.1f,%d,%.2f,%d,%.2f,%d,%ld,%ld,%ld,%ld,%ld,%ld,%.2f,%.2f\n", t, pmb,
                s->st.road_tiles, s->st.road_mb, s->st.scen_tiles, s->st.scen_mb, pend, s->st.loaded, s->st.built,
                s->st.stale, s->st.verified, s->st.misses, s->st.holds, s->st.frame_worst_ms, s->st.frame_worst_publish_ms);
        fflush(s->csv);
    }
    if ((int)(now - s->next_log_ms) >= 0) {
        s->next_log_ms = now + 10000u;
        TD5_LOG_I(LOG_TAG, "ROAM_STREAM t=%.0f s private=%.1f MB road=%d tiles (%.2f MB) scenery=%d tiles (%.2f MB) "
                  "pending=%d loaded=%ld built=%ld stale=%ld verified=%ld evicted=%ld/%ld miss=%ld hold=%ld "
                  "frame median=%.1f worst=%.1f worst_publish=%.1f ms install_worst=%.2f tick_worst=%.2f ms | worker max: "
                  "verify %u build %u bake %u ms",
                  t, pmb, s->st.road_tiles, s->st.road_mb, s->st.scen_tiles, s->st.scen_mb, pend, s->st.loaded,
                  s->st.built, s->st.stale, s->st.verified, s->st.evicted_road, s->st.evicted_scen, s->st.misses,
                  s->st.holds, rs_frame_pct(s, 0.5), s->st.frame_worst_ms, s->st.frame_worst_publish_ms,
                  s->st.install_worst_ms, s->st.tick_worst_ms, (unsigned)s->verify_ms_max, (unsigned)s->build_ms_max,
                  (unsigned)s->bake_ms_max);
    }
}

/* ============================================================================
 * SECTION: session
 * ========================================================================== */

static int rs_knob(const char *name, int def, int lo, int hi) { return td5_env_int(name, def, lo, hi); }
#define rs_knob_early rs_knob

int td5_roam_stream_begin(const char *place, const RoamGrid *g, TD5_RoamResident *rs)
{
    RsSession *s;
    int (*list)[2];
    int n, i;
    char path[512];
    if (s_stream.s) td5_roam_stream_end();
    if (!place || !g || !rs) return 0;
    s = (RsSession *)calloc(1, sizeof *s);
    list = (int (*)[2])malloc((size_t)RS_MAX_TILES * sizeof *list);
    if (!s || !list) { free(s); free(list); return 0; }
    snprintf(s->place, sizeof s->place, "%s", place);
    s->grid = *g;
    s->rs = rs;
    s->t_begin_ms = td5_plat_time_ms();
    /* [F9b] TD5RE_ROAM_STREAM_PARTIDO=1 (default): every lattice tile of the place boundary is
     * streamable and gets its table entry the first time something wants it; 0 = the F3 casco list */
    s->partido = rs_knob_early("TD5RE_ROAM_STREAM_PARTIDO", 1, 0, 1);
    if (s->partido) td5_roam_partido_load(place);
    s->nhash = RS_HASH0;
    s->hash = (int32_t *)malloc((size_t)s->nhash * sizeof(int32_t));
    n = td5_roam_casco_tiles(g, list, RS_MAX_TILES);
    s->fhist = (uint32_t *)calloc(RS_FRAME_BINS, sizeof(uint32_t));
    s->hc = (TD5_RoamHeight *)calloc(1, sizeof(TD5_RoamHeight));
    if (!s->hash || !s->fhist || !s->hc) { free(list); free(s->hash); free(s->fhist); free(s->hc); free(s); return 0; }
    for (i = 0; i < s->nhash; i++) s->hash[i] = -1;
    for (i = 0; i < n; i++)
        if (rs_add(s, list[i][0], list[i][1], 0) < 0) { free(list); free(s->hash); free(s->tile); free(s->fhist); free(s->hc); free(s); return 0; }
    s->ncasco = n;
    free(list);

    s->road_r      = rs_knob("TD5RE_ROAM_STREAM_ROAD_R", 1, 1, 4);
    s->view_r      = rs_knob("TD5RE_ROAM_STREAM_VIEW_R", 2, 1, 6);
    s->lead_ms     = rs_knob("TD5RE_ROAM_STREAM_LEAD_MS", 3000, 0, 20000);
    s->linger_ms   = rs_knob("TD5RE_ROAM_STREAM_LINGER_MS", 2000, 0, 60000);
    s->lru         = rs_knob("TD5RE_ROAM_STREAM_LRU", 1, 0, 1);
    s->verify      = rs_knob("TD5RE_ROAM_STREAM_VERIFY", 1, 0, 1);
    s->build       = rs_knob("TD5RE_ROAM_STREAM_BUILD", 1, 0, 1);
    s->worker_on   = rs_knob("TD5RE_ROAM_STREAM_WORKER", 1, 0, 1);
    s->inst_road   = rs_knob("TD5RE_ROAM_STREAM_INSTALL_ROAD", 4, 1, 64);
    s->inst_scen   = rs_knob("TD5RE_ROAM_STREAM_INSTALL_SCEN", 2, 1, 64);
    s->gate_ms     = rs_knob("TD5RE_ROAM_STREAM_GATE_MS", 60000, 1000, 600000);
    s->stat_ms     = rs_knob("TD5RE_ROAM_STREAM_STAT_MS", 1000, 100, 60000);
    s->guard_m     = rs_knob("TD5RE_ROAM_STREAM_GUARD_M", 6, 1, 50);
    s->road_budget = (double)rs_knob("TD5RE_ROAM_STREAM_ROAD_MB", 4, 1, 1024) * 1048576.0;
    s->scen_budget = (double)rs_knob("TD5RE_ROAM_STREAM_SCEN_MB", 16, 1, 4096) * 1048576.0;
    s->st.road_min = s->st.scen_min = -1;

    InitializeCriticalSection(&s->cs);
    s->cs_live = 1;
    s->wake = CreateEventA(NULL, FALSE, FALSE, NULL);
    s->done = CreateEventA(NULL, FALSE, FALSE, NULL);

    /* the render session: configuration fixed now, before the worker bakes against it */
    td5_roam_render_ensure_begun();
    td5_roam_render_set_streamed(1);

    {
        const char *dir = td5_plat_log_dir();
        snprintf(path, sizeof path, "%sroam_stream.csv", dir ? dir : "");
        s->csv = fopen(path, "w");
        if (s->csv) {
            fprintf(s->csv, "t_s,private_mb,road_tiles,road_mb,scen_tiles,scen_mb,pending,loaded,built,stale,verified,"
                            "misses,holds,frame_worst_ms,frame_worst_publish_ms\n");
            fflush(s->csv);
        }
    }
    s_stream.s = s;

    td5_plat_fpenv_capture(&s->fpenv);
    td5_hitch_set_worker_probe(rs_doing);   /* [F3-B DEV] */
    if (s->worker_on && s->wake && s->done) s->thread = CreateThread(NULL, 0, rs_thread_proc, s, 0, NULL);
    /* the worker never competes with the frame: a verify / build is a long CPU burst */
    if (s->thread) SetThreadPriority(s->thread, td5_env_int("TD5RE_ROAM_STREAM_PRIORITY", THREAD_PRIORITY_BELOW_NORMAL, -2, 2));
    if (!s->thread) {
        s->hc_ok = td5_roam_render_open_height(s->hc);
        rs_scn_open(s);
        TD5_LOG_W(LOG_TAG, "ROAM_STREAM: no worker thread (%s): tiles load on the main thread",
                  s->worker_on ? "CreateThread failed" : "TD5RE_ROAM_STREAM_WORKER=0");
    }
    TD5_LOG_I(LOG_TAG, "ROAM_STREAM begin place=%s streamable=%s casco tiles=%d road_r=%d view_r=%d lead=%d ms linger=%d ms "
              "budget road=%.0f MB scenery=%.0f MB lru=%d verify=%d build=%d install/frame road=%d scenery=%d worker=%s",
              place, s->partido ? (td5_roam_partido_has_boundary() ? "place boundary" : "every tile (no boundary)") : "casco",
              n, s->road_r, s->view_r, s->lead_ms, s->linger_ms, s->road_budget / 1048576.0,
              s->scen_budget / 1048576.0, s->lru, s->verify, s->build, s->inst_road, s->inst_scen,
              s->thread ? "thread" : "inline");
    return 1;
}

static void rs_write_summary(RsSession *s)
{
    const char *dir = td5_plat_log_dir();
    char path[512];
    FILE *f;
    rs_counts(s);
    s->st.frame_median_ms = rs_frame_pct(s, 0.5);
    TD5_LOG_I(LOG_TAG, "ROAM_STREAM summary: loaded=%ld built=%ld stale=%ld verified=%ld failed=%ld evicted road=%ld "
              "scenery=%ld | resident road %d..%d scenery %d..%d | miss=%ld hold=%ld sync_loads=%ld | gate %.0f ms | "
              "frames %ld median %.1f p99 %.1f worst %.1f worst_publish %.1f ms, install worst %.2f ms",
              s->st.loaded, s->st.built, s->st.stale, s->st.verified, s->st.failed, s->st.evicted_road,
              s->st.evicted_scen, s->st.road_min, s->st.road_max, s->st.scen_min, s->st.scen_max, s->st.misses,
              s->st.holds, s->st.sync_loads, s->st.gate_ms, s->fcount, s->st.frame_median_ms, rs_frame_pct(s, 0.99),
              s->st.frame_worst_ms, s->st.frame_worst_publish_ms, s->st.install_worst_ms);
    snprintf(path, sizeof path, "%sroam_stream.txt", dir ? dir : "");
    f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "loaded=%ld\nbuilt=%ld\nstale=%ld\nverified=%ld\nfailed=%ld\nevicted_road=%ld\nevicted_scen=%ld\n"
               "road_min=%d\nroad_max=%d\nscen_min=%d\nscen_max=%d\nmisses=%ld\nholds=%ld\nsync_loads=%ld\n"
               "gate_ms=%.0f\nframes=%ld\nframe_median_ms=%.2f\nframe_p99_ms=%.2f\nframe_worst_ms=%.2f\n"
               "frame_worst_publish_ms=%.2f\ninstall_worst_ms=%.3f\ntick_worst_ms=%.3f\n",
            s->st.loaded, s->st.built, s->st.stale, s->st.verified, s->st.failed, s->st.evicted_road,
            s->st.evicted_scen, s->st.road_min, s->st.road_max, s->st.scen_min, s->st.scen_max, s->st.misses,
            s->st.holds, s->st.sync_loads, s->st.gate_ms, s->fcount, s->st.frame_median_ms, rs_frame_pct(s, 0.99),
            s->st.frame_worst_ms, s->st.frame_worst_publish_ms, s->st.install_worst_ms, s->st.tick_worst_ms);
    fclose(f);
}

void td5_roam_stream_end(void)
{
    RsSession *s = s_stream.s;
    int i;
    if (!s) return;
    rs_write_summary(s);
    if (s->thread) {
        InterlockedExchange(&s->cancel, 1);
        SetEvent(s->wake);
        WaitForSingleObject(s->thread, INFINITE);
        CloseHandle(s->thread);
        s->thread = NULL;
    } else {
        if (s->ctx) td5_roam_tile_ctx_close(s->ctx);
        if (s->hc_ok) td5_roam_height_close(s->hc);
        td5_roam_scn_close(s->scn);
        s->ctx = NULL; s->hc_ok = 0; s->scn = NULL;
    }
    /* results nobody installed, then every resident tile */
    for (i = 0; i < s->nres; i++) rs_result_free(&s->res[i]);
    for (i = 0; i < s->ninbox; i++) rs_result_free(&s->inbox[i]);
    for (i = 0; i < s->ntile; i++) {
        if (s->tile[i].road) {
            td5_roam_resident_remove(s->rs, s->tile[i].tx, s->tile[i].tz);
            rs_free_tile(s->tile[i].road);
        }
    }
    td5_roam_render_end();                   /* frees every baked + retired mesh */
    if (s->csv) fclose(s->csv);
    if (s->wake) CloseHandle(s->wake);
    if (s->done) CloseHandle(s->done);
    if (s->cs_live) DeleteCriticalSection(&s->cs);
    free(s->inbox);
    free(s->tile);
    free(s->hash);
    free(s->fhist);
    free(s->hc);
    free(s);
    s_stream.s = NULL;
    TD5_LOG_I(LOG_TAG, "ROAM_STREAM end: worker joined, every streamed tile and mesh freed");
}

void td5_roam_stream_stats(TD5_RoamStreamStats *out)
{
    RsSession *s = s_stream.s;
    if (!out) return;
    memset(out, 0, sizeof *out);
    if (!s) return;
    rs_counts(s);
    s->st.frame_median_ms = rs_frame_pct(s, 0.5);
    *out = s->st;
}

/* ============================================================================
 * SECTION: tick (main, top of td5_game_tick)
 * ========================================================================== */

void td5_roam_stream_tick(void)
{
    RsSession *s = s_stream.s;
    uint64_t now_us, t0, t_req, t0_drain_end, t_ev;
    uint32_t now;
    int applied;
    td5_roam_physics_note_frame();     /* session start time, streamed or not */
    if (!s) {
        /* F2 full load (TD5RE_ROAM_STREAM=0): the same private-bytes samples, for the A/B */
        if (td5_roam_active()) {
            td5_roam_render_retire_tick();   /* the F2 near-eye loader retires tiles too */
            now = td5_plat_time_ms();
            if (!s_stream.f2_t0) { s_stream.f2_t0 = now; s_stream.f2_next = now; }
            if ((int)(now - s_stream.f2_next) >= 0) {
                s_stream.f2_next = now + 1000u;
                TD5_LOG_I(LOG_TAG, "ROAM_MEM t=%.1f private=%.1f MB (full load)",
                          (double)(now - s_stream.f2_t0) / 1000.0, rs_private_mb());
            }
        } else {
            s_stream.f2_t0 = 0;
        }
        return;
    }
    now_us = td5_plat_time_us();
    now = td5_plat_time_ms();
    if (!s->first_tick_done) {
        s->first_tick_done = 1;
        s->warm_ms = now + 2000u;
        s->next_stat_ms = now;
        TD5_LOG_I(LOG_TAG, "ROAM_STREAM first frame %u ms after the session began", (unsigned)(now - s->t_begin_ms));
    } else {
        /* frame time = tick-to-tick interval; a publish frame is one whose previous tick installed */
        const double dt = (double)(now_us - s->last_tick_us) / 1000.0;
        if (s->warm) {
            int b = (int)(dt * 10.0);
            if (b >= RS_FRAME_BINS) b = RS_FRAME_BINS - 1;
            if (b < 0) b = 0;
            s->fhist[b]++;
            s->fcount++;
            if (dt > s->st.frame_worst_ms) s->st.frame_worst_ms = dt;
            if (s->prev_installed && dt > s->st.frame_worst_publish_ms) s->st.frame_worst_publish_ms = dt;
            if (dt > 150.0)
                TD5_LOG_W(LOG_TAG, "ROAM_FRAME_HITCH at %u ms: %.0f ms since the last frame", (unsigned)now, dt);
            td5_roam_rebase_note_frame(dt);      /* [F9b] the frame that held a rebase */
        } else if ((int)(now - s->warm_ms) >= 0) {
            s->warm = 1;
        }
    }
    s->last_tick_us = now_us;

    {
        /* attribution of process memory growth seen between two frames (> 8 MB: a ~4 MB block
         * comes and goes between frames all the time) */
        const double pm = rs_private_mb();
        if (s->last_priv_mb > 0.0 && pm - s->last_priv_mb > 8.0)
            TD5_LOG_I(LOG_TAG, "ROAM_STREAM_MEM frame: private +%.1f MB since the last frame (that tick installed %d, "
                      "evicted %d; resident road %d scenery %d)", pm - s->last_priv_mb, s->last_installed,
                      s->last_evicted, s->st.road_tiles, s->st.scen_tiles);
        s->last_priv_mb = pm;
    }
    td5_roam_render_retire_tick();
    rs_compute_wanted(s);
    rs_update_requests(s);
    if (!s->thread) rs_inline_one(s);
    t_req = td5_plat_time_us();
    t0 = t_req;
    applied = rs_drain(s, s->inst_road, s->inst_scen, 0);
    if (applied) {
        const double ms = (double)(td5_plat_time_us() - t0) / 1000.0;
        if (ms > s->st.install_worst_ms) s->st.install_worst_ms = ms;
    }
    t0_drain_end = td5_plat_time_us();
    s->prev_installed = applied > 0;
    s->last_installed = applied;
    {
        const long ev0 = s->st.evicted_road + s->st.evicted_scen;
        rs_evict(s, now);
        s->last_evicted = (int)(s->st.evicted_road + s->st.evicted_scen - ev0);
    }
    t_ev = td5_plat_time_us();
    if ((int)(now - s->next_stat_ms) >= 0) {
        s->next_stat_ms = now + (uint32_t)s->stat_ms;
        rs_stat_line(s, now);
    }
    {
        /* the stream's own main-thread cost this frame */
        const uint64_t t_end = td5_plat_time_us();
        const double ms = (double)(t_end - now_us) / 1000.0;
        if (s->warm && ms > s->st.tick_worst_ms) s->st.tick_worst_ms = ms;
        if (ms > 20.0)
            TD5_LOG_W(LOG_TAG, "ROAM_STREAM_SLOW_TICK %.1f ms (wanted+requests %.1f, install %.1f, evict %.1f, stats %.1f)",
                      ms, (double)(t_req - now_us) / 1000.0, (double)(t0_drain_end - t_req) / 1000.0,
                      (double)(t_ev - t0_drain_end) / 1000.0, (double)(t_end - t_ev) / 1000.0);
    }
}

/* ============================================================================
 * SECTION: start gate, pin-all, guard
 * ========================================================================== */

static void rs_loadscreen(const char *label)
{
    if (s_stream.loadscreen) s_stream.loadscreen(label);
}

/* Wait until every tile in idx[] is ROAD-resident or failed (installing everything
 * that arrives). Returns the resident count. */
static int rs_wait_tiles(RsSession *s, const int *idx, int n, const char *what)
{
    const uint32_t t0 = td5_plat_time_ms();
    uint32_t next_draw = 0;
    int ready = 0, k;
    char label[96];
    for (;;) {
        if (!s->thread) rs_inline_one(s);
        rs_drain(s, -1, -1, 1);
        ready = 0;
        for (k = 0; k < n; k++) ready += (s->tile[idx[k]].road != NULL || s->tile[idx[k]].failed);
        if (ready >= n) break;
        if ((int)(td5_plat_time_ms() - t0) > s->gate_ms) {
            TD5_LOG_W(LOG_TAG, "ROAM_STREAM %s: gave up after %d ms with %d/%d tiles", what, s->gate_ms, ready, n);
            break;
        }
        if ((int)(td5_plat_time_ms() - next_draw) >= 0) {
            next_draw = td5_plat_time_ms() + 100u;
            snprintf(label, sizeof label, "Streaming streets %d/%d", ready, n);
            rs_loadscreen(label);
        }
        if (s->thread) WaitForSingleObject(s->done, 20);
    }
    ready = 0;
    for (k = 0; k < n; k++) ready += (s->tile[idx[k]].road != NULL);
    return ready;
}

int td5_roam_stream_wait_area(int32_t px, int32_t pz, int ring)
{
    RsSession *s = s_stream.s;
    int ctx, ctz, dx, dz, n = 0, ready, k;
    int idx[81];
    uint32_t t0;
    if (!s) return 0;
    if (ring < 0) ring = s->road_r;
    if (ring > 4) ring = 4;
    t0 = td5_plat_time_ms();
    roam_grid_tile_of(&s->grid, (double)px, (double)pz, &ctx, &ctz);
    for (dz = -ring; dz <= ring; dz++)
        for (dx = -ring; dx <= ring; dx++) {
            const int i = rs_get(s, ctx + dx, ctz + dz);
            if (i < 0) continue;
            s->tile[i].gate = 1;
            s->tile[i].prio = RS_PRIO_GATE + (abs(dx) > abs(dz) ? abs(dx) : abs(dz));
            idx[n++] = i;
        }
    for (k = 0; k < n; k++) {
        RsTile *t = &s->tile[idx[k]];
        const uint8_t need = (uint8_t)(rs_needed(t) & ~t->pend);
        if (need) rs_submit(s, idx[k], RS_JOB_LOAD, need, t->prio);
    }
    rs_loadscreen("Streaming streets");
    ready = rs_wait_tiles(s, idx, n, "start gate");
    for (k = 0; k < n; k++) s->tile[idx[k]].gate = 0;
    s->gate_done = 1;
    s->st.gate_ms = (double)(td5_plat_time_ms() - t0);
    TD5_LOG_I(LOG_TAG, "ROAM_STREAM_GATE spawn tile %d,%d + ring %d: %d/%d road tiles ready, waited %u ms, %u ms after "
              "the session began (the F2 full load read all %d casco tiles here)",
              ctx, ctz, ring, ready, n, (unsigned)(td5_plat_time_ms() - t0), (unsigned)(td5_plat_time_ms() - s->t_begin_ms),
              s->ncasco);
    return ready;
}

/* Read + install one ROAD tile on the main thread now. 1 when resident afterwards. */
static int rs_sync_road(RsSession *s, int idx)
{
    RoamTile *t = NULL;
    char err[96];
    RsTile *tl = &s->tile[idx];
    if (tl->road) return 1;
    if (tl->failed || !rs_read(s, tl->tx, tl->tz, &t, err, sizeof err)) return 0;
    rs_install_road(s, idx, t);
    s->st.sync_loads++;
    s->st.loaded++;
    if (s->verify && tl->road && tl->verify == 0)
        rs_submit(s, idx, RS_JOB_VERIFY, RS_ROAD, RS_PRIO_VERIFY);
    return tl->road != NULL;
}

int td5_roam_stream_pin_all(void)
{
    RsSession *s = s_stream.s;
    int i, n = 0, ready;
    int *idx;
    const uint32_t t0 = td5_plat_time_ms();
    if (!s) return 0;
    idx = (int *)malloc((size_t)(s->ncasco > 0 ? s->ncasco : 1) * sizeof(int));
    if (!idx) return 0;
    for (i = 0; i < s->ncasco; i++) {
        s->tile[i].pinned = 1;
        if (!rs_sync_road(s, i) && !s->tile[i].failed && !(s->tile[i].pend & RS_ROAD))
            rs_submit(s, i, RS_JOB_LOAD, RS_ROAD, 50);
        idx[n++] = i;
    }
    ready = rs_wait_tiles(s, idx, n, "pin all");
    free(idx);
    TD5_LOG_I(LOG_TAG, "ROAM_STREAM pin all: %d/%d casco road tiles resident and pinned in %u ms", ready, s->ncasco,
              (unsigned)(td5_plat_time_ms() - t0));
    return ready;
}

int td5_roam_stream_point_missing(int32_t x, int32_t z)
{
    RsSession *s = s_stream.s;
    int i;
    if (!s || !s->gate_done) return 0;
    {
        int tx, tz;
        roam_grid_tile_of(&s->grid, (double)FP_TRUNC(x) + s->rs->org_x, (double)FP_TRUNC(z) + s->rs->org_z, &tx, &tz);
        i = rs_find(s, tx, tz);
    }
    /* [F9b] a tile with no entry yet, inside the place, is missing too (it will be created and
     * requested by the next wanted pass); one beyond the boundary has nothing to wait for */
    if (i < 0) return 0;
    return !s->tile[i].outside && !s->tile[i].road;
}

int td5_roam_stream_guard(TD5_Actor *a)
{
    RsSession *s = s_stream.s;
    int slot, k, nm = 0, j, hold = 0;
    int miss[5];
    double px, pz, gu;
    long tick;
    static const int8_t off[5][2] = { { 0, 0 }, { 1, 1 }, { 1, -1 }, { -1, 1 }, { -1, -1 } };
    if (!s || !a || !s->gate_done) return 0;    /* before the spawn: the car is not placed yet */
    slot = (int)a->slot_index;
    if (slot < 0 || slot >= RS_SLOTS) slot = 0;
    /* only the racers the stream follows (rs_compute_wanted); any other actor reads flat ground
     * through td5_roam_stream_point_missing and must not pull tiles in or count as a miss */
    if (slot >= rs_racer_count()) return 0;
    tick = (long)g_td5.simulation_tick_counter;
    if (s->guard_valid[slot] && s->guard_tick[slot] == tick) return s->guard_hold[slot];
    px = (double)FP_TRUNC(a->world_pos.x) + s->rs->org_x;
    pz = (double)FP_TRUNC(a->world_pos.z) + s->rs->org_z;
    gu = (double)s->guard_m * RS_UPM;
    for (k = 0; k < 5; k++) {
        const int i = rs_tile_of_place(s, px + off[k][0] * gu, pz + off[k][1] * gu);
        if (i < 0 || s->tile[i].road) continue;
        for (j = 0; j < nm; j++) if (miss[j] == i) break;
        if (j == nm) miss[nm++] = i;
    }
    if (nm > 0) {
        s->st.misses++;
        if (s->st.misses <= RS_MISS_LOG || s->st.misses % 100 == 0)
            TD5_LOG_W(LOG_TAG, "ROAM_STREAM_MISS #%ld slot=%d tick=%ld pos place (%.0f,%.0f): %d tile(s) under the car "
                      "not resident (first %d,%d pend=%d failed=%d)", s->st.misses, slot, tick, px, pz, nm,
                      s->tile[miss[0]].tx, s->tile[miss[0]].tz, (int)s->tile[miss[0]].pend, (int)s->tile[miss[0]].failed);
        for (j = 0; j < nm; j++) {
            if (rs_sync_road(s, miss[j])) continue;
            if (s->tile[miss[j]].failed) continue;            /* nothing will come: the backstop handles it */
            hold = 1;
            if (!(s->tile[miss[j]].pend & RS_ROAD)) rs_submit(s, miss[j], RS_JOB_LOAD, RS_ROAD, RS_PRIO_URGENT);
        }
        if (hold) {
            s->st.holds++;
            if (s->st.holds <= RS_MISS_LOG || s->st.holds % 100 == 0)
                TD5_LOG_W(LOG_TAG, "ROAM_STREAM_HOLD #%ld slot=%d tick=%ld: the car is held until its tile is built",
                          s->st.holds, slot, tick);
        }
    }
    s->guard_valid[slot] = 1;
    s->guard_tick[slot] = tick;
    s->guard_hold[slot] = (uint8_t)hold;
    return hold;
}
