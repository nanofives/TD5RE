/**
 * td5_wrapper_backend.h -- D3D11 backend state: render-state cache, deferred
 * pane-record context, constant buffers, and the global D3D11Backend struct.
 * Split out of wrapper.h (2026-07-09, A3 refactor). Included from wrapper.h
 * only -- not meant to be included directly (relies on wrapper.h's forward
 * declarations of the Wrapper* COM object types).
 */

#ifndef TD5_WRAPPER_BACKEND_H
#define TD5_WRAPPER_BACKEND_H

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
#define BLEND_MULT               6  /* dst * src (P2 shadow darkening pass) */
#define BLEND_STATE_COUNT        7

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
/* [lighting rework P0] MRT variants: same color math + SV_Target1 = COLOR1
 * (world normal + material id) into the G-buffer. Auto-selected in place of
 * their base shader for z-writing non-blended draws while the G-buffer is
 * active (Backend_SetGBufferEnabled). */
#define PS_MODULATE_G            4
#define PS_MODULATE_ALPHA_G      5
#define PS_COUNT                 6

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

    /* Polygon-offset selector — 0 = default rasterizer, 1 = shadow decal
     * rasterizer (DepthBias + SlopeScaledDepthBias toward camera). Used to
     * resolve z-fighting on co-planar shadow quads without forcing a
     * constant depth bias that overshoots nearby occluders. */
    int     polygon_offset;

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
    int     current_rs_idx;     /* 0=default, 1=shadow decal */

    /* Dirty flag — set when any state changes, cleared after binding */
    int     dirty;
} RenderStateCache;

/* ========================================================================
 * [Phase B / Stage 2 — multithreaded pane recording, 2026-06-08]
 *
 * WrapperRecCtx bundles ALL per-draw mutable wrapper state for ONE pane being
 * recorded on a worker thread into its own DEFERRED context: the deferred
 * context itself, a private dynamic VB/IB + ring offsets, private viewport/fog
 * constant buffers (each Mapped per-draw, so they must not be shared across
 * concurrently-recording panes), the render-state cache, and the bound SRV.
 *
 * A thread-local pointer g_wrapper_rec selects the active bundle: NULL on the
 * main thread (the immediate-context path uses g_backend.* exactly as before —
 * byte-identical serial fallback) and the pane's bundle on a worker thread.
 * ======================================================================== */
typedef struct WrapperRecCtx {
    ID3D11DeviceContext      *dc;            /* deferred context */
    ID3D11Buffer             *vb, *ib;       /* private dynamic buffers */
    UINT                      vb_size, ib_size;
    UINT                      vb_off, ib_off;/* ring offsets into vb/ib */
    ID3D11Buffer             *cb_viewport;   /* private ViewportCB */
    ID3D11Buffer             *cb_fog;        /* private FogCB */
    RenderStateCache          state;         /* private state cache */
    ID3D11ShaderResourceView *current_srv;   /* private bound texture */
    int                       current_tex_has_alpha;
    int                       alpha_blend_enabled;
    int                       in_use;        /* allocated/active */
} WrapperRecCtx;

/* Thread-local active recording bundle. NULL => main thread / immediate path. */
extern __thread WrapperRecCtx *g_wrapper_rec;

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
    float   foliageAA;      /* [foliage AA] 1.0 = this draw uses the clamped,
                              * alpha-weighted manual reconstruction in
                              * SampleFoliageAA (ps_common.hlsli) instead of
                              * texMap.Sample(); 0.0 = normal sampling. */
} FogCB;

/* Deferred dynamic-light pass constant buffer (mirrors ps_light.hlsl LightCB).
 * Must stay 16-byte aligned; each light is 3 float4 (pos+range, color+intensity,
 * dir+coneCos). LIGHT_MAX_GPU must match LIGHT_MAX in ps_light.hlsl. */
