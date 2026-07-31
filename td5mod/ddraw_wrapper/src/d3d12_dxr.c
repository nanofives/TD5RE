/**
 * d3d12_dxr.c -- DirectX Raytracing (DXR) module for the ray-traced lighting
 * stack (LIGHTING QUALITY: HIGH). Wrapper-internal; talks to the D3D12 backend
 * only through d3d12_backend_priv.h (no shared globals). See RT_LIGHTING_PLAN.md.
 *
 * PHASE 0 SCOPE: acquire the DXR pipeline end to end and prove it runs --
 *   - a DXIL state object built from the single lib_6_3 blob (rgen_smoke),
 *   - a frozen global root signature (b0 CBV + u0 UAV table + static sampler),
 *   - a shader binding table (one raygen record, 32-byte identifier),
 *   - an owned shader-visible CBV/SRV/UAV heap,
 *   - a swapchain-sized UAV output texture,
 *   - DispatchRays writing a UV gradient, blitted over the backbuffer.
 * Everything is lazily created on first use and torn down in d3d12_dxr_shutdown.
 *
 * Later phases grow the state object (hit groups, miss/refl/shadow raygens),
 * the heap (bindless textures), and add BLAS/TLAS + G-buffer inputs. The SBT
 * layout, root signatures, and struct layouts are FROZEN in the plan -- follow
 * them; do not redesign.
 */

#define COBJMACROS
#define WIDL_EXPLICIT_AGGREGATE_RETURNS
#include "wrapper.h"
#include <d3d12.h>
#include "d3d12_backend_priv.h"
#include "shaders/rt_pipeline_bytes.h"   /* const unsigned char g_rt_pipeline[] */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- diagnostics ------------------------------------------------------------
 * WRAPPER_LOG is invisible in the standalone port (no wrapper DLL log sink), so
 * DXR bring-up mirrors the backend and flushes to log/d3d12_init.log. */
static void dxr_log(const char *fmt, ...)
{
    FILE *f = fopen("log/d3d12_init.log", "a");
    if (f) { va_list ap; va_start(ap, fmt); fputs("[dxr] ", f); vfprintf(f, fmt, ap); va_end(ap); fputc('\n', f); fflush(f); fclose(f); }
}

/* ---- module state ---------------------------------------------------------- */

#define DXR_HEAP_SLOTS       8      /* Phase 0: [0]=output UAV, [1]=output SRV   */
#define DXR_SBT_RAYGEN_OFF   0
#define DXR_SHADER_ID_SIZE   D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES   /* 32       */
#define DXR_SBT_BYTES        256    /* one 64-aligned region is plenty for P0    */

typedef struct {
    ID3D12Device5              *device5;    /* cached from the backend (not owned) */
    ID3D12GraphicsCommandList4 *list4;      /* current frame list4 (not owned)     */

    int                         inited;     /* pipeline objects built              */
    int                         disabled;   /* TD5RE_RT_DISABLE latched            */

    ID3D12RootSignature        *global_rs;  /* b0 CBV + u0 table + static sampler  */
    ID3D12StateObject          *so;         /* raytracing pipeline state object    */
    ID3D12Resource             *sbt;        /* UPLOAD shader binding table         */
    D3D12_GPU_VIRTUAL_ADDRESS   sbt_va;

    ID3D12DescriptorHeap       *heap;       /* shader-visible CBV/SRV/UAV, owned    */
    UINT                        heap_inc;   /* descriptor increment                 */

    /* Blit (UAV -> backbuffer) -- dxr-owned so we never touch the backend heap. */
    ID3D12RootSignature        *blit_rs;
    ID3D12PipelineState        *blit_pso;

    /* Output UAV texture (swapchain-sized). */
    ID3D12Resource             *output;
    UINT                        out_w, out_h;
    D3D12_RESOURCE_STATES       out_state;

    int                         smoke;      /* TD5RE_RT_SMOKE latched (-1 = unread) */
} DxrState;

static DxrState g_dxr = { 0 };

/* ---- descriptor-handle helpers (aggregate-return -> explicit out-param) ----- */

