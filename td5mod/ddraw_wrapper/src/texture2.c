/**
 * texture2.c - IDirect3DTexture2 COM implementation
 *
 * Wraps texture handles for the D3D6 interface. In D3D6, textures are obtained
 * via GetHandle() which returns an opaque DWORD handle. The game passes this
 * handle back through SetRenderState(D3DRENDERSTATE_TEXTUREHANDLE, handle).
 *
 * Our implementation returns the ID3D11ShaderResourceView pointer cast to DWORD
 * as the handle, so SetTexture can recover it directly.
 *
 * Vtable layout: IUnknown (3) + IDirect3DTexture (4) + IDirect3DTexture2 (0)
 * IDirect3DTexture2 inherits IDirect3DTexture which inherits IUnknown.
 * Total: 7 methods.
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
         * surface lifetime separately. */
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
 * Returns the ID3D11ShaderResourceView pointer cast to DWORD. The game will
 * pass this handle back via SetRenderState(D3DRENDERSTATE_TEXTUREHANDLE, handle),
 * and we recover the pointer via (ID3D11ShaderResourceView*)(DWORD_PTR)handle.
 * ======================================================================== */

static HRESULT STDMETHODCALLTYPE Texture_GetHandle(WrapperTexture *self, WrapperDevice *device, DWORD *handle)
{
    (void)device;

    WRAPPER_LOG("Texture::GetHandle: self=%p surface=%p", self, self->surface);

    if (!handle) return E_POINTER;

    if (self->surface && self->surface->d3d11_srv) {
        /* AddRef the SRV so it survives surface destruction.
         * M2DX releases the DirectDraw surface after Load, but keeps the
         * texture handle for rendering. Without AddRef, the SRV
         * is freed when the surface is destroyed -> dangling handle. */
        ID3D11ShaderResourceView_AddRef(self->surface->d3d11_srv);
        *handle = (DWORD)(DWORD_PTR)self->surface->d3d11_srv;
        WRAPPER_LOG("Texture::GetHandle: returning d3d11_srv %p as handle 0x%08lX (AddRef'd)",
                    self->surface->d3d11_srv, *handle);
    } else {
        *handle = 0;
        WRAPPER_LOG("Texture::GetHandle: no d3d11_srv, returning 0");
    }

    return DD_OK;
}

/* ========================================================================
 * IDirect3DTexture2::Load - CRITICAL
 *
 * Copies pixel data from srcTex's surface to this texture's surface.
 * M2DX uses this to upload decoded TGA/palette data into GPU textures:
 *   1. Decode image into a scratch system-memory surface
 *   2. Lock scratch, write pixels, unlock
 *   3. Call Load(gpuTex, scratchTex) to upload
 *
 * D3D11 strategy:
 *   - If source has sys_buffer: use UpdateSubresource (CPU -> GPU upload)
 *   - If source has d3d11_texture: use CopyResource (GPU -> GPU copy)
 *   - No LockRect equivalent on DEFAULT textures in D3D11
 * ======================================================================== */

/* Texture loading diagnostics */
static int s_tex_total = 0;
static int s_tex_png_override = 0;
static int s_tex_16to32 = 0;
static int s_tex_16to16 = 0;
static int s_tex_srv_fail = 0;
/* s_tex_diag_frame reserved for future per-frame summary logging */

