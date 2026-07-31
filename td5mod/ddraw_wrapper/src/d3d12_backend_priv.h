/**
 * d3d12_backend_priv.h -- wrapper-internal seam between d3d12_backend.c and the
 * DXR module d3d12_dxr.c (ray-traced lighting, LIGHTING QUALITY: HIGH).
 *
 * d3d12_backend.c keeps all of its state static; the small surface the DXR code
 * needs (device, current frame's command list4, deferred-release + fence + CB
 * ring helpers, current backbuffer / frame index) is published here through
 * accessor functions and a per-frame env snapshot -- NOT exported globals.
 *
 * Both TUs compile with COBJMACROS + WIDL_EXPLICIT_AGGREGATE_RETURNS before
 * <d3d12.h>; include that (via wrapper.h) before this header.
 */
#ifndef D3D12_BACKEND_PRIV_H
#define D3D12_BACKEND_PRIV_H

#include <d3d12.h>

/* Per-frame environment the DXR module reads from the backend. Snapshot it with
 * d3d12_priv_env() at the point RT work runs (inside an OPEN frame). Any pointer
 * may be NULL when RT is unavailable / the frame is closed -- callers check. */
typedef struct {
    ID3D12Device5              *device5;      /* NULL when the GPU lacks DXR      */
    ID3D12GraphicsCommandList  *list;         /* current frame's list (base)      */
    ID3D12GraphicsCommandList4 *list4;        /* same list, DXR interface         */
    ID3D12Resource             *backbuffer;   /* current backbuffer               */
    D3D12_CPU_DESCRIPTOR_HANDLE rtv;          /* current backbuffer RTV handle    */
    UINT                        frame_index;  /* 0..D3D12_FRAME_COUNT-1           */
    int                         width, height;/* swapchain / render dimensions    */
    int                         frame_open;   /* 1 if the command list is open    */
} d3d12_dxr_env;

/* ---- accessors implemented in d3d12_backend.c ------------------------------ */

/* Fill `out` with the current frame environment (see above). */
void  d3d12_priv_env(d3d12_dxr_env *out);
/* Ensure the frame command list is open (mirrors d3d12_frame_begin). */
void  d3d12_priv_frame_begin(void);
/* Deferred-release a COM resource once the in-flight frame's fence passes
 * (routes to the backend's retire queue; identical policy to d3d12_retire). */
void  d3d12_priv_retire(void *res);
/* Bump-allocate a 256-aligned CB slice from the current frame's upload ring and
 * copy `data` in; returns the GPU VA (0 on overflow / no ring). */
D3D12_GPU_VIRTUAL_ADDRESS d3d12_priv_ring_cb(const void *data, UINT size);
/* Backend's fullscreen-triangle VS + passthrough composite PS bytecode (SM5.0
 * DXBC), reused by the DXR module's UAV->backbuffer blit so the shader arrays
 * live in exactly one TU (they are external-linkage globals). */
void  d3d12_priv_fullscreen_shaders(const void **vs, SIZE_T *vs_len,
                                    const void **ps, SIZE_T *ps_len);

/* ---- DXR module entry points implemented in d3d12_dxr.c -------------------- */

/* Called from Backend_CreateDevice once the Device5 / CommandList4 interfaces
 * are (or are not) available -- caches them and resets the lazy-init flag.
 * dev5/list4 may be NULL (RT unavailable). */
void  d3d12_dxr_on_device(ID3D12Device5 *dev5, ID3D12GraphicsCommandList4 *list4);
/* Release every DXR object (called from d3d12_release_all before device teardown
 * and on device-lost). Safe to call when nothing was created. */
void  d3d12_dxr_shutdown(void);
/* 1 if DXR is usable (Device5 present, not disabled). */
int   d3d12_dxr_available(void);
/* Phase 0 smoke: lazily init + DispatchRays a UV gradient into the DXR output
 * UAV and blit it over the current backbuffer. Gated by TD5RE_RT_SMOKE at the
 * call site. Runs inside the currently-open frame's command list. */
void  d3d12_dxr_smoke_blit(void);
/* 1 if TD5RE_RT_SMOKE is set (cached). */
int   d3d12_dxr_smoke_enabled(void);

#endif /* D3D12_BACKEND_PRIV_H */
