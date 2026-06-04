/**
 * td5_platform_win32.c -- Win32 platform layer for TD5RE
 *
 * Bridges the TD5RE source port game logic to the D3D11 ddraw wrapper
 * and Windows APIs. Implements all td5_plat_* functions declared in
 * td5_platform.h.
 *
 * The rendering functions call directly into the D3D11 wrapper's internal
 * functions and the global g_backend state, NOT through COM vtables.
 * The wrapper objects (WrapperDevice, WrapperSurface, etc.) are received
 * during initialization via td5_platform_win32_init().
 */

#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <mmsystem.h>
#include <dsound.h>
#include <dinput.h>
#include <d3d11.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <malloc.h>
#include <zlib.h>   /* frame-dump PNG encoder (dev tool, see td5_plat_dump_frame_png) */
#include <math.h>

#include "td5_platform.h"

/* Pull in the wrapper types and backend access */
#include "../../ddraw_wrapper/src/wrapper.h"

#define LOG_TAG "platform"

/* ========================================================================
 * Module-level state
 * ======================================================================== */

/* COM interface pointers received from the wrapper at init time */
static WrapperDirectDraw   *s_ddraw4        = NULL;
static WrapperDevice       *s_d3ddevice3    = NULL;
static WrapperSurface      *s_primary       = NULL;

/* Window */
static HWND     s_hwnd          = NULL;
static int      s_window_w      = 640;
static int      s_window_h      = 480;
static int      s_window_bpp    = 16;
static int      s_fullscreen    = 0;
/* [S01 Display options 2026-06-04] 3-way window mode + the user's chosen
 * windowed/fullscreen resolution. Borderless overrides the render size to the
 * desktop; windowed and exclusive-fullscreen render at the chosen resolution. */
static int      s_window_mode   = 1;   /* 0=fullscreen exclusive, 1=windowed, 2=borderless */
static int      s_chosen_w      = 0;   /* last resolution picked in Display options */
static int      s_chosen_h      = 0;
/* Re-entrancy guard so the WM_SIZE-driven live resize ignores our own
 * programmatic SetWindowPos inside td5_plat_set_window_mode (which already
 * resizes the swap chain explicitly). */
static int      s_suppress_wm_size = 0;
static int      plat_resize_native(int width, int height, int bpp, int do_swap_reset);

/* Timing */
static LARGE_INTEGER s_qpc_freq;
static int           s_qpc_inited = 0;

/* Window subclass */
static WNDPROC s_original_wndproc = NULL;
static LRESULT CALLBACK TD5_WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

/* Input */
static uint8_t  s_keyboard[256];

/* Rebindable keyboard scancode table, per player, in the canonical action
 * order used by the control-config screen / original g_keyboardScanCodeTable:
 *   0=LEFT 1=RIGHT 2=ACCELERATE 3=BRAKE 4=HANDBRAKE 5=HORN/SIREN
 *   6=GEAR UP 7=GEAR DOWN 8=CHANGE VIEW 9=REAR VIEW
 * P1 defaults are the original's [CONFIRMED @ g_keyboardScanCodeTable 0x00464054
 *   = {cb cd c8 d0 10 9d 1e 2c 14 2d}].
 * P2 defaults preserve the port's split-screen WASD layout (the original's P2
 * keyboard defaults were not RE'd, so this is the non-regression choice).
 * The config screen pushes rebound codes here via td5_plat_input_set_keyboard_bindings
 * so a rebind actually takes effect in-race instead of being a write-only dead end. */
/* [PORT ENHANCEMENT 2026-06] slot 10 = PAUSE (default DIK_P 0x19) so the pause
 * action is rebindable alongside the rest. */
enum { TD5_KB_BIND_COUNT = 11 };
static uint8_t s_kb_bindings[2][TD5_KB_BIND_COUNT] = {
    /* P1: LEFT  RIGHT ACCEL BRAKE HBRK  HORN  GUP   GDN   VIEW  REAR  PAUSE */
    {     0xCB, 0xCD, 0xC8, 0xD0, 0x10, 0x9D, 0x1E, 0x2C, 0x14, 0x2D, 0x19 },
    /* P2:   A     D     W     S    CAPS  TAB    E     Q     R     F   PAUSE */
    {     0x1E, 0x20, 0x11, 0x1F, 0x3A, 0x0F, 0x12, 0x10, 0x13, 0x21, 0x19 },
};

static int      s_mouse_dx, s_mouse_dy;
static uint32_t s_mouse_buttons;
static LPDIRECTINPUT8A       s_dinput        = NULL;
static LPDIRECTINPUTDEVICE8A s_di_keyboard   = NULL;
static LPDIRECTINPUTDEVICE8A s_di_mouse      = NULL;
/* Per-player (slot) joystick devices. slot N = player N+1.
 * [PORT ENHANCEMENT 2026-06] Raised from 2 to 9 for N-way split-screen. The
 * ORIGINAL data model already enumerated 7 joystick slots with the source index
 * masked &7 (8 sources incl. keyboard) [CONFIRMED @ ScreenLocalizationInit
 * 0x004269D0 loop i<7; ScreenControlOptions 0x41DF20 &7]; the port had narrowed
 * this to 2. Each race player binds to the keyboard or one of the joysticks. */
#define TD5_PLAT_MAX_JS_SLOTS 9
static LPDIRECTINPUTDEVICE8A s_di_joystick[TD5_PLAT_MAX_JS_SLOTS] = { NULL };
/* 9-slot binding table per player, mirroring the original DAT_00463FC4 row:
 *   [0]=active flag, [1]/[2]=axis assignment (4=X-like, 5=Y-like, swappable),
 *   [3..8]=6 button actions (value 2..10 selects physical button = value-2).
 * s_joystick_bound[slot]==0 → unconfigured → GetJS-default button mapping. */
static int32_t s_joystick_bindings[TD5_PLAT_MAX_JS_SLOTS][9];
static int     s_joystick_bound[TD5_PLAT_MAX_JS_SLOTS] = { 0 };
/* [PORT ENHANCEMENT 2026-06] per-action binding codes (button | axis+dir) for
 * each player; when *_set, the in-race poll maps actions through these instead
 * of the fixed X=steer/Y=throttle/buttons default. See TD5_JSBIND_* in the .h. */
static uint32_t s_js_action_bind[TD5_PLAT_MAX_JS_SLOTS][TD5_JSBIND_ACTIONS];
static int      s_js_action_set[TD5_PLAT_MAX_JS_SLOTS] = { 0 };
/* Capture baseline (one remap active at a time). */
static long     s_js_cap_axis_base[8];
static uint32_t s_js_cap_btn_base;
static int      s_js_cap_slot = -1;
/* [PORT ENHANCEMENT 2026-06] dedicated non-exclusive joystick for FRONTEND
 * navigation (A=select, B=back, dpad/stick=move). Lazily created from the first
 * enumerated joystick when no per-player device exists; once a per-player device
 * is created, navigation reads that instead and this one is released. */
static LPDIRECTINPUTDEVICE8A s_di_fe_joystick = NULL;
static int      s_fe_js_suppress = 0;
/* [PORT ENHANCEMENT 2026-06] non-exclusive handles for the multiplayer lobby's
 * "press to join" scan across every enumerated joystick. */
static LPDIRECTINPUTDEVICE8A s_di_scan[16];
/* GetJS axis center (0xFA=250) and max tracked buttons (10); kept local so the
 * platform layer doesn't depend on td5_input.h (mirrors TD5_INPUT_JS_AXIS_CENTER
 * / TD5_INPUT_MAX_JS_BUTTONS). */
#define TD5_PLAT_JS_AXIS_CENTER 0xFA
#define TD5_PLAT_JS_MAX_BUTTONS 10
static char s_device_names[16][256];
static int  s_device_types[16];  /* 0=keyboard, 1=gamepad, 2=wheel/joystick */
static GUID s_device_guids[16];  /* DirectInput instance GUID per enumerated device (index 0 = keyboard, unused) */
static int  s_device_count = 0;

/* Multi-file logging — messages routed by module tag to separate log files.
 * Session rotation keeps up to 5 previous logs per category. */
#define TD5_LOG_CAT_FRONTEND 0
#define TD5_LOG_CAT_RACE     1
#define TD5_LOG_CAT_ENGINE   2
#define TD5_LOG_CAT_COUNT    3
#define TD5_LOG_MAX_SESSIONS 5

static const char *s_log_cat_names[TD5_LOG_CAT_COUNT] = {
    "frontend", "race", "engine"
};

typedef struct TD5_LogFile {
    HANDLE handle;
    char   buffer[4096];
    size_t buffer_used;
    unsigned int line_count;
} TD5_LogFile;

static TD5_LogFile s_log_files[TD5_LOG_CAT_COUNT];
static char s_log_dir[512];
static int  s_log_initialized = 0;

/* Runtime log filters (set via td5_plat_log_set_filters from main.c after INI
 * parse). Defaults match the historic behavior: master on, INFO and above,
 * all three categories enabled. */
static int          s_log_enabled   = 1;
static int          s_log_min_level = TD5_LOG_INFO;
static unsigned int s_log_cat_mask  = TD5_LOG_CAT_BIT_FRONTEND |
                                      TD5_LOG_CAT_BIT_RACE     |
                                      TD5_LOG_CAT_BIT_ENGINE;

/* Audio */
#define MAX_AUDIO_BUFFERS  256
#define MAX_AUDIO_CHANNELS 64

static LPDIRECTSOUND8          s_dsound      = NULL;
static LPDIRECTSOUNDBUFFER     s_ds_primary  = NULL;
/* Continuously-looping silent buffer that keeps the audio device/stream awake.
 * Without it the device idles when no sound is playing (as in the menus, where
 * SFX are sparse); the next sound then loses its first ~0.1-0.2s to wake-up
 * ramp, which entirely swallows short one-shot UI sounds (ping/whoosh). Kept
 * playing for the whole session so menu one-shots fire instantly. */
static LPDIRECTSOUNDBUFFER     s_ds_keepalive = NULL;
static LPDIRECTSOUNDBUFFER     s_ds_buffers[MAX_AUDIO_BUFFERS];
static LPDIRECTSOUNDBUFFER     s_ds_channels[MAX_AUDIO_CHANNELS];
static int                     s_ds_channel_buf[MAX_AUDIO_CHANNELS]; /* buffer index per channel */

/* Channel-handle generation/recycle bookkeeping (S11 sound-overload fix).
 *
 * A channel handle returned by td5_plat_audio_play is no longer the bare index
 * `i`. It is `(generation << AUDIO_CHANNEL_INDEX_BITS) | i`, where the generation
 * is bumped every time index `i` is (re)allocated. The TD5 sound module keeps a
 * slot->channel table (s_slot_to_channel) that is freely overwritten and can go
 * stale when a channel is recycled or stolen for a different sound. Without a
 * generation tag, a stale handle would resolve to whatever sound now occupies
 * that index — so a Stop()/Modify() meant for one slot would hit another's
 * voice (cross-talk), and in the worst case the whole mix could lock onto a
 * single stuck source. The generation lets td5_plat_audio_{stop,modify,is_playing}
 * detect a stale handle (gen mismatch) and treat it as a no-op. */
#define AUDIO_CHANNEL_INDEX_BITS 8
#define AUDIO_CHANNEL_INDEX_MASK 0xFF
#define AUDIO_CHANNEL_GEN_MAX    0x7FFFFF /* keep encoded handle within int >= 0 */
static uint32_t                s_ds_channel_gen[MAX_AUDIO_CHANNELS];    /* gen per index */
static uint32_t                s_ds_channel_serial[MAX_AUDIO_CHANNELS]; /* alloc order (steal pick) */
static int                     s_ds_channel_loop[MAX_AUDIO_CHANNELS];   /* 1 = looping (steal last) */
static uint32_t                s_audio_alloc_serial = 0;                /* monotonic alloc counter */

/* Voice instrumentation (S11): confirm no monotonic leak and that counts return
 * to baseline between races. Active = currently-allocated DirectSound voices
 * (DuplicateSoundBuffer instances), excluding the primary/keepalive buffers. */
static uint32_t                s_audio_alloc_count  = 0; /* total successful Play allocations */
static uint32_t                s_audio_free_count   = 0; /* total channel releases */
static uint32_t                s_audio_steal_count  = 0; /* Play allocations that stole a busy voice */
static uint32_t                s_audio_fail_count   = 0; /* Play attempts that could not allocate */
static int                     s_audio_active_count = 0; /* live voices right now */
static int                     s_audio_peak_active  = 0; /* high-water mark */

static DWORD                   s_ds_buffer_rates[MAX_AUDIO_BUFFERS];
static int                     s_audio_buf_count = 0;
static int                     s_master_volume   = 40;
/* When set, all SFX play/modify is forced to silence (e.g. while the in-race
 * pause menu is up). Looping buffers keep running so resume is click-free; the
 * per-frame audio mix simply re-applies volume 0 until unmuted. Music (CD) uses
 * a separate path and is unaffected. */
static int                     s_audio_muted     = 0;

#define TD5_STREAM_BUFFER_BYTES 0x81600u

typedef struct TD5_StreamAudioState {
    FILE                *fp;
    LPDIRECTSOUNDBUFFER  buffer;
    WAVEFORMATEX         wfx;
    DWORD                data_offset;
    DWORD                data_size;
    DWORD                data_cursor;
    DWORD                buffer_bytes;
    DWORD                half_bytes;
    DWORD                last_play_cursor;
    int                  loop;
    int                  eof;
    int                  active;
} TD5_StreamAudioState;

static TD5_StreamAudioState s_audio_stream;
/* Per-device force-feedback state. [PORT ENHANCEMENT 2026-06] grown from a
 * single device to one entry per joystick slot so each player's wheel/pad can
 * vibrate independently. Devices without FF leave their entry zeroed (no-op). */
static TD5_FFState          s_ff[TD5_PLAT_MAX_JS_SLOTS];

/* Rendering: texture pages */
#define MAX_TEXTURE_PAGES 1024
#define TD5_SHARED_FONT_PAGE 898
static WrapperSurface  *s_tex_surfaces[MAX_TEXTURE_PAGES];
static WrapperTexture  *s_tex_wrappers[MAX_TEXTURE_PAGES];
static DWORD            s_tex_handles[MAX_TEXTURE_PAGES];
static uint16_t         s_tex_widths[MAX_TEXTURE_PAGES];
static uint16_t         s_tex_heights[MAX_TEXTURE_PAGES];
static int              s_tex_page_count = 0;
static int              s_frame_draw_calls = 0;
static int              s_frame_vertices = 0;
static int              s_frame_indices = 0;
static int              s_last_bound_texture_page = -1;

/* Heap */
static HANDLE s_game_heap = NULL;

/* ========================================================================
 * Initialization -- called by the wrapper/loader after device creation
 * ======================================================================== */

void td5_platform_win32_init(void *ddraw4, void *d3ddevice3, void *primary_surface)
{
    s_ddraw4     = (WrapperDirectDraw *)ddraw4;
    s_d3ddevice3 = (WrapperDevice *)d3ddevice3;
    s_primary    = (WrapperSurface *)primary_surface;

    /* In standalone mode g_backend.hwnd is NULL (to avoid message forwarding
     * loops in DisplayWindowProc). Use the backend display window directly
     * so DirectSound and DirectInput get a valid cooperative-level HWND. */
    s_hwnd = g_backend.hwnd ? g_backend.hwnd : Backend_GetDisplayWindow();
    s_window_w   = g_backend.width;
    s_window_h   = g_backend.height;
    s_window_bpp = g_backend.bpp;
    s_fullscreen = !g_backend.windowed;
    /* Seed the chosen resolution + window mode from the boot state; main.c
     * applies the persisted [Display] WindowMode right after this returns. */
    s_chosen_w    = s_window_w;
    s_chosen_h    = s_window_h;
    s_window_mode = s_fullscreen ? 0 : 1;

    /* Subclass the wrapper's window to handle WM_CLOSE and hide system cursor */
    if (s_hwnd) {
        s_original_wndproc = (WNDPROC)SetWindowLongPtrA(s_hwnd, GWLP_WNDPROC,
                                                         (LONG_PTR)TD5_WndProc);

        /* Set window icon (use first resource icon, fall back to default app icon) */
        HICON hIcon = LoadIconA(GetModuleHandleA(NULL), MAKEINTRESOURCE(1));
        if (!hIcon) hIcon = LoadIconA(NULL, IDI_APPLICATION);
        if (hIcon) {
            SendMessageA(s_hwnd, WM_SETICON, ICON_BIG,   (LPARAM)hIcon);
            SendMessageA(s_hwnd, WM_SETICON, ICON_SMALL, (LPARAM)hIcon);
        }

        /* Windows 11 dark mode title bar */
        typedef HRESULT (WINAPI *PFN_DwmSetWindowAttribute)(HWND, DWORD, LPCVOID, DWORD);
        HMODULE hDwm = LoadLibraryA("dwmapi.dll");
        if (hDwm) {
            PFN_DwmSetWindowAttribute pDwm = (PFN_DwmSetWindowAttribute)
                GetProcAddress(hDwm, "DwmSetWindowAttribute");
            if (pDwm) {
                BOOL dark = TRUE;
                pDwm(s_hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
            }
            /* Keep dwmapi.dll loaded for the lifetime of the app */
        }
    }

    /* Init QPC frequency */
    QueryPerformanceFrequency(&s_qpc_freq);
    s_qpc_inited = 1;

    /* Create game heap */
    if (!s_game_heap)
        s_game_heap = HeapCreate(0, 1024 * 1024, 0); /* growable, 1 MB initial */

    memset(s_keyboard, 0, sizeof(s_keyboard));
    memset(s_tex_surfaces, 0, sizeof(s_tex_surfaces));
    memset(s_tex_wrappers, 0, sizeof(s_tex_wrappers));
    memset(s_tex_handles, 0, sizeof(s_tex_handles));
    memset(s_tex_widths, 0, sizeof(s_tex_widths));
    memset(s_tex_heights, 0, sizeof(s_tex_heights));
    memset(s_ds_buffers, 0, sizeof(s_ds_buffers));
    memset(s_ds_channels, 0, sizeof(s_ds_channels));
    memset(s_ds_channel_gen, 0, sizeof(s_ds_channel_gen));
    memset(s_ds_channel_serial, 0, sizeof(s_ds_channel_serial));
    memset(s_ds_channel_loop, 0, sizeof(s_ds_channel_loop));
    memset(s_ds_buffer_rates, 0, sizeof(s_ds_buffer_rates));
    s_audio_alloc_serial = 0;
    s_audio_alloc_count = s_audio_free_count = s_audio_steal_count = s_audio_fail_count = 0;
    s_audio_active_count = s_audio_peak_active = 0;
}

/* ========================================================================
 * Window / Display
 * ======================================================================== */

static uint32_t s_mouse_click_latch = 0; /* sticky per-button click flags */

static LRESULT CALLBACK TD5_WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CLOSE:
        PostQuitMessage(0);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_SYSKEYDOWN:
        /* Alt+F4 — explicit handler so it always closes even if original proc drops it */
        if (wParam == VK_F4) {
            PostQuitMessage(0);
            return 0;
        }
        break;
    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT) {
            SetCursor(NULL);
            return TRUE;
        }
        break;
    case WM_SIZE:
        /* [S01 2026-06-04] Live window resize (drag the sizing border / maximize)
         * in windowed mode: match the swap chain, backbuffer and render dims to
         * the new client area. Skip when minimized, during our own programmatic
         * resizes (s_suppress_wm_size), and in borderless / exclusive-fullscreen
         * (those own their size). */
        if (wParam != SIZE_MINIMIZED && !s_suppress_wm_size && s_window_mode == 1) {
            int cw = (int)LOWORD(lParam);
            int ch = (int)HIWORD(lParam);
            if (cw > 0 && ch > 0 && (cw != s_window_w || ch != s_window_h)) {
                s_chosen_w = cw;   /* keep the resolution selector + manual size in sync */
                s_chosen_h = ch;
                plat_resize_native(cw, ch, s_window_bpp, 1);
            }
        }
        break;
    /* Latch mouse clicks so a quick click released within one 33ms frame is not missed */
    case WM_LBUTTONDOWN: s_mouse_click_latch |= 1; break;
    case WM_RBUTTONDOWN: s_mouse_click_latch |= 2; break;
    case WM_MBUTTONDOWN: s_mouse_click_latch |= 4; break;
    }
    if (s_original_wndproc)
        return CallWindowProcA(s_original_wndproc, hwnd, msg, wParam, lParam);
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

int td5_plat_window_create(const char *title, const TD5_DisplayMode *mode)
{
    HWND existing;
    int w, h;

    if (!mode) return 0;

    w = mode->width;
    h = mode->height;
    s_window_bpp = mode->bpp;
    s_fullscreen = mode->fullscreen;

    /* If the wrapper already created a display window, use it */
    existing = Backend_GetDisplayWindow();
    if (existing) {
        s_hwnd = existing;
        s_window_w = w;
        s_window_h = h;
        return 1;
    }

    /* Otherwise create via Backend_CreateDevice which creates the window */
    if (Backend_CreateDevice(NULL, w, h, mode->bpp, !mode->fullscreen)) {
        s_hwnd = g_backend.hwnd;
        s_window_w = g_backend.width;
        s_window_h = g_backend.height;
        return 1;
    }

    /* Fallback: create a plain Win32 window */
    {
        WNDCLASSEXA wc;
        DWORD style;
        RECT wr;

        HICON hIcon = LoadIconA(GetModuleHandleA(NULL), MAKEINTRESOURCE(1));
        if (!hIcon) hIcon = LoadIconA(NULL, IDI_APPLICATION);

        ZeroMemory(&wc, sizeof(wc));
        wc.cbSize        = sizeof(wc);
        wc.style         = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc   = TD5_WndProc;
        wc.hInstance      = GetModuleHandleA(NULL);
        wc.hIcon          = hIcon;
        wc.hIconSm        = hIcon;
        wc.hCursor        = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground  = (HBRUSH)GetStockObject(BLACK_BRUSH);
        wc.lpszClassName  = "TD5RE_Window";
        RegisterClassExA(&wc);

        style = mode->fullscreen
            ? (WS_POPUP | WS_VISIBLE)
            : (WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_VISIBLE);

        wr.left = 0; wr.top = 0;
        wr.right = w; wr.bottom = h;
        AdjustWindowRect(&wr, style, FALSE);

        {
            static char env_title[256];
            const char *win_title = title ? title : "TD5RE";
            DWORD n = GetEnvironmentVariableA("TD5RE_WINDOW_TITLE",
                                              env_title, sizeof(env_title));
            if (n > 0 && n < sizeof(env_title)) win_title = env_title;

            s_hwnd = CreateWindowExA(0, "TD5RE_Window",
                win_title,
                style,
                CW_USEDEFAULT, CW_USEDEFAULT,
                wr.right - wr.left, wr.bottom - wr.top,
                NULL, NULL, GetModuleHandleA(NULL), NULL);
        }

        if (!s_hwnd) return 0;
        if (hIcon) {
            SendMessageA(s_hwnd, WM_SETICON, ICON_BIG,   (LPARAM)hIcon);
            SendMessageA(s_hwnd, WM_SETICON, ICON_SMALL, (LPARAM)hIcon);
        }
        ShowCursor(FALSE);
        SetCursor(NULL);
        s_window_w = w;
        s_window_h = h;
    }

    return 1;
}

void td5_plat_window_destroy(void)
{
    if (s_hwnd) {
        DestroyWindow(s_hwnd);
        s_hwnd = NULL;
    }
}

int td5_plat_pump_messages(void)
{
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT)
            return 0;
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return 1;
}

/* ----------------------------------------------------------------------------
 * Frame dump (dev tool): copy the swap-chain backbuffer to a PNG so the rendered
 * frame can be inspected. OS-level capture of the D3D11 window does not work here
 * (PrintWindow returns black; CopyFromScreen is occlusion-prone), so this gives a
 * reliable visual ground-truth loop for frontend faithfulness work. Enabled by
 * setting env var TD5RE_FRAMEDUMP=<path>; the path is (re)written every 30 frames.
 * -------------------------------------------------------------------------- */
static ID3D11Texture2D *s_fd_staging = NULL;
static int s_fd_w = 0, s_fd_h = 0;

