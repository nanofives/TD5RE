/**
 * d3d3.c - IDirect3D3 COM wrapper implementation
 *
 * Translates IDirect3D3 (DirectX 6) calls to our D3D11 backend.
 * Vtable inherits IDirect3D2 -> IDirect3D -> IUnknown.
 *
 * TD5 uses this interface to enumerate devices, create the D3D device,
 * enumerate Z-buffer formats, and create viewports.
 */

#include "wrapper.h"

/* ========================================================================
 * D3DDEVICEDESC_V1 - Hardware/software capability descriptor
 *
 * This is the D3DDEVICEDESC struct from DirectX 6. It's 252 bytes and
 * contains capability flags that M2DX.dll inspects to decide which
 * rendering features to enable.
 * ======================================================================== */

#pragma pack(push, 1)

typedef struct {
    DWORD   dwSize;
    DWORD   dwFlags;                /* D3DDD_* flags indicating valid fields */
    DWORD   dwColorModel;           /* D3DCOLORMODEL (1=RGB, 2=mono) */
    DWORD   dwDevCaps;              /* Device capabilities */
    /* D3DTRANSFORMCAPS */
    DWORD   dtcTransformCaps_dwSize;
    DWORD   dtcTransformCaps_dwCaps;
    /* bClipping */
    BOOL    bClipping;
    /* D3DLIGHTINGCAPS */
    DWORD   dlcLightingCaps_dwSize;
    DWORD   dlcLightingCaps_dwCaps;
    DWORD   dlcLightingCaps_dwLightingModel;
    DWORD   dlcLightingCaps_dwNumLights;
    /* D3DPRIMCAPS for line primitives */
    DWORD   dpcLineCaps_dwSize;
    DWORD   dpcLineCaps_dwMiscCaps;
    DWORD   dpcLineCaps_dwRasterCaps;
    DWORD   dpcLineCaps_dwZCmpCaps;
    DWORD   dpcLineCaps_dwSrcBlendCaps;
    DWORD   dpcLineCaps_dwDestBlendCaps;
    DWORD   dpcLineCaps_dwAlphaCmpCaps;
    DWORD   dpcLineCaps_dwShadeCaps;
    DWORD   dpcLineCaps_dwTextureCaps;
    DWORD   dpcLineCaps_dwTextureFilterCaps;
    DWORD   dpcLineCaps_dwTextureBlendCaps;
    DWORD   dpcLineCaps_dwTextureAddressCaps;
    DWORD   dpcLineCaps_dwStippleWidth;
    DWORD   dpcLineCaps_dwStippleHeight;
    /* D3DPRIMCAPS for triangle primitives */
    DWORD   dpcTriCaps_dwSize;
    DWORD   dpcTriCaps_dwMiscCaps;
    DWORD   dpcTriCaps_dwRasterCaps;
    DWORD   dpcTriCaps_dwZCmpCaps;
    DWORD   dpcTriCaps_dwSrcBlendCaps;
    DWORD   dpcTriCaps_dwDestBlendCaps;
    DWORD   dpcTriCaps_dwAlphaCmpCaps;
    DWORD   dpcTriCaps_dwShadeCaps;
    DWORD   dpcTriCaps_dwTextureCaps;
    DWORD   dpcTriCaps_dwTextureFilterCaps;
    DWORD   dpcTriCaps_dwTextureBlendCaps;
    DWORD   dpcTriCaps_dwTextureAddressCaps;
    DWORD   dpcTriCaps_dwStippleWidth;
    DWORD   dpcTriCaps_dwStippleHeight;
    /* Device render bit depth */
    DWORD   dwDeviceRenderBitDepth;
    /* Device Z buffer bit depth */
    DWORD   dwDeviceZBufferBitDepth;
    /* Max buffer/vertex sizes */
    DWORD   dwMaxBufferSize;
    DWORD   dwMaxVertexCount;
    /* DX6 extensions */
    DWORD   dwMinTextureWidth;
    DWORD   dwMinTextureHeight;
    DWORD   dwMaxTextureWidth;
    DWORD   dwMaxTextureHeight;
    DWORD   dwMinStippleWidth;
    DWORD   dwMaxStippleWidth;
    DWORD   dwMinStippleHeight;
    DWORD   dwMaxStippleHeight;
    DWORD   dwMaxTextureRepeat;
    DWORD   dwMaxTextureAspectRatio;
    DWORD   dwMaxAnisotropy;
    float   dvGuardBandLeft;
    float   dvGuardBandTop;
    float   dvGuardBandRight;
    float   dvGuardBandBottom;
    float   dvExtentsAdjust;
    DWORD   dwStencilCaps;
    /* FVF caps */
    DWORD   dwFVFCaps;
    DWORD   dwTextureOpCaps;
    WORD    wMaxTextureBlendStages;
    WORD    wMaxSimultaneousTextures;
} D3DDEVICEDESC_V1;

