/**
 * td5_platform_win32_fs.c -- Filesystem/memory platform primitives
 * (C9 module split, see REFACTOR_PLAN.md).
 *
 * Extracted verbatim from td5_platform_win32.c's "File I/O", "Human-readable
 * INI config", and "Memory" sections. Pure code motion -- same statements,
 * same order; shared state comes from td5_platform_internal.h.
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

/* ========================================================================
 * File I/O
 * ======================================================================== */

struct TD5_File {
    FILE *fp;
};

TD5_File *td5_plat_file_open(const char *path, const char *mode)
{
    TD5_File *f;
    FILE *fp;

    if (!path || !mode) return NULL;

    fp = fopen(path, mode);
    if (!fp) return NULL;

    f = (TD5_File *)malloc(sizeof(TD5_File));
    if (!f) { fclose(fp); return NULL; }
    f->fp = fp;
    return f;
}

void td5_plat_file_close(TD5_File *f)
{
    if (!f) return;
    if (f->fp) fclose(f->fp);
    free(f);
}

size_t td5_plat_file_read(TD5_File *f, void *buf, size_t size)
{
    if (!f || !f->fp || !buf || size == 0) return 0;
    return fread(buf, 1, size, f->fp);
}

size_t td5_plat_file_write(TD5_File *f, const void *buf, size_t size)
{
    if (!f || !f->fp || !buf || size == 0) return 0;
    return fwrite(buf, 1, size, f->fp);
}

int td5_plat_file_seek(TD5_File *f, int64_t offset, int origin)
{
    int whence;
    if (!f || !f->fp) return -1;

    switch (origin) {
    case 0: whence = SEEK_SET; break;
    case 1: whence = SEEK_CUR; break;
    case 2: whence = SEEK_END; break;
    default: return -1;
    }
    return _fseeki64(f->fp, offset, whence);
}

int64_t td5_plat_file_tell(TD5_File *f)
{
    if (!f || !f->fp) return -1;
    return _ftelli64(f->fp);
}

int64_t td5_plat_file_size(TD5_File *f)
{
    int64_t cur, sz;
    if (!f || !f->fp) return -1;
    cur = _ftelli64(f->fp);
    _fseeki64(f->fp, 0, SEEK_END);
    sz = _ftelli64(f->fp);
    _fseeki64(f->fp, cur, SEEK_SET);
    return sz;
}

int td5_plat_file_exists(const char *path)
{
    DWORD attr;
    if (!path) return 0;
    attr = GetFileAttributesA(path);
    return (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY));
}

int td5_plat_file_delete(const char *path)
{
    if (!path) return -1;
    return DeleteFileA(path) ? 0 : -1;
}

int td5_plat_file_rename(const char *from, const char *to)
{
    if (!from || !to) return -1;
    /* MOVEFILE_REPLACE_EXISTING so a stale .migrated backup doesn't block it. */
    return MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING) ? 0 : -1;
}

/* ========================================================================
 * Human-readable INI config (Config.td5/CupData.td5 retirement)
 * ======================================================================== */

void td5_plat_ini_resolve_path(const char *filename, char *out, size_t out_n)
{
    if (!out || out_n == 0) return;
    out[0] = '\0';
    if (!filename) return;

    DWORD n = GetModuleFileNameA(NULL, out, (DWORD)out_n);
    if (n > 0 && n < out_n) {
        char *sl = out + n;
        while (sl > out && *sl != '\\' && *sl != '/') sl--;
        if (sl > out) {
            sl[1] = '\0';
        } else {
            out[0] = '\0';
        }
        /* Append filename if it still fits. */
        if (strlen(out) + strlen(filename) + 1 <= out_n) {
            strcat(out, filename);
            return;
        }
    }
    /* Fallback: bare filename (cwd-relative). */
    strncpy(out, filename, out_n - 1);
    out[out_n - 1] = '\0';
}

int td5_plat_ini_get_int(const char *file, const char *section,
                         const char *key, int fallback)
{
    if (!file || !section || !key) return fallback;
    return (int)GetPrivateProfileIntA(section, key, fallback, file);
}

