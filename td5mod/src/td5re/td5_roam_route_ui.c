/**
 * td5_roam_route_ui.c -- GEO FREE ROAM: route planner input + drawing (PORT-ONLY, F5-A).
 * Contract: td5_roam_route_ui.h. Model + planner: td5_roam_route.c.
 *
 * DRAWING. Everything 2D goes through ONE batcher (rb_*): quads with the white page, flushed
 * as a few large td5_plat_render_draw_tris calls (td5_hud_prim_tris), because a city map is
 * thousands of line segments and one draw call each would cost frames. Text (td5_vui_text) is
 * immediate, so the batcher is flushed before any text and all text of a screen is drawn last.
 * The 3D guidance rides the debug-line batch (td5_render_debug_line_world), the same path the
 * collision wireframe uses.
 *
 * INPUT. Scan codes only (td5_plat_input_key_pressed), edge-detected here once per rendered
 * frame, so a real keyboard and the InputScript harness (td5_plat_input_inject_key) take the
 * same path. The name entry reads A-Z / 0-9 / - from scan codes for the same reason.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdarg.h>

#include "td5_platform.h"
#include "td5_config.h"
#include "td5_fp.h"
#include "td5re.h"
#include "td5_types.h"
#include "td5_render.h"
#include "td5_vectorui.h"
#include "td5_hud.h"
#include "td5_race_state.h"
#include "td5_geo_roads.h"
#include "td5_page_map.h"      /* HUD_WHITE_TEX_PAGE */
#include "td5_roam_mode.h"
#include "td5_roam_physics.h"
#include "td5_roam_stream.h"
#include "td5_roam_route.h"
#include "td5_roam_route_ui.h"
#include "../../../re/include/td5_actor_struct.h"

#define LOG_TAG "roam"

/* DIK scan codes */
#define K_ESC 0x01
#define K_MINUS 0x0C
#define K_EQUALS 0x0D
#define K_BACK 0x0E
#define K_ENTER 0x1C
#define K_LSHIFT 0x2A
#define K_RSHIFT 0x36
#define K_SPACE 0x39
#define K_F6 0x40
#define K_F7 0x41
#define K_F8 0x42
#define K_NPSUB 0x4A
#define K_NPADD 0x4E
#define K_UP 0xC8
#define K_DOWN 0xD0
#define K_LEFT 0xCB
#define K_RIGHT 0xCD
#define K_PGUP 0xC9
#define K_PGDN 0xD1
#define K_DELETE 0xD3
#define K_M 0x32
#define K_X 0x2D
#define K_C 0x2E
#define K_F 0x21
#define K_S 0x1F
#define K_L 0x26

#define PI_D 3.14159265358979323846

/* ============================================================ state ========== */

static struct {
    int      active, map_open, open_pending, want_pause;
    uint8_t  cur[256], prev[256];
    uint32_t last_ms;
    /* map view */
    double   cx, cz, ppm;               /* centre (place frame), pixels per metre */
    int      mode;                      /* 0 map, 1 name entry, 2 load list */
    char     name[TD5_ROUTE_NAME_MAX];
    int      name_len;
    char     list[32][TD5_ROUTE_NAME_MAX];
    int      nlist, sel;
    int      saved_flash;
    /* mouse */
    int      m_down, m_drag, m_pin, m_x0, m_y0, m_x, m_y, m_has, m_btn;
    long     map_frames;                /* frames since the map opened (dev mouse script clock) */
    double   m_cx0, m_cz0;
    uint32_t m_moved_ms;
    /* messages */
    char     toast[96];
    uint32_t toast_ms;
    /* guidance */
    int      hint;
} u;

static void ui_toast(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(u.toast, sizeof u.toast, fmt, ap);
    va_end(ap);
    u.toast_ms = td5_plat_time_ms();
    TD5_LOG_I(LOG_TAG, "ROUTE_UI: %s", u.toast);
}

static int ke(int sc) { return u.cur[sc] && !u.prev[sc]; }
static int kd(int sc) { return u.cur[sc]; }

/* ============================================================ car ============ */

static int car_place(double *px, double *pz, int *yaw12)
{
    TD5_Actor *a = td5_game_get_actor(0);
    int32_t ox, oz;
    if (!a) return 0;
    td5_roam_world_origin(&ox, &oz);
    *px = (double)FP_TRUNC(a->world_pos.x) + (double)ox;
    *pz = (double)FP_TRUNC(a->world_pos.z) + (double)oz;
    if (yaw12) *yaw12 = (int)((a->euler_accum.yaw >> 8) & 0xFFF);
    return 1;
}

static void route_summary(char *out, size_t cap)
{
    const TD5_RouteResult *r = td5_roam_route_result();
    const int n = td5_roam_route_wp_count();
    if (n == 0)       snprintf(out, cap, "NO ROUTE");
    else if (n == 1)  snprintf(out, cap, "1 POINT   ADD A FINISH");
    else if (!r->ok)  snprintf(out, cap, "%d POINTS  NO ROUTE FOUND", n);
    else if (r->length_m >= 1000.0)
        snprintf(out, cap, "%d POINTS   %.2f KM   %d STREETS", n, r->length_m / 1000.0, r->nedge);
    else
        snprintf(out, cap, "%d POINTS   %.0f M   %d STREETS", n, r->length_m, r->nedge);
}

/* ============================================================ batcher ========= */

#define RB_QUADS 6000
static TD5_D3DVertex rb_v[RB_QUADS * 4];
static uint16_t      rb_i[RB_QUADS * 6];
static int           rb_nq;

static void rb_flush(void)
{
    if (rb_nq > 0) {
        td5_hud_prim_tris(rb_v, rb_nq * 4, rb_i, rb_nq * 6);
        rb_nq = 0;
    }
}

static void rb_quad(float x0, float y0, float x1, float y1, float x2, float y2, float x3, float y3, uint32_t col)
{
    TD5_D3DVertex *v;
    uint16_t *ix;
    const uint16_t b = (uint16_t)(rb_nq * 4);
    if (rb_nq >= RB_QUADS) rb_flush();
    v = &rb_v[rb_nq * 4];
    ix = &rb_i[rb_nq * 6];
    v[0].screen_x = x0; v[0].screen_y = y0;
    v[1].screen_x = x1; v[1].screen_y = y1;
    v[2].screen_x = x2; v[2].screen_y = y2;
    v[3].screen_x = x3; v[3].screen_y = y3;
    {
        int k;
        for (k = 0; k < 4; k++) {
            v[k].depth_z = 0.0f; v[k].rhw = 1.0f; v[k].diffuse = col; v[k].specular = 0;
            v[k].tex_u = 0.5f; v[k].tex_v = 0.5f;
        }
    }
    ix[0] = b; ix[1] = (uint16_t)(b + 1); ix[2] = (uint16_t)(b + 2);
    ix[3] = b; ix[4] = (uint16_t)(b + 2); ix[5] = (uint16_t)(b + 3);
    rb_nq++;
}

static void rb_rect(float x, float y, float w, float h, uint32_t col)
{
    rb_quad(x, y, x + w, y, x + w, y + h, x, y + h, col);
}

static void rb_line(float x0, float y0, float x1, float y1, float w, uint32_t col)
{
    const float dx = x1 - x0, dy = y1 - y0;
    const float len = sqrtf(dx * dx + dy * dy);
    float nx, ny;
    const float h = w * 0.5f;
    if (len < 0.001f) { rb_rect(x0 - h, y0 - h, w, w, col); return; }
    nx = -dy / len * h; ny = dx / len * h;
    rb_quad(x0 + nx, y0 + ny, x1 + nx, y1 + ny, x1 - nx, y1 - ny, x0 - nx, y0 - ny, col);
}

