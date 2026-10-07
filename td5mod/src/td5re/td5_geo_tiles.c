/* ========================================================================
 * td5_geo_tiles.c -- OUTBOUND-ONLY OSM raster tile client. See td5_geo_tiles.h
 *                    for the policy this module is built to keep.
 *
 * SHAPE. One slot table (64 entries, one per atlas cell) is the whole state.
 * Every slot carries its (z,x,y) key and a state; the main thread and the
 * workers meet only there, under one CRITICAL_SECTION.
 *
 *   EMPTY      -- free cell
 *   QUEUED     -- the screen asked for it; a worker will pick it up
 *   FETCHING   -- a worker owns it (this is what caps concurrency at the
 *                 worker count: a slot is claimed before any socket opens)
 *   ON_DISK    -- bytes are in the cache file, waiting for the main thread
 *   RESIDENT   -- decoded and blitted into the atlas, UVs valid
 *   FAILED     -- cache miss AND the fetch failed; draw a placeholder
 *
 * The workers only ever touch the network and the cache FILE. Decoding stays
 * on the main thread: stb_image's error reporting is a global, and
 * td5_asset_decode_png_rgba32 is not documented thread-safe, so paying ~1 ms
 * for two decodes a frame is cheaper than reasoning about that.
 *
 * WHY A SLOT IS CLAIMED BEFORE THE REQUEST. The alternative -- a request
 * queue separate from the slot table -- lets the same tile be queued twice on
 * two consecutive frames and doubles the requests to OSM for no gain. Keying
 * the queue BY the slot makes a duplicate request unrepresentable.
 * ======================================================================== */

#include "td5_geo_tiles.h"
#include "td5_asset.h"
#include "td5_platform.h"

#include <windows.h>
#include <winhttp.h>
#include <direct.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define LOG_TAG "asset"

#define TILE_CACHE_ROOT "re/assets/geo/_tiles"

/* WinHTTP is wide-char only, so the host and the User-Agent need L"" twins of
 * the narrow constants in the header. Kept here, next to the only code that
 * uses them, with a compile-time check that they have not drifted apart --
 * a silently mismatched UA would be exactly the kind of anonymous traffic the
 * OSM policy forbids. */
#define TILE_HOST_W L"tile.openstreetmap.org"
#define TILE_UA_W   L"TD5RE/1.0 (+https://github.com/nanofives/TD5RE)"
_Static_assert(sizeof(TD5_GEO_TILE_HOST) == sizeof(TILE_HOST_W) / sizeof(wchar_t),
               "TD5_GEO_TILE_HOST and TILE_HOST_W have drifted apart");
_Static_assert(sizeof(TD5_GEO_TILE_UA) == sizeof(TILE_UA_W) / sizeof(wchar_t),
               "TD5_GEO_TILE_UA and TILE_UA_W have drifted apart");

/* A 256x256 PNG tile is a few tens of KB; 2 MB is a generous ceiling that
 * still refuses a server that starts streaming something else at us. */
#define TILE_MAX_BYTES (2 * 1024 * 1024)

enum {
    SLOT_EMPTY = 0,
    SLOT_QUEUED,
    SLOT_FETCHING,
    SLOT_ON_DISK,
    SLOT_RESIDENT,
    SLOT_FAILED
};

typedef struct {
    int      z, x, y;
    int      state;
    unsigned last_use;      /* frame counter, for LRU eviction */
} TileSlot;

static TileSlot           s_slot[TD5_GEO_TILE_SLOTS];
static uint32_t          *s_atlas;                 /* BGRA, DIM*DIM          */
static int                s_atlas_dirty;
static unsigned           s_frame;

static CRITICAL_SECTION   s_lock;
static CONDITION_VARIABLE s_wake;
static int                s_lock_ready;
static volatile LONG      s_running;
static void              *s_worker[TD5_GEO_TILE_WORKERS];
static int                s_open;

/* ------------------------------------------------------------- helpers --- */

static void tile_cache_path(int z, int x, int y, char *out, size_t n)
{
    snprintf(out, n, TILE_CACHE_ROOT "/%d/%d/%d.png", z, x, y);
}

/* Create re/assets/geo/_tiles/<z>/<x>/ one level at a time. CreateDirectoryA
 * does not make intermediate levels, and ERROR_ALREADY_EXISTS is the normal
 * case, not a failure. */
