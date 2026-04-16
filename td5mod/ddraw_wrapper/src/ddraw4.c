/**
 * ddraw4.c - IDirectDraw4 COM implementation
 *
 * Implements the full IDirectDraw4 vtable backed by D3D11.
 * IDirectDraw4 inherits: IUnknown -> IDirectDraw -> IDirectDraw2 -> IDirectDraw4
 *
 * Vtable has 27 methods total (3 IUnknown + 21 IDirectDraw + 1 IDirectDraw2 + 2 IDirectDraw4)
 * Actually IDirectDraw4 has 27 entries:
 *   IUnknown: 0-2 (QI, AddRef, Release)
 *   IDirectDraw: 3-21 (Compact through WaitForVerticalBlank)
 *   IDirectDraw2: 22 (GetAvailableVidMem)
 *   IDirectDraw4: 23-26 (GetSurfaceFromDC, RestoreAllSurfaces, TestCooperativeLevel, GetDeviceIdentifier)
 */

#include "wrapper.h"
#include <string.h>

/* ========================================================================
 * Forward declarations for all vtable methods
 * ======================================================================== */

static HRESULT __stdcall DD4_QueryInterface(WrapperDirectDraw *self, REFIID riid, void **ppv);
static ULONG   __stdcall DD4_AddRef(WrapperDirectDraw *self);
static ULONG   __stdcall DD4_Release(WrapperDirectDraw *self);
static HRESULT __stdcall DD4_Compact(WrapperDirectDraw *self);
static HRESULT __stdcall DD4_CreateClipper(WrapperDirectDraw *self, DWORD flags, void **clipper, void *outer);
static HRESULT __stdcall DD4_CreatePalette(WrapperDirectDraw *self, DWORD flags, void *entries, void **palette, void *outer);
static HRESULT __stdcall DD4_CreateSurface(WrapperDirectDraw *self, DDSURFACEDESC2_W *desc, WrapperSurface **surf, void *outer);
static HRESULT __stdcall DD4_DuplicateSurface(WrapperDirectDraw *self, void *src, void **dst);
static HRESULT __stdcall DD4_EnumDisplayModes(WrapperDirectDraw *self, DWORD flags, DDSURFACEDESC2_W *filter, void *ctx, LPDDENUMMODESCALLBACK2 callback);
static HRESULT __stdcall DD4_EnumSurfaces(WrapperDirectDraw *self, DWORD flags, DDSURFACEDESC2_W *desc, void *ctx, void *callback);
static HRESULT __stdcall DD4_FlipToGDISurface(WrapperDirectDraw *self);
static HRESULT __stdcall DD4_GetCaps(WrapperDirectDraw *self, void *driver_caps, void *hel_caps);
static HRESULT __stdcall DD4_GetDisplayMode(WrapperDirectDraw *self, DDSURFACEDESC2_W *desc);
static HRESULT __stdcall DD4_GetFourCCCodes(WrapperDirectDraw *self, DWORD *num_codes, DWORD *codes);
static HRESULT __stdcall DD4_GetGDISurface(WrapperDirectDraw *self, void **surface);
static HRESULT __stdcall DD4_GetMonitorFrequency(WrapperDirectDraw *self, DWORD *freq);
static HRESULT __stdcall DD4_GetScanLine(WrapperDirectDraw *self, DWORD *scanline);
static HRESULT __stdcall DD4_GetVerticalBlankStatus(WrapperDirectDraw *self, BOOL *is_vb);
static HRESULT __stdcall DD4_Initialize(WrapperDirectDraw *self, GUID *guid);
static HRESULT __stdcall DD4_RestoreDisplayMode(WrapperDirectDraw *self);
static HRESULT __stdcall DD4_SetCooperativeLevel(WrapperDirectDraw *self, HWND hwnd, DWORD flags);
static HRESULT __stdcall DD4_SetDisplayMode(WrapperDirectDraw *self, DWORD w, DWORD h, DWORD bpp, DWORD refresh, DWORD flags);
static HRESULT __stdcall DD4_WaitForVerticalBlank(WrapperDirectDraw *self, DWORD flags, HANDLE event);
static HRESULT __stdcall DD4_GetAvailableVidMem(WrapperDirectDraw *self, DDSCAPS2_W *caps, DWORD *total, DWORD *free_mem);
static HRESULT __stdcall DD4_GetSurfaceFromDC(WrapperDirectDraw *self, HDC dc, void **surface);
static HRESULT __stdcall DD4_RestoreAllSurfaces(WrapperDirectDraw *self);
static HRESULT __stdcall DD4_TestCooperativeLevel(WrapperDirectDraw *self);
static HRESULT __stdcall DD4_GetDeviceIdentifier(WrapperDirectDraw *self, void *ident, DWORD flags);

/* ========================================================================
 * Vtable definition
 * ======================================================================== */

