# Multiplayer Lobby State Machine — Complete Analysis

## Overview

The multiplayer lobby is implemented as an 18-case switch statement in `ScreenMultiplayerLobbyCore` at `0x0041C330` (screen index 0x0B in the frontend screen table at `0x004655C4`). It manages the entire lifecycle from entering the lobby to launching a race, including chat, player status synchronization, car/driver config exchange, and the pre-race handshake.

The lobby is the central hub of the multiplayer frontend flow. Players arrive here after the connection/session browser flow and leave either by starting a race, exiting to the main menu, or being disconnected/kicked.

**Key global: `DAT_00495204`** — the state counter driving the switch.

---

## Surrounding Screen Flow

Before documenting the lobby states, here is how players reach the lobby and where they go afterward. The screen navigation function `FUN_00414610(screenIndex)` resets `DAT_00495204 = 0` and sets the active screen handler from the pointer table.

### Screen Table (multiplayer-relevant entries)

| Index | Address | Name | Role |
|-------|---------|------|------|
| 0x05 | `0x00415490` | MainMenuHandler | Main menu — "Multiplayer" button navigates to screen 0x08 |
| 0x08 | `0x00418D50` | ScreenConnectionBrowser | DirectPlay service provider selection (TCP/IP, IPX, Serial, etc.) |
| 0x09 | `0x00419CF0` | ScreenSessionBrowser | Available session list with Join/Back buttons |
| 0x0A | `0x0041A7B0` | ScreenNetworkPreLobby | Session creation (host) or name entry + join (client) |
| 0x0B | `0x0041C330` | **ScreenMultiplayerLobbyCore** | The 18-state lobby documented here |
| 0x14 | `0x0040DFC0` | ScreenCarSelection | Car/track selection (reached via "Change Car" button) |
| 0x1D | `0x0041D630` | ScreenMultiplayerSetup | "Session locked" error dialog (shown on failed join) |

### Pre-Lobby Flow (menu to lobby)

```
MainMenu (0x05)
    |
    | [User picks "Multiplayer"]
    v
ScreenConnectionBrowser (0x08)
    |  - Calls DXPlay::ConnectionEnumerate()
    |  - Lists DirectPlay service providers (TCP/IP, IPX, Serial, Modem)
    |  - User scrolls list, picks one with OK
    |  - State 10: stores selected provider index, navigates to screen 0x09
    |  - OR: picks "Back" -> navigates to screen 0x05 (main menu)
    v
ScreenSessionBrowser (0x09)
    |  - State 0: Calls DXPlay::ConnectionPick(selectedProvider) which starts 3s session enum timer
    |  - Lists available sessions with Ok/Back buttons
    |  - User picks a session -> sets DAT_00496350=10, navigates to screen 0x0A
    |  - User picks "Back" -> DAT_00496350=8, navigates to screen 0x08
    v
ScreenNetworkPreLobby (0x0A)
    |
    +-- HOST path (DAT_004970a8 == 0, "Create new session"):
    |     State 0:  Show "Enter New Session Name" text input
    |     State 2:  User types session name, confirms
    |     State 4:  Show "Enter Player Name" text input
    |     State 6:  User types player name, confirms
    |     State 8:  Set g_networkSessionActive=1, navigate to screen 0x14 (car select)
    |     [After car select, navigate to screen 0x0B (lobby)]
    |
    +-- CLIENT path (DAT_004970a8 != 0, "Join existing session"):
    |     State 0x10: Show "Enter Player Name" text input
    |     State 0x12: User types player name, confirms
    |     State 0x14: DXPlay::JoinSession(sessionIndex, playerName)
    |                 On failure -> navigate to screen 0x1D (locked dialog)
    |     State 0x15: Send LOBBY_TEXT join message, navigate to screen 0x0B (lobby)
    |
    v
ScreenMultiplayerLobbyCore (0x0B) *** THIS DOCUMENT ***
```

---

## State Transition Diagram

