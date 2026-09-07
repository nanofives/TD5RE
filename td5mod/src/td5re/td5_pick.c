/**
 * td5_pick.c -- dev-only free-cam geometry/texture PICKER. See td5_pick.h.
 *
 * Active only while the free camera is active (single pane, sim paused, serial
 * render path). Per frame:
 *   begin_frame()  captures the cursor in render pixels and arms collection;
 *   consider()     [in the render mesh loop] keeps the nearest visible mesh
 *                  whose projected bounding disc contains the cursor;
 *   finish_frame() draws the highlight box and, on a left-click edge, copies
 *                  the hovered mesh's identity as JSON to the clipboard;
 *   hud_draw()     [in the HUD text pass] queues the on-screen label.
 *
 * Coordinate space: the render walk hands us bounding centres in RENDER-FLOAT
 * world space (world/256). td5_render_project_world takes exactly that space,
 * and td5_render_debug_line_world draws in it, so nothing is re-scaled here.
 */
#include "td5_pick.h"

#ifndef TD5RE_RELEASE

#include <stdio.h>
#include <string.h>
#include <math.h>

#include "td5re.h"
#include "td5_platform.h"
#include "td5_config.h"
#include "td5_camera.h"
#include "td5_render.h"
#include "td5_track.h"
#include "td5_trackgen.h"   /* auto-track page-name lookup */
#include "td5_asset.h"      /* shipped-level number for the import handle */
#include "td5_hud.h"

#define LOG_TAG "render"

/* --- config --- */
static int   s_enabled = -1;      /* TD5RE_PICK, default ON in dev */

/* --- per-frame collection state --- */
static int   s_collecting = 0;
static int   s_have_cursor = 0;
static float s_cursor_x = 0.0f, s_cursor_y = 0.0f;   /* render pixels */

/* --- best hover candidate this frame --- */
static int   s_best_valid = 0;
static const TD5_SpanDisplayList *s_best_block = NULL;
static int   s_best_slot = -1;
static const TD5_MeshHeader *s_best_mesh = NULL;
static float s_best_cx, s_best_cy, s_best_cz, s_best_r;
static float s_best_t = 0.0f;       /* nearest ray-hit distance (render units) */
static int   s_best_is_bb = 0;      /* winner is a camera-facing billboard */

/* Cursor ray + camera axes in render-float world space, built lazily on the
 * first consider() of the frame (camera state valid once the pane render starts). */
static int   s_ray_ready = 0;
static float s_ray_o[3], s_ray_d[3];
static float s_cam_right[3], s_cam_up[3];

/* [DIAG] per-frame counters for the on-screen readout (TD5RE_PICK_DIAG). */
static int   s_dbg = -1;
static int   s_dbg_seen, s_dbg_sphere, s_dbg_solidhit, s_dbg_bbhit;
static float s_dbg_v0d = -1.0f;   /* |firstvert - center| of a solid mesh (space check) */

/* --- resolved identity of the current hover (for HUD + click) --- */
static int   s_hud_valid = 0;
static char  s_hud_line[192];
static char  s_json[320];

/* --- copy-confirmation flash --- */
static int   s_flash = 0;

static int pick_enabled(void)
{
    if (s_enabled < 0) s_enabled = td5_env_flag_on("TD5RE_PICK");  /* default ON */
    return s_enabled;
}

int td5_pick_collecting(void) { return s_collecting; }

