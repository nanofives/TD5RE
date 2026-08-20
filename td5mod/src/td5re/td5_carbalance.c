/* ========================================================================
 * td5_carbalance.c — tracked per-car carparam corrections (PORT-ONLY).
 *
 * See td5_carbalance.h for why this module exists and where it hooks.
 *
 * ADDING AN ENTRY: append to k_overrides[] with the car's 3-letter code, the
 * carparam FILE offset (use the TD5CP_OFF_* names from td5_carparam.h), the
 * corrected value, and a one-line reason. Only int16 fields are supported;
 * that covers every performance field except vehicle_inertia (0xAC, int32).
 *
 * BEFORE CHANGING A MASS VALUE, read this: collision_mass (0x88) is INVERSE
 * mass — a HIGHER value means a LIGHTER car. It also no longer feeds only the
 * V2V collision impulse; td5_physics_assists.c wired it into uphill decel,
 * accel power-to-weight, and crash fly-away, so a mass edit shifts hill
 * climbing and crash behaviour too, not just the stat bar.
 * ======================================================================== */
#include <stdint.h>
#include <string.h>

#include "td5_carbalance.h"
#include "td5_carparam.h"
#include "td5_config.h"
#include "td5_platform.h"

#define LOG_TAG "asset"

/* Full carparam image size; a short buffer is never patched. */
#define TD5CB_CARPARAM_SIZE 0x10C

typedef struct {
    const char *code;    /* 3-letter car code, as in cars/<code>.zip */
    int         offset;  /* carparam FILE offset (TD5CP_OFF_*)       */
    int16_t     value;   /* corrected value                          */
    const char *reason;  /* why — shown in the log when applied      */
} TD5_CarOverride;

/* ---------------------------------------------------------------------------
 * The corrections.
 *
 * sp8 PITBULL collision_mass: the asset ships 32, which is the documented
 * TRAFFIC-vehicle inverse-mass override (td5_carparam.h:36) and is off the
 * player/AI roster scale entirely (that spans 3..20, median 16). Because the
 * accel proxy is torque * inv_mass, 32 paired with the roster's max torque
 * (180) gave Pitbull an acceleration score of 5760 against a roster median of
 * 1248 — 4.6x the median and 2x the next-fastest car, which is what stretched
 * the roster's accel spread to 17.45x. Corrected to TD5CP_INVMASS_MAX (20),
 * the lightest value any other player car legitimately uses (COBRA), which
 * keeps Pitbull the lightest/most-accelerative car without handing it the
 * traffic constant. Measured effect: accel score 5760 -> 3600, roster spread
 * 17.45x -> 10.91x, and Pitbull still leads the next cluster (3x 2880) by 25%
 * so it stays the reward car. The remaining 10.91x is dominated by the SLOW
 * end (nis HOT DOG at 330), which is left as authored — see below.
 *
 * Deliberately NOT corrected: nis HOT DOG (3), sp2 WAGON (8), day DAYTONA
 * (11). Those read as authored character for novelty/heavy vehicles rather
 * than data errors — the rest of the roster is a coherent scheme (16 typical,
 * 12 muscle, 18..20 light sports).
 * ------------------------------------------------------------------------- */
static const TD5_CarOverride k_overrides[] = {
    { "sp8", TD5CP_OFF_COLLISION_MASS, TD5CP_INVMASS_MAX,
      "was 32 = traffic inverse-mass override, off the 3..20 player scale" },
};

#define TD5CB_OVERRIDE_COUNT ((int)(sizeof(k_overrides) / sizeof(k_overrides[0])))

/* Case-insensitive tail match: does `path` end with `suffix`? */
static int cb_ends_with_ci(const char *path, const char *suffix)
{
    size_t lp, ls;
    if (!path || !suffix) return 0;
    lp = strlen(path);
    ls = strlen(suffix);
    if (ls > lp) return 0;
    {
        const char *p = path + (lp - ls);
        size_t i;
        for (i = 0; i < ls; i++) {
            char a = p[i], b = suffix[i];
            if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
            if (a != b) return 0;
        }
    }
    return 1;
}

/* Extract the 3-letter car code from a "…/cars/<code>.zip" path into out[4].
 * Returns 1 on success. Rejects anything that is not exactly 3 chars before
 * the ".zip", so custom-car folders (custom_*) never match an override. */
static int cb_car_code(const char *zip_path, char out[4])
{
    const char *dot, *p, *base;
    size_t n;
    if (!zip_path) return 0;
    if (!cb_ends_with_ci(zip_path, ".zip")) return 0;
    dot = zip_path + strlen(zip_path) - 4;   /* start of ".zip" */
    base = zip_path;
    for (p = zip_path; p < dot; p++)
        if (*p == '/' || *p == '\\') base = p + 1;
    n = (size_t)(dot - base);
    if (n != 3) return 0;
    out[0] = base[0]; out[1] = base[1]; out[2] = base[2]; out[3] = '\0';
    {   /* normalise to lower case for the table compare */
        int i;
        for (i = 0; i < 3; i++)
            if (out[i] >= 'A' && out[i] <= 'Z') out[i] = (char)(out[i] - 'A' + 'a');
    }
    return 1;
}

void td5_carbalance_apply(const char *entry_name, const char *zip_path,
                          void *data, int size)
{
    char    code[4];
    uint8_t *bytes = (uint8_t *)data;
    int      i;

    if (!bytes || size < TD5CB_CARPARAM_SIZE) return;
    if (!cb_ends_with_ci(entry_name, "carparam.dat")) return;
    if (!td5_env_flag_on("TD5RE_CAR_BALANCE")) return;   /* default ON */
    if (!cb_car_code(zip_path, code)) return;

    for (i = 0; i < TD5CB_OVERRIDE_COUNT; i++) {
        const TD5_CarOverride *o = &k_overrides[i];
        int16_t old;
        if (strcmp(code, o->code) != 0) continue;
        if (o->offset < 0 || o->offset + 2 > size) continue;
        memcpy(&old, bytes + o->offset, sizeof(old));
        if (old == o->value) continue;   /* asset already corrected */
        memcpy(bytes + o->offset, &o->value, sizeof(o->value));
        TD5_LOG_I(LOG_TAG, "carbalance: %s off=0x%X %d -> %d (%s)",
                  code, (unsigned)o->offset, (int)old, (int)o->value, o->reason);
    }
}
