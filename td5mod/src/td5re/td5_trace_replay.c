/*
 * td5_trace_replay.c -- Per-sub-tick state replay / injection harness.
 *
 * See td5_trace_replay.h for high-level docs. File format = same 'TD5R'
 * layout as tools/frida_state_snapshot.js. We dump 6 racer-slot actors +
 * 6 RouteState entries + a 128-byte globals blob per sub-tick.
 *
 * Inject path only overwrites actors + RS — the port's globals (RNG seeds,
 * accumulators, FPS, etc.) live in scattered TD5_GlobalState fields that
 * aren't well-modelled as one contiguous region. The bias cascade we care
 * about reads only actor + RS, so this is sufficient for the steering
 * divergence test. Globals are still dumped for the diff harness.
 */

#include "td5_trace_replay.h"

#include "td5_platform.h"
#include "td5re.h"
#include "td5_types.h"
#include "td5_ai.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOG_TAG "trace_replay"

/* ---- Format (must match frida_state_snapshot.js) ---------------------- */
#define RP_MAGIC          0x52354454u   /* 'TD5R' little-endian */
#define RP_VERSION        ((uint16_t)1)
#define RP_ACTOR_COUNT    ((uint16_t)6)
#define RP_ACTOR_STRIDE   ((uint16_t)0x0388)
#define RP_RS_COUNT       ((uint16_t)6)
#define RP_RS_STRIDE      ((uint16_t)0x011C)
#define RP_GLOBALS_BYTES  ((uint16_t)128)
#define RP_FILE_HEADER    32
#define RP_REC_HEADER     16
#define RP_REC_BYTES      (RP_REC_HEADER + \
                           (RP_ACTOR_COUNT * RP_ACTOR_STRIDE) + \
                           (RP_RS_COUNT * RP_RS_STRIDE) + \
                           RP_GLOBALS_BYTES)
/* = 16 + 5424 + 1704 + 128 = 7272 */

/* ---- Mode enum -------------------------------------------------------- */
enum { RP_MODE_OFF = 0, RP_MODE_DUMP = 1, RP_MODE_INJECT = 2, RP_MODE_BOTH = 3 };

/* ---- Module state ----------------------------------------------------- */
static int       s_mode             = RP_MODE_OFF;
static int       s_start_frame      = 0;
static int       s_end_frame        = 0;       /* 0 = unlimited */
static int       s_max_frames       = 0;       /* 0 = unlimited */
static int       s_sub_tick         = 0;       /* monotonic, increments per step */
static FILE     *s_dump_fp          = NULL;
static uint8_t  *s_orig_frames      = NULL;
static int       s_orig_frame_count = 0;
static int       s_active           = 0;
static int       s_dump_records     = 0;
static char      s_dump_path[256]   = {0};

extern uint8_t  *g_actor_table_base;   /* td5_game.c */

/* ---- Helpers ---------------------------------------------------------- */
static void wle_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)( v        & 0xFF);
    p[1] = (uint8_t)((v >>  8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}
static void wle_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)( v        & 0xFF);
    p[1] = (uint8_t)((v >>  8) & 0xFF);
}
static uint32_t rle_u32(const uint8_t *p) {
    return (uint32_t)p[0] |
           ((uint32_t)p[1] <<  8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}
static uint16_t rle_u16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

/* ---- File I/O --------------------------------------------------------- */

static int load_orig_frames(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        TD5_LOG_E(LOG_TAG, "fopen(%s) for inject failed", path);
        return 0;
    }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (sz < RP_FILE_HEADER) {
        TD5_LOG_E(LOG_TAG, "inject file too small (%ld B)", sz);
        fclose(fp);
        return 0;
    }

    uint8_t hdr[RP_FILE_HEADER];
    if (fread(hdr, 1, RP_FILE_HEADER, fp) != RP_FILE_HEADER) {
        TD5_LOG_E(LOG_TAG, "inject file header read failed");
        fclose(fp);
        return 0;
    }

    uint32_t magic = rle_u32(hdr + 0x00);
    if (magic != RP_MAGIC) {
        TD5_LOG_E(LOG_TAG, "bad magic 0x%08X (expected 0x%08X)", magic, RP_MAGIC);
        fclose(fp);
        return 0;
    }
    uint16_t version = rle_u16(hdr + 0x04);
    uint16_t ac      = rle_u16(hdr + 0x06);
    uint16_t astride = rle_u16(hdr + 0x08);
    uint16_t rsc     = rle_u16(hdr + 0x0A);
    uint16_t rsstr   = rle_u16(hdr + 0x0C);
    if (version != RP_VERSION || ac != RP_ACTOR_COUNT ||
        astride != RP_ACTOR_STRIDE || rsc != RP_RS_COUNT ||
        rsstr != RP_RS_STRIDE) {
        TD5_LOG_E(LOG_TAG, "shape mismatch v=%u ac=%u as=0x%X rsc=%u rs=0x%X",
                  version, ac, astride, rsc, rsstr);
        fclose(fp);
        return 0;
    }

    long body = sz - RP_FILE_HEADER;
    if (body % RP_REC_BYTES) {
        TD5_LOG_W(LOG_TAG, "inject body=%ld not multiple of %d (trailing junk ignored)",
                  body, RP_REC_BYTES);
    }
    long n = body / RP_REC_BYTES;
    if (n <= 0) {
        TD5_LOG_E(LOG_TAG, "no records in inject file");
        fclose(fp);
        return 0;
    }

    s_orig_frames = (uint8_t *)malloc((size_t)(n * RP_REC_BYTES));
    if (!s_orig_frames) {
        TD5_LOG_E(LOG_TAG, "malloc(%ld) failed", (long)(n * RP_REC_BYTES));
        fclose(fp);
        return 0;
    }
    size_t got = fread(s_orig_frames, 1, (size_t)(n * RP_REC_BYTES), fp);
    fclose(fp);
    if (got != (size_t)(n * RP_REC_BYTES)) {
        TD5_LOG_E(LOG_TAG, "inject body read short %zu/%ld", got, n * RP_REC_BYTES);
        free(s_orig_frames);
        s_orig_frames = NULL;
        return 0;
    }
    s_orig_frame_count = (int)n;
    TD5_LOG_I(LOG_TAG, "loaded %d orig frames from %s", s_orig_frame_count, path);
    return 1;
}

