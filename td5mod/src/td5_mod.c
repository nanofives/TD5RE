/**
 * td5_mod.c - Test Drive 5 Mod Framework
 *
 * Injected via Ultimate ASI Loader as td5_mod.asi (renamed DLL).
 * Uses MinHook for function hooks and PatchMemory for byte patches.
 *
 * Features:
 *   1. Damage Wobble Restoration    -- cut feature (direct patch)
 *   2. Snow Rendering Restoration   -- cut feature (MinHook)
 *   3. Debug Overlay                -- FPS/speed/gear during races (MinHook)
 *   4. Widescreen + High-FPS Fix   -- runtime patches replacing 31 binary mods
 *
 * Build: 32-bit DLL, rename .dll to .asi, place in scripts/ folder.
 * Requires: Ultimate ASI Loader (winmm.dll or dinput.dll proxy) in game root.
 * Requires: UNPATCHED original TD5_d3d.exe and M2DX.dll when WidescreenFix=1.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include "MinHook.h"
#include "td5_sdk.h"
#include "td5_tuning.h"
#include "td5_asset_dump.h"
/* #include "td5_png_replace.h" -- disabled for testing */

/* Stubs for PngReplace when module is not linked */
static int PngReplace_Init(const char *p) { (void)p; return 0; }
static uint32_t PngReplace_TryReplace(const char *a, const char *b, char *c, uint32_t d) { (void)a;(void)b;(void)c;(void)d; return 0; }
static uint32_t PngReplace_GetTgaSize(const char *a, const char *b) { (void)a;(void)b; return 0; }
static void PngReplace_Shutdown(void) {}

/* =========================================================================
 * DirectDraw Constants (minimal, avoids #include <ddraw.h>)
 * ========================================================================= */

#define DDBLT_WAIT              0x01000000UL
#define DDBLT_COLORFILL         0x00000400UL
#define DDWAITVB_BLOCKBEGIN     0x00000001UL

#define DDBLTFX_STRUCT_SIZE     100
#define DDBLTFX_OFF_FILLCOLOR   80

/* IDirectDrawSurface vtable slot indices (byte_offset / 4) */
#define VT_DDS_BLT              5    /* +0x14 */

/* IDirectDraw vtable slot indices */
#define VT_DD_WAITFORVBLANK     22   /* +0x58 */

/* COM function pointer types (stdcall ABI) */
typedef HRESULT (__stdcall *pfn_DDS_Blt)(
    void *self, RECT *lpDestRect, void *lpDDSSrc,
    RECT *lpSrcRect, DWORD dwFlags, void *lpDDBltFx);

typedef HRESULT (__stdcall *pfn_DD_WaitVBlank)(
    void *self, DWORD dwFlags, HANDLE hEvent);

/* =========================================================================
 * Configuration (loaded from scripts\td5_mod.ini)
 * ========================================================================= */

static struct {
    /* Feature toggles */
    int enable_damage_wobble;
    int enable_snow_rendering;
    int enable_widescreen_fix;
    int enable_debug_overlay;
    int enable_font_fix;
    /* Widescreen options */
    int ws_width;              /* 0 = auto-detect native resolution */
    int ws_height;             /* 0 = auto-detect native resolution */
    int ws_pillarbox;          /* 1 = 4:3 with black bars, 0 = stretch */
    int ws_vsync;              /* 1 = WaitForVerticalBlank in present */
    int ws_menu_frame_cap;     /* ms between menu logic ticks (16 = ~60Hz) */
    /* Windowed mode */
    int enable_windowed;       /* 1 = run in a window */
    int win_width;             /* Client area width  (default 1280) */
    int win_height;            /* Client area height (default 960)  */
    int win_borderless;        /* 1 = borderless (default), 0 = captioned */
    /* Skip options */
    int skip_intro;            /* 1 = skip intro movie */
    int skip_legals;           /* 1 = skip legal splash screens */
    /* Font fix options */
    char font_name[64];
    char font_file[MAX_PATH];  /* TTF file path (empty = system font) */
    int font_weight;           /* FW_NORMAL=400, FW_BOLD=700, etc. */
    int font_size_small;       /* frontend body text height in px (at 640x480) */
    int font_size_large;       /* frontend title height in px (at 640x480) */
    /* Asset override (loose file loading from extracted ZIPs) */
    int enable_asset_override;
    char assets_dir[MAX_PATH]; /* relative to CWD, default "assets" */
    /* Auto-dump: cycle through all tracks/cars to capture textures */
    int enable_auto_dump;
    int auto_dump_load_frames;  /* frames to wait in race for textures (default 120) */
} g_config;

/* Resolved INI path (set once, reused for hot-reload) */
static char g_iniPath[MAX_PATH] = {0};

static void LoadConfig(void) {
    char *ini = g_iniPath;
    if (!ini[0]) {
        GetModuleFileNameA(NULL, ini, MAX_PATH);
        char *s = strrchr(ini, '\\');
        if (s) strcpy(s + 1, "scripts\\td5_mod.ini");
    }

    g_config.enable_damage_wobble  = GetPrivateProfileIntA("Features", "DamageWobble",  1, ini);
    g_config.enable_snow_rendering = GetPrivateProfileIntA("Features", "SnowRendering", 1, ini);
    g_config.enable_widescreen_fix = GetPrivateProfileIntA("Features", "WidescreenFix", 0, ini);
    g_config.enable_debug_overlay  = GetPrivateProfileIntA("Features", "DebugOverlay",  0, ini);

    g_config.ws_width          = GetPrivateProfileIntA("Widescreen", "Width",        0, ini);
    g_config.ws_height         = GetPrivateProfileIntA("Widescreen", "Height",       0, ini);
    g_config.ws_pillarbox      = GetPrivateProfileIntA("Widescreen", "Pillarbox",    1, ini);
    g_config.ws_vsync          = GetPrivateProfileIntA("Widescreen", "VSync",        1, ini);
    g_config.ws_menu_frame_cap = GetPrivateProfileIntA("Widescreen", "MenuFrameCap", 16, ini);

    g_config.enable_windowed   = GetPrivateProfileIntA("Features", "Windowed",      0, ini);
    g_config.win_width         = GetPrivateProfileIntA("Windowed", "Width",      1280, ini);
    g_config.win_height        = GetPrivateProfileIntA("Windowed", "Height",      960, ini);
    g_config.win_borderless    = GetPrivateProfileIntA("Windowed", "Borderless",    1, ini);

    g_config.skip_intro        = GetPrivateProfileIntA("Features", "SkipIntro",     1, ini);
    g_config.skip_legals       = GetPrivateProfileIntA("Features", "SkipLegals",    1, ini);

    g_config.enable_font_fix   = GetPrivateProfileIntA("Features", "FontFix",       1, ini);
    GetPrivateProfileStringA("FontFix", "FontName", "FunctionX", g_config.font_name,
                             sizeof(g_config.font_name), ini);
    GetPrivateProfileStringA("FontFix", "FontFile", "FunctionX Bold.ttf", g_config.font_file,
                             sizeof(g_config.font_file), ini);
    g_config.font_weight       = GetPrivateProfileIntA("FontFix", "FontWeight",   400, ini);
    g_config.font_size_small   = GetPrivateProfileIntA("FontFix", "SmallFontSize", 16, ini);
    g_config.font_size_large   = GetPrivateProfileIntA("FontFix", "LargeFontSize", 32, ini);

    g_config.enable_asset_override = GetPrivateProfileIntA("AssetOverride", "Enable", 0, ini);
    GetPrivateProfileStringA("AssetOverride", "AssetsDir", "assets", g_config.assets_dir,
                             sizeof(g_config.assets_dir), ini);

    g_config.enable_auto_dump     = GetPrivateProfileIntA("AutoDump", "Enable", 0, ini);
    g_config.auto_dump_load_frames = GetPrivateProfileIntA("AutoDump", "LoadFrames", 120, ini);
}

/* =========================================================================
 * Hot-Reload: Press F11 to re-read td5_mod.ini while game is running
 *
 * Called per-frame from both frontend (LogicGate) and race (HUD overlay).
 * Only re-reads values that are safe to change at runtime (tuning knobs).
 * Structural patches (resolution, hooks) are NOT re-applied.
 * ========================================================================= */

static void CheckHotReload(void) {
    static int s_f11_was_down = 0;
    int f11_down = (GetAsyncKeyState(VK_F11) & 0x8000) != 0;

    if (f11_down && !s_f11_was_down) {
        char *ini = g_iniPath;

        g_config.enable_debug_overlay = GetPrivateProfileIntA("Features", "DebugOverlay", 0, ini);
        g_config.ws_menu_frame_cap    = GetPrivateProfileIntA("Widescreen", "MenuFrameCap", 0, ini);
        g_config.ws_vsync             = GetPrivateProfileIntA("Widescreen", "VSync", 1, ini);
        g_config.font_weight          = GetPrivateProfileIntA("FontFix", "FontWeight", 400, ini);
        g_config.font_size_small      = GetPrivateProfileIntA("FontFix", "SmallFontSize", 16, ini);
        g_config.font_size_large      = GetPrivateProfileIntA("FontFix", "LargeFontSize", 32, ini);
        GetPrivateProfileStringA("FontFix", "FontName", "FunctionX", g_config.font_name,
                                 sizeof(g_config.font_name), ini);

        OutputDebugStringA("[TD5MOD] Hot-reload: INI re-read (F11)\n");
    }
    s_f11_was_down = f11_down;
}

/* =========================================================================
 * Windowed Mode -- hook DXWin::Initialize to restyle the game window
 *
 * M2DX.dll creates a WS_POPUP window at fullscreen size. We hook
 * DXWin::Initialize (M2DX.dll+0x12a10) to:
 *   1. Clear the fullscreen flag so DXD3D::Create stays in windowed coop
 *   2. After the window is created, restyle it to a bordered window
 * ========================================================================= */

/* Forward declaration (defined in Utility section below; extern for td5_tuning.c) */
int PatchMemory(void *addr, const void *data, size_t len);

/* M2DX.dll offsets from DLL base */
#define M2DX_OFF_FULLSCREEN_FLAG  0x61c1c   /* DAT_10061c1c: startup fullscreen toggle */
#define M2DX_OFF_ADAPTER_JNZ     0x6637    /* JNZ in DXDraw::Create adapter windowed check */
#define M2DX_OFF_COOPLEVEL_JZ    0x78ca    /* JZ in ConfigureDirectDrawCooperativeLevel */

typedef int (__cdecl *pfn_DXWin_Initialize)(void);
static pfn_DXWin_Initialize Original_DXWin_Initialize = NULL;

