/**
 * d3d12_backend.c -- D3D12 backend engine (Phase 1 skeleton of the D3D12 port).
 *
 * Provides the Backend_* API (see wrapper.h) on top of Direct3D 12. This file is
 * compiled ONLY into libddraw_wrapper_d3d12.a (build.bat d3d12); the d3d11 build
 * uses d3d11_backend_*.c instead. Shared COM files + the game call the same
 * Backend_* surface and are backend-agnostic.
 *
 * PHASE 1 SCOPE: real device + direct queue + flip-discard swapchain + fence +
 * RTV heap + per-frame command list, plus clear/present so the window shows a
 * live cleared frame. Everything else (geometry, textures, state, passes) is
 * stubbed here so the library links; those fill in over Phases 2-4.
 *
 * The D3D12 objects live in a PRIVATE g_d3d12 struct; the shared public g_backend
 * (still a D3D11Backend layout, its ID3D11 fields unused here) carries the
 * backend-agnostic fields (width/height/hwnd/vsync/device_generation/...) and
 * g_backend.swap_chain (an IDXGISwapChain* base pointer, DXGI being shared).
 */

#define COBJMACROS
/* mingw-w64: make the struct-returning D3D12 methods (e.g.
 * GetCPUDescriptorHandleForHeapStart) take an explicit out-param in C, avoiding
 * the aggregate-return ABI mismatch. Must precede <d3d12.h>. */
#define WIDL_EXPLICIT_AGGREGATE_RETURNS
#include "wrapper.h"          /* g_backend, Backend typedefs, Win32 */
#include <d3d12.h>
#include <dxgi1_6.h>

#define D3D12_FRAME_COUNT 2

typedef struct {
    ID3D12Device              *device;
    ID3D12CommandQueue        *queue;
    IDXGISwapChain3           *swapchain;
    ID3D12CommandAllocator    *allocators[D3D12_FRAME_COUNT];
    ID3D12GraphicsCommandList *list;
    ID3D12DescriptorHeap      *rtv_heap;
    UINT                       rtv_size;
    ID3D12Resource            *backbuffers[D3D12_FRAME_COUNT];
    ID3D12Fence               *fence;
    HANDLE                     fence_event;
    UINT64                     fence_values[D3D12_FRAME_COUNT];
    UINT                       frame_index;   /* current swapchain backbuffer   */
    int                        frame_open;    /* command list recording a frame */
    ID3D12Debug               *debug;
    ID3D12InfoQueue           *info_queue;
    ID3D12Resource            *readback;      /* CPU-readable copy of the BB     */
    UINT64                     readback_size; /* current readback capacity       */
} D3D12State;

static D3D12State g_d3d12;

/* ======================================================================== *
 *  RENDER CORE (Phase 2) -- root signature, PSO cache, upload ring, and
 *  descriptor heaps that back the shared Backend_* draw API on D3D12.
 *
 *  Integration seam: the shared COM layer (device3.c) folds the game's D3D6
 *  render state into g_backend.state (RenderStateCache) exactly as it does for
 *  D3D11; the d3d12 draw path reads that cache to select a cached PSO + sampler
 *  + the viewport/fog root CBVs. One root signature is shared by every draw:
 *    b0 (VERTEX)  = ViewportCB      b0 (PIXEL) = FogCB
 *    t0 table     = bound texture   s0 table   = selected sampler
 *    b1 (PIXEL)   = SDF/FX CB (roundrect/gauge/arrow/cursor/msdf/fx uploads)
 * ======================================================================== */

#include "shaders/vs_pretransformed_bytes_50.h"
#include "shaders/vs_fullscreen_bytes_50.h"
#include "shaders/ps_modulate_bytes_50.h"
#include "shaders/ps_modulate_alpha_bytes_50.h"
#include "shaders/ps_decal_bytes_50.h"
#include "shaders/ps_luminance_alpha_bytes_50.h"
#include "shaders/ps_modulate_g_bytes_50.h"
#include "shaders/ps_modulate_alpha_g_bytes_50.h"

#ifndef TD5_VERTEX_STRIDE
#define TD5_VERTEX_STRIDE 32   /* XYZRHW: float4 pos + BGRA diffuse + BGRA specular + float2 uv */
#endif

/* --- opaque handle bodies (d3d12-private; the d3d11 bodies in
 * d3d11_backend_priv.h carry ID3D11 members and are not usable here) --- */
struct BackendConstBuffer {
    ID3D12Resource *res;    /* UPLOAD-heap, persistent-mapped, 256-aligned */
    void           *mapped;
    UINT            size;   /* 256-aligned byte size                       */
};
struct BackendPixelShader {
    const void *bc;
    SIZE_T      len;
};
struct BackendTexture {
    ID3D12Resource       *res;      /* DEFAULT-heap texture (NULL until created) */
    ID3D12Resource       *upload;   /* persistent UPLOAD staging (dynamic path)  */
    UINT                  srv_slot; /* permanent slot in s_srv_stage             */
    UINT                  rtv_slot; /* slot in s_tex_rtv_heap (has_rtv only)     */
    DXGI_FORMAT           fmt;
    UINT                  w, h;
    D3D12_RESOURCE_STATES rstate;
    int                   has_rtv;
    int                   valid;
    LONG                  ref;
    UINT                  gen;
};

/* --- render-core globals --- */
static ID3D12RootSignature  *s_root_sig;

static ID3D12DescriptorHeap *s_sampler_heap;     /* shader-visible, 4 samplers  */
static UINT                  s_sampler_size;

static ID3D12DescriptorHeap *s_srv_stage;        /* CPU heap: 1 SRV per texture */
static UINT                  s_srv_stage_size;
static UINT                  s_srv_stage_cap;
static UINT                  s_srv_stage_next;

static ID3D12DescriptorHeap *s_srv_ring;         /* shader-visible SRV ring     */
static UINT                  s_srv_ring_cap;
static UINT                  s_srv_ring_next;

static ID3D12DescriptorHeap *s_dsv_heap;         /* 1 DSV for the scene depth   */
static ID3D12Resource       *s_depth_tex;

static ID3D12DescriptorHeap *s_tex_rtv_heap;     /* RTVs for render-target texs */
static UINT                  s_tex_rtv_size;
static UINT                  s_tex_rtv_cap;
static UINT                  s_tex_rtv_next;

/* Per-frame-in-flight persistent-mapped UPLOAD ring (VB/IB/CB bump allocator). */
#define D3D12_UPLOAD_RING_BYTES (32u * 1024u * 1024u)
static ID3D12Resource       *s_upload[D3D12_FRAME_COUNT];
static unsigned char        *s_upload_cpu[D3D12_FRAME_COUNT];
static UINT64                 s_upload_gpu[D3D12_FRAME_COUNT];   /* GPU VA base */
static UINT                   s_upload_off[D3D12_FRAME_COUNT];   /* bump cursor */

/* PSO cache: keyed on {vs, ps, blend, ds, raster, topology-type}. */
typedef struct { UINT64 key; ID3D12PipelineState *pso; } PSOEntry;
#define D3D12_PSO_CACHE_MAX 256
static PSOEntry s_pso_cache[D3D12_PSO_CACHE_MAX];
static int      s_pso_count;

/* Builtin pixel shaders indexed by PS_* (SM5.0 bytecode blobs). */
static BackendPixelShader s_builtin_ps[PS_COUNT];

/* Persistent viewport + fog constant buffers (b0 VS / b0 PS). */
static BackendConstBuffer *s_viewport_cb;
static BackendConstBuffer *s_fog_cb;

/* Current draw state (selected into the next PSO / bindings). */
static int              s_cur_ps    = PS_MODULATE;
static int              s_cur_vs    = 0;              /* 0 = pretransformed      */
static BackendTexture  *s_cur_tex;
static BackendConstBuffer *s_cur_cb1;                 /* b1 SDF/FX (per-draw)    */
static D3D12_VIEWPORT   s_cur_vp;
static D3D12_RECT       s_cur_scissor;

/* ---- plumbing that lived in the filtered-out d3d11 backend files -------- *
 * g_wrapper_rec (deferred pane-record thread-local; the d3d12 backend does not
 * use deferred contexts, so it stays NULL) + the WrapperClipper COM stub +
 * Backend_CaptureDebug (d3d11 photo-capture, n/a here). Backend-agnostic;
 * duplicated for the d3d12 lib -- becomes canonical when d3d11 is deleted in
 * Phase 6. */
__thread WrapperRecCtx *g_wrapper_rec = NULL;