static void tile_make_dirs(int z, int x)
{
    char p[512];
    snprintf(p, sizeof(p), TILE_CACHE_ROOT);                 CreateDirectoryA(p, NULL);
    snprintf(p, sizeof(p), TILE_CACHE_ROOT "/%d", z);        CreateDirectoryA(p, NULL);
    snprintf(p, sizeof(p), TILE_CACHE_ROOT "/%d/%d", z, x);  CreateDirectoryA(p, NULL);
}

/* 1 when a cached tile exists and is younger than the max age. Anything we
 * cannot stat counts as absent, which costs one request and never a stale
 * draw. */
static int tile_cache_is_fresh(const char *path)
{
    struct _stat64 st;
    __time64_t now;
    if (_stat64(path, &st) != 0) return 0;
    if (st.st_size <= 0)         return 0;
    _time64(&now);
    if (now < st.st_mtime)       return 1;   /* clock skew: trust the file */
    return (now - st.st_mtime) < ((__time64_t)TD5_GEO_TILE_MAX_AGE_DAYS * 86400);
}

/* ----------------------------------------------------------- the fetch --- */

/* One tile over HTTPS. Returns 1 on success (bytes written to `path`).
 * Follows the WinHTTP sequence td5_save.c already uses for the celebrity-name
 * API, with three changes the OSM tile policy requires: TLS, an identifying
 * User-Agent, and a status-code check (a 403/429 body is not a PNG, and
 * writing it to the cache would poison the slot for a month). */
static int tile_http_get(int z, int x, int y, const char *path)
{
    HINTERNET hSess = NULL, hConn = NULL, hReq = NULL;
    wchar_t   wpath[128];
    BYTE     *body = NULL;
    DWORD     body_len = 0, status = 0, status_len = sizeof(status);
    int       ok = 0;
    char      tmp[520];   /* path[512] + ".part" + NUL, sized so -Wformat-truncation stays quiet */
    FILE     *f;

    swprintf(wpath, 128, L"/%d/%d/%d.png", z, x, y);

    hSess = WinHttpOpen(L"" /* per-request UA, set below */,
                        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSess) goto done;
    {
        DWORD t = 6000;
        WinHttpSetOption(hSess, WINHTTP_OPTION_CONNECT_TIMEOUT, &t, sizeof t);
        WinHttpSetOption(hSess, WINHTTP_OPTION_SEND_TIMEOUT,    &t, sizeof t);
        WinHttpSetOption(hSess, WINHTTP_OPTION_RECEIVE_TIMEOUT, &t, sizeof t);
    }

    hConn = WinHttpConnect(hSess, TILE_HOST_W,
                           INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConn) goto done;

    hReq = WinHttpOpenRequest(hConn, L"GET", wpath, NULL, WINHTTP_NO_REFERER,
                              WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!hReq) goto done;

    /* The identifying User-Agent. OSM blocks generic or absent ones, and
     * rightly so -- this is the line that makes the traffic attributable. */
    WinHttpAddRequestHeaders(hReq, L"User-Agent: " TILE_UA_W,
                             (DWORD)-1L, WINHTTP_ADDREQ_FLAG_ADD);

    if (!WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(hReq, NULL))
        goto done;

    if (!WinHttpQueryHeaders(hReq,
                             WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_len,
                             WINHTTP_NO_HEADER_INDEX))
        goto done;
    if (status != 200) {
        TD5_LOG_W(LOG_TAG, "geo tiles: %d/%d/%d -> HTTP %lu", z, x, y,
                  (unsigned long)status);
        goto done;
    }

    body = (BYTE *)malloc(TILE_MAX_BYTES);
    if (!body) goto done;
    for (;;) {
        DWORD avail = 0, got = 0;
        if (!WinHttpQueryDataAvailable(hReq, &avail) || avail == 0) break;
        if (body_len + avail > TILE_MAX_BYTES) avail = TILE_MAX_BYTES - body_len;
        if (avail == 0) break;
        if (!WinHttpReadData(hReq, body + body_len, avail, &got) || got == 0) break;
        body_len += got;
    }
    /* A PNG starts with the 8-byte signature; anything else is an error page
     * dressed as a 200 and must not enter the cache. */
    if (body_len < 8 || memcmp(body, "\x89PNG\r\n\x1a\n", 8) != 0) {
        TD5_LOG_W(LOG_TAG, "geo tiles: %d/%d/%d -> %lu bytes, not a PNG",
                  z, x, y, (unsigned long)body_len);
        goto done;
    }

    /* Write to .part then rename, so a half-written file can never be read
     * back as a valid cached tile by this run or the next one. */
    tile_make_dirs(z, x);
    snprintf(tmp, sizeof(tmp), "%s.part", path);
    f = fopen(tmp, "wb");
    if (!f) goto done;
    if (fwrite(body, 1, body_len, f) != body_len) { fclose(f); remove(tmp); goto done; }
    fclose(f);
    remove(path);                /* rename() will not overwrite on Windows */
    ok = (rename(tmp, path) == 0);
    if (!ok) remove(tmp);

done:
    if (body)  free(body);
    if (hReq)  WinHttpCloseHandle(hReq);
    if (hConn) WinHttpCloseHandle(hConn);
    if (hSess) WinHttpCloseHandle(hSess);
    return ok;
}