static int __cdecl Hook_DXWin_Initialize(void) {
    /* Clear fullscreen flag so DXD3D::Create skips FullScreen() call.
     * Environment() defaults this to 1; we override after it runs. */
    HMODULE m2dx = GetModuleHandleA("M2DX.dll");
    if (m2dx) {
        uintptr_t base = (uintptr_t)m2dx;

        /* Clear startup fullscreen flag */
        int zero = 0;
        PatchMemory((void *)(base + M2DX_OFF_FULLSCREEN_FLAG), &zero, 4);

        /* Patch DXDraw::Create: JNZ -> JMP to bypass adapter "no windowed"
         * check that forces exclusive fullscreen when DDrawCompat reports
         * the adapter as not supporting windowed cooperative level. */
        uint8_t jmp = 0xEB;
        PatchMemory((void *)(base + M2DX_OFF_ADAPTER_JNZ), &jmp, 1);

        /* Patch ConfigureDirectDrawCooperativeLevel: JZ -> JMP to always
         * take the DDSCL_NORMAL (windowed) path, never exclusive fullscreen. */
        PatchMemory((void *)(base + M2DX_OFF_COOPLEVEL_JZ), &jmp, 1);

        OutputDebugStringA("[TD5MOD] M2DX windowed patches applied\n");
    }

    int ret = Original_DXWin_Initialize();
    if (!ret) return ret;

    HWND hwnd = FindWindowA("Test Drive 5", "Test Drive 5");
    if (!hwnd) return ret;

    int w, h, x, y;
    if (g_config.win_borderless) {
        /* Borderless: keep WS_POPUP, just resize and center */
        w = g_config.win_width;
        h = g_config.win_height;
    } else {
        /* Captioned: title bar + close/minimize buttons.
         * Note: Windows forces classic borders on DirectDraw processes. */
        LONG style = WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
        SetWindowLongA(hwnd, GWL_STYLE, style);
        RECT rc = { 0, 0, g_config.win_width, g_config.win_height };
        AdjustWindowRectEx(&rc, style, FALSE, WS_EX_APPWINDOW);
        w = rc.right - rc.left;
        h = rc.bottom - rc.top;
    }
    x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2;
    y = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;

    SetWindowPos(hwnd, HWND_TOP, x, y, w, h, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    ShowCursor(1);  /* Restore mouse cursor for windowed mode */

    OutputDebugStringA("[TD5MOD] Window restyled for windowed mode\n");
    return ret;
}

/* =========================================================================
 * Widescreen Layout State
 * ========================================================================= */

static struct {
    int screen_w, screen_h;       /* Target display resolution */
    int content_w, content_h;     /* 4:3 content area dimensions */
    int pillar_left;              /* Left pillarbox width in pixels */
    RECT src_rect;                /* Always {0, 0, 640, 480} */
    RECT dest_rect;               /* Content area on front surface */
    RECT left_bar;                /* Left pillarbox rect */
    RECT right_bar;               /* Right pillarbox rect */
    uint8_t bltfx[DDBLTFX_STRUCT_SIZE];  /* DDBLTFX for black COLORFILL */
    int has_bars;                 /* Non-zero if pillarbox bars needed */
    int game_base_w, game_base_h; /* M2DX's internal resolution (default 640x480) */
} g_ws;

static void ComputeWidescreenLayout(void) {
    /* Determine target resolution */
    if (g_config.enable_windowed) {
        /* In windowed mode, target = window client area */
        g_ws.screen_w = g_config.win_width;
        g_ws.screen_h = g_config.win_height;
    } else if (g_config.ws_width > 0 && g_config.ws_height > 0) {
        g_ws.screen_w = g_config.ws_width;
        g_ws.screen_h = g_config.ws_height;
    } else {
        g_ws.screen_w = GetSystemMetrics(SM_CXSCREEN);
        g_ws.screen_h = GetSystemMetrics(SM_CYSCREEN);
    }

    /* Source rect matches M2DX's internal resolution (the back-buffer region) */
    if (g_ws.game_base_w == 0) g_ws.game_base_w = 640;
    if (g_ws.game_base_h == 0) g_ws.game_base_h = 480;
    g_ws.src_rect.left   = 0;
    g_ws.src_rect.top    = 0;
    g_ws.src_rect.right  = g_ws.game_base_w;
    g_ws.src_rect.bottom = g_ws.game_base_h;

    if (g_config.ws_pillarbox) {
        /* 4:3 pillarbox mode */
        g_ws.content_h = g_ws.screen_h;
        g_ws.content_w = g_ws.screen_h * 4 / 3;
        if (g_ws.content_w > g_ws.screen_w)
            g_ws.content_w = g_ws.screen_w;
        g_ws.pillar_left = (g_ws.screen_w - g_ws.content_w) / 2;

        g_ws.dest_rect.left   = g_ws.pillar_left;
        g_ws.dest_rect.top    = 0;
        g_ws.dest_rect.right  = g_ws.pillar_left + g_ws.content_w;
        g_ws.dest_rect.bottom = g_ws.screen_h;

        g_ws.left_bar.left   = 0;
        g_ws.left_bar.top    = 0;
        g_ws.left_bar.right  = g_ws.pillar_left;
        g_ws.left_bar.bottom = g_ws.screen_h;

        g_ws.right_bar.left   = g_ws.screen_w - g_ws.pillar_left;
        g_ws.right_bar.top    = 0;
        g_ws.right_bar.right  = g_ws.screen_w;
        g_ws.right_bar.bottom = g_ws.screen_h;

        g_ws.has_bars = (g_ws.pillar_left > 0);
    } else {
        /* Stretch to fill (no pillarbox) */
        g_ws.content_w   = g_ws.screen_w;
        g_ws.content_h   = g_ws.screen_h;
        g_ws.pillar_left = 0;

        g_ws.dest_rect.left   = 0;
        g_ws.dest_rect.top    = 0;
        g_ws.dest_rect.right  = g_ws.screen_w;
        g_ws.dest_rect.bottom = g_ws.screen_h;

        g_ws.has_bars = 0;
    }

    /* DDBLTFX for black color fill */
    memset(g_ws.bltfx, 0, DDBLTFX_STRUCT_SIZE);
    *(uint32_t *)&g_ws.bltfx[0] = DDBLTFX_STRUCT_SIZE;   /* dwSize = 100 */
    /* dwFillColor at offset 80 is 0 (black) by memset */
}

/* =========================================================================
 * Utility: Memory Patching
 * ========================================================================= */

int PatchMemory(void *addr, const void *data, size_t len) {
    DWORD old;
    if (!VirtualProtect(addr, len, PAGE_EXECUTE_READWRITE, &old))
        return 0;
    memcpy(addr, data, len);
    VirtualProtect(addr, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), addr, len);
    return 1;
}

/* Write E8 <rel32> at call site, pad remainder with NOPs */
static int PatchCallSite(uintptr_t site, void *target, int total_bytes) {
    uint8_t buf[16];
    int32_t rel = (int32_t)((uintptr_t)target - (site + 5));
    int i;
    buf[0] = 0xE8;  /* CALL rel32 */
    memcpy(&buf[1], &rel, 4);
    for (i = 5; i < total_bytes && i < (int)sizeof(buf); i++)
        buf[i] = 0x90;  /* NOP */
    return PatchMemory((void *)site, buf, total_bytes);
}

/* =========================================================================
 * Widescreen: Scale Cave -- C replacement for 89-byte ASM cave
 *
 * Blt-stretches back buffer {0,0,640,480} onto front surface at the
 * computed dest rect, draws black pillarbox bars, optionally waits VSync.
 *
 * Two entry points:
 *   ScaleCave_Present     -- with VSync (for Flip path + loading screen)
 *   Hook_PresentSoftware  -- without VSync (software path already did WaitForVBlank)
 * ========================================================================= */

/* Forward declarations for font queue (defined in Font Fix section) */
static void FontFix_FlushToFrontSurface(void *frontSurf);
static int g_fontQueueCount;

static void ScaleCave_Core(int with_vsync) {
    void **exref = g_ddExrefPtr;
    if (!exref) return;

    void *ddraw = exref[0];   /* IDirectDraw*              */
    void *front = exref[1];   /* IDirectDrawSurface* front */
    void *back  = exref[2];   /* IDirectDrawSurface* back  */
    if (!front || !back) return;

    void **front_vt = *(void ***)front;
    pfn_DDS_Blt blt = (pfn_DDS_Blt)front_vt[VT_DDS_BLT];

    /* In windowed mode, the front surface is the desktop primary.
     * Blt coordinates must be in screen space, offset by the window's
     * client-area origin. Read from M2DX globals updated each frame. */
    RECT dest = g_ws.dest_rect;
    if (g_config.enable_windowed) {
        HMODULE m2dx = GetModuleHandleA("M2DX.dll");
        if (m2dx) {
            int cx = *(int *)((uintptr_t)m2dx + 0x61bf4);
            int cy = *(int *)((uintptr_t)m2dx + 0x61bf8);
            dest.left   += cx;
            dest.right  += cx;
            dest.top    += cy;
            dest.bottom += cy;
        }
    }

    /* Blt stretch: back {0,0,640,480} -> front {dest} */
    blt(front, &dest, back, &g_ws.src_rect, DDBLT_WAIT, NULL);

    /* Black pillarbox bars via COLORFILL on front surface */
    if (g_ws.has_bars) {
        RECT lb = g_ws.left_bar, rb = g_ws.right_bar;
        if (g_config.enable_windowed) {
            HMODULE m2dx = GetModuleHandleA("M2DX.dll");
            if (m2dx) {
                int cx = *(int *)((uintptr_t)m2dx + 0x61bf4);
                int cy = *(int *)((uintptr_t)m2dx + 0x61bf8);
                lb.left += cx; lb.right += cx; lb.top += cy; lb.bottom += cy;
                rb.left += cx; rb.right += cx; rb.top += cy; rb.bottom += cy;
            }
        }
        blt(front, &lb, NULL, NULL,
            DDBLT_COLORFILL | DDBLT_WAIT, g_ws.bltfx);
        blt(front, &rb, NULL, NULL,
            DDBLT_COLORFILL | DDBLT_WAIT, g_ws.bltfx);
    }

    /* Flush deferred GDI text to front surface at native resolution */
    if (g_config.enable_font_fix && g_fontQueueCount > 0)
        FontFix_FlushToFrontSurface(front);

    /* VSync via IDirectDraw::WaitForVerticalBlank */
    if (with_vsync && g_config.ws_vsync && ddraw) {
        void **dd_vt = *(void ***)ddraw;
        pfn_DD_WaitVBlank wfvb = (pfn_DD_WaitVBlank)dd_vt[VT_DD_WAITFORVBLANK];
        wfvb(ddraw, DDWAITVB_BLOCKBEGIN, NULL);
    }
}

/* Called from Flip CALL-site patches (0x414D09, 0x42CBBE) */
static void __cdecl ScaleCave_Present(void) {
    ScaleCave_Core(1);
}

/* MinHook replacement for PresentFrontendBufferSoftware (0x425360).
 * The caller already did WaitForVerticalBlank, so skip VSync here. */
typedef void (__cdecl *fn_PresentSoftware)(void);
static fn_PresentSoftware Original_PresentSoftware = NULL;

static void __cdecl Hook_PresentSoftware(void) {
    ScaleCave_Core(0);
}

/* =========================================================================
 * Widescreen: Logic Gate -- C replacement for 65-byte ASM cave
 *
 * Called from screen function dispatch site (VA 0x414C9E, 6 bytes).
 * Gates menu logic to ~60Hz while rendering runs at monitor refresh rate.
 *
 * The original INC at 0x414EB0 is NOPped; this function handles the
 * frame counter increment when logic actually runs.
 * ========================================================================= */

static DWORD s_lastLogicTick = 0;

static void __cdecl LogicGate_ScreenDispatch(void) {
    CheckHotReload();  /* F11 hot-reload (frontend path) */

    /* MenuFrameCap=0 disables the gate: counter increments every frame.
     * MenuFrameCap>0 gates the counter to ~1000/cap Hz (16ms = ~60Hz). */
    if (g_config.ws_menu_frame_cap > 0) {
        DWORD now = TD5_IAT_timeGetTime();
        if (now - s_lastLogicTick >= (DWORD)g_config.ws_menu_frame_cap) {
            s_lastLogicTick = now;
            g_frontendAnimTick++;
        }
    } else {
        g_frontendAnimTick++;
    }

    /* ALWAYS call the screen function (handles input + state transitions).
     * On non-logic frames the counter is unchanged, so animations freeze
     * but input remains responsive at full refresh rate. */
    {
        void (__cdecl *screenFn)(void) = g_currentScreenFnPtr;
        if (screenFn)
            screenFn();
    }
}

/* =========================================================================
 * Feature: Damage Wobble Restoration
 *
 * GetDamageRulesStub (0x42C8D0) is 3 bytes: XOR EAX,EAX; RET + NOPs.
 * We patch it to JMP to _rand (0x448157) so collision wobble returns.
 * ========================================================================= */

static int PatchDamageWobble(void) {
    uint8_t jmp[5];
    int32_t rel = 0x00448157 - (0x0042C8D0 + 5);
    jmp[0] = 0xE9;  /* JMP rel32 */
    memcpy(&jmp[1], &rel, 4);

    if (!PatchMemory((void *)0x0042C8D0, jmp, 5)) {
        OutputDebugStringA("[TD5MOD] FAILED to patch damage wobble\n");
        return 0;
    }
    OutputDebugStringA("[TD5MOD] Damage wobble restored\n");
    return 1;
}

/* =========================================================================
 * Feature: Snow Rendering Restoration
 *
 * RenderAmbientParticleStreaks (0x446560) skips rendering when weather!=0.
 * Hook temporarily sets weather to 0 so snow renders as rain-style streaks.
 * ========================================================================= */

typedef void (__cdecl *fn_RenderParticles3)(int, float, int);
static fn_RenderParticles3 Original_RenderParticles = NULL;

static void __cdecl Hook_RenderParticles(int actorPtr, float param2, int viewIdx) {
    int saved = g_weatherType;
    if (g_config.enable_snow_rendering && saved == 1)
        g_weatherType = 0;
    Original_RenderParticles(actorPtr, param2, viewIdx);
    g_weatherType = saved;
}

/* =========================================================================
 * Feature: Debug Overlay
 *
 * Hooks RenderRaceHudOverlays (0x4388A0) to draw FPS/speed/gear text
 * during active race rendering via QueueRaceHudFormattedText.
 * ========================================================================= */

typedef void (__cdecl *fn_RenderHud)(float);
static fn_RenderHud Original_RenderRaceHudOverlays = NULL;

typedef void (__cdecl *fn_QueueHudText)(int, int, int, int, const char *);
#define GameQueueHudText ((fn_QueueHudText)0x00428320)

static void __cdecl Hook_RenderRaceHudOverlays(float param1) {
    CheckHotReload();  /* F11 hot-reload (race path) */

    if (g_config.enable_debug_overlay) {
        char buf[64];
        int fps = (g_normalizedFrameDt > 0.001f)
            ? (int)(30.0f / g_normalizedFrameDt) : 0;
        int spd = *(int32_t *)(TD5_ACTOR(0) + ACTOR_OFF_FORWARD_SPEED);
        int gear = *(int8_t *)(TD5_ACTOR(0) + ACTOR_OFF_GEAR_CURRENT);
        wsprintfA(buf, "FPS:%d SPD:%d G:%d", fps, spd >> 8, gear);
        GameQueueHudText(0, 10, 10, 1, buf);
    }

    Original_RenderRaceHudOverlays(param1);
}

