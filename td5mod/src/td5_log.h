/**
 * td5_log.h - Structured logging for TD5 Mod Framework
 *
 * INI-driven, per-category, leveled logging with file + OutputDebugString output.
 * Enable verbose logging per subsystem when debugging, disable when stable.
 *
 * Usage:
 *   TD5_LogInit(g_iniPath);                  // call once in DllMain
 *   LOG_I(LOG_CAT_CORE, "Mod loaded v%d", ver);
 *   LOG_D(LOG_CAT_RENDER, "RenderState 0x%X = %d", rs, val);
 *   TD5_LogShutdown();                      // call on DLL_PROCESS_DETACH
 *
 * INI section [Logging]:
 *   LogFile=td5_mod.log   ; empty = no file (OutputDebugString only)
 *   LogLevel=2            ; global: 0=off, 1=error, 2=warn, 3=info, 4=debug
 *   LogCore=0             ; per-category override (0=use global level)
 *   LogRender=4           ; e.g. set to 4 when debugging rendering
 */

#ifndef TD5_LOG_H
#define TD5_LOG_H

#include <windows.h>
#include <stdio.h>
#include <stdarg.h>

/* =========================================================================
 * Log Levels
 * ========================================================================= */
#define LOG_NONE    0
#define LOG_ERROR   1
#define LOG_WARN    2
#define LOG_INFO    3
#define LOG_DEBUG   4

/* =========================================================================
 * Log Categories -- one per subsystem
 * ========================================================================= */
#define LOG_CAT_CORE        0   /* init, shutdown, INI load, hot-reload     */
#define LOG_CAT_WIDESCREEN  1   /* widescreen patches, resolution, viewport */
#define LOG_CAT_FONT        2   /* GDI font fix, glyph atlas               */
#define LOG_CAT_PHYSICS     3   /* tuning, gravity, friction                */
#define LOG_CAT_AI          4   /* AI grip, rubber-band, steering           */
#define LOG_CAT_RENDER      5   /* D3D state, textures, primitives          */
#define LOG_CAT_INPUT       6   /* input hooks, control bits                */
#define LOG_CAT_AUDIO       7   /* CD audio, sound channels                 */
#define LOG_CAT_GAMEMODE    8   /* custom game modes                        */
#define LOG_CAT_SNOW        9   /* snow restore, weather                    */
#define LOG_CAT_COLLISION  10   /* collision impulse, damage                */
#define LOG_CAT_CAMERA     11   /* camera system, viewport                  */
#define LOG_CAT_COUNT      12

/* =========================================================================
 * INI key names for each category (index must match LOG_CAT_*)
 * ========================================================================= */
static const char* const g_logCatNames[LOG_CAT_COUNT] = {
    "Core", "Widescreen", "Font", "Physics", "AI", "Render",
    "Input", "Audio", "GameMode", "Snow", "Collision", "Camera"
};

static const char* const g_logCatINIKeys[LOG_CAT_COUNT] = {
    "LogCore", "LogWidescreen", "LogFont", "LogPhysics", "LogAI", "LogRender",
    "LogInput", "LogAudio", "LogGameMode", "LogSnow", "LogCollision", "LogCamera"
};

static const char* const g_logLevelTags[] = {
    "     ", "ERROR", "WARN ", "INFO ", "DEBUG"
};

/* =========================================================================
 * State
 * ========================================================================= */
static int   g_logGlobalLevel = LOG_WARN;       /* default: errors + warnings */
static int   g_logCatLevel[LOG_CAT_COUNT];       /* 0 = use global            */
static HANDLE g_logFile = INVALID_HANDLE_VALUE;
static CRITICAL_SECTION g_logLock;
static int   g_logInitialized = 0;

/* =========================================================================
 * Init / Shutdown
 * ========================================================================= */

/* Cached INI path for hot-reload (set once by TD5_LogInit) */
static char g_logIniPath[MAX_PATH] = {0};

