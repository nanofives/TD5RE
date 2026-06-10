/*
 * td5_fe_net.c -- frontend screens: net play / multiplayer lobbies.
 *
 * Split out of td5_frontend.c (2026-06). Handlers: ConnectionBrowser[8],
 * SessionPicker[9], CreateSession[10], NetworkLobby[11], SessionLocked[29],
 * MultiplayerLobby[30], LanMenu[31], DirectConnect[32], NetNickname[33].
 * Shared frontend state comes from td5_frontend_internal.h; original binary
 * addresses are noted in the per-screen comments.
 */

#include "td5_frontend.h"
#include "td5_asset.h"
#include "td5_game.h"
#include "td5_input.h"
#include "td5_net.h"
#include "td5_platform.h"
#include "td5_render.h"
#include "td5_save.h"
#include "td5_sound.h"
#include "td5_hud.h"
#include "td5re.h"
#include "td5_snk_strings.h"
#include "td5_vectorui.h"
#include "td5_font.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define LOG_TAG "frontend"
#include "td5_color.h"
#include "td5_frontend_internal.h"

static uint32_t s_mp_join_prev    = 0;     /* lobby join-scan mask last frame (edge) */

/* --- S10 net-play: explicit connection modes (LAN / Direct-IP) --- */
static char s_net_direct_ip[64];        /* "ip" or "ip:port" entry buffer (Direct join) */

static int  s_lobby_modal_armed;        /* 1 once the opening Enter is released */

static int  s_net_join_pending_ui;      /* awaiting JOIN_ACK before entering lobby */

static uint32_t s_net_join_wait_start;  /* tick when the join wait began (timeout) */

static int  s_net_cfg_enable_upnp = 1;       /* [Network] EnableUPnP (Direct host) */

static int  s_dialog_mode;              /* DAT_00496350 */

static int  s_per_slot_status[6];       /* DAT_00496980[6] */

static int  s_config_received[6];       /* DAT_00497262[6] */

static int  s_participant_flags[6];     /* DAT_0049725c[6] */

static int  s_race_active_flag;         /* DAT_00497324 */

static int  s_chat_dirty;              /* DAT_0049640c */

static uint32_t s_last_poll_timestamp;  /* DAT_004968a8 */

static char s_chat_input_buffer[64];    /* DAT_004972cc */

static char s_create_session_name[64];

static void frontend_set_text_input_prompt(const char *p) {
    if (p && p[0]) {
        strncpy(s_text_input_prompt, p, sizeof(s_text_input_prompt) - 1);
        s_text_input_prompt[sizeof(s_text_input_prompt) - 1] = '\0';
    } else {
        s_text_input_prompt[0] = '\0';
    }
}

/**
 * Send a network message via td5_net.
 * Wraps td5_net_send with the DXPTYPE cast and optional payload header.
 * For DATA messages (type 1), prepends a 4-byte payload size header
 * matching the original DXPDATA wire format.
 */
static void frontend_net_send(int type, const void *data, int size) {
    td5_net_send((TD5_NetMsgType)type, data, size);
}

/**
 * Receive a network message from the ring buffer.
 * Returns the DXPTYPE message type (0-12) or -1 if no message available.
 * Copies payload into buf (up to max_size bytes).
 */
/* S31: cycle the OPTIONS-modal kick target through {off} + the joined
 * remote slots. dir = +1 / -1. */
static int lobby_cycle_kick(int cur, int dir) {
    int cand[TD5_NET_MAX_PLAYERS + 1];
    int n = 0, i, idx = 0;
    cand[n++] = -1;
    for (i = 0; i < TD5_NET_MAX_PLAYERS; i++)
        if (td5_net_is_slot_active(i) && i != td5_net_local_slot())
            cand[n++] = i;
    for (i = 0; i < n; i++)
        if (cand[i] == cur) { idx = i; break; }
    idx = (idx + dir + n) % n;
    return cand[idx];
}

static int frontend_net_receive(void *buf, int max_size) {
    TD5_NetMsgType type;
    void *data = NULL;
    int size = 0;

    if (!td5_net_receive(&type, &data, &size))
        return -1;

    /* Copy payload into caller's buffer */
    if (data && size > 0 && buf && max_size > 0) {
        int copy_size = (size < max_size) ? size : max_size;
        memcpy(buf, data, (size_t)copy_size);
    }

    return (int)type;
}

/**
 * Seal or unseal the session (prevent/allow new joins).
 */
static void frontend_net_seal(int sealed) {
    td5_net_seal_session(sealed);
}

/**
 * Enumerate network providers and sessions.
 * Initializes the network subsystem if needed, enumerates connections,
 * picks the first (UDP LAN), and triggers session discovery.
 * Returns the number of sessions found.
 */
static int frontend_net_enumerate(void) {
    int conn_count;

    /* Initialize network if not already done (td5_net_init is idempotent) */
    if (!td5_net_init()) {
        TD5_LOG_W(LOG_TAG, "frontend_net_enumerate: td5_net_init failed");
        return 0;
    }

    conn_count = td5_net_enumerate_connections();
    if (conn_count > 0) {
        td5_net_pick_connection(0);
    }

    return td5_net_enumerate_sessions();
}

/**
 * Check if the local player is the session host.
 */
static int frontend_net_is_host(void) {
    return td5_net_is_host();
}

/**
 * Get the local player's slot index (0-5).
 */
static int frontend_net_local_slot(void) {
    return td5_net_local_slot();
}

/* ========================================================================
 * [30] Multiplayer Lobby  (PORT ENHANCEMENT 2026-06)
 *
 * Press-to-join: each input that presses A (joystick) / Enter (keyboard) joins
 * in order (join order = player number) and shows as READY. START (the button,
 * SPACE, or a joined player's confirm) proceeds to the per-player car select.
 * ======================================================================== */
