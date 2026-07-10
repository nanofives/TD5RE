/**
 * d3d11_backend_pipeline.c - Per-draw state, constant buffer updates, and
 * screen-space lighting/shadow/reflection passes
 *
 * Split out of d3d11_backend.c (2026-07-09, A3 refactor):
 *   - Constant buffer updates (viewport, fog)
 *   - Foliage-AA gate check
 *   - Pixel shader selection (transparency mode)
 *   - Render state cache -> D3D11 state object binding
 *   - Fullscreen quad rendering (present blit, compositing, screen-space passes)
 *   - G-buffer enable/ensure (lighting rework P0)
 *   - Screen-space sun-shadow pass (P2), screen-space reflections pass (P3)
 *   - Deferred dynamic-light pass
 *
 * See also d3d11_backend_device.c (device lifecycle/resource creation) and
 * d3d11_backend_draw.c (compositing + present + WrapperClipper).
 */

#include "wrapper.h"
#include <stdlib.h>
#include <string.h>

/* ========================================================================
 * Constant buffer updates
 * ======================================================================== */

void Backend_UpdateViewportCB(float w, float h)
{
    WrapperRecCtx *rc = g_wrapper_rec;
    ID3D11DeviceContext *ctx = rc ? rc->dc : g_backend.context;
    ID3D11Buffer *cb = rc ? rc->cb_viewport : g_backend.cb_viewport;
    ViewportCB vp;
    vp.viewportWidth  = w;
    vp.viewportHeight = h;
    vp._pad[0] = vp._pad[1] = 0.0f;
    ID3D11DeviceContext_UpdateSubresource(ctx,
        (ID3D11Resource*)cb, 0, NULL, &vp, 0, 0);
}

/* [foliage AA 2026-07-04, reworked 2026-07-04] A draw is "cutout foliage"
 * when the game has the color-key alpha test enabled (st->alpha_test_enable,
 * set only by D3DRS_COLORKEYENABLE) AND the bound texture came from a
 * transparent source (has_alpha: A1R5G5B5, or R5G6B5 with a color key). That
 * combination is exactly trees / fences / signs / chain-link — the opaque
 * sky/road/car-body are plain R5G6B5 (has_alpha=0) drawn with the color key
 * off, so they never match. Matches are rendered via a manual clamped,
 * alpha-weighted texel reconstruction (SampleFoliageAA in ps_common.hlsli,
 * selected per-draw by the foliageAA flag in FogCB) instead of the bound
 * sampler's hardware bilinear, so the 1-bit edge/dither alpha interpolates
 * into a smooth anti-aliased silhouette without leaking incidental
 * "transparent" texel color or wrap-seam artifacts at the sprite border (see
 * the FogCB.foliageAA comment in wrapper.h for why hardware bilinear alone
 * got both of those wrong). The tight scope avoids the old global
 * luminance-alpha regression (sky/road/cars turned translucent).
 *
 * [2026-07-06] ...AND the game's own blend must be DISABLED. Cutout foliage
 * is submitted with blending off (the color key alone shapes it); sprites
 * the game blends itself — Moscow's streetlamp glow heads (additive-style
 * blends), taillight glows — also carry a color key + alpha texture, and
 * the foliage-AA blend override (BLEND_SRCALPHA_INVSRC in
 * Backend_ApplyStateCache) replaced their additive blend, rendering glow
 * textures as solid dark balls. Blended draws keep the game's sampler,
 * shader path and blend exactly as requested. */
static int Backend_IsFoliageAA(const RenderStateCache *st, int has_alpha)
{
    return g_backend.foliage_aa_enabled && st->alpha_test_enable && has_alpha
        && !st->blend_enable;
}