static HRESULT STDMETHODCALLTYPE Texture_Load(WrapperTexture *self, WrapperTexture *srcTex)
{
    WrapperSurface *dst_surf, *src_surf;

    s_tex_total++;
    WRAPPER_LOG("Texture::Load: dst=%p src=%p (#%d)", self, srcTex, s_tex_total);

    if (!srcTex) return E_POINTER;

    dst_surf = self->surface;
    src_surf = srcTex->surface;

    if (!dst_surf || !src_surf) {
        WRAPPER_LOG("Texture::Load: NULL surface (dst=%p src=%p)", dst_surf, src_surf);
        return DDERR_GENERIC;
    }

    /* Helper macro: after uploading texture data, sync the WrapperTexture's SRV
     * to the destination surface's SRV. M2DX reuses surface objects — the SRV
     * stored in the WrapperTexture from a previous GetHandle call may be stale
     * (pointing to a destroyed texture). This ensures SetTexture binds the
     * texture that actually has the uploaded data. */
#define SYNC_SRV_AFTER_LOAD() do { \
    if (dst_surf->d3d11_srv) { \
        if (self->d3d11_srv && self->d3d11_srv != dst_surf->d3d11_srv) \
            ID3D11ShaderResourceView_Release(self->d3d11_srv); \
        self->d3d11_srv = dst_surf->d3d11_srv; \
        ID3D11ShaderResourceView_AddRef(dst_surf->d3d11_srv); \
    } \
} while(0)

    /* --- PNG override + dump (only R5G6B5 16-bit surfaces) --- */
    if (src_surf->sys_buffer && src_surf->width > 0 && src_surf->height > 0
        && src_surf->bpp == 16) {
        /* Dump first (before override modifies anything) */
        PngOverride_DumpTexture(src_surf->width, src_surf->height,
                                src_surf->sys_buffer, src_surf->pitch, 0);

        const char *png_path = PngOverride_Lookup(
            src_surf->width, src_surf->height,
            src_surf->sys_buffer, src_surf->pitch);
        if (png_path) {
            /* Direct A8R8G8B8 load from PNG -- no R5G6B5 round-trip.
             * PngOverride_LoadToTexture returns an ID3D11ShaderResourceView*. */
            ID3D11ShaderResourceView *png_srv = PngOverride_LoadToTexture(png_path);
            if (png_srv) {
                ID3D11Resource *png_resource = NULL;

                /* Get the underlying texture from the SRV */
                ID3D11ShaderResourceView_GetResource(png_srv, &png_resource);

                /* Update the SURFACE's texture and SRV */
                if (dst_surf->d3d11_texture)
                    ID3D11Texture2D_Release(dst_surf->d3d11_texture);
                dst_surf->d3d11_texture = (ID3D11Texture2D*)png_resource; /* takes ownership of GetResource ref */

                if (dst_surf->d3d11_srv)
                    ID3D11ShaderResourceView_Release(dst_surf->d3d11_srv);
                dst_surf->d3d11_srv = png_srv;
                ID3D11ShaderResourceView_AddRef(png_srv);

                if (dst_surf->d3d11_rtv) {
                    ID3D11RenderTargetView_Release(dst_surf->d3d11_rtv);
                    dst_surf->d3d11_rtv = NULL;
                }

                /* Update the WRAPPER TEXTURE's own SRV (used by SetTexture) */
                if (self->d3d11_srv)
                    ID3D11ShaderResourceView_Release(self->d3d11_srv);
                self->d3d11_srv = png_srv; /* png_srv has the extra AddRef from above */
                self->r5g6b5_source = 0;
                s_tex_png_override++;
                return DD_OK;
            }
        }
    }

    /* --- Primary path: sys_buffer -> GPU texture --- */
    if (dst_surf->d3d11_texture && src_surf->sys_buffer && g_backend.context) {
        /* Debug: verify source data quality */
        {
            static int s_vfy = 0;
            if (s_vfy < 10) {
                uint16_t *px = (uint16_t*)src_surf->sys_buffer;
                int total = (int)(src_surf->width * src_surf->height);
                int nz = 0, varied = 0, i;
                uint16_t first_nz = 0;
                for (i = 0; i < total && i < 8192; i++) {
                    if (px[i] != 0) {
                        nz++;
                        if (!first_nz) first_nz = px[i];
                        else if (px[i] != first_nz) varied = 1;
                    }
                }
                WRAPPER_LOG("Texture::Load DATA: src=%p %ux%u sysbuf=%p nz=%d varied=%d "
                    "row0=%04X,%04X,%04X,%04X mid=%04X,%04X,%04X,%04X",
                    src_surf, src_surf->width, src_surf->height, src_surf->sys_buffer,
                    nz, varied,
                    px[0], px[1], px[2], px[3],
                    px[total/2], px[total/2+1], px[total/2+2], px[total/2+3]);
                s_vfy++;
            }
        }
        DWORD copy_width, copy_height, y;
        BYTE *src_ptr;
        int src_has_alpha;

        src_has_alpha = (src_surf->pixel_format.dwFlags & DDPF_ALPHAPIXELS) ? 1 : 0;

        {
            static int s_fmt_log = 0;
            if (s_fmt_log < 30) {
                uint16_t *px = (uint16_t*)src_surf->sys_buffer;
                WRAPPER_LOG("Texture::Load FMT: src=%p %ux%u pf_flags=0x%X alpha_flag=%d "
                    "Rmask=0x%04X Gmask=0x%04X Bmask=0x%04X Amask=0x%04X "
                    "ck=%d ck_val=0x%04X px0=0x%04X px1=0x%04X",
                    src_surf, src_surf->width, src_surf->height,
                    src_surf->pixel_format.dwFlags, src_has_alpha,
                    src_surf->pixel_format.dwRBitMask, src_surf->pixel_format.dwGBitMask,
                    src_surf->pixel_format.dwBBitMask, src_surf->pixel_format.dwRGBAlphaBitMask,
                    src_surf->has_colorkey, src_surf->colorkey_low,
                    px ? px[0] : 0, px ? px[1] : 0);
                s_fmt_log++;
            }
        }

        copy_width = dst_surf->width;
        copy_height = dst_surf->height;
        if (src_surf->width < copy_width) copy_width = src_surf->width;
        if (src_surf->height < copy_height) copy_height = src_surf->height;
        src_ptr = (BYTE*)src_surf->sys_buffer;

        if (src_surf->bpp == 16 && dst_surf->dxgi_format == DXGI_FORMAT_B8G8R8A8_UNORM) {
            /* 16-bit source → 32-bit B8G8R8A8 destination (format fallback path).
             * Buffer must be full texture width for UpdateSubresource (pDstBox=NULL). */
            DWORD dst_w = dst_surf->width;
            DWORD dst_h = dst_surf->height;
            DWORD row32 = dst_w * 4;
            BYTE *buf32 = (BYTE*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, row32 * dst_h);
            if (buf32) {
                int is_a1r5g5b5 = 0;

                /* Detect if source is A1R5G5B5 (1-5-5-5) or R5G6B5 (5-6-5) */
                if (src_has_alpha) {
                    /* Run heuristic: sample pixels to detect actual layout */
                    uint16_t *sample = (uint16_t*)src_ptr;
                    DWORD total = copy_width * copy_height;
                    DWORD limit = (total < 4096) ? total : 4096;
                    DWORD nonzero = 0, bit15_clear = 0, bright_transparent = 0;
                    DWORD i;
                    for (i = 0; i < limit; i++) {
                        uint16_t px = sample[i];
                        if (px != 0) {
                            nonzero++;
                            if ((px & 0x8000) == 0) {
                                bit15_clear++;
                                uint16_t r = (px >> 10) & 0x1F;
                                uint16_t g = (px >> 5) & 0x1F;
                                uint16_t b = px & 0x1F;
                                if (r + g + b > 10)
                                    bright_transparent++;
                            }
                        }
                    }
                    /* If many bright pixels have bit15=0, it's R5G6B5 misclassified */
                    is_a1r5g5b5 = !(nonzero > 16 && bit15_clear * 100 > nonzero
                                    && bright_transparent > bit15_clear / 4);
                }

                for (y = 0; y < copy_height; y++) {
                    uint16_t *src16 = (uint16_t*)(src_ptr + y * src_surf->pitch);
                    uint32_t *dst32 = (uint32_t*)(buf32 + y * row32);
                    DWORD x;
                    if (is_a1r5g5b5) {
                        /* A1R5G5B5: bit15=alpha, bits 14-10=R, 9-5=G, 4-0=B */
                        for (x = 0; x < copy_width; x++) {
                            uint16_t c = src16[x];
                            uint32_t a = (c & 0x8000) ? 0xFF000000 : 0x00000000;
                            uint32_t r = ((c >> 10) & 0x1F) * 255 / 31;
                            uint32_t g = ((c >> 5)  & 0x1F) * 255 / 31;
                            uint32_t b = ((c)       & 0x1F) * 255 / 31;
                            dst32[x] = a | (r << 16) | (g << 8) | b;
                        }
                        self->r5g6b5_source = 0;
                    } else {
                        /* R5G6B5: bits 15-11=R, 10-5=G, 4-0=B, no alpha.
                         * If the source surface has a color key, bake it into alpha:
                         * pixels matching the color key get alpha=0 (transparent),
                         * all others get alpha=0xFF (opaque). This is critical for
                         * trees, fences, and other foliage that use black color key. */
                        int use_ck = src_surf->has_colorkey;
                        uint16_t ck = (uint16_t)(src_surf->colorkey_low & 0xFFFF);
                        for (x = 0; x < copy_width; x++) {
                            uint16_t c = src16[x];
                            uint32_t r = ((c >> 11) & 0x1F) * 255 / 31;
                            uint32_t g = ((c >> 5)  & 0x3F) * 255 / 63;
                            uint32_t b = ((c)       & 0x1F) * 255 / 31;
                            uint32_t a = (use_ck && c == ck) ? 0x00000000 : 0xFF000000;
                            dst32[x] = a | (r << 16) | (g << 8) | b;
                        }
                        self->r5g6b5_source = use_ck ? 0 : 1;
                    }
                }

                /* Recreate texture with converted 32-bit data embedded at creation */
                {
                    D3D11_TEXTURE2D_DESC td;
                    D3D11_SUBRESOURCE_DATA init;
                    ID3D11Texture2D *new_tex = NULL;
                    ID3D11ShaderResourceView *new_srv = NULL;

                    ZeroMemory(&td, sizeof(td));
                    td.Width = dst_w;
                    td.Height = dst_h;
                    td.MipLevels = 1;
                    td.ArraySize = 1;
                    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                    td.SampleDesc.Count = 1;
                    td.Usage = D3D11_USAGE_DEFAULT;
                    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

                    init.pSysMem = buf32;
                    init.SysMemPitch = row32;
                    init.SysMemSlicePitch = 0;

                    if (SUCCEEDED(ID3D11Device_CreateTexture2D(g_backend.device, &td, &init, &new_tex))) {
                        if (SUCCEEDED(ID3D11Device_CreateShaderResourceView(g_backend.device,
                                (ID3D11Resource*)new_tex, NULL, &new_srv))) {
                            if (dst_surf->d3d11_srv) ID3D11ShaderResourceView_Release(dst_surf->d3d11_srv);
                            if (dst_surf->d3d11_texture) ID3D11Texture2D_Release(dst_surf->d3d11_texture);
                            dst_surf->d3d11_texture = new_tex;
                            dst_surf->d3d11_srv = new_srv;
                        } else {
                            s_tex_srv_fail++;
                            WRAPPER_LOG("Texture::Load: SRV creation FAILED for 16to32 tex %ux%u (fail #%d)",
                                        dst_w, dst_h, s_tex_srv_fail);
                            ID3D11Texture2D_Release(new_tex);
                        }
                    }
                }
                HeapFree(GetProcessHeap(), 0, buf32);
                s_tex_16to32++;
                WRAPPER_LOG("Texture::Load: 16bpp->B8G8R8A8 recreate OK (%ux%u a1r5=%d) [total=%d png=%d 16to32=%d]",
                            copy_width, copy_height, is_a1r5g5b5,
                            s_tex_total, s_tex_png_override, s_tex_16to32);
                SYNC_SRV_AFTER_LOAD();
                return DD_OK;
            }
        } else if (src_surf->bpp == 16) {
            /* 16-bit source → 16-bit destination.
             * Recreate the texture with initial data instead of staging+copy.
             * This is the most reliable upload method — no staging texture issues. */
            D3D11_TEXTURE2D_DESC td;
            D3D11_SUBRESOURCE_DATA init_data;
            ID3D11Texture2D *new_tex = NULL;
            ID3D11ShaderResourceView *new_srv = NULL;
            BYTE *packed_buf = NULL;
            DWORD row16 = dst_surf->width * 2;

            /* Pack source data into contiguous buffer (no pitch gaps) */
            packed_buf = (BYTE*)HeapAlloc(GetProcessHeap(), 0, row16 * dst_surf->height);
            if (packed_buf) {
                BYTE *dp = packed_buf;
                BYTE *sp = src_ptr;
                for (y = 0; y < copy_height; y++) {
                    CopyMemory(dp, sp, copy_width * 2);
                    if (copy_width < dst_surf->width)
                        ZeroMemory(dp + copy_width * 2, (dst_surf->width - copy_width) * 2);
                    sp += src_surf->pitch;
                    dp += row16;
                }
                /* Zero remaining rows if source is shorter */
                for (; y < dst_surf->height; y++) {
                    ZeroMemory(dp, row16);
                    dp += row16;
                }

                ZeroMemory(&td, sizeof(td));
                td.Width = dst_surf->width;
                td.Height = dst_surf->height;
                td.MipLevels = 1;
                td.ArraySize = 1;
                td.Format = dst_surf->dxgi_format;
                td.SampleDesc.Count = 1;
                td.Usage = D3D11_USAGE_DEFAULT;
                td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

                init_data.pSysMem = packed_buf;
                init_data.SysMemPitch = row16;
                init_data.SysMemSlicePitch = 0;

                if (SUCCEEDED(ID3D11Device_CreateTexture2D(g_backend.device, &td, &init_data, &new_tex))) {
                    if (SUCCEEDED(ID3D11Device_CreateShaderResourceView(g_backend.device,
                            (ID3D11Resource*)new_tex, NULL, &new_srv))) {
                        /* Replace old texture and SRV */
                        if (dst_surf->d3d11_srv) ID3D11ShaderResourceView_Release(dst_surf->d3d11_srv);
                        if (dst_surf->d3d11_texture) ID3D11Texture2D_Release(dst_surf->d3d11_texture);
                        dst_surf->d3d11_texture = new_tex;
                        dst_surf->d3d11_srv = new_srv;
                    } else {
                        s_tex_srv_fail++;
                        WRAPPER_LOG("Texture::Load: SRV creation FAILED for 16to16 tex %ux%u fmt=%d (fail #%d)",
                                    dst_surf->width, dst_surf->height, dst_surf->dxgi_format, s_tex_srv_fail);
                        ID3D11Texture2D_Release(new_tex);
                    }
                }
                HeapFree(GetProcessHeap(), 0, packed_buf);
            }

            self->r5g6b5_source = !src_has_alpha;
            s_tex_16to16++;
            SYNC_SRV_AFTER_LOAD();
            WRAPPER_LOG("Texture::Load: 16bpp recreate+init_data %ux%u fmt=%d [total=%d png=%d 16to16=%d]",
                        copy_width, copy_height, dst_surf->dxgi_format,
                        s_tex_total, s_tex_png_override, s_tex_16to16);
            return DD_OK;
        } else {
            /* 32-bit source → 32-bit destination (direct copy) */
            DWORD row_bytes = copy_width * 4;
            if ((DWORD)src_surf->pitch == row_bytes) {
                ID3D11DeviceContext_UpdateSubresource(g_backend.context,
                    (ID3D11Resource*)dst_surf->d3d11_texture, 0, NULL,
                    src_ptr, row_bytes, 0);
            } else {
                BYTE *conv_buf = (BYTE*)HeapAlloc(GetProcessHeap(), 0, row_bytes * copy_height);
                if (conv_buf) {
                    BYTE *dp = conv_buf;
                    BYTE *sp = src_ptr;
                    for (y = 0; y < copy_height; y++) {
                        CopyMemory(dp, sp, row_bytes);
                        sp += src_surf->pitch;
                        dp += row_bytes;
                    }
                    ID3D11DeviceContext_UpdateSubresource(g_backend.context,
                        (ID3D11Resource*)dst_surf->d3d11_texture, 0, NULL,
                        conv_buf, row_bytes, 0);
                    HeapFree(GetProcessHeap(), 0, conv_buf);
                }
            }
            WRAPPER_LOG("Texture::Load: 32bpp direct UpdateSubresource OK (%ux%u)",
                        copy_width, copy_height);
            SYNC_SRV_AFTER_LOAD();
            return DD_OK;
        }
    }

    /* --- Fallback: GPU-to-GPU copy via CopyResource --- */
    if (dst_surf->d3d11_texture && src_surf->d3d11_texture && g_backend.context) {
        ID3D11DeviceContext_CopyResource(g_backend.context,
            (ID3D11Resource*)dst_surf->d3d11_texture,
            (ID3D11Resource*)src_surf->d3d11_texture);
        WRAPPER_LOG("Texture::Load: CopyResource OK");
        SYNC_SRV_AFTER_LOAD();
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

    WRAPPER_LOG("WrapperTexture_Create: %p (surface=%p, d3d11_srv=%p)",
                tex, surface, surface ? surface->d3d11_srv : NULL);
    return tex;
}