static int open_dump(const char *path) {
    s_dump_fp = fopen(path, "wb");
    if (!s_dump_fp) {
        TD5_LOG_E(LOG_TAG, "fopen(%s) for dump failed", path);
        return 0;
    }
    static char s_io_buf[256 * 1024];
    setvbuf(s_dump_fp, s_io_buf, _IOFBF, sizeof(s_io_buf));

    uint8_t hdr[RP_FILE_HEADER];
    memset(hdr, 0, sizeof(hdr));
    wle_u32(hdr + 0x00, RP_MAGIC);
    wle_u16(hdr + 0x04, RP_VERSION);
    wle_u16(hdr + 0x06, RP_ACTOR_COUNT);
    wle_u16(hdr + 0x08, RP_ACTOR_STRIDE);
    wle_u16(hdr + 0x0A, RP_RS_COUNT);
    wle_u16(hdr + 0x0C, RP_RS_STRIDE);
    wle_u16(hdr + 0x0E, RP_GLOBALS_BYTES);
    wle_u32(hdr + 0x10, 0);
    fwrite(hdr, 1, sizeof(hdr), s_dump_fp);
    fflush(s_dump_fp);
    TD5_LOG_I(LOG_TAG, "opened dump %s (rec=%d B)", path, RP_REC_BYTES);
    return 1;
}

/* ---- Globals blob serialization (port -> blob) ------------------------ */
/* Mirrors layout in tools/frida_state_snapshot.js fillGlobalsBlob().
 * We don't have the same per-tick view of every field; assemble from
 * g_td5 + accessible statics. */
static void pack_globals(uint8_t blob[RP_GLOBALS_BYTES]) {
    memset(blob, 0, RP_GLOBALS_BYTES);
    wle_u32(blob + 0x00, (uint32_t)g_td5.game_state);
    wle_u32(blob + 0x04, (uint32_t)g_td5.paused);
    wle_u32(blob + 0x08, (uint32_t)g_td5.race_end_fade_state);
    wle_u32(blob + 0x0C, g_td5.sim_time_accumulator);
    /* Float fields packed by copying raw bits. */
    union { float f; uint32_t u; } fu;
    fu.f = g_td5.sim_tick_budget;       wle_u32(blob + 0x10, fu.u);
    wle_u32(blob + 0x14, (uint32_t)g_td5.simulation_tick_counter);
    fu.f = g_td5.normalized_frame_dt;   wle_u32(blob + 0x18, fu.u);
    fu.f = g_td5.instant_fps;           wle_u32(blob + 0x1C, fu.u);
    wle_u32(blob + 0x20, (uint32_t)g_td5.viewport_count);
    wle_u32(blob + 0x24, (uint32_t)g_td5.split_screen_mode);
    /* RNG seeds + player control + slot tables are not exposed via the
     * trace whole-state path either; leave them zero for now. The diff
     * harness will EXPECT_DIVERGENT-flag these. */
}

/* ---- Step ------------------------------------------------------------- */

