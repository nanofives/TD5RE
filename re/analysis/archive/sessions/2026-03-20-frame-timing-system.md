# Frame Timing System Analysis (2026-03-20)

## Overview

Test Drive 5 uses a **fixed-timestep accumulator** pattern targeting **30 fps** (33.33ms per tick). The physics simulation advances in discrete 1/30s steps, while rendering runs at whatever rate the display allows. A fractional tick interpolant is extracted for smooth camera/render between ticks.

---

## Complete Timing Flow

```
timeGetTime()
     |
     v
frameDeltaMs = timeGetTime() - g_framePrevTimestamp
     |
     v
g_normalizedFrameDt = frameDeltaMs * 0.001 * 30.0
     |                    (seconds)   (ticks/sec)
     v
g_simTickBudget += g_normalizedFrameDt    (replenish)
g_simTickBudget = min(g_simTickBudget, 4.0)  (clamp)
     |
     v
g_simTimeAccumulator = (int)(g_simTickBudget * 65536.0)   (float -> 16.16 FP)
     |
     v
while (g_simTimeAccumulator >= 0x10000):
    RunOneSimTick()       // physics, AI, input, particles, etc.
    g_simTimeAccumulator -= 0x10000
    g_simTickBudget -= 1.0
     |
     v
g_subTickFraction = (g_simTimeAccumulator & 0xFFFF) / 65536.0
     |
     v
Render scene (camera interpolated by g_subTickFraction)
     |
     v
g_framePrevTimestamp = g_frameEndTimestamp
```

---

## Constants (.rdata)

| Address    | Type   | Value        | Name/Purpose |
|------------|--------|--------------|--------------|
| 0x45d5f4   | float  | 1.0          | Per-tick budget decrement |
| 0x45d650   | float  | 4.0          | g_simTickBudget max clamp (spiral-of-death protection) |
| 0x45d658   | double | 0.001        | Milliseconds to seconds conversion |
| 0x45d660   | double | 30.0         | Target simulation rate (ticks/second) |
| 0x45d668   | double | 1000.0       | Milliseconds per second (for FPS calc) |
| 0x45d670   | double | 1.0          | Numerator for FPS reciprocal (1.0 / frameDeltaMs) |
| 0x45d680   | float  | 1/255        | Fade alpha normalization (0.003921569) |
| 0x45d684   | float  | 255.0        | Max fade alpha |
| 0x45d688   | float  | 110000.0     | Fade accumulator scale (used with g_simTickBudget for race-end fade) |
| 0x45d68c   | float  | 1/65536      | 16.16 fixed-point to float (1.5258789e-05) |
| 0x45d690   | float  | 65536.0      | Float budget to 16.16 fixed-point scale |

---

## Global Variables (.data / .bss)

| Address    | Type   | Name                      | Description |
|------------|--------|---------------------------|-------------|
| 0x466E88   | float  | g_simTickBudget           | Fractional sim tick budget. Init=1.0. Accumulates g_normalizedFrameDt each frame; decremented by 1.0 per consumed tick; clamped to [0, 4.0]. |
| 0x466E90   | float  | g_instantFPS              | Instantaneous FPS = 1000.0 / frameDeltaMs |
| 0x4AAD70   | float  | g_normalizedFrameDt       | Frame delta normalized to 30fps: frameDeltaMs * 0.001 * 30.0. Equals 1.0 at exactly 30fps. |
| 0x4AAD98   | double | g_fpsReciprocal           | 1.0 / (float)frameDeltaMs (intermediate for FPS) |
| 0x4AADAC   | float  | g_smoothedFrameDt         | Copy of g_normalizedFrameDt unless DAT_00497324!=0 (network mode, where it may be forced) |
| 0x4AADA0   | int    | g_simulationTickCounter   | Total sim ticks since race start. Incremented unless camera transition gate active. Capped at 0x7FFFFFFF. |
| 0x4AAC5C   | int    | g_frameCounter            | Secondary tick counter (incremented every tick unconditionally) |
| 0x4AAED0   | uint32 | g_simTimeAccumulator      | Accumulator in 16.16 fixed-point units. One tick = 0x10000. |
| 0x4AAED4   | float  | g_simTickBudgetSnapshot   | Snapshot of g_simTickBudget at frame start (used by particle/streak rendering) |
| 0x4AAED8   | DWORD  | g_frameEndTimestamp       | timeGetTime() sampled at end-of-frame (after all rendering) |
| 0x4AAF40   | int    | g_benchmarkFirstFrameSkip | Benchmark mode: skip first frame for recording. Set 0 at init, 1 on first frame. |
| 0x4AAF44   | DWORD  | g_framePrevTimestamp      | timeGetTime() of previous frame end. Init'd in InitializeRaceSession. |
| 0x4AAF60   | float  | g_subTickFraction         | (g_simTimeAccumulator & 0xFFFF) / 65536.0 -- fractional position between sim ticks (0.0-1.0). Used by camera/render interpolation. |