#pragma pack(pop)

/* D3DDEVICEDESC flags */
#define D3DDD_COLORMODEL            0x00000001
#define D3DDD_DEVCAPS               0x00000002
#define D3DDD_TRANSFORMCAPS         0x00000004
#define D3DDD_LIGHTINGCAPS          0x00000010
#define D3DDD_BCLIPPING             0x00000020
#define D3DDD_LINECAPS              0x00000100
#define D3DDD_TRICAPS               0x00000200
#define D3DDD_DEVICERENDERBITDEPTH  0x00000400
#define D3DDD_DEVICEZBUFFERBITDEPTH 0x00000800
#define D3DDD_MAXBUFFERSIZE         0x00001000
#define D3DDD_MAXVERTEXCOUNT        0x00002000

/* Device caps */
#define D3DDEVCAPS6_HWRASTERIZATION         0x00000080
#define D3DDEVCAPS6_TEXTUREVIDEOMEMORY      0x00000200

/* Primitive caps - misc */
#define D3DPMISCCAPS6_MASKZ                 0x00000002

/* Primitive caps - raster */
#define D3DPRASTERCAPS6_DITHER              0x00000001
#define D3DPRASTERCAPS6_ZTEST               0x00000010
#define D3DPRASTERCAPS6_FOGVERTEX           0x00000080
#define D3DPRASTERCAPS6_FOGTABLE            0x00000100
#define D3DPRASTERCAPS6_SUBPIXEL            0x00000020
#define D3DPRASTERCAPS6_ZFOG                0x00200000
#define D3DPRASTERCAPS6_MIPMAPLODBIAS       0x00002000

/* Z compare caps */
#define D3DPCMPCAPS6_NEVER                  0x00000001
#define D3DPCMPCAPS6_LESS                   0x00000002
#define D3DPCMPCAPS6_EQUAL                  0x00000004
#define D3DPCMPCAPS6_LESSEQUAL              0x00000008
#define D3DPCMPCAPS6_GREATER                0x00000010
#define D3DPCMPCAPS6_NOTEQUAL               0x00000020
#define D3DPCMPCAPS6_GREATEREQUAL           0x00000040
#define D3DPCMPCAPS6_ALWAYS                 0x00000080

/* Blend caps */
#define D3DPBLENDCAPS6_ZERO                 0x00000001
#define D3DPBLENDCAPS6_ONE                  0x00000002
#define D3DPBLENDCAPS6_SRCALPHA             0x00000010
#define D3DPBLENDCAPS6_INVSRCALPHA          0x00000020
#define D3DPBLENDCAPS6_SRCCOLOR             0x00000004
#define D3DPBLENDCAPS6_INVSRCCOLOR          0x00000008
#define D3DPBLENDCAPS6_DESTALPHA            0x00000040
#define D3DPBLENDCAPS6_INVDESTALPHA         0x00000080
#define D3DPBLENDCAPS6_DESTCOLOR            0x00000100
#define D3DPBLENDCAPS6_INVDESTCOLOR         0x00000200

/* Shade caps */
#define D3DPSHADECAPS6_COLORGOURAUDRGB      0x00000008
#define D3DPSHADECAPS6_SPECULARGOURAUDRGB   0x00000200
#define D3DPSHADECAPS6_ALPHAGOURAUDBLEND    0x00004000
#define D3DPSHADECAPS6_FOGGOURAUD           0x00080000

/* Texture caps */
#define D3DPTEXTURECAPS6_PERSPECTIVE        0x00000001
#define D3DPTEXTURECAPS6_ALPHA              0x00000004
#define D3DPTEXTURECAPS6_POW2               0x00000002
#define D3DPTEXTURECAPS6_NONPOW2CONDITIONAL 0x00000100
#define D3DPTEXTURECAPS6_TRANSPARENCY       0x00000008