typedef struct WrapperDirectDrawVtbl {
    /* IUnknown */
    HRESULT (__stdcall *QueryInterface)(WrapperDirectDraw*, REFIID, void**);
    ULONG   (__stdcall *AddRef)(WrapperDirectDraw*);
    ULONG   (__stdcall *Release)(WrapperDirectDraw*);
    /* IDirectDraw */
    HRESULT (__stdcall *Compact)(WrapperDirectDraw*);
    HRESULT (__stdcall *CreateClipper)(WrapperDirectDraw*, DWORD, void**, void*);
    HRESULT (__stdcall *CreatePalette)(WrapperDirectDraw*, DWORD, void*, void**, void*);
    HRESULT (__stdcall *CreateSurface)(WrapperDirectDraw*, DDSURFACEDESC2_W*, WrapperSurface**, void*);
    HRESULT (__stdcall *DuplicateSurface)(WrapperDirectDraw*, void*, void**);
    HRESULT (__stdcall *EnumDisplayModes)(WrapperDirectDraw*, DWORD, DDSURFACEDESC2_W*, void*, LPDDENUMMODESCALLBACK2);
    HRESULT (__stdcall *EnumSurfaces)(WrapperDirectDraw*, DWORD, DDSURFACEDESC2_W*, void*, void*);
    HRESULT (__stdcall *FlipToGDISurface)(WrapperDirectDraw*);
    HRESULT (__stdcall *GetCaps)(WrapperDirectDraw*, void*, void*);
    HRESULT (__stdcall *GetDisplayMode)(WrapperDirectDraw*, DDSURFACEDESC2_W*);
    HRESULT (__stdcall *GetFourCCCodes)(WrapperDirectDraw*, DWORD*, DWORD*);
    HRESULT (__stdcall *GetGDISurface)(WrapperDirectDraw*, void**);
    HRESULT (__stdcall *GetMonitorFrequency)(WrapperDirectDraw*, DWORD*);
    HRESULT (__stdcall *GetScanLine)(WrapperDirectDraw*, DWORD*);
    HRESULT (__stdcall *GetVerticalBlankStatus)(WrapperDirectDraw*, BOOL*);
    HRESULT (__stdcall *Initialize)(WrapperDirectDraw*, GUID*);
    HRESULT (__stdcall *RestoreDisplayMode)(WrapperDirectDraw*);
    HRESULT (__stdcall *SetCooperativeLevel)(WrapperDirectDraw*, HWND, DWORD);
    HRESULT (__stdcall *SetDisplayMode)(WrapperDirectDraw*, DWORD, DWORD, DWORD, DWORD, DWORD);
    HRESULT (__stdcall *WaitForVerticalBlank)(WrapperDirectDraw*, DWORD, HANDLE);
    /* IDirectDraw2 */
    HRESULT (__stdcall *GetAvailableVidMem)(WrapperDirectDraw*, DDSCAPS2_W*, DWORD*, DWORD*);
    /* IDirectDraw4 */
    HRESULT (__stdcall *GetSurfaceFromDC)(WrapperDirectDraw*, HDC, void**);
    HRESULT (__stdcall *RestoreAllSurfaces)(WrapperDirectDraw*);
    HRESULT (__stdcall *TestCooperativeLevel)(WrapperDirectDraw*);
    HRESULT (__stdcall *GetDeviceIdentifier)(WrapperDirectDraw*, void*, DWORD);
} WrapperDirectDrawVtbl;

static WrapperDirectDrawVtbl s_dd4_vtbl = {
    DD4_QueryInterface,
    DD4_AddRef,
    DD4_Release,
    DD4_Compact,
    DD4_CreateClipper,
    DD4_CreatePalette,
    DD4_CreateSurface,
    DD4_DuplicateSurface,
    DD4_EnumDisplayModes,
    DD4_EnumSurfaces,
    DD4_FlipToGDISurface,
    DD4_GetCaps,
    DD4_GetDisplayMode,
    DD4_GetFourCCCodes,
    DD4_GetGDISurface,
    DD4_GetMonitorFrequency,
    DD4_GetScanLine,
    DD4_GetVerticalBlankStatus,
    DD4_Initialize,
    DD4_RestoreDisplayMode,
    DD4_SetCooperativeLevel,
    DD4_SetDisplayMode,
    DD4_WaitForVerticalBlank,
    DD4_GetAvailableVidMem,
    DD4_GetSurfaceFromDC,
    DD4_RestoreAllSurfaces,
    DD4_TestCooperativeLevel,
    DD4_GetDeviceIdentifier,
};

/* ========================================================================
 * WrapperDirectDraw struct (vtable pointer + ref count)
 * ======================================================================== */

struct WrapperDirectDraw {
    WrapperDirectDrawVtbl *vtbl;
    LONG                   ref_count;
};

/* ========================================================================
 * Construction
 * ======================================================================== */

WrapperDirectDraw* WrapperDirectDraw_Create(void)
{
    WrapperDirectDraw *dd = (WrapperDirectDraw*)HeapAlloc(
        GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(WrapperDirectDraw));
    if (!dd) return NULL;

    dd->vtbl = &s_dd4_vtbl;
    dd->ref_count = 1;
    WRAPPER_LOG("WrapperDirectDraw_Create: %p", dd);
    return dd;
}

/* ========================================================================
 * IUnknown methods
 * ======================================================================== */

static HRESULT __stdcall DD4_QueryInterface(WrapperDirectDraw *self, REFIID riid, void **ppv)
{
    WRAPPER_LOG("DD4_QueryInterface: %p iid={%08X-...}", self, riid->Data1);

    if (!ppv) return E_POINTER;

    /* IID_IDirectDraw4, IDirectDraw(v1/v2), or IUnknown - return self.
     * Our object's vtable starts with QI/AddRef/Release so it's compatible
     * with all IDirectDraw versions for the IUnknown methods. */
    extern const GUID IID_IDirectDraw;
    extern const GUID IID_IDirectDraw2;
    if (IsEqualGUID(riid, &IID_IDirectDraw4) ||
        IsEqualGUID(riid, &IID_IDirectDraw) ||
        IsEqualGUID(riid, &IID_IDirectDraw2) ||
        IsEqualGUID(riid, &IID_IUnknown)) {
        DD4_AddRef(self);
        *ppv = self;
        WRAPPER_LOG("DD4_QueryInterface: returning self (IDirectDraw variant)");
        return S_OK;
    }

    /* IID_IDirect3D3 - create/return D3D wrapper */
    if (IsEqualGUID(riid, &IID_IDirect3D3)) {
        if (!g_backend.d3d) {
            g_backend.d3d = WrapperD3D_Create(self);
            if (!g_backend.d3d) {
                *ppv = NULL;
                return E_OUTOFMEMORY;
            }
        }
        /* AddRef on the D3D wrapper is handled inside WrapperD3D_Create or
         * we manually addref here if reusing existing */
        *ppv = g_backend.d3d;
        WRAPPER_LOG("DD4_QueryInterface: returning IDirect3D3 %p", g_backend.d3d);
        return S_OK;
    }

    WRAPPER_LOG("DD4_QueryInterface: unsupported IID {%08X-%04X-%04X-...}",
                riid->Data1, riid->Data2, riid->Data3);
    *ppv = NULL;
    return E_NOINTERFACE;
}