/* =========================================================================
 * Widescreen: Apply All Patches
 *
 * Applies the runtime equivalent of 31 binary patches:
 *   - 14 PatchMemory byte patches (resolution, FOV, NOPs, DDBLT_WAIT)
 *   -  3 CALL-site redirections (scale cave x2, logic gate x1)
 *   -  5 M2DX.dll byte patches (4:3 filter, resolution defaults)
 *   -  9 eliminated (data/code caves -> C statics; PE header -> per-patch VP)
 * ========================================================================= */

static int ApplyWidescreenPatches(void) {
    int ok = 1;
    uint32_t w = (uint32_t)g_ws.screen_w;
    uint32_t h = (uint32_t)g_ws.screen_h;
    float fw = (float)g_ws.screen_w;
    float fh = (float)g_ws.screen_h;
    uint8_t b;

    OutputDebugStringA("[TD5MOD] Applying widescreen patches...\n");

    /* =================================================================
     * EXE: Resolution immediates
     * ================================================================= */

    /* Patch 1: WinMain MOV [ECX+0xBC], width (imm32 at VA 0x430AB2) */
    ok &= PatchMemory((void *)0x430AB2, &w, 4);

    /* Patch 2: WinMain MOV [EDX+0xC0], height (imm32 at VA 0x430AC2) */
    ok &= PatchMemory((void *)0x430AC2, &h, 4);

    /* Patch 3a: Menu init PUSH height (imm32 at VA 0x42A999) */
    ok &= PatchMemory((void *)0x42A999, &h, 4);

    /* Patch 3b: Menu init PUSH width (imm32 at VA 0x42A99E) */
    ok &= PatchMemory((void *)0x42A99E, &w, 4);

    /* Patch 3c: Menu init MOV [0x4AAF08], float width (imm32 at VA 0x42A9AD) */
    ok &= PatchMemory((void *)0x42A9AD, &fw, 4);

    /* Patch 3d: Menu init MOV [0x4AAF0C], float height (imm32 at VA 0x42A9B7) */
    ok &= PatchMemory((void *)0x42A9B7, &fh, 4);

    /* =================================================================
     * EXE: Projection / FOV fix (hor+ widescreen)
     *
     * Original: focal = width * 0.5625  (4:3 assumption)
     * Patched:  focal = height * 0.75   (correct for any aspect ratio)
     * ================================================================= */

    /* Patch 4: struct offset 0x0C -> 0x10 (select height instead of width) */
    b = 0x10;
    ok &= PatchMemory((void *)0x43E828, &b, 1);

    /* Patch 5: address byte 0x8C -> 0x80 (reference 0.75 instead of 0.5625) */
    b = 0x80;
    ok &= PatchMemory((void *)0x43E82C, &b, 1);

    /* =================================================================
     * EXE: Behavioral patches for high-refresh frontend
     * ================================================================= */

    /* Patch 18: DEC EAX -> NOP (force BlitFrontendCachedRect every frame) */
    b = 0x90;
    ok &= PatchMemory((void *)0x414BA7, &b, 1);

    /* Patch 19: INC EDX -> NOP (disable original frame counter increment;
     *           LogicGate_ScreenDispatch handles it conditionally) */
    ok &= PatchMemory((void *)0x414EB0, &b, 1);

    /* Patches 22-24: Add DDBLT_WAIT flag to frontend COLORFILL operations
     *   Each patches the MSB of a PUSH imm32 from 0x00 to 0x01
     *   (0x00000400 DDBLT_COLORFILL -> 0x01000400 DDBLT_COLORFILL|DDBLT_WAIT) */
    b = 0x01;
    ok &= PatchMemory((void *)0x423E2A, &b, 1);  /* ClearBackbufferWithColor */
    ok &= PatchMemory((void *)0x423F77, &b, 1);  /* FillPrimaryFrontendRect */
    ok &= PatchMemory((void *)0x424E2A, &b, 1);  /* FillPrimaryFrontendScanline */

    /* =================================================================
     * EXE: CALL-site redirections
     * ================================================================= */

    /* Patch 6 & 7: Flip -> ScaleCave_Present (fullscreen only).
     * In windowed mode the back buffer matches the window, so the original
     * Flip path (Blt with dirty rects) is correct -- no stretching needed. */
    /* Patch 6: Frontend Flip -> ScaleCave_Present
     *   Stretches the 640x480 frontend content to fill display/window.
     *   Original: 53 FF 15 04 D5 45 00 83 C4 04  (PUSH EBX; CALL [IAT]; ADD ESP,4)
     *   Patched:  E8 <rel32> 90 90 90 90 90       (CALL ScaleCave + 5x NOP) */
    ok &= PatchCallSite(0x414D09, ScaleCave_Present, 10);

    /* Patch 7: Loading screen Flip -> ScaleCave_Present
     *   Original: 50 FF 15 04 D5 45 00 83 C4 04  (PUSH EAX; CALL [IAT]; ADD ESP,4)
     *   Patched:  E8 <rel32> 90 90 90 90 90       (CALL ScaleCave + 5x NOP) */
    ok &= PatchCallSite(0x42CBBE, ScaleCave_Present, 10);

    /* Patch 20: Screen function dispatch -> LogicGate_ScreenDispatch
     *   Original: FF 15 38 52 49 00               (CALL [0x495238])
     *   Patched:  E8 <rel32> 90                   (CALL LogicGate + NOP) */
    ok &= PatchCallSite(0x414C9E, LogicGate_ScreenDispatch, 6);

    /* =================================================================
     * M2DX.DLL: Resolution and aspect-ratio patches
     * ================================================================= */

    {
        HMODULE m2dx = GetModuleHandleA("M2DX.dll");
        if (m2dx) {
            uintptr_t base = (uintptr_t)m2dx;

            /* Patch 27: Remove 4:3 aspect ratio filter (JE -> JMP) */
            b = 0xEB;
            ok &= PatchMemory((void *)(base + 0x80B3), &b, 1);

            /* Patch 28: EnumDisplayModes default width  (MOV EBX, imm32) */
            ok &= PatchMemory((void *)(base + 0x7F94), &w, 4);

            /* Patch 29: EnumDisplayModes default height (CMP [EAX], imm32) */
            ok &= PatchMemory((void *)(base + 0x7FA4), &h, 4);

            /* Patch 30: SelectPreferredDisplayMode width  (CMP [ESI-8], imm32) */
            ok &= PatchMemory((void *)(base + 0x2BC0), &w, 4);

            /* Patch 31: SelectPreferredDisplayMode height (CMP [ESI-4], imm32) */
            ok &= PatchMemory((void *)(base + 0x2BC9), &h, 4);

            OutputDebugStringA("[TD5MOD] M2DX.dll patches applied\n");
        } else {
            OutputDebugStringA("[TD5MOD] WARNING: M2DX.dll not found\n");
        }
    }

    {
        char msg[160];
        wsprintfA(msg, "[TD5MOD] Widescreen: %dx%d pillarbox=%d bars=%d vsync=%d gate=%dms\n",
            g_ws.screen_w, g_ws.screen_h,
            g_config.ws_pillarbox, g_ws.has_bars,
            g_config.ws_vsync, g_config.ws_menu_frame_cap);
        OutputDebugStringA(msg);
    }

    return ok;
}

/* =========================================================================
 * Feature: Font Fix (GDI Text + HUD Scaling)
 *
 * Replaces bitmap glyph atlas text rendering with Windows GDI TextOut for
 * crisp, scalable text in frontend menus.  Scales race HUD glyph quads so
 * text doesn't shrink at higher resolutions.
 *
 * Frontend: 5 MinHook hooks on the localized draw/measure wrappers.
 * Race HUD: Hook InitializeRaceHudFontGlyphAtlas to scale glyph sizes,
 *           Hook QueueRaceHudFormattedText to separate UV from screen size.
 * ========================================================================= */

/* --- GDI Font handles --- */
static HFONT g_hFontSmall      = NULL;  /* frontend body text */
static HFONT g_hFontLarge      = NULL;  /* frontend titles */
static HFONT g_hFontSmallNat   = NULL;  /* native-res body (for front surface) */
static HFONT g_hFontLargeNat   = NULL;  /* native-res title (for front surface) */
static HDC   g_hMeasureDC      = NULL;  /* persistent DC for measurement */
static int   g_fontFileLoaded  = 0;
static int   g_smallFontHeight = 0;     /* tmHeight of small font */
static int   g_largeFontHeight = 0;     /* tmHeight of large font */
static int   g_smallFontLead   = 0;     /* tmInternalLeading of small font */

/* --- Deferred text queue (frontend -> front surface at native res) --- */
#define FONT_QUEUE_MAX 128

typedef struct {
    char text[128];
    int x, y;             /* 640x480 virtual coordinates */
    int use_large_font;   /* 0 = small, 1 = large */
} FontQueueEntry;

static FontQueueEntry g_fontQueue[FONT_QUEUE_MAX];
/* g_fontQueueCount is declared earlier (forward) for ScaleCave_Core access */

/* --- Race HUD scaling state --- */
static float g_hudScaleX = 1.0f;    /* width/640: X dimension scale */
static float g_hudScaleY = 1.0f;    /* height/480: Y dimension scale */

/* --- DDraw COM helpers for GDI text --- */

typedef HRESULT (__stdcall *FN_SurfGetDC)(void *self, HDC *phdc);
typedef HRESULT (__stdcall *FN_SurfReleaseDC)(void *self, HDC hdc);

static HDC FontFix_GetDC(void *surf) {
    HDC hdc = NULL;
    void **vt;
    if (!surf) return NULL;
    vt = *(void ***)surf;
    if (SUCCEEDED(((FN_SurfGetDC)vt[VT_DDS_GETDC])(surf, &hdc)))
        return hdc;
    return NULL;
}

static void FontFix_ReleaseDC(void *surf, HDC hdc) {
    void **vt = *(void ***)surf;
    ((FN_SurfReleaseDC)vt[VT_DDS_RELEASEDC])(surf, hdc);
}

/* --- GDI text draw/measure helpers --- */

static int FontFix_DrawText(void *surf, const char *str, int x, int y, HFONT font) {
    HDC hdc;
    HFONT old;
    SIZE sz;
    int len;

    if (!surf || !str || !*str) return 0;
    hdc = FontFix_GetDC(surf);
    if (!hdc) return 0;

    old = (HFONT)SelectObject(hdc, font);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(255, 255, 255));

    len = lstrlenA(str);
    TextOutA(hdc, x, y, str, len);
    GetTextExtentPoint32A(hdc, str, len, &sz);

    SelectObject(hdc, old);
    FontFix_ReleaseDC(surf, hdc);
    return sz.cx;
}

static int FontFix_Measure(const char *str, HFONT font) {
    HFONT old;
    SIZE sz;
    int len;

    if (!str || !*str) return 0;
    if (!g_hMeasureDC) {
        g_hMeasureDC = CreateCompatibleDC(NULL);
        if (!g_hMeasureDC) return 0;
    }

    old = (HFONT)SelectObject(g_hMeasureDC, font);
    len = lstrlenA(str);
    GetTextExtentPoint32A(g_hMeasureDC, str, len, &sz);
    SelectObject(g_hMeasureDC, old);
    return sz.cx;
}

/* --- Frontend hook originals (stored by MinHook) --- */
static fn_DrawLocString     Orig_DrawLocPrimary   = NULL;
static fn_DrawLocString     Orig_DrawLocSecondary  = NULL;
static fn_DrawLocToSurface  Orig_DrawLocToSurface  = NULL;
static fn_MeasureOrDrawFont Orig_MeasureOrDrawFont = NULL;
static fn_MeasureOrCenter   Orig_MeasureOrCenter   = NULL;

/* --- Additional unhooked text renderers that also need GDI replacement --- */
typedef int (__cdecl *fn_DrawClippedToSurface)(const char*, int, int, void*, unsigned int);
typedef int (__cdecl *fn_DrawSmallFontToSurface)(const char*, unsigned int, int, void*);
static fn_DrawClippedToSurface   Orig_DrawClippedToSurface   = NULL;
static fn_DrawSmallFontToSurface Orig_DrawSmallFontToSurface = NULL;

/* --- Frontend: queue text for deferred native-res rendering --- */
static void FontFix_QueueText(const char *str, int x, int y, int large) {
    if (g_fontQueueCount < FONT_QUEUE_MAX) {
        FontQueueEntry *e = &g_fontQueue[g_fontQueueCount++];
        lstrcpynA(e->text, str, sizeof(e->text));
        e->x = x;
        e->y = y;
        e->use_large_font = large;
    }
}

/* Forward declaration */
static void FontFix_UpdateWidthTables(void);

/* --- Frontend font scaling: recreate GDI fonts scaled to canvas resolution --- */
static int g_fontsScaled = 0;