/* Texture filter caps */
#define D3DPTFILTERCAPS6_NEAREST            0x00000001
#define D3DPTFILTERCAPS6_LINEAR             0x00000002
#define D3DPTFILTERCAPS6_MIPNEAREST         0x00000004
#define D3DPTFILTERCAPS6_MIPLINEAR          0x00000008
#define D3DPTFILTERCAPS6_LINEARMIPNEAREST   0x00000010
#define D3DPTFILTERCAPS6_LINEARMIPLINEAR    0x00000020
#define D3DPTFILTERCAPS6_MAGFPOINT          0x01000000
#define D3DPTFILTERCAPS6_MAGFLINEAR         0x02000000
#define D3DPTFILTERCAPS6_MAGFANISOTROPIC    0x04000000
#define D3DPTFILTERCAPS6_MINFPOINT          0x00000100
#define D3DPTFILTERCAPS6_MINFLINEAR         0x00000200
#define D3DPTFILTERCAPS6_MINFANISOTROPIC    0x00000400
#define D3DPTFILTERCAPS6_MIPFPOINT          0x00010000
#define D3DPTFILTERCAPS6_MIPFLINEAR         0x00020000

/* Texture blend caps */
#define D3DPTBLENDCAPS6_DECAL               0x00000001
#define D3DPTBLENDCAPS6_MODULATE            0x00000004
#define D3DPTBLENDCAPS6_MODULATEALPHA       0x00000008
#define D3DPTBLENDCAPS6_COPY                0x00000040
#define D3DPTBLENDCAPS6_ADD                 0x00000080

/* Texture address caps */
#define D3DPTADDRESSCAPS6_WRAP              0x00000001
#define D3DPTADDRESSCAPS6_MIRROR            0x00000002
#define D3DPTADDRESSCAPS6_CLAMP             0x00000004

/* Bit depth flags */
#define DDBD_16                             0x00000400
#define DDBD_32                             0x00002000

/* ========================================================================
 * Helper: fill a D3DDEVICEDESC_V1 with HAL-like capabilities
 * ======================================================================== */

