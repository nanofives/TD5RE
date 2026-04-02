/**
 * device3.c - IDirect3DDevice3 COM wrapper implementation
 *
 * This is the most critical file in the wrapper. All rendering goes through
 * IDirect3DDevice3 methods, which we translate to D3D11 calls.
 *
 * TD5 uses pre-transformed vertices (XYZRHW|DIFFUSE|SPECULAR|TEX1, stride 32)
 * which are uploaded to a dynamic vertex buffer. The vertex shader converts
 * screen-space pixel coordinates to NDC. Pixel shaders replace the D3D6
 * fixed-function texture stage states.
 *
 * Vtable layout: IDirect3DDevice3 has a flat vtable (no inherited Device/Device2).
 */

#include "wrapper.h"

/* ========================================================================
 * D3DDEVICEDESC_V1 -- same struct as in d3d3.c (for GetCaps)
 * ======================================================================== */

#pragma pack(push, 1)

typedef struct {
    DWORD   dwSize;
    DWORD   dwFlags;
    DWORD   dwColorModel;
    DWORD   dwDevCaps;
    DWORD   dtcTransformCaps_dwSize;
    DWORD   dtcTransformCaps_dwCaps;
    BOOL    bClipping;
    DWORD   dlcLightingCaps_dwSize;
    DWORD   dlcLightingCaps_dwCaps;
    DWORD   dlcLightingCaps_dwLightingModel;
    DWORD   dlcLightingCaps_dwNumLights;
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
    DWORD   dwDeviceRenderBitDepth;
    DWORD   dwDeviceZBufferBitDepth;
    DWORD   dwMaxBufferSize;
    DWORD   dwMaxVertexCount;
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
    DWORD   dwFVFCaps;
    DWORD   dwTextureOpCaps;
    WORD    wMaxTextureBlendStages;
    WORD    wMaxSimultaneousTextures;
} D3DDEVICEDESC_V1_Dev;

#pragma pack(pop)

extern void FillDeviceDesc(void *desc);

/* ========================================================================
 * WrapperDevice COM object
 * ======================================================================== */

struct WrapperDevice {
    void           **vtbl;
    LONG             ref_count;
    WrapperSurface  *render_target;
    WrapperViewport *viewport;
};

/* ========================================================================
 * Helper: D3D6 primitive type → D3D11 topology
 * ======================================================================== */

static D3D11_PRIMITIVE_TOPOLOGY MapTopology(DWORD d3d6_prim)
{
    switch (d3d6_prim) {
    case D3DPT_POINTLIST:     return D3D11_PRIMITIVE_TOPOLOGY_POINTLIST;
    case D3DPT_LINELIST:      return D3D11_PRIMITIVE_TOPOLOGY_LINELIST;
    case D3DPT_LINESTRIP:     return D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP;
    case D3DPT_TRIANGLELIST:  return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    case D3DPT_TRIANGLESTRIP: return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
    default:                  return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    }
}

/* ========================================================================
 * Vtable methods — IUnknown
 * ======================================================================== */