void Backend_CaptureDebug(unsigned *out) { (void)out; }

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
{ (void)riid; if (!ppv) return E_POINTER; *ppv = self; InterlockedIncrement(&self->ref_count); return S_OK; }
static ULONG STDMETHODCALLTYPE Clipper_AddRef(WrapperClipper *self)
{ return (ULONG)InterlockedIncrement(&self->ref_count); }
static ULONG STDMETHODCALLTYPE Clipper_Release(WrapperClipper *self)
{ LONG ref = InterlockedDecrement(&self->ref_count); if (ref <= 0) { HeapFree(GetProcessHeap(), 0, self); return 0; } return (ULONG)ref; }
static HRESULT STDMETHODCALLTYPE Clipper_GetClipList(WrapperClipper *self, RECT *rect, void *rgndata, DWORD *size)
{ (void)self;(void)rect;(void)rgndata;(void)size; WRAPPER_STUB("Clipper::GetClipList"); }
static HRESULT STDMETHODCALLTYPE Clipper_GetHWnd(WrapperClipper *self, HWND *hwnd)
{ if (!hwnd) return E_POINTER; *hwnd = self->hwnd; return DD_OK; }
static HRESULT STDMETHODCALLTYPE Clipper_Initialize(WrapperClipper *self, void *ddraw, DWORD flags)
{ (void)self;(void)ddraw;(void)flags; WRAPPER_STUB("Clipper::Initialize"); }
static HRESULT STDMETHODCALLTYPE Clipper_IsClipListChanged(WrapperClipper *self, BOOL *changed)
{ (void)self; if (changed) *changed = FALSE; return DD_OK; }
static HRESULT STDMETHODCALLTYPE Clipper_SetClipList(WrapperClipper *self, void *rgndata, DWORD flags)
{ (void)self;(void)rgndata;(void)flags; WRAPPER_STUB("Clipper::SetClipList"); }
static HRESULT STDMETHODCALLTYPE Clipper_SetHWnd(WrapperClipper *self, DWORD flags, HWND hwnd)
{ (void)flags; self->hwnd = hwnd; return DD_OK; }
static WrapperClipperVtbl s_clipper_vtbl = {
    Clipper_QueryInterface, Clipper_AddRef, Clipper_Release, Clipper_GetClipList,
    Clipper_GetHWnd, Clipper_Initialize, Clipper_IsClipListChanged, Clipper_SetClipList, Clipper_SetHWnd,
};
WrapperClipper* WrapperClipper_Create(void)
{
    WrapperClipper *clip = (WrapperClipper*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(WrapperClipper));
    if (!clip) return NULL;
    clip->vtbl = (void*)&s_clipper_vtbl;
    clip->ref_count = 1;
    clip->hwnd = NULL;
    return clip;
}

/* ---- helpers ----------------------------------------------------------- */

static D3D12_CPU_DESCRIPTOR_HANDLE d3d12_rtv_handle(UINT i)
{
    /* mingw's C binding returns the handle struct by value (WIDL aggregate-return
     * wrapper), not via an out-param. */
    D3D12_CPU_DESCRIPTOR_HANDLE h;
    /* Call the vtable directly: mingw's COBJMACRO for this aggregate-return
     * method needs WIDL_C_INLINE_WRAPPERS (which would rewrite every COM macro).
     * The raw vtable entry takes the out-param pointer. */
    g_d3d12.rtv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(g_d3d12.rtv_heap, &h);
    h.ptr += (SIZE_T)i * g_d3d12.rtv_size;
    return h;
}

/* CPU-agnostic forensics/env used by this file (the D3D11 versions live in the
 * filtered-out d3d11_backend files, so the d3d12 lib needs its own). */
int Backend_D3DDebugEnabled(void)
{
    static int cached = -1;
    if (cached < 0) {
        const char *e = getenv("TD5RE_D3D_DEBUG");
        cached = (e && e[0] && e[0] != '0') ? 1 : 0;
    }
    return cached;
}

void Backend_NotePresent(void) { }

int Backend_NoteDeviceRemoved(HRESULT hr, const char *where)
{
    if (FAILED(hr)) {
        g_backend.device_removed = 1;
        WRAPPER_LOG("D3D12 DEVICE REMOVED at %s: hr=0x%08lX", where ? where : "?", hr);
    }
    return g_backend.device_removed;
}

static void d3d12_resource_barrier(ID3D12Resource *res,
                                   D3D12_RESOURCE_STATES from,
                                   D3D12_RESOURCE_STATES to)
{
    D3D12_RESOURCE_BARRIER b;
    ZeroMemory(&b, sizeof(b));
    b.Type  = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    b.Transition.pResource   = res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter  = to;
    ID3D12GraphicsCommandList_ResourceBarrier(g_d3d12.list, 1, &b);
}

/* Block until the GPU has finished up to the fence value we last signalled for
 * the given frame slot. */
static void d3d12_wait_frame(UINT slot)
{
    UINT64 want = g_d3d12.fence_values[slot];
    if (ID3D12Fence_GetCompletedValue(g_d3d12.fence) < want) {
        ID3D12Fence_SetEventOnCompletion(g_d3d12.fence, want, g_d3d12.fence_event);
        WaitForSingleObject(g_d3d12.fence_event, INFINITE);
    }
}

static void d3d12_wait_idle(void)
{
    if (!g_d3d12.queue || !g_d3d12.fence) return;
    UINT64 v = ++g_d3d12.fence_values[g_d3d12.frame_index];
    ID3D12CommandQueue_Signal(g_d3d12.queue, g_d3d12.fence, v);
    if (ID3D12Fence_GetCompletedValue(g_d3d12.fence) < v) {
        ID3D12Fence_SetEventOnCompletion(g_d3d12.fence, v, g_d3d12.fence_event);
        WaitForSingleObject(g_d3d12.fence_event, INFINITE);
    }
}

/* Open the command list for the current backbuffer and bind it as the RT. */
static void d3d12_frame_begin(void)
{
    UINT idx;
    if (g_d3d12.frame_open || !g_d3d12.device) return;
    idx = g_d3d12.frame_index;

    ID3D12CommandAllocator_Reset(g_d3d12.allocators[idx]);
    ID3D12GraphicsCommandList_Reset(g_d3d12.list, g_d3d12.allocators[idx], NULL);

    d3d12_resource_barrier(g_d3d12.backbuffers[idx],
        D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);

    {
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = d3d12_rtv_handle(idx);
        ID3D12GraphicsCommandList_OMSetRenderTargets(g_d3d12.list, 1, &rtv, FALSE, NULL);
    }
    g_d3d12.frame_open = 1;
}

/* Close + execute the current frame and Present. */
static void d3d12_frame_present(int sync)
{
    UINT idx = g_d3d12.frame_index;
    HRESULT hr;
    if (!g_d3d12.device || !g_d3d12.swapchain) return;

    if (!g_d3d12.frame_open) d3d12_frame_begin();  /* ensure something to present */

    d3d12_resource_barrier(g_d3d12.backbuffers[idx],
        D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);

    ID3D12GraphicsCommandList_Close(g_d3d12.list);
    {
        ID3D12CommandList *lists[1];
        lists[0] = (ID3D12CommandList *)g_d3d12.list;
        ID3D12CommandQueue_ExecuteCommandLists(g_d3d12.queue, 1, lists);
    }
    g_d3d12.frame_open = 0;

    Backend_NotePresent();
    hr = IDXGISwapChain3_Present(g_d3d12.swapchain, sync ? 1 : 0, 0);
    g_backend.present_count++;
    if (FAILED(hr)) { Backend_NoteDeviceRemoved(hr, "d3d12_frame_present/Present"); return; }

    /* Signal the fence for this slot, then advance to the next backbuffer and
     * wait for its previous work to finish (2 frames in flight). */
    {
        UINT64 v = ++g_d3d12.fence_values[idx];
        ID3D12CommandQueue_Signal(g_d3d12.queue, g_d3d12.fence, v);
    }
    g_d3d12.frame_index = IDXGISwapChain3_GetCurrentBackBufferIndex(g_d3d12.swapchain);
    /* Carry the fence target forward so d3d12_wait_frame guards the slot. */
    g_d3d12.fence_values[g_d3d12.frame_index] = g_d3d12.fence_values[g_d3d12.frame_index];
    d3d12_wait_frame(g_d3d12.frame_index);
}

/* ---- descriptor-handle helpers (aggregate-return -> raw vtable) --------- */

static D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle(ID3D12DescriptorHeap *h, UINT idx, UINT size)
{
    D3D12_CPU_DESCRIPTOR_HANDLE c;
    h->lpVtbl->GetCPUDescriptorHandleForHeapStart(h, &c);
    c.ptr += (SIZE_T)idx * size;
    return c;
}
static D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle(ID3D12DescriptorHeap *h, UINT idx, UINT size)
{
    D3D12_GPU_DESCRIPTOR_HANDLE g;
    h->lpVtbl->GetGPUDescriptorHandleForHeapStart(h, &g);
    g.ptr += (UINT64)idx * size;
    return g;
}

