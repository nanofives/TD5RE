/*
 * idd_wrap.c — wraps IDirectDraw (v1) / IDirectDraw4 / IDirectDraw7.
 *
 * Every method delegates to the underlying real interface except:
 *   QueryInterface        — hands back a wrapped IDD4/IDD7 when asked.
 *   SetCooperativeLevel   — flags forced to DDSCL_NORMAL.
 *   SetDisplayMode        — success, no actual mode change.
 *   CreateSurface         — delegates for now; primary wrap lands later.
 *
 * We use the COM-in-C pattern: each wrapper owns a custom lpVtbl plus a
 * pointer to the real interface (real_vN). AddRef/Release refcount our
 * wrapper and bridge to the real's refcount.
 */

#define CINTERFACE
#include "td5_ddraw.h"
#include <stdio.h>

/* ----- helper: IID match macro (CINTERFACE doesn't pull in IsEqualIID C
 *       inline in every SDK flavour; write our own.) */
static inline int iid_eq(REFIID a, REFIID b)
{
    return memcmp(a, b, sizeof(GUID)) == 0;
}

/* Make sure the game window's CLIENT area is exactly 640x480 (or whatever
 * the game asked for via SetCooperativeLevel + CreateSurface primary size).
 * M2DX's ApplyWindowedRenderSize is supposed to do this but fights modern
 * DPI behaviour. We force it after we see the SCL. Idempotent — fine to
 * call every SCL. */