int td5_plat_ini_get_str(const char *file, const char *section,
                         const char *key, const char *fallback,
                         char *out, size_t out_n)
{
    if (!out || out_n == 0) return 0;
    if (!file || !section || !key) { out[0] = '\0'; return 0; }
    DWORD n = GetPrivateProfileStringA(section, key,
                                       fallback ? fallback : "",
                                       out, (DWORD)out_n, file);
    return (int)n;
}

void td5_plat_ini_set_int(const char *file, const char *section,
                          const char *key, int value)
{
    char buf[16];
    if (!file || !section || !key) return;
    snprintf(buf, sizeof(buf), "%d", value);
    WritePrivateProfileStringA(section, key, buf, file);
}

void td5_plat_ini_set_str(const char *file, const char *section,
                          const char *key, const char *value)
{
    if (!file || !section || !key) return;
    WritePrivateProfileStringA(section, key, value ? value : "", file);
}

void td5_plat_ini_flush(const char *file)
{
    /* Passing NULL section/key/value flushes the cached profile so the next
     * Get* call re-reads the on-disk file we just rewrote via raw I/O. */
    if (!file) return;
    WritePrivateProfileStringA(NULL, NULL, NULL, file);
}

/* ========================================================================
 * Memory
 * ======================================================================== */

void *td5_plat_alloc_aligned(size_t size, size_t alignment)
{
    return _aligned_malloc(size, alignment);
}

void td5_plat_free_aligned(void *ptr)
{
    if (ptr) _aligned_free(ptr);
}

/* Heap traffic counters for the self-test degradation monitor (see
 * td5_plat_heap_stats). Interlocked: worker-pool jobs may allocate. */
static volatile LONG s_heap_alloc_count = 0;
static volatile LONG s_heap_free_count  = 0;

void *td5_plat_heap_alloc(size_t size)
{
    if (!s_game_heap)
        s_game_heap = HeapCreate(0, 1024 * 1024, 0);
    InterlockedIncrement(&s_heap_alloc_count);
    return HeapAlloc(s_game_heap, HEAP_ZERO_MEMORY, size);
}

void td5_plat_heap_free(void *ptr)
{
    if (ptr && s_game_heap) {
        InterlockedIncrement(&s_heap_free_count);
        HeapFree(s_game_heap, 0, ptr);
    }
}

void td5_plat_heap_stats(unsigned *allocs, unsigned *frees)
{
    if (allocs) *allocs = (unsigned)s_heap_alloc_count;
    if (frees)  *frees  = (unsigned)s_heap_free_count;
}

void td5_plat_set_window_title(const char *title)
{
    if (s_hwnd && title) SetWindowTextA(s_hwnd, title);
}

/* [CONFIRMED @ 0x00430cb0 ResetGameHeap; L5 promotion sweep audit 2026-05-18]
 * ARCH-DIVERGENCE -- two intentional deltas vs orig (both behaviour-neutral):
 *   1) Initial size: 1 MB (port) vs 24 MB (orig). Both heaps are growable on
 *      Win32 so the initial reservation only affects first-N-allocs latency;
 *      td5_plat_heap_alloc never observes a failure on either size. The
 *      sole user-visible signal would be process VA-reservation footprint at
 *      startup, which is irrelevant for the source port's target hardware.
 *   2) First-call gate: orig uses sentinel byte DAT_004aee4c; port uses
 *      `if (s_game_heap)` null-check. Same semantic (one-shot destroy on
 *      reset path; cold start skips the destroy). Header status table at
 *      td5_game.c:95 still reads "DONE (delegates to td5_plat_heap_reset)"
 *      — accurate at the dispatch level; this comment documents the
 *      intra-platform deltas. */
void td5_plat_heap_reset(void)
{
    if (s_game_heap) {
        HeapDestroy(s_game_heap);
        s_game_heap = NULL;
    }
    s_game_heap = HeapCreate(0, 1024 * 1024, 0);
}