```
                        +----------+
                        | State 0  |  INIT
                        |  Setup   |
                        +----+-----+
                             |
            +----------------+----------------+
            | (returning     | (first entry,  | (kicked flag
            |  from car      |  or client     |  DAT_00497328)
            |  selection)    |  join)         |
            |                |                v
            |                |         DXPlay::Destroy()
            |                |         -> Screen 0x1D (exit)
            |                |
            v                v
                        +----------+
                        | State 1  |  ANIMATE IN
                        |          |  (20 frames)
                        +----+-----+
                             |
                             v
                        +----------+
                        | State 2  |  TRANSITION COMPLETE
                        |          |  Enable input, set interactive
                        +----+-----+
                             |
                             v
      +------------->  +----------+  <-----------+
      |                | State 3  |              |
      |                |  MAIN    |              |
      |    +-----------+  LOBBY   +-----------+  |
      |    |           +----+-----+           |  |
      |    |                |                 |  |
      |    | [ChangeCar]    | [Exit]    [Start]  |
      |    v                v              |     |
      | Screen 0x14    +--------+          |     |
      | (car select)   | St. 6  |          |     |
      | returns to     | ERROR  |          |     |
      | State 0        | ANIM   |          |     |
      |                +---+----+          |     |
      |                    |               |     |
      |                    v               |     |
      |                +--------+          |     |
      |                | St. 7  |          |     |
      |                | DIALOG |          |     |
      |                | BUTTONS|          |     |
      |                +---+----+          |     |
      |                    |               |     |
      |                    v               |     |
      |                +--------+          |     |
      |                | St. 8  |          |     |
      |                | DIALOG |          |     |
      |                | INPUT  |          |     |
      |                +---+----+          |     |
      |                    |               |     |
      |                    v               |     |
      |                +--------+          |     |
      |                | St. 9  |          |     |
      |   +----------- | RESOLVE |         |     |
      |   |            +---+----+          |     |
      |   |[Yes/Ok->       |[No/cancel]    |     |
      |   | disconnect]    |               |     |
      |   v                +--------->-----+     |
      |  DXPlay::Destroy()                       |
      |  -> Screen 0x05                          |
      |                                          |
      |                                          |
      |  +-----if host-----+                     |
      |  |                  |                     |
      |  v                  |                     |
      | +---------+    +----+---+if client        |
      | | St. 5   |    | St. 5  |& not host       |
      | | PLAYER  |    | wait   |                 |
      | | READY   |    | for    |                 |
      | | CHECK   |    | host   |                 |
      | +----+----+    +--------+                 |
      |      |                                    |
      |      | [all players ready & count >= 2]   |
      |      v                                    |
      |  +--------+                               |
      |  | St. 12 |  SEAL & COLLECT CONFIG        |
      |  | (0x0C) |  DXPlay::SealSession(1)       |
      |  +---+----+  Send LOBBY_KICK to non-ready |
      |      |                                    |
      |      v                                    |
      |  +--------+                               |
      |  | St. 13 |  POLL CONFIG (250ms interval) |
      |  | (0x0D) |  Send LOBBY_REQUEST_CONFIG     |
      |  +---+----+  to each participating slot   |
      |      |                                    |
      |      | [all configs received]             |
      |      v                                    |
      |  +--------+                               |
      |  | St. 14 |  INIT RACE SCHEDULE           |
      |  | (0x0E) |  FUN_0040dac0() - assign AI   |
      |  +---+----+  cars to empty slots          |
      |      |                                    |
      |      v                                    |
      |  +--------+                               |
      |  | St. 15 |  BROADCAST SETTINGS (165ms)   |
      |  | (0x0F) |  Send LOBBY_SETTINGS (0x80B)  |
      |  +---+----+  to each participating slot   |
      |      |                                    |
      |      | [all clients acked]                |
      |      v                                    |
      |  +--------+                               |
      |  | St. 16 |  LAUNCH COUNTDOWN             |
      |  | (0x10) |  Wait 8 ticks                 |
      |  +---+----+  DXPlay::SendMessageA(4,...)  |
      |      |        = DXPSTART                  |
      |      v                                    |
      |  +--------+                               |
      |  | St. 17 |  WAIT FOR START CONFIRM       |
      |  | (0x11) |  ReceiveMessage loop           |
      |  +---+----+  until type==4 received       |
      |      |                                    |
      |      v                                    |
      | FUN_00414a90()                            |
      | = START RACE                              |
      |                                           |
      +-------------------------------------------+
              (State 10 = animate-out of error
               dialog, returns to State 2)
```

---

## Per-State Detailed Description

### State 0 — INITIALIZATION

**Address:** Entry of `ScreenMultiplayerLobbyCore` (`0x0041C330`), case 0

**What it does:**
1. Saves current car/driver configuration into lobby globals
2. **Kick check:** If `DAT_00497328 != 0` (kicked flag), destroys the DXPlay session, resets network globals, and navigates to screen `0x1D` (session locked error). This is the handler for opcode `0x12` (LOBBY_KICK).
3. If the session is sealed (`dpu_exref + 0xC0C != 0`), calls `DXPlay::SealSession(0)` to unseal it (re-entering lobby from car select).
4. Plays menu sound (`DXSound::Play(5)`)
5. Loads background texture (`FUN_00412e30(5)` — loads screen BG index 5)
6. Loads `Front_End/MainMenu.tga` from `Front_End/FrontEnd.zip`
7. Creates UI buttons:
   - Empty text input bar (`DAT_00497318`, width 0x1D0, height 0x18)
   - Message window (`DAT_00497314`, using `SNK_MessageWindowButTxt`, width 0x200, height 0x80)
   - Status panel (`DAT_0049731c`, using `SNK_StatusButTxt`, width 0xE0, height 0x86)
   - "Change Car" button (`SNK_ChangeCarButTxt`, width 200, height 0x20)
   - "Start" button (`SNK_StartButTxt`, width 0x78, height 0x20)
   - "Exit" button (`SNK_ExitButTxt`, width 0x78, height 0x20)