static void force_client_size(HWND hwnd, int cw, int ch)
{
    if (!hwnd || !IsWindow(hwnd)) return;
    RECT target = { 0, 0, cw, ch };
    DWORD style   = (DWORD)GetWindowLongA(hwnd, GWL_STYLE);
    DWORD exstyle = (DWORD)GetWindowLongA(hwnd, GWL_EXSTYLE);
    AdjustWindowRectEx(&target, style, FALSE, exstyle);
    int w = target.right  - target.left;
    int h = target.bottom - target.top;
    SetWindowPos(hwnd, NULL, 0, 0, w, h,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    td5dd_log("[proxy] forced client size %dx%d (window %dx%d)", cw, ch, w, h);
}

/* ---------- forward: wrapper constructors (defined below) ------------- */
LPDIRECTDRAW   td5dd_wrap_dd1(LPDIRECTDRAW  real);
LPDIRECTDRAW4  td5dd_wrap_dd4(LPDIRECTDRAW4 real);
LPDIRECTDRAW7  td5dd_wrap_dd7(LPDIRECTDRAW7 real);

/* =====================================================================
 *                          IDirectDraw (v1)
 * ===================================================================== */
typedef struct {
    IDirectDrawVtbl *lpVtbl;
    LPDIRECTDRAW     real;
    LONG             refcount;
} WrapDD1;

#define S1 ((WrapDD1 *)This)
#define R1 (S1->real)

static HRESULT STDMETHODCALLTYPE dd1_QI(IDirectDraw *This, REFIID riid, void **ppv)
{
    if (!ppv) return E_POINTER;
    *ppv = NULL;

    /* Ask real impl. If it gave us an IDD/IDD2/IDD4/IDD7, wrap before return. */
    void *raw = NULL;
    HRESULT hr = R1->lpVtbl->QueryInterface(R1, riid, &raw);
    if (FAILED(hr) || !raw) return hr;

    if (iid_eq(riid, &IID_IDirectDraw7)) {
        *ppv = td5dd_wrap_dd7((LPDIRECTDRAW7)raw);
    } else if (iid_eq(riid, &IID_IDirectDraw4)) {
        *ppv = td5dd_wrap_dd4((LPDIRECTDRAW4)raw);
    } else if (iid_eq(riid, &IID_IDirectDraw) || iid_eq(riid, &IID_IUnknown)) {
        /* Same object; bump our refcount and return ourselves. Release the
         * extra ref the real took on us. */
        ((LPDIRECTDRAW)raw)->lpVtbl->Release((LPDIRECTDRAW)raw);
        This->lpVtbl->AddRef(This);
        *ppv = This;
    } else {
        /* Unknown interface — pass through raw. Caller deals with it. */
        *ppv = raw;
    }
    return DD_OK;
}
static ULONG STDMETHODCALLTYPE dd1_AddRef(IDirectDraw *This)
{
    InterlockedIncrement(&S1->refcount);
    return R1->lpVtbl->AddRef(R1);
}
static ULONG STDMETHODCALLTYPE dd1_Release(IDirectDraw *This)
{
    ULONG r = R1->lpVtbl->Release(R1);
    if (InterlockedDecrement(&S1->refcount) == 0) {
        HeapFree(GetProcessHeap(), 0, S1);
    }
    return r;
}
static HRESULT STDMETHODCALLTYPE dd1_Compact(IDirectDraw *This)
    { return R1->lpVtbl->Compact(R1); }
static HRESULT STDMETHODCALLTYPE dd1_CreateClipper(IDirectDraw *This, DWORD a, LPDIRECTDRAWCLIPPER *b, IUnknown *c)
    { return R1->lpVtbl->CreateClipper(R1, a, b, c); }
static HRESULT STDMETHODCALLTYPE dd1_CreatePalette(IDirectDraw *This, DWORD a, LPPALETTEENTRY b, LPDIRECTDRAWPALETTE *c, IUnknown *d)
    { return R1->lpVtbl->CreatePalette(R1, a, b, c, d); }
static HRESULT STDMETHODCALLTYPE dd1_CreateSurface(IDirectDraw *This, LPDDSURFACEDESC a, LPDIRECTDRAWSURFACE *b, IUnknown *c)
    { return R1->lpVtbl->CreateSurface(R1, a, b, c); }
static HRESULT STDMETHODCALLTYPE dd1_DuplicateSurface(IDirectDraw *This, LPDIRECTDRAWSURFACE a, LPDIRECTDRAWSURFACE *b)
    { return R1->lpVtbl->DuplicateSurface(R1, a, b); }
static HRESULT STDMETHODCALLTYPE dd1_EnumDisplayModes(IDirectDraw *This, DWORD a, LPDDSURFACEDESC b, LPVOID c, LPDDENUMMODESCALLBACK d)
    { return R1->lpVtbl->EnumDisplayModes(R1, a, b, c, d); }
static HRESULT STDMETHODCALLTYPE dd1_EnumSurfaces(IDirectDraw *This, DWORD a, LPDDSURFACEDESC b, LPVOID c, LPDDENUMSURFACESCALLBACK d)
    { return R1->lpVtbl->EnumSurfaces(R1, a, b, c, d); }
static HRESULT STDMETHODCALLTYPE dd1_FlipToGDISurface(IDirectDraw *This)
    { return R1->lpVtbl->FlipToGDISurface(R1); }
static HRESULT STDMETHODCALLTYPE dd1_GetCaps(IDirectDraw *This, LPDDCAPS a, LPDDCAPS b)
    { return R1->lpVtbl->GetCaps(R1, a, b); }
static HRESULT STDMETHODCALLTYPE dd1_GetDisplayMode(IDirectDraw *This, LPDDSURFACEDESC a)
    { return R1->lpVtbl->GetDisplayMode(R1, a); }
static HRESULT STDMETHODCALLTYPE dd1_GetFourCCCodes(IDirectDraw *This, LPDWORD a, LPDWORD b)
    { return R1->lpVtbl->GetFourCCCodes(R1, a, b); }
static HRESULT STDMETHODCALLTYPE dd1_GetGDISurface(IDirectDraw *This, LPDIRECTDRAWSURFACE *a)
    { return R1->lpVtbl->GetGDISurface(R1, a); }
static HRESULT STDMETHODCALLTYPE dd1_GetMonitorFrequency(IDirectDraw *This, LPDWORD a)
    { return R1->lpVtbl->GetMonitorFrequency(R1, a); }
static HRESULT STDMETHODCALLTYPE dd1_GetScanLine(IDirectDraw *This, LPDWORD a)
    { return R1->lpVtbl->GetScanLine(R1, a); }
static HRESULT STDMETHODCALLTYPE dd1_GetVerticalBlankStatus(IDirectDraw *This, WINBOOL *a)
    { return R1->lpVtbl->GetVerticalBlankStatus(R1, a); }
static HRESULT STDMETHODCALLTYPE dd1_Initialize(IDirectDraw *This, GUID *a)
    { return R1->lpVtbl->Initialize(R1, a); }
static HRESULT STDMETHODCALLTYPE dd1_RestoreDisplayMode(IDirectDraw *This)
    { td5dd_log("[dd1] RestoreDisplayMode -> no-op"); return DD_OK; }
static HRESULT STDMETHODCALLTYPE dd1_SetCooperativeLevel(IDirectDraw *This, HWND hwnd, DWORD flags)
{
    td5dd_log("[dd1] SetCooperativeLevel(hwnd=%p flags=0x%08lX) forced DDSCL_NORMAL",
              (void *)hwnd, (unsigned long)flags);
    if (hwnd) g_td5dd_hwnd = hwnd;
    return R1->lpVtbl->SetCooperativeLevel(R1, hwnd, DDSCL_NORMAL);
}
static HRESULT STDMETHODCALLTYPE dd1_SetDisplayMode(IDirectDraw *This, DWORD w, DWORD h, DWORD bpp)
{
    td5dd_log("[dd1] SetDisplayMode(%lu,%lu,%lu) -> DD_OK (no change)",
              (unsigned long)w, (unsigned long)h, (unsigned long)bpp);
    return DD_OK;
}
static HRESULT STDMETHODCALLTYPE dd1_WaitForVerticalBlank(IDirectDraw *This, DWORD a, HANDLE b)
    { return R1->lpVtbl->WaitForVerticalBlank(R1, a, b); }

static IDirectDrawVtbl g_dd1_vtbl = {
    dd1_QI, dd1_AddRef, dd1_Release,
    dd1_Compact, dd1_CreateClipper, dd1_CreatePalette, dd1_CreateSurface,
    dd1_DuplicateSurface, dd1_EnumDisplayModes, dd1_EnumSurfaces,
    dd1_FlipToGDISurface, dd1_GetCaps, dd1_GetDisplayMode, dd1_GetFourCCCodes,
    dd1_GetGDISurface, dd1_GetMonitorFrequency, dd1_GetScanLine,
    dd1_GetVerticalBlankStatus, dd1_Initialize, dd1_RestoreDisplayMode,
    dd1_SetCooperativeLevel, dd1_SetDisplayMode, dd1_WaitForVerticalBlank,
};

LPDIRECTDRAW td5dd_wrap_dd1(LPDIRECTDRAW real)
{
    WrapDD1 *w = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*w));
    if (!w) return NULL;
    w->lpVtbl   = &g_dd1_vtbl;
    w->real     = real;
    w->refcount = 1;
    return (LPDIRECTDRAW)w;
}
#undef S1
#undef R1

