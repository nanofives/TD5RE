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
#define TD5_NET_UPNP_PORT_CONFLICT 6 /* IGD found but the port is already        */
                                     /* forwarded (static rule) -> 714/718       */

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
/* Last join rejection reason: 0=none, 1=session full, 2=wrong/again password,
 * 3=protocol version mismatch (peer runs a different td5re build). */
int  td5_net_get_join_nak_reason(void);
const char *td5_net_get_slot_name(int slot);    /* "" if empty */
int  td5_net_get_slot_latency_ms(int slot);     /* -1 = unknown (e.g. self/host) */

/* --- Per-frame sync --- */
int  td5_net_handle_host_frame(uint32_t *control_bits, float *frame_dt);
int  td5_net_handle_client_frame(uint32_t *control_bits, float *frame_dt);
/* Non-blocking frame poll variants used by td5_game.c's net race loop. */
int  td5_net_host_frame_nb(uint32_t *control_bits, float *frame_dt);
int  td5_net_client_frame_nb(uint32_t *control_bits, float *frame_dt);

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
/* Re-baseline the host-keepalive clock (client only) so a long detour through a
 * lobby sub-screen doesn't carry stale silence into the watchdog. */
void td5_net_lobby_touch_host_clock(void);
int  td5_net_get_enum_session_count(void);
const char *td5_net_get_enum_session_name(int index);
int  td5_net_get_enum_session_info(int index, int *player_count, int *max_players);

/* --- Protocol version (2026-08-19, Phase 0) -----------------------------
 * Every wire-visible change to this file MUST bump this number in the same
 * commit. It is exchanged in the JOIN handshake (DiscoveryMsg.proto_version)
 * and echoed in TD5_NetRaceConfig, so a peer running a different build is
 * rejected at the lobby door with WS2_NAK_VERSION instead of joining and
 * desyncing on lap 1 -- lockstep has no state correction, so a silent
 * mismatch is unrecoverable and looks like a physics bug.
 *
 * History:
 *   1 -- first versioned protocol. Adds DiscoveryMsg.proto_version and
 *        TD5_NetRaceConfig.{proto_version,track_fingerprint}. Every build
 *        before this is version 0 and is refused (it cannot parse these
 *        fields, and its TD5_NetRaceConfig is 428 bytes, not 436). */
