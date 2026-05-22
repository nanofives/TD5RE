---
batch: 24
area: network_multiplayer
tier: T5
target_todos: []
ghidra_session: a286e033d39d42c1a80aefce586b8e63
analyzed_addresses: 0x0042c470, 0x0041b610, 0x0041c330, 0x00418d50, 0x0041a7b0, 0x0041b390, 0x00418c60, 0x0041b420, 0x0041bd00
agent: Claude Opus 4.7 (1M context)
date: 2026-05-20
---

# Globals enumeration -- Network multiplayer (DXPlay session/lockstep/lobby)

## Summary

- Functions analyzed: 9
  - `PollRaceSessionInput @ 0x0042c470` -- in-race host/client pad exchange (calls `HandlePadHost`/`HandlePadClient`)
  - `ProcessFrontendNetworkMessages @ 0x0041b610` -- DXPlay opcode pump for the lobby (drains `ReceiveMessage` and a local 8-slot replay ring)
  - `RunFrontendNetworkLobby @ 0x0041c330` -- 18-state lobby/main screen FSM (host & client) with chat/status panels
  - `RunFrontendConnectionBrowser @ 0x00418d50` -- DPLAY service-provider picker (calls `ConnectionEnumerate`)
  - `RunFrontendCreateSessionFlow @ 0x0041a7b0` -- session-name + player-name input flow leading into `NewSession`/`JoinSession`
  - `CreateFrontendNetworkSession @ 0x0041b390` -- one-shot wrapper that calls `NewSession`
  - `QueueFrontendNetworkMessage @ 0x00418c60` -- helper that pushes a frontend chat/data record into the local lobby ring buffer
  - `RenderFrontendLobbyStatusPanel @ 0x0041b420` -- redraws the host/player roster panel
  - `RenderFrontendLobbyChatPanel @ 0x0041bd00` -- redraws the 6-row chat scrollback
- Unnamed `DAT_*` globals encountered: **44** (after de-dup)
- Already-named globals encountered (noted): 12 (`g_smoothedFrameDt`, `g_normalizedFrameDt`, `g_playerControlBits`, `g_inputPlaybackActive`, `g_replayModeFlag`, `g_cameraTransitionActive`, `g_audioOptionsOverlayActive`, `g_networkRaceActive`, `g_networkSessionActive`, `g_cheatRemoteBraking`, `g_networkControlBufferReset`, `g_randomSeedForRace`, `g_selectedScheduleIndex`, `g_selectedTrackDirection`, `g_selectedGameType`, `g_selectedCarIndex`, `gFrontendSlotCarIdTable`, `gFrontendSlotPositionTable`, `gFrontendSlotExtCarIdTable`)
- Proposals -- high confidence: **27**
- Proposals -- medium confidence: **12**
- Proposals -- comment-only (low confidence): **3**

## Methodology

Seeds were the **M2DX.DLL::DXPlay** imports listed by `external_imports_list`:

| import | callee site |
|---|---|
| `DXPlay::HandlePadHost` / `DXPlay::HandlePadClient` | only call site is `PollRaceSessionInput` |
| `DXPlay::ReceiveMessage` | called by `ProcessFrontendNetworkMessages` and the case-0x11 drain in `RunFrontendNetworkLobby` |
| `DXPlay::ConnectionEnumerate` | only call site is `RunFrontendConnectionBrowser` |
| `DXPlay::NewSession` | only call site is `CreateFrontendNetworkSession` |
| `DXPlay::JoinSession` | only call site is `RunFrontendCreateSessionFlow` case 0x14 |
| `DXPlay::SendMessageA` | 14 sites, all inside the lobby/network flows above |
| `DXPlay::Destroy` / `DXPlay::SealSession` / `DXPlay::UnSync` | called from the lobby and from the in-race teardown |
| `DXPlay::dpu` (data symbol at `0x0045d4e4` -> `0x00060230` in M2DX) | imported via `dpu_exref` indirection; **77 readers** — this is the master `tagDX_DPU` session struct |

Then per-function: decompiled, walked every `DAT_*` token, verified default symbol via `symbol_by_name`, classified writer/reader by `reference_to`. Relevance gate: a `DAT_*` is in-scope iff its primary writer or reader is inside the 9 analyzed functions AND its semantic role is network-side (session state / lockstep timing / lobby presence / chat scrollback / outbound packet build). Globals that overlap with the broader frontend overlay system (session-banner sizing `DAT_004962c4..cc`, generic display-mode button `DAT_00497314`) are listed in "Out-of-scope finds".

## Proposals