void Backend_UpdateFogCB(void)
{
    WrapperRecCtx *rc = g_wrapper_rec;
    ID3D11DeviceContext *ctx = rc ? rc->dc : g_backend.context;
    ID3D11Buffer *cb = rc ? rc->cb_fog : g_backend.cb_fog;
    RenderStateCache *st = rc ? &rc->state : &g_backend.state;
    FogCB fog;
    DWORD fc = st->fog_color;
    int foliage_aa = Backend_IsFoliageAA(st,
        rc ? rc->current_tex_has_alpha : g_backend.current_tex_has_alpha);
    fog.fogColor[0] = ((fc >> 16) & 0xFF) / 255.0f;  /* R */
    fog.fogColor[1] = ((fc >>  8) & 0xFF) / 255.0f;  /* G */
    fog.fogColor[2] = ((fc >>  0) & 0xFF) / 255.0f;  /* B */
    fog.fogColor[3] = 1.0f;
    fog.fogStart    = st->fog_start;
    fog.fogEnd      = st->fog_end;
    fog.fogDensity  = st->fog_density;
    fog.fogEnabled  = st->fog_enable;
    fog.alphaTestEnabled = st->alpha_test_enable;
    fog.alphaRef    = st->alpha_ref / 255.0f;
    /* [foliage-AA depth halo fix 2026-07-07] Foliage-AA draws (type-1 color-key
     * cutouts, blend off, z_write ON) reconstruct alpha with a bilinear kernel
     * (SampleFoliageAA) that smears non-zero alpha OUT of opaque texels into the
     * neighbouring transparent holes/gaps. At the preset's near-zero alpha_ref
     * (1/255) those smeared fringe/hole texels survive the alpha test, so they
     * WRITE DEPTH while contributing ~no colour under SRCALPHA blend — occluding
     * the world drawn afterwards and revealing the pre-painted skybox through a
     * cutout's transparent parts (Newcastle guardrails "see the skybox behind").
     * Raise the effective alpha test to 0.5 for foliage-AA draws (matching the
     * type-2 TRANSLUCENT_ANISO preset): the sub-50%-coverage reconstruction is
     * discarded so it never stamps depth into holes, while opaque cores (alpha
     * ~1 — e.g. cutout building WALLS) still pass and keep occluding, so this
     * does NOT re-open the 51dd13b2 "see-through geometry" regression that the
     * global z-write-off hunk caused. */
    if (foliage_aa && fog.alphaRef < 0.5f)
        fog.alphaRef = 0.5f;
    fog._pad1       = 0.0f;
    fog.foliageAA   = foliage_aa ? 1.0f : 0.0f;
    ID3D11DeviceContext_UpdateSubresource(ctx,
        (ID3D11Resource*)cb, 0, NULL, &fog, 0, 0);
}

/* ========================================================================
 * State cache → D3D11 binding
 * ======================================================================== */

static const char *s_ps_names[] = { "MODULATE", "MODULATE_ALPHA", "DECAL", "LUMINANCE_ALPHA",
                                    "MODULATE_G", "MODULATE_ALPHA_G" };

void Backend_SelectPixelShader(void)
{
    WrapperRecCtx *rc = g_wrapper_rec;
    RenderStateCache *st = rc ? &rc->state : &g_backend.state;
    ID3D11DeviceContext *ctx = rc ? rc->dc : g_backend.context;
    int idx;

    /* R5G6B5 textures are converted to B8G8R8A8 with alpha=0xFF (fully opaque).
     * A1R5G5B5 textures get correct alpha from bit15. Both formats have valid
     * alpha in the D3D11 texture, so we always use the standard shader path.
     *
     * LUMINANCE_ALPHA was previously auto-selected for blended R5G6B5 textures,
     * but this is WRONG: it replaces alpha with luminance, making dark pixels
     * transparent. The sky, road, and other opaque geometry rendered semi-transparent
     * over the cleared black background, causing missing track elements and
     * wrong car textures. */
    switch (st->texblend_mode) {
    case D3DTBLEND_DECAL:         idx = PS_DECAL; break;
    case D3DTBLEND_MODULATEALPHA: idx = PS_MODULATE_ALPHA; break;
    default:                      idx = PS_MODULATE; break;
    }

    /* [lighting rework P0] Promote to the MRT G-buffer variant for z-writing,
     * non-blended draws while the G-buffer is active. Immediate path only —
     * the deferred-context record path keeps the single-RT shaders. The RT1
     * binding follows in Backend_ApplyStateCache with the same predicate. */
    if (!rc && g_backend.gbuffer_enabled && g_backend.gbuffer_rtv &&
        st->z_write && !st->blend_enable &&
        !Backend_IsFoliageAA(st, g_backend.current_tex_has_alpha)) {
        if (idx == PS_MODULATE && g_backend.ps_shaders[PS_MODULATE_G])
            idx = PS_MODULATE_G;
        else if (idx == PS_MODULATE_ALPHA && g_backend.ps_shaders[PS_MODULATE_ALPHA_G])
            idx = PS_MODULATE_ALPHA_G;
    }

    if (idx != st->current_ps_idx) {
        static int s_ps_log = 0;
        if (!rc && s_ps_log < 30) {
            WRAPPER_LOG("SelectPS: %s (blend=%d tex_alpha=%d texblend=%d src=%u dst=%u)",
                s_ps_names[idx], g_backend.alpha_blend_enabled,
                g_backend.current_tex_has_alpha, st->texblend_mode,
                st->src_blend, st->dest_blend);
            s_ps_log++;
        }
        st->current_ps_idx = idx;
        ID3D11DeviceContext_PSSetShader(ctx,
            g_backend.ps_shaders[idx], NULL, 0);
    }
}

