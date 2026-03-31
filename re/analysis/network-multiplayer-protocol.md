# Network Multiplayer Protocol — Deep Dive

## Overview

TD5 network multiplayer uses Microsoft DirectPlay 4A (IDirectPlay4A) through the M2DX.DLL `DXPlay` subsystem. The architecture is **lockstep deterministic** — a host-authoritative frame synchronization model where the host collects per-player input each tick, merges it into a consolidated frame payload, and broadcasts the authoritative state to all clients. The game simulation runs identically on all machines from the same input stream, requiring no position/velocity replication and no interpolation or prediction.

**Key insight:** No game-world state (position, velocity, lap count, physics) is ever sent over the network. Only **controller input bitmasks** (one DWORD per player slot) and a **frame delta-time float** are synchronized. The deterministic simulation produces identical results on all machines.

---

## 1. DXPlay API (M2DX.DLL)

### 1.1 Exports

| # | VA | Signature | Purpose |
|---|---|---|---|
| 1 | `0x1000ab20` | `DXPlay::Environment()` | Zeros runtime state (0x2FA + 0x304 dwords); resets ring buffer indices |
| 2 | `0x1000ab50` | `DXPlay::Create()` | `CoCreateInstance(CLSID_DirectPlay, IID_IDirectPlay4A)`; starts worker thread |
| 3 | `0x1000abd0` | `DXPlay::Destroy()` | Calls `ShutdownDirectPlayWorker` |
| 4 | `0x1000abf0` | `DXPlay::Lobby()` | Stub, returns 0 |
| 5 | `0x1000ac00` | `DXPlay::ConnectionEnumerate()` | Enumerates service providers via `IDP4::EnumConnections` |
| 6 | `0x1000add0` | `DXPlay::ConnectionPick(int)` | `IDP4::InitializeConnection`; starts 3s session enum timer |
| 7 | `0x1000aed0` | `DXPlay::NewSession(name, playerName, gameType, maxPlayers, seed)` | Host: `IDP4::Open` + `CreatePlayer` + roster refresh |
| 8 | `0x1000b0d0` | `DXPlay::JoinSession(index, playerName)` | Client: joins enumerated session + `CreatePlayer` + roster refresh |
| 9 | `0x1000b2a0` | `DXPlay::SendMessageA(DXPTYPE, data*, size)` | Dispatches outbound messages by type |
| 10 | `0x1000b580` | `DXPlay::ReceiveMessage(type*, data**, size*)` | Dequeues from 16-entry ring buffer |
| 11 | `0x1000b680` | `DXPlay::HandlePadHost(controlBits*, frameDt*)` | **Host per-frame sync** |
| 12 | `0x1000b8b0` | `DXPlay::HandlePadClient(controlBits*, frameDt*)` | **Client per-frame sync** |
| 13 | `0x1000bb40` | `DXPlay::EnumerateSessions()` | `IDP4::EnumSessions`; caches GUIDs |
| 14 | `0x1000bc40` | `DXPlay::SealSession(int)` | Opens/closes session to new joins |
| 15 | `0x1000bca0` | `DXPlay::EnumSessionTimer(int)` | `SetTimer`/`KillTimer` for periodic 3s enumeration |
| 16 | `0x1000bd00` | `DXPlay::UnSync()` | Clears `g_isDirectPlaySyncActive` flag |

### 1.2 Internal Workers

| VA | Name | Purpose |
|---|---|---|
| `0x1000bfb0` | `StartDirectPlayWorker` | Creates 4 events + worker thread |
| `0x1000c050` | `ShutdownDirectPlayWorker` | Signals stop; waits; closes handles; releases COM |
| `0x1000c3e0` | `DirectPlayWorkerThreadProc` | `WaitForMultipleObjects` on {Receive, Stop}; dispatches messages |
| `0x1000c4d0` | `HandleDirectPlayAppMessage` | Routes types 0-12 from the wire into DLL state |
| `0x1000c9a0` | `HandleDirectPlaySystemMessage` | Host migration, session loss, player deletion |
| `0x1000bd10` | `RefreshCurrentSessionRoster` | `IDP4::EnumPlayers`; rebuilds ID/name/slot tables |

### 1.3 Win32 Events (4 total)

| Handle Global | Name | Usage |
|---|---|---|
| `g_hDirectPlayReceiveEvent` | Receive | Signaled by DirectPlay when data arrives |
| `g_hDirectPlayWorkerStopEvent` | Stop | Signaled to terminate worker thread |
| `g_hDirectPlayFrameAckEvent` | FrameAck | Host waits: all client inputs received; Client waits: host broadcast received |
| `g_hDirectPlaySyncEvent` | Sync | Barrier synchronization for resync/startup |

---

## 2. DXPTYPE Protocol Message Types

The protocol uses a **DWORD message type prefix** at offset 0 of every DirectPlay message. The `HandleDirectPlayAppMessage` function at `0x1000c4d0` is the authoritative dispatcher.

### 2.1 Complete Message Type Table

| Type | Name | Direction | Wire Size | Purpose |
|---|---|---|---|---|
| 0 | `DXPFRAME` | Host->All (broadcast) | 0x80 (128 bytes) | **Per-frame authoritative state** — merged input + timing |
| 0 | `DXPFRAME` | Client->Host (unicast) | 0x80 (128 bytes) | **Per-frame client input submission** |
| 1 | `DXPDATA` | Any->All (broadcast) | 8 + payload | **General data payload** — frontend commands, lobby opcodes |
| 2 | `DXPCHAT` | Any->All (broadcast) | 4 + strlen | **Chat text message** — lobby chat |
| 3 | `DXPDATA_TARGETED` | Any->All (broadcast) | 8 + payload | Same as type 1 but queued differently in ring buffer |
| 4 | `DXPSTART` | Host->Each (targeted) | 4 | **Race start trigger** — host signals all clients to begin |
| 5 | `DXPACK_REQUEST` | Host->Client | 4 | Host requests client acknowledgement |
| 6 | `DXPACK_REPLY` | Client->Host | 4 | Client sends acknowledgement to host |
| 7 | `DXPSTART_CONFIRM` | Host->All | 4 | Host confirms all clients are ready; race begins |
| 8 | `DXPROSTER` | Host->All | 0x34 (52 bytes) | **Roster snapshot** — player IDs, active flags, host ID |
| 9 | `DXPDISCONNECT` | Any | 4 | Explicit disconnect/connection-loss notification |
| 10 | `DXPRESYNCREQ` | Host->Each | 4 | **Resync request** — host initiates barrier resync |
| 11 | `DXPRESYNC` | Client->Host | 4 | Client responds to resync request |
| 12 | `DXPRESYNCACK` | Host->All | 4 | Host confirms resync complete, all resume |

### 2.2 Message Payload Layouts

#### Type 0 — DXPFRAME (0x80 bytes on wire)

This is the **critical per-tick message**. Both host-to-all broadcasts and client-to-host submissions use this format.

```
Offset  Size  Field
0x00    4     DXPTYPE = 0 (message type tag)
0x04    4     controlBits[0]    (DWORD — player slot 0 input bitmask)
0x08    4     controlBits[1]    (DWORD — player slot 1 input bitmask)
0x0C    4     controlBits[2]    (DWORD — player slot 2 input bitmask)
0x10    4     controlBits[3]    (DWORD — player slot 3 input bitmask)
0x14    4     controlBits[4]    (DWORD — player slot 4 input bitmask)
0x18    4     controlBits[5]    (DWORD — player slot 5 input bitmask)
0x1C    4     frameDeltaTime    (float — host's g_normalizedFrameDt)
0x20    4     syncSequence      (DWORD — monotonic frame counter for drift detection)
0x24-0x7F     (padding / reserved — 92 bytes, within the 0x80 send size)
```

**controlBits bitmask** (same as `gPlayerControlBits` from DXInput):