/* Moller-Trumbore ray/triangle. Returns 1 and sets *t on a front hit (t>eps). */
static int pick_ray_tri(const float v0[3], const float v1[3], const float v2[3],
                        float *t)
{
    float e1[3], e2[3], p[3], q[3], tv[3];
    float det, inv, u, v, tt;
    int i;
    for (i = 0; i < 3; i++) { e1[i] = v1[i] - v0[i]; e2[i] = v2[i] - v0[i]; }
    p[0] = s_ray_d[1]*e2[2] - s_ray_d[2]*e2[1];
    p[1] = s_ray_d[2]*e2[0] - s_ray_d[0]*e2[2];
    p[2] = s_ray_d[0]*e2[1] - s_ray_d[1]*e2[0];
    det = e1[0]*p[0] + e1[1]*p[1] + e1[2]*p[2];
    if (det > -1e-6f && det < 1e-6f) return 0;      /* ray parallel to tri */
    inv = 1.0f / det;
    for (i = 0; i < 3; i++) tv[i] = s_ray_o[i] - v0[i];
    u = (tv[0]*p[0] + tv[1]*p[1] + tv[2]*p[2]) * inv;
    if (u < 0.0f || u > 1.0f) return 0;
    q[0] = tv[1]*e1[2] - tv[2]*e1[1];
    q[1] = tv[2]*e1[0] - tv[0]*e1[2];
    q[2] = tv[0]*e1[1] - tv[1]*e1[0];
    v = (s_ray_d[0]*q[0] + s_ray_d[1]*q[1] + s_ray_d[2]*q[2]) * inv;
    if (v < 0.0f || u + v > 1.0f) return 0;
    tt = (e2[0]*q[0] + e2[1]*q[1] + e2[2]*q[2]) * inv;
    if (tt <= 1e-4f) return 0;                       /* behind / at the eye */
    *t = tt;
    return 1;
}

/* Intersect the cursor ray with a SOLID mesh's real quads/triangles. Sets
 * *out_t to the nearest front hit and returns 1 if any. Handles dispatch_type 0
 * (quads then tris, 4/3 verts each from a running cursor) -- what the auto-track
 * emitters produce; bails on any other opcode (unknown vertex stride). Vertices
 * are absolute render-float world for opaque meshes (origin 0), but origin is
 * added for robustness. */
static int pick_ray_mesh(const TD5_MeshHeader *mesh, float *out_t)
{
    const TD5_PrimitiveCmd *cmds = mesh->commands;
    const TD5_MeshVertex   *vtx  = mesh->vertices;
    int cc = mesh->command_count;
    int total = mesh->total_vertex_count;
    float ox = mesh->origin_x * (1.0f/256.0f);
    float oy = mesh->origin_y * (1.0f/256.0f);
    float oz = mesh->origin_z * (1.0f/256.0f);
    float best = 1e30f;
    int hit = 0, k, cursor = 0;

    if (!cmds || !vtx || cc <= 0 || cc > 4096) return 0;
    if (total <= 0 || total > 131072) return 0;

    #define PICK_VW(idx, out) do { \
        (out)[0] = vtx[(idx)].pos_x + ox; \
        (out)[1] = vtx[(idx)].pos_y + oy; \
        (out)[2] = vtx[(idx)].pos_z + oz; } while (0)

    for (k = 0; k < cc; k++) {
        int nq = cmds[k].quad_count;
        int nt = cmds[k].triangle_count;
        int q, tr;
        if (cmds[k].dispatch_type != 0) break;   /* unknown vertex stride */
        for (q = 0; q < nq; q++) {
            float a[3], b[3], c[3], d[3], t;
            if (cursor + 4 > total) { cursor = total; break; }
            PICK_VW(cursor + 0, a); PICK_VW(cursor + 1, b);
            PICK_VW(cursor + 2, c); PICK_VW(cursor + 3, d);
            if (pick_ray_tri(a, b, c, &t) && t < best) { best = t; hit = 1; }
            if (pick_ray_tri(a, c, d, &t) && t < best) { best = t; hit = 1; }
            cursor += 4;
        }
        for (tr = 0; tr < nt; tr++) {
            float a[3], b[3], c[3], t;
            if (cursor + 3 > total) { cursor = total; break; }
            PICK_VW(cursor + 0, a); PICK_VW(cursor + 1, b); PICK_VW(cursor + 2, c);
            if (pick_ray_tri(a, b, c, &t) && t < best) { best = t; hit = 1; }
            cursor += 3;
        }
    }
    #undef PICK_VW

    if (hit) *out_t = best;
    return hit;
}

