/* ========================================================================
 * td5_fe_devscreens.c -- CHANGELOG + PENDING TO TEST dev screens
 *
 * Split out of td5_frontend.c (2026-07-02): the version-banner CHANGELOG
 * screen (scrollable change list) and the PENDING TO TEST dev/QA checklist
 * screen. Content tables live in td5_changelog.h / td5_pending.h.
 * Cross-TU seam: td5_frontend_internal.h.
 * ======================================================================== */

#include "td5_frontend.h"
#include "td5_asset.h"
#include "td5_track_registry.h"  /* custom-track registry: name/slot lookups + slot headroom */
#include "td5_frontend_button_cache.h"
#include "td5_game.h"
#include "td5_profile.h"
#include "td5_input.h"
#include "td5_physics.h"
#include "td5_carparam.h"      /* shared carparam field map (rebuilt MORE STATS panel) */
#include "td5_net.h"
#include "td5_platform.h"
#include "td5_render.h"
#include "td5_save.h"
#include "td5_config.h"      /* shared TD5RE_* env-knob accessors */
#include "td5_sound.h"
#include "td5_hud.h"           /* per-viewport player-identity overlay (race) */
#include "td5re.h"
#include "td5_snk_strings.h"   /* byte-exact SNK_ labels baked from Language.dll */
#include "td5_credits.h"       /* SNK_CreditsText array + dev mugshot map (Extras scroll) */
#include "td5_vectorui.h"      /* public VectorUI surface (HUD reuses these primitives) */
#include "td5_font.h"          /* [S13] runtime TTF glyph cache (native menu text) */
#include "td5_version.h"       /* build identity (version / channel / date / git rev) */
#include "td5_changelog.h"     /* CHANGELOG screen content table (file-static here) */
#include "td5_pending.h"       /* PENDING TO TEST checklist (list/state/overlay) */
#include "deps/cjson/cJSON.h"  /* track-marker JSON (retired trak_markers*.dat) */
#include "td5_frontend_internal.h"

#define LOG_TAG "frontend"

/* ======== [split] CHANGELOG + PENDING screens (moved verbatim from td5_frontend.c) ======== */
/* ========================================================================
 * CHANGELOG screen (2026-06-25) -- version banner + scrollable change list,
 * plus the entry into the PENDING TO TEST checklist.
 *
 * Reached from a button beside EXIT on the main menu (present in dev AND
 * release). Body text is left-aligned with the CHANGELOG title and drawn a
 * notch smaller than menu text. Scroll with the MOUSE WHEEL or PAGE UP/DOWN
 * (HOME/END snap to the ends); UP/DOWN navigate the two buttons. Two buttons at
 * the bottom: PENDING TO TEST (opens the dev/QA checklist) and BACK; B/ESC also
 * returns to the main menu. Content lives in td5_changelog.h; build identity in
 * td5_version.h.
 * ======================================================================== */

/* Body viewport + layout in design (640x480) coordinates. */
#define CL_VIEW_TOP    70.0f
#define CL_VIEW_BOTTOM 404.0f
#define CL_LINE_H      16.0f             /* row pitch (a touch more air between lines) */
#define CL_LEFT_X      FE_TITLE_LEFT_X   /* 126: line text up with the CHANGELOG title */
#define CL_ITEM_INDENT 11.0f             /* hanging indent for bullet items (dot sits left) */
#define CL_FONT        0.82f             /* body text scale (smaller than menu text) */
#define CL_SCROLLBAR_X 614.0f

static float    s_changelog_scroll     = 0.0f;  /* top visible line (fractional)        */
static float    s_changelog_max_scroll = 0.0f;  /* set by the render pass each frame    */
static uint32_t s_changelog_last_ms    = 0;     /* for frame-rate-independent scrolling */
static int      s_cl_pending_btn       = -1;
static int      s_cl_back_btn          = -1;

