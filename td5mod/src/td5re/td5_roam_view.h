/**
 * td5_roam_view.h -- GEO FREE ROAM dev viewer (PORT-ONLY, DEV-ONLY, F1 task T8).
 * Implementation and frame chain: td5_roam_view.c.
 *
 * Draws the ROAMTILE road layer near the free-cam eye through the collision
 * overlay's debug-line path: boundary / wall edges white, ribbon quad and patch
 * triangle edges cyan. Knobs: TD5RE_ROAM_VIEW=1 turns it on (default off),
 * TD5RE_ROAM_VIEW_R=<metres> sets the distance cull (default 400).
 * Compiled out of RELEASE.
 */
#ifndef TD5_ROAM_VIEW_H
#define TD5_ROAM_VIEW_H

#include <stdint.h>

#ifndef TD5RE_RELEASE

/* 1 when TD5RE_ROAM_VIEW=1. */
int  td5_roam_view_wanted(void);

/* Queue the resident road tiles near the camera as debug lines. The caller
 * resets and flushes the debug-line batch around the call. cam_x/y/z is the
 * free-cam eye in raw 24.8 world units (the collision wireframe's units). A
 * no-op when the geo place has no ROAMTILE set. */
void td5_roam_view_draw(int32_t cam_x, int32_t cam_y, int32_t cam_z);

#endif /* TD5RE_RELEASE */
#endif /* TD5_ROAM_VIEW_H */
