/**
 * td5_platform_win32_log.c -- Threading + multi-file logging platform primitives
 * (C9 module split, second slice, see REFACTOR_PLAN.md).
 *
 * Extracted verbatim from td5_platform_win32.c's "Threading" and "Logging"
 * sections. Pure code motion -- same statements, same order; s_hwnd (used by
 * td5_plat_fatal's MessageBoxA) comes from td5_platform_internal.h.
 */

#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "td5_platform.h"
#include "td5_platform_internal.h"
#include "td5_config.h"

#define LOG_TAG "platform"
#include "td5_color.h"

/* Multi-file logging — messages routed by module tag to separate log files.
 * Session rotation keeps up to 5 previous logs per category. */
#define TD5_LOG_CAT_FRONTEND 0
#define TD5_LOG_CAT_RACE     1
#define TD5_LOG_CAT_ENGINE   2
#define TD5_LOG_CAT_COUNT    3
#define TD5_LOG_MAX_SESSIONS 5

static const char *s_log_cat_names[TD5_LOG_CAT_COUNT] = {
    "frontend", "race", "engine"
};

typedef struct TD5_LogFile {
    HANDLE handle;
    char   buffer[4096];
    size_t buffer_used;
    unsigned int line_count;
} TD5_LogFile;

static TD5_LogFile s_log_files[TD5_LOG_CAT_COUNT];
static char s_log_dir[512];
static int  s_log_initialized = 0;

/* Runtime log filters (set via td5_plat_log_set_filters from main.c after INI
 * parse). Defaults match the historic behavior: master on, INFO and above,
 * all three categories enabled. */
static int          s_log_enabled   = 1;
static int          s_log_min_level = TD5_LOG_INFO;
static unsigned int s_log_cat_mask  = TD5_LOG_CAT_BIT_FRONTEND |
                                      TD5_LOG_CAT_BIT_RACE     |
                                      TD5_LOG_CAT_BIT_ENGINE;

/* ========================================================================
 * Threading
 * ======================================================================== */

typedef struct TD5_ThreadCtx {
    void (*func)(void *);
    void *arg;
} TD5_ThreadCtx;

static DWORD WINAPI thread_trampoline(LPVOID param)
{
    TD5_ThreadCtx *tc = (TD5_ThreadCtx *)param;
    tc->func(tc->arg);
    free(tc);
    return 0;
}

void *td5_plat_thread_create(void (*func)(void *), void *arg)
{
    TD5_ThreadCtx *tc;
    HANDLE h;

    if (!func) return NULL;
    tc = (TD5_ThreadCtx *)malloc(sizeof(TD5_ThreadCtx));
    if (!tc) return NULL;
    tc->func = func;
    tc->arg  = arg;

    h = CreateThread(NULL, 0, thread_trampoline, tc, 0, NULL);
    if (!h) { free(tc); return NULL; }
    return (void *)h;
}

int td5_plat_thread_join(void *thread_handle)
{
    HANDLE h = (HANDLE)thread_handle;
    if (!h) return -1;
    WaitForSingleObject(h, INFINITE);
    CloseHandle(h);
    return 0;
}

void *td5_plat_event_create(void)
{
    HANDLE h = CreateEventA(NULL, FALSE, FALSE, NULL);
    return (void *)h;
}

void td5_plat_event_set(void *event_handle)
{
    if (event_handle)
        SetEvent((HANDLE)event_handle);
}

int td5_plat_event_wait(void *event_handle, uint32_t timeout_ms)
{
    DWORD result;
    if (!event_handle) return -1;
    result = WaitForSingleObject((HANDLE)event_handle,
        timeout_ms == 0 ? INFINITE : (DWORD)timeout_ms);
    return (result == WAIT_OBJECT_0) ? 0 : -1;
}

void td5_plat_event_destroy(void *event_handle)
{
    if (event_handle)
        CloseHandle((HANDLE)event_handle);
}

void *td5_plat_mutex_create(void)
{
    CRITICAL_SECTION *cs = (CRITICAL_SECTION *)malloc(sizeof(CRITICAL_SECTION));
    if (!cs) return NULL;
    InitializeCriticalSection(cs);
    return (void *)cs;
}

void td5_plat_mutex_lock(void *mutex_handle)
{
    if (mutex_handle)
        EnterCriticalSection((CRITICAL_SECTION *)mutex_handle);
}

void td5_plat_mutex_unlock(void *mutex_handle)
{
    if (mutex_handle)
        LeaveCriticalSection((CRITICAL_SECTION *)mutex_handle);
}

void td5_plat_mutex_destroy(void *mutex_handle)
{
    if (mutex_handle) {
        DeleteCriticalSection((CRITICAL_SECTION *)mutex_handle);
        free(mutex_handle);
    }
}

