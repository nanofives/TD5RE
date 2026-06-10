# TD5RE Netplay Architecture

Source-port replacement for the original game's DirectPlay 4A layer (DXPlay in M2DX.dll). Implementation: `td5mod/src/td5re/td5_net.c` (+`td5_net.h`), UPnP in `td5_upnp.c`, lobby/browse UI in `td5_fe_net.c`, lockstep consumption in `td5_game.c`. The model is **input lockstep with NO correction**: only per-frame input bitmasks + the host's frame dt cross the wire; every machine runs the full simulation and must stay bit-identical. The port's wire format is **not** interoperable with original `TD5_d3d.exe` peers (`td5_fe_net.c` ~line 789 `[ARCH-DIVERGENCE: DXPTYPE]`).

## 1. Transport

Winsock2 **UDP**, two sockets (`td5_net.c`):

- **Game socket** — non-blocking, `WSAEventSelect(FD_READ|FD_CLOSE)`. Host binds `WS2_GAME_PORT` **37050** (INI `[Network] GamePort`, `main.c:768`); clients bind ephemeral (port 0). Carries all DXPTYPE traffic *and* the magic-tagged control channel (so JOIN crosses NAT via the one opened port).
- **Discovery socket** — bound to **37051** (`WS2_DISCOVERY_PORT`), `SO_BROADCAST|SO_REUSEADDR`, polled in `td5_net_tick()` (max 16 datagrams/frame). Session *listing* only.

All discovery/control datagrams start with magic `0x54443552` ("TD5R"). `ws2_transport_recv` checks dword 0 against the magic **before** DXPTYPE dispatch and routes those to `ws2_handle_game_control`.

Two modes (`td5_net_set_mode`): `TD5_NET_MODE_LAN` (0) and `TD5_NET_MODE_DIRECT` (1).

- **LAN discovery** (`ws2_transport_enum`): non-blocking incremental browser — ~2 s window, re-broadcasts `QUERY` to 255.255.255.255:37051 + loopback every 250 ms, zero-timeout `select` drain, dedup by host addr+game_port, sealed sessions skipped, max 50 (`MAX_ENUM_SESSIONS`). Hosts answer `QUERY` with `ANNOUNCE` and additionally beacon `ANNOUNCE` (broadcast) every 1000 ms while LAN-mode + unsealed (`td5_net_tick`).
- **Direct IP** (`td5_net_join_direct`): resolves `ip[:port]` (inet_addr, getaddrinfo fallback), no enumeration.
- **UPnP** (`td5_upnp.c`, host + Direct mode + `[Network] EnableUPnP` only): SSDP M-SEARCH 239.255.255.250:1900 → device-description HTTP GET → SOAP `AddPortMapping`/`GetSpecificPortMappingEntry` (verify) on the WANIP/WANPPPConnection control URL. Short timeouts (SSDP 2 s, TCP 3 s), best-effort, status in `td5_net_get_upnp_status()` (`TD5_NET_UPNP_*`); mapping deleted in `td5_net_shutdown`. Entirely outside the determinism path.

**Threading** (mirrors original `DirectPlayWorkerThreadProc @0x1000c3e0`): one worker thread waits on {receive WSAEVENT, stop}; drains the socket and dispatches by type via `s_handlers[13]`. Non-frame messages are pushed to a 16-entry lock-free SPSC ring (`RingEntry` = 0x244 bytes: 4B type + 4B size + 0x23C payload; **oldest entry silently overwritten when full**); the game thread pops via `td5_net_receive`. Four Win32 events: receive, stop, `frame_ack` (auto-reset), `sync` (manual-reset).

## 2. Packet protocol (DXPTYPE 0–12)

First uint32 of every non-magic datagram is the type (`TD5_NetMsgType`, `td5_types.h:292`). Handlers in `td5_net.c` `s_handlers[]`.

