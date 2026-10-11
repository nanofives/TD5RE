/* td5_dev_forkfilter.h -- DEV-ONLY per-fork allow/deny filter (round 1016 H).
 *
 * Lets the fork validation harness (verify/geo_fork_validate.ps1) build a chosen
 * subset of the real forks of a geo route, so ONE fork can be A/B'd in isolation and
 * a verdict list (verify/out/fork_validate.json -> allow/deny) can be replayed.
 *
 *   TD5RE_GEO_FORK_DENY  = "94,210"   do not build the forks whose F (main span) is listed
 *   TD5RE_GEO_FORK_ALLOW = "94,210"   build ONLY those; "-1" = build none
 *
 * Applied to the weighted selection's RESULT, so the surviving forks keep exactly the
 * windows and tapers they have with everything on. Header-only on purpose: no new
 * module, no extern. Compiled to a constant 1 in a RELEASE build. */
#ifndef TD5_DEV_FORKFILTER_H
#define TD5_DEV_FORKFILTER_H

#ifdef TD5RE_RELEASE
static inline int td5_dev_forkfilter_allow(int F) { (void)F; return 1; }
static inline int td5_dev_forkfilter_predeny(int F) { (void)F; return 0; }
#else
#include <stdlib.h>
#include <string.h>

static int td5_dev_forkfilter_has(const char *list, int F)
{
    const char *p = list;
    while (p && *p) {
        char *end;
        long v = strtol(p, &end, 10);
        if (end != p && (int)v == F) return 1;
        p = (*end == ',' || *end == ' ') ? end + 1 : (end == p ? p + 1 : end);
    }
    return 0;
}

static inline int td5_dev_forkfilter_allow(int F)
{
    const char *allow = getenv("TD5RE_GEO_FORK_ALLOW");
    const char *deny  = getenv("TD5RE_GEO_FORK_DENY");
    if (deny  && deny[0]  && td5_dev_forkfilter_has(deny,  F)) return 0;
    if (allow && allow[0] && !td5_dev_forkfilter_has(allow, F)) return 0;
    return 1;
}

/* [1019 V] TD5RE_GEO_FORK_PRE_DENY="707": the candidate with that F is removed BEFORE the weighted selection, so the
 * selector builds the best set WITHOUT it (the DENY/ALLOW above only prune its result). For A/B-ing a fork that a
 * neighbouring candidate would otherwise take the room of (a left corridor beside a right one). */
static inline int td5_dev_forkfilter_predeny(int F)
{
    const char *pd = getenv("TD5RE_GEO_FORK_PRE_DENY");
    return pd && pd[0] && td5_dev_forkfilter_has(pd, F);
}
#endif
#endif /* TD5_DEV_FORKFILTER_H */
