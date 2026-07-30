/**
 * texture2.c - IDirect3DTexture2 COM implementation
 *
 * Wraps texture handles for the D3D6 interface. In D3D6, textures are obtained
 * via GetHandle() which returns an opaque DWORD handle. The game passes this
 * handle back through SetRenderState(D3DRENDERSTATE_TEXTUREHANDLE, handle).
 *
 * The handle is now a plain opaque token (no pointer masquerading): the port
 * binds through the texture's opaque BackendTexture handle, not the token.
 *
 * Vtable layout: IUnknown (3) + IDirect3DTexture (4) + IDirect3DTexture2 (0)
 * IDirect3DTexture2 inherits IDirect3DTexture which inherits IUnknown.
 * Total: 7 methods.
 *
 * D3D12 port Phase 0: all GPU texture work goes through td5_backend_texture.h;
 * this file holds no ID3D11* types.
 */

#include "wrapper.h"

/* Vtable type now defined in wrapper.h */

/* ========================================================================
 * IUnknown
 * ======================================================================== */

static HRESULT STDMETHODCALLTYPE Texture_QueryInterface(WrapperTexture *self, REFIID riid, void **ppv)
{
    WRAPPER_LOG("Texture::QueryInterface");
    if (!ppv) return E_POINTER;

    if (IsEqualGUID(riid, &IID_IUnknown) ||
        IsEqualGUID(riid, &IID_IDirect3DTexture2)) {
        *ppv = self;
        self->vtbl->AddRef(self);
        return S_OK;
    }

    *ppv = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE Texture_AddRef(WrapperTexture *self)
{
    LONG ref = InterlockedIncrement(&self->ref_count);
    WRAPPER_LOG("Texture::AddRef -> %ld", ref);
    return (ULONG)ref;
}

static ULONG STDMETHODCALLTYPE Texture_Release(WrapperTexture *self)
{
    LONG ref = InterlockedDecrement(&self->ref_count);
    WRAPPER_LOG("Texture::Release -> %ld", ref);
    if (ref <= 0) {
        WRAPPER_LOG("Texture: destroying %p (surface=%p)", self, self->surface);
        /* We do NOT release the parent surface here -- the surface owns us.
         * The surface's Release will free us if needed, or the game manages
         * surface lifetime separately. Drop our ref on the shared GPU handle. */
        Backend_TextureRelease(self->bt);
        HeapFree(GetProcessHeap(), 0, self);
        return 0;
    }
    return (ULONG)ref;
}

/* ========================================================================
 * IDirect3DTexture stubs
 * ======================================================================== */

static HRESULT STDMETHODCALLTYPE Texture_Initialize(WrapperTexture *self, void *device, void *surface)
{
    (void)self; (void)device; (void)surface;
    WRAPPER_STUB("Texture::Initialize");
}

static HRESULT STDMETHODCALLTYPE Texture_PaletteChanged(WrapperTexture *self, DWORD start, DWORD count)
{
    (void)self; (void)start; (void)count;
    WRAPPER_STUB("Texture::PaletteChanged");
}

/* ========================================================================
 * IDirect3DTexture2::GetHandle - CRITICAL
 *
 * The D3D3 API defines the handle as an opaque 32-bit DWORD. No caller maps
 * the handle back to a pointer -- the port binds through the texture's opaque
 * BackendTexture handle -- so the token cannot masquerade as a pointer on any
 * arch. The lifetime side effect is load-bearing: the port calls GetHandle so
 * the GPU texture outlives its owning surface (M2DX released surfaces after
 * Load but kept rendering from the handle; see Surface4_Release).
 * ======================================================================== */

static DWORD s_next_texture_handle = 1;

static HRESULT STDMETHODCALLTYPE Texture_GetHandle(WrapperTexture *self, WrapperDevice *device, DWORD *handle)
{
    (void)device;

    WRAPPER_LOG("Texture::GetHandle: self=%p surface=%p", self, self->surface);

    if (!handle) return E_POINTER;

    if (self->surface && self->surface->bt) {
        /* Give the wrapper its own ref to the surface's handle so it survives
         * surface destruction (the game keeps drawing from the handle after
         * releasing the surface -- see the header comment above). */
        if (self->bt != self->surface->bt) {
            if (self->bt) Backend_TextureRelease(self->bt);
            self->bt = self->surface->bt;
            Backend_TextureAddRef(self->bt);
        }
        *handle = s_next_texture_handle++;
        WRAPPER_LOG("Texture::GetHandle: bt %p, opaque handle 0x%08lX",
                    (void*)self->bt, *handle);
    } else {
        *handle = 0;
        WRAPPER_LOG("Texture::GetHandle: no bt, returning 0");
    }

    return DD_OK;
}

/* ========================================================================
 * IDirect3DTexture2::Load - CRITICAL
 *
 * Copies pixel data from srcTex's surface to this texture's surface. M2DX uses
 * this to upload decoded TGA/palette data into GPU textures. All the format
 * handling (PNG override, 16->32 with A1R5G5B5/R5G6B5 + colorkey, 16->16,
 * 32 direct, GPU copy) lives in Backend_TextureLoad now; here we just marshal
 * the surfaces' CPU pixels + format flags and sync the wrapper's handle to the
 * (possibly recreated) destination.
 * ======================================================================== */

static int s_tex_total = 0;

static HRESULT STDMETHODCALLTYPE Texture_Load(WrapperTexture *self, WrapperTexture *srcTex)
{
    WrapperSurface *dst_surf, *src_surf;
    int r5;

    s_tex_total++;
    WRAPPER_LOG("Texture::Load: dst=%p src=%p (#%d)", self, srcTex, s_tex_total);

    if (!srcTex) return E_POINTER;

    dst_surf = self->surface;
    src_surf = srcTex->surface;

    if (!dst_surf || !src_surf) {
        WRAPPER_LOG("Texture::Load: NULL surface (dst=%p src=%p)", dst_surf, src_surf);
        return DDERR_GENERIC;
    }

    /* Seed r5g6b5_source with the current value; the backend only reassigns it
     * on the paths that redefine it (matching the old Texture::Load). */
    r5 = self->r5g6b5_source;

    if (Backend_TextureLoad(&dst_surf->bt, dst_surf->width, dst_surf->height,
                            dst_surf->dxgi_format,
                            src_surf->sys_buffer, src_surf->pitch,
                            src_surf->width, src_surf->height, src_surf->bpp,
                            (src_surf->pixel_format.dwFlags & DDPF_ALPHAPIXELS) ? 1 : 0,
                            src_surf->has_colorkey, src_surf->colorkey_low,
                            src_surf->bt, &r5)) {
        self->r5g6b5_source = r5;

        /* Sync the wrapper's handle to the (possibly recreated) destination
         * surface handle so SetTexture binds the texture that got the data. */
        if (self->bt != dst_surf->bt) {
            if (self->bt) Backend_TextureRelease(self->bt);
            self->bt = dst_surf->bt;
            if (self->bt) Backend_TextureAddRef(self->bt);
        }
        WRAPPER_LOG("Texture::Load: OK (%ux%u r5g6b5=%d) [total=%d]",
                    dst_surf->width, dst_surf->height, self->r5g6b5_source, s_tex_total);
        return DD_OK;
    }

    WRAPPER_LOG("Texture::Load: no viable copy path");
    return DDERR_GENERIC;
}

/* ========================================================================
 * Static vtable
 * ======================================================================== */

static WrapperTextureVtbl s_texture_vtbl = {
    Texture_QueryInterface,
    Texture_AddRef,
    Texture_Release,
    /* No Initialize — IDirect3DTexture2 removed it from v1 */
    Texture_GetHandle,
    Texture_PaletteChanged,
    Texture_Load,
};

/* ========================================================================
 * Constructor
 * ======================================================================== */

WrapperTexture* WrapperTexture_Create(WrapperSurface *surface)
{
    WrapperTexture *tex = (WrapperTexture*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(WrapperTexture));
    if (!tex) return NULL;

    tex->vtbl = &s_texture_vtbl;
    tex->ref_count = 1;
    tex->surface = surface;
    tex->r5g6b5_source = 0;  /* Set to 1 only when source is confirmed R5G6B5 */

    WRAPPER_LOG("WrapperTexture_Create: %p (surface=%p, bt=%p)",
                tex, surface, surface ? (void*)surface->bt : NULL);
    return tex;
}
