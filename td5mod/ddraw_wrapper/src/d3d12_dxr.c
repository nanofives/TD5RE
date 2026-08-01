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
#include "shaders/ps_shadow_rt_bytes_50.h"  /* MULT sun-shadow composite (this TU only) */
#include "shaders/ps_light_rt_bytes_50.h"   /* additive light composite  (this TU only) */
#include "shaders/ps_ssr_rt_bytes_50.h"     /* reflection composite      (this TU only) */

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

/* Shader-visible heap slots (fixed):
 *   0 = output UAV (u0)      1 = TLAS SRV (t0)      2 = blit SRV
 *   3 = VB pool SRV (t, P3)  4 = IB pool SRV (t, P3)  [16..] bindless (P3) */
/* Heap: UAV range u0-u2 (slots 0..2) + SRV range t0-t2 (slots 3..5) form the
 * global RS descriptor table; blit/composite SRVs live past it. */
#define DXR_HEAP_SLOTS        4096
#define DXR_SLOT_OUTPUT_UAV   0   /* u0 debug/smoke   */
#define DXR_SLOT_SUNVIS_UAV   1   /* u1 sun shade     */
#define DXR_SLOT_LIGHTCOL_UAV 2   /* u2 light color   */
#define DXR_SLOT_TLAS_SRV     3   /* t0 TLAS          */
#define DXR_SLOT_DEPTH_SRV    4   /* t1 depth R32F    */
#define DXR_SLOT_GBUF_SRV     5   /* t2 gbuffer       */
#define DXR_SLOT_BLIT_SRV     6   /* debug output blit */
#define DXR_SLOT_SUNVIS_SRV   7   /* shadow composite  */
#define DXR_SLOT_LIGHTCOL_SRV 8   /* light composite   */
#define DXR_SLOT_REFLCOL_UAV  9   /* u3 reflection color */
#define DXR_SLOT_VB_SRV       10  /* t3 vertex pool    */
#define DXR_SLOT_IB_SRV       11  /* t4 index pool     */
#define DXR_SLOT_GEO_SRV      12  /* t5 GeoRecord      */
#define DXR_SLOT_REFLCOL_SRV  13  /* reflection composite */
#define DXR_SLOT_TABLE_BASE   0   /* table 0 gpu handle = slot 0 */

#define DXR_SHADER_ID_SIZE   D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES   /* 32       */
#define DXR_REGION_ALIGN     D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT /* 64  */

/* SBT layout (plan sec.4): [raygen | miss | hitgroup]. Each REGION start is
 * 64-aligned. The single RayGenerationShaderRecord passed to DispatchRays must
 * ALSO be 64-aligned, so raygen records are strided by 64 (miss/hitgroup records
 * use the 32-byte record stride within their tables). Raygen slots: [0]=smoke,
 * [1]=debug (reserved [2]=shadow, [3]=refl). Miss: [0]=miss_shadow, [1]=miss_refl.
 * Hitgroup: [0]="hg". */
#define DXR_RAYGEN_SMOKE      0
#define DXR_RAYGEN_DEBUG      1
#define DXR_RAYGEN_SHADOW     2
#define DXR_RAYGEN_LIGHT      3
#define DXR_RAYGEN_REFL       4
#define DXR_RAYGEN_COUNT      5
#define DXR_MISS_SHADOW       0
#define DXR_MISS_REFL         1
#define DXR_MISS_COUNT        2
#define DXR_HITGROUP_COUNT    1

#define DXR_SBT_RAYGEN_STRIDE 64     /* each raygen record 64-aligned (dispatch req) */
#define DXR_SBT_RAYGEN_OFF    0
#define DXR_SBT_MISS_OFF      320    /* 5 raygen slots * 64 = 320 (64-aligned)       */
#define DXR_SBT_HITGROUP_OFF  384    /* miss region (2*32=64) rounds to 64 -> 384    */
#define DXR_SBT_BYTES         512

/* Max instances the TLAS is sized for (plan: capacity 128 up front). */
#define DXR_MAX_INSTANCES     128
/* TDR guard: BLAS triangles built per frame during the warmup window. */
#define DXR_BLAS_TRIS_PER_FRAME 500000
#define DXR_MAX_MESHES        512

typedef struct {
    ID3D12Resource *blas;          /* result buffer (owned)                     */
    D3D12_GPU_VIRTUAL_ADDRESS blas_va;
    ID3D12Resource *staging;       /* UPLOAD staging for VB+IB (retired after copy) */
    UINT   vb_offset, ib_offset;   /* byte offsets into the pools               */
    UINT   vb_bytes,  ib_bytes;    /* staging copy sizes                        */
    UINT   nverts, nidx;
    UINT   nranges;
    BackendRTRange *ranges;        /* nranges entries (malloc'd copy)           */
    UINT   tri_count;
    int    used;                   /* slot occupied                             */
    int    built;                  /* BLAS built (else pending)                 */
    int    needs_copy;             /* VB/IB not yet copied into the pools        */
} DxrMesh;

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
    ID3D12PipelineState        *blit_pso;         /* opaque (debug view)          */

    /* P2b: sun-shadow + dynamic-light masks (UAV write + SRV composite) + the
     * MULT/additive composite PSOs (share blit_rs). */
    ID3D12Resource             *sunvis;           /* R32_FLOAT                    */
    ID3D12Resource             *lightcol;         /* R16G16B16A16_FLOAT           */
    UINT                        mask_w, mask_h;
    D3D12_RESOURCE_STATES       sunvis_state, lightcol_state;
    ID3D12PipelineState        *shadow_pso;       /* ps_shadow_rt, MULT           */
    ID3D12PipelineState        *light_pso;        /* ps_light_rt, additive        */

    /* P3 reflections: reflcol mask, composite PSO, GeoRecord (UPLOAD, mesh-indexed)
     * + VB/IB/GeoRecord SRVs (created once the pools exist). */
    ID3D12Resource             *reflcol;          /* R16G16B16A16_FLOAT rgb + weight */
    D3D12_RESOURCE_STATES       reflcol_state;
    ID3D12PipelineState        *refl_pso;         /* ps_ssr_rt, SRCALPHA_INVSRC   */
    ID3D12Resource             *geo_buf;          /* UPLOAD, GeoRecord[DXR_MAX_MESHES] */
    void                       *geo_mapped;
    int                         p3_srvs_ready;    /* vb/ib/geo SRVs created        */

    /* Output UAV texture (swapchain-sized). */
    ID3D12Resource             *output;
    UINT                        out_w, out_h;
    D3D12_RESOURCE_STATES       out_state;

    int                         smoke;      /* TD5RE_RT_SMOKE latched (-1 = unread) */
    int                         debugview;  /* TD5RE_RT_DEBUGVIEW latched (-1)      */

    /* ---- Phase 1: acceleration structures ---- */
    unsigned                    generation; /* bumped on device recreation         */

    /* Pooled VB/IB (DEFAULT ByteAddressBuffers; grown geometrically). */
    ID3D12Resource             *vb_pool, *ib_pool;
    UINT                        vb_cap, ib_cap;    /* bytes                        */
    UINT                        vb_used, ib_used;
    D3D12_RESOURCE_STATES       vb_state, ib_state;

    /* Shared growable BLAS scratch. */
    ID3D12Resource             *blas_scratch;
    UINT64                      blas_scratch_cap;

    /* Mesh registry (handle = index+1). */
    DxrMesh                     meshes[DXR_MAX_MESHES];

    /* TLAS (double-buffered result + per-frame instance-desc upload + scratch). */
    ID3D12Resource             *tlas[2];
    UINT64                      tlas_cap;          /* result bytes per buffer      */
    ID3D12Resource             *tlas_scratch;
    UINT64                      tlas_scratch_cap;
    ID3D12Resource             *inst_upload[2];    /* D3D12_RAYTRACING_INSTANCE_DESC[] */
    int                         tlas_parity;       /* 0/1                          */
    int                         tlas_valid;        /* a TLAS has been built         */

    /* Scene assembly (Backend_RTSceneBegin..End). */
    D3D12_RAYTRACING_INSTANCE_DESC *scene_inst;    /* points into inst_upload[cur] */
    UINT                        scene_count;
    int                         scene_open;

    /* Per-frame view constants (mirror of rt_common.hlsli RTViewCB). */
    struct {
        float camPos[3];   float focal;
        float right[3];    float centerX;
        float up[3];       float centerY;
        float fwd[3];      float rayTMin;
        float sunDir[3];   float rayTMax;
        float paneOrigin[2];
        float paneSize[2];
    } view;
    int                         have_view;
} DxrState;

static DxrState g_dxr = { 0 };

