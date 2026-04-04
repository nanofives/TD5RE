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

/** Discovery message types */
#define WS2_DISC_QUERY      1
#define WS2_DISC_ANNOUNCE   2
#define WS2_DISC_JOIN_REQ   3
#define WS2_DISC_JOIN_ACK   4

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
typedef int (*TransportEnumFn)(void);
typedef void (*TransportShutdownFn)(void);

static TransportSendFn      s_transport_send;
static TransportRecvFn      s_transport_recv;
static TransportInitFn      s_transport_init;
static TransportHostFn      s_transport_host;
static TransportJoinFn      s_transport_join;
static TransportEnumFn      s_transport_enum;
static TransportShutdownFn  s_transport_shutdown;

/* --- Discovery protocol message (sent on WS2_DISCOVERY_PORT) --- */
#pragma pack(push, 1)
typedef struct DiscoveryMsg {
    uint32_t    magic;
    uint32_t    disc_type;
    char        session_name[64];
    uint32_t    player_count;
    uint32_t    max_players;
    uint32_t    game_type;
    uint32_t    sealed;
    uint16_t    game_port;
    uint32_t    assigned_slot;
    uint32_t    assigned_id;
} DiscoveryMsg;
#pragma pack(pop)

/* --- Winsock2 UDP backend state --- */
static SOCKET       s_ws2_socket = INVALID_SOCKET;
static WSAEVENT     s_ws2_event = WSA_INVALID_EVENT;
static int          s_ws2_started;
static int          s_ws2_socket_bound;
static int          s_receive_event_is_ws2;
static SOCKADDR_IN  s_ws2_host_addr;
static SOCKADDR_IN  s_ws2_peer_addrs[TD5_NET_MAX_PLAYERS];
static int          s_ws2_peer_valid[TD5_NET_MAX_PLAYERS];
static SOCKET       s_ws2_disc_socket = INVALID_SOCKET;
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
static int              ws2_transport_enum(void);
static void             ws2_transport_shutdown(void);
static int              ws2_bind_socket(u_short port);
static uint32_t         ws2_resolve_sender_id(const SOCKADDR_IN *addr);
static const SOCKADDR_IN *ws2_get_target_addr(uint32_t target_id);
static int              ws2_discovery_init(void);
static void             ws2_discovery_shutdown(void);
static void             ws2_handle_join_request(const SOCKADDR_IN *from_addr);

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
            /* Receive event signaled -- drain all pending messages */
            uint8_t recv_buf[RING_ENTRY_SIZE];
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

/**
 * Type 4 -- DXPSTART (4 bytes)
 * Host signals race start to each client.
 * Client responds with ACK_REPLY (type 6).
 * Queued so game thread can transition to race.
 */