static ULONG __stdcall DD4_AddRef(WrapperDirectDraw *self)
{
    LONG ref = InterlockedIncrement(&self->ref_count);
    WRAPPER_LOG("DD4_AddRef: %p ref=%ld", self, ref);
    return ref;
}

static ULONG __stdcall DD4_Release(WrapperDirectDraw *self)
{
    LONG ref = InterlockedDecrement(&self->ref_count);
    WRAPPER_LOG("DD4_Release: %p ref=%ld", self, ref);
    if (ref <= 0) {
        /* Don't actually free the main DD object -- the game's ref counting
         * is sloppy (M2DX releases more than it AddRefs). Keep the object
         * alive; it's cleaned up at DLL unload. Clamp ref to 0. */
        self->ref_count = 0;
        WRAPPER_LOG("DD4_Release: ref<=0, keeping alive (main DD)");
    }
    return (ref < 0) ? 0 : ref;
}

/* ========================================================================
 * IDirectDraw methods (slots 3-21)
 * ======================================================================== */

/* 3: Compact */
static HRESULT __stdcall DD4_Compact(WrapperDirectDraw *self)
{
    (void)self;
    WRAPPER_STUB("DD4_Compact");
}

/* 4: CreateClipper */
static HRESULT __stdcall DD4_CreateClipper(WrapperDirectDraw *self, DWORD flags, void **clipper, void *outer)
{
    (void)self;
    (void)flags;
    (void)outer;
    WRAPPER_LOG("DD4_CreateClipper");

    if (!clipper) return E_POINTER;

    WrapperClipper *c = WrapperClipper_Create();
    if (!c) return E_OUTOFMEMORY;

    *clipper = c;
    return DD_OK;
}

/* 5: CreatePalette */
static HRESULT __stdcall DD4_CreatePalette(WrapperDirectDraw *self, DWORD flags, void *entries, void **palette, void *outer)
{
    (void)self;
    (void)flags;
    (void)entries;
    (void)outer;
    WRAPPER_LOG("DD4_CreatePalette (stub)");
    if (palette) *palette = NULL;
    return DDERR_UNSUPPORTED;
}

