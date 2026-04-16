/**
 * ddraw_main.c - DLL entry point and exported DirectDraw creation functions
 *
 * This file provides the four exported functions that the game (via M2DX.dll)
 * calls to create DirectDraw objects. We intercept these and return our
 * D3D11-backed wrapper objects instead.
 */

#include "wrapper.h"
#include <stdarg.h>

/* ========================================================================
 * File-based debug logger
 * ======================================================================== */

#ifdef WRAPPER_DEBUG
static FILE *g_logfile = NULL;

void wrapper_log(const char *fmt, ...) {
    if (!g_logfile) {
        /* Try log/ subdirectory first (created by td5_plat_log_init),
         * fall back to current directory */
        g_logfile = fopen("log\\wrapper.log", "w");
        if (!g_logfile)
            g_logfile = fopen("d3d11_wrapper.log", "w");
        if (!g_logfile) return;
        setbuf(g_logfile, NULL); /* unbuffered for crash safety */
    }
    va_list args;
    va_start(args, fmt);
    vfprintf(g_logfile, fmt, args);
    va_end(args);
    fprintf(g_logfile, "\n");
}
#endif

/* ========================================================================
 * Global backend state (single instance)
 * ======================================================================== */

D3D11Backend g_backend = {0};

/* ========================================================================
 * GUID definitions (declared extern in wrapper.h)
 * ======================================================================== */

/* IID_IDirectDraw4: {9C59509A-39BD-11D1-8C4A-00C04FD930C5} */
const GUID IID_IDirectDraw4 = {
    0x9C59509A, 0x39BD, 0x11D1,
    {0x8C, 0x4A, 0x00, 0xC0, 0x4F, 0xD9, 0x30, 0xC5}
};

/* IID_IDirectDrawSurface4: {0B2B8630-AD35-11D0-8EA6-00609797EA5B} */
const GUID IID_IDirectDrawSurface4 = {
    0x0B2B8630, 0xAD35, 0x11D0,
    {0x8E, 0xA6, 0x00, 0x60, 0x97, 0x97, 0xEA, 0x5B}
};

/* IID_IDirect3D3: {BB223240-E72B-11D0-A9B4-00AA00C0993E} */
const GUID IID_IDirect3D3 = {
    0xBB223240, 0xE72B, 0x11D0,
    {0xA9, 0xB4, 0x00, 0xAA, 0x00, 0xC0, 0x99, 0x3E}
};

/* IID_IDirect3DDevice3: {B0AB3B60-33D7-11D1-A981-00C04FD7B174} */
const GUID IID_IDirect3DDevice3 = {
    0xB0AB3B60, 0x33D7, 0x11D1,
    {0xA9, 0x81, 0x00, 0xC0, 0x4F, 0xD7, 0xB1, 0x74}
};

/* IID_IDirect3DViewport3: {B0AB3B61-33D7-11D1-A981-00C04FD7B174} */
const GUID IID_IDirect3DViewport3 = {
    0xB0AB3B61, 0x33D7, 0x11D1,
    {0xA9, 0x81, 0x00, 0xC0, 0x4F, 0xD7, 0xB1, 0x74}
};

/* IID_IDirect3DTexture2: {93281502-8CF8-11D0-89AB-00A0C9054129} */
const GUID IID_IDirect3DTexture2 = {
    0x93281502, 0x8CF8, 0x11D0,
    {0x89, 0xAB, 0x00, 0xA0, 0xC9, 0x05, 0x41, 0x29}
};

/* IID_IDirect3DHALDevice: {84E63dE0-46AA-11CF-816F-0000C020156E} */
const GUID IID_IDirect3DHALDevice = {
    0x84E63dE0, 0x46AA, 0x11CF,
    {0x81, 0x6F, 0x00, 0x00, 0xC0, 0x20, 0x15, 0x6E}
};