static int dxr_ensure_init(void);   /* forward: used by the AS feed below */

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
    D3D12_DESCRIPTOR_RANGE ranges[4];
    D3D12_ROOT_PARAMETER params[5];
    D3D12_STATIC_SAMPLER_DESC samp;
    D3D12_ROOT_SIGNATURE_DESC rsd;
    ID3D10Blob *sig = NULL, *err = NULL;
    HRESULT hr;

    /* Fixed descriptor table 0 (heap-slot offsets): UAV u0-u2 @0, SRV t0-t2 @3
     * (TLAS/depth/gbuffer), UAV u3 @9 (reflcol), SRV t3-t5 @10 (VB/IB/GeoRecord). */
    ZeroMemory(ranges, sizeof(ranges));
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[0].NumDescriptors = 3; ranges[0].BaseShaderRegister = 0;   /* u0-u2 */
    ranges[0].OffsetInDescriptorsFromTableStart = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[1].NumDescriptors = 3; ranges[1].BaseShaderRegister = 0;   /* t0-t2 */
    ranges[1].OffsetInDescriptorsFromTableStart = 3;
    ranges[2].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[2].NumDescriptors = 1; ranges[2].BaseShaderRegister = 3;   /* u3 reflcol */
    ranges[2].OffsetInDescriptorsFromTableStart = DXR_SLOT_REFLCOL_UAV;
    ranges[3].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[3].NumDescriptors = 3; ranges[3].BaseShaderRegister = 3;   /* t3-t5 VB/IB/Geo */
    ranges[3].OffsetInDescriptorsFromTableStart = DXR_SLOT_VB_SRV;

    ZeroMemory(params, sizeof(params));
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;   /* b0 debug view   */
    params[0].Descriptor.ShaderRegister = 0;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;   /* b1 ShadowCB     */
    params[1].Descriptor.ShaderRegister = 1;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;   /* b2 LightCB      */
    params[2].Descriptor.ShaderRegister = 2;
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;   /* b3 SSRCB        */
    params[3].Descriptor.ShaderRegister = 3;
    params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[4].DescriptorTable.NumDescriptorRanges = 4;
    params[4].DescriptorTable.pDescriptorRanges = ranges;
    params[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    ZeroMemory(&samp, sizeof(samp));
    samp.Filter   = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samp.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    samp.MaxLOD = D3D12_FLOAT32_MAX;
    samp.ShaderRegister = 0;
    samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    ZeroMemory(&rsd, sizeof(rsd));
    rsd.NumParameters = 5; rsd.pParameters = params;
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
    D3D12_EXPORT_DESC        exports[8];
    D3D12_HIT_GROUP_DESC     hg;
    D3D12_RAYTRACING_SHADER_CONFIG   shcfg;
    D3D12_RAYTRACING_PIPELINE_CONFIG pcfg;
    D3D12_GLOBAL_ROOT_SIGNATURE      grs;
    D3D12_STATE_SUBOBJECT so[5];
    D3D12_STATE_OBJECT_DESC sod;
    HRESULT hr;

    ZeroMemory(exports, sizeof(exports));
    exports[0].Name = L"rgen_smoke";
    exports[1].Name = L"rgen_debug";
    exports[2].Name = L"chit_refl";
    exports[3].Name = L"miss_shadow";
    exports[4].Name = L"miss_refl";
    exports[5].Name = L"rgen_shadow";
    exports[6].Name = L"rgen_light";
    exports[7].Name = L"rgen_refl";
    ZeroMemory(&lib, sizeof(lib));
    lib.DXILLibrary.pShaderBytecode = g_rt_pipeline;
    lib.DXILLibrary.BytecodeLength  = sizeof(g_rt_pipeline);
    lib.NumExports = 8; lib.pExports = exports;

    /* One hit group "hg" (plan sec.4). Phase 1: closest-hit only; anyhit_cutout
     * added in Phase 3. */
    ZeroMemory(&hg, sizeof(hg));
    hg.HitGroupExport = L"hg";
    hg.Type = D3D12_HIT_GROUP_TYPE_TRIANGLES;
    hg.ClosestHitShaderImport = L"chit_refl";

    shcfg.MaxPayloadSizeInBytes   = 32;   /* frozen (plan sec.4) */
    shcfg.MaxAttributeSizeInBytes = 8;
    pcfg.MaxTraceRecursionDepth   = 1;    /* Phase 1: raygen->1 ray (raised to 2 in P2b/P3) */
    grs.pGlobalRootSignature      = g_dxr.global_rs;

    ZeroMemory(so, sizeof(so));
    so[0].Type = D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY;              so[0].pDesc = &lib;
    so[1].Type = D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP;                 so[1].pDesc = &hg;
    so[2].Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG;  so[2].pDesc = &shcfg;
    so[3].Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG;so[3].pDesc = &pcfg;
    so[4].Type = D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE;     so[4].pDesc = &grs;

    ZeroMemory(&sod, sizeof(sod));
    sod.Type = D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE;
    sod.NumSubobjects = 5; sod.pSubobjects = so;

    hr = ID3D12Device5_CreateStateObject(g_dxr.device5, &sod, &IID_ID3D12StateObject, (void **)&g_dxr.so);
    if (FAILED(hr)) { dxr_log("CreateStateObject 0x%08lX", hr); return 0; }
    return 1;
}

static int dxr_sbt_put(void *base, ID3D12StateObjectProperties *props,
                       UINT off, const wchar_t *name)
{
    const void *id = ID3D12StateObjectProperties_GetShaderIdentifier(props, name);
    if (!id) { dxr_log("GetShaderIdentifier NULL for a record"); return 0; }
    memcpy((unsigned char *)base + off, id, DXR_SHADER_ID_SIZE);
    return 1;
}

static int dxr_create_sbt(void)
{
    ID3D12StateObjectProperties *props = NULL;
    D3D12_HEAP_PROPERTIES hp;
    D3D12_RESOURCE_DESC   bd;
    void  *mapped = NULL;
    int ok = 1;
    HRESULT hr;

    hr = ID3D12StateObject_QueryInterface(g_dxr.so, &IID_ID3D12StateObjectProperties, (void **)&props);
    if (FAILED(hr) || !props) { dxr_log("QI StateObjectProperties 0x%08lX", hr); return 0; }

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
        /* Raygen region: 64-strided so each record's StartAddress is 64-aligned. */
        ok &= dxr_sbt_put(mapped, props, DXR_SBT_RAYGEN_OFF + (UINT)DXR_RAYGEN_SMOKE  * DXR_SBT_RAYGEN_STRIDE, L"rgen_smoke");
        ok &= dxr_sbt_put(mapped, props, DXR_SBT_RAYGEN_OFF + (UINT)DXR_RAYGEN_DEBUG  * DXR_SBT_RAYGEN_STRIDE, L"rgen_debug");
        ok &= dxr_sbt_put(mapped, props, DXR_SBT_RAYGEN_OFF + (UINT)DXR_RAYGEN_SHADOW * DXR_SBT_RAYGEN_STRIDE, L"rgen_shadow");
        ok &= dxr_sbt_put(mapped, props, DXR_SBT_RAYGEN_OFF + (UINT)DXR_RAYGEN_LIGHT  * DXR_SBT_RAYGEN_STRIDE, L"rgen_light");
        ok &= dxr_sbt_put(mapped, props, DXR_SBT_RAYGEN_OFF + (UINT)DXR_RAYGEN_REFL   * DXR_SBT_RAYGEN_STRIDE, L"rgen_refl");
        /* Miss region: [0]=miss_shadow, [1]=miss_refl. */
        ok &= dxr_sbt_put(mapped, props, DXR_SBT_MISS_OFF + 0 * DXR_SHADER_ID_SIZE, L"miss_shadow");
        ok &= dxr_sbt_put(mapped, props, DXR_SBT_MISS_OFF + 1 * DXR_SHADER_ID_SIZE, L"miss_refl");
        /* Hitgroup region: [0]="hg". */
        ok &= dxr_sbt_put(mapped, props, DXR_SBT_HITGROUP_OFF, L"hg");
        { D3D12_RANGE wr; wr.Begin = 0; wr.End = DXR_SBT_BYTES; ID3D12Resource_Unmap(g_dxr.sbt, 0, &wr); }
    } else ok = 0;
    g_dxr.sbt_va = ID3D12Resource_GetGPUVirtualAddress(g_dxr.sbt);
    ID3D12StateObjectProperties_Release(props);
    return ok;
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
    /* Alpha-blend the blit so the debug view (raygen writes alpha 0.65 on hit,
     * 0 on miss) overlays the RT geometry on the raster scene. For the smoke
     * test the output alpha is 1.0 -> fully opaque, so this is harmless there. */
    pd.BlendState.RenderTarget[0].BlendEnable    = TRUE;
    pd.BlendState.RenderTarget[0].SrcBlend       = D3D12_BLEND_SRC_ALPHA;
    pd.BlendState.RenderTarget[0].DestBlend      = D3D12_BLEND_INV_SRC_ALPHA;
    pd.BlendState.RenderTarget[0].BlendOp        = D3D12_BLEND_OP_ADD;
    pd.BlendState.RenderTarget[0].SrcBlendAlpha  = D3D12_BLEND_ONE;
    pd.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    pd.BlendState.RenderTarget[0].BlendOpAlpha   = D3D12_BLEND_OP_ADD;
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
            dxr_cpu(g_dxr.heap, DXR_SLOT_OUTPUT_UAV));

    ZeroMemory(&srv, sizeof(srv));
    srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    ID3D12Device_CreateShaderResourceView((ID3D12Device *)g_dxr.device5, g_dxr.output, &srv,
            dxr_cpu(g_dxr.heap, DXR_SLOT_BLIT_SRV));
    return 1;
}

