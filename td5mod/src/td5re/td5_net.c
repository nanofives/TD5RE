/**
 * td5_net.c -- Multiplayer protocol, lockstep sync
 *
 * Source port replacement for DXPlay (M2DX.dll).
 * Original used DirectPlay 4A (IDirectPlay4A). This implementation uses
 * ENet for transport with the same lockstep deterministic protocol:
 * only input bitmasks + frame dt are synced, no game-world replication.
 *
 * Protocol: 13 DXPTYPE message types (0-12), 16-entry ring buffer,
 * 4 Win32 events, worker thread dispatching via WaitForMultipleObjects.
 *
 * Key references:
 *   re/analysis/network-multiplayer-protocol.md
 *   re/analysis/multiplayer-lobby-state-machine.md
 */

#include "td5_net.h"
#include "td5_platform.h"
#include "td5_upnp.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ========================================================================
 * Win32 threading primitives (matches original M2DX.dll architecture)
 *
 * The original DXPlay used Win32 events + worker thread with
 * WaitForMultipleObjects. We preserve this architecture exactly.
 * ======================================================================== */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

/* ========================================================================
 * Constants
 * ======================================================================== */

#define NET_LOG "net"

/** Ring buffer: 16 entries, 0x244 bytes each (4B type + 4B size + 0x23C payload) */
#define RING_ENTRY_COUNT    TD5_NET_RING_BUFFER_SIZE   /* 16 */
#define RING_PAYLOAD_MAX    0x23C                       /* 572 bytes */
#define RING_ENTRY_SIZE     0x244                       /* 580 bytes total */

/** Per-frame message size on wire */
#define FRAME_MSG_SIZE      TD5_NET_FRAME_MSG_SIZE      /* 0x80 = 128 bytes */

/** [SEC] Bytes prepended to every lockstep DXPTYPE datagram: the per-session
 *  auth token envelope [4B token][message]. Control messages (magic-tagged)
 *  are NOT enveloped -- they lead with WS2_DISCOVERY_MAGIC instead. */
#define NET_TOKEN_SIZE      4

/** Timeout for lockstep barrier waits (ms) */
#define SYNC_TIMEOUT_MS     20000

/** Maximum sessions cached from enumeration */
#define MAX_ENUM_SESSIONS   50

/** Maximum connections (service providers) */
#define MAX_CONNECTIONS     16

/** UDP port used by the Winsock2 backend for game traffic */
#define WS2_GAME_PORT       37050

/** UDP port used for LAN session discovery broadcasts */
#define WS2_DISCOVERY_PORT  37051

/** Magic number for discovery protocol messages */
#define WS2_DISCOVERY_MAGIC 0x54443552  /* "TD5R" */

/** Discovery message types (game-socket control channel, magic-tagged) */
#define WS2_DISC_QUERY       1
#define WS2_DISC_ANNOUNCE    2
#define WS2_DISC_JOIN_REQ    3
#define WS2_DISC_JOIN_ACK    4
#define WS2_DISC_JOIN_NAK    5   /* host rejected the join (full / bad password) */
#define WS2_DISC_PING        6   /* host -> client RTT probe (token = send time) */
#define WS2_DISC_PONG        7   /* client -> host echo                          */
#define WS2_DISC_ROSTER_INFO 8   /* host -> all: per-slot names + latency        */
#define WS2_DISC_CAR_INFO    9   /* client -> host: my car/paint pick (S31)      */

/** JOIN_NAK reason codes (also surfaced via td5_net_get_join_nak_reason). */
#define WS2_NAK_FULL         1
#define WS2_NAK_PASSWORD     2

/* Worker thread event indices for WaitForMultipleObjects */
#define EVT_RECEIVE         0
#define EVT_STOP            1
#define EVT_COUNT           2   /* only Receive + Stop in the wait array */

/* ========================================================================
 * Ring Buffer Entry
 * ======================================================================== */

typedef struct RingEntry {
    uint32_t    msg_type;                   /* DXPTYPE 0-12 */
    uint32_t    msg_size;                   /* payload size in bytes */
    uint8_t     payload[RING_PAYLOAD_MAX];  /* message payload */
} RingEntry;

/* Static assert: RingEntry must be exactly RING_ENTRY_SIZE */
typedef char sa_ring_size[(sizeof(RingEntry) == RING_ENTRY_SIZE) ? 1 : -1];

/* ========================================================================
 * Session / Player Info
 * ======================================================================== */

typedef struct PlayerSlot {
    uint32_t    id;             /* unique player id (index-based for source port) */
    uint32_t    active;         /* 1 = occupied, 0 = empty */
    char        name[64];       /* player name */
} PlayerSlot;

typedef struct SessionInfo {
    char        name[64];
    uint32_t    max_players;
    uint32_t    player_count;
    uint32_t    game_type;
    uint32_t    seed;
    int         sealed;
} SessionInfo;

typedef struct EnumSession {
    char        name[64];
    uint32_t    player_count;
    uint32_t    max_players;
    uint32_t    game_type;
} EnumSession;

/* ========================================================================
 * Module State (mirrors original DXPlay globals)
 * ======================================================================== */

/* --- Core state --- */
static int          s_initialized;
static int          s_is_host;              /* g_isInDirectPlaySession (1=host) */
static int          s_is_client;            /* g_isDirectPlayClient (1=in session) */
static int          s_active;               /* g_isDirectPlaySyncActive */
static int          s_connection_lost;      /* bConnectionLost */

/* --- S10: connection-mode / port / UPnP state --- */
static int          s_conn_mode = TD5_NET_MODE_LAN;
static int          s_game_port = WS2_GAME_PORT;        /* bound host game port */
static int          s_enable_upnp;                      /* host requested UPnP  */
static int          s_upnp_status = TD5_NET_UPNP_IDLE;
static uint16_t     s_upnp_mapped_port;                 /* port we asked UPnP to open */
static char         s_status_text[192];                 /* UI host/connect status line */
static char         s_local_ip[64];                     /* our LAN IP (display)  */
static char         s_external_ip[64];                  /* IGD WAN IP (double-NAT check) */
static int          net_ip_is_private(const char *dotted); /* fwd: double-NAT test (def below) */
static uint32_t     s_last_beacon_ms;                   /* periodic LAN ANNOUNCE clock */

/* --- S10: client join handshake (game-socket JOIN_REQ -> JOIN_ACK) --- */
static char         s_local_player_name[64];
static int          s_join_pending;                     /* awaiting JOIN_ACK    */
static int          s_join_attempts;                    /* JOIN_REQ retries so far */
static uint32_t     s_join_last_ms;                     /* last JOIN_REQ send clock */

/* --- S10b: lobby session config + per-slot info (names + latency) --- */
static char         s_host_password[32];                /* host: required join pw ("" = open) */
static char         s_join_password[32];                /* client: pw to send on join         */
static int          s_join_nak_reason;                  /* last JOIN_NAK reason (WS2_NAK_*)    */
static int          s_slot_latency[TD5_NET_MAX_PLAYERS];/* per-slot RTT ms (-1 unknown)        */
static char         s_slot_name[TD5_NET_MAX_PLAYERS][32];/* client-side roster names           */
static uint32_t     s_ping_last_ms;                     /* host: last PING broadcast clock     */
static uint32_t     s_roster_info_last_ms;              /* host: last ROSTER_INFO broadcast    */
/* [ITEM 3 2026-06-16] client: wall-clock of the last datagram seen from the
 * host. Lets the lobby detect an UNGRACEFUL host departure (process killed /
 * link dropped) where no DXPDISCONNECT arrives -- the host's 1 Hz ping +
 * roster-info keepalive stops, so a stale value means "host gone". 0 = none
 * seen yet (no watchdog until the first packet lands). */
static uint32_t     s_client_last_host_ms;
static uint32_t     s_session_token;                    /* [SEC] per-session token (0 = unknown) */

/* --- Session --- */
static SessionInfo  s_session;
static int          s_local_slot;           /* our slot index (0-5) */

/* --- Player roster --- */
static PlayerSlot   s_roster[TD5_NET_MAX_PLAYERS];
static int          s_player_count;

/* --- Enumerated sessions cache --- */
static EnumSession  s_enum_sessions[MAX_ENUM_SESSIONS];
static int          s_enum_session_count;

/* --- Connection (transport provider) cache --- */
static int          s_connection_count;
static int          s_selected_connection;

/* --- Ring buffer for received messages (16 entries) --- */
static RingEntry    s_ring[RING_ENTRY_COUNT];
static volatile LONG s_ring_write;          /* producer index (worker thread) */
static volatile LONG s_ring_read;           /* consumer index (game thread) */

/* --- Per-frame sync state --- */
static uint32_t     s_sync_sequence;        /* g_directPlaySyncSequence */
static uint32_t     s_player_sync_table[TD5_NET_MAX_PLAYERS]; /* received controlBits per slot */
static int          s_player_sync_received[TD5_NET_MAX_PLAYERS]; /* flag: input received this frame */
static float        s_received_frame_dt;    /* frame dt from host broadcast */

/* --- Resync barrier state --- */
static volatile LONG s_sync_generation;     /* g_directPlaySyncGeneration */
static int          s_waiting_for_sync_barrier; /* g_isWaitingForSyncBarrier */
static int          s_waiting_for_frame_ack;    /* g_isWaitingForFrameAck */
static int          s_pending_ack_count;    /* g_directPlayPendingAckCount */
static int          s_expected_ack_count;   /* g_directPlayExpectedAckCount */

/* --- 4 Win32 Events (matching original M2DX.dll) --- */
static HANDLE       s_evt_receive;          /* g_hDirectPlayReceiveEvent */
static HANDLE       s_evt_stop;             /* g_hDirectPlayWorkerStopEvent */
static HANDLE       s_evt_frame_ack;        /* g_hDirectPlayFrameAckEvent */
static HANDLE       s_evt_sync;             /* g_hDirectPlaySyncEvent */

/* --- Worker thread --- */
static HANDLE       s_worker_thread;
static DWORD        s_worker_thread_id;

/* --- Outbound frame buffer (reused each tick) --- */
static TD5_NetFrame s_outbound_frame;

/* --- Transport layer send/receive function pointers --- */
/*
 * The transport abstraction allows swapping ENet, SDL_net, Winsock, etc.
 * For the initial implementation we define the interface; the actual
 * transport backend is plugged in at init time.
 *
 * transport_send:    send raw bytes to a player (id=0 means broadcast)
 * transport_receive: non-blocking receive into buffer, returns bytes read or 0
 */
typedef int (*TransportSendFn)(uint32_t target_id, const void *data, int size);
typedef int (*TransportRecvFn)(uint32_t *sender_id, void *buf, int buf_size);
typedef int (*TransportInitFn)(void);
typedef int (*TransportHostFn)(const char *name, int max_players);
typedef int (*TransportJoinFn)(int session_index);
typedef int (*TransportJoinDirectFn)(const char *host_ip, int game_port);
typedef int (*TransportEnumFn)(void);
typedef void (*TransportShutdownFn)(void);

static TransportSendFn       s_transport_send;
static TransportRecvFn       s_transport_recv;
static TransportInitFn       s_transport_init;
static TransportHostFn       s_transport_host;
static TransportJoinFn       s_transport_join;
static TransportJoinDirectFn s_transport_join_direct;
static TransportEnumFn       s_transport_enum;
static TransportShutdownFn   s_transport_shutdown;

/* --- Discovery protocol message (sent on WS2_DISCOVERY_PORT) --- */
#pragma pack(push, 1)
typedef struct DiscoveryMsg {
    uint32_t    magic;
    uint32_t    disc_type;
    char        session_name[64];   /* ANNOUNCE: session name; JOIN_REQ: player name */
    uint32_t    player_count;
    uint32_t    max_players;
    uint32_t    game_type;
    uint32_t    sealed;
    uint16_t    game_port;
    uint32_t    assigned_slot;
    uint32_t    assigned_id;
    uint32_t    has_password;        /* ANNOUNCE: 1 if the session needs a password */
    uint32_t    nak_reason;          /* JOIN_NAK: WS2_NAK_* */
    char        password[32];        /* JOIN_REQ: password the joiner supplies      */
    uint32_t    session_token;       /* JOIN_ACK: per-session auth token (host->client) */
} DiscoveryMsg;

/* PING/PONG RTT probe (small; magic-tagged on the game socket). */
typedef struct CarInfoMsg {          /* S31: client announces its car pick */
    uint32_t    magic;
    uint32_t    disc_type;           /* WS2_DISC_CAR_INFO */
    uint32_t    slot;
    int32_t     car;
    int32_t     paint;
    int32_t     td6_color;          /* [S31] chosen TD6 body RGB (-1 = none) */
} CarInfoMsg;

typedef struct PingMsg {
    uint32_t    magic;
    uint32_t    disc_type;
    uint32_t    token;               /* host send timestamp (ms) echoed back */
} PingMsg;

/* Per-slot names + latency, broadcast by the host so every client can render the
 * lobby roster. Magic-tagged on the game socket. */
typedef struct RosterInfoMsg {
    uint32_t    magic;
    uint32_t    disc_type;
    uint32_t    active[TD5_NET_MAX_PLAYERS];
    uint32_t    latency_ms[TD5_NET_MAX_PLAYERS];
    char        names[TD5_NET_MAX_PLAYERS][32];
    uint32_t    host_slot;
} RosterInfoMsg;
#pragma pack(pop)

/* --- Winsock2 UDP backend state --- */
static SOCKET       s_ws2_socket = INVALID_SOCKET;

/* --- S31: race-config replication state ---------------------------------
 * s_race_config is armed on the host when it broadcasts DXPSTART (the same
 * bytes the clients get) and on clients when DXPSTART arrives. Consumers
 * (race schedule, InitRace seeding) gate on g_td5.network_active, so a
 * stale config from an earlier session cannot leak into local races. */
static TD5_NetRaceConfig s_race_config;
static int     s_race_config_valid = 0;
static volatile LONG s_race_config_seq = 0;  /* [SEC 2026-06-15] seqlock guarding
                                              * s_race_config across the worker
                                              * thread (writer) and game thread
                                              * (reader); odd = write in flight. */
static int32_t s_slot_car[TD5_NET_MAX_PLAYERS]   = { -1, -1, -1, -1, -1, -1 };
static int32_t s_slot_paint[TD5_NET_MAX_PLAYERS] = { 0 };
static int32_t s_slot_td6_color[TD5_NET_MAX_PLAYERS] = { -1, -1, -1, -1, -1, -1 };
static WSAEVENT     s_ws2_event = WSA_INVALID_EVENT;
static int          s_ws2_started;
static int          s_ws2_socket_bound;
static int          s_receive_event_is_ws2;
static SOCKADDR_IN  s_ws2_host_addr;
static SOCKADDR_IN  s_ws2_peer_addrs[TD5_NET_MAX_PLAYERS];
static int          s_ws2_peer_valid[TD5_NET_MAX_PLAYERS];
static SOCKET       s_ws2_disc_socket = INVALID_SOCKET;
/* [S31] Dedicated BROWSE socket (ephemeral port). Queries are sent and the
 * ANNOUNCE replies received here, NOT on the shared 37051 discovery socket:
 * with two instances on one machine both bound to 37051 (SO_REUSEADDR), the
 * host's unicast reply to a 37051 source was delivered to the FIRST binder
 * (the host itself) and the browser never saw any session. */
static SOCKET       s_ws2_browse_socket = INVALID_SOCKET;
static SOCKADDR_IN  s_ws2_enum_host_addrs[MAX_ENUM_SESSIONS];

/* ========================================================================
 * Forward Declarations
 * ======================================================================== */

static DWORD WINAPI     worker_thread_proc(LPVOID param);
static void             handle_app_message(uint32_t sender_id, const void *data, int size);
static void             ring_push(uint32_t type, const void *payload, int size);
static int              ring_pop(uint32_t *type, void **payload, int *size);
static int              ws2_transport_init(void);
static int              ws2_transport_send(uint32_t target_id, const void *data, int size);
static int              ws2_transport_recv(uint32_t *sender_id, void *buf, int buf_size);
static int              ws2_transport_host(const char *name, int max_players);
static int              ws2_transport_join(int session_index);
static int              ws2_transport_join_direct(const char *host_ip, int game_port);
static int              ws2_transport_enum(void);
static void             ws2_transport_shutdown(void);
static int              ws2_bind_socket(u_short port);
static uint32_t         ws2_resolve_sender_id(const SOCKADDR_IN *addr);
static const SOCKADDR_IN *ws2_get_target_addr(uint32_t target_id);
static int              ws2_discovery_init(void);
static void             ws2_discovery_shutdown(void);
static void             ws2_handle_join_request(const SOCKADDR_IN *from_addr);
static void             ws2_accept_join(const SOCKADDR_IN *from_addr, const char *name,
                                        const char *password, SOCKET reply_socket);
static void             ws2_handle_game_control(const void *buf, int size,
                                                const SOCKADDR_IN *from_addr);
static int              ws2_send_join_request(const char *player_name);
static void             ws2_send_join_nak(const SOCKADDR_IN *from_addr, int reason);
static void             ws2_broadcast_roster_info(void);
static void             ws2_send_pings(void);
static void             net_build_status_text(void);
static void             net_begin_client_join(const char *player_name);

/* Per-type handlers (13 DXPTYPE handlers) */
static void handle_frame(uint32_t sender, const void *data, int size);
static void handle_data(uint32_t sender, const void *data, int size);
static void handle_chat(uint32_t sender, const void *data, int size);
static void handle_data_targeted(uint32_t sender, const void *data, int size);
static void handle_start(uint32_t sender, const void *data, int size);
static void handle_ack_request(uint32_t sender, const void *data, int size);
static void handle_ack_reply(uint32_t sender, const void *data, int size);
static void handle_start_confirm(uint32_t sender, const void *data, int size);
static void handle_roster(uint32_t sender, const void *data, int size);
static void handle_disconnect(uint32_t sender, const void *data, int size);
static void handle_resync_req(uint32_t sender, const void *data, int size);
static void handle_resync(uint32_t sender, const void *data, int size);
static void handle_resync_ack(uint32_t sender, const void *data, int size);