/* IID_IDirect3DRGBDevice: {A4665C60-2673-11CF-A31A-00AA00B93356} */
const GUID IID_IDirect3DRGBDevice = {
    0xA4665C60, 0x2673, 0x11CF,
    {0xA3, 0x1A, 0x00, 0xAA, 0x00, 0xB9, 0x33, 0x56}
};

/* IID_IDirectDraw (v1): {6C14DB80-A733-11CE-A521-0020AF0BE560} */
const GUID IID_IDirectDraw = {
    0x6C14DB80, 0xA733, 0x11CE,
    {0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60}
};

/* IID_IDirectDraw2: {B3A6F3E0-2B43-11CF-A2DE-00AA00B93356} */
const GUID IID_IDirectDraw2 = {
    0xB3A6F3E0, 0x2B43, 0x11CF,
    {0xA2, 0xDE, 0x00, 0xAA, 0x00, 0xB9, 0x33, 0x56}
};

/* ========================================================================
 * HUD rendering flag — called by ASI mod to signal HUD draw phase
 * ======================================================================== */

/* ========================================================================
 * Backend initialization / shutdown
 * ======================================================================== */

int Backend_Init(void)
{
    WRAPPER_LOG("Backend_Init: Initializing D3D11 backend");

    /* Patch HandleRenderWindowResize EARLY — before M2DX creates any D3D
     * devices. If we wait until Backend_CreateDevice, M2DX's WM_SIZE
     * handling may destroy g_pDirect3DDevice during ShowWindow(SW_HIDE). */
    {
        HMODULE m2dx = GetModuleHandleA("M2DX.dll");
        if (m2dx) {
            BYTE *func = (BYTE*)m2dx + 0x4bc0;
            DWORD old_protect;
            if (VirtualProtect(func, 16, PAGE_EXECUTE_READWRITE, &old_protect)) {
                /* mov eax,[esp+4]; mov dword [eax],0; mov eax,1; ret */
                func[0]  = 0x8B; func[1]  = 0x44; func[2]  = 0x24; func[3]  = 0x04;
                func[4]  = 0xC7; func[5]  = 0x00; func[6]  = 0x00; func[7]  = 0x00;
                func[8]  = 0x00; func[9]  = 0x00;
                func[10] = 0xB8; func[11] = 0x01; func[12] = 0x00; func[13] = 0x00;
                func[14] = 0x00; func[15] = 0xC3;
                VirtualProtect(func, 16, old_protect, &old_protect);
                WRAPPER_LOG("Backend_Init: patched HandleRenderWindowResize EARLY");
            }
        }
    }

    /* Enumerate available display modes (uses temporary DXGI factory) */
    Backend_EnumerateModes();

    /* Initialize PNG texture override system */
    PngOverride_Init();

    WRAPPER_LOG("Backend_Init: OK, %d display modes enumerated", g_backend.mode_count);
    return 1;
}

