/* ========================================================================
 * td5_config.c — shared TD5RE_* environment-knob accessors. See td5_config.h.
 * ======================================================================== */
#include "td5_config.h"

#include <stdlib.h>   /* getenv, strtol, atof */

int td5_env_int(const char *name, int def, int lo, int hi)
{
    const char *e = getenv(name);
    long v;
    if (!e || !e[0]) return def;
    v = strtol(e, NULL, 10);
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    return (int)v;
}

float td5_env_float(const char *name, float def, float lo, float hi)
{
    const char *e = getenv(name);
    float v;
    if (!e || !e[0]) return def;
    v = (float)atof(e);
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    return v;
}

int td5_env_int_opt(const char *name, int lo, int hi, int notset)
{
    const char *e = getenv(name);
    long v;
    if (!e || !e[0]) return notset;
    v = strtol(e, NULL, 10);
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    return (int)v;
}

int td5_env_flag_on(const char *name)
{
    const char *e = getenv(name);
    return (e && e[0] == '0') ? 0 : 1;
}

int td5_env_flag_off(const char *name)
{
    const char *e = getenv(name);
    return (e && e[0] == '1') ? 1 : 0;
}
