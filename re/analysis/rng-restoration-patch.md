# RNG Restoration Patch -- FUN_0042C8D0

## Overview

The function at `0x0042C8D0` is a stubbed random number generator that always
returns 0 (`XOR EAX,EAX; RET`). This patch restores it by redirecting to the
existing CRT `_rand` function already present in the binary.

---

## The Problem

The 3-byte stub `33 C0 C3` causes two categories of visual/gameplay degradation:

### Collision Tumble (FUN_004079C0 -- 12 call sites)

When collision energy exceeds 90,000, the code computes:
```c
perturbation = rand() % param_4 - param_4 / 2;
```
With `rand()` returning 0, this always yields `-param_4 / 2`, producing:
- **Deterministic post-collision rotation** -- cars always spin the same way
- **No random tumble variation** -- every crash at the same energy looks identical
- The code applies this to rotation axes at offsets +0x1C0, +0x1C4, +0x1C8

### Vertex Color Shimmer (FUN_0043D830, FUN_0043D910, FUN_0043D9F0 -- 10 call sites)

Three vertex color perturbation functions apply:
```c
// FUN_0043D830 / FUN_0043D910: amplitude 7, offset 4
delta = (rand() & 7) - 4;  // expected range: -4..+3

// FUN_0043D9F0: amplitude 15, offset 8
delta = (rand() & 15) - 8; // expected range: -8..+7
```
These deltas are scaled by a speed factor and added to vertex color channels at
offsets +0x2EC, +0x2F0, +0x2F4, +0x2F8 of the car model structure.

With `rand()` returning 0:
- `(0 & 7) - 4 = -4` -- constant negative bias every frame
- `(0 & 15) - 8 = -8` -- constant negative bias every frame
- Vertex colors drift monotonically darker instead of shimmering randomly
- The intended "paint sparkle" / metallic glint effect is completely absent

---

## Patch Approach: JMP to CRT _rand (Option A)

### Why This Approach

Three options were evaluated:

| Option | Description | Pros | Cons |
|--------|-------------|------|------|
| **A: JMP _rand** | Redirect stub to existing CRT rand | 5-byte patch, uses proven code, thread-safe (TLS seed) | Slight overhead from TLS access |
| B: Inline LCG | Write a minimal LCG in the 16-byte space | Self-contained, no dependencies | Needs seed storage, not thread-safe, tight fit |
| C: Code cave LCG | Write LCG in .text padding at 0x45CF00 | More space for implementation | Adds complexity, still needs seed address |

**Option A was chosen** because:
1. `_rand` at `0x00448157` already exists and is well-tested MSVC CRT code
2. The 5-byte JMP fits comfortably in the 16 bytes available (3 bytes stub + 13 bytes NOP padding before the next function at 0x42C8E0)
3. Thread-safe via per-thread seed in TLS (handles the unlikely case of multi-threaded calls)
4. Returns 0..0x7FFF -- sufficient for all callers (collision uses `%`, vertex uses `& 7`/`& 15`)
5. Minimal patch surface = minimal risk

### Calling Convention Compatibility

| Property | Stub (0x42C8D0) | _rand (0x448157) |
|----------|-----------------|-------------------|
| Convention | __stdcall | __cdecl |
| Parameters | 0 | 0 |
| Return | int in EAX | int in EAX |
| Stack cleanup | N/A (0 params) | N/A (0 params) |

With zero parameters, `__stdcall` and `__cdecl` are equivalent -- no stack
cleanup mismatch is possible. The JMP causes `_rand`'s own `RET` to return
directly to the original caller. No trampoline needed.

### CRT _rand Implementation (for reference)

Decompiled from `0x00448157`:
```c
int __cdecl _rand(void) {
    DWORD *tls = _get_thread_data();   // FUN_00449a37
    uint seed = tls[5] * 0x343FD + 0x269EC3;
    tls[5] = seed;
    return (seed >> 16) & 0x7FFF;
}
```
This is the standard MSVC LCG: `seed = seed * 214013 + 2531011`, returning
bits 16..30 (range 0..32767).

---

## Exact Patch Bytes