### DXPlay session import indirection (M2DX `dpu` struct pointer)

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x0045d4e4 | void* | `g_dpuSessionStructPtr` | high | Already labeled `PTR_dpu_0045d4e4` (auto). Holds the import-table slot resolving to `M2DX.DLL::DXPlay::dpu` (a global `tagDX_DPU` struct living in M2DX's data segment at `00060230`). All 77 readers dereference it via `dpu_exref` to reach session fields at `+0xa28` (session name), `+0xa64` (per-slot 0x3c-byte name rows), `+0xbcc` (per-slot active/local flags, 6 dwords), `+0xbe4` (host slot index), `+0xbe8` (local slot index), `+0xbf0` (orphan-flag), `+0xbf8` (session seed echo), `+0xc00` (selected-connection index), `+0xc08` (in-session bit), `+0xc0c` (host-of-session bit), `+0x1e0` (enum-connection count), `+0x1e4` (enum-connection selection). **Rename the IAT slot symbol; do NOT try to define field-level offsets here -- that's the M2DX struct definition.** | port collapses this into module-static state in td5_net.c (`s_roster[]`, `s_local_slot`, `s_is_host`, `s_session`); not byte-mirrored |

### In-race host/client pad exchange state (called every input tick from `PollRaceSessionInput`)

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x00483000 | u32 | `g_player2ControlBits` | high | Aliased read at `g_playerControlBits + 4` in PollRaceSessionInput when the second slot processes the auto-pause bit. Direct write at 0x0042c5b7: `DAT_00483000 = DAT_00483000 \| 0x10000000;`. The two-player local-mux uses `(&g_playerControlBits)[iVar1]` (0/+4); 0x00483000 IS the +4 alias used when the second-iter index is written-back through a non-aliased path. | port `s_outbound_frame.control_bits[1]` at td5_net.c (canonical, indexed not aliased) |
| 0x0048f37c | u8 | `g_player2AutoPauseLatch` | high | Read at PollRaceSessionInput 0x0042c596: `else if ((iVar1 == 1) && (DAT_0048f37c != 0))`. Companion to `_DAT_0048f378` (player1). When non-zero, player2's auto-pause bit (0x10000000) is set into `g_player2ControlBits`. Twin of `g_player1AutoPauseLatch` at 0x0048f378 (already labeled, see Out-of-scope). | (none -- pause-latch behaviour TBD in port) |
| 0x004aaf98 | u32[2] | `g_cameraCycleDebounceTable` | high | Twin per-player cooldown counter. Written `(&DAT_004aaf98)[iVar1] = 10;` after CycleRaceCameraPreset (PollRaceSessionInput 0x0042c5e2). Decremented per tick at 0x0042c5b1 if non-zero. Used by both the `0x1000000` (cycle camera) and the function-key (VK_F4/F5) per-player camera-cycle paths. **Stride 4 = 2 dwords.** | port has no equivalent; cycle has no debounce. |
| 0x00466f88 | u32[2] | `g_cameraLookBackHoldTable` | high | Twin per-player "look-back" latch. At PollRaceSessionInput 0x0042c606: when bit `0x2000000` is set + race-running + camera-not-transitioning, `(&DAT_00466f88)[iVar1] = 1;` else 0. Read by camera presenter (offsets near 0x00466f88 are referenced in td5_camera). Stride 4 = 2 dwords. | port may have equivalent via input bit 25 (`TD5_INPUT_LOOK_BACK`); needs check |
| 0x004aaf93 | u8 | `g_autoExitKeyHeldState` | high | At PollRaceSessionInput 0x0042c708-0x0042c724: `DXInput::CheckKey(0x3d)` (= VK_F2). If pressed, `DAT_004aaf93 = 1`. If released and prior was 1, clear and set `g_inputPollDeferFlag = 1`. Edge-detect for the per-frame "F2 toggles input-poll mode" behaviour. **Note**: read only inside this function -- if this flag belongs to a wider F2 system the writer/reader will be in another batch's area. | (none -- F2-as-poll-defer not mirrored) |
| 0x0049722c | u32 | `g_lobbyPlayerStatus` | high | Per-local-slot lobby state machine: 0=idle, 1=ready/configured, 2=ready-to-start, 3=racing. Set to 0 on entry to RunFrontendNetworkLobby state 0; set to 3 in DXPSTART handler at 0x0041bbab (`if (DAT_00496978 == 4) { DAT_0049722c = 3; }`); set to 1/2/3 by lobby button-press branches at 0x0041cc3a, 0x0041cd14, 0x0041ce63. Read in two places in `ProcessFrontendNetworkMessages` (line ~600: copied into per-slot status table via `(&DAT_00496980)[*(int *)(pcVar13 + 0xbe8)] = DAT_0049722c;`) and broadcast to peers as DXPDATA opcode 0x10 / 0x11. | port `s_lobby_status[local_slot]`; semantic equivalent in lobby state machine TBD |
| 0x004aaf98 + 0x4 | u32 | -- second slot of `g_cameraCycleDebounceTable` -- already covered | -- | -- | -- |
| 0x00466f88 + 0x4 | u32 | -- second slot of `g_cameraLookBackHoldTable` -- already covered | -- | -- | -- |

### Per-slot lobby presence / chat scrollback ring (`ProcessFrontendNetworkMessages` heap)

These are the local mirror of the DXPlay-distributed lobby state. All reads happen inside `ProcessFrontendNetworkMessages` and `RunFrontendLobbyStatusPanel` / `RunFrontendLobbyChatPanel`.

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x00496978 | u32 | `g_lastReceivedDxpType` | high | Out-param of `DXPlay::ReceiveMessage((DXPTYPE*)&DAT_00496978, ...)` at 0x0041b690 and 0x0041d5b4. Read by the switch at the top of `ProcessFrontendNetworkMessages` (`if (DAT_00496978 == 1) ... else if (DAT_00496978 == 2) ... else if (DAT_00496978 == 4)`) and the case-0x11 drain in the lobby. Same DXPTYPE enum the port encodes as `TD5_DXPDATA`/`TD5_DXPCHAT`/`TD5_DXPSTART` etc. -- but here only types 1/2/4 are emitted by the lobby (chat = 2, frontend opcodes = 1, start broadcast = 4). | port `s_handlers[]` dispatch table (collapsed) |
| 0x00496980 | u32[6] | `g_lobbySlotStatusTable` | high | 6-entry per-slot status array, stride 4. Written by opcodes 0x10 (`(&DAT_00496980)[DAT_00497064[1]] = DAT_00497064[2]`) and 0x11 (same plus track/game-type), and by the periodic outbound broadcast at 0x0041bc0a. Read by `RenderFrontendLobbyStatusPanel` to pick the "Ready/Joining/Racing" string row from `SNK_NetPlayStatMsg_exref + status*10`. Mirrors per-slot `g_lobbyPlayerStatus`. | (none -- port maintains per-slot status via `s_lobby_status[]`) |
| 0x00496998 | u32 | `g_lobbyHostBootstrapDone` | high | Set to 1 once at top of host-only branch in `ProcessFrontendNetworkMessages` (0x0041b977-0x0041b97d) the first time the local slot equals the host slot, gating the one-shot "DXPlay-Now-Host" broadcast and per-slot SeshLeave broadcasts. Cleared to 0 in the else branch (we're not host). Also reset to 1 in `CreateFrontendNetworkSession`. Distinguishes "first frame after we became host" from steady-state. | (none) |
| 0x0049699c | u32 | `g_processNetworkMsgsCallCount` | high | Pre-incremented at function entry (`DAT_0049699c = DAT_0049699c + 1;`). Sole reader is the increment instruction itself -- this is a debugging/profiling tick counter. Single-purpose. | (none -- not needed) |
| 0x00497060 | u32 | `g_lobbyMsgRingReadIdx` | high | Local 8-entry lobby message ring read index. Wraps via `& 7`. Read at 0x0041b69c-0x0041b6bb in `ProcessFrontendNetworkMessages` (replay of locally-queued chat/data when DXPlay's `ReceiveMessage` returns no remote data). Cleared in `RunFrontendCreateSessionFlow` entry. | port equivalent: `s_ring_read` (different ring -- see Key Discoveries) |
| 0x0049705c | u32 | `g_lobbyMsgRingWriteIdx` | high | Writer index. Incremented in `QueueFrontendNetworkMessage` (0x00418c60) and reset in `RunFrontendCreateSessionFlow` entry. Buffer-full guard: `uVar2 = DAT_0049705c + 1 & 7; if (uVar2 != DAT_00497060) {...}`. | port `s_ring_write` |
| 0x004969a0 | u32[8] | `g_lobbyMsgRingTypeTable` | high | Per-slot type field of the lobby ring (8 entries, stride 4). Written by `QueueFrontendNetworkMessage` (offset `&DAT_004969a0 + DAT_0049705c * 4`); read by `ProcessFrontendNetworkMessages` replay (offset `&DAT_004969a0 + DAT_00497060 * 4`). Stride 4. | port `s_ring[i].msg_type` |
| 0x0049703c | u32[8] | `g_lobbyMsgRingSizeTable` | high | Companion size field, 8 entries × 4 bytes. Same indexing as type table. | port `s_ring[i].msg_size` |
| 0x004969d8 | u8[8][0xc4] | `g_lobbyMsgRingPayloadBlob` | high | Payload storage, 8 entries × 0xc4 (196) bytes stride. Read by `ProcessFrontendNetworkMessages` (`DAT_00497064 = &DAT_004969d8 + DAT_00497060 * 0xc4`) and copied by `QueueFrontendNetworkMessage`. For type-2 (chat) the first byte is overwritten with the sender slot id (`(&DAT_004969d8)[DAT_0049705c * 0xc4] = dpu_exref[0xbe8];`), payload starts at +1. **Struct-promotion candidate**: this and the 3 fields above are 8 × `{u32 type; u32 size; u8 payload[0xbc]}` (0xc4 == 4+4+0xbc). | port `s_ring[i].payload[]` (sized 0x23C not 0xc4 -- divergence flagged) |
| 0x00497064 | char* | `g_currentMsgPayloadPtr` | high | Pointer cursor into the active payload during DXPTYPE dispatch. Written by `DXPlay::ReceiveMessage` out-param and by the replay branch. Read by every opcode handler (`DAT_00497064[1]`, `DAT_00497064[2]`, etc -- treating it as a `char *` into the payload). | port: `void *payload` param passed to each handler |
| 0x004972c8 | u32 | `g_lastReceivedMsgSize` | high | Out-param of `DXPlay::ReceiveMessage(..., ..., (ulong *)&DAT_004972c8)` at 0x0041b690 and 0x0041d5b4. Read in the replay path (`_DAT_004972c8 = *(undefined4 *)(&DAT_0049703c + DAT_00497060 * 4)`). | port: `int size` param |

### Lobby per-slot driver-name buffer (read by status panel + sent on opcode 0x17)

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x004970bc | u8[6][0x3d] | `g_lobbyDriverDisplayNameTable` | high | 6 rows × 0x3d (61) bytes stride. Walked by the "broadcast SeshLeave" loop in `ProcessFrontendNetworkMessages` at 0x0041b9b2 (`pcVar16 = (char *)&DAT_004970bc; ... pcVar16 = pcVar16 + 0x3d`) and the host-of-session twin loop at 0x0041bb17. Stride confirmed by `iVar5 + 4 < 0xbe4 && pcVar16 += 0x3d` (6 iterations: 0xbcc -> 0xbe4 in dpu_exref). Each row holds the printable driver display name for the corresponding slot's last-known-state. | (none -- port reads from `s_roster[i].name`) |

### Frontend-side network host/local-slot mirror tables (copied per tick from `dpu_exref + 0xbcc` / `+0xa64`)

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x00497230 | u32[6] | `g_dpuSlotStateMirror` | high | Per-tick snapshot of dpu_exref's 6-entry per-slot active/local bitfield (`+0xbcc`). Loop at end of `ProcessFrontendNetworkMessages` (0x0041bbf0): `*puVar8 = *(undefined4 *)pcVar14;` where `pcVar14 = pcVar13 + 0xbcc`. Frontend reads this avoid taking the import-table indirection every frame. | (none -- direct read in port) |
| 0x004970bc + per-row | (covered above) | -- | -- | -- | -- |
| 0x004969d4 | u32 | `g_randomSeedForRace` | high | Already named (USER_DEFINED). Listed for completeness as it's the DXPlay seed echo. | port `s_session.seed` |
| 0x00497248 | u32 | `g_dpuLocalHostSlotMirror` | high | At end of `RunFrontendNetworkLobby` case 0xc: `_DAT_00497248 = *(undefined4 *)(pcVar7 + 0xbe4);` -- saves the host's slot index as seen by the local DPU at the moment the lobby finalises and transitions to the race. Read once during race-init. | (none) |
| 0x0049697c | u32 | `g_dpuLocalSlotMirror` | high | Same pattern: `_DAT_0049697c = *(undefined4 *)(pcVar7 + 0xbe8);` -- saves local slot index at lobby-exit. | port `s_local_slot` |

### Race-settings block published as DXPDATA opcode 0x15 (host -> all clients)

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x00497250 | u32 | `g_dxpdataRaceSettingsGameType` | high | First dword of a 0x1e × 4 = 0x78-byte block. At lobby case 0xc: `DAT_00497250 = g_selectedGameType;`. Block sent as `DXPlay::SendMessageA(1, &DAT_004968ac, 0x80)` (opcode 0x15 broadcast in case 0xf at 0x0041d4ac). Receiver (opcode 0x15 handler) copies 0x1e dwords into this address. | (none -- port re-derives) |
| 0x00497254 | u32 | `g_dxpdataRaceSettingsScheduleIndex` | high | Lobby case 0xc: `DAT_00497254 = g_selectedScheduleIndex;`. Second dword of the same block. | (none) |
| 0x00497258 | u32 | `g_dxpdataRaceSettingsTrackDirection` | high | Lobby case 0xc: `_DAT_00497258 = _g_selectedTrackDirection;`. Third dword of the same block. The full 0x78-byte block also embeds the host's per-slot car/skin/position selection picked up from `gFrontendSlotExtCarIdTable[hostSlot]`, `gFrontendSlotCarIdTable[hostSlot]`, `gFrontendSlotPositionTable[hostSlot]`, `DAT_004972b0[hostSlot]` (already-named slot tables). | (none) |
| 0x0049725c | u8[6] | `g_lobbyRoleAcceptedTable` | high | 6 per-slot "this slot's local config has been confirmed valid" flags, populated in lobby case 0xc by walking `g_lobbySlotStatusTable` and checking `*piVar6 == 2`. Read by lobby cases 0xd (wait-for-all-config-arrived), 0xe (clear-receipts), 0xf (wait-for-all-settings-acked). | (none) |
| 0x00497262 | u8[6] | `g_lobbyConfigReceiptTable` | high | 6 per-slot "we've received this slot's config" receipts. Set to 1 in opcode 0x14 / 0x16 handlers (`(&DAT_00497262)[pcVar16[1]] = 1;` in ProcessFrontendNetworkMessages). Cleared in lobby case 0xc on entry. Drives the gate-loop in case 0xd. | (none) |
| 0x00497320 | u32 | `g_pendingProfileBroadcastCount` | high | Set to slot-count by some caller (not in batch -- likely race-init); decremented by opcode 0x18 handler (`DAT_00497320 = DAT_00497320 + -1`). The opcode-0x18 handler also calls `QueueFrontendNetworkMessage(1, payload, 0x20)` -- re-queues the SAME message it just received as the local copy. Indicates a "I should still send X more profile broadcasts" countdown. | (none) |

### Per-iteration ack/disconnect latches in lobby cleanup branches

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x00497328 | u32 | `g_lobbyAbortRequestLatch` | high | Set to 1 by opcode 0x12 handler (`if (*(int *)(dpu_exref + 0xbe8) == (int)DAT_00497064[1]) { DAT_00497328 = 1; }`) — a "host or peer kicked us" / "abort lobby" notification. Read by lobby case 3, 8 and entry to drive the goto-back-to-frontend cleanup branch (sets `g_networkSessionActive = 0`, calls `DXPlay::Destroy()`). Reset to 0 by lobby entry. | port `s_kicked` (TBD -- ARCH-DIV against `s_connection_lost`) |
| 0x004968a8 | u32 | `g_lobbyHeartbeatTimestamp` | high | `timeGetTime()` cache, updated in `ProcessFrontendNetworkMessages` (`if (799 < DVar9 - DAT_004968a8) { DAT_004968a8 = DVar9; ...send periodic 0x10/0x11 keepalive }`) and in lobby state 0xd at 250ms (`0xf9 < ...`), state 0xf at 165ms (`0xa5 < ...`). The keepalive interval is roughly 800ms in the steady state. Reset to 0 in RunFrontendConnectionBrowser entry. | (none -- port handles heartbeat at a different cadence) |

### Outbound DXPlay packet scratch (used for every `SendMessageA(...)` call)

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x004968ac | u8[~0x80] | `g_dxpOutboundPacketBuf` | high | The first byte (`DAT_004968ac`) is the opcode (0x7f for SeshLeave/SeshNowHost/SeshJoin, 0x10/0x11/0x12/0x13/0x14/0x15/0x16/0x17/0x18 for typed opcodes). Subsequent bytes (`DAT_004968ad..`) are the payload. The full 0x80-byte block is the staging area for `DXPlay::SendMessageA(type, &DAT_004968ac, size)`. Block extent confirmed by opcode 0x15 (0x80-byte settings broadcast). 28 writers, all in network functions; 14 sites pass `&DAT_004968ac` to `SendMessageA`. | port `s_outbound_frame` and per-call stack buffers; not byte-mirrored |
| 0x004968ad | u8 | (inside `g_dxpOutboundPacketBuf` at +1) | -- | First payload byte; usually the sender's slot index. Operand-encoding artifact -- do not name separately. | -- |

### Lobby UI surface state (lifetime owned by `RunFrontendNetworkLobby`)

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x00497310 | void* | `g_lobbySessionHeaderSurface` | high | Surface tracked via `CreateTrackedFrontendSurface(0x1e0, 0x20)` at 0x0041c52a. Holds the "Session: XYZ / Player: ABC" header. Rendered by `RenderFrontendLobbyStatusPanel` (3 reads). Released in `ReleaseTrackedFrontendSurface` paths. | (none -- port uses an in-place draw) |
| 0x0049731c | void* | `g_lobbyPlayerRosterSurface` | high | Surface created at lobby entry (`DAT_0049731c = CreateFrontendDisplayModeButton((byte*)SNK_StatusButTxt_exref, -0xe0, 0, 0xe0, 0x86, 0)`). The 6-slot roster panel ("Status" tab) -- holds per-slot driver names + status strings. Read by `RenderFrontendLobbyStatusPanel` 3 sites. Stride = 0x10 per slot. | (none) |
| 0x00497318 | void* | `g_lobbyDecorationStripSurface` | high | Surface at lobby entry: `CreateFrontendDisplayModeButton((byte *)0x0, -0x1d0, 0, 0x1d0, 0x18, 0)` -- a thin (0x18 tall) decoration strip with no text. Never rendered explicitly here; persists through the lobby lifetime; released by frontend teardown. | (none) |
| 0x0049628c | int* | `g_lobbyErrorDialogSurface` | high | Created at `RunFrontendNetworkLobby` case 6 entry as `DAT_0049628c = CreateTrackedFrontendSurface(0x1e2, 0x40);` -- the "Connection lost / No remote players / Host left" error dialog. Filled by 4 case-6 branches selecting `SNK_NetErrString{1,2,3,4}_exref` pairs. Released on dismiss. | (none -- port logs via TD5_LOG_E and returns to main menu) |

### Connection (service-provider) browser state

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x004970a8 | u32 | `g_connBrowserCursorIndex` | high | 0-based hovered-row index in the 4-row visible connection-browser list. Set by mouse-click coordinates `(*(int *)(g_appExref + 0x104) - DAT_00499c7c + ...)/16` in case 6. Read by RunFrontendCreateSessionFlow entry guard `if (DAT_004970a8 != 0) { g_frontendInnerState = 0x10; }`. | (none -- port has only ENet/LAN list) |
| 0x00497038 | u32 | `g_connBrowserScrollOffset` | high | First-visible-row in the 4-row window. Adjusted by up/down keys (`DAT_00497038 = DAT_00497038 - 1` etc) and clamped against `*(int *)(dpu_exref + 0x1e0)` (= total enumerated providers). Reset to 0 on browser entry. | (none) |
| 0x0049730c | u32 | `g_connBrowserRedrawDirty` | high | Increments on every redraw of the connection-browser strip (`DAT_0049730c = DAT_0049730c + 1`). LSB used as a checker-pattern toggle (`if ((DAT_0049730c & 1) == 0) { fill_color=0x9c9c9c } else { fill_color=0 }`) for alternating-row highlight inside the BltColorFill. Looks like the dirty/animation counter for the highlight blink. | (none) |
| 0x004970ac | char[60] | `g_localComputerName` | high | Filled at browser entry: `GetComputerNameA(&DAT_004970ac, &local_8);` (local_8 = 16). Falls back to literal `"Clint Eastwood"` (s_Clint_Eastwood_00465f84) if `GetComputerNameA` fails or returns empty. Used as default session name. Capacity ~60 chars (next named global at 0x004970bc). `DAT_004970e7 = 0` truncation at offset 0x3b == 59 in failure path. | port `s_session.name` default |
| 0x00499c78 | u32 | `g_connBrowserListOriginX` | medium | Used as base-X coordinate in `RunFrontendConnectionBrowser` cases 5/6 (scrollbar arrow positions). Set elsewhere (not in this batch); read-only here. **Confirmation needed**: this overlaps with the wider frontend overlay positioning system. | (none) |
| 0x00499c7c | u32 | `g_connBrowserListOriginY` | medium | Companion Y. Same sites. Same caveat. | (none) |
| 0x00499ca8 | u32 | `g_connBrowserListRowStride` | medium | Used in row-hit-test: `iVar5 = (*(int *)(g_appExref + 0x104) - DAT_00499c7c) + -0x1c + DAT_00499ca8;`. Effectively the row-stride compensator. Read-only in this batch. | (none) |

### Cheat-decoder side-effects (writer outside this area but exposed here)

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x00496288 | void* | `g_browserScrollIndicatorSprite` | medium | Sprite handle used by `QueueFrontendOverlayRect` in case 5/6 of the connection browser ("up arrow" / "down arrow" 0xc×0xc red glyphs). Set elsewhere. | (none) |
| 0x00496284 | void* | `g_browserSelectionBarSprite` | medium | Sprite handle for the highlight bar (0x1b × 0xc) overlaid on the selected row. Set elsewhere. | (none) |

### Lobby in-progress error string set (low confidence)

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x00497314 | int* | `g_lobbyChatPanelSurface_OR_genericButton` | low | **Confusing**: WRITTEN in lobby entry as the chat panel surface (`CreateFrontendDisplayModeButton(... 0x200, 0x80, 0)`) but also written in 5+ other frontend screens (connection browser, create session, main menu) as "generic display-mode button surface". Same address reused as a transient surface slot. Marked low-confidence: this is the **frontend's shared chat/options-panel surface** rather than a network-specific global. **DO NOT rename** -- file a comment that says "transient surface, owner is whichever frontend screen is active" and let the consolidation session decide. | (none) |
| 0x0048f37c | (covered in PollRaceSessionInput section) | -- | -- | -- | -- |

## Key discoveries

1. **The original lockstep cadence is gated by `DXPlay::HandlePadHost / HandlePadClient`, NOT by an explicit frame counter or barrier.** Each tick `PollRaceSessionInput` (in-race) calls one or the other depending on `dpu_exref + 0xc0c` (host bit). The pad-exchange function INTERNALLY blocks until all players have submitted -- there is no exposed "frame number" in the binary other than `g_smoothedFrameDt` flowing in/out as float seconds. **Source-port divergence flag**: td5_net.c builds an elaborate event/ring/generation system. Orig hides all of that inside `M2DX.DLL!DXPlay::HandlePadHost` and `DXPlay::HandlePadClient`. The port's `s_sync_sequence` / `s_sync_generation` / `s_evt_sync` machinery has NO direct counterpart in the orig binary's data segment -- the orig probably has them inside M2DX (out of reach here).

2. **Max-player count is 6 across the entire game.** Confirmed three ways: (a) per-slot loops in `ProcessFrontendNetworkMessages` (`iVar5 < 0xbe4` starting at `0xbcc`, stride 4 = 6 dwords); (b) chat slot fields in the ring (`(char)pbVar6[-1] < '\x06'` test in `RenderFrontendLobbyChatPanel`); (c) `g_lobbySlotStatusTable` (`&DAT_00496980` to `&DAT_00496998`, span = 0x18 = 6 dwords). Source-port `TD5_NET_MAX_PLAYERS = 6` is correct.

3. **DXPTYPE opcode space differs from port's assumption.** The orig binary uses TWO distinct opcode namespaces wrapped in the same `DXPTYPE` enum:
   - **DXPlay transport opcodes** (passed to `DXPlay::SendMessageA(type, ...)`): only **1, 2, 4** seen in this batch. Type 1 = "frontend data" (generic, payload starts with a 1-byte sub-opcode); type 2 = "chat" (UTF-8 string); type 4 = "start race" (no payload).
   - **Frontend sub-opcodes** (first byte of a type-1 payload): **0x10..0x18 and 0x7F**. 0x7F is a "system message" envelope (SeshCreate / SeshJoin / SeshLeave / SeshNowHost / WaitForHost) -- string-bodied, displayed in chat. 0x10..0x18 are typed lobby control (slot status, ready, kick, profile reqs, settings publish, etc).
   
   The port's `s_handlers[13]` array models 13 distinct top-level types (DXPFRAME=0, DXPDATA=1, DXPCHAT=2, ...). **This is a divergent design choice**: orig piggybacks every non-frame message on type-1 with sub-opcodes, while the port flattens them into top-level distinct types. Wire-protocol-incompatible.

4. **The lobby's "Send chat / Send opcode" function is `QueueFrontendNetworkMessage` (0x00418c60), which writes into a LOCAL 8-entry ring (`g_lobbyMsgRingTypeTable/SizeTable/PayloadBlob`) BEFORE the message is even handed to DXPlay.** This local ring is then DRAINED by `ProcessFrontendNetworkMessages` (when `DXPlay::ReceiveMessage` returns no remote data, the drain feeds local-queued messages through the same dispatch). This makes the local player's outgoing messages also reach the lobby UI without a network round-trip -- they're echoed back locally. Port `s_ring` is structurally similar (16 entries vs 8, 0x23C payload vs 0xc4) but **the port lacks the local-echo design**: the port immediately sends and never replays locally. **Source-port divergence**: if the player can't see their own chat messages, that's the missing replay loop.

5. **Local 8-entry lobby ring payload stride is 0xc4 (196 bytes), NOT the 0x244 (580 bytes) used by the port's `RingEntry`.** Stride was confirmed by the indexing arithmetic at 0x00418c8c (`puVar3 = (undefined4 *)(&DAT_004969d8 + DAT_0049705c * 0xc4)`). The port's 0x23C payload claim in `td5_net.c:46` is based on M2DX-internal buffer estimates, not the lobby ring. The lobby ring's 0xc4 is sized for chat strings + small opcodes, not for the 0x80-byte frame messages.

6. **The "DXPlay session name + max-player count" are passed to `NewSession` via stack args, NOT through global state.** `CreateFrontendNetworkSession` calls `DXPlay::NewSession((char *)&DAT_00497068, (char *)&DAT_00496ff8, g_selectedGameType, (uint)(byte)(&DAT_00465f7c)[g_selectedGameType], g_randomSeedForRace)`. The 4th arg is `g_maxPlayersByGameType[g_selectedGameType]` -- a tiny LUT at `0x00465f7c`. **The 4 max-player-count bytes at 0x00465f7c are the cardinal source of truth for per-game-type max-player count**: this should be enumerated and named, but it's outside this batch's scope (config-LUTs area).

7. **`g_lobbyHostBootstrapDone @ 0x00496998` toggles between 0 (we're not host) and 1 (we are host AND we've published the SeshNowHost broadcast).** This is a **migration-aware** latch -- it cleanly resets when the host quits and another player gets promoted (their `dpu_exref + 0xbe4` will now equal `+ 0xbe8` and on the NEXT call to `ProcessFrontendNetworkMessages` they'll see `DAT_00496998 == 0` and trigger the SeshNowHost re-broadcast). **The port currently has nothing equivalent and will not signal "host migration" in the chat scrollback.** Cosmetic.

8. **The 0x12 (kick / leave) opcode does NOT differentiate "I was kicked" from "host disconnected".** The opcode-0x12 handler sets `g_lobbyAbortRequestLatch = 1` unconditionally when the target slot matches our local slot, regardless of who sent it. Port behaviour-equivalent: `s_connection_lost = 1`. **No port-side missing behaviour** (both cases lead to the same "leave lobby" cleanup).

9. **The session-banner sizing globals `DAT_004962c4/c8/cc/bc/c0` are NOT network-specific.** They're shared across every frontend screen that wants to draw the "TEST DRIVE 5" logo banner. ~100 readers across the whole frontend. Mentioned here only because the lobby uses them; consolidation should put them in the frontend-overlays batch (next T5 batch, if any).

## Out-of-scope finds

| address | brief note | probable area |
|---|---|---|
| 0x0048f308 | First dword of the player1-side car/livery setup; written by car-selection screens; read here only to seed lobby broadcast | car-selection (Tier 5 future batch) |
| 0x0048f31c | Same area | car-selection |
| 0x0048f334 | Player1 selected position/lane | car-selection |
| 0x0048f338 | Player1 selected tuning/transmission flags | car-selection |
| 0x0048f368 | Already labeled DAT only -- referenced as `g_player1_setup_carid` candidate | car-selection |
| 0x0048f370 | Player1 setup position duplicate | car-selection |
| 0x0048f378 | `g_player1AutoPauseLatch` (already labeled with `_` prefix) -- twin of `g_player2AutoPauseLatch` | input |
| 0x0048f380 | `g_player1AutoPauseLatch + 8` -- next slot in same cluster | input |
| 0x00465f7c | u8[N] LUT: max-players-per-game-type; passed as `NewSession` arg | config-LUT |
| 0x004962bc | "Lobby created / session-ready" generic flag, 17 readers | frontend session state (T5) |
| 0x004962c0 | Lobby-screen-active flag, 14 readers | frontend session state (T5) |
| 0x004962c4 | Session banner width -- 100 readers across all frontend screens | frontend overlay (T5) |
| 0x004962c8 | Session banner height | frontend overlay (T5) |
| 0x004962cc | Session banner Y-offset | frontend overlay (T5) |
| 0x00496288 | Browser scroll-indicator sprite handle (set elsewhere) | frontend sprite cache |
| 0x00496284 | Browser selection-bar sprite handle | frontend sprite cache |
| 0x00496358 | Frontend menu string-label surface (`CreateMenuStringLabelSurface(5)`) | frontend surfaces |
| 0x00496408 | Chat scrollback row count (read in `RenderFrontendLobbyChatPanel`, writers in opcode-1/2 handlers and `CreateFrontendNetworkSession`) | borderline -- could be added here as `g_chatScrollbackCount` |
| 0x0049640c | Chat scrollback dirty flag | borderline -- `g_chatScrollbackDirty` |
| 0x00496410 | Chat scrollback storage `(rows * 0x31 dwords stride)` | borderline -- `g_chatScrollbackBuf` (6 × 0xc4 bytes = same layout as lobby ring entries) |
| 0x00497068 | Session-name input buffer (64 bytes) -- only read in this batch's NewSession call; writer in name-input loop is mostly here | borderline -- could be added as `g_sessionNameInputBuf` |
| 0x00496ff8 | Player-name input buffer (64 bytes) -- referenced in 22 places across frontend; main "player name" state | frontend player-profile (T5 future) |
| 0x00497aa0/a4/a8 | 3-dword player-profile header (initials + counter): writer at `InitializeFrontendPresentationState`, read here once to build opcode 0x18 | frontend player-profile (T5 future) |
| 0x00497ac0 | Player-profile name string (separate from `g_localComputerName`): same area | frontend player-profile |
| 0x00499c78/c7c/ca8 | Connection-browser overlay coords (set by app-context init) | frontend overlay |

## TODO impact

No active TODOs target the network multiplayer area. The closest tangentially related items in MEMORY.md are:

- **`reference_arch_cardef_per_actor_indirection`** -- unrelated (cardef + actor stride, not network).
- **`todo_traffic_not_moving_2026-05-19`** -- unrelated (cardef seeding for traffic slots, single-player).
- **`reference_arch_find_offset_peer_return_minus_one`** -- unrelated.

**Structural reveal for port `td5_net.c` (flag for the source-port team):**

The port's design at `td5mod/src/td5re/td5_net.c` diverges from orig's protocol in 3 material ways:

1. **DXPTYPE enum is wider in the port (13 top-level types) than in orig (3 transport types: 1=data, 2=chat, 4=start; sub-opcodes 0x10..0x18, 0x7F nested inside type-1 payload).** The port's 13-handler dispatch table makes the wire format **not-compatible with the orig** -- a player on td5re.exe cannot join a player on TD5_d3d.exe + DPLAYX. If wire-compat is desired, the port should collapse handlers 1, 3, 4, 7, 8, 9, 10, 11, 12 into a single "type-1 sub-dispatch" matching orig's 0x10..0x18 set.

2. **No local-echo of outbound messages in the port.** Orig's `QueueFrontendNetworkMessage` writes into a local 8-entry ring that `ProcessFrontendNetworkMessages` drains alongside DXPlay-received messages. This is what makes the local player's chat appear in their own scrollback. Port currently `s_transport_send`s and never echoes -- local player's chat will not show in their own UI.

3. **Per-slot driver display-name strings live in `dpu_exref + 0xa64` (0x3c-byte rows) in orig.** Port has them in `s_roster[i].name` (`char[64]`). Stride is 0x3c (60) in orig vs 64 in port. This is wire-compatible IF the port truncates names to 60 chars before sending; it currently does not.

**Recommendation**: file this as a new memory entry  `reference_arch_dxptype_protocol_divergence_2026-05-20.md` so the source-port team knows the lobby networking is approximated, not byte-faithful. The port works for td5re.exe-to-td5re.exe sessions but cannot interoperate with a TD5_d3d.exe peer.
