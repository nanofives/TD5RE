/* ========================================================================
 * td5_fe_carstats.c -- Car stat bars + physics-derived MORE STATS panel
 *
 * Split out of td5_frontend.c (2026-07-02). Contents, in original order:
 *   - Car 'at a glance' stat bars (Speed / Accel / Handling ranges)
 *   - Car stats sub-screen: spec-field cache, carphys stats built from
 *     carparam.dat, speed tiers, physics-stats panel + stat-bar renderer
 * Cross-TU seam: td5_frontend_internal.h.
 * ======================================================================== */

#include "td5_frontend.h"
#include "td5_asset.h"
#include "td5_track_registry.h"  /* custom-track registry: name/slot lookups + slot headroom */
#include "td5_frontend_button_cache.h"
#include "td5_game.h"
#include "td5_profile.h"
#include "td5_input.h"
#include "td5_physics.h"
#include "td5_carparam.h"      /* shared carparam field map (rebuilt MORE STATS panel) */
#include "td5_net.h"
#include "td5_platform.h"
#include "td5_render.h"
#include "td5_save.h"
#include "td5_config.h"      /* shared TD5RE_* env-knob accessors */
#include "td5_sound.h"
#include "td5_hud.h"           /* per-viewport player-identity overlay (race) */
#include "td5re.h"
#include "td5_snk_strings.h"   /* byte-exact SNK_ labels baked from Language.dll */
#include "td5_credits.h"       /* SNK_CreditsText array + dev mugshot map (Extras scroll) */
#include "td5_vectorui.h"      /* public VectorUI surface (HUD reuses these primitives) */
#include "td5_font.h"          /* [S13] runtime TTF glyph cache (native menu text) */
#include "td5_version.h"       /* build identity (version / channel / date / git rev) */
#include "td5_changelog.h"     /* CHANGELOG screen content table (file-static here) */
#include "td5_pending.h"       /* PENDING TO TEST checklist (list/state/overlay) */
#include "deps/cjson/cJSON.h"  /* track-marker JSON (retired trak_markers*.dat) */
#include "td5_frontend_internal.h"

#define LOG_TAG "frontend"

/* ======== [split] glance stat bars (moved verbatim from td5_frontend.c) ======== */
/* ========================================================================
 * Car "at a glance" stat bars (Speed / Accel / Handling).
 *
 * Three indicator bars normalised ACROSS THE WHOLE ROSTER: the weakest car maps
 * to an empty bar and the strongest to a full one, everything else linearly
 * between.
 *
 *   SPEED, ACCEL : published config.nfo spec-sheet fields (field 7 = top speed
 *                  mph, field 8 = 0-60 sec). ACCEL is a TIME (lower = better) so
 *                  its fraction is inverted. These match the MORE STATS sheet.
 *   HANDLING     : derived from the REAL physics file, carparam.dat
 *                  vehicle_inertia (file 0xAC, int32 = yaw moment of inertia).
 *                  Lower inertia = quicker yaw rotation = more agile, so the bar
 *                  shows INVERTED inertia. Sourced from carparam (not the spec
 *                  sheet) because (a) it directly affects how the car drives and
 *                  (b) it is present for EVERY car — the spec sheet's lateral-acc
 *                  field (14) is the literal text "UNKNOWN" for both Aston Martin
 *                  Vantages (ext 6, ext 16), which used to blank the whole panel.
 *                  The game keeps true lateral grip (lateral_slip_stiffness)
 *                  nearly constant across cars, so inertia/agility is the only
 *                  handling axis that actually varies and ranks the roster.
 *
 * Cached one-time scan, mirroring frontend_build_speed_pool above; cop-chase
 * cars are excluded from the ranges the same way. No numbers are ever drawn —
 * bars only (user request). ======================================================== */
static float s_cstat_spd_min, s_cstat_spd_max;
static float s_cstat_acc_min, s_cstat_acc_max;   /* 0-60 time: min == quickest */
static int   s_cstat_ranges_built = 0;

/* Parse the two spec-sheet glance strings to numeric stats. Returns a PER-FIELD
 * bitmask: bit0 = speed (field 7), bit1 = accel (field 8); 0 = neither usable.
 * Each out-param holds its value when its bit is set, else -1.0f. Per-field (not
 * all-or-nothing) so a car missing one figure still shows the other. */
static int frontend_glance_from_fields(const char *f7, const char *f8,
                                       float *spd, float *acc) {
    float s = (float)atof(f7);
    float a = (float)atof(f8);
    int mask = 0;
    if (s > 0.0f) { *spd = s; mask |= 1; } else *spd = -1.0f;
    if (a > 0.0f) { *acc = a; mask |= 2; } else *acc = -1.0f;
    return mask;
}

/* Read a car's two spec-sheet glance stats straight from its config.nfo, WITHOUT
 * touching the shared s_car_spec cache (used only by the range builder, which
 * scans every car and would otherwise clobber the displayed car). The line-split
 * mirrors frontend_load_car_spec_fields. Returns the per-field bitmask. */
static int frontend_read_car_glance_stats(int ext_id, float *spd, float *acc) {
    const char *zip = td5_asset_get_car_zip_path(ext_id);
    char fields[17][48];
    char *data;
    int sz = 0, field;
    size_t i;
    if (!zip) return 0;
    data = (char *)td5_asset_open_and_read("config.nfo", zip, &sz);
    if (!data || sz <= 0) { free(data); return 0; }
    for (field = 0; field < 17; field++) fields[field][0] = '\0';
    field = 0; i = 0;
    while (field < 17 && i < (size_t)sz) {
        size_t j = 0;
        while (i < (size_t)sz && data[i] != '\n' && data[i] != '\r') {
            if (j + 1 < sizeof(fields[0])) fields[field][j++] = data[i];
            i++;
        }
        fields[field][j] = '\0';
        while (i < (size_t)sz && (data[i] == '\n' || data[i] == '\r')) i++;
        field++;
    }
    free(data);
    return frontend_glance_from_fields(fields[7], fields[8], spd, acc);
}

