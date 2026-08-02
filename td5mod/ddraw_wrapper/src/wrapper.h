/**
 * wrapper.h - D3D11 Wrapper for DirectDraw/Direct3D 6
 *
 * Replaces DDrawCompat's ddraw.dll with a custom implementation that
 * translates all DirectDraw/D3D6 COM calls to Direct3D 11 internally.
 * This eliminates DirectDraw from the process, preventing Windows from
 * applying DWM8And16BitMitigation (classic window borders).
 *
 * Architecture:
 *   Game (TD5_d3d.exe) -> M2DX.dll -> Our COM objects -> D3D11 -> GPU
 *
 * Key design:
 *   - Pre-transformed vertices (XYZRHW) converted to NDC in vertex shader
 *   - All rendering uses HLSL shaders (no fixed-function pipeline in D3D11)
 *   - Single texture unit
 *   - No stencil buffer
 *   - ~40 COM methods need real implementation
 *
 * [2026-07-09, A3 refactor] Split into per-interface headers (each self-
 * contained but meant to be included only via this file, in this order):
 *   td5_wrapper_backend.h      -- render state cache, deferred pane-record
 *                                 context, constant buffers, D3D11Backend
 *   td5_wrapper_ddraw_types.h  -- DDraw constants/flags/GUIDs/structures
 *   td5_wrapper_objects.h      -- D3D6 render-state constants + the
 *                                 WrapperSurface/Viewport/Texture/Clipper
 *                                 COM object structs + vtables
 * This header keeps the debug-logging macros, the forward declarations of
 * the Wrapper* COM object types (needed by all three), and the COM
 * interface function prototypes (the wrapper's actual public API).
 */

#ifndef TD5_D3D11_WRAPPER_H
#define TD5_D3D11_WRAPPER_H

#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <stdint.h>
#include <stdio.h>

/* ========================================================================
 * Debug logging
 * ======================================================================== */

/* Runtime gate. Default 1 (matches historic behavior). Call from main.c after
 * INI parse to silence the wrapper before Backend_Init opens log/wrapper.log.
 * Prototype is unconditional so callers compiled without WRAPPER_DEBUG (e.g.
 * main.c, td5_platform_win32.c) can still link against the wrapper archive,
 * which is always built with WRAPPER_DEBUG defined. */
void wrapper_set_enabled(int enabled);

#ifdef WRAPPER_DEBUG
void wrapper_log(const char *fmt, ...);
#define WRAPPER_LOG(fmt, ...) wrapper_log(fmt, ##__VA_ARGS__)
#else
#define WRAPPER_LOG(fmt, ...) ((void)0)
#endif

#define WRAPPER_STUB(name) \
    WRAPPER_LOG("STUB: %s", name); return S_OK

/* [x64 Stage 3] One-shot breadcrumb: logs the FIRST time each call site is
 * reached, then costs a predictable-branch test forever after.
 *
 * Purpose: locating a hard crash by "last breadcrumb printed" without the 870 KB
 * of per-call spam that ordinary WRAPPER_LOG produces in the draw path -- the
 * x64 build dies during first-frame setup, and per-call logging there is both
 * too noisy to read and slow enough to change the timing.
 *
 * The log is unbuffered (see wrapper_log), so the last line written IS the last
 * point reached, even on a hard fault. */
#define WRAPPER_ONCE(label) \
    do { static int once_ = 0; if (!once_) { once_ = 1; WRAPPER_LOG("ONCE: %s", label); } } while (0)

/* ========================================================================
 * Forward declarations for our COM objects
 * ======================================================================== */

typedef struct WrapperDirectDraw     WrapperDirectDraw;
typedef struct WrapperSurface        WrapperSurface;
typedef struct WrapperD3D            WrapperD3D;
typedef struct WrapperDevice         WrapperDevice;
typedef struct WrapperViewport       WrapperViewport;
typedef struct WrapperTexture        WrapperTexture;
typedef struct WrapperClipper        WrapperClipper;