void Backend_ApplyStateCache(void)
{
    WrapperRecCtx *rc = g_wrapper_rec;
    RenderStateCache *s = rc ? &rc->state : &g_backend.state;
    ID3D11DeviceContext *ctx = rc ? rc->dc : g_backend.context;

    if (!s->dirty) return;

    /* [foliage AA] Cutout foliage (color-key alpha test on a transparent-source
     * texture) is rendered via the shader's manual reconstruction (FogCB's
     * foliageAA flag, set in Backend_UpdateFogCB below) + standard alpha
     * blend, so its 1-bit edge/dither alpha anti-aliases. The bound sampler
     * is NOT involved for these draws (see ps_common.hlsli SampleFoliageAA).
     * Computed once here, reused for the blend and G-buffer-binding
     * decisions. */
    int foliage_aa = Backend_IsFoliageAA(s,
        rc ? rc->current_tex_has_alpha : g_backend.current_tex_has_alpha);

    /* Blend state */
    {
        int bi;
        if (foliage_aa) {
            bi = BLEND_SRCALPHA_INVSRC;
        } else if (!s->blend_enable) {
            bi = BLEND_OPAQUE;
        } else if (s->src_blend == D3D6BLEND_SRCALPHA && s->dest_blend == D3D6BLEND_INVSRCALPHA) {
            bi = BLEND_SRCALPHA_INVSRC;
        } else if (s->src_blend == D3D6BLEND_SRCALPHA && s->dest_blend == D3D6BLEND_ONE) {
            bi = BLEND_SRCALPHA_ONE;
        } else if (s->src_blend == D3D6BLEND_ONE && s->dest_blend == D3D6BLEND_ONE) {
            bi = BLEND_ONE_ONE;
        } else if (s->src_blend == D3D6BLEND_SRCALPHA && s->dest_blend == D3D6BLEND_SRCALPHA) {
            bi = BLEND_SRCALPHA_SRCALPHA;
        } else if (s->src_blend == D3D6BLEND_INVSRCALPHA && s->dest_blend == D3D6BLEND_INVSRCALPHA) {
            bi = BLEND_INVSRC_INVSRC;
        } else {
            /* Default to standard alpha blend for unhandled combos */
            bi = BLEND_SRCALPHA_INVSRC;
            WRAPPER_LOG("ApplyStateCache: unhandled blend combo src=%u dst=%u, using SRCALPHA_INVSRC",
                s->src_blend, s->dest_blend);
        }
        if (bi != s->current_blend_idx) {
            s->current_blend_idx = bi;
            ID3D11DeviceContext_OMSetBlendState(ctx, g_backend.blend_states[bi], NULL, 0xFFFFFFFF);
        }
    }

    /* Depth-stencil state */
    {
        int di;
        if (!s->z_enable && !s->z_write)      di = DS_Z_OFF_WRITE_OFF;
        else if (!s->z_enable && s->z_write)   di = DS_Z_OFF_WRITE_ON;
        else if (s->z_func == 1) {
            di = s->z_write ? DS_Z_ON_WRITE_ON_ALWAYS : DS_Z_ON_WRITE_OFF_ALWAYS;
        }
        else if (s->z_enable && s->z_write)    di = DS_Z_ON_WRITE_ON;
        else                                   di = DS_Z_ON_WRITE_OFF;

        if (di != s->current_ds_idx) {
            s->current_ds_idx = di;
            ID3D11DeviceContext_OMSetDepthStencilState(ctx, g_backend.ds_states[di], 0);
        }
    }

    /* Rasterizer state — default vs shadow-decal (polygon offset for shadows). */
    {
        int ri = s->polygon_offset ? 1 : 0;
        if (ri != s->current_rs_idx) {
            s->current_rs_idx = ri;
            ID3D11RasterizerState *target = ri ?
                g_backend.rs_state_shadow_decal :
                g_backend.rs_state;
            ID3D11DeviceContext_RSSetState(ctx, target);
        }
    }

    /* Sampler state */
    {
        int si;
        int linear = (s->mag_filter >= 2 || s->min_filter >= 2);
        int clamp  = (s->tex_address_u == 3 || s->tex_address_v == 3);
        if (linear && clamp)       si = SAMP_LINEAR_CLAMP;
        else if (linear && !clamp) si = SAMP_LINEAR_WRAP;
        else if (!linear && clamp) si = SAMP_POINT_CLAMP;
        else                       si = SAMP_POINT_WRAP;

        /* [foliage AA] No override here anymore: foliage_aa draws reconstruct
         * their own edge in the pixel shader via Load() (SampleFoliageAA),
         * which never touches the bound sampler, so forcing a filter/address
         * mode on it would be a no-op. Previously this forced bilinear while
         * keeping the game's WRAP addressing, which bled the texture's
         * opposite edge into the sprite border — that's what moved the fix
         * into the shader instead of the fixed-function sampler. foliage_aa
         * itself is still used below for the G-buffer-binding decision. */

        if (si != s->current_samp_idx) {
            s->current_samp_idx = si;
            ID3D11DeviceContext_PSSetSamplers(ctx, 0, 1, &g_backend.sampler_states[si]);
        }
    }

    /* Pixel shader */
    Backend_SelectPixelShader();

    /* [lighting rework P0] Bind/unbind the G-buffer as RT1 to match the shader
     * promotion in Backend_SelectPixelShader (same predicate). Immediate
     * context only; RT0 stays the game's 3D target (backbuffer surface). */
    if (!rc) {
        int want = (g_backend.gbuffer_enabled && g_backend.gbuffer_rtv &&
                    s->z_write && !s->blend_enable && !foliage_aa &&
                    g_backend.backbuffer && g_backend.backbuffer->d3d11_rtv) ? 1 : 0;
        if (want != g_backend.gbuffer_bound) {
            ID3D11RenderTargetView *rtvs[2];
            rtvs[0] = g_backend.backbuffer->d3d11_rtv;
            rtvs[1] = want ? g_backend.gbuffer_rtv : NULL;
            ID3D11DeviceContext_OMSetRenderTargets(ctx, want ? 2 : 1, rtvs,
                                                   g_backend.depth_dsv);
            g_backend.gbuffer_bound = want;
        }
    }

    /* Update fog+alpha CB (contains alpha test state which changes per-draw) */
    Backend_UpdateFogCB();

    s->dirty = 0;
}

