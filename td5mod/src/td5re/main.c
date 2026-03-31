/**
 * main.c -- Standalone entry point for TD5RE source port
 *
 * Bootstraps the D3D11 backend without M2DX/DirectDraw, initializes
 * the platform layer and all game modules, then runs the main loop.
 *
 * Initialization sequence:
 *   1. Backend_Init()          -- enumerate display modes, init PNG overrides
 *   2. Backend_CreateDevice()  -- create window, D3D11 device, swap chain,
 *                                 shaders, state objects, dynamic buffers
 *   3. Create wrapper COM objects (WrapperDirectDraw, WrapperDevice,
 *      WrapperSurface) so the platform layer has valid interface pointers
 *   4. td5_platform_win32_init() -- hand COM pointers to the platform layer
 *   5. td5re_init()            -- initialize all 15 game modules
 *   6. Main loop: PeekMessage pump + td5re_frame() each iteration
 *   7. td5re_shutdown()        -- shutdown all modules in reverse order
 *   8. Backend_Shutdown()      -- release all D3D11 resources
 */

#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <stdio.h>

/* Debug log to file + OutputDebugString */
static FILE *s_logfile = NULL;
static void dbglog(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    OutputDebugStringA(buf);
    OutputDebugStringA("\n");
    if (!s_logfile) s_logfile = fopen("td5re_debug.log", "w");
    if (s_logfile) { fprintf(s_logfile, "%s\n", buf); fflush(s_logfile); }
}

/* TD5RE headers */
#include "td5re.h"
#include "td5_platform.h"

/* Wrapper backend types and functions */
#include "../../ddraw_wrapper/src/wrapper.h"

/* ========================================================================
 * External declarations
 *
 * td5_platform_win32_init is not in td5_platform.h (it is Win32-specific).
 * The wrapper object creation functions are in wrapper.h.
 * ======================================================================== */

extern void td5_platform_win32_init(void *ddraw4, void *d3ddevice3,
                                    void *primary_surface);

/* ========================================================================
 * Default display configuration
 * ======================================================================== */

#define DEFAULT_WIDTH   640
#define DEFAULT_HEIGHT  480
#define DEFAULT_BPP     32
#define DEFAULT_WINDOWED 1

