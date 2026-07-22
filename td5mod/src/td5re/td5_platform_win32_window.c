/**
 * td5_platform_win32_window.c -- Window/Display + Timing platform primitives
 * (C9 module split, fourth slice, see REFACTOR_PLAN.md).
 *
 * Extracted verbatim from td5_platform_win32.c's "Window / Display" and
 * "Timing" sections, plus the module-level window/timing/wndproc-subclass
 * statics they own. td5_platform_win32_init() (staying in td5_platform_win32.c)
 * still seeds the window/mode/resolution fields and wires up the WndProc
 * subclass, so those symbols (window dims/mode/fullscreen/chosen-res,
 * s_primary, s_original_wndproc, TD5_WndProc, td5_load_app_icon) are shared
 * back via td5_platform_internal.h -- same pattern as the audio-device slice.
 * (td5_platform_win32_init()'s redundant QPC primer was removed, not shared:
 * td5_plat_time_us() below already lazily initializes s_qpc_freq/inited on
 * first call, so priming it a second time in Init was always a no-op.)
 * The one reverse-direction read (td5_plat_present's texture-page/frame-stat
 * lookups) is also bridged through td5_platform_internal.h.
 */

#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <dinput.h>
#include <d3d11.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>   /* frame-dump PNG encoder (dev tool, see td5_plat_dump_frame_png) */

#include "td5_platform.h"
#include "td5_platform_internal.h"
#include "td5_config.h"
#include "td5_rcmd.h"
#include "td5_render.h"  /* g_render_width/_height(+_f) resize cache */
#include "td5re.h"       /* td5re_set_render_dims */

/* Pull in the wrapper types and backend access */
#include "../../ddraw_wrapper/src/wrapper.h"

#define LOG_TAG "platform"
#include "td5_color.h"

/* Module-level state moved from td5_platform_win32.c -- owned here now. */
WrapperSurface      *s_primary       = NULL;

int      s_window_w      = 640;
int      s_window_h      = 480;
int      s_window_bpp    = 16;
int      s_fullscreen    = 0;
/* [S01 Display options 2026-06-04] 3-way window mode + the user's chosen
 * windowed/fullscreen resolution. Borderless overrides the render size to the
 * desktop; windowed and exclusive-fullscreen render at the chosen resolution. */
int      s_window_mode   = 1;   /* 0=fullscreen exclusive, 1=windowed, 2=borderless */
int      s_chosen_w      = 0;   /* last resolution picked in Display options */
int      s_chosen_h      = 0;
/* Re-entrancy guard so the WM_SIZE-driven live resize ignores our own
 * programmatic SetWindowPos inside td5_plat_set_window_mode (which already
 * resizes the swap chain explicitly). */
static int      s_suppress_wm_size = 0;
static int      plat_resize_native(int width, int height, int bpp, int do_swap_reset);

static LARGE_INTEGER s_qpc_freq;
static int           s_qpc_inited = 0;

WNDPROC s_original_wndproc = NULL;

/* ========================================================================
 * Window / Display
 * ======================================================================== */

uint32_t s_mouse_click_latch = 0; /* sticky per-button click flags */

/* WM_CHAR ring buffer for robust text input. Windows queues every typed
 * character (with correct shift/caps/repeat) and we drain it once per frame, so
 * input is never dropped by frame-rate spikes or GetAsyncKeyState '&1' polling
 * contention (the old text path missed keys whenever the menu's other
 * GetAsyncKeyState calls consumed the "pressed since last call" bit first). */
#define TD5_CHAR_QUEUE_SIZE 128
volatile unsigned char s_char_queue[TD5_CHAR_QUEUE_SIZE];
volatile int s_char_q_head;   /* producer (window proc) */
volatile int s_char_q_tail;   /* consumer (frontend)    */

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
volatile unsigned char s_nav_queue[TD5_NAV_QUEUE_SIZE];
volatile int s_nav_q_head;   /* producer (window proc) */
volatile int s_nav_q_tail;   /* consumer (frontend)    */
/* ESC stays a one-shot latch rather than a FIFO entry: a back-out is applied at
 * most once per frame so a buffered burst can't pop several screens at once. */
volatile int s_esc_latch = 0;

/* [PERF FIX 2026-06-05] Set by WM_DEVICECHANGE; consumed by
 * td5_plat_input_devices_changed(). Lets the frontend run the ~120ms blocking
 * EnumDevices rescan ONLY when the OS signals a USB device change, instead of
 * polling it every ~90 frames (the dominant frontend frame-time spike). */
volatile int s_devices_dirty = 0;

