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
#include "shaders/ps_composite_bytes_50.h"     /* present-time fullscreen blit */
#include "shaders/ps_shadow_bytes_50.h"        /* screen-space sun-shadow pass  */
#include "shaders/ps_light_bytes_50.h"         /* deferred dynamic-light pass   */
#include "shaders/ps_ssr_bytes_50.h"           /* screen-space reflections pass */

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
    int         id;    /* >= PS_COUNT: index into s_custom_ps for PSO cache keys */
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

static ID3D12DescriptorHeap *s_dsv_heap;         /* 2 DSVs: [0] writable, [1] read-only */
static UINT                  s_dsv_size;          /* DSV descriptor increment size */
static ID3D12Resource       *s_depth_tex;        /* R32_TYPELESS: DSV (D32) + SRV (R32) */
static UINT                  s_depth_srv_slot;    /* staging-heap slot of the R32_FLOAT depth SRV */
static D3D12_RESOURCE_STATES s_depth_state = D3D12_RESOURCE_STATE_DEPTH_WRITE;

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

/* Custom pixel shaders (SDF/vector UI, from Backend_CreatePixelShader), keyed by
 * id = PS_COUNT + index so d3d12_get_pso can cache their PSOs. */
#define D3D12_CUSTOM_PS_MAX 48
static BackendPixelShader *s_custom_ps[D3D12_CUSTOM_PS_MAX];
static int                 s_custom_ps_count;

/* Persistent viewport + fog constant buffers (b0 VS / b0 PS). */
static BackendConstBuffer *s_viewport_cb;
static BackendConstBuffer *s_fog_cb;

/* Present-time fullscreen blit PSO (vs_fullscreen + ps_composite, no input
 * layout -- SV_VertexID triangle). Composites a surface texture to the swapchain. */
static ID3D12PipelineState *s_fsquad_pso;

/* Offscreen screen-space passes (shadow/light/SSR): their own root signature
 * (root CBV b0 + SRV table t0-t2 + static point-clamp sampler s0) and a tiny
 * PSO cache keyed by {PS bytecode, blend}. All are vs_fullscreen SV_VertexID
 * triangles rendering color-only (no DS) onto the swapchain with a pass blend. */
static ID3D12RootSignature *s_pass_root_sig;
typedef struct { const void *ps; int blend; ID3D12PipelineState *pso; } PassPSO;
static PassPSO s_pass_pso[8];
static int     s_pass_pso_count;
static BackendTexture *s_scene_copy;   /* SSR: CopyResource of the scene color (t2) */

/* Current draw state (selected into the next PSO / bindings). */
static int              s_cur_ps    = PS_MODULATE;
static int              s_cur_vs    = 0;              /* 0 = pretransformed      */
static BackendTexture  *s_cur_tex;
static BackendConstBuffer *s_cur_cb1;                 /* b1 SDF/FX (per-draw)    */
static D3D12_VIEWPORT   s_cur_vp;
static D3D12_RECT       s_cur_scissor;

/* Dedicated one-shot copy list (texture uploads outside the frame list). */
static ID3D12CommandAllocator    *s_copy_alloc;
static ID3D12GraphicsCommandList *s_copy_list;

/* 1x1 white texture: default SRV (stage slot 0) for untextured draws
 * (PS_MODULATE * white == vertex colour). */
static BackendTexture  *s_white_tex;
static BackendTexture  *s_black_tex;   /* 1x1 zero: gbuffer/scene-copy placeholder for passes */

/* forward decls (definitions below the device lifecycle) */
static D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle(ID3D12DescriptorHeap *h, UINT idx, UINT size);
static D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle(ID3D12DescriptorHeap *h, UINT idx, UINT size);
static void d3d12_flush_uploads(void);
static void d3d12_diag(const char *fmt, ...);

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

/* ---- frame forensics (crash/TDR post-mortem, ported from d3d11 backend) ----
 * A 256-deep draw ring + a 16-present history, dumped on a device-hung debug
 * event and appended to the SEH crash file. Pure CPU bookkeeping (no GPU state)
 * -> golden-safe. The per-vertex extent/NaN scan (NoteVerts) is the only costly
 * part, so on D3D12 it is gated on the debug flag; the cheap draw-ring +
 * per-present counters are always on. */
#define TD5_DRAW_RING 256
typedef struct {
    unsigned       vcount, icount;
    unsigned short prim;
    unsigned char  indexed;
    unsigned       gen;
    const void    *srv;
} TD5DrawRec;
static TD5DrawRec s_draw_ring[TD5_DRAW_RING];
static unsigned s_draw_head, s_draw_total, s_draw_max_icount, s_draw_max_vcount;

#define TD5_FRAME_HIST 16
typedef struct {
    unsigned draws, verts, present;
    float min_x, max_x, min_y, max_y;
    unsigned nan_verts;
} TD5FrameStat;
static TD5FrameStat s_frame_hist[TD5_FRAME_HIST];
static unsigned s_frame_hist_head;
static TD5FrameStat s_cur_frame = { 0, 0, 0, 1e30f, -1e30f, 1e30f, -1e30f, 0 };
#define TD5_PRESENT_SENTINEL 0xFFFFu

void Backend_NoteVerts(const void *verts, unsigned vert_count, unsigned stride)
{
    const unsigned char *p = (const unsigned char *)verts;
    unsigned i;
    /* Costly (scans every vertex) -> only when hunting a TDR. */
    if (!Backend_D3DDebugEnabled() || !p || stride < 8) return;
    for (i = 0; i < vert_count; i++, p += stride) {
        float x = ((const float *)(const void *)p)[0];
        float y = ((const float *)(const void *)p)[1];
        if (x != x || y != y) { s_cur_frame.nan_verts++; continue; }
        if (x < s_cur_frame.min_x) s_cur_frame.min_x = x;
        if (x > s_cur_frame.max_x) s_cur_frame.max_x = x;
        if (y < s_cur_frame.min_y) s_cur_frame.min_y = y;
        if (y > s_cur_frame.max_y) s_cur_frame.max_y = y;
    }
}

void Backend_NoteDraw(unsigned prim, unsigned vcount, unsigned icount, int indexed)
{
    TD5DrawRec *r = &s_draw_ring[s_draw_head % TD5_DRAW_RING];
    s_cur_frame.draws++;
    s_cur_frame.verts += vcount;
    r->vcount = vcount; r->icount = icount;
    r->prim = (unsigned short)prim; r->indexed = (unsigned char)indexed;
    r->gen = g_backend.device_generation;
    r->srv = (const void *)s_cur_tex;
    s_draw_head++; s_draw_total++;
    if (icount > s_draw_max_icount) s_draw_max_icount = icount;
    if (vcount > s_draw_max_vcount) s_draw_max_vcount = vcount;
}

void Backend_NotePresent(void)
{
    TD5DrawRec *r = &s_draw_ring[s_draw_head % TD5_DRAW_RING];
    r->vcount  = (unsigned)g_backend.present_count;
    r->icount  = 0;
    r->prim    = TD5_PRESENT_SENTINEL;
    r->indexed = 0;
    r->gen     = g_backend.device_generation;
    r->srv     = (const void *)s_cur_tex;
    s_draw_head++; s_draw_total++;

    s_cur_frame.present = (unsigned)g_backend.present_count;
    s_frame_hist[s_frame_hist_head++ % TD5_FRAME_HIST] = s_cur_frame;
    s_cur_frame.draws = 0; s_cur_frame.verts = 0; s_cur_frame.present = 0;
    s_cur_frame.min_x = 1e30f; s_cur_frame.max_x = -1e30f;
    s_cur_frame.min_y = 1e30f; s_cur_frame.max_y = -1e30f;
    s_cur_frame.nan_verts = 0;
}

/* Shared ring writer. `f` is an already-open stream; `tag` labels the dump. */
static void d3d12_write_draw_ring(FILE *f, const char *tag)
{
    unsigned n, i, cur = g_backend.device_generation;
    if (!f) return;
    fprintf(f, "==== DRAW WATCH (%s): total_draws=%u max_icount=%u max_vcount=%u cur_gen=%u (last %d) ====\n",
            tag ? tag : "?", s_draw_total, s_draw_max_icount, s_draw_max_vcount, cur, TD5_DRAW_RING);
    {
        unsigned hn = s_frame_hist_head < TD5_FRAME_HIST ? s_frame_hist_head : TD5_FRAME_HIST;
        unsigned hi;
        fprintf(f, "==== FRAME HISTORY (oldest->newest; in-flight frame last) ====\n");
        for (hi = 0; hi < hn; hi++) {
            const TD5FrameStat *s = &s_frame_hist[(s_frame_hist_head - hn + hi) % TD5_FRAME_HIST];
            fprintf(f, "  present#%u draws=%u verts=%u x[%.0f..%.0f] y[%.0f..%.0f] nan=%u\n",
                    s->present, s->draws, s->verts, s->min_x, s->max_x, s->min_y, s->max_y, s->nan_verts);
        }
        fprintf(f, "  (in-flight) draws=%u verts=%u x[%.0f..%.0f] y[%.0f..%.0f] nan=%u\n",
                s_cur_frame.draws, s_cur_frame.verts, s_cur_frame.min_x, s_cur_frame.max_x,
                s_cur_frame.min_y, s_cur_frame.max_y, s_cur_frame.nan_verts);
    }
    n = s_draw_total < TD5_DRAW_RING ? s_draw_total : TD5_DRAW_RING;
    for (i = 0; i < n; i++) {
        unsigned idx = (s_draw_head - n + i) % TD5_DRAW_RING;
        TD5DrawRec *r = &s_draw_ring[idx];
        const char *stale = (r->gen < cur) ? "  <-- STALE (pre-reset resource)" : "";
        if (r->prim == TD5_PRESENT_SENTINEL)
            fprintf(f, "  [%u] PRESENT #%u gen=%u srv=%p%s\n", i, r->vcount, r->gen, r->srv, stale);
        else
            fprintf(f, "  [%u] %s prim=%u v=%u i=%u gen=%u srv=%p%s\n",
                    i, r->indexed ? "IDX" : "VTX", r->prim, r->vcount, r->icount, r->gen, r->srv, stale);
    }
}

