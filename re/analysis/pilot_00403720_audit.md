# Pilot Audit — 0x00403720 RefreshVehicleWheelContactFrames

**Date:** 2026-05-13
**Pool slot:** TD5_pool1 (pool0 had stuck channel lock)
**Port-side function:** `td5_physics_refresh_wheel_contacts` @ `td5_physics.c:5264`
**Worktree:** `.claude/worktrees/precise-00403720` on branch `precise-00403720`
**Caller graph:** `IntegrateVehiclePoseAndContacts (0x405E80)` and `UpdateVehiclePoseFromPhysicsState (0x4063A0)` — called TWICE per tick.
**Callee graph:** TransformShortVec3ByRenderMatrixRounded (0x42EB10), LoadRenderRotationMatrix (0x43DA80), LoadRenderTranslation (0x43DC20), UpdateActorTrackPosition (0x4440F0), ComputeActorTrackContactNormal (0x445450), ComputeActorTrackContactNormalExtended (0x4457E0).
**Body:** 766 bytes / 227 instructions / 95 decompiled lines.

## Function structure (from listing + decomp)

```
prologue:
    sVar3 = (cardef[0x82] * 0xB5 + sign) >> 8         // wheel preload
    LoadRenderRotationMatrix(actor + 0x120)
    LoadRenderTranslation(actor + 0x120)
    actor->damage_lockout = actor->wheel_contact_bitmask  // OLD slot snapshot

LOOP 1 (4 iterations, wheel probes at actor+0x40..0x7F):
    save old wheel_contact_pos[i] (x,y,z) from actor+0xF0+i*0xC
    write track_span_raw + sub_lane_index to wheel probe (actor+0x40+i*0x10)
    body_y_subtract_preload = cardef[wheel_offset + 0x42] - (susp_pos >> 8) - preload
    write body_y_subtract_preload to actor+0x212+i*8
    TransformShortVec3ByRenderMatrixRounded(actor+0x210+i*8, actor+0xF0+i*0xC)  // PASS 1
    body_y_full = body_y_subtract_preload + preload
    write body_y_full to actor+0x212+i*8
    TransformShortVec3ByRenderMatrixRounded(actor+0x210+i*8, actor+0x298+i*0xC)  // PASS 2 → hires
    SHL wheel_contact_pos[i].x/y/z by 8 (→ 24.8 FP)
    UpdateActorTrackPosition(probe, &wheel_contact_pos[i])
    ComputeActorTrackContactNormalExtended(probe, &wheel_contact_pos[i], &wcv[i], &ground_y)
    force = (wheel_contact_pos[i].y - ground_y) + gGravityConstant
    write gap_270[i] = (new_wheel_pos - old_wheel_pos) >> 8     // NO CLAMP
    if |force| < 0x200: force = 0
    if force > 0x800: airborne bit set, force = 12000
    else            : wheel_contact_pos[i].y = ground_y         // snap
    actor->wheel_load_accum[i] = force
end LOOP 1

actor->wheel_contact_bitmask = computed mask    // NEW slot

LOOP 2 (4 iterations, body probes at actor+0x00..0x3F):
    write track_span_raw + sub_lane_index to body probe (actor+0x00+i*0x10)
    TransformShortVec3ByRenderMatrixRounded(cardef+i*8, actor+0x90+i*0xC)  // ONE transform
    SHL probe_FL+i*0xC .x/y/z by 8
    UpdateActorTrackPosition(body_probe, actor+0x90+i*0xC)
    ComputeActorTrackContactNormal(body_probe, actor+0x90+i*0xC, actor+0x90+i*0xC + 4)
end LOOP 2
```

## Confirmed divergence points (port vs original)

### D1 — MISSING SECOND LOOP **(HIGH IMPACT)**
Original has a second 4-iteration loop that:
- Transforms `car_def[0..15]` shorts (4 vec3 body-probe offsets, stride 8 bytes) to world coords
- Stores world positions at `actor+0x90..0xB7` (probe_FL/FR/RL/RR)
- Calls `UpdateActorTrackPosition` and `ComputeActorTrackContactNormal` (the 3-arg version, NOT Extended) for each body probe

The port has NO such loop. Instead, at `td5_physics.c:5589-5594`, the per-wheel loop tail aliases:
```c
case 0: actor->probe_FL = actor->wheel_contact_pos[0]; break;
case 1: actor->probe_FR = actor->wheel_contact_pos[1]; break;
case 2: actor->probe_RL = actor->wheel_contact_pos[2]; break;
case 3: actor->probe_RR = actor->wheel_contact_pos[3]; break;
```

This means the port's probe_FL/FR/RL/RR point at WHEEL positions (preload-subtracted + susp-applied), not BODY probe positions (raw car_def offsets). Used in wall-contact detection (`UpdateActorTrackSegmentContacts`), so wall collisions trigger at wheel extents instead of body extents.

**Likely symptom:** wall collisions feel "tight" — the car can clip body panels into a wall before the wheels register, because wall detection is per-wheel only.