| Type | Name | Payload (on wire) | Sent when / by |
|---|---|---|---|
| 0 | DXPFRAME | 0x80 B `TD5_NetFrame`: type, `control_bits[6]`, `frame_delta_time` (float), `sync_sequence`, 92 B reserved | Client→host: own slot's input, once per render frame. Host→all: merged frame. |
| 1 | DXPDATA | `[type][u32 payload_size][payload]` | Broadcast; carries frontend lobby opcodes (payload byte 0): 0x12 LOBBY_KICK, 0x13 LOBBY_REQUEST_CONFIG, 0x15 LOBBY_SETTINGS (0x80 B) — legacy path, see §3. |
| 2 | DXPCHAT | `[type][NUL-terminated text]` | Lobby chat submit (`Screen_NetworkLobby` state 4). |
| 3 | DXPDATA_TARGETED | same as DXPDATA | Queued under a distinct ring type. |
| 4 | DXPSTART | 4 B | Host→each client at race launch (lobby state 0x10). Client auto-replies type 6. |
| 5 | DXPACK_REQUEST | 4 B | Host→client; client auto-replies type 6. |
| 6 | DXPACK_REPLY | 4 B | Client→host. When `s_pending_ack_count` hits 0 the host broadcasts type 7, sets `s_active=1`, `sync_sequence=0`. |
| 7 | DXPSTART_CONFIRM | 4 B | Host→all; client sets `s_active=1`, seq=0 → lockstep live. |
| 8 | DXPROSTER | 0x34 B `TD5_NetRoster`: type, `player_ids[6]`, `active_flags[6]`, `host_id` | Host→all on join/leave. A client whose id == `host_id` promotes itself (host migration). |
| 9 | DXPDISCONNECT | 4 B | Sets `s_connection_lost`. |
| 10 | DXPRESYNCREQ | 4 B | Host→clients, resync barrier phase 1. |
| 11 | DXPRESYNC | 4 B | Client→host, phase 2. |
| 12 | DXPRESYNCACK | 4 B | Host→clients, phase 3; both sides reset `sync_sequence=0`. |

**Magic control channel** (`DiscoveryMsg`/`PingMsg`/`RosterInfoMsg`, `#pragma pack(1)`): QUERY(1), ANNOUNCE(2: session name, player/max counts, game_type, sealed, game_port, has_password), JOIN_REQ(3: player name in the `session_name` field + password), JOIN_ACK(4: assigned_slot, assigned_id), JOIN_NAK(5: reason 1=full 2=password), PING(6)/PONG(7: token = host send-time ms; RTT clamped 5000), ROSTER_INFO(8: per-slot active/latency/name[32] + host_slot). Host sends PING + ROSTER_INFO every 1000 ms **in the lobby only** (`td5_net_tick`, gated `!s_active`).

## 3. Session model