static int frontend_changelog_visible_lines(void) {
    return (int)((CL_VIEW_BOTTOM - CL_VIEW_TOP) / CL_LINE_H);
}

void frontend_changelog_render(float sx, float sy) {
    char  ver[160];
    int   i;
    int   prev_case = s_fe_preserve_case;
    float top    = CL_VIEW_TOP;
    float bot    = CL_VIEW_BOTTOM;
    float fsx    = sx * CL_FONT;        /* smaller body-text scale */
    float fsy    = sy * CL_FONT;
    float scroll;

    /* Title (gold, top-left like every standard screen). */
    frontend_draw_screen_title("CHANGELOG", FE_TITLE_LEFT_X * sx, 17.0f * sy,
                               0xFFE3D708u, sx, sy);

    /* Build identity banner, left-aligned under the title. */
    if (TD5RE_GIT_REV[0])
        snprintf(ver, sizeof ver, "TD5RE v%s  %s  -  %s  -  built %s",
                 TD5RE_VERSION_STR, TD5RE_BUILD_CHANNEL, TD5RE_GIT_REV, TD5RE_BUILD_DATE);
    else
        snprintf(ver, sizeof ver, "TD5RE v%s  %s  -  built %s",
                 TD5RE_VERSION_STR, TD5RE_BUILD_CHANNEL, TD5RE_BUILD_DATE);

    /* Banner + body use mixed case for readability (the title path is always caps). */
    s_fe_preserve_case = 1;
    fe_draw_text(CL_LEFT_X * sx, 44.0f * sy, ver, 0xFF9098A0u, fsx, fsy);

    /* Clamp the live scroll to the current content and publish the max so the
     * handler can clamp against it next frame. */
    {
        int   vis  = frontend_changelog_visible_lines();
        float maxs = (float)(TD5_CHANGELOG_LINE_COUNT - vis);
        if (maxs < 0.0f) maxs = 0.0f;
        s_changelog_max_scroll = maxs;
        if (s_changelog_scroll < 0.0f) s_changelog_scroll = 0.0f;
        if (s_changelog_scroll > maxs) s_changelog_scroll = maxs;
    }
    scroll = s_changelog_scroll;

    /* Draw the visible window of lines, all left-aligned at the title x. A line is
     * skipped unless it sits inside [top, bot] so nothing bleeds over the banner
     * or the buttons. */
    for (i = 0; i < TD5_CHANGELOG_LINE_COUNT; i++) {
        const TD5_ChangelogLine *ln = &k_changelog_lines[i];
        float    y = top + ((float)i - scroll) * CL_LINE_H;
        uint32_t col;
        if (ln->style == CL_BLANK) continue;
        if (y < top - 0.5f || y > bot - CL_LINE_H) continue;
        if (ln->style == CL_ITEM) {
            /* Bullet list: the first line of an item gets a gold dot at the
             * margin; continuation lines (authored with a leading indent) drop
             * the indent and hang under the item text, aligned past the dot. */
            const char *t = ln->text;
            int cont = (t[0] == ' ');
            while (*t == ' ') t++;
            if (!cont)
                fe_draw_quad((CL_LEFT_X + 1.0f) * sx, (y + 13.5f) * sy,
                             3.5f * sx, 3.5f * sy, 0xFFE3D708u, -1, 0, 0, 0, 0);
            fe_draw_text((CL_LEFT_X + CL_ITEM_INDENT) * sx, y * sy, t,
                         0xFFD8DCE0u, fsx, fsy);
            continue;
        }
        switch (ln->style) {
        case CL_SECTION: col = 0xFFE3D708u; break;  /* gold  */
        case CL_DATE:    col = 0xFF66CCFFu; break;  /* cyan  */
        default:         col = 0xFFD8DCE0u; break;  /* white */
        }
        fe_draw_text(CL_LEFT_X * sx, y * sy, ln->text, col, fsx, fsy);
    }
    s_fe_preserve_case = prev_case;

    /* Scrollbar (only when the content overflows the viewport). */
    if (s_changelog_max_scroll > 0.0f) {
        float track_h = bot - top;
        float vis     = (float)frontend_changelog_visible_lines();
        float total   = (float)TD5_CHANGELOG_LINE_COUNT;
        float thumb_h = track_h * (vis / total);
        float frac    = scroll / s_changelog_max_scroll;
        float thumb_y;
        if (thumb_h < 16.0f) thumb_h = 16.0f;
        thumb_y = top + frac * (track_h - thumb_h);
        fe_draw_quad(CL_SCROLLBAR_X * sx, top * sy, 4.0f * sx, track_h * sy,
                     0x40FFFFFFu, -1, 0, 0, 0, 0);              /* track */
        fe_draw_quad(CL_SCROLLBAR_X * sx, thumb_y * sy, 4.0f * sx, thumb_h * sy,
                     0xFFE3D708u, -1, 0, 0, 0, 0);              /* thumb */
    }

    /* Footer hint, left-aligned + small. */
    fe_draw_text(CL_LEFT_X * sx, 420.0f * sy,
                 "MOUSE WHEEL / PAGE UP-DOWN: SCROLL", 0xFF8890A0u, fsx, fsy);
}