void FillDeviceDesc(D3DDEVICEDESC_V1 *desc)
{
    ZeroMemory(desc, sizeof(*desc));
    desc->dwSize = sizeof(*desc);

    desc->dwFlags = D3DDD_COLORMODEL | D3DDD_DEVCAPS | D3DDD_TRANSFORMCAPS |
                    D3DDD_LIGHTINGCAPS | D3DDD_BCLIPPING |
                    D3DDD_LINECAPS | D3DDD_TRICAPS |
                    D3DDD_DEVICERENDERBITDEPTH | D3DDD_DEVICEZBUFFERBITDEPTH |
                    D3DDD_MAXBUFFERSIZE | D3DDD_MAXVERTEXCOUNT;

    desc->dwColorModel = 1;  /* D3DCOLOR_RGB */
    desc->dwDevCaps = D3DDEVCAPS6_HWRASTERIZATION | D3DDEVCAPS6_TEXTUREVIDEOMEMORY;

    /* Transform caps */
    desc->dtcTransformCaps_dwSize = 8;
    desc->dtcTransformCaps_dwCaps = 1;  /* D3DTRANSFORMCAPS_CLIP */
    desc->bClipping = TRUE;

    /* Lighting caps */
    desc->dlcLightingCaps_dwSize = 16;
    desc->dlcLightingCaps_dwCaps = 1;   /* D3DLIGHTCAPS_POINT */
    desc->dlcLightingCaps_dwLightingModel = 1;  /* D3DLIGHTINGMODEL_RGB */
    desc->dlcLightingCaps_dwNumLights = 8;

    /* Shared primitive caps fields */
    #define FILL_PRIMCAPS(prefix) \
        desc->prefix##_dwSize = 56; /* sizeof(D3DPRIMCAPS) */ \
        desc->prefix##_dwMiscCaps = D3DPMISCCAPS6_MASKZ; \
        desc->prefix##_dwRasterCaps = D3DPRASTERCAPS6_DITHER | D3DPRASTERCAPS6_ZTEST | \
                                       D3DPRASTERCAPS6_FOGVERTEX | D3DPRASTERCAPS6_FOGTABLE | \
                                       D3DPRASTERCAPS6_SUBPIXEL | D3DPRASTERCAPS6_ZFOG | \
                                       D3DPRASTERCAPS6_MIPMAPLODBIAS; \
        desc->prefix##_dwZCmpCaps = D3DPCMPCAPS6_NEVER | D3DPCMPCAPS6_LESS | \
                                     D3DPCMPCAPS6_EQUAL | D3DPCMPCAPS6_LESSEQUAL | \
                                     D3DPCMPCAPS6_GREATER | D3DPCMPCAPS6_NOTEQUAL | \
                                     D3DPCMPCAPS6_GREATEREQUAL | D3DPCMPCAPS6_ALWAYS; \
        desc->prefix##_dwSrcBlendCaps = D3DPBLENDCAPS6_ZERO | D3DPBLENDCAPS6_ONE | \
                                         D3DPBLENDCAPS6_SRCALPHA | D3DPBLENDCAPS6_INVSRCALPHA | \
                                         D3DPBLENDCAPS6_SRCCOLOR | D3DPBLENDCAPS6_INVSRCCOLOR | \
                                         D3DPBLENDCAPS6_DESTALPHA | D3DPBLENDCAPS6_INVDESTALPHA | \
                                         D3DPBLENDCAPS6_DESTCOLOR | D3DPBLENDCAPS6_INVDESTCOLOR; \
        desc->prefix##_dwDestBlendCaps = desc->prefix##_dwSrcBlendCaps; \
        desc->prefix##_dwAlphaCmpCaps = desc->prefix##_dwZCmpCaps; \
        desc->prefix##_dwShadeCaps = D3DPSHADECAPS6_COLORGOURAUDRGB | \
                                      D3DPSHADECAPS6_SPECULARGOURAUDRGB | \
                                      D3DPSHADECAPS6_ALPHAGOURAUDBLEND | \
                                      D3DPSHADECAPS6_FOGGOURAUD; \
        desc->prefix##_dwTextureCaps = D3DPTEXTURECAPS6_PERSPECTIVE | D3DPTEXTURECAPS6_ALPHA | \
                                        D3DPTEXTURECAPS6_POW2 | \
                                        D3DPTEXTURECAPS6_NONPOW2CONDITIONAL | \
                                        D3DPTEXTURECAPS6_TRANSPARENCY; \
        desc->prefix##_dwTextureFilterCaps = D3DPTFILTERCAPS6_NEAREST | D3DPTFILTERCAPS6_LINEAR | \
                                              D3DPTFILTERCAPS6_MIPNEAREST | D3DPTFILTERCAPS6_MIPLINEAR | \
                                              D3DPTFILTERCAPS6_LINEARMIPNEAREST | D3DPTFILTERCAPS6_LINEARMIPLINEAR | \
                                              D3DPTFILTERCAPS6_MAGFPOINT | D3DPTFILTERCAPS6_MAGFLINEAR | \
                                              D3DPTFILTERCAPS6_MINFPOINT | D3DPTFILTERCAPS6_MINFLINEAR | \
                                              D3DPTFILTERCAPS6_MINFANISOTROPIC | \
                                              D3DPTFILTERCAPS6_MIPFPOINT | D3DPTFILTERCAPS6_MIPFLINEAR; \
        desc->prefix##_dwTextureBlendCaps = D3DPTBLENDCAPS6_DECAL | D3DPTBLENDCAPS6_MODULATE | \
                                             D3DPTBLENDCAPS6_MODULATEALPHA | D3DPTBLENDCAPS6_COPY | \
                                             D3DPTBLENDCAPS6_ADD; \
        desc->prefix##_dwTextureAddressCaps = D3DPTADDRESSCAPS6_WRAP | D3DPTADDRESSCAPS6_MIRROR | \
                                               D3DPTADDRESSCAPS6_CLAMP; \
        desc->prefix##_dwStippleWidth = 0; \
        desc->prefix##_dwStippleHeight = 0;

    FILL_PRIMCAPS(dpcLineCaps)
    FILL_PRIMCAPS(dpcTriCaps)
    #undef FILL_PRIMCAPS

    desc->dwDeviceRenderBitDepth = DDBD_16 | DDBD_32;
    desc->dwDeviceZBufferBitDepth = DDBD_16 | DDBD_32;
    desc->dwMaxBufferSize = 0;
    desc->dwMaxVertexCount = 65536;

    /* DX6 texture size limits */
    desc->dwMinTextureWidth = 1;
    desc->dwMinTextureHeight = 1;
    desc->dwMaxTextureWidth = 2048;
    desc->dwMaxTextureHeight = 2048;
    desc->dwMaxTextureRepeat = 2048;
    desc->dwMaxTextureAspectRatio = 2048;
    desc->dwMaxAnisotropy = 16;

    /* Guard band */
    desc->dvGuardBandLeft = -4096.0f;
    desc->dvGuardBandTop = -4096.0f;
    desc->dvGuardBandRight = 4096.0f;
    desc->dvGuardBandBottom = 4096.0f;
    desc->dvExtentsAdjust = 0.0f;

    desc->dwStencilCaps = 0;  /* TD5 doesn't use stencil */

    /* FVF support */
    desc->dwFVFCaps = 1;  /* 1 simultaneous texture */
    desc->dwTextureOpCaps = 0x3F;  /* basic ops */
    desc->wMaxTextureBlendStages = 1;
    desc->wMaxSimultaneousTextures = 1;
}