#include "td5_backend_texture.h"   /* opaque BackendTexture (referenced by objects.h) */
#include "td5_wrapper_backend.h"
#include "td5_wrapper_ddraw_types.h"
#include "td5_wrapper_objects.h"

/* ========================================================================
 * COM interface function prototypes
 * ======================================================================== */

/* Backend management */
int  Backend_Init(void);
void Backend_Shutdown(void);
int  Backend_CreateDevice(HWND hwnd, int width, int height, int bpp, int windowed);
int  Backend_Reset(int width, int height, int bpp, int windowed);
void Backend_EnumerateModes(void);
/* [S01 2026-06-04] Toggle DXGI exclusive fullscreen on the swap chain.
 * enable!=0 -> SetFullscreenState(TRUE) (exclusive), 0 -> SetFullscreenState(FALSE).
 * Idempotent and safe to call when no swap chain exists. Returns 1 on success. */
int  Backend_SetExclusiveFullscreen(int enable);

/* Object creation */
WrapperDirectDraw* WrapperDirectDraw_Create(void);
WrapperSurface*    WrapperSurface_Create(DWORD width, DWORD height, DWORD bpp, DWORD caps);
WrapperD3D*        WrapperD3D_Create(WrapperDirectDraw *ddraw);
WrapperDevice*     WrapperDevice_Create(WrapperSurface *render_target);
WrapperViewport*   WrapperViewport_Create(void);
WrapperTexture*    WrapperTexture_Create(WrapperSurface *surface);
WrapperClipper*    WrapperClipper_Create(void);

/* Surface helpers */
DXGI_FORMAT WrapperSurface_GetDXGIFormat(DWORD bpp, DWORD flags, DDPIXELFORMAT_W *pf);
void        WrapperSurface_EnsureSysBuffer(WrapperSurface *s);
void        WrapperSurface_FlushDirty(WrapperSurface *s);
/* [DEVICE-LOST recovery] If the surface's GPU objects were created on an older
 * device generation (i.e. a TDR + Backend_RecreateDevice happened since), drop
 * the stale D3D11 texture/SRV/RTV/staging and recreate them on the current
 * device, then mark the surface dirty so its sys_buffer content re-uploads.
 * No-op (cheap generation compare) when the surface is already current. */
void        WrapperSurface_EnsureDeviceCurrent(WrapperSurface *s);

/* Compositing: merge BltFast (2D) and D3D (3D) layers at present time */
void      Backend_EnsureCompositingTextures(int width, int height);
void      Backend_CompositeAndPresent(WrapperSurface *rt_surface, RECT *srcRect, RECT *dstRect);

/* Fullscreen quad rendering (for present blit and compositing) */
void      Backend_DrawFullscreenQuad(ID3D11ShaderResourceView *srv);

/* [D3D12 port Phase 0 — TRANSITIONAL] Raw SRV of a surface's GPU handle, for the
 * not-yet-fenced game-layer page renderer (td5_platform_win32*.c). Leaks the
 * D3D11 type on purpose; deleted once that renderer moves onto BackendTexture in
 * a later Phase 0 sub-step. NULL-safe (returns NULL). */
ID3D11ShaderResourceView *Backend_SurfaceGetSRV(WrapperSurface *s);
int  Backend_SurfaceHasRTV(WrapperSurface *s);
void Backend_SurfaceBindRenderTarget(WrapperSurface *s);

/* Backend-agnostic frame clears used by the shared COM files (device3 BeginScene,
 * viewport3 Clear). Backend_ClearBackbuffer clears the game render-target
 * (backbuffer texture RTV, or the swap-chain RTV as a fallback) to `rgba`
 * (4 floats, RGBA order). Backend_ClearDepth clears the backend depth buffer to
 * `z`. Both no-op without a device/target. */