/* ---------------------------------------------------------- the worker --- */

static void tile_worker(void *arg)
{
    /* No FP-env capture/apply here (the rule at td5_platform.h:146-166): this
     * worker does no float or double math at all -- it moves bytes between a
     * socket and a file. Decoding, which does, stays on the main thread. */
    (void)arg;

    for (;;) {
        int  i, claimed = -1, z = 0, x = 0, y = 0;
        char path[512];

        EnterCriticalSection(&s_lock);
        while (s_running) {
            for (i = 0; i < TD5_GEO_TILE_SLOTS; i++) {
                if (s_slot[i].state == SLOT_QUEUED) { claimed = i; break; }
            }
            if (claimed >= 0) break;
            SleepConditionVariableCS(&s_wake, &s_lock, 200);
        }
        if (claimed < 0) { LeaveCriticalSection(&s_lock); break; }  /* shutting down */
        s_slot[claimed].state = SLOT_FETCHING;
        z = s_slot[claimed].z; x = s_slot[claimed].x; y = s_slot[claimed].y;
        LeaveCriticalSection(&s_lock);

        tile_cache_path(z, x, y, path, sizeof(path));
        if (!tile_cache_is_fresh(path))
            (void)tile_http_get(z, x, y, path);

        EnterCriticalSection(&s_lock);
        /* The slot may have been re-keyed by an evicting lookup while we were
         * off the lock; only publish if it is still ours. */
        if (s_slot[claimed].state == SLOT_FETCHING &&
            s_slot[claimed].z == z && s_slot[claimed].x == x &&
            s_slot[claimed].y == y) {
            s_slot[claimed].state = td5_plat_file_exists(path) ? SLOT_ON_DISK
                                                               : SLOT_FAILED;
        }
        LeaveCriticalSection(&s_lock);
    }
}

/* ------------------------------------------------------------ lifecycle -- */

void td5_geo_tiles_open(void)
{
    int i;
    if (s_open) return;

    if (!s_lock_ready) {
        InitializeCriticalSection(&s_lock);
        InitializeConditionVariable(&s_wake);
        s_lock_ready = 1;
    }
    if (!s_atlas) {
        s_atlas = (uint32_t *)calloc((size_t)TD5_GEO_TILE_ATLAS_DIM *
                                     TD5_GEO_TILE_ATLAS_DIM, 4);
        if (!s_atlas) {
            TD5_LOG_E(LOG_TAG, "geo tiles: atlas allocation failed (%d x %d)",
                      TD5_GEO_TILE_ATLAS_DIM, TD5_GEO_TILE_ATLAS_DIM);
            return;
        }
        memset(s_slot, 0, sizeof(s_slot));
    }
    /* The slot table is NOT reset on a re-open: the atlas still holds the
     * tiles from last time, so keeping their keys is what makes re-entering
     * the screen cost zero requests and zero decodes. Clearing it here would
     * quietly undo the whole point of close() keeping the atlas. */
    s_atlas_dirty = 1;   /* the page may have been reused while we were away */

    /* Workers are created ONCE for the process, not once per screen entry:
     * close() deliberately does not join them (see its comment), so spawning
     * a fresh pair here would leak two threads every time the player opened
     * the map. They park on the condition variable while the screen is shut
     * and cost nothing; `s_open` is what gates whether work can reach them. */
    InterlockedExchange(&s_running, 1);
    for (i = 0; i < TD5_GEO_TILE_WORKERS; i++) {
        if (!s_worker[i])
            s_worker[i] = td5_plat_thread_create(tile_worker, NULL);
    }
    s_open = 1;
    TD5_LOG_I(LOG_TAG, "geo tiles: open (%d worker(s), cache %s, UA %s)",
              TD5_GEO_TILE_WORKERS, TILE_CACHE_ROOT, TD5_GEO_TILE_UA);
}

