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

/* Windowed mode: display window management */
void Backend_EnforceWindowSize(void);
void Backend_UpdateMousePosition(void);
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

/* [P3] Screen-space reflections: fullscreen ALPHA-BLENDED draw over the
 * CURRENT viewport that reflects the (already lit + shadowed) scene on
 * reflective materials via depth-buffer ray marching. Copies the backbuffer
 * to scene_copy_tex first. Run AFTER Backend_ApplyLightPass, BEFORE the
 * translucent VFX/HUD. No-op if ps_ssr/depth_srv/gbuffer_srv are NULL. */
void Backend_ApplySSRPass(const SSRCB *cb);

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