static void td5_png_chunk(FILE *f, const char *type, const unsigned char *data, unsigned len) {
    unsigned char be[4];
    uLong crc;
    be[0]=(unsigned char)(len>>24); be[1]=(unsigned char)(len>>16);
    be[2]=(unsigned char)(len>>8);  be[3]=(unsigned char)len;
    fwrite(be,1,4,f); fwrite(type,1,4,f);
    if (len && data) fwrite(data,1,len,f);
    crc = crc32(0,(const Bytef*)type,4);
    if (len && data) crc = crc32(crc,(const Bytef*)data,len);
    be[0]=(unsigned char)(crc>>24); be[1]=(unsigned char)(crc>>16);
    be[2]=(unsigned char)(crc>>8);  be[3]=(unsigned char)crc;
    fwrite(be,1,4,f);
}

static int td5_write_png_rgba(const char *path, const unsigned char *rgba, int w, int h) {
    FILE *f;
    unsigned char ihdr[13];
    unsigned char *raw, *comp;
    uLongf comp_len;
    unsigned long raw_len;
    int y, ok = 0;
    static const unsigned char sig[8] = {137,80,78,71,13,10,26,10};
    f = fopen(path,"wb"); if (!f) return 0;
    fwrite(sig,1,8,f);
    ihdr[0]=(unsigned char)(w>>24);ihdr[1]=(unsigned char)(w>>16);ihdr[2]=(unsigned char)(w>>8);ihdr[3]=(unsigned char)w;
    ihdr[4]=(unsigned char)(h>>24);ihdr[5]=(unsigned char)(h>>16);ihdr[6]=(unsigned char)(h>>8);ihdr[7]=(unsigned char)h;
    ihdr[8]=8; ihdr[9]=6; ihdr[10]=0; ihdr[11]=0; ihdr[12]=0;  /* 8-bit RGBA */
    td5_png_chunk(f,"IHDR",ihdr,13);
    raw_len = (unsigned long)h * (1u + (unsigned long)w*4u);
    raw = (unsigned char*)malloc(raw_len);
    if (raw) {
        for (y=0;y<h;y++){
            raw[(size_t)y*(1+w*4)] = 0;  /* filter type 0 (none) */
            memcpy(raw+(size_t)y*(1+w*4)+1, rgba+(size_t)y*w*4, (size_t)w*4);
        }
        comp_len = compressBound(raw_len);
        comp = (unsigned char*)malloc(comp_len);
        if (comp && compress2(comp,&comp_len,raw,raw_len,6)==Z_OK) {
            td5_png_chunk(f,"IDAT",comp,(unsigned)comp_len);
            td5_png_chunk(f,"IEND",NULL,0);
            ok = 1;
        }
        free(comp); free(raw);
    }
    fclose(f);
    return ok;
}

static void td5_plat_dump_frame_png(const char *path) {
    ID3D11Texture2D *bb = NULL;
    D3D11_TEXTURE2D_DESC d;
    D3D11_MAPPED_SUBRESOURCE m;
    if (!g_backend.swap_chain || !g_backend.device || !g_backend.context || !path) return;
    if (FAILED(IDXGISwapChain_GetBuffer(g_backend.swap_chain, 0, &IID_ID3D11Texture2D, (void**)&bb)) || !bb) return;
    ID3D11Texture2D_GetDesc(bb, &d);
    if (!s_fd_staging || s_fd_w != (int)d.Width || s_fd_h != (int)d.Height) {
        D3D11_TEXTURE2D_DESC sd = d;
        if (s_fd_staging) { ID3D11Texture2D_Release(s_fd_staging); s_fd_staging = NULL; }
        sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0;
        if (FAILED(ID3D11Device_CreateTexture2D(g_backend.device,&sd,NULL,&s_fd_staging))) {
            ID3D11Texture2D_Release(bb); return;
        }
        s_fd_w = (int)d.Width; s_fd_h = (int)d.Height;
    }
    ID3D11DeviceContext_CopyResource(g_backend.context,
        (ID3D11Resource*)s_fd_staging, (ID3D11Resource*)bb);
    if (SUCCEEDED(ID3D11DeviceContext_Map(g_backend.context,
            (ID3D11Resource*)s_fd_staging, 0, D3D11_MAP_READ, 0, &m))) {
        int w=(int)d.Width, h=(int)d.Height, x, yy;
        int bgra = (d.Format==DXGI_FORMAT_B8G8R8A8_UNORM ||
                    d.Format==DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);
        unsigned char *rgba = (unsigned char*)malloc((size_t)w*h*4);
        if (rgba) {
            for (yy=0;yy<h;yy++){
                const unsigned char *src=(const unsigned char*)m.pData + (size_t)yy*m.RowPitch;
                unsigned char *dst=rgba+(size_t)yy*w*4;
                for (x=0;x<w;x++){
                    unsigned char c0=src[x*4+0],c1=src[x*4+1],c2=src[x*4+2];
                    if (bgra){ dst[x*4+0]=c2; dst[x*4+1]=c1; dst[x*4+2]=c0; } /* BGRA->RGBA */
                    else     { dst[x*4+0]=c0; dst[x*4+1]=c1; dst[x*4+2]=c2; }
                    dst[x*4+3]=255;
                }
            }
            td5_write_png_rgba(path,rgba,w,h);
            free(rgba);
        }
        ID3D11DeviceContext_Unmap(g_backend.context,(ID3D11Resource*)s_fd_staging,0);
    }
    ID3D11Texture2D_Release(bb);
}

void td5_plat_present(int vsync)
{
    static int s_present_log_count = 0;
    float fallback_rgba[4] = { 0.20f, 0.32f, 0.46f, 1.0f };
    int empty_scene_fallback = 0;

    (void)vsync;

    /* Dev frame-dump: when TD5RE_FRAMEDUMP=<path> is set, write the swap-chain
     * backbuffer to that PNG every 30 frames (overwrite). Captures buffer 0 at
     * the top of present = the last presented frame; for a static frontend
     * screen this equals the current frame. Cheap no-op when unset. */
    {
        static int s_fd_init = 0, s_fd_on = 0, s_fd_count = 0, s_fd_seq = 0, s_fd_isseq = 0;
        static char s_fd_path[300];
        if (!s_fd_init) {
            const char *e = getenv("TD5RE_FRAMEDUMP");
            s_fd_init = 1;
            if (e && e[0]) {
                s_fd_on = 1; strncpy(s_fd_path, e, sizeof(s_fd_path)-1); s_fd_path[sizeof(s_fd_path)-1]='\0';
                /* If the path contains a '%', treat it as a printf format and write a
                 * numbered sequence (one PNG per dump tick) instead of overwriting. */
                s_fd_isseq = (strchr(s_fd_path, '%') != NULL);
            }
        }
        if (s_fd_on && (s_fd_count++ % 30) == 0) {
            if (s_fd_isseq) {
                char buf[320];
                snprintf(buf, sizeof(buf), s_fd_path, s_fd_seq++);
                td5_plat_dump_frame_png(buf);
            } else {
                td5_plat_dump_frame_png(s_fd_path);
            }
        }
    }

    if (s_present_log_count < 120 && g_backend.scene_rendered) {
        TD5_LOG_I("plat",
                  "present: scene=%d draws=%d verts=%d indices=%d tex=%d backbuffer_rtv=%p backbuffer_srv=%p swap_rtv=%p",
                  g_backend.scene_rendered,
                  s_frame_draw_calls,
                  s_frame_vertices,
                  s_frame_indices,
                  s_last_bound_texture_page,
                  g_backend.backbuffer ? g_backend.backbuffer->d3d11_rtv : NULL,
                  g_backend.backbuffer ? g_backend.backbuffer->d3d11_srv : NULL,
                  g_backend.swap_rtv);
        s_present_log_count++;
    }

    /* Race frames currently reach present with no submitted geometry because
     * higher-level world rendering is still stubbed. When that happens,
     * bypass the wrapper flip/composite path and clear the swap chain
     * directly so the window proves it is still rendering live frames. */
    if (g_backend.scene_rendered && s_frame_draw_calls == 0 &&
        g_backend.context && g_backend.swap_rtv && g_backend.swap_chain) {
        ID3D11DeviceContext_OMSetRenderTargets(g_backend.context, 1,
            &g_backend.swap_rtv, NULL);
        ID3D11DeviceContext_ClearRenderTargetView(g_backend.context,
            g_backend.swap_rtv, fallback_rgba);
        IDXGISwapChain_Present(g_backend.swap_chain, vsync ? 1 : 0, 0);
        empty_scene_fallback = 1;
    }

    if (empty_scene_fallback) {
        if (g_backend.backbuffer && g_backend.backbuffer->d3d11_rtv) {
            ID3D11DeviceContext_OMSetRenderTargets(g_backend.context, 1,
                &g_backend.backbuffer->d3d11_rtv, g_backend.depth_dsv);
        }
        return;
    }

    /* Standalone race rendering does not need the wrapper's multi-layer
     * present path. Blit the backbuffer SRV straight to the swap chain so
     * standalone scene draws remain visible while wrapper compositing is
     * still under reconstruction. */
    if (g_backend.standalone && g_backend.scene_rendered &&
        g_backend.context && g_backend.swap_chain && g_backend.swap_rtv &&
        g_backend.backbuffer && g_backend.backbuffer->d3d11_srv) {
        ID3D11DeviceContext_OMSetRenderTargets(g_backend.context, 0, NULL, NULL);
        ID3D11DeviceContext_OMSetRenderTargets(g_backend.context, 1,
            &g_backend.swap_rtv, NULL);
        Backend_DrawFullscreenQuad(g_backend.backbuffer->d3d11_srv);
        Backend_CaptureIfRequested();  /* photo-booth: grab the composited race frame */
        IDXGISwapChain_Present(g_backend.swap_chain, vsync ? 1 : 0, 0);

        if (g_backend.backbuffer->d3d11_rtv) {
            ID3D11DeviceContext_OMSetRenderTargets(g_backend.context, 1,
                &g_backend.backbuffer->d3d11_rtv, g_backend.depth_dsv);
        }
        return;
    }

    /* Present via the primary surface Flip, which triggers
     * Backend_CompositeAndPresent inside the wrapper */
    if (s_primary && s_primary->vtbl) {
        s_primary->vtbl->Flip(s_primary, NULL, DDFLIP_WAIT);
    } else if (g_backend.swap_chain) {
        IDXGISwapChain_Present(g_backend.swap_chain, vsync ? 1 : 0, 0);
    }
}

void td5_plat_present_texture_page(int page_index, int vsync)
{
    ID3D11ShaderResourceView *srv = NULL;

    if (page_index >= 0 && page_index < MAX_TEXTURE_PAGES) {
        if (s_tex_handles[page_index]) {
            srv = (ID3D11ShaderResourceView *)(DWORD_PTR)s_tex_handles[page_index];
        } else if (s_tex_surfaces[page_index] && s_tex_surfaces[page_index]->d3d11_srv) {
            srv = s_tex_surfaces[page_index]->d3d11_srv;
        }
    }

    if (!g_backend.context || !g_backend.swap_chain || !g_backend.swap_rtv || !srv)
        return;

    ID3D11DeviceContext_OMSetRenderTargets(g_backend.context, 1, &g_backend.swap_rtv, NULL);
    Backend_DrawFullscreenQuad(srv);
    IDXGISwapChain_Present(g_backend.swap_chain, vsync ? 1 : 0, 0);

    if (g_backend.backbuffer && g_backend.backbuffer->d3d11_rtv) {
        ID3D11DeviceContext_OMSetRenderTargets(g_backend.context, 1,
            &g_backend.backbuffer->d3d11_rtv, g_backend.depth_dsv);
    }
}

void td5_plat_get_window_size(int *width, int *height)
{
    if (width)  *width  = s_window_w;
    if (height) *height = s_window_h;
}

void td5_plat_set_fullscreen(int fullscreen)
{
    s_fullscreen = fullscreen;
    if (g_backend.swap_chain) {
        Backend_Reset(s_window_w, s_window_h, s_window_bpp, !fullscreen);
    }
}

void *td5_plat_get_native_window(void)
{
    return (void *)s_hwnd;
}

static int display_mode_exists(const TD5_DisplayMode *modes, int count,
                               int width, int height, int bpp)
{
    int i;
    for (i = 0; i < count; i++) {
        if (modes[i].width == width &&
            modes[i].height == height &&
            modes[i].bpp == bpp) {
            return 1;
        }
    }
    return 0;
}

int td5_plat_enum_display_modes(TD5_DisplayMode *modes, int max_count)
{
    DEVMODEW dm;
    int mode_index = 0;
    int count = 0;

    if (!modes || max_count <= 0) {
        return 0;
    }

    /* [S01 2026-06-04] Prefer the wrapper's DXGI output enumeration
     * (IDXGIOutput::GetDisplayModeList, run once at Backend_Init into
     * g_backend.modes) so the resolution selector lists true DXGI display
     * modes. DXGI lists every refresh-rate variant, so dedup by WxH and filter
     * to >=640x480. All enumerated DXGI formats are 32-bit BGRA/RGBA. Fall back
     * to the GDI EnumDisplaySettings list below if DXGI produced nothing. */
    if (g_backend.modes && g_backend.mode_count > 0) {
        int i;
        for (i = 0; i < g_backend.mode_count && count < max_count; i++) {
            int w = (int)g_backend.modes[i].Width;
            int h = (int)g_backend.modes[i].Height;
            if (w < 640 || h < 480) continue;
            if (display_mode_exists(modes, count, w, h, 32)) continue;
            modes[count].width      = w;
            modes[count].height     = h;
            modes[count].bpp        = 32;
            modes[count].fullscreen = 1;
            count++;
        }
        if (count > 0) {
            TD5_LOG_I(LOG_TAG, "enum_display_modes: %d modes from DXGI", count);
            return count;
        }
        /* else fall through to the GDI fallback */
    }

    ZeroMemory(&dm, sizeof(dm));
    dm.dmSize = sizeof(dm);

    while (EnumDisplaySettingsW(NULL, (DWORD)mode_index, &dm)) {
        if ((int)dm.dmPelsWidth >= 640 &&
            (int)dm.dmPelsHeight >= 480 &&
            (int)dm.dmBitsPerPel >= 16 &&
            !display_mode_exists(modes, count,
                (int)dm.dmPelsWidth, (int)dm.dmPelsHeight, (int)dm.dmBitsPerPel)) {
            modes[count].width = (int)dm.dmPelsWidth;
            modes[count].height = (int)dm.dmPelsHeight;
            modes[count].bpp = (int)dm.dmBitsPerPel;
            modes[count].fullscreen = 1;
            count++;
            if (count >= max_count) {
                break;
            }
        }
        mode_index++;
    }

    return count;
}

/* [S01 Display options 2026-06-04] Resize the swap-chain RT, the offscreen 3D
 * backbuffer, the wrapper frontend-scale and the source-port render-dim globals
 * to a new native client size. Does NOT touch the HWND style/position or the
 * exclusive-fullscreen state (callers own those). do_swap_reset==0 skips the
 * Backend_Reset/ResizeBuffers — used right after Backend_SetExclusiveFullscreen,
 * which already resized the swap-chain buffers itself. */
static int plat_resize_native(int width, int height, int bpp, int do_swap_reset)
{
    g_backend.target_width  = width;
    g_backend.target_height = height;

    if (do_swap_reset && g_backend.swap_chain) {
        int reset_w = g_backend.width  > 0 ? g_backend.width  : width;
        int reset_h = g_backend.height > 0 ? g_backend.height : height;
        if (!Backend_Reset(reset_w, reset_h, bpp, 1)) {
            TD5_LOG_E(LOG_TAG, "plat_resize_native: Backend_Reset FAILED");
            return 0;
        }
    }

    /* Recreate the offscreen 3D backbuffer so render_clear / set_viewport /
     * set_clip_rect (which read backbuffer->width/height) and the present blit
     * match the swap chain. */
    if (g_backend.standalone) {
        WrapperSurface *new_bb = WrapperSurface_Create(
            (DWORD)width, (DWORD)height, (DWORD)bpp,
            DDSCAPS_OFFSCREENPLAIN | DDSCAPS_3DDEVICE);
        if (new_bb) {
            WrapperSurface *old_bb = g_backend.backbuffer;
            g_backend.backbuffer = new_bb;
            if (old_bb && old_bb->vtbl && old_bb->vtbl->Release) {
                old_bb->vtbl->Release(old_bb);
            }
        } else {
            TD5_LOG_W(LOG_TAG, "plat_resize_native: backbuffer recreate FAILED at %dx%d",
                      width, height);
        }
    }

    /* Frontend-scale: the 2D UI is laid out at a 640x480 virtual resolution and
     * scaled to native; only the native side changes here. */
    g_backend.fe_scale.native_w = width;
    g_backend.fe_scale.native_h = height;
    if (g_backend.fe_scale.virtual_w > 0 && g_backend.fe_scale.virtual_h > 0 &&
        width  > g_backend.fe_scale.virtual_w &&
        height > g_backend.fe_scale.virtual_h) {
        g_backend.fe_scale.scale_x = (float)width  / (float)g_backend.fe_scale.virtual_w;
        g_backend.fe_scale.scale_y = (float)height / (float)g_backend.fe_scale.virtual_h;
        g_backend.fe_scale.enabled = 1;
    } else {
        g_backend.fe_scale.scale_x = 1.0f;
        g_backend.fe_scale.scale_y = 1.0f;
        g_backend.fe_scale.enabled = 0;
    }

    s_window_w   = width;
    s_window_h   = height;
    s_window_bpp = bpp;

    /* Sync the source-port render-dim globals (race viewport, HUD layout, VFX
     * projection, debug overlay). */
    {
        extern float g_render_width_f, g_render_height_f;
        extern int   g_render_width,   g_render_height;
        extern void  td5re_set_render_dims(int w, int h);
        g_render_width    = width;
        g_render_height   = height;
        g_render_width_f  = (float)width;
        g_render_height_f = (float)height;
        td5re_set_render_dims(width, height);
    }
    return 1;
}

/* Apply a resolution picked in Display options. Records it as the chosen
 * windowed/fullscreen resolution and re-applies the current window mode so the
 * change takes effect consistently (borderless ignores it and stays desktop). */
int td5_plat_apply_display_mode(int width, int height, int bpp)
{
    if (width <= 0 || height <= 0) return 0;
    s_chosen_w = width;
    s_chosen_h = height;
    TD5_LOG_I(LOG_TAG, "apply_display_mode: chosen=%dx%d bpp=%d mode=%d",
              width, height, bpp, s_window_mode);
    return td5_plat_set_window_mode(s_window_mode);
}

/* 0 = exclusive fullscreen, 1 = windowed, 2 = borderless. */
int td5_plat_get_window_mode(void)
{
    return s_window_mode;
}

int td5_plat_set_window_mode(int mode)
{
    HMONITOR mon;
    MONITORINFO mi;
    int desk_w, desk_h, bpp;
    DWORD style;

    if (mode < 0 || mode > 2) mode = 1;
    bpp = s_window_bpp > 0 ? s_window_bpp : 32;

    if (!s_hwnd) {
        s_window_mode = mode;
        s_fullscreen  = (mode == 0);
        g_backend.window_mode = mode;
        return 0;
    }

    mi.cbSize = sizeof(mi);
    mon = MonitorFromWindow(s_hwnd, MONITOR_DEFAULTTOPRIMARY);
    if (!mon || !GetMonitorInfoA(mon, &mi)) {
        mi.rcMonitor.left = 0; mi.rcMonitor.top = 0;
        mi.rcMonitor.right  = GetSystemMetrics(SM_CXSCREEN);
        mi.rcMonitor.bottom = GetSystemMetrics(SM_CYSCREEN);
        mi.rcWork = mi.rcMonitor; /* no taskbar info -> full monitor */
    }
    desk_w = mi.rcMonitor.right  - mi.rcMonitor.left;
    desk_h = mi.rcMonitor.bottom - mi.rcMonitor.top;
    if (s_chosen_w <= 0 || s_chosen_h <= 0) {
        s_chosen_w = s_window_w;
        s_chosen_h = s_window_h;
    }

    TD5_LOG_I(LOG_TAG, "set_window_mode: %d (was %d) chosen=%dx%d desktop=%dx%d",
              mode, s_window_mode, s_chosen_w, s_chosen_h, desk_w, desk_h);

    /* Suppress the WM_SIZE-driven live resize for the duration of this function:
     * the SetWindowPos / SetFullscreenState calls below fire WM_SIZE, but we
     * resize the swap chain explicitly via plat_resize_native. */
    s_suppress_wm_size = 1;

    /* Leaving exclusive fullscreen: drop the swap chain out of it first so the
     * subsequent restyle / ResizeBuffers operate on a windowed swap chain. */
    if (s_window_mode == 0 && mode != 0) {
        Backend_SetExclusiveFullscreen(0);
    }

    if (mode == 1) {
        /* Windowed: a fully resizable (WS_OVERLAPPEDWINDOW) bordered window at
         * the chosen resolution, centered in the monitor work area. The client
         * is clamped so the *framed* window fits the work area (taskbar-aware) —
         * without this a >=desktop pick pushes the title bar / lower menu rows
         * off screen and you can't change it again. The user can still drag the
         * sizing border or maximize afterwards (handled by WM_SIZE). */
        int w = s_chosen_w, h = s_chosen_h;
        int work_w = mi.rcWork.right  - mi.rcWork.left;
        int work_h = mi.rcWork.bottom - mi.rcWork.top;
        RECT wr;
        int outer_w, outer_h, frame_w, frame_h, x, y;
        s_fullscreen = 0;
        style = WS_OVERLAPPEDWINDOW | WS_VISIBLE;
        SetWindowLongA(s_hwnd, GWL_STYLE, (LONG)style);
        wr.left = 0; wr.top = 0; wr.right = w; wr.bottom = h;
        AdjustWindowRectEx(&wr, style, FALSE, 0);
        frame_w = (wr.right - wr.left) - w;   /* chrome overhead (borders+caption) */
        frame_h = (wr.bottom - wr.top) - h;
        if (w + frame_w > work_w) w = work_w - frame_w;
        if (h + frame_h > work_h) h = work_h - frame_h;
        if (w < 320) w = 320;
        if (h < 240) h = 240;
        outer_w = w + frame_w;
        outer_h = h + frame_h;
        x = mi.rcWork.left + (work_w - outer_w) / 2;
        y = mi.rcWork.top  + (work_h - outer_h) / 2;
        if (x < mi.rcWork.left) x = mi.rcWork.left;
        if (y < mi.rcWork.top)  y = mi.rcWork.top;
        SetWindowPos(s_hwnd, HWND_NOTOPMOST, x, y, outer_w, outer_h,
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW | SWP_NOACTIVATE);
        plat_resize_native(w, h, bpp, 1);
    } else if (mode == 2) {
        /* Borderless: WS_POPUP covering the whole monitor at desktop res. */
        s_fullscreen = 0;
        style = WS_POPUP | WS_VISIBLE;
        SetWindowLongA(s_hwnd, GWL_STYLE, (LONG)style);
        SetWindowPos(s_hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                     desk_w, desk_h, SWP_FRAMECHANGED | SWP_SHOWWINDOW | SWP_NOACTIVATE);
        plat_resize_native(desk_w, desk_h, bpp, 1);
    } else {
        /* Exclusive fullscreen at the chosen resolution. SetExclusiveFullscreen
         * resizes the swap-chain buffers itself, so plat_resize_native skips the
         * Backend_Reset (do_swap_reset=0) and only refreshes backbuffer/dims. */
        int w = s_chosen_w, h = s_chosen_h;
        s_fullscreen = 1;
        style = WS_POPUP | WS_VISIBLE;
        SetWindowLongA(s_hwnd, GWL_STYLE, (LONG)style);
        g_backend.target_width  = w;
        g_backend.target_height = h;
        if (g_backend.swap_chain) {
            Backend_SetExclusiveFullscreen(1);
        }
        plat_resize_native(w, h, bpp, 0);
    }

    s_suppress_wm_size = 0;
    s_window_mode = mode;
    g_backend.window_mode = mode;
    return 1;
}

/* [S01 2026-06-04] Toggle the present sync interval (Display options VSync). */
void td5_plat_set_vsync(int on)
{
    g_backend.vsync = on ? 1 : 0;
    TD5_LOG_I(LOG_TAG, "set_vsync: %d", g_backend.vsync);
}

/* [S01 2026-06-04] The user's current windowed/fullscreen resolution — tracks
 * drag-resizes (WM_SIZE updates s_chosen_*) but NOT the borderless desktop
 * override. Persisted as [Display] Width/Height so the window reopens at the
 * same size next launch (there is no Resolution row any more). */