- **Host**: `td5_net_create_session_ex(name, player, max, port, upnp)` (`td5_net_create_session` = LAN wrapper, default port, no UPnP). Host is slot 0, id 1. `SessionInfo.seed` is set but never transmitted (see §4 caveat).
- **Join**: `td5_net_join_session(idx,…)` (LAN) / `td5_net_join_direct(ip,port,…)` → `net_begin_client_join`: `s_local_slot=-1`, send JOIN_REQ; `td5_net_tick` retries every 500 ms, **12 attempts (~6 s)** then `connection_lost`. JOIN_ACK assigns slot/id and pins the host's game address.
- **Accept** (`ws2_accept_join`, host): idempotent for retried REQs; password gate (`s_host_password`, set via `td5_net_set_session_limits(max 2–6, pw)`) then capacity gate → NAK; new joiner gets lowest free slot 1..5, id = slot+1, `s_sync_generation++`, then `broadcast_roster()` + ROSTER_INFO.
- **Sealing** (`td5_net_seal_session`): sealed host NAKs joins, stops beaconing, browsers skip it. The lobby unseals on entry.
- **Lobby** (`Screen_NetworkLobby` [11], `td5_fe_net.c:808`): buttons START / CHANGE CAR / SELECT TRACK (returns via `s_flow_context==4`) / EXIT / OPTIONS (host: max-players + password modal). Roster mirrored each frame from `td5_net_is_slot_active`. Host START with ≥2 players: state 5 → **0x10** (8-tick countdown, `td5_net_send(DXPSTART)`) → **0x11** (wait `td5_net_is_active()`) → launch. Clients auto-launch when `td5_net_is_active()` flips (worker thread completed the 4→6→7 rendezvous). Solo-host START deliberately launches a plain single-player race (`network_active` stays 0).
- **Config push is vestigial**: states 0xC–0xF (seal + kick 0x12 + request-config 0x13 + settings 0x15) port the original flow but the live path bypasses them (state 5 jumps to 0x10) — *no race-config replication happens*; each machine launches from its own frontend selections. `Screen_SessionLocked` [29] shows on `s_kicked_flag`, which **no remote peer ever sets** in the port (`td5_frontend.c:9491`).
- **Launch wiring**: `s_launching_net_race` → `g_td5.network_active` (`td5_frontend.c:2684`); each net player occupies a racer slot 0..N−1 (currently rendered as N split viewports — documented follow-up, `td5_frontend.c:2625`). `network_active` forces drag-race mode, 4 laps, traffic/encounters/wanted OFF (`td5_game.c:1402`), skips traffic asset load (:1924), replaces AI rubber-banding with flat 0x100 throttle/steer bias (`td5_ai.c:1467`), and routes participant checks to `td5_net_is_slot_active` (`td5_game.c:6125`).

## 4. In-race lockstep

Once per **render frame** (not per 30 Hz tick), `td5_game_run_race_frame` calls `td5_game_net_sync_frame` (`td5_game.c:3338`) when `g_td5.network_active && td5_net_is_active()`:

1. Poll local input once; place its control bits at the local net slot.
2. `td5_net_handle_host_frame` (orig `HandlePadHost @0x1000b680`): host stores its bits, waits on `frame_ack` (20 s) until every active remote slot's DXPFRAME arrived, merges `control_bits[6]`, broadcasts DXPFRAME with its `frame_delta_time` + `sync_sequence`, seq++. `td5_net_handle_client_frame` (orig `@0x1000b8b0`): send own frame to host, wait for the broadcast (20 s), adopt merged bits + host dt. A `sync_sequence` mismatch only logs a warning — **no correction of any kind**.
3. The client corrects this frame's `sim_time_accumulator` by (host dt − local dt) so all machines execute the **same number of fixed 30 Hz sim ticks**.
4. Merged bits are written into all 6 slots (`td5_input_set_control_bits`) and the per-substep input poll is skipped (`net_lockstep`, `td5_game.c:3631`) — every substep this frame uses identical bits on every machine.

**Determinism is the whole protocol.** Since no game state is replicated, any divergence is permanent. `td5_msvc_rand.c` (DO NOT remove) overrides mingw's `rand()/srand()` with the MSVC 6.0 CRT LCG (`x*0x343FD+0x269EC3`, `>>16 & 0x7FFF`) — a different `rand()` stream changes AI cardef picks and cascades into physics. Any physics/AI change on a net-active path must be per-tick deterministic: no wall-clock reads, no frame-rate dependence, no order-dependent parallel reductions (this is why the multithreading work deferred parallel physics).

**Verified caveat — the race RNG seed is NOT exchanged.** InitRace step 0 calls `srand(GetTickCount())` *per machine* (`td5_game.c:1496–1502`); `SessionInfo.seed` is never sent. Determinism currently holds only because network mode disables the rand()-consuming sim systems (traffic/encounters/wanted off, AI bias fixed). Introducing any `rand()` into a sim-affecting net path desyncs immediately.