/* 6: CreateSurface - CRITICAL method */
static HRESULT __stdcall DD4_CreateSurface(WrapperDirectDraw *self, DDSURFACEDESC2_W *desc, WrapperSurface **surf, void *outer)
{
    DWORD caps;
    DWORD width, height, bpp;
    WrapperSurface *primary_surf = NULL;
    WrapperSurface *back_surf = NULL;

    (void)self;
    (void)outer;

    if (!desc || !surf) return E_POINTER;

    caps = desc->ddsCaps.dwCaps;
    width  = (desc->dwFlags & DDSD_WIDTH)  ? desc->dwWidth  : g_backend.width;
    height = (desc->dwFlags & DDSD_HEIGHT) ? desc->dwHeight : g_backend.height;
    bpp    = g_backend.bpp ? g_backend.bpp : 16;

    if (desc->dwFlags & DDSD_PIXELFORMAT) {
        if (desc->ddpfPixelFormat.dwRGBBitCount)
            bpp = desc->ddpfPixelFormat.dwRGBBitCount;
    }

    WRAPPER_LOG("DD4_CreateSurface: caps=0x%08X size=%dx%d bpp=%d flags=0x%08X",
                caps, width, height, bpp, desc->dwFlags);

    /* ----- Primary surface with flip chain ----- */
    if (caps & DDSCAPS_PRIMARYSURFACE) {
        /* Use backend dimensions (must be set by SetCooperativeLevel or SetDisplayMode) */
        width  = g_backend.width;
        height = g_backend.height;
        if (width == 0 || height == 0) {
            WRAPPER_LOG("DD4_CreateSurface: ERROR - backend dimensions not set!");
            return DDERR_GENERIC;
        }

        WRAPPER_LOG("DD4_CreateSurface: creating PRIMARY %dx%d", width, height);

        primary_surf = WrapperSurface_Create(width, height, bpp, caps);
        if (!primary_surf) return E_OUTOFMEMORY;

        /* Check for flip chain (complex + flip + back buffer count) */
        if ((caps & DDSCAPS_COMPLEX) && (caps & DDSCAPS_FLIP) &&
            (desc->dwFlags & DDSD_BACKBUFFERCOUNT) && desc->dwBackBufferCount > 0) {

            WRAPPER_LOG("DD4_CreateSurface: creating BACKBUFFER for flip chain (count=%d)",
                        desc->dwBackBufferCount);

            back_surf = WrapperSurface_Create(width, height, bpp,
                DDSCAPS_BACKBUFFER | DDSCAPS_VIDEOMEMORY | DDSCAPS_3DDEVICE);
            if (!back_surf) {
                /* Clean up primary on failure */
                HeapFree(GetProcessHeap(), 0, primary_surf);
                return E_OUTOFMEMORY;
            }

            primary_surf->attached_back = back_surf;
        }

        /* Store in global backend */
        g_backend.primary    = primary_surf;
        g_backend.backbuffer = back_surf;

        *surf = primary_surf;

        WRAPPER_LOG("DD4_CreateSurface: PRIMARY=%p BACK=%p", primary_surf, back_surf);
        return DD_OK;
    }

    /* ----- Z-buffer surface ----- */
    if (caps & DDSCAPS_ZBUFFER) {
        DWORD zbpp = 16;
        if (desc->dwFlags & DDSD_ZBUFFERBITDEPTH)
            zbpp = desc->dwZBufferBitDepth;
        if (desc->dwFlags & DDSD_PIXELFORMAT)
            zbpp = desc->ddpfPixelFormat.dwZBufferBitDepth;

        /* Override Z-buffer to match RT dimensions (native res) */
        if (g_backend.width > 0 && g_backend.height > 0) {
            width = (DWORD)g_backend.width;
            height = (DWORD)g_backend.height;
        }

        WRAPPER_LOG("DD4_CreateSurface: creating ZBUFFER %dx%d %d-bit", width, height, zbpp);

        WrapperSurface *zsurf = WrapperSurface_Create(width, height, zbpp, caps);
        if (!zsurf) return E_OUTOFMEMORY;

        g_backend.zbuffer = zsurf;
        *surf = zsurf;
        return DD_OK;
    }

    /* ----- Texture surface ----- */
    if (caps & DDSCAPS_TEXTURE) {
        WRAPPER_LOG("DD4_CreateSurface: creating TEXTURE %dx%d bpp=%d", width, height, bpp);

        /* Copy pixel format BEFORE creating the surface so that
         * WrapperSurface_Create picks the correct DXGI format
         * (e.g. B5G5R5A1 for textures with alpha). */
        WrapperSurface *tex = WrapperSurface_Create(width, height, bpp, caps);
        if (!tex) return E_OUTOFMEMORY;

        if (desc->dwFlags & DDSD_PIXELFORMAT) {
            tex->pixel_format = desc->ddpfPixelFormat;
        }

        /* If ALPHAPIXELS is set, recreate as B5G5R5A1.
         * This gives tree/fence cutout transparency via 1-bit alpha.
         * B5G6R5 textures (street lights, road) keep their format;
         * the pixel shader handles their transparency at render time. */
        if (bpp == 16 && (desc->dwFlags & DDSD_PIXELFORMAT) &&
            (desc->ddpfPixelFormat.dwFlags & DDPF_ALPHAPIXELS) &&
            tex->d3d11_texture && g_backend.device) {
            /* All textures are now B8G8R8A8_UNORM — no format recreation needed.
             * The 16-bit A1R5G5B5 source data will be converted to 32-bit in
             * Texture_Load's upload path. Just ensure the texture exists. */
            (void)0;
        }

        /* For SYSTEMMEMORY textures (scratch surfaces), pre-allocate sys_buffer
         * and expose it via the descriptor. M2DX writes decoded pixel data directly
         * to lpSurface WITHOUT calling Lock — it expects the pointer to be valid
         * immediately after CreateSurface. */
        if (caps & DDSCAPS_SYSTEMMEMORY) {
            WrapperSurface_EnsureSysBuffer(tex);
            desc->lpSurface = tex->sys_buffer;
            desc->lPitch    = tex->pitch;
            desc->dwFlags  |= DDSD_LPSURFACE | DDSD_PITCH;
            WRAPPER_LOG("DD4_CreateSurface: SYSMEM texture %p lpSurface=%p pitch=%d",
                tex, tex->sys_buffer, tex->pitch);
        }

        *surf = tex;
        return DD_OK;
    }

    /* ----- Offscreen plain surface ----- */
    if (caps & DDSCAPS_OFFSCREENPLAIN) {
        /* Override 3DDEVICE (render target) dimensions to native resolution.
         * M2DX creates the backbuffer at its own internal resolution (640x480),
         * but we want the RT at native res so the game renders at full quality.
         * Also override the matching ZBUFFER that follows. */
        if ((caps & DDSCAPS_3DDEVICE) && g_backend.width > 0 && g_backend.height > 0) {
            /* M2DX requested this size as its internal resolution. Compute vertex
             * scale factors to transform game-space coords to our native-res RT. */
            g_backend.m2dx_width = (int)width;
            g_backend.m2dx_height = (int)height;
            if (width > 0 && height > 0) {
                g_backend.vertex_scale_x = (float)g_backend.width / (float)width;
                g_backend.vertex_scale_y = (float)g_backend.height / (float)height;
            }
            WRAPPER_LOG("DD4_CreateSurface: overriding 3DDEVICE from %dx%d to NATIVE %dx%d (vtx scale %.3fx%.3f)",
                        width, height, g_backend.width, g_backend.height,
                        g_backend.vertex_scale_x, g_backend.vertex_scale_y);
            width = (DWORD)g_backend.width;
            height = (DWORD)g_backend.height;
        }

        WRAPPER_LOG("DD4_CreateSurface: creating OFFSCREEN %dx%d bpp=%d", width, height, bpp);

        {
            WrapperSurface *off = WrapperSurface_Create(width, height, bpp, caps);
        if (!off) return E_OUTOFMEMORY;

        if (desc->dwFlags & DDSD_PIXELFORMAT) {
            off->pixel_format = desc->ddpfPixelFormat;
        }

        /* Store 3DDEVICE surface as the render target / backbuffer */
        if (caps & DDSCAPS_3DDEVICE) {
            g_backend.backbuffer = off;
            WRAPPER_LOG("DD4_CreateSurface: stored as g_backend.backbuffer %dx%d (vtx scale %.2fx%.2f)",
                        width, height, g_backend.vertex_scale_x, g_backend.vertex_scale_y);

            /* Patch the game's InitializeRaceVideoConfiguration instruction immediates
             * so when the game calls it, it writes native res instead of 640x480.
             * Addresses match td5_mod.c's ApplyWidescreenPatches (proven correct). */
            {
                DWORD old_protect;
                int nw = g_backend.width, nh = g_backend.height;
                float fw = (float)nw, fh = (float)nh;

                /* PUSH height immediate at 0x42A999 */
                if (VirtualProtect((void*)0x42A999, 4, PAGE_EXECUTE_READWRITE, &old_protect)) {
                    *(int*)0x42A999 = nh;
                    VirtualProtect((void*)0x42A999, 4, old_protect, &old_protect);
                }
                /* PUSH width immediate at 0x42A99E */
                if (VirtualProtect((void*)0x42A99E, 4, PAGE_EXECUTE_READWRITE, &old_protect)) {
                    *(int*)0x42A99E = nw;
                    VirtualProtect((void*)0x42A99E, 4, old_protect, &old_protect);
                }
                /* MOV [gRenderWidthF] float immediate at 0x42A9AD */
                if (VirtualProtect((void*)0x42A9AD, 4, PAGE_EXECUTE_READWRITE, &old_protect)) {
                    *(float*)0x42A9AD = fw;
                    VirtualProtect((void*)0x42A9AD, 4, old_protect, &old_protect);
                }
                /* MOV [gRenderHeightF] float immediate at 0x42A9B7 */
                if (VirtualProtect((void*)0x42A9B7, 4, PAGE_EXECUTE_READWRITE, &old_protect)) {
                    *(float*)0x42A9B7 = fh;
                    VirtualProtect((void*)0x42A9B7, 4, old_protect, &old_protect);
                }

                /* Patch FOV: hor+ widescreen (height*0.75 instead of width*0.5625) */
                if (VirtualProtect((void*)0x43E828, 8, PAGE_EXECUTE_READWRITE, &old_protect)) {
                    *(BYTE*)0x43E828 = 0x10;
                    *(BYTE*)0x43E82C = 0x80;
                    VirtualProtect((void*)0x43E828, 8, old_protect, &old_protect);
                }

                WRAPPER_LOG("DD4_CreateSurface: patched InitRaceVideoConfig + FOV to %dx%d", nw, nh);

                /* Pre-configure projection and viewport layout for native resolution
                 * so the frustum culling planes are correct from the very first frame.
                 * Without this, the first few frames would use 640x480 frustum planes
                 * while vertices are already being scaled to native res, causing
                 * aggressive edge culling on track geometry. */
                { typedef void (__cdecl *fn_Proj)(int, int); ((fn_Proj)0x43E7E0)(nw, nh); }
                { typedef void (__cdecl *fn_Void)(void); ((fn_Void)0x42C2B0)(); }
            }
        }

        /* For SYSTEMMEMORY offscreen surfaces, pre-allocate sys_buffer.
         * M2DX writes directly to lpSurface without Lock. */
        if (caps & DDSCAPS_SYSTEMMEMORY) {
            WrapperSurface_EnsureSysBuffer(off);
            desc->lpSurface = off->sys_buffer;
            desc->lPitch    = off->pitch;
            desc->dwFlags  |= DDSD_LPSURFACE | DDSD_PITCH;
        }

        *surf = off;
        return DD_OK;
        } /* end fe_scale block */
    }

    /* ----- Fallback: generic surface ----- */
    WRAPPER_LOG("DD4_CreateSurface: creating GENERIC surface caps=0x%08X %dx%d", caps, width, height);
    {
        WrapperSurface *gen = WrapperSurface_Create(width, height, bpp, caps);
        if (!gen) return E_OUTOFMEMORY;
        /* SYSMEM generic surfaces also need lpSurface */
        if (caps & DDSCAPS_SYSTEMMEMORY) {
            WrapperSurface_EnsureSysBuffer(gen);
            desc->lpSurface = gen->sys_buffer;
            desc->lPitch    = gen->pitch;
            desc->dwFlags  |= DDSD_LPSURFACE | DDSD_PITCH;
        }
        *surf = gen;
        return DD_OK;
    }
}