/* Internal helpers */
static void             send_frame_to_all(const TD5_NetFrame *frame);
static void             send_to_player(uint32_t player_id, uint32_t type, const void *data, int size);
static void             send_to_all(uint32_t type, const void *data, int size);
static void             refresh_roster(void);
static int              count_active_remote_players(void);
static int              find_slot_by_id(uint32_t player_id);
static void             broadcast_roster(void);
static int              run_resync_barrier_host(void);
static int              run_resync_barrier_client(void);

/* ========================================================================
 * Dispatch table: 13 DXPTYPE handlers indexed by message type
 * ======================================================================== */

typedef void (*MsgHandler)(uint32_t sender, const void *data, int size);

static const MsgHandler s_handlers[13] = {
    /* 0  DXPFRAME          */ handle_frame,
    /* 1  DXPDATA           */ handle_data,
    /* 2  DXPCHAT           */ handle_chat,
    /* 3  DXPDATA_TARGETED  */ handle_data_targeted,
    /* 4  DXPSTART          */ handle_start,
    /* 5  DXPACK_REQUEST    */ handle_ack_request,
    /* 6  DXPACK_REPLY      */ handle_ack_reply,
    /* 7  DXPSTART_CONFIRM  */ handle_start_confirm,
    /* 8  DXPROSTER         */ handle_roster,
    /* 9  DXPDISCONNECT     */ handle_disconnect,
    /* 10 DXPRESYNCREQ      */ handle_resync_req,
    /* 11 DXPRESYNC         */ handle_resync,
    /* 12 DXPRESYNCACK      */ handle_resync_ack,
};

/* ========================================================================
 * Ring Buffer Implementation
 *
 * 16-entry ring, lock-free single-producer (worker) / single-consumer (game).
 * Each entry is 0x244 bytes: 4B type + 4B size + 0x23C payload.
 * ======================================================================== */

static void ring_reset(void)
{
    InterlockedExchange(&s_ring_write, 0);
    InterlockedExchange(&s_ring_read, 0);
    memset(s_ring, 0, sizeof(s_ring));
}

/**
 * Push a message into the ring buffer (called from worker thread).
 * If the ring is full, the oldest unread entry is silently overwritten.
 */
static void ring_push(uint32_t type, const void *payload, int size)
{
    LONG wr = InterlockedCompareExchange(&s_ring_write, 0, 0); /* read */
    LONG next = (wr + 1) % RING_ENTRY_COUNT;
    /* Note: if next == s_ring_read, ring is full -- overwrite oldest */

    RingEntry *e = &s_ring[wr % RING_ENTRY_COUNT];
    e->msg_type = type;
    if (size > (int)RING_PAYLOAD_MAX)
        size = (int)RING_PAYLOAD_MAX;
    e->msg_size = (uint32_t)size;
    if (payload && size > 0)
        memcpy(e->payload, payload, (size_t)size);
    else
        e->msg_size = 0;

    InterlockedExchange(&s_ring_write, next);
}

/**
 * Pop a message from the ring buffer (called from game thread).
 * Returns 1 if a message was dequeued, 0 if empty.
 * The caller receives a pointer into the ring entry payload -- valid until
 * the next ring_pop call.
 */
static int ring_pop(uint32_t *type, void **payload, int *size)
{
    LONG rd = InterlockedCompareExchange(&s_ring_read, 0, 0);
    LONG wr = InterlockedCompareExchange(&s_ring_write, 0, 0);
    if (rd == wr)
        return 0; /* empty */

    RingEntry *e = &s_ring[rd % RING_ENTRY_COUNT];
    if (type)    *type = e->msg_type;
    if (payload) *payload = e->payload;
    if (size)    *size = (int)e->msg_size;

    InterlockedExchange(&s_ring_read, (rd + 1) % RING_ENTRY_COUNT);
    return 1;
}

/* ========================================================================
 * Worker Thread
 *
 * Mirrors DirectPlayWorkerThreadProc at 0x1000c3e0.
 * WaitForMultipleObjects on {Receive, Stop}. On receive, pull all
 * pending messages from the transport layer and dispatch to handler.
 * On stop, exit cleanly.
 * ======================================================================== */

static DWORD WINAPI worker_thread_proc(LPVOID param)
{
    HANDLE wait_handles[EVT_COUNT];
    (void)param;

    TD5_LOG_I(NET_LOG, "Worker thread started");

    for (;;) {
        wait_handles[EVT_RECEIVE] = s_evt_receive;
        wait_handles[EVT_STOP]    = s_evt_stop;

        DWORD result = WaitForMultipleObjects(EVT_COUNT, wait_handles, FALSE, INFINITE);

        if (result == WAIT_OBJECT_0 + EVT_STOP) {
            /* Stop event signaled -- exit thread */
            TD5_LOG_I(NET_LOG, "Worker thread stopping (stop event)");
            break;
        }

        if (result == WAIT_OBJECT_0 + EVT_RECEIVE) {
            /* Receive event signaled -- drain all pending messages.
             * +NET_TOKEN_SIZE: a DXPTYPE datagram arrives token-enveloped and is
             * stripped down to <=RING_ENTRY_SIZE in ws2_transport_recv. */
            uint8_t recv_buf[RING_ENTRY_SIZE + NET_TOKEN_SIZE];
            uint32_t sender_id = 0;
            int bytes_read;

            if (s_receive_event_is_ws2 && s_ws2_socket != INVALID_SOCKET) {
                WSANETWORKEVENTS network_events;
                if (WSAEnumNetworkEvents(s_ws2_socket, (WSAEVENT)s_evt_receive, &network_events) == 0) {
                    if (network_events.lNetworkEvents & FD_CLOSE) {
                        s_connection_lost = 1;
                    }
                }
            }

            while (s_transport_recv &&
                   (bytes_read = s_transport_recv(&sender_id, recv_buf, sizeof(recv_buf))) > 0)
            {
                handle_app_message(sender_id, recv_buf, bytes_read);
            }

            /* Reset the receive event (auto-reset events do this automatically,
               but manual-reset needs explicit reset -- transport backend signals
               when more data arrives) */
            if (!s_receive_event_is_ws2) {
                ResetEvent(s_evt_receive);
            }
            continue;
        }

        /* WAIT_TIMEOUT or error -- check if we should exit */
        if (WaitForSingleObject(s_evt_stop, 0) == WAIT_OBJECT_0)
            break;
    }

    TD5_LOG_I(NET_LOG, "Worker thread exiting");
    return 0;
}

/* ========================================================================
 * Message Dispatcher
 *
 * Routes incoming wire messages to per-type handlers.
 * Mirrors HandleDirectPlayAppMessage at 0x1000c4d0.
 * ======================================================================== */

static void handle_app_message(uint32_t sender_id, const void *data, int size)
{
    const uint32_t *header;
    uint32_t msg_type;

    if (!data || size < 4)
        return;

    header = (const uint32_t *)data;
    msg_type = header[0];

    if (msg_type > 12) {
        TD5_LOG_W(NET_LOG, "Unknown DXPTYPE %u from sender %u", msg_type, sender_id);
        return;
    }

    /* [ITEM 3 2026-06-16] On a client, any datagram is proof the host is still
     * alive (star topology -- clients only converse with the host). Stamp the
     * keepalive clock so the lobby watchdog can spot an ungraceful host quit. */
    if (s_is_client && !s_is_host) {
        uint32_t now = td5_plat_time_ms();
        if (now == 0) now = 1;   /* keep 0 reserved for "never seen" */
        s_client_last_host_ms = now;
    }

    /* Dispatch to typed handler */
    if (s_handlers[msg_type]) {
        s_handlers[msg_type](sender_id, data, size);
    }
}

/* ========================================================================
 * DXPTYPE Handlers (13 message types)
 * ======================================================================== */

/**
 * Type 0 -- DXPFRAME (0x80 bytes)
 *
 * Two roles:
 *   Client->Host: client submits its local controlBits for its slot
 *   Host->All:    host broadcasts merged frame with all slots' controlBits
 *
 * On the host side (receiving from clients):
 *   - Copy the sender's controlBits into s_player_sync_table[slot]
 *   - Mark that slot as received
 *   - If all active clients have submitted, signal FrameAck
 *
 * On the client side (receiving host broadcast):
 *   - Copy all controlBits[0..5] into s_player_sync_table
 *   - Copy frameDeltaTime
 *   - Validate syncSequence
 *   - Signal FrameAck
 */
static void handle_frame(uint32_t sender, const void *data, int size)
{
    const TD5_NetFrame *frame;

    if (size < (int)sizeof(TD5_NetFrame))
        return;

    frame = (const TD5_NetFrame *)data;

    if (s_is_host) {
        /* Host receiving a client's input submission */
        int slot = find_slot_by_id(sender);
        if (slot < 0 || slot >= TD5_NET_MAX_PLAYERS)
            return;

        /* Store client's input for their slot */
        s_player_sync_table[slot] = frame->control_bits[slot];
        s_player_sync_received[slot] = 1;

        /* Check if all active remote players have submitted */
        {
            int all_received = 1;
            int i;
            for (i = 0; i < TD5_NET_MAX_PLAYERS; i++) {
                if (i == s_local_slot) continue;
                if (!s_roster[i].active) continue;
                if (!s_player_sync_received[i]) {
                    all_received = 0;
                    break;
                }
            }
            if (all_received) {
                SetEvent(s_evt_frame_ack);
            }
        }
    } else {
        /* Client receiving host's authoritative broadcast */
        int i;
        for (i = 0; i < TD5_NET_MAX_PLAYERS; i++) {
            s_player_sync_table[i] = frame->control_bits[i];
        }
        s_received_frame_dt = frame->frame_delta_time;

        /* Validate sync sequence */
        if (frame->sync_sequence != s_sync_sequence) {
            TD5_LOG_W(NET_LOG, "Sync sequence mismatch: expected %u, got %u",
                      s_sync_sequence, frame->sync_sequence);
        }
        s_sync_sequence = frame->sync_sequence + 1;

        /* Signal that authoritative frame data is available */
        SetEvent(s_evt_frame_ack);
    }
}

/**
 * Type 1 -- DXPDATA (variable size)
 * General data payload -- frontend lobby opcodes, status updates.
 * Layout: [4B type=1] [4B payload_size] [N payload_data]
 * Queued into ring buffer for game-thread consumption.
 */
static void handle_data(uint32_t sender, const void *data, int size)
{
    (void)sender;
    if (size > 8) {
        const uint8_t *p = (const uint8_t *)data;
        uint32_t payload_size = *(const uint32_t *)(p + 4);
        if (payload_size > (uint32_t)(size - 8))
            payload_size = (uint32_t)(size - 8);
        ring_push(TD5_DXPDATA, p + 8, (int)payload_size);
    } else {
        ring_push(TD5_DXPDATA, NULL, 0);
    }
}

/**
 * Type 2 -- DXPCHAT (variable size)
 * Chat text message. Layout: [4B type=2] [N null-terminated string]
 * Queued into ring buffer.
 */
static void handle_chat(uint32_t sender, const void *data, int size)
{
    (void)sender;
    if (size > 4) {
        const uint8_t *p = (const uint8_t *)data;
        ring_push(TD5_DXPCHAT, p + 4, size - 4);
    }
}

/**
 * Type 3 -- DXPDATA_TARGETED (variable size)
 * Same layout as DXPDATA but queued with a distinct type so the game
 * thread can differentiate targeted vs broadcast data messages.
 */
static void handle_data_targeted(uint32_t sender, const void *data, int size)
{
    (void)sender;
    if (size > 8) {
        const uint8_t *p = (const uint8_t *)data;
        uint32_t payload_size = *(const uint32_t *)(p + 4);
        if (payload_size > (uint32_t)(size - 8))
            payload_size = (uint32_t)(size - 8);
        ring_push(TD5_DXPDATA_TARGETED, p + 8, (int)payload_size);
    } else {
        ring_push(TD5_DXPDATA_TARGETED, NULL, 0);
    }
}

/* [SEC 2026-06-15] Publish the race config under the seqlock so a reader on the
 * game thread (td5_net_get_race_config) can never observe a half-written struct
 * torn by the worker thread mid-memcpy. Single writer per machine (host: game
 * thread via td5_net_send; client: worker thread via handle_start). */
static void race_config_publish(const void *src)
{
    InterlockedIncrement(&s_race_config_seq);            /* enter write (odd)  */
    memcpy(&s_race_config, src, sizeof(s_race_config));
    s_race_config_valid = 1;
    InterlockedIncrement(&s_race_config_seq);            /* leave write (even) */
}

/**
 * Type 4 -- DXPSTART (4 bytes)
 * Host signals race start to each client.
 * Client responds with ACK_REPLY (type 6).
 * Queued so game thread can transition to race.
 */
static void handle_start(uint32_t sender, const void *data, int size)
{
    (void)sender;

    TD5_LOG_I(NET_LOG, "Received DXPSTART");

    /* S31: the payload (after the type dword) carries the host's race config
     * -- adopt it so this machine launches the same track/cars/seed. */
    if (data && size >= 4 + (int)sizeof(TD5_NetRaceConfig)) {
        race_config_publish((const uint8_t *)data + 4);
        TD5_LOG_I(NET_LOG, "DXPSTART config: seed=0x%08X track=%d dir=%d",
                  s_race_config.rng_seed, s_race_config.track_index,
                  s_race_config.reverse_direction);
    } else {
        TD5_LOG_W(NET_LOG, "DXPSTART without race config (size=%d)", size);
    }

    /* Queue for game thread */
    ring_push(TD5_DXPSTART, NULL, 0);

    /* Reply with a SINGLE ACK_REPLY to the host. (A previous version also
     * called send_to_player(own id, ...), which the client-side address
     * fallback ALSO routed to the host -- with >=2 clients the resulting
     * double-ACK could zero the host's pending-ack counter before every
     * client had actually acked.) */
    if (!s_is_host && s_transport_send) {
        uint32_t one[1] = { TD5_DXPACK_REPLY };
        s_transport_send(0, one, 4); /* id 0 routes to the host on clients */
    }
}

/**
 * Type 5 -- DXPACK_REQUEST (4 bytes)
 * Host requests acknowledgement from client.
 * Client auto-replies with ACK_REPLY (type 6).
 */
static void handle_ack_request(uint32_t sender, const void *data, int size)
{
    (void)sender;
    (void)data;
    (void)size;

    TD5_LOG_D(NET_LOG, "Received DXPACK_REQUEST");

    if (!s_is_host) {
        /* Send ACK_REPLY back to host */
        uint32_t buf[1] = { TD5_DXPACK_REPLY };
        if (s_transport_send)
            s_transport_send(sender, buf, 4);
    }
}

/**
 * Type 6 -- DXPACK_REPLY (4 bytes)
 * Client sends acknowledgement to host.
 * Host decrements pending ack counter; when all received, signals.
 */
static void handle_ack_reply(uint32_t sender, const void *data, int size)
{
    (void)sender;
    (void)data;
    (void)size;

    TD5_LOG_D(NET_LOG, "Received DXPACK_REPLY from %u", sender);

    if (s_is_host) {
        s_pending_ack_count--;
        if (s_pending_ack_count <= 0) {
            /* All clients have acked -- broadcast START_CONFIRM */
            uint32_t confirm[1] = { TD5_DXPSTART_CONFIRM };
            send_to_all(TD5_DXPSTART_CONFIRM, confirm, 4);

            /* Enable frame sync */
            s_active = 1;
            s_sync_sequence = 0;

            /* Signal sync event */
            SetEvent(s_evt_sync);
        }
    }
}

/**
 * Type 7 -- DXPSTART_CONFIRM (4 bytes)
 * Host confirms all clients are ready; race begins.
 * Sets sync active flag and signals sync event.
 */
static void handle_start_confirm(uint32_t sender, const void *data, int size)
{
    (void)sender;
    (void)data;
    (void)size;

    TD5_LOG_I(NET_LOG, "Received DXPSTART_CONFIRM -- race starting");

    s_active = 1;
    s_sync_sequence = 0;

    /* Queue for game thread */
    ring_push(TD5_DXPSTART_CONFIRM, NULL, 0);

    /* Signal sync event to unblock any waiting code */
    SetEvent(s_evt_sync);
}

/**
 * Type 8 -- DXPROSTER (0x34 = 52 bytes)
 * Roster snapshot broadcast by host on join/leave/migration.
 * Layout: [4B type=8] [6x4B player_ids] [6x4B active_flags] [4B host_id]
 */
static void handle_roster(uint32_t sender, const void *data, int size)
{
    const TD5_NetRoster *roster;
    int i;

    (void)sender;

    if (size < (int)sizeof(TD5_NetRoster))
        return;

    roster = (const TD5_NetRoster *)data;

    s_player_count = 0;
    for (i = 0; i < TD5_NET_MAX_PLAYERS; i++) {
        s_roster[i].id     = roster->player_ids[i];
        s_roster[i].active = roster->active_flags[i];
        if (s_roster[i].active)
            s_player_count++;

        /* Detect if we are the host (host_id matches our slot's id) */
        if (s_roster[i].active && s_roster[i].id == roster->host_id) {
            if (i == s_local_slot) {
                if (!s_is_host) {
                    TD5_LOG_I(NET_LOG, "Host migration: we are now host (slot %d)", i);
                }
                s_is_host = 1;
            }
        }
    }

    /* Queue for game thread */
    ring_push(TD5_DXPROSTER, (const uint8_t *)data + 4, size - 4);
}

/**
 * Type 9 -- DXPDISCONNECT (4 bytes)
 * Explicit disconnect / connection-loss notification.
 * Sets connection_lost flag and queues for game thread.
 */
