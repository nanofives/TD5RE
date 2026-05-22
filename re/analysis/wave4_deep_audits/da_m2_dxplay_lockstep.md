# DA-M2 — DXPlay Lockstep / Pad-Replication Wire Protocol Audit

Wave4 Deep Audit M2. Cuts the wire-protocol gap between M2DX.dll's DXPlay
namespace and the port's `td5_net.c` flattened 13-handler design.

Source:
- M2DX.dll opened read-only in `TD5_pool6` (image base 0x10000000).
- `DXPlay::SendMessageA` @ 0x1000b2a0 (143)
- `DXPlay::ReceiveMessage` @ 0x1000b580 (134)
- `DXPlay::HandlePadHost` @ 0x1000b680 (88)
- `DXPlay::HandlePadClient` @ 0x1000b8b0 (87)
- `HandleDirectPlayAppMessage` @ 0x1000c4d0 (RECEIVE dispatch — counterpart to SendMessageA)
- `HandleDirectPlaySystemMessage` @ 0x1000c9a0 (DPSYS_*)
- Port: `td5mod/src/td5re/td5_net.c` + `td5_net.h` + `TD5_NetMsgType` in `td5_types.h:270`.

Globals referenced (M2DX `.data` block 0x1005b150..0x1005ec70):
- `_DAT_1005b150` = outbound packet header word (msg type, 4B)
- `g_directPlayOutboundMessagePayload` = `_DAT_1005b154` (payload start — type==1/3 stores size here)
- `DAT_1005b158` = secondary payload buffer used by case 1 + 3
- `_DAT_1005b16c` = host frame-dt slot (offset +0x1c)
- `_DAT_1005b170` = syncSequence slot (offset +0x20)
- `&DAT_1005b398` = 16-entry receive ring; stride 0x244; type at +0, size at +4, data at +0x8 (or +0x4 for chat)
- `&DAT_1005d7d8` = scratch payload pointer returned by ReceiveMessage
- `g_directPlayPlayerIdTable` (`_DAT_1005e761?`) and `&DAT_1005e5c0` = 6-slot active-flag table at +(slot*4); player-id table 0x83 dwords later
- `g_localDirectPlayPlayerId`, `g_hostDirectPlayPlayerId`, `g_pDirectPlay`
- `bConnectionLost`, `g_isDirectPlaySyncActive`, `g_isInDirectPlaySession`, `g_isDirectPlayClient`
- `g_isWaitingForSyncBarrier`, `g_isWaitingForFrameAck`
- `g_directPlaySyncGeneration`, `g_directPlaySyncSequence`
- `g_directPlayPendingAckCount`, `g_directPlayExpectedAckCount`
- `g_directPlayMessageQueueWriteIndex` (= `DAT_1005f8e8`?) / read cursor `DAT_1005f8e8`
- `g_directPlayPlayerSyncTable` (received controlBits per slot)
- `&DAT_1005f8a4` = snapshot of active table at frame-start
- `DAT_1005e5d8` = local slot index (skip self in broadcast)
- `DAT_1005e5dc` = local slot index used by host for sync table indexing
- `DAT_1005e5e4` = remote-player count (drives barrier active check)
- `g_hDirectPlayFrameAckEvent`, `g_hDirectPlaySyncEvent`

DirectPlay vtable: `(*pDirectPlay)[+0x68]` = `IDirectPlay4A::SendEx`,
`+0x54` = `GetPlayerData`, `+0x78` = `SetPlayerData`. Signature:
`SendEx(this, fromID, toID, flags, buf, len)`. `toID=0` = broadcast.

----

## Section A — `DXPlay::SendMessageA` case-by-case dispatch

`SendMessageA(DXPTYPE type, void *data, ulong size)` — early-out if
`bConnectionLost`, returns 1 on success / 0 if connection lost.

