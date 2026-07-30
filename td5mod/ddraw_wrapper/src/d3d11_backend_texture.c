/**
 * d3d11_backend_texture.c -- D3D11 implementation of the opaque BackendTexture
 * handle (td5_backend_texture.h).
 *
 * Phase 0 of the D3D12 port: all ID3D11Texture2D / ...ShaderResourceView /
 * ...RenderTargetView lifecycle for game/COM-emulation surfaces lives here, so
 * surface4.c / texture2.c / png_loader.c / device3.c can be built D3D11-free
 * against either backend library. The logic is lifted verbatim from the old
 * inline code in surface4.c (Create / FlushDirty / EnsureDeviceCurrent) and
 * texture2.c (Texture::Load) so behaviour stays BIT-IDENTICAL -- this phase
 * must not change a single rendered pixel.
 */

#include "wrapper.h"

/* Body of the opaque handle -- visible only inside this backend translation
 * unit. Consumers see `BackendTexture` as an incomplete type. */
struct BackendTexture {
    ID3D11Texture2D            *tex;      /* GPU texture (NULL for adopted-SRV) */
    ID3D11ShaderResourceView   *srv;      /* SRV for shader sampling */
    ID3D11RenderTargetView     *rtv;      /* RTV (render targets only)         */
    ID3D11Texture2D            *staging;  /* CPU-write staging (RT upload)      */
    DXGI_FORMAT                 format;
    DWORD                       width, height;
    unsigned                    device_generation;
    LONG                        ref_count;
};

/* ---- internal helpers -------------------------------------------------- */

static BackendTexture *bt_alloc(void)
{
    BackendTexture *bt = (BackendTexture*)calloc(1, sizeof(BackendTexture));
    if (bt) bt->ref_count = 1;
    return bt;
}

static void bt_free_gpu(BackendTexture *bt)
{
    if (!bt) return;
    if (bt->srv)     { ID3D11ShaderResourceView_Release(bt->srv);     bt->srv = NULL; }
    if (bt->rtv)     { ID3D11RenderTargetView_Release(bt->rtv);       bt->rtv = NULL; }
    if (bt->staging) { ID3D11Texture2D_Release(bt->staging);          bt->staging = NULL; }
    if (bt->tex)     { ID3D11Texture2D_Release(bt->tex);              bt->tex = NULL; }
}

/* ---- lifecycle --------------------------------------------------------- */

BackendTexture *Backend_TextureCreate(DWORD w, DWORD h, DXGI_FORMAT fmt,
                                      int is_rt, int needs_staging)
{
    D3D11_TEXTURE2D_DESC td;
    BackendTexture *bt;

    if (!g_backend.device) return NULL;
    bt = bt_alloc();
    if (!bt) return NULL;

    bt->format = fmt;
    bt->width  = w;
    bt->height = h;
    bt->device_generation = g_backend.device_generation;

    ZeroMemory(&td, sizeof(td));
    td.Width            = w;
    td.Height           = h;
    td.MipLevels        = 1;
    td.ArraySize        = 1;
    td.Format           = fmt;
    td.SampleDesc.Count = 1;
    td.Usage            = D3D11_USAGE_DEFAULT;
    td.BindFlags        = D3D11_BIND_SHADER_RESOURCE |
                          (is_rt ? D3D11_BIND_RENDER_TARGET : 0);

    if (SUCCEEDED(ID3D11Device_CreateTexture2D(g_backend.device, &td, NULL, &bt->tex))) {
        ID3D11Device_CreateShaderResourceView(g_backend.device,
            (ID3D11Resource*)bt->tex, NULL, &bt->srv);
        if (is_rt) {
            ID3D11Device_CreateRenderTargetView(g_backend.device,
                (ID3D11Resource*)bt->tex, NULL, &bt->rtv);
        }
        if (needs_staging) {
            D3D11_TEXTURE2D_DESC stg;
            ZeroMemory(&stg, sizeof(stg));
            stg.Width            = w;
            stg.Height           = h;
            stg.MipLevels        = 1;
            stg.ArraySize        = 1;
            stg.Format           = fmt;
            stg.SampleDesc.Count = 1;
            stg.Usage            = D3D11_USAGE_STAGING;
            stg.CPUAccessFlags   = D3D11_CPU_ACCESS_WRITE;
            ID3D11Device_CreateTexture2D(g_backend.device, &stg, NULL, &bt->staging);
        }
    }
    return bt;
}