/* Intersect the cursor ray with a camera-facing BILLBOARD (tree/sign). Its
 * stored vertices are LOCAL (x across, y up about a world origin), so we build
 * the world quad the renderer draws -- origin + x*right + y*up -- and ray-test
 * it. This is the tight test billboards need: without it a tall tree's whole
 * bounding sphere counted as a hit, so an off-axis tree stole the pick. */
static int pick_ray_billboard(const TD5_MeshHeader *mesh,
                              const float right[3], const float up[3], float *out_t)
{
    const TD5_PrimitiveCmd *cmds = mesh->commands;
    const TD5_MeshVertex   *vtx  = mesh->vertices;
    int cc = mesh->command_count, total = mesh->total_vertex_count;
    float o0 = mesh->origin_x * (1.0f/256.0f);
    float o1 = mesh->origin_y * (1.0f/256.0f);
    float o2 = mesh->origin_z * (1.0f/256.0f);
    float best = 1e30f;
    int hit = 0, k, cursor = 0;

    if (!cmds || !vtx || cc <= 0 || cc > 4096) return 0;
    if (total <= 0 || total > 131072) return 0;

    #define PICK_BBW(idx, out) do { \
        float lx = vtx[(idx)].pos_x, ly = vtx[(idx)].pos_y; \
        (out)[0] = o0 + lx*right[0] + ly*up[0]; \
        (out)[1] = o1 + lx*right[1] + ly*up[1]; \
        (out)[2] = o2 + lx*right[2] + ly*up[2]; } while (0)

    for (k = 0; k < cc; k++) {
        int nq = cmds[k].quad_count, nt = cmds[k].triangle_count, q, tr;
        if (cmds[k].dispatch_type != 0) break;
        for (q = 0; q < nq; q++) {
            float a[3], b[3], c[3], d[3], t;
            if (cursor + 4 > total) { cursor = total; break; }
            PICK_BBW(cursor+0, a); PICK_BBW(cursor+1, b);
            PICK_BBW(cursor+2, c); PICK_BBW(cursor+3, d);
            if (pick_ray_tri(a, b, c, &t) && t < best) { best = t; hit = 1; }
            if (pick_ray_tri(a, c, d, &t) && t < best) { best = t; hit = 1; }
            cursor += 4;
        }
        for (tr = 0; tr < nt; tr++) {
            float a[3], b[3], c[3], t;
            if (cursor + 3 > total) { cursor = total; break; }
            PICK_BBW(cursor+0, a); PICK_BBW(cursor+1, b); PICK_BBW(cursor+2, c);
            if (pick_ray_tri(a, b, c, &t) && t < best) { best = t; hit = 1; }
            cursor += 3;
        }
    }
    #undef PICK_BBW

    if (hit) *out_t = best;
    return hit;
}

void td5_pick_begin_frame(void)
{
    int cx, cy, cw, ch, rw, rh;

    s_collecting = 0;
    s_have_cursor = 0;
    s_best_valid = 0;
    s_ray_ready = 0;
    s_dbg_seen = s_dbg_sphere = s_dbg_solidhit = s_dbg_bbhit = 0;
    s_dbg_v0d = -1.0f;
    if (s_dbg < 0) { const char *e = getenv("TD5RE_PICK_DIAG");
                     s_dbg = (e && e[0] && e[0] != '0') ? 1 : 0; }  /* default OFF */

    if (!pick_enabled() || !td5_camera_freecam_active()) {
        s_hud_valid = 0;
        td5_plat_set_os_cursor_visible(0);   /* restore the hidden in-race cursor */
        return;
    }

    /* Show a real crosshair cursor while flying so the user sees where they aim. */
    td5_plat_set_os_cursor_visible(1);

    if (!td5_plat_input_get_mouse_pos(&cx, &cy, &cw, &ch) || cw <= 0 || ch <= 0)
        return;

    /* Scale client pixels -> render pixels. A flying camera is always a single
     * full-window pane, so the render viewport spans the whole client area and
     * projected screen_x/y are in render-pixel space (centre = render/2). */
    rw = g_td5.render_width  > 0 ? g_td5.render_width  : cw;
    rh = g_td5.render_height > 0 ? g_td5.render_height : ch;
    s_cursor_x = (float)cx * (float)rw / (float)cw;
    s_cursor_y = (float)cy * (float)rh / (float)ch;
    s_have_cursor = 1;
    s_collecting = 1;
}

