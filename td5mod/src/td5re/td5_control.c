/* ========================================================================
 * td5_control.c — live-control MCP transport (PORT-ONLY, DEV-ONLY)
 *
 * Opt-in UDP command server (127.0.0.1, default port 37060) that lets an
 * external process (scripts/td5re_mcp/server.py) drive a running td5re.exe.
 * See td5_control.h for the design/threading contract.
 *
 * Protocol: one JSON object per datagram.
 *   request : {"id":N,"cmd":"...","args":{...}}
 *   reply   : {"id":N,"ok":true,...}  or  {"id":N,"ok":false,"error":"..."}
 *
 * Verbs (v1): ping, get_state, start_race, end_race, set_screen,
 *   get_param, set_param, list_params, inject_key, tap_key, hold_key, quit.
 *
 * The listener thread only recvfrom()s and copies each datagram into a
 * mutex-guarded ring; td5_control_tick() (main thread) parses, executes and
 * replies. So every game/frontend call stays on the main thread.
 * ======================================================================== */
#ifndef TD5RE_RELEASE

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#include "td5re.h"
#include "td5_types.h"
#include "td5_control.h"
#include "td5_platform.h"
#include "td5_config.h"
#include "td5_frontend.h"
#include "td5_race_state.h"
#include "deps/cjson/cJSON.h"

#define LOG_TAG           "control"
#define CTRL_DEFAULT_PORT 37060
#define CTRL_RING_SLOTS   16
#define CTRL_DGRAM_MAX    2048
#define CTRL_PROTO_VER    1

/* ------------------------------------------------------------------------
 * Transport state
 * ---------------------------------------------------------------------- */
static int              s_enabled;
static SOCKET           s_sock = INVALID_SOCKET;
static HANDLE           s_thread;
static DWORD            s_thread_id;
static volatile LONG    s_stop;
static CRITICAL_SECTION s_lock;
static int              s_lock_init;

typedef struct {
    int                len;
    struct sockaddr_in from;
    char               data[CTRL_DGRAM_MAX];
} CtrlMsg;

static CtrlMsg s_ring[CTRL_RING_SLOTS];
static int     s_ring_head;   /* write index */
static int     s_ring_tail;   /* read index  */

/* end_race is a td5_game.c mutator; inverted (S7 pattern) so this module
 * never joins the td5_game.h includer set. Latched here, drained by
 * td5_game_tick() via td5_control_take_end_race_request(). */
static int s_end_race_request;

/* hold/tap key auto-release: frames remaining per DIK scancode. */
static int s_hold_frames[256];

/* ------------------------------------------------------------------------
 * Deferred (post-abort) race launch — a tiny state machine mirroring the
 * self-test director's SS_ENTER wait: when start_race{abort_current:true}
 * arrives mid-race we latch the end_race request and stash the scenario,
 * then launch it once the game returns to MENU.
 * ---------------------------------------------------------------------- */
#define CS_UNSET INT_MIN
typedef struct {
    int track, car, game_type, laps, opponents, players;
    int traffic, cops, dynamics, reverse, spectate, player_is_ai, auto_throttle;
} CtrlScenario;

static int          s_pending_launch;
static CtrlScenario s_pending;
static uint32_t     s_pending_deadline;

static void cs_init(CtrlScenario *s)
{
    int *p = (int *)s;
    size_t i, n = sizeof(*s) / sizeof(int);
    for (i = 0; i < n; i++) p[i] = CS_UNSET;
}

static void cs_from_json(CtrlScenario *s, cJSON *a)
{
    cJSON *v;
    cs_init(s);
    if (!a) return;
#define CS_GET(field, key) \
    if ((v = cJSON_GetObjectItemCaseSensitive(a, key)) && cJSON_IsNumber(v)) \
        s->field = v->valueint;
    CS_GET(track, "track")
    CS_GET(car, "car")
    CS_GET(game_type, "game_type")
    CS_GET(laps, "laps")
    CS_GET(opponents, "opponents")
    CS_GET(players, "players")
    CS_GET(traffic, "traffic")
    CS_GET(cops, "cops")
    CS_GET(dynamics, "dynamics")
    CS_GET(reverse, "reverse")
    CS_GET(spectate, "spectate")
    CS_GET(player_is_ai, "player_is_ai")
    CS_GET(auto_throttle, "auto_throttle")
#undef CS_GET
}

