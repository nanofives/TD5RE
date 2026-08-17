/**
 * td5_ai_driver.h -- closed-loop "Driver Model" AI (PORT-ONLY)
 *
 * A from-scratch racing-driver brain for opponent cars, added alongside the
 * existing byte-faithful AI and the SmartAI decision layer. It plans a racing
 * line and a physically-grounded speed profile from each car's own capability
 * and executes it with a predictive controller, writing the SAME five actor
 * command fields the player and the classic AI use (steering_command,
 * encounter_steering_cmd = throttle, brake_flag, handbrake_flag,
 * throttle_state). It is a COMMAND WRITER, not a physics fork -- collision,
 * suspension and drivetrain are untouched.
 *
 * Mode ladder ([GameOptions] AIModel, RACE OPTIONS row "AI MODEL"):
 *   0 = CLASSIC  -> byte-faithful original AI (SmartAI decision layer off)
 *   1 = SMART    -> SmartAI decision layer on, faithful execution layer
 *   2 = DRIVER   -> new closed-loop driver model (default)
 *
 * P0 (this commit): scaffolding only. DRIVER forwards to SMART -- the per-tick
 * tick returns "not handled" so the existing SmartAI path runs unchanged. The
 * knobs, menu row, dispatch fork and `driver` trace module are all in place so
 * later phases can fill in the path table, speed profile and controller behind
 * a stable seam.
 *
 * See docs/plans/ (Driver Model plan) for the phase breakdown.
 */

#ifndef TD5_AI_DRIVER_H
#define TD5_AI_DRIVER_H

/* AI mode enum. Persisted as g_td5.ini.ai_model; cycled by the RACE OPTIONS
 * "AI MODEL" row. Keep the numeric values stable -- they are written to the INI
 * and to save state. */
enum {
    TD5_AI_MODE_CLASSIC = 0,
    TD5_AI_MODE_SMART   = 1,
    TD5_AI_MODE_DRIVER  = 2,
    TD5_AI_MODE_COUNT   = 3
};

/* Effective AI mode for this run. Reads g_td5.ini.ai_model, clamped to a valid
 * enum value, with a TD5RE_AI_MODEL env override (0/1/2) for quick A/B testing.
 * Cheap; safe to call every tick. */
int  td5_ai_driver_mode(void);

/* Short display label ("CLASSIC" / "SMART" / "DRIVER") for a mode value. */
const char *td5_ai_driver_mode_name(int mode);

/* Race-level gate: the driver model owns racer AI slots this race. True only in
 * DRIVER mode and not in a faithful-only mode (drag / wanted). Mirrors the
 * shape of td5_ai_smart_active(). */
int  td5_ai_driver_active(void);

/* Per-slot ownership for diagnostics/trace: driver model is active AND this slot
 * is an AI-driven racer. Does not itself decide dispatch (the cop/drag/wanted
 * special cases are handled at the call site in ai_update_single_racer). */
int  td5_ai_driver_owns_slot(int slot);

/* Per-race init (per-slot driver state, deterministic seeds). Called from
 * td5_ai_init_race_actor_runtime alongside td5_ai_smart_race_init. Cheap no-op
 * when the driver model is not the active mode. */
void td5_ai_driver_race_init(void);

/* Per-tick driver update for one racer slot. Returns 1 if it fully drove the
 * car (caller must skip the classic/SmartAI path), 0 to fall through.
 *
 * P0: returns 0 unconditionally (forwards to SmartAI). */
int  td5_ai_driver_tick(int slot);

/* Current per-tick target speed for a slot, in the same raw units as the
 * actor's longitudinal_speed (+0x314). P0: returns 0 (no profile yet). Used by
 * the `driver` trace module for target-vs-actual calibration in later phases. */
int  td5_ai_driver_target_speed(int slot);

/* Current recovery-FSM state for a slot (0=NORMAL,1=AIRBORNE,2=SPUN,3=STUCK),
 * for the `driver` trace / diagnostics. */
int  td5_ai_driver_rec_state(int slot);

#endif /* TD5_AI_DRIVER_H */
