/* ============================================================================
 * td5_profile.c -- per-phase frame profiler (see td5_profile.h).
 * ========================================================================== */
#include "td5_profile.h"
#include "td5_platform.h"   /* TD5_LOG_* */
#include "td5_hitchwatch.h"   /* [roam F3-B] DEV stall sampler heartbeat */

#include <windows.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

#define PROF_MAX_ZONES 28
#define PROF_NAME_LEN  20

typedef struct {
    char     name[PROF_NAME_LEN];
    uint64_t accum_us;   /* total microseconds this window */
    uint64_t max_us;     /* worst single frame this window */
    uint32_t count;      /* frames this window */
} ProfZone;

static int           s_enabled = 0;
static LARGE_INTEGER s_freq;          /* QPC ticks/sec (0 until first enable) */
static LARGE_INTEGER s_prev;          /* time of the previous mark */
static LARGE_INTEGER s_frame_begin;   /* time of this frame's begin_frame() */
static LARGE_INTEGER s_window_start;
static int           s_started = 0;
static ProfZone      s_zones[PROF_MAX_ZONES];
static int           s_zone_count = 0;
/* Whole-frame total (begin_frame -> end_frame), accumulated EVERY frame and
 * logged once/window as "frame=avg/max fps=N". Unlike the per-sub-tick trace
 * stages (post_physics etc.), this is a true per-FRAME wall time, so it and the
 * derived FPS are the trustworthy ground truth for "is the frame budget blown". */
static uint64_t      s_frame_accum_us = 0;
static uint64_t      s_frame_max_us   = 0;
static uint32_t      s_frame_count    = 0;

void td5_profile_set_enabled(int on)
{
    s_enabled = on ? 1 : 0;
    if (s_enabled && s_freq.QuadPart == 0)
        QueryPerformanceFrequency(&s_freq);
}

int td5_profile_enabled(void) { return s_enabled; }

void td5_profile_begin_frame(void)
{
    td5_hitch_beat("frame_begin");   /* DEV, off unless TD5RE_HITCH_WATCH_MS */
    if (!s_enabled) return;
    QueryPerformanceCounter(&s_prev);
    s_frame_begin = s_prev;   /* frame total spans begin_frame() -> end_frame() */
    if (!s_started) { s_window_start = s_prev; s_started = 1; }
}

static ProfZone *prof_zone(const char *name)
{
    int i;
    for (i = 0; i < s_zone_count; i++)
        if (strcmp(s_zones[i].name, name) == 0) return &s_zones[i];
    if (s_zone_count >= PROF_MAX_ZONES) return NULL;
    {
        ProfZone *z = &s_zones[s_zone_count++];
        strncpy(z->name, name, PROF_NAME_LEN - 1);
        z->name[PROF_NAME_LEN - 1] = '\0';
        z->accum_us = 0; z->max_us = 0; z->count = 0;
        return z;
    }
}

void td5_profile_mark(const char *zone)
{
    LARGE_INTEGER now;
    uint64_t dt_us;
    ProfZone *z;
    td5_hitch_beat(zone);   /* DEV, off unless TD5RE_HITCH_WATCH_MS */
    if (!s_enabled || !zone || s_freq.QuadPart == 0) return;
    QueryPerformanceCounter(&now);
    dt_us = (uint64_t)((now.QuadPart - s_prev.QuadPart) * 1000000LL / s_freq.QuadPart);
    s_prev = now;
    z = prof_zone(zone);
    if (!z) return;
    z->accum_us += dt_us;
    z->count++;
    if (dt_us > z->max_us) z->max_us = dt_us;
}

