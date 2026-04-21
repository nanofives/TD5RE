/**
 * wrapper.h - D3D11 Wrapper for DirectDraw/Direct3D 6
 *
 * Replaces DDrawCompat's ddraw.dll with a custom implementation that
 * translates all DirectDraw/D3D6 COM calls to Direct3D 11 internally.
 * This eliminates DirectDraw from the process, preventing Windows from
 * applying DWM8And16BitMitigation (classic window borders).
 *
 * Architecture:
 *   Game (TD5_d3d.exe) -> M2DX.dll -> Our COM objects -> D3D11 -> GPU
 *
 * Key design:
 *   - Pre-transformed vertices (XYZRHW) converted to NDC in vertex shader
 *   - All rendering uses HLSL shaders (no fixed-function pipeline in D3D11)
 *   - Single texture unit
 *   - No stencil buffer
 *   - ~40 COM methods need real implementation
 */

#ifndef TD5_D3D11_WRAPPER_H
#define TD5_D3D11_WRAPPER_H

#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <stdint.h>
#include <stdio.h>

/* ========================================================================
 * Debug logging
 * ======================================================================== */

/* Runtime gate. Default 1 (matches historic behavior). Call from main.c after
 * INI parse to silence the wrapper before Backend_Init opens log/wrapper.log.
 * Prototype is unconditional so callers compiled without WRAPPER_DEBUG (e.g.
 * main.c, td5_platform_win32.c) can still link against the wrapper archive,
 * which is always built with WRAPPER_DEBUG defined. */
void wrapper_set_enabled(int enabled);

#ifdef WRAPPER_DEBUG
void wrapper_log(const char *fmt, ...);
#define WRAPPER_LOG(fmt, ...) wrapper_log(fmt, ##__VA_ARGS__)
#else
#define WRAPPER_LOG(fmt, ...) ((void)0)
#endif

#define WRAPPER_STUB(name) \
    WRAPPER_LOG("STUB: %s", name); return S_OK

/* ========================================================================
 * Forward declarations for our COM objects
 * ======================================================================== */

typedef struct WrapperDirectDraw     WrapperDirectDraw;
typedef struct WrapperSurface        WrapperSurface;
typedef struct WrapperD3D            WrapperD3D;
typedef struct WrapperDevice         WrapperDevice;
typedef struct WrapperViewport       WrapperViewport;
typedef struct WrapperTexture        WrapperTexture;
typedef struct WrapperClipper        WrapperClipper;

/* ========================================================================
 * Frontend native-resolution scaling
 *
 * The game renders frontend UI at 640x480. This struct enables transparent
 * upscaling: surfaces are created at native resolution, coordinates are
 * scaled in BltFast/Blt, and Lock/Unlock provides a virtual 640x480
 * buffer while maintaining native-res backing storage.
 * ======================================================================== */

typedef struct {
    int     enabled;        /* 1 = frontend native-res active */
    int     native_w;       /* Actual surface width (e.g. 1920) */
    int     native_h;       /* Actual surface height (e.g. 1440) */
    int     virtual_w;      /* Game's virtual width (always 640) */
    int     virtual_h;      /* Game's virtual height (always 480) */
    float   scale_x;        /* native_w / virtual_w */
    float   scale_y;        /* native_h / virtual_h */
    int     log_level;      /* 0=off, 1=summary, 2=per-frame, 3=per-blit */
    int     adjust_x;       /* Global X offset in native pixels (calibration) */
    int     adjust_y;       /* Global Y offset in native pixels (calibration) */
    int     filter_mode;    /* 0=nearest, 1=bilinear (for sprite upscale) */
} FrontendScale;

/* ========================================================================
 * Render state cache
 *
 * D3D11 uses immutable state objects instead of per-call SetRenderState.
 * We cache the current D3D6 render state values and create/bind the
 * appropriate D3D11 state objects when state changes.
 * ======================================================================== */

