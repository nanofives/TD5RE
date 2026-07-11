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
#include <mmsystem.h>
#include <dsound.h>
#include <stdint.h>

extern HWND   s_hwnd;
extern HANDLE s_game_heap;

/* Audio-device statics (C9 split, third slice) -- td5_platform_win32.c's
 * td5_platform_win32_init() resets these alongside its other module state,
 * but their definitions live in td5_platform_win32_audio.c. */
#define MAX_AUDIO_BUFFERS  256
#define MAX_AUDIO_CHANNELS 64

extern LPDIRECTSOUNDBUFFER s_ds_buffers[MAX_AUDIO_BUFFERS];
extern LPDIRECTSOUNDBUFFER s_ds_channels[MAX_AUDIO_CHANNELS];
extern uint32_t            s_ds_channel_gen[MAX_AUDIO_CHANNELS];
extern uint32_t            s_ds_channel_serial[MAX_AUDIO_CHANNELS];
extern int                 s_ds_channel_loop[MAX_AUDIO_CHANNELS];
extern DWORD                s_ds_buffer_rates[MAX_AUDIO_BUFFERS];
extern uint32_t             s_audio_alloc_serial;
extern uint32_t             s_audio_alloc_count;
extern uint32_t             s_audio_free_count;
extern uint32_t             s_audio_steal_count;
extern uint32_t             s_audio_fail_count;
extern int                  s_audio_active_count;
extern int                  s_audio_peak_active;


#endif /* TD5_PLATFORM_INTERNAL_H */
