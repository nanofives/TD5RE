/* ========================================================================
 * td5_alloc_log.h — raw-CRT allocation logger (PORT-ONLY, DEV-ONLY diagnostic)
 *
 * Force-included into every td5re translation unit via `-include` in
 * cflags.txt so malloc/calloc/realloc are macro-shimmed to logging wrappers
 * that capture size + __FILE__:__LINE__. Built for the x64 memory hunt:
 * x64 commits ~800 MB it never touches (private bytes 936 MB vs i686's
 * 128 MB at the same frontend screen, working sets near-identical), and the
 * 24 MB game heap (td5_plat_heap_alloc) is already accounted for — the
 * target is raw CRT allocation.
 *
 * Inert unless the TD5RE_ALLOC_LOG environment variable is set to a positive
 * integer N = log threshold in MB (e.g. TD5RE_ALLOC_LOG=8 logs every
 * allocation >= 8 MB). Hits append to log/alloc_<arch>.csv so an i686 run
 * and an x64 run can be diffed directly.
 *
 * The macros are function-like, so declarations (`void *malloc(size_t);`)
 * inside system headers WOULD be mangled if they were included after us —
 * which is why the real <stdlib.h>/<malloc.h> are pulled in first, making
 * every later include of them a guarded no-op. Sites that take malloc's
 * address without calling it (`= malloc;`) don't expand and stay unlogged;
 * that's fine — this is a size tracer, not an accounting layer. free() is
 * deliberately not wrapped: the wrappers return real CRT pointers.
 *
 * Compiled out of RELEASE (macros only; the wrapper functions themselves are
 * always built so the module list stays variant-independent).
 * ======================================================================== */
#ifndef TD5_ALLOC_LOG_H
#define TD5_ALLOC_LOG_H

#include <stdlib.h>
#include <malloc.h>

void *td5_alloc_log_malloc(size_t sz, const char *file, int line);
void *td5_alloc_log_calloc(size_t n, size_t sz, const char *file, int line);
void *td5_alloc_log_realloc(void *p, size_t sz, const char *file, int line);

#ifndef TD5RE_RELEASE
#define malloc(sz)     td5_alloc_log_malloc((sz), __FILE__, __LINE__)
#define calloc(n, sz)  td5_alloc_log_calloc((n), (sz), __FILE__, __LINE__)
#define realloc(p, sz) td5_alloc_log_realloc((p), (sz), __FILE__, __LINE__)
#endif

#endif /* TD5_ALLOC_LOG_H */