| Case | Symbolic name (port)     | Target | Wire frame layout                     | Ack tracking | Notes |
|------|---------------------------|--------|----------------------------------------|--------------|-------|
| 0    | DXPFRAME                  | NONE   | header-only no-op (falls through)      | none         | type 0 not sent here — emitted only by `HandlePadHost`/`HandlePadClient`. SendMessageA(0,...) is silent break. |
| 1    | DXPDATA (chat-mode A)     | Broadcast (toID=0) | type@+0 = 1, size@+4 = `size`, payload memcpy'd to `DAT_1005b158` (block at +0x8 of header packet but copied into a SECONDARY buffer at 0x1005b158); SendEx length = `size + 8` | none | Two buffers used — header msg-type at 0x1005b150 stays "1", but the payload bytes are written to a separate dedicated DAT_1005b158 block. SendEx sends 8 + size bytes from `&_DAT_1005b150`. |
| 2    | DXPCHAT                   | Broadcast (toID=0) | type@+0 = 2, payload memcpy'd to `g_directPlayOutboundMessagePayload` (=`+0x4`); SendEx length = `size + 4` | none | Single-buffer; payload is a 0-terminated string (ReceiveMessage uses strlen logic when re-reading). |
| 3    | DXPDATA_TARGETED          | Broadcast (toID=0) | Same layout as case 1 (type=3, secondary buffer at DAT_1005b158, size word at offset +4, payload at +8); SendEx length = `size + 8` | none | Identical wire shape to case 1, differs only by type tag — receiver-side semantics on dequeue. |
| 4    | DXPSTART                  | Per-active-peer iteration over g_directPlayPlayerIdTable[i] | type@+0; size = 4 (header only) | YES — sets expected/pending ack counters. For each peer slot ≠ local: if `(data[slot] != 0 && playerID != 0)` → set msg type 5 (RESYNC/ACK_REQUEST) and increment ack counters; else → msg type 9 (DISCONNECT). Then SendEx 4 bytes. If `expected == 0` → `bConnectionLost = 1`. | "data" param is treated as a `char[]` participation mask (1B/slot, 6 bytes). Selects each peer for DXPACK_REQUEST (5) or DXPDISCONNECT (9). |
| 5    | (synthesized by case 4)   | (n/a from SendMessageA) | type@+0 = 5; size = 4 | YES (driven by case 4) | Never invoked directly by SendMessageA — case 5 input is silently treated as default. |
| 6    | (synthesized by RX of 5)  | (n/a)  | n/a | n/a | Generated only by `HandleDirectPlayAppMessage` case 5 → reply 6 toward host. Not in SendMessageA dispatch. |
| 7    | DXPSTART_CONFIRM          | NONE   | falls through (default break)          | none         | Type 7 is **received-side only** (emitted by HandleDirectPlayAppMessage case 6 → host broadcasts type 7 to all). SendMessageA(7,...) is a no-op. |
| 8    | DXPROSTER                 | Broadcast (toID=0) | type@+0 = 8, payload @+4; SendEx length = `size + 4` | none | Used by HandleDirectPlaySystemMessage cases 3 and 0x101 (host-migration roster broadcast). Payload = 0x34 bytes = playerID[6] (24B) + activeFlag[6] (24B) + hostID (4B). |
| 9    | DXPDISCONNECT             | NONE   | falls through (default break)          | none         | Type 9 is **received-side only** (emitted as failure tag in case-4 per-peer loop OR by HandleDirectPlaySystemMessage). SendMessageA(9,...) is a no-op. |
| 10   | DXPRESYNCREQ              | Per-active-peer iteration | type@+0 = 10; size = 4 | YES — counter setup before loop: `expected=0; pending=0`. For each peer slot ≠ local with non-zero playerID: increment expected + pending; SendEx 4B to that peer. If `expected == 0` → `bConnectionLost = 1`. | Used by HOST to initiate resync barrier (called from HandlePadHost line 1000b6cb). |
| 11   | DXPRESYNC (client→host)   | n/a    | n/a | n/a | Sent ONLY by HandlePadClient's barrier loop (inlined at 0x1000b8e2-ish — uses SendEx directly with `_DAT_1005b150 = 0xb`). Not in SendMessageA dispatch. |
| 12   | DXPRESYNCACK              | n/a    | n/a | n/a | Sent ONLY by HandleDirectPlayAppMessage case 0xb (host's per-peer broadcast after pending=0) — uses SendEx directly with `_DAT_1005b150 = 0xc`. Not in SendMessageA dispatch. |
| default | Msg("SendMessage Unknown Type") | — | — | — | Logs warning. |

**Cases SendMessageA actually emits onto the wire:** 1, 2, 3, 4 (synthesizes 5+9), 8, 10.
**Cases that fall through silently:** 0, 5, 7, 9.

----

## Section B — Per-case wire payload spec

Wire frame is always:
```
struct {
    uint32_t  type;          // +0x00  (_DAT_1005b150)
    union {
        // case 2 / case 8:  payload starts at +0x04
        uint8_t  payload[];

        // case 1 / case 3:  size word, then payload at +0x08
        struct { uint32_t size; uint8_t data[]; } sized;

        // case 0 (DXPFRAME): full 0x80 broadcast
        struct {
            uint32_t  controlBits[6];   // +0x04..+0x1c
            float     frameDt;          // +0x1c (_DAT_1005b16c)
            uint32_t  syncSequence;     // +0x20 (_DAT_1005b170)
            uint8_t   pad[0x80 - 0x24]; // unused, broadcast at fixed 0x80B width
        } frame;
    };
}
```

Per-case sizes (the `dwDataSize` argument the host passes to `IDirectPlay4::SendEx`):

| Type | Outbound size | Source globals | Notes |
|------|---------------|----------------|-------|
| 1    | size + 8      | `&_DAT_1005b150` | header (4B type) + secondary block: 4B size + payload from DAT_1005b158. **Two-stage layout** — anomalous vs all other types. |
| 2    | size + 4      | `&_DAT_1005b150` | type + payload (string). |
| 3    | size + 8      | `&_DAT_1005b150` | same as case 1. |
| 4    | 4 each (per peer) | `&_DAT_1005b150` | header only; type set to 5 or 9 per peer. |
| 8    | size + 4      | `&_DAT_1005b150` | header + roster blob (typically 0x34B). |
| 10   | 4 each (per peer) | `&_DAT_1005b150` | header only. |
| DXPFRAME (Host) | 0x80 (fixed) | `&_DAT_1005b150` | controlBits[6] + frameDt + syncSequence + pad. |
| DXPFRAME (Client) | 0x80 (fixed) | `&_DAT_1005b150` | controlBits[local] at `g_directPlayOutboundMessagePayload` + syncSequence at +0x20; frameDt slot unused (host writes). |
| 11 (client→host) | 4 | inline | header only. |
| 12 (host→peers) | 4 each | inline | header only. |

Receive-side (HandleDirectPlayAppMessage at 0x1000c4d0) interprets payloads identically:

- **type==1 / type==3**: copy `min(size, 0x23C)` from `param_1+1` into ring entry payload (size word at ring +4, data at ring +8). Ring write-index = `(write & 0xf) + 1`.
- **type==2**: same but data at ring entry +5 (one-byte offset! the chat field), and the sender slot index (matched against `param_3` = sender DPID) is back-written to ring entry +4 as a single byte. So chat messages carry a 1-byte sender-slot prefix.
- **type==0 (FRAME)**:
  - If host (`g_isInDirectPlaySession`): scan player-id table to find sender slot. Write `param_1[1]` (sender's controlBits) into `g_directPlayPlayerSyncTable[slot]`. Validate `param_1[8]` (syncSequence) == `g_directPlaySyncSequence`. Decrement `pendingAckCount`; when 0, reset to expected and `SetEvent(FrameAckEvent)`.
  - If client: copy `param_1[1..6]` into `g_directPlayPlayerSyncTable[0..5]` directly, `syncSequence++`, `SetEvent(FrameAckEvent)`.
- **type==5 (ACK_REQUEST)**: client replies type 6 to host.
- **type==6 (ACK_REPLY)**: host decrements pending; when 0, broadcasts type 7 to all peers, then enqueues a type-4 marker into local ring and arms `isDirectPlaySyncActive = 1`.
- **type==7 (START_CONFIRM)**: enqueue type-4 marker into local ring; arm `isDirectPlaySyncActive = 1; syncSequence = 0`.
- **type==8 (ROSTER)**: replace local `g_directPlayPlayerIdTable[0..5]` and `&DAT_1005e5c0[0..5]` active-flag table from packet; update `g_hostDirectPlayPlayerId` from `param_1[0xd]`. Re-run `RefreshCurrentSessionRoster()`.
- **type==9 (DISCONNECT)**: set `bConnectionLost = 1`, log "Connection Lost", `ErrorN = 0x6d`.
- **type==10 (RESYNCREQ)**: client side → `syncGeneration++`. SetEvent SyncEvent / FrameAckEvent if blocked.
- **type==11 (RESYNC)**: host side → decrement pending; when 0, broadcast type 12 to peers + SetEvent SyncEvent locally.
- **type==12 (RESYNCACK)**: client side → SetEvent SyncEvent (unblocks barrier wait).

----

## Section C — `HandlePadHost` / `HandlePadClient` lockstep semantics

### HandlePadHost (host per-frame, returns 1 if connection healthy)

1. If `bConnectionLost` → return 0 immediately.
2. **Barrier outer loop** while `syncGeneration != 0`:
   - If `remotePlayerCount (DAT_1005e5e4) < 2` and generation == 1: clear generation, exit barrier loop.
   - Else: `ResetEvent(SyncEvent)`; `SendMessageA(10, NULL, 0)` (broadcast RESYNCREQ to peers, sets ack counters); set `isWaitingForSyncBarrier=1`; `WaitForSingleObject(SyncEvent, 20s)`. Timeout → return 0. On signal: `syncGeneration--`; clear waiting flag; loop.
3. Snapshot active-mask: copy `&DAT_1005e5c0[0..5]` → `&DAT_1005f8a4[0..5]` (6 dwords). This freezes the participating-set for this frame.
4. If `remotePlayerCount < 2`: replicate local `param_1[0]` to `param_1[DAT_1005f8c0]` and return immediately (single-player fast path).
5. Set `isWaitingForFrameAck=1`, `WaitForSingleObject(FrameAckEvent, 20s)`. Timeout → return 0.
6. Recheck `syncGeneration == 0` (could have been bumped during wait). If non-zero, loop back to step 2.
7. **Merge phase**:
   - `(&g_directPlayOutboundMessagePayload)[localSlot] = param_1[0]` (write own controlBits into outbound table).
   - For i in 0..5:
     - if i == localSlot: `param_1[i] = outbound[i]` (echo own).
     - else if `frame-snapshot[i] == 0`: `param_1[i] = 0; outbound[i] = 0` (player dropped during this frame).
     - else: `param_1[i] = outbound[i] = g_directPlayPlayerSyncTable[i]` (use most recent received controlBits).
8. Build broadcast frame: `_DAT_1005b150 = 0` (type=DXPFRAME), `_DAT_1005b16c = *param_2` (frameDt), `_DAT_1005b170 = syncSequence`.
9. `SendEx(pDirectPlay, localID, 0, 0, &_DAT_1005b150, 0x80)` — broadcast 0x80B DXPFRAME.
10. `syncSequence++`; `ResetEvent(FrameAckEvent)`; return `(bConnectionLost == 0)`.

### HandlePadClient (client per-frame, returns 1 if healthy)

1. Set `_DAT_1005e824 = param_1` (controlBits pointer cache).
2. **Outer barrier loop** while `syncGeneration != 0`:
   - **Host migration check**: if `g_isInDirectPlaySession != 0` (we became host) → call `HandlePadHost` and return.
   - `RefreshCurrentSessionRoster()` (drain return value 2 = retry).
   - Snapshot active-mask: copy `&DAT_1005e5c0[0..5]` → `&DAT_1005f8a4[0..5]`.
   - `g_directPlayOutboundMessagePayload = 4` (size field), `_DAT_1005b150 = 0xb` (DXPRESYNC type).
   - `SendEx(pDirectPlay, localID, hostID, 0, &_DAT_1005b150, 4)` (send RESYNC to host).
   - Set `isWaitingForSyncBarrier=1`, `WaitForSingleObject(SyncEvent, 20s)`. Timeout → return 0.
   - `ResetEvent(SyncEvent)`; `syncGeneration--`. Re-check `bConnectionLost`, re-check host-migration.
3. **Frame send**: `g_directPlayOutboundMessagePayload = param_1[0]` (local controlBits at +0x4 of buffer), `_DAT_1005b150 = 0` (DXPFRAME), `_DAT_1005b170 = syncSequence`.
4. `SendEx(pDirectPlay, localID, hostID, 0, &_DAT_1005b150, 0x80)` — send 0x80B DXPFRAME to host (unicast, not broadcast).
5. Set `isWaitingForFrameAck=1`, `WaitForSingleObject(FrameAckEvent, 20s)`. Timeout → return 0.
6. If `g_isInDirectPlaySession != 0` (became host mid-wait) → call HandlePadHost and return.
7. If `syncGeneration != 0` (resync triggered during wait) → loop back to step 2.
8. **Unpack received frame**: copy `g_directPlayPlayerSyncTable[0..5]` → `param_1[0..5]`; `*param_2 = _DAT_1005ec6c` (received frame-dt); paranoid check `DX::FCount == DAT_1005ec70`.

### Counters / barriers

- `syncGeneration` is the resync-pending counter. Bumped by HandleDirectPlaySystemMessage cases 5 (host-migration-pending) and 0x101 (player added/removed), AND by HandleDirectPlayAppMessage case 10 (DXPRESYNCREQ received). Decremented per completed barrier cycle.
- Host pumps RESYNCREQ→RESYNC→RESYNCACK round-trip; client pumps RESYNC→RESYNCACK round-trip.
- `pendingAckCount` decrements per ACK received (cases 6 / 11). When it hits 0, the host signals downstream (case 6→broadcast type 7; case 11→broadcast type 12).
- 20-second timeouts are hard — if any peer drops out for 20s the session enters connection-lost and returns 0.
- `syncSequence` advances 1 per frame; receivers validate but only warn on mismatch.

----

## Section D — Orig vs Port handler mapping

Port's flattened 13-entry table (td5_net.c:285):

| TD5_DXP* | Orig SendMessageA | Orig HandleDirectPlayAppMessage | Port handler | Coverage status |
|----------|-------------------|-----------------------------------|----------------|-----------------|
| 0 DXPFRAME | (HandlePad{Host,Client} inline) | RX merges controlBits | handle_frame + handle_host_frame + handle_client_frame | **Behaviorally faithful** for lockstep logic; wire shape differs (port uses ENet datagrams, no 0x80-fixed packet). |
| 1 DXPDATA | case 1 (broadcast, 2-buffer layout w/ size word) | case 1 (ring-push) | handle_data | **Wire-DIVERGENT**: port serializes `{type, size, payload}` flat; orig sends `{type, size@DAT_1005b158-block, payload}` from two static buffers. Port-emitted DXPDATA bytes will not deserialize on orig peer (size byte at wrong offset). |
| 2 DXPCHAT | case 2 (broadcast, single-buffer w/ string) | case 2 (ring-push w/ sender-slot prefix byte) | handle_chat | **Wire-DIVERGENT**: orig back-writes sender slot index into the ring entry at +5 to identify chat-author; port doesn't track per-message sender similarly. Lobby chat would route wrong author. |
| 3 DXPDATA_TARGETED | case 3 (same shape as case 1) | case 1 (shares dispatch with type 1!) | handle_data_targeted | Behaviorally maps; same wire-divergence as DXPDATA. Note orig's RX merges 1 and 3 into the same ring code path. |
| 4 DXPSTART | case 4 (per-peer 5/9 fan-out, ack-counter setup) | case 4 marker-only ring push | handle_start | **Partial**: port broadcasts a single TD5_DXPSTART; orig sends per-peer 5/9 with ack tracking. Port's ack counters get set in `td5_net_send` but there's no fan-out 5-or-9 dispatch. |
| 5 DXPACK_REQUEST | (synthesized by case 4) | case 5 → client replies 6 | handle_ack_request | OK — port sends 5 explicitly, orig synthesizes from case 4. Wire-compatible IF port emits 4B-sized type==5 datagram. |
| 6 DXPACK_REPLY | n/a (recv only) | case 6 (decrement pending, broadcast 7 when 0) | handle_ack_reply | OK |
| 7 DXPSTART_CONFIRM | (synthesized by RX of case 6) | case 7 (ring-push marker + arm sync) | handle_start_confirm | OK |
| 8 DXPROSTER | case 8 (0x34B blob: ids[6] + flags[6] + hostId) | case 8 (replace local tables) | handle_roster | **Wire-DIVERGENT**: port's roster blob layout differs (PlayerSlot structs include 64B names; orig sends 6×4B IDs + 6×4B flags + 1×4B hostID = 52B). |
| 9 DXPDISCONNECT | (synthesized by case-4 zero-active path) | case 9 (set connection-lost) | handle_disconnect | OK |
| 10 DXPRESYNCREQ | case 10 (host fan-out, ack counters) | case 10 (client side: syncGen++, unblock waits) | handle_resync_req | OK |
| 11 DXPRESYNC | (HandlePadClient inline) | case 0xb (host: decrement pending, broadcast 12 when 0) | handle_resync | OK |
| 12 DXPRESYNCACK | (case 0xb inline) | case 0xc (client: SetEvent SyncEvent) | handle_resync_ack | OK |

### Other architectural divergences (not in 13-case table)

1. **Lockstep state location** — Orig keeps `syncSequence`, `syncGeneration`, `pendingAck`, `expectedAck`, `g_directPlayPlayerSyncTable`, `&DAT_1005f8a4` snapshot active-mask, and `_DAT_1005ec6c` received-frame-dt as M2DX `.data` globals. Port uses `static` module-locals inside `td5_net.c`. Functionally equivalent, but memory-scan tools targeting M2DX offsets won't find these in the port.
2. **DirectPlay vtable replacement** — Orig calls `(*pDirectPlay)[+0x68] = IDirectPlay4A::SendEx`. Port uses Winsock2 via the `s_transport_send` function pointer. No DirectPlay GUID, no DPID-allocation handshake, no `SetPlayerData(0x21, ...)` host-flag push (system case 0x101).
3. **System-message channel** — Orig receives DPSYS_* messages on the same Win32 message queue as APP_* messages (HandleDirectPlaySystemMessage at 0x1000c9a0). Cases 0x21 (DPSYS_DELETEPLAYERFROMGROUP / del-player) and 0x31 (DPSYS_SESSIONLOST) drive connection-loss. Case 0x101 (DPSYS_HOST = host migration) re-broadcasts the roster as DXPTYPE 8 and pokes player data byte 0x21. Port has no equivalent system-message channel; host migration in the port is best-effort via `td5_net_handle_client_frame`'s "promoted to host" path but the corresponding 0x101 trigger doesn't exist.
4. **Driver-name stride** — Tangential but related: orig uses 0x3c-byte stride for `s_directPlayDriverNameBuffer` (60B), port uses 64B (per Wave3-O1A memory `arch_dxptype_protocol_divergence`).
5. **Ring buffer is structurally identical**: 16-entry × 0x244B/entry = 9.25 KB. Type at +0, size at +4, payload at +8 (or +5 for chat sender prefix). Port preserves this exactly (RING_ENTRY_COUNT=16, RING_ENTRY_SIZE=0x244, RING_PAYLOAD_MAX=0x23C).

----

## Section E — What the port would need for wire-compat with TD5_d3d.exe peers

(Acknowledging: this is almost certainly never implemented. The port already
acknowledges DXPTYPE wire-incompat as a parked ARCH-DIVERGENCE.)

Required for byte-level compat with M2DX.dll peers:

1. **DirectPlay transport**: instantiate `DirectPlay4A` COM object, register `DPLAPPID_GUID`, accept host migrations via `IDirectPlayLobby` and `SetPlayerData(0x21, …)`. Replace `s_transport_send`/`s_transport_recv` with DirectPlay's send/recv. (~600 lines, fragile.)
2. **Type-1 / type-3 two-stage layout**: refactor `td5_net_send` cases 1/3 to emit `{type:u32, size:u32, payload[size]}` using `&_DAT_1005b150` and `DAT_1005b158`-equivalent secondary buffer. Currently flat.
3. **Type-2 chat sender prefix**: when receiving DXPCHAT, look up sender DPID in `s_roster`, back-write slot index into ring entry at offset +5 BEFORE pushing. Currently missing.
4. **Type-4 START fan-out**: change `td5_net_send(TD5_DXPSTART, mask, 6)` to iterate `mask[]`; per active peer emit DXPACK_REQUEST (5), per inactive emit DXPDISCONNECT (9). Currently broadcasts a single TD5_DXPSTART.
5. **Type-8 ROSTER 0x34B blob**: replace `PlayerSlot[]` serialization with `{ids[6]:u32, activeFlags[6]:u32, hostID:u32}` = 13 dwords = 52B (matches orig case 8 unpack).
6. **DXPFRAME 0x80B fixed-width broadcast**: pad outbound frame to 0x80 even though only 0x24 is used. Receivers may validate length.
7. **`syncSequence` field at offset +0x20** of DXPFRAME (not +0x1c) — `+0x1c` is reserved for `frameDt` (host-only write, client zero).
8. **DPID-based broadcast (`toID=0`)**: Winsock UDP has no equivalent — would need DirectPlay or a multicast group. Currently port iterates `s_transport_send(playerID, ...)` per active peer.
9. **DPSYS_* event injection**: any host-migration or session-loss event in DirectPlay must be re-raised into HandlePad* via `syncGeneration++` + `SetEvent(SyncEvent | FrameAckEvent)`. Port has roster-update plumbing but not DPSYS-driven generation bumps.
10. **20-second WaitForSingleObject timeouts**: port already uses `SYNC_TIMEOUT_MS` constant; verify it equals 20000.

Practical assessment: items 2–7 are 1–2 days of careful work. Item 1
(DirectPlay COM resurrection) is weeks and requires a Windows host that
still installs `dplayx.dll` correctly. Items 8–10 are minor cleanups.
Going the other direction (TD5_d3d peers joining a TD5RE host) is a
non-goal — orig peers never see TD5RE's Winsock2 transport.

----

## Cleanup

Pool slot 6 released by `bash scripts/ghidra_pool.sh cleanup`. No
writebacks performed (read_only=true).