/* Build the roster-wide min/max for SPEED + ACCEL (idempotent / cached). Excludes
 * cop-chase cars like the speed pool, and any car whose config.nfo won't parse. */
static void frontend_build_carstat_ranges(void) {
    int id, n = 0;
    if (s_cstat_ranges_built) return;
    s_cstat_ranges_built = 1;
    s_cstat_spd_min = s_cstat_acc_min =  1e30f;
    s_cstat_spd_max = s_cstat_acc_max = -1e30f;
    for (id = 0; id < TD5_CAR_COUNT; id++) {
        float spd, acc;
        int mask;
        if (frontend_car_is_cop(id)) continue;
        mask = frontend_read_car_glance_stats(id, &spd, &acc);
        if (!mask) continue;
        if (mask & 1) { if (spd < s_cstat_spd_min) s_cstat_spd_min = spd; if (spd > s_cstat_spd_max) s_cstat_spd_max = spd; }
        if (mask & 2) { if (acc < s_cstat_acc_min) s_cstat_acc_min = acc; if (acc > s_cstat_acc_max) s_cstat_acc_max = acc; }
        n++;
    }
    if (n == 0) {   /* nothing readable — neutral ranges so bars render half-full */
        s_cstat_spd_min = s_cstat_acc_min = 0.0f;
        s_cstat_spd_max = s_cstat_acc_max = 1.0f;
        TD5_LOG_W(LOG_TAG, "carstat_ranges: no config.nfo readable — bars neutral");
        return;
    }
    TD5_LOG_I(LOG_TAG, "carstat_ranges: n=%d spd[%.0f..%.0f] 0-60[%.2f..%.2f]",
              n, s_cstat_spd_min, s_cstat_spd_max, s_cstat_acc_min, s_cstat_acc_max);
}

static float frontend_glance_frac(float v, float lo, float hi) {
    float f;
    if (hi - lo < 1e-6f) return 0.5f;     /* degenerate (single-car) range */
    f = (v - lo) / (hi - lo);
    if (f < 0.0f) f = 0.0f;
    if (f > 1.0f) f = 1.0f;
    return f;
}

/* Map SPEED + ACCEL to [0,1] bar fractions. ACCEL is inverted (a lower 0-60 time
 * -> a fuller bar). */
static void frontend_normalize_glance(float spd, float acc, float *fs, float *fa) {
    frontend_build_carstat_ranges();
    *fs = frontend_glance_frac(spd, s_cstat_spd_min, s_cstat_spd_max);
    *fa = 1.0f - frontend_glance_frac(acc, s_cstat_acc_min, s_cstat_acc_max);  /* inverted */
}

/* ---- HANDLING bar: real-physics agility from carparam.dat vehicle_inertia ----
 * vehicle_inertia (file 0xAC, int32 yaw moment) is the divisor in the yaw-torque
 * integration, so LOWER inertia = quicker rotation = more agile. The bar shows
 * inverted, roster-normalised inertia. Every drivable car carries a carparam.dat
 * so the bar is never blank. Per-car values are cached at the one-time roster
 * scan; cop cars are cached too (so their bar still draws) but excluded from the
 * min/max so they don't skew the scale (matches the SPEED/ACCEL ranges). */
static int   s_cstat_inertia[TD5_CAR_COUNT];   /* raw vehicle_inertia per ext_id, -1 = unread */
static int   s_cstat_inertia_min, s_cstat_inertia_max;
static int   s_cstat_handling_built = 0;

/* Read a car's vehicle_inertia (carparam.dat file offset 0xAC, int32), or -1. */
static int frontend_read_car_inertia(int ext_id) {
    const char *zip = td5_asset_get_car_zip_path(ext_id);
    int sz = 0, inertia = -1;
    void *data;
    if (!zip) return -1;
    data = td5_asset_open_and_read("carparam.dat", zip, &sz);
    if (data) {
        if (sz >= 0xB0)   /* need offset 0xAC + 4 bytes */
            inertia = (int)*(const int32_t *)((const uint8_t *)data + 0xAC);
        free(data);
    }
    return inertia;
}

/* Build the roster-wide inertia min/max + per-car cache (idempotent / cached). */
static void frontend_build_handling_range(void) {
    int id, n = 0, id_min = -1, id_max = -1;
    if (s_cstat_handling_built) return;
    s_cstat_handling_built = 1;
    s_cstat_inertia_min = 0x7fffffff;
    s_cstat_inertia_max = 0;
    for (id = 0; id < TD5_CAR_COUNT; id++) {
        int inertia = frontend_read_car_inertia(id);
        s_cstat_inertia[id] = (inertia > 0) ? inertia : -1;
        if (frontend_car_is_cop(id)) continue;     /* cached, but don't set the range */
        if (inertia <= 0) continue;
        if (inertia < s_cstat_inertia_min) { s_cstat_inertia_min = inertia; id_min = id; }
        if (inertia > s_cstat_inertia_max) { s_cstat_inertia_max = inertia; id_max = id; }
        n++;
    }
    if (n == 0) {   /* no carparam readable — neutral so the bar renders half-full */
        s_cstat_inertia_min = 0; s_cstat_inertia_max = 1;
        TD5_LOG_W(LOG_TAG, "handling_range: no carparam.dat readable — bar neutral");
        return;
    }
    TD5_LOG_I(LOG_TAG,
              "handling_range: n=%d inertia[%d..%d] mostAgile=ext%d leastAgile=ext%d (lower=more agile)",
              n, s_cstat_inertia_min, s_cstat_inertia_max, id_min, id_max);
}

/* HANDLING fraction for a car: inverted, roster-normalised inertia. Returns 1 and
 * fills *out on success; 0 if this car has no readable inertia (bar left empty). */
static int frontend_handling_frac(int ext_id, float *out) {
    frontend_build_handling_range();
    if (ext_id < 0 || ext_id >= TD5_CAR_COUNT) return 0;
    if (s_cstat_inertia[ext_id] <= 0) return 0;
    *out = 1.0f - frontend_glance_frac((float)s_cstat_inertia[ext_id],
                                       (float)s_cstat_inertia_min,
                                       (float)s_cstat_inertia_max);
    return 1;
}

