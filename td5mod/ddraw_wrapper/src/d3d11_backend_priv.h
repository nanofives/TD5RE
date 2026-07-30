/**
 * d3d11_backend_priv.h -- D3D11-backend-private definition of the BackendTexture
 * handle body + small surface accessors.
 *
 * Included ONLY by the d3d11_backend_*.c files (never by the shared COM files,
 * which see BackendTexture as an opaque type via td5_backend_texture.h). This
 * is where the D3D11 struct body lives so the backend's render code
 * (composite, pipeline, present) can reach a surface's GPU objects through its
 * opaque `bt` handle. The D3D12 backend will have its own equivalent priv
 * header; the shared files never include either.
 *
 * Include AFTER wrapper.h (needs the ID3D11* types + WrapperSurface).
 */

#ifndef TD5_D3D11_BACKEND_PRIV_H
#define TD5_D3D11_BACKEND_PRIV_H

/* Body of the opaque BackendTexture handle (td5_backend_texture.h). */
struct BackendTexture {
    ID3D11Texture2D            *tex;      /* GPU texture (NULL for adopted-SRV) */
    ID3D11ShaderResourceView   *srv;      /* SRV for shader sampling            */
    ID3D11RenderTargetView     *rtv;      /* RTV (render targets only)          */
    ID3D11Texture2D            *staging;  /* CPU-write staging (RT upload)       */
    DXGI_FORMAT                 format;
    DWORD                       width, height;
    unsigned                    device_generation;
    LONG                        ref_count;
};

/* Bodies of the opaque game-UI handles (td5_backend_texture.h). Used by the
 * port's vector-UI renderer (td5_frontend.c/td5_fe_menu.c) through the
 * Backend_*PixelShader/*ConstBuffer API so those files hold no ID3D11 types. */
struct BackendPixelShader { ID3D11PixelShader *ps; };
struct BackendConstBuffer { ID3D11Buffer *cb; };

/* Reach a surface's GPU objects through its opaque handle (NULL-safe). These
 * replace the old direct `surface->d3d11_srv/rtv/texture` member reads in the
 * backend render code. */
static inline ID3D11ShaderResourceView *bt_srv(const WrapperSurface *s)
{ return (s && s->bt) ? s->bt->srv : NULL; }
static inline ID3D11RenderTargetView *bt_rtv(const WrapperSurface *s)
{ return (s && s->bt) ? s->bt->rtv : NULL; }
static inline ID3D11Texture2D *bt_tex(const WrapperSurface *s)
{ return (s && s->bt) ? s->bt->tex : NULL; }

#endif /* TD5_D3D11_BACKEND_PRIV_H */