static HRESULT __stdcall Dev3_QueryInterface(WrapperDevice *self, REFIID riid, void **ppv)
{
    WRAPPER_LOG("IDirect3DDevice3::QueryInterface");
    if (!ppv) return E_POINTER;
    if (IsEqualGUID(riid, &IID_IDirect3DDevice3) || IsEqualGUID(riid, &IID_IUnknown)) {
        *ppv = self;
        self->ref_count++;
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}

static ULONG __stdcall Dev3_AddRef(WrapperDevice *self)
{
    self->ref_count++;
    return self->ref_count;
}

static ULONG __stdcall Dev3_Release(WrapperDevice *self)
{
    self->ref_count--;
    if (self->ref_count <= 0) {
        if (g_backend.d3ddevice == self)
            g_backend.d3ddevice = NULL;
        HeapFree(GetProcessHeap(), 0, self);
        return 0;
    }
    return self->ref_count;
}

/* ========================================================================
 * Vtable methods — IDirect3DDevice3
 * ======================================================================== */

/* 3: GetCaps */
static HRESULT __stdcall Dev3_GetCaps(WrapperDevice *self, void *hw_desc, void *sw_desc)
{
    (void)self;
    if (hw_desc) FillDeviceDesc(hw_desc);
    if (sw_desc) FillDeviceDesc(sw_desc);
    return DD_OK;
}

/* 4: GetStats */
static HRESULT __stdcall Dev3_GetStats(WrapperDevice *self, void *stats)
{
    (void)self;
    if (stats) ZeroMemory(stats, 24);
    return DD_OK;
}

/* 5: AddViewport */
static HRESULT __stdcall Dev3_AddViewport(WrapperDevice *self, WrapperViewport *vp)
{
    self->viewport = vp;
    return DD_OK;
}

/* 6: DeleteViewport */
static HRESULT __stdcall Dev3_DeleteViewport(WrapperDevice *self, WrapperViewport *vp)
{
    if (self->viewport == vp) self->viewport = NULL;
    return DD_OK;
}

/* 7: NextViewport */
static HRESULT __stdcall Dev3_NextViewport(WrapperDevice *self, void *vp_ref, void **vp_out, DWORD flags)
{
    (void)self; (void)vp_ref; (void)flags;
    if (vp_out) *vp_out = NULL;
    return E_FAIL;
}

/* 8: EnumTextureFormats */
static HRESULT __stdcall Dev3_EnumTextureFormats(WrapperDevice *self,
    LPD3DENUMPIXELFORMATSCALLBACK callback, void *ctx)
{
    DDPIXELFORMAT_W pf;
    (void)self;
    if (!callback) return E_POINTER;

    /* RGB565 - 16-bit, no alpha */
    ZeroMemory(&pf, sizeof(pf));
    pf.dwSize = sizeof(pf);
    pf.dwFlags = DDPF_RGB;
    pf.dwRGBBitCount = 16;
    pf.dwRBitMask = 0xF800;
    pf.dwGBitMask = 0x07E0;
    pf.dwBBitMask = 0x001F;
    if (callback(&pf, ctx) == DDENUMRET_CANCEL) return DD_OK;

    /* ARGB1555 - 16-bit, 1-bit alpha */
    ZeroMemory(&pf, sizeof(pf));
    pf.dwSize = sizeof(pf);
    pf.dwFlags = DDPF_RGB | DDPF_ALPHAPIXELS;
    pf.dwRGBBitCount = 16;
    pf.dwRBitMask = 0x7C00;
    pf.dwGBitMask = 0x03E0;
    pf.dwBBitMask = 0x001F;
    pf.dwRGBAlphaBitMask = 0x8000;
    if (callback(&pf, ctx) == DDENUMRET_CANCEL) return DD_OK;

    return DD_OK;
}

/* Per-frame draw call counter for diagnostics */
static int s_frame_draws = 0;
static int s_frame_idx_draws = 0;
static int s_frame_settex = 0;
static int s_frame_num = 0;
static int s_frame_blend_on = 0;
static int s_frame_blend_off = 0;
static int s_frame_ps_lum = 0;
static int s_frame_ps_mod = 0;
static int s_frame_prim_types[8] = {0}; /* count by D3DPT type */
static int s_frame_fan_verts = 0;  /* total verts in fan draws */

/* 9: BeginScene */
static HRESULT __stdcall Dev3_BeginScene(WrapperDevice *self)
{
    (void)self;
    if (!g_backend.device) return DDERR_GENERIC;

    /* Log previous frame's draw stats (skip hardcoded address reads in standalone) */
    if (!g_backend.standalone && s_frame_num > 0 && s_frame_num <= 50) {
        float grw = *(volatile float*)0x4AAF08;
        float grh = *(volatile float*)0x4AAF0C;
        float focal = *(volatile float*)0x4C3718;
        int gameState = *(volatile int*)0x4C3CE8;
        WRAPPER_LOG("FRAME %d: draws=%d idx_draws=%d settex=%d state=%d "
            "gRender=%.0fx%.0f focal=%.1f vtxScale=%.3fx%.3f "
            "blend_on=%d blend_off=%d ps_lum=%d ps_mod=%d "
            "tris=%d strips=%d fans=%d(v=%d) lists=%d",
            s_frame_num, s_frame_draws, s_frame_idx_draws, s_frame_settex,
            gameState, grw, grh, focal,
            g_backend.vertex_scale_x, g_backend.vertex_scale_y,
            s_frame_blend_on, s_frame_blend_off, s_frame_ps_lum, s_frame_ps_mod,
            s_frame_prim_types[D3DPT_TRIANGLELIST],
            s_frame_prim_types[D3DPT_TRIANGLESTRIP],
            s_frame_prim_types[D3DPT_TRIANGLEFAN], s_frame_fan_verts,
            s_frame_prim_types[D3DPT_LINELIST]);
    }
    s_frame_draws = 0;
    s_frame_idx_draws = 0;
    s_frame_settex = 0;
    s_frame_blend_on = 0;
    s_frame_blend_off = 0;
    s_frame_ps_lum = 0;
    s_frame_ps_mod = 0;
    { int i; for (i=0; i<8; i++) s_frame_prim_types[i]=0; }
    s_frame_fan_verts = 0;
    s_frame_num++;

    /* Fixup: when the game's gRenderWidth/Height are smaller than our native RT
     * (e.g. 640x480 from M2DX), overwrite them to native and reconfigure the
     * projection, viewport layout, and HUD. This ensures the frustum culling
     * planes match the actual render target size, preventing edge culling.
     * SKIP in standalone mode -- no original EXE memory to patch. */
    if (!g_backend.standalone && g_backend.m2dx_width > 0 && g_backend.width > g_backend.m2dx_width) {
        volatile float *rw = (volatile float*)0x4AAF08;
        float nw = (float)g_backend.width;
        float nh = (float)g_backend.height;

        if (*rw > 0.0f && *rw < nw) {
            *rw = nw;
            *(volatile float*)0x4AAF0C = nh;
            *(volatile float*)0x4AAF10 = nw * 0.5f;
            *(volatile float*)0x4AAF14 = nh * 0.5f;

            { typedef void (__cdecl *fn_Proj)(int, int); ((fn_Proj)0x43E7E0)((int)nw, (int)nh); }
            { typedef void (__cdecl *fn_Void)(void); ((fn_Void)0x42C2B0)(); }
            { int sm = (int)*(volatile char*)0x4AAF89;
              typedef void (__cdecl *fn_HudLayout)(int); ((fn_HudLayout)0x437BA0)(sm); }
            { typedef void (__cdecl *fn_PauseLayout)(int); ((fn_PauseLayout)0x43B7C0)(0); }

            WRAPPER_LOG("BeginScene: fixup gRender to %.0fx%.0f (frame %d)", nw, nh, s_frame_num);
        }

        /* Compute vertex scale from current gRenderWidth vs native RT.
         * After fixup: *rw == nw, so scale = 1.0 (game renders at native res).
         * Before fixup triggers: *rw == 640, scale = native/640 (we scale up). */
        {
            float cur_rw = *rw;
            float cur_rh = *(volatile float*)0x4AAF0C;
            if (cur_rw > 0.0f && cur_rh > 0.0f) {
                g_backend.vertex_scale_x = nw / cur_rw;
                g_backend.vertex_scale_y = nh / cur_rh;
            }
        }
    }

    /* Z-buffer clear (we bypassed M2DX's viewport Clear) */
    if (g_backend.depth_dsv) {
        ID3D11DeviceContext_ClearDepthStencilView(g_backend.context,
            g_backend.depth_dsv, D3D11_CLEAR_DEPTH, 1.0f, 0);
    }

    /* Clear backbuffer RT to opaque black */
    if (g_backend.backbuffer && g_backend.backbuffer->d3d11_rtv) {
        float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        ID3D11DeviceContext_ClearRenderTargetView(g_backend.context,
            g_backend.backbuffer->d3d11_rtv, black);
    }

    /* Upload BltFast content to RT before D3D renders on top */
    if (self->render_target && self->render_target->sys_buffer && self->render_target->dirty)
        WrapperSurface_FlushDirty(self->render_target);

    g_backend.in_scene = 1;
    g_backend.scene_rendered = 0;  /* Reset for new frame — set to 1 by EndScene */
    return DD_OK;
}

/* 10: EndScene */
static HRESULT __stdcall Dev3_EndScene(WrapperDevice *self)
{
    (void)self;
    g_backend.in_scene = 0;
    g_backend.scene_rendered = 1;
    return DD_OK;
}

/* 11: GetDirect3D */
static HRESULT __stdcall Dev3_GetDirect3D(WrapperDevice *self, void **d3d)
{
    (void)self;
    if (!d3d) return E_POINTER;
    *d3d = g_backend.d3d;
    return DD_OK;
}

/* 12: SetCurrentViewport */
static HRESULT __stdcall Dev3_SetCurrentViewport(WrapperDevice *self, WrapperViewport *vp)
{
    self->viewport = vp;
    return DD_OK;
}

/* 13: GetCurrentViewport */
static HRESULT __stdcall Dev3_GetCurrentViewport(WrapperDevice *self, void **vp)
{
    if (!vp) return E_POINTER;
    *vp = self->viewport;
    if (self->viewport) {
        typedef ULONG (__stdcall *AddRefFn)(void*);
        AddRefFn addref = (AddRefFn)(((void**)((void**)self->viewport)[0])[1]);
        addref(self->viewport);
    }
    return DD_OK;
}

/* 14: SetRenderTarget */
static HRESULT __stdcall Dev3_SetRenderTarget(WrapperDevice *self, WrapperSurface *target, DWORD flags)
{
    (void)flags;
    self->render_target = target;

    if (g_backend.context && target && target->d3d11_rtv) {
        ID3D11DeviceContext_OMSetRenderTargets(g_backend.context, 1,
            &target->d3d11_rtv, g_backend.depth_dsv);
    }
    return DD_OK;
}

/* 15: GetRenderTarget */
static HRESULT __stdcall Dev3_GetRenderTarget(WrapperDevice *self, void **surface)
{
    if (!surface) return E_POINTER;
    *surface = self->render_target;
    return DD_OK;
}

/* 16-20: Immediate mode (Begin/BeginIndexed/Vertex/Index/End) - not used by TD5 */
static HRESULT __stdcall Dev3_Begin(WrapperDevice *self, DWORD p, DWORD f, DWORD fl) { (void)self;(void)p;(void)f;(void)fl; WRAPPER_STUB("Dev3::Begin"); }
static HRESULT __stdcall Dev3_BeginIndexed(WrapperDevice *self, DWORD p, DWORD f, void *v, DWORD c, DWORD fl) { (void)self;(void)p;(void)f;(void)v;(void)c;(void)fl; WRAPPER_STUB("Dev3::BeginIndexed"); }
static HRESULT __stdcall Dev3_Vertex(WrapperDevice *self, void *v) { (void)self;(void)v; WRAPPER_STUB("Dev3::Vertex"); }
static HRESULT __stdcall Dev3_Index(WrapperDevice *self, WORD i) { (void)self;(void)i; WRAPPER_STUB("Dev3::Index"); }
static HRESULT __stdcall Dev3_End(WrapperDevice *self, DWORD f) { (void)self;(void)f; WRAPPER_STUB("Dev3::End"); }

/* 21: GetRenderState */
static HRESULT __stdcall Dev3_GetRenderState(WrapperDevice *self, DWORD type, DWORD *value)
{
    (void)self;
    if (!value) return E_POINTER;

    /* Return cached values */
    switch (type) {
    case D3DRS6_ZENABLE:         *value = g_backend.state.z_enable; break;
    case D3DRS6_ZWRITEENABLE:    *value = g_backend.state.z_write; break;
    case D3DRS6_ALPHABLENDENABLE:*value = g_backend.state.blend_enable; break;
    case D3DRS6_SRCBLEND:        *value = g_backend.state.src_blend; break;
    case D3DRS6_DESTBLEND:       *value = g_backend.state.dest_blend; break;
    case D3DRS6_FOGENABLE:       *value = g_backend.state.fog_enable; break;
    case D3DRS6_FOGCOLOR:        *value = g_backend.state.fog_color; break;
    default: *value = 0; break;
    }
    return DD_OK;
}

/* 22: SetRenderState -- CRITICAL: update state cache */
static HRESULT __stdcall Dev3_SetRenderState(WrapperDevice *self, DWORD type, DWORD value)
{
    RenderStateCache *s = &g_backend.state;
    (void)self;

    if (!g_backend.device) return DDERR_GENERIC;

    switch (type) {

    case D3DRS6_ZENABLE:
        s->z_enable = value ? 1 : 0;
        s->dirty = 1;
        break;

    case D3DRS6_ZWRITEENABLE:
        s->z_write = value ? 1 : 0;
        s->dirty = 1;
        break;

    case D3DRS6_ALPHABLENDENABLE:
        s->blend_enable = value ? 1 : 0;
        g_backend.alpha_blend_enabled = value ? 1 : 0;
        s->dirty = 1;
        break;

    case D3DRS6_SRCBLEND:
        s->src_blend = value;
        s->dirty = 1;
        break;

    case D3DRS6_DESTBLEND:
        s->dest_blend = value;
        s->dirty = 1;
        break;

    case D3DRS6_TEXTUREMAPBLEND:
        s->texblend_mode = (int)value;
        s->dirty = 1;
        break;

    case D3DRS6_FOGENABLE:
        s->fog_enable = value ? 1 : 0;
        s->dirty = 1;
        Backend_UpdateFogCB();
        break;

    case D3DRS6_FOGCOLOR:
        s->fog_color = value;
        if (s->fog_enable) Backend_UpdateFogCB();
        break;

    case D3DRS6_FOGTABLEMODE:
        s->fog_mode = (int)value;
        break;

    case D3DRS6_FOGSTART:
        /* Value is float bits as DWORD */
        s->fog_start = *(float*)&value;
        if (s->fog_enable) Backend_UpdateFogCB();
        break;

    case D3DRS6_FOGEND:
        s->fog_end = *(float*)&value;
        if (s->fog_enable) Backend_UpdateFogCB();
        break;

    case D3DRS6_FOGDENSITY:
        s->fog_density = *(float*)&value;
        if (s->fog_enable) Backend_UpdateFogCB();
        break;

    case D3DRS6_TEXADDRESSU:
        s->tex_address_u = value;
        s->dirty = 1;
        break;

    case D3DRS6_TEXADDRESSV:
        s->tex_address_v = value;
        s->dirty = 1;
        break;

    case D3DRS6_COLORKEYENABLE:
        /* Color key handled via alpha test in pixel shaders.
         * When enabled, pixels with alpha < alphaRef are discarded.
         * This is critical for A1R5G5B5 textures where transparent
         * pixels have alpha=0 and must not render. */
        s->alpha_test_enable = value ? 1 : 0;
        if (value && s->alpha_ref == 0)
            s->alpha_ref = 1;  /* Default: discard alpha=0 pixels (1/255 threshold) */
        s->dirty = 1;
        break;

    case D3DRS6_ZFUNC:
        /* D3DCMP_LESSEQUAL=4, D3DCMP_ALWAYS=8 */
        s->z_func = (value == 8) ? 1 : 0;
        s->dirty = 1;
        break;

    /* Ignored states */
    case D3DRS6_FILLMODE:
    case D3DRS6_SHADEMODE:
    case D3DRS6_CULLMODE:
    case D3DRS6_DITHERENABLE:
    case D3DRS6_SPECULARENABLE:
    case D3DRS6_STIPPLEDALPHA:
        break;

    default:
        WRAPPER_LOG("SetRenderState: unhandled type %u = 0x%X", type, value);
        break;
    }
    return DD_OK;
}

/* 23: GetLightState */
static HRESULT __stdcall Dev3_GetLightState(WrapperDevice *self, DWORD type, DWORD *value)
{ (void)self;(void)type;(void)value; WRAPPER_STUB("Dev3::GetLightState"); }

/* 24: SetLightState */
static HRESULT __stdcall Dev3_SetLightState(WrapperDevice *self, DWORD type, DWORD value)
{ (void)self;(void)type;(void)value; WRAPPER_STUB("Dev3::SetLightState"); }

/* 25: SetTransform — game uses XYZRHW, transforms are unused */
static HRESULT __stdcall Dev3_SetTransform(WrapperDevice *self, DWORD type, void *matrix)
{ (void)self;(void)type;(void)matrix; return DD_OK; }

/* 26: GetTransform */
static HRESULT __stdcall Dev3_GetTransform(WrapperDevice *self, DWORD type, void *matrix)
{ (void)self;(void)type;(void)matrix; WRAPPER_STUB("Dev3::GetTransform"); }

/* 27: MultiplyTransform */
static HRESULT __stdcall Dev3_MultiplyTransform(WrapperDevice *self, DWORD type, void *matrix)
{ (void)self;(void)type;(void)matrix; WRAPPER_STUB("Dev3::MultiplyTransform"); }

/* ========================================================================
 * 28: DrawPrimitive -- CRITICAL: main rendering path
 *
 * Upload vertices to dynamic VB, apply state cache, draw.
 * ======================================================================== */

static HRESULT __stdcall Dev3_DrawPrimitive(WrapperDevice *self,
    DWORD prim_type, DWORD fvf, void *verts, DWORD vert_count, DWORD flags)
{
    ID3D11DeviceContext *ctx = g_backend.context;
    D3D11_MAPPED_SUBRESOURCE mapped;
    UINT stride, data_size;
    UINT offset = 0;
    HRESULT hr;
    (void)self; (void)fvf; (void)flags;

    if (!ctx || !verts || vert_count == 0)
        return DDERR_GENERIC;

    stride = TD5_VERTEX_STRIDE;
    data_size = vert_count * stride;

    /* Scale pre-transformed vertex X/Y from game space to native RT space */
    if (g_backend.vertex_scale_x != 1.0f || g_backend.vertex_scale_y != 1.0f) {
        DWORD i;
        float sx = g_backend.vertex_scale_x;
        float sy = g_backend.vertex_scale_y;
        for (i = 0; i < vert_count; i++) {
            float *v = (float*)((BYTE*)verts + i * stride);
            v[0] *= sx;
            v[1] *= sy;
        }
    }

    /* Upload vertices to dynamic VB */
    hr = ID3D11DeviceContext_Map(ctx, (ID3D11Resource*)g_backend.dynamic_vb,
        0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr)) return DDERR_GENERIC;
    memcpy(mapped.pData, verts, data_size);
    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource*)g_backend.dynamic_vb, 0);

    s_frame_draws++;

    /* Bind VB and set topology */
    ID3D11DeviceContext_IASetVertexBuffers(ctx, 0, 1, &g_backend.dynamic_vb, &stride, &offset);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, MapTopology(prim_type));

    /* Ensure correct input layout and VS are bound */
    ID3D11DeviceContext_IASetInputLayout(ctx, g_backend.input_layout);
    ID3D11DeviceContext_VSSetShader(ctx, g_backend.vs_pretransformed, NULL, 0);

    /* Apply cached render state → D3D11 state objects */
    Backend_ApplyStateCache();

    /* Log primitive types to detect triangle fan usage */
    {
        static int s_draw_log = 0;
        static int s_prim_counts[8] = {0};
        if (prim_type < 8) s_prim_counts[prim_type]++;
        if (s_draw_log < 10) {
            WRAPPER_LOG("DrawPrimitive: verts=%u type=%u srv=%p z=%d blend=%d",
                vert_count, prim_type, g_backend.current_srv,
                g_backend.state.z_enable, g_backend.state.blend_enable);
            s_draw_log++;
        }
    }

    ID3D11DeviceContext_Draw(ctx, vert_count, 0);
    return DD_OK;
}