void Backend_Shutdown(void)
{
    int i;

    WRAPPER_LOG("Backend_Shutdown");
    PngOverride_Shutdown();

    /* Release compositing resources */
    if (g_backend.composite_srv) {
        ID3D11ShaderResourceView_Release(g_backend.composite_srv);
        g_backend.composite_srv = NULL;
    }
    if (g_backend.composite_tex) {
        ID3D11Texture2D_Release(g_backend.composite_tex);
        g_backend.composite_tex = NULL;
    }
    if (g_backend.composite_staging) {
        ID3D11Texture2D_Release(g_backend.composite_staging);
        g_backend.composite_staging = NULL;
    }
    g_backend.bltfast_surface = NULL;

    /* Release shaders */
    if (g_backend.vs_pretransformed) {
        ID3D11VertexShader_Release(g_backend.vs_pretransformed);
        g_backend.vs_pretransformed = NULL;
    }
    if (g_backend.vs_fullscreen) {
        ID3D11VertexShader_Release(g_backend.vs_fullscreen);
        g_backend.vs_fullscreen = NULL;
    }
    for (i = 0; i < PS_COUNT; i++) {
        if (g_backend.ps_shaders[i]) {
            ID3D11PixelShader_Release(g_backend.ps_shaders[i]);
            g_backend.ps_shaders[i] = NULL;
        }
    }
    if (g_backend.ps_composite) {
        ID3D11PixelShader_Release(g_backend.ps_composite);
        g_backend.ps_composite = NULL;
    }
    if (g_backend.input_layout) {
        ID3D11InputLayout_Release(g_backend.input_layout);
        g_backend.input_layout = NULL;
    }

    /* Release state objects */
    for (i = 0; i < BLEND_STATE_COUNT; i++) {
        if (g_backend.blend_states[i]) {
            ID3D11BlendState_Release(g_backend.blend_states[i]);
            g_backend.blend_states[i] = NULL;
        }
    }
    for (i = 0; i < DS_STATE_COUNT; i++) {
        if (g_backend.ds_states[i]) {
            ID3D11DepthStencilState_Release(g_backend.ds_states[i]);
            g_backend.ds_states[i] = NULL;
        }
    }
    if (g_backend.rs_state) {
        ID3D11RasterizerState_Release(g_backend.rs_state);
        g_backend.rs_state = NULL;
    }
    for (i = 0; i < SAMP_STATE_COUNT; i++) {
        if (g_backend.sampler_states[i]) {
            ID3D11SamplerState_Release(g_backend.sampler_states[i]);
            g_backend.sampler_states[i] = NULL;
        }
    }

    /* Release dynamic buffers */
    if (g_backend.dynamic_vb) {
        ID3D11Buffer_Release(g_backend.dynamic_vb);
        g_backend.dynamic_vb = NULL;
    }
    if (g_backend.dynamic_ib) {
        ID3D11Buffer_Release(g_backend.dynamic_ib);
        g_backend.dynamic_ib = NULL;
    }

    /* Release constant buffers */
    if (g_backend.cb_viewport) {
        ID3D11Buffer_Release(g_backend.cb_viewport);
        g_backend.cb_viewport = NULL;
    }
    if (g_backend.cb_fog) {
        ID3D11Buffer_Release(g_backend.cb_fog);
        g_backend.cb_fog = NULL;
    }

    /* Release render targets */
    if (g_backend.swap_rtv) {
        ID3D11RenderTargetView_Release(g_backend.swap_rtv);
        g_backend.swap_rtv = NULL;
    }
    if (g_backend.depth_dsv) {
        ID3D11DepthStencilView_Release(g_backend.depth_dsv);
        g_backend.depth_dsv = NULL;
    }
    if (g_backend.depth_tex) {
        ID3D11Texture2D_Release(g_backend.depth_tex);
        g_backend.depth_tex = NULL;
    }

    /* Release D3D11 core objects */
    if (g_backend.context) {
        ID3D11DeviceContext_Release(g_backend.context);
        g_backend.context = NULL;
    }
    if (g_backend.device) {
        ID3D11Device_Release(g_backend.device);
        g_backend.device = NULL;
    }
    if (g_backend.swap_chain) {
        IDXGISwapChain_Release(g_backend.swap_chain);
        g_backend.swap_chain = NULL;
    }
    if (g_backend.dxgi_factory) {
        IDXGIFactory1_Release(g_backend.dxgi_factory);
        g_backend.dxgi_factory = NULL;
    }

    /* Release display mode list */
    if (g_backend.modes) {
        HeapFree(GetProcessHeap(), 0, g_backend.modes);
        g_backend.modes = NULL;
    }
    g_backend.mode_count = 0;
    g_backend.mode_capacity = 0;
}