void td5_plat_get_chosen_resolution(int *w, int *h)
{
    if (w) *w = (s_chosen_w > 0) ? s_chosen_w : s_window_w;
    if (h) *h = (s_chosen_h > 0) ? s_chosen_h : s_window_h;
}

/* ========================================================================
 * Timing
 * ======================================================================== */

uint64_t td5_plat_time_us(void)
{
    LARGE_INTEGER now;
    if (!s_qpc_inited) {
        QueryPerformanceFrequency(&s_qpc_freq);
        s_qpc_inited = 1;
    }
    QueryPerformanceCounter(&now);
    /* Convert to microseconds: (count * 1000000) / freq */
    return (uint64_t)((now.QuadPart * 1000000LL) / s_qpc_freq.QuadPart);
}

uint32_t td5_plat_time_ms(void)
{
    return (uint32_t)timeGetTime();
}

void td5_plat_sleep(uint32_t ms)
{
    Sleep((DWORD)ms);
}

/* ========================================================================
 * File I/O
 * ======================================================================== */

struct TD5_File {
    FILE *fp;
};

TD5_File *td5_plat_file_open(const char *path, const char *mode)
{
    TD5_File *f;
    FILE *fp;

    if (!path || !mode) return NULL;

    fp = fopen(path, mode);
    if (!fp) return NULL;

    f = (TD5_File *)malloc(sizeof(TD5_File));
    if (!f) { fclose(fp); return NULL; }
    f->fp = fp;
    return f;
}

void td5_plat_file_close(TD5_File *f)
{
    if (!f) return;
    if (f->fp) fclose(f->fp);
    free(f);
}

size_t td5_plat_file_read(TD5_File *f, void *buf, size_t size)
{
    if (!f || !f->fp || !buf || size == 0) return 0;
    return fread(buf, 1, size, f->fp);
}

size_t td5_plat_file_write(TD5_File *f, const void *buf, size_t size)
{
    if (!f || !f->fp || !buf || size == 0) return 0;
    return fwrite(buf, 1, size, f->fp);
}

int td5_plat_file_seek(TD5_File *f, int64_t offset, int origin)
{
    int whence;
    if (!f || !f->fp) return -1;

    switch (origin) {
    case 0: whence = SEEK_SET; break;
    case 1: whence = SEEK_CUR; break;
    case 2: whence = SEEK_END; break;
    default: return -1;
    }
    return _fseeki64(f->fp, offset, whence);
}

int64_t td5_plat_file_tell(TD5_File *f)
{
    if (!f || !f->fp) return -1;
    return _ftelli64(f->fp);
}

int64_t td5_plat_file_size(TD5_File *f)
{
    int64_t cur, sz;
    if (!f || !f->fp) return -1;
    cur = _ftelli64(f->fp);
    _fseeki64(f->fp, 0, SEEK_END);
    sz = _ftelli64(f->fp);
    _fseeki64(f->fp, cur, SEEK_SET);
    return sz;
}

int td5_plat_file_exists(const char *path)
{
    DWORD attr;
    if (!path) return 0;
    attr = GetFileAttributesA(path);
    return (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY));
}

int td5_plat_file_delete(const char *path)
{
    if (!path) return -1;
    return DeleteFileA(path) ? 0 : -1;
}

int td5_plat_file_rename(const char *from, const char *to)
{
    if (!from || !to) return -1;
    /* MOVEFILE_REPLACE_EXISTING so a stale .migrated backup doesn't block it. */
    return MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING) ? 0 : -1;
}

/* ========================================================================
 * Human-readable INI config (Config.td5/CupData.td5 retirement)
 * ======================================================================== */

void td5_plat_ini_resolve_path(const char *filename, char *out, size_t out_n)
{
    if (!out || out_n == 0) return;
    out[0] = '\0';
    if (!filename) return;

    DWORD n = GetModuleFileNameA(NULL, out, (DWORD)out_n);
    if (n > 0 && n < out_n) {
        char *sl = out + n;
        while (sl > out && *sl != '\\' && *sl != '/') sl--;
        if (sl > out) {
            sl[1] = '\0';
        } else {
            out[0] = '\0';
        }
        /* Append filename if it still fits. */
        if (strlen(out) + strlen(filename) + 1 <= out_n) {
            strcat(out, filename);
            return;
        }
    }
    /* Fallback: bare filename (cwd-relative). */
    strncpy(out, filename, out_n - 1);
    out[out_n - 1] = '\0';
}

int td5_plat_ini_get_int(const char *file, const char *section,
                         const char *key, int fallback)
{
    if (!file || !section || !key) return fallback;
    return (int)GetPrivateProfileIntA(section, key, fallback, file);
}

int td5_plat_ini_get_str(const char *file, const char *section,
                         const char *key, const char *fallback,
                         char *out, size_t out_n)
{
    if (!out || out_n == 0) return 0;
    if (!file || !section || !key) { out[0] = '\0'; return 0; }
    DWORD n = GetPrivateProfileStringA(section, key,
                                       fallback ? fallback : "",
                                       out, (DWORD)out_n, file);
    return (int)n;
}

void td5_plat_ini_set_int(const char *file, const char *section,
                          const char *key, int value)
{
    char buf[16];
    if (!file || !section || !key) return;
    snprintf(buf, sizeof(buf), "%d", value);
    WritePrivateProfileStringA(section, key, buf, file);
}

void td5_plat_ini_set_str(const char *file, const char *section,
                          const char *key, const char *value)
{
    if (!file || !section || !key) return;
    WritePrivateProfileStringA(section, key, value ? value : "", file);
}

void td5_plat_ini_flush(const char *file)
{
    /* Passing NULL section/key/value flushes the cached profile so the next
     * Get* call re-reads the on-disk file we just rewrote via raw I/O. */
    if (!file) return;
    WritePrivateProfileStringA(NULL, NULL, NULL, file);
}

/* ========================================================================
 * Memory
 * ======================================================================== */

void *td5_plat_alloc_aligned(size_t size, size_t alignment)
{
    return _aligned_malloc(size, alignment);
}

void td5_plat_free_aligned(void *ptr)
{
    if (ptr) _aligned_free(ptr);
}

void *td5_plat_heap_alloc(size_t size)
{
    if (!s_game_heap)
        s_game_heap = HeapCreate(0, 1024 * 1024, 0);
    return HeapAlloc(s_game_heap, HEAP_ZERO_MEMORY, size);
}

void td5_plat_heap_free(void *ptr)
{
    if (ptr && s_game_heap)
        HeapFree(s_game_heap, 0, ptr);
}

/* [CONFIRMED @ 0x00430cb0 ResetGameHeap; L5 promotion sweep audit 2026-05-18]
 * ARCH-DIVERGENCE -- two intentional deltas vs orig (both behaviour-neutral):
 *   1) Initial size: 1 MB (port) vs 24 MB (orig). Both heaps are growable on
 *      Win32 so the initial reservation only affects first-N-allocs latency;
 *      td5_plat_heap_alloc never observes a failure on either size. The
 *      sole user-visible signal would be process VA-reservation footprint at
 *      startup, which is irrelevant for the source port's target hardware.
 *   2) First-call gate: orig uses sentinel byte DAT_004aee4c; port uses
 *      `if (s_game_heap)` null-check. Same semantic (one-shot destroy on
 *      reset path; cold start skips the destroy). Header status table at
 *      td5_game.c:95 still reads "DONE (delegates to td5_plat_heap_reset)"
 *      — accurate at the dispatch level; this comment documents the
 *      intra-platform deltas. */
void td5_plat_heap_reset(void)
{
    if (s_game_heap) {
        HeapDestroy(s_game_heap);
        s_game_heap = NULL;
    }
    s_game_heap = HeapCreate(0, 1024 * 1024, 0);
}

/* ========================================================================
 * Input
 * ======================================================================== */

/* DInput8 GUID (avoid needing dinput.lib for the GUID) */
static const GUID s_IID_IDirectInput8A =
    {0xBF798030,0x483A,0x4DA2,{0xAA,0x99,0x5D,0x64,0xED,0x36,0x97,0x00}};
static const GUID s_GUID_SysKeyboard =
    {0x6F1D2B61,0xD5A0,0x11CF,{0xBF,0xC7,0x44,0x45,0x53,0x54,0x00,0x00}};
static const GUID s_GUID_SysMouse =
    {0x6F1D2B60,0xD5A0,0x11CF,{0xBF,0xC7,0x44,0x45,0x53,0x54,0x00,0x00}};

int td5_plat_input_init(void)
{
    HRESULT hr;

    /* Attempt to create DirectInput8 */
    hr = DirectInput8Create(
        GetModuleHandleA(NULL),
        DIRECTINPUT_VERSION,
        &s_IID_IDirectInput8A,
        (void **)&s_dinput,
        NULL);

    if (FAILED(hr) || !s_dinput) {
        /* Fall back to GetKeyboardState in poll */
        s_dinput = NULL;
        return 1; /* non-fatal */
    }

    /* Create keyboard device */
    hr = IDirectInput8_CreateDevice(s_dinput, &s_GUID_SysKeyboard,
                                    &s_di_keyboard, NULL);
    if (SUCCEEDED(hr) && s_di_keyboard) {
        IDirectInputDevice8_SetDataFormat(s_di_keyboard, &c_dfDIKeyboard);
        if (s_hwnd)
            IDirectInputDevice8_SetCooperativeLevel(s_di_keyboard, s_hwnd,
                DISCL_FOREGROUND | DISCL_NONEXCLUSIVE);
        IDirectInputDevice8_Acquire(s_di_keyboard);
    }

    /* Create mouse device */
    hr = IDirectInput8_CreateDevice(s_dinput, &s_GUID_SysMouse,
                                    &s_di_mouse, NULL);
    if (SUCCEEDED(hr) && s_di_mouse) {
        IDirectInputDevice8_SetDataFormat(s_di_mouse, &c_dfDIMouse2);
        if (s_hwnd)
            IDirectInputDevice8_SetCooperativeLevel(s_di_mouse, s_hwnd,
                DISCL_FOREGROUND | DISCL_NONEXCLUSIVE);
        IDirectInputDevice8_Acquire(s_di_mouse);
    }

    return 1;
}

void td5_plat_input_shutdown(void)
{
    td5_plat_input_scan_join_release();
    if (s_di_fe_joystick) {
        IDirectInputDevice8_Unacquire(s_di_fe_joystick);
        IDirectInputDevice8_Release(s_di_fe_joystick);
        s_di_fe_joystick = NULL;
    }
    for (int js = 0; js < TD5_PLAT_MAX_JS_SLOTS; js++) {
        if (s_di_joystick[js]) {
            IDirectInputDevice8_Unacquire(s_di_joystick[js]);
            IDirectInputDevice8_Release(s_di_joystick[js]);
            s_di_joystick[js] = NULL;
        }
    }
    if (s_di_mouse) {
        IDirectInputDevice8_Unacquire(s_di_mouse);
        IDirectInputDevice8_Release(s_di_mouse);
        s_di_mouse = NULL;
    }
    if (s_di_keyboard) {
        IDirectInputDevice8_Unacquire(s_di_keyboard);
        IDirectInputDevice8_Release(s_di_keyboard);
        s_di_keyboard = NULL;
    }
    if (s_dinput) {
        IDirectInput8_Release(s_dinput);
        s_dinput = NULL;
    }
}

/* Apply rebound keyboard scancodes (from the control-config screen / Config.td5)
 * to the live binding table the in-race poll reads. `scancodes` is the canonical
 * 10-action array; a 0 byte (DIK_NONE) is ignored so unset slots keep their
 * default. Mirrors the original copying g_keyboardScanCodeTable into the live
 * M2DX binding table on the menu->race transition (0x00415546). */
void td5_plat_input_set_keyboard_bindings(int player, const uint8_t *scancodes, int count)
{
    if (player < 0 || player > 1 || !scancodes) return;
    if (count > TD5_KB_BIND_COUNT) count = TD5_KB_BIND_COUNT;
    for (int i = 0; i < count; i++) {
        if (scancodes[i] != 0) s_kb_bindings[player][i] = scancodes[i];
    }
    TD5_LOG_I(LOG_TAG, "input: applied %d kb bindings P%d (view=0x%02X rear=0x%02X handbrake=0x%02X)",
              count, player + 1, s_kb_bindings[player][8], s_kb_bindings[player][9],
              s_kb_bindings[player][4]);
}

/* DIJOYSTATE2 axis value by index 0..7 (lX,lY,lZ,lRx,lRy,lRz,slider0,slider1). */
static long js_axis_val(const DIJOYSTATE2 *js, int axis)
{
    switch (axis) {
        case 0: return js->lX;  case 1: return js->lY;  case 2: return js->lZ;
        case 3: return js->lRx; case 4: return js->lRy; case 5: return js->lRz;
        case 6: return js->rglSlider[0]; case 7: return js->rglSlider[1];
        default: return TD5_PLAT_JS_AXIS_CENTER;
    }
}

/* Magnitude [0..256] of an action's binding given the current device state.
 * Buttons are 0/256; axes are measured from centre (0xFA) in the bound
 * direction (works for sticks and centre-rest combined triggers). */
static int js_action_mag(const DIJOYSTATE2 *js, uint32_t code)
{
    if (code == TD5_JSBIND_NONE) return 0;
    if (code & TD5_JSBIND_AXIS) {
        int axis = (int)((code >> 1) & 0x7);
        int dir  = (int)(code & 1);
        long v   = js_axis_val(js, axis) - TD5_PLAT_JS_AXIS_CENTER;  /* -250..250 */
        int  m   = (dir == 0) ? (int)v : (int)(-v);
        if (m < 0) m = 0;
        if (m > TD5_PLAT_JS_AXIS_CENTER) m = TD5_PLAT_JS_AXIS_CENTER;
        return (m * 256) / TD5_PLAT_JS_AXIS_CENTER;
    }
    if (code & TD5_JSBIND_BUTTON) {
        int btn = (int)(code & 0xFF);
        if (btn >= 0 && btn < 128 && (js->rgbButtons[btn] & 0x80)) return 256;
        return 0;
    }
    return 0;
}

void td5_plat_input_poll(int slot, TD5_InputState *out)
{
    static POINT s_last_cursor_pos;
    static int s_last_cursor_valid = 0;
    HWND active_hwnd;
    HWND target_hwnd;

    if (!out) return;
    memset(out, 0, sizeof(*out));

    target_hwnd = s_hwnd ? s_hwnd : (HWND)(DWORD_PTR)Backend_GetDisplayWindow();
    active_hwnd = GetForegroundWindow();
    if (target_hwnd && active_hwnd != target_hwnd) {
        memset(s_keyboard, 0, sizeof(s_keyboard));
        s_mouse_dx = 0;
        s_mouse_dy = 0;
        s_mouse_buttons = 0;
        s_last_cursor_valid = 0;
        return;
    }

    /* Keyboard: use DInput if available AND acquired, else Win32 fallback.
     * DInput with DISCL_FOREGROUND requires a valid hwnd + focus. If hwnd
     * is NULL (common at startup), DInput fails silently. Always fall back
     * to GetKeyboardState which works without a window handle. */
    {
        int di_ok = 0;
        if (s_di_keyboard) {
            HRESULT hr = IDirectInputDevice8_GetDeviceState(s_di_keyboard,
                            sizeof(s_keyboard), s_keyboard);
            if (hr == DIERR_INPUTLOST || hr == DIERR_NOTACQUIRED) {
                IDirectInputDevice8_Acquire(s_di_keyboard);
                hr = IDirectInputDevice8_GetDeviceState(s_di_keyboard,
                        sizeof(s_keyboard), s_keyboard);
            }
            if (SUCCEEDED(hr)) {
                static int s_dinput_ok_logged = 0;
                if (!s_dinput_ok_logged) {
                    TD5_LOG_I(LOG_TAG, "DInput keyboard acquired successfully");
                    s_dinput_ok_logged = 1;
                }
                di_ok = 1;
            }
        }
        if (!di_ok) {
            /* Win32 fallback — DInput scancodes for extended keys (arrows, Ins,
             * Del, Home, End, etc.) live at indices 0x80-0xFF, formed by ORing
             * the base Set-1 scancode with 0x80 (i.e. E0,48 → 0xC8 for Up).
             * MapVirtualKey needs the E0 prefix in bits 8-15 for these keys;
             * passing 0xC8 raw returns 0 (unmapped). Use MAPVK_VSC_TO_VK_EX
             * to distinguish left/right variants. */
            static int s_fallback_warned = 0;
            if (!s_fallback_warned) {
                TD5_LOG_W(LOG_TAG, "DInput keyboard unavailable — using Win32 GetAsyncKeyState fallback");
                s_fallback_warned = 1;
            }
            memset(s_keyboard, 0, sizeof(s_keyboard));
            for (int sc = 0; sc < 256; sc++) {
                UINT win_sc = (sc >= 0x80) ? (0xE000u | ((UINT)sc & 0x7Fu))
                                           : (UINT)sc;
                UINT vk = MapVirtualKeyA(win_sc, MAPVK_VSC_TO_VK_EX);
                if (vk && (GetAsyncKeyState(vk) & 0x8000))
                    s_keyboard[sc] = 0x80;
            }
        }
    }

    /* Mouse */
    if (s_di_mouse) {
        DIMOUSESTATE2 ms;
        HRESULT hr;
        memset(&ms, 0, sizeof(ms));
        hr = IDirectInputDevice8_GetDeviceState(s_di_mouse, sizeof(ms), &ms);
        if (hr == DIERR_INPUTLOST || hr == DIERR_NOTACQUIRED) {
            IDirectInputDevice8_Acquire(s_di_mouse);
            hr = IDirectInputDevice8_GetDeviceState(s_di_mouse, sizeof(ms), &ms);
        }
        if (SUCCEEDED(hr)) {
            s_mouse_dx = ms.lX;
            s_mouse_dy = ms.lY;
            s_mouse_buttons = 0;
            if (ms.rgbButtons[0] & 0x80) s_mouse_buttons |= 1;
            if (ms.rgbButtons[1] & 0x80) s_mouse_buttons |= 2;
            if (ms.rgbButtons[2] & 0x80) s_mouse_buttons |= 4;
        }
    } else {
        POINT pt;
        GetCursorPos(&pt);
        if (s_last_cursor_valid) {
            s_mouse_dx = pt.x - s_last_cursor_pos.x;
            s_mouse_dy = pt.y - s_last_cursor_pos.y;
        } else {
            s_mouse_dx = 0;
            s_mouse_dy = 0;
            s_last_cursor_valid = 1;
        }
        s_last_cursor_pos = pt;
        s_mouse_buttons = 0;
        if (GetAsyncKeyState(VK_LBUTTON) & 0x8000) s_mouse_buttons |= 1;
        if (GetAsyncKeyState(VK_RBUTTON) & 0x8000) s_mouse_buttons |= 2;
        if (GetAsyncKeyState(VK_MBUTTON) & 0x8000) s_mouse_buttons |= 4;
    }

    out->mouse_dx      = (int16_t)s_mouse_dx;
    out->mouse_dy      = (int16_t)s_mouse_dy;
    /* OR in latched clicks so a sub-frame click is never missed; clear after reading */
    out->mouse_buttons = s_mouse_buttons | s_mouse_click_latch;
    s_mouse_click_latch = 0;

    /* Map keyboard to game buttons via the rebindable per-player binding table.
     * Defaults: P1 (slot 0) = original arrow-key layout; P2 (slot 1) = WASD.
     * Both are rebindable from the control-config screen. */
    {
        uint32_t bits = 0;

        #define K_DOWN(sc) (s_keyboard[(sc)] & 0x80)

        /* Table-driven: read each action's scancode from the rebindable
         * per-player binding table (s_kb_bindings), so a key rebound on the
         * control-config screen actually changes in-race input. The binding
         * table is the single source of truth (faithful to the original, which
         * reads its live M2DX binding table — itself a copy of the rebindable
         * g_keyboardScanCodeTable). A binding of 0 (DIK_NONE) reads s_keyboard[0]
         * which is never set, so an unbound slot is simply inert.
         * NOTE: the previous "modern alternates" (Space=handbrake, LShift/RShift
         * =gear) are intentionally dropped — keeping a second hardcoded key alive
         * after a rebind is exactly the bug being fixed. */
        {
            const uint8_t *kb = s_kb_bindings[(slot == 0) ? 0 : 1];
            if (K_DOWN(kb[0])) bits |= TD5_INPUT_STEER_LEFT;
            if (K_DOWN(kb[1])) bits |= TD5_INPUT_STEER_RIGHT;
            if (K_DOWN(kb[2])) bits |= TD5_INPUT_THROTTLE;
            if (K_DOWN(kb[3])) bits |= TD5_INPUT_BRAKE;
            if (K_DOWN(kb[4])) bits |= TD5_INPUT_HANDBRAKE;
            if (K_DOWN(kb[5])) bits |= TD5_INPUT_HORN;
            if (K_DOWN(kb[6])) bits |= TD5_INPUT_GEAR_UP;
            if (K_DOWN(kb[7])) bits |= TD5_INPUT_GEAR_DOWN;
            if (K_DOWN(kb[8])) bits |= TD5_INPUT_CAMERA_CHANGE;
            if (K_DOWN(kb[9])) bits |= TD5_INPUT_REAR_VIEW;
            if (K_DOWN(kb[10])) bits |= TD5_INPUT_PAUSE;   /* [PORT 2026-06] rebindable pause */
        }

        #undef K_DOWN

        /* Diagnostic: log any non-zero button state (throttled) */
        {
            static uint32_t s_btn_log_ctr = 0;
            if (bits && (s_btn_log_ctr++ % 60u) == 0u) {
                TD5_LOG_I(LOG_TAG, "input_poll slot=%d buttons=0x%08X",
                          slot, bits);
            }
        }

        out->buttons = bits;
    }

    /* Joystick: if a device is bound to this slot, REPLACE the control word
     * with the original M2DX GetJS packed format (analog axes + action buttons
     * + keyboard pause/escape). Faithful to PollRaceSessionInput 0x0042c470,
     * which uses DXInput::GetJS for a joystick player and the keyboard stick
     * otherwise. The device axis range is set to [0, 0x1F4] (=500) so the raw
     * value is the 9-bit packed axis centred at 0xFA (=250), matching
     * GetJS @ M2DX 0x1000a1e0 and the consumer at td5_input.c (Path B). */
    if (slot >= 0 && slot < TD5_PLAT_MAX_JS_SLOTS && s_di_joystick[slot]) {
        DIJOYSTATE2 js;
        HRESULT hr;
        memset(&js, 0, sizeof(js));
        hr = IDirectInputDevice8_Poll(s_di_joystick[slot]);
        if (hr == DIERR_INPUTLOST || hr == DIERR_NOTACQUIRED) {
            IDirectInputDevice8_Acquire(s_di_joystick[slot]);
            IDirectInputDevice8_Poll(s_di_joystick[slot]);
        }
        hr = IDirectInputDevice8_GetDeviceState(s_di_joystick[slot], sizeof(js), &js);
        if (SUCCEEDED(hr)) {
            static const uint32_t k_action_bits[6] = {
                TD5_INPUT_HANDBRAKE, TD5_INPUT_HORN, TD5_INPUT_GEAR_UP,
                TD5_INPUT_GEAR_DOWN, TD5_INPUT_CAMERA_CHANGE, TD5_INPUT_REAR_VIEW
            };
            uint32_t jbits = 0;
            long ax_x, ax_y;

            if (s_js_action_set[slot]) {
                /* [PORT ENHANCEMENT 2026-06] per-action bindings: each of the 10
                 * actions maps to a button or an axis/trigger direction. LEFT/RIGHT
                 * synthesize the analog steer axis, ACCELERATE/BRAKE the throttle
                 * axis (so a trigger drives them analog), the rest are digital. */
                const uint32_t *ab = s_js_action_bind[slot];
                int left  = js_action_mag(&js, ab[0]);
                int right = js_action_mag(&js, ab[1]);
                int accel = js_action_mag(&js, ab[2]);
                int brake = js_action_mag(&js, ab[3]);
                int steer = left - right;     /* -256..256 (LEFT input → steer left) */
                int thr   = accel - brake;    /* accelerate pushes Y below centre */
                ax_x = TD5_PLAT_JS_AXIS_CENTER + (steer * TD5_PLAT_JS_AXIS_CENTER) / 256;
                ax_y = TD5_PLAT_JS_AXIS_CENTER - (thr   * TD5_PLAT_JS_AXIS_CENTER) / 256;
                if (ax_x < 0) ax_x = 0; if (ax_x > 0x1FF) ax_x = 0x1FF;
                if (ax_y < 0) ax_y = 0; if (ax_y > 0x1FF) ax_y = 0x1FF;
                jbits |= ((uint32_t)ax_x & 0x1FF) | TD5_INPUT_ANALOG_X_FLAG;
                jbits |= (((uint32_t)ax_y & 0x1FF) << 9) | TD5_INPUT_ANALOG_Y_FLAG;
                for (int k = 0; k < 6; k++)
                    if (js_action_mag(&js, ab[4 + k]) >= 128) jbits |= k_action_bits[k];
                if (js_action_mag(&js, ab[10]) >= 128) jbits |= TD5_INPUT_PAUSE;  /* PAUSE */
            } else {
                /* Default mapping (unconfigured): X = steering, Y = throttle/brake;
                 * physical buttons 2..7 → handbrake/horn/gear/camera/rear. The
                 * legacy [1]/[2] axis-swap is honoured for 2-axis wheels. */
                const int32_t *bind = s_joystick_bindings[slot];
                int configured = s_joystick_bound[slot];
                ax_x = js.lX; ax_y = js.lY;
                if (configured && bind[1] == 5 && bind[2] == 4) { long t = ax_x; ax_x = ax_y; ax_y = t; }
                if (ax_x < 0) ax_x = 0; if (ax_x > 0x1FF) ax_x = 0x1FF;
                if (ax_y < 0) ax_y = 0; if (ax_y > 0x1FF) ax_y = 0x1FF;
                jbits |= ((uint32_t)ax_x & 0x1FF) | TD5_INPUT_ANALOG_X_FLAG;
                jbits |= (((uint32_t)ax_y & 0x1FF) << 9) | TD5_INPUT_ANALOG_Y_FLAG;
                {
                    static const int k_default_btn[6] = { 2, 3, 4, 5, 6, 7 };
                    for (int k = 0; k < 6; k++) {
                        int phys = k_default_btn[k];
                        if (configured) { int v = bind[3 + k]; if (v >= 2 && v <= 10) phys = v - 2; }
                        if (phys >= 0 && phys < TD5_PLAT_JS_MAX_BUTTONS &&
                            (js.rgbButtons[phys] & 0x80))
                            jbits |= k_action_bits[k];
                    }
                }
            }

            /* Pause / Escape always read from the keyboard (GetJS tail). */
            if (s_keyboard[0x19] & 0x80) jbits |= TD5_INPUT_PAUSE;   /* DIK_P   */
            if (s_keyboard[0x01] & 0x80) jbits |= TD5_INPUT_ESCAPE;  /* DIK_ESC */

            out->buttons  = jbits;
            out->analog_x = (int16_t)((int)ax_x - TD5_PLAT_JS_AXIS_CENTER);
            out->analog_y = (int16_t)((int)ax_y - TD5_PLAT_JS_AXIS_CENTER);
        }
    }
}

