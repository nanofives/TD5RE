/**
 * d3d11_backend_draw.c - Present-time compositing and WrapperClipper
 *
 * Split out of d3d11_backend.c (2026-07-09, A3 refactor):
 *   - Compositing: merge BltFast (2D) and D3D (3D) layers at present time
 *   - WrapperClipper stub (replaces IDirectDrawClipper; no real clip-list
 *     support needed -- the game only queries/sets an HWND)
 *
 * See also d3d11_backend_device.c (device lifecycle/resource creation) and
 * d3d11_backend_pipeline.c (per-draw state/lighting passes).
 */

#include "wrapper.h"
#include "d3d11_backend_priv.h"   /* BackendTexture body + bt_* surface accessors */

/* Backend-agnostic frame clears (see wrapper.h). Used by the shared COM files
 * so they issue no ID3D11 clear calls directly. */
void Backend_ClearBackbuffer(const float *rgba)
{
    ID3D11RenderTargetView *rtv = bt_rtv(g_backend.backbuffer);
    if (!g_backend.context) return;
    if (rtv)
        ID3D11DeviceContext_ClearRenderTargetView(g_backend.context, rtv, rgba);
    else if (g_backend.swap_rtv)
        ID3D11DeviceContext_ClearRenderTargetView(g_backend.context, g_backend.swap_rtv, rgba);
}

void Backend_ClearDepth(float z)
{
    if (g_backend.context && g_backend.depth_dsv)
        ID3D11DeviceContext_ClearDepthStencilView(g_backend.context,
            g_backend.depth_dsv, D3D11_CLEAR_DEPTH, z, 0);
}

/* D3D6 primitive type -> D3D11 topology (moved out of device3.c). */
static D3D11_PRIMITIVE_TOPOLOGY bt_map_topology(DWORD d3d6_prim)
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

/* Never leave PS slot 0 sampling a NULL SRV (observed to fault inside the NVIDIA
 * UMD at Present under very high split-screen draw counts). Bind the 1x1 white
 * texture instead -- with PS_MODULATE white*diffuse == diffuse. */
static void bt_guard_slot0_srv(ID3D11DeviceContext *ctx)
{
    if (!g_backend.current_srv && g_backend.white_srv) {
        g_backend.current_srv = g_backend.white_srv;
        ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &g_backend.white_srv);
    }
}

void Backend_DrawPrimitive(DWORD prim_type, UINT stride,
                           UINT base_vertex, UINT vert_count)
{
    ID3D11DeviceContext *ctx = g_backend.context;
    UINT offset = 0;
    if (!ctx) return;

    ID3D11DeviceContext_IASetVertexBuffers(ctx, 0, 1, &g_backend.dynamic_vb, &stride, &offset);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, bt_map_topology(prim_type));
    ID3D11DeviceContext_IASetInputLayout(ctx, g_backend.input_layout);
    ID3D11DeviceContext_VSSetShader(ctx, g_backend.vs_pretransformed, NULL, 0);

    Backend_ApplyStateCache();
    bt_guard_slot0_srv(ctx);

    Backend_NoteDraw(prim_type, vert_count, 0, 0);   /* [DRAW WATCH] */
    ID3D11DeviceContext_Draw(ctx, vert_count, base_vertex);
}

void Backend_DrawIndexedPrimitive(DWORD prim_type, UINT stride, UINT base_vertex,
                                  UINT start_index, UINT index_count, UINT vert_count)
{
    ID3D11DeviceContext *ctx = g_backend.context;
    UINT offset = 0;
    if (!ctx) return;

    ID3D11DeviceContext_IASetVertexBuffers(ctx, 0, 1, &g_backend.dynamic_vb, &stride, &offset);
    ID3D11DeviceContext_IASetIndexBuffer(ctx, g_backend.dynamic_ib, DXGI_FORMAT_R16_UINT, 0);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, bt_map_topology(prim_type));
    ID3D11DeviceContext_IASetInputLayout(ctx, g_backend.input_layout);
    ID3D11DeviceContext_VSSetShader(ctx, g_backend.vs_pretransformed, NULL, 0);

    Backend_ApplyStateCache();
    bt_guard_slot0_srv(ctx);

    Backend_NoteDraw(prim_type, vert_count, index_count, 1);   /* [DRAW WATCH] */
    ID3D11DeviceContext_DrawIndexed(ctx, index_count, start_index, (INT)base_vertex);
}