/* Close must be INSTANT: it runs on the frame the player presses BACK.
 *
 * So it does NOT join the workers. Joining would block the main thread until
 * whatever request is in flight returns -- up to the 6 s WinHTTP timeout, on
 * exactly the path where a tile is least likely to arrive (a dead network),
 * which would freeze the menu for six seconds every time someone backed out
 * of an offline map. Instead the workers stay parked on the condition
 * variable for the rest of the process, and `s_open` gates the only thing
 * that matters: whether a NEW request can be made. td5_geo_tiles_lookup
 * refuses to queue while it is 0, and the queued-but-not-started slots are
 * dropped here, so the most that outlives the screen is the one request
 * already on the wire -- which cannot be recalled in any case.
 */
void td5_geo_tiles_close(void)
{
    int i, dropped = 0;
    if (!s_open) return;

    s_open = 0;
    if (s_lock_ready) {
        EnterCriticalSection(&s_lock);
        for (i = 0; i < TD5_GEO_TILE_SLOTS; i++) {
            if (s_slot[i].state == SLOT_QUEUED) { s_slot[i].state = SLOT_EMPTY; dropped++; }
        }
        LeaveCriticalSection(&s_lock);
    }
    /* The atlas is kept: re-entering the screen then redraws the last view
     * with no request at all, which is the politest thing we can do. The slot
     * table is kept with it so the UVs still match. */
    TD5_LOG_I(LOG_TAG, "geo tiles: closed (no further requests; %d queued tile(s) "
              "dropped)", dropped);
}

/* ------------------------------------------------------------- the pump -- */

static void tile_blit_into_atlas(int slot, const uint32_t *px, int w, int h)
{
    const int col = slot % TD5_GEO_TILE_ATLAS_COLS;
    const int row = slot / TD5_GEO_TILE_ATLAS_COLS;
    const int ox  = col * TD5_GEO_TILE_PX;
    const int oy  = row * TD5_GEO_TILE_PX;
    int yy;

    if (w > TD5_GEO_TILE_PX) w = TD5_GEO_TILE_PX;
    if (h > TD5_GEO_TILE_PX) h = TD5_GEO_TILE_PX;
    for (yy = 0; yy < TD5_GEO_TILE_PX; yy++) {
        uint32_t *dst = &s_atlas[(size_t)(oy + yy) * TD5_GEO_TILE_ATLAS_DIM + ox];
        if (yy < h) {
            memcpy(dst, &px[(size_t)yy * w], (size_t)w * 4);
            if (w < TD5_GEO_TILE_PX)
                memset(dst + w, 0, (size_t)(TD5_GEO_TILE_PX - w) * 4);
        } else {
            memset(dst, 0, TD5_GEO_TILE_PX * 4);
        }
    }
}

void td5_geo_tiles_frame(void)
{
    int decoded = 0, i;

    s_frame++;
    if (!s_atlas || !s_lock_ready) return;

    for (i = 0; i < TD5_GEO_TILE_SLOTS && decoded < TD5_GEO_TILE_DECODES_PER_FRAME; i++) {
        char  path[512];
        void *px = NULL;
        int   w = 0, h = 0, z, x, y;

        EnterCriticalSection(&s_lock);
        if (s_slot[i].state != SLOT_ON_DISK) { LeaveCriticalSection(&s_lock); continue; }
        z = s_slot[i].z; x = s_slot[i].x; y = s_slot[i].y;
        LeaveCriticalSection(&s_lock);

        tile_cache_path(z, x, y, path, sizeof(path));
        if (td5_asset_decode_png_rgba32(path, &px, &w, &h) && px && w > 0 && h > 0) {
            tile_blit_into_atlas(i, (const uint32_t *)px, w, h);
            free(px);
            EnterCriticalSection(&s_lock);
            if (s_slot[i].z == z && s_slot[i].x == x && s_slot[i].y == y)
                s_slot[i].state = SLOT_RESIDENT;
            LeaveCriticalSection(&s_lock);
            s_atlas_dirty = 1;
        } else {
            /* A cached file that will not decode is worse than none: drop it
             * so the next lookup re-fetches instead of failing forever. */
            if (px) free(px);
            remove(path);
            EnterCriticalSection(&s_lock);
            if (s_slot[i].z == z && s_slot[i].x == x && s_slot[i].y == y)
                s_slot[i].state = SLOT_FAILED;
            LeaveCriticalSection(&s_lock);
        }
        decoded++;
    }

    if (s_atlas_dirty) {
        if (td5_plat_render_upload_texture(TD5_GEO_TILE_ATLAS_PAGE, s_atlas,
                                           TD5_GEO_TILE_ATLAS_DIM,
                                           TD5_GEO_TILE_ATLAS_DIM, 2)) {
            td5_plat_render_flush_uploads();   /* D3D12: sampling the same frame reads black otherwise */
            s_atlas_dirty = 0;
        }
    }
}