const uint8_t *td5_plat_input_get_keyboard(void)
{
    return s_keyboard;
}

int td5_plat_input_key_pressed(int scancode)
{
    if (scancode < 0 || scancode > 255) return 0;
    return (s_keyboard[scancode] & 0x80) ? 1 : 0;
}

/* Joystick enumeration callback */
static BOOL CALLBACK EnumJoysticksCallback(
    const DIDEVICEINSTANCEA *inst, void *ctx)
{
    (void)ctx;
    if (s_device_count < 16) {
        strncpy(s_device_names[s_device_count],
                inst->tszInstanceName,
                sizeof(s_device_names[0]) - 1);
        s_device_names[s_device_count][255] = '\0';
        /* Store the instance GUID so td5_plat_input_set_device can (re)create
         * this exact device later — without it, switching to any joystick is
         * impossible (s_di_joystick was never created). */
        s_device_guids[s_device_count] = inst->guidInstance;
        /* Classify device type: wheel/driving=2, gamepad=1 */
        {
            BYTE dev_type = GET_DIDEVICE_TYPE(inst->dwDevType);
            if (dev_type == DI8DEVTYPE_DRIVING ||
                dev_type == DI8DEVTYPE_FLIGHT  ||
                dev_type == DI8DEVTYPE_1STPERSON) {
                s_device_types[s_device_count] = 2; /* joystick/wheel */
            } else {
                s_device_types[s_device_count] = 1; /* gamepad */
            }
        }
        s_device_count++;
    }
    return DIENUM_CONTINUE;
}

int td5_plat_input_enumerate_devices(void)
{
    s_device_count = 0;
    /* Keyboard is always device 0 */
    strncpy(s_device_names[0], "Keyboard", sizeof(s_device_names[0]));
    s_device_types[0] = 0; /* keyboard */
    s_device_count = 1;

    if (s_dinput) {
        IDirectInput8_EnumDevices(s_dinput,
            DI8DEVCLASS_GAMECTRL,
            EnumJoysticksCallback,
            NULL,
            DIEDFL_ATTACHEDONLY);
    }
    TD5_LOG_I(LOG_TAG, "Input devices enumerated: count=%d primary=%s",
              s_device_count,
              (s_device_count > 0) ? s_device_names[0] : "<none>");
    return s_device_count;
}

const char *td5_plat_input_device_name(int index)
{
    if (index < 0 || index >= s_device_count) return NULL;
    return s_device_names[index];
}

int td5_plat_input_device_type(int index)
{
    if (index < 0 || index >= s_device_count) return 0;
    return s_device_types[index];
}

void td5_plat_input_set_device(int slot, int device_index)
{
    if (slot < 0 || slot >= TD5_PLAT_MAX_JS_SLOTS) {
        TD5_LOG_W(LOG_TAG, "Input device select: slot %d out of range", slot);
        return;
    }

    /* Device 0 = keyboard: release this slot's joystick (if any) and return. */
    if (device_index <= 0) {
        if (s_di_joystick[slot]) {
            IDirectInputDevice8_Unacquire(s_di_joystick[slot]);
            IDirectInputDevice8_Release(s_di_joystick[slot]);
            s_di_joystick[slot] = NULL;
        }
        TD5_LOG_I(LOG_TAG, "Input device selected: slot=%d device=%d name=%s",
                  slot, device_index, "Keyboard");
        return;
    }

    /* (Re)create the requested joystick for this player slot from its stored
     * instance GUID. Port-side reimplementation on DirectInput8 (the original
     * delegated this to M2DX's DXInput). The original supports up to 2 joystick
     * devices at once, so each slot owns its own device. */
    if (device_index >= s_device_count) {
        TD5_LOG_W(LOG_TAG, "Input device select: index %d out of range (count=%d)",
                  device_index, s_device_count);
        return;
    }
    if (!s_dinput) {
        TD5_LOG_W(LOG_TAG, "Input device select: DirectInput not initialized");
        return;
    }

    /* Drop this slot's previous joystick before creating the new one. */
    if (s_di_joystick[slot]) {
        IDirectInputDevice8_Unacquire(s_di_joystick[slot]);
        IDirectInputDevice8_Release(s_di_joystick[slot]);
        s_di_joystick[slot] = NULL;
    }

    /* Release the shared frontend-nav + lobby-scan handles so the per-player
     * device can take the physical joystick cleanly. */
    if (s_di_fe_joystick) {
        IDirectInputDevice8_Unacquire(s_di_fe_joystick);
        IDirectInputDevice8_Release(s_di_fe_joystick);
        s_di_fe_joystick = NULL;
    }
    td5_plat_input_scan_join_release();
    s_fe_js_suppress = 1;

    {
        HRESULT hr = IDirectInput8_CreateDevice(s_dinput,
                                                &s_device_guids[device_index],
                                                &s_di_joystick[slot], NULL);
        if (FAILED(hr) || !s_di_joystick[slot]) {
            TD5_LOG_W(LOG_TAG, "Input device select: CreateDevice failed hr=0x%08lX slot=%d device=%d (%s)",
                      (unsigned long)hr, slot, device_index, s_device_names[device_index]);
            s_di_joystick[slot] = NULL;
            return;
        }

        /* DIJOYSTATE2 format — matches the struct the poll reads. */
        IDirectInputDevice8_SetDataFormat(s_di_joystick[slot], &c_dfDIJoystick2);

        /* Exclusive foreground: required for force feedback (td5_plat_ff_init
         * binds this slot's own device); input reads still work in this mode. */
        if (s_hwnd)
            IDirectInputDevice8_SetCooperativeLevel(s_di_joystick[slot], s_hwnd,
                DISCL_EXCLUSIVE | DISCL_FOREGROUND);

        /* Absolute axis range [0, 0x1F4]=500 with a 10% deadzone, so the raw
         * axis value is the original's 9-bit packed axis centred at 0xFA=250
         * (deadzone 25/250 ~ 10%, per M2DX EnumerateJoystickDeviceCallback). */
        {
            DIPROPRANGE range;
            range.diph.dwSize       = sizeof(DIPROPRANGE);
            range.diph.dwHeaderSize = sizeof(DIPROPHEADER);
            range.diph.dwHow        = DIPH_DEVICE;
            range.diph.dwObj        = 0;
            range.lMin              = 0;
            range.lMax              = 0x1F4;
            IDirectInputDevice8_SetProperty(s_di_joystick[slot], DIPROP_RANGE, &range.diph);

            DIPROPDWORD dz;
            dz.diph.dwSize       = sizeof(DIPROPDWORD);
            dz.diph.dwHeaderSize = sizeof(DIPROPHEADER);
            dz.diph.dwHow        = DIPH_DEVICE;
            dz.diph.dwObj        = 0;
            dz.dwData            = 1000;  /* 10% of [0,10000] */
            IDirectInputDevice8_SetProperty(s_di_joystick[slot], DIPROP_DEADZONE, &dz.diph);
        }

        IDirectInputDevice8_Acquire(s_di_joystick[slot]);
    }

    TD5_LOG_I(LOG_TAG, "Input device selected: slot=%d device=%d name=%s (joystick created)",
              slot, device_index, s_device_names[device_index]);
}

/* Push the per-player 9-slot joystick binding table into the platform poll.
 * Mirrors td5_plat_input_set_keyboard_bindings; without this the control-config
 * screen's joystick bindings would be a write-only dead end. count<=9. */
void td5_plat_input_set_joystick_bindings(int slot, const int32_t *bindings, int count)
{
    if (slot < 0 || slot >= TD5_PLAT_MAX_JS_SLOTS || !bindings) return;
    int n = (count < 9) ? count : 9;
    for (int i = 0; i < n; i++) s_joystick_bindings[slot][i] = bindings[i];
    s_joystick_bound[slot] = 1;
    TD5_LOG_I(LOG_TAG,
              "Joystick bindings set: slot=%d [%d %d %d %d %d %d %d %d %d]",
              slot, s_joystick_bindings[slot][0], s_joystick_bindings[slot][1],
              s_joystick_bindings[slot][2], s_joystick_bindings[slot][3],
              s_joystick_bindings[slot][4], s_joystick_bindings[slot][5],
              s_joystick_bindings[slot][6], s_joystick_bindings[slot][7],
              s_joystick_bindings[slot][8]);
}

/* Raw physical joystick button bitmask for a device slot (bit i = button i down).
 * Used by the control-config per-button capture. [PORT ENHANCEMENT 2026-06] */
uint32_t td5_plat_input_joystick_buttons(int device_slot)
{
    DIJOYSTATE2 js;
    HRESULT hr;
    uint32_t mask = 0;
    int i;

    if (device_slot < 0 || device_slot >= TD5_PLAT_MAX_JS_SLOTS ||
        !s_di_joystick[device_slot])
        return 0;

    memset(&js, 0, sizeof(js));
    hr = IDirectInputDevice8_Poll(s_di_joystick[device_slot]);
    if (hr == DIERR_INPUTLOST || hr == DIERR_NOTACQUIRED) {
        IDirectInputDevice8_Acquire(s_di_joystick[device_slot]);
        IDirectInputDevice8_Poll(s_di_joystick[device_slot]);
    }
    if (FAILED(IDirectInputDevice8_GetDeviceState(s_di_joystick[device_slot],
                                                  sizeof(js), &js)))
        return 0;

    for (i = 0; i < 32; i++)
        if (js.rgbButtons[i] & 0x80) mask |= (1u << i);
    return mask;
}

static int td5_plat_js_read(int device_slot, DIJOYSTATE2 *js);   /* fwd */

/* State for td5_plat_input_joystick_neutral (below). A trigger rests at an
 * arbitrary OFF-CENTRE value AND a held trigger is "stable" (motionless) just
 * like a released one — so neither an absolute centre test NOR a stability test
 * alone can tell "held" from "released". The fix: LEARN each axis's rest position
 * while the UI is idle (td5_plat_input_joystick_learn_rest), then require every
 * axis to be back within a band of that learned rest (a held trigger reads far
 * from rest) AND motionless for a few frames (absorbs spring-back overshoot). */
static long s_neutral_rest_axes[8];          /* learned idle position per axis     */
static int  s_neutral_rest_valid   = 0;
static long s_neutral_prev_axes[8];          /* last frame, for the motion test    */
static int  s_neutral_have_prev    = 0;
static int  s_neutral_stable_count = 0;

/* Reset the settle/motion tracker — call when (re)entering a wait-for-release so a
 * stale stable-count from an earlier capture can't arm the next one instantly.
 * (The learned REST reference is intentionally preserved across resets.) */
void td5_plat_input_joystick_neutral_reset(void)
{
    s_neutral_have_prev    = 0;
    s_neutral_stable_count = 0;
}

/* Learn the current axis positions as the device's REST reference. Call every
 * frame while the binding UI is IDLE (not capturing) so the wait-for-release can
 * later distinguish a held trigger (far from rest) from a released one (back at
 * rest). Skips learning while a button is held (user mid-interaction).
 * [PORT ENHANCEMENT 2026-06] */
void td5_plat_input_joystick_learn_rest(int device_slot)
{
    DIJOYSTATE2 js;
    int i;
    if (!td5_plat_js_read(device_slot, &js)) { s_neutral_rest_valid = 0; return; }
    for (i = 0; i < 32; i++) if (js.rgbButtons[i] & 0x80) return;
    for (i = 0; i < 8; i++) s_neutral_rest_axes[i] = js_axis_val(&js, i);
    s_neutral_rest_valid = 1;
}

/* 1 once the joystick has fully RETURNED TO REST: no buttons / dpad, sticks near
 * centre, every axis (incl. off-centre-resting triggers/sliders) back within a
 * band of its learned rest, and motionless for a few frames. Gates per-action
 * capture so it waits for the previous input — including a trigger's release
 * travel — to finish before listening again.
 * [PORT ENHANCEMENT 2026-06] "wait until everything is released" */
int td5_plat_input_joystick_neutral(int device_slot)
{
    DIJOYSTATE2 js;
    int i, moved;
    long axes[8];
    long sticks[4];
    const long REST_BAND     = 55;   /* max |axis - learned rest| to count as released */
    const long STABLE_DELTA  = 12;   /* per-frame axis jitter tolerance (of 0..0x1F4)  */
    const int  STABLE_FRAMES = 3;    /* consecutive motionless frames = settled        */

    if (!td5_plat_js_read(device_slot, &js)) {           /* no device → neutral */
        td5_plat_input_joystick_neutral_reset();
        return 1;
    }
    /* Any button or dpad held resets the settle timer. */
    for (i = 0; i < 32; i++)
        if (js.rgbButtons[i] & 0x80) { s_neutral_have_prev = 0; s_neutral_stable_count = 0; return 0; }
    if (js.rgdwPOV[0] != 0xFFFFFFFFu) { s_neutral_have_prev = 0; s_neutral_stable_count = 0; return 0; }

    /* Sticks (lX/lY/lRx/lRy) must be near centre. */
    sticks[0] = js.lX; sticks[1] = js.lY; sticks[2] = js.lRx; sticks[3] = js.lRy;
    for (i = 0; i < 4; i++) {
        long d = sticks[i] - TD5_PLAT_JS_AXIS_CENTER;
        if (d > 90 || d < -90) { s_neutral_have_prev = 0; s_neutral_stable_count = 0; return 0; }
    }

    for (i = 0; i < 8; i++) axes[i] = js_axis_val(&js, i);

    /* Every axis must be back within a band of its LEARNED rest — this is what
     * catches a HELD trigger (it sits far from where it rests when released). */
    if (s_neutral_rest_valid) {
        for (i = 0; i < 8; i++) {
            long d = axes[i] - s_neutral_rest_axes[i];
            if (d > REST_BAND || d < -REST_BAND) {
                for (i = 0; i < 8; i++) s_neutral_prev_axes[i] = axes[i];
                s_neutral_have_prev = 1;
                s_neutral_stable_count = 0;
                return 0;
            }
        }
    }

    /* …and motionless for STABLE_FRAMES frames (absorbs spring-back overshoot so
     * we don't arm while an axis is still travelling through the rest band). */
    moved = 0;
    if (s_neutral_have_prev) {
        for (i = 0; i < 8; i++) {
            long d = axes[i] - s_neutral_prev_axes[i];
            if (d > STABLE_DELTA || d < -STABLE_DELTA) { moved = 1; break; }
        }
    } else {
        moved = 1;   /* first sample — need another frame to measure motion */
    }
    for (i = 0; i < 8; i++) s_neutral_prev_axes[i] = axes[i];
    s_neutral_have_prev = 1;
    if (moved) { s_neutral_stable_count = 0; return 0; }
    if (++s_neutral_stable_count < STABLE_FRAMES) return 0;
    return 1;
}

/* Re-enumerate input devices; returns 1 if the device count changed (hot-plug),
 * releasing the scan handles so they recreate against the fresh GUID list.
 * [PORT ENHANCEMENT 2026-06] */
int td5_plat_input_rescan_devices(void)
{
    int prev = s_device_count;
    td5_plat_input_enumerate_devices();
    if (s_device_count != prev) {
        td5_plat_input_scan_join_release();
        s_fe_js_suppress = 0;
        return 1;
    }
    return 0;
}

/* ---- Per-action joystick bindings (PORT ENHANCEMENT 2026-06) ---- */

void td5_plat_input_set_action_bindings(int slot, const uint32_t *codes, int count)
{
    int i;
    if (slot < 0 || slot >= TD5_PLAT_MAX_JS_SLOTS || !codes) return;
    if (count > TD5_JSBIND_ACTIONS) count = TD5_JSBIND_ACTIONS;
    for (i = 0; i < count; i++) s_js_action_bind[slot][i] = codes[i];
    for (; i < TD5_JSBIND_ACTIONS; i++) s_js_action_bind[slot][i] = TD5_JSBIND_NONE;
    s_js_action_set[slot] = 1;
    TD5_LOG_I(LOG_TAG, "Action bindings set: slot=%d", slot);
}

static int td5_plat_js_read(int device_slot, DIJOYSTATE2 *js)
{
    HRESULT hr;
    if (device_slot < 0 || device_slot >= TD5_PLAT_MAX_JS_SLOTS ||
        !s_di_joystick[device_slot])
        return 0;
    memset(js, 0, sizeof(*js));
    hr = IDirectInputDevice8_Poll(s_di_joystick[device_slot]);
    if (hr == DIERR_INPUTLOST || hr == DIERR_NOTACQUIRED) {
        IDirectInputDevice8_Acquire(s_di_joystick[device_slot]);
        IDirectInputDevice8_Poll(s_di_joystick[device_slot]);
    }
    return SUCCEEDED(IDirectInputDevice8_GetDeviceState(s_di_joystick[device_slot],
                                                        sizeof(*js), js));
}

void td5_plat_input_joystick_capture_begin(int device_slot)
{
    DIJOYSTATE2 js;
    int i;
    s_js_cap_slot = device_slot;
    s_js_cap_btn_base = 0;
    for (i = 0; i < 8; i++) s_js_cap_axis_base[i] = TD5_PLAT_JS_AXIS_CENTER;
    if (!td5_plat_js_read(device_slot, &js)) return;
    for (i = 0; i < 8; i++)  s_js_cap_axis_base[i] = js_axis_val(&js, i);
    for (i = 0; i < 32; i++) if (js.rgbButtons[i] & 0x80) s_js_cap_btn_base |= (1u << i);
}

int td5_plat_input_joystick_capture_poll(int device_slot, uint32_t *out_code)
{
    DIJOYSTATE2 js;
    int i;
    const long AXIS_THRESH = 100;   /* of the 0..0x1F4 range (~20%) */
    if (!out_code) return 0;
    if (!td5_plat_js_read(device_slot, &js)) return 0;

    /* Fresh button press (down now, up at capture-begin). */
    for (i = 0; i < 32; i++) {
        uint32_t bit = (1u << i);
        if ((js.rgbButtons[i] & 0x80) && !(s_js_cap_btn_base & bit)) {
            *out_code = TD5_JSBIND_BUTTON | (uint32_t)i;
            return 1;
        }
    }
    /* Axis / trigger moved past threshold from its rest baseline. */
    for (i = 0; i < 8; i++) {
        long d = js_axis_val(&js, i) - s_js_cap_axis_base[i];
        if (d > AXIS_THRESH || d < -AXIS_THRESH) {
            int dir = (d > 0) ? 0 : 1;
            *out_code = TD5_JSBIND_AXIS | ((uint32_t)i << 1) | (uint32_t)dir;
            return 1;
        }
    }
    return 0;
}

void td5_plat_input_describe_binding(uint32_t code, char *buf, int cap)
{
    if (!buf || cap <= 0) return;
    if (code == TD5_JSBIND_NONE) {
        snprintf(buf, (size_t)cap, "-");
    } else if (code & TD5_JSBIND_AXIS) {
        int axis = (int)((code >> 1) & 0x7);
        int dir  = (int)(code & 1);
        snprintf(buf, (size_t)cap, "AXIS %d%c", axis, dir ? '-' : '+');
    } else if (code & TD5_JSBIND_BUTTON) {
        snprintf(buf, (size_t)cap, "BTN %d", (int)(code & 0xFF) + 1);
    } else {
        snprintf(buf, (size_t)cap, "?");
    }
}

/* Enumerated index of the joystick that last produced a confirm/nav (the active
 * controller). -1 = none yet. [PORT ENHANCEMENT 2026-06] */
static int s_plat_active_js = -1;
int td5_plat_input_active_joystick(void) { return s_plat_active_js; }

/* Ensure a non-exclusive scan handle for enumerated joystick i (1..). */
static LPDIRECTINPUTDEVICE8A scan_dev(int i)
{
    if (i < 1 || i >= 16 || !s_dinput) return NULL;
    if (!s_di_scan[i]) {
        if (SUCCEEDED(IDirectInput8_CreateDevice(s_dinput, &s_device_guids[i],
                                                 &s_di_scan[i], NULL))) {
            DIPROPRANGE range;
            IDirectInputDevice8_SetDataFormat(s_di_scan[i], &c_dfDIJoystick2);
            if (s_hwnd)
                IDirectInputDevice8_SetCooperativeLevel(s_di_scan[i], s_hwnd,
                    DISCL_NONEXCLUSIVE | DISCL_FOREGROUND);
            range.diph.dwSize = sizeof(DIPROPRANGE);
            range.diph.dwHeaderSize = sizeof(DIPROPHEADER);
            range.diph.dwHow = DIPH_DEVICE;
            range.diph.dwObj = 0;
            range.lMin = 0; range.lMax = 0x1F4;
            IDirectInputDevice8_SetProperty(s_di_scan[i], DIPROP_RANGE, &range.diph);
            IDirectInputDevice8_Acquire(s_di_scan[i]);
        } else {
            s_di_scan[i] = NULL;
        }
    }
    return s_di_scan[i];
}

/* Multiplayer lobby "press to join" scan: bit i set if device i's join control
 * is held this frame — device 0 = keyboard (Enter), devices 1.. = joystick
 * button 0 (A). Lazily creates a non-exclusive handle per joystick.
 * [PORT ENHANCEMENT 2026-06] */
