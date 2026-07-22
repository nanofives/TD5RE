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
#include "td5_platform_internal.h"
#include "td5_config.h"  /* shared TD5RE_* env-knob helpers */
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

/* Window */
HWND     s_hwnd          = NULL;


/* Input */
static uint8_t  s_keyboard[256];

/* [INPUTSCRIPT 2026-07-03] Scripted-key overlay (td5_inputscript.c dev harness
 * + StartScreen nav walker). Merged into s_keyboard right after every hardware
 * fill — including the unfocused path — so injected keys read as physical
 * presses everywhere (frontend nav held-state, pause menu, dev keys, the
 * rebindable race key map) even when the window is in the background.
 * s_inject_active is a cheap gate so the merge loop costs nothing when the
 * harness is idle. */
static uint8_t  s_inject_keys[256];
static int      s_inject_active = 0;

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
static int      s_mouse_wheel;   /* accumulated DI wheel (lZ) delta; read+cleared by getter */
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
 * Control-Options remap overrides this.
 *
 * [CAMERA -> Y 2026-06-27] CHANGE VIEW defaults to Y (button 3) — the natural,
 * discoverable camera button — and HORN/SIREN takes the L3 / left-stick-click
 * slot (button 8) that CHANGE VIEW vacated. SELECT / Back (button 6) stays
 * dedicated to reset-car / stuck-recovery (td5_input.c). Net per-pad in-race
 * layout: Y changes view, L3 honks, R3 is rear view, SELECT resets the car,
 * Start pauses. (Prior layout had CHANGE VIEW on L3 and HORN on Y, which players
 * found unintuitive — see the recovery note in td5_input.c.) */