void Screen_Changelog(void) {
    switch (s_inner_state) {
    case 0:
        frontend_load_tga("Front_End/MainMenu.tga", "Front_End/FrontEnd.zip");
        frontend_reset_buttons();
        frontend_init_return_screen(TD5_SCREEN_CHANGELOG);   /* parent = MAIN_MENU */
        /* PENDING TO TEST sits up top against the right margin (clear of the
         * CHANGELOG title) and vertically centred on it; BACK stays at the
         * bottom, now centred. */
        s_cl_pending_btn = frontend_create_button("PENDING TO TEST", 420,  15, 200, 26);
        s_cl_back_btn    = frontend_create_button("BACK",            260, 440, 120, 26);
        s_selected_button   = s_cl_back_btn;
        s_changelog_scroll  = 0.0f;
        s_changelog_last_ms = 0;
        (void)td5_plat_input_get_mouse_wheel();  /* drop any wheel queued in the menu */
        s_anim_complete     = 1;  /* instant screen: enables B/ESC + fade-in chime */
        s_inner_state       = 1;
        TD5_LOG_I(LOG_TAG, "Screen_Changelog: enter (%d lines)", TD5_CHANGELOG_LINE_COUNT);
        break;

    case 1: {
        /* Frame-rate-independent page-key scroll. */
        uint32_t now = td5_plat_time_ms();
        uint32_t dt  = (s_changelog_last_ms && now > s_changelog_last_ms)
                           ? (now - s_changelog_last_ms) : 16u;
        if (dt > 100u) dt = 100u;   /* clamp after a stall so a tab-away can't fling */
        s_changelog_last_ms = now;

        const uint8_t *kb = td5_plat_input_get_keyboard();
        float step = (float)dt * 0.05f;

        /* Mouse wheel: 3 lines per notch (one notch = 120). Wheel-up scrolls up. */
        int wheel = td5_plat_input_get_mouse_wheel();
        if (wheel != 0) s_changelog_scroll -= (float)wheel / 120.0f * 3.0f;

        if (kb) {
            if (kb[0xC9] & 0x80) s_changelog_scroll -= step;            /* PAGE UP   */
            if (kb[0xD1] & 0x80) s_changelog_scroll += step;            /* PAGE DOWN */
            if (kb[0xC7] & 0x80) s_changelog_scroll  = 0.0f;            /* HOME      */
            if (kb[0xCF] & 0x80) s_changelog_scroll  = s_changelog_max_scroll; /* END */
        }

        if (s_changelog_scroll < 0.0f) s_changelog_scroll = 0.0f;
        if (s_changelog_scroll > s_changelog_max_scroll)
            s_changelog_scroll = s_changelog_max_scroll;

        /* Buttons (Enter / click / pad A). UP/DOWN move between them. B/ESC is the
         * shared display-loop back path (parent screen = MAIN_MENU). */
        if (s_input_ready && s_button_index >= 0) {
            if (s_button_index == s_cl_pending_btn) {
                frontend_play_sfx(3);
                td5_frontend_set_screen(TD5_SCREEN_PENDING_TEST);
            } else if (s_button_index == s_cl_back_btn) {
                frontend_play_sfx(5);
                td5_frontend_set_screen(TD5_SCREEN_MAIN_MENU);
            }
        }
        break;
    }
    }
}