/* 29: DrawIndexedPrimitive -- CRITICAL: indexed rendering path */
static HRESULT __stdcall Dev3_DrawIndexedPrimitive(WrapperDevice *self,
    DWORD prim_type, DWORD fvf, void *verts, DWORD vert_count,
    WORD *indices, DWORD index_count, DWORD flags)
{
    ID3D11DeviceContext *ctx = g_backend.context;
    D3D11_MAPPED_SUBRESOURCE mapped;
    UINT stride = TD5_VERTEX_STRIDE;
    UINT offset = 0;
    HRESULT hr;
    (void)self; (void)fvf; (void)flags;

    if (!ctx || !verts || !indices || vert_count == 0 || index_count == 0)
        return DDERR_GENERIC;

    /* Scale pre-transformed vertex X/Y */
    if (g_backend.vertex_scale_x != 1.0f || g_backend.vertex_scale_y != 1.0f) {
        DWORD i;
        float sx = g_backend.vertex_scale_x;
        float sy = g_backend.vertex_scale_y;
        for (i = 0; i < vert_count; i++) {
            float *v = (float*)((BYTE*)verts + i * stride);
            v[0] *= sx;
            v[1] *= sy;
        }
    }

    /* Upload vertices */
    hr = ID3D11DeviceContext_Map(ctx, (ID3D11Resource*)g_backend.dynamic_vb,
        0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr)) return DDERR_GENERIC;
    memcpy(mapped.pData, verts, vert_count * stride);
    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource*)g_backend.dynamic_vb, 0);

    s_frame_idx_draws++;
    if (prim_type < 8) s_frame_prim_types[prim_type]++;
    if (prim_type == D3DPT_TRIANGLEFAN) s_frame_fan_verts += vert_count;

    /* Upload indices */
    hr = ID3D11DeviceContext_Map(ctx, (ID3D11Resource*)g_backend.dynamic_ib,
        0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr)) return DDERR_GENERIC;
    memcpy(mapped.pData, indices, index_count * sizeof(WORD));
    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource*)g_backend.dynamic_ib, 0);

    /* Bind buffers */
    ID3D11DeviceContext_IASetVertexBuffers(ctx, 0, 1, &g_backend.dynamic_vb, &stride, &offset);
    ID3D11DeviceContext_IASetIndexBuffer(ctx, g_backend.dynamic_ib, DXGI_FORMAT_R16_UINT, 0);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, MapTopology(prim_type));
    ID3D11DeviceContext_IASetInputLayout(ctx, g_backend.input_layout);
    ID3D11DeviceContext_VSSetShader(ctx, g_backend.vs_pretransformed, NULL, 0);

    Backend_ApplyStateCache();

    /* Count blend/shader stats AFTER state cache applied */
    if (g_backend.state.blend_enable) s_frame_blend_on++;
    else s_frame_blend_off++;
    if (g_backend.state.current_ps_idx == PS_LUMINANCE_ALPHA) s_frame_ps_lum++;
    else s_frame_ps_mod++;

    /* Diagnostic: on frames 3-5, log ALL draws with primitive type */
    if (s_frame_num >= 3 && s_frame_num <= 5) {
        float *v0 = (float*)verts;
        float u0 = v0[6];  /* offset 24/4 = 6 */
        float v_0 = v0[7]; /* offset 28/4 = 7 */
        DWORD diff = *(DWORD*)((BYTE*)verts + 16);
        WRAPPER_LOG("  DIP[%d]: type=%u verts=%u idx=%u blend=%d ps=%d z=%d "
            "pos=(%.1f,%.1f,%.4f,%.4f) diff=%08X uv=(%.3f,%.3f) srv=%p r5g6b5=%d",
            s_frame_idx_draws, prim_type, vert_count, index_count,
            g_backend.state.blend_enable, g_backend.state.current_ps_idx,
            g_backend.state.z_enable,
            v0[0], v0[1], v0[2], v0[3], diff, u0, v_0,
            g_backend.current_srv, !g_backend.current_tex_has_alpha);
    }

    ID3D11DeviceContext_DrawIndexed(ctx, index_count, 0, 0);
    return DD_OK;
}

