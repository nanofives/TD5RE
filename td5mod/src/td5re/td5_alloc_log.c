/* ========================================================================
 * td5_alloc_log.c — raw-CRT allocation logger. See td5_alloc_log.h.
 * ======================================================================== */
#include "td5_alloc_log.h"

/* This TU is force-included with td5_alloc_log.h like every other, so the
 * macros are already active here — drop them so the wrappers below call the
 * real CRT allocators instead of recursing. */
#undef malloc
#undef calloc
#undef realloc

#include <stdio.h>
#include <direct.h>
#include <windows.h>

/* Threshold in bytes; 0 = disabled. -1 sentinel = env not read yet. Reading
 * the env twice on a first-use race is idempotent, so no lock is needed. */
static long long s_threshold = -1;

static long long alloc_log_threshold(void)
{
    if (s_threshold < 0) {
        const char *e = getenv("TD5RE_ALLOC_LOG");
        int mb = (e && e[0]) ? atoi(e) : 0;
        s_threshold = (mb > 0) ? (long long)mb * 1024 * 1024 : 0;
    }
    return s_threshold;
}

static void alloc_log_emit(const char *kind, size_t sz,
                           const char *file, int line, void *ret)
{
    /* Open-append-close per hit: few hits expected (threshold is MB-scale),
     * and it keeps the log crash-safe and thread-safe enough without a lock. */
    const char *path = (sizeof(void *) == 8) ? "log/alloc_x64.csv"
                                             : "log/alloc_i686.csv";
    FILE *f;
    _mkdir("log");
    f = fopen(path, "a");
    if (!f)
        return;
    fprintf(f, "%s,%llu,%s,%d,%p,pid=%lu\n",
            kind, (unsigned long long)sz, file ? file : "?", line, ret,
            GetCurrentProcessId());
    fclose(f);
}

void *td5_alloc_log_malloc(size_t sz, const char *file, int line)
{
    void *p = malloc(sz);
    if (alloc_log_threshold() && sz >= (size_t)alloc_log_threshold())
        alloc_log_emit("malloc", sz, file, line, p);
    return p;
}

void *td5_alloc_log_calloc(size_t n, size_t sz, const char *file, int line)
{
    void *p = calloc(n, sz);
    size_t total = n * sz; /* diagnostic only; calloc itself checks overflow */
    if (alloc_log_threshold() && total >= (size_t)alloc_log_threshold())
        alloc_log_emit("calloc", total, file, line, p);
    return p;
}

void *td5_alloc_log_realloc(void *old, size_t sz, const char *file, int line)
{
    void *p = realloc(old, sz);
    if (alloc_log_threshold() && sz >= (size_t)alloc_log_threshold())
        alloc_log_emit("realloc", sz, file, line, p);
    return p;
}
