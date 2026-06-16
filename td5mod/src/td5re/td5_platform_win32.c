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
#include "td5_rcmd.h"   /* Phase B render-transform: per-pane CPU command recording */

/* Pull in the wrapper types and backend access */
#include "../../ddraw_wrapper/src/wrapper.h"

/* Procedural, texture-free particle/VFX pixel shaders (smoke/rain/decal/glow).
 * Compiled bytecode embedded as g_ps_fx_* arrays; created lazily in
 * td5_plat_fx_begin and exposed to the VFX/render code through the FX API. */
#include "../../ddraw_wrapper/src/shaders/ps_fx_smoke_bytes.h"  /* g_ps_fx_smoke */
#include "../../ddraw_wrapper/src/shaders/ps_fx_rain_bytes.h"   /* g_ps_fx_rain  */
#include "../../ddraw_wrapper/src/shaders/ps_fx_decal_bytes.h"  /* g_ps_fx_decal */
#include "../../ddraw_wrapper/src/shaders/ps_fx_glow_bytes.h"   /* g_ps_fx_glow  */

#define LOG_TAG "platform"
#include "td5_color.h"

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
/* Instance GUID of the device bound to each slot (mirrors s_device_guids[idx]
 * of whatever td5_plat_input_set_device opened). Lets the FF layer map a slot
 * back to its enumerated device for XInput detection. [#1 2026-06-15] */
static GUID s_di_joystick_guid[TD5_PLAT_MAX_JS_SLOTS];
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

/* [PORT ENHANCEMENT 2026-06-07] Built-in DEFAULT per-action gamepad mapping.
 * Applied to any joystick that has no explicit Control-Options per-action config
 * (s_js_action_set==0) and no legacy Config.td5 9-slot remap, so a standard
 * controller works out of the box without visiting the binding screen. Targets
 * the Xbox-controller-via-DirectInput layout (an XInput pad surfaced through
 * dinput8): left stick X = steer; the two triggers SHARE the Z axis (RT pulls Z
 * below centre, LT pushes it above — a DirectInput limitation, so flooring both
 * at once cancels); face/shoulder/stick buttons follow the canonical XInput
 * order 0=A 1=B 2=X 3=Y 4=LB 5=RB 6=Back/View 7=Start/Menu 8=LS 9=RS. Entry
 * order matches k_ctrl_action_labels / the k_action_bits decode below. If a
 * given pad reports the triggers reversed or on a different axis, the
 * Control-Options remap overrides this. */
static const uint32_t k_default_js_action_bind[TD5_JSBIND_ACTIONS] = {
    TD5_JSBIND_AXIS   | (0u << 1) | 1u,  /*  0 LEFT        left stick X (-)          */
    TD5_JSBIND_AXIS   | (0u << 1) | 0u,  /*  1 RIGHT       left stick X (+)          */
    TD5_JSBIND_AXIS   | (2u << 1) | 1u,  /*  2 ACCELERATE  R2 / RT  (Z below centre) */
    TD5_JSBIND_AXIS   | (2u << 1) | 0u,  /*  3 BRAKE       L2 / LT  (Z above centre) */
    TD5_JSBIND_BUTTON | 2u,              /*  4 HANDBRAKE   X                         */
    TD5_JSBIND_BUTTON | 3u,              /*  5 HORN/SIREN  Y                         */
    TD5_JSBIND_BUTTON | 5u,              /*  6 GEAR UP     R1 / RB                    */
    TD5_JSBIND_BUTTON | 4u,              /*  7 GEAR DOWN   L1 / LB                    */
    TD5_JSBIND_BUTTON | 6u,              /*  8 CHANGE VIEW Back / View (Select)       */
    TD5_JSBIND_BUTTON | 9u,              /*  9 REAR VIEW   right stick click (R3)      */
    TD5_JSBIND_BUTTON | 7u,              /* 10 PAUSE       Start / Menu               */
};

/* [S27 2026-06-05] Per-device persistent-loss detection (controller hot-unplug).
 * SOURCE-PORT FEATURE — the 1999 binary had no hot-plug handling: M2DX/DXInput
 * owned the devices and the game never queried device presence, so there is no
 * original behaviour to RE here. The original poll path only re-Acquired on a
 * transient DIERR_INPUTLOST/NOTACQUIRED and never reported a persistent loss.
 *
 * We track a consecutive GetDeviceState-failure streak per device slot and
 * latch s_js_lost once it persists past a threshold (so a one-frame INPUTLOST
 * from an alt-tab focus change does NOT count as a disconnect). While a slot is
 * lost we throttle the re-Acquire retry to avoid hammering the DirectInput API
 * every frame for an unplugged pad. A later successful read clears the latch
 * (device reconnected). */
#define TD5_JS_LOST_FAIL_THRESHOLD   6   /* consecutive failed polls => "lost"   */
#define TD5_JS_REACQ_HOLDOFF_FRAMES 12   /* frames between re-Acquire tries while lost */
static int s_js_fail_streak[TD5_PLAT_MAX_JS_SLOTS]  = { 0 };
static int s_js_lost[TD5_PLAT_MAX_JS_SLOTS]         = { 0 };
static int s_js_reacq_holdoff[TD5_PLAT_MAX_JS_SLOTS] = { 0 };

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
static GUID s_device_product_guids[16]; /* DirectInput PRODUCT GUID (same model -> same product GUID) */
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

/* Per-voice volume table, reproduced byte-faithfully from M2DX
 * DXSound::Environment (0x1000cda0):
 *     table[v] = -(int)( (500/log10(2)) * log10(128/(v+1)) ),  v in 0..127
 * It maps the mixer's 0..127 volume to DirectSound centibels (hundredths of dB):
 * 0 dB at v=127, sloping down a log curve to a -35 dB FLOOR at v=0 — NOT silence.
 * DXSound::Modify sets each voice to table[vol] (no per-sound master scaling), and
 * the master SFX volume is a SEPARATE attenuation (DXSound::SetVolume applies
 * table[master>>9] to the primary buffer). The port previously used a different
 * 20*log10(vol/100) curve AND folded the master in per-sound (vol*master/100),
 * which (a) muted v=0 entirely so the original's faint engine/ambient layers
 * vanished (the player's own Drive loop runs at v=0 under the audible Reverb — a
 * real second engine layer in the original, dead silent in the port) and (b)
 * shifted the whole balance. We now match the table per-voice and apply the
 * master as an equivalent dB offset (adding dB == scaling the summed mix, which
 * is what the original's primary-buffer attenuation does). */
static int                     s_vol_table[128];
static int                     s_master_offset_cb = 0;   /* master attenuation, centibels (<=0) */
static void audio_build_vol_table(void);                 /* defined below, used in audio init */

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
/* When set, all audio is silenced because the game window is NOT the foreground
 * window (alt-tabbed away). Independent of s_audio_muted (the pause-menu mute):
 * they OR together in audio_effective_cb so neither clobbers the other. Driven
 * once per frame by td5_plat_audio_update_focus_mute(). */
static int                     s_audio_focus_muted = 0;

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
 * Application icon
 * ------------------------------------------------------------------------
 * The TD5 icon ships as a multi-size resource (16/32/48/256) embedded by
 * windres from td5re.rc -> td5re.ico under the lowest resource id (1), so
 * Explorer/taskbar pick it as the default application icon.
 *
 * The actual visible window is created by the D3D11 wrapper
 * (TD5_D3D11_Display class in d3d11_backend.c) whose WNDCLASS leaves
 * hIcon/hIconSm NULL -> Windows would otherwise show the generic default
 * icon on the taskbar / Alt-Tab / title bar. We override it at runtime via
 * WM_SETICON, and also set the WNDCLASS icons on the fallback window we
 * create ourselves.
 *
 * Use LoadImage (not LoadIcon): LoadIcon always returns the *large*
 * (SM_CXICON, usually 32px) frame, so a single LoadIcon handle reused for
 * ICON_SMALL forces Windows to downscale 32->16 (blurry). LoadImage with
 * the small/large system metrics selects the matching 16px / 32px frame
 * from the multi-size .ico.
 * ======================================================================== */
#define TD5RE_APP_ICON_ID 1

/* Load the embedded app icon at a specific pixel size; falls back to the
 * system default application icon if the resource is missing. */
static HICON td5_load_app_icon(int cx, int cy)
{
    HINSTANCE hinst = GetModuleHandleA(NULL);
    HICON h = (HICON)LoadImageA(hinst, MAKEINTRESOURCE(TD5RE_APP_ICON_ID),
                                IMAGE_ICON, cx, cy, LR_DEFAULTCOLOR);
    if (!h)
        h = (HICON)LoadImageA(NULL, IDI_APPLICATION, IMAGE_ICON, cx, cy,
                              LR_DEFAULTCOLOR | LR_SHARED);
    return h;
}

/* Apply the TD5 icon to a created window (big = Alt-Tab/title, small =
 * taskbar/title-bar). Safe to call repeatedly; handles are owned by the
 * caller for the window lifetime (process-lifetime windows -> never freed). */
static void td5_apply_window_icons(HWND hwnd)
{
    HICON hBig, hSmall;
    if (!hwnd)
        return;
    hBig   = td5_load_app_icon(GetSystemMetrics(SM_CXICON),
                               GetSystemMetrics(SM_CYICON));
    hSmall = td5_load_app_icon(GetSystemMetrics(SM_CXSMICON),
                               GetSystemMetrics(SM_CYSMICON));
    if (hBig)   SendMessageA(hwnd, WM_SETICON, ICON_BIG,   (LPARAM)hBig);
    if (hSmall) SendMessageA(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)hSmall);
    /* Also stamp the window-class icons so any default/derived paint uses
     * the TD5 icon. The wrapper's class registered them as NULL. */
    if (hSmall) SetClassLongPtrA(hwnd, GCLP_HICONSM, (LONG_PTR)hSmall);
    if (hBig)   SetClassLongPtrA(hwnd, GCLP_HICON,   (LONG_PTR)hBig);
}

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

        /* Set the TD5 app icon on the wrapper's display window (the visible
         * window) so the taskbar / Alt-Tab / title bar use it instead of the
         * generic default. Big = 32px frame, small = 16px frame. */
        td5_apply_window_icons(s_hwnd);

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

/* WM_CHAR ring buffer for robust text input. Windows queues every typed
 * character (with correct shift/caps/repeat) and we drain it once per frame, so
 * input is never dropped by frame-rate spikes or GetAsyncKeyState '&1' polling
 * contention (the old text path missed keys whenever the menu's other
 * GetAsyncKeyState calls consumed the "pressed since last call" bit first). */
#define TD5_CHAR_QUEUE_SIZE 128
static volatile unsigned char s_char_queue[TD5_CHAR_QUEUE_SIZE];
static volatile int s_char_q_head;   /* producer (window proc) */
static volatile int s_char_q_tail;   /* consumer (frontend)    */

