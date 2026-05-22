# DA-M1: M2DX Replay Codec Deep Audit

Date: 2026-05-22
Scope: `DXInput::WriteOpen / Write / WriteClose / ReadOpen / Read / ReadClose`
       (plus replay buffer table at 0x10033ba8)
Tools: Ghidra read-only on M2DX.dll (session pool14) and TD5_d3d.exe (session pool14).

---

## TL;DR

The M2DX replay codec is **purely in-memory** — there is no file I/O at any layer.
Recording fills a static DLL buffer at 0x10033ba8 during the race; playback runs
the race a second time with `g_inputPlaybackActive=1`, which routes
`PollRaceSessionInput` to call `DXInput::Read` instead of polling hardware. The
buffer survives only for the process lifetime.

The port has a *complete* byte-faithful implementation of all six functions
(including disk persistence under `[Replay] PersistToDisk` flag) but the wiring
between the frontend "View Replay" button and the race-init read/write branch
is broken: clicking View Replay sets `s_replay_mode_flag` and
`s_playback_active`, but the race-init code (`td5_game.c:1902`) gates on a
*different* static, `s_replay_mode`, which is never written by anyone. So the
race always re-opens in WRITE mode, overwriting the recording before
playback can read it. This is exactly the symptom in
`todo-view-replay-restarts-race-2026-05-19`.

---

## Section A — Orig file format spec

### In-memory header (DLL static, base 0x10033b90)

| Offset       | Size | Field             | Notes                                              |
|--------------|------|-------------------|----------------------------------------------------|
| 0x10033b90   | 4    | track_index       | WriteOpen/ReadOpen param_1                         |
| 0x10033b94   | 4    | entry_count       | `g_inputRecordEntryCount`                          |
| 0x10033b98   | 4    | last_frame_index  | WriteClose stores `frameCursor - 1`                |
| 0x10033b9c   | 12   | (padding/unused)  | header ends before entries; entries start at +0x18 |
| 0x10033ba8   | ...  | entries[]         | 8 bytes per entry, max 0x4E1C (19996) entries      |

### Per-entry encoding (8 bytes)

| Bytes | Field              | Notes                                                          |
|-------|--------------------|----------------------------------------------------------------|
| 0..3  | frame_and_channel  | low 15 bits = frame index, bit 15 (0x8000) = channel-1 flag    |
| 4..7  | value              | the 32-bit input-bits word                                     |

Channel = which 32-bit input word changed:
- `(frame_and_channel & 0x8000) == 0` → channel 0 = player-1 input
- `(frame_and_channel & 0x8000) != 0` → channel 1 = player-2 input

### Caps (hard-coded in Write @ 0x1000a660)

| Constant                        | Value     | Meaning                              |
|---------------------------------|-----------|--------------------------------------|
| Max entries                     | 0x4E1C    | 19996; matches port's TD5_INPUT_REC_MAX_ENTRIES |
| Max frame cursor                | 0x7FEE    | 32750 frames ≈ 1092 s at 30 Hz       |
| Channel-1 flag                  | 0x8000    |                                      |
| Strip mask in record (demo)     | 0xBBFFFFFF | Strips bits 26 (0x04000000) + 30 (0x40000000) when param_2==1 |

The strip mask: bits stripped are `~0xBBFFFFFF = 0x44000000`. From port poll
code these correspond to (bit 30 = escape/menu, bit 26 = camera-related
control). The port's prior memory note misattributed this as "camera+escape
via 0x44000000" — the bit-26 specifically is NOT the camera-cycle bit (that's
bit 24 = 0x01000000); it's a different control bit that ships gets stripped
so the replay can't accidentally cancel itself.

### Initial-state convention

Two entries always seeded at entry_count=2 on the first call to Write
(when frame_cursor==0):

```
entries[0] = { frame_and_channel = 0,        value = word0 }  // channel 0 baseline
entries[1] = { frame_and_channel = 0x8000,   value = word1 }  // channel 1 baseline
```

Subsequent calls emit only deltas: a new entry per channel only if `word !=
last_word`. Frame cursor advances every call regardless.

### Delta-decode invariant on Read

