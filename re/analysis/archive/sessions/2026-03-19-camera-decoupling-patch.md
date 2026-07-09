# Camera Decoupling from Physics Tick Loop

Date: 2026-03-19
Target: TD5_d3d.exe `RunRaceFrame` (0x42B580)

## Problem

`UpdateChaseCamera` (0x401590) runs inside the fixed-timestep physics loop:
```
while (g_simTimeAccumulator > 0xFFFF) {
    ... physics ...
    UpdateChaseCamera(...);  // <-- tied to physics tick rate
    ... particles, timer ...
    g_simTimeAccumulator -= 0x10000;
}
```
This means camera updates are locked to the 30Hz physics rate, causing visible
stutter when rendering at higher frame rates (60fps, 120fps, etc).

## Physics Loop Boundaries

- **Loop guard**: VA `0x42B8F5` -- `CMP [g_simTimeAccumulator], 0x10000; JC after_loop`
- **Loop body**: VA `0x42B907` to `0x42BAF0` (JNC back to 0x42B907)
- **After loop**: VA `0x42BAF6` -- `MOVSX ESI, byte [gRaceViewportLayoutMode]`

## Camera Calls Inside the Loop

| VA | Call | Move? | Rationale |
|----|------|-------|-----------|
| 0x42B938 | `CacheVehicleCameraAngles(primary, 0)` | No | Pure snapshot, harmless, feeds post-loop cameras |
| 0x42B95A | `CacheVehicleCameraAngles(secondary, 1)` | No | Same as above |
| 0x42B9F4 | `UpdateChaseCamera(actor, 1, view)` | **YES** | Contains damped integrators; move to once-per-frame |
| 0x42BA66 | `UpdateRaceCameraTransitionTimer()` | No | Timer must advance at physics rate (counts down 0x100/tick) |

### UpdateChaseCamera Viewport Loop (inside physics loop)

```asm
0042B9B3: MOVSX EAX, byte [gRaceViewportLayoutMode]
0042B9BA: SHL EAX, 0x6
0042B9BD: MOV ECX, [EAX + viewport_count_table]  ; viewport count
0042B9C3: XOR ESI, ESI                            ; i = 0
0042B9C5: TEST ECX, ECX
0042B9C7: JLE 0x42BA11                            ; skip if no viewports

loop:
0042B9C9: MOV EAX, [ESI*4 + gRaceCameraPresetMode] ; preset for this view
0042B9D0: TEST EAX, EAX
0042B9D2: JNZ 0x42B9FC                            ; skip if preset != 0 (non-chase mode)
          ; ... compute actor slot ptr ...
0042B9E7: PUSH ESI                                 ; param_3 = viewport index
0042B9F1: PUSH 0x1                                 ; param_2 = 1 (track heading)
0042B9F3: PUSH EDX                                 ; param_1 = actor slot ptr
0042B9F4: CALL UpdateChaseCamera                   ; <-- THE CALL
0042B9F9: ADD ESP, 0xC

0042B9FC: ; loop increment
          INC ESI
          CMP ESI, ECX
0042BA0F: JL loop
```

## UpdateChaseCamera Integrator Analysis

`UpdateChaseCamera` contains these cumulative state variables:
- `DAT_00482ef0[view]`: fly-in counter (increments to threshold)
- `DAT_00482f18[view]`: orbit angle (weighted accumulation tracking vehicle heading)
- `DAT_00482fa0[view]`: height smoothing (accumulates)
- `DAT_00482f00[view]`: radius (damped spring)
- `DAT_00482eb0[view]`: smoothed value (damped)

At 30fps (1 tick/frame), moving outside loop gives **identical** behavior.
At 60fps (2 ticks/frame), camera is slightly more responsive (less damping).
At 120fps, noticeably less damped but still functional.

## Other Camera Functions (NOT in physics loop)

These already run once per frame in the render pass:
- `UpdateRaceCameraTransitionState` (VA 0x42BCDB / 0x42BF15)
- `UpdateTracksideOrbitCamera` (VA 0x42BD4C / 0x42BFC0)
- `UpdateVehicleRelativeCamera` (VA 0x42BD20 / 0x42BF99)
- `SelectTracksideCameraProfile` + `UpdateTracksideCamera` (VA 0x42BD72+0x42BD93 / 0x42BF42+0x42BF63)
- `SetCameraWorldPosition`, `BuildCameraBasisFromAngles`, etc.

No changes needed for these.

## Patch Plan (3 patches)

### Patch 1: Disable UpdateChaseCamera inside physics loop
- **File offset**: `0x2B9D2` (VA `0x42B9D2`)
- **Change**: `0x75` (JNZ) -> `0xEB` (JMP short)
- **Size**: 1 byte
- **Effect**: The viewport loop's `if (preset == 0)` check becomes unconditional skip.
  The dead code (address calc + pushes + CALL + cleanup) never executes.

### Patch 2: JMP trampoline after physics loop
- **File offset**: `0x2BAF6` (VA `0x42BAF6`)
- **Original**: `0F BE 35 89 AF 4A 00` (MOVSX ESI, byte [0x4aaf89])
- **Patched**: `E9 A5 08 03 00 90 90` (JMP 0x45C3A0; NOP; NOP)
- **Size**: 7 bytes (5-byte JMP + 2 NOP)
- **Effect**: Redirects to code cave before continuing post-loop code.

