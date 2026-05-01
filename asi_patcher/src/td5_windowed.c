/*
 * td5_windowed.asi — runtime byte-patcher for Test Drive 5.
 *
 * Applies in memory only, at process attach, the same byte patches that
 * re/patches/patch_windowed_td5exe.py and patch_windowed_m2dx.py write to
 * disk. The on-disk binaries stay pristine.
 *
 * TD5_d3d.exe (module base = GetModuleHandle(NULL)):
 *   +0x2B48A : 74 1B       -> EB 1B       (JZ -> JMP, skip InitializeRaceSession FullScreen)
 *   +0x422B1 : 74 15       -> EB 15       (JZ -> JMP, skip RunMainGameLoop FullScreen)
 *   +0x0BA60 : A1          -> C3          (neuter ResetTexturePageCacheState)
 *
 * M2DX.dll (loaded as a static import of TD5_d3d.exe):
 *   +0x06637 : 75 45       -> EB 45       (JNZ -> JMP, skip adapter fullscreen-force)
 *   +0x078CA : 74 44       -> EB 44       (JZ  -> JMP, force DDSCL_NORMAL branch)
 *   +0x06684 : 74 68       -> EB 68       (JZ  -> JMP, ditto secondary site)
 *   +0x124C5 : 01 00 00 00 -> 00 00 00 00 (Environment "fullscreen preferred" = 0)
 *   +0x12ADB : PUSH 0x80000000 -> PUSH 0x00CF0000 (window style WS_POPUP -> WS_OVERLAPPEDWINDOW)
 *
 * These cooperate with td5_ddraw's IDirectDraw wrapper: the patches quiet
 * the game's redundant fullscreen requests before they reach DDraw, and
 * the window-style patch gives us a movable caption window. td5_ddraw does
 * the heavy lifting (SCL override, SDM no-op, pixel-format fix, client
 * resize). Together they deliver stable windowed 640×480 on Win11.
 *
 * M2DX is caught via LdrRegisterDllNotification. If it happens to already
 * be loaded when our DllMain runs (it always is, as a static import), we
 * patch it immediately in DllMain too.
 */

#include <windows.h>
#include <stdio.h>
#include <stdint.h>

/* ---------- LdrRegisterDllNotification prototypes (undocumented) ------- */

typedef struct _UNICODE_STRING_LOCAL {
    USHORT Length;
    USHORT MaximumLength;
    PWSTR  Buffer;
} UNICODE_STRING_LOCAL, *PUNICODE_STRING_LOCAL;
typedef const UNICODE_STRING_LOCAL *PCUNICODE_STRING_LOCAL;

typedef struct _LDR_DLL_LOADED_NOTIFICATION_DATA {
    ULONG  Flags;
    PCUNICODE_STRING_LOCAL FullDllName;
    PCUNICODE_STRING_LOCAL BaseDllName;
    PVOID  DllBase;
    ULONG  SizeOfImage;
} LDR_DLL_LOADED_NOTIFICATION_DATA, *PLDR_DLL_LOADED_NOTIFICATION_DATA;

typedef union _LDR_DLL_NOTIFICATION_DATA {
    LDR_DLL_LOADED_NOTIFICATION_DATA Loaded;
    LDR_DLL_LOADED_NOTIFICATION_DATA Unloaded;
} LDR_DLL_NOTIFICATION_DATA, *PLDR_DLL_NOTIFICATION_DATA;

#define LDR_DLL_NOTIFICATION_REASON_LOADED   1

typedef VOID (CALLBACK *PLDR_DLL_NOTIFICATION_FUNCTION)(
    ULONG NotificationReason,
    PLDR_DLL_NOTIFICATION_DATA NotificationData,
    PVOID Context);

typedef NTSTATUS (NTAPI *PFN_LdrRegisterDllNotification)(
    ULONG Flags,
    PLDR_DLL_NOTIFICATION_FUNCTION NotificationFunction,
    PVOID Context,
    PVOID *Cookie);

/* -------------------------- patch tables ------------------------------ */

typedef struct {
    DWORD   offset;
    size_t  length;
    BYTE    expected[8];
    BYTE    replacement[8];
    const char *desc;
} Patch;

static const Patch EXE_PATCHES[] = {
    { 0x2B48A, 2, { 0x74, 0x1B }, { 0xEB, 0x1B },
      "TD5_d3d InitializeRaceSession JZ->JMP (skip FullScreen)" },
    { 0x422B1, 2, { 0x74, 0x15 }, { 0xEB, 0x15 },
      "TD5_d3d RunMainGameLoop JZ->JMP (skip FullScreen)" },
    { 0x0BA60, 1, { 0xA1 },       { 0xC3 },
      "TD5_d3d ResetTexturePageCacheState -> RET" },
};

static const Patch M2DX_PATCHES[] = {
    { 0x06637, 2, { 0x75, 0x45 }, { 0xEB, 0x45 },
      "M2DX DXDraw::Create adapter JNZ->JMP" },
    { 0x078CA, 2, { 0x74, 0x44 }, { 0xEB, 0x44 },
      "M2DX ConfigureDirectDrawCooperativeLevel JZ->JMP" },
    { 0x06684, 2, { 0x74, 0x68 }, { 0xEB, 0x68 },
      "M2DX DXDraw::Create force DDSCL_NORMAL JZ->JMP" },
    { 0x124C5, 4, { 0x01, 0x00, 0x00, 0x00 }, { 0x00, 0x00, 0x00, 0x00 },
      "M2DX Environment fullscreen-preferred flag 1->0" },
    { 0x12ADB, 5, { 0x68, 0x00, 0x00, 0x00, 0x80 }, { 0x68, 0x00, 0x00, 0xCF, 0x00 },
      "M2DX DXWin::Initialize CreateWindowExA WS_POPUP->WS_OVERLAPPEDWINDOW" },
};