| Bit | Mask | Meaning |
|---|---|---|
| 0-15 | 0x0000FFFF | Analog stick / steering value |
| 16 | 0x00010000 | Unused |
| 17 | 0x00020000 | Accelerate |
| 18 | 0x00040000 | Brake |
| 19 | 0x00080000 | Handbrake |
| 20 | 0x00100000 | Horn / Siren |
| 21 | 0x00200000 | Gear Up |
| 22 | 0x00400000 | Gear Down |
| 23 | 0x00800000 | (reserved) |
| 24 | 0x01000000 | Change Camera |
| 25 | 0x02000000 | Rear View |
| 28 | 0x10000000 | Auto-transmission flag |
| 30 | 0x40000000 | Escape / Quit |

#### Type 1 — DXPDATA (variable)

```
Offset  Size  Field
0x00    4     DXPTYPE = 1
0x04    4     payload_size
0x08    N     payload_data[]
```

Used for frontend lobby sub-opcodes (see Section 5 below).

#### Type 2 — DXPCHAT (variable)

```
Offset  Size  Field
0x00    4     DXPTYPE = 2
0x04    N     null-terminated chat string (prefixed with sender slot index byte)
```

#### Type 4 — DXPSTART (4 bytes payload)

```
Offset  Size  Field
0x00    4     DXPTYPE = 4
```

Sent targeted from host to each connected player. On receive, clients respond with type 6 (DXPACK_REPLY). After all acks collected, host broadcasts type 7 (DXPSTART_CONFIRM).

#### Type 8 — DXPROSTER (0x34 bytes payload)

```
Offset  Size  Field
0x00    4     DXPTYPE = 8
0x04    24    playerIdTable[6]  (DPID — 6 player slot IDs, 4 bytes each)
0x1C    24    playerActiveFlags[6]  (int — 1=active, 0=empty per slot)
0x34    4     hostPlayerId      (DPID — current session host)
```

Broadcast by host on join, leave, and host migration events.

---

## 3. Per-Frame Sync Protocol (Race-Time)

### 3.1 Architecture

The frame sync uses a **lockstep barrier** model with 20-second timeouts:

```
                    HOST                                    CLIENT
                      |                                        |
                      |  <---- DXPFRAME (client's input) ---  |
                      |  (client sends its controlBits,       |
                      |   then blocks on FrameAck event)      |
                      |                                        |
  [collect all       ]|                                        |
  [client inputs     ]|                                        |
  [merge into frame  ]|                                        |
                      |                                        |
                      |  ---- DXPFRAME (merged state) ----->   |
                      |  (broadcast to ALL via DPID=0)         |
                      |                                        |
  [advance sim tick  ]|                            [advance sim tick]
                      |                                        |
```

### 3.2 Host Flow (`HandlePadHost` at `0x1000b680`)

1. Check for pending resync (`g_directPlaySyncGeneration != 0`) — if so, run barrier protocol
2. Copy player active flags to snapshot table
3. Wait on `g_hDirectPlayFrameAckEvent` (20s timeout) for all client inputs
4. When all clients' `g_directPlayPlayerSyncTable[]` entries received:
   - Merge: for each slot, if active and not host, copy received controlBits; if host, use local
   - Write merged controlBits[0..5] into outbound message buffer
   - Write host's `frameDeltaTime` (from `g_normalizedFrameDt`)
   - Write `g_directPlaySyncSequence` (monotonic counter)
   - Broadcast DXPFRAME (type 0, 0x80 bytes) to all players (DPID=0 = broadcast)
   - Increment `g_directPlaySyncSequence`
   - Reset `g_hDirectPlayFrameAckEvent`
5. Return to caller (PollRaceSessionInput) with merged controlBits

### 3.3 Client Flow (`HandlePadClient` at `0x1000b8b0`)

1. Check for pending resync — if resync in progress AND client has become host, call `HandlePadHost` instead (host migration)
2. Package local controlBits[local_slot] into DXPFRAME message
3. Send to host (targeted via `g_hostDirectPlayPlayerId`)
4. Wait on `g_hDirectPlayFrameAckEvent` (20s timeout) for host's broadcast
5. When received: copy `g_directPlayPlayerSyncTable[0..5]` into local `controlBits[0..5]`
6. Copy host's `frameDeltaTime` into local timing
7. Validate `g_directPlaySyncSequence` matches (Report() on mismatch)
8. Return to caller with authoritative frame data

### 3.4 Timeout Handling

Both host and client use a 20-second (`WaitForSingleObject(..., 20000)`) timeout:
- If timeout fires, the function returns 0 (failure)
- The caller (`PollRaceSessionInput`) sets the local player's ESC bit and reports auto-escape
- This effectively disconnects the player

### 3.5 Sequence Number Drift Detection

`g_directPlaySyncSequence` is incremented by the host after each frame broadcast. Clients check that the received sequence matches their expected value. On mismatch, `Report()` is called (debug log). This detects frame slip but does **not** trigger resync automatically — the resync protocol uses a separate generation counter.

---

## 4. Resync / Barrier Protocol (Types 9-12)

The resync protocol handles re-synchronization after topology changes (player join/leave, host migration). It is a **generation-counted barrier** protocol: each topology event increments a generation counter, and each generation requires a full barrier-drain cycle before frame sync resumes.

### 4.1 State Variables

| Global | VA | Purpose |
|---|---|---|
| `g_directPlaySyncGeneration` | DLL-internal | Resync generation counter — nonzero means resync needed; each topology event increments by 1; each completed barrier decrements by 1 |
| `g_isDirectPlaySyncActive` | DLL-internal | Master enable for frame synchronization (set to 1 when race starts; cleared by `DXPlay::UnSync()`) |
| `g_isWaitingForSyncBarrier` | DLL-internal | Flag: 1 while host is blocking on `g_hDirectPlaySyncEvent` waiting for client barrier acks |
| `g_isWaitingForFrameAck` | DLL-internal | Flag: 1 while waiting on `g_hDirectPlayFrameAckEvent` for frame input/broadcast |
| `g_directPlayPendingAckCount` | DLL-internal | Countdown of expected ack replies from remote players |
| `g_directPlayExpectedAckCount` | DLL-internal | Total number of active remote players (set during `SendMessageA` type 10) |

### 4.2 Message Formats

#### Type 9 — DXPDISCONNECT (4 bytes on wire)

```
Offset  Size  Field
0x00    4     DXPTYPE = 9
```

**Trigger:** Sent by `DXPlay::SendMessageA(4, ...)` (the DXPSTART dispatch) to each player slot whose active flag is 0 in the provided slot mask. This is an explicit "you are disconnected" notification sent during the pre-race start sequence to kick non-participating players.

**Handler (`HandleDirectPlayAppMessage` case 9):**
1. Calls `Report()` (debug log)
2. Sets `DXPlay::bConnectionLost = 1`
3. Displays "Connection Lost" message via `Msg()`
4. Sets `ErrorN = 0x6D` (fatal network error code)

**Key insight:** Type 9 is NOT a resync message despite its position in the enum. It is a hard disconnect notification. The "resync" protocol uses types 10-12 only.

#### Type 10 — DXPRESYNCREQ (4 bytes on wire)

```
Offset  Size  Field
0x00    4     DXPTYPE = 10
```

**Direction:** Host -> each active client (targeted unicast, NOT broadcast)

**Trigger:** Sent by `HandlePadHost` when `g_directPlaySyncGeneration >= 2`, or by `HandlePadClient` when the client detects it has become host during a resync cycle. The host iterates over all active player slots (excluding itself) and sends type 10 to each one individually.

**Sender flow (in `SendMessageA`, case 10):**
1. Zero `g_directPlayPendingAckCount` and `g_directPlayExpectedAckCount`
2. For each slot != local slot where active flag != 0:
   - Increment both `g_directPlayExpectedAckCount` and `g_directPlayPendingAckCount`
   - Send `{DXPTYPE=10}` (4 bytes) to that player's DPID via `IDP4::Send`
3. If `g_directPlayExpectedAckCount == 0` after the loop (no remote players): set `bConnectionLost = 1`

