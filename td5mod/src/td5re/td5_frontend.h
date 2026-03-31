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
TD5_ScreenIndex td5_frontend_get_screen(void);

/* --- Display loop (called from game tick in MENU state) --- */
int  td5_frontend_display_loop(void);

/* --- Resource management --- */
int  td5_frontend_init_resources(void);
void td5_frontend_release_resources(void);

/* --- UI rendering helpers --- */
void td5_frontend_render_ui_rects(void);
void td5_frontend_flush_sprite_blits(void);

#endif /* TD5_FRONTEND_H */