static BOOL g_m2dx_patched = FALSE;

/* --------------------------- logging ---------------------------------- */

static void plog(const char *fmt, ...)
{
    static CRITICAL_SECTION cs;
    static BOOL init = FALSE;
    if (!init) { InitializeCriticalSection(&cs); init = TRUE; }

    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    va_end(ap);
    buf[sizeof(buf) - 1] = '\0';

    EnterCriticalSection(&cs);
    char path[MAX_PATH];
    if (GetModuleFileNameA(NULL, path, MAX_PATH)) {
        char *sep = strrchr(path, '\\');
        if (sep) { *sep = '\0'; strcat(path, "\\td5_windowed.log"); }
        else strcpy(path, "td5_windowed.log");
    } else {
        strcpy(path, "td5_windowed.log");
    }
    FILE *f = fopen(path, "a");
    if (f) { fputs(buf, f); fputc('\n', f); fclose(f); }
    LeaveCriticalSection(&cs);
    OutputDebugStringA(buf);
}

/* ------------------------ patch application --------------------------- */

static BOOL apply_patches(BYTE *base, const Patch *list, size_t count,
                          const char *module_name)
{
    BOOL all_ok = TRUE;
    for (size_t i = 0; i < count; ++i) {
        const Patch *p = &list[i];
        BYTE *addr = base + p->offset;

        if (memcmp(addr, p->replacement, p->length) == 0) {
            plog("[%s] +0x%05X already patched (%s)", module_name, p->offset, p->desc);
            continue;
        }
        if (memcmp(addr, p->expected, p->length) != 0) {
            plog("[%s] +0x%05X UNEXPECTED bytes, skipping (%s)", module_name, p->offset, p->desc);
            all_ok = FALSE;
            continue;
        }

        DWORD old_prot = 0;
        if (!VirtualProtect(addr, p->length, PAGE_EXECUTE_READWRITE, &old_prot)) {
            plog("[%s] +0x%05X VirtualProtect failed (%lu)",
                 module_name, p->offset, GetLastError());
            all_ok = FALSE;
            continue;
        }
        memcpy(addr, p->replacement, p->length);
        VirtualProtect(addr, p->length, old_prot, &old_prot);
        FlushInstructionCache(GetCurrentProcess(), addr, p->length);
        plog("[%s] +0x%05X patched: %s", module_name, p->offset, p->desc);
    }
    return all_ok;
}

static void try_patch_m2dx(void)
{
    if (g_m2dx_patched) return;
    HMODULE h = GetModuleHandleA("M2DX.dll");
    if (!h) return;
    apply_patches((BYTE *)h, M2DX_PATCHES,
                  sizeof(M2DX_PATCHES) / sizeof(M2DX_PATCHES[0]),
                  "M2DX.dll");
    g_m2dx_patched = TRUE;
}

/* ---------------------- Ldr notification callback --------------------- */

static VOID CALLBACK dll_notify(ULONG reason,
                                PLDR_DLL_NOTIFICATION_DATA data,
                                PVOID ctx)
{
    (void)ctx;
    if (reason != LDR_DLL_NOTIFICATION_REASON_LOADED || !data) return;
    PCUNICODE_STRING_LOCAL base = data->Loaded.BaseDllName;
    if (!base || !base->Buffer || base->Length == 0) return;

    static const WCHAR target[] = L"M2DX.dll";
    size_t tlen = sizeof(target) / sizeof(WCHAR) - 1;
    if (base->Length / sizeof(WCHAR) != tlen) return;
    for (size_t i = 0; i < tlen; ++i) {
        WCHAR a = base->Buffer[i];
        WCHAR b = target[i];
        if (a >= L'A' && a <= L'Z') a = (WCHAR)(a - L'A' + L'a');
        if (b >= L'A' && b <= L'Z') b = (WCHAR)(b - L'A' + L'a');
        if (a != b) return;
    }

    apply_patches((BYTE *)data->Loaded.DllBase, M2DX_PATCHES,
                  sizeof(M2DX_PATCHES) / sizeof(M2DX_PATCHES[0]),
                  "M2DX.dll");
    g_m2dx_patched = TRUE;
}

static void install_m2dx_watch(void)
{
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    if (!ntdll) return;
    PFN_LdrRegisterDllNotification reg =
        (PFN_LdrRegisterDllNotification)GetProcAddress(
            ntdll, "LdrRegisterDllNotification");
    if (!reg) return;
    PVOID cookie = NULL;
    reg(0, dll_notify, NULL, &cookie);
}

/* ------------------------------ entry --------------------------------- */

BOOL WINAPI DllMain(HINSTANCE hinstDll, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(hinstDll);

    HMODULE exe = GetModuleHandleA(NULL);
    if (exe) {
        plog("td5_windowed.asi attached (EXE base=%p)", (void *)exe);
        apply_patches((BYTE *)exe, EXE_PATCHES,
                      sizeof(EXE_PATCHES) / sizeof(EXE_PATCHES[0]),
                      "TD5_d3d.exe");
    }
    try_patch_m2dx();
    install_m2dx_watch();
    return TRUE;
}