static void rb_tri(float x0, float y0, float x1, float y1, float x2, float y2, uint32_t col)
{
    rb_quad(x0, y0, x1, y1, x2, y2, x2, y2, col);
}

static void rb_disc(float cx, float cy, float r, uint32_t col)
{
    int k;
    float px = cx + r, py = cy;
    for (k = 1; k <= 8; k++) {
        const float a = (float)k * (float)(PI_D / 4.0);
        const float nx = cx + cosf(a) * r, ny = cy + sinf(a) * r;
        rb_tri(cx, cy, px, py, nx, ny, col);
        px = nx; py = ny;
    }
}

/* text: sizes in "pixels of cap height" are approximate, calibrated by eye */
static float s_text_k = 0.0f;           /* scale per requested pixel height */

static void ui_text(float x, float y, const char *s, uint32_t col, float px)
{
    if (!s_text_k) s_text_k = 1.0f / 26.0f;
    rb_flush();
    td5_vui_text(x, y, s, col, px * s_text_k, px * s_text_k);
}
static void ui_text_c(float cx, float y, const char *s, uint32_t col, float px)
{
    if (!s_text_k) s_text_k = 1.0f / 26.0f;
    rb_flush();
    td5_vui_text_centered(cx, y, s, col, px * s_text_k, px * s_text_k);
}
static float ui_text_w(const char *s, float px)
{
    if (!s_text_k) s_text_k = 1.0f / 26.0f;
    return td5_vui_text_width(s, px * s_text_k);
}

/* ============================================================ projection ====== */

typedef struct {
    double cx, cz;          /* centre, place frame */
    double k;               /* pixels per place unit */
    double m00, m01, m10, m11;
    float  ox, oy;          /* screen position of the centre */
    float  x0, y0, x1, y1;  /* clip rect, pixels */
} Proj;

static void proj_pt(const Proj *p, double X, double Z, float *sx, float *sy)
{
    const double dx = X - p->cx, dz = Z - p->cz;
    *sx = p->ox + (float)(p->k * (dx * p->m00 + dz * p->m01));
    *sy = p->oy + (float)(p->k * (dx * p->m10 + dz * p->m11));
}

static void proj_inv_northup(const Proj *p, float sx, float sy, double *X, double *Z)
{
    *X = p->cx + (double)(sx - p->ox) / p->k;
    *Z = p->cz - (double)(sy - p->oy) / p->k;
}

static int seg_visible(const Proj *p, float ax, float ay, float bx, float by, float m)
{
    if (ax < p->x0 - m && bx < p->x0 - m) return 0;
    if (ax > p->x1 + m && bx > p->x1 + m) return 0;
    if (ay < p->y0 - m && by < p->y0 - m) return 0;
    if (ay > p->y1 + m && by > p->y1 + m) return 0;
    return 1;
}

typedef struct {
    const Proj *p;
    int   min_rank;
    float w_rank[3];
    uint32_t col_rank[3];
    float ppm_px;           /* pixels per metre, for real-width roads */
    int   segs;
} StreetCtx;

static void street_cb(void *user, int klass, int lanes, int oneway, const int32_t *xz, int npt)
{
    StreetCtx *c = (StreetCtx *)user;
    const int rank = klass >= TD5_GEO_RC_SECONDARY ? 2 : (klass >= TD5_GEO_RC_RESIDENTIAL ? 1 : 0);
    float lx = 0, ly = 0, w;
    int i, have = 0;
    (void)oneway;
    if (rank < c->min_rank) return;
    w = c->w_rank[rank];
    {
        const float real = (float)lanes * 3.0f * c->ppm_px;
        if (real > w) w = real;
    }
    for (i = 0; i < npt; i++) {
        float sx, sy;
        proj_pt(c->p, (double)xz[2 * i], (double)xz[2 * i + 1], &sx, &sy);
        if (!have) { lx = sx; ly = sy; have = 1; continue; }
        if ((sx - lx) * (sx - lx) + (sy - ly) * (sy - ly) < 3.0f && i != npt - 1) continue;
        if (seg_visible(c->p, lx, ly, sx, sy, w)) { rb_line(lx, ly, sx, sy, w, c->col_rank[rank]); c->segs++; }
        lx = sx; ly = sy;
    }
}

static void link_cb(void *user, int klass, double x0, double z0, double x1, double z1)
{
    StreetCtx *c = (StreetCtx *)user;
    const int rank = klass >= TD5_GEO_RC_SECONDARY ? 2 : (klass >= TD5_GEO_RC_RESIDENTIAL ? 1 : 0);
    float ax, ay, bx, by, w;
    if (rank < c->min_rank) return;
    w = c->w_rank[rank];
    { const float real = 6.0f * c->ppm_px; if (real > w) w = real; }
    proj_pt(c->p, x0, z0, &ax, &ay);
    proj_pt(c->p, x1, z1, &bx, &by);
    if (seg_visible(c->p, ax, ay, bx, by, w)) { rb_line(ax, ay, bx, by, w, c->col_rank[rank]); c->segs++; }
}

static void draw_streets(const Proj *p, double half_w_units, double half_h_units, int min_rank, float ppm_px,
                         float scale)
{
    StreetCtx c;
    memset(&c, 0, sizeof c);
    c.p = p; c.min_rank = min_rank; c.ppm_px = ppm_px;
    c.w_rank[0] = 1.0f * scale; c.w_rank[1] = 1.6f * scale; c.w_rank[2] = 2.6f * scale;
    c.col_rank[0] = 0xFF3A424C; c.col_rank[1] = 0xFF59636F; c.col_rank[2] = 0xFF8C97A3;
    td5_roam_route_graph_query(p->cx - half_w_units, p->cz - half_h_units, p->cx + half_w_units, p->cz + half_h_units,
                               street_cb, &c);
    td5_roam_route_graph_links(p->cx - half_w_units, p->cz - half_h_units, p->cx + half_w_units, p->cz + half_h_units,
                               link_cb, &c);
}

/* the route line: dark casing, bright core, direction chevrons */
static void draw_route(const Proj *p, float core, float casing, float chevron_every)
{
    const TD5_RouteResult *r = td5_roam_route_result();
    int pass, i;
    if (!r->npt || r->npt < 2) return;
    for (pass = 0; pass < 2; pass++) {
        float lx = 0, ly = 0, acc = 0.0f;
        int have = 0;
        const float w = pass == 0 ? casing : core;
        const uint32_t col = pass == 0 ? 0xFF1A1200 : 0xFFFFB400;
        for (i = 0; i < r->npt; i++) {
            float sx, sy;
            proj_pt(p, r->px[i], r->pz[i], &sx, &sy);
            if (!have) { lx = sx; ly = sy; have = 1; continue; }
            if ((sx - lx) * (sx - lx) + (sy - ly) * (sy - ly) < 2.25f && i != r->npt - 1) continue;
            if (seg_visible(p, lx, ly, sx, sy, w)) {
                rb_line(lx, ly, sx, sy, w, col);
                if (w >= 3.0f) rb_rect(sx - w * 0.5f, sy - w * 0.5f, w, w, col);
                if (pass == 1 && chevron_every > 0.0f) {
                    const float dx = sx - lx, dy = sy - ly, len = sqrtf(dx * dx + dy * dy);
                    acc += len;
                    if (acc >= chevron_every && len > 0.5f) {
                        const float ux = dx / len, uy = dy / len, a = core * 1.3f;
                        acc = 0.0f;
                        rb_line(sx - ux * a - uy * a, sy - uy * a + ux * a, sx, sy, core * 0.55f, 0xFF1A1200);
                        rb_line(sx - ux * a + uy * a, sy - uy * a - ux * a, sx, sy, core * 0.55f, 0xFF1A1200);
                    }
                }
            }
            lx = sx; ly = sy;
        }
    }
}