uint32_t td5_plat_input_scan_join(void)
{
    uint32_t mask = 0;
    int i;
    if (s_keyboard[0x1C] & 0x80) mask |= 1u;     /* keyboard Enter = device 0 */
    for (i = 1; i < s_device_count && i < 16; i++) {
        DIJOYSTATE2 js;
        if (!s_di_scan[i]) {
            if (!s_dinput) continue;
            if (SUCCEEDED(IDirectInput8_CreateDevice(s_dinput, &s_device_guids[i],
                                                     &s_di_scan[i], NULL))) {
                IDirectInputDevice8_SetDataFormat(s_di_scan[i], &c_dfDIJoystick2);
                if (s_hwnd)
                    IDirectInputDevice8_SetCooperativeLevel(s_di_scan[i], s_hwnd,
                        DISCL_NONEXCLUSIVE | DISCL_FOREGROUND);
                IDirectInputDevice8_Acquire(s_di_scan[i]);
            } else { s_di_scan[i] = NULL; continue; }
        }
        memset(&js, 0, sizeof(js));
        if (FAILED(IDirectInputDevice8_Poll(s_di_scan[i]))) {
            IDirectInputDevice8_Acquire(s_di_scan[i]);
            IDirectInputDevice8_Poll(s_di_scan[i]);
        }
        if (SUCCEEDED(IDirectInputDevice8_GetDeviceState(s_di_scan[i], sizeof(js), &js)))
            if (js.rgbButtons[0] & 0x80) mask |= (1u << i);
    }
    return mask;
}

/* Release the lobby scan handles (call when leaving the lobby so per-player
 * exclusive devices can take the hardware cleanly). */
void td5_plat_input_scan_join_release(void)
{
    int i;
    for (i = 0; i < 16; i++) {
        if (s_di_scan[i]) {
            IDirectInputDevice8_Unacquire(s_di_scan[i]);
            IDirectInputDevice8_Release(s_di_scan[i]);
            s_di_scan[i] = NULL;
        }
    }
}

/* Frontend navigation bitmask aggregated across EVERY connected joystick, so any
 * configured pad drives the menus from startup:
 *   bit0 LEFT  bit1 RIGHT  bit2 UP  bit3 DOWN  bit4 A/confirm  bit5 B/back.
 * Also records which joystick produced A as the active controller. */
uint32_t td5_plat_input_frontend_nav(void)
{
    uint32_t bits = 0;
    int i;
    const long T = 130;
    for (i = 1; i < s_device_count && i < 16; i++) {
        DIJOYSTATE2 js;
        uint32_t db = 0;
        long cx, cy;
        LPDIRECTINPUTDEVICE8A dev = scan_dev(i);
        if (!dev) continue;
        memset(&js, 0, sizeof(js));
        if (FAILED(IDirectInputDevice8_Poll(dev))) {
            IDirectInputDevice8_Acquire(dev);
            IDirectInputDevice8_Poll(dev);
        }
        if (FAILED(IDirectInputDevice8_GetDeviceState(dev, sizeof(js), &js))) continue;

        cx = js.lX - TD5_PLAT_JS_AXIS_CENTER;
        cy = js.lY - TD5_PLAT_JS_AXIS_CENTER;
        if (cx < -T) db |= 1; if (cx > T) db |= 2;
        if (cy < -T) db |= 4; if (cy > T) db |= 8;   /* stick up = lY low = UP */
        if (js.rgdwPOV[0] != 0xFFFFFFFFu) {
            DWORD deg = js.rgdwPOV[0] / 100;
            if (deg > 270 || deg <  90) db |= 4;
            if (deg >  90 && deg < 270) db |= 8;
            if (deg > 180 && deg < 360) db |= 1;
            if (deg >   0 && deg < 180) db |= 2;
        }
        if (js.rgbButtons[0] & 0x80) db |= 0x10;     /* A = confirm/select */
        if (js.rgbButtons[1] & 0x80) db |= 0x20;     /* B = back/cancel    */
        if (db) {
            bits |= db;
            if (db & 0x10) s_plat_active_js = i;      /* A press marks this pad active */
        }
    }
    return bits;
}

/* In-race navigation bitmask from a player's EXCLUSIVE joystick device
 * (s_di_joystick[slot]) — same encoding as td5_plat_input_frontend_nav
 * (bit0 LEFT bit1 RIGHT bit2 UP bit3 DOWN bit4 A/confirm bit5 B/back). The
 * frontend scan handles are released while a race owns the device, so the pause
 * menu reads the player's own device through this instead. [PORT ENHANCEMENT 2026-06] */
uint32_t td5_plat_input_joystick_nav(int device_slot)
{
    DIJOYSTATE2 js;
    uint32_t db = 0;
    long cx, cy;
    const long T = 130;
    if (!td5_plat_js_read(device_slot, &js)) return 0;
    cx = js.lX - TD5_PLAT_JS_AXIS_CENTER;
    cy = js.lY - TD5_PLAT_JS_AXIS_CENTER;
    if (cx < -T) db |= 1; if (cx > T) db |= 2;
    if (cy < -T) db |= 4; if (cy > T) db |= 8;       /* stick up = lY low = UP */
    if (js.rgdwPOV[0] != 0xFFFFFFFFu) {
        DWORD deg = js.rgdwPOV[0] / 100;
        if (deg > 270 || deg <  90) db |= 4;
        if (deg >  90 && deg < 270) db |= 8;
        if (deg > 180 && deg < 360) db |= 1;
        if (deg >   0 && deg < 180) db |= 2;
    }
    if (js.rgbButtons[0] & 0x80) db |= 0x10;         /* A = confirm/select */
    if (js.rgbButtons[1] & 0x80) db |= 0x20;         /* B = back/cancel    */
    return db;
}

/* ========================================================================
 * Force Feedback
 * ======================================================================== */

static BOOL CALLBACK EnumConstantForceEffectsCallback(const DIEFFECTINFOA *info,
                                                      VOID *ctx)
{
    GUID *guid = (GUID *)ctx;
    if (info && IsEqualGUID(&info->guid, &GUID_ConstantForce)) {
        *guid = info->guid;
        return DIENUM_STOP;
    }
    return DIENUM_CONTINUE;
}

static void td5_ff_release_effects(int dev)
{
    int i;
    for (i = 0; i < 4; i++) {
        if (s_ff[dev].effects[i]) {
            IDirectInputEffect_Stop(s_ff[dev].effects[i]);
            IDirectInputEffect_Release(s_ff[dev].effects[i]);
            s_ff[dev].effects[i] = NULL;
        }
    }
}

static int td5_ff_create_constant_effect(int dev, int slot, DWORD axis, LONG direction)
{
    DICONSTANTFORCE cf;
    DIEFFECT eff;
    int ok;

    ZeroMemory(&cf, sizeof(cf));
    ZeroMemory(&eff, sizeof(eff));

    eff.dwSize = sizeof(eff);
    eff.dwFlags = DIEFF_CARTESIAN | DIEFF_OBJECTOFFSETS;
    eff.dwDuration = INFINITE;
    eff.dwGain = DI_FFNOMINALMAX;
    eff.dwTriggerButton = DIEB_NOTRIGGER;
    eff.cAxes = 1;
    eff.rgdwAxes = &axis;
    eff.rglDirection = &direction;
    eff.cbTypeSpecificParams = sizeof(cf);
    eff.lpvTypeSpecificParams = &cf;

    ok = SUCCEEDED(IDirectInputDevice8_CreateEffect(s_ff[dev].device,
        &s_ff[dev].effectGuid, &eff, &s_ff[dev].effects[slot], NULL));
    TD5_LOG_I(LOG_TAG, "FF effect create constant: dev=%d slot=%d axis=%lu direction=%ld result=%s",
              dev, slot, (unsigned long)axis, (long)direction, ok ? "ok" : "failed");
    return ok;
}

static int td5_ff_create_periodic_effect(int dev, int slot)
{
    DIPERIODIC periodic;
    DWORD axis = DIJOFS_X;
    LONG direction = 0;
    DIEFFECT eff;
    int ok;

    ZeroMemory(&periodic, sizeof(periodic));
    ZeroMemory(&eff, sizeof(eff));

    periodic.dwMagnitude = 0;
    periodic.lOffset = 0;
    periodic.dwPhase = 0;
    periodic.dwPeriod = DI_SECONDS / 8;

    eff.dwSize = sizeof(eff);
    eff.dwFlags = DIEFF_CARTESIAN | DIEFF_OBJECTOFFSETS;
    eff.dwDuration = INFINITE;
    eff.dwGain = DI_FFNOMINALMAX;
    eff.dwTriggerButton = DIEB_NOTRIGGER;
    eff.cAxes = 1;
    eff.rgdwAxes = &axis;
    eff.rglDirection = &direction;
    eff.cbTypeSpecificParams = sizeof(periodic);
    eff.lpvTypeSpecificParams = &periodic;

    ok = SUCCEEDED(IDirectInputDevice8_CreateEffect(s_ff[dev].device,
        &GUID_Sine, &eff, &s_ff[dev].effects[slot], NULL));
    TD5_LOG_I(LOG_TAG, "FF effect create periodic: dev=%d slot=%d axis=%lu result=%s",
              dev, slot, (unsigned long)axis, ok ? "ok" : "failed");
    return ok;
}

int td5_plat_ff_init(int device_slot)
{
    DIDEVCAPS caps;

    if (device_slot < 0 || device_slot >= TD5_PLAT_MAX_JS_SLOTS) {
        return 0;
    }

    /* Force feedback runs on this player's joystick (the wheel/pad). */
    if (!s_di_joystick[device_slot]) {
        return 0;
    }

    /* Re-init: drop any prior effects for this device first. */
    if (s_ff[device_slot].device) {
        td5_ff_release_effects(device_slot);
    }
    ZeroMemory(&s_ff[device_slot], sizeof(s_ff[device_slot]));
    s_ff[device_slot].device = (IDirectInputDevice8 *)s_di_joystick[device_slot];
    s_ff[device_slot].axes[0] = DIJOFS_X;
    s_ff[device_slot].axes[1] = DIJOFS_Y;
    s_ff[device_slot].directions[0] = 0;
    s_ff[device_slot].directions[1] = 0;

    ZeroMemory(&caps, sizeof(caps));
    caps.dwSize = sizeof(caps);
    if (FAILED(IDirectInputDevice8_GetCapabilities(s_ff[device_slot].device, &caps)) ||
        !(caps.dwFlags & DIDC_FORCEFEEDBACK)) {
        ZeroMemory(&s_ff[device_slot], sizeof(s_ff[device_slot]));
        return 0;
    }

    s_ff[device_slot].is_wheel = ((GET_DIDEVICE_TYPE(caps.dwDevType) == DI8DEVTYPE_DRIVING) ||
                     (GET_DIDEVICE_TYPE(caps.dwDevType) == DI8DEVTYPE_1STPERSON));

    IDirectInputDevice8_Acquire(s_ff[device_slot].device);

    if (FAILED(IDirectInputDevice8_EnumEffects(s_ff[device_slot].device,
            EnumConstantForceEffectsCallback, &s_ff[device_slot].effectGuid, DIEFT_ALL)) ||
        !IsEqualGUID(&s_ff[device_slot].effectGuid, &GUID_ConstantForce)) {
        ZeroMemory(&s_ff[device_slot], sizeof(s_ff[device_slot]));
        return 0;
    }

    if (!td5_ff_create_constant_effect(device_slot, 0, s_ff[device_slot].axes[0], 0) ||
        !td5_ff_create_constant_effect(device_slot, 1, s_ff[device_slot].axes[1], 9000) ||
        !td5_ff_create_constant_effect(device_slot, 2, s_ff[device_slot].axes[0], 0) ||
        !td5_ff_create_periodic_effect(device_slot, 3)) {
        td5_ff_release_effects(device_slot);
        ZeroMemory(&s_ff[device_slot], sizeof(s_ff[device_slot]));
        return 0;
    }

    TD5_LOG_I(LOG_TAG, "FF init ok: dev=%d is_wheel=%d", device_slot, s_ff[device_slot].is_wheel);
    return 1;
}

void td5_plat_ff_shutdown(void)
{
    int dev;
    for (dev = 0; dev < TD5_PLAT_MAX_JS_SLOTS; dev++) {
        if (s_ff[dev].device) {
            td5_ff_release_effects(dev);
        }
        ZeroMemory(&s_ff[dev], sizeof(s_ff[dev]));
    }
}

void td5_plat_ff_constant(int device_slot, int slot, int magnitude)
{
    HRESULT hr;

    if (device_slot < 0 || device_slot >= TD5_PLAT_MAX_JS_SLOTS) {
        return;
    }
    if (slot < 0 || slot >= 4 || !s_ff[device_slot].effects[slot]) {
        return;
    }

    if (slot == 3) {
        DIPERIODIC periodic;
        DIEFFECT eff;
        LONG direction = (magnitude >= 0) ? 0 : 18000;

        if (magnitude < 0) {
            magnitude = -magnitude;
        }
        if (magnitude > DI_FFNOMINALMAX) {
            magnitude = DI_FFNOMINALMAX;
        }

        ZeroMemory(&periodic, sizeof(periodic));
        ZeroMemory(&eff, sizeof(eff));
        periodic.dwMagnitude = (DWORD)magnitude;
        periodic.dwPeriod = DI_SECONDS / 8;

        eff.dwSize = sizeof(eff);
        eff.cAxes = 1;
        eff.rgdwAxes = &s_ff[device_slot].axes[0];
        eff.rglDirection = &direction;
        eff.cbTypeSpecificParams = sizeof(periodic);
        eff.lpvTypeSpecificParams = &periodic;

        hr = IDirectInputEffect_SetParameters(s_ff[device_slot].effects[slot], &eff,
            DIEP_AXES | DIEP_DIRECTION | DIEP_TYPESPECIFICPARAMS);
    } else {
        DICONSTANTFORCE cf;
        DIEFFECT eff;
        LONG direction = (magnitude >= 0) ? 0 : 18000;
        DWORD axis = (slot == 1) ? s_ff[device_slot].axes[1] : s_ff[device_slot].axes[0];

        if (magnitude < 0) {
            magnitude = -magnitude;
        }
        if (magnitude > DI_FFNOMINALMAX) {
            magnitude = DI_FFNOMINALMAX;
        }

        ZeroMemory(&cf, sizeof(cf));
        ZeroMemory(&eff, sizeof(eff));
        cf.lMagnitude = magnitude;

        eff.dwSize = sizeof(eff);
        eff.cAxes = 1;
        eff.rgdwAxes = &axis;
        eff.rglDirection = &direction;
        eff.cbTypeSpecificParams = sizeof(cf);
        eff.lpvTypeSpecificParams = &cf;

        hr = IDirectInputEffect_SetParameters(s_ff[device_slot].effects[slot], &eff,
            DIEP_AXES | DIEP_DIRECTION | DIEP_TYPESPECIFICPARAMS);
    }

    if (SUCCEEDED(hr)) {
        IDirectInputEffect_Start(s_ff[device_slot].effects[slot], 1, 0);
    }
}

void td5_plat_ff_stop(int device_slot, int slot)
{
    if (device_slot < 0 || device_slot >= TD5_PLAT_MAX_JS_SLOTS) {
        return;
    }
    if (slot < 0 || slot >= 4 || !s_ff[device_slot].effects[slot]) {
        return;
    }
    IDirectInputEffect_Stop(s_ff[device_slot].effects[slot]);
}

/* ========================================================================
 * Audio -- DirectSound8
 * ======================================================================== */

typedef struct TD5_WavFileInfo {
    WAVEFORMATEX wfx;
    DWORD        data_offset;
    DWORD        data_size;
} TD5_WavFileInfo;

static int td5_stream_parse_wav(FILE *fp, TD5_WavFileInfo *info)
{
    uint8_t riff[12];
    uint8_t chunk_header[8];
    uint8_t fmt_buf[40];
    int have_fmt = 0;
    int have_data = 0;

    if (!fp || !info) {
        return 0;
    }

    if (fseek(fp, 0, SEEK_SET) != 0 || fread(riff, 1, sizeof(riff), fp) != sizeof(riff)) {
        return 0;
    }
    if (memcmp(riff, "RIFF", 4) != 0 || memcmp(riff + 8, "WAVE", 4) != 0) {
        return 0;
    }

    ZeroMemory(info, sizeof(*info));

    while (fread(chunk_header, 1, sizeof(chunk_header), fp) == sizeof(chunk_header)) {
        DWORD chunk_size = *(DWORD *)(void *)(chunk_header + 4);
        long chunk_pos = ftell(fp);

        if (memcmp(chunk_header, "fmt ", 4) == 0) {
            size_t copy_size = chunk_size < sizeof(fmt_buf) ? chunk_size : sizeof(fmt_buf);
            ZeroMemory(fmt_buf, sizeof(fmt_buf));
            if (fread(fmt_buf, 1, copy_size, fp) != copy_size) {
                return 0;
            }
            if (fseek(fp, chunk_pos + (long)((chunk_size + 1u) & ~1u), SEEK_SET) != 0) {
                return 0;
            }

            ZeroMemory(&info->wfx, sizeof(info->wfx));
            memcpy(&info->wfx, fmt_buf,
                copy_size < sizeof(info->wfx) ? copy_size : sizeof(info->wfx));
            have_fmt = 1;
        } else if (memcmp(chunk_header, "data", 4) == 0) {
            info->data_offset = (DWORD)chunk_pos;
            info->data_size = chunk_size;
            if (fseek(fp, chunk_pos + (long)((chunk_size + 1u) & ~1u), SEEK_SET) != 0) {
                return 0;
            }
            have_data = 1;
        } else {
            if (fseek(fp, chunk_pos + (long)((chunk_size + 1u) & ~1u), SEEK_SET) != 0) {
                return 0;
            }
        }

        if (have_fmt && have_data) {
            break;
        }
    }

    if (!have_fmt || !have_data) {
        return 0;
    }

    return (fseek(fp, (long)info->data_offset, SEEK_SET) == 0);
}

static int td5_stream_rewind_data(void)
{
    if (!s_audio_stream.fp) {
        return 0;
    }
    if (fseek(s_audio_stream.fp, (long)s_audio_stream.data_offset, SEEK_SET) != 0) {
        return 0;
    }
    s_audio_stream.data_cursor = 0;
    s_audio_stream.eof = 0;
    return 1;
}

static void td5_stream_fill_half(int half_index)
{
    BYTE *ptr1 = NULL, *ptr2 = NULL;
    DWORD sz1 = 0, sz2 = 0;
    DWORD offset;
    DWORD total_size;
    DWORD written = 0;
    DWORD align;
    HRESULT hr;

    if (!s_audio_stream.buffer || !s_audio_stream.fp) {
        return;
    }

    offset = (DWORD)half_index * s_audio_stream.half_bytes;
    total_size = s_audio_stream.half_bytes;
    align = s_audio_stream.wfx.nBlockAlign ? s_audio_stream.wfx.nBlockAlign : 1;
    total_size -= total_size % align;
    if (total_size == 0) {
        total_size = s_audio_stream.half_bytes;
    }

    hr = IDirectSoundBuffer_Lock(s_audio_stream.buffer, offset, total_size,
        (void **)&ptr1, &sz1, (void **)&ptr2, &sz2, 0);
    if (FAILED(hr)) {
        return;
    }

    if (ptr1 && sz1) {
        ZeroMemory(ptr1, sz1);
    }
    if (ptr2 && sz2) {
        ZeroMemory(ptr2, sz2);
    }

    while (written < total_size) {
        DWORD remaining = s_audio_stream.data_size - s_audio_stream.data_cursor;
        DWORD request = total_size - written;
        BYTE *dst;
        DWORD segment_remaining;

        if (written < sz1) {
            dst = ptr1 + written;
            segment_remaining = sz1 - written;
        } else {
            dst = ptr2 + (written - sz1);
            segment_remaining = sz2 - (written - sz1);
        }
        if (request > segment_remaining) {
            request = segment_remaining;
        }

        if (remaining == 0) {
            if (!s_audio_stream.loop || !td5_stream_rewind_data()) {
                s_audio_stream.eof = 1;
                break;
            }
            remaining = s_audio_stream.data_size - s_audio_stream.data_cursor;
        }

        if (request > remaining) {
            request = remaining;
        }
        request -= request % align;
        if (request == 0) {
            request = remaining < align ? remaining : align;
        }

        if (fread(dst, 1, request, s_audio_stream.fp) != request) {
            s_audio_stream.eof = 1;
            break;
        }

        s_audio_stream.data_cursor += request;
        written += request;
    }

    IDirectSoundBuffer_Unlock(s_audio_stream.buffer, ptr1, sz1, ptr2, sz2);
}

int td5_plat_audio_init(void)
{
    HRESULT hr;
    DSBUFFERDESC desc;
    WAVEFORMATEX wfx;

    hr = DirectSoundCreate8(NULL, &s_dsound, NULL);
    if (FAILED(hr) || !s_dsound) {
        s_dsound = NULL;
        return 0;
    }

    hr = IDirectSound8_SetCooperativeLevel(s_dsound,
        s_hwnd ? s_hwnd : GetDesktopWindow(),
        DSSCL_PRIORITY);
    if (FAILED(hr)) {
        IDirectSound8_Release(s_dsound);
        s_dsound = NULL;
        return 0;
    }

    /* Create primary buffer for format control */
    ZeroMemory(&desc, sizeof(desc));
    desc.dwSize  = sizeof(DSBUFFERDESC);
    desc.dwFlags = DSBCAPS_PRIMARYBUFFER;
    hr = IDirectSound8_CreateSoundBuffer(s_dsound, &desc, &s_ds_primary, NULL);
    if (SUCCEEDED(hr) && s_ds_primary) {
        ZeroMemory(&wfx, sizeof(wfx));
        wfx.wFormatTag      = WAVE_FORMAT_PCM;
        wfx.nChannels        = 2;
        wfx.nSamplesPerSec   = 22050;
        wfx.wBitsPerSample   = 16;
        wfx.nBlockAlign       = wfx.nChannels * wfx.wBitsPerSample / 8;
        wfx.nAvgBytesPerSec   = wfx.nSamplesPerSec * wfx.nBlockAlign;
        IDirectSoundBuffer_SetFormat(s_ds_primary, &wfx);

        /* Keep the primary buffer — and therefore the DirectSound mixer —
         * running continuously. Without this the mixer goes idle whenever no
         * secondary buffer is playing; the next sound then incurs a wake-up
         * latency that swallows the start of short one-shots. In the frontend,
         * where only brief (~0.1s) selection/transition one-shots play with
         * gaps between them, that latency ate the whole sound, so menu SFX were
         * inaudible even though they were played at full volume. In a race the
         * continuous engine loops keep the mixer alive, so race SFX were never
         * affected — which is why the symptom was frontend-only. Playing the
         * primary looping (it outputs silence; it is the mix target) keeps the
         * pipeline hot and does not change any race behavior. */
        IDirectSoundBuffer_Play(s_ds_primary, 0, 0, DSBPLAY_LOOPING);
    }

    /* Silent keepalive: a continuously-looping SECONDARY buffer of pure silence.
     * This is what actually keeps the audio stream from idling between sounds —
     * playing the primary buffer alone proved insufficient on WDM/WASAPI, where
     * the render stream still ramps down when no secondary buffer is active. A
     * looping silent secondary keeps a live stream at all times, so short menu
     * one-shots (ping/whoosh) play instantly instead of being swallowed by the
     * device wake-up ramp. It is silence at minimum volume — inaudible, and it
     * changes nothing else (the race already stayed hot via its engine loops). */
    {
        DSBUFFERDESC kd;
        WAVEFORMATEX kwfx;
        const DWORD ka_bytes = 22050; /* ~0.5 s of 22050 Hz 16-bit mono silence */
        ZeroMemory(&kwfx, sizeof(kwfx));
        kwfx.wFormatTag      = WAVE_FORMAT_PCM;
        kwfx.nChannels       = 1;
        kwfx.nSamplesPerSec  = 22050;
        kwfx.wBitsPerSample  = 16;
        kwfx.nBlockAlign     = kwfx.nChannels * kwfx.wBitsPerSample / 8;
        kwfx.nAvgBytesPerSec = kwfx.nSamplesPerSec * kwfx.nBlockAlign;
        ZeroMemory(&kd, sizeof(kd));
        kd.dwSize        = sizeof(DSBUFFERDESC);
        kd.dwFlags       = DSBCAPS_GLOBALFOCUS | DSBCAPS_CTRLVOLUME;
        kd.dwBufferBytes = ka_bytes;
        kd.lpwfxFormat   = &kwfx;
        if (SUCCEEDED(IDirectSound8_CreateSoundBuffer(s_dsound, &kd, &s_ds_keepalive, NULL))
            && s_ds_keepalive) {
            void *p1 = NULL, *p2 = NULL; DWORD b1 = 0, b2 = 0;
            if (SUCCEEDED(IDirectSoundBuffer_Lock(s_ds_keepalive, 0, ka_bytes,
                                                  &p1, &b1, &p2, &b2, 0))) {
                /* Fill with a tiny alternating ±1 signal, NOT zeros: a pure-zero
                 * or minimum-volume buffer is detected as silent by the WASAPI
                 * audio engine, which then idles the device anyway (defeating
                 * the keepalive). ±1 at 16-bit is ~-90 dB — inaudible — but a
                 * genuine non-zero stream the engine must keep rendering. */
                int16_t *s1 = (int16_t *)p1;
                int16_t *s2 = (int16_t *)p2;
                DWORD n;
                for (n = 0; n < b1 / 2; n++) s1[n] = (n & 1) ? 1 : -1;
                if (s2) for (n = 0; n < b2 / 2; n++) s2[n] = (n & 1) ? 1 : -1;
                IDirectSoundBuffer_Unlock(s_ds_keepalive, p1, b1, p2, b2);
            }
            /* Play at full volume (DSBVOLUME_MAX = 0). Setting it to MIN makes
             * the engine treat the stream as silent and idle the device — the
             * original cause of menu transition sounds being dropped after a
             * pause. The ±1 data is inaudible, so full volume is safe. */
            IDirectSoundBuffer_SetVolume(s_ds_keepalive, DSBVOLUME_MAX);
            IDirectSoundBuffer_Play(s_ds_keepalive, 0, 0, DSBPLAY_LOOPING);
            TD5_LOG_I(LOG_TAG, "audio keepalive buffer started (silent loop)");
        } else {
            TD5_LOG_W(LOG_TAG, "audio keepalive buffer creation failed");
        }
    }

    s_audio_buf_count = 0;
    ZeroMemory(s_ds_buffer_rates, sizeof(s_ds_buffer_rates));
    return 1;
}