/* 7: DuplicateSurface */
static HRESULT __stdcall DD4_DuplicateSurface(WrapperDirectDraw *self, void *src, void **dst)
{
    (void)self;
    (void)src;
    if (dst) *dst = NULL;
    WRAPPER_STUB("DD4_DuplicateSurface");
}

/* 8: EnumDisplayModes */
static HRESULT __stdcall DD4_EnumDisplayModes(WrapperDirectDraw *self, DWORD flags, DDSURFACEDESC2_W *filter, void *ctx, LPDDENUMMODESCALLBACK2 callback)
{
    int i;
    DDSURFACEDESC2_W sd;

    (void)self;
    (void)flags;
    (void)filter;

    WRAPPER_LOG("DD4_EnumDisplayModes: %d modes available", g_backend.mode_count);

    if (!callback) return E_POINTER;

    /* Report all modes as 16bpp R5G6B5 to M2DX. The game only accepts 16-bit
     * modes, but modern GPUs only offer 32-bit modes. Our wrapper handles the
     * format translation internally (R5G6B5 surfaces, X8R8G8B8 swap chain).
     * Deduplicate: only report the highest refresh rate per unique WxH. */
    for (i = 0; i < g_backend.mode_count; i++) {
        DXGI_MODE_DESC *m = &g_backend.modes[i];
        int duplicate = 0;

        /* Skip duplicate resolutions (keep first = highest refresh from DXGI sort) */
        {
            int j;
            for (j = 0; j < i; j++) {
                if (g_backend.modes[j].Width == m->Width &&
                    g_backend.modes[j].Height == m->Height &&
                    g_backend.modes[j].Format == m->Format) {
                    duplicate = 1;
                    break;
                }
            }
        }
        if (duplicate) continue;

        memset(&sd, 0, sizeof(sd));
        sd.dwSize   = sizeof(sd);
        sd.dwFlags  = DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PITCH | DDSD_PIXELFORMAT;
        sd.dwWidth  = m->Width;
        sd.dwHeight = m->Height;

        /* Always report as 16bpp R5G6B5 — M2DX only accepts 16-bit modes */
        sd.ddpfPixelFormat.dwSize          = sizeof(DDPIXELFORMAT_W);
        sd.ddpfPixelFormat.dwFlags         = DDPF_RGB;
        sd.ddpfPixelFormat.dwRGBBitCount   = 16;
        sd.ddpfPixelFormat.dwRBitMask      = 0xF800;
        sd.ddpfPixelFormat.dwGBitMask      = 0x07E0;
        sd.ddpfPixelFormat.dwBBitMask      = 0x001F;
        sd.lPitch = m->Width * 2;

        sd.ddpfPixelFormat.dwRGBAlphaBitMask = 0;
        sd.ddsCaps.dwCaps = DDSCAPS_VIDEOMEMORY;
        sd.dwRefreshRate  = m->RefreshRate.Numerator /
                            (m->RefreshRate.Denominator ? m->RefreshRate.Denominator : 1);

        if (callback(&sd, ctx) == DDENUMRET_CANCEL)
            break;
    }

    return DD_OK;
}