void Screen_MultiplayerLobby(void) {
    switch (s_inner_state) {
    case 0:
        frontend_init_return_screen(TD5_SCREEN_MP_LOBBY);
        TD5_LOG_I(LOG_TAG, "MP Lobby: init");
        frontend_load_tga("Front_End/MainMenu.tga", "Front_End/FrontEnd.zip");
        /* [PERF 2026-06-06] Dropped a td5_input_enumerate_devices() here — it was a
         * ~120ms blocking IDirectInput8::EnumDevices on the lobby's entry frame.
         * The device list is already kept current by init + the WM_DEVICECHANGE
         * rescan, td5_plat_input_scan_join builds its scan handles lazily, and the
         * per-frame join scan below picks up any hot-plug within a frame or two. */
        s_mp_joined_count = 0;
        memset(s_mp_join_device, 0, sizeof(s_mp_join_device));
        s_mp_join_prev = td5_plat_input_scan_join();/* ignore inputs already held on entry */
        frontend_reset_buttons();
        frontend_create_button(SNK_StartRaceTxt, 220, 300, 200, 32);  /* 0 START */
        frontend_create_button(SNK_BackButTxt,   260, 360, 120, 32);  /* 1 BACK */
        s_selected_button = 0;
        s_anim_complete = 0;
        frontend_begin_timed_animation();
        s_inner_state = 1;
        break;
    case 1: case 2:
        frontend_present_buffer();
        s_inner_state++;
        break;
    case 3:
        if (frontend_update_timed_animation(0x27, 650) >= 1.0f) {
            s_anim_complete = 1;
            s_inner_state = 6;
        }
        break;
    case 6: {
        uint32_t scan  = td5_plat_input_scan_join();
        uint32_t newly = scan & ~s_mp_join_prev;
        int kbd_joined = 0, joined_now = 0, j, d, do_start = 0, do_back = 0;
        s_mp_join_prev = scan;

        for (j = 0; j < s_mp_joined_count; j++)
            if (s_mp_join_device[j] == 0) kbd_joined = 1;

        /* Joystick joins (devices 1..) in join order. */
        for (d = 1; d < 16; d++) {
            int already = 0;
            if (!(newly & (1u << d))) continue;
            for (j = 0; j < s_mp_joined_count; j++)
                if (s_mp_join_device[j] == d) already = 1;
            if (!already && s_mp_joined_count < TD5_MAX_HUMAN_PLAYERS) {
                s_mp_join_device[s_mp_joined_count++] = d;
                joined_now = 1;
                frontend_play_sfx(3);
                TD5_LOG_I(LOG_TAG, "MP Lobby: player %d join device %d", s_mp_joined_count, d);
            }
        }
        /* Keyboard join via Enter (device 0). The FIRST Enter joins; once joined,
         * Enter falls through to confirm the START button. */
        if ((newly & 1u) && !kbd_joined && s_mp_joined_count < TD5_MAX_HUMAN_PLAYERS) {
            s_mp_join_device[s_mp_joined_count++] = 0;
            joined_now = 1;
            frontend_play_sfx(3);
            TD5_LOG_I(LOG_TAG, "MP Lobby: player %d join keyboard", s_mp_joined_count);
        }

        if (!joined_now) {
            if (s_input_ready && s_button_index == 0) do_start = 1;  /* START button */
            if (s_input_ready && s_button_index == 1) do_back  = 1;  /* BACK button  */
            if (td5_plat_input_key_pressed(0x39))      do_start = 1;  /* SPACE        */
        }
        if (frontend_check_escape()) do_back = 1;                     /* ESC / gamepad B */

        if (do_start && s_mp_joined_count >= 1) {
            int p;
            s_num_human_players = s_mp_joined_count;
            s_two_player_mode   = 1;       /* engage split-screen multiplayer */
            s_mp_flow           = 1;
            /* 2+ players pick simultaneously in a grid (each on their own pad);
             * a lone player just gets the normal single-player car select. */
            s_mp_simul          = (s_mp_joined_count >= 2);
            s_mp_phase          = 0;       /* start at the name + colour setup window */
            for (p = 0; p < s_mp_joined_count; p++) {
                s_mp_player_car[p]    = s_selected_car;
                s_mp_player_paint[p]  = 0;
                s_mp_player_color[p]  = -1;
                s_mp_player_ready[p]  = 0;
                s_mp_pane_nav_prev[p] = 0;
                s_mp_player_name[p][0] = '\0';
                s_mp_player_accent[p] = (int)(k_mp_player_colors[p % TD5_MAX_HUMAN_PLAYERS] & 0x00FFFFFFu);
                /* Simultaneous select reads each pad through the still-alive,
                 * NON-exclusive scan handles, so per-player EXCLUSIVE devices
                 * (which would release those handles) are NOT bound until the
                 * picks are locked. Sequential / single binds now, as before. */
                if (!s_mp_simul) {
                    td5_input_set_input_source(p, s_mp_join_device[p]);
                    td5_save_set_player_device_index(p, (uint32_t)s_mp_join_device[p]);
                }
            }
            s_mp_car_player = 0;
            s_mp_simul_ready_ms = 0;
            if (!s_mp_simul)
                td5_plat_input_scan_join_release();
            TD5_LOG_I(LOG_TAG, "MP Lobby: START %d players -> %s car select",
                      s_mp_joined_count, s_mp_simul ? "simultaneous grid" : "sequential");
            td5_frontend_set_screen(TD5_SCREEN_CAR_SELECTION);
            return;
        }
        if (do_back) {
            s_mp_flow = 0;
            s_mp_simul = 0;
            td5_plat_input_scan_join_release();
            td5_frontend_set_screen(TD5_SCREEN_MAIN_MENU);
            return;
        }
        break;
    }
    default:
        td5_plat_input_scan_join_release();
        td5_frontend_set_screen(TD5_SCREEN_MAIN_MENU);
        break;
    }
}

/* S10: the local player name presented to the lobby/roster (persisted nickname). */
static const char *frontend_net_player_name(void) {
    return (g_td5.ini.net_nickname[0]) ? g_td5.ini.net_nickname : "Player";
}

/* S10: split "ip" or "ip:port" into an IP string + port (default game port). */
static void frontend_net_parse_ip_port(const char *in, char *ip_out, int ip_len, int *port_out) {
    const char *colon = strchr(in, ':');
    *port_out = s_net_cfg_game_port;
    if (colon) {
        int n = (int)(colon - in);
        if (n >= ip_len) n = ip_len - 1;
        memcpy(ip_out, in, (size_t)n);
        ip_out[n] = '\0';
        int p = atoi(colon + 1);
        if (p > 0 && p <= 65535) *port_out = p;
    } else {
        snprintf(ip_out, (size_t)ip_len, "%s", in);
    }
}

/* ========================================================================
 * [8] Screen_ConnectionBrowser -- S10 ONLINE connection MODE SELECT
 *
 * Two explicit modes: LAN GAME (-> Screen_LanMenu: host / discover) and
 * DIRECT IP (-> Screen_DirectConnect: host / join by IP). On the first net-play
 * visit with no saved nickname, routes to the nickname-entry screen first.
 * ======================================================================== */
void Screen_ConnectionBrowser(void) {
    switch (s_inner_state) {
    case 0: /* Init: net up + mode-select buttons (LAN / DIRECT / BACK) */
        frontend_init_return_screen(TD5_SCREEN_CONNECTION_BROWSER);
        TD5_LOG_D(LOG_TAG, "ConnectionBrowser: mode select");
        /* Seed [Network] config from the ini (game port + UPnP toggle). */
        s_net_cfg_game_port   = (g_td5.ini.net_game_port > 0 && g_td5.ini.net_game_port <= 65535)
                                ? g_td5.ini.net_game_port : 37050;
        s_net_cfg_enable_upnp = g_td5.ini.net_enable_upnp;

        /* First net-play visit with no saved nickname -> prompt for one. */
        if (!g_td5.ini.net_nickname[0]) {
            td5_frontend_set_screen(TD5_SCREEN_NET_NICKNAME);
            return;
        }

        /* [PERF 2026-06-06] Do NOT run LAN session discovery here. This is the
         * net-play MODE-SELECT screen (LAN / DIRECT / BACK) — it never displays the
         * session list, so the old frontend_net_enumerate() blocked the entry frame
         * ~500ms (synchronous LAN broadcast+poll) for a result that was discarded.
         * The actual session browser (Screen_SessionPicker) runs discovery itself.
         * Just make sure the net stack is up (cheap, idempotent). */
        td5_net_init();
        frontend_load_tga("Front_End/MainMenu.tga", "Front_End/FrontEnd.zip");
        /* [2026-06-07] Regular main-menu button height (0x20) + BACK left-aligned. */
        frontend_create_button("LAN GAME",  120, 193, 496, 0x20);
        frontend_create_button("DIRECT IP", 120, 257, 496, 0x20);
        frontend_create_button(SNK_BackButTxt, 120, 377, 112, 0x20);
        s_anim_tick = 0;
        s_inner_state = 1;
        break;

    case 1:
        s_inner_state = 2;
        break;

    case 2: /* Slide-in */
        if (frontend_update_timed_animation(0x10, 267) >= 1.0f) {
            s_anim_complete = 1;
            s_inner_state = 3;
        }
        break;

    case 3:
        frontend_present_buffer();
        s_inner_state = 4;
        break;

    case 4:
        frontend_present_buffer();
        s_inner_state = 5;
        break;

    case 5: /* Mode-select interaction */
        if (s_input_ready) {
            if (s_button_index == 0) {            /* LAN GAME */
                td5_net_set_mode(TD5_NET_MODE_LAN);
                s_return_screen = TD5_SCREEN_LAN_MENU;
                s_inner_state = 8;
            } else if (s_button_index == 1) {     /* DIRECT IP */
                td5_net_set_mode(TD5_NET_MODE_DIRECT);
                s_return_screen = TD5_SCREEN_DIRECT_CONNECT;
                s_inner_state = 8;
            } else if (s_button_index == 2) {     /* BACK */
                s_return_screen = TD5_SCREEN_MAIN_MENU;
                s_inner_state = 8;
            }
        }
        break;

    case 6:
    case 7:
        s_inner_state = 5;
        break;

    case 8: /* Slide-out prep */
        frontend_begin_timed_animation();
        s_inner_state = 9;
        break;

    case 9: /* Slide-out -> next screen */
        if (frontend_update_timed_animation(16, 267) >= 1.0f) {
            td5_frontend_set_screen((TD5_ScreenIndex)s_return_screen);
        }
        break;
    }
}