When `frame_cursor == 0`, Read seeds `replay_word0/1` from entries[0..1].
Each subsequent call advances `frame_cursor`; if the next unread entry's
`frame_and_channel & 0x7FFF == frame_cursor`, that entry's value overwrites
the matching channel's replay_word and `read_index` advances. Multiple
entries can fire on one frame (channel 0 then channel 1).

---

## Section B — Orig flow

### Race-start (InitializeRaceSession @ 0x0042aa10)

```c
ClearTrackSegmentVisibilityTable();
if (g_inputPlaybackActive == 0) {
    DXInput::WriteOpen(g_trackPoolIndex);   // 0x0042b29c
} else {
    DXInput::ReadOpen(g_trackPoolIndex);    // 0x0042b28d
}
```

### Per-sub-tick (PollRaceSessionInput @ 0x0042c470)

```c
if (g_inputPlaybackActive != 0) {
    DXInput::Read(&g_playerControlBits);    // 0x0042c487
    DXInput::GetKB();
    if (DXInput::CheckKey(1 /* ESC */) != 0)
        g_playerControlBits |= 0x40000000;
    goto LAB_0042c6fc;
}
// ... normal hardware polling ...
if (*(int *)(dpu_exref + 0xc08) == 0) {                   // non-network local game
    UpdateControllerForceFeedback(0);
    DXInput::Write(&g_playerControlBits, 1 /* strip mode */);  // 0x0042c6ee
    goto LAB_0042c6fc;
}
// network branch (host/client) skips Write
```

Secondary Write call: `RunAudioOptionsOverlay @ 0x0043bf70` (pause-menu Quit
Race) — injects `local_84 = 0x40000000` then `Write(&local_84, 0)` with
strip_mode=0 so the escape bit survives into the replay.

### Race-end (RunRaceFrame @ 0x0042b580)

When the fade-out alpha reaches 255 (`local_10 == _DAT_0045d684`), the
function tears down the race and calls:

```c
if (g_inputPlaybackActive == 0) {
    DXInput::WriteClose();   // 0x0042b85e
} else {
    DXInput::ReadClose();    // 0x0042b856 (no-op: shares VA with DXSound::LoadComplete)
}
```

### Frontend "View Replay" wiring (RunRaceResultsScreen @ 0x00422480 case 0xF button 1)

```c
case 1:                                          // 0x00422F1C
    g_frontendAnimFrameCounter = 0;
    g_frontendBootDispatchMode = 2;              // re-enter race path
    g_inputPlaybackActive = 1;
    g_frontendInnerState = g_frontendInnerState + 1;
    return;
```

After the slide-out animation, case 0x10 calls
`InitializeFrontendDisplayModeState()` which boots a fresh
`InitializeRaceSession`. Because `g_inputPlaybackActive == 1` is still set,
the race-init branch hits `ReadOpen` (and `Read` per tick, `ReadClose` at
end) — replaying the buffer that was filled during the previous race.

The buffer at 0x10033ba8 sits in the DLL's static `.data` section, so it
persists across the race-results screen → menu → race-2 transition without
needing serialization.

---

## Section C — Port-side state & gaps

### What the port has

`td5mod/src/td5re/td5_input.c` has byte-faithful implementations of all six
codec functions with matching constants:

| Port symbol                    | Original                | Notes                                |
|--------------------------------|-------------------------|--------------------------------------|
| `td5_input_write_open()`       | DXInput::WriteOpen      | line 1085, mirror clean              |
| `td5_input_write_frame()`      | DXInput::Write          | line 1175, strip + delta encode      |
| `td5_input_write_close()`      | DXInput::WriteClose     | line 1158, sets last_frame_index     |
| `td5_input_read_open()`        | DXInput::ReadOpen       | line 1307, resets read_index         |
| `td5_input_read_frame()`       | DXInput::Read           | line 1340, decode w/ channel flag    |
| `td5_input_read_close()`       | DXInput::ReadClose      | line 1333, no-op                     |

Port additions (not in orig, gated behind `[Replay] PersistToDisk` INI key):
- 24-byte file header w/ magic `TD5RPLY` + version 1 + track_index +
  entry_count + last_frame_index (matches in-memory header layout)
- `td5_input_replay_flush_to_disk()` called from `write_close()`
- `td5_input_replay_load_from_disk()` called from `read_open()`