/* ========================================================================
 * PENDING TO TEST screen (2026-06-25, modal reworked 2026-07-04) -- dev/QA
 * checklist.
 *
 * Reached from a button at the top of the main menu. Lists the features in
 * td5_pending.c; ENTER on a row opens a details modal (frontend_pending_render_modal)
 * showing the item's full text plus a longer testing-focused note
 * (td5_pending_detail_text). There is no more crossing-out/"mark tested" --
 * SUPR/DELETE is the only way to clear a row, once it's actually been
 * verified. An IN-GAME OVERLAY button (mirrored by F11) toggles the
 * right-edge overlay that lists every item. Items are paged so a long list
 * stays within FE_MAX_BUTTONS. Present in dev AND release.
 *
 * Each list row is a standard EMPTY-LABEL button, so it gets keyboard / pad /
 * mouse selection + the gold highlight for free; the item text is drawn over
 * the row in the POST-button render pass.
 * ======================================================================== */

#define PL_ROWS_PER_PAGE 10
#define PL_ROW_X     126                 /* line the rows up under the title (FE_TITLE_LEFT_X) */
#define PL_ROW_W     410                 /* a touch narrower than before (right edge ~536) */
#define PL_ROW_H     24
#define PL_ROW_Y0    88                  /* pushed down for breathing room below the header */
#define PL_ROW_STEP  27
#define PL_CTL_Y     (PL_ROW_Y0 + PL_ROWS_PER_PAGE * PL_ROW_STEP + 8)   /* 366 */
#define PENDING_TEXT_MAX_LOCAL 128       /* >= td5_pending.c PENDING_TEXT_MAX (120) + "..." */

/* ENTER-key details modal geometry (design units, 640x480 canvas). */
#define PL_MODAL_W               440.0f
#define PL_MODAL_H               260.0f
#define PL_MODAL_X               ((640.0f - PL_MODAL_W) * 0.5f)
#define PL_MODAL_Y               ((480.0f - PL_MODAL_H) * 0.5f)
#define PL_MODAL_PAD             20.0f
#define PL_MODAL_TITLE_MAX_LINES 3
#define PL_MODAL_BODY_MAX_LINES  9

static int s_pl_page        = 0;
static int s_pl_pages       = 1;
static int s_pl_row_count   = 0;
static int s_pl_overlay_btn = -1;
static int s_pl_back_btn    = -1;
static int s_pl_prev_btn    = -1;
static int s_pl_next_btn    = -1;
static int s_pl_modal_open  = 0;   /* details modal (ENTER on a row) owns input while set */
static int s_pl_modal_item  = -1;  /* global (unpaged) index of the item the modal describes */

/* (Re)create the button set for the current page. Called on entry and on every
 * page change. Row buttons are indices 0..row_count-1; the control buttons land
 * in whatever slots follow. */