8. Allocates chat input surface (`DAT_00497310 = FUN_00411f00(0x1E0, 0x20)`)
9. Sets `DAT_0049640c = 1` if returning from car select (`DAT_004962bc != 0`), else marks first entry

**DXPlay calls:** `DXPlay::Destroy()` (on kick), `DXPlay::SealSession(0)` (on re-entry from car select)

**UI:** Background, buttons created but off-screen (will animate in)

**Next state:** State 1 (increments `DAT_00495204`)

---

### State 1 — ANIMATE IN

**Address:** Case 1 (inline at `0x0041C58A`)

**What it does:**
1. Clears the screen on first frame (`DAT_0049522c == 1`)
2. Animates button positions over 20 frames (`DAT_0049522c` from 0 to 0x14):
   - Text input bar slides into position
   - Message window slides in from left
   - Status panel slides in
   - Change Car, Start, Exit buttons slide in from right
3. Renders background sprite and chat input surface each frame
4. When `DAT_0049522c == 0x14`: plays transition sound (`DXSound::Play(4)`), advances state

**DXPlay calls:** None

**UI:** Buttons animating into position, background rendering

**Next state:** State 2

---

### State 2 — TRANSITION COMPLETE / ENABLE INPUT

**Address:** Case 2 (inline at `0x0041C6C0`)

**What it does:**
1. Renders background and chat input surface
2. Sets up text input system:
   - `DAT_004969c0 = &DAT_004972cc` (chat input buffer pointer)
   - `_DAT_004969c8 = 0x3C` (max input length: 60 chars)
   - `DAT_004969d0 = 1` (enable text input mode)
3. Resets button selection to index 0
4. Calls `FUN_004258e0()` — enables button interaction mode
5. Sets `DAT_004654a0 = 5` (number of interactive buttons on screen)

**DXPlay calls:** None

**UI:** All elements in final position, text input cursor active

**Next state:** State 3

---

### State 3 — MAIN INTERACTIVE LOBBY

**Address:** Case 3 (at `0x0041C77D`), the largest and most complex case

**What it does:**

This is the idle/interactive state where players chat, change settings, and decide when to start. Each frame:

1. Renders background sprite and chat input surface
2. Checks `DAT_0049722c` (lobby action state):
   - **Value 3 (race start received):** Sets `DAT_00497324 = 1`, `DAT_004a2c8c = 2`, calls `FUN_00414a90()` (launch race). This happens on clients when they receive DXPSTART (type 4).
   - **Value 2 (waiting for host):** Switches to button index 5 (Exit). If local player IS the host (`dpu_exref+0xBE4 == dpu_exref+0xBE8`), resets to button 0 and clears action state.
3. Remaps button indices: button 2 -> 3 (Change Car -> Start mapping), button 1 -> 0
4. Processes input (`DAT_004951e8` = input ready flag):
   - **Button 3 (Change Car):** Sets `DAT_0049722c = 1`, restores font colors to white, cleans up screen, navigates to screen `0x14` (car selection), calls `FUN_0041b610()` (process network tick). If kicked during this, handles disconnect.
   - **Button 4 (Start):** Sets `DAT_0049722c = 2`. If local player is host, jumps to state 5. If client, sends "Wait for host" message via `FUN_00418c60(1, SNK_WaitForHostMsg, len)`.
   - **Button 5 (Exit):** Sets `DAT_0049722c = 1`, `DAT_00496350 = 2`, plays sound, jumps to state 6 (exit confirmation).
   - **Default:** Resets `DAT_0049722c = 0`
5. Calls `FUN_0041b610()` — processes all pending network messages (chat, status updates, opcodes)
6. **Disconnect check:** If `DAT_00497328 != 0` (kicked/disconnect), destroys session, frees resources, navigates to screen `0x1D`
7. Calls `FUN_0041bd00()` — updates lobby player list display (chat scrollback)
8. Calls `FUN_0041b420()` — polls network input
9. Calls `FUN_0041a670()` — updates network status display
10. If `DAT_004969d0 == 2` (text input confirmed = Enter pressed), advances to state 4

**DXPlay calls:** `DXPlay::Destroy()` (on disconnect), plus all calls inside `FUN_0041b610()`: `DXPlay::ReceiveMessage()`, `DXPlay::SendMessageA(1, statusUpdate, ...)` (periodic 800ms status broadcast)

**UI:** Full lobby display — chat window, player list, status panel, buttons

