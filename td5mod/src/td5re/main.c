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
#include <mmsystem.h>
#include <d3d11.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

/* TD5RE headers */
#include "td5re.h"
#include "td5_platform.h"

/* Wrapper backend types and functions */
#include "../../ddraw_wrapper/src/wrapper.h"

/* Bootstrap logging — routes through the platform logger (engine.log).
 * Also echoes to OutputDebugString for IDE visibility. */
static void dbglog(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    OutputDebugStringA(buf);
    OutputDebugStringA("\n");
    /* Forward to the centralized platform logger (engine category) */
    td5_plat_log(TD5_LOG_INFO, "main", "%s", buf);
}

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

/* Crash handler: logs the faulting address + stack walk before terminating. */
static LONG WINAPI td5_crash_handler(EXCEPTION_POINTERS *ep)
{
    char crash_msg[2048];
    int pos = 0;
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    void *addr = ep->ExceptionRecord->ExceptionAddress;
    void *fault_addr = (ep->ExceptionRecord->NumberParameters >= 2)
        ? (void *)ep->ExceptionRecord->ExceptionInformation[1] : NULL;
    pos += snprintf(crash_msg + pos, sizeof(crash_msg) - pos,
        "CRASH: code=0x%08lX at EIP=%p access=%p\n"
        "  EAX=0x%08lX EBX=0x%08lX ECX=0x%08lX EDX=0x%08lX\n"
        "  ESI=0x%08lX EDI=0x%08lX EBP=0x%08lX ESP=0x%08lX\n",
        code, addr, fault_addr,
        (unsigned long)ep->ContextRecord->Eax,
        (unsigned long)ep->ContextRecord->Ebx,
        (unsigned long)ep->ContextRecord->Ecx,
        (unsigned long)ep->ContextRecord->Edx,
        (unsigned long)ep->ContextRecord->Esi,
        (unsigned long)ep->ContextRecord->Edi,
        (unsigned long)ep->ContextRecord->Ebp,
        (unsigned long)ep->ContextRecord->Esp);
    /* Walk stack frames via EBP chain */
    {
        DWORD *ebp = (DWORD *)(uintptr_t)ep->ContextRecord->Ebp;
        DWORD eip = (DWORD)(uintptr_t)ep->ContextRecord->Eip;
        pos += snprintf(crash_msg + pos, sizeof(crash_msg) - pos, "  Stack: %08lX", (unsigned long)eip);
        for (int i = 0; i < 16 && ebp && !IsBadReadPtr(ebp, 8); i++) {
            DWORD ret = ebp[1];
            pos += snprintf(crash_msg + pos, sizeof(crash_msg) - pos, " <- %08lX", (unsigned long)ret);
            ebp = (DWORD *)(uintptr_t)ebp[0];
        }
        pos += snprintf(crash_msg + pos, sizeof(crash_msg) - pos, "\n");
    }
    /* Module info for crash EIP */
    {
        HMODULE hmod = NULL;
        char modname[MAX_PATH] = {0};
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)addr, &hmod) && hmod) {
            GetModuleFileNameA(hmod, modname, MAX_PATH);
            pos += snprintf(crash_msg + pos, sizeof(crash_msg) - pos,
                "  Module: %s base=%p offset=0x%08lX\n",
                modname, (void *)hmod, (unsigned long)((uintptr_t)addr - (uintptr_t)hmod));
        }
    }
    dbglog(crash_msg);
    td5_plat_log_flush();
    /* Also write to a dedicated crash file in the log directory */
    {
        char crash_path[600];
        const char *logdir = td5_plat_log_dir();
        snprintf(crash_path, sizeof(crash_path), "%scrash.log", logdir);
        {
            FILE *cf = fopen(crash_path, "w");
            if (cf) { fprintf(cf, "%s\n", crash_msg); fclose(cf); }
        }
    }
    MessageBoxA(NULL, crash_msg, "TD5RE Crash", MB_OK | MB_ICONERROR);
    return EXCEPTION_EXECUTE_HANDLER;
}

/* ========================================================================
 * INI configuration (td5re.ini next to executable)
 * ======================================================================== */

static char s_ini_path[MAX_PATH];

static void td5_load_ini(void)
{
    DWORD n = GetModuleFileNameA(NULL, s_ini_path, MAX_PATH);
    if (n > 0) {
        char *sl = s_ini_path + n;
        while (sl > s_ini_path && *sl != '\\' && *sl != '/') sl--;
        if (sl > s_ini_path) sl[1] = '\0';
        strcat(s_ini_path, "td5re.ini");
    } else {
        strcpy(s_ini_path, "td5re.ini");
    }
}