static void frontend_pending_build_buttons(void) {
    int total = td5_pending_count();
    int start, end, r;

    s_pl_pages = (total + PL_ROWS_PER_PAGE - 1) / PL_ROWS_PER_PAGE;
    if (s_pl_pages < 1) s_pl_pages = 1;
    if (s_pl_page >= s_pl_pages) s_pl_page = s_pl_pages - 1;
    if (s_pl_page < 0) s_pl_page = 0;

    frontend_reset_buttons();

    start = s_pl_page * PL_ROWS_PER_PAGE;
    end   = start + PL_ROWS_PER_PAGE;
    if (end > total) end = total;
    s_pl_row_count = end - start;

    for (r = 0; r < s_pl_row_count; r++)
        frontend_create_button("", PL_ROW_X, PL_ROW_Y0 + r * PL_ROW_STEP, PL_ROW_W, PL_ROW_H);

    /* Control row, left-aligned with the rows/title. All four carry an empty
     * label so their text is drawn in the POST-button pass (frontend_pending_render)
     * in the SAME smaller body font as the IN-GAME OVERLAY toggle, at the same
     * button-top y, so every control label lines up vertically. */
    s_pl_overlay_btn = frontend_create_button("", PL_ROW_X,       PL_CTL_Y, 216, 26);
    s_pl_back_btn    = frontend_create_button("", PL_ROW_X + 226, PL_CTL_Y,  98, 26);
    if (s_pl_pages > 1) {
        s_pl_prev_btn = frontend_create_button("", PL_ROW_X + 332, PL_CTL_Y, 64, 26);
        s_pl_next_btn = frontend_create_button("", PL_ROW_X + 404, PL_CTL_Y, 64, 26);
    } else {
        s_pl_prev_btn = -1;
        s_pl_next_btn = -1;
    }
    s_selected_button = (s_pl_row_count > 0) ? 0 : s_pl_back_btn;
}

/* Fit `src` into `max_w` screen-px at (pfx,pfy); if it's wider, truncate on a
 * whole-character boundary and append an ellipsis so the row never overruns its
 * button. Writes the result into out[out_sz] and returns its measured width (so
 * the strikethrough can match). Measurement honours s_fe_preserve_case, so call
 * it inside the same case-preserving region used to draw the rows. */
static float frontend_pending_fit_ellipsis(const char *src, float max_w,
                                           float pfx, float pfy,
                                           char *out, size_t out_sz) {
    if (fe_measure_text(src, pfx, pfy) <= max_w) {
        snprintf(out, out_sz, "%s", src);
        return fe_measure_text(out, pfx, pfy);
    }
    {
        float ell_w = fe_measure_text("...", pfx, pfy);
        char  tmp[PENDING_TEXT_MAX_LOCAL];
        int   cap = (int)out_sz - 4;          /* room for "..." + NUL in `out` */
        int   len = (int)strlen(src), n;
        if (cap > (int)sizeof tmp - 1) cap = (int)sizeof tmp - 1;
        if (len > cap) len = cap;
        for (n = len; n > 0; n--) {
            memcpy(tmp, src, (size_t)n);
            tmp[n] = '\0';
            if (fe_measure_text(tmp, pfx, pfy) + ell_w <= max_w) {
                memcpy(out, src, (size_t)n);   /* n <= out_sz-4, so "..." + NUL fit */
                out[n] = out[n + 1] = out[n + 2] = '.';
                out[n + 3] = '\0';
                return fe_measure_text(out, pfx, pfy);
            }
        }
        snprintf(out, out_sz, "...");
        return ell_w;
    }
}

/* ENTER-key details modal: dims the screen and draws a bordered box with the
 * highlighted item's full text as a title and its longer test-focus note
 * (td5_pending_detail_text) wrapped underneath. Drawn last so it sits on top
 * of the list/control-row text. Replaces the old crossing-out behaviour --
 * ENTER opens this instead of toggling a checkbox. */
