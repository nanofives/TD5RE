/**
 * td5_frontend_button_cache.h -- Baked frontend button surface cache
 *
 * Phase 6 of frontend mainmenu parity, extended to all eligible frontend
 * screens. Mirrors the original CreateFrontendDisplayModeButton
 * (0x00425DE0) lifecycle:
 *   - <bw> x <2*bh> BGRA texture per button (typically 224x64).
 *   - Top half  (y = 0..bh-1)  = state 1 (unselected, transparent interior).
 *   - Bottom half (y = bh..2*bh-1) = state 0 (selected,  purple interior).
 *
 * Per-frame rendering becomes a single fe_draw_quad blit reading the half
 * picked by focus, replacing the per-frame 9-slice + per-glyph fan
 * (~21 quads/button -> 1 quad/button).
 *
 * Eligibility: any non-disabled, non-selector button with a non-empty
 * label. Disabled (state 2) and selector buttons (need arrows + value
 * text on top) fall back to the per-frame 9-slice path.
 *
 * Lifecycle: lazy bake on first render via td5_fe_btncache_ensure_page().
 * Source PNG pixels (ButtonBits + BodyText) are loaded once and retained
 * for re-bakes triggered by label changes (screen transitions reuse
 * button slots with different labels -> cache miss -> re-bake).
 *
 * CPU compositing only -- no GPU render-to-texture. Source pixels are
 * BGRA32 buffers loaded via td5_asset_load_png_to_buffer().
 */

#ifndef TD5_FRONTEND_BUTTON_CACHE_H
#define TD5_FRONTEND_BUTTON_CACHE_H

#include <stdint.h>

/* Must be >= FE_MAX_BUTTONS (defined in td5_frontend.c). 16 currently. */
#define TD5_FE_BTNCACHE_MAX 16

/* Reset all cache slots (drops pixel buffers + invalidates pages).
 * Texture pages remain allocated in the platform layer; subsequent
 * bake calls will re-upload. */
void td5_fe_btncache_reset(void);

/* Bake one button cache slot.
 *   index           -- 0..TD5_FE_BTNCACHE_MAX-1, also the FE_Button index.
 *   label           -- nul-terminated ASCII label, used for staleness check.
 *   w, h            -- per-half dimensions (cache texture is w x (2*h)).
 *   bb_bgra/bbw/bbh -- ButtonBits.png BGRA32 source pixels (56x100 typical).
 *   font_bgra/fw/fh -- BodyText.png BGRA32 font atlas pixels.
 *   glyph_advance   -- 96-entry per-glyph advance widths (ASCII 0x20..0x7F).
 * Returns 1 on success (page uploaded), 0 on failure. */
int td5_fe_btncache_bake_button(int index, const char *label, int w, int h,
                                const uint8_t *bb_bgra, int bbw, int bbh,
                                const uint8_t *font_bgra, int fw, int fh,
                                const uint8_t *glyph_advance);

/* Returns the absolute texture page for `index` if the cached label +
 * dims match, or -1 if not cached / mismatch. */
int td5_fe_btncache_get_page(int index, const char *label);

/* Lazy bake helper: returns the cache page for (index, label, w, h),
 * baking on demand if the slot is empty or stale. Loads ButtonBits +
 * BodyText PNG source pixels once on first call (held until shutdown).
 * Returns the absolute texture page on success, -1 on failure (PNG load
 * fail, alloc fail, upload fail). Safe to call every frame -- the hot
 * path is one strcmp + one memcmp via the get_page check. */
int td5_fe_btncache_ensure_page(int index, const char *label, int w, int h,
                                const uint8_t *glyph_advance);

/* Drop the retained source pixel buffers. Call at shutdown. */
void td5_fe_btncache_release_sources(void);

/* Re-upload retained pixel buffers to their texture pages. Called from
 * frontend_recover_surfaces() so cached buttons survive device reset /
 * native resolution changes. No-op for empty slots. */
void td5_fe_btncache_recover(void);

#endif /* TD5_FRONTEND_BUTTON_CACHE_H */