/* 30-31: ClipStatus */
static HRESULT __stdcall Dev3_SetClipStatus(WrapperDevice *self, void *s) { (void)self;(void)s; WRAPPER_STUB("Dev3::SetClipStatus"); }
static HRESULT __stdcall Dev3_GetClipStatus(WrapperDevice *self, void *s) { (void)self;(void)s; WRAPPER_STUB("Dev3::GetClipStatus"); }

/* 32-35: Strided / VB draws — not used by TD5 */
static HRESULT __stdcall Dev3_DrawPrimitiveStrided(WrapperDevice *self, DWORD p, DWORD f, void *s, DWORD c, DWORD fl)
{ (void)self;(void)p;(void)f;(void)s;(void)c;(void)fl; WRAPPER_STUB("Dev3::DrawPrimitiveStrided"); }
static HRESULT __stdcall Dev3_DrawIndexedPrimitiveStrided(WrapperDevice *self, DWORD p, DWORD f, void *s, DWORD c, WORD *i, DWORD ic, DWORD fl)
{ (void)self;(void)p;(void)f;(void)s;(void)c;(void)i;(void)ic;(void)fl; WRAPPER_STUB("Dev3::DrawIndexedPrimitiveStrided"); }
static HRESULT __stdcall Dev3_DrawPrimitiveVB(WrapperDevice *self, DWORD p, void *v, DWORD s, DWORD c, DWORD f)
{ (void)self;(void)p;(void)v;(void)s;(void)c;(void)f; WRAPPER_STUB("Dev3::DrawPrimitiveVB"); }
static HRESULT __stdcall Dev3_DrawIndexedPrimitiveVB(WrapperDevice *self, DWORD p, void *v, WORD *i, DWORD ic, DWORD f)
{ (void)self;(void)p;(void)v;(void)i;(void)ic;(void)f; WRAPPER_STUB("Dev3::DrawIndexedPrimitiveVB"); }