void Backend_SetViewport(float x, float y, float w, float h, float min_z, float max_z)
{
    D3D11_VIEWPORT vp;
    if (!g_backend.context) return;
    vp.TopLeftX = x; vp.TopLeftY = y;
    vp.Width = w; vp.Height = h;
    vp.MinDepth = min_z; vp.MaxDepth = max_z;
    ID3D11DeviceContext_RSSetViewports(g_backend.context, 1, &vp);
}

/* ---- Vector-UI renderer backend API (see wrapper.h) -------------------- */

BackendPixelShader *Backend_CreatePixelShader(const void *bytecode, size_t len)
{
    BackendPixelShader *h;
    if (!g_backend.device || !bytecode) return NULL;
    h = (BackendPixelShader*)calloc(1, sizeof(*h));
    if (!h) return NULL;
    if (FAILED(ID3D11Device_CreatePixelShader(g_backend.device, bytecode, len, NULL, &h->ps))) {
        free(h);
        return NULL;
    }
    return h;
}

void Backend_ReleasePixelShader(BackendPixelShader *h)
{
    if (!h) return;
    if (h->ps) ID3D11PixelShader_Release(h->ps);
    free(h);
}

BackendConstBuffer *Backend_CreateConstBuffer(size_t size)
{
    BackendConstBuffer *h;
    D3D11_BUFFER_DESC bd;
    if (!g_backend.device) return NULL;
    h = (BackendConstBuffer*)calloc(1, sizeof(*h));
    if (!h) return NULL;
    ZeroMemory(&bd, sizeof(bd));
    bd.ByteWidth = (UINT)size;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(ID3D11Device_CreateBuffer(g_backend.device, &bd, NULL, &h->cb))) {
        free(h);
        return NULL;
    }
    return h;
}

void Backend_ReleaseConstBuffer(BackendConstBuffer *h)
{
    if (!h) return;
    if (h->cb) ID3D11Buffer_Release(h->cb);
    free(h);
}

void Backend_UpdateConstBuffer(BackendConstBuffer *h, const void *data, size_t size)
{
    (void)size;
    if (h && h->cb && g_backend.context)
        ID3D11DeviceContext_UpdateSubresource(g_backend.context,
            (ID3D11Resource*)h->cb, 0, NULL, data, 0, 0);
}

void Backend_BindConstBuffer(UINT slot, BackendConstBuffer *h)
{
    if (h && h->cb && g_backend.context)
        ID3D11DeviceContext_PSSetConstantBuffers(g_backend.context, slot, 1, &h->cb);
}

void Backend_SetBuiltinPixelShader(int ps_idx)
{
    if (g_backend.context && ps_idx >= 0 && ps_idx < PS_COUNT)
        ID3D11DeviceContext_PSSetShader(g_backend.context, g_backend.ps_shaders[ps_idx], NULL, 0);
}

void Backend_BindSampler(UINT slot, int sampler_idx)
{
    if (g_backend.context && sampler_idx >= 0 && sampler_idx < SAMP_STATE_COUNT)
        ID3D11DeviceContext_PSSetSamplers(g_backend.context, slot, 1,
            &g_backend.sampler_states[sampler_idx]);
}

void Backend_ForceBlendState(int blend_idx)
{
    if (g_backend.context && blend_idx >= 0 && blend_idx < BLEND_STATE_COUNT &&
        g_backend.blend_states[blend_idx]) {
        ID3D11DeviceContext_OMSetBlendState(g_backend.context,
            g_backend.blend_states[blend_idx], NULL, 0xFFFFFFFF);
        g_backend.state.current_blend_idx = blend_idx;
    }
}

void *Backend_PixelShaderRaw(BackendPixelShader *h)
{
    return h ? (void*)h->ps : NULL;
}

/* ---- platform-renderer draw entry points (see wrapper.h) -------------- */