Constants match orig exactly:
- `TD5_INPUT_REC_MAX_ENTRIES = 19996` (= 0x4E1C)
- `TD5_INPUT_REC_MAX_FRAMES  = 0x7FEE`
- `TD5_INPUT_REC_CHANNEL1_FLAG = 0x8000`
- `TD5_INPUT_RECORD_STRIP_MASK = 0xBBFFFFFF`

### Where the port wires it

| Port site                                  | Action                                  | Notes                                |
|--------------------------------------------|-----------------------------------------|--------------------------------------|
| `td5_game.c:1902-1908`                     | `if (s_replay_mode) read_open else write_open` | Step 12 of init race                |
| `td5_input.c:382`                          | `td5_input_write_frame(... , 1)` every tick     | Inside `td5_input_poll_race_session` |
| `td5_input.c:287-289`                      | `td5_input_read_frame()` in playback path        | Replaces hardware poll               |
| `td5_game.c:3478-3483`                     | `if (s_replay_mode) read_close else write_close` | Race teardown                        |
| `td5_frontend.c:9548-9550`                 | View-Replay button: `set_replay_mode(1)` + `set_playback_active(1)` | THIS IS THE GAP — see below |

### The exact gap

There are TWO unrelated "replay mode" statics inside the port:

- `s_replay_mode` in `td5_game.c` (file-static, used by the race init
  branch — the only thing that picks read vs write).
- `s_replay_mode_flag` in `td5_input.c` (file-static, used by camera/rear-view
  gating during poll).

The frontend button calls `td5_input_set_replay_mode(1)`, which writes the
`td5_input.c` static. But `td5_game.c:1902` reads its own `s_replay_mode`,
which has only one assignment site (the init at line 628, value 0). Nobody
ever writes 1 to it.

Result: the View-Replay button restarts the race with `s_replay_mode=0`, so
`td5_input_write_open()` is called again, which **clears the recording**
(via `memset(&s_rec, 0, sizeof(s_rec))` at line 1087) before
playback could have read it. From the user's POV the race just restarts as a
fresh recording session — exactly the
`todo-view-replay-restarts-race-2026-05-19` symptom.