/* ========================================================================
 * WrapperD3D COM object (implements IDirect3D3)
 * ======================================================================== */

struct WrapperD3D {
    void  **vtbl;
    LONG    ref_count;
    WrapperDirectDraw *ddraw;
};

/* Vtable methods */

static HRESULT __stdcall D3D3_QueryInterface(WrapperD3D *self, REFIID riid, void **ppv)
{
    WRAPPER_LOG("IDirect3D3::QueryInterface");
    if (!ppv) return E_POINTER;

    if (IsEqualGUID(riid, &IID_IDirect3D3) || IsEqualGUID(riid, &IID_IUnknown)) {
        *ppv = self;
        self->ref_count++;
        return S_OK;
    }

    WRAPPER_LOG("IDirect3D3::QueryInterface - unknown IID");
    *ppv = NULL;
    return E_NOINTERFACE;
}

static ULONG __stdcall D3D3_AddRef(WrapperD3D *self)
{
    self->ref_count++;
    WRAPPER_LOG("IDirect3D3::AddRef -> %ld", self->ref_count);
    return self->ref_count;
}

static ULONG __stdcall D3D3_Release(WrapperD3D *self)
{
    self->ref_count--;
    WRAPPER_LOG("IDirect3D3::Release -> %ld", self->ref_count);
    if (self->ref_count <= 0) {
        WRAPPER_LOG("IDirect3D3::Release - destroying");
        if (g_backend.d3d == self)
            g_backend.d3d = NULL;
        HeapFree(GetProcessHeap(), 0, self);
        return 0;
    }
    return self->ref_count;
}

static HRESULT __stdcall D3D3_EnumDevices(WrapperD3D *self,
    LPD3DENUMDEVICESCALLBACK3 callback, void *ctx)
{
    D3DDEVICEDESC_V1 hw_desc, sw_desc;
    HRESULT ret;
    (void)self;

    WRAPPER_LOG("IDirect3D3::EnumDevices");
    if (!callback) return E_POINTER;

    /* Fill both HW and SW descriptors with our capabilities */
    FillDeviceDesc(&hw_desc);
    FillDeviceDesc(&sw_desc);

    /* Call once with HAL device */
    ret = callback(
        (GUID*)&IID_IDirect3DHALDevice,
        "Direct3D HAL",
        "D3D11 Hardware Accelerated",
        &hw_desc,   /* D3DDEVICEDESC for HW */
        &sw_desc,   /* D3DDEVICEDESC for SW */
        ctx
    );

    WRAPPER_LOG("IDirect3D3::EnumDevices - callback returned %d", ret);
    return DD_OK;
}

static HRESULT __stdcall D3D3_CreateLight(WrapperD3D *self, void **light, void *outer)
{
    (void)self; (void)light; (void)outer;
    WRAPPER_STUB("IDirect3D3::CreateLight");
}

static HRESULT __stdcall D3D3_CreateMaterial(WrapperD3D *self, void **material, void *outer)
{
    (void)self; (void)material; (void)outer;
    WRAPPER_STUB("IDirect3D3::CreateMaterial");
}