/* Resolve the [lo,hi) car-pool slice for a difficulty tier (0/1/2). Degenerate
 * small pools collapse to "use the whole pool". */
void frontend_speed_band_for_tier(int tier, int *lo, int *hi) {
    int n = s_speed_pool_count;
    int b0, b1;
    if (tier < 0) tier = 0;
    if (tier > 2) tier = 2;
    b0 = n / 3;            /* slow|mid boundary */
    b1 = (2 * n) / 3;      /* mid|fast boundary */
    if (tier == 0)      { *lo = 0;  *hi = b0; }
    else if (tier == 1) { *lo = b0; *hi = b1; }
    else                { *lo = b1; *hi = n;  }
    if (*hi <= *lo) { *lo = 0; *hi = n; }   /* tiny pool -> all cars */
}

/* ======== [split] car stats sub-screen + MORE STATS (moved verbatim) ======== */
/* --- Car stats sub-screen (0x40DFC0 state 0xF) ------------------------------------ */

void frontend_load_car_spec_fields(int car_index) {
    int sz = 0, field;
    size_t i;
    char *data;
    if (car_index == s_car_spec_car) return;
    s_car_spec_car = car_index;
    for (field = 0; field < 17; field++) s_car_spec[field][0] = '\0';
    if (car_index < 0 || car_index >= td5_car_total_count()) return;
    data = (char *)td5_asset_open_and_read("config.nfo", s_car_zip_paths[car_index], &sz);
    if (!data || sz <= 0) return;
    field = 0; i = 0;
    while (field < 17 && i < (size_t)sz) {
        size_t j = 0;
        while (i < (size_t)sz && data[i] != '\n' && data[i] != '\r') {
            if (j + 1 < sizeof(s_car_spec[0]))
                s_car_spec[field][j++] = data[i];
            i++;
        }
        s_car_spec[field][j] = '\0';
        while (i < (size_t)sz && (data[i] == '\n' || data[i] == '\r')) i++;
        field++;
    }
    free(data);
}

/* ========================================================================
 * Rebuilt "MORE STATS" — physics-derived car stats from carparam.dat.
 * PORT ADDITION [2026-06-25]. The original spec sheet (config.nfo: PRICE,
 * ENGINE, DISPLACEMENT, ...) is purely cosmetic — physics never reads it. This
 * panel instead shows the REAL per-car parameters the simulation uses, so every
 * bar reflects how the car actually drives. The field offsets + heaviness math
 * live in td5_carparam.h, also consumed by the physics weight mechanics, so the
 * WEIGHT/ACCEL bars on this screen match the on-track feel.
 *
 * Sources (carparam.dat file offsets):
 *   WEIGHT     invert(collision_mass 0x88)   heavy = full bar
 *   TOP SPEED  top_speed_limit 0x100
 *   ACCEL      power-to-weight = torque(0xF4) * inv_mass(0x88)
 *   POWER      drive_torque_mult 0xF4
 *   BRAKING    brake_force 0xFA
 *   GRIP       aero / lateral coeff 0xB8
 *   HANDLING   invert(vehicle_inertia 0xAC)  agile = full bar
 *   DOWNFORCE  lateral_slip_stiffness 0x108
 *   DRIVETRAIN drivetrain_type 0x102 -> RWD/FWD/AWD   (text; 1=RWD 2=FWD 3=AWD,
 *              [CONFIRMED td5_physics.c:3867])
 *   BALANCE    front/(front+rear) weight 0xB4/0xB6     (text "F.. R..")
 * Bars are roster-normalised (cop cars cached but excluded from the ranges,
 * like the glance bars). One-time cached scan; per-frame touches no files. */
/* Display order (top -> bottom). Each constant's numeric value IS its row index,
 * so the labels/captions arrays below and the loop in frontend_render_physics_stats
 * follow this order; cps_bar_value() and frontend_carphys_frac() switch on the
 * NAMES, so reordering here only moves rows, never the data each maps to.
 * [2026-06-26] WEIGHT moved down to sit under GRIP (user request). */
enum {
    CPS_TOPSPEED = 0, CPS_ACCEL, CPS_POWER, CPS_BRAKING, CPS_GRIP,
    CPS_WEIGHT, CPS_HANDLING, CPS_DOWNFORCE, CPS_DRIVETRAIN, CPS_BALANCE,
    CPS_COUNT
};
#define CPS_BAR_COUNT 8   /* first 8 stats are bars; last 2 are text */

static const char *k_cps_labels[CPS_COUNT] = {
    "TOP SPEED", "ACCEL", "POWER", "BRAKING", "GRIP",
    "WEIGHT", "HANDLING", "DOWNFORCE", "DRIVETRAIN", "BALANCE"
};

/* [2026-06-26 PORT ADDITION] Short plain-language captions for the less-obvious
 * stats, drawn as a small dim second line under the label in the SINGLE-PLAYER
 * MORE STATS panel only (gated on !compact — the tiny split-screen MP panes have
 * no vertical room). NULL = the label already reads clearly on its own
 * (TOP SPEED/BRAKING/WEIGHT). Keep each caption short so it stays inside the
 * ~40%-of-panel label column and never runs under the bar. Order matches
 * k_cps_labels above. */
static const char *k_cps_explain[CPS_COUNT] = {
    NULL,                  /* TOP SPEED  */
    "power-to-weight",     /* ACCEL      */
    "engine output",       /* POWER  (raw engine pull, before weight) */
    NULL,                  /* BRAKING    */
    "cornering grip",      /* GRIP       */
    NULL,                  /* WEIGHT     */
    "agility",             /* HANDLING   */
    "grip at speed",       /* DOWNFORCE  */
    "driven wheels",       /* DRIVETRAIN */
    "weight: front / rear" /* BALANCE    */
};

