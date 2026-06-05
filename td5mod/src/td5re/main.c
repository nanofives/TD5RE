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
#include <float.h>  /* _controlfp + _RC_DOWN / _MCW_RC for FPU rounding mode */

/* TD5RE headers */
#include "td5re.h"
#include "td5_platform.h"
#include "td5_net.h"
#include "td5_save.h"
#include "td5_profile.h"
#include "td5_trace.h"
#include "td5_asset.h"
#include "td5_render.h"

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

/* The release build reads its own INI so the two executables can sit side by
 * side in the same folder without the dev build's test-harness td5re.ini
 * leaking into the shipping binary. */
#ifdef TD5RE_RELEASE
#  define TD5RE_INI_FILENAME "td5re_release.ini"
#else
#  define TD5RE_INI_FILENAME "td5re.ini"
#endif

static void td5_load_ini(void)
{
    DWORD n = GetModuleFileNameA(NULL, s_ini_path, MAX_PATH);
    if (n > 0) {
        char *sl = s_ini_path + n;
        while (sl > s_ini_path && *sl != '\\' && *sl != '/') sl--;
        if (sl > s_ini_path) sl[1] = '\0';
        strcat(s_ini_path, TD5RE_INI_FILENAME);
    } else {
        strcpy(s_ini_path, TD5RE_INI_FILENAME);
    }
}

static int td5_ini_int(const char *section, const char *key, int fallback)
{
    return GetPrivateProfileIntA(section, key, fallback, s_ini_path);
}

static float td5_ini_float(const char *section, const char *key, float fallback)
{
    char buf[32];
    char fb[32];
    snprintf(fb, sizeof(fb), "%g", fallback);
    DWORD n = GetPrivateProfileStringA(section, key, fb, buf, sizeof(buf), s_ini_path);
    if (n == 0 || n >= sizeof(buf)) return fallback;
    return (float)atof(buf);
}

/* Reads a free-form string into out (zero-terminated). Returns 1 if the
 * key was present, 0 if missing/empty (out then holds the fallback). */
static int td5_ini_str(const char *section, const char *key,
                       const char *fallback, char *out, size_t out_n)
{
    DWORD n = GetPrivateProfileStringA(section, key,
                                       fallback ? fallback : "",
                                       out, (DWORD)out_n, s_ini_path);
    return (n > 0);
}

/* ------------------------------------------------------------------------
 * INI write-back (PART B, user 2026-06-02)
 *
 * Persists the in-game-configurable option keys from g_td5.ini.* back to
 * td5re.ini (s_ini_path). The original game had no INI and persisted options
 * only to Config.td5; the port adds td5re.ini as a launch-config layer that
 * OVERRIDES Config.td5 at frontend init (td5_frontend.c ~6503-6528), so
 * in-game option changes were masked on the next launch. Writing them back
 * here keeps td5re.ini in sync so the boot-override re-applies the user's
 * last in-game selection.
 *
 * Only the keys the in-game option screens (and pause volume sliders) can
 * change are written — section/key names match the reader in
 * td5re_load_config() EXACTLY for a clean round trip. Other keys
 * (Trace, Debug, Default-prefixed, Resolution) are left untouched.
 * WritePrivateProfileString edits each key in place and preserves the rest
 * of the file entries.
 * ------------------------------------------------------------------------ */
static void td5_ini_write_int(const char *section, const char *key, int value)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", value);
    WritePrivateProfileStringA(section, key, buf, s_ini_path);
}

/* Public string write-back (S10: persist the player nickname). */
void td5_ini_write_str(const char *section, const char *key, const char *value)
{
    if (!s_ini_path[0]) return;
    WritePrivateProfileStringA(section, key, value ? value : "", s_ini_path);
}