static uint32_t pin_col(int kind)
{
    return kind == TD5_ROUTE_WP_START ? 0xFF2BD84B : (kind == TD5_ROUTE_WP_FINISH ? 0xFFEE3B3B : 0xFF3A98FF);
}

static void draw_pins(const Proj *p, float r, int numbered)
{
    int i;
    (void)numbered;
    for (i = 0; i < td5_roam_route_wp_count(); i++) {
        const TD5_RouteWp *w = td5_roam_route_wp(i);
        float sx, sy;
        if (!w) continue;
        proj_pt(p, w->snapped ? w->sx : (double)w->px, w->snapped ? w->sz : (double)w->pz, &sx, &sy);
        if (sx < p->x0 - r || sx > p->x1 + r || sy < p->y0 - r || sy > p->y1 + r) continue;
        rb_disc(sx, sy, r + 1.5f, 0xFF0A0A0A);
        rb_disc(sx, sy, r, pin_col(w->kind));
    }
}

static void draw_car(const Proj *p, double X, double Z, int yaw12, float sz)
{
    float sx, sy;
    const double y = (double)yaw12 * (2.0 * PI_D / 4096.0);
    /* forward in place = (sin y, cos y); screen forward = projected direction */
    float fx, fy, tx, ty;
    proj_pt(p, X, Z, &sx, &sy);
    proj_pt(p, X + sin(y) * 100.0, Z + cos(y) * 100.0, &tx, &ty);
    fx = tx - sx; fy = ty - sy;
    {
        const float l = sqrtf(fx * fx + fy * fy);
        if (l < 0.0001f) { fx = 0; fy = -1; } else { fx /= l; fy /= l; }
    }
    rb_tri(sx + fx * sz * 1.3f, sy + fy * sz * 1.3f,
           sx - fx * sz * 0.8f - fy * sz * 0.8f, sy - fy * sz * 0.8f + fx * sz * 0.8f,
           sx - fx * sz * 0.8f + fy * sz * 0.8f, sy - fy * sz * 0.8f - fx * sz * 0.8f, 0xFF101010);
    rb_tri(sx + fx * sz, sy + fy * sz,
           sx - fx * sz * 0.6f - fy * sz * 0.6f, sy - fy * sz * 0.6f + fx * sz * 0.6f,
           sx - fx * sz * 0.6f + fy * sz * 0.6f, sy - fy * sz * 0.6f - fx * sz * 0.6f, 0xFF35E8FF);
}

/* ============================================================ the map overlay == */

static void view_defaults(void)
{
    double px, pz;
    if (car_place(&px, &pz, NULL)) { u.cx = px; u.cz = pz; } else { u.cx = 0; u.cz = 0; }
    u.ppm = 0.45;
}

static void map_open_now(void)
{
    if (u.map_open) return;
    u.map_open = 1; u.mode = 0; u.open_pending = 4; u.map_frames = 0;
    view_defaults();
    td5_plat_set_os_cursor_visible(1);
    u.m_down = 0; u.m_drag = 0; u.m_pin = -1; u.m_has = 0;
    TD5_LOG_I(LOG_TAG, "ROUTE_UI: map open (waypoints=%d)", td5_roam_route_wp_count());
}

static void map_close(void)
{
    if (!u.map_open) return;
    u.map_open = 0; u.mode = 0;
    td5_plat_set_os_cursor_visible(0);
    TD5_LOG_I(LOG_TAG, "ROUTE_UI: map closed (waypoints=%d)", td5_roam_route_wp_count());
}

int td5_roam_route_ui_map_open(void) { return u.map_open; }

int td5_roam_route_ui_want_pause(void)
{
    const int w = u.want_pause;
    u.want_pause = 0;
    return w;
}

static float ui_scale(void)
{
    const float s = g_render_height_f / 720.0f;
    return s < 0.6f ? 0.6f : s;
}

static void map_layout(float *top, float *bot)
{
    const float S = ui_scale();
    *top = 34.0f * S;
    *bot = 54.0f * S;
}

static void map_proj(Proj *p)
{
    float top, bot;
    memset(p, 0, sizeof *p);
    map_layout(&top, &bot);
    p->cx = u.cx; p->cz = u.cz;
    p->k = u.ppm / 430.0;
    p->m00 = 1; p->m01 = 0; p->m10 = 0; p->m11 = -1;
    p->x0 = 0; p->x1 = g_render_width_f; p->y0 = top; p->y1 = g_render_height_f - bot;
    p->ox = g_render_width_f * 0.5f; p->oy = (p->y0 + p->y1) * 0.5f;
}

static void place_at(double X, double Z, int op)   /* 0 auto, 1 start, 2 via, 3 finish */
{
    const int32_t px = (int32_t)floor(X + 0.5), pz = (int32_t)floor(Z + 0.5);
    int ok = 0;
    const char *what = "";
    if (op == 0) op = td5_roam_route_wp_count() == 0 ? 1 : 2;
    if (op == 1)      { ok = td5_roam_route_set_start_at(px, pz);  what = "START"; }
    else if (op == 2) { ok = td5_roam_route_add_at(px, pz);        what = "WAYPOINT"; }
    else              { ok = td5_roam_route_set_finish_at(px, pz); what = "FINISH"; }
    if (ok) {
        char s[64];
        route_summary(s, sizeof s);
        ui_toast("%s SET   %s", what, s);
    } else {
        ui_toast("NO STREET WITHIN 250 M OF THAT SPOT");
    }
}