#define TD5_NET_PROTO_VERSION 1

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
    /* [POLICE rewrite 2026-06-19] Traffic + cops run in net races now (the
     * spawner + chase are lockstep-deterministic). These replicate the HOST's
     * choices so spawn caps + the cop cadence match on every peer — a
     * per-machine difference here would desync the spawner. */
    int32_t  traffic_volume;      /* 0=Off..4=VeryHigh (host's [GameOptions] Traffic) */
    int32_t  cops;                /* POLICE option: 1 = cops on, 0 = off */
    /* [NET GAME MODES 2026-07-04] Replicate the host's DYNAMICS choice
     * (0=ARCADE, 1=SIMULATION). Arcade adds 3x-collision launch + collectible
     * power-up boxes (td5_arcade); the box layout + pickups are deterministic by
     * design, but a per-machine ARCADE/SIM mismatch would place boxes on some
     * peers only and desync the pickup effects. Also gates Traffic Battle's
     * power-up boxes (they appear only in arcade dynamics). */
    int32_t  dynamics;            /* 0=ARCADE, 1=SIMULATION */
    int32_t  td6_color[6];        /* per-slot TD6 body RGB (0xFFFFFF = unpainted) */
    int32_t  car_index[6];        /* per net slot (TD5_NET_MAX_PLAYERS) */
    int32_t  paint_index[6];
    /* [NET GAME MODES 2026-07-04] Cup progression: which cup race this is
     * (0-based). The host advances it between races and broadcasts it so every
     * peer runs the same cup race (track + "race X of Y" + standings stay in
     * lockstep). -1 = not a cup race. */
    int32_t  cup_race_index;
    /* [MP GAME MODES 2026-06-22] Replicated game mode + per-mode options chosen
     * on the host's mode-vote/mode-config screens. All-int32 layout; copied
     * wholesale with the rest of the config (the DXPSTART payload + race_config
     * memcpy already use sizeof(TD5_NetRaceConfig), so growing the struct
     * replicates these fields with no extra wire code). */
    TD5_MpModeConfig mode_config;
    /* [RACE OPTIONS CONSOLIDATION 2026-07-21] The RACE OPTIONS screen is now the
     * host's single game-behaviour surface, so the remaining behaviour options it
     * edits are replicated too (host sets, all peers adopt). All are lockstep-
     * relevant: power-up box layout/pickups, damage model (toughness/deform/master
     * switch), V2V/V2W collision, and checkpoint timers must match on every peer
     * or the sim diverges. Appended (all int32) — copied wholesale by the existing
     * sizeof(TD5_NetRaceConfig) memcpy, no wire code. LANE ASSIST / TUTORIAL /
     * PLAYER NAME stay local-only (per-player / cosmetic, not sim state). */
    int32_t  powerups;            /* 0=OFF 1=CASUAL 2=CHAOS */
    int32_t  car_toughness;       /* 0=Low 1=Medium 2=High 3=Off */
    int32_t  car_deform;          /* 0=Low 1=Normal 2=High 3=Off */
    int32_t  car_damage;          /* master car-damage + HUD bar toggle */
    int32_t  collisions;          /* 3D collisions on/off */
    int32_t  checkpoint_timers;   /* checkpoint-timer system on/off */
    /* [NET PROTO PHASE 0 2026-08-19] Appended, all-uint32, same wire rules as
     * the rest of the struct. */
    uint32_t proto_version;       /* TD5_NET_PROTO_VERSION of the composing host */
    /* Identity of the track the host actually selected, not just its index.
     * track_index alone is ambiguous for CUSTOM tracks: slot 37+ is whatever
     * sits in that peer's own custom_tracks.json, so two players with
     * different manifests would each load a DIFFERENT track from the same
     * index and desync instantly with no error. Clients recompute this from
     * their own registry on DXPSTART and refuse to start on mismatch.
     * SCOPE: this fingerprints the manifest ENTRY (name/level/circuit/spans),
     * not the level file CONTENT -- two peers with the same manifest but
     * differently-edited level geometry are NOT caught by this. */
    uint32_t track_fingerprint;
} TD5_NetRaceConfig;
/* WIRE CONTRACT -- host->client race setup, exchanged by whole-struct memcpy
 * of sizeof(TD5_NetRaceConfig). All-int32 layout by design (see note above),
 * so it is identical on 32- and 64-bit builds. Adding a member changes the
 * protocol; bump BOTH this number and TD5_NET_PROTO_VERSION deliberately and
 * in step with both endpoints. 428 -> 436 at proto version 1. */
_Static_assert(sizeof(TD5_NetRaceConfig) == 436, "TD5_NetRaceConfig is a wire format -- size must not change");

void td5_net_set_local_car(int car_index, int paint_index, int td6_color);
int  td5_net_get_slot_td6_color(int slot);
int  td5_net_get_slot_car(int slot, int *car_index, int *paint_index);
int  td5_net_get_race_config(TD5_NetRaceConfig *out);
/* [MP RESTART RE-ROLL 2026-07-04] Overwrite just the archived rng_seed field
 * (track/cars/mode_config untouched) so a pause-menu net RESTART draws a fresh
 * cop-chase/wheel-style/traffic roll instead of replaying the exact same seed
 * every time -- mirrors local MP, which already reseeds from GetTickCount() on
 * every restart. Host calls this when composing the RACE_RESTART broadcast;
 * every client calls it after parsing the same seed back out of that
 * broadcast, so all machines land on the identical new value. */
void td5_net_update_race_seed(uint32_t new_seed);

/* [NET PROTO PHASE 0 2026-08-19] Identity of a track SLOT as this machine
 * would load it. Native slots hash the index alone (the schedule is compiled
 * in, so an index means the same track everywhere); custom slots hash the
 * local registry's manifest entry. Never returns 0 for a resolvable slot, so
 * 0 is usable as "not computed". */
uint32_t td5_net_track_fingerprint(int track_index);
/* Non-zero once a client has parsed a DXPSTART it must NOT act on (protocol
 * or track-identity mismatch with the host). The race launcher checks this
 * and aborts back to the lobby instead of starting a guaranteed-desync race.
 * Returns the same WS2_NAK_* vocabulary as the join gate. */
int  td5_net_get_start_reject_reason(void);
void td5_net_clear_start_reject(void);

#endif /* TD5_NET_H */
