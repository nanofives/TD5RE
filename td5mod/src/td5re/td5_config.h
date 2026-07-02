/* ========================================================================
 * td5_config.h — shared TD5RE_* environment-knob accessors (PORT-ONLY)
 *
 * The source port exposes ~300 tuning/diagnostic knobs via TD5RE_* environment
 * variables — a dev/override layer that runs parallel to td5re.ini and the
 * --Key=N CLI overrides. Historically the parse/clamp/default logic was copy-
 * pasted at every call site (static cache + getenv + atoi + clamp). This module
 * centralises it. Environment variables are fixed for a process run, so callers
 * typically read once into a file-static and cache the result:
 *
 *     static int s_foo = -1;
 *     if (s_foo < 0) s_foo = td5_env_int("TD5RE_FOO", 200, 0, 1000);
 *
 * Helpers return the default when the var is unset or empty. Integer parsing is
 * base-10 (atoi-equivalent — matches the overwhelming majority of call sites and
 * avoids leading-zero octal surprises).
 * ======================================================================== */
#ifndef TD5_CONFIG_H
#define TD5_CONFIG_H

/* Clamped integer knob: unset/empty -> def, else base-10 parse clamped to [lo,hi]. */
int   td5_env_int(const char *name, int def, int lo, int hi);

/* Clamped float knob: unset/empty -> def, else atof clamped to [lo,hi]. */
float td5_env_float(const char *name, float def, float lo, float hi);

/* Like td5_env_int but returns `notset` (choose a sentinel < lo) when the var is
 * absent, so a caller can distinguish "overridden" from "use default/level". */
int   td5_env_int_opt(const char *name, int lo, int hi, int notset);

/* Boolean flag, DEFAULT ON: unset -> 1; a value beginning with '0' -> 0; any
 * other value -> 1. Reproduces the faithful `(e && e[0]=='0') ? 0 : 1` idiom. */
int   td5_env_flag_on(const char *name);

/* Boolean flag, DEFAULT OFF: unset -> 0; a value beginning with '1' -> 1; any
 * other value -> 0. Reproduces the faithful `(e && e[0]=='1') ? 1 : 0` idiom. */
int   td5_env_flag_off(const char *name);

#endif /* TD5_CONFIG_H */
