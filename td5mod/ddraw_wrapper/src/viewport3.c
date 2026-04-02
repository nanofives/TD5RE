/**
 * viewport3.c - IDirect3DViewport3 COM implementation
 *
 * Translates D3D6 viewport calls to D3D11. The game uses viewports to define
 * the render area (full screen, split-screen halves) and to clear the frame.
 *
 * Vtable layout: IDirect3DViewport → IDirect3DViewport2 → IDirect3DViewport3
 * 21 methods total (3 IUnknown + 12 V1 + 3 V2 + 3 V3)
 */

#include "wrapper.h"

/* ========================================================================
 * Vtable type
 * ======================================================================== */

typedef struct WrapperViewportVtbl WrapperViewportVtbl;

struct WrapperViewportVtbl {
    /* IUnknown */
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(WrapperViewport *self, REFIID riid, void **ppv);
    ULONG   (STDMETHODCALLTYPE *AddRef)(WrapperViewport *self);
    ULONG   (STDMETHODCALLTYPE *Release)(WrapperViewport *self);
    /* IDirect3DViewport */
    HRESULT (STDMETHODCALLTYPE *Initialize)(WrapperViewport *self, void *d3d);
    HRESULT (STDMETHODCALLTYPE *GetViewport)(WrapperViewport *self, void *vp);
    HRESULT (STDMETHODCALLTYPE *SetViewport)(WrapperViewport *self, void *vp);
    HRESULT (STDMETHODCALLTYPE *TransformVertices)(WrapperViewport *self, DWORD count, void *data, DWORD flags, DWORD *offscreen);
    HRESULT (STDMETHODCALLTYPE *LightElements)(WrapperViewport *self, DWORD count, void *data);
    HRESULT (STDMETHODCALLTYPE *SetBackground)(WrapperViewport *self, DWORD hMat);
    HRESULT (STDMETHODCALLTYPE *GetBackground)(WrapperViewport *self, DWORD *hMat, BOOL *valid);
    HRESULT (STDMETHODCALLTYPE *SetBackgroundDepth)(WrapperViewport *self, void *surface);
    HRESULT (STDMETHODCALLTYPE *GetBackgroundDepth)(WrapperViewport *self, void **surface, BOOL *valid);
    HRESULT (STDMETHODCALLTYPE *Clear)(WrapperViewport *self, DWORD count, RECT *rects, DWORD flags);
    HRESULT (STDMETHODCALLTYPE *AddLight)(WrapperViewport *self, void *light);
    HRESULT (STDMETHODCALLTYPE *DeleteLight)(WrapperViewport *self, void *light);
    HRESULT (STDMETHODCALLTYPE *NextLight)(WrapperViewport *self, void *light, void **next, DWORD flags);
    /* IDirect3DViewport2 */
    HRESULT (STDMETHODCALLTYPE *GetViewport2)(WrapperViewport *self, D3DVIEWPORT2_W *vp);
    HRESULT (STDMETHODCALLTYPE *SetViewport2)(WrapperViewport *self, D3DVIEWPORT2_W *vp);
    /* IDirect3DViewport3 */
    HRESULT (STDMETHODCALLTYPE *SetBackgroundDepth2)(WrapperViewport *self, void *surface);
    HRESULT (STDMETHODCALLTYPE *GetBackgroundDepth2)(WrapperViewport *self, void **surface, BOOL *valid);
    HRESULT (STDMETHODCALLTYPE *Clear2)(WrapperViewport *self, DWORD count, void *rects, DWORD flags, DWORD color, float z, DWORD stencil);
};

/* ========================================================================
 * IUnknown
 * ======================================================================== */