void td5_pick_consider(const TD5_SpanDisplayList *block, int slot,
                       const TD5_MeshHeader *mesh,
                       float cx, float cy, float cz, float r)
{
    float oc[3], tca, dd2, thc, tsphere, t_hit;
    int is_billboard;

    if (!s_collecting || !s_have_cursor || !mesh) return;
    s_dbg_seen++;

    /* Build the cursor ray + camera axes once per frame (camera valid now). */
    if (!s_ray_ready) {
        td5_render_screen_ray(s_cursor_x, s_cursor_y, s_ray_o, s_ray_d);
        td5_render_get_camera_axes(s_cam_right, s_cam_up, NULL);
        s_ray_ready = 1;
    }

    /* Broad phase: ray vs bounding sphere. Rejects everything the ray misses. */
    oc[0] = cx - s_ray_o[0];
    oc[1] = cy - s_ray_o[1];
    oc[2] = cz - s_ray_o[2];
    tca = oc[0]*s_ray_d[0] + oc[1]*s_ray_d[1] + oc[2]*s_ray_d[2];
    if (tca < 0.0f) return;                       /* sphere is behind the camera */
    dd2 = (oc[0]*oc[0] + oc[1]*oc[1] + oc[2]*oc[2]) - tca*tca;
    if (dd2 > r*r) return;                         /* ray misses the sphere      */
    thc = sqrtf(r*r - dd2);
    tsphere = tca - thc;
    if (tsphere < 0.0f) tsphere = tca;             /* camera inside the sphere    */
    s_dbg_sphere++;

    (void)tsphere;
    is_billboard = (mesh->texture_page_id == 1 || mesh->texture_page_id == 2);

    {
        float best_local = 1e30f;
        int got = is_billboard
                  ? pick_ray_billboard(mesh, s_cam_right, s_cam_up, &best_local)
                  : pick_ray_mesh(mesh, &best_local);
        if (!got) return;   /* ray missed the real geometry -> not under cursor */
        t_hit = best_local;
        if (is_billboard) s_dbg_bbhit++; else s_dbg_solidhit++;
    }

    if (!s_best_valid || t_hit < s_best_t) {
        s_best_valid = 1;
        s_best_block = block;
        s_best_slot  = slot;
        s_best_mesh  = mesh;
        s_best_cx = cx; s_best_cy = cy; s_best_cz = cz; s_best_r = r;
        s_best_t = t_hit;
        s_best_is_bb = is_billboard;
    }
}

/* --- Screen-space highlight: ALWAYS-ON-TOP, follows the PERSPECTIVE shape -----
 * We project the picked mesh's REAL triangles to 2D and (a) outline each face,
 * (b) hatch each face with marching 45-degree lines clipped to the triangle, so
 * the fill foreshortens with the surface (a road reads as a trapezoid, not a
 * flat rectangle). Drawn as pre-transformed verts with depth forced to the
 * front (depth_z=0 passes the LEQ z-test, z-write off) so it is never occluded.
 * NOTE: for alpha-keyed billboards this outlines the QUAD, not the per-texel
 * leaf silhouette -- that needs a GPU alpha/ID pass (a further step). */