static const uint32_t k_default_js_action_bind[TD5_JSBIND_ACTIONS] = {
    TD5_JSBIND_AXIS   | (0u << 1) | 1u,  /*  0 LEFT        left stick X (-)          */
    TD5_JSBIND_AXIS   | (0u << 1) | 0u,  /*  1 RIGHT       left stick X (+)          */
    TD5_JSBIND_AXIS   | (2u << 1) | 1u,  /*  2 ACCELERATE  R2 / RT  (Z below centre) */
    TD5_JSBIND_AXIS   | (2u << 1) | 0u,  /*  3 BRAKE       L2 / LT  (Z above centre) */
    TD5_JSBIND_BUTTON | 2u,              /*  4 HANDBRAKE   X                         */
    TD5_JSBIND_BUTTON | 8u,              /*  5 HORN/SIREN  L3 / left stick click      */
    TD5_JSBIND_BUTTON | 5u,              /*  6 GEAR UP     R1 / RB                    */
    TD5_JSBIND_BUTTON | 4u,              /*  7 GEAR DOWN   L1 / LB                    */
    TD5_JSBIND_BUTTON | 3u,              /*  8 CHANGE VIEW Y                          */
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
/* [split-screen disconnect slot-restore 2026-06-24] GUID-STABLE enumeration index.
 * The original mapped player->device by a STABLE persisted source index (0-7) into a
 * FIXED device table [CONFIRMED @ ScreenControlOptions 0x0041DF20 reads
 * (&g_player1DeviceDesc)[g_player1InputSource]; per-frame device = inputSource-1 @
 * 0x0042C470], so a device's slot never moved when ANOTHER device dropped (the 1999
 * binary had no hot-plug at all — M2DX owned the devices). The port re-enumerated
 * attached-only (DIEDFL_ATTACHEDONLY) and rebuilt the list from scratch, so a
 * disconnect COMPACTED the array and shifted every later device DOWN one index ->
 * every binding keyed by that 1-based index (s_mp_join_device[p], persisted [Devices]
 * PlayerN, the FF controller_assignment, the mp_session device->profile/slot map)
 * silently re-pointed at a DIFFERENT physical pad, swapping split-screen players'
 * slots. We now PIN each index to its DirectInput INSTANCE GUID: a slot, once
 * assigned, keeps its index for the whole session; a disconnected device's slot is
 * RESERVED (present=0) rather than compacted away or reused, so its owner keeps the
 * slot and reclaims the same index when the pad reconnects. */
static int  s_device_present[16]  = { 0 };  /* 1 = attached this enumeration; 0 = reserved/absent */
static int  s_device_assigned[16] = { 0 };  /* 1 = slot owns a known instance GUID (reserved across hot-plug) */

/* GUID byte-compare (GUID is 16 contiguous bytes, no padding) — avoids depending
 * on the IsEqualGUID macro/ole32 linkage. */
static int td5_guid_equal(const GUID *a, const GUID *b)
{
    return memcmp(a, b, sizeof(GUID)) == 0;
}

/* The stable index that already owns this physical device's instance GUID, or -1.
 * A returning (reconnected) pad reclaims its original slot through this lookup. */
static int td5_plat_input_slot_for_instance_guid(const GUID *g)
{
    int i;
    for (i = 1; i < 16; i++)
        if (s_device_assigned[i] && td5_guid_equal(&s_device_guids[i], g))
            return i;
    return -1;
}


/* Audio */
/* Per-device force-feedback state. [PORT ENHANCEMENT 2026-06] grown from a
 * single device to one entry per joystick slot so each player's wheel/pad can
 * vibrate independently. Devices without FF leave their entry zeroed (no-op). */
static TD5_FFState          s_ff[TD5_PLAT_MAX_JS_SLOTS];

/* Rendering: texture pages */
#define MAX_TEXTURE_PAGES 1024
#define TD5_SHARED_FONT_PAGE 898
WrapperSurface  *s_tex_surfaces[MAX_TEXTURE_PAGES];
static WrapperTexture  *s_tex_wrappers[MAX_TEXTURE_PAGES];
DWORD            s_tex_handles[MAX_TEXTURE_PAGES];
static uint16_t         s_tex_widths[MAX_TEXTURE_PAGES];
static uint16_t         s_tex_heights[MAX_TEXTURE_PAGES];
int              s_frame_draw_calls = 0;
int              s_frame_vertices = 0;
int              s_frame_indices = 0;
int              s_last_bound_texture_page = -1;

/* Heap */
HANDLE s_game_heap = NULL;

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
HICON td5_load_app_icon(int cx, int cy)
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

/* Give the process an explicit AppUserModelID so the Windows taskbar groups
 * our window under its OWN button and renders THAT window's icon, instead of
 * borrowing a cached icon from the host/launcher process. Without this, fresh
 * machines (or a launcher with no AUMID) can show the generic default icon on
 * the taskbar even though the window-class / WM_SETICON icon is correct -- this
 * is the classic "icon works on the dev box but not on other PCs" symptom.
 *
 * Declared inline (mirrors the DwmSetWindowAttribute / XInput precedent) to
 * avoid pulling the heavy <shobjidl.h> COM header under WIN32_LEAN_AND_MEAN;
 * the symbol resolves at link time from shell32 (-lshell32). Best-effort: any
 * failure (older OS, missing export) is silently ignored.
 *
 * MUST be called BEFORE the app presents any UI (before the display window is
 * created / shown). Windows assigns the taskbar button its grouping identity
 * when the button is first created; setting the AUMID afterwards leaves the
 * existing button under the process-default identity and it never re-homes to
 * our window's icon. Set too late, a freshly-installed PC (no cached icon
 * association) shows the generic default icon -- the classic "icon works on the
 * dev box but is blank on another computer" symptom. main.c:WinMain therefore
 * calls this at the very top, before Backend_CreateDevice() builds the window.
 * Idempotent (guarded) so the later platform-init path is a harmless no-op. */
void td5_platform_win32_set_app_id(void)
{
    static int s_app_id_set = 0;
    typedef HRESULT (WINAPI *PFN_SetCurrentProcessExplicitAppUserModelID)(PCWSTR);
    HMODULE hShell;
    if (s_app_id_set)
        return;
    s_app_id_set = 1;
    hShell = LoadLibraryA("shell32.dll");
    if (hShell) {
        PFN_SetCurrentProcessExplicitAppUserModelID pSet =
            (PFN_SetCurrentProcessExplicitAppUserModelID)
            GetProcAddress(hShell, "SetCurrentProcessExplicitAppUserModelID");
        if (pSet)
            pSet(L"TD5RE.SourcePort");
        /* keep shell32 loaded for the process lifetime; do not FreeLibrary */
    }
}

/* Declare this process DPI-AWARE before any window, HMONITOR, or HDC exists.
 *
 * Symptom this fixes: on a machine whose display scaling is set above 100%
 * (e.g. a 4K laptop panel defaulting to 300%), a DPI-UNAWARE process is
 * silently virtualized by Windows -- it renders into a logical-resolution
 * back buffer (physical / scale) and the desktop compositor then BITMAP-
 * STRETCHES that buffer up to the physical pixels. The result is a blurry /
 * "pixelated" image that resizing the window cannot fix, because the stretch
 * happens after the app has already drawn. On a fresh PC the game looked
 * crisp on the dev box (100% scaling) but pixelated on the user's 4K/300%
 * panel -- the classic DPI-virtualization tell.
 *
 * Declaring awareness makes GetSystemMetrics / the swap-chain size report
 * TRUE physical pixels, so our D3D11 back buffer (sized from td5re.ini
 * Display Width/Height) maps 1:1 to the screen with no compositor stretch.
 *
 * Tried newest-first via LoadLibrary+GetProcAddress (mirrors the
 * SetCurrentProcessExplicitAppUserModelID / DwmSetWindowAttribute / XInput
 * precedent) so we neither hard-import shcore.dll nor need a newer SDK
 * header than the bundled MinGW provides:
 *   1. SetProcessDpiAwarenessContext(PER_MONITOR_AWARE_V2)  -- Win10 1703+
 *   2. SetProcessDpiAwareness(PROCESS_PER_MONITOR_DPI_AWARE) -- Win8.1+
 *   3. SetProcessDPIAware()                                  -- Vista+
 * Best-effort: if none resolve (pre-Vista) the process stays unaware, which
 * is the pre-fix behaviour. MUST run at the very top of WinMain, before
 * td5_platform_win32_set_app_id() / Backend_CreateDevice() create the window.
 * Idempotent (guarded) so any later call is a harmless no-op. */
void td5_platform_win32_enable_dpi_awareness(void)
{
    static int s_dpi_set = 0;
    /* DPI_AWARENESS_CONTEXT is an opaque HANDLE; PER_MONITOR_AWARE_V2 == -4. */
    typedef BOOL (WINAPI *PFN_SetProcessDpiAwarenessContext)(HANDLE);
    typedef HRESULT (WINAPI *PFN_SetProcessDpiAwareness)(int);
    typedef BOOL (WINAPI *PFN_SetProcessDPIAware)(void);
    HMODULE hUser32, hShcore;

    if (s_dpi_set)
        return;
    s_dpi_set = 1;

    /* 1. Per-Monitor-V2 (best: crisp + correct non-client scaling), Win10 1703+ */
    hUser32 = GetModuleHandleA("user32.dll");
    if (hUser32) {
        PFN_SetProcessDpiAwarenessContext pCtx =
            (PFN_SetProcessDpiAwarenessContext)(void *)
            GetProcAddress(hUser32, "SetProcessDpiAwarenessContext");
        if (pCtx && pCtx((HANDLE)(INT_PTR)-4)) {   /* PER_MONITOR_AWARE_V2 */
            TD5_LOG_I(LOG_TAG, "DPI: per-monitor-v2 awareness enabled");
            return;
        }
    }

    /* 2. Per-Monitor (Win8.1+) via shcore.dll */
    hShcore = LoadLibraryA("shcore.dll");
    if (hShcore) {
        PFN_SetProcessDpiAwareness pAwr =
            (PFN_SetProcessDpiAwareness)(void *)
            GetProcAddress(hShcore, "SetProcessDpiAwareness");
        if (pAwr && pAwr(2) == 0) {   /* PROCESS_PER_MONITOR_DPI_AWARE, S_OK */
            TD5_LOG_I(LOG_TAG, "DPI: per-monitor awareness enabled (shcore)");
            /* keep shcore loaded for the process lifetime; do not FreeLibrary */
            return;
        }
        FreeLibrary(hShcore);
    }

    /* 3. System-DPI aware (Vista+) -- last resort, still kills the stretch */
    if (hUser32) {
        PFN_SetProcessDPIAware pSys =
            (PFN_SetProcessDPIAware)(void *)
            GetProcAddress(hUser32, "SetProcessDPIAware");
        if (pSys && pSys()) {
            TD5_LOG_I(LOG_TAG, "DPI: system awareness enabled (legacy)");
            return;
        }
    }

    TD5_LOG_W(LOG_TAG, "DPI: no awareness API available -- staying DPI-unaware");
}

/* ========================================================================
 * Initialization -- called by the wrapper/loader after device creation
 * ======================================================================== */

void td5_platform_win32_init(void *ddraw4, void *d3ddevice3, void *primary_surface)
{
    s_ddraw4     = (WrapperDirectDraw *)ddraw4;
    s_d3ddevice3 = (WrapperDevice *)d3ddevice3;
    s_primary    = (WrapperSurface *)primary_surface;

    /* Taskbar identity is normally set at the very top of WinMain (before the
     * window exists). This fallback covers alternate entry flows (e.g. the
     * wrapper-DLL path) where WinMain did not run; the call is guarded so when
     * WinMain already set it this is a no-op. NOTE: by this point the window is
     * already shown, so a first-time set here is too late to re-home the taskbar
     * button -- WinMain is the path that actually fixes the blank-icon symptom. */
    td5_platform_win32_set_app_id();

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

/* Process-exit WGI rumble teardown. Defined far below near the WGI backend;
 * forward-declared here so the process-level input shutdown can release the WGI
 * activation factory + gamepad objects. The per-race FF shutdown must NOT —
 * doing so killed rumble after the first race (see td5_plat_ff_shutdown). */
static void td5_wgi_shutdown(void);

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
    /* [FF WGI 2026-06-25] Release the WGI rumble subsystem here (process exit),
     * NOT in the per-race td5_plat_ff_shutdown(). */
    td5_wgi_shutdown();
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
        /* [INPUTSCRIPT] keep scripted keys alive while unfocused so automated
         * test windows can run in the background (key_pressed readers only —
         * the button mapping below is still skipped, race actions inject at
         * the control-bits level in td5_input.c instead). */
        if (s_inject_active)
            for (int k = 0; k < 256; k++) s_keyboard[k] |= s_inject_keys[k];
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

    /* [INPUTSCRIPT] Merge the scripted-key overlay over the fresh hardware
     * snapshot — one insertion point covers every downstream reader
     * (key_pressed, the K_DOWN binding map, get_keyboard, jbits ESC/pause). */
    if (s_inject_active)
        for (int k = 0; k < 256; k++) s_keyboard[k] |= s_inject_keys[k];

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
            s_mouse_wheel += ms.lZ;   /* DI mouse Z-axis = scroll wheel (±120/notch) */
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
                if (ax_x < 0) ax_x = 0;
                if (ax_x > 0x1FF) ax_x = 0x1FF;
                if (ax_y < 0) ax_y = 0;
                if (ax_y > 0x1FF) ax_y = 0x1FF;
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
                if (ax_x < 0) ax_x = 0;
                if (ax_x > 0x1FF) ax_x = 0x1FF;
                if (ax_y < 0) ax_y = 0;
                if (ax_y > 0x1FF) ax_y = 0x1FF;
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

/* Accumulated mouse-wheel delta since the last call (then cleared). One notch is
 * ±120 (WHEEL_DELTA); positive = wheel-up/away. Sourced from the DirectInput
 * mouse Z-axis. [CHANGELOG scroll 2026-06-25] */
int td5_plat_input_get_mouse_wheel(void)
{
    int w = s_mouse_wheel;
    s_mouse_wheel = 0;
    return w;
}

int td5_plat_input_key_pressed(int scancode)
{
    if (scancode < 0 || scancode > 255) return 0;
    return (s_keyboard[scancode] & 0x80) ? 1 : 0;
}

/* [INPUTSCRIPT 2026-07-03] Scripted-key injection (dev harness + StartScreen
 * nav walker). Press/release a DIK scancode as if it were a physical key:
 *  - the overlay is merged into s_keyboard after each hardware fill (and
 *    immediately here so readers between polls see it);
 *  - arrow/Enter DOWN transitions also enqueue a WM_KEYDOWN nav event —
 *    frontend menu MOVES are drained exclusively from that FIFO, so a bare
 *    scancode overlay would never move the cursor;
 *  - ESC DOWN also sets the one-shot ESC latch (frontend back-out reads the
 *    latch, the pause menu reads the scancode — both paths covered). */
void td5_plat_input_inject_key(int scancode, int down)
{
    if (scancode < 0 || scancode > 255) return;
    if (down) {
        s_inject_keys[scancode] = 0x80;
        s_keyboard[scancode]   |= 0x80;
        s_inject_active = 1;

        {
            unsigned code = 0;
            switch (scancode) {
            case 0xCB: code = TD5_NAVKEY_LEFT;  break;
            case 0xCD: code = TD5_NAVKEY_RIGHT; break;
            case 0xC8: code = TD5_NAVKEY_UP;    break;
            case 0xD0: code = TD5_NAVKEY_DOWN;  break;
            case 0x1C: code = TD5_NAVKEY_ENTER; break;
            case 0x01: s_esc_latch = 1;         break;
            default: break;
            }
            if (code) {
                int next = (s_nav_q_head + 1) % TD5_NAV_QUEUE_SIZE;
                if (next != s_nav_q_tail) {
                    s_nav_queue[s_nav_q_head] = (unsigned char)code;
                    s_nav_q_head = next;
                }
            }
        }
    } else {
        s_inject_keys[scancode] = 0;
        s_keyboard[scancode]   &= (uint8_t)~0x80;
    }
}

void td5_plat_input_inject_clear(void)
{
    if (!s_inject_active) return;
    for (int k = 0; k < 256; k++) {
        if (s_inject_keys[k]) s_keyboard[k] &= (uint8_t)~0x80;
        s_inject_keys[k] = 0;
    }
    s_inject_active = 0;
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

/* Friendly controller-name override.
 *
 * Modern pads/wheels report long DirectInput instance names — e.g.
 * "Controller (8BitDo Ultimate 2C Wireless Controller)" — that overflow the
 * port's controller-config / multiplayer-join rows (the MP-join lobby draws the
 * name on a single un-wrapped line, so it spills off-screen). This pass maps a
 * recognised controller to a short canonical label and hard-caps anything
 * unrecognised so a name can never run past the row. Substring matching is
 * case-insensitive; the table is ordered most-specific-first so e.g. the
 * "xbox 360" entry wins over the generic "xbox" one.
 *
 * Port-only: the original delegated input to M2DX and never displayed these
 * strings, so there is no RE basis — this is a display-side enhancement that
 * sits alongside the device blocklist above. Edit the table to add models. */
#define TD5_FRIENDLY_NAME_MAX 20   /* visible chars before "..." truncation */

struct td5_friendly_name { const char *needle; const char *label; };

static const struct td5_friendly_name s_friendly_names[] = {
    /* 8BitDo (the user's primary pad). Identical units report the same name, so
     * the disambiguation pass appends " #1"/" #2" AFTER this shortening. */
    { "ultimate 2c",         "8BitDo Ultimate 2C" },
    { "8bitdo ultimate",     "8BitDo Ultimate"    },
    { "8bitdo pro 2",        "8BitDo Pro 2"       },
    { "8bitdo sn30",         "8BitDo SN30"        },
    { "8bitdo",              "8BitDo Pad"         },
    /* PlayStation. Name alone can't tell a raw-HID DS4 from a DS5 (both report
     * "Wireless Controller"); the user's PS pads are DS4 via DS4Windows, so that
     * generic string maps to DS4. "dualsense"/"ds5" still win when present. */
    { "dualsense edge",      "DualSense Edge"     },
    { "dualsense",           "DualSense"          },  /* DS5 */
    { "playstation(r)5",     "DualSense"          },
    { "dualshock 4",         "DualShock 4"        },  /* DS4 */
    { "playstation(r)4",     "DualShock 4"        },
    { "wireless controller", "DualShock 4"        },  /* DS4Windows DS4 / raw HID */
    { "dualshock 3",         "DualShock 3"        },  /* DS3 */
    { "playstation(r)3",     "DualShock 3"        },
    { "sixaxis",             "DualShock 3"        },
    /* Xbox (a wired 360 pad, plus DS4Windows' default X360 output, lands here). */
    { "xbox 360",            "Xbox 360"           },
    { "x360",                "Xbox 360"           },
    { "360 for windows",     "Xbox 360"           },
    { "xbox elite",          "Xbox Elite"         },
    { "xbox series",         "Xbox Series"        },
    { "xbox one",            "Xbox One"           },
    { "xbox",                "Xbox Pad"           },
    /* Nintendo */
    { "switch pro",          "Switch Pro"         },
    { "nintendo switch",     "Switch Pro"         },
    { "joy-con",             "Joy-Con"            },
    /* Wheels */
    { "g923",                "Logitech G923"      },
    { "g920",                "Logitech G920"      },
    { "g29",                 "Logitech G29"       },
    { "g27",                 "Logitech G27"       },
    { "g25",                 "Logitech G25"       },
    { "driving force",       "Logitech DF"        },
    { "momo",                "Logitech MOMO"      },
    { "logitech",            "Logitech"           },
    { "t300",                "Thrustmaster T300"  },
    { "t248",                "Thrustmaster T248"  },
    { "t150",                "Thrustmaster T150"  },
    { "tmx",                 "Thrustmaster TMX"   },
    { "thrustmaster",        "Thrustmaster"       },
    { "fanatec",             "Fanatec"            },
};

static void td5_plat_input_friendly_device_name(const char *raw,
                                                char *out, size_t out_sz)
{
    char low[256];
    size_t i, n;
    if (!out || out_sz == 0) return;
    if (!raw) { out[0] = '\0'; return; }

    /* Lower-case a bounded copy for case-insensitive substring matching. */
    for (i = 0; i + 1 < sizeof(low) && raw[i]; i++) {
        char c = raw[i];
        low[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    }
    low[i] = '\0';

    /* 1) Known controller -> short canonical label (most-specific first). */
    for (i = 0; i < sizeof(s_friendly_names) / sizeof(s_friendly_names[0]); i++) {
        if (strstr(low, s_friendly_names[i].needle)) {
            strncpy(out, s_friendly_names[i].label, out_sz - 1);
            out[out_sz - 1] = '\0';
            return;
        }
    }

    /* 2) Unknown: prefer the parenthesised model from a "Controller (Name)"
     *    wrapper (a common DirectInput shape) over the generic outer text. */
    {
        const char *open  = strchr(raw, '(');
        const char *close = open ? strrchr(raw, ')') : NULL;
        if (open && close && close > open + 1) {
            size_t inner = (size_t)(close - open - 1);
            if (inner > out_sz - 1) inner = out_sz - 1;
            memcpy(out, open + 1, inner);
            out[inner] = '\0';
        } else {
            strncpy(out, raw, out_sz - 1);
            out[out_sz - 1] = '\0';
        }
    }

    /* Trim trailing whitespace left by an unwrap or a sloppy instance name. */
    n = strlen(out);
    while (n > 0 && (out[n - 1] == ' ' || out[n - 1] == '\t')) out[--n] = '\0';

    /* 3) Hard length cap so an unrecognised name can never spill off-screen.
     *    Keep MAX-3 chars and append "..."; out_sz (256) always has room. */
    if (n > TD5_FRIENDLY_NAME_MAX && out_sz > (size_t)TD5_FRIENDLY_NAME_MAX) {
        size_t keep = TD5_FRIENDLY_NAME_MAX - 3;
        out[keep] = '.'; out[keep + 1] = '.'; out[keep + 2] = '.';
        out[keep + 3] = '\0';
    }
}

/* Joystick enumeration callback */
static BOOL CALLBACK EnumJoysticksCallback(
    const DIDEVICEINSTANCEA *inst, void *ctx)
{
    int idx, free_idx = -1, evict_idx = -1, i;
    (void)ctx;
    /* Override: blocked devices are skipped outright — never detected or usable. */
    if (td5_plat_input_device_name_is_blocked(inst->tszInstanceName)) {
        TD5_LOG_I(LOG_TAG, "Skipping blocked input device: \"%s\"",
                  inst->tszInstanceName);
        return DIENUM_CONTINUE;
    }
    /* [GUID-stable index 2026-06-24] Place this physical device at a STABLE slot so
     * a disconnect of some OTHER pad can't shift it (which would swap split-screen
     * players' slots — see the s_device_present/assigned note above):
     *   1. reuse the index this exact instance GUID already owns (a returning pad
     *      reclaims its original slot), else
     *   2. the lowest never-assigned slot, else
     *   3. evict the lowest currently-absent reserved slot (only when all 16 are
     *      assigned — a session would need >15 distinct pads to hit this). */
    idx = td5_plat_input_slot_for_instance_guid(&inst->guidInstance);
    if (idx < 0) {
        for (i = 1; i < 16; i++) {
            if (!s_device_assigned[i]) { free_idx = i; break; }
            if (evict_idx < 0 && !s_device_present[i]) evict_idx = i;
        }
        idx = (free_idx >= 0) ? free_idx : evict_idx;
        if (idx < 0) return DIENUM_CONTINUE;   /* all 16 slots hold present devices */
        /* Evicting a reserved slot reuses its index for a different physical device:
         * drop any cached lobby-scan handle so it recreates against the new GUID. */
        if (free_idx < 0 && s_di_scan[idx]) {
            IDirectInputDevice8_Unacquire(s_di_scan[idx]);
            IDirectInputDevice8_Release(s_di_scan[idx]);
            s_di_scan[idx] = NULL;
        }
        /* Store the instance GUID so td5_plat_input_set_device can (re)create this
         * exact device later — without it, switching to any joystick is impossible
         * (s_di_joystick was never created). */
        s_device_guids[idx]         = inst->guidInstance;
        s_device_product_guids[idx] = inst->guidProduct;
        s_device_assigned[idx]      = 1;
    }
    /* Refresh display name + type each pass (the name is re-read so the
     * disambiguation pass can re-suffix duplicates among present devices).
     * Apply the friendly-name override here so a long raw instance name is
     * shortened/capped before it reaches the controller UI. Runs every pass on
     * the STABLE slot, and before the disambiguation pass so its " #1"/" #2"
     * suffix lands on the SHORT label (two 8BitDo pads -> "8BitDo Ultimate 2C
     * #1"/"#2"). Binding is by GUID, not name, so this is display-only. */
    td5_plat_input_friendly_device_name(inst->tszInstanceName,
            s_device_names[idx], sizeof(s_device_names[0]));
    if (strcmp(inst->tszInstanceName, s_device_names[idx]) != 0)
        TD5_LOG_I(LOG_TAG, "Device name override: \"%s\" -> \"%s\"",
                  inst->tszInstanceName, s_device_names[idx]);
    {
        BYTE dev_type = GET_DIDEVICE_TYPE(inst->dwDevType);
        if (dev_type == DI8DEVTYPE_DRIVING ||
            dev_type == DI8DEVTYPE_FLIGHT  ||
            dev_type == DI8DEVTYPE_1STPERSON) {
            s_device_types[idx] = 2; /* joystick/wheel */
        } else {
            s_device_types[idx] = 1; /* gamepad */
        }
    }
    s_device_present[idx] = 1;
    if (idx >= s_device_count) s_device_count = idx + 1;
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
    /* Reserved-but-absent slots keep their preserved (possibly already-suffixed)
     * name across a disconnect and must not count toward the duplicate tally of
     * currently-attached devices — mark them done so they're neither re-suffixed
     * nor matched. */
    for (i = 1; i < s_device_count && i < 16; i++)
        if (!s_device_present[i]) numbered[i] = 1;
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
    int i;
    /* Keyboard is always device 0 and always present (reserved once). */
    if (!s_device_assigned[0]) {
        strncpy(s_device_names[0], "Keyboard", sizeof(s_device_names[0]) - 1);
        s_device_names[0][sizeof(s_device_names[0]) - 1] = '\0';
        s_device_types[0]    = 0; /* keyboard */
        s_device_assigned[0] = 1;
    }
    s_device_present[0] = 1;
    if (s_device_count < 1) s_device_count = 1;

    /* [GUID-stable index 2026-06-24] Do NOT rebuild the table from scratch. Mark
     * every joystick slot absent; the EnumDevices pass re-marks the attached ones
     * present at their GUID-pinned index. Reserved-but-absent slots KEEP their
     * GUID/name/type so a disconnected pad's index never moves and its owner keeps
     * the slot until it reconnects. */
    for (i = 1; i < 16; i++) s_device_present[i] = 0;

    if (s_dinput) {
        IDirectInput8_EnumDevices(s_dinput,
            DI8DEVCLASS_GAMECTRL,
            EnumJoysticksCallback,
            NULL,
            DIEDFL_ATTACHEDONLY);
    }

    /* Drop the cached lobby-scan handle for any slot that just went absent so a
     * later reconnect recreates a fresh, acquirable handle (a dead DI device
     * object never recovers via re-Acquire on the stale handle). */
    for (i = 1; i < s_device_count && i < 16; i++) {
        if (!s_device_present[i] && s_di_scan[i]) {
            IDirectInputDevice8_Unacquire(s_di_scan[i]);
            IDirectInputDevice8_Release(s_di_scan[i]);
            s_di_scan[i] = NULL;
        }
    }

    /* Append " #1"/" #2"/... to any controllers that share an identical name
     * so two of the same model can be told apart in the config UI. */
    td5_plat_input_disambiguate_device_names();
    TD5_LOG_I(LOG_TAG, "Input devices enumerated: count=%d primary=%s",
              s_device_count,
              (s_device_count > 0) ? s_device_names[0] : "<none>");
    for (i = 1; i < s_device_count && i < 16; i++) {
        /* Log the full DirectInput instance + product GUID per joystick. Two
         * physically-distinct controllers normally get distinct INSTANCE GUIDs;
         * some pads (notably 8BitDo) report a fixed/identical instance GUID for
         * every unit, which the binding layer (CreateDevice-by-GUID) cannot tell
         * apart — so two such pads collapse onto one bindable device. The log
         * makes that collision visible. present=0 = reserved across a disconnect. */
        const GUID *ig = &s_device_guids[i];
        const GUID *pg = &s_device_product_guids[i];
        TD5_LOG_I(LOG_TAG,
                  "  device[%d] type=%d present=%d name=\"%s\"",
                  i, s_device_types[i], s_device_present[i], s_device_names[i]);
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

/* [disconnect-modal 2026-06-21] Persistent-loss query for an ENUMERATED device
 * (1-based index into the scan handles, as stored in s_mp_join_device). The MP
 * setup flow polls pads through the non-exclusive scan handles (scan_dev /
 * td5_plat_input_device_nav), NOT the in-race exclusive per-player slots that
 * s_js_lost tracks — so this maintains its own streak/latch over the scan path.
 * It ACTIVELY polls here (rather than reading a latch updated elsewhere) so that
 * a caller spinning on it while the setup is frozen also detects reconnection:
 * a successful GetDeviceState clears the latch, mirroring the in-race re-Acquire
 * recovery. The same FAIL_THRESHOLD as the in-race detector debounces a one-
 * frame INPUTLOST from an alt-tab focus change. */
static int s_scan_fail_streak[16] = { 0 };
static int s_scan_lost[16]        = { 0 };
static LPDIRECTINPUTDEVICE8A scan_dev(int i);   /* defined below (lobby scan helper) */
int td5_plat_input_device_is_lost(int enum_index)
{
    LPDIRECTINPUTDEVICE8A dev;
    DIJOYSTATE2 js;
    if (enum_index <= 0 || enum_index >= s_device_count || enum_index >= 16) return 0;
    /* [focus 2026-06-23] While the window is unfocused (alt-tab) DirectInput
     * cannot read the pad — GetDeviceState fails every frame — which would trip
     * the lost-latch and pop "CONTROLLER DISCONNECTED" on a mere focus change.
     * Hold the current latch and skip the fail-streak while unfocused; a real
     * unplug is still detected on the first poll after focus returns. (The
     * in-race detector is already focus-safe: its poll is skipped when unfocused,
     * so s_js_lost never moves — this aligns the frontend scan path with it.) */
    if (s_hwnd && GetForegroundWindow() != s_hwnd)
        return s_scan_lost[enum_index];
    dev = scan_dev(enum_index);   /* (re)creates + acquires the handle if possible */
    if (!dev) {
        if (++s_scan_fail_streak[enum_index] >= TD5_JS_LOST_FAIL_THRESHOLD)
            s_scan_lost[enum_index] = 1;
        return s_scan_lost[enum_index];
    }
    if (FAILED(IDirectInputDevice8_Poll(dev))) {
        IDirectInputDevice8_Acquire(dev);   /* re-Acquire on transient INPUTLOST / replug */
        IDirectInputDevice8_Poll(dev);
    }
    if (FAILED(IDirectInputDevice8_GetDeviceState(dev, sizeof(js), &js))) {
        if (++s_scan_fail_streak[enum_index] >= TD5_JS_LOST_FAIL_THRESHOLD)
            s_scan_lost[enum_index] = 1;
    } else {
        s_scan_fail_streak[enum_index] = 0;
        s_scan_lost[enum_index]        = 0;   /* good read => present (cleared on reconnect) */
    }
    return s_scan_lost[enum_index];
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

/* [stuck-axis nav fix v2 2026-06-25] Last time (ms) each axis was seen AT REST.
 * The s_js_*_live gate above is sticky-ON: it only rejects a pad whose axis
 * NEVER passes through rest. A pad that centres once (so it gets marked live)
 * and THEN settles to an off-centre resting position (drift / worn or
 * uncalibrated stick) holds its direction FOREVER. Because the frontend menu
 * reader OR-aggregates every connected pad (td5_plat_input_frontend_nav), that
 * single pinned bit kills the consumer's RISING EDGE for that direction across
 * ALL pads — so with several joysticks connected, "UP and LEFT stop working" the
 * moment one biased pad rests toward up-left (DOWN/RIGHT keep working because no
 * pad happens to rest down-right). Tracking the last-at-rest time lets a held
 * axis drop back out of the nav OR after TD5_PLAT_JS_NAV_IDLE_MS, re-armed the
 * instant the axis returns to rest. Same [16] scan_dev() index space as
 * s_js_*_live (shared by the frontend menus and the car-select grid). */
static uint32_t s_js_x_rest_ms[16];
static uint32_t s_js_y_rest_ms[16];

/* Idle window (ms): an axis held off-centre this long without returning to rest
 * is treated as drift/bias and stops asserting its menu direction. Long enough
 * for a genuine press to register the consumer's rising edge across a slow frame
 * (the gamepad path is edge-only — no auto-repeat — so a real held stick already
 * moves the cursor exactly once); short enough that a stalled stick frees the
 * direction almost immediately. */
#define TD5_PLAT_JS_NAV_IDLE_MS 350u

/* Decode ONE analog axis into its menu nav bits with stuck/bias rejection.
 * `seen` is the sticky live gate (axis must have been at rest at least once);
 * `rest_ms` is the last-at-rest timestamp. Returns 0 (no direction) when the
 * axis is at rest, never been at rest, or has been held off-centre past the idle
 * window. neg_bit/pos_bit map an axis to its two directions (X -> LEFT/RIGHT,
 * Y -> UP/DOWN). Shared by every joystick menu-nav reader so the fix is uniform. */
static uint32_t td5_js_axis_nav_bits(long c, long T, uint32_t tnow,
                                     unsigned char *seen, uint32_t *rest_ms,
                                     uint32_t neg_bit, uint32_t pos_bit)
{
    if (c >= -T && c <= T) { *seen = 1; *rest_ms = tnow; return 0; }  /* at rest: trust + re-arm */
    if (!*seen) return 0;                                             /* pegged from power-on    */
    if ((uint32_t)(tnow - *rest_ms) >= TD5_PLAT_JS_NAV_IDLE_MS)
        return 0;                                                     /* held too long: drift    */
    return (c < 0) ? neg_bit : pos_bit;                              /* fresh, in-window press  */
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
    /* [GUID-stable index 2026-06-24] s_device_count no longer shrinks on a
     * disconnect (the slot is reserved), so a count comparison would MISS an
     * unplug. Compare the set of PRESENT devices instead — this flags a connect
     * (new bit) AND a disconnect (cleared bit). */
    unsigned prev_mask = 0, new_mask = 0;
    int i;
    for (i = 0; i < 16; i++) if (s_device_present[i]) prev_mask |= (1u << i);
    td5_plat_input_enumerate_devices();
    for (i = 0; i < 16; i++) if (s_device_present[i]) new_mask |= (1u << i);
    if (new_mask != prev_mask) {
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

const uint32_t *td5_plat_input_player_action_bindings(int slot)
{
    /* Mirror the in-race source selection in td5_plat_input_poll: an explicit
     * Control-Options remap (s_js_action_set) wins, else the built-in default
     * Xbox map. The rare legacy-Config.td5 raw-axis path falls through to the
     * default here — the tutorial only needs the per-action button/axis map. */
    if (slot < 0 || slot >= TD5_PLAT_MAX_JS_SLOTS) return k_default_js_action_bind;
    if (s_js_action_set[slot]) return s_js_action_bind[slot];
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

/* Defined in the gamepad-rumble block below; called from the menu/lobby input
 * scans to learn each pad's WGI gamepad from real button-press coincidences.
 * No-op when the WGI rumble path is unavailable. */
static void td5_xinput_correlate_tick(void);

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
            /* [#R3-5 reverted 2026-06-19] A scan-device deadzone caused auto-scroll
             * on hover for some pads — removed. The proper UP/LEFT fix waits on the
             * navdiag log. */
            IDirectInputDevice8_Acquire(s_di_scan[i]);
            /* [stuck-axis nav fix v2 2026-06-25] A freshly (re)created handle must
             * re-prove it is at rest before its axes can assert a menu direction.
             * The s_js_*_live flags persist across device Release (they are NOT
             * cleared there), so without this reset a handle recreated AFTER a
             * race / after entering a game mode — when the device can briefly
             * report a zeroed or off-centre state — would be trusted immediately
             * and could pin UP/LEFT. This is the stateful "works on a clean boot
             * but breaks after one race/mode" trigger. Clearing forces the
             * standard "seen at rest once" gate to re-arm for the new handle. */
            s_js_x_live[i] = 0;
            s_js_y_live[i] = 0;
        } else {
            s_di_scan[i] = NULL;
        }
    }
    return s_di_scan[i];
}

/* [SPLIT-SCREEN DEVICE BLEED 2026-06-30] Build a "physical device key" for a
 * DirectInput handle: the HID instance path (DIPROP_GUIDANDPATH) with the
 * trailing 4-hex-digit HID-collection token stripped. Two DirectInput
 * interfaces of the SAME physical controller (a pad that exposes more than one
 * game-controller HID top-level collection) get DISTINCT instance GUIDs but
 * share this key; two genuinely-distinct controllers — even identical models —
 * do NOT (their instance segments differ). This is the reliable physical-device
 * discriminator the instance GUID is not (some pads reuse a fixed instance GUID
 * across units — see the enumeration log note). Used to stop one physical pad
 * from claiming multiple split-screen player slots, which made one controller
 * drive several panes' cameras (the change-camera button flipped every slot the
 * pad was bound to). Returns 1 and fills `out` (lowercased) on success. On any
 * failure (no handle / property unsupported / empty path) returns 0 so callers
 * treat the device as distinct — i.e. the de-dup safely no-ops, never merges. */
static int dev_phys_key_from_handle(LPDIRECTINPUTDEVICE8A dev, char *out, int outsz)
{
    DIPROPGUIDANDPATH gp;
    int n = 0, i, last_amp = -1, hashbrace = -1, end;
    if (!dev || !out || outsz < 2) return 0;
    memset(&gp, 0, sizeof(gp));
    gp.diph.dwSize       = sizeof(gp);
    gp.diph.dwHeaderSize = sizeof(DIPROPHEADER);
    gp.diph.dwHow        = DIPH_DEVICE;
    gp.diph.dwObj        = 0;
    if (FAILED(IDirectInputDevice8_GetProperty(dev, DIPROP_GUIDANDPATH, &gp.diph)))
        return 0;
    /* HID paths are ASCII-safe; narrow + lowercase so the compare is canonical. */
    for (i = 0; gp.wszPath[i] && n < outsz - 1; i++) {
        char c = (char)gp.wszPath[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        out[n++] = c;
    }
    out[n] = '\0';
    if (n == 0) return 0;
    /* The class-GUID marker "#{...}" ends the meaningful part of the path. */
    for (i = 0; i + 1 < n; i++)
        if (out[i] == '#' && out[i + 1] == '{') { hashbrace = i; break; }
    end = (hashbrace >= 0) ? hashbrace : n;
    for (i = 0; i < end; i++) if (out[i] == '&') last_amp = i;
    /* Strip a trailing "&XXXX" ONLY when it is exactly 4 hex digits (the HID
     * top-level-collection index). Anything else is left intact so two distinct
     * devices can never be over-merged. */
    if (last_amp >= 0 && (end - last_amp - 1) == 4) {
        int ok = 1, k;
        for (k = last_amp + 1; k < end; k++) {
            char c = out[k];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) { ok = 0; break; }
        }
        if (ok) { out[last_amp] = '\0'; return 1; }
    }
    if (hashbrace >= 0) out[hashbrace] = '\0';   /* drop the class-GUID suffix */
    return 1;
}

/* Physical-device key for an ENUMERATED joystick index (lazily (re)creating its
 * non-exclusive scan handle). Lobby press-to-join uses this to refuse a second
 * interface of a pad already represented by a joined slot. */
int td5_plat_input_device_phys_key(int enum_index, char *out, int outsz)
{
    return dev_phys_key_from_handle(scan_dev(enum_index), out, outsz);
}

/* Physical-device key for the in-race device BOUND to a player input slot
 * (s_di_joystick[slot]) — no new handle is created. Used by the race-start
 * input-map diagnostic to surface two slots reading the same physical pad. */
int td5_plat_input_slot_phys_key(int slot, char *out, int outsz)
{
    if (slot < 0 || slot >= TD5_PLAT_MAX_JS_SLOTS || !s_di_joystick[slot]) return 0;
    return dev_phys_key_from_handle(s_di_joystick[slot], out, outsz);
}

/* Multiplayer lobby "press to join" scan: bit i set if device i's join control
 * is held this frame — device 0 = keyboard (Enter), devices 1.. = joystick
 * button 0 (A). Lazily creates a non-exclusive handle per joystick.
 * [PORT ENHANCEMENT 2026-06] */
uint32_t td5_plat_input_scan_join(void)
{
    uint32_t mask = 0;
    int i;
    /* Learn each joining pad's WGI gamepad from its A/Start press this frame —
     * the lobby join is where each player presses A on their OWN physical pad. */
    td5_xinput_correlate_tick();
    if (s_keyboard[0x1C] & 0x80) mask |= 1u;     /* keyboard Enter = device 0 */
    for (i = 1; i < s_device_count && i < 16; i++) {
        DIJOYSTATE2 js;
        /* [stuck-axis nav fix v2 2026-06-25] Create/fetch the handle through the
         * shared scan_dev() helper so it gets the SAME axis range (DIPROP_RANGE
         * 0..0x1F4, centre TD5_PLAT_JS_AXIS_CENTER) as every other reader.
         * Previously this lobby path created s_di_scan[i] WITHOUT the range; that
         * handle is then cached, so a later scan_dev() returned it un-ranged and
         * the menu read the device's raw 0..65535 axes — a centred stick decoded
         * to a pinned direction once the lobby had been visited. */
        LPDIRECTINPUTDEVICE8A dev = scan_dev(i);
        if (!dev) continue;
        memset(&js, 0, sizeof(js));
        if (FAILED(IDirectInputDevice8_Poll(dev))) {
            IDirectInputDevice8_Acquire(dev);
            IDirectInputDevice8_Poll(dev);
        }
        if (SUCCEEDED(IDirectInputDevice8_GetDeviceState(dev, sizeof(js), &js)))
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
    const long T = 130;  /* [#R3-5 reverted] 90 caused hover auto-scroll on biased pads; navdiag pending */
    td5_xinput_correlate_tick();   /* observe DI<->WGI from menu A/Start presses */
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

        uint32_t tnow = (uint32_t)td5_plat_time_ms();
        cx = js.lX - TD5_PLAT_JS_AXIS_CENTER;
        cy = js.lY - TD5_PLAT_JS_AXIS_CENTER;
        /* Trust a stick axis only while it has been at rest recently (see
         * s_js_*_rest_ms / TD5_PLAT_JS_NAV_IDLE_MS): an axis pegged from power-on
         * OR drifted to an off-centre rest can no longer jam a direction and
         * (OR-aggregated below) starve UP/LEFT for every other pad. The dpad
         * (POV) and buttons below are always honoured. (X -> LEFT/RIGHT bits
         * 1/2, Y -> UP/DOWN bits 4/8; stick up = lY low = UP.) */
        db |= td5_js_axis_nav_bits(cx, T, tnow, &s_js_x_live[i], &s_js_x_rest_ms[i], 1u, 2u);
        db |= td5_js_axis_nav_bits(cy, T, tnow, &s_js_y_live[i], &s_js_y_rest_ms[i], 4u, 8u);
        if (!pov_centered(js.rgdwPOV[0])) {
            DWORD deg = js.rgdwPOV[0] / 100;
            if (deg > 270 || deg <  90) db |= 4;
            if (deg >  90 && deg < 270) db |= 8;
            if (deg > 180 && deg < 360) db |= 1;
            if (deg >   0 && deg < 180) db |= 2;
        }
        if (js.rgbButtons[0] & 0x80) db |= 0x10;     /* A = confirm/select */
        if (js.rgbButtons[1] & 0x80) db |= 0x20;     /* B = back/cancel    */
        if (js.rgbButtons[2] & 0x80) db |= 0x80;     /* [#15] X = delete (name entry) */
        if (js.rgbButtons[7] & 0x80) db |= 0x40;     /* [#R3] Start = layout/confirm (was only on device_nav) */
        /* [#R3-5 DIAG 2026-06-19; rest-age added 2026-06-25] The deadzone+
         * threshold fix didn't resolve the "can't go UP/LEFT" report, so log what
         * the pad produces — raw axes, POV, decoded cx/cy, the live-gate flags,
         * the ms-since-each-axis-was-at-rest (rest=x/y: >= TD5_PLAT_JS_NAV_IDLE_MS
         * means that axis is being dropped as drift/bias, the v2 fix), and the nav
         * bits — on every direction-bit change (plus a periodic rest sample).
         * -> engine.log. Remove once the controller behaviour is understood. */
        {
            static uint32_t s_navdiag_prev[16];
            static uint32_t s_navdiag_div = 0;
            if ((db & 0x0Fu) != (s_navdiag_prev[i] & 0x0Fu) || ((++s_navdiag_div % 150u) == 0u)) {
                TD5_LOG_I(LOG_TAG,
                    "navdiag dev%d: lX=%ld lY=%ld POV=0x%lX cx=%ld cy=%ld live=%d/%d "
                    "rest=%u/%u db=0x%02X",
                    i, (long)js.lX, (long)js.lY, (unsigned long)js.rgdwPOV[0],
                    (long)cx, (long)cy, s_js_x_live[i], s_js_y_live[i],
                    (unsigned)(tnow - s_js_x_rest_ms[i]), (unsigned)(tnow - s_js_y_rest_ms[i]),
                    (unsigned)db);
            }
            s_navdiag_prev[i] = db;
        }
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
    const long T = 130;  /* [#R3-5 reverted] 90 caused hover auto-scroll on biased pads; navdiag pending */
    if (!td5_plat_js_read(device_slot, &js)) return 0;
    cx = js.lX - TD5_PLAT_JS_AXIS_CENTER;
    cy = js.lY - TD5_PLAT_JS_AXIS_CENTER;
    if (cx < -T) db |= 1;
    if (cx > T) db |= 2;
    if (cy < -T) db |= 4;
    if (cy > T) db |= 8;       /* stick up = lY low = UP */
    if (!pov_centered(js.rgdwPOV[0])) {
        DWORD deg = js.rgdwPOV[0] / 100;
        if (deg > 270 || deg <  90) db |= 4;
        if (deg >  90 && deg < 270) db |= 8;
        if (deg > 180 && deg < 360) db |= 1;
        if (deg >   0 && deg < 180) db |= 2;
    }
    if (js.rgbButtons[0] & 0x80) db |= 0x10;         /* A = confirm/select */
    if (js.rgbButtons[1] & 0x80) db |= 0x20;         /* B = back/cancel    */
    if (js.rgbButtons[2] & 0x80) db |= 0x80;         /* [#15] X = delete (name entry) */
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
    const long T = 130;  /* [#R3-5 reverted] 90 caused hover auto-scroll on biased pads; navdiag pending */
    LPDIRECTINPUTDEVICE8A dev;
    td5_xinput_correlate_tick();   /* observe DI<->WGI from car-select grid A/Start presses */
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
    /* Same stuck/bias rejection as td5_plat_input_frontend_nav, keyed by the same
     * scan_dev() index so a drifted stick can't pin this player's grid UP/LEFT. */
    {
        uint32_t tnow = (uint32_t)td5_plat_time_ms();
        db |= td5_js_axis_nav_bits(cx, T, tnow, &s_js_x_live[enum_index], &s_js_x_rest_ms[enum_index], 1u, 2u);
        db |= td5_js_axis_nav_bits(cy, T, tnow, &s_js_y_live[enum_index], &s_js_y_rest_ms[enum_index], 4u, 8u);
    }
    if (!pov_centered(js.rgdwPOV[0])) {
        DWORD deg = js.rgdwPOV[0] / 100;
        if (deg > 270 || deg <  90) db |= 4;
        if (deg >  90 && deg < 270) db |= 8;
        if (deg > 180 && deg < 360) db |= 1;
        if (deg >   0 && deg < 180) db |= 2;
    }
    if (js.rgbButtons[0] & 0x80) db |= 0x10;         /* A = confirm/select */
    if (js.rgbButtons[1] & 0x80) db |= 0x20;         /* B = back/cancel    */
    if (js.rgbButtons[2] & 0x80) db |= 0x80;         /* [#15] X = delete (name entry) */
    if (js.rgbButtons[7] & 0x80) db |= 0x40;         /* Start / Menu        */
    return db;
}

/* ========================================================================
 * Gamepad rumble — motor-magnitude model
 *
 * 8BitDo pads (and most modern XInput gamepads) report NO DIDC_FORCEFEEDBACK to
 * DirectInput, so the DI-effect path (below) no-ops on them. Rumble is driven
 * through Windows.Gaming.Input (the WGI block further down) — there is NO XInput
 * output path: XInput capped at 4 controllers, while WGI has no such cap and
 * reaches every pad (Win10+; all our machines qualify). This block holds only the
 * backend-neutral motor state the FF entry points (td5_plat_ff_constant /
 * td5_plat_ff_stop / td5_plat_ff_xinput_rumble) drive; td5_wgi_push() forwards it
 * to the pad. DI effects remain the path for wheels.
 *
 * The low (heavy) motor has two independent contributors so a stop on one doesn't
 * silence the other: [0]=side-collision pulse (effect slot 2), [1]=continuous buzz
 * (redline-in-manual OR drift — the input layer passes the louder of the two to
 * td5_plat_ff_xinput_rumble). Resolved s_xi_low = max(contrib[0],contrib[1]).
 * Continuous wheel forces — steering (slot 0) and terrain (slot 3) — are NOT routed
 * here, so a gamepad is silent during steady normal driving. (The s_xi_ and
 * _xinput_ names below are historical; the model is backend-neutral now.)
 * ======================================================================== */

static WORD s_xi_low [TD5_PLAT_MAX_JS_SLOTS];   /* low-freq (heavy) motor (resolved) */
static WORD s_xi_high[TD5_PLAT_MAX_JS_SLOTS];   /* high-freq (light) motor */
#define TD5_XI_LOW_COLLISION 0   /* contributor index: side-collision pulse */
#define TD5_XI_LOW_REDLINE   1   /* contributor index: continuous buzz (redline-in-manual / drift) */
static WORD s_xi_low_contrib[TD5_PLAT_MAX_JS_SLOTS][2];

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

/* ------------------------------------------------------------------------
 * Observed DI<->WGI correlation [SPLIT-SCREEN FF, WGI-only 2026-06-25]
 *
 * Rumble output goes through Windows.Gaming.Input (no 4-controller cap). WGI
 * exposes no link from a WGI Gamepad to a DirectInput device instance, so we
 * OBSERVE the mapping: when a player presses a button on their pad in the
 * menu/lobby, that press appears on both their DI device AND their WGI gamepad in
 * the same frame. When exactly one (still-unbound) DI device and exactly one
 * (still-unclaimed) WGI gamepad show the same button, they are provably the same
 * physical pad — bind them. Keyed to the physical device, so it is correct
 * regardless of pick order, identical VID/PID, or enumeration order. Captured at
 * lobby-join (each player presses A on their OWN pad) and persists for the
 * process lifetime (the USB mapping is constant).
 * ------------------------------------------------------------------------ */

/* Cached td5_device_is_xinput(): the raw-input scan is too costly to repeat
 * every menu frame, and the device set is fixed for the process lifetime. */
static signed char s_dev_xinput_cache[16];
static int s_dev_xinput_cache_inited = 0;
static int td5_device_is_xinput_cached(int i)
{
    if (i < 0 || i >= 16) return 0;
    if (!s_dev_xinput_cache_inited) {
        int k;
        for (k = 0; k < 16; k++) s_dev_xinput_cache[k] = -1;
        s_dev_xinput_cache_inited = 1;
    }
    if (s_dev_xinput_cache[i] < 0)
        s_dev_xinput_cache[i] = (signed char)(td5_device_is_xinput(i) ? 1 : 0);
    return s_dev_xinput_cache[i];
}

/* WGI rumble backend (defined below). Forward-declared here so the menu
 * correlation can learn each pad's WGI gamepad. */
static int  td5_ff_wgi_enabled(void);
static int  td5_wgi_init(void);
static int  td5_wgi_di_bound(int di);
static void td5_wgi_correlate_step(uint32_t di_a_mask, uint32_t di_start_mask);

/* Per-menu-frame correlation: read every still-unbound XInput-class pad's A/Start
 * through the shared scan handles and bind it to the matching WGI gamepad by
 * button-press coincidence. Called from the lobby join scan, the frontend nav
 * scan and the car-select grid scan. Cheap once everything is bound (the
 * unbound-skip leaves di_a/di_start zero and it returns before reading anything).
 * A = DI button 0 / GamepadButtons_A; Start = DI button 7 / GamepadButtons_Menu. */
static void td5_xinput_correlate_tick(void)
{
    uint32_t di_a = 0, di_start = 0;
    int i;
    if (!td5_ff_wgi_enabled() || !td5_wgi_init()) return;

    for (i = 1; i < s_device_count && i < 16; i++) {
        DIJOYSTATE2 js;
        LPDIRECTINPUTDEVICE8A dev;
        if (td5_wgi_di_bound(i)) continue;                 /* already bound */
        if (!td5_device_is_xinput_cached(i)) continue;     /* only XInput-class pads */
        dev = scan_dev(i);
        if (!dev) continue;
        memset(&js, 0, sizeof(js));
        if (FAILED(IDirectInputDevice8_Poll(dev))) {
            IDirectInputDevice8_Acquire(dev);
            IDirectInputDevice8_Poll(dev);
        }
        if (FAILED(IDirectInputDevice8_GetDeviceState(dev, sizeof(js), &js))) continue;
        if (js.rgbButtons[0] & 0x80) di_a     |= (1u << i);
        if (js.rgbButtons[7] & 0x80) di_start |= (1u << i);
    }
    if (!di_a && !di_start) return;                        /* nothing pressed to learn from */

    td5_wgi_correlate_step(di_a, di_start);
}

/* ========================================================================
 * Windows.Gaming.Input (WGI) rumble backend  [WGI-only 2026-06-25]
 *
 * The single gamepad-rumble output backend. WGI's Gamepad API has no controller
 * cap (XInput capped at 4; this is why XInput rumble was removed). INPUT still
 * comes from DirectInput; WGI is used only for OUTPUT, correlating each player's
 * DI device to its WGI Gamepad by the menu button-press coincidence above.
 *
 * WGI is WinRT, called through the documented WinRT COM ABI from C with the
 * interfaces/structs/IID transcribed verbatim from mingw windows.gaming.input.h.
 * combase.dll is loaded DYNAMICALLY (LoadLibrary+GetProcAddress, like the dwmapi
 * path) so a pre-Win8 host without WinRT just gets no rumble (the build needs no
 * extra link lib). All our target machines are Win10+.
 *
 * Knob TD5RE_FF_WGI (cached): default ON; "0" -> no gamepad rumble.
 * ======================================================================== */

#ifndef CO_E_NOTINITIALIZED
#define CO_E_NOTINITIALIZED 0x800401F0L
#endif

/* C-ABI subset of Windows.Gaming.Input (layout verbatim from the WinRT header). */
typedef struct TD5_WgiVibration {
    double LeftMotor, RightMotor, LeftTrigger, RightTrigger;   /* 0.0..1.0 */
} TD5_WgiVibration;
/* GamepadReading: only Buttons (offset 8) is read; the rest is sized to match the
 * 64-byte ABI struct. Every member sits at an 8-byte-multiple offset by explicit
 * padding, so the layout is identical under MSVC and MinGW regardless of GCC's
 * 32-bit double-alignment default. */
typedef struct TD5_WgiReading {
    unsigned long long Timestamp;   /* @0  */
    unsigned int       Buttons;     /* @8  GamepadButtons: A=0x4, Menu=0x1 */
    unsigned int       _pad;        /* @12 */
    double             _rest[6];    /* @16..63 triggers + thumbsticks (unused) */
} TD5_WgiReading;

typedef struct TD5_WgiGamepadVtbl {
    void          *QueryInterface_;
    unsigned long (__stdcall *AddRef)(void *This);
    unsigned long (__stdcall *Release)(void *This);
    void          *GetIids_, *GetRuntimeClassName_, *GetTrustLevel_;
    long          (__stdcall *get_Vibration)(void *This, TD5_WgiVibration *value);
    long          (__stdcall *put_Vibration)(void *This, TD5_WgiVibration value);
    long          (__stdcall *GetCurrentReading)(void *This, TD5_WgiReading *value);
} TD5_WgiGamepadVtbl;
typedef struct TD5_WgiGamepad { const TD5_WgiGamepadVtbl *lpVtbl; } TD5_WgiGamepad;

typedef struct TD5_WgiVectorViewVtbl {
    void          *QueryInterface_;
    unsigned long (__stdcall *AddRef)(void *This);
    unsigned long (__stdcall *Release)(void *This);
    void          *GetIids_, *GetRuntimeClassName_, *GetTrustLevel_;
    long          (__stdcall *GetAt)(void *This, unsigned int index, TD5_WgiGamepad **value);
    long          (__stdcall *get_Size)(void *This, unsigned int *value);
    void          *IndexOf_, *GetMany_;
} TD5_WgiVectorViewVtbl;
typedef struct TD5_WgiVectorView { const TD5_WgiVectorViewVtbl *lpVtbl; } TD5_WgiVectorView;

typedef struct TD5_WgiStaticsVtbl {
    void          *QueryInterface_;
    unsigned long (__stdcall *AddRef)(void *This);
    unsigned long (__stdcall *Release)(void *This);
    void          *GetIids_, *GetRuntimeClassName_, *GetTrustLevel_;
    void          *add_GamepadAdded_, *remove_GamepadAdded_;
    void          *add_GamepadRemoved_, *remove_GamepadRemoved_;
    long          (__stdcall *get_Gamepads)(void *This, TD5_WgiVectorView **value);
} TD5_WgiStaticsVtbl;
typedef struct TD5_WgiStatics { const TD5_WgiStaticsVtbl *lpVtbl; } TD5_WgiStatics;

/* IID_IGamepadStatics {8BBCE529-D49C-39E9-9560-E47DDE96B7C8} (from the header). */
static const GUID TD5_IID_IGamepadStatics =
    { 0x8bbce529, 0xd49c, 0x39e9, { 0x95, 0x60, 0xe4, 0x7d, 0xde, 0x96, 0xb7, 0xc8 } };

typedef long (WINAPI *PFN_RoGetActivationFactory)(void *hstr, const GUID *iid, void **factory);
typedef long (WINAPI *PFN_WindowsCreateString)(const WCHAR *src, UINT len, void **hstr);
typedef long (WINAPI *PFN_WindowsDeleteString)(void *hstr);
typedef long (WINAPI *PFN_RoInitialize)(int init_type);

static int s_wgi_loaded = 0;     /* combase probed (1 even if not found) */
static int s_wgi_avail  = 0;     /* factory resolved */
static PFN_RoGetActivationFactory s_RoGetActivationFactory = NULL;
static PFN_WindowsCreateString    s_WindowsCreateString    = NULL;
static PFN_WindowsDeleteString    s_WindowsDeleteString    = NULL;
static PFN_RoInitialize           s_RoInitialize           = NULL;
static TD5_WgiStatics *s_wgi_statics = NULL;

/* Observed binding: DI enumeration index -> WGI Gamepad (AddRef'd), NULL=unbound.
 * Per-player resolved pointer set at ff_init. */
static TD5_WgiGamepad *s_wgi_pad_for_di[16];
static TD5_WgiGamepad *s_wgi_pad[TD5_PLAT_MAX_JS_SLOTS];
static WORD s_wgi_last_lo[TD5_PLAT_MAX_JS_SLOTS];   /* last motor magnitudes pushed (de-dup) */
static WORD s_wgi_last_hi[TD5_PLAT_MAX_JS_SLOTS];

/* TD5RE_FF_WGI (cached): master gate for the WGI rumble path. Default ON. */
static int td5_ff_wgi_enabled(void)
{
    static int s_init = 0, s_on = 1;
    if (!s_init) {
        s_on = td5_env_flag_on("TD5RE_FF_WGI");
        s_init = 1;
        TD5_LOG_I(LOG_TAG, "FF WGI rumble path: %s",
                  s_on ? "enabled (default — lifts 4-pad cap)" : "disabled (TD5RE_FF_WGI=0)");
    }
    return s_on;
}

/* Resolve WGI once: load combase.dll, get the IGamepadStatics activation factory.
 * Idempotent; returns 1 if the WGI rumble path is usable. */
static int td5_wgi_init(void)
{
    static const WCHAR k_cls[] = L"Windows.Gaming.Input.Gamepad";  /* 28 chars */
    HMODULE h;
    void *cls = NULL;
    long hr;
    if (s_wgi_loaded) return s_wgi_avail;
    s_wgi_loaded = 1;
    if (!td5_ff_wgi_enabled()) return 0;
    h = LoadLibraryA("combase.dll");
    if (!h) { TD5_LOG_I(LOG_TAG, "FF WGI: combase.dll absent -> no gamepad rumble"); return 0; }
    s_RoGetActivationFactory = (PFN_RoGetActivationFactory)(void *)GetProcAddress(h, "RoGetActivationFactory");
    s_WindowsCreateString    = (PFN_WindowsCreateString)(void *)GetProcAddress(h, "WindowsCreateString");
    s_WindowsDeleteString    = (PFN_WindowsDeleteString)(void *)GetProcAddress(h, "WindowsDeleteString");
    s_RoInitialize           = (PFN_RoInitialize)(void *)GetProcAddress(h, "RoInitialize");
    if (!s_RoGetActivationFactory || !s_WindowsCreateString || !s_WindowsDeleteString) {
        TD5_LOG_W(LOG_TAG, "FF WGI: combase exports missing -> no gamepad rumble");
        return 0;
    }
    hr = s_WindowsCreateString(k_cls, 28u, &cls);
    if (FAILED(hr) || !cls) { TD5_LOG_W(LOG_TAG, "FF WGI: WindowsCreateString failed hr=0x%08lX", (unsigned long)hr); return 0; }
    hr = s_RoGetActivationFactory(cls, &TD5_IID_IGamepadStatics, (void **)&s_wgi_statics);
    if (hr == CO_E_NOTINITIALIZED && s_RoInitialize) {
        s_RoInitialize(1 /* RO_INIT_MULTITHREADED */);
        hr = s_RoGetActivationFactory(cls, &TD5_IID_IGamepadStatics, (void **)&s_wgi_statics);
    }
    s_WindowsDeleteString(cls);
    if (FAILED(hr) || !s_wgi_statics) {
        TD5_LOG_W(LOG_TAG, "FF WGI: RoGetActivationFactory(Gamepad) failed hr=0x%08lX -> no gamepad rumble",
                  (unsigned long)hr);
        s_wgi_statics = NULL;
        return 0;
    }
    s_wgi_avail = 1;
    TD5_LOG_I(LOG_TAG, "FF WGI: available (rumble lifts the 4-pad XInput cap)");
    return 1;
}

static int td5_wgi_di_bound(int di)
{
    return (di >= 0 && di < 16 && s_wgi_pad_for_di[di] != NULL);
}

/* Push the slot's cached low/high motor magnitudes to its WGI gamepad. */
static void td5_wgi_push(int device_slot)
{
    TD5_WgiGamepad *g;
    TD5_WgiVibration vib;
    WORD lo, hi;
    if (device_slot < 0 || device_slot >= TD5_PLAT_MAX_JS_SLOTS) return;
    g = s_wgi_pad[device_slot];
    if (!g) return;
    lo = s_xi_low[device_slot];
    hi = s_xi_high[device_slot];
    if (lo == s_wgi_last_lo[device_slot] && hi == s_wgi_last_hi[device_slot]) return;
    s_wgi_last_lo[device_slot] = lo;
    s_wgi_last_hi[device_slot] = hi;
    vib.LeftMotor    = (double)lo / 65535.0;   /* heavy/low-freq motor */
    vib.RightMotor   = (double)hi / 65535.0;   /* light/high-freq motor */
    vib.LeftTrigger  = 0.0;
    vib.RightTrigger = 0.0;
    g->lpVtbl->put_Vibration((void *)g, vib);
}

/* Snapshot the currently-connected WGI gamepads into out[] (each AddRef'd). The
 * caller Releases every returned pointer. Returns the count. */
static int td5_wgi_snapshot(TD5_WgiGamepad **out, int max)
{
    TD5_WgiVectorView *view = NULL;
    unsigned int n = 0, i;
    int c = 0;
    if (!s_wgi_avail || !s_wgi_statics) return 0;
    if (FAILED(s_wgi_statics->lpVtbl->get_Gamepads((void *)s_wgi_statics, &view)) || !view) return 0;
    if (FAILED(view->lpVtbl->get_Size((void *)view, &n))) n = 0;
    for (i = 0; i < n && c < max; i++) {
        TD5_WgiGamepad *g = NULL;
        if (SUCCEEDED(view->lpVtbl->GetAt((void *)view, i, &g)) && g) out[c++] = g;  /* GetAt AddRefs */
    }
    view->lpVtbl->Release((void *)view);
    return c;
}

static int td5_wgi_pad_is_claimed(TD5_WgiGamepad *g)
{
    int i;
    for (i = 0; i < 16; i++) if (s_wgi_pad_for_di[i] == g) return 1;
    return 0;
}

/* Is the stored gamepad object `g` still in WGI's live connected set?
 * [FF WGI hot-plug 2026-06-25] A wireless pad that sleeps or is unplugged is
 * removed from Gamepads, and WGI mints a NEW object on reconnect — so a
 * persisted binding to the old object goes stale (rumble would push into a dead
 * object forever). Pointer identity is the same liveness test the correlation
 * already relies on (td5_wgi_pad_is_claimed compares the stored pointers the
 * same way; WGI returns a stable IGamepad* per connected pad). Note: a pad that
 * is merely idle-but-connected stays in the list, so this only trips on a real
 * disconnect, not on inactivity. */
static int td5_wgi_pad_is_live(TD5_WgiGamepad *g)
{
    TD5_WgiGamepad *pads[16];
    int np, k, live = 0;
    if (!g) return 0;
    np = td5_wgi_snapshot(pads, 16);
    for (k = 0; k < np; k++) {
        if (pads[k] == g) live = 1;
        pads[k]->lpVtbl->Release((void *)pads[k]);   /* snapshot AddRef'd each */
    }
    return live;
}

/* Bind a DI device to a WGI gamepad when exactly ONE unbound XInput-class DI
 * device and exactly ONE unclaimed WGI gamepad hold the same menu button this
 * frame. Conservative (never binds on >1-either-side ambiguity) so it can never
 * mis-route; an ambiguous frame just resolves on a later one. */
static void td5_wgi_correlate_step(uint32_t di_a_mask, uint32_t di_start_mask)
{
    TD5_WgiGamepad *pads[16];
    uint32_t wgi_a = 0, wgi_start = 0;
    uint32_t di_masks[2], wgi_masks[2];
    int np, k, pass;
    if (!s_wgi_avail) return;
    if (!di_a_mask && !di_start_mask) return;
    np = td5_wgi_snapshot(pads, 16);
    if (np <= 0) return;

    for (k = 0; k < np; k++) {
        TD5_WgiReading rd;
        memset(&rd, 0, sizeof(rd));
        if (SUCCEEDED(pads[k]->lpVtbl->GetCurrentReading((void *)pads[k], &rd))) {
            if (rd.Buttons & 0x4u) wgi_a     |= (1u << k);   /* GamepadButtons_A */
            if (rd.Buttons & 0x1u) wgi_start |= (1u << k);   /* GamepadButtons_Menu (Start) */
        }
    }

    di_masks[0] = di_a_mask;     wgi_masks[0] = wgi_a;
    di_masks[1] = di_start_mask; wgi_masks[1] = wgi_start;
    for (pass = 0; pass < 2; pass++) {
        int di_count = 0, wgi_count = 0, the_di = -1, the_k = -1, i;
        if (!di_masks[pass]) continue;
        for (k = 0; k < np; k++) {
            if (!(wgi_masks[pass] & (1u << k))) continue;
            if (td5_wgi_pad_is_claimed(pads[k])) continue;
            wgi_count++; the_k = k;
        }
        if (wgi_count != 1) continue;
        for (i = 1; i < s_device_count && i < 16; i++) {
            if (!(di_masks[pass] & (1u << i))) continue;
            if (s_wgi_pad_for_di[i] != NULL) continue;        /* already bound */
            if (!td5_device_is_xinput_cached(i)) continue;
            di_count++; the_di = i;
        }
        if (di_count != 1) continue;
        pads[the_k]->lpVtbl->AddRef((void *)pads[the_k]);     /* retain past the snapshot release */
        s_wgi_pad_for_di[the_di] = pads[the_k];
        TD5_LOG_I(LOG_TAG,
            "FF WGI correlate: DI device %d (%s) <-> WGI gamepad (button coincidence)",
            the_di, (the_di < s_device_count ? s_device_names[the_di] : "?"));
    }

    for (k = 0; k < np; k++) if (pads[k]) pads[k]->lpVtbl->Release((void *)pads[k]);
}

/* Release all WGI gamepad objects + the factory (called at FF shutdown). */
static void td5_wgi_shutdown(void)
{
    int i;
    for (i = 0; i < 16; i++) {
        if (s_wgi_pad_for_di[i]) {
            s_wgi_pad_for_di[i]->lpVtbl->Release((void *)s_wgi_pad_for_di[i]);
            s_wgi_pad_for_di[i] = NULL;
        }
    }
    for (i = 0; i < TD5_PLAT_MAX_JS_SLOTS; i++) s_wgi_pad[i] = NULL;
    if (s_wgi_statics) {
        s_wgi_statics->lpVtbl->Release((void *)s_wgi_statics);
        s_wgi_statics = NULL;
    }
    s_wgi_avail = 0;
    /* Symmetric teardown: clear the one-shot guard too so a later td5_wgi_init()
     * re-resolves the factory instead of returning the stale s_wgi_avail=0. With
     * the fix this runs only at process exit, but keeping init/shutdown symmetric
     * prevents the "permanently dead after teardown" footgun from ever recurring. */
    s_wgi_loaded = 0;
    TD5_LOG_I(LOG_TAG, "FF WGI: subsystem released (process exit)");
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
    if (device_slot < 0 || device_slot >= TD5_PLAT_MAX_JS_SLOTS) return 0;
    /* Route here only if a WGI gamepad is bound for this slot (so a gamepad's
     * events don't fall through to the DI-effect path it cannot use). */
    if (s_wgi_pad[device_slot] == NULL) return 0;
    w = td5_xinput_scale(magnitude);
    if (slot == 1) {
        s_xi_high[device_slot] = w;                       /* sharp crash/gear jolt motor */
        td5_wgi_push(device_slot);
    } else if (slot == 2) {
        /* side-collision pulse -> low motor collision contributor */
        s_xi_low_contrib[device_slot][TD5_XI_LOW_COLLISION] = w;
        s_xi_low[device_slot] = s_xi_low_contrib[device_slot][0] > s_xi_low_contrib[device_slot][1]
                              ? s_xi_low_contrib[device_slot][0] : s_xi_low_contrib[device_slot][1];
        td5_wgi_push(device_slot);
    }
    /* slot 0 (steering) and slot 3 (terrain/drift): consumed but NOT routed to a
     * motor — these are continuous wheel forces that would buzz a gamepad. */
    return 1;
}

/* Route one effect-slot STOP to the XInput motors. Returns 1 if handled here.
 * Only slots 1/2 own a motor contribution; slots 0/3 are silent no-ops. */
static int td5_xinput_route_stop(int device_slot, int slot)
{
    if (device_slot < 0 || device_slot >= TD5_PLAT_MAX_JS_SLOTS) return 0;
    if (s_wgi_pad[device_slot] == NULL) return 0;  /* no WGI gamepad bound */
    if (slot == 1) {
        s_xi_high[device_slot] = 0;
        td5_wgi_push(device_slot);
    } else if (slot == 2) {
        s_xi_low_contrib[device_slot][TD5_XI_LOW_COLLISION] = 0;
        s_xi_low[device_slot] = s_xi_low_contrib[device_slot][0] > s_xi_low_contrib[device_slot][1]
                              ? s_xi_low_contrib[device_slot][0] : s_xi_low_contrib[device_slot][1];
        td5_wgi_push(device_slot);
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

    if (device_slot < 0 || device_slot >= TD5_PLAT_MAX_JS_SLOTS) {
        return 0;
    }

    /* Force feedback runs on this player's joystick (the wheel/pad). */
    if (!s_di_joystick[device_slot]) {
        return 0;
    }

    /* Gamepads (8BitDo & most modern pads) report NO DIDC_FORCEFEEDBACK to
     * DirectInput, so the DI-effect path below no-ops on them; their rumble goes
     * through WGI instead. Wheels keep the DI-effect path. Resolve which enumerated
     * device this slot is bound to (by instance GUID); if it is an XInput-class
     * gamepad, route it to the WGI gamepad observed for it in the menus. */
    s_wgi_pad[device_slot] = NULL;
    s_wgi_last_lo[device_slot] = s_wgi_last_hi[device_slot] = 0xFFFF; /* force the rest-push below */
    s_xi_low[device_slot] = s_xi_high[device_slot] = 0;
    s_xi_low_contrib[device_slot][0] = s_xi_low_contrib[device_slot][1] = 0;
    {
        int di = -1, k;
        for (k = 1; k < s_device_count && k < 16; k++) {
            if (IsEqualGUID(&s_device_guids[k], &s_di_joystick_guid[device_slot])) {
                di = k;
                break;
            }
        }
        if (di > 0 && td5_device_is_xinput(di)) {
            if (td5_ff_wgi_enabled() && td5_wgi_init()) {
                /* [FF WGI hot-plug 2026-06-25] Drop a STALE binding before using
                 * it. Because bindings now persist across races, a pad that slept
                 * or was unplugged since it was learned points at a WGI object
                 * that's been removed; clear it so the menu correlation relearns
                 * the reconnected pad's new object instead of pushing rumble into
                 * a dead one. (A merely-idle connected pad stays in the live set,
                 * so this only trips on a real disconnect.) */
                if (s_wgi_pad_for_di[di] && !td5_wgi_pad_is_live(s_wgi_pad_for_di[di])) {
                    TD5_LOG_W(LOG_TAG,
                              "FF WGI: bound gamepad for DI device %d is gone "
                              "(sleep/disconnect) -> clearing binding to relearn", di);
                    s_wgi_pad_for_di[di]->lpVtbl->Release((void *)s_wgi_pad_for_di[di]);
                    s_wgi_pad_for_di[di] = NULL;
                }
                if (s_wgi_pad_for_di[di]) {
                    s_wgi_pad[device_slot] = s_wgi_pad_for_di[di];
                    xinput_ready = 1;
                    td5_wgi_push(device_slot);   /* motors at rest */
                    /* Log the per-player rumble route so a manual test can confirm
                     * each player drives their OWN pad. A swap here is the "wrong
                     * joystick" symptom. */
                    TD5_LOG_I(LOG_TAG,
                              "FF route: player_slot=%d -> DI device=%d (%s) -> WGI gamepad",
                              device_slot, di, s_device_names[di]);
                } else {
                    TD5_LOG_W(LOG_TAG,
                              "FF: slot=%d device=%d is an XInput pad but no WGI gamepad observed yet "
                              "(press A on it in the menu)", device_slot, di);
                }
            } else if (td5_ff_wgi_enabled()) {
                TD5_LOG_W(LOG_TAG,
                          "FF: slot=%d device=%d is an XInput pad but WGI is unavailable "
                          "(combase/WinRT missing)", device_slot, di);
            }
            /* TD5RE_FF_WGI=0 -> gamepad rumble intentionally off (silent). */
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
         * this slot to WGI rumble above, FF is still functional — report success
         * so the input layer drives effects (which we forward to the motor).
         * Otherwise this device truly has no FF. */
        ZeroMemory(&s_ff[device_slot], sizeof(s_ff[device_slot]));
        if (xinput_ready) {
            TD5_LOG_I(LOG_TAG, "FF init ok (WGI rumble only): dev=%d", device_slot);
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
        return xinput_ready;   /* WGI rumble still usable if it was assigned */
    }

    if (!td5_ff_create_constant_effect(device_slot, 0, s_ff[device_slot].axes[0], 0) ||
        !td5_ff_create_constant_effect(device_slot, 1, s_ff[device_slot].axes[1], 9000) ||
        !td5_ff_create_constant_effect(device_slot, 2, s_ff[device_slot].axes[0], 0) ||
        !td5_ff_create_periodic_effect(device_slot, 3)) {
        td5_ff_release_effects(device_slot);
        ZeroMemory(&s_ff[device_slot], sizeof(s_ff[device_slot]));
        return xinput_ready;   /* WGI rumble still usable if it was assigned */
    }

    TD5_LOG_I(LOG_TAG, "FF init ok: dev=%d is_wheel=%d wgi=%d",
              device_slot, s_ff[device_slot].is_wheel, s_wgi_pad[device_slot] != NULL);
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
        /* Stop the motor on this slot's WGI gamepad (if any), then drop the
         * per-slot routing pointer. The gamepad OBJECT stays alive in
         * s_wgi_pad_for_di[] — see the note below — so the next race re-resolves
         * s_wgi_pad[dev] from it in td5_plat_ff_init(). */
        if (s_wgi_pad[dev]) {
            s_xi_low[dev] = s_xi_high[dev] = 0;
            s_xi_low_contrib[dev][0] = s_xi_low_contrib[dev][1] = 0;
            td5_wgi_push(dev);
        }
        s_wgi_pad[dev] = NULL;
    }
    /* [FF WGI per-race-teardown regression 2026-06-25] Do NOT call
     * td5_wgi_shutdown() here. td5_input_ff_shutdown() -> td5_plat_ff_shutdown()
     * runs at the END OF EVERY RACE (td5_game.c:6473), but the WGI activation
     * factory (s_wgi_statics) and the observed DI<->WGI correlation bindings
     * (s_wgi_pad_for_di[]) are PROCESS-LIFETIME state: the USB mapping is constant
     * and each binding is learned ONCE from a menu/lobby A-press. Releasing them
     * per race — together with the one-shot s_wgi_loaded guard in td5_wgi_init()
     * that then keeps returning the now-zeroed s_wgi_avail — left WGI permanently
     * unavailable after the first race, so gamepad rumble died on EVERY pad
     * ("force feedback works for the first races, then nothing"). The real WGI
     * teardown lives in td5_plat_input_shutdown() (process exit only). */
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
 * 0 silences it. No-op on a DI FF wheel (no WGI gamepad bound) — there the same buzz
 * already rides on effect slot 3, so the wheel path is untouched. The input layer
 * passes the louder of redline-in-manual and drift here (0 when neither applies),
 * so the gamepad motor sees ONLY that buzz (never terrain/steering) and returns to
 * silence the moment the car stops drifting / leaves the limiter. */
void td5_plat_ff_xinput_rumble(int device_slot, int magnitude)
{
    WORD w;
    if (device_slot < 0 || device_slot >= TD5_PLAT_MAX_JS_SLOTS) return;
    if (s_wgi_pad[device_slot] == NULL) return;  /* not a WGI gamepad (DI wheel / unbound) */
    w = td5_xinput_scale(magnitude);
    if (w == s_xi_low_contrib[device_slot][TD5_XI_LOW_REDLINE])
        return;                                  /* unchanged -> skip the push */
    s_xi_low_contrib[device_slot][TD5_XI_LOW_REDLINE] = w;
    s_xi_low[device_slot] = s_xi_low_contrib[device_slot][0] > s_xi_low_contrib[device_slot][1]
                          ? s_xi_low_contrib[device_slot][0] : s_xi_low_contrib[device_slot][1];
    td5_wgi_push(device_slot);
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
 * Rendering Backend -- Bridge to D3D11 wrapper
 *
 * These call directly into the wrapper's D3D11 backend via the global
 * g_backend state and the WrapperDevice/WrapperSurface functions.
 * ======================================================================== */

void td5_plat_render_begin_scene(void)
{
    if (g_backend.device_removed) return;
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
    if (g_backend.device_removed) return;
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
    static unsigned s_fx_gen = 0;   /* device generation these objects were built for */
    int i = (int)which;
    if (i < 0 || i > 3) return NULL;

    /* [DEVICE-LOST 2026-07-20] These procedural FX shaders + the b1 constant
     * buffer are cached here, outside the wrapper's recreated resource set, so a
     * device-lost recovery (bumps g_backend.device_generation) leaves them
     * pointing at the freed device — the next in-race particle/glow draw would
     * bind dead-device objects. Drop them on a generation change and reset the
     * per-shader "tried" flags so they recreate on demand. */
    if (g_backend.device && s_fx_gen != g_backend.device_generation) {
        int k;
        for (k = 0; k < 4; k++) {
            if (s_fx_ps[k]) { ID3D11PixelShader_Release(s_fx_ps[k]); s_fx_ps[k] = NULL; }
            s_fx_tried[k] = 0;
        }
        if (s_fx_cb) { ID3D11Buffer_Release(s_fx_cb); s_fx_cb = NULL; }
        s_fx_gen = g_backend.device_generation;
    }

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
    if (g_backend.device_removed) return;
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
        Backend_NoteDraw(4 /*TRIANGLELIST*/, (unsigned)vertex_count,
                         (unsigned)index_count, 1);   /* [DRAW WATCH] */
        ID3D11DeviceContext_DrawIndexed(ctx, (UINT)index_count, start_index, (INT)base_vertex);
    } else {
        /* Non-indexed draw */
        ID3D11DeviceContext_IASetPrimitiveTopology(ctx,
            D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        Backend_NoteDraw(4 /*TRIANGLELIST*/, (unsigned)vertex_count, 0, 0);  /* [DRAW WATCH] */
        ID3D11DeviceContext_Draw(ctx, (UINT)vertex_count, base_vertex);
    }
}

void td5_plat_render_draw_lines(const TD5_D3DVertex *verts, int vert_count)
{
    if (g_backend.device_removed) return;
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

    Backend_NoteDraw(2 /*LINELIST*/, (unsigned)vert_count, 0, 0);   /* [DRAW WATCH] */
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

void td5_plat_render_draw_tris_flat(const TD5_D3DVertex *verts, int vert_count,
                                    const uint16_t *indices, int index_count)
{
    if (g_backend.device_removed) return;
    if (td5_rcmd_recording()) { td5_rcmd_draw_tris(verts, vert_count, indices, index_count); return; }
    WrapperRecCtx *rc = g_wrapper_rec;
    ID3D11DeviceContext *ctx = rc ? rc->dc : g_backend.context;
    ID3D11Buffer *vb    = rc ? rc->vb : g_backend.dynamic_vb;
    ID3D11Buffer *ib    = rc ? rc->ib : g_backend.dynamic_ib;
    ID3D11Buffer *cbvp  = rc ? rc->cb_viewport : g_backend.cb_viewport;
    ID3D11Buffer *cbfog = rc ? rc->cb_fog : g_backend.cb_fog;
    RenderStateCache *st = rc ? &rc->state : &g_backend.state;
    UINT stride = TD5_VERTEX_STRIDE;
    UINT offset = 0;
    UINT base_vertex = 0, start_index = 0;

    if (!ctx || !verts || vert_count < 3 || !indices || index_count < 3) return;
    if (!g_backend.white_srv) return;

    if (!Backend_StreamUpload(verts, (UINT)vert_count, stride,
                              (const void *)indices, (UINT)index_count,
                              &base_vertex, &start_index))
        return;

    ID3D11DeviceContext_IASetVertexBuffers(ctx, 0, 1, &vb, &stride, &offset);
    ID3D11DeviceContext_IASetInputLayout(ctx, g_backend.input_layout);
    ID3D11DeviceContext_IASetIndexBuffer(ctx, ib, DXGI_FORMAT_R16_UINT, 0);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx,
        D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    ID3D11DeviceContext_VSSetShader(ctx, g_backend.vs_pretransformed, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(ctx, 0, 1, &cbvp);

    /* PS_MODULATE * white texel = vertex color (flat shaded, no source texture). */
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
    /* Z test on (road occludes against sky / itself), Z write off (a flat-color
     * ribbon needs no depth ordering; later opaque actors still draw on top). */
    ID3D11DeviceContext_OMSetDepthStencilState(ctx,
        g_backend.ds_states[DS_Z_ON_WRITE_OFF], 0);
    ID3D11DeviceContext_OMSetBlendState(ctx,
        g_backend.blend_states[BLEND_OPAQUE], NULL, 0xFFFFFFFF);

    Backend_NoteDraw(4 /*TRIANGLELIST flat*/, (unsigned)vert_count,
                     (unsigned)index_count, 1);   /* [DRAW WATCH] */
    ID3D11DeviceContext_DrawIndexed(ctx, (UINT)index_count, start_index, (INT)base_vertex);

    st->current_blend_idx = -1;
    st->current_ds_idx    = -1;
    st->current_samp_idx  = -1;
    st->current_ps_idx    = -1;
    st->dirty = 1;
    if (rc) rc->current_srv = NULL; else g_backend.current_srv = NULL;
    if (!rc) s_last_bound_texture_page = -1;
}

void td5_plat_render_set_preset(TD5_RenderPreset preset)
{
    if (g_backend.device_removed) return;
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
         * pixels"): the sub-50%-alpha fringe taps are alpha-tested away. Type-2
         * pages are binary color-key (0/255 alpha — see td5_asset.c case 2), so
         * opaque texels (alpha=255) survive the strict `< alphaRef` test and,
         * under SRCALPHA/INVSRCALPHA, blend fully opaque. (Historically the bake
         * forced a uniform 0x80 here, making native type-2 geometry render 50%
         * translucent — that misreading of BindRaceTexturePage has been fixed;
         * type 2 differs from type 1 only by z_write=0, not by translucency.) */
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
        s->polygon_offset    = 0;  /* vertex-level view-z pull + the world-Y lift
                                    * (td5_render.c shadow_lift_frac) win the tie;
                                    * a rasterizer pull would shove the shadow over
                                    * kerbs/walls (the regression LEQUAL fixed). */
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
    if (g_backend.device_removed) return;
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
    if (g_backend.device_removed) return;
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

void td5_plat_render_apply_lights(const float cam_pos[3], const float basis9[9],
                                  float focal, float center_x, float center_y,
                                  float vp_x, float vp_y,
                                  float depth_scale, float depth_bias,
                                  const TD5_LightGPU *lights, int light_count,
                                  int occl_steps, float pane_w, float pane_h)
{
    if (g_backend.device_removed) return;
    /* Serial/immediate path only — the deferred light pass composites the whole
     * pane at once, so it must run on the main context (not a pane-recording
     * deferred context). */
    if (td5_rcmd_recording()) return;
    if (!lights || light_count <= 0) return;

    LightCB cb;
    memset(&cb, 0, sizeof(cb));
    cb.camPosFocal[0] = cam_pos[0]; cb.camPosFocal[1] = cam_pos[1]; cb.camPosFocal[2] = cam_pos[2];
    cb.camPosFocal[3] = focal;
    cb.rightCx[0] = basis9[0]; cb.rightCx[1] = basis9[1]; cb.rightCx[2] = basis9[2]; cb.rightCx[3] = center_x;
    cb.upCy[0]    = basis9[3]; cb.upCy[1]    = basis9[4]; cb.upCy[2]    = basis9[5]; cb.upCy[3]    = center_y;
    cb.fwdDepthScale[0] = basis9[6]; cb.fwdDepthScale[1] = basis9[7]; cb.fwdDepthScale[2] = basis9[8];
    cb.fwdDepthScale[3] = depth_scale;

    int n = light_count;
    if (n > LIGHT_MAX_GPU) n = LIGHT_MAX_GPU;
    cb.misc[0] = depth_bias;
    cb.misc[1] = (float)n;
    cb.misc[2] = vp_x;
    cb.misc[3] = vp_y;
    /* [P2] per-light occlusion march config (0 steps = off) */
    cb.ext[0] = (float)occl_steps;
    cb.ext[1] = pane_w;
    cb.ext[2] = pane_h;

    for (int i = 0; i < n; i++) {
        const TD5_LightGPU *L = &lights[i];
        cb.lights[i*3+0][0] = L->x; cb.lights[i*3+0][1] = L->y; cb.lights[i*3+0][2] = L->z; cb.lights[i*3+0][3] = L->range;
        cb.lights[i*3+1][0] = L->r; cb.lights[i*3+1][1] = L->g; cb.lights[i*3+1][2] = L->b; cb.lights[i*3+1][3] = L->intensity;
        cb.lights[i*3+2][0] = L->dx; cb.lights[i*3+2][1] = L->dy; cb.lights[i*3+2][2] = L->dz; cb.lights[i*3+2][3] = L->cone;
    }

    Backend_ApplyLightPass(&cb);
}

void td5_plat_render_set_gbuffer(int on)
{
    if (g_backend.device_removed) return;
    /* Serial/immediate path only (same constraint as the light pass). */
    if (td5_rcmd_recording()) return;
    Backend_SetGBufferEnabled(on);
}

void td5_plat_render_apply_shadow(const float cam_pos[3], const float basis9[9],
                                  float focal, float center_x, float center_y,
                                  float vp_x, float vp_y,
                                  float depth_scale, float depth_bias,
                                  const float sun_dir[3], float strength,
                                  int steps, float max_dist, float thickness,
                                  float start_off, float pane_w, float pane_h)
{
    if (g_backend.device_removed) return;
    /* Serial/immediate path only (same constraint as the light pass). */
    if (td5_rcmd_recording()) return;
    if (!sun_dir || strength <= 0.0f || steps <= 0) return;

    ShadowCB cb;
    memset(&cb, 0, sizeof(cb));
    cb.camPosFocal[0] = cam_pos[0]; cb.camPosFocal[1] = cam_pos[1]; cb.camPosFocal[2] = cam_pos[2];
    cb.camPosFocal[3] = focal;
    cb.rightCx[0] = basis9[0]; cb.rightCx[1] = basis9[1]; cb.rightCx[2] = basis9[2]; cb.rightCx[3] = center_x;
    cb.upCy[0]    = basis9[3]; cb.upCy[1]    = basis9[4]; cb.upCy[2]    = basis9[5]; cb.upCy[3]    = center_y;
    cb.fwdDepthScale[0] = basis9[6]; cb.fwdDepthScale[1] = basis9[7]; cb.fwdDepthScale[2] = basis9[8];
    cb.fwdDepthScale[3] = depth_scale;
    cb.misc[0] = depth_bias;
    cb.misc[1] = vp_x;
    cb.misc[2] = vp_y;
    cb.misc[3] = strength;
    cb.sun[0] = sun_dir[0]; cb.sun[1] = sun_dir[1]; cb.sun[2] = sun_dir[2];
    cb.sun[3] = max_dist;
    cb.params[0] = (float)steps;
    cb.params[1] = thickness;
    cb.params[2] = start_off;
    cb.params[3] = pane_w;
    cb.params2[0] = pane_h;

    Backend_ApplyShadowPass(&cb);
}

void td5_plat_render_apply_ssr(const float cam_pos[3], const float basis9[9],
                               float focal, float center_x, float center_y,
                               float vp_x, float vp_y,
                               float depth_scale, float depth_bias,
                               const float refl8[8], float wet_boost,
                               float intensity, int steps, float max_dist,
                               float thickness, float pane_w, float pane_h)
{
    if (g_backend.device_removed) return;
    /* Serial/immediate path only (same constraint as the light pass). */
    if (td5_rcmd_recording()) return;
    if (!refl8 || intensity <= 0.0f || steps <= 0) return;

    SSRCB cb;
    memset(&cb, 0, sizeof(cb));
    cb.camPosFocal[0] = cam_pos[0]; cb.camPosFocal[1] = cam_pos[1]; cb.camPosFocal[2] = cam_pos[2];
    cb.camPosFocal[3] = focal;
    cb.rightCx[0] = basis9[0]; cb.rightCx[1] = basis9[1]; cb.rightCx[2] = basis9[2]; cb.rightCx[3] = center_x;
    cb.upCy[0]    = basis9[3]; cb.upCy[1]    = basis9[4]; cb.upCy[2]    = basis9[5]; cb.upCy[3]    = center_y;
    cb.fwdDepthScale[0] = basis9[6]; cb.fwdDepthScale[1] = basis9[7]; cb.fwdDepthScale[2] = basis9[8];
    cb.fwdDepthScale[3] = depth_scale;
    cb.misc[0] = depth_bias;
    cb.misc[1] = vp_x;
    cb.misc[2] = vp_y;
    cb.misc[3] = wet_boost;
    cb.params[0] = (float)steps;
    cb.params[1] = max_dist;
    cb.params[2] = thickness;
    cb.params[3] = pane_w;
    cb.params2[0] = pane_h;
    cb.params2[1] = intensity;
    for (int i = 0; i < 4; i++) { cb.reflA[i] = refl8[i]; cb.reflB[i] = refl8[4 + i]; }

    Backend_ApplySSRPass(&cb);
}

int td5_plat_render_upload_texture(int page_index, const void *pixels,
                                    int width, int height, int format)
{
    if (g_backend.device_removed) return 0;
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

/* [DEVICE-LOST recovery] After Backend_RecreateDevice() bumps the device
 * generation, every game texture still points at GPU objects from the removed
 * device. Walk the texture-page registry (which retains each surface's CPU
 * pixel copy in sys_buffer) plus the main scene render target, rebuild their
 * D3D11 backing on the new device and re-upload the pixels, then refresh the
 * cached SRV handle the bind path uses. */
void td5_plat_render_recover_textures(void)
{
    int i, n = 0;

    /* Main scene render target surface is not in the page table; rebuild it so
     * the next frame has a valid RT/SRV to draw into and blit from. */
    if (g_backend.backbuffer)
        WrapperSurface_EnsureDeviceCurrent(g_backend.backbuffer);

    for (i = 0; i < MAX_TEXTURE_PAGES; i++) {
        WrapperSurface *s = s_tex_surfaces[i];
        if (!s) continue;
        WrapperSurface_EnsureDeviceCurrent(s);
        WrapperSurface_FlushDirty(s);   /* re-upload sys_buffer into fresh texture */
        /* The bind path caches the raw SRV pointer as the texture handle. */
        s_tex_handles[i] = (DWORD)(DWORD_PTR)s->d3d11_srv;
        n++;
    }
    TD5_LOG_I(LOG_TAG, "device recovery: rebuilt %d texture pages on new device", n);
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
    if (g_backend.device_removed) return;
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
    if (g_backend.device_removed) return;
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
    if (g_backend.device_removed) return;
    Backend_RecExecute(index);
}
void td5_plat_render_restore_main_rt(void)
{
    if (g_backend.device_removed) return;
    Backend_RestoreMainRenderTarget();
}

void td5_plat_render_clear(uint32_t color)
{
    if (g_backend.device_removed) return;
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

