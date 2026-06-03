/**
 * td5_vectorui.h -- Public surface of the resolution-independent "VectorUI"
 * renderer.
 *
 * The VectorUI primitives (MSDF text, procedural rounded-rect panels, selector
 * arrows, and the analytic SDF gauge dial) live in td5_frontend.c, which owns
 * the D3D11 pixel shaders + constant buffers. They were originally built for the
 * frontend menus; this header exposes them so the in-race HUD (td5_hud.c) can
 * reuse the exact same crisp-at-any-resolution primitives.
 *
 * Conventions (identical to the frontend's fe_draw_* and the HUD's 640x480
 * virtual layout):
 *   - All x/y/w/h are SCREEN PIXELS (already scaled to the render target).
 *   - sx/sy are the canvas scale: render_width/640, render_height/480.
 *   - Colours are 0xAARRGGBB.
 *   - Every entry is a no-op / returns 0 when VectorUI is disabled or the
 *     shader/atlas failed to load, so callers can fall back to the bitmap path.
 *
 * The GPU resources are created once at frontend init and persist for the whole
 * session (menu + race), so the HUD may call these freely during a race.
 */

#ifndef TD5_VECTORUI_H
#define TD5_VECTORUI_H

#include <stdint.h>

/* --- Availability gates (check before use; fall back to bitmap if 0) --- */
int td5_vui_text_available(void);    /* MSDF text ready */
int td5_vui_shapes_available(void);  /* roundrect + arrow ready */
int td5_vui_gauge_available(void);   /* SDF gauge dial ready */

/* --- MSDF text (BodyText font atlas) --- */
void  td5_vui_text(float x, float y, const char *s, uint32_t color, float sx, float sy);
void  td5_vui_text_centered(float cx, float y, const char *s, uint32_t color, float sx, float sy);
float td5_vui_text_width(const char *s, float sx);

/* --- Generic quad (solid or textured). tex_page < 0 => 1x1 white. --- */
void td5_vui_quad(float x, float y, float w, float h, uint32_t color, int tex_page,
                  float u0, float v0, float u1, float v1);

/* --- In-race HUD font SDF (original typeface, crisp at any resolution) ---
 * td5_vui_hudfont_page() returns the SDF atlas page (-1 if unavailable).
 * td5_vui_msdf_quad draws one glyph cell through the distance-field shader;
 * u0..v1 are NORMALISED UVs into `page`. */
int  td5_vui_hudfont_page(void);
int  td5_vui_pausefont_page(void);   /* pause-menu font SDF page (-1 if unavailable) */
void td5_vui_msdf_quad(float x, float y, float w, float h, uint32_t color, int page,
                       float u0, float v0, float u1, float v1);

/* --- Procedural neon rounded-rect panel/button. Returns 0 if unavailable. ---
 * r_large = TL/BR radius, r_small = TR/BL radius; border_side/border_topbot =
 * rim band thickness; mid/inner/outer = 3-stop metallic rim; fill + fill_alpha =
 * interior (fill_alpha 0 => border only). */
int td5_vui_roundrect(float x, float y, float w, float h,
                      float r_large, float r_small, float border_side, float border_topbot,
                      uint32_t mid, uint32_t inner, uint32_t outer,
                      uint32_t fill, float fill_alpha);

/* --- Procedural selector arrow (triangle SDF). dir_right: 0 = left, 1 = right. */
int td5_vui_arrow(float x, float y, float w, float h, int dir_right, uint32_t color);

/* --- Analytic SDF gauge dial (HUD speedometer + tachometer) ---
 * Draws a dial face disc + bright outer ring + radial major/minor tick marks +
 * an optional redline arc + center pivot dot, all anti-aliased and crisp at any
 * resolution. The NEEDLE is NOT drawn here -- callers draw it as a separate quad
 * to stay byte-faithful to the original needle math.
 *
 * Angles are in DEGREES, screen convention: 0 = +X (right), increasing CLOCKWISE
 * (screen Y points down) -- matching the original dial generator
 * (angle = 150deg + value/max * 240deg). */
typedef struct TD5_VuiGauge {
    float    cx, cy;            /* dial centre (screen px) */
    float    radius;            /* outer disc radius (screen px) */
    float    inner_radius;      /* inner 3D circle radius (0 => none) */
    float    tick_out;          /* tick outer radius (screen px) */
    float    sweep_start_deg;   /* first tick angle */
    float    sweep_end_deg;     /* last tick angle */
    int      tick_count;        /* total ticks along the sweep (>=2, <=64) */
    int      major_every;       /* every Nth tick (0-based) is a major tick */
    float    major_len_px;      /* major tick length (inward from tick_out) */
    float    minor_len_px;      /* minor tick length */
    float    redline_start_deg; /* red zone start (red teeth + rim); >= end => none */
    float    redline_end_deg;   /* red zone end */
    float    rim_red_px;        /* red rim arc band thickness */
    float    pivot_px;          /* centre pivot hub radius (0 => none) */
    uint32_t face_color;        /* outer disc (semi-transparent) */
    uint32_t inner_color;       /* inner 3D disc */
    uint32_t tick_color;        /* white tick teeth */
    uint32_t redline_color;     /* red teeth + red rim arc */
    uint32_t pivot_color;       /* pivot hub (alpha 0 => none) */
} TD5_VuiGauge;

void td5_vui_gauge(const TD5_VuiGauge *g);

#endif /* TD5_VECTORUI_H */