static HRESULT __stdcall D3D3_CreateViewport(WrapperD3D *self, void **viewport, void *outer)
{
    (void)self; (void)outer;

    WRAPPER_LOG("IDirect3D3::CreateViewport");
    if (!viewport) return E_POINTER;

    *viewport = WrapperViewport_Create();
    if (!*viewport) return E_OUTOFMEMORY;

    WRAPPER_LOG("IDirect3D3::CreateViewport -> %p", *viewport);
    return DD_OK;
}

static HRESULT __stdcall D3D3_FindDevice(WrapperD3D *self, void *search, void *result)
{
    (void)self; (void)search; (void)result;
    WRAPPER_STUB("IDirect3D3::FindDevice");
}

static HRESULT __stdcall D3D3_CreateDevice(WrapperD3D *self, GUID *guid,
    WrapperSurface *target, WrapperDevice **out, void *outer)
{
    (void)self; (void)guid; (void)outer;

    WRAPPER_LOG("IDirect3D3::CreateDevice (target=%p)", target);
    if (!out) return E_POINTER;

    *out = WrapperDevice_Create(target);
    if (!*out) return E_OUTOFMEMORY;

    g_backend.d3ddevice = *out;
    WRAPPER_LOG("IDirect3D3::CreateDevice -> %p", *out);
    return DD_OK;
}

/* CreateVertexBuffer - stub, TD5 doesn't use vertex buffers */
static HRESULT __stdcall D3D3_CreateVertexBuffer(WrapperD3D *self, void *desc, void **vb, DWORD flags, void *outer)
{
    (void)self; (void)desc; (void)vb; (void)flags; (void)outer;
    WRAPPER_LOG("IDirect3D3::CreateVertexBuffer (STUB)");
    return E_NOTIMPL;
}

static HRESULT __stdcall D3D3_EvictManagedTextures(WrapperD3D *self)
{
    (void)self;
    WRAPPER_STUB("IDirect3D3::EvictManagedTextures");
}

static HRESULT __stdcall D3D3_EnumZBufferFormats(WrapperD3D *self, GUID *device,
    LPD3DENUMPIXELFORMATSCALLBACK cb, void *ctx)
{
    DDPIXELFORMAT_W pf;
    (void)self; (void)device;

    WRAPPER_LOG("IDirect3D3::EnumZBufferFormats");
    if (!cb) return E_POINTER;

    /* 16-bit Z buffer */
    ZeroMemory(&pf, sizeof(pf));
    pf.dwSize = sizeof(pf);
    pf.dwFlags = DDPF_ZBUFFER;
    pf.dwZBufferBitDepth = 16;
    pf.dwZBitMask = 0xFFFF;
    WRAPPER_LOG("  Offering Z16");
    cb(&pf, ctx);

    /* 32-bit Z buffer */
    ZeroMemory(&pf, sizeof(pf));
    pf.dwSize = sizeof(pf);
    pf.dwFlags = DDPF_ZBUFFER;
    pf.dwZBufferBitDepth = 32;
    pf.dwZBitMask = 0xFFFFFFFF;
    WRAPPER_LOG("  Offering Z32");
    cb(&pf, ctx);

    return DD_OK;
}

/* ========================================================================
 * Vtable
 * ======================================================================== */

static void *d3d3_vtbl[] = {
    /* IUnknown */
    /* 0 */ D3D3_QueryInterface,
    /* 1 */ D3D3_AddRef,
    /* 2 */ D3D3_Release,
    /* IDirect3D3 */
    /* 3 */ D3D3_EnumDevices,
    /* 4 */ D3D3_CreateLight,
    /* 5 */ D3D3_CreateMaterial,
    /* 6 */ D3D3_CreateViewport,
    /* 7 */ D3D3_FindDevice,
    /* 8 */ D3D3_CreateDevice,
    /* 9 */ D3D3_CreateVertexBuffer,
    /*10 */ D3D3_EnumZBufferFormats,
    /*11 */ D3D3_EvictManagedTextures,
};

/* ========================================================================
 * Constructor
 * ======================================================================== */

WrapperD3D* WrapperD3D_Create(WrapperDirectDraw *ddraw)
{
    WrapperD3D *obj = (WrapperD3D*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(WrapperD3D));
    if (!obj) return NULL;

    obj->vtbl = d3d3_vtbl;
    obj->ref_count = 1;
    obj->ddraw = ddraw;

    g_backend.d3d = obj;
    WRAPPER_LOG("WrapperD3D_Create -> %p", obj);
    return obj;
}