/* ---- Phase 1: acceleration structures ------------------------------------- */

#define DXR_VB_POOL_BYTES (128u * 1024u * 1024u)
#define DXR_IB_POOL_BYTES ( 48u * 1024u * 1024u)

static ID3D12Resource *dxr_default_buffer(UINT64 size, D3D12_RESOURCE_STATES state,
                                          D3D12_RESOURCE_FLAGS flags)
{
    D3D12_HEAP_PROPERTIES hp;
    D3D12_RESOURCE_DESC   bd;
    ID3D12Resource *res = NULL;
    ZeroMemory(&hp, sizeof(hp)); hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    ZeroMemory(&bd, sizeof(bd));
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = size; bd.Height = 1;
    bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.Format = DXGI_FORMAT_UNKNOWN;
    bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; bd.Flags = flags;
    if (FAILED(ID3D12Device_CreateCommittedResource((ID3D12Device *)g_dxr.device5, &hp,
            D3D12_HEAP_FLAG_NONE, &bd, state, NULL, &IID_ID3D12Resource, (void **)&res)))
        return NULL;
    return res;
}

/* Write a null TLAS SRV into a heap slot (keeps the shared descriptor table
 * valid before any TLAS exists, e.g. the smoke path which never reads it). */
static void dxr_write_null_tlas_srv(UINT slot)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC srv;
    ZeroMemory(&srv, sizeof(srv));
    srv.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.RaytracingAccelerationStructure.Location = 0;   /* null AS */
    ID3D12Device_CreateShaderResourceView((ID3D12Device *)g_dxr.device5, NULL, &srv, dxr_cpu(g_dxr.heap, slot));
}

static void dxr_refresh_tlas_srv(ID3D12Resource *tlas)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC srv;
    ZeroMemory(&srv, sizeof(srv));
    srv.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.RaytracingAccelerationStructure.Location = ID3D12Resource_GetGPUVirtualAddress(tlas);
    ID3D12Device_CreateShaderResourceView((ID3D12Device *)g_dxr.device5, NULL, &srv, dxr_cpu(g_dxr.heap, DXR_SLOT_TLAS_SRV));
}

static int dxr_ensure_pools(void)
{
    if (g_dxr.vb_pool && g_dxr.ib_pool) return 1;
    /* D3D12 creates all buffers effectively in COMMON regardless of the
     * requested initial state (debug-layer info 1328), so create + track as
     * COMMON; the copy path then explicitly transitions COMMON->COPY_DEST->
     * NON_PIXEL_SHADER_RESOURCE (the AS-build input state). Pre-sized to the
     * documented budget (Phase 1 has no geometric growth -- see as-built note). */
    g_dxr.vb_pool = dxr_default_buffer(DXR_VB_POOL_BYTES, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_NONE);
    g_dxr.ib_pool = dxr_default_buffer(DXR_IB_POOL_BYTES, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_NONE);
    if (!g_dxr.vb_pool || !g_dxr.ib_pool) { dxr_log("pool alloc FAILED"); return 0; }
    g_dxr.vb_cap = DXR_VB_POOL_BYTES; g_dxr.ib_cap = DXR_IB_POOL_BYTES;
    g_dxr.vb_used = g_dxr.ib_used = 0;
    g_dxr.vb_state = g_dxr.ib_state = D3D12_RESOURCE_STATE_COMMON;
    return 1;
}

/* P3 GeoRecord (mirror of the HLSL struct): one per mesh (single range each in
 * the Phase 1 feed) -> indexed by mesh slot = InstanceID. */
typedef struct { UINT vb_byte_off, ib_byte_off, texture_index, matid; } DxrGeoRecord;

static void dxr_ensure_geo_buf(void)
{
    D3D12_HEAP_PROPERTIES hp; D3D12_RESOURCE_DESC bd;
    if (g_dxr.geo_buf) return;
    ZeroMemory(&hp, sizeof(hp)); hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    ZeroMemory(&bd, sizeof(bd));
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = (UINT64)DXR_MAX_MESHES * sizeof(DxrGeoRecord);
    bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.Format = DXGI_FORMAT_UNKNOWN;
    bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(ID3D12Device_CreateCommittedResource((ID3D12Device *)g_dxr.device5, &hp, D3D12_HEAP_FLAG_NONE, &bd,
            D3D12_RESOURCE_STATE_GENERIC_READ, NULL, &IID_ID3D12Resource, (void **)&g_dxr.geo_buf))) { dxr_log("geo_buf alloc FAILED"); return; }
    { D3D12_RANGE r; r.Begin = 0; r.End = 0; ID3D12Resource_Map(g_dxr.geo_buf, 0, &r, &g_dxr.geo_mapped); }
    if (g_dxr.geo_mapped) memset(g_dxr.geo_mapped, 0, (size_t)DXR_MAX_MESHES * sizeof(DxrGeoRecord));
}

/* Write the GeoRecord for mesh slot (its single range r=0). */
static void dxr_write_geo(int slot, const DxrMesh *m)
{
    DxrGeoRecord *g;
    dxr_ensure_geo_buf();
    if (!g_dxr.geo_mapped || slot < 0 || slot >= DXR_MAX_MESHES) return;
    g = &((DxrGeoRecord *)g_dxr.geo_mapped)[slot];
    g->vb_byte_off   = m->vb_offset;
    g->ib_byte_off   = m->ib_offset + (m->nranges ? m->ranges[0].first_index * 2u : 0u);
    g->texture_index = m->nranges ? m->ranges[0].texture_id : 0u;
    g->matid         = m->nranges ? m->ranges[0].matid_flags : 0u;
}

static void dxr_free_mesh(DxrMesh *m)
{
    if (m->blas)    { d3d12_priv_retire(m->blas); m->blas = NULL; }
    if (m->staging) { d3d12_priv_retire(m->staging); m->staging = NULL; }
    free(m->ranges); m->ranges = NULL;
    ZeroMemory(m, sizeof(*m));
}

