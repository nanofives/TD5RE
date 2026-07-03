/**
 * td5_light2.c -- Lighting rework v2 (P0 infrastructure)
 *
 * Holds the master mode knob. The per-pane colored-zone state lives in
 * RenderScratch (td5_render_internal.h: tl_chroma / tl_amb_rgb) because it is
 * render scratch written during the actor pass; this module owns the config
 * surface so later phases (GPU scene CB, sun/sky model, SSR) have a home.
 */
#include <stdlib.h>

#include "td5_light2.h"
#include "td5_platform.h"
#include "td5_config.h"

#define LOG_TAG "render"

static int s_mode = TD5_LIGHT2_ENHANCE;   /* default; main.c pushes INI/CLI */
static int s_env_read = 0;

void td5_light2_set_mode(int mode)
{
    if (mode < TD5_LIGHT2_CLASSIC) mode = TD5_LIGHT2_CLASSIC;
    if (mode > TD5_LIGHT2_ENHANCE) mode = TD5_LIGHT2_ENHANCE;
    s_mode = mode;
    TD5_LOG_I(LOG_TAG, "light2: mode=%d (%s)", s_mode,
              s_mode ? "enhance" : "classic");
}

int td5_light2_mode(void)
{
    /* One-shot env override for A/B without touching the INI:
     * TD5RE_LIGHT2_MODE=0|1 */
    if (!s_env_read) {
        s_env_read = 1;
        const char *e = getenv("TD5RE_LIGHT2_MODE");
        if (e && e[0]) {
            int v = atoi(e);
            if (v >= TD5_LIGHT2_CLASSIC && v <= TD5_LIGHT2_ENHANCE) {
                s_mode = v;
                TD5_LOG_I(LOG_TAG, "light2: env override mode=%d", s_mode);
            }
        }
    }
    return s_mode;
}

int td5_light2_active(void)
{
    return td5_light2_mode() >= TD5_LIGHT2_ENHANCE;
}