/* =====================================================================
 *                          IDirectDraw4
 * ===================================================================== */
typedef struct {
    IDirectDraw4Vtbl *lpVtbl;
    LPDIRECTDRAW4     real;
    LONG              refcount;
} WrapDD4;

#define S4 ((WrapDD4 *)This)
#define R4 (S4->real)

static HRESULT STDMETHODCALLTYPE dd4_QI(IDirectDraw4 *This, REFIID riid, void **ppv)
{
    if (!ppv) return E_POINTER;
    *ppv = NULL;
    void *raw = NULL;
    HRESULT hr = R4->lpVtbl->QueryInterface(R4, riid, &raw);
    if (FAILED(hr) || !raw) return hr;
    if (iid_eq(riid, &IID_IDirectDraw7))      *ppv = td5dd_wrap_dd7((LPDIRECTDRAW7)raw);
    else if (iid_eq(riid, &IID_IDirectDraw4)) {
        ((LPDIRECTDRAW4)raw)->lpVtbl->Release((LPDIRECTDRAW4)raw);
        This->lpVtbl->AddRef(This);
        *ppv = This;
    }
    else if (iid_eq(riid, &IID_IDirectDraw))  *ppv = td5dd_wrap_dd1((LPDIRECTDRAW)raw);
    else *ppv = raw;
    return DD_OK;
}
static ULONG STDMETHODCALLTYPE dd4_AddRef(IDirectDraw4 *This)
    { InterlockedIncrement(&S4->refcount); return R4->lpVtbl->AddRef(R4); }