void td5_profile_end_frame(void)
{
    LARGE_INTEGER now;
    uint64_t win_us;
    char line[512];
    int n = 0, i;
    if (!s_enabled || !s_started || s_freq.QuadPart == 0) return;
    QueryPerformanceCounter(&now);

    /* Accumulate this frame's total wall time EVERY frame (not just on the
     * once/second log), so the avg covers the whole window and the FPS is exact. */
    {
        uint64_t frame_us = (uint64_t)((now.QuadPart - s_frame_begin.QuadPart)
                                       * 1000000LL / s_freq.QuadPart);
        s_frame_accum_us += frame_us;
        s_frame_count++;
        if (frame_us > s_frame_max_us) s_frame_max_us = frame_us;
    }

    win_us = (uint64_t)((now.QuadPart - s_window_start.QuadPart) * 1000000LL / s_freq.QuadPart);
    if (win_us < 1000000) return;   /* log once per ~second */

    n += snprintf(line + n, sizeof(line) - n, "PROFILE (ms avg/max)");
    /* Whole-frame total + derived FPS first — the trustworthy per-frame ground
     * truth (the trace-stage zones below are per-sub-tick and read inflated). */
    if (s_frame_count) {
        double favg = (double)s_frame_accum_us / (double)s_frame_count / 1000.0;
        double fmx  = (double)s_frame_max_us / 1000.0;
        double fps  = (favg > 0.0) ? 1000.0 / favg : 0.0;
        n += snprintf(line + n, sizeof(line) - n, "  frame=%.2f/%.2f fps=%.0f",
                      favg, fmx, fps);
    }
    s_frame_accum_us = 0; s_frame_max_us = 0; s_frame_count = 0;
    for (i = 0; i < s_zone_count && n < (int)sizeof(line) - 40; i++) {
        ProfZone *z = &s_zones[i];
        if (z->count) {   /* skip zones not hit this window (e.g. frontend zones during a race) */
            double avg = (double)z->accum_us / (double)z->count / 1000.0;
            double mx  = (double)z->max_us / 1000.0;
            n += snprintf(line + n, sizeof(line) - n, "  %s=%.1f/%.1f", z->name, avg, mx);
        }
        z->accum_us = 0; z->max_us = 0; z->count = 0;   /* reset the window */
    }
    TD5_LOG_W("plat", "%s", line);
    s_window_start = now;
}

/* ---- [round UA 2026-11-08] frame-time percentiles (see td5_profile.h) ---- */
#include <stdlib.h>
#include "td5_config.h"

static int    s_fs_target = -1;      /* frames to record (0 = off, -1 = unread) */
static int    s_fs_skip, s_fs_quit;
static int    s_fs_seen, s_fs_n, s_fs_done;
static float *s_fs_ms;
static LARGE_INTEGER s_fs_prev, s_fs_freq;
static double s_fs_load_ms = -1.0;   /* process start -> first race frame */

static int fs_cmp(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return (x > y) - (x < y);
}

static float fs_pct(const float *sorted, int n, double p)
{
    int i = (int)(p * (double)(n - 1) + 0.5);
    if (i < 0) i = 0;
    if (i >= n) i = n - 1;
    return sorted[i];
}