`td5_input_set_playback_active(1)` IS wired correctly — but it only governs
the poll path (`s_playback_active` is checked at `td5_input.c:287`). On
its own, with no `read_open()` ever called, `s_rec.entry_count` will be 0
(it was just memset'd by write_open) so `read_frame` returns the
"recording-exhausted" branch and emits zero input forever.

### Cross-effects

The double-static design also affects two minor sites:
- `td5_game.c:4691`: `is_replay_mode_active() = td5_input_is_playback_active() || s_replay_mode` — `s_replay_mode` half is dead branch.
- `td5_render.c:2198`: `height_base_offset = td5_input_is_playback_active() ? -18 : -36` — this works correctly because it reads playback_active.

But the **camera-replay logic in td5_camera.c:2071,2424 and HUD overlay
suppression in td5_hud.c:1755** all key on the global `g_replay_mode`, which
is assigned from `s_replay_mode` at `td5_game.c:1305`. So with the gap,
replay-style camera/HUD never engages during the "replay" race either —
the entire mode is silently a no-op.

---

## Section D — Actionable items to close `todo-view-replay-restarts-race`

The minimum fix is to set `s_replay_mode = 1` (in `td5_game.c`) when the
View Replay button is pressed, before `frontend_init_race_schedule()`
re-enters race init.

1. **Add a setter in td5_game.c** (mirror of `td5_input_set_replay_mode`):
   ```c
   void td5_game_set_replay_mode(int v) { s_replay_mode = v; }
   ```
   and a header declaration in `td5_game.h`.

2. **Wire it in td5_frontend.c:9548** alongside the existing two calls:
   ```c
   td5_game_set_replay_mode(1);          // NEW — picks read_open branch
   td5_input_set_replay_mode(1);         // existing — gates camera/rear-view
   td5_input_set_playback_active(1);     // existing — gates poll path
   frontend_init_race_schedule();
   ```

3. **Reset on race teardown** is already in place: `td5_game.c:628`
   initializes `s_replay_mode = 0` on every game-init, and the
   teardown at line 3483 resets playback_active to 0. After a replay race
   ends the user returns to results, and starting a NEW race will see
   `s_replay_mode=0` (write mode) — correct.

4. **Verification path**: with the fix, click View Replay after a race;
   td5_game.c:1903 should fire `td5_input_read_open("replay.td5")` and
   the per-tick log "Replay open: path=replay.td5 entries=N frames=M"
   should appear with the same N/M as the previous race's "Replay write
   close" line. The replay should reproduce input bit-for-bit because the
   in-memory `s_rec` buffer is reused (it was filled by the previous race
   and never cleared between races — only `frame_cursor` and `read_index`
   reset in `read_open`).

5. **Disk-persistence (optional)**: the port already supports disk
   persistence via `[Replay] PersistToDisk=1`. With the fix, this also
   becomes correct (read_open will load the file if it exists). No
   additional work needed.

**Out of scope** for this TODO (separate bugs / future work):
- The two `s_replay_mode_flag` vs `g_replay_mode` shadows in
  `td5_input.c` and `td5_game.c` should probably be merged into a single
  global once we trust the gate. Today's fix only resolves the visible
  symptom; the cleanup is cosmetic.
- Camera-side replay handling (td5_camera.c) reads `g_replay_mode` which
  is set from `s_replay_mode` at td5_game.c:1305; the fix above ALSO
  closes this path so trackside / orbit cameras engage in replay mode.

---

## Section E — Byte-faithful re-port reference (already implemented; included for completeness)

The codec is small enough to fit on a slide. Anyone redoing this from scratch
needs:

### Globals (translates to a single `RecordingState` struct)

```c
struct ReplayBuffer {
    int32_t  track_index;       // +0x00  (orig at 0x10033b90)
    int32_t  entry_count;       // +0x04  (orig at 0x10033b94)
    int32_t  last_frame_index;  // +0x08  (orig at 0x10033b98 — written by WriteClose)
    int32_t  _pad[3];           // +0x0c..+0x17 unused in orig
    struct {
        uint32_t frame_and_channel;  // low 15 bits = frame, bit 15 = channel
        uint32_t value;
    } entries[19996];           // +0x18..  (orig at 0x10033ba8 onward)
    // Read/Write state lives elsewhere in orig (segment 0x1005a*):
    uint32_t frame_cursor;      // (orig at 0x1005adc0)
    uint32_t last_word0;        // (orig at 0x1005adb8)
    uint32_t last_word1;        // (orig at 0x1005adb0)
    uint32_t read_index;        // (orig at 0x1005adb4)
    uint32_t replay_word0;      // (orig at 0x1005ada0)
    uint32_t replay_word1;      // (orig at 0x1005acb0)
};
```

### The five entry points (pseudocode)

```c
// 0x1000a640
int WriteOpen(int track_idx) {
    rec.track_index = track_idx;
    rec.entry_count = 0;
    rec.frame_cursor = 0;
    return 1;
}

// 0x1000a660 — param_2==1 strips bits 0x44000000
int Write(const uint32_t input[2], uint32_t flags) {
    uint32_t w0 = input[0], w1 = input[1];
    if (flags == 1) {
        w0 &= 0xBBFFFFFF;
        w1 &= 0xBBFFFFFF;
    }
    if (rec.frame_cursor == 0) {
        rec.entries[0] = (Entry){ .frame_and_channel = 0,      .value = w0 };
        rec.entries[1] = (Entry){ .frame_and_channel = 0x8000, .value = w1 };
        rec.last_word0 = w0;  rec.last_word1 = w1;
        rec.entry_count = 2;  rec.frame_cursor = 1;
        return 1;
    }
    if (rec.entry_count < 0x4E1C && rec.frame_cursor < 0x7FEE) {
        if (w0 != rec.last_word0) {
            rec.entries[rec.entry_count++] = (Entry){ rec.frame_cursor,           w0 };
            rec.last_word0 = w0;
        }
        if (w1 != rec.last_word1) {
            rec.entries[rec.entry_count++] = (Entry){ rec.frame_cursor | 0x8000,  w1 };
            rec.last_word1 = w1;
        }
    }
    rec.frame_cursor++;
    return 1;
}

// 0x1000a740
int WriteClose(void) {
    rec.last_frame_index = rec.frame_cursor - 1;
    return 1;
}

// 0x1000a760
int ReadOpen(int track_idx /*unused at this layer*/) {
    rec.frame_cursor = 0;
    rec.read_index = 0;
    return 1;
}

// 0x1000a780
int Read(uint32_t out[2]) {
    if (rec.frame_cursor == 0) {
        rec.replay_word0 = rec.entries[0].value;   // initial-state baseline
        rec.replay_word1 = rec.entries[1].value;
        rec.read_index = 2;
    } else {
        if (rec.read_index >= (uint32_t)rec.entry_count) {
            out[0] = 0; out[1] = 0;
            return 0;                              // recording exhausted
        }
        uint32_t fc = rec.entries[rec.read_index].frame_and_channel;
        if (rec.frame_cursor == fc) {              // channel-0 entry (flag=0)
            rec.replay_word0 = rec.entries[rec.read_index++].value;
        }
        // Re-fetch in case we just advanced
        fc = rec.entries[rec.read_index].frame_and_channel;
        if ((fc & 0x8000) && rec.frame_cursor == (fc & 0x7FFF)) {
            rec.replay_word1 = rec.entries[rec.read_index++].value;
        }
    }
    out[0] = rec.replay_word0;
    out[1] = rec.replay_word1;
    rec.frame_cursor++;
    return 1;
}

// 0x1000d370 — shares VA with DXSound::LoadComplete
int ReadClose(void) { return 1; }
```

### Sequencing requirements from caller

- Once per race-start: `WriteOpen(track)` OR `ReadOpen(track)` (mutually exclusive on `g_inputPlaybackActive`).
- Once per simulation sub-tick from `PollRaceSessionInput`: `Write(&controls, 1)` OR `Read(&controls)`.
- Once at race-end fade-complete: `WriteClose()` OR `ReadClose()`.
- ONE extra `Write(&{0x40000000}, 0)` from the audio-options pause overlay's "Quit Race" path (strip_mode=0 — the escape bit must survive into the replay so it terminates at the same frame).

### Cap behavior

Records past the 19996-entry / 32750-frame limit are silently dropped (the
`if (entry_count < 0x4E1C && frame_cursor < 0x7FEE)` gate). The frame
cursor still advances so the recording's effective length matches real
time; on playback past those caps, Read returns 0 (exhausted) and outputs
zeros forever.

---

## Appendix — Ghidra addresses (M2DX.dll @ image base 0x10000000)

```
WriteOpen       0x1000a640   8 bytes
Write           0x1000a660  211 bytes
WriteClose      0x1000a740   8 bytes
ReadOpen        0x1000a760  17 bytes
Read            0x1000a780  168 bytes
ReadClose       0x1000d370   2 bytes (no-op stub, shares VA with DXSound::LoadComplete)
```

## Appendix — TD5_d3d.exe call sites

```
InitializeRaceSession  @ 0x0042aa10
  → ReadOpen  call at 0x0042b28d (via PTR_ReadOpen_0045d450)
  → WriteOpen call at 0x0042b29c (via PTR_WriteOpen_0045d454)

PollRaceSessionInput   @ 0x0042c470
  → Read  call at 0x0042c487 (via PTR_Read_0045d420)
  → Write call at 0x0042c6ee (via PTR_Write_0045d438)  — per-tick recording

RunRaceFrame           @ 0x0042b580
  → ReadClose  call at 0x0042b856 (via PTR_ReadClose_0045d448)
  → WriteClose call at 0x0042b85e (via PTR_WriteClose_0045d44c)

RunAudioOptionsOverlay @ 0x0043bf70
  → Write call at 0x0043c33f (via PTR_Write_0045d438) — Quit Race injects 0x40000000
```

## Appendix — Frontend trigger

```
RunRaceResultsScreen @ 0x00422480, case 0xF (button-press dispatch),
  button index 1 ("View Replay"):
    writes g_inputPlaybackActive = 1
    sets g_frontendBootDispatchMode = 2
    advances g_frontendInnerState
  → case 0x10 then calls InitializeFrontendDisplayModeState()
  → next frame re-enters race init with g_inputPlaybackActive=1
  → ReadOpen / Read / ReadClose branch is taken
```