typedef struct {
    int     valid;
    int32_t mass;        /* 0x88  i16 inverse-mass (higher = lighter) */
    int32_t inertia;     /* 0xAC  i32 */
    int32_t fwt, rwt;    /* 0xB4/0xB6 i16 */
    int32_t grip;        /* 0xB8  i16 */
    int32_t torque;      /* 0xF4  i16 */
    int32_t brake;       /* 0xFA  i16 */
    int32_t topspd;      /* 0x100 i16 */
    int32_t drivetrain;  /* 0x102 i16 */
    int32_t downforce;   /* 0x108 i16 */
    int64_t accel_score; /* derived torque * inv_mass (power-to-weight) */
} CarPhysStats;

static CarPhysStats s_cps[TD5_CAR_COUNT];
static float        s_cps_min[CPS_BAR_COUNT], s_cps_max[CPS_BAR_COUNT];
static int          s_cps_built = 0;

static int32_t cps_rd_i16(const uint8_t *d, int sz, int off) {
    if (off < 0 || off + 2 > sz) return 0;
    return (int32_t)(int16_t)((uint16_t)d[off] | ((uint16_t)d[off + 1] << 8));
}
static int32_t cps_rd_i32(const uint8_t *d, int sz, int off) {
    if (off < 0 || off + 4 > sz) return 0;
    return (int32_t)((uint32_t)d[off] | ((uint32_t)d[off + 1] << 8) |
                     ((uint32_t)d[off + 2] << 16) | ((uint32_t)d[off + 3] << 24));
}

/* Per-bar raw value, with the inversion-direction baked in so that a HIGHER
 * return = a FULLER bar (WEIGHT/HANDLING are negated because lower mass-term /
 * lower inertia mean heavier / more agile). */
static float cps_bar_value(const CarPhysStats *c, int stat) {
    switch (stat) {
    case CPS_WEIGHT:    return -(float)c->mass;          /* heavier (lower invmass) -> higher */
    case CPS_TOPSPEED:  return  (float)c->topspd;
    case CPS_ACCEL:     return  (float)c->accel_score;
    case CPS_POWER:     return  (float)c->torque;
    case CPS_BRAKING:   return  (float)c->brake;
    case CPS_GRIP:      return  (float)c->grip;
    case CPS_HANDLING:  return -(float)c->inertia;       /* lower inertia -> higher (agile) */
    case CPS_DOWNFORCE: return  (float)c->downforce;
    default:            return  0.0f;
    }
}

static void frontend_build_carphys_stats(void) {
    int id, s;
    if (s_cps_built) return;
    s_cps_built = 1;
    for (s = 0; s < CPS_BAR_COUNT; s++) { s_cps_min[s] = 1e30f; s_cps_max[s] = -1e30f; }
    for (id = 0; id < TD5_CAR_COUNT; id++) {
        const char *zip = td5_asset_get_car_zip_path(id);
        int sz = 0;
        uint8_t *d;
        CarPhysStats *c = &s_cps[id];
        memset(c, 0, sizeof(*c));
        if (!zip) continue;
        d = (uint8_t *)td5_asset_open_and_read("carparam.dat", zip, &sz);
        if (!d) continue;
        if (sz >= TD5CP_OFF_LATERAL_SLIP + 2) {
            c->mass       = cps_rd_i16(d, sz, TD5CP_OFF_COLLISION_MASS);
            c->inertia    = cps_rd_i32(d, sz, TD5CP_OFF_VEHICLE_INERTIA);
            c->fwt        = cps_rd_i16(d, sz, TD5CP_OFF_FRONT_WEIGHT);
            c->rwt        = cps_rd_i16(d, sz, TD5CP_OFF_REAR_WEIGHT);
            c->grip       = cps_rd_i16(d, sz, TD5CP_OFF_AERO);
            c->torque     = cps_rd_i16(d, sz, TD5CP_OFF_DRIVE_TORQUE);
            c->brake      = cps_rd_i16(d, sz, TD5CP_OFF_BRAKE_FORCE);
            c->topspd     = cps_rd_i16(d, sz, TD5CP_OFF_TOP_SPEED);
            c->drivetrain = cps_rd_i16(d, sz, TD5CP_OFF_DRIVETRAIN);
            c->downforce  = cps_rd_i16(d, sz, TD5CP_OFF_LATERAL_SLIP);
            c->accel_score = (int64_t)c->torque * (int64_t)(c->mass > 0 ? c->mass : 1);
            c->valid = (c->mass > 0 && c->topspd > 0);
        }
        free(d);
        if (!c->valid) continue;
        if (frontend_car_is_cop(id)) continue;   /* cached, excluded from the ranges */
        for (s = 0; s < CPS_BAR_COUNT; s++) {
            float v = cps_bar_value(c, s);
            if (v < s_cps_min[s]) s_cps_min[s] = v;
            if (v > s_cps_max[s]) s_cps_max[s] = v;
        }
    }
    for (s = 0; s < CPS_BAR_COUNT; s++)
        if (s_cps_max[s] < s_cps_min[s]) { s_cps_min[s] = 0.0f; s_cps_max[s] = 1.0f; }
    TD5_LOG_I(LOG_TAG, "carphys_stats: built physics MORE STATS (mass term [%.0f..%.0f] top[%.0f..%.0f])",
              -s_cps_max[CPS_WEIGHT], -s_cps_min[CPS_WEIGHT],
              s_cps_min[CPS_TOPSPEED], s_cps_max[CPS_TOPSPEED]);
}

/* Normalised [0,1] bar fraction for (ext_id, bar stat); -1 if invalid. */
static float frontend_carphys_frac(int ext_id, int stat) {
    CarPhysStats *c;
    float v, lo, hi;
    frontend_build_carphys_stats();
    if (ext_id < 0 || ext_id >= TD5_CAR_COUNT || stat < 0 || stat >= CPS_BAR_COUNT) return -1.0f;
    c = &s_cps[ext_id];
    if (!c->valid) return -1.0f;
    v  = cps_bar_value(c, stat);
    lo = s_cps_min[stat]; hi = s_cps_max[stat];
    if (hi - lo < 1e-6f) return 0.5f;
    v = (v - lo) / (hi - lo);
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    return v;
}