void td5_plat_audio_shutdown(void)
{
    int i;

    td5_plat_audio_stream_stop();

    if (s_ds_keepalive) {
        IDirectSoundBuffer_Stop(s_ds_keepalive);
        IDirectSoundBuffer_Release(s_ds_keepalive);
        s_ds_keepalive = NULL;
    }

    for (i = 0; i < MAX_AUDIO_CHANNELS; i++) {
        if (s_ds_channels[i]) {
            IDirectSoundBuffer_Stop(s_ds_channels[i]);
            IDirectSoundBuffer_Release(s_ds_channels[i]);
            s_ds_channels[i] = NULL;
        }
    }
    for (i = 0; i < s_audio_buf_count; i++) {
        if (s_ds_buffers[i]) {
            IDirectSoundBuffer_Release(s_ds_buffers[i]);
            s_ds_buffers[i] = NULL;
        }
        s_ds_buffer_rates[i] = 0;
    }
    s_audio_buf_count = 0;

    if (s_ds_primary) {
        IDirectSoundBuffer_Release(s_ds_primary);
        s_ds_primary = NULL;
    }
    if (s_dsound) {
        IDirectSound8_Release(s_dsound);
        s_dsound = NULL;
    }
}

int td5_plat_audio_load_wav(const void *data, size_t size)
{
    const uint8_t *p = (const uint8_t *)data;
    const uint8_t *end = p + size;
    WAVEFORMATEX wfx;
    DSBUFFERDESC desc;
    LPDIRECTSOUNDBUFFER buf = NULL;
    HRESULT hr;
    const uint8_t *fmt_chunk = NULL;
    const uint8_t *data_chunk = NULL;
    uint32_t data_size = 0;
    void *ptr1 = NULL, *ptr2 = NULL;
    DWORD sz1 = 0, sz2 = 0;
    int idx;

    if (!s_dsound) {
        TD5_LOG_E(LOG_TAG, "Audio load rejected: DirectSound is NULL");
        return -1;
    }
    if (!data) {
        TD5_LOG_E(LOG_TAG, "Audio load rejected: data is NULL");
        return -1;
    }
    if (size < 44) {
        TD5_LOG_E(LOG_TAG, "Audio load rejected: WAV too small (%u bytes)",
                  (unsigned int)size);
        return -1;
    }
    if (s_audio_buf_count >= MAX_AUDIO_BUFFERS) {
        TD5_LOG_E(LOG_TAG, "Audio load rejected: buffer pool full (%d)",
                  s_audio_buf_count);
        return -1;
    }

    /* Parse RIFF WAV header */
    if (memcmp(p, "RIFF", 4) != 0) return -1;
    if (memcmp(p + 8, "WAVE", 4) != 0) return -1;

    /* Find fmt and data chunks */
    p += 12;
    while (p + 8 <= end) {
        uint32_t chunk_size = *(const uint32_t *)(p + 4);
        const uint8_t *next = p + 8 + ((chunk_size + 1u) & ~1u);
        if (next < p || next > end) {
            TD5_LOG_E(LOG_TAG, "Audio load rejected: malformed WAV chunk overruns buffer");
            return -1;
        }
        if (memcmp(p, "fmt ", 4) == 0) {
            fmt_chunk = p + 8;
        } else if (memcmp(p, "data", 4) == 0) {
            data_chunk = p + 8;
            data_size = chunk_size;
        }
        p = next; /* chunk data + padding */
    }

    if (!fmt_chunk || !data_chunk || data_size == 0) {
        TD5_LOG_E(LOG_TAG, "Audio load rejected: missing fmt/data chunk");
        return -1;
    }
    if (fmt_chunk + 16 > end) {
        TD5_LOG_E(LOG_TAG, "Audio load rejected: truncated fmt chunk");
        return -1;
    }
    if (data_chunk + data_size > end) {
        TD5_LOG_E(LOG_TAG, "Audio load rejected: truncated data chunk size=%u file=%u",
                  (unsigned int)data_size, (unsigned int)size);
        return -1;
    }

    /* Build WAVEFORMATEX from fmt chunk */
    ZeroMemory(&wfx, sizeof(wfx));
    wfx.wFormatTag      = *(const uint16_t *)(fmt_chunk + 0);
    wfx.nChannels        = *(const uint16_t *)(fmt_chunk + 2);
    wfx.nSamplesPerSec   = *(const uint32_t *)(fmt_chunk + 4);
    wfx.nAvgBytesPerSec  = *(const uint32_t *)(fmt_chunk + 8);
    wfx.nBlockAlign       = *(const uint16_t *)(fmt_chunk + 12);
    wfx.wBitsPerSample   = *(const uint16_t *)(fmt_chunk + 14);

    /* Create sound buffer.
     * DSBCAPS_GLOBALFOCUS: keep SFX audible when the game window is not the
     * strict foreground window. Without it, DirectSound mutes these buffers
     * (and their DuplicateSoundBuffer copies) on focus loss — which silenced
     * the frontend selection/transition one-shots while the player was tabbing
     * between the (windowed) game and other windows, even though the buffers
     * were played at full volume. The streaming music buffer already sets this
     * flag; SFX must match it so menu and in-race SFX behave consistently. */
    ZeroMemory(&desc, sizeof(desc));
    desc.dwSize          = sizeof(DSBUFFERDESC);
    desc.dwFlags         = DSBCAPS_CTRLVOLUME | DSBCAPS_CTRLPAN | DSBCAPS_CTRLFREQUENCY
                         | DSBCAPS_GLOBALFOCUS | DSBCAPS_STATIC;
    desc.dwBufferBytes   = data_size;
    desc.lpwfxFormat     = &wfx;

    hr = IDirectSound8_CreateSoundBuffer(s_dsound, &desc, &buf, NULL);
    if (FAILED(hr) || !buf) {
        TD5_LOG_E(LOG_TAG, "CreateSoundBuffer failed hr=0x%08lX size=%u",
                  (unsigned long)hr, (unsigned int)data_size);
        return -1;
    }

    /* Copy WAV data into the buffer */
    hr = IDirectSoundBuffer_Lock(buf, 0, data_size, &ptr1, &sz1, &ptr2, &sz2, 0);
    if (FAILED(hr)) {
        TD5_LOG_E(LOG_TAG, "SoundBuffer lock failed hr=0x%08lX", (unsigned long)hr);
        IDirectSoundBuffer_Release(buf);
        return -1;
    }
    if (ptr1 && sz1) memcpy(ptr1, data_chunk, sz1 < data_size ? sz1 : data_size);
    if (ptr2 && sz2 && data_size > sz1) memcpy(ptr2, data_chunk + sz1, sz2);
    IDirectSoundBuffer_Unlock(buf, ptr1, sz1, ptr2, sz2);

    idx = s_audio_buf_count++;
    s_ds_buffers[idx] = buf;
    s_ds_buffer_rates[idx] = wfx.nSamplesPerSec;
    TD5_LOG_I(LOG_TAG,
              "Audio buffer created: id=%d fmt=%uHz/%uch/%ubit size=%u",
              idx,
              (unsigned int)wfx.nSamplesPerSec,
              (unsigned int)wfx.nChannels,
              (unsigned int)wfx.wBitsPerSample,
              (unsigned int)data_size);
    return idx;
}

void td5_plat_audio_free(int buffer_index)
{
    if (buffer_index < 0 || buffer_index >= s_audio_buf_count) return;
    if (s_ds_buffers[buffer_index]) {
        IDirectSoundBuffer_Release(s_ds_buffers[buffer_index]);
        s_ds_buffers[buffer_index] = NULL;
    }
}

/* Stop + release the voice at channel index `i` and update the live count.
 * The generation is NOT bumped here — it is bumped on the next allocation of
 * this index, which is what invalidates any stale handle still pointing at it. */
static void audio_release_channel(int i)
{
    if (i < 0 || i >= MAX_AUDIO_CHANNELS) return;
    if (s_ds_channels[i]) {
        IDirectSoundBuffer_Stop(s_ds_channels[i]);
        IDirectSoundBuffer_Release(s_ds_channels[i]);
        s_ds_channels[i] = NULL;
        s_ds_channel_loop[i] = 0;
        if (s_audio_active_count > 0) s_audio_active_count--;
        s_audio_free_count++;
    }
}

/* Resolve a (generation,index) handle back to a live channel index, or -1 if
 * the handle is stale (the index was recycled/stolen for a different sound) or
 * the channel is no longer live. This is the guard that stops a stale
 * s_slot_to_channel entry from cross-talking onto another slot's voice. */
static int audio_resolve_channel(TD5_AudioChannel h)
{
    int idx;
    uint32_t gen;
    if (h < 0) return -1;
    idx = (int)((uint32_t)h & AUDIO_CHANNEL_INDEX_MASK);
    gen = (uint32_t)h >> AUDIO_CHANNEL_INDEX_BITS;
    if (idx < 0 || idx >= MAX_AUDIO_CHANNELS) return -1;
    if (!s_ds_channels[idx]) return -1;
    if (s_ds_channel_gen[idx] != gen) return -1;
    return idx;
}

/* Pick a channel index for a new voice.
 *
 *   1. an empty slot, else
 *   2. a finished (non-looping one-shot that stopped) slot — reclaimed lazily,
 *      else
 *   3. STEAL the least-important busy voice: the oldest non-looping voice if any
 *      is still playing, otherwise the oldest looping voice.
 *
 * Pass 3 is the critical S11 fix. The original M2DX DXSound::Play never fails on
 * a full bank — it walks the duplicate chain and steals the tail voice (see
 * re/analysis/wave4_deep_audits/da_m3_dxsound_polyphony.md §B). The old port
 * returned -1 once all 64 channels were busy; because looping engine/siren/
 * traffic voices never report "stopped", pass 2 then found nothing and every
 * later Play failed — the mix stuck on whatever loops already held the pool
 * (the "plays one sound for the rest of the session" symptom, made more likely
 * by >6 players spawning more loops). Stealing guarantees forward progress:
 * Play always returns a usable voice, and a stolen looping voice that is still
 * wanted simply re-Plays next frame (its old handle resolves stale, so
 * slot_is_playing() reports idle and the mixer restarts it).
 *
 * Victim choice (rare — only under genuine >64 concurrency): steal the OLDEST
 * one-shot first (it is the closest to finishing, so cutting it is least
 * audible), and only if every busy voice is looping, steal the NEWEST loop. The
 * long-lived loops carry the stable bed — the local player's own engine and the
 * ambient/rain/siren layers are started first (lowest serial), so stealing the
 * newest loop preserves them and sacrifices the most recently spawned overflow
 * voice instead. */
static int find_free_channel(void)
{
    int i;
    int steal = -1;
    uint32_t steal_serial = 0;

    /* Pass 1: an empty slot. */
    for (i = 0; i < MAX_AUDIO_CHANNELS; i++) {
        if (!s_ds_channels[i]) return i;
    }
    /* Pass 2: a busy-but-finished slot (one-shot that already stopped). */
    for (i = 0; i < MAX_AUDIO_CHANNELS; i++) {
        DWORD status = 0;
        IDirectSoundBuffer_GetStatus(s_ds_channels[i], &status);
        if (!(status & DSBSTATUS_PLAYING)) {
            audio_release_channel(i);
            return i;
        }
    }
    /* Pass 3a: steal the oldest still-playing one-shot (lowest serial, loop==0). */
    for (i = 0; i < MAX_AUDIO_CHANNELS; i++) {
        if (s_ds_channel_loop[i]) continue;
        if (steal < 0 || s_ds_channel_serial[i] < steal_serial) {
            steal = i;
            steal_serial = s_ds_channel_serial[i];
        }
    }
    /* Pass 3b: no one-shot to steal — steal the newest loop (highest serial). */
    if (steal < 0) {
        for (i = 0; i < MAX_AUDIO_CHANNELS; i++) {
            if (steal < 0 || s_ds_channel_serial[i] > steal_serial) {
                steal = i;
                steal_serial = s_ds_channel_serial[i];
            }
        }
    }
    if (steal >= 0) {
        s_audio_steal_count++;
        audio_release_channel(steal);
        return steal;
    }
    return -1;
}

/** Convert 0-100 linear volume to DirectSound's logarithmic dB scale.
 *  DS range: DSBVOLUME_MIN (-10000) to DSBVOLUME_MAX (0). */
static LONG vol_to_ds(int volume)
{
    if (volume <= 0)   return DSBVOLUME_MIN;
    if (volume >= 100) return DSBVOLUME_MAX;
    /* Perceptual amplitude-dB attenuation: 20*log10(v/100), in DirectSound's
     * hundredths-of-a-dB. The old -50*(100-v) was linear-IN-dB and far too steep
     * (v=50 -> -25 dB), which buried every mid-volume sound: a wall scrape at
     * ~v48 mapped to -26 dB and was masked under the engine, so it was inaudible
     * even at full SFX volume. Log maps v=50 -> -6 dB, v=25 -> -12 dB, v=10 ->
     * -20 dB, keeping quieter one-shots (scrape, distant hits) audible while the
     * loud sounds (engine/collisions near v100) stay at full level. */
    LONG ds = (LONG)(2000.0 * log10((double)volume / 100.0));
    if (ds < DSBVOLUME_MIN) ds = DSBVOLUME_MIN;
    if (ds > DSBVOLUME_MAX) ds = DSBVOLUME_MAX;
    return ds;
}

/** Pass through a native DirectSound-scale pan (-10000..+10000).
 *
 * The TD5 sound module (td5_sound.c) emits pans already in DirectSound's
 * native units, mirroring the original game's DXSound::Modify calls:
 *   0           = centered (most sounds; all non-split-screen AI/traffic)
 *   +/-~491 max = single-player steering pan (UpdateVehicleAudioMix 0x441160:
 *                 steering_command * -0x51EB851F >> 38, range +/-0x18000)
 *   +/-10000    = split-screen viewport sides (literal in the original)
 *
 * The previous implementation clamped the input to +/-100 then multiplied by
 * 100, which turned the subtle +/-491 steering pan into a full hard +/-10000
 * pan -- the "very aggressive stereo when steering" bug. (Split-screen +/-10000
 * survived only by coincidence: clamp to +/-100 then x100 = +/-10000.)
 * Clamp to the native DirectSound range and pass through unchanged. */
static LONG pan_to_ds(int pan)
{
    if (pan < -10000) pan = -10000;
    if (pan >  10000) pan =  10000;
    return (LONG)pan;
}

static DWORD td5_audio_translate_frequency(int buffer_index, int frequency)
{
    DWORD native_rate = 22050;
    double scaled;

    if (frequency <= 0) {
        return 0;
    }

    if (buffer_index >= 0 && buffer_index < MAX_AUDIO_BUFFERS &&
        s_ds_buffer_rates[buffer_index] != 0) {
        native_rate = s_ds_buffer_rates[buffer_index];
    }

    scaled = ((double)frequency * (double)native_rate) / 22050.0;
    if (scaled < (double)DSBFREQUENCY_MIN) scaled = (double)DSBFREQUENCY_MIN;
    if (scaled > (double)DSBFREQUENCY_MAX) scaled = (double)DSBFREQUENCY_MAX;
    return (DWORD)(scaled + 0.5);
}

TD5_AudioChannel td5_plat_audio_play(int buffer_index, int loop,
                                      int volume, int pan, int frequency)
{
    LPDIRECTSOUNDBUFFER src_buf, dup_buf = NULL;
    HRESULT hr;
    int ch;

    if (!s_dsound) return TD5_AUDIO_INVALID_CHANNEL;
    if (buffer_index < 0 || buffer_index >= s_audio_buf_count) return TD5_AUDIO_INVALID_CHANNEL;
    src_buf = s_ds_buffers[buffer_index];
    if (!src_buf) { s_audio_fail_count++; return TD5_AUDIO_INVALID_CHANNEL; }

    ch = find_free_channel();
    if (ch < 0) { s_audio_fail_count++; return TD5_AUDIO_INVALID_CHANNEL; }

    /* Duplicate the buffer so multiple instances can play simultaneously */
    hr = IDirectSound8_DuplicateSoundBuffer(s_dsound, src_buf, &dup_buf);
    if (FAILED(hr) || !dup_buf) { s_audio_fail_count++; return TD5_AUDIO_INVALID_CHANNEL; }

    /* Claim the index: bump its generation so any stale handle that still names
     * this index resolves invalid, and record alloc order + loop flag for the
     * voice-steal policy. */
    s_ds_channel_gen[ch]++;
    if (s_ds_channel_gen[ch] > AUDIO_CHANNEL_GEN_MAX) s_ds_channel_gen[ch] = 1;
    s_ds_channels[ch] = dup_buf;
    s_ds_channel_buf[ch] = buffer_index;
    s_ds_channel_serial[ch] = ++s_audio_alloc_serial;
    s_ds_channel_loop[ch] = loop ? 1 : 0;
    s_audio_alloc_count++;
    s_audio_active_count++;
    if (s_audio_active_count > s_audio_peak_active) s_audio_peak_active = s_audio_active_count;

    /* Apply parameters */
    {
        int effective_vol = s_audio_muted ? 0 : (volume * s_master_volume) / 100;
        IDirectSoundBuffer_SetVolume(dup_buf, vol_to_ds(effective_vol));
    }
    IDirectSoundBuffer_SetPan(dup_buf, pan_to_ds(pan));
    if (frequency > 0)
        IDirectSoundBuffer_SetFrequency(dup_buf, td5_audio_translate_frequency(buffer_index, frequency));

    IDirectSoundBuffer_SetCurrentPosition(dup_buf, 0);
    IDirectSoundBuffer_Play(dup_buf, 0, 0, loop ? DSBPLAY_LOOPING : 0);

    TD5_LOG_I(LOG_TAG, "Audio channel play: channel=%d gen=%u buffer=%d loop=%d active=%d",
              ch, (unsigned)s_ds_channel_gen[ch], buffer_index, loop, s_audio_active_count);

    return (TD5_AudioChannel)(((int)s_ds_channel_gen[ch] << AUDIO_CHANNEL_INDEX_BITS) | ch);
}

void td5_plat_audio_stop(TD5_AudioChannel ch)
{
    int idx = audio_resolve_channel(ch);
    if (idx < 0) return;
    TD5_LOG_I(LOG_TAG, "Audio channel stop: channel=%d buffer=%d active=%d",
              idx, s_ds_channel_buf[idx], s_audio_active_count - 1);
    audio_release_channel(idx);
}

void td5_plat_audio_modify(TD5_AudioChannel ch, int volume, int pan, int frequency)
{
    LPDIRECTSOUNDBUFFER buf;
    int idx = audio_resolve_channel(ch);
    if (idx < 0) return;
    buf = s_ds_channels[idx];

    {
        int effective_vol = s_audio_muted ? 0 : (volume * s_master_volume) / 100;
        IDirectSoundBuffer_SetVolume(buf, vol_to_ds(effective_vol));
    }
    IDirectSoundBuffer_SetPan(buf, pan_to_ds(pan));
    if (frequency > 0)
        IDirectSoundBuffer_SetFrequency(buf, td5_audio_translate_frequency(s_ds_channel_buf[idx], frequency));
}

int td5_plat_audio_is_playing(TD5_AudioChannel ch)
{
    DWORD status = 0;
    int idx = audio_resolve_channel(ch);
    if (idx < 0) return 0;
    IDirectSoundBuffer_GetStatus(s_ds_channels[idx], &status);
    return (status & DSBSTATUS_PLAYING) ? 1 : 0;
}

/* Release every active SFX voice (NOT the primary or silent-keepalive buffers).
 * Called at race teardown so voice counts return to baseline between races and
 * no looping engine/siren/traffic voice can survive into the next race or the
 * menus. The keepalive stays running so menu one-shots remain instant. */
void td5_plat_audio_stop_all(void)
{
    int i;
    for (i = 0; i < MAX_AUDIO_CHANNELS; i++) {
        audio_release_channel(i);
    }
}

int td5_plat_audio_active_channels(void)
{
    return s_audio_active_count;
}

void td5_plat_audio_log_stats(const char *tag)
{
    TD5_LOG_I(LOG_TAG,
              "voice stats [%s]: active=%d peak=%d alloc=%u free=%u steal=%u fail=%u",
              tag ? tag : "",
              s_audio_active_count, s_audio_peak_active,
              (unsigned)s_audio_alloc_count, (unsigned)s_audio_free_count,
              (unsigned)s_audio_steal_count, (unsigned)s_audio_fail_count);
}

void td5_plat_audio_set_master_volume(int volume)
{
    if (volume < 0)   volume = 0;
    if (volume > 100) volume = 100;
    s_master_volume = volume;
}

void td5_plat_audio_set_muted(int muted)
{
    s_audio_muted = muted ? 1 : 0;
}

int td5_plat_audio_stream_play(const char *wav_path, int loop)
{
    TD5_WavFileInfo info;
    DSBUFFERDESC desc;
    FILE *fp;
    HRESULT hr;

    if (!s_dsound || !wav_path) {
        return 0;
    }

    td5_plat_audio_stream_stop();

    fp = fopen(wav_path, "rb");
    if (!fp) {
        return 0;
    }
    if (!td5_stream_parse_wav(fp, &info)) {
        fclose(fp);
        return 0;
    }

    ZeroMemory(&desc, sizeof(desc));
    desc.dwSize = sizeof(desc);
    desc.dwFlags = DSBCAPS_GETCURRENTPOSITION2 | DSBCAPS_GLOBALFOCUS |
                   DSBCAPS_CTRLVOLUME | DSBCAPS_CTRLFREQUENCY;
    desc.dwBufferBytes = TD5_STREAM_BUFFER_BYTES;
    desc.lpwfxFormat = &info.wfx;

    hr = IDirectSound8_CreateSoundBuffer(s_dsound, &desc, &s_audio_stream.buffer, NULL);
    if (FAILED(hr) || !s_audio_stream.buffer) {
        fclose(fp);
        return 0;
    }

    s_audio_stream.fp = fp;
    s_audio_stream.wfx = info.wfx;
    s_audio_stream.data_offset = info.data_offset;
    s_audio_stream.data_size = info.data_size;
    s_audio_stream.data_cursor = 0;
    s_audio_stream.buffer_bytes = TD5_STREAM_BUFFER_BYTES;
    s_audio_stream.half_bytes = TD5_STREAM_BUFFER_BYTES / 2;
    s_audio_stream.last_play_cursor = 0;
    s_audio_stream.loop = loop ? 1 : 0;
    s_audio_stream.eof = 0;
    s_audio_stream.active = 1;

    td5_stream_rewind_data();
    td5_stream_fill_half(0);
    td5_stream_fill_half(1);

    IDirectSoundBuffer_SetCurrentPosition(s_audio_stream.buffer, 0);
    IDirectSoundBuffer_Play(s_audio_stream.buffer, 0, 0, DSBPLAY_LOOPING);
    TD5_LOG_I(LOG_TAG, "Audio stream open: path=%s loop=%d size=%u",
              wav_path, loop, (unsigned int)info.data_size);
    return 1;
}