static D3D12_CPU_DESCRIPTOR_HANDLE dxr_cpu(ID3D12DescriptorHeap *h, UINT idx)
{
    D3D12_CPU_DESCRIPTOR_HANDLE c;
    h->lpVtbl->GetCPUDescriptorHandleForHeapStart(h, &c);
    c.ptr += (SIZE_T)idx * g_dxr.heap_inc;
    return c;
}
static D3D12_GPU_DESCRIPTOR_HANDLE dxr_gpu(ID3D12DescriptorHeap *h, UINT idx)
{
    D3D12_GPU_DESCRIPTOR_HANDLE g;
    h->lpVtbl->GetGPUDescriptorHandleForHeapStart(h, &g);
    g.ptr += (UINT64)idx * g_dxr.heap_inc;
    return g;
}

static void dxr_barrier(ID3D12GraphicsCommandList *cl, ID3D12Resource *res,
                        D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER b;
    if (before == after) return;
    ZeroMemory(&b, sizeof(b));
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource   = res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter  = after;
    ID3D12GraphicsCommandList_ResourceBarrier(cl, 1, &b);
}
static void dxr_uav_barrier(ID3D12GraphicsCommandList *cl, ID3D12Resource *res)
{
    D3D12_RESOURCE_BARRIER b;
    ZeroMemory(&b, sizeof(b));
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    b.UAV.pResource = res;
    ID3D12GraphicsCommandList_ResourceBarrier(cl, 1, &b);
}

/* ---- capability ------------------------------------------------------------ */

void d3d12_dxr_on_device(ID3D12Device5 *dev5, ID3D12GraphicsCommandList4 *list4)
{
    /* Device (re)created: cached interfaces change; any previously built
     * pipeline objects belong to the old device and were already released by
     * d3d12_dxr_shutdown during teardown. Reset lazy-init. */
    g_dxr.device5 = dev5;
    g_dxr.list4   = list4;
    g_dxr.inited  = 0;
    if (g_dxr.smoke == 0) g_dxr.smoke = -1;   /* re-read env after a fresh device */
    dxr_log("on_device dev5=%p list4=%p", (void *)dev5, (void *)list4);
}

int d3d12_dxr_available(void)
{
    if (g_dxr.disabled) return 0;
    return g_dxr.device5 != NULL;
}

int d3d12_dxr_smoke_enabled(void)
{
    if (g_dxr.smoke < 0) {
        const char *e = getenv("TD5RE_RT_SMOKE");
        g_dxr.smoke = (e && e[0] && e[0] != '0') ? 1 : 0;
    }
    return g_dxr.smoke;
}

/* ---- pipeline construction (lazy) ------------------------------------------ */

static int dxr_create_global_rs(void)
{
    D3D12_DESCRIPTOR_RANGE uav_range;
    D3D12_ROOT_PARAMETER params[2];
    D3D12_STATIC_SAMPLER_DESC samp;
    D3D12_ROOT_SIGNATURE_DESC rsd;
    ID3D10Blob *sig = NULL, *err = NULL;
    HRESULT hr;

    ZeroMemory(&uav_range, sizeof(uav_range));
    uav_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uav_range.NumDescriptors = 1;                 /* u0 output (Phase 0)          */
    uav_range.BaseShaderRegister = 0;
    uav_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    ZeroMemory(params, sizeof(params));
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;   /* b0 per-dispatch */
    params[0].Descriptor.ShaderRegister = 0;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &uav_range;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    ZeroMemory(&samp, sizeof(samp));
    samp.Filter   = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samp.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    samp.MaxLOD = D3D12_FLOAT32_MAX;
    samp.ShaderRegister = 0;
    samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    ZeroMemory(&rsd, sizeof(rsd));
    rsd.NumParameters = 2; rsd.pParameters = params;
    rsd.NumStaticSamplers = 1; rsd.pStaticSamplers = &samp;
    rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;   /* DXR global RS: no DENY flags */

    hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err);
    if (FAILED(hr)) {
        dxr_log("global rootsig serialize 0x%08lX: %s", hr,
                err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "(no blob)");
        if (err) ID3D10Blob_Release(err);
        return 0;
    }
    hr = ID3D12Device_CreateRootSignature((ID3D12Device *)g_dxr.device5, 0,
            ID3D10Blob_GetBufferPointer(sig), ID3D10Blob_GetBufferSize(sig),
            &IID_ID3D12RootSignature, (void **)&g_dxr.global_rs);
    ID3D10Blob_Release(sig);
    if (err) ID3D10Blob_Release(err);
    if (FAILED(hr)) { dxr_log("global rootsig create 0x%08lX", hr); return 0; }
    return 1;
}