/* ========================================================================
 * Fullscreen quad rendering
 * ======================================================================== */

void Backend_DrawFullscreenQuad(ID3D11ShaderResourceView *srv)
{
    ID3D11DeviceContext *ctx = g_backend.context;
    D3D11_VIEWPORT vp;
    FLOAT vp_w = (FLOAT)(g_backend.target_width  > 0 ? g_backend.target_width  : g_backend.width);
    FLOAT vp_h = (FLOAT)(g_backend.target_height > 0 ? g_backend.target_height : g_backend.height);

    vp.TopLeftX = 0.0f;
    vp.TopLeftY = 0.0f;
    vp.Width    = vp_w;
    vp.Height   = vp_h;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);

    /* Switch to fullscreen VS + composite PS */
    ID3D11DeviceContext_VSSetShader(ctx, g_backend.vs_fullscreen, NULL, 0);
    ID3D11DeviceContext_PSSetShader(ctx, g_backend.ps_composite, NULL, 0);
    ID3D11DeviceContext_IASetInputLayout(ctx, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    /* Bind source texture */
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &srv);

    /* Use point (nearest-neighbor) sampler for crisp pixel-perfect present */
    ID3D11DeviceContext_PSSetSamplers(ctx, 0, 1, &g_backend.sampler_states[SAMP_POINT_CLAMP]);

    /* Force opaque blit — the backbuffer is the final composed frame.
     * All transparency was resolved during rendering (alpha blend in
     * DrawPrimitive) and BltFast (color key skip). The present blit
     * should copy the result as-is to the swap chain. */
    ID3D11DeviceContext_OMSetBlendState(ctx, g_backend.blend_states[BLEND_OPAQUE], NULL, 0xFFFFFFFF);
    ID3D11DeviceContext_OMSetDepthStencilState(ctx, g_backend.ds_states[DS_Z_OFF_WRITE_OFF], 0);

    /* Draw 3 vertices (fullscreen triangle from SV_VertexID) */
    ID3D11DeviceContext_Draw(ctx, 3, 0);

    /* Restore game rendering state */
    ID3D11DeviceContext_VSSetShader(ctx, g_backend.vs_pretransformed, NULL, 0);
    ID3D11DeviceContext_IASetInputLayout(ctx, g_backend.input_layout);
    Backend_SelectPixelShader();

    /* Restore depth state from cache */
    ID3D11DeviceContext_OMSetDepthStencilState(ctx,
        g_backend.ds_states[g_backend.state.current_ds_idx], 0);

    /* Restore blend state from cache */
    ID3D11DeviceContext_OMSetBlendState(ctx,
        g_backend.blend_states[g_backend.state.current_blend_idx], NULL, 0xFFFFFFFF);

    /* Restore sampler from cache */
    ID3D11DeviceContext_PSSetSamplers(ctx, 0, 1,
        &g_backend.sampler_states[g_backend.state.current_samp_idx]);

    /* Unbind SRV to avoid D3D11 warning (RT and SRV conflict) */
    {
        ID3D11ShaderResourceView *null_srv = NULL;
        ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &null_srv);
    }
}