/* 36: ComputeSphereVisibility */
static HRESULT __stdcall Dev3_ComputeSphereVisibility(WrapperDevice *self, void *s, DWORD c, DWORD f, DWORD *r)
{ (void)self;(void)s;(void)c;(void)f;(void)r; WRAPPER_STUB("Dev3::ComputeSphereVisibility"); }

/* 37: GetTexture */
static HRESULT __stdcall Dev3_GetTexture(WrapperDevice *self, DWORD stage, void **out)
{ (void)self;(void)stage;(void)out; WRAPPER_STUB("Dev3::GetTexture"); }

/* 38: SetTexture -- CRITICAL: bind texture SRV for rendering */
static int s_settex_log = 0;

static HRESULT __stdcall Dev3_SetTexture(WrapperDevice *self, DWORD stage, WrapperTexture *tex)
{
    (void)self;
    if (!g_backend.context) return DDERR_GENERIC;

    s_frame_settex++;
    if (s_settex_log < 20)
        WRAPPER_LOG("Dev3_SetTexture: stage=%u tex=%p", stage, tex);

    if (!tex) {
        if (stage == 0) {
            g_backend.current_tex_has_alpha = 1;
            g_backend.current_srv = NULL;
            ID3D11ShaderResourceView *null_srv = NULL;
            ID3D11DeviceContext_PSSetShaderResources(g_backend.context, 0, 1, &null_srv);
            g_backend.state.dirty = 1;
        }
        return DD_OK;
    }

    /* Get SRV from WrapperTexture (survives surface destruction) or parent surface */
    {
        ID3D11ShaderResourceView *srv = tex->d3d11_srv;
        if (!srv && tex->surface)
            srv = tex->surface->d3d11_srv;

        if (s_settex_log < 20) {
            WRAPPER_LOG("  tex->d3d11_srv=%p surf=%p surf->d3d11_srv=%p r5g6b5=%d",
                tex->d3d11_srv, tex->surface,
                tex->surface ? tex->surface->d3d11_srv : NULL,
                tex->r5g6b5_source);
            s_settex_log++;
        }

        if (srv) {
            /* CRITICAL: prevent binding the backbuffer's own SRV as a texture.
             * D3D11 detects this RT/SRV conflict and automatically UNBINDS the RTV,
             * causing all subsequent draws to produce no output. This happens when
             * surface reuse causes a WrapperTexture's SRV to point to the backbuffer. */
            if (g_backend.backbuffer && srv == g_backend.backbuffer->d3d11_srv) {
                static int s_warn = 0;
                if (s_warn < 5) {
                    WRAPPER_LOG("  WARNING: blocked RT/SRV hazard! tex=%p srv=%p == backbuffer srv", tex, srv);
                    s_warn++;
                }
                /* Bind NULL instead to avoid the hazard */
                ID3D11ShaderResourceView *null_srv = NULL;
                ID3D11DeviceContext_PSSetShaderResources(g_backend.context, stage, 1, &null_srv);
                return DD_OK;
            }

            if (stage == 0) {
                g_backend.current_srv = srv;
                g_backend.current_tex_has_alpha = !tex->r5g6b5_source;
                g_backend.state.dirty = 1;
            }
            ID3D11DeviceContext_PSSetShaderResources(g_backend.context, stage, 1, &srv);
        } else {
            WRAPPER_LOG("  WARNING: no SRV for texture %p!", tex);
            if (stage == 0) {
                g_backend.current_tex_has_alpha = 1;
                g_backend.state.dirty = 1;
            }
            ID3D11ShaderResourceView *null_srv = NULL;
            ID3D11DeviceContext_PSSetShaderResources(g_backend.context, stage, 1, &null_srv);
        }
    }
    return DD_OK;
}