static int dxr_create_state_object(void)
{
    D3D12_DXIL_LIBRARY_DESC lib;
    D3D12_EXPORT_DESC        exports[1];
    D3D12_RAYTRACING_SHADER_CONFIG   shcfg;
    D3D12_RAYTRACING_PIPELINE_CONFIG pcfg;
    D3D12_GLOBAL_ROOT_SIGNATURE      grs;
    D3D12_STATE_SUBOBJECT so[4];
    D3D12_STATE_OBJECT_DESC sod;
    HRESULT hr;

    ZeroMemory(exports, sizeof(exports));
    exports[0].Name = L"rgen_smoke";
    ZeroMemory(&lib, sizeof(lib));
    lib.DXILLibrary.pShaderBytecode = g_rt_pipeline;
    lib.DXILLibrary.BytecodeLength  = sizeof(g_rt_pipeline);
    lib.NumExports = 1; lib.pExports = exports;

    shcfg.MaxPayloadSizeInBytes   = 32;   /* frozen (plan sec.4) */
    shcfg.MaxAttributeSizeInBytes = 8;
    pcfg.MaxTraceRecursionDepth   = 1;    /* Phase 0: no TraceRay */
    grs.pGlobalRootSignature      = g_dxr.global_rs;

    ZeroMemory(so, sizeof(so));
    so[0].Type = D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY;              so[0].pDesc = &lib;
    so[1].Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG;  so[1].pDesc = &shcfg;
    so[2].Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG;so[2].pDesc = &pcfg;
    so[3].Type = D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE;     so[3].pDesc = &grs;

    ZeroMemory(&sod, sizeof(sod));
    sod.Type = D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE;
    sod.NumSubobjects = 4; sod.pSubobjects = so;

    hr = ID3D12Device5_CreateStateObject(g_dxr.device5, &sod, &IID_ID3D12StateObject, (void **)&g_dxr.so);
    if (FAILED(hr)) { dxr_log("CreateStateObject 0x%08lX", hr); return 0; }
    return 1;
}

static int dxr_create_sbt(void)
{
    ID3D12StateObjectProperties *props = NULL;
    D3D12_HEAP_PROPERTIES hp;
    D3D12_RESOURCE_DESC   bd;
    const void *id;
    void  *mapped = NULL;
    HRESULT hr;

    hr = ID3D12StateObject_QueryInterface(g_dxr.so, &IID_ID3D12StateObjectProperties, (void **)&props);
    if (FAILED(hr) || !props) { dxr_log("QI StateObjectProperties 0x%08lX", hr); return 0; }
    id = ID3D12StateObjectProperties_GetShaderIdentifier(props, L"rgen_smoke");
    if (!id) { dxr_log("GetShaderIdentifier(rgen_smoke) NULL"); ID3D12StateObjectProperties_Release(props); return 0; }

    ZeroMemory(&hp, sizeof(hp)); hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    ZeroMemory(&bd, sizeof(bd));
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = DXR_SBT_BYTES; bd.Height = 1;
    bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.Format = DXGI_FORMAT_UNKNOWN;
    bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    hr = ID3D12Device_CreateCommittedResource((ID3D12Device *)g_dxr.device5, &hp, D3D12_HEAP_FLAG_NONE, &bd,
            D3D12_RESOURCE_STATE_GENERIC_READ, NULL, &IID_ID3D12Resource, (void **)&g_dxr.sbt);
    if (FAILED(hr)) { dxr_log("SBT CreateCommittedResource 0x%08lX", hr); ID3D12StateObjectProperties_Release(props); return 0; }
    { D3D12_RANGE r; r.Begin = 0; r.End = 0; ID3D12Resource_Map(g_dxr.sbt, 0, &r, &mapped); }
    if (mapped) {
        memset(mapped, 0, DXR_SBT_BYTES);
        memcpy((unsigned char *)mapped + DXR_SBT_RAYGEN_OFF, id, DXR_SHADER_ID_SIZE);
        { D3D12_RANGE wr; wr.Begin = 0; wr.End = DXR_SBT_BYTES; ID3D12Resource_Unmap(g_dxr.sbt, 0, &wr); }
    }
    g_dxr.sbt_va = ID3D12Resource_GetGPUVirtualAddress(g_dxr.sbt);
    ID3D12StateObjectProperties_Release(props);
    return 1;
}