/* 9: EnumSurfaces */
static HRESULT __stdcall DD4_EnumSurfaces(WrapperDirectDraw *self, DWORD flags, DDSURFACEDESC2_W *desc, void *ctx, void *callback)
{
    (void)self; (void)flags; (void)desc; (void)ctx; (void)callback;
    WRAPPER_STUB("DD4_EnumSurfaces");
}

/* 10: FlipToGDISurface */
static HRESULT __stdcall DD4_FlipToGDISurface(WrapperDirectDraw *self)
{
    (void)self;
    WRAPPER_STUB("DD4_FlipToGDISurface");
}

/* 11: GetCaps */
static HRESULT __stdcall DD4_GetCaps(WrapperDirectDraw *self, void *driver_caps, void *hel_caps)
{
    /*
     * DDCAPS structure is 380 bytes (dwSize at offset 0).
     * We fill basic capability flags to satisfy M2DX.
     *
     * Offsets of key fields in DDCAPS:
     *   0x00: dwSize
     *   0x04: dwCaps       (general caps)
     *   0x08: dwCaps2
     *   0x0C: dwCKeyCaps   (color key caps)
     *   0x10: dwFXCaps     (fx caps)
     *   0x14: dwFXAlphaCaps
     *   0x18: dwPalCaps
     *   0x1C: dwSVCaps
     *   ...
     *   0x50: dwVidMemTotal
     *   0x54: dwVidMemFree
     *   ...
     *   0x94: ddsCaps.dwCaps (DDSCAPS)
     */

    (void)self;
    WRAPPER_LOG("DD4_GetCaps");

    /* DDCAPS_DX7 size = 380 bytes */
    #define DDCAPS_SIZE 380

    /* General caps flags */
    #define DDCAPS_BLT           0x00000040
    #define DDCAPS_BLTCOLORFILL  0x04000000
    #define DDCAPS_BLTSTRETCH    0x00000200
    #define DDCAPS_COLORKEY      0x00000400
    #define DDCAPS_3D            0x00000001

    /* Color key caps */
    #define DDCKEYCAPS_SRCBLT    0x00000800

    if (driver_caps) {
        DWORD *dc = (DWORD*)driver_caps;
        memset(dc, 0, DDCAPS_SIZE);
        dc[0] = DDCAPS_SIZE;                       /* dwSize */
        dc[1] = DDCAPS_BLT | DDCAPS_BLTCOLORFILL | /* dwCaps */
                DDCAPS_BLTSTRETCH | DDCAPS_COLORKEY | DDCAPS_3D;
        dc[3] = DDCKEYCAPS_SRCBLT;                 /* dwCKeyCaps */

        /* Video memory — hardcoded 256MB, game only uses this for texture budget */
        dc[0x50/4] = 256 * 1024 * 1024;  /* dwVidMemTotal */
        dc[0x54/4] = 256 * 1024 * 1024;  /* dwVidMemFree */

        /* Surface caps */
        dc[0x94/4] = DDSCAPS_PRIMARYSURFACE | DDSCAPS_OFFSCREENPLAIN |
                     DDSCAPS_TEXTURE | DDSCAPS_ZBUFFER | DDSCAPS_3DDEVICE |
                     DDSCAPS_VIDEOMEMORY | DDSCAPS_FLIP | DDSCAPS_COMPLEX;
    }

    if (hel_caps) {
        /* HEL (hardware emulation layer) - same as driver for us */
        DWORD *hc = (DWORD*)hel_caps;
        memset(hc, 0, DDCAPS_SIZE);
        hc[0] = DDCAPS_SIZE;
        hc[1] = DDCAPS_BLT | DDCAPS_BLTCOLORFILL | DDCAPS_BLTSTRETCH | DDCAPS_COLORKEY;
    }

    return DD_OK;
}