/* [HOST CAR OPTIONS 2026-06-28] Speed classes by acceleration + top-speed ONLY.
 * Each built-in, non-cop car with a valid carparam has a combined score = mean of
 * its normalized TOP-SPEED and ACCEL fractions (both roster-normalised + cop-
 * excluded by frontend_carphys_frac). Cars are sorted by that score and assigned to
 * classes by RANK so every class is guaranteed to hold SEVERAL DISTINCT cars:
 *   - the single slowest and single fastest car are dropped (the extremes),
 *   - SLOW = the next K slowest, FAST = the next K fastest, AVG = K around the
 *     median. The three windows never overlap, so each car is in at most ONE class
 *     and no car is ever shared between classes.
 * K = TD5RE_HOST_TIER_CARS, default max(9, ~15% of the scored roster), clamped so
 * the three windows + the two dropped extremes all fit. [user 2026-06-29: a fixed
 * ~10% band collapsed 'fast' to a single car (Pitbull 2); each class must hold AT
 * LEAST 9 distinct cars and no car may sit in more than one class.] Cached once. */
static int   s_speed_built = 0;
static float s_speed_min   = 0.0f, s_speed_max = 1.0f;   /* combined-score range */
static int   s_car_tier[TD5_CAR_COUNT];                  /* per-car class: -1/0/1/2 */

static float frontend_car_speed_score(int car) {
    float ft = frontend_carphys_frac(car, CPS_TOPSPEED);
    float fa = frontend_carphys_frac(car, CPS_ACCEL);
    if (ft < 0.0f || fa < 0.0f) return -1.0f;   /* invalid / cop */
    return 0.5f * (ft + fa);
}

/* Target cars per class for a scored roster of `n`. Default = max(9, ~15% of n) so
 * each class holds at least 9 distinct cars [user 2026-06-29]; TD5RE_HOST_TIER_CARS
 * overrides with an explicit count. Caller still clamps to the no-overlap maximum. */
static int frontend_host_tier_cars(int n) {
    const char *e = getenv("TD5RE_HOST_TIER_CARS");
    int k;
    if (e && e[0]) {
        k = atoi(e);
    } else {
        k = (n * 15 + 50) / 100;   /* ~15% of the pool, rounded */
        if (k < 9) k = 9;          /* floor: at least 9 cars per class */
    }
    if (k < 2)  k = 2;
    if (k > 40) k = 40;
    return k;
}

static void frontend_build_speed_tiers(void) {
    int   ids[TD5_CAR_COUNT];
    float sc[TD5_CAR_COUNT];
    int   i, j, n = 0;
    if (s_speed_built) return;
    s_speed_built = 1;
    for (i = 0; i < TD5_CAR_COUNT; i++) s_car_tier[i] = -1;
    for (i = 0; i < TD5_CAR_COUNT; i++) {
        float s = frontend_car_speed_score(i);
        if (s < 0.0f) continue;
        ids[n] = i; sc[n] = s; n++;
    }
    /* Insertion sort (ids parallel to sc) ascending by score (n <= 76, runs once). */
    for (i = 1; i < n; i++) {
        float sv = sc[i]; int iv = ids[i];
        for (j = i - 1; j >= 0 && sc[j] > sv; j--) { sc[j + 1] = sc[j]; ids[j + 1] = ids[j]; }
        sc[j + 1] = sv; ids[j + 1] = iv;
    }
    s_speed_min = (n > 0) ? sc[0]     : 0.0f;
    s_speed_max = (n > 0) ? sc[n - 1] : 1.0f;
    if (n >= 5) {
        int k    = frontend_host_tier_cars(n);
        int kmax = (n - 2) / 3;                  /* room for 3 windows + 2 dropped extremes */
        int slo, shi, alo, ahi, flo, fhi;
        if (kmax < 1) kmax = 1;
        if (k > kmax) k = kmax;
        slo = 1;             shi = 1 + k;                  /* SLOW: next-slowest K (rank 0 dropped)    */
        alo = (n - k) / 2;   ahi = (n - k) / 2 + k;        /* AVG : K around the median                */
        flo = n - 1 - k;     fhi = n - 1;                  /* FAST: next-fastest K (rank n-1 dropped)   */
        for (j = slo; j < shi; j++) s_car_tier[ids[j]] = 0;
        for (j = alo; j < ahi; j++) s_car_tier[ids[j]] = 1;
        for (j = flo; j < fhi; j++) s_car_tier[ids[j]] = 2;
        TD5_LOG_I(LOG_TAG, "host car speed tiers: %d cars, %d/class, slow[%d,%d) avg[%d,%d) fast[%d,%d)",
                  n, k, slo, shi, alo, ahi, flo, fhi);
    } else {
        TD5_LOG_W(LOG_TAG, "host car speed tiers: only %d scored cars - classes disabled", n);
    }
}

float frontend_car_speed_norm(int car) {
    float s = frontend_car_speed_score(car);
    float nrm;
    if (s < 0.0f) return -1.0f;
    frontend_build_speed_tiers();
    if (s_speed_max - s_speed_min < 1e-6f) return 0.5f;
    nrm = (s - s_speed_min) / (s_speed_max - s_speed_min);
    if (nrm < 0.0f) nrm = 0.0f;
    if (nrm > 1.0f) nrm = 1.0f;
    return nrm;
}

float frontend_speed_tier_center(int tier) {
    return (tier <= 0) ? 0.15f : (tier == 1) ? 0.50f : 0.85f;
}

int frontend_carphys_speed_tier(int car) {
    if (car < 0 || car >= TD5_CAR_COUNT) return -1;
    frontend_build_speed_tiers();
    return s_car_tier[car];
}