static TD5_D3DVertex s_hl_verts[2048];   /* known-safe line-batch size */
static int           s_hl_count;
static float         s_hl_phase;   /* animated hatch offset, set per draw */
/* Reject projected coords beyond this many pixels off the render target: a
 * vertex just in front of the near plane projects to a gigantic coordinate,
 * which both stalls the hatch loop and feeds a degenerate line to the GPU
 * (observed as a TDR). Triangles with any such vertex are skipped. */
#define HL_COORD_BOUND 12000.0f

static void hl_seg(float x0, float y0, float x1, float y1, uint32_t col)
{
    TD5_D3DVertex a, b;
    if (s_hl_count + 2 > (int)(sizeof s_hl_verts / sizeof s_hl_verts[0])) return;
    a.screen_x = x0; a.screen_y = y0; a.depth_z = 0.0f; a.rhw = 1.0f;
    a.diffuse = col; a.specular = 0; a.tex_u = 0.0f; a.tex_v = 0.0f;
    b = a; b.screen_x = x1; b.screen_y = y1;
    s_hl_verts[s_hl_count++] = a;
    s_hl_verts[s_hl_count++] = b;
}

/* Hatch one screen-space triangle with marching anti-diagonals (x+y=d), each
 * line clipped to the triangle by intersecting its three edges. */
static void hl_hatch_tri(const float p0[2], const float p1[2], const float p2[2],
                         float spacing, uint32_t col)
{
    const float *P[3]; float mn, mx, d;
    P[0] = p0; P[1] = p1; P[2] = p2;
    mn = mx = p0[0] + p0[1];
    {
        float s1 = p1[0] + p1[1], s2 = p2[0] + p2[1];
        if (s1 < mn) mn = s1;
        if (s1 > mx) mx = s1;
        if (s2 < mn) mn = s2;
        if (s2 > mx) mx = s2;
    }
    if ((mx - mn) / spacing > 600.0f) return;   /* guard: absurd span -> skip fill */
    for (d = floorf(mn / spacing) * spacing + s_hl_phase; d <= mx; d += spacing) {
        float cs[3][2]; int nc = 0, e, ia = 0, ib = 1, i, j; float bestd = -1.0f;
        for (e = 0; e < 3 && nc < 3; e++) {
            const float *A = P[e], *B = P[(e + 1) % 3];
            float denom = (B[0] - A[0]) + (B[1] - A[1]);
            float s;
            if (denom > -1e-6f && denom < 1e-6f) continue;
            s = (d - A[0] - A[1]) / denom;
            if (s < 0.0f || s > 1.0f) continue;
            cs[nc][0] = A[0] + s * (B[0] - A[0]);
            cs[nc][1] = A[1] + s * (B[1] - A[1]);
            nc++;
        }
        if (nc < 2) continue;
        for (i = 0; i < nc; i++) for (j = i + 1; j < nc; j++) {
            float ddx = cs[i][0]-cs[j][0], ddy = cs[i][1]-cs[j][1];
            float dd = ddx*ddx + ddy*ddy;
            if (dd > bestd) { bestd = dd; ia = i; ib = j; }
        }
        hl_seg(cs[ia][0], cs[ia][1], cs[ib][0], cs[ib][1], col);
    }
}

/* Project a world point; return 1 + screen xy on success (in front of camera). */
static int hl_project(float wx, float wy, float wz, float out[2])
{
    float sx, sy, rhw;
    if (!td5_render_project_world(wx, wy, wz, &sx, &sy, &rhw)) return 0;
    /* Reject wildly off-screen projections (near-plane blow-up) so a degenerate
     * huge line can never reach the GPU (TDR) and the hatch loop stays bounded. */
    if (sx < -HL_COORD_BOUND || sx > HL_COORD_BOUND ||
        sy < -HL_COORD_BOUND || sy > HL_COORD_BOUND) return 0;
    out[0] = sx; out[1] = sy;
    return 1;
}