| File Offset | VA | Original | Patched | Disassembly |
|-------------|-----|----------|---------|-------------|
| `0x02C8D0` | `0x0042C8D0` | `33 C0 C3 90 90` | `E9 82 B8 01 00` | `JMP 0x00448157` |

### Calculation

```
source_va   = 0x0042C8D0
instruction = E9 rel32       (5 bytes)
next_ip     = 0x0042C8D5     (source + 5)
target_va   = 0x00448157     (_rand entry)
rel32       = 0x00448157 - 0x0042C8D5 = 0x0001B882
encoded     = E9 82 B8 01 00 (little-endian)
```

### Memory Layout at Patch Site

```
0042C8D0: E9 82 B8 01 00   JMP _rand       <-- patched (was: 33 C0 C3)
0042C8D5: 90 90 90 90 90   NOP padding     <-- untouched
0042C8DA: 90 90 90 90 90   NOP padding     <-- untouched
0042C8DF: 90               NOP padding     <-- untouched
0042C8E0: 53               PUSH EBX        <-- next function (FUN_0042C8E0)
```

---

## Expected Visual Differences

### Collision Response

**Before (stub returns 0):**
- Crash at energy 100,000 always applies `-param_4/2` to all rotation axes
- Every collision at the same speed/angle produces identical post-crash spin
- Cars slide rather than tumble; collisions feel "stiff"

**After (rand returns 0..0x7FFF):**
- Each rotation axis gets `rand() % param_4 - param_4/2`, a value randomly
  distributed across the full perturbation range
- Same-speed collisions produce different tumble directions each time
- Cars spin, flip, and tumble unpredictably after high-energy impacts

### Vertex Color Shimmer

**Before (stub returns 0):**
- `(0 & 7) - 4 = -4` applied every frame to R/G/B/A vertex channels
- Colors drift steadily darker (monotonic negative bias)
- Car surfaces appear flat and lifeless

**After (rand returns 0..0x7FFF):**
- `(rand & 7) - 4` yields values in {-4, -3, -2, -1, 0, 1, 2, 3}
- Vertex colors jitter randomly around their base value each frame
- Car paint shows subtle metallic sparkle/shimmer, especially at speed
- The three perturbation functions (0x43D830, 0x43D910, 0x43D9F0) apply
  different scale factors based on car speed, so shimmer intensity increases
  with velocity as originally intended

---

## Risk Assessment

| Factor | Rating | Notes |
|--------|--------|-------|
| Patch correctness | **Very Low risk** | 5-byte JMP, trivially verifiable |
| Calling convention | **No risk** | Zero-parameter functions are convention-agnostic |
| Return value range | **Very Low risk** | 0..0x7FFF, callers use `%` and `&` to constrain |
| Thread safety | **No risk** | _rand uses per-thread TLS seed |
| Performance | **Negligible** | _rand is ~30 cycles (TLS lookup + multiply + shift) |
| Visual impact | **Low risk** | Collision tumble becomes dynamic; vertex shimmer is subtle |
| Reversibility | **Trivial** | `python patch_restore_rng.py --revert` restores original bytes |

### Worst Case Scenarios

1. **Collision tumble too dramatic:** The perturbation range is capped by `param_4`
   (derived from collision energy). The original developers chose these ranges, so
   this is restoring intended behavior. If it looks excessive, the collision energy
   threshold (90,000) and the divisors in FUN_004079C0 can be tuned separately.

2. **Vertex shimmer too visible:** The `& 7` and `& 15` masks strictly limit the
   amplitude. At the maximum, vertex colors change by +/-4 or +/-8 out of 256 per
   channel per frame -- a ~1.5% to ~3% fluctuation. This is barely perceptible and
   was the original design intent.

3. **Deterministic replay divergence:** If the game has any replay or network
   synchronization that assumes deterministic `rand()` sequencing, this could cause
   desync. However, the callers are purely cosmetic (visual perturbation), so
   this is not a concern for gameplay state.

---

## Usage

```bash
# Apply the patch
python patch_restore_rng.py path/to/TD5_d3d.exe

# Verify current state
python patch_restore_rng.py --verify path/to/TD5_d3d.exe

# Revert to original stub
python patch_restore_rng.py --revert path/to/TD5_d3d.exe
```