static void handle_disconnect(uint32_t sender, const void *data, int size)
{
    (void)data;
    (void)size;

    /* [S31] HOST receiving a CLIENT's disconnect (kick honoured, lobby exit,
     * app close): free that roster slot and tell everyone. Without this the
     * row lingered forever and a rejoin grabbed a NEW slot ("kicked player
     * still listed; 3 players after rejoining"). Only a CLIENT losing its
     * link to the host flags connection_lost. */
    if (s_is_host) {
        int slot = find_slot_by_id(sender);
        TD5_LOG_W(NET_LOG, "Received DXPDISCONNECT from id=%u (slot %d)",
                  sender, slot);
        if (slot > 0 && slot < TD5_NET_MAX_PLAYERS) {
            int i, n = 0;
            s_roster[slot].active  = 0;
            s_roster[slot].id      = 0;
            s_roster[slot].name[0] = '\0';
            s_ws2_peer_valid[slot] = 0;
            s_slot_car[slot]   = -1;
            s_slot_paint[slot] = 0;
            for (i = 0; i < TD5_NET_MAX_PLAYERS; i++)
                if (s_roster[i].active) n++;
            s_player_count = n;
            s_slot_td6_color[slot] = -1;
            ws2_broadcast_roster_info();
            /* Mid-race: the lockstep barrier may be blocked waiting on this
             * client's input. Wake it -- the freed slot is no longer counted,
             * so the host merges the remaining players and races on (the
             * window froze for the full 20 s timeout otherwise). */
            SetEvent(s_evt_frame_ack);
        }
        ring_push(TD5_DXPDISCONNECT, NULL, 0);
        return;
    }

    TD5_LOG_W(NET_LOG, "Received DXPDISCONNECT");

    s_connection_lost = 1;

    /* Queue for game thread */
    ring_push(TD5_DXPDISCONNECT, NULL, 0);
}

/**
 * Type 10 -- DXPRESYNCREQ (4 bytes)
 * Host initiates resync barrier. Received by clients.
 *
 * Handler:
 *   1. Increment s_sync_generation (this client needs to drain a generation)
 *   2. If waiting on sync barrier, signal sync event
 *   3. If waiting on frame ack, signal frame ack event
 *
 * This wakes up any client blocked in HandlePadClient so it can enter
 * the resync drain loop.
 */
static void handle_resync_req(uint32_t sender, const void *data, int size)
{
    (void)sender;
    (void)data;
    (void)size;

    TD5_LOG_I(NET_LOG, "Received DXPRESYNCREQ");

    InterlockedIncrement(&s_sync_generation);

    if (s_waiting_for_sync_barrier)
        SetEvent(s_evt_sync);

    if (s_waiting_for_frame_ack)
        SetEvent(s_evt_frame_ack);
}

/**
 * Type 11 -- DXPRESYNC (4 bytes)
 * Client responds to host's resync request.
 * Only processed on host side.
 *
 * Handler (host):
 *   1. Decrement pending ack count
 *   2. When all clients responded:
 *      - Send DXPRESYNCACK (type 12) to each active remote player
 *      - Signal sync event (unblocks host's barrier wait)
 *      - Reset pending count
 */
static void handle_resync(uint32_t sender, const void *data, int size)
{
    (void)sender;
    (void)data;
    (void)size;

    if (!s_is_host) return;

    TD5_LOG_D(NET_LOG, "Received DXPRESYNC from %u", sender);

    s_pending_ack_count--;
    if (s_pending_ack_count <= 0) {
        /* All clients responded -- send DXPRESYNCACK to each */
        int i;
        uint32_t ack_buf[1] = { TD5_DXPRESYNCACK };
        for (i = 0; i < TD5_NET_MAX_PLAYERS; i++) {
            if (i == s_local_slot) continue;
            if (!s_roster[i].active) continue;
            if (s_transport_send)
                s_transport_send(s_roster[i].id, ack_buf, 4);
        }

        /* Signal host's sync event */
        SetEvent(s_evt_sync);

        /* Reset for next potential barrier cycle */
        s_pending_ack_count = s_expected_ack_count;
    }
}

/**
 * Type 12 -- DXPRESYNCACK (4 bytes)
 * Host confirms resync complete. Received by clients.
 * Signals sync event to unblock client's barrier wait.
 */
static void handle_resync_ack(uint32_t sender, const void *data, int size)
{
    (void)sender;
    (void)data;
    (void)size;

    TD5_LOG_I(NET_LOG, "Received DXPRESYNCACK");

    /* Unblock client's HandlePadClient barrier wait */
    SetEvent(s_evt_sync);
}

/* ========================================================================
 * Internal Helpers
 * ======================================================================== */

/** Find the roster slot index for a given player ID. Returns -1 if not found. */
static int find_slot_by_id(uint32_t player_id)
{
    int i;
    for (i = 0; i < TD5_NET_MAX_PLAYERS; i++) {
        if (s_roster[i].active && s_roster[i].id == player_id)
            return i;
    }
    return -1;
}

/** Count active remote players (active slots excluding local). */
static int count_active_remote_players(void)
{
    int count = 0, i;
    for (i = 0; i < TD5_NET_MAX_PLAYERS; i++) {
        if (i == s_local_slot) continue;
        if (s_roster[i].active) count++;
    }
    return count;
}

/** Send typed message to a specific player via transport. */
static void send_to_player(uint32_t player_id, uint32_t type, const void *data, int size)
{
    (void)type;
    if (s_transport_send)
        s_transport_send(player_id, data, size);
}

/** Broadcast typed message to all players via transport (id=0 = broadcast). */
static void send_to_all(uint32_t type, const void *data, int size)
{
    (void)type;
    if (s_transport_send)
        s_transport_send(0, data, size);
}

/** Broadcast the full frame to all players. */
static void send_frame_to_all(const TD5_NetFrame *frame)
{
    if (s_transport_send)
        s_transport_send(0, frame, FRAME_MSG_SIZE);
}

/** Refresh the roster by querying the transport layer. */
static void refresh_roster(void)
{
    /* In the source port, roster is maintained by the host and distributed
       via DXPROSTER messages. This function is a no-op for the transport
       layer -- the host calls broadcast_roster() after any topology change. */
    TD5_LOG_D(NET_LOG, "Roster refresh requested");
}

/** Host: build and broadcast a DXPROSTER message to all players. */
static void broadcast_roster(void)
{
    TD5_NetRoster msg;
    int i;

    if (!s_is_host) return;

    memset(&msg, 0, sizeof(msg));
    msg.msg_type = TD5_DXPROSTER;

    s_player_count = 0;
    for (i = 0; i < TD5_NET_MAX_PLAYERS; i++) {
        msg.player_ids[i]   = s_roster[i].id;
        msg.active_flags[i] = s_roster[i].active;
        if (s_roster[i].active) s_player_count++;
    }
    msg.host_id = s_roster[s_local_slot].id;

    send_to_all(TD5_DXPROSTER, &msg, sizeof(msg));
}

/* ========================================================================
 * Winsock2 UDP transport backend
 * ======================================================================== */

static int ws2_transport_init(void)
{
    WSADATA wsa_data;
    u_long nonblocking = 1;

    if (s_ws2_started) {
        return 1;
    }

    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        TD5_LOG_E(NET_LOG, "WSAStartup failed");
        return 0;
    }

    s_ws2_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_ws2_socket == INVALID_SOCKET) {
        TD5_LOG_E(NET_LOG, "socket(AF_INET, SOCK_DGRAM) failed: %d", (int)WSAGetLastError());
        WSACleanup();
        return 0;
    }

    if (ioctlsocket(s_ws2_socket, FIONBIO, &nonblocking) != 0) {
        TD5_LOG_E(NET_LOG, "ioctlsocket(FIONBIO) failed: %d", (int)WSAGetLastError());
        closesocket(s_ws2_socket);
        s_ws2_socket = INVALID_SOCKET;
        WSACleanup();
        return 0;
    }

    s_ws2_event = WSACreateEvent();
    if (s_ws2_event == WSA_INVALID_EVENT) {
        TD5_LOG_E(NET_LOG, "WSACreateEvent failed: %d", (int)WSAGetLastError());
        closesocket(s_ws2_socket);
        s_ws2_socket = INVALID_SOCKET;
        WSACleanup();
        return 0;
    }

    if (WSAEventSelect(s_ws2_socket, s_ws2_event, FD_READ | FD_CLOSE) != 0) {
        TD5_LOG_E(NET_LOG, "WSAEventSelect failed: %d", (int)WSAGetLastError());
        WSACloseEvent(s_ws2_event);
        s_ws2_event = WSA_INVALID_EVENT;
        closesocket(s_ws2_socket);
        s_ws2_socket = INVALID_SOCKET;
        WSACleanup();
        return 0;
    }

    memset(&s_ws2_host_addr, 0, sizeof(s_ws2_host_addr));
    memset(s_ws2_peer_addrs, 0, sizeof(s_ws2_peer_addrs));
    memset(s_ws2_peer_valid, 0, sizeof(s_ws2_peer_valid));
    s_evt_receive = (HANDLE)s_ws2_event;
    s_receive_event_is_ws2 = 1;
    s_ws2_socket_bound = 0;
    s_ws2_started = 1;

    TD5_LOG_I(NET_LOG, "Winsock2 UDP backend initialized");
    return 1;
}

static int ws2_bind_socket(u_short port)
{
    SOCKADDR_IN local_addr;

    if (s_ws2_socket == INVALID_SOCKET) {
        return 0;
    }
    if (s_ws2_socket_bound) {
        return 1;
    }

    memset(&local_addr, 0, sizeof(local_addr));
    local_addr.sin_family = AF_INET;
    local_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    local_addr.sin_port = htons(port);

    if (bind(s_ws2_socket, (const struct sockaddr *)&local_addr, sizeof(local_addr)) == SOCKET_ERROR) {
        TD5_LOG_E(NET_LOG, "bind(%u) failed: %d", (unsigned)port, (int)WSAGetLastError());
        return 0;
    }

    s_ws2_socket_bound = 1;
    return 1;
}

static int ws2_transport_host(const char *name, int max_players)
{
    (void)name;
    (void)max_players;

    if (!ws2_bind_socket((u_short)s_game_port)) {
        TD5_LOG_E(NET_LOG, "Host: failed to bind game port %u", (unsigned)s_game_port);
        return 0;
    }

    memset(&s_ws2_host_addr, 0, sizeof(s_ws2_host_addr));
    s_ws2_host_addr.sin_family = AF_INET;
    s_ws2_host_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    s_ws2_host_addr.sin_port = htons((u_short)s_game_port);

    s_ws2_peer_addrs[0] = s_ws2_host_addr;
    s_ws2_peer_valid[0] = 1;

    /* LAN discovery beacon socket (session LISTING only -- the actual join now
       rides the game socket). Bound in both modes; harmless in Direct. */
    ws2_discovery_init();

    /* Our LAN IP, for display + the UPnP InternalClient value. */
    if (!td5_upnp_get_local_ip(s_local_ip, sizeof(s_local_ip)))
        s_local_ip[0] = '\0';

    /* S10: optional UPnP IGD port-mapping so a Direct-IP host is reachable
       across NAT without manual port forwarding. Best-effort; never fatal. */
    if (s_enable_upnp) {
        s_upnp_status = TD5_NET_UPNP_MAPPING;
        if (td5_upnp_map_port((uint16_t)s_game_port, 1 /*UDP*/, "TD5RE",
                              0 /*permanent lease*/) &&
            td5_upnp_verify_port((uint16_t)s_game_port, 1)) {
            s_upnp_status = TD5_NET_UPNP_MAPPED;
            s_upnp_mapped_port = (uint16_t)s_game_port;
            /* [2026-06-16] Double-NAT check. The mapping was accepted by the
             * nearest IGD, but if that router's OWN WAN IP is private (RFC1918
             * 10/8, 172.16/12, 192.168/16) or CGNAT (100.64/10), it sits behind
             * ANOTHER router (e.g. a TP-Link Deco behind a Movistar fiber modem)
             * -- so the port we just opened is NOT reachable from the internet.
             * UPnP only opens ONE hop; the upstream router needs its own forward.
             * Surface it as a distinct status instead of a misleading "opened". */
            if (td5_upnp_get_external_ip(s_external_ip, sizeof(s_external_ip)) &&
                net_ip_is_private(s_external_ip)) {
                s_upnp_status = TD5_NET_UPNP_DOUBLE_NAT;
                TD5_LOG_W(NET_LOG, "UPnP: UDP %u opened on router, but its WAN IP "
                          "%s is private -> double-NAT; forward on the upstream router too",
                          (unsigned)s_game_port, s_external_ip);
            } else {
                TD5_LOG_I(NET_LOG, "UPnP: UDP %u opened + verified on router (WAN %s)",
                          (unsigned)s_game_port,
                          s_external_ip[0] ? s_external_ip : "?");
            }
        } else if (td5_upnp_found_igd()) {
            /* The router answered SSDP but refused the mapping. Distinguish a
             * port collision (714/718 -- the port is already forwarded, usually
             * a leftover static rule in the router UI) from a generic refusal,
             * so the status text stops claiming "no router/IGD". */
            int f = td5_upnp_last_map_fault();
            if (f == 718 || f == 714 || f == 501) {
                s_upnp_status = TD5_NET_UPNP_PORT_CONFLICT;
                TD5_LOG_W(NET_LOG, "UPnP: UDP %u already forwarded on the router "
                          "(fault %d) -- remove that rule or pick another port",
                          (unsigned)s_game_port, f);
            } else {
                s_upnp_status = TD5_NET_UPNP_FAILED;
                TD5_LOG_W(NET_LOG, "UPnP: router refused UDP %u mapping (fault %d) "
                          "-- forward manually", (unsigned)s_game_port, f);
            }
        } else {
            s_upnp_status = TD5_NET_UPNP_FAILED;
            TD5_LOG_W(NET_LOG, "UPnP: no UPnP router found for UDP %u (manual forward needed)",
                      (unsigned)s_game_port);
        }
    } else {
        s_upnp_status = TD5_NET_UPNP_UNAVAILABLE;
    }

    TD5_LOG_I(NET_LOG, "Host: bound game port %u (mode=%s ip=%s)",
              (unsigned)s_game_port,
              s_conn_mode == TD5_NET_MODE_DIRECT ? "direct" : "lan",
              s_local_ip[0] ? s_local_ip : "?");
    return 1;
}

/* LAN join by enumerated session index: point at the discovered host's game
 * address and bind an ephemeral local game socket. The JOIN_REQ itself is sent
 * (and retried) by td5_net_join_session via ws2_send_join_request(). */
static int ws2_transport_join(int session_index)
{
    if (session_index < 0 || session_index >= s_enum_session_count) {
        TD5_LOG_E(NET_LOG, "Join: invalid session index %d", session_index);
        return 0;
    }

    if (!ws2_bind_socket(0)) {
        TD5_LOG_E(NET_LOG, "Join: failed to bind game socket");
        return 0;
    }

    s_ws2_host_addr = s_ws2_enum_host_addrs[session_index];
    s_ws2_peer_addrs[0] = s_ws2_host_addr;
    s_ws2_peer_valid[0] = 1;

    TD5_LOG_I(NET_LOG, "Join(LAN): host %s:%u",
              inet_ntoa(s_ws2_host_addr.sin_addr),
              (unsigned)ntohs(s_ws2_host_addr.sin_port));
    return 1;
}

/* Direct join by explicit IP[:port]: resolve, point at the host's game address,
 * and bind an ephemeral local game socket. JOIN_REQ sent by td5_net_join_direct. */
static int ws2_transport_join_direct(const char *host_ip, int game_port)
{
    SOCKADDR_IN addr;

    if (!host_ip || !host_ip[0])
        return 0;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)game_port);
    addr.sin_addr.s_addr = inet_addr(host_ip);
    if (addr.sin_addr.s_addr == INADDR_NONE) {
        struct addrinfo hints, *res = NULL;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        if (getaddrinfo(host_ip, NULL, &hints, &res) != 0 || !res) {
            TD5_LOG_E(NET_LOG, "Join(direct): cannot resolve '%s'", host_ip);
            return 0;
        }
        addr.sin_addr = ((SOCKADDR_IN *)res->ai_addr)->sin_addr;
        freeaddrinfo(res);
    }

    if (!ws2_bind_socket(0)) {
        TD5_LOG_E(NET_LOG, "Join(direct): failed to bind game socket");
        return 0;
    }

    s_ws2_host_addr = addr;
    s_ws2_peer_addrs[0] = s_ws2_host_addr;
    s_ws2_peer_valid[0] = 1;

    TD5_LOG_I(NET_LOG, "Join(direct): host %s:%u", host_ip, (unsigned)game_port);
    return 1;
}

/* Broadcast the discovery QUERY (LAN broadcast + loopback for same-machine). */
static int ws2_browse_init(void)
{
    SOCKADDR_IN bind_addr;
    BOOL opt_broadcast = TRUE;
    u_long nonblocking = 1;

    if (s_ws2_browse_socket != INVALID_SOCKET)
        return 1;
    s_ws2_browse_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_ws2_browse_socket == INVALID_SOCKET)
        return 0;
    setsockopt(s_ws2_browse_socket, SOL_SOCKET, SO_BROADCAST,
               (const char *)&opt_broadcast, sizeof(opt_broadcast));
    ioctlsocket(s_ws2_browse_socket, FIONBIO, &nonblocking);
    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    bind_addr.sin_port = 0;   /* ephemeral: replies come back uniquely to us */
    if (bind(s_ws2_browse_socket, (const struct sockaddr *)&bind_addr,
             (int)sizeof(bind_addr)) == SOCKET_ERROR) {
        closesocket(s_ws2_browse_socket);
        s_ws2_browse_socket = INVALID_SOCKET;
        return 0;
    }
    return 1;
}

static void ws2_send_disc_query(void)
{
    SOCKADDR_IN bcast_addr, lo_addr;
    DiscoveryMsg query;
    if (!ws2_browse_init())
        return;
    memset(&query, 0, sizeof(query));
    query.magic = WS2_DISCOVERY_MAGIC;
    query.disc_type = WS2_DISC_QUERY;

    memset(&bcast_addr, 0, sizeof(bcast_addr));
    bcast_addr.sin_family = AF_INET;
    bcast_addr.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    bcast_addr.sin_port = htons(WS2_DISCOVERY_PORT);
    sendto(s_ws2_browse_socket, (const char *)&query, sizeof(query), 0,
           (const struct sockaddr *)&bcast_addr, (int)sizeof(bcast_addr));

    memset(&lo_addr, 0, sizeof(lo_addr));
    lo_addr.sin_family = AF_INET;
    lo_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    lo_addr.sin_port = htons(WS2_DISCOVERY_PORT);
    sendto(s_ws2_browse_socket, (const char *)&query, sizeof(query), 0,
           (const struct sockaddr *)&lo_addr, (int)sizeof(lo_addr));
}