static int dxr_create_heap(void)
{
    D3D12_DESCRIPTOR_HEAP_DESC hd;
    ZeroMemory(&hd, sizeof(hd));
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = DXR_HEAP_SLOTS;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(ID3D12Device_CreateDescriptorHeap((ID3D12Device *)g_dxr.device5, &hd,
            &IID_ID3D12DescriptorHeap, (void **)&g_dxr.heap))) { dxr_log("CreateDescriptorHeap FAILED"); return 0; }
    g_dxr.heap_inc = ID3D12Device_GetDescriptorHandleIncrementSize((ID3D12Device *)g_dxr.device5,
                        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    return 1;
}

static int dxr_create_blit(void)
{
    D3D12_DESCRIPTOR_RANGE srv_range;
    D3D12_ROOT_PARAMETER param;
    D3D12_STATIC_SAMPLER_DESC samp;
    D3D12_ROOT_SIGNATURE_DESC rsd;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd;
    ID3D10Blob *sig = NULL, *err = NULL;
    const void *vs = NULL, *ps = NULL; SIZE_T vs_len = 0, ps_len = 0;
    HRESULT hr;

    ZeroMemory(&srv_range, sizeof(srv_range));
    srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srv_range.NumDescriptors = 1; srv_range.BaseShaderRegister = 0;   /* t0 */
    srv_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    ZeroMemory(&param, sizeof(param));
    param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    param.DescriptorTable.NumDescriptorRanges = 1;
    param.DescriptorTable.pDescriptorRanges = &srv_range;
    param.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    ZeroMemory(&samp, sizeof(samp));
    samp.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    samp.MaxLOD = D3D12_FLOAT32_MAX;
    samp.ShaderRegister = 0; samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    ZeroMemory(&rsd, sizeof(rsd));
    rsd.NumParameters = 1; rsd.pParameters = &param;
    rsd.NumStaticSamplers = 1; rsd.pStaticSamplers = &samp;
    rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_DENY_VERTEX_SHADER_ROOT_ACCESS;

    hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err);
    if (FAILED(hr)) {
        dxr_log("blit rootsig serialize 0x%08lX: %s", hr,
                err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "(no blob)");
        if (err) ID3D10Blob_Release(err);
        return 0;
    }
    hr = ID3D12Device_CreateRootSignature((ID3D12Device *)g_dxr.device5, 0,
            ID3D10Blob_GetBufferPointer(sig), ID3D10Blob_GetBufferSize(sig),
            &IID_ID3D12RootSignature, (void **)&g_dxr.blit_rs);
    ID3D10Blob_Release(sig);
    if (err) ID3D10Blob_Release(err);
    if (FAILED(hr)) { dxr_log("blit rootsig create 0x%08lX", hr); return 0; }

    d3d12_priv_fullscreen_shaders(&vs, &vs_len, &ps, &ps_len);
    if (!vs || !ps) { dxr_log("blit shaders unavailable"); return 0; }
    ZeroMemory(&pd, sizeof(pd));
    pd.pRootSignature = g_dxr.blit_rs;
    pd.VS.pShaderBytecode = vs; pd.VS.BytecodeLength = vs_len;
    pd.PS.pShaderBytecode = ps; pd.PS.BytecodeLength = ps_len;
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = 0xFFFFFFFFu;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1; pd.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM;
    pd.DSVFormat = DXGI_FORMAT_UNKNOWN;
    pd.SampleDesc.Count = 1;
    if (FAILED(ID3D12Device_CreateGraphicsPipelineState((ID3D12Device *)g_dxr.device5, &pd,
            &IID_ID3D12PipelineState, (void **)&g_dxr.blit_pso))) { dxr_log("blit PSO FAILED"); return 0; }
    return 1;
}