void Backend_PlatDrawTris(WrapperRecCtx *rc, const void *verts, int vert_count,
                          const void *indices, int index_count,
                          void *ps_override, int ps_override_samp)
{
    ID3D11DeviceContext *ctx = rc ? rc->dc : g_backend.context;
    ID3D11Buffer *vb    = rc ? rc->vb : g_backend.dynamic_vb;
    ID3D11Buffer *ib    = rc ? rc->ib : g_backend.dynamic_ib;
    ID3D11Buffer *cbvp  = rc ? rc->cb_viewport : g_backend.cb_viewport;
    ID3D11Buffer *cbfog = rc ? rc->cb_fog : g_backend.cb_fog;
    RenderStateCache *st = rc ? &rc->state : &g_backend.state;
    UINT stride = TD5_VERTEX_STRIDE, offset = 0, base_vertex = 0, start_index = 0;
    int has_idx = (indices && index_count > 0);

    if (!ctx || !verts || vert_count <= 0) return;
    if (!Backend_StreamUpload(verts, (UINT)vert_count, stride,
                              has_idx ? indices : NULL, has_idx ? (UINT)index_count : 0,
                              &base_vertex, &start_index))
        return;

    ID3D11DeviceContext_IASetVertexBuffers(ctx, 0, 1, &vb, &stride, &offset);
    ID3D11DeviceContext_IASetInputLayout(ctx, g_backend.input_layout);
    ID3D11DeviceContext_VSSetShader(ctx, g_backend.vs_pretransformed, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(ctx, 0, 1, &cbvp);
    ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 1, &cbfog);

    Backend_ApplyStateCache();

    if (has_idx) {
        ID3D11DeviceContext_IASetIndexBuffer(ctx, ib, DXGI_FORMAT_R16_UINT, 0);
        ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        /* Force correct PS + sampler before every draw (honour MSDF override) */
        if (ps_override) {
            ID3D11DeviceContext_PSSetShader(ctx, (ID3D11PixelShader *)ps_override, NULL, 0);
            ID3D11DeviceContext_PSSetSamplers(ctx, 0, 1, &g_backend.sampler_states[ps_override_samp]);
        } else {
            ID3D11DeviceContext_PSSetShader(ctx,
                g_backend.ps_shaders[st->texblend_mode == 5 ? 1 : 0], NULL, 0);
            {
                int si = (st->mag_filter >= 2) ? SAMP_LINEAR_WRAP : SAMP_POINT_WRAP;
                ID3D11DeviceContext_PSSetSamplers(ctx, 0, 1, &g_backend.sampler_states[si]);
            }
        }
        Backend_NoteDraw(4, (unsigned)vert_count, (unsigned)index_count, 1);
        ID3D11DeviceContext_DrawIndexed(ctx, (UINT)index_count, start_index, (INT)base_vertex);
    } else {
        ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        Backend_NoteDraw(4, (unsigned)vert_count, 0, 0);
        ID3D11DeviceContext_Draw(ctx, (UINT)vert_count, base_vertex);
    }
}

void Backend_PlatDrawWhite(WrapperRecCtx *rc, const void *verts, int vert_count,
                           const void *indices, int index_count, int is_lines)
{
    ID3D11DeviceContext *ctx = rc ? rc->dc : g_backend.context;
    ID3D11Buffer *vb    = rc ? rc->vb : g_backend.dynamic_vb;
    ID3D11Buffer *ib    = rc ? rc->ib : g_backend.dynamic_ib;
    ID3D11Buffer *cbvp  = rc ? rc->cb_viewport : g_backend.cb_viewport;
    ID3D11Buffer *cbfog = rc ? rc->cb_fog : g_backend.cb_fog;
    RenderStateCache *st = rc ? &rc->state : &g_backend.state;
    UINT stride = TD5_VERTEX_STRIDE, offset = 0, base_vertex = 0, start_index = 0;
    int has_idx = (!is_lines && indices && index_count > 0);

    if (!ctx || !verts || !g_backend.white_srv) return;
    if (!Backend_StreamUpload(verts, (UINT)vert_count, stride,
                              has_idx ? indices : NULL, has_idx ? (UINT)index_count : 0,
                              &base_vertex, has_idx ? &start_index : NULL))
        return;

    ID3D11DeviceContext_IASetVertexBuffers(ctx, 0, 1, &vb, &stride, &offset);
    ID3D11DeviceContext_IASetInputLayout(ctx, g_backend.input_layout);
    if (has_idx)
        ID3D11DeviceContext_IASetIndexBuffer(ctx, ib, DXGI_FORMAT_R16_UINT, 0);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx,
        is_lines ? D3D11_PRIMITIVE_TOPOLOGY_LINELIST : D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    ID3D11DeviceContext_VSSetShader(ctx, g_backend.vs_pretransformed, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(ctx, 0, 1, &cbvp);

    /* PS_MODULATE * white texel = vertex colour. Fog + alpha test off via a
     * fresh fog CB bounded to this draw. */
    ID3D11DeviceContext_PSSetShader(ctx, g_backend.ps_shaders[PS_MODULATE], NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &g_backend.white_srv);
    ID3D11DeviceContext_PSSetSamplers(ctx, 0, 1, &g_backend.sampler_states[SAMP_POINT_CLAMP]);
    ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 1, &cbfog);
    {
        FogCB fog = {0};
        fog.fogEnabled = 0;
        fog.alphaTestEnabled = 0;
        fog.alphaRef = 0.0f;
        ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource*)cbfog, 0, NULL, &fog, 0, 0);
    }

    ID3D11DeviceContext_RSSetState(ctx, g_backend.rs_state);
    /* Z test on (occlude correctly), Z write off (don't poison depth). */
    ID3D11DeviceContext_OMSetDepthStencilState(ctx, g_backend.ds_states[DS_Z_ON_WRITE_OFF], 0);
    ID3D11DeviceContext_OMSetBlendState(ctx, g_backend.blend_states[BLEND_OPAQUE], NULL, 0xFFFFFFFF);

    if (has_idx) {
        Backend_NoteDraw(4, (unsigned)vert_count, (unsigned)index_count, 1);
        ID3D11DeviceContext_DrawIndexed(ctx, (UINT)index_count, start_index, (INT)base_vertex);
    } else {
        Backend_NoteDraw(2, (unsigned)vert_count, 0, 0);
        ID3D11DeviceContext_Draw(ctx, (UINT)vert_count, base_vertex);
    }

    /* Invalidate cached state-object indices so the next ApplyStateCache re-binds. */
    st->current_blend_idx = -1;
    st->current_ds_idx    = -1;
    st->current_samp_idx  = -1;
    st->current_ps_idx    = -1;
    st->dirty = 1;
    /* Force texture rebind on next draw — current_srv was overwritten with white. */
    if (rc) rc->current_srv = NULL; else g_backend.current_srv = NULL;
}