void      Backend_ClearBackbuffer(const float *rgba);
void      Backend_ClearDepth(float z);

/* Draw submission for the D3D6 entry points (device3.c). Each binds the
 * pretransformed pipeline (VB[/IB]/topology/input-layout/VS from the appended
 * ring slice), resolves the render-state cache, guarantees a non-null slot-0
 * SRV, notes the draw, and issues it -- so device3.c holds no ID3D11 draw calls.
 * `prim_type` is the D3D6 D3DPT_* primitive type; `stride` is the vertex stride.
 * No-op without a device. */
void      Backend_DrawPrimitive(DWORD prim_type, UINT stride,
                                UINT base_vertex, UINT vert_count);
void      Backend_DrawIndexedPrimitive(DWORD prim_type, UINT stride,
                                UINT base_vertex, UINT start_index,
                                UINT index_count, UINT vert_count);
/* Set the rasterizer viewport (viewport3.c). No-op without a device. */
void      Backend_SetViewport(float x, float y, float w, float h,
                              float min_z, float max_z);

/* ---- Vector-UI renderer backend API (td5_frontend.c / td5_fe_menu.c) ------
 * The port draws its procedural SDF menu/HUD widgets with game-owned pixel
 * shaders + constant buffers. These wrap the D3D11 objects behind opaque
 * handles so the game-layer UI code holds no ID3D11 types. All NULL-safe. */
BackendPixelShader *Backend_CreatePixelShader(const void *bytecode, size_t len);
void  Backend_ReleasePixelShader(BackendPixelShader *ps);
BackendConstBuffer *Backend_CreateConstBuffer(size_t size);
void  Backend_ReleaseConstBuffer(BackendConstBuffer *cb);
void  Backend_UpdateConstBuffer(BackendConstBuffer *cb, const void *data, size_t size);
void  Backend_BindConstBuffer(UINT slot, BackendConstBuffer *cb);      /* PS slot */
void  Backend_SetBuiltinPixelShader(int ps_idx);   /* g_backend.ps_shaders[idx] */
void  Backend_BindSampler(UINT slot, int sampler_idx);       /* SAMP_* index */
void  Backend_ForceBlendState(int blend_idx);      /* bind + update cache idx */
/* [TRANSITIONAL] raw underlying shader for td5_plat_render_set_ps_override,
 * whose void* is still an ID3D11PixelShader* until the platform renderer is
 * fenced. Deleted with that fence. */
void *Backend_PixelShaderRaw(BackendPixelShader *ps);

/* Platform-renderer draw entry points (td5_platform_win32.c). Each does the
 * StreamUpload + pretransformed-pipeline bind + state resolve + draw so the
 * platform layer holds no ID3D11 calls. `rc` is the (opaque) deferred pane-record
 * bundle or NULL for the immediate context. Backend_PlatDrawTris honours a
 * ps_override (raw backend shader) + sampler index, else the texblend-selected
 * builtin PS. Backend_PlatDrawWhite draws PS_MODULATE*white (fog/alpha off, fixed
 * DS/blend) for debug lines (is_lines=1, non-indexed) and flat-colour ribbons
 * (is_lines=0, indexed), and invalidates the state cache afterward. */
void Backend_PlatDrawTris(WrapperRecCtx *rc, const void *verts, int vert_count,
                          const void *indices, int index_count,
                          void *ps_override, int ps_override_samp);
void Backend_PlatDrawWhite(WrapperRecCtx *rc, const void *verts, int vert_count,
                           const void *indices, int index_count, int is_lines);

/* Platform viewport/scissor/texture-bind (rc-aware). Backend_PlatSetViewport
 * sets a 0..1-depth viewport; Backend_PlatSetScissor sets the scissor rect;
 * Backend_PlatBindTextureSRV binds `srv` (a raw backend SRV, TRANSITIONAL void*)
 * at PS slot 0 (NULL binds nothing sampled), updates current_srv + marks the
 * state cache dirty. `rc` is the opaque deferred bundle or NULL for immediate. */