static void TD5_LogInit(const char *iniPath)
{
    char path[MAX_PATH];
    char val[16];
    int i;
    const char *ini;
    char *lastSlash;

    /* Cache the EXE-relative INI path for hot-reload */
    if (iniPath && iniPath[0]) {
        strncpy(g_logIniPath, iniPath, MAX_PATH - 1);
        g_logIniPath[MAX_PATH - 1] = '\0';
    } else {
        /* Fallback: derive from EXE path */
        GetModuleFileNameA(NULL, g_logIniPath, MAX_PATH);
        lastSlash = strrchr(g_logIniPath, '\\');
        if (lastSlash) strcpy(lastSlash + 1, "scripts\\td5_mod.ini");
    }
    ini = g_logIniPath;

    InitializeCriticalSection(&g_logLock);

    /* Global level */
    g_logGlobalLevel = GetPrivateProfileIntA("Logging", "LogLevel", LOG_WARN, ini);
    if (g_logGlobalLevel < LOG_NONE)  g_logGlobalLevel = LOG_NONE;
    if (g_logGlobalLevel > LOG_DEBUG) g_logGlobalLevel = LOG_DEBUG;

    /* Per-category overrides */
    for (i = 0; i < LOG_CAT_COUNT; i++) {
        g_logCatLevel[i] = GetPrivateProfileIntA("Logging", g_logCatINIKeys[i], 0, ini);
    }

    /* Log file — resolve relative to scripts/ dir (same dir as INI) */
    GetPrivateProfileStringA("Logging", "LogFile", "", path, sizeof(path), ini);
    if (path[0] != '\0') {
        char full[MAX_PATH];
        /* Derive scripts/ directory from the INI path */
        strncpy(full, ini, MAX_PATH - 1);
        full[MAX_PATH - 1] = '\0';
        lastSlash = strrchr(full, '\\');
        if (lastSlash) {
            strcpy(lastSlash + 1, path);
        } else {
            strncpy(full, path, MAX_PATH - 1);
        }
        g_logFile = CreateFileA(full, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    }

    g_logInitialized = 1;

    /* Suppress unused warnings for the arrays in translation units that
       only include this header but don't call TD5_Log directly. */
    (void)g_logCatNames;
    (void)g_logCatINIKeys;
    (void)g_logLevelTags;
    (void)val;
}

static void TD5_LogShutdown(void)
{
    if (!g_logInitialized) return;
    if (g_logFile != INVALID_HANDLE_VALUE) {
        CloseHandle(g_logFile);
        g_logFile = INVALID_HANDLE_VALUE;
    }
    DeleteCriticalSection(&g_logLock);
    g_logInitialized = 0;
}

/** Re-read log levels from INI (call on hot-reload F11) */
static void TD5_LogReload(void)
{
    const char *ini = g_logIniPath;
    int i;

    g_logGlobalLevel = GetPrivateProfileIntA("Logging", "LogLevel", LOG_WARN, ini);
    if (g_logGlobalLevel < LOG_NONE)  g_logGlobalLevel = LOG_NONE;
    if (g_logGlobalLevel > LOG_DEBUG) g_logGlobalLevel = LOG_DEBUG;

    for (i = 0; i < LOG_CAT_COUNT; i++) {
        g_logCatLevel[i] = GetPrivateProfileIntA("Logging", g_logCatINIKeys[i], 0, ini);
    }
}

/* =========================================================================
 * Core logging function
 * ========================================================================= */

static void TD5_Log(int category, int level, const char* fmt, ...)
{
    char buf[512];
    char line[600];
    int effective_level;
    va_list ap;
    DWORD written;
    int len;

    if (!g_logInitialized) return;
    if (level <= LOG_NONE) return;

    /* Check category level (0 = fall through to global) */
    if (category >= 0 && category < LOG_CAT_COUNT && g_logCatLevel[category] > 0)
        effective_level = g_logCatLevel[category];
    else
        effective_level = g_logGlobalLevel;

    if (level > effective_level) return;

    /* Format message */
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    /* Build line: [LEVEL] [Category] message\n */
    len = snprintf(line, sizeof(line), "[TD5MOD] [%s] [%-11s] %s\n",
                   g_logLevelTags[level],
                   (category >= 0 && category < LOG_CAT_COUNT) ? g_logCatNames[category] : "???",
                   buf);

    EnterCriticalSection(&g_logLock);

    /* OutputDebugString (always, for DebugView) */
    OutputDebugStringA(line);

    /* File output */
    if (g_logFile != INVALID_HANDLE_VALUE && len > 0) {
        WriteFile(g_logFile, line, (DWORD)len, &written, NULL);
    }

    LeaveCriticalSection(&g_logLock);
}

/* =========================================================================
 * Convenience macros
 * ========================================================================= */

#define LOG_E(cat, ...) TD5_Log((cat), LOG_ERROR, __VA_ARGS__)
#define LOG_W(cat, ...) TD5_Log((cat), LOG_WARN,  __VA_ARGS__)
#define LOG_I(cat, ...) TD5_Log((cat), LOG_INFO,  __VA_ARGS__)
#define LOG_D(cat, ...) TD5_Log((cat), LOG_DEBUG, __VA_ARGS__)

/* Log a patch operation: address, size, description */
#define LOG_PATCH(cat, addr, size, desc) \
    LOG_I((cat), "PATCH 0x%08X (%d bytes): %s", (unsigned)(addr), (int)(size), (desc))

/* Log a hook operation: target address, description */
#define LOG_HOOK(cat, addr, desc) \
    LOG_I((cat), "HOOK  0x%08X: %s", (unsigned)(addr), (desc))

/* Log a value change: what, old, new */
#define LOG_CHANGE_INT(cat, name, old_val, new_val) \
    LOG_D((cat), "%s: %d -> %d", (name), (int)(old_val), (int)(new_val))

#define LOG_CHANGE_FLOAT(cat, name, old_val, new_val) \
    LOG_D((cat), "%s: %.4f -> %.4f", (name), (double)(old_val), (double)(new_val))

#define LOG_CHANGE_HEX(cat, name, old_val, new_val) \
    LOG_D((cat), "%s: 0x%08X -> 0x%08X", (name), (unsigned)(old_val), (unsigned)(new_val))

#endif /* TD5_LOG_H */