**Resync barrier** (generation-counted 3-phase, types 10→11→12): `s_sync_generation` increments on topology events (join accept, RESYNCREQ received). `run_resync_barrier_host` loops: send REQ to each client, wait `sync` event for all RESYNCs, send RESYNCACK to each, decrement generation; client mirror in `run_resync_barrier_client`; both reset `sync_sequence=0`. The frame handlers run the barrier before and re-check after their waits. Host migration: a client promoted via DXPROSTER switches to the host barrier/frame path mid-call.

## 5. Failure modes

- **Lockstep timeout**: `SYNC_TIMEOUT_MS` = 20 000 ms on every barrier/frame wait → handler returns 0 → `td5_game_net_sync_frame` sets `TD5_INPUT_ESCAPE` on the local slot → race exits via the normal quit/fade path.
- **No reliability layer**: raw UDP, no retransmit of DXPTYPE messages. A lost DXPFRAME stalls the peer until the 20 s timeout. Lost START/CONFIRM is partially covered by the JOIN_REQ retry (join only), not at race start.
- **`s_connection_lost` setters**: DXPDISCONNECT, `FD_CLOSE`, `WSAECONNRESET`, barrier with zero remote players, RESYNCREQ send with no clients, JOIN_REQ retry exhaustion. Read via `td5_net_is_connection_lost()`; the Direct-connect UI additionally has its own 8 s join wait + password re-prompt (`Screen_DirectConnect` states 5/7/8).
- **Latent quirk** (read in `handle_start`): a client replies to DXPSTART with **two** ACK_REPLY datagrams (the `send_to_player(own id, …)` call resolves to the host address via the client fallback in `ws2_get_target_addr`, then an explicit `s_transport_send(0,…)` also targets the host). With ≥2 clients the host's `s_pending_ack_count` can hit 0 before all clients actually acked.
- **Kick**: only the legacy DXPDATA 0x12 path exists; nothing in the port sets `s_kicked_flag` remotely.

## 6. Key entry points

| Entry point | File:line | Role |
|---|---|---|
| `td5_net_init` / `td5_net_shutdown` | td5_net.c:2036 / :2138 | Events + worker + WS2 transport up/down; UPnP unmap on shutdown |
| `td5_net_tick` | td5_net.c:2176 (called td5_game.c:853, every game tick) | Discovery poll, ANNOUNCE beacon, JOIN_REQ retry, lobby PING/ROSTER_INFO |
| `td5_net_create_session_ex` / `td5_net_join_session` / `td5_net_join_direct` | td5_net.c:2388 / :2471 / :2506 | Host / LAN join / direct join |
| `td5_net_handle_host_frame` / `_client_frame` | td5_net.c:2684 / :2794 | Per-frame lockstep barrier |
| `td5_game_net_sync_frame` | td5_game.c:3338 | Game-side lockstep driver (engaged td5_game.c:3489) |
| `worker_thread_proc` / `handle_app_message` | td5_net.c:437 / :502 | Receive thread + DXPTYPE dispatch |
| `ws2_handle_game_control` / `ws2_accept_join` | td5_net.c:1706 / :1571 | Magic control channel; join accept/NAK |
| `td5_net_send` / `td5_net_receive` | td5_net.c:2949 / :3089 | Typed send (per-type routing) / ring pop |
| `Screen_NetworkLobby` / `Screen_DirectConnect` / `Screen_SessionPicker` | td5_fe_net.c:808 / :481 / :650 | Lobby FSM / direct host-join UI / LAN browser |
| `td5_upnp_map_port` / `td5_upnp_verify_port` / `td5_upnp_unmap_port` | td5_upnp.c:529 / :562 / :618 | Router port mapping |
| `rand` / `srand` override | td5_msvc_rand.c:30 | MSVC CRT LCG — lockstep/replay determinism keystone |

Dev harness: `TD5RE_NET_SELFTEST=host|join` (+`_IP`/`_PW`/`_MAX`) runs the Direct-IP join handshake headlessly (`main.c:515`, dev builds); `TD5RE_NET_LOBBY=1|2` boots a host lobby via `--StartScreen=11` (`td5_fe_net.c:817`). Logs under tag `net` → `log/engine.log`.