**Next states:**
- State 4 (Enter key pressed — chat submit)
- State 5 (host clicks Start and enough players ready)
- State 6 (Exit button or error)
- Screen 0x14 (Change Car button)
- Screen 0x1D (kicked/disconnected)
- `FUN_00414a90()` (client receives race start signal)

---

### State 4 — CHAT TEXT SUBMISSION

**Address:** Case 4 (at `0x0041CA29`)

**What it does:**
1. Renders background sprite
2. Calls `FUN_004258c0()` — finalizes button setup
3. Calls `FUN_0041c030()` — processes the chat input buffer:
   - If empty, returns 0 (no send)
   - If starts with `*`, checks for admin commands:
     - `*kIlL` — sends LOBBY_KICK (opcode 0x12) to target slot
     - `*fRoM` — sends LOBBY_REQUEST_NAME (opcode 0x17)
     - `*sEnD` — sends custom LOBBY_TEXT with slot prefix
     - `*nAmE` — sends a rename notification
   - Otherwise, converts emoticon text (`:-)`, `:-(`  etc.) to special character codes (0x1B-0x1F)
   - Returns 1 if message should be sent as chat
4. If `FUN_0041c030()` returns nonzero, formats the message via `FUN_00418c60(2, chatBuffer, len)` and sends it via `DXPlay::SendMessageA(2, formattedMsg, len)` (DXPCHAT type)
5. Returns to state 2 (re-enables text input)

**DXPlay calls:** `DXPlay::SendMessageA(2, ...)` (chat message), `DXPlay::SendMessageA(1, ...)` (admin commands)

**UI:** Chat input cleared, message appears in scrollback

**Next state:** State 2 (always)

---

### State 5 — PLAYER READY CHECK

**Address:** Case 5 (at `0x0041CAC4`)

**What it does:**
1. Renders background
2. Writes local player's status value (`DAT_0049722c`) into the per-slot status table: `DAT_00496980[localSlotIdx] = DAT_0049722c`
3. Iterates over all 6 player slots:
   - Counts active players (`dpu_exref+0xBCC[slot] != 0` → active)
   - Counts players with status == 2 (ready)
4. **If ALL active players are ready AND count >= 2:** Transitions to state 0x0C (start pre-race sequence)
5. **If only 1 player is ready:** Sets `DAT_00496350 = 1`, plays sound, transitions to state 6 (show "not enough players" error)
6. **If not all ready:** Sets `DAT_00496350 = 0` (waiting indicator), plays sound, transitions to state 6

**DXPlay calls:** None directly (status is broadcast in state 3 by `FUN_0041b610`)

**UI:** No change (single-frame check)

**Next states:**
- State 0x0C (all ready, 2+ players)
- State 6 (not enough players or not all ready)

---

### State 6 — ERROR/CONFIRMATION ANIMATE IN

**Address:** Case 6 (at `0x0041CB83`)

**What it does:**
1. Animates buttons 0 and 1 (message window and first UI element) sliding into position over 24 frames (`0x18`)
2. Renders background
3. On first frame (`DAT_0049522c == 1`), creates the error/confirmation overlay surface (`DAT_0049628c = FUN_00411f00(0x1E2, 0x40)`) and fills it:
   - **DAT_00496350 < 2** (exit confirmation or not-enough-players):
     - Line 1: `SNK_NetErrString1[DAT_00496350 * 0x28]` — error message
     - Line 2: `SNK_NetErrString2[DAT_00496350 * 0x20]` — additional info
   - **DAT_00496350 >= 2** (disconnect/session-lost):
     - Line 1: `SNK_NetErrString3` — "Connection lost" or similar
     - Line 2: `SNK_NetErrString4` — additional info
4. When animation complete (`DAT_0049522c == 0x18`): plays sound, advances state

**DXPlay calls:** None

**UI:** Error/confirmation message box sliding into view

**Next state:** State 7

---

### State 7 — SHOW DIALOG BUTTONS

**Address:** Case 7 (at `0x0041CD64`)

**What it does:**
1. Renders background and error overlay
2. Calls `FUN_004264e0()` — clears/prepares button area
3. Creates dialog buttons based on `DAT_00496350`:
   - **DAT_00496350 == 0 or 2** (requires Yes/No answer): Creates "Yes" and "No" buttons (`SNK_YesButTxt`, `SNK_NoxButTxt`)
   - **DAT_00496350 != 0 and != 2** (information only): Creates single "Ok" button (`SNK_OkButTxt`)

**DXPlay calls:** None

**UI:** Yes/No or Ok button(s) appear below the error message

**Next state:** State 8

---

### State 8 — DIALOG INPUT HANDLING

**Address:** Case 8 (at `0x0041CE4F`)