/* ========================================================================
 * Logging — multi-file with session rotation
 *
 * Messages are routed to frontend.log / race.log / engine.log based on
 * the module tag.  On startup, existing logs are rotated up to 5 previous
 * sessions (e.g. frontend.log → frontend.1.log → ... → frontend.5.log).
 * ======================================================================== */

static const char *s_log_level_names[] = { "DBG", "INF", "WRN", "ERR" };

static int td5_log_classify_module(const char *module)
{
    if (!module) return TD5_LOG_CAT_ENGINE;

    /* Frontend category */
    if (strcmp(module, "frontend") == 0 ||
        strcmp(module, "hud")      == 0 ||
        strcmp(module, "save")     == 0 ||
        strcmp(module, "input")    == 0)
        return TD5_LOG_CAT_FRONTEND;

    /* Race category */
    if (strcmp(module, "td5_game") == 0 ||
        strcmp(module, "physics")  == 0 ||
        strcmp(module, "ai")       == 0 ||
        strcmp(module, "track")    == 0 ||
        strcmp(module, "camera")   == 0 ||
        strcmp(module, "vfx")      == 0 ||
        strcmp(module, "laneassist") == 0)
        return TD5_LOG_CAT_RACE;

    /* Everything else: render, asset, platform, sound, net, fmv, main, ... */
    return TD5_LOG_CAT_ENGINE;
}

static void td5_log_flush_file(TD5_LogFile *lf)
{
    DWORD written;
    if (lf->handle == INVALID_HANDLE_VALUE || lf->buffer_used == 0) return;
    WriteFile(lf->handle, lf->buffer, (DWORD)lf->buffer_used, &written, NULL);
    lf->buffer_used = 0;
}

static void td5_plat_log_shutdown(void)
{
    int i;
    for (i = 0; i < TD5_LOG_CAT_COUNT; i++) {
        if (s_log_files[i].handle != INVALID_HANDLE_VALUE) {
            td5_log_flush_file(&s_log_files[i]);
            FlushFileBuffers(s_log_files[i].handle);
            CloseHandle(s_log_files[i].handle);
            s_log_files[i].handle = INVALID_HANDLE_VALUE;
        }
    }
}

static void td5_log_rotate_file(const char *base_path)
{
    char old_path[600], new_path[600];
    int i;

    /* Delete the oldest (generation MAX_SESSIONS) */
    snprintf(old_path, sizeof(old_path), "%s.%d.log", base_path, TD5_LOG_MAX_SESSIONS);
    DeleteFileA(old_path);

    /* Shift generations up: N-1 → N, N-2 → N-1, ... 1 → 2 */
    for (i = TD5_LOG_MAX_SESSIONS - 1; i >= 1; i--) {
        snprintf(old_path, sizeof(old_path), "%s.%d.log", base_path, i);
        snprintf(new_path, sizeof(new_path), "%s.%d.log", base_path, i + 1);
        MoveFileA(old_path, new_path);
    }

    /* Current .log → .1.log */
    snprintf(old_path, sizeof(old_path), "%s.log", base_path);
    snprintf(new_path, sizeof(new_path), "%s.1.log", base_path);
    MoveFileA(old_path, new_path);
}

