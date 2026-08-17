/* ========================================================================
 * td5_ai_driver.c -- closed-loop "Driver Model" AI (PORT-ONLY)
 *
 * See td5_ai_driver.h for the design overview and the mode ladder.
 *
 * P0 SCOPE (this commit): scaffolding + seam only.
 *   - mode resolution (INI + env override) and race-level gate
 *   - per-race init hook (currently a deterministic no-op placeholder)
 *   - per-tick entry that FORWARDS to SmartAI (returns "not handled") so the
 *     existing behaviour is byte-for-byte unchanged in DRIVER mode
 *   - target-speed accessor (returns 0; no profile yet)
 *
 * The path table, quasi-steady-state speed profile, pure-pursuit + PI
 * controller, racecraft and personalities land in P1..P4 behind this seam.
 * ======================================================================== */

#include "td5_ai_driver.h"

#include "td5_types.h"
#include "td5re.h"          /* g_td5 (ini.ai_model, drag/wanted flags) */
#include "td5_platform.h"   /* TD5_LOG_* */
#include "td5_config.h"     /* td5_env_int_opt (TD5RE_AI_MODEL override) */
#include "td5_race_state.h" /* td5_game_get_slot_state */

#include <stdlib.h>

#define LOG_TAG "ai"

/* ------------------------------------------------------------------------
 * Mode resolution
 * ---------------------------------------------------------------------- */

int td5_ai_driver_mode(void)
{
    int mode = g_td5.ini.ai_model;

    /* TD5RE_AI_MODEL env override (0/1/2) for quick A/B without touching the
     * INI. Sentinel -1 = "not set" so an absent var keeps the INI value. */
    int ov = td5_env_int_opt("TD5RE_AI_MODEL", 0, TD5_AI_MODE_COUNT - 1, -1);
    if (ov >= 0) mode = ov;

    if (mode < 0) mode = 0;
    if (mode >= TD5_AI_MODE_COUNT) mode = TD5_AI_MODE_COUNT - 1;
    return mode;
}

const char *td5_ai_driver_mode_name(int mode)
{
    switch (mode) {
    case TD5_AI_MODE_CLASSIC: return "CLASSIC";
    case TD5_AI_MODE_SMART:   return "SMART";
    case TD5_AI_MODE_DRIVER:  return "DRIVER";
    default:                  return "?";
    }
}

int td5_ai_driver_active(void)
{
    /* DRIVER mode owns racer AI, except in the faithful-only modes (drag,
     * wanted/cop-chase) which keep their dedicated drivers. Same shape as
     * td5_ai_smart_active(). */
    return td5_ai_driver_mode() == TD5_AI_MODE_DRIVER
        && !g_td5.drag_race_enabled
        && !g_td5.wanted_mode_enabled;
}

int td5_ai_driver_owns_slot(int slot)
{
    if (slot < 0 || slot >= TD5_MAX_RACER_SLOTS) return 0;
    if (!td5_ai_driver_active()) return 0;
    /* Only AI-driven racer slots (state 0). Human (1) and finished/dead (2)
     * slots are not driven by the model. */
    return td5_game_get_slot_state(slot) == 0;
}

/* ------------------------------------------------------------------------
 * Per-race init
 * ---------------------------------------------------------------------- */

void td5_ai_driver_race_init(void)
{
    if (td5_ai_driver_mode() != TD5_AI_MODE_DRIVER) return;
    /* P1+: build the per-route path table and per-car speed profile here, and
     * seed per-slot driver/personality state from td5_game_get_race_seed()
     * decorrelated by slot (see td5_ai_smart_race_init for the deterministic
     * seeding pattern -- no CRT rand()). */
    TD5_LOG_I(LOG_TAG, "ai_driver_race_init: DRIVER mode (P0 scaffold, forwards to SMART)");
}

/* ------------------------------------------------------------------------
 * Per-tick update
 * ---------------------------------------------------------------------- */

int td5_ai_driver_tick(int slot)
{
    (void)slot;
    /* P0: forward to SmartAI. Returning 0 tells the dispatcher to run the
     * existing classic/SmartAI path, so DRIVER mode == SMART mode for now. */
    return 0;
}

int td5_ai_driver_target_speed(int slot)
{
    (void)slot;
    /* P1: the per-span speed-profile target for this slot's current position. */
    return 0;
}