/* ========================================================================
 * [33] Screen_NetNickname -- enter the player nickname (first net-play visit;
 * also reachable from Multiplayer Options). Persisted to td5re.ini [Network].
 * ======================================================================== */
void Screen_NetNickname(void) {
    switch (s_inner_state) {
    case 0:
        frontend_init_return_screen(TD5_SCREEN_NET_NICKNAME);
        frontend_load_tga("Front_End/MainMenu.tga", "Front_End/FrontEnd.zip");
        /* The text-input widget IS the field (gold frame at 120,193); only the
         * OK button is a real button (index 0). */
        frontend_create_button(SNK_OkButTxt, 232, 377, 160, 0x20);
        if (!g_td5.ini.net_nickname[0])
            snprintf(g_td5.ini.net_nickname, sizeof(g_td5.ini.net_nickname), "Player");
        frontend_begin_text_input(g_td5.ini.net_nickname, (int)sizeof(g_td5.ini.net_nickname));
        frontend_set_text_input_prompt("ENTER YOUR NICKNAME");
        s_inner_state = 1;
        break;

    case 1:
        frontend_handle_text_input_key();   /* process keystrokes into the buffer */
        if (frontend_check_escape()) {       /* ESC == cancel (don't persist) */
            int from_mpopts = s_nickname_from_mpopts;
            s_nickname_from_mpopts = 0;
            /* Back to MP options if that's where we came from, else the main
             * menu — NOT the connection browser, which would just bounce
             * straight back here on a first-run (empty) nickname. */
            td5_frontend_set_screen(from_mpopts ? TD5_SCREEN_TWO_PLAYER_OPTIONS
                                                : TD5_SCREEN_MAIN_MENU);
            break;
        }
        if (frontend_text_input_confirmed() || (s_input_ready && s_button_index == 0)) {
            int from_mpopts = s_nickname_from_mpopts;
            s_nickname_from_mpopts = 0;
            if (!g_td5.ini.net_nickname[0])
                snprintf(g_td5.ini.net_nickname, sizeof(g_td5.ini.net_nickname), "Player");
            td5_ini_write_str("Network", "Nickname", g_td5.ini.net_nickname);
            TD5_LOG_I(LOG_TAG, "Nickname set: \"%s\"", g_td5.ini.net_nickname);
            td5_frontend_set_screen(from_mpopts ? TD5_SCREEN_TWO_PLAYER_OPTIONS
                                                : TD5_SCREEN_CONNECTION_BROWSER);
        }
        break;
    }
}

/* ========================================================================
 * [31] Screen_LanMenu -- LAN GAME: host a new game or discover existing ones.
 * ======================================================================== */
void Screen_LanMenu(void) {
    switch (s_inner_state) {
    case 0:
        frontend_init_return_screen(TD5_SCREEN_LAN_MENU);
        td5_net_set_mode(TD5_NET_MODE_LAN);
        frontend_load_tga("Front_End/MainMenu.tga", "Front_End/FrontEnd.zip");
        /* [2026-06-07] Regular main-menu button height (0x20) + BACK left-aligned. */
        frontend_create_button("HOST NEW LAN GAME",  120, 193, 496, 0x20);
        frontend_create_button("DISCOVER LAN GAMES", 120, 257, 496, 0x20);
        frontend_create_button(SNK_BackButTxt, 120, 377, 112, 0x20);
        s_inner_state = 1;
        break;

    case 1:
        frontend_present_buffer();
        if (frontend_check_escape()) {            /* ESC == BACK */
            td5_frontend_set_screen(TD5_SCREEN_CONNECTION_BROWSER);
            break;
        }
        if (s_input_ready) {
            if (s_button_index == 0)              /* HOST -> name entry + create */
                td5_frontend_set_screen(TD5_SCREEN_CREATE_SESSION);
            else if (s_button_index == 1)         /* DISCOVER -> session list */
                td5_frontend_set_screen(TD5_SCREEN_SESSION_PICKER);
            else if (s_button_index == 2)         /* BACK */
                td5_frontend_set_screen(TD5_SCREEN_CONNECTION_BROWSER);
        }
        break;
    }
}

/* ========================================================================
 * [32] Screen_DirectConnect -- DIRECT IP: host a game (port + UPnP) or join one
 * by IP[:port]. Sub-layouts are swapped in place via frontend_reset_buttons()
 * so button indices never collide (the bug from the old inner-state version).
 * ======================================================================== */