/* ---- soft-particle depth binding (see wrapper.h) ---------------------- */

static ID3D11RenderTargetView *s_soft_saved_rtv = NULL;
static ID3D11DepthStencilView *s_soft_saved_dsv = NULL;
static int                     s_soft_active     = 0;

int Backend_BindSceneDepthReadonly(void)
{
    ID3D11DeviceContext *ctx = g_backend.context;
    if (!ctx || !g_backend.depth_srv || !g_backend.depth_dsv_readonly) return 0;

    /* Save the currently bound RTV + DSV (AddRef'd) so we can restore them. */
    s_soft_saved_rtv = NULL;
    s_soft_saved_dsv = NULL;
    ID3D11DeviceContext_OMGetRenderTargets(ctx, 1, &s_soft_saved_rtv, &s_soft_saved_dsv);

    /* Same colour target, but the READ-ONLY depth view (so depth can also be an
     * SRV), plus the depth SRV at t1 for the smoke shader. */
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &s_soft_saved_rtv, g_backend.depth_dsv_readonly);
    ID3D11DeviceContext_PSSetShaderResources(ctx, 1, 1, &g_backend.depth_srv);
    s_soft_active = 1;
    return 1;
}

void Backend_UnbindSceneDepthReadonly(void)
{
    ID3D11DeviceContext *ctx = g_backend.context;
    if (!ctx || !s_soft_active) return;
    s_soft_active = 0;

    /* Unbind t1 (avoids a read/write hazard when depth is next a writable
     * target), then restore the original RTV + writable DSV. */
    {
        ID3D11ShaderResourceView *null_srv = NULL;
        ID3D11DeviceContext_PSSetShaderResources(ctx, 1, 1, &null_srv);
    }
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &s_soft_saved_rtv, s_soft_saved_dsv);

    if (s_soft_saved_rtv) { ID3D11RenderTargetView_Release(s_soft_saved_rtv); s_soft_saved_rtv = NULL; }
    if (s_soft_saved_dsv) { ID3D11DepthStencilView_Release(s_soft_saved_dsv); s_soft_saved_dsv = NULL; }
}
#include <stdlib.h>
#include <string.h>
#include <dxgi1_3.h>

/* [x64 memory hunt 2026-07-30] TD5RE_DXGI_TRIM=N calls IDXGIDevice3::Trim()
 * every N presents (unset/0 = off). The x64 NVIDIA UMD retains ~48 idle 16 MB
 * system-memory pool chunks (~768 MB committed-but-untouched private bytes;
 * the i686 UMD keeps only ~4 — measured via VirtualQueryEx region dumps at
 * one frontend screen). Trim() is the documented request to release exactly
 * such internal idle pools. Local IID so no dxguid link dependency. */
static const GUID s_IID_IDXGIDevice3 =
    {0x6007896c, 0x3244, 0x4afd, {0xbf, 0x18, 0xa6, 0xd3, 0xbe, 0xda, 0x50, 0x23}};

