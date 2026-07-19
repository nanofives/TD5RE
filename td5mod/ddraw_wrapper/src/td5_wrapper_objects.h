/**
 * td5_wrapper_objects.h -- D3D6 render-state type constants, the D3D6
 * viewport structure, and the WrapperSurface/WrapperViewport/WrapperTexture/
 * WrapperClipper COM object structs + vtables (replacing IDirectDrawSurface4/
 * IDirect3DViewport3/IDirect3DTexture2/IDirectDrawClipper). Split out of
 * wrapper.h (2026-07-09, A3 refactor). Included from wrapper.h only -- not
 * meant to be included directly (needs wrapper.h's forward declarations of
 * the Wrapper* COM object types and td5_wrapper_ddraw_types.h's DDraw
 * structures, both included first).
 */

#ifndef TD5_WRAPPER_OBJECTS_H
#define TD5_WRAPPER_OBJECTS_H

/* ========================================================================
 * D3D6 render state types (used by IDirect3DDevice3::SetRenderState)
 * ======================================================================== */

/* Primitive types (same values as D3D11_PRIMITIVE_TOPOLOGY minus 1) */
#define D3DPT_POINTLIST     1
#define D3DPT_LINELIST      2
#define D3DPT_LINESTRIP     3
#define D3DPT_TRIANGLELIST  4
#define D3DPT_TRIANGLESTRIP 5
#define D3DPT_TRIANGLEFAN   6

/* TD5 vertex stride: XYZRHW(16) + DIFFUSE(4) + SPECULAR(4) + TEX1(8) = 32 bytes */
#define TD5_VERTEX_STRIDE   32

/* D3D6 render state types (map to D3D11 state objects) */
/* Values match D3DRENDERSTATETYPE in d3dtypes.h (DirectX 6 SDK). */
#define D3DRS6_ZENABLE            7
#define D3DRS6_FILLMODE           8
#define D3DRS6_SHADEMODE          9
#define D3DRS6_ZWRITEENABLE      14
#define D3DRS6_SRCBLEND          19
#define D3DRS6_DESTBLEND         20
#define D3DRS6_TEXTUREMAPBLEND   21
#define D3DRS6_CULLMODE          22
#define D3DRS6_ZFUNC             23
#define D3DRS6_DITHERENABLE      26
#define D3DRS6_ALPHABLENDENABLE  27
#define D3DRS6_FOGENABLE         28
#define D3DRS6_SPECULARENABLE    29
#define D3DRS6_STIPPLEDALPHA     33
#define D3DRS6_FOGCOLOR          34
#define D3DRS6_FOGTABLEMODE      35
#define D3DRS6_FOGSTART          36
#define D3DRS6_FOGEND            37
#define D3DRS6_FOGDENSITY        38
#define D3DRS6_COLORKEYENABLE    41
#define D3DRS6_TEXADDRESSU       44
#define D3DRS6_TEXADDRESSV       45

/* D3D6 texture stage state types */
#define D3DTSS6_MAGFILTER        16
#define D3DTSS6_MINFILTER        17
#define D3DTSS6_MIPFILTER        18
#define D3DTSS6_MIPMAPLODBIAS   19

/* D3D6 texture blend modes (for D3DRS6_TEXTUREMAPBLEND) */
#define D3DTBLEND_DECAL          1
#define D3DTBLEND_MODULATE       4
#define D3DTBLEND_MODULATEALPHA  5

/* D3D6 blend factors (same numeric values as D3D11_BLEND minus some offsets) */
#define D3D6BLEND_ZERO           1
#define D3D6BLEND_ONE            2
#define D3D6BLEND_SRCCOLOR       3
#define D3D6BLEND_INVSRCCOLOR    4
#define D3D6BLEND_SRCALPHA       5
#define D3D6BLEND_INVSRCALPHA    6
#define D3D6BLEND_DESTALPHA      7
#define D3D6BLEND_INVDESTALPHA   8
#define D3D6BLEND_DESTCOLOR      9
#define D3D6BLEND_INVDESTCOLOR  10

/* ========================================================================
 * D3D viewport structure (D3DVIEWPORT2 from D3D6)
 * ======================================================================== */

typedef struct {
    DWORD dwSize;
    DWORD dwX;
    DWORD dwY;
    DWORD dwWidth;
    DWORD dwHeight;
    float dvClipX;
    float dvClipY;
    float dvClipWidth;
    float dvClipHeight;
    float dvMinZ;
    float dvMaxZ;
} D3DVIEWPORT2_W;

/* ========================================================================
 * WrapperSurface - replaces IDirectDrawSurface4
 *
 * Each surface is backed by either:
 *   - A D3D11 texture (for 3D-renderable surfaces)
 *   - A system memory buffer (for software Lock/Unlock)
 *   - The D3D11 swap chain back buffer (for the primary flip chain)
 * ======================================================================== */