void Backend_TextureAddRef(BackendTexture *bt)
{
    if (bt) InterlockedIncrement(&bt->ref_count);
}

void Backend_TextureRelease(BackendTexture *bt)
{
    if (!bt) return;
    if (InterlockedDecrement(&bt->ref_count) <= 0) {
        bt_free_gpu(bt);
        free(bt);
    }
}

int Backend_TextureIsValid(const BackendTexture *bt)
{
    return bt && (bt->tex || bt->rtv);
}

int Backend_TextureHasRTV(const BackendTexture *bt)
{
    return bt && bt->rtv != NULL;
}

/* ---- binding ----------------------------------------------------------- */

void Backend_TextureBind(BackendTexture *bt)
{
    ID3D11ShaderResourceView *srv = (bt && bt->srv) ? bt->srv : g_backend.white_srv;
    g_backend.current_srv = srv;
    if (g_backend.context)
        ID3D11DeviceContext_PSSetShaderResources(g_backend.context, 0, 1, &srv);
}

void Backend_TextureBindRenderTarget(BackendTexture *bt)
{
    if (!g_backend.context || !bt || !bt->rtv) return;
    ID3D11DeviceContext_OMSetRenderTargets(g_backend.context,
        1, &bt->rtv, g_backend.depth_dsv);
}

void Backend_TextureClearRT(BackendTexture *bt, const float *rgba)
{
    if (!g_backend.context || !bt || !bt->rtv) return;
    ID3D11DeviceContext_ClearRenderTargetView(g_backend.context, bt->rtv, rgba);
}

/* ---- upload (surface FlushDirty) --------------------------------------- */

void Backend_TextureUpload(BackendTexture *bt, const void *sys, LONG src_pitch,
                           DWORD w, DWORD h, DWORD src_bpp)
{
    if (!bt || !bt->tex || !sys || !g_backend.context) return;

    if (src_bpp == 16 && bt->format == DXGI_FORMAT_B8G8R8A8_UNORM) {
        /* Convert R5G6B5 16bpp sys buffer -> B8G8R8A8 32bpp for upload */
        DWORD row32 = w * 4;
        BYTE *buf32 = (BYTE*)malloc(row32 * h);
        if (buf32) {
            DWORD y;
            for (y = 0; y < h; y++) {
                uint16_t *src16 = (uint16_t*)((BYTE*)sys + y * src_pitch);
                uint32_t *dst32 = (uint32_t*)(buf32 + y * row32);
                DWORD x;
                for (x = 0; x < w; x++) {
                    uint16_t c = src16[x];
                    uint32_t r = ((c >> 11) & 0x1F) * 255 / 31;
                    uint32_t g = ((c >> 5)  & 0x3F) * 255 / 63;
                    uint32_t b = ((c)       & 0x1F) * 255 / 31;
                    dst32[x] = 0xFF000000 | (r << 16) | (g << 8) | b;
                }
            }
            ID3D11DeviceContext_UpdateSubresource(g_backend.context,
                (ID3D11Resource*)bt->tex, 0, NULL, buf32, row32, 0);
            free(buf32);
        }
    } else {
        /* Use a transient staging texture for reliable, pitch-correct upload */
        D3D11_TEXTURE2D_DESC td;
        ID3D11Texture2D *staging = NULL;
        ZeroMemory(&td, sizeof(td));
        td.Width            = w;
        td.Height           = h;
        td.MipLevels        = 1;
        td.ArraySize        = 1;
        td.Format           = bt->format;
        td.SampleDesc.Count = 1;
        td.Usage            = D3D11_USAGE_STAGING;
        td.CPUAccessFlags   = D3D11_CPU_ACCESS_WRITE;
        if (SUCCEEDED(ID3D11Device_CreateTexture2D(g_backend.device, &td, NULL, &staging)) && staging) {
            D3D11_MAPPED_SUBRESOURCE mapped;
            if (SUCCEEDED(ID3D11DeviceContext_Map(g_backend.context,
                    (ID3D11Resource*)staging, 0, D3D11_MAP_WRITE, 0, &mapped))) {
                DWORD bpp_bytes = src_bpp / 8;
                DWORD row_bytes = w * bpp_bytes;
                DWORD y;
                for (y = 0; y < h; y++) {
                    CopyMemory((BYTE*)mapped.pData + y * mapped.RowPitch,
                               (BYTE*)sys + y * src_pitch, row_bytes);
                }
                ID3D11DeviceContext_Unmap(g_backend.context, (ID3D11Resource*)staging, 0);
                ID3D11DeviceContext_CopyResource(g_backend.context,
                    (ID3D11Resource*)bt->tex, (ID3D11Resource*)staging);
            }
            ID3D11Texture2D_Release(staging);
        }
    }
}