static void name_input(void)
{
    static const struct { uint8_t sc; char c; } k_chars[] = {
        {0x02,'1'},{0x03,'2'},{0x04,'3'},{0x05,'4'},{0x06,'5'},{0x07,'6'},{0x08,'7'},{0x09,'8'},{0x0A,'9'},{0x0B,'0'},
        {0x10,'q'},{0x11,'w'},{0x12,'e'},{0x13,'r'},{0x14,'t'},{0x15,'y'},{0x16,'u'},{0x17,'i'},{0x18,'o'},{0x19,'p'},
        {0x1E,'a'},{0x1F,'s'},{0x20,'d'},{0x21,'f'},{0x22,'g'},{0x23,'h'},{0x24,'j'},{0x25,'k'},{0x26,'l'},
        {0x2C,'z'},{0x2D,'x'},{0x2E,'c'},{0x2F,'v'},{0x30,'b'},{0x31,'n'},{0x32,'m'}
    };
    size_t i;
    const int shift = kd(K_LSHIFT) || kd(K_RSHIFT);
    if (ke(K_ESC)) { u.mode = 0; ui_toast("SAVE CANCELLED"); return; }
    if (ke(K_BACK)) { if (u.name_len > 0) u.name[--u.name_len] = '\0'; return; }
    if (ke(K_ENTER)) {
        if (!td5_roam_route_name_ok(u.name)) { ui_toast("NAME: LETTERS, DIGITS, - AND _ ONLY"); return; }
        if (td5_roam_route_save(u.name)) { ui_toast("SAVED  %s", u.name); u.mode = 0; }
        else ui_toast("SAVE FAILED (NEED START AND ANOTHER POINT)");
        return;
    }
    if (ke(K_MINUS) && u.name_len < TD5_ROUTE_NAME_MAX - 1) { u.name[u.name_len++] = shift ? '_' : '-'; u.name[u.name_len] = '\0'; return; }
    for (i = 0; i < sizeof k_chars / sizeof k_chars[0]; i++) {
        if (ke(k_chars[i].sc) && u.name_len < TD5_ROUTE_NAME_MAX - 1) {
            char c = k_chars[i].c;
            if (shift && c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
            u.name[u.name_len++] = c; u.name[u.name_len] = '\0';
        }
    }
}

static void fit_route(void);

static void list_input(void)
{
    if (ke(K_ESC)) { u.mode = 0; return; }
    if (u.nlist == 0) return;
    if (ke(K_UP))   u.sel = (u.sel + u.nlist - 1) % u.nlist;
    if (ke(K_DOWN)) u.sel = (u.sel + 1) % u.nlist;
    if (ke(K_ENTER)) {
        if (td5_roam_route_load(u.list[u.sel])) {
            const TD5_RouteResult *r = td5_roam_route_result();
            ui_toast("LOADED  %s   %.2f KM  %d STREETS%s", u.list[u.sel], r->length_m / 1000.0, r->nedge,
                     td5_roam_route_map_changed() ? "  MAP CHANGED" : "");
            u.mode = 0;
            fit_route();
        } else {
            ui_toast("COULD NOT LOAD %s", u.list[u.sel]);
        }
    }
}

static void fit_route(void)
{
    const TD5_RouteResult *r = td5_roam_route_result();
    double minx = 1e30, maxx = -1e30, minz = 1e30, maxz = -1e30;
    int i;
    float top, bot;
    if (!r->npt) { view_defaults(); return; }
    for (i = 0; i < r->npt; i++) {
        if (r->px[i] < minx) minx = r->px[i];
        if (r->px[i] > maxx) maxx = r->px[i];
        if (r->pz[i] < minz) minz = r->pz[i];
        if (r->pz[i] > maxz) maxz = r->pz[i];
    }
    map_layout(&top, &bot);
    {
        const double w_m = (maxx - minx) / 430.0 + 80.0, h_m = (maxz - minz) / 430.0 + 80.0;
        const double kx = g_render_width_f * 0.9 / w_m, kz = (g_render_height_f - top - bot) * 0.9 / h_m;
        u.ppm = kx < kz ? kx : kz;
        if (u.ppm < 0.04) u.ppm = 0.04;
        if (u.ppm > 3.0) u.ppm = 3.0;
    }
    u.cx = (minx + maxx) * 0.5; u.cz = (minz + maxz) * 0.5;
}

#ifndef TD5RE_RELEASE
/* DEV: TD5RE_ROAM_ROUTE_MOUSE="frame:x,y,down/frame:x,y,down/..." replaces the real mouse while the map is
 * open: the state at map frame N (frames since the map opened) is the last entry with frame <= N, x,y in
 * render pixels. Lets a test run the click / drag code without a real pointer (a synthetic OS click would
 * land on whatever window is on top). Inert when the variable is unset. */
static int fake_mouse(int *mx, int *my, int *down)
{
    static int init, n;
    static struct { long f; int x, y, d; } e[48];
    int i, have = 0;
    if (!init) {
        const char *s = getenv("TD5RE_ROAM_ROUTE_MOUSE");
        init = 1;
        while (s && *s && n < 48) {
            long f; int x, y, d, used = 0;
            if (sscanf(s, "%ld:%d,%d,%d%n", &f, &x, &y, &d, &used) < 4) break;
            e[n].f = f; e[n].x = x; e[n].y = y; e[n].d = d; n++;
            s += used;
            if (*s == '|' || *s == '/') s++;
        }
        if (n) TD5_LOG_W(LOG_TAG, "ROUTE_UI: dev mouse script with %d step(s) (TD5RE_ROAM_ROUTE_MOUSE)", n);
    }
    if (!n) return 0;
    for (i = 0; i < n; i++) if (e[i].f <= u.map_frames) { *mx = e[i].x; *my = e[i].y; *down = e[i].d; have = 1; }
    return have;
}
#endif

static void map_input(double dt)
{
    u.map_frames++;
    Proj p;
    int wheel;
    int mx = 0, my = 0, cw = 0, ch = 0;
    const int shift = kd(K_LSHIFT) || kd(K_RSHIFT);

    if (u.mode == 1) { name_input(); return; }
    if (u.mode == 2) { list_input(); return; }

    map_proj(&p);

    /* ---- keyboard ---- */
    {
        const double pan = (shift ? 1500.0 : 520.0) * dt / u.ppm * 430.0;   /* place units */
        if (kd(K_LEFT))  u.cx -= pan;
        if (kd(K_RIGHT)) u.cx += pan;
        if (kd(K_UP))    u.cz += pan;
        if (kd(K_DOWN))  u.cz -= pan;
        if (kd(K_EQUALS) || kd(K_NPADD) || kd(K_PGUP)) u.ppm *= exp(1.6 * dt);
        if (kd(K_MINUS) || kd(K_NPSUB) || kd(K_PGDN))  u.ppm /= exp(1.6 * dt);
    }
    wheel = td5_plat_input_get_mouse_wheel();
    if (wheel) u.ppm *= pow(1.18, (double)wheel / 120.0);
    if (u.ppm < 0.04) u.ppm = 0.04;
    if (u.ppm > 3.0) u.ppm = 3.0;

    if (ke(K_ESC) || ke(K_M)) {
        const int to_drive = ke(K_ESC);
        map_close();
        (void)to_drive;
        return;
    }
    if (ke(K_ENTER) || ke(K_SPACE)) place_at(u.cx, u.cz, 0);
    if (ke(0x02)) place_at(u.cx, u.cz, 1);
    if (ke(0x03)) place_at(u.cx, u.cz, 2);
    if (ke(0x04)) place_at(u.cx, u.cz, 3);
    if (ke(K_BACK))   { if (td5_roam_route_undo()) ui_toast("UNDO"); else ui_toast("NOTHING TO UNDO"); }
    if (ke(K_DELETE)) { if (td5_roam_route_clear()) ui_toast("ROUTE CLEARED"); }
    if (ke(K_C)) view_defaults();
    if (ke(K_F)) fit_route();
    if (ke(K_X)) {
        int idx;
        if (u.m_has && td5_plat_time_ms() - u.m_moved_ms < 3000) {
            double X, Z;
            proj_inv_northup(&p, (float)u.m_x, (float)u.m_y, &X, &Z);
            idx = td5_roam_route_pick(X, Z, 30.0 / u.ppm);
        } else {
            idx = td5_roam_route_pick(u.cx, u.cz, 30.0 / u.ppm);
        }
        if (idx >= 0 && td5_roam_route_remove(idx)) ui_toast("POINT REMOVED"); else ui_toast("NO POINT NEAR THE CURSOR");
    }
    if (ke(K_S)) {
        if (td5_roam_route_wp_count() < 2 || !td5_roam_route_result()->ok) {
            ui_toast("NEED A START, A FINISH AND A ROUTE BETWEEN THEM");
        } else {
            u.mode = 1;
            snprintf(u.name, sizeof u.name, "%s", td5_roam_route_current_name());
            if (!u.name[0]) {
                char names[32][TD5_ROUTE_NAME_MAX];
                const int n = td5_roam_route_list(names, 32);
                snprintf(u.name, sizeof u.name, "route%d", n + 1);
            }
            u.name_len = (int)strlen(u.name);
        }
        return;
    }
    if (ke(K_L)) {
        u.nlist = td5_roam_route_list(u.list, 32);
        u.sel = 0;
        u.mode = 2;
        return;
    }

    /* ---- mouse ---- */
    {
        int fdown = 0, have = 0;
#ifndef TD5RE_RELEASE
        have = fake_mouse(&mx, &my, &fdown);
#endif
        if (!have && td5_plat_input_get_mouse_pos(&mx, &my, &cw, &ch) && cw > 0 && ch > 0) {
            mx = (int)((float)mx * g_render_width_f / (float)cw);
            my = (int)((float)my * g_render_height_f / (float)ch);
            fdown = td5_plat_input_mouse_left_down();
            have = 1;
        }
        if (have) {
            if (mx != u.m_x || my != u.m_y) u.m_moved_ms = td5_plat_time_ms();
            u.m_x = mx; u.m_y = my; u.m_has = 1;
        }
        u.m_btn = fdown;
    }
    if (u.m_has) {
        const int down = u.m_btn;
        const int in_map = u.m_y >= p.y0 && u.m_y <= p.y1;
        if (down && !u.m_down && in_map) {
            double X, Z;
            u.m_down = 1; u.m_drag = 0; u.m_x0 = u.m_x; u.m_y0 = u.m_y; u.m_cx0 = u.cx; u.m_cz0 = u.cz;
            proj_inv_northup(&p, (float)u.m_x, (float)u.m_y, &X, &Z);
            u.m_pin = td5_roam_route_pick(X, Z, 16.0 * ui_scale() / u.ppm);
        } else if (down && u.m_down) {
            const int dx = u.m_x - u.m_x0, dy = u.m_y - u.m_y0;
            if (!u.m_drag && dx * dx + dy * dy > 25) u.m_drag = 1;
            if (u.m_drag && u.m_pin < 0) {
                u.cx = u.m_cx0 - (double)dx / (u.ppm / 430.0);
                u.cz = u.m_cz0 + (double)dy / (u.ppm / 430.0);
            }
        } else if (!down && u.m_down) {
            double X, Z;
            u.m_down = 0;
            proj_inv_northup(&p, (float)u.m_x, (float)u.m_y, &X, &Z);
            if (u.m_drag && u.m_pin >= 0) {
                if (td5_roam_route_move(u.m_pin, (int32_t)floor(X + 0.5), (int32_t)floor(Z + 0.5)))
                    ui_toast("POINT MOVED");
                else ui_toast("NO STREET WITHIN 250 M OF THAT SPOT");
            } else if (!u.m_drag) {
                if (u.m_pin >= 0) ui_toast("DRAG THE PIN TO MOVE IT, X TO REMOVE IT");
                else if (in_map) place_at(X, Z, 0);
            }
            u.m_pin = -1; u.m_drag = 0;
        }
    }
}

void td5_roam_route_ui_draw_overlay(void)
{
    Proj p;
    const float W = g_render_width_f, H = g_render_height_f, S = ui_scale();
    float top, bot;
    char sum[96], line[160];
    const TD5_RouteResult *r = td5_roam_route_result();
    double px, pz;
    int yaw = 0, have_car;
    int min_rank;
    int i;

    if (W < 2.0f || H < 2.0f) return;
    map_layout(&top, &bot);
    map_proj(&p);
    have_car = car_place(&px, &pz, &yaw);

    td5_plat_render_set_preset(TD5_PRESET_TRANSLUCENT_LINEAR_HUD);
    rb_rect(0, 0, W, H, 0xFF0C1015);

    /* streets: the smaller the zoom the fewer classes */
    min_rank = u.ppm < 0.075 ? 2 : (u.ppm < 0.19 ? 1 : 0);
    draw_streets(&p, (W * 0.5 + 40.0) / p.k, ((p.y1 - p.y0) * 0.5 + 40.0) / p.k, min_rank, (float)u.ppm, S);
    draw_route(&p, 4.0f * S, 7.0f * S, 70.0f * S);
    if (have_car) draw_car(&p, px, pz, yaw, 9.0f * S);
    draw_pins(&p, 8.0f * S, 1);

    /* bars */
    rb_rect(0, 0, W, top, 0xF0161B22);
    rb_rect(0, top - 2.0f * S, W, 2.0f * S, 0xFF2A323D);
    rb_rect(0, H - bot, W, bot, 0xF0161B22);
    rb_rect(0, H - bot, W, 2.0f * S, 0xFF2A323D);

    /* reticle at the view centre */
    {
        const float cx = p.ox, cy = p.oy, a = 11.0f * S, g = 3.0f * S;
        rb_line(cx - a, cy, cx - g, cy, 1.6f * S, 0xFFFFFFFF);
        rb_line(cx + g, cy, cx + a, cy, 1.6f * S, 0xFFFFFFFF);
        rb_line(cx, cy - a, cx, cy - g, 1.6f * S, 0xFFFFFFFF);
        rb_line(cx, cy + g, cx, cy + a, 1.6f * S, 0xFFFFFFFF);
    }
    /* north arrow + scale bar */
    {
        const float nx = W - 38.0f * S, ny = top + 40.0f * S;
        rb_tri(nx, ny - 16.0f * S, nx - 7.0f * S, ny + 6.0f * S, nx + 7.0f * S, ny + 6.0f * S, 0xFFE8E8E8);
        {
            static const double k_len[] = { 20, 50, 100, 200, 500, 1000, 2000, 5000 };
            double want_m = 130.0 * S / u.ppm, use = k_len[0];
            int q;
            for (q = 0; q < 8; q++) if (k_len[q] <= want_m) use = k_len[q];
            {
                const float bw = (float)(use * u.ppm), bx = 18.0f * S, by = H - bot - 16.0f * S;
                rb_rect(bx, by, bw, 3.0f * S, 0xFFE8E8E8);
                rb_rect(bx, by - 4.0f * S, 2.0f * S, 8.0f * S, 0xFFE8E8E8);
                rb_rect(bx + bw - 2.0f * S, by - 4.0f * S, 2.0f * S, 8.0f * S, 0xFFE8E8E8);
                snprintf(line, sizeof line, use >= 1000.0 ? "%.0f KM" : "%.0f M", use >= 1000.0 ? use / 1000.0 : use);
                ui_text(bx + bw + 8.0f * S, by - 8.0f * S, line, 0xFFE8E8E8, 15.0f * S);
            }
        }
    }
    /* modal boxes (geometry first) */
    if (u.mode == 1 || u.mode == 2) {
        const float bw = 360.0f * S, bh = (u.mode == 1 ? 120.0f : 70.0f + 26.0f * (float)(u.nlist < 8 ? (u.nlist ? u.nlist : 1) : 8)) * S;
        const float bx = (W - bw) * 0.5f, by = (H - bh) * 0.5f;
        rb_rect(bx - 3.0f * S, by - 3.0f * S, bw + 6.0f * S, bh + 6.0f * S, 0xFFFFB400);
        rb_rect(bx, by, bw, bh, 0xFF11161D);
        if (u.mode == 1) rb_rect(bx + 16.0f * S, by + 52.0f * S, bw - 32.0f * S, 30.0f * S, 0xFF1F2733);
        else if (u.nlist > 0) {
            const int first = u.sel < 4 ? 0 : (u.sel - 4 > u.nlist - 8 ? (u.nlist - 8 < 0 ? 0 : u.nlist - 8) : u.sel - 4);
            rb_rect(bx + 10.0f * S, by + 46.0f * S + 26.0f * S * (float)(u.sel - first), bw - 20.0f * S, 24.0f * S, 0xFF25507A);
        }
    }

    /* ---- text pass ---- */
    route_summary(sum, sizeof sum);
    ui_text(14.0f * S, 7.0f * S, "ROUTE PLANNER", 0xFFFFB400, 19.0f * S);
    {
        const float w = ui_text_w(sum, 17.0f * S);
        ui_text((W - w) * 0.5f, 8.0f * S, sum, 0xFFFFFFFF, 17.0f * S);
    }
    {
        const char *nm = td5_roam_route_current_name();
        if (r->ok && r->npt > 0) snprintf(line, sizeof line, "%s%s  #%08X", nm[0] ? nm : "UNSAVED",
                                          td5_roam_route_map_changed() ? "  MAP CHANGED" : "", (unsigned)r->hash);
        else snprintf(line, sizeof line, "%s", nm[0] ? nm : "UNSAVED");
        {
            const float w = ui_text_w(line, 16.0f * S);
            ui_text(W - w - 14.0f * S, 9.0f * S, line, td5_roam_route_map_changed() ? 0xFFFF6A3A : 0xFFB8C2CF, 16.0f * S);
        }
    }
    ui_text(14.0f * S, H - bot + 7.0f * S,
            "ARROWS PAN   + / - OR WHEEL ZOOM   ENTER OR CLICK PLACE   1 START   2 WAYPOINT   3 FINISH   X REMOVE   DRAG A PIN TO MOVE",
            0xFFC8D0DA, 14.0f * S);
    ui_text(14.0f * S, H - bot + 29.0f * S,
            "BKSP UNDO   DEL CLEAR   C CAR   F FIT   S SAVE   L LOAD   M PAUSE MENU   ESC DRIVE",
            0xFFC8D0DA, 14.0f * S);
    /* pin labels */
    for (i = 0; u.mode == 0 && i < td5_roam_route_wp_count(); i++) {
        const TD5_RouteWp *w = td5_roam_route_wp(i);
        float sx, sy;
        char lab[16];
        if (!w) continue;
        proj_pt(&p, w->snapped ? w->sx : (double)w->px, w->snapped ? w->sz : (double)w->pz, &sx, &sy);
        if (sy < p.y0 + 10.0f || sy > p.y1 - 6.0f || sx < 6.0f || sx > W - 6.0f) continue;
        if (w->kind == TD5_ROUTE_WP_START) snprintf(lab, sizeof lab, "S");
        else if (w->kind == TD5_ROUTE_WP_FINISH) snprintf(lab, sizeof lab, "F");
        else snprintf(lab, sizeof lab, "%d", i);
        ui_text_c(sx, sy - 9.0f * S, lab, 0xFF000000, 15.0f * S);
    }
    /* toast */
    if (u.toast[0] && td5_plat_time_ms() - u.toast_ms < 3500) {
        const float w = ui_text_w(u.toast, 17.0f * S), bx = (W - w) * 0.5f - 12.0f * S, by = top + 12.0f * S;
        rb_rect(bx, by, w + 24.0f * S, 30.0f * S, 0xE0000000);
        ui_text(bx + 12.0f * S, by + 6.0f * S, u.toast, 0xFFFFFFFF, 17.0f * S);
    }
    /* modal text */
    if (u.mode == 1) {
        const float bw = 360.0f * S, bx = (W - bw) * 0.5f, by = (H - 120.0f * S) * 0.5f;
        char buf[64];
        ui_text(bx + 16.0f * S, by + 14.0f * S, "SAVE ROUTE AS", 0xFFFFB400, 18.0f * S);
        snprintf(buf, sizeof buf, "%s_", u.name);
        ui_text(bx + 24.0f * S, by + 56.0f * S, buf, 0xFFFFFFFF, 20.0f * S);
        ui_text(bx + 16.0f * S, by + 92.0f * S, "TYPE A NAME   ENTER SAVE   ESC CANCEL", 0xFFB8C2CF, 13.0f * S);
    } else if (u.mode == 2) {
        const float bw = 360.0f * S, bx = (W - bw) * 0.5f;
        const float bh = (70.0f + 26.0f * (float)(u.nlist < 8 ? (u.nlist ? u.nlist : 1) : 8)) * S, by = (H - bh) * 0.5f;
        ui_text(bx + 16.0f * S, by + 12.0f * S, "LOAD ROUTE", 0xFFFFB400, 18.0f * S);
        if (u.nlist == 0) ui_text(bx + 16.0f * S, by + 50.0f * S, "NO SAVED ROUTES YET", 0xFFB8C2CF, 16.0f * S);
        else {
            const int first = u.sel < 4 ? 0 : (u.sel - 4 > u.nlist - 8 ? (u.nlist - 8 < 0 ? 0 : u.nlist - 8) : u.sel - 4);
            for (i = 0; i < 8 && first + i < u.nlist; i++)
                ui_text(bx + 20.0f * S, by + 49.0f * S + 26.0f * S * (float)i, u.list[first + i], 0xFFFFFFFF, 17.0f * S);
        }
        ui_text(bx + 16.0f * S, by + bh - 24.0f * S, "UP / DOWN   ENTER LOAD   ESC CANCEL", 0xFFB8C2CF, 13.0f * S);
    }
    rb_flush();
    td5_plat_render_set_preset(TD5_PRESET_OPAQUE_LINEAR);
}

/* ============================================================ the HUD line ===== */

void td5_roam_route_ui_draw_hud(void)
{
    const float W = g_render_width_f, S = ui_scale();
    char sum[96];
    const int paused = td5_game_is_pause_menu_active();
    if (!td5_roam_active() || u.map_open || W < 2.0f) return;
    td5_plat_render_set_preset(TD5_PRESET_TRANSLUCENT_LINEAR_HUD);
    if (paused) {
        const float H = g_render_height_f;
        const char *t = "M  ROUTE MAP:  PLAN, SAVE AND LOAD A ROUTE";
        const float w = ui_text_w(t, 16.0f * S);
        rb_rect((W - w) * 0.5f - 12.0f * S, H - 52.0f * S, w + 24.0f * S, 30.0f * S, 0xB0000000);
        ui_text((W - w) * 0.5f, H - 46.0f * S, t, 0xFFFFB400, 16.0f * S);
        rb_flush();
        td5_plat_render_set_preset(TD5_PRESET_OPAQUE_LINEAR);
        return;
    }
    {
        const int toast = u.toast[0] && td5_plat_time_ms() - u.toast_ms < 3000;
        const char *hint = "F6 START   F7 WAYPOINT   F8 FINISH   BKSP UNDO   DEL CLEAR   M MAP";
        route_summary(sum, sizeof sum);
        {
            const char *l1 = toast ? u.toast : sum;
            const float w1 = ui_text_w(l1, 15.0f * S), w2 = ui_text_w(hint, 12.5f * S);
            const float bw = (w1 > w2 ? w1 : w2) + 24.0f * S;
            rb_rect((W - bw) * 0.5f, 4.0f * S, bw, 40.0f * S, 0x90000000);
            ui_text_c(W * 0.5f, 7.0f * S, l1, toast ? 0xFFFFE070 : 0xFFFFFFFF, 15.0f * S);
            ui_text_c(W * 0.5f, 26.0f * S, hint, 0xFFC0C8D2, 12.5f * S);
        }
    }
    rb_flush();
    td5_plat_render_set_preset(TD5_PRESET_OPAQUE_LINEAR);
}

/* ============================================================ the minimap ====== */

void td5_roam_route_ui_draw_minimap(float x, float y, float w, float h)
{
    Proj p;
    double px, pz;
    int yaw;
    const float S = w / 160.0f;
    if (!td5_roam_active() || !td5_roam_route_graph_ready()) return;
    if (!car_place(&px, &pz, &yaw)) return;
    {
        const double ya = (double)yaw * (2.0 * PI_D / 4096.0);
        const double half_m = 220.0;             /* the pane shows 440 m across */
        memset(&p, 0, sizeof p);
        p.cx = px; p.cz = pz;
        p.k = (double)(w * 0.5f) / (half_m * 430.0);
        p.m00 = cos(ya); p.m01 = -sin(ya); p.m10 = -sin(ya); p.m11 = -cos(ya);
        p.ox = x + w * 0.5f; p.oy = y + h * 0.5f;
        p.x0 = x; p.y0 = y; p.x1 = x + w; p.y1 = y + h;
        td5_plat_render_set_preset(TD5_PRESET_TRANSLUCENT_LINEAR_HUD);
        td5_render_set_clip_rect(x, x + w, y, y + h);
        rb_rect(x, y, w, h, 0xC00C1015);
        draw_streets(&p, half_m * 430.0 * 1.5, half_m * 430.0 * 1.5, 0, (float)(p.k * 430.0), 1.0f * S < 1.0f ? 1.0f : S * 0.9f);
        draw_route(&p, 3.0f * S, 5.5f * S, 40.0f * S);
        draw_pins(&p, 5.0f * S, 0);
        /* the car, always pointing up */
        rb_tri(p.ox, p.oy - 9.0f * S, p.ox - 6.0f * S, p.oy + 7.0f * S, p.ox + 6.0f * S, p.oy + 7.0f * S, 0xFF101010);
        rb_tri(p.ox, p.oy - 7.0f * S, p.ox - 4.5f * S, p.oy + 5.0f * S, p.ox + 4.5f * S, p.oy + 5.0f * S, 0xFF35E8FF);
        /* border */
        rb_rect(x, y, w, 1.5f * S, 0xFFB0B8C2);
        rb_rect(x, y + h - 1.5f * S, w, 1.5f * S, 0xFFB0B8C2);
        rb_rect(x, y, 1.5f * S, h, 0xFFB0B8C2);
        rb_rect(x + w - 1.5f * S, y, 1.5f * S, h, 0xFFB0B8C2);
        /* N marker on the rim */
        {
            const double nf = cos((double)yaw * (2.0 * PI_D / 4096.0)), nr = -sin((double)yaw * (2.0 * PI_D / 4096.0));
            const float nx = p.ox + (float)nr * (w * 0.5f - 10.0f * S), ny = p.oy - (float)nf * (h * 0.5f - 10.0f * S);
            rb_disc(nx, ny, 7.0f * S, 0xFF1A1F27);
            rb_flush();
            td5_vui_text_centered(nx, ny - 6.5f * S, "N", 0xFFFFFFFF, 11.0f * S / 26.0f, 11.0f * S / 26.0f);
        }
        rb_flush();
        td5_render_set_clip_rect(0.0f, g_render_width_f, 0.0f, g_render_height_f);
        td5_plat_render_set_preset(TD5_PRESET_OPAQUE_LINEAR);
    }
}

/* ============================================================ on-road guidance == */

static int route_nearest(const TD5_RouteResult *r, double px, double pz, int hint)
{
    int i, lo = 0, hi = r->npt - 1, best = -1;
    double bd = 1e300;
    if (hint >= 0 && hint < r->npt) { lo = hint - 80 < 0 ? 0 : hint - 80; hi = hint + 80 > r->npt - 1 ? r->npt - 1 : hint + 80; }
    for (i = lo; i <= hi; i++) {
        const double dx = r->px[i] - px, dz = r->pz[i] - pz, d = dx * dx + dz * dz;
        if (d < bd) { bd = d; best = i; }
    }
    if (hint >= 0 && bd > (60.0 * 430.0) * (60.0 * 430.0)) return route_nearest(r, px, pz, -1);   /* lost: global */
    return best;
}

static float gnd_y(double wx_units, double wz_units, int *ok)
{
    const int32_t x24 = (int32_t)(wx_units * 256.0), z24 = (int32_t)(wz_units * 256.0);
    if (td5_roam_stream_active() && td5_roam_stream_point_missing(x24, z24)) { *ok = 0; return 0.0f; }
    *ok = 1;
    return (float)td5_roam_ground(x24, z24, 0, 0, NULL) * (1.0f / 256.0f);
}

/* world-space quads: screen-space triangles with the debug lines' depth, drawn depth-tested and
 * blended (TD5_PRESET_TRANSLUCENT_POINT_ZTEST) so the ribbon lies on the road and the car is in front */
#define WQ_QUADS 900
static TD5_D3DVertex wq_v[WQ_QUADS * 4];
static uint16_t      wq_i[WQ_QUADS * 6];
static int           wq_n;

static void wq_flush(void)
{
    if (wq_n > 0) {
        td5_plat_render_set_preset(TD5_PRESET_TRANSLUCENT_POINT_ZTEST);
        td5_plat_render_bind_texture(HUD_WHITE_TEX_PAGE);
        td5_plat_render_draw_tris(wq_v, wq_n * 4, wq_i, wq_n * 6);
        wq_n = 0;
    }
}

/* four world points (render-float: place - origin, y) in order around the quad */
static void wq_quad(const float a[3], const float b[3], const float c[3], const float d[3], uint32_t col)
{
    TD5_D3DVertex *v;
    uint16_t *ix;
    const uint16_t base = (uint16_t)(wq_n * 4);
    if (wq_n >= WQ_QUADS) wq_flush();
    v = &wq_v[wq_n * 4];
    if (!td5_render_project_world_vertex(a[0], a[1], a[2], col, &v[0]) ||
        !td5_render_project_world_vertex(b[0], b[1], b[2], col, &v[1]) ||
        !td5_render_project_world_vertex(c[0], c[1], c[2], col, &v[2]) ||
        !td5_render_project_world_vertex(d[0], d[1], d[2], col, &v[3])) return;
    ix = &wq_i[wq_n * 6];
    ix[0] = base; ix[1] = (uint16_t)(base + 1); ix[2] = (uint16_t)(base + 2);
    ix[3] = base; ix[4] = (uint16_t)(base + 2); ix[5] = (uint16_t)(base + 3);
    wq_n++;
}

/* a flat strip from P0 to P1 (render-float xz, y0 / y1) of half width hw, shifted sideways by off */
static void wq_strip(double x0, double z0, float y0, double x1, double z1, float y1, double hw, uint32_t col,
                     double off)
{
    const double dx = x1 - x0, dz = z1 - z0, len = sqrt(dx * dx + dz * dz);
    double rx, rz;
    float a[3], b[3], c[3], d[3];
    if (len < 1.0) return;
    rx = dz / len; rz = -dx / len;
    a[0] = (float)(x0 + rx * (off - hw)); a[1] = y0; a[2] = (float)(z0 + rz * (off - hw));
    b[0] = (float)(x0 + rx * (off + hw)); b[1] = y0; b[2] = (float)(z0 + rz * (off + hw));
    c[0] = (float)(x1 + rx * (off + hw)); c[1] = y1; c[2] = (float)(z1 + rz * (off + hw));
    d[0] = (float)(x1 + rx * (off - hw)); d[1] = y1; d[2] = (float)(z1 + rz * (off - hw));
    wq_quad(a, b, c, d, col);
}

void td5_roam_route_ui_draw_world(int view)
{
    const TD5_RouteResult *r = td5_roam_route_result();
    double px, pz;
    int32_t ox, oz;
    int i, i0, i1, nwp = td5_roam_route_wp_count();
    const float lift = 70.0f;               /* 0.16 m above the road */
    const double M = 430.0;
    if (view != 0 || !td5_roam_active() || nwp < 1) return;
    if (!car_place(&px, &pz, NULL)) return;
    td5_roam_world_origin(&ox, &oz);

    /* beacons over the pins */
    for (i = 0; i < nwp; i++) {
        const TD5_RouteWp *w = td5_roam_route_wp(i);
        double X, Z, dx, dz;
        int ok;
        float gy;
        if (!w) continue;
        X = w->snapped ? w->sx : (double)w->px; Z = w->snapped ? w->sz : (double)w->pz;
        dx = X - px; dz = Z - pz;
        if (dx * dx + dz * dz > (900.0 * M) * (900.0 * M)) continue;
        gy = gnd_y(X - ox, Z - oz, &ok);
        if (!ok) continue;
        {
            const float rx = (float)(X - ox), rz = (float)(Z - oz), tall = (float)(30.0 * M);
            const uint32_t c = pin_col(w->kind);
            const float s = (float)(1.1 * M);
            td5_render_debug_line_world(rx, gy, rz, rx, gy + tall, rz, c);
            td5_render_debug_line_world(rx - s, gy + tall, rz, rx + s, gy + tall, rz, c);
            td5_render_debug_line_world(rx, gy + tall, rz - s, rx, gy + tall, rz + s, c);
            td5_render_debug_line_world(rx - s, gy, rz, rx + s, gy, rz, c);
            td5_render_debug_line_world(rx, gy, rz - s, rx, gy, rz + s, c);
        }
    }
    if (!r->ok || r->npt < 2) return;

    u.hint = route_nearest(r, px, pz, u.hint);
    if (u.hint < 0) return;
    i0 = u.hint;
    while (i0 > 0 && r->dist_m[u.hint] - r->dist_m[i0] < 10.0) i0--;
    i1 = u.hint;
    while (i1 + 1 < r->npt && r->dist_m[i1 + 1] - r->dist_m[u.hint] < 320.0) i1++;
    {
        double next_chev = r->dist_m[i0] + 4.0;
        for (i = i0; i < i1; i++) {
            const double ax = r->px[i] - ox, az = r->pz[i] - oz, bx = r->px[i + 1] - ox, bz = r->pz[i + 1] - oz;
            const double dx = bx - ax, dz = bz - az, len = sqrt(dx * dx + dz * dz);
            int ok_a, ok_b;
            float ya, yb;
            if (len < 1.0) continue;
            ya = gnd_y(ax, az, &ok_a) + lift; yb = gnd_y(bx, bz, &ok_b) + lift;
            if (!ok_a || !ok_b) continue;
            wq_strip(ax, az, ya, bx, bz, yb, 0.85 * M, 0x70FFB400, 0.0);          /* the amber band    */
            wq_strip(ax, az, ya, bx, bz, yb, 0.10 * M, 0xE0FFE070, 0.85 * M);     /* its bright edges  */
            wq_strip(ax, az, ya, bx, bz, yb, 0.10 * M, 0xE0FFE070, -0.85 * M);
            /* chevrons every 9 m: a V of two strips pointing along the route */
            while (next_chev < r->dist_m[i + 1]) {
                const double t = (next_chev - r->dist_m[i]) / (r->dist_m[i + 1] - r->dist_m[i] + 1e-9);
                const double fx = dx / len, fz = dz / len, rxn = fz, rzn = -fx;
                const double cxp = ax + dx * t, czp = az + dz * t;
                const float cy = ya + (yb - ya) * (float)t + 8.0f;
                const double tipx = cxp + fx * 1.5 * M, tipz = czp + fz * 1.5 * M;
                const double w1x = cxp - fx * 0.5 * M + rxn * 1.3 * M, w1z = czp - fz * 0.5 * M + rzn * 1.3 * M;
                const double w2x = cxp - fx * 0.5 * M - rxn * 1.3 * M, w2z = czp - fz * 0.5 * M - rzn * 1.3 * M;
                wq_strip(w1x, w1z, cy, tipx, tipz, cy, 0.17 * M, 0xF0FFFFFF, 0.0);
                wq_strip(tipx, tipz, cy, w2x, w2z, cy, 0.17 * M, 0xF0FFFFFF, 0.0);
                next_chev += 9.0;
            }
        }
    }
    wq_flush();
    td5_plat_render_set_preset(TD5_PRESET_OPAQUE_LINEAR);
}

/* ============================================================ frame ============ */

static void ui_begin(void)
{
    const char *place = td5_roam_route_place();
    memset(&u, 0, sizeof u);
    u.active = 1; u.m_pin = -1; u.hint = -1;
    td5_roam_route_reset();
    td5_roam_route_graph_begin_async(place);
    TD5_LOG_I(LOG_TAG, "ROUTE_UI: session begin place=%s (planner graph building in the background)", place);
}

static void ui_end(void)
{
    if (u.map_open) td5_plat_set_os_cursor_visible(0);
    TD5_LOG_I(LOG_TAG, "ROUTE_UI: session end");
    td5_roam_route_reset();
    if (!td5_roam_route_graph_building()) td5_roam_route_graph_free();   /* the planner graph is per session */
    memset(&u, 0, sizeof u);
}

static void drop_keys(void)
{
    double px, pz;
    if (!car_place(&px, &pz, NULL)) return;
    if (ke(K_F6)) place_at(px, pz, 1);
    if (ke(K_F7)) place_at(px, pz, 2);
    if (ke(K_F8)) place_at(px, pz, 3);
    if (ke(K_BACK))   { if (td5_roam_route_undo()) ui_toast("UNDO"); else ui_toast("NOTHING TO UNDO"); }
    if (ke(K_DELETE)) { if (td5_roam_route_clear()) ui_toast("ROUTE CLEARED"); }
}

void td5_roam_route_ui_frame(void)
{
    uint32_t now;
    double dt;
    int i;
    if (!td5_roam_active()) { if (u.active) ui_end(); return; }
    if (!u.active) ui_begin();
    memcpy(u.prev, u.cur, sizeof u.prev);
    for (i = 0; i < 256; i++) u.cur[i] = (uint8_t)(td5_plat_input_key_pressed(i) ? 1 : 0);
    now = td5_plat_time_ms();
    dt = u.last_ms ? (double)(now - u.last_ms) * 0.001 : 0.016;
    if (dt > 0.1) dt = 0.1;
    u.last_ms = now;

#ifndef TD5RE_RELEASE
    /* DEV: F10 dumps the last presented frame to <log dir>roam_route_shot_NN.png (script / harness) */
    if (ke(0x44)) {
        static int s_shot;
        char path[512];
        const char *dir = td5_plat_log_dir();
        snprintf(path, sizeof path, "%sroam_route_shot_%02d.png", dir ? dir : "", s_shot++);
        td5_plat_request_frame_dump(path);
        TD5_LOG_I(LOG_TAG, "ROUTE_UI: frame dump %s", path);
    }
#endif
    if (u.map_open) {
        if (!td5_game_is_pause_menu_active()) {
            if (u.open_pending > 0) u.open_pending--; else { map_close(); return; }
        } else {
            u.open_pending = 0;
        }
        map_input(dt);
        return;
    }
    if (td5_game_is_pause_menu_active()) {
        if (ke(K_M)) map_open_now();
        return;
    }
    if (ke(K_M)) { u.want_pause = 1; map_open_now(); return; }
    drop_keys();
}

#ifndef TD5RE_RELEASE
void td5_roam_route_ui_log_state(const char *tag)
{
    const TD5_RouteResult *r = td5_roam_route_result();
    TD5_LOG_I(LOG_TAG, "ROUTE_STATE %s: waypoints=%d ok=%d edges=%d length=%.1f m hash=%08X",
              tag ? tag : "", td5_roam_route_wp_count(), r->ok, r->nedge, r->length_m, (unsigned)r->hash);
}
#endif