/* (Re)create the output UAV texture + its UAV/SRV descriptors at w x h. */
static int dxr_ensure_output(int w, int h)
{
    D3D12_HEAP_PROPERTIES hp;
    D3D12_RESOURCE_DESC   rd;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav;
    D3D12_SHADER_RESOURCE_VIEW_DESC  srv;
    HRESULT hr;
    if (g_dxr.output && g_dxr.out_w == (UINT)w && g_dxr.out_h == (UINT)h) return 1;
    if (g_dxr.output) { d3d12_priv_retire(g_dxr.output); g_dxr.output = NULL; }

    ZeroMemory(&hp, sizeof(hp)); hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    ZeroMemory(&rd, sizeof(rd));
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = (UINT64)w; rd.Height = (UINT)h; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;   /* typed-UAV safe (unlike BGRA8)    */
    rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    hr = ID3D12Device_CreateCommittedResource((ID3D12Device *)g_dxr.device5, &hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, NULL, &IID_ID3D12Resource, (void **)&g_dxr.output);
    if (FAILED(hr)) { dxr_log("output tex create 0x%08lX (%dx%d)", hr, w, h); return 0; }
    g_dxr.out_w = (UINT)w; g_dxr.out_h = (UINT)h;
    g_dxr.out_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    ZeroMemory(&uav, sizeof(uav));
    uav.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    ID3D12Device_CreateUnorderedAccessView((ID3D12Device *)g_dxr.device5, g_dxr.output, NULL, &uav,
            dxr_cpu(g_dxr.heap, 0));   /* slot 0 = u0 */

    ZeroMemory(&srv, sizeof(srv));
    srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    ID3D12Device_CreateShaderResourceView((ID3D12Device *)g_dxr.device5, g_dxr.output, &srv,
            dxr_cpu(g_dxr.heap, 1));   /* slot 1 = blit SRV */
    return 1;
}

static int dxr_ensure_init(void)
{
    if (g_dxr.inited) return 1;
    if (!g_dxr.device5) return 0;
    if (!dxr_create_heap())        goto fail;
    if (!dxr_create_global_rs())   goto fail;
    if (!dxr_create_state_object())goto fail;
    if (!dxr_create_sbt())         goto fail;
    if (!dxr_create_blit())        goto fail;
    g_dxr.inited = 1;
    dxr_log("pipeline init OK (state object + SBT + heap + blit)");
    return 1;
fail:
    dxr_log("pipeline init FAILED");
    d3d12_dxr_shutdown();
    return 0;
}

/* ---- Phase 0 smoke: DispatchRays gradient + blit over the backbuffer -------- */