void td5_trace_replay_step(void) {
    if (!s_active) return;
    if (!g_actor_table_base) return;

    int32_t *rs_base = td5_ai_get_route_state(0);
    if (!rs_base) return;

    int N = s_sub_tick;

    /* Frame cap on dump file. */
    if (s_max_frames > 0 && s_dump_records >= s_max_frames &&
        (s_mode == RP_MODE_DUMP || s_mode == RP_MODE_BOTH))
    {
        if (s_dump_fp) {
            fflush(s_dump_fp);
            fclose(s_dump_fp);
            s_dump_fp = NULL;
            TD5_LOG_I(LOG_TAG, "dump cap %d reached; closed", s_max_frames);
        }
    }

    /* ---- 1. Dump (before any inject for this tick) ----
     * The "both" mode semantics: at tick N, port has just finished sub-tick
     * N starting from whatever state existed at the END of tick N-1 (which
     * was injected from orig[N-1] if N > start_frame). So dumping NOW
     * captures the per-tick transform result. */
    if ((s_mode == RP_MODE_DUMP || s_mode == RP_MODE_BOTH) && s_dump_fp) {
        uint8_t rec[RP_REC_BYTES];
        wle_u32(rec + 0x00, 0);            /* frame: unused on port side */
        wle_u32(rec + 0x04, (uint32_t)N);
        wle_u32(rec + 0x08, (uint32_t)g_td5.simulation_tick_counter);
        wle_u32(rec + 0x0C, (uint32_t)(g_td5.paused ? 0x3 : 0x0));

        size_t off = RP_REC_HEADER;
        memcpy(rec + off, g_actor_table_base,
               (size_t)RP_ACTOR_COUNT * RP_ACTOR_STRIDE);
        off += (size_t)RP_ACTOR_COUNT * RP_ACTOR_STRIDE;
        memcpy(rec + off, rs_base,
               (size_t)RP_RS_COUNT * RP_RS_STRIDE);
        off += (size_t)RP_RS_COUNT * RP_RS_STRIDE;
        pack_globals(rec + off);

        fwrite(rec, 1, sizeof(rec), s_dump_fp);
        s_dump_records++;
        if ((s_dump_records & 0x3F) == 0) fflush(s_dump_fp);
    }

    /* ---- 2. Inject (sets state for NEXT sub-tick body) ----
     * Port-local pointer fields MUST be preserved across inject -- they
     * point into the port's heap (s_loaded_cardef etc.) and would crash
     * the port if overwritten with the orig binary's addresses. The
     * tools/diff_replay_frames.py EXPECTED_DIVERGENT lists are the source
     * of truth for which offsets to preserve. */
    if ((s_mode == RP_MODE_INJECT || s_mode == RP_MODE_BOTH) &&
        s_orig_frames && N < s_orig_frame_count)
    {
        int in_window =
            (N >= s_start_frame) &&
            (s_end_frame <= 0 || N < s_end_frame);
        if (in_window) {
            const uint8_t *rec = s_orig_frames + (size_t)N * RP_REC_BYTES;
            const uint8_t *actor_src = rec + RP_REC_HEADER;
            const uint8_t *rs_src    = actor_src +
                (size_t)RP_ACTOR_COUNT * RP_ACTOR_STRIDE;

            /* Actor inject: copy 0x388 bytes per slot but preserve the
             * pointer block at +0x1B0..+0x1BF (4 dwords) and the frame
             * counter at +0x338. Both diverge by construction (process-
             * local). */
            for (int s = 0; s < RP_ACTOR_COUNT; s++) {
                uint8_t *dst = g_actor_table_base + s * RP_ACTOR_STRIDE;
                const uint8_t *src = actor_src + s * RP_ACTOR_STRIDE;
                /* Save preserved fields. */
                uint8_t saved_ptrs[16];
                uint8_t saved_frame[4];
                memcpy(saved_ptrs, dst + 0x1B0, 16);
                memcpy(saved_frame, dst + 0x338, 4);
                /* Copy verbatim. */
                memcpy(dst, src, RP_ACTOR_STRIDE);
                /* Restore preserved fields. */
                memcpy(dst + 0x1B0, saved_ptrs, 16);
                memcpy(dst + 0x338, saved_frame, 4);
            }

            /* RS inject: most fields are byte-faithful, but three are
             * pointer-vs-index incompatible between orig and port and MUST
             * be preserved from the port's own state:
             *
             *   RS_ROUTE_TABLE_PTR (index 0x00, byte offset 0x00):
             *     orig stores absolute pointer into LEFT.TRK/RIGHT.TRK heap;
             *     port stores absolute pointer into its own loaded data.
             *
             *   RS_SCRIPT_BASE_PTR (index 0x3A, byte offset 0xE8):
             *     orig stores absolute pointer to one of DAT_00473cc8 /
             *     g_script_program_a..d (binary .rdata); port stores
             *     absolute pointer to its own static arrays.
             *
             *   RS_SCRIPT_IP (index 0x3B, byte offset 0xEC):
             *     orig stores ABSOLUTE POINTER to current opcode word
             *     (advanced by +4 each step). port stores INTEGER INDEX
             *     into the base[] array (advanced by +1 each step). Copying
             *     orig's pointer-form into port's index field makes
             *     base[ip] walk ~5MB past the array into random memory --
             *     hangs td5re.exe with garbage opcode dispatch.
             *     Discovered via Moscow PlayerIsAI=1 hang investigation
             *     2026-05-16. */
            for (int s = 0; s < RP_RS_COUNT; s++) {
                uint8_t *dst = (uint8_t *)(rs_base) + s * RP_RS_STRIDE;
                const uint8_t *src = rs_src + s * RP_RS_STRIDE;
                uint8_t saved_route_ptr[4];
                uint8_t saved_script_base[4];
                uint8_t saved_script_ip[4];
                memcpy(saved_route_ptr,   dst + 0x00, 4);
                memcpy(saved_script_base, dst + 0xE8, 4);
                memcpy(saved_script_ip,   dst + 0xEC, 4);
                memcpy(dst, src, RP_RS_STRIDE);
                memcpy(dst + 0x00, saved_route_ptr,   4);
                memcpy(dst + 0xE8, saved_script_base, 4);
                memcpy(dst + 0xEC, saved_script_ip,   4);
            }
        }
    }

    s_sub_tick++;
}