int Backend_NoteDeviceRemoved(HRESULT hr, const char *where)
{
    if (FAILED(hr)) {
        g_backend.device_removed = 1;
        WRAPPER_LOG("D3D12 DEVICE REMOVED at %s: hr=0x%08lX", where ? where : "?", hr);
        /* Dump the draw ring + frame history for the TDR post-mortem (opt-in). */
        if (Backend_D3DDebugEnabled()) {
            FILE *f = fopen("log/gpu_d3d_debug.log", "a");
            if (f) {
                HRESULT rr = (g_d3d12.device ? ID3D12Device_GetDeviceRemovedReason(g_d3d12.device) : hr);
                fprintf(f, "==== D3D12 DEVICE REMOVED at %s hr=0x%08lX removed_reason=0x%08lX ====\n",
                        where ? where : "?", hr, rr);
                d3d12_write_draw_ring(f, "device-removed");
                fflush(f); fclose(f);
            }
        }
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

/* Single monotonic fence counter shared by present + copy/upload waits.
 * (The earlier per-slot ++fence_values[frame_index] scheme corrupted the values
 * when d3d12_wait_idle -- called by flush_uploads mid-frame -- and present both
 * bumped the same slot, hanging the 2nd present's wait_frame.) */
static UINT64 s_fence_val;

static void d3d12_wait_value(UINT64 v)
{
    if (ID3D12Fence_GetCompletedValue(g_d3d12.fence) < v) {
        ID3D12Fence_SetEventOnCompletion(g_d3d12.fence, v, g_d3d12.fence_event);
        WaitForSingleObject(g_d3d12.fence_event, INFINITE);
    }
}

/* Queue a signal of the next monotonic value and return it. */
static UINT64 d3d12_signal(void)
{
    UINT64 v = ++s_fence_val;
    ID3D12CommandQueue_Signal(g_d3d12.queue, g_d3d12.fence, v);
    return v;
}

static void d3d12_wait_idle(void)
{
    if (!g_d3d12.queue || !g_d3d12.fence) return;
    d3d12_wait_value(d3d12_signal());
}

/* ---- deferred deletion queue ------------------------------------------------
 * D3D11 releases resources immediately; on D3D12 a resource may still be
 * referenced by a command list that is queued but not yet complete on the GPU,
 * so freeing it now is a GPU use-after-free (corruption / device-removed).
 * Instead, any RUNTIME release routes here with the fence value the current
 * in-flight frame will signal at its next present (s_fence_val + 1, since
 * d3d12_signal pre-increments); the resource is Release()d only once the GPU
 * has passed that value. Drained every frame. This is also the foundation for
 * the Phase 5 recreation-leak fix (everything lands on one list). */
typedef struct { IUnknown *res; UINT64 fence; } D3D12Retired;
static D3D12Retired *s_retire;
static int s_retire_count, s_retire_cap;

/* Retire a resource to be freed once the GPU passes the given fence value. */
static void d3d12_retire_at(void *res, UINT64 fence)
{
    if (!res) return;
    /* No device/queue yet (early teardown) -> release immediately. */
    if (!g_d3d12.fence) { IUnknown_Release((IUnknown *)res); return; }
    if (s_retire_count == s_retire_cap) {
        int nc = s_retire_cap ? s_retire_cap * 2 : 64;
        D3D12Retired *n = (D3D12Retired *)realloc(s_retire, (size_t)nc * sizeof(*n));
        if (!n) { IUnknown_Release((IUnknown *)res); return; }  /* OOM: free now (leak-safe) */
        s_retire = n; s_retire_cap = nc;
    }
    s_retire[s_retire_count].res   = (IUnknown *)res;
    s_retire[s_retire_count].fence = fence;
    s_retire_count++;
}

/* Default: retire behind the value the current in-flight frame will signal at
 * its next present (s_fence_val + 1, since d3d12_signal pre-increments). */
static void d3d12_retire(void *res) { d3d12_retire_at(res, s_fence_val + 1); }

/* Free everything the GPU has finished with. Call once per frame. */
static void d3d12_drain_retired(void)
{
    UINT64 done;
    int i = 0;
    if (!g_d3d12.fence || s_retire_count == 0) return;
    done = ID3D12Fence_GetCompletedValue(g_d3d12.fence);
    while (i < s_retire_count) {
        if (s_retire[i].fence <= done) {
            IUnknown_Release(s_retire[i].res);
            s_retire[i] = s_retire[--s_retire_count];   /* swap-remove */
        } else {
            i++;
        }
    }
}

/* Force-drain at shutdown/device-loss: the caller has already waited idle, so
 * every fence is satisfied -- release the lot unconditionally. */
static void d3d12_flush_retired(void)
{
    int i;
    for (i = 0; i < s_retire_count; i++) IUnknown_Release(s_retire[i].res);
    s_retire_count = 0;
    free(s_retire); s_retire = NULL; s_retire_cap = 0;
}

/* Per-command-list binding cache (D3D11 ApplyStateCache equivalent): the
 * descriptor heaps / root signature / frame-CBVs are invariant across a frame's
 * draws, and PSO/topology usually repeat -- setting them every draw was pure
 * per-draw CPU overhead. Bind the invariants once and skip redundant switches.
 * Reset by d3d12_frame_begin (fresh command list). */
static int                    s_frame_static_bound;   /* heaps+rootsig+b0/b1 CBVs set this frame */
static ID3D12PipelineState   *s_last_pso;
static D3D_PRIMITIVE_TOPOLOGY  s_last_topo = (D3D_PRIMITIVE_TOPOLOGY)-1;

/* Open the command list for the current backbuffer and bind it as the RT. */
static void d3d12_frame_begin(void)
{
    UINT idx;
    if (g_d3d12.frame_open || !g_d3d12.device) return;
    idx = g_d3d12.frame_index;

    ID3D12CommandAllocator_Reset(g_d3d12.allocators[idx]);
    ID3D12GraphicsCommandList_Reset(g_d3d12.list, g_d3d12.allocators[idx], NULL);
    s_upload_off[idx] = 0;   /* recycle this slot's upload ring (GPU done via fence) */

    d3d12_resource_barrier(g_d3d12.backbuffers[idx],
        D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);

    {
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = d3d12_rtv_handle(idx);
        D3D12_CPU_DESCRIPTOR_HANDLE dsv = cpu_handle(s_dsv_heap, 0, s_dsv_size);
        static const float s_black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        ID3D12GraphicsCommandList_OMSetRenderTargets(g_d3d12.list, 1, &rtv, FALSE, s_dsv_heap ? &dsv : NULL);
        /* Flip-discard leaves the backbuffer undefined each frame; clear it so
         * the port's Rec/Plat draws land on a clean slate (the game does not
         * always issue Backend_ClearBackbuffer per frame). */
        ID3D12GraphicsCommandList_ClearRenderTargetView(g_d3d12.list, rtv, s_black, 0, NULL);
        if (s_dsv_heap)
            ID3D12GraphicsCommandList_ClearDepthStencilView(g_d3d12.list, dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, NULL);
    }
    /* Default full-RT viewport + scissor (overridden per pane by SetViewport). */
    s_cur_vp.TopLeftX = 0.0f; s_cur_vp.TopLeftY = 0.0f;
    s_cur_vp.Width = (float)g_backend.width; s_cur_vp.Height = (float)g_backend.height;
    s_cur_vp.MinDepth = 0.0f; s_cur_vp.MaxDepth = 1.0f;
    s_cur_scissor.left = 0; s_cur_scissor.top = 0;
    s_cur_scissor.right = g_backend.width; s_cur_scissor.bottom = g_backend.height;
    /* Fresh command list -> invalidate the per-draw binding cache. */
    s_frame_static_bound = 0;
    s_last_pso  = NULL;
    s_last_topo = (D3D_PRIMITIVE_TOPOLOGY)-1;
    g_d3d12.frame_open = 1;
}

/* ---- framedump capture (must happen BEFORE the flip; flip-discard recycles
 *      the presented buffer, so a post-Present readback is undefined) -------- */
static int            s_cap_on = -1;
static unsigned char *s_cap_buf;
static UINT           s_cap_w, s_cap_h;
static D3D12_PLACED_SUBRESOURCE_FOOTPRINT *s_cap_pending_fp;
static unsigned       s_dbg_draws, s_dbg_clears;   /* bring-up instrumentation */

static int d3d12_capture_active(void)
{
    if (s_cap_on < 0) {
        const char *e = getenv("TD5RE_FRAMEDUMP");
        const char *r = getenv("TD5RE_D3D12_CAPTURE");
        s_cap_on = ((e && e[0]) || (r && r[0])) ? 1 : 0;
    }
    return s_cap_on;
}

/* Record backbuffer[idx] (in RENDER_TARGET) -> readback + leave it in PRESENT,
 * on the still-open frame list. Fills fp; returns 1 if recorded. */
static int d3d12_record_capture(UINT idx, D3D12_PLACED_SUBRESOURCE_FOOTPRINT *fp)
{
    ID3D12Resource *bb = g_d3d12.backbuffers[idx];
    D3D12_RESOURCE_DESC rd;
    UINT num_rows = 0; UINT64 row_bytes = 0, total = 0;
    D3D12_TEXTURE_COPY_LOCATION dst, src;
    D3D12_RESOURCE_BARRIER b;

    bb->lpVtbl->GetDesc(bb, &rd);
    ID3D12Device_GetCopyableFootprints(g_d3d12.device, &rd, 0, 1, 0, fp, &num_rows, &row_bytes, &total);
    if (!g_d3d12.readback || g_d3d12.readback_size < total) {
        D3D12_HEAP_PROPERTIES hp; D3D12_RESOURCE_DESC bd;
        if (g_d3d12.readback) { ID3D12Resource_Release(g_d3d12.readback); g_d3d12.readback = NULL; }
        ZeroMemory(&hp, sizeof(hp)); hp.Type = D3D12_HEAP_TYPE_READBACK;
        ZeroMemory(&bd, sizeof(bd));
        bd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width=total; bd.Height=1; bd.DepthOrArraySize=1;
        bd.MipLevels=1; bd.Format=DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count=1; bd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(ID3D12Device_CreateCommittedResource(g_d3d12.device, &hp, D3D12_HEAP_FLAG_NONE, &bd,
                D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource, (void **)&g_d3d12.readback)))
            return 0;
        g_d3d12.readback_size = total;
    }
    ZeroMemory(&b, sizeof(b)); b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource=bb; b.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore=D3D12_RESOURCE_STATE_RENDER_TARGET; b.Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE;
    ID3D12GraphicsCommandList_ResourceBarrier(g_d3d12.list, 1, &b);
    ZeroMemory(&dst, sizeof(dst)); dst.pResource=g_d3d12.readback; dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint=*fp;
    ZeroMemory(&src, sizeof(src)); src.pResource=bb; src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; src.SubresourceIndex=0;
    ID3D12GraphicsCommandList_CopyTextureRegion(g_d3d12.list, &dst, 0, 0, 0, &src, NULL);
    b.Transition.StateBefore=D3D12_RESOURCE_STATE_COPY_SOURCE; b.Transition.StateAfter=D3D12_RESOURCE_STATE_PRESENT;
    ID3D12GraphicsCommandList_ResourceBarrier(g_d3d12.list, 1, &b);
    return 1;
}

/* Map the readback (copy already GPU-complete) + convert BGRA->RGBA into s_cap_buf. */
static void d3d12_store_capture(const D3D12_PLACED_SUBRESOURCE_FOOTPRINT *fp)
{
    unsigned char *mapped = NULL;
    UINT w = fp->Footprint.Width, h = fp->Footprint.Height, y, x;
    D3D12_RANGE rr; rr.Begin = 0; rr.End = (SIZE_T)fp->Footprint.RowPitch * h;
    if (FAILED(ID3D12Resource_Map(g_d3d12.readback, 0, &rr, (void **)&mapped)) || !mapped) return;
    if (s_cap_w != w || s_cap_h != h) { free(s_cap_buf); s_cap_buf = (unsigned char *)malloc((size_t)w*h*4); s_cap_w=w; s_cap_h=h; }
    if (s_cap_buf) {
        for (y = 0; y < h; y++) {
            const unsigned char *s = mapped + (size_t)y * fp->Footprint.RowPitch;
            unsigned char *d = s_cap_buf + (size_t)y * w * 4;
            for (x = 0; x < w; x++) { d[x*4+0]=s[x*4+2]; d[x*4+1]=s[x*4+1]; d[x*4+2]=s[x*4+0]; d[x*4+3]=s[x*4+3]; }
        }
    }
    { D3D12_RANGE wr; wr.Begin=0; wr.End=0; ID3D12Resource_Unmap(g_d3d12.readback, 0, &wr); }
}

/* Close + execute the current frame and Present. */
static void d3d12_frame_present(int sync)
{
    UINT idx = g_d3d12.frame_index;
    HRESULT hr;
    static int s_pdiag = 0;
    int diag = (s_pdiag < 4);
    if (!g_d3d12.device || !g_d3d12.swapchain) return;
    if (diag) { d3d12_diag("present[%d] enter idx=%u open=%d", s_pdiag, idx, g_d3d12.frame_open); }

    if (!g_d3d12.frame_open) d3d12_frame_begin();  /* ensure something to present */

    /* Framedump capture (dev): copy this frame BEFORE the flip, else read it back
     * after present + wait. Otherwise plain RT->PRESENT. */
    {
        static D3D12_PLACED_SUBRESOURCE_FOOTPRINT s_fp;
        int cap = d3d12_capture_active() ? d3d12_record_capture(idx, &s_fp) : 0;
        if (!cap)
            d3d12_resource_barrier(g_d3d12.backbuffers[idx],
                D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        s_cap_pending_fp = cap ? &s_fp : NULL;
    }

    ID3D12GraphicsCommandList_Close(g_d3d12.list);
    {
        ID3D12CommandList *lists[1];
        lists[0] = (ID3D12CommandList *)g_d3d12.list;
        ID3D12CommandQueue_ExecuteCommandLists(g_d3d12.queue, 1, lists);
    }
    g_d3d12.frame_open = 0;

    Backend_NotePresent();
    if (diag) d3d12_diag("present[%d] pre-Present", s_pdiag);
    hr = IDXGISwapChain3_Present(g_d3d12.swapchain, sync ? 1 : 0, 0);
    g_backend.present_count++;
    if (FAILED(hr)) { Backend_NoteDeviceRemoved(hr, "d3d12_frame_present/Present"); return; }
    if (diag) d3d12_diag("present[%d] post-Present hr=0x%08lX", s_pdiag, hr);

    /* Record the fence value that marks end-of-work for the slot we just
     * submitted, advance to the next backbuffer, and wait for THAT slot's prior
     * frame to finish before we reuse its allocator/upload ring (2 in flight). */
    g_d3d12.fence_values[idx] = d3d12_signal();
    g_d3d12.frame_index = IDXGISwapChain3_GetCurrentBackBufferIndex(g_d3d12.swapchain);
    d3d12_wait_value(g_d3d12.fence_values[g_d3d12.frame_index]);

    /* Capture copy is now GPU-complete (waited on idx's fence above). */
    if (s_cap_pending_fp) { d3d12_wait_value(g_d3d12.fence_values[idx]); d3d12_store_capture(s_cap_pending_fp); s_cap_pending_fp = NULL; }
    /* Free resources whose referencing frame the GPU has now passed. */
    d3d12_drain_retired();
    if (diag) { d3d12_diag("present[%d] done, next idx=%u", s_pdiag, g_d3d12.frame_index); s_pdiag++; }
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
    if (ps_id >= 0 && ps_id < PS_COUNT) { *bc = s_builtin_ps[ps_id].bc; *len = s_builtin_ps[ps_id].len; return; }
    {
        int ci = ps_id - PS_COUNT;
        if (ci >= 0 && ci < s_custom_ps_count && s_custom_ps[ci]) {
            *bc = s_custom_ps[ci]->bc; *len = s_custom_ps[ci]->len; return;
        }
    }
    *bc = s_builtin_ps[PS_MODULATE].bc; *len = s_builtin_ps[PS_MODULATE].len;
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

/* --- render-state -> index selection (replicated from Backend_ApplyStateCache
 *     / Backend_SelectPixelShader; foliage-AA + G-buffer promotion deferred to
 *     Phase 4 -- the frontend uses neither). --- */

static int d3d12_sel_blend(const RenderStateCache *s)
{
    if (!s->blend_enable) return BLEND_OPAQUE;
    if (s->src_blend==D3D6BLEND_SRCALPHA    && s->dest_blend==D3D6BLEND_INVSRCALPHA) return BLEND_SRCALPHA_INVSRC;
    if (s->src_blend==D3D6BLEND_SRCALPHA    && s->dest_blend==D3D6BLEND_ONE)         return BLEND_SRCALPHA_ONE;
    if (s->src_blend==D3D6BLEND_ONE         && s->dest_blend==D3D6BLEND_ONE)         return BLEND_ONE_ONE;
    if (s->src_blend==D3D6BLEND_SRCALPHA    && s->dest_blend==D3D6BLEND_SRCALPHA)    return BLEND_SRCALPHA_SRCALPHA;
    if (s->src_blend==D3D6BLEND_INVSRCALPHA && s->dest_blend==D3D6BLEND_INVSRCALPHA) return BLEND_INVSRC_INVSRC;
    return BLEND_SRCALPHA_INVSRC;
}
static int d3d12_sel_ds(const RenderStateCache *s)
{
    if (!s->z_enable && !s->z_write) return DS_Z_OFF_WRITE_OFF;
    if (!s->z_enable &&  s->z_write) return DS_Z_OFF_WRITE_ON;
    if (s->z_func == 1) return s->z_write ? DS_Z_ON_WRITE_ON_ALWAYS : DS_Z_ON_WRITE_OFF_ALWAYS;
    if (s->z_enable && s->z_write) return DS_Z_ON_WRITE_ON;
    return DS_Z_ON_WRITE_OFF;
}
static int d3d12_sel_samp(const RenderStateCache *s)
{
    int linear = (s->mag_filter >= 2 || s->min_filter >= 2);
    int clamp  = (s->tex_address_u == 3 || s->tex_address_v == 3);
    if (linear && clamp) return SAMP_LINEAR_CLAMP;
    if (linear)          return SAMP_LINEAR_WRAP;
    if (clamp)           return SAMP_POINT_CLAMP;
    return SAMP_POINT_WRAP;
}
static int d3d12_sel_ps(const RenderStateCache *s)
{
    switch (s->texblend_mode) {
    case D3DTBLEND_DECAL:         return PS_DECAL;
    case D3DTBLEND_MODULATEALPHA: return PS_MODULATE_ALPHA;
    default:                      return PS_MODULATE;
    }
}
static D3D_PRIMITIVE_TOPOLOGY d3d12_map_topo(DWORD prim, int *topo_type)
{
    switch (prim) {
    case D3DPT_POINTLIST:     *topo_type = 2; return D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
    case D3DPT_LINELIST:      *topo_type = 1; return D3D_PRIMITIVE_TOPOLOGY_LINELIST;
    case D3DPT_LINESTRIP:     *topo_type = 1; return D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;
    case D3DPT_TRIANGLESTRIP: *topo_type = 0; return D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
    default:                  *topo_type = 0; return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    }
}

/* Bind the full pipeline for one draw from the current state + stream cursor,
 * then issue Draw/DrawIndexed. Everything is (re)bound each draw -- D3D12 keeps
 * no cross-call state and the cost is trivial at frontend/HUD draw counts. */
static void d3d12_bind_and_draw(D3D_PRIMITIVE_TOPOLOGY topo, int topo_type,
                                int blend, int ds, int raster, int ps, int samp,
                                UINT stride, int indexed, UINT base_vertex, UINT start_index, UINT count)
{
    ID3D12GraphicsCommandList *cl = g_d3d12.list;
    ID3D12PipelineState *pso;
    ID3D12DescriptorHeap *heaps[2];
    BackendTexture *tex = s_cur_tex ? s_cur_tex : s_white_tex;
    UINT slot, fi = g_d3d12.frame_index;
    D3D12_VERTEX_BUFFER_VIEW vbv;

    if (!g_d3d12.frame_open) d3d12_frame_begin();
    if (!tex) return;
    s_dbg_draws++;
    d3d12_flush_uploads();   /* make any pending texture uploads GPU-resident first */
    pso = d3d12_get_pso(s_cur_vs, ps, blend, ds, raster, topo_type);
    if (!pso) return;

    /* Invariant across the frame's draws -> bind once (heaps + root sig + the
     * b0/b1 CBVs, whose GPU-VA is fixed; their CONTENTS are updated in place via
     * the mapped upload CB). */
    if (!s_frame_static_bound) {
        heaps[0] = s_srv_ring; heaps[1] = s_sampler_heap;
        ID3D12GraphicsCommandList_SetDescriptorHeaps(cl, 2, heaps);
        ID3D12GraphicsCommandList_SetGraphicsRootSignature(cl, s_root_sig);
        ID3D12GraphicsCommandList_SetGraphicsRootConstantBufferView(cl, 0, ID3D12Resource_GetGPUVirtualAddress(s_viewport_cb->res));
        ID3D12GraphicsCommandList_SetGraphicsRootConstantBufferView(cl, 1, ID3D12Resource_GetGPUVirtualAddress(s_fog_cb->res));
        s_frame_static_bound = 1;
    }
    if (pso != s_last_pso) { ID3D12GraphicsCommandList_SetPipelineState(cl, pso); s_last_pso = pso; }
    if (s_cur_cb1 && s_cur_cb1->res)
        ID3D12GraphicsCommandList_SetGraphicsRootConstantBufferView(cl, 4, ID3D12Resource_GetGPUVirtualAddress(s_cur_cb1->res));

    /* SRV table: copy the bound texture's staging SRV into a fresh ring slot. */
    slot = s_srv_ring_next++;
    if (s_srv_ring_next >= s_srv_ring_cap) s_srv_ring_next = 0;
    ID3D12Device_CopyDescriptorsSimple(g_d3d12.device, 1,
        cpu_handle(s_srv_ring, slot, s_srv_stage_size),
        cpu_handle(s_srv_stage, tex->srv_slot, s_srv_stage_size),
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(cl, 2, gpu_handle(s_srv_ring, slot, s_srv_stage_size));
    ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(cl, 3, gpu_handle(s_sampler_heap, samp, s_sampler_size));

    if (topo != s_last_topo) { ID3D12GraphicsCommandList_IASetPrimitiveTopology(cl, topo); s_last_topo = topo; }
    vbv.BufferLocation = s_upload_gpu[fi]; vbv.SizeInBytes = D3D12_UPLOAD_RING_BYTES; vbv.StrideInBytes = stride;
    ID3D12GraphicsCommandList_IASetVertexBuffers(cl, 0, 1, &vbv);
    ID3D12GraphicsCommandList_RSSetViewports(cl, 1, &s_cur_vp);
    ID3D12GraphicsCommandList_RSSetScissorRects(cl, 1, &s_cur_scissor);

    if (indexed) {
        D3D12_INDEX_BUFFER_VIEW ibv;
        ibv.BufferLocation = s_upload_gpu[fi]; ibv.SizeInBytes = D3D12_UPLOAD_RING_BYTES; ibv.Format = DXGI_FORMAT_R16_UINT;
        ID3D12GraphicsCommandList_IASetIndexBuffer(cl, &ibv);
        ID3D12GraphicsCommandList_DrawIndexedInstanced(cl, count, 1, start_index, (INT)base_vertex, 0);
    } else {
        ID3D12GraphicsCommandList_DrawInstanced(cl, count, 1, base_vertex, 0);
    }
    /* Crash-ring bookkeeping (all draw paths funnel here). */
    Backend_NoteDraw((unsigned)topo_type, count, indexed ? count : 0, indexed);
}

/* Composite a source texture over the whole current RT (present-time blit).
 * `src` must be in PIXEL_SHADER_RESOURCE state (surfaces are, post-upload). */
static void d3d12_fullscreen_blit(BackendTexture *src)
{
    ID3D12GraphicsCommandList *cl = g_d3d12.list;
    ID3D12DescriptorHeap *heaps[2];
    D3D12_VIEWPORT vp;
    D3D12_RECT sc;
    UINT slot;

    if (!src || !src->res || !s_fsquad_pso) return;
    d3d12_flush_uploads();   /* the surface texture was just FlushDirty'd -> make it resident */

    /* The source may be a render-target surface left in RENDER_TARGET state;
     * it must be PIXEL_SHADER_RESOURCE to sample. Transition on the frame list. */
    if (src->rstate != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) {
        D3D12_RESOURCE_BARRIER tb;
        ZeroMemory(&tb, sizeof(tb));
        tb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        tb.Transition.pResource = src->res;
        tb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        tb.Transition.StateBefore = src->rstate;
        tb.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        ID3D12GraphicsCommandList_ResourceBarrier(cl, 1, &tb);
        src->rstate = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    }

    heaps[0] = s_srv_ring; heaps[1] = s_sampler_heap;
    ID3D12GraphicsCommandList_SetDescriptorHeaps(cl, 2, heaps);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(cl, s_root_sig);
    ID3D12GraphicsCommandList_SetPipelineState(cl, s_fsquad_pso);
    ID3D12GraphicsCommandList_SetGraphicsRootConstantBufferView(cl, 0, ID3D12Resource_GetGPUVirtualAddress(s_viewport_cb->res));
    ID3D12GraphicsCommandList_SetGraphicsRootConstantBufferView(cl, 1, ID3D12Resource_GetGPUVirtualAddress(s_fog_cb->res));

    slot = s_srv_ring_next++;
    if (s_srv_ring_next >= s_srv_ring_cap) s_srv_ring_next = 0;
    ID3D12Device_CopyDescriptorsSimple(g_d3d12.device, 1,
        cpu_handle(s_srv_ring, slot, s_srv_stage_size),
        cpu_handle(s_srv_stage, src->srv_slot, s_srv_stage_size),
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(cl, 2, gpu_handle(s_srv_ring, slot, s_srv_stage_size));
    ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(cl, 3, gpu_handle(s_sampler_heap, SAMP_POINT_CLAMP, s_sampler_size));

    vp.TopLeftX = 0.0f; vp.TopLeftY = 0.0f; vp.Width = (float)g_backend.width; vp.Height = (float)g_backend.height;
    vp.MinDepth = 0.0f; vp.MaxDepth = 1.0f;
    sc.left = 0; sc.top = 0; sc.right = g_backend.width; sc.bottom = g_backend.height;
    ID3D12GraphicsCommandList_RSSetViewports(cl, 1, &vp);
    ID3D12GraphicsCommandList_RSSetScissorRects(cl, 1, &sc);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(cl, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D12GraphicsCommandList_DrawInstanced(cl, 3, 1, 0, 0);
    /* This path set PSO + topology directly -> invalidate the bind cache so any
     * later bind_and_draw on this list rebinds them (heaps/rootsig/CBVs were
     * re-set identically above, so s_frame_static_bound stays valid). */
    s_last_pso  = s_fsquad_pso;
    s_last_topo = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
}

/* ---- offscreen screen-space passes (shadow/light/SSR) ------------------- */

static int d3d12_create_pass_root_sig(void)
{
    D3D12_DESCRIPTOR_RANGE srv_range;
    D3D12_ROOT_PARAMETER params[2];
    D3D12_STATIC_SAMPLER_DESC samp;
    D3D12_ROOT_SIGNATURE_DESC rsd;
    ID3D10Blob *sig = NULL, *err = NULL;
    HRESULT hr;

    ZeroMemory(&srv_range, sizeof(srv_range));
    srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srv_range.NumDescriptors = 3; srv_range.BaseShaderRegister = 0;   /* t0-t2 */
    srv_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    ZeroMemory(params, sizeof(params));
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;              /* b0 pass CB */
    params[0].Descriptor.ShaderRegister = 0;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; /* t0-t2 */
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &srv_range;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    ZeroMemory(&samp, sizeof(samp));
    samp.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    samp.MaxLOD = D3D12_FLOAT32_MAX;
    samp.ShaderRegister = 0; samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    ZeroMemory(&rsd, sizeof(rsd));
    rsd.NumParameters = 2; rsd.pParameters = params;
    rsd.NumStaticSamplers = 1; rsd.pStaticSamplers = &samp;
    rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_DENY_VERTEX_SHADER_ROOT_ACCESS;

    hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err);
    if (FAILED(hr)) {
        d3d12_diag("pass rootsig serialize 0x%08lX: %s", hr,
                   err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "(no blob)");
        if (err) ID3D10Blob_Release(err);
        return 0;
    }
    hr = ID3D12Device_CreateRootSignature(g_d3d12.device, 0,
            ID3D10Blob_GetBufferPointer(sig), ID3D10Blob_GetBufferSize(sig),
            &IID_ID3D12RootSignature, (void **)&s_pass_root_sig);
    ID3D10Blob_Release(sig);
    if (err) ID3D10Blob_Release(err);
    if (FAILED(hr)) { d3d12_diag("pass rootsig create 0x%08lX", hr); return 0; }
    d3d12_diag("pass root sig OK");
    return 1;
}

static ID3D12PipelineState *d3d12_get_pass_pso(const void *ps, SIZE_T ps_len, int blend)
{
    int i;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd;
    ID3D12PipelineState *pso = NULL;
    for (i = 0; i < s_pass_pso_count; i++)
        if (s_pass_pso[i].ps == ps && s_pass_pso[i].blend == blend) return s_pass_pso[i].pso;
    if (s_pass_pso_count >= (int)(sizeof(s_pass_pso)/sizeof(s_pass_pso[0]))) return NULL;
    ZeroMemory(&pd, sizeof(pd));
    pd.pRootSignature = s_pass_root_sig;
    pd.VS.pShaderBytecode = g_vs_fullscreen_50; pd.VS.BytecodeLength = sizeof(g_vs_fullscreen_50);
    pd.PS.pShaderBytecode = ps; pd.PS.BytecodeLength = ps_len;
    d3d12_fill_blend(blend, &pd.BlendState);
    d3d12_fill_ds(DS_Z_OFF_WRITE_OFF, &pd.DepthStencilState);
    d3d12_fill_raster(0, &pd.RasterizerState);
    pd.SampleMask = 0xFFFFFFFFu;
    pd.InputLayout.NumElements = 0;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1; pd.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM;
    pd.DSVFormat = DXGI_FORMAT_UNKNOWN;   /* color-only, no depth bound */
    pd.SampleDesc.Count = 1;
    if (FAILED(ID3D12Device_CreateGraphicsPipelineState(g_d3d12.device, &pd,
            &IID_ID3D12PipelineState, (void **)&pso))) {
        d3d12_diag("pass PSO create FAILED (blend=%d)", blend);
        return NULL;
    }
    s_pass_pso[s_pass_pso_count].ps = ps;
    s_pass_pso[s_pass_pso_count].blend = blend;
    s_pass_pso[s_pass_pso_count].pso = pso;
    s_pass_pso_count++;
    return pso;
}

/* Bump-allocate a 256-aligned constant-buffer slice from the current frame's
 * upload ring and copy `data` into it. Safe: the ring slot is recycled only
 * after its frame's fence completes. Returns the GPU VA (0 on overflow). */
static D3D12_GPU_VIRTUAL_ADDRESS d3d12_ring_cb(const void *data, UINT size)
{
    UINT fi = g_d3d12.frame_index, cur;
    UINT asz = (size + 255u) & ~255u;
    if (!s_upload_cpu[fi]) return 0;
    cur = (s_upload_off[fi] + 255u) & ~255u;
    if ((UINT64)cur + asz > D3D12_UPLOAD_RING_BYTES) { WRAPPER_LOG("D3D12 CB ring overflow"); return 0; }
    memcpy(s_upload_cpu[fi] + cur, data, size);
    s_upload_off[fi] = cur + asz;
    return s_upload_gpu[fi] + cur;
}

/* Run one full-screen deferred pass: sample scene depth (t0) + gbuffer (t1) +
 * scene-copy (t2) [placeholders where a resource doesn't exist yet], blend the
 * PS output onto the current swapchain RT. srv_slots[] are STAGING-heap slots;
 * missing ones are padded with the depth slot (shaders .Load, ignore). */
static void d3d12_fullscreen_pass(const void *ps, SIZE_T ps_len, int blend,
                                  const void *cbdata, UINT cbsize,
                                  const UINT *srv_slots, int nsrv)
{
    ID3D12GraphicsCommandList *cl = g_d3d12.list;
    ID3D12PipelineState *pso;
    ID3D12DescriptorHeap *heaps[2];
    D3D12_GPU_VIRTUAL_ADDRESS cbva;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv;
    UINT ring0, i, fi;
    if (!g_d3d12.device || !s_pass_root_sig || !s_depth_tex || g_backend.device_removed) return;
    if (!g_d3d12.frame_open) d3d12_frame_begin();
    d3d12_flush_uploads();
    pso = d3d12_get_pass_pso(ps, ps_len, blend);
    if (!pso) return;
    cbva = d3d12_ring_cb(cbdata, cbsize);
    if (!cbva) return;
    fi = g_d3d12.frame_index;

    /* Depth -> PIXEL_SHADER_RESOURCE so the pass can sample it. */
    if (s_depth_state != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) {
        d3d12_resource_barrier(s_depth_tex, s_depth_state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        s_depth_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    }

    /* Color-only RT (no DSV -- depth is a shader input this pass). */
    rtv = d3d12_rtv_handle(fi);
    ID3D12GraphicsCommandList_OMSetRenderTargets(cl, 1, &rtv, FALSE, NULL);

    heaps[0] = s_srv_ring; heaps[1] = s_sampler_heap;
    ID3D12GraphicsCommandList_SetDescriptorHeaps(cl, 2, heaps);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(cl, s_pass_root_sig);
    ID3D12GraphicsCommandList_SetPipelineState(cl, pso);
    ID3D12GraphicsCommandList_SetGraphicsRootConstantBufferView(cl, 0, cbva);

    /* Copy the 3 SRVs into contiguous ring slots (pad missing with depth). */
    ring0 = s_srv_ring_next;
    if (ring0 + 3 >= s_srv_ring_cap) ring0 = 0;
    for (i = 0; i < 3; i++) {
        /* Pad missing SRVs with the ZERO texture (matches d3d11's cleared
         * gbuffer/absent scene-copy), NOT depth -- depth as "normal/matid"
         * garbage makes the shadow shader over-darken. */
        UINT src = (i < (UINT)nsrv) ? srv_slots[i] : (s_black_tex ? s_black_tex->srv_slot : s_depth_srv_slot);
        ID3D12Device_CopyDescriptorsSimple(g_d3d12.device, 1,
            cpu_handle(s_srv_ring, ring0 + i, s_srv_stage_size),
            cpu_handle(s_srv_stage, src, s_srv_stage_size),
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }
    s_srv_ring_next = ring0 + 3;
    ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(cl, 1, gpu_handle(s_srv_ring, ring0, s_srv_stage_size));

    ID3D12GraphicsCommandList_RSSetViewports(cl, 1, &s_cur_vp);
    ID3D12GraphicsCommandList_RSSetScissorRects(cl, 1, &s_cur_scissor);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(cl, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D12GraphicsCommandList_DrawInstanced(cl, 3, 1, 0, 0);

    /* Restore: depth back to writable, RT with DSV for subsequent world/VFX. */
    if (s_depth_state != D3D12_RESOURCE_STATE_DEPTH_WRITE) {
        d3d12_resource_barrier(s_depth_tex, s_depth_state, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        s_depth_state = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    }
    {
        D3D12_CPU_DESCRIPTOR_HANDLE dsv = cpu_handle(s_dsv_heap, 0, s_dsv_size);
        ID3D12GraphicsCommandList_OMSetRenderTargets(cl, 1, &rtv, FALSE, &dsv);
    }
    /* We changed root sig + PSO + RT -> invalidate the main draw path's cache. */
    s_frame_static_bound = 0;
    s_last_pso  = NULL;
    s_last_topo = (D3D_PRIMITIVE_TOPOLOGY)-1;
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

/* ---- textures ---------------------------------------------------------- */

/* Batched texture uploader: record every texture copy on ONE persistent copy
 * list and keep its staging buffers alive; flush (execute + single wait + free
 * staging) is deferred to just-before-use (draw/blit) or a size threshold,
 * instead of a full GPU stall per texture -- otherwise frontend asset load,
 * which uploads hundreds of textures, crawls at <1 fps. */
static int              s_copy_open;
static ID3D12Resource **s_up_staging;
static int              s_up_count, s_up_cap;
static UINT64           s_up_bytes;
static UINT64           s_copy_fence;   /* fence value of the last submitted copy batch */

static void d3d12_copy_ensure(void)
{
    if (!s_copy_open) {
        /* Reusing the copy allocator requires its prior batch to be GPU-done.
         * We no longer wait_idle per flush (see below), so gate the reset on the
         * prior copy's fence -- almost always already passed (render work ran in
         * between), so this is a no-op stall in steady state. */
        if (s_copy_fence) d3d12_wait_value(s_copy_fence);
        ID3D12CommandAllocator_Reset(s_copy_alloc);
        ID3D12GraphicsCommandList_Reset(s_copy_list, s_copy_alloc, NULL);
        s_copy_open = 1;
    }
}
static void d3d12_flush_uploads(void)
{
    int i;
    ID3D12CommandList *lists[1];
    if (!s_copy_open) return;
    ID3D12GraphicsCommandList_Close(s_copy_list);
    lists[0] = (ID3D12CommandList *)s_copy_list;
    ID3D12CommandQueue_ExecuteCommandLists(g_d3d12.queue, 1, lists);
    /* The copy runs on the same DIRECT queue and is submitted BEFORE the render
     * list that reads the texture, so the copy is complete before any draw
     * sampling it -- no CPU wait_idle is needed for correctness (that full stall
     * per upload was the ~2x frame-time cost vs d3d11). The staging buffers only
     * need to outlive the copy: retire them behind the copy's fence and drain
     * them when the GPU passes it. */
    s_copy_fence = d3d12_signal();
    for (i = 0; i < s_up_count; i++)
        if (s_up_staging[i]) d3d12_retire_at(s_up_staging[i], s_copy_fence);
    s_up_count = 0; s_up_bytes = 0; s_copy_open = 0;
}
static void d3d12_up_track(ID3D12Resource *st, UINT64 bytes)
{
    if (s_up_count >= s_up_cap) {
        int nc = s_up_cap ? s_up_cap * 2 : 128;
        ID3D12Resource **n = (ID3D12Resource **)realloc(s_up_staging, (size_t)nc * sizeof(*n));
        if (!n) { ID3D12Resource_Release(st); return; }  /* drop (leak-safe) on OOM */
        s_up_staging = n; s_up_cap = nc;
    }
    s_up_staging[s_up_count++] = st; s_up_bytes += bytes;
}

/* Create a DEFAULT-heap 2D texture + its SRV in the staging heap (permanent
 * per-texture slot). rt=1 also allocates an RTV slot. Starts life in
 * PIXEL_SHADER_RESOURCE (or RENDER_TARGET) so the first bind needs no barrier. */
static BackendTexture *d3d12_tex_create(UINT w, UINT h, DXGI_FORMAT fmt, int rt)
{
    BackendTexture *bt;
    D3D12_HEAP_PROPERTIES hp;
    D3D12_RESOURCE_DESC   td;
    D3D12_SHADER_RESOURCE_VIEW_DESC srvd;
    D3D12_RESOURCE_STATES init_state;
    HRESULT hr;

    if (!g_d3d12.device || w == 0 || h == 0) return NULL;
    bt = (BackendTexture *)calloc(1, sizeof(*bt));
    if (!bt) return NULL;

    ZeroMemory(&hp, sizeof(hp)); hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    ZeroMemory(&td, sizeof(td));
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = w; td.Height = h; td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = fmt; td.SampleDesc.Count = 1;
    td.Flags = rt ? D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET : D3D12_RESOURCE_FLAG_NONE;
    init_state = rt ? D3D12_RESOURCE_STATE_RENDER_TARGET : D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    hr = ID3D12Device_CreateCommittedResource(g_d3d12.device, &hp, D3D12_HEAP_FLAG_NONE, &td,
            init_state, NULL, &IID_ID3D12Resource, (void **)&bt->res);
    if (FAILED(hr)) { free(bt); return NULL; }

    bt->fmt = fmt; bt->w = w; bt->h = h; bt->rstate = init_state;
    bt->has_rtv = rt; bt->valid = 1; bt->ref = 1; bt->gen = g_backend.device_generation;

    if (s_srv_stage_next >= s_srv_stage_cap) { WRAPPER_LOG("D3D12 SRV stage heap full"); s_srv_stage_next = 1; }
    bt->srv_slot = s_srv_stage_next++;
    ZeroMemory(&srvd, sizeof(srvd));
    srvd.Format = fmt; srvd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvd.Texture2D.MipLevels = 1;
    ID3D12Device_CreateShaderResourceView(g_d3d12.device, bt->res, &srvd,
        cpu_handle(s_srv_stage, bt->srv_slot, s_srv_stage_size));

    if (rt) {
        if (s_tex_rtv_next >= s_tex_rtv_cap) { WRAPPER_LOG("D3D12 tex RTV heap full"); s_tex_rtv_next = 0; }
        bt->rtv_slot = s_tex_rtv_next++;
        ID3D12Device_CreateRenderTargetView(g_d3d12.device, bt->res, NULL,
            cpu_handle(s_tex_rtv_heap, bt->rtv_slot, s_tex_rtv_size));
    }
    return bt;
}

/* Upload tightly/`src_pitch`-packed pixels into a DEFAULT texture via a staging
 * UPLOAD buffer + CopyTextureRegion on the one-shot copy list. */
static void d3d12_tex_upload(BackendTexture *bt, const void *px, UINT bytes_per_px, UINT src_pitch)
{
    D3D12_RESOURCE_DESC td;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
    UINT num_rows = 0; UINT64 row_bytes = 0, total = 0;
    ID3D12Resource *staging = NULL;
    D3D12_HEAP_PROPERTIES hp;
    D3D12_RESOURCE_DESC bd;
    unsigned char *map = NULL;
    UINT y;
    HRESULT hr;

    if (!bt || !bt->res || !px) return;
    bt->res->lpVtbl->GetDesc(bt->res, &td);
    ID3D12Device_GetCopyableFootprints(g_d3d12.device, &td, 0, 1, 0, &fp, &num_rows, &row_bytes, &total);

    ZeroMemory(&hp, sizeof(hp)); hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    ZeroMemory(&bd, sizeof(bd));
    bd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width=total; bd.Height=1; bd.DepthOrArraySize=1;
    bd.MipLevels=1; bd.Format=DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count=1; bd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    hr = ID3D12Device_CreateCommittedResource(g_d3d12.device, &hp, D3D12_HEAP_FLAG_NONE, &bd,
            D3D12_RESOURCE_STATE_GENERIC_READ, NULL, &IID_ID3D12Resource, (void **)&staging);
    if (FAILED(hr)) { WRAPPER_LOG("D3D12 tex staging alloc 0x%08lX", hr); return; }

    { D3D12_RANGE r; r.Begin=0; r.End=0; ID3D12Resource_Map(staging, 0, &r, (void **)&map); }
    if (map) {
        UINT copy = (UINT)(row_bytes < src_pitch ? row_bytes : src_pitch);
        for (y = 0; y < bt->h; y++)
            memcpy(map + (size_t)y * fp.Footprint.RowPitch,
                   (const unsigned char *)px + (size_t)y * src_pitch, copy);
        ID3D12Resource_Unmap(staging, 0, NULL);
    }
    (void)bytes_per_px;

    {
        ID3D12GraphicsCommandList *cl;
        D3D12_TEXTURE_COPY_LOCATION dst, src;
        D3D12_RESOURCE_BARRIER b;
        d3d12_copy_ensure();
        cl = s_copy_list;
        ZeroMemory(&dst, sizeof(dst)); dst.pResource=bt->res; dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dst.SubresourceIndex=0;
        ZeroMemory(&src, sizeof(src)); src.pResource=staging; src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; src.PlacedFootprint=fp;
        ZeroMemory(&b, sizeof(b)); b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource=bt->res; b.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore=bt->rstate; b.Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_DEST;
        ID3D12GraphicsCommandList_ResourceBarrier(cl, 1, &b);
        ID3D12GraphicsCommandList_CopyTextureRegion(cl, &dst, 0, 0, 0, &src, NULL);
        b.Transition.StateBefore=D3D12_RESOURCE_STATE_COPY_DEST; b.Transition.StateAfter=D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        ID3D12GraphicsCommandList_ResourceBarrier(cl, 1, &b);
        bt->rstate = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;   /* effective after flush */
        d3d12_up_track(staging, total);                            /* keep staging alive until flush */
    }
    /* Bound the batch so staging memory doesn't grow without limit during load. */
    if (s_up_count >= 128 || s_up_bytes > (96ull << 20)) d3d12_flush_uploads();
}

/* ---- render-core init / teardown --------------------------------------- */

/* Reuse *pbt if it already matches w/h/fmt (just re-upload); else create a fresh
 * texture, release the old, and swap it in. `pixels` are tightly packed in `fmt`
 * at `pitch` bytes/row (already converted -- raw upload, no format handling). */
static void d3d12_bt_recreate_from_init(BackendTexture **pbt, UINT w, UINT h,
                                        DXGI_FORMAT fmt, const void *pixels, UINT pitch)
{
    BackendTexture *dst = pbt ? *pbt : NULL;
    if (dst && dst->res && dst->w == w && dst->h == h && dst->fmt == fmt) {
        d3d12_tex_upload(dst, pixels, 4, pitch);   /* reuse: no descriptor-slot churn */
        return;
    }
    {
        BackendTexture *nbt = d3d12_tex_create(w, h, fmt, 0);
        if (!nbt) return;
        d3d12_tex_upload(nbt, pixels, 4, pitch);
        if (pbt) { if (dst) Backend_TextureRelease(dst); *pbt = nbt; }
    }
}

/* Flushed init-diagnostic sink (WRAPPER_LOG is not captured in the standalone
 * exe, so root-sig serialize errors etc. would otherwise be invisible). */
static void d3d12_diag(const char *fmt, ...)
{
    FILE *f = fopen("log/d3d12_init.log", "a");
    if (f) { va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap); fputc('\n', f); fflush(f); fclose(f); }
}

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
        d3d12_diag("SerializeRootSignature 0x%08lX: %s", hr,
                   err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "(no blob)");
        WRAPPER_LOG("D3D12 SerializeRootSignature 0x%08lX", hr);
        if (err) ID3D10Blob_Release(err);
        return 0;
    }
    hr = ID3D12Device_CreateRootSignature(g_d3d12.device, 0,
            ID3D10Blob_GetBufferPointer(sig), ID3D10Blob_GetBufferSize(sig),
            &IID_ID3D12RootSignature, (void **)&s_root_sig);
    ID3D10Blob_Release(sig);
    if (err) ID3D10Blob_Release(err);
    if (FAILED(hr)) { d3d12_diag("CreateRootSignature 0x%08lX", hr); return 0; }
    d3d12_diag("root sig OK");
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
    if (!d3d12_create_pass_root_sig()) return 0;

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

    /* Scene depth buffer: R32_TYPELESS so it can be both a depth target (D32
     * DSV) AND sampled as an SRV (R32_FLOAT) by the offscreen passes / soft
     * particles. Two DSVs: [0] writable, [1] read-only (bind alongside the depth
     * SRV for depth-aware effects without a write hazard). */
    {
        D3D12_HEAP_PROPERTIES hp;
        D3D12_RESOURCE_DESC   td;
        D3D12_CLEAR_VALUE     cv;
        D3D12_DEPTH_STENCIL_VIEW_DESC dvd;
        D3D12_SHADER_RESOURCE_VIEW_DESC srvd;
        ZeroMemory(&hp, sizeof(hp)); hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        ZeroMemory(&td, sizeof(td));
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = (UINT64)width; td.Height = (UINT)height; td.DepthOrArraySize = 1; td.MipLevels = 1;
        td.Format = DXGI_FORMAT_R32_TYPELESS; td.SampleDesc.Count = 1;
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;  /* SRV allowed (no DENY flag) */
        ZeroMemory(&cv, sizeof(cv)); cv.Format = DXGI_FORMAT_D32_FLOAT; cv.DepthStencil.Depth = 1.0f;
        hr = ID3D12Device_CreateCommittedResource(g_d3d12.device, &hp, D3D12_HEAP_FLAG_NONE, &td,
                D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv, &IID_ID3D12Resource, (void **)&s_depth_tex);
        if (FAILED(hr)) { d3d12_diag("depth R32_TYPELESS CreateCommittedResource 0x%08lX", hr); return 0; }
        s_depth_state = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        s_dsv_size = ID3D12Device_GetDescriptorHandleIncrementSize(g_d3d12.device, D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
        ZeroMemory(&hd, sizeof(hd));
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV; hd.NumDescriptors = 2;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        if (FAILED(ID3D12Device_CreateDescriptorHeap(g_d3d12.device, &hd, &IID_ID3D12DescriptorHeap, (void **)&s_dsv_heap))) return 0;
        ZeroMemory(&dvd, sizeof(dvd)); dvd.Format = DXGI_FORMAT_D32_FLOAT; dvd.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        ID3D12Device_CreateDepthStencilView(g_d3d12.device, s_depth_tex, &dvd, cpu_handle(s_dsv_heap, 0, s_dsv_size));
        dvd.Flags = D3D12_DSV_FLAG_READ_ONLY_DEPTH;   /* [1] read-only DSV */
        ID3D12Device_CreateDepthStencilView(g_d3d12.device, s_depth_tex, &dvd, cpu_handle(s_dsv_heap, 1, s_dsv_size));
        /* R32_FLOAT SRV in the CPU staging heap (copied into the ring when a pass
         * binds it), permanent slot. */
        s_depth_srv_slot = s_srv_stage_next++;
        ZeroMemory(&srvd, sizeof(srvd));
        srvd.Format = DXGI_FORMAT_R32_FLOAT;
        srvd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvd.Texture2D.MipLevels = 1;
        ID3D12Device_CreateShaderResourceView(g_d3d12.device, s_depth_tex, &srvd,
            cpu_handle(s_srv_stage, s_depth_srv_slot, s_srv_stage_size));
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

    /* One-shot copy command list (texture uploads, closed until used). */
    if (FAILED(ID3D12Device_CreateCommandAllocator(g_d3d12.device, D3D12_COMMAND_LIST_TYPE_DIRECT,
            &IID_ID3D12CommandAllocator, (void **)&s_copy_alloc))) return 0;
    if (FAILED(ID3D12Device_CreateCommandList(g_d3d12.device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT,
            s_copy_alloc, NULL, &IID_ID3D12GraphicsCommandList, (void **)&s_copy_list))) return 0;
    ID3D12GraphicsCommandList_Close(s_copy_list);

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

    /* 1x1 white default texture (also validates the whole texture upload path
     * -- CreateCommittedResource + staging + CopyTextureRegion + barriers + SRV
     * -- against the debug layer at init). */
    s_white_tex = d3d12_tex_create(1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, 0);
    if (!s_white_tex) return 0;
    { unsigned int wp = 0xFFFFFFFFu; d3d12_tex_upload(s_white_tex, &wp, 4, 4); }
    /* 1x1 zero texture: the offscreen passes bind it at t1/t2 until the real
     * gbuffer/scene-copy exist, mirroring d3d11's gbuffer cleared-to-0 (matid 0
     * = legacy fallback) so the shaders take the same path. */
    s_black_tex = d3d12_tex_create(1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, 0);
    if (!s_black_tex) return 0;
    { unsigned int bp = 0x00000000u; d3d12_tex_upload(s_black_tex, &bp, 4, 4); }
    d3d12_flush_uploads();

    /* Warm-up PSO: exercises get_pso + validates root-sig/shader/layout wiring
     * against the debug layer at init (before any frame). */
    if (!d3d12_get_pso(0, PS_MODULATE, BLEND_SRCALPHA_INVSRC, DS_Z_OFF_WRITE_OFF, 0, 0)) return 0;

    /* Present-time fullscreen blit PSO (no input layout; SV_VertexID triangle). */
    {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd;
        ZeroMemory(&pd, sizeof(pd));
        pd.pRootSignature = s_root_sig;
        pd.VS.pShaderBytecode = g_vs_fullscreen_50; pd.VS.BytecodeLength = sizeof(g_vs_fullscreen_50);
        pd.PS.pShaderBytecode = g_ps_composite_50;  pd.PS.BytecodeLength = sizeof(g_ps_composite_50);
        d3d12_fill_blend(BLEND_OPAQUE, &pd.BlendState);
        d3d12_fill_ds(DS_Z_OFF_WRITE_OFF, &pd.DepthStencilState);
        d3d12_fill_raster(0, &pd.RasterizerState);
        pd.SampleMask = 0xFFFFFFFFu;
        pd.InputLayout.NumElements = 0;   /* fullscreen triangle from SV_VertexID */
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 1; pd.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM;
        pd.DSVFormat = DXGI_FORMAT_D32_FLOAT; pd.SampleDesc.Count = 1;
        if (FAILED(ID3D12Device_CreateGraphicsPipelineState(g_d3d12.device, &pd,
                &IID_ID3D12PipelineState, (void **)&s_fsquad_pso))) {
            WRAPPER_LOG("D3D12 fsquad PSO create FAILED");
            return 0;
        }
    }

    d3d12_diag("render_core_init OK (samplers+heaps+upload+white+PSOs)");
    WRAPPER_LOG("D3D12 render core: root sig + %d samplers + heaps + %uMB upload ring x%d + warm PSO OK",
                SAMP_STATE_COUNT, D3D12_UPLOAD_RING_BYTES >> 20, D3D12_FRAME_COUNT);
    return 1;
}

static void d3d12_render_core_shutdown(void)
{
    int i;
    d3d12_flush_uploads();
    free(s_up_staging); s_up_staging = NULL; s_up_cap = 0;
    if (s_fsquad_pso)  { ID3D12PipelineState_Release(s_fsquad_pso); s_fsquad_pso = NULL; }
    for (i = 0; i < s_pass_pso_count; i++) if (s_pass_pso[i].pso) ID3D12PipelineState_Release(s_pass_pso[i].pso);
    s_pass_pso_count = 0;
    if (s_pass_root_sig) { ID3D12RootSignature_Release(s_pass_root_sig); s_pass_root_sig = NULL; }
    if (s_white_tex)   { if (s_white_tex->res) ID3D12Resource_Release(s_white_tex->res); free(s_white_tex); s_white_tex = NULL; }
    if (s_black_tex)   { if (s_black_tex->res) ID3D12Resource_Release(s_black_tex->res); free(s_black_tex); s_black_tex = NULL; }
    if (s_scene_copy)  { if (s_scene_copy->res) ID3D12Resource_Release(s_scene_copy->res); free(s_scene_copy); s_scene_copy = NULL; }
    if (s_copy_list)   { ID3D12GraphicsCommandList_Release(s_copy_list); s_copy_list = NULL; }
    if (s_copy_alloc)  { ID3D12CommandAllocator_Release(s_copy_alloc); s_copy_alloc = NULL; }
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

/* ---- display window (standalone passes hwnd=NULL -> backend owns it) ---- */

static HWND s_display_hwnd;

static LRESULT CALLBACK D3D12DisplayWindowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_SETCURSOR && LOWORD(lp) == HTCLIENT) { SetCursor(NULL); return TRUE; }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static HWND d3d12_create_display_window(int client_w, int client_h)
{
    WNDCLASSEXA wc;
    DWORD style = WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_VISIBLE;
    RECT wr = { 0, 0, client_w, client_h };
    int scr_w, scr_h, x, y;
    HWND hwnd;
    static char title_buf[256];
    const char *title = "Test Drive 5";
    DWORD n;

    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = D3D12DisplayWindowProc;
    wc.hInstance     = GetModuleHandleA(NULL);
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = "TD5_D3D12_Display";
    RegisterClassExA(&wc);

    AdjustWindowRect(&wr, style, FALSE);
    scr_w = GetSystemMetrics(SM_CXSCREEN);
    scr_h = GetSystemMetrics(SM_CYSCREEN);
    x = (scr_w - (wr.right - wr.left)) / 2;
    y = (scr_h - (wr.bottom - wr.top)) / 2;

    n = GetEnvironmentVariableA("TD5RE_WINDOW_TITLE", title_buf, sizeof(title_buf));
    if (n > 0 && n < sizeof(title_buf)) title = title_buf;

    hwnd = CreateWindowExA(0, "TD5_D3D12_Display", title, style, x, y,
                           wr.right - wr.left, wr.bottom - wr.top,
                           NULL, NULL, GetModuleHandleA(NULL), NULL);
    if (hwnd) { ShowWindow(hwnd, SW_SHOW); UpdateWindow(hwnd); }
    else WRAPPER_LOG("D3D12 create_display_window FAILED");
    return hwnd;
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

    /* Standalone (main.c) passes hwnd=NULL: the backend owns the display window,
     * exactly like the D3D11 backend. Without this, CreateSwapChainForHwnd(NULL)
     * fails and CreateDevice returns 0 (-> MessageBox + exit). */
    if (!hwnd && windowed) {
        if (!s_display_hwnd) s_display_hwnd = d3d12_create_display_window(width, height);
        hwnd = s_display_hwnd;
    }
    if (!hwnd) { d3d12_diag("CreateDevice: no window (hwnd NULL, windowed=%d)", windowed); return 0; }

    /* Match the D3D11 backend: size the render to the window's ACTUAL client
     * rect. AdjustWindowRect + a caption bar can push the outer window past the
     * screen, so Windows clamps it and the real client area ends up smaller
     * than the requested (INI) resolution. D3D11 renders to that clamped client
     * (GetClientRect); we must too, or the swapchain is larger than the window
     * (DXGI downscales on present) and framedumps disagree by the clamp delta. */
    if (windowed) {
        RECT crc;
        if (GetClientRect(hwnd, &crc) && crc.right > 0 && crc.bottom > 0) {
            if ((int)crc.right != width || (int)crc.bottom != height)
                d3d12_diag("CreateDevice: client rect %ldx%ld (requested %dx%d) -> using client",
                           crc.right, crc.bottom, width, height);
            width  = (int)crc.right;
            height = (int)crc.bottom;
        }
    }

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

/* Release EVERY D3D12 object (device-level + render-core + deferred queue) and
 * zero g_d3d12. `wait_gpu` waits for idle first (clean shutdown); pass 0 when
 * the device is REMOVED -- its fence will never signal, so a wait would hang.
 * Frees the whole deferred-deletion list unconditionally afterward (the GPU is
 * either idle or dead, so nothing can still reference the resources) -- this is
 * what fixes the per-recreation leak. */
static void d3d12_release_all(int wait_gpu)
{
    UINT i;
    if (wait_gpu) d3d12_wait_idle();
    d3d12_render_core_shutdown();  /* its final flush_uploads may retire staging + signal */
    if (wait_gpu) d3d12_wait_idle();  /* ensure that last copy fence is passed */
    d3d12_flush_retired();         /* free every deferred resource (no live refs) */
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

void Backend_Shutdown(void)
{
    d3d12_release_all(1);
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
    s_dbg_clears++;
    d3d12_frame_begin();
    {
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = d3d12_rtv_handle(g_d3d12.frame_index);
        ID3D12GraphicsCommandList_ClearRenderTargetView(g_d3d12.list, rtv, rgba, 0, NULL);
    }
}

void Backend_ClearSwapChainRT(const float *rgba) { Backend_ClearBackbuffer(rgba); }
void Backend_BindSwapChainRT(void)               { d3d12_frame_begin(); }
void Backend_ClearDepth(float z)
{
    if (!g_d3d12.device || !s_dsv_heap) return;
    d3d12_frame_begin();
    ID3D12GraphicsCommandList_ClearDepthStencilView(g_d3d12.list, cpu_handle(s_dsv_heap, 0, s_dsv_size),
        D3D12_CLEAR_FLAG_DEPTH, z, 0, 0, NULL);
}

void Backend_PresentSwapChain(int sync)          { d3d12_frame_present(sync); }

/* Present-time composite. Frontend (2D, scene_rendered==0): flush the primary
 * surface's CPU sys_buffer to its GPU texture and blit it to the swapchain.
 * Race (3D, scene_rendered==1): DrawPrimitive already rendered to the swapchain
 * in this Phase-2 bring-up, so present as-is. (Render-to-surface + the two-layer
 * HUD-overlay composite land with Phase-3 offscreen surfaces.) */
void Backend_CompositeAndPresent(WrapperSurface *rt, RECT *s, RECT *d)
{
    (void)s; (void)d;
    if (!g_d3d12.device || !g_d3d12.swapchain || g_backend.device_removed) return;
    if (!g_d3d12.frame_open) d3d12_frame_begin();

    {
        static int s_cc = 0;
        if ((s_cc % 120) == 0) { d3d12_diag("composite[%d] rt=%p bt=%p dirty=%d scene=%d draws=%u clears=%u srv=%p",
            s_cc, (void*)rt, rt?(void*)rt->bt:NULL, rt?rt->dirty:-1, g_backend.scene_rendered,
            s_dbg_draws, s_dbg_clears, (void*)s_cur_tex); }
        s_cc++;
    }
    /* The port renders every pane straight to the swapchain via the Rec/Plat
     * draw path (Backend_PlatDrawTris) -- there is no offscreen game RT to blit
     * here (unlike the D3D11 wrapper-DLL flow). Present what was drawn. A
     * dirty 2D BltFast surface (rare) is still composited over it. */
    if (rt && rt->bt && rt->bt->res && rt->dirty) {
        WrapperSurface_FlushDirty(rt);
        d3d12_fullscreen_blit(rt->bt);
    }

    Backend_CaptureIfRequested();
    d3d12_frame_present(g_backend.vsync ? 1 : 0);
}

HWND Backend_GetDisplayWindow(void) { return s_display_hwnd ? s_display_hwnd : g_backend.hwnd; }

/* Framedump / render-golden capture: return the most recent PRE-FLIP frame
 * captured by d3d12_frame_present (flip-discard makes a post-present readback
 * undefined). Requires TD5RE_FRAMEDUMP / TD5RE_D3D12_CAPTURE to be set so the
 * per-present capture is armed. Freshly malloc'd RGBA8, caller frees. */
unsigned char *Backend_CaptureBackbufferRGBA(int *out_w, int *out_h)
{
    unsigned char *out;
    if (!s_cap_buf || s_cap_w == 0 || s_cap_h == 0) {
        if (!d3d12_capture_active())
            WRAPPER_LOG("D3D12 capture: set TD5RE_FRAMEDUMP/TD5RE_D3D12_CAPTURE to arm per-present capture");
        return NULL;
    }
    out = (unsigned char *)malloc((size_t)s_cap_w * s_cap_h * 4);
    if (!out) return NULL;
    memcpy(out, s_cap_buf, (size_t)s_cap_w * s_cap_h * 4);
    if (out_w) *out_w = (int)s_cap_w;
    if (out_h) *out_h = (int)s_cap_h;
    return out;
}

/* ======================================================================== *
 *  STUBS -- not yet implemented on D3D12 (fill in Phases 2-4). They let the
 *  library link so the window/clear/present skeleton runs.
 * ======================================================================== */

/* Deferred dynamic lights: additive fullscreen pass reconstructing world pos
 * from depth (t0) and accumulating each light's contribution. t1 (gbuffer
 * normals) is the zero placeholder -> N.L falls back to legacy (matches the
 * port's empty gbuffer on the PlatDrawTris path). Runs over the active pane's
 * viewport/scissor. BLEND_ONE_ONE (additive). */
void Backend_ApplyLightPass(const LightCB *cb)
{
    UINT srvs[1];
    if (!cb) return;
    srvs[0] = s_depth_srv_slot;
    d3d12_fullscreen_pass(g_ps_light_50, sizeof(g_ps_light_50), BLEND_ONE_ONE,
                          cb, sizeof(LightCB), srvs, 1);
}
/* Screen-space reflections: copy the current scene color to s_scene_copy, then
 * an alpha-blended fullscreen pass that marches reflections in screen space
 * (depth t0, gbuffer t1=zero, scene copy t2). BLEND_SRCALPHA_INVSRC. */
void Backend_ApplySSRPass(const SSRCB *cb)
{
    ID3D12GraphicsCommandList *cl = g_d3d12.list;
    UINT fi, srvs[3];
    if (!cb || !g_d3d12.device || g_backend.device_removed) return;
    if (!g_d3d12.frame_open) d3d12_frame_begin();
    fi = g_d3d12.frame_index;

    /* Ensure the scene-copy texture matches the backbuffer. */
    if (!s_scene_copy || s_scene_copy->w != (UINT)g_backend.width ||
        s_scene_copy->h != (UINT)g_backend.height) {
        if (s_scene_copy) { if (s_scene_copy->res) d3d12_retire(s_scene_copy->res); free(s_scene_copy); s_scene_copy = NULL; }
        s_scene_copy = d3d12_tex_create((UINT)g_backend.width, (UINT)g_backend.height,
                                        DXGI_FORMAT_B8G8R8A8_UNORM, 0);
        if (!s_scene_copy) return;
    }

    /* Copy swapchain backbuffer -> scene_copy (reflections read the pre-SSR scene). */
    d3d12_resource_barrier(g_d3d12.backbuffers[fi], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    if (s_scene_copy->rstate != D3D12_RESOURCE_STATE_COPY_DEST)
        d3d12_resource_barrier(s_scene_copy->res, s_scene_copy->rstate, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12GraphicsCommandList_CopyResource(cl, s_scene_copy->res, g_d3d12.backbuffers[fi]);
    d3d12_resource_barrier(s_scene_copy->res, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    s_scene_copy->rstate = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    d3d12_resource_barrier(g_d3d12.backbuffers[fi], D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);

    srvs[0] = s_depth_srv_slot;
    srvs[1] = s_black_tex ? s_black_tex->srv_slot : s_depth_srv_slot;
    srvs[2] = s_scene_copy->srv_slot;
    d3d12_fullscreen_pass(g_ps_ssr_50, sizeof(g_ps_ssr_50), BLEND_SRCALPHA_INVSRC,
                          cb, sizeof(SSRCB), srvs, 3);
}
/* Screen-space ray-marched sun shadow: multiplicative fullscreen pass that
 * reconstructs world pos from scene depth (t0) and darkens shadowed pixels.
 * t1 (gbuffer) is the depth SRV placeholder for now -- the gbuffer is not yet
 * populated (Phase 4 later), which matches the port's largely-empty gbuffer on
 * the PlatDrawTris path. BLEND_MULT so out = dst*src. */
void Backend_ApplyShadowPass(const ShadowCB *cb)
{
    UINT srvs[1];
    if (!cb) return;
    srvs[0] = s_depth_srv_slot;
    d3d12_fullscreen_pass(g_ps_shadow_50, sizeof(g_ps_shadow_50), BLEND_MULT,
                          cb, sizeof(ShadowCB), srvs, 1);
}
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
    ps->id = PS_MODULATE;   /* fallback if the registry is full */
    if (s_custom_ps_count < D3D12_CUSTOM_PS_MAX) {
        ps->id = PS_COUNT + s_custom_ps_count;
        s_custom_ps[s_custom_ps_count++] = ps;
    }
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
void Backend_DrawPrimitive(DWORD prim, UINT stride, UINT base_vertex, UINT vert_count)
{
    const RenderStateCache *s = &g_backend.state;
    int tt; D3D_PRIMITIVE_TOPOLOGY topo;
    if (!g_d3d12.device) return;
    Backend_UpdateFogCB();   /* per-draw alpha-test/fog */
    topo = d3d12_map_topo(prim, &tt);
    d3d12_bind_and_draw(topo, tt, d3d12_sel_blend(s), d3d12_sel_ds(s), s->polygon_offset ? 1 : 0,
                        d3d12_sel_ps(s), d3d12_sel_samp(s), stride, 0, base_vertex, 0, vert_count);
}
void Backend_DrawIndexedPrimitive(DWORD prim, UINT stride, UINT base_vertex, UINT start_index, UINT index_count, UINT vert_count)
{
    const RenderStateCache *s = &g_backend.state;
    int tt; D3D_PRIMITIVE_TOPOLOGY topo;
    (void)vert_count;
    if (!g_d3d12.device) return;
    Backend_UpdateFogCB();
    topo = d3d12_map_topo(prim, &tt);
    d3d12_bind_and_draw(topo, tt, d3d12_sel_blend(s), d3d12_sel_ds(s), s->polygon_offset ? 1 : 0,
                        d3d12_sel_ps(s), d3d12_sel_samp(s), stride, 1, base_vertex, start_index, index_count);
}
/* Append GPU forensics to the SEH crash file (from the exe's crash handler).
 * NOT gated on debug -- a crash is rare and we always want it. Reads only
 * scalar/pointer VALUES, never derefs a GPU object -> safe in a fault handler. */
void Backend_DumpCrashDiag(const char *path)
{
    FILE *f = fopen(path ? path : "log/crash.log", "a");
    if (!f) return;
    fprintf(f,
        "\n---- D3D12 GPU crash diagnostics ----\n"
        "  device=%p queue=%p swapchain=%p\n"
        "  device_generation=%u device_removed=%d present_count=%lu\n"
        "  cur_tex=%p windowed=%d rt=%dx%d frame_index=%u\n"
        "  diag_context=\"%s\"\n",
        (void *)g_d3d12.device, (void *)g_d3d12.queue, (void *)g_d3d12.swapchain,
        g_backend.device_generation, g_backend.device_removed, g_backend.present_count,
        (void *)s_cur_tex, g_backend.windowed, g_backend.width, g_backend.height,
        g_d3d12.frame_index, g_backend.diag_context);
    d3d12_write_draw_ring(f, "SEH crash");
    fflush(f); fclose(f);
}
void Backend_EnforceWindowSize(void) { }
void Backend_EnsureCompositingTextures(int w, int h) { (void)w; (void)h; }
void Backend_ForceBlendState(int blend_idx) { (void)blend_idx; }
int  Backend_GetCapture(unsigned char **px, int *w, int *h) { (void)px;(void)w;(void)h; return 0; }
void Backend_MaybeTrim(void) { }
void *Backend_PixelShaderRaw(BackendPixelShader *ps) { return ps; }
/* The port's real render path: td5_platform_win32.c records TD5_D3DVertex (the
 * 32-byte XYZRHW layout) + an optional override pixel shader (BackendPixelShader*)
 * + a sampler index, and binds textures by BackendTexture*. Route straight through
 * the draw core (immediate; the deferred-context/pane machinery is bypassed). */
void Backend_PlatBindTextureSRV(WrapperRecCtx *rc, void *srv)
{
    (void)rc;
    s_cur_tex = (BackendTexture *)srv;   /* our SurfaceGetSRV / page cache returns BackendTexture* */
}
void Backend_PlatDrawTris(WrapperRecCtx *rc, const void *v, int vc, const void *idx, int ic, void *pso, int ss)
{
    /* Resolve blend/depth-stencil/raster from g_backend.state -- populated by
     * td5_plat_render_set_preset (rc/g_wrapper_rec is NULL in this backend, so
     * presets always land in g_backend.state). This is the D3D11
     * Backend_ApplyStateCache equivalent: without it the 3D world would draw
     * with fixed Z-OFF UI state and rely on draw order instead of depth test. */
    const RenderStateCache *s = &g_backend.state;
    BackendPixelShader *ps = (BackendPixelShader *)pso;
    /* PS + sampler: honour the MSDF/SDF override; otherwise mirror d3d11's
     * PlatDrawTris (ps = texblend_mode==5 ? PS_MODULATE_ALPHA : PS_MODULATE;
     * samp = mag_filter>=2 ? LINEAR_WRAP : POINT_WRAP). */
    int ps_id = ps ? ps->id
                   : (s->texblend_mode == 5 ? PS_MODULATE_ALPHA : PS_MODULATE);
    int samp  = ps ? ((ss >= 0 && ss < SAMP_STATE_COUNT) ? ss : SAMP_LINEAR_CLAMP)
                   : (s->mag_filter >= 2 ? SAMP_LINEAR_WRAP : SAMP_POINT_WRAP);
    UINT bv = 0, si = 0;
    int indexed = (idx && ic > 0);
    (void)rc;
    if (!g_d3d12.device || !v || vc <= 0) return;
    if (!Backend_StreamUpload(v, (UINT)vc, TD5_VERTEX_STRIDE, indexed ? idx : NULL, indexed ? (UINT)ic : 0, &bv, &si))
        return;
    d3d12_bind_and_draw(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, 0,
                        d3d12_sel_blend(s), d3d12_sel_ds(s), s->polygon_offset ? 1 : 0,
                        ps_id, samp,
                        TD5_VERTEX_STRIDE, indexed, bv, si, indexed ? (UINT)ic : (UINT)vc);
}
void Backend_PlatDrawWhite(WrapperRecCtx *rc, const void *v, int vc, const void *idx, int ic, int lines)
{
    UINT bv = 0, si = 0;
    BackendTexture *save;
    int indexed = (idx && ic > 0);
    (void)rc;
    if (!g_d3d12.device || !v || vc <= 0) return;
    if (!Backend_StreamUpload(v, (UINT)vc, TD5_VERTEX_STRIDE, indexed ? idx : NULL, indexed ? (UINT)ic : 0, &bv, &si))
        return;
    save = s_cur_tex; s_cur_tex = NULL;   /* PS_MODULATE * white == vertex colour */
    /* Match d3d11 PlatDrawWhite: opaque blend, Z-test on / Z-write off (occlude
     * correctly without poisoning depth), raster 0, point-clamp sampler. */
    d3d12_bind_and_draw(lines ? D3D_PRIMITIVE_TOPOLOGY_LINELIST : D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, lines ? 1 : 0,
                        BLEND_OPAQUE, DS_Z_ON_WRITE_OFF, 0, PS_MODULATE, SAMP_POINT_CLAMP,
                        TD5_VERTEX_STRIDE, indexed, bv, si, indexed ? (UINT)ic : (UINT)vc);
    s_cur_tex = save;
}
void Backend_PlatSetScissor(WrapperRecCtx *rc, int l, int t, int r, int b)
{
    (void)rc;
    s_cur_scissor.left = l; s_cur_scissor.top = t; s_cur_scissor.right = r; s_cur_scissor.bottom = b;
}
void Backend_PlatSetViewport(WrapperRecCtx *rc, int x, int y, int w, int h)
{
    (void)rc;
    s_cur_vp.TopLeftX = (float)x; s_cur_vp.TopLeftY = (float)y;
    s_cur_vp.Width = (float)w; s_cur_vp.Height = (float)h; s_cur_vp.MinDepth = 0.0f; s_cur_vp.MaxDepth = 1.0f;
    s_cur_scissor.left = x; s_cur_scissor.top = y; s_cur_scissor.right = x + w; s_cur_scissor.bottom = y + h;
}
WrapperRecCtx *Backend_RecBegin(int i, int x, int y, int w, int h) { (void)i;(void)x;(void)y;(void)w;(void)h; return NULL; }
void Backend_RecEnd(WrapperRecCtx *rc) { (void)rc; }
void Backend_RecExecute(int i) { (void)i; }
int  Backend_RecPoolEnsure(int c) { (void)c; return 0; }
void Backend_RecPoolRelease(void) { }
/* Recover from a removed device (TDR/hang): tear down every GPU object WITHOUT
 * waiting on the dead fence, recreate the device/swapchain/render-core on the
 * SAME window, and bump device_generation so the backend-agnostic surface/
 * texture rebuild machinery re-uploads on next use. Leak-free: d3d12_release_all
 * frees the whole deferred-deletion list. */
int Backend_RecreateDevice(void)
{
    int  w = g_backend.width, h = g_backend.height, bpp = g_backend.bpp, windowed = g_backend.windowed;
    UINT gen = g_backend.device_generation;
    HWND hwnd = s_display_hwnd;   /* reuse the existing window */
    /* A forced-test loss (TD5RE_FORCE_DEVICE_LOST) latches device_removed while
     * the GPU device is still ALIVE -> wait for idle so we release cleanly. A
     * real TDR/removal -> GetDeviceRemovedReason != S_OK -> skip the wait (the
     * dead fence never signals). */
    int alive = (g_d3d12.device &&
                 ID3D12Device_GetDeviceRemovedReason(g_d3d12.device) == S_OK);
    WRAPPER_LOG("D3D12 RecreateDevice: begin (gen %u -> %u) %dx%d alive=%d", gen, gen + 1, w, h, alive);

    d3d12_release_all(alive);
    /* The new device's fence restarts at completed-value 0, so our monotonic
     * counters MUST reset or the first wait_value(old-high) would hang forever. */
    s_fence_val  = 0;
    s_copy_fence = 0;

    if (!Backend_CreateDevice(hwnd, w, h, bpp, windowed)) {
        WRAPPER_LOG("D3D12 RecreateDevice: CreateDevice FAILED");
        return 0;
    }
    g_backend.device_generation = gen + 1;
    g_backend.device_removed    = 0;
    WRAPPER_LOG("D3D12 RecreateDevice: OK (gen=%u)", gen + 1);
    return 1;
}
void Backend_ReleaseConstBuffer(BackendConstBuffer *cb) { if (cb) { if (cb->res) ID3D12Resource_Release(cb->res); free(cb); } }
void Backend_ReleasePixelShader(BackendPixelShader *ps) { if (ps) { free((void *)ps->bc); free(ps); } }
void Backend_RequestCapture(void) { }
int  Backend_Reset(int w, int h, int bpp, int windowed) { (void)w;(void)h;(void)bpp;(void)windowed; return 1; }
void Backend_RestoreMainRenderTarget(void) { }
void Backend_SelectPixelShader(void) { }  /* PS chosen at draw time from g_backend.state */
void Backend_SetBuiltinPixelShader(int ps_idx) { if (ps_idx >= 0 && ps_idx < PS_COUNT) s_cur_ps = ps_idx; }
int  Backend_SetExclusiveFullscreen(int enable) { (void)enable; return 1; }
void Backend_SetGBufferEnabled(int on) { (void)on; }
void Backend_SetViewport(float x, float y, float w, float h, float mn, float mx)
{
    s_cur_vp.TopLeftX = x; s_cur_vp.TopLeftY = y; s_cur_vp.Width = w; s_cur_vp.Height = h;
    s_cur_vp.MinDepth = mn; s_cur_vp.MaxDepth = mx;
}

/* Stream verts (+optional u16 indices) into the current frame's upload ring;
 * returns base_vertex / start_index for the VBV/IBV bound at ring base 0. */
int Backend_StreamUpload(const void *verts, UINT vc, UINT stride, const void *indices, UINT ic, UINT *obv, UINT *osi)
{
    UINT fi = g_d3d12.frame_index;
    UINT cur, vbytes, voff;
    if (!s_upload_cpu[fi] || !verts || vc == 0 || stride == 0) return 0;
    Backend_NoteVerts(verts, vc, stride);   /* crash-ring extent/NaN scan (debug-gated) */
    vbytes = vc * stride;
    cur = (s_upload_off[fi] + stride - 1) / stride * stride;   /* stride-align -> integer base_vertex */
    if ((UINT64)cur + vbytes > D3D12_UPLOAD_RING_BYTES) { WRAPPER_LOG("D3D12 VB ring overflow"); return 0; }
    memcpy(s_upload_cpu[fi] + cur, verts, vbytes);
    voff = cur; cur += vbytes;
    if (obv) *obv = voff / stride;
    if (indices && ic) {
        UINT ibytes = ic * 2u, ioff;
        cur = (cur + 1u) & ~1u;                                /* 2-align R16 indices */
        if ((UINT64)cur + ibytes > D3D12_UPLOAD_RING_BYTES) { WRAPPER_LOG("D3D12 IB ring overflow"); return 0; }
        memcpy(s_upload_cpu[fi] + cur, indices, ibytes);
        ioff = cur; cur += ibytes;
        if (osi) *osi = ioff / 2u;
    }
    s_upload_off[fi] = cur;
    return 1;
}
void Backend_SurfaceBindRenderTarget(WrapperSurface *s) { (void)s; }
/* The port treats the returned handle as an opaque texture token and passes it
 * back to Backend_PlatBindTextureSRV, so return the surface's BackendTexture*. */
ID3D11ShaderResourceView *Backend_SurfaceGetSRV(WrapperSurface *s) { return s ? (ID3D11ShaderResourceView *)s->bt : NULL; }
int  Backend_SurfaceHasRTV(WrapperSurface *s) { (void)s; return 0; }
void Backend_TextureAddRef(BackendTexture *bt) { if (bt) InterlockedIncrement(&bt->ref); }
BackendTexture *Backend_TextureAdopt(void *n) { (void)n; return NULL; }  /* n/a: no external ID3D11 texture on d3d12 */
int  Backend_TextureBind(BackendTexture *bt, UINT stage) { (void)stage; s_cur_tex = bt; return 1; }
void Backend_TextureBindRenderTarget(BackendTexture *bt) { (void)bt; /* Phase 3 (offscreen surfaces) */ }
void Backend_TextureClearRT(BackendTexture *bt, const float *rgba) { (void)bt;(void)rgba; /* Phase 3 */ }
BackendTexture *Backend_TextureCreate(DWORD w, DWORD h, DXGI_FORMAT f, int rt, int st) { (void)st; return d3d12_tex_create((UINT)w, (UINT)h, f, rt); }
void Backend_TextureEnsureCurrent(BackendTexture *bt, DWORD w, DWORD h, DXGI_FORMAT f, int rt, int st) { (void)bt;(void)w;(void)h;(void)f;(void)rt;(void)st; /* Phase 3 (surface resize) */ }
BackendTexture *Backend_TextureFromBGRA(const void *px, int w, int h)
{
    BackendTexture *bt = d3d12_tex_create((UINT)w, (UINT)h, DXGI_FORMAT_B8G8R8A8_UNORM, 0);
    if (bt && px) d3d12_tex_upload(bt, px, 4, (UINT)w * 4u);
    return bt;
}
int  Backend_TextureHasRTV(const BackendTexture *bt) { return bt && bt->has_rtv; }
int  Backend_TextureIsValid(const BackendTexture *bt) { return bt && bt->valid && bt->res != NULL; }
/* Decode M2DX source pixels (TGA/palette, 16 or 32 bpp) into the destination
 * GPU texture as B8G8R8A8 (all wrapper surfaces are forced to BGRA8), replicating
 * the D3D11 Backend_TextureLoad: A1R5G5B5-vs-R5G6B5 auto-detection, colorkey ->
 * alpha=0, 32bpp direct. Falls back to a GPU copy from src_bt when there are no
 * CPU pixels. (PNG-override path deferred.) */
int Backend_TextureLoad(BackendTexture **pbt, DWORD dst_w, DWORD dst_h, DXGI_FORMAT dst_fmt,
                        const void *src_pixels, LONG src_pitch, DWORD src_w, DWORD src_h, DWORD src_bpp,
                        int src_has_alpha, int has_colorkey, DWORD colorkey_low,
                        BackendTexture *src_bt, int *out_r5)
{
    DWORD copy_w, copy_h, y, x, row32;
    BackendTexture *dst = pbt ? *pbt : NULL;
    unsigned char *buf;
    (void)dst_fmt;
    if (!g_d3d12.device) return 0;

    if (src_pixels && src_w > 0 && src_h > 0) {
        const unsigned char *sp = (const unsigned char *)src_pixels;
        copy_w = dst_w < src_w ? dst_w : src_w;
        copy_h = dst_h < src_h ? dst_h : src_h;
        row32  = dst_w * 4u;
        buf = (unsigned char *)calloc((size_t)row32 * dst_h, 1);
        if (!buf) return 0;

        if (src_bpp == 16) {
            int is_a1 = 0;
            if (src_has_alpha) {
                const uint16_t *s = (const uint16_t *)sp;
                DWORD total = copy_w * copy_h, limit = total < 4096 ? total : 4096, i;
                DWORD nonzero = 0, bit15c = 0, bright = 0;
                for (i = 0; i < limit; i++) {
                    uint16_t px = s[i];
                    if (px) { nonzero++;
                        if (!(px & 0x8000)) { bit15c++;
                            if (((px>>10)&0x1F) + ((px>>5)&0x1F) + (px&0x1F) > 10) bright++; } }
                }
                is_a1 = !(nonzero > 16 && bit15c * 100 > nonzero && bright > bit15c / 4);
            }
            for (y = 0; y < copy_h; y++) {
                const uint16_t *s16 = (const uint16_t *)(sp + (size_t)y * src_pitch);
                uint32_t *d32 = (uint32_t *)(buf + (size_t)y * row32);
                if (is_a1) {
                    for (x = 0; x < copy_w; x++) {
                        uint16_t c = s16[x];
                        uint32_t a = (c & 0x8000) ? 0xFF000000u : 0u;
                        uint32_t r = ((c>>10)&0x1F)*255u/31u, g = ((c>>5)&0x1F)*255u/31u, b = (c&0x1F)*255u/31u;
                        d32[x] = a | (r<<16) | (g<<8) | b;
                    }
                    if (out_r5) *out_r5 = 0;
                } else {
                    int use_ck = has_colorkey; uint16_t ck = (uint16_t)(colorkey_low & 0xFFFF);
                    for (x = 0; x < copy_w; x++) {
                        uint16_t c = s16[x];
                        uint32_t r = ((c>>11)&0x1F)*255u/31u, g = ((c>>5)&0x3F)*255u/63u, b = (c&0x1F)*255u/31u;
                        uint32_t a = (use_ck && c == ck) ? 0u : 0xFF000000u;
                        d32[x] = a | (r<<16) | (g<<8) | b;
                    }
                    if (out_r5) *out_r5 = use_ck ? 0 : 1;
                }
            }
        } else {
            /* 32bpp source -> tight BGRA rows. */
            for (y = 0; y < copy_h; y++)
                memcpy(buf + (size_t)y * row32, sp + (size_t)y * src_pitch, (size_t)copy_w * 4);
        }
        d3d12_bt_recreate_from_init(pbt, dst_w, dst_h, DXGI_FORMAT_B8G8R8A8_UNORM, buf, row32);
        free(buf);
        return 1;
    }

    /* No CPU pixels: GPU copy from src_bt (surface->surface blit), same size. */
    if (src_bt && src_bt->res && src_bt->w == dst_w && src_bt->h == dst_h) {
        BackendTexture *nd = (dst && dst->res && dst->w == dst_w && dst->h == dst_h)
                             ? dst : d3d12_tex_create(dst_w, dst_h, DXGI_FORMAT_B8G8R8A8_UNORM, 0);
        D3D12_RESOURCE_BARRIER b[2];
        ID3D12GraphicsCommandList *cl;
        if (!nd) return 0;
        d3d12_copy_ensure(); cl = s_copy_list;
        ZeroMemory(b, sizeof(b));
        b[0].Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b[0].Transition.pResource=src_bt->res;
        b[0].Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b[0].Transition.StateBefore=src_bt->rstate; b[0].Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE;
        b[1].Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b[1].Transition.pResource=nd->res;
        b[1].Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b[1].Transition.StateBefore=nd->rstate; b[1].Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_DEST;
        ID3D12GraphicsCommandList_ResourceBarrier(cl, 2, b);
        ID3D12GraphicsCommandList_CopyResource(cl, nd->res, src_bt->res);
        b[0].Transition.StateBefore=D3D12_RESOURCE_STATE_COPY_SOURCE; b[0].Transition.StateAfter=D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        b[1].Transition.StateBefore=D3D12_RESOURCE_STATE_COPY_DEST;   b[1].Transition.StateAfter=D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        ID3D12GraphicsCommandList_ResourceBarrier(cl, 2, b);
        src_bt->rstate = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        nd->rstate = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        if (nd != dst && pbt) { if (dst) Backend_TextureRelease(dst); *pbt = nd; }
        return 1;
    }
    if (out_r5) *out_r5 = 0;
    return 0;
}
void Backend_TextureRelease(BackendTexture *bt)
{
    if (!bt) return;
    if (InterlockedDecrement(&bt->ref) <= 0) {
        if (bt == s_cur_tex) s_cur_tex = NULL;
        /* Defer the GPU resource free until the in-flight frame that may still
         * reference this texture (e.g. the mid-frame recreate path) completes.
         * The CPU-side struct is safe to free now -- nothing on the GPU touches it. */
        if (bt->res) d3d12_retire(bt->res);
        free(bt);
    }
}
void Backend_TextureUpload(BackendTexture *bt, const void *sys, LONG src_pitch, DWORD w, DWORD h, DWORD src_bpp)
{
    if (!bt || !bt->res || !sys) return;

    if (src_bpp == 16 && bt->fmt == DXGI_FORMAT_B8G8R8A8_UNORM) {
        /* Convert R5G6B5 -> B8G8R8A8 (opaque) then upload (matches D3D11). */
        DWORD row32 = w * 4u, y, x;
        unsigned char *buf = (unsigned char *)malloc((size_t)row32 * h);
        if (!buf) return;
        for (y = 0; y < h; y++) {
            const uint16_t *s16 = (const uint16_t *)((const unsigned char *)sys + (size_t)y * src_pitch);
            uint32_t       *d32 = (uint32_t *)(buf + (size_t)y * row32);
            for (x = 0; x < w; x++) {
                uint16_t c = s16[x];
                uint32_t r = ((c >> 11) & 0x1F) * 255u / 31u;
                uint32_t g = ((c >>  5) & 0x3F) * 255u / 63u;
                uint32_t b = ( c        & 0x1F) * 255u / 31u;
                d32[x] = 0xFF000000u | (r << 16) | (g << 8) | b;
            }
        }
        d3d12_tex_upload(bt, buf, 4, row32);
        free(buf);
    } else {
        d3d12_tex_upload(bt, sys, src_bpp / 8u, (UINT)src_pitch);
    }
}
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
