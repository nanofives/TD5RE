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

/* Window/timing statics (C9 split, fourth slice) -- td5_platform_win32.c's
 * td5_platform_win32_init() still seeds these and wires up the WndProc
 * subclass, but their definitions live in td5_platform_win32_window.c. */
typedef struct WrapperSurface WrapperSurface;

extern WrapperSurface *s_primary;
extern int             s_window_w;
extern int             s_window_h;
extern int             s_window_bpp;
extern int             s_fullscreen;
extern int             s_window_mode;
extern int             s_chosen_w;
extern int             s_chosen_h;
extern WNDPROC          s_original_wndproc;

LRESULT CALLBACK TD5_WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
HICON td5_load_app_icon(int cx, int cy);

/* TD5_WndProc's own queues/latches -- read by td5_plat_input_poll() and
 * friends in td5_platform_win32.c's Input section (not part of this slice). */
#define TD5_CHAR_QUEUE_SIZE 128
#define TD5_NAV_QUEUE_SIZE 64

extern uint32_t              s_mouse_click_latch;
extern volatile unsigned char s_char_queue[TD5_CHAR_QUEUE_SIZE];
extern volatile int           s_char_q_head;
extern volatile int           s_char_q_tail;
extern volatile unsigned char s_nav_queue[TD5_NAV_QUEUE_SIZE];
extern volatile int           s_nav_q_head;
extern volatile int           s_nav_q_tail;
extern volatile int           s_esc_latch;
extern volatile int           s_devices_dirty;

/* Rendering-backend statics (texture-page cache + per-frame stats) --
 * td5_plat_present()/td5_plat_present_texture_page() in
 * td5_platform_win32_window.c read these; they're still owned/written by
 * td5_platform_win32.c's Rendering Backend section. */
#define MAX_TEXTURE_PAGES 1024

extern WrapperSurface *s_tex_surfaces[MAX_TEXTURE_PAGES];
extern DWORD            s_tex_handles[MAX_TEXTURE_PAGES];
extern int               s_frame_draw_calls;
extern int               s_frame_vertices;
extern int               s_frame_indices;
extern int               s_last_bound_texture_page;

/* [DEVICE-LOST recovery] Rebuild every registered texture-page surface on the
 * freshly recreated device and refresh the cached SRV handles. Called from the
 * present path (td5_platform_win32_window.c) after Backend_RecreateDevice. */
void td5_plat_render_recover_textures(void);

#endif /* TD5_PLATFORM_INTERNAL_H */