static ULONG STDMETHODCALLTYPE dd4_Release(IDirectDraw4 *This)
{
    ULONG r = R4->lpVtbl->Release(R4);
    if (InterlockedDecrement(&S4->refcount) == 0) HeapFree(GetProcessHeap(), 0, S4);
    return r;
}
static HRESULT STDMETHODCALLTYPE dd4_Compact(IDirectDraw4 *This)
    { return R4->lpVtbl->Compact(R4); }
static HRESULT STDMETHODCALLTYPE dd4_CreateClipper(IDirectDraw4 *This, DWORD a, LPDIRECTDRAWCLIPPER *b, IUnknown *c)
    { return R4->lpVtbl->CreateClipper(R4, a, b, c); }
static HRESULT STDMETHODCALLTYPE dd4_CreatePalette(IDirectDraw4 *This, DWORD a, LPPALETTEENTRY b, LPDIRECTDRAWPALETTE *c, IUnknown *d)
    { return R4->lpVtbl->CreatePalette(R4, a, b, c, d); }
static HRESULT STDMETHODCALLTYPE dd4_CreateSurface(IDirectDraw4 *This, LPDDSURFACEDESC2 a, LPDIRECTDRAWSURFACE4 *b, IUnknown *c)
{
    /* Force 16bpp RGB565 pixel format on offscreen surfaces that don't
     * specify one. Real DDraw defaults these to the desktop (32bpp), but
     * the game writes 16bpp bytes into them → corrupted menu colors. We
     * leave primary, Z-buffer, palette and any surface with an explicit
     * pixel format alone. */
    DDSURFACEDESC2 patched;
    if (a && !(a->dwFlags & DDSD_PIXELFORMAT)
          && !(a->ddsCaps.dwCaps & (DDSCAPS_PRIMARYSURFACE |
                                    DDSCAPS_ZBUFFER |
                                    DDSCAPS_PALETTE)))
    {
        patched = *a;
        patched.dwFlags |= DDSD_PIXELFORMAT;
        patched.ddpfPixelFormat.dwSize        = sizeof(DDPIXELFORMAT);
        patched.ddpfPixelFormat.dwFlags       = DDPF_RGB;
        patched.ddpfPixelFormat.dwRGBBitCount = 16;
        patched.ddpfPixelFormat.dwRBitMask    = 0xF800;
        patched.ddpfPixelFormat.dwGBitMask    = 0x07E0;
        patched.ddpfPixelFormat.dwBBitMask    = 0x001F;
        a = &patched;
    }
    HRESULT hr = R4->lpVtbl->CreateSurface(R4, a, b, c);
    if (a && (a->ddsCaps.dwCaps & DDSCAPS_PRIMARYSURFACE)) {
        DWORD w = (a->dwFlags & DDSD_WIDTH)  ? a->dwWidth  : 640;
        DWORD h = (a->dwFlags & DDSD_HEIGHT) ? a->dwHeight : 480;
        if (w == 0) w = 640;
        if (h == 0) h = 480;
        force_client_size(g_td5dd_hwnd, (int)w, (int)h);
    }
    return hr;
}
static HRESULT STDMETHODCALLTYPE dd4_DuplicateSurface(IDirectDraw4 *This, LPDIRECTDRAWSURFACE4 a, LPDIRECTDRAWSURFACE4 *b)
    { return R4->lpVtbl->DuplicateSurface(R4, a, b); }
static HRESULT STDMETHODCALLTYPE dd4_EnumDisplayModes(IDirectDraw4 *This, DWORD a, LPDDSURFACEDESC2 b, LPVOID c, LPDDENUMMODESCALLBACK2 d)
    { return R4->lpVtbl->EnumDisplayModes(R4, a, b, c, d); }