void Backend_MaybeTrim(void)
{
    static int s_every = -2;               /* -2 = env not read yet */
    static unsigned s_present_count;
    IDXGIDevice3 *dev3 = NULL;

    if (s_every == -2) {
        const char *e = getenv("TD5RE_DXGI_TRIM");
        s_every = (e && e[0]) ? atoi(e) : 0;
    }
    if (s_every <= 0) return;
    if (++s_present_count % (unsigned)s_every) return;

    if (g_backend.device &&
        SUCCEEDED(ID3D11Device_QueryInterface(g_backend.device,
                                              &s_IID_IDXGIDevice3, (void **)&dev3))) {
        static int s_logged;
        IDXGIDevice3_Trim(dev3);
        IDXGIDevice3_Release(dev3);
        if (!s_logged) {
            FILE *f;
            s_logged = 1;
            /* Own one-shot file: WRAPPER_LOG is disabled in standalone runs,
             * and "did Trim actually fire?" must be verifiable. */
            f = fopen("log/trim.log", "a");
            if (f) { fprintf(f, "IDXGIDevice3::Trim FIRED (every %d presents)\n", s_every); fclose(f); }
        }
    } else {
        static int s_logged_fail;
        if (!s_logged_fail) {
            FILE *f;
            s_logged_fail = 1;
            f = fopen("log/trim.log", "a");
            if (f) { fprintf(f, "QI(IDXGIDevice3) FAILED - Trim unavailable\n"); fclose(f); }
        }
    }
}

/* ========================================================================
 * Compositing: merge BltFast (2D) and D3D (3D) layers at present
 * ======================================================================== */

void Backend_EnsureCompositingTextures(int width, int height)
{
    HRESULT hr;

    if (g_backend.composite_tex &&
        g_backend.composite_w == width && g_backend.composite_h == height)
        return;

    /* Release old */
    if (g_backend.composite_srv)     { ID3D11ShaderResourceView_Release(g_backend.composite_srv);  g_backend.composite_srv = NULL; }
    if (g_backend.composite_tex)     { ID3D11Texture2D_Release(g_backend.composite_tex);           g_backend.composite_tex = NULL; }
    if (g_backend.composite_staging) { ID3D11Texture2D_Release(g_backend.composite_staging);       g_backend.composite_staging = NULL; }

    /* GPU texture (for rendering as quad) */
    {
        D3D11_TEXTURE2D_DESC td;
        ZeroMemory(&td, sizeof(td));
        td.Width  = (UINT)width;
        td.Height = (UINT)height;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        hr = ID3D11Device_CreateTexture2D(g_backend.device, &td, NULL, &g_backend.composite_tex);
        if (FAILED(hr)) {
            /* Fall back to B8G8R8A8 if B5G6R5 not supported */
            td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            hr = ID3D11Device_CreateTexture2D(g_backend.device, &td, NULL, &g_backend.composite_tex);
            if (FAILED(hr)) {
                WRAPPER_LOG("EnsureCompositingTextures: CreateTexture2D FAILED hr=0x%08lX", hr);
                return;
            }
        }

        hr = ID3D11Device_CreateShaderResourceView(g_backend.device,
            (ID3D11Resource*)g_backend.composite_tex, NULL, &g_backend.composite_srv);
        if (FAILED(hr)) {
            WRAPPER_LOG("EnsureCompositingTextures: CreateSRV FAILED hr=0x%08lX", hr);
            return;
        }

        /* Staging texture for CPU upload */
        td.Usage = D3D11_USAGE_STAGING;
        td.BindFlags = 0;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        hr = ID3D11Device_CreateTexture2D(g_backend.device, &td, NULL, &g_backend.composite_staging);
        if (FAILED(hr)) {
            WRAPPER_LOG("EnsureCompositingTextures: staging FAILED hr=0x%08lX", hr);
            return;
        }
    }

    g_backend.composite_w = width;
    g_backend.composite_h = height;
    WRAPPER_LOG("EnsureCompositingTextures: %dx%d OK", width, height);
}

/**
 * Upload sys_buffer to composite texture with color-key alpha.
 * Black pixels (R5G6B5 == 0x0000) get alpha=0 (transparent).
 * All other pixels get alpha=1 (opaque HUD content).
 */