static void FontFix_ScaleForCanvas(void) {
    int cw = g_frontendCanvasW;
    int ch = g_frontendCanvasH;
    float sy;
    int newSmall, newLarge;

    if (g_fontsScaled || ch <= 480) return;
    g_fontsScaled = 1;

    sy = (float)ch / 480.0f;
    newSmall = (int)(g_config.font_size_small * sy);
    newLarge = (int)(g_config.font_size_large * sy);

    if (g_hFontSmall) DeleteObject(g_hFontSmall);
    g_hFontSmall = CreateFontA(
        -newSmall, 0, 0, 0, g_config.font_weight,
        FALSE, FALSE, FALSE,
        ANSI_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
        ANTIALIASED_QUALITY, FF_SWISS | VARIABLE_PITCH,
        g_config.font_name);

    if (g_hFontLarge) DeleteObject(g_hFontLarge);
    g_hFontLarge = CreateFontA(
        -newLarge, 0, 0, 0, g_config.font_weight,
        FALSE, FALSE, FALSE,
        ANSI_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
        ANTIALIASED_QUALITY, FF_SWISS | VARIABLE_PITCH,
        g_config.font_name);

    /* Remeasure heights */
    {
        HDC hdc = CreateCompatibleDC(NULL);
        TEXTMETRICA tm;
        HFONT old = (HFONT)SelectObject(hdc, g_hFontSmall);
        GetTextMetricsA(hdc, &tm);
        g_smallFontHeight = tm.tmHeight;
        g_smallFontLead   = tm.tmExternalLeading;
        SelectObject(hdc, g_hFontLarge);
        GetTextMetricsA(hdc, &tm);
        g_largeFontHeight = tm.tmHeight;
        SelectObject(hdc, old);
        DeleteDC(hdc);
    }

    /* Update width tables with new font metrics */
    FontFix_UpdateWidthTables();

    /* Patch gallery slideshow FIXED position for car selection screen.
     * Only safe 4-byte immediates (MOV dword, PUSH imm32). Random position
     * bounds in AdvanceExtrasGallerySlideshow left unpatched (uses imm8
     * which sign-extends, causing negative coords at >2x scale). */
    {
        DWORD prot;
        float sx = (float)cw / 640.0f;
        float ssy = (float)ch / 480.0f;
        int galX = (int)(0x76 * sx);
        int galY = (int)(0x8C * ssy);

        /* MOV [galleryX], imm32 at 0x40D98E */
        VirtualProtect((void*)0x40D98E, 4, PAGE_READWRITE, &prot);
        *(int*)0x40D98E = galX;
        VirtualProtect((void*)0x40D98E, 4, prot, &prot);

        /* MOV [galleryY], imm32 at 0x40D998 */
        VirtualProtect((void*)0x40D998, 4, PAGE_READWRITE, &prot);
        *(int*)0x40D998 = galY;
        VirtualProtect((void*)0x40D998, 4, prot, &prot);

        /* BltFast PUSH Y (5-byte PUSH imm32 at 0x40D9C7) */
        VirtualProtect((void*)0x40D9C7, 4, PAGE_READWRITE, &prot);
        *(int*)0x40D9C7 = galY;
        VirtualProtect((void*)0x40D9C7, 4, prot, &prot);

        OutputDebugStringA("[TD5MOD] Gallery fixed-position patches applied\n");
    }

    {
        char msg[128];
        wsprintfA(msg, "[TD5MOD] Font scaled for %dx%d: small=%d large=%d (%.2fx)\n",
                  cw, ch, newSmall, newLarge, sy);
        OutputDebugStringA(msg);
    }
}

/* --- Frontend: flush queued text to front surface at native resolution --- */
static void FontFix_FlushToFrontSurface(void *frontSurf) {
    int i;
    float scaleX, scaleY;
    if (!frontSurf || g_fontQueueCount == 0) return;

    scaleX = (float)g_ws.content_w / (float)g_ws.game_base_w;
    scaleY = (float)g_ws.content_h / (float)g_ws.game_base_h;

    for (i = 0; i < g_fontQueueCount; i++) {
        FontQueueEntry *e = &g_fontQueue[i];
        int nx = g_ws.pillar_left + (int)(e->x * scaleX);
        int ny = (int)(e->y * scaleY);
        HFONT font = e->use_large_font ? g_hFontLargeNat : g_hFontSmallNat;
        FontFix_DrawText(frontSurf, e->text, nx, ny, font);
    }
    g_fontQueueCount = 0;
}

/* --- Frontend replacement: DrawFrontendLocalizedStringPrimary (0x4242B0) --- */
static int __cdecl Hook_DrawLocPrimary(const char *str, unsigned int x, int y) {
    /* if (!g_fontsScaled) FontFix_ScaleForCanvas(); — disabled */
    if (!str || !*str) return 0;
    if (g_config.enable_widescreen_fix && g_hFontSmallNat) {
        FontFix_QueueText(str, (int)x, y, 0);
        return FontFix_Measure(str, g_hFontSmall);
    }
    return FontFix_DrawText(g_primaryWorkSurface, str, (int)x, y, g_hFontSmall);
}

/* --- Frontend replacement: DrawFrontendLocalizedStringSecondary (0x424390) --- */
static int __cdecl Hook_DrawLocSecondary(const char *str, unsigned int x, int y) {
    /* if (!g_fontsScaled) FontFix_ScaleForCanvas(); — disabled */
    if (!str || !*str) return 0;
    if (g_config.enable_widescreen_fix && g_hFontSmallNat) {
        FontFix_QueueText(str, (int)x, y, 0);
        return FontFix_Measure(str, g_hFontSmall);
    }
    return FontFix_DrawText(g_secondaryWorkSurface, str, (int)x, y, g_hFontSmall);
}

/* --- Frontend replacement: DrawFrontendLocalizedStringToSurface (0x424560) ---
 *     Button surfaces are dual-frame (normal + highlight stacked vertically).
 *     The caller passes Y within whichever frame is being drawn.
 *     We get the surface height, determine the frame height (surfH / 2),
 *     identify which frame Y falls in, and center text within that frame. */
static int __cdecl Hook_DrawLocToSurface(const char *str, int x, int y, void *surf) {
    HDC hdc;
    HFONT old;
    SIZE sz;
    /* if (!g_fontsScaled) FontFix_ScaleForCanvas(); — disabled */
    RECT clipRect;
    int len, centeredY, surfH, frameH, frameTop;

    if (!str || !*str || !surf) return 0;
    hdc = FontFix_GetDC(surf);
    if (!hdc) return 0;

    old = (HFONT)SelectObject(hdc, g_hFontSmall);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(255, 255, 255));

    len = lstrlenA(str);
    GetTextExtentPoint32A(hdc, str, len, &sz);

    /* Get actual surface dimensions */
    GetClipBox(hdc, &clipRect);
    surfH = clipRect.bottom;

    {
        /* Scale button detection threshold for higher resolutions.
         * At 640x480: max dual-frame = 64px, threshold = 80.
         * At 1280x960 (2x): max dual-frame = 128px, threshold = 160. */
        int ch = g_frontendCanvasH;
        int buttonThreshold = (ch > 480) ? (int)(80.0f * (float)ch / 480.0f) : 80;
        int singleThreshold = (ch > 480) ? (int)(40.0f * (float)ch / 480.0f) : 40;

    if (surfH > 0 && surfH <= buttonThreshold) {
        /* Button surfaces: normal buttons are dual-frame (height*2, normal+highlight
         * stacked), preview/grayed buttons are single-frame (original height). */
        int isDualFrame = (surfH > singleThreshold);
        frameH = isDualFrame ? surfH / 2 : surfH;
        frameTop = (isDualFrame && y >= frameH) ? frameH : 0;
        centeredY = frameTop + (frameH - sz.cy) / 2;
    } else {
        /* Non-button surface (info panels, etc.): use original Y */
        centeredY = y;
    }
    } /* end threshold scope */

    TextOutA(hdc, x, centeredY, str, len);

    SelectObject(hdc, old);
    FontFix_ReleaseDC(surf, hdc);
    return sz.cx;
}

/* --- Frontend replacement: MeasureOrDrawFrontendFontString (0x412D50) ---
 *     destSurf==NULL -> measure only (returns width); else draw with large font */
static int __cdecl Hook_MeasureOrDrawFont(const char *str, unsigned int x,
                                           unsigned int y, void *destSurf) {
    /* if (!g_fontsScaled) FontFix_ScaleForCanvas(); — disabled */
    if (!str || !*str) return 0;
    if (!destSurf)
        return FontFix_Measure(str, g_hFontLarge) + 4;
    if (g_config.enable_widescreen_fix && g_hFontLargeNat) {
        FontFix_QueueText(str, (int)x, (int)y, 1);
        return FontFix_Measure(str, g_hFontLarge) + 4;
    }
    return FontFix_DrawText(destSurf, str, (int)x, (int)y, g_hFontLarge);
}

/* --- Frontend replacement: MeasureOrCenterFrontendLocalizedString (0x424A50) ---
 *     totalWidth==0 -> returns pixel width; else returns centered X position */
static int __cdecl Hook_MeasureOrCenter(const char *str, int startX, int totalWidth) {
    int textW;
    if (!str || !*str) return startX;
    textW = FontFix_Measure(str, g_hFontSmall);
    if (totalWidth == 0)
        return textW;
    return startX + (int)((unsigned int)(totalWidth - textW - startX)) / 2;
}

/* --- Frontend replacement: DrawFrontendSmallFontStringToSurface (0x424660) ---
 *     Used for small text drawn directly to arbitrary surfaces (not via localized wrappers). */
static int __cdecl Hook_DrawSmallFontToSurface(const char *str, unsigned int x, int y, void *surf) {
    int drawX;
    if (!str || !*str || !surf) return 0;
    if (!g_hFontSmall) return Orig_DrawSmallFontToSurface(str, x, y, surf);
    /* Clamp X to 0: centering formula wraps negative when text > surface width */
    drawX = (int)x;
    if (drawX < 0) drawX = 0;
    {
        int w = FontFix_DrawText(surf, str, drawX, y, g_hFontSmall);
        if (w > 0) return w;
        return Orig_DrawSmallFontToSurface(str, x, y, surf);
    }
}

/* --- Frontend replacement: DrawFrontendClippedStringToSurface (0x424740) ---
 *     Draws text to surface with a maximum pixel width budget (for word-wrapped text).
 *     Called by DrawFrontendWrappedStringLine for each word/segment. */
static int __cdecl Hook_DrawClippedToSurface(const char *str, int x, int y, void *surf,
                                              unsigned int maxWidth) {
    HDC hdc;
    HFONT old;
    SIZE sz;
    RECT clipRect;
    int len;

    if (!str || !*str || !surf) return 0;
    if (!g_hFontSmall) return Orig_DrawClippedToSurface(str, x, y, surf, maxWidth);

    hdc = FontFix_GetDC(surf);
    if (!hdc) return Orig_DrawClippedToSurface(str, x, y, surf, maxWidth);

    old = (HFONT)SelectObject(hdc, g_hFontSmall);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(255, 255, 255));

    len = lstrlenA(str);
    GetTextExtentPoint32A(hdc, str, len, &sz);

    /* Clip text to maxWidth budget */
    clipRect.left   = x;
    clipRect.top    = y;
    clipRect.right  = x + (int)maxWidth;
    clipRect.bottom = y + sz.cy + 2;
    ExtTextOutA(hdc, x, y, ETO_CLIPPED, &clipRect, str, len, NULL);

    SelectObject(hdc, old);
    FontFix_ReleaseDC(surf, hdc);
    return sz.cx;
}

/* --- Frontend: update per-character width tables with GDI metrics --- */
static void FontFix_UpdateWidthTables(void) {
    HDC hdc;
    HFONT old;
    DWORD prot;
    int c;
    SIZE sz;
    char ch;

    hdc = CreateCompatibleDC(NULL);
    if (!hdc) return;

    /* Small font tables */
    old = (HFONT)SelectObject(hdc, g_hFontSmall);
    VirtualProtect(g_smallFontAdvance, 128, PAGE_READWRITE, &prot);
    VirtualProtect(g_smallFontDisplay, 128, PAGE_READWRITE, &prot);
    VirtualProtect(g_smallFontYOffset, 128, PAGE_READWRITE, &prot);
    for (c = 0; c < 128; c++) {
        if (c < 0x20) {
            g_smallFontAdvance[c] = 0;
            g_smallFontDisplay[c] = 0;
            g_smallFontYOffset[c] = 0;
            continue;
        }
        ch = (char)c;
        GetTextExtentPoint32A(hdc, &ch, 1, &sz);
        g_smallFontAdvance[c] = (signed char)(sz.cx > 120 ? 120 : sz.cx);
        g_smallFontDisplay[c] = g_smallFontAdvance[c];
        g_smallFontYOffset[c] = 0;
    }

    /* Large font tables */
    SelectObject(hdc, g_hFontLarge);
    VirtualProtect(g_largeFontAdvance, 128, PAGE_READWRITE, &prot);
    VirtualProtect(g_largeFontDisplay, 128, PAGE_READWRITE, &prot);
    for (c = 0; c < 128; c++) {
        if (c < 0x20) {
            g_largeFontAdvance[c] = 0;
            g_largeFontDisplay[c] = 0;
            continue;
        }
        ch = (char)c;
        GetTextExtentPoint32A(hdc, &ch, 1, &sz);
        g_largeFontAdvance[c] = (signed char)(sz.cx > 120 ? 120 : sz.cx);
        g_largeFontDisplay[c] = g_largeFontAdvance[c];
    }

    SelectObject(hdc, old);
    DeleteDC(hdc);
    OutputDebugStringA("[TD5MOD] Font width tables updated\n");
}