void td5_plat_audio_stream_stop(void)
{
    if (s_audio_stream.active) {
        TD5_LOG_I(LOG_TAG, "Audio stream close: size=%u cursor=%u",
                  (unsigned int)s_audio_stream.data_size,
                  (unsigned int)s_audio_stream.data_cursor);
    }
    if (s_audio_stream.buffer) {
        IDirectSoundBuffer_Stop(s_audio_stream.buffer);
        IDirectSoundBuffer_Release(s_audio_stream.buffer);
    }
    if (s_audio_stream.fp) {
        fclose(s_audio_stream.fp);
    }
    ZeroMemory(&s_audio_stream, sizeof(s_audio_stream));
}

void td5_plat_audio_stream_refresh(void)
{
    DWORD status = 0;
    DWORD play_cursor = 0;
    int prev_half;
    int cur_half;

    if (!s_audio_stream.active || !s_audio_stream.buffer) {
        return;
    }

    IDirectSoundBuffer_GetStatus(s_audio_stream.buffer, &status);
    if (!(status & DSBSTATUS_PLAYING)) {
        return;
    }

    if (FAILED(IDirectSoundBuffer_GetCurrentPosition(s_audio_stream.buffer, &play_cursor, NULL))) {
        return;
    }

    prev_half = (s_audio_stream.last_play_cursor < s_audio_stream.half_bytes) ? 0 : 1;
    cur_half = (play_cursor < s_audio_stream.half_bytes) ? 0 : 1;

    if (s_audio_stream.eof && !s_audio_stream.loop && play_cursor < s_audio_stream.last_play_cursor) {
        td5_plat_audio_stream_stop();
        return;
    }

    if (cur_half != prev_half) {
        td5_stream_fill_half(prev_half);
        TD5_LOG_D(LOG_TAG, "Audio stream refresh: play_cursor=%u filled_half=%d",
                  (unsigned int)play_cursor, prev_half);
    }

    s_audio_stream.last_play_cursor = play_cursor;
}

int td5_plat_audio_stream_is_playing(void)
{
    DWORD status = 0;

    if (!s_audio_stream.active || !s_audio_stream.buffer) {
        return 0;
    }
    IDirectSoundBuffer_GetStatus(s_audio_stream.buffer, &status);
    return (status & DSBSTATUS_PLAYING) ? 1 : 0;
}

/* ========================================================================
 * CD Audio (via MCI)
 * ======================================================================== */

static int s_cd_initialized = 0;

static void cd_ensure_init(void)
{
    if (!s_cd_initialized) {
        mciSendStringA("open cdaudio", NULL, 0, NULL);
        mciSendStringA("set cdaudio time format tmsf", NULL, 0, NULL);
        s_cd_initialized = 1;
    }
}

void td5_plat_cd_play(int track)
{
    char cmd[128];
    cd_ensure_init();
    snprintf(cmd, sizeof(cmd), "play cdaudio from %d to %d", track, track + 1);
    TD5_LOG_I(LOG_TAG, "CD play track=%d", track);
    mciSendStringA(cmd, NULL, 0, NULL);
}

void td5_plat_cd_stop(void)
{
    cd_ensure_init();
    TD5_LOG_I(LOG_TAG, "CD stop");
    mciSendStringA("stop cdaudio", NULL, 0, NULL);
}

void td5_plat_cd_set_volume(int volume)
{
    char cmd[128];
    cd_ensure_init();
    /* MCI audio volume: 0-1000 */
    snprintf(cmd, sizeof(cmd), "setaudio cdaudio volume to %d",
             (volume * 1000) / 100);
    mciSendStringA(cmd, NULL, 0, NULL);
}

/* ========================================================================
 * Rendering Backend -- Bridge to D3D11 wrapper
 *
 * These call directly into the wrapper's D3D11 backend via the global
 * g_backend state and the WrapperDevice/WrapperSurface functions.
 * ======================================================================== */

void td5_plat_render_begin_scene(void)
{
    D3D11_VIEWPORT vp;
    FLOAT vp_w, vp_h;

    if (!g_backend.device || !g_backend.context) return;

    vp_w = (FLOAT)((g_backend.backbuffer && g_backend.backbuffer->width)
        ? g_backend.backbuffer->width
        : g_backend.width);
    vp_h = (FLOAT)((g_backend.backbuffer && g_backend.backbuffer->height)
        ? g_backend.backbuffer->height
        : g_backend.height);

    /* Bind backbuffer as render target so draw calls land on the surface
     * that Backend_CompositeAndPresent reads, not the swap chain. */
    if (g_backend.backbuffer && g_backend.backbuffer->d3d11_rtv) {
        ID3D11DeviceContext_OMSetRenderTargets(g_backend.context, 1,
            &g_backend.backbuffer->d3d11_rtv, g_backend.depth_dsv);
    }

    /* Reassert a valid viewport every scene. The loading screen can render
     * before gameplay code configures a viewport explicitly. */
    vp.TopLeftX = 0.0f;
    vp.TopLeftY = 0.0f;
    vp.Width    = vp_w;
    vp.Height   = vp_h;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    ID3D11DeviceContext_RSSetViewports(g_backend.context, 1, &vp);
    Backend_UpdateViewportCB(vp_w, vp_h);

    /* Reset scissor rect to the full render target at scene start so any
     * HUD-set sub-rect from the previous frame doesn't leak into this one. */
    {
        D3D11_RECT scissor;
        scissor.left   = 0;
        scissor.top    = 0;
        scissor.right  = (LONG)vp_w;
        scissor.bottom = (LONG)vp_h;
        ID3D11DeviceContext_RSSetScissorRects(g_backend.context, 1, &scissor);
    }

    /* Clear depth buffer */
    if (g_backend.depth_dsv) {
        ID3D11DeviceContext_ClearDepthStencilView(g_backend.context,
            g_backend.depth_dsv, D3D11_CLEAR_DEPTH, 1.0f, 0);
    }

    g_backend.in_scene = 1;
    g_backend.scene_rendered = 0;
    s_frame_draw_calls = 0;
    s_frame_vertices = 0;
    s_frame_indices = 0;
    s_last_bound_texture_page = -1;
}

void td5_plat_render_end_scene(void)
{
    g_backend.in_scene = 0;
    g_backend.scene_rendered = 1;
}

/* Optional frontend pixel-shader override (VectorUI / MSDF text). When set, the
 * indexed draw path binds this PS + sampler instead of the texblend-derived
 * pair it normally forces. The frontend sets it around a text batch and clears
 * it after. Typed void* so td5_platform.h stays free of D3D11 types. */
static void *s_render_ps_override = NULL;
static int   s_render_ps_override_samp = SAMP_LINEAR_CLAMP;

void td5_plat_render_set_ps_override(void *ps, int sampler_idx)
{
    s_render_ps_override = ps;
    s_render_ps_override_samp = sampler_idx;
}

void td5_plat_render_clear_ps_override(void)
{
    s_render_ps_override = NULL;
}

void td5_plat_render_draw_tris(const TD5_D3DVertex *verts, int vertex_count,
                                const uint16_t *indices, int index_count)
{
    ID3D11DeviceContext *ctx = g_backend.context;
    D3D11_MAPPED_SUBRESOURCE mapped;
    UINT stride = TD5_VERTEX_STRIDE; /* 32 bytes */
    UINT offset = 0;
    HRESULT hr;

    if (!ctx || !verts || vertex_count <= 0) return;

    s_frame_draw_calls++;
    s_frame_vertices += vertex_count;
    if (indices && index_count > 0)
        s_frame_indices += index_count;

    /* Upload vertices to dynamic VB */
    hr = ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)g_backend.dynamic_vb,
        0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr)) return;
    memcpy(mapped.pData, verts, (size_t)vertex_count * stride);
    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)g_backend.dynamic_vb, 0);

    /* Bind VB, input layout, shaders */
    ID3D11DeviceContext_IASetVertexBuffers(ctx, 0, 1, &g_backend.dynamic_vb,
                                           &stride, &offset);
    ID3D11DeviceContext_IASetInputLayout(ctx, g_backend.input_layout);
    ID3D11DeviceContext_VSSetShader(ctx, g_backend.vs_pretransformed, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(ctx, 0, 1, &g_backend.cb_viewport);
    ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 1, &g_backend.cb_fog);

    /* Apply render state cache -> D3D11 state objects */
    Backend_ApplyStateCache();

    if (indices && index_count > 0) {
        /* Indexed draw */
        hr = ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)g_backend.dynamic_ib,
            0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (FAILED(hr)) return;
        memcpy(mapped.pData, indices, (size_t)index_count * sizeof(uint16_t));
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)g_backend.dynamic_ib, 0);

        ID3D11DeviceContext_IASetIndexBuffer(ctx, g_backend.dynamic_ib,
                                              DXGI_FORMAT_R16_UINT, 0);
        ID3D11DeviceContext_IASetPrimitiveTopology(ctx,
            D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        /* Force correct PS and sampler before every draw (honor MSDF override) */
        if (s_render_ps_override) {
            ID3D11DeviceContext_PSSetShader(ctx,
                (ID3D11PixelShader *)s_render_ps_override, NULL, 0);
            ID3D11DeviceContext_PSSetSamplers(ctx, 0, 1,
                &g_backend.sampler_states[s_render_ps_override_samp]);
        } else {
            ID3D11DeviceContext_PSSetShader(ctx,
                g_backend.ps_shaders[g_backend.state.texblend_mode == 5 ? 1 : 0], NULL, 0);
            {
                int si = (g_backend.state.mag_filter >= 2) ? SAMP_LINEAR_WRAP : SAMP_POINT_WRAP;
                ID3D11DeviceContext_PSSetSamplers(ctx, 0, 1, &g_backend.sampler_states[si]);
            }
        }
        ID3D11DeviceContext_DrawIndexed(ctx, (UINT)index_count, 0, 0);
    } else {
        /* Non-indexed draw */
        ID3D11DeviceContext_IASetPrimitiveTopology(ctx,
            D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D11DeviceContext_Draw(ctx, (UINT)vertex_count, 0);
    }
}

void td5_plat_render_draw_lines(const TD5_D3DVertex *verts, int vert_count)
{
    ID3D11DeviceContext *ctx = g_backend.context;
    D3D11_MAPPED_SUBRESOURCE mapped;
    UINT stride = TD5_VERTEX_STRIDE;
    UINT offset = 0;
    HRESULT hr;

    if (!ctx || !verts || vert_count < 2) return;
    if (!g_backend.white_srv) return;
    if ((size_t)vert_count * stride > g_backend.dynamic_vb_size) return;

    hr = ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)g_backend.dynamic_vb,
        0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr)) return;
    memcpy(mapped.pData, verts, (size_t)vert_count * stride);
    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)g_backend.dynamic_vb, 0);

    ID3D11DeviceContext_IASetVertexBuffers(ctx, 0, 1, &g_backend.dynamic_vb,
                                           &stride, &offset);
    ID3D11DeviceContext_IASetInputLayout(ctx, g_backend.input_layout);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx,
        D3D11_PRIMITIVE_TOPOLOGY_LINELIST);

    ID3D11DeviceContext_VSSetShader(ctx, g_backend.vs_pretransformed, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(ctx, 0, 1, &g_backend.cb_viewport);

    /* PS_MODULATE * white texel = vertex color. Disable fog and alpha test
     * via a fresh fog CB upload bounded to this draw. */
    ID3D11DeviceContext_PSSetShader(ctx, g_backend.ps_shaders[PS_MODULATE], NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &g_backend.white_srv);
    ID3D11DeviceContext_PSSetSamplers(ctx, 0, 1, &g_backend.sampler_states[SAMP_POINT_CLAMP]);
    ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 1, &g_backend.cb_fog);
    {
        FogCB fog = {0};
        fog.fogEnabled = 0;
        fog.alphaTestEnabled = 0;
        fog.alphaRef = 0.0f;
        ID3D11DeviceContext_UpdateSubresource(ctx,
            (ID3D11Resource*)g_backend.cb_fog, 0, NULL, &fog, 0, 0);
    }

    ID3D11DeviceContext_RSSetState(ctx, g_backend.rs_state);
    /* Z test on (so lines occlude correctly), Z write off (don't poison depth
     * for subsequent overlays), no blend. */
    ID3D11DeviceContext_OMSetDepthStencilState(ctx,
        g_backend.ds_states[DS_Z_ON_WRITE_OFF], 0);
    ID3D11DeviceContext_OMSetBlendState(ctx,
        g_backend.blend_states[BLEND_OPAQUE], NULL, 0xFFFFFFFF);

    ID3D11DeviceContext_Draw(ctx, (UINT)vert_count, 0);

    /* Invalidate cached state-object indices so the next ApplyStateCache
     * actually re-binds (current_* values must mismatch what we just set). */
    g_backend.state.current_blend_idx = -1;
    g_backend.state.current_ds_idx    = -1;
    g_backend.state.current_samp_idx  = -1;
    g_backend.state.current_ps_idx    = -1;
    g_backend.state.dirty = 1;
    /* Force texture rebind on next draw — current_srv was overwritten with white. */
    g_backend.current_srv = NULL;
    s_last_bound_texture_page = -1;
}

void td5_plat_render_set_preset(TD5_RenderPreset preset)
{
    RenderStateCache *s = &g_backend.state;

    /* Default to no polygon offset; the SHADOW preset opts in explicitly. */
    s->polygon_offset = 0;

    switch (preset) {
    case TD5_PRESET_OPAQUE_ANISO:
        s->blend_enable = 0;
        s->z_enable     = 1;
        s->z_write      = 1;
        s->z_func       = 0; /* LESSEQUAL — explicit reset so SKY's ALWAYS doesn't leak */
        s->mag_filter   = 2; /* LINEAR */
        s->min_filter   = 2;
        s->texblend_mode = D3DTBLEND_MODULATE;
        s->alpha_test_enable = 1;
        s->alpha_ref         = 1; /* discard alpha < 1/255 (color-keyed pixels) */
        break;

    case TD5_PRESET_TRANSLUCENT_LINEAR:
        s->blend_enable = 1;
        s->src_blend    = D3D6BLEND_SRCALPHA;
        s->dest_blend   = D3D6BLEND_INVSRCALPHA;
        s->z_enable     = 0; /* 2D overlay: no depth test, draw order = z-order */
        s->z_write      = 0;
        s->z_func       = 0;
        s->mag_filter   = 2;
        s->min_filter   = 2;
        s->texblend_mode = D3DTBLEND_MODULATEALPHA;
        s->alpha_test_enable = 1;
        /* alpha_ref = 0x80 (1-bit-alpha cutoff): with bilinear sampling on
         * color-keyed textures the edge taps produce alphas in (0,255).
         * Discarding < 128 prunes the dark/black fringes around transparent
         * pixels (street-light backgrounds, props). Type-2 semi-transparent
         * pages sit at exactly 0x80 — strict < keeps them.
         * NOTE: HUD widgets route through TRANSLUCENT_LINEAR_HUD instead —
         * orig M2DX SetRenderState @ 0x10001770 uses ALPHAREF=0/NOTEQUAL,
         * so HUD needs alpha_ref=1 for parity. */
        s->alpha_ref         = 0x80;
        break;

    case TD5_PRESET_TRANSLUCENT_LINEAR_HUD:
        /* Same blend/filter as TRANSLUCENT_LINEAR, but alpha_ref=1 to mirror
         * orig M2DX.dll DXD3D::SetRenderState @ 0x10001770 which sets
         * D3DRS_ALPHAREF=0 + D3DRS_ALPHAFUNC=D3DCMP_NOTEQUAL (discard alpha==0
         * only). Used by hud_submit_quad so speedometer/lap-timer/digits keep
         * anti-aliased edge pixels that the 0x80 cutoff would discard. */
        s->blend_enable = 1;
        s->src_blend    = D3D6BLEND_SRCALPHA;
        s->dest_blend   = D3D6BLEND_INVSRCALPHA;
        s->z_enable     = 0;
        s->z_write      = 0;
        s->z_func       = 0;
        s->mag_filter   = 2;
        s->min_filter   = 2;
        s->texblend_mode = D3DTBLEND_MODULATEALPHA;
        s->alpha_test_enable = 1;
        s->alpha_ref         = 1;
        break;

    case TD5_PRESET_TRANSLUCENT_ANISO:
        s->blend_enable = 1;
        s->src_blend    = D3D6BLEND_SRCALPHA;
        s->dest_blend   = D3D6BLEND_INVSRCALPHA;
        s->z_enable     = 1;
        s->z_write      = 0;
        s->z_func       = 0;
        s->mag_filter   = 2;
        s->min_filter   = 2;
        s->texblend_mode = D3DTBLEND_MODULATEALPHA;
        s->alpha_test_enable = 1;
        s->alpha_ref         = 1;
        break;

    case TD5_PRESET_OPAQUE_LINEAR:
        s->blend_enable = 0;
        s->z_enable     = 1;
        s->z_write      = 1;
        s->z_func       = 0;
        s->mag_filter   = 2;
        s->min_filter   = 2;
        s->texblend_mode = D3DTBLEND_MODULATE;
        s->alpha_test_enable = 1;
        s->alpha_ref         = 1; /* discard alpha < 1/255 (color-keyed pixels) */
        break;

    case TD5_PRESET_TRANSLUCENT_POINT:
        s->blend_enable = 1;
        s->src_blend    = D3D6BLEND_SRCALPHA;
        s->dest_blend   = D3D6BLEND_INVSRCALPHA;
        s->z_enable     = 0;
        s->z_write      = 0;
        s->z_func       = 0;
        s->mag_filter   = 0; /* POINT — no bilinear bleed into transparent border pixels */
        s->min_filter   = 0;
        s->texblend_mode = D3DTBLEND_MODULATEALPHA;
        s->alpha_test_enable = 1;
        s->alpha_ref         = 1;
        break;

    case TD5_PRESET_SHADOW:
        /* Shadow quads. DEPTH TEST DISABLED [FIX: ground shimmer].
         *
         * The shadow is a FLAT quad through the 4 wheel-contact probes (scaled
         * 1.25x outward) projected through a SEPARATE transform from the road, so
         * coplanar shadow/road depths never tie deterministically — the LEQUAL
         * test loses on jittery sub-LSB / bumpy-road pixels and the shadow drops
         * out per-frame ("shimmers against the ground"). No constant or slope
         * depth bias cured it (r7 diagnostic: only z_func=ALWAYS rendered the
         * shadow cleanly on the road).
         *
         * The historical blocker to disabling the test was "shadow drew over the
         * car." That is GONE since the 2026-06-01 shadow PRE-PASS: every car body
         * is drawn AFTER all shadows with z_write=1, so the opaque body
         * unconditionally overwrites any shadow pixel it covers. So we drop the
         * depth test entirely (z_enable=0) — the shadow renders cleanly on the
         * ground (no tie, no shimmer) and the body-overwrite handles over-car.
         * (Trade-off: a wall/scenery prop BETWEEN the camera and the ground
         * shadow no longer occludes it; rare in the chase view and far less
         * objectionable than the constant shimmer.) z_write stays OFF. */
        s->blend_enable = 1;
        s->src_blend    = D3D6BLEND_SRCALPHA;
        s->dest_blend   = D3D6BLEND_INVSRCALPHA;
        s->z_enable     = 0;  /* no depth test — pre-pass + body-overwrite handles occlusion */
        s->z_write      = 0;
        s->z_func       = 0;
        s->polygon_offset    = 0;  /* depth bias moot with the test off */
        s->mag_filter   = 0;
        s->min_filter   = 0;
        s->texblend_mode = D3DTBLEND_MODULATEALPHA;
        s->alpha_test_enable = 1;
        s->alpha_ref         = 1;
        break;

    case TD5_PRESET_ADDITIVE:
        /* BindRaceTexturePage @ 0x0040B660 case 3:
         *   ALPHABLENDENABLE (0x1B) = 1
         *   SRCBLEND         (0x13) = D3DBLEND_ONE (2)
         *   DESTBLEND        (0x14) = D3DBLEND_ONE (2)
         * Additive draws happen in the deferred pass AFTER all opaque
         * geometry has finished writing depth, so z_write can stay off
         * here — we still z_test so lights hidden behind opaque walls
         * stay hidden. Alpha test on with ref=1 to discard palette-index-0
         * background pixels (zero-RGB, zero-alpha from the type-3 loader). */
        s->blend_enable      = 1;
        s->src_blend         = D3D6BLEND_ONE;
        s->dest_blend        = D3D6BLEND_ONE;
        s->z_enable          = 1;
        s->z_write           = 0;
        s->z_func            = 0;
        s->mag_filter        = 2; /* LINEAR */
        s->min_filter        = 2;
        s->texblend_mode     = D3DTBLEND_MODULATE;
        s->alpha_test_enable = 1;
        s->alpha_ref         = 1;
        break;

    case TD5_PRESET_ADDITIVE_GLOW:
        /* Cop-light strobe marker (RenderTrackedActorMarker @ 0x0043cde0).
         * Additive ONE/ONE like TD5_PRESET_ADDITIVE and z_test ON (lights are
         * occluded by walls), but with ALPHA TEST OFF — the original marker
         * submit path sets no alpha test. With LINEAR filtering, the near-binary
         * police-light texels then blend into a SOFT, DIFFUSED glow instead of
         * being hard-clipped at the alpha=1 contour into clear rectangles.
         * (Additive means the zero-RGB background still contributes nothing, so
         * dropping the alpha test does NOT reintroduce a gray/colored box.) */
        s->blend_enable      = 1;
        s->src_blend         = D3D6BLEND_ONE;
        s->dest_blend        = D3D6BLEND_ONE;
        s->z_enable          = 1;
        s->z_write           = 0;
        s->z_func            = 0;
        s->mag_filter        = 2; /* LINEAR */
        s->min_filter        = 2;
        s->texblend_mode     = D3DTBLEND_MODULATE;
        s->alpha_test_enable = 0;  /* <-- key: no hard alpha clip → diffused edges */
        s->alpha_ref         = 0;
        break;

    case TD5_PRESET_ADDITIVE_OVERLAY:
        /* Same blend/filter as TD5_PRESET_ADDITIVE (orig 0x0040B660 case 3:
         * ONE/ONE additive), but with z_test OFF — a 2D screen-space additive
         * glow that draws on top of everything. Used by the victory-star pulse
         * HUD (td5_render_submit_additive_hud). World-space smoke used to share
         * this preset; it now uses TD5_PRESET_ADDITIVE_WORLD (z-tested) below,
         * because the original draws smoke depth-tested (see that case). The
         * additive blend is the critical part for color parity — the glow is
         * white because of ONE/ONE, NOT because of vertex color. */
        s->blend_enable      = 1;
        s->src_blend         = D3D6BLEND_ONE;
        s->dest_blend        = D3D6BLEND_ONE;
        s->z_enable          = 0;
        s->z_write           = 0;
        s->z_func            = 0;
        s->mag_filter        = 2;
        s->min_filter        = 2;
        s->texblend_mode     = D3DTBLEND_MODULATE;
        s->alpha_test_enable = 1;
        s->alpha_ref         = 1;
        break;

    case TD5_PRESET_ADDITIVE_WORLD:
        /* Depth-tested additive smoke. Same ONE/ONE additive blend as
         * TD5_PRESET_ADDITIVE_OVERLAY, but z_test ON (LEQUAL) so world-space
         * particle smoke is OCCLUDED by walls/cars — matching the original.
         *
         * RE (Ghidra, 2026-06-01): the orig renders queued translucent
         * primitives (incl. wheel smoke) in FlushQueuedTranslucentPrimitives
         * @0x00431340, which is called by RunRaceFrame @0x0042b580 while the
         * TD5_RACE_PASS_OPAQUE preset is still active — i.e. ZFUNC=LESSEQUAL
         * with the z-buffer enabled. SetRaceRenderStatePreset @0x0040b070 only
         * ever sets ZFUNC(0x17)+ZWRITEENABLE(0xe), never ZENABLE(7); ZENABLE
         * stays TRUE for the whole scene (proven by the SKY pass avoiding
         * occlusion via ZFUNC=ALWAYS rather than disabling ZENABLE).
         * BindRaceTexturePage @0x0040b660 only sets blend+clamp, never z-state.
         *
         * z_write=0 (vs the orig flush's inherited ZWRITE=1): deliberate, and
         * consistent with TD5_PRESET_ADDITIVE — additive particles shouldn't
         * write depth (avoids near puffs suppressing farther overlapping puffs'
         * additive contribution / self-z-fighting). polygon_offset stays 0:
         * smoke is volumetric, not a coplanar ground decal like SHADOW, and the
         * depth already ties exactly thanks to the -64 NEAR_DEPTH_OFFSET folded
         * in at the submit site, so the car body correctly occludes smoke just
         * behind it. */
        s->blend_enable      = 1;
        s->src_blend         = D3D6BLEND_ONE;
        s->dest_blend        = D3D6BLEND_ONE;
        s->z_enable          = 1;
        s->z_write           = 0;
        s->z_func            = 0;  /* LEQUAL */
        s->mag_filter        = 2;
        s->min_filter        = 2;
        s->texblend_mode     = D3DTBLEND_MODULATE;
        s->alpha_test_enable = 1;
        s->alpha_ref         = 1;
        break;

    case TD5_PRESET_TRANSLUCENT_POINT_ZTEST:
        /* Identical to TD5_PRESET_TRANSLUCENT_POINT (SRCALPHA/INVSRCALPHA,
         * point filter, MODULATEALPHA, alpha_ref=1) but with the depth test
         * ENABLED (LEQUAL), z_write OFF. Used by vehicle brake-light billboards
         * so a nearer car body occludes a farther car's brake light, matching
         * the original's deferred z-tested tail-light flush (RenderVehicle-
         * TaillightQuads @0x004011c0 queues into FlushQueuedTranslucent-
         * Primitives @0x00431340, which runs with ZENABLE on / ZFUNC=LESSEQUAL;
         * SetRaceRenderStatePreset @0x0040b070 never disables ZENABLE).
         * z_write OFF because the port draws brakes inline during the actor
         * loop — writing depth would let an earlier brake occlude a later car
         * body. No polygon_offset — a brake light is not a coplanar ground decal
         * (unlike TD5_PRESET_SHADOW). The (z_enable=1, z_write=0, z_func=LEQUAL)
         * combination is already exercised by TD5_PRESET_SHADOW / ADDITIVE_WORLD,
         * so the d3d11 backend DSV mapping needs no change. */
        s->blend_enable      = 1;
        s->src_blend         = D3D6BLEND_SRCALPHA;
        s->dest_blend        = D3D6BLEND_INVSRCALPHA;
        s->z_enable          = 1;
        s->z_write           = 0;
        s->z_func            = 0;  /* LEQUAL */
        s->mag_filter        = 0;  /* POINT — no bilinear bleed into transparent border pixels */
        s->min_filter        = 0;
        s->texblend_mode     = D3DTBLEND_MODULATEALPHA;
        s->alpha_test_enable = 1;
        s->alpha_ref         = 1;
        break;

    case TD5_PRESET_SKY:
        /* Sky dome — faithful match to original SetRaceRenderStatePreset(0)
         * @ 0x0040b070 case 0 (final fallthrough block 0x40b0d8-0x40b0e9):
         *   D3DRS_ZFUNC        (0x17) = 8 (D3DCMP_ALWAYS)  [CONFIRMED @ 0x0040b0d8]
         *   D3DRS_ZWRITEENABLE (0x0E) = 0                  [CONFIRMED @ 0x0040b0e1]
         *
         * The original's sky vertices use the same depth formula as track
         * (screen_z = (view_z - 64.0) * 1.5259e-5), so the dome's projected
         * depth values are tiny for camera-centered geometry. The reason
         * this doesn't occlude track is purely because z_func=ALWAYS makes
         * the depth comparison vacuous AND z_write=0 leaves the cleared
         * far value in the buffer. Track follows with ZFUNC=LESSEQUAL +
         * ZWRITE=1 (every other preset above resets z_func=0).
         *
         * In the wrapper (d3d11_backend.c:1102), z_func=1 + z_write=0 maps
         * to a distinct DSV (DS_Z_ON_WRITE_OFF_ALWAYS) so the cache cannot
         * hide a missed apply. */
        s->blend_enable      = 0;
        s->z_enable          = 1;
        s->z_write           = 0;
        s->z_func            = 1; /* ALWAYS — sky never fails depth test */
        s->mag_filter        = 2; /* LINEAR */
        s->min_filter        = 2;
        s->texblend_mode     = D3DTBLEND_MODULATE;
        s->alpha_test_enable = 1;
        s->alpha_ref         = 1;
        break;
    }

    g_backend.alpha_blend_enabled = s->blend_enable;
    s->dirty = 1;
}