/* [PERF 2026-06-06] NON-BLOCKING incremental LAN discovery.
 *
 * The old version blocked the calling frame up to 500ms (a select-poll loop) — a
 * visible freeze on the SessionPicker entry frame, and the full 500ms when no host
 * answered (the common single-player case). Now the frontend calls this every frame
 * while the LAN browser is open and it never blocks: it (re)opens a ~2s discovery
 * window on first use / after a >1.5s idle gap, re-broadcasts the QUERY every 250ms
 * within that window so late-starting hosts are still found, and drains whatever has
 * arrived since the last call with a zero-timeout select. The session list fills in
 * live. Sessions are deduped by host address+port (the periodic rebroadcast makes a
 * host answer many times per window). Returns the current discovered count. */
static DWORD s_ws2_enum_win_start = 0;   /* GetTickCount of the active window, 0 = none */
static DWORD s_ws2_enum_last_bcast = 0;
static DWORD s_ws2_enum_last_call  = 0;
static DWORD s_ws2_enum_seen[MAX_ENUM_SESSIONS];  /* [S31] last ANNOUNCE per entry */

static int ws2_transport_enum(void)
{
    DWORD now;
    int i;

    if (!ws2_browse_init())
        return 0;
    now = GetTickCount();

    if (s_ws2_enum_win_start == 0 || (now - s_ws2_enum_last_call) > 1500) {
        /* Fresh open / refresh: reset the list and start a new discovery window. */
        s_enum_session_count = 0;
        memset(s_enum_sessions, 0, sizeof(s_enum_sessions));
        memset(s_ws2_enum_host_addrs, 0, sizeof(s_ws2_enum_host_addrs));
        s_ws2_enum_win_start = now;
        ws2_send_disc_query();
        s_ws2_enum_last_bcast = now;
    } else if ((now - s_ws2_enum_last_bcast) >= 250 &&
               (now - s_ws2_enum_win_start) < 2000) {
        ws2_send_disc_query();
        s_ws2_enum_last_bcast = now;
    } else if ((now - s_ws2_enum_last_bcast) >= 3000) {
        /* [S31] Steady-state auto-refresh: the browser stays open, so keep
         * asking every ~3 s -- a host that starts AFTER the window closed
         * still shows up without leaving and re-entering the screen. */
        ws2_send_disc_query();
        s_ws2_enum_last_bcast = now;
    }
    s_ws2_enum_last_call = now;

    /* Drain all responses currently waiting — NON-BLOCKING (zero-timeout select). */
    for (;;) {
        DiscoveryMsg resp;
        SOCKADDR_IN from_addr;
        int from_len = (int)sizeof(from_addr);
        int ret, dup, slot;
        fd_set readfds;
        struct timeval tv;

        FD_ZERO(&readfds);
        FD_SET(s_ws2_browse_socket, &readfds);
        tv.tv_sec = 0;
        tv.tv_usec = 0;
        if (select(0, &readfds, NULL, NULL, &tv) <= 0)
            break;

        ret = recvfrom(s_ws2_browse_socket, (char *)&resp, sizeof(resp), 0,
                       (struct sockaddr *)&from_addr, &from_len);
        if (ret < (int)sizeof(DiscoveryMsg)) continue;
        if (resp.magic != WS2_DISCOVERY_MAGIC) continue;
        if (resp.disc_type != WS2_DISC_ANNOUNCE) continue;
        if (resp.sealed) continue;

        /* Dedup by host address+game port (a host answers each rebroadcast).
         * A SAME-MACHINE host answers the browse query twice -- once via the
         * broadcast (reply sourced from the LAN adapter IP) and once via the
         * explicit loopback send (reply from 127.0.0.1) -- so an announce
         * whose source or stored twin is loopback with the same name+port is
         * the same session, not a second one. */
        dup = 0; slot = s_enum_session_count;
        for (i = 0; i < s_enum_session_count; i++) {
            int same_addr =
                (s_ws2_enum_host_addrs[i].sin_addr.s_addr == from_addr.sin_addr.s_addr &&
                 s_ws2_enum_host_addrs[i].sin_port == htons(resp.game_port));
            int loop_pair =
                ((s_ws2_enum_host_addrs[i].sin_addr.s_addr == htonl(INADDR_LOOPBACK) ||
                  from_addr.sin_addr.s_addr == htonl(INADDR_LOOPBACK)) &&
                 s_ws2_enum_host_addrs[i].sin_port == htons(resp.game_port) &&
                 strncmp(s_enum_sessions[i].name, resp.session_name,
                         sizeof(s_enum_sessions[i].name) - 1) == 0);
            if (same_addr || loop_pair) { slot = i; dup = 1; break; }
        }
        if (!dup && slot >= MAX_ENUM_SESSIONS) continue;   /* list full */

        strncpy(s_enum_sessions[slot].name, resp.session_name,
                sizeof(s_enum_sessions[slot].name) - 1);
        s_enum_sessions[slot].name[sizeof(s_enum_sessions[slot].name) - 1] = '\0';
        s_enum_sessions[slot].player_count = resp.player_count;
        s_enum_sessions[slot].max_players  = resp.max_players;
        s_enum_sessions[slot].game_type    = resp.game_type;
        s_ws2_enum_host_addrs[slot] = from_addr;
        s_ws2_enum_host_addrs[slot].sin_port = htons(resp.game_port);
        s_ws2_enum_seen[slot] = now;

        if (!dup) {
            TD5_LOG_I(NET_LOG, "Enum: found session \"%s\" (%u/%u)",
                      resp.session_name, resp.player_count, resp.max_players);
            s_enum_session_count++;
        }
    }

    /* [S31] Expire entries whose host stopped answering (~3 missed refresh
     * cycles) so a closed session drops off the auto-refreshing list. Only
     * meaningful in the steady state -- during the initial window every
     * entry is younger than the threshold anyway. */
    for (i = 0; i < s_enum_session_count; ) {
        if ((now - s_ws2_enum_seen[i]) > 10000) {
            int k;
            TD5_LOG_I(NET_LOG, "Enum: session \"%s\" expired (host silent)",
                      s_enum_sessions[i].name);
            for (k = i; k < s_enum_session_count - 1; k++) {
                s_enum_sessions[k]       = s_enum_sessions[k + 1];
                s_ws2_enum_host_addrs[k] = s_ws2_enum_host_addrs[k + 1];
                s_ws2_enum_seen[k]       = s_ws2_enum_seen[k + 1];
            }
            s_enum_session_count--;
        } else {
            i++;
        }
    }
    return s_enum_session_count;
}

static uint32_t ws2_resolve_sender_id(const SOCKADDR_IN *addr)
{
    int i;

    if (!addr) {
        return 0;
    }

    /* First pass: exact match on existing peer addresses */
    for (i = 0; i < TD5_NET_MAX_PLAYERS; i++) {
        if (!s_ws2_peer_valid[i]) {
            continue;
        }
        if (s_ws2_peer_addrs[i].sin_port == addr->sin_port &&
            s_ws2_peer_addrs[i].sin_addr.s_addr == addr->sin_addr.s_addr) {
            if (s_roster[i].id != 0) {
                return s_roster[i].id;
            }
            return (uint32_t)(i + 1);
        }
    }

    /* Check against host address */
    if (addr->sin_port == s_ws2_host_addr.sin_port &&
        addr->sin_addr.s_addr == s_ws2_host_addr.sin_addr.s_addr) {
        return 1;
    }

    /* Second pass (host only): match by IP only -- the peer's game-socket port
       may differ from the discovery-socket port we stored during join handling.
       Update the stored port to the actual game-socket port when found. */
    if (s_is_host) {
        for (i = 1; i < TD5_NET_MAX_PLAYERS; i++) {
            if (!s_ws2_peer_valid[i] || !s_roster[i].active)
                continue;
            if (s_ws2_peer_addrs[i].sin_addr.s_addr == addr->sin_addr.s_addr) {
                /* Update port to the actual game-socket port */
                s_ws2_peer_addrs[i].sin_port = addr->sin_port;
                TD5_LOG_D(NET_LOG, "Updated peer slot %d port to %u",
                          i, (unsigned)ntohs(addr->sin_port));
                if (s_roster[i].id != 0) {
                    return s_roster[i].id;
                }
                return (uint32_t)(i + 1);
            }
        }
    }

    return 0;
}

static int ws2_transport_recv(uint32_t *sender_id, void *buf, int buf_size)
{
    SOCKADDR_IN from_addr;
    int from_len;
    int ret;

    if (s_ws2_socket == INVALID_SOCKET || !buf || buf_size <= 0 || !s_ws2_socket_bound) {
        return 0;
    }

    for (;;) {
        from_len = (int)sizeof(from_addr);
        ret = recvfrom(s_ws2_socket, (char *)buf, buf_size, 0,
                       (struct sockaddr *)&from_addr, &from_len);
        if (ret == SOCKET_ERROR) {
            int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK) {
                return 0;
            }
            TD5_LOG_W(NET_LOG, "recvfrom failed: %d", err);
            if (err == WSAECONNRESET) {
                s_connection_lost = 1;
            }
            return 0;
        }

        /* S10: the JOIN handshake rides the GAME socket (so it crosses routers
           via the UPnP-opened port), tagged with WS2_DISCOVERY_MAGIC so it is
           distinguishable from a DXPTYPE (0-12) message BEFORE dispatch. The
           lockstep wire protocol is untouched. Consume it here and keep
           draining the next datagram. */
        if (ret >= 8 && *(const uint32_t *)buf == WS2_DISCOVERY_MAGIC) {
            ws2_handle_game_control(buf, ret, &from_addr);
            continue;
        }

        /* [SEC 2026-06-15] DXPTYPE traffic is wrapped in a per-session token
         * envelope [4B token][message]. Validate + strip it before anything
         * else: an off-path blind spoofer that never saw the JOIN handshake
         * cannot guess the random token, so it can't forge frames even knowing
         * the endpoint addresses. (On-path sniffers can read the token off the
         * wire -- out of scope; that needs per-packet crypto.) The token is not
         * known in the pre-join window (== 0) -> fall back to the address check
         * alone so legitimate early packets aren't dropped. */
        if (ret < (int)(NET_TOKEN_SIZE + 4))
            continue;                       /* too short for a token + a type */
        {
            uint32_t pkt_token;
            memcpy(&pkt_token, buf, NET_TOKEN_SIZE);
            if (s_session_token != 0 && pkt_token != s_session_token) {
                TD5_LOG_W(NET_LOG, "Dropped DXPTYPE with bad session token");
                continue;
            }
            memmove(buf, (uint8_t *)buf + NET_TOKEN_SIZE,
                    (size_t)(ret - NET_TOKEN_SIZE));
            ret -= NET_TOKEN_SIZE;
        }

        /* [SEC 2026-06-15] Source-validate lockstep DXPTYPE traffic before it
         * reaches any handler. The protocol is star-topology + host-mediated,
         * so the ONLY legitimate source is:
         *   - on a client: the host's address;
         *   - on the host: a known, joined peer (resolved id != 0).
         * Dropping anything else stops a spoofed datagram from any other host
         * on the network from injecting frames / DISCONNECT / ROSTER / RESYNC
         * (trivial remote kick / roster-hijack / desync). UDP source addresses
         * are still forgeable off-path -- a per-session token would be the
         * stronger guarantee -- but this closes the unauthenticated LAN/any-host
         * case at one choke point covering every DXPTYPE handler. */
        {
            uint32_t sid = ws2_resolve_sender_id(&from_addr);
            if (s_is_host) {
                if (sid == 0) {
                    TD5_LOG_W(NET_LOG, "Dropped DXPTYPE from unknown source");
                    continue;
                }
            } else if (from_addr.sin_addr.s_addr != s_ws2_host_addr.sin_addr.s_addr ||
                       from_addr.sin_port != s_ws2_host_addr.sin_port) {
                TD5_LOG_W(NET_LOG, "Dropped DXPTYPE from non-host source");
                continue;
            }
            if (sender_id) {
                *sender_id = sid;
            }
            return ret;
        }
    }
}

static const SOCKADDR_IN *ws2_get_target_addr(uint32_t target_id)
{
    int slot;

    if (target_id == 0) {
        return NULL;
    }

    slot = find_slot_by_id(target_id);
    if (slot >= 0 && slot < TD5_NET_MAX_PLAYERS && s_ws2_peer_valid[slot]) {
        return &s_ws2_peer_addrs[slot];
    }

    if (!s_is_host) {
        return &s_ws2_host_addr;
    }

    return NULL;
}

static int ws2_transport_send(uint32_t target_id, const void *data, int size)
{
    const SOCKADDR_IN *target_addr;
    int sent;
    int i;
    uint8_t pkt[RING_ENTRY_SIZE + NET_TOKEN_SIZE];
    int     pkt_len;

    if (s_ws2_socket == INVALID_SOCKET || !data || size <= 0 || !s_ws2_socket_bound) {
        return 0;
    }

    /* [SEC 2026-06-15] Wrap the message in the per-session token envelope
     * [4B token][message]; ws2_transport_recv validates + strips it. Senders
     * always hold a non-zero token by the time they emit DXPTYPE traffic (host:
     * from session create; client: from JOIN_ACK). */
    if (size > (int)sizeof(pkt) - NET_TOKEN_SIZE) {
        TD5_LOG_W(NET_LOG, "send size %d exceeds token envelope", size);
        return 0;
    }
    memcpy(pkt, &s_session_token, NET_TOKEN_SIZE);
    memcpy(pkt + NET_TOKEN_SIZE, data, (size_t)size);
    pkt_len = size + NET_TOKEN_SIZE;

    if (target_id == 0) {
        if (s_is_host) {
            int sent_any = 0;
            for (i = 0; i < TD5_NET_MAX_PLAYERS; i++) {
                if (i == s_local_slot || !s_roster[i].active || !s_ws2_peer_valid[i]) {
                    continue;
                }
                sent = sendto(s_ws2_socket, (const char *)pkt, pkt_len, 0,
                              (const struct sockaddr *)&s_ws2_peer_addrs[i],
                              (int)sizeof(s_ws2_peer_addrs[i]));
                if (sent == SOCKET_ERROR) {
                    TD5_LOG_W(NET_LOG, "sendto(broadcast slot=%d) failed: %d", i, (int)WSAGetLastError());
                    continue;
                }
                sent_any = 1;
            }
            return sent_any ? 1 : 0;
        }

        target_addr = &s_ws2_host_addr;
    } else {
        target_addr = ws2_get_target_addr(target_id);
    }

    if (!target_addr) {
        return 0;
    }

    sent = sendto(s_ws2_socket, (const char *)pkt, pkt_len, 0,
                  (const struct sockaddr *)target_addr, (int)sizeof(*target_addr));
    if (sent == SOCKET_ERROR) {
        TD5_LOG_W(NET_LOG, "sendto(target=%u) failed: %d", target_id, (int)WSAGetLastError());
        return 0;
    }

    return 1;
}

static void ws2_transport_shutdown(void)
{
    ws2_discovery_shutdown();

    if (s_ws2_socket != INVALID_SOCKET) {
        closesocket(s_ws2_socket);
        s_ws2_socket = INVALID_SOCKET;
    }
    if (s_ws2_event != WSA_INVALID_EVENT) {
        WSACloseEvent(s_ws2_event);
        s_ws2_event = WSA_INVALID_EVENT;
    }
    if (s_ws2_started) {
        WSACleanup();
        s_ws2_started = 0;
    }

    s_evt_receive = NULL;
    s_receive_event_is_ws2 = 0;
    s_ws2_socket_bound = 0;
    memset(&s_ws2_host_addr, 0, sizeof(s_ws2_host_addr));
    memset(s_ws2_peer_addrs, 0, sizeof(s_ws2_peer_addrs));
    memset(s_ws2_peer_valid, 0, sizeof(s_ws2_peer_valid));
    memset(s_ws2_enum_host_addrs, 0, sizeof(s_ws2_enum_host_addrs));
}

/* ========================================================================
 * Discovery Socket + Join Request Handler
 * ======================================================================== */

static int ws2_discovery_init(void)
{
    SOCKADDR_IN bind_addr;
    BOOL opt_broadcast = TRUE;
    BOOL opt_reuse = TRUE;
    u_long nonblocking = 1;

    if (s_ws2_disc_socket != INVALID_SOCKET)
        return 1;

    s_ws2_disc_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_ws2_disc_socket == INVALID_SOCKET) {
        TD5_LOG_E(NET_LOG, "Discovery: socket() failed: %d", (int)WSAGetLastError());
        return 0;
    }

    setsockopt(s_ws2_disc_socket, SOL_SOCKET, SO_BROADCAST,
               (const char *)&opt_broadcast, sizeof(opt_broadcast));
    setsockopt(s_ws2_disc_socket, SOL_SOCKET, SO_REUSEADDR,
               (const char *)&opt_reuse, sizeof(opt_reuse));
    ioctlsocket(s_ws2_disc_socket, FIONBIO, &nonblocking);

    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    bind_addr.sin_port = htons(WS2_DISCOVERY_PORT);

    if (bind(s_ws2_disc_socket, (const struct sockaddr *)&bind_addr,
             sizeof(bind_addr)) == SOCKET_ERROR)
    {
        TD5_LOG_W(NET_LOG, "Discovery: bind(%u) failed: %d (non-fatal for clients)",
                  (unsigned)WS2_DISCOVERY_PORT, (int)WSAGetLastError());
    }

    TD5_LOG_I(NET_LOG, "Discovery socket initialized on port %u",
              (unsigned)WS2_DISCOVERY_PORT);
    return 1;
}