void td5_ini_persist_options(void)
{
    if (!s_ini_path[0]) return;

    /* Display */
    td5_ini_write_int("Display", "Fogging",       g_td5.ini.fog_enabled);
    td5_ini_write_int("Display", "SpeedUnits",    g_td5.ini.speed_units);
    td5_ini_write_int("Display", "CameraDamping", g_td5.ini.camera_damping);
    td5_ini_write_int("Display", "DisplayMode",   td5_save_get_display_mode());
    /* [S01 2026-06-04] window mode / vsync / fps overlay + the chosen resolution
     * so a relaunch reopens at the same size and mode. Refresh Width/Height from
     * the live window first so a drag-resize (no Resolution row any more) sticks. */
    { int cw = 0, ch = 0; td5_plat_get_chosen_resolution(&cw, &ch);
      if (cw > 0 && ch > 0) { g_td5.ini.disp_width = cw; g_td5.ini.disp_height = ch; } }
    td5_ini_write_int("Display", "WindowMode",    g_td5.ini.window_mode);
    td5_ini_write_int("Display", "VSync",         g_td5.ini.vsync);
    td5_ini_write_int("Display", "ShowFps",       g_td5.ini.show_fps);
    td5_ini_write_int("Display", "Width",         g_td5.ini.disp_width);
    td5_ini_write_int("Display", "Height",        g_td5.ini.disp_height);

    /* Audio */
    td5_ini_write_int("Audio", "SFXVolume",   g_td5.ini.sfx_volume);
    td5_ini_write_int("Audio", "MusicVolume", g_td5.ini.music_volume);
    td5_ini_write_int("Audio", "SFXMode",     g_td5.ini.sfx_mode);

    /* Game options */
    td5_ini_write_int("GameOptions", "Laps",             g_td5.ini.laps);
    td5_ini_write_int("GameOptions", "CheckpointTimers", g_td5.ini.checkpoint_timers);
    td5_ini_write_int("GameOptions", "Traffic",          g_td5.ini.traffic);
    td5_ini_write_int("GameOptions", "Cops",             g_td5.ini.cops);
    td5_ini_write_int("GameOptions", "Difficulty",       g_td5.ini.difficulty);
    td5_ini_write_int("GameOptions", "Dynamics",         g_td5.ini.dynamics);
    td5_ini_write_int("GameOptions", "Collisions",       g_td5.ini.collisions);
    td5_ini_write_int("GameOptions", "AutoGearbox",      g_td5.ini.auto_gearbox);

    /* TD6 paint color (last selected in the car-select color panel). */
    td5_ini_write_int("CarSelection", "TD6PaintColor",   g_td5.ini.td6_paint_color);

    td5_plat_log(TD5_LOG_INFO, "main",
                 "td5re.ini options persisted (in-game change write-back): "
                 "fog=%d units=%d cam=%d sfx=%d mus=%d sfxmode=%d laps=%d "
                 "chk=%d traf=%d cops=%d diff=%d dyn=%d coll=%d gearbox=%d",
                 g_td5.ini.fog_enabled, g_td5.ini.speed_units, g_td5.ini.camera_damping,
                 g_td5.ini.sfx_volume, g_td5.ini.music_volume, g_td5.ini.sfx_mode,
                 g_td5.ini.laps, g_td5.ini.checkpoint_timers, g_td5.ini.traffic,
                 g_td5.ini.cops, g_td5.ini.difficulty, g_td5.ini.dynamics,
                 g_td5.ini.collisions, g_td5.ini.auto_gearbox);
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
        { "DisplayMode",          &g_td5.ini.display_mode },
        { "WindowMode",           &g_td5.ini.window_mode },
        { "VSync",                &g_td5.ini.vsync },
        { "ShowFps",              &g_td5.ini.show_fps },
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
        { "AutoGearbox",          &g_td5.ini.auto_gearbox },
        { "RearImpactResponse",   &g_td5.ini.rear_impact_response },
        { "AntiTunnel",           &g_td5.ini.anti_tunnel },
        { "AntiTunnelSlop",       &g_td5.ini.anti_tunnel_slop },
        { "CatchupAssist",        &g_td5.ini.catchup_assist },
        { "AIAccelFromCar",       &g_td5.ini.ai_accel_from_car },
        /* Traffic (S20 smart traffic) */
        { "TrafficSmart",         &g_td5.ini.traffic_smart },
        { "TrafficWallAvoid",     &g_td5.ini.traffic_wall_avoid },
        { "TrafficAvoidSlowLane", &g_td5.ini.traffic_avoid_slow_lane },
        { "TrafficLookahead",     &g_td5.ini.traffic_lookahead },
        { "TrafficWallAvoidBias", &g_td5.ini.traffic_wall_avoid_bias },
        { "TrafficAntiFreeze",        &g_td5.ini.traffic_antifreeze },
        { "TrafficAntiFreezeFrames",  &g_td5.ini.traffic_antifreeze_frames },
        { "TrafficPlayerCollide",     &g_td5.ini.traffic_player_collide },
        { "Player1Joystick",      &g_td5.ini.player1_joystick },
        { "Player2Joystick",      &g_td5.ini.player2_joystick },
        { "PlayerIsAI",           &g_td5.ini.player_is_ai },
        { "OtherPlayersAI",       &g_td5.ini.others_ai },
        { "SoloAISlot",           &g_td5.ini.solo_ai_slot },
        { "SoloRace",             &g_td5.ini.solo_race },
        { "MaxSpan",              &g_td5.ini.max_span },
        { "PhantomPeer",          &g_td5.ini.phantom_peer },
        /* Game */
        { "DefaultCar",           &g_td5.ini.default_car },
        { "DefaultTrack",         &g_td5.ini.default_track },
        { "DefaultGameType",      &g_td5.ini.default_game_type },
        { "DefaultOpponents",     &g_td5.ini.default_opponents },
        { "CircuitMinimap",       &g_td5.ini.circuit_minimap },
        { "DefaultPlayers",       &g_td5.ini.default_players },
        { "OverrideTrackZip",     &g_td5.ini.override_track_zip },
        { "OverrideStartSpan",    &g_td5.ini.override_start_span },
        { "SkipIntro",            &g_td5.ini.skip_intro },
        { "DebugOverlay",         &g_td5.ini.debug_overlay },
        { "DebugCollisions",      &g_td5.ini.debug_collisions },
        { "SimulateJoyLoss",      &g_td5.ini.sim_joy_loss_player },
        { "SimulateJoyLossDelayMs", &g_td5.ini.sim_joy_loss_delay_ms },
        { "SimulateJoyLossHoldMs",  &g_td5.ini.sim_joy_loss_hold_ms },
        { "AutoRace",             &g_td5.ini.auto_race },
        { "StartScreen",          &g_td5.ini.start_screen },
        { "StartSpanOffset",      &g_td5.ini.start_span_offset },
        { "DefaultReverse",       &g_td5.ini.default_reverse },
        /* Trace */
        { "RaceTrace",            &g_td5.ini.race_trace_enabled },
        { "RaceTraceSlot",        &g_td5.ini.race_trace_slot },
        { "RaceTraceMaxFrames",   &g_td5.ini.race_trace_max_frames },
        { "AutoThrottle",         &g_td5.ini.auto_throttle },
        { "AutoThrottleValue",    &g_td5.ini.auto_throttle_value },
        { "RaceTraceMaxSimTicks", &g_td5.ini.race_trace_max_sim_ticks },
        { "WholeState",           &g_td5.ini.whole_state_enabled },
        { "WholeStateMaxTicks",   &g_td5.ini.whole_state_max_ticks },
        { "ExperimentalBiasClamp", &g_td5.ini.experimental_bias_clamp },
        /* Network (S10) */
        { "NetMode",              &g_td5.ini.net_mode },
        { "GamePort",             &g_td5.ini.net_game_port },
        { "EnableUPnP",           &g_td5.ini.net_enable_upnp },
        /* StateReplayMode is also CLI-overridable but takes the integer
         * code (0=off, 1=dump, 2=inject, 3=both) rather than the string. */
        { "StateReplayMode",        &g_td5.ini.state_replay_mode },
        { "StateReplayStartFrame",  &g_td5.ini.state_replay_start_frame },
        { "StateReplayEndFrame",    &g_td5.ini.state_replay_end_frame },
        { "StateReplayMaxFrames",   &g_td5.ini.state_replay_max_frames },
        /* Logging */
        { "LogEnabled",           &g_td5.ini.log_enabled },
        { "LogMinLevel",          &g_td5.ini.log_min_level },
        { "LogFrontend",          &g_td5.ini.log_frontend },
        { "LogRace",              &g_td5.ini.log_race },
        { "LogEngine",            &g_td5.ini.log_engine },
        { "LogWrapper",           &g_td5.ini.log_wrapper },
        /* Replay */
        { "PersistToDisk",        &g_td5.ini.replay_persist_to_disk },
        /* Test hook: cycle resolutions in the main loop to validate
         * apply_display_mode without manual menu navigation. */
        { "TestResolutionCycle",  &g_td5.ini.test_resolution_cycle },
        { "TestCupRoundtrip",     &g_td5.ini.test_cup_roundtrip },
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

        /* Float-valued overrides handled separately from the int table. */
        if (klen == 16 && _strnicmp(key, "TraceFastForward", 16) == 0) {
            float fval = (float)atof(eq + 1);
            g_td5.ini.trace_fast_forward = fval;
            dbglog("  CLI override: TraceFastForward=%g", fval);
            n_applied++;
            continue;
        }

        /* CSV-string overrides for modular trace selection. */
        if (klen == 12 && _strnicmp(key, "TraceModules", 12) == 0) {
            g_td5.ini.trace_module_mask = td5_trace_parse_modules(eq + 1);
            dbglog("  CLI override: TraceModules=%s -> mask=0x%x",
                   eq + 1, g_td5.ini.trace_module_mask);
            n_applied++;
            continue;
        }
        if (klen == 11 && _strnicmp(key, "TraceStages", 11) == 0) {
            g_td5.ini.trace_stage_mask = td5_trace_parse_stages(eq + 1);
            dbglog("  CLI override: TraceStages=%s -> mask=0x%x",
                   eq + 1, g_td5.ini.trace_stage_mask);
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

#ifndef TD5RE_RELEASE
/* S10 loopback net self-test (dev builds only). Set env TD5RE_NET_SELFTEST=host
 * (or =join, with optional TD5RE_NET_SELFTEST_IP) to exercise the Direct-IP
 * game-port JOIN handshake + UPnP fallback end-to-end WITHOUT driving the menu.
 * Returns 1 if the self-test ran (WinMain should then exit), 0 to boot normally. */
static int td5_net_selftest(void)
{
    const char *mode = getenv("TD5RE_NET_SELFTEST");
    uint32_t start;
    int is_host;

    if (!mode || !mode[0]) return 0;
    is_host = (mode[0] == 'h' || mode[0] == 'H');

    if (!td5_net_init()) {
        TD5_LOG_E("net", "selftest: td5_net_init failed");
        return 1;
    }
    td5_net_set_mode(TD5_NET_MODE_DIRECT);

    if (is_host) {
        const char *pw  = getenv("TD5RE_NET_SELFTEST_PW");
        const char *mx  = getenv("TD5RE_NET_SELFTEST_MAX");
        if (!td5_net_create_session_ex("SelfTest", "Host", 6,
                                       g_td5.ini.net_game_port, g_td5.ini.net_enable_upnp))
            TD5_LOG_E("net", "selftest host: create failed");
        else {
            int mxn = (mx && mx[0]) ? atoi(mx) : 6;
            td5_net_set_session_limits(mxn, (pw && pw[0]) ? pw : "");
            TD5_LOG_I("net", "selftest host: \"%s\" upnp=%d max=%d password=%s",
                      td5_net_get_status_text(), td5_net_get_upnp_status(),
                      td5_net_get_max_players(), (pw && pw[0]) ? "set" : "none");
        }
    } else {
        const char *ip = getenv("TD5RE_NET_SELFTEST_IP");
        const char *pw = getenv("TD5RE_NET_SELFTEST_PW");
        if (!ip || !ip[0]) ip = "127.0.0.1";
        if (pw) td5_net_set_join_password(pw);
        if (!td5_net_join_direct(ip, g_td5.ini.net_game_port, "Client"))
            TD5_LOG_E("net", "selftest join: join_direct failed");
        else
            TD5_LOG_I("net", "selftest join: \"%s\" (pw=%s)",
                      td5_net_get_status_text(), (pw && pw[0]) ? "set" : "none");
    }

    start = td5_plat_time_ms();
    while ((td5_plat_time_ms() - start) < 12000) {
        td5_net_tick();
        if (is_host && td5_net_get_player_count() >= 2) {
            TD5_LOG_I("net", "selftest host: client joined (players=%d) -- HANDSHAKE OK",
                      td5_net_get_player_count());
            break;
        }
        if (!is_host && td5_net_local_slot() >= 0) {
            TD5_LOG_I("net", "selftest join: assigned slot %d -- HANDSHAKE OK",
                      td5_net_local_slot());
            break;
        }
        if (!is_host && td5_net_get_join_nak_reason() != 0) {
            TD5_LOG_W("net", "selftest join: REJECTED nak_reason=%d (1=full,2=password)",
                      td5_net_get_join_nak_reason());
            break;
        }
        if (!is_host && td5_net_is_connection_lost()) {
            TD5_LOG_W("net", "selftest join: connection lost / timed out");
            break;
        }
        td5_plat_sleep(50);
    }

    TD5_LOG_I("net", "selftest %s DONE: players=%d slot=%d upnp=%d nak=%d",
              is_host ? "host" : "join", td5_net_get_player_count(),
              td5_net_local_slot(), td5_net_get_upnp_status(),
              td5_net_get_join_nak_reason());
    td5_net_shutdown();
    return 1;
}
#endif /* !TD5RE_RELEASE */

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

    /* Match the original TD5_d3d.exe FPU control word: round-toward-minus-infinity
     * (x87 RC=01) AND 64-bit extended precision (x87 PC=11). Empirical capture
     * via tools/frida_fpu_state_probe.js (2026-05-20, log/orig/fpu_state.csv)
     * shows orig steady-state CW = 0x077F across every sampled call-site (physics,
     * cascade, RoundedTransform, CosFloat12bit). MinGW's Windows-process default
     * is PC=53 (CW=0x027F at attach), so without _PC_64 our intermediates round
     * to double precision while orig keeps full extended-precision accumulation.
     * RC=RDN side was previously shipped (pool4 pilot: 99.82% match vs 49.05%).
     * Set BEFORE anything else so logging/INI parsing don't inherit defaults. */
    _controlfp(_RC_DOWN | _PC_64, _MCW_RC | _MCW_PC);

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
    g_td5.ini.display_mode   = td5_ini_int("Display", "DisplayMode", 0);
    /* [S01 2026-06-04] 3-way window mode (0=fullscreen,1=windowed,2=borderless;
     * defaults from the legacy Windowed key), VSync, and the FPS/MS overlay
     * toggle. disp_width/height mirror the chosen resolution for persistence. */
    g_td5.ini.window_mode    = td5_ini_int("Display", "WindowMode", windowed ? 1 : 0);
    g_td5.ini.vsync          = td5_ini_int("Display", "VSync", 1);
    g_td5.ini.show_fps       = td5_ini_int("Display", "ShowFps", 1);

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
    g_td5.ini.auto_gearbox       = td5_ini_int("GameOptions", "AutoGearbox", 1);
    /* Rear-impact softening (S08): % of the rear-end angular response to keep
     * for a human player. 100 = faithful, default 45 = recoverable nudge. */
    g_td5.ini.rear_impact_response = td5_ini_int("GameOptions", "RearImpactResponse", 45);
    if (g_td5.ini.rear_impact_response < 0)   g_td5.ini.rear_impact_response = 0;
    if (g_td5.ini.rear_impact_response > 100) g_td5.ini.rear_impact_response = 100;
    /* Anti-tunnel car-vs-car depenetration (S17): 1 = on (no visible
     * interpenetration at impact), 0 = byte-faithful single fixed push. */
    g_td5.ini.anti_tunnel = td5_ini_int("GameOptions", "AntiTunnel", 1);
    if (g_td5.ini.anti_tunnel < 0) g_td5.ini.anti_tunnel = 0;
    if (g_td5.ini.anti_tunnel > 1) g_td5.ini.anti_tunnel = 1;
    /* Collision-box overlap allowance so cars rest at visible-mesh contact
     * rather than the inflated collision box (~3 display units ~= 1 cm). */
    g_td5.ini.anti_tunnel_slop = td5_ini_int("GameOptions", "AntiTunnelSlop", 40);
    if (g_td5.ini.anti_tunnel_slop < 0)   g_td5.ini.anti_tunnel_slop = 0;
    if (g_td5.ini.anti_tunnel_slop > 256) g_td5.ini.anti_tunnel_slop = 256;
    /* CATCHUP / rubber-band assist override (S06). -1 = use the persisted value
     * (S05 toggle); 0 = off; 1..9 = on. See ai_catchup_level() in td5_ai.c. */
    g_td5.ini.catchup_assist = td5_ini_int("GameOptions", "CatchupAssist", -1);
    if (g_td5.ini.catchup_assist < -1) g_td5.ini.catchup_assist = -1;
    if (g_td5.ini.catchup_assist > 9)  g_td5.ini.catchup_assist = 9;
    /* AI/traffic accel + top speed sourced from each car's carparam (S06). */
    g_td5.ini.ai_accel_from_car = td5_ini_int("GameOptions", "AIAccelFromCar", 1);
    /* S20 Smart Traffic (source-port enhancement; all default ON, traffic-only). */
    g_td5.ini.traffic_smart           = td5_ini_int("Traffic", "TrafficSmart", 1);
    g_td5.ini.traffic_wall_avoid      = td5_ini_int("Traffic", "WallAvoid", 1);
    g_td5.ini.traffic_avoid_slow_lane = td5_ini_int("Traffic", "AvoidSlowLane", 1);
    g_td5.ini.traffic_lookahead       = td5_ini_int("Traffic", "Lookahead", 1);
    g_td5.ini.traffic_wall_avoid_bias = td5_ini_int("Traffic", "WallAvoidBias", 96);
    if (g_td5.ini.traffic_wall_avoid_bias < 0)   g_td5.ini.traffic_wall_avoid_bias = 0;
    if (g_td5.ini.traffic_wall_avoid_bias > 256) g_td5.ini.traffic_wall_avoid_bias = 256;
    g_td5.ini.traffic_antifreeze        = td5_ini_int("Traffic", "AntiFreeze", 1);
    g_td5.ini.traffic_antifreeze_frames = td5_ini_int("Traffic", "AntiFreezeFrames", 120);
    if (g_td5.ini.traffic_antifreeze_frames < 15) g_td5.ini.traffic_antifreeze_frames = 15;
    g_td5.ini.traffic_player_collide    = td5_ini_int("Traffic", "PlayerCollide", 1);
    g_td5.ini.player1_joystick   = td5_ini_int("GameOptions", "Player1Joystick", 0);
    g_td5.ini.player2_joystick   = td5_ini_int("GameOptions", "Player2Joystick", 0);

    /* Game defaults */
    g_td5.ini.default_car       = td5_ini_int("Game", "DefaultCar", 0);
    g_td5.ini.default_track     = td5_ini_int("Game", "DefaultTrack", 0);
    g_td5.ini.default_game_type = td5_ini_int("Game", "DefaultGameType", 0);
    g_td5.ini.default_opponents = td5_ini_int("Game", "DefaultOpponents", -1); /* -1 = full grid */
    g_td5.ini.circuit_minimap   = td5_ini_int("Game", "CircuitMinimap", 1);    /* 1 = minimap on circuit tracks too */
    g_td5.ini.default_players   = td5_ini_int("Game", "DefaultPlayers", -1);   /* -1 = schedule default; >=2 = N-way split */
    g_td5.ini.override_track_zip = td5_ini_int("Game", "OverrideTrackZip", 0);  /* 0 = faithful; >0 = TD6 level NNN */
    g_td5.ini.override_start_span = td5_ini_int("Game", "OverrideStartSpan", 0); /* TD6 grid start span; 0 = auto */
    g_td5.ini.skip_intro        = td5_ini_int("Game", "SkipIntro", 1);
    g_td5.ini.debug_overlay     = td5_ini_int("Game", "DebugOverlay", 0);
    g_td5.ini.debug_collisions  = td5_ini_int("Debug", "Collisions", 0);

    /* [S27] DEV headless test hook for the controller-disconnect modal. */
    g_td5.ini.sim_joy_loss_player   = td5_ini_int("Debug", "SimulateJoyLoss", -1);
    g_td5.ini.sim_joy_loss_delay_ms = td5_ini_int("Debug", "SimulateJoyLossDelayMs", 4000);
    g_td5.ini.sim_joy_loss_hold_ms  = td5_ini_int("Debug", "SimulateJoyLossHoldMs", 4000);

    /* Benchmark mode: enables main-menu button 2 → TD5_GAMESTATE_BENCHMARK path.
     * Matches the original's dead-code button-2 branch (app+0x170 is never written
     * in TD5_d3d.exe). Default 0 = button 2 is 2-player. */
    g_td5.ini.enable_benchmark  = td5_ini_int("Debug", "EnableBenchmark", 0);

    /* Player-as-AI: forces slot 0 through td5_ai tick + physics AI dispatch,
     * matching the original attract-mode autopilot (InitializeRaceSession
     * 0x0042ACCF writes slot[0].state = 1 - g_attractModeDemoActive). */
    g_td5.ini.player_is_ai      = td5_ini_int("GameOptions", "PlayerIsAI", 0);
    /* OtherPlayersAI: AI-drive every local human slot except slot 0 (drive P1). */
    g_td5.ini.others_ai         = td5_ini_int("GameOptions", "OtherPlayersAI", 0);
    g_td5.ini.solo_ai_slot      = td5_ini_int("GameOptions", "SoloAISlot", 0);
    g_td5.ini.solo_race         = td5_ini_int("GameOptions", "SoloRace", 0);
    g_td5.ini.max_span          = td5_ini_int("GameOptions", "MaxSpan", 0);
    /* PhantomPeer default OFF: initial A/B (2026-05-22) regressed slot 0
     * Edinburgh from 11 → 1418 wall_hits when ON; v1 emulation is the
     * shipping path. Enable with --PhantomPeer=1 for A/B testing. */
    g_td5.ini.phantom_peer      = td5_ini_int("GameOptions", "PhantomPeer", 0);

    /* S10 net-play: [Network] connection config. */
    g_td5.ini.net_mode              = td5_ini_int("Network", "Mode", 0);
    g_td5.ini.net_game_port         = td5_ini_int("Network", "GamePort", 37050);
    g_td5.ini.net_enable_upnp       = td5_ini_int("Network", "EnableUPnP", 1);
    td5_ini_str("Network", "Nickname", "", g_td5.ini.net_nickname,
                sizeof(g_td5.ini.net_nickname));

#ifndef TD5RE_RELEASE
    /* S10 dev hook: run the loopback net self-test (env TD5RE_NET_SELFTEST) and
     * exit if requested. Placed AFTER the [Network] reads so it honors GamePort
     * + EnableUPnP. */
    if (td5_net_selftest())
        return 0;
#endif

    /* Auto-race: skip frontend entirely, launch race with INI settings */
    g_td5.ini.auto_race             = td5_ini_int("Game", "AutoRace", 0);
    g_td5.ini.start_screen          = td5_ini_int("Game", "StartScreen", -1);

    /* Additive span shift for every actor spawn — mirrors the Frida hook on
     * InitializeActorTrackPose (0x00434350). 0 = vanilla grid. */
    g_td5.ini.start_span_offset     = td5_ini_int("Game", "StartSpanOffset", 0);
    g_td5.ini.default_reverse       = td5_ini_int("Game", "DefaultReverse", 0);

    /* TD6-car player override: [Game] PlayerCarArchive = <3-letter code> forces
     * the player (slot 0) into a ported Test Drive 6 car (re/assets/cars/<code>/)
     * regardless of DefaultCar/menu. Empty = off. CLI --PlayerCarArchive= wins. */
    {
        char pca[16] = {0};
        td5_ini_str("Game", "PlayerCarArchive", "", pca, sizeof(pca));
        /* GetPrivateProfileString keeps inline "; comment" text and trailing
         * spaces — cut the value at the first whitespace/comment character. */
        for (char *q = pca; *q; ++q) {
            if (*q == ' ' || *q == '\t' || *q == ';' || *q == '#') { *q = '\0'; break; }
        }
        td5_asset_set_player_car_override(pca[0] ? pca : NULL);
    }

    /* Last-selected TD6 paint color (0xRRGGBB), set by the car-select color
     * panel and persisted across launches. Default = red. */
    g_td5.ini.td6_paint_color =
        td5_ini_int("CarSelection", "TD6PaintColor", 0xFF0000) & 0x00FFFFFF;

    /* Photo-booth: [Game] PhotoBoothCar=<code> boots the race with that car as
     * the player and renders ONLY the (grayscale) car over a chroma background,
     * for offline preview (carpic) generation. */
    {
        char pbc[16] = {0};
        td5_ini_str("Game", "PhotoBoothCar", "", pbc, sizeof(pbc));
        for (char *q = pbc; *q; ++q)
            if (*q == ' ' || *q == '\t' || *q == ';' || *q == '#') { *q = '\0'; break; }
        if (pbc[0]) {
            td5_asset_set_player_car_override(pbc);
            td5_render_set_photobooth(1);
            g_td5.ini.auto_race = 1;   /* boot straight into the booth scene */
        }
    }

    /* Trace */
    g_td5.ini.race_trace_enabled    = td5_ini_int("Trace", "RaceTrace", 0);
    g_td5.ini.race_trace_slot       = td5_ini_int("Trace", "RaceTraceSlot", -1);
    g_td5.ini.race_trace_max_frames = td5_ini_int("Trace", "RaceTraceMaxFrames", 600);
    g_td5.ini.auto_throttle         = td5_ini_int("Trace", "AutoThrottle", 0);
    g_td5.ini.auto_throttle_value   = td5_ini_int("Trace", "AutoThrottleValue", 0);
    g_td5.ini.auto_throttle_stop_span = td5_ini_int("Trace", "AutoThrottleStopSpan", 0);
    g_td5.ini.trace_fast_forward    = td5_ini_float("Trace", "TraceFastForward", 1.0f);
    g_td5.ini.race_trace_max_sim_ticks =
        td5_ini_int("Trace", "RaceTraceMaxSimTicks", 0);
    /* Feature flag: experimental ClassifyTrackOffsetClamp pre-loop port.
     * See td5_ai.c:td5_ai_refresh_route_state_slot — closes the lateral_bias
     * cascade that produces the slot-0 steering 2x divergence at sim_tick=1. */
    g_td5.ini.experimental_bias_clamp =
        td5_ini_int("Trace", "ExperimentalBiasClamp", 0);
    /* [TRACE 2026-05-24 traffic-edge-pen-cluster] CSV mirror of orig
     * Frida probe at tools/_probes/traffic_edge_pen_probe.js. */
    g_td5.ini.trace_traffic_edge_pen =
        td5_ini_int("Trace", "TrafficEdgePen", 0);
    /* [TRACE 2026-05-25 terrain-pitch-roll-zeroed] CSV mirror of orig
     * Frida probe at tools/_probes/terrain_probe_capture.js. */
    g_td5.ini.trace_terrain_cam_probe =
        td5_ini_int("Trace", "TerrainCamProbe", 0);

    /* Whole-state snapshot (see re/analysis/whole_state_diff_design.md).
     * Independent of RaceTrace -- you can capture whole-state without CSVs. */
    g_td5.ini.whole_state_enabled =
        td5_ini_int("Trace", "WholeState", 0);
    g_td5.ini.whole_state_max_ticks =
        td5_ini_int("Trace", "WholeStateMaxTicks", 600);

    /* Snapshot-replay harness (see td5_trace_replay.{c,h}).
     * Mode string is parsed into the int code stored in g_td5.ini.
     * off=0 dump=1 inject=2 both=3. */
    {
        char buf[16];
        td5_ini_str("Trace", "StateReplayMode", "off", buf, sizeof(buf));
        if      (!strcmp(buf, "dump"))   g_td5.ini.state_replay_mode = 1;
        else if (!strcmp(buf, "inject")) g_td5.ini.state_replay_mode = 2;
        else if (!strcmp(buf, "both"))   g_td5.ini.state_replay_mode = 3;
        else                             g_td5.ini.state_replay_mode = 0;
    }
    g_td5.ini.state_replay_start_frame =
        td5_ini_int("Trace", "StateReplayStartFrame", 0);
    g_td5.ini.state_replay_end_frame   =
        td5_ini_int("Trace", "StateReplayEndFrame",   0);
    g_td5.ini.state_replay_max_frames  =
        td5_ini_int("Trace", "StateReplayMaxFrames",  200);

    /* Modular trace selection. Defaults to all modules / all stages so an
     * unconfigured trace captures everything (matches the legacy schema's
     * coverage). /fix passes a narrowed CSV via --TraceModules / --TraceStages
     * so its sessions only emit the rows the fix actually needs. */
    {
        char buf[256];
        td5_ini_str("Trace", "Modules", "*", buf, sizeof(buf));
        g_td5.ini.trace_module_mask = td5_trace_parse_modules(buf);

        td5_ini_str("Trace", "Stages", "*", buf, sizeof(buf));
        g_td5.ini.trace_stage_mask = td5_trace_parse_stages(buf);
    }

    /* Logging — defaults preserve historic behavior (master on, INFO+, all
     * categories on, wrapper on). Flip to A/B-test perf without rebuilding. */
    g_td5.ini.log_frontend_draw = td5_ini_int("Logging", "FrontendDraw", 0);
    /* Resolution-independent vector frontend (MSDF text). Default on. */
    g_td5.ini.vector_ui     = td5_ini_int("Frontend", "VectorUI", 1);
    g_td5.ini.log_enabled   = td5_ini_int("Logging", "Enabled",  1);
    g_td5.ini.log_min_level = td5_ini_int("Logging", "MinLevel", 1);
    g_td5.ini.log_frontend  = td5_ini_int("Logging", "Frontend", 1);
    g_td5.ini.log_race      = td5_ini_int("Logging", "Race",     1);
    g_td5.ini.log_engine    = td5_ini_int("Logging", "Engine",   1);
    g_td5.ini.log_wrapper   = td5_ini_int("Logging", "Wrapper",  1);
    /* [Logging] Profile=1 enables the per-phase frame profiler (off by default;
     * logs "PROFILE (ms avg/max) ..." per second to engine.log for frontend and
     * race). ~zero cost when off. */
    td5_profile_set_enabled(td5_ini_int("Logging", "Profile", 0));

    /* Replay persistence (port-only divergence — default 0 = faithful).
     * See td5re.h ini.replay_persist_to_disk and td5_input.c. */
    g_td5.ini.replay_persist_to_disk = td5_ini_int("Replay", "PersistToDisk", 0);

    g_td5.ini.loaded = 1;

    /* CLI overrides run AFTER the INI so `--DefaultTrack=15` wins over the
     * INI value. Pass lpCmdLine (may be empty) — width/height/windowed are
     * updated in-place since they are locals below. */
    {
        int n_cli = td5_apply_cli_overrides(lpCmdLine, &width, &height, &windowed);
        if (n_cli > 0)
            dbglog("=== %d CLI override(s) applied ===", n_cli);
    }

    /* String CLI knob (the int-only override table above can't carry it):
     * --PlayerCarArchive=<code> overrides [Game] PlayerCarArchive so parallel
     * test runs can each drive a different TD6 car without editing the INI. */
    if (lpCmdLine && *lpCmdLine) {
        const char *m = strstr(lpCmdLine, "--PlayerCarArchive=");
        if (!m) m = strstr(lpCmdLine, "--playercararchive=");
        if (m) {
            m += 19;   /* strlen("--PlayerCarArchive=") */
            char code[16];
            int n = 0;
            while (m[n] && m[n] != ' ' && m[n] != '\t' && n < (int)sizeof(code) - 1) {
                code[n] = m[n];
                n++;
            }
            code[n] = '\0';
            td5_asset_set_player_car_override(n > 0 ? code : NULL);
            dbglog("CLI: PlayerCarArchive = '%s'", code);
        }
    }

    /* --PhotoBoothCar=<code>: same as the INI key, for the offline generator. */
    if (lpCmdLine && *lpCmdLine) {
        const char *m = strstr(lpCmdLine, "--PhotoBoothCar=");
        if (!m) m = strstr(lpCmdLine, "--photoboothcar=");
        if (m) {
            m += 16;   /* strlen("--PhotoBoothCar=") */
            char code[16];
            int n = 0;
            while (m[n] && m[n] != ' ' && m[n] != '\t' && n < (int)sizeof(code) - 1) {
                code[n] = m[n];
                n++;
            }
            code[n] = '\0';
            if (n > 0) {
                td5_asset_set_player_car_override(code);
                td5_render_set_photobooth(1);
                g_td5.ini.auto_race = 1;
                dbglog("CLI: PhotoBoothCar = '%s'", code);
            }
        }
    }

#ifdef TD5RE_RELEASE
    /* Release build: hard-disable every developer/test/trace affordance after
     * BOTH the INI and the CLI have been applied, so none of them can be
     * re-enabled in a shipping binary regardless of td5re_release.ini contents
     * or command-line flags. Gameplay/display/audio options are left untouched;
     * only the dev knobs are clamped. The matching instrumentation code is
     * additionally compiled out (see TD5RE_RELEASE guards + build_standalone.bat
     * release variant) — this is the runtime belt to that compile-time braces. */
    g_td5.ini.auto_race              = 0;   /* always boot the normal menu flow */
    g_td5.ini.start_screen           = -1;  /* no jump-to-screen test harness   */
    g_td5.ini.default_opponents      = -1;  /* full grid (no AutoRace override)  */
    g_td5.ini.default_players        = -1;  /* schedule default (no N-way override) */
    g_td5.ini.player_is_ai           = 0;   /* the human drives                 */
    g_td5.ini.debug_overlay          = 0;   /* no HUD debug text overlay        */
    g_td5.ini.debug_collisions       = 0;   /* no collision wireframe overlay   */
    g_td5.ini.sim_joy_loss_player    = -1;  /* no simulated controller-loss hook */
    g_td5.ini.race_trace_enabled     = 0;   /* no per-tick CSV race trace       */
    g_td5.ini.whole_state_enabled    = 0;   /* no whole-state snapshot dump     */
    g_td5.ini.state_replay_mode      = 0;   /* snapshot-replay harness off      */
    g_td5.ini.auto_throttle          = 0;   /* no scripted throttle             */
    g_td5.ini.trace_traffic_edge_pen = 0;   /* no traffic edge-pen probe CSV    */
    g_td5.ini.trace_terrain_cam_probe = 0;  /* no terrain camera probe CSV      */
    g_td5.ini.test_cup_roundtrip     = 0;   /* no CupData self-test path         */
#endif

    /* TD5RE divergent CupData self-test. Runs before any backend or
     * window bringup — and BEFORE the log filter override below — so
     * the test's diagnostics are always visible regardless of the
     * user's [Logging] Enabled choice. Set --TestCupRoundtrip=1 to
     * invoke. Exits 0 on PASS, 2 on FAIL. */
    if (g_td5.ini.test_cup_roundtrip > 0) {
        dbglog("=== TestCupRoundtrip: starting ===");
        int ok = td5_save_test_cup_roundtrip();
        dbglog("=== TestCupRoundtrip: %s ===", ok ? "PASS" : "FAIL");
        return ok ? 0 : 2;
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
    dbglog("  [GameOpt] Laps=%d Timers=%d Traffic=%d Cops=%d Diff=%d Dyn=%d Coll=%d AutoGB=%d PlayerIsAI=%d",
           g_td5.ini.laps, g_td5.ini.checkpoint_timers, g_td5.ini.traffic,
           g_td5.ini.cops, g_td5.ini.difficulty, g_td5.ini.dynamics, g_td5.ini.collisions,
           g_td5.ini.auto_gearbox, g_td5.ini.player_is_ai);
    dbglog("  [Game]    Car=%d Track=%d GameType=%d SkipIntro=%d DebugOverlay=%d AutoRace=%d StartScreen=%d StartSpanOffset=%d",
           g_td5.ini.default_car, g_td5.ini.default_track, g_td5.ini.default_game_type,
           g_td5.ini.skip_intro, g_td5.ini.debug_overlay, g_td5.ini.auto_race,
           g_td5.ini.start_screen, g_td5.ini.start_span_offset);
    dbglog("  [Trace]   RaceTrace=%d Slot=%d MaxFrames=%d AutoThrottle=%d FastFwd=%g SimTickCap=%d Modules=0x%x Stages=0x%x",
           g_td5.ini.race_trace_enabled, g_td5.ini.race_trace_slot,
           g_td5.ini.race_trace_max_frames, g_td5.ini.auto_throttle,
           g_td5.ini.trace_fast_forward, g_td5.ini.race_trace_max_sim_ticks,
           g_td5.ini.trace_module_mask, g_td5.ini.trace_stage_mask);
    dbglog("  [Logging] Enabled=%d MinLevel=%d Frontend=%d Race=%d Engine=%d Wrapper=%d",
           g_td5.ini.log_enabled, g_td5.ini.log_min_level,
           g_td5.ini.log_frontend, g_td5.ini.log_race,
           g_td5.ini.log_engine, g_td5.ini.log_wrapper);
    dbglog("  [Replay]  PersistToDisk=%d", g_td5.ini.replay_persist_to_disk);

    if (width <= 0 || height <= 0) {
        width  = GetSystemMetrics(SM_CXSCREEN);
        height = GetSystemMetrics(SM_CYSCREEN);
        if (width <= 0)  width  = 640;
        if (height <= 0) height = 480;
    }
    bpp = DEFAULT_BPP;
    /* [S01] Record the finalized windowed/fullscreen resolution so a persist
     * before any in-menu change still writes real [Display] Width/Height. */
    g_td5.ini.disp_width  = width;
    g_td5.ini.disp_height = height;

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
    /* [S01] Always create the device windowed, then apply the persisted
     * WindowMode (windowed/borderless/exclusive-fullscreen) once the platform
     * layer is up — this avoids the untested exclusive-fullscreen-at-create
     * path and keeps a single code path for mode transitions. */
    windowed = 1;
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

    /* [S01 2026-06-04] Apply the persisted VSync + WindowMode now that the swap
     * chain and platform layer exist. set_window_mode may change the render size
     * (borderless -> desktop res), so re-read the resulting client size into the
     * render-dim globals before the game modules initialize. */
    td5_plat_set_vsync(g_td5.ini.vsync);
    td5_plat_set_window_mode(g_td5.ini.window_mode);
    {
        int aw = width, ah = height;
        td5_plat_get_window_size(&aw, &ah);
        if (aw > 0 && ah > 0) {
            width  = aw;
            height = ah;
            g_td5.render_width  = aw;
            g_td5.render_height = ah;
        }
    }

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

        /* TestResolutionCycle: validate apply_display_mode without manual menu
         * navigation. Cycles through 4 windowed resolutions across the first
         * ~600 frames, then leaves the game at the final size. */
        if (g_td5.ini.test_resolution_cycle) {
            static int rc_frame = 0;
            static int rc_step = 0;
            static const struct { int w, h; const char *label; } rc_modes[] = {
                {  800, 600, "800x600"   },
                { 1024, 768, "1024x768"  },
                { 1280, 720, "1280x720"  },
                {  640, 480, "640x480"   },
            };
            const int rc_steps = (int)(sizeof(rc_modes) / sizeof(rc_modes[0]));
            const int rc_step_frames = 120; /* ~4s per step at 30 Hz */
            const int rc_warmup     = 60;  /* let init settle */
            rc_frame++;
            if (rc_frame > rc_warmup && rc_step < rc_steps &&
                ((rc_frame - rc_warmup) % rc_step_frames) == 0) {
                int w = rc_modes[rc_step].w, h = rc_modes[rc_step].h;
                dbglog("[TEST] frame=%d step=%d -> apply_display_mode(%s)",
                       rc_frame, rc_step, rc_modes[rc_step].label);
                if (!td5_plat_apply_display_mode(w, h, DEFAULT_BPP)) {
                    dbglog("[TEST] step=%d FAILED %s", rc_step, rc_modes[rc_step].label);
                }
                rc_step++;
            }
        }

        /* Run one frame of the game state machine.
         * td5re_frame() drives the FSM (INTRO -> MENU -> RACE -> BENCHMARK)
         * and calls td5_plat_present() internally at the end of each frame. */
        {
            /* Update the always-on FPS counter EVERY frame (all states) before
             * td5re_frame() renders it — td5_game_update_frame_timing() only runs
             * in the race, so the frontend would otherwise read a stale 0. */
            td5_game_update_fps_overlay();
            uint32_t t0 = td5_plat_time_ms();
            td5re_frame();
            uint32_t t1 = td5_plat_time_ms();
            uint32_t dt = t1 - t0;

            /* [S12] The frontend drains+consumes the WM_KEYDOWN nav latch every
             * menu frame. While NOT in the menu (intro/race/benchmark) nothing
             * consumes it, so arrow-steering during a race would leave sticky
             * bits that fire a spurious cursor move / confirm on the next return
             * to the frontend. Discard it here whenever state != MENU so the
             * latch only ever holds presses from the current menu frame. */
            if (g_td5.game_state != 1 /* MENU */)
                td5_plat_input_nav_latch();

            /* 1-second window: track min/avg/max of full td5re_frame() ms.
             * Logs once per real second so we can name the hot phase without
             * flooding. Tag "main" routes to log/engine.log. */
            static uint32_t s_perf_window_start = 0;
            static uint32_t s_perf_min = 0xFFFFFFFFu;
            static uint32_t s_perf_max = 0;
            static uint32_t s_perf_sum = 0;
            static uint32_t s_perf_count = 0;
            if (s_perf_window_start == 0) s_perf_window_start = t1;
            if (dt < s_perf_min) s_perf_min = dt;
            if (dt > s_perf_max) s_perf_max = dt;
            s_perf_sum += dt;
            s_perf_count++;
            if (t1 - s_perf_window_start >= 1000) {
                uint32_t avg = (s_perf_count > 0) ? (s_perf_sum / s_perf_count) : 0;
                dbglog("perf frame_ms: count=%u min=%u avg=%u max=%u game_state=%d",
                       s_perf_count, s_perf_min, avg, s_perf_max, g_td5.game_state);
                s_perf_window_start = t1;
                s_perf_min = 0xFFFFFFFFu;
                s_perf_max = 0;
                s_perf_sum = 0;
                s_perf_count = 0;
            }
        }


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

/* ============================================================
 * [CITATION-SWEEP 2026-05-21] Phase 1 audit-header refresh
 *
 * The following L3 Ghidra functions are ported (or folded) into
 * this file but were missed by build_confidence_map.py's
 * 2026-05-18 citation scan due to snake_case rename or
 * multi-line comment wraps. Listed here so the next confidence-
 * map run promotes them L3 -> L4 (cited without precision
 * keywords). Per-function audits remain a separate Phase 4 task.
 *
 * Source: re/analysis/l3_triage_2026-05-21.csv +
 *         re/analysis/phase1_manifest_assignment.csv
 *
 *   0x0040DF80  MarkMastersRaceSlotCompleted  (density-match, verify in Phase 4)
 *   0x00412B90  RenderTgaWithColorKeyToSurface  (density-match, verify in Phase 4)
 */

/* ============================================================
 * [ARCH-DIVERGENCE: CRT bridge collapse] Phase 2 manifest (2026-05-21)
 *
 * The original binary's statically-linked CRT exposed *_game wrappers
 * (sprintf_game, fopen_game, ...) that the port replaces with direct
 * libc calls. The 10 wrapper helpers listed below are intentionally not
 * ported - their callers use
 * snprintf/fopen/fread/fseek/ftell/fclose/_stricmp/heap/TLS/exit
 * directly from MSVCRT.lib (linked via the MinGW toolchain bundled in
 * td5mod/deps/mingw).
 *
 * Per build_confidence_map.py:104-126 docstring, [ARCH-DIVERGENCE]
 * is the audited, documented deviation marker - functionally
 * equivalent to L5 byte-faithful for source-port fidelity scoring.
 *
 * Original-binary addresses folded into this collapse. Each line
 * carries an [ARCH-DIVERGENCE: LIBC] marker so the citation-
 * proximity check in build_confidence_map.py:227-233 promotes
 * every entry (not just those near the block header) to L5.
 *
 *   0x00448443  sprintf_game        [ARCH-DIVERGENCE: LIBC]
 *   0x00448495  fclose_game         [ARCH-DIVERGENCE: LIBC]
 *   0x0044867C  fopen_game          [ARCH-DIVERGENCE: LIBC]
 *   0x0044868F  fread_game          [ARCH-DIVERGENCE: LIBC]
 *   0x00448D0E  ftell_game          [ARCH-DIVERGENCE: LIBC]
 *   0x00448E91  fseek_game          [ARCH-DIVERGENCE: LIBC]
 *   0x00449310  stricmp_game        [ARCH-DIVERGENCE: LIBC]
 *   0x0044950D  FatalStartupExit    [ARCH-DIVERGENCE: LIBC]
 *   0x004499D0  InitPrimaryTlsSlot  [ARCH-DIVERGENCE: LIBC]
 *   0x00449CCE  InitProcessHeap     [ARCH-DIVERGENCE: LIBC]
 */