void Backend_EnumerateModes(void)
{
    IDXGIFactory1 *factory = NULL;
    IDXGIAdapter *adapter = NULL;
    IDXGIOutput *output = NULL;
    DXGI_MODE_DESC *mode_list = NULL;
    UINT mode_count = 0;
    UINT i;
    HRESULT hr;

    static const DXGI_FORMAT formats[] = {
        DXGI_FORMAT_B8G8R8X8_UNORM,
        DXGI_FORMAT_B8G8R8A8_UNORM,
        DXGI_FORMAT_R8G8B8A8_UNORM,
    };
    int f;

    /* Free old list */
    if (g_backend.modes) {
        HeapFree(GetProcessHeap(), 0, g_backend.modes);
        g_backend.modes = NULL;
    }
    g_backend.mode_count = 0;
    g_backend.mode_capacity = 0;

    /* Create a temporary DXGI factory for enumeration */
    hr = CreateDXGIFactory1(&IID_IDXGIFactory1, (void**)&factory);
    if (FAILED(hr)) {
        WRAPPER_LOG("Backend_EnumerateModes: CreateDXGIFactory1 FAILED hr=0x%08lX", hr);
        return;
    }

    /* Get the default adapter and its primary output */
    hr = IDXGIFactory1_EnumAdapters(factory, 0, &adapter);
    if (FAILED(hr)) {
        WRAPPER_LOG("Backend_EnumerateModes: EnumAdapters FAILED hr=0x%08lX", hr);
        IDXGIFactory1_Release(factory);
        return;
    }

    hr = IDXGIAdapter_EnumOutputs(adapter, 0, &output);
    if (FAILED(hr)) {
        WRAPPER_LOG("Backend_EnumerateModes: EnumOutputs FAILED hr=0x%08lX", hr);
        IDXGIAdapter_Release(adapter);
        IDXGIFactory1_Release(factory);
        return;
    }

    for (f = 0; f < 3; f++) {
        mode_count = 0;
        hr = IDXGIOutput_GetDisplayModeList(output, formats[f], 0, &mode_count, NULL);
        if (FAILED(hr) || mode_count == 0) continue;

        mode_list = (DXGI_MODE_DESC*)HeapAlloc(GetProcessHeap(), 0, mode_count * sizeof(DXGI_MODE_DESC));
        if (!mode_list) continue;

        hr = IDXGIOutput_GetDisplayModeList(output, formats[f], 0, &mode_count, mode_list);
        if (FAILED(hr)) {
            HeapFree(GetProcessHeap(), 0, mode_list);
            continue;
        }

        for (i = 0; i < mode_count; i++) {
            /* Grow array if needed */
            if (g_backend.mode_count >= g_backend.mode_capacity) {
                int new_cap = g_backend.mode_capacity ? g_backend.mode_capacity * 2 : 128;
                DXGI_MODE_DESC *new_arr = (DXGI_MODE_DESC*)HeapAlloc(GetProcessHeap(), 0, new_cap * sizeof(DXGI_MODE_DESC));
                if (!new_arr) continue;
                if (g_backend.modes) {
                    CopyMemory(new_arr, g_backend.modes, g_backend.mode_count * sizeof(DXGI_MODE_DESC));
                    HeapFree(GetProcessHeap(), 0, g_backend.modes);
                }
                g_backend.modes = new_arr;
                g_backend.mode_capacity = new_cap;
            }
            g_backend.modes[g_backend.mode_count++] = mode_list[i];
        }

        HeapFree(GetProcessHeap(), 0, mode_list);
    }

    IDXGIOutput_Release(output);
    IDXGIAdapter_Release(adapter);
    IDXGIFactory1_Release(factory);
}

/* ========================================================================
 * Exported functions (replacing ddraw.dll exports)
 * ======================================================================== */

