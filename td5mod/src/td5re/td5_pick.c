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

#include "td5re.h"
#include "td5_platform.h"
#include "td5_config.h"
#include "td5_camera.h"
#include "td5_render.h"
#include "td5_track.h"
#include "td5_trackgen.h"   /* auto-track page-name lookup */
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
static float s_best_viewz = 0.0f;   /* nearest-wins depth */
static float s_best_d2 = 0.0f;      /* screen dist^2 tiebreak */

/* --- resolved identity of the current hover (for HUD + click) --- */
static int   s_hud_valid = 0;
static char  s_hud_line[192];
static char  s_json[320];

/* --- click edge + copy flash --- */
static int   s_prev_lmb = 0;
static int   s_flash = 0;

static int pick_enabled(void)
{
    if (s_enabled < 0) s_enabled = td5_env_flag_on("TD5RE_PICK");  /* default ON */
    return s_enabled;
}

int td5_pick_collecting(void) { return s_collecting; }

void td5_pick_begin_frame(void)
{
    int cx, cy, cw, ch, rw, rh;

    s_collecting = 0;
    s_have_cursor = 0;
    s_best_valid = 0;

    if (!pick_enabled() || !td5_camera_freecam_active()) {
        s_hud_valid = 0;
        return;
    }

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
    float sx, sy, rhw, dx, dy, d2, rpx, viewz;

    if (!s_collecting || !s_have_cursor || !mesh) return;
    if (!td5_render_project_world(cx, cy, cz, &sx, &sy, &rhw)) return;  /* behind cam */

    dx = sx - s_cursor_x;
    dy = sy - s_cursor_y;
    d2 = dx * dx + dy * dy;

    /* On-screen radius of the bounding sphere: px = r * focal / view_z. Floor it
     * so distant/small meshes stay hoverable rather than collapsing to a point. */
    rpx = r * td5_render_get_focal_length() * rhw;
    if (rpx < 5.0f) rpx = 5.0f;
    if (d2 > rpx * rpx) return;   /* cursor not over this mesh's disc */

    viewz = (rhw > 0.0f) ? (1.0f / rhw) : 1e30f;

    /* Nearest to the camera wins; tie broken by closeness to the cursor. */
    if (!s_best_valid || viewz < s_best_viewz ||
        (viewz == s_best_viewz && d2 < s_best_d2)) {
        s_best_valid = 1;
        s_best_block = block;
        s_best_slot  = slot;
        s_best_mesh  = mesh;
        s_best_cx = cx; s_best_cy = cy; s_best_cz = cz; s_best_r = r;
        s_best_viewz = viewz;
        s_best_d2 = d2;
    }
}

