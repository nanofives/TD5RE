/**
 * surface4.c - IDirectDrawSurface4 COM implementation
 *
 * Wraps IDirectDrawSurface4 with a D3D11-backed surface.
 * The vtable inherits IDirectDrawSurface3->2->1->IUnknown.
 * Lock/Unlock uses a sys_buffer intermediary for pixel access.
 * Blt to primary surface is the present path (Backend_CompositeAndPresent).
 */

#include "wrapper.h"
#include <string.h>
#include <stdlib.h>

/* Original game design resolution — used as base for frontend sprite scaling */
#define TD5_DESIGN_W 640
#define TD5_DESIGN_H 480

/* EnumAttachedSurfaces callback type: LPDDENUMSURFACESCALLBACK2_W in wrapper.h */
typedef LPDDENUMSURFACESCALLBACK2_W LPDDENUMSURFACESCALLBACK2;

/* ========================================================================
 * Forward declarations for vtable methods
 * ======================================================================== */

static HRESULT __stdcall Surface4_QueryInterface(WrapperSurface *self, REFIID riid, void **out);
static ULONG   __stdcall Surface4_AddRef(WrapperSurface *self);
static ULONG   __stdcall Surface4_Release(WrapperSurface *self);
static HRESULT __stdcall Surface4_AddAttachedSurface(WrapperSurface *self, WrapperSurface *other);
static HRESULT __stdcall Surface4_AddOverlayDirtyRect(WrapperSurface *self, RECT *rect);
static HRESULT __stdcall Surface4_Blt(WrapperSurface *self, RECT *dst, WrapperSurface *src, RECT *srcRect, DWORD flags, DDBLTFX_W *fx);
static HRESULT __stdcall Surface4_BltBatch(WrapperSurface *self, void *batch, DWORD count, DWORD flags);
static HRESULT __stdcall Surface4_BltFast(WrapperSurface *self, DWORD x, DWORD y, WrapperSurface *src, RECT *srcRect, DWORD flags);
static HRESULT __stdcall Surface4_DeleteAttachedSurface(WrapperSurface *self, DWORD flags, WrapperSurface *attached);
static HRESULT __stdcall Surface4_EnumAttachedSurfaces(WrapperSurface *self, void *ctx, LPDDENUMSURFACESCALLBACK2 cb);
static HRESULT __stdcall Surface4_EnumOverlayZOrders(WrapperSurface *self, DWORD flags, void *ctx, void *cb);
static HRESULT __stdcall Surface4_Flip(WrapperSurface *self, WrapperSurface *target, DWORD flags);
static HRESULT __stdcall Surface4_GetAttachedSurface(WrapperSurface *self, DDSCAPS2_W *caps, WrapperSurface **out);
static HRESULT __stdcall Surface4_GetBltStatus(WrapperSurface *self, DWORD flags);
static HRESULT __stdcall Surface4_GetCaps(WrapperSurface *self, DDSCAPS2_W *caps);
static HRESULT __stdcall Surface4_GetClipper(WrapperSurface *self, void **clipper);
static HRESULT __stdcall Surface4_GetColorKey(WrapperSurface *self, DWORD flags, void *ck);
static HRESULT __stdcall Surface4_GetDC(WrapperSurface *self, HDC *hdc);
static HRESULT __stdcall Surface4_GetFlipStatus(WrapperSurface *self, DWORD flags);
static HRESULT __stdcall Surface4_GetOverlayPosition(WrapperSurface *self, LONG *x, LONG *y);
static HRESULT __stdcall Surface4_GetPalette(WrapperSurface *self, void **palette);
static HRESULT __stdcall Surface4_GetPixelFormat(WrapperSurface *self, DDPIXELFORMAT_W *pf);
static HRESULT __stdcall Surface4_GetSurfaceDesc(WrapperSurface *self, DDSURFACEDESC2_W *desc);
static HRESULT __stdcall Surface4_Initialize(WrapperSurface *self, void *ddraw, DDSURFACEDESC2_W *desc);
static HRESULT __stdcall Surface4_IsLost(WrapperSurface *self);
static HRESULT __stdcall Surface4_Lock(WrapperSurface *self, RECT *rect, DDSURFACEDESC2_W *desc, DWORD flags, HANDLE event);
static HRESULT __stdcall Surface4_ReleaseDC(WrapperSurface *self, HDC hdc);
static HRESULT __stdcall Surface4_Restore(WrapperSurface *self);
static HRESULT __stdcall Surface4_SetClipper(WrapperSurface *self, WrapperClipper *clipper);
static HRESULT __stdcall Surface4_SetColorKey(WrapperSurface *self, DWORD flags, void *ck);
static HRESULT __stdcall Surface4_SetOverlayPosition(WrapperSurface *self, LONG x, LONG y);
static HRESULT __stdcall Surface4_SetPalette(WrapperSurface *self, void *palette);
static HRESULT __stdcall Surface4_Unlock(WrapperSurface *self, RECT *rect);
static HRESULT __stdcall Surface4_UpdateOverlay(WrapperSurface *self, RECT *src, WrapperSurface *dst, RECT *dstRect, DWORD flags, void *fx);
static HRESULT __stdcall Surface4_UpdateOverlayDisplay(WrapperSurface *self, DWORD flags);
static HRESULT __stdcall Surface4_UpdateOverlayZOrder(WrapperSurface *self, DWORD flags, WrapperSurface *ref);
static HRESULT __stdcall Surface4_GetDDInterface(WrapperSurface *self, void **ddraw);
static HRESULT __stdcall Surface4_PageLock(WrapperSurface *self, DWORD flags);
static HRESULT __stdcall Surface4_PageUnlock(WrapperSurface *self, DWORD flags);
static HRESULT __stdcall Surface4_SetSurfaceDesc(WrapperSurface *self, DDSURFACEDESC2_W *desc, DWORD flags);
static HRESULT __stdcall Surface4_SetPrivateData(WrapperSurface *self, REFGUID tag, void *data, DWORD size, DWORD flags);
static HRESULT __stdcall Surface4_GetPrivateData(WrapperSurface *self, REFGUID tag, void *data, DWORD *size);
static HRESULT __stdcall Surface4_FreePrivateData(WrapperSurface *self, REFGUID tag);
static HRESULT __stdcall Surface4_GetUniquenessValue(WrapperSurface *self, DWORD *value);
static HRESULT __stdcall Surface4_ChangeUniquenessValue(WrapperSurface *self);

/* Vtable definition now in wrapper.h */

static WrapperSurfaceVtbl s_surface4_vtbl = {
    Surface4_QueryInterface,
    Surface4_AddRef,
    Surface4_Release,
    Surface4_AddAttachedSurface,
    Surface4_AddOverlayDirtyRect,
    Surface4_Blt,
    Surface4_BltBatch,
    Surface4_BltFast,
    Surface4_DeleteAttachedSurface,
    Surface4_EnumAttachedSurfaces,
    Surface4_EnumOverlayZOrders,
    Surface4_Flip,
    Surface4_GetAttachedSurface,
    Surface4_GetBltStatus,
    Surface4_GetCaps,
    Surface4_GetClipper,
    Surface4_GetColorKey,
    Surface4_GetDC,
    Surface4_GetFlipStatus,
    Surface4_GetOverlayPosition,
    Surface4_GetPalette,
    Surface4_GetPixelFormat,
    Surface4_GetSurfaceDesc,
    Surface4_Initialize,
    Surface4_IsLost,
    Surface4_Lock,
    Surface4_ReleaseDC,
    Surface4_Restore,
    Surface4_SetClipper,
    Surface4_SetColorKey,
    Surface4_SetOverlayPosition,
    Surface4_SetPalette,
    Surface4_Unlock,
    Surface4_UpdateOverlay,
    Surface4_UpdateOverlayDisplay,
    Surface4_UpdateOverlayZOrder,
    Surface4_GetDDInterface,
    Surface4_PageLock,
    Surface4_PageUnlock,
    Surface4_SetSurfaceDesc,
    Surface4_SetPrivateData,
    Surface4_GetPrivateData,
    Surface4_FreePrivateData,
    Surface4_GetUniquenessValue,
    Surface4_ChangeUniquenessValue,
};

/* ========================================================================
 * Helpers
 * ======================================================================== */

/**
 * Fill a DDSURFACEDESC2_W from surface properties.
 */