static void frontend_pending_render_modal(float sx, float sy) {
    float mx   = PL_MODAL_X * sx, my = PL_MODAL_Y * sy;
    float mw   = PL_MODAL_W * sx, mh = PL_MODAL_H * sy;
    float pad  = PL_MODAL_PAD * sx;
    float pfx  = sx * 0.82f, pfy = sy * 0.82f;
    float text_w = mw - 2.0f * pad;
    int   prev_case = s_fe_preserve_case;
    char  title_lines[PL_MODAL_TITLE_MAX_LINES][64];
    char  body_lines[PL_MODAL_BODY_MAX_LINES][64];
    int   n_title, n_body, li;
    float y, border_t = 2.0f * sy;
    const uint32_t border_c = 0xFFE3D708u;   /* gold, matches the screen title */

    if (s_pl_modal_item < 0 || s_pl_modal_item >= td5_pending_count()) return;

    fe_draw_quad(0.0f, 0.0f, 640.0f * sx, 480.0f * sy, 0xC8000008u, -1, 0, 0, 0, 0);  /* dim behind */

    fe_draw_quad(mx, my, mw, mh, 0xF2141018u, -1, 0, 0, 0, 0);                       /* box fill */
    fe_draw_quad(mx, my, mw, border_t, border_c, -1, 0, 0, 0, 0);                    /* top */
    fe_draw_quad(mx, my + mh - border_t, mw, border_t, border_c, -1, 0, 0, 0, 0);    /* bottom */
    fe_draw_quad(mx, my, border_t, mh, border_c, -1, 0, 0, 0, 0);                    /* left */
    fe_draw_quad(mx + mw - border_t, my, border_t, mh, border_c, -1, 0, 0, 0, 0);    /* right */

    s_fe_preserve_case = 1;
    n_title = fe_wrap_text_lines(td5_pending_text(s_pl_modal_item), text_w, pfx, pfy,
                                 title_lines, PL_MODAL_TITLE_MAX_LINES);
    n_body  = fe_wrap_text_lines(td5_pending_detail_text(s_pl_modal_item), text_w, pfx, pfy,
                                 body_lines, PL_MODAL_BODY_MAX_LINES);

    y = my + pad;
    for (li = 0; li < n_title; li++) {
        float tw = fe_measure_text(title_lines[li], pfx, pfy);
        fe_draw_text(mx + (mw - tw) * 0.5f, y, title_lines[li], 0xFFE3D708u, pfx, pfy);
        y += 16.0f * sy;
    }
    y += 10.0f * sy;
    fe_draw_quad(mx + pad, y, mw - 2.0f * pad, 1.0f * sy, 0xFF3A3E48u, -1, 0, 0, 0, 0);  /* divider */
    y += 12.0f * sy;
    for (li = 0; li < n_body; li++) {
        fe_draw_text(mx + pad, y, body_lines[li], 0xFFE6EAF0u, pfx, pfy);
        y += 15.0f * sy;
    }
    s_fe_preserve_case = prev_case;

    td5_vui_text_centered(mx + mw * 0.5f, my + mh - 22.0f * sy,
                          "ENTER / B / BACK = CLOSE", 0xFF8890A0u, sx, sy);
}