int Backend_RTMeshCreate(const BackendRTVertex *verts, unsigned nverts,
                         const unsigned short *idx, unsigned nidx,
                         const BackendRTRange *ranges, unsigned nranges)
{
    int slot = -1; UINT i;
    UINT vb_bytes, ib_bytes, vb_off, ib_off;
    D3D12_HEAP_PROPERTIES hp; D3D12_RESOURCE_DESC bd;
    ID3D12Resource *staging = NULL; unsigned char *map = NULL;
    DxrMesh *m;

    if (!g_dxr.device5 || g_dxr.disabled) return 0;
    if (!verts || !idx || !ranges || nverts == 0 || nidx == 0 || nranges == 0) return 0;
    if (nidx % 3) return 0;
    if (!g_dxr.inited && !dxr_ensure_init()) return 0;
    if (!dxr_ensure_pools()) return 0;

    for (i = 0; i < DXR_MAX_MESHES; i++) if (!g_dxr.meshes[i].used) { slot = (int)i; break; }
    if (slot < 0) { dxr_log("mesh registry full"); return 0; }

    vb_bytes = nverts * (UINT)sizeof(BackendRTVertex);
    ib_bytes = nidx * 2u;
    vb_off = (g_dxr.vb_used + 15u) & ~15u;
    ib_off = (g_dxr.ib_used + 3u)  & ~3u;
    if ((UINT64)vb_off + vb_bytes > g_dxr.vb_cap || (UINT64)ib_off + ib_bytes > g_dxr.ib_cap) {
        dxr_log("pool overflow (vb %u/%u ib %u/%u) -- mesh dropped",
                vb_off + vb_bytes, g_dxr.vb_cap, ib_off + ib_bytes, g_dxr.ib_cap);
        return 0;
    }

    /* Staging UPLOAD buffer: [verts | indices], copied into the pools at build. */
    ZeroMemory(&hp, sizeof(hp)); hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    ZeroMemory(&bd, sizeof(bd));
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = vb_bytes + ib_bytes; bd.Height = 1;
    bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.Format = DXGI_FORMAT_UNKNOWN;
    bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(ID3D12Device_CreateCommittedResource((ID3D12Device *)g_dxr.device5, &hp, D3D12_HEAP_FLAG_NONE, &bd,
            D3D12_RESOURCE_STATE_GENERIC_READ, NULL, &IID_ID3D12Resource, (void **)&staging))) {
        dxr_log("mesh staging alloc FAILED"); return 0;
    }
    { D3D12_RANGE r; r.Begin = 0; r.End = 0; ID3D12Resource_Map(staging, 0, &r, (void **)&map); }
    if (!map) { ID3D12Resource_Release(staging); return 0; }
    memcpy(map, verts, vb_bytes);
    memcpy(map + vb_bytes, idx, ib_bytes);
    { D3D12_RANGE wr; wr.Begin = 0; wr.End = vb_bytes + ib_bytes; ID3D12Resource_Unmap(staging, 0, &wr); }

    m = &g_dxr.meshes[slot];
    ZeroMemory(m, sizeof(*m));
    m->ranges = (BackendRTRange *)malloc((size_t)nranges * sizeof(BackendRTRange));
    if (!m->ranges) { ID3D12Resource_Release(staging); return 0; }
    memcpy(m->ranges, ranges, (size_t)nranges * sizeof(BackendRTRange));
    m->staging = staging;
    m->vb_offset = vb_off; m->ib_offset = ib_off;
    m->vb_bytes = vb_bytes; m->ib_bytes = ib_bytes;
    m->nverts = nverts; m->nidx = nidx; m->nranges = nranges;
    m->tri_count = nidx / 3;
    m->used = 1; m->built = 0; m->needs_copy = 1;

    g_dxr.vb_used = vb_off + vb_bytes;
    g_dxr.ib_used = ib_off + ib_bytes;
    dxr_write_geo(slot, m);   /* P3: GeoRecord for the reflection hit shading */
    return slot + 1;   /* handle */
}

void Backend_RTMeshDestroy(int handle)
{
    if (handle <= 0 || handle > DXR_MAX_MESHES) return;
    if (!g_dxr.meshes[handle - 1].used) return;
    dxr_free_mesh(&g_dxr.meshes[handle - 1]);
}

/* Copy staged meshes into the pools + build BLASes (chunked by tri budget).
 * Called from Backend_RTSceneEnd inside the open frame. */