/* Outline + perspective hatch of the picked mesh's real geometry. Walks the same
 * dispatch-0 quads/tris the ray test does; billboard verts are placed on the
 * camera-facing plane so the hatch matches what is drawn. */
static void pick_draw_highlight(const TD5_MeshHeader *mesh, int is_bb,
                                const float right[3], const float up[3],
                                uint32_t col)
{
    const TD5_PrimitiveCmd *cmds = mesh->commands;
    const TD5_MeshVertex   *vtx  = mesh->vertices;
    int cc = mesh->command_count, total = mesh->total_vertex_count;
    float o0 = mesh->origin_x * (1.0f/256.0f);
    float o1 = mesh->origin_y * (1.0f/256.0f);
    float o2 = mesh->origin_z * (1.0f/256.0f);
    int k, cursor = 0;

    if (!cmds || !vtx || cc <= 0 || cc > 4096 || total <= 0 || total > 131072) return;

    s_hl_count = 0;
    s_hl_phase = fmodf((float)td5_plat_time_ms() * 0.03f, 16.0f);

    #define HL_VW(idx, w3) do { \
        if (is_bb) { float lx = vtx[(idx)].pos_x, ly = vtx[(idx)].pos_y; \
            (w3)[0] = o0 + lx*right[0] + ly*up[0]; \
            (w3)[1] = o1 + lx*right[1] + ly*up[1]; \
            (w3)[2] = o2 + lx*right[2] + ly*up[2]; } \
        else { (w3)[0] = vtx[(idx)].pos_x + o0; \
               (w3)[1] = vtx[(idx)].pos_y + o1; \
               (w3)[2] = vtx[(idx)].pos_z + o2; } } while (0)

    for (k = 0; k < cc; k++) {
        int nq = cmds[k].quad_count, nt = cmds[k].triangle_count, q, tr;
        if (cmds[k].dispatch_type != 0) break;
        for (q = 0; q < nq; q++) {
            float w[4][3], s[4][2]; int ok4 = 1, m;
            if (cursor + 4 > total) { cursor = total; break; }
            for (m = 0; m < 4; m++) { HL_VW(cursor + m, w[m]);
                if (!hl_project(w[m][0], w[m][1], w[m][2], s[m])) ok4 = 0; }
            if (ok4) {
                hl_seg(s[0][0],s[0][1], s[1][0],s[1][1], col);   /* outline */
                hl_seg(s[1][0],s[1][1], s[2][0],s[2][1], col);
                hl_seg(s[2][0],s[2][1], s[3][0],s[3][1], col);
                hl_seg(s[3][0],s[3][1], s[0][0],s[0][1], col);
                hl_hatch_tri(s[0], s[1], s[2], 16.0f, col);      /* fill */
                hl_hatch_tri(s[0], s[2], s[3], 16.0f, col);
            }
            cursor += 4;
        }
        for (tr = 0; tr < nt; tr++) {
            float w[3][3], s[3][2]; int ok3 = 1, m;
            if (cursor + 3 > total) { cursor = total; break; }
            for (m = 0; m < 3; m++) { HL_VW(cursor + m, w[m]);
                if (!hl_project(w[m][0], w[m][1], w[m][2], s[m])) ok3 = 0; }
            if (ok3) {
                hl_seg(s[0][0],s[0][1], s[1][0],s[1][1], col);
                hl_seg(s[1][0],s[1][1], s[2][0],s[2][1], col);
                hl_seg(s[2][0],s[2][1], s[0][0],s[0][1], col);
                hl_hatch_tri(s[0], s[1], s[2], 16.0f, col);
            }
            cursor += 3;
        }
    }
    #undef HL_VW

    if (s_hl_count >= 2)
        td5_plat_render_draw_lines(s_hl_verts, s_hl_count);
}