void Backend_PlatSetViewport(WrapperRecCtx *rc, int x, int y, int w, int h);
void Backend_PlatSetScissor(WrapperRecCtx *rc, int left, int top, int right, int bottom);
void Backend_PlatBindTextureSRV(WrapperRecCtx *rc, void *srv);

/* Window/present backend helpers (td5_platform_win32_window.c) so that file
 * holds no ID3D11/DXGI. SwapChainReady = context && swap_chain && swap_rtv;
 * HasSwapChain = swap_chain present. Bind/Clear/Unbind operate on the swap-chain
 * RTV. DrawFullscreenQuadRaw blits a raw (transitional void*) SRV. PresentSwapChain
 * does the NotePresent + Present + present-count + device-lost latch + trim.
 * CaptureBackbufferRGBA reads back the current backbuffer as RGBA (caller frees;
 * NULL on failure). */
int  Backend_SwapChainReady(void);
int  Backend_HasSwapChain(void);
/* Backend-agnostic readiness checks so the shared/game files don't null-check
 * the D3D11-typed g_backend.device/context directly (lets a D3D12 backend report
 * its own readiness). */
int  Backend_HasDevice(void);
int  Backend_HasContext(void);
void Backend_BindSwapChainRT(void);
void Backend_ClearSwapChainRT(const float *rgba);
void Backend_UnbindRenderTargets(void);
void Backend_DrawFullscreenQuadRaw(void *srv);
void Backend_PresentSwapChain(int sync);
unsigned char *Backend_CaptureBackbufferRGBA(int *out_w, int *out_h);

/* Soft-particle depth binding (smoke): swap the bound depth target to the
 * read-only DSV and expose scene depth as PS resource t1, so a shader can
 * sample depth while the hardware z-test still runs. Begin returns 1 if the
 * soft-particle resources exist (0 = unavailable). End restores the writable
 * depth target + original RTV. */
int  Backend_BindSceneDepthReadonly(void);
void Backend_UnbindSceneDepthReadonly(void);

/* Windowed mode: display window management */
void Backend_EnforceWindowSize(void);
HWND Backend_GetDisplayWindow(void);

/* Photo-booth frame capture (offline car-preview generation). RequestCapture
 * grabs the next presented frame; GetCapture returns its BGRA pixels (w*h*4),
 * owned by the backend, valid until the next capture. Returns 0 if not ready. */
void Backend_RequestCapture(void);
int  Backend_GetCapture(unsigned char **px, int *w, int *h);
void Backend_CaptureIfRequested(void);  /* call before every Present */

/* Render state management */
void Backend_ApplyStateCache(void);  /* Bind D3D11 state objects from cache */
void Backend_SelectPixelShader(void); /* Choose PS based on texblend + alpha + tex format */
void Backend_UpdateFogCB(void);      /* Upload fog constant buffer */
void Backend_FlushUploadsSync(void); /* Flush + WAIT texture uploads (one-shot/on-entry residency) */
void Backend_UpdateViewportCB(float w, float h); /* Upload viewport constant buffer */

/* Deferred dynamic-light pass: upload `cb` (camera + light array), then draw a
 * fullscreen additive pass that samples scene depth (depth_srv), reconstructs
 * world position, and accumulates the lights onto the scene render target. Runs
 * over the CURRENT D3D viewport/scissor (call after the opaque world geometry of
 * a viewport, before translucent VFX/HUD). No-op if depth_srv/ps_light are NULL. */
void Backend_ApplyLightPass(const LightCB *cb);

/* [P2] Screen-space ray-marched sun-shadow pass: fullscreen MULTIPLICATIVE
 * draw over the CURRENT viewport that darkens pixels whose path to the sun is
 * blocked by on-screen geometry (depth-buffer march). Run AFTER the opaque
 * world, BEFORE Backend_ApplyLightPass (so additive lights are not darkened).
 * No-op if depth_srv/ps_shadow are NULL. */
