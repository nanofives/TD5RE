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

/* --- State query --- */
int  td5_net_is_host(void);
int  td5_net_get_player_count(void);
int  td5_net_is_active(void);
int  td5_net_is_slot_active(int slot);  /* dpu_exref[0xBCC + slot*4]: 1=local, 0=remote/empty */
int  td5_net_local_slot(void);
int  td5_net_is_connection_lost(void);
int  td5_net_get_enum_session_count(void);
const char *td5_net_get_enum_session_name(int index);

#endif /* TD5_NET_H */