/* ---- device-lost rebuild (surface EnsureDeviceCurrent) ----------------- */

void Backend_TextureEnsureCurrent(BackendTexture *bt, DWORD w, DWORD h,
                                  DXGI_FORMAT fmt, int is_rt, int needs_staging)
{
    if (!bt || !g_backend.device) return;
    if (bt->device_generation == g_backend.device_generation) return;

    /* Drop the dead device's child objects (CPU-side refcount only). */
    bt_free_gpu(bt);

    {
        D3D11_TEXTURE2D_DESC td;
        ZeroMemory(&td, sizeof(td));
        td.Width            = w;
        td.Height           = h;
        td.MipLevels        = 1;
        td.ArraySize        = 1;
        td.Format           = fmt;
        td.SampleDesc.Count = 1;
        td.Usage            = D3D11_USAGE_DEFAULT;
        td.BindFlags        = D3D11_BIND_SHADER_RESOURCE |
                              (is_rt ? D3D11_BIND_RENDER_TARGET : 0);
        if (SUCCEEDED(ID3D11Device_CreateTexture2D(g_backend.device, &td, NULL, &bt->tex))) {
            ID3D11Device_CreateShaderResourceView(g_backend.device,
                (ID3D11Resource*)bt->tex, NULL, &bt->srv);
            if (is_rt) {
                ID3D11Device_CreateRenderTargetView(g_backend.device,
                    (ID3D11Resource*)bt->tex, NULL, &bt->rtv);
                if (needs_staging) {
                    D3D11_TEXTURE2D_DESC stg;
                    ZeroMemory(&stg, sizeof(stg));
                    stg.Width            = w;
                    stg.Height           = h;
                    stg.MipLevels        = 1;
                    stg.ArraySize        = 1;
                    stg.Format           = fmt;
                    stg.SampleDesc.Count = 1;
                    stg.Usage            = D3D11_USAGE_STAGING;
                    stg.CPUAccessFlags   = D3D11_CPU_ACCESS_WRITE;
                    ID3D11Device_CreateTexture2D(g_backend.device, &stg, NULL, &bt->staging);
                }
            }
        }
    }
    bt->format = fmt;
    bt->width  = w;
    bt->height = h;
    bt->device_generation = g_backend.device_generation;
}

/* ---- adopt a raw SRV (PNG override path) ------------------------------- */

BackendTexture *Backend_TextureAdopt(void *native_srv)
{
    ID3D11ShaderResourceView *srv = (ID3D11ShaderResourceView*)native_srv;
    BackendTexture *bt;
    ID3D11Resource *res = NULL;

    if (!srv) return NULL;
    bt = bt_alloc();
    if (!bt) return NULL;

    bt->srv = srv;                         /* takes the caller's ref */
    ID3D11ShaderResourceView_GetResource(srv, &res);
    bt->tex = (ID3D11Texture2D*)res;       /* GetResource returns a ref */
    bt->format = DXGI_FORMAT_B8G8R8A8_UNORM;
    bt->device_generation = g_backend.device_generation;
    return bt;
}

/* ---- the full IDirect3DTexture2::Load upload --------------------------- */

