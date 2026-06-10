/* ============================================================================
 * td5_profile.c -- per-phase frame profiler (see td5_profile.h).
 * ========================================================================== */
#include "td5_profile.h"
#include "td5_platform.h"   /* TD5_LOG_* */

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