/* WM_KEYDOWN navigation FIFO. Same robustness model as the WM_CHAR queue above,
 * but for menu cursor/Enter keys: Windows posts a WM_KEYDOWN for every genuine
 * press, so we queue each one (auto-repeat filtered via lParam bit 30) and the
 * frontend drains them in order. A burst of taps during a frame-time spike is
 * preserved press-for-press instead of being collapsed into a single bit (the
 * previous OR-latch did exactly that — three quick DOWNs became one move), and
 * unlike the once-per-frame DirectInput immediate read, a press that lands AND
 * releases inside one slow frame is still captured. The old GetAsyncKeyState '&1'
 * approach was rejected for a poll-contention bug (other GetAsyncKeyState callers
 * consumed the "pressed since last call" bit first — see td5_frontend.c);
 * WM_KEYDOWN has no such contention. */
#define TD5_NAV_QUEUE_SIZE 64
static volatile unsigned char s_nav_queue[TD5_NAV_QUEUE_SIZE];
static volatile int s_nav_q_head;   /* producer (window proc) */
static volatile int s_nav_q_tail;   /* consumer (frontend)    */
/* ESC stays a one-shot latch rather than a FIFO entry: a back-out is applied at
 * most once per frame so a buffered burst can't pop several screens at once. */
static volatile int s_esc_latch = 0;

/* [PERF FIX 2026-06-05] Set by WM_DEVICECHANGE; consumed by
 * td5_plat_input_devices_changed(). Lets the frontend run the ~120ms blocking
 * EnumDevices rescan ONLY when the OS signals a USB device change, instead of
 * polling it every ~90 frames (the dominant frontend frame-time spike). */
static volatile int s_devices_dirty = 0;

