# TD5RE Expected Behavior Map

Reference: original TD5_d3d.exe behavior vs source port expected behavior.
Use this to validate each subsystem during testing.

## Startup Sequence

| Step | Original | Expected in Port | Log Tag |
|------|----------|-----------------|---------|
| 1. Window creation | 640x480 DirectDraw window | D3D11 window (windowed or fullscreen) | platform |
| 2. Module init | 15 modules initialized in order | Same order: asset,save,input,sound,render,track,physics,ai,camera,vfx,hud,frontend,net,fmv,game | td5re |
| 3. Config load | Config.td5 loaded, XOR decrypted, CRC validated | Same | save |
| 4. Frontend init | Load font atlas, cursor, button textures from frontend.zip | Same | frontend |
| 5. Intro movie | Play intro.mp4/intro.avi if present | MFPlay MP4, skip on keypress | fmv |
| 6. Legal screens | Display legal1.tga + legal2.tga with fade | Same, 500ms fade, 5s hold | fmv |

## Frontend Menu Flow

| Screen | Original Behavior | Expected | Log Evidence |
|--------|-------------------|----------|-------------|
| Startup Init [28] | One-shot: load config, enumerate devices, set defaults | Same | frontend: "StartupInit" |
| Localization [0] | Detect language from regional INI | Same, default English | frontend: "LocalizationInit" |
| Main Menu [5] | 7 items, highlight cycling, background image | Same | frontend: screen=5 |
| Quick Race [7] | Car/track selection launchers | Same | frontend: screen=7 |
| Car Selection [20] | 3D preview, spin animation, lock/unlock, stats | Same | frontend: screen=20 |
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
| 9. Actor spawn | 6 racers on grid + 6 traffic | 6 racers (staggered spans) + 6 traffic | td5_game: "Spawning actor" |
| 10. Physics init | Per-actor physics state, gravity, tuning | Same | physics: "init_vehicle_runtime" |
| 11. AI init | Route data, rubber band params, tier config | Same | ai: "init_race_actor" |
| 12. Input recording | Open replay.td5 for write | Same | input: "write_open" |
| 13. Sound loading | Ambient WAVs, vehicle engine banks | Same | sound: "Loading WAV" |
| 14. HUD init | Overlay resources, layout, pause menu | Same | hud: "init_overlay" |
| 15. Countdown | 3-2-1-GO, actors paused | Same | td5_game: "countdown" |

## In-Race Frame Loop

| System | Original (0x42B580) | Expected | Log Evidence |
|--------|---------------------|----------|-------------|
| Frame timing | Fixed timestep, max 4 ticks/frame | Same | td5_game: "dt=X ticks=N" |
| Input polling | Keyboard + joystick per player | Same | input: "control_bits" |
| Camera | Chase cam (7 presets), trackside for replay | Same, routes to real actor data | camera: "chase/trackside/bumper" |
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
| ESC pressed | Pause, show overlay | Same |
| Up/Down | Navigate 5 items (View/Music/SFX/Continue/Exit) | 3 items (Resume/Options/Quit) |
| Continue | Resume race, restore DXInput | Resume, unpause |
| Exit | Retire all actors, fade out | Fade out |

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
| Graphics API | DirectDraw/Direct3D retained mode | D3D11 via wrapper |
| Video codec | EA TGQ | MFPlay (MP4/AVI) |
| Network | DirectPlay | Winsock2 UDP |
| Resolution | 640x480x16 default | 640x480x32 (windowed) |
| Cross-fade | MMX SIMD 16-bit blend | Software RGBA32 blend |
| Snow weather | Cut feature (buffers allocated, rendering gated) | Same (faithful cut) |
