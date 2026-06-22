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

int  s_cs_edit;                         /* S31 host-setup: 1=name, 2=password, 3=port editing */
static int  s_cs_direct;                /* S31 host-setup: 1 = Direct-IP host, 0 = LAN */
char s_cs_port_buf[8];                  /* [ITEM 4] GAME PORT text-entry buffer (Direct host); read by the overlay */
static int  s_cs_esc_guard;             /* swallow the ESC that cancelled an edit */
static int  s_kick_button_slot[5];      /* lobby kick button k -> net slot */

static int  s_net_join_pending_ui;      /* awaiting JOIN_ACK before entering lobby */

static uint32_t s_net_join_wait_start;  /* tick when the join wait began (timeout) */

/* [ITEM 5 2026-06-16] On-screen "why the join failed" message. Built from the
 * net status text + the last JOIN_NAK reason so a Direct-IP / LAN join failure
 * tells the user host-unreachable vs session-full vs wrong-password instead of
 * silently bouncing back to the chooser. Shown on the join-failed sub-screen,
 * then OK/ESC returns to the chooser. */
static char s_net_join_fail_msg[160];

/* Compose the failure line. nak: 0=timeout/no-response, 1=session full,
 * 2=wrong/missing password. Keep it short -- it rides a single button label. */
static void frontend_net_build_join_fail(int nak) {
    const char *why;
    switch (nak) {
    case 1:  why = "SESSION FULL"; break;
    case 2:  why = "WRONG PASSWORD"; break;
    default: why = "HOST UNREACHABLE (NO RESPONSE)"; break;
    }
    snprintf(s_net_join_fail_msg, sizeof(s_net_join_fail_msg),
             "COULD NOT CONNECT: %s", why);
    TD5_LOG_W(LOG_TAG, "join failed: %s", s_net_join_fail_msg);
}

static int  s_net_cfg_enable_upnp = 1;       /* [Network] EnableUPnP (Direct host) */

static int  s_dialog_mode;              /* DAT_00496350 */

static int  s_per_slot_status[6];       /* DAT_00496980[6] */

static int  s_config_received[6];       /* DAT_00497262[6] */

static int  s_participant_flags[6];     /* DAT_0049725c[6] */

static int  s_race_active_flag;         /* DAT_00497324 */


static uint32_t s_last_poll_timestamp;  /* DAT_004968a8 */

/* [S31] Per-slot READY latch (clients toggle via the lobby READY button,
 * broadcast as DXPDATA opcode 0x18 {op, slot, state}); reset on every lobby
 * entry. The roster overlay draws the green READY tag from this. */
uint8_t     s_slot_ready[6];
static int  s_my_ready;
static int  s_car_announce_done;  /* [S31] set once CAR_INFO went out with a
                                   * valid slot (JOIN_ACK can land after the
                                   * first lobby entry's announce attempt) */

char s_create_session_name[64];

/* ========================================================================
 * [BUG #10 2026-06] BACK TO LOBBY keeps the previously-selected CONTROLLERS.
 *
 * The press-to-join lobby records each player's controller in the shared
 * s_mp_join_device[]/s_mp_joined_count working set (owned by td5_frontend.c).
 * But Screen_MultiplayerLobby's case-0 init unconditionally wipes that set on
 * EVERY entry — including the round-trip BACK TO LOBBY from the car-select grid
 * (mp_simul_back_to_lobby) or a finished race (td5_frontend_return_to_lobby
 * with s_mp_flow set). So returning to the lobby dropped every player's pad and
 * forced everyone to re-press their controller to re-join.
 *
 * Fix: a process-lifetime snapshot of the device map, mirroring the existing
 * MpSession roster store (which already survives Main-Menu cleanup but does NOT
 * carry the device assignment). The lobby's START handler snapshots the map as
 * the players leave for car-select; case-0 RESTORES it instead of wiping when
 * we are RETURNING within the same MP flow (s_mp_flow still set — only the
 * Main-Menu cleanup / td5_frontend_init clear it, i.e. a genuinely fresh lobby
 * entry). Frontend-only, never routed through the net config structs.
 *
 * Gated by TD5RE_LOBBY_KEEP_PADS (default ON; exactly "0" restores the old
 * wipe-every-time behaviour). Env read cached + logged once.
 * ======================================================================== */
static int  s_lobby_pads_valid;                       /* 1 once START snapshotted a map */
static int  s_lobby_pads_count;                       /* snapshotted joined-player count */
static int  s_lobby_pads_device[TD5_MAX_HUMAN_PLAYERS];/* snapshotted per-player device idx */

static int lobby_keep_pads_enabled(void) {
    static int s_cached = -1;   /* -1=unread, 0=legacy wipe, 1=keep */
    if (s_cached < 0) {
        const char *e = getenv("TD5RE_LOBBY_KEEP_PADS");
        s_cached = (e && e[0] == '0' && e[1] == '\0') ? 0 : 1;   /* default ON */
        TD5_LOG_I(LOG_TAG, "lobby keep pads: %s",
                  s_cached ? "on (restore controllers on back-to-lobby)"
                           : "off (legacy re-pick every entry)");
    }
    return s_cached;
}

/* Snapshot the live join map into the process-lifetime store (called as the
 * players leave the lobby with a confirmed roster). */
static void lobby_pads_save(void) {
    int p;
    if (!lobby_keep_pads_enabled()) return;
    s_lobby_pads_count = s_mp_joined_count;
    if (s_lobby_pads_count < 0) s_lobby_pads_count = 0;
    if (s_lobby_pads_count > TD5_MAX_HUMAN_PLAYERS)
        s_lobby_pads_count = TD5_MAX_HUMAN_PLAYERS;
    for (p = 0; p < TD5_MAX_HUMAN_PLAYERS; p++)
        s_lobby_pads_device[p] = s_mp_join_device[p];
    s_lobby_pads_valid = 1;
}

/* Restore the snapshotted join map into the live working set on a back-to-lobby
 * round-trip. Returns 1 if it restored a non-empty roster (so case-0 can skip
 * the wipe), 0 otherwise (fresh entry / feature off / nothing stored). */
static int lobby_pads_restore(void) {
    int p;
    if (!lobby_keep_pads_enabled()) return 0;
    if (!s_lobby_pads_valid || s_lobby_pads_count <= 0) return 0;
    /* Only on a genuine return within the same MP flow. A fresh lobby entry from
     * the main menu has had s_mp_flow cleared by the Main-Menu cleanup, so the
     * stale snapshot must NOT leak back in. */
    if (!s_mp_flow) return 0;
    s_mp_joined_count = s_lobby_pads_count;
    for (p = 0; p < TD5_MAX_HUMAN_PLAYERS; p++)
        s_mp_join_device[p] = s_lobby_pads_device[p];
    TD5_LOG_I(LOG_TAG, "MP Lobby: restored %d controller(s) on back-to-lobby",
              s_mp_joined_count);
    return 1;
}

/* [ITEM 3 2026-06-16] TD5RE_LOBBY_HOST_WATCHDOG (default ON): a client whose
 * host has gone (clean DXPDISCONNECT *or* an ungraceful drop detected by the
 * 1 Hz keepalive going silent) exits the dead lobby straight to the MAIN MENU.
 * "0" restores the legacy behaviour (connection-lost only -> connection
 * browser; no silence watchdog). */