static void ws2_discovery_shutdown(void)
{
    if (s_ws2_disc_socket != INVALID_SOCKET) {
        closesocket(s_ws2_disc_socket);
        s_ws2_disc_socket = INVALID_SOCKET;
    }
    if (s_ws2_browse_socket != INVALID_SOCKET) {
        closesocket(s_ws2_browse_socket);
        s_ws2_browse_socket = INVALID_SOCKET;
    }
}

/** Host: tell a joiner we rejected them (session full / wrong password). */
static void ws2_send_join_nak(const SOCKADDR_IN *from_addr, int reason)
{
    DiscoveryMsg nak;
    if (!from_addr || s_ws2_socket == INVALID_SOCKET) return;
    memset(&nak, 0, sizeof(nak));
    nak.magic = WS2_DISCOVERY_MAGIC;
    nak.disc_type = WS2_DISC_JOIN_NAK;
    nak.nak_reason = (uint32_t)reason;
    sendto(s_ws2_socket, (const char *)&nak, sizeof(nak), 0,
           (const struct sockaddr *)from_addr, (int)sizeof(*from_addr));
}

/**
 * Host-side: accept (or re-ACK) a join from `from_addr`, sending JOIN_ACK back
 * over `reply_socket`. Enforces the join password + the max-players limit
 * (JOIN_NAK on rejection). Idempotent so a retried JOIN_REQ is harmless.
 */
