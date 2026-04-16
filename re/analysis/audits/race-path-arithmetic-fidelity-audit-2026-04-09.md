# Race-Path Arithmetic Fidelity Audit — 2026-04-09

Scope: arithmetic choices in the current port that are most likely to drift from
the original executable and show up as race bugs.

## Priority Risks

| Priority | File | Spot | Risk | Why It Matters |
|---|---|---|---|---|
| High | [`td5_physics.c`](C:/Users/maria/Desktop/Proyectos/TD5RE/td5mod/src/td5re/td5_physics.c) | `cos_fixed12`, `sin_fixed12`, `atan2_fixed12` (`155`-`183`) | Uses host `cos`, `sin`, `atan2` instead of the original lookup/quantized path. | Trig quantization feeds wall response, steering, heading conversion, suspension gravity projection, collision normals, and matrix rebuilds. Small differences accumulate quickly. |
| High | [`td5_track.c`](C:/Users/maria/Desktop/Proyectos/TD5RE/td5mod/src/td5re/td5_track.c) | `angle_from_vector_full()` (`2000`-`2021`) | Replaces the original angle path with a direct approximation. | Track heading affects AI alignment, camera direction, actor initialization, and route thresholds. |
| High | [`td5_physics.c`](C:/Users/maria/Desktop/Proyectos/TD5RE/td5mod/src/td5re/td5_physics.c) | suspension substitute path (`1882`-`1937`, `1968`-`1998`) | The arithmetic is stable, but it is not the original arithmetic. | This is the main race-physics fidelity gap. It changes the math model, not just the numeric precision. |
| Medium | [`td5_camera.c`](C:/Users/maria/Desktop/Proyectos/TD5RE/td5mod/src/td5re/td5_camera.c) | dynamic distance quantization (`1353`-`1368`) | Uses `(int)dist_f` after `sqrtf`, marked as simplified `__ftol`. | Trackside camera thresholds and FOV transitions are sensitive to integer truncation and x87 rounding behavior. |
| Medium | [`td5_ai.c`](C:/Users/maria/Desktop/Proyectos/TD5RE/td5mod/src/td5re/td5_ai.c) | steering-rate / max-steer denominators (`740`-`820`) | Integer division order is preserved in shape, but exact saturation behavior has not been validated against the binary. | Small denominator drift here changes AI steering aggression and oscillation damping. |
| Medium | [`td5_physics.c`](C:/Users/maria/Desktop/Proyectos/TD5RE/td5mod/src/td5re/td5_physics.c) | traffic damping rounding (`1138`-`1141`) | Signed bias is explicitly injected before the shift. | This is a good candidate for per-tick sign or rounding drift if the original used a slightly different bias path. |
| Medium | [`td5_physics.c`](C:/Users/maria/Desktop/Proyectos/TD5RE/td5mod/src/td5re/td5_physics.c) | velocity safety clamp (`868`-`883`, `1103`-`1113`) | This is intentionally non-original safety logic. | It keeps the car controllable, but it can hide upstream force divergence by clipping the symptom instead of fixing the cause. |
| Low | [`td5_game.c`](C:/Users/maria/Desktop/Proyectos/TD5RE/td5mod/src/td5re/td5_game.c) | rounded vec3 helper (`2392` onward) | The source already documents original `__ftol` behavior, but the surrounding uses have not all been audited. | This is less likely to drive race bugs than the items above, but it matters for exact projection / camera parity. |

## What To Preserve Aggressively

1. Integer division order.
   `((a << n) / b) * c` is not interchangeable with `(a * c << n) / b`.

2. Signed right shifts.
   Many vehicle formulas rely on arithmetic right shift, not logical shift or float conversion.

3. Lookup-table quantization.
   Replacing a 12-bit or LUT-based angle helper with `sin`, `cos`, or `atan2` often looks harmless and then produces visible drift.

4. Saturation points.
   Hard clamps such as `0x578`, `0x18000`, `0x800`, `4000`, and `6000` are part of the behavior, not cleanup targets.

5. Fixed-point boundary conversions.
   The riskiest places are `24.8 -> integer`, `12-bit angle -> trig`, and per-wheel contact / suspension projections.

## Recommended Next Arithmetic Work

1. Restore or emulate the original trig / angle LUT behavior in one place and use it consistently.
2. Differential-check `angle_from_vector_full()` against the binary for a small corpus of vectors.
3. Treat the suspension math as a controlled approximation until original XZ projection logic is recovered; do not “improve” it casually.
4. Mark all non-original safety math as temporary in follow-up RE docs and tests.
