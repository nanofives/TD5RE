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
void Backend_BindConstBuffer(UINT slot, BackendConstBuffer *cb) { (void)slot; (void)cb; }
void Backend_BindSampler(UINT slot, int sampler_idx) { (void)slot; (void)sampler_idx; }
int  Backend_BindSceneDepthReadonly(void) { return 0; }
void Backend_CaptureIfRequested(void) { }
BackendConstBuffer *Backend_CreateConstBuffer(size_t size) { (void)size; return NULL; }
BackendPixelShader *Backend_CreatePixelShader(const void *bytecode, size_t len) { (void)bytecode; (void)len; return NULL; }
void Backend_DrainD3DDebug(const char *where) { (void)where; }
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
void *Backend_PixelShaderRaw(BackendPixelShader *ps) { (void)ps; return NULL; }
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
void Backend_ReleaseConstBuffer(BackendConstBuffer *cb) { (void)cb; }
void Backend_ReleasePixelShader(BackendPixelShader *ps) { (void)ps; }
void Backend_RequestCapture(void) { }
int  Backend_Reset(int w, int h, int bpp, int windowed) { (void)w;(void)h;(void)bpp;(void)windowed; return 1; }
void Backend_RestoreMainRenderTarget(void) { }
void Backend_SelectPixelShader(void) { }
void Backend_SetBuiltinPixelShader(int ps_idx) { (void)ps_idx; }
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
void Backend_UpdateConstBuffer(BackendConstBuffer *cb, const void *data, size_t size) { (void)cb;(void)data;(void)size; }
void Backend_UpdateFogCB(void) { }
void Backend_UpdateViewportCB(float w, float h) { (void)w;(void)h; }