/* ---------------------------------------------------------- the lookup --- */

int td5_geo_tiles_lookup(int z, int x, int y,
                         float *u0, float *v0, float *u1, float *v1)
{
    int i, free_slot = -1, lru = -1;
    unsigned lru_age = 0;
    int resident = 0;

    if (!s_atlas || !s_lock_ready) return 0;

    EnterCriticalSection(&s_lock);

    for (i = 0; i < TD5_GEO_TILE_SLOTS; i++) {
        if (s_slot[i].state != SLOT_EMPTY &&
            s_slot[i].z == z && s_slot[i].x == x && s_slot[i].y == y) {
            s_slot[i].last_use = s_frame;
            if (s_slot[i].state == SLOT_RESIDENT) {
                /* Half-texel inset: the sampler is WRAP, not CLAMP, so UVs
                 * exactly on a cell edge bleed the neighbouring tile in. */
                const float inv = 1.0f / (float)TD5_GEO_TILE_ATLAS_DIM;
                const int   col = i % TD5_GEO_TILE_ATLAS_COLS;
                const int   row = i / TD5_GEO_TILE_ATLAS_COLS;
                *u0 = ((float)(col * TD5_GEO_TILE_PX) + 0.5f) * inv;
                *v0 = ((float)(row * TD5_GEO_TILE_PX) + 0.5f) * inv;
                *u1 = ((float)((col + 1) * TD5_GEO_TILE_PX) - 0.5f) * inv;
                *v1 = ((float)((row + 1) * TD5_GEO_TILE_PX) - 0.5f) * inv;
                resident = 1;
            }
            LeaveCriticalSection(&s_lock);
            return resident;
        }
    }

    /* Not in the table. Claim a cell: a free one, else the least recently
     * drawn RESIDENT/FAILED one. A QUEUED or FETCHING cell is never evicted --
     * that would orphan a worker's request. */
    for (i = 0; i < TD5_GEO_TILE_SLOTS; i++) {
        if (s_slot[i].state == SLOT_EMPTY) { free_slot = i; break; }
        if (s_slot[i].state == SLOT_RESIDENT || s_slot[i].state == SLOT_FAILED) {
            const unsigned age = s_frame - s_slot[i].last_use;
            if (lru < 0 || age > lru_age) { lru = i; lru_age = age; }
        }
    }
    if (free_slot < 0) free_slot = lru;

    /* Every cell is busy fetching: do nothing this frame rather than grow the
     * queue. The screen draws a placeholder and asks again next frame. */
    if (free_slot >= 0 && s_open) {
        s_slot[free_slot].z        = z;
        s_slot[free_slot].x        = x;
        s_slot[free_slot].y        = y;
        s_slot[free_slot].state    = SLOT_QUEUED;
        s_slot[free_slot].last_use = s_frame;
        WakeConditionVariable(&s_wake);
    }
    LeaveCriticalSection(&s_lock);
    return 0;
}

int td5_geo_tiles_pending(void)
{
    int i, n = 0;
    if (!s_lock_ready) return 0;
    EnterCriticalSection(&s_lock);
    for (i = 0; i < TD5_GEO_TILE_SLOTS; i++)
        if (s_slot[i].state == SLOT_QUEUED || s_slot[i].state == SLOT_FETCHING ||
            s_slot[i].state == SLOT_ON_DISK)
            n++;
    LeaveCriticalSection(&s_lock);
    return n;
}

int td5_geo_tiles_failed(void)
{
    int i, n = 0;
    if (!s_lock_ready) return 0;
    EnterCriticalSection(&s_lock);
    for (i = 0; i < TD5_GEO_TILE_SLOTS; i++)
        if (s_slot[i].state == SLOT_FAILED) n++;
    LeaveCriticalSection(&s_lock);
    return n;
}