void Screen_DirectConnect(void) {
    /* ESC == BACK [2026-06-07]. The chooser (state 1) backs out to the
     * connection browser; every JOIN/HOST sub-layout backs out to the chooser
     * (re-enter DIRECT_CONNECT at state 0). State 0 is skipped — it is still
     * building the layout this frame. */
    if (s_inner_state != 0 && frontend_check_escape()) {
        if (s_inner_state == 1) {
            td5_frontend_set_screen(TD5_SCREEN_CONNECTION_BROWSER);
        } else {
            /* Backing out of a JOIN/HOST sub-layout: drop any half-open session
             * so a cancelled host/join can't leak, then return to the chooser. */
            if (s_network_active) frontend_net_destroy();
            s_net_join_pending_ui = 0;
            td5_frontend_set_screen(TD5_SCREEN_DIRECT_CONNECT);
        }
        return;
    }
    switch (s_inner_state) {
    case 0: /* HOST / JOIN / BACK chooser */
        frontend_init_return_screen(TD5_SCREEN_DIRECT_CONNECT);
        td5_net_set_mode(TD5_NET_MODE_DIRECT);
        frontend_load_tga("Front_End/MainMenu.tga", "Front_End/FrontEnd.zip");
        /* [2026-06-07] Regular main-menu button height (0x20) and BACK left-
         * aligned with the action buttons above (x=120) for a consistent look. */
        frontend_create_button("HOST GAME", 120, 193, 496, 0x20);
        frontend_create_button("JOIN GAME", 120, 257, 496, 0x20);
        frontend_create_button(SNK_BackButTxt, 120, 377, 112, 0x20);
        s_inner_state = 1;
        break;

    case 1: /* chooser interaction */
        frontend_present_buffer();
        if (s_input_ready) {
            if (s_button_index == 0) {            /* HOST */
                if (td5_net_create_session_ex("TD5RE Game", frontend_net_player_name(),
                                              6, s_net_cfg_game_port, s_net_cfg_enable_upnp)) {
                    s_network_active = 1;
                    s_inner_state = 4;            /* show host status */
                } else {
                    TD5_LOG_W(LOG_TAG, "Direct host create failed");
                    td5_frontend_set_screen(TD5_SCREEN_CONNECTION_BROWSER);
                }
            } else if (s_button_index == 1) {     /* JOIN */
                snprintf(s_net_direct_ip, sizeof(s_net_direct_ip), "127.0.0.1");
                s_inner_state = 2;
            } else if (s_button_index == 2) {     /* BACK */
                td5_frontend_set_screen(TD5_SCREEN_CONNECTION_BROWSER);
            }
        }
        break;

    case 2: /* JOIN: build IP-entry layout (fresh buttons) */
        frontend_reset_buttons();
        frontend_load_tga("Front_End/MainMenu.tga", "Front_End/FrontEnd.zip");
        /* The text-input widget IS the field; only BACK is a real button (index 0). */
        frontend_create_button(SNK_BackButTxt, 278, 289, 112, 0x20);
        frontend_begin_text_input(s_net_direct_ip, (int)sizeof(s_net_direct_ip));
        frontend_set_text_input_prompt("ENTER HOST IP[:PORT]");
        s_inner_state = 3;
        break;

    case 3: /* JOIN: IP entry interaction */
        frontend_handle_text_input_key();   /* process keystrokes into the buffer */
        if (frontend_text_input_confirmed()) {
            char ip[64];
            int port = s_net_cfg_game_port;
            frontend_net_parse_ip_port(s_net_direct_ip, ip, sizeof(ip), &port);
            if (td5_net_join_direct(ip, port, frontend_net_player_name())) {
                s_network_active = 1;
                s_net_join_pending_ui = 1;
                s_net_join_wait_start = td5_plat_time_ms();
                s_inner_state = 5;                /* wait for JOIN_ACK */
            } else {
                TD5_LOG_W(LOG_TAG, "Direct join '%s' failed", s_net_direct_ip);
                td5_frontend_set_screen(TD5_SCREEN_DIRECT_CONNECT);
            }
            break;
        }
        if (s_input_ready && s_button_index == 0) {   /* BACK -> chooser */
            td5_frontend_set_screen(TD5_SCREEN_DIRECT_CONNECT);
        }
        break;

    case 4: /* HOST: show local IP + UPnP status (fresh buttons) */
        frontend_reset_buttons();
        frontend_load_tga("Front_End/MainMenu.tga", "Front_End/FrontEnd.zip");
        frontend_create_button(td5_net_get_status_text(), 80, 193, 480, 0x40);
        frontend_create_button("CONTINUE", 232, 377, 160, 0x20);
        s_inner_state = 6;
        break;

    case 6: /* HOST status interaction -> lobby */
        frontend_present_buffer();
        if (s_input_ready) {
            s_network_active = 1;
            td5_frontend_set_screen(TD5_SCREEN_NETWORK_LOBBY);
        }
        break;

    case 5: /* JOIN: wait for the host's JOIN_ACK (slot assigned) */
        frontend_present_buffer();
        if (td5_net_local_slot() >= 0) {
            s_net_join_pending_ui = 0;
            s_network_active = 1;
            td5_frontend_set_screen(TD5_SCREEN_NETWORK_LOBBY);
        } else if (td5_net_get_join_nak_reason() == 2) {  /* host needs a password */
            s_inner_state = 7;                            /* prompt + retry */
        } else if (td5_net_is_connection_lost() ||
                   (td5_plat_time_ms() - s_net_join_wait_start) > 8000) {
            TD5_LOG_W(LOG_TAG, "Direct join: no response / rejected (full)");
            s_net_join_pending_ui = 0;
            s_network_active = 0;
            frontend_net_destroy();
            td5_frontend_set_screen(TD5_SCREEN_DIRECT_CONNECT);
        }
        break;

    case 7: /* JOIN: the host rejected us for a wrong/missing password -> re-prompt */
        frontend_reset_buttons();
        frontend_load_tga("Front_End/MainMenu.tga", "Front_End/FrontEnd.zip");
        frontend_create_button(SNK_BackButTxt, 278, 289, 112, 0x20);
        s_lobby_password[0] = '\0';
        frontend_begin_text_input(s_lobby_password, (int)sizeof(s_lobby_password));
        frontend_set_text_input_prompt("PASSWORD REQUIRED");
        s_inner_state = 8;
        break;

    case 8: /* JOIN: password entry interaction -> re-join with the password */
        frontend_handle_text_input_key();
        if (frontend_text_input_confirmed()) {
            char ip[64];
            int port = s_net_cfg_game_port;
            td5_net_set_join_password(s_lobby_password);
            frontend_net_parse_ip_port(s_net_direct_ip, ip, sizeof(ip), &port);
            if (td5_net_join_direct(ip, port, frontend_net_player_name())) {
                s_net_join_wait_start = td5_plat_time_ms();
                s_inner_state = 5;
            } else {
                td5_frontend_set_screen(TD5_SCREEN_DIRECT_CONNECT);
            }
            break;
        }
        if (s_input_ready && s_button_index == 0) {   /* BACK -> chooser */
            td5_frontend_set_screen(TD5_SCREEN_DIRECT_CONNECT);
        }
        break;
    }
}

/* S10: set the SESSION_PICKER selector (button 0) label from s_net_session_sel
 * (discover-only: 0..count-1 are the discovered LAN sessions; hosting is the
 * separate LAN-menu "HOST" option). */
static void frontend_net_label_session_selector(void) {
    int count = td5_net_get_enum_session_count();
    char buf[64];
    if (count <= 0) {
        s_net_session_sel = 0;
        snprintf(buf, sizeof(buf), "(NO LAN GAMES FOUND)");
    } else {
        if (s_net_session_sel < 0) s_net_session_sel = count - 1;   /* wrap */
        if (s_net_session_sel >= count) s_net_session_sel = 0;
        snprintf(buf, sizeof(buf), "%s  (%d/%d)",
                 td5_net_get_enum_session_name(s_net_session_sel),
                 s_net_session_sel + 1, count);
    }
    strncpy(s_buttons[0].label, buf, sizeof(s_buttons[0].label) - 1);
    s_buttons[0].label[sizeof(s_buttons[0].label) - 1] = '\0';
}

void Screen_SessionPicker(void) {
    /* [PERF 2026-06-06] LAN discovery is now non-blocking + incremental, so poll it
     * every frame after init: the session list fills in live as hosts answer,
     * instead of the old 500ms select-poll freeze on the entry frame. */
    if (s_inner_state >= 1) {
        td5_net_enumerate_sessions();
        frontend_net_label_session_selector();
    }
    switch (s_inner_state) {
    case 0: /* Init: kick off LAN discovery + build the session selector */
        frontend_init_return_screen(TD5_SCREEN_SESSION_PICKER);
        TD5_LOG_D(LOG_TAG, "SessionPicker: init (LAN discovery)");
        td5_net_set_mode(TD5_NET_MODE_LAN);
        td5_net_enumerate_sessions();             /* start a discovery window (non-blocking) */
        frontend_load_tga("Front_End/MainMenu.tga", "Front_End/FrontEnd.zip");
        /* [FIXED 2026-06-02, runtime @0x499c78] list (120,193) 496x128; OK (120,377) 96; BACK (232,377) 112. */
        frontend_create_button(SNK_ChooseSessionButTxt, 120, 193, 496, 128);  /* slot 0: session selector */
        frontend_create_button(SNK_OkButTxt,     120, 377,  96, 0x20);   /* slot 1 */
        frontend_create_button(SNK_BackButTxt,   232, 377, 112, 0x20);   /* slot 2 */
        s_net_session_sel = 0;
        frontend_net_label_session_selector();
        s_anim_tick = 0;
        s_inner_state = 1;
        break;

    case 1: /* Build session list */
        s_inner_state = 2;
        break;

    case 2: /* Slide-in (~500ms) */
        if (frontend_update_timed_animation(0x10, 267) >= 1.0f) {
            s_anim_complete = 1;
            s_inner_state = 3;
        }
        break;

    case 3: /* Interaction */
        if (frontend_check_escape()) {        /* ESC == BACK (-> LAN menu) */
            s_return_screen = TD5_SCREEN_LAN_MENU;
            s_inner_state = 5;
            break;
        }
        if (s_input_ready) {
            /* Slot 0 = session selector (host or join target), slot 1 = OK, slot 2 = Back. */
            int active_button = (s_button_index >= 0) ? s_button_index : s_selected_button;
            int delta = frontend_option_delta();
            if (active_button == 0 && delta != 0) {
                s_net_session_sel += (delta > 0) ? 1 : -1;
                frontend_net_label_session_selector();
                frontend_play_sfx(2);
            } else if (s_button_index == 1) { /* OK -> join the selected session */
                if (td5_net_get_enum_session_count() <= 0) {
                    frontend_play_sfx(10);        /* nothing to join */
                } else if (td5_net_join_session(s_net_session_sel, frontend_net_player_name())) {
                    s_network_active = 1;
                    s_return_screen = TD5_SCREEN_NETWORK_LOBBY;
                    s_inner_state = 5;
                } else {
                    TD5_LOG_W(LOG_TAG, "LAN join %d failed", s_net_session_sel);
                    frontend_play_sfx(10);
                }
            } else if (s_button_index == 2) { /* Back -> LAN menu */
                s_return_screen = TD5_SCREEN_LAN_MENU;
                s_inner_state = 5;
            }
        }
        break;

    case 5: /* Slide-out prep */
        frontend_begin_timed_animation();
        s_inner_state = 6;
        break;

    case 6: /* Slide-out (~500ms) */
        if (frontend_update_timed_animation(16, 267) >= 1.0f) {
            td5_frontend_set_screen((TD5_ScreenIndex)s_return_screen);
        }
        break;

    default:
        break;
    }
}