**What it does:**
1. Renders background and error overlay
2. Calls `FUN_0041b610()` — continues processing network messages
3. **Disconnect check:** If kicked (`DAT_00497328 != 0`), destroys session, cleans up, navigates to screen `0x1D`
4. Calls `FUN_0041b420()` — polls network input
5. Processes button input (`DAT_004951e8`):
   - **DAT_00496350 == 1** (information/OK): any button press advances
   - **Button 0 (Yes):** If `DAT_00496350 != 2`, sets `DAT_00496350 = 0` (confirm exit), advances
   - **Button 1 (No):** Sets `DAT_00496350 = 1` (cancel), advances
   - **If DAT_00496350 == 2:** advances regardless (forced exit)

**DXPlay calls:** `DXPlay::Destroy()` (on disconnect), plus calls inside `FUN_0041b610()`

**UI:** Dialog with Yes/No or Ok buttons, waiting for user input

**Next state:** State 9

---

### State 9 — DIALOG RESOLUTION

**Address:** Case 9 (at `0x0041CF9B`)

**What it does:**
1. Renders background and error overlay
2. Calls `FUN_00426390()` — cleans up screen resources
3. Calls `FUN_00426540()` — additional cleanup
4. Resolves based on `DAT_00496350`:
   - **DAT_00496350 == 0** (user confirmed "Yes" to exit or start): Jump to state 0x0C (start pre-race sequence)
   - **DAT_00496350 == 2** (forced disconnect/exit): Full cleanup — free message window surface, free overlay surface, reset font colors, navigate to screen `0x05` (main menu), call `DXPlay::Destroy()`, reset all network globals
   - **DAT_00496350 == 1** (user chose "No" / cancel): Play sound, go to state 10 (animate error dialog out, return to lobby)

**DXPlay calls:** `DXPlay::Destroy()` (on forced exit)

**UI:** Dialog dismissed

**Next states:**
- State 0x0C (confirmed start)
- State 10 (canceled, return to lobby)
- Screen 0x05 (main menu, on forced disconnect)

---

### State 10 (0x0A) — ERROR DIALOG ANIMATE OUT

**Address:** Case 10 (at `0x0041D0C2`)

**What it does:**
1. Animates buttons and error overlay sliding out (reverse of state 6) over 24 frames
2. When complete (`DAT_0049522c == 0x18`):
   - Plays transition sound
   - Frees error overlay surface (`DAT_0049628c = FUN_00411e30(DAT_0049628c)`)
   - Resets `DAT_0049722c = 0` (clear lobby action state)
   - Returns to state 2 (re-enable input)

**DXPlay calls:** None

**UI:** Error dialog animating off-screen

**Next state:** State 2

---

### State 12 (0x0C) — SEAL SESSION & COLLECT CONFIGS (Host Only)

**Address:** Case 0x0C (at `0x0041D195`)

**What it does:**
1. Renders background
2. Frees any existing overlay surface
3. **Builds participant table:** For each of 6 slots, copies active flag from `dpu_exref+0xBCC`. If a slot's status (`DAT_00496980[slot]`) is NOT 2 (not ready), marks it as inactive. Clears all "config received" flags (`DAT_00497262[slot] = 0`).
4. **Seals the session:** `DXPlay::SealSession(1)` — prevents new joins
5. **Kicks non-ready players:** For each slot that is inactive but has a connected player (`dpu_exref+0xBCC[slot] != 0`), sends `LOBBY_KICK` (opcode `0x12`) via `DXPlay::SendMessageA(1, {0x12, slotIdx}, 8)`
6. **Stores host's own config:**
   - `DAT_00497254 = DAT_004a2c90` (track schedule)
   - `DAT_00497250 = DAT_0049635c` (game type)
   - `_DAT_00497258 = DAT_004a2c98` (reverse flag)
   - Per-slot car/driver/variant/flags for host's own slot
7. Sets host's config-received flag to 1
8. Stores host slot index and DPID

**DXPlay calls:** `DXPlay::SealSession(1)`, `DXPlay::SendMessageA(1, LOBBY_KICK, 8)` per non-ready slot

**UI:** Background only (transition frame)

**Next state:** State 0x0D

---

### State 13 (0x0D) — POLL CLIENT CONFIGS (Host Only, 250ms interval)

**Address:** Case 0x0D (at `0x0041D327`)

**What it does:**
1. Renders background
2. Calls `FUN_0041b610()` — processes network (receives opcode 0x14 config replies)
3. Every 250ms (`timeGetTime() - DAT_004968a8 > 0xF9`):
   - Iterates over all 6 slots
   - For each participating slot (active flag set) that has not yet sent config (`DAT_00497262[slot] == 0`):
     - If the player disconnected (`dpu_exref+0xBCC[slot] == 0`), marks as inactive
     - Otherwise, sends `LOBBY_REQUEST_CONFIG` (opcode `0x13`) via `DXPlay::SendMessageA(1, {0x13, slotIdx}, 4)`
     - Breaks after finding first missing slot (one request per tick)
   - **If all 6 slots have config (or are inactive):** Advances to state 0x0E