__declspec(dllexport)
HRESULT WINAPI DirectDrawCreate(GUID *driver, void **ddraw, void *outer)
{
    WRAPPER_LOG("DirectDrawCreate called");
    (void)driver;
    (void)outer;

    if (!ddraw) return E_POINTER;

    /* Ensure backend is initialized */
    {
        static int s_initialized = 0;
        if (!s_initialized) {
            if (!Backend_Init()) return DDERR_GENERIC;
            s_initialized = 1;
        }
    }

    /* ---------------------------------------------------------------
     * Native resolution patches: applied ONCE, before M2DX enumerates
     * display modes. Makes M2DX select native monitor resolution as
     * its preferred mode, and patches the game's frontend canvas
     * globals so the entire UI renders at native resolution.
     * --------------------------------------------------------------- */
    {
        static int s_patched = 0;
        if (!s_patched) {
            int nw, nh;
            char ini_path[MAX_PATH];
            GetModuleFileNameA(NULL, ini_path, MAX_PATH);
            { char *s = strrchr(ini_path, '\\'); if (s) strcpy(s + 1, "td5re.ini"); }
            nw = GetPrivateProfileIntA("Display", "Width", 0, ini_path);
            nh = GetPrivateProfileIntA("Display", "Height", 0, ini_path);
            if (nw <= 0 || nh <= 0) {
                nw = GetSystemMetrics(SM_CXSCREEN);
                nh = GetSystemMetrics(SM_CYSCREEN);
            }
            if (nw > 0 && nh > 0 && (nw != 640 || nh != 480)) {
                DWORD old;
                HMODULE m2dx = GetModuleHandleA("M2DX.dll");

                WRAPPER_LOG("Applying native resolution patches: %dx%d", nw, nh);

                /* --- M2DX.dll: accept widescreen + prefer native mode --- */
                if (m2dx) {
                    uintptr_t base = (uintptr_t)m2dx;
                    BYTE jmp = 0xEB;

                    /* Patch 1: bypass 4:3 aspect ratio filter (JZ -> JMP) */
                    VirtualProtect((void *)(base + 0x80B3), 1, PAGE_EXECUTE_READWRITE, &old);
                    *(BYTE *)(base + 0x80B3) = jmp;
                    VirtualProtect((void *)(base + 0x80B3), 1, old, &old);

                    /* Patch 2-3: EnumDisplayModes default width/height */
                    VirtualProtect((void *)(base + 0x7F94), 20, PAGE_EXECUTE_READWRITE, &old);
                    *(int *)(base + 0x7F94) = nw;
                    *(int *)(base + 0x7FA4) = nh;
                    VirtualProtect((void *)(base + 0x7F94), 20, old, &old);

                    /* Patch 4-5: SelectPreferredDisplayMode width/height */
                    VirtualProtect((void *)(base + 0x2BC0), 16, PAGE_EXECUTE_READWRITE, &old);
                    *(int *)(base + 0x2BC0) = nw;
                    *(int *)(base + 0x2BC9) = nh;
                    VirtualProtect((void *)(base + 0x2BC0), 16, old, &old);

                    WRAPPER_LOG("  M2DX.dll patches applied (base=%p)", (void *)base);
                }

                /* --- TD5_d3d.exe: native resolution throughout --- */

                /* Patch 6-7: WinMain app_exref width/height */
                VirtualProtect((void *)0x430AB2, 20, PAGE_EXECUTE_READWRITE, &old);
                *(int *)0x430AB2 = nw;
                *(int *)0x430AC2 = nh;
                VirtualProtect((void *)0x430AB2, 20, old, &old);

                /* Patch 8-9: InitializeFrontendResourcesAndState canvas globals */
                VirtualProtect((void *)0x4148B2, 16, PAGE_EXECUTE_READWRITE, &old);
                *(int *)0x4148B2 = nw;
                *(int *)0x4148BC = nh;
                VirtualProtect((void *)0x4148B2, 16, old, &old);

                /* Patch 10-11: RunFrontendDisplayLoop BlitFrontendCachedRect args */
                VirtualProtect((void *)0x414B8D, 10, PAGE_EXECUTE_READWRITE, &old);
                *(int *)0x414B8D = nh;   /* PUSH height */
                *(int *)0x414B92 = nw;   /* PUSH width */
                VirtualProtect((void *)0x414B8D, 10, old, &old);

                WRAPPER_LOG("  EXE patches applied (frontend canvas %dx%d)", nw, nh);
            }
            s_patched = 1;
        }
    }

    g_backend.ddraw = WrapperDirectDraw_Create();
    if (!g_backend.ddraw) return E_OUTOFMEMORY;

    *ddraw = g_backend.ddraw;
    WRAPPER_LOG("DirectDrawCreate: returned WrapperDirectDraw %p", g_backend.ddraw);
    return DD_OK;
}