/* ========================================================================
 * WinMain
 * ======================================================================== */

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                   LPSTR lpCmdLine, int nCmdShow)
{
    WrapperDirectDraw *ddraw    = NULL;
    WrapperDevice     *device   = NULL;
    WrapperSurface    *primary  = NULL;
    WrapperSurface    *backbuf  = NULL;
    MSG msg;
    int running = 1;
    int width   = DEFAULT_WIDTH;
    int height  = DEFAULT_HEIGHT;
    int bpp     = DEFAULT_BPP;
    int windowed = DEFAULT_WINDOWED;

    (void)hInstance;
    (void)hPrevInstance;
    (void)lpCmdLine;
    (void)nCmdShow;

    /* ---------------------------------------------------------------
     * Step 1: Initialize backend (display mode enumeration, PNG init)
     * --------------------------------------------------------------- */
    dbglog("=== TD5RE Standalone Start ===");
    dbglog("Step 1: Backend_Init...");
    if (!Backend_Init()) {
        MessageBoxA(NULL, "Backend_Init failed.\n\n"
                    "Ensure D3D11 is available on this system.",
                    "TD5RE - Fatal Error", MB_OK | MB_ICONERROR);
        return 1;
    }

    /* ---------------------------------------------------------------
     * Step 2: Create D3D11 device, swap chain, window, shaders, etc.
     *
     * In the wrapper DLL flow, Backend_CreateDevice is called from
     * WrapperDirectDraw::SetCooperativeLevel. For standalone mode,
     * we call it directly with a NULL hwnd (it will create its own
     * display window in windowed mode).
     * --------------------------------------------------------------- */
    dbglog("Step 1: Backend_Init OK");
    dbglog("Step 2: Backend_CreateDevice(%d x %d, bpp=%d, windowed=%d)...", width, height, bpp, windowed);
    fflush(s_logfile);  /* ensure log is written before potential crash */
    if (!Backend_CreateDevice(NULL, width, height, bpp, windowed)) {
        MessageBoxA(NULL, "Backend_CreateDevice failed.\n\n"
                    "Could not create D3D11 device and swap chain.",
                    "TD5RE - Fatal Error", MB_OK | MB_ICONERROR);
        Backend_Shutdown();
        return 1;
    }

    /* ---------------------------------------------------------------
     * Step 3: Create wrapper COM objects
     *
     * The platform layer (td5_platform_win32.c) expects valid
     * WrapperDirectDraw, WrapperDevice, and WrapperSurface pointers.
     * These provide the COM vtable interface that the render module
     * calls through for texture management, surface operations, etc.
     * --------------------------------------------------------------- */

    /* Mark standalone mode so wrapper skips all hardcoded address fixups */
    g_backend.standalone = 1;
    g_backend.vertex_scale_x = 1.0f;
    g_backend.vertex_scale_y = 1.0f;

    /* NOTE: g_backend.hwnd intentionally left as NULL in standalone mode.
     * Setting it to s_display_hwnd would cause DisplayWindowProc to forward
     * keyboard messages back to the display window (infinite loop).
     * Instead, td5_platform_win32_init reads Backend_GetDisplayWindow()
     * directly for audio/input cooperative level calls. */

    /* Read actual RT dimensions (backend may have overridden with INI/native) */
    width  = g_backend.target_width  > 0 ? g_backend.target_width  : g_backend.width;
    height = g_backend.target_height > 0 ? g_backend.target_height : g_backend.height;
    if (width <= 0)  width  = 640;
    if (height <= 0) height = 480;

    dbglog("Step 2: Backend_CreateDevice OK (hwnd=%p, ctx=%p, swap=%p) RT=%dx%d",
           (void*)g_backend.hwnd, (void*)g_backend.context, (void*)g_backend.swap_chain,
           width, height);
    dbglog("Step 3: Creating wrapper COM objects...");
    /* Create WrapperDirectDraw (IDirectDraw4 wrapper) */
    ddraw = WrapperDirectDraw_Create();
    if (!ddraw) {
        MessageBoxA(NULL, "WrapperDirectDraw_Create failed.",
                    "TD5RE - Fatal Error", MB_OK | MB_ICONERROR);
        Backend_Shutdown();
        return 1;
    }
    g_backend.ddraw = ddraw;

    /* Create primary surface (front buffer wrapper) */
    primary = WrapperSurface_Create(
        (DWORD)g_backend.width, (DWORD)g_backend.height,
        (DWORD)g_backend.bpp,
        0x200 /* DDSCAPS_PRIMARYSURFACE */);
    if (!primary) {
        MessageBoxA(NULL, "WrapperSurface_Create (primary) failed.",
                    "TD5RE - Fatal Error", MB_OK | MB_ICONERROR);
        Backend_Shutdown();
        return 1;
    }
    g_backend.primary = primary;

    /* Create back buffer surface.
     * DDSCAPS_OFFSCREENPLAIN | DDSCAPS_3DDEVICE (0x2040) creates a full D3D11
     * render target with texture + SRV + RTV, which is required for:
     *   - td5_plat_render_clear to clear the backbuffer RTV
     *   - Backend_CompositeAndPresent to blit backbuffer -> swap chain
     *   - td5_plat_render_draw_tris to render 3D geometry into the backbuffer */
    backbuf = WrapperSurface_Create(
        (DWORD)g_backend.width, (DWORD)g_backend.height,
        (DWORD)g_backend.bpp,
        DDSCAPS_OFFSCREENPLAIN | DDSCAPS_3DDEVICE);
    if (backbuf) {
        g_backend.backbuffer = backbuf;
    }

    /* Create WrapperDevice (IDirect3DDevice3 wrapper) */
    device = WrapperDevice_Create(backbuf ? backbuf : primary);
    if (!device) {
        MessageBoxA(NULL, "WrapperDevice_Create failed.",
                    "TD5RE - Fatal Error", MB_OK | MB_ICONERROR);
        Backend_Shutdown();
        return 1;
    }
    g_backend.d3ddevice = device;

    /* ---------------------------------------------------------------
     * Step 4: Initialize the Win32 platform layer
     *
     * This hands the COM interface pointers to td5_platform_win32.c
     * so that all td5_plat_* functions can operate. It also copies
     * g_backend.hwnd, width, height, bpp into the platform state.
     * --------------------------------------------------------------- */
    dbglog("Step 3: COM objects OK (ddraw=%p, device=%p, primary=%p)", (void*)ddraw, (void*)device, (void*)primary);
    dbglog("Step 4: td5_platform_win32_init...");
    td5_platform_win32_init(ddraw, device, primary);

    /* ---------------------------------------------------------------
     * Step 5: Initialize all 15 game modules
     *
     * td5re_init() walks the module table in order: game, physics,
     * track, ai, render, frontend, hud, sound, input, asset, save,
     * net, camera, vfx, fmv.
     * --------------------------------------------------------------- */
    /* Change working directory to data/ so all relative asset paths
     * (LOADING.ZIP, LEGALS.ZIP, SOUND\SOUND.ZIP, etc.) resolve correctly. */
    SetCurrentDirectoryA("data");
    dbglog("Working directory changed to data/");

    /* Update global state with actual render dimensions */
    g_td5.render_width  = width;
    g_td5.render_height = height;
    dbglog("Step 4: Platform init OK (render=%dx%d)", width, height);
    dbglog("Step 5: td5re_init (15 modules)...");
    if (!td5re_init()) {
        MessageBoxA(NULL, "td5re_init failed.\n\n"
                    "One or more game modules could not initialize.",
                    "TD5RE - Fatal Error", MB_OK | MB_ICONERROR);
        td5re_shutdown();
        Backend_Shutdown();
        return 1;
    }

    /* ---------------------------------------------------------------
     * Step 6: Main loop
     *
     * Standard Win32 message pump with PeekMessage (non-blocking).
     * Each iteration calls td5re_frame() which drives the game state
     * machine (intro -> menu -> race -> benchmark).
     * --------------------------------------------------------------- */
    dbglog("Step 5: td5re_init OK -- entering main loop");
    dbglog("game_state=%d, frontend_init_pending=%d", g_td5.game_state, g_td5.frontend_init_pending);

    while (running) {
        /* Drain all pending Windows messages */
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                running = 0;
                break;
            }
            /* DEBUG: F5 triggers a race start to test loading screen */
            if (msg.message == WM_KEYDOWN && msg.wParam == VK_F5
                    && g_td5.game_state == 1 /* MENU */) {
                g_td5.race_requested = 1;
                g_td5.track_index = 0;
                dbglog("DEBUG: F5 pressed -- triggering race start");
            }
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }

        if (!running)
            break;

        { static int frame_count = 0; frame_count++;
          if (frame_count <= 10) dbglog("Frame %d: state=%d", frame_count, g_td5.game_state); }

        /* Run one frame of the game state machine.
         * td5re_frame() drives the FSM (INTRO -> MENU -> RACE -> BENCHMARK)
         * and calls td5_plat_present() internally at the end of each frame. */
        td5re_frame();

        Sleep(16); /* ~60fps cap */

        /* Also check the global quit flag */
        if (g_td5.quit_requested) {
            running = 0;
        }
    }

    /* ---------------------------------------------------------------
     * Step 7: Shutdown
     * --------------------------------------------------------------- */
    td5re_shutdown();
    Backend_Shutdown();

    return 0;
}