/* --- Race HUD: Hook InitializeRaceHudFontGlyphAtlas (0x428240) ---
 *     After original init: save UV sizes, scale display sizes for resolution */
static fn_InitRaceHudFontAtlas Orig_InitRaceHudFontAtlas = NULL;

static void __cdecl Hook_InitRaceHudFontAtlas(void) {
    /* Call original; atlas is left unmodified. Scale is computed lazily
     * in Hook_QueueRaceHudText when g_renderWidthF/HeightF are valid. */
    Orig_InitRaceHudFontAtlas();
    g_hudScaleX = 1.0f;
    g_hudScaleY = 1.0f;
    OutputDebugStringA("[TD5MOD] HUD font atlas hook called\n");
}

/* --- Race HUD: Hook QueueRaceHudFormattedText (0x428320) ---
 *     Reimplementation that uses scaled atlas sizes for screen quads
 *     but original UV sizes for texture coordinates. */

typedef void (__cdecl *fn_QueueRaceHudTextVA)(int, int, int, int, const char *, ...);
static fn_QueueRaceHudTextVA Orig_QueueRaceHudText = NULL;

static void __cdecl Hook_QueueRaceHudText(int fontIdx, int posX, int posY,
                                           int centered, const char *fmt, ...) {
    char buf[256];
    va_list ap;
    int len, i;
    float curX, curY;
    float totalScaledW;
    float *atlas;
    int atlasOff;
    int queueOff;
    uint32_t texData;

    va_start(ap, fmt);
    wvsprintfA(buf, fmt, ap);
    va_end(ap);

    len = lstrlenA(buf);
    if (len <= 0 || g_hudGlyphQueueCount + len >= 0x101) return;

    /* Compute scale factors every call from current render resolution.
     * HUD coordinates are screen pixels in a centered coordinate system;
     * positions are scaled by width/640 and height/480 in the HUD layout.
     * Glyph sizes must scale the same way. */
    {
        /* 640x480 = original game design resolution (HUD layout baseline) */
        #define TD5_DESIGN_WIDTH  640.0f
        #define TD5_DESIGN_HEIGHT 480.0f
        float scX = g_renderWidthF / TD5_DESIGN_WIDTH;
        float scY = g_renderHeightF / TD5_DESIGN_HEIGHT;
        g_hudScaleX = (scX >= 1.0f) ? scX : 1.0f;
        g_hudScaleY = (scY >= 1.0f) ? scY : 1.0f;
    }

    atlasOff = fontIdx * 0x404;
    atlas = (float *)(uintptr_t)(g_hudGlyphAtlasBase + atlasOff);

    /* Map characters through the HUD lookup table */
    for (i = 0; i < len; i++)
        buf[i] = (char)g_hudCharMap[(unsigned char)buf[i]];

    /* Compute total SCALED width for centering.
     * Atlas stores original glyph sizes (unmodified); we scale for screen. */
    totalScaledW = 0.0f;
    for (i = 0; i < len; i++)
        totalScaledW += atlas[(unsigned char)buf[i] * 4 + 2] * g_hudScaleX;

    curX = (float)posX;
    curY = (float)posY;
    if (centered) {
        /* Original formula: center offset = ((len-1) + totalWidth) * 0.5
         * The (len-1) accounts for 1-pixel gaps; we scale those too. */
        curX -= ((float)(len - 1) * g_hudScaleX + totalScaledW) * 0.5f;
    }

    /* Get texture data from the atlas (pointer at +0x400, then field at +0x3C) */
    texData = *(uint32_t *)((uintptr_t)atlas + 0x400);
    texData = *(uint32_t *)(texData + 0x3C);

    queueOff = (int)(uintptr_t)g_hudGlyphQueueBuf + g_hudGlyphQueueCount * 0xB8;

    for (i = 0; i < len; i++) {
        int glyphIdx = (unsigned char)buf[i];
        float *glyph = &atlas[glyphIdx * 4];
        float uStart = glyph[0];                 /* UV origin X */
        float vStart = glyph[1];                  /* UV origin Y */
        float uvW    = glyph[2];                  /* original glyph width (UV extent) */
        float uvH    = glyph[3];                  /* original glyph height (UV extent) */
        float scrW   = uvW * g_hudScaleX;         /* scaled screen width */
        float scrH   = uvH * g_hudScaleY;         /* scaled screen height */
        float scrR   = curX + scrW;
        float scrB   = curY + scrH;
        float uEnd   = uStart + uvW;             /* UV uses original size */
        float vEnd   = vStart + uvH;

        union { int i; float f; } tmpl[28];
        tmpl[0].i  = queueOff;
        tmpl[1].i  = (int)0xFFFFFFFFu;
        /* Screen X: v0=left, v1=left, v2=right, v3=right */
        tmpl[2].f  = curX;
        tmpl[3].f  = curX;
        tmpl[4].f  = scrR;
        tmpl[5].f  = scrR;
        /* Screen Y: v0=top, v1=bottom, v2=bottom, v3=top */
        tmpl[6].f  = curY;
        tmpl[7].f  = scrB;
        tmpl[8].f  = scrB;
        tmpl[9].f  = curY;
        /* Z/rhw = 128.1f */
        tmpl[10].i = 0x4300199A;
        tmpl[11].i = 0x4300199A;
        tmpl[12].i = 0x4300199A;
        tmpl[13].i = 0x4300199A;
        /* UV u: original sizes (not scaled) */
        tmpl[14].f = uStart;
        tmpl[15].f = uStart;
        tmpl[16].f = uEnd;
        tmpl[17].f = uEnd;
        /* UV v: original sizes (not scaled) */
        tmpl[18].f = vStart;
        tmpl[19].f = vEnd;
        tmpl[20].f = vEnd;
        tmpl[21].f = vStart;
        /* Colors = white opaque */
        tmpl[22].i = (int)0xFFFFFFFFu;
        tmpl[23].i = (int)0xFFFFFFFFu;
        tmpl[24].i = (int)0xFFFFFFFFu;
        tmpl[25].i = (int)0xFFFFFFFFu;
        /* Primitive type + texture */
        tmpl[26].i = 0;
        tmpl[27].i = (int)texData;

        TD5_BuildSpriteQuadTemplate((int *)tmpl);

        queueOff += 0xB8;
        curX += scrW + 1.0f * g_hudScaleX;  /* advance by scaled width + scaled gap */
    }

    g_hudGlyphQueueCount += len;
}

/* --- Font Fix: create fonts and install all hooks --- */
static int InstallFontFixHooks(void) {
    int ok = 1;

    /* Load TTF from file if specified */
    if (g_config.font_file[0]) {
        g_fontFileLoaded = AddFontResourceExA(g_config.font_file, FR_PRIVATE, 0);
        if (g_fontFileLoaded) {
            char msg[MAX_PATH + 48];
            wsprintfA(msg, "[TD5MOD] Loaded font file: %s\n", g_config.font_file);
            OutputDebugStringA(msg);
        } else {
            char msg[MAX_PATH + 48];
            wsprintfA(msg, "[TD5MOD] FAILED to load font: %s\n", g_config.font_file);
            OutputDebugStringA(msg);
        }
    }

    /* Create GDI fonts at 640x480 virtual resolution (for measurement + button surfaces) */
    g_hFontSmall = CreateFontA(
        -g_config.font_size_small, 0, 0, 0, g_config.font_weight,
        FALSE, FALSE, FALSE,
        ANSI_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
        ANTIALIASED_QUALITY, FF_SWISS | VARIABLE_PITCH,
        g_config.font_name);

    g_hFontLarge = CreateFontA(
        -g_config.font_size_large, 0, 0, 0, g_config.font_weight,
        FALSE, FALSE, FALSE,
        ANSI_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
        ANTIALIASED_QUALITY, FF_SWISS | VARIABLE_PITCH,
        g_config.font_name);

    if (!g_hFontSmall || !g_hFontLarge) {
        OutputDebugStringA("[TD5MOD] FAILED to create GDI fonts\n");
        return 0;
    }

    /* Get font metrics for vertical centering */
    {
        HDC hdc = CreateCompatibleDC(NULL);
        if (hdc) {
            TEXTMETRICA tm;
            HFONT old = (HFONT)SelectObject(hdc, g_hFontSmall);
            GetTextMetricsA(hdc, &tm);
            g_smallFontHeight = tm.tmHeight;
            g_smallFontLead   = tm.tmInternalLeading;
            SelectObject(hdc, g_hFontLarge);
            GetTextMetricsA(hdc, &tm);
            g_largeFontHeight = tm.tmHeight;
            SelectObject(hdc, old);
            DeleteDC(hdc);
            {
                char msg[128];
                wsprintfA(msg, "[TD5MOD] Font metrics: small H=%d lead=%d, large H=%d\n",
                          g_smallFontHeight, g_smallFontLead, g_largeFontHeight);
                OutputDebugStringA(msg);
            }
        }
    }

    /* Create native-resolution fonts for crisp front-surface rendering.
     * Scale font sizes by content_height / 480 (from widescreen layout). */
    if (g_config.enable_widescreen_fix && g_ws.content_h > g_ws.game_base_h) {
        float natScale = (float)g_ws.content_h / (float)g_ws.game_base_h;
        int natSmall = (int)(g_config.font_size_small * natScale);
        int natLarge = (int)(g_config.font_size_large * natScale);

        g_hFontSmallNat = CreateFontA(
            -natSmall, 0, 0, 0, g_config.font_weight,
            FALSE, FALSE, FALSE,
            ANSI_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
            ANTIALIASED_QUALITY, FF_SWISS | VARIABLE_PITCH,
            g_config.font_name);

        g_hFontLargeNat = CreateFontA(
            -natLarge, 0, 0, 0, g_config.font_weight,
            FALSE, FALSE, FALSE,
            ANSI_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
            ANTIALIASED_QUALITY, FF_SWISS | VARIABLE_PITCH,
            g_config.font_name);

        {
            char msg[128];
            wsprintfA(msg, "[TD5MOD] Native fonts: small=%d large=%d\n", natSmall, natLarge);
            OutputDebugStringA(msg);
        }
    }

    /* Update width tables so the game's layout code uses GDI metrics */
    FontFix_UpdateWidthTables();

    /* Description line spacing patches — DISABLED for investigation
     * (was patching ADD EBX,0xC to ADD EBX,fontHeight+2 at 3 addresses) */

    /* Frontend hooks (5 localized text wrappers) */
    #define HOOK_FONT(addr, hook, orig) \
        if (MH_CreateHook((LPVOID)(addr), (LPVOID)&(hook), (LPVOID *)&(orig)) == MH_OK) { \
            MH_EnableHook((LPVOID)(addr)); \
        } else { \
            OutputDebugStringA("[TD5MOD] FAILED font hook at " #addr "\n"); \
            ok = 0; \
        }

    HOOK_FONT(VA_DrawLocPrimary,   Hook_DrawLocPrimary,   Orig_DrawLocPrimary);
    HOOK_FONT(VA_DrawLocSecondary, Hook_DrawLocSecondary,  Orig_DrawLocSecondary);
    HOOK_FONT(VA_DrawLocToSurface, Hook_DrawLocToSurface,  Orig_DrawLocToSurface);
    HOOK_FONT(VA_MeasureOrDraw,    Hook_MeasureOrDrawFont, Orig_MeasureOrDrawFont);
    HOOK_FONT(VA_MeasureOrCenter,  Hook_MeasureOrCenter,   Orig_MeasureOrCenter);

    /* Additional small-font renderers (word-wrap, clipped text, etc.) */
    HOOK_FONT(0x00424660, Hook_DrawSmallFontToSurface, Orig_DrawSmallFontToSurface);
    HOOK_FONT(0x00424740, Hook_DrawClippedToSurface,   Orig_DrawClippedToSurface);

    /* Race HUD hooks (atlas init + text queue) */
    HOOK_FONT(0x00428240, Hook_InitRaceHudFontAtlas, Orig_InitRaceHudFontAtlas);
    HOOK_FONT(TD5_QueueRaceHudFormattedText_VA, Hook_QueueRaceHudText, Orig_QueueRaceHudText);

    #undef HOOK_FONT

    {
        char msg[128];
        wsprintfA(msg, "[TD5MOD] Font fix installed: font=%s weight=%d\n",
                  g_config.font_name, g_config.font_weight);
        OutputDebugStringA(msg);
    }
    return ok;
}

/* =========================================================================
 * Feature: Frontend Scaling (resolution-independent element positioning)
 *
 * When the game runs at resolutions above 640x480, frontend elements
 * (buttons, sprites, overlays) are positioned using center +/- offset.
 * The center is correct for native resolution, but offsets are hardcoded
 * for 640x480. We hook 3 gateway functions to scale these offsets.
 *
 * Formula: scaled_pos = center + (pos - center) * (canvasSize / 640or480)
 * ========================================================================= */

/* --- Frontend scaling originals --- */
typedef int* (__cdecl *fn_CreateButton)(const char*, int, int, unsigned int, int, int);
typedef void (__cdecl *fn_MoveSpriteRect)(int, int, int);
typedef void (__cdecl *fn_QueueOverlayRect)(int, int, int, int, int, int, int, int);
typedef void* (__cdecl *fn_CreateTrackedSurface)(int, int);
typedef void  (__cdecl *fn_LoadTgaToSurface)(const char*, const char*);
typedef void  (__cdecl *fn_Copy16BitRect)(int, int, void*, void*, int);

static fn_CreateButton       Orig_CreateButton       = NULL;
static fn_MoveSpriteRect     Orig_MoveSpriteRect      = NULL;
static fn_QueueOverlayRect   Orig_QueueOverlayRect   = NULL;
static fn_CreateTrackedSurface Orig_CreateTrackedSurface = NULL;
static fn_LoadTgaToSurface   Orig_LoadTgaToSurface   = NULL;
static fn_Copy16BitRect      Orig_Copy16BitRect      = NULL;

/* --- Vector button background (replaces bitmap 9-slice from ButtonBits.tga) --- */
typedef void (__cdecl *fn_DrawButtonBg)(void*, int, int, int, int);
static fn_DrawButtonBg Orig_DrawButtonBg = NULL;

static void __cdecl Hook_DrawButtonBg(void *surface, int yOffset, int width, int height, int style) {
    HDC hdc;
    HPEN oldPen;
    HBRUSH oldBrush;
    int cw = g_frontendCanvasW;
    float scale = (cw > 640) ? (float)cw / 640.0f : 1.0f;
    int borderW, cornerR, i;
    int x0, y0, x1, y1;

    /* Colors from pixel analysis of original ButtonBits.png:
     * Gold section: outer (107,90,0) → mid (156,140,0) → inner (214,198,0), fill (57,33,82)
     * Blue section: outer (0,24,123) → mid (33,74,255) → inner (140,165,255), fill transparent */
    typedef struct { COLORREF outer, mid, inner; COLORREF fill; int has_fill; } ButtonStyle;
    ButtonStyle styles[3] = {
        { RGB(107,90,0),   RGB(173,156,0),  RGB(214,198,0),   RGB(57,33,82), 1 },  /* 0: highlight/gold */
        { RGB(0,24,123),   RGB(33,74,255),  RGB(140,165,255), RGB(0,0,0),    0 },   /* 1: normal/blue */
        { RGB(0,24,123),   RGB(33,74,255),  RGB(90,123,255),  RGB(20,20,40), 1 },   /* 2: preview */
    };
    ButtonStyle *s;

    if (!surface) { Orig_DrawButtonBg(surface, yOffset, width, height, style); return; }

    hdc = FontFix_GetDC(surface);
    if (!hdc) { Orig_DrawButtonBg(surface, yOffset, width, height, style); return; }

    if (style < 0 || style > 2) style = 1;
    s = &styles[style];

    /* Stadium/capsule shape: corner radius = half the frame height
     * This creates semicircular ends matching the original ButtonBits shape */
    cornerR = height;  /* Full height for RoundRect ellipse = semicircle ends */
    borderW = (int)(3.0f * scale);
    if (borderW < 2) borderW = 2;

    x0 = 0;
    y0 = yOffset;
    x1 = width;
    y1 = yOffset + height;

    /* Fill background first */
    if (s->has_fill) {
        HBRUSH fillBrush = CreateSolidBrush(s->fill);
        oldPen = (HPEN)SelectObject(hdc, GetStockObject(NULL_PEN));
        oldBrush = (HBRUSH)SelectObject(hdc, fillBrush);
        RoundRect(hdc, x0, y0, x1, y1, cornerR, cornerR);
        SelectObject(hdc, oldBrush);
        SelectObject(hdc, oldPen);
        DeleteObject(fillBrush);
    }

    /* Gradient border: draw overlapping layers from outer (thick+dark) to inner (thin+bright).
     * Each layer is a full RoundRect outline that covers the previous one's inner edge,
     * creating a smooth dark→bright gradient across the border width. */
    {
        int totalW = borderW;
        int numLayers = totalW + 1;
        if (numLayers < 3) numLayers = 3;
        if (numLayers > 8) numLayers = 8;

        for (i = 0; i < numLayers; i++) {
            /* Interpolate color from outer to inner */
            float t = (float)i / (float)(numLayers - 1);
            int r = (int)(GetRValue(s->outer) * (1.0f - t) + GetRValue(s->inner) * t);
            int g = (int)(GetGValue(s->outer) * (1.0f - t) + GetGValue(s->inner) * t);
            int b = (int)(GetBValue(s->outer) * (1.0f - t) + GetBValue(s->inner) * t);
            HPEN layerPen = CreatePen(PS_SOLID, 1, RGB(r, g, b));
            int cr = cornerR - i * 2;
            if (cr < 4) cr = 4;

            oldPen = (HPEN)SelectObject(hdc, layerPen);
            oldBrush = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));

            RoundRect(hdc, x0 + i, y0 + i, x1 - i, y1 - i, cr, cr);

            SelectObject(hdc, oldBrush);
            SelectObject(hdc, oldPen);
            DeleteObject(layerPen);
        }
    }

    FontFix_ReleaseDC(surface, hdc);
}