static void dxr_build_pending(ID3D12GraphicsCommandList *cl, ID3D12GraphicsCommandList4 *cl4)
{
    UINT i;
    int any_copy = 0;
    UINT budget = DXR_BLAS_TRIS_PER_FRAME;

    if (!dxr_ensure_pools()) return;

    /* --- Phase A: copy all staged meshes into the pools (cheap). --- */
    for (i = 0; i < DXR_MAX_MESHES; i++) if (g_dxr.meshes[i].used && g_dxr.meshes[i].needs_copy) { any_copy = 1; break; }
    if (any_copy) {
        dxr_barrier(cl, g_dxr.vb_pool, g_dxr.vb_state, D3D12_RESOURCE_STATE_COPY_DEST);
        dxr_barrier(cl, g_dxr.ib_pool, g_dxr.ib_state, D3D12_RESOURCE_STATE_COPY_DEST);
        g_dxr.vb_state = g_dxr.ib_state = D3D12_RESOURCE_STATE_COPY_DEST;
        for (i = 0; i < DXR_MAX_MESHES; i++) {
            DxrMesh *m = &g_dxr.meshes[i];
            if (!m->used || !m->needs_copy || !m->staging) continue;
            ID3D12GraphicsCommandList_CopyBufferRegion(cl, g_dxr.vb_pool, m->vb_offset, m->staging, 0, m->vb_bytes);
            ID3D12GraphicsCommandList_CopyBufferRegion(cl, g_dxr.ib_pool, m->ib_offset, m->staging, m->vb_bytes, m->ib_bytes);
            d3d12_priv_retire(m->staging);   /* freed after this frame's GPU work */
            m->staging = NULL; m->needs_copy = 0;
        }
        dxr_barrier(cl, g_dxr.vb_pool, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        dxr_barrier(cl, g_dxr.ib_pool, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        g_dxr.vb_state = g_dxr.ib_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    }

    /* --- Phase B: build BLASes for unbuilt meshes, within the tri budget. --- */
    for (i = 0; i < DXR_MAX_MESHES; i++) {
        DxrMesh *m = &g_dxr.meshes[i];
        D3D12_RAYTRACING_GEOMETRY_DESC *geo;
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs;
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO pi;
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build;
        D3D12_GPU_VIRTUAL_ADDRESS vb_va, ib_va;
        UINT r;
        if (!m->used || m->built || m->needs_copy) continue;
        if (budget != DXR_BLAS_TRIS_PER_FRAME && m->tri_count > budget) continue; /* leave big ones for next frame if some built */

        geo = (D3D12_RAYTRACING_GEOMETRY_DESC *)calloc(m->nranges, sizeof(*geo));
        if (!geo) continue;
        vb_va = ID3D12Resource_GetGPUVirtualAddress(g_dxr.vb_pool) + m->vb_offset;
        ib_va = ID3D12Resource_GetGPUVirtualAddress(g_dxr.ib_pool) + m->ib_offset;
        for (r = 0; r < m->nranges; r++) {
            geo[r].Type  = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
            geo[r].Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;   /* CUTOUT revisited in P3 */
            geo[r].Triangles.VertexBuffer.StartAddress  = vb_va;   /* pos at offset 0 */
            geo[r].Triangles.VertexBuffer.StrideInBytes = sizeof(BackendRTVertex);
            geo[r].Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
            geo[r].Triangles.VertexCount  = m->nverts;
            geo[r].Triangles.IndexBuffer  = ib_va + (UINT64)m->ranges[r].first_index * 2u;
            geo[r].Triangles.IndexFormat  = DXGI_FORMAT_R16_UINT;
            geo[r].Triangles.IndexCount   = m->ranges[r].index_count;
        }
        ZeroMemory(&inputs, sizeof(inputs));
        inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        inputs.NumDescs = m->nranges;
        inputs.pGeometryDescs = geo;

        ZeroMemory(&pi, sizeof(pi));
        ID3D12Device5_GetRaytracingAccelerationStructurePrebuildInfo(g_dxr.device5, &inputs, &pi);
        if (pi.ResultDataMaxSizeInBytes == 0) { free(geo); continue; }

        /* Shared growable scratch. */
        if (g_dxr.blas_scratch_cap < pi.ScratchDataSizeInBytes) {
            if (g_dxr.blas_scratch) d3d12_priv_retire(g_dxr.blas_scratch);
            /* Scratch: created COMMON (buffers ignore initial state) and
             * common-promoted to UAV on the build's use. */
            g_dxr.blas_scratch = dxr_default_buffer(pi.ScratchDataSizeInBytes,
                    D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            g_dxr.blas_scratch_cap = g_dxr.blas_scratch ? pi.ScratchDataSizeInBytes : 0;
        }
        m->blas = dxr_default_buffer(pi.ResultDataMaxSizeInBytes,
                    D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        if (!m->blas || !g_dxr.blas_scratch) { if (m->blas){d3d12_priv_retire(m->blas);m->blas=NULL;} free(geo); continue; }
        m->blas_va = ID3D12Resource_GetGPUVirtualAddress(m->blas);

        ZeroMemory(&build, sizeof(build));
        build.DestAccelerationStructureData    = m->blas_va;
        build.Inputs                           = inputs;
        build.ScratchAccelerationStructureData = ID3D12Resource_GetGPUVirtualAddress(g_dxr.blas_scratch);
        ID3D12GraphicsCommandList4_BuildRaytracingAccelerationStructure(cl4, &build, 0, NULL);
        dxr_uav_barrier(cl, m->blas);
        dxr_uav_barrier(cl, g_dxr.blas_scratch);   /* shared scratch reuse fence */
        free(geo);

        m->built = 1;
        if (m->tri_count >= budget) budget = 0; else budget -= m->tri_count;
        /* RTMARK crumb for crash.log forensics. */
        Backend_NoteRTMark("blas_build");
        if (budget == 0) break;
    }
}

/* ---- TLAS assembly -------------------------------------------------------- */

void Backend_RTSceneBegin(void)
{
    d3d12_dxr_env e;
    int cur;
    if (!g_dxr.device5 || g_dxr.disabled) return;
    if (!g_dxr.inited && !dxr_ensure_init()) return;
    d3d12_priv_env(&e);
    if (!e.frame_open) return;
    cur = g_dxr.tlas_parity;

    /* Per-frame instance-desc upload (double-buffered). */
    if (!g_dxr.inst_upload[cur]) {
        D3D12_HEAP_PROPERTIES hp; D3D12_RESOURCE_DESC bd;
        ZeroMemory(&hp, sizeof(hp)); hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        ZeroMemory(&bd, sizeof(bd));
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = (UINT64)DXR_MAX_INSTANCES * sizeof(D3D12_RAYTRACING_INSTANCE_DESC);
        bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.Format = DXGI_FORMAT_UNKNOWN;
        bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(ID3D12Device_CreateCommittedResource((ID3D12Device *)g_dxr.device5, &hp, D3D12_HEAP_FLAG_NONE, &bd,
                D3D12_RESOURCE_STATE_GENERIC_READ, NULL, &IID_ID3D12Resource, (void **)&g_dxr.inst_upload[cur]))) {
            dxr_log("inst upload alloc FAILED"); g_dxr.scene_open = 0; return;
        }
    }
    { D3D12_RANGE r; r.Begin = 0; r.End = 0; ID3D12Resource_Map(g_dxr.inst_upload[cur], 0, &r, (void **)&g_dxr.scene_inst); }
    g_dxr.scene_count = 0;
    g_dxr.scene_open = (g_dxr.scene_inst != NULL);
}

void Backend_RTSceneInstance(int mesh, const float m3x4[12], unsigned flags)
{
    D3D12_RAYTRACING_INSTANCE_DESC *d;
    DxrMesh *m;
    (void)flags;
    if (!g_dxr.scene_open || g_dxr.scene_count >= DXR_MAX_INSTANCES) return;
    if (mesh <= 0 || mesh > DXR_MAX_MESHES) return;
    m = &g_dxr.meshes[mesh - 1];
    if (!m->used || !m->built) return;   /* skip meshes whose BLAS isn't ready */

    d = &g_dxr.scene_inst[g_dxr.scene_count];
    memcpy(d->Transform, m3x4, 12 * sizeof(float));   /* row-major 3x4 */
    d->InstanceID = (UINT)(mesh - 1);                  /* P3: GeoRecord index = mesh slot */
    d->InstanceMask = 0xFF;
    d->InstanceContributionToHitGroupIndex = 0;        /* single hit group */
    d->Flags = D3D12_RAYTRACING_INSTANCE_FLAG_NONE;
    d->AccelerationStructure = m->blas_va;
    g_dxr.scene_count++;
}

void Backend_RTSceneEnd(void)
{
    d3d12_dxr_env e;
    ID3D12GraphicsCommandList  *cl;
    ID3D12GraphicsCommandList4 *cl4;
    int cur;
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs;
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO pi;
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build;

    if (!g_dxr.scene_open) return;
    d3d12_priv_env(&e);
    if (!e.frame_open || !e.list || !e.list4) { g_dxr.scene_open = 0; return; }
    cl = e.list; cl4 = e.list4; cur = g_dxr.tlas_parity;
    if (g_dxr.inst_upload[cur]) { D3D12_RANGE wr; wr.Begin = 0; wr.End = g_dxr.scene_count * sizeof(D3D12_RAYTRACING_INSTANCE_DESC); ID3D12Resource_Unmap(g_dxr.inst_upload[cur], 0, &wr); }
    g_dxr.scene_inst = NULL;

    /* Build BLASes that are still pending (chunked) before the TLAS references them. */
    dxr_build_pending(cl, cl4);

    /* TLAS sized for DXR_MAX_INSTANCES up front. */
    ZeroMemory(&inputs, sizeof(inputs));
    inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
    inputs.NumDescs = g_dxr.scene_count;
    inputs.InstanceDescs = ID3D12Resource_GetGPUVirtualAddress(g_dxr.inst_upload[cur]);

    if (!g_dxr.tlas[cur]) {
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS cap = inputs;
        cap.NumDescs = DXR_MAX_INSTANCES;
        ZeroMemory(&pi, sizeof(pi));
        ID3D12Device5_GetRaytracingAccelerationStructurePrebuildInfo(g_dxr.device5, &cap, &pi);
        g_dxr.tlas[cur] = dxr_default_buffer(pi.ResultDataMaxSizeInBytes,
                D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        if (g_dxr.tlas_scratch_cap < pi.ScratchDataSizeInBytes) {
            if (g_dxr.tlas_scratch) d3d12_priv_retire(g_dxr.tlas_scratch);
            g_dxr.tlas_scratch = dxr_default_buffer(pi.ScratchDataSizeInBytes,
                    D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            g_dxr.tlas_scratch_cap = g_dxr.tlas_scratch ? pi.ScratchDataSizeInBytes : 0;
        }
        if (!g_dxr.tlas[cur] || !g_dxr.tlas_scratch) { dxr_log("TLAS alloc FAILED"); g_dxr.scene_open = 0; return; }
    }

    ZeroMemory(&build, sizeof(build));
    build.DestAccelerationStructureData    = ID3D12Resource_GetGPUVirtualAddress(g_dxr.tlas[cur]);
    build.Inputs                           = inputs;
    build.ScratchAccelerationStructureData = ID3D12Resource_GetGPUVirtualAddress(g_dxr.tlas_scratch);
    ID3D12GraphicsCommandList4_BuildRaytracingAccelerationStructure(cl4, &build, 0, NULL);
    dxr_uav_barrier(cl, g_dxr.tlas[cur]);
    dxr_refresh_tlas_srv(g_dxr.tlas[cur]);
    Backend_NoteRTMark("tlas_build");

    g_dxr.tlas_valid = 1;
    g_dxr.tlas_parity ^= 1;   /* double-buffer for the next frame */
    g_dxr.scene_open = 0;
}

unsigned Backend_RTGeneration(void) { return g_dxr.generation; }

void Backend_RTSetView(const float cam_pos[3], const float basis9[9],
                       float focal, float center_x, float center_y,
                       int pane_x, int pane_y, int pane_w, int pane_h,
                       const float sun_dir[3])
{
    int i;
    if (!cam_pos || !basis9) return;
    for (i = 0; i < 3; i++) {
        g_dxr.view.camPos[i] = cam_pos[i];
        g_dxr.view.right[i]  = basis9[0 + i];
        g_dxr.view.up[i]     = basis9[3 + i];
        g_dxr.view.fwd[i]    = basis9[6 + i];
        g_dxr.view.sunDir[i] = sun_dir ? sun_dir[i] : 0.0f;
    }
    g_dxr.view.focal   = focal;
    g_dxr.view.centerX = center_x;
    g_dxr.view.centerY = center_y;
    g_dxr.view.rayTMin = 0.5f;
    g_dxr.view.rayTMax = 1.0e7f;
    g_dxr.view.paneOrigin[0] = (float)pane_x; g_dxr.view.paneOrigin[1] = (float)pane_y;
    g_dxr.view.paneSize[0]   = (float)pane_w; g_dxr.view.paneSize[1]   = (float)pane_h;
    g_dxr.have_view = 1;
}

/* ---- P2b: RT sun-shadow + dynamic-light passes ---------------------------- */

static ID3D12Resource *dxr_uav_texture(int w, int h, DXGI_FORMAT fmt)
{
    D3D12_HEAP_PROPERTIES hp; D3D12_RESOURCE_DESC rd; ID3D12Resource *r = NULL;
    ZeroMemory(&hp, sizeof(hp)); hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    ZeroMemory(&rd, sizeof(rd));
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; rd.Width = (UINT64)w; rd.Height = (UINT)h;
    rd.DepthOrArraySize = 1; rd.MipLevels = 1; rd.Format = fmt; rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (FAILED(ID3D12Device_CreateCommittedResource((ID3D12Device *)g_dxr.device5, &hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, NULL, &IID_ID3D12Resource, (void **)&r)))
        return NULL;
    return r;
}

/* (Re)create the sun-visibility + light-color mask textures + their UAV/SRV. */
static int dxr_ensure_masks(int w, int h)
{
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv;
    if (g_dxr.sunvis && g_dxr.lightcol && g_dxr.reflcol && g_dxr.mask_w == (UINT)w && g_dxr.mask_h == (UINT)h) return 1;
    if (g_dxr.sunvis)   { d3d12_priv_retire(g_dxr.sunvis);   g_dxr.sunvis = NULL; }
    if (g_dxr.lightcol) { d3d12_priv_retire(g_dxr.lightcol); g_dxr.lightcol = NULL; }
    if (g_dxr.reflcol)  { d3d12_priv_retire(g_dxr.reflcol);  g_dxr.reflcol = NULL; }
    g_dxr.sunvis   = dxr_uav_texture(w, h, DXGI_FORMAT_R32_FLOAT);
    g_dxr.lightcol = dxr_uav_texture(w, h, DXGI_FORMAT_R16G16B16A16_FLOAT);
    g_dxr.reflcol  = dxr_uav_texture(w, h, DXGI_FORMAT_R16G16B16A16_FLOAT);
    if (!g_dxr.sunvis || !g_dxr.lightcol || !g_dxr.reflcol) { dxr_log("mask alloc FAILED"); return 0; }
    g_dxr.mask_w = (UINT)w; g_dxr.mask_h = (UINT)h;
    g_dxr.sunvis_state = g_dxr.lightcol_state = g_dxr.reflcol_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    ZeroMemory(&uav, sizeof(uav)); uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    uav.Format = DXGI_FORMAT_R32_FLOAT;
    ID3D12Device_CreateUnorderedAccessView((ID3D12Device *)g_dxr.device5, g_dxr.sunvis, NULL, &uav, dxr_cpu(g_dxr.heap, DXR_SLOT_SUNVIS_UAV));
    uav.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    ID3D12Device_CreateUnorderedAccessView((ID3D12Device *)g_dxr.device5, g_dxr.lightcol, NULL, &uav, dxr_cpu(g_dxr.heap, DXR_SLOT_LIGHTCOL_UAV));

    ZeroMemory(&srv, sizeof(srv)); srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; srv.Texture2D.MipLevels = 1;
    srv.Format = DXGI_FORMAT_R32_FLOAT;
    ID3D12Device_CreateShaderResourceView((ID3D12Device *)g_dxr.device5, g_dxr.sunvis, &srv, dxr_cpu(g_dxr.heap, DXR_SLOT_SUNVIS_SRV));
    srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    ID3D12Device_CreateShaderResourceView((ID3D12Device *)g_dxr.device5, g_dxr.lightcol, &srv, dxr_cpu(g_dxr.heap, DXR_SLOT_LIGHTCOL_SRV));

    uav.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    ID3D12Device_CreateUnorderedAccessView((ID3D12Device *)g_dxr.device5, g_dxr.reflcol, NULL, &uav, dxr_cpu(g_dxr.heap, DXR_SLOT_REFLCOL_UAV));
    srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    ID3D12Device_CreateShaderResourceView((ID3D12Device *)g_dxr.device5, g_dxr.reflcol, &srv, dxr_cpu(g_dxr.heap, DXR_SLOT_REFLCOL_SRV));
    return 1;
}

/* P3: create the VB/IB (ByteAddressBuffer) + GeoRecord (StructuredBuffer) SRVs
 * into the DXR heap once the pools + geo buffer exist. */
static int dxr_ensure_p3_srvs(void)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC srv;
    if (g_dxr.p3_srvs_ready) return 1;
    if (!g_dxr.vb_pool || !g_dxr.ib_pool) return 0;
    dxr_ensure_geo_buf();
    if (!g_dxr.geo_buf) return 0;

    ZeroMemory(&srv, sizeof(srv));
    srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Format = DXGI_FORMAT_R32_TYPELESS;
    srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    srv.Buffer.NumElements = g_dxr.vb_cap / 4;
    ID3D12Device_CreateShaderResourceView((ID3D12Device *)g_dxr.device5, g_dxr.vb_pool, &srv, dxr_cpu(g_dxr.heap, DXR_SLOT_VB_SRV));
    srv.Buffer.NumElements = g_dxr.ib_cap / 4;
    ID3D12Device_CreateShaderResourceView((ID3D12Device *)g_dxr.device5, g_dxr.ib_pool, &srv, dxr_cpu(g_dxr.heap, DXR_SLOT_IB_SRV));

    ZeroMemory(&srv, sizeof(srv));
    srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Format = DXGI_FORMAT_UNKNOWN;
    srv.Buffer.NumElements = DXR_MAX_MESHES;
    srv.Buffer.StructureByteStride = sizeof(DxrGeoRecord);
    ID3D12Device_CreateShaderResourceView((ID3D12Device *)g_dxr.device5, g_dxr.geo_buf, &srv, dxr_cpu(g_dxr.heap, DXR_SLOT_GEO_SRV));
    g_dxr.p3_srvs_ready = 1;
    return 1;
}

static ID3D12PipelineState *dxr_make_composite_pso(const void *ps, SIZE_T ps_len, int additive)
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd; ID3D12PipelineState *pso = NULL;
    const void *vs = NULL, *cps = NULL; SIZE_T vs_len = 0, cps_len = 0;
    D3D12_RENDER_TARGET_BLEND_DESC *rt;
    d3d12_priv_fullscreen_shaders(&vs, &vs_len, &cps, &cps_len);
    ZeroMemory(&pd, sizeof(pd));
    pd.pRootSignature = g_dxr.blit_rs;
    pd.VS.pShaderBytecode = vs; pd.VS.BytecodeLength = vs_len;
    pd.PS.pShaderBytecode = ps; pd.PS.BytecodeLength = ps_len;
    rt = &pd.BlendState.RenderTarget[0];
    rt->BlendEnable = (additive != 2);   /* 2 = opaque debug (raw mask) */
    if (additive == 1)      { rt->SrcBlend = D3D12_BLEND_ONE;        rt->DestBlend = D3D12_BLEND_ONE; }        /* additive */
    else if (additive == 0) { rt->SrcBlend = D3D12_BLEND_DEST_COLOR; rt->DestBlend = D3D12_BLEND_ZERO; }       /* MULT     */
    else if (additive == 3) { rt->SrcBlend = D3D12_BLEND_SRC_ALPHA;  rt->DestBlend = D3D12_BLEND_INV_SRC_ALPHA; } /* refl alpha */
    rt->BlendOp = D3D12_BLEND_OP_ADD;
    /* Preserve the destination (backbuffer) alpha: out.a = dst.a. The scene
     * arrives opaque (a=1); the reflection shader emits its Fresnel weight in
     * alpha as the COLOR src-blend factor, but that must NOT be written back to
     * the backbuffer alpha -- doing so left the frame with a~0, which the
     * swapchain ignores on present (screen is correct) but a captured framedump
     * PNG renders as transparent -> a phantom "near-white washout". Keeping dst
     * alpha makes framedump-based verification match what the player sees. */
    rt->SrcBlendAlpha = D3D12_BLEND_ZERO; rt->DestBlendAlpha = D3D12_BLEND_ONE; rt->BlendOpAlpha = D3D12_BLEND_OP_ADD;
    rt->RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = 0xFFFFFFFFu;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1; pd.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM;
    pd.DSVFormat = DXGI_FORMAT_UNKNOWN; pd.SampleDesc.Count = 1;
    if (FAILED(ID3D12Device_CreateGraphicsPipelineState((ID3D12Device *)g_dxr.device5, &pd,
            &IID_ID3D12PipelineState, (void **)&pso))) { dxr_log("composite PSO FAILED (add=%d)", additive); return NULL; }
    return pso;
}

/* Shared shadow/light pass: dispatch the raygen against the TLAS (reading depth +
 * G-buffer), then composite the mask over the pane. cb is the game's ShadowCB (b1)
 * or LightCB (b2). Returns 1 if it ran (caller skips the LOW march). */
/* mode: 0 = sun shadow, 1 = dynamic light, 2 = reflection. */
static int dxr_lighting_pass(const void *cb, UINT cb_size, int mode)
{
    d3d12_dxr_env e; d3d12_dxr_scene scene;
    ID3D12GraphicsCommandList  *cl; ID3D12GraphicsCommandList4 *cl4;
    ID3D12DescriptorHeap *heaps[1];
    D3D12_GPU_VIRTUAL_ADDRESS cbva;
    D3D12_DISPATCH_RAYS_DESC d;
    D3D12_SHADER_RESOURCE_VIEW_DESC srvd;
    D3D12_VIEWPORT vp; D3D12_RECT sc;
    const float *f = (const float *)cb;
    int paneX, paneY, paneW, paneH; UINT raygen, cb_param;
    ID3D12Resource *mask; D3D12_RESOURCE_STATES *mask_state; UINT mask_srv; ID3D12PipelineState *comp;

    d3d12_priv_env(&e);
    if (g_dxr.disabled || !e.device5 || !e.list4 || !e.frame_open || !g_dxr.tlas_valid) return 0;
    g_dxr.device5 = e.device5; g_dxr.list4 = e.list4;
    if (!dxr_ensure_init() || !dxr_ensure_masks(e.width, e.height)) return 0;
    if (mode == 2) {
        if (!dxr_ensure_p3_srvs()) return 0;
        if (!g_dxr.refl_pso) g_dxr.refl_pso = dxr_make_composite_pso(g_ps_ssr_rt_50, sizeof(g_ps_ssr_rt_50), 3);
        comp = g_dxr.refl_pso;
        { static int dbg = -1; static ID3D12PipelineState *s_rdbg;
          if (dbg < 0) { const char *ev = getenv("TD5RE_RT_REFLDBG"); dbg = (ev && ev[0] && ev[0] != '0') ? 1 : 0; }
          if (dbg) { if (!s_rdbg) s_rdbg = dxr_make_composite_pso(g_ps_ssr_rt_50, sizeof(g_ps_ssr_rt_50), 2);
                     if (s_rdbg) comp = s_rdbg; } }
    } else if (mode == 1) {
        if (!g_dxr.light_pso)  g_dxr.light_pso  = dxr_make_composite_pso(g_ps_light_rt_50,  sizeof(g_ps_light_rt_50),  1);
        comp = g_dxr.light_pso;
    } else {
        if (!g_dxr.shadow_pso) g_dxr.shadow_pso = dxr_make_composite_pso(g_ps_shadow_rt_50, sizeof(g_ps_shadow_rt_50), 0);
        comp = g_dxr.shadow_pso;
        /* Debug: opaque grayscale mask (TD5RE_RT_MASK) -- shadow pass only. */
        { static int dbg = -1; static ID3D12PipelineState *s_dbgpso;
          if (dbg < 0) { const char *ev = getenv("TD5RE_RT_MASK"); dbg = (ev && ev[0] && ev[0] != '0') ? 1 : 0; }
          if (dbg) { if (!s_dbgpso) s_dbgpso = dxr_make_composite_pso(g_ps_shadow_rt_50, sizeof(g_ps_shadow_rt_50), 2);
                     if (s_dbgpso) comp = s_dbgpso; } }
    }
    if (!comp) return 0;
    if (!d3d12_priv_scene_inputs(&scene)) return 0;   /* needs depth + gbuffer; transitions them */
    cl = e.list; cl4 = e.list4;

    /* depth SRV (R32_FLOAT) t1 / gbuffer SRV (R8G8B8A8) t2 into the DXR heap. */
    ZeroMemory(&srvd, sizeof(srvd)); srvd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; srvd.Texture2D.MipLevels = 1;
    srvd.Format = DXGI_FORMAT_R32_FLOAT;
    ID3D12Device_CreateShaderResourceView((ID3D12Device *)g_dxr.device5, scene.depth, &srvd, dxr_cpu(g_dxr.heap, DXR_SLOT_DEPTH_SRV));
    srvd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    ID3D12Device_CreateShaderResourceView((ID3D12Device *)g_dxr.device5, scene.gbuffer, &srvd, dxr_cpu(g_dxr.heap, DXR_SLOT_GBUF_SRV));

    /* Pane rect from the CB. Shadow/SSR: vpX/Y=misc.y/z (idx17,18), paneW=params.w(27),
     * paneH=params2.x(28). Light: vpX/Y=misc.z/w (idx18,19), paneW=ext.y(21), paneH=ext.z(22). */
    if (mode == 1) { paneX = (int)f[18]; paneY = (int)f[19]; paneW = (int)f[21]; paneH = (int)f[22]; }
    else           { paneX = (int)f[17]; paneY = (int)f[18]; paneW = (int)f[27]; paneH = (int)f[28]; }
    if (paneW <= 0 || paneH <= 0) { paneX = 0; paneY = 0; paneW = e.width; paneH = e.height; }

    cbva = d3d12_priv_ring_cb(cb, cb_size);
    if (!cbva) { d3d12_priv_restore_scene_inputs(); return 0; }

    switch (mode) {
    case 2:  mask = g_dxr.reflcol;  mask_state = &g_dxr.reflcol_state;  mask_srv = DXR_SLOT_REFLCOL_SRV;  raygen = DXR_RAYGEN_REFL;   cb_param = 3; break;
    case 1:  mask = g_dxr.lightcol; mask_state = &g_dxr.lightcol_state; mask_srv = DXR_SLOT_LIGHTCOL_SRV; raygen = DXR_RAYGEN_LIGHT;  cb_param = 2; break;
    default: mask = g_dxr.sunvis;   mask_state = &g_dxr.sunvis_state;   mask_srv = DXR_SLOT_SUNVIS_SRV;   raygen = DXR_RAYGEN_SHADOW; cb_param = 1; break;
    }
    if (*mask_state != D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
        dxr_barrier(cl, mask, *mask_state, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        *mask_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }

    heaps[0] = g_dxr.heap;
    ID3D12GraphicsCommandList_SetDescriptorHeaps(cl, 1, heaps);
    ID3D12GraphicsCommandList_SetComputeRootSignature(cl, g_dxr.global_rs);
    ID3D12GraphicsCommandList4_SetPipelineState1(cl4, g_dxr.so);
    ID3D12GraphicsCommandList_SetComputeRootConstantBufferView(cl, cb_param, cbva);  /* b1 shadow / b2 light / b3 ssr */
    ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(cl, 4, dxr_gpu(g_dxr.heap, DXR_SLOT_TABLE_BASE));
    ZeroMemory(&d, sizeof(d));
    d.RayGenerationShaderRecord.StartAddress = g_dxr.sbt_va + DXR_SBT_RAYGEN_OFF + (UINT64)raygen * DXR_SBT_RAYGEN_STRIDE;
    d.RayGenerationShaderRecord.SizeInBytes  = DXR_SHADER_ID_SIZE;
    d.MissShaderTable.StartAddress  = g_dxr.sbt_va + DXR_SBT_MISS_OFF;
    d.MissShaderTable.SizeInBytes   = (UINT64)DXR_MISS_COUNT * DXR_SHADER_ID_SIZE;
    d.MissShaderTable.StrideInBytes = DXR_SHADER_ID_SIZE;
    d.HitGroupTable.StartAddress    = g_dxr.sbt_va + DXR_SBT_HITGROUP_OFF;
    d.HitGroupTable.SizeInBytes     = (UINT64)DXR_HITGROUP_COUNT * DXR_SHADER_ID_SIZE;
    d.HitGroupTable.StrideInBytes   = DXR_SHADER_ID_SIZE;
    d.Width = (UINT)paneW; d.Height = (UINT)paneH; d.Depth = 1;
    ID3D12GraphicsCommandList4_DispatchRays(cl4, &d);
    dxr_uav_barrier(cl, mask);
    Backend_NoteRTMark(mode == 2 ? "dispatch_refl" : mode == 1 ? "dispatch_light" : "dispatch_shadow");

    d3d12_priv_restore_scene_inputs();
    dxr_barrier(cl, mask, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    *mask_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

    /* Composite over the pane (color-only RT; MULT for shadow, additive for light). */
    ID3D12GraphicsCommandList_OMSetRenderTargets(cl, 1, &e.rtv, FALSE, NULL);
    ID3D12GraphicsCommandList_SetDescriptorHeaps(cl, 1, heaps);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(cl, g_dxr.blit_rs);
    ID3D12GraphicsCommandList_SetPipelineState(cl, comp);
    ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(cl, 0, dxr_gpu(g_dxr.heap, mask_srv));
    vp.TopLeftX = (float)paneX; vp.TopLeftY = (float)paneY; vp.Width = (float)paneW; vp.Height = (float)paneH;
    vp.MinDepth = 0.0f; vp.MaxDepth = 1.0f;
    sc.left = paneX; sc.top = paneY; sc.right = paneX + paneW; sc.bottom = paneY + paneH;
    ID3D12GraphicsCommandList_RSSetViewports(cl, 1, &vp);
    ID3D12GraphicsCommandList_RSSetScissorRects(cl, 1, &sc);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(cl, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D12GraphicsCommandList_DrawInstanced(cl, 3, 1, 0, 0);

    d3d12_priv_end_rt_pass();   /* restore RTV+DSV + invalidate backend draw cache */
    return 1;
}

int d3d12_dxr_shadow_pass(const ShadowCB *cb) { return cb ? dxr_lighting_pass(cb, sizeof(ShadowCB), 0) : 0; }
int d3d12_dxr_light_pass(const LightCB *cb)   { return cb ? dxr_lighting_pass(cb, sizeof(LightCB), 1)  : 0; }
int d3d12_dxr_ssr_pass(const SSRCB *cb)       { return cb ? dxr_lighting_pass(cb, sizeof(SSRCB), 2)    : 0; }

void Backend_RTDebugView(void)
{
    d3d12_dxr_env e;
    ID3D12GraphicsCommandList  *cl;
    ID3D12GraphicsCommandList4 *cl4;
    ID3D12DescriptorHeap *heaps[1];
    D3D12_GPU_VIRTUAL_ADDRESS cbva;
    D3D12_DISPATCH_RAYS_DESC d;
    D3D12_VIEWPORT vp; D3D12_RECT sc;
    int pw, ph;

    d3d12_priv_env(&e);
    if (g_dxr.disabled || !e.device5 || !e.list4 || !e.frame_open) return;
    if (!g_dxr.tlas_valid || !g_dxr.have_view) return;
    g_dxr.device5 = e.device5; g_dxr.list4 = e.list4;
    if (!dxr_ensure_init() || !dxr_ensure_output(e.width, e.height)) return;
    cl = e.list; cl4 = e.list4;
    pw = (int)g_dxr.view.paneSize[0]; ph = (int)g_dxr.view.paneSize[1];
    if (pw <= 0 || ph <= 0) { pw = e.width; ph = e.height; }

    cbva = d3d12_priv_ring_cb(&g_dxr.view, (UINT)sizeof(g_dxr.view));
    if (!cbva) return;

    if (g_dxr.out_state != D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
        dxr_barrier(cl, g_dxr.output, g_dxr.out_state, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        g_dxr.out_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }
    heaps[0] = g_dxr.heap;
    ID3D12GraphicsCommandList_SetDescriptorHeaps(cl, 1, heaps);
    ID3D12GraphicsCommandList_SetComputeRootSignature(cl, g_dxr.global_rs);
    ID3D12GraphicsCommandList4_SetPipelineState1(cl4, g_dxr.so);
    ID3D12GraphicsCommandList_SetComputeRootConstantBufferView(cl, 0, cbva);   /* b0 view */
    ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(cl, 4, dxr_gpu(g_dxr.heap, DXR_SLOT_TABLE_BASE));

    ZeroMemory(&d, sizeof(d));
    d.RayGenerationShaderRecord.StartAddress = g_dxr.sbt_va + DXR_SBT_RAYGEN_OFF + (UINT64)DXR_RAYGEN_DEBUG * DXR_SBT_RAYGEN_STRIDE;
    d.RayGenerationShaderRecord.SizeInBytes  = DXR_SHADER_ID_SIZE;
    d.MissShaderTable.StartAddress  = g_dxr.sbt_va + DXR_SBT_MISS_OFF;
    d.MissShaderTable.SizeInBytes   = (UINT64)DXR_MISS_COUNT * DXR_SHADER_ID_SIZE;
    d.MissShaderTable.StrideInBytes = DXR_SHADER_ID_SIZE;
    d.HitGroupTable.StartAddress    = g_dxr.sbt_va + DXR_SBT_HITGROUP_OFF;
    d.HitGroupTable.SizeInBytes     = (UINT64)DXR_HITGROUP_COUNT * DXR_SHADER_ID_SIZE;
    d.HitGroupTable.StrideInBytes   = DXR_SHADER_ID_SIZE;
    d.Width = (UINT)pw; d.Height = (UINT)ph; d.Depth = 1;
    ID3D12GraphicsCommandList4_DispatchRays(cl4, &d);
    dxr_uav_barrier(cl, g_dxr.output);
    Backend_NoteRTMark("dispatch_debug");

    /* Blit the output over the pane rect. */
    dxr_barrier(cl, g_dxr.output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    g_dxr.out_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    ID3D12GraphicsCommandList_OMSetRenderTargets(cl, 1, &e.rtv, FALSE, NULL);
    ID3D12GraphicsCommandList_SetDescriptorHeaps(cl, 1, heaps);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(cl, g_dxr.blit_rs);
    ID3D12GraphicsCommandList_SetPipelineState(cl, g_dxr.blit_pso);
    ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(cl, 0, dxr_gpu(g_dxr.heap, DXR_SLOT_BLIT_SRV));
    vp.TopLeftX = g_dxr.view.paneOrigin[0]; vp.TopLeftY = g_dxr.view.paneOrigin[1];
    vp.Width = (float)pw; vp.Height = (float)ph; vp.MinDepth = 0.0f; vp.MaxDepth = 1.0f;
    sc.left = (LONG)g_dxr.view.paneOrigin[0]; sc.top = (LONG)g_dxr.view.paneOrigin[1];
    sc.right = sc.left + pw; sc.bottom = sc.top + ph;
    ID3D12GraphicsCommandList_RSSetViewports(cl, 1, &vp);
    ID3D12GraphicsCommandList_RSSetScissorRects(cl, 1, &sc);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(cl, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D12GraphicsCommandList_DrawInstanced(cl, 3, 1, 0, 0);
}

static int dxr_ensure_init(void)
{
    if (g_dxr.inited) return 1;
    if (!g_dxr.device5) return 0;
    if (!dxr_create_heap())        goto fail;
    dxr_write_null_tlas_srv(DXR_SLOT_TLAS_SRV);   /* valid descriptor before any TLAS */
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
    ID3D12GraphicsCommandList_SetComputeRootConstantBufferView(cl, 0, g_dxr.sbt_va);   /* b0 (unused) */
    ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(cl, 4, dxr_gpu(g_dxr.heap, DXR_SLOT_TABLE_BASE));

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
    ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(cl, 0, dxr_gpu(g_dxr.heap, DXR_SLOT_BLIT_SRV));
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
    int i;
    /* Meshes + pools + scratch (device-lost drill: everything torn down here). */
    for (i = 0; i < DXR_MAX_MESHES; i++) {
        DxrMesh *m = &g_dxr.meshes[i];
        if (m->blas)    { ID3D12Resource_Release(m->blas); m->blas = NULL; }
        if (m->staging) { ID3D12Resource_Release(m->staging); m->staging = NULL; }
        free(m->ranges); m->ranges = NULL;
        ZeroMemory(m, sizeof(*m));
    }
    for (i = 0; i < 2; i++) {
        if (g_dxr.tlas[i])        { ID3D12Resource_Release(g_dxr.tlas[i]);        g_dxr.tlas[i] = NULL; }
        if (g_dxr.inst_upload[i]) { ID3D12Resource_Release(g_dxr.inst_upload[i]); g_dxr.inst_upload[i] = NULL; }
    }
    if (g_dxr.tlas_scratch) { ID3D12Resource_Release(g_dxr.tlas_scratch); g_dxr.tlas_scratch = NULL; }
    if (g_dxr.blas_scratch) { ID3D12Resource_Release(g_dxr.blas_scratch); g_dxr.blas_scratch = NULL; }
    if (g_dxr.vb_pool)      { ID3D12Resource_Release(g_dxr.vb_pool);      g_dxr.vb_pool = NULL; }
    if (g_dxr.ib_pool)      { ID3D12Resource_Release(g_dxr.ib_pool);      g_dxr.ib_pool = NULL; }
    g_dxr.vb_cap = g_dxr.ib_cap = g_dxr.vb_used = g_dxr.ib_used = 0;
    g_dxr.blas_scratch_cap = g_dxr.tlas_scratch_cap = g_dxr.tlas_cap = 0;
    g_dxr.tlas_parity = 0; g_dxr.tlas_valid = 0;
    g_dxr.scene_open = 0; g_dxr.scene_inst = NULL; g_dxr.scene_count = 0;

    if (g_dxr.sunvis)    { ID3D12Resource_Release(g_dxr.sunvis);       g_dxr.sunvis = NULL; }
    if (g_dxr.lightcol)  { ID3D12Resource_Release(g_dxr.lightcol);     g_dxr.lightcol = NULL; }
    if (g_dxr.reflcol)   { ID3D12Resource_Release(g_dxr.reflcol);      g_dxr.reflcol = NULL; }
    if (g_dxr.shadow_pso){ ID3D12PipelineState_Release(g_dxr.shadow_pso); g_dxr.shadow_pso = NULL; }
    if (g_dxr.light_pso) { ID3D12PipelineState_Release(g_dxr.light_pso);  g_dxr.light_pso = NULL; }
    if (g_dxr.refl_pso)  { ID3D12PipelineState_Release(g_dxr.refl_pso);   g_dxr.refl_pso = NULL; }
    if (g_dxr.geo_buf)   { ID3D12Resource_Release(g_dxr.geo_buf); g_dxr.geo_buf = NULL; g_dxr.geo_mapped = NULL; }
    g_dxr.p3_srvs_ready = 0;
    g_dxr.mask_w = g_dxr.mask_h = 0;
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
    g_dxr.generation++;   /* device-lost: game re-feeds meshes (handles now stale) */
    /* device5/list4 are backend-owned; do not release here. */
}