/* 39: GetTextureStageState */
static HRESULT __stdcall Dev3_GetTextureStageState(WrapperDevice *self, DWORD s, DWORD t, DWORD *v)
{ (void)self;(void)s;(void)t;(void)v; WRAPPER_STUB("Dev3::GetTextureStageState"); }

/* 40: SetTextureStageState — filter states update sampler cache */
static HRESULT __stdcall Dev3_SetTextureStageState(WrapperDevice *self,
    DWORD stage, DWORD type, DWORD value)
{
    (void)self; (void)stage;
    if (!g_backend.device) return DDERR_GENERIC;

    switch (type) {
    case D3DTSS6_MAGFILTER:
        g_backend.state.mag_filter = value;
        g_backend.state.dirty = 1;
        break;
    case D3DTSS6_MINFILTER:
        g_backend.state.min_filter = value;
        g_backend.state.dirty = 1;
        break;
    case D3DTSS6_MIPFILTER:
    case D3DTSS6_MIPMAPLODBIAS:
        /* Mip filtering handled by sampler state; D3D11 sampler already
         * includes mip filter in the preset. No separate tracking needed. */
        break;
    default:
        /* Other TSS types (COLOROP, etc.) are handled by our pixel shaders
         * via the texblend_mode in the state cache. No action needed. */
        break;
    }
    return DD_OK;
}