static HRESULT STDMETHODCALLTYPE dd4_EnumSurfaces(IDirectDraw4 *This, DWORD a, LPDDSURFACEDESC2 b, LPVOID c, LPDDENUMSURFACESCALLBACK2 d)
    { return R4->lpVtbl->EnumSurfaces(R4, a, b, c, d); }
static HRESULT STDMETHODCALLTYPE dd4_FlipToGDISurface(IDirectDraw4 *This)
    { return R4->lpVtbl->FlipToGDISurface(R4); }
static HRESULT STDMETHODCALLTYPE dd4_GetCaps(IDirectDraw4 *This, LPDDCAPS a, LPDDCAPS b)
    { return R4->lpVtbl->GetCaps(R4, a, b); }
static HRESULT STDMETHODCALLTYPE dd4_GetDisplayMode(IDirectDraw4 *This, LPDDSURFACEDESC2 a)
    { return R4->lpVtbl->GetDisplayMode(R4, a); }
static HRESULT STDMETHODCALLTYPE dd4_GetFourCCCodes(IDirectDraw4 *This, LPDWORD a, LPDWORD b)
    { return R4->lpVtbl->GetFourCCCodes(R4, a, b); }
static HRESULT STDMETHODCALLTYPE dd4_GetGDISurface(IDirectDraw4 *This, LPDIRECTDRAWSURFACE4 *a)
    { return R4->lpVtbl->GetGDISurface(R4, a); }
static HRESULT STDMETHODCALLTYPE dd4_GetMonitorFrequency(IDirectDraw4 *This, LPDWORD a)
    { return R4->lpVtbl->GetMonitorFrequency(R4, a); }
static HRESULT STDMETHODCALLTYPE dd4_GetScanLine(IDirectDraw4 *This, LPDWORD a)
    { return R4->lpVtbl->GetScanLine(R4, a); }
static HRESULT STDMETHODCALLTYPE dd4_GetVerticalBlankStatus(IDirectDraw4 *This, WINBOOL *a)
    { return R4->lpVtbl->GetVerticalBlankStatus(R4, a); }
static HRESULT STDMETHODCALLTYPE dd4_Initialize(IDirectDraw4 *This, GUID *a)
    { return R4->lpVtbl->Initialize(R4, a); }
static HRESULT STDMETHODCALLTYPE dd4_RestoreDisplayMode(IDirectDraw4 *This)
    { td5dd_log("[dd4] RestoreDisplayMode -> no-op"); return DD_OK; }
static HRESULT STDMETHODCALLTYPE dd4_SetCooperativeLevel(IDirectDraw4 *This, HWND hwnd, DWORD flags)
{
    td5dd_log("[dd4] SetCooperativeLevel(hwnd=%p flags=0x%08lX) forced DDSCL_NORMAL",
              (void *)hwnd, (unsigned long)flags);
    if (hwnd) g_td5dd_hwnd = hwnd;
    HRESULT hr = R4->lpVtbl->SetCooperativeLevel(R4, hwnd, DDSCL_NORMAL);
    force_client_size(hwnd, 640, 480);
    return hr;
}
static HRESULT STDMETHODCALLTYPE dd4_SetDisplayMode(IDirectDraw4 *This, DWORD w, DWORD h, DWORD bpp, DWORD rr, DWORD fl)
{
    td5dd_log("[dd4] SetDisplayMode(%lu,%lu,%lu) -> DD_OK (no change)",
              (unsigned long)w, (unsigned long)h, (unsigned long)bpp);
    return DD_OK;
}
static HRESULT STDMETHODCALLTYPE dd4_WaitForVerticalBlank(IDirectDraw4 *This, DWORD a, HANDLE b)
    { return R4->lpVtbl->WaitForVerticalBlank(R4, a, b); }
static HRESULT STDMETHODCALLTYPE dd4_GetAvailableVidMem(IDirectDraw4 *This, LPDDSCAPS2 a, LPDWORD b, LPDWORD c)
    { return R4->lpVtbl->GetAvailableVidMem(R4, a, b, c); }
static HRESULT STDMETHODCALLTYPE dd4_GetSurfaceFromDC(IDirectDraw4 *This, HDC a, LPDIRECTDRAWSURFACE4 *b)
    { return R4->lpVtbl->GetSurfaceFromDC(R4, a, b); }
