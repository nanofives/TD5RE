# TD5RE Expected Behavior Map

Reference: original TD5_d3d.exe behavior vs source port expected behavior.
Use this to validate each subsystem during testing. (Last full refresh: 2026-06-09.)

## Recent changes (2026-06)

Port-side work that changed behavior since the original version of this map:

- **FPS decoupling + working VSync** — the `[Display] VSync` key actually gates
  Present now; sky/fog/billboard/marker advances moved into the fixed 30Hz sim
  tick, frontend animation counters pace at 60Hz regardless of render FPS.
- **Camera rewrite (FPS-independent)** — every in-race camera mode solves on the
  30Hz tick and interpolates per frame (`td5_camera_solve_tick_all` /
  `td5_camera_apply_view`); kill-switch env `TD5RE_CAM_NEW=0` selects the legacy
  per-frame path until the rewrite is fully soak-tested.
- **N-way split-screen** — up to 9 local humans (Multiplayer Options grid picker),
  plus dev-only AI spectator panes (`[Game] SpectateScreens`). Pane rects come
  from one shared function (`td5_game_get_pane_rect`) for viewports/HUD/dividers.
- **Simultaneous MP car select** — all joined players pick car/paint/transmission
  at once in a split grid, each on their own controller.
- **Car-select stat bars** — SPEED/ACCEL/GRIP relative bars + MORE STATS button
  on both SP and MP car selection.
- **Save system** — binary `Config.td5`/`CupData.td5` retired; settings live in
  `td5re.ini` + `td5re_input/progress/cup.ini` (legacy files imported once, then
  renamed `*.migrated`).
- **Netplay** — Winsock2 UDP lockstep + UPnP port mapping; LAN lobby browse is
  non-blocking; ESC works on every net screen.
- **Cleanup 2026-06-09** — the RE parity-trace scaffolding (30 pilot-trace
  modules, whole-state/state-replay harnesses) was deleted; `[Trace] RaceTrace`
  CSVs remain as the supported diagnostic. The pause overlay rebuilds from the
  live screen centre every frame (no baked layout state).

## Startup Sequence

| Step | Original | Expected in Port | Log Tag |
|------|----------|-----------------|---------|
| 1. Window creation | 640x480 DirectDraw window | D3D12 window (windowed or fullscreen) | platform |
| 2. Module init | 15 modules initialized in order | Same order: asset,save,input,sound,render,track,physics,ai,camera,vfx,hud,frontend,net,fmv,game | td5re |
| 3. Config load | Config.td5 loaded, XOR decrypted, CRC validated | INI files (td5re.ini + td5re_input/progress/cup.ini); legacy Config.td5/CupData.td5 imported once then renamed *.migrated | save |
| 4. Frontend init | Load font atlas, cursor, button textures from frontend.zip | Same | frontend |
| 5. Intro movie | Play intro.mp4/intro.avi if present | MFPlay MP4, skip on keypress | fmv |
| 6. Legal screens | Display legal1.tga + legal2.tga with fade | Same, 500ms fade, 5s hold | fmv |

## Frontend Menu Flow

| Screen | Original Behavior | Expected | Log Evidence |
|--------|-------------------|----------|-------------|
| Startup Init [28] | One-shot: load config, enumerate devices, set defaults | Same | frontend: "StartupInit" |
| Localization [0] | Detect language from regional INI | Same, default English | frontend: "LocalizationInit" |
| Main Menu [5] | 7 items, highlight cycling, background image | Same | frontend: screen=5 |
| Quick Race [7] | Car/track selection launchers | **Port-enhanced**: Car/Track/Direction/Players/Opponents selectors, no Drag Strip (see QUICKRACE_PLAYER_SETUP.md) | frontend: screen=7 |
| Car Selection [20] | 3D preview, spin animation, lock/unlock, stats | **Port-enhanced**: + SPEED/ACCEL/GRIP stat bars, MORE STATS spec sheet; MP flow uses a simultaneous split-grid picker | frontend: screen=20 |
| Track Selection [21] | Preview images, lock status | Same | frontend: screen=21 |
| Options Hub [12] | 5 sub-categories | Same | frontend: screen=12 |
| Display Options [16] | Resolution list, fog, speed units | Enumerated modes, apply on race start | frontend: screen=16 |

## Race Session Init