void frontend_pending_render(float sx, float sy) {
    char buf[96];
    int  total     = td5_pending_count();
    int  start     = s_pl_page * PL_ROWS_PER_PAGE;
    int  prev_case = s_fe_preserve_case;
    float pfx = sx * 0.82f;       /* smaller body-text scale (matches CHANGELOG) */
    float pfy = sy * 0.82f;
    int  r;

    frontend_draw_screen_title("PENDING TO TEST", FE_TITLE_LEFT_X * sx, 17.0f * sy,
                               0xFFE3D708u, sx, sy);
    snprintf(buf, sizeof buf, "%d ITEMS TO TEST", total);
    td5_vui_text_centered(320.0f * sx, 48.0f * sy, buf, 0xFFB0B8C0u, sx, sy);

    /* Per-row item text (left-aligned). */
    for (r = 0; r < s_pl_row_count; r++) {
        int   item = start + r;
        float bx, by, bw, bh, tx, avail;
        char  rowbuf[PENDING_TEXT_MAX_LOCAL];
        frontend_get_button_render_rect(r, sx, sy, &bx, &by, &bw, &bh);
        tx    = bx + 10.0f * sx;
        avail = (bx + bw) - tx - 8.0f * sx;  /* text room left in the row (small right-rim padding) */
        s_fe_preserve_case = 1;
        frontend_pending_fit_ellipsis(td5_pending_text(item), avail, pfx, pfy,
                                      rowbuf, sizeof rowbuf);
        fe_draw_text(tx, by, rowbuf, 0xFFE6EAF0u, pfx, pfy);
        s_fe_preserve_case = prev_case;
    }

    /* IN-GAME OVERLAY toggle label (centred on its button). */
    if (s_pl_overlay_btn >= 0) {
        float bx, by, bw, bh, tw;
        int   on = td5_pending_overlay_on();
        frontend_get_button_render_rect(s_pl_overlay_btn, sx, sy, &bx, &by, &bw, &bh);
        snprintf(buf, sizeof buf, "IN-GAME OVERLAY: %s", on ? "ON" : "OFF");
        tw = fe_measure_text(buf, pfx, pfy);
        fe_draw_text(bx + (bw - tw) * 0.5f, by, buf,
                     on ? 0xFF66E066u : 0xFFD8DCE0u, pfx, pfy);
    }

    /* BACK / < PREV / NEXT > labels — same smaller body font as the overlay
     * toggle, centred on their buttons and drawn at the button-top y so all the
     * control-row labels sit on the same line. */
    {
        const struct { int btn; const char *txt; } ctl[3] = {
            { s_pl_back_btn, "BACK"   },
            { s_pl_prev_btn, "< PREV" },
            { s_pl_next_btn, "NEXT >" },
        };
        for (int c = 0; c < 3; c++) {
            float bx, by, bw, bh, tw;
            if (ctl[c].btn < 0) continue;
            frontend_get_button_render_rect(ctl[c].btn, sx, sy, &bx, &by, &bw, &bh);
            tw = fe_measure_text(ctl[c].txt, pfx, pfy);
            fe_draw_text(bx + (bw - tw) * 0.5f, by, ctl[c].txt, 0xFFE6EAF0u, pfx, pfy);
        }
    }

    if (s_pl_pages > 1) {
        snprintf(buf, sizeof buf, "PAGE %d / %d", s_pl_page + 1, s_pl_pages);
        td5_vui_text_centered(320.0f * sx, (float)(PL_CTL_Y + 30) * sy, buf, 0xFF8890A0u, sx, sy);
    }
    td5_vui_text_centered(320.0f * sx, (float)(PL_CTL_Y + 56) * sy,
                          "ENTER = DETAILS   -   SUPR = DELETE   -   B / BACK = RETURN",
                          0xFF8890A0u, sx, sy);

    if (s_pl_modal_open) frontend_pending_render_modal(sx, sy);
}