/* Write the scenario's provided fields into g_td5.ini and arm auto_race —
 * the exact recipe td5_frontend_auto_race_setup() consumes next MENU frame
 * (same as st_apply_scenario). Fields left CS_UNSET keep their INI value.
 * auto_throttle defaults ON so a bare start_race actually drives headless. */
static void cs_apply(const CtrlScenario *s)
{
    if (s->track        != CS_UNSET) g_td5.ini.default_track     = s->track;
    if (s->car          != CS_UNSET) g_td5.ini.default_car       = s->car;
    if (s->game_type    != CS_UNSET) g_td5.ini.default_game_type = s->game_type;
    if (s->laps         != CS_UNSET) g_td5.ini.laps              = s->laps;
    if (s->opponents    != CS_UNSET) g_td5.ini.default_opponents = s->opponents;
    if (s->players      != CS_UNSET) g_td5.ini.default_players   = s->players;
    if (s->traffic      != CS_UNSET) g_td5.ini.traffic           = s->traffic;
    if (s->cops         != CS_UNSET) g_td5.ini.cops              = s->cops;
    if (s->dynamics     != CS_UNSET) g_td5.ini.dynamics          = s->dynamics;
    if (s->reverse      != CS_UNSET) g_td5.ini.default_reverse   = s->reverse ? 1 : 0;
    if (s->spectate     != CS_UNSET) g_td5.ini.spectate_screens  = s->spectate;
    if (s->player_is_ai != CS_UNSET) g_td5.ini.player_is_ai      = s->player_is_ai;
    g_td5.ini.auto_throttle = (s->auto_throttle != CS_UNSET) ? s->auto_throttle : 1;
    g_td5.ini.auto_race = 1;   /* MENU state consumes this next frame */
}

/* ------------------------------------------------------------------------
 * Whitelisted INI params (name -> field, clamp range, when it applies)
 * ---------------------------------------------------------------------- */
typedef struct {
    const char *name;
    int        *ptr;
    int         lo, hi;
    int         at_launch;   /* 1 = takes effect at next race launch, 0 = immediate */
} CtrlIntParam;

static const CtrlIntParam k_params[] = {
    { "default_track",     &g_td5.ini.default_track,      0, 63,  1 },
    { "default_car",       &g_td5.ini.default_car,       -1, 63,  1 },
    { "default_game_type", &g_td5.ini.default_game_type,  0, 15,  1 },
    { "laps",              &g_td5.ini.laps,              -1, 99,  1 },
    { "default_opponents", &g_td5.ini.default_opponents, -1,  5,  1 },
    { "default_players",   &g_td5.ini.default_players,   -1,  6,  1 },
    { "traffic",           &g_td5.ini.traffic,            0,  4,  1 },
    { "cops",              &g_td5.ini.cops,               0,  1,  1 },
    { "dynamics",          &g_td5.ini.dynamics,           0,  1,  1 },
    { "default_reverse",   &g_td5.ini.default_reverse,    0,  1,  1 },
    { "spectate_screens",  &g_td5.ini.spectate_screens,   0,  5,  1 },
    { "player_is_ai",      &g_td5.ini.player_is_ai,       0,  1,  1 },
    { "auto_throttle",     &g_td5.ini.auto_throttle,      0,  1,  0 },
    { "debug_overlay",     &g_td5.ini.debug_overlay,      0,  1,  0 },
    { "car_damage",        &g_td5.ini.car_damage,         0,  1,  0 },
    { "difficulty",        &g_td5.ini.difficulty,         0,  2,  1 },
    { "lane_assist",       &g_td5.ini.lane_assist,        0,  1,  0 },
    { "sfx_volume",        &g_td5.ini.sfx_volume,         0,100,  0 },
    { "music_volume",      &g_td5.ini.music_volume,       0,100,  0 },
};
#define K_PARAMS_N (sizeof(k_params) / sizeof(k_params[0]))