| Step | Original (0x42AA10) | Expected | Log Evidence |
|------|---------------------|----------|-------------|
| 1. Loading screen | Random TGA from LOADING.ZIP | Same | td5_game: "Step 1" |
| 2. Heap reset | Reset game heap | Same | td5_game: "Step 2" |
| 3. Config apply | Apply selected display mode | Same | td5_game: "Step 3" |
| 4-6. Asset loading | Level ZIP, strip data, routes, textures | Same | asset: "Loading..." |
| 7-8. Vehicle loading | himodel.dat + carparam.dat per slot | Same | asset: "Vehicle loaded" |
| 9. Actor spawn | 6 racers on grid + 6 traffic | `humans+opponents` racers (default 6; Quick Race configurable, see QUICKRACE_PLAYER_SETUP.md) + 6 traffic | td5_game: "spawning N racers" |
| 10. Physics init | Per-actor physics state, gravity, tuning | Same | physics: "init_vehicle_runtime" |
| 11. AI init | Route data, rubber band params, tier config | Same | ai: "init_race_actor" |
| 12. Input recording | Open replay.td5 for write | Same | input: "write_open" |
| 13. Sound loading | Ambient WAVs, vehicle engine banks | Same | sound: "Loading WAV" |
| 14. HUD init | Overlay resources, layout, pause menu | Same | hud: "init_overlay" |
| 15. Countdown | 3-2-1-GO, actors paused | Same | td5_game: "countdown" |

## In-Race Frame Loop

