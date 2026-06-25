/**
 * td5_version.h -- TD5RE source-port version identity.
 *
 * Single source of truth for the human version number, the build channel
 * (dev vs release), the build date and (when available) the git short
 * revision. Consumed by the in-game CHANGELOG screen and any other place
 * that needs to print the build identity.
 *
 * Bump TD5RE_VERSION_* BY HAND when cutting a release. The build date is the
 * compiler's __DATE__ (always present). The git short hash is injected by
 * build_standalone.bat as -DTD5RE_GIT_REV="...." when git is available; a bare
 * compile with no -D still works because of the fallback below.
 */
#ifndef TD5_VERSION_H
#define TD5_VERSION_H

#define TD5RE_VERSION_MAJOR 1
#define TD5RE_VERSION_MINOR 0
#define TD5RE_VERSION_PATCH 0
#define TD5RE_VERSION_STR   "1.0.0"

/* Build channel tag (the EXE variant this header was compiled into). */
#ifdef TD5RE_RELEASE
#  define TD5RE_BUILD_CHANNEL "release"
#else
#  define TD5RE_BUILD_CHANNEL "dev"
#endif

/* Git short revision, injected by the build script. Empty when git was not
 * available at build time (or for a hand-run compile). Callers test
 * TD5RE_GIT_REV[0] to decide whether to show it. */
#ifndef TD5RE_GIT_REV
#  define TD5RE_GIT_REV ""
#endif

/* Compiler build date, e.g. "Jun 25 2026". */
#define TD5RE_BUILD_DATE __DATE__

#endif /* TD5_VERSION_H */