/* [lighting rework P0] Lazily (re)create the G-buffer at the current
 * render-target size. Must match the backbuffer/depth dimensions or the MRT
 * bind is invalid. Returns 1 when usable. */
static int Backend_GBufferEnsure(void)
{
    HRESULT hr;
    int w = g_backend.target_width  > 0 ? g_backend.target_width  : g_backend.width;
    int h = g_backend.target_height > 0 ? g_backend.target_height : g_backend.height;

    if (w <= 0 || h <= 0 || !g_backend.device) return 0;
    if (g_backend.gbuffer_rtv && g_backend.gbuffer_w == w && g_backend.gbuffer_h == h)
        return 1;

    if (g_backend.gbuffer_srv) { ID3D11ShaderResourceView_Release(g_backend.gbuffer_srv); g_backend.gbuffer_srv = NULL; }
    if (g_backend.gbuffer_rtv) { ID3D11RenderTargetView_Release(g_backend.gbuffer_rtv);   g_backend.gbuffer_rtv = NULL; }
    if (g_backend.gbuffer_tex) { ID3D11Texture2D_Release(g_backend.gbuffer_tex);          g_backend.gbuffer_tex = NULL; }
    g_backend.gbuffer_w = g_backend.gbuffer_h = 0;
    g_backend.gbuffer_bound = 0;

    {
        D3D11_TEXTURE2D_DESC td;
        ZeroMemory(&td, sizeof(td));
        td.Width  = (UINT)w;
        td.Height = (UINT)h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

        hr = ID3D11Device_CreateTexture2D(g_backend.device, &td, NULL, &g_backend.gbuffer_tex);
        if (FAILED(hr)) { WRAPPER_LOG("GBufferEnsure: CreateTexture2D FAILED hr=0x%08lX", hr); return 0; }
    }
    hr = ID3D11Device_CreateRenderTargetView(g_backend.device,
        (ID3D11Resource*)g_backend.gbuffer_tex, NULL, &g_backend.gbuffer_rtv);
    if (FAILED(hr)) {
        WRAPPER_LOG("GBufferEnsure: CreateRTV FAILED hr=0x%08lX", hr);
        ID3D11Texture2D_Release(g_backend.gbuffer_tex); g_backend.gbuffer_tex = NULL;
        return 0;
    }
    hr = ID3D11Device_CreateShaderResourceView(g_backend.device,
        (ID3D11Resource*)g_backend.gbuffer_tex, NULL, &g_backend.gbuffer_srv);
    if (FAILED(hr)) {
        WRAPPER_LOG("GBufferEnsure: CreateSRV FAILED hr=0x%08lX", hr);
        ID3D11RenderTargetView_Release(g_backend.gbuffer_rtv); g_backend.gbuffer_rtv = NULL;
        ID3D11Texture2D_Release(g_backend.gbuffer_tex);        g_backend.gbuffer_tex = NULL;
        return 0;
    }
    g_backend.gbuffer_w = w;
    g_backend.gbuffer_h = h;
    WRAPPER_LOG("GBufferEnsure: created %dx%d R8G8B8A8 (normal+matid)", w, h);
    return 1;
}

