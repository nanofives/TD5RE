/**
 * td5_roam_route_ui.h -- GEO FREE ROAM: route planner input + drawing (PORT-ONLY, F5-A).
 * Model and planner: td5_roam_route.h.
 *
 * KEYS (roam, driving):   F6 START at the car    F7 WAYPOINT at the car    F8 FINISH at the car
 *                         Backspace undo         Delete clear the route    M route map
 * MAP OVERLAY (M, from driving or from the pause menu; the sim stays frozen by the pause menu):
 *   arrows pan, + / - or wheel zoom, Enter / click = place (AUTO: first START, then WAYPOINT),
 *   1 / 2 / 3 = place START / WAYPOINT / FINISH at the cross-hair, X remove the nearest pin,
 *   Backspace undo, Delete clear, C centre on the car, F fit the route, S save (type a name,
 *   Enter), L load list (Up/Down, Enter), M back to the pause menu, Esc back to driving.
 *   Mouse: click places, drag a pin moves it, drag the empty map pans.
 *
 * HOOKS (td5_game.c / td5_hud.c, each a one-line call):
 *   td5_roam_route_ui_frame        once per rendered frame, top of frame_run_sim_loop
 *   td5_roam_route_ui_want_pause   OR-ed into the ESC / PAUSE edge that opens the pause menu
 *   td5_roam_route_ui_map_open     skips the pause menu's own input + draw while the map is up
 *   td5_roam_route_ui_draw_overlay the full-screen map (replaces the pause panel)
 *   td5_roam_route_ui_draw_hud     key hint + toast line, in the HUD overlay pass
 *   td5_roam_route_ui_draw_world   on-road guidance, per pane, after the world pass
 *   td5_roam_route_ui_draw_minimap inside td5_hud_render_minimap
 */
#ifndef TD5_ROAM_ROUTE_UI_H
#define TD5_ROAM_ROUTE_UI_H

#include <stdint.h>

void td5_roam_route_ui_frame(void);
/* [F8b] end the planner session now (route, type + laps, planner graph): td5_roam_end calls it. The frame hook
 * only runs inside a race, so without it the next roam session inherited the last one's route and type. */
void td5_roam_route_ui_session_end(void);
int  td5_roam_route_ui_want_pause(void);      /* 1 once per M press while driving */
int  td5_roam_route_ui_map_open(void);
/* [F6-B] open / close the route map from the sprint flow (open also asks for the pause menu) */
void td5_roam_route_ui_open_map(void);
void td5_roam_route_ui_close_map(void);
void td5_roam_route_ui_fit(void);             /* [F9a] frame the whole route in the map (DEV shots) */
void td5_roam_route_ui_draw_overlay(void);
void td5_roam_route_ui_draw_hud(void);
void td5_roam_route_ui_draw_world(int view);
/* x,y,w,h: the minimap pane, screen pixels. Draws a heading-up street map + the route. */
void td5_roam_route_ui_draw_minimap(float x, float y, float w, float h);

#ifndef TD5RE_RELEASE
/* DEV: log the route state ("ROUTE_STATE ...") so a script can compare it across runs. */
void td5_roam_route_ui_log_state(const char *tag);
#endif

#endif /* TD5_ROAM_ROUTE_UI_H */