void Backend_ApplyShadowPass(const ShadowCB *cb);
void Backend_ApplyGIPass(const ShadowCB *cb);   /* [P4] sky-visibility GI (HIGH-only) */

/* [P3] Screen-space reflections: fullscreen ALPHA-BLENDED draw over the
 * CURRENT viewport that reflects the (already lit + shadowed) scene on
 * reflective materials via depth-buffer ray marching. Copies the backbuffer
 * to scene_copy_tex first. Run AFTER Backend_ApplyLightPass, BEFORE the
 * translucent VFX/HUD. No-op if ps_ssr/depth_srv/gbuffer_srv are NULL. */
void Backend_ApplySSRPass(const SSRCB *cb);

/* [RT lighting] 1 when the device supports DirectX Raytracing (Device5 QI'd,
 * OPTIONS5 tier >= 1.0, TD5RE_RT_DISABLE not set). 0 after device-lost until the
 * device is recreated + re-queried. The game gates LIGHTING QUALITY: HIGH on
 * this (auto-fallback to LOW when 0). See RT_LIGHTING_PLAN.md. */
int Backend_RTAvailable(void);

/* [RT lighting Phase 1] World-space geometry feed for the acceleration
 * structures. All calls are no-ops when RT is unavailable. Coordinates are
 * game world space in FLOAT (24.8 fixed / 256.0, +Y down preserved, no axis
 * flips) so raster reconstruction and rays share one convention. */
typedef struct {                         /* 24 bytes, pos at offset 0 */
    float    pos[3];                     /* world-space (track) / object-space (mesh) */
    float    uv[2];                      /* primary UV (stored for Phase 3 hit shading) */
    unsigned color;                      /* packed vertex color (Phase 3)              */
} BackendRTVertex;
typedef struct {                         /* one BLAS geometry per range */
    unsigned first_index;                /* first index into the mesh's index buffer */
    unsigned index_count;                /* multiple of 3                             */
    unsigned texture_id;                 /* wrapper texture handle/key (opaque, P3)   */
    unsigned matid_flags;                /* material id + flags (opaque until P2/P3)   */
} BackendRTRange;

/* Create a retained RT mesh (builds a BLAS lazily on first scene use). Returns a
 * handle > 0, or 0 on failure / RT unavailable. idx are 16-bit into verts. */
int  Backend_RTMeshCreate(const BackendRTVertex *verts, unsigned nverts,
                          const unsigned short *idx, unsigned nidx,
                          const BackendRTRange *ranges, unsigned nranges);
void Backend_RTMeshDestroy(int handle);

/* Per-frame TLAS assembly. Begin, add each visible instance (row-major 3x4
 * world transform; translation in m[3],m[7],m[11]), End builds the TLAS. */
void Backend_RTSceneBegin(void);
void Backend_RTSceneInstance(int mesh, const float m3x4[12], unsigned flags);
void Backend_RTSceneEnd(void);

/* AS generation counter: bumped on device recreation so the game re-feeds mesh
 * handles (they were destroyed with the old device). */
unsigned Backend_RTGeneration(void);

/* Push an RT work crumb (RTMARK:<tag>) to the crash-forensics ring so a TDR
 * post-mortem shows which RT operation was in flight. */
void Backend_NoteRTMark(const char *tag);

/* [RT lighting P2b] HIGH mode: the deferred shadow/light passes run the RT
 * dispatch + composite instead of the screen-space march. Set per frame by the
 * game (= td5_rt_active()). No-op when DXR is unavailable. */
void Backend_RTSetMode(int high);

/* [RT lighting P3] Register the currently-bound texture page into the DXR
 * bindless table (index = game page id) so reflection hits sample the real
 * texture. Called from the game's texture-page bind while RT is active; a no-op
 * otherwise. Transitions the texture to a shader-readable state for the RT read. */
