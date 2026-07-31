/**
 * td5_rt.c -- game-side ray-traced lighting layer (LIGHTING QUALITY: HIGH).
 * PORT-ONLY. See docs/plans/RT_LIGHTING_PLAN.md and td5_rt.h.
 *
 * Phase 0: capability + activation predicates only. Availability is delegated
 * to the platform bridge (td5_plat_rt_available -> Backend_RTAvailable). The
 * requested quality is env-seeded (TD5RE_RT) until the GRAPHICS OPTIONS row +
 * [Lighting] Quality INI land in Phase 4.
 */
#include "td5_rt.h"
#include "td5_platform.h"
#include "td5_config.h"

/* -1 = unread (seed from env on first query). 0 = LOW, 1 = HIGH. */
static int s_quality_high = -1;

static int rt_quality_seed(void)
{
    if (s_quality_high < 0)
        s_quality_high = td5_env_int("TD5RE_RT", 0, 0, 1);
    return s_quality_high;
}

int td5_rt_available(void)
{
    return td5_plat_rt_available();
}

int td5_rt_quality_high(void)
{
    return rt_quality_seed();
}

void td5_rt_set_quality(int high)
{
    s_quality_high = high ? 1 : 0;
}

int td5_rt_active(void)
{
    if (!td5_rt_available()) return 0;
    if (!rt_quality_seed())  return 0;
    /* "in a race, not frontend/FMV, lighting enabled" is refined in Phase 2b
     * where td5_rt_active() first gates an actual dispatch. Phase 0 has no
     * caller, so availability + quality is a safe, conservative predicate. */
    return 1;
}