/* Blend state indices (pre-created at device init) */
#define BLEND_OPAQUE             0  /* No blending */
#define BLEND_SRCALPHA_INVSRC    1  /* Standard alpha blend */
#define BLEND_SRCALPHA_ONE       2  /* Additive with source alpha */
#define BLEND_ONE_ONE            3  /* Full additive */
#define BLEND_SRCALPHA_SRCALPHA  4  /* src*srcA + dst*srcA (TD5 transparency) */
#define BLEND_INVSRC_INVSRC      5  /* src*(1-srcA) + dst*(1-srcA) (M2DX baseline) */
#define BLEND_STATE_COUNT        6

/* Depth-stencil state indices: [z_enable][z_write][z_func_always] */
#define DS_Z_ON_WRITE_ON         0  /* Z on, write on, LESSEQUAL */
#define DS_Z_ON_WRITE_OFF        1  /* Z on, write off, LESSEQUAL */
#define DS_Z_OFF_WRITE_OFF       2  /* Z off, write off */
#define DS_Z_OFF_WRITE_ON        3  /* Z off, write on */
#define DS_Z_ON_WRITE_ON_ALWAYS  4  /* Z on, write on, ALWAYS */
#define DS_Z_ON_WRITE_OFF_ALWAYS 5  /* Z on, write off, ALWAYS */
#define DS_STATE_COUNT           6

/* Sampler state indices */
#define SAMP_POINT_WRAP          0
#define SAMP_POINT_CLAMP         1
#define SAMP_LINEAR_WRAP         2
#define SAMP_LINEAR_CLAMP        3
#define SAMP_STATE_COUNT         4

/* Pixel shader selection */
#define PS_MODULATE              0  /* tex * diffuse, alpha = tex */
#define PS_MODULATE_ALPHA        1  /* tex * diffuse, alpha = tex * diffuse */
#define PS_DECAL                 2  /* tex only */
#define PS_LUMINANCE_ALPHA       3  /* tex * diffuse, alpha = luminance */
#define PS_COUNT                 4

typedef struct {
    /* D3D6 render state values (as received from game) */
    int     z_enable;
    int     z_write;
    int     blend_enable;
    DWORD   src_blend;      /* D3D6 blend factor */
    DWORD   dest_blend;     /* D3D6 blend factor */
    int     texblend_mode;  /* D3DTBLEND_DECAL / MODULATE / MODULATEALPHA */
    int     alpha_test_enable;
    DWORD   alpha_ref;
    int     z_func;         /* 0=default(LESSEQUAL), 1=ALWAYS */

    /* Fog state */
    int     fog_enable;
    DWORD   fog_color;      /* D3DCOLOR */
    float   fog_start;
    float   fog_end;
    float   fog_density;
    int     fog_mode;       /* 1=linear, 2=exp, 3=exp2 */

    /* Sampler state */
    DWORD   mag_filter;     /* 1=POINT, 2=LINEAR */
    DWORD   min_filter;
    DWORD   tex_address_u;  /* 1=WRAP, 3=CLAMP */
    DWORD   tex_address_v;

    /* Currently bound state objects */
    int     current_blend_idx;
    int     current_ds_idx;
    int     current_samp_idx;
    int     current_ps_idx;

    /* Dirty flag — set when any state changes, cleared after binding */
    int     dirty;
} RenderStateCache;

/* Viewport constant buffer (updated when viewport changes) */
typedef struct {
    float   viewportWidth;
    float   viewportHeight;
    float   _pad[2];
} ViewportCB;

/* Fog + alpha test constant buffer (updated when fog/alpha state changes) */
typedef struct {
    float   fogColor[4];    /* RGB + unused A */
    float   fogStart;
    float   fogEnd;
    float   fogDensity;
    int     fogEnabled;
    int     alphaTestEnabled;
    float   alphaRef;       /* 0..1 range */
    float   _pad1;
    float   _pad2;
} FogCB;

/* ========================================================================
 * Global D3D11 backend state (single-instance, game is single-threaded)
 * ======================================================================== */

