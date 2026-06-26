/* ========================================================================
 * td5_carparam.h — shared carparam.dat field map + derived-stat math.
 *
 * PORT ADDITION [2026-06-25, car-parameter stats + weight mechanics].
 *
 * Single source of truth for the per-car physics parameters that the rebuilt
 * "MORE STATS" car-select panels DISPLAY and the new weight/aero MECHANICS
 * CONSUME, so the screen always reflects what the simulation actually does.
 *
 * Two addressing schemes read the SAME 268-byte file:
 *   - The FRONTEND reads carparam.dat straight from the unpacked asset, so it
 *     uses absolute FILE offsets (the TD5CP_OFF_* constants below).
 *   - The PHYSICS loads the file split into two tables — cardef (file
 *     0x00..0x8C) at actor+0x1B8 and tuning (file 0x8C..0x10C) at actor+0x1BC —
 *     and reads via its CDEF_S()/PHYS_S() macros. The correspondence is:
 *         file 0x88  == CDEF_S(a, 0x88)                 (collision_mass)
 *         file 0xAC  == PHYS_S/I(a, 0x20)  [tuning+0x20] (vehicle_inertia)
 *         file 0xB8  == PHYS_S(a, 0x2C)    [tuning+0x2C] (aero / lateral coeff)
 *         file 0xF4  == PHYS_S(a, 0x68)    [tuning+0x68] (drive_torque_mult)
 *         file 0xFA  == PHYS_S(a, 0x6E)    [tuning+0x6E] (brake_force)
 *         file 0x100 == PHYS_S(a, 0x74)    [tuning+0x74] (top_speed_limit)
 *         file 0x102 == PHYS_S(a, 0x76)    [tuning+0x76] (drivetrain_type)
 *         file 0x108 == PHYS_S(a, 0x7C)    [tuning+0x7C] (lateral_slip_stiffness)
 *   i.e. tuning local = file_off - 0x8C. The physics side keeps its own named
 *   PHYS_S / CDEF_S macros; this header is the documented bridge + the mass math.
 *
 * FIELD SEMANTICS (verified against re/assets/cars/<car>/carparam.json, which
 * carries per-field RE descriptions, and the in-session Ghidra decompilation):
 *
 *   collision_mass (0x88, int16) — INVERSE MASS in the V2V impulse. A HIGHER
 *     value = LIGHTER car (gains more velocity from a hit, gets flung); a LOWER
 *     value = HEAVIER (resists, plows through). [CONFIRMED — td5_physics.c:5858
 *     verified comment + Ghidra ApplyVehicleCollisionImpulse @0x004079C0: the
 *     dv=impulse*mass write makes |dv| rise with the field. The carparam.json
 *     "higher=heavier" prose is the lone outlier and is wrong.] Roster span for
 *     player/AI cars ~3..20 (median 16); traffic override = 32 (lightest).
 *
 *   vehicle_inertia (0xAC, int32) — yaw moment; LOWER = more agile (HANDLING).
 *   drive_torque_multiplier (0xF4, int16) — engine output (POWER / ACCEL).
 *   brake_force (0xFA, int16) — BRAKING.
 *   aero (0xB8, int16) — aerodynamic / lateral coefficient (GRIP display).
 *   top_speed_limit (0x100, int16) — hard speed cap (TOP SPEED).
 *   drivetrain_type (0x102, int16) — 1=RWD, 2=FWD, 3=AWD.
 *   lateral_slip_stiffness (0x108, int16) — slip-circle speed coupling; used as
 *     the DOWNFORCE rating (best per-car variation, 32..360).
 *   front_/rear_weight_dist (0xB4/0xB6, int16) — static axle split (WEIGHT BAL).
 * ======================================================================== */
#ifndef TD5_CARPARAM_H
#define TD5_CARPARAM_H

#include <stdint.h>

/* ---- carparam.dat absolute FILE offsets (frontend reads these directly) ---- */
#define TD5CP_OFF_COLLISION_MASS   0x88   /* int16  inverse-mass: HIGHER = lighter */
#define TD5CP_OFF_VEHICLE_INERTIA  0xAC   /* int32  yaw inertia: LOWER = more agile */
#define TD5CP_OFF_FRONT_WEIGHT     0xB4   /* int16 */
#define TD5CP_OFF_REAR_WEIGHT      0xB6   /* int16 */
#define TD5CP_OFF_AERO             0xB8   /* int16  aero / lateral coeff (GRIP)    */
#define TD5CP_OFF_DRIVE_TORQUE     0xF4   /* int16  engine torque mult (POWER)     */
#define TD5CP_OFF_BRAKE_FORCE      0xFA   /* int16  BRAKING                        */
#define TD5CP_OFF_TOP_SPEED        0x100  /* int16  TOP SPEED                       */
#define TD5CP_OFF_DRIVETRAIN       0x102  /* int16  1=RWD 2=FWD 3=AWD              */
#define TD5CP_OFF_LATERAL_SLIP     0x108  /* int16  slip coupling (DOWNFORCE)      */

/* ---- inverse-mass roster reference (player/AI cars; traffic=32 excluded) ----
 * Heaviness is anchored at the MEDIAN inverse-mass so a typical car is neutral. */
#define TD5CP_INVMASS_REF   16
#define TD5CP_INVMASS_MIN   3
#define TD5CP_INVMASS_MAX   20

/* Heaviness as a Q8 factor (0x100 = the median/neutral car). >0x100 = heavier
 * than median, <0x100 = lighter. True mass is proportional to 1/inv_mass, so
 * heaviness = REF/inv_mass. inv_mass<=0 -> neutral. PURE function — compiled
 * into both the frontend (display) and physics (behaviour) so the WEIGHT bar
 * and the weight mechanics agree by construction. Clamped to a wide [0.25,4.0]
 * band; each consuming mechanic applies its own grounded strength + final clamp
 * on top. */
static inline int32_t td5cp_heaviness_q8(int32_t inv_mass)
{
    int32_t q8;
    if (inv_mass <= 0) return 0x100;
    q8 = (TD5CP_INVMASS_REF * 0x100) / inv_mass;   /* REF / inv_mass, Q8 */
    if (q8 < 0x40)  q8 = 0x40;     /* 0.25 floor (lightest end) */
    if (q8 > 0x400) q8 = 0x400;    /* 4.00 ceil  (heaviest end) */
    return q8;
}

/* Blend a raw Q8 factor toward neutral (0x100) by strength_pct, then clamp to
 * [lo_q8, hi_q8]. Used by the weight mechanics to keep a "grounded" feel:
 * strength_pct=100 applies the full raw factor, 0 disables it. PURE. */
static inline int32_t td5cp_blend_clamp_q8(int32_t raw_q8, int32_t strength_pct,
                                           int32_t lo_q8, int32_t hi_q8)
{
    int32_t out = 0x100 + (((raw_q8 - 0x100) * strength_pct) / 100);
    if (out < lo_q8) out = lo_q8;
    if (out > hi_q8) out = hi_q8;
    return out;
}

#endif /* TD5_CARPARAM_H */