static void handle_start(uint32_t sender, const void *data, int size)
{
    (void)sender;
    (void)data;
    (void)size;

    TD5_LOG_I(NET_LOG, "Received DXPSTART");

    /* Queue for game thread */
    ring_push(TD5_DXPSTART, NULL, 0);

    /* Auto-reply with ACK_REPLY if we are a client */
    if (!s_is_host) {
        uint32_t reply_type = TD5_DXPACK_REPLY;
        send_to_player(s_roster[s_local_slot].id, reply_type, &reply_type, 4);
        /* Actually send to host, not to self */
        if (s_transport_send) {
            uint32_t buf[1] = { TD5_DXPACK_REPLY };
            /* Find host ID */
            int i;
            for (i = 0; i < TD5_NET_MAX_PLAYERS; i++) {
                if (s_roster[i].active && i != s_local_slot) {
                    /* In the original, send to g_hostDirectPlayPlayerId.
                       We find the host slot (slot with lowest ID or marked host). */
                    break;
                }
            }
            s_transport_send(0, buf, 4); /* send to host (id 0 = host convention) */
        }
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
    (void)sender;
    (void)data;
    (void)size;

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

    if (!ws2_bind_socket(WS2_GAME_PORT)) {
        TD5_LOG_E(NET_LOG, "Host: failed to bind game port %u", (unsigned)WS2_GAME_PORT);
        return 0;
    }

    memset(&s_ws2_host_addr, 0, sizeof(s_ws2_host_addr));
    s_ws2_host_addr.sin_family = AF_INET;
    s_ws2_host_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    s_ws2_host_addr.sin_port = htons(WS2_GAME_PORT);

    s_ws2_peer_addrs[0] = s_ws2_host_addr;
    s_ws2_peer_valid[0] = 1;

    ws2_discovery_init();

    TD5_LOG_I(NET_LOG, "Host: bound on port %u, discovery active", (unsigned)WS2_GAME_PORT);
    return 1;
}

static int ws2_transport_join(int session_index)
{
    DiscoveryMsg join_req;
    SOCKADDR_IN disc_addr;
    uint32_t hello[2];

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

    /* Send join request to host's discovery port */
    memset(&join_req, 0, sizeof(join_req));
    join_req.magic = WS2_DISCOVERY_MAGIC;
    join_req.disc_type = WS2_DISC_JOIN_REQ;

    disc_addr = s_ws2_host_addr;
    disc_addr.sin_port = htons(WS2_DISCOVERY_PORT);

    if (s_ws2_disc_socket == INVALID_SOCKET)
        ws2_discovery_init();

    if (s_ws2_disc_socket != INVALID_SOCKET) {
        sendto(s_ws2_disc_socket, (const char *)&join_req, sizeof(join_req), 0,
               (const struct sockaddr *)&disc_addr, (int)sizeof(disc_addr));
    }

    /* Send a hello on the game socket so host learns our address */
    hello[0] = TD5_DXPDATA;
    hello[1] = 0;
    sendto(s_ws2_socket, (const char *)hello, 8, 0,
           (const struct sockaddr *)&s_ws2_host_addr,
           (int)sizeof(s_ws2_host_addr));

    TD5_LOG_I(NET_LOG, "Join: connected to host at port %u",
              (unsigned)ntohs(s_ws2_host_addr.sin_port));
    return 1;
}

static int ws2_transport_enum(void)
{
    SOCKADDR_IN bcast_addr, lo_addr;
    DiscoveryMsg query;
    DWORD start_tick;
    int count = 0;

    memset(s_enum_sessions, 0, sizeof(s_enum_sessions));
    memset(s_ws2_enum_host_addrs, 0, sizeof(s_ws2_enum_host_addrs));

    if (s_ws2_disc_socket == INVALID_SOCKET) {
        if (!ws2_discovery_init())
            return 0;
    }

    memset(&query, 0, sizeof(query));
    query.magic = WS2_DISCOVERY_MAGIC;
    query.disc_type = WS2_DISC_QUERY;

    /* Broadcast on LAN */
    memset(&bcast_addr, 0, sizeof(bcast_addr));
    bcast_addr.sin_family = AF_INET;
    bcast_addr.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    bcast_addr.sin_port = htons(WS2_DISCOVERY_PORT);
    sendto(s_ws2_disc_socket, (const char *)&query, sizeof(query), 0,
           (const struct sockaddr *)&bcast_addr, (int)sizeof(bcast_addr));

    /* Also try loopback for same-machine testing */
    memset(&lo_addr, 0, sizeof(lo_addr));
    lo_addr.sin_family = AF_INET;
    lo_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    lo_addr.sin_port = htons(WS2_DISCOVERY_PORT);
    sendto(s_ws2_disc_socket, (const char *)&query, sizeof(query), 0,
           (const struct sockaddr *)&lo_addr, (int)sizeof(lo_addr));

    /* Poll for responses up to 500ms */
    start_tick = GetTickCount();
    while ((GetTickCount() - start_tick) < 500 && count < MAX_ENUM_SESSIONS) {
        DiscoveryMsg resp;
        SOCKADDR_IN from_addr;
        int from_len = (int)sizeof(from_addr);
        int ret;
        fd_set readfds;
        struct timeval tv;

        FD_ZERO(&readfds);
        FD_SET(s_ws2_disc_socket, &readfds);
        tv.tv_sec = 0;
        tv.tv_usec = 50000;

        if (select(0, &readfds, NULL, NULL, &tv) <= 0)
            continue;

        ret = recvfrom(s_ws2_disc_socket, (char *)&resp, sizeof(resp), 0,
                       (struct sockaddr *)&from_addr, &from_len);
        if (ret < (int)sizeof(DiscoveryMsg))
            continue;
        if (resp.magic != WS2_DISCOVERY_MAGIC)
            continue;
        if (resp.disc_type != WS2_DISC_ANNOUNCE)
            continue;
        if (resp.sealed)
            continue;

        strncpy(s_enum_sessions[count].name, resp.session_name,
                sizeof(s_enum_sessions[count].name) - 1);
        s_enum_sessions[count].player_count = resp.player_count;
        s_enum_sessions[count].max_players  = resp.max_players;
        s_enum_sessions[count].game_type    = resp.game_type;

        s_ws2_enum_host_addrs[count] = from_addr;
        s_ws2_enum_host_addrs[count].sin_port = htons(resp.game_port);

        TD5_LOG_I(NET_LOG, "Enum: found session \"%s\" (%u/%u)",
                  resp.session_name, resp.player_count, resp.max_players);
        count++;
    }

    TD5_LOG_I(NET_LOG, "Enum: discovered %d session(s)", count);
    return count;
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
    int from_len = (int)sizeof(from_addr);
    int ret;

    if (s_ws2_socket == INVALID_SOCKET || !buf || buf_size <= 0 || !s_ws2_socket_bound) {
        return 0;
    }

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

    if (sender_id) {
        *sender_id = ws2_resolve_sender_id(&from_addr);
    }
    return ret;
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

    if (s_ws2_socket == INVALID_SOCKET || !data || size <= 0 || !s_ws2_socket_bound) {
        return 0;
    }

    if (target_id == 0) {
        if (s_is_host) {
            int sent_any = 0;
            for (i = 0; i < TD5_NET_MAX_PLAYERS; i++) {
                if (i == s_local_slot || !s_roster[i].active || !s_ws2_peer_valid[i]) {
                    continue;
                }
                sent = sendto(s_ws2_socket, (const char *)data, size, 0,
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

    sent = sendto(s_ws2_socket, (const char *)data, size, 0,
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
}

static void ws2_handle_join_request(const SOCKADDR_IN *from_addr)
{
    int slot = -1;
    int i;
    DiscoveryMsg ack;

    if (!s_is_host || s_session.sealed)
        return;

    for (i = 1; i < TD5_NET_MAX_PLAYERS; i++) {
        if (!s_roster[i].active) {
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        TD5_LOG_W(NET_LOG, "Join request rejected: no free slots");
        return;
    }

    s_roster[slot].id = (uint32_t)(slot + 1);
    s_roster[slot].active = 1;
    snprintf(s_roster[slot].name, sizeof(s_roster[slot].name), "Player %d", slot + 1);
    s_player_count++;

    s_ws2_peer_addrs[slot] = *from_addr;
    s_ws2_peer_valid[slot] = 1;

    memset(&ack, 0, sizeof(ack));
    ack.magic = WS2_DISCOVERY_MAGIC;
    ack.disc_type = WS2_DISC_JOIN_ACK;
    ack.assigned_slot = (uint32_t)slot;
    ack.assigned_id   = s_roster[slot].id;
    ack.game_port     = ntohs(s_ws2_host_addr.sin_port);
    strncpy(ack.session_name, s_session.name, sizeof(ack.session_name) - 1);

    sendto(s_ws2_disc_socket, (const char *)&ack, sizeof(ack), 0,
           (const struct sockaddr *)from_addr, (int)sizeof(*from_addr));

    broadcast_roster();
    InterlockedIncrement(&s_sync_generation);

    TD5_LOG_I(NET_LOG, "Join accepted: slot=%d id=%u", slot, s_roster[slot].id);
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

    memset(s_player_sync_table, 0, sizeof(s_player_sync_table));
    memset(s_player_sync_received, 0, sizeof(s_player_sync_received));
    memset(&s_outbound_frame, 0, sizeof(s_outbound_frame));

    s_evt_receive = NULL;
    s_evt_stop = NULL;
    s_evt_frame_ack = NULL;
    s_evt_sync = NULL;
    s_receive_event_is_ws2 = 0;

    /* Install Winsock2 UDP transport backend */
    s_transport_send     = ws2_transport_send;
    s_transport_recv     = ws2_transport_recv;
    s_transport_init     = ws2_transport_init;
    s_transport_host     = ws2_transport_host;
    s_transport_join     = ws2_transport_join;
    s_transport_enum     = ws2_transport_enum;
    s_transport_shutdown = ws2_transport_shutdown;

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

    /* Stop worker thread and close events */
    stop_worker();

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
    if (!s_initialized)
        return;

    /* Poll the discovery socket for queries (host) and join requests */
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
                if (s_is_host) {
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
                    announce.game_port    = WS2_GAME_PORT;
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
                    s_local_slot = (int)disc_msg.assigned_slot;
                    if (s_local_slot >= 0 && s_local_slot < TD5_NET_MAX_PLAYERS) {
                        s_roster[s_local_slot].id = disc_msg.assigned_id;
                        s_roster[s_local_slot].active = 1;
                        TD5_LOG_I(NET_LOG, "Assigned slot %d, id %u",
                                  s_local_slot, disc_msg.assigned_id);
                    }
                }
                break;

            default:
                break;
            }
        }
    }

    /* Check for connection loss */
    if (s_connection_lost) {
        TD5_LOG_W(NET_LOG, "Connection lost detected in tick");
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
int td5_net_create_session(const char *name, const char *player_name, int max_players)
{
    if (!s_initialized) {
        TD5_LOG_E(NET_LOG, "Cannot create session: not initialized");
        return 0;
    }

    if (max_players < 1) max_players = 1;
    if (max_players > TD5_NET_MAX_PLAYERS) max_players = TD5_NET_MAX_PLAYERS;

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

    /* Ask transport to host */
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
    }
    s_player_count = 1;

    /* Reset frame ack event (manual-reset, create fresh for session) */
    ResetEvent(s_evt_frame_ack);

    /* Broadcast initial roster */
    broadcast_roster();

    TD5_LOG_I(NET_LOG, "Session created: \"%s\" (max=%d, seed=%u)",
              s_session.name, max_players, s_session.seed);
    return 1;
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

    /* Ask transport to join */
    if (s_transport_join) {
        if (!s_transport_join(session_index)) {
            TD5_LOG_E(NET_LOG, "Transport failed to join session %d", session_index);
            return 0;
        }
    }

    /* Set client flags */
    s_is_host = 0;
    s_is_client = 1;
    s_connection_lost = 0;

    /* Local player -- slot will be assigned by host via roster message */
    s_local_slot = -1; /* will be resolved when roster arrives */
    memset(s_roster, 0, sizeof(s_roster));

    /* Store our player name for later slot assignment */
    /* The host will assign our slot and send a DXPROSTER update */
    if (player_name) {
        /* Temporarily store in slot 0; will be relocated on roster update */
        strncpy(s_roster[0].name, player_name, sizeof(s_roster[0].name) - 1);
        s_roster[0].name[sizeof(s_roster[0].name) - 1] = '\0';
    }

    /* Refresh roster */
    refresh_roster();

    TD5_LOG_I(NET_LOG, "Joined session index %d as \"%s\"",
              session_index, player_name ? player_name : "(unnamed)");
    return 1;
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

    wait_result = WaitForSingleObject(s_evt_frame_ack, SYNC_TIMEOUT_MS);
    if (wait_result != WAIT_OBJECT_0) {
        s_waiting_for_frame_ack = 0;
        TD5_LOG_W(NET_LOG, "Host HandlePadHost: timed out waiting for client inputs");
        return 0; /* Caller sets ESC bit, disconnects */
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

    /* Broadcast to all players (DPID=0 = broadcast) */
    send_frame_to_all(&s_outbound_frame);

    /* Increment sequence counter */
    s_sync_sequence++;

    /* Reset per-frame receive flags */
    for (i = 0; i < TD5_NET_MAX_PLAYERS; i++)
        s_player_sync_received[i] = 0;

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

    if (!s_active)
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

    if (s_transport_send)
        s_transport_send(host_id, &send_frame, FRAME_MSG_SIZE);

    /* 4. Wait for host's broadcast */
    s_waiting_for_frame_ack = 1;
    ResetEvent(s_evt_frame_ack);

    wait_result = WaitForSingleObject(s_evt_frame_ack, SYNC_TIMEOUT_MS);
    s_waiting_for_frame_ack = 0;

    if (wait_result != WAIT_OBJECT_0) {
        TD5_LOG_W(NET_LOG, "Client HandlePadClient: timed out waiting for host broadcast");
        return 0; /* Caller sets ESC bit, disconnects */
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
    s_enum_session_count = 0;

    if (s_transport_enum) {
        s_enum_session_count = s_transport_enum();
        if (s_enum_session_count > MAX_ENUM_SESSIONS)
            s_enum_session_count = MAX_ENUM_SESSIONS;
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
    case TD5_DXPCHAT:
    case TD5_DXPDATA_TARGETED:
        /* Broadcast to all */
        s_transport_send(0, buf, total_size);
        break;

    case TD5_DXPSTART:
        /* Host sends START to each active client, DISCONNECT to inactive */
        if (!s_is_host) return 0;
        s_pending_ack_count = 0;
        s_expected_ack_count = 0;
        for (i = 0; i < TD5_NET_MAX_PLAYERS; i++) {
            if (i == s_local_slot) continue;
            if (s_roster[i].active) {
                /* Check if this slot is participating (from data bitmask) */
                uint32_t start_msg[1] = { TD5_DXPSTART };
                s_transport_send(s_roster[i].id, start_msg, 4);
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