static const CtrlIntParam *ctrl_find_param(const char *name)
{
    size_t i;
    if (!name) return NULL;
    for (i = 0; i < K_PARAMS_N; i++)
        if (strcmp(k_params[i].name, name) == 0) return &k_params[i];
    return NULL;
}

static const char *ctrl_state_name(int st)
{
    switch (st) {
    case TD5_GAMESTATE_INTRO: return "INTRO";
    case TD5_GAMESTATE_MENU:  return "MENU";
    case TD5_GAMESTATE_RACE:  return "RACE";
    default:                  return "OTHER";
    }
}

/* ------------------------------------------------------------------------
 * Command execution (main thread)
 * ---------------------------------------------------------------------- */
static void ctrl_err(cJSON *reply, const char *msg)
{
    cJSON_AddBoolToObject(reply, "ok", 0);
    cJSON_AddStringToObject(reply, "error", msg ? msg : "error");
}

static void ctrl_exec(cJSON *req, cJSON *reply)
{
    cJSON *j_cmd  = cJSON_GetObjectItemCaseSensitive(req, "cmd");
    cJSON *j_args = cJSON_GetObjectItemCaseSensitive(req, "args");
    const char *cmd = (j_cmd && cJSON_IsString(j_cmd)) ? j_cmd->valuestring : "";

    if (strcmp(cmd, "ping") == 0) {
        cJSON_AddBoolToObject(reply, "ok", 1);
        cJSON_AddNumberToObject(reply, "proto", CTRL_PROTO_VER);
        cJSON_AddStringToObject(reply, "build", "dev");
        cJSON_AddNumberToObject(reply, "pid", (double)GetCurrentProcessId());
        return;
    }

    if (strcmp(cmd, "get_state") == 0) {
        int st = g_td5.game_state;
        cJSON_AddBoolToObject(reply, "ok", 1);
        cJSON_AddNumberToObject(reply, "game_state", st);
        cJSON_AddStringToObject(reply, "game_state_name", ctrl_state_name(st));
        cJSON_AddNumberToObject(reply, "screen", (double)td5_frontend_get_screen());
        cJSON_AddBoolToObject(reply, "paused", td5_game_is_pause_menu_active() ? 1 : 0);
        if (st == TD5_GAMESTATE_RACE) {
            cJSON *race = cJSON_CreateObject();
            cJSON_AddNumberToObject(race, "track",       g_td5.ini.default_track);
            cJSON_AddNumberToObject(race, "game_type",   g_td5.ini.default_game_type);
            cJSON_AddNumberToObject(race, "sim_tick",    g_td5.simulation_tick_counter);
            cJSON_AddNumberToObject(race, "num_actors",  td5_game_get_total_actor_count());
            cJSON_AddNumberToObject(race, "player_slot", td5_game_get_player_slot(0));
            cJSON_AddItemToObject(reply, "race", race);
        }
        return;
    }

    if (strcmp(cmd, "start_race") == 0) {
        int abort_current = 0;
        cJSON *v = j_args ? cJSON_GetObjectItemCaseSensitive(j_args, "abort_current") : NULL;
        if (v && cJSON_IsBool(v)) abort_current = cJSON_IsTrue(v) ? 1 : 0;

        if (g_td5.game_state == TD5_GAMESTATE_MENU) {
            CtrlScenario sc;
            cs_from_json(&sc, j_args);
            cs_apply(&sc);
            cJSON_AddBoolToObject(reply, "ok", 1);
            cJSON_AddBoolToObject(reply, "pending", 0);
        } else if (g_td5.game_state == TD5_GAMESTATE_RACE && abort_current) {
            cs_from_json(&s_pending, j_args);
            s_pending_launch   = 1;
            s_pending_deadline = td5_plat_time_ms() + 60000u;
            s_end_race_request = 1;   /* td5_game_tick aborts the current race */
            cJSON_AddBoolToObject(reply, "ok", 1);
            cJSON_AddBoolToObject(reply, "pending", 1);
        } else {
            ctrl_err(reply, "start_race requires MENU state (or abort_current:true while racing)");
        }
        return;
    }

    if (strcmp(cmd, "end_race") == 0) {
        if (g_td5.game_state == TD5_GAMESTATE_RACE) {
            s_end_race_request = 1;
            cJSON_AddBoolToObject(reply, "ok", 1);
        } else {
            ctrl_err(reply, "not racing");
        }
        return;
    }

    if (strcmp(cmd, "set_screen") == 0) {
        cJSON *v = j_args ? cJSON_GetObjectItemCaseSensitive(j_args, "screen") : NULL;
        if (g_td5.game_state != TD5_GAMESTATE_MENU) {
            ctrl_err(reply, "set_screen requires MENU state");
        } else if (!v || !cJSON_IsNumber(v)) {
            ctrl_err(reply, "missing 'screen'");
        } else if (v->valueint < 0 || v->valueint >= TD5_SCREEN_COUNT) {
            ctrl_err(reply, "screen out of range");
        } else {
            td5_frontend_set_screen((TD5_ScreenIndex)v->valueint);
            cJSON_AddBoolToObject(reply, "ok", 1);
            cJSON_AddNumberToObject(reply, "screen", (double)td5_frontend_get_screen());
        }
        return;
    }

    if (strcmp(cmd, "get_param") == 0) {
        cJSON *v = j_args ? cJSON_GetObjectItemCaseSensitive(j_args, "name") : NULL;
        const char *name = (v && cJSON_IsString(v)) ? v->valuestring : NULL;
        const CtrlIntParam *p = ctrl_find_param(name);
        if (p) {
            cJSON_AddBoolToObject(reply, "ok", 1);
            cJSON_AddNumberToObject(reply, "value", *p->ptr);
        } else if (name && strcmp(name, "trace_fast_forward") == 0) {
            cJSON_AddBoolToObject(reply, "ok", 1);
            cJSON_AddNumberToObject(reply, "value", g_td5.ini.trace_fast_forward);
        } else {
            ctrl_err(reply, "unknown param");
        }
        return;
    }

    if (strcmp(cmd, "set_param") == 0) {
        cJSON *jn = j_args ? cJSON_GetObjectItemCaseSensitive(j_args, "name") : NULL;
        cJSON *jv = j_args ? cJSON_GetObjectItemCaseSensitive(j_args, "value") : NULL;
        const char *name = (jn && cJSON_IsString(jn)) ? jn->valuestring : NULL;
        const CtrlIntParam *p = ctrl_find_param(name);
        if (!jv || !cJSON_IsNumber(jv)) {
            ctrl_err(reply, "missing numeric 'value'");
        } else if (p) {
            int val = jv->valueint;
            if (val < p->lo) val = p->lo;
            if (val > p->hi) val = p->hi;
            *p->ptr = val;
            cJSON_AddBoolToObject(reply, "ok", 1);
            cJSON_AddNumberToObject(reply, "value", val);
            cJSON_AddStringToObject(reply, "applies", p->at_launch ? "next_race" : "immediate");
        } else if (name && strcmp(name, "trace_fast_forward") == 0) {
            double val = jv->valuedouble;
            if (val < 1.0)  val = 1.0;
            if (val > 16.0) val = 16.0;
            g_td5.ini.trace_fast_forward = (float)val;
            cJSON_AddBoolToObject(reply, "ok", 1);
            cJSON_AddNumberToObject(reply, "value", val);
            cJSON_AddStringToObject(reply, "applies", "next_race");
        } else {
            ctrl_err(reply, "unknown param");
        }
        return;
    }

    if (strcmp(cmd, "list_params") == 0) {
        cJSON *arr = cJSON_CreateArray();
        size_t i;
        for (i = 0; i < K_PARAMS_N; i++)
            cJSON_AddItemToArray(arr, cJSON_CreateString(k_params[i].name));
        cJSON_AddItemToArray(arr, cJSON_CreateString("trace_fast_forward"));
        cJSON_AddBoolToObject(reply, "ok", 1);
        cJSON_AddItemToObject(reply, "params", arr);
        return;
    }

    if (strcmp(cmd, "inject_key") == 0 || strcmp(cmd, "tap_key") == 0 ||
        strcmp(cmd, "hold_key") == 0) {
        cJSON *jd = j_args ? cJSON_GetObjectItemCaseSensitive(j_args, "dik") : NULL;
        if (!jd || !cJSON_IsNumber(jd) || jd->valueint < 0 || jd->valueint > 255) {
            ctrl_err(reply, "missing/invalid 'dik' (0..255)");
            return;
        }
        {
            int dik = jd->valueint;
            if (strcmp(cmd, "inject_key") == 0) {
                cJSON *jw = cJSON_GetObjectItemCaseSensitive(j_args, "down");
                int down = (jw && cJSON_IsBool(jw)) ? (cJSON_IsTrue(jw) ? 1 : 0) : 1;
                td5_plat_input_inject_key(dik, down);
            } else {
                /* tap = short hold; hold_key = N frames (default per cmd). */
                cJSON *jf = cJSON_GetObjectItemCaseSensitive(j_args, "frames");
                int frames = (jf && cJSON_IsNumber(jf)) ? jf->valueint
                                                        : (strcmp(cmd, "tap_key") == 0 ? 2 : 10);
                if (frames < 1)   frames = 1;
                if (frames > 600) frames = 600;
                td5_plat_input_inject_key(dik, 1);
                s_hold_frames[dik] = frames;
            }
            cJSON_AddBoolToObject(reply, "ok", 1);
        }
        return;
    }

    if (strcmp(cmd, "quit") == 0) {
        g_td5.quit_requested = 1;   /* main loop exits -> clean shutdown + log flush */
        cJSON_AddBoolToObject(reply, "ok", 1);
        return;
    }

    ctrl_err(reply, "unknown cmd");
}