static HRESULT STDMETHODCALLTYPE Viewport_QueryInterface(WrapperViewport *self, REFIID riid, void **ppv)
{
    WRAPPER_LOG("Viewport::QueryInterface");
    if (!ppv) return E_POINTER;

    if (IsEqualGUID(riid, &IID_IUnknown) ||
        IsEqualGUID(riid, &IID_IDirect3DViewport3)) {
        *ppv = self;
        self->vtbl->AddRef(self);
        return S_OK;
    }

    *ppv = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE Viewport_AddRef(WrapperViewport *self)
{
    LONG ref = InterlockedIncrement(&self->ref_count);
    WRAPPER_LOG("Viewport::AddRef -> %ld", ref);
    return (ULONG)ref;
}

static ULONG STDMETHODCALLTYPE Viewport_Release(WrapperViewport *self)
{
    LONG ref = InterlockedDecrement(&self->ref_count);
    WRAPPER_LOG("Viewport::Release -> %ld", ref);
    if (ref <= 0) {
        /* Static storage — don't free, just reset ref to 1 */
        self->ref_count = 1;
        return 0;
    }
    return (ULONG)ref;
}

/* ========================================================================
 * IDirect3DViewport stubs (V1)
 * ======================================================================== */

static HRESULT STDMETHODCALLTYPE Viewport_Initialize(WrapperViewport *self, void *d3d)
{
    (void)self; (void)d3d;
    WRAPPER_STUB("Viewport::Initialize");
}

static HRESULT STDMETHODCALLTYPE Viewport_GetViewport(WrapperViewport *self, void *vp)
{
    (void)self; (void)vp;
    WRAPPER_STUB("Viewport::GetViewport (V1)");
}

static HRESULT STDMETHODCALLTYPE Viewport_SetViewport(WrapperViewport *self, void *vp)
{
    (void)self; (void)vp;
    WRAPPER_STUB("Viewport::SetViewport (V1)");
}

static HRESULT STDMETHODCALLTYPE Viewport_TransformVertices(WrapperViewport *self, DWORD count, void *data, DWORD flags, DWORD *offscreen)
{
    (void)self; (void)count; (void)data; (void)flags; (void)offscreen;
    WRAPPER_STUB("Viewport::TransformVertices");
}

static HRESULT STDMETHODCALLTYPE Viewport_LightElements(WrapperViewport *self, DWORD count, void *data)
{
    (void)self; (void)count; (void)data;
    WRAPPER_STUB("Viewport::LightElements");
}

static HRESULT STDMETHODCALLTYPE Viewport_SetBackground(WrapperViewport *self, DWORD hMat)
{
    (void)self; (void)hMat;
    WRAPPER_STUB("Viewport::SetBackground");
}

static HRESULT STDMETHODCALLTYPE Viewport_GetBackground(WrapperViewport *self, DWORD *hMat, BOOL *valid)
{
    (void)self; (void)hMat; (void)valid;
    WRAPPER_STUB("Viewport::GetBackground");
}

static HRESULT STDMETHODCALLTYPE Viewport_SetBackgroundDepth(WrapperViewport *self, void *surface)
{
    (void)self; (void)surface;
    WRAPPER_STUB("Viewport::SetBackgroundDepth");
}

static HRESULT STDMETHODCALLTYPE Viewport_GetBackgroundDepth(WrapperViewport *self, void **surface, BOOL *valid)
{
    (void)self; (void)surface; (void)valid;
    WRAPPER_STUB("Viewport::GetBackgroundDepth");
}

/* ========================================================================
 * IDirect3DViewport::Clear (V1) - delegates to Clear2
 * ======================================================================== */

static HRESULT STDMETHODCALLTYPE Viewport_Clear(WrapperViewport *self, DWORD count, RECT *rects, DWORD flags)
{
    WRAPPER_LOG("Viewport::Clear (V1): count=%lu flags=0x%08lX -> delegating to Clear2", count, flags);
    return self->vtbl->Clear2(self, count, (void*)rects, flags, 0x00000000, 1.0f, 0);
}

/* ========================================================================
 * IDirect3DViewport light stubs
 * ======================================================================== */

static HRESULT STDMETHODCALLTYPE Viewport_AddLight(WrapperViewport *self, void *light)
{
    (void)self; (void)light;
    WRAPPER_STUB("Viewport::AddLight");
}

static HRESULT STDMETHODCALLTYPE Viewport_DeleteLight(WrapperViewport *self, void *light)
{
    (void)self; (void)light;
    WRAPPER_STUB("Viewport::DeleteLight");
}

static HRESULT STDMETHODCALLTYPE Viewport_NextLight(WrapperViewport *self, void *light, void **next, DWORD flags)
{
    (void)self; (void)light; (void)next; (void)flags;
    WRAPPER_STUB("Viewport::NextLight");
}

/* ========================================================================
 * IDirect3DViewport2 methods
 * ======================================================================== */

static HRESULT STDMETHODCALLTYPE Viewport_GetViewport2(WrapperViewport *self, D3DVIEWPORT2_W *vp)
{
    WRAPPER_LOG("Viewport::GetViewport2");
    if (!vp) return E_POINTER;
    CopyMemory(vp, &self->vp_data, sizeof(D3DVIEWPORT2_W));
    return DD_OK;
}

static HRESULT STDMETHODCALLTYPE Viewport_SetViewport2(WrapperViewport *self, D3DVIEWPORT2_W *vp)
{
    WRAPPER_LOG("Viewport::SetViewport2: X=%lu Y=%lu W=%lu H=%lu MinZ=%.3f MaxZ=%.3f",
                vp->dwX, vp->dwY, vp->dwWidth, vp->dwHeight, vp->dvMinZ, vp->dvMaxZ);

    /* Store the viewport data */
    CopyMemory(&self->vp_data, vp, sizeof(D3DVIEWPORT2_W));

    /* Apply to D3D11 device context. If the game sets a viewport matching the
     * old 640x480 internal resolution but our RT is at native res, override
     * the viewport to cover the full RT. This happens because M2DX stores
     * its own resolution internally and the game reads from there. */
    if (g_backend.context) {
        D3D11_VIEWPORT vp11;
        vp11.TopLeftX = (FLOAT)vp->dwX;
        vp11.TopLeftY = (FLOAT)vp->dwY;
        vp11.Width    = (FLOAT)vp->dwWidth;
        vp11.Height   = (FLOAT)vp->dwHeight;
        vp11.MinDepth = vp->dvMinZ;
        vp11.MaxDepth = vp->dvMaxZ;

        /* Override to RT dimensions if game passes undersized viewport */
        if (g_backend.backbuffer && vp11.Width < (FLOAT)g_backend.width) {
            WRAPPER_LOG("Viewport::SetViewport2: overriding %.0fx%.0f -> %dx%d (native RT)",
                        vp11.Width, vp11.Height, g_backend.width, g_backend.height);
            vp11.Width  = (FLOAT)g_backend.width;
            vp11.Height = (FLOAT)g_backend.height;
        }

        ID3D11DeviceContext_RSSetViewports(g_backend.context, 1, &vp11);
        Backend_UpdateViewportCB(vp11.Width, vp11.Height);
    }

    return DD_OK;
}

/* ========================================================================
 * IDirect3DViewport3 methods
 * ======================================================================== */

static HRESULT STDMETHODCALLTYPE Viewport_SetBackgroundDepth2(WrapperViewport *self, void *surface)
{
    (void)self; (void)surface;
    WRAPPER_STUB("Viewport::SetBackgroundDepth2");
}

static HRESULT STDMETHODCALLTYPE Viewport_GetBackgroundDepth2(WrapperViewport *self, void **surface, BOOL *valid)
{
    (void)self; (void)surface; (void)valid;
    WRAPPER_STUB("Viewport::GetBackgroundDepth2");
}

static HRESULT STDMETHODCALLTYPE Viewport_Clear2(WrapperViewport *self, DWORD count, void *rects, DWORD flags, DWORD color, float z, DWORD stencil)
{
    (void)self;
    (void)count;
    (void)rects;
    (void)stencil;

    WRAPPER_LOG("Viewport::Clear2: count=%lu flags=0x%08lX color=0x%08lX z=%.3f stencil=%lu",
                count, flags, color, z, stencil);

    if (!g_backend.context) {
        WRAPPER_LOG("Viewport::Clear2: no device context!");
        return DDERR_GENERIC;
    }

    if (flags & 1) {  /* D3DCLEAR_TARGET */
        float clearColor[4] = {
            ((color >> 16) & 0xFF) / 255.0f,
            ((color >> 8) & 0xFF) / 255.0f,
            (color & 0xFF) / 255.0f,
            ((color >> 24) & 0xFF) / 255.0f
        };
        /* Clear the backbuffer RTV (where 3D draws go), not the swap chain.
         * BeginScene also clears the backbuffer, but M2DX calls Viewport::Clear
         * separately and the game expects the actual render target to be cleared. */
        if (g_backend.backbuffer && g_backend.backbuffer->d3d11_rtv)
            ID3D11DeviceContext_ClearRenderTargetView(g_backend.context, g_backend.backbuffer->d3d11_rtv, clearColor);
        else if (g_backend.swap_rtv)
            ID3D11DeviceContext_ClearRenderTargetView(g_backend.context, g_backend.swap_rtv, clearColor);
    }
    if ((flags & 2) && g_backend.depth_dsv) {  /* D3DCLEAR_ZBUFFER */
        ID3D11DeviceContext_ClearDepthStencilView(g_backend.context, g_backend.depth_dsv, D3D11_CLEAR_DEPTH, z, 0);
    }

    return DD_OK;
}

/* ========================================================================
 * Static vtable
 * ======================================================================== */

static WrapperViewportVtbl s_viewport_vtbl = {
    /* IUnknown */
    Viewport_QueryInterface,
    Viewport_AddRef,
    Viewport_Release,
    /* IDirect3DViewport */
    Viewport_Initialize,
    Viewport_GetViewport,
    Viewport_SetViewport,
    Viewport_TransformVertices,
    Viewport_LightElements,
    Viewport_SetBackground,
    Viewport_GetBackground,
    Viewport_SetBackgroundDepth,
    Viewport_GetBackgroundDepth,
    Viewport_Clear,
    Viewport_AddLight,
    Viewport_DeleteLight,
    Viewport_NextLight,
    /* IDirect3DViewport2 */
    Viewport_GetViewport2,
    Viewport_SetViewport2,
    /* IDirect3DViewport3 */
    Viewport_SetBackgroundDepth2,
    Viewport_GetBackgroundDepth2,
    Viewport_Clear2,
};

/* ========================================================================
 * Constructor
 * ======================================================================== */

WrapperViewport* WrapperViewport_Create(void)
{
    /* Use static storage — heap corruption from the game's texture streaming
     * can overwrite heap-allocated COM objects' vtable pointers, causing
     * CALL-to-NULL crashes. Static storage is immune to heap corruption. */
    static WrapperViewport s_viewport_storage;
    WrapperViewport *vp = &s_viewport_storage;
    ZeroMemory(vp, sizeof(WrapperViewport));

    vp->vtbl = &s_viewport_vtbl;
    vp->ref_count = 1;

    /* Default viewport: full screen */
    vp->vp_data.dwSize   = sizeof(D3DVIEWPORT2_W);
    vp->vp_data.dwX      = 0;
    vp->vp_data.dwY      = 0;
    vp->vp_data.dwWidth  = (DWORD)g_backend.width;
    vp->vp_data.dwHeight = (DWORD)g_backend.height;
    vp->vp_data.dvMinZ   = 0.0f;
    vp->vp_data.dvMaxZ   = 1.0f;
    vp->vp_data.dvClipX      = -1.0f;
    vp->vp_data.dvClipY      =  1.0f;
    vp->vp_data.dvClipWidth  =  2.0f;
    vp->vp_data.dvClipHeight =  2.0f;

    WRAPPER_LOG("WrapperViewport_Create: %p (%lux%lu)", vp, vp->vp_data.dwWidth, vp->vp_data.dwHeight);
    return vp;
}
