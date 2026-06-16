/**
 * td5_net.h -- Multiplayer protocol, lockstep sync
 *
 * Lockstep deterministic: only input bitmasks + frame dt are synced.
 * No game-world state replication. 13 message types (DXPTYPE 0-12).
 *
 * Original functions (DXPlay in M2DX.dll):
 *   DXPlay::Environment, Create, Destroy
 *   DXPlay::ConnectionEnumerate, ConnectionPick
 *   DXPlay::NewSession, JoinSession
 *   DXPlay::SendMessageA, ReceiveMessage
 *   DXPlay::HandlePadHost (per-frame host sync)
 *   DXPlay::HandlePadClient (per-frame client sync)
 *   DXPlay::EnumerateSessions, SealSession
 *   DXPlay::UnSync
 */

#ifndef TD5_NET_H
#define TD5_NET_H

#include "td5_types.h"

/* --- Connection modes (S10 net-play rework) --- */
#define TD5_NET_MODE_LAN        0   /* auto-discovery via LAN broadcast beacon */
#define TD5_NET_MODE_DIRECT     1   /* explicit host/join by IP:port           */

/* --- UPnP IGD port-mapping status (host, Direct mode) --- */
#define TD5_NET_UPNP_IDLE        0  /* not attempted                           */
#define TD5_NET_UPNP_MAPPING     1  /* discovery / SOAP in progress            */
#define TD5_NET_UPNP_MAPPED      2  /* router opened the port (verified)       */
#define TD5_NET_UPNP_FAILED      3  /* attempted, router refused / unreachable */
#define TD5_NET_UPNP_UNAVAILABLE 4  /* disabled by config                      */
#define TD5_NET_UPNP_DOUBLE_NAT  5  /* mapped, but IGD WAN IP is private        */
                                    /* (double-NAT: another router in front)    */

int  td5_net_init(void);
void td5_net_shutdown(void);
void td5_net_tick(void);

/* --- Session management --- */
int  td5_net_create_session(const char *name, const char *player_name, int max_players);
int  td5_net_join_session(int session_index, const char *player_name);
void td5_net_seal_session(int sealed);
void td5_net_unsync(void);

/* --- S10: explicit connection modes --- */
int  td5_net_set_mode(int mode);            /* TD5_NET_MODE_LAN / _DIRECT */
int  td5_net_get_mode(void);
/* Host with an explicit game port + optional UPnP IGD port-mapping. */
int  td5_net_create_session_ex(const char *name, const char *player_name,
                               int max_players, int game_port, int enable_upnp);
/* Join an explicit host by IP (and port); no enumeration (Direct mode). */
int  td5_net_join_direct(const char *host_ip, int game_port, const char *player_name);
int  td5_net_get_upnp_status(void);         /* TD5_NET_UPNP_* */
const char *td5_net_get_status_text(void);  /* human-readable host / connect status */
int  td5_net_get_local_ip(char *buf, int len);

/* --- S10b: lobby session limits + per-slot info --- */
/* Host: set max players (2..6) + an optional join password ("" = open). */
void td5_net_set_session_limits(int max_players, const char *password);
int  td5_net_get_max_players(void);
/* Client: set the password to send with the JOIN request (before joining). */
void td5_net_set_join_password(const char *password);
/* Last join rejection reason: 0=none, 1=session full, 2=wrong/again password. */
int  td5_net_get_join_nak_reason(void);
const char *td5_net_get_slot_name(int slot);    /* "" if empty */
int  td5_net_get_slot_latency_ms(int slot);     /* -1 = unknown (e.g. self/host) */

/* --- Per-frame sync --- */
int  td5_net_handle_host_frame(uint32_t *control_bits, float *frame_dt);
int  td5_net_handle_client_frame(uint32_t *control_bits, float *frame_dt);

/* --- Session browser --- */
int  td5_net_enumerate_connections(void);
int  td5_net_pick_connection(int index);
int  td5_net_enumerate_sessions(void);

/* --- Messaging --- */
int  td5_net_send(TD5_NetMsgType type, const void *data, int size);
int  td5_net_receive(TD5_NetMsgType *type, void **data, int *size);
void td5_net_race_done(void);

/* --- State query --- */
int  td5_net_is_host(void);
int  td5_net_get_player_count(void);
int  td5_net_is_active(void);
int  td5_net_is_slot_active(int slot);  /* dpu_exref[0xBCC + slot*4]: 1=local, 0=remote/empty */
int  td5_net_local_slot(void);
int  td5_net_is_connection_lost(void);
/* [ITEM 3] ms since the client last heard from the host (lobby keepalive);
 * -1 if not a client or no host packet has arrived yet. */
int  td5_net_lobby_host_silence_ms(void);
int  td5_net_get_enum_session_count(void);
const char *td5_net_get_enum_session_name(int index);
int  td5_net_get_enum_session_info(int index, int *player_count, int *max_players);

/* --- S31 network race config (2026-06-10) -------------------------------
 * Host-authoritative race parameters broadcast in the DXPSTART payload so
 * every machine launches the SAME race. Lockstep has no state correction,
 * so a per-machine difference in any of these (track, direction, any
 * slot's carparam, or the RNG stream) is a guaranteed permanent desync. */
typedef struct TD5_NetRaceConfig {
    uint32_t rng_seed;            /* InitRace srand + schedule AI-pick seed */
    int32_t  track_index;
    int32_t  reverse_direction;
    int32_t  lap_count;           /* informational (net races force 4) */
    int32_t  num_opponents;       /* AI opponent count (decides active slots) */
    int32_t  difficulty;          /* difficulty tier (AI car pool row) */
    int32_t  td6_color[6];        /* per-slot TD6 body RGB (0xFFFFFF = unpainted) */
    int32_t  car_index[6];        /* per net slot (TD5_NET_MAX_PLAYERS) */
    int32_t  paint_index[6];
} TD5_NetRaceConfig;

void td5_net_set_local_car(int car_index, int paint_index, int td6_color);
int  td5_net_get_slot_td6_color(int slot);
int  td5_net_get_slot_car(int slot, int *car_index, int *paint_index);
int  td5_net_get_race_config(TD5_NetRaceConfig *out);

#endif /* TD5_NET_H */
