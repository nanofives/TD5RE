/**
 * td5_backend_texture.h -- opaque, backend-agnostic GPU texture handle.
 *
 * Phase 0 of the D3D12 port (docs/plans/D3D12_PORT_PLAN.md). The wrapper's
 * shared COM-emulation files (surface4.c, texture2.c, png_loader.c, device3.c)
 * hold a `BackendTexture*` and go through this API instead of dereferencing
 * ID3D11Texture2D / ...ShaderResourceView / ...RenderTargetView directly, so
 * those files compile against EITHER backend library. The active backend
 * (d3d11_backend_texture.c today; d3d12_backend_texture.c later) owns the
 * struct body and implements every function here.
 *
 * DXGI_FORMAT is deliberately kept in these signatures: it is a dxgi.h type,
 * shared by both backends (both link -ldxgi), NOT a d3d11.h type -- so the
 * shared files may keep using it after d3d11.h is fenced out.
 *
 * Included from wrapper.h (after the forward declarations). Every function is
 * a cheap no-op / NULL-return when there is no live device.
 */

#ifndef TD5_BACKEND_TEXTURE_H
#define TD5_BACKEND_TEXTURE_H

/* Opaque handle -- body defined only inside the active backend .c file. */
typedef struct BackendTexture BackendTexture;

/* Create a GPU texture. Always SHADER_RESOURCE-bindable; adds RENDER_TARGET
 * when is_rt, and a CPU-write STAGING sibling when needs_staging (the RT
 * upload path used one). Refcount starts at 1; stamps the current device
 * generation. Returns NULL if there is no device or on failure. */
BackendTexture *Backend_TextureCreate(DWORD w, DWORD h, DXGI_FORMAT fmt,
                                      int is_rt, int needs_staging);

/* Refcount control. A handle can outlive its owning surface: the D3D6 game
 * released a surface after IDirect3DTexture2::GetHandle but kept drawing from
 * the handle, and the port inherits that lifetime. Model it with an explicit
 * AddRef on the handle instead of the old raw-SRV AddRef. Release frees the
 * underlying GPU objects at refcount 0 and is NULL-safe. */
void Backend_TextureAddRef(BackendTexture *bt);
void Backend_TextureRelease(BackendTexture *bt);

/* Upload a full CPU image into an existing texture (the surface FlushDirty
 * path). When src_bpp==16 and the texture is B8G8R8A8, converts R5G6B5->BGRA;
 * otherwise uploads through a transient staging copy (respecting GPU row
 * pitch). No-op without a device. */
void Backend_TextureUpload(BackendTexture *bt, const void *sys, LONG src_pitch,
                           DWORD w, DWORD h, DWORD src_bpp);

/* The full IDirect3DTexture2::Load upload. Handles the PNG override, the
 * 16bpp->B8G8R8A8 path (A1R5G5B5-vs-R5G6B5 detection + colorkey->alpha bake),
 * the 16bpp->16bpp path, the 32bpp direct path, and the GPU->GPU copy fallback.
 * May recreate the destination texture: *pbt is released and replaced with the
 * new handle. Writes *out_r5g6b5_source (0/1). `src_bt` is the source's handle
 * (may be NULL when only src_pixels is available). Returns 1 on success, 0 if
 * no viable path. */
int Backend_TextureLoad(BackendTexture **pbt, DWORD dst_w, DWORD dst_h,
                        DXGI_FORMAT dst_fmt,
                        const void *src_pixels, LONG src_pitch,
                        DWORD src_w, DWORD src_h, DWORD src_bpp,
                        int src_has_alpha, int has_colorkey, DWORD colorkey_low,
                        BackendTexture *src_bt, int *out_r5g6b5_source);

/* Bind `bt` as the sampled texture (pixel-shader SRV slot 0). NULL binds the
 * backend's 1x1 white fallback. */
void Backend_TextureBind(BackendTexture *bt);

/* Bind `bt` as the sole render target (+ the backend depth DSV), matching the
 * OMSetRenderTargets the RT-surface creation path used to issue. */
void Backend_TextureBindRenderTarget(BackendTexture *bt);

/* Clear bt's render-target view to `rgba` (4 floats, RGBA order as
 * ClearRenderTargetView expects). No-op if bt has no RTV. */
void Backend_TextureClearRT(BackendTexture *bt, const float *rgba);

/* device-lost recovery: if bt's GPU objects were created on an older device
 * generation, drop them and recreate on the current device (caller re-uploads
 * pixels afterward). No-op when already current or without a device. */
void Backend_TextureEnsureCurrent(BackendTexture *bt, DWORD w, DWORD h,
                                  DXGI_FORMAT fmt, int is_rt, int needs_staging);

/* Adopt a raw backend-native texture object as a BackendTexture (used by the
 * PNG-override path, which decodes straight to a GPU texture). Backend-private
 * detail lives behind the void*; only the active backend calls this. Returns a
 * new handle (refcount 1) or NULL. */
BackendTexture *Backend_TextureAdopt(void *native_srv);

/* True if bt currently owns a valid GPU texture (the "mark this surface as
 * video memory" check keyed off tex/rtv presence). NULL-safe (returns 0). */
int Backend_TextureIsValid(const BackendTexture *bt);
int Backend_TextureHasRTV(const BackendTexture *bt);

#endif /* TD5_BACKEND_TEXTURE_H */
