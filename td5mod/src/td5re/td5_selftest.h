/* ========================================================================
 * td5_selftest.h — in-session automated test suite (PORT-ONLY, DEV-ONLY)
 *
 * A frame-ticked "test director" that, inside ONE td5re.exe session, walks
 * the frontend screens, runs a matrix of races with varied parameters
 * (tracks incl. TD6 conversions, circuit, reverse, arcade/sim, traffic,
 * cops, split-screen spectate panes), and monitors degradation (working
 * set, private bytes, GDI/USER objects, handles, heap alloc balance,
 * frame times) across repeated identical races. Emits a machine-readable
 * report (log/selftest_report.csv + .md) and a nonzero process exit code
 * on failure so scripts/CI can gate on it.
 *
 * Enable with [SelfTest] Enabled=1 (or --SelfTest=1). Suite: 0=smoke,
 * 1=full (--SelfTestSuite=N). Compiled out of the release build.
 * ======================================================================== */
#ifndef TD5_SELFTEST_H
#define TD5_SELFTEST_H

#ifndef TD5RE_RELEASE

/* Called once from WinMain after INI + CLI config is final and (when active)
 * before any subsystem boots: forces the harness baseline knobs (SkipIntro,
 * logging, muted SFX, debug overlay) so a bare --SelfTest=1 is complete. */
void td5_selftest_boot(void);

/* Frame hook — first statement of td5_game_tick(). Inert when not active. */
void td5_selftest_tick(void);

/* 1 while the suite is driving the session. */
int td5_selftest_active(void);

/* Process exit code: 0 = suite passed or never ran, 1 = any step failed. */
int td5_selftest_exit_code(void);

#else /* TD5RE_RELEASE: whole module compiled out */

#define td5_selftest_boot()      ((void)0)
#define td5_selftest_tick()      ((void)0)
#define td5_selftest_active()    0
#define td5_selftest_exit_code() 0

#endif /* TD5RE_RELEASE */

#endif /* TD5_SELFTEST_H */