static void ctrl_handle(const CtrlMsg *m)
{
    cJSON *req = cJSON_Parse(m->data);
    cJSON *reply = cJSON_CreateObject();
    double id = 0.0;

    if (req) {
        cJSON *jid = cJSON_GetObjectItemCaseSensitive(req, "id");
        if (jid && cJSON_IsNumber(jid)) id = jid->valuedouble;
    }
    cJSON_AddNumberToObject(reply, "id", id);

    if (!req) {
        ctrl_err(reply, "invalid json");
    } else {
        ctrl_exec(req, reply);
    }

    {
        char *txt = cJSON_PrintUnformatted(reply);
        if (txt) {
            sendto(s_sock, txt, (int)strlen(txt), 0,
                   (const struct sockaddr *)&m->from, sizeof(m->from));
            cJSON_free(txt);
        }
    }
    if (req) cJSON_Delete(req);
    cJSON_Delete(reply);
}

/* ------------------------------------------------------------------------
 * Listener thread — recvfrom + enqueue only (no game state touched).
 * ---------------------------------------------------------------------- */
static DWORD WINAPI ctrl_thread_proc(LPVOID param)
{
    (void)param;
    TD5_LOG_I(LOG_TAG, "listener thread started");
    while (!s_stop) {
        struct sockaddr_in from;
        int fromlen = (int)sizeof(from);
        char buf[CTRL_DGRAM_MAX];
        int n = recvfrom(s_sock, buf, sizeof(buf) - 1, 0,
                         (struct sockaddr *)&from, &fromlen);
        if (s_stop) break;
        if (n <= 0) {
            int err = WSAGetLastError();
            if (err == WSAEINTR || err == WSAENOTSOCK || err == WSAECONNRESET)
                { if (s_stop) break; }
            Sleep(5);   /* avoid busy-spin on transient error */
            continue;
        }
        buf[n] = '\0';
        EnterCriticalSection(&s_lock);
        {
            int next = (s_ring_head + 1) % CTRL_RING_SLOTS;
            if (next != s_ring_tail) {
                CtrlMsg *slot = &s_ring[s_ring_head];
                slot->len  = n;
                slot->from = from;
                memcpy(slot->data, buf, (size_t)n + 1);
                s_ring_head = next;
            }
            /* ring full -> drop; the client times out and retries */
        }
        LeaveCriticalSection(&s_lock);
    }
    TD5_LOG_I(LOG_TAG, "listener thread exiting");
    return 0;
}