static void fill_surface_desc(WrapperSurface *s, DDSURFACEDESC2_W *desc)
{
    memset(desc, 0, sizeof(*desc));
    desc->dwSize   = sizeof(DDSURFACEDESC2_W);
    desc->dwFlags  = DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PITCH | DDSD_PIXELFORMAT;
    /* Report virtual dimensions to game if this is a frontend surface */
    if (s->is_frontend_surface && s->virtual_width > 0) {
        desc->dwWidth  = s->virtual_width;
        desc->dwHeight = s->virtual_height;
        desc->lPitch   = s->virtual_pitch;
    } else {
        desc->dwWidth  = s->width;
        desc->dwHeight = s->height;
        desc->lPitch   = s->pitch;
    }
    memcpy(&desc->ddpfPixelFormat, &s->pixel_format, sizeof(DDPIXELFORMAT_W));
    desc->ddsCaps.dwCaps  = s->caps;
    desc->ddsCaps.dwCaps2 = 0;
    desc->ddsCaps.dwCaps3 = 0;
    desc->ddsCaps.dwCaps4 = 0;
}

/**
 * Software fill of a rectangle in sys_buffer with a 16-bit or 32-bit color.
 */
static void software_fill(WrapperSurface *s, RECT *dst, DWORD color)
{
    DWORD x0, y0, x1, y1, bpp_bytes, y;

    WrapperSurface_EnsureSysBuffer(s);

    if (dst) {
        x0 = (DWORD)dst->left;
        y0 = (DWORD)dst->top;
        x1 = (DWORD)dst->right;
        y1 = (DWORD)dst->bottom;
    } else {
        x0 = 0; y0 = 0;
        x1 = s->width;
        y1 = s->height;
    }

    if (x1 > s->width)  x1 = s->width;
    if (y1 > s->height) y1 = s->height;

    bpp_bytes = s->bpp / 8;

    for (y = y0; y < y1; y++) {
        unsigned char *row = (unsigned char*)s->sys_buffer + y * s->pitch + x0 * bpp_bytes;
        DWORD x;
        if (bpp_bytes == 2) {
            unsigned short *p = (unsigned short*)row;
            unsigned short c = (unsigned short)(color & 0xFFFF);
            for (x = x0; x < x1; x++) *p++ = c;
        } else {
            DWORD *p = (DWORD*)row;
            for (x = x0; x < x1; x++) *p++ = color;
        }
    }
}

/**
 * Software blit from src sys_buffer to dst sys_buffer.
 */
static void software_blt(WrapperSurface *dst_surf, DWORD dstX, DWORD dstY,
                          WrapperSurface *src_surf, RECT *srcRect, int use_colorkey)
{
    LONG sx0, sy0;
    DWORD sw, sh, bpp_bytes, y, copy_bytes;

    WrapperSurface_EnsureSysBuffer(dst_surf);
    WrapperSurface_EnsureSysBuffer(src_surf);

    if (srcRect) {
        sx0 = srcRect->left;
        sy0 = srcRect->top;
        sw  = (DWORD)(srcRect->right - srcRect->left);
        sh  = (DWORD)(srcRect->bottom - srcRect->top);
    } else {
        sx0 = 0; sy0 = 0;
        sw = src_surf->width;
        sh = src_surf->height;
    }

    /* Reject completely out-of-bounds coordinates (signed overflow protection).
     * BltFast uses DWORD for x,y but the game passes negative values for
     * off-screen animation (slide-in effects). These appear as huge DWORD values. */
    if (dstX >= dst_surf->width || dstY >= dst_surf->height)
        return;
    if (dstX > 0x7FFFFFFF || dstY > 0x7FFFFFFF)
        return;  /* Negative coordinate disguised as large unsigned */

    /* Clip to dest bounds */
    if (dstX + sw > dst_surf->width)  sw = dst_surf->width - dstX;
    if (dstY + sh > dst_surf->height) sh = dst_surf->height - dstY;

    bpp_bytes = dst_surf->bpp / 8;
    copy_bytes = sw * bpp_bytes;

    /* Debug: log first 20 unique source surfaces with data status */
    {
        static void *s_seen[20] = {0};
        static int s_count = 0;
        int already = 0, i;
        for (i = 0; i < s_count; i++) { if (s_seen[i] == src_surf) { already = 1; break; } }
        if (!already && s_count < 20 && src_surf->sys_buffer && sh > 0) {
            s_seen[s_count++] = src_surf;
            uint16_t *px = (uint16_t*)src_surf->sys_buffer;
            int has_data = 0;
            for (i = 0; i < (int)(src_surf->width * src_surf->height) && i < 1000; i++) {
                if (px[i] != 0) { has_data = 1; break; }
            }
            WRAPPER_LOG("software_blt: src=%p %ux%u has_data=%d px0=0x%04X -> dst=%p (%u,%u)",
                src_surf, src_surf->width, src_surf->height, has_data, px[0],
                dst_surf, dstX, dstY);
        }
    }

    if (src_surf->bpp == 16 && dst_surf->bpp == 16) {
        /* 16-bit same format */
        uint16_t ck = (uint16_t)(src_surf->colorkey_low & 0xFFFF);
        for (y = 0; y < sh; y++) {
            uint16_t *src_row = (uint16_t*)((unsigned char*)src_surf->sys_buffer
                + (sy0 + y) * src_surf->pitch) + sx0;
            uint16_t *dst_row = (uint16_t*)((unsigned char*)dst_surf->sys_buffer
                + (dstY + y) * dst_surf->pitch) + dstX;
            if (use_colorkey && src_surf->has_colorkey) {
                DWORD x;
                for (x = 0; x < sw; x++) {
                    if (src_row[x] != ck)
                        dst_row[x] = src_row[x];
                }
            } else {
                memcpy(dst_row, src_row, sw * 2);
            }
        }
    } else if (src_surf->bpp == 32 && dst_surf->bpp == 32) {
        /* 32-bit same format */
        uint32_t ck = src_surf->colorkey_low;
        for (y = 0; y < sh; y++) {
            uint32_t *src_row = (uint32_t*)((unsigned char*)src_surf->sys_buffer
                + (sy0 + y) * src_surf->pitch) + sx0;
            uint32_t *dst_row = (uint32_t*)((unsigned char*)dst_surf->sys_buffer
                + (dstY + y) * dst_surf->pitch) + dstX;
            if (use_colorkey && src_surf->has_colorkey) {
                DWORD x;
                for (x = 0; x < sw; x++) {
                    if ((src_row[x] & 0x00FFFFFF) != (ck & 0x00FFFFFF))
                        dst_row[x] = src_row[x];
                }
            } else {
                memcpy(dst_row, src_row, sw * 4);
            }
        }
    } else if (src_surf->bpp == 16 && dst_surf->bpp == 32) {
        /* RGB565 → ARGB8888 conversion with color key */
        uint16_t ck = (uint16_t)(src_surf->colorkey_low & 0xFFFF);
        DWORD x;
        for (y = 0; y < sh; y++) {
            uint16_t *src_row = (uint16_t*)((unsigned char*)src_surf->sys_buffer
                + (sy0 + y) * src_surf->pitch) + sx0;
            uint32_t *dst_row = (uint32_t*)((unsigned char*)dst_surf->sys_buffer
                + (dstY + y) * dst_surf->pitch) + dstX;
            for (x = 0; x < sw; x++) {
                uint16_t px = src_row[x];
                if (use_colorkey && src_surf->has_colorkey && px == ck)
                    continue;  /* Skip transparent pixel */
                DWORD r = ((px >> 11) & 0x1F) * 255 / 31;
                DWORD g = ((px >> 5)  & 0x3F) * 255 / 63;
                DWORD b = ((px)       & 0x1F) * 255 / 31;
                dst_row[x] = 0xFF000000 | (r << 16) | (g << 8) | b;
            }
        }
    } else {
        /* Fallback: direct copy with source bpp, no color key */
        DWORD src_bpp_bytes = src_surf->bpp / 8;
        for (y = 0; y < sh; y++) {
            unsigned char *dst_row = (unsigned char*)dst_surf->sys_buffer
                + (dstY + y) * dst_surf->pitch + dstX * bpp_bytes;
            unsigned char *src_row = (unsigned char*)src_surf->sys_buffer
                + (sy0 + y) * src_surf->pitch + sx0 * src_bpp_bytes;
            memcpy(dst_row, src_row, sw * src_bpp_bytes);
        }
    }
}

/* ========================================================================
 * IUnknown
 * ======================================================================== */

