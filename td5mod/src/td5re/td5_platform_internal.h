/* td5_platform_internal.h -- PRIVATE seam between td5_platform_win32.c and
 * td5_platform_win32_fs.c (C9 module split, see REFACTOR_PLAN.md).
 *
 * Not a public API -- only the td5_platform_win32*.c TUs include this.
 * Declarations here must match td5_platform_win32.c's definitions
 * verbatim (same rule as td5_physics_internal.h / td5_ai_internal.h /
 * td5_track_internal.h).
 */

#ifndef TD5_PLATFORM_INTERNAL_H
#define TD5_PLATFORM_INTERNAL_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

extern HWND   s_hwnd;
extern HANDLE s_game_heap;

#endif /* TD5_PLATFORM_INTERNAL_H */
