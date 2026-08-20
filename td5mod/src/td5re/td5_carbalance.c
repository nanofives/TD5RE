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
/* ---------------------------------------------------------------------------
 * DONOR-PARAM CARS: aud, pro, xjr.
 *
 * TD6's param.zip ships NO <code>param.dat for these three, so
 * convert_td6_cars.py:379-395 fell back to the donor car's carparam wholesale
 * (documented there as "approximate physics", stamped [param=DONOR]; the donor
 * was tvr CERBERA). Result: four roster slots -- tvr, aud, pro, xjr -- all
 * drove as a CERBERA. pro was byte-identical to tvr in every field; aud and
 * xjr differed only in wheel_pos (0x40), which is `live` so at least those two
 * sat on their own wheelbase.
 *
 * The visible symptom was displayed-vs-actual divergence: the SELECT CAR spec
 * sheets are authored from a separate table (convert_td6_cars.py TD6_STATS) and
 * claimed a 40 mph / 2.5 s spread across these cars while the actual spread was
 * zero. Four consecutive rows also shared the identical tier score 0.376.
 *
 * There is no correct value to RESTORE -- the source data does not exist -- so
 * these are DERIVED from the only per-car intent that does: the config.nfo spec
 * sheet (line 7 = top speed mph, line 8 = 0-60 s). Method: nearest neighbour in
 * (mph, 0-60) space over the 65 roster cars that have genuine params, ranges
 * normalised, the three donor cars themselves excluded so they cannot poison it.
 *
 *   aud  EXACT twin (distance 0.000) of att "1999_AUDI_TT_COUPE" -- the same
 *        car, already on the roster with real TD6 params. Its values are
 *        COPIED, which is correct: two Audi TTs should drive alike.
 *   pro  inverse-distance-weighted mean of its 5 nearest neighbours
 *        (tur .035, sp3 .049, lit .115, esp .116, sky .139).
 *   xjr  same, over (gts .007, cer .048, lgt .063, atp .068, chd .068).
 *
 * Weighted means rather than copying the single nearest car, so pro/xjr get
 * their own distinct values instead of becoming a fresh clone of gts/tur --
 * which would just relocate the bug being fixed here.
 *
 * A global least-squares fit was tried first and REJECTED as too weak to carry
 * grip or braking: lateral-G vs drag_coefficient reaches only R2=0.06 over the
 * roster (R2=0.33 once the four extreme/fictional cars 128, s12, sp4 and sp8 are
 * excluded), and 60-0 ft vs brake_force only R2=0.36. Top speed fit acceptably
 * (R2=0.79) and 0-60 vs accel weakly (R2=0.42). Nearest-neighbour is used
 * instead because it relies on local roster structure rather than a weak global
 * trend.
 *
 * [CORRECTION 2026-08-20] An earlier revision of this comment claimed
 * "R2=0.006, no relationship at all" and called the lateral-G column junk,
 * citing sp4's 3.0G as impossible. Both claims were wrong, caused by a scratch
 * parser whose regex required a leading digit and so read ".99G" as 99.0. The
 * spec-sheet values are FAITHFUL: 128/sp4/sp8's 2.1G/3.0G/3.5G come byte-for-byte
 * out of the original per-car zips under original/cars/, s12's 2.1G is authored
 * in the tracked TD6_STATS
 * table, and the two literal "UNKNOWN" entries (atp, van) are what the 1999 game
 * shipped for those Aston Martin concept cars. The kNN decision below stands on
 * the corrected numbers; only the stated reason needed fixing.
 *
 * CONFIDENCE: top speed and torque are well supported; brake_force is
 * the noisiest of the five and is the first thing to revisit if these cars
 * brake oddly. grip/mass come along from the neighbourhood, which is why all
 * three land on invmass 16 (the roster-typical value) instead of CERBERA's 18.
 * ------------------------------------------------------------------------- */
static const TD5_CarOverride k_overrides[] = {
    { "sp8", TD5CP_OFF_COLLISION_MASS, TD5CP_INVMASS_MAX,
      "was 32 = traffic inverse-mass override, off the 3..20 player scale" },

    /* aud AUDI TT — copied from att, its exact spec twin (151 mph, 6.4 s). */
    { "aud", TD5CP_OFF_TOP_SPEED,       786, "donor-param: = att (exact spec twin)" },
    { "aud", TD5CP_OFF_DRIVE_TORQUE,     50, "donor-param: = att" },
    { "aud", TD5CP_OFF_COLLISION_MASS,   16, "donor-param: = att" },
    { "aud", TD5CP_OFF_BRAKE_FORCE,     422, "donor-param: = att" },
    { "aud", TD5CP_OFF_AERO,           2200, "donor-param: = att" },

    /* pro FORD MUSTANG COBRA — 155 mph, 5.5 s; kNN5 weighted mean. */
    { "pro", TD5CP_OFF_TOP_SPEED,       765, "donor-param: kNN5 from spec 155mph/5.5s" },
    { "pro", TD5CP_OFF_DRIVE_TORQUE,     59, "donor-param: kNN5" },
    { "pro", TD5CP_OFF_COLLISION_MASS,   16, "donor-param: kNN5" },
    { "pro", TD5CP_OFF_BRAKE_FORCE,     542, "donor-param: kNN5 (noisiest field)" },
    { "pro", TD5CP_OFF_AERO,           2239, "donor-param: kNN5" },

    /* xjr JAGUAR XJR-15 — 191 mph, 3.9 s; kNN5 weighted mean. */
    { "xjr", TD5CP_OFF_TOP_SPEED,       947, "donor-param: kNN5 from spec 191mph/3.9s" },
    { "xjr", TD5CP_OFF_DRIVE_TORQUE,    145, "donor-param: kNN5" },
    { "xjr", TD5CP_OFF_COLLISION_MASS,   16, "donor-param: kNN5" },
    { "xjr", TD5CP_OFF_BRAKE_FORCE,     541, "donor-param: kNN5 (noisiest field)" },
    { "xjr", TD5CP_OFF_AERO,           2526, "donor-param: kNN5" },
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
