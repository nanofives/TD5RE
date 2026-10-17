/**
 * td5_hitchwatch.c -- DEV-only main-thread stall sampler (PORT-ONLY, roam F3-B).
 * Contract: td5_hitchwatch.h. Off unless TD5RE_HITCH_WATCH_MS > 0.
 */
#ifndef TD5RE_RELEASE

#include <windows.h>
#include <psapi.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "td5_platform.h"
#include "td5_config.h"
#include "td5_hitchwatch.h"

#define HW_MAX_FRAMES 40
#define HW_MAX_PER_STALL 60   /* samples per stall (3 s at 50 ms): a loading screen has no beats */

typedef struct {
    /* written by the main thread, read by the watchdog */
    volatile LONG64       beat_qpc;
    volatile const char  *beat_zone;
    volatile LONG64       beat_cycles;     /* main thread cycle time at the beat      */
    volatile LONG         beat_faults;     /* process page faults at the beat (lazy)   */
    volatile LONG         beat_seq;
    /* fixed after start */
    int                   state;           /* 0 untested, 1 running, -1 off            */
    int                   thresh_ms, every_ms;
    HANDLE                main_thread, watchdog;
    DWORD                 main_tid;
    LARGE_INTEGER         freq;
    volatile TD5_HitchWorkerFn worker_probe;
    volatile LONG         samples;
    FILE                 *out;
} HitchWatch;

static HitchWatch s_hw;

static void hw_module_of(DWORD64 addr, char *name, size_t cap, DWORD64 *base)
{
    HMODULE m = NULL;
    char path[MAX_PATH];
    *base = 0;
    name[0] = '?'; name[1] = 0;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)(uintptr_t)addr, &m) && m) {
        const char *p;
        size_t n;
        *base = (DWORD64)(uintptr_t)m;
        if (GetModuleFileNameA(m, path, sizeof path)) {
            p = strrchr(path, '\\');
            p = p ? p + 1 : path;
            n = strlen(p);
            if (n >= cap) n = cap - 1;
            memcpy(name, p, n);
            name[n] = 0;
        }
    }
}

static DWORD WINAPI hw_thread(LPVOID param)
{
    LONG last_seq = -1;
    LONG64 last_sample_qpc = 0;
    int per_stall = 0;
    (void)param;
    for (;;) {
        LARGE_INTEGER now;
        LONG64 bq;
        LONG seq;
        double since_ms;
        Sleep(5);
        seq = s_hw.beat_seq;
        bq = s_hw.beat_qpc;
        if (!bq) continue;
        QueryPerformanceCounter(&now);
        since_ms = (double)(now.QuadPart - bq) * 1000.0 / (double)s_hw.freq.QuadPart;
        if (since_ms < (double)s_hw.thresh_ms) { last_seq = -1; continue; }
        if (seq == last_seq && per_stall >= HW_MAX_PER_STALL) continue;   /* e.g. a loading screen: no beats */
        if (seq == last_seq &&
            (double)(now.QuadPart - last_sample_qpc) * 1000.0 / (double)s_hw.freq.QuadPart < (double)s_hw.every_ms)
            continue;
        {
            CONTEXT ctx;
            DWORD64 frames[HW_MAX_FRAMES];
            int nf = 0, ok;
            ULONG64 cyc = 0;
            PROCESS_MEMORY_COUNTERS pmc;
            const char *zone = (const char *)s_hw.beat_zone;
            const TD5_HitchWorkerFn wp = s_hw.worker_probe;
            const char *wdoing = wp ? wp() : "-";
            memset(&ctx, 0, sizeof ctx);
            ctx.ContextFlags = CONTEXT_FULL;
            if (SuspendThread(s_hw.main_thread) == (DWORD)-1) continue;
            ok = GetThreadContext(s_hw.main_thread, &ctx);
            if (ok) {
                /* unwind in place: nothing allocated, nothing written but locals */
                CONTEXT c = ctx;
                while (nf < HW_MAX_FRAMES && c.Rip) {
                    DWORD64 img = 0;
                    PRUNTIME_FUNCTION fe;
                    frames[nf++] = c.Rip;
                    fe = RtlLookupFunctionEntry(c.Rip, &img, NULL);
                    if (fe) {
                        PVOID hd = NULL;
                        DWORD64 ef = 0;
                        RtlVirtualUnwind(UNW_FLAG_NHANDLER, img, c.Rip, fe, &c, &hd, &ef, NULL);
                    } else {              /* leaf: return address at [rsp] */
                        c.Rip = *(DWORD64 *)(uintptr_t)c.Rsp;
                        c.Rsp += 8;
                    }
                }
            }
            QueryThreadCycleTime(s_hw.main_thread, &cyc);
            ResumeThread(s_hw.main_thread);
            per_stall = (seq == last_seq) ? per_stall + 1 : 1;
            last_seq = seq;
            last_sample_qpc = now.QuadPart;
            InterlockedIncrement(&s_hw.samples);
            memset(&pmc, 0, sizeof pmc);
            pmc.cb = sizeof pmc;
            GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc);
            if (s_hw.out) {
                int i;
                const LONG64 bc = s_hw.beat_cycles;
                fprintf(s_hw.out, "HITCH sample=%ld seq=%ld zone=%s stalled=%.1f ms main_cycles_since_beat=%.1f M "
                        "faults_since_beat=%ld private=%.1f MB worker=%s\n",
                        (long)s_hw.samples, (long)seq, zone ? zone : "?", since_ms,
                        bc ? (double)((LONG64)cyc - bc) / 1e6 : -1.0,
                        (long)((LONG)pmc.PageFaultCount - s_hw.beat_faults),
                        (double)pmc.PagefileUsage / (1024.0 * 1024.0), wdoing ? wdoing : "-");
                for (i = 0; i < nf; i++) {
                    char mod[64];
                    DWORD64 base;
                    hw_module_of(frames[i], mod, sizeof mod, &base);
                    fprintf(s_hw.out, "  #%02d %s+0x%llx\n", i, mod,
                            (unsigned long long)(frames[i] - base));
                }
                fflush(s_hw.out);
            }
        }
    }
    return 0;
}