typedef struct {
    /* D3D11 core */
    ID3D11Device            *device;
    ID3D11DeviceContext      *context;
    IDXGISwapChain          *swap_chain;
    IDXGIFactory1           *dxgi_factory;

    /* Render targets */
    ID3D11RenderTargetView  *swap_rtv;      /* Swap chain back buffer RTV */
    ID3D11Texture2D         *depth_tex;     /* Depth buffer texture */
    ID3D11DepthStencilView  *depth_dsv;     /* Depth buffer DSV */

    /* Window */
    HWND                hwnd;
    int                 windowed;
    int                 width;
    int                 height;
    int                 bpp;

    /* Our wrapper objects */
    WrapperDirectDraw   *ddraw;
    WrapperD3D          *d3d;
    WrapperDevice       *d3ddevice;

    /* Surfaces */
    WrapperSurface      *primary;      /* Primary (front) surface */
    WrapperSurface      *backbuffer;   /* Back buffer (render target for D3D) */
    WrapperSurface      *zbuffer;      /* Z-buffer surface */
    WrapperSurface      *bltfast_surface; /* SYSMEM surface that receives BltFast writes */

    /* Compositing: BltFast content uploaded to GPU for present-time merge */
    ID3D11Texture2D             *composite_tex;     /* GPU texture for BltFast content */
    ID3D11ShaderResourceView    *composite_srv;     /* SRV for composite_tex */
    ID3D11Texture2D             *composite_staging;  /* Staging for CPU upload */
    int                          composite_w;
    int                          composite_h;

    /* Texture state */
    ID3D11ShaderResourceView    *current_srv;  /* Currently bound texture SRV */

    /* Shaders */
    ID3D11VertexShader      *vs_pretransformed;
    ID3D11VertexShader      *vs_fullscreen;
    ID3D11PixelShader       *ps_shaders[PS_COUNT];  /* Indexed by PS_* constants */
    ID3D11PixelShader       *ps_composite;           /* Fullscreen blit shader */
    ID3D11InputLayout       *input_layout;           /* TD5_FVF vertex layout */

    /* Immutable state objects (pre-created at init) */
    ID3D11BlendState        *blend_states[BLEND_STATE_COUNT];
    ID3D11DepthStencilState *ds_states[DS_STATE_COUNT];
    ID3D11RasterizerState   *rs_state;               /* CullNone, solid fill */
    ID3D11SamplerState      *sampler_states[SAMP_STATE_COUNT];

    /* Dynamic buffers for immediate-mode rendering */
    ID3D11Buffer            *dynamic_vb;    /* WRITE_DISCARD vertex buffer */
    ID3D11Buffer            *dynamic_ib;    /* WRITE_DISCARD index buffer */
    UINT                     dynamic_vb_size;
    UINT                     dynamic_ib_size;

    /* Constant buffers */
    ID3D11Buffer            *cb_viewport;   /* ViewportCB */
    ID3D11Buffer            *cb_fog;        /* FogCB */

    /* Render state cache */
    RenderStateCache        state;

    /* Render state tracking */
    int                 in_scene;       /* Between BeginScene/EndScene */
    int                 scene_rendered; /* 3D content was drawn this frame */
    DWORD               coop_flags;    /* Cooperative level flags */

    /* Transparency mode tracking */
    int                 current_tex_has_alpha;  /* Bound texture has alpha channel */
    int                 alpha_blend_enabled;    /* ALPHABLENDENABLE state */

    /* Window management */
    WNDPROC             orig_wndproc;  /* Original M2DX window proc */
    int                 target_width;  /* Desired window client width */
    int                 target_height; /* Desired window client height */

    /* Display mode enumeration */
    DXGI_MODE_DESC      *modes;
    int                 mode_count;
    int                 mode_capacity;

    /* Vertex coordinate scaling: game emits XYZRHW vertices in M2DX's internal
     * resolution (e.g. 640x480) but our RT is at native res. These scale factors
     * are applied to every pre-transformed vertex X/Y in DrawPrimitive. */
    float               vertex_scale_x;  /* native_w / m2dx_w (e.g. 2048/640 = 3.2) */
    float               vertex_scale_y;  /* native_h / m2dx_h (e.g. 1152/480 = 2.4) */
    int                 m2dx_width;      /* M2DX's internal resolution (what game thinks) */
    int                 m2dx_height;

    /* Standalone mode: skip all hardcoded address fixups (no original EXE loaded) */
    int                 standalone;

    /* Frontend native-resolution scaling */
    FrontendScale       fe_scale;
} D3D11Backend;