void Screen_CreateSession(void) {
    switch (s_inner_state) {
    case 0: /* Init: show "Enter New Session Name" text input */
        frontend_init_return_screen(TD5_SCREEN_CREATE_SESSION);
        TD5_LOG_D(LOG_TAG, "CreateSession: init");
        frontend_load_tga("Front_End/MainMenu.tga", "Front_End/FrontEnd.zip");
        /* The text-input widget IS the name field (gold frame at 120,193);
         * only BACK is a real button (index 0). */
        frontend_create_button(SNK_BackButTxt, 278, 289, 112, 0x20);
        memset(s_create_session_name, 0, sizeof(s_create_session_name));
        strcpy(s_create_session_name, "New Session");
        frontend_begin_text_input(s_create_session_name, (int)sizeof(s_create_session_name));
        frontend_set_text_input_prompt("ENTER SESSION NAME");
        s_anim_tick = 0;
        s_inner_state = 1;
        break;

    case 1: /* Slide-in (~500ms) */
        if (frontend_update_timed_animation(0x10, 267) >= 1.0f) {
            s_anim_complete = 1;
            s_inner_state = 2;
        }
        break;

    case 2: /* Name input */
        frontend_handle_text_input_key();   /* process keystrokes into the buffer */
        if (frontend_check_escape()) {       /* ESC == BACK (-> LAN menu) */
            s_return_screen = TD5_SCREEN_LAN_MENU;
            s_inner_state = 3;
            break;
        }
        if (frontend_text_input_confirmed()) {
            /* S10: actually host a LAN session under the entered name. */
            td5_net_set_mode(TD5_NET_MODE_LAN);
            if (td5_net_create_session(s_create_session_name, frontend_net_player_name(), 6)) {
                s_network_active = 1;
                s_return_screen = TD5_SCREEN_NETWORK_LOBBY;
            } else {
                TD5_LOG_W(LOG_TAG, "LAN host create failed");
                s_return_screen = TD5_SCREEN_LAN_MENU;
            }
            s_inner_state = 3;
            break;
        }
        if (s_input_ready && s_button_index == 0) {   /* BACK -> LAN menu */
            s_return_screen = TD5_SCREEN_LAN_MENU;
            s_inner_state = 3;
        }
        break;

    case 3: /* Slide-out */
        s_anim_tick = 0;
        s_inner_state = 4;
        break;

    /* [ARCH-DIVERGENCE: DXPTYPE] Orig states 4-15 ran the full DirectPlay
     * host/client handshake (provider negotiation, session create/join,
     * player-slot assignment). Port's DXPTYPE wire format is incompatible
     * with TD5_d3d.exe peers (see file-footer manifest @ ~:10301), so the
     * handshake is unreachable end-to-end. All 12 states collapse to a
     * single transition into the lobby; s_network_active gates the lobby's
     * own behavior. */
    case 4: case 5: case 6: case 7: case 8: case 9:
    case 10: case 11: case 12: case 13: case 14: case 15:
        s_network_active = (s_return_screen == TD5_SCREEN_NETWORK_LOBBY);
        td5_frontend_set_screen((TD5_ScreenIndex)s_return_screen);
        break;

    default:
        td5_frontend_set_screen(TD5_SCREEN_NETWORK_LOBBY);
        break;
    }
}