static HRESULT STDMETHODCALLTYPE dd4_RestoreAllSurfaces(IDirectDraw4 *This)
    { return R4->lpVtbl->RestoreAllSurfaces(R4); }
static HRESULT STDMETHODCALLTYPE dd4_TestCooperativeLevel(IDirectDraw4 *This)
    { return R4->lpVtbl->TestCooperativeLevel(R4); }
static HRESULT STDMETHODCALLTYPE dd4_GetDeviceIdentifier(IDirectDraw4 *This, LPDDDEVICEIDENTIFIER a, DWORD b)
    { return R4->lpVtbl->GetDeviceIdentifier(R4, a, b); }

static IDirectDraw4Vtbl g_dd4_vtbl = {
    dd4_QI, dd4_AddRef, dd4_Release,
    dd4_Compact, dd4_CreateClipper, dd4_CreatePalette, dd4_CreateSurface,
    dd4_DuplicateSurface, dd4_EnumDisplayModes, dd4_EnumSurfaces,
    dd4_FlipToGDISurface, dd4_GetCaps, dd4_GetDisplayMode, dd4_GetFourCCCodes,
    dd4_GetGDISurface, dd4_GetMonitorFrequency, dd4_GetScanLine,
    dd4_GetVerticalBlankStatus, dd4_Initialize, dd4_RestoreDisplayMode,
    dd4_SetCooperativeLevel, dd4_SetDisplayMode, dd4_WaitForVerticalBlank,
    dd4_GetAvailableVidMem, dd4_GetSurfaceFromDC, dd4_RestoreAllSurfaces,
    dd4_TestCooperativeLevel, dd4_GetDeviceIdentifier,
};

LPDIRECTDRAW4 td5dd_wrap_dd4(LPDIRECTDRAW4 real)
{
    WrapDD4 *w = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*w));
    if (!w) return NULL;
    w->lpVtbl   = &g_dd4_vtbl;
    w->real     = real;
    w->refcount = 1;
    return (LPDIRECTDRAW4)w;
}
#undef S4
#undef R4

/* =====================================================================
 *                          IDirectDraw7
 * ===================================================================== */
typedef struct {
    IDirectDraw7Vtbl *lpVtbl;
    LPDIRECTDRAW7     real;
    LONG              refcount;
} WrapDD7;

#define S7 ((WrapDD7 *)This)
#define R7 (S7->real)

static HRESULT STDMETHODCALLTYPE dd7_QI(IDirectDraw7 *This, REFIID riid, void **ppv)
{
    if (!ppv) return E_POINTER;
    *ppv = NULL;
    void *raw = NULL;
    HRESULT hr = R7->lpVtbl->QueryInterface(R7, riid, &raw);
    if (FAILED(hr) || !raw) return hr;
    if (iid_eq(riid, &IID_IDirectDraw7)) {
        ((LPDIRECTDRAW7)raw)->lpVtbl->Release((LPDIRECTDRAW7)raw);
        This->lpVtbl->AddRef(This);
        *ppv = This;
    }
    else if (iid_eq(riid, &IID_IDirectDraw4)) *ppv = td5dd_wrap_dd4((LPDIRECTDRAW4)raw);
    else if (iid_eq(riid, &IID_IDirectDraw))  *ppv = td5dd_wrap_dd1((LPDIRECTDRAW)raw);
    else *ppv = raw;
    return DD_OK;
}
static ULONG STDMETHODCALLTYPE dd7_AddRef(IDirectDraw7 *This)
    { InterlockedIncrement(&S7->refcount); return R7->lpVtbl->AddRef(R7); }
static ULONG STDMETHODCALLTYPE dd7_Release(IDirectDraw7 *This)
{
    ULONG r = R7->lpVtbl->Release(R7);
    if (InterlockedDecrement(&S7->refcount) == 0) HeapFree(GetProcessHeap(), 0, S7);
    return r;
}
static HRESULT STDMETHODCALLTYPE dd7_Compact(IDirectDraw7 *This)
    { return R7->lpVtbl->Compact(R7); }