/* Scale a coordinate relative to canvas center.
 * pos is in native canvas space (e.g., center_1024 - offset_640).
 * We recover the 640-era position and rescale to native proportionally. */
static int fe_scale_x(int x) {
    int cw = g_frontendCanvasW;
    if (cw <= 640) return x;  /* no scaling needed at base res */
    {
        float scale = (float)cw / 640.0f;
        int center = cw / 2;
        return center + (int)((float)(x - center) * scale);
    }
}

static int fe_scale_y(int y) {
    int ch = g_frontendCanvasH;
    if (ch <= 480) return y;
    {
        float scale = (float)ch / 480.0f;
        int center = ch / 2;
        return center + (int)((float)(y - center) * scale);
    }
}

static int fe_scale_w(int w) {
    int cw = g_frontendCanvasW;
    if (cw <= 640) return w;
    return (int)((float)w * (float)cw / 640.0f);
}

static int fe_scale_h(int h) {
    int ch = g_frontendCanvasH;
    if (ch <= 480) return h;
    return (int)((float)h * (float)ch / 480.0f);
}

/* --- Hook: CreateFrontendDisplayModeButton (0x425DE0) --- */
static int* __cdecl Hook_CreateButton(const char *text, int x, int y,
                                       unsigned int w, int h, int flags) {
    return Orig_CreateButton(text, fe_scale_x(x), fe_scale_y(y),
                             (unsigned int)fe_scale_w((int)w), fe_scale_h(h), flags);
}

/* --- Hook: MoveFrontendSpriteRect (0x4259D0) --- */
static void __cdecl Hook_MoveSpriteRect(int idx, int x, int y) {
    Orig_MoveSpriteRect(idx, fe_scale_x(x), fe_scale_y(y));
}

/* --- Hook: QueueFrontendOverlayRect (0x425660) ---
 *     Only scale destination position, NOT width/height. The source surface
 *     is at original resolution — scaling w/h would read past its boundary. */
static void __cdecl Hook_QueueOverlayRect(int dstX, int dstY, int srcX, int srcY,
                                           int w, int h, int color, int surface) {
    Orig_QueueOverlayRect(fe_scale_x(dstX), fe_scale_y(dstY),
                          srcX, srcY,
                          w, h,
                          color, surface);
}

/* --- Hook: CreateTrackedFrontendSurface (0x411F00) ---
 *     Scale surface dimensions for info panels, description boxes, etc.
 *     Skip the main 640x480 work surfaces (handled by wrapper). */
static void* __cdecl Hook_CreateTrackedSurface(int w, int h) {
    int cw = g_frontendCanvasW;
    int ch = g_frontendCanvasH;
    /* Only scale small surfaces (info panels, not the 640x480 work surfaces) */
    if (cw > 640 && ch > 480 && w < 640 && h < 480) {
        int sw = (int)((float)w * (float)cw / 640.0f);
        int sh = (int)((float)h * (float)ch / 480.0f);
        return Orig_CreateTrackedSurface(sw, sh);
    }
    return Orig_CreateTrackedSurface(w, h);
}

/* --- Hook: LoadTgaToFrontendSurface16bpp (0x4125B0) ---
 *     The original loads 640x480 TGA into g_primaryWorkSurface via Lock/Unlock.
 *     The game's Unlock fires the wrapper's Surface4_Unlock. We signal the wrapper
 *     to stretch the content by setting an env var BEFORE calling the original.
 *     The wrapper checks this flag in Unlock and stretches if set.
 *
 *     Simpler approach: the original already locks, writes 640x480, unlocks.
 *     We re-lock after, stretch in-place, and unlock again. Use the SAME
 *     vtable the game uses (slot 25=Lock, slot 27=Unlock in IDirectDrawSurface4). */
static void __cdecl Hook_LoadTgaToSurface(const char *tgaName, const char *zipName) {
    int cw = g_frontendCanvasW;
    int ch = g_frontendCanvasH;
    /* Clear the work surface to black before TGA load — at native res the TGA
     * only fills 640x480 of the larger surface, leaving stale pixels on the right */
    if (cw > 640 && ch > 480 && g_primaryWorkSurface) {
        typedef void (__cdecl *fn_BltFill)(int, int, int, int, int, void*);
        ((fn_BltFill)0x00424050)(0, 0, 0, cw, ch, g_primaryWorkSurface);
    }
    Orig_LoadTgaToSurface(tgaName, zipName);
}

/* --- Hook: Copy16BitSurfaceRect (0x4251A0) ---
 *     Scale the destination coordinates for save/restore dirty-rect operations. */
static void __cdecl Hook_Copy16BitRect(int dstX, int dstY, void *dstSurf,
                                        void *srcRect, int flags) {
    /* Pass through — the rect coordinates are already in native space
     * since the surfaces are at native resolution. No scaling needed here
     * because the game computes these from the already-scaled button positions. */
    Orig_Copy16BitRect(dstX, dstY, dstSurf, srcRect, flags);
}

/* --- Gallery position globals --- */
#define g_galleryX   (*(int*)0x0048F2F4)
#define g_galleryY   (*(int*)0x0048F2F8)

/* --- Hook: UpdateExtrasGalleryDisplay (0x40D830) ---
 *     After the original sets gallery X/Y positions, scale them for native res. */
typedef void (__cdecl *fn_UpdateGallery)(void);
static fn_UpdateGallery Orig_UpdateGallery = NULL;

static void __cdecl Hook_UpdateGallery(void) {
    Orig_UpdateGallery();
    /* Scale the gallery position globals that were just set */
    {
        int cw = g_frontendCanvasW;
        int ch = g_frontendCanvasH;
        if (cw > 640 && ch > 480) {
            g_galleryX = (int)((float)g_galleryX * (float)cw / 640.0f);
            g_galleryY = (int)((float)g_galleryY * (float)ch / 480.0f);
        }
    }
}

/* --- Hook: AdvanceExtrasGallerySlideshow (0x40D750) ---
 *     Same — scale the random position after it's computed. */
typedef uint32_t (__cdecl *fn_AdvanceGallery)(void);
static fn_AdvanceGallery Orig_AdvanceGallery = NULL;

static uint32_t __cdecl Hook_AdvanceGallery(void) {
    uint32_t ret = Orig_AdvanceGallery();
    {
        int cw = g_frontendCanvasW;
        int ch = g_frontendCanvasH;
        if (cw > 640 && ch > 480) {
            g_galleryX = (int)((float)g_galleryX * (float)cw / 640.0f);
            g_galleryY = (int)((float)g_galleryY * (float)ch / 480.0f);
        }
    }
    return ret;
}