static void fs_write(void)
{
    FILE *f;
    float *srt;
    double sum = 0.0, med;
    int i, spikes = 0, over33 = 0, over50 = 0, over100 = 0;
    const char *tag = getenv("TD5RE_FRAMESTATS_TAG");
    if (s_fs_n <= 0) return;
    srt = (float *)malloc(sizeof(float) * (size_t)s_fs_n);
    if (!srt) return;
    memcpy(srt, s_fs_ms, sizeof(float) * (size_t)s_fs_n);
    qsort(srt, (size_t)s_fs_n, sizeof(float), fs_cmp);
    for (i = 0; i < s_fs_n; i++) sum += s_fs_ms[i];
    med = fs_pct(srt, s_fs_n, 0.50);
    for (i = 0; i < s_fs_n; i++) {
        if (s_fs_ms[i] > 2.0 * med) spikes++;
        if (s_fs_ms[i] > 33.33f) over33++;
        if (s_fs_ms[i] > 50.0f)  over50++;
        if (s_fs_ms[i] > 100.0f) over100++;
    }
    f = fopen("log/framestats.txt", "a");
    if (f) {
        fprintf(f, "=== FRAMESTATS tag=%s frames=%d skip=%d ===\n", (tag && tag[0]) ? tag : "-", s_fs_n, s_fs_skip);
        fprintf(f, "cpu_frame_ms mean=%.2f p50=%.2f p90=%.2f p95=%.2f p99=%.2f p99.9=%.2f max=%.2f fps_mean=%.1f fps_p1low=%.1f\n",
                sum / s_fs_n, med, fs_pct(srt, s_fs_n, 0.90), fs_pct(srt, s_fs_n, 0.95),
                fs_pct(srt, s_fs_n, 0.99), fs_pct(srt, s_fs_n, 0.999), srt[s_fs_n - 1],
                1000.0 * s_fs_n / sum, 1000.0 / fs_pct(srt, s_fs_n, 0.99));
        fprintf(f, "spikes(>2x p50)=%d over33ms=%d over50ms=%d over100ms=%d\n", spikes, over33, over50, over100);
        fprintf(f, "load_ms process_start->first_race_frame=%.0f\n", s_fs_load_ms);
        /* the 8 worst frames with their index (when in the run they happened) */
        {
            int worst[8], w, j, k;
            for (w = 0; w < 8; w++) worst[w] = -1;
            for (j = 0; j < s_fs_n; j++) {
                for (w = 0; w < 8; w++) {
                    if (worst[w] < 0 || s_fs_ms[j] > s_fs_ms[worst[w]]) {
                        for (k = 7; k > w; k--) worst[k] = worst[k - 1];
                        worst[w] = j; break;
                    }
                }
            }
            fprintf(f, "worst:");
            for (w = 0; w < 8 && worst[w] >= 0; w++) fprintf(f, " #%d=%.1f", worst[w], s_fs_ms[worst[w]]);
            fputc('\n', f);
        }
        fclose(f);
    }
    TD5_LOG_W("plat", "FRAMESTATS written (%d frames, p50=%.2f p99=%.2f max=%.2f ms)",
              s_fs_n, med, fs_pct(srt, s_fs_n, 0.99), srt[s_fs_n - 1]);
    free(srt);
    td5_plat_gpu_stats_dump("log/framestats.txt");
}

int td5_profile_race_frame(void)
{
    LARGE_INTEGER now;
    if (s_fs_target < 0) {
        s_fs_target = td5_env_int("TD5RE_FRAMESTATS", 0, 0, 200000);
        s_fs_skip   = td5_env_int("TD5RE_FRAMESTATS_SKIP", 120, 0, 100000);
        s_fs_quit   = td5_env_int("TD5RE_FRAMESTATS_QUIT", 1, 0, 1);
        if (s_fs_target > 0) {
            s_fs_ms = (float *)calloc((size_t)s_fs_target, sizeof(float));
            if (!s_fs_ms) s_fs_target = 0;
            QueryPerformanceFrequency(&s_fs_freq);
        }
    }
    if (s_fs_target <= 0 || s_fs_done) return 0;
    QueryPerformanceCounter(&now);
    if (s_fs_seen == 0) {
        /* Load time: process creation -> first race frame (boot + frontend skip
         * + track load + RT warmup), wall clock. */
        FILETIME ct, et, kt, ut, nowft;
        if (GetProcessTimes(GetCurrentProcess(), &ct, &et, &kt, &ut)) {
            ULARGE_INTEGER a, b;
            GetSystemTimeAsFileTime(&nowft);
            a.LowPart = ct.dwLowDateTime; a.HighPart = ct.dwHighDateTime;
            b.LowPart = nowft.dwLowDateTime; b.HighPart = nowft.dwHighDateTime;
            s_fs_load_ms = (double)(b.QuadPart - a.QuadPart) / 10000.0;
        }
    }
    if (s_fs_seen++ > s_fs_skip && s_fs_n < s_fs_target)
        s_fs_ms[s_fs_n++] = (float)((double)(now.QuadPart - s_fs_prev.QuadPart) * 1000.0 / (double)s_fs_freq.QuadPart);
    s_fs_prev = now;
    if (s_fs_n >= s_fs_target) {
        fs_write();
        s_fs_done = 1;
        return s_fs_quit;
    }
    return 0;
}