/* Recreate *pbt as a plain SHADER_RESOURCE texture from `pixels` (init data). */
static int bt_recreate_from_init(BackendTexture **pbt, DWORD w, DWORD h,
                                 DXGI_FORMAT fmt, const void *pixels, DWORD pitch)
{
    D3D11_TEXTURE2D_DESC td;
    D3D11_SUBRESOURCE_DATA init;
    ID3D11Texture2D *new_tex = NULL;
    ID3D11ShaderResourceView *new_srv = NULL;

    ZeroMemory(&td, sizeof(td));
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = fmt; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    init.pSysMem = pixels; init.SysMemPitch = pitch; init.SysMemSlicePitch = 0;

    if (SUCCEEDED(ID3D11Device_CreateTexture2D(g_backend.device, &td, &init, &new_tex))) {
        if (SUCCEEDED(ID3D11Device_CreateShaderResourceView(g_backend.device,
                (ID3D11Resource*)new_tex, NULL, &new_srv))) {
            BackendTexture *bt = *pbt;
            if (!bt) { bt = bt_alloc(); *pbt = bt; }
            if (bt) {
                bt_free_gpu(bt);
                bt->tex = new_tex; bt->srv = new_srv;
                bt->format = fmt; bt->width = w; bt->height = h;
                bt->device_generation = g_backend.device_generation;
                return 1;
            }
        }
        if (new_srv) ID3D11ShaderResourceView_Release(new_srv);
        ID3D11Texture2D_Release(new_tex);
    }
    return 0;
}