static HRESULT __stdcall Surface4_QueryInterface(WrapperSurface *self, REFIID riid, void **out)
{
    if (!out) return E_POINTER;

    if (IsEqualGUID(riid, &IID_IDirectDrawSurface4) ||
        IsEqualGUID(riid, &IID_IUnknown)) {
        Surface4_AddRef(self);
        *out = self;
        return S_OK;
    }

    if (IsEqualGUID(riid, &IID_IDirect3DTexture2)) {
        if (!self->texture_wrapper) {
            self->texture_wrapper = WrapperTexture_Create(self);
            if (!self->texture_wrapper) {
                *out = NULL;
                return E_NOINTERFACE;
            }
        }
        /* AddRef the texture wrapper (caller owns the ref) */
        *out = self->texture_wrapper;
        /* The texture's own AddRef is handled inside WrapperTexture_Create or by caller */
        return S_OK;
    }

    WRAPPER_LOG("Surface4_QueryInterface: unknown IID");
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG __stdcall Surface4_AddRef(WrapperSurface *self)
{
    return InterlockedIncrement(&self->ref_count);
}

static ULONG __stdcall Surface4_Release(WrapperSurface *self)
{
    LONG ref = InterlockedDecrement(&self->ref_count);
    WRAPPER_LOG("Surface4_Release: %p ref=%ld (%ux%u caps=0x%X)", self, ref, self->width, self->height, self->caps);
    if (ref <= 0) {
        /* Don't free primary/backbuffer surfaces — game ref counting is sloppy */
        if (self == g_backend.primary || self == g_backend.backbuffer || self == g_backend.zbuffer) {
            WRAPPER_LOG("Surface4_Release: keeping alive (backend surface)");
            self->ref_count = 0;
            return 0;
        }
        WRAPPER_LOG("Surface4_Release: destroying surface %p", self);

        /* Clear bltfast_surface reference if this was the tracked surface */
        if (g_backend.bltfast_surface == self)
            g_backend.bltfast_surface = NULL;

        /* If this surface has a texture_wrapper that the game may still use
         * for rendering, transfer the D3D11 SRV to the WrapperTexture
         * so it survives surface destruction. M2DX releases DirectDraw surfaces
         * after Texture::Load but keeps the WrapperTexture for SetTexture. */
        if (self->texture_wrapper && self->d3d11_srv) {
            self->texture_wrapper->d3d11_srv = self->d3d11_srv;
            ID3D11ShaderResourceView_AddRef(self->d3d11_srv);
            WRAPPER_LOG("Surface4_Release: transferred d3d11_srv %p to WrapperTexture %p",
                self->d3d11_srv, self->texture_wrapper);
        }

        if (self->virtual_lock_buffer) {
            free(self->virtual_lock_buffer);
            self->virtual_lock_buffer = NULL;
        }
        if (self->sys_buffer) {
            free(self->sys_buffer);
            self->sys_buffer = NULL;
        }
        if (self->d3d11_staging) {
            ID3D11Texture2D_Release(self->d3d11_staging);
            self->d3d11_staging = NULL;
        }
        if (self->d3d11_rtv) {
            ID3D11RenderTargetView_Release(self->d3d11_rtv);
            self->d3d11_rtv = NULL;
        }
        if (self->d3d11_srv) {
            ID3D11ShaderResourceView_Release(self->d3d11_srv);
            self->d3d11_srv = NULL;
        }
        if (self->d3d11_texture) {
            ID3D11Texture2D_Release(self->d3d11_texture);
            self->d3d11_texture = NULL;
        }
        free(self);
        return 0;
    }
    return (ULONG)ref;
}

/* ========================================================================
 * IDirectDrawSurface methods
 * ======================================================================== */

static HRESULT __stdcall Surface4_AddAttachedSurface(WrapperSurface *self, WrapperSurface *other)
{
    WRAPPER_LOG("Surface4_AddAttachedSurface: self=%p other=%p (caps=0x%X)",
                self, other, other ? other->caps : 0);
    if (!other) return E_POINTER;

    /* Attach Z-buffer to render target */
    if (other->caps & DDSCAPS_ZBUFFER) {
        self->attached_zbuf = other;
        Surface4_AddRef(other);
        /* D3D11 depth is managed by the backend (depth_dsv), not per-surface */
        WRAPPER_LOG("Surface4_AddAttachedSurface: Z-buffer attached (depth managed by backend)");
        return DD_OK;
    }

    /* Attach back buffer */
    if (other->caps & DDSCAPS_BACKBUFFER) {
        self->attached_back = other;
        Surface4_AddRef(other);
        return DD_OK;
    }

    WRAPPER_LOG("Surface4_AddAttachedSurface: unknown attachment caps=0x%X", other->caps);
    return DD_OK;
}

static HRESULT __stdcall Surface4_AddOverlayDirtyRect(WrapperSurface *self, RECT *rect)
{
    WRAPPER_STUB("Surface4_AddOverlayDirtyRect");
}

static HRESULT __stdcall Surface4_Blt(WrapperSurface *self, RECT *dst,
    WrapperSurface *src, RECT *srcRect, DWORD flags, DDBLTFX_W *fx)
{
    WRAPPER_LOG("Surface4_Blt: self=%p dst=(%d,%d,%d,%d) src=%p flags=0x%08x",
                self,
                dst ? dst->left : 0, dst ? dst->top : 0,
                dst ? dst->right : 0, dst ? dst->bottom : 0,
                src, flags);

    /* Color fill — always use software_fill for all surfaces */
    if (flags & DDBLT_COLORFILL) {
        DWORD fill_color = fx ? fx->dwFillColor : 0;

        WrapperSurface_EnsureSysBuffer(self);
        software_fill(self, dst, fill_color);
        self->dirty = 1;
        return DD_OK;
    }

    /* Source blit */
    if (src) {
        /* Present path: Blt to primary surface. */
        if (self == g_backend.primary && g_backend.device) {
            Backend_CompositeAndPresent(src, srcRect, dst);
            return DD_OK;
        }

        /* Software fallback for all other surface-to-surface blits */
        {
            DWORD dstX = dst ? (DWORD)dst->left : 0;
            DWORD dstY = dst ? (DWORD)dst->top  : 0;
            software_blt(self, dstX, dstY, src, srcRect, (flags & DDBLT_KEYSRC) ? 1 : 0);
        }
        return DD_OK;
    }

    return DD_OK;
}

static HRESULT __stdcall Surface4_BltBatch(WrapperSurface *self, void *batch, DWORD count, DWORD flags)
{
    WRAPPER_STUB("Surface4_BltBatch");
}

/**
 * Scaled software blit for frontend native-res rendering.
 * Source pixels at original size are stretched into the native-res destination
 * with scaled coordinates. Uses nearest-neighbor with color key support.
 */
/**
 * Scaled software blit for runtime texture upscaling.
 * Computes scale factor from source vs destination surface dimensions.
 * Uses bilinear interpolation for smooth upscaling of frontend textures.
 */
static void software_blt_scaled(
    WrapperSurface *dst_surf, DWORD dstX, DWORD dstY,
    WrapperSurface *src_surf, RECT *srcRect, int use_colorkey)
{
    LONG sx0, sy0;
    DWORD sw, sh, dw, dh;
    float scaleX, scaleY;

    WrapperSurface_EnsureSysBuffer(dst_surf);
    WrapperSurface_EnsureSysBuffer(src_surf);

    /* Source rect */
    if (srcRect) {
        sx0 = srcRect->left; sy0 = srcRect->top;
        sw = (DWORD)(srcRect->right - srcRect->left);
        sh = (DWORD)(srcRect->bottom - srcRect->top);
    } else {
        sx0 = 0; sy0 = 0;
        sw = src_surf->width;
        sh = src_surf->height;
    }

    if (sw == 0 || sh == 0) return;

    /* Compute scale from ratio of destination to source surface dimensions.
     * E.g., ButtonBits (56x100) -> button surface (112x200) = 2x scale.
     * Use the smaller of the two ratios to avoid overflowing the dest. */
    scaleX = (src_surf->width > 0) ? (float)dst_surf->width / (float)src_surf->width : 1.0f;
    scaleY = (src_surf->height > 0) ? (float)dst_surf->height / (float)src_surf->height : 1.0f;

    /* Clamp scale to reasonable range */
    if (scaleX < 1.0f) scaleX = 1.0f;
    if (scaleY < 1.0f) scaleY = 1.0f;
    if (scaleX > 8.0f) scaleX = 8.0f;
    if (scaleY > 8.0f) scaleY = 8.0f;

    /* Scale destination position and copy size */
    dw = (DWORD)(sw * scaleX);
    dh = (DWORD)(sh * scaleY);
    {
        DWORD ndstX = (DWORD)(dstX * scaleX);
        DWORD ndstY = (DWORD)(dstY * scaleY);

        /* Bounds check */
        if (ndstX >= dst_surf->width || ndstY >= dst_surf->height) return;
        if (ndstX + dw > dst_surf->width)  dw = dst_surf->width - ndstX;
        if (ndstY + dh > dst_surf->height) dh = dst_surf->height - ndstY;

        /* Bilinear interpolation with color key */
        if (src_surf->bpp == 16 && dst_surf->bpp == 16) {
            uint16_t ck = (uint16_t)(src_surf->colorkey_low & 0xFFFF);
            DWORD y;
            for (y = 0; y < dh; y++) {
                float fy = sy0 + ((float)y / scaleY);
                DWORD iy = (DWORD)fy;
                float fy_frac = fy - (float)iy;
                DWORD iy1 = (iy + 1 < src_surf->height) ? iy + 1 : iy;

                uint16_t *src_r0 = (uint16_t*)((BYTE*)src_surf->sys_buffer + iy * src_surf->pitch);
                uint16_t *src_r1 = (uint16_t*)((BYTE*)src_surf->sys_buffer + iy1 * src_surf->pitch);
                uint16_t *dst_row = (uint16_t*)((BYTE*)dst_surf->sys_buffer
                    + (ndstY + y) * dst_surf->pitch);
                DWORD x;

                for (x = 0; x < dw; x++) {
                    float fx = sx0 + ((float)x / scaleX);
                    DWORD ix = (DWORD)fx;
                    float fx_frac = fx - (float)ix;
                    DWORD ix1 = (ix + 1 < src_surf->width) ? ix + 1 : ix;

                    /* Sample 4 source pixels */
                    uint16_t p00 = src_r0[ix],  p10 = src_r0[ix1];
                    uint16_t p01 = src_r1[ix],  p11 = src_r1[ix1];

                    /* Color key: use nearest-neighbor for keyed pixels */
                    if (use_colorkey && src_surf->has_colorkey) {
                        uint16_t nearest = src_r0[(DWORD)(fx + 0.5f)];
                        if (nearest == ck) continue;
                        /* If any sample is color key, fall back to nearest */
                        if (p00 == ck || p10 == ck || p01 == ck || p11 == ck) {
                            dst_row[ndstX + x] = nearest;
                            continue;
                        }
                    }

                    /* Bilinear blend in RGB565 */
                    {
                        /* Decompose 4 pixels to RGB */
                        float w00 = (1.0f - fx_frac) * (1.0f - fy_frac);
                        float w10 = fx_frac * (1.0f - fy_frac);
                        float w01 = (1.0f - fx_frac) * fy_frac;
                        float w11 = fx_frac * fy_frac;

                        float r = ((p00>>11)&0x1F)*w00 + ((p10>>11)&0x1F)*w10 +
                                  ((p01>>11)&0x1F)*w01 + ((p11>>11)&0x1F)*w11;
                        float g = ((p00>>5)&0x3F)*w00 + ((p10>>5)&0x3F)*w10 +
                                  ((p01>>5)&0x3F)*w01 + ((p11>>5)&0x3F)*w11;
                        float b = (p00&0x1F)*w00 + (p10&0x1F)*w10 +
                                  (p01&0x1F)*w01 + (p11&0x1F)*w11;

                        uint16_t result = ((uint16_t)(r + 0.5f) << 11) |
                                          ((uint16_t)(g + 0.5f) << 5) |
                                          (uint16_t)(b + 0.5f);
                        dst_row[ndstX + x] = result;
                    }
                }
            }
        }

        dst_surf->dirty = 1;
    }
}

static HRESULT __stdcall Surface4_BltFast(WrapperSurface *self, DWORD x, DWORD y,
    WrapperSurface *src, RECT *srcRect, DWORD flags)
{
    /* Reject off-screen coordinates (game uses negative values for slide-in animations) */
    if (x > 0x7FFFFFFF || y > 0x7FFFFFFF)
        return DD_OK;

    WRAPPER_LOG("Surface4_BltFast: dst=%p(%ux%u) x=%u y=%u src=%p(%ux%u) flags=0x%08x",
                self, self->width, self->height, x, y, src, src ? src->width : 0, src ? src->height : 0, flags);

    if (!src)
        return DDERR_GENERIC;

    if (x >= self->width || y >= self->height)
        return DD_OK;

    software_blt(self, x, y, src, srcRect,
                 (flags & DDBLTFAST_SRCCOLORKEY) ? 1 : 0);
    self->dirty = 1;

    /* Track this surface as the BltFast destination (2D layer).
     * At present time, its sys_buffer is composited as the background layer
     * underneath D3D render target content.
     * Only track surfaces that are NOT the D3D render target and match its dimensions.
     * RT surfaces have d3d11_staging — exclude them. */
    {
        WrapperSurface *rt = g_backend.backbuffer;
        if (rt && self != rt && !(self->caps & DDSCAPS_3DDEVICE) &&
            (self->width == rt->width && self->height == rt->height)) {
            if (g_backend.bltfast_surface != self) {
                WRAPPER_LOG("BltFast: tracking %p as bltfast_surface (%ux%u)",
                    self, self->width, self->height);
                g_backend.bltfast_surface = self;
            }
        }
    }

    return DD_OK;
}

static HRESULT __stdcall Surface4_DeleteAttachedSurface(WrapperSurface *self, DWORD flags, WrapperSurface *attached)
{
    WRAPPER_STUB("Surface4_DeleteAttachedSurface");
}

static HRESULT __stdcall Surface4_EnumAttachedSurfaces(WrapperSurface *self, void *ctx, LPDDENUMSURFACESCALLBACK2 cb)
{
    WRAPPER_LOG("Surface4_EnumAttachedSurfaces: self=%p", self);

    if (!cb)
        return DDERR_GENERIC;

    if (self->attached_back) {
        DDSURFACEDESC2_W desc;
        fill_surface_desc(self->attached_back, &desc);
        cb(self->attached_back, &desc, ctx);
    }

    return DD_OK;
}

static HRESULT __stdcall Surface4_EnumOverlayZOrders(WrapperSurface *self, DWORD flags, void *ctx, void *cb)
{
    WRAPPER_STUB("Surface4_EnumOverlayZOrders");
}

static HRESULT __stdcall Surface4_Flip(WrapperSurface *self, WrapperSurface *target, DWORD flags)
{
    WRAPPER_LOG("Surface4_Flip: self=%p target=%p flags=0x%08x", self, target, flags);

    if (!g_backend.device)
        return DDERR_GENERIC;

    /* Enforce windowed mode size/style on every present frame */
    Backend_EnforceWindowSize();

    /* Use the compositing path (handles both BltFast + D3D layer merging) */
    if (g_backend.backbuffer) {
        Backend_CompositeAndPresent(g_backend.backbuffer, NULL, NULL);
        return DD_OK;
    }

    /* Fallback: simple present via swap chain */
    IDXGISwapChain_Present(g_backend.swap_chain, 1, 0);
    return DD_OK;
}

static HRESULT __stdcall Surface4_GetAttachedSurface(WrapperSurface *self, DDSCAPS2_W *caps, WrapperSurface **out)
{
    WRAPPER_LOG("Surface4_GetAttachedSurface: self=%p caps=0x%08x", self, caps ? caps->dwCaps : 0);

    if (!out)
        return E_POINTER;

    if (caps) {
        if ((caps->dwCaps & DDSCAPS_BACKBUFFER) && self->attached_back) {
            Surface4_AddRef(self->attached_back);
            *out = self->attached_back;
            return DD_OK;
        }
        if ((caps->dwCaps & DDSCAPS_ZBUFFER) && self->attached_zbuf) {
            Surface4_AddRef(self->attached_zbuf);
            *out = self->attached_zbuf;
            return DD_OK;
        }
        if (caps->dwCaps & DDSCAPS_MIPMAP) {
            *out = NULL;
            return DDERR_UNSUPPORTED;
        }
    }

    *out = NULL;
    return DDERR_GENERIC;
}

static HRESULT __stdcall Surface4_GetBltStatus(WrapperSurface *self, DWORD flags)
{
    return DD_OK;
}

static HRESULT __stdcall Surface4_GetCaps(WrapperSurface *self, DDSCAPS2_W *caps)
{
    WRAPPER_LOG("Surface4_GetCaps: self=%p", self);

    if (!caps)
        return E_POINTER;

    caps->dwCaps  = self->caps;
    caps->dwCaps2 = 0;
    caps->dwCaps3 = 0;
    caps->dwCaps4 = 0;
    return DD_OK;
}

static HRESULT __stdcall Surface4_GetClipper(WrapperSurface *self, void **clipper)
{
    WRAPPER_STUB("Surface4_GetClipper");
}

static HRESULT __stdcall Surface4_GetColorKey(WrapperSurface *self, DWORD flags, void *ck)
{
    DWORD *ck_pair;

    WRAPPER_LOG("Surface4_GetColorKey: self=%p", self);

    if (!ck)
        return E_POINTER;

    if (!self->has_colorkey)
        return DDERR_GENERIC;

    /* DDCOLORKEY is two DWORDs: dwColorSpaceLowValue, dwColorSpaceHighValue */
    ck_pair = (DWORD*)ck;
    ck_pair[0] = self->colorkey_low;
    ck_pair[1] = self->colorkey_high;
    return DD_OK;
}

static HRESULT __stdcall Surface4_GetDC(WrapperSurface *self, HDC *hdc)
{
    HBITMAP hbm;
    HDC dc;
    /* Extra space for BI_BITFIELDS color masks (3 DWORDs) */
    struct { BITMAPINFOHEADER h; DWORD masks[3]; } bmi_buf;
    BITMAPINFO *bmi = (BITMAPINFO *)&bmi_buf;

    WRAPPER_LOG("Surface4_GetDC: self=%p %ux%u %ubpp", self, self->width, self->height, self->bpp);
    if (!hdc) return E_POINTER;

    WrapperSurface_EnsureSysBuffer(self);

    /* Create a GDI-compatible memory DC with a DIB section backed by sys_buffer.
     * M2DX uses this DC for GDI text rendering (TextOut, DrawText). */
    ZeroMemory(&bmi_buf, sizeof(bmi_buf));
    bmi_buf.h.biSize        = sizeof(BITMAPINFOHEADER);
    bmi_buf.h.biWidth       = (LONG)self->width;
    bmi_buf.h.biHeight      = -(LONG)self->height;  /* Top-down DIB */
    bmi_buf.h.biPlanes      = 1;
    bmi_buf.h.biBitCount    = (WORD)self->bpp;
    bmi_buf.h.biCompression = BI_BITFIELDS;
    bmi_buf.h.biSizeImage   = self->height * self->pitch;

    /* Set color masks for RGB565 or XRGB8888 */
    if (self->bpp == 16) {
        bmi_buf.masks[0] = 0xF800;  /* R */
        bmi_buf.masks[1] = 0x07E0;  /* G */
        bmi_buf.masks[2] = 0x001F;  /* B */
    } else {
        bmi_buf.masks[0] = 0x00FF0000;  /* R */
        bmi_buf.masks[1] = 0x0000FF00;  /* G */
        bmi_buf.masks[2] = 0x000000FF;  /* B */
    }

    dc = CreateCompatibleDC(NULL);
    if (!dc) {
        WRAPPER_LOG("Surface4_GetDC: CreateCompatibleDC failed");
        *hdc = NULL;
        return DDERR_GENERIC;
    }

    /* Create DIB section — GDI draws into this bitmap which shares our pixel format */
    {
        void *dib_bits = NULL;
        hbm = CreateDIBSection(dc, bmi, DIB_RGB_COLORS, &dib_bits, NULL, 0);
        if (!hbm || !dib_bits) {
            WRAPPER_LOG("Surface4_GetDC: CreateDIBSection failed");
            DeleteDC(dc);
            *hdc = NULL;
            return DDERR_GENERIC;
        }

        /* Copy current sys_buffer into the DIB so GDI draws on top of existing content */
        memcpy(dib_bits, self->sys_buffer, self->height * self->pitch);
    }

    SelectObject(dc, hbm);

    /* Store the DC and bitmap handle for ReleaseDC to retrieve and clean up.
     * Use GDI SetWindowOrgEx with a magic value to smuggle the bitmap handle. */
    self->locked = 1;  /* Mark as "DC locked" */

    /* Save hbm handle in the surface (reuse a field or add one).
     * For simplicity, store it via SetPropA on the window. Not ideal but works. */
    /* Actually, store in a static — single-threaded game, only one DC at a time */
    {
        static HBITMAP s_dc_bitmap = NULL;
        static void *s_dc_bits = NULL;
        static WrapperSurface *s_dc_surface = NULL;
        s_dc_bitmap = hbm;
        s_dc_bits = NULL;  /* Re-query from the DIB */
        s_dc_surface = self;

        /* Get the actual DIB bits pointer */
        DIBSECTION ds;
        GetObject(hbm, sizeof(ds), &ds);
        s_dc_bits = ds.dsBm.bmBits;
    }

    *hdc = dc;
    WRAPPER_LOG("Surface4_GetDC: returning HDC %p", dc);
    return DD_OK;
}

static HRESULT __stdcall Surface4_GetFlipStatus(WrapperSurface *self, DWORD flags)
{
    return DD_OK;
}

static HRESULT __stdcall Surface4_GetOverlayPosition(WrapperSurface *self, LONG *x, LONG *y)
{
    WRAPPER_STUB("Surface4_GetOverlayPosition");
}

static HRESULT __stdcall Surface4_GetPalette(WrapperSurface *self, void **palette)
{
    WRAPPER_STUB("Surface4_GetPalette");
}

static HRESULT __stdcall Surface4_GetPixelFormat(WrapperSurface *self, DDPIXELFORMAT_W *pf)
{
    WRAPPER_LOG("Surface4_GetPixelFormat: self=%p", self);

    if (!pf)
        return E_POINTER;

    memcpy(pf, &self->pixel_format, sizeof(DDPIXELFORMAT_W));
    return DD_OK;
}

static HRESULT __stdcall Surface4_GetSurfaceDesc(WrapperSurface *self, DDSURFACEDESC2_W *desc)
{
    WRAPPER_LOG("Surface4_GetSurfaceDesc: self=%p (%ux%u %ubpp)", self, self->width, self->height, self->bpp);

    if (!desc)
        return E_POINTER;

    fill_surface_desc(self, desc);
    return DD_OK;
}

static HRESULT __stdcall Surface4_Initialize(WrapperSurface *self, void *ddraw, DDSURFACEDESC2_W *desc)
{
    WRAPPER_STUB("Surface4_Initialize");
}

static HRESULT __stdcall Surface4_IsLost(WrapperSurface *self)
{
    return DD_OK;
}

/* ========================================================================
 * Frontend scaling: upsample/downsample between virtual and native buffers
 * ======================================================================== */

/**
 * Nearest-neighbor upscale: virtual_lock_buffer (640x480) -> sys_buffer (native).
 */
static void fe_upsample_buffer(WrapperSurface *s)
{
    float sx = (g_backend.m2dx_width > 0) ? (float)s->width / (float)g_backend.m2dx_width : 1.0f;
    float sy = (g_backend.m2dx_height > 0) ? (float)s->height / (float)g_backend.m2dx_height : 1.0f;
    DWORD vw = (DWORD)g_backend.m2dx_width, vh = (DWORD)g_backend.m2dx_height;
    DWORD nw = s->width, nh = s->height;
    DWORD y;

    if (!s->virtual_lock_buffer || !s->sys_buffer) return;

    for (y = 0; y < nh; y++) {
        DWORD src_y = (DWORD)(y / sy);
        if (src_y >= vh) src_y = vh - 1;
        if (s->bpp == 16) {
            uint16_t *src_row = (uint16_t*)((BYTE*)s->virtual_lock_buffer + src_y * s->virtual_pitch);
            uint16_t *dst_row = (uint16_t*)((BYTE*)s->sys_buffer + y * s->pitch);
            DWORD x;
            for (x = 0; x < nw; x++) {
                DWORD src_x = (DWORD)(x / sx);
                if (src_x >= vw) src_x = vw - 1;
                dst_row[x] = src_row[src_x];
            }
        }
    }
    FE_LOG(2, "fe_upsample: %ux%u -> %ux%u", vw, vh, nw, nh);
}

/**
 * Nearest-neighbor downsample: sys_buffer (native) -> virtual_lock_buffer (640x480).
 */
static void fe_downsample_buffer(WrapperSurface *s)
{
    float sx = (g_backend.m2dx_width > 0) ? (float)s->width / (float)g_backend.m2dx_width : 1.0f;
    float sy = (g_backend.m2dx_height > 0) ? (float)s->height / (float)g_backend.m2dx_height : 1.0f;
    DWORD vw = (DWORD)g_backend.m2dx_width, vh = (DWORD)g_backend.m2dx_height;
    DWORD y;

    if (!s->virtual_lock_buffer || !s->sys_buffer) return;

    for (y = 0; y < vh; y++) {
        DWORD src_y = (DWORD)(y * sy);
        if (src_y >= s->height) src_y = s->height - 1;
        if (s->bpp == 16) {
            uint16_t *src_row = (uint16_t*)((BYTE*)s->sys_buffer + src_y * s->pitch);
            uint16_t *dst_row = (uint16_t*)((BYTE*)s->virtual_lock_buffer + y * s->virtual_pitch);
            DWORD x;
            for (x = 0; x < vw; x++) {
                DWORD src_x = (DWORD)(x * sx);
                if (src_x >= s->width) src_x = s->width - 1;
                dst_row[x] = src_row[src_x];
            }
        }
    }
}

static HRESULT __stdcall Surface4_Lock(WrapperSurface *self, RECT *rect,
    DDSURFACEDESC2_W *desc, DWORD flags, HANDLE event)
{
    /* Log ALL Lock calls for texture surfaces (caps & TEXTURE), limit others */
    {
        static int s_lock_log = 0;
        int is_tex = (self->caps & DDSCAPS_TEXTURE) ? 1 : 0;
        if (is_tex || s_lock_log < 100) {
            WRAPPER_LOG("Surface4_Lock: self=%p %ux%u caps=0x%X bpp=%u sys=%p flags=0x%x",
                self, self->width, self->height, self->caps, self->bpp,
                self->sys_buffer, flags);
            if (!is_tex) s_lock_log++;
        }
    }

    if (!desc)
        return E_POINTER;

    /* Ensure sys_buffer is allocated */
    WrapperSurface_EnsureSysBuffer(self);

    /* In D3D11, textures can't be locked directly. The game writes to
     * sys_buffer via Lock, and FlushDirty uploads to the GPU later.
     * Just use sys_buffer as-is — no GPU readback needed. */

    /* Fill the desc (reports virtual dimensions if frontend surface) */
    fill_surface_desc(self, desc);

    /* Frontend surface with virtual lock buffer: game writes to 640x480 buffer,
     * upscaled to native on Unlock. */
    if (self->virtual_lock_buffer && g_backend.fe_scale.enabled) {
        if (rect) {
            DWORD bpp_bytes = self->bpp / 8;
            desc->lpSurface = (unsigned char*)self->virtual_lock_buffer
                + rect->top * self->virtual_pitch + rect->left * bpp_bytes;
            desc->dwWidth  = (DWORD)(rect->right - rect->left);
            desc->dwHeight = (DWORD)(rect->bottom - rect->top);
        } else {
            desc->lpSurface = self->virtual_lock_buffer;
        }
    } else {
        /* Normal path: direct sys_buffer access */
        if (rect) {
            DWORD bpp_bytes = self->bpp / 8;
            desc->lpSurface = (unsigned char*)self->sys_buffer
                + rect->top * self->pitch + rect->left * bpp_bytes;
            desc->dwWidth  = (DWORD)(rect->right - rect->left);
            desc->dwHeight = (DWORD)(rect->bottom - rect->top);
        } else {
            desc->lpSurface = self->sys_buffer;
        }
    }

    desc->dwFlags |= DDSD_LPSURFACE;
    self->locked = 1;

    return DD_OK;
}

static HRESULT __stdcall Surface4_ReleaseDC(WrapperSurface *self, HDC hdc)
{
    WRAPPER_LOG("Surface4_ReleaseDC: self=%p hdc=%p", self, hdc);
    if (!hdc) return DDERR_GENERIC;

    /* Get the current bitmap from the DC and copy its bits back to sys_buffer */
    HBITMAP hbm = (HBITMAP)GetCurrentObject(hdc, OBJ_BITMAP);
    if (hbm) {
        DIBSECTION ds;
        if (GetObject(hbm, sizeof(ds), &ds) && ds.dsBm.bmBits) {
            /* Copy DIB bits back to sys_buffer */
            DWORD copy_size = self->height * self->pitch;
            memcpy(self->sys_buffer, ds.dsBm.bmBits, copy_size);
            WRAPPER_LOG("Surface4_ReleaseDC: copied %u bytes back to sys_buffer", copy_size);
        }
        DeleteObject(hbm);
    }

    DeleteDC(hdc);
    self->locked = 0;
    self->dirty = 1;  /* Mark for upload to D3D11 */
    return DD_OK;
}

static HRESULT __stdcall Surface4_Restore(WrapperSurface *self)
{
    WRAPPER_LOG("Surface4_Restore: self=%p", self);
    return DD_OK;
}

static HRESULT __stdcall Surface4_SetClipper(WrapperSurface *self, WrapperClipper *clipper)
{
    WRAPPER_LOG("Surface4_SetClipper: self=%p clipper=%p", self, clipper);
    /* Store but don't use -- D3D11 handles clipping */
    return DD_OK;
}

static HRESULT __stdcall Surface4_SetColorKey(WrapperSurface *self, DWORD flags, void *ck)
{
    DWORD *ck_pair;

    WRAPPER_LOG("Surface4_SetColorKey: self=%p flags=0x%08x", self, flags);

    if (!ck)
        return E_POINTER;

    /* DDCOLORKEY is two DWORDs: dwColorSpaceLowValue, dwColorSpaceHighValue */
    ck_pair = (DWORD*)ck;
    self->colorkey_low  = ck_pair[0];
    self->colorkey_high = ck_pair[1];
    self->has_colorkey  = 1;
    return DD_OK;
}

static HRESULT __stdcall Surface4_SetOverlayPosition(WrapperSurface *self, LONG x, LONG y)
{
    WRAPPER_STUB("Surface4_SetOverlayPosition");
}

static HRESULT __stdcall Surface4_SetPalette(WrapperSurface *self, void *palette)
{
    WRAPPER_STUB("Surface4_SetPalette");
}

/**
 * Auto-stretch: detect if a surface larger than 640x480 contains only 640x480
 * of actual content (rest is black) and stretch it to fill the surface.
 * When HD replacement assets are loaded, they fill the surface natively and
 * this function becomes a no-op.
 */
static void fe_auto_stretch_content(WrapperSurface *s)
{
    DWORD base_w = 640, base_h = 480;
    DWORD y;

    /* Only stretch non-texture, non-3D surfaces larger than base resolution */
    if (!s->sys_buffer || s->bpp != 16) return;
    if (s->width <= base_w || s->height <= base_h) return;
    /* Skip textures and z-buffers. Allow 3DDEVICE (backbuffer) — the row-480
     * heuristic correctly distinguishes frontend (640x480 TGA) from 3D (native). */
    if (s->caps & (DDSCAPS_TEXTURE | DDSCAPS_ZBUFFER)) return;

    /* Heuristic: check if row base_h (first row beyond 640x480) is all black.
     * If not, the content is already at native resolution (HD asset) — skip. */
    {
        uint16_t *check_row = (uint16_t*)((BYTE*)s->sys_buffer + base_h * s->pitch);
        int has_content = 0;
        DWORD x;
        /* Sample a few pixels across the row */
        for (x = 0; x < s->width && x < 800; x += 40) {
            if (check_row[x] != 0) { has_content = 1; break; }
        }
        if (has_content) return;  /* HD asset already fills surface */
    }

    /* Also check if row 0 has content beyond column base_w */
    {
        uint16_t *first_row = (uint16_t*)s->sys_buffer;
        int has_wide_content = 0;
        DWORD x;
        for (x = base_w; x < s->width && x < base_w + 200; x += 20) {
            if (first_row[x] != 0) { has_wide_content = 1; break; }
        }
        if (has_wide_content) return;  /* Content already wider than 640 */
    }

    /* Stretch 640x480 content to fill surface (nearest-neighbor, in-place).
     * Work bottom-up to avoid overwriting source data. */
    {
        float sx = (float)base_w / (float)s->width;
        float sy = (float)base_h / (float)s->height;

        /* Pass 1: stretch vertically + horizontally from bottom-right to top-left.
         * Use a temporary row buffer to avoid source corruption. */
        uint16_t *tmp_row = (uint16_t*)malloc(s->width * 2);
        if (!tmp_row) return;

        for (y = s->height; y > 0; y--) {
            DWORD dy = y - 1;
            DWORD src_y = (DWORD)(dy * sy);
            uint16_t *src_row, *dst_row;
            DWORD x;
            if (src_y >= base_h) src_y = base_h - 1;
            src_row = (uint16_t*)((BYTE*)s->sys_buffer + src_y * s->pitch);
            dst_row = (uint16_t*)((BYTE*)s->sys_buffer + dy * s->pitch);
            /* Build stretched row in temp buffer */
            for (x = 0; x < s->width; x++) {
                DWORD src_x = (DWORD)(x * sx);
                if (src_x >= base_w) src_x = base_w - 1;
                tmp_row[x] = src_row[src_x];
            }
            memcpy(dst_row, tmp_row, s->width * 2);
        }
        free(tmp_row);
    }

    WRAPPER_LOG("fe_auto_stretch: %p stretched 640x480 -> %ux%u", s, s->width, s->height);
}

static HRESULT __stdcall Surface4_Unlock(WrapperSurface *self, RECT *rect)
{
    WRAPPER_LOG("Surface4_Unlock: self=%p", self);

    if (!self->locked)
        return DDERR_GENERIC;

    /* Frontend surface: upscale virtual_lock_buffer -> sys_buffer */
    if (self->virtual_lock_buffer && g_backend.fe_scale.enabled) {
        fe_upsample_buffer(self);
        self->dirty = 1;
        self->locked = 0;
        FE_LOG(2, "Unlock frontend %p: upscaled %ux%u -> %ux%u",
               self, self->virtual_width, self->virtual_height,
               self->width, self->height);
        return DD_OK;
    }

    /* Mark dirty — the actual D3D11 texture upload happens in FlushDirty
     * (called from BeginScene or before present). */
    self->dirty = 1;
    self->locked = 0;
    return DD_OK;
}

static HRESULT __stdcall Surface4_UpdateOverlay(WrapperSurface *self, RECT *src,
    WrapperSurface *dst, RECT *dstRect, DWORD flags, void *fx)
{
    WRAPPER_STUB("Surface4_UpdateOverlay");
}

static HRESULT __stdcall Surface4_UpdateOverlayDisplay(WrapperSurface *self, DWORD flags)
{
    WRAPPER_STUB("Surface4_UpdateOverlayDisplay");
}

static HRESULT __stdcall Surface4_UpdateOverlayZOrder(WrapperSurface *self, DWORD flags, WrapperSurface *ref)
{
    WRAPPER_STUB("Surface4_UpdateOverlayZOrder");
}

static HRESULT __stdcall Surface4_GetDDInterface(WrapperSurface *self, void **ddraw)
{
    WRAPPER_LOG("Surface4_GetDDInterface: self=%p", self);

    if (!ddraw)
        return E_POINTER;

    *ddraw = g_backend.ddraw;
    return DD_OK;
}

static HRESULT __stdcall Surface4_PageLock(WrapperSurface *self, DWORD flags)
{
    WRAPPER_STUB("Surface4_PageLock");
}

static HRESULT __stdcall Surface4_PageUnlock(WrapperSurface *self, DWORD flags)
{
    WRAPPER_STUB("Surface4_PageUnlock");
}

static HRESULT __stdcall Surface4_SetSurfaceDesc(WrapperSurface *self, DDSURFACEDESC2_W *desc, DWORD flags)
{
    WRAPPER_STUB("Surface4_SetSurfaceDesc");
}

static HRESULT __stdcall Surface4_SetPrivateData(WrapperSurface *self, REFGUID tag, void *data, DWORD size, DWORD flags)
{
    WRAPPER_STUB("Surface4_SetPrivateData");
}

static HRESULT __stdcall Surface4_GetPrivateData(WrapperSurface *self, REFGUID tag, void *data, DWORD *size)
{
    WRAPPER_STUB("Surface4_GetPrivateData");
}

static HRESULT __stdcall Surface4_FreePrivateData(WrapperSurface *self, REFGUID tag)
{
    WRAPPER_STUB("Surface4_FreePrivateData");
}

static HRESULT __stdcall Surface4_GetUniquenessValue(WrapperSurface *self, DWORD *value)
{
    WRAPPER_STUB("Surface4_GetUniquenessValue");
}

static HRESULT __stdcall Surface4_ChangeUniquenessValue(WrapperSurface *self)
{
    WRAPPER_STUB("Surface4_ChangeUniquenessValue");
}

/* ========================================================================
 * Construction helpers
 * ======================================================================== */

/**
 * Ensure the system memory buffer is allocated for Lock/Unlock.
 */
void WrapperSurface_EnsureSysBuffer(WrapperSurface *s)
{
    if (s->sys_buffer)
        return;

    s->pitch = s->width * (s->bpp / 8);
    s->sys_buffer = calloc(s->height, (size_t)s->pitch);
    WRAPPER_LOG("WrapperSurface_EnsureSysBuffer: %p allocated %ux%u pitch=%d",
                s, s->width, s->height, s->pitch);
}

/**
 * Upload dirty sys_buffer content to the D3D11 texture via UpdateSubresource.
 * Called before D3D rendering (BeginScene) and before present (Blt to primary).
 */
void WrapperSurface_FlushDirty(WrapperSurface *s)
{
    if (!s) return;
    if (!s->dirty) return;
    if (!s->sys_buffer) return;

    /* Debug: check if render target sys_buffer has any non-zero data */
    {
        static int s_logged = 0;
        if (s_logged < 3 && s->bpp == 16) {
            uint16_t *px = (uint16_t*)s->sys_buffer;
            int total = (int)(s->width * s->height);
            int nonzero = 0, i;
            for (i = 0; i < total; i++) { if (px[i] != 0) nonzero++; }
            {
                uint16_t *p = (uint16_t*)s->sys_buffer;
                WRAPPER_LOG("FlushDirty: surf=%p nonzero=%d/%d px[0..3]=%04X %04X %04X %04X dirty_before=%d",
                    s, nonzero, total, p[0], p[1], p[2], p[3], s->dirty);
            }
            s_logged++;
        }
    }

    /* Auto-stretch disabled: frontend scaling hooks in ASI handle element
     * positioning. Background TGAs stay at 640x480 pixel content in the
     * native-res surface; the present quad with POINT filter upscales. */

    if (s->d3d11_texture && s->sys_buffer && g_backend.context) {
        if (s->bpp == 16 && s->dxgi_format == DXGI_FORMAT_B8G8R8A8_UNORM) {
            /* Convert R5G6B5 16bpp sys_buffer → B8G8R8A8 32bpp for upload */
            DWORD row32 = s->width * 4;
            BYTE *buf32 = (BYTE*)malloc(row32 * s->height);
            if (buf32) {
                DWORD y;
                for (y = 0; y < s->height; y++) {
                    uint16_t *src16 = (uint16_t*)((BYTE*)s->sys_buffer + y * s->pitch);
                    uint32_t *dst32 = (uint32_t*)(buf32 + y * row32);
                    DWORD x;
                    for (x = 0; x < s->width; x++) {
                        uint16_t c = src16[x];
                        uint32_t r = ((c >> 11) & 0x1F) * 255 / 31;
                        uint32_t g = ((c >> 5)  & 0x3F) * 255 / 63;
                        uint32_t b = ((c)       & 0x1F) * 255 / 31;
                        dst32[x] = 0xFF000000 | (r << 16) | (g << 8) | b;
                    }
                }
                ID3D11DeviceContext_UpdateSubresource(g_backend.context,
                    (ID3D11Resource*)s->d3d11_texture, 0, NULL,
                    buf32, row32, 0);
                free(buf32);
            }
        } else {
            /* Use staging texture for reliable upload (respects GPU row pitch alignment) */
            D3D11_TEXTURE2D_DESC td;
            ID3D11Texture2D *staging = NULL;
            ZeroMemory(&td, sizeof(td));
            td.Width = s->width;
            td.Height = s->height;
            td.MipLevels = 1;
            td.ArraySize = 1;
            td.Format = s->dxgi_format;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_STAGING;
            td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            if (SUCCEEDED(ID3D11Device_CreateTexture2D(g_backend.device, &td, NULL, &staging)) && staging) {
                D3D11_MAPPED_SUBRESOURCE mapped;
                if (SUCCEEDED(ID3D11DeviceContext_Map(g_backend.context,
                        (ID3D11Resource*)staging, 0, D3D11_MAP_WRITE, 0, &mapped))) {
                    DWORD bpp_bytes = s->bpp / 8;
                    DWORD row_bytes = s->width * bpp_bytes;
                    DWORD y;
                    for (y = 0; y < s->height; y++) {
                        CopyMemory((BYTE*)mapped.pData + y * mapped.RowPitch,
                                   (BYTE*)s->sys_buffer + y * s->pitch,
                                   row_bytes);
                    }
                    ID3D11DeviceContext_Unmap(g_backend.context, (ID3D11Resource*)staging, 0);
                    ID3D11DeviceContext_CopyResource(g_backend.context,
                        (ID3D11Resource*)s->d3d11_texture, (ID3D11Resource*)staging);
                }
                ID3D11Texture2D_Release(staging);
            }
        }
        WRAPPER_LOG("FlushDirty: upload surf=%p %ux%u pitch=%d fmt=%d",
            s, s->width, s->height, s->pitch, s->dxgi_format);
    }

    s->dirty = 0;
}

/**
 * Determine the DXGI format from bpp and pixel format.
 */
DXGI_FORMAT WrapperSurface_GetDXGIFormat(DWORD bpp, DWORD flags, DDPIXELFORMAT_W *pf)
{
    (void)pf; (void)bpp;
    if (flags & DDPF_ZBUFFER)
        return DXGI_FORMAT_D16_UNORM;
    /* Force ALL surfaces to B8G8R8A8_UNORM — universally supported. */
    return DXGI_FORMAT_B8G8R8A8_UNORM;

    /* Dead code below kept for reference */
    if (bpp == 32) {
        if (pf && (pf->dwFlags & DDPF_ALPHAPIXELS))
            return DXGI_FORMAT_B8G8R8A8_UNORM;
        return DXGI_FORMAT_B8G8R8X8_UNORM;
    }

    return DXGI_FORMAT_B8G8R8A8_UNORM;
}

/**
 * Create a new WrapperSurface with the given dimensions.
 */
WrapperSurface* WrapperSurface_Create(DWORD width, DWORD height, DWORD bpp, DWORD caps)
{
    WrapperSurface *s = (WrapperSurface*)calloc(1, sizeof(WrapperSurface));
    if (!s) return NULL;

    s->vtbl      = &s_surface4_vtbl;
    s->ref_count = 1;
    s->width     = width;
    s->height    = height;
    s->bpp       = bpp;
    s->caps      = caps;
    s->pitch     = width * (bpp / 8);

    /* Frontend surface scaling fields — initialized but NOT activated yet.
     * The game's dirty-rect save/restore system, button compositing, and
     * background Lock/Unlock all assume the surface is the same size as the
     * game's virtual resolution. Scaling the surface breaks these systems.
     *
     * Instead, surfaces stay at 640x480. The StretchRect at present time
     * (Blt to primary / compositing path) handles the upscale to native res.
     * The fe_scale infrastructure is kept for future per-element scaling. */
    s->virtual_width       = 0;
    s->virtual_height      = 0;
    s->virtual_pitch       = 0;
    s->virtual_lock_buffer = NULL;
    s->is_frontend_surface = 0;

    /* Set up pixel format */
    s->pixel_format.dwSize  = sizeof(DDPIXELFORMAT_W);

    if (caps & DDSCAPS_ZBUFFER) {
        /* Z-buffer: use DDPF_ZBUFFER format, not RGB */
        s->pixel_format.dwFlags = DDPF_ZBUFFER;
        s->pixel_format.dwZBufferBitDepth = bpp;
        /* Z-bit masks */
        if (bpp == 16) s->pixel_format.dwZBitMask = 0xFFFF;
        else if (bpp == 32) s->pixel_format.dwZBitMask = 0xFFFFFFFF;
        s->pitch = width * (bpp / 8);
    } else {
        s->pixel_format.dwFlags = DDPF_RGB;
        s->pixel_format.dwRGBBitCount = bpp;

        if (bpp == 16) {
            /* RGB565 */
            s->pixel_format.dwRBitMask = 0xF800;
            s->pixel_format.dwGBitMask = 0x07E0;
            s->pixel_format.dwBBitMask = 0x001F;
        } else if (bpp == 32) {
            /* XRGB8888 */
            s->pixel_format.dwRBitMask = 0x00FF0000;
            s->pixel_format.dwGBitMask = 0x0000FF00;
            s->pixel_format.dwBBitMask = 0x000000FF;
        }
    }

    /* Create D3D11 backing if we have a device and this is a vidmem surface */
    if (g_backend.device && !(caps & DDSCAPS_SYSTEMMEMORY)) {
        DXGI_FORMAT fmt = WrapperSurface_GetDXGIFormat(bpp, s->pixel_format.dwFlags, &s->pixel_format);
        s->dxgi_format = fmt;

        if (caps & DDSCAPS_ZBUFFER) {
            /* Z-buffer: depth is managed by the backend (depth_dsv).
             * No per-surface D3D11 resource needed. */
            WRAPPER_LOG("WrapperSurface_Create: Z-buffer (depth managed by backend)");
        } else if ((caps & DDSCAPS_TEXTURE) || (caps & DDSCAPS_VIDEOMEMORY)) {
            /* Texture or video-memory surface: create texture + SRV */
            D3D11_TEXTURE2D_DESC td;
            ZeroMemory(&td, sizeof(td));
            td.Width            = width;
            td.Height           = height;
            td.MipLevels        = 1;
            td.ArraySize        = 1;
            td.Format           = fmt;
            td.SampleDesc.Count = 1;
            td.Usage            = D3D11_USAGE_DEFAULT;
            td.BindFlags        = D3D11_BIND_SHADER_RESOURCE;

            HRESULT hr = ID3D11Device_CreateTexture2D(g_backend.device,
                &td, NULL, &s->d3d11_texture);
            if (SUCCEEDED(hr)) {
                ID3D11Device_CreateShaderResourceView(g_backend.device,
                    (ID3D11Resource*)s->d3d11_texture, NULL, &s->d3d11_srv);
            }
        } else if (caps & DDSCAPS_OFFSCREENPLAIN) {
            if (caps & DDSCAPS_3DDEVICE) {
                /* Render target surface: D3D renders to this.
                 * Create texture + SRV + RTV, and set as render target. */
                D3D11_TEXTURE2D_DESC td;
                ZeroMemory(&td, sizeof(td));
                td.Width            = width;
                td.Height           = height;
                td.MipLevels        = 1;
                td.ArraySize        = 1;
                td.Format           = fmt;
                td.SampleDesc.Count = 1;
                td.Usage            = D3D11_USAGE_DEFAULT;
                td.BindFlags        = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

                HRESULT hr = ID3D11Device_CreateTexture2D(g_backend.device,
                    &td, NULL, &s->d3d11_texture);
                if (SUCCEEDED(hr)) {
                    ID3D11Device_CreateShaderResourceView(g_backend.device,
                        (ID3D11Resource*)s->d3d11_texture, NULL, &s->d3d11_srv);
                    ID3D11Device_CreateRenderTargetView(g_backend.device,
                        (ID3D11Resource*)s->d3d11_texture, NULL, &s->d3d11_rtv);

                    /* Create a staging texture for CPU upload (FlushDirty) */
                    {
                        D3D11_TEXTURE2D_DESC stg;
                        ZeroMemory(&stg, sizeof(stg));
                        stg.Width            = width;
                        stg.Height           = height;
                        stg.MipLevels        = 1;
                        stg.ArraySize        = 1;
                        stg.Format           = fmt;
                        stg.SampleDesc.Count = 1;
                        stg.Usage            = D3D11_USAGE_STAGING;
                        stg.CPUAccessFlags   = D3D11_CPU_ACCESS_WRITE;
                        ID3D11Device_CreateTexture2D(g_backend.device,
                            &stg, NULL, &s->d3d11_staging);
                    }

                    /* Set as the device render target */
                    ID3D11DeviceContext_OMSetRenderTargets(g_backend.context,
                        1, &s->d3d11_rtv, g_backend.depth_dsv);
                    WRAPPER_LOG("WrapperSurface_Create: set as D3D11 RT + SRV + staging");
                }
            } else {
                /* Plain offscreen surface — create a default texture for GPU use */
                D3D11_TEXTURE2D_DESC td;
                ZeroMemory(&td, sizeof(td));
                td.Width            = width;
                td.Height           = height;
                td.MipLevels        = 1;
                td.ArraySize        = 1;
                td.Format           = fmt;
                td.SampleDesc.Count = 1;
                td.Usage            = D3D11_USAGE_DEFAULT;
                td.BindFlags        = D3D11_BIND_SHADER_RESOURCE;

                HRESULT hr = ID3D11Device_CreateTexture2D(g_backend.device,
                    &td, NULL, &s->d3d11_texture);
                if (SUCCEEDED(hr)) {
                    ID3D11Device_CreateShaderResourceView(g_backend.device,
                        (ID3D11Resource*)s->d3d11_texture, NULL, &s->d3d11_srv);
                }
            }
        } else if (caps & DDSCAPS_PRIMARYSURFACE) {
            /* Primary surface: no D3D11 backing — swap chain managed by backend */
            WRAPPER_LOG("WrapperSurface_Create: primary surface (swap chain managed by backend)");
        }
    }

    /* If we created D3D11 backing, mark the surface as video memory.
     * M2DX checks this flag to determine if the render target is in VRAM. */
    if (s->d3d11_texture || s->d3d11_rtv) {
        s->caps |= DDSCAPS_VIDEOMEMORY;
    }

    WRAPPER_LOG("WrapperSurface_Create: %p %ux%u %ubpp caps=0x%08x tex=%p srv=%p rtv=%p",
                s, width, height, bpp, s->caps, s->d3d11_texture, s->d3d11_srv, s->d3d11_rtv);

    return s;
}

/* Backend_EnsureCompositingTextures, upload_bltfast_to_texture,
 * draw_fullscreen_quad, and Backend_CompositeAndPresent are now
 * implemented in d3d11_backend.c */