### Patch 3: Code cave -- UpdateChaseCamera viewport loop
- **File offset**: `0x5C3A0` (VA `0x45C3A0`)
- **Size**: 106 bytes (in zero-filled .rdata padding)
- **Content**: Exact replica of the original viewport loop from the physics body,
  followed by the displaced MOVSX instruction, then JMP back to `0x42BAFD`.

```
0x45C3A0: 0F BE 05 89 AF 4A 00   MOVSX EAX, byte [gRaceViewportLayoutMode]
0x45C3A7: C1 E0 06               SHL EAX, 0x06
0x45C3AA: 8B 88 48 AE 4A 00      MOV ECX, [EAX + viewport_count]
0x45C3B0: 33 F6                  XOR ESI, ESI
0x45C3B2: 85 C9                  TEST ECX, ECX
0x45C3B4: 7E 48                  JLE done
loop:
0x45C3B6: 8B 04 B5 D8 2F 48 00  MOV EAX, [ESI*4 + gRaceCameraPresetMode]
0x45C3BD: 85 C0                  TEST EAX, EAX
0x45C3BF: 75 28                  JNZ skip
0x45C3C1: 8B 04 B5 A0 6E 46 00  MOV EAX, [ESI*4 + gPrimarySelectedSlot]
0x45C3C8: 8D 0C C5 00 00 00 00  LEA ECX, [EAX*8]
0x45C3CF: 2B C8                  SUB ECX, EAX
0x45C3D1: C1 E1 04               SHL ECX, 0x04
0x45C3D4: 56                     PUSH ESI
0x45C3D5: 03 C8                  ADD ECX, EAX
0x45C3D7: 8D 14 CD 08 B1 4A 00  LEA EDX, [ECX*8 + gActorTable]
0x45C3DE: 6A 01                  PUSH 0x1
0x45C3E0: 52                     PUSH EDX
0x45C3E1: E8 AA 51 FA FF         CALL UpdateChaseCamera
0x45C3E6: 83 C4 0C               ADD ESP, 0x0C
skip:
0x45C3E9: 0F BE 05 89 AF 4A 00  MOVSX EAX, byte [gRaceViewportLayoutMode]
0x45C3F0: C1 E0 06               SHL EAX, 0x06
0x45C3F3: 8B 88 48 AE 4A 00     MOV ECX, [EAX + viewport_count]
0x45C3F9: 46                     INC ESI
0x45C3FA: 3B F1                  CMP ESI, ECX
0x45C3FC: 7C B8                  JL loop
done:
0x45C3FE: 0F BE 35 89 AF 4A 00  MOVSX ESI, byte [gRaceViewportLayoutMode]  ; displaced
0x45C405: E9 F3 F6 FC FF         JMP 0x42BAFD  ; return
```

### Raw bytes for Patch 3:
```
0F BE 05 89 AF 4A 00 C1 E0 06 8B 88 48 AE 4A 00
33 F6 85 C9 7E 48 8B 04 B5 D8 2F 48 00 85 C0 75
28 8B 04 B5 A0 6E 46 00 8D 0C C5 00 00 00 00 2B
C8 C1 E1 04 56 03 C8 8D 14 CD 08 B1 4A 00 6A 01
52 E8 AA 51 FA FF 83 C4 0C 0F BE 05 89 AF 4A 00
C1 E0 06 8B 88 48 AE 4A 00 46 3B F1 7C B8 0F BE
35 89 AF 4A 00 E9 F3 F6 FC FF
```

## Edge Cases

### Audio Options Overlay
When `g_audioOptionsOverlayActive != 0`, the physics loop is skipped entirely
(JNZ at VA 0x42B8EF jumps to 0x42BAF6). Our trampoline at 0x42BAF6 will still
execute, calling UpdateChaseCamera even though the overlay is active. This is
acceptable: the camera integrators will converge with no physics changes, producing
a smooth idle. The original code skipped the camera entirely during overlay.

### Split-Screen
The viewport loop handles both viewports (ESI=0 and ESI=1) when
`gRaceViewportLayoutMode != 0`. The code cave replicates this exactly.

### Replay Mode
In replay mode, `gRaceCameraPresetMode[view] != 0`, so UpdateChaseCamera is skipped
by the `JNZ skip` check. The replay camera (`UpdateTracksideCamera`) already runs
outside the loop. No impact.

## Code Cave Layout (updated)

| VA Range | Size | Use |
|----------|------|-----|
| 0x45C1A1 | 62B | dead |
| 0x45C1F5 | 52B | COLORFILL cave |
| 0x45C229 | ~39B | BltFast trampoline (zeroed) |
| 0x45C250 | 32B | Blt data |
| 0x45C270 | 56B | Blt code |
| 0x45C2B0 | 70B | dead |
| 0x45C2F6 | 25B | car sprite |
| 0x45C310 | 16B | srcRect data |
| 0x45C320 | 16B | destRect data |
| 0x45C330 | 83B | scale-to-fullscreen code |
| **0x45C3A0** | **106B** | **Camera decoupling cave (NEW)** |
| 0x45C40A+ | ~3062B | free |

## Future Improvements

1. **Interpolation**: Save previous-tick camera state; after loop, interpolate between
   previous and current using sub-tick fraction `(g_simTimeAccumulator & 0xFFFF) / 0x10000`.
   This would give perfectly smooth camera at any FPS.

2. **Tick-count scaling**: Multiply damping constants by tick count to maintain consistent
   feel across frame rates. More complex but preserves original camera behavior exactly.