/* ---- render-state -> D3D12 pipeline descriptors (verbatim from the D3D11
 *      state-object descs in d3d11_backend_device.c so raster output matches) */

static const D3D12_INPUT_ELEMENT_DESC s_input_layout[4] = {
    { "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    { "COLOR",    0, DXGI_FORMAT_B8G8R8A8_UNORM,      0, 16, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    { "COLOR",    1, DXGI_FORMAT_B8G8R8A8_UNORM,      0, 20, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,        0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
};

static void d3d12_fill_blend(int idx, D3D12_BLEND_DESC *bd)
{
    D3D12_RENDER_TARGET_BLEND_DESC *rt = &bd->RenderTarget[0];
    ZeroMemory(bd, sizeof(*bd));
    rt->RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    rt->BlendOp       = D3D12_BLEND_OP_ADD;
    rt->BlendOpAlpha  = D3D12_BLEND_OP_ADD;
    rt->SrcBlendAlpha = D3D12_BLEND_ONE;
    rt->DestBlendAlpha= D3D12_BLEND_INV_SRC_ALPHA;
    switch (idx) {
    case BLEND_OPAQUE:
        rt->BlendEnable = FALSE; return;
    case BLEND_SRCALPHA_INVSRC:
        rt->BlendEnable=TRUE; rt->SrcBlend=D3D12_BLEND_SRC_ALPHA; rt->DestBlend=D3D12_BLEND_INV_SRC_ALPHA;
        rt->SrcBlendAlpha=D3D12_BLEND_ONE; rt->DestBlendAlpha=D3D12_BLEND_INV_SRC_ALPHA; return;
    case BLEND_SRCALPHA_ONE:
        rt->BlendEnable=TRUE; rt->SrcBlend=D3D12_BLEND_SRC_ALPHA; rt->DestBlend=D3D12_BLEND_ONE;
        rt->SrcBlendAlpha=D3D12_BLEND_ONE; rt->DestBlendAlpha=D3D12_BLEND_ONE; return;
    case BLEND_ONE_ONE:
        rt->BlendEnable=TRUE; rt->SrcBlend=D3D12_BLEND_ONE; rt->DestBlend=D3D12_BLEND_ONE;
        rt->SrcBlendAlpha=D3D12_BLEND_ONE; rt->DestBlendAlpha=D3D12_BLEND_ONE; return;
    case BLEND_SRCALPHA_SRCALPHA:
        rt->BlendEnable=TRUE; rt->SrcBlend=D3D12_BLEND_SRC_ALPHA; rt->DestBlend=D3D12_BLEND_SRC_ALPHA;
        rt->SrcBlendAlpha=D3D12_BLEND_ONE; rt->DestBlendAlpha=D3D12_BLEND_SRC_ALPHA; return;
    case BLEND_INVSRC_INVSRC:
        rt->BlendEnable=TRUE; rt->SrcBlend=D3D12_BLEND_INV_SRC_ALPHA; rt->DestBlend=D3D12_BLEND_INV_SRC_ALPHA;
        rt->SrcBlendAlpha=D3D12_BLEND_ONE; rt->DestBlendAlpha=D3D12_BLEND_INV_SRC_ALPHA; return;
    case BLEND_MULT:
        rt->BlendEnable=TRUE; rt->SrcBlend=D3D12_BLEND_ZERO; rt->DestBlend=D3D12_BLEND_SRC_COLOR;
        rt->SrcBlendAlpha=D3D12_BLEND_ZERO; rt->DestBlendAlpha=D3D12_BLEND_ONE; return;
    default:
        rt->BlendEnable=FALSE; return;
    }
}

static void d3d12_fill_ds(int idx, D3D12_DEPTH_STENCIL_DESC *dd)
{
    ZeroMemory(dd, sizeof(*dd));
    dd->DepthFunc    = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    dd->StencilEnable= FALSE;
    switch (idx) {
    case DS_Z_ON_WRITE_ON:         dd->DepthEnable=TRUE;  dd->DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ALL;  break;
    case DS_Z_ON_WRITE_OFF:        dd->DepthEnable=TRUE;  dd->DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ZERO; break;
    case DS_Z_OFF_WRITE_OFF:       dd->DepthEnable=FALSE; dd->DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ZERO; break;
    case DS_Z_OFF_WRITE_ON:        dd->DepthEnable=FALSE; dd->DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ALL;  break;
    case DS_Z_ON_WRITE_ON_ALWAYS:  dd->DepthEnable=TRUE;  dd->DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ALL;  dd->DepthFunc=D3D12_COMPARISON_FUNC_ALWAYS; break;
    case DS_Z_ON_WRITE_OFF_ALWAYS: dd->DepthEnable=TRUE;  dd->DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ZERO; dd->DepthFunc=D3D12_COMPARISON_FUNC_ALWAYS; break;
    default:                       dd->DepthEnable=TRUE;  dd->DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ALL;  break;
    }
}

static void d3d12_fill_raster(int idx, D3D12_RASTERIZER_DESC *rd)
{
    ZeroMemory(rd, sizeof(*rd));
    rd->FillMode = D3D12_FILL_MODE_SOLID;
    rd->CullMode = D3D12_CULL_MODE_NONE;
    rd->DepthClipEnable = FALSE;   /* legacy XYZRHW never clipped on Z */
    if (idx == 1) { rd->DepthBias = -500; rd->SlopeScaledDepthBias = -1.5f; }  /* shadow decal */
}

/* ---- PSO cache --------------------------------------------------------- */

static void d3d12_resolve_ps(int ps_id, const void **bc, SIZE_T *len)
{
    if (ps_id >= 0 && ps_id < PS_COUNT) { *bc = s_builtin_ps[ps_id].bc; *len = s_builtin_ps[ps_id].len; }
    else { *bc = s_builtin_ps[PS_MODULATE].bc; *len = s_builtin_ps[PS_MODULATE].len; }
}

/* topo_type: 0=triangle, 1=line */
static ID3D12PipelineState *d3d12_get_pso(int vs_idx, int ps_id, int blend, int ds, int raster, int topo_type)
{
    UINT64 key;
    int i;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd;
    ID3D12PipelineState *pso = NULL;
    const void *vbc, *pbc; SIZE_T vlen, plen;
    HRESULT hr;

    key = ((UINT64)(vs_idx & 0xF)) | ((UINT64)(ps_id & 0x3F) << 4) | ((UINT64)(blend & 0xF) << 10)
        | ((UINT64)(ds & 0xF) << 14) | ((UINT64)(raster & 0x3) << 18) | ((UINT64)(topo_type & 0x3) << 20);
    for (i = 0; i < s_pso_count; i++) if (s_pso_cache[i].key == key) return s_pso_cache[i].pso;
    if (s_pso_count >= D3D12_PSO_CACHE_MAX) { WRAPPER_LOG("D3D12 PSO cache full"); return NULL; }

    vbc = (vs_idx == 1) ? (const void *)g_vs_fullscreen_50 : (const void *)g_vs_pretransformed_50;
    vlen= (vs_idx == 1) ? sizeof(g_vs_fullscreen_50) : sizeof(g_vs_pretransformed_50);
    d3d12_resolve_ps(ps_id, &pbc, &plen);

    ZeroMemory(&pd, sizeof(pd));
    pd.pRootSignature = s_root_sig;
    pd.VS.pShaderBytecode = vbc; pd.VS.BytecodeLength = vlen;
    pd.PS.pShaderBytecode = pbc; pd.PS.BytecodeLength = plen;
    d3d12_fill_blend(blend, &pd.BlendState);
    d3d12_fill_ds(ds, &pd.DepthStencilState);
    d3d12_fill_raster(raster, &pd.RasterizerState);
    pd.SampleMask = 0xFFFFFFFFu;
    pd.InputLayout.pInputElementDescs = s_input_layout;
    pd.InputLayout.NumElements = 4;
    pd.PrimitiveTopologyType = topo_type == 1 ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE
                                              : D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM;
    pd.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    pd.SampleDesc.Count = 1;

    hr = ID3D12Device_CreateGraphicsPipelineState(g_d3d12.device, &pd, &IID_ID3D12PipelineState, (void **)&pso);
    if (FAILED(hr) || !pso) { WRAPPER_LOG("D3D12 CreateGraphicsPipelineState (ps=%d blend=%d ds=%d) 0x%08lX", ps_id, blend, ds, hr); return NULL; }
    s_pso_cache[s_pso_count].key = key;
    s_pso_cache[s_pso_count].pso = pso;
    s_pso_count++;
    return pso;
}

/* ---- upload ring + const buffers --------------------------------------- */

/* Bump-allocate `size` bytes (aligned) from the current frame's upload ring;
 * returns the GPU VA and copies `src` in when non-NULL. 0 on overflow. */
static UINT64 d3d12_upload(const void *src, UINT size, UINT align, UINT *out_off)
{
    UINT idx = g_d3d12.frame_index;
    UINT off = (s_upload_off[idx] + (align - 1)) & ~(align - 1);
    if (off + size > D3D12_UPLOAD_RING_BYTES) { WRAPPER_LOG("D3D12 upload ring overflow"); return 0; }
    if (src) memcpy(s_upload_cpu[idx] + off, src, size);
    s_upload_off[idx] = off + size;
    if (out_off) *out_off = off;
    return s_upload_gpu[idx] + off;
}

static BackendConstBuffer *d3d12_cb_create(UINT size)
{
    BackendConstBuffer *cb;
    D3D12_HEAP_PROPERTIES hp;
    D3D12_RESOURCE_DESC bd;
    HRESULT hr;
    UINT asz = (size + 255u) & ~255u;

    cb = (BackendConstBuffer *)calloc(1, sizeof(*cb));
    if (!cb) return NULL;
    ZeroMemory(&hp, sizeof(hp)); hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    ZeroMemory(&bd, sizeof(bd));
    bd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width=asz; bd.Height=1; bd.DepthOrArraySize=1;
    bd.MipLevels=1; bd.Format=DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count=1; bd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    hr = ID3D12Device_CreateCommittedResource(g_d3d12.device, &hp, D3D12_HEAP_FLAG_NONE, &bd,
            D3D12_RESOURCE_STATE_GENERIC_READ, NULL, &IID_ID3D12Resource, (void **)&cb->res);
    if (FAILED(hr)) { free(cb); return NULL; }
    { D3D12_RANGE r; r.Begin=0; r.End=0; ID3D12Resource_Map(cb->res, 0, &r, &cb->mapped); }
    cb->size = asz;
    return cb;
}

/* ---- render-core init / teardown --------------------------------------- */

static int d3d12_create_root_sig(void)
{
    D3D12_DESCRIPTOR_RANGE srv_range, samp_range;
    D3D12_ROOT_PARAMETER params[5];
    D3D12_ROOT_SIGNATURE_DESC rsd;
    ID3D10Blob *sig = NULL, *err = NULL;
    HRESULT hr;

    ZeroMemory(&srv_range, sizeof(srv_range));
    srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srv_range.NumDescriptors = 1; srv_range.BaseShaderRegister = 0;
    srv_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    ZeroMemory(&samp_range, sizeof(samp_range));
    samp_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
    samp_range.NumDescriptors = 1; samp_range.BaseShaderRegister = 0;
    samp_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    ZeroMemory(params, sizeof(params));
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;              /* b0 VS: viewport */
    params[0].Descriptor.ShaderRegister = 0;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;              /* b0 PS: fog */
    params[1].Descriptor.ShaderRegister = 0;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; /* t0 PS: texture */
    params[2].DescriptorTable.NumDescriptorRanges = 1;
    params[2].DescriptorTable.pDescriptorRanges = &srv_range;
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; /* s0 PS: sampler */
    params[3].DescriptorTable.NumDescriptorRanges = 1;
    params[3].DescriptorTable.pDescriptorRanges = &samp_range;
    params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;              /* b1 PS: SDF/FX */
    params[4].Descriptor.ShaderRegister = 1;
    params[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    ZeroMemory(&rsd, sizeof(rsd));
    rsd.NumParameters = 5; rsd.pParameters = params;
    rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err);
    if (FAILED(hr)) {
        WRAPPER_LOG("D3D12 SerializeRootSignature 0x%08lX %s", hr,
                    err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "");
        if (err) ID3D10Blob_Release(err);
        return 0;
    }
    hr = ID3D12Device_CreateRootSignature(g_d3d12.device, 0,
            ID3D10Blob_GetBufferPointer(sig), ID3D10Blob_GetBufferSize(sig),
            &IID_ID3D12RootSignature, (void **)&s_root_sig);
    ID3D10Blob_Release(sig);
    if (err) ID3D10Blob_Release(err);
    if (FAILED(hr)) { WRAPPER_LOG("D3D12 CreateRootSignature 0x%08lX", hr); return 0; }
    return 1;
}

static void d3d12_create_samplers(void)
{
    D3D12_SAMPLER_DESC sd;
    int i;
    static const struct { D3D12_FILTER f; D3D12_TEXTURE_ADDRESS_MODE a; } tbl[SAMP_STATE_COUNT] = {
        { D3D12_FILTER_MIN_MAG_MIP_POINT,  D3D12_TEXTURE_ADDRESS_MODE_WRAP  }, /* SAMP_POINT_WRAP  */
        { D3D12_FILTER_MIN_MAG_MIP_POINT,  D3D12_TEXTURE_ADDRESS_MODE_CLAMP }, /* SAMP_POINT_CLAMP */
        { D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_WRAP  }, /* SAMP_LINEAR_WRAP */
        { D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_CLAMP }, /* SAMP_LINEAR_CLAMP*/
    };
    for (i = 0; i < SAMP_STATE_COUNT; i++) {
        ZeroMemory(&sd, sizeof(sd));
        sd.Filter = tbl[i].f;
        sd.AddressU = sd.AddressV = sd.AddressW = tbl[i].a;
        sd.MaxAnisotropy = 1;
        sd.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
        sd.MinLOD = 0.0f; sd.MaxLOD = D3D12_FLOAT32_MAX;
        ID3D12Device_CreateSampler(g_d3d12.device, &sd, cpu_handle(s_sampler_heap, i, s_sampler_size));
    }
}

static int d3d12_render_core_init(int width, int height)
{
    D3D12_DESCRIPTOR_HEAP_DESC hd;
    HRESULT hr;
    UINT i;

    if (!d3d12_create_root_sig()) return 0;

    /* Shader-visible SAMPLER heap (4 samplers). */
    ZeroMemory(&hd, sizeof(hd));
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER; hd.NumDescriptors = SAMP_STATE_COUNT;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(ID3D12Device_CreateDescriptorHeap(g_d3d12.device, &hd, &IID_ID3D12DescriptorHeap, (void **)&s_sampler_heap))) return 0;
    s_sampler_size = ID3D12Device_GetDescriptorHandleIncrementSize(g_d3d12.device, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    d3d12_create_samplers();

    s_srv_stage_size = ID3D12Device_GetDescriptorHandleIncrementSize(g_d3d12.device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    /* CPU-only staging SRV heap (one permanent slot per texture). */
    s_srv_stage_cap = 8192;
    ZeroMemory(&hd, sizeof(hd));
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors = s_srv_stage_cap;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    if (FAILED(ID3D12Device_CreateDescriptorHeap(g_d3d12.device, &hd, &IID_ID3D12DescriptorHeap, (void **)&s_srv_stage))) return 0;
    s_srv_stage_next = 1;   /* slot 0 reserved for a null/white default */

    /* Shader-visible SRV ring (one slot per textured draw, wraps per frame). */
    s_srv_ring_cap = 16384;
    ZeroMemory(&hd, sizeof(hd));
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors = s_srv_ring_cap;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(ID3D12Device_CreateDescriptorHeap(g_d3d12.device, &hd, &IID_ID3D12DescriptorHeap, (void **)&s_srv_ring))) return 0;

    /* RTV heap for render-target textures (surfaces / G-buffer). */
    s_tex_rtv_size = g_d3d12.rtv_size;
    s_tex_rtv_cap = 256;
    ZeroMemory(&hd, sizeof(hd));
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = s_tex_rtv_cap;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    if (FAILED(ID3D12Device_CreateDescriptorHeap(g_d3d12.device, &hd, &IID_ID3D12DescriptorHeap, (void **)&s_tex_rtv_heap))) return 0;

    /* Scene depth buffer (D32_FLOAT) + DSV. */
    {
        D3D12_HEAP_PROPERTIES hp;
        D3D12_RESOURCE_DESC   td;
        D3D12_CLEAR_VALUE     cv;
        D3D12_DEPTH_STENCIL_VIEW_DESC dvd;
        ZeroMemory(&hp, sizeof(hp)); hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        ZeroMemory(&td, sizeof(td));
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = (UINT64)width; td.Height = (UINT)height; td.DepthOrArraySize = 1; td.MipLevels = 1;
        td.Format = DXGI_FORMAT_D32_FLOAT; td.SampleDesc.Count = 1;
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        ZeroMemory(&cv, sizeof(cv)); cv.Format = DXGI_FORMAT_D32_FLOAT; cv.DepthStencil.Depth = 1.0f;
        hr = ID3D12Device_CreateCommittedResource(g_d3d12.device, &hp, D3D12_HEAP_FLAG_NONE, &td,
                D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv, &IID_ID3D12Resource, (void **)&s_depth_tex);
        if (FAILED(hr)) return 0;
        ZeroMemory(&hd, sizeof(hd));
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV; hd.NumDescriptors = 1;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        if (FAILED(ID3D12Device_CreateDescriptorHeap(g_d3d12.device, &hd, &IID_ID3D12DescriptorHeap, (void **)&s_dsv_heap))) return 0;
        ZeroMemory(&dvd, sizeof(dvd)); dvd.Format = DXGI_FORMAT_D32_FLOAT; dvd.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        ID3D12Device_CreateDepthStencilView(g_d3d12.device, s_depth_tex, &dvd, cpu_handle(s_dsv_heap, 0, 1));
    }

    /* Per-frame persistent-mapped UPLOAD ring. */
    for (i = 0; i < D3D12_FRAME_COUNT; i++) {
        D3D12_HEAP_PROPERTIES hp;
        D3D12_RESOURCE_DESC   bd;
        void *p = NULL;
        ZeroMemory(&hp, sizeof(hp)); hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        ZeroMemory(&bd, sizeof(bd));
        bd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width=D3D12_UPLOAD_RING_BYTES; bd.Height=1;
        bd.DepthOrArraySize=1; bd.MipLevels=1; bd.Format=DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count=1;
        bd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        hr = ID3D12Device_CreateCommittedResource(g_d3d12.device, &hp, D3D12_HEAP_FLAG_NONE, &bd,
                D3D12_RESOURCE_STATE_GENERIC_READ, NULL, &IID_ID3D12Resource, (void **)&s_upload[i]);
        if (FAILED(hr)) return 0;
        { D3D12_RANGE r; r.Begin=0; r.End=0; ID3D12Resource_Map(s_upload[i], 0, &r, &p); }
        s_upload_cpu[i] = (unsigned char *)p;
        s_upload_gpu[i] = ID3D12Resource_GetGPUVirtualAddress(s_upload[i]);
        s_upload_off[i] = 0;
    }

    /* Builtin PS bytecode table. */
    s_builtin_ps[PS_MODULATE].bc        = g_ps_modulate_50;        s_builtin_ps[PS_MODULATE].len        = sizeof(g_ps_modulate_50);
    s_builtin_ps[PS_MODULATE_ALPHA].bc  = g_ps_modulate_alpha_50;  s_builtin_ps[PS_MODULATE_ALPHA].len  = sizeof(g_ps_modulate_alpha_50);
    s_builtin_ps[PS_DECAL].bc           = g_ps_decal_50;           s_builtin_ps[PS_DECAL].len           = sizeof(g_ps_decal_50);
    s_builtin_ps[PS_LUMINANCE_ALPHA].bc = g_ps_luminance_alpha_50; s_builtin_ps[PS_LUMINANCE_ALPHA].len = sizeof(g_ps_luminance_alpha_50);
    s_builtin_ps[PS_MODULATE_G].bc      = g_ps_modulate_g_50;      s_builtin_ps[PS_MODULATE_G].len      = sizeof(g_ps_modulate_g_50);
    s_builtin_ps[PS_MODULATE_ALPHA_G].bc= g_ps_modulate_alpha_g_50;s_builtin_ps[PS_MODULATE_ALPHA_G].len= sizeof(g_ps_modulate_alpha_g_50);

    /* Persistent viewport (b0 VS) + fog (b0 PS) const buffers. */
    s_viewport_cb = d3d12_cb_create(sizeof(ViewportCB));
    s_fog_cb      = d3d12_cb_create(sizeof(FogCB));
    if (!s_viewport_cb || !s_fog_cb) return 0;
    { ViewportCB vp; ZeroMemory(&vp,sizeof(vp)); vp.viewportWidth=(float)width; vp.viewportHeight=(float)height;
      memcpy(s_viewport_cb->mapped, &vp, sizeof(vp)); }
    { FogCB fog; ZeroMemory(&fog,sizeof(fog)); memcpy(s_fog_cb->mapped, &fog, sizeof(fog)); }

    /* Warm-up PSO: exercises get_pso + validates root-sig/shader/layout wiring
     * against the debug layer at init (before any frame). */
    if (!d3d12_get_pso(0, PS_MODULATE, BLEND_SRCALPHA_INVSRC, DS_Z_OFF_WRITE_OFF, 0, 0)) return 0;

    WRAPPER_LOG("D3D12 render core: root sig + %d samplers + heaps + %uMB upload ring x%d + warm PSO OK",
                SAMP_STATE_COUNT, D3D12_UPLOAD_RING_BYTES >> 20, D3D12_FRAME_COUNT);
    return 1;
}

static void d3d12_render_core_shutdown(void)
{
    int i;
    if (s_viewport_cb) { if (s_viewport_cb->res) ID3D12Resource_Release(s_viewport_cb->res); free(s_viewport_cb); s_viewport_cb = NULL; }
    if (s_fog_cb)      { if (s_fog_cb->res)      ID3D12Resource_Release(s_fog_cb->res);      free(s_fog_cb);      s_fog_cb = NULL; }
    for (i = 0; i < s_pso_count; i++) if (s_pso_cache[i].pso) ID3D12PipelineState_Release(s_pso_cache[i].pso);
    s_pso_count = 0;
    for (i = 0; i < D3D12_FRAME_COUNT; i++) if (s_upload[i]) { ID3D12Resource_Release(s_upload[i]); s_upload[i] = NULL; s_upload_cpu[i] = NULL; }
    if (s_depth_tex)     { ID3D12Resource_Release(s_depth_tex); s_depth_tex = NULL; }
    if (s_dsv_heap)      { ID3D12DescriptorHeap_Release(s_dsv_heap); s_dsv_heap = NULL; }
    if (s_tex_rtv_heap)  { ID3D12DescriptorHeap_Release(s_tex_rtv_heap); s_tex_rtv_heap = NULL; }
    if (s_srv_ring)      { ID3D12DescriptorHeap_Release(s_srv_ring); s_srv_ring = NULL; }
    if (s_srv_stage)     { ID3D12DescriptorHeap_Release(s_srv_stage); s_srv_stage = NULL; }
    if (s_sampler_heap)  { ID3D12DescriptorHeap_Release(s_sampler_heap); s_sampler_heap = NULL; }
    if (s_root_sig)      { ID3D12RootSignature_Release(s_root_sig); s_root_sig = NULL; }
}

/* ---- device lifecycle -------------------------------------------------- */

int Backend_CreateDevice(HWND hwnd, int width, int height, int bpp, int windowed)
{
    IDXGIFactory4 *factory = NULL;
    IDXGISwapChain1 *sc1 = NULL;
    DXGI_SWAP_CHAIN_DESC1 scd;
    D3D12_COMMAND_QUEUE_DESC qd;
    D3D12_DESCRIPTOR_HEAP_DESC hd;
    UINT i;
    HRESULT hr;

    ZeroMemory(&g_d3d12, sizeof(g_d3d12));

    /* Debug layer + DRED under TD5RE_D3D_DEBUG. */
    if (Backend_D3DDebugEnabled()) {
        if (SUCCEEDED(D3D12GetDebugInterface(&IID_ID3D12Debug, (void **)&g_d3d12.debug)))
            ID3D12Debug_EnableDebugLayer(g_d3d12.debug);
        {
            ID3D12DeviceRemovedExtendedDataSettings *dred = NULL;
            if (SUCCEEDED(D3D12GetDebugInterface(&IID_ID3D12DeviceRemovedExtendedDataSettings, (void **)&dred))) {
                ID3D12DeviceRemovedExtendedDataSettings_SetAutoBreadcrumbsEnablement(dred, D3D12_DRED_ENABLEMENT_FORCED_ON);
                ID3D12DeviceRemovedExtendedDataSettings_SetPageFaultEnablement(dred, D3D12_DRED_ENABLEMENT_FORCED_ON);
                ID3D12DeviceRemovedExtendedDataSettings_Release(dred);
            }
        }
    }

    /* NB: do NOT pass DXGI_CREATE_FACTORY_DEBUG -- it requires the DXGI debug
     * layer (dxgidebug.dll), absent on some hosts, and fails factory creation.
     * The D3D12 debug layer (EnableDebugLayer above) provides the validation. */
    hr = CreateDXGIFactory2(0, &IID_IDXGIFactory4, (void **)&factory);
    if (FAILED(hr)) { WRAPPER_LOG("D3D12: CreateDXGIFactory2 0x%08lX", hr); return 0; }

    hr = D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&g_d3d12.device);
    if (FAILED(hr) || !g_d3d12.device) {
        WRAPPER_LOG("D3D12: D3D12CreateDevice(FL11_0) 0x%08lX", hr);
        IDXGIFactory4_Release(factory);
        return 0;
    }

    if (Backend_D3DDebugEnabled())
        ID3D12Device_QueryInterface(g_d3d12.device, &IID_ID3D12InfoQueue, (void **)&g_d3d12.info_queue);

    /* Direct command queue. */
    ZeroMemory(&qd, sizeof(qd));
    qd.Type  = D3D12_COMMAND_LIST_TYPE_DIRECT;
    qd.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    hr = ID3D12Device_CreateCommandQueue(g_d3d12.device, &qd, &IID_ID3D12CommandQueue, (void **)&g_d3d12.queue);
    if (FAILED(hr)) { WRAPPER_LOG("D3D12: CreateCommandQueue 0x%08lX", hr); goto fail; }

    /* Flip-discard swapchain, BufferCount=2, B8G8R8A8 (forced change from the
     * D3D11 BufferCount=1/DISCARD -- harmless because the game never renders to
     * the swapchain directly in the finished port; skeleton clears it). */
    ZeroMemory(&scd, sizeof(scd));
    scd.BufferCount      = D3D12_FRAME_COUNT;
    scd.Width            = (UINT)width;
    scd.Height           = (UINT)height;
    scd.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
    scd.BufferUsage      = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.SwapEffect       = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scd.SampleDesc.Count = 1;
    hr = IDXGIFactory4_CreateSwapChainForHwnd(factory, (IUnknown *)g_d3d12.queue,
            hwnd, &scd, NULL, NULL, &sc1);
    if (FAILED(hr)) { WRAPPER_LOG("D3D12: CreateSwapChainForHwnd 0x%08lX", hr); goto fail; }
    IDXGISwapChain1_QueryInterface(sc1, &IID_IDXGISwapChain3, (void **)&g_d3d12.swapchain);
    IDXGISwapChain1_Release(sc1);
    /* Keep DXGI from stealing Alt+Enter; window management stays with the port. */
    IDXGIFactory4_MakeWindowAssociation(factory, hwnd, DXGI_MWA_NO_ALT_ENTER);
    g_d3d12.frame_index = IDXGISwapChain3_GetCurrentBackBufferIndex(g_d3d12.swapchain);

    /* RTV heap + a view per backbuffer. */
    ZeroMemory(&hd, sizeof(hd));
    hd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = D3D12_FRAME_COUNT;
    hd.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    hr = ID3D12Device_CreateDescriptorHeap(g_d3d12.device, &hd, &IID_ID3D12DescriptorHeap, (void **)&g_d3d12.rtv_heap);
    if (FAILED(hr)) { WRAPPER_LOG("D3D12: CreateDescriptorHeap(RTV) 0x%08lX", hr); goto fail; }
    g_d3d12.rtv_size = ID3D12Device_GetDescriptorHandleIncrementSize(g_d3d12.device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    for (i = 0; i < D3D12_FRAME_COUNT; i++) {
        hr = IDXGISwapChain3_GetBuffer(g_d3d12.swapchain, i, &IID_ID3D12Resource, (void **)&g_d3d12.backbuffers[i]);
        if (FAILED(hr)) { WRAPPER_LOG("D3D12: swapchain GetBuffer(%u) 0x%08lX", i, hr); goto fail; }
        {
            D3D12_CPU_DESCRIPTOR_HANDLE rtv = d3d12_rtv_handle(i);
            ID3D12Device_CreateRenderTargetView(g_d3d12.device, g_d3d12.backbuffers[i], NULL, rtv);
        }
        hr = ID3D12Device_CreateCommandAllocator(g_d3d12.device, D3D12_COMMAND_LIST_TYPE_DIRECT,
                &IID_ID3D12CommandAllocator, (void **)&g_d3d12.allocators[i]);
        if (FAILED(hr)) { WRAPPER_LOG("D3D12: CreateCommandAllocator(%u) 0x%08lX", i, hr); goto fail; }
        g_d3d12.fence_values[i] = 0;
    }

    /* Graphics command list (created open, immediately closed). */
    hr = ID3D12Device_CreateCommandList(g_d3d12.device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT,
            g_d3d12.allocators[g_d3d12.frame_index], NULL, &IID_ID3D12GraphicsCommandList, (void **)&g_d3d12.list);
    if (FAILED(hr)) { WRAPPER_LOG("D3D12: CreateCommandList 0x%08lX", hr); goto fail; }
    ID3D12GraphicsCommandList_Close(g_d3d12.list);

    /* Fence + event. */
    hr = ID3D12Device_CreateFence(g_d3d12.device, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&g_d3d12.fence);
    if (FAILED(hr)) { WRAPPER_LOG("D3D12: CreateFence 0x%08lX", hr); goto fail; }
    g_d3d12.fence_event = CreateEventA(NULL, FALSE, FALSE, NULL);

    /* Phase 2 render core: root sig, PSO cache, upload ring, descriptor heaps. */
    {
        int rc_ok = d3d12_render_core_init(width, height);
        Backend_DrainD3DDebug(rc_ok ? "render_core_init_ok" : "render_core_init_FAIL");
        if (!rc_ok) {
            WRAPPER_LOG("D3D12: render core init FAILED");
            IDXGIFactory4_Release(factory);
            Backend_Shutdown();
            return 0;
        }
    }

    IDXGIFactory4_Release(factory);

    /* Publish backend-agnostic state to the shared g_backend view. */
    g_backend.hwnd     = hwnd;
    g_backend.width    = width;
    g_backend.height   = height;
    g_backend.bpp      = bpp;
    g_backend.windowed = windowed;
    g_backend.swap_chain = (IDXGISwapChain *)g_d3d12.swapchain;  /* base pointer for HasSwapChain */
    g_backend.device_generation = (g_backend.device_generation ? g_backend.device_generation : 1);
    g_backend.device_removed = 0;

    WRAPPER_LOG("D3D12: device + swapchain created %dx%d (backbuffers=%d, flip-discard)",
                width, height, D3D12_FRAME_COUNT);
    return 1;

fail:
    if (factory) IDXGIFactory4_Release(factory);
    Backend_Shutdown();
    return 0;
}

void Backend_Shutdown(void)
{
    UINT i;
    d3d12_wait_idle();
    d3d12_render_core_shutdown();
    if (g_d3d12.list)     ID3D12GraphicsCommandList_Release(g_d3d12.list);
    for (i = 0; i < D3D12_FRAME_COUNT; i++) {
        if (g_d3d12.backbuffers[i]) ID3D12Resource_Release(g_d3d12.backbuffers[i]);
        if (g_d3d12.allocators[i])  ID3D12CommandAllocator_Release(g_d3d12.allocators[i]);
    }
    if (g_d3d12.readback)   ID3D12Resource_Release(g_d3d12.readback);
    if (g_d3d12.rtv_heap)   ID3D12DescriptorHeap_Release(g_d3d12.rtv_heap);
    if (g_d3d12.fence)      ID3D12Fence_Release(g_d3d12.fence);
    if (g_d3d12.fence_event) CloseHandle(g_d3d12.fence_event);
    if (g_d3d12.swapchain)  IDXGISwapChain3_Release(g_d3d12.swapchain);
    if (g_d3d12.queue)      ID3D12CommandQueue_Release(g_d3d12.queue);
    if (g_d3d12.info_queue) ID3D12InfoQueue_Release(g_d3d12.info_queue);
    if (g_d3d12.device)     ID3D12Device_Release(g_d3d12.device);
    if (g_d3d12.debug)      ID3D12Debug_Release(g_d3d12.debug);
    ZeroMemory(&g_d3d12, sizeof(g_d3d12));
    g_backend.swap_chain = NULL;
}

/* ---- readiness --------------------------------------------------------- */

int Backend_HasDevice(void)     { return g_d3d12.device != NULL; }
int Backend_HasContext(void)    { return g_d3d12.list != NULL; }     /* command list == "context" */
int Backend_HasSwapChain(void)  { return g_d3d12.swapchain != NULL; }
int Backend_SwapChainReady(void){ return g_d3d12.device && g_d3d12.swapchain && g_d3d12.list; }

/* ---- clear / present --------------------------------------------------- */

void Backend_ClearBackbuffer(const float *rgba)
{
    if (!g_d3d12.device) return;
    d3d12_frame_begin();
    {
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = d3d12_rtv_handle(g_d3d12.frame_index);
        ID3D12GraphicsCommandList_ClearRenderTargetView(g_d3d12.list, rtv, rgba, 0, NULL);
    }
}

void Backend_ClearSwapChainRT(const float *rgba) { Backend_ClearBackbuffer(rgba); }
void Backend_BindSwapChainRT(void)               { d3d12_frame_begin(); }
void Backend_ClearDepth(float z)                 { (void)z; /* no depth buffer in the skeleton yet */ }

void Backend_PresentSwapChain(int sync)          { d3d12_frame_present(sync); }
void Backend_CompositeAndPresent(WrapperSurface *rt, RECT *s, RECT *d)
{
    (void)rt; (void)s; (void)d;
    d3d12_frame_present(g_backend.vsync ? 1 : 0);
}

HWND Backend_GetDisplayWindow(void) { return g_backend.hwnd; }

/* Framedump / render-golden capture: read back the current backbuffer as a
 * freshly-malloc'd RGBA8 buffer (caller frees), matching the D3D11
 * Backend_CaptureBackbufferRGBA contract.
 *
 * D3D12 flip-discard makes a *post*-present readback unreliable (the presented
 * surface is recycled), so this captures the in-flight frame synchronously: it
 * copies the backbuffer (which holds this frame's rendered content while the
 * frame command list is open) into a READBACK-heap buffer on that same list,
 * flushes to the GPU, waits, and maps. The frame is left in PRESENT state with
 * frame_open=0, so the game's subsequent Present begins a fresh frame cleanly.
 * (Cost: the captured frame itself is not shown -- acceptable for a periodic
 * dev framedump / A-B capture; the next frame presents normally.) */
unsigned char *Backend_CaptureBackbufferRGBA(int *out_w, int *out_h)
{
    UINT idx, y, x;
    ID3D12Resource *bb;
    D3D12_RESOURCE_DESC rd;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
    UINT num_rows = 0;
    UINT64 row_bytes = 0, total = 0;
    unsigned char *out, *mapped = NULL;
    UINT w, h;
    HRESULT hr;

    if (!g_d3d12.device) return NULL;

    /* Ensure the backbuffer holds a rendered (or at least cleared) frame. */
    if (!g_d3d12.frame_open) d3d12_frame_begin();
    idx = g_d3d12.frame_index;
    bb  = g_d3d12.backbuffers[idx];

    /* GetDesc is an aggregate-return method -> call the raw vtable with out-param
     * (same mingw/WIDL reason as d3d12_rtv_handle). */
    bb->lpVtbl->GetDesc(bb, &rd);
    w = (UINT)rd.Width;
    h = rd.Height;

    ID3D12Device_GetCopyableFootprints(g_d3d12.device, &rd, 0, 1, 0,
                                       &fp, &num_rows, &row_bytes, &total);

    /* (Re)allocate the READBACK buffer if it is missing or too small. */
    if (!g_d3d12.readback || g_d3d12.readback_size < total) {
        D3D12_HEAP_PROPERTIES hp;
        D3D12_RESOURCE_DESC   bd;
        if (g_d3d12.readback) { ID3D12Resource_Release(g_d3d12.readback); g_d3d12.readback = NULL; }
        ZeroMemory(&hp, sizeof(hp));
        hp.Type = D3D12_HEAP_TYPE_READBACK;
        ZeroMemory(&bd, sizeof(bd));
        bd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width            = total;
        bd.Height           = 1;
        bd.DepthOrArraySize = 1;
        bd.MipLevels        = 1;
        bd.Format           = DXGI_FORMAT_UNKNOWN;
        bd.SampleDesc.Count = 1;
        bd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        hr = ID3D12Device_CreateCommittedResource(g_d3d12.device, &hp, D3D12_HEAP_FLAG_NONE,
                &bd, D3D12_RESOURCE_STATE_COPY_DEST, NULL,
                &IID_ID3D12Resource, (void **)&g_d3d12.readback);
        if (FAILED(hr)) { WRAPPER_LOG("D3D12 capture: readback alloc 0x%08lX", hr); return NULL; }
        g_d3d12.readback_size = total;
    }

    /* Record BB -> readback copy on the open frame list, then flush. */
    d3d12_resource_barrier(bb, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    {
        D3D12_TEXTURE_COPY_LOCATION dst, src;
        ZeroMemory(&dst, sizeof(dst));
        dst.pResource       = g_d3d12.readback;
        dst.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = fp;
        ZeroMemory(&src, sizeof(src));
        src.pResource        = bb;
        src.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;
        ID3D12GraphicsCommandList_CopyTextureRegion(g_d3d12.list, &dst, 0, 0, 0, &src, NULL);
    }
    d3d12_resource_barrier(bb, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);

    ID3D12GraphicsCommandList_Close(g_d3d12.list);
    {
        ID3D12CommandList *lists[1];
        lists[0] = (ID3D12CommandList *)g_d3d12.list;
        ID3D12CommandQueue_ExecuteCommandLists(g_d3d12.queue, 1, lists);
    }
    g_d3d12.frame_open = 0;
    d3d12_wait_idle();

    /* Map + convert BGRA -> RGBA row by row (source pitch is 256-aligned). */
    {
        D3D12_RANGE rr; rr.Begin = 0; rr.End = (SIZE_T)total;
        hr = ID3D12Resource_Map(g_d3d12.readback, 0, &rr, (void **)&mapped);
        if (FAILED(hr) || !mapped) { WRAPPER_LOG("D3D12 capture: Map 0x%08lX", hr); return NULL; }
    }
    out = (unsigned char *)malloc((size_t)w * h * 4);
    if (!out) { ID3D12Resource_Unmap(g_d3d12.readback, 0, NULL); return NULL; }
    for (y = 0; y < h; y++) {
        const unsigned char *srow = mapped + (size_t)y * fp.Footprint.RowPitch;
        unsigned char       *drow = out   + (size_t)y * w * 4;
        for (x = 0; x < w; x++) {
            drow[x*4+0] = srow[x*4+2];  /* R <- B */
            drow[x*4+1] = srow[x*4+1];  /* G      */
            drow[x*4+2] = srow[x*4+0];  /* B <- R */
            drow[x*4+3] = srow[x*4+3];  /* A      */
        }
    }
    { D3D12_RANGE wr; wr.Begin = 0; wr.End = 0; ID3D12Resource_Unmap(g_d3d12.readback, 0, &wr); }

    if (out_w) *out_w = (int)w;
    if (out_h) *out_h = (int)h;
    return out;
}

/* ======================================================================== *
 *  STUBS -- not yet implemented on D3D12 (fill in Phases 2-4). They let the
 *  library link so the window/clear/present skeleton runs.
 * ======================================================================== */

void Backend_ApplyLightPass(const LightCB *cb) { (void)cb; }
void Backend_ApplySSRPass(const SSRCB *cb) { (void)cb; }
void Backend_ApplyShadowPass(const ShadowCB *cb) { (void)cb; }
void Backend_ApplyStateCache(void) { }
/* b1 (SDF/FX) is the only game-bound CB slot; b0 VS/PS are backend-owned
 * (viewport/fog) and bound directly by the draw path. */
void Backend_BindConstBuffer(UINT slot, BackendConstBuffer *cb) { if (slot == 1) s_cur_cb1 = cb; }
void Backend_BindSampler(UINT slot, int sampler_idx) { (void)slot; (void)sampler_idx; }
int  Backend_BindSceneDepthReadonly(void) { return 0; }
void Backend_CaptureIfRequested(void) { }
BackendConstBuffer *Backend_CreateConstBuffer(size_t size) { return d3d12_cb_create((UINT)size); }

/* Copy the bytecode: PSOs referencing it are built lazily, so it must outlive
 * the caller's buffer. */
BackendPixelShader *Backend_CreatePixelShader(const void *bytecode, size_t len)
{
    BackendPixelShader *ps;
    void *copy;
    if (!bytecode || !len) return NULL;
    ps = (BackendPixelShader *)calloc(1, sizeof(*ps));
    if (!ps) return NULL;
    copy = malloc(len);
    if (!copy) { free(ps); return NULL; }
    memcpy(copy, bytecode, len);
    ps->bc = copy; ps->len = (SIZE_T)len;
    return ps;
}
/* Drain the D3D12 debug-layer InfoQueue to a flushed log file. This is the
 * interim headless verification net during render-core bring-up: validation
 * errors (bad barriers, root-sig/PSO mismatches, ...) land in log/d3d12_debug.log
 * immediately (fflush), surviving a force-kill unlike the buffered engine log. */
void Backend_DrainD3DDebug(const char *where)
{
    UINT64 n, i;
    FILE *f;
    if (!g_d3d12.info_queue) return;
    n = ID3D12InfoQueue_GetNumStoredMessages(g_d3d12.info_queue);
    if (!n) return;
    f = fopen("log/d3d12_debug.log", "a");
    for (i = 0; i < n; i++) {
        SIZE_T len = 0;
        D3D12_MESSAGE *m;
        ID3D12InfoQueue_GetMessage(g_d3d12.info_queue, i, NULL, &len);
        if (!len) continue;
        m = (D3D12_MESSAGE *)malloc(len);
        if (m && SUCCEEDED(ID3D12InfoQueue_GetMessage(g_d3d12.info_queue, i, m, &len)) && f) {
            fprintf(f, "[%s][sev=%d id=%d] %.*s\n", where ? where : "-",
                    (int)m->Severity, (int)m->ID,
                    (int)m->DescriptionByteLength, m->pDescription ? m->pDescription : "");
        }
        free(m);
    }
    if (f) { fflush(f); fclose(f); }
    ID3D12InfoQueue_ClearStoredMessages(g_d3d12.info_queue);
}
void Backend_DrawFullscreenQuad(ID3D11ShaderResourceView *srv) { (void)srv; }
void Backend_DrawFullscreenQuadRaw(void *srv) { (void)srv; }
void Backend_DrawIndexedPrimitive(DWORD p, UINT s, UINT bv, UINT si, UINT ic, UINT vc) { (void)p;(void)s;(void)bv;(void)si;(void)ic;(void)vc; }
void Backend_DrawPrimitive(DWORD p, UINT s, UINT bv, UINT vc) { (void)p;(void)s;(void)bv;(void)vc; }
void Backend_DumpCrashDiag(const char *path) { (void)path; }
void Backend_EnforceWindowSize(void) { }
void Backend_EnsureCompositingTextures(int w, int h) { (void)w; (void)h; }
void Backend_ForceBlendState(int blend_idx) { (void)blend_idx; }
int  Backend_GetCapture(unsigned char **px, int *w, int *h) { (void)px;(void)w;(void)h; return 0; }
void Backend_MaybeTrim(void) { }
void Backend_NoteDraw(unsigned prim, unsigned vc, unsigned ic, int indexed) { (void)prim;(void)vc;(void)ic;(void)indexed; }
void Backend_NoteVerts(const void *v, unsigned vc, unsigned s) { (void)v;(void)vc;(void)s; }
void *Backend_PixelShaderRaw(BackendPixelShader *ps) { return ps; }
void Backend_PlatBindTextureSRV(WrapperRecCtx *rc, void *srv) { (void)rc;(void)srv; }
void Backend_PlatDrawTris(WrapperRecCtx *rc, const void *v, int vc, const void *idx, int ic, void *pso, int ss) { (void)rc;(void)v;(void)vc;(void)idx;(void)ic;(void)pso;(void)ss; }
void Backend_PlatDrawWhite(WrapperRecCtx *rc, const void *v, int vc, const void *idx, int ic, int lines) { (void)rc;(void)v;(void)vc;(void)idx;(void)ic;(void)lines; }
void Backend_PlatSetScissor(WrapperRecCtx *rc, int l, int t, int r, int b) { (void)rc;(void)l;(void)t;(void)r;(void)b; }
void Backend_PlatSetViewport(WrapperRecCtx *rc, int x, int y, int w, int h) { (void)rc;(void)x;(void)y;(void)w;(void)h; }
WrapperRecCtx *Backend_RecBegin(int i, int x, int y, int w, int h) { (void)i;(void)x;(void)y;(void)w;(void)h; return NULL; }
void Backend_RecEnd(WrapperRecCtx *rc) { (void)rc; }
void Backend_RecExecute(int i) { (void)i; }
int  Backend_RecPoolEnsure(int c) { (void)c; return 0; }
void Backend_RecPoolRelease(void) { }
int  Backend_RecreateDevice(void) { return 0; }
void Backend_ReleaseConstBuffer(BackendConstBuffer *cb) { if (cb) { if (cb->res) ID3D12Resource_Release(cb->res); free(cb); } }
void Backend_ReleasePixelShader(BackendPixelShader *ps) { if (ps) { free((void *)ps->bc); free(ps); } }
void Backend_RequestCapture(void) { }
int  Backend_Reset(int w, int h, int bpp, int windowed) { (void)w;(void)h;(void)bpp;(void)windowed; return 1; }
void Backend_RestoreMainRenderTarget(void) { }
void Backend_SelectPixelShader(void) { }  /* PS chosen at draw time from g_backend.state */
void Backend_SetBuiltinPixelShader(int ps_idx) { if (ps_idx >= 0 && ps_idx < PS_COUNT) s_cur_ps = ps_idx; }
int  Backend_SetExclusiveFullscreen(int enable) { (void)enable; return 1; }
void Backend_SetGBufferEnabled(int on) { (void)on; }
void Backend_SetViewport(float x, float y, float w, float h, float mn, float mx) { (void)x;(void)y;(void)w;(void)h;(void)mn;(void)mx; }
int  Backend_StreamUpload(const void *v, UINT vc, UINT s, const void *idx, UINT ic, UINT *obv, UINT *osi) { (void)v;(void)vc;(void)s;(void)idx;(void)ic; if(obv)*obv=0; if(osi)*osi=0; return 0; }
void Backend_SurfaceBindRenderTarget(WrapperSurface *s) { (void)s; }
ID3D11ShaderResourceView *Backend_SurfaceGetSRV(WrapperSurface *s) { (void)s; return NULL; }
int  Backend_SurfaceHasRTV(WrapperSurface *s) { (void)s; return 0; }
void Backend_TextureAddRef(BackendTexture *bt) { (void)bt; }
BackendTexture *Backend_TextureAdopt(void *n) { (void)n; return NULL; }
int  Backend_TextureBind(BackendTexture *bt, UINT stage) { (void)bt;(void)stage; return 0; }
void Backend_TextureBindRenderTarget(BackendTexture *bt) { (void)bt; }
void Backend_TextureClearRT(BackendTexture *bt, const float *rgba) { (void)bt;(void)rgba; }
BackendTexture *Backend_TextureCreate(DWORD w, DWORD h, DXGI_FORMAT f, int rt, int st) { (void)w;(void)h;(void)f;(void)rt;(void)st; return NULL; }
void Backend_TextureEnsureCurrent(BackendTexture *bt, DWORD w, DWORD h, DXGI_FORMAT f, int rt, int st) { (void)bt;(void)w;(void)h;(void)f;(void)rt;(void)st; }
BackendTexture *Backend_TextureFromBGRA(const void *px, int w, int h) { (void)px;(void)w;(void)h; return NULL; }
int  Backend_TextureHasRTV(const BackendTexture *bt) { (void)bt; return 0; }
int  Backend_TextureIsValid(const BackendTexture *bt) { (void)bt; return 0; }
int  Backend_TextureLoad(BackendTexture **pbt, DWORD dw, DWORD dh, DXGI_FORMAT df, const void *sp, LONG spitch, DWORD sw, DWORD sh, DWORD sbpp, int sha, int hck, DWORD ck, BackendTexture *sbt, int *r5) { (void)pbt;(void)dw;(void)dh;(void)df;(void)sp;(void)spitch;(void)sw;(void)sh;(void)sbpp;(void)sha;(void)hck;(void)ck;(void)sbt; if(r5)*r5=0; return 0; }
void Backend_TextureRelease(BackendTexture *bt) { (void)bt; }
void Backend_TextureUpload(BackendTexture *bt, const void *sys, LONG p, DWORD w, DWORD h, DWORD bpp) { (void)bt;(void)sys;(void)p;(void)w;(void)h;(void)bpp; }
void Backend_UnbindRenderTargets(void) { }
void Backend_UnbindSceneDepthReadonly(void) { }
void Backend_UpdateConstBuffer(BackendConstBuffer *cb, const void *data, size_t size)
{
    if (cb && cb->mapped && data) memcpy(cb->mapped, data, size > cb->size ? cb->size : size);
}

/* Fold g_backend.state (populated by the shared device3.c) into the FogCB.
 * (foliageAA is set per-draw by the foliage path; left 0 here.) */
void Backend_UpdateFogCB(void)
{
    FogCB fog;
    const RenderStateCache *st = &g_backend.state;
    if (!s_fog_cb) return;
    ZeroMemory(&fog, sizeof(fog));
    fog.fogEnabled = st->fog_enable;
    if (st->fog_enable) {
        DWORD c = st->fog_color;
        fog.fogColor[0] = ((c >> 16) & 0xFF) / 255.0f;
        fog.fogColor[1] = ((c >>  8) & 0xFF) / 255.0f;
        fog.fogColor[2] = ( c        & 0xFF) / 255.0f;
        fog.fogStart = st->fog_start; fog.fogEnd = st->fog_end; fog.fogDensity = st->fog_density;
    }
    fog.alphaTestEnabled = st->alpha_test_enable;
    fog.alphaRef = (float)st->alpha_ref / 255.0f;
    memcpy(s_fog_cb->mapped, &fog, sizeof(fog));
}

void Backend_UpdateViewportCB(float w, float h)
{
    ViewportCB vp;
    if (!s_viewport_cb) return;
    ZeroMemory(&vp, sizeof(vp));
    vp.viewportWidth = w; vp.viewportHeight = h;
    memcpy(s_viewport_cb->mapped, &vp, sizeof(vp));
}