int Backend_TextureLoad(BackendTexture **pbt, DWORD dst_w, DWORD dst_h,
                        DXGI_FORMAT dst_fmt,
                        const void *src_pixels, LONG src_pitch,
                        DWORD src_w, DWORD src_h, DWORD src_bpp,
                        int src_has_alpha, int has_colorkey, DWORD colorkey_low,
                        BackendTexture *src_bt, int *out_r5g6b5_source)
{
    DWORD copy_width, copy_height, y;
    BackendTexture *dst = pbt ? *pbt : NULL;

    if (out_r5g6b5_source) *out_r5g6b5_source = 0;

    /* --- PNG override + dump (only R5G6B5 16-bit sources) --- */
    if (src_pixels && src_w > 0 && src_h > 0 && src_bpp == 16) {
        PngOverride_DumpTexture(src_w, src_h, src_pixels, src_pitch, 0);
        const char *png_path = PngOverride_Lookup(src_w, src_h, src_pixels, src_pitch);
        if (png_path) {
            ID3D11ShaderResourceView *png_srv = PngOverride_LoadToTexture(png_path);
            if (png_srv) {
                BackendTexture *nbt = Backend_TextureAdopt(png_srv);
                if (nbt) {
                    if (dst) Backend_TextureRelease(dst);
                    *pbt = nbt;
                    if (out_r5g6b5_source) *out_r5g6b5_source = 0;
                    return 1;
                }
            }
        }
    }

    /* --- Primary path: CPU pixels -> GPU texture --- */
    if (src_pixels && g_backend.context) {
        BYTE *src_ptr = (BYTE*)src_pixels;

        copy_width  = dst_w; copy_height = dst_h;
        if (src_w < copy_width)  copy_width  = src_w;
        if (src_h < copy_height) copy_height = src_h;

        if (src_bpp == 16 && dst_fmt == DXGI_FORMAT_B8G8R8A8_UNORM) {
            /* 16-bit source -> 32-bit B8G8R8A8 (format fallback). */
            DWORD row32 = dst_w * 4;
            BYTE *buf32 = (BYTE*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, row32 * dst_h);
            if (buf32) {
                int is_a1r5g5b5 = 0;
                if (src_has_alpha) {
                    uint16_t *sample = (uint16_t*)src_ptr;
                    DWORD total = copy_width * copy_height;
                    DWORD limit = (total < 4096) ? total : 4096;
                    DWORD nonzero = 0, bit15_clear = 0, bright_transparent = 0, i;
                    for (i = 0; i < limit; i++) {
                        uint16_t px = sample[i];
                        if (px != 0) {
                            nonzero++;
                            if ((px & 0x8000) == 0) {
                                bit15_clear++;
                                uint16_t r = (px >> 10) & 0x1F;
                                uint16_t g = (px >> 5) & 0x1F;
                                uint16_t b = px & 0x1F;
                                if (r + g + b > 10) bright_transparent++;
                            }
                        }
                    }
                    is_a1r5g5b5 = !(nonzero > 16 && bit15_clear * 100 > nonzero
                                    && bright_transparent > bit15_clear / 4);
                }
                for (y = 0; y < copy_height; y++) {
                    uint16_t *src16 = (uint16_t*)(src_ptr + y * src_pitch);
                    uint32_t *dst32 = (uint32_t*)(buf32 + y * row32);
                    DWORD x;
                    if (is_a1r5g5b5) {
                        for (x = 0; x < copy_width; x++) {
                            uint16_t c = src16[x];
                            uint32_t a = (c & 0x8000) ? 0xFF000000 : 0x00000000;
                            uint32_t r = ((c >> 10) & 0x1F) * 255 / 31;
                            uint32_t g = ((c >> 5)  & 0x1F) * 255 / 31;
                            uint32_t b = ((c)       & 0x1F) * 255 / 31;
                            dst32[x] = a | (r << 16) | (g << 8) | b;
                        }
                        if (out_r5g6b5_source) *out_r5g6b5_source = 0;
                    } else {
                        int use_ck = has_colorkey;
                        uint16_t ck = (uint16_t)(colorkey_low & 0xFFFF);
                        for (x = 0; x < copy_width; x++) {
                            uint16_t c = src16[x];
                            uint32_t r = ((c >> 11) & 0x1F) * 255 / 31;
                            uint32_t g = ((c >> 5)  & 0x3F) * 255 / 63;
                            uint32_t b = ((c)       & 0x1F) * 255 / 31;
                            uint32_t a = (use_ck && c == ck) ? 0x00000000 : 0xFF000000;
                            dst32[x] = a | (r << 16) | (g << 8) | b;
                        }
                        if (out_r5g6b5_source) *out_r5g6b5_source = use_ck ? 0 : 1;
                    }
                }
                bt_recreate_from_init(pbt, dst_w, dst_h, DXGI_FORMAT_B8G8R8A8_UNORM, buf32, row32);
                HeapFree(GetProcessHeap(), 0, buf32);
                return 1;
            }
        } else if (src_bpp == 16) {
            /* 16-bit source -> 16-bit destination. */
            DWORD row16 = dst_w * 2;
            BYTE *packed_buf = (BYTE*)HeapAlloc(GetProcessHeap(), 0, row16 * dst_h);
            if (packed_buf) {
                BYTE *dp = packed_buf;
                BYTE *sp = src_ptr;
                for (y = 0; y < copy_height; y++) {
                    CopyMemory(dp, sp, copy_width * 2);
                    if (copy_width < dst_w)
                        ZeroMemory(dp + copy_width * 2, (dst_w - copy_width) * 2);
                    sp += src_pitch;
                    dp += row16;
                }
                for (; y < dst_h; y++) { ZeroMemory(dp, row16); dp += row16; }
                bt_recreate_from_init(pbt, dst_w, dst_h, dst_fmt, packed_buf, row16);
                HeapFree(GetProcessHeap(), 0, packed_buf);
            }
            if (out_r5g6b5_source) *out_r5g6b5_source = !src_has_alpha;
            return 1;
        } else {
            /* 32-bit source -> 32-bit destination (direct copy). */
            if (!dst || !dst->tex) return 0;
            DWORD row_bytes = copy_width * 4;
            if ((DWORD)src_pitch == row_bytes) {
                ID3D11DeviceContext_UpdateSubresource(g_backend.context,
                    (ID3D11Resource*)dst->tex, 0, NULL, src_ptr, row_bytes, 0);
            } else {
                BYTE *conv_buf = (BYTE*)HeapAlloc(GetProcessHeap(), 0, row_bytes * copy_height);
                if (conv_buf) {
                    BYTE *dp = conv_buf, *sp = src_ptr;
                    for (y = 0; y < copy_height; y++) {
                        CopyMemory(dp, sp, row_bytes);
                        sp += src_pitch; dp += row_bytes;
                    }
                    ID3D11DeviceContext_UpdateSubresource(g_backend.context,
                        (ID3D11Resource*)dst->tex, 0, NULL, conv_buf, row_bytes, 0);
                    HeapFree(GetProcessHeap(), 0, conv_buf);
                }
            }
            return 1;
        }
    }

    /* --- Fallback: GPU-to-GPU copy via CopyResource --- */
    if (dst && dst->tex && src_bt && src_bt->tex && g_backend.context) {
        ID3D11DeviceContext_CopyResource(g_backend.context,
            (ID3D11Resource*)dst->tex, (ID3D11Resource*)src_bt->tex);
        return 1;
    }

    return 0;
}
