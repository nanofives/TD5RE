/**
 * td5_wrapper_ddraw_types.h -- DirectDraw constants, flags, GUIDs and the
 * DDraw structure subset needed by M2DX.dll (minimal definitions to avoid
 * requiring ddraw.h from the SDK). Split out of wrapper.h (2026-07-09, A3
 * refactor). Included from wrapper.h only -- not meant to be included
 * directly.
 */

#ifndef TD5_WRAPPER_DDRAW_TYPES_H
#define TD5_WRAPPER_DDRAW_TYPES_H

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
#define DDSD_PITCH               0x00000008
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

#endif /* TD5_WRAPPER_DDRAW_TYPES_H */
