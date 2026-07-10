/*
 * td5_fe_devscreens.h -- public API of td5_fe_devscreens.c (CHANGELOG +
 * PENDING TO TEST dev screens, UI GUIDE / MP GUIDE galleries). Split out of
 * td5_frontend_internal.h (2026-07-09, A9 refactor) so this module's own
 * surface is declared once, in its own header, instead of scattered
 * through the shared frontend-internal grab-bag. s_fe_preserve_case,
 * fe_draw_text/fe_measure_text/fe_wrap_text_lines/
 * frontend_get_button_render_rect stay in td5_frontend_internal.h -- they're
 * defined in td5_frontend.c and merely consumed here.
 */

#ifndef TD5_FE_DEVSCREENS_H
#define TD5_FE_DEVSCREENS_H

void Screen_Changelog(void);
void frontend_changelog_render(float sx, float sy);

void Screen_PendingTest(void);
void frontend_pending_render(float sx, float sy);

void Screen_UiGuide(void);              /* dev UI style guide (slot 1) */
void frontend_uiguide_render(float sx, float sy);

void Screen_MpGuide(void);              /* dev MP-widgets gallery (slot 44) */
void frontend_mpguide_render(float sx, float sy);

#endif /* TD5_FE_DEVSCREENS_H */
