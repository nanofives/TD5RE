/*
 * td5_fe_carstats.h -- public API of td5_fe_carstats.c (car stat bars +
 * physics-derived MORE STATS panel). Split out of td5_frontend_internal.h
 * (2026-07-09, A9 refactor) so this module's own surface is declared once,
 * in its own header, instead of scattered through the shared frontend-
 * internal grab-bag. s_car_spec/s_car_spec_car stay in
 * td5_frontend_internal.h -- they're defined in td5_frontend.c and merely
 * consumed here.
 */

#ifndef TD5_FE_CARSTATS_H
#define TD5_FE_CARSTATS_H

void  frontend_render_car_stats_overlay(float sx, float sy);
void  frontend_speed_band_for_tier(int tier, int *lo, int *hi);
void  frontend_load_car_spec_fields(int car_index);
float frontend_car_speed_norm(int car);
float frontend_speed_tier_center(int tier);
int   frontend_carphys_speed_tier(int car);
void  frontend_render_physics_stats(int ext_id, float px, float py, float pw, float ph,
                                    uint32_t accent, int compact, float sx, float sy);
void  frontend_draw_car_stat_bars(float bx, float by, float bw, float bh,
                                  const char *f7, const char *f8, int car_ext_id,
                                  uint32_t accent, int compact, float frame_scale,
                                  float sx, float sy);

#endif /* TD5_FE_CARSTATS_H */