static int td5_ini_int(const char *section, const char *key, int fallback)
{
    return GetPrivateProfileIntA(section, key, fallback, s_ini_path);
}

/* ------------------------------------------------------------------------
 * Command-line override parser
 *
 * Accepts `--Key=N` tokens on the command line to override any key read
 * from td5re.ini. Applied AFTER the INI loads, so precedence is
 * CLI > td5re.ini > compiled defaults. Useful for one-off tests without
 * editing the INI (parallel runs, CI, skill invocations).
 *
 * Example:
 *   td5re.exe --DefaultTrack=15 --DefaultGameType=0 --AutoRace=1
 *
 * Keys are case-insensitive and match the INI names verbatim. Values are
 * integers (atoi). Unknown keys log a warning and are ignored. Width,
 * Height and Windowed live as locals in WinMain and are updated via
 * out-params. `--Help` / `-h` logs the full key list and exits.
 * ------------------------------------------------------------------------ */
typedef struct {
    const char *name;
    int        *target;
} CliOverride;

static int td5_apply_cli_overrides(const char *cmdline,
                                   int *pwidth, int *pheight, int *pwindowed)
{
    CliOverride table[] = {
        /* Display */
        { "Fogging",              &g_td5.ini.fog_enabled },
        { "SpeedUnits",           &g_td5.ini.speed_units },
        { "CameraDamping",        &g_td5.ini.camera_damping },
        /* Audio */
        { "SFXVolume",            &g_td5.ini.sfx_volume },
        { "MusicVolume",          &g_td5.ini.music_volume },
        { "SFXMode",              &g_td5.ini.sfx_mode },
        /* GameOptions */
        { "Laps",                 &g_td5.ini.laps },
        { "CheckpointTimers",     &g_td5.ini.checkpoint_timers },
        { "Traffic",              &g_td5.ini.traffic },
        { "Cops",                 &g_td5.ini.cops },
        { "Difficulty",           &g_td5.ini.difficulty },
        { "Dynamics",             &g_td5.ini.dynamics },
        { "Collisions",           &g_td5.ini.collisions },
        { "PlayerIsAI",           &g_td5.ini.player_is_ai },
        /* Game */
        { "DefaultCar",           &g_td5.ini.default_car },
        { "DefaultTrack",         &g_td5.ini.default_track },
        { "DefaultGameType",      &g_td5.ini.default_game_type },
        { "SkipIntro",            &g_td5.ini.skip_intro },
        { "DebugOverlay",         &g_td5.ini.debug_overlay },
        { "AutoRace",             &g_td5.ini.auto_race },
        { "StartScreen",          &g_td5.ini.start_screen },
        { "StartSpanOffset",      &g_td5.ini.start_span_offset },
        { "DefaultReverse",       &g_td5.ini.default_reverse },
        /* Trace */
        { "RaceTrace",            &g_td5.ini.race_trace_enabled },
        { "RaceTraceSlot",        &g_td5.ini.race_trace_slot },
        { "RaceTraceMaxFrames",   &g_td5.ini.race_trace_max_frames },
        { "AutoThrottle",         &g_td5.ini.auto_throttle },
        { "TraceFastForward",     &g_td5.ini.trace_fast_forward },
        { "RaceTraceMaxSimTicks", &g_td5.ini.race_trace_max_sim_ticks },
        /* Logging */
        { "LogEnabled",           &g_td5.ini.log_enabled },
        { "LogMinLevel",          &g_td5.ini.log_min_level },
        { "LogFrontend",          &g_td5.ini.log_frontend },
        { "LogRace",              &g_td5.ini.log_race },
        { "LogEngine",            &g_td5.ini.log_engine },
        { "LogWrapper",           &g_td5.ini.log_wrapper },
    };
    const size_t table_n = sizeof(table) / sizeof(table[0]);
    int n_applied = 0;

    if (!cmdline || !*cmdline) return 0;

    const char *p = cmdline;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;

        const char *start = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        size_t tok_len = (size_t)(p - start);

        if (tok_len < 2 || start[0] != '-') {
            dbglog("  CLI: skipping bare token '%.*s'", (int)tok_len, start);
            continue;
        }
        /* Accept --key=val; single-dash -h/--help shows usage. */
        if (tok_len == 2 && start[1] == 'h') goto show_help;
        if (tok_len >= 6 && _strnicmp(start, "--help", 6) == 0) goto show_help;
        if (start[1] != '-') {
            dbglog("  CLI: skipping '%.*s' (need --Key=Val)",
                   (int)tok_len, start);
            continue;
        }

        const char *key = start + 2;
        size_t key_max = tok_len - 2;
        const char *eq = memchr(key, '=', key_max);
        if (!eq) {
            dbglog("  CLI: skipping '%.*s' (no '=' separator)",
                   (int)tok_len, start);
            continue;
        }
        size_t klen = (size_t)(eq - key);
        int val = atoi(eq + 1);

        /* Special cases — Width/Height/Windowed are WinMain locals. */
        if (klen == 5 && _strnicmp(key, "Width", 5) == 0) {
            *pwidth = val;
            dbglog("  CLI override: Width=%d", val);
            n_applied++;
            continue;
        }
        if (klen == 6 && _strnicmp(key, "Height", 6) == 0) {
            *pheight = val;
            dbglog("  CLI override: Height=%d", val);
            n_applied++;
            continue;
        }
        if (klen == 8 && _strnicmp(key, "Windowed", 8) == 0) {
            *pwindowed = val;
            dbglog("  CLI override: Windowed=%d", val);
            n_applied++;
            continue;
        }

        size_t i;
        int matched = 0;
        for (i = 0; i < table_n; ++i) {
            if (strlen(table[i].name) == klen &&
                _strnicmp(key, table[i].name, klen) == 0) {
                *table[i].target = val;
                dbglog("  CLI override: %s=%d", table[i].name, val);
                matched = 1;
                n_applied++;
                break;
            }
        }
        if (!matched) {
            dbglog("  CLI: unknown key '%.*s' (ignored)", (int)klen, key);
        }
    }
    return n_applied;