LRESULT CALLBACK TD5_WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
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
static unsigned s_fd_gen = 0;   /* device generation the staging texture belongs to */

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
    /* [DEVICE-LOST 2026-07-20] Also recreate when the device generation changed:
     * after a TDR recovery a same-resolution staging texture from the old device
     * would mix a dead-device resource with the new-device context in CopyResource. */
    if (!s_fd_staging || s_fd_w != (int)d.Width || s_fd_h != (int)d.Height ||
        s_fd_gen != g_backend.device_generation) {
        D3D11_TEXTURE2D_DESC sd = d;
        if (s_fd_staging) { ID3D11Texture2D_Release(s_fd_staging); s_fd_staging = NULL; }
        sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0;
        if (FAILED(ID3D11Device_CreateTexture2D(g_backend.device,&sd,NULL,&s_fd_staging))) {
            ID3D11Texture2D_Release(bb); return;
        }
        s_fd_w = (int)d.Width; s_fd_h = (int)d.Height;
        s_fd_gen = g_backend.device_generation;
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

/* Runtime one-shot frame-dump request (live-control `framedump` verb).
 * Latched on the main thread (td5_control_tick), serviced at the next
 * present by the same td5_plat_dump_frame_png path as the env dump. */
static char s_fd_request[300];

void td5_plat_request_frame_dump(const char *path)
{
    if (!path || !path[0]) return;
    strncpy(s_fd_request, path, sizeof(s_fd_request) - 1);
    s_fd_request[sizeof(s_fd_request) - 1] = '\0';
}

/* [DEVICE-LOST 2026-07-13] The STANDALONE present path (this file) calls
 * IDXGISwapChain_Present directly on several branches -- NOT through the
 * wrapper's Backend_CompositeAndPresent -- so its Present HRESULTs were never
 * checked for device-lost. Races (the crash-prone path) present here. Route
 * every direct present through this helper so a TDR is caught + latched
 * (Backend_NoteDeviceRemoved logs GetDeviceRemovedReason to
 * log/gpu_device_lost.log) instead of cascading into the driver NULL-deref
 * (0xC0000005) that killed the process. */
static void plat_present_swapchain(int sync)
{
#ifndef TD5RE_RELEASE
    /* Dev fault injection: TD5RE_FORCE_DEVICE_LOST=N simulates a
     * DEVICE_REMOVED on present frame N to exercise the survival path with no
     * real GPU hang. Compiled out of release. */
    {
        static int s_fdl_trip = -2;      /* -2 = unread env, -1 = disabled */
        static unsigned s_fdl_frame = 0;
        if (s_fdl_trip == -2) {
            const char *e = getenv("TD5RE_FORCE_DEVICE_LOST");
            s_fdl_trip = (e && e[0]) ? atoi(e) : -1;
        }
        if (s_fdl_trip >= 0 && (int)(s_fdl_frame++) >= s_fdl_trip) {
            Backend_NoteDeviceRemoved(DXGI_ERROR_DEVICE_REMOVED,
                                      "td5_plat_present/forced-test");
            s_fdl_trip = -1;   /* trip once — let recovery bring the device back */
            return;
        }
    }
#endif
    /* [crash-diag 2026-07-22] Mark the ring so a fault INSIDE Present shows as
     * a trailing PRESENT entry (vs a scene draw) in the crash dump. */
    Backend_NotePresent();
    HRESULT hr = IDXGISwapChain_Present(g_backend.swap_chain, sync, 0);
    g_backend.present_count++;   /* [diag] frame counter for device-lost forensics */
    if (FAILED(hr)) Backend_NoteDeviceRemoved(hr, "td5_plat_present/Present");
}

void td5_plat_set_diag_context(const char *ctx)
{
    if (!ctx) { g_backend.diag_context[0] = '\0'; return; }
    snprintf(g_backend.diag_context, sizeof(g_backend.diag_context), "%s", ctx);
}

int td5_plat_device_lost(void)
{
    return g_backend.device_removed;
}

/* [crash-diag 2026-07-21] Seam so the exe SEH crash handler (main.c) can append
 * GPU forensics to crash.log without pulling in wrapper headers. Safe from an
 * AV handler: reads only VALUES from g_backend + the recent-draw ring. */
void td5_plat_dump_gpu_crash_diag(const char *path)
{
    Backend_DumpCrashDiag(path);
}

/* [DEVICE-LOST recovery] Try to bring the GPU back after a TDR removed the
 * device. Rate-limited (recreating a device is expensive and the driver may
 * need a moment after a TDR) and capped so a permanently dead adapter (physical
 * removal, driver uninstall) doesn't spin D3D11CreateDevice forever. On success
 * Backend_RecreateDevice clears g_backend.device_removed and the game's texture
 * pages are rebuilt from their retained CPU copies. */
static void plat_try_recover_device(void)
{
    static int s_attempts = 0;
    static int s_cooldown = 0;

    if (!g_backend.device_removed) { s_attempts = 0; s_cooldown = 0; return; }
    if (s_attempts >= 240) return;              /* give up on a truly dead adapter */
    if (s_cooldown > 0) { s_cooldown--; return; }
    s_cooldown = 30;                            /* retry ~every 30 frames */
    s_attempts++;

    TD5_LOG_W(LOG_TAG, "device lost: recovery attempt %d", s_attempts);
    if (Backend_RecreateDevice()) {
        td5_plat_render_recover_textures();
        TD5_LOG_W(LOG_TAG, "device recovered after %d attempt(s)", s_attempts);
        s_attempts = 0;
        s_cooldown = 0;
    }
}

void td5_plat_present(int vsync)
{
    static int s_present_log_count = 0;
    float fallback_rgba[4] = { 0.20f, 0.32f, 0.46f, 1.0f };
    int empty_scene_fallback = 0;

    /* [D3D DEBUG 2026-07-19] Flush the debug-layer InfoQueue for the frame just
     * drawn to log/gpu_d3d_debug.log (crash-safe: fopen+fflush+fclose per drain)
     * so the messages immediately preceding a driver crash are on disk. No-op
     * unless TD5RE_D3D_DEBUG=1. */
    Backend_DrainD3DDebug("present");

    /* [DEVICE-LOST recovery] On a removed device, attempt recovery instead of
     * presenting a frame that was drawn as no-ops on the dead device. Whether or
     * not the attempt succeeds this call, skip present; the next frame renders
     * cleanly on the recovered device (device_removed is cleared on success). */
    if (g_backend.device_removed) {
        plat_try_recover_device();
        return;
    }

    /* [FRAME CAP 2026-07-19] When the present is effectively unsynced (VSync off,
     * or a bypassed present path), pace the frame loop to a sane maximum so the
     * GPU isn't flooded with tens of thousands of back-to-back submissions per
     * second. That submission storm — the results/frontend screen was spinning
     * at ~35k FPS — is what drove the GPU into a TDR (DEVICE_HUNG) at the
     * race->results transition; capping it prevents the hang the device-lost
     * recovery path only recovers from. No effect when VSync is on (the vblank
     * wait already paces). Knob TD5RE_FRAME_CAP=<fps> overrides; 0 = uncapped
     * (e.g. benchmarking). Default 300 FPS is ~100x below the rate that hung the
     * GPU and is imperceptible for gameplay and menus. */
    {
        static int s_cap_fps = -1;            /* -1 = unread env */
        static uint64_t s_cap_last_us = 0;
        if (s_cap_fps < 0) {
            const char *e = getenv("TD5RE_FRAME_CAP");
            s_cap_fps = (e && e[0]) ? atoi(e) : 300;
            if (s_cap_fps < 0) s_cap_fps = 0;
        }
        if (s_cap_fps > 0 && !(vsync && g_backend.vsync)) {
            uint64_t min_dt = 1000000ULL / (uint64_t)s_cap_fps;
            uint64_t now = td5_plat_time_us();
            if (s_cap_last_us != 0 && now > s_cap_last_us &&
                (now - s_cap_last_us) < min_dt) {
                uint64_t remain = min_dt - (now - s_cap_last_us);
                /* Sleep the bulk (leave ~1ms) to yield the CPU, then spin the
                 * remainder — Sleep granularity is ~1ms even with
                 * timeBeginPeriod(1), too coarse for exact pacing alone. */
                if (remain > 1500) td5_plat_sleep((uint32_t)((remain - 1000) / 1000));
                do { now = td5_plat_time_us(); } while (now - s_cap_last_us < min_dt);
            }
            s_cap_last_us = now;
        }
    }

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

    /* One-shot runtime dump request (live-control `framedump` verb). */
    if (s_fd_request[0]) {
        td5_plat_dump_frame_png(s_fd_request);
        TD5_LOG_I("plat", "frame dumped -> %s", s_fd_request);
        s_fd_request[0] = '\0';
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
        plat_present_swapchain((vsync && g_backend.vsync) ? 1 : 0);
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
        plat_present_swapchain((vsync && g_backend.vsync) ? 1 : 0);

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
        plat_present_swapchain((vsync && g_backend.vsync) ? 1 : 0);
    }
}

void td5_plat_present_texture_page(int page_index, int vsync)
{
    if (g_backend.device_removed) return;
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
    plat_present_swapchain((vsync && g_backend.vsync) ? 1 : 0);

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