/* ---- Lifecycle -------------------------------------------------------- */

int td5_trace_replay_init(void) {
    /* Mode is read from g_td5.ini.state_replay_mode (set by main.c from
     * [Trace] StateReplayMode = off|dump|inject|both). Paths come from
     * env vars so they can be overridden per-run without re-encoding the
     * INI file (same pattern as TD5RE_WHOLE_STATE_PATH). */
    s_mode = g_td5.ini.state_replay_mode;
    if (s_mode == RP_MODE_OFF) return 0;

    const char *path = getenv("TD5RE_STATE_REPLAY_PATH");
    if (!path || !path[0]) path = "tools/frida_csv/state_snapshot_original.bin";

    const char *dump_path = getenv("TD5RE_STATE_REPLAY_DUMP_PATH");
    if (!dump_path || !dump_path[0]) dump_path = "log/port_state_snapshot.bin";
    snprintf(s_dump_path, sizeof(s_dump_path), "%s", dump_path);

    s_start_frame = g_td5.ini.state_replay_start_frame;
    s_end_frame   = g_td5.ini.state_replay_end_frame;
    s_max_frames  = g_td5.ini.state_replay_max_frames;
    if (s_max_frames <= 0) s_max_frames = 200;

    /* Inject requires reading orig frames. */
    if (s_mode == RP_MODE_INJECT || s_mode == RP_MODE_BOTH) {
        if (!load_orig_frames(path)) {
            TD5_LOG_E(LOG_TAG, "init: orig frames load failed; harness disabled");
            s_mode = RP_MODE_OFF;
            return 0;
        }
    }
    /* Dump requires writing port snapshot file. */
    if (s_mode == RP_MODE_DUMP || s_mode == RP_MODE_BOTH) {
        if (!open_dump(s_dump_path)) {
            TD5_LOG_E(LOG_TAG, "init: dump open failed; harness disabled");
            if (s_orig_frames) { free(s_orig_frames); s_orig_frames = NULL; }
            s_mode = RP_MODE_OFF;
            return 0;
        }
    }

    s_sub_tick = 0;
    s_dump_records = 0;
    s_active = 1;
    TD5_LOG_I(LOG_TAG,
              "active: mode=%d start=%d end=%d max=%d",
              s_mode, s_start_frame, s_end_frame, s_max_frames);
    return 1;
}

void td5_trace_replay_shutdown(void) {
    if (!s_active) return;
    if (s_dump_fp) {
        fflush(s_dump_fp);
        fclose(s_dump_fp);
        s_dump_fp = NULL;
    }
    if (s_orig_frames) {
        free(s_orig_frames);
        s_orig_frames = NULL;
    }
    TD5_LOG_I(LOG_TAG, "shutdown: dumped=%d frames sub_tick=%d",
              s_dump_records, s_sub_tick);
    s_active = 0;
}

int td5_trace_replay_active(void)  { return s_active; }
int td5_trace_replay_sub_tick(void) { return s_sub_tick; }