void d3d12_dxr_smoke_blit(void)
{
    d3d12_dxr_env e;
    ID3D12GraphicsCommandList  *cl;
    ID3D12GraphicsCommandList4 *cl4;
    ID3D12DescriptorHeap *heaps[1];
    D3D12_DISPATCH_RAYS_DESC d;
    D3D12_VIEWPORT vp;
    D3D12_RECT sc;

    d3d12_priv_env(&e);
    if (g_dxr.disabled || !e.device5 || !e.list4 || !e.frame_open) return;
    g_dxr.device5 = e.device5;
    g_dxr.list4   = e.list4;
    if (!dxr_ensure_init()) return;
    if (!dxr_ensure_output(e.width, e.height)) return;
    cl  = e.list;
    cl4 = e.list4;

    /* --- DispatchRays: fill the output UAV --- */
    if (g_dxr.out_state != D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
        dxr_barrier(cl, g_dxr.output, g_dxr.out_state, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        g_dxr.out_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }
    heaps[0] = g_dxr.heap;
    ID3D12GraphicsCommandList_SetDescriptorHeaps(cl, 1, heaps);
    ID3D12GraphicsCommandList_SetComputeRootSignature(cl, g_dxr.global_rs);
    ID3D12GraphicsCommandList4_SetPipelineState1(cl4, g_dxr.so);
    /* param 0 = b0 CBV (unused by rgen_smoke, but the RS declares it: bind the
     * SBT VA as a harmless valid CBV so the root arg is initialized). */
    ID3D12GraphicsCommandList_SetComputeRootConstantBufferView(cl, 0, g_dxr.sbt_va);
    ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(cl, 1, dxr_gpu(g_dxr.heap, 0));

    ZeroMemory(&d, sizeof(d));
    d.RayGenerationShaderRecord.StartAddress = g_dxr.sbt_va + DXR_SBT_RAYGEN_OFF;
    d.RayGenerationShaderRecord.SizeInBytes  = DXR_SHADER_ID_SIZE;
    /* No miss / hit-group tables in Phase 0 (rgen_smoke issues no TraceRay). */
    d.Width = (UINT)e.width; d.Height = (UINT)e.height; d.Depth = 1;
    ID3D12GraphicsCommandList4_DispatchRays(cl4, &d);
    dxr_uav_barrier(cl, g_dxr.output);

    /* --- Blit the UAV over the current backbuffer --- */
    dxr_barrier(cl, g_dxr.output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    g_dxr.out_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

    ID3D12GraphicsCommandList_OMSetRenderTargets(cl, 1, &e.rtv, FALSE, NULL);
    ID3D12GraphicsCommandList_SetDescriptorHeaps(cl, 1, heaps);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(cl, g_dxr.blit_rs);
    ID3D12GraphicsCommandList_SetPipelineState(cl, g_dxr.blit_pso);
    ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(cl, 0, dxr_gpu(g_dxr.heap, 1));
    vp.TopLeftX = 0.0f; vp.TopLeftY = 0.0f; vp.Width = (float)e.width; vp.Height = (float)e.height;
    vp.MinDepth = 0.0f; vp.MaxDepth = 1.0f;
    sc.left = 0; sc.top = 0; sc.right = e.width; sc.bottom = e.height;
    ID3D12GraphicsCommandList_RSSetViewports(cl, 1, &vp);
    ID3D12GraphicsCommandList_RSSetScissorRects(cl, 1, &sc);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(cl, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D12GraphicsCommandList_DrawInstanced(cl, 3, 1, 0, 0);
}

/* ---- teardown -------------------------------------------------------------- */

void d3d12_dxr_shutdown(void)
{
    if (g_dxr.output)    { ID3D12Resource_Release(g_dxr.output);       g_dxr.output = NULL; }
    if (g_dxr.blit_pso)  { ID3D12PipelineState_Release(g_dxr.blit_pso);g_dxr.blit_pso = NULL; }
    if (g_dxr.blit_rs)   { ID3D12RootSignature_Release(g_dxr.blit_rs); g_dxr.blit_rs = NULL; }
    if (g_dxr.sbt)       { ID3D12Resource_Release(g_dxr.sbt);          g_dxr.sbt = NULL; }
    if (g_dxr.so)        { ID3D12StateObject_Release(g_dxr.so);        g_dxr.so = NULL; }
    if (g_dxr.global_rs) { ID3D12RootSignature_Release(g_dxr.global_rs);g_dxr.global_rs = NULL; }
    if (g_dxr.heap)      { ID3D12DescriptorHeap_Release(g_dxr.heap);   g_dxr.heap = NULL; }
    g_dxr.out_w = g_dxr.out_h = 0;
    g_dxr.sbt_va = 0;
    g_dxr.inited = 0;
    /* device5/list4 are backend-owned; do not release here. */
}