**Handler (`HandleDirectPlayAppMessage` case 10):**
1. Calls `Report()` (debug trace: "Received DXPRESYNCREQ")
2. Increments `g_directPlaySyncGeneration` (signals this client also needs to drain a generation)
3. If `g_isWaitingForSyncBarrier != 0`: signals `g_hDirectPlaySyncEvent` (unblocks any pending wait)
4. If `g_isWaitingForFrameAck != 0`: signals `g_hDirectPlayFrameAckEvent` (unblocks frame wait)

**What this achieves:** The RESYNCREQ wakes up any client that might be blocked waiting for a frame or barrier event, allowing it to break out of its current wait and enter the resync drain loop.

#### Type 11 — DXPRESYNC (4 bytes on wire)

```
Offset  Size  Field
0x00    4     DXPTYPE = 11
```

**Direction:** Client -> Host (targeted unicast to `g_hostDirectPlayPlayerId`)

**Trigger:** Sent by `HandlePadClient` during the resync barrier drain. When a client detects `g_directPlaySyncGeneration != 0` and it is NOT the host, it:
1. Refreshes the session roster via `RefreshCurrentSessionRoster()`
2. Copies current player active flags
3. Sends `{DXPTYPE=11}` (4 bytes) to the host
4. Sets `g_isWaitingForSyncBarrier = 1`
5. Blocks on `g_hDirectPlaySyncEvent` with 20s timeout
6. On success: clears `g_isWaitingForSyncBarrier`, resets `g_hDirectPlaySyncEvent`, decrements `g_directPlaySyncGeneration`