static const char *frontend_carphys_drivetrain_text(int ext_id) {
    int dt;
    if (ext_id < 0 || ext_id >= TD5_CAR_COUNT || !s_cps[ext_id].valid) return "-";
    dt = (int)s_cps[ext_id].drivetrain;
    return (dt == 1) ? "RWD" : (dt == 2) ? "FWD" : (dt == 3) ? "AWD" : "-";
}
/* Front-axle weight share as a 0..100 int (rear = 100 - front); 0 if invalid. */
static int frontend_carphys_front_pct(int ext_id) {
    if (ext_id >= 0 && ext_id < TD5_CAR_COUNT && s_cps[ext_id].valid) {
        int fw = (int)s_cps[ext_id].fwt, rw = (int)s_cps[ext_id].rwt;
        int tot = fw + rw;
        if (tot > 0) return (fw * 100 + tot / 2) / tot;
    }
    return 0;
}
static void frontend_carphys_balance_text(int ext_id, char *out, size_t cap) {
    int f = frontend_carphys_front_pct(ext_id);
    if (f <= 0) snprintf(out, cap, "-");
    else        snprintf(out, cap, "%d%% / %d%%", f, 100 - f);  /* front% / rear% */
}

/* Unified physics-stats panel renderer (SP overlay + MP pane). Draws CPS_COUNT
 * rows of label + bar (first 8) or text value (last 2). `compact` shrinks the
 * label font for the small MP panes. */
void frontend_render_physics_stats(int ext_id, float px, float py, float pw, float ph,
                                          uint32_t accent, int compact, float sx, float sy) {
    int i;
    float rh   = ph / (float)CPS_COUNT;
    float lblw = pw * (compact ? 0.46f : 0.40f);
    float barx = px + lblw + 4.0f;
    float barw = (px + pw) - barx - 2.0f;
    float lsx = sx, lsy = sy, capd;
    char  val[32];

    frontend_build_carphys_stats();
    if (barw < 6.0f) barw = 6.0f;
    if (compact) {
        float s = rh / 11.0f;
        if (s > 1.0f) s = 1.0f;
        if (s < 0.42f) s = 0.42f;
        lsx = sx * s; lsy = sy * s;
    }
    capd = SMALLFONT_TTF_CAP * (lsy / sy);

    for (i = 0; i < CPS_COUNT; i++) {
        float ry  = py + (float)i * rh;
        float tyc = (ry + (rh - capd) * 0.5f) * sy;
        const char *expl = compact ? NULL : k_cps_explain[i];
        if (expl) {
            /* [2026-06-26] Two-line label: stat name on top, dim caption beneath.
             * Both sit in the label column (left of barx), so the bar / text value
             * to the right is untouched. Offsets in design px (SP rh=22.4): label
             * cell-top ry+2 (baseline ry+12), caption cell-top ry+12 at 0.62x so
             * its descenders end ~ry+20, inside the row. */
            float exsx = lsx * 0.62f, exsy = lsy * 0.62f;
            fe_draw_small_text(px * sx, (ry + 2.0f)  * sy, k_cps_labels[i], 0xFFC8C8C8u, lsx,  lsy);
            fe_draw_small_text(px * sx, (ry + 12.0f) * sy, expl,           0xFF8FA0B4u, exsx, exsy);
        } else {
            fe_draw_small_text(px * sx, tyc, k_cps_labels[i], 0xFFC8C8C8u, lsx, lsy);
        }
        if (i < CPS_BAR_COUNT) {
            float barh = rh - (compact ? 3.0f : 6.0f);
            float bary, f;
            if (barh < 2.0f) barh = 2.0f;
            bary = ry + (rh - barh) * 0.5f;
            td5_plat_render_set_preset(TD5_PRESET_OPAQUE_LINEAR);
            fe_draw_quad(barx * sx, bary * sy, barw * sx, barh * sy, 0xFF101828u, -1, 0, 0, 1, 1);
            f = frontend_carphys_frac(ext_id, i);
            if (f > 0.0f)
                fe_draw_quad(barx * sx, bary * sy, f * barw * sx, barh * sy, accent, -1, 0, 0, 1, 1);
        } else if (i == CPS_BALANCE && !compact) {
            /* [2026-06-26] BALANCE as a visual front/rear weight-split graph (SP):
             * the bar is split into a FRONT segment (left, amber accent) and a REAR
             * segment (right, blue). A bright tick at the centre marks the 50/50
             * neutral point, so a nose- or tail-heavy car reads at a glance. The
             * exact percentages are printed inside their own segments. */
            int   fpct = frontend_carphys_front_pct(ext_id);
            float barh = rh - 6.0f;
            float bary, ff;
            if (barh < 2.0f) barh = 2.0f;
            bary = ry + (rh - barh) * 0.5f;
            ff   = (fpct > 0) ? (float)fpct / 100.0f : 0.5f;
            td5_plat_render_set_preset(TD5_PRESET_OPAQUE_LINEAR);
            fe_draw_quad(barx * sx, bary * sy, barw * sx, barh * sy, 0xFF101828u, -1, 0, 0, 1, 1);
            if (fpct > 0) {
                fe_draw_quad(barx * sx, bary * sy, ff * barw * sx, barh * sy,
                             accent, -1, 0, 0, 1, 1);                       /* front (left)  */
                fe_draw_quad((barx + ff * barw) * sx, bary * sy,
                             (1.0f - ff) * barw * sx, barh * sy,
                             0xFF4890D0u, -1, 0, 0, 1, 1);                  /* rear (right)  */
            }
            /* centre 50/50 reference tick (drawn over the fills) */
            fe_draw_quad((barx + 0.5f * barw - 0.75f) * sx, (bary - 1.0f) * sy,
                         1.5f * sx, (barh + 2.0f) * sy, 0xFFFFFFFFu, -1, 0, 0, 1, 1);
            if (fpct > 0) {
                char  fp[8], rp[8];
                float pls_x = lsx * 0.7f, pls_y = lsy * 0.7f;
                float pcapd = SMALLFONT_TTF_CAP * (pls_y / sy);
                float pty   = (ry + (rh - pcapd) * 0.5f) * sy;
                float rw_txt;
                snprintf(fp, sizeof fp, "%d%%", fpct);
                snprintf(rp, sizeof rp, "%d%%", 100 - fpct);
                rw_txt = fe_measure_small_text(rp) * fe_glyph_sx(pls_x, pls_y);
                fe_draw_small_text((barx + 3.0f) * sx, pty, fp, 0xFF202830u, pls_x, pls_y);       /* dark on amber */
                fe_draw_small_text((barx + barw - 3.0f) * sx - rw_txt, pty, rp, 0xFFFFFFFFu, pls_x, pls_y); /* white on blue */
            }
        } else {
            const char *t;
            if (i == CPS_DRIVETRAIN) t = frontend_carphys_drivetrain_text(ext_id);
            else { frontend_carphys_balance_text(ext_id, val, sizeof val); t = val; }
            fe_draw_small_text(barx * sx, tyc, t, 0xFFFFFFFFu, lsx, lsy);
        }
    }
}