/* Gather the mesh's per-command texture pages (the header field is a billboard
 * tag, so pages come from the primitive commands). Returns the primary page
 * (command 0), fills up to `cap` distinct pages into `pages`, count in *pn. */
static int pick_mesh_pages(const TD5_MeshHeader *mesh, int *pages, int cap, int *pn)
{
    int primary = -1, n = 0, k;
    int cc = mesh->command_count;
    const TD5_PrimitiveCmd *cmds = mesh->commands;

    *pn = 0;
    if (!cmds || cc <= 0) return -1;
    if (cc > 4096) cc = 4096;
    for (k = 0; k < cc; k++) {
        int pg = cmds[k].texture_page_id;
        int j, seen = 0;
        if (k == 0) primary = pg;
        for (j = 0; j < n; j++) if (pages[j] == pg) { seen = 1; break; }
        if (!seen && n < cap) pages[n++] = pg;
    }
    *pn = n;
    return primary;
}

void td5_pick_finish_frame(void)
{
    if (!pick_enabled() || !td5_camera_freecam_active()) {
        s_hud_valid = 0;
        return;
    }

    if (s_best_valid && s_best_mesh) {
        int   entry = td5_track_display_list_index(s_best_block);
        int   pages[6], npages = 0;
        int   primary = pick_mesh_pages(s_best_mesh, pages, 6, &npages);
        int   verts = s_best_mesh->total_vertex_count;
        int   cmds  = s_best_mesh->command_count;
        float r = s_best_r;
        int   is_auto = td5_trackgen_is_auto_slot(g_td5.track_index);
        const char *pgname = is_auto ? td5_trackgen_page_name(primary) : NULL;
        /* [IMPORT HANDLE] On a SHIPPED track the pick names the level the
         * page lives in ("level014" page 276), which is exactly the
         * (level, page) key re/tools/gen_tg_pages.py --from-pick consumes to
         * bake that art into the auto-track. The auto track keeps "AUTO"
         * (its pages are already generator pages, so there is nothing to
         * import). level_num mirrors td5_asset_level_number: TD5 zip number
         * 1..39, or the converted TD6 level (7..12, 18..22). */
        int   level_num = is_auto ? 0 : td5_asset_level_number(g_td5.track_index);
        char  trackid[16];
        if (is_auto) snprintf(trackid, sizeof trackid, "AUTO");
        else         snprintf(trackid, sizeof trackid, "level%03d", level_num);
        const char *kind = (is_auto && entry >= 0)
                           ? td5_trackgen_mesh_kind_name(entry, s_best_slot) : NULL;
        uint32_t col = (s_flash > 0) ? 0xFF33FF33u : 0xFFFFFF00u;  /* green flash / yellow */

        pick_draw_highlight(s_best_mesh, s_best_is_bb, s_cam_right, s_cam_up, col);

        /* Human label (pos matches the debug POS overlay: world/256 units). */
        {
            char pgextra[16];
            char pgn[40];
            char kindstr[24];
            pgextra[0] = '\0';
            if (npages > 1) snprintf(pgextra, sizeof pgextra, " +%d", npages - 1);
            if (pgname) snprintf(pgn, sizeof pgn, " (%s)", pgname);
            else        pgn[0] = '\0';
            if (kind) snprintf(kindstr, sizeof kindstr, " %s", kind);
            else      kindstr[0] = '\0';
            snprintf(s_hud_line, sizeof s_hud_line,
                     "PICK %s entry %d slot %d%s  page %d%s%s  pos %.1f %.1f %.1f  r %.1f  [LMB=copy]",
                     trackid, entry, s_best_slot, kindstr, primary, pgn, pgextra,
                     s_best_cx, s_best_cy, s_best_cz, r);
        }

        /* Clipboard payload -- compact single line. The old JSON ran ~190 chars
         * and wrapped in every terminal / chat window these picks get pasted
         * into. Same information in ~70 chars:
         *   AUTO L90 e13 s13 terrain p2:GREEN+368 pos 92953,-2266,14178 r12005 v16 c2
         * Positions round to whole units: they are world/256 render floats, so
         * the two decimals never helped identify a mesh.
         * [PICK 2026-09-06] user request: "make the copied coordinates fit in one line". */
        {
            int off, i;
            off = snprintf(s_json, sizeof s_json, "%s L%d e%d s%d",
                           trackid, level_num, entry, s_best_slot);
            if (kind && off > 0 && off < (int)sizeof s_json)
                off += snprintf(s_json + off, sizeof s_json - off, " %s", kind);
            if (off > 0 && off < (int)sizeof s_json)
                off += snprintf(s_json + off, sizeof s_json - off, " p%d", primary);
            if (pgname && off > 0 && off < (int)sizeof s_json)
                off += snprintf(s_json + off, sizeof s_json - off, ":%s", pgname);
            /* pages[0] is the primary and is already printed; list only extras. */
            for (i = 1; i < npages && off > 0 && off < (int)sizeof s_json; i++)
                off += snprintf(s_json + off, sizeof s_json - off, "+%d", pages[i]);
            if (off > 0 && off < (int)sizeof s_json)
                snprintf(s_json + off, sizeof s_json - off,
                         " pos %.0f,%.0f,%.0f r%.0f v%d c%d",
                         s_best_cx, s_best_cy, s_best_cz, r, verts, cmds);
        }
        s_hud_valid = 1;

        /* Copy ONLY on a real left-click delivered to the window (message latch,
         * not a global async-key read which fired without a click). */
        if (td5_plat_pick_take_click()) {
            if (td5_plat_clipboard_set_text(s_json)) {
                TD5_LOG_I(LOG_TAG, "pick: copied %s", s_json);
                s_flash = 120;   /* ~visible confirmation window */
            } else {
                TD5_LOG_W(LOG_TAG, "pick: clipboard copy FAILED for %s", s_json);
            }
        }
    } else {
        s_hud_valid = 0;
        td5_plat_pick_take_click();   /* drain so a click off-mesh isn't buffered */
    }

    if (s_flash > 0) s_flash--;
}