#define LIGHT_MAX_GPU 32
typedef struct {
    float camPosFocal[4];    /* xyz = camera world pos, w = focal            */
    float rightCx[4];        /* xyz = camera right,   w = viewport center X  */
    float upCy[4];           /* xyz = camera up,      w = viewport center Y  */
    float fwdDepthScale[4];  /* xyz = camera forward, w = depth scale        */
    float misc[4];           /* x = depth bias, y = count, z = vpX, w = vpY  */
    float ext[4];            /* [P2] x = occlusion steps (0=off), y = pane W, z = pane H */
    float lights[LIGHT_MAX_GPU * 3][4];
} LightCB;

/* [P2] Screen-space ray-marched sun-shadow pass constant buffer (mirrors
 * ps_shadow.hlsl ShadowCB; 16-byte aligned). */
typedef struct {
    float camPosFocal[4];    /* xyz = camera world pos, w = focal            */
    float rightCx[4];        /* xyz = camera right,   w = viewport center X  */
    float upCy[4];           /* xyz = camera up,      w = viewport center Y  */
    float fwdDepthScale[4];  /* xyz = camera forward, w = depth scale        */
    float misc[4];           /* x = depth bias, y = vpX, z = vpY, w = strength */
    float sun[4];            /* xyz = surface->light dir (unit), w = max march dist */
    float params[4];         /* x = steps, y = thickness, z = start offset, w = pane W */
    float params2[4];        /* x = pane H, yzw reserved                     */
} ShadowCB;