static HRESULT STDMETHODCALLTYPE dd7_CreateClipper(IDirectDraw7 *This, DWORD a, LPDIRECTDRAWCLIPPER *b, IUnknown *c)
    { return R7->lpVtbl->CreateClipper(R7, a, b, c); }
static HRESULT STDMETHODCALLTYPE dd7_CreatePalette(IDirectDraw7 *This, DWORD a, LPPALETTEENTRY b, LPDIRECTDRAWPALETTE *c, IUnknown *d)
    { return R7->lpVtbl->CreatePalette(R7, a, b, c, d); }
static HRESULT STDMETHODCALLTYPE dd7_CreateSurface(IDirectDraw7 *This, LPDDSURFACEDESC2 a, LPDIRECTDRAWSURFACE7 *b, IUnknown *c)
    { return R7->lpVtbl->CreateSurface(R7, a, b, c); }
static HRESULT STDMETHODCALLTYPE dd7_DuplicateSurface(IDirectDraw7 *This, LPDIRECTDRAWSURFACE7 a, LPDIRECTDRAWSURFACE7 *b)
    { return R7->lpVtbl->DuplicateSurface(R7, a, b); }
static HRESULT STDMETHODCALLTYPE dd7_EnumDisplayModes(IDirectDraw7 *This, DWORD a, LPDDSURFACEDESC2 b, LPVOID c, LPDDENUMMODESCALLBACK2 d)
    { return R7->lpVtbl->EnumDisplayModes(R7, a, b, c, d); }
static HRESULT STDMETHODCALLTYPE dd7_EnumSurfaces(IDirectDraw7 *This, DWORD a, LPDDSURFACEDESC2 b, LPVOID c, LPDDENUMSURFACESCALLBACK7 d)
    { return R7->lpVtbl->EnumSurfaces(R7, a, b, c, d); }
static HRESULT STDMETHODCALLTYPE dd7_FlipToGDISurface(IDirectDraw7 *This)
    { return R7->lpVtbl->FlipToGDISurface(R7); }
static HRESULT STDMETHODCALLTYPE dd7_GetCaps(IDirectDraw7 *This, LPDDCAPS a, LPDDCAPS b)
    { return R7->lpVtbl->GetCaps(R7, a, b); }
static HRESULT STDMETHODCALLTYPE dd7_GetDisplayMode(IDirectDraw7 *This, LPDDSURFACEDESC2 a)
    { return R7->lpVtbl->GetDisplayMode(R7, a); }
static HRESULT STDMETHODCALLTYPE dd7_GetFourCCCodes(IDirectDraw7 *This, LPDWORD a, LPDWORD b)
    { return R7->lpVtbl->GetFourCCCodes(R7, a, b); }
static HRESULT STDMETHODCALLTYPE dd7_GetGDISurface(IDirectDraw7 *This, LPDIRECTDRAWSURFACE7 *a)
    { return R7->lpVtbl->GetGDISurface(R7, a); }
static HRESULT STDMETHODCALLTYPE dd7_GetMonitorFrequency(IDirectDraw7 *This, LPDWORD a)
    { return R7->lpVtbl->GetMonitorFrequency(R7, a); }
static HRESULT STDMETHODCALLTYPE dd7_GetScanLine(IDirectDraw7 *This, LPDWORD a)
    { return R7->lpVtbl->GetScanLine(R7, a); }
static HRESULT STDMETHODCALLTYPE dd7_GetVerticalBlankStatus(IDirectDraw7 *This, WINBOOL *a)
    { return R7->lpVtbl->GetVerticalBlankStatus(R7, a); }
static HRESULT STDMETHODCALLTYPE dd7_Initialize(IDirectDraw7 *This, GUID *a)
    { return R7->lpVtbl->Initialize(R7, a); }
static HRESULT STDMETHODCALLTYPE dd7_RestoreDisplayMode(IDirectDraw7 *This)
    { td5dd_log("[dd7] RestoreDisplayMode -> no-op"); return DD_OK; }