void td5_plat_log_init(void)
{
    int i;
    if (s_log_initialized) return;

    /* Determine log directory: <exe_dir>/log/ */
    {
        DWORD n = GetModuleFileNameA(NULL, s_log_dir, sizeof(s_log_dir) - 32);
        if (n > 0) {
            char *slash = s_log_dir + n;
            while (slash > s_log_dir && *slash != '\\' && *slash != '/') slash--;
            if (slash > s_log_dir) slash[1] = '\0';
        } else {
            strcpy(s_log_dir, ".\\");
        }
        strcat(s_log_dir, "log\\");
    }

    /* Create directory (ignore error if already exists) */
    CreateDirectoryA(s_log_dir, NULL);

    /* Rotate each category's log files */
    for (i = 0; i < TD5_LOG_CAT_COUNT; i++) {
        char base[600];
        char path[600];
        DWORD written;
        const char *marker = "[session start]\n";

        snprintf(base, sizeof(base), "%s%s", s_log_dir, s_log_cat_names[i]);
        td5_log_rotate_file(base);

        /* Open fresh log file */
        snprintf(path, sizeof(path), "%s.log", base);
        s_log_files[i].handle = CreateFileA(path, FILE_APPEND_DATA,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        s_log_files[i].buffer_used = 0;
        s_log_files[i].line_count = 0;

        if (s_log_files[i].handle != INVALID_HANDLE_VALUE) {
            WriteFile(s_log_files[i].handle, marker, (DWORD)strlen(marker), &written, NULL);
        }
    }

    /* Also rotate crash.log */
    {
        char base[600];
        snprintf(base, sizeof(base), "%scrash", s_log_dir);
        td5_log_rotate_file(base);
    }

    atexit(td5_plat_log_shutdown);
    s_log_initialized = 1;
}

const char *td5_plat_log_dir(void)
{
    return s_log_dir;
}

void td5_plat_log_set_filters(int enabled, int min_level, unsigned int cat_mask)
{
    s_log_enabled   = enabled ? 1 : 0;
    s_log_min_level = min_level;
    s_log_cat_mask  = cat_mask;
}

/* WARN/ERROR call totals — counted at the call site regardless of sink
 * filters, so the self-test suite sees warnings even with logging gated.
 * Interlocked: worker-pool jobs log too. */
static volatile LONG s_log_warn_total = 0;
static volatile LONG s_log_err_total  = 0;

void td5_plat_log_counts(unsigned *warns, unsigned *errors)
{
    if (warns)  *warns  = (unsigned)s_log_warn_total;
    if (errors) *errors = (unsigned)s_log_err_total;
}

void td5_plat_log(TD5_LogLevel level, const char *module, const char *fmt, ...)
{
    char buf[2048];
    va_list ap;
    int off, cat;
    DWORD written;
    TD5_LogFile *lf;

    if (!fmt) return;

    if (level == TD5_LOG_WARN)       InterlockedIncrement(&s_log_warn_total);
    else if (level == TD5_LOG_ERROR) InterlockedIncrement(&s_log_err_total);

    /* Fast-path gating — bail before any formatting cost.
     *  - Errors are NEVER suppressed (still go to stdout below); they're rare
     *    and we don't want a perf-A/B run to silently swallow crashes.
     *  - Master switch off → drop everything except errors.
     *  - Below configured min_level → drop. */
    if (level < TD5_LOG_ERROR) {
        if (!s_log_enabled) return;
        if ((int)level < s_log_min_level) return;
        cat = td5_log_classify_module(module);
        if (!(s_log_cat_mask & (1u << cat))) return;
    }

    off = snprintf(buf, sizeof(buf), "[%s][%s] ",
        (level >= 0 && level <= 3) ? s_log_level_names[level] : "???",
        module ? module : "td5");

    va_start(ap, fmt);
    vsnprintf(buf + off, sizeof(buf) - off, fmt, ap);
    va_end(ap);

    /* Always newline-terminate */
    {
        size_t len = strlen(buf);
        if (len > 0 && len < sizeof(buf) - 2 && buf[len-1] != '\n') {
            buf[len] = '\n';
            buf[len+1] = '\0';
        }
    }

    /* Lazy init if td5_plat_log_init() was not called yet */
    if (!s_log_initialized) td5_plat_log_init();

    cat = td5_log_classify_module(module);
    lf = &s_log_files[cat];

    if (lf->handle != INVALID_HANDLE_VALUE) {
        size_t len = strlen(buf);
        if (len > sizeof(lf->buffer)) {
            td5_log_flush_file(lf);
            WriteFile(lf->handle, buf, (DWORD)len, &written, NULL);
        } else {
            if (lf->buffer_used + len > sizeof(lf->buffer)) {
                td5_log_flush_file(lf);
            }
            memcpy(lf->buffer + lf->buffer_used, buf, len);
            lf->buffer_used += len;
        }
        lf->line_count++;
        /* Flush to disk periodically — only on ERROR or every 256 messages.
         * Flushing on WARN caused thousands of synchronous WriteFile calls per
         * frame during rendering (invalid mesh opcodes, wall impulse logs),
         * which was the root cause of the 5-10 second frame freeze in race mode. */
        if (level >= TD5_LOG_ERROR || (lf->line_count % 256) == 0) {
            td5_log_flush_file(lf);
        }
    }

    /* Echo WARN and ERROR to the console (visible even when running from IDE) */
    if (level >= TD5_LOG_WARN) {
        HANDLE hcon = GetStdHandle(STD_OUTPUT_HANDLE);
        if (hcon && hcon != INVALID_HANDLE_VALUE) {
            DWORD written2;
            WriteFile(hcon, buf, (DWORD)strlen(buf), &written2, NULL);
        }
    }
}

void td5_plat_log_flush(void)
{
    int i;
    for (i = 0; i < TD5_LOG_CAT_COUNT; i++) {
        if (s_log_files[i].handle != INVALID_HANDLE_VALUE) {
            td5_log_flush_file(&s_log_files[i]);
            FlushFileBuffers(s_log_files[i].handle);
        }
    }
}

/* td5_plat_fatal is not declared in the header but is part of the platform
 * contract for unrecoverable errors. */
void td5_plat_fatal(const char *msg)
{
    if (msg) {
        OutputDebugStringA(msg);
        MessageBoxA(s_hwnd, msg, "TD5RE Fatal Error",
                    MB_OK | MB_ICONERROR);
    }
    ExitProcess(1);
}