show_help:
    {
        size_t i;
        dbglog("td5re.exe CLI overrides (--Key=N, case-insensitive):");
        dbglog("  --Width=N --Height=N --Windowed=N");
        for (i = 0; i < table_n; ++i) {
            dbglog("  --%s=N", table[i].name);
        }
    }
    return n_applied;
}

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
    (void)nCmdShow;

    SetUnhandledExceptionFilter(td5_crash_handler);

    /* Initialize multi-file logging (creates log/ dir, rotates old sessions) */
    td5_plat_log_init();

    /* Load INI before anything else */
    td5_load_ini();
    width    = td5_ini_int("Display", "Width", 0);
    height   = td5_ini_int("Display", "Height", 0);
    windowed = td5_ini_int("Display", "Windowed", 1);

    /* Display options */
    g_td5.ini.fog_enabled    = td5_ini_int("Display", "Fogging", 1);
    g_td5.ini.speed_units    = td5_ini_int("Display", "SpeedUnits", 0);
    g_td5.ini.camera_damping = td5_ini_int("Display", "CameraDamping", 5);

    /* Audio */
    g_td5.ini.sfx_volume    = td5_ini_int("Audio", "SFXVolume", 80);
    g_td5.ini.music_volume  = td5_ini_int("Audio", "MusicVolume", 80);
    g_td5.ini.sfx_mode      = td5_ini_int("Audio", "SFXMode", 0);

    /* Game options */
    g_td5.ini.laps               = td5_ini_int("GameOptions", "Laps", 0);
    g_td5.ini.checkpoint_timers  = td5_ini_int("GameOptions", "CheckpointTimers", 1);
    g_td5.ini.traffic            = td5_ini_int("GameOptions", "Traffic", 1);
    g_td5.ini.cops               = td5_ini_int("GameOptions", "Cops", 1);
    g_td5.ini.difficulty         = td5_ini_int("GameOptions", "Difficulty", 1);
    g_td5.ini.dynamics           = td5_ini_int("GameOptions", "Dynamics", 0);
    g_td5.ini.collisions         = td5_ini_int("GameOptions", "Collisions", 1);

    /* Game defaults */
    g_td5.ini.default_car       = td5_ini_int("Game", "DefaultCar", 0);
    g_td5.ini.default_track     = td5_ini_int("Game", "DefaultTrack", 0);
    g_td5.ini.default_game_type = td5_ini_int("Game", "DefaultGameType", 0);
    g_td5.ini.skip_intro        = td5_ini_int("Game", "SkipIntro", 1);
    g_td5.ini.debug_overlay     = td5_ini_int("Game", "DebugOverlay", 0);

    /* Benchmark mode: enables main-menu button 2 → TD5_GAMESTATE_BENCHMARK path.
     * Matches the original's dead-code button-2 branch (app+0x170 is never written
     * in TD5_d3d.exe). Default 0 = button 2 is 2-player. */
    g_td5.ini.enable_benchmark  = td5_ini_int("Debug", "EnableBenchmark", 0);

    /* Player-as-AI: forces slot 0 through td5_ai tick + physics AI dispatch,
     * matching the original attract-mode autopilot (InitializeRaceSession
     * 0x0042ACCF writes slot[0].state = 1 - g_attractModeDemoActive). */
    g_td5.ini.player_is_ai      = td5_ini_int("GameOptions", "PlayerIsAI", 0);

    /* Auto-race: skip frontend entirely, launch race with INI settings */
    g_td5.ini.auto_race             = td5_ini_int("Game", "AutoRace", 0);
    g_td5.ini.start_screen          = td5_ini_int("Game", "StartScreen", -1);

    /* Additive span shift for every actor spawn — mirrors the Frida hook on
     * InitializeActorTrackPose (0x00434350). 0 = vanilla grid. */
    g_td5.ini.start_span_offset     = td5_ini_int("Game", "StartSpanOffset", 0);
    g_td5.ini.default_reverse       = td5_ini_int("Game", "DefaultReverse", 0);

    /* Trace */
    g_td5.ini.race_trace_enabled    = td5_ini_int("Trace", "RaceTrace", 0);
    g_td5.ini.race_trace_slot       = td5_ini_int("Trace", "RaceTraceSlot", -1);
    g_td5.ini.race_trace_max_frames = td5_ini_int("Trace", "RaceTraceMaxFrames", 600);
    g_td5.ini.auto_throttle         = td5_ini_int("Trace", "AutoThrottle", 0);
    g_td5.ini.trace_fast_forward    = td5_ini_int("Trace", "TraceFastForward", 0);
    g_td5.ini.race_trace_max_sim_ticks =
        td5_ini_int("Trace", "RaceTraceMaxSimTicks", 0);

    /* Logging — defaults preserve historic behavior (master on, INFO+, all
     * categories on, wrapper on). Flip to A/B-test perf without rebuilding. */
    g_td5.ini.log_frontend_draw = td5_ini_int("Logging", "FrontendDraw", 0);
    g_td5.ini.log_enabled   = td5_ini_int("Logging", "Enabled",  1);
    g_td5.ini.log_min_level = td5_ini_int("Logging", "MinLevel", 1);
    g_td5.ini.log_frontend  = td5_ini_int("Logging", "Frontend", 1);
    g_td5.ini.log_race      = td5_ini_int("Logging", "Race",     1);
    g_td5.ini.log_engine    = td5_ini_int("Logging", "Engine",   1);
    g_td5.ini.log_wrapper   = td5_ini_int("Logging", "Wrapper",  1);

    g_td5.ini.loaded = 1;

    /* CLI overrides run AFTER the INI so `--DefaultTrack=15` wins over the
     * INI value. Pass lpCmdLine (may be empty) — width/height/windowed are
     * updated in-place since they are locals below. */
    {
        int n_cli = td5_apply_cli_overrides(lpCmdLine, &width, &height, &windowed);
        if (n_cli > 0)
            dbglog("=== %d CLI override(s) applied ===", n_cli);
    }

    /* Apply log filters now — Backend_Init runs after this and is the heaviest
     * wrapper-log emitter, so silencing here lets the perf A/B reflect startup
     * cost too, not just the steady-state race loop. */
    {
        unsigned int cat_mask = 0;
        if (g_td5.ini.log_frontend) cat_mask |= TD5_LOG_CAT_BIT_FRONTEND;
        if (g_td5.ini.log_race)     cat_mask |= TD5_LOG_CAT_BIT_RACE;
        if (g_td5.ini.log_engine)   cat_mask |= TD5_LOG_CAT_BIT_ENGINE;
        td5_plat_log_set_filters(g_td5.ini.log_enabled,
                                 g_td5.ini.log_min_level, cat_mask);
        /* Wrapper is gated by master AND its own per-sink switch — same
         * convention as the platform log categories. */
        wrapper_set_enabled(g_td5.ini.log_enabled && g_td5.ini.log_wrapper);
    }

    dbglog("=== td5re.ini loaded from: %s ===", s_ini_path);
    dbglog("  [Display] Width=%d Height=%d Windowed=%d", width, height, windowed);
    dbglog("  [Display] Fogging=%d SpeedUnits=%d CameraDamping=%d",
           g_td5.ini.fog_enabled, g_td5.ini.speed_units, g_td5.ini.camera_damping);
    dbglog("  [Audio]   SFXVolume=%d MusicVolume=%d SFXMode=%d",
           g_td5.ini.sfx_volume, g_td5.ini.music_volume, g_td5.ini.sfx_mode);
    dbglog("  [GameOpt] Laps=%d Timers=%d Traffic=%d Cops=%d Diff=%d Dyn=%d Coll=%d PlayerIsAI=%d",
           g_td5.ini.laps, g_td5.ini.checkpoint_timers, g_td5.ini.traffic,
           g_td5.ini.cops, g_td5.ini.difficulty, g_td5.ini.dynamics, g_td5.ini.collisions,
           g_td5.ini.player_is_ai);
    dbglog("  [Game]    Car=%d Track=%d GameType=%d SkipIntro=%d DebugOverlay=%d AutoRace=%d StartScreen=%d StartSpanOffset=%d",
           g_td5.ini.default_car, g_td5.ini.default_track, g_td5.ini.default_game_type,
           g_td5.ini.skip_intro, g_td5.ini.debug_overlay, g_td5.ini.auto_race,
           g_td5.ini.start_screen, g_td5.ini.start_span_offset);
    dbglog("  [Trace]   RaceTrace=%d Slot=%d MaxFrames=%d AutoThrottle=%d FastFwd=%d SimTickCap=%d",
           g_td5.ini.race_trace_enabled, g_td5.ini.race_trace_slot,
           g_td5.ini.race_trace_max_frames, g_td5.ini.auto_throttle,
           g_td5.ini.trace_fast_forward, g_td5.ini.race_trace_max_sim_ticks);
    dbglog("  [Logging] Enabled=%d MinLevel=%d Frontend=%d Race=%d Engine=%d Wrapper=%d",
           g_td5.ini.log_enabled, g_td5.ini.log_min_level,
           g_td5.ini.log_frontend, g_td5.ini.log_race,
           g_td5.ini.log_engine, g_td5.ini.log_wrapper);

    if (width <= 0 || height <= 0) {
        width  = GetSystemMetrics(SM_CXSCREEN);
        height = GetSystemMetrics(SM_CYSCREEN);
        if (width <= 0)  width  = 640;
        if (height <= 0) height = 480;
    }
    bpp = DEFAULT_BPP;

    /* Set Windows timer resolution to 1ms so Sleep() and GetTickCount()
     * are accurate. Without this, Sleep(16) may sleep ~30ms (15.6ms default). */
    timeBeginPeriod(1);

    /* All output goes to td5re_debug.log — no console window needed. */

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
    /* Pre-set target dimensions so Backend_CreateDevice skips its own INI read */
    g_backend.target_width  = width;
    g_backend.target_height = height;
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
    /* CWD stays at the project root — all assets are pre-extracted under
     * re/assets/.  No dependency on original/. */
    dbglog("Working directory: project root (assets in re/)");

    /* Update global state with actual render dimensions and INI settings */
    g_td5.render_width  = width;
    g_td5.render_height = height;
    g_td5.intro_movie_pending = g_td5.ini.skip_intro ? 0 : 1;
    dbglog("Step 4: Platform init OK (render=%dx%d, skip_intro=%d)", width, height, g_td5.ini.skip_intro);
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
            /* DEBUG: F5 triggers a race start to test loading screen.
             * Scans for the first available levelXXX.zip and uses that track. */
            if (msg.message == WM_KEYDOWN && msg.wParam == VK_F5
                    && g_td5.game_state == 1 /* MENU */) {
                int f5_track = 0;
                { char zippath[64]; int t;
                  for (t = 0; t < 40; t++) {
                      snprintf(zippath, sizeof(zippath), "level%03d.zip", t + 1);
                      if (td5_plat_file_exists(zippath)) { f5_track = t; break; }
                  }
                }
                g_td5.race_requested = 1;
                g_td5.track_index = f5_track;
                dbglog("DEBUG: F5 pressed -- race start on track_index=%d", f5_track);
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
    timeEndPeriod(1);
    td5_plat_log_flush();

    return 0;
}
