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

/* Timing */
static LARGE_INTEGER s_qpc_freq;
static int           s_qpc_inited = 0;

/* Window subclass */
static WNDPROC s_original_wndproc = NULL;
static LRESULT CALLBACK TD5_WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

/* Input */
static uint8_t  s_keyboard[256];
static int      s_mouse_dx, s_mouse_dy;
static uint32_t s_mouse_buttons;
static LPDIRECTINPUT8A       s_dinput        = NULL;
static LPDIRECTINPUTDEVICE8A s_di_keyboard   = NULL;
static LPDIRECTINPUTDEVICE8A s_di_mouse      = NULL;
static LPDIRECTINPUTDEVICE8A s_di_joystick   = NULL;
static char s_device_names[16][256];
static int  s_device_types[16];  /* 0=keyboard, 1=gamepad, 2=wheel/joystick */
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

/* Audio */
#define MAX_AUDIO_BUFFERS  256
#define MAX_AUDIO_CHANNELS 64

static LPDIRECTSOUND8          s_dsound      = NULL;
static LPDIRECTSOUNDBUFFER     s_ds_primary  = NULL;
static LPDIRECTSOUNDBUFFER     s_ds_buffers[MAX_AUDIO_BUFFERS];
static LPDIRECTSOUNDBUFFER     s_ds_channels[MAX_AUDIO_CHANNELS];
static int                     s_ds_channel_buf[MAX_AUDIO_CHANNELS]; /* buffer index per channel */
static DWORD                   s_ds_buffer_rates[MAX_AUDIO_BUFFERS];
static int                     s_audio_buf_count = 0;
static int                     s_master_volume   = 40;

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
static TD5_FFState          s_ff;

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
    memset(s_ds_buffer_rates, 0, sizeof(s_ds_buffer_rates));
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

        s_hwnd = CreateWindowExA(0, "TD5RE_Window",
            title ? title : "TD5RE",
            style,
            CW_USEDEFAULT, CW_USEDEFAULT,
            wr.right - wr.left, wr.bottom - wr.top,
            NULL, NULL, GetModuleHandleA(NULL), NULL);

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