/* Forward-declare the callback type needed by the surface vtbl */
typedef HRESULT (WINAPI *LPDDENUMSURFACESCALLBACK2_W)(
    WrapperSurface *surface, DDSURFACEDESC2_W *desc, void *ctx);

typedef struct WrapperSurfaceVtbl WrapperSurfaceVtbl;

struct WrapperSurfaceVtbl {
    /* IUnknown */
    HRESULT (__stdcall *QueryInterface)(WrapperSurface*, REFIID, void**);
    ULONG   (__stdcall *AddRef)(WrapperSurface*);
    ULONG   (__stdcall *Release)(WrapperSurface*);
    /* IDirectDrawSurface4 */
    HRESULT (__stdcall *AddAttachedSurface)(WrapperSurface*, WrapperSurface*);
    HRESULT (__stdcall *AddOverlayDirtyRect)(WrapperSurface*, RECT*);
    HRESULT (__stdcall *Blt)(WrapperSurface*, RECT*, WrapperSurface*, RECT*, DWORD, DDBLTFX_W*);
    HRESULT (__stdcall *BltBatch)(WrapperSurface*, void*, DWORD, DWORD);
    HRESULT (__stdcall *BltFast)(WrapperSurface*, DWORD, DWORD, WrapperSurface*, RECT*, DWORD);
    HRESULT (__stdcall *DeleteAttachedSurface)(WrapperSurface*, DWORD, WrapperSurface*);
    HRESULT (__stdcall *EnumAttachedSurfaces)(WrapperSurface*, void*, LPDDENUMSURFACESCALLBACK2_W);
    HRESULT (__stdcall *EnumOverlayZOrders)(WrapperSurface*, DWORD, void*, void*);
    HRESULT (__stdcall *Flip)(WrapperSurface*, WrapperSurface*, DWORD);
    HRESULT (__stdcall *GetAttachedSurface)(WrapperSurface*, DDSCAPS2_W*, WrapperSurface**);
    HRESULT (__stdcall *GetBltStatus)(WrapperSurface*, DWORD);
    HRESULT (__stdcall *GetCaps)(WrapperSurface*, DDSCAPS2_W*);
    HRESULT (__stdcall *GetClipper)(WrapperSurface*, void**);
    HRESULT (__stdcall *GetColorKey)(WrapperSurface*, DWORD, void*);
    HRESULT (__stdcall *GetDC)(WrapperSurface*, HDC*);
    HRESULT (__stdcall *GetFlipStatus)(WrapperSurface*, DWORD);
    HRESULT (__stdcall *GetOverlayPosition)(WrapperSurface*, LONG*, LONG*);
    HRESULT (__stdcall *GetPalette)(WrapperSurface*, void**);
    HRESULT (__stdcall *GetPixelFormat)(WrapperSurface*, DDPIXELFORMAT_W*);
    HRESULT (__stdcall *GetSurfaceDesc)(WrapperSurface*, DDSURFACEDESC2_W*);
    HRESULT (__stdcall *Initialize)(WrapperSurface*, void*, DDSURFACEDESC2_W*);
    HRESULT (__stdcall *IsLost)(WrapperSurface*);
    HRESULT (__stdcall *Lock)(WrapperSurface*, RECT*, DDSURFACEDESC2_W*, DWORD, HANDLE);
    HRESULT (__stdcall *ReleaseDC)(WrapperSurface*, HDC);
    HRESULT (__stdcall *Restore)(WrapperSurface*);
    HRESULT (__stdcall *SetClipper)(WrapperSurface*, WrapperClipper*);
    HRESULT (__stdcall *SetColorKey)(WrapperSurface*, DWORD, void*);
    HRESULT (__stdcall *SetOverlayPosition)(WrapperSurface*, LONG, LONG);
    HRESULT (__stdcall *SetPalette)(WrapperSurface*, void*);
    HRESULT (__stdcall *Unlock)(WrapperSurface*, RECT*);
    HRESULT (__stdcall *UpdateOverlay)(WrapperSurface*, RECT*, WrapperSurface*, RECT*, DWORD, void*);
    HRESULT (__stdcall *UpdateOverlayDisplay)(WrapperSurface*, DWORD);
    HRESULT (__stdcall *UpdateOverlayZOrder)(WrapperSurface*, DWORD, WrapperSurface*);
    HRESULT (__stdcall *GetDDInterface)(WrapperSurface*, void**);
    HRESULT (__stdcall *PageLock)(WrapperSurface*, DWORD);
    HRESULT (__stdcall *PageUnlock)(WrapperSurface*, DWORD);
    HRESULT (__stdcall *SetSurfaceDesc)(WrapperSurface*, DDSURFACEDESC2_W*, DWORD);
    HRESULT (__stdcall *SetPrivateData)(WrapperSurface*, REFGUID, void*, DWORD, DWORD);
    HRESULT (__stdcall *GetPrivateData)(WrapperSurface*, REFGUID, void*, DWORD*);
    HRESULT (__stdcall *FreePrivateData)(WrapperSurface*, REFGUID);
    HRESULT (__stdcall *GetUniquenessValue)(WrapperSurface*, DWORD*);
    HRESULT (__stdcall *ChangeUniquenessValue)(WrapperSurface*);
};