static void Backend_UploadOverlayWithAlpha(WrapperSurface *s)
{
    DWORD row32 = s->width * 4;
    BYTE *buf32;
    DWORD y;

    if (!s->sys_buffer || s->bpp != 16) return;

    Backend_EnsureCompositingTextures((int)s->width, (int)s->height);
    if (!g_backend.composite_tex) return;

    buf32 = (BYTE*)malloc(row32 * s->height);
    if (!buf32) return;

    for (y = 0; y < s->height; y++) {
        uint16_t *src16 = (uint16_t*)((BYTE*)s->sys_buffer + y * s->pitch);
        uint32_t *dst32 = (uint32_t*)(buf32 + y * row32);
        DWORD x;
        for (x = 0; x < s->width; x++) {
            uint16_t c = src16[x];
            if (c == 0) {
                dst32[x] = 0x00000000;  /* color key → fully transparent */
            } else {
                uint32_t r = ((c >> 11) & 0x1F) * 255 / 31;
                uint32_t g = ((c >> 5)  & 0x3F) * 255 / 63;
                uint32_t b = ((c)       & 0x1F) * 255 / 31;
                dst32[x] = 0xFF000000 | (r << 16) | (g << 8) | b;
            }
        }
    }

    ID3D11DeviceContext_UpdateSubresource(g_backend.context,
        (ID3D11Resource*)g_backend.composite_tex, 0, NULL,
        buf32, row32, 0);
    free(buf32);
}

