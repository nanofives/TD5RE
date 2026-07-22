/* ========================================================================
 * td5_inputscript.h — scripted input harness (dev/test affordance)
 *
 * Drives the game from a plain-text command file instead of a human at the
 * keyboard. Supersedes [Trace] AutoThrottle for FUNCTIONAL testing: where
 * AutoThrottle can only hold full gas on slot 0, a script can exercise every
 * input the game reads — throttle, brake, handbrake, horn, gears, camera,
 * rear view, steering taps, pause, and any raw key (menu navigation, ESC,
 * R-recovery, F12 overlay, ...). AutoThrottle itself is retired for testing
 * but kept alive for /diff-race parity captures (its instant-0x100 throttle
 * write is what the original-side Frida harness mirrors — do not remove).
 *
 * Enable via td5re.ini:
 *     [Trace]
 *     InputScript = inputscripts/full_controls_demo.txt
 * or CLI: --InputScript=path (CLI > INI). Path is relative to the CWD.
 *
 * Script format — one command per line, '#' starts a comment:
 *     <when> <verb> [args]
 *   <when>  N   = fire N harness ticks after the last `sync` (or file start)
 *           +N  = fire N ticks after the PREVIOUS command's fire time
 *           Omitted (bare verb) = fire at the previous command's time.
 *           One harness tick == 1/60 s of WALL TIME (menu AND race) — fps-
 *           independent, and a `press` (4 ticks) always spans >= 2 sim polls.
 *           Under TraceFastForward the sim runs faster than the wall clock,
 *           so script timing shifts relative to sim ticks — use AutoThrottle
 *           for tick-exact parity captures.
 *   verbs:
 *     throttle|brake|handbrake|horn|gearup|geardown|camera|rearview|
 *     left|right|pause|escape  <0|1|press>
 *         Race action bits OR'd into this slot's control word AFTER the
 *         hardware poll — flows through the identical downstream paths as
 *         real input (steering ramp, horn edge latch, gear debounce, ...).
 *         1 = hold, 0 = release, press = hold 4 ticks then auto-release.
 *     key <name|0xNN> <0|1|press>
 *         Raw DIK scancode injection at the platform layer — read by menu
 *         navigation, the pause menu, dev keys, and the rebindable in-race
 *         keyboard map exactly as a physical key. Names: up down left right
 *         enter esc space backspace tab p q a z t x r f1..f12, or 0xNN hex.
 *     slot <N>
 *         Target racer slot for subsequent race-action verbs (default 0).
 *     sync race|menu
 *         Suspend until the game reaches that context (race = in-race with
 *         sim running; menu = frontend live), then re-base the clock to 0.
 *     log <text>
 *         Emit a marker line to the input log (frontend.log).
 *     quit
 *         Request a clean shutdown (flushes logs).
 *
 * Inert when no script is configured (zero per-frame cost beyond one branch).
 * Release builds clamp InputScript empty in main.c like the other dev knobs.
 * ======================================================================== */
#ifndef TD5_INPUTSCRIPT_H
#define TD5_INPUTSCRIPT_H

#include <stdint.h>

int  td5_inputscript_init(void);      /* load + parse the configured script */
void td5_inputscript_shutdown(void);

/* Non-zero while a script is loaded and not yet finished. */
int  td5_inputscript_active(void);

/* Advance the harness clock + fire due commands. Called once per rendered
 * frame from the game state runner (menu, race, pause — all states). */
void td5_inputscript_frame_tick(void);

/* Currently-held race action bits for a slot (OR-mask over the polled
 * hardware word). 0 when inactive or no held actions. */
uint32_t td5_inputscript_race_bits(int slot);

/* Resolve an action verb name ("throttle", "brake", "left", ...) to its
 * TD5_INPUT_* bit, or 0 if unknown. Shared with the live-control module's
 * hold_action/release_action verbs so both speak the same action names. */
uint32_t td5_inputscript_lookup_action(const char *name);

#endif /* TD5_INPUTSCRIPT_H */