---

## Detailed Disassembly Trace

### 1. Accumulator Computation (top of RunRaceFrame, 0x42b5d1)

```asm
0042b5d1: FLD  [0x466E88]      ; g_simTickBudget (float)
0042b5d7: MOV  [0x4AADEC], EDI ; clear flag
0042b5dd: FMUL [0x45d690]      ; * 65536.0
0042b5e3: CALL __ftol           ; float -> int
0042b5e8: MOV  [0x4AAED0], EAX ; g_simTimeAccumulator
```

### 2. Sub-tick Fraction Extraction (0x42b711)

```asm
0042b711: MOV  ECX, [0x4AAED0] ; g_simTimeAccumulator
0042b717: AND  ECX, 0xFFFF     ; mask low 16 bits (fractional part)
0042b71d: MOV  [ESP+0x18], ECX
0042b721: MOV  [ESP+0x1c], EDI ; zero high dword (for FILD qword)
0042b725: FILD qword [ESP+0x18]; load as 64-bit int -> float
0042b729: FMUL [0x45d68c]      ; * (1/65536) = 1.5258789e-05
0042b72f: FSTP [0x4AAF60]      ; g_subTickFraction
```

### 3. Tick Period Literal (0x42b8fa)

```asm
0042b8fa: MOV  EBX, 0x10000    ; IMMEDIATE literal, not a global
; Used in the sim loop:
0042b8ff: CMP  EAX, EBX        ; while (accumulator >= 0x10000)
0042ba82: SUB  ECX, EBX        ; accumulator -= 0x10000
0042baea: CMP  [0x4AAED0], EBX ; loop-back test
```

### 4. Per-Tick Budget Decrement (inside sim loop, 0x42ba6b)

```asm
0042ba6b: FLD  [0x466E88]      ; g_simTickBudget
0042ba77: FSUB [0x45d5f4]      ; - 1.0
0042ba82: SUB  ECX, EBX        ; g_simTimeAccumulator -= 0x10000
0042ba86: FSTP [0x466E88]      ; store back
0042ba8c: MOV  [0x4AAED0], ECX
```

### 5. Frame Delta & FPS Computation (end of frame, 0x42c12d)

```asm
0042c12d: CALL [0x45d5c0]      ; timeGetTime() via IAT
0042c133: MOV  ECX, [0x4AAF44] ; g_framePrevTimestamp
0042c139: MOV  [0x4AAED8], EAX ; g_frameEndTimestamp = now
0042c13e: SUB  EAX, ECX        ; frameDeltaMs
0042c140: MOV  [ESP+0x18], EAX
0042c144: FILD [ESP+0x18]      ; (float)frameDeltaMs
0042c148: FLD  [0x45d670]      ; 1.0 (double)
0042c155: FDIV ST0, ST1        ; 1.0 / frameDelta
0042c157: FST  [0x4AAD98]      ; g_fpsReciprocal (double)
0042c15d: FMUL [0x45d668]      ; * 1000.0
0042c163: FSTP [0x466E90]      ; g_instantFPS = 1000/frameDelta
; ST0 still has (float)frameDeltaMs from the FILD
0042c169: FMUL [0x45d660]      ; * 30.0 (double promotion)
0042c16f: FMUL [0x45d658]      ; * 0.001
0042c175: FSTP [0x4AAD70]      ; g_normalizedFrameDt
```

Note: the FPU stack manipulation means the `(float)frameDeltaMs` value loaded at 0x42c144 remains on ST1 after the FDIV at 0x42c155, and after the FSTP at 0x42c163 pops the FPS result, ST0 has `frameDeltaMs` again. The FMUL chain at 0x42c169/0x42c16f then computes `frameDeltaMs * 30.0 * 0.001`.

### 6. Budget Replenishment & Clamp (0x42c1e6)

```asm
0042c1e6: FLD  [0x4AADAC]      ; g_smoothedFrameDt
0042c1ec: FADD [0x466E88]      ; + g_simTickBudget
0042c1f2: FST  [0x466E88]      ; g_simTickBudget += dt
0042c1f8: FCOMP [0x45d650]     ; compare with 4.0
0042c200: TEST AH, 0x41        ; if budget > 4.0
0042c205: MOV  [0x466E88], 0x40800000  ; g_simTickBudget = 4.0
```

### 7. Benchmark Override (0x42c235)

```asm
0042c235: MOV [0x466E88], 0x40400000  ; g_simTickBudget = 3.0 (float)
```

In benchmark mode, the budget is forced to 3.0 every frame, meaning exactly 3 sim ticks per frame regardless of actual wall time.