/* 12: GetDisplayMode */
static HRESULT __stdcall DD4_GetDisplayMode(WrapperDirectDraw *self, DDSURFACEDESC2_W *desc)
{
    (void)self;
    WRAPPER_LOG("DD4_GetDisplayMode");

    if (!desc) return E_POINTER;

    memset(desc, 0, sizeof(*desc));
    desc->dwSize   = sizeof(DDSURFACEDESC2_W);
    desc->dwFlags  = DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PITCH | DDSD_PIXELFORMAT;
    desc->dwWidth  = g_backend.width;
    desc->dwHeight = g_backend.height;
    if (desc->dwWidth == 0) {
        desc->dwWidth  = GetSystemMetrics(SM_CXSCREEN);
        desc->dwHeight = GetSystemMetrics(SM_CYSCREEN);
        WRAPPER_LOG("DD4_GetDisplayMode: fallback to monitor res %dx%d", desc->dwWidth, desc->dwHeight);
    }

    desc->ddpfPixelFormat.dwSize       = sizeof(DDPIXELFORMAT_W);
    desc->ddpfPixelFormat.dwFlags      = DDPF_RGB;

    /* Always report 16bpp R5G6B5 — the game only works in 16-bit mode.
     * Our wrapper handles the 16→32 format translation internally. */
    desc->ddpfPixelFormat.dwRGBBitCount   = 16;
    desc->ddpfPixelFormat.dwRBitMask       = 0xF800;
    desc->ddpfPixelFormat.dwGBitMask       = 0x07E0;
    desc->ddpfPixelFormat.dwBBitMask       = 0x001F;
    desc->lPitch = desc->dwWidth * 2;

    desc->ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE | DDSCAPS_VIDEOMEMORY;

    return DD_OK;
}

/* 13: GetFourCCCodes */
static HRESULT __stdcall DD4_GetFourCCCodes(WrapperDirectDraw *self, DWORD *num_codes, DWORD *codes)
{
    (void)self; (void)codes;
    WRAPPER_LOG("DD4_GetFourCCCodes (stub)");
    if (num_codes) *num_codes = 0;
    return DD_OK;
}

/* 14: GetGDISurface */
static HRESULT __stdcall DD4_GetGDISurface(WrapperDirectDraw *self, void **surface)
{
    (void)self;
    if (surface) *surface = NULL;
    WRAPPER_STUB("DD4_GetGDISurface");
}

/* 15: GetMonitorFrequency */
static HRESULT __stdcall DD4_GetMonitorFrequency(WrapperDirectDraw *self, DWORD *freq)
{
    (void)self;
    WRAPPER_LOG("DD4_GetMonitorFrequency");
    if (!freq) return E_POINTER;
    *freq = 60;
    return DD_OK;
}

/* 16: GetScanLine */
static HRESULT __stdcall DD4_GetScanLine(WrapperDirectDraw *self, DWORD *scanline)
{
    (void)self;
    WRAPPER_LOG("DD4_GetScanLine (stub)");
    if (scanline) *scanline = 0;
    return DD_OK;
}

/* 17: GetVerticalBlankStatus */
static HRESULT __stdcall DD4_GetVerticalBlankStatus(WrapperDirectDraw *self, BOOL *is_vb)
{
    (void)self;
    if (!is_vb) return E_POINTER;
    *is_vb = TRUE;
    return DD_OK;
}

/* 18: Initialize */
static HRESULT __stdcall DD4_Initialize(WrapperDirectDraw *self, GUID *guid)
{
    (void)self; (void)guid;
    WRAPPER_STUB("DD4_Initialize");
}

/* 19: RestoreDisplayMode */
static HRESULT __stdcall DD4_RestoreDisplayMode(WrapperDirectDraw *self)
{
    (void)self;
    WRAPPER_LOG("DD4_RestoreDisplayMode (D3D11 manages this)");
    return DD_OK;
}

/* 20: SetCooperativeLevel */
static HRESULT __stdcall DD4_SetCooperativeLevel(WrapperDirectDraw *self, HWND hwnd, DWORD flags)
{
    (void)self;
    WRAPPER_LOG("DD4_SetCooperativeLevel: hwnd=%p flags=0x%08X", hwnd, flags);

    g_backend.hwnd = hwnd;
    g_backend.coop_flags = flags;

    /* Always force windowed mode — D3D11 wrapper handles presentation.
     * Game may request fullscreen but we override to windowed. */
    g_backend.windowed = 1;
    WRAPPER_LOG("DD4_SetCooperativeLevel: forced WINDOWED (game flags=0x%08X)", flags);

    /*
     * If this is a windowed mode call and the D3D11 device hasn't been
     * created yet, create it now. For fullscreen, we defer creation
     * until we know the resolution (SetDisplayMode or first CreateSurface).
     */
    /* Create D3D11 device. Read target resolution from INI [Windowed] section.
     * Falls back to monitor native resolution if Width/Height are 0. */
    if (!g_backend.device) {
        int init_w, init_h;
        char ini_path[MAX_PATH];
        GetModuleFileNameA(NULL, ini_path, MAX_PATH);
        { char *s = strrchr(ini_path, '\\'); if (s) strcpy(s + 1, "td5re.ini"); }

        init_w = GetPrivateProfileIntA("Display", "Width", 0, ini_path);
        init_h = GetPrivateProfileIntA("Display", "Height", 0, ini_path);
        if (init_w <= 0 || init_h <= 0) {
            init_w = GetSystemMetrics(SM_CXSCREEN);
            init_h = GetSystemMetrics(SM_CYSCREEN);
            if (init_w <= 0) init_w = 800;
            if (init_h <= 0) init_h = 600;
        }
        WRAPPER_LOG("DD4_SetCooperativeLevel: creating D3D11 device at %dx%d", init_w, init_h);
        Backend_CreateDevice(hwnd, init_w, init_h, 16, 1);
    }

    return DD_OK;
}