static void hw_start(void)
{
    char path[512];
    const char *dir;
    s_hw.thresh_ms = td5_env_int("TD5RE_HITCH_WATCH_MS", 0, 0, 60000);
    if (s_hw.thresh_ms <= 0) { s_hw.state = -1; return; }
    s_hw.every_ms = td5_env_int("TD5RE_HITCH_WATCH_EVERY_MS", 50, 5, 10000);
    QueryPerformanceFrequency(&s_hw.freq);
    s_hw.main_tid = GetCurrentThreadId();
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &s_hw.main_thread,
                         THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0)) {
        s_hw.state = -1;
        return;
    }
    dir = td5_plat_log_dir();
    snprintf(path, sizeof path, "%shitch.txt", dir ? dir : "");
    s_hw.out = fopen(path, "w");
    if (s_hw.out) {
        fprintf(s_hw.out, "HITCHWATCH threshold=%d ms every=%d ms exe_base=0x%llx\n", s_hw.thresh_ms, s_hw.every_ms,
                (unsigned long long)(uintptr_t)GetModuleHandleA(NULL));
        fflush(s_hw.out);
    }
    s_hw.state = 1;
    s_hw.watchdog = CreateThread(NULL, 64 * 1024, hw_thread, NULL, 0, NULL);
    if (s_hw.watchdog) SetThreadPriority(s_hw.watchdog, THREAD_PRIORITY_TIME_CRITICAL);
    TD5_LOG_I("plat", "HITCHWATCH on: threshold %d ms, samples every %d ms -> %shitch.txt", s_hw.thresh_ms,
              s_hw.every_ms, dir ? dir : "");
}

void td5_hitch_beat(const char *zone)
{
    LARGE_INTEGER now;
    ULONG64 cyc = 0;
    if (s_hw.state < 0) return;
    if (s_hw.state == 0) { hw_start(); if (s_hw.state < 0) return; }
    if (GetCurrentThreadId() != s_hw.main_tid) return;
    QueryPerformanceCounter(&now);
    QueryThreadCycleTime(GetCurrentThread(), &cyc);
    {
        PROCESS_MEMORY_COUNTERS pmc;
        pmc.cb = sizeof pmc;
        /* page faults: one cheap counter read per beat */
        if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc)) s_hw.beat_faults = (LONG)pmc.PageFaultCount;
    }
    s_hw.beat_zone = zone;
    s_hw.beat_cycles = (LONG64)cyc;
    s_hw.beat_qpc = now.QuadPart;
    InterlockedIncrement(&s_hw.beat_seq);
}

void td5_hitch_set_worker_probe(TD5_HitchWorkerFn fn) { s_hw.worker_probe = fn; }

long td5_hitch_samples(void) { return (long)s_hw.samples; }

#endif /* !TD5RE_RELEASE */