void Backend_SetGBufferEnabled(int on)
{
    on = on ? 1 : 0;
    if (on && (!g_backend.device || !g_backend.context)) on = 0;
    if (on && !Backend_GBufferEnsure()) on = 0;

    if (g_backend.gbuffer_enabled != on) {
        g_backend.gbuffer_enabled = on;
        WRAPPER_LOG("SetGBufferEnabled: %d", on);
    }
    /* Force the next draw to re-evaluate the PS promotion + RT1 binding. */
    g_backend.state.current_ps_idx = -1;
    g_backend.state.dirty = 1;

    /* Per-frame clear: matid 0 everywhere = "no G-buffer data" so pixels never
     * touched by a z-writing world draw keep the legacy lighting fallback. */
    if (on) {
        const float clear[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        ID3D11DeviceContext_ClearRenderTargetView(g_backend.context,
                                                  g_backend.gbuffer_rtv, clear);
    }
}

/* [P2] Screen-space ray-marched sun-shadow pass — see wrapper.h. Same RT/SRV
 * juggling discipline as Backend_ApplyLightPass below, but the fullscreen draw
 * uses the MULTIPLICATIVE blend so shadowed pixels darken the scene colour. */
void Backend_ApplyShadowPass(const ShadowCB *cb)
{
    ID3D11DeviceContext *ctx = g_backend.context;
    ID3D11RenderTargetView *rtv;
    ID3D11ShaderResourceView *null_srv = NULL;

    if (!ctx || !cb) return;
    if (!g_backend.ps_shadow || !g_backend.cb_shadow || !g_backend.depth_srv) return;
    if (!g_backend.backbuffer || !g_backend.backbuffer->d3d11_rtv) return;
    rtv = g_backend.backbuffer->d3d11_rtv;

    ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource*)g_backend.cb_shadow,
                                          0, NULL, cb, 0, 0);

    /* Colour-only RT: depth free for SRV sampling; drops G-buffer RT1 too. */
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, NULL);
    g_backend.gbuffer_bound = 0;

    ID3D11DeviceContext_VSSetShader(ctx, g_backend.vs_fullscreen, NULL, 0);
    ID3D11DeviceContext_PSSetShader(ctx, g_backend.ps_shadow, NULL, 0);
    ID3D11DeviceContext_IASetInputLayout(ctx, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &g_backend.depth_srv);
    ID3D11DeviceContext_PSSetShaderResources(ctx, 1, 1, &g_backend.gbuffer_srv);
    ID3D11DeviceContext_PSSetSamplers(ctx, 0, 1, &g_backend.sampler_states[SAMP_POINT_CLAMP]);
    ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 1, &g_backend.cb_shadow);

    /* Multiplicative darkening, depth test OFF. */
    ID3D11DeviceContext_OMSetBlendState(ctx, g_backend.blend_states[BLEND_MULT], NULL, 0xFFFFFFFF);
    ID3D11DeviceContext_OMSetDepthStencilState(ctx, g_backend.ds_states[DS_Z_OFF_WRITE_OFF], 0);

    ID3D11DeviceContext_Draw(ctx, 3, 0);

    /* Unbind SRVs, restore RT + game state (see ApplyLightPass rationale). */
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &null_srv);
    ID3D11DeviceContext_PSSetShaderResources(ctx, 1, 1, &null_srv);
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, g_backend.depth_dsv);
    ID3D11DeviceContext_VSSetShader(ctx, g_backend.vs_pretransformed, NULL, 0);
    ID3D11DeviceContext_IASetInputLayout(ctx, g_backend.input_layout);
    g_backend.state.current_ps_idx = -1;
    g_backend.state.dirty = 1;
    Backend_SelectPixelShader();
    ID3D11DeviceContext_OMSetDepthStencilState(ctx,
        g_backend.ds_states[g_backend.state.current_ds_idx], 0);
    ID3D11DeviceContext_OMSetBlendState(ctx,
        g_backend.blend_states[g_backend.state.current_blend_idx], NULL, 0xFFFFFFFF);
    ID3D11DeviceContext_PSSetSamplers(ctx, 0, 1,
        &g_backend.sampler_states[g_backend.state.current_samp_idx]);
    ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 1, &g_backend.cb_fog);
}

/* [P3] (Re)create the scene-colour copy to match the backbuffer surface
 * texture (CopyResource requires identical size/format). */
static int Backend_SceneCopyEnsure(void)
{
    HRESULT hr;
    D3D11_TEXTURE2D_DESC td;

    if (!g_backend.backbuffer || !g_backend.backbuffer->d3d11_texture) return 0;
    ID3D11Texture2D_GetDesc(g_backend.backbuffer->d3d11_texture, &td);
    if (g_backend.scene_copy_tex &&
        g_backend.scene_copy_w == (int)td.Width &&
        g_backend.scene_copy_h == (int)td.Height)
        return 1;

    if (g_backend.scene_copy_srv) { ID3D11ShaderResourceView_Release(g_backend.scene_copy_srv); g_backend.scene_copy_srv = NULL; }
    if (g_backend.scene_copy_tex) { ID3D11Texture2D_Release(g_backend.scene_copy_tex);          g_backend.scene_copy_tex = NULL; }
    g_backend.scene_copy_w = g_backend.scene_copy_h = 0;

    td.Usage          = D3D11_USAGE_DEFAULT;
    td.BindFlags      = D3D11_BIND_SHADER_RESOURCE;
    td.CPUAccessFlags = 0;
    td.MiscFlags      = 0;
    td.MipLevels      = 1;
    hr = ID3D11Device_CreateTexture2D(g_backend.device, &td, NULL, &g_backend.scene_copy_tex);
    if (FAILED(hr)) { WRAPPER_LOG("SceneCopyEnsure: CreateTexture2D FAILED hr=0x%08lX", hr); return 0; }
    hr = ID3D11Device_CreateShaderResourceView(g_backend.device,
        (ID3D11Resource*)g_backend.scene_copy_tex, NULL, &g_backend.scene_copy_srv);
    if (FAILED(hr)) {
        WRAPPER_LOG("SceneCopyEnsure: CreateSRV FAILED hr=0x%08lX", hr);
        ID3D11Texture2D_Release(g_backend.scene_copy_tex); g_backend.scene_copy_tex = NULL;
        return 0;
    }
    g_backend.scene_copy_w = (int)td.Width;
    g_backend.scene_copy_h = (int)td.Height;
    WRAPPER_LOG("SceneCopyEnsure: created %ux%u scene-colour copy", td.Width, td.Height);
    return 1;
}