__declspec(dllexport)
HRESULT WINAPI DirectDrawCreateEx(GUID *driver, void **ddraw, REFIID iid, void *outer)
{
    WRAPPER_LOG("DirectDrawCreateEx called (iid ignored, same as DirectDrawCreate)");
    (void)iid;
    return DirectDrawCreate(driver, ddraw, outer);
}

__declspec(dllexport)
HRESULT WINAPI DirectDrawEnumerateA(LPDDENUMCALLBACKA callback, void *ctx)
{
    WRAPPER_LOG("DirectDrawEnumerateA called");
    if (!callback) return E_POINTER;

    callback(NULL, "D3D11 Wrapper", "Primary", ctx);
    return DD_OK;
}

__declspec(dllexport)
HRESULT WINAPI DirectDrawEnumerateExA(LPDDENUMCALLBACKEXA callback, void *ctx, DWORD flags)
{
    WRAPPER_LOG("DirectDrawEnumerateExA called (flags=0x%08X)", flags);
    (void)flags;
    if (!callback) return E_POINTER;

    callback(NULL, "D3D11 Wrapper", "Primary", ctx, NULL);
    return DD_OK;
}

/* ========================================================================
 * DllMain
 * ======================================================================== */

/* Crash handler to log the faulting address */
static LONG WINAPI CrashHandler(EXCEPTION_POINTERS *ep)
{
    if (ep && ep->ExceptionRecord && ep->ContextRecord) {
        DWORD esp = ep->ContextRecord->Esp;
        WRAPPER_LOG("!!! CRASH: code=0x%08X addr=0x%08X EIP=0x%08X",
                    (unsigned)ep->ExceptionRecord->ExceptionCode,
                    (unsigned)(DWORD_PTR)ep->ExceptionRecord->ExceptionAddress,
                    (unsigned)ep->ContextRecord->Eip);
        WRAPPER_LOG("    EAX=%08X EBX=%08X ECX=%08X EDX=%08X",
                    (unsigned)ep->ContextRecord->Eax, (unsigned)ep->ContextRecord->Ebx,
                    (unsigned)ep->ContextRecord->Ecx, (unsigned)ep->ContextRecord->Edx);
        WRAPPER_LOG("    ESP=%08X EBP=%08X ESI=%08X EDI=%08X",
                    (unsigned)ep->ContextRecord->Esp, (unsigned)ep->ContextRecord->Ebp,
                    (unsigned)ep->ContextRecord->Esi, (unsigned)ep->ContextRecord->Edi);
        /* Dump stack — look for return addresses in EXE (0x40xxxx) and M2DX (0x100xxxxx) */
        {
            DWORD *stk = (DWORD*)esp;
            int i;
            WRAPPER_LOG("    Stack dump (ESP=%08X):", esp);
            for (i = 0; i < 32; i += 4) {
                WRAPPER_LOG("      [+%02X] %08X %08X %08X %08X",
                    i*4, stk[i], stk[i+1], stk[i+2], stk[i+3]);
            }
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved)
{
    (void)hinstDLL;
    (void)lpvReserved;

    switch (fdwReason) {
    case DLL_PROCESS_ATTACH:
        WRAPPER_LOG("=== D3D11 Wrapper DLL loaded ===");
        DisableThreadLibraryCalls(hinstDLL);
        SetUnhandledExceptionFilter(CrashHandler);
        break;

    case DLL_PROCESS_DETACH:
        WRAPPER_LOG("=== D3D11 Wrapper DLL unloading ===");
        Backend_Shutdown();
        break;
    }
    return TRUE;
}