/* [P3] Screen-space reflections constant buffer (mirrors ps_ssr.hlsl SSRCB). */
typedef struct {
    float camPosFocal[4];    /* xyz = camera world pos, w = focal            */
    float rightCx[4];        /* xyz = camera right,   w = viewport center X  */
    float upCy[4];           /* xyz = camera up,      w = viewport center Y  */
    float fwdDepthScale[4];  /* xyz = camera forward, w = depth scale        */
    float misc[4];           /* x = depth bias, y = vpX, z = vpY, w = wet-road boost */
    float params[4];         /* x = steps, y = max march dist, z = thickness, w = pane W */
    float params2[4];        /* x = pane H, y = master intensity, zw reserved */
    float reflA[4];          /* base reflectivity, material ids 0..3         */
    float reflB[4];          /* base reflectivity, material ids 4..7         */
} SSRCB;

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
    ID3D11Texture2D         *depth_tex;     /* Depth buffer texture (R32_TYPELESS) */
    ID3D11DepthStencilView  *depth_dsv;     /* Depth buffer DSV (D32_FLOAT, writable) */
    /* [2026-06-08 soft particles] Read-only DSV + SRV over the SAME depth_tex so
     * the smoke shader can SAMPLE scene depth (SRV) while the depth is still bound
     * for z-testing (read-only DSV) — enables depth-aware soft-particle fade. Both
     * NULL if creation failed (soft particles then silently disabled). */
    ID3D11DepthStencilView   *depth_dsv_readonly; /* D32_FLOAT, READ_ONLY_DEPTH */
    ID3D11ShaderResourceView *depth_srv;          /* R32_FLOAT view of depth_tex */

    /* [lighting rework P0] G-buffer: R8G8B8A8 target carrying world normal
     * (rgb, biased) + material id (a) written by the ps_*_g MRT variants
     * during z-writing opaque draws; sampled by the deferred light pass for
     * N.L. Lazily created at render-target size; NULL until first enable. */
    ID3D11Texture2D          *gbuffer_tex;
    ID3D11RenderTargetView   *gbuffer_rtv;
    ID3D11ShaderResourceView *gbuffer_srv;
    int                       gbuffer_w, gbuffer_h;
    int                       gbuffer_enabled;   /* game wants G-buffer this frame */
    int                       gbuffer_bound;     /* RT1 currently bound (OM state) */

    /* [foliage AA 2026-07-04, reworked 2026-07-04] Anti-alias 2D cutout
     * foliage (trees/fences/signs). When set, draws the game submits with
     * color-key alpha test on a transparent-source texture (A1R5G5B5 /
     * color-keyed R5G6B5) are rendered via a manual clamped, alpha-weighted
     * 4-tap reconstruction (SampleFoliageAA in ps_common.hlsli) instead of
     * texMap.Sample(), so the 1-bit edge/dither alpha smooths into an
     * anti-aliased silhouette instead of a jagged point-sampled cutout.
     * Sampling manually (not via the bound sampler) avoids two bugs hardware
     * bilinear had: (1) "transparent" texels keep whatever incidental RGB
     * was baked into the source art (e.g. sky-blue painted behind the tree)
     * since texture2.c never zeroes it, so bilinear would leak that color
     * into the edge regardless of what's actually behind the sprite; (2) the
     * clamped-index fetch never wraps to the texture's opposite edge, unlike
     * the sampler's WRAP addressing, which caused a seam/bar at the border.
     * Read once from TD5RE_FOLIAGE_AA at device init (default 1). See
     * Backend_IsFoliageAA. */
    int                       foliage_aa_enabled;

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

    /* 1x1 white texture used by the debug-line path (PS_MODULATE * white = vertex color) */
    ID3D11Texture2D             *white_tex;
    ID3D11ShaderResourceView    *white_srv;

    /* Shaders */
    ID3D11VertexShader      *vs_pretransformed;
    ID3D11VertexShader      *vs_fullscreen;
    ID3D11PixelShader       *ps_shaders[PS_COUNT];  /* Indexed by PS_* constants */
    ID3D11PixelShader       *ps_composite;           /* Fullscreen blit shader */
    ID3D11PixelShader       *ps_light;                /* Deferred dynamic-light pass */
    ID3D11PixelShader       *ps_shadow;               /* [P2] SS sun-shadow pass */
    ID3D11PixelShader       *ps_ssr;                  /* [P3] SS reflections pass */

    /* [P3] Scene-colour copy sampled by the SSR pass (reflections must read
     * the scene while drawing onto it). (Re)created to match the backbuffer
     * surface texture; NULL until the first SSR pass. */
    ID3D11Texture2D          *scene_copy_tex;
    ID3D11ShaderResourceView *scene_copy_srv;
    int                       scene_copy_w, scene_copy_h;
    ID3D11InputLayout       *input_layout;           /* TD5_FVF vertex layout */

    /* Immutable state objects (pre-created at init) */
    ID3D11BlendState        *blend_states[BLEND_STATE_COUNT];
    ID3D11DepthStencilState *ds_states[DS_STATE_COUNT];
    ID3D11RasterizerState   *rs_state;               /* CullNone, solid fill */
    ID3D11RasterizerState   *rs_state_shadow_decal;  /* same + DepthBias + SlopeScaledDepthBias */
    ID3D11SamplerState      *sampler_states[SAMP_STATE_COUNT];

    /* Dynamic buffers for immediate-mode rendering */
    ID3D11Buffer            *dynamic_vb;    /* WRITE_DISCARD vertex buffer */
    ID3D11Buffer            *dynamic_ib;    /* WRITE_DISCARD index buffer */
    UINT                     dynamic_vb_size;
    UINT                     dynamic_ib_size;

    /* Constant buffers */
    ID3D11Buffer            *cb_viewport;   /* ViewportCB */
    ID3D11Buffer            *cb_fog;        /* FogCB */
    ID3D11Buffer            *cb_light;      /* LightCB (deferred light pass) */
    ID3D11Buffer            *cb_shadow;     /* [P2] ShadowCB (SS sun shadows) */
    ID3D11Buffer            *cb_ssr;        /* [P3] SSRCB (SS reflections) */

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

    /* [S01 Display options 2026-06-04] Runtime present/window-mode knobs.
     *   vsync       : Present sync interval (1 = wait for vblank, 0 = tear/uncapped).
     *                 Defaults to 1 (the original DDraw DDFLIP_VSYNC behavior).
     *   window_mode : 0 = exclusive fullscreen, 1 = windowed, 2 = borderless.
     *                 Owned by the platform layer; mirrored here so the wrapper's
     *                 swap-chain code can branch on it. */
    int                 vsync;
    int                 window_mode;

    /* Vertex coordinate scaling: game emits XYZRHW vertices in M2DX's internal
     * resolution (e.g. 640x480) but our RT is at native res. These scale factors
     * are applied to every pre-transformed vertex X/Y in DrawPrimitive. */
    float               vertex_scale_x;  /* native_w / m2dx_w (e.g. 2048/640 = 3.2) */
    float               vertex_scale_y;  /* native_h / m2dx_h (e.g. 1152/480 = 2.4) */
    int                 m2dx_width;      /* M2DX's internal resolution (what game thinks) */
    int                 m2dx_height;

    /* Standalone mode: skip all hardcoded address fixups (no original EXE loaded) */
    int                 standalone;

    /* [DEVICE-LOST 2026-07-13] Latched when a GPU call returns a device-
     * removed/reset/hung HRESULT. Once set, the present/render path no-ops so
     * we STOP issuing commands on a dead device -- otherwise the driver
     * eventually NULL-derefs (0xC0000005) and takes the whole process down,
     * turning a recoverable TDR into a hard crash. Backend_NoteDeviceRemoved()
     * also logs GetDeviceRemovedReason() so the root cause is diagnosable. */
    int                 device_removed;

    /* [DEVICE-LOST recovery] Bumped every time the D3D11 device is (re)created.
     * Starts at 1 on first Backend_CreateDevice; each successful
     * Backend_RecreateDevice() after a TDR increments it. Surfaces carry their
     * own copy (WrapperSurface.device_generation); a mismatch means the
     * surface's GPU objects belong to a dead device and must be rebuilt. */
    unsigned            device_generation;

    /* [diag] TDR/device-lost forensics. present_count is bumped on every
     * swap-chain Present so the device-lost log can name the frame; diag_context
     * is a short breadcrumb the caller (e.g. the self-test step runner or the
     * race loop) sets so a hang names WHICH scene/step was on screen -- these
     * pinpoint the offending frame without a timestamped log. */
    unsigned long       present_count;
    char                diag_context[64];

    /* Frontend native-resolution scaling */
    FrontendScale       fe_scale;
} D3D11Backend;