void Backend_CompositeAndPresent(WrapperSurface *rt_surface, RECT *srcRect, RECT *dstRect)
{
    HRESULT hr;
    static int s_log_count = 0;
    ID3D11DeviceContext *ctx = g_backend.context;

    (void)srcRect; (void)dstRect;
    if (!g_backend.swap_chain || !ctx) return;
    /* [DEVICE-LOST] Once the device is gone, stop feeding it -- issuing more
     * commands on a removed device is what NULL-derefs inside the driver. */
    if (g_backend.device_removed) return;

    Backend_EnforceWindowSize();

    /* Two-layer composite: when 3D rendered this frame, the 3D scene lives
     * on the D3D11 texture (via RTV). If BltFast also wrote HUD overlay to
     * sys_buffer, FlushDirty would destroy the 3D scene by overwriting the
     * GPU texture. Instead, draw them as separate layers:
     *   Layer 1: backbuffer D3D11 texture (3D scene) — opaque
     *   Layer 2: sys_buffer (BltFast HUD overlay) — alpha blended
     *
     * Even if no BltFast wrote this frame (dirty=0), we must NOT FlushDirty
     * because that would overwrite the 3D scene with stale sys_buffer data. */
    if (g_backend.scene_rendered && rt_surface && bt_srv(rt_surface)) {

        /* Upload BltFast overlay to a SECOND composite texture with color-key alpha
         * (only if BltFast wrote this frame; skip if no HUD content) */
        int has_overlay = (rt_surface->dirty && rt_surface->sys_buffer);

        /* Unbind the backbuffer as RT to avoid D3D11 RT/SRV hazard, then
         * copy it to the composite texture for safe reading as SRV */
        ID3D11DeviceContext_OMSetRenderTargets(ctx, 0, NULL, NULL);

        Backend_EnsureCompositingTextures((int)rt_surface->width, (int)rt_surface->height);
        if (g_backend.composite_tex) {
            ID3D11DeviceContext_CopyResource(ctx,
                (ID3D11Resource*)g_backend.composite_tex,
                (ID3D11Resource*)bt_tex(rt_surface));
        }

        /* Bind swap chain as render target */
        ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &g_backend.swap_rtv, NULL);

        /* Layer 1: draw 3D scene (opaque) from the COPY */
        Backend_DrawFullscreenQuad(g_backend.composite_srv ? g_backend.composite_srv : bt_srv(rt_surface));

        /* DrawFullscreenQuad changes VS/PS/InputLayout/Sampler — invalidate
         * all cached indices so next ApplyStateCache actually re-sets them. */
        g_backend.state.current_ps_idx = -1;
        g_backend.state.current_samp_idx = -1;
        g_backend.state.current_blend_idx = -1;
        g_backend.state.current_ds_idx = -1;
        g_backend.state.dirty = 1;

        /* Now upload BltFast overlay to composite texture (reusing it for layer 2) */
        if (has_overlay)
            Backend_UploadOverlayWithAlpha(rt_surface);

        /* Layer 2: draw BltFast overlay (alpha blended) — only if BltFast wrote */
        if (has_overlay && g_backend.composite_srv) {
            ID3D11DeviceContext_VSSetShader(ctx, g_backend.vs_fullscreen, NULL, 0);
            ID3D11DeviceContext_PSSetShader(ctx, g_backend.ps_composite, NULL, 0);
            ID3D11DeviceContext_IASetInputLayout(ctx, NULL);
            ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &g_backend.composite_srv);
            ID3D11DeviceContext_PSSetSamplers(ctx, 0, 1,
                &g_backend.sampler_states[SAMP_POINT_CLAMP]);

            /* Alpha blend: overlay pixels with alpha=1 replace, alpha=0 show through */
            ID3D11DeviceContext_OMSetBlendState(ctx,
                g_backend.blend_states[BLEND_SRCALPHA_INVSRC], NULL, 0xFFFFFFFF);
            ID3D11DeviceContext_OMSetDepthStencilState(ctx,
                g_backend.ds_states[DS_Z_OFF_WRITE_OFF], 0);

            ID3D11DeviceContext_Draw(ctx, 3, 0);

            /* Restore rendering state.
             * Invalidate current_ps_idx so SelectPixelShader actually
             * re-sets the shader (composite changed it to ps_composite). */
            ID3D11DeviceContext_VSSetShader(ctx, g_backend.vs_pretransformed, NULL, 0);
            ID3D11DeviceContext_IASetInputLayout(ctx, g_backend.input_layout);
            g_backend.state.current_ps_idx = -1;
            Backend_SelectPixelShader();
            ID3D11DeviceContext_OMSetDepthStencilState(ctx,
                g_backend.ds_states[g_backend.state.current_ds_idx], 0);
            ID3D11DeviceContext_OMSetBlendState(ctx,
                g_backend.blend_states[g_backend.state.current_blend_idx], NULL, 0xFFFFFFFF);
            ID3D11DeviceContext_PSSetSamplers(ctx, 0, 1,
                &g_backend.sampler_states[g_backend.state.current_samp_idx]);
            {
                ID3D11ShaderResourceView *null_srv = NULL;
                ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &null_srv);
            }
        }

        if (s_log_count < 200) {
            WRAPPER_LOG("CompositeAndPresent[%d]: TWO-LAYER scene=%d dirty=%d overlay=%d srv=%p",
                s_log_count, g_backend.scene_rendered,
                rt_surface ? rt_surface->dirty : -1,
                (rt_surface && rt_surface->dirty && rt_surface->sys_buffer) ? 1 : 0,
                rt_surface ? bt_srv(rt_surface) : NULL);
            s_log_count++;
        }

        rt_surface->dirty = 0;
        /* Do NOT clear scene_rendered here — the game may call Blt to primary
         * multiple times per frame. Clearing here causes the second present to
         * take the SINGLE-LAYER path, which FlushDirty's the sys_buffer and
         * OVERWRITES the 3D scene. scene_rendered is cleared in BeginScene
         * when the next frame starts. */

    } else {
        /* Single-layer path: no 3D this frame, just flush and present.
         * Used for frontend menus (BltFast only, no DrawPrimitive). */
        if (s_log_count < 200) {
            WRAPPER_LOG("CompositeAndPresent[%d]: SINGLE-LAYER scene=%d dirty=%d rt_srv=%p",
                s_log_count, g_backend.scene_rendered,
                rt_surface ? rt_surface->dirty : -1,
                rt_surface ? bt_srv(rt_surface) : NULL);
            s_log_count++;
        }

        if (rt_surface && rt_surface->dirty)
            WrapperSurface_FlushDirty(rt_surface);

        if (rt_surface && bt_srv(rt_surface)) {
            ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &g_backend.swap_rtv, NULL);
            Backend_DrawFullscreenQuad(bt_srv(rt_surface));
        }
        /* Do NOT clear scene_rendered here either — same reason as TWO-LAYER path */
    }

    /* Photo-booth: grab the composited backbuffer before Present (flip model
     * rotates buffer 0 after Present, so capture must happen here). */
    Backend_CaptureIfRequested();

    /* [S01 2026-06-04] sync interval driven by the Display-options VSync toggle
     * (g_backend.vsync; 1=wait-for-vblank, 0=uncapped/tear). Defaults to 1. */
    hr = IDXGISwapChain_Present(g_backend.swap_chain, g_backend.vsync ? 1 : 0, 0);
    if (FAILED(hr)) {
        /* Diagnose + latch device-lost (halts further submission); otherwise
         * log a bounded number of transient present failures. */
        if (!Backend_NoteDeviceRemoved(hr, "CompositeAndPresent/Present") &&
            s_log_count < 10) {
            s_log_count++;
            WRAPPER_LOG("CompositeAndPresent: Present FAILED hr=0x%08lX", hr);
        }
    }

    Backend_MaybeTrim();

    /* Restore game RT (backbuffer, not swap chain) */
    {
        ID3D11RenderTargetView *bb_rtv = bt_rtv(g_backend.backbuffer);
        if (bb_rtv) {
            ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &bb_rtv, g_backend.depth_dsv);
        }
    }
    g_backend.gbuffer_bound = 0;   /* RT juggling above dropped RT1 */
}

