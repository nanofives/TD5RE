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
#include <stdlib.h>
#include <string.h>

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
    if (g_backend.scene_rendered && rt_surface && rt_surface->d3d11_srv) {

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
                (ID3D11Resource*)rt_surface->d3d11_texture);
        }

        /* Bind swap chain as render target */
        ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &g_backend.swap_rtv, NULL);

        /* Layer 1: draw 3D scene (opaque) from the COPY */
        Backend_DrawFullscreenQuad(g_backend.composite_srv ? g_backend.composite_srv : rt_surface->d3d11_srv);

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
                rt_surface ? rt_surface->d3d11_srv : NULL);
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
                rt_surface ? rt_surface->d3d11_srv : NULL);
            s_log_count++;
        }

        if (rt_surface && rt_surface->dirty)
            WrapperSurface_FlushDirty(rt_surface);

        if (rt_surface && rt_surface->d3d11_srv) {
            ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &g_backend.swap_rtv, NULL);
            Backend_DrawFullscreenQuad(rt_surface->d3d11_srv);
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

    /* Restore game RT (backbuffer, not swap chain) */
    if (g_backend.backbuffer && g_backend.backbuffer->d3d11_rtv) {
        ID3D11DeviceContext_OMSetRenderTargets(ctx, 1,
            &g_backend.backbuffer->d3d11_rtv, g_backend.depth_dsv);
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