**DXPlay calls:** `DXPlay::SendMessageA(1, LOBBY_REQUEST_CONFIG, 4)`, plus `DXPlay::ReceiveMessage()` inside `FUN_0041b610()`

**UI:** Background only (waiting frame)

**Timeout behavior:** No explicit timeout — keeps polling every 250ms until all configs received or players disconnect. If a player disconnects, their slot is cleared and no longer blocks progress.

**Next state:** State 0x0E (all configs collected)

---

### State 14 (0x0E) — INITIALIZE RACE SCHEDULE (Host Only)

**Address:** Case 0x0E (at `0x0041D458`)

**What it does:**
1. Renders background
2. Clears all config-received flags (`DAT_00497262[0..5] = 0`)
3. Sets host's own flag to 1 (`DAT_00497262[localSlot] = 1`)
4. Sets `DAT_00497324 = 1` (race starting flag)
5. Calls `FUN_0040dac0()` — **initializes the race series schedule:**
   - Fills empty (non-participating) slots with AI-controlled cars
   - Randomizes car/driver selection for AI slots (avoiding duplicates)
   - Handles special game types (type 2 = random cars, type 5 = specific car pool)
   - Records timestamp for sync timing

**DXPlay calls:** None

**UI:** Background only

**Next state:** State 0x0F

---

### State 15 (0x0F) — BROADCAST SETTINGS TO CLIENTS (Host Only, 165ms interval)

**Address:** Case 0x0F (at `0x0041D4F0`)

**What it does:**
1. Renders background, resets frame counter
2. Calls `FUN_0041b610()` — processes network (receives opcode 0x16 ack replies)
3. Every 165ms (`timeGetTime() - DAT_004968a8 > 0xA5`):
   - Iterates over all 6 participating slots
   - For each slot that is active, connected, but has not acked (`DAT_00497262[slot] == 0`):
     - If player disconnected, marks as inactive
     - Otherwise: builds a 0x80-byte `LOBBY_SETTINGS` message (opcode `0x15`) containing:
       - 0x78 bytes of settings from `DAT_00497250` (game type, track, per-slot car/driver tables, host info)
     - Sends via `DXPlay::SendMessageA(1, settingsPacket, 0x80)`
     - Breaks after first un-acked slot
   - **If all slots acked:** Advances to state 0x10

**DXPlay calls:** `DXPlay::SendMessageA(1, LOBBY_SETTINGS, 0x80)`

**UI:** Background only

**Timeout behavior:** No explicit timeout — keeps polling every 165ms. Disconnected players are automatically cleared.

**Next state:** State 0x10 (all clients acknowledged settings)

---

### State 16 (0x10) — LAUNCH COUNTDOWN (Host Only)

**Address:** Case 0x10 (at `0x0041D540`)

**What it does:**
1. Renders background
2. Waits for `DAT_0049522c == 8` (8 frames / ticks of animation delay)
3. When ready:
   - Sets `DAT_00497324 = 1` (race active flag)
   - Sets `DAT_004a2c8c = 2` (race mode = multiplayer)
   - Sends `DXPlay::SendMessageA(4, &DAT_0049725c, 0)` — this is **DXPSTART** (message type 4), which triggers the race start handshake protocol (types 4 -> 5 -> 6 -> 7)

**DXPlay calls:** `DXPlay::SendMessageA(4, slotMask, 0)` — DXPSTART

**UI:** Background only (brief pause)

**Next state:** State 0x11

---

### State 17 (0x11) — WAIT FOR START CONFIRMATION (Client)

**Address:** Case 0x11 (at `0x0041D5B1`)

**What it does:**
1. Enters a tight receive loop: `DXPlay::ReceiveMessage()` until:
   - A message of type 4 (DXPSTART) is received, OR
   - `ReceiveMessage` returns nonzero (no more messages)
2. When DXPSTART received: Calls `FUN_00414a90()` — launches the race

**DXPlay calls:** `DXPlay::ReceiveMessage()` in a loop

**UI:** Background only (frozen display while waiting)

**Next state:** Race starts (exits lobby screen entirely)

**Note:** The host reaches this state after sending DXPSTART in state 0x10. The client enters this path when `DAT_0049722c == 3` is set by `FUN_0041b610()` upon receiving a type-4 message — this triggers the race launch from state 3.

---

## DXPlay API Calls by State