/* [P3] Screen-space reflections — see wrapper.h. Copies the backbuffer to
 * scene_copy_tex, then draws a fullscreen ALPHA-BLENDED pass that reflects
 * the copied scene on reflective materials. Same RT/SRV/restore discipline
 * as the shadow + light passes. */
void Backend_ApplySSRPass(const SSRCB *cb)
{
    ID3D11DeviceContext *ctx = g_backend.context;
    ID3D11RenderTargetView *rtv;
    ID3D11ShaderResourceView *null_srv = NULL;

    if (!ctx || !cb) return;
    if (!g_backend.ps_ssr || !g_backend.cb_ssr || !g_backend.depth_srv) return;
    if (!g_backend.gbuffer_srv) return;   /* SSR needs per-pixel normals */
    if (!g_backend.backbuffer || !g_backend.backbuffer->d3d11_rtv) return;
    if (!Backend_SceneCopyEnsure()) return;
    rtv = g_backend.backbuffer->d3d11_rtv;

    ID3D11DeviceContext_CopyResource(ctx,
        (ID3D11Resource*)g_backend.scene_copy_tex,
        (ID3D11Resource*)g_backend.backbuffer->d3d11_texture);

    ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource*)g_backend.cb_ssr,
                                          0, NULL, cb, 0, 0);

    /* Colour-only RT: depth free for SRV sampling; drops G-buffer RT1 too. */
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, NULL);
    g_backend.gbuffer_bound = 0;

    ID3D11DeviceContext_VSSetShader(ctx, g_backend.vs_fullscreen, NULL, 0);
    ID3D11DeviceContext_PSSetShader(ctx, g_backend.ps_ssr, NULL, 0);
    ID3D11DeviceContext_IASetInputLayout(ctx, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &g_backend.depth_srv);
    ID3D11DeviceContext_PSSetShaderResources(ctx, 1, 1, &g_backend.gbuffer_srv);
    ID3D11DeviceContext_PSSetShaderResources(ctx, 2, 1, &g_backend.scene_copy_srv);
    ID3D11DeviceContext_PSSetSamplers(ctx, 0, 1, &g_backend.sampler_states[SAMP_POINT_CLAMP]);
    ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 1, &g_backend.cb_ssr);

    /* Standard alpha blend: out = refl*w + scene*(1-w); depth test OFF. */
    ID3D11DeviceContext_OMSetBlendState(ctx, g_backend.blend_states[BLEND_SRCALPHA_INVSRC], NULL, 0xFFFFFFFF);
    ID3D11DeviceContext_OMSetDepthStencilState(ctx, g_backend.ds_states[DS_Z_OFF_WRITE_OFF], 0);

    ID3D11DeviceContext_Draw(ctx, 3, 0);

    /* Unbind SRVs, restore RT + game state (see ApplyLightPass rationale). */
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &null_srv);
    ID3D11DeviceContext_PSSetShaderResources(ctx, 1, 1, &null_srv);
    ID3D11DeviceContext_PSSetShaderResources(ctx, 2, 1, &null_srv);
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, g_backend.depth_dsv);
    ID3D11DeviceContext_VSSetShader(ctx, g_backend.vs_pretransformed, NULL, 0);
    ID3D11DeviceContext_IASetInputLayout(ctx, g_backend.input_layout);
    g_backend.state.current_ps_idx = -1;
    g_backend.state.dirty = 1;
    Backend_SelectPixelShader();
    ID3D11DeviceContext_OMSetDepthStencilState(ctx,
        g_backend.ds_states[g_backend.state.current_ds_idx], 0);
    ID3D11DeviceContext_OMSetBlendState(ctx,
        g_backend.blend_states[g_backend.state.current_blend_idx], NULL, 0xFFFFFFFF);
    ID3D11DeviceContext_PSSetSamplers(ctx, 0, 1,
        &g_backend.sampler_states[g_backend.state.current_samp_idx]);
    ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 1, &g_backend.cb_fog);
}