/* ------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */
void td5_control_init(void)
{
    WSADATA wsa;
    struct sockaddr_in addr;
    int port;

    if (!g_td5.ini.control_enabled) return;

    port = td5_env_int("TD5RE_CONTROL_PORT", CTRL_DEFAULT_PORT, 1, 65535);

    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        TD5_LOG_E(LOG_TAG, "WSAStartup failed");
        return;
    }
    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_sock == INVALID_SOCKET) {
        TD5_LOG_E(LOG_TAG, "socket() failed (%d)", WSAGetLastError());
        WSACleanup();
        return;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons((u_short)port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   /* localhost-only */
    if (bind(s_sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        TD5_LOG_E(LOG_TAG, "bind(127.0.0.1:%d) failed (%d)", port, WSAGetLastError());
        closesocket(s_sock);
        s_sock = INVALID_SOCKET;
        WSACleanup();
        return;
    }

    InitializeCriticalSection(&s_lock);
    s_lock_init = 1;
    s_ring_head = s_ring_tail = 0;
    s_stop = 0;

    s_thread = CreateThread(NULL, 0, ctrl_thread_proc, NULL, 0, &s_thread_id);
    if (!s_thread) {
        TD5_LOG_E(LOG_TAG, "CreateThread failed");
        DeleteCriticalSection(&s_lock);
        s_lock_init = 0;
        closesocket(s_sock);
        s_sock = INVALID_SOCKET;
        WSACleanup();
        return;
    }

    s_enabled = 1;
    TD5_LOG_I(LOG_TAG, "control server listening on 127.0.0.1:%d (pid=%lu)",
              port, (unsigned long)GetCurrentProcessId());
}

void td5_control_shutdown(void)
{
    if (!s_enabled) return;

    InterlockedExchange(&s_stop, 1);
    if (s_sock != INVALID_SOCKET) {
        closesocket(s_sock);   /* unblocks the listener's recvfrom */
        s_sock = INVALID_SOCKET;
    }
    if (s_thread) {
        WaitForSingleObject(s_thread, 2000);
        CloseHandle(s_thread);
        s_thread = NULL;
    }
    if (s_lock_init) {
        DeleteCriticalSection(&s_lock);
        s_lock_init = 0;
    }
    WSACleanup();
    s_enabled = 0;
    TD5_LOG_I(LOG_TAG, "control server stopped");
}

void td5_control_tick(void)
{
    if (!s_enabled) return;

    /* Deferred (post-abort) race launch: wait for MENU, then apply. */
    if (s_pending_launch) {
        if (g_td5.game_state == TD5_GAMESTATE_MENU) {
            cs_apply(&s_pending);
            s_pending_launch = 0;
            TD5_LOG_I(LOG_TAG, "deferred start_race launched (post-abort)");
        } else if ((int32_t)(td5_plat_time_ms() - s_pending_deadline) >= 0) {
            s_pending_launch = 0;
            TD5_LOG_W(LOG_TAG, "deferred start_race timed out waiting for MENU");
        }
    }

    /* Auto-release held keys whose countdown has elapsed. */
    {
        int k;
        for (k = 0; k < 256; k++)
            if (s_hold_frames[k] > 0 && --s_hold_frames[k] == 0)
                td5_plat_input_inject_key(k, 0);
    }

    /* Drain queued command datagrams (execute + reply on the main thread). */
    for (;;) {
        CtrlMsg m;
        EnterCriticalSection(&s_lock);
        if (s_ring_tail == s_ring_head) {
            LeaveCriticalSection(&s_lock);
            break;
        }
        m = s_ring[s_ring_tail];
        s_ring_tail = (s_ring_tail + 1) % CTRL_RING_SLOTS;
        LeaveCriticalSection(&s_lock);
        ctrl_handle(&m);
    }
}

int td5_control_take_end_race_request(void)
{
    if (!s_end_race_request) return 0;
    s_end_race_request = 0;
    return 1;
}

#endif /* !TD5RE_RELEASE */