void td5_pick_hud_draw(void)
{
    if (!pick_enabled() || !td5_camera_freecam_active()) return;
    if (s_flash > 0)
        td5_hud_queue_text(0, 8, 96, 0, "COPIED to clipboard  |  %s", s_hud_line);
    else if (s_hud_valid)
        td5_hud_queue_text(0, 8, 96, 0, "%s", s_hud_line);
    else
        td5_hud_queue_text(0, 8, 96, 0, "FREE CAM PICK: hover geometry, click to copy");

    if (s_dbg > 0) {
        td5_hud_queue_text(0, 8, 110, 0,
            "  dbg cur(%.0f,%.0f)/%dx%d ray(%.2f,%.2f,%.2f) seen=%d sph=%d solid=%d bb=%d v0d=%.0f",
            s_cursor_x, s_cursor_y,
            g_td5.render_width, g_td5.render_height,
            s_ray_d[0], s_ray_d[1], s_ray_d[2],
            s_dbg_seen, s_dbg_sphere, s_dbg_solidhit, s_dbg_bbhit, s_dbg_v0d);
    }
}

#else  /* TD5RE_RELEASE -- feature compiled out */

void td5_pick_begin_frame(void) {}
int  td5_pick_collecting(void) { return 0; }
void td5_pick_consider(const TD5_SpanDisplayList *block, int slot,
                       const TD5_MeshHeader *mesh,
                       float cx, float cy, float cz, float r)
{ (void)block; (void)slot; (void)mesh; (void)cx; (void)cy; (void)cz; (void)r; }
void td5_pick_finish_frame(void) {}
void td5_pick_hud_draw(void) {}

#endif /* TD5RE_RELEASE */
