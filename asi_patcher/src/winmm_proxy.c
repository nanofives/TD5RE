/*
 * winmm.dll proxy — Ultimate-ASI-Loader-style plugin host.
 *
 * Forwarded exports are provided by winmm.def (name = winmm_real.name).
 * This translation unit only contributes DllMain, which scans the proxy's
 * own directory for *.asi files on DLL_PROCESS_ATTACH and LoadLibraryA's
 * each one. Any .asi-authored DllMain gets its chance to patch the game.
 *
 * The real winmm.dll must live next to this DLL as winmm_real.dll (a plain
 * copy of C:\Windows\SysWOW64\winmm.dll). install.bat handles that copy.
 */

#include <windows.h>
#include <stdio.h>

static void load_asi_plugins(HINSTANCE hinstDll)
{
    char proxy_path[MAX_PATH];
    if (!GetModuleFileNameA(hinstDll, proxy_path, MAX_PATH))
        return;

    char *last_sep = strrchr(proxy_path, '\\');
    if (!last_sep)
        return;
    *last_sep = '\0';

    char search[MAX_PATH + 16];
    snprintf(search, sizeof(search), "%s\\*.asi", proxy_path);

    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(search, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;

    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;
        char full[MAX_PATH + MAX_PATH];
        snprintf(full, sizeof(full), "%s\\%s", proxy_path, fd.cFileName);
        LoadLibraryA(full);
    } while (FindNextFileA(h, &fd));

    FindClose(h);
}

BOOL WINAPI DllMain(HINSTANCE hinstDll, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinstDll);
        load_asi_plugins(hinstDll);
    }
    return TRUE;
}
