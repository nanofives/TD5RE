/**
 * td5_frontend.h -- Menu screens, screen table, navigation
 *
 * 30-entry screen function table at 0x4655C4. Each screen is a state machine
 * with inner states driven by g_frontendInnerState. Navigation via SetFrontendScreen().
 *
 * Original functions (30 screen entries):
 *   0x4269D0  [0]  ScreenLocalizationInit
 *   0x415030  [1]  ScreenPositionerDebugTool (unreachable)
 *   0x4275A0  [2]  RunAttractModeDemoScreen
 *   0x427290  [3]  ScreenLanguageSelect
 *   0x4274A0  [4]  ScreenLegalCopyright
 *   0x415490  [5]  ScreenMainMenuAnd1PRaceFlow
 *   0x4168B0  [6]  RaceTypeCategoryMenuStateMachine
 *   0x4213D0  [7]  ScreenQuickRaceMenu
 *   0x418D50  [8]  RunFrontendConnectionBrowser
 *   0x419CF0  [9]  RunFrontendSessionPicker
 *   0x41A7B0  [10] RunFrontendCreateSessionFlow
 *   0x41C330  [11] RunFrontendNetworkLobby
 *   0x41D890  [12] ScreenOptionsHub
 *   0x41F990  [13] ScreenGameOptions
 *   0x41DF20  [14] ScreenControlOptions
 *   0x41EA90  [15] ScreenSoundOptions
 *   0x420400  [16] ScreenDisplayOptions
 *   0x420C70  [17] ScreenTwoPlayerOptions
 *   0x40FE00  [18] ScreenControllerBindingPage
 *   0x418460  [19] ScreenMusicTestExtras
 *   0x40DFC0  [20] CarSelectionScreenStateMachine
 *   0x427630  [21] TrackSelectionScreenStateMachine
 *   0x417D50  [22] ScreenExtrasGallery
 *   0x413580  [23] ScreenPostRaceHighScoreTable
 *   0x422480  [24] RunRaceResultsScreen
 *   0x413BC0  [25] ScreenPostRaceNameEntry
 *   0x4237F0  [26] ScreenCupFailedDialog
 *   0x423A80  [27] ScreenCupWonDialog
 *   0x415370  [28] ScreenStartupInit
 *   0x41D630  [29] ScreenSessionLockedDialog
 */

#ifndef TD5_FRONTEND_H
#define TD5_FRONTEND_H

#include "td5_types.h"

/* --- Module lifecycle --- */
int  td5_frontend_init(void);
void td5_frontend_shutdown(void);
void td5_frontend_tick(void);

/* --- Screen navigation --- */
void td5_frontend_set_screen(TD5_ScreenIndex index);
void td5_frontend_return_to_lobby(void);
void td5_frontend_leave_net_session(void);
/* [2026-06-19] Show the "CONNECTION LOST" notice (then return to the main menu)
 * after a netplay session drops -- host timeout, host quit/DXPDISCONNECT, or a
 * mid-race lockstep loss. Pass a short human-readable reason (or NULL). */
void td5_frontend_show_net_disconnect(const char *reason);
TD5_ScreenIndex td5_frontend_get_screen(void);

/* --- Test-harness hooks (StartScreen nav walker / scripted-input engine) ---
 * force_input: acquire(1)/release(0) — treat the window as focused so injected
 * input isn't flushed while a test window runs in the background.
 * ready: current screen has settled and has at least one clickable button.
 * select: move keyboard focus to button `index` (an injected ENTER then
 * confirms it through the real input path). Returns 1 on success. */
void td5_frontend_harness_force_input(int on);
int  td5_frontend_harness_ready(void);
int  td5_frontend_harness_select(int index);

/* --- Display loop (called from game tick in MENU state) --- */
int  td5_frontend_display_loop(void);

/* --- Resource management --- */
int  td5_frontend_init_resources(void);
void td5_frontend_release_resources(void);

/* --- Auto-race (skip frontend, launch from INI settings) --- */
void td5_frontend_auto_race_setup(void);

/* --- MP split-screen position select (#6): cell -> actor slot to show there.
 * Returns -1 when positions are not active (knob off / not assigned / not the
 * local MP race), so the InitRace viewport map keeps the identity default. --- */
int  td5_frontend_mp_view_actor_slot(int cell);

/* --- Per-local-player menu transmission choice (#2): 1 = MANUAL, 0 = AUTO.
 * Used by td5_input.c to put a car into manual when the menu selects it
 * (MP uses s_mp_player_trans[player]; single-player uses s_selected_transmission). --- */
int  td5_frontend_get_player_manual(int player);

/* --- Per-local-player LANE ASSIST choice: 1 = on, 0 = off. MP: car-select grid
 * per-player; single-player: [Input] LaneAssist (Game Options). Read at race
 * start to seed td5_laneassist's per-player enable. --- */
int  td5_frontend_get_player_laneassist(int player);

/* --- UI rendering helpers --- */
void td5_frontend_render_ui_rects(void);
void td5_frontend_flush_sprite_blits(void);

#ifndef TD5RE_RELEASE
/* --- Self-test director queries (td5_selftest.c; dev builds only) --- */
/* 1 once the current screen's slide-in animation has completed. */
int  td5_frontend_selftest_settled(void);
/* Number of active+enabled (navigable) buttons on the current screen. */
int  td5_frontend_selftest_button_count(void);
/* Result of the last nav-reachability selftest run (TD5RE_NAV_SELFTEST):
 * which screen it ran on, how many navigable buttons it reached vs total.
 * *screen = -1 if it has not run yet. Any out-param may be NULL. */
void td5_frontend_selftest_nav_result(int *screen, int *reached, int *navigable);
#endif

#endif /* TD5_FRONTEND_H */