| System | Original (0x42B580) | Expected | Log Evidence |
|--------|---------------------|----------|-------------|
| Frame timing | Fixed timestep, max 4 ticks/frame | Same 30Hz sim; render/present decoupled (uncapped or VSync'd FPS) | td5_game: "dt=X ticks=N" |
| Input polling | Keyboard + joystick per player | Same | input: "control_bits" |
| Camera | Chase cam (7 presets), trackside for replay | Same modes; per-tick solve + per-frame interpolation (FPS-independent) | camera: "chase/trackside/bumper" |
| Physics tick | Iterate all actors, update dynamics | Same | physics: "tick N actors" |
| Collision V2V | AABB broadphase + 7-iter TOI | Same | physics: "collision pair" |
| Collision V2W | Track edge wall impulse | Simplified edge check | physics: "wall impulse" |
| AI tick | Rubber band + steering + script VM + traffic | Same | ai: "rubber_band" |
| Track update | Position tracking, checkpoint detection | Same | track: "checkpoint" |
| VFX tick | Tire tracks, billboard anims | Same | vfx: "active emitters" |
| Race order | Bubble sort by span position | Same | td5_game: "race_order" |
| Render | Sky + track spans + actors + translucent + HUD | Same pipeline | render: "frame stats" |
| Sound mix | Engine pitch/volume, Doppler, distance atten | Same | sound: "mix" |

## Race End / Results

| Event | Original | Expected | Log Evidence |
|-------|----------|----------|-------------|
| Finish detection | 4-bit sector bitmask, lap counting | Same | td5_game: "check_race_completion" |
| Cooldown | 0x3FFFFF accumulator | Same | td5_game: "pending_finish" |
| Fade out | Directional fade based on viewport | Same | td5_game: "begin_fade_out" |
| Results table | Sort by time/score, award position points | Same | td5_game: "build_results" |
| Results screen | Position display, cup standings | Same | frontend: screen=24 |
| High score | Name entry if qualifying | Same | frontend: screen=25 |

## Pause Menu

| Action | Original (0x43BF70) | Expected |
|--------|---------------------|----------|
| ESC pressed | Pause, show overlay | Same (overlay quads rebuilt per frame from the live screen centre) |
| Up/Down | Navigate 5 items (View/Music/SFX/Continue/Exit) | 6 rows: VIEW + SOUND sliders, CONTINUE, RESTART RACE, QUIT TO MENU, EXIT GAME (S15 rework) |
| Continue | Resume race, restore DXInput | Resume, unpause |
| Exit | Retire all actors, fade out | Fade out (QUIT TO MENU) or quit app (EXIT GAME) |

## Expected Audio

| Sound | When | Source |
|-------|------|--------|
| Engine drive loop | Per-vehicle, pitch from RPM | WAV from vehicle bank |
| Engine rev loop | Idle/neutral | WAV from vehicle bank |
| Skid | Tire slip > threshold | Ambient bank |
| Horn | Button press | Vehicle bank |
| Collision impact | V2V contact | One-shot positional |
| Music | CD audio or stream | MCI or stream playback |
| Frontend SFX | Menu navigation/confirm | 10 frontend WAVs |

## Known Differences from Original

| Area | Original | Port |
|------|----------|------|
| Graphics API | DirectDraw/Direct3D retained mode | D3D12 via wrapper |
| Video codec | EA TGQ | MFPlay (MP4/AVI) |
| Network | DirectPlay | Winsock2 UDP |
| Resolution | 640x480x16 default | 640x480x32 (windowed) |
| Cross-fade | MMX SIMD 16-bit blend | Software RGBA32 blend |
| Snow weather | Cut feature (buffers allocated, rendering gated) | Same (faithful cut) |
| `replay.td5` | In-memory only — M2DX DXInput's filename param is dead scaffolding (5 methods @ 0x1000A640..0x1000A780 have zero file-I/O callees) | Faithful by default. Optional disk persistence via `[Replay] PersistToDisk=1` (default `0`) — port-only feature flag |
| CupData.td5 actor pointers | 14 actors saved as flat memcpy from `0x4AB108` (Serialize @ 0x411120); raw `.data` pointers persist at actor+0x1B0 (himodel via `DAT_004C3D10`), +0x1B8 (tuning via `gVehicleTuningTable @ 0x4AE580`), +0x1BC (physics via `gVehiclePhysicsTable @ 0x4AE280`). Reloading after a binary rebuild that relocates `.data` leaves these pointers dangling. | Extended file format (12998 B) appends a 32-byte overlay at offset `0x32A6` (magic `"TD5RE_v1"` + 6 racer-slot car indices). Loader pushes IDs into `g_td5.car_index` / `g_td5.ai_car_indices[1..5]` so race-init's `td5_asset_load_vehicle()` re-resolves all three pointers from the asset registry. Defensive: actor+0x1B0/+0x1B8/+0x1BC are zeroed in `s_actor_table` for slots 0..5 after restore so any premature read crashes cleanly instead of dereferencing a stale address. Legacy 12966-byte files (no overlay) still load: CRC is validated against the actual buffer size, and the overlay branch is skipped. Self-test: `td5re.exe --TestCupRoundtrip=1` (returns 0 on PASS, 2 on FAIL). |

## Optional: replay.td5 disk persistence (port-only)

When `[Replay] PersistToDisk=1` (or `--PersistToDisk=1`) is set, `td5_input_write_close()` flushes the recorded input ring to `replay.td5` next to the executable on race end, and `td5_input_read_open()` reads it back when entering replay mode. With the flag at its default `0`, replays remain memory-only — bit-for-bit faithful to the original M2DX behavior.

File format (little-endian, total `24 + entry_count*8` bytes):

```
offset  size   field
0       8      magic            "TD5RPLY\0"
8       4      version          1
12      4      track_index
16      4      entry_count      0..19996
20      4      last_frame_index 0..0x7FEE
24      N*8    entries[]        TD5_InputRecordEntry { frame_and_channel u32, value u32 }
```

Loader rejects the file on bad magic, version mismatch, or out-of-range counts; on track-index mismatch it logs a WARN but loads anyway (caller must arrange a matching scenario for deterministic playback).

## Intentional Divergences

Ports of original behaviour that were deliberately changed because the literal
asm produced an emergent gameplay bug. Each entry documents the original rule,
the port rule, and the acceptance criteria.

### Wall response — soft clamp REVERTED (no longer a divergence)

- **File**: `td5_physics.c` (`td5_physics_wall_response`, tangential-damping
  branches — see the dated comment block in the function).
- **History**: an earlier port version replaced the original's hard zero on
  the `v_para <= 0` sign-flip with a magnitude-only soft clamp (theory:
  hard-zero killed wall-friction reorient on glancing hits). **Reverted
  2026-05-10** — a Frida long-pass showed the soft clamp made AI cars carry
  a sign-flipped tangential velocity after wall hits, roll back, get hit
  again, and stall permanently (port AI stuck at span ~430 while original
  AI cruised to span 1373 in the same geometry). A Ghidra re-read of
  `0x00406B65-69` confirmed the original hard-zeros BOTH branches.
- **Current rule**: byte-faithful — both branches hard-zero on sign-flip,
  with the original's round-toward-zero `>> 6` idiom preserved.

### Quick Race configurable Players + Opponents (and `DefaultOpponents` debug knob)

- **Files**: `td5_frontend.c` (`Screen_QuickRaceMenu`, `frontend_init_race_schedule`),
  `td5_game.c` (`InitRace`), `td5_ai.c` (`td5_ai_init_race_actor_runtime`).
- **Original**: Quick Race `0x4213D0` is Car / Track / OK / Back only and always
  runs a fixed 6-car field (slot 0 + 5 AI).
- **Port rule**: the Quick Race screen adds **Players** and **Opponents**
  selectors (humans + AI ≤ 16 slots); a Forwards/Backwards direction toggle; and
  drops the Drag Strip from the track cycler. Dropped opponents are not spawned,
  not AI-driven, and not rendered. Up to 9 local humans each get their own
  viewport pane (N-way split, Multiplayer Options grid picker). Default `1+5=6`
  is byte-identical to the original grid.
- **Debug knob**: `[Game] DefaultOpponents=N` / `--DefaultOpponents=N` (`-1` =
  full grid) forces the AI-opponent count for AutoRace without the menu — a dev
  tool for isolating one AI or building minimal repros. Release builds force `-1`.
- **Full reference + the two-slot-state-table gotcha**: see
  `QUICKRACE_PLAYER_SETUP.md` (same dir).

## RT lighting (LOW vs HIGH) + LIGHTING OPTIONS screen [RT2 P8]

- **`[Lighting] Quality` = LOW (0)** keeps the classic screen-space stack
  (byte-identical to pre-RT). **HIGH (1, default when a DXR device is present)**
  runs the ray-traced shadow / GI / reflection / light passes. On a non-DXR
  machine HIGH auto-falls-back to LOW (`td5_rt_active()` gate) — no INI rewrite.
- **LIGHTING OPTIONS** (GRAPHICS OPTIONS → "LIGHTING OPTIONS →", screen 51):
  per-feature tiers, all defaulting to the **highest** — SHADOW QUALITY
  (1/4/8 rays), SHADOW DETAIL (HALF/FULL), REFLECTIONS (OFF/HALF/FULL),
  REFLECTION RANGE (NEAR/FAR/UNLIMITED), GLOBAL ILLUMINATION (OFF/LOW/HIGH),
  SUN & SKY (CLASSIC/AUTO), LIGHTS (BASIC/REALISTIC). Rows 1–7 are HIGH-only and
  appear greyed/inert at LOW; row 0 (LIGHTING QUALITY) is always live when DXR
  exists. All persist to `[Lighting]` and map onto the `TD5RE_RT_*` env knobs
  (an explicitly-set env still wins, for A/B). Most tier changes take effect on
  the next race (the RT passes cache their env read); LIGHTING QUALITY applies
  live. Residuals (see docs/plans/RT_LIGHTING2_PLAN.md P8 as-built): the
  half-res dispatch (SHADOW DETAIL HALF / REFLECTIONS HALF) currently renders
  FULL, and SUN & SKY CLASSIC only drops the sun disc (full zone-sun override
  pending).

## CHAOS CO-OP (MP mode 5) — one car per team, many drivers

PORT-ONLY, no original counterpart. 4 / 6 / 8 **local** humans, two teams, one
car each. Every human ("seat") holds ONE control axis of their team's car.
Design: `docs/plans/CHAOS_COOP_MODE_PLAN.md`. Modules: `td5_chaos.c` (seat
table, roles, rotation), `td5_chaos_fold.c` (the input fold), `td5_fe_chaos.c`
(the CHAOS TEAMS board). Everything below is **intended**, not a bug.

- **Opposed inputs cancel.** Contributions to an axis are summed, then clamped
  to `-256..+256`. In a team of 4 the LEFT owner pushing left and the RIGHT
  owner pushing right sum to zero and the car goes straight. Same for THROTTLE
  against BRAKE. Two people fighting the wheel producing nothing is the mode
  working, and the car only moves at all if the throttle owner presses.
  A seat pushing the direction it does not own contributes nothing: LEFT
  discards a negative steer value, RIGHT discards a positive one, THROTTLE
  discards braking, BRAKE discards throttle.
- **Each axis is re-encoded DIGITAL or ANALOG independently** (`td5_chaos_fold.c`,
  decision D1). An axis goes out digital when **every** seat contributing to it
  was digital, so an all-keyboard team still gets the digital steering ramp
  (`s_steer_ramp[]`, Path A in `td5_input.c`) instead of instant full lock. As
  soon as one contributor is a pad, that axis goes out as the packed analog axis
  (Path B) and any digital contributor on it counts as a full 256. The X and Y
  flags are separate bits, so a team can legitimately run analog steering with a
  digital pedal axis, or the reverse. Consequence worth knowing: mixing a pad
  and a keyboard on the SAME axis makes the keyboard seat feel like a full-lock
  input rather than a ramped one.
  The fold cannot be a bitwise OR of the seat words: a bound pad makes the
  platform **replace** the word after collapsing both steering directions into
  one packed axis, so a seat owning only LEFT carries no `STEER_LEFT` bit at
  all. The fold therefore works in the axis domain.
- **The BRAKE (or PEDALS) owner can reverse the car.** The gearbox is forced to
  AUTO in this mode (below), and the automatic FSM force-selects REVERSE on a
  negative signed throttle term ([CONFIRMED @ 0x0042EF1F], in
  `td5_physics_drivetrain.c`). Holding brake at a standstill with nobody on the throttle
  backs the car up. This is not a special case bolted on for the mode, it is the
  stock automatic behaviour reached through a split pedal axis, and it is the
  only way a chaos team can undo a wall.
- **Gearbox is forced to AUTO.** No seat owns the gearbox: `GEAR_UP` /
  `GEAR_DOWN` are never folded in, and the post-processing pass clears the
  manual bit last, so the chaos forcing outranks drag mode, the car-select
  manual pick and `TD5RE_FORCE_MANUAL`. A fifth role would starve the size-2
  team, which has only two seats to cover steering and both pedals.
- **RECOVER is team-shared**, along with horn, camera change and rear view: any
  seat on the team fires them. Handbrake is NOT shared, it belongs to whoever
  owns the brake half of the pedal axis (the BRAKE owner at size 4, the PEDALS
  owner at sizes 2 and 3). Pause / Escape is shared across BOTH teams, so the
  two-pane race pauses as one. RECOVER is team-shared deliberately even though
  the plan's owner table does not list it: a chaos car whose recovery sat on one
  rotating seat could stay wedged.
- **Each team rotates on its OWN milestones**, so the two cars generally swap
  roles at different moments. Roles shift down one row per rotation and the
  bottom row wraps to row 1. A milestone arms a 3 s countdown (90 sim ticks) and
  the swap lands when it reaches zero. A second milestone reached while a
  countdown is already running does not restart it. Over a full race both cars
  cross the same milestones, so the rotation counts end up equal even though the
  timing does not. Milestones are polled on the 30 Hz **sim** clock, never at
  the checkpoint-crossing site, whose caller runs once per rendered frame.
- **On a circuit, CHECKPOINT means a quarter-lap arc.** Circuits in this engine
  have no checkpoints at all (the lap/sector path is a separate branch in
  `advance_pending_finish_state`), so the trigger is generalised: on a
  point-to-point track CHECKPOINT is a real checkpoint crossing (4 shipped, 5
  counting the finish, 4 or 2 on an AUTO track), and on a circuit it is one of 4
  equal arcs of the ring, quantized **after** `td5_track_branch_to_main_span()`.
  That remap is mandatory on AUTO tracks: a raw fork or corridor span aliases
  onto an unrelated low main-ring span and would fire a spurious rotation every
  time a car took a branch. HALF LAP is every other checkpoint / 2 arcs, LAP is
  the lap crossing (so on a point-to-point track LAP effectively never fires),
  TIME is every N seconds. A drag strip has one checkpoint and no lap, so every
  trigger except OFF falls back to TIME there.
- **Local only.** The mode is not offered when `network_active`, and the vote row
  is greyed at any head count that is not 4, 6 or 8 or when AI players are in
  the lobby. The seat table lives in the module-owned `TD5_ChaosConfig`, not in
  the replicated `TD5_MpModeConfig`, so the mode does not touch the net protocol
  version (`TD5_NetRaceConfig` is size-asserted at 484 bytes). Replay records
  slots 0 and 1 only, which happen to be exactly the two car slots, so the
  **car** replays correctly but the per-seat attribution is lost.
- **Seat colours are one table, shared by the board and the HUD**:
  `td5_chaos_seat_color(seat)`, 8 distinct hues carrying the literal
  `k_mp_player_colors[0..7]` values. The per-profile accent override the other MP
  screens apply is deliberately NOT used here, because `s_mp_player_accent` is
  indexed by local setup slot (of which this mode has two) and the HUD cannot
  read it at all.
- **Dev/CI entry:** the mode needs one controller per seat, so without a pad wall
  the only way in is `TD5RE_CHAOS_FAKE_SEATS=4|6|8` (dev builds), with
  `TD5RE_CHAOS_TRIGGER=0..4` and `TD5RE_CHAOS_PERIOD=10..60`. It fabricates the
  seat table at race init, all seats on device 0, and collapses the race to 2
  cars / 2 panes / 0 AI opponents. `--StartScreen=54` reaches the CHAOS TEAMS
  board and seeds a dev fake roster of 4 for layout framedumps. Neither path
  exercises per-seat input: that is a manual check with real pads.