/* 21: SetDisplayMode */
static HRESULT __stdcall DD4_SetDisplayMode(WrapperDirectDraw *self, DWORD w, DWORD h, DWORD bpp, DWORD refresh, DWORD flags)
{
    (void)self; (void)refresh; (void)flags;
    WRAPPER_LOG("DD4_SetDisplayMode: M2DX requested %dx%d %dbpp, native %dx%d",
                w, h, bpp, g_backend.target_width, g_backend.target_height);

    /* Store M2DX's internal resolution (what the game thinks it's running at).
     * The game's vertex transforms, viewport, and gRenderWidth/Height will use
     * these values. We can't change them — M2DX stores them internally. */
    g_backend.m2dx_width = (int)w;
    g_backend.m2dx_height = (int)h;

    /* Our RT is at native resolution. Compute vertex scale factors to transform
     * game-space coordinates (640x480) to native-space (2048x1152). */
    g_backend.width = g_backend.target_width;
    g_backend.height = g_backend.target_height;
    g_backend.bpp = 16;

    if (w > 0 && h > 0) {
        g_backend.vertex_scale_x = (float)g_backend.target_width / (float)w;
        g_backend.vertex_scale_y = (float)g_backend.target_height / (float)h;
    } else {
        g_backend.vertex_scale_x = 1.0f;
        g_backend.vertex_scale_y = 1.0f;
    }

    WRAPPER_LOG("DD4_SetDisplayMode: vertex scale %.3fx%.3f (%dx%d -> %dx%d)",
                g_backend.vertex_scale_x, g_backend.vertex_scale_y,
                g_backend.m2dx_width, g_backend.m2dx_height,
                g_backend.target_width, g_backend.target_height);

    /* Frontend scaling: game sees virtual (640x480), wrapper renders at native */
    g_backend.fe_scale.virtual_w = (int)w;   /* game's resolution */
    g_backend.fe_scale.virtual_h = (int)h;
    g_backend.fe_scale.native_w  = g_backend.target_width;
    g_backend.fe_scale.native_h  = g_backend.target_height;
    if (g_backend.target_width != (int)w || g_backend.target_height != (int)h) {
        g_backend.fe_scale.scale_x = (float)g_backend.target_width / (float)w;
        g_backend.fe_scale.scale_y = (float)g_backend.target_height / (float)h;
        g_backend.fe_scale.enabled = 1;
        WRAPPER_LOG("DD4_SetDisplayMode: FrontendScale ENABLED %.2fx%.2f (%dx%d -> %dx%d)",
                    g_backend.fe_scale.scale_x, g_backend.fe_scale.scale_y,
                    (int)w, (int)h, g_backend.target_width, g_backend.target_height);
    } else {
        g_backend.fe_scale.scale_x = 1.0f;
        g_backend.fe_scale.scale_y = 1.0f;
        g_backend.fe_scale.enabled = 0;
    }

    return DD_OK;
}

/* 22: WaitForVerticalBlank */
static HRESULT __stdcall DD4_WaitForVerticalBlank(WrapperDirectDraw *self, DWORD flags, HANDLE event)
{
    (void)self; (void)flags; (void)event;
    Sleep(1);
    return DD_OK;
}

/* ========================================================================
 * IDirectDraw2 methods (slot 22)
 * ======================================================================== */

/* 22: GetAvailableVidMem */
static HRESULT __stdcall DD4_GetAvailableVidMem(WrapperDirectDraw *self, DDSCAPS2_W *caps, DWORD *total, DWORD *free_mem)
{
    DWORD avail = 256 * 1024 * 1024; /* 256MB — game only uses this for texture budget */

    (void)self;
    (void)caps;
    WRAPPER_LOG("DD4_GetAvailableVidMem");

    if (total)    *total    = avail;
    if (free_mem) *free_mem = avail;

    return DD_OK;
}

/* ========================================================================
 * IDirectDraw4 methods (slots 23-26)
 * ======================================================================== */

/* 23: GetSurfaceFromDC */
static HRESULT __stdcall DD4_GetSurfaceFromDC(WrapperDirectDraw *self, HDC dc, void **surface)
{
    (void)self; (void)dc;
    if (surface) *surface = NULL;
    WRAPPER_STUB("DD4_GetSurfaceFromDC");
}

/* 24: RestoreAllSurfaces */
static HRESULT __stdcall DD4_RestoreAllSurfaces(WrapperDirectDraw *self)
{
    (void)self;
    WRAPPER_LOG("DD4_RestoreAllSurfaces (no-op, D3D11 manages resources)");
    return DD_OK;
}

/* 25: TestCooperativeLevel */
static HRESULT __stdcall DD4_TestCooperativeLevel(WrapperDirectDraw *self)
{
    (void)self;
    return DD_OK;
}

/* 26: GetDeviceIdentifier */
static HRESULT __stdcall DD4_GetDeviceIdentifier(WrapperDirectDraw *self, void *ident, DWORD flags)
{
    /*
     * DDDEVICEIDENTIFIER2 layout (relevant fields):
     *   char szDriver[512]       +0x000
     *   char szDescription[512]  +0x200
     *   ...
     *   GUID guidDeviceIdentifier +0x478 (total size ~1072 bytes)
     *
     * We fill in minimal data -- M2DX only reads description for logging.
     */

    (void)self;
    (void)flags;
    WRAPPER_LOG("DD4_GetDeviceIdentifier");

    if (!ident) return E_POINTER;

    memset(ident, 0, 1072); /* DDDEVICEIDENTIFIER2 size */

    /* szDriver at offset 0 */
    lstrcpynA((char*)ident, "d3d11wrapper.dll", 512);

    /* szDescription at offset 0x200 */
    lstrcpynA((char*)ident + 0x200, "D3D11 Wrapper for DirectDraw", 512);

    return DD_OK;
}
