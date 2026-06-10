/*
 * td5_frontend_internal.h -- shared state surface of the frontend module.
 *
 * The frontend is split across several TUs (td5_frontend.c core + per-screen
 * cluster files td5_fe_*.c). Everything here is INTERNAL to the frontend --
 * other modules must keep using the public td5_frontend.h API. Symbols listed
 * here used to be file-scope statics of the monolithic td5_frontend.c; they
 * are documented at their definitions in td5_frontend.c.
 */

#ifndef TD5_FRONTEND_INTERNAL_H
#define TD5_FRONTEND_INTERNAL_H

#include <stdint.h>
#include "td5_types.h"

extern char s_lobby_password[32];
extern char s_mp_player_name[TD5_MAX_HUMAN_PLAYERS][16];
extern const uint32_t k_mp_player_colors[TD5_MAX_HUMAN_PLAYERS];
extern int      s_fe_logic_ticks;
extern int      s_mp_car_player;
extern int      s_mp_flow;
extern int      s_mp_join_device[TD5_MAX_HUMAN_PLAYERS];
extern int      s_mp_joined_count;
extern int      s_mp_player_car[TD5_MAX_HUMAN_PLAYERS];
extern int      s_mp_player_color[TD5_MAX_HUMAN_PLAYERS];
extern int      s_mp_player_paint[TD5_MAX_HUMAN_PLAYERS];
extern int      s_mp_player_ready[TD5_MAX_HUMAN_PLAYERS];
extern int      s_mp_simul;
extern int  s_anim_complete;
extern int  s_anim_tick;
extern int  s_arrow_input;
extern int  s_button_index;
extern int  s_flow_context;
extern int  s_inner_state;
extern int  s_input_ready;
extern int  s_kicked_flag;
extern int  s_launching_net_race;
extern int  s_lobby_action;
extern int  s_lobby_max_players;
extern int  s_lobby_modal;
extern int  s_mp_phase;
extern int  s_mp_player_accent[TD5_MAX_HUMAN_PLAYERS];
extern int  s_net_cfg_game_port;
extern int  s_net_session_sel;
extern int  s_network_active;
extern int  s_nickname_from_mpopts;
extern int  s_num_human_players;
extern int  s_return_screen;
extern int  s_selected_car;
extern int  s_text_input_state;
extern int  s_two_player_mode;
extern int s_selected_button;
extern uint32_t s_mp_pane_nav_prev[TD5_MAX_HUMAN_PLAYERS];
extern uint32_t s_mp_simul_ready_ms;
float frontend_update_timed_animation(int max_tick, uint32_t duration_ms);
int frontend_check_escape(void);
int frontend_create_button(const char *label, int x, int y, int w, int h);
int frontend_load_tga(const char *name, const char *archive);
int frontend_option_delta(void);
int frontend_text_input_confirmed(void);
void Screen_ConnectionBrowser(void);
void Screen_CreateSession(void);
void Screen_DirectConnect(void);
void Screen_LanMenu(void);
void Screen_MultiplayerLobby(void);
void Screen_NetNickname(void);
void Screen_NetworkLobby(void);
void Screen_SessionLocked(void);
void Screen_SessionPicker(void);
void frontend_begin_text_input(char *buffer, int capacity);
void frontend_begin_timed_animation(void);
void frontend_handle_text_input_key(void);
void frontend_init_display_mode_state(void);
void frontend_init_race_schedule(void);
void frontend_init_return_screen(TD5_ScreenIndex screen);
void frontend_net_destroy(void);
void frontend_play_sfx(int id);
void frontend_present_buffer(void);
void frontend_render_session_locked_overlay(float sx, float sy);
void frontend_reset_buttons(void);
void td5_frontend_set_screen(TD5_ScreenIndex index);
#define FE_MAX_BUTTONS 16

typedef struct {
    int active;
    int x, y, w, h;
    int disabled;
    int hidden;             /* 1 = not drawn at all (mirrors original moving the
                             * sprite off-screen, e.g. the Direction toggle on
                             * forward-only/circuit tracks). Pair with disabled
                             * so nav/mouse also skip it. */
    int highlight_ramp;     /* 0-6: smooth highlight fade (original uses 6-step ramp) */
    int is_selector;        /* 1 = left/right selector widget; always uses blue 9-slice */
    char label[64];
} FE_Button;

extern FE_Button s_buttons[FE_MAX_BUTTONS];
extern char s_text_input_prompt[40];

/* @GENERATED-SYMBOLS@ */

#endif /* TD5_FRONTEND_INTERNAL_H */