void Screen_PendingTest(void) {
    switch (s_inner_state) {
    case 0:
        frontend_load_tga("Front_End/MainMenu.tga", "Front_End/FrontEnd.zip");
        frontend_init_return_screen(TD5_SCREEN_PENDING_TEST);
        s_return_screen = TD5_SCREEN_CHANGELOG;   /* reached FROM the changelog screen */
        s_pl_page       = 0;
        s_pl_modal_open = 0;
        s_pl_modal_item = -1;
        frontend_pending_build_buttons();
        s_anim_complete = 1;   /* instant screen: enables B/ESC + fade-in chime */
        s_inner_state   = 1;
        TD5_LOG_I(LOG_TAG, "Screen_PendingTest: enter (%d items)", td5_pending_count());
        /* Dev harness: TD5RE_PENDING_MODAL_TEST=1 auto-opens an item's details
         * modal on entry (item 0 by default; TD5RE_PENDING_MODAL_TEST_ITEM=N
         * picks another index), so a headless launch (StartScreen=42) can
         * verify the modal draws without live ENTER input -- same pattern as
         * TD5RE_MP_SIMUL_PREVIEW_PHASE for the MP profile screen. */
        if (td5_pending_count() > 0 && td5_env_flag_on("TD5RE_PENDING_MODAL_TEST")) {
            s_pl_modal_item = td5_env_int("TD5RE_PENDING_MODAL_TEST_ITEM", 0, 0,
                                          td5_pending_count() - 1);
            s_pl_modal_open = 1;
            TD5_LOG_I(LOG_TAG, "Screen_PendingTest: TD5RE_PENDING_MODAL_TEST auto-open item=%d",
                      s_pl_modal_item);
        }
        break;

    case 1: {
        /* SUPR / DELETE drops the currently-highlighted checklist row outright
         * (vs ENTER, which opens the details modal). td5_plat_input_key_pressed
         * is level-triggered, so debounce on the rising edge: one delete per
         * press. */
        static int s_pl_supr_down = 0;
        int supr;

        /* The details modal owns the frame while open: a fresh confirm press
         * (ENTER / pad A / click) OR ESC/gamepad-B closes it and returns focus
         * to the list underneath. frontend_check_escape() consumes the escape
         * edge here so the central back-handler (td5_frontend.c step 7) can't
         * ALSO fire on the same press and leave the screen entirely -- same
         * "steal the edge" idiom td5_gameopts_name_edit_tick uses for its own
         * sub-mode. Everything else (SUPR, paging, BACK) is swallowed. */
        if (s_pl_modal_open) {
            if ((s_input_ready && s_button_index >= 0) || frontend_check_escape()) {
                s_pl_modal_open = 0;
                s_pl_modal_item = -1;
                frontend_play_sfx(5);
            }
            break;
        }

        supr = td5_plat_input_key_pressed(0xD3);   /* DIK_DELETE ("SUPR") */
        if (supr && !s_pl_supr_down &&
            s_selected_button >= 0 && s_selected_button < s_pl_row_count) {
            int sel  = s_selected_button;
            int item = s_pl_page * PL_ROWS_PER_PAGE + sel;
            td5_pending_delete(item);
            frontend_pending_build_buttons();          /* page/row counts shifted */
            if (s_pl_row_count > 0) {                  /* keep the cursor in place */
                if (sel >= s_pl_row_count) sel = s_pl_row_count - 1;
                s_selected_button = sel;
            }
            frontend_play_sfx(5);
        }
        s_pl_supr_down = supr;

        if (s_input_ready && s_button_index >= 0) {
            int b = s_button_index;
            if (b < s_pl_row_count) {                 /* open the details modal for this row */
                int item = s_pl_page * PL_ROWS_PER_PAGE + b;
                s_pl_modal_item = item;
                s_pl_modal_open = 1;
                frontend_play_sfx(3);
                TD5_LOG_I(LOG_TAG, "Screen_PendingTest: modal open item=%d", item);
            } else if (b == s_pl_overlay_btn) {
                td5_pending_toggle_overlay();
                frontend_play_sfx(2);
            } else if (b == s_pl_back_btn) {
                frontend_play_sfx(5);
                td5_frontend_set_screen(TD5_SCREEN_CHANGELOG);   /* back to the changelog menu */
            } else if (s_pl_prev_btn >= 0 && b == s_pl_prev_btn) {
                if (s_pl_page > 0) s_pl_page--;
                frontend_pending_build_buttons();
                frontend_play_sfx(2);
            } else if (s_pl_next_btn >= 0 && b == s_pl_next_btn) {
                if (s_pl_page < s_pl_pages - 1) s_pl_page++;
                frontend_pending_build_buttons();
                frontend_play_sfx(2);
            }
        }
        break;
    }
    }
}