/* Draw the 12 edges of an axis-aligned box (render-float world space). */
static void pick_draw_box(float x0, float y0, float z0,
                          float x1, float y1, float z1, uint32_t col)
{
    /* bottom rectangle */
    td5_render_debug_line_world(x0, y0, z0, x1, y0, z0, col);
    td5_render_debug_line_world(x1, y0, z0, x1, y0, z1, col);
    td5_render_debug_line_world(x1, y0, z1, x0, y0, z1, col);
    td5_render_debug_line_world(x0, y0, z1, x0, y0, z0, col);
    /* top rectangle */
    td5_render_debug_line_world(x0, y1, z0, x1, y1, z0, col);
    td5_render_debug_line_world(x1, y1, z0, x1, y1, z1, col);
    td5_render_debug_line_world(x1, y1, z1, x0, y1, z1, col);
    td5_render_debug_line_world(x0, y1, z1, x0, y1, z0, col);
    /* vertical posts */
    td5_render_debug_line_world(x0, y0, z0, x0, y1, z0, col);
    td5_render_debug_line_world(x1, y0, z0, x1, y1, z0, col);
    td5_render_debug_line_world(x1, y0, z1, x1, y1, z1, col);
    td5_render_debug_line_world(x0, y0, z1, x0, y1, z1, col);
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

    td5_render_debug_lines_reset();

    if (s_best_valid && s_best_mesh) {
        int   entry = td5_track_display_list_index(s_best_block);
        int   pages[6], npages = 0;
        int   primary = pick_mesh_pages(s_best_mesh, pages, 6, &npages);
        int   verts = s_best_mesh->total_vertex_count;
        int   cmds  = s_best_mesh->command_count;
        float r = s_best_r;
        int   is_auto = td5_trackgen_is_auto_slot(g_td5.track_index);
        const char *pgname = is_auto ? td5_trackgen_page_name(primary) : NULL;
        const char *kind = (is_auto && entry >= 0)
                           ? td5_trackgen_mesh_kind_name(entry, s_best_slot) : NULL;
        uint32_t col = (s_flash > 0) ? 0xFF33FF33u : 0xFFFFEE00u;  /* green flash / yellow */

        pick_draw_box(s_best_cx - r, s_best_cy - r, s_best_cz - r,
                      s_best_cx + r, s_best_cy + r, s_best_cz + r, col);

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
                     "PICK entry %d slot %d%s  page %d%s%s  pos %.1f %.1f %.1f  r %.1f  [LMB=copy]",
                     entry, s_best_slot, kindstr, primary, pgn, pgextra,
                     s_best_cx, s_best_cy, s_best_cz, r);
        }

        /* JSON payload for the clipboard. */
        {
            int off, i;
            off = snprintf(s_json, sizeof s_json,
                     "{\"track\":\"AUTO\",\"entry\":%d,\"slot\":%d,\"page\":%d,",
                     entry, s_best_slot, primary);
            if (kind && off > 0 && off < (int)sizeof s_json)
                off += snprintf(s_json + off, sizeof s_json - off,
                                "\"kind\":\"%s\",", kind);
            if (pgname && off > 0 && off < (int)sizeof s_json)
                off += snprintf(s_json + off, sizeof s_json - off,
                                "\"page_name\":\"%s\",", pgname);
            if (off > 0 && off < (int)sizeof s_json)
                off += snprintf(s_json + off, sizeof s_json - off, "\"pages\":[");
            for (i = 0; i < npages && off > 0 && off < (int)sizeof s_json; i++)
                off += snprintf(s_json + off, sizeof s_json - off,
                                "%s%d", i ? "," : "", pages[i]);
            if (off > 0 && off < (int)sizeof s_json)
                snprintf(s_json + off, sizeof s_json - off,
                         "],\"pos\":[%.2f,%.2f,%.2f],\"radius\":%.2f,\"verts\":%d,\"cmds\":%d}",
                         s_best_cx, s_best_cy, s_best_cz, r, verts, cmds);
        }
        s_hud_valid = 1;

        /* Left-click edge -> copy. */
        {
            int lmb = td5_plat_input_mouse_left_down();
            if (lmb && !s_prev_lmb) {
                if (td5_plat_clipboard_set_text(s_json)) {
                    TD5_LOG_I(LOG_TAG, "pick: copied %s", s_json);
                    s_flash = 12;
                } else {
                    TD5_LOG_W(LOG_TAG, "pick: clipboard copy FAILED for %s", s_json);
                }
            }
            s_prev_lmb = lmb;
        }
    } else {
        s_hud_valid = 0;
        s_prev_lmb = td5_plat_input_mouse_left_down();
    }

    td5_render_debug_lines_flush();
    if (s_flash > 0) s_flash--;
}

void td5_pick_hud_draw(void)
{
    if (!pick_enabled() || !td5_camera_freecam_active()) return;
    if (s_hud_valid)
        td5_hud_queue_text(0, 8, 96, 0, "%s", s_hud_line);
    else
        td5_hud_queue_text(0, 8, 96, 0, "FREE CAM PICK: hover geometry to identify");
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