**Handler (`HandleDirectPlayAppMessage` case 11, host-side only):**
1. Only processes if `g_isInDirectPlaySession != 0` (host mode)
2. Decrements `g_directPlayPendingAckCount`
3. When `g_directPlayPendingAckCount == 0` (all clients responded):
   - Sends DXPRESYNCACK (type 12) to each active remote player
   - Signals `g_hDirectPlaySyncEvent` (unblocks the host's own barrier wait)
   - Resets `g_directPlayPendingAckCount = g_directPlayExpectedAckCount`

#### Type 12 — DXPRESYNCACK (4 bytes on wire)

```
Offset  Size  Field
0x00    4     DXPTYPE = 12
```

**Direction:** Host -> each active client (targeted unicast)

**Trigger:** Sent by the host's `HandleDirectPlayAppMessage` case 11 handler after all clients have replied with type 11.

**Handler (`HandleDirectPlayAppMessage` case 12, client-side):**
1. Calls `Report()` (debug trace: "Received DXPRESYNCACK")
2. Signals `g_hDirectPlaySyncEvent` — this unblocks the client's `HandlePadClient` which was waiting in the barrier

### 4.3 Complete Resync Sequence Diagram

```
    Event: Player leaves/joins or host migration detected
    ========================================================

    HOST (HandlePadHost)                           CLIENT (HandlePadClient)
         |                                              |
    [detects g_directPlaySyncGeneration >= 2]           |
         |                                              |
    1. ResetEvent(SyncEvent)                            |
    2. SendMessageA(10) -> DXPRESYNCREQ to each    --> [receives type 10]
    3. g_isWaitingForSyncBarrier = 1                    |
    4. WaitForSingleObject(SyncEvent, 20s)         3a. g_directPlaySyncGeneration++
         |                                         3b. Signal SyncEvent if waiting
         |                                         3c. Signal FrameAckEvent if waiting
         |                                              |
         |                                    [HandlePadClient resync loop]
         |                                         4. RefreshCurrentSessionRoster()
         |                                         5. Send DXPRESYNC (type 11) to host
         |                                         6. g_isWaitingForSyncBarrier = 1
         |                                         7. WaitForSingleObject(SyncEvent, 20s)
         |                                              |
    [worker thread receives type 11]                    |
    5. g_directPlayPendingAckCount--                    |
    6. if count == 0:                                   |
       - Send DXPRESYNCACK (type 12) to each  -->  [receives type 12]
       - Signal SyncEvent                          8. Signal SyncEvent
         |                                              |
    7. g_isWaitingForSyncBarrier = 0               9. g_isWaitingForSyncBarrier = 0
    8. g_directPlaySyncGeneration--               10. g_directPlaySyncGeneration--
         |                                              |
    [if g_directPlaySyncGeneration still > 0,           |
     repeat from step 1]                                |
         |                                              |
    [resume normal frame sync]                    [resume normal frame sync]
```

### 4.4 Resync Trigger Conditions

| Trigger | Source | Effect |
|---|---|---|
| `DPSYS_HOST` (0x101) — host migration | `HandleDirectPlaySystemMessage` | `g_directPlaySyncGeneration++`; also sets `g_isInDirectPlaySession = 1` on new host |
| `DPSYS_DESTROYPLAYERORGROUP` (0x005) — player left | `HandleDirectPlaySystemMessage` | `g_directPlaySyncGeneration++` (only if `g_isDirectPlaySyncActive != 0`) |
| Multiple rapid events | Cumulative | Each event increments generation; the barrier loop drains them one-by-one |

### 4.5 What Data Is Resynchronized?

**No game state is resynchronized.** The resync protocol is purely a barrier/fence mechanism:

1. It ensures all machines reach the same logical synchronization point before resuming lockstep frame exchange
2. It allows the roster to be refreshed (`RefreshCurrentSessionRoster()`) so all machines agree on who is active
3. It allows the host role to stabilize after migration
4. After the barrier completes, `g_directPlaySyncSequence` is reset to 0, and the lockstep frame counter restarts

The simulation state (positions, velocities, lap counts) is NEVER transmitted. Because the game is fully deterministic from identical inputs, all machines remain in sync as long as frame ordering is maintained. The resync barrier simply ensures frame ordering is re-established after a topology disruption.

### 4.6 Resync Timeout Behavior

Both host and client use a 20-second timeout on their barrier waits:
- **Host timeout** (waiting for all type 11 replies): `Report()` called, function returns 0
- **Client timeout** (waiting for type 12 confirmation): `Report()` called, function returns 0
- The caller (`PollRaceSessionInput`) interprets return 0 as failure, sets the local player's ESC bit, effectively disconnecting

### 4.7 Host Migration During Resync

If `HandlePadClient` detects that `g_isInDirectPlaySession != 0` (meaning this machine has been promoted to host by DirectPlay), it immediately calls `HandlePadHost` instead of continuing the client resync path. This transparent switch ensures:
1. The new host takes over barrier coordination
2. The new host sends DXPRESYNCREQ to remaining clients
3. Frame sync resumes under the new host's authority

### 4.8 Relationship Between Types 4-7 and Types 9-12

The startup and resync protocols are structurally similar but serve different purposes:

| Protocol | Types | When | Purpose |
|---|---|---|---|
| **Race start** | 4 (DXPSTART) -> 5 (ACK_REQ) -> 6 (ACK_REPLY) -> 7 (START_CONFIRM) | Lobby -> Race transition | Synchronize all machines to begin frame sync simultaneously |
| **Resync** | 10 (RESYNCREQ) -> 11 (RESYNC) -> 12 (RESYNCACK) | During active race | Re-establish barrier after topology change |
| **Disconnect** | 9 (DISCONNECT) | Pre-race start | Kick non-participating slots |

The start protocol (4-7) additionally initializes `g_isDirectPlaySyncActive = 1` and resets `g_directPlaySyncSequence = 0`, which the resync protocol does not need to do (sync is already active).

---

## 5. Frontend Lobby Sub-Protocol

Within DXPTYPE=1 (DXPDATA) messages, the first byte is a **frontend opcode**. These are processed by `ProcessFrontendNetworkMessages` at `0x0041b610` in the EXE. All opcodes are sent wrapped in a DXPlay type-1 envelope.

### 5.1 Frontend Opcodes — Complete Reference

#### Opcode 0x10 — LOBBY_STATUS (4 bytes)

```
Offset  Size  Field
0x00    1     opcode = 0x10
0x01    1     slotIdx     (which player slot, 0-5)
0x02    1     statusValue (see status table below)
0x03    1     (padding)
```

**Sent by:** Every machine, every 800ms (`ProcessFrontendNetworkMessages` periodic broadcast).
**Handler:** `DAT_00496980[slotIdx] = statusValue` — updates the per-slot status display.
**When:** Sent by non-host players. The host uses opcode 0x11 instead (which includes additional fields).

#### Opcode 0x11 — LOBBY_STATUS_EXT (8 bytes)

```
Offset  Size  Field
0x00    1     opcode = 0x11
0x01    1     slotIdx
0x02    1     statusValue
0x03    1     DAT_004a2c90 (track direction / schedule index, low byte)
0x04    1     DAT_004a2c98 (reverse track flag, low byte)
0x05    1     g_selectedGameType (low byte)
0x06    2     (padding)
```

**Sent by:** Host only, every 800ms.
**Handler:** Same as 0x10 for status, plus updates `DAT_004a2c90`, `DAT_004a2c98`, and `g_selectedGameType` on all clients. This is how the host's track/game-type selection is propagated to the lobby display in real-time.

#### Opcode 0x12 — LOBBY_KICK (8 bytes)

```
Offset  Size  Field
0x00    1     opcode = 0x12
0x01    1     targetSlotIdx (which slot to kick)
0x02-0x07     (padding)
```

**Sent by:** Host during pre-race sequence (state 0xC) to each slot that is not marked ready. Also sent via `*kIlL` chat command.
**Handler:** If `targetSlotIdx == localSlotIdx` (the target is this machine): sets `DAT_00497328 = 1` which triggers lobby exit -> disconnect screen (screen 0x1D).

#### Opcode 0x13 — LOBBY_REQUEST_CONFIG (4 bytes)

```
Offset  Size  Field
0x00    1     opcode = 0x13
0x01    1     targetSlotIdx (which client to query)
0x02-0x03     (padding)
```

**Sent by:** Host during pre-race sequence (state 0xD) — polls each participating client for their car/driver config.
**Handler:** If `targetSlotIdx == localSlotIdx`:
1. Build reply as opcode 0x14:
   ```
   Offset  Size  Field
   0x00    1     opcode = 0x14
   0x01    1     localSlotIdx
   0x02    1     DAT_0048f364 (car selection index)
   0x03    1     DAT_0048f368 (driver model index)
   0x04    1     flags byte: (DAT_0049629c << 1 | DAT_004aaf7c) << 4 | DAT_0048f378
                 Bit layout: [7:5]=difficulty*2|autoTransmit, [4]=0, [3:0]=DAT_0048f378 (driver color)
   0x05    1     DAT_0048f370 (driver variant)
   0x06-0x07     (padding)
   ```
2. Send via `DXPlay::SendMessageA(1, reply, 8)`

#### Opcode 0x14 — LOBBY_CAR_SELECTION (5 bytes effective)

```
Offset  Size  Field
0x00    1     opcode = 0x14
0x01    1     slotIdx
0x02    1     carId        (index into car table)
0x03    1     driverId     (driver model index)
0x04    1     flagsByte    (auto-trans, difficulty, driver color — packed)
```

**Sent by:** Client in response to opcode 0x13.
**Handler (receiving side):**
1. `DAT_00497268[slotIdx] = carId`
2. `DAT_00497280[slotIdx] = driverId`
3. `DAT_00497298[slotIdx] = flags[offset 4]` (driver variant)
4. `DAT_004972b0[slotIdx] = flags[offset 4]` (packed flags — auto-trans | difficulty)
5. Falls through to set `DAT_00497262[slotIdx] = 1` (config received flag)

#### Opcode 0x15 — LOBBY_SETTINGS (0x80 bytes)

```
Offset  Size  Field
0x00    1     opcode = 0x15
0x01    120   settingsBlock (0x78 bytes, copied from DAT_00497250)
```

**Sent by:** Host during pre-race sequence (state 0xF) to each participating client.
**Handler:** Copies the 0x78 byte settings block into `DAT_00497250` on the client. Then sends opcode 0x16 as acknowledgment.

**Settings block layout (DAT_00497250, 0x78 bytes = 30 DWORDs):**

```
Offset  Size  Field
0x00    4     g_selectedGameType
0x04    4     DAT_004a2c90 (track schedule index)
0x08    4     DAT_004a2c98 (reverse track direction flag)
0x0C    6x4   DAT_00497268[0..5] — per-slot car selection
0x24    6x4   DAT_00497280[0..5] — per-slot driver model
0x3C    6x4   DAT_00497298[0..5] — per-slot driver variant
0x54    6x4   DAT_004972b0[0..5] — per-slot control flags (auto-trans | difficulty)
0x6C    4     DAT_0049697C (host slot index copy)
0x70    4     DAT_00497248 (host DPID copy)
0x74    4     (padding)
```

#### Opcode 0x16 — LOBBY_READY_ACK (4 bytes)

```
Offset  Size  Field
0x00    1     opcode = 0x16
0x01    1     slotIdx (acknowledging client's slot)
0x02-0x03     (padding)
```

**Sent by:** Client automatically after receiving opcode 0x15 settings block.
**Handler:** Sets `DAT_00497262[slotIdx] = 1` (this client's settings are acknowledged). When all participating slots have acked, the host proceeds to state 0x10 (launch).

#### Opcode 0x17 — LOBBY_REQUEST_NAME (8 bytes)

```
Offset  Size  Field
0x00    1     opcode = 0x17
0x01    1     targetSlotIdx
0x02-0x07     (padding)
```

**Sent by:** Via `*fRoM` chat command — requests a specific player's profile information.
**Handler:** If `targetSlotIdx == localSlotIdx`:
1. Build reply as opcode 0x18 (0x20 bytes):
   ```
   Offset  Size  Field
   0x00    1     opcode = 0x18
   0x01    1     slotIdx + '1' (ASCII slot number: '1'-'6')
   0x02    1     ':' separator
   0x03    4     DAT_00497AA0 (profile data word 0)
   0x07    4     DAT_00497AA4 (profile data word 1)
   0x0B    4     DAT_00497AA8 (profile data word 2)
   0x0F    1     ',' separator
   0x10    16    Player name string (from DAT_00497AC0, null-terminated)
   ```
2. Send via `DXPlay::SendMessageA(1, reply, 0x20)`

#### Opcode 0x18 — LOBBY_NAME_REPLY (0x20 bytes)

```
Offset  Size  Field
0x00    1     opcode = 0x18
0x01    1     slotIdx + '1' (ASCII)
0x02    1     ':'
0x03    12    profile data
0x0F    1     ','
0x10    16    player name (null-terminated)
```

**Sent by:** Client in response to opcode 0x17.
**Handler:** If `DAT_00497320 != 0` (pending name request counter): decrements counter, rewrites opcode byte to 0x7F, and re-queues as a LOBBY_TEXT message (makes the profile info appear as chat text). This creates a visible "SlotN: profile, PlayerName" string in the chat panel.

#### Opcode 0x7F — LOBBY_TEXT (variable length)

```
Offset  Size  Field
0x00    1     opcode = 0x7F
0x01    N     null-terminated string (system message text)
```

**Sent by:** Various system events — session creation, player join/leave, host migration notifications.
**Handler:** The opcode 0x7F messages bypass the opcode switch entirely (`< 6` check fails for 0x7F) and fall through to the default path which treats the entire payload as a displayable text entry in the chat scrollback buffer.

Specific system messages using 0x7F:
- `SNK_SeshCreateMsg`: "Session created" (sent by host on `CreateFrontendNetworkSession`)
- `SNK_SeshJoinMsg`: "has joined" (sent by client on `JoinSession`)
- `SNK_SeshLeaveMsg`: "has left the session" (appended to player name on disconnect detection)
- `SNK_NowHostMsg`: "is now the host" (sent when host migration detected)
- `SNK_WaitForHostMsg`: "waiting for host" (sent by non-host clicking Start)

### 5.2 Lobby Status Values (DAT_00496980 per-slot table)

| Value | Meaning | Display String (via `SNK_NetPlayStatMsg`) |
|---|---|---|
| 0 | Not ready / idle | (first entry in 10-char string table) |
| 1 | In lobby, waiting | (second entry) |
| 2 | Ready to start | (third entry) |
| 3 | Confirmed / starting | (fourth entry) |
| 0x7F | Inactive / empty slot | (high-index entry, used as sentinel) |

The status panel (`RenderFrontendLobbyStatusPanel`) renders each active slot's player name (from `dpu_exref + 0xA64 + slot*0x3C`) alongside the status string. The host slot is rendered using `DAT_00496274` (highlight font), other slots use `DAT_00496270` (normal font). If the local player is the host, an "(host)" label is appended.

### 5.3 Car Selection Exchange Sequence

The complete car/driver selection exchange during pre-race (lobby states 0xC through 0x10):

```
    HOST (lobby state 0xC)                        CLIENT
         |                                            |
    1. SealSession(1) -- no more joins                |
    2. For each non-ready slot:                       |
       Send LOBBY_KICK (0x12)                    --> [if me: disconnect]
         |                                            |
    3. Store host's own car/driver into tables         |
    4. Transition to state 0xD                        |
         |                                            |
    STATE 0xD -- Config Collection                    |
         |                                            |
    5. For each participating slot (every 250ms):     |
       Send LOBBY_REQUEST_CONFIG (0x13)          --> [receives 0x13]
         |                                         6. Build & send LOBBY_CAR_SELECTION (0x14)
    7. Receive 0x14, store in car tables         <--  |
       Set DAT_00497262[slot] = 1                     |
    8. When all slots have DAT_00497262 == 1:         |
       Transition to state 0xE                        |
         |                                            |
    STATE 0xE -- Schedule Init                        |
         |                                            |
    9. InitializeRaceSeriesSchedule()                 |
    10. Mark host as ready                            |
    11. Transition to state 0xF                       |
         |                                            |
    STATE 0xF -- Settings Broadcast                   |
         |                                            |
    12. For each participating slot (every 165ms):    |
        Send LOBBY_SETTINGS (0x15, 0x80 bytes)   --> [receives 0x15]
         |                                        13. Copy 0x78 bytes into local settings
         |                                        14. Send LOBBY_READY_ACK (0x16)
    15. Receive 0x16, set ack flag               <--  |
    16. When all acked: transition to state 0x10      |
         |                                            |
    STATE 0x10 -- Launch                              |
         |                                            |
    17. Wait 8 ticks (animation)                      |
    18. DXPlay::SendMessageA(4, slotMask) = DXPSTART  |
         |                                            |
    [See Types 4-7 race start protocol]               |
```

### 5.4 Car Selection Tables

| Address | Size | Purpose |
|---|---|---|
| `DAT_00497268[slot]` | 6x int | Car ID per slot (index into car ZIP table) |
| `DAT_00497280[slot]` | 6x int | Driver model per slot |
| `DAT_00497298[slot]` | 6x int | Driver variant / color per slot |
| `DAT_004972b0[slot]` | 6x int | Packed flags: `(difficulty*2 | autoTransmit) << 4 | driverColor` |
| `DAT_00497262[slot]` | 6x byte | Config-received flag (set by opcodes 0x14 and 0x16) |
| `DAT_0049725C[slot]` | 6x byte | Participating-in-race flag (set during state 0xC from active + ready) |

### 5.5 What Happens When a Player Changes Their Car

During the active lobby (state 3), a player can press "Change Car" (button index 3):
1. The lobby sets `DAT_0049722c = 1` (status = "selecting car")
2. Transitions to Car Selection screen (screen 0x14 = 20)
3. The 800ms periodic status broadcast (opcode 0x10 or 0x11) automatically publishes the new status value to all other players
4. When the player returns from car selection, their status returns to 0 (idle)
5. The actual car ID is NOT sent during the lobby — it is only collected during the pre-race config collection (opcode 0x13/0x14 exchange in state 0xD)

**Key insight:** Car selection is a local-only operation during the lobby phase. Other players only see the status change ("selecting car" -> "ready"). The car data is exchanged only once, during the host-initiated pre-race sequence.

### 5.6 Chat Command System

The chat input (`NormalizeFrontendChatTokens` at `0x41C030`) supports special `*` prefixed commands and emoticon substitution:

**Chat commands:**

| Command | Opcode | Effect |
|---|---|---|
| `*kIlL<N>` | 0x12 | Kick player in slot N (host only). N is ASCII digit, converted to slot index |
| `*cTrL<N>` | opcode byte = N-'1' | Send control command to slot N (repurposes the text as a raw opcode < 6) |
| `*fRoM<N>` | 0x17 | Request player name/profile from slot N. Increments `DAT_00497320` (pending reply counter) |
| `*dEtA<N>` | 0x7F prefix | Send text message prefixed with slot N's name (detail/action emote) |

**Emoticon substitution:**

| Input | Output Byte | Rendered As |
|---|---|---|
| `:)` or `:>` | 0x1B | Happy face |
| `:(` or `:<` | 0x1C | Sad face |
| `;)` or `;>` | 0x1D | Wink |
| `:o` / `:O` / `:0` | 0x1E | Surprised face |
| `%o` / `%O` / `%\` / `%0` | 0x1F | Confused/drunk face |
| `:-X` variants | Same as above but skip the `-` | (nose variant) |
| `%^X` variants | Same as above but skip the `^` | (hat variant) |

All emoticon sequences are collapsed: the multi-character sequence is replaced with a single byte, and the remaining string is shifted left.

---

## 6. Session Management Flow

### 6.1 DirectPlay Session Properties

**Session descriptor structure** (DPSESSIONDESC2, 0x50 bytes):

| Offset | Field | Value |
|---|---|---|
| +0x00 | dwSize | 0x50 |
| +0x04 | dwFlags | 0x44 (open) or 0x45 (sealed) |
| +0x0C-1C | guidApplication | From `DAT_10061AF0` — application GUID (zeroed in .data, set at runtime) |
| +0x20 | dwMaxPlayers | From `DAT_00465F7C[gameType]` per-game-type table |
| +0x28 | lpszSessionName | Session name string pointer |
| +0x38 | dwUser1 | `gameType` — the g_selectedGameType value |
| +0x3C | dwUser2 | `seed` — `timeGetTime()` value at session creation (deterministic RNG seed) |
| +0x40 | dwUser3 | Copied as 10 (hardcoded) |

**Max players per game type** (table at `0x00465F7C`, 8 bytes):

| Game Type | Max Players | Description |
|---|---|---|
| 0 (default) | 6 | Standard race (6 = full grid) |
| 1-6 (championship/cup modes) | 2 | Head-to-head only |
| 7 (Time Trial) | 6 | Full grid |

**Session creation flow** (`DXPlay::NewSession` at `0x1000AED0`):

1. Kill any active session enum timer
2. Fill DPSESSIONDESC2 with name, GUID, maxPlayers, gameType, seed
3. `IDP4::Open(desc)` with DPOPEN_CREATE flag
4. Set `g_isInDirectPlaySession = 1` (this machine is host)
5. Set `g_isDirectPlayClient = 1` (in a session)
6. Create manual-reset event for frame ack
7. `IDP4::CreatePlayer(name, receiveEvent, DPPLAYER_LOCAL)` — creates local player with `DPPLAYER_LOCAL` flag (0x100)
8. Clear `bConnectionLost`
9. `RefreshCurrentSessionRoster()` to populate slot tables

**Session join flow** (`DXPlay::JoinSession` at `0x1000B0D0`):

1. Kill any active session enum timer
2. Fill DPSESSIONDESC2 with the GUID from the selected enumerated session (stored at `DAT_1005DF54 + index * 0x10`)
3. `IDP4::Open(desc)` with DPOPEN_JOIN flag (implicit flag = 1 in second arg)
4. `IDP4::CreatePlayer(name, receiveEvent, 0)` — no DPPLAYER_LOCAL flag for joined clients
5. Clear `bConnectionLost`
6. `RefreshCurrentSessionRoster()`

### 6.2 Session Discovery

**Periodic enumeration** (`DXPlay::EnumSessionTimer`):
- `ConnectionPick` starts a Windows timer (3-second interval, timer ID = 1)
- On each `WM_TIMER` message, `DXPlay::EnumerateSessions()` is called
- `EnumerateSessions` calls `IDP4::EnumSessions()` with `DPENUMSESSIONS_AVAILABLE`
- Results are cached: session GUIDs at `DAT_1005DF54` (stride 0x10), count at `DAT_1005E418`, max ~50 sessions
- Session names are copied into the DPU block via `CopySessionNameCallback` at stride 0x3C per entry

**Session name callback** (`CopySessionNameCallback` at `0x1000C320`):
- For the host (enumerating players): copies player names into `0x1005EEFB + slot * 0x84`; host names are prefixed with `!`
- For clients (roster refresh): copies into `0x1005E62C + index * 0x3C`; records DPIDs in `DAT_1005E7E4[index]`; identifies host by `DPPLAYER_SERVERPLAYER` flag (0x100)

### 6.3 Session Sealing (`DXPlay::SealSession` at `0x1000BC40`)

Toggles the session between open and closed:
- `SealSession(0)` — sets flags to 0x44 (DPSESSION_JOINDISABLED clear = open)
- `SealSession(1)` — sets flags to 0x45 (DPSESSION_JOINDISABLED set = closed)
- Calls `IDP4::SetSessionDesc()` to apply the change

The lobby calls `SealSession(0)` when entering the lobby (allow joins) and `SealSession(1)` at state 0xC when the host initiates the pre-race sequence (no more joins allowed).

### 6.4 Connection Browser (`RunFrontendConnectionBrowser` at `0x418D50`)

State machine (Entry 8 in frontend screen table):

1. **State 0:** Get computer name via `GetComputerNameA` (default "Clint Eastwood" if unavailable); capitalize first letter, lowercase rest; call `DXPlay::ConnectionEnumerate()` to discover service providers (TCP/IP, IPX, modem, serial)
2. **State 1-2:** Render provider list with scroll (max 4 visible, arrow indicators for scrolling); user selects provider
3. **State 3-4:** Call `DXPlay::ConnectionPick(index)` which initializes the chosen service provider and starts 3-second periodic session enumeration
4. **State 5:** Display available sessions from `dpu_exref + 0x00` (session name rows, stride 0x3C); display count from `dpu_exref + 0x1E0`; cursor-based selection with scroll
5. **State 6:** User picks session -> go to Session Picker (screen 9), or enters name for Create Session (screen 10), or Back (screen 5)
6. **States 8-0xB:** Exit animations and cleanup; on Back: cross-fade transition back to main menu

### 6.5 Session Picker (`RunFrontendSessionPicker` at `0x419CF0`)

1. **State 0:** Calls `DXPlay::ConnectionPick()` to activate session enumeration for the selected provider
2. **States 1-2:** Render session list (max 4 visible) from `dpu_exref + 0xBFC` (session count); entrance animation
3. **State 3:** Active selection: OK joins the session, Back returns to connection browser
4. **State 4:** Cursor-driven session highlighting with scroll support
5. On OK: saves selected index to `dpu_exref + 0xC00`, stops enum timer, transitions to Create Session Flow (screen 10, state 0x14)

### 6.6 Create Session (`RunFrontendCreateSessionFlow` at `0x41A7B0`)

**Host path (new session):**

1. **State 0:** Enter session name (max 16 chars at `DAT_00497068`); display text input box
2. **State 2:** If name empty, use computer name as default
3. **State 4:** Enter player name (max 16 chars at `DAT_00496FF8`); display text input box
4. **State 8:** Call `CreateFrontendNetworkSession()`:
   - `DXPlay::NewSession(sessionName, playerName, gameType, maxPlayers, timeGetTime())`
   - Initialize all 6 status slots to 0x7F (inactive)
   - Queue LOBBY_TEXT system message "Session created"
   - Transition to Network Lobby (screen 0xB = 11)
5. Clears game mode flags: `gWantedModeEnabled`, `gTimeTrialModeEnabled`, `g_selectedGameType` = 0

**Client path (joining, state 0x14):**

1. `DXPlay::JoinSession(selectedIndex, playerName)`
2. If join fails: show error screen (screen 0x1D)
3. If join succeeds: queue LOBBY_TEXT "player joined" message, transition to lobby (screen 0xB)

### 6.7 Roster Management (`RefreshCurrentSessionRoster` at `0x1000BD10`)

This function is the central roster synchronization mechanism, called:
- After `NewSession` and `JoinSession`
- On `DPSYS_CREATEPLAYERORGROUP` (player joins)
- On `DPSYS_DESTROYPLAYERORGROUP` (player leaves)
- On `DPSYS_HOST` (host migration)
- During `HandlePadClient` resync barrier

**Algorithm:**
1. Re-entrancy guard: if already refreshing, return 2 (caller loops until done)
2. `IDP4::GetSessionDesc()` — fetch current session descriptor
3. Copy session descriptor locally; update `g_directPlayPlayerCount`
4. `IDP4::EnumPlayers()` with `CopySessionNameCallback` to rebuild player name/ID tables
5. Match new player IDs against existing slot table:
   - Existing players: keep their slot position, copy updated name
   - New players: assign to first empty slot
6. Update `g_localSlotIndex` and `g_hostSlotIndex` by scanning for matching DPIDs
7. If not host: copy player data into DPU export block for EXE access

### 6.8 Network Lobby (`RunFrontendNetworkLobby` at `0x41C330`)

17-state machine (states 0-0x11 minus 0xB, plus 0xC-0x11 as sequential pre-race states). Key states:

| State | Purpose | Details |
|---|---|---|
| 0 | **Initialize** | Load `MainMenu.tga`, create 6 buttons (title, messages, status, Change Car, Start, Exit); `SealSession(0)` if host (open for joins); handle pending disconnect |
| 1 | **Entrance animation** | Animate button positions sliding in (20 ticks) |
| 2 | **Lobby active (idle)** | Set up chat input; deactivate cursor overlay; transition to state 3 |
| 3 | **Main interaction loop** | Process network messages; render chat, status, input panels; handle button clicks |
| 4 | **Chat submit** | `NormalizeFrontendChatTokens()` processes emoticons/commands; if valid text, queue + send via `DXPlay::SendMessageA(2, chatText, len)`; return to state 2 |
| 5 | **Start validation (host)** | Count active players with status==2 (ready); require count == total active AND count > 1; if met -> state 0xC; else show error message |
| 6-7 | **Error message animation** | Display error popup (0=not enough ready, 1=only one player, 2=connection lost); Yes/No buttons |
| 8 | **Error interaction** | Process messages during error display; Yes=retry, No=leave |
| 9 | **Error cleanup** | Release popup surfaces; dispatch based on return: 0=retry start, 1=back to lobby, 2=full exit |
| 10 | **Error exit animation** | Slide-out animation; release surfaces; return to state 2 |
| 0xC | **Pre-race: seal & kick** | `SealSession(1)`; build participating-slot table from active+ready; send LOBBY_KICK (0x12) to non-participating slots; store host's car selection; init host's flags |
| 0xD | **Pre-race: config collection** | Every 250ms: find next unconfirmed slot, send LOBBY_REQUEST_CONFIG (0x13); when all `DAT_00497262[slot] == 1` -> state 0xE |
| 0xE | **Pre-race: schedule init** | `InitializeRaceSeriesSchedule()`; mark host ready flag; clear all config-ack flags; transition to state 0xF |
| 0xF | **Pre-race: settings broadcast** | Every 165ms: send LOBBY_SETTINGS (0x15, 0x80 bytes) to next unconfirmed slot; when all acked via 0x16 -> state 0x10 |
| 0x10 | **Pre-race: launch** | Wait 8 animation ticks; `DXPlay::SendMessageA(4, slotMask, 0)` = DXPSTART trigger; transition to state 0x11 |
| 0x11 | **Pre-race: wait for start ack** | Drain `DXPlay::ReceiveMessage()` until type 4 (DXPSTART) received; then `InitializeFrontendDisplayModeState()` -> race begins |

**Button layout in state 3:**
- Button 3 = "Change Car" -> `SetFrontendScreen(0x14)` (Car Selection), status = 1
- Button 4 = "Start" -> If host: status = 2, go to state 5; if client: send `SNK_WaitForHostMsg` ("waiting for host")
- Button 5 = "Exit" -> Show exit confirmation (state 6)

**Disconnect handling:** Every state checks `DAT_00497328` — if set (by receiving LOBBY_KICK 0x12 or connection loss), the lobby immediately calls `DXPlay::Destroy()`, releases surfaces, and navigates to the error screen (screen 0x1D).

---

## 7. Race Integration (`PollRaceSessionInput` at `0x42C470`)

### 7.1 Input Polling and Network Bridge

`PollRaceSessionInput` is called from `RunRaceFrame` at the top of each simulation tick:

```
if (dpu_exref[0xC08] != 0) {       // Network session active?
    PollRaceSessionInput();          // Network path (once per frame)
} else {
    // Local-only path (called per sim substep)
    PollRaceSessionInput();
}
```

**Network path inside PollRaceSessionInput:**

1. Poll local DXInput devices -> write to `gPlayerControlBits[localSlot]`
2. Check `dpu_exref + 0xC0C` (is host?):
   - **Host:** Set `DAT_004aadac = g_normalizedFrameDt`, call `DXPlay::HandlePadHost(&gPlayerControlBits, &DAT_004aadac)`
   - **Client:** Call `DXPlay::HandlePadClient(&gPlayerControlBits, &DAT_004aadac)`
3. On failure (timeout/disconnect): set ESC bit on local player, report error
4. Apply force-feedback on network-determined local slot
5. Check slot activity: if `dpu_exref + 0xBCC + slot*4 == 0` (player disconnected), set slot state to 0x03 (disconnected)

### 7.2 Local vs. Network Participant Test

`IsLocalRaceParticipantSlot(slot)` at `0x42CBE0`:
- **Network:** returns `dpu_exref[0xBCC + slot*4]` (the active-flags from the DPU block)
- **Split-screen:** returns `slot < 2`
- **Single player:** returns `slot == 0`

### 7.3 Simulation Tick Loop (from `RunRaceFrame`)

After `PollRaceSessionInput` returns with the authoritative `gPlayerControlBits[0..5]`:

```
while (g_simTimeAccumulator > 0xFFFF) {
    for each active slot:
        UpdatePlayerVehicleControlState(slot)   // Decode controlBits -> steering, throttle, brake
    UpdateRaceActors()                           // Physics, AI, collisions
    ResolveVehicleContacts()
    UpdateRaceOrder()
    // ... camera, particles, etc.
}
```

The critical point: **all machines execute the same UpdatePlayerVehicleControlState with identical input bits**, so physics divergence is impossible (assuming deterministic floating-point, which MSVC x86 provides via x87 FPU).

---

## 8. DPU Shared Data Block

The DPU (`tagDX_DPU`) is a 0xC10-byte struct exported from M2DX.DLL at `DXPlay::dpu` and imported by the EXE via `dpu_exref` (IAT at `0x0045D4E4`).

### 8.1 Key DPU Offsets (from EXE perspective: `dpu_exref + offset`)

| Offset | Size | Type | Purpose |
|---|---|---|---|
| +0x000 | 0x3C * N | char[0x3C][N] | Connection provider name strings (stride 0x3C) |
| +0x1E0 | 4 | int | Enumerated connection provider / session count |
| +0x1E4 | 4 | int | Selected connection provider index |
| +0xA28 | 0x3C | char[60] | Current session name string |
| +0xA64 | 0x3C * 6 | char[60][6] | Player name strings (6 slots, stride 0x3C) |
| +0xBCC | 4 * 6 | int[6] | Per-slot active/local flags (1 = local participant, 0 = remote/empty) |
| +0xBE4 | 4 | int | Host slot index (which of the 6 slots is the host) |
| +0xBE8 | 4 | int | Local slot index (which of the 6 slots is "me") |
| +0xBEC | 4 | int | (reserved) |
| +0xBF0 | 4 | int | Session active flag (0 = ended/disconnected) |
| +0xC00 | 4 | int | Selected session index for join |
| +0xC04 | 4 | int | Session list refresh flag |
| +0xC08 | 4 | int | **Network mode active** (nonzero = multiplayer race) |
| +0xC0C | 4 | int | **Is host** (nonzero = this machine is the session host) |

### 8.2 DLL-Internal Globals

| VA | Name | Purpose |
|---|---|---|
| `0x1005DC58` | `g_pDirectPlay` | IDirectPlay4A* COM pointer |
| `0x1005B398` | `g_messageRingBuffer` | 16 entries x 0x244 bytes inbound message queue |
| `0x1005F8E8` | `g_messageQueueReadIndex` | Consumer index (ReceiveMessage) |
| `0x1005ECD0` | `g_messageQueueWriteIndex` | Producer index (worker thread) |
| `0x1005B150` | `g_outboundMessageBuffer` | 0x80-byte staging area for outbound frames |
| `0x1005B154` | `g_directPlayOutboundMessagePayload` | Outbound payload size/data field |
| `0x1005B158` | `g_outboundPayloadData` | Start of payload data within outbound buffer |
| `0x1005E7D0` | `g_directPlayPlayerIdTable` | DPID[6] — DirectPlay player IDs per slot |
| `0x1005E5C0` | `g_playerActiveFlags` | int[6] — 1=active, 0=empty |
| `0x1005EC54` | `g_directPlayPlayerSyncTable` | DWORD[6] — per-slot received controlBits (from last frame) |
| `0x1005EC6C` | `g_receivedFrameDt` | float — host's frameDeltaTime from last frame |
| `0x1005EC70` | `g_receivedFrameCount` | DWORD — host's FCount from last frame |
| `0x1005E5D8` | `g_localSlotIndex` | Local player's slot in the 6-entry table |
| `0x1005E5DC` | `g_hostSlotIndex` | Host's slot index |
| `0x1005E5E4` | `g_directPlayPlayerCount` | Current number of players in session |
| `0x1005B148` | `bConnectionLost` | Connection lost flag |
| `0x1005F8CC` | `g_sessionGameType` | Game type from session descriptor |
| `0x1005F8D0` | `g_sessionSeed` | Random seed from session creation |

---

## 9. Host Migration

### 9.1 DirectPlay System Messages

| SysMsg ID | Name | Handling |
|---|---|---|
| 0x003 | `DPSYS_CREATEPLAYERORGROUP` | Host broadcasts DXPROSTER (type 8) with updated player tables |
| 0x005 | `DPSYS_DESTROYPLAYERORGROUP` | If sync active: increment `g_directPlaySyncGeneration`; signal pending waits |
| 0x021 | `DPSYS_DELETEPLAYER` | Set `bConnectionLost`; signal FrameAck event |
| 0x031 | `DPSYS_SESSIONLOST` | Set `bConnectionLost`; error 0x6D |
| 0x101 | `DPSYS_HOST` | **Host migration:** local player becomes host |

### 9.2 DPSYS_HOST Migration Sequence

1. DirectPlay promotes a surviving player to host, sends `DPSYS_HOST` (0x101)
2. `HandleDirectPlaySystemMessage`:
   - Increment `g_directPlaySyncGeneration`
   - Set `g_isInDirectPlaySession = 1`
   - Set `g_hostDirectPlayPlayerId = g_localDirectPlayPlayerId` (I am now host)
   - Broadcast DXPROSTER (type 8) with updated host ID
   - Update player data via `IDP4::SetPlayerData`
   - Signal both FrameAck and Sync events (unblock any waiting)
3. On next `HandlePadClient` call: detects `g_isInDirectPlaySession != 0`, transparently switches to calling `HandlePadHost` instead
4. Race continues seamlessly with new host

### 9.3 Client Disconnect During Race

When `g_playerActiveFlags[slot]` transitions from 1 to 0:
- `PollRaceSessionInput` sets `gRaceSlotStateTable[slot].state = 0x03` (disconnected)
- Disconnected actors get their velocity/acceleration zeroed (coast to stop)
- Race continues for remaining players

---

## 10. EXE-Side Network Functions

| VA | Name | Purpose |
|---|---|---|
| `0x00418C60` | `QueueFrontendNetworkMessage` | 8-entry ring buffer for outbound lobby messages (stride 0xC4) |
| `0x0041B390` | `CreateFrontendNetworkSession` | Calls `DXPlay::NewSession`; inits slot table; sends system chat |
| `0x0041B610` | `ProcessFrontendNetworkMessages` | Polls `DXPlay::ReceiveMessage` + local queue; dispatches opcodes 0x10-0x18, 0x7F |
| `0x0041C330` | `RunFrontendNetworkLobby` | 17-state lobby screen machine (states 0-0x11, minus 0xB) |
| `0x00418D50` | `RunFrontendConnectionBrowser` | Connection provider + session browser (12 states) |
| `0x00419CF0` | `RunFrontendSessionPicker` | Session list selection UI (8 states) |
| `0x0041A7B0` | `RunFrontendCreateSessionFlow` | Session name/player name input + creation (22 states) |
| `0x0041A530` | `RenderFrontendCreateSessionNameInput` | Text input box for session/player name |
| `0x0041A670` | `RenderFrontendLobbyChatInput` | Chat input box rendering |
| `0x0041BD00` | `RenderFrontendLobbyChatPanel` | Chat history scrollback panel (6 visible lines, surface-scrolling) |
| `0x0041C030` | `NormalizeFrontendChatTokens` | Chat command parser (`*kIlL`, `*cTrL`, `*fRoM`, `*dEtA`) + emoticon substitution |
| `0x0041B420` | `RenderFrontendLobbyStatusPanel` | Player name/status grid; host slot highlighted; "(host)" label appended |
| `0x00419B30` | `RenderFrontendSessionBrowser` | Session list rendering for session picker |
| `0x0042C470` | `PollRaceSessionInput` | Race-time input bridge (local input -> network sync -> controlBits) |
| `0x0042CBE0` | `IsLocalRaceParticipantSlot` | Per-slot local/remote determination |

---

## 11. DAT_004962a0 — Two-Player Mode State

This global controls the 1P vs 2P vs network path divergence throughout the game:

| Value | Meaning | Set By |
|---|---|---|
| 0 | Single player | `ScreenMainMenuAnd1PRaceFlow` default |
| 1 | 2P split-screen: player 1 selecting car | Main menu "Two Player" button |
| 2 | 2P split-screen: player 2 selecting car | `CarSelectionScreenStateMachine` after P1 |
| (3-4) | (transitional split-screen states) | Car selection flow |
| 5-6 | Network multiplayer (not directly set here) | Network path uses `dpu_exref+0xC08` instead |

**Important:** For network play, `DAT_004962a0` is NOT set to 5 or 6. Instead, network mode is determined by checking `dpu_exref + 0xC08 != 0`. The game distinguishes local 2P from network via:
- `dpu_exref + 0xC08 == 0` AND `DAT_004962a0 != 0` -> local split-screen
- `dpu_exref + 0xC08 != 0` -> network multiplayer

---

## 12. Key Architectural Properties

### 12.1 No State Replication
- **Only input bitmasks are synchronized** — no positions, velocities, lap counts, or game-world state crosses the wire
- The simulation is fully deterministic from identical inputs
- This means network bandwidth is constant: 128 bytes per frame regardless of game complexity

### 12.2 No Latency Compensation
- **No input prediction** — clients block until host broadcasts the authoritative frame
- **No interpolation** — all machines run at exactly the same sim tick rate
- **No extrapolation** — if the host is slow, everyone waits
- Frame rate is dictated by the host's `g_normalizedFrameDt` value

### 12.3 No Dead Reckoning
- Position divergence is impossible because the same physics code runs on all machines with the same inputs
- This is a 1998-era lockstep model, common in racing games of that period

### 12.4 Maximum Players
- 6 slots total (matching the 6-racer limit from single-player)
- `g_directPlayPlayerIdTable` and all per-slot arrays are hardcoded to 6 entries
- `NewSession` passes `maxPlayers` from `DAT_00465F7C[gameType]` (per-game-type player limit)

### 12.5 Session Security
- Sessions use a GUID from `DAT_10061AF0` for identification
- `SealSession` toggles the DirectPlay session descriptor's open/close bit (0x44 vs 0x45)
- No authentication, no encryption — typical of 1998 LAN-era games

### 12.6 CheckOut_exref Encoding

The lobby writes a packed diagnostic value into `CheckOut_exref` every frame:
```
*CheckOut_exref = (localSlot << 16 | hostSlot) ^ 0x80000000
```
This XOR-obfuscated word encodes both the local and host slot indices for runtime debugging/cheat detection. It is written at the top of `RunFrontendNetworkLobby` state dispatch.

### 12.7 Session Browser Row Format

`RenderFrontendSessionBrowser` renders rows from `dpu_exref + 0x164` (stride 0x84 per session):

| Offset from Row Base | Size | Content |
|---|---|---|
| +0x00 (= dpu+0x164) | 0x3C | Session name string |
| +0x3C (= dpu+0x1A0) | 0x3C | Secondary label (host name / description) |
| +0x78 (= dpu+0x1DC) | 4 | Current player count |
| +0x7C (= dpu+0x1E0) | 4 | Max player count |
| +0x80 (= dpu+0x1E4) | 4 | Game type index (indexes `SNK_GameTypeTxt`) |

The first row (index 0) is always "New Session" (from `SNK_NewSessionTxt`). The session count is at `dpu_exref + 0xBFC`.

### 12.8 Default Player Name

`RunFrontendConnectionBrowser` uses `GetComputerNameA` for the default player name (max 59 chars, stored at `DAT_004970AC`). If the call fails, the fallback is the hardcoded string `"Clint Eastwood"` at `0x00465F84`. The first character is uppercased and the remainder lowercased via `_toupper` / `_strlwr`.

### 12.9 Worker Thread Architecture

The DirectPlay worker thread (`DirectPlayWorkerThreadProc` at `0x1000C3E0`) uses `WaitForMultipleObjects` on two handles:
- Index 0: `g_hDirectPlayReceiveEvent` — signaled by DirectPlay when inbound data arrives
- Index 1: `g_hDirectPlayWorkerStopEvent` — signaled to terminate the thread

On index 0 (receive): the thread loops calling `IDP4::Receive(DPRECEIVE_ALL)` with a 0x240-byte stack buffer until `DPERR_NOMESSAGES`. Each message is routed:
- `fromId == 0` (system) -> `HandleDirectPlaySystemMessage`
- `fromId != 0` (app) -> `HandleDirectPlayAppMessage(data, size, fromId)`

On index 1 (stop): `ExitThread(0)` immediately.

The 0x240 receive buffer limits the maximum single message to 576 bytes. If a message exceeds this, the thread logs `"BUFFER_TOO_SMALL"` and drops it.

### 12.10 Inbound Message Ring Buffer

The inbound application message queue uses a 16-entry ring buffer:
- Base: `0x1005B398` (DLL-internal)
- Entry size: 0x244 bytes (580 bytes)
- Layout per entry: `[DXPTYPE:4][payload:0x240]`
- Write index: `g_directPlayMessageQueueWriteIndex` (producer, incremented as `(idx & 0xF) + 1`)
- Read index: `DAT_1005F8E8` (consumer, incremented as `(idx & 0xF) + 1`)
- Wraparound: modular via `& 0xF` then `+1`, so indices cycle 1-16 (not 0-15)

Types 1 and 3 are queued with `entry.type = 1` and payload copied from offset +4 (skipping type field).
Type 2 (chat) is queued with `entry.type = 2` and the sender slot index prepended at `entry + 0x05`.
- The random seed (`timeGetTime()` at session creation) is stored in the session descriptor and shared to all players for deterministic NPC placement