extern D3D11Backend g_backend;

/* ========================================================================
 * PNG texture override (png_loader.c)
 * ======================================================================== */

void PngOverride_Init(void);
void PngOverride_Shutdown(void);
const char *PngOverride_Lookup(DWORD width, DWORD height,
                               const void *pixel_data, LONG pitch);
int PngOverride_HasAlpha(const char *path);
int PngOverride_WriteToSurface(const char *path, DWORD width, DWORD height,
                               void *pixel_data, LONG pitch);
ID3D11ShaderResourceView *PngOverride_LoadToTexture(const char *path);
void PngOverride_DumpTexture(DWORD width, DWORD height,
                             const void *pixel_data, LONG pitch,
                             uint32_t crc);

/* Frontend scaling log macro — only emits when log_level >= requested level */
#ifdef WRAPPER_DEBUG
#define FE_LOG(level, fmt, ...) \
    do { if (g_backend.fe_scale.log_level >= (level)) \
        wrapper_log("[FE] " fmt, ##__VA_ARGS__); } while(0)
#else
#define FE_LOG(level, fmt, ...) ((void)0)
#endif

/* ========================================================================
 * DirectDraw constants & types needed for the COM interfaces
 * (Minimal definitions to avoid requiring ddraw.h from the SDK)
 * ======================================================================== */

/* Cooperative level flags */
#define DDSCL_NORMAL            0x00000008
#define DDSCL_FULLSCREEN        0x00000001
#define DDSCL_EXCLUSIVE         0x00000010
#define DDSCL_ALLOWMODEX        0x00000040
#define DDSCL_ALLOWREBOOT       0x00000002

/* Surface caps */
#define DDSCAPS_PRIMARYSURFACE  0x00000200
#define DDSCAPS_BACKBUFFER      0x00000004
#define DDSCAPS_OFFSCREENPLAIN  0x00000040
#define DDSCAPS_FLIP            0x00000010
#define DDSCAPS_COMPLEX         0x00000008
#define DDSCAPS_VIDEOMEMORY     0x00004000
#define DDSCAPS_SYSTEMMEMORY    0x00000800
#define DDSCAPS_TEXTURE         0x00001000
#define DDSCAPS_3DDEVICE        0x00002000
#define DDSCAPS_ZBUFFER         0x00020000
#define DDSCAPS_MIPMAP          0x00400000
#define DDSCAPS_LOCALVIDMEM     0x10000000

/* Surface desc flags */
#define DDSD_CAPS               0x00000001
#define DDSD_HEIGHT             0x00000002
#define DDSD_WIDTH              0x00000004
#define DDSD_PITCH              0x00000008
#define DDSD_BACKBUFFERCOUNT    0x00000020
#define DDSD_ZBUFFERBITDEPTH    0x00000040
#define DDSD_PIXELFORMAT        0x00001000
#define DDSD_MIPMAPCOUNT        0x00020000
#define DDSD_LPSURFACE          0x00000800

/* Pixel format flags */
#define DDPF_RGB                0x00000040
#define DDPF_ALPHAPIXELS        0x00000001
#define DDPF_PALETTEINDEXED8    0x00000020
#define DDPF_ZBUFFER            0x00000400

/* Blt flags */
#define DDBLT_WAIT              0x01000000
#define DDBLT_COLORFILL         0x00000400
#define DDBLT_KEYSRC            0x00008000

/* BltFast flags */
#define DDBLTFAST_NOCOLORKEY    0x00000000
#define DDBLTFAST_SRCCOLORKEY   0x00000001
#define DDBLTFAST_WAIT          0x00000010

/* Flip flags */
#define DDFLIP_WAIT             0x00000001

/* Lock flags */
#define DDLOCK_WAIT             0x00000001
#define DDLOCK_READONLY         0x00000010
#define DDLOCK_NOSYSLOCK        0x00000800

/* WaitForVerticalBlank flags */
#define DDWAITVB_BLOCKBEGIN     0x00000001

/* Enumeration flags */
#define DDENUMRET_OK            1
#define DDENUMRET_CANCEL        0

/* Error codes */
#define DD_OK                   0
#define DDERR_GENERIC           E_FAIL
#define DDERR_UNSUPPORTED       0x80004001
#define DDERR_SURFACELOST       0x887601C2

/* GUIDs */
extern const GUID IID_IDirectDraw4;
extern const GUID IID_IDirectDrawSurface4;
extern const GUID IID_IDirect3D3;
extern const GUID IID_IDirect3DDevice3;
extern const GUID IID_IDirect3DViewport3;
extern const GUID IID_IDirect3DTexture2;
extern const GUID IID_IDirect3DHALDevice;
extern const GUID IID_IDirect3DRGBDevice;

/* ========================================================================
 * DirectDraw structures (subset needed by M2DX.dll)
 * ======================================================================== */

#pragma pack(push, 1)

typedef struct {
    DWORD dwSize;
    DWORD dwFlags;
    DWORD dwFourCC;
    union {
        DWORD dwRGBBitCount;
        DWORD dwYUVBitCount;
        DWORD dwZBufferBitDepth;
        DWORD dwAlphaBitDepth;
    };
    union {
        DWORD dwRBitMask;
        DWORD dwYBitMask;
        DWORD dwStencilBitDepth;
    };
    union {
        DWORD dwGBitMask;
        DWORD dwUBitMask;
        DWORD dwZBitMask;
    };
    union {
        DWORD dwBBitMask;
        DWORD dwVBitMask;
        DWORD dwStencilBitMask;
    };
    union {
        DWORD dwRGBAlphaBitMask;
        DWORD dwYUVAlphaBitMask;
    };
} DDPIXELFORMAT_W;

typedef struct {
    DWORD dwCaps;
    DWORD dwCaps2;
    DWORD dwCaps3;
    DWORD dwCaps4;
} DDSCAPS2_W;

typedef struct {
    DWORD           dwSize;
    DWORD           dwFlags;
    DWORD           dwHeight;
    DWORD           dwWidth;
    union {
        LONG        lPitch;
        DWORD       dwLinearSize;
    };
    DWORD           dwBackBufferCount;
    union {
        DWORD       dwMipMapCount;
        DWORD       dwZBufferBitDepth;
        DWORD       dwRefreshRate;
    };
    DWORD           dwAlphaBitDepth;
    DWORD           dwReserved;
    void           *lpSurface;
    DWORD           ddckCKDestOverlay_low;
    DWORD           ddckCKDestOverlay_high;
    DWORD           ddckCKDestBlt_low;
    DWORD           ddckCKDestBlt_high;
    DWORD           ddckCKSrcOverlay_low;
    DWORD           ddckCKSrcOverlay_high;
    DWORD           ddckCKSrcBlt_low;
    DWORD           ddckCKSrcBlt_high;
    DDPIXELFORMAT_W ddpfPixelFormat;
    DDSCAPS2_W      ddsCaps;
    DWORD           dwTextureStage;
} DDSURFACEDESC2_W;

typedef struct {
    DWORD dwSize;
    /* Remaining fields of DDBLTFX (100 bytes total) */
    DWORD dwDDFX;
    DWORD dwROP;
    DWORD dwDDROP;
    DWORD dwRotationAngle;
    DWORD dwZBufferOpCode;
    DWORD dwZBufferLow;
    DWORD dwZBufferHigh;
    DWORD dwZBufferBaseDest;
    DWORD dwZDestConstBitDepth;
    union {
        DWORD dwZDestConst;
        void *lpDDSZBufferDest;
    };
    DWORD dwZSrcConstBitDepth;
    union {
        DWORD dwZSrcConst;
        void *lpDDSZBufferSrc;
    };
    DWORD dwAlphaEdgeBlendBitDepth;
    DWORD dwAlphaEdgeBlend;
    DWORD dwReserved;
    DWORD dwAlphaDestConstBitDepth;
    union {
        DWORD dwAlphaDestConst;
        void *lpDDSAlphaDest;
    };
    DWORD dwAlphaSrcConstBitDepth;
    union {
        DWORD dwAlphaSrcConst;
        void *lpDDSAlphaSrc;
    };
    union {
        DWORD dwFillColor;
        DWORD dwFillDepth;
        DWORD dwFillPixel;
        void *lpDDSPattern;
    };
    /* DDCOLORKEY */
    DWORD ddckDestColorkey_low;
    DWORD ddckDestColorkey_high;
    DWORD ddckSrcColorkey_low;
    DWORD ddckSrcColorkey_high;
} DDBLTFX_W;

#pragma pack(pop)

/* Callback types */
typedef HRESULT (WINAPI *LPDDENUMMODESCALLBACK2)(DDSURFACEDESC2_W*, void*);
typedef HRESULT (WINAPI *LPDDENUMCALLBACKA)(GUID*, char*, char*, void*);
typedef HRESULT (WINAPI *LPDDENUMCALLBACKEXA)(GUID*, char*, char*, void*, HMONITOR);
typedef HRESULT (CALLBACK *LPD3DENUMDEVICESCALLBACK3)(GUID*, char*, char*, void*, void*, void*);
typedef HRESULT (CALLBACK *LPD3DENUMPIXELFORMATSCALLBACK)(DDPIXELFORMAT_W*, void*);

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

/* ========================================================================
 * COM interface function prototypes
 * ======================================================================== */

/* Backend management */
int  Backend_Init(void);
void Backend_Shutdown(void);
int  Backend_CreateDevice(HWND hwnd, int width, int height, int bpp, int windowed);
int  Backend_Reset(int width, int height, int bpp, int windowed);
void Backend_EnumerateModes(void);

/* Object creation */
WrapperDirectDraw* WrapperDirectDraw_Create(void);
WrapperSurface*    WrapperSurface_Create(DWORD width, DWORD height, DWORD bpp, DWORD caps);
WrapperD3D*        WrapperD3D_Create(WrapperDirectDraw *ddraw);
WrapperDevice*     WrapperDevice_Create(WrapperSurface *render_target);
WrapperViewport*   WrapperViewport_Create(void);
WrapperTexture*    WrapperTexture_Create(WrapperSurface *surface);
WrapperClipper*    WrapperClipper_Create(void);

/* Surface helpers */
DXGI_FORMAT WrapperSurface_GetDXGIFormat(DWORD bpp, DWORD flags, DDPIXELFORMAT_W *pf);
void        WrapperSurface_EnsureSysBuffer(WrapperSurface *s);
void        WrapperSurface_FlushDirty(WrapperSurface *s);

/* Compositing: merge BltFast (2D) and D3D (3D) layers at present time */
void      Backend_EnsureCompositingTextures(int width, int height);
void      Backend_CompositeAndPresent(WrapperSurface *rt_surface, RECT *srcRect, RECT *dstRect);

/* Fullscreen quad rendering (for present blit and compositing) */
void      Backend_DrawFullscreenQuad(ID3D11ShaderResourceView *srv);

/* Windowed mode: display window management */
void Backend_EnforceWindowSize(void);
void Backend_UpdateMousePosition(void);
HWND Backend_GetDisplayWindow(void);

/* Render state management */
void Backend_ApplyStateCache(void);  /* Bind D3D11 state objects from cache */
void Backend_SelectPixelShader(void); /* Choose PS based on texblend + alpha + tex format */
void Backend_UpdateFogCB(void);      /* Upload fog constant buffer */
void Backend_UpdateViewportCB(float w, float h); /* Upload viewport constant buffer */

#endif /* TD5_D3D11_WRAPPER_H */
