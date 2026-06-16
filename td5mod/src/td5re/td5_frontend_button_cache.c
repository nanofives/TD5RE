/**
 * td5_frontend_button_cache.c -- Baked main-menu button surface cache (CPU).
 *
 * [2026-06-16 RETIRED] Under VectorUI (the shipped default) every frontend
 * button frame is drawn procedurally via the ps_roundrect shader path
 * (fe_draw_button_frame -> fe_draw_roundrect in td5_frontend.c). This CPU
 * button cache -- which composited the ButtonBits.png 9-slice + the BodyText.png
 * glyph atlas into a baked 224x64 surface per button -- was only reachable with
 * VectorUI OFF, and its only call site (td5_fe_btncache_ensure_page from the
 * main button loop) has been removed. With it gone the cache never loads
 * ButtonBits.png or BodyText.png, so both source assets are deletable.
 *
 * The public API is kept as inert no-op stubs so the lifecycle callers in
 * td5_frontend.c (reset / release_sources / recover) still link without change.
 * The former bake path (9-slice + glyph compositor, ButtonBits/BodyText loader)
 * is removed wholesale. If VectorUI-off baked buttons are ever wanted again,
 * recover the implementation from git history (pre-2026-06-16).
 */

#include "td5_frontend_button_cache.h"

/* ----------------------------------------------------------------- API
 * All stubs: no bake, no source-pixel load, no texture pages held. */

void td5_fe_btncache_reset(void) {
}

int td5_fe_btncache_bake_button(int index, const char *label, int w, int h,
                                const uint8_t *bb_bgra, int bbw, int bbh,
                                const uint8_t *font_bgra, int fw, int fh,
                                const uint8_t *glyph_advance) {
    (void)index; (void)label; (void)w; (void)h;
    (void)bb_bgra; (void)bbw; (void)bbh;
    (void)font_bgra; (void)fw; (void)fh; (void)glyph_advance;
    return 0;
}

int td5_fe_btncache_get_page(int index, const char *label) {
    (void)index; (void)label;
    return -1;
}

int td5_fe_btncache_ensure_page(int index, const char *label, int w, int h,
                                const uint8_t *glyph_advance) {
    (void)index; (void)label; (void)w; (void)h; (void)glyph_advance;
    return -1;
}

void td5_fe_btncache_release_sources(void) {
}

void td5_fe_btncache_recover(void) {
}