void td5_plat_render_bind_texture(int page_index)
{
    ID3D11ShaderResourceView *srv = NULL;

    if (page_index >= 0 && page_index < MAX_TEXTURE_PAGES) {
        /* Use the texture handle (SRV pointer) if available */
        if (s_tex_handles[page_index]) {
            srv = (ID3D11ShaderResourceView *)(DWORD_PTR)s_tex_handles[page_index];
        } else if (s_tex_surfaces[page_index] && s_tex_surfaces[page_index]->d3d11_srv) {
            srv = s_tex_surfaces[page_index]->d3d11_srv;
        }
    }

    g_backend.current_srv = srv;
    s_last_bound_texture_page = page_index;
    if (g_backend.context && srv) {
        ID3D11DeviceContext_PSSetShaderResources(g_backend.context, 0, 1, &srv);
    } else if (g_backend.context) {
        ID3D11ShaderResourceView *null_srv = NULL;
        ID3D11DeviceContext_PSSetShaderResources(g_backend.context, 0, 1, &null_srv);
    }

    g_backend.state.dirty = 1;
}

void td5_plat_render_set_fog(int enable, uint32_t color,
                              float start, float end, float density)
{
    RenderStateCache *s = &g_backend.state;

    s->fog_enable  = enable ? 1 : 0;
    s->fog_color   = (DWORD)color;
    s->fog_start   = start;
    s->fog_end     = end;
    s->fog_density = density;
    s->fog_mode    = 1; /* linear */
    s->dirty       = 1;

    Backend_UpdateFogCB();
}

int td5_plat_render_upload_texture(int page_index, const void *pixels,
                                    int width, int height, int format)
{
    WrapperSurface *surf;
    WrapperTexture *tex;
    DWORD caps, bpp;

    if (page_index < 0 || page_index >= MAX_TEXTURE_PAGES) return 0;
    if (!pixels || width <= 0 || height <= 0) return 0;
    if (!g_backend.device) return 0;

    /* Determine bpp from format: 0 = R5G6B5 (16-bit), 1 = A1R5G5B5 (16-bit),
     * 2 = A8R8G8B8 (32-bit) */
    bpp = (format == 2) ? 32 : 16;
    caps = DDSCAPS_TEXTURE | DDSCAPS_VIDEOMEMORY;

    /* Release old surface if any */
    if (s_tex_surfaces[page_index]) {
        if (s_tex_wrappers[page_index]) {
            s_tex_wrappers[page_index]->vtbl->Release(s_tex_wrappers[page_index]);
            s_tex_wrappers[page_index] = NULL;
        }
        s_tex_surfaces[page_index]->vtbl->Release(s_tex_surfaces[page_index]);
        s_tex_surfaces[page_index] = NULL;
        s_tex_handles[page_index] = 0;
    }

    /* Create new texture surface */
    surf = WrapperSurface_Create((DWORD)width, (DWORD)height, bpp, caps);
    if (!surf) return 0;

    /* Upload pixel data via sys_buffer -> UpdateSubresource */
    WrapperSurface_EnsureSysBuffer(surf);
    if (surf->sys_buffer) {
        size_t row_bytes = (size_t)width * (bpp / 8);
        size_t total = row_bytes * (size_t)height;
        if (format == 2) {
            const uint8_t *src32 = (const uint8_t *)pixels;
            uint8_t *dst32 = (uint8_t *)surf->sys_buffer;
            size_t pixel_count = (size_t)width * (size_t)height;
            int apply_font_colorkey = 0;

            /* BodyText.tga is decoded from RGB data into opaque BGRA before it
             * reaches this upload path. Restore the original black colorkey
             * behavior here so the font background stays transparent. */
            if (page_index == TD5_SHARED_FONT_PAGE) {
                apply_font_colorkey = 1;
                for (size_t i = 0; i < pixel_count; i++) {
                    if (src32[i * 4 + 3] != 0xFF) {
                        apply_font_colorkey = 0;
                        break;
                    }
                }
            }

            if (!apply_font_colorkey) {
                /* The decoder already produced BGRA, so this is an identity copy
                 * for every non-font page — a memcpy instead of a per-pixel loop
                 * (the loop was costing ~1.8M iterations per 1632x1120 carpic). */
                memcpy(dst32, src32, total);
            } else {
                for (size_t i = 0; i < pixel_count; i++) {
                    uint8_t b = src32[i * 4 + 0];
                    uint8_t g = src32[i * 4 + 1];
                    uint8_t r = src32[i * 4 + 2];
                    unsigned a = (((unsigned)r + g + b) < 16u) ? 0u : 255u;
                    dst32[i * 4 + 0] = b;
                    dst32[i * 4 + 1] = g;
                    dst32[i * 4 + 2] = r;
                    dst32[i * 4 + 3] = (uint8_t)a;
                }
            }
        } else {
            memcpy(surf->sys_buffer, pixels, total);
        }
        surf->dirty = 1;
        WrapperSurface_FlushDirty(surf);
    }

    /* Create texture wrapper and get handle */
    tex = WrapperTexture_Create(surf);
    if (!tex) {
        surf->vtbl->Release(surf);
        return 0;
    }

    {
        DWORD handle = 0;
        tex->vtbl->GetHandle(tex, s_d3ddevice3, &handle);
        s_tex_handles[page_index] = handle;
    }

    s_tex_surfaces[page_index] = surf;
    s_tex_wrappers[page_index] = tex;
    s_tex_widths[page_index]   = (uint16_t)width;
    s_tex_heights[page_index]  = (uint16_t)height;
    TD5_LOG_I(LOG_TAG, "Texture upload: page=%d size=%dx%d format=%d",
              page_index, width, height, format);

    return 1;
}

void td5_plat_render_get_texture_dims(int page_index, int *w, int *h)
{
    if (page_index >= 0 && page_index < MAX_TEXTURE_PAGES &&
        s_tex_widths[page_index] > 0 && s_tex_heights[page_index] > 0) {
        *w = s_tex_widths[page_index];
        *h = s_tex_heights[page_index];
    }
}

void td5_plat_render_set_viewport(int x, int y, int width, int height)
{
    D3D11_VIEWPORT vp;

    if (!g_backend.context) return;

    vp.TopLeftX = (FLOAT)x;
    vp.TopLeftY = (FLOAT)y;
    vp.Width    = (FLOAT)width;
    vp.Height   = (FLOAT)height;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;

    ID3D11DeviceContext_RSSetViewports(g_backend.context, 1, &vp);
    Backend_UpdateViewportCB((float)width, (float)height);
    TD5_LOG_I(LOG_TAG, "Viewport set: x=%d y=%d w=%d h=%d", x, y, width, height);
}

void td5_plat_render_set_clip_rect(int left, int top, int right, int bottom)
{
    D3D11_RECT rc;

    if (!g_backend.context) return;

    /* Clamp to render target dimensions so we never set an invalid rect. */
    int rt_w = (g_backend.backbuffer && g_backend.backbuffer->width)
             ? g_backend.backbuffer->width
             : (int)g_backend.width;
    int rt_h = (g_backend.backbuffer && g_backend.backbuffer->height)
             ? g_backend.backbuffer->height
             : (int)g_backend.height;

    if (left < 0)            left   = 0;
    if (top  < 0)            top    = 0;
    if (right  > rt_w)       right  = rt_w;
    if (bottom > rt_h)       bottom = rt_h;
    if (right  < left)       right  = left;
    if (bottom < top)        bottom = top;

    rc.left   = left;
    rc.top    = top;
    rc.right  = right;
    rc.bottom = bottom;
    ID3D11DeviceContext_RSSetScissorRects(g_backend.context, 1, &rc);
}

void td5_plat_render_clear(uint32_t color)
{
    float rgba[4];

    if (!g_backend.context) return;

    /* Extract ARGB components to float RGBA */
    rgba[0] = (float)((color >> 16) & 0xFF) / 255.0f; /* R */
    rgba[1] = (float)((color >>  8) & 0xFF) / 255.0f; /* G */
    rgba[2] = (float)((color >>  0) & 0xFF) / 255.0f; /* B */
    rgba[3] = 1.0f;                                     /* A */

    /* Clear backbuffer RTV */
    if (g_backend.backbuffer && g_backend.backbuffer->d3d11_rtv) {
        ID3D11DeviceContext_ClearRenderTargetView(g_backend.context,
            g_backend.backbuffer->d3d11_rtv, rgba);
    } else if (g_backend.swap_rtv) {
        ID3D11DeviceContext_ClearRenderTargetView(g_backend.context,
            g_backend.swap_rtv, rgba);
    }

    /* Clear depth buffer */
    if (g_backend.depth_dsv) {
        ID3D11DeviceContext_ClearDepthStencilView(g_backend.context,
            g_backend.depth_dsv, D3D11_CLEAR_DEPTH, 1.0f, 0);
    }
    TD5_LOG_D(LOG_TAG, "Render clear: color=0x%08X", (unsigned int)color);
}

/* ========================================================================
 * Threading
 * ======================================================================== */

typedef struct TD5_ThreadCtx {
    void (*func)(void *);
    void *arg;
} TD5_ThreadCtx;

static DWORD WINAPI thread_trampoline(LPVOID param)
{
    TD5_ThreadCtx *tc = (TD5_ThreadCtx *)param;
    tc->func(tc->arg);
    free(tc);
    return 0;
}

void *td5_plat_thread_create(void (*func)(void *), void *arg)
{
    TD5_ThreadCtx *tc;
    HANDLE h;

    if (!func) return NULL;
    tc = (TD5_ThreadCtx *)malloc(sizeof(TD5_ThreadCtx));
    if (!tc) return NULL;
    tc->func = func;
    tc->arg  = arg;

    h = CreateThread(NULL, 0, thread_trampoline, tc, 0, NULL);
    if (!h) { free(tc); return NULL; }
    return (void *)h;
}

int td5_plat_thread_join(void *thread_handle)
{
    HANDLE h = (HANDLE)thread_handle;
    if (!h) return -1;
    WaitForSingleObject(h, INFINITE);
    CloseHandle(h);
    return 0;
}

void *td5_plat_event_create(void)
{
    HANDLE h = CreateEventA(NULL, FALSE, FALSE, NULL);
    return (void *)h;
}

void td5_plat_event_set(void *event_handle)
{
    if (event_handle)
        SetEvent((HANDLE)event_handle);
}

int td5_plat_event_wait(void *event_handle, uint32_t timeout_ms)
{
    DWORD result;
    if (!event_handle) return -1;
    result = WaitForSingleObject((HANDLE)event_handle,
        timeout_ms == 0 ? INFINITE : (DWORD)timeout_ms);
    return (result == WAIT_OBJECT_0) ? 0 : -1;
}

void td5_plat_event_destroy(void *event_handle)
{
    if (event_handle)
        CloseHandle((HANDLE)event_handle);
}

void *td5_plat_mutex_create(void)
{
    CRITICAL_SECTION *cs = (CRITICAL_SECTION *)malloc(sizeof(CRITICAL_SECTION));
    if (!cs) return NULL;
    InitializeCriticalSection(cs);
    return (void *)cs;
}

void td5_plat_mutex_lock(void *mutex_handle)
{
    if (mutex_handle)
        EnterCriticalSection((CRITICAL_SECTION *)mutex_handle);
}

void td5_plat_mutex_unlock(void *mutex_handle)
{
    if (mutex_handle)
        LeaveCriticalSection((CRITICAL_SECTION *)mutex_handle);
}

void td5_plat_mutex_destroy(void *mutex_handle)
{
    if (mutex_handle) {
        DeleteCriticalSection((CRITICAL_SECTION *)mutex_handle);
        free(mutex_handle);
    }
}

/* ========================================================================
 * Logging — multi-file with session rotation
 *
 * Messages are routed to frontend.log / race.log / engine.log based on
 * the module tag.  On startup, existing logs are rotated up to 5 previous
 * sessions (e.g. frontend.log → frontend.1.log → ... → frontend.5.log).
 * ======================================================================== */

static const char *s_log_level_names[] = { "DBG", "INF", "WRN", "ERR" };

static int td5_log_classify_module(const char *module)
{
    if (!module) return TD5_LOG_CAT_ENGINE;

    /* Frontend category */
    if (strcmp(module, "frontend") == 0 ||
        strcmp(module, "hud")      == 0 ||
        strcmp(module, "save")     == 0 ||
        strcmp(module, "input")    == 0)
        return TD5_LOG_CAT_FRONTEND;

    /* Race category */
    if (strcmp(module, "td5_game") == 0 ||
        strcmp(module, "physics")  == 0 ||
        strcmp(module, "ai")       == 0 ||
        strcmp(module, "track")    == 0 ||
        strcmp(module, "camera")   == 0 ||
        strcmp(module, "vfx")      == 0)
        return TD5_LOG_CAT_RACE;

    /* Everything else: render, asset, platform, sound, net, fmv, main, ... */
    return TD5_LOG_CAT_ENGINE;
}

static void td5_log_flush_file(TD5_LogFile *lf)
{
    DWORD written;
    if (lf->handle == INVALID_HANDLE_VALUE || lf->buffer_used == 0) return;
    WriteFile(lf->handle, lf->buffer, (DWORD)lf->buffer_used, &written, NULL);
    lf->buffer_used = 0;
}

static void td5_plat_log_shutdown(void)
{
    int i;
    for (i = 0; i < TD5_LOG_CAT_COUNT; i++) {
        if (s_log_files[i].handle != INVALID_HANDLE_VALUE) {
            td5_log_flush_file(&s_log_files[i]);
            FlushFileBuffers(s_log_files[i].handle);
            CloseHandle(s_log_files[i].handle);
            s_log_files[i].handle = INVALID_HANDLE_VALUE;
        }
    }
}

static void td5_log_rotate_file(const char *base_path)
{
    char old_path[600], new_path[600];
    int i;

    /* Delete the oldest (generation MAX_SESSIONS) */
    snprintf(old_path, sizeof(old_path), "%s.%d.log", base_path, TD5_LOG_MAX_SESSIONS);
    DeleteFileA(old_path);

    /* Shift generations up: N-1 → N, N-2 → N-1, ... 1 → 2 */
    for (i = TD5_LOG_MAX_SESSIONS - 1; i >= 1; i--) {
        snprintf(old_path, sizeof(old_path), "%s.%d.log", base_path, i);
        snprintf(new_path, sizeof(new_path), "%s.%d.log", base_path, i + 1);
        MoveFileA(old_path, new_path);
    }

    /* Current .log → .1.log */
    snprintf(old_path, sizeof(old_path), "%s.log", base_path);
    snprintf(new_path, sizeof(new_path), "%s.1.log", base_path);
    MoveFileA(old_path, new_path);
}

void td5_plat_log_init(void)
{
    int i;
    if (s_log_initialized) return;

    /* Determine log directory: <exe_dir>/log/ */
    {
        DWORD n = GetModuleFileNameA(NULL, s_log_dir, sizeof(s_log_dir) - 32);
        if (n > 0) {
            char *slash = s_log_dir + n;
            while (slash > s_log_dir && *slash != '\\' && *slash != '/') slash--;
            if (slash > s_log_dir) slash[1] = '\0';
        } else {
            strcpy(s_log_dir, ".\\");
        }
        strcat(s_log_dir, "log\\");
    }

    /* Create directory (ignore error if already exists) */
    CreateDirectoryA(s_log_dir, NULL);

    /* Rotate each category's log files */
    for (i = 0; i < TD5_LOG_CAT_COUNT; i++) {
        char base[600];
        char path[600];
        DWORD written;
        const char *marker = "[session start]\n";

        snprintf(base, sizeof(base), "%s%s", s_log_dir, s_log_cat_names[i]);
        td5_log_rotate_file(base);

        /* Open fresh log file */
        snprintf(path, sizeof(path), "%s.log", base);
        s_log_files[i].handle = CreateFileA(path, FILE_APPEND_DATA,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        s_log_files[i].buffer_used = 0;
        s_log_files[i].line_count = 0;

        if (s_log_files[i].handle != INVALID_HANDLE_VALUE) {
            WriteFile(s_log_files[i].handle, marker, (DWORD)strlen(marker), &written, NULL);
        }
    }

    /* Also rotate crash.log */
    {
        char base[600];
        snprintf(base, sizeof(base), "%scrash", s_log_dir);
        td5_log_rotate_file(base);
    }

    atexit(td5_plat_log_shutdown);
    s_log_initialized = 1;
}

const char *td5_plat_log_dir(void)
{
    return s_log_dir;
}

void td5_plat_log_set_filters(int enabled, int min_level, unsigned int cat_mask)
{
    s_log_enabled   = enabled ? 1 : 0;
    s_log_min_level = min_level;
    s_log_cat_mask  = cat_mask;
}

void td5_plat_log(TD5_LogLevel level, const char *module, const char *fmt, ...)
{
    char buf[2048];
    va_list ap;
    int off, cat;
    DWORD written;
    TD5_LogFile *lf;

    if (!fmt) return;

    /* Fast-path gating — bail before any formatting cost.
     *  - Errors are NEVER suppressed (still go to stdout below); they're rare
     *    and we don't want a perf-A/B run to silently swallow crashes.
     *  - Master switch off → drop everything except errors.
     *  - Below configured min_level → drop. */
    if (level < TD5_LOG_ERROR) {
        if (!s_log_enabled) return;
        if ((int)level < s_log_min_level) return;
        cat = td5_log_classify_module(module);
        if (!(s_log_cat_mask & (1u << cat))) return;
    }

    off = snprintf(buf, sizeof(buf), "[%s][%s] ",
        (level >= 0 && level <= 3) ? s_log_level_names[level] : "???",
        module ? module : "td5");

    va_start(ap, fmt);
    vsnprintf(buf + off, sizeof(buf) - off, fmt, ap);
    va_end(ap);

    /* Always newline-terminate */
    {
        size_t len = strlen(buf);
        if (len > 0 && len < sizeof(buf) - 2 && buf[len-1] != '\n') {
            buf[len] = '\n';
            buf[len+1] = '\0';
        }
    }

    /* Lazy init if td5_plat_log_init() was not called yet */
    if (!s_log_initialized) td5_plat_log_init();

    cat = td5_log_classify_module(module);
    lf = &s_log_files[cat];

    if (lf->handle != INVALID_HANDLE_VALUE) {
        size_t len = strlen(buf);
        if (len > sizeof(lf->buffer)) {
            td5_log_flush_file(lf);
            WriteFile(lf->handle, buf, (DWORD)len, &written, NULL);
        } else {
            if (lf->buffer_used + len > sizeof(lf->buffer)) {
                td5_log_flush_file(lf);
            }
            memcpy(lf->buffer + lf->buffer_used, buf, len);
            lf->buffer_used += len;
        }
        lf->line_count++;
        /* Flush to disk periodically — only on ERROR or every 256 messages.
         * Flushing on WARN caused thousands of synchronous WriteFile calls per
         * frame during rendering (invalid mesh opcodes, wall impulse logs),
         * which was the root cause of the 5-10 second frame freeze in race mode. */
        if (level >= TD5_LOG_ERROR || (lf->line_count % 256) == 0) {
            td5_log_flush_file(lf);
        }
    }

    /* Echo WARN and ERROR to the console (visible even when running from IDE) */
    if (level >= TD5_LOG_WARN) {
        HANDLE hcon = GetStdHandle(STD_OUTPUT_HANDLE);
        if (hcon && hcon != INVALID_HANDLE_VALUE) {
            DWORD written2;
            WriteFile(hcon, buf, (DWORD)strlen(buf), &written2, NULL);
        }
    }
}

void td5_plat_log_flush(void)
{
    int i;
    for (i = 0; i < TD5_LOG_CAT_COUNT; i++) {
        if (s_log_files[i].handle != INVALID_HANDLE_VALUE) {
            td5_log_flush_file(&s_log_files[i]);
            FlushFileBuffers(s_log_files[i].handle);
        }
    }
}

/* td5_plat_fatal is not declared in the header but is part of the platform
 * contract for unrecoverable errors. */
void td5_plat_fatal(const char *msg)
{
    if (msg) {
        OutputDebugStringA(msg);
        MessageBoxA(s_hwnd, msg, "TD5RE Fatal Error",
                    MB_OK | MB_ICONERROR);
    }
    ExitProcess(1);
}