| State | DXPlay Call | Purpose |
|-------|-------------|---------|
| 0 | `DXPlay::Destroy()` | On kick — clean up session |
| 0 | `DXPlay::SealSession(0)` | Unseal session (re-entry from car select) |
| 3 | `DXPlay::Destroy()` | On disconnect during lobby idle |
| 3 | `DXPlay::ReceiveMessage()` | Via FUN_0041b610 — process incoming messages |
| 3 | `DXPlay::SendMessageA(1, ...)` | Via FUN_0041b610 — periodic status broadcast (800ms) |
| 4 | `DXPlay::SendMessageA(2, ...)` | Send chat message (DXPCHAT) |
| 4 | `DXPlay::SendMessageA(1, ...)` | Send admin commands (LOBBY_KICK, etc.) |
| 8 | `DXPlay::Destroy()` | On disconnect during dialog |
| 9 | `DXPlay::Destroy()` | On confirmed exit |
| 0x0C | `DXPlay::SealSession(1)` | Seal session (no more joins) |
| 0x0C | `DXPlay::SendMessageA(1, 0x12, 8)` | LOBBY_KICK to non-ready slots |
| 0x0D | `DXPlay::SendMessageA(1, 0x13, 4)` | LOBBY_REQUEST_CONFIG polling |
| 0x0D | `DXPlay::ReceiveMessage()` | Via FUN_0041b610 — receive config replies |
| 0x0F | `DXPlay::SendMessageA(1, 0x15, 0x80)` | LOBBY_SETTINGS broadcast |
| 0x0F | `DXPlay::ReceiveMessage()` | Via FUN_0041b610 — receive ack replies |
| 0x10 | `DXPlay::SendMessageA(4, slotMask, 0)` | DXPSTART — trigger race start protocol |
| 0x11 | `DXPlay::ReceiveMessage()` | Wait for DXPSTART confirmation |

---

## Error Handling and Timeout Behavior

### Kick Detection (DAT_00497328)

The kicked flag `DAT_00497328` is set to 1 when the local player receives opcode `0x12` (LOBBY_KICK) with their own slot index as the target. This is checked in states 0, 3, and 8. The response is always the same:

```
DAT_00497324 = 0
DAT_004962bc = 0
g_networkSessionActive = 0
DXPlay::Destroy()
FUN_00414610(0x1D)  -> Screen "Session Locked" dialog
```

### Disconnect Detection

Player disconnects are detected by `FUN_0041b610()` checking `dpu_exref+0xBCC[slot]` (player active flags maintained by the DXPlay worker thread). When a previously active slot becomes 0:
- A system message "`<PlayerName> has left the session`" (`SNK_SeshLeaveMsg`) is posted to chat
- If the disconnecting player was the host and the local player is promoted, a "`<PlayerName> is now the host`" (`SNK_NowHostMsg`) message is posted

### Host Migration

When `dpu_exref+0xBE4 == dpu_exref+0xBE8` (local player ID matches host ID), the system recognizes the local machine as the new host. The periodic status broadcast in `FUN_0041b610()` automatically switches from opcode `0x10` (LOBBY_STATUS) to opcode `0x11` (LOBBY_STATUS_EXT), propagating track/game-type settings to all clients.

### Pre-Race Timeout (States 0x0D, 0x0F)

The config collection (state 0x0D) and settings broadcast (state 0x0F) have **no hard timeout**. They poll at 250ms and 165ms intervals respectively, and rely on disconnect detection to clear unresponsive players. A player whose connection drops will have their `dpu_exref+0xBCC[slot]` flag cleared by the DirectPlay worker thread, which the polling loops detect and handle by marking the slot inactive.

### Chat System Error Handling

The chat processing function `FUN_0041c030()` at `0x0041C030` handles admin commands silently (returns 0, no chat message sent) and converts emoticons to special characters inline. Invalid admin commands are treated as regular chat text.

---

## Connection Flow: Menu Entry to In-Game

### Complete sequence (Host path):

```
1. MainMenu (0x05) -> "Multiplayer" button
2. ScreenConnectionBrowser (0x08)
   - DXPlay::ConnectionEnumerate() lists service providers
   - User selects TCP/IP, IPX, Serial, or Modem
   - DXPlay::ConnectionPick(index) initializes provider
3. ScreenSessionBrowser (0x09)
   - DXPlay::EnumSessionTimer starts 3s periodic enumeration
   - User sees empty list, selects "Ok" with first entry -> create new
   - DXPlay::EnumSessionTimer(0) stops enumeration
4. ScreenNetworkPreLobby (0x0A)
   - States 0-2: Enter session name
   - States 4-6: Enter player name
   - State 8: g_networkSessionActive = 1
   - Navigate to screen 0x14 (car selection)
5. ScreenCarSelection (0x14)
   - User picks car and track
   - Navigate to screen 0x0B (lobby)
6. ScreenMultiplayerLobbyCore (0x0B)
   - State 0: Init, unseal session, create UI
   - State 1: Animate in
   - State 2: Enable input
   - State 3: Idle in lobby (chat, wait for players)
   - [Players join, status updates fly every 800ms]
   - State 3: Host clicks Start (button 4)
   - State 5: All players ready check passes
   - State 0x0C: Seal session, kick non-ready
   - State 0x0D: Collect car configs from all clients
   - State 0x0E: Initialize race schedule (fill AI slots)
   - State 0x0F: Broadcast settings to all clients
   - State 0x10: 8-frame delay, send DXPSTART
   - State 0x11: Wait for start confirmation
   - FUN_00414a90() -> RACE BEGINS
```