static LRESULT CALLBACK TD5_WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CLOSE:
        PostQuitMessage(0);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_CHAR: {
        /* Queue the translated character (handles shift/caps + key repeat). */
        int next = (s_char_q_head + 1) % TD5_CHAR_QUEUE_SIZE;
        if (next != s_char_q_tail) {
            s_char_queue[s_char_q_head] = (unsigned char)wParam;
            s_char_q_head = next;
        }
        return 0;
    }
    case WM_SYSKEYDOWN:
        /* Alt+F4 — explicit handler so it always closes even if original proc drops it */
        if (wParam == VK_F4) {
            PostQuitMessage(0);
            return 0;
        }
        break;
    case WM_DEVICECHANGE:
        /* USB controller arrival/removal. DBT_DEVNODES_CHANGED is broadcast to
         * every top-level window without RegisterDeviceNotification, so a flag is
         * all we need: the frontend rescans on the next poll instead of running
         * EnumDevices (a ~120ms blocking HID enumeration) on a frame timer. */
        s_devices_dirty = 1;
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
    /* Queue genuine nav-key presses (auto-repeat filtered via lParam bit 30) so
     * menu navigation is never dropped OR collapsed at high MS: each press is one
     * FIFO entry, drained in order by the frontend. ESC uses a separate one-shot
     * latch. Falls through to the default proc below so WM_CHAR is still produced
     * for text-entry screens. */
    case WM_KEYDOWN:
        if (!(lParam & (1L << 30))) {
            unsigned code = 0;
            switch (wParam) {
            case VK_LEFT:   code = TD5_NAVKEY_LEFT;  break;
            case VK_RIGHT:  code = TD5_NAVKEY_RIGHT; break;
            case VK_UP:     code = TD5_NAVKEY_UP;    break;
            case VK_DOWN:   code = TD5_NAVKEY_DOWN;  break;
            case VK_RETURN: code = TD5_NAVKEY_ENTER; break;
            case VK_ESCAPE: s_esc_latch = 1;         break;
            default: break;
            }
            if (code) {
                int next = (s_nav_q_head + 1) % TD5_NAV_QUEUE_SIZE;
                if (next != s_nav_q_tail) {   /* drop only if 64-deep queue is full */
                    s_nav_queue[s_nav_q_head] = (unsigned char)code;
                    s_nav_q_head = next;
                }
            }
        }
        break;
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

        /* Big icon for the class/Alt-Tab, small for the title bar/taskbar,
         * each picked from the matching frame of the multi-size .ico. */
        HICON hIconBig = td5_load_app_icon(GetSystemMetrics(SM_CXICON),
                                           GetSystemMetrics(SM_CYICON));
        HICON hIconSm  = td5_load_app_icon(GetSystemMetrics(SM_CXSMICON),
                                           GetSystemMetrics(SM_CYSMICON));

        ZeroMemory(&wc, sizeof(wc));
        wc.cbSize        = sizeof(wc);
        wc.style         = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc   = TD5_WndProc;
        wc.hInstance      = GetModuleHandleA(NULL);
        wc.hIcon          = hIconBig;
        wc.hIconSm        = hIconSm;
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
        if (hIconBig)
            SendMessageA(s_hwnd, WM_SETICON, ICON_BIG,   (LPARAM)hIconBig);
        if (hIconSm)
            SendMessageA(s_hwnd, WM_SETICON, ICON_SMALL, (LPARAM)hIconSm);
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

    /* [S01] effective sync interval = caller's request AND the Display-options
     * VSync toggle (g_backend.vsync). Loading screens pass vsync=0 to stay
     * uncapped; race/frontend pass 1 and thus follow the option. */

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
        IDXGISwapChain_Present(g_backend.swap_chain, (vsync && g_backend.vsync) ? 1 : 0, 0);
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
        IDXGISwapChain_Present(g_backend.swap_chain, (vsync && g_backend.vsync) ? 1 : 0, 0);

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
        IDXGISwapChain_Present(g_backend.swap_chain, (vsync && g_backend.vsync) ? 1 : 0, 0);
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
    IDXGISwapChain_Present(g_backend.swap_chain, (vsync && g_backend.vsync) ? 1 : 0, 0);

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
        int got = 0;
        memset(&js, 0, sizeof(js));

        /* [S27] Re-Acquire throttle: once a slot is flagged lost, only retry the
         * Poll/Acquire every TD5_JS_REACQ_HOLDOFF_FRAMES frames so an unplugged
         * pad doesn't hammer DirectInput each frame. A live (non-lost) slot polls
         * every frame exactly as before. */
        int try_poll = 1;
        if (s_js_lost[slot]) {
            if (s_js_reacq_holdoff[slot] > 0) { s_js_reacq_holdoff[slot]--; try_poll = 0; }
            else s_js_reacq_holdoff[slot] = TD5_JS_REACQ_HOLDOFF_FRAMES;
        }
        if (try_poll) {
            hr = IDirectInputDevice8_Poll(s_di_joystick[slot]);
            if (hr == DIERR_INPUTLOST || hr == DIERR_NOTACQUIRED) {
                IDirectInputDevice8_Acquire(s_di_joystick[slot]);
                IDirectInputDevice8_Poll(s_di_joystick[slot]);
            }
            hr = IDirectInputDevice8_GetDeviceState(s_di_joystick[slot], sizeof(js), &js);
            got = SUCCEEDED(hr);
        }

        /* [S27] Disconnect bookkeeping. A persistent run of failures latches the
         * slot as lost; the next good read clears it (device reconnected). */
        if (got) {
            if (s_js_lost[slot])
                TD5_LOG_I(LOG_TAG, "joystick slot=%d reacquired (controller back)", slot);
            s_js_lost[slot] = 0;
            s_js_fail_streak[slot] = 0;
        } else if (try_poll) {
            if (s_js_fail_streak[slot] < 1000000) s_js_fail_streak[slot]++;
            if (!s_js_lost[slot] && s_js_fail_streak[slot] >= TD5_JS_LOST_FAIL_THRESHOLD) {
                s_js_lost[slot] = 1;
                s_js_reacq_holdoff[slot] = TD5_JS_REACQ_HOLDOFF_FRAMES;
                TD5_LOG_W(LOG_TAG, "joystick slot=%d lost (controller disconnected after %d failed polls)",
                          slot, s_js_fail_streak[slot]);
            }
        }

        if (got) {
            static const uint32_t k_action_bits[6] = {
                TD5_INPUT_HANDBRAKE, TD5_INPUT_HORN, TD5_INPUT_GEAR_UP,
                TD5_INPUT_GEAR_DOWN, TD5_INPUT_CAMERA_CHANGE, TD5_INPUT_REAR_VIEW
            };
            uint32_t jbits = 0;
            long ax_x, ax_y;

            /* Pick the per-action binding source. Explicit Control-Options
             * config (s_js_action_set) always wins. Otherwise default to the
             * built-in Xbox-style gamepad map (k_default_js_action_bind) — UNLESS
             * a legacy Config.td5 9-slot mapping was explicitly remapped (axis
             * swap or a real button value 2..10), which keeps the old raw-axis
             * decode in the else-branch for imported wheels/pads. */
            const uint32_t *ab;
            if (s_js_action_set[slot]) {
                ab = s_js_action_bind[slot];
            } else {
                const int32_t *lb = s_joystick_bindings[slot];
                int legacy_remap = (s_joystick_bound[slot] && lb[1] == 5 && lb[2] == 4);
                for (int k = 0; !legacy_remap && k < 6; k++)
                    if (lb[3 + k] >= 2 && lb[3 + k] <= 10) legacy_remap = 1;
                ab = legacy_remap ? NULL : k_default_js_action_bind;
            }

            if (ab) {
                /* [PORT ENHANCEMENT 2026-06] per-action bindings: each of the 10
                 * actions maps to a button or an axis/trigger direction. LEFT/RIGHT
                 * synthesize the analog steer axis, ACCELERATE/BRAKE the throttle
                 * axis (so a trigger drives them analog), the rest are digital. */
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
                /* Legacy Config.td5 raw mapping (imported 2-axis wheels/pads that
                 * explicitly remapped a button or axis): X = steering, Y =
                 * throttle/brake; physical buttons 2..7 →
                 * handbrake/horn/gear/camera/rear. The legacy [1]/[2] axis-swap is
                 * honoured for 2-axis wheels. Fresh setups never reach here — they
                 * use k_default_js_action_bind above. */
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

/* Pop the next queued typed character (WM_CHAR). Returns 0 when the queue is
 * empty. Drains independently of frame rate so no keystroke is ever dropped. */
int td5_plat_input_get_char(void)
{
    int ch;
    if (s_char_q_tail == s_char_q_head) return 0;
    ch = (int)s_char_queue[s_char_q_tail];
    s_char_q_tail = (s_char_q_tail + 1) % TD5_CHAR_QUEUE_SIZE;
    return ch;
}

/* Discard any pending typed characters (called when a text field opens so
 * keystrokes from the prior screen don't leak in). */
void td5_plat_input_flush_chars(void)
{
    s_char_q_tail = s_char_q_head;
}

/* Pop the next queued nav event (WM_KEYDOWN FIFO). Returns a single TD5_NAVKEY_*
 * code, or 0 when empty. Frame-rate independent: every cursor/Enter tap captured
 * by the window proc between two slow frames is preserved in order, so a burst of
 * presses during an MS spike is drained press-for-press instead of collapsed. */
unsigned td5_plat_input_nav_pop(void)
{
    unsigned code;
    if (s_nav_q_tail == s_nav_q_head) return 0;
    code = (unsigned)s_nav_queue[s_nav_q_tail];
    s_nav_q_tail = (s_nav_q_tail + 1) % TD5_NAV_QUEUE_SIZE;
    return code;
}

/* Discard all pending nav events and the ESC latch (called when leaving the menu
 * or while a text field is open so stale presses don't fire on the next frame). */
void td5_plat_input_flush_nav(void)
{
    s_nav_q_tail = s_nav_q_head;
    s_esc_latch = 0;
}

/* One-shot ESC read-and-clear: 1 if a WM_KEYDOWN VK_ESCAPE arrived since the last
 * call. Lets a quick ESC the immediate read missed still back out, at most once
 * per frame. */
int td5_plat_input_esc_taken(void)
{
    if (s_esc_latch) { s_esc_latch = 0; return 1; }
    return 0;
}

/* Device-detection override (blocklist).
 *
 * Any controller whose DirectInput instance name CONTAINS one of these strings
 * (case-insensitive) is dropped during enumeration: it is never added to
 * s_device_names[]/s_device_guids[], so it cannot be detected, listed in the
 * controller-config / multiplayer-join UI, nor assigned to a player slot.
 *
 * The user's "Keychron Link" keyboards enumerate under DI8DEVCLASS_GAMECTRL as
 * game controllers but are not real pads/wheels; this filter keeps them out of
 * the controller pool entirely. Edit the list to block other devices. */
static const char *const s_blocked_device_substrings[] = {
    "keychron link",
};

static int td5_plat_input_device_name_is_blocked(const char *name)
{
    char low[256];
    size_t i;
    if (!name) return 0;
    /* Lower-case a bounded copy, then substring-match each blocked entry. */
    for (i = 0; i + 1 < sizeof(low) && name[i]; i++) {
        char c = name[i];
        low[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    }
    low[i] = '\0';
    for (i = 0; i < sizeof(s_blocked_device_substrings) /
                    sizeof(s_blocked_device_substrings[0]); i++) {
        if (strstr(low, s_blocked_device_substrings[i]))
            return 1;
    }
    return 0;
}

/* Joystick enumeration callback */
static BOOL CALLBACK EnumJoysticksCallback(
    const DIDEVICEINSTANCEA *inst, void *ctx)
{
    (void)ctx;
    /* Override: blocked devices are skipped outright — never detected or usable. */
    if (td5_plat_input_device_name_is_blocked(inst->tszInstanceName)) {
        TD5_LOG_I(LOG_TAG, "Skipping blocked input device: \"%s\"",
                  inst->tszInstanceName);
        return DIENUM_CONTINUE;
    }
    if (s_device_count < 16) {
        strncpy(s_device_names[s_device_count],
                inst->tszInstanceName,
                sizeof(s_device_names[0]) - 1);
        s_device_names[s_device_count][255] = '\0';
        /* Store the instance GUID so td5_plat_input_set_device can (re)create
         * this exact device later — without it, switching to any joystick is
         * impossible (s_di_joystick was never created). */
        s_device_guids[s_device_count] = inst->guidInstance;
        s_device_product_guids[s_device_count] = inst->guidProduct;
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

/* Disambiguate identically-named devices.
 *
 * Two physically-identical controllers (e.g. a pair of the same gamepad/wheel)
 * report the SAME tszInstanceName from DirectInput; only their instance GUID
 * (s_device_guids[]) differs — which is what actually binds a device to a slot,
 * so the two are already independently selectable. The display name, however,
 * is identical, leaving the user unable to tell them apart in the controller
 * config / multiplayer-join UI.
 *
 * This pass scans the freshly-enumerated names and, for any name shared by two
 * or more devices, appends " #1", " #2", ... in enumeration order. A device
 * with a unique name is left untouched (no lone "#1"). Numbering follows
 * DirectInput's enumeration order, which is stable for a fixed set of attached
 * devices, so a given physical pad keeps its number across rescans. Operates on
 * the raw re-read names each enumeration, so it is idempotent (never stacks
 * "#1 #1"). Index 0 (the keyboard) is skipped. */
static void td5_plat_input_disambiguate_device_names(void)
{
    int numbered[16] = {0};   /* device already given a suffix this pass */
    int i, j;
    for (i = 1; i < s_device_count && i < 16; i++) {
        char key[256];    /* untrimmed comparison key (matches raw names) */
        char disp[256];   /* trimmed base used to build the displayed name */
        size_t n;
        int total = 0, ord = 0;
        if (numbered[i]) continue;
        /* How many not-yet-numbered devices share this exact name? */
        for (j = i; j < s_device_count && j < 16; j++) {
            if (!numbered[j] && strcmp(s_device_names[j], s_device_names[i]) == 0)
                total++;
        }
        if (total < 2) continue;   /* unique name -> leave it plain */
        /* Capture the shared name. `key` stays untrimmed so the strcmp below
         * still matches the raw enumerated names (incl. the first occurrence);
         * `disp` is trimmed of trailing whitespace so a name with a trailing
         * space (e.g. "Keychron Link ") renders "...Link #1", not "...Link  #1".
         * The "%.*s #%d" precision bounds `disp` so the " #NN" suffix can never
         * overflow the 256-byte buffer. */
        strncpy(key, s_device_names[i], sizeof(key) - 1);
        key[sizeof(key) - 1] = '\0';
        strncpy(disp, key, sizeof(disp));
        n = strlen(disp);
        while (n > 0 && (disp[n - 1] == ' ' || disp[n - 1] == '\t'))
            disp[--n] = '\0';
        for (j = i; j < s_device_count && j < 16; j++) {
            if (!numbered[j] && strcmp(s_device_names[j], key) == 0) {
                ord++;
                snprintf(s_device_names[j], sizeof(s_device_names[j]),
                         "%.*s #%d", (int)(sizeof(s_device_names[j]) - 8),
                         disp, ord);
                numbered[j] = 1;
            }
        }
    }
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
    /* Append " #1"/" #2"/... to any controllers that share an identical name
     * so two of the same model can be told apart in the config UI. */
    td5_plat_input_disambiguate_device_names();
    TD5_LOG_I(LOG_TAG, "Input devices enumerated: count=%d primary=%s",
              s_device_count,
              (s_device_count > 0) ? s_device_names[0] : "<none>");
    for (int di = 1; di < s_device_count && di < 16; di++) {
        /* Log the full DirectInput instance + product GUID per joystick. Two
         * physically-distinct controllers normally get distinct INSTANCE GUIDs;
         * some pads (notably 8BitDo) report a fixed/identical instance GUID for
         * every unit, which the binding layer (CreateDevice-by-GUID) cannot tell
         * apart — so two such pads collapse onto one bindable device. The log
         * makes that collision visible. */
        const GUID *ig = &s_device_guids[di];
        const GUID *pg = &s_device_product_guids[di];
        TD5_LOG_I(LOG_TAG,
                  "  device[%d] type=%d name=\"%s\"",
                  di, s_device_types[di], s_device_names[di]);
        TD5_LOG_I(LOG_TAG,
                  "    inst=%08lX-%04X-%04X-%02X%02X%02X%02X%02X%02X%02X%02X "
                  "prod=%08lX",
                  (unsigned long)ig->Data1, ig->Data2, ig->Data3,
                  ig->Data4[0], ig->Data4[1], ig->Data4[2], ig->Data4[3],
                  ig->Data4[4], ig->Data4[5], ig->Data4[6], ig->Data4[7],
                  (unsigned long)pg->Data1);
    }
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
        ZeroMemory(&s_di_joystick_guid[slot], sizeof(s_di_joystick_guid[slot]));
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
            ZeroMemory(&s_di_joystick_guid[slot], sizeof(s_di_joystick_guid[slot]));
            return;
        }

        /* Remember which enumerated device this slot is now bound to, so the FF
         * layer can identify it for XInput rumble routing. [#1 2026-06-15] */
        s_di_joystick_guid[slot] = s_device_guids[device_index];

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

/* [S27 2026-06-05] Report whether the joystick bound to this player slot has
 * been persistently lost (physically disconnected). Returns 0 for a slot with
 * no joystick (keyboard player) so callers never modal a keyboard player. The
 * lost latch is maintained by the in-race poll above; a slot whose device went
 * away keeps its s_di_joystick handle (so we can re-Acquire on reconnect). */
int td5_plat_input_joystick_is_lost(int device_slot)
{
    if (device_slot < 0 || device_slot >= TD5_PLAT_MAX_JS_SLOTS) return 0;
    if (!s_di_joystick[device_slot]) return 0;   /* keyboard / no device assigned */
    return s_js_lost[device_slot];
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

/* [nav UP-stuck fix 2026-06-15] A DirectInput POV hat is "centered" (not pressed)
 * when its value is 0xFFFFFFFF *per the docs* — but many drivers report a centered
 * hat as 0xFFFF (65535), or stuff junk in the high word. The old `!= 0xFFFFFFFFu`
 * test then treated a resting hat as deg=655, which `deg > 270` reads as a
 * permanent UP — so the UP edge never re-armed and "can't navigate up" (down/
 * left/right unaffected). Treat any value whose low word is 0xFFFF as centered. */
static int pov_centered(DWORD pov)
{
    return (pov == 0xFFFFFFFFu) || ((pov & 0xFFFFu) == 0xFFFFu) || ((pov / 100u) > 360u);
}

/* [stuck-axis nav fix 2026-06-15] Per-enumerated-device stick liveness. A stick
 * axis is only trusted for menu nav once it has been observed AT REST (inside the
 * deadband). Some pads power on reporting a missing or trigger-style axis as a
 * constant min/max; that would assert a PERMANENT direction — the nav edge never
 * re-arms ("can't navigate up") and the car-select hold-repeat's held-direction
 * never releases ("holding never stops"). A real self-centering stick passes on
 * the first at-rest frame, so normal stick nav is unaffected. Indexed by the
 * enumerated device index used by scan_dev(). */
static unsigned char s_js_x_live[16];
static unsigned char s_js_y_live[16];

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
    if (!pov_centered(js.rgdwPOV[0])) { s_neutral_have_prev = 0; s_neutral_stable_count = 0; return 0; }

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
/* One-shot: returns 1 (and clears the latch) if a WM_DEVICECHANGE has arrived
 * since the last call. The frontend gates its EnumDevices rescan on this so the
 * blocking enumeration runs only on an actual device hot-plug, not every frame. */
int td5_plat_input_devices_changed(void)
{
    if (s_devices_dirty) { s_devices_dirty = 0; return 1; }
    return 0;
}

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

const uint32_t *td5_plat_input_default_action_bindings(void)
{
    return k_default_js_action_bind;
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
        /* Trust a stick axis only after it has been seen at rest (see s_js_*_live):
         * an axis pegged from power-on never centers, so it can never jam a
         * direction. The dpad (POV) and buttons below are always honoured. */
        if (cx >= -T && cx <= T) s_js_x_live[i] = 1;
        if (cy >= -T && cy <= T) s_js_y_live[i] = 1;
        if (s_js_x_live[i]) { if (cx < -T) db |= 1; if (cx > T) db |= 2; }
        if (s_js_y_live[i]) { if (cy < -T) db |= 4; if (cy > T) db |= 8; }  /* stick up = lY low = UP */
        if (!pov_centered(js.rgdwPOV[0])) {
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
    if (!pov_centered(js.rgdwPOV[0])) {
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

/* Navigation bitmask for ONE enumerated device via its shared non-exclusive scan
 * handle (scan_dev). Used by the simultaneous-multiplayer car-select grid, where
 * several pads are polled independently while the lobby scan handles are still
 * alive (per-player EXCLUSIVE devices aren't bound until the picks are locked).
 * Same bit encoding as td5_plat_input_frontend_nav. [PORT ENHANCEMENT 2026-06-07] */
uint32_t td5_plat_input_device_nav(int enum_index)
{
    DIJOYSTATE2 js;
    uint32_t db = 0;
    long cx, cy;
    const long T = 130;
    LPDIRECTINPUTDEVICE8A dev;
    if (enum_index <= 0 || enum_index >= s_device_count || enum_index >= 16) return 0;
    dev = scan_dev(enum_index);
    if (!dev) return 0;
    memset(&js, 0, sizeof(js));
    if (FAILED(IDirectInputDevice8_Poll(dev))) {
        IDirectInputDevice8_Acquire(dev);
        IDirectInputDevice8_Poll(dev);
    }
    if (FAILED(IDirectInputDevice8_GetDeviceState(dev, sizeof(js), &js))) return 0;
    cx = js.lX - TD5_PLAT_JS_AXIS_CENTER;
    cy = js.lY - TD5_PLAT_JS_AXIS_CENTER;
    if (cx < -T) db |= 1; if (cx > T) db |= 2;
    if (cy < -T) db |= 4; if (cy > T) db |= 8;       /* stick up = lY low = UP */
    if (!pov_centered(js.rgdwPOV[0])) {
        DWORD deg = js.rgdwPOV[0] / 100;
        if (deg > 270 || deg <  90) db |= 4;
        if (deg >  90 && deg < 270) db |= 8;
        if (deg > 180 && deg < 360) db |= 1;
        if (deg >   0 && deg < 180) db |= 2;
    }
    if (js.rgbButtons[0] & 0x80) db |= 0x10;         /* A = confirm/select */
    if (js.rgbButtons[1] & 0x80) db |= 0x20;         /* B = back/cancel    */
    if (js.rgbButtons[7] & 0x80) db |= 0x40;         /* Start / Menu        */
    return db;
}

/* ========================================================================
 * XInput rumble (force-feedback for gamepads) [#1 2026-06-15]
 *
 * 8BitDo pads (and most modern XInput gamepads) expose rumble via XInput
 * (XInputSetState + XINPUT_VIBRATION), NOT DirectInput force-feedback EFFECTS.
 * On such a device IDirectInputDevice8::GetCapabilities reports no
 * DIDC_FORCEFEEDBACK and the DI-effect path (below) silently no-ops, so FF
 * "does nothing". This block adds an XInput rumble output and the FF entry
 * points (td5_plat_ff_constant / td5_plat_ff_stop) prefer it for any device
 * that turns out to be XInput-capable; DI effects remain the path for wheels.
 *
 * Knob TD5RE_FF_XINPUT (cached): default ON; "0" reverts to DI-effects only.
 *
 * Linkage: XInput is loaded DYNAMICALLY (LoadLibraryA + GetProcAddress, like
 * the dwmapi block in td5_plat_init) so a missing xinput DLL can never break
 * startup and the build needs NO -lxinput. We declare the tiny ABI we use here
 * rather than #include <xinput.h> (mirrors the inline-typedef dwmapi precedent).
 * ======================================================================== */

typedef struct TD5_XINPUT_VIBRATION { WORD wLeftMotorSpeed; WORD wRightMotorSpeed; } TD5_XINPUT_VIBRATION;
/* XInputGetState wants an XINPUT_STATE; we only read dwPacketNumber + need the
 * right byte size, so use a fixed 16-byte blob (dwPacketNumber + XINPUT_GAMEPAD). */
typedef struct TD5_XINPUT_STATE { DWORD dwPacketNumber; BYTE pad[12]; } TD5_XINPUT_STATE;
typedef DWORD (WINAPI *PFN_XInputSetState)(DWORD, TD5_XINPUT_VIBRATION *);
typedef DWORD (WINAPI *PFN_XInputGetState)(DWORD, TD5_XINPUT_STATE *);

#define TD5_XINPUT_MAX_USERS 4   /* XUSER_MAX_COUNT */
#define TD5_ERROR_SUCCESS    0L

static PFN_XInputSetState s_xi_set = NULL;
static PFN_XInputGetState s_xi_get = NULL;
static int  s_xi_loaded = 0;     /* DLL probed (1 even if not found) */
static int  s_xi_avail  = 0;     /* both procs resolved */
/* Per-player-slot XInput routing: the XInput user index (0..3) the slot's pad
 * was assigned, or -1 = not an XInput device (use DI effects). Cached low/high
 * motor magnitudes (0..65535) so we only XInputSetState on change, and so the
 * four DI effect-slots can each own one motor without stomping the other. */
static int  s_xi_user[TD5_PLAT_MAX_JS_SLOTS];
static WORD s_xi_low [TD5_PLAT_MAX_JS_SLOTS];   /* low-freq (heavy) motor (resolved) */
static WORD s_xi_high[TD5_PLAT_MAX_JS_SLOTS];   /* high-freq (light) motor */
/* The low (heavy) motor is driven by two independent contributors so a stop on
 * one doesn't silence the other: [0]=side-collision pulse (effect slot 2),
 * [1]=continuous buzz (redline-in-manual OR drift — the input layer passes the
 * louder of the two to td5_plat_ff_xinput_rumble; #5(3) 2026-06-16). The resolved
 * s_xi_low is max(contrib[0],contrib[1]). [gamepad-rumble fix 2026-06-15]
 * Continuous wheel forces — steering (slot 0) and terrain (slot 3) — are NOT
 * routed here, so a gamepad is silent during steady normal driving; the buzz
 * contributor is 0 whenever the car is neither at the limiter nor sliding. */
#define TD5_XI_LOW_COLLISION 0   /* contributor index: side-collision pulse */
#define TD5_XI_LOW_REDLINE   1   /* contributor index: continuous buzz (redline-in-manual / drift) */
static WORD s_xi_low_contrib[TD5_PLAT_MAX_JS_SLOTS][2];

/* TD5RE_FF_XINPUT (cached): master gate for the XInput rumble path. Default ON. */
static int td5_ff_xinput_enabled(void)
{
    static int s_init = 0;
    static int s_on = 1;
    if (!s_init) {
        const char *e = getenv("TD5RE_FF_XINPUT");
        s_on = (e && e[0] == '0') ? 0 : 1;   /* default ON */
        s_init = 1;
        TD5_LOG_I(LOG_TAG, "FF XInput rumble path: %s",
                  s_on ? "enabled (default)" : "disabled (DI effects only)");
    }
    return s_on;
}

/* Lazily resolve XInput once. Tries the modern DLLs first, then the legacy ones.
 * Idempotent; safe to call before any device exists. */
static int td5_xinput_load(void)
{
    static const char *names[] = {
        "xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll", "xinput1_2.dll", "xinput1_1.dll"
    };
    HMODULE h = NULL;
    int i;
    if (s_xi_loaded) return s_xi_avail;
    s_xi_loaded = 1;
    for (i = 0; i < (int)(sizeof(names) / sizeof(names[0])); i++) {
        h = LoadLibraryA(names[i]);
        if (h) break;
    }
    if (h) {
        s_xi_set = (PFN_XInputSetState)(void *)GetProcAddress(h, "XInputSetState");
        s_xi_get = (PFN_XInputGetState)(void *)GetProcAddress(h, "XInputGetState");
        s_xi_avail = (s_xi_set != NULL && s_xi_get != NULL);
        /* Keep the DLL loaded for the lifetime of the app (matches dwmapi). */
    }
    TD5_LOG_I(LOG_TAG, "XInput load: %s (dll=%s)",
              s_xi_avail ? "available" : "unavailable",
              (i < (int)(sizeof(names)/sizeof(names[0])) && h) ? names[i] : "<none>");
    return s_xi_avail;
}

/* Is the DirectInput device at enumeration index `device_index` an XInput pad?
 *
 * MSDN-documented detection: an XInput device's name in the raw-input device
 * list contains the substring "IG_"; its DirectInput PRODUCT GUID encodes the
 * VID/PID in Data1 (== MAKELONG(VID,PID)). We scan GetRawInputDeviceList for
 * HID devices whose name contains "IG_", parse their VID_xxxx&PID_xxxx, and
 * test whether the device's product-GUID device-id matches. Uses only
 * user32/GetRawInputDeviceInfo (no extra lib). Returns 1 if XInput. */
static int td5_device_is_xinput(int device_index)
{
    UINT n_dev = 0, i;
    PRAWINPUTDEVICELIST list = NULL;
    DWORD target_id;            /* MAKELONG(VID,PID) for this DI device */
    int is_xi = 0;

    if (device_index <= 0 || device_index >= s_device_count || device_index >= 16)
        return 0;
    target_id = (DWORD)s_device_product_guids[device_index].Data1;

    if (GetRawInputDeviceList(NULL, &n_dev, sizeof(RAWINPUTDEVICELIST)) != 0 || n_dev == 0)
        return 0;
    list = (PRAWINPUTDEVICELIST)malloc(sizeof(RAWINPUTDEVICELIST) * n_dev);
    if (!list) return 0;
    if (GetRawInputDeviceList(list, &n_dev, sizeof(RAWINPUTDEVICELIST)) == (UINT)-1) {
        free(list);
        return 0;
    }

    for (i = 0; i < n_dev && !is_xi; i++) {
        char name[512];
        UINT name_len = (UINT)sizeof(name);
        unsigned int vid = 0, pid = 0;
        const char *p;
        if (list[i].dwType != RIM_TYPEHID) continue;
        if (GetRawInputDeviceInfoA(list[i].hDevice, RIDI_DEVICENAME, name, &name_len) == (UINT)-1)
            continue;
        if (!strstr(name, "IG_")) continue;   /* not an XInput-class device */
        /* Device path looks like \\?\HID#VID_045E&PID_028E&IG_00#... */
        p = strstr(name, "VID_");
        if (p) sscanf(p + 4, "%4x", &vid);
        p = strstr(name, "PID_");
        if (p) sscanf(p + 4, "%4x", &pid);
        if (vid && pid && (DWORD)MAKELONG(vid, pid) == target_id)
            is_xi = 1;
    }
    free(list);
    return is_xi;
}

/* Pick an XInput user index (0..3) for a slot whose DI device is XInput-class.
 * XInput exposes no DI-instance correlation, so we hand out connected user
 * indices in ascending order, skipping ones already claimed by another slot.
 * Returns -1 if none free/connected. */
static int td5_xinput_assign_user(int device_slot)
{
    int u, s;
    if (!s_xi_get) return -1;
    for (u = 0; u < TD5_XINPUT_MAX_USERS; u++) {
        TD5_XINPUT_STATE st;
        int claimed = 0;
        for (s = 0; s < TD5_PLAT_MAX_JS_SLOTS; s++)
            if (s != device_slot && s_xi_user[s] == u) { claimed = 1; break; }
        if (claimed) continue;
        ZeroMemory(&st, sizeof(st));
        if (s_xi_get((DWORD)u, &st) == TD5_ERROR_SUCCESS)
            return u;   /* connected and free */
    }
    return -1;
}

/* Push the slot's cached low/high motor magnitudes to its XInput pad. */
static void td5_xinput_push(int device_slot)
{
    TD5_XINPUT_VIBRATION vib;
    int u;
    if (device_slot < 0 || device_slot >= TD5_PLAT_MAX_JS_SLOTS) return;
    u = s_xi_user[device_slot];
    if (u < 0 || !s_xi_set) return;
    vib.wLeftMotorSpeed  = s_xi_low [device_slot];
    vib.wRightMotorSpeed = s_xi_high[device_slot];
    s_xi_set((DWORD)u, &vib);
}

/* Map a DI effect magnitude (0..DI_FFNOMINALMAX) to a motor word (0..65535). */
static WORD td5_xinput_scale(int magnitude)
{
    long m;
    if (magnitude < 0) magnitude = -magnitude;
    if (magnitude > DI_FFNOMINALMAX) magnitude = DI_FFNOMINALMAX;
    m = ((long)magnitude * 65535L) / DI_FFNOMINALMAX;
    if (m > 65535L) m = 65535L;
    return (WORD)m;
}

/* Route one effect-slot write to the XInput motors. [gamepad-rumble fix
 * 2026-06-15] Only the EVENT-driven effects reach a motor, so a gamepad never
 * buzzes during normal driving:
 *   slot 1 (TD5_FF_SLOT_FRONTAL — crash + gear JOLTS) -> HIGH-freq motor.
 *   slot 2 (TD5_FF_SLOT_SIDE   — side collision)      -> LOW-freq motor.
 *   slot 0 (steering constant)  and slot 3 (terrain periodic) -> NO motor.
 * The redline-in-manual buzz uses the other low-motor contributor, set by
 * td5_plat_ff_xinput_rumble. Returns 1 if this is an XInput device (so the
 * caller does NOT fall through to the DI-effect path); the slot-0/3 writes are
 * intentionally consumed-but-silent here. DI FF wheels keep all four slots. */
static int td5_xinput_route_effect(int device_slot, int slot, int magnitude)
{
    WORD w;
    if (!s_xi_loaded) return 0;                  /* ff_init never ran -> s_xi_user not armed */
    if (device_slot < 0 || device_slot >= TD5_PLAT_MAX_JS_SLOTS) return 0;
    if (s_xi_user[device_slot] < 0) return 0;   /* not an XInput device */
    w = td5_xinput_scale(magnitude);
    if (slot == 1) {
        s_xi_high[device_slot] = w;                       /* sharp crash/gear jolt motor */
        td5_xinput_push(device_slot);
    } else if (slot == 2) {
        /* side-collision pulse -> low motor collision contributor */
        s_xi_low_contrib[device_slot][TD5_XI_LOW_COLLISION] = w;
        s_xi_low[device_slot] = s_xi_low_contrib[device_slot][0] > s_xi_low_contrib[device_slot][1]
                              ? s_xi_low_contrib[device_slot][0] : s_xi_low_contrib[device_slot][1];
        td5_xinput_push(device_slot);
    }
    /* slot 0 (steering) and slot 3 (terrain/drift): consumed but NOT routed to a
     * motor — these are continuous wheel forces that would buzz a gamepad. */
    return 1;
}

/* Route one effect-slot STOP to the XInput motors. Returns 1 if handled here.
 * Only slots 1/2 own a motor contribution; slots 0/3 are silent no-ops. */
static int td5_xinput_route_stop(int device_slot, int slot)
{
    if (!s_xi_loaded) return 0;                  /* ff_init never ran -> s_xi_user not armed */
    if (device_slot < 0 || device_slot >= TD5_PLAT_MAX_JS_SLOTS) return 0;
    if (s_xi_user[device_slot] < 0) return 0;
    if (slot == 1) {
        s_xi_high[device_slot] = 0;
        td5_xinput_push(device_slot);
    } else if (slot == 2) {
        s_xi_low_contrib[device_slot][TD5_XI_LOW_COLLISION] = 0;
        s_xi_low[device_slot] = s_xi_low_contrib[device_slot][0] > s_xi_low_contrib[device_slot][1]
                              ? s_xi_low_contrib[device_slot][0] : s_xi_low_contrib[device_slot][1];
        td5_xinput_push(device_slot);
    }
    /* slot 0 / slot 3 own no motor contribution -> nothing to clear. */
    return 1;
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
    int xinput_ready = 0;

    /* s_xi_user[] is zero-initialized by the C runtime, but 0 is a VALID XInput
     * user index, so on first entry mark every slot "not XInput" (-1). */
    {
        static int s_xi_user_inited = 0;
        if (!s_xi_user_inited) {
            int s;
            for (s = 0; s < TD5_PLAT_MAX_JS_SLOTS; s++) s_xi_user[s] = -1;
            s_xi_user_inited = 1;
        }
    }

    if (device_slot < 0 || device_slot >= TD5_PLAT_MAX_JS_SLOTS) {
        return 0;
    }

    /* Force feedback runs on this player's joystick (the wheel/pad). */
    if (!s_di_joystick[device_slot]) {
        return 0;
    }

    /* [#1 2026-06-15] Prefer XInput rumble for XInput-capable pads (8BitDo &
     * most modern gamepads), which expose rumble via XInputSetState and report
     * NO DIDC_FORCEFEEDBACK to DirectInput (so the DI-effect path below would
     * silently no-op on them). Wheels keep the DI-effect path. We resolve which
     * enumerated device this slot is bound to via its instance GUID, test it for
     * XInput, and claim a user index. DI-effect init still runs afterwards so a
     * device that happens to support both keeps the richer effect path too. */
    s_xi_user[device_slot] = -1;
    s_xi_low[device_slot] = s_xi_high[device_slot] = 0;
    s_xi_low_contrib[device_slot][0] = s_xi_low_contrib[device_slot][1] = 0;
    if (td5_ff_xinput_enabled() && td5_xinput_load()) {
        int di = -1, k;
        for (k = 1; k < s_device_count && k < 16; k++) {
            if (IsEqualGUID(&s_device_guids[k], &s_di_joystick_guid[device_slot])) {
                di = k;
                break;
            }
        }
        if (di > 0 && td5_device_is_xinput(di)) {
            int u = td5_xinput_assign_user(device_slot);
            if (u >= 0) {
                s_xi_user[device_slot] = u;
                xinput_ready = 1;
                td5_xinput_push(device_slot);   /* ensure motors start at rest */
                TD5_LOG_I(LOG_TAG,
                          "FF: slot=%d device=%d (%s) using XInput rumble user=%d",
                          device_slot, di, s_device_names[di], u);
            } else {
                TD5_LOG_W(LOG_TAG,
                          "FF: slot=%d device=%d is XInput but no free user index",
                          device_slot, di);
            }
        }
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
        /* No DI force-feedback (the 8BitDo / typical-gamepad case). If we routed
         * this slot to XInput rumble above, FF is still functional — report
         * success so the input layer drives effects (which we forward to the
         * motors). Otherwise this device truly has no FF. */
        ZeroMemory(&s_ff[device_slot], sizeof(s_ff[device_slot]));
        if (xinput_ready) {
            TD5_LOG_I(LOG_TAG, "FF init ok (XInput rumble only): dev=%d", device_slot);
        }
        return xinput_ready;
    }

    s_ff[device_slot].is_wheel = ((GET_DIDEVICE_TYPE(caps.dwDevType) == DI8DEVTYPE_DRIVING) ||
                     (GET_DIDEVICE_TYPE(caps.dwDevType) == DI8DEVTYPE_1STPERSON));

    IDirectInputDevice8_Acquire(s_ff[device_slot].device);

    if (FAILED(IDirectInputDevice8_EnumEffects(s_ff[device_slot].device,
            EnumConstantForceEffectsCallback, &s_ff[device_slot].effectGuid, DIEFT_ALL)) ||
        !IsEqualGUID(&s_ff[device_slot].effectGuid, &GUID_ConstantForce)) {
        ZeroMemory(&s_ff[device_slot], sizeof(s_ff[device_slot]));
        return xinput_ready;   /* XInput rumble still usable if it was assigned */
    }

    if (!td5_ff_create_constant_effect(device_slot, 0, s_ff[device_slot].axes[0], 0) ||
        !td5_ff_create_constant_effect(device_slot, 1, s_ff[device_slot].axes[1], 9000) ||
        !td5_ff_create_constant_effect(device_slot, 2, s_ff[device_slot].axes[0], 0) ||
        !td5_ff_create_periodic_effect(device_slot, 3)) {
        td5_ff_release_effects(device_slot);
        ZeroMemory(&s_ff[device_slot], sizeof(s_ff[device_slot]));
        return xinput_ready;   /* XInput rumble still usable if it was assigned */
    }

    TD5_LOG_I(LOG_TAG, "FF init ok: dev=%d is_wheel=%d xinput_user=%d",
              device_slot, s_ff[device_slot].is_wheel, s_xi_user[device_slot]);
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
        /* Stop the motors and release the XInput routing for this slot. */
        if (s_xi_user[dev] >= 0) {
            s_xi_low[dev] = s_xi_high[dev] = 0;
            s_xi_low_contrib[dev][0] = s_xi_low_contrib[dev][1] = 0;
            td5_xinput_push(dev);
        }
        s_xi_user[dev] = -1;
    }
}

void td5_plat_ff_constant(int device_slot, int slot, int magnitude)
{
    HRESULT hr;

    if (device_slot < 0 || device_slot >= TD5_PLAT_MAX_JS_SLOTS) {
        return;
    }
    if (slot < 0 || slot >= 4) {
        return;
    }
    /* [#1 2026-06-15] XInput-routed device: drive the motors instead of DI
     * effects (an XInput-only pad has no s_ff[...].effects[] at all). */
    if (td5_xinput_route_effect(device_slot, slot, magnitude)) {
        return;
    }
    if (!s_ff[device_slot].effects[slot]) {
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

/* [#1 2026-06-15; #5(3) 2026-06-16] GAMEPAD continuous-buzz motor. Drives the low
 * (heavy) motor's dedicated buzz contributor with `magnitude` (0..DI_FFNOMINALMAX);
 * 0 silences it. No-op on a DI FF wheel (s_xi_user < 0) — there the same buzz
 * already rides on effect slot 3, so the wheel path is untouched. The input layer
 * passes the louder of redline-in-manual and drift here (0 when neither applies),
 * so the gamepad motor sees ONLY that buzz (never terrain/steering) and returns to
 * silence the moment the car stops drifting / leaves the limiter. */
void td5_plat_ff_xinput_rumble(int device_slot, int magnitude)
{
    WORD w;
    if (!s_xi_loaded) return;                    /* ff_init never ran -> not armed */
    if (device_slot < 0 || device_slot >= TD5_PLAT_MAX_JS_SLOTS) return;
    if (s_xi_user[device_slot] < 0) return;      /* not an XInput device (DI wheel) */
    w = td5_xinput_scale(magnitude);
    if (w == s_xi_low_contrib[device_slot][TD5_XI_LOW_REDLINE])
        return;                                  /* unchanged -> skip the SetState */
    s_xi_low_contrib[device_slot][TD5_XI_LOW_REDLINE] = w;
    s_xi_low[device_slot] = s_xi_low_contrib[device_slot][0] > s_xi_low_contrib[device_slot][1]
                          ? s_xi_low_contrib[device_slot][0] : s_xi_low_contrib[device_slot][1];
    td5_xinput_push(device_slot);
}

void td5_plat_ff_stop(int device_slot, int slot)
{
    if (device_slot < 0 || device_slot >= TD5_PLAT_MAX_JS_SLOTS) {
        return;
    }
    if (slot < 0 || slot >= 4) {
        return;
    }
    /* [#1 2026-06-15] XInput-routed device: clear that effect's motor. */
    if (td5_xinput_route_stop(device_slot, slot)) {
        return;
    }
    if (!s_ff[device_slot].effects[slot]) {
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

    audio_build_vol_table();          /* faithful per-voice volume curve */

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

    /* Reap finished one-shots up front so they don't linger as "active" voices
     * and slowly creep the live count toward the cap over a long race (each
     * collision/hit allocates a one-shot voice that stays allocated until
     * reclaimed). Loops are intentionally NOT reaped here — they are meant to
     * keep playing; a momentarily-idle loop is recovered by the per-buffer dedup
     * in td5_plat_audio_play, not torn down. */
    for (i = 0; i < MAX_AUDIO_CHANNELS; i++) {
        if (s_ds_channels[i] && !s_ds_channel_loop[i]) {
            DWORD status = 0;
            IDirectSoundBuffer_GetStatus(s_ds_channels[i], &status);
            if (!(status & DSBSTATUS_PLAYING)) {
                audio_release_channel(i);
            }
        }
    }

    /* Pass 1: an empty slot. */
    for (i = 0; i < MAX_AUDIO_CHANNELS; i++) {
        if (!s_ds_channels[i]) return i;
    }
    /* Pass 2: steal the oldest still-playing ONE-SHOT (lowest serial, loop==0).
     *
     * [S11] Looping voices are NEVER reclaimed or stolen here. The mixer keeps
     * the slot->channel handle of every engine/skid/siren/traffic loop and
     * modulates it every frame via slot_modify(); if find_free_channel recycled
     * that channel for another sound, the slot's handle would resolve stale and
     * slot_modify() would silently no-op — leaving the loop frozen at its last
     * volume/pitch for the rest of the race (the "engine/skid sound locks into a
     * constant loop when I drift or get rear-ended" bug: a drift/collision spawns
     * one-shot hits that used to steal the engine/skid voice's channel). A
     * momentarily-idle loop (a volume-0 buffer WASAPI parked, a lost buffer) is
     * NOT torn down — the per-buffer dedup in td5_plat_audio_play restarts it in
     * place on the next re-issue, keeping its channel and handle stable. The
     * per-buffer dedup caps loops at ~one per distinct sound (well under 64), so
     * pinning them never starves one-shots. */
    for (i = 0; i < MAX_AUDIO_CHANNELS; i++) {
        if (s_ds_channel_loop[i]) continue;
        if (steal < 0 || s_ds_channel_serial[i] < steal_serial) {
            steal = i;
            steal_serial = s_ds_channel_serial[i];
        }
    }
    if (steal >= 0) {
        s_audio_steal_count++;
        audio_release_channel(steal);
        return steal;
    }
    return -1;
}

/** Fill s_vol_table with the original M2DX per-voice attenuation curve. Safe to
 *  call before/after DirectSound init; uses only libm. */
static void audio_build_vol_table(void)
{
    int i;
    double k = 500.0 / log10(2.0);  /* == 1660.964..., the original's scale */
    for (i = 0; i < 128; i++) {
        /* (int) truncates toward zero, matching the original's __ftol(). */
        s_vol_table[i] = -(int)(k * log10(128.0 / (double)(i + 1)));
    }
}

/** Final DirectSound volume (centibels) for a mixer voice volume (0..127):
 *  faithful per-voice table value plus the separate master attenuation. */
static LONG audio_effective_cb(int vol)
{
    int cb;
    if (s_audio_muted || s_audio_focus_muted) return DSBVOLUME_MIN;
    if (vol < 0)   vol = 0;
    if (vol > 127) vol = 127;
    cb = s_vol_table[vol] + s_master_offset_cb;
    if (cb < DSBVOLUME_MIN) cb = DSBVOLUME_MIN;
    if (cb > DSBVOLUME_MAX) cb = DSBVOLUME_MAX;
    return (LONG)cb;
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

    /* [S11] One looping voice per source buffer.
     *
     * The original M2DX kept a fixed buffer per sound and re-Played THAT buffer;
     * it never allocated a voice per Play(). The port instead DuplicateSoundBuffer's
     * a fresh voice every Play and only tracks the latest in the caller's
     * slot->channel table — so when the per-frame mixer re-issues a loop start
     * (which it does whenever GetStatus transiently reports the looping buffer
     * idle: a volume-0 buffer WASAPI parked, a lost buffer, a stolen voice), each
     * re-issue stranded the previous voice. With >6 racers those orphans piled up
     * ~2/frame until all 64 voices were copies of one engine loop, droning over
     * everything — the "overload / stuck on one sound" the user hit. Slot-level
     * stop-before-replay couldn't catch it (the slot's stored handle had already
     * gone stale). Deduping at the source buffer is reliable: a looping buffer
     * gets exactly ONE voice. A re-issue finds that voice and reuses it —
     * reapplying volume/pan/freq and restarting it only if it actually stopped
     * (which also recovers a lost buffer) — so nothing accumulates and an already-
     * playing loop is not even interrupted (no per-frame restart click). */
    if (loop) {
        for (ch = 0; ch < MAX_AUDIO_CHANNELS; ch++) {
            LPDIRECTSOUNDBUFFER ex = s_ds_channels[ch];
            DWORD status;
            if (!ex || !s_ds_channel_loop[ch] || s_ds_channel_buf[ch] != buffer_index)
                continue;
            status = 0;
            IDirectSoundBuffer_GetStatus(ex, &status);
            if (status & DSBSTATUS_BUFFERLOST) {
                IDirectSoundBuffer_Restore(ex);
                status = 0;
                IDirectSoundBuffer_GetStatus(ex, &status);
            }
            IDirectSoundBuffer_SetVolume(ex, audio_effective_cb(volume));
            IDirectSoundBuffer_SetPan(ex, pan_to_ds(pan));
            if (frequency > 0)
                IDirectSoundBuffer_SetFrequency(ex, td5_audio_translate_frequency(buffer_index, frequency));
            if (!(status & DSBSTATUS_PLAYING))
                IDirectSoundBuffer_Play(ex, 0, 0, DSBPLAY_LOOPING);
            s_ds_channel_serial[ch] = ++s_audio_alloc_serial; /* refresh recency vs steal */
            return (TD5_AudioChannel)(((int)s_ds_channel_gen[ch] << AUDIO_CHANNEL_INDEX_BITS) | ch);
        }
    }

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
    IDirectSoundBuffer_SetVolume(dup_buf, audio_effective_cb(volume));
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

    IDirectSoundBuffer_SetVolume(buf, audio_effective_cb(volume));
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

/* Does this handle still name a live voice this slot owns? Unlike is_playing,
 * this ignores DirectSound's transient PLAYING bit — a looping voice that was
 * momentarily parked (volume 0) or lost still counts as owned. Used so the mixer
 * doesn't tear down/re-trigger a loop it is still modulating just because the
 * backend briefly reported it idle. Returns 0 for a stale/released handle. */
int td5_plat_audio_channel_valid(TD5_AudioChannel ch)
{
    return audio_resolve_channel(ch) >= 0 ? 1 : 0;
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
    int idx;
    if (volume < 0)   volume = 0;
    if (volume > 100) volume = 100;
    s_master_volume = volume;
    /* Master attenuation = table[master>>9], exactly as DXSound::SetVolume indexes
     * it (the 0..100 slider maps to the original's 0..0xFFFF range, then >>9 gives
     * the 0..127 table index). Applied as a dB offset on every voice, which is
     * equivalent to the original scaling the summed primary buffer. */
    idx = (volume * 0xFFFF / 100) >> 9;
    if (idx < 0)   idx = 0;
    if (idx > 127) idx = 127;
    if (s_vol_table[127] == 0 && s_vol_table[0] == 0) audio_build_vol_table();
    s_master_offset_cb = s_vol_table[idx];
}

void td5_plat_audio_set_muted(int muted)
{
    s_audio_muted = muted ? 1 : 0;
}

/* Mute all DirectSound output while the game window is not the foreground
 * window; restore on refocus. Edge-triggered so it only acts on a focus
 * change. Per-voice SFX pick up s_audio_focus_muted via audio_effective_cb on
 * their next per-frame volume re-apply (looping engine/tyre voices update every
 * frame); the ambient stream plays at a fixed volume that bypasses
 * audio_effective_cb, so its buffer volume is set explicitly here. CD audio
 * (MCI cdaudio) is a separate device and is left alone. */
void td5_plat_audio_update_focus_mute(void)
{
    int focused = (!s_hwnd) || (GetForegroundWindow() == s_hwnd);
    int want    = focused ? 0 : 1;
    if (want == s_audio_focus_muted)
        return;                       /* no change -> nothing to do */
    s_audio_focus_muted = want;
    if (s_audio_stream.buffer)
        IDirectSoundBuffer_SetVolume(s_audio_stream.buffer,
                                     want ? DSBVOLUME_MIN : DSBVOLUME_MAX);
    TD5_LOG_I(LOG_TAG, "audio focus-mute: window %s -> %s",
              focused ? "focused" : "unfocused", want ? "MUTED" : "unmuted");
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
    if (s_audio_focus_muted)   /* started while alt-tabbed away: stay silent */
        IDirectSoundBuffer_SetVolume(s_audio_stream.buffer, DSBVOLUME_MIN);
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

/* ========================================================================
 * Procedural texture-free FX shaders (smoke / rain / decal / glow)
 *
 * These replace the SMOKE / RAINDROP / FADEWHT / BRAKED / POLICELT_* atlas
 * sprites with analytic, resolution-independent pixel shaders. The draw model
 * is identical to the frontend VectorUI gauge: upload a tiny b1 constant
 * buffer (global time + one shader-specific param), bind the FX pixel shader
 * via the existing PS-override path, then let the caller issue ordinary
 * td5_plat_render_draw_tris batches (the per-particle data rides in the vertex
 * COLOR0/COLOR1 channels). Lazily created on first use; if creation fails the
 * begin() returns 0 and the caller keeps its textured fallback.
 * ======================================================================== */

/* Must match cbuffer FxParams (register b1) in every ps_fx_*.hlsl. */
typedef struct FxParamsCB {
    float time;   /* seconds, monotonic */
    float p0;     /* shader-specific (glow: 0=src-alpha lamp, 1=additive) */
    float p1;     /* shader-specific (glow: gaussian tightness) */
    float p2;
} FxParamsCB;

static ID3D11PixelShader *s_fx_ps[4]   = { NULL, NULL, NULL, NULL };
static int                s_fx_tried[4] = { 0, 0, 0, 0 };
static ID3D11Buffer      *s_fx_cb       = NULL;

static ID3D11PixelShader *fx_ensure_shader(TD5_FxShader which)
{
    int i = (int)which;
    if (i < 0 || i > 3) return NULL;
    if (s_fx_ps[i]) return s_fx_ps[i];
    if (s_fx_tried[i]) return NULL;   /* already failed once — don't retry every frame */
    s_fx_tried[i] = 1;

    if (!g_backend.device) return NULL;

    const BYTE *code = NULL;
    SIZE_T      size = 0;
    const char *name = "?";
    switch (which) {
    case TD5_FX_SMOKE: code = g_ps_fx_smoke; size = sizeof(g_ps_fx_smoke); name = "smoke"; break;
    case TD5_FX_RAIN:  code = g_ps_fx_rain;  size = sizeof(g_ps_fx_rain);  name = "rain";  break;
    case TD5_FX_DECAL: code = g_ps_fx_decal; size = sizeof(g_ps_fx_decal); name = "decal"; break;
    case TD5_FX_GLOW:  code = g_ps_fx_glow;  size = sizeof(g_ps_fx_glow);  name = "glow";  break;
    default: return NULL;
    }

    HRESULT hr = ID3D11Device_CreatePixelShader(g_backend.device, code, size, NULL, &s_fx_ps[i]);
    if (FAILED(hr)) {
        s_fx_ps[i] = NULL;
        TD5_LOG_W(LOG_TAG, "fx shader '%s' create failed hr=0x%08lX", name, (unsigned long)hr);
        return NULL;
    }

    if (!s_fx_cb) {
        D3D11_BUFFER_DESC bd;
        memset(&bd, 0, sizeof(bd));
        bd.ByteWidth = sizeof(FxParamsCB);   /* 16 bytes, 16-aligned */
        bd.Usage     = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        HRESULT hcb = ID3D11Device_CreateBuffer(g_backend.device, &bd, NULL, &s_fx_cb);
        if (FAILED(hcb)) {
            s_fx_cb = NULL;
            TD5_LOG_W(LOG_TAG, "fx cbuffer create failed hr=0x%08lX", (unsigned long)hcb);
            ID3D11PixelShader_Release(s_fx_ps[i]);
            s_fx_ps[i] = NULL;
            return NULL;
        }
    }

    TD5_LOG_I(LOG_TAG, "procedural FX shader '%s' ready (texture-free)", name);
    return s_fx_ps[i];
}

int td5_plat_fx_begin(TD5_FxShader which, float time_seconds, float p0)
{
    ID3D11DeviceContext *ctx = g_backend.context;
    if (!ctx) return 0;

    ID3D11PixelShader *ps = fx_ensure_shader(which);
    if (!ps || !s_fx_cb) return 0;

    FxParamsCB cb;
    cb.time = time_seconds;
    cb.p0   = p0;
    cb.p1   = 0.0f;
    cb.p2   = 0.0f;
    ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)s_fx_cb, 0, NULL, &cb, 0, 0);
    /* b1 is never touched by td5_plat_render_draw_tris (it binds b0/fog only),
     * so this binding survives every draw inside the begin/end bracket. */
    ID3D11DeviceContext_PSSetConstantBuffers(ctx, 1, 1, &s_fx_cb);

    td5_plat_render_set_ps_override((void *)ps, SAMP_LINEAR_CLAMP);
    return 1;
}

void td5_plat_fx_end(void)
{
    td5_plat_render_clear_ps_override();
}

/* --- Soft-particle depth binding (smoke) ------------------------------------
 * Binds the scene depth as PS resource t1 and swaps the bound depth target to
 * the READ-ONLY DSV so the smoke shader can SAMPLE scene depth for a depth-aware
 * soft fade while the hardware z-test still runs. Returns 1 if the soft-particle
 * resources exist (the caller passes that as the smoke shader's soft flag and
 * MUST call td5_plat_fx_soft_end() after the draw to restore the writable depth
 * target). Returns 0 (soft disabled) if the backend couldn't create them. */
static ID3D11RenderTargetView *s_soft_saved_rtv = NULL;
static ID3D11DepthStencilView *s_soft_saved_dsv = NULL;
static int                     s_soft_active     = 0;

int td5_plat_fx_soft_begin(void)
{
    ID3D11DeviceContext *ctx = g_backend.context;
    if (!ctx || !g_backend.depth_srv || !g_backend.depth_dsv_readonly) return 0;

    /* Save the currently bound RTV + DSV (AddRef'd) so we can restore them. */
    s_soft_saved_rtv = NULL;
    s_soft_saved_dsv = NULL;
    ID3D11DeviceContext_OMGetRenderTargets(ctx, 1, &s_soft_saved_rtv, &s_soft_saved_dsv);

    /* Same colour target, but the READ-ONLY depth view (so depth can also be an
     * SRV), plus the depth SRV at t1 for the smoke shader. */
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &s_soft_saved_rtv, g_backend.depth_dsv_readonly);
    ID3D11DeviceContext_PSSetShaderResources(ctx, 1, 1, &g_backend.depth_srv);
    s_soft_active = 1;
    return 1;
}

void td5_plat_fx_soft_end(void)
{
    ID3D11DeviceContext *ctx = g_backend.context;
    if (!ctx || !s_soft_active) return;
    s_soft_active = 0;

    /* Unbind t1 (avoids a read/write hazard when the depth is next a writable
     * target), then restore the original RTV + writable DSV. */
    ID3D11ShaderResourceView *null_srv = NULL;
    ID3D11DeviceContext_PSSetShaderResources(ctx, 1, 1, &null_srv);
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &s_soft_saved_rtv, s_soft_saved_dsv);

    if (s_soft_saved_rtv) { ID3D11RenderTargetView_Release(s_soft_saved_rtv); s_soft_saved_rtv = NULL; }
    if (s_soft_saved_dsv) { ID3D11DepthStencilView_Release(s_soft_saved_dsv); s_soft_saved_dsv = NULL; }
}

void td5_plat_render_draw_tris(const TD5_D3DVertex *verts, int vertex_count,
                                const uint16_t *indices, int index_count)
{
    /* [Phase B render-transform] If this thread is building a pane command list,
     * record the draw (copy geometry) instead of issuing it; replayed live later. */
    if (td5_rcmd_recording()) {
        td5_rcmd_draw_tris(verts, vertex_count, indices, index_count);
        return;
    }
    /* [Phase B Stage 2] route to the thread-local pane bundle when recording. */
    WrapperRecCtx *rc = g_wrapper_rec;
    ID3D11DeviceContext *ctx = rc ? rc->dc : g_backend.context;
    ID3D11Buffer *vb    = rc ? rc->vb : g_backend.dynamic_vb;
    ID3D11Buffer *ib    = rc ? rc->ib : g_backend.dynamic_ib;
    ID3D11Buffer *cbvp  = rc ? rc->cb_viewport : g_backend.cb_viewport;
    ID3D11Buffer *cbfog = rc ? rc->cb_fog : g_backend.cb_fog;
    RenderStateCache *st = rc ? &rc->state : &g_backend.state;
    UINT stride = TD5_VERTEX_STRIDE; /* 32 bytes */
    UINT offset = 0;
    UINT base_vertex = 0, start_index = 0;
    int  has_idx;

    if (!ctx || !verts || vertex_count <= 0) return;

    has_idx = (indices && index_count > 0);

    s_frame_draw_calls++;
    s_frame_vertices += vertex_count;
    if (has_idx)
        s_frame_indices += index_count;

    /* [2026-06-08 streaming-ring] Append verts (+ indices) to the dynamic ring
     * with WRITE_NO_OVERWRITE (DISCARD only on wrap) instead of a per-draw
     * WRITE_DISCARD — the latter serialized the CPU on the GPU once the per-frame
     * draw count climbed (split-screen). base_vertex/start_index address the
     * appended slice; the VB/IB stay bound at byte offset 0. */
    if (!Backend_StreamUpload(verts, (UINT)vertex_count, stride,
                              has_idx ? (const void *)indices : NULL,
                              has_idx ? (UINT)index_count : 0,
                              &base_vertex, &start_index))
        return;

    /* Bind VB, input layout, shaders */
    ID3D11DeviceContext_IASetVertexBuffers(ctx, 0, 1, &vb,
                                           &stride, &offset);
    ID3D11DeviceContext_IASetInputLayout(ctx, g_backend.input_layout);
    ID3D11DeviceContext_VSSetShader(ctx, g_backend.vs_pretransformed, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(ctx, 0, 1, &cbvp);
    ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 1, &cbfog);

    /* Apply render state cache -> D3D11 state objects */
    Backend_ApplyStateCache();

    if (has_idx) {
        /* Indexed draw */
        ID3D11DeviceContext_IASetIndexBuffer(ctx, ib,
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
                g_backend.ps_shaders[st->texblend_mode == 5 ? 1 : 0], NULL, 0);
            {
                int si = (st->mag_filter >= 2) ? SAMP_LINEAR_WRAP : SAMP_POINT_WRAP;
                ID3D11DeviceContext_PSSetSamplers(ctx, 0, 1, &g_backend.sampler_states[si]);
            }
        }
        ID3D11DeviceContext_DrawIndexed(ctx, (UINT)index_count, start_index, (INT)base_vertex);
    } else {
        /* Non-indexed draw */
        ID3D11DeviceContext_IASetPrimitiveTopology(ctx,
            D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D11DeviceContext_Draw(ctx, (UINT)vertex_count, base_vertex);
    }
}

void td5_plat_render_draw_lines(const TD5_D3DVertex *verts, int vert_count)
{
    if (td5_rcmd_recording()) { td5_rcmd_draw_lines(verts, vert_count); return; }
    WrapperRecCtx *rc = g_wrapper_rec;
    ID3D11DeviceContext *ctx = rc ? rc->dc : g_backend.context;
    ID3D11Buffer *vb    = rc ? rc->vb : g_backend.dynamic_vb;
    ID3D11Buffer *cbvp  = rc ? rc->cb_viewport : g_backend.cb_viewport;
    ID3D11Buffer *cbfog = rc ? rc->cb_fog : g_backend.cb_fog;
    RenderStateCache *st = rc ? &rc->state : &g_backend.state;
    UINT stride = TD5_VERTEX_STRIDE;
    UINT offset = 0;
    UINT base_vertex = 0;

    if (!ctx || !verts || vert_count < 2) return;
    if (!g_backend.white_srv) return;

    /* [2026-06-08 streaming-ring] Append to the dynamic ring (DISCARD only on
     * wrap) instead of a per-call WRITE_DISCARD; draw from base_vertex. */
    if (!Backend_StreamUpload(verts, (UINT)vert_count, stride, NULL, 0,
                              &base_vertex, NULL))
        return;

    ID3D11DeviceContext_IASetVertexBuffers(ctx, 0, 1, &vb,
                                           &stride, &offset);
    ID3D11DeviceContext_IASetInputLayout(ctx, g_backend.input_layout);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx,
        D3D11_PRIMITIVE_TOPOLOGY_LINELIST);

    ID3D11DeviceContext_VSSetShader(ctx, g_backend.vs_pretransformed, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(ctx, 0, 1, &cbvp);

    /* PS_MODULATE * white texel = vertex color. Disable fog and alpha test
     * via a fresh fog CB upload bounded to this draw. */
    ID3D11DeviceContext_PSSetShader(ctx, g_backend.ps_shaders[PS_MODULATE], NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &g_backend.white_srv);
    ID3D11DeviceContext_PSSetSamplers(ctx, 0, 1, &g_backend.sampler_states[SAMP_POINT_CLAMP]);
    ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 1, &cbfog);
    {
        FogCB fog = {0};
        fog.fogEnabled = 0;
        fog.alphaTestEnabled = 0;
        fog.alphaRef = 0.0f;
        ID3D11DeviceContext_UpdateSubresource(ctx,
            (ID3D11Resource*)cbfog, 0, NULL, &fog, 0, 0);
    }

    ID3D11DeviceContext_RSSetState(ctx, g_backend.rs_state);
    /* Z test on (so lines occlude correctly), Z write off (don't poison depth
     * for subsequent overlays), no blend. */
    ID3D11DeviceContext_OMSetDepthStencilState(ctx,
        g_backend.ds_states[DS_Z_ON_WRITE_OFF], 0);
    ID3D11DeviceContext_OMSetBlendState(ctx,
        g_backend.blend_states[BLEND_OPAQUE], NULL, 0xFFFFFFFF);

    ID3D11DeviceContext_Draw(ctx, (UINT)vert_count, base_vertex);

    /* Invalidate cached state-object indices so the next ApplyStateCache
     * actually re-binds (current_* values must mismatch what we just set). */
    st->current_blend_idx = -1;
    st->current_ds_idx    = -1;
    st->current_samp_idx  = -1;
    st->current_ps_idx    = -1;
    st->dirty = 1;
    /* Force texture rebind on next draw — current_srv was overwritten with white. */
    if (rc) rc->current_srv = NULL; else g_backend.current_srv = NULL;
    if (!rc) s_last_bound_texture_page = -1;
}

void td5_plat_render_set_preset(TD5_RenderPreset preset)
{
    if (td5_rcmd_recording()) { td5_rcmd_set_preset((int)preset); return; }
    WrapperRecCtx *rc = g_wrapper_rec;
    RenderStateCache *s = rc ? &rc->state : &g_backend.state;

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
        /* [S22 fix #1: tree/sprite black-border fringe] This is the type-2
         * alpha-blended billboard preset (trees, fences, foliage, signs). The
         * world indexed draw path (td5_plat_render_draw_tris) samples these
         * 1-bit-alpha (A1R5G5B5) cutout pages with BILINEAR (mag_filter=2), so
         * the edge taps between an opaque tree texel and a transparent BLACK
         * border texel produce intermediate alphas in (0,255) with darkened
         * RGB — the visible "black line" around tree borders. alpha_ref was 1
         * (discard only alpha<1/255), which let every fringe tap survive.
         * Raise to 0x80, exactly as TD5_PRESET_TRANSLUCENT_LINEAR already does
         * ("Discarding < 128 prunes the dark/black fringes around transparent
         * pixels"): the sub-50%-alpha fringe taps are alpha-tested away while
         * genuine type-2 semi-transparent surfaces sit at exactly 0x80 and the
         * strict `< alphaRef` test (ps_common.hlsli) keeps them. Solid opaque
         * geometry is alpha=255 throughout and is unaffected. */
        s->alpha_ref         = 0x80;
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
        /* Shadow mesh + tire-mark ground decals. DEPTH TEST RE-ENABLED
         * [FIX 2026-06-11: other cars' shadows painted over geometry].
         *
         * History: the LEGACY shadow was a FLAT quad through the 4 wheel-contact
         * probes projected through a SEPARATE transform from the road, so its
         * depth diverged from the curved road surface across the quad — the
         * LEQUAL test lost on bumpy-road pixels and the shadow shimmered; no
         * bias cured it, so the test was disabled (z_enable=0) and over-car
         * relied on the shadow PRE-PASS body-overwrite. The z-off trade-off
         * ("a wall/prop between the camera and the shadow no longer occludes
         * it") turned glaring with the 2026-06-10 conforming raycast mesh:
         * other cars' shadows visibly paint over kerbs/walls/crests. It also
         * silently regressed the tire-mark through-walls fix (td5_vfx.c
         * submits marks via this preset EXPECTING a LEQUAL test).
         *
         * The conforming mesh removes the shimmer blocker: its node heights
         * come from the SAME barycentric ground solve the road renders from,
         * and both write depth as (vz - NEAR_DEPTH_OFFSET)*DEPTH_NORMALIZE_INV
         * (td5_render.c:1050 vs :5394), so shadow/road depths now tie to FP
         * error and the existing SHADOW_DEPTH_Z_BIAS 2-view-z toward-camera
         * pull wins LEQUAL robustly. Tire marks carry their own TIRE_DECAL_BIAS.
         * The legacy quad (TD5RE_SHADOW_RAYCAST=0 debug A/B only) may shimmer
         * again under the test — accepted for a debug fallback.
         *
         * z_write stays OFF (shared with tire marks; overlapping decals must
         * not z-fight each other). The shadow PRE-PASS stays — bodies drawn
         * after all shadows still unconditionally overwrite over-car pixels. */
        s->blend_enable = 1;
        s->src_blend    = D3D6BLEND_SRCALPHA;
        s->dest_blend   = D3D6BLEND_INVSRCALPHA;
        s->z_enable     = 1;  /* LEQUAL — walls/crests/kerbs occlude ground decals again */
        s->z_write      = 0;
        s->z_func       = 0;  /* LEQUAL */
        s->polygon_offset    = 0;  /* vertex-level view-z pulls handle the tie instead */
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

    case TD5_PRESET_VEHICLE_FADE:
        /* [dynamic-traffic] Whole-car fade for GTA-style traffic spawn/despawn.
         * SRCALPHA/INVSRCALPHA with the fade in the per-vertex diffuse alpha
         * (MODULATEALPHA: texture * diffuse incl. alpha). z_write stays ON —
         * unlike the shared translucent presets — so the mesh's own front
         * faces still occlude its back faces mid-fade; without it a fading
         * car renders inside-out. alpha_ref=1, NOT TRANSLUCENT_ANISO's 0x80:
         * the fade routinely sits below 50% alpha and a 0x80 cutoff would
         * alpha-test the whole car away for the first half of the fade-in. */
        s->blend_enable      = 1;
        s->src_blend         = D3D6BLEND_SRCALPHA;
        s->dest_blend        = D3D6BLEND_INVSRCALPHA;
        s->z_enable          = 1;
        s->z_write           = 1;
        s->z_func            = 0;  /* LEQUAL */
        s->mag_filter        = 2;  /* LINEAR — same as the opaque body preset */
        s->min_filter        = 2;
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

    if (rc) rc->alpha_blend_enabled = s->blend_enable;
    else    g_backend.alpha_blend_enabled = s->blend_enable;
    s->dirty = 1;
}

void td5_plat_render_bind_texture(int page_index)
{
    if (td5_rcmd_recording()) { td5_rcmd_bind_texture(page_index); return; }
    WrapperRecCtx *rc = g_wrapper_rec;
    ID3D11DeviceContext *ctx = rc ? rc->dc : g_backend.context;
    ID3D11ShaderResourceView *srv = NULL;

    if (page_index >= 0 && page_index < MAX_TEXTURE_PAGES) {
        /* Use the texture handle (SRV pointer) if available */
        if (s_tex_handles[page_index]) {
            srv = (ID3D11ShaderResourceView *)(DWORD_PTR)s_tex_handles[page_index];
        } else if (s_tex_surfaces[page_index] && s_tex_surfaces[page_index]->d3d11_srv) {
            srv = s_tex_surfaces[page_index]->d3d11_srv;
        }
    }

    if (rc) rc->current_srv = srv;
    else    g_backend.current_srv = srv;
    if (!rc) s_last_bound_texture_page = page_index;  /* immediate-path hint only */
    if (ctx && srv) {
        ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &srv);
    } else if (ctx) {
        ID3D11ShaderResourceView *null_srv = NULL;
        ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &null_srv);
    }

    if (rc) rc->state.dirty = 1;
    else    g_backend.state.dirty = 1;
}

void td5_plat_render_set_fog(int enable, uint32_t color,
                              float start, float end, float density)
{
    if (td5_rcmd_recording()) { td5_rcmd_set_fog(enable, color, start, end, density); return; }
    WrapperRecCtx *rc = g_wrapper_rec;
    RenderStateCache *s = rc ? &rc->state : &g_backend.state;

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
    if (td5_rcmd_recording()) { td5_rcmd_set_viewport(x, y, width, height); return; }
    WrapperRecCtx *wrc = g_wrapper_rec;
    ID3D11DeviceContext *ctx = wrc ? wrc->dc : g_backend.context;
    D3D11_VIEWPORT vp;

    if (!ctx) return;

    vp.TopLeftX = (FLOAT)x;
    vp.TopLeftY = (FLOAT)y;
    vp.Width    = (FLOAT)width;
    vp.Height   = (FLOAT)height;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;

    ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
    Backend_UpdateViewportCB((float)width, (float)height);
    /* INFO log only on the main thread (logger is not thread-safe). */
    if (!wrc) TD5_LOG_I(LOG_TAG, "Viewport set: x=%d y=%d w=%d h=%d", x, y, width, height);
}

void td5_plat_render_set_clip_rect(int left, int top, int right, int bottom)
{
    if (td5_rcmd_recording()) { td5_rcmd_set_clip(left, top, right, bottom); return; }
    WrapperRecCtx *wrc = g_wrapper_rec;
    ID3D11DeviceContext *ctx = wrc ? wrc->dc : g_backend.context;
    D3D11_RECT rc;

    if (!ctx) return;

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
    ID3D11DeviceContext_RSSetScissorRects(ctx, 1, &rc);
}

/* [Phase B Stage 2b] Thin platform wrappers over the wrapper's deferred-context
 * pane-record API, so td5_game.c can drive threaded pane recording without
 * pulling D3D11 types in. The opaque void* carries the WrapperRecCtx handle. */
int td5_plat_render_pane_pool_ensure(int count)
{
    return Backend_RecPoolEnsure(count);
}
void *td5_plat_render_pane_begin(int index, int x, int y, int w, int h)
{
    return (void *)Backend_RecBegin(index, x, y, w, h);
}
void td5_plat_render_pane_end(void *rc)
{
    Backend_RecEnd((WrapperRecCtx *)rc);
}
void td5_plat_render_pane_execute(int index)
{
    Backend_RecExecute(index);
}
void td5_plat_render_restore_main_rt(void)
{
    Backend_RestoreMainRenderTarget();
}

void td5_plat_render_clear(uint32_t color)
{
    float rgba[4];

    if (!g_backend.context) return;

    /* Extract ARGB components to float RGBA */
    td5_argb_to_rgb_f(rgba, color);
    rgba[3] = 1.0f;

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