struct WrapperSurface {
    WrapperSurfaceVtbl *vtbl;
    LONG                ref_count;

    /* Surface properties */
    DWORD               width;
    DWORD               height;
    DWORD               bpp;        /* Bits per pixel (16 or 32) */
    DWORD               caps;       /* DDSCAPS flags */
    DDPIXELFORMAT_W     pixel_format;

    /* D3D11 backing */
    ID3D11Texture2D            *d3d11_texture;  /* GPU texture */
    ID3D11ShaderResourceView   *d3d11_srv;      /* SRV (for shader sampling) */
    ID3D11RenderTargetView     *d3d11_rtv;      /* RTV (for render targets only) */
    ID3D11Texture2D            *d3d11_staging;  /* Staging texture for Lock/Unlock */
    DXGI_FORMAT                 dxgi_format;    /* DXGI format of the texture */

    /* [DEVICE-LOST recovery] Stamp of the g_backend.device_generation the above
     * D3D11 objects were created on. When the GPU device is recreated after a
     * TDR, g_backend.device_generation is bumped; any surface whose stamp is
     * stale has its GPU backing rebuilt from sys_buffer before next use
     * (WrapperSurface_EnsureDeviceCurrent). */
    unsigned                    device_generation;

    /* System memory backing (for Lock/Unlock) */
    void               *sys_buffer;
    LONG                pitch;
    int                 locked;
    int                 dirty;     /* sys_buffer modified by BltFast, needs upload */

    /* Linked surfaces */
    WrapperSurface     *attached_back;  /* Attached back buffer */
    WrapperSurface     *attached_zbuf;  /* Attached z-buffer */

    /* Texture wrapper (created on QueryInterface for IDirect3DTexture2) */
    WrapperTexture     *texture_wrapper;

    /* Color key */
    DWORD               colorkey_low;
    DWORD               colorkey_high;
    int                 has_colorkey;

    /* Frontend scaling support */
    DWORD               virtual_width;      /* Width game believes (e.g. 640) */
    DWORD               virtual_height;     /* Height game believes (e.g. 480) */
    LONG                virtual_pitch;      /* Pitch for virtual buffer */
    void               *virtual_lock_buffer;/* Virtual-size buffer for Lock/Unlock */
    int                 is_frontend_surface;/* This surface participates in FE scaling */
};

/* ========================================================================
 * WrapperViewport - replaces IDirect3DViewport3
 * ======================================================================== */

typedef struct WrapperViewportVtbl WrapperViewportVtbl;

struct WrapperViewport {
    WrapperViewportVtbl *vtbl;
    LONG                 ref_count;
    D3DVIEWPORT2_W       vp_data;
};

/* ========================================================================
 * WrapperTexture - replaces IDirect3DTexture2
 * ======================================================================== */

typedef struct WrapperTextureVtbl WrapperTextureVtbl;

struct WrapperTextureVtbl {
    /* IUnknown */
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(WrapperTexture *self, REFIID riid, void **ppv);
    ULONG   (STDMETHODCALLTYPE *AddRef)(WrapperTexture *self);
    ULONG   (STDMETHODCALLTYPE *Release)(WrapperTexture *self);
    /* IDirect3DTexture2 */
    HRESULT (STDMETHODCALLTYPE *GetHandle)(WrapperTexture *self, WrapperDevice *device, DWORD *handle);
    HRESULT (STDMETHODCALLTYPE *PaletteChanged)(WrapperTexture *self, DWORD start, DWORD count);
    HRESULT (STDMETHODCALLTYPE *Load)(WrapperTexture *self, WrapperTexture *srcTex);
};

struct WrapperTexture {
    WrapperTextureVtbl *vtbl;
    LONG                ref_count;
    WrapperSurface     *surface;
    ID3D11ShaderResourceView *d3d11_srv;  /* Survives surface destruction */
    int                 r5g6b5_source; /* 1 = data was R5G6B5 (needs luminance alpha) */
};

/* ========================================================================
 * WrapperClipper - replaces IDirectDrawClipper
 * ======================================================================== */

struct WrapperClipper {
    void *vtbl;
    LONG  ref_count;
    HWND  hwnd;
};

#endif /* TD5_WRAPPER_OBJECTS_H */