### D2 — gap_270 CLAMP at ±20000 fp8 **(MEDIUM IMPACT)**
`td5_physics.c:5496-5498`:
```c
#define CLAMP_DELTA(v) ((v) > 20000 ? 0 : (v) < -20000 ? 0 : (v))
dx = CLAMP_DELTA(dx); dy = CLAMP_DELTA(dy); dz = CLAMP_DELTA(dz);
```

Original has **no** clamp. Decomp: `*local_28 = (short)((*piVar1 - local_c) + (...>>0x1f & 0xff) >> 8);` — plain delta with sign-extending round.

Justification in port comment: "indicates teleport". But the original handles teleports differently (or absorbs the large delta downstream). Per `feedback-precise-port-over-approximation`, this is exactly the kind of approximation to remove.

**Likely symptom:** at legitimate large wheel-motion ticks (wall collision rebound, spawn settle), the port zeros gap_270 instead of using the real delta → suspension excitation differs → recovery dynamics diverge.

### D3 — `s_prev_wheel_*` static storage **(LOW IMPACT, needs verify)**
Port stores previous-tick wheel positions in static arrays `s_prev_wheel_tx/ty/tz[slot]`, writing them at the END of the function. Original captures previous values at function ENTRY by reading the wheel_contact_pos field BEFORE overwrite (decomp local_c/8/4).

**Functionally equivalent if and only if** the function is called the same number of times per tick by both port and original. Both callers (0x405E80 + 0x4063A0) appear to call it once each → 2 calls per tick. The original's "old" on the second call is the FIRST call's output; the port's `s_prev_wheel` on the second call is also the first call's output. Should match — but verify with the Frida trace.

### D4 — inline normal computation **(MEDIUM IMPACT)**
Original calls `ComputeActorTrackContactNormalExtended @ 0x4457E0` per wheel (4-arg, writes wcv[i] + ground_y).
Port reimplements as `td5_track_compute_contact_height_with_normal` + manual normalize-to-4096 using `td5_isqrt`. The integer sqrt must produce the same magnitude as 0x4457E0's normalize, which calls 0x0042CD40 (vector-normalize-to-4096).

**Verify:** Frida-capture `ComputeActorTrackContactNormalExtended` output vs port's `td5_track_compute_contact_height_with_normal` output for the same input span+sub_lane+world_xz.

### D5 — probe_span fallback clamping **(LOW IMPACT)**
Port clamps `probe_span` against `td5_track_get_span_count` with two-level fallback (track_span_raw → 0). Original passes the probe directly to `UpdateActorTrackPosition` which handles bounds itself. Probably equivalent at runtime but adds branches.

### D6 — wheel_contact_velocities normalize divisor **(needs verify)**
Original 0x0042CD40 may use a different normalize routine (sqrt path) than `td5_isqrt`. Bit-exact normalization is unlikely without matching the original's integer sqrt approximation algorithm.

### D7 — per-60-frame log branches **(LOW IMPACT)**
Lines 5502, 5546: conditional log calls. Adds branches not in original. Negligible for output diff but affects code layout.

## Capture schema for pilot

Per call (4 rows per slot per tick, one per wheel):

**Keys:** `sim_tick`, `slot`, `caller` ("integrate_pose" vs "pose_from_physics"), `wheel_i`
**Inputs:**
- `actor_addr` (hex, for sanity)
- `track_span_raw`, `track_sub_lane`
- `cardef_0x82` (preload base)
- `render_pos_x`, `render_pos_y`, `render_pos_z` (after LoadRenderTranslation runs)
- `rot[0..8]` (after LoadRenderRotationMatrix runs)
- `wda_x`, `wda_y`, `wda_z` (wheel_display_angles[i][0..2])
- `susp_pos` (wheel_suspension_pos[i])
- `old_wcb`, `old_dam_lockout`
- `old_wcp_x`, `old_wcp_y`, `old_wcp_z` (at entry, before overwrite)
- `cardef_wheel_y` (cardef[wheel_offset + 0x42])

**Outputs:**
- `new_wcp_x`, `new_wcp_y`, `new_wcp_z` (post-snap)
- `hires_x`, `hires_y`, `hires_z`
- `wcv_x`, `wcv_y`, `wcv_z` (surface normal)
- `gap270_x`, `gap270_y`, `gap270_z`
- `ground_y`
- `force` (load_accum[i])
- `new_wcb_bit` (just this wheel's bit)
- `probe_FL/FR/RL/RR_x/y/z` (post-LOOP2 — captures D1 divergence)

Schema is ~30 columns; manageable with a single CSV.

## Next actions for the pilot

1. **Generate** `tools/frida_pool1_00403720.js` to capture entry+exit per-wheel.
2. **Add** port-side trace emit (`td5_trace_emit_func` with addr=0x00403720) at the per-wheel loop boundaries.
3. **Build** the worktree.
4. **Run** one Edinburgh race (track=1, slot 0, span 0) capturing both traces.
5. **Diff** column-by-column.
6. **Fix** highest-impact divergence first (D1: implement the second loop).
7. **Re-run** until zero diff.