/* --- Install frontend scaling hooks --- */
static int InstallFrontendScaleHooks(void) {
    int ok = 1;

    /* Always install hooks — canvas globals aren't set until later.
     * The scaling functions check canvas size at call time. */

    #define HOOK_FE(addr, hook, orig) \
        if (MH_CreateHook((LPVOID)(addr), (LPVOID)&(hook), (LPVOID *)&(orig)) == MH_OK) { \
            MH_EnableHook((LPVOID)(addr)); \
        } else { \
            OutputDebugStringA("[TD5MOD] FAILED FE scale hook at " #addr "\n"); \
            ok = 0; \
        }

    /* HOOK_FE(0x00425B60, Hook_DrawButtonBg, Orig_DrawButtonBg); — GDI can't match original shape */
    HOOK_FE(0x00425DE0, Hook_CreateButton,        Orig_CreateButton);
    HOOK_FE(0x004259D0, Hook_MoveSpriteRect,     Orig_MoveSpriteRect);
    HOOK_FE(0x00425660, Hook_QueueOverlayRect,   Orig_QueueOverlayRect);
    /* Gallery hooks disabled — cause crash. Use runtime byte patches instead. */
    /* HOOK_FE(0x0040D830, Hook_UpdateGallery,      Orig_UpdateGallery); */
    /* HOOK_FE(0x0040D750, Hook_AdvanceGallery,     Orig_AdvanceGallery); */
    /* Disabled hooks — need more investigation */
    /* HOOK_FE(0x00411F00, Hook_CreateTrackedSurface, Orig_CreateTrackedSurface); */
    /* HOOK_FE(0x004125B0, Hook_LoadTgaToSurface,   Orig_LoadTgaToSurface); */

    #undef HOOK_FE

    {
        char msg[128];
        wsprintfA(msg, "[TD5MOD] Frontend scaling installed: canvas %dx%d (scale %.2fx%.2f)\n",
                  g_frontendCanvasW, g_frontendCanvasH,
                  (float)g_frontendCanvasW / 640.0f, (float)g_frontendCanvasH / 480.0f);
        OutputDebugStringA(msg);
    }
    return ok;
}

/* =========================================================================
 * Asset Override — load extracted loose files instead of ZIP contents
 *
 * Hooks ReadArchiveEntry (0x440790) and GetArchiveEntrySize (0x4409B0).
 * Maps the zipPath parameter to an assets/ subfolder, tries fopen() on the
 * loose file first, falls back to the original ZIP-based function.
 *
 * zipPath examples from game code:
 *   "level001.zip"                    -> assets/levels/level001/
 *   "static.zip"                      -> assets/static/
 *   "cars\cam.zip"                    -> assets/cars/cam/
 *   "traffic.zip"                     -> assets/traffic/
 *   "LOADING.ZIP"                     -> assets/loading/
 *   "legals.zip"                      -> assets/legals/
 *   "environs.zip"                    -> assets/environs/
 *   "SOUND\SOUND.ZIP"                 -> assets/sound/
 *   "Cup.zip"                         -> assets/cup/
 *   "Front End\FrontEnd.zip"          -> assets/frontend/
 *   "Front End\Extras\Extras.zip"     -> assets/extras/
 *   "Front End\Extras\Mugshots.zip"   -> assets/mugshots/
 *   "Front End\Sounds\Sounds.zip"     -> assets/sounds/
 *   "Front End\Tracks\Tracks.zip"     -> assets/tracks/
 * ========================================================================= */

static fn_ReadArchiveEntry   Original_ReadArchiveEntry   = NULL;
static fn_GetArchiveEntrySize Original_GetArchiveEntrySize = NULL;

/**
 * Map a zipPath to its assets/ subfolder. Returns 0 on success, -1 if unknown.
 * out_path receives the full path: "<assets_dir>/<subfolder>/<entryName>"
 */
static int AssetOverride_ResolvePath(const char *entryName, const char *zipPath,
                                     char *out_path, int out_size)
{
    const char *assets = g_config.assets_dir;
    const char *zp = zipPath;
    char subfolder[128];
    const char *basename;
    int n;

    /* Skip leading path separators */
    while (*zp == '\\' || *zp == '/') zp++;

    /* Match zipPath to subfolder */
    if (_strnicmp(zp, "level", 5) == 0 && zp[5] >= '0' && zp[5] <= '9') {
        /* level001.zip -> levels/level001 */
        /* Extract the level number portion (e.g., "001" from "level001.zip") */
        n = 0;
        while (zp[5 + n] && zp[5 + n] != '.') n++;
        snprintf(subfolder, sizeof(subfolder), "levels/level%.*s", n, zp + 5);
    }
    else if (_strnicmp(zp, "cars\\", 5) == 0 || _strnicmp(zp, "cars/", 5) == 0) {
        /* cars\cam.zip -> cars/cam */
        basename = zp + 5;
        n = 0;
        while (basename[n] && basename[n] != '.') n++;
        snprintf(subfolder, sizeof(subfolder), "cars/%.*s", n, basename);
    }
    else if (_strnicmp(zp, "static.zip", 10) == 0) {
        strcpy(subfolder, "static");
    }
    else if (_strnicmp(zp, "traffic.zip", 11) == 0) {
        strcpy(subfolder, "traffic");
    }
    else if (_strnicmp(zp, "environs.zip", 12) == 0) {
        strcpy(subfolder, "environs");
    }
    else if (_strnicmp(zp, "loading.zip", 11) == 0) {
        strcpy(subfolder, "loading");
    }
    else if (_strnicmp(zp, "legals.zip", 10) == 0) {
        strcpy(subfolder, "legals");
    }
    else if (_strnicmp(zp, "Cup.zip", 7) == 0) {
        strcpy(subfolder, "cup");
    }
    else if (_strnicmp(zp, "sound\\sound.zip", 15) == 0 ||
             _strnicmp(zp, "sound/sound.zip", 15) == 0) {
        strcpy(subfolder, "sound");
    }
    else if (_strnicmp(zp, "Front End\\FrontEnd.zip", 22) == 0 ||
             _strnicmp(zp, "Front End/FrontEnd.zip", 22) == 0 ||
             _strnicmp(zp, "Front End\\frontend.zip", 22) == 0 ||
             _strnicmp(zp, "Front End/frontend.zip", 22) == 0) {
        strcpy(subfolder, "frontend");
    }
    else if (_strnicmp(zp, "Front End\\Extras\\Extras.zip", 27) == 0 ||
             _strnicmp(zp, "Front End/Extras/Extras.zip", 27) == 0) {
        strcpy(subfolder, "extras");
    }
    else if (_strnicmp(zp, "Front End\\Extras\\Mugshots.zip", 29) == 0 ||
             _strnicmp(zp, "Front End/Extras/Mugshots.zip", 29) == 0) {
        strcpy(subfolder, "mugshots");
    }
    else if (_strnicmp(zp, "Front End\\Sounds\\Sounds.zip", 27) == 0 ||
             _strnicmp(zp, "Front End/Sounds/Sounds.zip", 27) == 0) {
        strcpy(subfolder, "sounds");
    }
    else if (_strnicmp(zp, "Front End\\Tracks\\Tracks.zip", 27) == 0 ||
             _strnicmp(zp, "Front End/Tracks/Tracks.zip", 27) == 0) {
        strcpy(subfolder, "tracks");
    }
    else {
        return -1;  /* Unknown ZIP — let original handle it */
    }

    /* Build full path: assets_dir/subfolder/entryName
     * Path components are short (assets=~6, subfolder=~20, entry=~20) so
     * MAX_PATH (260) is always sufficient. Silence truncation warning. */
    {
        int n = snprintf(out_path, out_size, "%s/%s/%s", assets, subfolder, entryName);
        if (n < 0 || n >= out_size) return -1;
    }

    /* Normalize backslashes to forward slashes for fopen */
    {
        char *p = out_path;
        while (*p) { if (*p == '\\') *p = '/'; p++; }
    }
    return 0;
}

static uint32_t __cdecl Hook_ReadArchiveEntry(const char *entryName,
                                               const char *zipPath,
                                               char *destBuf,
                                               uint32_t maxSize)
{
    char path[MAX_PATH];
    uint32_t bytesRead = 0;

    /* Side-channel: record current archive entry for texture dump categorization.
     * The ddraw wrapper reads this via GetEnvironmentVariable during texture dump. */
    {
        char tag[512];
        snprintf(tag, sizeof(tag), "%s|%s", zipPath ? zipPath : "", entryName ? entryName : "");
        SetEnvironmentVariableA("TD5_LAST_ARCHIVE", tag);
    }

    /* PNG-to-TGA replacement (highest priority for TGA files) */
    bytesRead = PngReplace_TryReplace(entryName, zipPath, destBuf, maxSize);
    if (bytesRead > 0)
        return bytesRead;

    /* Loose file override from assets/ directory */
    if (AssetOverride_ResolvePath(entryName, zipPath, path, sizeof(path)) == 0) {
        FILE *f = fopen(path, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            bytesRead = (uint32_t)ftell(f);
            if (bytesRead > maxSize) bytesRead = maxSize;
            fseek(f, 0, SEEK_SET);
            bytesRead = (uint32_t)fread(destBuf, 1, bytesRead, f);
            fclose(f);
            AssetDump_OnFileRead(entryName, zipPath, destBuf, bytesRead);
            return bytesRead;
        }
    }

    /* Original ZIP read */
    bytesRead = Original_ReadArchiveEntry(entryName, zipPath, destBuf, maxSize);
    AssetDump_OnFileRead(entryName, zipPath, destBuf, bytesRead);
    return bytesRead;
}

static uint32_t __cdecl Hook_GetArchiveEntrySize(const char *entryName,
                                                  const char *zipPath)
{
    char path[MAX_PATH];
    uint32_t size;

    /* PNG replacement size takes priority */
    size = PngReplace_GetTgaSize(entryName, zipPath);
    if (size > 0) return size;

    if (AssetOverride_ResolvePath(entryName, zipPath, path, sizeof(path)) == 0) {
        FILE *f = fopen(path, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            size = (uint32_t)ftell(f);
            fclose(f);
            return size;
        }
    }
    return Original_GetArchiveEntrySize(entryName, zipPath);
}

/* =========================================================================
 * Auto-Dump: programmatically cycle all tracks/cars to capture all textures
 *
 * Hooks NoOpHookStub (0x418450, called per-frame at 9 lifecycle points).
 * State machine: wait for menu → set car/track → trigger race →
 *                wait for textures → force exit → advance → repeat.
 *
 * Globals used:
 *   0x48F31C = selected car index (written by car select screen)
 *   0x4A2C90 = selected track schedule index
 *   0x4AAF6C = selected game type
 *   0x49524C = g_startRaceConfirmFlag (set to 1 → triggers race)
 *   0x4C3CE8 = g_gameState (0=intro, 1=menu, 2=race)
 * ========================================================================= */

#define ADDR_CAR_INDEX          (*(int32_t*)0x0048F31C)  /* menu cursor car */
#define ADDR_CAR_INDEX2         (*(int32_t*)0x0048F364)  /* g_selectedCarIndex */
#define ADDR_RACE_SLOT_CAR      (*(int32_t*)0x00497268)  /* race schedule: player car (read by InitRaceSession) */
#define ADDR_TRACK_SCHEDULE     (*(int32_t*)0x004A2C90)
#define ADDR_START_RACE_CONFIRM (*(int32_t*)0x0049524C)
#define ADDR_FRONTEND_INIT_PEND (*(int32_t*)0x00474C04)

/* Direct level ZIP numbers (from level%03d.zip filenames) */
static const int s_levelNumbers[] = {
    1, 2, 3, 4, 5, 6, 13, 14, 15, 16, 17, 23, 25, 26, 27, 28, 29, 30, 37, 39
};
#define LEVEL_COUNT (sizeof(s_levelNumbers) / sizeof(s_levelNumbers[0]))

/* g_currentLevelNumber: the level ZIP index used by InitializeRaceSession */
#define ADDR_LEVEL_NUMBER (*(int32_t*)0x004AAF3C)

/* Total cars in the game */
#define CAR_COUNT 37

enum {
    ADUMP_IDLE = 0,
    ADUMP_WAIT_MENU,
    ADUMP_SETUP,
    ADUMP_WAIT_RACE,
    ADUMP_WAIT_LOAD,
    ADUMP_EXIT_RACE,
    ADUMP_DONE
};

static int s_adump_state = ADUMP_IDLE;
static int s_adump_car = 0;
static int s_adump_track = 0;
static int s_adump_frame = 0;
static int s_adump_tick = 0;  /* prevent multi-call per frame (9 call sites) */
static fn_NoOpHookStub Original_NoOpHookStub = NULL;

/* Hook LoadRaceVehicleAssets (0x443280) to override the player's car index
 * right before the game loads car ZIP assets. */
typedef void (__cdecl *fn_LoadRaceVehicleAssets)(void);
static fn_LoadRaceVehicleAssets Original_LoadRaceVehicleAssets = NULL;

static void __cdecl Hook_LoadRaceVehicleAssets(void)
{
    /* No car override — let the game pick naturally.
     * The ASI asset dump captures all files from ReadArchiveEntry
     * regardless of which cars are loaded. */
    Original_LoadRaceVehicleAssets();
}

/* g_raceEndControl at 0x483980: race fade-out timer.
 * Set to >= 0x400000 to trigger BuildRaceResultsTable + race exit.
 * The game's normal cleanup path handles everything. */
#define ADDR_RACE_END_CONTROL (*(int32_t*)0x00483980)

static void __cdecl Hook_NoOpHookStub(void)
{
    /* Only run once per frame (first of the 9 call sites) */
    DWORD now = GetTickCount();
    if (now == (DWORD)s_adump_tick) return;
    s_adump_tick = (int)now;

    if (s_adump_state == ADUMP_IDLE) return;

    switch (s_adump_state) {
    case ADUMP_WAIT_MENU:
        if (g_gameState == 1) {
            s_adump_frame = 0;
            s_adump_state = ADUMP_SETUP;
        }
        break;

    case ADUMP_SETUP:
        /* Wait for menu to stabilize after race exit */
        s_adump_frame++;
        if (s_adump_frame < 60) break;

        {
            int levelNum = s_levelNumbers[s_adump_track];
            char msg[128];

            /* Rotate player car: advance by 2 per track to spread coverage.
             * 19 tracks × 2 = 38 > 37 cars, so we wrap and cover all. */
            s_adump_car = (s_adump_track * 2) % CAR_COUNT;

            /* Set car and track globals before generating the schedule */
            g_selectedCarIndex = s_adump_car;
            ADDR_TRACK_SCHEDULE = s_adump_track;
            g_selectedGameType = 0;  /* Quick Race */

            /* Clear schedule flag and generate fresh schedule */
            *(int32_t*)0x497324 = 0;
            TD5_InitializeRaceSeriesSchedule();

            /* Trigger race */
            ADDR_START_RACE_CONFIRM = 1;

            wsprintfA(msg, "[TD5MOD] AutoDump: track %d/%d (level%03d) car=%d\n",
                      s_adump_track + 1, 19, levelNum, s_adump_car);
            OutputDebugStringA(msg);
        }
        s_adump_frame = 0;
        s_adump_state = ADUMP_WAIT_RACE;
        break;

    case ADUMP_WAIT_RACE:
        if (g_gameState == 2) {
            s_adump_frame = 0;
            s_adump_state = ADUMP_WAIT_LOAD;
        }
        /* Timeout: if race doesn't start in 300 ticks, skip */
        s_adump_frame++;
        if (s_adump_frame > 300) {
            OutputDebugStringA("[TD5MOD] AutoDump: timeout waiting for race\n");
            s_adump_state = ADUMP_EXIT_RACE;
        }
        break;

    case ADUMP_WAIT_LOAD:
        /* Wait for textures to load and be dumped */
        s_adump_frame++;
        if (s_adump_frame >= g_config.auto_dump_load_frames) {
            s_adump_state = ADUMP_EXIT_RACE;
        }
        break;

    case ADUMP_EXIT_RACE:
        /* Trigger normal race completion: set fade-out timer past threshold.
         * The game calls BuildRaceResultsTable() and exits to results/menu. */
        ADDR_RACE_END_CONTROL = 0x400000;
        s_adump_frame = 0;

        /* Advance to next track (cap at 19 = skip drag strip at index 19) */
        s_adump_track++;
        if (s_adump_track >= 19) {
            OutputDebugStringA("[TD5MOD] AutoDump: COMPLETE — all tracks dumped\n");
            s_adump_state = ADUMP_DONE;
            break;
        }
        s_adump_state = ADUMP_WAIT_MENU;
        break;

    case ADUMP_DONE:
        /* Auto-disable so next launch doesn't restart the cycle */
        WritePrivateProfileStringA("AutoDump", "Enable", "0", g_iniPath);
        OutputDebugStringA("[TD5MOD] AutoDump: disabled in INI for next launch\n");
        s_adump_state = ADUMP_IDLE;
        break;
    }
}

/* =========================================================================
 * Hook Installation (entry point for all features)
 * ========================================================================= */

static int InstallHooks(void) {
    if (MH_Initialize() != MH_OK) {
        OutputDebugStringA("[TD5MOD] MinHook init failed\n");
        return 0;
    }

    /* --- Windowed Mode (hook DXWin::Initialize in M2DX.dll) --- */
    if (g_config.enable_windowed) {
        HMODULE m2dx = GetModuleHandleA("M2DX.dll");
        if (m2dx) {
            void *pInit = (void *)((uintptr_t)m2dx + 0x12a10);
            if (MH_CreateHook(pInit, &Hook_DXWin_Initialize,
                              (LPVOID *)&Original_DXWin_Initialize) == MH_OK) {
                MH_EnableHook(pInit);
                OutputDebugStringA("[TD5MOD] DXWin::Initialize hooked for windowed mode\n");
            } else {
                OutputDebugStringA("[TD5MOD] FAILED to hook DXWin::Initialize\n");
            }
        } else {
            OutputDebugStringA("[TD5MOD] WARNING: M2DX.dll not found for windowed hook\n");
        }
    }

    /* --- Widescreen + High-FPS (PatchMemory + CALL-site + MinHook) --- */
    if (g_config.enable_widescreen_fix) {
        ComputeWidescreenLayout();
        if (!ApplyWidescreenPatches())
            OutputDebugStringA("[TD5MOD] WARNING: some widescreen patches failed\n");

        /* Hook the software present path (normal frontend present).
         * The Flip path (screen dump only) is handled by CALL-site patch 6. */
        if (MH_CreateHook((LPVOID)0x00425360, &Hook_PresentSoftware,
                          (LPVOID *)&Original_PresentSoftware) == MH_OK) {
            MH_EnableHook((LPVOID)0x00425360);
            OutputDebugStringA("[TD5MOD] PresentFrontendBufferSoftware hooked\n");
        } else {
            OutputDebugStringA("[TD5MOD] FAILED to hook PresentFrontendBufferSoftware\n");
        }
    }

    /* --- Skip Intro Movie (RET at entry of PlayIntroMovie 0x43C440) --- */
    if (g_config.skip_intro) {
        uint8_t ret = 0xC3;
        PatchMemory((void *)0x0043C440, &ret, 1);
        OutputDebugStringA("[TD5MOD] Intro movie skipped\n");
    }

    /* --- Skip Legal Screens (RET at entry of ShowLegalScreens 0x42C8E0) --- */
    if (g_config.skip_legals) {
        uint8_t ret = 0xC3;
        PatchMemory((void *)0x0042C8E0, &ret, 1);
        OutputDebugStringA("[TD5MOD] Legal screens skipped\n");
    }

    /* --- Unlock all 19 circuit tracks (default 16, index 19 = drag strip, 20+ = cup entries) --- */
    *(int32_t *)0x00466840 = 19;
    OutputDebugStringA("[TD5MOD] Track count set to 19 (all circuit tracks visible)\n");

    /* --- Damage Wobble (direct memory patch, no MinHook) --- */
    if (g_config.enable_damage_wobble)
        PatchDamageWobble();

    /* --- Snow Rendering (MinHook on RenderAmbientParticleStreaks) --- */
    if (g_config.enable_snow_rendering) {
        if (MH_CreateHook((LPVOID)0x00446560, &Hook_RenderParticles,
                          (LPVOID *)&Original_RenderParticles) == MH_OK) {
            MH_EnableHook((LPVOID)0x00446560);
            OutputDebugStringA("[TD5MOD] Snow rendering hook installed\n");
        } else {
            OutputDebugStringA("[TD5MOD] FAILED to hook RenderAmbientParticleStreaks\n");
        }
    }

    /* --- Debug Overlay (MinHook on RenderRaceHudOverlays) --- */
    if (g_config.enable_debug_overlay) {
        if (MH_CreateHook((LPVOID)0x004388A0, &Hook_RenderRaceHudOverlays,
                          (LPVOID *)&Original_RenderRaceHudOverlays) == MH_OK) {
            MH_EnableHook((LPVOID)0x004388A0);
            OutputDebugStringA("[TD5MOD] Debug overlay hook installed\n");
        } else {
            OutputDebugStringA("[TD5MOD] FAILED to hook RenderRaceHudOverlays\n");
        }
    }

    /* --- Font Fix (GDI frontend text + HUD scaling) --- */
    if (g_config.enable_font_fix) {
        if (!InstallFontFixHooks())
            OutputDebugStringA("[TD5MOD] WARNING: some font fix hooks failed\n");
    }

    /* --- Frontend Scaling — DISABLED for investigation --- */
    /* if (!InstallFrontendScaleHooks())
        OutputDebugStringA("[TD5MOD] WARNING: some frontend scale hooks failed\n"); */

    /* --- Asset Dump + PNG Replace --- */
    {
        int asset_dump_active = AssetDump_Init(g_iniPath);
        int png_replace_active = PngReplace_Init(g_iniPath);

        /* --- ReadArchiveEntry hook (needed by AssetOverride / AssetDump / PngReplace) --- */
        if (g_config.enable_asset_override || asset_dump_active || png_replace_active) {
            if (MH_CreateHook((LPVOID)0x00440790, &Hook_ReadArchiveEntry,
                              (LPVOID *)&Original_ReadArchiveEntry) == MH_OK) {
                MH_EnableHook((LPVOID)0x00440790);
                OutputDebugStringA("[TD5MOD] ReadArchiveEntry hooked\n");
            } else {
                OutputDebugStringA("[TD5MOD] FAILED to hook ReadArchiveEntry\n");
            }
        }

        /* --- GetArchiveEntrySize hook (needed by AssetOverride / PngReplace) --- */
        if (g_config.enable_asset_override || png_replace_active) {
            if (MH_CreateHook((LPVOID)0x004409B0, &Hook_GetArchiveEntrySize,
                              (LPVOID *)&Original_GetArchiveEntrySize) == MH_OK) {
                MH_EnableHook((LPVOID)0x004409B0);
                OutputDebugStringA("[TD5MOD] GetArchiveEntrySize hooked\n");
            } else {
                OutputDebugStringA("[TD5MOD] FAILED to hook GetArchiveEntrySize\n");
            }
        }

        if (g_config.enable_asset_override) {
            char msg[MAX_PATH + 64];
            wsprintfA(msg, "[TD5MOD] Asset override: dir=%s\n", g_config.assets_dir);
            OutputDebugStringA(msg);
        }
    }

    /* --- Auto-Dump (cycle all tracks/cars for texture capture) --- */
    if (g_config.enable_auto_dump) {
        if (MH_CreateHook((LPVOID)0x00418450, &Hook_NoOpHookStub,
                          (LPVOID *)&Original_NoOpHookStub) == MH_OK &&
            MH_CreateHook((LPVOID)0x00443280, &Hook_LoadRaceVehicleAssets,
                          (LPVOID *)&Original_LoadRaceVehicleAssets) == MH_OK) {
            MH_EnableHook((LPVOID)0x00418450);
            MH_EnableHook((LPVOID)0x00443280);
            s_adump_state = ADUMP_WAIT_MENU;
            OutputDebugStringA("[TD5MOD] AutoDump: enabled, will cycle 20 tracks x 37 cars\n");
        } else {
            OutputDebugStringA("[TD5MOD] FAILED to hook for auto-dump\n");
        }
    }

    /* --- AI / Collision / Physics Tuning (INI-driven overrides) --- */
    {
        char tuning_ini[MAX_PATH];
        char *ts;
        GetModuleFileNameA(NULL, tuning_ini, MAX_PATH);
        ts = strrchr(tuning_ini, '\\');
        if (ts) strcpy(ts + 1, "scripts\\td5_mod.ini");

        Tuning_Init(tuning_ini);
        Tuning_InstallHooks();
    }

    OutputDebugStringA("[TD5MOD] All hooks installed\n");
    return 1;
}

/* =========================================================================
 * DLL Entry Point
 * ========================================================================= */

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID reserved) {
    (void)reserved;

    switch (reason) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hModule);

        /* Strip DWM8And16BitMitigation from compat layer BEFORE DDrawCompat
         * initializes. This prevents Windows from forcing classic borders. */
        SetEnvironmentVariableA("__COMPAT_LAYER", "HighDpiAware");

        LoadConfig();

        {
            char msg[160];
            wsprintfA(msg,
                "[TD5MOD] Config: Wobble=%d Snow=%d WS=%d Debug=%d Font=%d Win=%d\n",
                g_config.enable_damage_wobble, g_config.enable_snow_rendering,
                g_config.enable_widescreen_fix, g_config.enable_debug_overlay,
                g_config.enable_font_fix, g_config.enable_windowed);
            OutputDebugStringA(msg);
        }

        if (!InstallHooks()) {
            OutputDebugStringA("[TD5MOD] Hook installation failed!\n");
            return FALSE;
        }
        break;

    case DLL_PROCESS_DETACH:
        PngReplace_Shutdown();
        AssetDump_Shutdown();
        MH_DisableHook(MH_ALL_HOOKS);
        MH_Uninitialize();
        if (g_hFontSmall)    { DeleteObject(g_hFontSmall);    g_hFontSmall = NULL; }
        if (g_hFontLarge)    { DeleteObject(g_hFontLarge);    g_hFontLarge = NULL; }
        if (g_hFontSmallNat) { DeleteObject(g_hFontSmallNat); g_hFontSmallNat = NULL; }
        if (g_hFontLargeNat) { DeleteObject(g_hFontLargeNat); g_hFontLargeNat = NULL; }
        if (g_hMeasureDC)    { DeleteDC(g_hMeasureDC);        g_hMeasureDC = NULL; }
        if (g_fontFileLoaded && g_config.font_file[0])
            RemoveFontResourceExA(g_config.font_file, FR_PRIVATE, 0);
        OutputDebugStringA("[TD5MOD] Unloaded\n");
        break;
    }

    return TRUE;
}