extern D3D11Backend g_backend;

/* [DEVICE-LOST 2026-07-13] If `hr` is a device-removed/reset/hung code, log it
 * + the GetDeviceRemovedReason() detail (HUNG / REMOVED / RESET /
 * DRIVER_INTERNAL_ERROR / INVALID_CALL) and latch g_backend.device_removed.
 * `where` names the call site for the log. Returns 1 if the device is (now)
 * removed, else 0. Cheap no-op on a healthy SUCCEEDED hr. */
int Backend_NoteDeviceRemoved(HRESULT hr, const char *where);

/* [DEVICE-LOST recovery] Recreate the D3D11 device + swap chain + all
 * backend-owned GPU resources (render targets, shaders, state objects, dynamic
 * buffers, constant buffers) after a TDR left the old device removed. Reuses
 * the existing display window and the game-established scaling/dimension state
 * (does NOT re-run window creation or fe_scale setup). On success bumps
 * g_backend.device_generation and clears g_backend.device_removed; per-surface
 * GPU textures are rebuilt lazily from sys_buffer on next use. Returns 1 on
 * success, 0 on failure (device stays removed so the caller can retry). */
int Backend_RecreateDevice(void);

/* ========================================================================
 * PNG texture override (png_loader.c)
 * ======================================================================== */

void PngOverride_Init(void);
void PngOverride_Shutdown(void);
/* Drop cached override SRVs on device-lost (called from Backend_ReleaseDeviceObjects). */
void PngOverride_InvalidateCache(void);
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

#endif /* TD5_WRAPPER_BACKEND_H */