/* ========================================================================
 * WrapperClipper - minimal IDirectDrawClipper stub
 * ======================================================================== */

typedef struct WrapperClipperVtbl WrapperClipperVtbl;

struct WrapperClipperVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(WrapperClipper *self, REFIID riid, void **ppv);
    ULONG   (STDMETHODCALLTYPE *AddRef)(WrapperClipper *self);
    ULONG   (STDMETHODCALLTYPE *Release)(WrapperClipper *self);
    HRESULT (STDMETHODCALLTYPE *GetClipList)(WrapperClipper *self, RECT *rect, void *rgndata, DWORD *size);
    HRESULT (STDMETHODCALLTYPE *GetHWnd)(WrapperClipper *self, HWND *hwnd);
    HRESULT (STDMETHODCALLTYPE *Initialize)(WrapperClipper *self, void *ddraw, DWORD flags);
    HRESULT (STDMETHODCALLTYPE *IsClipListChanged)(WrapperClipper *self, BOOL *changed);
    HRESULT (STDMETHODCALLTYPE *SetClipList)(WrapperClipper *self, void *rgndata, DWORD flags);
    HRESULT (STDMETHODCALLTYPE *SetHWnd)(WrapperClipper *self, DWORD flags, HWND hwnd);
};

static HRESULT STDMETHODCALLTYPE Clipper_QueryInterface(WrapperClipper *self, REFIID riid, void **ppv)
{
    (void)riid;
    if (!ppv) return E_POINTER;
    *ppv = self;
    InterlockedIncrement(&self->ref_count);
    return S_OK;
}

static ULONG STDMETHODCALLTYPE Clipper_AddRef(WrapperClipper *self)
{
    return (ULONG)InterlockedIncrement(&self->ref_count);
}

static ULONG STDMETHODCALLTYPE Clipper_Release(WrapperClipper *self)
{
    LONG ref = InterlockedDecrement(&self->ref_count);
    if (ref <= 0) {
        HeapFree(GetProcessHeap(), 0, self);
        return 0;
    }
    return (ULONG)ref;
}

static HRESULT STDMETHODCALLTYPE Clipper_GetClipList(WrapperClipper *self, RECT *rect, void *rgndata, DWORD *size)
{ (void)self; (void)rect; (void)rgndata; (void)size; WRAPPER_STUB("Clipper::GetClipList"); }

static HRESULT STDMETHODCALLTYPE Clipper_GetHWnd(WrapperClipper *self, HWND *hwnd)
{
    if (!hwnd) return E_POINTER;
    *hwnd = self->hwnd;
    return DD_OK;
}

static HRESULT STDMETHODCALLTYPE Clipper_Initialize(WrapperClipper *self, void *ddraw, DWORD flags)
{ (void)self; (void)ddraw; (void)flags; WRAPPER_STUB("Clipper::Initialize"); }

static HRESULT STDMETHODCALLTYPE Clipper_IsClipListChanged(WrapperClipper *self, BOOL *changed)
{
    (void)self;
    if (changed) *changed = FALSE;
    return DD_OK;
}

static HRESULT STDMETHODCALLTYPE Clipper_SetClipList(WrapperClipper *self, void *rgndata, DWORD flags)
{ (void)self; (void)rgndata; (void)flags; WRAPPER_STUB("Clipper::SetClipList"); }

static HRESULT STDMETHODCALLTYPE Clipper_SetHWnd(WrapperClipper *self, DWORD flags, HWND hwnd)
{
    (void)flags;
    self->hwnd = hwnd;
    return DD_OK;
}

static WrapperClipperVtbl s_clipper_vtbl = {
    Clipper_QueryInterface,
    Clipper_AddRef,
    Clipper_Release,
    Clipper_GetClipList,
    Clipper_GetHWnd,
    Clipper_Initialize,
    Clipper_IsClipListChanged,
    Clipper_SetClipList,
    Clipper_SetHWnd,
};

WrapperClipper* WrapperClipper_Create(void)
{
    WrapperClipper *clip = (WrapperClipper*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(WrapperClipper));
    if (!clip) return NULL;
    clip->vtbl = (void*)&s_clipper_vtbl;
    clip->ref_count = 1;
    clip->hwnd = NULL;
    WRAPPER_LOG("WrapperClipper_Create: %p", clip);
    return clip;
}