static void ws2_accept_join(const SOCKADDR_IN *from_addr, const char *name,
                            const char *password, SOCKET reply_socket)
{
    int slot = -1, active_count = 0;
    int i;
    DiscoveryMsg ack;

    if (!s_is_host || !from_addr)
        return;
    if (s_session.sealed) {
        ws2_send_join_nak(from_addr, WS2_NAK_FULL);
        return;
    }

    /* Already-known peer (a retried JOIN_REQ)? Re-use its slot, just re-ACK. */
    for (i = 1; i < TD5_NET_MAX_PLAYERS; i++) {
        if (s_ws2_peer_valid[i] && s_roster[i].active &&
            s_ws2_peer_addrs[i].sin_addr.s_addr == from_addr->sin_addr.s_addr &&
            s_ws2_peer_addrs[i].sin_port == from_addr->sin_port) {
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        /* New joiner: password gate, then capacity gate. */
        if (s_host_password[0] &&
            (!password || strncmp(password, s_host_password, sizeof(s_host_password)) != 0)) {
            TD5_LOG_W(NET_LOG, "Join rejected: wrong password");
            ws2_send_join_nak(from_addr, WS2_NAK_PASSWORD);
            return;
        }
        for (i = 0; i < TD5_NET_MAX_PLAYERS; i++)
            if (s_roster[i].active) active_count++;
        if (active_count >= (int)s_session.max_players) {
            TD5_LOG_W(NET_LOG, "Join rejected: session full (%d/%u)",
                      active_count, s_session.max_players);
            ws2_send_join_nak(from_addr, WS2_NAK_FULL);
            return;
        }
        for (i = 1; i < TD5_NET_MAX_PLAYERS; i++) {
            if (!s_roster[i].active) { slot = i; break; }
        }
    }

    if (slot < 0) {
        ws2_send_join_nak(from_addr, WS2_NAK_FULL);
        return;
    }

    if (!s_roster[slot].active) {
        s_roster[slot].id = (uint32_t)(slot + 1);
        s_roster[slot].active = 1;
        if (name && name[0])
            snprintf(s_roster[slot].name, sizeof(s_roster[slot].name), "%s", name);
        else
            snprintf(s_roster[slot].name, sizeof(s_roster[slot].name), "Player %d", slot + 1);
        s_slot_latency[slot] = 0;
        s_player_count++;
        InterlockedIncrement(&s_sync_generation);
        TD5_LOG_I(NET_LOG, "Join accepted: slot=%d id=%u name=\"%s\"",
                  slot, s_roster[slot].id, s_roster[slot].name);
    }

    s_ws2_peer_addrs[slot] = *from_addr;
    s_ws2_peer_valid[slot] = 1;

    memset(&ack, 0, sizeof(ack));
    ack.magic = WS2_DISCOVERY_MAGIC;
    ack.disc_type = WS2_DISC_JOIN_ACK;
    ack.assigned_slot = (uint32_t)slot;
    ack.assigned_id   = s_roster[slot].id;
    ack.game_port     = (uint16_t)s_game_port;
    ack.session_token = s_session_token;   /* [SEC] only sent after the pw gate */
    strncpy(ack.session_name, s_session.name, sizeof(ack.session_name) - 1);

    if (reply_socket != INVALID_SOCKET) {
        sendto(reply_socket, (const char *)&ack, sizeof(ack), 0,
               (const struct sockaddr *)from_addr, (int)sizeof(*from_addr));
    }

    broadcast_roster();
    ws2_broadcast_roster_info();
}

/** Legacy discovery-socket join entry; delegates to the shared accept path. */
static void ws2_handle_join_request(const SOCKADDR_IN *from_addr)
{
    ws2_accept_join(from_addr, NULL, NULL, s_ws2_disc_socket);
}

/** Host: broadcast per-slot names + latency so clients can render the roster. */
static void ws2_broadcast_roster_info(void)
{
    RosterInfoMsg info;
    int i;
    if (!s_is_host || s_ws2_socket == INVALID_SOCKET) return;
    memset(&info, 0, sizeof(info));
    info.magic = WS2_DISCOVERY_MAGIC;
    info.disc_type = WS2_DISC_ROSTER_INFO;
    info.host_slot = (uint32_t)s_local_slot;
    for (i = 0; i < TD5_NET_MAX_PLAYERS; i++) {
        info.active[i] = s_roster[i].active ? 1u : 0u;
        info.latency_ms[i] = (s_slot_latency[i] >= 0) ? (uint32_t)s_slot_latency[i] : 0u;
        snprintf(info.names[i], sizeof(info.names[i]), "%s", s_roster[i].name);
    }
    for (i = 0; i < TD5_NET_MAX_PLAYERS; i++) {
        if (i == s_local_slot || !s_roster[i].active || !s_ws2_peer_valid[i]) continue;
        sendto(s_ws2_socket, (const char *)&info, sizeof(info), 0,
               (const struct sockaddr *)&s_ws2_peer_addrs[i],
               (int)sizeof(s_ws2_peer_addrs[i]));
    }
}

/** Host: send an RTT PING (token = now) to each active client. */
static void ws2_send_pings(void)
{
    PingMsg ping;
    int i;
    if (!s_is_host || s_ws2_socket == INVALID_SOCKET) return;
    memset(&ping, 0, sizeof(ping));
    ping.magic = WS2_DISCOVERY_MAGIC;
    ping.disc_type = WS2_DISC_PING;
    ping.token = td5_plat_time_ms();
    for (i = 0; i < TD5_NET_MAX_PLAYERS; i++) {
        if (i == s_local_slot || !s_roster[i].active || !s_ws2_peer_valid[i]) continue;
        sendto(s_ws2_socket, (const char *)&ping, sizeof(ping), 0,
               (const struct sockaddr *)&s_ws2_peer_addrs[i],
               (int)sizeof(s_ws2_peer_addrs[i]));
    }
}

/**
 * Dispatch a magic-tagged control datagram received on the GAME socket: the
 * JOIN handshake (REQ/ACK/NAK) plus the lobby PING/PONG + ROSTER_INFO channel.
 */
static void ws2_handle_game_control(const void *buf, int size, const SOCKADDR_IN *from_addr)
{
    uint32_t type;
    if (!buf || !from_addr || size < 8)
        return;
    type = ((const uint32_t *)buf)[1];   /* disc_type follows the magic */

    switch (type) {
    case WS2_DISC_JOIN_REQ:
        /* Host: a client wants in (player name + password in the payload). */
        if (s_is_host && size >= (int)sizeof(DiscoveryMsg)) {
            const DiscoveryMsg *dm = (const DiscoveryMsg *)buf;
            char name[64], pw[32];
            strncpy(name, dm->session_name, sizeof(name) - 1); name[sizeof(name) - 1] = '\0';
            strncpy(pw, dm->password, sizeof(pw) - 1);         pw[sizeof(pw) - 1] = '\0';
            ws2_accept_join(from_addr, name, pw, s_ws2_socket);
        }
        break;

    case WS2_DISC_JOIN_ACK:
        /* Client: the host assigned our slot/id and we now know its game addr. */
        if (!s_is_host && size >= (int)sizeof(DiscoveryMsg)) {
            const DiscoveryMsg *dm = (const DiscoveryMsg *)buf;
            int slot = (int)dm->assigned_slot;
            if (slot >= 0 && slot < TD5_NET_MAX_PLAYERS) {
                s_join_nak_reason = 0;
                s_local_slot = slot;
                s_session_token = dm->session_token;   /* [SEC] adopt the session token */
                s_roster[slot].id = dm->assigned_id ? dm->assigned_id : (uint32_t)(slot + 1);
                s_roster[slot].active = 1;
                if (!s_roster[0].active) { s_roster[0].id = 1; s_roster[0].active = 1; }
                s_ws2_host_addr = *from_addr;
                s_ws2_peer_addrs[0] = *from_addr;
                s_ws2_peer_valid[0] = 1;
                TD5_LOG_I(NET_LOG, "JOIN_ACK: slot %d id %u (host %s:%u)",
                          slot, s_roster[slot].id, inet_ntoa(from_addr->sin_addr),
                          (unsigned)ntohs(from_addr->sin_port));
            }
        }
        break;

    case WS2_DISC_JOIN_NAK:
        /* Client: rejected (wrong password / full). The UI re-prompts/back-offs. */
        if (!s_is_host && size >= (int)sizeof(DiscoveryMsg)) {
            const DiscoveryMsg *dm = (const DiscoveryMsg *)buf;
            s_join_nak_reason = (int)dm->nak_reason;
            s_join_pending = 0;
            TD5_LOG_W(NET_LOG, "JOIN_NAK: reason=%d", s_join_nak_reason);
        }
        break;

    case WS2_DISC_PING:
        /* Client: echo the host's RTT probe back as PONG (same token). */
        if (size >= (int)sizeof(PingMsg)) {
            PingMsg pong = *(const PingMsg *)buf;
            pong.disc_type = WS2_DISC_PONG;
            sendto(s_ws2_socket, (const char *)&pong, sizeof(pong), 0,
                   (const struct sockaddr *)from_addr, (int)sizeof(*from_addr));
        }
        break;

    case WS2_DISC_CAR_INFO:
        /* S31: a client announced its car/paint pick. Host-only. */
        if (s_is_host && size >= (int)sizeof(CarInfoMsg)) {
            const CarInfoMsg *cm = (const CarInfoMsg *)buf;
            if (cm->slot < TD5_NET_MAX_PLAYERS) {
                s_slot_car[cm->slot]       = cm->car;
                s_slot_paint[cm->slot]     = cm->paint;
                s_slot_td6_color[cm->slot] = cm->td6_color;
                TD5_LOG_I(NET_LOG, "CAR_INFO: slot %u car=%d paint=%d color=%06X",
                          cm->slot, cm->car, cm->paint, (unsigned)cm->td6_color);
            }
        }
        break;

    case WS2_DISC_PONG:
        /* Host: RTT = now - token for the slot this address belongs to. */
        if (s_is_host && size >= (int)sizeof(PingMsg)) {
            const PingMsg *pm = (const PingMsg *)buf;
            int slot = find_slot_by_id(ws2_resolve_sender_id(from_addr));
            if (slot >= 0 && slot < TD5_NET_MAX_PLAYERS) {
                uint32_t rtt = td5_plat_time_ms() - pm->token;
                s_slot_latency[slot] = (rtt > 5000) ? 5000 : (int)rtt;
            }
        }
        break;

    case WS2_DISC_ROSTER_INFO:
        /* Client: adopt the host's per-slot names + latency for the lobby UI. */
        if (!s_is_host && size >= (int)sizeof(RosterInfoMsg)) {
            const RosterInfoMsg *ri = (const RosterInfoMsg *)buf;
            int i;
            for (i = 0; i < TD5_NET_MAX_PLAYERS; i++) {
                s_roster[i].active = ri->active[i] ? 1 : 0;
                s_slot_latency[i] = (int)ri->latency_ms[i];
                /* [SEC 2026-06-15] Bound the read: ri->names[i] is wire data
                 * and may not be NUL-terminated within its 32 bytes; a bare
                 * "%s" would over-read past the field/struct/stack recv buffer.
                 * The precision caps the source scan at the field width. */
                snprintf(s_slot_name[i], sizeof(s_slot_name[i]), "%.*s",
                         (int)sizeof(ri->names[i]) - 1, ri->names[i]);
                snprintf(s_roster[i].name, sizeof(s_roster[i].name), "%.*s",
                         (int)sizeof(ri->names[i]) - 1, ri->names[i]);
            }
        }
        break;

    default:
        break;
    }
}

/** Client-side: send a magic JOIN_REQ (player name + password) to the host. */
static int ws2_send_join_request(const char *player_name)
{
    DiscoveryMsg req;

    if (s_ws2_socket == INVALID_SOCKET || !s_ws2_socket_bound)
        return 0;

    memset(&req, 0, sizeof(req));
    req.magic = WS2_DISCOVERY_MAGIC;
    req.disc_type = WS2_DISC_JOIN_REQ;
    if (player_name && player_name[0])
        strncpy(req.session_name, player_name, sizeof(req.session_name) - 1);
    strncpy(req.password, s_join_password, sizeof(req.password) - 1);

    return sendto(s_ws2_socket, (const char *)&req, sizeof(req), 0,
                  (const struct sockaddr *)&s_ws2_host_addr,
                  (int)sizeof(s_ws2_host_addr)) != SOCKET_ERROR;
}

/* ========================================================================
 * Resync Barrier Implementation
 *
 * Generation-counted 3-phase: request -> reply -> ack
 * Each topology event increments s_sync_generation.
 * Each completed barrier cycle decrements it by 1.
 * The barrier loops until s_sync_generation reaches 0.
 * ======================================================================== */

/**
 * Host-side resync barrier.
 * Called from HandlePadHost when s_sync_generation >= 2.
 *
 * Flow:
 *   1. ResetEvent(SyncEvent)
 *   2. Send DXPRESYNCREQ (type 10) to each active client
 *   3. Wait on SyncEvent (20s timeout) for all clients to respond
 *   4. On success: decrement s_sync_generation, loop if still > 0
 *   5. Reset sync sequence to 0 after barrier completes
 *
 * Returns 1 on success, 0 on timeout/failure.
 */
static int run_resync_barrier_host(void)
{
    while (InterlockedCompareExchange(&s_sync_generation, 0, 0) != 0) {
        DWORD wait_result;
        int i;
        uint32_t req_buf[1] = { TD5_DXPRESYNCREQ };

        TD5_LOG_I(NET_LOG, "Host resync barrier: generation=%ld",
                  (long)s_sync_generation);

        /* 1. Reset sync event */
        ResetEvent(s_evt_sync);

        /* 2. Send DXPRESYNCREQ to each active remote player */
        s_pending_ack_count = 0;
        s_expected_ack_count = 0;
        for (i = 0; i < TD5_NET_MAX_PLAYERS; i++) {
            if (i == s_local_slot) continue;
            if (!s_roster[i].active) continue;
            s_pending_ack_count++;
            s_expected_ack_count++;
            if (s_transport_send)
                s_transport_send(s_roster[i].id, req_buf, 4);
        }

        if (s_expected_ack_count == 0) {
            /* No remote players -- connection lost */
            TD5_LOG_W(NET_LOG, "Resync barrier: no remote players, connection lost");
            s_connection_lost = 1;
            return 0;
        }

        /* 3. Wait for all clients to respond with DXPRESYNC (type 11) */
        s_waiting_for_sync_barrier = 1;
        wait_result = WaitForSingleObject(s_evt_sync, SYNC_TIMEOUT_MS);
        s_waiting_for_sync_barrier = 0;

        if (wait_result != WAIT_OBJECT_0) {
            TD5_LOG_W(NET_LOG, "Host resync barrier timed out");
            return 0;
        }

        /* 4. Decrement generation (one barrier cycle complete) */
        InterlockedDecrement(&s_sync_generation);
        ResetEvent(s_evt_sync);

        TD5_LOG_I(NET_LOG, "Host resync barrier cycle complete, remaining=%ld",
                  (long)s_sync_generation);
    }

    /* 5. Reset sync sequence after full barrier drain */
    s_sync_sequence = 0;
    return 1;
}

/**
 * Client-side resync barrier.
 * Called from HandlePadClient when s_sync_generation != 0.
 *
 * Flow:
 *   1. Refresh roster
 *   2. Send DXPRESYNC (type 11) to host
 *   3. Wait on SyncEvent (20s timeout) for DXPRESYNCACK
 *   4. On success: decrement s_sync_generation, loop if still > 0
 *
 * Returns 1 on success, 0 on timeout/failure.
 */
static int run_resync_barrier_client(void)
{
    while (InterlockedCompareExchange(&s_sync_generation, 0, 0) != 0) {
        DWORD wait_result;
        uint32_t resync_buf[1] = { TD5_DXPRESYNC };
        uint32_t host_id = 0;
        int i;

        /* Check for host migration: if we became host, switch to host path */
        if (s_is_host) {
            TD5_LOG_I(NET_LOG, "Client detected host migration, switching to host barrier");
            return run_resync_barrier_host();
        }

        TD5_LOG_I(NET_LOG, "Client resync barrier: generation=%ld",
                  (long)s_sync_generation);

        /* 1. Refresh roster */
        refresh_roster();

        /* 2. Find host and send DXPRESYNC */
        for (i = 0; i < TD5_NET_MAX_PLAYERS; i++) {
            if (s_roster[i].active && i != s_local_slot) {
                host_id = s_roster[i].id;
                break; /* In a proper implementation, track the host slot explicitly */
            }
        }

        if (s_transport_send)
            s_transport_send(host_id, resync_buf, 4);

        /* 3. Wait for DXPRESYNCACK from host */
        s_waiting_for_sync_barrier = 1;
        ResetEvent(s_evt_sync);
        wait_result = WaitForSingleObject(s_evt_sync, SYNC_TIMEOUT_MS);
        s_waiting_for_sync_barrier = 0;

        if (wait_result != WAIT_OBJECT_0) {
            TD5_LOG_W(NET_LOG, "Client resync barrier timed out");
            return 0;
        }

        /* 4. Decrement generation */
        InterlockedDecrement(&s_sync_generation);
        ResetEvent(s_evt_sync);

        TD5_LOG_I(NET_LOG, "Client resync barrier cycle complete, remaining=%ld",
                  (long)s_sync_generation);
    }

    /* Reset sync sequence */
    s_sync_sequence = 0;
    return 1;
}

/* ========================================================================
 * Worker Thread Start / Stop
 *
 * Mirrors StartDirectPlayWorker (0x1000bfb0) and
 * ShutdownDirectPlayWorker (0x1000c050).
 * ======================================================================== */

static int start_worker(void)
{
    /* Create 4 events */
    if (!s_evt_receive) {
        s_evt_receive = CreateEventA(NULL, TRUE, FALSE, NULL); /* manual-reset */
        s_receive_event_is_ws2 = 0;
    }
    s_evt_stop      = CreateEventA(NULL, TRUE,  FALSE, NULL); /* manual-reset */
    s_evt_frame_ack = CreateEventA(NULL, FALSE, FALSE, NULL); /* auto-reset */
    s_evt_sync      = CreateEventA(NULL, TRUE,  FALSE, NULL); /* manual-reset */

    if (!s_evt_receive || !s_evt_stop || !s_evt_frame_ack || !s_evt_sync) {
        TD5_LOG_E(NET_LOG, "Failed to create events");
        return 0;
    }

    /* Create worker thread */
    s_worker_thread = CreateThread(NULL, 0, worker_thread_proc, NULL, 0, &s_worker_thread_id);
    if (!s_worker_thread) {
        TD5_LOG_E(NET_LOG, "Failed to create worker thread");
        return 0;
    }

    TD5_LOG_I(NET_LOG, "Worker started (thread=%lu)", (unsigned long)s_worker_thread_id);
    return 1;
}

static void stop_worker(void)
{
    if (s_evt_stop) {
        /* Signal stop event */
        SetEvent(s_evt_stop);

        /* Wait for worker thread to exit */
        if (s_worker_thread) {
            WaitForSingleObject(s_worker_thread, 5000);
            CloseHandle(s_worker_thread);
            s_worker_thread = NULL;
        }
    }

    /* Close event handles */
    if (s_evt_receive) {
        if (s_receive_event_is_ws2) {
            WSACloseEvent((WSAEVENT)s_evt_receive);
        } else {
            CloseHandle(s_evt_receive);
        }
        s_evt_receive = NULL;
        s_receive_event_is_ws2 = 0;
        s_ws2_event = WSA_INVALID_EVENT;
    }
    if (s_evt_stop)      { CloseHandle(s_evt_stop);      s_evt_stop      = NULL; }
    if (s_evt_frame_ack) { CloseHandle(s_evt_frame_ack); s_evt_frame_ack = NULL; }
    if (s_evt_sync)      { CloseHandle(s_evt_sync);      s_evt_sync      = NULL; }
}

/* ========================================================================
 * Public API: Init / Shutdown / Tick
 * ======================================================================== */

/**
 * Initialize the network subsystem.
 * Mirrors DXPlay::Environment() + DXPlay::Create().
 *
 * Zeros all runtime state, resets ring buffer indices, creates 4 events
 * and starts the worker thread.
 */
int td5_net_init(void)
{
    if (s_initialized) {
        TD5_LOG_W(NET_LOG, "td5_net_init called when already initialized");
        return 1;
    }

    /* Zero all state (mirrors Environment() zeroing 0x2FA + 0x304 dwords) */
    memset(s_roster, 0, sizeof(s_roster));
    memset(&s_session, 0, sizeof(s_session));
    memset(s_enum_sessions, 0, sizeof(s_enum_sessions));
    s_is_host = 0;
    s_is_client = 0;
    s_active = 0;
    s_connection_lost = 0;
    s_local_slot = 0;
    s_player_count = 0;
    s_enum_session_count = 0;
    s_connection_count = 0;
    s_selected_connection = -1;
    s_sync_sequence = 0;
    InterlockedExchange(&s_sync_generation, 0);
    s_waiting_for_sync_barrier = 0;
    s_waiting_for_frame_ack = 0;
    s_pending_ack_count = 0;
    s_expected_ack_count = 0;
    s_received_frame_dt = 0.0f;

    /* S10: connection-mode / port / UPnP / join state */
    s_conn_mode = TD5_NET_MODE_LAN;
    s_game_port = WS2_GAME_PORT;
    s_enable_upnp = 0;
    s_upnp_status = TD5_NET_UPNP_IDLE;
    s_upnp_mapped_port = 0;
    s_status_text[0] = '\0';
    s_local_ip[0] = '\0';
    s_last_beacon_ms = 0;
    s_local_player_name[0] = '\0';
    s_join_pending = 0;
    s_join_attempts = 0;
    s_join_last_ms = 0;

    /* S10b: lobby session config + per-slot info */
    s_host_password[0] = '\0';
    s_join_password[0] = '\0';
    s_join_nak_reason = 0;
    s_ping_last_ms = 0;
    s_roster_info_last_ms = 0;
    s_session_token = 0;
    {
        int li;
        for (li = 0; li < TD5_NET_MAX_PLAYERS; li++) {
            s_slot_latency[li] = -1;
            s_slot_name[li][0] = '\0';
        }
    }

    memset(s_player_sync_table, 0, sizeof(s_player_sync_table));
    memset(s_player_sync_received, 0, sizeof(s_player_sync_received));
    memset(&s_outbound_frame, 0, sizeof(s_outbound_frame));

    s_evt_receive = NULL;
    s_evt_stop = NULL;
    s_evt_frame_ack = NULL;
    s_evt_sync = NULL;
    s_receive_event_is_ws2 = 0;

    /* Install Winsock2 UDP transport backend */
    s_transport_send        = ws2_transport_send;
    s_transport_recv        = ws2_transport_recv;
    s_transport_init        = ws2_transport_init;
    s_transport_host        = ws2_transport_host;
    s_transport_join        = ws2_transport_join;
    s_transport_join_direct = ws2_transport_join_direct;
    s_transport_enum        = ws2_transport_enum;
    s_transport_shutdown    = ws2_transport_shutdown;

    /* Reset ring buffer */
    ring_reset();

    if (s_transport_init && !s_transport_init()) {
        TD5_LOG_E(NET_LOG, "Failed to initialize transport backend");
        return 0;
    }

    /* Start worker thread + 4 events */
    if (!start_worker()) {
        TD5_LOG_E(NET_LOG, "Failed to start network worker");
        if (s_transport_shutdown) {
            s_transport_shutdown();
        }
        return 0;
    }

    s_initialized = 1;
    TD5_LOG_I(NET_LOG, "Network subsystem initialized");
    return 1;
}

/**
 * Shutdown the network subsystem.
 * Mirrors DXPlay::Destroy() -> ShutdownDirectPlayWorker().
 */
void td5_net_shutdown(void)
{
    if (!s_initialized)
        return;

    TD5_LOG_I(NET_LOG, "Shutting down network subsystem");

    /* S31: tell the peers we're leaving BEFORE tearing the transport down,
     * so a host quit doesn't leave clients sitting in a dead lobby (the
     * DXPDISCONNECT handler sets s_connection_lost on the receivers). */
    if ((s_is_host || s_is_client) && s_transport_send)
        td5_net_send(TD5_DXPDISCONNECT, NULL, 0);

    /* Stop worker thread and close events */
    stop_worker();

    /* S10: release any UPnP port-mapping we opened for the host game port. */
    if (s_upnp_mapped_port) {
        td5_upnp_unmap_port(s_upnp_mapped_port, 1 /*UDP*/);
        s_upnp_mapped_port = 0;
        s_upnp_status = TD5_NET_UPNP_IDLE;
    }

    /* Notify transport */
    if (s_transport_shutdown)
        s_transport_shutdown();

    /* Clear state */
    s_active = 0;
    s_is_host = 0;
    s_is_client = 0;
    s_connection_lost = 0;
    s_player_count = 0;

    ring_reset();

    s_initialized = 0;
    TD5_LOG_I(NET_LOG, "Network subsystem shut down");
}

/**
 * Per-frame network tick. Called from the main game loop.
 * Processes any pending ring buffer messages that need game-thread action.
 */
void td5_net_tick(void)
{
    uint32_t now;

    if (!s_initialized)
        return;

    now = td5_plat_time_ms();

    /* Poll the discovery socket for QUERY (-> ANNOUNCE) and the legacy
       discovery-socket join path. The primary join now rides the game socket. */
    if (s_ws2_disc_socket != INVALID_SOCKET) {
        DiscoveryMsg disc_msg;
        SOCKADDR_IN from_addr;
        int from_len, ret;
        int max_polls = 16;

        while (max_polls-- > 0) {
            from_len = (int)sizeof(from_addr);
            ret = recvfrom(s_ws2_disc_socket, (char *)&disc_msg, sizeof(disc_msg), 0,
                           (struct sockaddr *)&from_addr, &from_len);
            if (ret < (int)sizeof(DiscoveryMsg))
                break;
            if (disc_msg.magic != WS2_DISCOVERY_MAGIC)
                continue;

            switch (disc_msg.disc_type) {
            case WS2_DISC_QUERY:
                if (s_is_host && !s_session.sealed) {
                    DiscoveryMsg announce;
                    memset(&announce, 0, sizeof(announce));
                    announce.magic = WS2_DISCOVERY_MAGIC;
                    announce.disc_type = WS2_DISC_ANNOUNCE;
                    strncpy(announce.session_name, s_session.name,
                            sizeof(announce.session_name) - 1);
                    announce.player_count = (uint32_t)s_player_count;
                    announce.max_players  = s_session.max_players;
                    announce.game_type    = s_session.game_type;
                    announce.sealed       = (uint32_t)s_session.sealed;
                    announce.game_port    = (uint16_t)s_game_port;
                    sendto(s_ws2_disc_socket, (const char *)&announce,
                           sizeof(announce), 0,
                           (const struct sockaddr *)&from_addr,
                           (int)sizeof(from_addr));
                }
                break;

            case WS2_DISC_JOIN_REQ:
                if (s_is_host)
                    ws2_handle_join_request(&from_addr);
                break;

            case WS2_DISC_JOIN_ACK:
                if (!s_is_host && s_local_slot < 0) {
                    /* [SEC 2026-06-15] Validate the assigned slot BEFORE storing
                     * it: an out-of-range value used to persist in s_local_slot
                     * (only the roster write was guarded) and could later index
                     * s_player_sync_table[] out of bounds. Mirrors the primary
                     * game-socket JOIN_ACK path in ws2_handle_game_control. */
                    int slot = (int)disc_msg.assigned_slot;
                    if (slot >= 0 && slot < TD5_NET_MAX_PLAYERS) {
                        s_local_slot = slot;
                        s_session_token = disc_msg.session_token;  /* [SEC] */
                        s_roster[slot].id = disc_msg.assigned_id;
                        s_roster[slot].active = 1;
                        TD5_LOG_I(NET_LOG, "Assigned slot %d, id %u",
                                  slot, disc_msg.assigned_id);
                    }
                }
                break;

            default:
                break;
            }
        }
    }

    /* S10: periodic LAN ANNOUNCE beacon so passive listeners can find an open
       host without first sending a QUERY. */
    if (s_is_host && s_conn_mode == TD5_NET_MODE_LAN && !s_session.sealed &&
        s_ws2_disc_socket != INVALID_SOCKET && (now - s_last_beacon_ms) >= 1000) {
        DiscoveryMsg announce;
        SOCKADDR_IN bcast;
        memset(&announce, 0, sizeof(announce));
        announce.magic = WS2_DISCOVERY_MAGIC;
        announce.disc_type = WS2_DISC_ANNOUNCE;
        strncpy(announce.session_name, s_session.name, sizeof(announce.session_name) - 1);
        announce.player_count = (uint32_t)s_player_count;
        announce.max_players  = s_session.max_players;
        announce.game_type    = s_session.game_type;
        announce.sealed       = (uint32_t)s_session.sealed;
        announce.game_port    = (uint16_t)s_game_port;
        memset(&bcast, 0, sizeof(bcast));
        bcast.sin_family = AF_INET;
        bcast.sin_addr.s_addr = htonl(INADDR_BROADCAST);
        bcast.sin_port = htons(WS2_DISCOVERY_PORT);
        sendto(s_ws2_disc_socket, (const char *)&announce, sizeof(announce), 0,
               (const struct sockaddr *)&bcast, (int)sizeof(bcast));
        s_last_beacon_ms = now;
    }

    /* S10: client JOIN_REQ retry until the host assigns a slot (JOIN_ACK). */
    if (s_join_pending) {
        if (s_local_slot >= 0) {
            s_join_pending = 0;
            net_build_status_text();
            TD5_LOG_I(NET_LOG, "Join complete: local slot %d", s_local_slot);
        } else if ((now - s_join_last_ms) >= 500) {
            if (s_join_attempts >= 12) {       /* ~6 s of retries */
                s_join_pending = 0;
                s_connection_lost = 1;
                TD5_LOG_W(NET_LOG, "Join timed out (no JOIN_ACK from host)");
            } else {
                ws2_send_join_request(s_local_player_name);
                s_join_attempts++;
                s_join_last_ms = now;
            }
        }
    }

    /* S10b: host lobby keepalive — RTT pings + per-slot names/latency broadcast
       (lobby phase only; lockstep frame sync owns the wire once the race starts). */
    if (s_is_host && !s_active) {
        if ((now - s_ping_last_ms) >= 1000) {
            ws2_send_pings();
            s_ping_last_ms = now;
        }
        if ((now - s_roster_info_last_ms) >= 1000) {
            ws2_broadcast_roster_info();
            s_roster_info_last_ms = now;
        }
    }
}

/* ========================================================================
 * Public API: Session Management
 * ======================================================================== */

/**
 * Create a new multiplayer session (host path).
 * Mirrors DXPlay::NewSession().
 *
 * Flow:
 *   1. Fill session descriptor with name, max_players, game_type, seed
 *   2. Open session via transport (DPOPEN_CREATE equivalent)
 *   3. Set s_is_host = 1, s_is_client = 1
 *   4. Create local player in slot 0
 *   5. Refresh roster and broadcast
 */
/* [SEC 2026-06-15] Generate the per-session auth token (host, at session
 * create). High-res QPC + wall clock so an off-path attacker that never saw
 * the JOIN handshake cannot predict it; deliberately kept OUT of the CRT
 * rand() stream (lockstep determinism). Never 0 (reserved "unknown") and never
 * the discovery magic (that value would collide with the control-message tag
 * used to demux datagrams on the wire). */
static uint32_t net_make_session_token(void)
{
    LARGE_INTEGER qpc;
    uint32_t t;
    QueryPerformanceCounter(&qpc);
    t = (uint32_t)qpc.QuadPart
        ^ ((uint32_t)(qpc.QuadPart >> 32) * 2654435761u)
        ^ (td5_plat_time_ms() * 40503u);
    if (t == 0) t = 0xA5A5A5A5u;
    if (t == WS2_DISCOVERY_MAGIC) t ^= 0xFFFFFFFFu;
    return t;
}

/* True if <dotted> is an RFC1918 / CGNAT / link-local IPv4 address -- i.e. NOT a
 * public internet address. Used to detect double-NAT: if the IGD's own WAN IP is
 * private, a UPnP-opened port is still unreachable from the internet. */
static int net_ip_is_private(const char *dotted)
{
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (!dotted || !dotted[0]) return 0;
    if (sscanf(dotted, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return 0;
    if (a == 10)                          return 1;  /* 10.0.0.0/8         */
    if (a == 172 && b >= 16 && b <= 31)   return 1;  /* 172.16.0.0/12      */
    if (a == 192 && b == 168)             return 1;  /* 192.168.0.0/16     */
    if (a == 100 && b >= 64 && b <= 127)  return 1;  /* 100.64.0.0/10 CGNAT*/
    if (a == 169 && b == 254)             return 1;  /* 169.254.0.0/16 LL  */
    if (a == 127 || a == 0)               return 1;  /* loopback / unspec  */
    return 0;
}

/**
 * Build the human-readable host/connect status line for the lobby UI
 * (local IP + port, plus UPnP outcome when hosting Direct).
 */
static void net_build_status_text(void)
{
    const char *ip = s_local_ip[0] ? s_local_ip : "?";

    if (s_is_host) {
        if (s_conn_mode == TD5_NET_MODE_DIRECT) {
            /* [ITEM 4 2026-06-16] Distinguish the three UPnP outcomes clearly --
             * MAPPED (router opened the port), FAILED (router refused / no IGD
             * found -> forward manually), DISABLED (UPnP off in setup). Always
             * name the UDP port the user must forward in the non-mapped cases. */
            switch (s_upnp_status) {
            case TD5_NET_UPNP_MAPPED:
                snprintf(s_status_text, sizeof(s_status_text),
                         "Host %s:%d  -  UPnP: port opened on router",
                         ip, s_game_port);
                break;
            case TD5_NET_UPNP_DOUBLE_NAT:
                snprintf(s_status_text, sizeof(s_status_text),
                         "Host %s:%d  -  UPnP ok but DOUBLE-NAT (WAN %s) - forward UDP %d on upstream router",
                         ip, s_game_port,
                         s_external_ip[0] ? s_external_ip : "private", s_game_port);
                break;
            case TD5_NET_UPNP_MAPPING:
                snprintf(s_status_text, sizeof(s_status_text),
                         "Host %s:%d  -  UPnP: mapping port...",
                         ip, s_game_port);
                break;
            case TD5_NET_UPNP_PORT_CONFLICT:
                snprintf(s_status_text, sizeof(s_status_text),
                         "Host %s:%d  -  UPnP: UDP %d already forwarded on router - remove that rule or change port",
                         ip, s_game_port, s_game_port);
                break;
            case TD5_NET_UPNP_FAILED:
                snprintf(s_status_text, sizeof(s_status_text),
                         "Host %s:%d  -  UPnP: no router or mapping refused - forward UDP %d manually",
                         ip, s_game_port, s_game_port);
                break;
            case TD5_NET_UPNP_UNAVAILABLE:
                snprintf(s_status_text, sizeof(s_status_text),
                         "Host %s:%d  -  UPnP OFF - forward UDP %d manually",
                         ip, s_game_port, s_game_port);
                break;
            default:
                snprintf(s_status_text, sizeof(s_status_text),
                         "Host %s:%d", ip, s_game_port);
                break;
            }
        } else {
            snprintf(s_status_text, sizeof(s_status_text),
                     "Hosting LAN game on %s:%d", ip, s_game_port);
        }
    } else if (s_is_client) {
        if (s_local_slot >= 0)   /* JOIN_ACK received -> we are in */
            snprintf(s_status_text, sizeof(s_status_text),
                     "Connected to %s:%u",
                     inet_ntoa(s_ws2_host_addr.sin_addr),
                     (unsigned)ntohs(s_ws2_host_addr.sin_port));
        else
            snprintf(s_status_text, sizeof(s_status_text),
                     "Connecting to %s:%u ...",
                     inet_ntoa(s_ws2_host_addr.sin_addr),
                     (unsigned)ntohs(s_ws2_host_addr.sin_port));
    } else {
        s_status_text[0] = '\0';
    }
}

/** Shared client-join finalize: set flags, store name, kick off the
 *  game-socket JOIN handshake (retried by td5_net_tick). */
static void net_begin_client_join(const char *player_name)
{
    s_is_host = 0;
    s_is_client = 1;
    s_connection_lost = 0;
    s_local_slot = -1;            /* assigned by host via JOIN_ACK / DXPROSTER */
    s_session_token = 0;          /* [SEC] learned from the host's JOIN_ACK */
    memset(s_roster, 0, sizeof(s_roster));

    if (player_name)
        snprintf(s_local_player_name, sizeof(s_local_player_name), "%s", player_name);
    else
        s_local_player_name[0] = '\0';

    ResetEvent(s_evt_frame_ack);

    s_join_nak_reason = 0;   /* clear any prior rejection before this attempt */
    s_client_last_host_ms = 0;   /* [ITEM 3] host-keepalive watchdog: none seen yet */
    s_join_pending = 1;
    ws2_send_join_request(s_local_player_name);
    s_join_attempts = 1;
    s_join_last_ms = td5_plat_time_ms();

    net_build_status_text();
}

/**
 * Host with an explicit game port and optional UPnP IGD port-mapping.
 * Mirrors DXPlay::NewSession() with the S10 transport extensions.
 */
int td5_net_create_session_ex(const char *name, const char *player_name,
                              int max_players, int game_port, int enable_upnp)
{
    if (!s_initialized) {
        TD5_LOG_E(NET_LOG, "Cannot create session: not initialized");
        return 0;
    }

    if (max_players < 1) max_players = 1;
    if (max_players > TD5_NET_MAX_PLAYERS) max_players = TD5_NET_MAX_PLAYERS;

    /* S10: the transport host path reads s_game_port / s_enable_upnp. */
    s_game_port = (game_port > 0 && game_port <= 65535) ? game_port : WS2_GAME_PORT;
    s_enable_upnp = enable_upnp ? 1 : 0;
    s_upnp_status = TD5_NET_UPNP_IDLE;

    /* [SEC 2026-06-15] Mint this session's auth token. Sent to each client in
     * its JOIN_ACK (after the password gate) and required on every DXPTYPE
     * datagram thereafter -- see ws2_transport_send / _recv. */
    s_session_token = net_make_session_token();

    /* Fill session info */
    memset(&s_session, 0, sizeof(s_session));
    if (name) {
        strncpy(s_session.name, name, sizeof(s_session.name) - 1);
        s_session.name[sizeof(s_session.name) - 1] = '\0';
    }
    s_session.max_players = (uint32_t)max_players;
    s_session.player_count = 1;
    s_session.seed = td5_plat_time_ms();
    s_session.sealed = 0;

    /* Ask transport to host (binds the game port + runs UPnP if requested) */
    if (s_transport_host) {
        if (!s_transport_host(name, max_players)) {
            TD5_LOG_E(NET_LOG, "Transport failed to host session");
            return 0;
        }
    }

    /* Set host flags */
    s_is_host = 1;
    s_is_client = 1;
    s_connection_lost = 0;

    /* Create local player in slot 0 */
    s_local_slot = 0;
    memset(s_roster, 0, sizeof(s_roster));
    s_roster[0].id = 1; /* host always gets ID 1 */
    s_roster[0].active = 1;
    if (player_name) {
        strncpy(s_roster[0].name, player_name, sizeof(s_roster[0].name) - 1);
        s_roster[0].name[sizeof(s_roster[0].name) - 1] = '\0';
        snprintf(s_local_player_name, sizeof(s_local_player_name), "%s", player_name);
    }
    s_player_count = 1;

    /* Reset frame ack event (manual-reset, create fresh for session) */
    ResetEvent(s_evt_frame_ack);

    /* Broadcast initial roster */
    broadcast_roster();

    net_build_status_text();

    TD5_LOG_I(NET_LOG, "Session created: \"%s\" (max=%d port=%d upnp=%d seed=%u) %s",
              s_session.name, max_players, s_game_port, s_enable_upnp,
              s_session.seed, s_status_text);
    return 1;
}

int td5_net_create_session(const char *name, const char *player_name, int max_players)
{
    /* LAN host convenience wrapper: default game port, no UPnP. */
    return td5_net_create_session_ex(name, player_name, max_players,
                                     WS2_GAME_PORT, 0);
}

/**
 * Join an existing session (client path).
 * Mirrors DXPlay::JoinSession().
 *
 * Flow:
 *   1. Open session via transport (DPOPEN_JOIN equivalent)
 *   2. Create local player
 *   3. Set s_is_host = 0, s_is_client = 1
 *   4. Wait for roster from host
 */
int td5_net_join_session(int session_index, const char *player_name)
{
    if (!s_initialized) {
        TD5_LOG_E(NET_LOG, "Cannot join session: not initialized");
        return 0;
    }

    if (session_index < 0 || session_index >= s_enum_session_count) {
        TD5_LOG_E(NET_LOG, "Invalid session index %d (count=%d)",
                  session_index, s_enum_session_count);
        return 0;
    }

    s_conn_mode = TD5_NET_MODE_LAN;

    /* Point the transport at the discovered host + bind a local game socket. */
    if (s_transport_join) {
        if (!s_transport_join(session_index)) {
            TD5_LOG_E(NET_LOG, "Transport failed to join session %d", session_index);
            return 0;
        }
    }

    net_begin_client_join(player_name);

    TD5_LOG_I(NET_LOG, "Join(LAN) session %d as \"%s\"",
              session_index, player_name ? player_name : "(unnamed)");
    return 1;
}

/**
 * Join an explicit host by IP[:port] -- no enumeration (Direct mode).
 * The JOIN handshake rides the game socket so it works across routers when the
 * host's port is reachable (e.g. opened via UPnP).
 */
int td5_net_join_direct(const char *host_ip, int game_port, const char *player_name)
{
    if (!s_initialized) {
        TD5_LOG_E(NET_LOG, "Cannot join: not initialized");
        return 0;
    }
    if (!host_ip || !host_ip[0]) {
        TD5_LOG_E(NET_LOG, "Join(direct): empty host IP");
        return 0;
    }
    if (game_port <= 0 || game_port > 65535)
        game_port = WS2_GAME_PORT;

    s_conn_mode = TD5_NET_MODE_DIRECT;
    s_game_port = game_port;

    if (s_transport_join_direct) {
        if (!s_transport_join_direct(host_ip, game_port)) {
            TD5_LOG_E(NET_LOG, "Transport failed to join %s:%d", host_ip, game_port);
            return 0;
        }
    }

    net_begin_client_join(player_name);

    TD5_LOG_I(NET_LOG, "Join(direct) %s:%d as \"%s\"",
              host_ip, game_port, player_name ? player_name : "(unnamed)");
    return 1;
}

/* ========================================================================
 * Public API: S10 connection-mode + status
 * ======================================================================== */

int td5_net_set_mode(int mode)
{
    if (mode != TD5_NET_MODE_LAN && mode != TD5_NET_MODE_DIRECT)
        return 0;
    s_conn_mode = mode;
    TD5_LOG_I(NET_LOG, "Connection mode set to %s",
              mode == TD5_NET_MODE_DIRECT ? "DIRECT" : "LAN");
    return 1;
}

int td5_net_get_mode(void)
{
    return s_conn_mode;
}

int td5_net_get_upnp_status(void)
{
    return s_upnp_status;
}

const char *td5_net_get_status_text(void)
{
    net_build_status_text();   /* [S31] live: "Connecting" flips to "Connected" */
    return s_status_text;
}

int td5_net_get_local_ip(char *buf, int len)
{
    if (!buf || len <= 0)
        return 0;
    if (s_local_ip[0]) {
        snprintf(buf, (size_t)len, "%s", s_local_ip);
        return 1;
    }
    if (td5_upnp_get_local_ip(buf, len)) {
        snprintf(s_local_ip, sizeof(s_local_ip), "%s", buf);
        return 1;
    }
    buf[0] = '\0';
    return 0;
}

/* --- S10b: lobby session limits + per-slot info --- */

void td5_net_set_session_limits(int max_players, const char *password)
{
    if (max_players < 2) max_players = 2;
    if (max_players > TD5_NET_MAX_PLAYERS) max_players = TD5_NET_MAX_PLAYERS;
    s_session.max_players = (uint32_t)max_players;
    if (password) {
        strncpy(s_host_password, password, sizeof(s_host_password) - 1);
        s_host_password[sizeof(s_host_password) - 1] = '\0';
    } else {
        s_host_password[0] = '\0';
    }
    TD5_LOG_I(NET_LOG, "Session limits: max=%d password=%s",
              max_players, s_host_password[0] ? "set" : "none");
    if (s_is_host)
        ws2_broadcast_roster_info();
}

int td5_net_get_max_players(void)
{
    return (int)s_session.max_players;
}

void td5_net_set_join_password(const char *password)
{
    if (password) {
        strncpy(s_join_password, password, sizeof(s_join_password) - 1);
        s_join_password[sizeof(s_join_password) - 1] = '\0';
    } else {
        s_join_password[0] = '\0';
    }
}

int td5_net_get_join_nak_reason(void)
{
    return s_join_nak_reason;
}

const char *td5_net_get_slot_name(int slot)
{
    if (slot < 0 || slot >= TD5_NET_MAX_PLAYERS) return "";
    if (!s_roster[slot].active) return "";
    if (s_roster[slot].name[0]) return s_roster[slot].name;
    return s_slot_name[slot];
}

int td5_net_get_slot_latency_ms(int slot)
{
    if (slot < 0 || slot >= TD5_NET_MAX_PLAYERS) return -1;
    return s_slot_latency[slot];
}

/**
 * Seal/unseal the session (prevent/allow new joins).
 * Mirrors DXPlay::SealSession().
 *
 * sealed=0: flags=0x44 (open)
 * sealed=1: flags=0x45 (DPSESSION_JOINDISABLED = closed)
 */
void td5_net_seal_session(int sealed)
{
    s_session.sealed = sealed;
    TD5_LOG_I(NET_LOG, "Session %s", sealed ? "sealed" : "unsealed");
    /* Transport layer would update session descriptor here */
}

/**
 * Clear the sync active flag (stop frame synchronization).
 * Mirrors DXPlay::UnSync() at 0x1000bd00.
 */
void td5_net_unsync(void)
{
    s_active = 0;
    TD5_LOG_I(NET_LOG, "Frame sync deactivated (unsync)");
}

/* ========================================================================
 * Public API: Per-Frame Sync
 * ======================================================================== */

/**
 * Host per-frame sync (HandlePadHost at 0x1000b680).
 *
 * Flow:
 *   1. Check for pending resync (generation != 0) -- run barrier protocol
 *   2. Copy player active flags to snapshot
 *   3. Wait on FrameAck event (20s) for all client inputs
 *   4. When all received:
 *      - Merge: for each slot, copy received controlBits (host uses local)
 *      - Write merged controlBits[0..5] into outbound frame
 *      - Write host's frameDeltaTime and syncSequence
 *      - Broadcast DXPFRAME (type 0, 0x80 bytes)
 *      - Increment syncSequence
 *      - Reset FrameAck event
 *   5. Return merged controlBits to caller
 *
 * @param control_bits  Array of 6 uint32_t -- on entry, [local_slot] has local input.
 *                      On return, all slots filled with merged input.
 * @param frame_dt      On entry, host's local normalized frame dt.
 *                      Unchanged on return (host is authoritative).
 * @return 1 on success, 0 on timeout/failure.
 */
int td5_net_handle_host_frame(uint32_t *control_bits, float *frame_dt)
{
    DWORD wait_result;
    int i;

    if (!s_active || !s_is_host)
        return 0;

    /* 1. Check for pending resync */
    if (InterlockedCompareExchange(&s_sync_generation, 0, 0) != 0) {
        if (!run_resync_barrier_host())
            return 0;
    }

    if (s_connection_lost)
        return 0;

    /* 2. Store local input and mark as received */
    s_player_sync_table[s_local_slot] = control_bits[s_local_slot];
    s_player_sync_received[s_local_slot] = 1;

    /* Check if we are the only player (no wait needed) */
    if (count_active_remote_players() == 0) {
        /* Solo host -- no clients to wait for */
        goto merge_and_broadcast;
    }

    /* 3. Wait for all client inputs */
    s_waiting_for_frame_ack = 1;
    ResetEvent(s_evt_frame_ack);

    /* Check if already all received before waiting */
    {
        int all_received = 1;
        for (i = 0; i < TD5_NET_MAX_PLAYERS; i++) {
            if (i == s_local_slot) continue;
            if (!s_roster[i].active) continue;
            if (!s_player_sync_received[i]) {
                all_received = 0;
                break;
            }
        }
        if (all_received) goto done_waiting;
    }

    /* Wait in short slices so a peer quit (DXPDISCONNECT -> s_connection_lost)
     * aborts the barrier in <=250 ms instead of blocking the game thread for
     * the full 20 s timeout (the window reads as frozen). */
    {
        DWORD waited = 0;
        for (;;) {
            wait_result = WaitForSingleObject(s_evt_frame_ack, 250);
            if (wait_result == WAIT_OBJECT_0 || s_connection_lost) break;
            waited += 250;
            if (waited >= SYNC_TIMEOUT_MS) break;
        }
    }
    if (wait_result != WAIT_OBJECT_0) {
        s_waiting_for_frame_ack = 0;
        TD5_LOG_W(NET_LOG, "Host HandlePadHost: %s waiting for client inputs",
                  s_connection_lost ? "connection lost" : "timed out");
        return 0; /* Caller quits the race */
    }

done_waiting:
    s_waiting_for_frame_ack = 0;

    /* Check if resync was triggered during the wait */
    if (InterlockedCompareExchange(&s_sync_generation, 0, 0) != 0) {
        if (!run_resync_barrier_host())
            return 0;
    }

merge_and_broadcast:
    /* 4. Merge: build outbound frame with all players' controlBits */
    memset(&s_outbound_frame, 0, sizeof(s_outbound_frame));
    s_outbound_frame.msg_type = TD5_DXPFRAME;

    for (i = 0; i < TD5_NET_MAX_PLAYERS; i++) {
        if (s_roster[i].active) {
            s_outbound_frame.control_bits[i] = s_player_sync_table[i];
        } else {
            s_outbound_frame.control_bits[i] = 0;
        }
    }

    s_outbound_frame.frame_delta_time = *frame_dt;
    s_outbound_frame.sync_sequence = s_sync_sequence;

    /* Reset per-frame receive flags BEFORE broadcasting [S31 deadlock fix]:
     * a client sends its NEXT frame the instant the broadcast lands, and on
     * a low-latency link that next frame can beat a post-broadcast wipe --
     * its received flag would be erased and both sides stall to the 20 s
     * timeout. Clearing first is race-free: no client can send frame N+1
     * until it has seen broadcast N. */
    for (i = 0; i < TD5_NET_MAX_PLAYERS; i++)
        s_player_sync_received[i] = 0;

    /* Broadcast to all players (DPID=0 = broadcast) */
    send_frame_to_all(&s_outbound_frame);

    /* Increment sequence counter */
    s_sync_sequence++;

    /* 5. Copy merged controlBits back to caller */
    for (i = 0; i < TD5_NET_MAX_PLAYERS; i++)
        control_bits[i] = s_outbound_frame.control_bits[i];

    return 1;
}

/**
 * Client per-frame sync (HandlePadClient at 0x1000b8b0).
 *
 * Flow:
 *   1. Check for pending resync -- if client has become host, call
 *      HandlePadHost instead (host migration). Otherwise run client barrier.
 *   2. Package local controlBits[local_slot] into DXPFRAME message
 *   3. Send to host (targeted via host player ID)
 *   4. Wait on FrameAck event (20s) for host's broadcast
 *   5. Copy controlBits[0..5] from received broadcast
 *   6. Copy host's frameDeltaTime into local timing
 *   7. Validate syncSequence
 *   8. Return authoritative frame data to caller
 *
 * @param control_bits  Array of 6 uint32_t -- on entry, [local_slot] has local input.
 *                      On return, all slots filled with host's merged input.
 * @param frame_dt      On return, host's authoritative frame dt.
 * @return 1 on success, 0 on timeout/failure.
 */
int td5_net_handle_client_frame(uint32_t *control_bits, float *frame_dt)
{
    DWORD wait_result;
    TD5_NetFrame send_frame;
    uint32_t host_id = 0;
    int i;

    if (!s_active || s_connection_lost)
        return 0;

    /* 1. Check for pending resync */
    if (InterlockedCompareExchange(&s_sync_generation, 0, 0) != 0) {
        /* If we became host during resync, switch to host path */
        if (s_is_host) {
            TD5_LOG_I(NET_LOG, "Client promoted to host during resync, calling host handler");
            return td5_net_handle_host_frame(control_bits, frame_dt);
        }
        if (!run_resync_barrier_client())
            return 0;
    }

    /* 2. Package local input into DXPFRAME */
    memset(&send_frame, 0, sizeof(send_frame));
    send_frame.msg_type = TD5_DXPFRAME;
    if (s_local_slot >= 0 && s_local_slot < TD5_NET_MAX_PLAYERS)
        send_frame.control_bits[s_local_slot] = control_bits[s_local_slot];

    /* 3. Send to host */
    /* Find host's player ID (first active slot that is not us, or slot 0) */
    for (i = 0; i < TD5_NET_MAX_PLAYERS; i++) {
        if (s_roster[i].active && i != s_local_slot) {
            host_id = s_roster[i].id;
            break;
        }
    }

    /* 4. Arm the wait BEFORE sending [S31 deadlock fix]: on a low-latency
     * link the host's broadcast can land before a post-send ResetEvent, and
     * resetting then throws the auto-reset signal away -> both sides stall
     * to the 20 s timeout. The broadcast for THIS frame cannot precede our
     * own send (the host needs our input to merge), so arming first is
     * race-free. */
    s_waiting_for_frame_ack = 1;
    ResetEvent(s_evt_frame_ack);

    if (s_transport_send)
        s_transport_send(host_id, &send_frame, FRAME_MSG_SIZE);

    /* Sliced wait: see the host-side comment -- bail fast on a dead link. */
    {
        DWORD waited = 0;
        for (;;) {
            wait_result = WaitForSingleObject(s_evt_frame_ack, 250);
            if (wait_result == WAIT_OBJECT_0 || s_connection_lost) break;
            waited += 250;
            if (waited >= SYNC_TIMEOUT_MS) break;
        }
    }
    s_waiting_for_frame_ack = 0;

    if (wait_result != WAIT_OBJECT_0) {
        TD5_LOG_W(NET_LOG, "Client HandlePadClient: %s waiting for host broadcast",
                  s_connection_lost ? "connection lost" : "timed out");
        return 0; /* Caller quits the race */
    }

    /* Check if resync was triggered during the wait */
    if (InterlockedCompareExchange(&s_sync_generation, 0, 0) != 0) {
        if (s_is_host)
            return td5_net_handle_host_frame(control_bits, frame_dt);
        if (!run_resync_barrier_client())
            return 0;
    }

    /* 5. Copy merged controlBits from received broadcast */
    for (i = 0; i < TD5_NET_MAX_PLAYERS; i++)
        control_bits[i] = s_player_sync_table[i];

    /* 6. Copy host's authoritative frame dt */
    *frame_dt = s_received_frame_dt;

    /* 7. syncSequence validated in handle_frame() */

    return 1;
}

/* ========================================================================
 * Public API: Session Browser
 * ======================================================================== */

/**
 * Enumerate available connection (transport) providers.
 * Mirrors DXPlay::ConnectionEnumerate().
 * Returns the number of available providers.
 */
int td5_net_enumerate_connections(void)
{
    /* In the source port, we offer a single connection type
       (the configured transport backend). Future: enumerate ENet, LAN, etc. */
    s_connection_count = 1; /* At minimum, the default transport */

    TD5_LOG_I(NET_LOG, "Enumerated %d connection(s)", s_connection_count);
    return s_connection_count;
}

/**
 * Select a connection provider.
 * Mirrors DXPlay::ConnectionPick().
 * Starts periodic session enumeration (3-second timer in original).
 */
int td5_net_pick_connection(int index)
{
    if (index < 0 || index >= s_connection_count) {
        TD5_LOG_E(NET_LOG, "Invalid connection index %d", index);
        return 0;
    }

    s_selected_connection = index;

    /* Start session enumeration */
    td5_net_enumerate_sessions();

    TD5_LOG_I(NET_LOG, "Connection %d picked, session enum started", index);
    return 1;
}

/**
 * Enumerate available sessions on the selected connection.
 * Mirrors DXPlay::EnumerateSessions().
 * Caches results: GUIDs at stride 0x10, names at stride 0x3C.
 * Returns count of discovered sessions.
 */
int td5_net_enumerate_sessions(void)
{
    /* [PERF 2026-06-06] Do NOT zero s_enum_session_count here: the WS2 transport is
     * now non-blocking + INCREMENTAL and owns the accumulator across frames (it
     * resets on a fresh discovery window / >1.5s idle). Zeroing every call would
     * wipe the partial results between frames. The transport returns the current
     * count; callers poll this each frame while the LAN browser is open. */
    if (s_transport_enum) {
        s_enum_session_count = s_transport_enum();
        if (s_enum_session_count > MAX_ENUM_SESSIONS)
            s_enum_session_count = MAX_ENUM_SESSIONS;
    } else {
        s_enum_session_count = 0;
    }

    TD5_LOG_D(NET_LOG, "Enumerated %d session(s)", s_enum_session_count);
    return s_enum_session_count;
}

/* ========================================================================
 * Public API: Messaging
 * ======================================================================== */

/**
 * Send a message of given DXPTYPE.
 * Mirrors DXPlay::SendMessageA().
 *
 * Dispatches outbound messages by type:
 *   - Type 0 (FRAME): handled internally by HandlePad{Host,Client}
 *   - Type 1 (DATA): broadcast to all (DPID=0)
 *   - Type 2 (CHAT): broadcast to all
 *   - Type 3 (DATA_TARGETED): broadcast to all
 *   - Type 4 (START): targeted to each active slot
 *   - Type 5 (ACK_REQ): targeted to each active client
 *   - Type 8 (ROSTER): broadcast to all
 *   - Type 9 (DISCONNECT): targeted to specific player
 *   - Type 10 (RESYNCREQ): targeted to each active client (sets up ack counters)
 */
/* ========================================================================
 * S31 -- race-config replication API
 * ======================================================================== */

/** Record this machine's car/paint pick; clients also announce it to the
 *  host over the control channel. Called on every lobby entry (including
 *  the return from CHANGE CAR). */
void td5_net_set_local_car(int car_index, int paint_index, int td6_color)
{
    if (s_local_slot >= 0 && s_local_slot < TD5_NET_MAX_PLAYERS) {
        s_slot_car[s_local_slot]       = car_index;
        s_slot_paint[s_local_slot]     = paint_index;
        s_slot_td6_color[s_local_slot] = td6_color;
    }
    if (!s_is_host && s_ws2_socket != INVALID_SOCKET && s_local_slot >= 0) {
        CarInfoMsg msg;
        memset(&msg, 0, sizeof(msg));
        msg.magic     = WS2_DISCOVERY_MAGIC;
        msg.disc_type = WS2_DISC_CAR_INFO;
        msg.slot      = (uint32_t)s_local_slot;
        msg.car       = car_index;
        msg.paint     = paint_index;
        msg.td6_color = td6_color;
        sendto(s_ws2_socket, (const char *)&msg, (int)sizeof(msg), 0,
               (const struct sockaddr *)&s_ws2_host_addr,
               (int)sizeof(s_ws2_host_addr));
    }
}

/** The latest announced TD6 body colour for a slot (-1 = none chosen). */
int td5_net_get_slot_td6_color(int slot)
{
    if (slot < 0 || slot >= TD5_NET_MAX_PLAYERS)
        return -1;
    return s_slot_td6_color[slot];
}

/** Host: the latest announced car/paint for a slot (-1 car = none yet). */
int td5_net_get_slot_car(int slot, int *car_index, int *paint_index)
{
    if (slot < 0 || slot >= TD5_NET_MAX_PLAYERS) return 0;
    if (car_index)   *car_index   = s_slot_car[slot];
    if (paint_index) *paint_index = s_slot_paint[slot];
    return 1;
}

/** The armed race config (host: as broadcast; client: as received). */
int td5_net_get_race_config(TD5_NetRaceConfig *out)
{
    /* [SEC 2026-06-15] Seqlock read: retry until we capture a snapshot taken
     * outside any writer's update window (even sequence, unchanged across the
     * copy). The config changes at most once per race, so this almost never
     * spins; the bounded loop just guarantees forward progress. */
    int tries;
    for (tries = 0; tries < 1000; tries++) {
        LONG s1 = InterlockedCompareExchange(&s_race_config_seq, 0, 0);
        TD5_NetRaceConfig tmp;
        int  valid;
        LONG s2;
        if (s1 & 1)
            continue;                       /* writer mid-update */
        valid = s_race_config_valid;
        tmp   = s_race_config;              /* struct snapshot */
        s2    = InterlockedCompareExchange(&s_race_config_seq, 0, 0);
        if (s1 == s2) {                     /* no write straddled the copy */
            if (!valid) return 0;
            if (out) *out = tmp;
            return 1;
        }
    }
    /* Pathological contention fallback (effectively unreachable). */
    if (!s_race_config_valid) return 0;
    if (out) *out = s_race_config;
    return 1;
}

int td5_net_send(TD5_NetMsgType type, const void *data, int size)
{
    uint8_t buf[RING_ENTRY_SIZE];
    int total_size;
    int i;

    if (!s_transport_send)
        return 0;

    /* Build wire message: prepend type DWORD */
    if (size + 4 > (int)sizeof(buf)) {
        TD5_LOG_E(NET_LOG, "Send message too large: type=%d size=%d", type, size);
        return 0;
    }

    memcpy(buf, &type, 4);
    if (data && size > 0)
        memcpy(buf + 4, data, (size_t)size);
    total_size = size + 4;

    switch (type) {
    case TD5_DXPFRAME:
        /* Frame messages are sent by the per-frame sync functions, not here */
        TD5_LOG_W(NET_LOG, "Use handle_host/client_frame for DXPFRAME, not send()");
        return 0;

    case TD5_DXPDATA:
    case TD5_DXPDATA_TARGETED: {
        /* Wire layout [4B type][4B payload_size][N payload] -- handle_data /
         * handle_data_targeted read the size dword at +4 and the payload at
         * +8. td5_net_send used to omit the size field, so receivers parsed
         * the first 4 payload bytes as the length and surfaced the REMAINDER
         * as the message: the lobby kick (payload[0]=0x12) arrived as zeros
         * and was silently dropped. */
        uint8_t wire[RING_ENTRY_SIZE];
        uint32_t psz = (uint32_t)size;
        if (size + 8 > (int)sizeof(wire)) {
            TD5_LOG_E(NET_LOG, "Send message too large: type=%d size=%d", type, size);
            return 0;
        }
        memcpy(wire, &type, 4);
        memcpy(wire + 4, &psz, 4);
        if (data && size > 0)
            memcpy(wire + 8, data, (size_t)size);
        s_transport_send(0, wire, size + 8);
        break;
    }
    case TD5_DXPCHAT:
        /* Broadcast to all ([4B type][string] -- handle_chat reads at +4) */
        s_transport_send(0, buf, total_size);
        break;

    case TD5_DXPSTART:
        /* Host sends START to each active client, DISCONNECT to inactive */
        if (!s_is_host) return 0;
        s_pending_ack_count = 0;
        s_expected_ack_count = 0;
        /* S31: archive the race config we are about to broadcast so the
         * host's own launch path reads the exact bytes the clients get. */
        if (data && size >= (int)sizeof(TD5_NetRaceConfig)) {
            race_config_publish(data);
        }
        for (i = 0; i < TD5_NET_MAX_PLAYERS; i++) {
            if (i == s_local_slot) continue;
            if (s_roster[i].active) {
                /* S31: the full payload (race config) rides with the START */
                s_transport_send(s_roster[i].id, buf, total_size);
                s_pending_ack_count++;
                s_expected_ack_count++;
            }
        }
        break;

    case TD5_DXPACK_REQUEST:
        /* Host -> each active client */
        if (!s_is_host) return 0;
        for (i = 0; i < TD5_NET_MAX_PLAYERS; i++) {
            if (i == s_local_slot) continue;
            if (!s_roster[i].active) continue;
            s_transport_send(s_roster[i].id, buf, total_size);
        }
        break;

    case TD5_DXPACK_REPLY:
        /* Client -> host */
        if (s_is_host) return 0;
        {
            uint32_t host_id = 0;
            for (i = 0; i < TD5_NET_MAX_PLAYERS; i++) {
                if (s_roster[i].active && i != s_local_slot) {
                    host_id = s_roster[i].id;
                    break;
                }
            }
            s_transport_send(host_id, buf, total_size);
        }
        break;

    case TD5_DXPSTART_CONFIRM:
    case TD5_DXPROSTER:
        /* Broadcast */
        s_transport_send(0, buf, total_size);
        break;

    case TD5_DXPDISCONNECT:
        /* Targeted -- data contains target info or broadcast */
        s_transport_send(0, buf, total_size);
        break;

    case TD5_DXPRESYNCREQ:
        /* Host -> each active client; set up ack counters */
        if (!s_is_host) return 0;
        s_pending_ack_count = 0;
        s_expected_ack_count = 0;
        for (i = 0; i < TD5_NET_MAX_PLAYERS; i++) {
            if (i == s_local_slot) continue;
            if (!s_roster[i].active) continue;
            s_pending_ack_count++;
            s_expected_ack_count++;
            s_transport_send(s_roster[i].id, buf, total_size);
        }
        if (s_expected_ack_count == 0)
            s_connection_lost = 1;
        break;

    case TD5_DXPRESYNC:
        /* Client -> host */
        {
            uint32_t host_id = 0;
            for (i = 0; i < TD5_NET_MAX_PLAYERS; i++) {
                if (s_roster[i].active && i != s_local_slot) {
                    host_id = s_roster[i].id;
                    break;
                }
            }
            s_transport_send(host_id, buf, total_size);
        }
        break;

    case TD5_DXPRESYNCACK:
        /* Host -> each active client */
        for (i = 0; i < TD5_NET_MAX_PLAYERS; i++) {
            if (i == s_local_slot) continue;
            if (!s_roster[i].active) continue;
            s_transport_send(s_roster[i].id, buf, total_size);
        }
        break;

    default:
        TD5_LOG_W(NET_LOG, "Unknown send type %d", type);
        return 0;
    }

    return 1;
}

/**
 * Dequeue a received message from the 16-entry ring buffer.
 * Mirrors DXPlay::ReceiveMessage().
 *
 * Returns 1 if a message was dequeued, 0 if the ring is empty.
 * The data pointer is valid until the next receive call.
 */
int td5_net_receive(TD5_NetMsgType *type, void **data, int *size)
{
    uint32_t raw_type;

    if (!ring_pop(&raw_type, data, size))
        return 0;

    if (type)
        *type = (TD5_NetMsgType)raw_type;

    return 1;
}

/* ========================================================================
 * Public API: State Query
 * ======================================================================== */

int td5_net_is_host(void)
{
    return s_is_host;
}

int td5_net_get_player_count(void)
{
    return s_player_count;
}

int td5_net_is_active(void)
{
    return s_active;
}

/* [S31] Race over: drop the lockstep-active latch + per-frame sync state.
 * Without this, td5_net_is_active() stayed TRUE after a race, and the lobby's
 * client auto-launch ("DXPSTART rendezvous active") fired the instant anyone
 * re-entered or joined the lobby -- the race re-started immediately. */
void td5_net_race_done(void)
{
    int i;
    if (!s_active)
        return;
    s_active = 0;
    s_sync_sequence = 0;
    s_waiting_for_frame_ack = 0;
    for (i = 0; i < TD5_NET_MAX_PLAYERS; i++)
        s_player_sync_received[i] = 0;
    TD5_LOG_I(NET_LOG, "Race done: lockstep sync deactivated");
}
int td5_net_local_slot(void)
{
    return s_local_slot;
}

int td5_net_is_slot_active(int slot)
{
    if (slot < 0 || slot >= TD5_NET_MAX_PLAYERS)
        return 0;
    return s_roster[slot].active ? 1 : 0;
}

int td5_net_is_connection_lost(void)
{
    return s_connection_lost;
}

/* [ITEM 3 2026-06-16] Milliseconds since the client last heard from the host
 * (host ping + roster-info keepalive run at 1 Hz in the lobby). Returns -1 if
 * inapplicable (we are the host, not a client, or no host packet seen yet) so
 * the lobby watchdog only arms after the link is genuinely established. */
int td5_net_lobby_host_silence_ms(void)
{
    if (s_is_host || !s_is_client || s_client_last_host_ms == 0)
        return -1;
    return (int)(td5_plat_time_ms() - s_client_last_host_ms);
}

/* [2026-06-16] Re-baseline the host-keepalive clock to "now" (clients only).
 * Called when the client (re)enters the lobby so a long detour through a
 * sub-screen (Change Car / Select Track) does not carry STALE silence that
 * instantly trips the watchdog -- the host gets a fresh window to prove it is
 * alive (a real keepalive lands within ~1 s; a genuinely dead host still trips
 * after the threshold). No-op for the host / before the first host packet. */
void td5_net_lobby_touch_host_clock(void)
{
    if (s_is_host || !s_is_client || s_client_last_host_ms == 0)
        return;
    {
        uint32_t now = td5_plat_time_ms();
        if (now == 0) now = 1;
        s_client_last_host_ms = now;
    }
}

int td5_net_get_enum_session_count(void)
{
    return s_enum_session_count;
}

const char *td5_net_get_enum_session_name(int index)
{
    if (index < 0 || index >= s_enum_session_count)
        return "";
    return s_enum_sessions[index].name;
}

int td5_net_get_enum_session_info(int index, int *player_count, int *max_players)
{
    if (index < 0 || index >= s_enum_session_count)
        return 0;
    if (player_count) *player_count = s_enum_sessions[index].player_count;
    if (max_players)  *max_players  = s_enum_sessions[index].max_players;
    return 1;
}