void Screen_NetworkLobby(void) {
    switch (s_inner_state) {
    case 0: /* INITIALIZATION */
        frontend_init_return_screen(TD5_SCREEN_NETWORK_LOBBY);
        TD5_LOG_D(LOG_TAG, "NetworkLobby: state 0 - init");

#ifndef TD5RE_RELEASE
        /* Dev hook: TD5RE_NET_LOBBY=1 boots straight into a host lobby (e.g.
         * --StartScreen=11) so the lobby UI can be inspected without a 2nd PC. */
        if (!s_network_active && getenv("TD5RE_NET_LOBBY")) {
            const char *nl = getenv("TD5RE_NET_LOBBY");
            td5_net_init();
            td5_net_set_mode(TD5_NET_MODE_DIRECT);
            if (nl[0] == 'j' || nl[0] == 'J') {
                /* dev: join a loopback host (2-instance lockstep testing) */
                td5_net_join_direct("127.0.0.1", g_td5.ini.net_game_port,
                                    frontend_net_player_name());
            } else {
                td5_net_create_session_ex("DevLobby", frontend_net_player_name(), 6,
                                          g_td5.ini.net_game_port, 0);
            }
            s_network_active = 1;
        }
#endif

        /* S31: (re)announce this machine's car pick to the host -- runs on
         * every lobby entry, including the return from CHANGE CAR. */
        td5_net_set_local_car(s_selected_car, s_selected_paint);

        /* Kick check: if kicked flag set, destroy session, go to SessionLocked */
        if (s_kicked_flag) {
            s_kicked_flag = 0;   /* consume -- only frontend boot cleared it before */
            s_race_active_flag = 0;
            s_network_active = 0;
            frontend_net_destroy();
            td5_frontend_set_screen(TD5_SCREEN_SESSION_LOCKED);
            return;
        }

        /* If session is sealed (re-entering from car select), unseal */
        if (s_network_active) {
            frontend_net_seal(0);
        }

        frontend_play_sfx(5);
        frontend_load_tga("Front_End/MainMenu.tga", "Front_End/FrontEnd.zip");

        /* Create UI elements at their faithful resting positions. The original
         * RunFrontendNetworkLobby @0x0041C330 slides these in (case 1) and they
         * rest at counter==0x14 in a TWO-COLUMN layout (halfW=320, halfH=240,
         * iVar9=halfH-0x9f=81): a tall MESSAGE WINDOW + STATUS panel on the left,
         * the CHANGE CAR/START/EXIT action buttons in the right column at x=360.
         * [verified via decomp 2026-06-02] */
        /* S10b: clean net-lobby layout. The ported layout had an empty chat
         * input strip + a "MESSAGE WINDOW" panel that overlapped the roster and
         * the action buttons. Replaced with a left roster (drawn by the overlay,
         * no navigable panel buttons) + a right column of action buttons. Fixed
         * indices: 0=START 1=CHANGE CAR 2=SELECT TRACK 3=EXIT 4=OPTIONS(host).
         * [2026-06-07] Added SELECT TRACK -> the track picker (returns to the
         * lobby via flow_context==4); rows below it shift down one slot (0x28). */
        frontend_create_button(SNK_StartButTxt,     400, 110, 190, 0x28); /* 0 START */
        frontend_create_button(SNK_ChangeCarButTxt, 400, 158, 190, 0x28); /* 1 CHANGE CAR */
        frontend_create_button("SELECT TRACK",      400, 206, 190, 0x28); /* 2 SELECT TRACK */
        frontend_create_button(SNK_ExitButTxt,      400, 254, 190, 0x28); /* 3 EXIT */
        if (frontend_net_is_host())
            frontend_create_button("OPTIONS",       400, 302, 190, 0x28); /* 4 (host) */

        memset(s_chat_input_buffer, 0, sizeof(s_chat_input_buffer));
        s_lobby_modal = 0;

        s_chat_dirty = (s_network_active) ? 1 : 0;
        s_lobby_action = 0;
        s_anim_tick = 0;
        s_inner_state = 1;
        break;

    case 1: /* ANIMATE IN (~600ms) */
        /* Animate buttons sliding into position */
        if (frontend_update_timed_animation(0x14, 333) >= 1.0f) {
            frontend_play_sfx(4);
            s_anim_complete = 1;
            s_inner_state = 2;
        }
        break;

    case 2: /* TRANSITION COMPLETE / ENABLE INPUT */
        /* Set up text input: buffer ptr, max 60 chars, enable */
        s_text_input_state = 1;
        s_inner_state = 3;
        break;

    case 3: /* MAIN INTERACTIVE LOBBY */
        /* Render background and chat input */
        frontend_present_buffer();

#ifndef TD5RE_RELEASE
        /* Dev hook: TD5RE_NET_LOBBY=2 auto-opens the OPTIONS modal once. */
        {
            static int s_dev_modal_once = 0;
            const char *lv = getenv("TD5RE_NET_LOBBY");
            if (lv && lv[0] == '2' && !s_dev_modal_once && !s_lobby_modal &&
                frontend_net_is_host()) {
                s_dev_modal_once = 1;
                s_lobby_kick_sel = -1;
                s_lobby_max_players = td5_net_get_max_players();
                if (s_lobby_max_players < 2 || s_lobby_max_players > 6) s_lobby_max_players = 6;
                s_lobby_password[0] = '\0';
                frontend_begin_text_input(s_lobby_password, (int)sizeof(s_lobby_password));
                frontend_set_text_input_prompt("PASSWORD (BLANK = OPEN)");
                s_lobby_modal_armed = 1;   /* dev auto-open: no Enter to wait on */
                s_lobby_modal = 1;
            }
        }
#endif

        /* S10b: host OPTIONS modal (max players + password) — takes input focus
         * while open. Password edits via the WM_CHAR text path; Left/Right adjust
         * max players; Enter applies + closes; Esc cancels. */
        if (s_lobby_modal) {
            /* Wait for the OPTIONS-Enter (which opened this modal) to be released
             * before accepting input — otherwise its WM_CHAR key-repeat '\r'
             * instantly confirms and closes the modal ("can't change options"). */
            if (!s_lobby_modal_armed) {
                td5_plat_input_flush_chars();
                if (!td5_plat_input_key_pressed(0x1C))   /* Enter scancode */
                    s_lobby_modal_armed = 1;
                break;
            }
            frontend_handle_text_input_key();
            if (frontend_text_input_confirmed()) {
                td5_net_set_session_limits(s_lobby_max_players, s_lobby_password);
                /* S31: apply the kick selection (if any). DXPDATA opcode 0x12,
                 * payload[1] = target slot; only the target reacts. */
                if (s_lobby_kick_sel >= 0 &&
                    td5_net_is_slot_active(s_lobby_kick_sel) &&
                    s_lobby_kick_sel != td5_net_local_slot()) {
                    uint8_t kick_msg[8] = {0x12, 0, 0, 0, 0, 0, 0, 0};
                    kick_msg[1] = (uint8_t)s_lobby_kick_sel;
                    frontend_net_send(1, kick_msg, 8);
                    TD5_LOG_I(LOG_TAG, "Lobby options: kick sent for slot %d",
                              s_lobby_kick_sel);
                }
                s_lobby_kick_sel = -1;
                TD5_LOG_I(LOG_TAG, "Lobby options: max=%d password=%s",
                          s_lobby_max_players, s_lobby_password[0] ? "set" : "none");
                s_lobby_modal = 0;
                s_text_input_state = 1;             /* restore chat input */
                frontend_play_sfx(3);
            } else if (s_arrow_input & 1) {          /* LEFT (robust poll edge) */
                if (s_lobby_max_players > 2) s_lobby_max_players--;
                frontend_play_sfx(2);
            } else if (s_arrow_input & 2) {          /* RIGHT */
                if (s_lobby_max_players < 6) s_lobby_max_players++;
                frontend_play_sfx(2);
            } else if (s_arrow_input & 4) {          /* UP: previous kick target */
                s_lobby_kick_sel = lobby_cycle_kick(s_lobby_kick_sel, -1);
                frontend_play_sfx(2);
            } else if (s_arrow_input & 8) {          /* DOWN: next kick target */
                s_lobby_kick_sel = lobby_cycle_kick(s_lobby_kick_sel, +1);
                frontend_play_sfx(2);
            } else if (td5_plat_input_key_pressed(0x01)) {   /* ESC = cancel */
                s_lobby_modal = 0;
                s_lobby_kick_sel = -1;
                s_text_input_state = 1;
            }
            break;
        }

        /* ESC backs out of the lobby = the EXIT action (tear the session down so
         * no dangling host/session leaks). Mirrors button index 3. */
        if (frontend_check_escape()) {
            TD5_LOG_I(LOG_TAG, "NetworkLobby: ESC -> exit (destroy session)");
            frontend_net_destroy();
            s_network_active = 0;
            s_lobby_modal = 0;
            td5_frontend_set_screen(TD5_SCREEN_CONNECTION_BROWSER);
            return;
        }

        /* S10: keep the participant table mirrored to the live roster so the
         * host's ready check + the status panel reflect who has actually
         * joined (slots populated by the JOIN handshake / DXPROSTER). */
        {
            int slot;
            for (slot = 0; slot < 6; slot++)
                s_participant_flags[slot] = td5_net_is_slot_active(slot) ? 1 : 0;
        }

        /* S31: drain queued net messages. The only lobby-relevant opcode is
         * LOBBY_KICK (DXPDATA payload[0]=0x12, payload[1]=target slot). */
        {
            uint8_t nbuf[64];
            int ntype;
            memset(nbuf, 0, sizeof(nbuf));
            while ((ntype = frontend_net_receive(nbuf, sizeof(nbuf))) >= 0) {
                if (ntype == (int)TD5_DXPDATA && nbuf[0] == 0x12 &&
                    !frontend_net_is_host() &&
                    (int)nbuf[1] == td5_net_local_slot()) {
                    TD5_LOG_I(LOG_TAG, "NetworkLobby: kicked by host");
                    s_kicked_flag = 1;
                }
                memset(nbuf, 0, sizeof(nbuf));
            }
        }
        if (s_kicked_flag) {
            s_kicked_flag = 0;
            s_race_active_flag = 0;
            s_network_active = 0;
            frontend_net_destroy();
            td5_frontend_set_screen(TD5_SCREEN_SESSION_LOCKED);
            return;
        }

#ifndef TD5RE_RELEASE
        /* Dev hook: TD5RE_NET_AUTOSTART=1 -- the host fires START automatically
         * once a client has joined (headless 2-instance lockstep testing). */
        {
            static int s_dev_autostart_done = 0;
            if (!s_dev_autostart_done && getenv("TD5RE_NET_AUTOSTART") &&
                frontend_net_is_host() && td5_net_get_player_count() >= 2) {
                s_dev_autostart_done = 1;
                TD5_LOG_I(LOG_TAG, "NetworkLobby: dev autostart (players=%d)",
                          td5_net_get_player_count());
                s_lobby_action = 2;
                s_inner_state = 5;
                return;
            }
        }
#endif

        /* S10: a client auto-launches into the race once the host's DXPSTART
         * rendezvous has activated lockstep sync (td5_net_is_active). The host
         * launches via state 5 -> 0x10 -> 0x11. */
        if ((s_lobby_action == 3) ||
            (!frontend_net_is_host() && td5_net_is_active())) {
            TD5_LOG_I(LOG_TAG, "NetworkLobby: client race start (sync active)");
            /* S31: adopt the host's race config before building the schedule. */
            {
                TD5_NetRaceConfig ncfg;
                if (td5_net_get_race_config(&ncfg)) {
                    s_selected_track  = ncfg.track_index;
                    s_track_direction = ncfg.reverse_direction;
                    TD5_LOG_I(LOG_TAG,
                              "NetworkLobby: adopted host config track=%d dir=%d seed=0x%08X",
                              ncfg.track_index, ncfg.reverse_direction, ncfg.rng_seed);
                }
            }
            s_launching_net_race = 1;
            s_race_active_flag = 1;
            frontend_init_race_schedule();
            frontend_init_display_mode_state();
            return;
        }

        /* Process button input
         * (indices: 0=START 1=CHANGE CAR 2=SELECT TRACK 3=EXIT 4=OPTIONS). */
        if (s_input_ready && s_button_index >= 0) {
            switch (s_button_index) {
            case 0: /* START */
                if (frontend_net_is_host()) {
                    if (td5_net_get_player_count() <= 1) {
                        /* Solo host: no peers to rendezvous with -> just play a
                         * single-player race (network_active stays 0, so the
                         * lockstep barrier isn't engaged and can't stall). */
                        TD5_LOG_I(LOG_TAG, "NetworkLobby: solo start (1 player)");
                        s_launching_net_race = 0;
                        s_race_active_flag = 1;
                        frontend_init_race_schedule();
                        frontend_init_display_mode_state();
                        return;
                    }
                    s_lobby_action = 2;
                    s_inner_state = 5; /* multi-player ready check -> DXPSTART */
                }
                /* Client: waits for the host's DXPSTART (handled in the sync poll). */
                break;

            case 1: /* CHANGE CAR */
                s_lobby_action = 1;
                td5_frontend_set_screen(TD5_SCREEN_CAR_SELECTION);
                return;

            case 2: /* SELECT TRACK -> the track picker. flow_context==4 makes the
                     * track screen's exit dispatch return here (not launch a race),
                     * so the host can set the track and come back to the lobby. */
                s_flow_context = 4;
                td5_frontend_set_screen(TD5_SCREEN_TRACK_SELECTION);
                return;

            case 3: /* EXIT -> tear down the session and leave the lobby */
                TD5_LOG_I(LOG_TAG, "NetworkLobby: exit -> destroy session");
                frontend_net_destroy();
                s_network_active = 0;
                s_lobby_modal = 0;
                td5_frontend_set_screen(TD5_SCREEN_CONNECTION_BROWSER);
                return;

            case 4: /* OPTIONS (host) -> open the max-players/password modal */
                if (frontend_net_is_host()) {
                    s_lobby_max_players = td5_net_get_max_players();
                    if (s_lobby_max_players < 2 || s_lobby_max_players > 6)
                        s_lobby_max_players = 6;
                    s_lobby_password[0] = '\0';
                    frontend_begin_text_input(s_lobby_password, (int)sizeof(s_lobby_password));
                    frontend_set_text_input_prompt("PASSWORD (BLANK = OPEN)");
                    s_lobby_kick_sel = -1;
                    s_lobby_modal_armed = 0;   /* arm after the Enter is released */
                    s_lobby_modal = 1;
                    frontend_play_sfx(3);
                }
                break;

            default:
                s_lobby_action = 0;
                break;
            }
        }

        /* Process network messages */
        /* Check for disconnect */
        if (s_kicked_flag) {
            frontend_net_destroy();
            td5_frontend_set_screen(TD5_SCREEN_SESSION_LOCKED);
            return;
        }

        /* Update lobby player list display */
        /* Poll network input */

        /* Check text input confirmed (Enter pressed) -> chat submit */
        if (frontend_text_input_confirmed()) {
            s_inner_state = 4;
        }
        break;

    case 4: /* CHAT TEXT SUBMISSION */
        /* Process chat input buffer: check admin commands, emoticons */
        /* If valid, send as DXPCHAT (type 2) */
        if (s_chat_input_buffer[0] != '\0') {
            frontend_net_send(2, s_chat_input_buffer, (int)strlen(s_chat_input_buffer) + 1);
        }
        memset(s_chat_input_buffer, 0, sizeof(s_chat_input_buffer));
        s_inner_state = 2; /* re-enable text input */
        break;

    case 5: /* PLAYER READY CHECK (host only) */
    {
        int slot, active_count = 0, ready_count = 0;
        /* Write local status */
        s_per_slot_status[frontend_net_local_slot()] = s_lobby_action;

        for (slot = 0; slot < 6; slot++) {
            if (s_participant_flags[slot] != 0) {
                active_count++;
                if (s_per_slot_status[slot] == 2) {
                    ready_count++;
                }
            }
        }

        (void)ready_count;
        if (active_count >= 2) {
            /* S10: enough players have joined -> begin the DXPSTART rendezvous.
             * The per-slot config/settings exchange (states 0xC-0xF) is bypassed
             * for the lockstep path -- only input bitmasks + dt are synced at
             * race time, so no pre-race car/settings replication is required. */
            s_inner_state = 0x10;
        } else {
            /* Not enough players to start a network race. */
            s_dialog_mode = 1;
            frontend_play_sfx(5);
            s_inner_state = 6;
        }
    }
        break;

    case 6: /* ERROR/CONFIRMATION ANIMATE IN (24 frames) */
        s_anim_tick = 0;
        /* Create overlay surface with error message:
         * dialog_mode < 2: SNK_NetErrString1/2
         * dialog_mode >= 2: SNK_NetErrString3/4 */
        s_inner_state = 7;
        break;

    case 7: /* SHOW DIALOG BUTTONS */
        /* Create Yes/No or OK button based on dialog_mode */
        if (s_dialog_mode == 0 || s_dialog_mode == 2) {
            frontend_create_button(SNK_YesButTxt, -80, 0, 80, 0x20);
            frontend_create_button(SNK_NoxButTxt,  -80, 0, 80, 0x20);
        } else {
            frontend_create_button(SNK_OkButTxt, -80, 0, 80, 0x20);
        }
        s_inner_state = 8;
        break;

    case 8: /* DIALOG INPUT HANDLING */
        /* Continue processing network messages */
        if (s_kicked_flag) {
            frontend_net_destroy();
            td5_frontend_set_screen(TD5_SCREEN_SESSION_LOCKED);
            return;
        }

        if (s_input_ready) {
            if (s_dialog_mode == 1) {
                /* Info/OK: any press advances */
                s_inner_state = 9;
            } else if (s_button_index == 0) {
                /* Yes: confirm exit */
                s_dialog_mode = 0;
                s_inner_state = 9;
            } else {
                /* No: cancel */
                s_dialog_mode = 1;
                s_inner_state = 9;
            }
        }
        break;

    case 9: /* DIALOG RESOLUTION */
        if (s_dialog_mode == 0) {
            /* Confirmed exit -> start pre-race or disconnect */
            s_inner_state = 0x0C;
        } else if (s_dialog_mode == 2) {
            /* Forced disconnect */
            frontend_net_destroy();
            td5_frontend_set_screen(TD5_SCREEN_MAIN_MENU);
            return;
        } else {
            /* Cancelled -> animate out, return to lobby */
            frontend_play_sfx(5);
            s_inner_state = 10;
        }
        break;

    case 10: /* ERROR DIALOG ANIMATE OUT (24 frames) */
        s_anim_tick = 0;
        s_inner_state = 11;
        break;

    case 11:
        s_anim_tick += 2 * s_fe_logic_ticks;
        if (s_anim_tick >= 0x0C) { /* 12 frames */
            s_lobby_action = 0;
            s_inner_state = 2; /* return to enabled input */
        }
        break;

    case 0x0C: /* SEAL SESSION & COLLECT CONFIGS (host only) */
        TD5_LOG_I(LOG_TAG, "NetworkLobby: seal session, collect configs");
        frontend_net_seal(1);

        /* Build participant table, kick non-ready players */
        {
            int slot;
            for (slot = 0; slot < 6; slot++) {
                s_config_received[slot] = 0;
                if (s_participant_flags[slot] && s_per_slot_status[slot] != 2) {
                    /* Kick non-ready: send LOBBY_KICK (opcode 0x12) */
                    uint8_t kick_msg[8] = {0x12, 0, 0, 0, 0, 0, 0, 0};
                    kick_msg[4] = (uint8_t)slot;
                    frontend_net_send(1, kick_msg, 8);
                    s_participant_flags[slot] = 0;
                }
            }
        }

        /* Store host's own config, mark received */
        s_config_received[frontend_net_local_slot()] = 1;
        s_last_poll_timestamp = td5_plat_time_ms();
        s_inner_state = 0x0D;
        break;

    case 0x0D: /* POLL CLIENT CONFIGS (250ms interval) */
    {
        uint32_t now = td5_plat_time_ms();
        /* Process network (receive config replies) */

        if ((now - s_last_poll_timestamp) > 250) {
            s_last_poll_timestamp = now;
            int all_done = 1;
            int slot;
            for (slot = 0; slot < 6; slot++) {
                if (s_participant_flags[slot] && !s_config_received[slot]) {
                    /* Send LOBBY_REQUEST_CONFIG (opcode 0x13) */
                    uint8_t req_msg[4] = {0x13, 0, 0, 0};
                    req_msg[1] = (uint8_t)slot;
                    frontend_net_send(1, req_msg, 4);
                    all_done = 0;
                    break; /* one per tick */
                }
            }
            if (all_done) {
                s_inner_state = 0x0E;
            }
        }
    }
        break;

    case 0x0E: /* INITIALIZE RACE SCHEDULE (host only) */
        TD5_LOG_I(LOG_TAG, "NetworkLobby: init race schedule");
        {
            int slot;
            for (slot = 0; slot < 6; slot++) s_config_received[slot] = 0;
            s_config_received[frontend_net_local_slot()] = 1;
        }
        s_race_active_flag = 1;
        /* Fill empty slots with AI-controlled cars */
        s_last_poll_timestamp = td5_plat_time_ms();
        s_inner_state = 0x0F;
        break;

    case 0x0F: /* BROADCAST SETTINGS TO CLIENTS (165ms interval) */
    {
        uint32_t now = td5_plat_time_ms();
        s_anim_tick = 0;

        if ((now - s_last_poll_timestamp) > 165) {
            s_last_poll_timestamp = now;
            int all_acked = 1;
            int slot;
            for (slot = 0; slot < 6; slot++) {
                if (s_participant_flags[slot] && !s_config_received[slot]) {
                    /* Send LOBBY_SETTINGS (opcode 0x15, 0x80 bytes) */
                    uint8_t settings_msg[0x80];
                    memset(settings_msg, 0, sizeof(settings_msg));
                    settings_msg[0] = 0x15;
                    frontend_net_send(1, settings_msg, 0x80);
                    all_acked = 0;
                    break;
                }
            }
            if (all_acked) {
                s_inner_state = 0x10;
            }
        }
    }
        break;

    case 0x10: /* LAUNCH COUNTDOWN (8 ticks, then send DXPSTART) */
        s_anim_tick += 2 * s_fe_logic_ticks;
        if (s_anim_tick >= 8) {
            s_race_active_flag = 1;
            /* S31: build the authoritative race config and broadcast it in the
             * DXPSTART payload -- seed, track, direction, per-slot cars. The
             * clients adopt it before running their race schedule; the host's
             * own schedule reads back the archived copy, so every machine
             * builds the identical grid from the identical RNG stream. */
            {
                TD5_NetRaceConfig cfg;
                int slot, c, p;
                memset(&cfg, 0, sizeof(cfg));
                cfg.rng_seed          = (uint32_t)td5_plat_time_ms() ^
                                        ((uint32_t)rand() << 16);
                cfg.track_index       = s_selected_track;
                cfg.reverse_direction = s_track_direction;
                cfg.lap_count         = 4;   /* net races force 4-lap drag mode */
                for (slot = 0; slot < 6; slot++) {
                    cfg.car_index[slot]   = 0;
                    cfg.paint_index[slot] = 0;
                    if (td5_net_get_slot_car(slot, &c, &p) && c >= 0) {
                        cfg.car_index[slot]   = c;
                        cfg.paint_index[slot] = p;
                    }
                }
                frontend_net_send(4, &cfg, (int)sizeof(cfg));
                TD5_LOG_I(LOG_TAG,
                          "NetworkLobby: DXPSTART config seed=0x%08X track=%d dir=%d",
                          cfg.rng_seed, cfg.track_index, cfg.reverse_direction);
            }
            s_inner_state = 0x11;
        }
        break;

    case 0x11: /* WAIT FOR START CONFIRMATION */
    {
        /* The DXPSTART rendezvous (handle_start / ack_reply / start_confirm in
         * td5_net.c, driven by the worker thread) activates lockstep sync on all
         * machines. Drain the ring and launch once sync is active. */
        uint8_t recv_buf[256];
        (void)frontend_net_receive(recv_buf, sizeof(recv_buf));
        if (td5_net_is_active()) {
            TD5_LOG_I(LOG_TAG, "NetworkLobby: host race start (sync active)");
            s_launching_net_race = 1;
            s_race_active_flag = 1;
            frontend_init_race_schedule();
            frontend_init_display_mode_state();
            return;
        }
    }
        break;
    }
}