void frontend_render_car_stats_overlay(float sx, float sy) {
    /* [2026-06-25] Rebuilt MORE STATS: real per-car physics parameters from
     * carparam.dat (frontend_render_physics_stats), replacing the cosmetic
     * config.nfo spec sheet (PRICE/ENGINE/HP — never read by the simulation).
     * Drawn in the car preview area (x=232, y=126, ~384x224). s_car_spec_car is
     * the live selected car (set by frontend_load_car_spec_fields each frame). */
    int ext = s_car_spec_car;
    TD5_LOG_I(LOG_TAG, "car_stats_overlay(physics): car=%d", ext);
    /* [2026-06-26] Shifted the panel RIGHT (x 232->252) so its labels clear the
     * port-added RANDOMIZE chip (canvas 218..246, y171..199). The original drew
     * this header column at canvasW-0x198 = 232 [CONFIRMED @0x0040dff1] but it had
     * NO randomize button there; the chip is a port enhancement, so this small
     * shift is a deliberate port-only divergence. The right edge stays at 616
     * (pw 384->364), preserving the original's clean right margin. */
    frontend_render_physics_stats(ext, 252.0f, 126.0f, 364.0f, 224.0f,
                                  0xFFE8C040u /* amber == FE_CARSTAT_ACCENT */, 0, sx, sy);
}

/* Car-select ENTRY sidebar slide-in duration (bar/curve/topbar sweeping in +
 * the blue fill growing from the right). Halved from the original 2500ms so the
 * screen fades in 2x faster (S04 user request). Referenced by BOTH the state-2
 * advance gate (Screen_CarSelection) and the slide-in render below, which derive
 * the same t from s_anim_start_ms — keep them on this one constant so they stay
 * in sync. (frontend_update_timed_animation already applies a global 2x factor,
 * so the slide actually settles in ~half this value of wall-clock.) */

/* [ARCH-DIVERGENCE: DDraw QueueFrontendOverlayRect -> D3D11 fe_draw_surface_rect; L5 sweep 2026-05-21]
 *   Port reimplements DrawCarSelectionPreviewOverlay (0x0040DDC0) using D3D11
 *   batched quads instead of DDraw blit-queue. Same animation phases (state 0
 *   static / state 11=0xB slide-out / state 14=0xE slide-in), same coordinate
 *   constants (0x198 width, 0x118 height, 0x5A alpha, 0x40/0x20 step deltas,
 *   0x4A8 offscreen offset). DDraw color-key + per-frame surface tracking
 *   replaced by tex-page + alpha blending. */
/* Non-interactive "at a glance" stat panel: a button-framed box with three
 * relative bars (Speed / Accel / Handling), drawn between PAINT and MORE STATS on
 * every car-select screen. SPEED/ACCEL come from raw config.nfo spec strings (SP
 * from the shared s_car_spec cache, MP from its per-pane spec cache); HANDLING is
 * derived from the car's carparam.dat (by `car_ext_id`) via a one-time cached
 * scan, so per-frame this still touches no files. `accent` fills the bars (player
 * colour in MP); `compact` shrinks the rows + scales the labels down so the full
 * names still fit in the small split panes. */