void Backend_RTRegisterBoundPage(unsigned page_id);

/* Per-frame RT view constants (camera + sun) for the primary/debug ray. cam_pos
 * is FLOAT world space; basis9 is row-major {right,up,fwd}; focal/center match
 * the raster projection (see td5_render.c debug_line_project). sun is a world
 * direction. pane_* select the sub-rect for split-screen dispatch. */
void Backend_RTSetView(const float cam_pos[3], const float basis9[9],
                       float focal, float center_x, float center_y,
                       int pane_x, int pane_y, int pane_w, int pane_h,
                       const float sun_dir[3]);

/* Debug primary-ray view (TD5RE_RT_DEBUGVIEW): DispatchRays a per-pixel camera
 * ray against the TLAS and blit hitT / instance-hash false color over the pane.
 * The Phase 1 alignment gate compares this to a raster framedump. */
void Backend_RTDebugView(void);

/* [lighting rework P0] Per-frame G-buffer gate. on=1: (re)create the G-buffer
 * at render-target size if needed, clear it (matid 0 = "no data"), and let the
 * per-draw state machinery bind it as RT1 + swap in the ps_*_g MRT shader
 * variants for z-writing non-blended draws. on=0: unbind and stop writing.
 * Call once per rendered frame BEFORE the world pass (race frames only). */
void Backend_SetGBufferEnabled(int on);

/* [2026-06-08 streaming-ring] Append vertices (+ optional 16-bit indices) to the
 * dynamic VB/IB ring with WRITE_NO_OVERWRITE (DISCARD only on wrap). On success
 * returns 1 and writes the draw offsets: *out_base_vertex (BaseVertexLocation /
 * StartVertexLocation) and *out_start_index (StartIndexLocation, only when
 * indices!=NULL). Bind the buffers at byte offset 0 and pass these to the draw
 * call. Returns 0 (skip the draw) if a single batch exceeds the buffer or Map
 * fails. Shared by every draw path so the ring stays consistent. */
int Backend_StreamUpload(const void *verts, UINT vert_count, UINT stride,
                         const void *indices, UINT index_count,
                         UINT *out_base_vertex, UINT *out_start_index);

/* ========================================================================
 * [Phase B / Stage 2] Multithreaded pane-record API (deferred contexts)
 * NOTE: non-functional on the dev driver (ExecuteCommandList no-op) — kept as
 * gated-off scaffolding; the live threaded path is the CPU command list
 * (td5_rcmd) which replays on the immediate context.
 *
 * Usage per frame when threaded panes are enabled:
 *   Backend_RecPoolEnsure(n)            -- lazily create n deferred-context
 *                                          bundles + per-pane buffers (once).
 *   (on worker thread, per pane i:)
 *     WrapperRecCtx *rc = Backend_RecBegin(i, vp_x, vp_y, vp_w, vp_h);
 *     ... record draws (g_wrapper_rec == rc for this thread) ...
 *     Backend_RecEnd(rc);               -- FinishCommandList into rc
 *   (on main thread, in pane order:)
 *     Backend_RecExecute(i);            -- ExecuteCommandList on the immediate ctx
 *
 * Returns NULL / no-op if deferred contexts are unavailable on this device, so
 * the caller can fall back to the serial immediate path.
 * ======================================================================== */
int             Backend_RecPoolEnsure(int count);   /* 1 = ready, 0 = unavailable */
WrapperRecCtx  *Backend_RecBegin(int index, int vp_x, int vp_y, int vp_w, int vp_h);
void            Backend_RecEnd(WrapperRecCtx *rc);
void            Backend_RecExecute(int index);
void            Backend_RecPoolRelease(void);
void            Backend_RestoreMainRenderTarget(void);

#endif /* TD5_D3D11_WRAPPER_H */