### 8. Timestamp Update (0x42c246)

```asm
0042c23f: MOV ECX, [0x4AAED8]  ; g_frameEndTimestamp
0042c246: MOV [0x4AAF44], ECX  ; g_framePrevTimestamp = g_frameEndTimestamp
```

---

## Frame Rate Behavior

### No Explicit Frame Cap

There is **no Sleep(), WaitForSingleObject(), or busy-wait** anywhere in the race loop or its callers. The game loop in `GameWinMain` (0x430a90) uses `PeekMessageA` (non-blocking) and immediately calls `RunMainGameLoop()` on every pump cycle.

### VSync in Fullscreen

In fullscreen mode, `DXDraw::Flip` (M2DX.dll 0x10006fe0) calls:
```
IDirectDrawSurface::Flip(NULL, DDFLIP_WAIT)  ; flags = 0x1
```
Since `DDFLIP_NOVSYNC` (0x8) is NOT set, this waits for vertical blank, effectively capping framerate to the monitor refresh rate in true fullscreen.

### VSync Timing Interaction

Critical detail: the `timeGetTime()` call (0x42c12d) and `g_framePrevTimestamp` update (0x42c246) both happen **BEFORE** `EndRaceScene()` (0x42c254), which is where VSync blocks inside `DXDraw::Flip`. This means:

```
Frame N:
  [sim + render]
  T_N = timeGetTime()        <- captured BEFORE VSync
  g_framePrevTimestamp = T_N
  EndRaceScene -> Flip -> VSync WAIT (blocks here)
  [audio, return]

Frame N+1:
  [sim + render]
  T_{N+1} = timeGetTime()    <- captured AFTER Frame N's VSync completed
  frameDelta = T_{N+1} - T_N  <- INCLUDES Frame N's VSync wait
```

So `frameDelta` measures wall time from post-render to post-render, **including** the VSync wait from the previous frame. At 60Hz VSync, `frameDelta` ~= 16.67ms, `g_normalizedFrameDt` ~= 0.5. The timing is self-consistent -- VSync artificially inflates the measured delta, which in turn throttles the tick budget replenishment, preventing the sim from running faster than real time.

### Windowed Mode (incl. dgVoodoo2)

In windowed mode, Flip uses `IDirectDrawSurface::Blt` with `DDBLT_WAIT` (0x1000000) -- no VSync, frames run uncapped. Under dgVoodoo2 (which wraps DirectDraw), the VSync behavior depends on dgVoodoo2's configuration.

### Faster Than 30fps

At 60fps, each frame adds ~0.5 to the tick budget. The sim loop fires every other frame (alternating 0 and 1 tick). The g_subTickFraction provides smooth interpolation between discrete sim states for camera positioning.

### Slower Than 30fps

At 15fps, each frame adds ~2.0 to the budget, so the sim loop runs 2 ticks to catch up. At 7.5fps, it runs 4 ticks (the clamp limit). Below 7.5fps, the 4.0 clamp kicks in and the simulation genuinely slows down relative to wall time (spiral-of-death protection).

---

## RunMainGameLoop Case 2 (0x442170)

Case 2 has **no timing code** around RunRaceFrame:
```c
case 2:
    iVar2 = RunRaceFrame();
    *(app_exref + 0x16c) = 0;  // clear frontend-active flag
    if (iVar2 != 0) {
        // race ended: restore display mode, transition state
        DAT_004c3ce8 = (benchmark ? 3 : 1);
        DXPlay::UnSync();
    }
    break;
```

---

## Network Mode (DAT_00497324)

When `DAT_00497324 != 0` (network/LAN play), the `g_normalizedFrameDt -> g_smoothedFrameDt` copy at 0x42c17d is skipped. This allows the network subsystem to inject its own frame timing into `g_smoothedFrameDt` (0x4AADAC) to keep simulation synchronized across peers.

---

## Audio Options Overlay

When `g_audioOptionsOverlayActive` (0x4AAF8C) is nonzero:
- The sub-tick fraction is NOT computed (g_subTickFraction is stale)
- The entire sim tick loop is SKIPPED (no physics advances)
- The budget replenishment still happens via the overlay path
- This effectively pauses the simulation while the audio options menu is open

---

## Initialization (InitializeRaceSession, 0x42AF80)

```
g_simulationTickCounter = 0      (0x4AADA0)
g_frameCounter = 0               (0x4AAC5C)
g_simTickBudget = 1.0            (0x466E88, written as 0x3f800000)
g_normalizedFrameDt = 0          (0x4AAD70)
g_framePrevTimestamp = timeGetTime()  (0x4AAF44)
g_benchmarkFirstFrameSkip = 0    (0x4AAF40)
```

Starting the budget at 1.0 means the first frame will immediately run one sim tick, regardless of frame delta.