void frontend_draw_car_stat_bars(float bx, float by, float bw, float bh,
                                        const char *f7, const char *f8, int car_ext_id,
                                        uint32_t accent, int compact, float frame_scale,
                                        float sx, float sy) {
    static const char *lbl[3]  = { "SPEED", "ACCEL", "HANDLING" };
    static const char *lblS[3] = { "S", "A", "H" };   /* narrow-column fallback */
    const char **labels = lbl;
    float spd = 0, acc = 0, fr[3] = { 0, 0, 0 };
    int valid_mask;   /* bit0=speed, bit1=accel, bit2=handling — gates per-bar fill */
    float padx = compact ? 4.0f : 7.0f;
    float pady = compact ? 2.0f : 6.0f;
    /* In the single-column MP layout (compact==1) the panel sits UNDER the CAR/PAINT
     * buttons, so line the labels up with their text (x+17) and stop the bars before
     * the ◄► arrows (x+w-18). In the two-column card (compact==2) the panel is its OWN
     * column with no buttons to align to, so use tight padding and let the bar track
     * fill the whole column. SP (compact==0) keeps padx. */
    float content_l = (compact == 2) ? 5.0f : (compact ? 17.0f : padx);
    float content_r = (compact == 2) ? 5.0f : (compact ? 18.0f : padx);
    float top  = by + pady;
    float rowh = (bh - 2.0f * pady) / 3.0f;
    float barh = compact ? (rowh - 2.0f) : (rowh - 5.0f);
    float lsx = sx, lsy = sy, capd, lblw, barx, barw;
    int i;

    if (barh < 2.0f) barh = 2.0f;
    /* Shrink the label font to the row in compact (small split) panes so the
     * full names still fit; SP keeps the full small-font size.
     * [rebalance 2026-06-28] Reach full label size sooner (rowh/9 vs /11) and lift
     * the floor 0.42 -> 0.60 so SPEED/ACCEL/HANDLING stay legible in the cramped
     * 5+ player panes. The single-column flex below now reserves this panel its
     * readable height FIRST (stealing from the car/buttons), so the floor rarely
     * binds; when it does (the over-subscribed 7-9p 3x3 grid) 0.60 is the smallest
     * size that still reads. */
    if (compact) {
        /* [2026-06-29] In the two-column card (compact==2) the stat panel is its own
         * narrow column, so cap the label font a touch below full size: a slightly
         * smaller "HANDLING" frees horizontal room for a LONGER bar while the full
         * word stays legible. The single-column panel (compact==1) keeps full size. */
        float s = rowh / 9.0f;
        float smax = (compact == 2) ? 0.78f : 1.0f;
        if (s > smax) s = smax;
        if (s < 0.60f) s = 0.60f;
        lsx = sx * s; lsy = sy * s;
    }
    capd = SMALLFONT_TTF_CAP * (lsy / sy);

    /* [2026-06-26] Source all three quick bars from the SAME roster-normalised
     * carparam physics (frontend_carphys_frac) the MORE STATS panel uses, so
     * SPEED/ACCEL/HANDLING read on an IDENTICAL scale across both displays. Was:
     * SPEED/ACCEL from the cosmetic config.nfo spec glance + HANDLING from a
     * separate inertia frac — a visibly different scale than MORE STATS. The spec
     * glance / inertia frac are kept only as a fallback for a car whose
     * carparam.dat failed to load (carphys frac returns -1). */
    {
        float sf = frontend_carphys_frac(car_ext_id, CPS_TOPSPEED);  /* SPEED <-> TOP SPEED */
        float af = frontend_carphys_frac(car_ext_id, CPS_ACCEL);
        float hf = frontend_carphys_frac(car_ext_id, CPS_HANDLING);
        valid_mask = 0;
        if (sf >= 0.0f) { fr[0] = sf; valid_mask |= 1; }
        if (af >= 0.0f) { fr[1] = af; valid_mask |= 2; }
        if (hf >= 0.0f) { fr[2] = hf; valid_mask |= 4; }
        if ((valid_mask & 3) != 3) {           /* carparam missing -> spec glance */
            int gm = frontend_glance_from_fields(f7, f8, &spd, &acc);
            if (gm) {
                float gs = 0.0f, ga = 0.0f;
                frontend_normalize_glance(spd, acc, &gs, &ga);
                if (!(valid_mask & 1) && (gm & 1)) { fr[0] = gs; valid_mask |= 1; }
                if (!(valid_mask & 2) && (gm & 2)) { fr[1] = ga; valid_mask |= 2; }
            }
        }
        if (!(valid_mask & 4) && frontend_handling_frac(car_ext_id, &fr[2]))
            valid_mask |= 4;                    /* carparam missing -> inertia frac */
    }

    /* Frame: the regular blue/unselected button look (non-interactive). In the
     * compact (small-split) panes the rim is thinned to match the pane buttons.
     * [2026-06-29] When the caller passes a frame_scale > 0 (the MP grid passes
     * the BUTTON height's scale), use it so the panel's border is IDENTICAL to the
     * buttons beside it regardless of the panel's own height — fixes the "stat
     * panel border looks fatter than the buttons" mismatch. frame_scale <= 0 keeps
     * the legacy self-sized rim (bh/32). The SP panel keeps the full menu rim. */
    {
        float fscale = (frame_scale > 0.0f) ? frame_scale : bh / 32.0f;
        if (fscale > 1.0f) fscale = 1.0f;
        if (fscale < 0.34f) fscale = 0.34f;
        fe_draw_button_frame_fill_scaled(bx * sx, by * sy, bw * sx, bh * sy, 1,
                                         0xFF392152u, compact ? fscale : 1.0f, sx, sy);
    }

    /* Full-name label column, sized to the WIDEST label ("HANDLING"). If the box is
     * too narrow for the full word + a usable bar (a narrow two-column stats panel),
     * fall back to single-letter labels (S/A/H) so each bar still says WHICH stat it
     * is, instead of three anonymous bars. Only drop the label entirely when even a
     * single letter won't fit. */
    {
        float wmax = fe_measure_small_text(lbl[0]);
        float w1 = fe_measure_small_text(lbl[1]);
        float w2 = fe_measure_small_text(lbl[2]);
        if (w1 > wmax) wmax = w1;
        if (w2 > wmax) wmax = w2;
        lblw = wmax * fe_glyph_sx(lsx, lsy) / sx;
        if (bw < content_l + content_r + lblw + 8.0f) {
            float sw = fe_measure_small_text("H") * fe_glyph_sx(lsx, lsy) / sx;
            if (bw >= content_l + content_r + sw + 8.0f) { labels = lblS; lblw = sw; }
            else lblw = 0.0f;
        }
    }
    barx = bx + content_l + (lblw > 0.0f ? lblw + 4.0f : 0.0f);
    barw = (bx + bw - content_r) - barx;
    if (barw < 4.0f) barw = 4.0f;

    td5_plat_render_set_preset(TD5_PRESET_OPAQUE_LINEAR);
    for (i = 0; i < 3; i++) {
        float ry   = top + (float)i * rowh;
        float bary = ry + (rowh - barh) * 0.5f;
        /* Fill only the bars with a valid value. HANDLING is always present
         * (every car has carparam inertia); a spec-sheet SPEED/ACCEL that failed
         * to parse leaves just the empty track. The gate also stops the INVERTED
         * accel fraction from painting a full bar when 0-60 is missing
         * (frac(-1)=0 -> 1-0 = full). */
        float fillw = ((valid_mask >> i) & 1) ? fr[i] * barw : 0.0f;
        if (lblw > 0.0f) {
            float ty = (ry + (rowh - capd) * 0.5f) * sy;
            fe_draw_small_text((bx + content_l) * sx, ty, labels[i], 0xFFC8C8C8u, lsx, lsy);
        }
        fe_draw_quad(barx * sx, bary * sy, barw * sx, barh * sy, 0xFF101828u, -1, 0, 0, 1, 1);
        if (fillw > 0.0f)
            fe_draw_quad(barx * sx, bary * sy, fillw * sx, barh * sy, accent, -1, 0, 0, 1, 1);
    }
}