/* Deferred dynamic-light pass — see Backend_ApplyLightPass in wrapper.h.
 * Samples scene depth (depth_srv) to reconstruct world position per pixel and
 * accumulates the dynamic lights additively onto the scene colour target. Runs
 * over the CURRENT D3D viewport + scissor (so it respects the active split-screen
 * pane). Depth must be UNBOUND as a DSV while sampled as an SRV, so we swap the
 * render target to colour-only for the draw, then restore colour+depth. */
void Backend_ApplyLightPass(const LightCB *cb)
{
    ID3D11DeviceContext *ctx = g_backend.context;
    ID3D11RenderTargetView *rtv;
    ID3D11ShaderResourceView *null_srv = NULL;

    if (!ctx || !cb) return;
    if (!g_backend.ps_light || !g_backend.cb_light || !g_backend.depth_srv) return;
    if (!g_backend.backbuffer || !g_backend.backbuffer->d3d11_rtv) return;
    rtv = g_backend.backbuffer->d3d11_rtv;

    /* Upload camera + light array. */
    ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource*)g_backend.cb_light,
                                          0, NULL, cb, 0, 0);

    /* Colour-only RT so depth_tex is free to be sampled as an SRV. This also
     * unbinds the G-buffer RT1 (whole-RT-set replace) so it can be sampled. */
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, NULL);
    g_backend.gbuffer_bound = 0;

    ID3D11DeviceContext_VSSetShader(ctx, g_backend.vs_fullscreen, NULL, 0);
    ID3D11DeviceContext_PSSetShader(ctx, g_backend.ps_light, NULL, 0);
    ID3D11DeviceContext_IASetInputLayout(ctx, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &g_backend.depth_srv);
    /* [lighting rework P0] G-buffer normals at t1 (NULL when inactive — the
     * shader's Load then returns 0 = "no data" and keeps legacy behavior). */
    ID3D11DeviceContext_PSSetShaderResources(ctx, 1, 1, &g_backend.gbuffer_srv);
    ID3D11DeviceContext_PSSetSamplers(ctx, 0, 1, &g_backend.sampler_states[SAMP_POINT_CLAMP]);
    ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 1, &g_backend.cb_light);

    /* Additive ONE/ONE, depth test OFF (we light the reconstructed surface). */
    ID3D11DeviceContext_OMSetBlendState(ctx, g_backend.blend_states[BLEND_ONE_ONE], NULL, 0xFFFFFFFF);
    ID3D11DeviceContext_OMSetDepthStencilState(ctx, g_backend.ds_states[DS_Z_OFF_WRITE_OFF], 0);

    /* Fullscreen triangle, clipped to the current viewport/scissor (the pane). */
    ID3D11DeviceContext_Draw(ctx, 3, 0);

    /* Unbind the depth SRV BEFORE re-binding it as a DSV (and the G-buffer SRV
     * before it can be re-bound as RT1 by the next opaque draw). */
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &null_srv);
    ID3D11DeviceContext_PSSetShaderResources(ctx, 1, 1, &null_srv);

    /* Restore colour + depth RT and the game's shader/state so the following
     * translucent/HUD draws continue normally. */
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, g_backend.depth_dsv);
    ID3D11DeviceContext_VSSetShader(ctx, g_backend.vs_pretransformed, NULL, 0);
    ID3D11DeviceContext_IASetInputLayout(ctx, g_backend.input_layout);
    /* [lighting rework P0] The PS binding was replaced by ps_light above but
     * the cache index still names the game shader — invalidate so
     * Backend_SelectPixelShader actually re-binds instead of early-outing. */
    g_backend.state.current_ps_idx = -1;
    g_backend.state.dirty = 1;
    Backend_SelectPixelShader();
    ID3D11DeviceContext_OMSetDepthStencilState(ctx,
        g_backend.ds_states[g_backend.state.current_ds_idx], 0);
    ID3D11DeviceContext_OMSetBlendState(ctx,
        g_backend.blend_states[g_backend.state.current_blend_idx], NULL, 0xFFFFFFFF);
    ID3D11DeviceContext_PSSetSamplers(ctx, 0, 1,
        &g_backend.sampler_states[g_backend.state.current_samp_idx]);
    /* Re-bind the game's fog cbuffer to PS b0 (we overwrote it with cb_light). */
    ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 1, &g_backend.cb_fog);
}