void td5_plat_present(int vsync)
{
    static int s_present_log_count = 0;
    float fallback_rgba[4] = { 0.20f, 0.32f, 0.46f, 1.0f };
    int empty_scene_fallback = 0;

    (void)vsync;
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

int td5_plat_apply_display_mode(int width, int height, int bpp)
{
    /* In windowed mode, don't change the desktop resolution.
     * Just record the desired dimensions for the render target. */
    if (!s_fullscreen) {
        s_window_w = width;
        s_window_h = height;
        s_window_bpp = bpp;
        return 1;
    }

    {
        DEVMODEW dm;
        ZeroMemory(&dm, sizeof(dm));
        dm.dmSize = sizeof(dm);
        dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL;
        dm.dmPelsWidth = (DWORD)width;
        dm.dmPelsHeight = (DWORD)height;
        dm.dmBitsPerPel = (DWORD)bpp;

        if (ChangeDisplaySettingsW(&dm, CDS_FULLSCREEN) != DISP_CHANGE_SUCCESSFUL) {
            return 0;
        }
    }

    s_window_w = width;
    s_window_h = height;
    s_window_bpp = bpp;
    return 1;
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
    if (s_di_joystick) {
        IDirectInputDevice8_Unacquire(s_di_joystick);
        IDirectInputDevice8_Release(s_di_joystick);
        s_di_joystick = NULL;
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

    /* Map keyboard to game buttons.
     * Split-screen layout:
     *   P1 (slot 0): Arrow keys + LShift/RShift + Space
     *   P2 (slot 1): WASD + Q/E + Tab
     * Single-player: all keys mapped to slot 0 (backwards compatible). */
    {
        uint32_t bits = 0;

        #define K_DOWN(sc) (s_keyboard[(sc)] & 0x80)

        if (slot == 0) {
            /* Player 1: arrow keys (always), plus WASD in single-player only */
            if (K_DOWN(DIK_LEFT))                     bits |= TD5_INPUT_STEER_LEFT;
            if (K_DOWN(DIK_RIGHT))                    bits |= TD5_INPUT_STEER_RIGHT;
            if (K_DOWN(DIK_UP))                       bits |= TD5_INPUT_THROTTLE;
            if (K_DOWN(DIK_DOWN))                     bits |= TD5_INPUT_BRAKE;
            if (K_DOWN(DIK_LSHIFT))                   bits |= TD5_INPUT_GEAR_DOWN;
            if (K_DOWN(DIK_RSHIFT))                   bits |= TD5_INPUT_GEAR_UP;
            if (K_DOWN(DIK_SPACE))                    bits |= TD5_INPUT_HORN;
        } else {
            /* Player 2: WASD + Q/E */
            if (K_DOWN(DIK_A))                        bits |= TD5_INPUT_STEER_LEFT;
            if (K_DOWN(DIK_D))                        bits |= TD5_INPUT_STEER_RIGHT;
            if (K_DOWN(DIK_W))                        bits |= TD5_INPUT_THROTTLE;
            if (K_DOWN(DIK_S))                        bits |= TD5_INPUT_BRAKE;
            if (K_DOWN(DIK_Q))                        bits |= TD5_INPUT_GEAR_DOWN;
            if (K_DOWN(DIK_E))                        bits |= TD5_INPUT_GEAR_UP;
            if (K_DOWN(DIK_TAB))                      bits |= TD5_INPUT_HORN;
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

    /* Joystick: slot 0 uses first joystick, slot 1 uses second (if available) */
    if (slot == 0 && s_di_joystick) {
        DIJOYSTATE2 js;
        HRESULT hr;
        memset(&js, 0, sizeof(js));
        hr = IDirectInputDevice8_Poll(s_di_joystick);
        if (hr == DIERR_INPUTLOST || hr == DIERR_NOTACQUIRED) {
            IDirectInputDevice8_Acquire(s_di_joystick);
            IDirectInputDevice8_Poll(s_di_joystick);
        }
        hr = IDirectInputDevice8_GetDeviceState(s_di_joystick, sizeof(js), &js);
        if (SUCCEEDED(hr)) {
            out->analog_x = (int16_t)((js.lX - 32768) >> 0);
            out->analog_y = (int16_t)((js.lY - 32768) >> 0);
            if (out->analog_x != 0) out->buttons |= TD5_INPUT_ANALOG_X_FLAG;
            if (out->analog_y != 0) out->buttons |= TD5_INPUT_ANALOG_Y_FLAG;
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
    (void)slot;
    /* Device 0 is keyboard, no joystick device to create */
    if (device_index <= 0) {
        if (s_di_joystick) {
            IDirectInputDevice8_Unacquire(s_di_joystick);
            IDirectInputDevice8_Release(s_di_joystick);
            s_di_joystick = NULL;
        }
        TD5_LOG_I(LOG_TAG, "Input device selected: slot=%d device=%d name=%s",
                  slot, device_index, "Keyboard");
        return;
    }

    /* For joystick devices, re-enumerate and create the requested one */
    /* This is simplified -- a full implementation would store GUIDs */
    (void)device_index;
    TD5_LOG_I(LOG_TAG, "Input device selected: slot=%d device=%d name=%s",
              slot, device_index,
              (device_index >= 0 && device_index < s_device_count) ? s_device_names[device_index] : "<unknown>");
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

static void td5_ff_release_effects(void)
{
    int i;
    for (i = 0; i < 4; i++) {
        if (s_ff.effects[i]) {
            IDirectInputEffect_Stop(s_ff.effects[i]);
            IDirectInputEffect_Release(s_ff.effects[i]);
            s_ff.effects[i] = NULL;
        }
    }
}

static int td5_ff_create_constant_effect(int slot, DWORD axis, LONG direction)
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

    ok = SUCCEEDED(IDirectInputDevice8_CreateEffect(s_ff.device,
        &s_ff.effectGuid, &eff, &s_ff.effects[slot], NULL));
    TD5_LOG_I(LOG_TAG, "FF effect create constant: slot=%d axis=%lu direction=%ld result=%s",
              slot, (unsigned long)axis, (long)direction, ok ? "ok" : "failed");
    return ok;
}

static int td5_ff_create_periodic_effect(int slot)
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

    ok = SUCCEEDED(IDirectInputDevice8_CreateEffect(s_ff.device,
        &GUID_Sine, &eff, &s_ff.effects[slot], NULL));
    TD5_LOG_I(LOG_TAG, "FF effect create periodic: slot=%d axis=%lu result=%s",
              slot, (unsigned long)axis, ok ? "ok" : "failed");
    return ok;
}

int td5_plat_ff_init(int device_index)
{
    DIDEVCAPS caps;

    (void)device_index;

    if (!s_di_joystick) {
        return 0;
    }

    ZeroMemory(&s_ff, sizeof(s_ff));
    s_ff.device = (IDirectInputDevice8 *)s_di_joystick;
    s_ff.axes[0] = DIJOFS_X;
    s_ff.axes[1] = DIJOFS_Y;
    s_ff.directions[0] = 0;
    s_ff.directions[1] = 0;

    ZeroMemory(&caps, sizeof(caps));
    caps.dwSize = sizeof(caps);
    if (FAILED(IDirectInputDevice8_GetCapabilities(s_ff.device, &caps)) ||
        !(caps.dwFlags & DIDC_FORCEFEEDBACK)) {
        ZeroMemory(&s_ff, sizeof(s_ff));
        return 0;
    }

    s_ff.is_wheel = ((GET_DIDEVICE_TYPE(caps.dwDevType) == DI8DEVTYPE_DRIVING) ||
                     (GET_DIDEVICE_TYPE(caps.dwDevType) == DI8DEVTYPE_1STPERSON));

    IDirectInputDevice8_Acquire(s_ff.device);

    if (FAILED(IDirectInputDevice8_EnumEffects(s_ff.device,
            EnumConstantForceEffectsCallback, &s_ff.effectGuid, DIEFT_ALL)) ||
        !IsEqualGUID(&s_ff.effectGuid, &GUID_ConstantForce)) {
        ZeroMemory(&s_ff, sizeof(s_ff));
        return 0;
    }

    if (!td5_ff_create_constant_effect(0, s_ff.axes[0], 0) ||
        !td5_ff_create_constant_effect(1, s_ff.axes[1], 9000) ||
        !td5_ff_create_constant_effect(2, s_ff.axes[0], 0) ||
        !td5_ff_create_periodic_effect(3)) {
        td5_ff_release_effects();
        ZeroMemory(&s_ff, sizeof(s_ff));
        return 0;
    }

    return 1;
}

void td5_plat_ff_shutdown(void)
{
    td5_ff_release_effects();
    ZeroMemory(&s_ff, sizeof(s_ff));
}

void td5_plat_ff_constant(int slot, int magnitude)
{
    HRESULT hr;

    if (slot < 0 || slot >= 4 || !s_ff.effects[slot]) {
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
        eff.rgdwAxes = &s_ff.axes[0];
        eff.rglDirection = &direction;
        eff.cbTypeSpecificParams = sizeof(periodic);
        eff.lpvTypeSpecificParams = &periodic;

        hr = IDirectInputEffect_SetParameters(s_ff.effects[slot], &eff,
            DIEP_AXES | DIEP_DIRECTION | DIEP_TYPESPECIFICPARAMS);
    } else {
        DICONSTANTFORCE cf;
        DIEFFECT eff;
        LONG direction = (magnitude >= 0) ? 0 : 18000;
        DWORD axis = (slot == 1) ? s_ff.axes[1] : s_ff.axes[0];

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

        hr = IDirectInputEffect_SetParameters(s_ff.effects[slot], &eff,
            DIEP_AXES | DIEP_DIRECTION | DIEP_TYPESPECIFICPARAMS);
    }

    if (SUCCEEDED(hr)) {
        IDirectInputEffect_Start(s_ff.effects[slot], 1, 0);
    }
}

void td5_plat_ff_stop(int slot)
{
    if (slot < 0 || slot >= 4 || !s_ff.effects[slot]) {
        return;
    }
    IDirectInputEffect_Stop(s_ff.effects[slot]);
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
    }

    s_audio_buf_count = 0;
    ZeroMemory(s_ds_buffer_rates, sizeof(s_ds_buffer_rates));
    return 1;
}

void td5_plat_audio_shutdown(void)
{
    int i;

    td5_plat_audio_stream_stop();

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

    /* Create sound buffer */
    ZeroMemory(&desc, sizeof(desc));
    desc.dwSize          = sizeof(DSBUFFERDESC);
    desc.dwFlags         = DSBCAPS_CTRLVOLUME | DSBCAPS_CTRLPAN | DSBCAPS_CTRLFREQUENCY
                         | DSBCAPS_STATIC;
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

static int find_free_channel(void)
{
    int i;
    /* First pass: find an empty slot */
    for (i = 0; i < MAX_AUDIO_CHANNELS; i++) {
        if (!s_ds_channels[i]) return i;
    }
    /* Second pass: find a stopped channel */
    for (i = 0; i < MAX_AUDIO_CHANNELS; i++) {
        DWORD status = 0;
        if (s_ds_channels[i]) {
            IDirectSoundBuffer_GetStatus(s_ds_channels[i], &status);
            if (!(status & DSBSTATUS_PLAYING)) {
                IDirectSoundBuffer_Release(s_ds_channels[i]);
                s_ds_channels[i] = NULL;
                return i;
            }
        }
    }
    return -1;
}

/** Convert 0-100 linear volume to DirectSound's logarithmic dB scale.
 *  DS range: DSBVOLUME_MIN (-10000) to DSBVOLUME_MAX (0). */
static LONG vol_to_ds(int volume)
{
    if (volume <= 0) return DSBVOLUME_MIN;
    if (volume >= 100) return DSBVOLUME_MAX;
    /* Approximate log scale: -100 * (100 - volume) */
    return (LONG)(-50 * (100 - volume));
}

/** Convert -100..+100 pan to DS pan range (-10000..+10000). */
static LONG pan_to_ds(int pan)
{
    if (pan < -100) pan = -100;
    if (pan > 100)  pan = 100;
    return (LONG)(pan * 100);
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
    if (!src_buf) return TD5_AUDIO_INVALID_CHANNEL;

    ch = find_free_channel();
    if (ch < 0) return TD5_AUDIO_INVALID_CHANNEL;

    /* Duplicate the buffer so multiple instances can play simultaneously */
    hr = IDirectSound8_DuplicateSoundBuffer(s_dsound, src_buf, &dup_buf);
    if (FAILED(hr) || !dup_buf) return TD5_AUDIO_INVALID_CHANNEL;

    s_ds_channels[ch] = dup_buf;
    s_ds_channel_buf[ch] = buffer_index;

    /* Apply parameters */
    {
        int effective_vol = (volume * s_master_volume) / 100;
        IDirectSoundBuffer_SetVolume(dup_buf, vol_to_ds(effective_vol));
    }
    IDirectSoundBuffer_SetPan(dup_buf, pan_to_ds(pan));
    if (frequency > 0)
        IDirectSoundBuffer_SetFrequency(dup_buf, td5_audio_translate_frequency(buffer_index, frequency));

    IDirectSoundBuffer_SetCurrentPosition(dup_buf, 0);
    IDirectSoundBuffer_Play(dup_buf, 0, 0, loop ? DSBPLAY_LOOPING : 0);

    TD5_LOG_I(LOG_TAG, "Audio channel play: channel=%d buffer=%d loop=%d",
              ch, buffer_index, loop);

    return (TD5_AudioChannel)ch;
}

void td5_plat_audio_stop(TD5_AudioChannel ch)
{
    if (ch < 0 || ch >= MAX_AUDIO_CHANNELS) return;
    if (s_ds_channels[ch]) {
        TD5_LOG_I(LOG_TAG, "Audio channel stop: channel=%d buffer=%d",
                  ch, s_ds_channel_buf[ch]);
        IDirectSoundBuffer_Stop(s_ds_channels[ch]);
        IDirectSoundBuffer_Release(s_ds_channels[ch]);
        s_ds_channels[ch] = NULL;
    }
}

void td5_plat_audio_modify(TD5_AudioChannel ch, int volume, int pan, int frequency)
{
    LPDIRECTSOUNDBUFFER buf;
    if (ch < 0 || ch >= MAX_AUDIO_CHANNELS) return;
    buf = s_ds_channels[ch];
    if (!buf) return;

    {
        int effective_vol = (volume * s_master_volume) / 100;
        IDirectSoundBuffer_SetVolume(buf, vol_to_ds(effective_vol));
    }
    IDirectSoundBuffer_SetPan(buf, pan_to_ds(pan));
    if (frequency > 0)
        IDirectSoundBuffer_SetFrequency(buf, td5_audio_translate_frequency(s_ds_channel_buf[ch], frequency));
}

int td5_plat_audio_is_playing(TD5_AudioChannel ch)
{
    DWORD status = 0;
    if (ch < 0 || ch >= MAX_AUDIO_CHANNELS) return 0;
    if (!s_ds_channels[ch]) return 0;
    IDirectSoundBuffer_GetStatus(s_ds_channels[ch], &status);
    return (status & DSBSTATUS_PLAYING) ? 1 : 0;
}

void td5_plat_audio_set_master_volume(int volume)
{
    if (volume < 0)   volume = 0;
    if (volume > 100) volume = 100;
    s_master_volume = volume;
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
        /* Force correct PS and sampler before every draw */
        ID3D11DeviceContext_PSSetShader(ctx,
            g_backend.ps_shaders[g_backend.state.texblend_mode == 5 ? 1 : 0], NULL, 0);
        {
            int si = (g_backend.state.mag_filter >= 2) ? SAMP_LINEAR_WRAP : SAMP_POINT_WRAP;
            ID3D11DeviceContext_PSSetSamplers(ctx, 0, 1, &g_backend.sampler_states[si]);
        }
        ID3D11DeviceContext_DrawIndexed(ctx, (UINT)index_count, 0, 0);
    } else {
        /* Non-indexed draw */
        ID3D11DeviceContext_IASetPrimitiveTopology(ctx,
            D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D11DeviceContext_Draw(ctx, (UINT)vertex_count, 0);
    }
}

void td5_plat_render_set_preset(TD5_RenderPreset preset)
{
    RenderStateCache *s = &g_backend.state;

    switch (preset) {
    case TD5_PRESET_OPAQUE_ANISO:
        s->blend_enable = 0;
        s->z_enable     = 1;
        s->z_write      = 1;
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
        s->mag_filter   = 2;
        s->min_filter   = 2;
        s->texblend_mode = D3DTBLEND_MODULATEALPHA;
        s->alpha_test_enable = 1;
        /* alpha_ref = 0x80 (1-bit-alpha cutoff): with bilinear sampling on
         * color-keyed textures the edge taps produce alphas in (0,255).
         * Discarding < 128 prunes the dark/black fringes around transparent
         * pixels (street-light backgrounds, props). Type-2 semi-transparent
         * pages sit at exactly 0x80 — strict < keeps them. */
        s->alpha_ref         = 0x80;
        break;

    case TD5_PRESET_TRANSLUCENT_ANISO:
        s->blend_enable = 1;
        s->src_blend    = D3D6BLEND_SRCALPHA;
        s->dest_blend   = D3D6BLEND_INVSRCALPHA;
        s->z_enable     = 1;
        s->z_write      = 0;
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
        s->mag_filter   = 0; /* POINT — no bilinear bleed into transparent border pixels */
        s->min_filter   = 0;
        s->texblend_mode = D3DTBLEND_MODULATEALPHA;
        s->alpha_test_enable = 1;
        s->alpha_ref         = 1;
        break;

    case TD5_PRESET_ADDITIVE:
        /* Type-3 tpages — street-light sprites, headlight glows, additive
         * particles. Original BindRaceTexturePage @ 0x0040B660 case 3 set
         * SRCBLEND=ONE / DESTBLEND=ONE, but the original ran at 16bpp
         * R5G6B5 where the narrower channel range tempered the blow-out.
         * Running that at 32bpp in D3D11 pushes every lit pixel toward
         * fully saturated white.
         *
         * SRCALPHA/ONE gives the same hard-cut darkness for background
         * pixels (combined with alpha_test ref=16) but scales the additive
         * contribution of lit pixels by their own brightness (alpha =
         * max(r,g,b) after upload). Visual: a soft glow that adds most at
         * the bright core and trails off at the edges — matches the
         * period art better than a flat ONE/ONE splat. */
        s->blend_enable      = 1;
        s->src_blend         = D3D6BLEND_SRCALPHA;
        s->dest_blend        = D3D6BLEND_ONE;
        s->z_enable          = 1;
        s->z_write           = 1;
        s->mag_filter        = 2; /* LINEAR */
        s->min_filter        = 2;
        s->texblend_mode     = D3DTBLEND_MODULATE;
        s->alpha_test_enable = 1;
        s->alpha_ref         = 16;
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

            for (size_t i = 0; i < pixel_count; i++) {
                uint8_t b = src32[i * 4 + 0];
                uint8_t g = src32[i * 4 + 1];
                uint8_t r = src32[i * 4 + 2];
                uint8_t a = src32[i * 4 + 3];

                if (apply_font_colorkey) {
                    a = (((unsigned int)r + (unsigned int)g + (unsigned int)b) < 16u) ? 0 : 255;
                }

                dst32[i * 4 + 0] = b;
                dst32[i * 4 + 1] = g;
                dst32[i * 4 + 2] = r;
                dst32[i * 4 + 3] = a;
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

void td5_plat_log(TD5_LogLevel level, const char *module, const char *fmt, ...)
{
    char buf[2048];
    va_list ap;
    int off, cat;
    DWORD written;
    TD5_LogFile *lf;

    if (!fmt) return;

    /* Skip DEBUG messages at runtime for performance */
    if (level < TD5_LOG_INFO) return;

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
