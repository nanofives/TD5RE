/* ========================================================================
 * td5_control.h — live-control MCP transport (PORT-ONLY, DEV-ONLY)
 *
 * A tiny opt-in UDP command server that lets an external process (the
 * Python MCP server in scripts/td5re_mcp) drive a RUNNING td5re.exe:
 * launch/abort races, jump/read frontend screens, get/set whitelisted
 * INI knobs, inject input, take screenshots (host side), and query state.
 *
 * It reuses the game's proven in-process control verbs (the same
 * g_td5.ini + auto_race race-launch recipe the self-test director uses,
 * td5_frontend_set_screen/get_screen, td5_plat_input_inject_key). The
 * ONLY thing this module adds is the transport.
 *
 * THREADING: a dedicated listener thread does recvfrom + copies each
 * datagram into a mutex-guarded ring. td5_control_tick() — called at the
 * top of td5_game_tick() — is the ONLY place that parses/executes commands
 * and touches game state, so every game/frontend call stays main-thread.
 *
 * Enable with [Control] Enabled=1 (or --Control=1). Default OFF: a normal
 * dev launch never opens a socket. Whole module compiled out under
 * TD5RE_RELEASE (no control surface ships).
 * ======================================================================== */
#ifndef TD5_CONTROL_H
#define TD5_CONTROL_H

#include <stdint.h>

#ifndef TD5RE_RELEASE

/* Open the control socket + start the listener thread. No-op unless
 * [Control] Enabled=1. Call once from WinMain after INI + CLI config is
 * final (beside td5_selftest_boot). Safe to call when disabled. */
void td5_control_init(void);

/* Close the socket + join the listener thread. Call once at shutdown. */
void td5_control_shutdown(void);

/* Frame hook — call at the top of td5_game_tick(). Drains queued commands
 * on the main thread and sends replies. Inert when the socket is closed. */
void td5_control_tick(void);

/* Dependency inversion (S7 pattern): the control module cannot call the
 * race-abort mutator directly without joining the td5_game.h includer set.
 * Instead td5_control_tick() latches an end-race request when the `end_race`
 * command is drained, and td5_game_tick() (which owns the mutator) polls
 * this and performs the abort. Returns 1 exactly once per queued request. */
int td5_control_take_end_race_request(void);

/* Currently-held race action bits for a slot (hold_action/release_action
 * verbs), OR'd over the polled hardware word in td5_input_poll_race_session
 * exactly like td5_inputscript_race_bits. Returns 0 when the control server
 * is disabled or nothing is held — the sim path is unperturbed unless a
 * client actively holds an action. */
uint32_t td5_control_race_bits(int slot);

#else /* TD5RE_RELEASE: whole module compiled out */

#define td5_control_init()                  ((void)0)
#define td5_control_shutdown()              ((void)0)
#define td5_control_tick()                  ((void)0)
#define td5_control_take_end_race_request() 0
#define td5_control_race_bits(slot)         0u

#endif /* TD5RE_RELEASE */

#endif /* TD5_CONTROL_H */
