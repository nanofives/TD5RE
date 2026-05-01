/*
 * td5_ddraw — drop-in ddraw.dll that wraps the system DDraw so TD5_d3d.exe
 * runs in a real 640×480 window without forcing Win11 to mode-switch the
 * desktop. Scope is deliberately narrow:
 *
 *   - DirectDrawCreate is our own. All other exports forward via ddraw.def
 *     to ddraw_real.dll (a copy of C:\Windows\SysWOW64\ddraw.dll).
 *   - The returned IDirectDraw / 2 / 4 / 7 is wrapped so SetCooperativeLevel
 *     is forced to DDSCL_NORMAL and SetDisplayMode is a no-op (success).
 *   - CreateSurface intercepts the primary: the game gets a fake primary we
 *     own. Everything else (back buffers, off-screen surfaces) delegates to
 *     the real interface.
 *   - The fake primary owns a 16bpp shadow buffer for the game to Lock/Blt
 *     into. On unlock/blt-to-primary/flip, we convert 16 → 32bpp and
 *     StretchBlt into the game's HWND client area.
 *
 * This source file is the scaffolding + forwarding. The wrap layers live
 * in idd_wrap.c / surface_wrap.c / convert.c.
 */

#include <windows.h>
#include <ddraw.h>
#include <stdio.h>
#include <stdint.h>
#include "td5_ddraw.h"

HMODULE     g_real_ddraw = NULL;
PFN_DDCreate g_real_DDCreate = NULL;
HWND         g_td5dd_hwnd = NULL;

static CRITICAL_SECTION g_log_cs;
static BOOL             g_log_cs_init = FALSE;

void td5dd_log(const char *fmt, ...)
{
    if (!g_log_cs_init) { InitializeCriticalSection(&g_log_cs); g_log_cs_init = TRUE; }

    char msg[768];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(msg, sizeof(msg) - 1, fmt, ap);
    va_end(ap);
    msg[sizeof(msg) - 1] = '\0';

    EnterCriticalSection(&g_log_cs);
    char path[MAX_PATH];
    if (GetModuleFileNameA(NULL, path, MAX_PATH)) {
        char *sep = strrchr(path, '\\');
        if (sep) { *sep = '\0'; strcat(path, "\\td5_ddraw.log"); }
        else strcpy(path, "td5_ddraw.log");
    } else {
        strcpy(path, "td5_ddraw.log");
    }
    FILE *f = fopen(path, "a");
    if (f) { fputs(msg, f); fputc('\n', f); fclose(f); }
    LeaveCriticalSection(&g_log_cs);

    OutputDebugStringA(msg);
}

static BOOL load_real_ddraw(void)
{
    if (g_real_ddraw) return TRUE;
    g_real_ddraw = LoadLibraryA("ddraw_real.dll");
    if (!g_real_ddraw)
        g_real_ddraw = LoadLibraryA("C:\\Windows\\SysWOW64\\ddraw.dll");
    if (!g_real_ddraw) {
        td5dd_log("[proxy] failed to load real ddraw.dll (err=%lu)",
                  GetLastError());
        return FALSE;
    }
    g_real_DDCreate = (PFN_DDCreate)GetProcAddress(
        g_real_ddraw, "DirectDrawCreate");
    if (!g_real_DDCreate) {
        td5dd_log("[proxy] DirectDrawCreate not found in real ddraw");
        return FALSE;
    }
    td5dd_log("[proxy] real ddraw loaded @ %p", (void *)g_real_ddraw);
    return TRUE;
}

__declspec(dllexport) HRESULT WINAPI DirectDrawCreate(
    GUID *guid, LPDIRECTDRAW *lplpDD, IUnknown *outer)
{
    if (!load_real_ddraw()) return E_FAIL;

    LPDIRECTDRAW real = NULL;
    HRESULT hr = g_real_DDCreate(guid, &real, outer);
    td5dd_log("[proxy] DirectDrawCreate guid=%p -> hr=0x%08lX real=%p",
              (void *)guid, (unsigned long)hr, (void *)real);
    if (FAILED(hr) || !real) {
        if (lplpDD) *lplpDD = NULL;
        return hr;
    }

    LPDIRECTDRAW wrapped = td5dd_wrap_dd1(real);
    if (!wrapped) {
        real->lpVtbl->Release(real);
        if (lplpDD) *lplpDD = NULL;
        return E_OUTOFMEMORY;
    }
    if (lplpDD) *lplpDD = wrapped;
    return DD_OK;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID r)
{
    (void)r;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        td5dd_log("[proxy] td5_ddraw.dll attached");
    }
    return TRUE;
}