static int lobby_host_watchdog_enabled(void) {
    static int s_cached = -1;   /* -1=unread, 0=legacy, 1=watchdog+menu */
    if (s_cached < 0) {
        const char *e = getenv("TD5RE_LOBBY_HOST_WATCHDOG");
        s_cached = (e && e[0] == '0' && e[1] == '\0') ? 0 : 1;   /* default ON */
        TD5_LOG_I(LOG_TAG, "lobby host watchdog: %s",
                  s_cached ? "on (detect host-gone -> main menu)"
                           : "off (legacy connection-lost -> browser)");
    }
    return s_cached;
}

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
static int frontend_net_receive(void *buf, int max_size) {
    TD5_NetMsgType type;
    void *data = NULL;
    int size = 0;

    if (!td5_net_receive(&type, &data, &size))
        return -1;

    /* Copy payload into caller's buffer, ALWAYS leaving a NUL terminator: the
     * wire payload is not guaranteed terminated, so a consumer that treats it
     * as a C string (e.g. chat text) could otherwise over-read past it. Binary
     * opcode consumers read small fixed offsets, well inside max_size-1. */
    if (buf && max_size > 0) {
        int copy_size = 0;
        if (data && size > 0) {
            copy_size = (size < max_size - 1) ? size : max_size - 1;
            memcpy(buf, data, (size_t)copy_size);
        }
        ((char *)buf)[copy_size] = '\0';
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

/**
 * [BUG #4 2026-06] Vertical offset (UI/layout space) to centre a per-player
 * accent SWATCH on the player-name FONT cap-band in the MP/net lobby roster.
 *
 * The roster render (frontend_render_mp_lobby_overlay in td5_frontend.c — NOT
 * editable from here) draws the swatch quad and the name text at the SAME row
 * Y. But fe_draw_text anchors text at the glyph CELL TOP, and the MontBlanc
 * main face caps occupy cell rows ~8..23 of the 24px cell, so the cap-band
 * CENTRE sits at  y + 15.5*text_scale  (same 15.5f anchor td5_hud.c uses for
 * its name plates). A swatch drawn at the bare row Y therefore reads too high.
 * Returning  15.5*text_scale - swatch_h*0.5  shifts a swatch of height
 * swatch_h so its centre lands on that cap-band centre.
 *
 * WIRING (the parent edits td5_frontend.c): in frontend_render_mp_lobby_overlay
 * add this offset to the swatch quad's row_y BEFORE it is scaled by sy, i.e.
 *     row_y += frontend_lobby_swatch_y_offset(0.8f, 14.0f);
 * (0.8f = the lobby name text_scale, 14.0f = the swatch height in layout space;
 * the quad is then drawn at row_y * sy as today).
 *
 * Gated by TD5RE_LOBBY_ALIGN (default ON); "0" returns 0.0f for legacy
 * (swatch flush with the cell top). Env read is cached + logged once.
 */
float frontend_lobby_swatch_y_offset(float text_scale, float swatch_h) {
    static int s_lobby_align = -1;   /* -1=unread, 0=legacy, 1=aligned */
    if (s_lobby_align < 0) {
        const char *e = getenv("TD5RE_LOBBY_ALIGN");
        s_lobby_align = (e && e[0] == '0') ? 0 : 1;   /* default ON */
        TD5_LOG_I(LOG_TAG, "lobby swatch align: %s",
                  s_lobby_align ? "on (centre swatch on text cap-band)"
                                : "off (legacy cell-top)");
    }
    if (!s_lobby_align)
        return 0.0f;
    return 15.5f * text_scale - swatch_h * 0.5f;
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
        /* [R9 2026-06-19] Clear the simultaneous-grid flag on entry so a post-race
         * return to THIS (local split-screen) lobby never inherits a stale
         * s_mp_simul that blanks the global "MULTIPLAYER" header — the local-lobby
         * partner to the title-suppression exemption + the NETWORK_LOBBY [#24]
         * fix. The MP flow re-asserts s_mp_simul on the next CarSelection entry. */
        s_mp_simul = 0;
        frontend_load_tga("Front_End/MainMenu.tga", "Front_End/FrontEnd.zip");
        /* [PERF 2026-06-06] Dropped a td5_input_enumerate_devices() here — it was a
         * ~120ms blocking IDirectInput8::EnumDevices on the lobby's entry frame.
         * The device list is already kept current by init + the WM_DEVICECHANGE
         * rescan, td5_plat_input_scan_join builds its scan handles lazily, and the
         * per-frame join scan below picks up any hot-plug within a frame or two. */
        /* [BUG #10] On a back-to-lobby round-trip (same MP flow) restore the
         * previously-bound controllers so nobody has to re-press; a fresh entry
         * from the main menu falls through to the legacy clean wipe. */
        if (!lobby_pads_restore()) {
            s_mp_joined_count = 0;
            memset(s_mp_join_device, 0, sizeof(s_mp_join_device));
        }
        s_mp_join_prev = td5_plat_input_scan_join();/* ignore inputs already held on entry */
        frontend_reset_buttons();
        frontend_create_button(SNK_StartRaceTxt, 220, 300, 200, 32);  /* 0 START */
        frontend_create_button(SNK_BackButTxt,   260, 360, 120, 32);  /* 1 BACK */
        /* Backing out of the pad-driven car grid lands here with the mouse
         * cursor hidden (the grid hides it) — restore it, this screen has
         * clickable START/BACK buttons. [cursor-fix 2026-06-12] */
        frontend_set_cursor_visible(1);
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
            /* [BUG #10] Snapshot the confirmed controller map so a later BACK TO
             * LOBBY (from car-select or the finished race) restores each player's
             * pad instead of forcing a re-pick. */
            lobby_pads_save();
            /* 2+ players pick simultaneously in a grid (each on their own pad);
             * a lone player just gets the normal single-player car select. */
            s_mp_simul          = (s_mp_joined_count >= 2);
            s_mp_phase          = 0;       /* start at the name + colour setup window */
            for (p = 0; p < s_mp_joined_count; p++) {
                /* [MP SESSION PERSISTENCE 2026-06] Re-entering the MP menu in the
                 * same process run restores each player's saved identity + car
                 * (name/accent/car/paint/color/trans) from the persistent store;
                 * otherwise seed defaults exactly as before. ready/pane_nav are
                 * per-session runtime state and always reset. Gated by
                 * TD5RE_MP_SESSION (when off / not yet valid this is byte-identical
                 * to the old wipe-every-time behaviour). */
                if (mp_session_restore_player_for_device(p)) {
                    /* [per-device 2026-06-21] restored the profile this device
                     * (s_mp_join_device[p]) used earlier this session. */
                } else {
                    s_mp_player_car[p]    = s_selected_car;
                    s_mp_player_paint[p]  = 0;
                    s_mp_player_color[p]  = -1;
                    s_mp_player_name[p][0] = '\0';
                    s_mp_player_accent[p] = (int)(k_mp_player_colors[p % TD5_MAX_HUMAN_PLAYERS] & 0x00FFFFFFu);
                }
                s_mp_player_ready[p]  = 0;
                s_mp_pane_nav_prev[p] = 0;
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

/* S10: split "host" or "host:port" into a host string + port (default game
 * port). [ITEM 6 2026-06-16] "host" is passed through UNCHANGED -- a dotted IP,
 * a LAN name, or a DDNS hostname all flow straight to td5_net_join_direct(),
 * whose transport (ws2_transport_join_direct) resolves non-numeric input via
 * getaddrinfo. We deliberately do NOT validate/reject non-numeric input here:
 * only the trailing ":port" is split off and parsed; the host text is copied
 * verbatim so a DDNS name like "myhost.ddns.net" survives intact. */
static void frontend_net_parse_ip_port(const char *in, char *ip_out, int ip_len, int *port_out) {
    const char *colon = strrchr(in, ':');   /* rightmost ':' = the port separator */
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
            if (s_button_index == 0) {            /* HOST -> setup screen */
                s_cs_direct = 0;
                td5_frontend_set_screen(TD5_SCREEN_CREATE_SESSION);
            }
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
            if (s_button_index == 0) {            /* HOST -> setup screen [S31] */
                s_cs_direct = 1;
                td5_frontend_set_screen(TD5_SCREEN_CREATE_SESSION);
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
        /* [ITEM 6] Hostnames + DDNS names are accepted, not just dotted IPs. */
        frontend_set_text_input_prompt("ENTER HOST IP / HOSTNAME [:PORT]");
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
                /* [ITEM 3 2026-06-16] td5_net_join_direct() returns false only
                 * when the address can't be resolved/parsed -- the old code
                 * bounced silently to the chooser, so a typo'd or bogus IP gave
                 * no feedback. Surface it like the timeout/full failures. */
                TD5_LOG_W(LOG_TAG, "Direct join '%s' failed (bad address)", s_net_direct_ip);
                snprintf(s_net_join_fail_msg, sizeof(s_net_join_fail_msg),
                         "COULD NOT CONNECT: INVALID ADDRESS");
                s_inner_state = 9;                /* failure message */
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
        } else if (td5_net_get_join_nak_reason() == 1 ||  /* session full */
                   td5_net_is_connection_lost() ||
                   (td5_plat_time_ms() - s_net_join_wait_start) > 8000) {
            /* [ITEM 5] Show WHY before bouncing out: full vs no-response. The
             * NAK reason (if any) is more specific than the timeout. */
            int nak = td5_net_get_join_nak_reason();
            frontend_net_build_join_fail(nak);
            s_net_join_pending_ui = 0;
            s_network_active = 0;
            frontend_net_destroy();
            s_inner_state = 9;                            /* failure message */
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
                /* [ITEM 3] same bad-address feedback on the password re-join. */
                snprintf(s_net_join_fail_msg, sizeof(s_net_join_fail_msg),
                         "COULD NOT CONNECT: INVALID ADDRESS");
                s_inner_state = 9;
            }
            break;
        }
        if (s_input_ready && s_button_index == 0) {   /* BACK -> chooser */
            td5_frontend_set_screen(TD5_SCREEN_DIRECT_CONNECT);
        }
        break;

    case 9: /* JOIN: connection FAILED -- show the reason (fresh buttons) [ITEM 5] */
        frontend_reset_buttons();
        frontend_load_tga("Front_End/MainMenu.tga", "Front_End/FrontEnd.zip");
        frontend_create_button(s_net_join_fail_msg, 80, 193, 480, 0x40); /* 0 message */
        frontend_create_button(SNK_OkButTxt, 232, 377, 160, 0x20);       /* 1 OK */
        s_inner_state = 10;
        break;

    case 10: /* JOIN: failure-message interaction -> back to chooser */
        frontend_present_buffer();
        if (s_input_ready) {     /* OK (or the message button) acknowledges */
            td5_frontend_set_screen(TD5_SCREEN_DIRECT_CONNECT);
        }
        break;
    }
}

/* [ITEM 1 2026-06-16] TD5RE_LAN_JOIN_WAIT (default ON): join a LAN session via
 * the wait-for-JOIN_ACK flow (password prompt + clear failure) instead of the
 * legacy straight-to-lobby jump. "0" restores the old behaviour. */
static int lan_join_wait_enabled(void) {
    static int s_cached = -1;   /* -1=unread, 0=legacy jump, 1=wait-for-ACK */
    if (s_cached < 0) {
        const char *e = getenv("TD5RE_LAN_JOIN_WAIT");
        s_cached = (e && e[0] == '0' && e[1] == '\0') ? 0 : 1;   /* default ON */
        TD5_LOG_I(LOG_TAG, "LAN join wait-for-ACK: %s",
                  s_cached ? "on (password prompt + clear failure)"
                           : "off (legacy straight-to-lobby)");
    }
    return s_cached;
}

/* [S31] SESSION_PICKER list: one row button per discovered LAN session
 * (indices 0..5) + BACK (6). Rows refresh every frame as discovery replies
 * arrive; ENTER on a row joins that session directly. */
static int s_picker_prev_count = -1;
static int s_picker_join_index = -1;   /* [ITEM 1] session row currently being joined */
static void frontend_net_update_session_list(void) {
    int count = td5_net_get_enum_session_count();
    int i;
    for (i = 0; i < 6 && i < FE_MAX_BUTTONS; i++) {
        if (!s_buttons[i].active) continue;
        if (i < count) {
            int cur = 0, max = 0;
            td5_net_get_enum_session_info(i, &cur, &max);
            snprintf(s_buttons[i].label, sizeof(s_buttons[i].label),
                     "%s  %d/%d", td5_net_get_enum_session_name(i), cur, max);
            s_buttons[i].hidden   = 0;
            s_buttons[i].disabled = 0;
        } else if (i == 0 && count <= 0) {
            snprintf(s_buttons[i].label, sizeof(s_buttons[i].label),
                     "(SEARCHING FOR LAN GAMES...)");
            s_buttons[i].hidden   = 0;
            s_buttons[i].disabled = 1;
        } else {
            s_buttons[i].label[0] = '\0';
            s_buttons[i].hidden   = 1;
            s_buttons[i].disabled = 1;
        }
    }
    /* First session appearing: pull the selection up onto the list (it
     * parked on BACK while every row was disabled). */
    if (s_picker_prev_count <= 0 && count > 0 && s_selected_button == 6)
        s_selected_button = 0;
    s_picker_prev_count = count;
}

void Screen_SessionPicker(void) {
    /* [PERF 2026-06-06] LAN discovery is non-blocking + incremental: poll it
     * every frame after init so the list fills in live as hosts answer.
     * [ITEM 1 2026-06-16] Only during the list build / browse states (1..3) --
     * the join-wait / password / failure sub-states (5..11) rebuild their own
     * button layouts, so don't relabel them as session rows here. */
    if (s_inner_state >= 1 && s_inner_state <= 3) {
        td5_net_enumerate_sessions();
        frontend_net_update_session_list();
    }
    switch (s_inner_state) {
    case 0: /* Init: kick off LAN discovery + build the session list */
    {
        int i;
        frontend_init_return_screen(TD5_SCREEN_SESSION_PICKER);
        TD5_LOG_D(LOG_TAG, "SessionPicker: init (LAN discovery)");
        /* [ITEM 3] Re-arm the net stack on every entry. A prior join attempt
         * may have torn it down (frontend_net_destroy -> td5_net_shutdown sets
         * s_initialized=0); td5_net_init is idempotent, so this revives a clean
         * stack on re-entry without disturbing a still-live one. */
        td5_net_init();
        td5_net_set_mode(TD5_NET_MODE_LAN);
        td5_net_enumerate_sessions();             /* start a discovery window (non-blocking) */
        frontend_load_tga("Front_End/MainMenu.tga", "Front_End/FrontEnd.zip");
        for (i = 0; i < 6; i++) {                 /* 0..5: session rows */
            int bi = frontend_create_button("", 120, 150 + i * 38, 420, 0x20);
            if (bi >= 0 && bi < FE_MAX_BUTTONS) {
                s_buttons[bi].hidden   = 1;
                s_buttons[bi].disabled = 1;
            }
        }
        frontend_create_button(SNK_BackButTxt, 120, 377, 112, 0x20);   /* 6 */
        s_picker_prev_count = -1;
        frontend_net_update_session_list();
        s_anim_tick = 0;
        s_inner_state = 1;
        break;
    }

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
        if (s_input_ready && s_button_index >= 0) {
            if (s_button_index <= 5) {        /* session row -> join it */
                if (s_button_index < td5_net_get_enum_session_count() &&
                    td5_net_join_session(s_button_index, frontend_net_player_name())) {
                    s_picker_join_index = s_button_index;   /* [ITEM 1] for password re-join */
                    /* [ITEM 1 2026-06-16] Don't jump straight to the lobby --
                     * WAIT for the host's JOIN_ACK, exactly like Direct-IP join
                     * (Screen_DirectConnect case 5). This makes a passworded LAN
                     * session prompt for the password instead of hanging at
                     * "connecting", and surfaces full/no-response failures.
                     * Gated by TD5RE_LAN_JOIN_WAIT (default ON; "0" restores the
                     * legacy straight-to-lobby jump). */
                    s_network_active = 1;
                    if (lan_join_wait_enabled()) {
                        s_net_join_pending_ui = 1;
                        s_net_join_wait_start = td5_plat_time_ms();
                        s_inner_state = 7;            /* wait for JOIN_ACK */
                    } else {
                        s_return_screen = TD5_SCREEN_NETWORK_LOBBY;
                        s_inner_state = 5;
                    }
                } else {
                    TD5_LOG_W(LOG_TAG, "LAN join %d failed", s_button_index);
                    frontend_play_sfx(10);
                }
            } else if (s_button_index == 6) { /* Back -> LAN menu */
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

    case 7: /* [ITEM 1] JOIN: wait for the host's JOIN_ACK (mirrors DirectConnect 5) */
        frontend_present_buffer();
        if (frontend_check_escape()) {        /* cancel the join attempt */
            s_net_join_pending_ui = 0;
            s_network_active = 0;
            frontend_net_destroy();
            td5_frontend_set_screen(TD5_SCREEN_SESSION_PICKER);
            return;
        }
        if (td5_net_local_slot() >= 0) {
            s_net_join_pending_ui = 0;
            s_network_active = 1;
            td5_frontend_set_screen(TD5_SCREEN_NETWORK_LOBBY);
            return;
        } else if (td5_net_get_join_nak_reason() == 2) {  /* host needs a password */
            s_inner_state = 8;                            /* prompt + retry */
        } else if (td5_net_get_join_nak_reason() == 1 ||  /* session full */
                   td5_net_is_connection_lost() ||
                   (td5_plat_time_ms() - s_net_join_wait_start) > 8000) {
            int nak = td5_net_get_join_nak_reason();
            frontend_net_build_join_fail(nak);
            s_net_join_pending_ui = 0;
            s_network_active = 0;
            frontend_net_destroy();
            s_inner_state = 10;                           /* failure message */
        }
        break;

    case 8: /* [ITEM 1] JOIN: host rejected for a wrong/missing password -> prompt */
        frontend_reset_buttons();
        frontend_load_tga("Front_End/MainMenu.tga", "Front_End/FrontEnd.zip");
        frontend_create_button(SNK_BackButTxt, 278, 289, 112, 0x20);
        s_lobby_password[0] = '\0';
        frontend_begin_text_input(s_lobby_password, (int)sizeof(s_lobby_password));
        frontend_set_text_input_prompt("PASSWORD REQUIRED");
        s_inner_state = 9;
        break;

    case 9: /* [ITEM 1] JOIN: password entry -> re-join with the password */
        frontend_handle_text_input_key();
        if (frontend_text_input_confirmed()) {
            td5_net_set_join_password(s_lobby_password);
            /* Re-join the SAME session row. The selection that opened this flow
             * is still the current row; re-resolve it from the live list. */
            if (s_picker_join_index >= 0 &&
                s_picker_join_index < td5_net_get_enum_session_count() &&
                td5_net_join_session(s_picker_join_index, frontend_net_player_name())) {
                s_net_join_wait_start = td5_plat_time_ms();
                s_inner_state = 7;
            } else {
                td5_frontend_set_screen(TD5_SCREEN_SESSION_PICKER);
            }
            return;
        }
        if (s_input_ready && s_button_index == 0) {   /* BACK -> picker */
            /* Drop the half-open join so the re-entered picker starts clean. */
            s_net_join_pending_ui = 0;
            s_network_active = 0;
            frontend_net_destroy();
            td5_frontend_set_screen(TD5_SCREEN_SESSION_PICKER);
            return;
        }
        break;

    case 10: /* [ITEM 1/5] JOIN: connection FAILED -- show the reason */
        frontend_reset_buttons();
        frontend_load_tga("Front_End/MainMenu.tga", "Front_End/FrontEnd.zip");
        frontend_create_button(s_net_join_fail_msg, 80, 193, 480, 0x40); /* 0 message */
        frontend_create_button(SNK_OkButTxt, 232, 377, 160, 0x20);       /* 1 OK */
        s_inner_state = 11;
        break;

    case 11: /* [ITEM 1/5] JOIN: failure-message interaction -> back to picker */
        frontend_present_buffer();
        if (s_input_ready || frontend_check_escape()) {
            td5_frontend_set_screen(TD5_SCREEN_SESSION_PICKER);
            return;
        }
        break;

    default:
        break;
    }
}

void Screen_CreateSession(void) {
    switch (s_inner_state) {
    case 0: /* Init: HOST GAME setup — name / max players / password [S31] */
        frontend_init_return_screen(TD5_SCREEN_CREATE_SESSION);
        TD5_LOG_D(LOG_TAG, "CreateSession: init (direct=%d)", s_cs_direct);
        frontend_load_tga("Front_End/MainMenu.tga", "Front_End/FrontEnd.zip");
        /* [ITEM 4 2026-06-16] Direct hosting exposes two extra rows the LAN
         * host doesn't need: a UPnP on/off toggle (index 5) and a GAME PORT
         * entry (index 6). They slot in BELOW password; only HOST/BACK shift
         * down. NAME (index 0) + PASSWORD (index 2) keep their ORIGINAL y
         * (160 / 256) because frontend_render_create_session_overlay (in
         * td5_frontend.c, not editable here) draws their value text at those
         * hard-coded rows -- moving the buttons would detach the values. MAX
         * (index 1) tracks its button so it may move, but is left in place.
         * The fixed indices 0..4 keep the LAN interaction switch untouched. */
        if (s_cs_direct) {
            /* [ITEM 1 2026-06-16] Direct-host setup has 7 rows; the old layout
             * spaced them unevenly (48/48/40/36/40/40) and ran down to y=412.
             * Re-laid out with a uniform 40px pitch from y=148 so the rows are
             * evenly spaced and the block sits a little higher. Creation order
             * fixes the indices (0..6); the y-positions set the VISUAL order
             * (NAME, MAX, PASSWORD, UPNP, PORT, HOST, BACK). Navigation is
             * spatial (frontend_spatial_pick), so visual!=index order is fine.
             * NAME/PASSWORD value text is drawn by the overlay relative to the
             * live button row now, so re-spacing no longer detaches it. */
            frontend_create_button("SESSION NAME", 120, 148, 224, 0x20);  /* 0 */
            {   /* 1: MAX PLAYERS — selector-style (value tracks the button) */
                int bi = frontend_create_button("", 120, 188, 224, 0x20);
                if (bi >= 0 && bi < FE_MAX_BUTTONS) s_buttons[bi].is_selector = 1;
            }
            frontend_create_button("PASSWORD",     120, 228, 224, 0x20);  /* 2 */
            frontend_create_button("HOST",         120, 348, 160, 0x20);  /* 3 */
            frontend_create_button(SNK_BackButTxt, 120, 388, 112, 0x20);  /* 4 */
            /* 5/6: UPnP toggle + GAME PORT. Plain-label buttons (NOT selectors)
             * so the generic button renderer draws the live label -- the
             * selector value renderer in td5_frontend.c only knows MAX PLAYERS.
             * The labels are refreshed each interaction frame (case 2). They sit
             * between PASSWORD and HOST visually (indices stay 5/6). */
            frontend_create_button("UPNP: ON",     120, 268, 224, 0x20);  /* 5 */
            frontend_create_button("GAME PORT",    120, 308, 224, 0x20);  /* 6 */
        } else {
            frontend_create_button("SESSION NAME", 120, 160, 224, 0x20);  /* 0 */
            {   /* 1: MAX PLAYERS — selector-style (value + ◄ ► inside the button,
                 * drawn in the post-button pass like the other selector widgets) */
                int bi = frontend_create_button("", 120, 208, 224, 0x20);
                if (bi >= 0 && bi < FE_MAX_BUTTONS)
                    s_buttons[bi].is_selector = 1;
            }
            frontend_create_button("PASSWORD",     120, 256, 224, 0x20);  /* 2 */
            frontend_create_button("HOST",         120, 320, 160, 0x20);  /* 3 */
            frontend_create_button(SNK_BackButTxt, 120, 377, 112, 0x20);  /* 4 */
        }
        if (s_create_session_name[0] == '\0')
            strcpy(s_create_session_name, "New Session");
        /* [ITEM 3 2026-06-19] Start the host password blank on each new session.
         * s_lobby_password is SHARED with the JOIN flow, so a password typed to
         * join a passworded host would otherwise leak into this host field. The
         * join paths clear it themselves before prompting (cases 7/8), so
         * blanking here is safe and only affects host setup. */
        s_lobby_password[0] = '\0';
        if (s_lobby_max_players < 2 || s_lobby_max_players > 6)
            s_lobby_max_players = 6;
        /* [ITEM 4] Seed the UPnP toggle + game port from the live [Network] cfg
         * (already mirrored from the ini in Screen_ConnectionBrowser). */
        if (s_net_cfg_game_port < 1 || s_net_cfg_game_port > 65535)
            s_net_cfg_game_port = 37050;
        /* [2026-06-16] Seed the GAME PORT text buffer so the inline field (drawn
         * to the right of the button by frontend_render_create_session_overlay)
         * shows the live value before it is ever edited. */
        snprintf(s_cs_port_buf, sizeof(s_cs_port_buf), "%d", s_net_cfg_game_port);
        s_cs_edit = 0;
        s_cs_esc_guard = 0;
        s_anim_tick = 0;
        s_inner_state = 1;
        break;

    case 1: /* Slide-in (~500ms) */
        if (frontend_update_timed_animation(0x10, 267) >= 1.0f) {
            s_anim_complete = 1;
            s_inner_state = 2;
        }
        break;

    case 2: /* Setup interaction [S31] */
        /* [ITEM 4] Keep the Direct-host UPnP toggle label showing the live value
         * (a plain button drawn by the generic renderer). GAME PORT (index 6) is
         * now a LABEL-ONLY button; its value is drawn as an inline field to the
         * right by frontend_render_create_session_overlay, like NAME/PASSWORD. */
        if (s_cs_direct && 5 < FE_MAX_BUTTONS && s_buttons[5].active) {
            snprintf(s_buttons[5].label, sizeof(s_buttons[5].label),
                     "UPNP: %s", s_net_cfg_enable_upnp ? "ON" : "OFF");
        }
        if (s_cs_edit) {
            /* Editing NAME, PASSWORD or GAME PORT: Enter confirms, ESC cancels
             * (and is swallowed so it cannot double as BACK). */
            frontend_handle_text_input_key();
            if (td5_plat_input_key_pressed(0x01)) {
                /* Cancelling a port edit -> restore the field to the committed
                 * value so the inline display doesn't keep the typed garbage. */
                if (s_cs_edit == 3)
                    snprintf(s_cs_port_buf, sizeof(s_cs_port_buf), "%d", s_net_cfg_game_port);
                s_cs_edit = 0;
                s_text_input_state = 0;
                s_cs_esc_guard = 1;
                td5_plat_input_flush_chars();
            } else if (frontend_text_input_confirmed()) {
                if (s_cs_edit == 3) {     /* [ITEM 4] commit the GAME PORT entry */
                    int p = atoi(s_cs_port_buf);
                    if (p >= 1 && p <= 65535) s_net_cfg_game_port = p;
                    else s_net_cfg_game_port = 37050;
                    snprintf(s_cs_port_buf, sizeof(s_cs_port_buf), "%d", s_net_cfg_game_port);
                }
                s_cs_edit = 0;
                s_text_input_state = 0;
            }
            break;
        }
        if (s_cs_esc_guard) {
            if (!td5_plat_input_key_pressed(0x01)) s_cs_esc_guard = 0;
            break;
        }
        if (frontend_check_escape()) {       /* ESC == BACK */
            s_return_screen = s_cs_direct ? TD5_SCREEN_DIRECT_CONNECT
                                          : TD5_SCREEN_LAN_MENU;
            s_inner_state = 3;
            break;
        }
        /* MAX PLAYERS row: Left/Right adjust while it is the selected row. */
        if (s_selected_button == 1) {
            if (s_arrow_input & 1) {
                if (s_lobby_max_players > 2) s_lobby_max_players--;
                frontend_play_sfx(2);
            } else if (s_arrow_input & 2) {
                if (s_lobby_max_players < 6) s_lobby_max_players++;
                frontend_play_sfx(2);
            }
        }
        /* [ITEM 4] UPnP toggle row (Direct host only): Left/Right flip ON/OFF. */
        if (s_cs_direct && s_selected_button == 5 && (s_arrow_input & 3)) {
            s_net_cfg_enable_upnp = !s_net_cfg_enable_upnp;
            frontend_play_sfx(2);
        }
        if (s_input_ready && s_button_index >= 0) {
            switch (s_button_index) {
            case 0: /* SESSION NAME */
                frontend_begin_text_input(s_create_session_name,
                                          (int)sizeof(s_create_session_name));
                frontend_set_text_input_prompt("SESSION NAME");
                s_cs_edit = 1;
                break;
            case 1: /* MAX PLAYERS: activate = cycle */
                s_lobby_max_players = (s_lobby_max_players >= 6)
                                          ? 2 : s_lobby_max_players + 1;
                frontend_play_sfx(2);
                break;
            case 2: /* PASSWORD */
                frontend_begin_text_input(s_lobby_password,
                                          (int)sizeof(s_lobby_password));
                frontend_set_text_input_prompt("PASSWORD (BLANK = OPEN)");
                s_cs_edit = 2;
                break;
            case 3: { /* HOST */
                int ok;
                td5_net_set_mode(s_cs_direct ? TD5_NET_MODE_DIRECT
                                             : TD5_NET_MODE_LAN);
                if (s_cs_direct) {
                    /* [ITEM 4] Persist the chosen port + UPnP toggle so the
                     * next launch (and the boot-host CLI path in main.c) reuse
                     * them. Written via the public string writer formatting the
                     * int -- the reader (td5_ini_int / GetPrivateProfileIntA)
                     * parses it straight back. */
                    char pbuf[16];
                    g_td5.ini.net_game_port   = s_net_cfg_game_port;
                    g_td5.ini.net_enable_upnp = s_net_cfg_enable_upnp;
                    snprintf(pbuf, sizeof(pbuf), "%d", s_net_cfg_game_port);
                    td5_ini_write_str("Network", "GamePort", pbuf);
                    td5_ini_write_str("Network", "EnableUPnP",
                                      s_net_cfg_enable_upnp ? "1" : "0");
                    ok = td5_net_create_session_ex(s_create_session_name,
                                                   frontend_net_player_name(),
                                                   s_lobby_max_players,
                                                   s_net_cfg_game_port,
                                                   s_net_cfg_enable_upnp);
                } else
                    ok = td5_net_create_session(s_create_session_name,
                                                frontend_net_player_name(),
                                                s_lobby_max_players);
                if (ok) {
                    td5_net_set_session_limits(s_lobby_max_players,
                                               s_lobby_password);
                    s_network_active = 1;
                    s_return_screen = TD5_SCREEN_NETWORK_LOBBY;
                } else {
                    TD5_LOG_W(LOG_TAG, "host create failed (direct=%d)", s_cs_direct);
                    s_return_screen = s_cs_direct ? TD5_SCREEN_DIRECT_CONNECT
                                                  : TD5_SCREEN_LAN_MENU;
                }
                s_inner_state = 3;
                break;
            }
            case 4: /* BACK */
                s_return_screen = s_cs_direct ? TD5_SCREEN_DIRECT_CONNECT
                                              : TD5_SCREEN_LAN_MENU;
                s_inner_state = 3;
                break;
            case 5: /* [ITEM 4] UPnP toggle: activate = flip ON/OFF */
                if (s_cs_direct) {
                    s_net_cfg_enable_upnp = !s_net_cfg_enable_upnp;
                    frontend_play_sfx(2);
                }
                break;
            case 6: /* [ITEM 4] GAME PORT: open the numeric text entry */
                if (s_cs_direct) {
                    snprintf(s_cs_port_buf, sizeof(s_cs_port_buf), "%d",
                             s_net_cfg_game_port);
                    frontend_begin_text_input(s_cs_port_buf,
                                              (int)sizeof(s_cs_port_buf));
                    frontend_set_text_input_prompt("GAME PORT (DEFAULT 37050)");
                    s_cs_edit = 3;
                }
                break;
            }
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
        /* [#24] The netplay lobby uses the GLOBAL title path; clear the
         * split-screen simultaneous-grid flag on entry so a post-race return
         * never inherits a stale s_mp_simul that would blank the "NET PLAY"
         * header (root-cause partner to the lobby title-gate exemption). */
        s_mp_simul = 0;
        /* [2026-06-16] Re-baseline the host-keepalive clock on (re)entry so a
         * long detour through Change Car / Select Track doesn't carry stale
         * silence that instantly trips the host-gone watchdog. */
        td5_net_lobby_touch_host_clock();

#ifndef TD5RE_RELEASE
        /* Dev hook: TD5RE_NET_LOBBY=1 boots straight into a host lobby (e.g.
         * --StartScreen=11) so the lobby UI can be inspected without a 2nd PC. */
        if (!s_network_active && getenv("TD5RE_NET_LOBBY") &&
            getenv("TD5RE_NET_LOBBY")[0]) {   /* empty string = unset */
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
        td5_net_set_local_car(s_selected_car, s_selected_paint,
                              g_td5.ini.td6_paint_color);
        s_car_announce_done = (td5_net_local_slot() >= 0);

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
        /* [S31] Button 0: the host STARTS the race; clients toggle READY.
         * Width 180 so SELECT TRACK fits inside the frame. */
        frontend_create_button(frontend_net_is_host() ? SNK_StartButTxt
                                                      : "READY",
                                                    430, 110, 180, 0x20); /* 0 */
        frontend_create_button(SNK_ChangeCarButTxt, 430, 150, 180, 0x20); /* 1 CHANGE CAR */
        frontend_create_button("SELECT TRACK",      430, 190, 180, 0x20); /* 2 SELECT TRACK */
        frontend_create_button(SNK_ExitButTxt,      430, 230, 180, 0x20); /* 3 EXIT */
        /* [S31] Track choice is the host's call (the DXPSTART config overrides
         * any client-side pick anyway) -- hide SELECT TRACK for joiners. */
        if (!frontend_net_is_host()) {
            s_buttons[2].hidden   = 1;
            s_buttons[2].disabled = 1;
        }
        /* [S31 redesign] indices 4..8: per-row KICK buttons (host only) —
         * positioned/unhidden each frame next to the joined remote players;
         * the exit-door icon is drawn over them in the post-button pass. */
        if (frontend_net_is_host()) {
            int k;
            for (k = 0; k < 5; k++) {
                int bi = frontend_create_button("", 0, 0, 26, 22);
                if (bi >= 0 && bi < FE_MAX_BUTTONS) {
                    s_buttons[bi].hidden   = 1;
                    s_buttons[bi].disabled = 1;
                }
                s_kick_button_slot[k] = -1;
            }
        }

        /* [S31] READY state resets on every lobby entry (incl. the CHANGE
         * CAR return) -- joiners press READY again. */
        memset(s_slot_ready, 0, sizeof(s_slot_ready));
        s_my_ready = 0;

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
        /* S31: chat is OPT-IN (press T in the lobby). Keeping the text-input
         * flag armed here suppressed the keyboard nav FIFO for the whole
         * lobby (frontend_poll_input flushes nav while a text field is open),
         * which made every lobby button unreachable by keyboard. */
        frontend_reset_text_input();   /* also drops a stale confirm latch */
        s_inner_state = 3;
        break;

    case 3: /* MAIN INTERACTIVE LOBBY */
        /* [ITEM 4 2026-06-19] Removed the redundant frontend_present_buffer()
         * that ran here every lobby frame. The frontend display loop already
         * presents once per frame; the extra vblank-synced Present(1) meant TWO
         * presents per lobby frame, and with VSync on + the single-back-buffer
         * BitBlt swap chain they serialized across separate vblanks, halving the
         * lobby frame rate (the 30 fps on a 60 Hz panel the user reported). */

        /* [S31] Late car announce: the state-0 attempt is a no-op while the
         * JOIN handshake hasn't assigned our slot yet. */
        if (!s_car_announce_done && td5_net_local_slot() >= 0) {
            td5_net_set_local_car(s_selected_car, s_selected_paint,
                                  g_td5.ini.td6_paint_color);
            s_car_announce_done = 1;
        }

        /* ESC backs out of the lobby = the EXIT action (tear the session down so
         * no dangling host/session leaks). Mirrors button index 3. */
        if (frontend_check_escape()) {
            TD5_LOG_I(LOG_TAG, "NetworkLobby: ESC -> exit (destroy session)");
            frontend_net_destroy();
            s_network_active = 0;
            td5_frontend_set_screen(TD5_SCREEN_CONNECTION_BROWSER);
            return;
        }

        /* S10: keep the participant table mirrored to the live roster so the
         * host's ready check + the status panel reflect who has actually
         * joined (slots populated by the JOIN handshake / DXPROSTER). */
        {
            int slot;
            for (slot = 0; slot < 6; slot++) {
                s_participant_flags[slot] = td5_net_is_slot_active(slot) ? 1 : 0;
                if (!s_participant_flags[slot])
                    s_slot_ready[slot] = 0;   /* left/kicked -> not ready */
            }
        }

        /* [S31 redesign] place the per-row KICK buttons (host): kick button k
         * sits at the right edge of the k-th remote player's roster row. */
        if (frontend_net_is_host()) {
            int prow = 0, k = 0;
            int pslot;
            for (pslot = 0; pslot < TD5_NET_MAX_PLAYERS; pslot++) {
                if (!td5_net_is_slot_active(pslot)) continue;
                if (pslot != td5_net_local_slot() && k < 5) {
                    int bi = 4 + k;
                    if (bi < FE_MAX_BUTTONS && s_buttons[bi].active) {
                        s_buttons[bi].x = FE_LOBBY_X + FE_LOBBY_PANEL_W - 38;
                        s_buttons[bi].y = FE_LOBBY_ROW0_Y + prow * FE_LOBBY_ROW_H - 3;
                        s_buttons[bi].hidden   = 0;
                        s_buttons[bi].disabled = 0;
                        s_kick_button_slot[k]  = pslot;
                        k++;
                    }
                }
                prow++;
            }
            for (; k < 5; k++) {
                int bi = 4 + k;
                if (bi < FE_MAX_BUTTONS) {
                    s_buttons[bi].hidden   = 1;
                    s_buttons[bi].disabled = 1;
                }
                s_kick_button_slot[k] = -1;
            }
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
                } else if (ntype == (int)TD5_DXPDATA && nbuf[0] == 0x18 &&
                           nbuf[1] < 6) {
                    /* [S31] READY toggle from a client. */
                    s_slot_ready[nbuf[1]] = nbuf[2] ? 1 : 0;
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

        /* S31/[ITEM 3]: the host quit (or the link died) -> leave the dead lobby.
         * Two triggers:
         *   (a) a clean host quit broadcasts DXPDISCONNECT -> s_connection_lost;
         *   (b) an UNGRACEFUL host quit (process killed, link dropped) sends no
         *       DXPDISCONNECT, so the 1 Hz host keepalive simply goes silent --
         *       td5_net_lobby_host_silence_ms() crossing ~6 s catches that (the
         *       watchdog only arms once the first host packet has arrived, so a
         *       still-connecting client never trips it). The watchdog + the
         *       exit-to-main-menu route are gated by TD5RE_LOBBY_HOST_WATCHDOG;
         *       when off, only the legacy connection-lost -> browser path runs. */
        if (!frontend_net_is_host()) {
            int lost    = td5_net_is_connection_lost();
            int silence = td5_net_lobby_host_silence_ms();
            int wd_on   = lobby_host_watchdog_enabled();
            int gone    = lost || (wd_on && silence >= 0 && silence > 6000);
            if (gone) {
                TD5_LOG_I(LOG_TAG,
                          "NetworkLobby: host gone (lost=%d silence=%dms) -> leaving lobby",
                          lost, silence);
                s_race_active_flag = 0;
                s_network_active = 0;
                frontend_net_destroy();
                /* [ITEM 3] Exit to the MAIN MENU (not back into the dead lobby
                 * or the connection browser) when the watchdog owns the exit;
                 * legacy path keeps the browser destination. */
                /* [2026-06-19] Show the CONNECTION LOST notice (then main menu)
                 * instead of silently jumping; covers host timeout + host quit. */
                if (wd_on)
                    td5_frontend_show_net_disconnect(lost ? "The host left the session"
                                                          : "The host timed out");
                else
                    td5_frontend_set_screen(TD5_SCREEN_CONNECTION_BROWSER);
                return;
            }
        }

#ifndef TD5RE_RELEASE
        /* Dev hook: TD5RE_NET_AUTOSTART=1 -- the host fires START automatically
         * once a client has joined (headless 2-instance lockstep testing). */
        {
            static int s_dev_autostart_done = 0;
            const char *as = getenv("TD5RE_NET_AUTOSTART");
            if (!s_dev_autostart_done && as && as[0] &&   /* empty = unset */
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
                    /* [S31] The race reads g_td5.reverse_direction, which only
                     * the track screen's OK handler used to set -- a client
                     * (or a host that never opened the track screen) raced the
                     * wrong way. Difficulty picks the AI car-pool row, so it
                     * must match everywhere too. */
                    g_td5.reverse_direction = ncfg.reverse_direction;
                    g_td5.difficulty_tier   = ncfg.difficulty;
                    TD5_LOG_I(LOG_TAG,
                              "NetworkLobby: adopted host config track=%d dir=%d diff=%d seed=0x%08X",
                              ncfg.track_index, ncfg.reverse_direction,
                              ncfg.difficulty, ncfg.rng_seed);
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
                else {
                    /* [S31] Client: toggle READY and tell the host (the race
                     * itself starts on the host's DXPSTART). */
                    s_my_ready = !s_my_ready;
                    if (td5_net_local_slot() >= 0 && td5_net_local_slot() < 6)
                        s_slot_ready[td5_net_local_slot()] = (uint8_t)s_my_ready;
                    {
                        uint8_t rd_msg[8] = {0x18, 0, 0, 0, 0, 0, 0, 0};
                        rd_msg[1] = (uint8_t)td5_net_local_slot();
                        rd_msg[2] = (uint8_t)s_my_ready;
                        frontend_net_send(1, rd_msg, 8);
                    }
                    frontend_play_sfx(3);
                }
                break;

            case 1: /* CHANGE CAR */
                s_lobby_action = 1;
                td5_frontend_set_screen(TD5_SCREEN_CAR_SELECTION);
                return;

            case 2: /* SELECT TRACK -> the track picker (host only; hidden for
                     * joiners). flow_context==4 makes the track screen's exit
                     * dispatch return here (not launch a race). */
                if (frontend_net_is_host()) {
                    s_flow_context = 4;
                    td5_frontend_set_screen(TD5_SCREEN_TRACK_SELECTION);
                    return;
                }
                break;

            case 3: /* EXIT -> tear down the session and leave the lobby */
                TD5_LOG_I(LOG_TAG, "NetworkLobby: exit -> destroy session");
                frontend_net_destroy();
                s_network_active = 0;
                td5_frontend_set_screen(TD5_SCREEN_CONNECTION_BROWSER);
                return;

            default:
                /* [S31 redesign] indices 4..8 = per-row KICK buttons (host). */
                if (frontend_net_is_host() &&
                    s_button_index >= 4 && s_button_index <= 8) {
                    int ks = s_kick_button_slot[s_button_index - 4];
                    if (ks >= 0 && td5_net_is_slot_active(ks) &&
                        ks != td5_net_local_slot()) {
                        uint8_t kick_msg[8] = {0x12, 0, 0, 0, 0, 0, 0, 0};
                        kick_msg[1] = (uint8_t)ks;
                        frontend_net_send(1, kick_msg, 8);
                        frontend_play_sfx(3);
                        TD5_LOG_I(LOG_TAG, "NetworkLobby: kick sent for slot %d", ks);
                    }
                    break;
                }
                s_lobby_action = 0;
                break;
            }
        }

        /* Process network messages */
        /* Check for disconnect */
        if (s_kicked_flag) {
            s_kicked_flag = 0;
            frontend_net_destroy();
            td5_frontend_set_screen(TD5_SCREEN_SESSION_LOCKED);
            return;
        }

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
                    kick_msg[1] = (uint8_t)slot;
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
                /* Live frontend choices, NOT g_td5.* -- those are only
                 * refreshed by the schedule (which runs AFTER this config is
                 * broadcast) and were stale ("AI players are not loading"). */
                cfg.lap_count         = s_game_option_laps + 1;
                cfg.num_opponents     = s_num_ai_opponents;
                if (cfg.num_opponents < 0)  cfg.num_opponents = 0;
                if (cfg.num_opponents > TD5_MAX_RACER_SLOTS - 2)
                    cfg.num_opponents = TD5_MAX_RACER_SLOTS - 2;
                cfg.difficulty        = g_td5.difficulty_tier;
                /* [POLICE rewrite] Replicate the host's traffic volume + POLICE
                 * setting so the deterministic spawner + cop cadence match on
                 * every peer. */
                cfg.traffic_volume    = g_td5.ini.traffic;
                cfg.cops              = g_td5.ini.cops;
                for (slot = 0; slot < 6; slot++) {
                    int col = td5_net_get_slot_td6_color(slot);
                    cfg.car_index[slot]   = 0;
                    cfg.paint_index[slot] = 0;
                    /* 0xFFFFFF = "leave the grey base art" -- the same value
                     * the painter treats as no-op, so machines that never
                     * announced a colour render identically everywhere. */
                    cfg.td6_color[slot]   = (col >= 0) ? col : 0x00FFFFFF;
                    if (td5_net_get_slot_car(slot, &c, &p) && c >= 0) {
                        cfg.car_index[slot]   = c;
                        cfg.paint_index[slot] = p;
                    }
                }
                /* [MP GAME MODES 2026-06-22] Replicate the host's chosen game
                 * mode + per-mode options so every peer boots the same mode. */
                cfg.mode_config = g_td5.mp_mode_config;
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
            /* [S31] The host launches HERE (not through the state-3 client
             * block), so commit the broadcast config on this path too --
             * g_td5.reverse_direction is otherwise only written by the track
             * screen's OK handler and could be stale. */
            {
                TD5_NetRaceConfig ncfg;
                if (td5_net_get_race_config(&ncfg)) {
                    s_selected_track        = ncfg.track_index;
                    s_track_direction       = ncfg.reverse_direction;
                    g_td5.reverse_direction = ncfg.reverse_direction;
                    g_td5.difficulty_tier   = ncfg.difficulty;
                }
            }
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

    case 5: /* Wait for confirm -> main menu (the net-disconnect notice also
             * auto-times out so an unattended client doesn't sit here forever). */
        if (g_net_disconnect_mode) s_anim_tick += s_fe_logic_ticks;
        if (s_input_ready ||
            (g_net_disconnect_mode && s_anim_tick > 0x20 + 480 /* ~8s past slide-in */)) {
            g_net_disconnect_mode = 0;
            td5_frontend_set_screen(TD5_SCREEN_MAIN_MENU);
        }
        break;
    }
}

/* ========================================================================
 * MP GAME MODES (2026-06-22) — mode vote / mode config / cup winners
 *
 * SCAFFOLD: these handlers are registered in the screen table but not yet
 * reached by the live MP flow (the car/position transition wiring lands in a
 * later step). Bodies are intentionally minimal so the screens compile green
 * and, if entered prematurely, fall through to a safe screen instead of
 * softlocking. The interactive vote/config UI + net plumbing replace these.
 * ======================================================================== */

/* Screen_MpModeVote + Screen_MpModeConfig are implemented in td5_fe_race.c
 * (they reuse that file's local split-screen per-player input statics:
 * s_num_human_players, mp_simul_player_nav, s_mp_player_ready, k_mp_player_colors,
 * mp_pos_small_centered, ...). Only Screen_CupWinners stays here for now. */

void Screen_CupWinners(void) {
    switch (s_inner_state) {
    case 0:
        TD5_LOG_I(LOG_TAG, "Screen_CupWinners: enter (races=%d)",
                  td5_game_mp_cup_race_count());
        frontend_load_tga("Front_End/MainMenu.tga", "Front_End/FrontEnd.zip");
        frontend_reset_buttons();
        frontend_create_button(SNK_OkButTxt, 320 - 60, 400, 120, 28);
        s_anim_complete = 1;
        s_inner_state   = 1;
        break;
    default:
        /* Confirm (button / ESC) returns to the main menu and ends the cup. */
        if ((s_input_ready && s_button_index >= 0) || frontend_check_escape()) {
            td5_game_mp_cup_end();
            td5_frontend_set_screen(TD5_SCREEN_MAIN_MENU);
        }
        break;
    }
}

/* Final cup standings / podium — sorted by accumulated points (desc). Team
 * totals are shown when team mode is on. Drawn with the public VectorUI text. */
void frontend_cup_winners_render(float sx, float sy) {
    extern const uint32_t k_mp_player_colors[];
    int order[TD5_MAX_RACER_SLOTS], n = 0, i, j;
    char buf[64];
    float y;

    td5_vui_quad(0.0f, 0.0f, 640.0f * sx, 480.0f * sy, 0xE0000000u, -1, 0, 0, 1, 1);
    td5_vui_text_centered(320.0f * sx, 60.0f * sy, "CUP WINNERS", 0xFFFFE060u, sx, sy);
    snprintf(buf, sizeof buf, "FINAL STANDINGS  (%d RACES)", td5_game_mp_cup_race_count());
    td5_vui_text_centered(320.0f * sx, 92.0f * sy, buf, 0xFFB0B8C0u, sx, sy);

    /* Collect active racer slots, then selection-sort by points (descending). */
    for (i = 0; i < TD5_MAX_RACER_SLOTS; i++)
        if (td5_game_get_slot_state(i) != 3) order[n++] = i;
    for (i = 0; i < n; i++)
        for (j = i + 1; j < n; j++)
            if (td5_game_mp_cup_points(order[j]) > td5_game_mp_cup_points(order[i])) {
                int t = order[i]; order[i] = order[j]; order[j] = t;
            }

    y = 132.0f;
    for (i = 0; i < n && i < 6; i++) {
        int slot = order[i];
        uint32_t col = (k_mp_player_colors[slot % TD5_MAX_HUMAN_PLAYERS] & 0x00FFFFFFu) | 0xFF000000u;
        if (td5_game_mp_cup_team_mode())
            snprintf(buf, sizeof buf, "%d.  PLAYER %d  (TEAM %d)  -  %d PTS",
                     i + 1, slot + 1, td5_game_mp_cup_team(slot) + 1,
                     td5_game_mp_cup_points(slot));
        else
            snprintf(buf, sizeof buf, "%d.  PLAYER %d  -  %d PTS",
                     i + 1, slot + 1, td5_game_mp_cup_points(slot));
        td5_vui_text_centered(320.0f * sx, y * sy, buf,
                              (i == 0) ? 0xFFFFFFFFu : col, sx, sy);
        y += 30.0f;
    }

    /* Team totals (when teams are on): sum points per team id, up to 4 teams. */
    if (td5_game_mp_cup_team_mode()) {
        int tot[4] = {0,0,0,0}, k;
        for (i = 0; i < n; i++) {
            int tm = td5_game_mp_cup_team(order[i]);
            if (tm >= 0 && tm < 4) tot[tm] += td5_game_mp_cup_points(order[i]);
        }
        y += 8.0f;
        td5_vui_text_centered(320.0f * sx, y * sy, "TEAM TOTALS", 0xFFFFE060u, sx, sy);
        y += 26.0f;
        for (k = 0; k < 4; k++) {
            if (tot[k] == 0) continue;
            snprintf(buf, sizeof buf, "TEAM %d  -  %d PTS", k + 1, tot[k]);
            td5_vui_text_centered(320.0f * sx, y * sy, buf, 0xFFC0C8D0u, sx, sy);
            y += 24.0f;
        }
    }
}