void Screen_SessionLocked(void) {
    switch (s_inner_state) {
    case 0: /* Init */
        frontend_init_return_screen(TD5_SCREEN_SESSION_LOCKED);
        TD5_LOG_D(LOG_TAG, "SessionLocked: init");
        frontend_load_tga("Front_End/MainMenu.tga", "Front_End/FrontEnd.zip");
        /* Dialog 0x198x0x70 (408x112) rendered live in
         * frontend_render_session_locked_overlay (called from render_ui_rects).
         * Original: identical structure to CupFailed — CreateTrackedFrontendSurface,
         * then DrawFrontendLocalizedStringToSurface x2 for:
         *   SNK_SorryTxt     y=0x00 ("SORRY")          [CONFIRMED Language.dll]
         *   SNK_SeshLockedTxt y=0x38 ("SESSION LOCKED") [CONFIRMED Language.dll]
         * [CONFIRMED @ ScreenSessionLockedDialog 0x0041D630] */
        /* [FIXED 2026-06-02, decomp @0x41D630] OK rests at (296,289) — slides via
         * MoveFrontendSpriteRect(0,(halfW-0x318)+0x20*0x18, halfH+0x31) = (296, 289), same as CupFailed. */
        frontend_create_button(SNK_OkButTxt, 296, 289, 0x60, 0x20);
        s_anim_tick = 0;
        s_inner_state = 1;
        break;

    case 1: case 2: case 3: /* Present (3 frames) */
        frontend_present_buffer();
        s_inner_state++;
        break;

    case 4: /* Slide-in: 32 frames [CONFIRMED @ 0x41D630 case 4: anim==0x20 exit] */
        s_anim_tick += s_fe_logic_ticks;
        if (s_anim_tick >= 0x20) {
            s_inner_state = 5;
        }
        break;

    case 5: /* Wait for confirm -> main menu */
        if (s_input_ready) {
            td5_frontend_set_screen(TD5_SCREEN_MAIN_MENU);
        }
        break;
    }
}