static HRESULT STDMETHODCALLTYPE dd7_SetCooperativeLevel(IDirectDraw7 *This, HWND hwnd, DWORD flags)
{
    td5dd_log("[dd7] SetCooperativeLevel(hwnd=%p flags=0x%08lX) forced DDSCL_NORMAL",
              (void *)hwnd, (unsigned long)flags);
    if (hwnd) g_td5dd_hwnd = hwnd;
    return R7->lpVtbl->SetCooperativeLevel(R7, hwnd, DDSCL_NORMAL);
}
static HRESULT STDMETHODCALLTYPE dd7_SetDisplayMode(IDirectDraw7 *This, DWORD w, DWORD h, DWORD bpp, DWORD rr, DWORD fl)
{
    td5dd_log("[dd7] SetDisplayMode(%lu,%lu,%lu) -> DD_OK (no change)",
              (unsigned long)w, (unsigned long)h, (unsigned long)bpp);
    return DD_OK;
}
static HRESULT STDMETHODCALLTYPE dd7_WaitForVerticalBlank(IDirectDraw7 *This, DWORD a, HANDLE b)
    { return R7->lpVtbl->WaitForVerticalBlank(R7, a, b); }
static HRESULT STDMETHODCALLTYPE dd7_GetAvailableVidMem(IDirectDraw7 *This, LPDDSCAPS2 a, LPDWORD b, LPDWORD c)
    { return R7->lpVtbl->GetAvailableVidMem(R7, a, b, c); }
static HRESULT STDMETHODCALLTYPE dd7_GetSurfaceFromDC(IDirectDraw7 *This, HDC a, LPDIRECTDRAWSURFACE7 *b)
    { return R7->lpVtbl->GetSurfaceFromDC(R7, a, b); }
static HRESULT STDMETHODCALLTYPE dd7_RestoreAllSurfaces(IDirectDraw7 *This)
    { return R7->lpVtbl->RestoreAllSurfaces(R7); }
static HRESULT STDMETHODCALLTYPE dd7_TestCooperativeLevel(IDirectDraw7 *This)
    { return R7->lpVtbl->TestCooperativeLevel(R7); }
static HRESULT STDMETHODCALLTYPE dd7_GetDeviceIdentifier(IDirectDraw7 *This, LPDDDEVICEIDENTIFIER2 a, DWORD b)
    { return R7->lpVtbl->GetDeviceIdentifier(R7, a, b); }
static HRESULT STDMETHODCALLTYPE dd7_StartModeTest(IDirectDraw7 *This, LPSIZE a, DWORD b, DWORD c)
    { return R7->lpVtbl->StartModeTest(R7, a, b, c); }
static HRESULT STDMETHODCALLTYPE dd7_EvaluateMode(IDirectDraw7 *This, DWORD a, DWORD *b)
    { return R7->lpVtbl->EvaluateMode(R7, a, b); }

static IDirectDraw7Vtbl g_dd7_vtbl = {
    dd7_QI, dd7_AddRef, dd7_Release,
    dd7_Compact, dd7_CreateClipper, dd7_CreatePalette, dd7_CreateSurface,
    dd7_DuplicateSurface, dd7_EnumDisplayModes, dd7_EnumSurfaces,
    dd7_FlipToGDISurface, dd7_GetCaps, dd7_GetDisplayMode, dd7_GetFourCCCodes,
    dd7_GetGDISurface, dd7_GetMonitorFrequency, dd7_GetScanLine,
    dd7_GetVerticalBlankStatus, dd7_Initialize, dd7_RestoreDisplayMode,
    dd7_SetCooperativeLevel, dd7_SetDisplayMode, dd7_WaitForVerticalBlank,
    dd7_GetAvailableVidMem, dd7_GetSurfaceFromDC, dd7_RestoreAllSurfaces,
    dd7_TestCooperativeLevel, dd7_GetDeviceIdentifier,
    dd7_StartModeTest, dd7_EvaluateMode,
};

LPDIRECTDRAW7 td5dd_wrap_dd7(LPDIRECTDRAW7 real)
{
    WrapDD7 *w = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*w));
    if (!w) return NULL;
    w->lpVtbl   = &g_dd7_vtbl;
    w->real     = real;
    w->refcount = 1;
    return (LPDIRECTDRAW7)w;
}