### Complete sequence (Client path):

```
1. MainMenu (0x05) -> "Multiplayer" button
2. ScreenConnectionBrowser (0x08) [same as host]
3. ScreenSessionBrowser (0x09)
   - Sees sessions in list
   - Selects a session, clicks Ok -> join
   - DXPlay::EnumSessionTimer(0) stops enumeration
4. ScreenNetworkPreLobby (0x0A)
   - State 0x10: Enter player name
   - State 0x12: Confirm name
   - State 0x14: DXPlay::JoinSession(sessionIndex, playerName)
     - On failure -> Screen 0x1D (session locked)
   - State 0x15: Send join message, navigate to lobby
5. ScreenMultiplayerLobbyCore (0x0B)
   - State 0: Init, create UI
   - State 1-2: Animate in, enable input
   - State 3: Idle in lobby
   - [Host clicks Start, client receives status updates]
   - [Client receives LOBBY_REQUEST_CONFIG (0x13)]
   - [Client responds with LOBBY_CAR_SELECTION (0x14)]
   - [Client receives LOBBY_SETTINGS (0x15)]
   - [Client sends LOBBY_READY_ACK (0x16)]
   - State 3: DAT_0049722c set to 3 by FUN_0041b610 on receiving DXPSTART
   - State 3: FUN_00414a90() -> RACE BEGINS
```

---

## Key Global Variables

| Address | Name | Role in Lobby |
|---------|------|---------------|
| `0x00495204` | screenState | Main state counter (0-0x11) |
| `0x0049522c` | frameCounter | Animation frame counter, reset per state |
| `0x0049722c` | lobbyActionState | 0=idle, 1=navigating, 2=waiting for host, 3=race starting |
| `0x00497328` | kickedFlag | Set by opcode 0x12, triggers disconnect |
| `0x00496350` | dialogMode | 0=confirm exit, 1=info, 2=forced disconnect, 9=back to connection browser |
| `0x004962bc` | sessionEnteredFlag | 1 after entering lobby, used to detect re-entry from car select |
| `0x00496980[6]` | perSlotStatus | Per-slot status: 0=idle, 1=waiting, 2=ready, 3=starting, 0x7F=empty |
| `0x00497262[6]` | configReceivedFlags | Per-slot flags: 1=config received during pre-race |
| `0x0049725c[6]` | participantFlags | Per-slot participation: 0=inactive, 1=human active, 2=AI |
| `0x00497324` | raceActiveFlag | Set to 1 when race is launching |
| `0x00496408` | chatMessageCount | Number of messages in chat scrollback (max 6) |
| `0x0049640c` | chatDirtyFlag | Set to 1 when chat display needs refresh |
| `0x004968a8` | lastPollTimestamp | timeGetTime() of last periodic network operation |
| `0x004969d0` | textInputState | 1=active, 2=confirmed (Enter pressed) |
| `0x004969c0` | textInputBufferPtr | Points to current text input buffer |
| `0x004972cc` | chatInputBuffer | 60-char buffer for typed chat text |
| `0x00497314` | messageWindowSurface | DirectDraw surface for chat/message display |
| `0x0049731c` | statusPanelHandle | Button handle for status display panel |
| `0x00497310` | chatInputSurface | DirectDraw surface for text input display |

---

## Chat System and Admin Commands

The chat processing function at `0x0041C030` handles special `*`-prefixed admin commands before regular text:

| Command | Opcode Sent | Effect |
|---------|-------------|--------|
| `*kIlL<N>` | `0x12` (LOBBY_KICK) | Kicks player in slot N (1-6) |
| `*fRoM<N>` | `0x17` (LOBBY_REQUEST_NAME) | Requests profile info from slot N |
| `*sEnD<N><text>` | Custom LOBBY_TEXT (0x7F) | Sends text with slot prefix |
| `*nAmE<text>` | LOBBY_TEXT (0x7F) | Sends rename notification |

Regular chat messages are scanned for emoticon patterns and converted to special glyph codes:

| Text Pattern | Glyph Code | Visual |
|-------------|------------|--------|
| `:-)` or `:->` | 0x1B | Smiley |
| `:-(`  or `:-<` | 0x1C | Sad face |
| `;-)` or `;->` | 0x1D | Wink |
| `:o` / `:O` / `:0` | 0x1E | Surprised |
| `%o` / `%O` / `%\` / `%0` | 0x1F | Dizzy |