/* 41: ValidateDevice */
static HRESULT __stdcall Dev3_ValidateDevice(WrapperDevice *self, DWORD *passes)
{
    (void)self;
    if (passes) *passes = 1;
    return DD_OK;
}

/* 42: Initialize (IDirect3DDevice3 has this at a different vtable slot than Device1) */
static HRESULT __stdcall Dev3_Initialize(WrapperDevice *self, void *d3d, GUID *guid, void *desc)
{ (void)self;(void)d3d;(void)guid;(void)desc; WRAPPER_STUB("Dev3::Initialize"); }

/* ========================================================================
 * Vtable -- matches the COM binary layout M2DX.dll expects
 *
 * IDirect3DDevice3 has a FLAT vtable (not inheriting Device/Device2).
 * Layout from d3d.h: IUnknown(3) + 40 methods = 43 entries
 * ======================================================================== */

static void *device3_vtbl[] = {
    /*  0 */ Dev3_QueryInterface,
    /*  1 */ Dev3_AddRef,
    /*  2 */ Dev3_Release,
    /*  3 */ Dev3_GetCaps,
    /*  4 */ Dev3_GetStats,
    /*  5 */ Dev3_AddViewport,
    /*  6 */ Dev3_DeleteViewport,
    /*  7 */ Dev3_NextViewport,
    /*  8 */ Dev3_EnumTextureFormats,
    /*  9 */ Dev3_BeginScene,
    /* 10 */ Dev3_EndScene,
    /* 11 */ Dev3_GetDirect3D,
    /* 12 */ Dev3_SetCurrentViewport,
    /* 13 */ Dev3_GetCurrentViewport,
    /* 14 */ Dev3_SetRenderTarget,
    /* 15 */ Dev3_GetRenderTarget,
    /* 16 */ Dev3_Begin,
    /* 17 */ Dev3_BeginIndexed,
    /* 18 */ Dev3_Vertex,
    /* 19 */ Dev3_Index,
    /* 20 */ Dev3_End,
    /* 21 */ Dev3_GetRenderState,
    /* 22 */ Dev3_SetRenderState,
    /* 23 */ Dev3_GetLightState,
    /* 24 */ Dev3_SetLightState,
    /* 25 */ Dev3_SetTransform,
    /* 26 */ Dev3_GetTransform,
    /* 27 */ Dev3_MultiplyTransform,
    /* 28 */ Dev3_DrawPrimitive,
    /* 29 */ Dev3_DrawIndexedPrimitive,
    /* 30 */ Dev3_SetClipStatus,
    /* 31 */ Dev3_GetClipStatus,
    /* 32 */ Dev3_DrawPrimitiveStrided,
    /* 33 */ Dev3_DrawIndexedPrimitiveStrided,
    /* 34 */ Dev3_DrawPrimitiveVB,
    /* 35 */ Dev3_DrawIndexedPrimitiveVB,
    /* 36 */ Dev3_ComputeSphereVisibility,
    /* 37 */ Dev3_GetTexture,
    /* 38 */ Dev3_SetTexture,
    /* 39 */ Dev3_GetTextureStageState,
    /* 40 */ Dev3_SetTextureStageState,
    /* 41 */ Dev3_ValidateDevice,
};

/* ========================================================================
 * Constructor
 * ======================================================================== */

WrapperDevice* WrapperDevice_Create(WrapperSurface *render_target)
{
    /* Static storage — prevents heap corruption from overwriting vtable */
    static WrapperDevice s_device_storage;
    WrapperDevice *obj = &s_device_storage;
    ZeroMemory(obj, sizeof(WrapperDevice));

    obj->vtbl = device3_vtbl;
    obj->ref_count = 1;
    obj->render_target = render_target;
    obj->viewport = NULL;

    WRAPPER_LOG("WrapperDevice_Create -> %p (target=%p)", obj, render_target);
    return obj;
}
